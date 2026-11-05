#include "networkservice.h"

#include "appinfo.h"
#include "discovery.h"
#include "history.h"
#include "httpclient.h"
#include "httpserver.h"
#include "logger.h"
#include "sendworker.h"
#include "settings.h"
#include "theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QNetworkInterface>
#include <QMutex>
#include <QMutexLocker>
#include <QRunnable>
#include <QSet>
#include <QSslSocket>
#include <QTcpSocket>
#include <QThreadPool>
#include <QThread>
#include <QTimer>
#include <QUuid>

namespace {

// Sockets opened on the dual stack wildcard report IPv4 peers as
// "::ffff:a.b.c.d"; the rest of the app only needs the dotted form.
QString plainAddress(const QString &address)
{
    if (address.startsWith(QLatin1String("::ffff:"), Qt::CaseInsensitive))
        return address.mid(7);
    return address;
}

// Posts /v2/cancel to a peer from a scratch thread: the receiving side must
// tell the sender to stop, and doing that on the GUI thread would freeze the
// window for the duration of a TLS handshake.
class CancelNotifier : public QThread
{
public:
    CancelNotifier(const crypto::Identity &identity, const QString &address, quint16 port,
                   bool tls, const QString &sessionId)
        : m_identity(identity)
        , m_address(address)
        , m_port(port)
        , m_tls(tls)
        , m_sessionId(sessionId)
    {
    }

protected:
    void run() override
    {
        const QString path = QStringLiteral("/api/localsend/v2/cancel?sessionId=") + m_sessionId;
        const httpclient::Response response = httpclient::request(
                m_address, m_port, m_tls, QStringLiteral("POST"), path, QByteArray(),
                QByteArray("application/json"), &m_identity, true, 6000);
        logger::write(QStringLiteral("receive: cancel notify %1:%2 status=%3 %4")
                          .arg(m_address)
                          .arg(m_port)
                          .arg(response.status)
                          .arg(response.error));
    }

private:
    crypto::Identity m_identity;
    QString m_address;
    quint16 m_port;
    bool m_tls;
    QString m_sessionId;
};

} // namespace

