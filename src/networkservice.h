#ifndef NETWORKSERVICE_H
#define NETWORKSERVICE_H

#include "crypto.h"
#include "httpserver.h"
#include "protocol.h"

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

class Discovery;
class QThread;
class QTimer;
class SendWorker;
class Settings;

// Owns the device identity, the HTTPS server, the announcement socket and the
// peer table. The UI only talks to this class.
class NetworkService : public QObject
{
    Q_OBJECT

public:
    struct Device
    {
        Device() : registered(false) {}

        protocol::DeviceInfo info;
        QString address;      // IPv4 the peer is reachable at
        QDateTime lastSeen;
        bool registered;      // true once a full /register round trip succeeded
    };

    explicit NetworkService(Settings *settings, QObject *parent = 0);
    ~NetworkService();

    // One prepare-upload request waiting for the user to accept or decline.
    struct IncomingOffer
    {
        IncomingOffer() : requestId(-1) {}

        int requestId;
        protocol::DeviceInfo sender;
        QList<protocol::FileDto> files;
        QString address;
    };

    bool start(QString *error);
    void stop();

    // Command line override for the listening port. It must not be written back
    // to lsaero.ini: a test run with --port used to change the user's config.
    void setPortOverride(quint16 port) { m_portOverride = port; }

    const crypto::Identity &identity() const { return m_identity; }
    const protocol::DeviceInfo &self() const { return m_self; }
    QList<Device> devices() const;
    int deviceCount() const { return m_devices.size(); }

    // Local /24 probes, the fallback LocalSend itself uses when the multicast
    // announcement does not get confirmed within one second. Blocking, keep it
    // off the GUI thread (the --scan switch uses it directly).
    QList<Device> scanSubnet(const QString &baseAddress, int timeoutMs);
    // One base per usable IPv4 interface (APIPA included, otherwise a VM with
    // no DHCP lease would never scan anything).
    static QStringList localSubnetBases();
    static QString localSubnetBase();
    // True when the address belongs to one of our own /24s.
    static bool isLocalAddress(const QString &address);

    // "Refresh list": ask every known peer for /info before painting the list,
    // so a peer that was shut down disappears at once instead of lingering
    // until the 15 s prune timeout, then sweep the subnets for new ones.
    void refreshDevices();
    // Manual "add device by address": query /info on host:port and add the peer
    // to the list when it answers (used where multicast and scanning are both
    // blocked).
    void addManualDevice(const QString &host, quint16 port);

    // Live rename: the device alias shown to peers and in our own header.
    void setAlias(const QString &alias);
    QString statusLine() const { return m_statusLine; }
    bool listening() const { return m_listening; }
    quint16 port() const { return m_port; }

    // Sending: one session at a time. The transfer runs on a worker thread, so
    // the window keeps repainting and the cancel button stays clickable. A text
    // message is a single FileDto whose body lives in `preview`; its entry in
    // `paths` is empty and the bytes are sent from memory.
    bool sending() const { return m_sending; }
    bool receiving() const { return m_incomingFiles > 0 && m_incomingDone < m_incomingFiles; }
    void sendFiles(const Device &target, const QList<protocol::FileDto> &files,
                   const QList<QString> &paths, const QString &pin = QString(),
                   const QStringList &roots = QStringList());
    void cancelSending();

    // Abort the receive session in progress (partial files are dropped).
    void cancelIncoming();

    // Re-reads settings that the server needs (currently only the PIN).
    void applySettings();

    // Receiving: called from the UI once the user answered an offer.
    // Test hook: answer every offer with "accept" into `saveDir` and never ask
    // the UI. Keeping it in the service (instead of the window) matters: a
    // second automatic answer next to the modal dialog left every offer stuck
    // on "waiting for user".
    void setAutoAccept(bool on, const QString &saveDir);
    // Debug helper for headless tests: answer every offer with "reject". Used by
    // the command line to reproduce "peer refused, send again" without driving
    // the GUI.
    void setAutoReject(bool on);

    void respondToOffer(int requestId, bool accept, const QString &saveDir);

public slots:
    void announce();

signals:
    void devicesChanged();
    void statusChanged(const QString &status);
    void logLine(const QString &line);

    void incomingOffer(const NetworkService::IncomingOffer &offer);
    // The peer closed the connection while its offer was still on screen.
    void incomingOfferDropped(int requestId);
    // Emitted right after we answer a prepare-upload with "accept": the window
    // uses it to show the whole-session progress row, including the
    // --auto-accept path where the offer never reaches the UI.
    void incomingAccepted(int files, qint64 bytes);
    void incomingProgress(const QString &fileId, qint64 received, qint64 total);
    void incomingFinished(const QString &fileId, const QString &path, const QString &error);
    void incomingCompleted(const QString &peer, int files, qint64 bytes);
    void incomingCancelled(const QString &address);
    void sendProgress(const QString &fileId, qint64 sent, qint64 total);
    void sendFinished(bool ok, const QString &message);

