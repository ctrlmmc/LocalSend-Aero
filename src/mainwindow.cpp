#include "mainwindow.h"

#include "appinfo.h"
#include "autostart.h"
#include "history.h"
#include "logger.h"
#include "networkservice.h"
#include "protocol.h"
#include "settings.h"
#include "theme.h"
#include "ui.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QClipboard>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QUrl>
#include <QTabBar>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <windows.h>

namespace {

const int kCardMargin = 6;
const int kCardRadius = 3;
const int kCardPadding = 12;
// Extra distance between the card's top edge and the tab strip.
const int kTabStripInset = 8;
// Gap between the card's bottom edge and the transfer footer.
const int kFooterGap = 8;

// Central widget: paints the translucent rounded card described in the
// requirements. The sheet around the card is cleared with
// CompositionMode_Source so that the DWM glass extended into the client area
// stays visible (this is exactly what the P1 S1 spike proved on this machine).
class CardWidget : public QWidget
{
public:
    explicit CardWidget(bool glassMode, QWidget *parent = 0)
        : QWidget(parent)
        , m_glassMode(glassMode)
        , m_anchor(0)
        , m_anchorInset(0)
        , m_bottomAnchor(0)
    {
    }

    // The card starts at the tab row, not under the header: the icon, the
    // alias and the subtitle sit directly on the glass. The inset keeps the
    // card edge where it was when the tab strip is pushed down inside it.
    void setAnchor(QWidget *anchor) { m_anchor = anchor; }
    void setAnchorInset(int inset) { m_anchorInset = inset; }
    // Bottom edge of the card follows this widget: the transfer footer sits
    // below the card, so the card stops where the page content stops.
    void setBottomAnchor(QWidget *anchor) { m_bottomAnchor = anchor; }

    // Composition can be switched off while the window is open (Windows 7
    // "basic" theme, remote session, a full-screen game). The card must then
    // stop painting a translucent sheet over glass that is no longer there and
    // fall back to the plain window colour.
    void setGlassMode(bool on)
    {
        if (m_glassMode == on)
            return;
        m_glassMode = on;
        update();
    }
    bool glassMode() const { return m_glassMode; }

protected:
    void paintEvent(QPaintEvent *)
    {
        QPainter painter(this);
        if (!m_glassMode) {
            painter.fillRect(rect(), palette().color(QPalette::Window));
            return;
        }

        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        qreal top = kCardMargin;
        if (m_anchor) {
            const QPoint anchorTop = m_anchor->mapTo(this, QPoint(0, 0));
            top = qMax<qreal>(kCardMargin, anchorTop.y() - m_anchorInset);
        }
        qreal bottom = height() - kCardMargin;
        if (m_bottomAnchor && m_bottomAnchor->height() > 0)
            bottom = m_bottomAnchor->mapTo(this, QPoint(0, m_bottomAnchor->height())).y();

        const QRectF card = QRectF(QPointF(kCardMargin, top),
                                   QPointF(width() - kCardMargin, bottom));
        QPainterPath path;
        path.addRoundedRect(card, kCardRadius, kCardRadius);
        // A cosmetic pen is exactly one device pixel: 1.0 would become two
        // pixels on a 200% display, which looked too heavy.
        painter.setPen(QPen(QColor(0, 0, 0, 0x66), 0));
        painter.setBrush(QColor(0xff, 0xff, 0xff, 0xCC));
        painter.drawPath(path);
    }

private:
    bool m_glassMode;
    QWidget *m_anchor;
    int m_anchorInset;
    QWidget *m_bottomAnchor;
};

// The strip above the card is not covered by the card, so it doubles as the
// window drag handle: the native title bar is thin and users expect to be able
// to drag by the header.
class HeaderBar : public QWidget
{
public:
    explicit HeaderBar(QWidget *parent = 0)
        : QWidget(parent)
        , m_dragging(false)
    {
    }

protected:
    void mousePressEvent(QMouseEvent *event)
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        QWidget *top = window();
        if (top->isMaximized()) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_dragging = true;
        m_offset = event->globalPos() - top->frameGeometry().topLeft();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event)
    {
        if (!m_dragging || !(event->buttons() & Qt::LeftButton)) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        window()->move(event->globalPos() - m_offset);
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event)
    {
        m_dragging = false;
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event)
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mouseDoubleClickEvent(event);
            return;
        }
        QWidget *top = window();
        if (top->isMaximized())
            top->showNormal();
        else
            top->showMaximized();
        event->accept();
    }

private:
    bool m_dragging;
    QPoint m_offset;
};

QLabel *makeHintLabel(const QString &text, QWidget *parent){
    QLabel *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: #5a5a5a;"));
    return label;
}

// Modal notice: TaskDialog on Vista and newer (the shell's own dialog), with a
// native QMessageBox fallback when comctl32 does not expose it.
void showNotice(QWidget *parent, const QString &title, const QString &text, bool warning)
{
    ui::notice(parent, title, text, warning);
}

} // namespace

MainWindow::MainWindow(Settings *settings, bool glassMode, QWidget *parent)
    : QMainWindow(parent)
    , m_settings(settings)
    , m_network(0)
    , m_glassMode(glassMode)
    , m_glassActive(false)
    , m_card(0)
    , m_glassPoll(0)
    , m_glassSwitchPending(false)
    , m_trayHintShown(false)
    , m_quitting(false)
    , m_receiving(false)
    , m_transferIconActive(false)
    , m_trayRetries(0)
    , m_offerRequestId(0)
    , m_offerDropped(false)
    , m_tray(0)
    , m_tabBar(0)
    , m_pages(0)
    , m_headerAlias(0)
    , m_headerSubtitle(0)
    , m_saveDirLabel(0)
    , m_autostartBox(0)
    , m_askBox(0)
    , m_httpsBox(0)
    , m_languageBox(0)
    , m_fontBox(0)
    , m_networkStatus(0)
    , m_deviceList(0)
    , m_pickButton(0)
    , m_folderButton(0)
    , m_sendButton(0)
    , m_textButton(0)
    , m_refreshButton(0)
    , m_addDeviceButton(0)
    , m_cancelButton(0)
    , m_fileList(0)
    , m_progress(0)
    , m_transferStatus(0)
    , m_progressTotal(0)
    , m_transferStatusTotal(0)
    , m_footer(0)
    , m_rateLastBytes(0)
    , m_rateBps(0)
    , m_totalRateLast(0)
    , m_totalRateBps(0)
    , m_multiFile(false)
    , m_aggTotal(0)
    , m_aggDone(0)
    , m_historyTree(0)
    , m_receiveWatchdog(0)
    , m_pinBox(0)
    , m_pinEdit(0)
    , m_aliasEdit(0)
    , m_whiteAliasBox(0)
    , m_whiteAliasHint(0)
{
    setWindowTitle(QString::fromLatin1(appinfo::appName()));
    setWindowIcon(theme::appIcon());
    setAcceptDrops(true);

    m_receiveWatchdog = new QTimer(this);
    m_receiveWatchdog->setSingleShot(true);
    connect(m_receiveWatchdog, SIGNAL(timeout()), this, SLOT(onReceiveStalled()));

    // The alpha attribute can only be honoured while the native window is being
    // created, so decide once here and rebuild the HWND whenever composition
    // changes at runtime (see switchGlassMode()).
    m_glassActive = m_glassMode && theme::isVistaOrLater() && theme::compositionEnabled();
    if (m_glassActive)
        setAttribute(Qt::WA_TranslucentBackground, true);

    setupUi();
    setupTray();
    loadFromSettings();

    // Windows 7 does not reliably deliver WM_DWMCOMPOSITIONCHANGED to every
    // window, and the user may switch Aero <-> Basic at any time. A cheap poll
    // of DwmIsCompositionEnabled() makes the switch work either way.
    if (m_glassMode) {
        m_glassPoll = new QTimer(this);
        m_glassPoll->setInterval(1000);
        connect(m_glassPoll, SIGNAL(timeout()), this, SLOT(applyGlassState()));
        m_glassPoll->start();
    }

    // Compact enough for small laptop panels (1280x800 and even 1024x600).
    resize(640, 460);
    setMinimumSize(560, 400);
}

MainWindow::~MainWindow()
{
}

void MainWindow::setCurrentTab(int index)
{
    if (m_tabBar && index >= 0 && index < m_tabBar->count())
        m_tabBar->setCurrentIndex(index);
}

