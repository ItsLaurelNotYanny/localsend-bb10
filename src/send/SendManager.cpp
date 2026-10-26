#include "send/SendManager.h"

#include <QFileInfo>
#include <QUrl>

#include "app/Application.h"
#include "protocol/ProtocolJson.h"
#include "protocol/CryptoUtil.h"
#include "security/LocalIdentity.h"
#include "security/PosixTlsHttpClient.h"

SendManager::SendManager(localsend::Application *app,
                         QNetworkAccessManager *nam,
                         QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_nam(nam)
    , m_state(State_Idle)
    , m_failureReason(Failure_None)
    , m_fileSize(0)
    , m_isHttps(false)
    , m_generation(0)
    , m_httpsPrepareClient(0)
    , m_httpsUploadClient(0)
    , m_httpsCancelClient(0)
    , m_file(0)
    , m_prepareReply(0)
    , m_uploadReply(0)
    , m_consentTimer(new QTimer(this))
    , m_inactivityTimer(new QTimer(this))
{
    m_consentTimer->setSingleShot(true);
    connect(m_consentTimer, SIGNAL(timeout()), this, SLOT(onConsentTimeout()));

    m_inactivityTimer->setSingleShot(true);
    connect(m_inactivityTimer, SIGNAL(timeout()), this, SLOT(onInactivityTimeout()));
}

SendManager::~SendManager()
{
    cleanup(true);
}

QString SendManager::stateString() const
{
    switch (m_state) {
        case State_Idle: return "Idle";
        case State_Preparing: return "Preparing";
        case State_Uploading: return "Uploading";
        case State_Completed: return "Completed";
        case State_Failed: return "Failed";
        case State_Cancelled: return "Cancelled";
        default: return "Unknown";
    }
}

QString SendManager::failureReasonString() const
{
    switch (m_failureReason) {
        case Failure_None: return "None";
        case Failure_LocalFileNotFound: return "LocalFileNotFound";
        case Failure_PeerRejected: return "PeerRejected";
        case Failure_PeerBusy: return "PeerBusy";
        case Failure_PeerPinRequired: return "PeerPinRequired";
        case Failure_NetworkError: return "NetworkError";
        case Failure_Timeout: return "Timeout";
        case Failure_ChecksumMismatch: return "ChecksumMismatch";
        case Failure_ProtocolError: return "ProtocolError";
        default: return "Unknown";
    }
}

void SendManager::transitionState(SendState newState)
{
    m_state = newState;
    qDebug() << "[SendManager] State transition ->" << stateString() << "requestId:" << m_requestId;
    emit sendStateChanged(m_requestId, stateString());
}

void SendManager::fail(const QString &reason, FailureReason failureCode)
{
    if (m_state == State_Failed || m_state == State_Cancelled || m_state == State_Completed) {
        return;
    }
    quint64 curGen = ++m_generation;
    Q_UNUSED(curGen);
    QString reqId = m_requestId;
    if (reqId.isEmpty()) {
        return;
    }

    m_failureReason = failureCode;
    qWarning() << "[SendManager] Transfer failure:" << reason
               << "code:" << failureReasonString() << "requestId:" << reqId;

    cleanup(true);
    m_state = State_Failed;
    emit sendStateChanged(reqId, "Failed");
    emit sendFailed(reqId, reason);
    emit sendFailedWithReason(reqId, reason, static_cast<int>(failureCode));
}

bool SendManager::sendFile(const PeerDevice &peer, const QString &filePath, QString *error)
{
    if (m_state == State_Preparing || m_state == State_Uploading) {
        if (error) {
            *error = "SendManager is already busy with an active transfer";
        }
        return false;
    }

    // Check protocol: http or https
    QString proto = peer.info.protocol.toLower();
    if (proto != "http" && proto != "https") {
        m_failureReason = Failure_ProtocolError;
        if (error) {
            *error = "Unsupported transfer protocol: " + peer.info.protocol;
        }
        return false;
    }

    if (proto == "https") {
        QString normPin = localsend::LocalIdentity::normalizeFingerprint(peer.info.fingerprint);
        if (normPin.isEmpty() || normPin.length() != 64) {
            m_failureReason = Failure_PeerPinRequired;
            if (error) {
                *error = "Valid peer certificate fingerprint is required for HTTPS transfer";
            }
            return false;
        }
        m_activePin = normPin;
        m_isHttps = true;
    } else {
        m_activePin.clear();
        m_isHttps = false;
    }

    QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile() || !fi.isReadable()) {
        m_failureReason = Failure_LocalFileNotFound;
        if (error) {
            *error = "Local file does not exist or is not readable: " + filePath;
        }
        qWarning() << "[SendManager] File not found or unreadable:" << filePath;
        return false;
    }

    if (fi.size() > 100 * 1024 * 1024) { // 100 MiB product limit
        m_failureReason = Failure_ProtocolError;
        if (error) {
            *error = "File size exceeds 100 MiB product limit";
        }
        return false;
    }

    if (fi.size() < 0) {
        m_failureReason = Failure_ProtocolError;
        if (error) {
            *error = "Negative file size";
        }
        return false;
    }

    quint64 thisGen = ++m_generation;
    QString thisRequestId = CryptoUtil::randomHex(16);
    if (m_app && !m_app->beginTransfer(thisRequestId)) {
        m_failureReason = Failure_PeerBusy;
        if (error) {
            *error = "Transfer slot occupied";
        }
        return false;
    }

    m_failureReason = Failure_None;
    m_requestId = thisRequestId;
    m_peer = peer;
    m_filePath = filePath;
    m_fileSize = fi.size();
    m_fileId = CryptoUtil::randomHex(8);

    transitionState(State_Preparing);
    if (m_state != State_Preparing || m_generation != thisGen || m_requestId != thisRequestId) {
        return false;
    }

    PrepareUploadRequest req;
    req.info = m_app ? m_app->localInfo() : DeviceInfo();

    FileMeta meta;
    meta.id = m_fileId;
    meta.fileName = fi.fileName();
    meta.size = fi.size();
    meta.fileType = "application/octet-stream";
    meta.sha256 = QString(); // null/empty: avoids premature full-file scan
    req.files.insert(m_fileId, meta);

    QString jsonErr;
    QByteArray body = ProtocolJson::encodePrepareRequest(req.info, meta, &jsonErr);
    if (body.isEmpty()) {
        if (error) {
            *error = "Failed to encode prepare-upload JSON: " + jsonErr;
        }
        if (m_generation == thisGen && m_state != State_Failed && m_state != State_Cancelled) {
            fail("Failed to encode prepare-upload JSON", Failure_ProtocolError);
        }
        return false;
    }

    if (m_generation != thisGen || m_state != State_Preparing || m_requestId != thisRequestId) {
        return false;
    }

    bool isHttps = (proto == "https");
    if (isHttps) {
        m_httpsPrepareClient = new localsend::PosixTlsHttpClient(
            m_app ? m_app->localIdentity() : localsend::LocalIdentity(), this);
        connect(m_httpsPrepareClient, SIGNAL(finished(int,QByteArray)),
                this, SLOT(onHttpsPrepareFinished(int,QByteArray)));
        connect(m_httpsPrepareClient, SIGNAL(failed(int,QString)),
                this, SLOT(onHttpsPrepareFailed(int,QString)));

        QMap<QString, QString> headers;
        headers.insert("Content-Type", "application/json");

        if (!m_httpsPrepareClient->sendRequest("POST", peer.address, peer.info.port,
                                               "/api/localsend/v2/prepare-upload",
                                               m_activePin, headers, body, 120000)) {
            if (error) {
                *error = "Failed to initiate HTTPS prepare-upload request";
            }
            if (m_generation == thisGen && m_state != State_Failed && m_state != State_Cancelled) {
                fail("Failed to initiate HTTPS prepare-upload request", Failure_NetworkError);
            }
            return false;
        }

        m_consentTimer->start(120000); // 120s consent timeout
        qDebug() << "[SendManager] Sent HTTPS /prepare-upload to" << peer.address.toString() << ":" << peer.info.port
                 << "with frozen pin" << m_activePin << "awaiting consent (timeout 120s)...";
        return true;
    }

    QUrl url;
    url.setScheme("http");
    url.setHost(peer.address.toString());
    url.setPort(peer.info.port);
    url.setPath("/api/localsend/v2/prepare-upload");

    QNetworkRequest netReq(url);
    netReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    m_prepareReply = m_nam->post(netReq, body);
    if (!m_prepareReply) {
        if (error) {
            *error = "Failed to initiate prepare HTTP request";
        }
        if (m_generation == thisGen && m_state != State_Failed && m_state != State_Cancelled) {
            fail("Failed to initiate prepare HTTP request", Failure_NetworkError);
        }
        return false;
    }

    connect(m_prepareReply, SIGNAL(finished()), this, SLOT(onPrepareReplyFinished()));

    m_consentTimer->start(120000); // 120s consent timeout
    qDebug() << "[SendManager] Sent /prepare-upload to" << peer.address.toString() << ":" << peer.info.port
             << "awaiting consent (timeout 120s)...";

    return true;
}

