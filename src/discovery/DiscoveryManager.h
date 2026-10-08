#ifndef DISCOVERY_MANAGER_H
#define DISCOVERY_MANAGER_H

#include <QObject>
#include <QUdpSocket>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QMap>
#include <QList>

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

signals:
    void peerUpdated(const PeerDevice &peer);
    void peerRemoved(const QString &fingerprint);
    void discoveryError(const QString &error);

private slots:
    void onUdpReadyRead();
    void onRegisterReplyFinished();

private:
    void sendUdpAnnouncement(bool announce);
    void sendHttpRegister(const QHostAddress &address, quint16 port);

    QNetworkAccessManager *m_nam;
    QUdpSocket *m_udpSocket;
    DeviceInfo m_localInfo;
    QNetworkInterface m_interface;
    QHostAddress m_multicastAddress;

    QMap<QString, PeerDevice> m_peers; // Keyed by fingerprint
    QMap<QNetworkReply*, PeerDevice> m_pendingRegisters;
};

#endif // DISCOVERY_MANAGER_H
