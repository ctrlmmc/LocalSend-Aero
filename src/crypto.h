#ifndef CRYPTO_H
#define CRYPTO_H

#include <QByteArray>
#include <QString>
#include <QSslCertificate>
#include <QSslKey>

// Device identity: a self-signed RSA-2048 certificate, exactly like the
// reference implementation (CN=LocalSend User, 10 years, PKCS#8 PEM key).
// The SHA-256 of the DER encoding is the protocol fingerprint.
namespace crypto {

struct Identity
{
    Identity() : valid(false) {}

    QByteArray certPem;
    QByteArray keyPem;
    QString fingerprint;  // SHA-256(DER), upper case hex without separators
    QString path;         // directory holding cert.pem / key.pem
    QSslCertificate certificate;
    QSslKey privateKey;
    bool valid;
};

// Loads <dir>\cert.pem + key.pem, generating a fresh pair on first start or
// when the stored files cannot be parsed.
Identity loadOrCreate(const QString &dir);

// SHA-256 of the certificate's DER encoding, upper case hex.
QString fingerprintOf(const QSslCertificate &certificate);

} // namespace crypto

#endif // CRYPTO_H
