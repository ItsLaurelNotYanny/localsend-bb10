#ifndef DISCOVERY_MANAGER_H
#define DISCOVERY_MANAGER_H

#include <QObject>
#include <QUdpSocket>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QMap>
#include <QList>
#include <QTimer>

#include "protocol/ProtocolTypes.h"

class DiscoveryManager : public QObject {
    Q_OBJECT

public:
    static const quint16 DEFAULT_PORT = 53317;

    explicit DiscoveryManager(QNetworkAccessManager *nam, QObject *parent = 0);
    virtual ~DiscoveryManager();

    bool start(const DeviceInfo &localInfo, const QNetworkInterface &iface = QNetworkInterface(), QString *error = 0);
    void stop();

    void refresh();
    void registerPeer(const QHostAddress &address, quint16 port);
    void handleIncomingRegister(const DeviceInfo &info, const QHostAddress &peerAddress);

    QList<PeerDevice> peers() const;
    PeerDevice findPeer(const QString &fingerprint) const;

    void updateLocalInfo(const DeviceInfo &info);

    bool isScanning() const { return m_isScanning; }
    QString activeInterfaceAddress() const;
    QString activeInterfaceSubnet() const;

    QList<QHostAddress> historicalPeers() const { return m_historicalPeers; }
    void addHistoricalPeer(const QHostAddress &addr);
    QList<QHostAddress> udpSweepQueue() const { return m_udpSweepQueue; }

signals:
    void peerUpdated(const PeerDevice &peer);
    void peerRemoved(const QString &fingerprint);
    void discoveryError(const QString &error);
    void scanningChanged(bool isScanning);

private slots:
    void onUdpReadyRead();
    void onRegisterReplyFinished();
    void onAnnounceTimeout();
    void onProbeReplyFinished();
    void onHttpsProbeReplyFinished();
    void pumpScanQueue();
    void cancelSubnetScan();
    void pumpUdpSweep();

private:
    bool selectPrimaryInterface(QNetworkInterface *outIface, QNetworkAddressEntry *outEntry) const;
    void sendUdpAnnouncement(bool announce);
    void sendHttpRegister(const QHostAddress &address, quint16 port);
    void startSubnetScan();
    void probeHttpsHost(const QHostAddress &addr);
    void setSocketBuffers();
    QList<QHostAddress> loadArpCacheIps() const;

    QNetworkAccessManager *m_nam;
    QUdpSocket *m_udpSocket;
    QTimer *m_announceTimer;
    QTimer *m_scanPumpTimer;
    QTimer *m_scanDeadlineTimer;
    QTimer *m_udpSweepTimer;
    DeviceInfo m_localInfo;
    QNetworkInterface m_interface;
    QHostAddress m_multicastAddress;

    QMap<QString, PeerDevice> m_peers; // Keyed by fingerprint
    QMap<QNetworkReply*, PeerDevice> m_pendingRegisters; // Explicit manual register replies

    // Historical peers and prioritized targets
    QList<QHostAddress> m_historicalPeers;

    // Paced UDP sweep across subnet (Issue 2 QNX UDP buffer overrun protection)
    QList<QHostAddress> m_udpSweepQueue;

    // Subnet HTTP sweep (Section 3.2 Legacy / Subnet Scan)
    bool m_isScanning;
    QList<QHostAddress> m_scanQueue;
    QMap<QNetworkReply*, QHostAddress> m_activeProbes;
    QMap<QNetworkReply*, QHostAddress> m_activeHttpsProbes;
};

#endif // DISCOVERY_MANAGER_H
