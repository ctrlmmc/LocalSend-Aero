#ifndef LS_AERO_UI_H
#define LS_AERO_UI_H

#include <QString>

class QWidget;

// Shell-level dialogs. On Vista and later these are real TaskDialogs (the
// "Windows 通用组件库 6.0" look the app is supposed to use); on older systems,
// or when the TaskDialog API is unavailable, they fall back to QMessageBox.
namespace ui {

void notice(QWidget *parent, const QString &title, const QString &text, bool warning = false);

// Returns true when the user picks the affirmative button.
bool confirm(QWidget *parent, const QString &title, const QString &text,
             const QString &acceptText = QString(), const QString &rejectText = QString());

// "Open the folder that contains this file", Windows Explorer style.
void revealInExplorer(const QString &path);

} // namespace ui

#endif // LS_AERO_UI_H
