#ifndef SETTINGS_H
#define SETTINGS_H

#include <QSettings>
#include <QString>

// Persistent configuration for LocalSend Aero.
//
// The file lives next to lsaero.exe (portable layout). If that directory is
// read-only (Program Files, CD, ...) it falls back to the per-user data
// directory, so the app keeps working without administrator rights.
class Settings
{
public:
    Settings();

    static QString configPath();

    QString alias() const;
    void setAlias(const QString &alias);
    // True when lsaero.ini holds an explicit name (the settings box then shows
    // it; an absent key means "use the computer name" and the box stays empty).
    bool aliasSet() const;

    int port() const;
    void setPort(int port);

    QString saveDir() const;
    void setSaveDir(const QString &dir);

    bool askEveryTime() const;
    void setAskEveryTime(bool on);

    bool autostart() const;
    void setAutostart(bool on);

    bool httpsEnabled() const;
    void setHttpsEnabled(bool on);

    // "auto", "zh_CN" or "en"
    QString language() const;
    void setLanguage(const QString &language);

    // Empty means "use the Qt/system default font".
    QString fontFamily() const;
    void setFontFamily(const QString &family);

    QString deviceType() const;

    // Header alias turns white while the window is maximised. Vista needs this
    // (its maximised glass frame is black); the default follows the OS when the
    // key is absent from lsaero.ini.
    bool whiteAliasMaximized() const;
    void setWhiteAliasMaximized(bool on);
    bool whiteAliasMaximizedSet() const;

    // Empty means "no PIN required" for incoming transfers.
    QString receivePin() const;
    void setReceivePin(const QString &pin);

    QString defaultAlias() const;
    QString defaultSaveDir() const;

private:
    QSettings m_settings;
};

#endif // SETTINGS_H
