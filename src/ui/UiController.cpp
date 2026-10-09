#include "ui/UiController.h"

#include <QHostAddress>
#include <QDebug>

#if defined(BB_CASCADES) || defined(Q_OS_QNX)
#include <bb/cascades/pickers/FilePicker.hpp>
#include <bb/cascades/pickers/FilePickerMode.hpp>
#include <bb/cascades/pickers/FilePickerSortFlag.hpp>
#include <bb/system/InvokeManager.hpp>
#include <bb/system/InvokeRequest.hpp>
#else
#include <QDesktopServices>
#endif
#include <QUrl>
#include <QFile>
#include <QDir>

namespace localsend {

UiController::UiController(Application *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_transferState("Idle")
    , m_transferProgress(0)
    , m_transferStatusText("Ready")
    , m_hasIncomingOffer(false)
{
    Q_ASSERT(m_app != 0);

    // Connect Application signals
    connect(m_app, SIGNAL(localInfoChanged(DeviceInfo)),
            this, SIGNAL(deviceAliasChanged()));
    connect(m_app, SIGNAL(localInfoChanged(DeviceInfo)),
            this, SIGNAL(devicePortChanged()));
    connect(m_app, SIGNAL(transferSlotChanged(bool,QString)),
            this, SLOT(onTransferSlotChanged(bool,QString)));

    // Connect DiscoveryManager signals
    if (m_app->discoveryManager()) {
        connect(m_app->discoveryManager(), SIGNAL(peerUpdated(PeerDevice)),
                this, SLOT(onPeerUpdated(PeerDevice)));
        connect(m_app->discoveryManager(), SIGNAL(peerRemoved(QString)),
                this, SLOT(onPeerRemoved(QString)));
        connect(m_app->discoveryManager(), SIGNAL(discoveryError(QString)),
                this, SLOT(onDiscoveryError(QString)));
    }

    // Connect SendManager signals
    if (m_app->sendManager()) {
        connect(m_app->sendManager(), SIGNAL(sendStateChanged(QString,QString)),
                this, SLOT(onSendStateChanged(QString,QString)));
        connect(m_app->sendManager(), SIGNAL(sendProgress(QString,qint64,qint64)),
                this, SLOT(onSendProgress(QString,qint64,qint64)));
        connect(m_app->sendManager(), SIGNAL(sendFinished(QString)),
                this, SLOT(onSendFinished(QString)));
        connect(m_app->sendManager(), SIGNAL(sendFailed(QString,QString)),
                this, SLOT(onSendFailed(QString,QString)));
    }

    // Connect ReceiveServer signals
    if (m_app->receiveServer()) {
        connect(m_app->receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                this, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));
        connect(m_app->receiveServer(), SIGNAL(receiveProgress(QString,qint64,qint64)),
                this, SLOT(onReceiveProgress(QString,qint64,qint64)));
        connect(m_app->receiveServer(), SIGNAL(receiveFinished(QString,QString)),
                this, SLOT(onReceiveFinished(QString,QString)));
        connect(m_app->receiveServer(), SIGNAL(receiveCancelled(QString,QString)),
                this, SLOT(onReceiveCancelled(QString,QString)));
        connect(m_app->receiveServer(), SIGNAL(offerCancelled(QString,QString)),
                this, SLOT(onReceiveOfferCancelled(QString,QString)));
        connect(m_app->receiveServer(), SIGNAL(receiveFailed(QString,QString)),
                this, SLOT(onReceiveFailed(QString,QString)));
    }

    updatePeersModel();
}

UiController::~UiController()
{
#if defined(BB_CASCADES) || defined(Q_OS_QNX)
    if (m_activeFilePicker) {
        delete m_activeFilePicker.data();
        m_activeFilePicker = 0;
    }
#endif
}

QString UiController::deviceAlias() const
{
    return m_app->localInfo().alias;
}

void UiController::setDeviceAlias(const QString &alias)
{
    if (alias.trimmed().isEmpty()) {
        return;
    }
    m_app->setAlias(alias.trimmed());
    emit deviceAliasChanged();
}

int UiController::devicePort() const
{
    return m_app->localInfo().port;
}

QString UiController::receiveDirectory() const
{
    return m_app->receiveDirectory();
}

bool UiController::transferActive() const
{
    return m_app->isTransferActive();
}

void UiController::refreshDevices()
{
    if (m_app->discoveryManager()) {
        m_app->discoveryManager()->refresh();
    }
}

void UiController::addManualPeer(const QString &ipv4, int port)
{
    if (ipv4.trimmed().isEmpty() || port <= 0 || port > 65535) {
        m_lastError = "Invalid IP address or port";
        emit lastErrorChanged();
        return;
    }

    QHostAddress addr(ipv4.trimmed());
    if (addr.isNull() || addr.protocol() != QAbstractSocket::IPv4Protocol) {
        m_lastError = "Please enter a valid IPv4 address";
        emit lastErrorChanged();
        return;
    }

    if (m_app->discoveryManager()) {
        m_app->discoveryManager()->registerPeer(addr, (quint16)port);
    }
}

void UiController::chooseFile()
{
#if defined(BB_CASCADES) || defined(Q_OS_QNX)
    if (m_activeFilePicker) {
        // Picker dialog already active; prevent duplicate allocations
        return;
    }

    bb::cascades::pickers::FilePicker *picker = new bb::cascades::pickers::FilePicker(this);
    m_activeFilePicker = picker;
    picker->setType(bb::cascades::pickers::FileType::Other);
    picker->setTitle(tr("Select File to Send"));
    picker->setMode(bb::cascades::pickers::FilePickerMode::Picker);
    connect(picker, SIGNAL(fileSelected(QStringList)), this, SLOT(onFilePicked(QStringList)));
    connect(picker, SIGNAL(canceled()), this, SLOT(onFilePickerCanceled()));
    picker->open();
#else
    qDebug() << "[UiController] chooseFile called in non-Cascades environment";
#endif
}

void UiController::onFilePicked(const QStringList &selectedFiles)
{
#if defined(BB_CASCADES) || defined(Q_OS_QNX)
    if (m_activeFilePicker) {
        m_activeFilePicker->deleteLater();
        m_activeFilePicker = 0;
    }
#endif
    if (!selectedFiles.isEmpty()) {
        setSelectedFilePath(selectedFiles.first());
    }
}

void UiController::onFilePickerCanceled()
{
#if defined(BB_CASCADES) || defined(Q_OS_QNX)
    if (m_activeFilePicker) {
        m_activeFilePicker->deleteLater();
        m_activeFilePicker = 0;
    }
#endif
}

void UiController::setSelectedFilePath(const QString &path)
{
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        m_lastError = QString("File not found: %1").arg(path);
        emit lastErrorChanged();
        return;
    }

