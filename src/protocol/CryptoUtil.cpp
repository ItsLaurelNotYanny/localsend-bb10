#include "protocol/CryptoUtil.h"

#include <posix_tls/posix_tls.h>
#include <QFile>
#include <cstring>

QString CryptoUtil::randomHex(int byteCount)
{
    if (byteCount <= 0) {
        return QString();
    }
    QByteArray buf(byteCount, 0);
    int rc = posix_tls_random_bytes(reinterpret_cast<unsigned char*>(buf.data()), static_cast<size_t>(byteCount));
    if (rc != POSIX_TLS_OK) {
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

namespace {

static const unsigned int K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

inline unsigned int rotr32(unsigned int x, unsigned int n)
{
    return (x >> n) | (x << (32u - n));
}

} // namespace

Sha256Context::Sha256Context()
    : m_bitLen(0)
    , m_bufferLen(0)
    , m_finalized(false)
{
    reset();
}

void Sha256Context::reset()
{
    m_state[0] = 0x6a09e667u;
    m_state[1] = 0xbb67ae85u;
    m_state[2] = 0x3c6ef372u;
    m_state[3] = 0xa54ff53au;
    m_state[4] = 0x510e527fu;
    m_state[5] = 0x9b05688cu;
    m_state[6] = 0x1f83d9abu;
    m_state[7] = 0x5be0cd19u;
    m_bitLen = 0;
    m_bufferLen = 0;
    m_finalized = false;
    m_digest.clear();
}

void Sha256Context::transform(const unsigned char block[64])
{
    unsigned int w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<unsigned int>(block[i * 4]) << 24) |
               (static_cast<unsigned int>(block[i * 4 + 1]) << 16) |
               (static_cast<unsigned int>(block[i * 4 + 2]) << 8) |
               (static_cast<unsigned int>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        unsigned int s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    unsigned int a = m_state[0];
    unsigned int b = m_state[1];
    unsigned int c = m_state[2];
    unsigned int d = m_state[3];
    unsigned int e = m_state[4];
    unsigned int f = m_state[5];
    unsigned int g = m_state[6];
    unsigned int h = m_state[7];

    for (int i = 0; i < 64; ++i) {
        unsigned int S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int temp1 = h + S1 + ch + K256[i] + w[i];
        unsigned int S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256Context::update(const char *data, qint64 length)
{
    if (m_finalized || !data || length <= 0) {
        return;
    }

    const unsigned char *in = reinterpret_cast<const unsigned char*>(data);
    size_t remaining = static_cast<size_t>(length);
    m_bitLen += static_cast<unsigned long long>(remaining) * 8ULL;

    while (remaining > 0) {
        size_t take = 64u - m_bufferLen;
        if (take > remaining) {
            take = remaining;
        }
        std::memcpy(m_buffer + m_bufferLen, in, take);
        m_bufferLen += static_cast<unsigned int>(take);
        in += take;
        remaining -= take;

        if (m_bufferLen == 64u) {
            transform(m_buffer);
            m_bufferLen = 0;
        }
    }
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

    m_buffer[m_bufferLen++] = 0x80u;
    if (m_bufferLen > 56u) {
        while (m_bufferLen < 64u) {
            m_buffer[m_bufferLen++] = 0;
        }
        transform(m_buffer);
        m_bufferLen = 0;
    }
    while (m_bufferLen < 56u) {
        m_buffer[m_bufferLen++] = 0;
    }

    for (int i = 0; i < 8; ++i) {
        m_buffer[56 + i] = static_cast<unsigned char>((m_bitLen >> (56 - i * 8)) & 0xFFu);
    }
    transform(m_buffer);

    unsigned char md[32];
    for (int i = 0; i < 8; ++i) {
        md[i * 4]     = static_cast<unsigned char>((m_state[i] >> 24) & 0xFFu);
        md[i * 4 + 1] = static_cast<unsigned char>((m_state[i] >> 16) & 0xFFu);
        md[i * 4 + 2] = static_cast<unsigned char>((m_state[i] >> 8) & 0xFFu);
        md[i * 4 + 3] = static_cast<unsigned char>(m_state[i] & 0xFFu);
    }

    m_finalized = true;
    QByteArray raw(reinterpret_cast<const char*>(md), 32);
    m_digest = QString::fromLatin1(raw.toHex().toLower());
    return m_digest;
}
