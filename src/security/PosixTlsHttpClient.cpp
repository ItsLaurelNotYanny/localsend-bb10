#include "security/PosixTlsHttpClient.h"

#include <QDebug>
#include <QStringList>

namespace localsend {

namespace {
class PosixTlsContextHolder : public QObject {
public:
    posix_tls_context_t *ctx;
    PosixTlsContextHolder(posix_tls_context_t *c, QObject *parent) : QObject(parent), ctx(c) {}
    virtual ~PosixTlsContextHolder() {
        if (ctx) {
            posix_tls_context_destroy(ctx);
            ctx = 0;
        }
    }
};
}

PosixTlsHttpClient::PosixTlsHttpClient(const LocalIdentity &identity, QObject *parent)
    : QObject(parent)
    , m_identity(identity)
    , m_ctx(0)
    , m_socket(0)
    , m_timer(new QTimer(this))
    , m_timeoutMs(0)
    , m_state(State_Idle)
    , m_port(0)
    , m_bodyStream(0)
    , m_bodySize(0)
    , m_bodyBytesWritten(0)
    , m_statusCode(0)
    , m_contentLength(-1)
    , m_headersParsed(false)
    , m_chunkedResponse(false)
    , m_chunkedComplete(false)
    , m_pumpingStream(false)
    , m_lastTlsError(POSIX_TLS_OK)
{
    m_timer->setSingleShot(true);
    connect(m_timer, SIGNAL(timeout()), this, SLOT(onTimeout()));
}

PosixTlsHttpClient::~PosixTlsHttpClient()
{
    cleanupSocket();
}

void PosixTlsHttpClient::cleanupSocket()
{
    if (m_timer) {
        m_timer->stop();
    }
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = 0;
    }
    if (m_ctx) {
        // If m_ctx was not yet assigned to an m_socket holder
        posix_tls_context_destroy(m_ctx);
        m_ctx = 0;
    }
}

void PosixTlsHttpClient::abort()
{
    if (m_state == State_Finished || m_state == State_Failed || m_state == State_Aborted) {
        return;
    }
    m_state = State_Aborted;
    cleanupSocket();
}

bool PosixTlsHttpClient::sendRequest(const QString &method,
                                    const QHostAddress &address,
                                    quint16 port,
                                    const QString &pathAndQuery,
                                    const QString &expectedPin,
                                    const QMap<QString, QString> &headers,
                                    const QByteArray &body,
                                    int timeoutMs)
{
    return sendStreamRequest(method, address, port, pathAndQuery, expectedPin, headers, 0, body.size(), timeoutMs)
        ? (m_requestBody = body, true)
        : false;
}