void SendManager::onPrepareReplyFinished()
{
    m_consentTimer->stop();

    if (!m_prepareReply) {
        return;
    }

    int status = m_prepareReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray body = m_prepareReply->readAll();
    QString errStr = m_prepareReply->errorString();
    bool hasNetErr = (m_prepareReply->error() != QNetworkReply::NoError);

    m_prepareReply->disconnect(this);
    m_prepareReply->deleteLater();
    m_prepareReply = 0;

    if (status == 204) {
        // Peer indicated no transfer needed
        qDebug() << "[SendManager] Peer returned 204 No Content (transfer skipped)";
        QString reqId = m_requestId;
        transitionState(State_Completed);
        emit sendFinished(reqId);
        cleanup(true);
        return;
    }

    if (status == 403) {
        fail("Transfer rejected by remote peer (HTTP 403)", Failure_PeerRejected);
        return;
    }

    if (status == 409) {
        fail("Remote peer is busy (HTTP 409)", Failure_PeerBusy);
        return;
    }

    if (status == 401) {
        fail("Remote peer requires PIN (HTTP 401 unsupported)", Failure_PeerPinRequired);
        return;
    }

    if (status == 429) {
        fail("Remote peer rate-limited requests (HTTP 429)", Failure_NetworkError);
        return;
    }

    if (status >= 500) {
        fail(QString("Peer server error during prepare (HTTP %1)").arg(status), Failure_NetworkError);
        return;
    }

    if (status > 0 && status != 200) {
        fail(QString("Prepare request failed with HTTP %1").arg(status), Failure_ProtocolError);
        return;
    }

    if (hasNetErr) {
        fail("Network error during /prepare-upload: " + errStr, Failure_NetworkError);
        return;
    }

    PrepareUploadResponse resp;
    QString err;
    if (!ProtocolJson::decodePrepareResponse(body, &resp, &err)) {
        fail("Malformed prepare response JSON: " + err, Failure_ProtocolError);
        return;
    }

    if (!resp.fileTokens.contains(m_fileId)) {
        fail("File was not accepted by peer (no token returned for fileId)", Failure_PeerRejected);
        return;
    }

    m_sessionId = resp.sessionId;
    m_token = resp.fileTokens.value(m_fileId);

    // Open file for streaming - QFile is kept alive for the duration of the upload
    m_file = new QFile(m_filePath, this);
    if (!m_file->open(QIODevice::ReadOnly)) {
        fail("Failed to open local file for reading: " + m_file->errorString(), Failure_LocalFileNotFound);
        return;
    }

    QUrl uploadUrl;
    uploadUrl.setScheme("http");
    uploadUrl.setHost(m_peer.address.toString());
    uploadUrl.setPort(m_peer.info.port);
    uploadUrl.setPath("/api/localsend/v2/upload");
    setUrlQueryString(uploadUrl, "sessionId=" +
        QString::fromLatin1(QUrl::toPercentEncoding(m_sessionId)) +
        "&fileId=" +
        QString::fromLatin1(QUrl::toPercentEncoding(m_fileId)) +
        "&token=" +
        QString::fromLatin1(QUrl::toPercentEncoding(m_token)));

    QNetworkRequest uploadReq(uploadUrl);
    uploadReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    uploadReq.setHeader(QNetworkRequest::ContentLengthHeader, m_fileSize);

    // Stream directly from QFile using QNetworkAccessManager::post
    m_uploadReply = m_nam->post(uploadReq, m_file);
    if (!m_uploadReply) {
        fail("Failed to initiate streaming upload", Failure_NetworkError);
        return;
    }

    connect(m_uploadReply, SIGNAL(uploadProgress(qint64,qint64)),
            this, SLOT(onUploadProgress(qint64,qint64)));
    connect(m_uploadReply, SIGNAL(finished()),
            this, SLOT(onUploadReplyFinished()));

    transitionState(State_Uploading);
    m_inactivityTimer->start(30000); // 30s inactivity timeout
    qDebug() << "[SendManager] Starting streaming upload to" << m_peer.address.toString()
             << "total bytes:" << m_fileSize;
}

