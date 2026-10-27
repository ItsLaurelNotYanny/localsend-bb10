#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <QObject>
#include <QTcpServer>
#include <QHostAddress>
#include <QList>

class HttpConnection;

namespace localsend {
class LocalIdentity;
class PosixTlsSocket;
}

struct posix_tls_context;
typedef struct posix_tls_context posix_tls_context_t;

class InboundTcpServer : public QTcpServer {
    Q_OBJECT
public:
    explicit InboundTcpServer(QObject *parent = 0) : QTcpServer(parent) {}
signals:
    void nativeSocketAccepted(int socketFd);
protected:
#if QT_VERSION >= 0x050000
    virtual void incomingConnection(qintptr socketDescriptor) {
        emit nativeSocketAccepted(static_cast<int>(socketDescriptor));
    }
#else
    virtual void incomingConnection(int socketDescriptor) {
        emit nativeSocketAccepted(socketDescriptor);
    }
#endif
};

class HttpServer : public QObject {
    Q_OBJECT

public:
    static const int MAX_ACTIVE_CONNECTIONS = 8; // Socket budget from project specifications
    static const int DEFAULT_HANDSHAKE_TIMEOUT_MS = 10000;

    explicit HttpServer(QObject *parent = 0);
    virtual ~HttpServer();

    bool setTlsIdentity(const localsend::LocalIdentity &identity, QString *error = 0);
    bool setSecure(bool secure, QString *error = 0);
    bool isSecure() const { return m_isSecure; }

    void setHandshakeTimeout(int timeoutMs) { m_handshakeTimeoutMs = timeoutMs; }
    int handshakeTimeout() const { return m_handshakeTimeoutMs; }

    bool listen(const QHostAddress &address, quint16 port, QString *error = 0);
    void stop();

    bool isListening() const;
    quint16 serverPort() const;
    QHostAddress serverAddress() const;
    int activeConnectionsCount() const { return m_connections.size(); }
    int handshakingSocketsCount() const { return m_handshakingSockets.size(); }

signals:
    void connectionOpened(HttpConnection *connection);

private slots:
    void onNativeSocketAccepted(int socketFd);
    void onTlsSocketEncrypted();
    void onTlsSocketFailed(int code, const QString &message);
    void onTlsSocketDisconnected();
    void onConnectionDisconnected();

private:
    void destroyTlsContext();

    InboundTcpServer *m_tcpServer;
    bool m_isSecure;
    int m_handshakeTimeoutMs;
    posix_tls_context_t *m_tlsContext;
    QList<localsend::PosixTlsSocket*> m_handshakingSockets;
    QList<HttpConnection*> m_connections;
};

#endif // HTTP_SERVER_H
