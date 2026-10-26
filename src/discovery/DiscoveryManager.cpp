#include "discovery/DiscoveryManager.h"
#include "protocol/ProtocolJson.h"

#include <QNetworkRequest>
#include <QTimer>
#include <QSet>
#include <QFile>
#include <QTextStream>
#include <QStringList>
#include <QRegExp>
#include <QDebug>

#if defined(Q_OS_UNIX) || defined(Q_OS_QNX) || !defined(Q_OS_WIN)
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#endif

DiscoveryManager::DiscoveryManager(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
    , m_udpSocket(new QUdpSocket(this))
    , m_announceTimer(new QTimer(this))
    , m_scanPumpTimer(new QTimer(this))
    , m_scanDeadlineTimer(new QTimer(this))
    , m_udpSweepTimer(new QTimer(this))
    , m_multicastAddress(QHostAddress("224.0.0.167"))
    , m_isScanning(false)
{
    connect(m_udpSocket, SIGNAL(readyRead()), this, SLOT(onUdpReadyRead()));
    connect(m_announceTimer, SIGNAL(timeout()), this, SLOT(onAnnounceTimeout()));
    connect(m_scanPumpTimer, SIGNAL(timeout()), this, SLOT(pumpScanQueue()));
    connect(m_udpSweepTimer, SIGNAL(timeout()), this, SLOT(pumpUdpSweep()));
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
    if (!m_interface.isValid()) {
        QNetworkAddressEntry primaryEntry;
        selectPrimaryInterface(&m_interface, &primaryEntry);
    }

    // Bind UDP socket to port 53317 with address sharing (Qt4/Qt5 dual baseline)
    qDebug() << "[Discovery] Starting DiscoveryManager on interface:" << (m_interface.isValid() ? m_interface.name() : "default")
             << "IP:" << activeInterfaceAddress()
             << "Subnet:" << activeInterfaceSubnet();

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

    qDebug() << "[Discovery] UDP socket bind result:" << bound << "port:" << DEFAULT_PORT
             << "state:" << m_udpSocket->state() << "isValid:" << m_udpSocket->isValid()
             << "isOpen:" << m_udpSocket->isOpen();

    if (!bound) {
        if (error) {
            *error = m_udpSocket->errorString();
        }
        return false;
    }

    // Configure expanded send/receive buffers (64 KiB) for QNX io-pkt-v4
    setSocketBuffers();

    // Join LocalSend multicast group on all active network interfaces
    QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (int i = 0; i < ifaces.size(); ++i) {
        const QNetworkInterface &iface = ifaces.at(i);
        if ((iface.flags() & QNetworkInterface::IsUp) && (iface.flags() & QNetworkInterface::CanMulticast)) {
            bool joined = m_udpSocket->joinMulticastGroup(m_multicastAddress, iface);
            qDebug() << "[Discovery] Join multicast group on" << iface.name() << "result:" << joined;
        }
    }
    bool globalJoin = m_udpSocket->joinMulticastGroup(m_multicastAddress);
    qDebug() << "[Discovery] Join global multicast group result:" << globalJoin;

    if (m_interface.isValid()) {
        m_udpSocket->setMulticastInterface(m_interface);
        QNetworkInterface actualMcast = m_udpSocket->multicastInterface();
        qDebug() << "[Discovery] Multicast outgoing interface requested:" << m_interface.name()
                 << "effective:" << (actualMcast.isValid() ? actualMcast.name() : m_interface.name());
    } else {
        qDebug() << "[Discovery] No active LAN interface resolved for multicast outgoing configuration";
    }

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
    m_udpSweepAttempts.clear();

    if (m_announceTimer) {
        m_announceTimer->stop();
    }

    if (m_udpSocket && (m_udpSocket->isValid() || m_udpSocket->isOpen())) {
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
        if (!dgram.isEmpty() && m_udpSocket && (m_udpSocket->isValid() || m_udpSocket->isOpen())) {
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
    if (!m_udpSocket || (!m_udpSocket->isValid() && !m_udpSocket->isOpen())) {
        return;
    }

    QString err;
    QByteArray datagram = ProtocolJson::encodeAnnouncement(m_localInfo, announce, &err);
    if (!datagram.isEmpty()) {
        // 1. Send to standard LocalSend multicast address (224.0.0.167:53317)
        qint64 mRes = m_udpSocket->writeDatagram(datagram, m_multicastAddress, DEFAULT_PORT);

        // 2. Send to standard LocalSend global broadcast address (255.255.255.255:53317)
        qint64 bRes = m_udpSocket->writeDatagram(datagram, QHostAddress::Broadcast, DEFAULT_PORT);

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
                    qint64 dRes = m_udpSocket->writeDatagram(datagram, bcast, DEFAULT_PORT);
                    qDebug() << "[Discovery] Directed broadcast to" << bcast.toString() << "res=" << dRes;
                }
            }
        }
        qDebug() << "[Discovery] UDP announcement sent (announce=" << announce
                 << "len=" << datagram.size() << "multicast=" << mRes << "broadcast=" << bRes << ")";
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

        qDebug() << "[Discovery] UDP datagram received from" << senderAddress.toString()
                 << ":" << senderPort << "len=" << datagram.size();

        if (datagram.size() > ProtocolJson::MAX_JSON_SIZE) {
            qWarning() << "[Discovery] Datagram oversized, dropped";
            continue; // Ignore oversized datagram
        }

        DeviceInfo info;
        bool announce = false;
        QString err;
        if (!ProtocolJson::decodeAnnouncement(datagram, &info, &announce, &err)) {
            qWarning() << "[Discovery] Announcement decode failed:" << err;
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

        addHistoricalPeer(senderAddress);
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
    addHistoricalPeer(address);
    QString uErr;
    QByteArray dgram = ProtocolJson::encodeAnnouncement(m_localInfo, true, &uErr);
    if (!dgram.isEmpty() && m_udpSocket && (m_udpSocket->isValid() || m_udpSocket->isOpen())) {
        m_udpSocket->writeDatagram(dgram, address, port > 0 ? port : DEFAULT_PORT);
    }

    // If peer is already known and uses HTTPS protocol, send HTTPS register using frozen pin
    for (QMap<QString, PeerDevice>::const_iterator it = m_peers.begin(); it != m_peers.end(); ++it) {
        if (it.value().address == address && (port == 0 || it.value().info.port == port)) {
            if (it.value().info.protocol.toLower() == "https") {
                QString pin = it.value().info.fingerprint;
                if (!pin.isEmpty() && pin.length() == 64) {
                    sendHttpsRegister(address, port > 0 ? port : it.value().info.port, pin);
                    return;
                }
                // Unknown-pin bootstrap policy: do not send plaintext HTTP to TLS port
                return;
            }
        }
    }

    sendHttpRegister(address, port);
}

void DiscoveryManager::sendHttpsRegister(const QHostAddress &address, quint16 port, const QString &pin)
{
    // Prevent duplicate pending registers to the same endpoint
    for (QMap<localsend::PosixTlsHttpClient*, PeerDevice>::const_iterator it = m_pendingHttpsRegisters.begin();
         it != m_pendingHttpsRegisters.end(); ++it) {
        if (it.value().address == address && it.value().info.port == port) {
            return;
        }
    }

    QString normPin = localsend::LocalIdentity::normalizeFingerprint(pin);
    if (normPin.isEmpty() || normPin.length() != 64) {
        qWarning() << "[Discovery] Missing or invalid peer pin for HTTPS register:" << address << port;
        return;
    }

    localsend::PosixTlsHttpClient *client = new localsend::PosixTlsHttpClient(m_localIdentity, this);
    connect(client, SIGNAL(finished(int,QByteArray)), this, SLOT(onHttpsRegisterFinished(int,QByteArray)));
    connect(client, SIGNAL(failed(int,QString)), this, SLOT(onHttpsRegisterFailed(int,QString)));

    QMap<QString, QString> headers;
    headers.insert("Content-Type", "application/json");

    QString err;
    QByteArray body = ProtocolJson::encodeRegisterRequest(m_localInfo, &err);
    if (body.isEmpty()) {
        client->deleteLater();
        return;
    }

    PeerDevice target;
    target.address = address;
    target.info.port = port;
    target.info.protocol = "https";
    target.info.fingerprint = normPin;
    m_pendingHttpsRegisters.insert(client, target);

    if (!client->sendRequest("POST", address, port, "/api/localsend/v2/register", normPin, headers, body, 5000)) {
        m_pendingHttpsRegisters.remove(client);
        client->deleteLater();
    }
}

void DiscoveryManager::onHttpsRegisterFinished(int status, const QByteArray &responseBody)
{
    localsend::PosixTlsHttpClient *client = qobject_cast<localsend::PosixTlsHttpClient*>(sender());
    if (!client || !m_pendingHttpsRegisters.contains(client)) {
        return;
    }

    PeerDevice target = m_pendingHttpsRegisters.take(client);
    client->deleteLater();

    if (status == 200) {
        PeerDevice peer;
        QString err;
        if (ProtocolJson::decodeRegisterResponse(responseBody, target, &peer, &err)) {
            if (peer.info.fingerprint != m_localInfo.fingerprint) {
                peer.lastSeen = QDateTime::currentDateTime();
                m_peers.insert(peer.info.fingerprint, peer);
                qDebug() << "[Discovery] Peer updated via HTTPS register:" << peer.info.alias
                         << "(" << peer.info.deviceModel << ")"
                         << target.address.toString() << ":" << peer.info.port << "protocol:" << peer.info.protocol;
                emit peerUpdated(peer);
            }
        } else {
            qWarning() << "[Discovery] HTTPS register response decode failed:" << err;
        }
    } else {
        qWarning() << "[Discovery] HTTPS register returned non-200 status:" << status;
    }
}

void DiscoveryManager::onHttpsRegisterFailed(int code, const QString &msg)
{
    localsend::PosixTlsHttpClient *client = qobject_cast<localsend::PosixTlsHttpClient*>(sender());
    if (!client || !m_pendingHttpsRegisters.contains(client)) {
        return;
    }

    PeerDevice target = m_pendingHttpsRegisters.take(client);
    client->deleteLater();

    qWarning() << "[Discovery] Outgoing HTTPS register failed code:" << code << "msg:" << msg
               << "target:" << target.address.toString() << ":" << target.info.port;
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
            addHistoricalPeer(peer.address);
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

    addHistoricalPeer(cleanAddress);
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

void DiscoveryManager::addHistoricalPeer(const QHostAddress &addr)
{
    if (addr.isNull() || addr == QHostAddress::LocalHost || addr == QHostAddress::Broadcast) {
        return;
    }

    QHostAddress cleanAddr = addr;
    if (cleanAddr.protocol() == QAbstractSocket::IPv6Protocol) {
        quint32 ipv4 = cleanAddr.toIPv4Address();
        if (ipv4 != 0) {
            cleanAddr = QHostAddress(ipv4);
        }
    }

    if (cleanAddr.protocol() != QAbstractSocket::IPv4Protocol) {
        return;
    }

    m_historicalPeers.removeAll(cleanAddr);
    m_historicalPeers.prepend(cleanAddr);
    while (m_historicalPeers.size() > 50) {
        m_historicalPeers.removeLast();
    }
}

void DiscoveryManager::setSocketBuffers()
{
    if (!m_udpSocket) {
        return;
    }

    // Set Qt-level socket buffer options (Qt 5+)
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    m_udpSocket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 65536);
    m_udpSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 65536);
#endif

    // Set native POSIX / QNX socket buffer options
    // (Critical on QNX io-pkt-v4 where default non-blocking UDP SO_SNDBUF is ~8-9 KiB)
#if !defined(Q_OS_WIN)
    int fd = m_udpSocket->socketDescriptor();
    if (fd >= 0) {
        int bufSize = 65536;
        int resSnd = ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&bufSize, sizeof(bufSize));
        int errSnd = (resSnd < 0) ? errno : 0;
        int resRcv = ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&bufSize, sizeof(bufSize));
        int errRcv = (resRcv < 0) ? errno : 0;

        int actualSnd = 0;
        int actualRcv = 0;
        socklen_t optLen = sizeof(actualSnd);
        int getSnd = ::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *)&actualSnd, &optLen);
        int errGetSnd = (getSnd < 0) ? errno : 0;
        optLen = sizeof(actualRcv);
        int getRcv = ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&actualRcv, &optLen);
        int errGetRcv = (getRcv < 0) ? errno : 0;

        qDebug() << "[Discovery] Socket buffers: setsockopt SO_SNDBUF (req=65536, res=" << resSnd
                 << (errSnd ? QString(" errno=%1").arg(errSnd) : QString())
                 << ", actual=" << (getSnd == 0 ? actualSnd : -1)
                 << (errGetSnd ? QString(" get_errno=%1").arg(errGetSnd) : QString())
                 << ") SO_RCVBUF (req=65536, res=" << resRcv
                 << (errRcv ? QString(" errno=%1").arg(errRcv) : QString())
                 << ", actual=" << (getRcv == 0 ? actualRcv : -1)
                 << (errGetRcv ? QString(" get_errno=%1").arg(errGetRcv) : QString()) << ")";
    }