void SendManager::onUploadProgress(qint64 bytesSent, qint64 bytesTotal)
{
    m_inactivityTimer->start(30000); // Reset 30s inactivity timeout on progress

    int percent = 0;
    if (bytesTotal > 0) {
        percent = static_cast<int>((bytesSent * 100) / bytesTotal);
    }
    qDebug() << "[SendManager] Progress:" << bytesSent << "/" << bytesTotal
             << "bytes (" << percent << "%)";

    emit sendProgress(m_requestId, bytesSent, bytesTotal);
}

void SendManager::onUploadReplyFinished()
{
    m_inactivityTimer->stop();

    if (!m_uploadReply) {
        return;
    }

    int status = m_uploadReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString errStr = m_uploadReply->errorString();
    bool hasNetErr = (m_uploadReply->error() != QNetworkReply::NoError);

    m_uploadReply->disconnect(this);
    m_uploadReply->deleteLater();
    m_uploadReply = 0;

    QString reqId = m_requestId;

    if (status == 200) {
        qDebug() << "[SendManager] Upload completed and acknowledged by peer (HTTP 200)";
        transitionState(State_Completed);
        emit sendFinished(reqId);
        cleanup(true);
    } else if (status == 422) {
        fail("Peer reported SHA-256 checksum mismatch (HTTP 422)", Failure_ChecksumMismatch);
    } else if (status >= 500) {
        fail(QString("Peer server error during upload (HTTP %1)").arg(status), Failure_NetworkError);
    } else if (status > 0) {
        fail(QString("Upload rejected by peer (HTTP %1)").arg(status), Failure_ProtocolError);
    } else if (hasNetErr) {
        fail("Network error during streaming upload: " + errStr, Failure_NetworkError);
    }
}

