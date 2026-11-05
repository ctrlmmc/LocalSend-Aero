#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

// LocalSend v2 wire format (compatible with 2.1 clients as well).
//
// Everything here mirrors ref\localsend\packages\core\src\http\dto_v2.rs and
// model\transfer.rs. Two details matter for interoperability:
//   * 1.17.x (protocol 2.1) names the checksum "hash", 1.18.x (2.2) "sha256";
//     we write "sha256" and accept both when parsing.
//   * register responses never carry "port"/"protocol" - those only exist in
//     the announcement / register request. Do not add them.
namespace protocol {

extern const char *const kVersion;          // "2.2"
extern const char *const kMulticastGroup;   // "224.0.0.167"
extern const char *const kApiPrefix;        // "/api/localsend/v2"

const quint16 kDefaultPort = 53317;
const int kMulticastPort = 53317;

struct DeviceInfo
{
    DeviceInfo() : port(0), download(false), valid(false) {}

    QString alias;
    QString version;
    QString deviceModel;
    QString deviceType;   // mobile / desktop / web / headless / server
    QString fingerprint;  // SHA-256 of the DER certificate, upper case hex
    int port;
    QString protocol;     // "http" or "https"
    bool download;
    bool valid;
};

QJsonObject deviceToJson(const DeviceInfo &device, bool includeTransport);
DeviceInfo deviceFromJson(const QJsonObject &object);

struct FileMetadata
{
    QString modified;
    QString accessed;
};

struct FileDto
{
    FileDto() : size(0) {}

    QString id;
    QString fileName;
    qint64 size;
    QString fileType;     // MIME type
    QString sha256;       // wire field "sha256" (2.2) or "hash" (2.1)
    QString preview;      // text messages travel here
    FileMetadata metadata;
};

QJsonObject fileToJson(const FileDto &file);
FileDto fileFromJson(const QJsonObject &object);
QMap<QString, FileDto> filesFromJson(const QJsonObject &object);

QJsonObject prepareUploadRequest(const DeviceInfo &info, const QMap<QString, FileDto> &files);
QJsonObject prepareUploadResponse(const QString &sessionId, const QMap<QString, QString> &tokens);

// Incoming prepare-upload, already parsed: the receiver UI decides which files
// to accept and where they go.
struct PrepareUpload
{
    PrepareUpload() : accepted(false) {}

    DeviceInfo info;                 // sender as advertised in the body
    QString senderAddress;           // source address of the TCP connection
    QMap<QString, FileDto> files;
    bool accepted;
};

QString userName();

// Expands a user selection into concrete files. Directories are walked
// recursively because the protocol has no directory object: every leaf becomes
// one entry whose name is "folder/sub/file", which is what the reference client
// does (AddDirectoryAction) and what its receiver uses to rebuild the tree.
// `names` receives the display names in the same order as `files`.
void expandLocalPaths(const QStringList &inputs, QStringList *files, QStringList *names,
                      int *emptyFolders = 0);

} // namespace protocol

#endif // PROTOCOL_H
