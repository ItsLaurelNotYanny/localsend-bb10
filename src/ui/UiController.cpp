#include "ui/UiController.h"

#include <QHostAddress>
#include <QDebug>

#if defined(BB_CASCADES) || defined(Q_OS_QNX)
#include <bb/cascades/pickers/FilePicker.hpp>
#include <bb/cascades/pickers/FilePickerMode.hpp>
#include <bb/cascades/pickers/FilePickerSortFlag.hpp>
#endif

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
    }

    updatePeersModel();
}

UiController::~UiController()
{
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
    bb::cascades::pickers::FilePicker *picker = new bb::cascades::pickers::FilePicker(this);
    picker->setType(bb::cascades::pickers::FileType::Other);
    picker->setTitle(tr("Select File to Send"));
    picker->setMode(bb::cascades::pickers::FilePickerMode::Picker);
    connect(picker, SIGNAL(fileSelected(QStringList)), this, SLOT(onFilePicked(QStringList)));
    picker->open();
#else
    qDebug() << "[UiController] chooseFile called in non-Cascades environment";
#endif
}

void UiController::onFilePicked(const QStringList &selectedFiles)
{
    if (!selectedFiles.isEmpty()) {
        setSelectedFilePath(selectedFiles.first());
    }
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

void UiController::acceptIncoming(const QString &requestId, const QString &fileId)
{
    if (m_app->receiveServer()) {
        m_app->receiveServer()->accept(requestId, fileId);
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
    if (m_app->sendManager() && m_app->sendManager()->state() != SendManager::State_Idle) {
        m_app->sendManager()->cancel();
    } else if (m_app->receiveServer()) {
        if (m_app->receiveServer()->hasPendingOffer()) {
            m_app->receiveServer()->reject(m_app->receiveServer()->pendingRequestId());
        } else if (m_app->receiveServer()->hasActiveSession()) {
            m_app->receiveServer()->cancelLocal(m_app->receiveServer()->pendingRequestId());
        }
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

    if (!files.isEmpty()) {
        const FileMeta &meta = files.first();
        m_incomingFileName = meta.fileName;
        m_incomingFileSize = formatBytes(meta.size);
        m_incomingFileId = meta.id;
    } else {
        m_incomingFileName = "Unknown";
        m_incomingFileSize = "0 B";
        m_incomingFileId.clear();
    }

    m_transferState = "Preparing";
    m_transferStatusText = QString("Incoming transfer request from %1").arg(m_incomingPeerAlias);

    emit incomingOfferChanged();
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

    QString baseName = QFileInfo(savedPath).fileName();
    m_transferStatusText = QString("Received %1 successfully!").arg(baseName);

    emit incomingOfferChanged();
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