void SendManager::onHttpsPrepareFinished(int status, const QByteArray &body)
{
    m_consentTimer->stop();

    if (!m_httpsPrepareClient) {
        return;
    }

    m_httpsPrepareClient->disconnect(this);
    m_httpsPrepareClient->deleteLater();
    m_httpsPrepareClient = 0;

    if (status == 204) {
        qDebug() << "[SendManager] Peer returned 204 No Content (transfer skipped)";
        QString reqId = m_requestId;
        transitionState(State_Completed);
        emit sendFinished(reqId);
        cleanup(true);
        return;
    }

    if (status == 403) {
        fail("Transfer rejected by remote peer (HTTP 403)", Failure_PeerRejected);
        return;
    }

    if (status == 409) {
        fail("Remote peer is busy (HTTP 409)", Failure_PeerBusy);
        return;
    }

    if (status == 401) {
        fail("Remote peer requires PIN (HTTP 401 unsupported)", Failure_PeerPinRequired);
        return;
    }

    if (status == 429) {
        fail("Remote peer rate-limited requests (HTTP 429)", Failure_NetworkError);
        return;
    }

    if (status >= 500) {
        fail(QString("Peer server error during prepare (HTTP %1)").arg(status), Failure_NetworkError);
        return;
    }

    if (status > 0 && status != 200) {
        fail(QString("Prepare request failed with HTTP %1").arg(status), Failure_ProtocolError);
        return;
    }

    PrepareUploadResponse resp;
    QString err;
    if (!ProtocolJson::decodePrepareResponse(body, &resp, &err)) {
        fail("Malformed prepare response JSON: " + err, Failure_ProtocolError);
        return;
    }

    if (!resp.fileTokens.contains(m_fileId)) {
        fail("File was not accepted by peer (no token returned for fileId)", Failure_PeerRejected);
        return;
    }

    m_sessionId = resp.sessionId;
    m_token = resp.fileTokens.value(m_fileId);

    // Open file for streaming - QFile is kept alive for the duration of the upload
    m_file = new QFile(m_filePath, this);
    if (!m_file->open(QIODevice::ReadOnly)) {
        fail("Failed to open local file for reading: " + m_file->errorString(), Failure_LocalFileNotFound);
        return;
    }

    QString uploadPath = "/api/localsend/v2/upload?sessionId=" +
        QString::fromLatin1(QUrl::toPercentEncoding(m_sessionId)) +
        "&fileId=" +
        QString::fromLatin1(QUrl::toPercentEncoding(m_fileId)) +
        "&token=" +
        QString::fromLatin1(QUrl::toPercentEncoding(m_token));

    QMap<QString, QString> uploadHeaders;
    uploadHeaders.insert("Content-Type", "application/octet-stream");

    m_httpsUploadClient = new localsend::PosixTlsHttpClient(
        m_app ? m_app->localIdentity() : localsend::LocalIdentity(), this);
    connect(m_httpsUploadClient, SIGNAL(uploadProgress(qint64,qint64)),
            this, SLOT(onHttpsUploadProgress(qint64,qint64)));
    connect(m_httpsUploadClient, SIGNAL(finished(int,QByteArray)),
            this, SLOT(onHttpsUploadFinished(int,QByteArray)));
    connect(m_httpsUploadClient, SIGNAL(failed(int,QString)),
            this, SLOT(onHttpsUploadFailed(int,QString)));

    if (!m_httpsUploadClient->sendStreamRequest("POST", m_peer.address, m_peer.info.port,
                                                uploadPath, m_activePin, uploadHeaders,
                                                m_file, m_fileSize, 30000)) {
        fail("Failed to initiate HTTPS streaming upload", Failure_NetworkError);
        return;
    }

    transitionState(State_Uploading);
    m_inactivityTimer->start(30000); // 30s inactivity timeout
    qDebug() << "[SendManager] Starting HTTPS streaming upload to" << m_peer.address.toString()
             << "frozen pin:" << m_activePin << "total bytes:" << m_fileSize;
}

