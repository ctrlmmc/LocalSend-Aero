#include "sendworker.h"

#include "httpclient.h"
#include "logger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>

SendWorker::SendWorker(const Job &job, QObject *parent)
    : QObject(parent)
    , m_job(job)
{
    m_cancel.storeRelease(0);
    m_remoteCancel.storeRelease(0);
}

void SendWorker::requestCancel(bool remote)
{
    if (remote)
        m_remoteCancel.storeRelease(1);
    m_cancel.storeRelease(1);
}

void SendWorker::run()
{
    QMap<QString, protocol::FileDto> fileMap;
    for (int i = 0; i < m_job.files.size(); ++i)
        fileMap.insert(m_job.files.at(i).id, m_job.files.at(i));

    const QByteArray prepareBody =
            QJsonDocument(protocol::prepareUploadRequest(m_job.self, fileMap))
                    .toJson(QJsonDocument::Compact);
    QString preparePath = QStringLiteral("/api/localsend/v2/prepare-upload");
    if (!m_job.pin.isEmpty())
        preparePath += QStringLiteral("?pin=") + m_job.pin;

    emit stage(tr("等待 %1 确认接收…").arg(m_job.targetAlias));

    // A text message is already visible on the peer as soon as the request is
    // parsed, so waiting the full file timeout would only delay the result.
    const int prepareTimeout = m_job.textOnly ? 12000 : 30000;
    const httpclient::Response prepare = httpclient::request(
        m_job.address, m_job.port, m_job.tls, QStringLiteral("POST"), preparePath, prepareBody,
        QByteArray("application/json"), &m_job.identity, true, prepareTimeout,
        [this]() { return isCancelled(); });

    logger::write(QStringLiteral("send: prepare -> %1 status=%2 error=%3 body=%4")
                      .arg(m_job.address)
                      .arg(prepare.status)
                      .arg(prepare.error)
                      .arg(QString::fromUtf8(prepare.body.left(200))));

    // The fingerprint we show in the device list came from the announcement; if
    // the certificate behind the connection belongs to someone else, the token
    // (and the files) would go to an impostor on the same LAN.
    if (!prepare.peerFingerprint.isEmpty() && !m_job.targetFingerprint.isEmpty()
        && prepare.peerFingerprint.compare(m_job.targetFingerprint, Qt::CaseInsensitive) != 0) {
        logger::write(QStringLiteral("send: certificate mismatch for %1 (%2 != %3)")
                          .arg(m_job.address, prepare.peerFingerprint, m_job.targetFingerprint));
        emit finished(false, tr("对方证书与本机记录的指纹不一致，已中止发送。"), -1, false);
        return;
    }

    if (prepare.error == QStringLiteral("cancelled") || isCancelled()) {
        // Tell the peer to drop the offer it is still showing (LocalSend 1.18
        // accepts a cancel from the pending sender's address without an id).
        // 1.17 has no such endpoint and only reacts to the closed connection.
        const httpclient::Response notify = httpclient::request(
            m_job.address, m_job.port, m_job.tls, QStringLiteral("POST"),
            QStringLiteral("/api/localsend/v2/cancel"), QByteArray(),
            QByteArray("application/json"), &m_job.identity, true, 3000);
        logger::write(QStringLiteral("cancel (pending) -> status=%1 error=%2")
                          .arg(notify.status)
                          .arg(notify.error));
        emit finished(false, isRemoteCancelled() ? tr("接收者已取消") : tr("已取消发送"), 0, false);
        return;
    }

    if (prepare.status == 204) {
        if (m_job.textOnly) {
            emit finished(true, tr("文本已送达 %1 (对方关闭了确认窗口)").arg(m_job.targetAlias),
                          204, false);
            return;
        }
        emit finished(false, tr("对方拒绝接收"), 204, false);
        return;
    }

    if (prepare.status == 401) {
        if (m_job.pin.isEmpty()) {
            // The UI asks for the PIN and calls us again.
            emit finished(false, QString(), 401, true);
            return;
        }
        emit finished(false, tr("PIN 码不正确"), 401, false);
        return;
    }

    if (prepare.status == 412) {
        // The receiver answered 412 because it saw its own fingerprint in our
        // announcement: two copies of this app are sharing a certs\ directory.
        emit finished(false,
                      tr("对方与本案使用同一套证书（很可能是同一个安装副本），请删除其中一个副本的 certs 目录后重试"),
                      412, false);
        return;
    }

    if (prepare.status == 429) {
        emit finished(false, tr("PIN 码尝试次数过多，请稍后再试"), 429, false);
        return;
    }

    if (prepare.status != 200) {
        const bool timeout = prepare.status < 0 && prepare.error == QStringLiteral("no HTTP response");
        if (timeout) {
            if (m_job.textOnly) {
                emit finished(true,
                              tr("文本内容已送达 %1 (对方可能仍在弹窗确认)").arg(m_job.targetAlias),
                              prepare.status, false);
                return;
            }
            emit finished(false, tr("对方没有在 30 秒内确认接收 (可能正在弹窗询问)"),
                          prepare.status, false);
            return;
        }
        // A peer that is gone never completes the TLS handshake (or drops the
        // connection mid-way). Distinguish "not there at all" from "there, but
        // busy": only the first one may evict the device from the list, and the
        // second one must not claim the device went offline.
        if (prepare.status < 0 && !prepare.error.isEmpty()) {
            const QString reason = prepare.error.toLower();
            const bool gone = reason.contains(QLatin1String("refused"))
                    || reason.contains(QLatin1String("unreachable"))
                    || reason.contains(QLatin1String("no route"))
                    || reason.contains(QLatin1String("remote host closed"))
                    || reason.contains(QLatin1String("timed out"));
            if (gone) {
                emit finished(false, tr("设备已离线，无法连接 %1").arg(m_job.targetAlias),
                              prepare.status, false);
                return;
            }
            if (reason.contains(QLatin1String("tls handshake failed"))
                || reason.contains(QLatin1String("handshake"))) {
                emit finished(false,
                              tr("无法与 %1 建立安全连接，对方可能正在处理上一次传输，请稍后重试")
                                      .arg(m_job.targetAlias),
                              prepare.status, false);
                return;
            }
        }
        if (prepare.status < 0 && m_job.textOnly) {
            emit finished(true, tr("文本已送达 %1 (对方已关闭确认窗口)").arg(m_job.targetAlias),
                          prepare.status, false);
            return;
        }
        QString reason;
        if (!prepare.error.isEmpty())
            reason = prepare.error;
        else
            reason = tr("接收方返回状态 %1").arg(prepare.status);
        if (!prepare.body.isEmpty()) {
            const QString body = QString::fromUtf8(prepare.body).trimmed();
            if (!body.isEmpty() && body.size() < 200)
                reason += QStringLiteral(": ") + body;
        }
        emit finished(false, tr("接收方拒绝了本次发送：%1").arg(reason), prepare.status, false);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument answer = QJsonDocument::fromJson(prepare.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !answer.isObject()) {
        emit finished(false, tr("接收方响应无法解析"), 200, false);
        return;
    }

    const QJsonObject answerObject = answer.object();
    const QString sessionId = answerObject.value(QStringLiteral("sessionId")).toString();
    const QJsonObject tokenObject = answerObject.value(QStringLiteral("files")).toObject();

    QMap<QString, QString> tokens;
    for (QJsonObject::const_iterator it = tokenObject.constBegin(); it != tokenObject.constEnd(); ++it)
        tokens.insert(it.key(), it.value().toString());

    if (sessionId.isEmpty() || tokens.isEmpty()) {
        emit finished(false, tr("接收方没有接受任何文件"), 200, false);
        return;
    }

    // From here on the receiver may cancel this session by POSTing /v2/cancel
    // with our id; the service layer matches it against this signal.
    emit prepared(sessionId);

    bool failed = false;
    QString failure;
    int uploaded = 0;

    // Sent while the upload connection is still open; the reference client
    // only reports "sender cancelled" when it receives this request.
    const QString cancelPath = QStringLiteral("/api/localsend/v2/cancel?sessionId=%1").arg(sessionId);
    const std::function<void()> notifyCancel = [this, cancelPath]() {
        const httpclient::Response cancel = httpclient::request(
            m_job.address, m_job.port, m_job.tls, QStringLiteral("POST"), cancelPath, QByteArray(),
            QByteArray("application/json"), &m_job.identity, true, 4000);
        logger::write(QStringLiteral("cancel -> status=%1 error=%2")
                          .arg(cancel.status)
                          .arg(cancel.error));
    };

    // One TLS connection for the whole session: a folder with hundreds of
    // small files used to open a connection per file, and on a real network
    // that eventually failed with "Socket operation timed out".
    httpclient::Session session;
    httpclient::Response opened;
    if (!session.open(m_job.address, m_job.port, m_job.tls, &m_job.identity, 30000,
                      [this]() { return isCancelled(); }, &opened)) {
        emit finished(false,
                      tr("无法与 %1 建立连接：%2").arg(m_job.targetAlias, opened.error), 0, false);
        return;
    }

    const auto pushFile = [&](int index, const QString &query) {
        const protocol::FileDto &file = m_job.files.at(index);
        const qint64 total = file.size;
        const std::function<bool(qint64, qint64)> progressHook =
                [this, &file, total](qint64 sent, qint64) {
            emit progress(file.id, sent, total);
            return !isCancelled();
        };
        if (m_job.paths.at(index).isEmpty()) {
            return session.post(query, file.preview.toUtf8(),
                                QByteArray("application/octet-stream"), 60000,
                                [this]() { return isCancelled(); });
        }
        return session.postFile(query, m_job.paths.at(index), total, 60000, progressHook,
                                notifyCancel, [this]() { return isCancelled(); });
    };

    for (int i = 0; i < m_job.files.size() && !failed; ++i) {
        const protocol::FileDto &file = m_job.files.at(i);
        if (!tokens.contains(file.id))
            continue;
        if (isCancelled()) {
            failed = true;
            failure = isRemoteCancelled() ? tr("接收者已取消") : tr("已取消");
            break;
        }

        emit stage(tr("正在发送 %1").arg(file.fileName));

        const QString query = QStringLiteral("/api/localsend/v2/upload?sessionId=%1&fileId=%2&token=%3")
                                  .arg(sessionId, file.id, tokens.value(file.id));
        const qint64 total = file.size;

        httpclient::Response upload = pushFile(i, query);

        // A keep-alive connection can be closed by the peer between two files
        // (idle timeout, or its own error handling). Reconnect once before
        // blaming the network.
        if (upload.status < 0 && !isCancelled()) {
            const QString reason = upload.error.toLower();
            const bool retryable = reason.contains(QLatin1String("timed out"))
                    || reason.contains(QLatin1String("handshake"))
                    || reason.contains(QLatin1String("no http response"))
                    || reason.contains(QLatin1String("connect failed"));
            if (retryable) {
                logger::write(QStringLiteral("send: retrying %1 after: %2")
                                  .arg(file.fileName, upload.error));
                session.close();
                upload = pushFile(i, query);
            }
        }

        // The first attempt may have reached the receiver even though its answer
        // was lost; the retry then hits "this file was already received". That
        // is a success, not a failure of the whole transfer.
        if (upload.status == 403
            && upload.body.trimmed() == QByteArray("file-already-received")) {
            logger::write(QStringLiteral("send: %1 already received by the peer").arg(file.fileName));
            upload.status = 200;
            upload.body.clear();
        }

        if (upload.status != 200) {
            failed = true;
            const QString body = QString::fromUtf8(upload.body).trimmed();
            if (upload.error == QStringLiteral("cancelled")) {
                failure = isRemoteCancelled() ? tr("接收者已取消") : tr("已取消");
            } else if (body == QStringLiteral("receiver-cancelled")) {
                // The receiving user pressed "取消接收"; our socket was closed on
                // purpose, this is not a disk or permission problem.
                failure = tr("接收者已取消");
            } else if (upload.status == 500) {
                // LocalSend answers 500 when its own writer failed (full disk,
                // missing permission). The JSON body only repeats the status
                // code, so explain the likely cause instead of showing it.
                failure = tr("接收方无法保存文件 (对端磁盘空间不足或没有写入权限)");
            } else if (upload.error.startsWith(QStringLiteral("peer closed"))) {
                // The receiver hung up mid-transfer: its user declined the file
                // after our prepare-upload had already been accepted.
                failure = tr("接收者已取消");
            } else if (!body.isEmpty()) {
                // The peer explains the failure in its body ("Failed to create
                // D:/Downloads\lsaero.exe: 拒绝访问。 (os error 5)"): surface that
                // instead of a generic transport message.
                failure = tr("接收方报错：%1")
                                  .arg(QString::fromUtf8(upload.body).trimmed().left(300));
            } else if (upload.error.startsWith(QStringLiteral("write"))) {
                failure = tr("对方中断了接收 (对端可能无法写入保存位置)");
            } else if (upload.error.isEmpty()) {
                failure = tr("接收方返回状态 %1").arg(upload.status);
            } else {
                failure = upload.error;
            }
            break;
        }

        logger::write(QStringLiteral("send: upload %1 -> status=%2 bytes=%3")
                          .arg(file.fileName)
                          .arg(upload.status)
                          .arg(total));
        ++uploaded;
    }

    if (failed) {
        // Tell the peer to drop whatever it still holds for this session.
        httpclient::request(m_job.address, m_job.port, m_job.tls, QStringLiteral("POST"),
                            QStringLiteral("/api/localsend/v2/cancel?sessionId=%1").arg(sessionId),
                            QByteArray(), QByteArray("application/json"), &m_job.identity, true,
                            4000);
        emit finished(false, failure, 0, false);
        return;
    }

    logger::write(QStringLiteral("send -> %1 ok, %2 file(s)").arg(m_job.address).arg(uploaded));
    emit finished(true, tr("已向 %1 发送 %2 个文件").arg(m_job.targetAlias).arg(uploaded),
                  200, false);
}
