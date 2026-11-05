#include "appinfo.h"
#include "autostart.h"
#include "logger.h"
#include "mainwindow.h"
#include "networkservice.h"
#include "protocol.h"
#include "i18n.h"
#include "settings.h"
#include "singleinstance.h"
#include "theme.h"
#include "ui.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QPixmap>
#include <QScreen>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <QWindow>

#include <windows.h>

namespace {

// Development helper used by the P2 acceptance run: verifies the pieces that
// cannot be seen in a screenshot (config path, tray availability, autostart
// round trip, single-instance pipe) and writes the result to a text file.
int runSelfTest(Settings &settings, SingleInstance &instance, const QString &path)
{
    QStringList lines;
    lines << QStringLiteral("app=") + QString::fromLatin1(appinfo::appName())
          << QStringLiteral("version=") + QString::fromLatin1(appinfo::appVersion())
          << QStringLiteral("qt=") + QString::fromLatin1(qVersion())
          << QStringLiteral("style=") + (qApp->style() ? qApp->style()->objectName() : QString())
          << QStringLiteral("font=") + QApplication::font().family() + QStringLiteral(" / ")
                 + QString::number(QApplication::font().pointSizeF())
          << QStringLiteral("font_setting=") + settings.fontFamily()
          << QStringLiteral("styles=") + QStyleFactory::keys().join(QStringLiteral(", "))
          << QStringLiteral("composition=") + (theme::compositionEnabled() ? QStringLiteral("on") : QStringLiteral("off"))
          << QStringLiteral("card_enabled=") + (theme::cardEnabled(false, false) ? QStringLiteral("yes") : QStringLiteral("no"))
          << QStringLiteral("tray_available=") + (QSystemTrayIcon::isSystemTrayAvailable() ? QStringLiteral("yes") : QStringLiteral("no"))
          << QStringLiteral("config_path=") + Settings::configPath()
          << QStringLiteral("config_writable=") + (QFileInfo(Settings::configPath()).isWritable() ? QStringLiteral("yes") : QStringLiteral("no"))
          << QStringLiteral("save_dir=") + settings.saveDir()
          << QStringLiteral("alias=") + settings.alias()
          << QStringLiteral("port=") + QString::number(settings.port())
          << QStringLiteral("single_instance_first=") + (instance.isFirstInstance() ? QStringLiteral("yes") : QStringLiteral("no"));

    const bool before = autostart::isEnabled();
    autostart::setEnabled(true);
    const bool enabled = autostart::isEnabled();
    autostart::setEnabled(false);
    const bool disabled = autostart::isEnabled();
    autostart::setEnabled(before);

    lines << QStringLiteral("autostart_before=") + (before ? QStringLiteral("on") : QStringLiteral("off"))
          << QStringLiteral("autostart_after_set=") + (enabled ? QStringLiteral("on") : QStringLiteral("off"))
          << QStringLiteral("autostart_after_clear=") + (disabled ? QStringLiteral("on") : QStringLiteral("off"))
          << QStringLiteral("autostart_restored=") + (autostart::isEnabled() == before ? QStringLiteral("yes") : QStringLiteral("no"));

    const QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        lines << QStringLiteral("screen=") + QString::number(screen->size().width()) + QStringLiteral("x")
                     + QString::number(screen->size().height())
              << QStringLiteral("dpr=") + QString::number(screen->devicePixelRatio());
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 2;
    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    for (int i = 0; i < lines.size(); ++i)
        stream << lines.at(i) << "\n";
    file.close();
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);

    QApplication app(argc, argv);
    app.setApplicationName(QString::fromLatin1(appinfo::appName()));
    app.setApplicationVersion(QString::fromLatin1(appinfo::appVersion()));
    app.setOrganizationName(QString::fromLatin1(appinfo::orgName()));

