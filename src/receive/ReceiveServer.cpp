#include "receive/ReceiveServer.h"

#include <QUrl>
#include <QDir>
#include <QFileInfo>

#include "app/Application.h"
#include "discovery/DiscoveryManager.h"
#include "receive/HttpServer.h"
#include "receive/HttpConnection.h"
#include "protocol/ProtocolJson.h"

ReceiveServer::ReceiveServer(localsend::Application *app,
                             DiscoveryManager *discovery,
                             QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_discovery(discovery)
    , m_server(0)
    , m_consentTimer(new QTimer(this))
    , m_idleUploadTimer(new QTimer(this))
    , m_inactivityTimer(new QTimer(this))
{
    m_pendingOffer.connection = 0;

    m_activeSession.partFile = 0;
    m_activeSession.bytesReceived = 0;
    m_activeSession.isUploading = false;
    m_activeSession.connection = 0;
    m_activeSession.peerPort = 0;

    m_consentTimer->setSingleShot(true);
    connect(m_consentTimer, SIGNAL(timeout()), this, SLOT(onConsentTimeout()));

    m_idleUploadTimer->setSingleShot(true);
    connect(m_idleUploadTimer, SIGNAL(timeout()), this, SLOT(onIdleUploadTimeout()));

    m_inactivityTimer->setSingleShot(true);
    connect(m_inactivityTimer, SIGNAL(timeout()), this, SLOT(onInactivityTimeout()));
}

ReceiveServer::~ReceiveServer()
{
    clearPendingOffer();
    cleanupActiveSession(false);
}

void ReceiveServer::attach(HttpServer *server)
{
    if (m_server == server) {
        return;
    }
    m_server = server;
    if (m_server) {
        connect(m_server, SIGNAL(connectionOpened(HttpConnection*)),
                this, SLOT(onConnectionOpened(HttpConnection*)));
    }
}

void ReceiveServer::onConnectionOpened(HttpConnection *connection)
{
    if (!connection) {
        return;
    }

    ConnectionState state;
    state.isUpload = false;
    m_connStates.insert(connection, state);

    connect(connection, SIGNAL(headersReady(HttpRequestHead)),
            this, SLOT(onHeadersReady(HttpRequestHead)));
    connect(connection, SIGNAL(bodyChunk(QByteArray)),
            this, SLOT(onBodyChunk(QByteArray)));
    connect(connection, SIGNAL(requestFinished()),
            this, SLOT(onRequestFinished()));
    connect(connection, SIGNAL(disconnected()),
            this, SLOT(onConnectionDisconnected()));
}

void ReceiveServer::onHeadersReady(const HttpRequestHead &head)
{
    HttpConnection *conn = qobject_cast<HttpConnection*>(sender());
    if (!conn || !m_connStates.contains(conn)) {
        return;
    }

    m_connStates[conn].head = head;
    QString path = head.target.path();

    if (head.method == "GET" && (path == "/api/localsend/v2/info" || path == "/api/localsend/v1/info")) {
        handleInfoRoute(conn);
    } else if (head.method == "POST" && path == "/api/localsend/v2/register") {
        if (head.contentLength > ProtocolJson::MAX_JSON_SIZE) {
            conn->respond(413, "text/plain", "JSON payload too large");
            return;
        }
        conn->allowBody();
    } else if (head.method == "POST" && path == "/api/localsend/v2/prepare-upload") {
        if (head.contentLength > ProtocolJson::MAX_JSON_SIZE) {
            conn->respond(413, "text/plain", "JSON payload too large");
            return;
        }
        conn->allowBody();
    } else if (head.method == "POST" && path == "/api/localsend/v2/cancel") {
        handleCancelRoute(conn, head);
    } else if (head.method == "POST" && path == "/api/localsend/v2/upload") {
        int httpStatus = 200;
        QString error;
        m_connStates[conn].isUpload = true;
        conn->setUploadStream(true);
        conn->stopRequestTimeout();

        if (!beginUpload(head, &httpStatus, &error)) {
            conn->respond(httpStatus, "text/plain", error.toUtf8());
            return;
        }

        m_activeSession.connection = conn;
        conn->allowBody(); // Sends 100 Continue if requested, and enables streaming
    } else {
        conn->respond(404, "text/plain", "Endpoint not found");
    }
}

void ReceiveServer::onBodyChunk(const QByteArray &chunk)
{
    HttpConnection *conn = qobject_cast<HttpConnection*>(sender());
    if (!conn || !m_connStates.contains(conn)) {
        return;
    }

    if (m_connStates[conn].isUpload) {
        int httpStatus = 200;
        QString error;
        if (!appendUpload(chunk, &httpStatus, &error)) {
            conn->respond(httpStatus, "text/plain", error.toUtf8());
            conn->abort();
        }
    } else {
        m_connStates[conn].bodyBuffer.append(chunk);
        if (m_connStates[conn].bodyBuffer.size() > ProtocolJson::MAX_JSON_SIZE) {
            conn->respond(413, "text/plain", "JSON payload too large");
            conn->abort();
        }
    }
}

void ReceiveServer::onRequestFinished()
{
    HttpConnection *conn = qobject_cast<HttpConnection*>(sender());
    if (!conn || !m_connStates.contains(conn)) {
        return;
    }

    if (m_connStates[conn].isUpload) {
        QString savedPath;
        int httpStatus = 200;
        QString error;
        if (finishUpload(&savedPath, &httpStatus, &error)) {
            conn->respond(200, "text/plain", "");
        } else {
            conn->respond(httpStatus, "text/plain", error.toUtf8());
        }
    } else {
        QString path = m_connStates[conn].head.target.path();
        if (path == "/api/localsend/v2/register") {
            handleRegisterRoute(conn, m_connStates[conn].bodyBuffer);
        } else if (path == "/api/localsend/v2/prepare-upload") {
            handlePrepareUploadRoute(conn, m_connStates[conn].bodyBuffer);
        }
    }
}

void ReceiveServer::onConnectionDisconnected()
{
    HttpConnection *conn = qobject_cast<HttpConnection*>(sender());
    if (!conn) {
        return;
    }

    if (m_pendingOffer.connection == conn) {
        QString reqId = m_pendingOffer.requestId;
        if (m_app) {
            m_app->endTransfer(m_pendingOffer.requestId);
        }
        clearPendingOffer();
        emit offerCancelled(reqId, "Remote peer disconnected before consent");
    }

    if (m_activeSession.connection == conn && m_activeSession.isUploading) {
        // Disconnected mid-upload
        QString sessId = m_activeSession.sessionId;
        cleanupActiveSession(false);
        emit receiveFailed(sessId, "Remote peer disconnected during upload");
    }

    m_connStates.remove(conn);
}

void ReceiveServer::handleInfoRoute(HttpConnection *conn)
{
    DeviceInfo info = m_app ? m_app->localInfo() : DeviceInfo();
    QString err;
    QByteArray respJson = ProtocolJson::encodeDeviceResponse(info, &err);
    conn->respond(200, "application/json", respJson);
}

void ReceiveServer::handleRegisterRoute(HttpConnection *conn, const QByteArray &body)
{
    DeviceInfo peerInfo;
    QString err;
    if (!ProtocolJson::decodeRegisterRequest(body, &peerInfo, &err)) {
        conn->respond(400, "text/plain", "Malformed register JSON");
        return;
    }

    if (m_discovery) {
        m_discovery->handleIncomingRegister(peerInfo, conn->peerAddress());
    }

    DeviceInfo info = m_app ? m_app->localInfo() : DeviceInfo();
    QByteArray respJson = ProtocolJson::encodeDeviceResponse(info, &err);
    conn->respond(200, "application/json", respJson);
}

void ReceiveServer::handlePrepareUploadRoute(HttpConnection *conn, const QByteArray &body)
{
    if (conn->peerAddress().isNull()) {
        conn->respond(403, "text/plain", "Unknown client source address");
        return;
    }

    PrepareUploadRequest req;
    QString err;
    if (!ProtocolJson::decodePrepareRequest(body, &req, &err)) {
        conn->respond(400, "text/plain", "Malformed prepare-upload JSON");
        return;
    }

    if (req.files.isEmpty()) {
        conn->respond(400, "text/plain", "Empty files map");
        return;
    }

    // Validate files: size, filename traversal, limits
    QMapIterator<QString, FileMeta> it(req.files);
    while (it.hasNext()) {
        it.next();
        const FileMeta &meta = it.value();
        if (meta.fileName.trimmed().isEmpty() ||
            meta.fileName.contains('/') ||
            meta.fileName.contains('\\') ||
            meta.fileName.contains("..")) {
            conn->respond(400, "text/plain", "Invalid file name");
            return;
        }
        if (meta.size < 0) {
            conn->respond(400, "text/plain", "Negative file size");
            return;
        }
        if (meta.size > 100 * 1024 * 1024) { // Initial product limit: 100 MiB
            conn->respond(403, "text/plain", "File size exceeds 100 MiB product limit");
            return;
        }
    }

    // Check slot availability
    if (m_app && m_app->isTransferActive()) {
        conn->respond(409, "text/plain", "Transfer slot is occupied");
        return;
    }
    if (hasPendingOffer() || hasActiveSession()) {
        conn->respond(409, "text/plain", "Transfer slot is occupied");
        return;
    }

    QString requestId = CryptoUtil::randomHex(16);
    if (m_app && !m_app->beginTransfer(requestId)) {
        conn->respond(409, "text/plain", "Transfer slot is occupied");
        return;
    }

    m_pendingOffer.requestId = requestId;
    m_pendingOffer.peer.info = req.info;
    m_pendingOffer.peer.address = conn->peerAddress();
    m_pendingOffer.files = req.files;
    m_pendingOffer.connection = conn;

    m_consentTimer->start(120000); // 120s consent timeout
    emit incomingOffer(requestId, m_pendingOffer.peer, req.files.values());
    // The connection is NOT responded to yet; kept open in State_WaitingResponse
}

void ReceiveServer::handleCancelRoute(HttpConnection *conn, const HttpRequestHead &head)
{
    QString sessionId = getUrlQueryItem(head.target, "sessionId");
    QHostAddress peerAddr = conn ? conn->peerAddress() : QHostAddress();

    if (!sessionId.isEmpty() && sessionId == m_activeSession.sessionId) {
        handleCancel(sessionId, peerAddr);
    } else {
        emit remoteCancelRequested(sessionId, peerAddr);
    }

    // Cancel endpoint responds with idempotent 200
    conn->respond(200, "text/plain", "");
}

bool ReceiveServer::accept(const QString &requestId, const QString &fileId)
{
    if (m_pendingOffer.requestId != requestId || !m_pendingOffer.files.contains(fileId)) {
        return false;
    }

    m_consentTimer->stop();
    FileMeta selectedMeta = m_pendingOffer.files.value(fileId);

    QString sessionId = CryptoUtil::randomHex(16);
    QString token = CryptoUtil::randomHex(16);

    QString dirPath = m_app ? m_app->receiveDirectory() : "data/received";
    QDir dir(dirPath);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            if (m_pendingOffer.connection) {
                m_pendingOffer.connection->respond(500, "text/plain", "Failed to create receive directory");
            }
            if (m_app) {
                m_app->endTransfer(requestId);
            }
            clearPendingOffer();
            emit receiveFailed(requestId, "Failed to create receive directory");
            return false;
        }
    }

    QString partPath = dir.filePath(QString(".ls_%1.part").arg(sessionId));
    QFile *partFile = new QFile(partPath, this);
    if (!partFile->open(QIODevice::WriteOnly)) {
        delete partFile;
        if (m_pendingOffer.connection) {
            m_pendingOffer.connection->respond(500, "text/plain", "Failed to create destination temporary file");
        }
        if (m_app) {
            m_app->endTransfer(requestId);
        }
        clearPendingOffer();
        emit receiveFailed(requestId, "Failed to create destination temporary file");
        return false;
    }

    m_activeSession.requestId = requestId;
    m_activeSession.sessionId = sessionId;
    m_activeSession.fileId = fileId;
    m_activeSession.token = token;
    m_activeSession.meta = selectedMeta;
    m_activeSession.peerAddress = m_pendingOffer.peer.address;
    m_activeSession.peerPort = m_pendingOffer.peer.info.port;
    m_activeSession.partFilePath = partPath;
    m_activeSession.partFile = partFile;
    m_activeSession.bytesReceived = 0;
    m_activeSession.isUploading = false;
    m_activeSession.shaCtx.reset();
    m_activeSession.connection = 0;

    PrepareUploadResponse resp;
    resp.sessionId = sessionId;
    resp.fileTokens.insert(fileId, token);

    QString err;
    QByteArray respJson = ProtocolJson::encodePrepareResponse(resp, &err);

    HttpConnection *conn = m_pendingOffer.connection;
    clearPendingOffer();

    if (conn) {
        conn->respond(200, "application/json", respJson);
    }

    m_idleUploadTimer->start(60000); // 60s idle upload start timeout
    return true;
}