void MainWindow::setNetworkService(NetworkService *service)
{
    m_network = service;
    if (!m_network)
        return;

    connect(m_network, &NetworkService::devicesChanged, this, &MainWindow::refreshDevices);
    connect(m_network, &NetworkService::statusChanged, this, [this](const QString &line) {
        if (m_networkStatus)
            m_networkStatus->setText(line);
    });
    connect(m_network, &NetworkService::sendProgress, this, &MainWindow::onSendProgress);
    connect(m_network, &NetworkService::sendFinished, this, &MainWindow::onSendFinished);
    connect(m_network, &NetworkService::pinRequired, this, &MainWindow::onPinRequired);
    connect(m_network, &NetworkService::manualDeviceFailed, this,
            [this](const QString &host, quint16 port) {
        ui::notice(this, tr("无法连接到 %1:%2").arg(host).arg(port),
                   tr("请确认对方已经运行、地址正确，并且两台电脑在同一网络。"),
                   true);
    });
    connect(m_network, &NetworkService::manualDeviceAdded, this, [this](const QString &alias) {
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("已添加设备 %1。").arg(alias));
        }
    });
    connect(m_network, &NetworkService::incomingOffer, this, &MainWindow::onIncomingOffer);
    connect(m_network, &NetworkService::incomingAccepted, this, [this](int files, qint64 bytes) {
        // Service-level accept (--auto-accept) and the dialog path both land
        // here; beginAggregate() is idempotent, so the dialog's own call is fine.
        beginAggregate(bytes, files);
    });
    connect(m_network, &NetworkService::incomingOfferDropped, this, [this](int requestId) {
        // A queued offer that got withdrawn before its dialog came up must not
        // be answered later: the sender is gone and answering used to crash.
        for (int i = m_offerQueue.size() - 1; i >= 0; --i) {
            if (m_offerQueue.at(i).requestId == requestId)
                m_offerQueue.removeAt(i);
        }
        // The sender closed the window while the offer was on screen; the
        // dialog has nothing left to answer, so close it and remember that the
        // request id is already gone.
        if (requestId != m_offerRequestId)
            return;
        m_offerDropped = true;
        if (m_offerDialog)
            m_offerDialog->reject();
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("对方取消了发送。"));
        }
        m_receiving = false;
        updateSendState();
    });
    connect(m_network, &NetworkService::incomingProgress, this,
            [this](const QString &fileId, qint64 received, qint64 total) {
        if (!m_progress)
            return;
        if (m_receiveWatchdog)
            m_receiveWatchdog->start(30000);
        m_progress->setVisible(true);
        if (total > 0)
            m_progress->setValue(static_cast<int>(received * 100 / total));
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("正在接收 %1 / %2")
                                          .arg(formatBytes(received), formatBytes(total))
                                      + rateSuffix(fileId, received, total));
        }
        updateAggregate(fileId, received);
    });
    connect(m_network, &NetworkService::incomingCompleted, this,
            [this](const QString &peer, int files, qint64 bytes) {
        m_receiving = false;
        resetRate();
        endAggregate();
        if (m_receiveWatchdog)
            m_receiveWatchdog->stop();
        if (m_progress) {
            m_progress->setValue(100);
            m_progress->setVisible(false);
        }
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("已从 %1 接收 %2 个文件 (共 %3)")
                                          .arg(peer)
                                          .arg(files)
                                          .arg(formatBytes(bytes)));
        }
        updateSendState();
        refreshHistory();
    });
    connect(m_network, &NetworkService::incomingFinished, this, [this](const QString &, const QString &, const QString &error) {
        if (error.isEmpty())
            return;
        // An aborted transfer must not leave a half-filled progress bar behind.
        m_receiving = false;
        endAggregate();
        if (m_receiveWatchdog)
            m_receiveWatchdog->stop();
        if (m_progress) {
            m_progress->setValue(0);
            m_progress->setVisible(false);
        }
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("接收中断：%1").arg(error));
        }
        updateSendState();
        // NetworkService writes the history entry before it emits, so the table
        // must be rebuilt here too - otherwise row -> entry mapping shifts and
        // the context menu acts on the wrong transfer.
        refreshHistory();
    });
    connect(m_network, &NetworkService::incomingCancelled, this, [this](const QString &address) {
        Q_UNUSED(address);
        m_receiving = false;
        endAggregate();
        if (m_receiveWatchdog)
            m_receiveWatchdog->stop();
        if (m_progress) {
            m_progress->setValue(0);
            m_progress->setVisible(false);
        }
        updateSendState();
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("对方取消了传输，未完成的文件已丢弃。"));
        }
        refreshHistory();
    });

    if (m_networkStatus)
        m_networkStatus->setText(m_network->statusLine());
    refreshDevices();
}

void MainWindow::setupUi()
{
    CardWidget *card = new CardWidget(m_glassActive, this);
    m_card = card;
    setCentralWidget(card);

    QVBoxLayout *outer = new QVBoxLayout(card);
    const int inset = m_glassMode ? (kCardMargin + kCardPadding) : kCardPadding;
    outer->setContentsMargins(0, 0, 0, kCardMargin);
    outer->setSpacing(0);

    // Header: application icon, device alias and a one-line description. It
    // lives outside the card (on the glass) and doubles as a drag handle.
    HeaderBar *headerBar = new HeaderBar(card);
    QHBoxLayout *header = new QHBoxLayout(headerBar);
    // The header sits on the glass with a 16 px top and an 8 px bottom inset.
    header->setContentsMargins(inset, 16, inset, 8);
    header->setSpacing(14);

    QLabel *iconLabel = new QLabel(headerBar);
    iconLabel->setPixmap(theme::appIcon().pixmap(48, 48));
    iconLabel->setFixedSize(48, 48);
    header->addWidget(iconLabel);

    QVBoxLayout *titles = new QVBoxLayout;
    titles->setSpacing(2);

    m_headerAlias = new QLabel(headerBar);
    QFont aliasFont = QApplication::font();
    const qreal aliasSize = aliasFont.pointSizeF();
    if (aliasSize > 0)
        aliasFont.setPointSizeF(aliasSize + 4.5);
    aliasFont.setBold(true);
    m_headerAlias->setFont(aliasFont);

    m_headerSubtitle = makeHintLabel(tr("在局域网内与 LocalSend 设备互传文件"), headerBar);

    titles->addWidget(m_headerAlias);
    titles->addWidget(m_headerSubtitle);
    header->addLayout(titles, 1);

    outer->addWidget(headerBar);

    // A plain QTabBar + QStackedWidget instead of QTabWidget: QTabWidget paints
    // an opaque "pane" behind its pages (and styling it transparent makes Qt
    // switch the child buttons to the non-native stylesheet renderer). With the
    // stack the pages stay transparent, so the translucent card shows through,
    // while every control keeps its native windowsvista look.
    QWidget *cardArea = new QWidget(card);
    QVBoxLayout *cardLayout = new QVBoxLayout(cardArea);
    // The tab strip sits 8 px lower inside the card than its top edge; the card
    // itself keeps its position (setAnchorInset below compensates).
    cardLayout->setContentsMargins(inset, 4 + kTabStripInset, inset, inset);
    cardLayout->setSpacing(6);

    m_tabBar = new QTabBar(cardArea);
    m_tabBar->addTab(tr("发送"));
    m_tabBar->addTab(tr("历史"));
    m_tabBar->addTab(tr("设置"));
    m_tabBar->setExpanding(false);
    m_tabBar->setFocusPolicy(Qt::NoFocus);

    m_pages = new QStackedWidget(cardArea);
    m_pages->addWidget(createSendTab());
    m_pages->addWidget(createHistoryTab());
    m_pages->addWidget(createSettingsTab());

    connect(m_tabBar, &QTabBar::currentChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_tabBar, &QTabBar::currentChanged, this, [this](int index) {
        if (index == 1)
            refreshHistory();
    });

    cardLayout->addWidget(m_tabBar);
    cardLayout->addWidget(m_pages, 1);
    outer->addWidget(cardArea, 1);

    // Transfer footer: progress bar and status hint live *below* the card, on
    // the glass, with an 8 px gap. It only takes room while a transfer runs.
    m_footer = new QWidget(card);
    QVBoxLayout *footerLayout = new QVBoxLayout(m_footer);
    footerLayout->setContentsMargins(inset, kFooterGap, inset, 0);
    footerLayout->setSpacing(4);

    m_progress = new QProgressBar(m_footer);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setVisible(false);
    footerLayout->addWidget(m_progress);

    m_transferStatus = makeHintLabel(QString(), m_footer);
    // The footer sits on the glass, not on the 80% white card, so the default
    // #5a5a5a hint colour is too washed out there.
    m_transferStatus->setStyleSheet(QStringLiteral("color: #1f1f1f;"));
    m_transferStatus->setVisible(false);
    footerLayout->addWidget(m_transferStatus);

    // Aggregate row: only a multi-file transfer has anything to aggregate, so
    // both widgets stay hidden for single files and for text messages.
    m_progressTotal = new QProgressBar(m_footer);
    m_progressTotal->setRange(0, 100);
    m_progressTotal->setValue(0);
    m_progressTotal->setVisible(false);
    footerLayout->addWidget(m_progressTotal);

    m_transferStatusTotal = makeHintLabel(QString(), m_footer);
    m_transferStatusTotal->setStyleSheet(QStringLiteral("color: #1f1f1f;"));
    m_transferStatusTotal->setVisible(false);
    footerLayout->addWidget(m_transferStatusTotal);

    m_footer->setVisible(false);
    outer->addWidget(m_footer);

    m_progress->installEventFilter(this);
    m_transferStatus->installEventFilter(this);
    m_progressTotal->installEventFilter(this);
    m_transferStatusTotal->installEventFilter(this);

    card->setAnchor(m_tabBar);
    card->setAnchorInset(kTabStripInset);
    card->setBottomAnchor(cardArea);
}

