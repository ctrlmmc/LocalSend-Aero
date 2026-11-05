#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include "crypto.h"

#include <QByteArray>
#include <QMap>
#include <QString>

#include <functional>

class QSslSocket;

// Blocking HTTPS/HTTP client used by the discovery path.
//
// The reference implementation always pins the peer certificate by
// fingerprint, but for a first contact we only know the fingerprint from the
// announcement, so pinning is optional here. Errors never abort the process:
// callers get status -1 plus a readable reason.
namespace httpclient {

struct Response
{
    Response() : status(-1), complete(false) {}

    int status;
    QByteArray body;
    QMap<QString, QString> headers;
    QString peerFingerprint;
    QString error;
    // True when the whole body arrived; a keep-alive connection must not be
    // reused after a partial response.
    bool complete;
};

// `cancelled` is polled while waiting for the response; returning true aborts
// the call and yields error "cancelled". Waiting also pumps the GUI event loop
// so a long prepare-upload (the receiver keeps a dialog open) does not freeze
// the window.
Response request(const QString &host,
                 quint16 port,
                 bool tls,
                 const QString &method,
                 const QString &pathAndQuery,
                 const QByteArray &body,
                 const QByteArray &contentType,
                 const crypto::Identity *identity,
                 bool sendClientCertificate,
                 int timeoutMs = 5000,
                 const std::function<bool()> &cancelled = std::function<bool()>(),
                 int maxAttempts = 2);

// Streams one local file to the peer with a Content-Length body (no chunked:
// the size is always known here, and it keeps 1.17.x happy). The progress
// callback gets (sent, total) and may return false to abort the transfer.
// `cancelAction` runs before the socket is closed, so the caller can tell the
// peer about the cancellation while the session is still alive.
// `cancelled` is polled during the connect/handshake, where the progress
// callback never runs (a peer that went offline keeps us in Connecting).
Response upload(const QString &host,
                quint16 port,
                bool tls,
                const QString &pathAndQuery,
                const QString &filePath,
                qint64 size,
                const crypto::Identity *identity,
                int timeoutMs,
                const std::function<bool(qint64, qint64)> &progress,
                const std::function<void()> &cancelAction = std::function<void()>(),
                const std::function<bool()> &cancelled = std::function<bool()>());

// Same as upload() but takes the bytes from memory. Used for text messages so
// no temporary file has to be written (a locked-down %TEMP% used to break
// them).
Response uploadData(const QString &host,
                    quint16 port,
                    bool tls,
                    const QString &pathAndQuery,
                    const QByteArray &data,
                    const crypto::Identity *identity,
                    int timeoutMs,
                    const std::function<bool(qint64, qint64)> &progress,
                    const std::function<void()> &cancelAction = std::function<void()>(),
                    const std::function<bool()> &cancelled = std::function<bool()>());

// A keep-alive connection: one TLS handshake per transfer session instead of
// one per file. Sending a folder with a few hundred small files used to open a
// connection per file, which piles up TIME_WAIT entries on a real network and
// eventually fails with "Socket operation timed out".
class Session
{
public:
    Session();
    ~Session();

    // Opens the connection up front; returns false and fills `response` on error.
    bool open(const QString &host,
              quint16 port,
              bool tls,
              const crypto::Identity *identity,
              int timeoutMs,
              const std::function<bool()> &cancelled,
              Response *response);

    void close();
    bool isOpen();

    Response post(const QString &pathAndQuery,
                  const QByteArray &body,
                  const QByteArray &contentType,
                  int timeoutMs,
                  const std::function<bool()> &cancelled);

    Response postFile(const QString &pathAndQuery,
                      const QString &filePath,
                      qint64 size,
                      int timeoutMs,
                      const std::function<bool(qint64, qint64)> &progress,
                      const std::function<void()> &cancelAction,
                      const std::function<bool()> &cancelled);

private:
    Q_DISABLE_COPY(Session)

    bool ensure(int timeoutMs, const std::function<bool()> &cancelled, Response *response);
    void dropSocket();

    QString m_host;
    quint16 m_port;
    bool m_tls;
    crypto::Identity m_identity;
    bool m_hasIdentity;
    QSslSocket *m_socket;
};

} // namespace httpclient

#endif // HTTPCLIENT_H
