#include "theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QScreen>
#include <QStyle>
#include <QStyleFactory>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <windows.h>

namespace {

typedef HRESULT(WINAPI *DwmIsCompositionEnabledFn)(BOOL *);
typedef HRESULT(WINAPI *DwmExtendFrameIntoClientAreaFn)(HWND, const void *);

// Test hook: when active, compositionEnabled() reports this value instead of
// asking DWM. Used by --force-opaque to check the opaque fallback layout on
// systems where Aero cannot be turned off any more.
bool g_compositionOverrideActive = false;
bool g_compositionOverride = true;

// MARGINS from dwmapi.h (also missing from the MinGW 4.9.2 headers).
struct Margins
{
    int left;
    int right;
    int top;
    int bottom;
};

DwmIsCompositionEnabledFn resolveDwmIsCompositionEnabled()
{
    static DwmIsCompositionEnabledFn fn = 0;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        HMODULE module = ::LoadLibraryW(L"dwmapi.dll");
        if (module)
            fn = reinterpret_cast<DwmIsCompositionEnabledFn>(
                ::GetProcAddress(module, "DwmIsCompositionEnabled"));
    }
    return fn;
}

DwmExtendFrameIntoClientAreaFn resolveDwmExtendFrameIntoClientArea(){
    static DwmExtendFrameIntoClientAreaFn fn = 0;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        HMODULE module = ::LoadLibraryW(L"dwmapi.dll");
        if (module)
            fn = reinterpret_cast<DwmExtendFrameIntoClientAreaFn>(
                ::GetProcAddress(module, "DwmExtendFrameIntoClientArea"));
    }
    return fn;
}

} // namespace

namespace theme {

void applyAppStyle()
{
    QApplication *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app)
        return;

    const QStringList keys = QStyleFactory::keys();
    for (int i = 0; i < keys.size(); ++i) {
        if (keys.at(i).compare(QStringLiteral("windowsvista"), Qt::CaseInsensitive) == 0) {
            QStyle *style = QStyleFactory::create(keys.at(i));
            if (style)
                app->setStyle(style);
            return;
        }
    }
}

bool compositionEnabled()
{
    if (g_compositionOverrideActive)
        return g_compositionOverride;

    DwmIsCompositionEnabledFn fn = resolveDwmIsCompositionEnabled();
    if (!fn)
        return false;
    BOOL enabled = FALSE;
    if (FAILED(fn(&enabled)))
        return false;
    return enabled == TRUE;
}

void setCompositionOverride(bool enabled, bool active)
{
    g_compositionOverride = enabled;
    g_compositionOverrideActive = active;
}

bool isWindowsVista()
{
    DWORD majorVersion = 0;
    DWORD minorVersion = 0;
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return false;
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    RtlGetVersionFn rtlGetVersion =
        reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion)
        return false;
    OSVERSIONINFOW info;
    ::ZeroMemory(&info, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0)
        return false;
    majorVersion = info.dwMajorVersion;
    minorVersion = info.dwMinorVersion;
    return majorVersion == 6 && minorVersion == 0;
}

bool isVistaOrLater()
{
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return false;
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    RtlGetVersionFn rtlGetVersion =
        reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion)
        return false;
    OSVERSIONINFOW info;
    ::ZeroMemory(&info, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0)
        return false;
    return info.dwMajorVersion >= 6;
}

bool cardEnabled(bool force, bool disable)
{
    // Windows XP has no DWM at all: never build the glass window there, even
    // with --aero (the alpha surface would turn into a black client area).
    if (disable || !isVistaOrLater())
        return false;
    // Follow the platform, not the current composition state: Windows 7 lets
    // Aero be switched on while the program is running, and the window must
    // already be alpha aware when that happens (the attribute is only honoured
    // at creation time). `force` therefore has nothing left to override on
    // Vista+; it is kept so the command line stays compatible.
    Q_UNUSED(force);
    return true;
}