QWidget *MainWindow::createSendTab()
{
    QWidget *page = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 14, 12, 12);
    layout->setSpacing(8);

    QLabel *title = new QLabel(tr("发送文件"), page);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    layout->addWidget(makeHintLabel(
        tr("选择文件或把文件拖进窗口，也可以发一段文字；附近的 LocalSend 设备会出现在下面的列表里。"),
        page));

    m_pickButton = new QPushButton(tr("选择文件"), page);
    connect(m_pickButton, SIGNAL(clicked()), this, SLOT(pickFiles()));

    m_folderButton = new QPushButton(tr("选择文件夹"), page);
    connect(m_folderButton, SIGNAL(clicked()), this, SLOT(pickFolder()));

    m_textButton = new QPushButton(tr("发送文本"), page);
    connect(m_textButton, SIGNAL(clicked()), this, SLOT(sendText()));

    m_refreshButton = new QPushButton(tr("刷新列表"), page);
    connect(m_refreshButton, SIGNAL(clicked()), this, SLOT(refreshDevices()));
    // A manual refresh also verifies the peers we already show (so a closed peer
    // disappears at once) and announces, so UDP-only peers learn about us right
    // away instead of waiting for the next 5 s keep-alive.
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        if (m_network)
            m_network->refreshDevices();
    });

    m_addDeviceButton = new QPushButton(tr("添加设备"), page);
    connect(m_addDeviceButton, SIGNAL(clicked()), this, SLOT(addDeviceManually()));

    m_sendButton = new QPushButton(tr("发送"), page);
    m_sendButton->setEnabled(false);
    connect(m_sendButton, SIGNAL(clicked()), this, SLOT(sendSelected()));

    m_cancelButton = new QPushButton(tr("取消"), page);
    m_cancelButton->setVisible(false);
    connect(m_cancelButton, SIGNAL(clicked()), this, SLOT(cancelSend()));

    QHBoxLayout *row = new QHBoxLayout;
    row->addWidget(m_pickButton);
    row->addWidget(m_folderButton);
    row->addWidget(m_textButton);
    row->addWidget(m_refreshButton);
    row->addWidget(m_addDeviceButton);
    row->addStretch(1);
    row->addWidget(m_cancelButton);
    row->addWidget(m_sendButton);
    layout->addLayout(row);

    m_fileList = new QListWidget(page);
    m_fileList->setUniformItemSizes(true);
    m_fileList->setMaximumHeight(64);
    m_fileList->setStyleSheet(QStringLiteral("QListWidget { background: rgba(255, 255, 255, 230); }"));
    m_fileList->setVisible(false);
    layout->addWidget(m_fileList);

    m_networkStatus = makeHintLabel(tr("网络未启动"), page);
    layout->addWidget(m_networkStatus);

    m_deviceList = new QListWidget(page);
    m_deviceList->setUniformItemSizes(true);
    m_deviceList->setAlternatingRowColors(false);
    // A white list box keeps the entries readable over the translucent card;
    // only the background is forced, the frame stays native.
    m_deviceList->setStyleSheet(QStringLiteral("QListWidget { background: rgba(255, 255, 255, 230); }"));
    connect(m_deviceList, &QListWidget::itemSelectionChanged, this, [this]() { updateSendState(); });
    layout->addWidget(m_deviceList, 1);

    refreshDevices();
    return page;
}

void MainWindow::updateSendState()
{
    // Safety net: a stalled or aborted transfer left the bar filling the footer
    // while nothing was running any more. Whatever path got us here, an idle
    // window never shows the progress bar.
    if (m_progress && !(m_network && m_network->sending()) && !m_receiving
        && m_progress->isVisible()) {
        m_progress->setValue(0);
        m_progress->setVisible(false);
    }

    // Every transfer transition ends up here, so this is also where the footer
    // strip learns whether the progress bar / status hint need room. Relying on
    // the widgets' Show events alone is not enough: a child of a hidden parent
    // never receives Show, so the footer would stay collapsed forever.
    updateFooterVisibility();

    const bool hasFiles = !m_selectedPaths.isEmpty() || !m_textBody.isEmpty();
    const bool hasDevice = m_deviceList && m_deviceList->currentItem() != 0
            && (m_deviceList->currentItem()->flags() & Qt::ItemIsEnabled);
    // m_receiving covers the dialog path; NetworkService::receiving() also
    // covers transfers accepted without a dialog (--auto-accept) and is the
    // authoritative "bytes are moving" flag for the icon swap.
    const bool receivingNow = m_receiving || (m_network && m_network->receiving());
    const bool busy = (m_network && m_network->sending()) || receivingNow;
    updateTransferIcon(busy);

    if (m_sendButton)
        m_sendButton->setEnabled(hasFiles && hasDevice && !busy);
    // While a transfer runs the whole send tab is frozen: starting a second
    // session or changing the selection in the middle of one used to leave the
    // UI in a state that no longer matched the network.
    if (m_pickButton)
        m_pickButton->setEnabled(!busy);
    if (m_folderButton)
        m_folderButton->setEnabled(!busy);
    if (m_textButton)
        m_textButton->setEnabled(!busy);
    if (m_refreshButton)
        m_refreshButton->setEnabled(!busy);
    if (m_addDeviceButton)
        m_addDeviceButton->setEnabled(!busy);
    if (m_deviceList)
        m_deviceList->setEnabled(!busy);
    if (m_cancelButton) {
        const bool sending = m_network && m_network->sending();
        // The same button aborts an outbound transfer and an inbound one: both
        // used to be uncancellable once they started.
        m_cancelButton->setVisible(sending || receivingNow);
        m_cancelButton->setText(sending ? tr("取消发送") : tr("取消接收"));
    }

    // The listening port / device count line is for the idle view. During a
    // transfer the footer already carries the progress text, and the two used
    // to sit on top of each other.
    if (m_networkStatus)
        m_networkStatus->setVisible(!busy);
}

void MainWindow::addDeviceManually()
{
    if (!m_network)
        return;

    // QInputDialog::getText() cannot grey out its OK button, and an empty
    // address is a guaranteed failure, so the dialog is built by hand.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("添加设备"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 12);
    layout->setSpacing(10);
    layout->addWidget(new QLabel(tr("输入对方的 IP 地址（可带端口，如 192.168.1.5:53317）"), &dialog));

    QLineEdit *edit = new QLineEdit(&dialog);
    edit->setMinimumWidth(280);
    layout->addWidget(edit);

    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    QPushButton *cancel = new QPushButton(tr("取消"), &dialog);
    QPushButton *ok = new QPushButton(tr("确定"), &dialog);
    ok->setDefault(true);
    ok->setEnabled(false);
    buttons->addWidget(cancel);
    buttons->addWidget(ok);
    layout->addLayout(buttons);

    connect(edit, &QLineEdit::textChanged, this, [ok](const QString &text) {
        ok->setEnabled(!text.trimmed().isEmpty());
    });
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(edit, &QLineEdit::returnPressed, ok, &QPushButton::click);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString value = edit->text().trimmed();
    if (value.isEmpty())
        return;

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
    m_network->addManualDevice(host, port);
}

void MainWindow::pickFiles()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("选择要发送的文件"));
    if (paths.isEmpty())
        return;
    addFiles(paths);
}

void MainWindow::pickFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("选择要发送的文件夹"));
    if (dir.isEmpty())
        return;
    addFiles(QStringList() << dir);
}

void MainWindow::addFiles(const QStringList &paths)
{
    if (paths.isEmpty())
        return;

    // Folders are expanded here: the protocol has no directory object, the
    // reference client sends one entry per file with a "folder/sub/file" name
    // and the receiver recreates the tree from that name.
    QStringList files;
    QStringList names;
    int skippedFolders = 0;
    protocol::expandLocalPaths(paths, &files, &names, &skippedFolders);

    if (files.isEmpty()) {
        if (skippedFolders > 0)
            ui::notice(this, tr("空文件夹"), tr("这个文件夹里没有可以发送的文件。"), false);
        return;
    }

    m_textBody.clear();
    m_textFileName.clear();
    m_textPreview.clear();

    m_selectedPaths = files;
    m_selectedNames = names;
    m_selectedRoots = paths;
    m_fileList->clear();
    for (int i = 0; i < files.size(); ++i) {
        const QFileInfo info(files.at(i));
        new QListWidgetItem(tr("%1 (%2 KB)").arg(names.at(i))
                                .arg(QString::number((info.size() + 1023) / 1024)), m_fileList);
    }
    m_fileList->setVisible(true);
    updateSendState();
}

void MainWindow::addTextMessage(const QString &text)
{
    if (text.trimmed().isEmpty())
        return;

    // Same convention as the reference client: one "file" whose body also sits
    // in "preview", so the peer can show it as a message bubble. The bytes stay
    // in memory; writing them to %TEMP% failed on machines where that directory
    // is not writable and the whole feature was unusable there.
    m_selectedPaths.clear();
    m_selectedRoots.clear();
    m_textBody = text.toUtf8();
    m_textPreview = text;
    m_textFileName = QStringLiteral("lsaero-text-%1.txt")
            .arg(QUuid::createUuid().toString().mid(1, 8));

    m_fileList->clear();
    const QString label = text.simplified();
    new QListWidgetItem(tr("文本消息：%1").arg(label.left(40) + (label.size() > 40 ? QStringLiteral("…") : QString())),
                        m_fileList);
    m_fileList->setVisible(true);
    updateSendState();
}

void MainWindow::sendText()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("发送文本"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 12);
    layout->setSpacing(10);
    layout->addWidget(new QLabel(tr("文本将作为一条消息发送给选中的设备。"), &dialog));

    QPlainTextEdit *edit = new QPlainTextEdit(&dialog);
    edit->setMinimumSize(320, 120);
    layout->addWidget(edit);

    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    QPushButton *cancel = new QPushButton(tr("取消"), &dialog);
    QPushButton *ok = new QPushButton(tr("完成"), &dialog);
    ok->setDefault(true);
    // Whitespace-only text is not a message; keep the button dead until there
    // is something to send.
    ok->setEnabled(false);
    buttons->addWidget(cancel);
    buttons->addWidget(ok);
    layout->addLayout(buttons);

    connect(edit, &QPlainTextEdit::textChanged, this, [edit, ok]() {
        ok->setEnabled(!edit->toPlainText().trimmed().isEmpty());
    });
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);

    edit->setFocus();

    if (dialog.exec() != QDialog::Accepted)
        return;
    addTextMessage(edit->toPlainText());
}

void MainWindow::cancelSend()
{
    if (!m_network)
        return;

    if (m_network->sending()) {
        m_network->cancelSending();
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("正在取消发送…"));
        }
        updateSendState();
        return;
    }

    if (m_receiving || m_network->receiving()) {
        m_network->cancelIncoming();
        m_receiving = false;
        endAggregate();
        updateSendState();
        if (m_progress)
            m_progress->setVisible(false);
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("已取消接收，未完成的文件已丢弃。"));
        }
    }
}

