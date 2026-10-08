#include "receive/HttpServer.h"
#include "receive/HttpConnection.h"

HttpServer::HttpServer(QObject *parent)
    : QObject(parent)
    , m_tcpServer(new QTcpServer(this))
{
    connect(m_tcpServer, SIGNAL(newConnection()), this, SLOT(onNewConnection()));
}

HttpServer::~HttpServer()
{
    stop();
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

void HttpServer::onNewConnection()
{
    while (m_tcpServer->hasPendingConnections()) {
        QTcpSocket *socket = m_tcpServer->nextPendingConnection();
        if (!socket) {
            continue;
        }

        // Enforce socket budget (max 8 active connections)
        if (m_connections.size() >= MAX_ACTIVE_CONNECTIONS) {
            QByteArray resp = "HTTP/1.1 503 Service Unavailable\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n\r\n";
            socket->write(resp);
            socket->flush();
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }

        HttpConnection *connection = new HttpConnection(socket, this);
        m_connections.append(connection);

        connect(connection, SIGNAL(disconnected()),
                this, SLOT(onConnectionDisconnected()));

        emit connectionOpened(connection);
    }
}

void HttpServer::onConnectionDisconnected()
{
    HttpConnection *connection = qobject_cast<HttpConnection*>(sender());
    if (connection) {
        m_connections.removeAll(connection);
    }
}
