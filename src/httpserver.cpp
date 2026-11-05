#include "httpserver.h"

#include "logger.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QUuid>
#include <QSslCertificate>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpSocket>
#include <QUrl>

namespace {

bool decodeChunkedBody(const QByteArray &input, QByteArray *output, bool *complete, int *consumed = 0)
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

QByteArray statusText(int status)
{
    switch (status) {
    case 200: return QByteArray("OK");
    case 204: return QByteArray("No Content");
    case 400: return QByteArray("Bad Request");
    case 401: return QByteArray("Unauthorized");
    case 403: return QByteArray("Forbidden");
    case 404: return QByteArray("Not Found");
    case 405: return QByteArray("Method Not Allowed");
    case 409: return QByteArray("Conflict");
    case 412: return QByteArray("Precondition Failed");
    case 422: return QByteArray("Unprocessable Entity");
    case 429: return QByteArray("Too Many Requests");
    default: return QByteArray("Internal Server Error");
    }
}

QString plainAddress(const QString &address)
{
    if (address.startsWith(QLatin1String("::ffff:"), Qt::CaseInsensitive))
        return address.mid(7);
    return address;
}

// One peer supplied path component rewritten into something the local file
// system accepts, following the rules of the reference implementation
// (packages/core/src/util/filename.rs, Rules::Windows).
QString sanitizeComponent(const QString &part)
{
    QString result;
    result.reserve(part.size());
    for (int i = 0; i < part.size(); ++i) {
        const QChar c = part.at(i);
        const ushort u = c.unicode();
        const bool illegal = u < 0x20 || u == 0x7f
                || u == QLatin1Char('<') || u == QLatin1Char('>') || u == QLatin1Char(':')
                || u == QLatin1Char('"') || u == QLatin1Char('/') || u == QLatin1Char('\\')
                || u == QLatin1Char('|') || u == QLatin1Char('?') || u == QLatin1Char('*');
        result.append(illegal ? QLatin1Char('_') : c);
    }

    // A trailing run of dots or spaces is not legal on Windows: collapse it.
    int end = result.size();
    while (end > 0 && (result.at(end - 1) == QLatin1Char('.') || result.at(end - 1) == QLatin1Char(' ')))
        --end;
    if (end != result.size()) {
        result.truncate(end);
        result.append(QLatin1Char('_'));
    }

    // "CON", "NUL.txt", "LPT1" and friends keep their meaning with any
    // extension, so the stem decides.
    static const char *const kReserved[] = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
    };
    const int dot = result.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? result : result.left(dot)).toLower();
    for (size_t i = 0; i < sizeof(kReserved) / sizeof(kReserved[0]); ++i) {
        if (stem == QLatin1String(kReserved[i])) {
            result = QLatin1Char('_') + result.mid(stem.size());
            break;
        }
    }

    if (result.size() > 255)
        result.truncate(255);
    end = result.size();
    while (end > 0 && (result.at(end - 1) == QLatin1Char('.') || result.at(end - 1) == QLatin1Char(' ')))
        --end;
    if (end != result.size()) {
        result.truncate(end);
        result.append(QLatin1Char('_'));
    }

    if (result.isEmpty() || result == QLatin1String(".") || result == QLatin1String(".."))
        result = QStringLiteral("untitled");
    return result;
}

// Splits a peer supplied name into safe path components, keeping the directory
// structure a folder transfer sends as "folder/sub/file". Names that try to
// leave the target directory are refused outright, exactly like the reference
// receiver does ("Path traversal detected"), rather than rewritten.
bool sanitizeRelativeName(const QString &name, QStringList *components, QString *error)
{
    QString normalised = name;
    normalised.replace(QLatin1Char('\\'), QLatin1Char('/'));

    if (normalised.startsWith(QLatin1Char('/'))
            || (normalised.size() >= 2 && normalised.at(1) == QLatin1Char(':')
                    && normalised.at(0).isLetter())) {
        *error = QStringLiteral("Path traversal detected");
        return false;
    }

    const QStringList parts = normalised.split(QLatin1Char('/'), QString::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        const QString &part = parts.at(i);
        if (part == QLatin1String("..")) {
            *error = QStringLiteral("Path traversal detected");
            return false;
        }
        if (part == QLatin1String(".") || part.isEmpty())
            continue;
        components->append(sanitizeComponent(part));
    }

    if (components->isEmpty())
        components->append(QStringLiteral("untitled"));
    return true;
}

// Keeps an existing file and picks "name (2).ext", "name (3).ext", ... instead
// of overwriting it: LocalSend sends whatever name the sender picked, and two
// transfers with the same name are the normal case, not the exception.
QString uniquePath(const QString &directory, const QString &fileName)
{
    const QString safe = fileName;
    QString candidate = QDir(directory).filePath(safe);
    if (!QFile::exists(candidate))
        return candidate;

    const QFileInfo info(safe);
    const QString base = info.completeBaseName().isEmpty() ? safe : info.completeBaseName();
    const QString suffix = info.suffix();
    for (int i = 2; i < 10000; ++i) {
        const QString name = suffix.isEmpty()
                ? QStringLiteral("%1 (%2)").arg(base).arg(i)
                : QStringLiteral("%1 (%2).%3").arg(base).arg(i).arg(suffix);
        candidate = QDir(directory).filePath(name);
        if (!QFile::exists(candidate))
            return candidate;
    }
    return QDir(directory).filePath(safe);
}

