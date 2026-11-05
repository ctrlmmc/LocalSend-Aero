#include "discovery.h"

#include "logger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QTimer>
#include <QUdpSocket>

namespace {

QHostAddress interfaceIPv4(const QNetworkInterface &iface)
{
    const QList<QNetworkAddressEntry> entries = iface.addressEntries();
    for (int i = 0; i < entries.size(); ++i) {
        const QHostAddress address = entries.at(i).ip();
        if (address.protocol() == QAbstractSocket::IPv4Protocol)
            return address;
    }
    return QHostAddress();
}

} // namespace

Discovery::Discovery(QObject *parent)
    : QObject(parent)
{
}

bool Discovery::start(const protocol::DeviceInfo &self, QString *error)
{
    m_self = self;

    if (!m_sockets.isEmpty())
        return true;

    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (int i = 0; i < interfaces.size(); ++i) {
        const QNetworkInterface &iface = interfaces.at(i);
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        // Same XP caveat as the subnet scan: an adapter that is up but not
        // flagged running still carries traffic, so do not discard it. The
        // multicast flag is the only hard requirement.
        if (!(flags & QNetworkInterface::IsUp) || (flags & QNetworkInterface::IsLoopBack))
            continue;
        if (!(flags & QNetworkInterface::CanMulticast))
            continue;
        if (interfaceIPv4(iface).isNull())
            continue;

        QString joinError;
        if (!addInterfaceSocket(iface, &joinError))
            logger::write(QStringLiteral("discovery: %1 skipped: %2")
                              .arg(iface.humanReadableName(), joinError));
    }

    if (m_sockets.isEmpty()) {
        if (error)
            *error = tr("没有可用的网络接口");
        return false;
    }

    logger::write(QStringLiteral("discovery: listening on udp %1 (%2 interface sockets)")
                      .arg(protocol::kMulticastPort)
                      .arg(m_sockets.size()));
    return true;
}

bool Discovery::addInterfaceSocket(const QNetworkInterface &iface, QString *joinError)
{
    const QHostAddress localAddress = interfaceIPv4(iface);
    if (localAddress.isNull()) {
        if (joinError)
            *joinError = QStringLiteral("no IPv4 address");
        return false;
    }

    QUdpSocket *socket = new QUdpSocket(this);

    // Bind to the interface address so the kernel picks that interface for the
    // outgoing datagrams, then join the group on the same interface.
    if (!socket->bind(localAddress, protocol::kMulticastPort,
                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        // Another listener may already hold that exact address (for example a
        // second copy of this app); fall back to the wildcard bind.
        if (!socket->bind(QHostAddress::AnyIPv4, protocol::kMulticastPort,
                          QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            if (joinError)
                *joinError = socket->errorString();
            delete socket;
            return false;
        }
    }

    const QHostAddress group(QLatin1String(protocol::kMulticastGroup));
    if (!socket->joinMulticastGroup(group, iface)) {
        if (joinError)
            *joinError = socket->errorString();
        delete socket;
        return false;
    }

    socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);

    connect(socket, &QUdpSocket::readyRead, this, &Discovery::onReadyRead);
    m_sockets.append(socket);

    logger::write(QStringLiteral("discovery: joined %1 on %2 (%3)")
                      .arg(group.toString(), iface.humanReadableName(), localAddress.toString()));
    return true;
}

void Discovery::stop()
{
    for (int i = 0; i < m_sockets.size(); ++i) {
        m_sockets.at(i)->close();
        m_sockets.at(i)->deleteLater();
    }
    m_sockets.clear();
}

void Discovery::setSelf(const protocol::DeviceInfo &self)
{
    m_self = self;
}

void Discovery::announceOnce()
{
    if (m_sockets.isEmpty())
        return;
    sendAnnouncement();
}

void Discovery::announce()
{
    if (m_sockets.isEmpty())
        return;
    sendAnnouncement();
    QTimer::singleShot(500, this, [this]() { sendAnnouncement(); });
    QTimer::singleShot(2000, this, [this]() { sendAnnouncement(); });
}

void Discovery::sendAnnouncement()
{
    if (m_sockets.isEmpty() || !m_self.valid)
        return;

    QJsonObject object = protocol::deviceToJson(m_self, true);
    // Without this flag LocalSend ignores an announcement completely.
    object.insert(QStringLiteral("announce"), true);

    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const QHostAddress group(QLatin1String(protocol::kMulticastGroup));

    int writtenTotal = 0;
    for (int i = 0; i < m_sockets.size(); ++i) {
        const qint64 written = m_sockets.at(i)->writeDatagram(payload, group, protocol::kMulticastPort);
        if (written > 0)
            writtenTotal += static_cast<int>(written);
    }
    logger::write(QStringLiteral("discovery: announcement %1 bytes on %2 sockets (%3 bytes sent)")
                      .arg(payload.size())
                      .arg(m_sockets.size())
                      .arg(writtenTotal));
}

void Discovery::onReadyRead()
{
    QUdpSocket *socket = qobject_cast<QUdpSocket *>(sender());
    if (!socket)
        return;

    while (socket->hasPendingDatagrams()) {
        // QNetworkDatagram only exists since Qt 5.8, this build targets 5.6.
        QByteArray payload;
        payload.resize(static_cast<int>(socket->pendingDatagramSize()));
        QHostAddress senderAddress;
        quint16 senderPort = 0;
        const qint64 read = socket->readDatagram(payload.data(), payload.size(),
                                                 &senderAddress, &senderPort);
        if (read <= 0)
            continue;
        payload.resize(static_cast<int>(read));

        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            logger::write(QStringLiteral("discovery: unparsable datagram from %1")
                              .arg(senderAddress.toString()));
            continue;
        }

        const QJsonObject object = document.object();
        if (!object.value(QStringLiteral("announce")).toBool())
            continue;

        protocol::DeviceInfo device = protocol::deviceFromJson(object);
        if (!device.valid) {
            logger::write(QStringLiteral("discovery: announcement without fingerprint ignored"));
            continue;
        }
        if (device.fingerprint.compare(m_self.fingerprint, Qt::CaseInsensitive) == 0) {
            // Our own announcement comes back through the multicast loopback.
            // The port tells the two cases apart: an identical fingerprint on a
            // *different* port means another copy of this app reused our
            // certs\ folder, and hiding it made "two windows cannot see each
            // other" impossible to diagnose.
            if (device.port == m_self.port)
                continue;
            logger::write(QStringLiteral("discovery: %1 on port %2 shares our fingerprint (copied certs\\?)")
                              .arg(device.alias)
                              .arg(device.port));
        }

        logger::write(QStringLiteral("discovery: %1 announced by %2:%3 (%4)")
                          .arg(device.alias, senderAddress.toString())
                          .arg(device.port)
                          .arg(device.protocol));
        emit announcementReceived(device, senderAddress);
    }
}