void MainWindow::sendSelected()
{
    if (!m_network || !m_deviceList || !m_deviceList->currentItem())
        return;

    // Look the device up by fingerprint: the row index is only valid for the
    // list that painted it, and the device map can change between a click and
    // this slot (a peer dropping off, a new one being found).
    const QString fingerprint = m_deviceList->currentItem()->data(Qt::UserRole).toString();
    const QList<NetworkService::Device> devices = m_network->devices();
    int row = -1;
    for (int i = 0; i < devices.size(); ++i) {
        if (devices.at(i).info.fingerprint == fingerprint) {
            row = i;
            break;
        }
    }
    if (row < 0) {
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("所选设备已离线，请刷新列表后重试。"));
        }
        return;
    }

    m_pendingTarget = devices.at(row);
    m_pendingFiles.clear();
    m_pendingPaths.clear();

    if (!m_textBody.isEmpty()) {
        protocol::FileDto file;
        file.id = QUuid::createUuid().toString().mid(1, 36);
        file.fileName = m_textFileName;
        file.size = m_textBody.size();
        file.fileType = QStringLiteral("text/plain");
        file.preview = m_textPreview;
        m_pendingFiles.append(file);
        m_pendingPaths.append(QString());   // body comes from memory
    } else {
        for (int i = 0; i < m_selectedPaths.size(); ++i) {
            const QFileInfo info(m_selectedPaths.at(i));
            // Folder picks keep their "folder/sub/file" name, so the receiver
            // can rebuild the tree; plain picks use the bare file name.
            const QString name = m_selectedNames.value(i, info.fileName());
            protocol::FileDto file;
            file.id = QUuid::createUuid().toString().mid(1, 36);
            file.fileName = name;
            file.size = info.size();
            file.fileType = name.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive)
                    ? QStringLiteral("text/plain")
                    : QStringLiteral("application/octet-stream");
            m_pendingFiles.append(file);
            m_pendingPaths.append(m_selectedPaths.at(i));
        }
    }

    resetRate();
    qint64 aggBytes = 0;
    for (int i = 0; i < m_pendingFiles.size(); ++i)
        aggBytes += m_pendingFiles.at(i).size;
    beginAggregate(aggBytes, m_pendingFiles.size());
    m_progress->setVisible(true);
    m_progress->setValue(0);
    m_transferStatus->setVisible(true);
    m_transferStatus->setText(tr("正在与 %1 建立会话…").arg(m_pendingTarget.info.alias));

    // sendFiles() only starts the worker thread and returns at once, so the
    // busy state (and the cancel button) can be applied right after it.
    m_network->sendFiles(m_pendingTarget, m_pendingFiles, m_pendingPaths, QString(), m_selectedRoots);
    updateSendState();
}

void MainWindow::onPinRequired(const QString &alias)
{
    bool ok = false;
    const QString pin = QInputDialog::getText(this, tr("需要 PIN 码"),
                                              tr("%1 要求输入 PIN 码：").arg(alias),
                                              QLineEdit::Normal, QString(), &ok);
    if (!ok) {
        // The service already dropped the session and will not emit
        // sendFinished for a "needs PIN" result, so the page has to be reset
        // here - otherwise every control stays greyed out.
        resetRate();
        if (m_progress) {
            m_progress->setValue(0);
            m_progress->setVisible(false);
        }
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("已取消发送"));
        }
        updateSendState();
        return;
    }

    resetRate();
    m_progress->setVisible(true);
    m_transferStatus->setVisible(true);
    m_transferStatus->setText(tr("正在使用 PIN 码重试…"));
    m_network->sendFiles(m_pendingTarget, m_pendingFiles, m_pendingPaths, pin.trimmed(), m_selectedRoots);
    updateSendState();
}

void MainWindow::onSendProgress(const QString &fileId, qint64 sent, qint64 total)
{
    if (!m_progress)
        return;
    if (total > 0)
        m_progress->setValue(static_cast<int>(sent * 100 / total));
    if (m_transferStatus)
        m_transferStatus->setText(tr("已发送 %1 / %2").arg(formatBytes(sent), formatBytes(total))
                                  + rateSuffix(fileId, sent, total));
    updateAggregate(fileId, sent);
}

void MainWindow::onSendFinished(bool ok, const QString &message)
{
    resetRate();
    endAggregate();
    if (m_progress) {
        m_progress->setValue(ok ? 100 : 0);
        m_progress->setVisible(false);
    }
    if (m_transferStatus) {
        m_transferStatus->setText(message);
        m_transferStatus->setVisible(true);
    }
    updateSendState();
    refreshHistory();

    // Cancels are already visible in the status line, and the "still receiving"
    // guard is only a safety net (the buttons are disabled while receiving):
    // neither deserves a modal dialog.
    if (!ok && message != tr("已取消发送") && message != tr("接收者已取消")
        && message != tr("正在接收文件，请等接收完成后再发送"))
        showNotice(this, tr("发送失败"), message, true);
}

void MainWindow::onIncomingOffer(const NetworkService::IncomingOffer &offer)
{
    // The reference app forces itself to the front when a transfer arrives.
    showFromTray();

    // Never run a modal dialog inside the socket's readyRead handler: while a
    // blocking exec() sits on that stack the server does not notice the sender
    // hanging up, so the dialog stayed on screen for a request that no longer
    // existed (and answering it crashed). Hop to the event loop first.
    const NetworkService::IncomingOffer copy = offer;
    QTimer::singleShot(0, this, [this, copy]() { openOfferDialog(copy); });
}

void MainWindow::openOfferDialog(const NetworkService::IncomingOffer &offer)
{
    // One question at a time. Two senders arriving together used to open two
    // nested exec() dialogs that fought over the single m_offerDialog slot, and
    // leaving a dialog on screen for a request that no longer existed.
    if (m_offerDialog) {
        m_offerQueue.append(offer);
        return;
    }

    openOfferDialogNow(offer);

    // Each dialog below is modal, so this loop runs once per completed answer.
    while (!m_offerQueue.isEmpty() && !m_quitting)
        openOfferDialogNow(m_offerQueue.takeFirst());
}

void MainWindow::openOfferDialogNow(const NetworkService::IncomingOffer &offer)
{
    const bool textOnly = offer.files.size() == 1
            && !offer.files.first().preview.isEmpty()
            && offer.files.first().fileName.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive);
    if (textOnly) {
        offerTextMessage(offer);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("接收文件"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 12);
    layout->setSpacing(10);

    QLabel *headline = new QLabel(tr("%1 想向本机发送 %2 个文件")
                                      .arg(offer.sender.alias)
                                      .arg(offer.files.size()),
                                  &dialog);
    QFont headlineFont = headline->font();
    headlineFont.setBold(true);
    headline->setFont(headlineFont);
    layout->addWidget(headline);

    QListWidget *list = new QListWidget(&dialog);
    list->setUniformItemSizes(true);
    for (int i = 0; i < offer.files.size(); ++i) {
        const protocol::FileDto &file = offer.files.at(i);
        if (!file.preview.isEmpty()) {
            // Text message: the body travels in "preview", show it as such.
            QListWidgetItem *item = new QListWidgetItem(
                tr("文本消息：%1").arg(file.preview.simplified().left(120)), list);
            item->setToolTip(file.preview);
        } else {
            new QListWidgetItem(tr("%1 (%2 KB)").arg(file.fileName)
                                    .arg(QString::number((file.size + 1023) / 1024)), list);
        }
    }
    list->setMaximumHeight(140);
    layout->addWidget(list);

    // "每次接收前询问保存位置" off: reuse the configured folder and only ask
    // whether to accept, otherwise the sender would have to answer twice.
    QLineEdit *dirEdit = 0;
    if (m_settings->askEveryTime()) {
        layout->addWidget(new QLabel(tr("保存到："), &dialog));
        QHBoxLayout *dirRow = new QHBoxLayout;
        dirEdit = new QLineEdit(m_settings->saveDir(), &dialog);
        QPushButton *browse = new QPushButton(tr("更改"), &dialog);
        connect(browse, &QPushButton::clicked, &dialog, [this, dirEdit]() {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("选择保存位置"),
                                                                 dirEdit->text());
            if (!dir.isEmpty())
                dirEdit->setText(dir);
        });
        dirRow->addWidget(dirEdit, 1);
        dirRow->addWidget(browse);
        layout->addLayout(dirRow);
    } else {
        QLabel *destination = new QLabel(
                tr("将保存到 %1").arg(QDir::toNativeSeparators(m_settings->saveDir())), &dialog);
        destination->setStyleSheet(QStringLiteral("color: #5a5a5a;"));
        layout->addWidget(destination);
    }

    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    QPushButton *decline = new QPushButton(tr("拒绝"), &dialog);
    QPushButton *accept = new QPushButton(tr("接受"), &dialog);
    accept->setDefault(true);
    buttons->addWidget(decline);
    buttons->addWidget(accept);
    layout->addLayout(buttons);

    connect(decline, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(accept, &QPushButton::clicked, &dialog, &QDialog::accept);

    // While this dialog is open the sender may hang up. Remember which offer it
    // belongs to so incomingOfferDropped() can close it and so that we never try
    // to answer a request that no longer exists.
    m_offerRequestId = offer.requestId;
    m_offerDropped = false;
    m_offerDialog = &dialog;

    const bool accepted = dialog.exec() == QDialog::Accepted;
    const bool dropped = m_offerDropped;
    const QString saveDir = dirEdit ? dirEdit->text() : m_settings->saveDir();

    m_offerDialog = 0;
    m_offerRequestId = 0;
    m_offerDropped = false;

    if (dropped) {
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("对方取消了发送。"));
        }
        updateSendState();
        return;
    }

    m_network->respondToOffer(offer.requestId, accepted, saveDir);
    if (accepted) {
        m_receiving = true;
        resetRate();
        qint64 aggBytes = 0;
        for (int i = 0; i < offer.files.size(); ++i)
            aggBytes += offer.files.at(i).size;
        beginAggregate(aggBytes, offer.files.size());
        if (m_progress) {
            m_progress->setVisible(true);
            m_progress->setValue(0);
        }
        updateSendState();
        m_transferStatus->setVisible(true);
        m_transferStatus->setText(tr("正在接收 %1 个文件到 %2")
                                      .arg(offer.files.size())
                                      .arg(QDir::toNativeSeparators(saveDir)));
        // The sender may vanish right after we accepted (crash, cable, or its
        // user cancels): without a watchdog the window would keep claiming
        // "正在接收" and every later send would stay blocked.
        if (m_receiveWatchdog)
            m_receiveWatchdog->start(30000);
    }
}

