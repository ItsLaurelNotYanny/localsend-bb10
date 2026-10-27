#include "security/PosixTlsSocket.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

namespace localsend {

namespace {

bool setFdNonBlocking(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

void queryPeerEndpoint(int fd, QHostAddress *outAddr, quint16 *outPort)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getpeername(fd, reinterpret_cast<struct sockaddr*>(&ss), &len) != 0) {
        return;
    }

    if (ss.ss_family == AF_INET) {
        const struct sockaddr_in *sin = reinterpret_cast<const struct sockaddr_in*>(&ss);
        if (outAddr) {
            *outAddr = QHostAddress(ntohl(sin->sin_addr.s_addr));
        }
        if (outPort) {
            *outPort = ntohs(sin->sin_port);
        }
    } else if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = reinterpret_cast<const struct sockaddr_in6*>(&ss);
        bool isV4Mapped = true;
        for (int i = 0; i < 10; ++i) {
            if (sin6->sin6_addr.s6_addr[i] != 0) {
                isV4Mapped = false;
                break;
            }
        }
        if (isV4Mapped && sin6->sin6_addr.s6_addr[10] == 0xff && sin6->sin6_addr.s6_addr[11] == 0xff) {
            quint32 ipv4 = (static_cast<quint32>(sin6->sin6_addr.s6_addr[12]) << 24) |
                           (static_cast<quint32>(sin6->sin6_addr.s6_addr[13]) << 16) |
                           (static_cast<quint32>(sin6->sin6_addr.s6_addr[14]) << 8)  |
                           (static_cast<quint32>(sin6->sin6_addr.s6_addr[15]));
            if (outAddr) {
                *outAddr = QHostAddress(ipv4);
            }
        } else {
            if (outAddr) {
                *outAddr = QHostAddress(reinterpret_cast<const struct sockaddr*>(&ss));
            }
        }
        if (outPort) {
            *outPort = ntohs(sin6->sin6_port);
        }
    }
}

} // namespace

PosixTlsSocket::PosixTlsSocket(posix_tls_context_t *ctx, QObject *parent)
    : QIODevice(parent)
    , m_ctx(ctx)
    , m_conn(0)
    , m_rawFd(-1)
    , m_isServer(false)
    , m_peerPort(0)
    , m_state(State_Unconnected)
    , m_lastTlsErr(POSIX_TLS_OK)
    , m_handshakeWantWrite(false)
    , m_readWantWrite(false)
    , m_writeWantRead(false)
    , m_shutdownWantWrite(false)
    , m_shutdownWantRead(false)
    , m_peerClosedCleanly(false)
    , m_hasUnreadSslData(false)
    , m_readBufferCap(DEFAULT_READ_BUFFER_CAP)
    , m_activeWriteOffset(0)
    , m_readNotifier(0)
    , m_writeNotifier(0)
    , m_deadlineTimer(new QTimer(this))
    , m_drainTimer(new QTimer(this))
    , m_writeProgressTimer(new QTimer(this))
    , m_handshakeTimeoutMs(DEFAULT_HANDSHAKE_TIMEOUT_MS)
    , m_writeTimeoutMs(DEFAULT_WRITE_TIMEOUT_MS)
    , m_wantWriteCount(0)
    , m_wantReadCount(0)
    , m_writeRetrySameBufferCount(0)
    , m_lastWriteRetryPtr(0)
    , m_lastWriteRetryLen(0)
{
    m_deadlineTimer->setSingleShot(true);
    connect(m_deadlineTimer, SIGNAL(timeout()), this, SLOT(onTimeout()));

    m_drainTimer->setSingleShot(true);
    connect(m_drainTimer, SIGNAL(timeout()), this, SLOT(onDrainMore()));

    m_writeProgressTimer->setSingleShot(true);
    connect(m_writeProgressTimer, SIGNAL(timeout()), this, SLOT(onTimeout()));
}

PosixTlsSocket::~PosixTlsSocket()
{
    abort();
}

int PosixTlsSocket::activeFd() const
{
    if (m_conn) {
        return posix_tls_connection_get_fd(m_conn);
    }
    return m_rawFd;
}

void PosixTlsSocket::setReadBufferSize(qint64 size)
{
    m_readBufferCap = (size > 0) ? size : DEFAULT_READ_BUFFER_CAP;
    if (m_state == State_Encrypted) {
        updateNotifierInterests();
        if (m_hasUnreadSslData && m_readBuffer.size() < m_readBufferCap && !m_drainTimer->isActive()) {
            m_drainTimer->start(0);
        }
    }
}