bool PosixTlsHttpClient::sendStreamRequest(const QString &method,
                                          const QHostAddress &address,
                                          quint16 port,
                                          const QString &pathAndQuery,
                                          const QString &expectedPin,
                                          const QMap<QString, QString> &headers,
                                          QIODevice *bodyStream,
                                          qint64 bodySize,
                                          int timeoutMs)
{
    if (m_state != State_Idle) {
        qWarning() << "[PosixTlsHttpClient] Client is not idle";
        return false;
    }

    if (address.isNull() || port == 0) {
        finishError(POSIX_TLS_ERR_INVALID_ARG, "Invalid target host or port");
        return false;
    }

    QString normalizedPin = LocalIdentity::normalizeFingerprint(expectedPin);
    if (normalizedPin.isEmpty() || normalizedPin.length() != 64) {
        finishError(POSIX_TLS_ERR_INVALID_PIN, "Invalid or missing peer certificate pin");
        return false;
    }

    m_method = method.toUpper();
    m_address = address;
    m_port = port;
    m_pathAndQuery = pathAndQuery.startsWith('/') ? pathAndQuery : ("/" + pathAndQuery);
    m_expectedPin = normalizedPin;
    m_requestHeaders = headers;
    m_bodyStream = bodyStream;
    m_bodySize = bodySize;
    m_bodyBytesWritten = 0;
    m_requestBody.clear();

    m_statusCode = 0;
    m_statusText.clear();
    m_responseHeaders.clear();
    m_readBuffer.clear();
    m_responseBody.clear();
    m_rawChunkBuffer.clear();
    m_contentLength = -1;
    m_headersParsed = false;
    m_chunkedResponse = false;
    m_chunkedComplete = false;
    m_pumpingStream = false;
    m_timeoutMs = timeoutMs;
    m_lastTlsError = POSIX_TLS_OK;
    m_errorString.clear();
    m_peerFingerprint.clear();

    m_ctx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    if (!m_ctx) {
        finishError(POSIX_TLS_ERR_OUT_OF_MEMORY, "posix_tls_context_create failed");
        return false;
    }

    if (m_identity.isValid()) {
        int credRc = posix_tls_context_set_credentials(
            m_ctx,
            m_identity.certPem().constData(), static_cast<size_t>(m_identity.certPem().size()),
            m_identity.keyPem().constData(), static_cast<size_t>(m_identity.keyPem().size()));
        if (credRc != POSIX_TLS_OK) {
            cleanupSocket();
            finishError(credRc, "Failed to configure local identity credentials");
            return false;
        }
    }

    int pinRc = posix_tls_context_set_expected_fingerprint(m_ctx, m_expectedPin.toLatin1().constData());
    if (pinRc != POSIX_TLS_OK) {
        cleanupSocket();
        finishError(pinRc, "Failed to configure expected certificate pin on context");
        return false;
    }

    m_socket = new PosixTlsSocket(m_ctx, this);
    new PosixTlsContextHolder(m_ctx, m_socket);
    m_ctx = 0; // Context ownership transferred to m_socket via PosixTlsContextHolder
    connect(m_socket, SIGNAL(encrypted()), this, SLOT(onSocketEncrypted()));
    connect(m_socket, SIGNAL(readyRead()), this, SLOT(onSocketReadyRead()));
    connect(m_socket, SIGNAL(bytesWritten(qint64)), this, SLOT(onSocketBytesWritten(qint64)));
    connect(m_socket, SIGNAL(disconnected()), this, SLOT(onSocketDisconnected()));
    connect(m_socket, SIGNAL(tlsError(int,QString)), this, SLOT(onSocketTlsError(int,QString)));

    m_state = State_Connecting;
    if (timeoutMs > 0) {
        m_timer->start(timeoutMs);
    }

    if (!m_socket->connectToHostEncrypted(m_address, m_port, QString(), timeoutMs)) {
        // connectToHostEncrypted already triggers tlsError or fails
        return false;
    }

    return true;
}

void PosixTlsHttpClient::onSocketEncrypted()
{
    if (m_state != State_Connecting) {
        return;
    }

    m_peerFingerprint = m_socket->peerFingerprint();
    m_state = State_Sending;

    // Build HTTP/1.1 request headers
    QByteArray headerBlock;
    headerBlock.append(m_method.toLatin1());
    headerBlock.append(' ');
    headerBlock.append(m_pathAndQuery.toLatin1());
    headerBlock.append(" HTTP/1.1\r\n");

    headerBlock.append("Host: ");
    headerBlock.append(m_address.toString().toLatin1());
    headerBlock.append(':');
    headerBlock.append(QByteArray::number(m_port));
    headerBlock.append("\r\n");

    headerBlock.append("User-Agent: localsend-bb10\r\n");
    headerBlock.append("Connection: close\r\n");

    qint64 totalBodyLen = m_bodyStream ? m_bodySize : m_requestBody.size();
    headerBlock.append("Content-Length: ");
    headerBlock.append(QByteArray::number(totalBodyLen));
    headerBlock.append("\r\n");

    for (QMap<QString, QString>::const_iterator it = m_requestHeaders.begin();
         it != m_requestHeaders.end(); ++it) {
        QString key = it.key().trimmed();
        if (key.compare("host", Qt::CaseInsensitive) == 0 ||
            key.compare("connection", Qt::CaseInsensitive) == 0 ||
            key.compare("content-length", Qt::CaseInsensitive) == 0) {
            continue;
        }
        headerBlock.append(key.toLatin1());
        headerBlock.append(": ");
        headerBlock.append(it.value().toLatin1());
        headerBlock.append("\r\n");
    }
    headerBlock.append("\r\n");

    if (m_bodyStream == 0) {
        // In-memory request
        headerBlock.append(m_requestBody);
        qint64 written = m_socket->write(headerBlock);
        if (written < 0) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "Failed to write HTTP request to socket");
            return;
        }
        if (m_state == State_Sending && m_socket) {
            m_socket->flush();
            m_state = State_AwaitingResponse;
        }
    } else {
        // Streaming request: send header first, then stream body
        qint64 written = m_socket->write(headerBlock);
        if (written < 0) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "Failed to write HTTP headers to socket");
            return;
        }
        if (m_state == State_Sending && m_socket) {
            pumpStream();
        }
    }
}

