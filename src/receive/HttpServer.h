#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <QObject>
#include <QTcpServer>
#include <QHostAddress>
#include <QList>

class HttpConnection;

class HttpServer : public QObject {
    Q_OBJECT

public:
    static const int MAX_ACTIVE_CONNECTIONS = 8; // Socket budget from project specifications

    explicit HttpServer(QObject *parent = 0);
    virtual ~HttpServer();

    bool listen(const QHostAddress &address, quint16 port, QString *error = 0);
    void stop();

    bool isListening() const;
    quint16 serverPort() const;
    QHostAddress serverAddress() const;
    int activeConnectionsCount() const { return m_connections.size(); }

signals:
    void connectionOpened(HttpConnection *connection);

private slots:
    void onNewConnection();
    void onConnectionDisconnected();

private:
    QTcpServer *m_tcpServer;
    QList<HttpConnection*> m_connections;
};

#endif // HTTP_SERVER_H
