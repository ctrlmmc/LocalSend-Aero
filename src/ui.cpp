#include "ui.h"

#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QWidget>

#include <windows.h>
#include <commctrl.h>

// The MinGW 4.9 runtime that ships with Qt 5.6 predates the TaskDialog API:
// its commctrl.h has no TASKDIALOGCONFIG at all. The structure and the flags
// are stable since Vista, so they are declared here instead of requiring a
// newer SDK. A newer toolchain defines TD_WARNING_ICON and skips this block.
#ifndef TD_WARNING_ICON

#define TDF_ALLOW_DIALOG_CANCELLATION 0x0008
#define TDF_POSITION_RELATIVE_TO_WINDOW 0x1000
#define TD_WARNING_ICON MAKEINTRESOURCEW(-1)
#define TD_ERROR_ICON MAKEINTRESOURCEW(-2)
#define TD_INFORMATION_ICON MAKEINTRESOURCEW(-3)
#define TD_SHIELD_ICON MAKEINTRESOURCEW(-4)
#define TD_OK_BUTTON 0x0001
#define TD_YES_BUTTON 0x0006
#define TD_NO_BUTTON 0x0007
#define TD_CANCEL_BUTTON 0x0008
#define TD_CLOSE_BUTTON 0x0009

// dwCommonButtons takes flags, not the button IDs above.
#define TDCBF_OK_BUTTON 0x0001
#define TDCBF_YES_BUTTON 0x0002
#define TDCBF_NO_BUTTON 0x0004

typedef HRESULT(CALLBACK *PFTASKDIALOGCALLBACK)(HWND, UINT, WPARAM, LPARAM, LONG_PTR);

typedef struct _TASKDIALOG_BUTTON
{
    int nButtonID;
    PCWSTR pszButtonText;
} TASKDIALOG_BUTTON, *PTASKDIALOG_BUTTON;

typedef struct _TASKDIALOGCONFIG
{
    UINT cbSize;
    HWND hwndParent;
    HINSTANCE hInstance;
    DWORD dwFlags;
    DWORD dwCommonButtons;
    PCWSTR pszWindowTitle;
    union {
        HICON hMainIcon;
        PCWSTR pszMainIcon;
    };
    PCWSTR pszMainInstruction;
    PCWSTR pszContent;
    UINT cButtons;
    const TASKDIALOG_BUTTON *pButtons;
    int nDefaultButton;
    UINT cRadioButtons;
    const TASKDIALOG_BUTTON *pRadioButtons;
    int nDefaultRadioButton;
    PCWSTR pszVerificationText;
    PCWSTR pszExpandedInformation;
    PCWSTR pszExpandedControlText;
    PCWSTR pszCollapsedControlText;
    union {
        HICON hFooterIcon;
        PCWSTR pszFooterIcon;
    };
    PCWSTR pszFooter;
    PFTASKDIALOGCALLBACK pfCallback;
    LONG_PTR lpCallbackData;
    UINT cxWidth;
} TASKDIALOGCONFIG, *PTASKDIALOGCONFIG;

#endif // TD_WARNING_ICON