    // The peer wants a PIN before it accepts the files; the UI asks for one and
    // calls sendFiles() again with it.
    void pinRequired(const QString &alias);

    // A manual "add device" did not reach anyone at that address.
    void manualDeviceFailed(const QString &host, quint16 port);
    // The peer added by hand answered and is now in the device list.
    void manualDeviceAdded(const QString &alias);

private slots:
    void onAnnouncement(const protocol::DeviceInfo &device, const QHostAddress &address);
    void onDeviceRegistered(const protocol::DeviceInfo &device, const QString &address);
    void onPrepareUpload(const HttpServer::PendingUpload &offer);
    void onIncomingFinished(const QString &fileId, const QString &path, const QString &error);
    // The server dropped a session while files were still outstanding: without
    // this the counters would keep `receiving()` true forever.
    void onSessionCancelled(const QString &address);
    void onSendWorkerFinished(bool ok, const QString &message, int status, bool needsPin);
    void onSendPrepared(const QString &sessionId);
    void onCancelRequested(const QString &sessionId, const QString &address);
    void onOfferDropped(int requestId);
    void pruneDevices();
    // Subnet sweep results, applied on the GUI thread once the worker is done.
    void applyScanResults();
    // Verification results of a "refresh list" / manual add, applied on the GUI thread.
    void applyVerifyResults();
    // A send that failed because the peer is unreachable drops it from the list
    // right away instead of waiting for the 15 s timeout.
    void forgetDeviceByAddress(const QString &address);

private:
    void resetIncoming();
    // Writes the history entry and clears the counters once a receive is over,
    // whether it completed or was cut short.
    void finishIncoming();
    void upsertDevice(const protocol::DeviceInfo &device, const QString &address, bool registered);
    void registerWith(const protocol::DeviceInfo &device, const QString &address);
    void setStatus(const QString &status);
    void updateStatusLine();
    // LocalSend's fallback: multicast alone is not enough on networks that drop
    // broadcasts (bridged VMs, access points with client isolation, firewalls),
    // so sweep our own /24s for peers that listen on 53317.
    void startSubnetScan();
    // Stops the sweep thread (kept out of line: it needs the worker type).
    void stopScanThread();
    void stopVerifyThread();

    Settings *m_settings;
    crypto::Identity m_identity;
    protocol::DeviceInfo m_self;
    HttpServer *m_server;
    Discovery *m_discovery;
    QTimer *m_announceTimer;
    QTimer *m_pruneTimer;
    QTimer *m_scanTimer;
    QThread *m_scanThread;
    QThread *m_verifyThread;
    // Target of the manual "add device" in flight, used for the failure hint.
    QString m_manualHost;
    quint16 m_manualPort;
    QMap<QString, Device> m_devices;
    QHash<QString, QDateTime> m_lastRegister;
    QString m_statusLine;
    QString m_subnetBase;
    QStringList m_subnetBases;
    bool m_listening;
    quint16 m_port;
    int m_portOverride;
    bool m_sending;
    bool m_cancelSend;
    QThread *m_sendThread;
    SendWorker *m_sendWorker;

    // Bookkeeping for the history entry of the send session in progress.
    QString m_sendPeer;
    QString m_sendFingerprint;
    QString m_sendAddress;
    // Session id handed out by the receiver: a POST /v2/cancel carrying it is
    // the receiver aborting *our* upload, not a sender cancelling its upload.
    QString m_sendSessionId;
    int m_sendFileCount;
    qint64 m_sendBytes;
    // Source paths of the running send, kept for the "send again" history entry.
    QStringList m_sendPaths;
    // The files/folders the user actually picked: replaying these reproduces a
    // folder transfer, whereas m_sendPaths holds one entry per expanded file.
    QStringList m_sendRoots;

    // Bookkeeping for the history entry of the receive session in progress.
    QString m_incomingPeer;
    QString m_incomingFingerprint;
    int m_incomingFiles;
    qint64 m_incomingBytes;
    int m_incomingDone;
    QString m_incomingFirstPath;
    QString m_incomingError;
    QString m_incomingSaveDir;
    // Every file that actually landed in this session, for "open folder".
    QStringList m_incomingPaths;
    // Top level items of the receive session (the folder for folder transfers).
    QStringList m_incomingRoots;
    bool m_autoAccept;
    QString m_autoAcceptDir;
    bool m_autoReject;
};

#endif // NETWORKSERVICE_H
