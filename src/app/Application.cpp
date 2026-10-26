#include "app/Application.h"

#include <QUuid>
#include <QThread>
#include <QFileInfo>

#include "discovery/DiscoveryManager.h"
#include "receive/ReceiveServer.h"
#include "send/SendManager.h"

namespace localsend {

Application::Application(QObject *parent, QSettings *settings)
    : QObject(parent)
    , m_httpServer(new HttpServer(this))
    , m_nam(new QNetworkAccessManager(this))
    , m_discovery(new DiscoveryManager(m_nam, this))
    , m_receiveServer(new ReceiveServer(this, m_discovery, this))
    , m_sendManager(new SendManager(this, m_nam, this))
    , m_settings(settings ? settings : new QSettings("LocalSend", "LocalSend-BB10"))
    , m_ownsSettings(settings == 0)
{
    initSettings();

    // Route remote cancellation from ReceiveServer to SendManager
    connect(m_receiveServer, SIGNAL(remoteCancelRequested(QString,QHostAddress)),
            m_sendManager, SLOT(handleRemoteCancel(QString,QHostAddress)));
}

Application::~Application()
{
    stop();
    if (m_ownsSettings) {
        delete m_settings;
        m_settings = 0;
    }
}

QString Application::identityStorageDirectory() const
{
    if (!m_identityStorageDir.trimmed().isEmpty()) {
        return m_identityStorageDir;
    }
    if (m_settings) {
        QString customDir = m_settings->value("security/identityDir").toString().trimmed();
        if (!customDir.isEmpty()) {
            return customDir;
        }
        QFileInfo fi(m_settings->fileName());
        QString baseDir = fi.absolutePath();
        if (!baseDir.isEmpty()) {
            return baseDir + "/localsend-identity";
        }
    }
    return "data/identity";
}

void Application::setIdentityStorageDirectory(const QString &path)
{
    if (path.trimmed().isEmpty() || m_identityStorageDir == path) {
        return;
    }
    m_identityStorageDir = path;
    if (m_settings) {
        m_settings->setValue("security/identityDir", path);
        m_settings->sync();
    }
    m_identityInitError.clear();
    if (LocalIdentity::loadOrCreate(m_identityStorageDir, &m_identity, &m_identityInitError)) {
        m_localInfo.fingerprint = m_identity.fingerprint();
        if (m_settings) {
            m_settings->setValue("device/fingerprint", m_localInfo.fingerprint);
        m_discovery->setLocalIdentity(m_identity);
            m_settings->sync();
        }
        m_discovery->updateLocalInfo(m_localInfo);
        m_discovery->setLocalIdentity(m_identity);
        emit localInfoChanged(m_localInfo);
    }
}

void Application::initSettings()
{
    // 1. Device identity (persisted EC P-256 certificate & key + SHA-256 DER fingerprint)
    m_identityInitError.clear();
    QString idDir = identityStorageDirectory();
    if (LocalIdentity::loadOrCreate(idDir, &m_identity, &m_identityInitError)) {
        m_localInfo.fingerprint = m_identity.fingerprint();
        m_settings->setValue("device/fingerprint", m_localInfo.fingerprint);
        m_discovery->setLocalIdentity(m_identity);
    } else {
        QString fp = m_settings->value("device/fingerprint").toString();
        m_localInfo.fingerprint = fp;
    }

    // 2. Device alias
    QString alias = m_settings->value("device/alias").toString();
    if (alias.trimmed().isEmpty()) {
        alias = "BlackBerry Passport";
        m_settings->setValue("device/alias", alias);
    }
    m_localInfo.alias = alias;

    // 3. Port & network
    int portVal = m_settings->value("network/port", 53317).toInt();
    if (portVal < 1 || portVal > 65535) {
        portVal = 53317;
    }
    m_localInfo.port = static_cast<quint16>(portVal);
    m_localInfo.version = "2.2";
    m_localInfo.protocol = "http";
    m_localInfo.deviceModel = "BlackBerry Passport";
    m_localInfo.deviceType = "mobile";
    m_localInfo.download = false;

    // 4. Storage directory
#if defined(Q_OS_BLACKBERRY) || defined(__QNX__) || defined(BB_CASCADES)
    QString defaultRxDir = "/accounts/1000/shared/downloads/LocalSend";
#else
    QString defaultRxDir = "data/received";
#endif

    QString rxDir = m_settings->value("storage/receiveDirectory").toString();
    if (rxDir.trimmed().isEmpty() || rxDir == "data/received") {
        rxDir = defaultRxDir;
        m_settings->setValue("storage/receiveDirectory", rxDir);
    }
    m_receiveDirectory = rxDir;
    m_settings->sync();
}

bool Application::start(QString *error)
{
    stop();

    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        if (error) {
            *error = QString("Configuration storage error (status %1)").arg(m_settings->status());
        }
        return false;
    }

