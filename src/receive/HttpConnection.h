#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QMap>
#include <QHostAddress>
#include <QTimer>
#include "protocol/ProtocolTypes.h"

namespace localsend {
class PosixTlsSocket;
}

class HttpConnection : public QObject {
    Q_OBJECT

public:
    static const int MAX_HEADER_SIZE = 16384;      // 16 KiB header limit
    static const int MAX_TRAILER_SIZE = 4096;      // 4 KiB chunk trailers limit
    static const int MAX_BUFFER_CHUNK = 262144;    // 256 KiB socket read chunk target

    enum ConnectionState {
        State_ReadingHeaders,
        State_AwaitingBodyPermission,
        State_ReadingBody,
        State_WaitingResponse,
        State_Responded,
        State_Closed
    };

    enum ChunkState {
        Chunk_ReadSize,
        Chunk_ReadData,
        Chunk_ReadDataCrlf,
        Chunk_ReadTrailers
    };

    static const int DEFAULT_REQUEST_TIMEOUT_MS = 10000;

    explicit HttpConnection(QTcpSocket *socket, QObject *parent = 0);
    explicit HttpConnection(localsend::PosixTlsSocket *socket, QObject *parent = 0);
    virtual ~HttpConnection();

    const HttpRequestHead &requestHead() const { return m_head; }
    ConnectionState state() const { return m_state; }

    bool isSecure() const { return m_isSecure; }
    QHostAddress peerAddress() const { return m_peerAddress; }
    quint16 peerPort() const { return m_peerPort; }
    QString peerFingerprint() const;
    QString negotiatedTlsVersion() const;
    QString negotiatedCipher() const;

    void setRequestTimeout(int timeoutMs);
    int requestTimeout() const { return m_requestTimeoutMs; }
    void stopRequestTimeout();
    void setUploadStream(bool isUpload);
    bool isUploadStream() const { return m_isUploadStream; }

    QTcpSocket *socket() const { return m_tcpSocket; }
    localsend::PosixTlsSocket *tlsSocket() const { return m_tlsSocket; }
    QIODevice *device() const;

public slots:
    void allowBody(); // Send 100 Continue if requested, and start streaming body
    void respond(int status, const QByteArray &contentType = QByteArray(),
                 const QByteArray &body = QByteArray());
    void abort();

signals:
    void headersReady(const HttpRequestHead &head);
    void bodyChunk(const QByteArray &chunk);
    void requestFinished();
    void disconnected();

private slots:
    void onReadyRead();
    void onSocketDisconnected();
    void onSocketError(QAbstractSocket::SocketError error);
    void onTlsError(int code, const QString &message);
    void onRequestTimeout();

private:
    void processHeaderBuffer();
    bool parseRequestHeaders(const QByteArray &headerData, QString *error);
    void processBodyData();
    void processFixedBody();
    void processChunkedBody();
    void sendErrorResponse(int status, const QByteArray &message);
    static QByteArray statusReason(int status);

    qint64 bytesAvailable() const;
    QByteArray readRaw(qint64 maxLen);
    qint64 writeRaw(const QByteArray &data);
    void flushRaw();
    void disconnectRaw();
    void abortRaw();
    bool isOpen() const;

    QTcpSocket *m_tcpSocket;
    localsend::PosixTlsSocket *m_tlsSocket;
    bool m_isSecure;
    QHostAddress m_peerAddress;
    quint16 m_peerPort;
    ConnectionState m_state;
    HttpRequestHead m_head;

    bool m_expect100;
    bool m_bodyAllowed;

    QByteArray m_headerBuffer;
    QByteArray m_bodyBuffer;

    qint64 m_bodyBytesRead;

    // Chunked transfer decoding state
    ChunkState m_chunkState;
    qint64 m_currentChunkSize;
    qint64 m_currentChunkRead;

    QTimer *m_requestTimer;
    int m_requestTimeoutMs;
    bool m_isUploadStream;
};

#endif // HTTP_CONNECTION_H
