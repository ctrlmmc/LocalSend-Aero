#include "autostart.h"
#include "appinfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QString>

namespace {

QString runKeyPath()
{
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
}

QString valueName()
{
    // One Run value per installation folder: two portable copies may both be
    // allowed to start with Windows, and a shared name would silently
    // overwrite the other one's command line.
    const qint64 hash = qHash(QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toLower());
    return QStringLiteral("%1-%2").arg(QString::fromLatin1(appinfo::appName()),
                                       QString::number(hash & 0xffff, 16));
}

} // namespace

namespace autostart {

QString commandLine()
{
    const QString exe = QCoreApplication::applicationFilePath();
    return QStringLiteral("\"%1\" --tray").arg(QDir::toNativeSeparators(exe));
}

bool isEnabled()
{
    QSettings run(runKeyPath(), QSettings::NativeFormat);
    const QString stored = run.value(valueName()).toString();
    if (stored.isEmpty())
        return false;

    // Treat any entry pointing at our executable as enabled, even if the
    // path was quoted differently by an older build.
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return stored.contains(exe, Qt::CaseInsensitive);
}

void setEnabled(bool on)
{
    QSettings run(runKeyPath(), QSettings::NativeFormat);
    if (on)
        run.setValue(valueName(), commandLine());
    else
        run.remove(valueName());
    run.sync();
}

} // namespace autostart