// Incremental chunked body decoder. State lives between readyRead() calls so a
// multi megabyte file never has to sit in memory as one buffer.
struct ChunkStream
{
    ChunkStream() : state(0), remaining(0), written(0), done(false) {}

    int state;        // 0 = size line, 1 = data, 2 = CRLF after data, 3 = trailers, 4 = finished
    int remaining;    // bytes left in the current chunk
    qint64 written;
    bool done;
};

// Feeds everything decodable into sink; returns false on a malformed stream.
bool pumpChunked(QByteArray *buffer, QFile *sink, ChunkStream *stream)
{
    for (;;) {
        if (stream->state == 4)
            return true;

        if (stream->state == 0) {
            const int lineEnd = buffer->indexOf("\r\n");
            if (lineEnd < 0)
                return true;
            const QByteArray sizeLine = buffer->left(lineEnd).split(';').at(0).trimmed();
            buffer->remove(0, lineEnd + 2);

            bool ok = false;
            const int size = sizeLine.toInt(&ok, 16);
            if (!ok || size < 0)
                return false;

            if (size == 0) {
                // The last chunk is followed by an (usually empty) trailer
                // section. Leaving those two bytes in the buffer made the next
                // keep-alive request start with "\r\n", which parsed as an
                // empty request line and produced a 404 for the *next* file of
                // a folder transfer.
                stream->state = 3;
                continue;
            }
            stream->remaining = size;
            stream->state = 1;
            continue;
        }

        if (stream->state == 1) {
            if (buffer->isEmpty())
                return true;
            const int take = qMin(stream->remaining, buffer->size());
            const qint64 written = sink ? sink->write(buffer->constData(), take) : take;
            if (written != take)
                return false;
            stream->written += written;
            buffer->remove(0, take);
            stream->remaining -= take;
            if (stream->remaining == 0)
                stream->state = 2;
            continue;
        }

        if (stream->state == 2) {
            if (buffer->size() < 2)
                return true;
            if (!buffer->startsWith("\r\n"))
                return false;
            buffer->remove(0, 2);
            stream->state = 0;
            continue;
        }

        if (stream->state == 3) {
            const int lineEnd = buffer->indexOf("\r\n");
            if (lineEnd < 0)
                return true;
            const QByteArray trailer = buffer->left(lineEnd);
            buffer->remove(0, lineEnd + 2);
            if (trailer.isEmpty()) {
                stream->state = 4;
                stream->done = true;
                return true;
            }
            continue;
        }
    }
}

} // namespace

// One client connection: parses requests incrementally (keep-alive aware) and
// answers them through the owning HttpServer.
class HttpConnection : public QObject
{
public:
    HttpConnection(qintptr handle, bool tls, const crypto::Identity &identity, HttpServer *server)
        : QObject(server)
        , m_server(server)
        , m_socket(0)
        , m_tls(tls)
        , m_haveHeaders(false)
        , m_contentLength(-1)
        , m_chunked(false)
        , m_awaitingAnswer(false)
        , m_pendingRequestId(0)
        , m_sink(0)
        , m_uploadExpected(0)
        , m_uploadReceived(0)
        , m_uploadKeepAlive(true)
        , m_uploadError(false)
        , m_discardBody(false)
    {
        Q_UNUSED(identity);

        if (tls) {
            QSslSocket *ssl = new QSslSocket(this);
            ssl->setSocketDescriptor(handle);
            QSslConfiguration configuration = ssl->sslConfiguration();
            configuration.setLocalCertificate(server->m_identity.certificate);
            configuration.setPrivateKey(server->m_identity.privateKey);
            // QueryPeer asks for a client certificate without rejecting
            // self-signed ones; the API layer then pins the fingerprint.
            configuration.setPeerVerifyMode(QSslSocket::QueryPeer);
            ssl->setSslConfiguration(configuration);
            // Two members share the name: the getter and the signal.
            connect(ssl,
                    static_cast<void (QSslSocket::*)(const QList<QSslError> &)>(&QSslSocket::sslErrors),
                    this, [ssl](const QList<QSslError> &) { ssl->ignoreSslErrors(); });
            connect(ssl, &QSslSocket::encrypted, this, [this]() { onEncrypted(); });
            connect(ssl, &QSslSocket::readyRead, this, [this]() { onReadyRead(); });
            connect(ssl, &QSslSocket::disconnected, this, &HttpConnection::onDisconnected);
            m_socket = ssl;
            ssl->startServerEncryption();
        } else {
            QTcpSocket *plain = new QTcpSocket(this);
            plain->setSocketDescriptor(handle);
            connect(plain, &QTcpSocket::readyRead, this, [this]() { onReadyRead(); });
            connect(plain, &QTcpSocket::disconnected, this, &HttpConnection::onDisconnected);
            m_socket = plain;
            m_peerAddress = plainAddress(plain->peerAddress().toString());
        }

        server->m_connections.insert(this);
    }