void PosixTlsSocket::setWriteTimeout(int timeoutMs)
{
    m_writeTimeoutMs = timeoutMs;
    if (m_writeTimeoutMs <= 0) {
        m_writeProgressTimer->stop();
    } else if (bytesToWrite() > 0 && (m_state == State_Encrypted || m_state == State_Closing)) {
        if (!m_writeProgressTimer->isActive()) {
            m_writeProgressTimer->start(m_writeTimeoutMs);
        }
    }
}

bool PosixTlsSocket::connectToHostEncrypted(const QHostAddress &address, quint16 port,
                                            const QString &hostname, int timeoutMs)
{
    abort();
    m_state = State_Unconnected;

    if (!m_ctx) {
        failSocket(POSIX_TLS_ERR_INVALID_ARG, "Null posix_tls_context_t");
        return false;
    }

    if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol || port == 0) {
        failSocket(POSIX_TLS_ERR_INVALID_ARG, "Invalid IPv4 peer address or port");
        return false;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        failSocket(POSIX_TLS_ERR_IO_FAILED, QString("socket() failed: %1").arg(QString::fromLocal8Bit(::strerror(errno))));
        return false;
    }

    if (!setFdNonBlocking(fd)) {
        ::close(fd);
        failSocket(POSIX_TLS_ERR_IO_FAILED, "Failed to set O_NONBLOCK on client socket");
        return false;
    }

    struct sockaddr_in saddr;
    ::memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(address.toIPv4Address());
    saddr.sin_port = htons(port);

    int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr));
    if (rc < 0 && errno != EINPROGRESS) {
        QString errMsg = QString("connect() failed: %1").arg(QString::fromLocal8Bit(::strerror(errno)));
        ::close(fd);
        failSocket(POSIX_TLS_ERR_IO_FAILED, errMsg);
        return false;
    }

    m_rawFd = fd;
    m_isServer = false;
    m_hostname = hostname.isEmpty() ? address.toString() : hostname;
    m_peerAddress = address;
    m_peerPort = port;
    m_handshakeTimeoutMs = (timeoutMs > 0) ? timeoutMs : DEFAULT_HANDSHAKE_TIMEOUT_MS;
    m_lastTlsErr = POSIX_TLS_OK;
    m_peerClosedCleanly = false;

    QIODevice::open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    setupNotifiers(fd);

    if (m_handshakeTimeoutMs > 0) {
        m_deadlineTimer->start(m_handshakeTimeoutMs);
    }

    if (rc == 0) {
        // Connected immediately
        m_state = State_ConnectingTcp;
        emit connected();
        if (m_state != State_ConnectingTcp) {
            if (m_rawFd >= 0) {
                ::close(m_rawFd);
                m_rawFd = -1;
            }
            return false;
        }
        int takeFd = m_rawFd;
        m_rawFd = -1;
        m_conn = posix_tls_connection_create(m_ctx, takeFd);
        if (!m_conn) {
            ::close(takeFd);
            failSocket(POSIX_TLS_ERR_OUT_OF_MEMORY, "posix_tls_connection_create failed");
            return false;
        }
        m_state = State_Handshaking;
        m_handshakeWantWrite = true; updateNotifierInterests(); QMetaObject::invokeMethod(this, "advanceHandshake", Qt::QueuedConnection);
    } else {
        m_state = State_ConnectingTcp;
        updateNotifierInterests();
    }

    return m_state != State_Error && m_state != State_Closed;
}

bool PosixTlsSocket::startServerEncryption(int socketFd, int timeoutMs)
{
    return adoptDescriptorAndStartHandshake(socketFd, true, QString(), timeoutMs);
}

bool PosixTlsSocket::startClientEncryption(int socketFd, const QString &hostname, int timeoutMs)
{
    return adoptDescriptorAndStartHandshake(socketFd, false, hostname, timeoutMs);
}