    m_selectedFilePath = fi.absoluteFilePath();
    m_selectedFileName = fi.fileName();
    m_selectedFileSize = formatBytes(fi.size());

    emit selectedFileChanged();
}

void UiController::clearSelectedFile()
{
    m_selectedFilePath.clear();
    m_selectedFileName.clear();
    m_selectedFileSize.clear();

    emit selectedFileChanged();
}

bool UiController::sendSelectedFile(const QString &fingerprint)
{
    if (!hasSelectedFile()) {
        m_lastError = "No file selected to send";
        emit lastErrorChanged();
        return false;
    }

    if (m_app->isTransferActive()) {
        m_lastError = "Another transfer is already in progress";
        emit lastErrorChanged();
        return false;
    }

    PeerDevice peer;
    if (m_app->discoveryManager()) {
        peer = m_app->discoveryManager()->findPeer(fingerprint);
    }

    if (peer.info.fingerprint.isEmpty()) {
        m_lastError = "Target peer device not found";
        emit lastErrorChanged();
        return false;
    }

    if (peer.info.protocol.toLower() == "https") {
        m_lastError = "Target peer requires HTTPS which is unsupported in this MVP";
        emit lastErrorChanged();
        return false;
    }

    QString err;
    bool ok = m_app->sendManager()->sendFile(peer, m_selectedFilePath, &err);
    if (!ok) {
        m_lastError = err.isEmpty() ? "Failed to initiate file transfer" : err;
        emit lastErrorChanged();
        return false;
    }

    m_transferState = "Preparing";
    m_transferProgress = 0;
    m_transferStatusText = QString("Preparing to send %1...").arg(m_selectedFileName);

    emit transferStateChanged();
    emit transferProgressChanged();
    emit transferStatusTextChanged();
    return true;
}

void UiController::setSelectedIncomingFileId(const QString &fileId)
{
    selectIncomingFile(fileId);
}

