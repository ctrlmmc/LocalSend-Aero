#ifndef HISTORY_H
#define HISTORY_H

#include <QDateTime>
#include <QList>
#include <QString>

// Transfer history, stored as one JSON object per line next to the config file.
// The list is small (a few hundred rows), so loading it wholesale is fine.
namespace history {

struct Entry
{
    QDateTime time;
    QString direction;   // "send" or "receive"
    QString peer;
    QString fingerprint;
    int fileCount;
    qint64 bytes;
    bool ok;
    QString detail;
    // Local paths: the saved files for "receive", the expanded sources for "send".
    QStringList paths;
    // What the user actually picked / what landed as a whole: folders stay
    // folders here, so "send again" reproduces the original selection and
    // "open containing folder" highlights the folder instead of one file
    // inside it.
    QStringList roots;
};

QString path();
void append(const Entry &entry);
QList<Entry> load(int limit = 200);
void clear();

} // namespace history

#endif // HISTORY_H
