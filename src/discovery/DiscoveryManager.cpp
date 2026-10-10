#include "discovery/DiscoveryManager.h"
#include "protocol/ProtocolJson.h"

#include <QNetworkRequest>
#include <QTimer>
#include <QSet>
#include <QDebug>

DiscoveryManager::DiscoveryManager(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
    , m_udpSocket(new QUdpSocket(this))
    , m_announceTimer(new QTimer(this))
    , m_scanPumpTimer(new QTimer(this))
    , m_scanDeadlineTimer(new QTimer(this))
    , m_multicastAddress(QHostAddress("224.0.0.167"))
    , m_isScanning(false)
{
    connect(m_udpSocket, SIGNAL(readyRead()), this, SLOT(onUdpReadyRead()));
    connect(m_announceTimer, SIGNAL(timeout()), this, SLOT(onAnnounceTimeout()));
    connect(m_scanPumpTimer, SIGNAL(timeout()), this, SLOT(pumpScanQueue()));
    m_scanDeadlineTimer->setSingleShot(true);
    connect(m_scanDeadlineTimer, SIGNAL(timeout()), this, SLOT(cancelSubnetScan()));
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

    // 5. Active probe: Direct unicast UDP announcement and HTTP register to known peers
    QString err;
    QByteArray dgram = ProtocolJson::encodeAnnouncement(m_localInfo, true, &err);
    QList<PeerDevice> currentPeers = m_peers.values();
    for (int i = 0; i < currentPeers.size(); ++i) {
        const PeerDevice &peer = currentPeers.at(i);
        if (!dgram.isEmpty() && m_udpSocket && m_udpSocket->isOpen()) {
            m_udpSocket->writeDatagram(dgram, peer.address, DEFAULT_PORT);
            if (peer.info.port != DEFAULT_PORT && peer.info.port > 0) {
                m_udpSocket->writeDatagram(dgram, peer.address, peer.info.port);
            }
        }
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
                quint32 ipVal = entry.ip().toIPv4Address();
                quint32 maskVal = entry.netmask().toIPv4Address();
                if (maskVal == 0) {
                    maskVal = 0xFFFFFF00;
                }
                if (bcast.isNull()) {
                    bcast = QHostAddress(ipVal | (~maskVal));
                }
                if (!bcast.isNull() && bcast != QHostAddress::Broadcast) {
                    m_udpSocket->writeDatagram(datagram, bcast, DEFAULT_PORT);
                }
            }
        }
        qDebug() << "[Discovery] UDP announcement sent (announce=" << announce << ")";
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
        qDebug() << "[Discovery] Peer updated via UDP:" << info.alias << "(" << info.deviceModel << ")"
                 << senderAddress.toString() << ":" << info.port << "protocol:" << info.protocol;
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
    QString uErr;
    QByteArray dgram = ProtocolJson::encodeAnnouncement(m_localInfo, true, &uErr);
    if (!dgram.isEmpty() && m_udpSocket && m_udpSocket->isOpen()) {
        m_udpSocket->writeDatagram(dgram, address, port > 0 ? port : DEFAULT_PORT);
    }
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
    qDebug() << "[Discovery] Peer updated via HTTP register:" << info.alias << "(" << info.deviceModel << ")"
             << cleanAddress.toString() << ":" << info.port << "protocol:" << info.protocol;
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

bool DiscoveryManager::selectPrimaryInterface(QNetworkInterface *outIface, QNetworkAddressEntry *outEntry) const
{
    if (m_interface.isValid() && (m_interface.flags() & QNetworkInterface::IsUp)) {
        QList<QNetworkAddressEntry> entries = m_interface.addressEntries();
        for (int i = 0; i < entries.size(); ++i) {
            const QNetworkAddressEntry &e = entries.at(i);
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && e.ip() != QHostAddress::LocalHost) {
                if (outIface) *outIface = m_interface;
                if (outEntry) *outEntry = e;
                return true;
            }
        }
    }

    QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    int bestScore = -100;
    QNetworkInterface bestIface;
    QNetworkAddressEntry bestEntry;

    for (int i = 0; i < ifaces.size(); ++i) {
        const QNetworkInterface &iface = ifaces.at(i);
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack) ||
            (iface.flags() & QNetworkInterface::IsPointToPoint)) {
            continue;
        }

        QString name = iface.name().toLower();
        int score = 10;
        if (name.contains("ti_wlan") || name.contains("wlan") || name.contains("wifi") || name.contains("wl")) {
            score = 100;
        } else if (name.contains("en") || name.contains("eth")) {
            score = 80;
        } else if (name.contains("docker") || name.contains("veth") || name.contains("virbr") ||
                   name.contains("tun") || name.contains("tap") || name.contains("usb") || name.contains("rndis")) {
            score = -10;
        }

        QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (int j = 0; j < entries.size(); ++j) {
            const QNetworkAddressEntry &e = entries.at(j);
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && e.ip() != QHostAddress::LocalHost) {
                if (score > bestScore) {
                    bestScore = score;
                    bestIface = iface;
                    bestEntry = e;
                }
            }
        }
    }

    if (bestScore > -100) {
        if (outIface) *outIface = bestIface;
        if (outEntry) *outEntry = bestEntry;
        return true;
    }

    return false;
}