void PosixTlsHttpClient::pumpStream()
{
    if (m_pumpingStream || !m_bodyStream || m_state != State_Sending || !m_socket) {
        return;
    }

    m_pumpingStream = true;

    const qint64 CHUNK_SIZE = 65536; // 64 KiB bounded chunks
    while (m_state == State_Sending && m_socket && m_bodyBytesWritten < m_bodySize) {
        qint64 buffered = m_socket->bytesToWrite();
        if (buffered + CHUNK_SIZE > PosixTlsSocket::MAX_WRITE_BUFFER_CAP) {
            break; // Backpressure: wait for socket writes to drain before queuing more
        }

        qint64 toRead = qMin(CHUNK_SIZE, m_bodySize - m_bodyBytesWritten);
        QByteArray chunk = m_bodyStream->read(toRead);
        if (chunk.isEmpty()) {
            if (m_bodyBytesWritten < m_bodySize) {
                m_pumpingStream = false;
                finishError(POSIX_TLS_ERR_IO_FAILED, "Stream ended prematurely before expected body size was sent");
                return;
            }
            break;
        }

        qint64 written = m_socket->write(chunk);
        if (written < 0) {
            m_pumpingStream = false;
            finishError(POSIX_TLS_ERR_IO_FAILED, "Socket write failure during streaming upload");
            return;
        }

        m_bodyBytesWritten += chunk.size();
        emit uploadProgress(m_bodyBytesWritten, m_bodySize);

        // Upload progress slot may invoke abort() or finish
        if (m_state != State_Sending || !m_socket) {
            m_pumpingStream = false;
            return;
        }

        // Reset inactivity timer on progress
        if (m_timer && m_timer->isActive() && m_timeoutMs > 0) {
            m_timer->start(m_timeoutMs);
        }
    }

    if (m_state == State_Sending && m_socket && m_bodyBytesWritten >= m_bodySize) {
        m_socket->flush();
        m_state = State_AwaitingResponse;
        if (m_timer && m_timeoutMs > 0) {
            m_timer->start(m_timeoutMs);
        }
    }

    m_pumpingStream = false;
}

void PosixTlsHttpClient::onSocketBytesWritten(qint64 bytes)
{
    Q_UNUSED(bytes);
    if (m_state == State_Sending && m_bodyStream) {
        pumpStream();
    }
}

void PosixTlsHttpClient::onSocketReadyRead()
{
    if (!m_socket) return;
    QByteArray chunk = m_socket->readAll();
    if (!chunk.isEmpty()) {
        m_readBuffer.append(chunk);
        if (m_readBuffer.size() > MAX_RAW_BUFFER_CAP) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "HTTP response buffer exceeded allowable limit");
            return;
        }
        parseResponse();
    }
}

