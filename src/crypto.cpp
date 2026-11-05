#include "crypto.h"

#include "logger.h"
#include "protocol.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSslCertificate>
#include <QSslKey>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace {

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const qint64 written = file.write(data);
    file.close();
    return written == data.size();
}

BIO *bioFromByteArray(const QByteArray &data)
{
    BIO *bio = BIO_new(BIO_s_mem());
    BIO_write(bio, data.constData(), data.size());
    return bio;
}

QByteArray bioToByteArray(BIO *bio)
{
    char *data = 0;
    const long size = BIO_get_mem_data(bio, &data);
    return size > 0 ? QByteArray(data, static_cast<int>(size)) : QByteArray();
}

QByteArray generateCertificatePem(QByteArray *keyPem, QString *error)
{
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();

    RSA *rsa = RSA_generate_key(2048, RSA_F4, 0, 0);
    if (!rsa) {
        *error = QStringLiteral("RSA_generate_key failed");
        return QByteArray();
    }

    EVP_PKEY *pkey = EVP_PKEY_new();
    EVP_PKEY_assign_RSA(pkey, rsa);

    X509 *certificate = X509_new();
    X509_set_version(certificate, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate),
                     static_cast<long>(QDateTime::currentMSecsSinceEpoch() & 0x7fffffff));
    X509_gmtime_adj(X509_get_notBefore(certificate), 0);
    X509_gmtime_adj(X509_get_notAfter(certificate), 60L * 60L * 24L * 365L * 10L);
    X509_set_pubkey(certificate, pkey);

    // wincrypt.h (pulled in by the Windows headers) defines X509_NAME as a
    // macro, so the typedef from x509.h cannot be spelled here; keep it auto.
    auto name = X509_get_subject_name(certificate);
    const QByteArray commonName = protocol::userName().toUtf8();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                               reinterpret_cast<const unsigned char *>(commonName.constData()),
                               -1, -1, 0);
    X509_set_issuer_name(certificate, name);

    if (!X509_sign(certificate, pkey, EVP_sha256())) {
        *error = QStringLiteral("X509_sign failed");
        X509_free(certificate);
        EVP_PKEY_free(pkey);
        return QByteArray();
    }

    BIO *certBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certBio, certificate);
    BIO *keyBio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(keyBio, pkey, 0, 0, 0, 0, 0);

    const QByteArray certPem = bioToByteArray(certBio);
    *keyPem = bioToByteArray(keyBio);

    BIO_free(certBio);
    BIO_free(keyBio);
    X509_free(certificate);
    EVP_PKEY_free(pkey);
    return certPem;
}

} // namespace

namespace crypto {

QString fingerprintOf(const QSslCertificate &certificate)
{
    return QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex().toUpper());
}

Identity loadOrCreate(const QString &dir)
{
    Identity identity;
    identity.path = dir;
    QDir().mkpath(dir);

    const QString certPath = dir + QStringLiteral("/cert.pem");
    const QString keyPath = dir + QStringLiteral("/key.pem");

    QByteArray certPem = readFile(certPath);
    QByteArray keyPem = readFile(keyPath);

    QSslCertificate certificate(certPem, QSsl::Pem);
    QSslKey key(keyPem, QSsl::Rsa, QSsl::Pem);

    if (certificate.isNull() || key.isNull()) {
        QString error;
        certPem = generateCertificatePem(&keyPem, &error);
        if (certPem.isEmpty()) {
            logger::write(QStringLiteral("crypto: certificate generation failed: ") + error);
            return identity;
        }
        writeFile(certPath, certPem);
        writeFile(keyPath, keyPem);
        certificate = QSslCertificate(certPem, QSsl::Pem);
        key = QSslKey(keyPem, QSsl::Rsa, QSsl::Pem);
        logger::write(QStringLiteral("crypto: generated new identity in ") + dir);
    } else {
        logger::write(QStringLiteral("crypto: loaded identity from ") + dir);
    }

    if (certificate.isNull() || key.isNull()) {
        logger::write(QStringLiteral("crypto: stored certificate is unusable"));
        return identity;
    }

    identity.certPem = certPem;
    identity.keyPem = keyPem;
    identity.certificate = certificate;
    identity.privateKey = key;
    identity.fingerprint = fingerprintOf(certificate);
    identity.valid = true;
    logger::write(QStringLiteral("crypto: fingerprint=") + identity.fingerprint);
    return identity;
}

} // namespace crypto
