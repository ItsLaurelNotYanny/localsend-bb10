#include "protocol/CryptoUtil.h"

#include <QFile>
#include <openssl/rand.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

QString CryptoUtil::randomHex(int byteCount)
{
    if (byteCount <= 0) {
        return QString();
    }
    QByteArray buf(byteCount, 0);
    int rc = RAND_bytes(reinterpret_cast<unsigned char*>(buf.data()), byteCount);
    if (rc != 1) {
        // Entropy failure
        return QString();
    }
    return QString::fromLatin1(buf.toHex().toLower());
}

QString CryptoUtil::sha256File(const QString &filePath, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return QString();
    }

    Sha256Context ctx;
    char buffer[65536];
    while (!file.atEnd()) {
        qint64 read = file.read(buffer, sizeof(buffer));
        if (read < 0) {
            if (error) {
                *error = file.errorString();
            }
            return QString();
        }
        if (read > 0) {
            ctx.update(buffer, read);
        }
    }
    return ctx.finalHex();
}

Sha256Context::Sha256Context()
    : m_finalized(false)
{
    reset();
}

void Sha256Context::reset()
{
    SHA256_Init(&m_ctx);
    m_finalized = false;
    m_digest.clear();
}

void Sha256Context::update(const char *data, qint64 length)
{
    if (m_finalized || !data || length <= 0) {
        return;
    }
    SHA256_Update(&m_ctx, data, static_cast<size_t>(length));
}

void Sha256Context::update(const QByteArray &data)
{
    update(data.constData(), data.size());
}

QString Sha256Context::finalHex()
{
    if (m_finalized) {
        return m_digest;
    }
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256_Final(md, &m_ctx);
    m_finalized = true;

    QByteArray raw(reinterpret_cast<const char*>(md), SHA256_DIGEST_LENGTH);
    m_digest = QString::fromLatin1(raw.toHex().toLower());
    return m_digest;
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
