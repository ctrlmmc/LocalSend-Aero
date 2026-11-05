#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "networkservice.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSystemTrayIcon>

class QCheckBox;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class Settings;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    // glassMode: native window frame plus DwmExtendFrameIntoClientArea, so the
    // 6px border around the card shows the Aero glass (Vista/7) or the window
    // behind it (Windows 8+ with composition enabled).
    explicit MainWindow(Settings *settings, bool glassMode, QWidget *parent = 0);
    ~MainWindow();

    void setCurrentTab(int index);
    // The window never owns the service: main() keeps it alive for the whole
    // session so the listener and the discovery socket share one lifetime.
    void setNetworkService(NetworkService *service);

public slots:
    // A secondary process asked the running instance to come to the front.
    void onSecondInstanceMessage(const QString &message);
    void showFromTray();

protected:
    void closeEvent(QCloseEvent *event);
    void showEvent(QShowEvent *event);
    void changeEvent(QEvent *event);
    void dragEnterEvent(QDragEnterEvent *event);
    void dropEvent(QDropEvent *event);
    // Watches the transfer widgets so the footer (and with it, the card's
    // bottom edge) follows their visibility without touching every call site.
    bool eventFilter(QObject *watched, QEvent *event);

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onAutostartToggled(bool on);
    void chooseSaveDir();
    void onLanguageChanged(int index);
    void onFontChanged(int index);
    void onWhiteAliasToggled(bool on);
    void refreshDevices();
    void pickFiles();
    void pickFolder();
    // Ask for an address and hand it to the service (for networks where neither
    // multicast nor scanning reaches the peer).
    void addDeviceManually();
    void sendSelected();
    void cancelSend();
    void sendText();
    void onSendProgress(const QString &fileId, qint64 sent, qint64 total);
    void onSendFinished(bool ok, const QString &message);
    void onPinRequired(const QString &alias);
    void onIncomingOffer(const NetworkService::IncomingOffer &offer);
    void onReceiveStalled();
    void refreshHistory();
    void clearHistory();
    void onHistoryContextMenu(const QPoint &pos);
    void onHistoryActivated(QTreeWidgetItem *item, int column);
    // Saves the device name as it is typed and re-announces it right away.
    void onAliasChanged(const QString &text);
    // Re-applies the Aero/glass state. Schedules a rebuild when the composition
    // state changed (a HWND cannot switch between alpha and plain at runtime).
    // These are slots because the polling timer and the deferred rebuild are
    // wired through the meta-object system.
    void applyGlassState();
    void switchGlassMode();

private:
    void setupUi();
    void setupTray();
    void loadFromSettings();
    void scheduleGlassSwitch();
    void refreshGlassVisuals();
    void updateSendState();
    // Swaps the title bar / tray icon to the transfer art while `active`.
    void updateTransferIcon(bool active);
    // Applies the "white alias while maximised" rule (Vista compatibility).
    void updateHeaderColors();

