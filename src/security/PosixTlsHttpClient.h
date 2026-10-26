#ifndef POSIX_TLS_HTTP_CLIENT_H
#define POSIX_TLS_HTTP_CLIENT_H

#include <QObject>
#include <QString>
#include <QHostAddress>
#include <QByteArray>
#include <QMap>
#include <QTimer>
#include <QIODevice>

#include "security/LocalIdentity.h"
#include "security/PosixTlsSocket.h"

namespace localsend {

// Minimal non-blocking HTTP/1.1 client adapter for outgoing HTTPS requests over PosixTlsSocket.
// Enforces strict leaf certificate DER SHA-256 fingerprint pinning via posix-tls.
// Ownership & lifecycle rules:
// - Owns posix_tls_context_t and PosixTlsSocket; PosixTlsSocket is destroyed before context.
// - Zero application payload or transfer credentials transmitted before TLS handshake completes.
// - Supports bounded streaming upload from QIODevice with backpressure and finite deadlines.
class PosixTlsHttpClient : public QObject {
    Q_OBJECT

public:
    enum State {
        State_Idle,
        State_Connecting,
        State_Sending,
        State_AwaitingResponse,
        State_Finished,
        State_Failed,
        State_Aborted
    };

    explicit PosixTlsHttpClient(const LocalIdentity &identity = LocalIdentity(), QObject *parent = 0);
    virtual ~PosixTlsHttpClient();

    bool sendRequest(const QString &method,
                     const QHostAddress &address,
                     quint16 port,
                     const QString &pathAndQuery,
                     const QString &expectedPin,
                     const QMap<QString, QString> &headers,
                     const QByteArray &body,
                     int timeoutMs = 15000);

    bool sendStreamRequest(const QString &method,
                           const QHostAddress &address,
                           quint16 port,
                           const QString &pathAndQuery,
                           const QString &expectedPin,
                           const QMap<QString, QString> &headers,
                           QIODevice *bodyStream,
                           qint64 bodySize,
                           int timeoutMs = 30000);

    void abort();

    State state() const { return m_state; }
    int statusCode() const { return m_statusCode; }
    QString statusText() const { return m_statusText; }
    QMap<QString, QString> responseHeaders() const { return m_responseHeaders; }
    QByteArray responseBody() const { return m_responseBody; }

    int lastTlsError() const { return m_lastTlsError; }
    QString errorString() const { return m_errorString; }
    QString peerFingerprint() const { return m_peerFingerprint; }

signals:
    void uploadProgress(qint64 bytesSent, qint64 totalBytes);
    void finished(int statusCode, const QByteArray &responseBody);
    void failed(int tlsErrorCode, const QString &errorMessage);

private slots:
    void onSocketEncrypted();
    void onSocketReadyRead();
    void onSocketBytesWritten(qint64 bytes);
    void onSocketDisconnected();
    void onSocketTlsError(int code, const QString &msg);
    void onTimeout();

private:
    void cleanupSocket();
    void pumpStream();
    void parseResponse();
    void finishSuccess();
    void finishError(int code, const QString &msg);

    LocalIdentity m_identity;
    posix_tls_context_t *m_ctx;
    PosixTlsSocket *m_socket;
    QTimer *m_timer;

    State m_state;
    QString m_method;
    QHostAddress m_address;
    quint16 m_port;
    QString m_pathAndQuery;
    QString m_expectedPin;
    QMap<QString, QString> m_requestHeaders;
    QByteArray m_requestBody;
    QIODevice *m_bodyStream;
    qint64 m_bodySize;
    qint64 m_bodyBytesWritten;

    int m_statusCode;
    QString m_statusText;
    QMap<QString, QString> m_responseHeaders;
    QByteArray m_readBuffer;
    QByteArray m_responseBody;
    qint64 m_contentLength;
    bool m_headersParsed;
    bool m_chunkedResponse;

    int m_lastTlsError;
    QString m_errorString;
    QString m_peerFingerprint;
};

} // namespace localsend

#endif // POSIX_TLS_HTTP_CLIENT_H