void UiController::selectIncomingFile(const QString &fileId)
{
    if (fileId.isEmpty() || m_selectedIncomingFileId == fileId) {
        return;
    }

    for (int i = 0; i < m_incomingFiles.size(); ++i) {
        QVariantMap map = m_incomingFiles.at(i).toMap();
        if (map.value("id").toString() == fileId) {
            m_selectedIncomingFileId = fileId;
            m_incomingFileId = fileId;
            m_incomingFileName = map.value("fileName").toString();
            m_incomingFileSize = map.value("formattedSize").toString();
            emit selectedIncomingFileIdChanged();
            emit incomingOfferChanged();
            return;
        }
    }
}

void UiController::acceptIncoming(const QString &requestId, const QString &fileId)
{
    if (!m_hasIncomingOffer || m_incomingRequestId != requestId) {
        return;
    }

    QString targetFileId = fileId.trimmed().isEmpty() ? m_selectedIncomingFileId : fileId.trimmed();
    bool ok = false;
    if (m_app->receiveServer()) {
        ok = m_app->receiveServer()->accept(requestId, targetFileId);
    }

    if (!ok) {
        m_hasIncomingOffer = false;
        m_transferState = "Failed";
        m_lastError = "Failed to accept file (destination directory or storage error)";
        m_transferStatusText = m_lastError;

        emit incomingOfferChanged();
        emit transferStateChanged();
        emit transferStatusTextChanged();
        emit lastErrorChanged();
        return;
    }

    m_hasIncomingOffer = false;
    m_transferState = "Receiving";
    m_transferProgress = 0;
    m_transferStatusText = QString("Accepting %1...").arg(m_incomingFileName);

    emit incomingOfferChanged();
    emit transferStateChanged();
    emit transferProgressChanged();
    emit transferStatusTextChanged();
}

void UiController::rejectIncoming(const QString &requestId)
{
    if (!m_hasIncomingOffer || m_incomingRequestId != requestId) {
        return;
    }

    if (m_app->receiveServer()) {
        m_app->receiveServer()->reject(requestId);
    }

    m_hasIncomingOffer = false;
    m_transferState = "Idle";
    m_transferStatusText = "Transfer rejected";

    emit incomingOfferChanged();
    emit transferStateChanged();
    emit transferStatusTextChanged();
}

void UiController::cancelTransfer()
{
    bool sendActive = m_app->sendManager() &&
                      (m_app->sendManager()->state() == SendManager::State_Preparing ||
                       m_app->sendManager()->state() == SendManager::State_Uploading);

    if (sendActive) {
        m_app->sendManager()->cancel();
    } else if (m_app->receiveServer() && m_app->receiveServer()->hasPendingOffer()) {
        m_app->receiveServer()->reject(m_app->receiveServer()->pendingRequestId());
    } else if (m_app->receiveServer() && m_app->receiveServer()->hasActiveSession()) {
        m_app->receiveServer()->cancelLocal(m_app->receiveServer()->activeRequestId());
    } else if (m_app->isTransferActive()) {
        QString activeId = m_app->activeTransferId();
        if (m_app->sendManager() && m_app->sendManager()->activeRequestId() == activeId) {
            m_app->sendManager()->cancel();
        } else if (m_app->receiveServer()) {
            m_app->receiveServer()->cancelLocal(activeId);
        }
    } else {
        // Nothing active to cancel
        return;
    }

    m_transferState = "Cancelled";
    m_transferStatusText = "Transfer cancelled by user";

    emit transferStateChanged();
    emit transferStatusTextChanged();
}

void UiController::clearError()
{
    m_lastError.clear();
    emit lastErrorChanged();
}

void UiController::openFile(const QString &filePath)
{
    QString target = filePath.trimmed().isEmpty() ? m_lastSavedFilePath : filePath.trimmed();
    if (target.isEmpty() || !QFile::exists(target)) {
        m_lastError = "File does not exist: " + target;
        emit lastErrorChanged();
        return;
    }

#if defined(BB_CASCADES) || defined(Q_OS_QNX)
    bb::system::InvokeManager mgr;
    bb::system::InvokeRequest request;
    request.setAction("bb.action.VIEW");
    request.setUri(QUrl::fromLocalFile(target));
    mgr.invoke(request);
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(target));
#endif
}