namespace ui {
namespace {

// TaskDialogIndirect lives in comctl32 v6, i.e. it exists from Vista on. The
// call is resolved dynamically so that the executable still starts on XP.
typedef HRESULT(WINAPI * TaskDialogIndirectFn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);

TaskDialogIndirectFn taskDialogIndirect()
{
    static TaskDialogIndirectFn fn = 0;
    static bool looked = false;
    if (!looked) {
        looked = true;
        HMODULE comctl = ::LoadLibraryW(L"comctl32.dll");
        if (comctl) {
            fn = reinterpret_cast<TaskDialogIndirectFn>(
                ::GetProcAddress(comctl, "TaskDialogIndirect"));
        }
    }
    return fn;
}

HWND parentHandle(QWidget *parent)
{
    return parent ? reinterpret_cast<HWND>(parent->winId()) : 0;
}

bool taskDialog(const QString &title, const QString &text, bool warning, bool confirm,
                const QString &acceptText, const QString &rejectText, HWND parent, bool *accepted)
{
    if (!theme::isVistaOrLater())
        return false;
    TaskDialogIndirectFn call = taskDialogIndirect();
    if (!call)
        return false;

    // TaskDialogIndirect takes UTF-16 strings; the QString data is exactly that.
    TASKDIALOGCONFIG config;
    ::ZeroMemory(&config, sizeof(config));
    config.cbSize = sizeof(config);
    config.hwndParent = parent;
    config.hInstance = ::GetModuleHandleW(0);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;

    // The caller's "title" is the short headline: it becomes the large dark
    // blue main instruction. The details go in the content area (small black
    // text), which is what Windows' own dialogs do.
    const QString windowTitle = QCoreApplication::applicationName();
    config.pszWindowTitle = reinterpret_cast<LPCWSTR>(windowTitle.utf16());
    config.pszMainInstruction = reinterpret_cast<LPCWSTR>(title.utf16());
    config.pszContent = text.isEmpty() ? 0 : reinterpret_cast<LPCWSTR>(text.utf16());

    // Custom buttons carry the caller's wording ("清空" / "取消") instead of the
    // generic "是" / "否"; dwCommonButtons must then stay empty, or Windows adds
    // its own set on top.
    const QString acceptLabel = acceptText.isEmpty() ? QCoreApplication::translate("ui", "确定") : acceptText;
    const QString rejectLabel = rejectText.isEmpty() ? QCoreApplication::translate("ui", "取消") : rejectText;
    TASKDIALOG_BUTTON buttons[2];
    ::ZeroMemory(buttons, sizeof(buttons));
    buttons[0].nButtonID = IDYES;
    buttons[0].pszButtonText = reinterpret_cast<PCWSTR>(acceptLabel.utf16());
    buttons[1].nButtonID = IDNO;
    buttons[1].pszButtonText = reinterpret_cast<PCWSTR>(rejectLabel.utf16());

    if (confirm) {
        config.dwCommonButtons = 0;
        config.cButtons = 2;
        config.pButtons = buttons;
        config.nDefaultButton = IDYES;
    } else {
        config.dwCommonButtons = TDCBF_OK_BUTTON;
        config.nDefaultButton = IDOK;
    }

    if (warning)
        config.pszMainIcon = TD_WARNING_ICON;
    else
        config.pszMainIcon = TD_INFORMATION_ICON;

    int pressed = 0;
    HRESULT result = call(&config, &pressed, 0, 0);
    if (FAILED(result))
        return false;

    if (accepted)
        *accepted = (pressed == IDYES || pressed == IDOK);
    return true;
}

} // namespace

void notice(QWidget *parent, const QString &title, const QString &text, bool warning)
{
    bool accepted = false;
    if (taskDialog(title, text, warning, false, QString(), QString(), parentHandle(parent), &accepted))
        return;

    QMessageBox box(parent);
    box.setWindowTitle(QCoreApplication::applicationName());
    box.setText(title);
    box.setInformativeText(text);
    box.setIcon(warning ? QMessageBox::Warning : QMessageBox::Information);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

bool confirm(QWidget *parent, const QString &title, const QString &text,
             const QString &acceptText, const QString &rejectText)
{
    bool accepted = false;
    if (taskDialog(title, text, true, true, acceptText, rejectText, parentHandle(parent), &accepted))
        return accepted;

    QMessageBox box(parent);
    box.setWindowTitle(QCoreApplication::applicationName());
    box.setText(title);
    box.setInformativeText(text);
    box.setIcon(QMessageBox::Question);
    QPushButton *yes = box.addButton(acceptText.isEmpty() ? QMessageBox::tr("是") : acceptText,
                                     QMessageBox::AcceptRole);
    box.addButton(rejectText.isEmpty() ? QMessageBox::tr("否") : rejectText, QMessageBox::RejectRole);
    box.setDefaultButton(yes);
    box.exec();
    return box.clickedButton() == yes;
}

void revealInExplorer(const QString &path)
{
    if (path.isEmpty())
        return;

    // /select highlights the item in its parent folder; for a directory the
    // folder itself ends up selected, which is what "open containing folder"
    // should do for a received folder transfer.
    const QString native = QDir::toNativeSeparators(path);
    if (!QFileInfo::exists(path))
        return;

    // explorer.exe parses its own command line, and quotes around the whole
    // "/select,<path>" token are not enough once the path contains spaces: the
    // quotes have to sit around the path itself.
    const QString argument = QStringLiteral("/select,\"%1\"").arg(native);
    QProcess::startDetached(QStringLiteral("explorer.exe"), QStringList() << argument);
}

} // namespace ui