bool PosixTlsSocket::adoptDescriptorAndStartHandshake(int fd, bool isServer, const QString &hostname, int timeoutMs)
{
    abort();
    m_state = State_Unconnected;

    if (fd < 0) {
        failSocket(POSIX_TLS_ERR_INVALID_ARG, "Invalid socket descriptor");
        return false;
    }

    if (!m_ctx) {
        ::close(fd);
        failSocket(POSIX_TLS_ERR_INVALID_ARG, "Null posix_tls_context_t");
        return false;
    }

    if (!setFdNonBlocking(fd)) {
        ::close(fd);
        failSocket(POSIX_TLS_ERR_IO_FAILED, "Failed to set O_NONBLOCK on adopted socket");
        return false;
    }

    queryPeerEndpoint(fd, &m_peerAddress, &m_peerPort);

    m_conn = posix_tls_connection_create(m_ctx, fd);
    if (!m_conn) {
        // posix_tls_connection_create did not take ownership on failure; close once here
        ::close(fd);
        failSocket(POSIX_TLS_ERR_OUT_OF_MEMORY, "posix_tls_connection_create failed");
        return false;
    }
    // Ownership of fd is now strictly held by m_conn
    m_rawFd = -1;
    m_isServer = isServer;
    m_hostname = hostname.isEmpty() ? m_peerAddress.toString() : hostname;
    m_handshakeTimeoutMs = (timeoutMs > 0) ? timeoutMs : DEFAULT_HANDSHAKE_TIMEOUT_MS;
    m_lastTlsErr = POSIX_TLS_OK;
    m_peerClosedCleanly = false;
    m_state = State_Handshaking;

    QIODevice::open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    setupNotifiers(fd);

    if (m_handshakeTimeoutMs > 0) {
        m_deadlineTimer->start(m_handshakeTimeoutMs);
    }

    advanceHandshake();
    return m_state != State_Error && m_state != State_Closed;
}

void PosixTlsSocket::setupNotifiers(int fd)
{
    teardownNotifiers();
    m_readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    m_readNotifier->setEnabled(false);
    connect(m_readNotifier, SIGNAL(activated(int)), this, SLOT(onReadActivated(int)));

    m_writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, SIGNAL(activated(int)), this, SLOT(onWriteActivated(int)));
}

void PosixTlsSocket::teardownNotifiers()
{
    if (m_readNotifier) {
        m_readNotifier->setEnabled(false);
        m_readNotifier->disconnect(this);
        delete m_readNotifier;
        m_readNotifier = 0;
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(false);
        m_writeNotifier->disconnect(this);
        delete m_writeNotifier;
        m_writeNotifier = 0;
    }
}

void PosixTlsSocket::destroyConnectionAndFd()
{
    m_deadlineTimer->stop();
    m_drainTimer->stop();
    m_writeProgressTimer->stop();
    teardownNotifiers();

    if (m_conn) {
        // Sole owner of the underlying fd; closes it once and invalidates to -1
        posix_tls_connection_destroy(m_conn);
        m_conn = 0;
    }
    if (m_rawFd >= 0) {
        ::close(m_rawFd);
        m_rawFd = -1;
    }
}

void PosixTlsSocket::updateNotifierInterests()
{
    if (!m_readNotifier || !m_writeNotifier) {
        return;
    }

    bool wantRead = false;
    bool wantWrite = false;

    switch (m_state) {
    case State_ConnectingTcp:
        wantWrite = true;
        break;
    case State_Handshaking:
        wantRead = !m_handshakeWantWrite;
        wantWrite = m_handshakeWantWrite;
        break;
    case State_Encrypted: {
        bool hasPendingWrite = (!m_activeWriteBlock.isEmpty() || !m_queuedWriteData.isEmpty());
        bool canAcceptMoreReads = (!m_peerClosedCleanly && m_readBuffer.size() < m_readBufferCap);
        wantRead = (canAcceptMoreReads && !m_readWantWrite) || (hasPendingWrite && m_writeWantRead);
        wantWrite = (hasPendingWrite && !m_writeWantRead) || (canAcceptMoreReads && m_readWantWrite);
        break;
    }
    case State_Closing: {
        bool hasPendingWrite = (!m_activeWriteBlock.isEmpty() || !m_queuedWriteData.isEmpty());
        if (hasPendingWrite) {
            wantRead = m_writeWantRead;
            wantWrite = !m_writeWantRead;
        } else {
            wantRead = m_shutdownWantRead;
            wantWrite = m_shutdownWantWrite || !m_shutdownWantRead;
        }
        break;
    }
    default:
        wantRead = false;
        wantWrite = false;
        break;
    }

    m_readNotifier->setEnabled(wantRead);
    m_writeNotifier->setEnabled(wantWrite);
}

