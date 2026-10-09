#include "discovery/DiscoveryManager.h"
#include "protocol/ProtocolJson.h"

#include <QNetworkRequest>
#include <QTimer>
#include <QSet>

DiscoveryManager::DiscoveryManager(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
    , m_udpSocket(new QUdpSocket(this))
    , m_announceTimer(new QTimer(this))
    , m_scanPumpTimer(new QTimer(this))
    , m_multicastAddress(QHostAddress("224.0.0.167"))
    , m_isScanning(false)
{
    connect(m_udpSocket, SIGNAL(readyRead()), this, SLOT(onUdpReadyRead()));
    connect(m_announceTimer, SIGNAL(timeout()), this, SLOT(onAnnounceTimeout()));
    connect(m_scanPumpTimer, SIGNAL(timeout()), this, SLOT(pumpScanQueue()));
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

    // Join LocalSend multicast group on all active network interfaces
    QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (int i = 0; i < ifaces.size(); ++i) {
        const QNetworkInterface &iface = ifaces.at(i);
        if ((iface.flags() & QNetworkInterface::IsUp) && (iface.flags() & QNetworkInterface::CanMulticast)) {
            m_udpSocket->joinMulticastGroup(m_multicastAddress, iface);
        }
    }
    m_udpSocket->joinMulticastGroup(m_multicastAddress);

    // Start periodic 30s announcement heartbeat to handle network drops and late-joining peers
    if (m_announceTimer) {
        m_announceTimer->start(30000);
    }

    // Broadcast initial presence announcement
    sendUdpAnnouncement(true);

    // Trigger initial subnet HTTP scan (Section 3.2 Legacy/Subnet Scan)
    startSubnetScan();
    return true;
}

void DiscoveryManager::stop()
{
    cancelSubnetScan();

    if (m_announceTimer) {
        m_announceTimer->stop();
    }

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
    // 1. Prune peers not seen within the last 120 seconds
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-120);
    QList<QString> staleFp;
    for (QMap<QString, PeerDevice>::const_iterator it = m_peers.begin(); it != m_peers.end(); ++it) {
        if (it.value().lastSeen < cutoff) {
            staleFp.append(it.key());
        }
    }
    for (int i = 0; i < staleFp.size(); ++i) {
        m_peers.remove(staleFp.at(i));
        emit peerRemoved(staleFp.at(i));
    }

    // 2. Re-join multicast groups on active interfaces
    QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (int i = 0; i < ifaces.size(); ++i) {
        const QNetworkInterface &iface = ifaces.at(i);
        if ((iface.flags() & QNetworkInterface::IsUp) && (iface.flags() & QNetworkInterface::CanMulticast)) {
            m_udpSocket->joinMulticastGroup(m_multicastAddress, iface);
        }
    }
    m_udpSocket->joinMulticastGroup(m_multicastAddress);

    // 3. Send full-channel discovery announcements (multicast + broadcast + subnets)
    sendUdpAnnouncement(true);

    // 4. Reset 30s periodic announcement heartbeat
    if (m_announceTimer && m_announceTimer->isActive()) {
        m_announceTimer->start(30000);
    }

    // 5. Active probe: HTTP register check to all known peers
    QList<PeerDevice> currentPeers = m_peers.values();
    for (int i = 0; i < currentPeers.size(); ++i) {
        const PeerDevice &peer = currentPeers.at(i);
        if (peer.info.protocol.toLower() == "http") {
            sendHttpRegister(peer.address, peer.info.port);
        }
    }

    // 6. Subnet HTTP active sweep across local /24 network (Section 3.2 Subnet Scan)
    startSubnetScan();
}

