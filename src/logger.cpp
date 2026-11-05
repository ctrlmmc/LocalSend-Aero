#include "logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

namespace {

QMutex g_mutex;
QString g_path;

} // namespace

namespace logger {

void init(const QString &path)
{
    QMutexLocker lock(&g_mutex);
    g_path = path;
    const QString dir = QFileInfo(path).absolutePath();
    if (!dir.isEmpty())
        QDir().mkpath(dir);

    // Keep the previous run: after a crash the interesting lines are the ones
    // written before the restart, not the few it managed afterwards.
    QFile previous(path);
    if (previous.exists()) {
        const qint64 size = previous.size();
        if (size > 0 && size < 2 * 1024 * 1024) {
            QFile::remove(path + QStringLiteral(".1"));
            QFile::rename(path, path + QStringLiteral(".1"));
        } else {
            previous.remove();
        }
    }

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.close();
}

QString path()
{
    QMutexLocker lock(&g_mutex);
    return g_path;
}

void write(const QString &line)
{
    QMutexLocker lock(&g_mutex);
    if (g_path.isEmpty())
        return;
    QFile file(g_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz "))
           << line << QLatin1Char('\n');
    file.close();
}

} // namespace logger