void SendManager::onHttpsPrepareFailed(int tlsErrorCode, const QString &errorMessage)
{
    m_consentTimer->stop();
    if (m_httpsPrepareClient) {
        m_httpsPrepareClient->disconnect(this);
        m_httpsPrepareClient->deleteLater();
        m_httpsPrepareClient = 0;
    }

    if (tlsErrorCode == POSIX_TLS_ERR_PIN_MISMATCH || tlsErrorCode == POSIX_TLS_ERR_INVALID_PIN) {
        fail("Peer certificate pin verification failed during prepare: " + errorMessage, Failure_PeerPinRequired);
    } else {
        fail("HTTPS prepare connection failed: " + errorMessage, Failure_NetworkError);
    }
}

void SendManager::onHttpsUploadProgress(qint64 bytesSent, qint64 totalBytes)
{
    onUploadProgress(bytesSent, totalBytes);
}

void SendManager::onHttpsUploadFinished(int status, const QByteArray &body)
{
    Q_UNUSED(body);
    m_inactivityTimer->stop();

    if (!m_httpsUploadClient) {
        return;
    }

    m_httpsUploadClient->disconnect(this);
    m_httpsUploadClient->deleteLater();
    m_httpsUploadClient = 0;

    QString reqId = m_requestId;

    if (status == 200) {
        qDebug() << "[SendManager] HTTPS upload completed and acknowledged by peer (HTTP 200)";
        transitionState(State_Completed);
        emit sendFinished(reqId);
        cleanup(true);
    } else if (status == 422) {
        fail("Peer reported SHA-256 checksum mismatch (HTTP 422)", Failure_ChecksumMismatch);
    } else if (status >= 500) {
        fail(QString("Peer server error during upload (HTTP %1)").arg(status), Failure_NetworkError);
    } else if (status > 0) {
        fail(QString("Upload rejected by peer (HTTP %1)").arg(status), Failure_ProtocolError);
    } else {
        fail("Upload failed without valid HTTP acknowledgment", Failure_NetworkError);
    }
}

