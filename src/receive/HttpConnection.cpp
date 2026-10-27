#include "receive/HttpConnection.h"
#include "security/PosixTlsSocket.h"

#include <QStringList>

HttpConnection::HttpConnection(QTcpSocket *socket, QObject *parent)
    : QObject(parent)
    , m_tcpSocket(socket)
    , m_tlsSocket(0)
    , m_isSecure(false)
    , m_peerPort(0)
    , m_state(State_ReadingHeaders)
    , m_expect100(false)
    , m_bodyAllowed(false)
    , m_bodyBytesRead(0)
    , m_chunkState(Chunk_ReadSize)
    , m_currentChunkSize(0)
    , m_currentChunkRead(0)
{
    if (m_tcpSocket) {
        m_tcpSocket->setParent(this);
        m_tcpSocket->setReadBufferSize(MAX_BUFFER_CHUNK);
        m_peerAddress = m_tcpSocket->peerAddress();
        if (m_peerAddress.protocol() == QAbstractSocket::IPv6Protocol) {
            quint32 ipv4 = m_peerAddress.toIPv4Address();
            if (ipv4 != 0) {
                m_peerAddress = QHostAddress(ipv4);
            }
        }
        m_peerPort = m_tcpSocket->peerPort();
        m_head.peerAddress = m_peerAddress;
        m_head.isSecure = false;

        connect(m_tcpSocket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
        connect(m_tcpSocket, SIGNAL(disconnected()), this, SLOT(onSocketDisconnected()));
        connect(m_tcpSocket, SIGNAL(error(QAbstractSocket::SocketError)),
                this, SLOT(onSocketError(QAbstractSocket::SocketError)));
    }
}

HttpConnection::HttpConnection(localsend::PosixTlsSocket *socket, QObject *parent)
    : QObject(parent)
    , m_tcpSocket(0)
    , m_tlsSocket(socket)
    , m_isSecure(true)
    , m_peerPort(0)
    , m_state(State_ReadingHeaders)
    , m_expect100(false)
    , m_bodyAllowed(false)
    , m_bodyBytesRead(0)
    , m_chunkState(Chunk_ReadSize)
    , m_currentChunkSize(0)
    , m_currentChunkRead(0)
{
    if (m_tlsSocket) {
        m_tlsSocket->setParent(this);
        m_tlsSocket->setReadBufferSize(MAX_BUFFER_CHUNK);
        m_peerAddress = m_tlsSocket->peerAddress();
        if (m_peerAddress.protocol() == QAbstractSocket::IPv6Protocol) {
            quint32 ipv4 = m_peerAddress.toIPv4Address();
            if (ipv4 != 0) {
                m_peerAddress = QHostAddress(ipv4);
            }
        }
        m_peerPort = m_tlsSocket->peerPort();
        m_head.peerAddress = m_peerAddress;
        m_head.isSecure = true;
        m_head.peerFingerprint = m_tlsSocket->peerFingerprint();

        connect(m_tlsSocket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
        connect(m_tlsSocket, SIGNAL(disconnected()), this, SLOT(onSocketDisconnected()));
        connect(m_tlsSocket, SIGNAL(tlsError(int,QString)),
                this, SLOT(onTlsError(int,QString)));
    }
}

HttpConnection::~HttpConnection()
{
    if (isOpen()) {
        abortRaw();
    }
}

QIODevice *HttpConnection::device() const
{
    if (m_tlsSocket) return m_tlsSocket;
    if (m_tcpSocket) return m_tcpSocket;
    return 0;
}

QString HttpConnection::peerFingerprint() const
{
    return m_tlsSocket ? m_tlsSocket->peerFingerprint() : QString();
}

QString HttpConnection::negotiatedTlsVersion() const
{
    return m_tlsSocket ? m_tlsSocket->negotiatedTlsVersion() : QString();
}

QString HttpConnection::negotiatedCipher() const
{
    return m_tlsSocket ? m_tlsSocket->negotiatedCipher() : QString();
}

qint64 HttpConnection::bytesAvailable() const
{
    if (m_tlsSocket) return m_tlsSocket->bytesAvailable();
    if (m_tcpSocket) return m_tcpSocket->bytesAvailable();
    return 0;
}

QByteArray HttpConnection::readRaw(qint64 maxLen)
{
    if (m_tlsSocket) return m_tlsSocket->read(maxLen);
    if (m_tcpSocket) return m_tcpSocket->read(maxLen);
    return QByteArray();
}

qint64 HttpConnection::writeRaw(const QByteArray &data)
{
    if (m_tlsSocket) return m_tlsSocket->write(data);
    if (m_tcpSocket) return m_tcpSocket->write(data);
    return -1;
}

void HttpConnection::flushRaw()
{
    if (m_tlsSocket) m_tlsSocket->flush();
    else if (m_tcpSocket) m_tcpSocket->flush();
}

void HttpConnection::disconnectRaw()
{
    if (m_tlsSocket) m_tlsSocket->disconnectFromHost();
    else if (m_tcpSocket) m_tcpSocket->disconnectFromHost();
}

void HttpConnection::abortRaw()
{
    if (m_tlsSocket) m_tlsSocket->abort();
    else if (m_tcpSocket) m_tcpSocket->abort();
}

bool HttpConnection::isOpen() const
{
    if (m_tlsSocket) return m_tlsSocket->isOpen();
    if (m_tcpSocket) return m_tcpSocket->isOpen();
    return false;
}

void HttpConnection::allowBody()
{
    if (m_state != State_AwaitingBodyPermission) {
        return;
    }

    if (m_expect100 && isOpen()) {
        writeRaw("HTTP/1.1 100 Continue\r\n\r\n");
        flushRaw();
        m_expect100 = false;
    }

    m_bodyAllowed = true;
    m_state = State_ReadingBody;

    processBodyData();

    // Drain any pending socket data if state remains ReadingBody
    if (m_state == State_ReadingBody && bytesAvailable() > 0) {
        onReadyRead();
    }
}

void HttpConnection::respond(int status, const QByteArray &contentType, const QByteArray &body)
{
    if (m_state == State_Responded || m_state == State_Closed || !isOpen()) {
        return;
    }

    m_state = State_Responded;

    QByteArray response;
    response.append("HTTP/1.1 ");
    response.append(QByteArray::number(status));
    response.append(" ");
    response.append(statusReason(status));
    response.append("\r\n");

    if (!contentType.isEmpty()) {
        response.append("Content-Type: ");
        response.append(contentType);
        response.append("\r\n");
    }

    response.append("Content-Length: ");
    response.append(QByteArray::number(body.size()));
    response.append("\r\n");
    response.append("Connection: close\r\n\r\n");

    if (!body.isEmpty()) {
        response.append(body);
    }

    writeRaw(response);
    flushRaw();
    disconnectRaw();
}

void HttpConnection::abort()
{
    if (m_state == State_Closed) {
        return;
    }
    m_state = State_Closed;
    abortRaw();
    emit disconnected();
    deleteLater();
}

void HttpConnection::onReadyRead()
{
    if (m_state == State_ReadingHeaders) {
        qint64 bytesAvail = bytesAvailable();
        if (bytesAvail <= 0) {
            return;
        }
        qint64 toRead = qMin(bytesAvail, static_cast<qint64>(MAX_BUFFER_CHUNK));
        if (m_headerBuffer.size() + toRead > MAX_HEADER_SIZE + MAX_BUFFER_CHUNK) {
            sendErrorResponse(431, "Request Header Fields Too Large");
            return;
        }
        QByteArray chunk = readRaw(toRead);
        m_headerBuffer.append(chunk);
        processHeaderBuffer();
    } else if (m_state == State_AwaitingBodyPermission) {
        // Buffer arriving body data before permission is granted, bounded to MAX_BUFFER_CHUNK
        qint64 bytesAvail = bytesAvailable();
        if (bytesAvail <= 0) {
            return;
        }
        qint64 toRead = qMin(bytesAvail, static_cast<qint64>(MAX_BUFFER_CHUNK));
        QByteArray chunk = readRaw(toRead);
        if (m_bodyBuffer.size() + chunk.size() > MAX_BUFFER_CHUNK) {
            sendErrorResponse(413, "Body buffer exceeded before permission");
            return;
        }
        m_bodyBuffer.append(chunk);
    } else if (m_state == State_ReadingBody) {
        while (isOpen() && bytesAvailable() > 0 && m_state == State_ReadingBody) {
            qint64 bytesAvail = bytesAvailable();
            qint64 toRead = qMin(bytesAvail, static_cast<qint64>(MAX_BUFFER_CHUNK));
            QByteArray chunk = readRaw(toRead);
            if (m_bodyBuffer.size() + chunk.size() > MAX_BUFFER_CHUNK * 2) {
                sendErrorResponse(413, "Body buffer exceeded limit");
                return;
            }
            m_bodyBuffer.append(chunk);
            processBodyData();
        }
    } else {
        // Discard extra data after requestFinished or response
        readRaw(qMin(bytesAvailable(), static_cast<qint64>(MAX_BUFFER_CHUNK)));
    }
}

void HttpConnection::onSocketDisconnected()
{
    if (m_state == State_Closed) {
        return;
    }
    m_state = State_Closed;
    emit disconnected();
    deleteLater();
}

void HttpConnection::onSocketError(QAbstractSocket::SocketError)
{
    abort();
}

void HttpConnection::onTlsError(int code, const QString &message)
{
    Q_UNUSED(code);
    Q_UNUSED(message);
    abort();
}

void HttpConnection::processHeaderBuffer()
{
    int headerEnd = m_headerBuffer.indexOf("\r\n\r\n");
    int delimLen = 4;
    if (headerEnd == -1) {
        headerEnd = m_headerBuffer.indexOf("\n\n");
        delimLen = 2;
    }

    if (headerEnd == -1) {
        if (m_headerBuffer.size() > MAX_HEADER_SIZE) {
            sendErrorResponse(431, "Request Header Fields Too Large");
        }
        return;
    }

    QByteArray headerBytes = m_headerBuffer.left(headerEnd);
    QByteArray remainder = m_headerBuffer.mid(headerEnd + delimLen);
    m_headerBuffer.clear();

    if (headerBytes.size() > MAX_HEADER_SIZE) {
        sendErrorResponse(431, "Request Header Fields Too Large");
        return;
    }

    QString error;
    if (!parseRequestHeaders(headerBytes, &error)) {
        // parseRequestHeaders will have sent appropriate error response
        return;
    }

    if (!remainder.isEmpty()) {
        if (remainder.size() > MAX_BUFFER_CHUNK) {
            sendErrorResponse(413, "Body buffer exceeded limit");
            return;
        }
        m_bodyBuffer.append(remainder);
    }

    m_state = State_AwaitingBodyPermission;
    emit headersReady(m_head);

    // If caller changed state (e.g. responded or aborted) inside headersReady callback
    if (m_state != State_AwaitingBodyPermission) {
        return;
    }

    // If no body is expected, finish immediately
    if (m_head.contentLength == 0 || (m_head.contentLength == -1 && !m_head.chunked)) {
        m_state = State_WaitingResponse;
        emit requestFinished();
    } else if (m_bodyAllowed) {
        m_state = State_ReadingBody;
        processBodyData();
    }
}

bool HttpConnection::parseRequestHeaders(const QByteArray &headerData, QString *error)
{
    QList<QByteArray> lines = headerData.split('\n');
    if (lines.isEmpty()) {
        sendErrorResponse(400, "Empty request");
        return false;
    }

    // Parse Request Line
    QByteArray reqLine = lines.first().trimmed();
    QList<QByteArray> parts = reqLine.split(' ');
    if (parts.size() < 3) {
        sendErrorResponse(400, "Malformed request line");
        return false;
    }

    m_head.method = parts.at(0).toUpper();
    m_head.target = QUrl::fromEncoded(parts.at(1));
    QByteArray version = parts.at(2).toUpper();

    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        sendErrorResponse(505, "HTTP Version Not Supported");
        return false;
    }

    m_head.headers.clear();
    m_head.contentLength = -1;
    m_head.chunked = false;
    m_expect100 = false;

    // Parse Headers
    bool hasContentLength = false;
    for (int i = 1; i < lines.size(); ++i) {
        QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty()) continue;

        int colon = line.indexOf(':');
        if (colon == -1) {
            sendErrorResponse(400, "Malformed header line");
            return false;
        }

        QByteArray key = line.left(colon).trimmed().toLower();
        QByteArray val = line.mid(colon + 1).trimmed();

        // Check for duplicate / ambiguous Content-Length
        if (key == "content-length") {
            if (hasContentLength && m_head.headers.value(key) != val) {
                sendErrorResponse(400, "Conflicting duplicate Content-Length headers");
                return false;
            }
            hasContentLength = true;

            bool ok = false;
            qint64 cl = val.toLongLong(&ok);
            if (!ok || cl < 0) {
                sendErrorResponse(400, "Invalid Content-Length");
                return false;
            }
            m_head.contentLength = cl;
        }

        m_head.headers.insert(key, val);
    }

    // Check Transfer-Encoding
    if (m_head.headers.contains("transfer-encoding")) {
        QByteArray te = m_head.headers.value("transfer-encoding").toLower().trimmed();
        if (te == "chunked") {
            m_head.chunked = true;
        } else {
            sendErrorResponse(501, "Unsupported Transfer-Encoding");
            return false;
        }

        // Prevent CL+TE desync / attack
        if (hasContentLength) {
            sendErrorResponse(400, "Both Content-Length and chunked Transfer-Encoding specified");
            return false;
        }
    }

    // Check Expect header
    if (m_head.headers.contains("expect")) {
        QByteArray exp = m_head.headers.value("expect").trimmed().toLower();
        if (exp == "100-continue") {
            m_expect100 = true;
        } else {
            sendErrorResponse(417, "Expectation Failed");
            return false;
        }
    }

    return true;
}

