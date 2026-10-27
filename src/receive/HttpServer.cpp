#include "receive/HttpServer.h"
#include "receive/HttpConnection.h"
#include "security/PosixTlsSocket.h"
#include "security/LocalIdentity.h"

#include <posix_tls/posix_tls.h>

#include <sys/socket.h>
#include <unistd.h>
#include <QDebug>

HttpServer::HttpServer(QObject *parent)
    : QObject(parent)
    , m_tcpServer(new InboundTcpServer(this))
    , m_isSecure(false)
    , m_handshakeTimeoutMs(DEFAULT_HANDSHAKE_TIMEOUT_MS)
    , m_tlsContext(0)
{
    connect(m_tcpServer, SIGNAL(nativeSocketAccepted(int)),
            this, SLOT(onNativeSocketAccepted(int)));
}

HttpServer::~HttpServer()
{
    stop();
    destroyTlsContext();
}

void HttpServer::destroyTlsContext()
{
    if (m_tlsContext) {
        posix_tls_context_destroy(m_tlsContext);
        m_tlsContext = 0;
    }
}

void HttpServer::setTlsIdentity(const localsend::LocalIdentity &identity)
{
    destroyTlsContext();
    if (!identity.isValid()) {
        m_isSecure = false;
        return;
    }

    m_tlsContext = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    if (!m_tlsContext) {
        qWarning() << "[HttpServer] Failed to create posix_tls_context_t";
        m_isSecure = false;
        return;
    }

    int rc = posix_tls_context_set_credentials(
        m_tlsContext,
        identity.certPem().constData(), static_cast<size_t>(identity.certPem().size()),
        identity.keyPem().constData(), static_cast<size_t>(identity.keyPem().size()));

    if (rc != POSIX_TLS_OK) {
        qWarning() << "[HttpServer] Failed to configure server identity credentials:" << rc;
        destroyTlsContext();
        m_isSecure = false;
        return;
    }

    m_isSecure = true;
}

void HttpServer::setSecure(bool secure)
{
    if (secure && !m_tlsContext) {
        qWarning() << "[HttpServer] Cannot enable TLS without valid credentials context";
        m_isSecure = false;
        return;
    }
    m_isSecure = secure;
}

bool HttpServer::listen(const QHostAddress &address, quint16 port, QString *error)
{
    stop();

    if (!m_tcpServer->listen(address, port)) {
        if (error) {
            *error = m_tcpServer->errorString();
        }
        return false;
    }

    return true;
}

void HttpServer::stop()
{
    if (m_tcpServer && m_tcpServer->isListening()) {
        m_tcpServer->close();
    }

    // Abort and clean all handshaking sockets
    QList<localsend::PosixTlsSocket*> handshaking = m_handshakingSockets;
    m_handshakingSockets.clear();
    for (int i = 0; i < handshaking.size(); ++i) {
        localsend::PosixTlsSocket *s = handshaking.at(i);
        if (s) {
            s->disconnect(this);
            s->abort();
            s->deleteLater();
        }
    }

    // Abort and clean all active connections
    QList<HttpConnection*> conns = m_connections;
    m_connections.clear();
    for (int i = 0; i < conns.size(); ++i) {
        HttpConnection *c = conns.at(i);
        if (c) {
            c->abort();
        }
    }
}

bool HttpServer::isListening() const
{
    return m_tcpServer ? m_tcpServer->isListening() : false;
}

quint16 HttpServer::serverPort() const
{
    return m_tcpServer ? m_tcpServer->serverPort() : 0;
}

QHostAddress HttpServer::serverAddress() const
{
    return m_tcpServer ? m_tcpServer->serverAddress() : QHostAddress();
}

void HttpServer::onNativeSocketAccepted(int socketFd)
{
    if (socketFd < 0) {
        return;
    }

    // Enforce socket budget (max 8 active connections + handshakes)
    if (m_connections.size() + m_handshakingSockets.size() >= MAX_ACTIVE_CONNECTIONS) {
        if (!m_isSecure) {
            const char resp[] = "HTTP/1.1 503 Service Unavailable\r\n"
                                "Content-Length: 0\r\n"
                                "Connection: close\r\n\r\n";
            ::send(socketFd, resp, sizeof(resp) - 1, 0);
        }
        ::close(socketFd);
        return;
    }

    if (m_isSecure) {
        if (!m_tlsContext) {
            ::close(socketFd);
            return;
        }

        localsend::PosixTlsSocket *tlsSocket = new localsend::PosixTlsSocket(m_tlsContext, this);
        m_handshakingSockets.append(tlsSocket);

        connect(tlsSocket, SIGNAL(encrypted()), this, SLOT(onTlsSocketEncrypted()));
        connect(tlsSocket, SIGNAL(tlsError(int,QString)), this, SLOT(onTlsSocketFailed(int,QString)));
        connect(tlsSocket, SIGNAL(disconnected()), this, SLOT(onTlsSocketDisconnected()));

        if (!tlsSocket->startServerEncryption(socketFd, m_handshakeTimeoutMs)) {
            m_handshakingSockets.removeAll(tlsSocket);
            tlsSocket->deleteLater();
        }
    } else {
        QTcpSocket *tcpSocket = new QTcpSocket(this);
        if (!tcpSocket->setSocketDescriptor(socketFd)) {
            delete tcpSocket;
            ::close(socketFd);
            return;
        }

        HttpConnection *connection = new HttpConnection(tcpSocket, this);
        m_connections.append(connection);

        connect(connection, SIGNAL(disconnected()),
                this, SLOT(onConnectionDisconnected()));

        emit connectionOpened(connection);
    }
}

void HttpServer::onTlsSocketEncrypted()
{
    localsend::PosixTlsSocket *tlsSocket = qobject_cast<localsend::PosixTlsSocket*>(sender());
    if (!tlsSocket || !m_handshakingSockets.contains(tlsSocket)) {
        return;
    }

    m_handshakingSockets.removeAll(tlsSocket);
    tlsSocket->disconnect(this);

    HttpConnection *connection = new HttpConnection(tlsSocket, this);
    m_connections.append(connection);

    connect(connection, SIGNAL(disconnected()),
            this, SLOT(onConnectionDisconnected()));

    emit connectionOpened(connection);
}

void HttpServer::onTlsSocketFailed(int code, const QString &message)
{
    Q_UNUSED(code);
    Q_UNUSED(message);
    localsend::PosixTlsSocket *tlsSocket = qobject_cast<localsend::PosixTlsSocket*>(sender());
    if (tlsSocket && m_handshakingSockets.contains(tlsSocket)) {
        m_handshakingSockets.removeAll(tlsSocket);
        tlsSocket->abort();
        tlsSocket->deleteLater();
    }
}

void HttpServer::onTlsSocketDisconnected()
{
    localsend::PosixTlsSocket *tlsSocket = qobject_cast<localsend::PosixTlsSocket*>(sender());
    if (tlsSocket && m_handshakingSockets.contains(tlsSocket)) {
        m_handshakingSockets.removeAll(tlsSocket);
        tlsSocket->deleteLater();
    }
}

void HttpServer::onConnectionDisconnected()
{
    HttpConnection *connection = qobject_cast<HttpConnection*>(sender());
    if (connection) {
        m_connections.removeAll(connection);
    }
}