QString DiscoveryManager::activeInterfaceAddress() const
{
    QNetworkInterface iface;
    QNetworkAddressEntry entry;
    if (selectPrimaryInterface(&iface, &entry)) {
        return entry.ip().toString();
    }
    return QString();
}

QString DiscoveryManager::activeInterfaceSubnet() const
{
    QNetworkInterface iface;
    QNetworkAddressEntry entry;
    if (selectPrimaryInterface(&iface, &entry)) {
        quint32 ipVal = entry.ip().toIPv4Address();
        quint32 maskVal = entry.netmask().toIPv4Address();
        if (maskVal == 0) maskVal = 0xFFFFFF00;
        int prefixLen = 0;
        quint32 m = maskVal;
        while (m) {
            prefixLen += (m & 1);
            m >>= 1;
        }
        QHostAddress netAddr(ipVal & maskVal);
        return QString("%1/%2").arg(netAddr.toString()).arg(prefixLen);
    }
    return QString();
}

void DiscoveryManager::startSubnetScan()
{
    if (m_isScanning) {
        return;
    }

    cancelSubnetScan();

    QNetworkInterface iface;
    QNetworkAddressEntry entry;
    if (!selectPrimaryInterface(&iface, &entry)) {
        return;
    }

    quint32 localIpVal = entry.ip().toIPv4Address();
    quint32 maskVal = entry.netmask().toIPv4Address();
    if (maskVal == 0) maskVal = 0xFFFFFF00;

    int prefixLen = 0;
    quint32 m = maskVal;
    while (m) {
        prefixLen += (m & 1);
        m >>= 1;
    }

    quint32 netVal = localIpVal & maskVal;
    int localHostNum = static_cast<int>(localIpVal & (~maskVal));

    QList<int> hostOrder;
    QSet<int> seen;

    if (prefixLen >= 24) {
        int maxHost = static_cast<int>((~maskVal) & 0xFF);
        if (maxHost < 2) maxHost = 254;

        // 1. Immediate neighbors (delta ±1 to ±40)
        for (int delta = 1; delta <= 40; ++delta) {
            int left = localHostNum - delta;
            int right = localHostNum + delta;
            if (left >= 1 && left <= maxHost && !seen.contains(left)) {
                hostOrder.append(left);
                seen.insert(left);
            }
            if (right >= 1 && right <= maxHost && !seen.contains(right)) {
                hostOrder.append(right);
                seen.insert(right);
            }
        }

        // 2. Common gateway & device hosts
        static const int commonHosts[] = {1, 2, 3, 4, 10, 50, 100, 101, 102, 150, 200, 254};
        for (size_t k = 0; k < sizeof(commonHosts)/sizeof(commonHosts[0]); ++k) {
            int h = commonHosts[k];
            if (h != localHostNum && h >= 1 && h <= maxHost && !seen.contains(h)) {
                hostOrder.append(h);
                seen.insert(h);
            }
        }

        // 3. Additional nearby hosts up to cap (max 80 hosts for responsive scan)
        for (int h = 1; h <= maxHost && hostOrder.size() < 80; ++h) {
            if (h != localHostNum && !seen.contains(h)) {
                hostOrder.append(h);
                seen.insert(h);
            }
        }
    } else {
        // Subnet prefix < 24 (e.g. /16 or /8):
        // Constrain to the local /24 neighborhood slice around host (±20) to avoid sweeping thousands of hosts
        int hostByte = static_cast<int>(localIpVal & 0xFF);
        for (int delta = 1; delta <= 20; ++delta) {
            int left = hostByte - delta;
            int right = hostByte + delta;
            if (left >= 1 && left <= 254 && !seen.contains(left)) {
                hostOrder.append(left);
                seen.insert(left);
            }
            if (right >= 1 && right <= 254 && !seen.contains(right)) {
                hostOrder.append(right);
                seen.insert(right);
            }
        }
        static const int commonHosts[] = {1, 2, 254};
        for (size_t k = 0; k < sizeof(commonHosts)/sizeof(commonHosts[0]); ++k) {
            int h = commonHosts[k];
            if (h != hostByte && !seen.contains(h)) {
                hostOrder.append(h);
                seen.insert(h);
            }
        }
    }

    QSet<quint32> allLocalIps;
    QList<QNetworkInterface> allIfaces = QNetworkInterface::allInterfaces();
    for (int i = 0; i < allIfaces.size(); ++i) {
        QList<QNetworkAddressEntry> entries = allIfaces.at(i).addressEntries();
        for (int j = 0; j < entries.size(); ++j) {
            if (entries.at(j).ip().protocol() == QAbstractSocket::IPv4Protocol) {
                allLocalIps.insert(entries.at(j).ip().toIPv4Address());
            }
        }
    }

    quint32 baseIpVal = (prefixLen >= 24) ? netVal : (localIpVal & 0xFFFFFF00);

    m_scanQueue.clear();
    for (int i = 0; i < hostOrder.size(); ++i) {
        quint32 targetIpVal = baseIpVal | static_cast<quint32>(hostOrder.at(i));
        if (allLocalIps.contains(targetIpVal)) {
            continue;
        }
        m_scanQueue.append(QHostAddress(targetIpVal));
    }

    // Fast unicast UDP announcement sweep to all target hosts
    // This immediately bypasses router multicast/broadcast filtering
    // and causes modern LocalSend peers (macOS, iOS, Android, Windows) to register back via HTTP
    QString uErr;
    QByteArray sweepDgram = ProtocolJson::encodeAnnouncement(m_localInfo, true, &uErr);
    if (!sweepDgram.isEmpty() && m_udpSocket && m_udpSocket->isOpen()) {
        for (int i = 0; i < m_scanQueue.size(); ++i) {
            m_udpSocket->writeDatagram(sweepDgram, m_scanQueue.at(i), DEFAULT_PORT);
        }
    }

    qDebug() << "[Discovery] Subnet scan started for" << m_scanQueue.size() << "hosts on base"
             << QHostAddress(baseIpVal).toString();

    m_isScanning = true;
    emit scanningChanged(true);

    if (m_scanDeadlineTimer) {
        m_scanDeadlineTimer->start(8000); // 8-second bounded whole-scan deadline
    }

    pumpScanQueue();
}