    theme::applyAppStyle();
    app.setWindowIcon(theme::appIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription(QString::fromLatin1(appinfo::appName()));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption aeroOption(QStringLiteral("aero"),
                                  QStringLiteral("Force the Aero card drawing (no fallback)."));
    QCommandLineOption noAeroOption(QStringLiteral("no-aero"),
                                    QStringLiteral("Disable the Aero card drawing."));
    QCommandLineOption forceOpaqueOption(
            QStringLiteral("force-opaque"),
            QStringLiteral("Pretend Aero is off (test helper for the opaque fallback)."));
    QCommandLineOption toggleCompositionOption(
            QStringLiteral("toggle-composition-after"),
            QStringLiteral("Flip the simulated Aero state after <ms> and post "
                           "WM_DWMCOMPOSITIONCHANGED (test helper)."),
            QStringLiteral("ms"));
    QCommandLineOption silentCompositionOption(
            QStringLiteral("toggle-composition-silent-after"),
            QStringLiteral("Flip the simulated Aero state after <ms> without posting "
                           "the message, to test the polling fallback (test helper)."),
            QStringLiteral("ms"));
    QCommandLineOption trayOption(QStringLiteral("tray"),
                                  QStringLiteral("Start minimized in the notification area."));
    QCommandLineOption maximizeOption(QStringLiteral("maximize"),
                                      QStringLiteral("Start with the main window maximized."));
    QCommandLineOption uiTestOption(QStringLiteral("ui-test"),
                                    QStringLiteral("Show a dialog for screenshots: confirm, notice, adddevice or text."),
                                    QStringLiteral("kind"), QStringLiteral("confirm"));
    QCommandLineOption portOption(QStringLiteral("port"),
                                  QStringLiteral("Override the listening port."),
                                  QStringLiteral("port"));
    QCommandLineOption tabOption(QStringLiteral("tab"),
                                 QStringLiteral("Select a tab index on startup."),
                                 QStringLiteral("index"));
    QCommandLineOption shotOption(QStringLiteral("shot"),
                                  QStringLiteral("Save a client-area screenshot to <file> and exit."),
                                  QStringLiteral("file"));
    QCommandLineOption shotScreenOption(QStringLiteral("shot-screen"),
                                        QStringLiteral("Capture the window from the screen to <file> and exit."),
                                        QStringLiteral("file"));
    QCommandLineOption selfTestOption(QStringLiteral("selftest"),
                                      QStringLiteral("Run internal checks and write the report to <file>."),
                                      QStringLiteral("file"));
    QCommandLineOption scanOption(QStringLiteral("scan"),
                                  QStringLiteral("Probe <base>/24 for LocalSend peers, print them and exit (\"auto\" uses the primary subnet)."),
                                  QStringLiteral("base"));
    QCommandLineOption shotDelayOption(QStringLiteral("shot-delay"),
                                       QStringLiteral("Milliseconds to wait before --shot/--shot-screen captures (default 1500/1800)."),
                                       QStringLiteral("ms"));
    QCommandLineOption sendOption(QStringLiteral("send"),
                                  QStringLiteral("Send --file to <host:port> as a LocalSend peer and exit."),
                                  QStringLiteral("host:port"));
    QCommandLineOption fileOption(QStringLiteral("file"),
                                  QStringLiteral("File to send with --send."),
                                  QStringLiteral("path"));
    QCommandLineOption acceptOption(QStringLiteral("auto-accept"),
                                    QStringLiteral("Accept incoming transfers into <dir> without asking."),
                                    QStringLiteral("dir"));
    QCommandLineOption rejectOption(QStringLiteral("auto-reject"),
                                    QStringLiteral("Reject incoming transfers without asking (test helper)."));
    QCommandLineOption pinOption(QStringLiteral("pin"),
                                 QStringLiteral("PIN code to use with --send."),
                                 QStringLiteral("digits"));
    QCommandLineOption textOption(QStringLiteral("text"),
                                  QStringLiteral("Send <text> as a message with --send."),
                                  QStringLiteral("text"));
    QCommandLineOption cancelAfterOption(QStringLiteral("send-cancel-after"),
                                         QStringLiteral("Abort the --send transfer after <ms> (tests the cancel button)."),
                                         QStringLiteral("ms"));
    QCommandLineOption cancelReceiveOption(QStringLiteral("cancel-receive-after"),
                                           QStringLiteral("Abort an --auto-accept transfer after <ms>."),
                                           QStringLiteral("ms"));
    // Test helpers for the two buttons that would otherwise need a real mouse.
    QCommandLineOption addDeviceOption(QStringLiteral("add-device"),
                                       QStringLiteral("Add a peer by <host[:port]> instead of clicking the button."),
                                       QStringLiteral("address"));
    QCommandLineOption refreshAfterOption(QStringLiteral("refresh-after"),
                                          QStringLiteral("Press \"refresh list\" after <ms>."),
                                          QStringLiteral("ms"));
    parser.addOption(aeroOption);
    parser.addOption(noAeroOption);
    parser.addOption(forceOpaqueOption);
    parser.addOption(toggleCompositionOption);
    parser.addOption(silentCompositionOption);
    parser.addOption(trayOption);
    parser.addOption(maximizeOption);
    parser.addOption(uiTestOption);
    parser.addOption(portOption);
    parser.addOption(tabOption);
    parser.addOption(shotOption);
    parser.addOption(shotScreenOption);
    parser.addOption(selfTestOption);
    parser.addOption(scanOption);
    parser.addOption(shotDelayOption);
    parser.addOption(sendOption);
    parser.addOption(fileOption);
    parser.addOption(acceptOption);
    parser.addOption(rejectOption);
    parser.addOption(pinOption);
    parser.addOption(textOption);
    parser.addOption(cancelAfterOption);
    parser.addOption(cancelReceiveOption);
    parser.addOption(addDeviceOption);
    parser.addOption(refreshAfterOption);
    parser.process(app);

    Settings settings;

    // Log file next to the configuration: one line per network event, it is the
    // only way to inspect what happened on a user machine without a debugger.
    {
        const QFileInfo configInfo(Settings::configPath());
        logger::init(configInfo.absolutePath() + QStringLiteral("/lsaero.log"));
    }

    // Font selection: empty means "system default" (the shell message font),
    // otherwise the family stored under app/font_family.
    {
        QFont appFont = theme::systemUiFont();
        const QString family = settings.fontFamily();
        if (!family.isEmpty())
            appFont.setFamily(family);
        app.setFont(appFont);
    }

    // --port is a test switch: it must not be persisted, otherwise a single test
    // run would change the listening port of the installed copy.
    int portOverride = 0;
    if (parser.isSet(portOption)) {
        bool ok = false;
        const int port = parser.value(portOption).toInt(&ok);
        if (ok && port > 0 && port < 65536)
            portOverride = port;
    }

    // Language: install the translator before any widget exists, so every tr()
    // call below (titles, buttons, hints) returns the chosen language. The
    // setting only takes effect on the next start, which the settings page says.
    i18n::install(settings.language());
    logger::write(QStringLiteral("i18n: language setting=%1 active=%2")
                      .arg(settings.language())
                      .arg(i18n::activeLanguage()));

    // Two copies on the same machine normally mean "bring the existing window
    // forward". A copy that lives in another folder is a separate installation
    // though - that is how two peers are exercised on one machine - so the key
    // carries the program directory. An explicit --port still forces a unique
    // key as well.
    QString instanceKey = QString::fromLatin1(appinfo::singleInstanceKey());
    instanceKey += QStringLiteral("-")
            + QString::number(qHash(QCoreApplication::applicationDirPath().toLower()), 16);
    if (portOverride > 0)
        instanceKey += QStringLiteral("-") + QString::number(portOverride);
    SingleInstance instance(instanceKey);
    if (parser.isSet(selfTestOption))
        return runSelfTest(settings, instance, parser.value(selfTestOption));

    // A command line run must not be swallowed by a window that happens to be
    // open: --send would silently do nothing and still report success.
    const bool commandLineRun = parser.isSet(sendOption) || parser.isSet(addDeviceOption)
            || parser.isSet(acceptOption);
    if (!commandLineRun && !instance.isFirstInstance()) {
        // Another copy is already running: bring it forward and quit.
        instance.notifyFirstInstance(QStringLiteral("show"));
        return 0;
    }

    if (parser.isSet(forceOpaqueOption))
        theme::setCompositionOverride(false, true);
    const bool cardDesign = theme::cardEnabled(parser.isSet(aeroOption), parser.isSet(noAeroOption));
    MainWindow window(&settings, cardDesign);
    QObject::connect(&instance, SIGNAL(messageReceived(QString)),
                     &window, SLOT(onSecondInstanceMessage(QString)));

    // Start the listener before the window becomes visible: peers that answer
    // our announcement expect the port to accept a connection immediately.
    NetworkService network(&settings);
    if (portOverride > 0)
        network.setPortOverride(static_cast<quint16>(portOverride));
    QString networkError;
    // Only --send is headless: it drives one session and exits, and the window's
    // modal error box would keep the process alive. --auto-accept still keeps
    // the window, so the receive UI (progress, speed, footer) can be observed.
    // --auto-reject is a test helper: it must not pop a dialog either.
    const bool autoReject = parser.isSet(rejectOption);
    if (autoReject)
        network.setAutoReject(true);
    const bool headless = parser.isSet(sendOption) || autoReject;
    if (network.start(&networkError)) {
        if (!headless)
            window.setNetworkService(&network);
    } else {
        logger::write(QStringLiteral("network: start failed: ") + networkError);
    }

    if (parser.isSet(tabOption)) {
        bool ok = false;
        const int tab = parser.value(tabOption).toInt(&ok);
        if (ok)
            window.setCurrentTab(tab);
    }

    if (parser.isSet(addDeviceOption)) {
        const QString value = parser.value(addDeviceOption).trimmed();
        QString host = value;
        quint16 port = protocol::kDefaultPort;
        const int colon = value.lastIndexOf(QLatin1Char(':'));
        if (colon > 0) {
            bool portOk = false;
            const int parsed = value.mid(colon + 1).toInt(&portOk);
            if (portOk && parsed > 0 && parsed < 65536) {
                port = static_cast<quint16>(parsed);
                host = value.left(colon);
            }
        }
        QTimer::singleShot(600, &network, [&network, host, port]() {
            network.addManualDevice(host, port);
        });
    }

    if (parser.isSet(refreshAfterOption)) {
        const int delay = qMax(100, parser.value(refreshAfterOption).toInt());
        QTimer::singleShot(delay, &network, [&network]() { network.refreshDevices(); });
    }

    if (parser.isSet(scanOption)) {
        // Subnet scan smoke test: the multicast announcement is only sent when
        // a LocalSend peer opens its send tab, so the /24 probe is the reliable
        // way to find peers on the very first start.
        QString base = parser.value(scanOption);
        QStringList bases;
        if (base.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
            bases = NetworkService::localSubnetBases();
        } else if (!base.isEmpty()) {
            bases.append(base);
        }

        QTextStream out(stdout);
        out.setCodec("UTF-8");
        out << "scan bases=" << bases.join(QLatin1String(",")) << "\n" << flush;

        QList<NetworkService::Device> peers;
        for (int b = 0; b < bases.size(); ++b) {
            const QList<NetworkService::Device> found = network.scanSubnet(bases.at(b), 400);
            for (int i = 0; i < found.size(); ++i) {
                bool duplicate = false;
                for (int j = 0; j < peers.size(); ++j) {
                    if (peers.at(j).info.fingerprint == found.at(i).info.fingerprint)
                        duplicate = true;
                }
                if (!duplicate)
                    peers.append(found.at(i));
            }
        }
        for (int i = 0; i < peers.size(); ++i) {
            const NetworkService::Device &peer = peers.at(i);
            out << "peer " << peer.info.alias << " " << peer.address << ":" << peer.info.port
                << " " << peer.info.protocol << " " << peer.info.version
                << " " << peer.info.fingerprint << "\n";
        }
        out << "peers=" << peers.size() << "\n" << flush;
        return 0;
    }

    if (parser.isSet(acceptOption)) {
        // Headless receive mode: answer every offer with "accept" instead of
        // showing the confirmation dialog. The switch lives in the service so
        // the window never opens a dialog for the same offer.
        const QString dir = parser.value(acceptOption);
        bool cancelOk = false;
        const int cancelReceiveAfter = parser.value(cancelReceiveOption).toInt(&cancelOk);
        network.setAutoAccept(true, dir);

        if (cancelOk && cancelReceiveAfter > 0) {
            // Test hook: let the upload start, then abort it from the receiving
            // side once progress has been reported.
            QSharedPointer<bool> armed(new bool(false));
            QObject::connect(&network, &NetworkService::incomingProgress, &app,
                             [&network, cancelOk, cancelReceiveAfter, armed](const QString &, qint64, qint64) {
                                 Q_UNUSED(cancelOk)
                                 if (*armed)
                                     return;
                                 *armed = true;
                                 QTimer::singleShot(cancelReceiveAfter, &network, [&network, cancelReceiveAfter]() {
                                     logger::write(QStringLiteral("receive: cancel after %1 ms (test)")
                                                       .arg(cancelReceiveAfter));
                                     network.cancelIncoming();
                                 });
                             });
        }
        QObject::connect(&network, &NetworkService::incomingFinished, &app,
                         [](const QString &fileId, const QString &path, const QString &error) {
                             Q_UNUSED(fileId);
                             logger::write(error.isEmpty()
                                                   ? QStringLiteral("received %1").arg(path)
                                                   : QStringLiteral("receive failed: %1").arg(error));
                         });

    }

    if (parser.isSet(sendOption)) {
        // Headless send for the P4 acceptance run: --send host:port --file path.
        QTextStream out(stdout);
        out.setCodec("UTF-8");

        QString target = parser.value(sendOption);
        QString filePath = parser.value(fileOption);
        // Allow an explicit scheme so the plain-HTTP path can be tested too;
        // the GUI always uses the protocol announced by the peer.
        QString scheme = QStringLiteral("https");
        if (target.startsWith(QStringLiteral("http://"))) {
            scheme = QStringLiteral("http");
            target = target.mid(7);
        } else if (target.startsWith(QStringLiteral("https://"))) {
            target = target.mid(8);
        }
        const int colon = target.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0) {
            out << "send: bad arguments\n" << flush;
            return 2;
        }

        NetworkService::Device device;
        device.address = target.left(colon);
        device.info.port = target.mid(colon + 1).toInt();
        device.info.protocol = scheme;
        device.info.alias = device.address;

        QList<protocol::FileDto> files;
        QList<QString> paths;

        if (parser.isSet(textOption)) {
            // Text message: the body only travels in "preview", no temp file is
            // written (the reference client does the same for in-memory text).
            const QString text = parser.value(textOption);
            protocol::FileDto file;
            file.id = QUuid::createUuid().toString().mid(1, 36);
            file.fileName = QStringLiteral("lsaero-text-%1.txt").arg(file.id.left(8));
            file.fileType = QStringLiteral("text/plain");
            file.preview = text;
            file.size = text.toUtf8().size();
            files.append(file);
            paths.append(QString());
        } else {
            // --file also takes a folder, expanded the same way the GUI does, so
            // the folder transfer path can be exercised from a script.
            QStringList expanded;
            QStringList names;
            int emptyFolders = 0;
            protocol::expandLocalPaths(QStringList() << filePath, &expanded, &names, &emptyFolders);
            if (expanded.isEmpty()) {
                out << "send: bad arguments (no files)\n" << flush;
                return 2;
            }
            for (int i = 0; i < expanded.size(); ++i) {
                protocol::FileDto entry;
                entry.id = QUuid::createUuid().toString().mid(1, 36);
                entry.fileName = names.at(i);
                entry.fileType = entry.fileName.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive)
                        ? QStringLiteral("text/plain")
                        : QStringLiteral("application/octet-stream");
                entry.size = QFileInfo(expanded.at(i)).size();
                files.append(entry);
                paths.append(expanded.at(i));
            }
            out << "send: " << files.size() << " file(s)\n" << flush;
        }
        const QString pin = parser.value(pinOption);