void ReceiveServer::reject(const QString &requestId)
{
    if (m_pendingOffer.requestId.isEmpty() || m_pendingOffer.requestId != requestId) {
        return;
    }

    QString idToRelease = requestId;
    m_consentTimer->stop();
    HttpConnection *conn = m_pendingOffer.connection;
    clearPendingOffer();

    if (conn) {
        conn->respond(403, "application/json", "{\"message\":\"Rejected by user\"}");
    }

    if (m_app) {
        m_app->endTransfer(idToRelease);
    }
}

bool ReceiveServer::beginUpload(const HttpRequestHead &head, int *httpStatus, QString *error)
{
    QString sessionId = getUrlQueryItem(head.target, "sessionId");
    QString fileId = getUrlQueryItem(head.target, "fileId");
    QString token = getUrlQueryItem(head.target, "token");

    if (sessionId.isEmpty() || fileId.isEmpty() || token.isEmpty()) {
        if (httpStatus) *httpStatus = 400;
        if (error) *error = "Missing query parameters";
        return false;
    }

    if (m_activeSession.sessionId.isEmpty() || m_activeSession.sessionId != sessionId) {
        if (httpStatus) *httpStatus = 403;
        if (error) *error = "Invalid or expired session";
        return false;
    }

    if (m_activeSession.fileId != fileId || m_activeSession.token != token) {
        if (httpStatus) *httpStatus = 403;
        if (error) *error = "Unauthorized fileId or token";
        return false;
    }

    if (head.peerAddress.isNull() || m_activeSession.peerAddress.isNull() || head.peerAddress != m_activeSession.peerAddress) {
        // Address mismatch
        if (httpStatus) *httpStatus = 403;
        if (error) *error = "Source IP mismatch";
        return false;
    }

    if (m_activeSession.isUploading) {
        if (httpStatus) *httpStatus = 409;
        if (error) *error = "Upload already in progress";
        return false;
    }

    if (head.contentLength >= 0 && head.contentLength != m_activeSession.meta.size) {
        if (httpStatus) *httpStatus = 400;
        if (error) *error = "Content-Length does not match declared metadata size";
        return false;
    }

    m_idleUploadTimer->stop();
    m_activeSession.isUploading = true;
    m_inactivityTimer->start(30000); // 30s inactivity timeout

    if (httpStatus) *httpStatus = 200;
    return true;
}