void DiscoveryManager::onAnnounceTimeout()
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
        // 1. Send to standard LocalSend multicast address (224.0.0.167:53317)
        m_udpSocket->writeDatagram(datagram, m_multicastAddress, DEFAULT_PORT);

        // 2. Send to standard LocalSend global broadcast address (255.255.255.255:53317)
        m_udpSocket->writeDatagram(datagram, QHostAddress::Broadcast, DEFAULT_PORT);

        // 3. Send to local subnet directed broadcast on active physical LAN interfaces
        // Fallback: calculate broadcast from IP & netmask if OS report is null
        QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
        for (int i = 0; i < ifaces.size(); ++i) {
            const QNetworkInterface &iface = ifaces.at(i);
            if (!(iface.flags() & QNetworkInterface::IsUp) ||
                (iface.flags() & QNetworkInterface::IsLoopBack) ||
                (iface.flags() & QNetworkInterface::IsPointToPoint)) {
                continue;
            }
            QList<QNetworkAddressEntry> entries = iface.addressEntries();
            for (int j = 0; j < entries.size(); ++j) {
                const QNetworkAddressEntry &entry = entries.at(j);
                if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                    continue;
                }
                QHostAddress bcast = entry.broadcast();
                if (bcast.isNull() && !entry.netmask().isNull()) {
                    quint32 ipVal = entry.ip().toIPv4Address();
                    quint32 maskVal = entry.netmask().toIPv4Address();
                    if (maskVal != 0) {
                        bcast = QHostAddress(ipVal | (~maskVal));
                    }
                }
                if (!bcast.isNull() && bcast != QHostAddress::Broadcast) {
                    m_udpSocket->writeDatagram(datagram, bcast, DEFAULT_PORT);
                }
            }
        }
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

        // If this peer was previously recorded, remove it as it is unreachable
        for (QMap<QString, PeerDevice>::iterator it = m_peers.begin(); it != m_peers.end(); ++it) {
            if (it.value().address == target.address && it.value().info.port == target.info.port) {
                QString fp = it.key();
                m_peers.erase(it);
                emit peerRemoved(fp);
                break;
            }
        }

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
            if (peer.info.fingerprint == m_localInfo.fingerprint) {
                reply->deleteLater();
                return;
            }
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

    QHostAddress cleanAddress = peerAddress;
    if (cleanAddress.protocol() == QAbstractSocket::IPv6Protocol) {
        quint32 ipv4 = cleanAddress.toIPv4Address();
        if (ipv4 != 0) {
            cleanAddress = QHostAddress(ipv4);
        }
    }

    PeerDevice peer;
    peer.info = info;
    peer.address = cleanAddress;
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

QString DiscoveryManager::activeInterfaceAddress() const
{
    QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (int i = 0; i < ifaces.size(); ++i) {
        const QNetworkInterface &iface = ifaces.at(i);
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack) ||
            (iface.flags() & QNetworkInterface::IsPointToPoint)) {
            continue;
        }
        QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (int j = 0; j < entries.size(); ++j) {
            const QNetworkAddressEntry &e = entries.at(j);
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && e.ip() != QHostAddress::LocalHost) {
                return e.ip().toString();
            }
        }
    }
    return QString();
}

QString DiscoveryManager::activeInterfaceSubnet() const
{
    QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (int i = 0; i < ifaces.size(); ++i) {
        const QNetworkInterface &iface = ifaces.at(i);
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack) ||
            (iface.flags() & QNetworkInterface::IsPointToPoint)) {
            continue;
        }
        QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (int j = 0; j < entries.size(); ++j) {
            const QNetworkAddressEntry &e = entries.at(j);
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && e.ip() != QHostAddress::LocalHost) {
                quint32 ipVal = e.ip().toIPv4Address();
                quint32 maskVal = e.netmask().toIPv4Address();
                if (maskVal == 0) maskVal = 0xFFFFFF00;
                QHostAddress netAddr(ipVal & maskVal);
                return QString("%1/24").arg(netAddr.toString());
            }
        }
    }
    return QString();
}

void DiscoveryManager::startSubnetScan()
{
    if (m_isScanning) {
        return;
    }

    cancelSubnetScan();

    QHostAddress localIp;
    QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (int i = 0; i < ifaces.size(); ++i) {
        const QNetworkInterface &iface = ifaces.at(i);
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack) ||
            (iface.flags() & QNetworkInterface::IsPointToPoint)) {
            continue;
        }
        QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (int j = 0; j < entries.size(); ++j) {
            const QNetworkAddressEntry &e = entries.at(j);
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && e.ip() != QHostAddress::LocalHost) {
                localIp = e.ip();
                break;
            }
        }
        if (!localIp.isNull()) {
            break;
        }
    }

    if (localIp.isNull()) {
        return;
    }

    quint32 localIpVal = localIp.toIPv4Address();
    quint32 baseIpVal = localIpVal & 0xFFFFFF00;
    int localHostNum = static_cast<int>(localIpVal & 0xFF);

    QList<int> hostOrder;
    QSet<int> seen;

    // 1. Prioritize immediate neighbors (localHostNum ± 1 to 20)
    for (int delta = 1; delta <= 20; ++delta) {
        int left = localHostNum - delta;
        int right = localHostNum + delta;
        if (left >= 1 && left <= 254 && !seen.contains(left)) {
            hostOrder.append(left);
            seen.insert(left);
        }
        if (right >= 1 && right <= 254 && !seen.contains(right)) {
            hostOrder.append(right);
            seen.insert(right);
        }
    }

    // 2. Common device host numbers
    static const int commonHosts[] = {1, 2, 3, 4, 10, 100, 101, 102, 103, 104, 105, 200, 254};
    for (size_t k = 0; k < sizeof(commonHosts)/sizeof(commonHosts[0]); ++k) {
        int h = commonHosts[k];
        if (h != localHostNum && !seen.contains(h) && h >= 1 && h <= 254) {
            hostOrder.append(h);
            seen.insert(h);
        }
    }

    // 3. Fill in remaining /24 subnet addresses (1..254)
    for (int h = 1; h <= 254; ++h) {
        if (h != localHostNum && !seen.contains(h)) {
            hostOrder.append(h);
            seen.insert(h);
        }
    }

    QSet<quint32> allLocalIps;
    for (int i = 0; i < ifaces.size(); ++i) {
        QList<QNetworkAddressEntry> entries = ifaces.at(i).addressEntries();
        for (int j = 0; j < entries.size(); ++j) {
            if (entries.at(j).ip().protocol() == QAbstractSocket::IPv4Protocol) {
                allLocalIps.insert(entries.at(j).ip().toIPv4Address());
            }
        }
    }

    m_scanQueue.clear();
    for (int i = 0; i < hostOrder.size(); ++i) {
        quint32 targetIpVal = baseIpVal | static_cast<quint32>(hostOrder.at(i));
        if (allLocalIps.contains(targetIpVal)) {
            continue;
        }
        m_scanQueue.append(QHostAddress(targetIpVal));
    }

    m_isScanning = true;
    emit scanningChanged(true);

    pumpScanQueue();
}