void PosixTlsSocket::onReadActivated(int)
{
    if (m_state == State_Handshaking) {
        advanceHandshake();
    } else if (m_state == State_Encrypted) {
        if (m_writeWantRead) {
            advanceWrites();
        }
        if ((m_state == State_Encrypted || m_state == State_Closing) && !m_readWantWrite) {
            advanceReads();
        }
    } else if (m_state == State_Closing) {
        if (!m_activeWriteBlock.isEmpty() || !m_queuedWriteData.isEmpty()) {
            if (m_writeWantRead) {
                advanceWrites();
            }
        }
        if (m_state == State_Closing && m_activeWriteBlock.isEmpty() && m_queuedWriteData.isEmpty()) {
            if (m_shutdownWantRead) {
                advanceShutdown();
            }
        }
    }
}

void PosixTlsSocket::onWriteActivated(int)
{
    if (m_state == State_ConnectingTcp) {
        advanceTcpConnect();
    } else if (m_state == State_Handshaking) {
        advanceHandshake();
    } else if (m_state == State_Encrypted) {
        if (m_readWantWrite) {
            advanceReads();
        }
        if ((m_state == State_Encrypted || m_state == State_Closing) && !m_writeWantRead) {
            advanceWrites();
        }
    } else if (m_state == State_Closing) {
        if (!m_activeWriteBlock.isEmpty() || !m_queuedWriteData.isEmpty()) {
            if (!m_writeWantRead) {
                advanceWrites();
            }
        }
        if (m_state == State_Closing && m_activeWriteBlock.isEmpty() && m_queuedWriteData.isEmpty()) {
            if (!m_shutdownWantRead) {
                advanceShutdown();
            }
        }
    }
}

void PosixTlsSocket::onDrainMore()
{
    if (m_state == State_Encrypted) {
        advanceReads();
    }
}

void PosixTlsSocket::onTimeout()
{
    QObject *snd = sender();
    if (snd == m_writeProgressTimer) {
        failSocket(POSIX_TLS_ERR_IO_FAILED, "Write progress deadline exceeded");
        return;
    }

    if (m_state == State_ConnectingTcp || m_state == State_Handshaking) {
        failSocket(POSIX_TLS_ERR_IO_FAILED, "TLS handshake deadline exceeded");
    } else if (m_state == State_Closing) {
        bool hasPendingWrites = (!m_activeWriteBlock.isEmpty() || !m_queuedWriteData.isEmpty());
        if (hasPendingWrites) {
            failSocket(POSIX_TLS_ERR_IO_FAILED, "Flush timeout in State_Closing: unsent bytes remain");
        } else {
            failSocket(POSIX_TLS_ERR_IO_FAILED, "TLS shutdown deadline exceeded in State_Closing");
        }
    }
}

void PosixTlsSocket::advanceTcpConnect()
{
    if (m_state != State_ConnectingTcp || m_rawFd < 0) {
        return;
    }

    int soErr = 0;
    socklen_t len = sizeof(soErr);
    if (::getsockopt(m_rawFd, SOL_SOCKET, SO_ERROR, &soErr, &len) < 0 || soErr != 0) {
        int errCode = (soErr != 0) ? soErr : errno;
        failSocket(POSIX_TLS_ERR_IO_FAILED,
                   QString("Async TCP connect failed: %1").arg(QString::fromLocal8Bit(::strerror(errCode))));
        return;
    }

    emit connected();
    if (m_state != State_ConnectingTcp) {
        return;
    }

    int takeFd = m_rawFd;
    m_rawFd = -1;
    m_conn = posix_tls_connection_create(m_ctx, takeFd);
    if (!m_conn) {
        ::close(takeFd);
        failSocket(POSIX_TLS_ERR_OUT_OF_MEMORY, "posix_tls_connection_create failed after TCP connect");
        return;
    }

    m_state = State_Handshaking;
    advanceHandshake();
}