NetworkService::NetworkService(Settings *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_server(0)
    , m_discovery(0)
    , m_announceTimer(0)
    , m_pruneTimer(0)
    , m_scanTimer(0)
    , m_scanThread(0)
    , m_verifyThread(0)
    , m_manualPort(0)
    , m_listening(false)
    , m_port(protocol::kDefaultPort)
    , m_portOverride(0)
    , m_sending(false)
    , m_cancelSend(false)
    , m_sendThread(0)
    , m_sendWorker(0)
    , m_sendFileCount(0)
    , m_sendBytes(0)
    , m_incomingFiles(0)
    , m_incomingBytes(0)
    , m_incomingDone(0)
    , m_autoAccept(false)
    , m_autoReject(false)
{
}

NetworkService::~NetworkService()
{
    stop();
}

void NetworkService::setStatus(const QString &status)
{
    m_statusLine = status;
    emit statusChanged(status);
}

void NetworkService::updateStatusLine()
{
    if (!m_listening) {
        setStatus(tr("网络未启动"));
        return;
    }

    QString line = tr("监听端口 %1 (%2)").arg(m_port)
                       .arg(m_settings->httpsEnabled() ? QStringLiteral("HTTPS") : QStringLiteral("HTTP"));
    if (!m_devices.isEmpty())
        line += QStringLiteral("，") + tr("已发现 %1 台设备").arg(m_devices.size());
    setStatus(line);
}

bool NetworkService::start(QString *error)
{
    const QString configDir = QFileInfo(Settings::configPath()).absolutePath();
    const QString certDir = configDir + QStringLiteral("/certs");

    m_identity = crypto::loadOrCreate(certDir);
    if (!m_identity.valid) {
        if (error)
            *error = tr("设备证书生成失败");
        return false;
    }

    m_port = m_portOverride > 0 ? static_cast<quint16>(m_portOverride)
                                : static_cast<quint16>(m_settings->port());
    m_self.alias = m_settings->alias();
    m_self.version = QString::fromLatin1(protocol::kVersion);
    m_self.deviceModel = QStringLiteral("Windows");
    m_self.deviceType = m_settings->deviceType();
    m_self.fingerprint = m_identity.fingerprint;
    m_self.port = m_port;
    m_self.protocol = m_settings->httpsEnabled() ? QStringLiteral("https") : QStringLiteral("http");
    m_self.download = false;
    m_self.valid = true;

    m_server = new HttpServer(m_identity, this);
    m_server->setSelf(m_self);
    m_server->setReceivePin(m_settings->receivePin());
    connect(m_server, &HttpServer::deviceRegistered, this, &NetworkService::onDeviceRegistered);
    connect(m_server, &HttpServer::prepareUploadIncoming, this, &NetworkService::onPrepareUpload);
    connect(m_server, &HttpServer::prepareUploadDropped, this, &NetworkService::onOfferDropped);
    connect(m_server, &HttpServer::uploadProgress, this, &NetworkService::incomingProgress);
    connect(m_server, &HttpServer::uploadFinished, this, &NetworkService::onIncomingFinished);
    connect(m_server, &HttpServer::sessionCancelled, this, &NetworkService::onSessionCancelled);

    // We dropped a transfer we were receiving: tell the sender, otherwise it
    // walks through the remaining files of a session nobody is listening to.
    connect(m_server, &HttpServer::incomingCancelRequested, this,
            [this](const QString &address, quint16 port, bool tls, const QString &sessionId) {
        CancelNotifier *notifier = new CancelNotifier(m_identity, address, port, tls, sessionId);
        connect(notifier, &QThread::finished, notifier, &QObject::deleteLater);
        notifier->start();
    });
    connect(m_server, &HttpServer::cancelRequested, this, &NetworkService::onCancelRequested);

    QString serverError;
    if (!m_server->start(m_port, m_settings->httpsEnabled(), &serverError)) {
        // Port 53317 is a strong convention but not a hard requirement; fall
        // back to the next free port like the reference client does.
        bool started = false;
        for (quint16 candidate = m_port + 1; candidate < m_port + 20 && !started; ++candidate) {
            started = m_server->start(candidate, m_settings->httpsEnabled(), &serverError);
            if (started)
                m_port = candidate;
        }
        if (!started) {
            if (error)
                *error = tr("无法监听端口：") + serverError;
            logger::write(QStringLiteral("server: failed to listen: ") + serverError);
            return false;
        }
        m_self.port = m_port;
        m_server->setSelf(m_self);
        logger::write(QStringLiteral("server: fell back to port %1").arg(m_port));
    }

    m_listening = true;
    m_subnetBases = localSubnetBases();
    m_subnetBase = m_subnetBases.isEmpty() ? QString() : m_subnetBases.first();
    logger::write(QStringLiteral("network: local subnets = %1").arg(m_subnetBases.join(QLatin1String(", "))));

    m_discovery = new Discovery(this);
    connect(m_discovery, &Discovery::announcementReceived, this, &NetworkService::onAnnouncement);
    QString discoveryError;
    if (!m_discovery->start(m_self, &discoveryError)) {
        logger::write(QStringLiteral("discovery: failed to start: ") + discoveryError);
        setStatus(tr("发现服务启动失败：") + discoveryError);
        return false;
    }

    updateStatusLine();
    logger::write(QStringLiteral("network: self alias=%1 fingerprint=%2 port=%3 protocol=%4")
                      .arg(m_self.alias, m_self.fingerprint)
                      .arg(m_port)
                      .arg(m_self.protocol));
    // Which TLS backend is live matters on old systems: a shipped OpenSSL that
    // fails to load turns every send into "无法建立安全连接" with no clue why.
    logger::write(QStringLiteral("tls: openssl=%1 supported=%2 window=%3")
                      .arg(QSslSocket::sslLibraryVersionString())
                      .arg(QSslSocket::supportsSsl() ? 1 : 0)
                      .arg(theme::isVistaOrLater()
                                   && !qEnvironmentVariableIsSet("LSAERO_FORCE_LEGACY_SCAN")
                           ? QStringLiteral("modern")
                           : QStringLiteral("legacy")));

    QTimer::singleShot(300, this, SLOT(announce()));

    // Keep announcing: LocalSend only announces while its send tab is open, so
    // a peer that sits on another page never learns about us from a one-shot
    // burst. Every announcement also refreshes the peers that answer us.
    m_announceTimer = new QTimer(this);
    m_announceTimer->setInterval(5000);
    connect(m_announceTimer, &QTimer::timeout, this, [this]() {
        if (m_discovery)
            m_discovery->announceOnce();
    });
    m_announceTimer->start();

    // Drop peers that stopped answering. Three missed announcements (15 s) is
    // the same patience LocalSend uses before it forgets a device.
    m_pruneTimer = new QTimer(this);
    m_pruneTimer->setInterval(5000);
    connect(m_pruneTimer, &QTimer::timeout, this, &NetworkService::pruneDevices);
    m_pruneTimer->start();

    // Multicast is not enough on networks that filter broadcasts (bridged VMs,
    // APs with client isolation, firewalls), so sweep our own /24s the way
    // LocalSend's smart scan does once the announcement goes unheard.
    // A sweep opens up to a hundred short-lived sockets. Windows XP only allows
    // ten half-open connections and has a tiny ephemeral port pool, so sweeping
    // it as often as a modern Windows burned that pool and every later TLS
    // handshake timed out. Modern systems keep the fast cadence.
    // LSAERO_FORCE_LEGACY_SCAN=1 reproduces the XP profile on a modern system
    // (see docs/P19) so the windowed sweep can be verified without an XP box.
    const bool modernWindows = theme::isVistaOrLater()
            && !qEnvironmentVariableIsSet("LSAERO_FORCE_LEGACY_SCAN");

    m_scanTimer = new QTimer(this);
    m_scanTimer->setInterval(modernWindows ? 15000 : 60000);
    connect(m_scanTimer, &QTimer::timeout, this, &NetworkService::startSubnetScan);
    m_scanTimer->start();
    QTimer::singleShot(modernWindows ? 1200 : 4000, this, &NetworkService::startSubnetScan);

    return true;
}

void NetworkService::stop()
{
    // A transfer may still be running when the window is closed. Its QThread is
    // parented to this object, and destroying a running QThread aborts the
    // process ("QThread: Destroyed while thread is still running"), so ask the
    // worker to stop and wait for it before the socket objects go away.
    if (m_sendThread) {
        if (m_sendWorker)
            m_sendWorker->requestCancel(false);
        if (!m_sendThread->wait(5000)) {
            m_sendThread->terminate();
            m_sendThread->wait(1000);
        }
        m_sendThread = 0;
        m_sendWorker = 0;
    }
    if (m_announceTimer) {
        m_announceTimer->stop();
        m_announceTimer->deleteLater();
        m_announceTimer = 0;
    }
    if (m_pruneTimer) {
        m_pruneTimer->stop();
        m_pruneTimer->deleteLater();
        m_pruneTimer = 0;
    }
    if (m_scanTimer) {
        m_scanTimer->stop();
        m_scanTimer->deleteLater();
        m_scanTimer = 0;
    }
    if (m_scanThread) {
        // Do not let a sweep land on a half torn down service.
        stopScanThread();
    }
    if (m_verifyThread)
        stopVerifyThread();
    if (m_discovery) {
        m_discovery->stop();
        m_discovery->deleteLater();
        m_discovery = 0;
    }
    if (m_server) {
        m_server->stop();
        m_server->deleteLater();
        m_server = 0;
    }
    m_listening = false;
}

void NetworkService::announce()
{
    if (m_discovery)
        m_discovery->announce();
}

QList<NetworkService::Device> NetworkService::devices() const
{
    QList<Device> list;
    for (QMap<QString, Device>::const_iterator it = m_devices.constBegin(); it != m_devices.constEnd(); ++it)
        list.append(it.value());
    return list;
}

void NetworkService::upsertDevice(const protocol::DeviceInfo &device, const QString &address, bool registered)
{
    if (device.fingerprint.isEmpty())
        return;

    Device entry = m_devices.value(device.fingerprint);
    entry.info = device;
    if (!address.isEmpty()) {
        // A peer registers from every interface it can reach us on, so the
        // same fingerprint arrives several times with different source
        // addresses. Prefer the one that sits in one of our own subnets: that
        // is the address we want to dial later.
        const bool candidateLocal = isLocalAddress(address);
        const bool currentLocal = isLocalAddress(entry.address);
        if (entry.address.isEmpty() || (candidateLocal && !currentLocal))
            entry.address = address;
    }
    entry.lastSeen = QDateTime::currentDateTime();
    entry.registered = entry.registered || registered;
    m_devices.insert(device.fingerprint, entry);

    updateStatusLine();
    emit devicesChanged();
}

void NetworkService::applySettings()
{
    if (m_server)
        m_server->setReceivePin(m_settings->receivePin());
}

void NetworkService::pruneDevices()
{
    // A peer that does not answer our announcements for 15 s is gone: either its
    // window was closed or it moved to another network. Without this the list
    // keeps phantom entries forever.
    const QDateTime threshold = QDateTime::currentDateTime().addSecs(-15);
    bool removed = false;
    for (QMap<QString, Device>::iterator it = m_devices.begin(); it != m_devices.end();) {
        if (it.value().lastSeen < threshold) {
            logger::write(QStringLiteral("devices: forgot %1 (%2)")
                              .arg(it.value().info.alias, it.key().left(8)));
            it = m_devices.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) {
        updateStatusLine();
        emit devicesChanged();
    }
}

void NetworkService::forgetDeviceByAddress(const QString &address)
{
    bool removed = false;
    for (QMap<QString, Device>::iterator it = m_devices.begin(); it != m_devices.end();) {
        if (it.value().address.compare(address, Qt::CaseInsensitive) == 0) {
            logger::write(QStringLiteral("devices: forgot %1 (%2) after failed send")
                              .arg(it.value().info.alias, it.key().left(8)));
            it = m_devices.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) {
        updateStatusLine();
        emit devicesChanged();
    }
}

void NetworkService::onIncomingFinished(const QString &fileId, const QString &path, const QString &error)
{
    Q_UNUSED(fileId);
    ++m_incomingDone;
    if (m_incomingFirstPath.isEmpty() && !path.isEmpty())
        m_incomingFirstPath = path;
    if (!path.isEmpty()) {
        m_incomingPaths.append(path);
        // A folder transfer lands as saveDir/<folder>/<sub>/<file>; the history
        // menu should reveal <folder>, not one file inside it.
        QString root = path;
        if (!m_incomingSaveDir.isEmpty()) {
            const QString relative = QDir(m_incomingSaveDir).relativeFilePath(path);
            const int slash = relative.indexOf(QLatin1Char('/'));
            if (slash > 0)
                root = QDir(m_incomingSaveDir).filePath(relative.left(slash));
        }
        if (!m_incomingRoots.contains(root))
            m_incomingRoots.append(root);
    }
    if (!error.isEmpty() && m_incomingError.isEmpty())
        m_incomingError = error;

    // A broken transfer must be finalised at once: the server only reports the
    // file it was writing when the sender hung up, so waiting for the remaining
    // uploads would leave `receiving()` true forever and block the next send.
    const bool broken = !m_incomingError.isEmpty();
    if (m_incomingFiles > 0 && (m_incomingDone >= m_incomingFiles || broken))
        finishIncoming();

    emit incomingFinished(fileId, path, error);
}

void NetworkService::onSessionCancelled(const QString &address)
{
    // The sender cancelled between two files (or never started): no
    // uploadFinished arrives for the file that was never opened, so this is the
    // only place that can close the session bookkeeping.
    if (m_incomingFiles > 0 && m_incomingDone < m_incomingFiles) {
        if (m_incomingError.isEmpty())
            m_incomingError = tr("发送方中断了传输");
        logger::write(QStringLiteral("receive: session cancelled by %1 (%2/%3 file(s))")
                          .arg(address)
                          .arg(m_incomingDone)
                          .arg(m_incomingFiles));
        finishIncoming();
    }
    emit incomingCancelled(address);
}

void NetworkService::finishIncoming()
{
    const QString peer = m_incomingPeer;
    const int files = m_incomingFiles > 0 ? m_incomingFiles : m_incomingDone;
    const qint64 bytes = m_incomingBytes;
    const bool ok = m_incomingError.isEmpty();
    const int doneSoFar = m_incomingDone;
    const QString failure = m_incomingError;

    history::Entry entry;
    entry.time = QDateTime::currentDateTime();
    entry.direction = QStringLiteral("receive");
    entry.peer = peer;
    entry.fingerprint = m_incomingFingerprint;
    entry.fileCount = files;
    entry.bytes = bytes;
    entry.ok = ok;
    entry.detail = ok ? m_incomingFirstPath : m_incomingError;
    entry.paths = m_incomingPaths;
    entry.roots = m_incomingRoots;
    history::append(entry);

    resetIncoming();

    if (ok)
        emit incomingCompleted(peer, files, bytes);
    else
        logger::write(QStringLiteral("receive: aborted after %1/%2 file(s): %3")
                          .arg(doneSoFar)
                          .arg(files)
                          .arg(failure));
}

void NetworkService::resetIncoming()
{
    m_incomingPeer.clear();
    m_incomingFingerprint.clear();
    m_incomingFiles = 0;
    m_incomingBytes = 0;
    m_incomingDone = 0;
    m_incomingFirstPath.clear();
    m_incomingError.clear();
    m_incomingPaths.clear();
    m_incomingRoots.clear();
    m_incomingSaveDir.clear();
}

void NetworkService::onOfferDropped(int requestId)
{
    // The sender gave up while the receive dialog was still open. Forget the
    // offer, otherwise `receiving()` would stay true and block every send.
    resetIncoming();
    updateStatusLine();
    emit incomingOfferDropped(requestId);
}

void NetworkService::onAnnouncement(const protocol::DeviceInfo &device, const QHostAddress &address)
{
    // Strip the IPv4-mapped prefix that Qt reports for dual stack sockets.
    const QString addressString = plainAddress(address.toString());

    upsertDevice(device, addressString, false);

    // Announcements arrive in bursts (three per scan) and we broadcast every
    // five seconds, so registering on every single packet would hammer the peer
    // with keep-alive TLS handshakes. One register per peer per 8 s is enough:
    // the device entry is already refreshed by upsertDevice above.
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime last = m_lastRegister.value(device.fingerprint);
    if (last.isValid() && last.secsTo(now) < 8)
        return;
    m_lastRegister.insert(device.fingerprint, now);

    // Answer the announcement with a register call, exactly like the reference
    // app does. Deferred so the event loop can paint first: the request is
    // blocking for at most a few seconds and must not stall the UI thread's
    // current signal emission.
    QTimer::singleShot(0, this, [this, device, addressString]() {
        registerWith(device, addressString);
    });
}

void NetworkService::registerWith(const protocol::DeviceInfo &device, const QString &address)
{
    const bool tls = device.protocol.compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
    const int port = device.port > 0 ? device.port : protocol::kDefaultPort;

    const QByteArray body = QJsonDocument(protocol::deviceToJson(m_self, true)).toJson(QJsonDocument::Compact);
    const httpclient::Response response = httpclient::request(
        address, static_cast<quint16>(port), tls, QStringLiteral("POST"),
        QStringLiteral("/api/localsend/v2/register"), body, QByteArray("application/json"),
        &m_identity, true, 4000);

    if (response.status != 200) {
        logger::write(QStringLiteral("register -> %1:%2 failed: status=%3 %4")
                          .arg(address).arg(port).arg(response.status).arg(response.error));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response.body, &parseError);
    protocol::DeviceInfo peer = device;
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        const protocol::DeviceInfo info = protocol::deviceFromJson(document.object());
        // The info response has no port/protocol: keep what the announcement said.
        if (!info.alias.isEmpty())
            peer.alias = info.alias;
        if (!info.version.isEmpty())
            peer.version = info.version;
        if (!info.deviceModel.isEmpty())
            peer.deviceModel = info.deviceModel;
        if (!info.deviceType.isEmpty())
            peer.deviceType = info.deviceType;
    }

    logger::write(QStringLiteral("register -> %1:%2 ok (peer cert %3)")
                      .arg(address).arg(port).arg(response.peerFingerprint));
    upsertDevice(peer, address, true);
}

void NetworkService::onDeviceRegistered(const protocol::DeviceInfo &device, const QString &address)
{
    const QString peerAddress = plainAddress(address);
    logger::write(QStringLiteral("register <- %1 (%2) at %3")
                      .arg(device.alias, device.fingerprint, peerAddress));
    upsertDevice(device, peerAddress, true);
}

QStringList NetworkService::localSubnetBases()
{
    // Every usable IPv4 interface contributes a /24 probe range. APIPA
    // (169.254.x) is included on purpose: a VM whose adapter has no DHCP lease
    // is reachable only through that range, and skipping it made discovery
    // useless inside virtual machines.
    QStringList bases;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (int i = 0; i < interfaces.size(); ++i) {
        const QNetworkInterface &iface = interfaces.at(i);
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (flags & QNetworkInterface::IsLoopBack)
            continue;
        if (!(flags & QNetworkInterface::IsUp))
            continue;
        // Windows XP (and a few virtual adapters on newer systems) sometimes
        // reports an adapter as up but not running while it still carries a
        // usable IPv4 address. Requiring both flags left the scan with no
        // ranges at all on those machines, so only the address decides.

        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (int j = 0; j < entries.size(); ++j) {
            const QHostAddress address = entries.at(j).ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            if (address.isLoopback())
                continue;
            const QString text = address.toString();
            const int lastDot = text.lastIndexOf(QLatin1Char('.'));
            if (lastDot <= 0)
                continue;
            const QString base = text.left(lastDot);
            if (!bases.contains(base))
                bases.append(base);
        }
    }
    return bases;
}

QString NetworkService::localSubnetBase()
{
    const QStringList bases = localSubnetBases();
    // Prefer a real LAN over APIPA when both exist, but never drop APIPA.
    for (int i = 0; i < bases.size(); ++i) {
        if (!bases.at(i).startsWith(QLatin1String("169.254")))
            return bases.at(i);
    }
    return bases.isEmpty() ? QString() : bases.first();
}

bool NetworkService::isLocalAddress(const QString &address)
{
    if (address.isEmpty())
        return false;
    const QStringList bases = localSubnetBases();
    for (int i = 0; i < bases.size(); ++i) {
        if (address.startsWith(bases.at(i) + QLatin1Char('.')))
            return true;
    }
    return false;
}

namespace {

// One /24 sweep. Split from NetworkService::scanSubnet so the GUI can run it on
// a worker thread and hand the hits back instead of touching member state from
// there.
QList<QPair<protocol::DeviceInfo, QString> > findPeersInBase(const QString &baseAddress,
                                                            const crypto::Identity &identity,
                                                            const QString &ownFingerprint,
                                                            int probeMs)
{
    QList<QPair<protocol::DeviceInfo, QString> > hits;
    if (baseAddress.isEmpty())
        return hits;

    // A TCP connect to an unreachable host costs one SYN timeout (seconds), so
    // asking 254 hosts for /info one by one took minutes. Opening every socket
    // at once and waiting a short budget turns the sweep into a sub-second
    // operation on a healthy LAN - but Windows XP caps half-open connections at
    // ten and has a very small ephemeral port pool, so a 254-wide burst made
    // every following connection (including the TLS handshake right after)
    // time out. Old systems probe in a small window instead.
    const bool modernWindows = theme::isVistaOrLater()
            && !qEnvironmentVariableIsSet("LSAERO_FORCE_LEGACY_SCAN");
    const int window = modernWindows ? 254 : 8;
    const int hostCount = 254;
    const int budget = modernWindows ? probeMs : qMin(probeMs, 250);
    // On a modern system every host is probed at once, so one probe timeout is
    // the whole sweep. The windowed sweep needs room for its batches instead.
    const int sweepBudget = modernWindows
            ? budget
            : ((hostCount + window - 1) / window) * (budget + 150) + 1000;

    QList<QTcpSocket *> probes;
    QList<QString> addresses;
    probes.reserve(hostCount);
    for (int host = 1; host < 255; ++host) {
        addresses.append(QStringLiteral("%1.%2").arg(baseAddress).arg(host));
        probes.append(0);
    }

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

    int next = 0;
    int pending = 0;
    int finished = 0;

    // Fills the window; the timer keeps refilling it as probes settle, which
    // keeps the socket count bounded without a recursive callback chain.
    QTimer pump;
    pump.setInterval(10);
    const auto fill = [&]() {
        while (pending < window && next < hostCount) {
            const int index = next++;
            QTcpSocket *probe = new QTcpSocket;
            probes[index] = probe;
            ++pending;
            // A host outside our /24 never answers, so its connect can hang for
            // the whole sweep and stall the window. Settle every probe exactly
            // once: by its own signal, or by the per-window timeout.
            probe->setProperty("settled", false);
            const auto settle = [probe, &finished, &pending]() {
                if (probe->property("settled").toBool())
                    return;
                probe->setProperty("settled", true);
                ++finished;
                --pending;
            };
            QObject::connect(probe, &QTcpSocket::connected, &loop, settle);
            QObject::connect(probe,
                             static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(
                                     &QAbstractSocket::error),
                             &loop, [settle](QAbstractSocket::SocketError) { settle(); });
            QTimer::singleShot(budget + 100, probe, [probe, settle]() {
                probe->abort();
                settle();
            });
            probe->connectToHost(addresses.at(index), protocol::kDefaultPort);
        }
        if ((pending == 0 && next >= hostCount) || finished >= hostCount)
            loop.quit();
    };
    QObject::connect(&pump, &QTimer::timeout, &loop, fill);
    pump.start();
    fill();

    deadline.start(sweepBudget);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    deadline.stop();
    pump.stop();

    QList<QString> reachable;
    for (int i = 0; i < probes.size(); ++i) {
        QTcpSocket *probe = probes.at(i);
        if (!probe)
            continue;
        if (probe->state() == QAbstractSocket::ConnectedState)
            reachable.append(addresses.at(i));
        probe->abort();
        delete probe;
    }
    logger::write(QStringLiteral("scan: %1.x probed %2 host(s), window %3, %4 reachable")
                      .arg(baseAddress)
                      .arg(next)
                      .arg(window)
                      .arg(reachable.size()));

    for (int i = 0; i < reachable.size(); ++i) {
        const QString address = reachable.at(i);

        bool matched = false;
        for (int attempt = 0; attempt < 2 && !matched; ++attempt) {
            const bool tls = (attempt == 0);
            const httpclient::Response response = httpclient::request(
                address, protocol::kDefaultPort, tls, QStringLiteral("GET"),
                QStringLiteral("/api/localsend/v2/info"), QByteArray(), QByteArray(),
                &identity, tls, 4000);

            if (response.status != 200)
                continue;

            const QJsonDocument document = QJsonDocument::fromJson(response.body);
            if (!document.isObject())
                continue;

            protocol::DeviceInfo info = protocol::deviceFromJson(document.object());
            if (!info.valid || info.fingerprint.compare(ownFingerprint, Qt::CaseInsensitive) == 0)
                continue;

            info.port = protocol::kDefaultPort;
            info.protocol = tls ? QStringLiteral("https") : QStringLiteral("http");
            hits.append(qMakePair(info, address));
            matched = true;
        }
    }

    logger::write(QStringLiteral("scan: %1.x done, %2 reachable, %3 peer(s)")
                      .arg(baseAddress)
                      .arg(reachable.size())
                      .arg(hits.size()));
    return hits;
}

// Sweeps every local /24 on a worker thread and hands the hits back through a
// queued call, so the GUI never blocks on a dead host.
class SubnetScanner : public QThread
{public:
    SubnetScanner(QObject *parent, const QStringList &bases, const crypto::Identity &identity,
                  const QString &ownFingerprint)
        : QThread(parent)
        , m_bases(bases)
        , m_identity(identity)
        , m_ownFingerprint(ownFingerprint)
    {
    }

    QList<QPair<protocol::DeviceInfo, QString> > hits() const { return m_hits; }

protected:
    void run() Q_DECL_OVERRIDE
    {
        for (int i = 0; i < m_bases.size(); ++i) {
            if (isInterruptionRequested())
                return;
            m_hits.append(findPeersInBase(m_bases.at(i), m_identity, m_ownFingerprint, 700));
        }
    }

private:
    QStringList m_bases;
    crypto::Identity m_identity;
    QString m_ownFingerprint;
    QList<QPair<protocol::DeviceInfo, QString> > m_hits;
};

// One peer we want to (re-)confirm: used by "refresh list" and by the manual
// "add device" entry.
struct PeerTarget
{
    QString fingerprint;
    // Fingerprint that must *not* answer (ourselves); empty disables the check.
    QString selfFingerprint;
    QString address;
    quint16 port;
    bool tls;
};

// Asks one peer for /info. Runs on a pooled thread so a "refresh list" with
// several dead entries costs one timeout in total instead of one per entry.
class VerifyTask : public QRunnable
{
public:
    VerifyTask(const PeerTarget &target, const crypto::Identity &identity, QMutex *mutex,
               QList<QPair<protocol::DeviceInfo, QString> > *hits, QSet<QString> *alive)
        : m_target(target)
        , m_identity(identity)
        , m_mutex(mutex)
        , m_hits(hits)
        , m_alive(alive)
    {
    }

    void run() Q_DECL_OVERRIDE
    {
        // One attempt, one protocol: a dead peer must disappear from the list
        // right away, so this is a single 700 ms cap with no internal retry
        // (the recorded protocol is only wrong if the peer was reinstalled).
        const httpclient::Response response = httpclient::request(
            m_target.address, m_target.port, m_target.tls, QStringLiteral("GET"),
            QStringLiteral("/api/localsend/v2/info"), QByteArray(), QByteArray(),
            &m_identity, m_target.tls, 700, std::function<bool()>(), 1);

        if (response.status != 200)
            return;

        const QJsonDocument document = QJsonDocument::fromJson(response.body);
        if (!document.isObject())
            return;

        protocol::DeviceInfo info = protocol::deviceFromJson(document.object());
        if (!info.valid)
            return;
        if (!m_target.fingerprint.isEmpty()
            && info.fingerprint.compare(m_target.fingerprint, Qt::CaseInsensitive) != 0)
            return;
        if (!m_target.selfFingerprint.isEmpty()
            && info.fingerprint.compare(m_target.selfFingerprint, Qt::CaseInsensitive) == 0) {
            logger::write(QStringLiteral("devices: %1 is this machine, ignored").arg(m_target.address));
            return;
        }

        info.port = m_target.port;
        info.protocol = m_target.tls ? QStringLiteral("https") : QStringLiteral("http");

        QMutexLocker locker(m_mutex);
        m_hits->append(qMakePair(info, m_target.address));
        m_alive->insert(info.fingerprint);
    }

private:
    PeerTarget m_target;
    crypto::Identity m_identity;
    QMutex *m_mutex;
    QList<QPair<protocol::DeviceInfo, QString> > *m_hits;
    QSet<QString> *m_alive;
};

class PeerVerifier : public QThread
{
public:
    PeerVerifier(QObject *parent, const QList<PeerTarget> &targets, const crypto::Identity &identity,
                 bool manual)
        : QThread(parent)
        , m_targets(targets)
        , m_identity(identity)
        , m_manual(manual)
    {
    }

    QList<QPair<protocol::DeviceInfo, QString> > hits() const { return m_hits; }
    QSet<QString> alive() const { return m_alive; }
    bool manual() const { return m_manual; }
    // Fingerprints that existed when the refresh started: a peer that announced
    // itself while the sweep was running must not be dropped just because it was
    // not part of that sweep.
    QSet<QString> requested() const
    {
        QSet<QString> keys;
        for (int i = 0; i < m_targets.size(); ++i)
            keys.insert(m_targets.at(i).fingerprint);
        return keys;
    }

protected:
    void run() Q_DECL_OVERRIDE
    {
        QElapsedTimer timer;
        timer.start();
        QMutex mutex;
        QThreadPool pool;
        pool.setMaxThreadCount(qMax(1, m_targets.size()));
        for (int i = 0; i < m_targets.size(); ++i)
            pool.start(new VerifyTask(m_targets.at(i), m_identity, &mutex, &m_hits, &m_alive));

        // Every task gives up after two 700 ms timeouts at the latest.
        pool.waitForDone(4000);
        logger::write(QStringLiteral("verify: %1 target(s) done in %2 ms (%3 alive)")
                          .arg(m_targets.size())
                          .arg(timer.elapsed())
                          .arg(m_alive.size()));
    }

private:
    QList<PeerTarget> m_targets;
    crypto::Identity m_identity;
    bool m_manual;
    QList<QPair<protocol::DeviceInfo, QString> > m_hits;
    QSet<QString> m_alive;
};

} // namespace

void NetworkService::refreshDevices()
{
    if (!m_listening || m_verifyThread)
        return;

    // Ask every peer we show whether it is really still there. A peer that was
    // closed answers with a refused connection (instant) or nothing (one
    // timeout), so the list can be corrected in well under two seconds instead
    // of waiting for the 15 s prune timeout.
    QList<PeerTarget> targets;
    for (QMap<QString, Device>::const_iterator it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
        PeerTarget target;
        target.fingerprint = it.key();
        target.address = it.value().address;
        target.port = it.value().info.port > 0 ? it.value().info.port : m_port;
        target.tls = it.value().info.protocol.compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
        targets.append(target);
    }

    if (!targets.isEmpty()) {
        PeerVerifier *verifier = new PeerVerifier(this, targets, m_identity, false);
        m_verifyThread = verifier;
        connect(verifier, &QThread::finished, this, &NetworkService::applyVerifyResults);
        logger::write(QStringLiteral("devices: refreshing %1 peer(s)").arg(targets.size()));
        verifier->start();
    }

    // Look for peers that are not in the list yet (and let UDP-only peers learn
    // about us without waiting for the next keep-alive).
    if (m_discovery)
        m_discovery->announceOnce();
    startSubnetScan();
}

void NetworkService::addManualDevice(const QString &host, quint16 port)
{
    const QString address = host.trimmed();
    if (address.isEmpty() || !m_listening) {
        emit manualDeviceFailed(address, port);
        return;
    }
    if (m_verifyThread) {
        emit manualDeviceFailed(address, port);
        return;
    }

    m_manualHost = address;
    m_manualPort = port;

    // Probe both transports at once: a peer that turned HTTPS off answers the
    // plain request, and handing our own fingerprint over makes the probe drop
    // ourselves instead of listing the local machine.
    QList<PeerTarget> targets;
    for (int i = 0; i < 2; ++i) {
        PeerTarget target;
        target.address = address;
        target.port = port;
        target.tls = (i == 0);
        target.selfFingerprint = m_self.fingerprint;
        targets.append(target);
    }

    PeerVerifier *verifier = new PeerVerifier(this, targets, m_identity, true);
    m_verifyThread = verifier;
    connect(verifier, &QThread::finished, this, &NetworkService::applyVerifyResults);
    logger::write(QStringLiteral("devices: manual probe %1:%2").arg(address).arg(port));
    verifier->start();
}

void NetworkService::setAlias(const QString &alias)
{
    if (alias.isEmpty() || alias == m_self.alias)
        return;
    m_self.alias = alias;
    // Both the announcement (Discovery) and the /info reply (HttpServer) carry
    // the alias, so both copies have to be updated for peers to see the new
    // name right away.
    if (m_discovery)
        m_discovery->setSelf(m_self);
    if (m_server)
        m_server->setSelf(m_self);
    // Peers that already know us keep the old name until the next announcement
    // or refresh, so nudge them now.
    if (m_discovery)
        m_discovery->announceOnce();
    logger::write(QStringLiteral("network: alias changed to %1").arg(alias));
}

void NetworkService::applyVerifyResults()
{
    PeerVerifier *verifier = static_cast<PeerVerifier *>(m_verifyThread);
    m_verifyThread = 0;
    if (!verifier)
        return;

    const QList<QPair<protocol::DeviceInfo, QString> > hits = verifier->hits();
    const QSet<QString> alive = verifier->alive();
    const QSet<QString> requested = verifier->requested();

    if (verifier->manual()) {
        if (hits.isEmpty()) {
            logger::write(QStringLiteral("devices: manual probe failed for %1:%2")
                              .arg(m_manualHost).arg(m_manualPort));
            emit manualDeviceFailed(m_manualHost, m_manualPort);
        } else {
            upsertDevice(hits.first().first, hits.first().second, false);
            emit manualDeviceAdded(hits.first().first.alias);
        }
        verifier->deleteLater();
        return;
    }

    int dropped = 0;
    for (QMap<QString, Device>::iterator it = m_devices.begin(); it != m_devices.end();) {
        if (alive.contains(it.key()) || !requested.contains(it.key())) {
            ++it;
            continue;
        }
        logger::write(QStringLiteral("devices: dropped %1 (%2) on refresh")
                          .arg(it.value().info.alias, it.value().address));
        ++dropped;
        it = m_devices.erase(it);
    }

    for (int i = 0; i < hits.size(); ++i)
        upsertDevice(hits.at(i).first, hits.at(i).second, false);

    logger::write(QStringLiteral("devices: refresh done, %1 alive, %2 dropped")
                      .arg(alive.size()).arg(dropped));
    if (dropped > 0 || !hits.isEmpty()) {
        updateStatusLine();
        emit devicesChanged();
    }
    verifier->deleteLater();
}

void NetworkService::stopVerifyThread()
{
    PeerVerifier *verifier = static_cast<PeerVerifier *>(m_verifyThread);
    m_verifyThread = 0;
    if (!verifier)
        return;
    verifier->disconnect(this);
    verifier->requestInterruption();
    if (!verifier->wait(5000)) {
        // Still sweeping: deleting a running QThread aborts the process, so keep
        // it alive and let it finish on its own (it disconnects itself).
        logger::write(QStringLiteral("verify: thread still running on shutdown"));
        verifier->setParent(0);
        connect(verifier, &QThread::finished, verifier, &QObject::deleteLater);
        return;
    }
    delete verifier;
}

void NetworkService::startSubnetScan()
{
    // One sweep at a time, never while a transfer is using the wire.
    if (m_scanThread || !m_listening || m_sending || receiving())
        return;
    if (m_subnetBases.isEmpty())
        return;

    SubnetScanner *scanner = new SubnetScanner(this, m_subnetBases, m_identity, m_self.fingerprint);
    m_scanThread = scanner;
    connect(scanner, &QThread::finished, this, &NetworkService::applyScanResults);
    logger::write(QStringLiteral("scan: sweeping %1")
                      .arg(m_subnetBases.join(QLatin1String(", "))));
    scanner->start();
}

void NetworkService::applyScanResults()
{
    SubnetScanner *scanner = static_cast<SubnetScanner *>(m_scanThread);
    m_scanThread = 0;
    if (!scanner)
        return;

    const QList<QPair<protocol::DeviceInfo, QString> > hits = scanner->hits();
    for (int i = 0; i < hits.size(); ++i)
        upsertDevice(hits.at(i).first, hits.at(i).second, false);
    logger::write(QStringLiteral("scan: sweep found %1 peer(s)").arg(hits.size()));
    scanner->deleteLater();
}

void NetworkService::stopScanThread()
{
    SubnetScanner *scanner = static_cast<SubnetScanner *>(m_scanThread);
    m_scanThread = 0;
    if (!scanner)
        return;
    scanner->disconnect(this);
    scanner->requestInterruption();
    if (!scanner->wait(5000)) {
        // A /24 sweep can outlive the shutdown timeout; leaking one finished
        // thread is far cheaper than deleting a running one (that aborts).
        logger::write(QStringLiteral("scan: thread still running on shutdown"));
        scanner->setParent(0);
        connect(scanner, &QThread::finished, scanner, &QObject::deleteLater);
        return;
    }
    delete scanner;
}

QList<NetworkService::Device> NetworkService::scanSubnet(const QString &baseAddress, int timeoutMs)
{
    QList<Device> found;
    const QList<QPair<protocol::DeviceInfo, QString> > hits =
        findPeersInBase(baseAddress, m_identity, m_self.fingerprint, timeoutMs);
    for (int i = 0; i < hits.size(); ++i) {
        Device entry;
        entry.info = hits.at(i).first;
        entry.address = hits.at(i).second;
        entry.lastSeen = QDateTime::currentDateTime();
        entry.registered = false;
        found.append(entry);
        upsertDevice(entry.info, entry.address, false);
    }
    return found;
}

void NetworkService::onPrepareUpload(const HttpServer::PendingUpload &offer)
{
    IncomingOffer incoming;
    incoming.requestId = offer.requestId;
    incoming.sender = offer.info;
    incoming.address = plainAddress(offer.address);
    for (QMap<QString, protocol::FileDto>::const_iterator it = offer.files.constBegin();
         it != offer.files.constEnd(); ++it) {
        incoming.files.append(it.value());
    }

    logger::write(QStringLiteral("offer <- %1 (%2 files) from %3")
                      .arg(incoming.sender.alias)
                      .arg(incoming.files.size())
                      .arg(incoming.address));

    m_incomingPeer = incoming.sender.alias;
    m_incomingFingerprint = incoming.sender.fingerprint;
    m_incomingFiles = incoming.files.size();
    m_incomingBytes = 0;
    m_incomingDone = 0;
    m_incomingFirstPath.clear();
    m_incomingError.clear();
    m_incomingPaths.clear();
    m_incomingRoots.clear();
    m_incomingSaveDir.clear();
    for (int i = 0; i < incoming.files.size(); ++i)
        m_incomingBytes += incoming.files.at(i).size;

    if (m_autoReject) {
        const int requestId = incoming.requestId;
        logger::write(QStringLiteral("auto-reject offer %1 from %2 (%3 files) [debug]")
                          .arg(requestId)
                          .arg(incoming.sender.alias)
                          .arg(incoming.files.size()));
        QTimer::singleShot(0, this, [this, requestId]() {
            respondToOffer(requestId, false, QString());
        });
        return;
    }

    if (m_autoAccept) {
        const int requestId = incoming.requestId;
        logger::write(QStringLiteral("auto-accept offer %1 from %2 (%3 files)")
                          .arg(requestId)
                          .arg(incoming.sender.alias)
                          .arg(incoming.files.size()));
        // Answer outside this signal emission: respondToOffer() writes to the
        // pending connection, which must not happen while the server is still
        // walking its own signal list.
        QTimer::singleShot(0, this, [this, requestId]() {
            respondToOffer(requestId, true, m_autoAcceptDir);
        });
        return;
    }

    emit incomingOffer(incoming);
}

void NetworkService::setAutoAccept(bool on, const QString &saveDir)
{
    m_autoAccept = on;
    m_autoAcceptDir = saveDir;
}

void NetworkService::setAutoReject(bool on)
{
    m_autoReject = on;
}

void NetworkService::respondToOffer(int requestId, bool accept, const QString &saveDir)
{
    if (!m_server)
        return;

    if (!accept) {
        m_server->answerPrepareUpload(requestId, QMap<QString, QString>(), QString(), QString());
        resetIncoming();
        logger::write(QStringLiteral("offer %1 declined").arg(requestId));
        updateStatusLine();
        return;
    }

    const HttpServer::PendingUpload pending = m_server->pendingUpload(requestId);
    if (pending.requestId <= 0) {
        // The peer hung up while the dialog was open: nothing to answer.
        m_server->answerPrepareUpload(requestId, QMap<QString, QString>(), QString(), QString());
        resetIncoming();
        updateStatusLine();
        return;
    }

    QDir().mkpath(saveDir);
    m_incomingSaveDir = saveDir;

    const QString sessionId = QUuid::createUuid().toString().mid(1, 36);
    QMap<QString, QString> tokens;
    for (QMap<QString, protocol::FileDto>::const_iterator it = pending.files.constBegin();
         it != pending.files.constEnd(); ++it) {
        tokens.insert(it.key(), QUuid::createUuid().toString().mid(1, 36));
    }

    m_server->answerPrepareUpload(requestId, tokens, saveDir, sessionId);
    logger::write(QStringLiteral("offer %1 accepted, %2 file(s) -> %3")
                      .arg(requestId)
                      .arg(tokens.size())
                      .arg(saveDir));

    // The window needs the file count and the total size to draw the aggregate
    // row. With --auto-accept the offer never reaches the UI, so announce the
    // accepted session here instead of only in the dialog path.
    qint64 offeredBytes = 0;
    for (QMap<QString, protocol::FileDto>::const_iterator it = pending.files.constBegin();
         it != pending.files.constEnd(); ++it) {
        if (it.value().size > 0)
            offeredBytes += it.value().size;
    }
    emit incomingAccepted(pending.files.size(), offeredBytes);
}

void NetworkService::sendFiles(const Device &target, const QList<protocol::FileDto> &files,
                               const QList<QString> &paths, const QString &pin,
                               const QStringList &roots)
{
    if (m_sending) {
        emit sendFinished(false, tr("已有发送任务在进行"));
        return;
    }
    if (receiving()) {
        emit sendFinished(false, tr("正在接收文件，请等接收完成后再发送"));
        return;
    }
    if (files.isEmpty() || files.size() != paths.size()) {
        emit sendFinished(false, tr("没有可发送的文件"));
        return;
    }
    if (m_sendThread) {
        emit sendFinished(false, tr("上一次发送还在收尾，请稍后再试"));
        return;
    }

    SendWorker::Job job;
    job.address = target.address;
    job.port = target.info.port > 0 ? static_cast<quint16>(target.info.port) : protocol::kDefaultPort;
    job.tls = target.info.protocol.compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
    job.targetAlias = target.info.alias;
    job.targetFingerprint = target.info.fingerprint;
    job.self = m_self;
    job.files = files;
    job.paths = paths;
    job.pin = pin;
    job.identity = m_identity;
    job.textOnly = files.size() == 1 && !files.first().preview.isEmpty() && paths.first().isEmpty();

    m_sending = true;
    m_cancelSend = false;
    m_sendPeer = target.info.alias;
    m_sendFingerprint = target.info.fingerprint;
    m_sendAddress = target.address;
    m_sendFileCount = files.size();
    m_sendBytes = 0;
    m_sendPaths.clear();
    for (int i = 0; i < paths.size(); ++i) {
        if (!paths.at(i).isEmpty())
            m_sendPaths.append(paths.at(i));
    }
    m_sendRoots = roots.isEmpty() ? m_sendPaths : roots;
    for (int i = 0; i < files.size(); ++i)
        m_sendBytes += files.at(i).size;

    setStatus(tr("等待 %1 确认接收… (点取消可中止)").arg(job.targetAlias));

    // Everything from here on lives on the worker thread: prepare-upload can
    // block for half a minute while the peer shows its dialog, and a big upload
    // would otherwise freeze the window (and the cancel button).
    QThread *thread = new QThread(this);
    SendWorker *worker = new SendWorker(job);
    worker->moveToThread(thread);
    m_sendThread = thread;
    m_sendWorker = worker;

    connect(thread, &QThread::started, worker, &SendWorker::run);
    connect(worker, &SendWorker::stage, this, [this](const QString &text) { setStatus(text); });
    connect(worker, &SendWorker::progress, this, &NetworkService::sendProgress);
    connect(worker, &SendWorker::prepared, this, &NetworkService::onSendPrepared);
    connect(worker, &SendWorker::finished, this, &NetworkService::onSendWorkerFinished);
    connect(worker, &SendWorker::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread]() {
        m_sendThread = 0;
        m_sendWorker = 0;
        updateStatusLine();
        thread->deleteLater();
    });
    thread->start();
}

void NetworkService::cancelSending()
{
    m_cancelSend = true;
    if (m_sendWorker)
        m_sendWorker->requestCancel();
    logger::write(QStringLiteral("send: cancel requested"));
}

void NetworkService::cancelIncoming()
{
    if (m_server)
        m_server->cancelIncoming();
    // The user gave up on this transfer: drop the bookkeeping right away so
    // `receiving()` cannot stay true and block the next send (the sender may
    // still push a few bytes before it sees our cancel request).
    resetIncoming();
    updateStatusLine();
}

void NetworkService::onSendPrepared(const QString &sessionId)
{
    m_sendSessionId = sessionId;
    logger::write(QStringLiteral("send: session ready %1").arg(sessionId));
}

void NetworkService::onCancelRequested(const QString &sessionId, const QString &address)
{
    // Only the receiver of *this* session may cancel it, and only while we are
    // still uploading; everything else is a stale or unrelated cancel.
    if (!m_sending || sessionId.isEmpty() || sessionId != m_sendSessionId)
        return;
    if (address.compare(m_sendAddress, Qt::CaseInsensitive) != 0)
        return;
    logger::write(QStringLiteral("send: receiver cancelled session %1").arg(sessionId));
    m_cancelSend = true;
    if (m_sendWorker)
        m_sendWorker->requestCancel(true);
}

void NetworkService::onSendWorkerFinished(bool ok, const QString &message, int status, bool needsPin)
{
    Q_UNUSED(status)

    if (needsPin) {
        m_sending = false;
        m_cancelSend = false;
        // No session was created, so a late cancel from this peer must not be
        // able to match the next attempt through a stale id.
        m_sendSessionId.clear();
        updateStatusLine();
        // The UI asks for the PIN and calls sendFiles() again with it.
        emit pinRequired(m_sendPeer);
        return;
    }

    const bool cancelled = (message == tr("已取消发送") || message == tr("已取消"));
    const bool remoteCancelled = (message == tr("接收者已取消"));
    m_sending = false;
    m_cancelSend = false;
    m_sendSessionId.clear();
    updateStatusLine();

    history::Entry entry;
    entry.time = QDateTime::currentDateTime();
    entry.direction = QStringLiteral("send");
    entry.peer = m_sendPeer;
    entry.fingerprint = m_sendFingerprint;
    entry.fileCount = m_sendFileCount;
    entry.bytes = m_sendBytes;
    entry.ok = ok;
    entry.detail = ok ? m_sendAddress : message;
    entry.paths = m_sendPaths;
    entry.roots = m_sendRoots;
    history::append(entry);

    logger::write(QStringLiteral("send -> %1 ok=%2 message=%3")
                      .arg(m_sendAddress)
                      .arg(ok ? 1 : 0)
                      .arg(message));

    if (ok) {
        emit sendFinished(true, message);
        return;
    }
    if (cancelled) {
        emit sendFinished(false, tr("已取消发送"));
        return;
    }
    if (remoteCancelled) {
        emit sendFinished(false, tr("接收者已取消"));
        return;
    }
    if (message.startsWith(tr("设备已离线")))
        forgetDeviceByAddress(m_sendAddress);
    // The dialog already carries "发送失败" as its headline; repeating it in the
    // body read as "发送失败：发送失败：…".
    emit sendFinished(false, message);
}
