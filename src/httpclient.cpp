#include "httpclient.h"

#include "crypto.h"
#include "logger.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QStringList>
#include <QThread>
#include <QTimer>

namespace httpclient {

namespace {

// A retry only helps when the failure could be a leftover from the previous
// transfer. A closed port answers "Connection refused" immediately and will
// answer the same way 250 ms later, so keep those fast (the device list
// verifier relies on it: every dead peer must be dropped without a pause).
bool worthRetrying(const QString &text)
{
    const QString lower = text.toLower();
    if (lower.contains(QStringLiteral("refused")))
        return false;
    if (lower.contains(QStringLiteral("host not found")))
        return false;
    if (lower.contains(QStringLiteral("network is unreachable")))
        return false;
    return true;
}

bool decodeChunked(const QByteArray &input, QByteArray *output, bool *complete, int *consumed = 0)
{
    int offset = 0;
    QByteArray result;

    for (;;) {
        const int lineEnd = input.indexOf("\r\n", offset);
        if (lineEnd < 0)
            break;

        const QByteArray sizeLine = input.mid(offset, lineEnd - offset);
        bool ok = false;
        const int size = sizeLine.isEmpty() ? -1 : sizeLine.split(';').at(0).trimmed().toInt(&ok, 16);
        if (!ok || size < 0)
            return false;

        const int dataStart = lineEnd + 2;
        if (size == 0) {
            // Trailer headers are ignored; the terminating CRLF may still be missing.
            const int end = input.indexOf("\r\n", dataStart);
            if (end < 0)
                break;
            *output = result;
            *complete = true;
            if (consumed)
                *consumed = end + 2;
            return true;
        }

        if (input.size() < dataStart + size + 2)
            break;

        result.append(input.mid(dataStart, size));
        offset = dataStart + size + 2;
    }

    *output = result;
    *complete = false;
    return true;
}

QByteArray buildRequest(const QString &method,
                        const QString &host,
                        quint16 port,
                        const QString &pathAndQuery,
                        const QByteArray &body,
                        const QByteArray &contentType)
{
    QByteArray request;
    request.append(method.toLatin1());
    request.append(' ');
    request.append(pathAndQuery.toLatin1());
    request.append(" HTTP/1.1\r\n");
    request.append("Host: ");
    request.append(host.toLatin1());
    request.append(':');
    request.append(QByteArray::number(port));
    request.append("\r\n");
    request.append("Connection: close\r\n");
    request.append("User-Agent: LocalSend Aero/0.1\r\n");
    if (!body.isEmpty() || method == QLatin1String("POST")) {
        request.append("Content-Type: ");
        request.append(contentType.isEmpty() ? QByteArray("application/json") : contentType);
        request.append("\r\n");
        request.append("Content-Length: ");
        request.append(QByteArray::number(body.size()));
        request.append("\r\n");
    }
    request.append("\r\n");
    request.append(body);
    return request;
}

bool connectToPeer(QSslSocket *socket, const QString &host, quint16 port, bool tls,
                   const crypto::Identity *identity, bool sendClientCertificate,
                   int timeoutMs, Response *response,
                   const std::function<bool()> &cancelled = std::function<bool()>(),
                   int maxAttempts = 2)
{
    // A peer that went offline stays in Connecting for ~20 s (TCP SYN retries);
    // capping this keeps "device unreachable" quick. A *busy* peer needs more
    // room though: the first folder transfer of a fresh install regularly lost
    // the TLS handshake at 6 s, so the cap is 10 s and a timed out handshake is
    // retried once before the error surfaces.
    const int connectTimeout = qMin(timeoutMs, 10000);
    const bool guiThread = (QThread::currentThread() == QCoreApplication::instance()->thread());

    // QSslSocket has both a sslErrors() getter and a sslErrors() signal.
    if (tls) {
        QObject::connect(socket,
                         static_cast<void (QSslSocket::*)(const QList<QSslError> &)>(&QSslSocket::sslErrors),
                         [socket](const QList<QSslError> &) { socket->ignoreSslErrors(); });
    }

    QString lastError;
    for (int attempt = 0; attempt < qMax(1, maxAttempts); ++attempt) {
        if (attempt > 0) {
            // The first attempt died (timeout, or the peer closed the connection
            // while it was still tearing down the previous transfer): start from
            // a clean socket instead of reusing whatever half-open state Qt left
            // behind, and give the listener a moment to settle.
            socket->abort();
            logger::write(QStringLiteral("client %1:%2 retrying %3 connection after: %4")
                              .arg(host)
                              .arg(port)
                              .arg(tls ? QStringLiteral("tls") : QStringLiteral("plain"))
                              .arg(lastError));
            {
                // Short, cancellable pause. QThread::msleep is protected, and a
                // local event loop also keeps the GUI thread pumping (the peer
                // may be connecting back to us at this very moment).
                QEventLoop pause;
                QTimer::singleShot(guiThread ? 250 : 150, &pause, SLOT(quit()));
                pause.exec();
            }
        }

        if (tls) {
            QSslConfiguration configuration = socket->sslConfiguration();
            configuration.setPeerVerifyMode(QSslSocket::QueryPeer);
            if (identity && sendClientCertificate && identity->valid) {
                configuration.setLocalCertificate(identity->certificate);
                configuration.setPrivateKey(identity->privateKey);
            }
            socket->setSslConfiguration(configuration);
            socket->connectToHostEncrypted(host, port);
            // On the GUI thread the handshake must not block: the peer may be
            // calling us back at the same moment, and our own QTcpServer only
            // accepts that connection while this process keeps pumping events.
            // On a worker thread (SendWorker) blocking waits are correct — and
            // processEvents() from a non-GUI thread would fight the main loop.
            QElapsedTimer handshakeTimer;
            handshakeTimer.start();
            while (!socket->isEncrypted()) {
                if (guiThread)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                else
                    socket->waitForEncrypted(200);
                // A peer that went offline leaves us in Connecting for ~20 s
                // (TCP SYN retries); without this check the cancel button never
                // leaves its "cancelling" state.
                if (cancelled && cancelled()) {
                    socket->abort();
                    response->error = QStringLiteral("cancelled");
                    return false;
                }
                if (socket->state() == QAbstractSocket::UnconnectedState)
                    break;
                if (handshakeTimer.elapsed() >= connectTimeout)
                    break;
            }
            if (!socket->isEncrypted()) {
                // Include the SSL library state: on Windows XP the usual reason
                // for a failing handshake is a missing/mismatched OpenSSL DLL,
                // and "Socket operation timed out" alone sends everyone hunting
                // in the wrong direction.
                lastError = QStringLiteral("TLS handshake failed: ") + socket->errorString();
                if (!QSslSocket::supportsSsl())
                    lastError += QStringLiteral(" (no TLS backend: ") + QSslSocket::sslLibraryBuildVersionString() + QLatin1Char(')');
                logger::write(QStringLiteral("client %1:%2 tls error: %3").arg(host).arg(port).arg(lastError));
                // Retry only when it might help: a peer that just answered
                // (rejected / cancelled) closes this connection from its side,
                // and the very next attempt then hits a listener that is still
                // tearing the old session down. One quiet retry hides that
                // window instead of telling the user the device went offline.
                if (!worthRetrying(lastError))
                    break;
                continue;
            }
            response->peerFingerprint = crypto::fingerprintOf(socket->peerCertificate());
            return true;
        }

        socket->connectToHost(host, port);
        QElapsedTimer connectTimer;
        connectTimer.start();
        while (socket->state() != QAbstractSocket::ConnectedState) {
            if (guiThread)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            else
                socket->waitForConnected(200);
            if (cancelled && cancelled()) {
                socket->abort();
                response->error = QStringLiteral("cancelled");
                return false;
            }
            if (socket->state() == QAbstractSocket::UnconnectedState)
                break;
            if (connectTimer.elapsed() >= connectTimeout)
                break;
        }
        if (socket->state() != QAbstractSocket::ConnectedState) {
            lastError = QStringLiteral("connect failed: ") + socket->errorString();
            // Same reasoning as the handshake branch: retry only the transient
            // cases, report a refused port right away.
            if (!worthRetrying(lastError))
                break;
            continue;
        }
        return true;
    }

    response->error = lastError;
    return false;
}

// Reads one HTTP response (Content-Length, chunked or close-delimited).
// While waiting it keeps the GUI responsive and honours the cancel flag, so a
// receiver that keeps its confirmation dialog open for a long time does not
// make our window look hung.
void readResponse(QSslSocket *socket, int timeoutMs, Response *response,
                  const std::function<bool()> &cancelled = std::function<bool()>())
{
    QByteArray buffer;
    QByteArray payload;
    bool headersDone = false;
    bool bodyComplete = false;
    int status = -1;
    QMap<QString, QString> headers;

    QElapsedTimer timer;
    timer.start();
    const bool guiThread = (QThread::currentThread() == QCoreApplication::instance()->thread());

    while (!bodyComplete && timer.elapsed() < timeoutMs) {
        // On the GUI thread always pump, so the peer's own requests still land
        // while we wait for its answer. A worker thread must not touch the main
        // event dispatcher; the blocking wait below is enough there.
        if (guiThread)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (cancelled && cancelled()) {
            response->error = QStringLiteral("cancelled");
            // Close with a FIN instead of a RST: the peer's server can only
            // notice that the request died once the connection shuts down
            // cleanly (LocalSend 1.17 keeps its dialog open when we reset).
            socket->flush();
            socket->disconnectFromHost();
            socket->waitForDisconnected(1000);
            break;
        }

        if (!socket->bytesAvailable() && !socket->waitForReadyRead(200)) {
            if (socket->state() == QAbstractSocket::UnconnectedState)
                break;
            continue;
        }
        buffer.append(socket->readAll());

        if (!headersDone) {
            const int headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0)
                continue;

            const QByteArray headerBlock = buffer.left(headerEnd);
            buffer.remove(0, headerEnd + 4);
            headersDone = true;

            const QList<QByteArray> lines = headerBlock.split('\n');
            if (lines.isEmpty())
                continue;
            const QList<QByteArray> statusParts = lines.at(0).trimmed().split(' ');
            if (statusParts.size() >= 2) {
                bool ok = false;
                status = statusParts.at(1).toInt(&ok);
                if (!ok)
                    status = -1;
            }
            for (int i = 1; i < lines.size(); ++i) {
                const QByteArray line = lines.at(i).trimmed();
                const int colon = line.indexOf(':');
                if (colon <= 0)
                    continue;
                headers.insert(QString::fromLatin1(line.left(colon)).toLower(),
                               QString::fromLatin1(line.mid(colon + 1)).trimmed());
            }
        }

        if (!headersDone)
            continue;

        // 100-continue (or any other informational answer) has no body: keep
        // waiting for the real response line instead of treating it as final.
        if (status >= 100 && status < 200) {
            headersDone = false;
            headers.clear();
            status = -1;
            continue;
        }

        const QString transferEncoding = headers.value(QStringLiteral("transfer-encoding")).toLower();
        if (transferEncoding.contains(QLatin1String("chunked"))) {
            bool complete = false;
            QByteArray decoded;
            if (!decodeChunked(buffer, &decoded, &complete)) {
                response->error = QStringLiteral("malformed chunked body");
                break;
            }
            payload = decoded;
            bodyComplete = complete;
        } else {
            bool ok = false;
            // A 204/304 has no body at all; a plain toInt() would also overflow
            // on anything above 2 GB.
            if (status == 204 || status == 304) {
                bodyComplete = true;
            } else {
                const qint64 length = headers.value(QStringLiteral("content-length")).toLongLong(&ok);
                if (!ok) {
                    // No length and no chunking: read until the peer closes.
                    if (socket->state() == QAbstractSocket::UnconnectedState) {
                        payload = buffer;
                        bodyComplete = true;
                    }
                } else if (buffer.size() >= length) {
                    payload = buffer.left(static_cast<int>(length));
                    bodyComplete = true;
                }
            }
        }
    }

