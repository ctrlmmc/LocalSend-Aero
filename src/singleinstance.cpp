#include "singleinstance.h"

#include <QLocalServer>
#include <QLocalSocket>

SingleInstance::SingleInstance(const QString &key, QObject *parent)
    : QObject(parent)
    , m_key(key)
    , m_first(true)
    , m_server(0)
{
    QLocalSocket probe;
    probe.connectToServer(m_key);
    if (probe.waitForConnected(300)) {
        m_first = false;
        probe.disconnectFromServer();
        return;
    }

    QLocalServer::removeServer(m_key);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_key)) {
        // Degraded mode: keep running rather than refusing to start.
        m_first = true;
        return;
    }
    connect(m_server, SIGNAL(newConnection()), this, SLOT(onNewConnection()));
}

SingleInstance::~SingleInstance()
{
    if (m_server)
        m_server->close();
}

bool SingleInstance::notifyFirstInstance(const QString &message)
{
    QLocalSocket socket;
    socket.connectToServer(m_key);
    if (!socket.waitForConnected(700))
        return false;

    const QByteArray payload = message.toUtf8() + '\n';
    socket.write(payload);
    socket.flush();
    socket.waitForBytesWritten(700);
    socket.disconnectFromServer();
    return true;
}

void SingleInstance::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        if (!socket)
            break;

        if (socket->waitForReadyRead(300)) {
            const QByteArray data = socket->readAll();
            const QString message = QString::fromUtf8(data).trimmed();
            if (!message.isEmpty())
                emit messageReceived(message);
        }

        socket->disconnectFromServer();
        socket->deleteLater();
    }
}