void UiController::openReceiveDirectory()
{
    QString dirPath = m_app->receiveDirectory();
    QDir dir(dirPath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

#if defined(BB_CASCADES) || defined(Q_OS_QNX)
    bb::system::InvokeManager mgr;
    bb::system::InvokeRequest request;
    request.setAction("bb.action.OPEN");
    request.setUri(QUrl::fromLocalFile(dirPath));
    mgr.invoke(request);
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(dirPath));
#endif
}

void UiController::onPeerUpdated(const PeerDevice &peer)
{
    Q_UNUSED(peer);
    updatePeersModel();
}

void UiController::onPeerRemoved(const QString &fingerprint)
{
    Q_UNUSED(fingerprint);
    updatePeersModel();
}

void UiController::onDiscoveryError(const QString &error)
{
    m_lastError = QString("Discovery error: %1").arg(error);
    emit lastErrorChanged();
}

void UiController::updatePeersModel()
{
    if (!m_app->discoveryManager()) {
        return;
    }

    QList<PeerDevice> peerList = m_app->discoveryManager()->peers();
    QVariantList list;

    for (int i = 0; i < peerList.size(); ++i) {
        const PeerDevice &p = peerList.at(i);
        QVariantMap map;
        map["alias"] = p.info.alias.isEmpty() ? "Unknown Device" : p.info.alias;
        map["deviceModel"] = p.info.deviceModel.isEmpty() ? "LocalSend Peer" : p.info.deviceModel;
        map["deviceType"] = p.info.deviceType.isEmpty() ? "desktop" : p.info.deviceType;
        map["address"] = p.address.toString();
        map["port"] = p.info.port;
        map["fingerprint"] = p.info.fingerprint;
        map["protocol"] = p.info.protocol;
        map["isHttps"] = (p.info.protocol.toLower() == "https");
        map["displayAddress"] = QString("%1:%2").arg(p.address.toString()).arg(p.info.port);
        list.append(map);
    }

    m_peers = list;
    emit peersChanged();
}

void UiController::onSendStateChanged(const QString &requestId, const QString &state)
{
    Q_UNUSED(requestId);
    m_transferState = state;

    if (state == "Preparing") {
        m_transferStatusText = "Connecting and awaiting recipient approval...";
    } else if (state == "Uploading") {
        m_transferStatusText = "Uploading file...";
    } else if (state == "Completed") {
        m_transferStatusText = "Transfer completed successfully!";
    } else if (state == "Failed") {
        m_transferStatusText = "Transfer failed";
    } else if (state == "Cancelled") {
        m_transferStatusText = "Transfer cancelled";
    }

    emit transferStateChanged();
    emit transferStatusTextChanged();
}

void UiController::onSendProgress(const QString &requestId, qint64 done, qint64 total)
{
    Q_UNUSED(requestId);
    m_transferProgress = total > 0 ? (int)((done * 100) / total) : 0;
    m_transferStatusText = QString("Sending: %1 / %2 (%3%)")
                               .arg(formatBytes(done))
                               .arg(formatBytes(total))
                               .arg(m_transferProgress);

    emit transferProgressChanged();
    emit transferStatusTextChanged();
}

void UiController::onSendFinished(const QString &requestId)
{
    Q_UNUSED(requestId);
    m_transferState = "Completed";
    m_transferProgress = 100;
    m_transferStatusText = "File sent successfully!";

    emit transferStateChanged();
    emit transferProgressChanged();
    emit transferStatusTextChanged();
    emit transferFinished(m_transferStatusText);
}

void UiController::onSendFailed(const QString &requestId, const QString &reason)
{
    Q_UNUSED(requestId);
    m_transferState = "Failed";
    m_lastError = QString("Send failed: %1").arg(reason);
    m_transferStatusText = m_lastError;

    emit transferStateChanged();
    emit transferStatusTextChanged();
    emit lastErrorChanged();
}