void HttpConnection::processBodyData()
{
    if (m_state != State_ReadingBody) {
        return;
    }

    if (m_head.chunked) {
        processChunkedBody();
    } else {
        processFixedBody();
    }
}

void HttpConnection::processFixedBody()
{
    while (!m_bodyBuffer.isEmpty() && m_bodyBytesRead < m_head.contentLength) {
        qint64 remaining = m_head.contentLength - m_bodyBytesRead;
        int takeBytes = qMin(static_cast<qint64>(m_bodyBuffer.size()), remaining);

        QByteArray chunk = m_bodyBuffer.left(takeBytes);
        m_bodyBuffer.remove(0, takeBytes);
        m_bodyBytesRead += chunk.size();

        emit bodyChunk(chunk);
        if (m_state != State_ReadingBody) {
            return;
        }
    }

    if (m_state != State_ReadingBody) {
        return;
    }

    if (m_bodyBytesRead == m_head.contentLength) {
        m_state = State_WaitingResponse;
        emit requestFinished();
    } else if (m_bodyBytesRead > m_head.contentLength) {
        sendErrorResponse(400, "Actual body size exceeded Content-Length");
    }
}

void HttpConnection::processChunkedBody()
{
    while (!m_bodyBuffer.isEmpty() && m_state == State_ReadingBody) {
        if (m_chunkState == Chunk_ReadSize) {
            int crlf = m_bodyBuffer.indexOf("\r\n");
            if (crlf == -1) {
                if (m_bodyBuffer.size() > 1024) {
                    sendErrorResponse(400, "Chunk size line too long");
                    return;
                }
                return; // Need more bytes
            }

            QByteArray sizeLine = m_bodyBuffer.left(crlf).trimmed();
            m_bodyBuffer.remove(0, crlf + 2);

            // Strip chunk extensions if present
            int semi = sizeLine.indexOf(';');
            if (semi != -1) {
                sizeLine = sizeLine.left(semi).trimmed();
            }

            bool ok = false;
            qint64 chunkSize = sizeLine.toLongLong(&ok, 16);
            if (!ok || chunkSize < 0) {
                sendErrorResponse(400, "Malformed chunk size");
                return;
            }

            if (chunkSize == 0) {
                m_chunkState = Chunk_ReadTrailers;
            } else {
                m_currentChunkSize = chunkSize;
                m_currentChunkRead = 0;
                m_chunkState = Chunk_ReadData;
            }
        } else if (m_chunkState == Chunk_ReadData) {
            qint64 remaining = m_currentChunkSize - m_currentChunkRead;
            int takeBytes = qMin(static_cast<qint64>(m_bodyBuffer.size()), remaining);

            if (takeBytes > 0) {
                QByteArray data = m_bodyBuffer.left(takeBytes);
                m_bodyBuffer.remove(0, takeBytes);
                m_currentChunkRead += data.size();
                m_bodyBytesRead += data.size();

                emit bodyChunk(data);
                if (m_state != State_ReadingBody) {
                    return;
                }
            }

            if (m_currentChunkRead == m_currentChunkSize) {
                m_chunkState = Chunk_ReadDataCrlf;
            }
        } else if (m_chunkState == Chunk_ReadDataCrlf) {
            if (m_bodyBuffer.size() < 2) {
                return; // Need more bytes
            }
            if (!m_bodyBuffer.startsWith("\r\n")) {
                sendErrorResponse(400, "Missing CRLF after chunk data");
                return;
            }
            m_bodyBuffer.remove(0, 2);
            m_chunkState = Chunk_ReadSize;
        } else if (m_chunkState == Chunk_ReadTrailers) {
            // Check for final empty line "\r\n" or trailers followed by "\r\n\r\n"
            if (m_bodyBuffer.startsWith("\r\n")) {
                m_bodyBuffer.remove(0, 2);
                m_state = State_WaitingResponse;
                emit requestFinished();
                return;
            }
            int trailerEnd = m_bodyBuffer.indexOf("\r\n\r\n");
            if (trailerEnd != -1) {
                m_bodyBuffer.remove(0, trailerEnd + 4);
                m_state = State_WaitingResponse;
                emit requestFinished();
                return;
            }
            if (m_bodyBuffer.size() > MAX_TRAILER_SIZE) {
                sendErrorResponse(400, "Chunk trailers exceeded maximum size");
                return;
            }
            return; // Wait for trailer completion
        }
    }
}

void HttpConnection::sendErrorResponse(int status, const QByteArray &message)
{
    respond(status, "text/plain", message);
}

QByteArray HttpConnection::statusReason(int status)
{
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 417: return "Expectation Failed";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}
