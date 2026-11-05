#ifndef DISCOVERY_H
#define DISCOVERY_H

#include "protocol.h"

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

class QNetworkInterface;
class QUdpSocket;

// Multicast discovery, mirroring the reference implementation:
//   * announcements go to 224.0.0.167:53317 as JSON with "announce": true
//   * one UDP socket per network interface, so every network is covered
//     (a single default-interface socket misses the others on a multi-homed
//     machine: the OS only delivers multicast to the interface it was joined on)
//   * announce() sends a burst (0 / 500 / 2000 ms); announceOnce() sends one
//     datagram and is what the 5 s keep-alive timer calls
//   * peers answer with an HTTP /register call, never over UDP
class Discovery : public QObject
{
    Q_OBJECT

public:
    explicit Discovery(QObject *parent = 0);

    bool start(const protocol::DeviceInfo &self, QString *error);
    void stop();
    void announce();
    void announceOnce();
    // Renaming the device at runtime: the next announcement carries the new
    // alias, no restart needed.
    void setSelf(const protocol::DeviceInfo &self);

signals:
    void announcementReceived(const protocol::DeviceInfo &device, const QHostAddress &address);
    void logLine(const QString &line);

private slots:
    void onReadyRead();

private:
    void sendAnnouncement();
    bool addInterfaceSocket(const QNetworkInterface &iface, QString *joinError);

    QList<QUdpSocket *> m_sockets;
    protocol::DeviceInfo m_self;
};

#endif // DISCOVERY_H