void SendManager::onHttpsUploadFailed(int tlsErrorCode, const QString &errorMessage)
{
    m_inactivityTimer->stop();
    if (m_httpsUploadClient) {
        m_httpsUploadClient->disconnect(this);
        m_httpsUploadClient->deleteLater();
        m_httpsUploadClient = 0;
    }

    if (tlsErrorCode == POSIX_TLS_ERR_PIN_MISMATCH || tlsErrorCode == POSIX_TLS_ERR_INVALID_PIN) {
        fail("Peer certificate pin verification failed during upload: " + errorMessage, Failure_PeerPinRequired);
    } else {
        fail("HTTPS streaming upload network error: " + errorMessage, Failure_NetworkError);
    }
}

void SendManager::cancel()
{
    if (m_state != State_Preparing && m_state != State_Uploading) {
        return;
    }

    quint64 curGen = ++m_generation;
    Q_UNUSED(curGen);
    QString reqId = m_requestId;
    QString sessId = m_sessionId;
    PeerDevice peer = m_peer;
    QString pin = m_activePin;
    bool isHttps = m_isHttps;

    qDebug() << "[SendManager] Cancelling active transfer requestId:" << reqId;
    cleanup(true);
    m_state = State_Cancelled;
    emit sendStateChanged(reqId, "Cancelled");

    if (!sessId.isEmpty()) {
        if (isHttps) {
            sendRemoteCancelHttps(peer, sessId, pin);
        } else {
            sendRemoteCancel(peer, sessId);
        }
    }
}

void SendManager::handleRemoteCancel(const QString &sessionId, const QHostAddress &peer)
{
    if ((m_state == State_Preparing || m_state == State_Uploading) && m_sessionId == sessionId && m_peer.address == peer) {
        qDebug() << "[SendManager] Remote peer cancelled transfer sessionId:" << sessionId;
        fail("Transfer cancelled by remote peer", Failure_PeerRejected);
    }
}

void SendManager::onNetworkLost()
{
    if (m_state == State_Preparing || m_state == State_Uploading) {
        fail("Network lost during transfer", Failure_NetworkError);
    }
}

