#ifndef APPLICATION_H
#define APPLICATION_H

#include <QObject>
#include <QString>
#include <QDir>
#include <QSettings>
#include <QNetworkAccessManager>

#include "protocol/ProtocolTypes.h"
#include "receive/HttpServer.h"
#include "security/LocalIdentity.h"

class DiscoveryManager;
class ReceiveServer;
class SendManager;

namespace localsend {

// Localsend Application follows Qt's single-threaded event-loop concurrency model.
// All public methods and slots must be invoked from the thread affined with Application (the thread() of Application).
class Application : public QObject {
    Q_OBJECT

public:
    explicit Application(QObject *parent = 0, QSettings *settings = 0);
    virtual ~Application();

    bool start(QString *error = 0);
    void stop();

    DeviceInfo localInfo() const;
    void setAlias(const QString &alias);
    bool setPort(quint16 port, QString *error = 0);
    bool setProtocol(const QString &protocol, QString *error = 0, bool persist = true);
    QString protocol() const { return m_localInfo.protocol; }
    void setReceiveDirectory(const QString &path);
    QString receiveDirectory() const { return m_receiveDirectory; }

    void setIdentityStorageDirectory(const QString &path);
    QString identityStorageDirectory() const;
    const LocalIdentity &localIdentity() const { return m_identity; }
    bool hasValidIdentity() const { return m_identity.isValid(); }

    // Shared single active transfer slot management
    bool beginTransfer(const QString &requestId);
    void endTransfer(const QString &requestId);
    bool isTransferActive() const { return !m_activeTransferId.isEmpty(); }
    QString activeTransferId() const { return m_activeTransferId; }

    HttpServer *httpServer() const { return m_httpServer; }
    QNetworkAccessManager *networkAccessManager() const { return m_nam; }
    DiscoveryManager *discoveryManager() const { return m_discovery; }
    ReceiveServer *receiveServer() const { return m_receiveServer; }
    SendManager *sendManager() const { return m_sendManager; }
    QSettings *settings() const { return m_settings; }

signals:
    void localInfoChanged(const DeviceInfo &info);
    void transferSlotChanged(bool busy, const QString &activeRequestId);

private:
    void initSettings();

    DeviceInfo m_localInfo;
    LocalIdentity m_identity;
    QString m_identityStorageDir;
    QString m_identityInitError;
    QString m_receiveDirectory;
    QString m_activeTransferId;

    HttpServer *m_httpServer;
    QNetworkAccessManager *m_nam;
    DiscoveryManager *m_discovery;
    ReceiveServer *m_receiveServer;
    SendManager *m_sendManager;
    QSettings *m_settings;
    bool m_ownsSettings;
};

} // namespace localsend

typedef localsend::Application AppController;

#endif // APPLICATION_H
