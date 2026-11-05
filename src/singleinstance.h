#ifndef SINGLEINSTANCE_H
#define SINGLEINSTANCE_H

#include <QObject>
#include <QString>

class QLocalServer;

// Keeps a single LocalSend Aero process per user session.
//
// The first instance owns a named pipe (QLocalServer). Any later instance
// connects, forwards a small command string and exits; the command lets the
// running instance raise its window (including from the tray).
class SingleInstance : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstance(const QString &key, QObject *parent = 0);
    ~SingleInstance();

    bool isFirstInstance() const { return m_first; }

    // Only meaningful for a secondary instance.
    bool notifyFirstInstance(const QString &message);

signals:
    void messageReceived(const QString &message);

private slots:
    void onNewConnection();

private:
    QString m_key;
    bool m_first;
    QLocalServer *m_server;
};

#endif // SINGLEINSTANCE_H