    ~HttpConnection()
    {
        // A half-written upload must not survive the connection: the partial
        // file would stay behind and the UI would wait for a completion that
        // can never come.
        abortUpload();
        if (m_socket)
            m_socket->abort();
        if (!m_server->m_shuttingDown)
            m_server->m_connections.remove(this);
    }

    // Used by "cancel receiving": tell the sender why, drop the partial file and
    // hang up. Without the response the sender could only report a generic
    // "peer closed the connection".
    void abortActiveUpload()
    {
        if (m_sink) {
            sendResponse(400, QByteArray("receiver-cancelled"), false, QByteArray("text/plain"));
            abortUpload();
        }
        if (m_socket) {
            m_socket->flush();
            m_socket->disconnectFromHost();
        }
    }

    int requestId() const { return m_pendingRequestId; }

    // Called by HttpServer once the UI accepted (or rejected) a prepare-upload.
    // The save directory travels in the session, not in the answer.
    void deliverPrepareAnswer(const QMap<QString, QString> &tokens, const QString &saveDir,
                              const QString &sessionId)
    {
        Q_UNUSED(saveDir)
        m_awaitingAnswer = false;
        m_pendingRequestId = 0;

        if (tokens.isEmpty()) {
            // 204 tells the sender "everything was declined", which is a
            // documented, non-error outcome.
            sendResponse(204, QByteArray(), true);
        } else {
            const QJsonObject object = protocol::prepareUploadResponse(sessionId, tokens);
            sendResponse(200, QJsonDocument(object).toJson(QJsonDocument::Compact), true);
        }

        if (m_socket)
            processBuffer();
    }

    void disconnectNow()
    {
        if (m_socket)
            m_socket->disconnectFromHost();
    }

private:
    void onEncrypted()
    {
        QSslSocket *ssl = qobject_cast<QSslSocket *>(m_socket);
        if (ssl) {
            m_peerAddress = plainAddress(ssl->peerAddress().toString());
            m_peerFingerprint = crypto::fingerprintOf(ssl->peerCertificate());
            logger::write(QStringLiteral("server: TLS established peer=%1 cipher=%2 cert=%3")
                              .arg(m_peerAddress, ssl->sessionCipher().name(),
                                   m_peerFingerprint.isEmpty() ? QStringLiteral("none") : m_peerFingerprint));
        }
        onReadyRead();
    }

    void onDisconnected()
    {
        // A peer that closes while its confirmation dialog is open must not
        // leave a dangling connection behind: answering that request later
        // would write into a deleted object.
        if (m_pendingRequestId > 0)
            m_server->dropPendingRequest(m_pendingRequestId);
        if (m_sink) {
            // The sender hung up mid-file: drop the partial file and let the UI
            // leave the "receiving" state instead of waiting forever.
            const QString fileId = m_uploadFileId;
            abortUpload();
            emit m_server->uploadFinished(fileId, QString(), tr("发送方中断了传输"));
        }
        deleteLater();
    }

    void onReadyRead()
    {
        if (!m_socket)
            return;
        QSslSocket *ssl = qobject_cast<QSslSocket *>(m_socket);
        if (ssl && !ssl->isEncrypted())
            return;

        m_buffer.append(m_socket->readAll());
        // Anything without a complete header line is not a client we want:
        // an unauthenticated sender could otherwise grow this buffer forever.
        if (!m_haveHeaders && m_buffer.size() > 64 * 1024) {
            logger::write(QStringLiteral("server: header too large, dropping %1").arg(m_peerAddress));
            sendResponse(431, QByteArray("Request header too large"), false, QByteArray("text/plain"));
            m_buffer.clear();
            m_socket->disconnectFromHost();
            return;
        }
        processBuffer();
    }

