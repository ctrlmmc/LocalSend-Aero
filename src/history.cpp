#include "history.h"

#include "logger.h"
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTextStream>

#include <algorithm>

namespace history {

namespace {

const char kFileName[] = "history.jsonl";

// The file used to grow for ever while only the newest 200 rows are shown.
// Called once it gets big; keeps the newest entries only.
void trim()
{
    QFile source(path());
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QList<QByteArray> lines = source.readAll().split('\n');
    source.close();
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
        lines.removeLast();
    const int keep = 500;
    if (lines.size() <= keep)
        return;
    const QByteArray tail = lines.mid(lines.size() - keep).join('\n') + '\n';

    QFile target(path());
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    target.write(tail);
    target.close();
    logger::write(QStringLiteral("history: trimmed to %1 entries").arg(keep));
}

} // namespace

QString path()
{
    const QFileInfo info(Settings::configPath());
    return info.absolutePath() + QStringLiteral("/") + QString::fromLatin1(kFileName);
}

void append(const Entry &entry)
{
    QJsonObject object;
    object.insert(QStringLiteral("time"), entry.time.toString(Qt::ISODate));
    object.insert(QStringLiteral("direction"), entry.direction);
    object.insert(QStringLiteral("peer"), entry.peer);
    object.insert(QStringLiteral("fingerprint"), entry.fingerprint);
    object.insert(QStringLiteral("files"), entry.fileCount);
    object.insert(QStringLiteral("bytes"), static_cast<double>(entry.bytes));
    object.insert(QStringLiteral("ok"), entry.ok);
    object.insert(QStringLiteral("detail"), entry.detail);
    if (!entry.paths.isEmpty()) {
        QJsonArray paths;
        for (int i = 0; i < entry.paths.size(); ++i)
            paths.append(entry.paths.at(i));
        object.insert(QStringLiteral("paths"), paths);
    }
    if (!entry.roots.isEmpty()) {
        QJsonArray roots;
        for (int i = 0; i < entry.roots.size(); ++i)
            roots.append(entry.roots.at(i));
        object.insert(QStringLiteral("roots"), roots);
    }

    QFile file(path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        logger::write(QStringLiteral("history: cannot append: %1").arg(file.errorString()));
        return;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.write("\n");
    file.close();

    // The file used to grow for ever while only the newest 200 rows are shown.
    if (file.size() > 256 * 1024)
        trim();
}

QList<Entry> load(int limit)
{
    QList<Entry> rows;
    QFile file(path());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return rows;

    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;

        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject())
            continue;
        const QJsonObject object = document.object();

        Entry entry;
        entry.time = QDateTime::fromString(object.value(QStringLiteral("time")).toString(), Qt::ISODate);
        entry.direction = object.value(QStringLiteral("direction")).toString();
        entry.peer = object.value(QStringLiteral("peer")).toString();
        entry.fingerprint = object.value(QStringLiteral("fingerprint")).toString();
        entry.fileCount = object.value(QStringLiteral("files")).toInt();
        entry.bytes = static_cast<qint64>(object.value(QStringLiteral("bytes")).toDouble());
        entry.ok = object.value(QStringLiteral("ok")).toBool();
        entry.detail = object.value(QStringLiteral("detail")).toString();
        const QJsonArray storedPaths = object.value(QStringLiteral("paths")).toArray();
        for (int i = 0; i < storedPaths.size(); ++i) {
            const QString stored = storedPaths.at(i).toString();
            if (!stored.isEmpty())
                entry.paths.append(stored);
        }
        const QJsonArray storedRoots = object.value(QStringLiteral("roots")).toArray();
        for (int i = 0; i < storedRoots.size(); ++i) {
            const QString stored = storedRoots.at(i).toString();
            if (!stored.isEmpty())
                entry.roots.append(stored);
        }
        if (entry.roots.isEmpty())
            entry.roots = entry.paths;
        rows.append(entry);
    }

    // Newest first, capped so the table stays responsive.
    std::reverse(rows.begin(), rows.end());
    if (limit > 0 && rows.size() > limit)
        rows = rows.mid(0, limit);
    return rows;
}

void clear()
{
    QFile::remove(path());
}

} // namespace history
