#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QMap>
#include <QHostAddress>
#include "protocol/ProtocolTypes.h"

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

    explicit HttpConnection(QTcpSocket *socket, QObject *parent = 0);
    virtual ~HttpConnection();

    const HttpRequestHead &requestHead() const { return m_head; }
    ConnectionState state() const { return m_state; }
    QTcpSocket *socket() const { return m_socket; }

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

private:
    void processHeaderBuffer();
    bool parseRequestHeaders(const QByteArray &headerData, QString *error);
    void processBodyData();
    void processFixedBody();
    void processChunkedBody();
    void sendErrorResponse(int status, const QByteArray &message);
    static QByteArray statusReason(int status);

    QTcpSocket *m_socket;
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
};

#endif // HTTP_CONNECTION_H