void DiscoveryManager::cancelSubnetScan()
{
    if (m_scanDeadlineTimer) {
        m_scanDeadlineTimer->stop();
    }

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

    QList<QNetworkReply*> httpsReplies = m_activeHttpsProbes.keys();
    m_activeHttpsProbes.clear();
    for (int i = 0; i < httpsReplies.size(); ++i) {
        QNetworkReply *reply = httpsReplies.at(i);
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
            qDebug() << "[Discovery] Probing" << targetAddr.toString();
            // Also send direct unicast UDP datagram to target host
            QString uErr;
            QByteArray dgram = ProtocolJson::encodeAnnouncement(m_localInfo, true, &uErr);
            if (!dgram.isEmpty() && m_udpSocket && m_udpSocket->isOpen()) {
                m_udpSocket->writeDatagram(dgram, targetAddr, DEFAULT_PORT);
            }

            m_activeProbes.insert(reply, targetAddr);
            connect(reply, SIGNAL(finished()), this, SLOT(onProbeReplyFinished()));

            // 600 ms deadline for subnet scan probe (fast responsive LAN sweep)
            QTimer *timer = new QTimer(reply);
            timer->setSingleShot(true);
            connect(timer, SIGNAL(timeout()), reply, SLOT(abort()));
            timer->start(600);
        }
    }

    if (m_scanQueue.isEmpty() && m_activeProbes.isEmpty() && m_activeHttpsProbes.isEmpty()) {
        if (m_scanDeadlineTimer) {
            m_scanDeadlineTimer->stop();
        }
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
                qDebug() << "[Discovery] Peer updated via HTTP probe:" << peer.info.alias
                         << targetAddr.toString() << ":" << peer.info.port;
                emit peerUpdated(peer);
            }
        }
    } else {
        // If HTTP probe failed, send another direct UDP announcement as fallback
        QString uErr;
        QByteArray dgram = ProtocolJson::encodeAnnouncement(m_localInfo, true, &uErr);
        if (!dgram.isEmpty() && m_udpSocket && m_udpSocket->isOpen()) {
            m_udpSocket->writeDatagram(dgram, targetAddr, DEFAULT_PORT);
        }

        // If HTTP failed (e.g. RemoteHostClosedError or TLS reset on port 53317), probe HTTPS as fallback
        if (reply->error() == QNetworkReply::RemoteHostClosedError ||
            reply->error() == QNetworkReply::ProtocolUnknownError ||
            reply->error() == QNetworkReply::SslHandshakeFailedError) {
            probeHttpsHost(targetAddr);
        }
    }

    reply->deleteLater();

    pumpScanQueue();
}