#endif
}

bool DiscoveryManager::getSocketBufferSizes(int *sndBuf, int *rcvBuf) const
{
    if (!m_udpSocket) {
        return false;
    }
#if !defined(Q_OS_WIN)
    int fd = m_udpSocket->socketDescriptor();
    if (fd >= 0) {
        int s = 0, r = 0;
        socklen_t len = sizeof(s);
        int res1 = ::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *)&s, &len);
        len = sizeof(r);
        int res2 = ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&r, &len);
        if (res1 == 0 && res2 == 0) {
            if (sndBuf) *sndBuf = s;
            if (rcvBuf) *rcvBuf = r;
            return true;
        }
    }
#endif
    return false;
}

void DiscoveryManager::queueUdpSweepTarget(const QHostAddress &addr)
{
    if (addr.isNull() || addr == QHostAddress::LocalHost || addr == QHostAddress::Broadcast) {
        return;
    }
    quint32 targetIp = addr.toIPv4Address();
    if (m_udpSweepAttempts.value(targetIp, 0) >= 3) {
        qDebug() << "[Discovery] Target" << addr.toString()
                 << "already reached maximum attempts (3) for this scan; skipping fallback enqueue";
        return;
    }
    if (!m_udpSweepQueue.contains(addr)) {
        m_udpSweepQueue.append(addr);
    }
    if (m_udpSweepTimer && !m_udpSweepTimer->isActive()) {
        m_udpSweepTimer->start(15);
    }
}