protected:
    // Watches WM_DWMCOMPOSITIONCHANGED so the window can fall back to the
    // opaque layout when Aero is switched off while the app is running.
    bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;

    QWidget *createSendTab();
    QWidget *createHistoryTab();
    QWidget *createSettingsTab();
    void addFiles(const QStringList &paths);
    void addTextMessage(const QString &text);
    void openOfferDialog(const NetworkService::IncomingOffer &offer);
    void openOfferDialogNow(const NetworkService::IncomingOffer &offer);
    void offerTextMessage(const NetworkService::IncomingOffer &offer);

    // Shows/hides the footer strip and repaints the card so its bottom edge
    // lands exactly kFooterGap above the footer.
    void updateFooterVisibility();
    // "12.4 MB/s, 剩余 8 秒" for a transfer that moves `done` of `total` bytes.
    QString rateSuffix(const QString &fileId, qint64 done, qint64 total);
    void resetRate();
    // Same smoothing for the aggregate bar of a multi-file transfer.
    QString totalRateSuffix(qint64 done, qint64 total);
    void resetTotalRate();
    // Aggregate progress across every file of the current session; the second
    // bar only exists while two or more files travel.
    void beginAggregate(qint64 total, int files);
    void updateAggregate(const QString &fileId, qint64 sent);
    void endAggregate();
    static QString formatBytes(qint64 bytes);
    static QString formatSeconds(double seconds);

    Settings *m_settings;
    NetworkService *m_network;
    bool m_glassMode;
    // True while the DWM glass is actually applied (composition on). It drifts
    // from m_glassMode when Aero is turned off at runtime.
    bool m_glassActive;
    QWidget *m_card;
    // Polls the composition state: Windows 7 does not always deliver
    // WM_DWMCOMPOSITIONCHANGED to a Qt window, and the theme switch has to be
    // picked up either way.
    QTimer *m_glassPoll;
    bool m_glassSwitchPending;
    bool m_trayHintShown;
    bool m_quitting;
    bool m_receiving;
    // The tray/title icon swaps to the transfer art while bytes are moving;
    // the big header icon always keeps the app icon.
    bool m_transferIconActive;
    int m_trayRetries;

    // Receive dialog bookkeeping: while it is on screen the sender may cancel,
    // and answering a request that no longer exists must never happen.
    QPointer<QDialog> m_offerDialog;
    int m_offerRequestId;
    bool m_offerDropped;
    // Offers that arrived while another question was on screen.
    QList<NetworkService::IncomingOffer> m_offerQueue;

    QSystemTrayIcon *m_tray;
    QTabBar *m_tabBar;
    QStackedWidget *m_pages;
    QLabel *m_headerAlias;
    QLabel *m_headerSubtitle;
    QLabel *m_saveDirLabel;
    QCheckBox *m_autostartBox;
    QCheckBox *m_askBox;
    QCheckBox *m_httpsBox;
    QComboBox *m_languageBox;
    QComboBox *m_fontBox;
    QLabel *m_networkStatus;
    QListWidget *m_deviceList;
    QPushButton *m_pickButton;
    QPushButton *m_folderButton;
    QPushButton *m_sendButton;
    QPushButton *m_textButton;
    QPushButton *m_refreshButton;
    QPushButton *m_addDeviceButton;
    QPushButton *m_cancelButton;
    QListWidget *m_fileList;
    QProgressBar *m_progress;
    QLabel *m_transferStatus;
    // Second bar for multi-file transfers: whole-session percentage plus the
    // same speed/remaining sentence as the per-file row.
    QProgressBar *m_progressTotal;
    QLabel *m_transferStatusTotal;
    QWidget *m_footer;

    // Transfer rate: exponential average over ~1 s windows, reset per file.
    QElapsedTimer m_rateTimer;
    QString m_rateFileId;
    qint64 m_rateLastBytes;
    double m_rateBps;
    // Own meter for the aggregate bar, so the two rows never fight over one
    // measurement window.
    QElapsedTimer m_totalRateTimer;
    qint64 m_totalRateLast;
    double m_totalRateBps;
    bool m_multiFile;
    qint64 m_aggTotal;
    qint64 m_aggDone;
    QHash<QString, qint64> m_aggSeen;
    QTreeWidget *m_historyTree;
    // Fires when an accepted inbound transfer stops making progress: the
    // sender can die or cancel without ever telling us, and the UI must not
    // stay stuck in "正在接收" (which also blocks every later send).
    QTimer *m_receiveWatchdog;
    QCheckBox *m_pinBox;
    QLineEdit *m_pinEdit;
    QLineEdit *m_aliasEdit;
    QCheckBox *m_whiteAliasBox;
    // "此选项在 Aero 禁用时无效。" under the checkbox: shown, not disabled,
    // when the window runs with the plain system background.
    QLabel *m_whiteAliasHint;
    QStringList m_selectedPaths;
    // The picks as the user made them (folders stay folders): replaying these
    // reproduces a folder transfer in the history's "send again".
    QStringList m_selectedRoots;
    // Display name sent to the peer: "folder/sub/file.txt" for folder picks,
    // the plain file name otherwise.
    QStringList m_selectedNames;

    // Text message in memory: no temporary file, `preview` carries the body.
    QByteArray m_textBody;
    QString m_textFileName;
    QString m_textPreview;

    // Kept for the PIN retry: the user is asked for the code after the peer
    // answered 401, and the same selection is sent again.
    NetworkService::Device m_pendingTarget;
    QList<protocol::FileDto> m_pendingFiles;
    QList<QString> m_pendingPaths;
};

#endif // MAINWINDOW_H