void PosixTlsSocket::advanceHandshake()
{
    if (m_state != State_Handshaking || !m_conn) {
        return;
    }

    QByteArray hostUtf8 = m_hostname.toUtf8();
    const char *hostPtr = hostUtf8.isEmpty() ? "127.0.0.1" : hostUtf8.constData();

    int rc = m_isServer ? posix_tls_accept(m_conn) : posix_tls_connect(m_conn, hostPtr);
    if (rc == POSIX_TLS_OK) {
        m_deadlineTimer->stop();
        m_handshakeWantWrite = false;
        m_state = State_Encrypted;

        char verBuf[32] = {0};
        if (posix_tls_connection_get_negotiated_version(m_conn, verBuf, sizeof(verBuf)) == POSIX_TLS_OK) {
            m_negotiatedTlsVersion = QString::fromLatin1(verBuf);
        }
        char cipherBuf[64] = {0};
        if (posix_tls_connection_get_cipher(m_conn, cipherBuf, sizeof(cipherBuf)) == POSIX_TLS_OK) {
            m_negotiatedCipher = QString::fromLatin1(cipherBuf);
        }
        char fpBuf[65] = {0};
        if (posix_tls_connection_get_peer_fingerprint(m_conn, fpBuf) == POSIX_TLS_OK) {
            m_peerFingerprint = QString::fromLatin1(fpBuf);
        }

        updateNotifierInterests();
        emit encrypted();
        if (m_state != State_Encrypted && m_state != State_Closing) {
            return;
        }
        // Flush any writes queued before handshake completed and drain any early post-handshake data
        if (!m_activeWriteBlock.isEmpty() || !m_queuedWriteData.isEmpty()) {
            advanceWrites();
        }
        if (m_state == State_Encrypted) {
            advanceReads();
        }
        return;
    }

    if (rc == POSIX_TLS_ERR_WANT_READ) {
        m_wantReadCount++;
        m_handshakeWantWrite = false;
        updateNotifierInterests();
        return;
    }
    if (rc == POSIX_TLS_ERR_WANT_WRITE) {
        m_wantWriteCount++;
        m_handshakeWantWrite = true;
        updateNotifierInterests();
        return;
    }

    const char *detail = posix_tls_get_last_error(m_conn);
    QString errMsg = (detail && detail[0] != '\0')
        ? QString::fromUtf8(detail)
        : QString::fromLatin1(posix_tls_strerror(rc));
    failSocket(rc, errMsg);
}

void PosixTlsSocket::advanceReads()
{
    if ((m_state != State_Encrypted && m_state != State_Closing) || !m_conn || m_peerClosedCleanly) {
        return;
    }

    qint64 drainedThisTurn = 0;
    bool gotNewBytes = false;
    char chunk[16384];

    // Drain decrypted frames in a loop until WANT_READ/WANT_WRITE, cap reached, or EOF.
    // Never rely solely on another kernel readability notification when SSL already buffered frames.
    bool stoppedAtBoundary = true;
    while (m_readBuffer.size() < m_readBufferCap && drainedThisTurn < MAX_DRAIN_PER_EVENT) {
        qint64 spaceLeft = m_readBufferCap - m_readBuffer.size();
        size_t toRead = static_cast<size_t>(qMin(static_cast<qint64>(sizeof(chunk)), spaceLeft));
        if (toRead == 0) {
            break;
        }

        size_t n = 0;
        int rc = posix_tls_read(m_conn, chunk, toRead, &n);
        if (rc == POSIX_TLS_OK) {
            m_readWantWrite = false;
            if (n == 0) {
                m_peerClosedCleanly = true;
                stoppedAtBoundary = false;
                break;
            }
            m_readBuffer.append(chunk, static_cast<int>(n));
            drainedThisTurn += static_cast<qint64>(n);
            gotNewBytes = true;
        } else if (rc == POSIX_TLS_ERR_WANT_READ) {
            m_wantReadCount++;
            m_readWantWrite = false;
            stoppedAtBoundary = false;
            break;
        } else if (rc == POSIX_TLS_ERR_WANT_WRITE) {
            m_wantWriteCount++;
            m_readWantWrite = true;
            stoppedAtBoundary = false;
            break;
        } else if (rc == POSIX_TLS_ERR_CLOSED) {
            m_readWantWrite = false;
            m_peerClosedCleanly = true;
            stoppedAtBoundary = false;
            break;
        } else {
            m_hasUnreadSslData = false;
            const char *detail = posix_tls_get_last_error(m_conn);
            QString errMsg = (detail && detail[0] != '\0')
                ? QString::fromUtf8(detail)
                : QString::fromLatin1(posix_tls_strerror(rc));
            if (gotNewBytes) {
                emit readyRead();
                if (m_state == State_Closed || m_state == State_Error) {
                    return;
                }
            }
            failSocket(rc, errMsg);
            return;
        }
    }

    m_hasUnreadSslData = stoppedAtBoundary && !m_peerClosedCleanly;

    // If we hit MAX_DRAIN_PER_EVENT or buffer cap without hitting WANT_READ,
    // schedule a 0ms continuation whenever buffer space is available so remaining SSL internal frames
    // are drained without waiting on kernel select.
    if (m_hasUnreadSslData && m_readBuffer.size() < m_readBufferCap && !m_peerClosedCleanly) {
        if (!m_drainTimer->isActive()) {
            m_drainTimer->start(0);
        }
    }

    updateNotifierInterests();

    if (gotNewBytes) {
        emit readyRead();
        if (m_state == State_Closed || m_state == State_Error) {
            return;
        }
    }

    if (m_hasUnreadSslData && m_readBuffer.size() < m_readBufferCap && !m_peerClosedCleanly) {
        if (!m_drainTimer->isActive()) {
            m_drainTimer->start(0);
        }
    }

    if (m_peerClosedCleanly && m_activeWriteBlock.isEmpty() && m_queuedWriteData.isEmpty()) {
        completeClose();
    }
}