    void sendResponse(int status, const QByteArray &body, bool keepAlive, const QByteArray &contentType = QByteArray("application/json"))
    {
        if (!m_socket)
            return;

        QByteArray header;
        header.append("HTTP/1.1 ");
        header.append(QByteArray::number(status));
        header.append(' ');
        header.append(statusText(status));
        header.append("\r\n");
        if (!body.isEmpty()) {
            header.append("Content-Type: ");
            header.append(contentType);
            header.append("\r\n");
        }
        header.append("Content-Length: ");
        header.append(QByteArray::number(body.size()));
        header.append("\r\n");
        header.append(keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
        header.append("\r\n");

        m_socket->write(header);
        if (!body.isEmpty())
            m_socket->write(body);
        m_socket->flush();
    }

    void abortUpload()
    {
        if (!m_sink)
            return;
        m_sink->close();
        m_sink->remove();
        delete m_sink;
        m_sink = 0;
    }

    // Every rejected upload has to reach the UI as well: without the
    // uploadFinished signal the receive page keeps saying "正在接收" forever
    // (the peer already got its 403/500 and gave up).
    void rejectUpload(int status, const QString &reason,
                      const QByteArray &body = QByteArray())
    {
        logger::write(QStringLiteral("upload: rejected status=%1 reason=%2 file=%3")
                          .arg(status)
                          .arg(reason, m_uploadFileId));
        // The answer is normally empty, but "this file is already here" needs a
        // machine readable marker: after a lost response the sender retries and
        // must be able to tell that case apart from a real rejection.
        sendResponse(status, body, false, body.isEmpty() ? QByteArray("application/json")
                                                         : QByteArray("text/plain"));
        m_uploadError = true;
        emit m_server->uploadFinished(m_uploadFileId, QString(), reason);
    }

    void processBuffer()
    {
        for (;;) {
            if (m_awaitingAnswer)
                return;

            if (!m_haveHeaders) {
                const int headerEnd = m_buffer.indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;

                const QByteArray headerBlock = m_buffer.left(headerEnd);
                m_buffer.remove(0, headerEnd + 4);
                parseHeaders(headerBlock);

                const bool upload = m_path == QLatin1String("/api/localsend/v2/upload")
                                    && m_method == QLatin1String("POST");
                if (upload)
                    beginUpload();

                if (m_uploadError) {
                    // The rejection reached the peer already, but it is still
                    // streaming the body. Closing the socket now makes rustls
                    // based peers report "IncompleteMessage" instead of the real
                    // error, so swallow the rest of the body first and hang up
                    // once the request is complete.
                    m_discardBody = true;
                    if (!m_chunked && m_contentLength <= 0) {
                        if (m_socket)
                            m_socket->disconnectFromHost();
                        return;
                    }
                }

                if (!m_discardBody
                    && m_headers.value(QStringLiteral("expect")).toLower().contains(QLatin1String("100-continue"))) {
                    m_socket->write("HTTP/1.1 100 Continue\r\n\r\n");
                    m_socket->flush();
                }
            }

            if (m_discardBody) {
                if (m_chunked) {
                    const bool ok = pumpChunked(&m_buffer, 0, &m_chunk);
                    if (!ok || m_chunk.done) {
                        if (m_socket)
                            m_socket->disconnectFromHost();
                    }
                    return;
                }
                const qint64 left = m_contentLength - m_uploadReceived;
                const qint64 take = qMin<qint64>(left, m_buffer.size());
                if (take > 0) {
                    m_buffer.remove(0, static_cast<int>(take));
                    m_uploadReceived += take;
                }
                if (m_uploadReceived >= m_contentLength) {
                    if (m_socket)
                        m_socket->disconnectFromHost();
                }
                return;
            }

            if (m_sink) {
                // Streaming file upload: consume whatever arrived, answer once
                // the body is complete.
                bool ok = true;
                if (m_chunked) {
                    ok = pumpChunked(&m_buffer, m_sink, &m_chunk);
                    m_uploadReceived = m_chunk.written;
                    if (!ok || m_chunk.done) {
                        finishUpload(ok);
                        continue;
                    }
                } else {
                    const qint64 left = m_uploadExpected - m_uploadReceived;
                    const qint64 take = qMin<qint64>(left, m_buffer.size());
                    if (take > 0) {
                        const qint64 written = m_sink->write(m_buffer.constData(), take);
                        if (written != take) {
                            finishUpload(false);
                            continue;
                        }
                        m_uploadReceived += written;
                        m_buffer.remove(0, static_cast<int>(take));
                    }
                    if (m_uploadReceived >= m_uploadExpected) {
                        finishUpload(true);
                        continue;
                    }
                }

                if (!m_uploadFileId.isEmpty())
                    emit m_server->uploadProgress(m_uploadFileId, m_uploadReceived, m_uploadExpected);
                return;
            }

            QByteArray body;
            if (m_chunked) {
                QByteArray decoded;
                bool complete = false;
                int consumed = 0;
                if (!decodeChunkedBody(m_buffer, &decoded, &complete, &consumed)) {
                    sendResponse(400, QByteArray(), false);
                    m_socket->disconnectFromHost();
                    return;
                }
                if (!complete)
                    return;
                m_buffer.remove(0, consumed);
                body = decoded;
            } else if (m_contentLength > 0) {
                if (m_buffer.size() < m_contentLength)
                    return;
                body = m_buffer.left(m_contentLength);
                m_buffer.remove(0, m_contentLength);
            }

            const bool keepAlive =
                m_headers.value(QStringLiteral("connection")).toLower() != QLatin1String("close");
            handle(m_method, m_path, body, keepAlive);

            m_haveHeaders = false;
            m_method.clear();
            m_path.clear();
            m_query.clear();
            m_headers.clear();
            m_contentLength = -1;
            m_chunked = false;
            m_discardBody = false;
            m_uploadError = false;

            if (m_awaitingAnswer)
                return;

            if (!keepAlive) {
                m_socket->disconnectFromHost();
                return;
            }
        }
    }

    void parseHeaders(const QByteArray &block)
    {
        const QList<QByteArray> lines = block.split('\n');
        if (!lines.isEmpty()) {
            const QList<QByteArray> requestLine = lines.at(0).trimmed().split(' ');
            if (requestLine.size() >= 2) {
                m_method = QString::fromLatin1(requestLine.at(0));
                const QString target = QString::fromLatin1(requestLine.at(1));
                const int queryStart = target.indexOf(QLatin1Char('?'));
                m_path = queryStart < 0 ? target : target.left(queryStart);
                m_query = queryStart < 0 ? QString() : target.mid(queryStart + 1);
            }
        }

        m_headers.clear();
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            m_headers.insert(QString::fromLatin1(line.left(colon)).toLower(),
                             QString::fromLatin1(line.mid(colon + 1)).trimmed());
        }

        m_chunked = m_headers.value(QStringLiteral("transfer-encoding")).toLower().contains(QLatin1String("chunked"));
        bool ok = false;
        m_contentLength = m_headers.value(QStringLiteral("content-length")).toInt(&ok);
        if (!ok)
            m_contentLength = -1;
        m_haveHeaders = true;
    }

