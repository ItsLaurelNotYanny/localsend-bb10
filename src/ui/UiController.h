#ifndef UICONTROLLER_H
#define UICONTROLLER_H

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QFileInfo>
#include <QPointer>

#include "app/Application.h"
#include "discovery/DiscoveryManager.h"
#include "receive/ReceiveServer.h"
#include "send/SendManager.h"

namespace localsend {

class UiController : public QObject {
    Q_OBJECT

    // Local device properties
    Q_PROPERTY(QString deviceAlias READ deviceAlias WRITE setDeviceAlias NOTIFY deviceAliasChanged)
    Q_PROPERTY(int devicePort READ devicePort NOTIFY devicePortChanged)
    Q_PROPERTY(QString receiveDirectory READ receiveDirectory NOTIFY receiveDirectoryChanged)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    Q_PROPERTY(QString certificateFingerprint READ certificateFingerprint NOTIFY certificateFingerprintChanged)
    Q_PROPERTY(bool isHttps READ isHttps NOTIFY isHttpsChanged)
    Q_PROPERTY(QString serverProtocol READ serverProtocol NOTIFY serverProtocolChanged)

    // Peer list property
    Q_PROPERTY(QVariantList peers READ peers NOTIFY peersChanged)
    Q_PROPERTY(int peerCount READ peerCount NOTIFY peersChanged)
    Q_PROPERTY(int peersRevision READ peersRevision NOTIFY peersChanged)
    Q_PROPERTY(bool isScanning READ isScanning NOTIFY isScanningChanged)
    Q_PROPERTY(QString networkStatus READ networkStatus NOTIFY networkStatusChanged)

    // Selected file for sending
    Q_PROPERTY(QString selectedFilePath READ selectedFilePath NOTIFY selectedFileChanged)
    Q_PROPERTY(QString selectedFileName READ selectedFileName NOTIFY selectedFileChanged)
    Q_PROPERTY(QString selectedFileSize READ selectedFileSize NOTIFY selectedFileChanged)
    Q_PROPERTY(bool hasSelectedFile READ hasSelectedFile NOTIFY selectedFileChanged)

    // Active transfer status
    Q_PROPERTY(bool transferActive READ transferActive NOTIFY transferStateChanged)
    Q_PROPERTY(QString transferState READ transferState NOTIFY transferStateChanged)
    Q_PROPERTY(int transferProgress READ transferProgress NOTIFY transferProgressChanged)
    Q_PROPERTY(QString transferStatusText READ transferStatusText NOTIFY transferStatusTextChanged)
    Q_PROPERTY(bool sha256Verified READ sha256Verified NOTIFY transferStateChanged)
    Q_PROPERTY(QString lastTransferSha256 READ lastTransferSha256 NOTIFY transferStateChanged)

    // Incoming offer prompt
    Q_PROPERTY(bool hasIncomingOffer READ hasIncomingOffer NOTIFY incomingOfferChanged)
    Q_PROPERTY(QString incomingPeerAlias READ incomingPeerAlias NOTIFY incomingOfferChanged)
    Q_PROPERTY(QString incomingFileName READ incomingFileName NOTIFY incomingOfferChanged)
    Q_PROPERTY(QString incomingFileSize READ incomingFileSize NOTIFY incomingOfferChanged)
    Q_PROPERTY(QString incomingRequestId READ incomingRequestId NOTIFY incomingOfferChanged)
    Q_PROPERTY(QString incomingFileId READ incomingFileId NOTIFY incomingOfferChanged)
    Q_PROPERTY(QVariantList incomingFiles READ incomingFiles NOTIFY incomingOfferChanged)
    Q_PROPERTY(QString selectedIncomingFileId READ selectedIncomingFileId WRITE setSelectedIncomingFileId NOTIFY selectedIncomingFileIdChanged)

    // Saved path of received file
    Q_PROPERTY(QString lastSavedFilePath READ lastSavedFilePath NOTIFY lastSavedFilePathChanged)

    // Error and notifications
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString diagnosticLogPath READ diagnosticLogPath CONSTANT)

public:
    explicit UiController(Application *app, QObject *parent = 0);
    virtual ~UiController();

    // Getters
    QString deviceAlias() const;
    int devicePort() const;
    QString receiveDirectory() const;
    QString appVersion() const { return "v0.2.0 (Build 4)"; }
    QString certificateFingerprint() const;
    bool isHttps() const;
    QString serverProtocol() const;
    QVariantList peers() const { return m_peers; }
    int peerCount() const { return m_peers.size(); }
    int peersRevision() const { return m_peersRevision; }
    bool isScanning() const;
    QString networkStatus() const;

    QString selectedFilePath() const { return m_selectedFilePath; }
    QString selectedFileName() const { return m_selectedFileName; }
    QString selectedFileSize() const { return m_selectedFileSize; }
    bool hasSelectedFile() const { return !m_selectedFilePath.isEmpty(); }

