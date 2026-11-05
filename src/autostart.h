#ifndef AUTOSTART_H
#define AUTOSTART_H

#include <QString>

namespace autostart {

QString commandLine();
bool isEnabled();
void setEnabled(bool on);

} // namespace autostart

#endif // AUTOSTART_H