    if (!bodyComplete && status > 0 && payload.isEmpty())
        response->error = QStringLiteral("response incomplete");

    response->status = status;
    response->body = payload;
    response->headers = headers;
    response->complete = bodyComplete;

    if (status < 0 && response->error.isEmpty())
        response->error = QStringLiteral("no HTTP response");
}

} // namespace

Response request(const QString &host,
                 quint16 port,
                 bool tls,
                 const QString &method,
                 const QString &pathAndQuery,
                 const QByteArray &body,
                 const QByteArray &contentType,
                 const crypto::Identity *identity,
                 bool sendClientCertificate,
                 int timeoutMs,
                 const std::function<bool()> &cancelled,
                 int maxAttempts)
{
    Response response;

    QSslSocket socket;
    if (!connectToPeer(&socket, host, port, tls, identity, sendClientCertificate, timeoutMs, &response,
                       cancelled, maxAttempts))
        return response;

    const QByteArray requestBytes = buildRequest(method, host, port, pathAndQuery, body, contentType);
    socket.write(requestBytes);
    if (!socket.waitForBytesWritten(timeoutMs)) {
        response.error = QStringLiteral("write failed: ") + socket.errorString();
        return response;
    }

    readResponse(&socket, timeoutMs, &response, cancelled);
    socket.disconnectFromHost();
    return response;
}