void MainWindow::onReceiveStalled()
{
    if (!m_receiving && !(m_network && m_network->receiving()))
        return;
    m_receiving = false;
    endAggregate();
    // Dropping only the UI flag left NetworkService::receiving() true forever,
    // which kept the whole send page disabled until the app was restarted.
    if (m_network)
        m_network->cancelIncoming();
    if (m_progress)
        m_progress->setVisible(false);
    if (m_transferStatus) {
        m_transferStatus->setVisible(true);
        m_transferStatus->setText(tr("接收超时：发送方没有继续传输数据。"));
    }
    updateSendState();
    logger::write(QStringLiteral("receive: watchdog stopped a stalled session"));
}

void MainWindow::offerTextMessage(const NetworkService::IncomingOffer &offer)
{
    // A text message is not a file: there is nothing to save and asking for a
    // directory made no sense. Show the body, offer "复制", and treat "关闭" as
    // a decline (the same 204 the reference client sends).
    QDialog dialog(this);
    dialog.setWindowTitle(tr("接收文本"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 12);
    layout->setSpacing(10);

    QLabel *headline = new QLabel(tr("%1 发来一条文本消息").arg(offer.sender.alias), &dialog);
    QFont headlineFont = headline->font();
    headlineFont.setBold(true);
    headline->setFont(headlineFont);
    layout->addWidget(headline);

    QPlainTextEdit *body = new QPlainTextEdit(offer.files.first().preview, &dialog);
    body->setReadOnly(true);
    body->setMinimumSize(360, 160);
    layout->addWidget(body);

    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    QPushButton *copy = new QPushButton(tr("复制"), &dialog);
    QPushButton *close = new QPushButton(tr("关闭"), &dialog);
    close->setDefault(true);
    buttons->addWidget(copy);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    bool copied = false;
    // "复制" only copies: the message is never accepted/stored, so it takes the
    // same decline path as "关闭". Asking the sender to upload a text we already
    // showed the user made the peer report a transfer it never needed.
    connect(copy, &QPushButton::clicked, &dialog, [body, &copied, &dialog]() {
        QApplication::clipboard()->setText(body->toPlainText());
        copied = true;
        dialog.reject();
    });
    connect(close, &QPushButton::clicked, &dialog, [&dialog]() { dialog.reject(); });

    m_offerRequestId = offer.requestId;
    m_offerDropped = false;
    m_offerDialog = &dialog;

    dialog.exec();
    const bool dropped = m_offerDropped;

    m_offerDialog = 0;
    m_offerRequestId = 0;
    m_offerDropped = false;

    if (dropped) {
        if (m_transferStatus) {
            m_transferStatus->setVisible(true);
            m_transferStatus->setText(tr("对方取消了发送。"));
        }
        updateSendState();
        return;
    }

    m_network->respondToOffer(offer.requestId, false, QString());
    if (m_transferStatus) {
        m_transferStatus->setText(copied ? tr("文本已复制到剪贴板。") : QString());
        m_transferStatus->setVisible(copied);
    }
}

void MainWindow::refreshDevices()
{
    if (!m_deviceList)
        return;

    // Rebuilding the list used to drop the selection (and leave "发送" enabled
    // for a device that was no longer selected). The row index cannot be used
    // here: devicesChanged arrives after the device map changed, so the row
    // would point at whatever now sorts into that position - the item itself
    // carries the fingerprint for exactly this reason.
    QString selectedFingerprint;
    if (m_deviceList->currentItem())
        selectedFingerprint = m_deviceList->currentItem()->data(Qt::UserRole).toString();

    m_deviceList->clear();

    if (!m_network || m_network->deviceCount() == 0) {
        QListWidgetItem *placeholder = new QListWidgetItem(tr("正在搜索附近的设备…"), m_deviceList);
        placeholder->setFlags(Qt::NoItemFlags);
        updateSendState();
        return;
    }

    const QList<NetworkService::Device> devices = m_network->devices();
    int restoreRow = -1;
    for (int i = 0; i < devices.size(); ++i) {
        const NetworkService::Device &device = devices.at(i);
        const QString text = tr("%1 — %2 (%3, 端口 %4)")
                                 .arg(device.info.alias, device.address,
                                      device.info.protocol.toUpper())
                                 .arg(device.info.port);
        QListWidgetItem *item = new QListWidgetItem(text, m_deviceList);
        item->setData(Qt::UserRole, device.info.fingerprint);
        item->setToolTip(tr("指纹：%1\n版本：%2\n设备类型：%3")
                             .arg(device.info.fingerprint, device.info.version,
                                  device.info.deviceType));
        if (!selectedFingerprint.isEmpty() && device.info.fingerprint == selectedFingerprint)
            restoreRow = i;
    }

    if (restoreRow >= 0)
        m_deviceList->setCurrentRow(restoreRow);

    updateSendState();
}

QWidget *MainWindow::createHistoryTab()
{
    QWidget *page = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 14, 12, 12);
    layout->setSpacing(8);

    QLabel *title = new QLabel(tr("传输历史"), page);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    layout->addWidget(makeHintLabel(tr("最近 200 条收发记录。"), page));

    m_historyTree = new QTreeWidget(page);
    m_historyTree->setRootIsDecorated(false);
    m_historyTree->setAlternatingRowColors(true);
    m_historyTree->setUniformRowHeights(true);
    m_historyTree->setStyleSheet(QStringLiteral("QTreeWidget { background: rgba(255, 255, 255, 230); }"));
    QStringList headers;
    headers << tr("时间") << tr("方向") << tr("设备") << tr("文件") << tr("大小") << tr("结果");
    m_historyTree->setHeaderLabels(headers);
    m_historyTree->header()->setStretchLastSection(true);
    m_historyTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_historyTree, &QTreeWidget::customContextMenuRequested,
            this, &MainWindow::onHistoryContextMenu);
    connect(m_historyTree, &QTreeWidget::itemDoubleClicked,
            this, &MainWindow::onHistoryActivated);
    layout->addWidget(m_historyTree, 1);

    QHBoxLayout *row = new QHBoxLayout;
    QPushButton *refresh = new QPushButton(tr("刷新"), page);
    QPushButton *clear = new QPushButton(tr("清空历史"), page);
    connect(refresh, SIGNAL(clicked()), this, SLOT(refreshHistory()));
    connect(clear, SIGNAL(clicked()), this, SLOT(clearHistory()));
    row->addWidget(refresh);
    row->addWidget(clear);
    row->addStretch(1);
    layout->addLayout(row);

    refreshHistory();
    return page;
}

void MainWindow::refreshHistory()
{
    if (!m_historyTree)
        return;

    m_historyTree->clear();
    const QList<history::Entry> entries = history::load(200);
    for (int i = 0; i < entries.size(); ++i) {
        const history::Entry &entry = entries.at(i);
        QString result;
        if (entry.ok)
            result = tr("成功");
        else if (entry.detail.contains(tr("取消")) || entry.detail.contains(tr("中断")))
            result = tr("已取消");
        else
            result = tr("失败");
        QStringList cells;
        cells << entry.time.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
              << (entry.direction == QLatin1String("send") ? tr("发送") : tr("接收"))
              << entry.peer
              << QString::number(entry.fileCount)
              << formatBytes(entry.bytes)
              << result;
        QTreeWidgetItem *item = new QTreeWidgetItem(m_historyTree, cells);
        if (!entry.detail.isEmpty())
            item->setToolTip(5, entry.detail);
        if (!entry.ok)
            item->setForeground(5, QBrush(QColor(0xa0, 0x20, 0x20)));
    }
    for (int column = 0; column < m_historyTree->columnCount(); ++column)
        m_historyTree->resizeColumnToContents(column);
}

