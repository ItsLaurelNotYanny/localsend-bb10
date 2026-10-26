#ifndef POSIX_TLS_SOCKET_H
#define POSIX_TLS_SOCKET_H

#include <QIODevice>
#include <QHostAddress>
#include <QSocketNotifier>
#include <QTimer>
#include <QByteArray>
#include <QString>

#include <posix_tls/posix_tls.h>

namespace localsend {

// Non-blocking Qt4/C++03 QIODevice transport backed by posix-tls.
// Strict ownership contract:
// - Before posix_tls_connection_create succeeds, PosixTlsSocket owns m_rawFd.
// - Once posix_tls_connection_create succeeds, posix_tls_connection_t owns the fd;
//   posix_tls_connection_destroy closes the fd exactly once.
// - QSocketNotifier never closes the fd; it is disabled and deleted before destroying the connection.
class PosixTlsSocket : public QIODevice {
    Q_OBJECT

public:
    static const int DEFAULT_HANDSHAKE_TIMEOUT_MS = 10000;
    static const int DEFAULT_SHUTDOWN_TIMEOUT_MS  = 2000;
    static const int DEFAULT_WRITE_TIMEOUT_MS     = 10000;
    static const int DEFAULT_READ_BUFFER_CAP      = 262144; // 256 KiB decrypted read buffer cap
    static const int MAX_WRITE_BUFFER_CAP         = 1048576; // 1 MiB pending write queue cap
    static const int MAX_DRAIN_PER_EVENT          = 262144; // Max decrypted bytes drained per event turn

    enum SocketState {
        State_Unconnected,
        State_ConnectingTcp,
        State_Handshaking,
        State_Encrypted,
        State_Closing,
        State_Closed,
        State_Error
    };

    explicit PosixTlsSocket(posix_tls_context_t *ctx, QObject *parent = 0);
    virtual ~PosixTlsSocket();

    // Initiate non-blocking client TCP connection and TLS handshake to host:port
    bool connectToHostEncrypted(const QHostAddress &address, quint16 port,
                                const QString &hostname = QString(),
                                int timeoutMs = DEFAULT_HANDSHAKE_TIMEOUT_MS);

    // Adopt an already-connected non-blocking native socket descriptor for client or server TLS handshake.
    // On return true, ownership of socketFd is transferred to PosixTlsSocket / posix_tls_connection_t.
    // On return false, caller retains ownership of socketFd (or if connection create failed after socket takeover,
    // caller should check error). To make ownership 100% unambiguous: startServerEncryption / startClientEncryption
    // ALWAYS takes ownership of socketFd (closing it immediately if initialization fails).
    bool startServerEncryption(int socketFd, int timeoutMs = DEFAULT_HANDSHAKE_TIMEOUT_MS);
    bool startClientEncryption(int socketFd, const QString &hostname = QString(),
                               int timeoutMs = DEFAULT_HANDSHAKE_TIMEOUT_MS);

    void setReadBufferSize(qint64 size);
    qint64 readBufferSize() const { return m_readBufferCap; }

    void setHandshakeTimeout(int timeoutMs) { m_handshakeTimeoutMs = timeoutMs; }
    int handshakeTimeout() const { return m_handshakeTimeoutMs; }

    void setWriteTimeout(int timeoutMs);
    int writeTimeout() const { return m_writeTimeoutMs; }

    SocketState state() const { return m_state; }
    bool isEncrypted() const { return m_state == State_Encrypted; }
    int lastTlsErrorCode() const { return m_lastTlsErr; }
    int socketDescriptor() const { return activeFd(); }

    QHostAddress peerAddress() const { return m_peerAddress; }
    quint16 peerPort() const { return m_peerPort; }

    QString negotiatedTlsVersion() const;
    QString negotiatedCipher() const;
    QString peerFingerprint() const;

    // QIODevice overrides
    virtual bool isSequential() const { return true; }
    virtual bool atEnd() const;
    virtual qint64 bytesAvailable() const;
    virtual qint64 bytesToWrite() const;
    virtual bool canReadLine() const;
    virtual void close();

    // Flush queued plaintext and initiate graceful TLS close_notify shutdown
    void disconnectFromHost();
    // Immediate abort and descriptor teardown
    void abort();
    // Attempt immediate non-blocking write flush
    bool flush();

    // Test / diagnostic inspection of exact retry buffer preservation
    int wantWriteCount() const { return m_wantWriteCount; }
    int wantReadCount() const { return m_wantReadCount; }
    int writeRetrySameBufferCount() const { return m_writeRetrySameBufferCount; }

signals:
    void connected();
    void encrypted();
    void disconnected();
    void tlsError(int posixTlsErrorCode, const QString &message);

protected:
    virtual qint64 readData(char *data, qint64 maxSize);
    virtual qint64 writeData(const char *data, qint64 maxSize);

private slots:
    void advanceHandshake();
    void onReadActivated(int fd);
    void onWriteActivated(int fd);
    void onDrainMore();
    void onTimeout();

private:
    bool adoptDescriptorAndStartHandshake(int fd, bool isServer, const QString &hostname, int timeoutMs);
    void setupNotifiers(int fd);
    void teardownNotifiers();
    void destroyConnectionAndFd();
    int activeFd() const;

    void advanceTcpConnect();
    void advanceReads();
    void advanceWrites();
    void advanceShutdown();
    void updateNotifierInterests();
    void failSocket(int tlsErrCode, const QString &reason);
    void completeClose();

    posix_tls_context_t *m_ctx;
    posix_tls_connection_t *m_conn;
    int m_rawFd; // Valid (>= 0) ONLY before m_conn is created; -1 once m_conn owns the fd

    bool m_isServer;
    QString m_hostname;
    QHostAddress m_peerAddress;
    quint16 m_peerPort;

    SocketState m_state;
    int m_lastTlsErr;
    bool m_handshakeWantWrite;
    bool m_readWantWrite;
    bool m_writeWantRead;
    bool m_shutdownWantWrite;
    bool m_shutdownWantRead;
    bool m_peerClosedCleanly;
    bool m_hasUnreadSslData;

    QString m_negotiatedTlsVersion;
    QString m_negotiatedCipher;
    QString m_peerFingerprint;

    qint64 m_readBufferCap;
    QByteArray m_readBuffer;

    // Pending write queue: m_activeWriteBlock holds the exact buffer currently passed to posix_tls_write
    // so that WANT_READ / WANT_WRITE retries pass the exact same pointer and length.
    QByteArray m_activeWriteBlock;
    size_t m_activeWriteOffset;
    QByteArray m_queuedWriteData;

    QSocketNotifier *m_readNotifier;
    QSocketNotifier *m_writeNotifier;
    QTimer *m_deadlineTimer;
    QTimer *m_drainTimer;
    QTimer *m_writeProgressTimer;
    int m_handshakeTimeoutMs;
    int m_writeTimeoutMs;

    int m_wantWriteCount;
    int m_wantReadCount;
    int m_writeRetrySameBufferCount;
    const char *m_lastWriteRetryPtr;
    size_t m_lastWriteRetryLen;
};

} // namespace localsend

#endif // POSIX_TLS_SOCKET_H
