#ifndef CRYPTO_UTIL_H
#define CRYPTO_UTIL_H

#include <QString>
#include <QByteArray>
#include <openssl/sha.h>

class CryptoUtil {
public:
    // Generate cryptographically secure random hexadecimal string
    static QString randomHex(int byteCount = 16);

    // Compute SHA-256 for a file path (incremental 64 KiB reads)
    static QString sha256File(const QString &filePath, QString *error = 0);
};

class Sha256Context {
public:
    Sha256Context();
    void reset();
    void update(const char *data, qint64 length);
    void update(const QByteArray &data);
    QString finalHex();

private:
    SHA256_CTX m_ctx;
    bool m_finalized;
    QString m_digest;
};

#endif // CRYPTO_UTIL_H