namespace {

QByteArray buildUploadHead(const QString &host,
                           quint16 port,
                           const QString &pathAndQuery,
                           qint64 size,
                           bool keepAlive,
                           const QByteArray &contentType)
{
    QByteArray header;
    header.append("POST ");
    header.append(pathAndQuery.toLatin1());
    header.append(" HTTP/1.1\r\nHost: ");
    header.append(host.toLatin1());
    header.append(':');
    header.append(QByteArray::number(port));
    header.append(keepAlive ? "\r\nConnection: keep-alive\r\n" : "\r\nConnection: close\r\n");
    header.append("User-Agent: LocalSend Aero/0.1\r\n");
    header.append("Content-Type: ");
    header.append(contentType.isEmpty() ? QByteArray("application/octet-stream") : contentType);
    header.append("\r\nContent-Length: ");
    header.append(QByteArray::number(size));
    header.append("\r\n\r\n");
    return header;
}

// Streams `size` bytes from `source` into the socket. Returns false when the
// body could not be written; `response` then either holds the peer's early
// answer (status > 0) or an error description.
bool writeStreamBody(QSslSocket *socket,
                     QIODevice *source,
                     qint64 size,
                     int timeoutMs,
                     const std::function<bool(qint64, qint64)> &progress,
                     const std::function<void()> &cancelAction,
                     const std::function<bool()> &cancelled,
                     Response *response)
{
    const int chunkSize = 64 * 1024;
    // A receiver that already gave up (bad save path, disk full) closes the
    // socket; do not wait the whole transfer timeout for that.
    const int writeTimeout = qMin(timeoutMs, 15000);
    QByteArray chunk;
    chunk.resize(chunkSize);
    qint64 sent = 0;
    bool locallyCancelled = false;

    while (sent < size) {
        const qint64 read = source->read(chunk.data(), chunkSize);
        if (read <= 0) {
            // The file vanished or shrank after prepare-upload: stop instead of
            // waiting for a response the peer can never send for a short body.
            response->error = QStringLiteral("source ended after %1 of %2 bytes")
                                  .arg(sent)
                                  .arg(size);
            return false;
        }

        qint64 written = 0;
        while (written < read) {
            const qint64 n = socket->write(chunk.constData() + written, read - written);
            if (n < 0) {
                response->error = QStringLiteral("peer closed: ") + socket->errorString();
                return false;
            }
            written += n;

            // Wait in slices so the cancel button reacts within ~0.5 s even
            // while a slow link is draining the socket buffer.
            int waited = 0;
            while (!socket->waitForBytesWritten(500)) {
                waited += 500;
                if (cancelled && cancelled()) {
                    if (cancelAction)
                        cancelAction();
                    response->error = QStringLiteral("cancelled");
                    return false;
                }
                if (socket->state() != QAbstractSocket::ConnectedState)
                    break;
                if (waited < writeTimeout)
                    continue;
                // Try to pick up an early error response before giving up.
                readResponse(socket, 2000, response);
                if (response->status <= 0) {
                    response->error = (socket->state() == QAbstractSocket::ConnectedState)
                            ? QStringLiteral("write timeout")
                            : QStringLiteral("peer closed while uploading");
                }
                return false;
            }

            if (socket->bytesAvailable() > 0) {
                // The peer answered before the body was complete: read the
                // response (usually an error) and stop writing.
                readResponse(socket, 3000, response);
                return false;
            }

            if (socket->state() != QAbstractSocket::ConnectedState) {
                // The receiver hung up mid-transfer (its user cancelled, or it
                // refused the file). Stop instead of pushing the rest of the
                // body into a dead socket.
                response->error = QStringLiteral("peer closed while uploading");
                return false;
            }
        }

        sent += read;
        if (progress && !progress(sent, size)) {
            locallyCancelled = true;
            break;
        }
    }

    if (locallyCancelled) {
        // Give the caller a chance to notify the peer (POST /v2/cancel) while
        // the connection is still usable; the reference client only shows
        // "sender cancelled" when it receives that request.
        if (cancelAction)
            cancelAction();
        response->error = QStringLiteral("cancelled");
        return false;
    }

    return true;
}

// Shared body of upload()/uploadData(): streams `size` bytes from `source`
// with a Content-Length header, then reads the response.
Response uploadStream(const QString &host,
                      quint16 port,
                      bool tls,
                      const QString &pathAndQuery,
                      QIODevice *source,
                      qint64 size,
                      const crypto::Identity *identity,
                      int timeoutMs,
                      const std::function<bool(qint64, qint64)> &progress,
                      const std::function<void()> &cancelAction,
                      const std::function<bool()> &cancelled = std::function<bool()>())
{
    Response response;

    QSslSocket socket;
    if (!connectToPeer(&socket, host, port, tls, identity, true, timeoutMs, &response, cancelled))
        return response;

    socket.write(buildUploadHead(host, port, pathAndQuery, size, false, QByteArray()));

    if (!writeStreamBody(&socket, source, size, timeoutMs, progress, cancelAction, cancelled,
                         &response)) {
        if (response.status <= 0) {
            socket.disconnectFromHost();
            socket.waitForDisconnected(1000);
        }
        return response;
    }

    socket.flush();
    readResponse(&socket, timeoutMs, &response);
    socket.disconnectFromHost();
    return response;
}

} // namespace