void DiscoveryManager::probeHttpsHost(const QHostAddress &addr)
{
    if (!m_nam) {
        return;
    }

    for (QMap<QString, PeerDevice>::const_iterator it = m_peers.begin(); it != m_peers.end(); ++it) {
        if (it.value().address == addr) {
            return;
        }
    }

    QUrl url;
    url.setScheme("https");
    url.setHost(addr.toString());
    url.setPort(DEFAULT_PORT);
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
        reply->ignoreSslErrors();
        m_activeHttpsProbes.insert(reply, addr);
        connect(reply, SIGNAL(finished()), this, SLOT(onHttpsProbeReplyFinished()));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), reply, SLOT(ignoreSslErrors()));

        QTimer *timer = new QTimer(reply);
        timer->setSingleShot(true);
        connect(timer, SIGNAL(timeout()), reply, SLOT(abort()));
        timer->start(600);
    }
}

void DiscoveryManager::onHttpsProbeReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }

    if (!m_activeHttpsProbes.contains(reply)) {
        reply->deleteLater();
        return;
    }

    QHostAddress targetAddr = m_activeHttpsProbes.take(reply);

    if (reply->error() == QNetworkReply::NoError) {
        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode == 200) {
            QByteArray respData = reply->readAll();
            PeerDevice target;
            target.address = targetAddr;
            target.info.port = DEFAULT_PORT;
            target.info.protocol = "https";

            PeerDevice peer;
            QString err;
            if (ProtocolJson::decodeRegisterResponse(respData, target, &peer, &err)) {
                if (peer.info.fingerprint != m_localInfo.fingerprint) {
                    peer.info.protocol = "https";
                    m_peers.insert(peer.info.fingerprint, peer);
                    emit peerUpdated(peer);
                }
            }
        }
    }

    reply->deleteLater();

    if (m_scanQueue.isEmpty() && m_activeProbes.isEmpty() && m_activeHttpsProbes.isEmpty()) {
        if (m_scanDeadlineTimer) {
            m_scanDeadlineTimer->stop();
        }
        if (m_isScanning) {
            m_isScanning = false;
            emit scanningChanged(false);
        }
    }
}