    QString queryValue(const QString &key) const
    {
        const QStringList pairs = m_query.split(QLatin1Char('&'), QString::SkipEmptyParts);
        for (int i = 0; i < pairs.size(); ++i) {
            const QString &pair = pairs.at(i);
            const int equals = pair.indexOf(QLatin1Char('='));
            if (equals <= 0)
                continue;
            if (pair.left(equals) == key)
                return QUrl::fromPercentEncoding(pair.mid(equals + 1).toUtf8());
        }
        return QString();
    }

    void beginUpload()
    {
        m_uploadError = false;
        m_uploadReceived = 0;
        m_uploadFileId = queryValue(QStringLiteral("fileId"));
        const QString sessionId = queryValue(QStringLiteral("sessionId"));
        const QString token = queryValue(QStringLiteral("token"));

        const HttpServer::Session session = m_server->m_sessions.value(sessionId);
        if (session.sessionId.isEmpty()) {
            rejectUpload(403, tr("会话已失效"));
            return;
        }
        if (session.tokens.value(m_uploadFileId) != token || token.isEmpty()) {
            rejectUpload(403, tr("传输凭据无效"));
            return;
        }
        if (session.sender.compare(m_peerAddress, Qt::CaseInsensitive) != 0) {
            rejectUpload(403, tr("传输来源与会话不符"));
            return;
        }
        if (session.completed.contains(m_uploadFileId)) {
            rejectUpload(403, tr("该文件已接收过"), QByteArray("file-already-received"));
            return;
        }

        const protocol::FileDto file = session.files.value(m_uploadFileId);
        m_uploadDto = file;
        m_uploadSessionId = sessionId;
        m_uploadSaveDir = session.saveDir;
        m_uploadExpected = m_chunked ? file.size : m_contentLength;

        // A declared size of 0 with data behind it used to divide by zero in the
        // progress bar, and a truncated body was accepted as a complete file.
        if (file.size < 0) {
            rejectUpload(422, "Invalid file size");
            return;
        }
        if (file.size > 0 && !m_chunked && m_contentLength != file.size) {
            logger::write(QStringLiteral("upload: refused file=%1 content-length=%2 size=%3")
                              .arg(file.fileName).arg(m_contentLength).arg(file.size));
            rejectUpload(422, "Content-Length does not match the announced size");
            return;
        }

        QDir().mkpath(session.saveDir);
        QStringList components;
        QString pathError;
        if (!sanitizeRelativeName(file.fileName, &components, &pathError)) {
            logger::write(QStringLiteral("upload: refused name=%1 (%2)").arg(file.fileName, pathError));
            rejectUpload(500, pathError.toUtf8());
            return;
        }
        const QString targetName = components.takeLast();
        const QString targetDir = components.isEmpty()
                ? session.saveDir
                : QDir(session.saveDir).filePath(components.join(QLatin1Char('/')));
        QDir().mkpath(targetDir);
        m_uploadDir = targetDir;
        m_uploadName = targetName;
        const QString target = QDir(targetDir).filePath(
                QStringLiteral(".lsaero-%1.part")
                        .arg(QUuid::createUuid().toString().mid(1, 8)));
        m_sink = new QFile(target);
        if (!m_sink->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            logger::write(QStringLiteral("upload: cannot write %1: %2").arg(target, m_sink->errorString()));
            delete m_sink;
            m_sink = 0;
            rejectUpload(500, tr("无法写入保存位置：%1").arg(QDir::toNativeSeparators(session.saveDir)));
            return;
        }
        m_uploadPartPath = target;
        m_uploadKeepAlive = m_headers.value(QStringLiteral("connection")).toLower() != QLatin1String("close");
        logger::write(QStringLiteral("upload: start file=%1 expected=%2 dir=%3")
                          .arg(file.fileName).arg(m_uploadExpected).arg(session.saveDir));
    }