void MainWindow::onHistoryContextMenu(const QPoint &pos)
{
    if (!m_historyTree)
        return;
    QTreeWidgetItem *item = m_historyTree->itemAt(pos);
    if (!item)
        return;

    // The tree is rebuilt from history.jsonl in the same order, so the row
    // index doubles as the entry index.
    const int row = m_historyTree->indexOfTopLevelItem(item);
    const QList<history::Entry> entries = history::load(200);
    if (row < 0 || row >= entries.size())
        return;
    const history::Entry entry = entries.at(row);

    // roots keeps the user's original selection: a folder stays a folder, so
    // "send again" replays the folder transfer instead of N loose files.
    const QStringList roots = entry.roots.isEmpty() ? entry.paths : entry.roots;

    QStringList existing;
    for (int i = 0; i < entry.paths.size(); ++i) {
        if (QFileInfo(entry.paths.at(i)).exists())
            existing.append(entry.paths.at(i));
    }
    QStringList existingRoots;
    for (int i = 0; i < roots.size(); ++i) {
        if (QFileInfo(roots.at(i)).exists())
            existingRoots.append(roots.at(i));
    }

    QMenu menu(this);
    QAction *revealAction = menu.addAction(tr("打开所在文件夹"));
    revealAction->setEnabled(!existingRoots.isEmpty());
    QAction *resendAction = 0;
    if (entry.direction != QLatin1String("receive")) {
        menu.addSeparator();
        resendAction = menu.addAction(tr("重新发送"));
        // Text messages and files deleted from disk since then cannot be reused.
        resendAction->setEnabled(!existingRoots.isEmpty() && !existing.isEmpty());
    }

    QAction *chosen = menu.exec(m_historyTree->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;

    if (chosen == revealAction) {
        if (!existingRoots.isEmpty()) {
            logger::write(QStringLiteral("history: reveal %1").arg(existingRoots.first()));
            ui::revealInExplorer(QFileInfo(existingRoots.first()).absoluteFilePath());
        }
        return;
    }

    if (existingRoots.size() < roots.size()) {
        ui::notice(this, tr("重新发送"),
                   tr("有 %1 个文件已不存在，只加入仍能找到的 %2 个。")
                       .arg(roots.size() - existingRoots.size())
                       .arg(existingRoots.size()),
                   false);
    }
    addFiles(existingRoots);
    m_tabBar->setCurrentIndex(0);
}

void MainWindow::onHistoryActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column)
    if (!item || !m_historyTree)
        return;

    const int row = m_historyTree->indexOfTopLevelItem(item);
    const QList<history::Entry> entries = history::load(200);
    if (row < 0 || row >= entries.size())
        return;
    const history::Entry entry = entries.at(row);
    const QStringList roots = entry.roots.isEmpty() ? entry.paths : entry.roots;

    QStringList existing;
    for (int i = 0; i < entry.paths.size(); ++i) {
        if (QFileInfo(entry.paths.at(i)).exists())
            existing.append(entry.paths.at(i));
    }
    QStringList existingRoots;
    for (int i = 0; i < roots.size(); ++i) {
        if (QFileInfo(roots.at(i)).exists())
            existingRoots.append(roots.at(i));
    }
    if (existing.isEmpty() || existingRoots.isEmpty()) {
        ui::notice(this, tr("打开"), tr("文件已不存在，可能已被移动或删除。"), true);
        return;
    }

    if (entry.direction == QLatin1String("receive")) {
        // Opening the folder itself is what a folder transfer should do.
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(existingRoots.first()).absoluteFilePath()));
        return;
    }

    addFiles(existingRoots);
    m_tabBar->setCurrentIndex(0);
}

void MainWindow::clearHistory()
{
    if (!ui::confirm(this, tr("确定要删除全部传输记录吗？"), QString(), tr("清空"), tr("取消")))
        return;
    history::clear();
    refreshHistory();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    // Folders are accepted too: AddDirectoryAction in the reference client does
    // the same, expanding the tree into one file entry per leaf.
    QStringList paths;
    const QList<QUrl> urls = event->mimeData()->urls();
    for (int i = 0; i < urls.size(); ++i) {
        const QString path = urls.at(i).toLocalFile();
        if (path.isEmpty())
            continue;
        const QFileInfo info(path);
        if (info.isFile() || info.isDir())
            paths.append(path);
    }
    if (paths.isEmpty())
        return;

    if (m_network && (m_network->sending() || m_network->receiving())) {
        ui::notice(this, tr("正在传输"), tr("请等这次传输结束后再添加。"), true);
        return;
    }

    addFiles(paths);
    if (m_tabBar)
        m_tabBar->setCurrentIndex(0);
    event->acceptProposedAction();
}

QWidget *MainWindow::createSettingsTab()
{
    QWidget *page = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 14, 12, 12);
    layout->setSpacing(10);

    QFormLayout *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);

    // Device name: blank means "use the computer name", which is also what the
    // placeholder shows. Keeping the box empty for the default avoids writing a
    // machine specific value into a portable lsaero.ini.
    m_aliasEdit = new QLineEdit(page);
    m_aliasEdit->setMaxLength(64);
    m_aliasEdit->setPlaceholderText(m_settings->defaultAlias());
    m_aliasEdit->setText(m_settings->aliasSet() ? m_settings->alias() : QString());
    form->addRow(tr("本机名称："), m_aliasEdit);
    connect(m_aliasEdit, &QLineEdit::textChanged, this, &MainWindow::onAliasChanged);

    QHBoxLayout *dirRow = new QHBoxLayout;
    dirRow->setSpacing(8);
    m_saveDirLabel = new QLabel(page);
    m_saveDirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPushButton *changeDir = new QPushButton(tr("更改"), page);
    changeDir->setFixedWidth(84);
    dirRow->addWidget(m_saveDirLabel, 1);
    dirRow->addWidget(changeDir);
    form->addRow(tr("保存位置："), dirRow);

    m_askBox = new QCheckBox(tr("每次接收前询问保存位置"), page);
    form->addRow(QString(), m_askBox);

    m_httpsBox = new QCheckBox(tr("使用加密传输 (HTTPS)"), page);
    form->addRow(QString(), m_httpsBox);

    m_autostartBox = new QCheckBox(tr("登录时自动启动 (最小化到托盘)"), page);
    form->addRow(QString(), m_autostartBox);

    QHBoxLayout *pinRow = new QHBoxLayout;
    pinRow->setSpacing(8);
    m_pinBox = new QCheckBox(tr("接收时要求 PIN 码"), page);
    m_pinEdit = new QLineEdit(page);
    m_pinEdit->setMaxLength(4);
    m_pinEdit->setFixedWidth(64);
    m_pinEdit->setValidator(new QIntValidator(0, 9999, m_pinEdit));
    m_pinEdit->setEnabled(false);
    pinRow->addWidget(m_pinBox);
    pinRow->addWidget(m_pinEdit);
    pinRow->addStretch(1);
    form->addRow(QString(), pinRow);
    connect(m_pinBox, &QCheckBox::toggled, this, [this](bool on) {
        m_pinEdit->setEnabled(on);
        if (!on) {
            m_settings->setReceivePin(QString());
            if (m_network)
                m_network->applySettings();
            return;
        }
        // Tick without a PIN would promise protection that is not there: keep
        // the box unchecked until four digits are entered.
        if (m_pinEdit->text().size() != 4) {
            QSignalBlocker blocker(m_pinBox);
            m_pinBox->setChecked(false);
            m_pinEdit->setEnabled(true);
            m_pinEdit->setFocus();
            if (m_transferStatus) {
                m_transferStatus->setVisible(true);
                m_transferStatus->setText(tr("请先输入 4 位 PIN 码。"));
            }
            return;
        }
        m_settings->setReceivePin(m_pinEdit->text());
        if (m_network)
            m_network->applySettings();
    });
    connect(m_pinEdit, &QLineEdit::textEdited, this, [this](const QString &text) {
        // Editing the number invalidates the stored PIN until four digits are
        // back: a 3-digit box next to a 4-digit stored PIN confused everyone.
        if (!m_pinBox->isChecked())
            return;
        if (text.size() == 4) {
            m_settings->setReceivePin(text);
        } else {
            m_settings->setReceivePin(QString());
            QSignalBlocker blocker(m_pinBox);
            m_pinBox->setChecked(false);
            m_pinEdit->setEnabled(true);
        }
        if (m_network)
            m_network->applySettings();
    });

    m_languageBox = new QComboBox(page);
    m_languageBox->addItem(tr("跟随系统"));
    m_languageBox->addItem(tr("简体中文"));
    m_languageBox->addItem(tr("English"));
    form->addRow(tr("界面语言："), m_languageBox);

    m_fontBox = new QComboBox(page);
    m_fontBox->addItem(tr("系统默认"));
    const QFontDatabase fontDb;
    const QStringList families = fontDb.families();
    for (int i = 0; i < families.size(); ++i)
        m_fontBox->addItem(families.at(i));
    form->addRow(tr("应用字体："), m_fontBox);

    m_whiteAliasBox = new QCheckBox(tr("窗口最大化时将标题文本颜色改为白色 (兼容 Windows Vista 熄灯效果)"), page);
    form->addRow(QString(), m_whiteAliasBox);
    connect(m_whiteAliasBox, &QCheckBox::toggled, this, &MainWindow::onWhiteAliasToggled);

    // The option stays tickable without Aero, but then it does nothing: say so
    // underneath instead of silently ignoring the setting.
    m_whiteAliasHint = makeHintLabel(tr("此选项在 Aero 禁用时无效。"), page);
    m_whiteAliasHint->setVisible(false);
    form->addRow(QString(), m_whiteAliasHint);

    layout->addLayout(form);
    layout->addWidget(makeHintLabel(tr("本机名称改完立即生效；加密传输、字体与语言设置将在重新启动程序后生效。"), page));
    layout->addStretch(1);

    connect(changeDir, SIGNAL(clicked()), this, SLOT(chooseSaveDir()));
    return page;
}

void MainWindow::setupTray()
{
    // On a cold boot the tray can be unavailable for a few seconds (Explorer
    // still starting, a shell restart). Giving up immediately left the app
    // without an icon and closing the window then quit it for good, so retry.
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        if (m_trayRetries >= 20) {
            // No tray at all: a window that starts hidden (--tray, autostart)
            // would have no UI and no way back, so show it instead.
            if (!isVisible())
                showNormal();
            return;
        }
        ++m_trayRetries;
        QTimer::singleShot(1500, this, [this]() { setupTray(); });
        return;
    }
    if (m_tray)
        return;

    m_trayRetries = 0;
    m_tray = new QSystemTrayIcon(theme::appIcon(), this);
    m_tray->setToolTip(QString::fromLatin1(appinfo::appName()));

    QMenu *menu = new QMenu(this);
    QAction *showAction = menu->addAction(tr("显示主窗口"));
    menu->addSeparator();
    QAction *quitAction = menu->addAction(tr("退出"));

    connect(showAction, SIGNAL(triggered()), this, SLOT(showFromTray()));
    connect(quitAction, &QAction::triggered, this, [this]() {
        // Mark the exit so an open offer dialog does not answer the peer on the
        // way out (it used to send a "declined" right before the process died).
        m_quitting = true;
        if (m_offerDialog)
            m_offerDialog->reject();
        qApp->quit();
    });
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        onTrayActivated(reason);
    });

    m_tray->setContextMenu(menu);
    m_tray->show();
}

