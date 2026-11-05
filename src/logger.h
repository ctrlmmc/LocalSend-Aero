#ifndef LOGGER_H
#define LOGGER_H

#include <QString>

// Minimal append-only logger. Network code is asynchronous and hard to debug
// from the UI alone, so every request/announcement is mirrored to a file next
// to lsaero.ini and forwarded to the status bar.
namespace logger {

void init(const QString &path);
QString path();
void write(const QString &line);

} // namespace logger

#endif // LOGGER_H
