#ifndef SEND_MANAGER_H
#define SEND_MANAGER_H

#include <QObject>
#include <QString>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QTimer>
#include <QDebug>

#include "protocol/ProtocolTypes.h"

namespace localsend {
class Application;
}

class SendManager : public QObject {
    Q_OBJECT

public:
    enum SendState {
        State_Idle,
        State_Preparing,
        State_Uploading,
        State_Completed,
        State_Failed,
        State_Cancelled
    };

    enum FailureReason {
        Failure_None,
        Failure_LocalFileNotFound,
        Failure_PeerRejected,
        Failure_PeerBusy,
        Failure_PeerPinRequired,
        Failure_NetworkError,
        Failure_Timeout,
        Failure_ChecksumMismatch,
        Failure_ProtocolError
    };

    explicit SendManager(localsend::Application *app,
                         QNetworkAccessManager *nam,
                         QObject *parent = 0);
    virtual ~SendManager();

    bool sendFile(const PeerDevice &peer, const QString &filePath, QString *error = 0);
    void cancel();

    SendState state() const { return m_state; }
    QString stateString() const;
    FailureReason failureReason() const { return m_failureReason; }
    QString failureReasonString() const;

    QString activeRequestId() const { return m_requestId; }
    QString activeSessionId() const { return m_sessionId; }

public slots:
    void handleRemoteCancel(const QString &sessionId, const QHostAddress &peer);
    void onNetworkLost();

signals:
    void sendStateChanged(const QString &requestId, const QString &state);
    void sendProgress(const QString &requestId, qint64 done, qint64 total);
    void sendFinished(const QString &requestId);
    void sendFailed(const QString &requestId, const QString &reason);
    void sendFailedWithReason(const QString &requestId, const QString &reason, int failureCode);

private slots:
    void onPrepareReplyFinished();
    void onUploadReplyFinished();
    void onUploadProgress(qint64 bytesSent, qint64 bytesTotal);
    void onConsentTimeout();
    void onInactivityTimeout();

private:
    void transitionState(SendState newState);
    void fail(const QString &reason, FailureReason failureCode);
    void cleanup(bool releaseSlot);
    void sendRemoteCancel(const PeerDevice &peer, const QString &sessionId);

    localsend::Application *m_app;
    QNetworkAccessManager *m_nam;

    SendState m_state;
    FailureReason m_failureReason;

    QString m_requestId;
    QString m_sessionId;
    QString m_fileId;
    QString m_token;

    PeerDevice m_peer;
    QString m_filePath;
    qint64 m_fileSize;

    QFile *m_file;
    QNetworkReply *m_prepareReply;
    QNetworkReply *m_uploadReply;

    QTimer *m_consentTimer;     // 120s awaiting peer acceptance
    QTimer *m_inactivityTimer;  // 30s stalled upload timeout
};

#endif // SEND_MANAGER_H