void PosixTlsSocket::advanceWrites()
{
    if ((m_state != State_Encrypted && m_state != State_Closing) || !m_conn) {
        return;
    }

    qint64 totalWrittenThisTurn = 0;

    while (!m_activeWriteBlock.isEmpty() || !m_queuedWriteData.isEmpty()) {
        // Populate m_activeWriteBlock only when the previous active chunk has been completely written.
        // This guarantees that across WANT_READ / WANT_WRITE retries, the exact pointer and length
        // passed to posix_tls_write remain immutable.
        if (m_activeWriteBlock.isEmpty()) {
            int take = qMin(m_queuedWriteData.size(), 16384);
            m_activeWriteBlock = m_queuedWriteData.left(take);
            m_queuedWriteData.remove(0, take);
            m_activeWriteOffset = 0;
        }

        const char *ptr = m_activeWriteBlock.constData() + m_activeWriteOffset;
        size_t len = static_cast<size_t>(m_activeWriteBlock.size()) - m_activeWriteOffset;

        if (m_lastWriteRetryPtr != 0) {
            if (ptr == m_lastWriteRetryPtr && len == m_lastWriteRetryLen) {
                m_writeRetrySameBufferCount++;
            }
            m_lastWriteRetryPtr = 0;
            m_lastWriteRetryLen = 0;
        }

        size_t written = 0;
        int rc = posix_tls_write(m_conn, ptr, len, &written);
        if (rc == POSIX_TLS_OK) {
            m_writeWantRead = false;
            m_activeWriteOffset += written;
            totalWrittenThisTurn += static_cast<qint64>(written);
            if (m_activeWriteOffset >= static_cast<size_t>(m_activeWriteBlock.size())) {
                m_activeWriteBlock.clear();
                m_activeWriteOffset = 0;
            }
        } else if (rc == POSIX_TLS_ERR_WANT_WRITE) {
            m_wantWriteCount++;
            m_writeWantRead = false;
            m_lastWriteRetryPtr = ptr;
            m_lastWriteRetryLen = len;
            break;
        } else if (rc == POSIX_TLS_ERR_WANT_READ) {
            m_wantReadCount++;
            m_writeWantRead = true;
            m_lastWriteRetryPtr = ptr;
            m_lastWriteRetryLen = len;
            break;
        } else if (rc == POSIX_TLS_ERR_CLOSED) {
            failSocket(POSIX_TLS_ERR_CLOSED, "Connection closed by peer while write was pending");
            return;
        } else {
            const char *detail = posix_tls_get_last_error(m_conn);
            QString errMsg = (detail && detail[0] != '\0')
                ? QString::fromUtf8(detail)
                : QString::fromLatin1(posix_tls_strerror(rc));
            failSocket(rc, errMsg);
            return;
        }
    }

    if (totalWrittenThisTurn > 0) {
        if (bytesToWrite() > 0 && m_writeTimeoutMs > 0) {
            m_writeProgressTimer->start(m_writeTimeoutMs);
        }
    }
    if (bytesToWrite() == 0) {
        m_writeProgressTimer->stop();
    } else if (m_writeTimeoutMs > 0 && !m_writeProgressTimer->isActive()) {
        m_writeProgressTimer->start(m_writeTimeoutMs);
    }

    updateNotifierInterests();

    if (totalWrittenThisTurn > 0) {
        emit bytesWritten(totalWrittenThisTurn);
        if (m_state == State_Closed || m_state == State_Error) {
            return;
        }
    }

    if (m_state == State_Closing && m_activeWriteBlock.isEmpty() && m_queuedWriteData.isEmpty()) {
        advanceShutdown();
    }
}

