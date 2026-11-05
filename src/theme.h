#ifndef THEME_H
#define THEME_H

#include <QFont>
#include <QIcon>

class QWidget;

namespace theme {

// Applies the Windows native widget style ("windowsvista" when available).
void applyAppStyle();

// The system UI font (NONCLIENTMETRICS message font, e.g. "Segoe UI 9pt").
// Falls back to QApplication::font() when the system query fails.
QFont systemUiFont();

// True when DWM composition (Aero glass) is currently active.
// Loaded dynamically so that the binary still starts on Windows XP.
bool compositionEnabled();

// Test hook (--force-opaque): pretend composition is off so the opaque
// fallback layout can be exercised on systems where Aero cannot be disabled.
void setCompositionOverride(bool enabled, bool active);

// True only on Windows Vista (6.0). Vista paints the maximized glass frame
// black, so the header text has to switch to white there. Newer systems keep
// the light frame and do not need the swap.
bool isWindowsVista();

// True on Windows Vista and later (major version >= 6). TaskDialog needs it.
bool isVistaOrLater();

// Decides whether the window is built for the glass card.
// force=true  -> always (--aero, no fallback)
// disable=true-> never (--no-aero)
// Otherwise it follows the platform (DWM present), *not* the current
// composition state: on Windows 7 the user can switch Aero on while the
// program runs and the window has to be ready for both modes.
bool cardEnabled(bool force, bool disable);

// Makes the window alpha aware and, while composition is on, extends the DWM
// frame over the whole client area. The attribute can only be honoured before
// the platform window is created, so call this from the constructor.
// Returns false when composition is off (the caller draws an opaque card).
bool enableGlass(QWidget *widget);

// Drops the DWM frame extension again (composition toggled off at runtime).
// The per-pixel alpha window is kept: Qt ignores WA_TranslucentBackground
// changes after creation, and keeping it is what lets glass come back without
// recreating (and repositioning) the window.
void disableGlass(QWidget *widget);

// Asks DWM to recompute the non-client frame (call after changing the frame
// extension) so a Win7 Aero <-> Basic switch repaints the caption immediately.
void refreshFrame(QWidget *widget);

// Program icon: the .ico designed for the app, shipped as PNGs in icons.qrc.
QIcon appIcon();

// Windows shell icon (16/32/48).
QIcon appIconSmall();

// Icon shown in the title bar and the tray while a transfer is running.
QIcon transferIcon();

} // namespace theme

#endif // THEME_H