void DiscoveryManager::cancelSubnetScan()
{
    m_scanQueue.clear();

    QList<QNetworkReply*> replies = m_activeProbes.keys();
    m_activeProbes.clear();
    for (int i = 0; i < replies.size(); ++i) {
        QNetworkReply *reply = replies.at(i);
        if (reply) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }

    if (m_isScanning) {
        m_isScanning = false;
        emit scanningChanged(false);
    }
}

void DiscoveryManager::pumpScanQueue()
{
    if (!m_nam) {
        return;
    }

    const int MAX_CONCURRENT_PROBES = 12;

    while (m_activeProbes.size() < MAX_CONCURRENT_PROBES && !m_scanQueue.isEmpty()) {
        QHostAddress targetAddr = m_scanQueue.takeFirst();

        // Check if we already have an active peer with this address (skip if known)
        bool alreadyKnown = false;
        for (QMap<QString, PeerDevice>::const_iterator it = m_peers.begin(); it != m_peers.end(); ++it) {
            if (it.value().address == targetAddr) {
                alreadyKnown = true;
                break;
            }
        }
        if (alreadyKnown) {
            continue;
        }

        QUrl url;
        url.setScheme("http");
        url.setHost(targetAddr.toString());
        url.setPort(DEFAULT_PORT);
        url.setPath("/api/localsend/v2/register");

        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

        QString err;
        QByteArray body = ProtocolJson::encodeRegisterRequest(m_localInfo, &err);
        if (body.isEmpty()) {
            continue;
        }

        QNetworkReply *reply = m_nam->post(req, body);
        if (reply) {
            m_activeProbes.insert(reply, targetAddr);
            connect(reply, SIGNAL(finished()), this, SLOT(onProbeReplyFinished()));

            // 1500 ms deadline for subnet scan probe
            QTimer *timer = new QTimer(reply);
            timer->setSingleShot(true);
            connect(timer, SIGNAL(timeout()), reply, SLOT(abort()));
            timer->start(1500);
        }
    }

    if (m_scanQueue.isEmpty() && m_activeProbes.isEmpty()) {
        if (m_isScanning) {
            m_isScanning = false;
            emit scanningChanged(false);
        }
    }
}

void DiscoveryManager::onProbeReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }

    if (!m_activeProbes.contains(reply)) {
        reply->deleteLater();
        return;
    }

    QHostAddress targetAddr = m_activeProbes.take(reply);

    if (reply->error() == QNetworkReply::NoError) {
        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode == 200) {
            QByteArray respData = reply->readAll();
            PeerDevice target;
            target.address = targetAddr;
            target.info.port = DEFAULT_PORT;
            target.info.protocol = "http";

            PeerDevice peer;
            QString err;
            if (ProtocolJson::decodeRegisterResponse(respData, target, &peer, &err)) {
                if (peer.info.fingerprint == m_localInfo.fingerprint) {
                    reply->deleteLater();
                    pumpScanQueue();
                    return;
                }
                m_peers.insert(peer.info.fingerprint, peer);
                emit peerUpdated(peer);
            }
        }
    }

    reply->deleteLater();

    pumpScanQueue();
}