    bool transferActive() const;
    QString transferState() const { return m_transferState; }
    int transferProgress() const { return m_transferProgress; }
    QString transferStatusText() const { return m_transferStatusText; }
    bool sha256Verified() const { return m_sha256Verified; }
    QString lastTransferSha256() const { return m_lastTransferSha256; }

    bool hasIncomingOffer() const { return m_hasIncomingOffer; }
    QString incomingPeerAlias() const { return m_incomingPeerAlias; }
    QString incomingFileName() const { return m_incomingFileName; }
    QString incomingFileSize() const { return m_incomingFileSize; }
    QString incomingRequestId() const { return m_incomingRequestId; }
    QString incomingFileId() const { return m_incomingFileId; }
    QVariantList incomingFiles() const { return m_incomingFiles; }
    QString selectedIncomingFileId() const { return m_selectedIncomingFileId; }
    void setSelectedIncomingFileId(const QString &fileId);

    QString lastSavedFilePath() const { return m_lastSavedFilePath; }
    QString lastError() const { return m_lastError; }

    // Q_INVOKABLE control methods
    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void addManualPeer(const QString &ipv4, int port);
    Q_INVOKABLE void chooseFile();
    Q_INVOKABLE void setSelectedFilePath(const QString &path);
    Q_INVOKABLE void clearSelectedFile();
    Q_INVOKABLE bool sendSelectedFile(const QString &fingerprint);
    Q_INVOKABLE void selectIncomingFile(const QString &fileId);
    Q_INVOKABLE void acceptIncoming(const QString &requestId, const QString &fileId = QString());
    Q_INVOKABLE void rejectIncoming(const QString &requestId);
    Q_INVOKABLE void cancelTransfer();
    Q_INVOKABLE void setDeviceAlias(const QString &alias);
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void openFile(const QString &filePath = QString());
    Q_INVOKABLE void openReceiveDirectory();
    Q_INVOKABLE QString diagnosticLogPath() const;
    Q_INVOKABLE void openDiagnosticLog();
    Q_INVOKABLE void setPeersContainer(QObject *containerObj);
    Q_INVOKABLE void populatePeers(QObject *containerObj = 0);

signals:
    void deviceAliasChanged();
    void devicePortChanged();
    void receiveDirectoryChanged();
    void peersChanged();
    void isScanningChanged();
    void networkStatusChanged();
    void selectedFileChanged();
    void transferStateChanged();
    void transferProgressChanged();
    void transferStatusTextChanged();
    void incomingOfferChanged();
    void selectedIncomingFileIdChanged();
    void lastSavedFilePathChanged();
    void lastErrorChanged();
    void certificateFingerprintChanged();
    void isHttpsChanged();
    void serverProtocolChanged();
    void transferFinished(const QString &message);

private slots:
    void onSendCardButtonClicked();

    // Application slots
    void onTransferSlotChanged(bool busy, const QString &activeRequestId);
    void onLocalInfoChanged(const DeviceInfo &info);

    // DiscoveryManager slots
    void onPeerUpdated(const PeerDevice &peer);
    void onPeerRemoved(const QString &fingerprint);
    void onDiscoveryError(const QString &error);
    void onScanningChanged(bool isScanning);

    // SendManager slots
    void onSendStateChanged(const QString &requestId, const QString &state);
    void onSendProgress(const QString &requestId, qint64 done, qint64 total);
    void onSendFinished(const QString &requestId);
    void onSendFailed(const QString &requestId, const QString &reason);

    // ReceiveServer slots
    void onIncomingOffer(const QString &requestId, const PeerDevice &peer, const QList<FileMeta> &files);
    void onReceiveProgress(const QString &sessionId, qint64 done, qint64 total);
    void onReceiveFinished(const QString &sessionId, const QString &savedPath);
    void onReceiveCancelled(const QString &sessionId, const QString &reason);
    void onReceiveOfferCancelled(const QString &requestId, const QString &reason);
    void onReceiveFailed(const QString &id, const QString &reason);

    // Cascades FilePicker completion (BB10 only)
    void onFilePicked(const QStringList &selectedFiles);
    void onFilePickerCanceled();

private:
    void updatePeersModel();
    static QString formatBytes(qint64 bytes);

    Application *m_app;
    QPointer<QObject> m_activeFilePicker;
    QPointer<QObject> m_peersContainer;

    QVariantList m_peers;
    int m_peersRevision;

    QString m_selectedFilePath;
    QString m_selectedFileName;
    QString m_selectedFileSize;

    QString m_transferState;
    int m_transferProgress;
    QString m_transferStatusText;
    bool m_sha256Verified;
    QString m_lastTransferSha256;

    bool m_hasIncomingOffer;
    QString m_incomingPeerAlias;
    QString m_incomingFileName;
    QString m_incomingFileSize;
    QString m_incomingRequestId;
    QString m_incomingFileId;
    QVariantList m_incomingFiles;
    QString m_selectedIncomingFileId;

    QString m_lastSavedFilePath;
    QString m_lastError;
};

} // namespace localsend

#endif // UICONTROLLER_H