void PosixTlsSocket::advanceShutdown()
{
    if (m_state != State_Closing || !m_conn) {
        completeClose();
        return;
    }

    int rc = posix_tls_shutdown(m_conn);
    if (rc == POSIX_TLS_OK || rc == POSIX_TLS_ERR_CLOSED) {
        completeClose();
        return;
    }
    if (rc == POSIX_TLS_ERR_WANT_READ) {
        m_wantReadCount++;
        m_shutdownWantRead = true;
        m_shutdownWantWrite = false;
        updateNotifierInterests();
        return;
    }
    if (rc == POSIX_TLS_ERR_WANT_WRITE) {
        m_wantWriteCount++;
        m_shutdownWantRead = false;
        m_shutdownWantWrite = true;
        updateNotifierInterests();
        return;
    }

    const char *detail = posix_tls_get_last_error(m_conn);
    QString errMsg = (detail && detail[0] != '\0')
        ? QString::fromUtf8(detail)
        : QString::fromLatin1(posix_tls_strerror(rc));
    failSocket(rc, errMsg);
}

void PosixTlsSocket::disconnectFromHost()
{
    if (m_state == State_Unconnected || m_state == State_Closed || m_state == State_Error || m_state == State_Closing) {
        return;
    }

    if (m_state == State_ConnectingTcp || m_state == State_Handshaking) {
        abort();
        return;
    }

    m_state = State_Closing;
    m_deadlineTimer->start(DEFAULT_SHUTDOWN_TIMEOUT_MS);

    // Flush pending application response bytes before sending TLS close_notify
    if (!m_activeWriteBlock.isEmpty() || !m_queuedWriteData.isEmpty()) {
        advanceWrites();
    } else {
        advanceShutdown();
    }
}

bool PosixTlsSocket::flush()
{
    if (m_state == State_Encrypted || m_state == State_Closing) {
        advanceWrites();
        return m_activeWriteBlock.isEmpty() && m_queuedWriteData.isEmpty();
    }
    return false;
}

void PosixTlsSocket::abort()
{
    bool wasActive = (m_state == State_ConnectingTcp ||
                      m_state == State_Handshaking ||
                      m_state == State_Encrypted ||
                      m_state == State_Closing);

    m_state = State_Closed;
    m_hasUnreadSslData = false;
    m_negotiatedTlsVersion.clear();
    m_negotiatedCipher.clear();
    m_peerFingerprint.clear();
    m_activeWriteBlock.clear();
    m_activeWriteOffset = 0;
    m_queuedWriteData.clear();
    m_lastWriteRetryPtr = 0;
    m_lastWriteRetryLen = 0;
    m_readBuffer.clear();

    destroyConnectionAndFd();
    if (isOpen()) {
        QIODevice::close();
    }

    if (wasActive) {
        emit disconnected();
    }
}

void PosixTlsSocket::close()
{
    disconnectFromHost();
    if (m_state == State_Closed || m_state == State_Unconnected || m_state == State_Error) {
        m_readBuffer.clear();
        if (isOpen()) {
            QIODevice::close();
        }
    }
}

void PosixTlsSocket::failSocket(int tlsErrCode, const QString &reason)
{
    if (m_state == State_Error) {
        return;
    }

    if (m_conn) {
        char fpBuf[65] = {0};
        if (posix_tls_connection_get_peer_fingerprint(m_conn, fpBuf) == POSIX_TLS_OK) {
            m_peerFingerprint = QString::fromLatin1(fpBuf);
        }
    }

    m_lastTlsErr = tlsErrCode;
    setErrorString(reason);
    m_state = State_Error;
    m_hasUnreadSslData = false;

    m_activeWriteBlock.clear();
    m_activeWriteOffset = 0;
    m_queuedWriteData.clear();
    m_lastWriteRetryPtr = 0;
    m_lastWriteRetryLen = 0;

    destroyConnectionAndFd();
    if (isOpen()) {
        QIODevice::close();
    }

    emit tlsError(tlsErrCode, reason);
    emit disconnected();
}