bool ReceiveServer::appendUpload(const QByteArray &chunk, int *httpStatus, QString *error)
{
    if (!m_activeSession.isUploading || !m_activeSession.partFile) {
        if (httpStatus) *httpStatus = 400;
        if (error) *error = "No active upload session";
        return false;
    }

    m_inactivityTimer->start(30000); // Refresh inactivity timer on progress

    if (m_activeSession.bytesReceived + chunk.size() > m_activeSession.meta.size) {
        if (httpStatus) *httpStatus = 400;
        if (error) *error = "Received bytes exceed declared file size";
        cleanupActiveSession(false);
        return false;
    }

    if (!chunk.isEmpty()) {
        qint64 written = m_activeSession.partFile->write(chunk);
        if (written != chunk.size()) {
            if (httpStatus) *httpStatus = 500;
            if (error) *error = "Disk write failure";
            cleanupActiveSession(false);
            return false;
        }
        m_activeSession.shaCtx.update(chunk);
        m_activeSession.bytesReceived += chunk.size();
    }

    emit receiveProgress(m_activeSession.sessionId,
                        m_activeSession.bytesReceived,
                        m_activeSession.meta.size);

    if (httpStatus) *httpStatus = 200;
    return true;
}

bool ReceiveServer::finishUpload(QString *savedPath, int *httpStatus, QString *error)
{
    if (!m_activeSession.isUploading || !m_activeSession.partFile) {
        if (httpStatus) *httpStatus = 400;
        if (error) *error = "No active upload session";
        return false;
    }

    m_inactivityTimer->stop();

    if (m_activeSession.bytesReceived != m_activeSession.meta.size) {
        if (httpStatus) *httpStatus = 400;
        if (error) *error = QString("Incomplete upload: expected %1 bytes, got %2")
                            .arg(m_activeSession.meta.size).arg(m_activeSession.bytesReceived);
        QString failedSessionId = m_activeSession.sessionId;
        cleanupActiveSession(false);
        emit receiveFailed(failedSessionId, *error);
        return false;
    }

    // Verify SHA-256 if supplied
    if (!m_activeSession.meta.sha256.trimmed().isEmpty()) {
        QString computed = m_activeSession.shaCtx.finalHex();
        if (computed.compare(m_activeSession.meta.sha256.trimmed(), Qt::CaseInsensitive) != 0) {
            if (httpStatus) *httpStatus = 422;
            if (error) *error = QString("SHA-256 mismatch: declared %1, computed %2")
                                .arg(m_activeSession.meta.sha256).arg(computed);
            QString failedSessionId = m_activeSession.sessionId;
            cleanupActiveSession(false);
            emit receiveFailed(failedSessionId, *error);
            return false;
        }
    }

    bool flushOk = m_activeSession.partFile->flush();
    if (!flushOk || m_activeSession.partFile->error() != QFile::NoError) {
        if (httpStatus) *httpStatus = 500;
        if (error) *error = "Failed to flush destination file (I/O error or disk full)";
        QString failedSessionId = m_activeSession.sessionId;
        cleanupActiveSession(false);
        emit receiveFailed(failedSessionId, *error);
        return false;
    }

    m_activeSession.partFile->close();
    if (m_activeSession.partFile->error() != QFile::NoError) {
        if (httpStatus) *httpStatus = 500;
        if (error) *error = "Failed to close destination file (I/O error)";
        QString failedSessionId = m_activeSession.sessionId;
        cleanupActiveSession(false);
        emit receiveFailed(failedSessionId, *error);
        return false;
    }

    QString dirPath = m_app ? m_app->receiveDirectory() : "data/received";
    QString finalDest = resolveUniqueDestination(dirPath, m_activeSession.meta.fileName);

    if (!QFile::rename(m_activeSession.partFilePath, finalDest)) {
        if (httpStatus) *httpStatus = 500;
        if (error) *error = "Failed to commit destination file";
        QString failedSessionId = m_activeSession.sessionId;
        cleanupActiveSession(false);
        emit receiveFailed(failedSessionId, *error);
        return false;
    }

    if (savedPath) {
        *savedPath = finalDest;
    }
    if (httpStatus) {
        *httpStatus = 200;
    }

    QString doneSessionId = m_activeSession.sessionId;
    QString reqId = m_activeSession.requestId;

    cleanupActiveSession(true); // Don't delete committed destination file!
    emit receiveFinished(doneSessionId, finalDest);

    if (m_app) {
        m_app->endTransfer(reqId);
    }

    return true;
}

