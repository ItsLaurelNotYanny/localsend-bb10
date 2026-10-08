#include "discovery/DiscoveryManager.h"
#include "protocol/ProtocolJson.h"

#include <QNetworkRequest>
#include <QTimer>

DiscoveryManager::DiscoveryManager(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
    , m_udpSocket(new QUdpSocket(this))
    , m_multicastAddress(QHostAddress("224.0.0.167"))
{
    connect(m_udpSocket, SIGNAL(readyRead()), this, SLOT(onUdpReadyRead()));
}

DiscoveryManager::~DiscoveryManager()
{
    stop();
}

bool DiscoveryManager::start(const DeviceInfo &localInfo, const QNetworkInterface &iface, QString *error)
{
    stop();

    m_localInfo = localInfo;
    m_interface = iface;

    // Bind UDP socket to port 53317 with address sharing (Qt4/Qt5 dual baseline)
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    bool bound = m_udpSocket->bind(QHostAddress::AnyIPv4, DEFAULT_PORT,
                                   QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
#else
    bool bound = m_udpSocket->bind(QHostAddress::Any, DEFAULT_PORT,
                                   QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
#endif
    if (!bound) {
        bound = m_udpSocket->bind(DEFAULT_PORT,
                                  QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    }

    if (!bound) {
        if (error) {
            *error = m_udpSocket->errorString();
        }
        return false;
    }

    // Join LocalSend multicast group
    if (m_interface.isValid()) {
        m_udpSocket->joinMulticastGroup(m_multicastAddress, m_interface);
    } else {
        m_udpSocket->joinMulticastGroup(m_multicastAddress);
    }

    // Broadcast initial presence announcement
    sendUdpAnnouncement(true);
    return true;
}

void DiscoveryManager::stop()
{
    if (m_udpSocket && m_udpSocket->isOpen()) {
        m_udpSocket->close();
    }

    // Safely abort pending register requests without iterator invalidation (R009)
    QList<QNetworkReply*> pending = m_pendingRegisters.keys();
    m_pendingRegisters.clear();
    for (int i = 0; i < pending.size(); ++i) {
        QNetworkReply *reply = pending.at(i);
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }
}

void DiscoveryManager::refresh()
{
    sendUdpAnnouncement(true);
}

void DiscoveryManager::updateLocalInfo(const DeviceInfo &info)
{
    m_localInfo = info;
}

void DiscoveryManager::sendUdpAnnouncement(bool announce)
{
    if (!m_udpSocket || !m_udpSocket->isOpen()) {
        return;
    }

    QString err;
    QByteArray datagram = ProtocolJson::encodeAnnouncement(m_localInfo, announce, &err);
    if (!datagram.isEmpty()) {
        m_udpSocket->writeDatagram(datagram, m_multicastAddress, DEFAULT_PORT);
    }
}

void DiscoveryManager::onUdpReadyRead()
{
    while (m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpSocket->pendingDatagramSize());

        QHostAddress senderAddress;
        quint16 senderPort = 0;
        m_udpSocket->readDatagram(datagram.data(), datagram.size(), &senderAddress, &senderPort);

        if (datagram.size() > ProtocolJson::MAX_JSON_SIZE) {
            continue; // Ignore oversized datagram
        }

        DeviceInfo info;
        bool announce = false;
        QString err;
        if (!ProtocolJson::decodeAnnouncement(datagram, &info, &announce, &err)) {
            continue;
        }

        // Ignore self-announcements
        if (info.fingerprint == m_localInfo.fingerprint) {
            continue;
        }

        // Normalize IPv4-mapped IPv6 address (Qt 4.8 compatible)
        if (senderAddress.protocol() == QAbstractSocket::IPv6Protocol) {
            quint32 ipv4 = senderAddress.toIPv4Address();
            if (ipv4 != 0) {
                senderAddress = QHostAddress(ipv4);
            }
        }

        PeerDevice peer;
        peer.info = info;
        peer.address = senderAddress;
        peer.lastSeen = QDateTime::currentDateTime();

        m_peers.insert(info.fingerprint, peer);
        emit peerUpdated(peer);

        // Respond ONLY IF peer announced (announce: true)
        if (announce) {
            if (info.protocol.toLower() == "http") {
                sendHttpRegister(senderAddress, info.port);
            } else {
                // Peer is HTTPS: do not send plaintext HTTP to TLS port; respond via UDP multicast
                sendUdpAnnouncement(false);
            }
        }
    }
}

void DiscoveryManager::registerPeer(const QHostAddress &address, quint16 port)
{
    sendHttpRegister(address, port);
}

void DiscoveryManager::sendHttpRegister(const QHostAddress &address, quint16 port)
{
    if (!m_nam) {
        return;
    }

    // Prevent duplicate pending registers to the same endpoint (R011)
    for (QMap<QNetworkReply*, PeerDevice>::const_iterator it = m_pendingRegisters.begin();
         it != m_pendingRegisters.end(); ++it) {
        if (it.value().address == address && it.value().info.port == port) {
            return;
        }
    }

    QUrl url;
    url.setScheme("http");
    url.setHost(address.toString());
    url.setPort(port);
    url.setPath("/api/localsend/v2/register");

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QString err;
    QByteArray body = ProtocolJson::encodeRegisterRequest(m_localInfo, &err);
    if (body.isEmpty()) {
        return;
    }

    QNetworkReply *reply = m_nam->post(req, body);
    if (reply) {
        PeerDevice target;
        target.address = address;
        target.info.port = port;
        target.info.protocol = "http";

        m_pendingRegisters.insert(reply, target);
        connect(reply, SIGNAL(finished()), this, SLOT(onRegisterReplyFinished()));

        // 3-second register request deadline (R011)
        QTimer *timer = new QTimer(reply);
        timer->setSingleShot(true);
        connect(timer, SIGNAL(timeout()), reply, SLOT(abort()));
        timer->start(3000);
    }
}

void DiscoveryManager::onRegisterReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }

    if (!m_pendingRegisters.contains(reply)) {
        reply->deleteLater();
        return;
    }

    PeerDevice target = m_pendingRegisters.take(reply);

    if (reply->error() != QNetworkReply::NoError) {
        emit discoveryError(reply->errorString());
        // Fallback to multicast response with announce:false
        sendUdpAnnouncement(false);
        reply->deleteLater();
        return;
    }

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 200) {
        QByteArray respData = reply->readAll();
        PeerDevice peer;
        QString err;
        if (ProtocolJson::decodeRegisterResponse(respData, target, &peer, &err)) {
            m_peers.insert(peer.info.fingerprint, peer);
            emit peerUpdated(peer);
        } else {
            emit discoveryError(err);
        }
    } else {
        emit discoveryError(QString("HTTP register failed with status %1").arg(statusCode));
        sendUdpAnnouncement(false);
    }

    reply->deleteLater();
}

void DiscoveryManager::handleIncomingRegister(const DeviceInfo &info, const QHostAddress &peerAddress)
{
    if (info.fingerprint == m_localInfo.fingerprint) {
        return;
    }

    PeerDevice peer;
    peer.info = info;
    peer.address = peerAddress;
    peer.lastSeen = QDateTime::currentDateTime();

    m_peers.insert(info.fingerprint, peer);
    emit peerUpdated(peer);
}

QList<PeerDevice> DiscoveryManager::peers() const
{
    return m_peers.values();
}

PeerDevice DiscoveryManager::findPeer(const QString &fingerprint) const
{
    return m_peers.value(fingerprint);
}