void PosixTlsHttpClient::parseResponse()
{
    if (!m_headersParsed) {
        int headerEnd = m_readBuffer.indexOf("\r\n\r\n");
        int delimiterLen = 4;
        if (headerEnd < 0) {
            headerEnd = m_readBuffer.indexOf("\n\n");
            delimiterLen = 2;
        }

        if (headerEnd < 0) {
            if (m_readBuffer.size() > MAX_HEADER_SIZE) {
                finishError(POSIX_TLS_ERR_IO_FAILED, "HTTP response headers exceed maximum allowable size (64 KiB)");
            }
            return; // Headers not complete yet
        }

        if (headerEnd > MAX_HEADER_SIZE) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "HTTP response headers exceed maximum allowable size (64 KiB)");
            return;
        }

        QByteArray headerBytes = m_readBuffer.left(headerEnd);
        m_responseBody = m_readBuffer.mid(headerEnd + delimiterLen);
        m_readBuffer.clear();

        QString headerText = QString::fromLatin1(headerBytes.constData(), headerBytes.size());
        QStringList lines = headerText.split(QRegExp("\r?\n"));
        if (lines.isEmpty()) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "Empty HTTP response status line");
            return;
        }

        // Parse status line: HTTP/1.1 <statusCode> <statusText>
        QString statusLine = lines.first().trimmed();
        QStringList parts = statusLine.split(' ', QString::SkipEmptyParts);
        if (parts.size() < 2) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "Malformed HTTP status line: " + statusLine);
            return;
        }

        const QString &version = parts.at(0);
        if (version != "HTTP/1.1" && version != "HTTP/1.0") {
            finishError(POSIX_TLS_ERR_IO_FAILED, "Unsupported or malformed HTTP version: " + version);
            return;
        }

        const QString &statusStr = parts.at(1);
        if (statusStr.length() != 3) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "Malformed HTTP status code width: " + statusStr);
            return;
        }

        bool statusOk = false;
        m_statusCode = statusStr.toInt(&statusOk);
        if (!statusOk || m_statusCode < 100 || m_statusCode > 599) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "Malformed HTTP status code: " + statusStr);
            return;
        }
        if (parts.size() >= 3) {
            QStringList rest(parts.mid(2));
            m_statusText = rest.join(" ");
        }

        // Handle 1xx informational responses (e.g. 100 Continue):
        // Discard 1xx interim status and re-parse remainder for the final response.
        if (m_statusCode >= 100 && m_statusCode < 200) {
            m_readBuffer = m_responseBody;
            m_responseBody.clear();
            m_rawChunkBuffer.clear();
            m_statusCode = 0;
            m_statusText.clear();
            m_responseHeaders.clear();
            m_headersParsed = false;
            m_contentLength = -1;
            m_chunkedResponse = false;
            m_chunkedComplete = false;
            if (!m_readBuffer.isEmpty()) {
                parseResponse();
            }
            return;
        }

        bool hasContentLength = false;
        qint64 firstContentLength = -1;

        // Parse headers
        for (int i = 1; i < lines.size(); ++i) {
            const QString &line = lines.at(i);
            int colonIdx = line.indexOf(':');
            if (colonIdx > 0) {
                QString name = line.left(colonIdx).trimmed().toLower();
                QString val = line.mid(colonIdx + 1).trimmed();
                m_responseHeaders.insert(name, val);

                if (name == "content-length") {
                    bool ok = false;
                    qint64 cl = val.toLongLong(&ok);
                    if (!ok || cl < 0) {
                        finishError(POSIX_TLS_ERR_IO_FAILED, "Invalid Content-Length header: " + val);
                        return;
                    }
                    if (hasContentLength && cl != firstContentLength) {
                        finishError(POSIX_TLS_ERR_IO_FAILED, "Conflicting Content-Length headers");
                        return;
                    }
                    hasContentLength = true;
                    firstContentLength = cl;
                    m_contentLength = cl;
                } else if (name == "transfer-encoding") {
                    if (val.toLower().contains("chunked")) {
                        m_chunkedResponse = true;
                    }
                }
            }
        }

        if (m_chunkedResponse && hasContentLength) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "Ambiguous framing: both Transfer-Encoding and Content-Length present");
            return;
        }

        m_headersParsed = true;

        if (m_contentLength > MAX_RESPONSE_BODY_SIZE) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "HTTP response Content-Length exceeds allowable limit (4 MiB)");
            return;
        }

        if (m_chunkedResponse) {
            m_rawChunkBuffer = m_responseBody;
            m_responseBody.clear();
        }
    } else {
        // Headers already parsed, m_readBuffer has extra body data
        if (!m_readBuffer.isEmpty()) {
            if (m_chunkedResponse) {
                m_rawChunkBuffer.append(m_readBuffer);
            } else {
                m_responseBody.append(m_readBuffer);
            }
            m_readBuffer.clear();
        }
    }

    if (m_chunkedResponse) {
        if (m_rawChunkBuffer.size() > MAX_RAW_BUFFER_CAP) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "Chunked response buffer exceeded allowable limit");
            return;
        }
        QByteArray decoded;
        bool complete = false;
        if (!decodeChunkedBody(m_rawChunkBuffer, &decoded, &complete)) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "Malformed chunked transfer encoding");
            return;
        }
        m_responseBody = decoded;
        m_chunkedComplete = complete;
        if (m_responseBody.size() > MAX_RESPONSE_BODY_SIZE) {
            finishError(POSIX_TLS_ERR_IO_FAILED, "HTTP response body exceeds allowable limit (4 MiB)");
            return;
        }
        if (complete) {
            bool uploadPending = false;
            if (m_bodyStream) {
                if (m_bodyBytesWritten < m_bodySize) {
                    uploadPending = true;
                } else if (m_socket && m_socket->bytesToWrite() > 0) {
                    uploadPending = true;
                }
            } else if (!m_requestBody.isEmpty()) {
                if (m_socket && m_socket->bytesToWrite() > 0) {
                    uploadPending = true;
                }
            }
            if (uploadPending) {
                finishError(POSIX_TLS_ERR_IO_FAILED, "Server responded before request body was fully sent");
                return;
            }
            finishSuccess();
            return;
        }
    }

    if (m_responseBody.size() > MAX_RESPONSE_BODY_SIZE) {
        finishError(POSIX_TLS_ERR_IO_FAILED, "HTTP response body exceeds allowable limit (4 MiB)");
        return;
    }

    // Check completion condition
    if (m_contentLength >= 0) {
        if (m_responseBody.size() >= m_contentLength) {
            if (m_responseBody.size() > m_contentLength) {
                m_responseBody.truncate(static_cast<int>(m_contentLength));
            }
            bool uploadPending = false;
            if (m_bodyStream) {
                if (m_bodyBytesWritten < m_bodySize) {
                    uploadPending = true;
                } else if (m_socket && m_socket->bytesToWrite() > 0) {
                    uploadPending = true;
                }
            } else if (!m_requestBody.isEmpty()) {
                if (m_socket && m_socket->bytesToWrite() > 0) {
                    uploadPending = true;
                }
            }
            if (uploadPending) {
                finishError(POSIX_TLS_ERR_IO_FAILED, "Server responded before request body was fully sent");
                return;
            }
            finishSuccess();
        }
    }
}