void MainWindow::loadFromSettings()
{
    m_headerAlias->setText(m_settings->alias());
    m_saveDirLabel->setText(QDir::toNativeSeparators(m_settings->saveDir()));

    if (m_autostartBox)
        m_autostartBox->setChecked(autostart::isEnabled());
    if (m_askBox)
        m_askBox->setChecked(m_settings->askEveryTime());
    if (m_httpsBox)
        m_httpsBox->setChecked(m_settings->httpsEnabled());

    if (m_languageBox) {
        const QString language = m_settings->language();
        int index = 0;
        if (language == QLatin1String("zh_CN"))
            index = 1;
        else if (language == QLatin1String("en"))
            index = 2;
        m_languageBox->setCurrentIndex(index);
    }

    if (m_fontBox) {
        const QString family = m_settings->fontFamily();
        const int index = family.isEmpty() ? 0 : m_fontBox->findText(family);
        m_fontBox->setCurrentIndex(index < 0 ? 0 : index);
    }

    if (m_pinBox && m_pinEdit) {
        const QString pin = m_settings->receivePin();
        m_pinBox->setChecked(!pin.isEmpty());
        m_pinEdit->setText(pin);
        m_pinEdit->setEnabled(!pin.isEmpty());
    }

    if (m_whiteAliasBox) {
        // Blocked so loading does not write the OS-derived default back into
        // lsaero.ini: an absent key must keep following the system.
        const bool blocked = m_whiteAliasBox->blockSignals(true);
        m_whiteAliasBox->setChecked(m_settings->whiteAliasMaximized());
        m_whiteAliasBox->blockSignals(blocked);
    }
    if (m_whiteAliasHint)
        m_whiteAliasHint->setVisible(!m_glassActive);
    updateHeaderColors();

    if (m_autostartBox) {
        connect(m_autostartBox, &QCheckBox::toggled, this, &MainWindow::onAutostartToggled);
    }
    if (m_languageBox) {
        connect(m_languageBox, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                this, &MainWindow::onLanguageChanged);
    }
    if (m_fontBox) {
        connect(m_fontBox, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                this, &MainWindow::onFontChanged);
    }
    if (m_askBox) {
        connect(m_askBox, &QCheckBox::toggled, this, [this](bool on) {
            m_settings->setAskEveryTime(on);
        });
    }
    if (m_httpsBox) {
        connect(m_httpsBox, &QCheckBox::toggled, this, [this](bool on) {
            m_settings->setHttpsEnabled(on);
        });
    }
}

void MainWindow::onAliasChanged(const QString &text)
{
    if (!m_settings)
        return;

    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        // Blank the key entirely: the computer name is the default and nothing
        // machine specific should leak into a portable lsaero.ini.
        m_settings->setAlias(QString());
    } else {
        m_settings->setAlias(trimmed);
    }

    if (m_headerAlias)
        m_headerAlias->setText(m_settings->alias());
    if (m_network)
        m_network->setAlias(m_settings->alias());
    updateHeaderColors();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
        updateHeaderColors();
}

void MainWindow::onWhiteAliasToggled(bool on)
{
    m_settings->setWhiteAliasMaximized(on);
    updateHeaderColors();
}

void MainWindow::updateTransferIcon(bool active)
{
    if (m_transferIconActive == active)
        return;
    m_transferIconActive = active;

    const QIcon icon = active ? theme::transferIcon() : theme::appIcon();
    setWindowIcon(icon);
    if (m_tray)
        m_tray->setIcon(icon);
}

void MainWindow::updateHeaderColors()
{
    if (!m_headerAlias)
        return;

    // Vista paints the maximised glass frame black; the alias then has to turn
    // white to stay readable. Every newer Windows keeps the light frame, so the
    // default palette stays correct there (the OS decides when lsaero.ini has
    // no value yet). Without Aero there is no dark frame, so the option is inert.
    const bool white = m_glassActive && m_settings->whiteAliasMaximized() && isMaximized();
    QPalette palette = m_headerAlias->palette();
    const QColor normal = QApplication::palette().color(QPalette::WindowText);
    const QColor color = white ? QColor(Qt::white) : normal;
    palette.setColor(QPalette::WindowText, color);
    palette.setColor(QPalette::Text, color);
    m_headerAlias->setPalette(palette);

    // Vista turns the maximised glass frame black ("lights out"); the subtitle
    // and the footer text then need different tints to stay legible.
    if (m_headerSubtitle)
        m_headerSubtitle->setStyleSheet(white ? QStringLiteral("color: #999999;")
                                              : QStringLiteral("color: #5a5a5a;"));
    if (m_transferStatus)
        m_transferStatus->setStyleSheet(white ? QStringLiteral("color: #ffffff;")
                                              : QStringLiteral("color: #1f1f1f;"));
    if (m_progress) {
        QPalette barPalette = m_progress->palette();
        const QColor barText = white ? QColor(Qt::white)
                                     : QApplication::palette().color(QPalette::Text);
        const QPalette::ColorGroup groups[] = { QPalette::Active, QPalette::Inactive, QPalette::Disabled };
        for (int i = 0; i < 3; ++i) {
            barPalette.setColor(groups[i], QPalette::Text, barText);
            barPalette.setColor(groups[i], QPalette::WindowText, barText);
            barPalette.setColor(groups[i], QPalette::ButtonText, barText);
        }
        m_progress->setPalette(barPalette);
        // The aggregate bar draws its percentage with the same colours.
        if (m_progressTotal)
            m_progressTotal->setPalette(barPalette);
    }
    if (m_transferStatusTotal) {
        m_transferStatusTotal->setStyleSheet(white ? QStringLiteral("color: #ffffff;")
                                                   : QStringLiteral("color: #1f1f1f;"));
    }

    logger::write(QStringLiteral("header: maximized=%1 white_alias=%2 color=%3")
                      .arg(isMaximized() ? 1 : 0)
                      .arg(white ? 1 : 0)
                      .arg(color.name()));
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (m_glassMode)
        applyGlassState();

    // Opening the window is the moment the user expects the list to be fresh
    // (the reference app announces whenever its send screen becomes visible).
    if (m_network)
        m_network->announce();
}

void MainWindow::applyGlassState()
{
    if (!m_glassMode)
        return;

    const bool glass = theme::compositionEnabled() && theme::isVistaOrLater();
    if (glass != m_glassActive) {
        // A HWND cannot switch between a per-pixel-alpha surface and a plain
        // one: Qt only reads WA_TranslucentBackground while the native window
        // is created. Windows 7 needs the plain window when Aero is off (an
        // alpha window without DWM shows the unpainted area as black), so the
        // only reliable fix is to rebuild the window for the new mode.
        scheduleGlassSwitch();
        return;
    }
    refreshGlassVisuals();
}

void MainWindow::scheduleGlassSwitch()
{
    if (m_glassSwitchPending)
        return;
    m_glassSwitchPending = true;
    QTimer::singleShot(0, this, SLOT(switchGlassMode()));
}

void MainWindow::switchGlassMode()
{
    m_glassSwitchPending = false;
    if (!m_glassMode)
        return;

    const bool glass = theme::compositionEnabled() && theme::isVistaOrLater();
    if (glass == m_glassActive)
        return;

    // The window is rebuilt without children being destroyed (destroy(true,
    // false) drops only the HWND), so geometry, maximized state and the widget
    // tree survive the switch.
    const bool wasVisible = isVisible();
    const bool wasMaximized = isMaximized();
    const QRect geometryBefore = geometry();

    logger::write(QStringLiteral("theme: composition %1, rebuilding window")
                      .arg(glass ? QStringLiteral("on") : QStringLiteral("off")));

    hide();
    destroy(true, false);
    setAttribute(Qt::WA_TranslucentBackground, glass);
    m_glassActive = glass;
    setGeometry(geometryBefore);

    if (wasMaximized)
        showMaximized();
    else if (wasVisible)
        show();

    refreshGlassVisuals();
    logger::write(QStringLiteral("theme: rebuilt window %1x%2 at %3,%4")
                      .arg(width()).arg(height()).arg(x()).arg(y()));
}

