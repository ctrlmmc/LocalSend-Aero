#ifndef HTTPSERVER_H
#define HTTPSERVER_H

#include "crypto.h"
#include "protocol.h"

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTcpServer>

class HttpConnection;

// Minimal HTTP/1.1 server speaking the LocalSend v2 API.
//
// Interop constraints discovered in P1:
//   * 1.18.x uploads with "Transfer-Encoding: chunked" and sends
//     "Expect: 100-continue", so both must be implemented or the sender stalls.
//   * 1.18.x server requires a client certificate; as a server we must request
//     one (QueryPeer) and match it against the advertised fingerprint, while
//     1.17.x clients simply do not present any.
class HttpServer : public QTcpServer
{
    Q_OBJECT

public:
    // One accepted-but-not-answered prepare-upload, exposed to the UI.
    struct PendingUpload
    {
        PendingUpload() : requestId(0) {}

        int requestId;
        protocol::DeviceInfo info;
        QMap<QString, protocol::FileDto> files;
        QString address;
    };

    // State of an accepted transfer, keyed by session id.
    struct Session
    {
        QString sessionId;
        QMap<QString, QString> tokens;
        QMap<QString, protocol::FileDto> files;
        QString saveDir;
        QString sender;
        // Announced endpoint of the sender: cancelling a transfer is a request
        // *to the sender*, so its port and protocol are needed here.
        protocol::DeviceInfo senderInfo;
        QSet<QString> completed;
    };

    HttpServer(const crypto::Identity &identity, QObject *parent = 0);
    // Connections are QObject children, but their destructor unregisters itself
    // from m_connections, so that set must still be alive while they go away.
    ~HttpServer();

    bool start(quint16 port, bool useTls, QString *error);
    void stop();

    void setSelf(const protocol::DeviceInfo &self);
    bool isTls() const { return m_tls; }
    quint16 port() const { return serverPort(); }

    // Empty disables the check. Senders must pass "?pin=<digits>".
    void setReceivePin(const QString &pin) { m_pin = pin; }
    QString receivePin() const { return m_pin; }

    // Copy of a still pending offer; requestId is 0 when it is gone.
    PendingUpload pendingUpload(int requestId) const { return m_pendingRequests.value(requestId); }

    // Answer a prepare-upload: accepted files get a token, the receiver also
    // decides the target directory. An empty token map rejects everything and
    // answers 204, the documented "sender closed the transfer" outcome.
    void answerPrepareUpload(int requestId, const QMap<QString, QString> &tokens,
                             const QString &saveDir, const QString &sessionId);

    // The peer closed the connection while its offer was still pending.
    void dropPendingRequest(int requestId);

    // User pressed "cancel" while receiving: close every connection that is
    // currently uploading, forget the sessions and report the cancellation.
    void cancelIncoming();

    // Sender address of a live session (used to label the cancelled transfer).
    QString sessionSender() const;

signals:
    void deviceRegistered(const protocol::DeviceInfo &device, const QString &address);
    void activity(const QString &line);
    void prepareUploadIncoming(const HttpServer::PendingUpload &request);
    void prepareUploadDropped(int requestId);

    void uploadProgress(const QString &fileId, qint64 received, qint64 total);
    void uploadFinished(const QString &fileId, const QString &path, const QString &error);
    void sessionCancelled(const QString &address);
    // We are receiving and the user pressed cancel: the sender must be told,
    // otherwise it keeps uploading the remaining files into a dead session.
    // The service layer posts /v2/cancel to the announced endpoint.
    void incomingCancelRequested(const QString &address, quint16 port, bool tls,
                                 const QString &sessionId);
    // A peer cancelled a transfer it is *receiving* from us (or announced a
    // session id we do not serve): the service layer matches it against the
    // session id of the outbound transfer that is currently running.
    void cancelRequested(const QString &sessionId, const QString &address);

protected:
    void incomingConnection(qintptr handle) override;

private:
    friend class HttpConnection;

    int nextRequestId() { return ++m_requestCounter; }
    int beginPrepareUpload(const protocol::PrepareUpload &request, const QString &address,
                           HttpConnection *connection);
    void markFileReceived(const QString &sessionId, const QString &fileId, const QString &path);
    void cancelSessionsFrom(const QString &address);

    // 0 = accepted, 401 = wrong or missing PIN, 429 = too many attempts.
    int checkPin(const QString &address, const QString &provided);

    crypto::Identity m_identity;
    protocol::DeviceInfo m_self;
    QString m_pin;
    QHash<QString, int> m_pinAttempts;
    bool m_tls;
    int m_requestCounter;
    QMap<int, QPointer<HttpConnection> > m_pendingConnections;
    QMap<int, PendingUpload> m_pendingRequests;
    QMap<QString, Session> m_sessions;
    QSet<HttpConnection *> m_connections;
    // Set while the destructor tears the connections down, so their destructors
    // know they must not touch m_connections any more.
    bool m_shuttingDown;
};

#endif // HTTPSERVER_H