void PosixTlsSocket::completeClose()
{
    if (m_state == State_Closed) {
        return;
    }

    m_state = State_Closed;
    m_hasUnreadSslData = false;
    m_activeWriteBlock.clear();
    m_activeWriteOffset = 0;
    m_queuedWriteData.clear();
    m_lastWriteRetryPtr = 0;
    m_lastWriteRetryLen = 0;
    destroyConnectionAndFd();
    emit disconnected();
}

bool PosixTlsSocket::atEnd() const
{
    return (m_state == State_Closed || m_state == State_Error || m_peerClosedCleanly) &&
           m_readBuffer.isEmpty() && QIODevice::atEnd();
}

qint64 PosixTlsSocket::bytesAvailable() const
{
    return static_cast<qint64>(m_readBuffer.size()) + QIODevice::bytesAvailable();
}

qint64 PosixTlsSocket::bytesToWrite() const
{
    qint64 activeRem = 0;
    if (!m_activeWriteBlock.isEmpty() && m_activeWriteOffset < static_cast<size_t>(m_activeWriteBlock.size())) {
        activeRem = static_cast<qint64>(m_activeWriteBlock.size()) - static_cast<qint64>(m_activeWriteOffset);
    }
    return activeRem + static_cast<qint64>(m_queuedWriteData.size());
}

bool PosixTlsSocket::canReadLine() const
{
    return m_readBuffer.contains('\n') || QIODevice::canReadLine();
}

qint64 PosixTlsSocket::readData(char *data, qint64 maxSize)
{
    if (maxSize <= 0) {
        return 0;
    }
    if (m_readBuffer.isEmpty()) {
        if (m_state == State_Error) {
            return -1;
        }
        return 0;
    }

    bool wasFull = (m_readBuffer.size() >= m_readBufferCap);
    int take = static_cast<int>(qMin(maxSize, static_cast<qint64>(m_readBuffer.size())));
    ::memcpy(data, m_readBuffer.constData(), static_cast<size_t>(take));
    m_readBuffer.remove(0, take);

    if (m_state == State_Encrypted && !m_peerClosedCleanly) {
        updateNotifierInterests();
        if ((wasFull || m_hasUnreadSslData) && m_readBuffer.size() < m_readBufferCap && !m_drainTimer->isActive()) {
            m_drainTimer->start(0);
        }
    }

    return static_cast<qint64>(take);
}

qint64 PosixTlsSocket::writeData(const char *data, qint64 maxSize)
{
    if (m_state != State_Encrypted && m_state != State_Handshaking && m_state != State_ConnectingTcp) {
        setErrorString("Socket is not in a writable state");
        return -1;
    }
    if (maxSize <= 0) {
        return 0;
    }

    if (bytesToWrite() + maxSize > MAX_WRITE_BUFFER_CAP) {
        setErrorString("Write buffer capacity exceeded");
        return -1;
    }

    m_queuedWriteData.append(data, static_cast<int>(maxSize));

    if (m_writeTimeoutMs > 0 && !m_writeProgressTimer->isActive()) {
        m_writeProgressTimer->start(m_writeTimeoutMs);
    }

    if (m_state == State_Encrypted) {
        advanceWrites();
    } else {
        updateNotifierInterests();
    }

    return maxSize;
}

QString PosixTlsSocket::negotiatedTlsVersion() const
{
    if (!m_conn) return m_negotiatedTlsVersion;
    char buf[32] = {0};
    if (posix_tls_connection_get_negotiated_version(m_conn, buf, sizeof(buf)) == POSIX_TLS_OK) {
        return QString::fromLatin1(buf);
    }
    return m_negotiatedTlsVersion;
}

QString PosixTlsSocket::negotiatedCipher() const
{
    if (!m_conn) return m_negotiatedCipher;
    char buf[64] = {0};
    if (posix_tls_connection_get_cipher(m_conn, buf, sizeof(buf)) == POSIX_TLS_OK) {
        return QString::fromLatin1(buf);
    }
    return m_negotiatedCipher;
}

QString PosixTlsSocket::peerFingerprint() const
{
    if (!m_conn) return m_peerFingerprint;
    char buf[65] = {0};
    if (posix_tls_connection_get_peer_fingerprint(m_conn, buf) == POSIX_TLS_OK) {
        return QString::fromLatin1(buf);
    }
    return m_peerFingerprint;
}

} // namespace localsend