        QObject::connect(&network, &NetworkService::sendFinished, &app,
                         [&out](bool ok, const QString &message) {
                             out << "send " << (ok ? "ok" : "failed") << ": " << message << "\n" << flush;
                             QCoreApplication::exit(ok ? 0 : 1);
                         });
        QObject::connect(&network, &NetworkService::pinRequired, &app,
                         [&out](const QString &alias) {
                             out << "send: pin required by " << alias << "\n" << flush;
                             QCoreApplication::exit(3);
                         });
        QObject::connect(&network, &NetworkService::sendProgress, &app,
                         [](const QString &, qint64 sent, qint64 total) {
                             static qint64 lastPercent = -1;
                             const qint64 percent = total > 0 ? sent * 100 / total : 0;
                             if (percent / 25 != lastPercent / 25) {
                                 lastPercent = percent;
                                 logger::write(QStringLiteral("send progress %1%").arg(percent));
                             }
                         });

        QTimer::singleShot(0, &network, [&network, device, files, paths, pin, filePath]() {
            // `filePath` is what the caller picked (a folder stays a folder), so
            // the history entry can replay the same transfer.
            network.sendFiles(device, files, paths, pin, QStringList() << filePath);
        });

        if (parser.isSet(cancelAfterOption)) {
            bool ok = false;
            const int after = parser.value(cancelAfterOption).toInt(&ok);
            if (ok && after > 0) {
                QTimer::singleShot(after, &network, [&network, after]() {
                    logger::write(QStringLiteral("send: cancel after %1 ms (test)").arg(after));
                    network.cancelSending();
                });
            }
        }
        return app.exec();
    }

    if (parser.isSet(trayOption))
        window.hide();
    else if (parser.isSet(maximizeOption))
        window.showMaximized();
    else
        window.show();

    if (parser.isSet(uiTestOption)) {
        // Screenshot aid: shows a dialog so the evidence pictures do not depend
        // on hitting a button with a synthetic click.
        const QString kind = parser.value(uiTestOption);
        QTimer::singleShot(900, &window, [&window, kind]() {
            if (kind == QLatin1String("notice")) {
                ui::notice(&window, QStringLiteral("发送失败"),
                           QStringLiteral("接收方拒绝了本次发送：对方正在忙。"), true);
            } else if (kind == QLatin1String("adddevice")) {
                QMetaObject::invokeMethod(&window, "addDeviceManually");
            } else if (kind == QLatin1String("text")) {
                QMetaObject::invokeMethod(&window, "sendText");
            } else {
                ui::confirm(&window, QStringLiteral("确定要删除全部传输记录吗？"), QString(),
                            QStringLiteral("清空"), QStringLiteral("取消"));
            }
        });
    }

    if (parser.isSet(toggleCompositionOption) || parser.isSet(silentCompositionOption)) {
        // Simulates a Win7 Aero <-> Basic switch on systems where composition
        // cannot be turned off. The silent variant skips the window message so
        // only the polling fallback can notice the change.
        const bool silent = parser.isSet(silentCompositionOption);
        bool ok = false;
        const int delay = (silent ? parser.value(silentCompositionOption)
                                  : parser.value(toggleCompositionOption)).toInt(&ok);
        const bool target = !theme::compositionEnabled();
        QTimer::singleShot(ok && delay > 0 ? delay : 2000, &window, [&window, target, silent]() {
            theme::setCompositionOverride(target, true);
            logger::write(QStringLiteral("theme: simulated composition %1 (%2)")
                              .arg(target ? QStringLiteral("on") : QStringLiteral("off"),
                                   silent ? QStringLiteral("silent") : QStringLiteral("message")));
            if (!silent) {
                if (QWindow *handle = window.windowHandle())
                    ::PostMessageW(reinterpret_cast<HWND>(handle->winId()), 0x0320, 0, 0);
            }
        });
    }

    int shotDelay = 0;
    if (parser.isSet(shotDelayOption)) {
        bool ok = false;
        const int value = parser.value(shotDelayOption).toInt(&ok);
        if (ok && value > 0)
            shotDelay = value;
    }

    if (parser.isSet(shotOption)) {
        // Deterministic client-area grab for the P2 evidence screenshots:
        // it renders the widget offscreen, so overlapping windows do not
        // pollute the picture.
        const QString shotPath = parser.value(shotOption);
        QTimer::singleShot(shotDelay > 0 ? shotDelay : 1500, &window, [&window, shotPath]() {
            const QPixmap shot = window.grab();
            shot.save(shotPath);
            QCoreApplication::quit();
        });
    }

    if (parser.isSet(shotScreenOption)) {
        // Screen capture (not widget grab): the Aero glass and the translucency
        // only exist on the composited desktop, so they must be read back from
        // the screen. The window is raised first, without stealing focus.
        const QString shotPath = parser.value(shotScreenOption);
        window.setWindowFlags(window.windowFlags() | Qt::WindowStaysOnTopHint);
        window.show();
        window.raise();
        QTimer::singleShot(shotDelay > 0 ? shotDelay : 1800, &window, [&window, shotPath]() {
            QScreen *screen = window.windowHandle() ? window.windowHandle()->screen()
                                                    : QGuiApplication::primaryScreen();
            if (screen) {
                // frameGeometry() includes the native Aero frame, so the
                // screenshot shows the real window border and caption.
                const QRect frame = window.frameGeometry();
                const QPixmap shot = screen->grabWindow(0, frame.x(), frame.y(),
                                                        frame.width(), frame.height());
                shot.save(shotPath);
            }
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