void SendManager::onConsentTimeout()
{
    if (m_state == State_Preparing) {
        fail("Remote peer consent timed out after 120 seconds", Failure_Timeout);
    }
}

void SendManager::onInactivityTimeout()
{
    if (m_state == State_Uploading) {
        fail("Upload stalled without network progress after 30 seconds", Failure_Timeout);
    }
}

void SendManager::cleanup(bool releaseSlot)
{
    m_consentTimer->stop();
    m_inactivityTimer->stop();

    if (m_prepareReply) {
        m_prepareReply->disconnect(this);
        m_prepareReply->abort();
        m_prepareReply->deleteLater();
        m_prepareReply = 0;
    }

    if (m_uploadReply) {
        m_uploadReply->disconnect(this);
        m_uploadReply->abort();
        m_uploadReply->deleteLater();
        m_uploadReply = 0;
    }

    if (m_httpsPrepareClient) {
        m_httpsPrepareClient->disconnect(this);
        m_httpsPrepareClient->abort();
        m_httpsPrepareClient->deleteLater();
        m_httpsPrepareClient = 0;
    }

    if (m_httpsUploadClient) {
        m_httpsUploadClient->disconnect(this);
        m_httpsUploadClient->abort();
        m_httpsUploadClient->deleteLater();
        m_httpsUploadClient = 0;
    }

    if (m_file) {
        m_file->close();
        m_file->deleteLater();
        m_file = 0;
    }

    QString reqId = m_requestId;

    // Retain terminal state (Completed, Failed, Cancelled)
    if (m_state != State_Completed && m_state != State_Failed && m_state != State_Cancelled) {
        m_state = State_Idle;
    }
    m_requestId.clear();
    m_sessionId.clear();
    m_fileId.clear();
    m_token.clear();
    m_filePath.clear();
    m_fileSize = 0;
    m_activePin.clear();
    m_isHttps = false;

    if (releaseSlot && m_app && !reqId.isEmpty()) {
        m_app->endTransfer(reqId);
    }
}

void SendManager::sendRemoteCancelHttps(const PeerDevice &peer, const QString &sessionId, const QString &pin)
{
    if (sessionId.isEmpty() || pin.isEmpty()) {
        return;
    }

    localsend::PosixTlsHttpClient *cancelClient = new localsend::PosixTlsHttpClient(
        m_app ? m_app->localIdentity() : localsend::LocalIdentity(), this);
    connect(cancelClient, SIGNAL(finished(int,QByteArray)), cancelClient, SLOT(deleteLater()));
    connect(cancelClient, SIGNAL(failed(int,QString)), cancelClient, SLOT(deleteLater()));

    QString cancelPath = "/api/localsend/v2/cancel?sessionId=" +
        QString::fromLatin1(QUrl::toPercentEncoding(sessionId));
    QMap<QString, QString> headers;
    headers.insert("Content-Type", "application/json");

    cancelClient->sendRequest("POST", peer.address, peer.info.port, cancelPath, pin, headers, QByteArray(), 3000);
}

void SendManager::sendRemoteCancel(const PeerDevice &peer, const QString &sessionId)
{
    if (sessionId.isEmpty() || !m_nam) {
        return;
    }

    QUrl cancelUrl;
    cancelUrl.setScheme("http");
    cancelUrl.setHost(peer.address.toString());
    cancelUrl.setPort(peer.info.port);
    cancelUrl.setPath("/api/localsend/v2/cancel");
    setUrlQueryString(cancelUrl, "sessionId=" +
        QString::fromLatin1(QUrl::toPercentEncoding(sessionId)));

    QNetworkRequest req(cancelUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = m_nam->post(req, QByteArray());
    if (reply) {
        connect(reply, SIGNAL(finished()), reply, SLOT(deleteLater()));
        QTimer *timer = new QTimer(reply);
        timer->setSingleShot(true);
        connect(timer, SIGNAL(timeout()), reply, SLOT(abort()));
        timer->start(3000);
    }
}