QFont systemUiFont()
{
    QFont font = QApplication::font();

    // The shell message font is what Explorer itself uses for dialogs and
    // menus ("Segoe UI 9pt" on Vista+, "Tahoma 8pt" on XP). Asking the system
    // keeps the UI consistent with the rest of the desktop instead of falling
    // back to Qt's legacy "MS Shell Dlg 2".
    DWORD majorVersion = 6;
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
        RtlGetVersionFn rtlGetVersion =
            reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtlGetVersion) {
            OSVERSIONINFOW info;
            ::ZeroMemory(&info, sizeof(info));
            info.dwOSVersionInfoSize = sizeof(info);
            if (rtlGetVersion(&info) == 0)
                majorVersion = info.dwMajorVersion;
        }
    }

    NONCLIENTMETRICSW metrics;
    ::ZeroMemory(&metrics, sizeof(metrics));
    metrics.cbSize = sizeof(metrics);
    if (majorVersion < 6)
        metrics.cbSize -= sizeof(int); // Vista added iPaddedBorderWidth.

    if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize, &metrics, 0))
        return font;

    const QString family = QString::fromWCharArray(metrics.lfMessageFont.lfFaceName);
    if (!family.isEmpty() && font.family().compare(family, Qt::CaseInsensitive) != 0) {
        // The windowsvista style seeds QApplication::font() from the legacy
        // "MS Shell Dlg 2" alias, which resolves to SimSun on a Chinese
        // Windows. The shell message font ("Segoe UI" on Vista+, with the
        // system CJK fallback for Chinese text) is what Explorer and native
        // dialogs actually use, so adopt its family but keep Qt's point size.
        font.setFamily(family);
    }

    return font;
}

bool enableGlass(QWidget *widget)
{
    if (!widget)
        return false;

    // Per-pixel alpha is a creation-time property: Windows (and Qt) ignore the
    // attribute once the HWND exists. Set it whenever the OS can composite, and
    // leave the decision of *drawing* glass to the composition state below.
    if (isVistaOrLater())
        widget->setAttribute(Qt::WA_TranslucentBackground, true);

    DwmExtendFrameIntoClientAreaFn extendFrame = resolveDwmExtendFrameIntoClientArea();
    const bool glass = compositionEnabled();
    if (!extendFrame)
        return glass;

    Margins margins;
    // Aero off: reset the extension, otherwise the frame area stays black
    // (Win7 "basic" theme) because nothing composites the cleared alpha there.
    margins.left = glass ? -1 : 0;
    margins.right = glass ? -1 : 0;
    margins.top = glass ? -1 : 0;
    margins.bottom = glass ? -1 : 0;
    const bool ok = SUCCEEDED(extendFrame(reinterpret_cast<HWND>(widget->winId()), &margins));
    return glass && ok;
}

void disableGlass(QWidget *widget)
{
    if (!widget)
        return;

    DwmExtendFrameIntoClientAreaFn extendFrame = resolveDwmExtendFrameIntoClientArea();
    if (extendFrame) {
        Margins margins;
        margins.left = 0;
        margins.right = 0;
        margins.top = 0;
        margins.bottom = 0;
        extendFrame(reinterpret_cast<HWND>(widget->winId()), &margins);
    }
    // Deliberately keep WA_TranslucentBackground: Qt ignores changes to it after
    // the window was created, and the alpha aware surface is exactly what lets
    // Aero come back later without recreating (and losing the position of) the
    // window. The card paints every pixel opaque while composition is off.
}

void refreshFrame(QWidget *widget)
{
    if (!widget)
        return;
    // SWP_FRAMECHANGED with no move/size/zorder: makes DWM/the theme redraw the
    // caption for the new frame extension right away.
    ::SetWindowPos(reinterpret_cast<HWND>(widget->winId()), 0, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                       | SWP_NOACTIVATE);
}

QIcon appIcon()
{
    static QIcon icon;
    if (icon.isNull()) {
        icon.addFile(QStringLiteral(":/icons/lsaero-16.png"), QSize(16, 16));
        icon.addFile(QStringLiteral(":/icons/lsaero-32.png"), QSize(32, 32));
        icon.addFile(QStringLiteral(":/icons/lsaero-48.png"), QSize(48, 48));
        icon.addFile(QStringLiteral(":/icons/lsaero-64.png"), QSize(64, 64));
        icon.addFile(QStringLiteral(":/icons/lsaero-256.png"), QSize(256, 256));
    }
    return icon;
}

QIcon appIconSmall()
{
    return appIcon();
}

QIcon transferIcon()
{
    static QIcon icon;
    if (icon.isNull()) {
        icon.addFile(QStringLiteral(":/icons/tran-16.png"), QSize(16, 16));
        icon.addFile(QStringLiteral(":/icons/tran-32.png"), QSize(32, 32));
        icon.addFile(QStringLiteral(":/icons/tran-48.png"), QSize(48, 48));
    }
    return icon;
}

} // namespace theme
