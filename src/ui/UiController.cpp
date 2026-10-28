#include "ui/UiController.h"

#include <QHostAddress>
#include <QDebug>

#if defined(BB_CASCADES) || defined(Q_OS_QNX)
#include <bb/cascades/pickers/FilePicker.hpp>
#include <bb/cascades/pickers/FilePickerMode.hpp>
#include <bb/cascades/pickers/FilePickerSortFlag.hpp>
#include <bb/cascades/Container>
#include <bb/cascades/Label>
#include <bb/cascades/Button>
#include <bb/cascades/StackLayout>
#include <bb/cascades/StackLayoutProperties>
#include <bb/cascades/SystemDefaults>
#include <bb/cascades/TextStyleDefinition>
#include <bb/cascades/Color>
#include <bb/cascades/VerticalAlignment>
#include <bb/cascades/LayoutOrientation>
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
    , m_peersRevision(0)
    , m_transferState("Idle")
    , m_transferProgress(0)
    , m_transferStatusText("Ready")
    , m_sha256Verified(false)
    , m_hasIncomingOffer(false)
{
    Q_ASSERT(m_app != 0);

    // Connect Application signals
    connect(m_app, SIGNAL(localInfoChanged(DeviceInfo)),
            this, SLOT(onLocalInfoChanged(DeviceInfo)));
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
        connect(m_app->discoveryManager(), SIGNAL(scanningChanged(bool)),
                this, SLOT(onScanningChanged(bool)));
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

QString UiController::certificateFingerprint() const
{
    return m_app ? m_app->localInfo().fingerprint : QString();
}

bool UiController::isHttps() const
{
    return m_app ? (m_app->localInfo().protocol.toLower() == "https") : false;
}

QString UiController::serverProtocol() const
{
    return m_app ? m_app->localInfo().protocol : "http";
}

void UiController::onLocalInfoChanged(const DeviceInfo &info)
{
    Q_UNUSED(info);
    emit deviceAliasChanged();
    emit devicePortChanged();
    emit certificateFingerprintChanged();
    emit isHttpsChanged();
    emit serverProtocolChanged();
}

bool UiController::transferActive() const
{
    return m_app->isTransferActive();
}

bool UiController::isScanning() const
{
    return m_app->discoveryManager() ? m_app->discoveryManager()->isScanning() : false;
}

QString UiController::networkStatus() const
{
    if (!m_app->discoveryManager()) {
        return "Network: Offline";
    }
    QString ip = m_app->discoveryManager()->activeInterfaceAddress();
    if (ip.isEmpty()) {
        return "Wi-Fi: Disconnected";
    }
    QString subnet = m_app->discoveryManager()->activeInterfaceSubnet();
    return QString("Wi-Fi: %1 (%2)").arg(ip).arg(subnet);
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
    populatePeers();
}

void UiController::clearSelectedFile()
{
    m_selectedFilePath.clear();
    m_selectedFileName.clear();
    m_selectedFileSize.clear();

    emit selectedFileChanged();
    populatePeers();
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
    m_sha256Verified = false;
    m_lastTransferSha256.clear();
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
    if (target.endsWith(".txt", Qt::CaseInsensitive)) {
        request.setMimeType("text/plain");
    }
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

QString UiController::diagnosticLogPath() const
{
    QString docPath = "/accounts/1000/shared/documents/localsend_launch.txt";
    if (QFile::exists(docPath)) {
        return docPath;
    }
    QString dataPath = "data/localsend_launch.txt";
    if (QFile::exists(dataPath)) {
        return dataPath;
    }
    return docPath;
}

void UiController::openDiagnosticLog()
{
    QString path = diagnosticLogPath();
    if (!QFile::exists(path)) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << "LocalSend diagnostic log initialized.\n";
            f.close();
        }
    }
    openFile(path);
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

void UiController::onScanningChanged(bool isScanning)
{
    Q_UNUSED(isScanning);
    emit isScanningChanged();
    emit networkStatusChanged();
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
        if (p.info.fingerprint == m_app->localInfo().fingerprint) {
            continue;
        }
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
    m_peersRevision++;
    emit peersChanged();
    emit networkStatusChanged();
    populatePeers();
}

void UiController::onSendStateChanged(const QString &requestId, const QString &state)
{
    Q_UNUSED(requestId);
    m_transferState = state;

    if (state == "Preparing") {
        m_sha256Verified = false;
        m_lastTransferSha256.clear();
        m_transferStatusText = "Connecting and awaiting recipient approval...";
    } else if (state == "Uploading") {
        m_sha256Verified = false;
        m_lastTransferSha256.clear();
        m_transferStatusText = "Uploading file...";
    } else if (state == "Completed") {
        m_transferStatusText = "Transfer completed successfully!";
        m_sha256Verified = true;
        if (!m_selectedFilePath.isEmpty() && QFile::exists(m_selectedFilePath)) {
            m_lastTransferSha256 = CryptoUtil::sha256File(m_selectedFilePath);
        }
    } else if (state == "Failed") {
        m_sha256Verified = false;
        m_lastTransferSha256.clear();
        m_transferStatusText = "Transfer failed";
    } else if (state == "Cancelled") {
        m_sha256Verified = false;
        m_lastTransferSha256.clear();
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

    m_sha256Verified = false;
    m_lastTransferSha256.clear();
    if (m_app && m_app->receiveServer()) {
        m_sha256Verified = m_app->receiveServer()->lastSha256Verified();
        m_lastTransferSha256 = m_app->receiveServer()->lastComputedSha256();
    }
    if (m_lastTransferSha256.isEmpty() && QFile::exists(savedPath)) {
        m_lastTransferSha256 = CryptoUtil::sha256File(savedPath);
        m_sha256Verified = !m_lastTransferSha256.isEmpty();
    }

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
    m_sha256Verified = false;
    m_lastTransferSha256.clear();
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
    emit transferStateChanged();
    populatePeers();
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

void UiController::setPeersContainer(QObject *containerObj)
{
    m_peersContainer = containerObj;
    populatePeers();
}

void UiController::populatePeers(QObject *containerObj)
{
#if defined(BB_CASCADES) || defined(Q_OS_QNX)
    QObject *target = containerObj ? containerObj : m_peersContainer.data();
    bb::cascades::Container *container = qobject_cast<bb::cascades::Container*>(target);
    if (!container) {
        return;
    }

    container->removeAll();

    for (int i = 0; i < m_peers.size(); ++i) {
        QVariantMap p = m_peers.at(i).toMap();
        QString alias = p.value("alias").toString();
        QString model = p.value("deviceModel").toString();
        QString addr = p.value("displayAddress").toString();
        QString fp = p.value("fingerprint").toString();
        bool isHttps = p.value("isHttps").toBool();

        bb::cascades::Container *card = new bb::cascades::Container();
        card->setTopPadding(16.0f);
        card->setBottomPadding(16.0f);
        card->setLeftPadding(16.0f);
        card->setRightPadding(16.0f);
        card->setBottomMargin(12.0f);
        card->setBackground(bb::cascades::Color::fromARGB(0xFFF7FAFC));

        bb::cascades::Container *row = new bb::cascades::Container();
        bb::cascades::StackLayout *layout = new bb::cascades::StackLayout();
        layout->setOrientation(bb::cascades::LayoutOrientation::LeftToRight);
        row->setLayout(layout);

        // Left column: Device details
        bb::cascades::Container *left = new bb::cascades::Container();
        bb::cascades::StackLayoutProperties *leftProps = new bb::cascades::StackLayoutProperties();
        leftProps->setSpaceQuota(3.0f);
        left->setLayoutProperties(leftProps);

        bb::cascades::Label *aliasLabel = new bb::cascades::Label();
        aliasLabel->setText(alias);
        aliasLabel->textStyle()->setBase(bb::cascades::SystemDefaults::TextStyles::titleText());
        aliasLabel->textStyle()->setColor(bb::cascades::Color::fromARGB(0xFF2D3748));
        left->add(aliasLabel);

        bb::cascades::Label *subLabel = new bb::cascades::Label();
        subLabel->setText(model + QString::fromUtf8(" • ") + addr);
        subLabel->textStyle()->setBase(bb::cascades::SystemDefaults::TextStyles::smallText());
        subLabel->textStyle()->setColor(bb::cascades::Color::fromARGB(0xFF718096));
        left->add(subLabel);

        if (isHttps) {
            bb::cascades::Label *httpsLabel = new bb::cascades::Label();
            httpsLabel->setText("HTTPS");
            httpsLabel->textStyle()->setBase(bb::cascades::SystemDefaults::TextStyles::smallText());
            httpsLabel->textStyle()->setColor(bb::cascades::Color::fromARGB(0xFF38A169));
            left->add(httpsLabel);
        }
        row->add(left);

        // Right column: Send button
        bb::cascades::Button *sendBtn = new bb::cascades::Button();
        sendBtn->setText("Send");
        sendBtn->setEnabled(hasSelectedFile() && !transferActive());
        sendBtn->setProperty("peerFingerprint", fp);
        sendBtn->setVerticalAlignment(bb::cascades::VerticalAlignment::Center);
        bb::cascades::StackLayoutProperties *btnProps = new bb::cascades::StackLayoutProperties();
        btnProps->setSpaceQuota(1.0f);
        sendBtn->setLayoutProperties(btnProps);

        connect(sendBtn, SIGNAL(clicked()), this, SLOT(onSendCardButtonClicked()));
        row->add(sendBtn);

        card->add(row);
        container->add(card);
    }
#else
    Q_UNUSED(containerObj);
#endif
}

void UiController::onSendCardButtonClicked()
{
#if defined(BB_CASCADES) || defined(Q_OS_QNX)
    bb::cascades::Button *btn = qobject_cast<bb::cascades::Button*>(sender());
    if (btn) {
        QString fp = btn->property("peerFingerprint").toString();
        if (!fp.isEmpty()) {
            sendSelectedFile(fp);
        }
    }
#endif
}

} // namespace localsend