bool PosixTlsHttpClient::decodeChunkedBody(const QByteArray &input, QByteArray *output, bool *complete)
{
    if (complete) *complete = false;
    if (output) output->clear();
    qint64 pos = 0;
    const qint64 inputSize = input.size();

    while (pos < inputSize) {
        int lineEnd = input.indexOf("\r\n", static_cast<int>(pos));
        int headerLen = 2;
        if (lineEnd < 0) {
            lineEnd = input.indexOf("\n", static_cast<int>(pos));
            headerLen = 1;
            if (lineEnd < 0) {
                return true; // Incomplete chunk header, wait for more data
            }
        }

        QByteArray sizeHex = input.mid(static_cast<int>(pos), lineEnd - static_cast<int>(pos)).trimmed();
        int semi = sizeHex.indexOf(';');
        if (semi >= 0) {
            sizeHex = sizeHex.left(semi).trimmed();
        }

        bool ok = false;
        qint64 chunkSize = sizeHex.toLongLong(&ok, 16);
        if (!ok || chunkSize < 0 || chunkSize > MAX_RESPONSE_BODY_SIZE) {
            return false; // Malformed or excessive chunk size
        }

        pos = lineEnd + headerLen;

        if (chunkSize == 0) {
            // Final chunk: must be followed by CRLF or LF (empty line) or trailer fields ending with empty line
            if (pos >= inputSize) {
                return true; // Delimiter/trailer has not arrived yet, wait for more data
            }

            if (pos + 1 < inputSize && input.at(static_cast<int>(pos)) == '\r' && input.at(static_cast<int>(pos) + 1) == '\n') {
                if (complete) *complete = true;
                return true;
            } else if (input.at(static_cast<int>(pos)) == '\n') {
                if (complete) *complete = true;
                return true;
            } else if (pos + 1 == inputSize && input.at(static_cast<int>(pos)) == '\r') {
                return true; // Partial CRLF terminator: wait for \n
            } else {
                // Trailer fields ending with \r\n\r\n or \n\n
                int trailerEnd = input.indexOf("\r\n\r\n", static_cast<int>(pos));
                if (trailerEnd >= 0) {
                    if (complete) *complete = true;
                    return true;
                }
                trailerEnd = input.indexOf("\n\n", static_cast<int>(pos));
                if (trailerEnd >= 0) {
                    if (complete) *complete = true;
                    return true;
                }
                return true; // Incomplete trailer fields, wait for more data
            }
        }

        // Check if full chunk data is available without integer overflow
        if (inputSize - pos < chunkSize) {
            return true; // Incomplete chunk data, wait for more
        }

        if (output) {
            if (output->size() + chunkSize > MAX_RESPONSE_BODY_SIZE) {
                return false; // Decoded body exceeds limit
            }
            output->append(input.mid(static_cast<int>(pos), static_cast<int>(chunkSize)));
        }

        pos += chunkSize;

        // Chunk data MUST be followed by CRLF or LF
        if (pos >= inputSize) {
            return true; // Wait for separator
        }
        if (pos + 1 < inputSize && input.at(static_cast<int>(pos)) == '\r' && input.at(static_cast<int>(pos) + 1) == '\n') {
            pos += 2;
        } else if (input.at(static_cast<int>(pos)) == '\n') {
            pos += 1;
        } else if (pos + 1 == inputSize && input.at(static_cast<int>(pos)) == '\r') {
            return true; // Partial CRLF separator: wait for \n
        } else {
            return false; // Invalid chunk data separator
        }
    }

    return true;
}