    void finishUpload(bool bodyOk)
    {
        QFile *sink = m_sink;
        m_sink = 0;
        if (!sink)
            return;

        sink->close();
        const QString partPath = m_uploadPartPath;
        const protocol::FileDto file = m_uploadDto;
        const bool keepAlive = m_uploadKeepAlive;

        QString error;
        QString finalPath;
        if (!bodyOk) {
            error = QStringLiteral("malformed body");
        } else if (file.size > 0 && m_uploadReceived != file.size) {
            // Never hand a short file over as if it were complete: the only
            // defence left would be the optional SHA-256 of the sender.
            error = QStringLiteral("Incomplete upload");
            logger::write(QStringLiteral("upload: truncated file=%1 received=%2 expected=%3")
                              .arg(file.fileName).arg(m_uploadReceived).arg(file.size));
        } else {
            bool checksumOk = true;
            if (!file.sha256.isEmpty()) {
                QFile done(partPath);
                if (done.open(QIODevice::ReadOnly)) {
                    QCryptographicHash hash(QCryptographicHash::Sha256);
                    if (hash.addData(&done)) {
                        const QString actual = QString::fromLatin1(hash.result().toHex()).toUpper();
                        checksumOk = actual.compare(file.sha256.trimmed(), Qt::CaseInsensitive) == 0;
                        if (!checksumOk)
                            error = QStringLiteral("Checksum mismatch");
                    }
                }
            }
            if (checksumOk && error.isEmpty()) {
                finalPath = uniquePath(m_uploadDir, m_uploadName);
                if (!QFile::rename(partPath, finalPath))
                    error = QStringLiteral("cannot save file");
                else
                    m_server->markFileReceived(m_uploadSessionId, file.id, finalPath);
            }
        }

        delete sink;

        // When the body was broken the client is still streaming the rest of it,
        // so this connection can never be reused: its remaining bytes would be
        // parsed as a new request.
        const bool reuse = keepAlive && bodyOk;
        if (!error.isEmpty()) {
            QFile::remove(partPath);
            logger::write(QStringLiteral("upload: failed file=%1 error=%2").arg(file.fileName, error));
            const int status = (error == QLatin1String("Checksum mismatch")
                                || error == QLatin1String("Incomplete upload")) ? 422 : 500;
            sendResponse(status, error.toUtf8(), reuse, QByteArray("text/plain"));
        } else {
            logger::write(QStringLiteral("upload: saved %1 (%2 bytes)").arg(finalPath).arg(m_uploadReceived));
            sendResponse(200, QByteArray(), reuse);
        }

        if (!reuse && m_socket)
            m_socket->disconnectFromHost();

        m_haveHeaders = false;
        m_method.clear();
        m_path.clear();
        m_query.clear();
        m_headers.clear();
        m_contentLength = -1;
        m_chunked = false;
        m_chunk = ChunkStream();
        m_uploadFileId.clear();
        m_uploadReceived = 0;
        m_uploadExpected = 0;
        m_uploadSaveDir.clear();
        m_uploadDir.clear();
        m_uploadName.clear();
        m_uploadPartPath.clear();
        m_uploadDto = protocol::FileDto();
    }

    void handle(const QString &method, const QString &path, const QByteArray &body, bool keepAlive)
    {
        logger::write(QStringLiteral("server %1 %2 peer=%3 tls=%4 cert=%5")
                          .arg(method, path, m_peerAddress,
                               m_tls ? QStringLiteral("yes") : QStringLiteral("no"),
                               m_peerFingerprint.isEmpty() ? QStringLiteral("none") : m_peerFingerprint));

        if (path == QLatin1String("/api/localsend/v2/info") && method == QLatin1String("GET")) {
            const QJsonObject object = protocol::deviceToJson(m_server->m_self, false);
            sendResponse(200, QJsonDocument(object).toJson(QJsonDocument::Compact), keepAlive);
            return;
        }

        if (path == QLatin1String("/api/localsend/v2/register") && method == QLatin1String("POST")) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                sendResponse(400, QByteArray(), keepAlive);
                return;
            }

            const protocol::DeviceInfo device = protocol::deviceFromJson(document.object());
            if (!device.valid) {
                sendResponse(400, QByteArray(), keepAlive);
                return;
            }

            // A device that reports our own fingerprint *and* our own port is
            // this very instance talking to itself (a looped announcement).
            // Two copies sharing one certs\ folder also share a fingerprint, but
            // they listen on different ports, so they are accepted here.
            if (!m_server->m_self.fingerprint.isEmpty()
                && device.fingerprint.compare(m_server->m_self.fingerprint, Qt::CaseInsensitive) == 0
                && device.port == m_server->m_self.port) {
                sendResponse(412, QByteArray(), keepAlive);
                return;
            }

            if (m_tls && !m_peerFingerprint.isEmpty()
                && device.fingerprint.compare(m_peerFingerprint, Qt::CaseInsensitive) != 0) {
                logger::write(QStringLiteral("server register: certificate/fingerprint mismatch"));
                sendResponse(403, QByteArray(), keepAlive);
                return;
            }

            emit m_server->deviceRegistered(device, m_peerAddress);

