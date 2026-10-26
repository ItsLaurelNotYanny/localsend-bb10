#include "security/LocalIdentity.h"

#include <posix_tls/posix_tls.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

namespace localsend {

namespace {

bool readRegularFileNoFollow(const QString &path, QByteArray *outData, mode_t *outMode, QString *error)
{
    QByteArray nativePath = QFile::encodeName(path);
    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(nativePath.constData(), flags);
    if (fd < 0) {
        if (error) {
            *error = QString("Failed to open %1: %2").arg(path).arg(QString::fromLocal8Bit(::strerror(errno)));
        }
        return false;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0) {
        if (error) {
            *error = QString("Failed to stat %1: %2").arg(path).arg(QString::fromLocal8Bit(::strerror(errno)));
        }
        ::close(fd);
        return false;
    }

    if (!S_ISREG(st.st_mode)) {
        if (error) {
            *error = QString("Identity file is not a regular file: %1").arg(path);
        }
        ::close(fd);
        return false;
    }

    // Restore blocking mode for reading the regular file
    int curFlags = ::fcntl(fd, F_GETFL, 0);
    if (curFlags >= 0) {
        ::fcntl(fd, F_SETFL, curFlags & ~O_NONBLOCK);
    }

    if (st.st_size <= 0 || st.st_size > 256 * 1024) {
        if (error) {
            *error = QString("Identity file has invalid size (%1 bytes): %2").arg(static_cast<qlonglong>(st.st_size)).arg(path);
        }
        ::close(fd);
        return false;
    }

    if (outMode) {
        *outMode = (st.st_mode & 0777);
    }

    QByteArray buf;
    buf.resize(static_cast<int>(st.st_size));
    size_t totalRead = 0;
    size_t expected = static_cast<size_t>(st.st_size);
    while (totalRead < expected) {
        ssize_t n = ::read(fd, buf.data() + totalRead, expected - totalRead);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (error) {
                *error = QString("Failed to read %1: %2").arg(path).arg(QString::fromLocal8Bit(::strerror(errno)));
            }
            ::close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        totalRead += static_cast<size_t>(n);
    }

    ::close(fd);
    if (totalRead != expected) {
        if (error) {
            *error = QString("Short read on identity file: %1").arg(path);
        }
        return false;
    }

    *outData = buf;
    return true;
}

bool exclusiveCreateFile(const QString &path, const char *data, size_t len, mode_t mode, bool *createdOut, QString *error)
{
    if (createdOut) *createdOut = false;
    QByteArray nativePath = QFile::encodeName(path);

    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif

    int fd = ::open(nativePath.constData(), flags, mode);
    if (fd < 0) {
        if (error) {
            *error = QString("Failed to create %1: %2").arg(path).arg(QString::fromLocal8Bit(::strerror(errno)));
        }
        return false;
    }
    if (createdOut) *createdOut = true;

    if (::fchmod(fd, mode) != 0) {
        int savedErr = errno;
        ::close(fd);
        ::unlink(nativePath.constData());
        if (createdOut) *createdOut = false;
        if (error) {
            *error = QString("Failed to set permissions on %1: %2").arg(path).arg(QString::fromLocal8Bit(::strerror(savedErr)));
        }
        return false;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t n = ::write(fd, data + written, len - written);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            int savedErr = errno;
            ::close(fd);
            ::unlink(nativePath.constData());
            if (createdOut) *createdOut = false;
            if (error) {
                *error = QString("Failed to write %1: %2").arg(path).arg(QString::fromLocal8Bit(::strerror(savedErr)));
            }
            return false;
        }
        written += static_cast<size_t>(n);
    }

    if (::fsync(fd) != 0) {
        int savedErr = errno;
        ::close(fd);
        ::unlink(nativePath.constData());
        if (createdOut) *createdOut = false;
        if (error) {
            *error = QString("Failed to sync %1: %2").arg(path).arg(QString::fromLocal8Bit(::strerror(savedErr)));
        }
        return false;
    }

    if (::close(fd) != 0) {
        int savedErr = errno;
        ::unlink(nativePath.constData());
        if (createdOut) *createdOut = false;
        if (error) {
            *error = QString("Failed to close %1: %2").arg(path).arg(QString::fromLocal8Bit(::strerror(savedErr)));
        }
        return false;
    }

    return true;
}

bool validateCertAndKeyPair(const QByteArray &certPem, const QByteArray &keyPem, QString *outFp, QString *error)
{
    char fpBuf[65];
    int rc = posix_tls_calculate_cert_fingerprint(certPem.constData(), static_cast<size_t>(certPem.size()), fpBuf);
    if (rc != POSIX_TLS_OK) {
        if (error) {
            *error = QString("Invalid certificate PEM: %1").arg(QString::fromLatin1(posix_tls_strerror(rc)));
        }
        return false;
    }

    posix_tls_context_t *ctx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    if (!ctx) {
        if (error) {
            *error = "Failed to create TLS context for identity validation";
        }
        return false;
    }

    rc = posix_tls_context_set_credentials(ctx,
                                           certPem.constData(), static_cast<size_t>(certPem.size()),
                                           keyPem.constData(), static_cast<size_t>(keyPem.size()));
    posix_tls_context_destroy(ctx);

    if (rc != POSIX_TLS_OK) {
        if (error) {
            *error = QString("Certificate and private key validation failed: %1").arg(QString::fromLatin1(posix_tls_strerror(rc)));
        }
        return false;
    }

    if (outFp) {
        *outFp = QString::fromLatin1(fpBuf);
    }
    return true;
}

} // namespace