Response upload(const QString &host,
                quint16 port,
                bool tls,
                const QString &pathAndQuery,
                const QString &filePath,
                qint64 size,
                const crypto::Identity *identity,
                int timeoutMs,
                const std::function<bool(qint64, qint64)> &progress,
                const std::function<void()> &cancelAction,
                const std::function<bool()> &cancelled)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        Response response;
        response.error = QStringLiteral("cannot read ") + filePath;
        return response;
    }
    return uploadStream(host, port, tls, pathAndQuery, &file, size, identity, timeoutMs, progress,
                        cancelAction, cancelled);
}

Response uploadData(const QString &host,
                    quint16 port,
                    bool tls,
                    const QString &pathAndQuery,
                    const QByteArray &data,
                    const crypto::Identity *identity,
                    int timeoutMs,
                    const std::function<bool(qint64, qint64)> &progress,
                    const std::function<void()> &cancelAction,
                    const std::function<bool()> &cancelled)
{
    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly)) {
        Response response;
        response.error = QStringLiteral("cannot buffer text body");
        return response;
    }
    return uploadStream(host, port, tls, pathAndQuery, &buffer, data.size(), identity, timeoutMs,
                        progress, cancelAction, cancelled);
}

Session::Session()
    : m_port(0), m_tls(false), m_hasIdentity(false), m_socket(0)
{
}