            const QJsonObject object = protocol::deviceToJson(m_server->m_self, false);
            sendResponse(200, QJsonDocument(object).toJson(QJsonDocument::Compact), keepAlive);
            return;
        }

        if (path == QLatin1String("/api/localsend/v2/prepare-upload") && method == QLatin1String("POST")) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                sendResponse(400, QByteArray(), keepAlive);
                return;
            }

            const QJsonObject object = document.object();
            protocol::PrepareUpload request;
            request.info = protocol::deviceFromJson(object.value(QStringLiteral("info")).toObject());
            request.files = protocol::filesFromJson(object.value(QStringLiteral("files")).toObject());
            if (!request.info.valid || request.files.isEmpty()) {
                // Empty file list is a client bug; the reference server answers 400.
                sendResponse(400, QByteArray(), keepAlive);
                return;
            }

            // TLS pins the sender identity: the announced fingerprint must be
            // the one in the client certificate.
            if (m_tls && !m_peerFingerprint.isEmpty()
                && request.info.fingerprint.compare(m_peerFingerprint, Qt::CaseInsensitive) != 0) {
                sendResponse(403, QByteArray(), keepAlive);
                return;
            }

            // Optional PIN, matching the reference behaviour: three bad attempts
            // per address lock the sender out with 429 until it succeeds.
            const int pinResult = m_server->checkPin(m_peerAddress, queryValue(QStringLiteral("pin")));
            if (pinResult != 0) {
                logger::write(QStringLiteral("server prepare-upload: pin rejected (%1) for %2")
                                  .arg(pinResult)
                                  .arg(m_peerAddress));
                const QByteArray reason = pinResult == 429
                                              ? QByteArray("Too many requests")
                                              : (queryValue(QStringLiteral("pin")).isEmpty()
                                                     ? QByteArray("PIN required")
                                                     : QByteArray("Invalid PIN"));
                sendResponse(pinResult, reason, keepAlive, QByteArray("text/plain"));
                return;
            }

            m_pendingRequestId = m_server->beginPrepareUpload(request, m_peerAddress, this);
            m_awaitingAnswer = true;
            logger::write(QStringLiteral("server prepare-upload: %1 file(s) from %2, waiting for user")
                              .arg(request.files.size())
                              .arg(request.info.alias));
            return;
        }

        if (path == QLatin1String("/api/localsend/v2/cancel") && method == QLatin1String("POST")) {
            const QString cancelSession = queryValue(QStringLiteral("sessionId"));
            m_server->cancelSessionsFrom(m_peerAddress);
            // The receiver cancels a transfer by POSTing this to the *sender*:
            // without forwarding it the upload would run to completion while the
            // other side already threw the data away.
            emit m_server->cancelRequested(cancelSession, m_peerAddress);
            sendResponse(200, QByteArray(), keepAlive);
            return;
        }

        sendResponse(404, QByteArray(), keepAlive);
    }

private:
    HttpServer *m_server;
    QAbstractSocket *m_socket;
    bool m_tls;
    bool m_haveHeaders;
    QString m_method;
    QString m_path;
    QString m_query;
    QMap<QString, QString> m_headers;
    int m_contentLength;
    bool m_chunked;
    QByteArray m_buffer;
    QString m_peerFingerprint;
    QString m_peerAddress;

    bool m_awaitingAnswer;
    int m_pendingRequestId;

    QFile *m_sink;
    ChunkStream m_chunk;
    qint64 m_uploadExpected;
    qint64 m_uploadReceived;
    QString m_uploadFileId;
    QString m_uploadSaveDir;
    QString m_uploadDir;
    QString m_uploadName;
    QString m_uploadPartPath;
    QString m_uploadSessionId;
    protocol::FileDto m_uploadDto;
    bool m_uploadKeepAlive;
    bool m_uploadError;
    bool m_discardBody;
};

HttpServer::HttpServer(const crypto::Identity &identity, QObject *parent)
    : QTcpServer(parent)
    , m_identity(identity)
    , m_tls(true)
    , m_requestCounter(0)
    , m_shuttingDown(false)
{
}

HttpServer::~HttpServer()
{
    // The connections are QObject children, so they would be deleted by
    // ~QObject *after* m_connections is gone; their destructor would then touch
    // freed memory. Delete them here while the members are still alive.
    m_shuttingDown = true;
    const QList<HttpConnection *> connections = m_connections.toList();
    m_connections.clear();
    for (int i = 0; i < connections.size(); ++i)
        delete connections.at(i);
}

void HttpServer::setSelf(const protocol::DeviceInfo &self)
{
    m_self = self;
}

bool HttpServer::start(quint16 port, bool useTls, QString *error)
{
    m_tls = useTls;
    if (!listen(QHostAddress::Any, port)) {
        if (error)
            *error = errorString();
        return false;
    }
    logger::write(QStringLiteral("server listening on %1 (%2)")
                      .arg(port)
                      .arg(useTls ? QStringLiteral("https") : QStringLiteral("http")));
    return true;
}

void HttpServer::stop()
{
    close();
}

void HttpServer::incomingConnection(qintptr handle)
{
    new HttpConnection(handle, m_tls, m_identity, this);
}

int HttpServer::beginPrepareUpload(const protocol::PrepareUpload &request, const QString &address,
                                   HttpConnection *connection)
{
    const int id = nextRequestId();
    m_pendingConnections.insert(id, connection);

    PendingUpload pending;
    pending.requestId = id;
    pending.info = request.info;
    pending.files = request.files;
    pending.address = address;
    m_pendingRequests.insert(id, pending);

    // Remember the announced sender fingerprint for the session that will be
    // created when the user answers.
    emit prepareUploadIncoming(pending);
    return id;
}