QString LocalIdentity::calculateFingerprint(const QByteArray &certPem, QString *error)
{
    if (certPem.isEmpty()) {
        if (error) *error = "Empty certificate PEM";
        return QString();
    }
    char fpBuf[65];
    int rc = posix_tls_calculate_cert_fingerprint(certPem.constData(), static_cast<size_t>(certPem.size()), fpBuf);
    if (rc != POSIX_TLS_OK) {
        if (error) *error = QString::fromLatin1(posix_tls_strerror(rc));
        return QString();
    }
    return QString::fromLatin1(fpBuf);
}

QString LocalIdentity::normalizeFingerprint(const QString &rawPin)
{
    QString clean;
    clean.reserve(64);
    for (int i = 0; i < rawPin.length(); ++i) {
        ushort u = rawPin.at(i).unicode();
        if (u == ':' || u == ' ' || u == '-') {
            continue;
        }
        if (u >= '0' && u <= '9') {
            clean.append(QChar(u));
        } else if (u >= 'a' && u <= 'f') {
            clean.append(QChar(u));
        } else if (u >= 'A' && u <= 'F') {
            clean.append(QChar(u - 'A' + 'a'));
        } else {
            return QString();
        }
    }
    if (clean.length() != 64) {
        return QString();
    }
    return clean;
}

bool LocalIdentity::loadOrCreate(const QString &storageDir, LocalIdentity *out, QString *error)
{
    if (!out) {
        if (error) *error = "Null output LocalIdentity pointer";
        return false;
    }
    if (storageDir.trimmed().isEmpty()) {
        if (error) *error = "Empty identity storage directory";
        return false;
    }

    QDir dir(storageDir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            if (error) {
                *error = QString("Failed to create identity storage directory: %1").arg(storageDir);
            }
            return false;
        }
    }

    QString certPath = dir.filePath("device_cert.pem");
    QString keyPath = dir.filePath("device_key.pem");

    QByteArray nativeCertPath = QFile::encodeName(certPath);
    QByteArray nativeKeyPath = QFile::encodeName(keyPath);

    struct stat certSt;
    struct stat keySt;
    bool certExists = (::lstat(nativeCertPath.constData(), &certSt) == 0);
    bool keyExists = (::lstat(nativeKeyPath.constData(), &keySt) == 0);

    // Partial state check: if only one of cert or key exists, fail visibly without overwriting
    if (certExists != keyExists) {
        if (error) {
            *error = QString("Corrupt/partial identity state in %1: certExists=%2, keyExists=%3")
                         .arg(storageDir)
                         .arg(certExists ? "true" : "false")
                         .arg(keyExists ? "true" : "false");
        }
        return false;
    }

    if (certExists && keyExists) {
        QByteArray certPem;
        QByteArray keyPem;
        mode_t keyMode = 0;

        if (!readRegularFileNoFollow(certPath, &certPem, 0, error)) {
            return false;
        }
        if (!readRegularFileNoFollow(keyPath, &keyPem, &keyMode, error)) {
            return false;
        }

        // Reject group/other accessible private key
        if ((keyMode & 0077) != 0) {
            if (error) {
                *error = QString("Insecure private key permissions (0%1) on %2; must be 0600")
                             .arg(QString::number(keyMode, 8))
                             .arg(keyPath);
            }
            return false;
        }

        QString fp;
        if (!validateCertAndKeyPair(certPem, keyPem, &fp, error)) {
            return false;
        }

        out->m_certPath = certPath;
        out->m_keyPath = keyPath;
        out->m_certPem = certPem;
        out->m_keyPem = keyPem;
        out->m_fingerprint = fp;
        return true;
    }

    // Neither file exists: generate a new EC P-256 self-signed identity
    char *rawCert = 0;
    size_t rawCertLen = 0;
    char *rawKey = 0;
    size_t rawKeyLen = 0;

    int rc = posix_tls_generate_self_signed_cert(&rawCert, &rawCertLen, &rawKey, &rawKeyLen);
    if (rc != POSIX_TLS_OK || !rawCert || !rawKey) {
        if (rawCert) posix_tls_free_string(rawCert);
        if (rawKey) posix_tls_free_string(rawKey);
        if (error) {
            *error = QString("Failed to generate self-signed certificate: %1")
                         .arg(QString::fromLatin1(posix_tls_strerror(rc)));
        }
        return false;
    }

    QByteArray certPem(rawCert, static_cast<int>(rawCertLen));
    QByteArray keyPem(rawKey, static_cast<int>(rawKeyLen));
    posix_tls_free_string(rawCert);
    posix_tls_free_string(rawKey);

    QString fp;
    if (!validateCertAndKeyPair(certPem, keyPem, &fp, error)) {
        return false;
    }

    bool certCreated = false;
    bool keyCreated = false;

    if (!exclusiveCreateFile(certPath, certPem.constData(), static_cast<size_t>(certPem.size()), 0644, &certCreated, error)) {
        return false;
    }

    if (!exclusiveCreateFile(keyPath, keyPem.constData(), static_cast<size_t>(keyPem.size()), 0600, &keyCreated, error)) {
        if (certCreated) {
            ::unlink(nativeCertPath.constData());
        }
        return false;
    }

    out->m_certPath = certPath;
    out->m_keyPath = keyPath;
    out->m_certPem = certPem;
    out->m_keyPem = keyPem;
    out->m_fingerprint = fp;
    return true;
}

} // namespace localsend
