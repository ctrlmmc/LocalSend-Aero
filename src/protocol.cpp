#include "protocol.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace protocol {

const char *const kVersion = "2.2";
const char *const kMulticastGroup = "224.0.0.167";
const char *const kApiPrefix = "/api/localsend/v2";

namespace {

QString stringOr(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QString::fromLatin1(key));
    return value.isString() ? value.toString() : QString();
}

} // namespace

QJsonObject deviceToJson(const DeviceInfo &device, bool includeTransport)
{
    QJsonObject object;
    object.insert(QStringLiteral("alias"), device.alias);
    object.insert(QStringLiteral("version"), device.version);
    if (!device.deviceModel.isEmpty())
        object.insert(QStringLiteral("deviceModel"), device.deviceModel);
    object.insert(QStringLiteral("deviceType"), device.deviceType);
    object.insert(QStringLiteral("fingerprint"), device.fingerprint);
    if (includeTransport) {
        object.insert(QStringLiteral("port"), device.port);
        object.insert(QStringLiteral("protocol"), device.protocol);
        object.insert(QStringLiteral("download"), device.download);
    }
    return object;
}

DeviceInfo deviceFromJson(const QJsonObject &object)
{
    DeviceInfo device;
    device.alias = stringOr(object, "alias");
    device.version = stringOr(object, "version");
    device.deviceModel = stringOr(object, "deviceModel");
    device.deviceType = stringOr(object, "deviceType");
    device.fingerprint = stringOr(object, "fingerprint");
    device.port = object.value(QStringLiteral("port")).toInt();
    device.protocol = stringOr(object, "protocol");
    device.download = object.value(QStringLiteral("download")).toBool();
    device.valid = !device.alias.isEmpty() && !device.fingerprint.isEmpty();
    if (device.deviceType.isEmpty())
        device.deviceType = QStringLiteral("desktop");
    return device;
}

QJsonObject fileToJson(const FileDto &file)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), file.id);
    object.insert(QStringLiteral("fileName"), file.fileName);
    object.insert(QStringLiteral("size"), file.size);
    object.insert(QStringLiteral("fileType"), file.fileType);
    if (!file.sha256.isEmpty())
        object.insert(QStringLiteral("sha256"), file.sha256);
    if (!file.preview.isEmpty())
        object.insert(QStringLiteral("preview"), file.preview);
    if (!file.metadata.modified.isEmpty() || !file.metadata.accessed.isEmpty()) {
        QJsonObject metadata;
        if (!file.metadata.modified.isEmpty())
            metadata.insert(QStringLiteral("modified"), file.metadata.modified);
        if (!file.metadata.accessed.isEmpty())
            metadata.insert(QStringLiteral("accessed"), file.metadata.accessed);
        object.insert(QStringLiteral("metadata"), metadata);
    }
    return object;
}

FileDto fileFromJson(const QJsonObject &object)
{
    FileDto file;
    file.id = stringOr(object, "id");
    file.fileName = stringOr(object, "fileName");
    file.size = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());
    file.fileType = stringOr(object, "fileType");
    file.sha256 = stringOr(object, "sha256");
    // 1.17.x and the v2.1 flavour of the spec still use "hash".
    if (file.sha256.isEmpty())
        file.sha256 = stringOr(object, "hash");
    file.preview = stringOr(object, "preview");
    const QJsonObject metadata = object.value(QStringLiteral("metadata")).toObject();
    file.metadata.modified = stringOr(metadata, "modified");
    file.metadata.accessed = stringOr(metadata, "accessed");
    return file;
}

QMap<QString, FileDto> filesFromJson(const QJsonObject &object)
{
    QMap<QString, FileDto> files;
    for (QJsonObject::const_iterator it = object.constBegin(); it != object.constEnd(); ++it) {
        const FileDto file = fileFromJson(it.value().toObject());
        if (!file.id.isEmpty())
            files.insert(file.id, file);
    }
    return files;
}

QJsonObject prepareUploadRequest(const DeviceInfo &info, const QMap<QString, FileDto> &files)
{
    QJsonObject object;
    object.insert(QStringLiteral("info"), deviceToJson(info, true));

    QJsonObject fileObject;
    for (QMap<QString, FileDto>::const_iterator it = files.constBegin(); it != files.constEnd(); ++it)
        fileObject.insert(it.key(), fileToJson(it.value()));
    object.insert(QStringLiteral("files"), fileObject);
    return object;
}

QJsonObject prepareUploadResponse(const QString &sessionId, const QMap<QString, QString> &tokens)
{
    QJsonObject files;
    for (QMap<QString, QString>::const_iterator it = tokens.constBegin(); it != tokens.constEnd(); ++it)
        files.insert(it.key(), it.value());

    QJsonObject object;
    object.insert(QStringLiteral("sessionId"), sessionId);
    object.insert(QStringLiteral("files"), files);
    return object;
}

QString userName()
{
    return QStringLiteral("LocalSend User");
}

void expandLocalPaths(const QStringList &inputs, QStringList *files, QStringList *names,
                      int *emptyFolders)
{
    if (!files || !names)
        return;
    files->clear();
    names->clear();
    if (emptyFolders)
        *emptyFolders = 0;

    for (int i = 0; i < inputs.size(); ++i) {
        const QFileInfo info(inputs.at(i));
        if (info.isDir()) {
            // "C:\pics" -> "pics"; a bare root ("C:\") keeps its absolute path
            // so the relative names stay meaningful.
            const QString base = info.fileName().isEmpty() ? info.absoluteFilePath() : info.fileName();
            QDir dir(info.absoluteFilePath());
            QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::Hidden | QDir::System,
                            QDirIterator::Subdirectories);
            int added = 0;
            while (it.hasNext()) {
                const QString file = it.next();
                const QString relative = dir.relativeFilePath(file).replace(QLatin1Char('\\'),
                                                                           QLatin1Char('/'));
                files->append(file);
                names->append(base + QLatin1Char('/') + relative);
                ++added;
            }
            if (added == 0 && emptyFolders)
                ++*emptyFolders;
        } else if (info.isFile()) {
            files->append(info.absoluteFilePath());
            names->append(info.fileName());
        }
    }
}

} // namespace protocol