void HttpServer::dropPendingRequest(int requestId)
{
    // Only bookkeeping: the connection is already gone, the UI just has to stop
    // waiting for an answer it will never send.
    if (m_pendingConnections.remove(requestId) == 0 && !m_pendingRequests.contains(requestId))
        return;
    const PendingUpload pending = m_pendingRequests.take(requestId);
    if (!pending.files.isEmpty())
        emit prepareUploadDropped(requestId);
    logger::write(QStringLiteral("server prepare-upload %1 dropped (peer closed)").arg(requestId));
}

void HttpServer::answerPrepareUpload(int requestId, const QMap<QString, QString> &tokens,
                                     const QString &saveDir, const QString &sessionId)
{
    // The connection may have died while the user was looking at the dialog
    // (peer hit "cancel"). Answering a stale request would touch freed memory,
    // which is exactly the crash the current build has to avoid.
    const QPointer<HttpConnection> connection = m_pendingConnections.take(requestId);
    if (connection.isNull()) {
        m_pendingRequests.remove(requestId);
        return;
    }

    const PendingUpload pending = m_pendingRequests.take(requestId);

    if (!pending.files.isEmpty() && !tokens.isEmpty()) {
        Session session;
        session.sessionId = sessionId;
        session.tokens = tokens;
        session.files = pending.files;
        session.saveDir = saveDir;
        session.sender = pending.address;
        session.senderInfo = pending.info;
        m_sessions.insert(sessionId, session);
    }

    connection->deliverPrepareAnswer(tokens, saveDir, sessionId);
}

void HttpServer::markFileReceived(const QString &sessionId, const QString &fileId, const QString &path)
{
    Session session = m_sessions.value(sessionId);
    if (session.sessionId.isEmpty())
        return;
    session.completed.insert(fileId);
    m_sessions.insert(sessionId, session);

    emit uploadFinished(fileId, path, QString());
}

void HttpServer::cancelIncoming()
{
    QString sender;
    // Tell every sender whose transfer we are dropping: closing the socket on
    // its own only makes the *next* upload fail, and the peer keeps going
    // through the whole file list. Reference semantics: POST /v2/cancel with
    // the session id, addresed to the sender.
    QList<QPair<QString, Session> > cancelledSessions;
    for (QMap<QString, Session>::const_iterator it = m_sessions.constBegin();
         it != m_sessions.constEnd(); ++it) {
        if (sender.isEmpty())
            sender = it.value().sender;
        cancelledSessions.append(qMakePair(it.key(), it.value()));
    }

    // Pending offers still waiting for a user decision must tell the UI to
    // close its dialog before the connections go away.
    const QList<int> pendingIds = m_pendingRequests.keys();
    for (int i = 0; i < pendingIds.size(); ++i)
        emit prepareUploadDropped(pendingIds.at(i));

    m_sessions.clear();
    m_pendingRequests.clear();
    m_pendingConnections.clear();

    // Copy: aborting may delete connections from the set.
    const QList<HttpConnection *> connections = m_connections.toList();
    for (int i = 0; i < connections.size(); ++i)
        connections.at(i)->abortActiveUpload();

    logger::write(QStringLiteral("receive: cancelled by user (%1)").arg(sender));
    if (!sender.isEmpty())
        emit sessionCancelled(sender);

    // Reach the sender's HTTP endpoint ourselves; the socket we just aborted is
    // the one it is uploading on, so this needs a fresh connection.
    for (int i = 0; i < cancelledSessions.size(); ++i) {
        const Session &session = cancelledSessions.at(i).second;
        if (session.sender.isEmpty() || session.sessionId.isEmpty())
            continue;
        const bool senderTls = session.senderInfo.protocol.compare(QLatin1String("https"),
                                                                  Qt::CaseInsensitive) == 0;
        const quint16 senderPort = session.senderInfo.port > 0
                ? static_cast<quint16>(session.senderInfo.port)
                : protocol::kDefaultPort;
        emit incomingCancelRequested(session.sender, senderPort, senderTls, session.sessionId);
    }
}

QString HttpServer::sessionSender() const
{
    for (QMap<QString, Session>::const_iterator it = m_sessions.constBegin();
         it != m_sessions.constEnd(); ++it) {
        return it.value().sender;
    }
    return QString();
}

void HttpServer::cancelSessionsFrom(const QString &address)
{
    QList<QString> toRemove;
    for (QMap<QString, Session>::const_iterator it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
        if (it.value().sender.compare(address, Qt::CaseInsensitive) == 0)
            toRemove.append(it.key());
    }
    for (int i = 0; i < toRemove.size(); ++i)
        m_sessions.remove(toRemove.at(i));

    if (!toRemove.isEmpty())
        emit sessionCancelled(address);
}

int HttpServer::checkPin(const QString &address, const QString &provided)
{
    if (m_pin.isEmpty())
        return 0;

    // Same rules as the reference implementation (packages/core pin.rs): a
    // missing PIN is rejected without counting as an attempt, a wrong PIN
    // increments the per-address counter, three failures block with 429 and a
    // correct PIN resets the counter.
    const int failures = m_pinAttempts.value(address, 0);
    if (failures >= 3)
        return 429;

    if (provided.isEmpty())
        return 401;

    if (provided == m_pin) {
        m_pinAttempts.remove(address);
        return 0;
    }

    m_pinAttempts.insert(address, failures + 1);
    return 401;
}
