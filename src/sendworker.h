#ifndef SENDWORKER_H
#define SENDWORKER_H

#include "crypto.h"
#include "protocol.h"

#include <QAtomicInt>
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

// Runs the whole outbound transfer (prepare-upload, every file upload, cancel
// on failure) in a worker thread.
//
// Sending used to run on the GUI thread with a hand pumped event loop, which
// made the window stutter and left the cancel button unclickable while a big
// file was on the wire. Everything here is deliberately blocking: it lives on
// its own thread and reports back with signals.
class SendWorker : public QObject
{
    Q_OBJECT

public:
    struct Job
    {
        Job() : port(0), tls(true), textOnly(false) {}

        QString address;
        quint16 port;
        bool tls;
        QString targetAlias;
        // Certificate fingerprint announced by the peer; used to pin the TLS
        // session so an impostor cannot collect the upload tokens.
        QString targetFingerprint;
        protocol::DeviceInfo self;
        QList<protocol::FileDto> files;
        QList<QString> paths; // empty string means "body lives in preview"
        QString pin;
        crypto::Identity identity;
        bool textOnly;
    };

    explicit SendWorker(const Job &job, QObject *parent = 0);

    // Thread safe: the GUI calls this while the worker is inside a blocking
    // socket call. remote is true when the receiving side asked us to stop.
    void requestCancel(bool remote = false);

public slots:
    void run();

signals:
    void stage(const QString &text);
    void progress(const QString &fileId, qint64 sent, qint64 total);
    // Emitted once prepare-upload succeeded: the peer can cancel this id, so the
    // service layer needs it to recognise a cancel coming from the receiver.
    void prepared(const QString &sessionId);
    // needsPin is only true when the peer asked for a PIN and none was sent.
    void finished(bool ok, const QString &message, int status, bool needsPin);

private:
    bool isCancelled() const { return m_cancel.loadAcquire() != 0; }
    bool isRemoteCancelled() const { return m_remoteCancel.loadAcquire() != 0; }

    Job m_job;
    QAtomicInt m_cancel;
    QAtomicInt m_remoteCancel;
};

#endif // SENDWORKER_H