Session::~Session()
{
    close();
}

void Session::dropSocket()
{
    if (!m_socket)
        return;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
        if (m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->waitForDisconnected(1000);
    }
    delete m_socket;
    m_socket = 0;
}

void Session::close()
{
    dropSocket();
}

bool Session::isOpen()
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

bool Session::open(const QString &host,
                   quint16 port,
                   bool tls,
                   const crypto::Identity *identity,
                   int timeoutMs,
                   const std::function<bool()> &cancelled,
                   Response *response)
{
    m_host = host;
    m_port = port;
    m_tls = tls;
    if (identity) {
        m_identity = *identity;
        m_hasIdentity = true;
    }

    dropSocket();
    m_socket = new QSslSocket;
    if (!connectToPeer(m_socket, m_host, m_port, m_tls, m_hasIdentity ? &m_identity : 0, true,
                       timeoutMs, response, cancelled)) {
        dropSocket();
        return false;
    }
    return true;
}

bool Session::ensure(int timeoutMs, const std::function<bool()> &cancelled, Response *response)
{
    if (isOpen())
        return true;

    dropSocket();
    m_socket = new QSslSocket;
    if (!connectToPeer(m_socket, m_host, m_port, m_tls, m_hasIdentity ? &m_identity : 0, true,
                       timeoutMs, response, cancelled)) {
        dropSocket();
        return false;
    }
    return true;
}