void DiscoveryManager::injectScanTarget(const QHostAddress &addr)
{
    m_scanQueue.append(addr);
    m_isScanning = true;
    pumpScanQueue();
}

QList<QHostAddress> DiscoveryManager::loadArpCacheIps() const
{
    QList<QHostAddress> result;
    QFile file("/proc/net/arp");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return result;
    }

    QTextStream in(&file);
    in.readLine(); // Skip header
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QStringList parts = line.split(QRegExp("\\s+"), QString::SkipEmptyParts);
        if (parts.size() >= 4) {
            QString ipStr = parts.at(0);
            QString flagsStr = parts.at(2);
            bool ok = false;
            int flags = flagsStr.toInt(&ok, 0);
            // Flags: 0x2 is ATF_COM (completed ARP entry)
            if (ok && (flags & 0x2)) {
                QHostAddress addr(ipStr);
                if (addr.protocol() == QAbstractSocket::IPv4Protocol && !addr.isNull()) {
                    result.append(addr);
                }
            }
        }
    }
    return result;
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
    cancelSubnetScan();

    QNetworkInterface iface;
    QNetworkAddressEntry entry;
    if (!selectPrimaryInterface(&iface, &entry)) {
        return;
    }

    if (iface.isValid() && (!m_interface.isValid() || m_interface.name() != iface.name())) {
        m_interface = iface;
        if (m_udpSocket && (m_udpSocket->isValid() || m_udpSocket->isOpen())) {
            m_udpSocket->setMulticastInterface(m_interface);
            QNetworkInterface actualMcast = m_udpSocket->multicastInterface();
            qDebug() << "[Discovery] Multicast outgoing interface refreshed to:" << m_interface.name()
                     << "effective:" << (actualMcast.isValid() ? actualMcast.name() : m_interface.name());
        }
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

    int maxHost = static_cast<int>((~maskVal) & 0xFF);
    if (maxHost < 2) maxHost = 254;

    // 1. Historical peers within this subnet (MRU order, top priority)
    for (int i = 0; i < m_historicalPeers.size(); ++i) {
        const QHostAddress &hAddr = m_historicalPeers.at(i);
        quint32 hIpVal = hAddr.toIPv4Address();
        if ((hIpVal & maskVal) == netVal) {
            int h = static_cast<int>(hIpVal & (~maskVal));
            if (h != localHostNum && h >= 1 && h <= maxHost && !seen.contains(h)) {
                hostOrder.append(h);
                seen.insert(h);
                qDebug() << "[Discovery] Prioritizing historical peer host:" << hAddr.toString();
            }
        }
    }

    // 2. Active ARP cache hosts (completed ARP entries on LAN)
    QList<QHostAddress> arpHosts = loadArpCacheIps();
    for (int i = 0; i < arpHosts.size(); ++i) {
        const QHostAddress &aAddr = arpHosts.at(i);
        quint32 aIpVal = aAddr.toIPv4Address();
        if ((aIpVal & maskVal) == netVal) {
            int h = static_cast<int>(aIpVal & (~maskVal));
            if (h != localHostNum && h >= 1 && h <= maxHost && !seen.contains(h)) {
                hostOrder.append(h);
                seen.insert(h);
                qDebug() << "[Discovery] Prioritizing active ARP host:" << aAddr.toString();
            }
        }
    }

    if (prefixLen >= 24) {
        // 3. Immediate neighbors (delta ±1 to ±40)
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

        // 4. Common gateway & device hosts
        static const int commonHosts[] = {1, 2, 3, 4, 10, 50, 100, 101, 102, 150, 200, 254};
        for (size_t k = 0; k < sizeof(commonHosts)/sizeof(commonHosts[0]); ++k) {
            int h = commonHosts[k];
            if (h != localHostNum && h >= 1 && h <= maxHost && !seen.contains(h)) {
                hostOrder.append(h);
                seen.insert(h);
            }
        }

        // 5. Additional nearby hosts up to cap (max 80 hosts for responsive scan)
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
    m_udpSweepAttempts.clear();
    for (int i = 0; i < hostOrder.size(); ++i) {
        quint32 targetIpVal = baseIpVal | static_cast<quint32>(hostOrder.at(i));
        if (allLocalIps.contains(targetIpVal)) {
            continue;
        }
        m_scanQueue.append(QHostAddress(targetIpVal));
    }

    // Configure expanded UDP socket send/recv buffers (64 KiB) before sweeping
    setSocketBuffers();

    // Start paced unicast UDP announcement sweep across the subnet.
    // Instead of blasting all datagrams simultaneously in a tight C++ loop (which overruns
    // QNX 6.5 io-pkt 8 KiB socket send buffers and causes ENOBUFS packet drops),
    // we enqueue them into m_udpSweepQueue and pace transmission via m_udpSweepTimer
    // at 15 ms intervals (5 datagrams/batch, ~260 ms for 88 hosts).
    if (m_udpSweepTimer) {
        m_udpSweepTimer->stop();
    }
    m_udpSweepQueue = m_scanQueue;
    if (m_udpSweepTimer && !m_udpSweepQueue.isEmpty()) {
        m_udpSweepTimer->start(15);
    }

    qDebug() << "[Discovery] Subnet scan started for" << m_scanQueue.size() << "hosts on base"
             << QHostAddress(baseIpVal).toString();

    m_isScanning = true;
    emit scanningChanged(true);

    if (m_scanDeadlineTimer) {
        m_scanDeadlineTimer->start(12000); // 12-second bounded whole-scan deadline
    }

    pumpScanQueue();
}

void DiscoveryManager::cancelSubnetScan()
{
    if (m_scanDeadlineTimer) {
        m_scanDeadlineTimer->stop();
    }
    if (m_udpSweepTimer) {
        m_udpSweepTimer->stop();
    }
    m_udpSweepQueue.clear();
    m_udpSweepAttempts.clear();

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

void DiscoveryManager::pumpUdpSweep()
{
    if (!m_udpSocket || (!m_udpSocket->isValid() && !m_udpSocket->isOpen()) || m_udpSweepQueue.isEmpty()) {
        if (m_udpSweepTimer) {
            m_udpSweepTimer->stop();
        }
        if (m_scanQueue.isEmpty() && m_activeProbes.isEmpty()) {
            if (m_scanDeadlineTimer) {
                m_scanDeadlineTimer->stop();
            }
            if (m_isScanning) {
                m_isScanning = false;
                m_udpSweepAttempts.clear();
                emit scanningChanged(false);
            }
        }
        return;
    }

    QString uErr;
    QByteArray sweepDgram = ProtocolJson::encodeAnnouncement(m_localInfo, true, &uErr);
    if (sweepDgram.isEmpty()) {
        if (m_udpSweepTimer) {
            m_udpSweepTimer->stop();
        }
        m_udpSweepQueue.clear();
        m_udpSweepAttempts.clear();
        return;
    }

    const int BATCH_SIZE = 5;
    const int MAX_ATTEMPTS = 3;
    int sentCount = 0;

    while (!m_udpSweepQueue.isEmpty() && sentCount < BATCH_SIZE) {
        QHostAddress target = m_udpSweepQueue.takeFirst();
        quint32 targetIp = target.toIPv4Address();
        if (m_udpSweepAttempts.value(targetIp, 0) >= MAX_ATTEMPTS) {
            qDebug() << "[Discovery] Target" << target.toString()
                     << "already reached max attempts (" << MAX_ATTEMPTS << "); skipping send";
            continue;
        }
        int attempts = m_udpSweepAttempts.value(targetIp, 0) + 1;
        m_udpSweepAttempts.insert(targetIp, attempts);

        qint64 bytesWritten = m_udpSocket->writeDatagram(sweepDgram, target, DEFAULT_PORT);
        if (bytesWritten < 0) {
            QAbstractSocket::SocketError err = m_udpSocket->error();
            QString errStr = m_udpSocket->errorString();
            bool isTransient = (
#if QT_VERSION >= QT_VERSION_CHECK(5, 3, 0)
                                err == QAbstractSocket::TemporaryError ||
#endif
                                err == QAbstractSocket::SocketResourceError ||
#if defined(Q_OS_UNIX) || defined(Q_OS_QNX) || defined(Q_OS_BLACKBERRY) || defined(__QNXNTO__)
                                errno == ENOBUFS || errno == EAGAIN || errno == EWOULDBLOCK
#else
                                false
#endif
                               );

            if (isTransient && attempts < MAX_ATTEMPTS) {
                // Transient network buffer congestion: defer target behind unattempted targets (tail)
                // so failing target never blocks subsequent unattempted targets (P1).
                qDebug() << "[Discovery] UDP sweep write transient failure to" << target.toString()
                         << "attempt" << attempts << "/" << MAX_ATTEMPTS
                         << "error:" << errStr << "- deferring to tail and throttling batch";
                m_udpSweepQueue.append(target);
                break; // Pause current batch to let network stack drain
            } else {
                // Permanent error or exceeded max attempts: drop and continue with rest of queue
                qWarning() << "[Discovery] UDP sweep dropping target" << target.toString()
                           << "after" << attempts << "attempts. Error:" << err << errStr;
                if (!m_udpSocket->isValid() && !m_udpSocket->isOpen()) {
                    break;
                }
                continue;
            }
        }
        sentCount++;
    }

    if (m_udpSweepQueue.isEmpty()) {
        if (m_udpSweepTimer) {
            m_udpSweepTimer->stop();
        }
        qDebug() << "[Discovery] UDP sweep completed successfully";
        if (m_scanQueue.isEmpty() && m_activeProbes.isEmpty()) {
            if (m_scanDeadlineTimer) {
                m_scanDeadlineTimer->stop();
            }
            if (m_isScanning) {
                m_isScanning = false;
                m_udpSweepAttempts.clear();
                emit scanningChanged(false);
            }
        }
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

            m_activeProbes.insert(reply, targetAddr);
            connect(reply, SIGNAL(finished()), this, SLOT(onProbeReplyFinished()));

            // 450 ms deadline for subnet scan probe (fast responsive LAN sweep)
            QTimer *timer = new QTimer(reply);
            timer->setSingleShot(true);
            connect(timer, SIGNAL(timeout()), reply, SLOT(abort()));
            timer->start(450);
        }
    }

    if (m_scanQueue.isEmpty() && m_activeProbes.isEmpty() && m_udpSweepQueue.isEmpty()) {
        if (m_scanDeadlineTimer) {
            m_scanDeadlineTimer->stop();
        }
        if (m_isScanning) {
            m_isScanning = false;
            m_udpSweepAttempts.clear();
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
                addHistoricalPeer(peer.address);
                m_peers.insert(peer.info.fingerprint, peer);
                qDebug() << "[Discovery] Peer updated via HTTP probe:" << peer.info.alias
                         << targetAddr.toString() << ":" << peer.info.port;
                emit peerUpdated(peer);
            }
        }
    } else {
        queueUdpSweepTarget(targetAddr);
    }

    reply->deleteLater();

    pumpScanQueue();
}
