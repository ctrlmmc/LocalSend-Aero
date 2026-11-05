#include "settings.h"

#include "theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostInfo>
#include <QStandardPaths>
#include <QStringList>

namespace {

const char kAlias[] = "app/alias";
const char kPort[] = "network/port";
const char kSaveDir[] = "receive/save_dir";
const char kAskEveryTime[] = "receive/ask_every_time";
const char kAutostart[] = "app/autostart";
const char kHttps[] = "network/https";
const char kLanguage[] = "app/language";
const char kFontFamily[] = "app/font_family";
const char kReceivePin[] = "receive/pin";
const char kWhiteAliasMaximized[] = "app/white_alias_maximized";

} // namespace

Settings::Settings()
    : m_settings(configPath(), QSettings::IniFormat)
{
    m_settings.setIniCodec("UTF-8");
}

QString Settings::configPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        const QString candidate = appDir + QStringLiteral("/lsaero.ini");
        QFile probe(candidate);
        if (probe.open(QIODevice::Append)) {
            probe.close();
            return candidate;
        }
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.lsaero");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/lsaero.ini");
}

QString Settings::defaultAlias() const
{
    const QByteArray computer = qgetenv("COMPUTERNAME");
    if (!computer.isEmpty())
        return QString::fromLocal8Bit(computer);
    const QString host = QHostInfo::localHostName();
    if (!host.isEmpty())
        return host;
    return QStringLiteral("Windows PC");
}

QString Settings::defaultSaveDir() const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/Downloads");
    return QDir::toNativeSeparators(dir);
}

QString Settings::alias() const
{
    // An absent (or blank) value means "use the computer name": that keeps
    // lsaero.ini free of machine specific data for portable installs.
    const QString stored = m_settings.value(QString::fromLatin1(kAlias)).toString().trimmed();
    return stored.isEmpty() ? defaultAlias() : stored;
}

void Settings::setAlias(const QString &alias)
{
    const QString trimmed = alias.trimmed();
    if (trimmed.isEmpty()) {
        m_settings.remove(QString::fromLatin1(kAlias));
        return;
    }
    m_settings.setValue(QString::fromLatin1(kAlias), trimmed);
}

bool Settings::aliasSet() const
{
    const QString stored = m_settings.value(QString::fromLatin1(kAlias)).toString().trimmed();
    return !stored.isEmpty();
}

int Settings::port() const
{
    return m_settings.value(QString::fromLatin1(kPort), 53317).toInt();
}

void Settings::setPort(int port)
{
    m_settings.setValue(QString::fromLatin1(kPort), port);
}

QString Settings::saveDir() const
{
    const QString stored = m_settings.value(QString::fromLatin1(kSaveDir)).toString();
    if (!stored.isEmpty())
        return QDir::fromNativeSeparators(stored);
    return QDir::fromNativeSeparators(defaultSaveDir());
}

void Settings::setSaveDir(const QString &dir)
{
    m_settings.setValue(QString::fromLatin1(kSaveDir), QDir::toNativeSeparators(dir));
}

bool Settings::askEveryTime() const
{
    return m_settings.value(QString::fromLatin1(kAskEveryTime), false).toBool();
}

void Settings::setAskEveryTime(bool on)
{
    m_settings.setValue(QString::fromLatin1(kAskEveryTime), on);
}

bool Settings::autostart() const
{
    return m_settings.value(QString::fromLatin1(kAutostart), false).toBool();
}

void Settings::setAutostart(bool on)
{
    m_settings.setValue(QString::fromLatin1(kAutostart), on);
}

bool Settings::httpsEnabled() const
{
    return m_settings.value(QString::fromLatin1(kHttps), true).toBool();
}

void Settings::setHttpsEnabled(bool on)
{
    m_settings.setValue(QString::fromLatin1(kHttps), on);
}

QString Settings::language() const
{
    return m_settings.value(QString::fromLatin1(kLanguage), QStringLiteral("auto")).toString();
}

void Settings::setLanguage(const QString &language)
{
    m_settings.setValue(QString::fromLatin1(kLanguage), language);
}

QString Settings::fontFamily() const
{
    return m_settings.value(QString::fromLatin1(kFontFamily), QString()).toString();
}

void Settings::setFontFamily(const QString &family)
{
    m_settings.setValue(QString::fromLatin1(kFontFamily), family);
}

QString Settings::deviceType() const
{
    return QStringLiteral("desktop");
}

bool Settings::whiteAliasMaximized() const
{
    // Unset means "decide by OS": Vista paints the maximised glass frame black,
    // so a white title keeps the alias readable there; every newer Windows
    // keeps the light frame and the default colour is correct.
    const bool fallback = theme::isWindowsVista();
    return m_settings.value(QString::fromLatin1(kWhiteAliasMaximized), fallback).toBool();
}

void Settings::setWhiteAliasMaximized(bool on)
{
    m_settings.setValue(QString::fromLatin1(kWhiteAliasMaximized), on);
}

bool Settings::whiteAliasMaximizedSet() const
{
    return m_settings.contains(QString::fromLatin1(kWhiteAliasMaximized));
}

QString Settings::receivePin() const
{
    return m_settings.value(QString::fromLatin1(kReceivePin), QString()).toString();
}

void Settings::setReceivePin(const QString &pin)
{
    m_settings.setValue(QString::fromLatin1(kReceivePin), pin);
}