Response Session::post(const QString &pathAndQuery,
                       const QByteArray &body,
                       const QByteArray &contentType,
                       int timeoutMs,
                       const std::function<bool()> &cancelled)
{
    Response response;
    if (!ensure(timeoutMs, cancelled, &response))
        return response;

    QByteArray head;
    head.append("POST ");
    head.append(pathAndQuery.toLatin1());
    head.append(" HTTP/1.1\r\nHost: ");
    head.append(m_host.toLatin1());
    head.append(':');
    head.append(QByteArray::number(m_port));
    head.append("\r\nConnection: keep-alive\r\nUser-Agent: LocalSend Aero/0.1\r\n");
    head.append("Content-Type: ");
    head.append(contentType.isEmpty() ? QByteArray("application/json") : contentType);
    head.append("\r\nContent-Length: ");
    head.append(QByteArray::number(body.size()));
    head.append("\r\n\r\n");
    head.append(body);

    m_socket->write(head);
    if (!m_socket->waitForBytesWritten(timeoutMs)) {
        response.error = QStringLiteral("write failed: ") + m_socket->errorString();
        dropSocket();
        return response;
    }

    readResponse(m_socket, timeoutMs, &response, cancelled);
    if (!isOpen() || response.status <= 0 || !response.complete)
        dropSocket();
    else if (response.headers.value(QStringLiteral("connection")).compare(QStringLiteral("close"),
                                                                         Qt::CaseInsensitive) == 0)
        dropSocket();
    return response;
}

Response Session::postFile(const QString &pathAndQuery,
                           const QString &filePath,
                           qint64 size,
                           int timeoutMs,
                           const std::function<bool(qint64, qint64)> &progress,
                           const std::function<void()> &cancelAction,
                           const std::function<bool()> &cancelled)
{
    Response response;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        response.error = QStringLiteral("cannot read ") + filePath;
        return response;
    }

    if (!ensure(timeoutMs, cancelled, &response))
        return response;

    m_socket->write(buildUploadHead(m_host, m_port, pathAndQuery, size, true, QByteArray()));
    if (!writeStreamBody(m_socket, &file, size, timeoutMs, progress, cancelAction, cancelled,
                         &response)) {
        // The body may have been cut short while the peer already answered, so
        // this socket carries an unfinished request and can never be reused.
        dropSocket();
        return response;
    }

    m_socket->flush();
    readResponse(m_socket, timeoutMs, &response, cancelled);
    if (!isOpen() || response.status <= 0 || !response.complete)
        dropSocket();
    else if (response.headers.value(QStringLiteral("connection")).compare(QStringLiteral("close"),
                                                                         Qt::CaseInsensitive) == 0)
        dropSocket();
    return response;
}

} // namespace httpclient