void ReceiveServer::cancelLocal(const QString &requestId)
{
    if (!m_pendingOffer.requestId.isEmpty() && (requestId.isEmpty() || m_pendingOffer.requestId == requestId)) {
        QString targetId = m_pendingOffer.requestId;
        reject(targetId);
        return;
    }

    if (!m_activeSession.requestId.isEmpty() && (requestId.isEmpty() || m_activeSession.requestId == requestId || m_activeSession.sessionId == requestId)) {
        QString sessId = m_activeSession.sessionId;
        HttpConnection *conn = m_activeSession.connection;
        cleanupActiveSession(false);
        emit receiveCancelled(sessId, "Cancelled locally");
        if (conn) {
            conn->abort();
        }
    }
}

void ReceiveServer::handleCancel(const QString &sessionId, const QHostAddress &peer)
{
    if (!peer.isNull() && !m_activeSession.peerAddress.isNull() &&
        m_activeSession.sessionId == sessionId && m_activeSession.peerAddress == peer) {
        QString sessId = m_activeSession.sessionId;
        HttpConnection *conn = m_activeSession.connection;
        cleanupActiveSession(false);
        emit receiveCancelled(sessId, "Cancelled by remote peer");
        if (conn) {
            conn->abort();
        }
    }
}

void ReceiveServer::onNetworkLost()
{
    if (hasPendingOffer()) {
        QString targetId = m_pendingOffer.requestId;
        reject(targetId);
    }
    if (hasActiveSession()) {
        cancelLocal(m_activeSession.requestId);
    }
}

