#ifndef RECEIVE_SERVER_H
#define RECEIVE_SERVER_H

#include <QObject>
#include <QString>
#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QTimer>
#include <QFile>

#include "protocol/ProtocolTypes.h"
#include "protocol/CryptoUtil.h"

namespace localsend {
class Application;
}

class HttpServer;
class HttpConnection;
class DiscoveryManager;

class ReceiveServer : public QObject {
    Q_OBJECT

public:
    explicit ReceiveServer(localsend::Application *app,
                           DiscoveryManager *discovery = 0,
                           QObject *parent = 0);
    virtual ~ReceiveServer();

    void attach(HttpServer *server);
    void setDiscoveryManager(DiscoveryManager *discovery) { m_discovery = discovery; }

    // T05: Asynchronous consent entry points
    void accept(const QString &requestId, const QString &fileId);
    void reject(const QString &requestId);

    // T06: Core upload handling entry points
    bool beginUpload(const HttpRequestHead &head, int *httpStatus = 0, QString *error = 0);
    bool appendUpload(const QByteArray &chunk, int *httpStatus = 0, QString *error = 0);
    bool finishUpload(QString *savedPath = 0, int *httpStatus = 0, QString *error = 0);

    // T08: Cancellation and lifecycle entry points
    void cancelLocal(const QString &requestId);
    void handleCancel(const QString &sessionId, const QHostAddress &peer);
    void onNetworkLost();

    // Query state
    bool hasPendingOffer() const { return !m_pendingOffer.requestId.isEmpty(); }
    QString pendingRequestId() const { return m_pendingOffer.requestId; }
    bool hasActiveSession() const { return !m_activeSession.sessionId.isEmpty(); }
    QString activeSessionId() const { return m_activeSession.sessionId; }

signals:
    // T05
    void incomingOffer(const QString &requestId, const PeerDevice &peer,
                       const QList<FileMeta> &files);

    // T06
    void receiveProgress(const QString &sessionId, qint64 done, qint64 total);
    void receiveFinished(const QString &sessionId, const QString &savedPath);

    // T08
    void receiveCancelled(const QString &sessionId, const QString &reason);
    void remoteCancelRequested(const QString &sessionId, const QHostAddress &peer);

private slots:
    void onConnectionOpened(HttpConnection *connection);
    void onHeadersReady(const HttpRequestHead &head);
    void onBodyChunk(const QByteArray &chunk);
    void onRequestFinished();
    void onConnectionDisconnected();

    void onConsentTimeout();
    void onIdleUploadTimeout();
    void onInactivityTimeout();

private:
    struct PendingOffer {
        QString requestId;
        PeerDevice peer;
        QMap<QString, FileMeta> files;
        HttpConnection *connection;
    };

    struct ActiveSession {
        QString requestId;
        QString sessionId;
        QString fileId;
        QString token;
        FileMeta meta;
        QHostAddress peerAddress;
        quint16 peerPort;
        QString partFilePath;
        QFile *partFile;
        Sha256Context shaCtx;
        qint64 bytesReceived;
        bool isUploading;
        HttpConnection *connection;
    };

    struct ConnectionState {
        HttpRequestHead head;
        QByteArray bodyBuffer;
        bool isUpload;
    };

    void handleInfoRoute(HttpConnection *conn);
    void handleRegisterRoute(HttpConnection *conn, const QByteArray &body);
    void handlePrepareUploadRoute(HttpConnection *conn, const QByteArray &body);
    void handleCancelRoute(HttpConnection *conn, const HttpRequestHead &head);

    void clearPendingOffer();
    void cleanupActiveSession(bool committed);
    QString resolveUniqueDestination(const QString &dirPath, const QString &fileName) const;

    localsend::Application *m_app;
    DiscoveryManager *m_discovery;
    HttpServer *m_server;

    PendingOffer m_pendingOffer;
    ActiveSession m_activeSession;

    QMap<HttpConnection*, ConnectionState> m_connStates;

    QTimer *m_consentTimer;     // 120s awaiting user consent
    QTimer *m_idleUploadTimer;  // 60s awaiting upload start after consent
    QTimer *m_inactivityTimer;  // 30s stalled upload timeout
};

#endif // RECEIVE_SERVER_H