void PosixTlsHttpClient::onSocketDisconnected()
{
    bool uploadPending = false;
    if (m_bodyStream) {
        if (m_bodyBytesWritten < m_bodySize) {
            uploadPending = true;
        } else if (m_socket && m_socket->bytesToWrite() > 0) {
            uploadPending = true;
        }
    } else if (!m_requestBody.isEmpty()) {
        if (m_socket && m_socket->bytesToWrite() > 0) {
            uploadPending = true;
        }
    }

    if (m_state == State_AwaitingResponse) {
        if (!m_headersParsed) {
            finishError(POSIX_TLS_ERR_CLOSED, "Connection closed before HTTP response received");
        } else if (m_contentLength >= 0 && m_responseBody.size() < m_contentLength) {
            finishError(POSIX_TLS_ERR_CLOSED, "Connection closed before complete HTTP response body received");
        } else if (m_chunkedResponse && !m_chunkedComplete) {
            finishError(POSIX_TLS_ERR_CLOSED, "Connection closed before chunked HTTP response completed");
        } else if (uploadPending) {
            finishError(POSIX_TLS_ERR_CLOSED, "Connection closed before request body upload completed");
        } else {
            finishSuccess();
        }
    } else if (m_state == State_Connecting || m_state == State_Sending) {
        finishError(POSIX_TLS_ERR_CLOSED, "Connection prematurely closed by peer");
    }
}

void PosixTlsHttpClient::onSocketTlsError(int code, const QString &msg)
{
    qWarning() << "[PosixTlsHttpClient] TLS error code:" << code << "msg:" << msg;
    finishError(code, msg);
}

void PosixTlsHttpClient::onTimeout()
{
    qWarning() << "[PosixTlsHttpClient] Operation timed out";
    finishError(POSIX_TLS_ERR_IO_FAILED, "HTTP client operation timed out");
}

void PosixTlsHttpClient::finishSuccess()
{
    if (m_state == State_Finished || m_state == State_Failed || m_state == State_Aborted) {
        return;
    }
    m_state = State_Finished;
    if (m_timer) m_timer->stop();
    int code = m_statusCode;
    QByteArray body = m_responseBody;
    cleanupSocket();
    emit finished(code, body);
}

void PosixTlsHttpClient::finishError(int code, const QString &msg)
{
    if (m_state == State_Finished || m_state == State_Failed || m_state == State_Aborted) {
        return;
    }
    m_state = State_Failed;
    if (m_timer) m_timer->stop();
    m_lastTlsError = code;
    m_errorString = msg;
    cleanupSocket();
    emit failed(code, msg);
}

} // namespace localsend