void ReceiveServer::onConsentTimeout()
{
    if (hasPendingOffer()) {
        QString reqId = m_pendingOffer.requestId;
        reject(reqId);
        emit offerCancelled(reqId, "Consent timeout");
    }
}

void ReceiveServer::onIdleUploadTimeout()
{
    if (hasActiveSession() && !m_activeSession.isUploading) {
        QString sessId = m_activeSession.sessionId;
        cleanupActiveSession(false);
        emit receiveFailed(sessId, "Upload initiation timeout");
    }
}

void ReceiveServer::onInactivityTimeout()
{
    if (hasActiveSession() && m_activeSession.isUploading) {
        QString sessId = m_activeSession.sessionId;
        HttpConnection *conn = m_activeSession.connection;
        cleanupActiveSession(false);
        emit receiveFailed(sessId, "Upload stalled inactivity timeout");
        if (conn) {
            conn->abort();
        }
    }
}

void ReceiveServer::clearPendingOffer()
{
    m_consentTimer->stop();
    m_pendingOffer.requestId.clear();
    m_pendingOffer.files.clear();
    m_pendingOffer.connection = 0;
}

void ReceiveServer::cleanupActiveSession(bool committed)
{
    m_idleUploadTimer->stop();
    m_inactivityTimer->stop();

    if (m_activeSession.partFile) {
        m_activeSession.partFile->close();
        if (!committed) {
            m_activeSession.partFile->remove();
        }
        delete m_activeSession.partFile;
        m_activeSession.partFile = 0;
    }

    QString reqId = m_activeSession.requestId;
    m_activeSession.requestId.clear();
    m_activeSession.sessionId.clear();
    m_activeSession.fileId.clear();
    m_activeSession.token.clear();
    m_activeSession.partFilePath.clear();
    m_activeSession.isUploading = false;
    m_activeSession.bytesReceived = 0;
    m_activeSession.connection = 0;

    if (!committed && m_app && !reqId.isEmpty()) {
        m_app->endTransfer(reqId);
    }
}

QString ReceiveServer::resolveUniqueDestination(const QString &dirPath, const QString &fileName) const
{
    QDir dir(dirPath);
    QString cleanName = QFileInfo(fileName).fileName();
    if (cleanName.isEmpty()) {
        cleanName = "received_file";
    }

    QFileInfo fi(cleanName);
    QString baseName = fi.completeBaseName();
    QString ext = fi.suffix();
    if (!ext.isEmpty()) {
        ext = "." + ext;
    }

    QString candidate = cleanName;
    int counter = 1;
    while (dir.exists(candidate) || QFile::exists(dir.filePath(candidate))) {
        candidate = QString("%1 (%2)%3").arg(baseName).arg(counter).arg(ext);
        counter++;
    }

    return dir.filePath(candidate);
}