    if (!m_identityInitError.isEmpty() || !m_identity.isValid()) {
        if (!LocalIdentity::loadOrCreate(identityStorageDirectory(), &m_identity, &m_identityInitError)) {
            if (error) {
                *error = QString("Local TLS identity error: %1").arg(m_identityInitError);
            }
            return false;
        }
        m_localInfo.fingerprint = m_identity.fingerprint();
        m_settings->setValue("device/fingerprint", m_localInfo.fingerprint);
        m_discovery->setLocalIdentity(m_identity);
        m_settings->sync();
    }

    // Ensure receive directory exists
    QDir dir(m_receiveDirectory);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            QString fallbackDir = "data/received";
            QDir fallback(fallbackDir);
            if (fallback.mkpath(".")) {
                m_receiveDirectory = fallbackDir;
            } else {
                if (error) {
                    *error = QString("Failed to create receive directory: %1 (fallback %2 also failed)")
                                 .arg(m_receiveDirectory).arg(fallbackDir);
                }
                return false;
            }
        }
    }

    // Attach receive server to HTTP server
    m_receiveServer->attach(m_httpServer);

    // Start HTTP server on configured port
    if (!m_httpServer->listen(QHostAddress::Any, m_localInfo.port, error)) {
        return false;
    }

    if (m_localInfo.port == 0) {
        m_localInfo.port = m_httpServer->serverPort();
    }

    // Start discovery manager
    m_discovery->start(m_localInfo);

    return true;
}

void Application::stop()
{
    if (m_discovery) {
        m_discovery->stop();
    }
    if (m_receiveServer) {
        m_receiveServer->onNetworkLost();
    }
    if (m_sendManager) {
        m_sendManager->onNetworkLost();
    }
    if (m_httpServer) {
        m_httpServer->stop();
    }
    if (!m_activeTransferId.isEmpty()) {
        m_activeTransferId.clear();
        emit transferSlotChanged(false, QString());
    }
}

DeviceInfo Application::localInfo() const
{
    return m_localInfo;
}

void Application::setAlias(const QString &alias)
{
    if (alias.trimmed().isEmpty() || m_localInfo.alias == alias) {
        return;
    }
    m_localInfo.alias = alias;
    m_settings->setValue("device/alias", alias);
    m_settings->sync();
    m_discovery->updateLocalInfo(m_localInfo);
    emit localInfoChanged(m_localInfo);
}

bool Application::setPort(quint16 port, QString *error)
{
    if (port != 0 && m_localInfo.port == port) {
        return true;
    }
    if (isTransferActive()) {
        if (error) *error = "Cannot change port while transfer is active";
        return false;
    }

    if (m_httpServer && m_httpServer->isListening()) {
        quint16 oldPort = m_localInfo.port;
        m_httpServer->stop();
        if (!m_httpServer->listen(QHostAddress::Any, port, error)) {
            // Rebind failed, rollback to previous port
            m_httpServer->listen(QHostAddress::Any, oldPort, 0);
            return false;
        }
        m_localInfo.port = m_httpServer->serverPort();
    } else {
        m_localInfo.port = port;
    }

    if (m_localInfo.port != 0) {
        m_settings->setValue("network/port", m_localInfo.port);
        m_settings->sync();
    }
    m_discovery->updateLocalInfo(m_localInfo);
    emit localInfoChanged(m_localInfo);
    return true;
}

void Application::setReceiveDirectory(const QString &path)
{
    if (path.trimmed().isEmpty() || m_receiveDirectory == path) {
        return;
    }
    m_receiveDirectory = path;
    m_settings->setValue("storage/receiveDirectory", path);
    m_settings->sync();
}

bool Application::beginTransfer(const QString &requestId)
{
    Q_ASSERT_X(thread() == QThread::currentThread(), "Application::beginTransfer", "Must be called from owning thread");
    if (requestId.trimmed().isEmpty()) {
        return false;
    }
    if (!m_activeTransferId.isEmpty()) {
        return false; // Transfer slot is already occupied
    }

    m_activeTransferId = requestId;
    emit transferSlotChanged(true, requestId);
    return true;
}

void Application::endTransfer(const QString &requestId)
{
    Q_ASSERT_X(thread() == QThread::currentThread(), "Application::endTransfer", "Must be called from owning thread");
    // Only the matching owner can release the slot
    if (!m_activeTransferId.isEmpty() && m_activeTransferId == requestId) {
        m_activeTransferId.clear();
        emit transferSlotChanged(false, QString());
    }
}

} // namespace localsend