void MainWindow::refreshGlassVisuals()
{
    const bool glass = m_glassActive;

    if (glass)
        theme::enableGlass(this);
    else
        theme::disableGlass(this);
    // DWM caches the frame; ask it to redraw the caption now instead of waiting
    // for the next activation (Win7 switches take effect immediately).
    theme::refreshFrame(this);

    // Without composition the window is plain again: the card paints the whole
    // client area with the window colour so nothing stays unpainted.
    CardWidget *card = static_cast<CardWidget *>(m_card);
    if (card) {
        card->setGlassMode(glass);
        card->update();
    }

    // "此选项在 Aero 禁用时无效。" appears as soon as the glass goes away (and
    // disappears again when Aero comes back); the checkbox itself stays
    // clickable, the setting is simply inert.
    if (m_whiteAliasHint)
        m_whiteAliasHint->setVisible(!glass);
    // The white-alias rule depends on the glass state, so refresh the header
    // colours whenever composition flips.
    updateHeaderColors();

    if (centralWidget())
        centralWidget()->update();
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, long *result)
{
    // WM_DWMCOMPOSITIONCHANGED is only declared by headers with WINVER >= 0x0600;
    // Qt 5.6 builds with 0x0501 by default, so use the message number directly.
    const UINT kDwmCompositionChanged = 0x0320;
    if ((eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") && message) {
        MSG *msg = static_cast<MSG *>(message);
        if (msg->message == kDwmCompositionChanged) {
            logger::write(QStringLiteral("theme: DWM composition changed"));
            applyGlassState();
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_quitting || !m_tray || !m_tray->isVisible() || !QSystemTrayIcon::isSystemTrayAvailable()) {
        event->accept();
        return;
    }

    event->ignore();
    hide();

    if (!m_trayHintShown) {
        m_trayHintShown = true;
        m_tray->showMessage(QString::fromLatin1(appinfo::appName()),
                            tr("LocalSend Aero 仍在后台运行，单击托盘图标可以重新打开窗口。"),
                            QSystemTrayIcon::Information, 4000);
    }
}

void MainWindow::showFromTray()
{
    if (isMinimized())
        setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();

    HWND handle = reinterpret_cast<HWND>(winId());
    ::SetForegroundWindow(handle);
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
        showFromTray();
}

void MainWindow::onSecondInstanceMessage(const QString &message)
{
    if (message.startsWith(QLatin1String("show"), Qt::CaseInsensitive))
        showFromTray();
}

void MainWindow::onAutostartToggled(bool on)
{
    autostart::setEnabled(on);
    m_settings->setAutostart(on);
}

void MainWindow::chooseSaveDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择保存位置"), m_settings->saveDir());
    if (dir.isEmpty())
        return;

    m_settings->setSaveDir(dir);
    m_saveDirLabel->setText(QDir::toNativeSeparators(dir));
}

void MainWindow::onLanguageChanged(int index)
{
    QString language = QStringLiteral("auto");
    if (index == 1)
        language = QStringLiteral("zh_CN");
    else if (index == 2)
        language = QStringLiteral("en");
    m_settings->setLanguage(language);
}

void MainWindow::onFontChanged(int index)
{
    // Index 0 is the "system default" entry, which stores an empty family.
    const QString family = index <= 0 ? QString() : m_fontBox->itemText(index);
    m_settings->setFontFamily(family);
}

void MainWindow::updateFooterVisibility()
{
    const bool any = (m_progress && !m_progress->isHidden())
            || (m_transferStatus && !m_transferStatus->isHidden())
            || (m_progressTotal && !m_progressTotal->isHidden())
            || (m_transferStatusTotal && !m_transferStatusTotal->isHidden());
    if (m_footer && m_footer->isVisible() != any)
        m_footer->setVisible(any);

    // The footer grows from one row to four when a multi-file transfer starts.
    // Showing a child does not force the parent's layout to re-measure, so the
    // extra rows used to be clipped away; invalidate and re-activate instead.
    if (m_footer) {
        m_footer->updateGeometry();
        if (QWidget *host = m_footer->parentWidget()) {
            if (QLayout *hostLayout = host->layout()) {
                hostLayout->invalidate();
                hostLayout->activate();
            }
        }
        if (QLayout *footerLayout = m_footer->layout()) {
            footerLayout->invalidate();
            footerLayout->activate();
        }
    }

    QWidget *central = centralWidget();
    if (central)
        central->update();

    if (qEnvironmentVariableIsSet("LSAERO_UI_DEBUG") && m_footer) {
        QWidget *host = m_footer->parentWidget();
        logger::write(QStringLiteral("ui: footer=%1,%2 %3x%4 hint=%5x%6 host=%7 pb=%8,%9,%10 lbl=%11,%12,%13 tot=%14,%15,%16 lblt=%17,%18,%19")
                          .arg(m_footer->x()).arg(m_footer->y())
                          .arg(m_footer->width()).arg(m_footer->height())
                          .arg(m_footer->sizeHint().width()).arg(m_footer->sizeHint().height())
                          .arg(host ? host->metaObject()->className() : "none")
                          .arg(m_progress->y()).arg(m_progress->height()).arg(m_progress->isVisible())
                          .arg(m_transferStatus->y()).arg(m_transferStatus->height()).arg(m_transferStatus->isVisible())
                          .arg(m_progressTotal->y()).arg(m_progressTotal->height()).arg(m_progressTotal->isVisible())
                          .arg(m_transferStatusTotal->y()).arg(m_transferStatusTotal->height()).arg(m_transferStatusTotal->isVisible()));
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_progress || watched == m_transferStatus || watched == m_progressTotal
         || watched == m_transferStatusTotal)
        && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
        // Defer: the caller may still be inside a batch of visibility changes.
        QTimer::singleShot(0, this, [this]() { updateFooterVisibility(); });
    }
    return QMainWindow::eventFilter(watched, event);
}

QString MainWindow::formatBytes(qint64 bytes)
{
    if (bytes < 1024)
        return tr("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return tr("%1 KB").arg(QString::number(bytes / 1024.0, 'f', 1));
    if (bytes < qint64(1024) * 1024 * 1024)
        return tr("%1 MB").arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 1));
    return tr("%1 GB").arg(QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2));
}

QString MainWindow::formatSeconds(double seconds)
{
    const int total = qMax(1, static_cast<int>(seconds + 0.5));
    if (total < 60)
        return tr("%1 秒").arg(total);
    if (total < 3600)
        return tr("%1 分 %2 秒").arg(total / 60).arg(total % 60);
    return tr("%1 小时 %2 分").arg(total / 3600).arg((total % 3600) / 60);
}

void MainWindow::resetRate()
{
    m_rateTimer.invalidate();
    m_rateFileId.clear();
    m_rateLastBytes = 0;
    m_rateBps = 0.0;
}

QString MainWindow::rateSuffix(const QString &fileId, qint64 done, qint64 total)
{
    // A new file restarts the measurement; within a file the rate is smoothed
    // over roughly one second so the number does not flicker per 64 KB chunk.
    if (fileId != m_rateFileId || done < m_rateLastBytes || !m_rateTimer.isValid()) {
        m_rateFileId = fileId;
        m_rateLastBytes = done;
        m_rateBps = 0.0;
        m_rateTimer.start();
        return QString();
    }

    const qint64 elapsed = m_rateTimer.elapsed();
    if (elapsed < 400)
        return m_rateBps > 0 ? tr(" (%1/s，剩余 %2)")
                                      .arg(formatBytes(static_cast<qint64>(m_rateBps)),
                                           formatSeconds((total - done) / m_rateBps))
                             : QString();

    const double sample = (done - m_rateLastBytes) * 1000.0 / elapsed;
    m_rateBps = m_rateBps <= 0 ? sample : (m_rateBps * 0.6 + sample * 0.4);
    m_rateLastBytes = done;
    m_rateTimer.restart();

    if (m_rateBps < 1.0)
        return QString();
    const double remaining = total > done ? (total - done) / m_rateBps : 0.0;
    return tr(" (%1/s，剩余 %2)")
            .arg(formatBytes(static_cast<qint64>(m_rateBps)), formatSeconds(remaining));
}

void MainWindow::resetTotalRate()
{
    m_totalRateTimer.invalidate();
    m_totalRateLast = 0;
    m_totalRateBps = 0.0;
}

QString MainWindow::totalRateSuffix(qint64 done, qint64 total)
{
    if (!m_totalRateTimer.isValid() || done < m_totalRateLast) {
        m_totalRateLast = done;
        m_totalRateBps = 0.0;
        m_totalRateTimer.start();
        return QString();
    }

    const qint64 elapsed = m_totalRateTimer.elapsed();
    if (elapsed < 400)
        return m_totalRateBps > 0 ? tr(" (%1/s，剩余 %2)")
                                          .arg(formatBytes(static_cast<qint64>(m_totalRateBps)),
                                               formatSeconds((total - done) / m_totalRateBps))
                                 : QString();

    const double sample = (done - m_totalRateLast) * 1000.0 / elapsed;
    m_totalRateBps = m_totalRateBps <= 0 ? sample : (m_totalRateBps * 0.6 + sample * 0.4);
    m_totalRateLast = done;
    m_totalRateTimer.restart();

    if (m_totalRateBps < 1.0)
        return QString();
    const double remaining = total > done ? (total - done) / m_totalRateBps : 0.0;
    return tr(" (%1/s，剩余 %2)")
            .arg(formatBytes(static_cast<qint64>(m_totalRateBps)), formatSeconds(remaining));
}

void MainWindow::beginAggregate(qint64 total, int files)
{
    m_aggSeen.clear();
    m_aggTotal = total;
    m_aggDone = 0;
    // A single file (or a text message) has nothing extra to show: the file bar
    // already is the whole story.
    m_multiFile = files >= 2;
    resetTotalRate();
    if (m_progressTotal) {
        m_progressTotal->setValue(0);
        m_progressTotal->setVisible(m_multiFile);
    }
    if (m_transferStatusTotal) {
        m_transferStatusTotal->setVisible(m_multiFile);
        if (m_multiFile)
            m_transferStatusTotal->setText(tr("总计 %1 / %2")
                                               .arg(formatBytes(0), formatBytes(total)));
        else
            m_transferStatusTotal->clear();
    }
    updateFooterVisibility();
}

void MainWindow::updateAggregate(const QString &fileId, qint64 sent)
{
    if (!m_multiFile)
        return;
    if (sent < 0)
        sent = 0;
    // The worker walks files in order, so summing the last reported position of
    // every file seen so far gives a monotonic whole-session counter.
    m_aggSeen.insert(fileId, sent);
    qint64 done = 0;
    for (QHash<QString, qint64>::const_iterator it = m_aggSeen.constBegin();
         it != m_aggSeen.constEnd(); ++it) {
        done += it.value();
    }
    m_aggDone = done;
    if (m_progressTotal && m_aggTotal > 0)
        m_progressTotal->setValue(static_cast<int>(qMin<qint64>(100, done * 100 / m_aggTotal)));
    if (m_transferStatusTotal) {
        m_transferStatusTotal->setText(tr("总计 %1 / %2")
                                           .arg(formatBytes(done), formatBytes(m_aggTotal))
                                       + totalRateSuffix(done, m_aggTotal));
    }
}

void MainWindow::endAggregate()
{
    m_multiFile = false;
    m_aggSeen.clear();
    m_aggDone = 0;
    resetTotalRate();
    if (m_progressTotal) {
        m_progressTotal->setValue(0);
        m_progressTotal->setVisible(false);
    }
    if (m_transferStatusTotal) {
        m_transferStatusTotal->clear();
        m_transferStatusTotal->setVisible(false);
    }
    updateFooterVisibility();
}