void UiController::onIncomingOffer(const QString &requestId, const PeerDevice &peer, const QList<FileMeta> &files)
{
    m_hasIncomingOffer = true;
    m_incomingRequestId = requestId;
    m_incomingPeerAlias = peer.info.alias.isEmpty() ? peer.address.toString() : peer.info.alias;

    m_incomingFiles.clear();
    for (int i = 0; i < files.size(); ++i) {
        const FileMeta &meta = files.at(i);
        QVariantMap map;
        map["id"] = meta.id;
        map["fileName"] = meta.fileName;
        map["size"] = meta.size;
        map["formattedSize"] = formatBytes(meta.size);
        map["fileType"] = meta.fileType;
        m_incomingFiles.append(map);
    }

    if (!files.isEmpty()) {
        const FileMeta &meta = files.first();
        m_selectedIncomingFileId = meta.id;
        m_incomingFileName = meta.fileName;
        m_incomingFileSize = formatBytes(meta.size);
        m_incomingFileId = meta.id;
    } else {
        m_selectedIncomingFileId.clear();
        m_incomingFileName = "Unknown";
        m_incomingFileSize = "0 B";
        m_incomingFileId.clear();
    }

    m_transferState = "Preparing";
    m_transferStatusText = QString("Incoming transfer request from %1").arg(m_incomingPeerAlias);

    emit incomingOfferChanged();
    emit selectedIncomingFileIdChanged();
    emit transferStateChanged();
    emit transferStatusTextChanged();
}

void UiController::onReceiveProgress(const QString &sessionId, qint64 done, qint64 total)
{
    Q_UNUSED(sessionId);
    m_transferState = "Receiving";
    m_transferProgress = total > 0 ? (int)((done * 100) / total) : 0;
    m_transferStatusText = QString("Receiving: %1 / %2 (%3%)")
                               .arg(formatBytes(done))
                               .arg(formatBytes(total))
                               .arg(m_transferProgress);

    emit transferStateChanged();
    emit transferProgressChanged();
    emit transferStatusTextChanged();
}

void UiController::onReceiveFinished(const QString &sessionId, const QString &savedPath)
{
    Q_UNUSED(sessionId);
    m_transferState = "Completed";
    m_transferProgress = 100;
    m_hasIncomingOffer = false;
    m_lastSavedFilePath = savedPath;

    QString baseName = QFileInfo(savedPath).fileName();
    m_transferStatusText = QString("Received %1 successfully! Saved to: %2").arg(baseName).arg(savedPath);

    emit incomingOfferChanged();
    emit lastSavedFilePathChanged();
    emit transferStateChanged();
    emit transferProgressChanged();
    emit transferStatusTextChanged();
    emit transferFinished(m_transferStatusText);
}

void UiController::onReceiveCancelled(const QString &sessionId, const QString &reason)
{
    Q_UNUSED(sessionId);
    m_transferState = "Cancelled";
    m_hasIncomingOffer = false;
    m_transferStatusText = QString("Receive cancelled: %1").arg(reason);

    emit incomingOfferChanged();
    emit transferStateChanged();
    emit transferStatusTextChanged();
}

void UiController::onReceiveOfferCancelled(const QString &requestId, const QString &reason)
{
    if (m_hasIncomingOffer && m_incomingRequestId == requestId) {
        m_hasIncomingOffer = false;
        m_transferState = "Idle";
        m_transferStatusText = QString("Offer cancelled: %1").arg(reason);

        emit incomingOfferChanged();
        emit transferStateChanged();
        emit transferStatusTextChanged();
    }
}

void UiController::onReceiveFailed(const QString &id, const QString &reason)
{
    Q_UNUSED(id);
    m_hasIncomingOffer = false;
    m_transferState = "Failed";
    m_lastError = QString("Receive failed: %1").arg(reason);
    m_transferStatusText = m_lastError;

    emit incomingOfferChanged();
    emit transferStateChanged();
    emit transferStatusTextChanged();
    emit lastErrorChanged();
}

void UiController::onTransferSlotChanged(bool busy, const QString &activeRequestId)
{
    Q_UNUSED(activeRequestId);
    if (!busy) {
        if (m_hasIncomingOffer && m_app->receiveServer() && !m_app->receiveServer()->hasPendingOffer()) {
            m_hasIncomingOffer = false;
            emit incomingOfferChanged();
        }
    }
}

QString UiController::formatBytes(qint64 bytes)
{
    if (bytes < 1024) {
        return QString("%1 B").arg(bytes);
    } else if (bytes < 1024 * 1024) {
        double kb = (double)bytes / 1024.0;
        return QString("%1 KB").arg(QString::number(kb, 'f', 1));
    } else if (bytes < (qint64)1024 * 1024 * 1024) {
        double mb = (double)bytes / (1024.0 * 1024.0);
        return QString("%1 MB").arg(QString::number(mb, 'f', 1));
    } else {
        double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
        return QString("%1 GB").arg(QString::number(gb, 'f', 2));
    }
}

} // namespace localsend
