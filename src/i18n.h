#ifndef LS_AERO_I18N_H
#define LS_AERO_I18N_H

#include <QString>

class QTranslator;

// Runtime language switching. The app is written in Chinese and ships an
// English table; installing the translator makes every existing tr() call
// return English, so neither the UI code nor the strings have to change.
//
// Must be called before the first widget is created (main() does it right
// after the settings are loaded), because some strings are baked into the
// layout at construction time. That is why the setting takes effect after a
// restart.
namespace i18n {

// language: "system" | "zh" | "en" (the value stored in app/language).
void install(const QString &language);

// The language actually in use, "zh" or "en", for logging / diagnostics.
QString activeLanguage();

} // namespace i18n

#endif // LS_AERO_I18N_H
