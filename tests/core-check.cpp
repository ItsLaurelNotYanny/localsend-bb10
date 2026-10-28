#include "protocol/ProtocolTypes.h"
#include "protocol/ProtocolJson.h"
#include "protocol/CryptoUtil.h"
#include "security/LocalIdentity.h"
#include "security/PosixTlsSocket.h"
#include "security/PosixTlsHttpClient.h"
#include "receive/HttpServer.h"
#include "receive/HttpConnection.h"
#include "receive/ReceiveServer.h"
#include "send/SendManager.h"
#include "app/Application.h"
#include "discovery/DiscoveryManager.h"
#include "ui/UiController.h"

#include <QUdpSocket>
#include <QTemporaryFile>
#include <QDir>
#include <QFile>
#include <QBuffer>

#include <QCoreApplication>
#include <QTime>
#include <QTcpSocket>
#include <QThread>
#include <QCryptographicHash>
#include <iostream>
#include <cstdlib>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int g_testCount = 0;
static int g_passCount = 0;
static int g_failCount = 0;

#define TEST_CASE(name) \
    do { \
        std::cout << "[RUN] " << name << std::endl; \
        g_testCount++; \
    } while (0)

#define ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  FAILED: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            g_failCount++; \
            return; \
        } \
    } while (0)

#define ASSERT_FALSE(cond, msg) \
    do { \
        if ((cond)) { \
            std::cerr << "  FAILED: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            g_failCount++; \
            return; \
        } \
    } while (0)

#define TEST_PASS() \
    do { \
        g_passCount++; \
        std::cout << "  PASS" << std::endl; \
    } while (0)

// Helper to pump event loop
static bool waitForCondition(bool *flag, int timeoutMs = 2000)
{
    QTime t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (*flag) return true;
        usleep(5000);
    }
    return *flag;
}

// Test HTTP Receiver Object
class TestHttpReceiver : public QObject {
    Q_OBJECT
public:
    HttpConnection *lastConn;
    HttpRequestHead lastHead;
    QByteArray receivedBody;
    bool headersReceived;
    bool requestFinishedReceived;
    bool disconnectedReceived;

    TestHttpReceiver(QObject *parent = 0)
        : QObject(parent)
        , lastConn(0)
        , headersReceived(false)
        , requestFinishedReceived(false)
        , disconnectedReceived(false)
    {
    }

public slots:
    void onConnection(HttpConnection *conn)
    {
        lastConn = conn;
        connect(conn, SIGNAL(headersReady(HttpRequestHead)), this, SLOT(onHeaders(HttpRequestHead)));
        connect(conn, SIGNAL(bodyChunk(QByteArray)), this, SLOT(onChunk(QByteArray)));
        connect(conn, SIGNAL(requestFinished()), this, SLOT(onFinished()));
        connect(conn, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
    }

    void onHeaders(const HttpRequestHead &head)
    {
        lastHead = head;
        headersReceived = true;
    }

    void onChunk(const QByteArray &chunk)
    {
        receivedBody.append(chunk);
    }

    void onFinished()
    {
        requestFinishedReceived = true;
    }

    void onDisconnected()
    {
        disconnectedReceived = true;
    }
};

static bool waitSocketReadyRead(QTcpSocket *socket, int timeoutMs = 2000)
{
    QTime t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (socket->bytesAvailable() > 0) return true;
        usleep(2000);
    }
    return socket->bytesAvailable() > 0;
}

static bool waitSocketConnected(QTcpSocket *socket, int timeoutMs = 2000)
{
    QTime t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (socket->state() == QAbstractSocket::ConnectedState) return true;
        usleep(2000);
    }
    return socket->state() == QAbstractSocket::ConnectedState;
}

class ConnsReceiver : public QObject {
    Q_OBJECT
public:
    QList<HttpConnection*> conns;
    ConnsReceiver(QObject *parent = 0) : QObject(parent) {}
public slots:
    void onConn(HttpConnection *c) { conns.append(c); }
};

// ==========================================
// T01 Protocol JSON Tests
// ==========================================

void testAnnouncementValid()
{
    TEST_CASE("Announcement - Valid with known and extra fields");
    QByteArray json = "{\n"
                      "  \"alias\": \"Passport\",\n"
                      "  \"version\": \"2.2\",\n"
                      "  \"deviceModel\": \"BlackBerry Passport\",\n"
                      "  \"deviceType\": \"mobile\",\n"
                      "  \"fingerprint\": \"fp-12345\",\n"
                      "  \"port\": 53317,\n"
                      "  \"protocol\": \"http\",\n"
                      "  \"download\": false,\n"
                      "  \"announce\": true,\n"
                      "  \"extraField\": \"ignored\"\n"
                      "}";

    DeviceInfo info;
    bool announce = false;
    QString err;
    bool ok = ProtocolJson::decodeAnnouncement(json, &info, &announce, &err);

    ASSERT_TRUE(ok, err.toLocal8Bit().constData());
    ASSERT_TRUE(info.alias == "Passport", "alias mismatch");
    ASSERT_TRUE(info.version == "2.2", "version mismatch");
    ASSERT_TRUE(info.deviceType == "mobile", "deviceType mismatch");
    ASSERT_TRUE(info.port == 53317, "port mismatch");
    ASSERT_TRUE(info.protocol == "http", "protocol mismatch");
    ASSERT_FALSE(info.download, "download must be false");
    ASSERT_TRUE(announce, "announce must be true");

    TEST_PASS();
}

void testAnnouncementDeviceTypeFallback()
{
    TEST_CASE("Announcement - Unknown deviceType falls back to desktop");
    QByteArray json = "{"
                      "\"alias\":\"Peer\","
                      "\"version\":\"2.1\","
                      "\"deviceType\":\"supercomputer\","
                      "\"fingerprint\":\"fp-999\","
                      "\"port\":53317,"
                      "\"protocol\":\"http\""
                      "}";

    DeviceInfo info;
    bool announce = true;
    QString err;
    bool ok = ProtocolJson::decodeAnnouncement(json, &info, &announce, &err);

    ASSERT_TRUE(ok, err.toLocal8Bit().constData());
    ASSERT_TRUE(info.deviceType == "desktop", "unknown deviceType should fall back to desktop");
    ASSERT_FALSE(info.download, "missing download should default to false");
    ASSERT_TRUE(announce, "missing announce should default to true");

    TEST_PASS();
}

void testAnnouncementVersionIncompatible()
{
    TEST_CASE("Announcement - Incompatible major version (3.0 and 1.0) rejected");
    DeviceInfo info;
    QString err;

    QByteArray v3 = "{\"alias\":\"A\",\"version\":\"3.0\",\"fingerprint\":\"F\",\"port\":53317,\"protocol\":\"http\"}";
    ASSERT_FALSE(ProtocolJson::decodeAnnouncement(v3, &info, NULL, &err), "v3.0 should be rejected");

    QByteArray v1 = "{\"alias\":\"A\",\"version\":\"1.0\",\"fingerprint\":\"F\",\"port\":53317,\"protocol\":\"http\"}";
    ASSERT_FALSE(ProtocolJson::decodeAnnouncement(v1, &info, NULL, &err), "v1.0 should be rejected");

    TEST_PASS();
}

void testAnnouncementInvalidPort()
{
    TEST_CASE("Announcement - Invalid port (0, 70000, string) rejected");
    DeviceInfo info;
    QString err;

    QByteArray p0 = "{\"alias\":\"A\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":0,\"protocol\":\"http\"}";
    ASSERT_FALSE(ProtocolJson::decodeAnnouncement(p0, &info, NULL, &err), "port 0 rejected");

    QByteArray pBig = "{\"alias\":\"A\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":70000,\"protocol\":\"http\"}";
    ASSERT_FALSE(ProtocolJson::decodeAnnouncement(pBig, &info, NULL, &err), "port 70000 rejected");

    QByteArray pStr = "{\"alias\":\"A\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":\"53317\",\"protocol\":\"http\"}";
    ASSERT_FALSE(ProtocolJson::decodeAnnouncement(pStr, &info, NULL, &err), "string port rejected");

    TEST_PASS();
}

void testRegisterResponseFieldPreservation()
{
    TEST_CASE("Register Response - Preserves target port, protocol, and observed IP");
    QByteArray resJson = "{\n"
                         "  \"alias\": \"Fedora LocalSend\",\n"
                         "  \"version\": \"2.2\",\n"
                         "  \"deviceModel\": \"ThinkPad\",\n"
                         "  \"deviceType\": \"desktop\",\n"
                         "  \"fingerprint\": \"fedora-fp-123\",\n"
                         "  \"download\": true\n"
                         "}";

    PeerDevice target;
    target.address = QHostAddress("192.168.1.105");
    target.info.port = 53317;
    target.info.protocol = "http";

    PeerDevice peer;
    QString err;
    bool ok = ProtocolJson::decodeRegisterResponse(resJson, target, &peer, &err);

    ASSERT_TRUE(ok, err.toLocal8Bit().constData());
    ASSERT_TRUE(peer.info.alias == "Fedora LocalSend", "alias mismatch");
    ASSERT_TRUE(peer.info.port == 53317, "target port must be preserved");
    ASSERT_TRUE(peer.info.protocol == "http", "target protocol must be preserved");
    ASSERT_TRUE(peer.address == QHostAddress("192.168.1.105"), "observed address preserved");
    ASSERT_TRUE(peer.info.download, "download field true");

    TEST_PASS();
}

void testPrepareRequestValidSingleFile()
{
    TEST_CASE("Prepare Request - Single file with Chinese and spaces, 10 MiB");
    QByteArray reqJson = "{\n"
                         "  \"info\": {\n"
                         "    \"alias\": \"PC\",\n"
                         "    \"version\": \"2.2\",\n"
                         "    \"fingerprint\": \"pc-fp\",\n"
                         "    \"port\": 53317,\n"
                         "    \"protocol\": \"http\"\n"
                         "  },\n"
                         "  \"files\": {\n"
                         "    \"file-abc\": {\n"
                         "      \"id\": \"file-abc\",\n"
                         "      \"fileName\": \"\\u6d4b\\u8bd5 document 1.bin\",\n"
                         "      \"size\": 10485760,\n"
                         "      \"fileType\": \"application/octet-stream\",\n"
                         "      \"sha256\": \"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"\n"
                         "    }\n"
                         "  }\n"
                         "}";

    PrepareUploadRequest req;
    QString err;
    bool ok = ProtocolJson::decodePrepareRequest(reqJson, &req, &err);

    ASSERT_TRUE(ok, err.toLocal8Bit().constData());
    ASSERT_TRUE(req.files.size() == 1, "exactly one file");
    ASSERT_TRUE(req.files.contains("file-abc"), "file-abc missing");

    const FileMeta &meta = req.files.value("file-abc");
    ASSERT_TRUE(meta.size == 10485760, "size must be 10485760 (10 MiB)");
    ASSERT_TRUE(meta.fileName == QString::fromUtf8("测试 document 1.bin"), "unicode filename mismatch");
    ASSERT_TRUE(meta.sha256 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 mismatch");

    TEST_PASS();
}

void testPrepareRequestLargeSize64Bit()
{
    TEST_CASE("Prepare Request - 64-bit size (4 GiB: 4294967296)");
    QByteArray reqJson = "{\n"
                         "  \"info\": {\"alias\":\"P\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":53317,\"protocol\":\"http\"},\n"
                         "  \"files\": {\n"
                         "    \"file-big\": {\n"
                         "      \"id\": \"file-big\",\n"
                         "      \"fileName\": \"big.img\",\n"
                         "      \"size\": 4294967296\n"
                         "    }\n"
                         "  }\n"
                         "}";

    PrepareUploadRequest req;
    QString err;
    bool ok = ProtocolJson::decodePrepareRequest(reqJson, &req, &err);

    ASSERT_TRUE(ok, err.toLocal8Bit().constData());
    ASSERT_TRUE(req.files.value("file-big").size == 4294967296LL, "64-bit size mismatch");

    TEST_PASS();
}

void testPrepareRequestRejectInvalid()
{
    TEST_CASE("Prepare Request - Rejection cases: array files, traversal, bad sizes, id mismatch");
    PrepareUploadRequest req;
    QString err;

    // Files is array
    QByteArray arrFiles = "{\"info\":{\"alias\":\"P\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":53317,\"protocol\":\"http\"},\"files\":[]}";
    ASSERT_FALSE(ProtocolJson::decodePrepareRequest(arrFiles, &req, &err), "array files must be rejected");

    // Id mismatch between key and inner id
    QByteArray idMismatch = "{\"info\":{\"alias\":\"P\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":53317,\"protocol\":\"http\"},"
                           "\"files\":{\"key-1\":{\"id\":\"key-2\",\"fileName\":\"a.txt\",\"size\":10}}}";
    ASSERT_FALSE(ProtocolJson::decodePrepareRequest(idMismatch, &req, &err), "id mismatch must be rejected");

    // Path traversal in filename
    QByteArray trav = "{\"info\":{\"alias\":\"P\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":53317,\"protocol\":\"http\"},"
                      "\"files\":{\"k\":{\"id\":\"k\",\"fileName\":\"../../etc/passwd\",\"size\":10}}}";
    ASSERT_FALSE(ProtocolJson::decodePrepareRequest(trav, &req, &err), "path traversal must be rejected");

    // Negative size
    QByteArray negSize = "{\"info\":{\"alias\":\"P\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":53317,\"protocol\":\"http\"},"
                         "\"files\":{\"k\":{\"id\":\"k\",\"fileName\":\"a.txt\",\"size\":-1}}}";
    ASSERT_FALSE(ProtocolJson::decodePrepareRequest(negSize, &req, &err), "negative size must be rejected");

    // String size
    QByteArray strSize = "{\"info\":{\"alias\":\"P\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":53317,\"protocol\":\"http\"},"
                         "\"files\":{\"k\":{\"id\":\"k\",\"fileName\":\"a.txt\",\"size\":\"1000\"}}}";
    ASSERT_FALSE(ProtocolJson::decodePrepareRequest(strSize, &req, &err), "string size must be rejected");

    // Fractional size
    QByteArray fracSize = "{\"info\":{\"alias\":\"P\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":53317,\"protocol\":\"http\"},"
                          "\"files\":{\"k\":{\"id\":\"k\",\"fileName\":\"a.txt\",\"size\":10.5}}}";
    ASSERT_FALSE(ProtocolJson::decodePrepareRequest(fracSize, &req, &err), "fractional size must be rejected");

    TEST_PASS();
}

void testOversizedJsonRejected()
{
    TEST_CASE("Oversized JSON (>64 KiB) rejected");
    DeviceInfo info;
    QString err;

    QByteArray bigData = "{\"alias\":\"";
    bigData.append(QByteArray(70000, 'x'));
    bigData.append("\",\"version\":\"2.2\",\"fingerprint\":\"F\",\"port\":53317,\"protocol\":\"http\"}");

    ASSERT_FALSE(ProtocolJson::decodeAnnouncement(bigData, &info, NULL, &err), "oversized payload must be rejected");

    TEST_PASS();
}

void testRoundTripEncodeDecode()
{
    TEST_CASE("Round trip serialization and deserialization");
    DeviceInfo info;
    info.alias = "BlackBerry Passport";
    info.version = "2.2";
    info.deviceModel = "SQW100-1";
    info.deviceType = "mobile";
    info.fingerprint = "passport-xyz";
    info.port = 53317;
    info.protocol = "http";
    info.download = false;

    QString err;
    QByteArray encAnnounce = ProtocolJson::encodeAnnouncement(info, true, &err);
    ASSERT_TRUE(!encAnnounce.isEmpty(), "encodeAnnouncement produced empty output");

    DeviceInfo decInfo;
    bool announceFlag = false;
    bool ok = ProtocolJson::decodeAnnouncement(encAnnounce, &decInfo, &announceFlag, &err);
    ASSERT_TRUE(ok, err.toLocal8Bit().constData());
    ASSERT_TRUE(decInfo.alias == info.alias, "alias round-trip mismatch");
    ASSERT_TRUE(decInfo.fingerprint == info.fingerprint, "fingerprint round-trip mismatch");
    ASSERT_TRUE(announceFlag == true, "announce round-trip mismatch");

    // Prepare response round trip
    PrepareUploadResponse res;
    res.sessionId = "session-789";
    res.fileTokens.insert("file-1", "token-abc");

    QByteArray encRes = ProtocolJson::encodePrepareResponse(res, &err);
    ASSERT_TRUE(!encRes.isEmpty(), "encodePrepareResponse produced empty output");

    PrepareUploadResponse decRes;
    ok = ProtocolJson::decodePrepareResponse(encRes, &decRes, &err);
    ASSERT_TRUE(ok, err.toLocal8Bit().constData());
    ASSERT_TRUE(decRes.sessionId == "session-789", "sessionId mismatch");
    ASSERT_TRUE(decRes.fileTokens.value("file-1") == "token-abc", "token mismatch");

    TEST_PASS();
}

// ==========================================
// T02 HTTP Server & Stream Parser Tests
// ==========================================

void testHttpOneByteHeaderFragmentation()
{
    TEST_CASE("HTTP - One-byte header fragmentation");
    HttpServer server;
    QString err;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0, &err), "server listen failed");
    quint16 port = server.serverPort();

    TestHttpReceiver receiver;
    QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)),
                     &receiver, SLOT(onConnection(HttpConnection*)));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, port);
    ASSERT_TRUE(client.waitForConnected(1000), "client connect failed");

    QByteArray request = "POST /api/test HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
    for (int i = 0; i < request.size(); ++i) {
        client.write(request.mid(i, 1));
        client.flush();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(1000);
    }

    ASSERT_TRUE(waitForCondition(&receiver.headersReceived), "headersReady not emitted");
    ASSERT_TRUE(receiver.lastHead.method == "POST", "method mismatch");
    ASSERT_TRUE(receiver.lastHead.contentLength == 5, "contentLength mismatch");

    // Body streaming requires allowBody()
    receiver.lastConn->allowBody();
    ASSERT_TRUE(waitForCondition(&receiver.requestFinishedReceived), "requestFinished not emitted");
    ASSERT_TRUE(receiver.receivedBody == "hello", "body mismatch");

    // Test response
    receiver.lastConn->respond(200, "text/plain", "OK");
    ASSERT_TRUE(client.waitForReadyRead(1000), "client did not receive response");
    QByteArray clientResp = client.readAll();
    ASSERT_TRUE(clientResp.contains("HTTP/1.1 200 OK"), "expected 200 OK");
    ASSERT_TRUE(clientResp.contains("Connection: close"), "expected Connection: close");
    ASSERT_TRUE(clientResp.endsWith("OK"), "expected body OK");

    server.stop();
    TEST_PASS();
}

void testHttpHeadersAndBodyTogether()
{
    TEST_CASE("HTTP - Headers and body in same packet");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    TestHttpReceiver receiver;
    QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)),
                     &receiver, SLOT(onConnection(HttpConnection*)));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(client.waitForConnected(1000), "connect failed");

    QByteArray request = "POST /api/upload HTTP/1.1\r\nContent-Length: 9\r\n\r\n123456789";
    client.write(request);
    client.flush();

    ASSERT_TRUE(waitForCondition(&receiver.headersReceived), "headersReady not emitted");
    receiver.lastConn->allowBody();
    ASSERT_TRUE(waitForCondition(&receiver.requestFinishedReceived), "requestFinished not emitted");
    ASSERT_TRUE(receiver.receivedBody == "123456789", "body mismatch");

    server.stop();
    TEST_PASS();
}

void testHttpContentLengthZero()
{
    TEST_CASE("HTTP - Content-Length: 0 completes immediately without body wait");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    TestHttpReceiver receiver;
    QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)),
                     &receiver, SLOT(onConnection(HttpConnection*)));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(client.waitForConnected(1000), "connect failed");

    QByteArray request = "GET /api/info HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    client.write(request);
    client.flush();

    ASSERT_TRUE(waitForCondition(&receiver.headersReceived), "headersReady not emitted");
    ASSERT_TRUE(waitForCondition(&receiver.requestFinishedReceived), "requestFinished not emitted");

    server.stop();
    TEST_PASS();
}

void testHttpChunkedTransfer()
{
    TEST_CASE("HTTP - Transfer-Encoding: chunked with trailers");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    TestHttpReceiver receiver;
    QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)),
                     &receiver, SLOT(onConnection(HttpConnection*)));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(client.waitForConnected(1000), "connect failed");

    QByteArray reqHeaders = "POST /api/chunked HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    client.write(reqHeaders);
    client.flush();

    ASSERT_TRUE(waitForCondition(&receiver.headersReceived), "headersReady not emitted");
    ASSERT_TRUE(receiver.lastHead.chunked, "head.chunked must be true");

    receiver.lastConn->allowBody();

    // Send chunks: "Hello" (size 5), " World!" (size 7), last chunk 0 with trailer
    client.write("5\r\nHello\r\n");
    client.flush();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    client.write("7;ext=foo\r\n World!\r\n");
    client.flush();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    client.write("0\r\nX-Checksum: abc\r\n\r\n");
    client.flush();

    ASSERT_TRUE(waitForCondition(&receiver.requestFinishedReceived), "requestFinished not emitted");
    ASSERT_TRUE(receiver.receivedBody == "Hello World!", "chunked body mismatch");

    server.stop();
    TEST_PASS();
}

void testHttpExpect100Continue()
{
    TEST_CASE("HTTP - Expect: 100-continue handshake wait and emission");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    TestHttpReceiver receiver;
    QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)),
                     &receiver, SLOT(onConnection(HttpConnection*)));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(client.waitForConnected(1000), "connect failed");

    QByteArray reqHeaders = "POST /api/data HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n";
    client.write(reqHeaders);
    client.flush();

    ASSERT_TRUE(waitForCondition(&receiver.headersReceived), "headersReady not emitted");

    // Client should NOT have received 100 yet before allowBody()
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    ASSERT_TRUE(client.bytesAvailable() == 0, "100 continue sent prematurely");

    // Now server allows body
    receiver.lastConn->allowBody();
    ASSERT_TRUE(client.waitForReadyRead(1000), "client did not receive 100 Continue");
    QByteArray interim = client.readAll();
    ASSERT_TRUE(interim.contains("HTTP/1.1 100 Continue"), "expected 100 Continue");

    // Client sends the 4 bytes
    client.write("abcd");
    client.flush();

    ASSERT_TRUE(waitForCondition(&receiver.requestFinishedReceived), "requestFinished not emitted");
    ASSERT_TRUE(receiver.receivedBody == "abcd", "body mismatch");

    server.stop();
    TEST_PASS();
}

void testHttpClTeConflictRejected()
{
    TEST_CASE("HTTP - CL+TE conflict rejected with 400");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    TestHttpReceiver receiver;
    QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)),
                     &receiver, SLOT(onConnection(HttpConnection*)));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketConnected(&client), "connect failed");

    QByteArray attackReq = "POST /api/test HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n";
    client.write(attackReq);
    client.flush();

    ASSERT_TRUE(waitSocketReadyRead(&client), "expected 400 response");
    QByteArray resp = client.readAll();
    ASSERT_TRUE(resp.contains("HTTP/1.1 400 Bad Request"), "must reject CL+TE conflict with 400");

    server.stop();
    TEST_PASS();
}

void testHttpUnsupportedTransferEncodingRejected()
{
    TEST_CASE("HTTP - Unsupported Transfer-Encoding (gzip) rejected with 501");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketConnected(&client), "connect failed");

    QByteArray gzipReq = "POST /api/test HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n";
    client.write(gzipReq);
    client.flush();

    ASSERT_TRUE(waitSocketReadyRead(&client), "expected 501 response");
    QByteArray resp = client.readAll();
    ASSERT_TRUE(resp.contains("HTTP/1.1 501 Not Implemented"), "must reject non-chunked Transfer-Encoding with 501");

    server.stop();
    TEST_PASS();
}

void testHttpOversizedHeaderRejected()
{
    TEST_CASE("HTTP - Oversized headers (>16 KiB) rejected with 431");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketConnected(&client), "connect failed");

    QByteArray bigHeader = "GET / HTTP/1.1\r\nX-Long: ";
    bigHeader.append(QByteArray(18000, 'A'));
    bigHeader.append("\r\n\r\n");

    client.write(bigHeader);
    client.flush();

    ASSERT_TRUE(waitSocketReadyRead(&client), "expected 431 response");
    QByteArray resp = client.readAll();
    ASSERT_TRUE(resp.contains("HTTP/1.1 431 Request Header Fields Too Large"), "must return 431");

    server.stop();
    TEST_PASS();
}

void testHttpConcurrentConnectionsNonBlocking()
{
    TEST_CASE("HTTP - Concurrent connections: pending prepare does not block other connections");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    ConnsReceiver connRecv;
    QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)),
                     &connRecv, SLOT(onConn(HttpConnection*)));

    // Client 1 connects and sends prepare request
    QTcpSocket client1;
    client1.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketConnected(&client1), "client1 connect failed");
    client1.write("POST /api/prepare HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    client1.flush();

    QTime t;
    t.start();
    while (connRecv.conns.isEmpty() && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(!connRecv.conns.isEmpty(), "server did not accept client1");
    HttpConnection *conn1 = connRecv.conns.first();

    // Client 2 connects while client 1 is pending response!
    QTcpSocket client2;
    client2.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketConnected(&client2), "client2 connect failed");
    client2.write("POST /api/register HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    client2.flush();

    t.start();
    while (connRecv.conns.size() < 2 && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(connRecv.conns.size() == 2, "server did not accept client2 concurrently");
    HttpConnection *conn2 = connRecv.conns.at(1);

    // Client 2 completes and responds immediately
    conn2->respond(200, "application/json", "{\"status\":\"ok\"}");
    ASSERT_TRUE(waitSocketReadyRead(&client2), "client2 response not received");
    QByteArray resp2 = client2.readAll();
    ASSERT_TRUE(resp2.contains("200 OK"), "client2 expected 200");

    // Client 1 is still pending and now responds
    conn1->respond(200, "application/json", "{\"sessionId\":\"123\"}");
    ASSERT_TRUE(waitSocketReadyRead(&client1), "client1 response not received");
    QByteArray resp1 = client1.readAll();
    ASSERT_TRUE(resp1.contains("200 OK"), "client1 expected 200");

    server.stop();
    TEST_PASS();
}

void testHttpConnectionLimit()
{
    TEST_CASE("HTTP - Socket budget (max 8) rejects 9th connection with 503");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    QList<QTcpSocket*> clients;
    for (int i = 0; i < HttpServer::MAX_ACTIVE_CONNECTIONS; ++i) {
        QTcpSocket *c = new QTcpSocket();
        c->connectToHost(QHostAddress::LocalHost, server.serverPort());
        ASSERT_TRUE(waitSocketConnected(c), "client connect failed");
        c->write("POST /test HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
        c->flush();
        clients.append(c);
    }

    QTime t;
    t.start();
    while (server.activeConnectionsCount() < HttpServer::MAX_ACTIVE_CONNECTIONS && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(server.activeConnectionsCount() == HttpServer::MAX_ACTIVE_CONNECTIONS, "all 8 connections active");

    // 9th connection attempts to connect
    QTcpSocket client9;
    client9.connectToHost(QHostAddress::LocalHost, server.serverPort());

    ASSERT_TRUE(waitSocketReadyRead(&client9), "expected 503 from client9");
    QByteArray resp = client9.readAll();
    ASSERT_TRUE(resp.contains("503 Service Unavailable"), "expected 503 Service Unavailable");

    for (int i = 0; i < clients.size(); ++i) {
        delete clients.at(i);
    }
    server.stop();
    TEST_PASS();
}

// ==========================================
// T03 Application & Identity Tests
// ==========================================

void testAppIdentityAndSettingsPersistence()
{
    TEST_CASE("Application - Identity persistence across restarts");
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.clear();
    }

    QString fp1, alias1;
    {
        localsend::Application app1;
        fp1 = app1.localInfo().fingerprint;
        alias1 = app1.localInfo().alias;
        ASSERT_TRUE(!fp1.isEmpty(), "fingerprint should not be empty");
        ASSERT_TRUE(!alias1.isEmpty(), "alias should not be empty");

        app1.setAlias("Custom Alias");
        app1.setPort(54321);
    }

    {
        localsend::Application app2;
        ASSERT_TRUE(app2.localInfo().fingerprint == fp1, "fingerprint must persist across restarts");
        ASSERT_TRUE(app2.localInfo().alias == "Custom Alias", "alias must persist across restarts");
        ASSERT_TRUE(app2.localInfo().port == 54321, "port must persist across restarts");

        QString err;
        QByteArray ann = ProtocolJson::encodeAnnouncement(app2.localInfo(), true, &err);
        ASSERT_TRUE(ann.contains("\"port\":54321"), "announcement must contain updated port");
    }

    TEST_PASS();
}

void testAppTransferSlotMutex()
{
    TEST_CASE("Application - Single transfer slot mutual exclusion");
    localsend::Application app;

    ASSERT_FALSE(app.isTransferActive(), "initially transfer slot must be free");
    ASSERT_TRUE(app.beginTransfer("transfer-A"), "beginTransfer transfer-A should succeed");
    ASSERT_TRUE(app.isTransferActive(), "transfer slot must be active");
    ASSERT_TRUE(app.activeTransferId() == "transfer-A", "active transfer id mismatch");

    // Second transfer must be rejected (single slot)
    ASSERT_FALSE(app.beginTransfer("transfer-B"), "concurrent transfer must be rejected");

    // Release with wrong owner should NOT release slot
    app.endTransfer("transfer-wrong");
    ASSERT_TRUE(app.isTransferActive(), "wrong owner must not release slot");

    // Release with matching owner
    app.endTransfer("transfer-A");
    ASSERT_FALSE(app.isTransferActive(), "transfer slot must be released");

    // Now second transfer can acquire slot
    ASSERT_TRUE(app.beginTransfer("transfer-B"), "beginTransfer transfer-B should now succeed");
    app.endTransfer("transfer-B");
    ASSERT_FALSE(app.isTransferActive(), "slot must be free");

    TEST_PASS();
}

void testAppOccupiedPortStartupFailure()
{
    TEST_CASE("Application - Occupied port reports startup failure cleanly");
    QTcpServer blocker;
    ASSERT_TRUE(blocker.listen(QHostAddress::Any, 0), "blocker server listen failed");
    quint16 blockedPort = blocker.serverPort();

    localsend::Application app;
    app.setPort(blockedPort);

    QString err;
    bool ok = app.start(&err);
    ASSERT_FALSE(ok, "start() must fail when port is occupied");
    ASSERT_TRUE(!err.isEmpty(), "error message must be provided");

    blocker.close();
    TEST_PASS();
}

void testAppLifecycleStartStop()
{
    TEST_CASE("Application - Clean lifecycle start/stop without socket leakage");
    localsend::Application app;
    app.setPort(0);

    QString err;
    ASSERT_TRUE(app.start(&err), "app start 1 failed");
    ASSERT_TRUE(app.httpServer()->isListening(), "httpServer must be listening");

    app.stop();
    ASSERT_FALSE(app.httpServer()->isListening(), "httpServer must stop listening");

    ASSERT_TRUE(app.start(&err), "app restart failed");
    ASSERT_TRUE(app.httpServer()->isListening(), "httpServer must be listening again");

    app.stop();
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.remove("network/protocol");
        s.sync();
    }
    TEST_PASS();
}

// ==========================================
// T04 UDP Multicast & Discovery Tests
// ==========================================

class TestPeerTracker : public QObject {
    Q_OBJECT
public:
    QList<PeerDevice> updatedPeers;
    QString lastError;

    TestPeerTracker(QObject *parent = 0) : QObject(parent) {}

public slots:
    void onPeerUpdated(const PeerDevice &peer)
    {
        updatedPeers.append(peer);
    }
    void onDiscoveryError(const QString &err)
    {
        lastError = err;
    }
};

class MockRegisterHandler : public QObject {
    Q_OBJECT
public:
    DeviceInfo receivedReq;
    bool registerReceived;
    MockRegisterHandler(QObject *parent = 0)
        : QObject(parent), registerReceived(false), m_conn(0) {}

public slots:
    void onConnection(HttpConnection *conn)
    {
        connect(conn, SIGNAL(headersReady(HttpRequestHead)), this, SLOT(onHeaders(HttpRequestHead)));
        connect(conn, SIGNAL(bodyChunk(QByteArray)), this, SLOT(onBody(QByteArray)));
        connect(conn, SIGNAL(requestFinished()), this, SLOT(onFinished()));
        m_conn = conn;
        m_body.clear();
    }
    void onHeaders(const HttpRequestHead &head)
    {
        m_head = head;
        m_conn->allowBody();
    }
    void onBody(const QByteArray &chunk)
    {
        m_body.append(chunk);
    }
    void onFinished()
    {
        QString err;
        if (ProtocolJson::decodeRegisterRequest(m_body, &receivedReq, &err)) {
            registerReceived = true;
            DeviceInfo resInfo;
            resInfo.alias = "PC Mock Peer";
            resInfo.version = "2.2";
            resInfo.deviceModel = "Linux";
            resInfo.deviceType = "desktop";
            resInfo.fingerprint = "pc-mock-fp";
            resInfo.download = false;
            QByteArray resBody = ProtocolJson::encodeDeviceResponse(resInfo, &err);
            m_conn->respond(200, "application/json", resBody);
        }
    }

private:
    HttpConnection *m_conn;
    HttpRequestHead m_head;
    QByteArray m_body;
};

class MockPeerReceiver : public QObject {
    Q_OBJECT
public:
    int prepareStatusCode;
    int uploadStatusCode;
    bool holdUpload;
    PrepareUploadRequest receivedPrepareReq;
    QByteArray receivedUploadData;
    HttpRequestHead uploadHead;
    QString cancelReceivedSession;

    MockPeerReceiver(QObject *parent = 0)
        : QObject(parent)
        , prepareStatusCode(200)
        , uploadStatusCode(200)
        , holdUpload(false)
    {}

public slots:
    void onConnection(HttpConnection *conn)
    {
        connect(conn, SIGNAL(headersReady(HttpRequestHead)), this, SLOT(onHeaders(HttpRequestHead)));
        connect(conn, SIGNAL(bodyChunk(QByteArray)), this, SLOT(onBody(QByteArray)));
        connect(conn, SIGNAL(requestFinished()), this, SLOT(onFinished()));
    }

    void onHeaders(const HttpRequestHead &head)
    {
        HttpConnection *conn = qobject_cast<HttpConnection*>(sender());
        if (!conn) return;

        QString path = head.target.path();
        if (path == "/api/localsend/v2/prepare-upload") {
            conn->allowBody();
        } else if (path == "/api/localsend/v2/upload") {
            uploadHead = head;
            if (uploadStatusCode != 200 && uploadStatusCode != 422) {
                conn->respond(uploadStatusCode, "text/plain", "Upload Error");
                return;
            }
            conn->allowBody();
        } else if (path == "/api/localsend/v2/cancel") {
            cancelReceivedSession = getUrlQueryItem(head.target, "sessionId");
            conn->respond(200, "text/plain", "");
        }
    }

    void onBody(const QByteArray &chunk)
    {
        HttpConnection *conn = qobject_cast<HttpConnection*>(sender());
        if (!conn) return;

        QString path = conn->requestHead().target.path();
        if (path == "/api/localsend/v2/upload") {
            receivedUploadData.append(chunk);
        } else if (path == "/api/localsend/v2/prepare-upload") {
            m_prepareBody.append(chunk);
        }
    }

    void onFinished()
    {
        HttpConnection *conn = qobject_cast<HttpConnection*>(sender());
        if (!conn) return;

        QString path = conn->requestHead().target.path();
        if (path == "/api/localsend/v2/prepare-upload") {
            QString err;
            ProtocolJson::decodePrepareRequest(m_prepareBody, &receivedPrepareReq, &err);
            if (prepareStatusCode == 200) {
                PrepareUploadResponse resp;
                resp.sessionId = "mock-sess-1";
                if (!receivedPrepareReq.files.isEmpty()) {
                    QString fileId = receivedPrepareReq.files.keys().first();
                    resp.fileTokens.insert(fileId, "mock-tok-1");
                }
                QByteArray resBody = ProtocolJson::encodePrepareResponse(resp, &err);
                conn->respond(200, "application/json", resBody);
            } else if (prepareStatusCode == 204) {
                conn->respond(204, "text/plain", "");
            } else {
                conn->respond(prepareStatusCode, "text/plain", "Prepare Error");
            }
        } else if (path == "/api/localsend/v2/upload") {
            if (!holdUpload) {
                conn->respond(uploadStatusCode, "text/plain", "");
            }
        }
    }

private:
    QByteArray m_prepareBody;
};

class TestSendTracker : public QObject {
    Q_OBJECT
public:
    QStringList states;
    qint64 lastProgressDone;
    qint64 lastProgressTotal;
    bool finished;
    QString failedReason;
    int failedCode;

    TestSendTracker(QObject *parent = 0)
        : QObject(parent), lastProgressDone(0), lastProgressTotal(0), finished(false), failedCode(0) {}

public slots:
    void onStateChanged(const QString &reqId, const QString &state) {
        Q_UNUSED(reqId);
        states.append(state);
    }
    void onProgress(const QString &reqId, qint64 done, qint64 total) {
        Q_UNUSED(reqId);
        if (done > lastProgressDone) {
            lastProgressDone = done;
        }
        if (total > lastProgressTotal) {
            lastProgressTotal = total;
        }
    }
    void onFinished(const QString &reqId) {
        Q_UNUSED(reqId);
        finished = true;
    }
    void onFailed(const QString &reqId, const QString &reason) {
        Q_UNUSED(reqId);
        failedReason = reason;
    }
    void onFailedWithReason(const QString &reqId, const QString &reason, int code) {
        Q_UNUSED(reqId);
        Q_UNUSED(reason);
        failedCode = code;
    }
};

class AutoAcceptOfferHandler : public QObject {
    Q_OBJECT
public:
    ReceiveServer *server;
    bool offerReceived;
    QString acceptedFileId;
    QString receivedPath;

    AutoAcceptOfferHandler(ReceiveServer *srv, QObject *parent = 0)
        : QObject(parent), server(srv), offerReceived(false) {}

public slots:
    void onIncomingOffer(const QString &requestId, const PeerDevice &peer, const QList<FileMeta> &files)
    {
        Q_UNUSED(peer);
        offerReceived = true;
        if (!files.isEmpty() && server) {
            acceptedFileId = files.first().id;
            server->accept(requestId, acceptedFileId);
        }
    }
    void onReceiveFinished(const QString &sessionId, const QString &savedPath)
    {
        Q_UNUSED(sessionId);
        receivedPath = savedPath;
    }
};

class RxBodyTracker : public QObject {
    Q_OBJECT
public:
    qint64 bytes;
    RxBodyTracker(QObject *parent = 0) : QObject(parent), bytes(0) {}
public slots:
    void onProgress(const QString &sessId, qint64 done, qint64 total) {
        Q_UNUSED(sessId);
        Q_UNUSED(total);
        bytes = done;
    }
};

class RejectOfferHandler : public QObject {
    Q_OBJECT
public:
    ReceiveServer *server;
    RejectOfferHandler(ReceiveServer *s = 0, QObject *parent = 0) : QObject(parent), server(s) {}
public slots:
    void onIncomingOffer(const QString &reqId, const PeerDevice &peer, const QList<FileMeta> &files) {
        Q_UNUSED(peer);
        Q_UNUSED(files);
        if (server) {
            server->reject(reqId);
        }
    }
};

class RxMidStreamCancelHandler : public QObject {
    Q_OBJECT
public:
    ReceiveServer *server;
    bool cancelled;
    RxMidStreamCancelHandler(ReceiveServer *s = 0, QObject *parent = 0)
        : QObject(parent), server(s), cancelled(false) {}
public slots:
    void onProgress(const QString &sessId, qint64 done, qint64 total) {
        Q_UNUSED(total);
        if (done > 0 && !cancelled && server) {
            cancelled = true;
            server->cancelLocal(sessId);
        }
    }
};

class TxMidStreamCancelHandler : public QObject {
    Q_OBJECT
public:
    SendManager *sendManager;
    bool cancelled;
    bool sawUploading;
    qint64 bytesAtCancel;

    TxMidStreamCancelHandler(SendManager *sm = 0, QObject *parent = 0)
        : QObject(parent), sendManager(sm), cancelled(false), sawUploading(false), bytesAtCancel(0) {}

public slots:
    void onReceiveProgress(const QString &sessId, qint64 done, qint64 total) {
        Q_UNUSED(sessId);
        Q_UNUSED(total);
        if (done > 0 && !cancelled && sendManager) {
            if (sendManager->state() == SendManager::State_Uploading) {
                sawUploading = true;
                cancelled = true;
                bytesAtCancel = done;
                sendManager->cancel();
            }
        }
    }
};

void testDiscoverySelfIgnored()
{
    TEST_CASE("Discovery - Self-announcements ignored without duplicate entry");
    QNetworkAccessManager nam;
    DiscoveryManager dm(&nam);

    DeviceInfo localInfo;
    localInfo.alias = "Local BB10";
    localInfo.fingerprint = "fp-local-unique";
    localInfo.port = 53317;

    dm.start(localInfo);

    TestPeerTracker tracker;
    QObject::connect(&dm, SIGNAL(peerUpdated(PeerDevice)), &tracker, SLOT(onPeerUpdated(PeerDevice)));

    QString err;
    QByteArray selfAnn = ProtocolJson::encodeAnnouncement(localInfo, true, &err);

    QUdpSocket sender;
    sender.writeDatagram(selfAnn, QHostAddress::LocalHost, 53317);

    QTime t;
    t.start();
    while (t.elapsed() < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(tracker.updatedPeers.isEmpty(), "self-announcement must be ignored");
    ASSERT_TRUE(dm.peers().isEmpty(), "peer list must be empty");

    dm.stop();
    TEST_PASS();
}

void testDiscoveryRepeatedAnnouncementsOneRow()
{
    TEST_CASE("Discovery - Repeated announcements update peer without duplicates");
    QNetworkAccessManager nam;
    DiscoveryManager dm(&nam);

    DeviceInfo localInfo;
    localInfo.fingerprint = "fp-local";
    dm.start(localInfo);

    DeviceInfo peerInfo;
    peerInfo.alias = "Remote Peer";
    peerInfo.fingerprint = "fp-remote-999";
    peerInfo.port = 53317;
    peerInfo.protocol = "http";
    peerInfo.version = "2.2";

    // Simulate 20 announcements with announce: false (so no HTTP register is fired)
    QString err;
    QByteArray ann = ProtocolJson::encodeAnnouncement(peerInfo, false, &err);

    QUdpSocket sender;
    for (int i = 0; i < 20; ++i) {
        sender.writeDatagram(ann, QHostAddress::LocalHost, 53317);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    QTime t;
    t.start();
    while (dm.peers().isEmpty() && t.elapsed() < 500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(dm.peers().size() == 1, "exactly one peer row must exist");
    ASSERT_TRUE(dm.findPeer("fp-remote-999").info.alias == "Remote Peer", "peer alias mismatch");

    dm.stop();
    TEST_PASS();
}

void testDiscoveryMalformedAndOversizedUdpSafe()
{
    TEST_CASE("Discovery - Malformed and oversized UDP datagrams handled safely");
    QNetworkAccessManager nam;
    DiscoveryManager dm(&nam);

    DeviceInfo localInfo;
    localInfo.fingerprint = "fp-local";
    dm.start(localInfo);

    QUdpSocket sender;
    sender.writeDatagram("not-valid-json-datagram", QHostAddress::LocalHost, 53317);

    QByteArray bigData(70000, 'X');
    sender.writeDatagram(bigData, QHostAddress::LocalHost, 53317);

    QTime t;
    t.start();
    while (t.elapsed() < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(dm.peers().isEmpty(), "malformed packets must not create peers");

    dm.stop();
    TEST_PASS();
}

void testDiscoveryManualRegisterHttp()
{
    TEST_CASE("Discovery - Manual HTTP register discovers peer and records observed IP/port");
    HttpServer mockPeerServer;
    ASSERT_TRUE(mockPeerServer.listen(QHostAddress::LocalHost, 0), "mock peer listen failed");
    quint16 mockPort = mockPeerServer.serverPort();

    MockRegisterHandler mockHandler;
    QObject::connect(&mockPeerServer, SIGNAL(connectionOpened(HttpConnection*)),
                     &mockHandler, SLOT(onConnection(HttpConnection*)));

    QNetworkAccessManager nam;
    DiscoveryManager dm(&nam);
    DeviceInfo localInfo;
    localInfo.alias = "Local BB10";
    localInfo.fingerprint = "bb10-fp";
    dm.start(localInfo);

    TestPeerTracker tracker;
    QObject::connect(&dm, SIGNAL(peerUpdated(PeerDevice)), &tracker, SLOT(onPeerUpdated(PeerDevice)));

    dm.registerPeer(QHostAddress::LocalHost, mockPort);

    QTime t;
    t.start();
    while (tracker.updatedPeers.isEmpty() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(mockHandler.registerReceived, "mock peer must receive register request");
    ASSERT_TRUE(mockHandler.receivedReq.alias == "Local BB10", "request alias mismatch");
    ASSERT_TRUE(!tracker.updatedPeers.isEmpty(), "peerUpdated must be emitted on register response");

    PeerDevice discovered = dm.findPeer("pc-mock-fp");
    ASSERT_TRUE(discovered.info.alias == "PC Mock Peer", "discovered alias mismatch");
    ASSERT_TRUE(discovered.info.port == mockPort, "discovered port must match target port");
    ASSERT_TRUE(discovered.info.protocol == "http", "discovered protocol must match target protocol");

    mockPeerServer.stop();
    dm.stop();
    TEST_PASS();
}

void testSendNonExistentFileFails()
{
    TEST_CASE("Sender - Non-existent local file fails with LocalFileNotFound");
    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = 53317;
    peer.info.protocol = "http";

    TestSendTracker tracker;
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

    QString err;
    bool ok = sm.sendFile(peer, "/tmp/definitely_not_existing_file_12345.bin", &err);
    ASSERT_FALSE(ok, "sendFile must return false for non-existent file");
    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_LocalFileNotFound, "failure reason must be LocalFileNotFound");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must remain free");
    TEST_PASS();
}

void testSendHttpsPeerRejected()
{
    TEST_CASE("Sender - HTTPS peer rejected before sending plaintext");
    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = 53317;
    peer.info.protocol = "https"; // HTTPS peer

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "temporary file open failed");
    tmpFile.write("test data");
    tmpFile.flush();

    QString err;
    bool ok = sm.sendFile(peer, tmpFile.fileName(), &err);
    ASSERT_FALSE(ok, "sendFile must reject HTTPS peer");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must remain free");
    TEST_PASS();
}

void testSendSuccessfulUpload()
{
    TEST_CASE("Sender - Full upload flow: prepare 200, streaming upload, peer 200, completed");
    HttpServer mockServer;
    ASSERT_TRUE(mockServer.listen(QHostAddress::LocalHost, 0), "mock server listen failed");
    quint16 port = mockServer.serverPort();

    MockPeerReceiver mockPeer;
    QObject::connect(&mockServer, SIGNAL(connectionOpened(HttpConnection*)),
                     &mockPeer, SLOT(onConnection(HttpConnection*)));

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "temporary file open failed");
    QByteArray payload(16384, 'K'); // 16 KiB
    tmpFile.write(payload);
    tmpFile.flush();

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = port;
    peer.info.protocol = "http";

    TestSendTracker tracker;
    QObject::connect(&sm, SIGNAL(sendStateChanged(QString,QString)), &tracker, SLOT(onStateChanged(QString,QString)));
    QObject::connect(&sm, SIGNAL(sendProgress(QString,qint64,qint64)), &tracker, SLOT(onProgress(QString,qint64,qint64)));
    QObject::connect(&sm, SIGNAL(sendFinished(QString)), &tracker, SLOT(onFinished(QString)));
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

    QString err;
    bool ok = sm.sendFile(peer, tmpFile.fileName(), &err);
    ASSERT_TRUE(ok, "sendFile must succeed");

    QTime t;
    t.start();
    while (!tracker.finished && tracker.failedReason.isEmpty() && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(tracker.finished, "sendFinished must be emitted");
    ASSERT_TRUE(tracker.failedReason.isEmpty(), "send must not fail");
    ASSERT_TRUE(sm.state() == SendManager::State_Completed, "state must be Completed");
    ASSERT_TRUE(mockPeer.receivedUploadData == payload, "uploaded data must match file payload");
    ASSERT_TRUE(tracker.lastProgressDone == 16384, "progress done must reach 16384");
    ASSERT_TRUE(tracker.lastProgressTotal == 16384, "progress total must be 16384");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must be released after completion");

    mockServer.stop();
    TEST_PASS();
}

void testSendPeerRejected403()
{
    TEST_CASE("Sender - Peer rejected prepare (403) aborts and reports PeerRejected");
    HttpServer mockServer;
    ASSERT_TRUE(mockServer.listen(QHostAddress::LocalHost, 0), "mock server listen failed");
    quint16 port = mockServer.serverPort();

    MockPeerReceiver mockPeer;
    mockPeer.prepareStatusCode = 403; // Reject
    QObject::connect(&mockServer, SIGNAL(connectionOpened(HttpConnection*)),
                     &mockPeer, SLOT(onConnection(HttpConnection*)));

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "temporary file open failed");
    tmpFile.write("payload");
    tmpFile.flush();

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = port;
    peer.info.protocol = "http";

    TestSendTracker tracker;
    QObject::connect(&sm, SIGNAL(sendFinished(QString)), &tracker, SLOT(onFinished(QString)));
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));
    QObject::connect(&sm, SIGNAL(sendFailedWithReason(QString,QString,int)), &tracker, SLOT(onFailedWithReason(QString,QString,int)));

    sm.sendFile(peer, tmpFile.fileName());

    QTime t;
    t.start();
    while (tracker.failedReason.isEmpty() && !tracker.finished && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(!tracker.finished, "send must not finish successfully");
    ASSERT_TRUE(!tracker.failedReason.isEmpty(), "sendFailed must be emitted");
    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_PeerRejected, "failureReason must be PeerRejected");
    ASSERT_TRUE(tracker.failedCode == SendManager::Failure_PeerRejected, "failedCode must match PeerRejected");
    ASSERT_TRUE(sm.state() == SendManager::State_Failed, "state must be Failed");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must be released");

    mockServer.stop();
    TEST_PASS();
}

void testSendPeerBusy409()
{
    TEST_CASE("Sender - Peer busy (409) aborts and reports PeerBusy");
    HttpServer mockServer;
    ASSERT_TRUE(mockServer.listen(QHostAddress::LocalHost, 0), "mock server listen failed");
    quint16 port = mockServer.serverPort();

    MockPeerReceiver mockPeer;
    mockPeer.prepareStatusCode = 409; // Busy
    QObject::connect(&mockServer, SIGNAL(connectionOpened(HttpConnection*)),
                     &mockPeer, SLOT(onConnection(HttpConnection*)));

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "temporary file open failed");
    tmpFile.write("payload");
    tmpFile.flush();

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = port;
    peer.info.protocol = "http";

    TestSendTracker tracker;
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

    sm.sendFile(peer, tmpFile.fileName());

    QTime t;
    t.start();
    while (tracker.failedReason.isEmpty() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_PeerBusy, "failureReason must be PeerBusy");
    ASSERT_TRUE(sm.state() == SendManager::State_Failed, "state must be Failed");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must be released");

    mockServer.stop();
    TEST_PASS();
}

void testSendPeerChecksumMismatch422()
{
    TEST_CASE("Sender - Peer reports checksum mismatch (422) on upload");
    HttpServer mockServer;
    ASSERT_TRUE(mockServer.listen(QHostAddress::LocalHost, 0), "mock server listen failed");
    quint16 port = mockServer.serverPort();

    MockPeerReceiver mockPeer;
    mockPeer.uploadStatusCode = 422; // Checksum mismatch
    QObject::connect(&mockServer, SIGNAL(connectionOpened(HttpConnection*)),
                     &mockPeer, SLOT(onConnection(HttpConnection*)));

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "temporary file open failed");
    tmpFile.write("checksum_payload");
    tmpFile.flush();

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = port;
    peer.info.protocol = "http";

    TestSendTracker tracker;
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

    sm.sendFile(peer, tmpFile.fileName());

    QTime t;
    t.start();
    while (tracker.failedReason.isEmpty() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_ChecksumMismatch, "failureReason must be ChecksumMismatch");
    ASSERT_TRUE(sm.state() == SendManager::State_Failed, "state must be Failed");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must be released");

    mockServer.stop();
    TEST_PASS();
}

void testSendPeerServerInternalError500()
{
    TEST_CASE("Sender - Peer internal server error (500) reports NetworkError and releases slot");
    HttpServer mockServer;
    ASSERT_TRUE(mockServer.listen(QHostAddress::LocalHost, 0), "mock server listen failed");
    quint16 port = mockServer.serverPort();

    MockPeerReceiver mockPeer;
    mockPeer.uploadStatusCode = 500;
    QObject::connect(&mockServer, SIGNAL(connectionOpened(HttpConnection*)),
                     &mockPeer, SLOT(onConnection(HttpConnection*)));

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "temporary file open failed");
    tmpFile.write("error_payload");
    tmpFile.flush();

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = port;
    peer.info.protocol = "http";

    TestSendTracker tracker;
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

    sm.sendFile(peer, tmpFile.fileName());

    QTime t;
    t.start();
    while (tracker.failedReason.isEmpty() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_NetworkError, "failureReason must be NetworkError");
    ASSERT_TRUE(sm.state() == SendManager::State_Failed, "state must be Failed");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must be released");

    mockServer.stop();
    TEST_PASS();
}

void testSendLocalCancellation()
{
    TEST_CASE("Sender - Local cancellation stops IO, notifies peer, and frees slot");
    HttpServer mockServer;
    ASSERT_TRUE(mockServer.listen(QHostAddress::LocalHost, 0), "mock server listen failed");
    quint16 port = mockServer.serverPort();

    MockPeerReceiver mockPeer;
    mockPeer.holdUpload = true; // Hold upload open
    QObject::connect(&mockServer, SIGNAL(connectionOpened(HttpConnection*)),
                     &mockPeer, SLOT(onConnection(HttpConnection*)));

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "temporary file open failed");
    QByteArray bigData(32768, 'Z');
    tmpFile.write(bigData);
    tmpFile.flush();

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = port;
    peer.info.protocol = "http";

    sm.sendFile(peer, tmpFile.fileName());

    // Wait until Uploading state
    QTime t;
    t.start();
    while (sm.state() != SendManager::State_Uploading && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(sm.state() == SendManager::State_Uploading, "state must be Uploading before cancel");
    QString activeSessionId = sm.activeSessionId();

    // Cancel locally
    sm.cancel();

    ASSERT_TRUE(sm.state() == SendManager::State_Cancelled, "state must be Cancelled");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must be released");

    // Pump events for remote cancel notification
    t.start();
    while (mockPeer.cancelReceivedSession.isEmpty() && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(mockPeer.cancelReceivedSession == activeSessionId, "remote peer must receive cancel with sessionId");

    mockServer.stop();
    TEST_PASS();
}

void testEndToEndBidirectionalTransfer()
{
    TEST_CASE("End-to-End - LocalSend transfer between SendManager and ReceiveServer with Chinese name");

    // 1. Setup Receiver
    localsend::Application rxApp;
    rxApp.setProtocol("http", 0, false);
    rxApp.setPort(0); // auto-assigned
    rxApp.setReceiveDirectory("/tmp/localsend_rx_test");
    QDir("/tmp/localsend_rx_test").removeRecursively();

    QString rxErr;
    ASSERT_TRUE(rxApp.start(&rxErr), "rxApp start failed");
    quint16 rxPort = rxApp.httpServer()->serverPort();

    AutoAcceptOfferHandler offerHandler(rxApp.receiveServer());
    QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &offerHandler, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));
    QObject::connect(rxApp.receiveServer(), SIGNAL(receiveFinished(QString,QString)),
                     &offerHandler, SLOT(onReceiveFinished(QString,QString)));

    // 2. Setup Sender & create test file with Chinese name
    QDir("/tmp/localsend_tx_test").mkpath(".");
    QString txFilePath = "/tmp/localsend_tx_test/测试传输文件.bin";
    QFile txFile(txFilePath);
    ASSERT_TRUE(txFile.open(QIODevice::WriteOnly), "txFile open failed");
    QByteArray testPayload(65536, 'E'); // 64 KiB test payload
    txFile.write(testPayload);
    txFile.close();

    localsend::Application txApp;
    QNetworkAccessManager nam;
    SendManager sm(&txApp, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = rxPort;
    peer.info.protocol = "http";

    TestSendTracker tracker;
    QObject::connect(&sm, SIGNAL(sendFinished(QString)), &tracker, SLOT(onFinished(QString)));
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

    // 3. Initiate Send
    QString sendErr;
    bool ok = sm.sendFile(peer, txFilePath, &sendErr);
    ASSERT_TRUE(ok, "sendFile must succeed");

    QTime t;
    t.start();
    while ((!tracker.finished || offerHandler.receivedPath.isEmpty()) && tracker.failedReason.isEmpty() && t.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(tracker.finished, "sender must finish");
    ASSERT_TRUE(tracker.failedReason.isEmpty(), "sender must not fail");
    ASSERT_TRUE(!offerHandler.receivedPath.isEmpty(), "receiver must finish and report saved path");

    // 4. Verify received file integrity
    QFile rxFile(offerHandler.receivedPath);
    ASSERT_TRUE(rxFile.exists(), "received file must exist on disk");
    ASSERT_TRUE(rxFile.open(QIODevice::ReadOnly), "received file must be readable");
    QByteArray rxPayload = rxFile.readAll();
    ASSERT_TRUE(rxPayload.size() == testPayload.size(), "received file size must match original");
    ASSERT_TRUE(rxPayload == testPayload, "received file content must match byte-for-byte");

    // Check SHA-256 match
    QString txSha = CryptoUtil::sha256File(txFilePath);
    QString rxSha = CryptoUtil::sha256File(offerHandler.receivedPath);
    ASSERT_TRUE(!txSha.isEmpty() && txSha == rxSha, "SHA-256 digests must match exactly");

    // Verify both transfer slots released
    ASSERT_TRUE(!rxApp.isTransferActive(), "receiver transfer slot must be released");
    ASSERT_TRUE(!txApp.isTransferActive(), "sender transfer slot must be released");

    rxApp.stop();
    txFile.remove();
    rxFile.remove();
    QDir("/tmp/localsend_rx_test").removeRecursively();
    QDir("/tmp/localsend_tx_test").removeRecursively();
    TEST_PASS();
}

// ==========================================
// R001 - R008 Review Issue Verification Tests
// ==========================================

class CallbackGuardReceiver : public QObject {
    Q_OBJECT
public:
    enum ActionOnHeaders { Header_Allow, Header_Reject403 };
    enum ActionOnChunk { Chunk_None, Chunk_Reject500 };

    ActionOnHeaders headerAction;
    ActionOnChunk chunkAction;
    int headersCount;
    int chunksCount;
    int finishCount;
    int disconnectCount;
    qint64 receivedBytes;
    HttpConnection *conn;

    CallbackGuardReceiver(QObject *parent = 0)
        : QObject(parent), headerAction(Header_Allow), chunkAction(Chunk_None)
        , headersCount(0), chunksCount(0), finishCount(0), disconnectCount(0), receivedBytes(0), conn(0) {}

public slots:
    void onConnection(HttpConnection *c) {
        conn = c;
        connect(c, SIGNAL(headersReady(HttpRequestHead)), this, SLOT(onHeaders(HttpRequestHead)));
        connect(c, SIGNAL(bodyChunk(QByteArray)), this, SLOT(onChunk(QByteArray)));
        connect(c, SIGNAL(requestFinished()), this, SLOT(onFinished()));
        connect(c, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
    }
    void onHeaders(const HttpRequestHead &) {
        headersCount++;
        if (headerAction == Header_Reject403) {
            conn->respond(403, "text/plain", "Forbidden");
        } else if (headerAction == Header_Allow) {
            conn->allowBody();
        }
    }
    void onChunk(const QByteArray &chunk) {
        chunksCount++;
        receivedBytes += chunk.size();
        if (chunkAction == Chunk_Reject500) {
            conn->respond(500, "text/plain", "Server Error");
        }
    }
    void onFinished() {
        finishCount++;
    }
    void onDisconnected() {
        disconnectCount++;
    }
};

void testHttpBoundedReadsAndCallbackGuards()
{
    TEST_CASE("R001/R002 - Bounded reads, callback state changes, and single disconnection");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    // Case 1: Synchronous 403 in headersReady must not emit bodyChunk or requestFinished
    {
        CallbackGuardReceiver recv;
        recv.headerAction = CallbackGuardReceiver::Header_Reject403;
        QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)), &recv, SLOT(onConnection(HttpConnection*)));

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, server.serverPort());
        ASSERT_TRUE(waitSocketConnected(&client), "client connect failed");
        client.write("POST /test HTTP/1.1\r\nContent-Length: 10\r\n\r\n0123456789");
        client.flush();

        ASSERT_TRUE(waitSocketReadyRead(&client), "client wait response failed");
        QByteArray resp = client.readAll();
        ASSERT_TRUE(resp.contains("403 Forbidden"), "expected 403 response");
        ASSERT_TRUE(recv.headersCount == 1, "headersReady must be emitted once");
        ASSERT_TRUE(recv.chunksCount == 0, "bodyChunk must not be emitted after 403");
        ASSERT_TRUE(recv.finishCount == 0, "requestFinished must not be emitted after 403");
    }

    // Case 2: Synchronous 500 in bodyChunk stops subsequent chunks and requestFinished
    {
        CallbackGuardReceiver recv;
        recv.headerAction = CallbackGuardReceiver::Header_Allow;
        recv.chunkAction = CallbackGuardReceiver::Chunk_Reject500;
        QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)), &recv, SLOT(onConnection(HttpConnection*)));

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, server.serverPort());
        ASSERT_TRUE(waitSocketConnected(&client), "client connect failed");
        client.write("POST /test HTTP/1.1\r\nContent-Length: 20\r\n\r\n01234567890123456789");
        client.flush();

        ASSERT_TRUE(waitSocketReadyRead(&client), "client wait response failed");
        QByteArray resp = client.readAll();
        ASSERT_TRUE(resp.contains("500 Internal Server Error"), "expected 500 response");
        ASSERT_TRUE(recv.finishCount == 0, "requestFinished must not be emitted after 500 abort");
    }

    // Case 3: abort() emits disconnected exactly once
    {
        CallbackGuardReceiver recv;
        QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)), &recv, SLOT(onConnection(HttpConnection*)));

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, server.serverPort());
        ASSERT_TRUE(waitSocketConnected(&client), "client connect failed");
        client.write("POST /test HTTP/1.1\r\nContent-Length: 10\r\n\r\n");
        client.flush();

        QTime t;
        t.start();
        while (!recv.conn && t.elapsed() < 1000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        ASSERT_TRUE(recv.conn != 0, "conn must exist");
        recv.conn->abort();
        recv.conn->abort(); // Second call must be idempotent
        t.restart();
        while (recv.disconnectCount == 0 && t.elapsed() < 1000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        ASSERT_TRUE(recv.disconnectCount == 1, "disconnected must be emitted exactly once");
    }

    server.stop();
    TEST_PASS();
}

void testHttpSocketLargeBufferedDrain()
{
    TEST_CASE("R001 - Large 1 MiB request drains socket buffer completely without stalling");
    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    CallbackGuardReceiver recv;
    recv.headerAction = CallbackGuardReceiver::Header_Allow;
    QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)), &recv, SLOT(onConnection(HttpConnection*)));

    int totalBytes = 1048576; // 1 MiB
    QByteArray bodyPayload(totalBytes, 'K');

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketConnected(&client), "client connect failed");

    QByteArray head = "POST /large HTTP/1.1\r\nContent-Length: " + QByteArray::number(totalBytes) + "\r\n\r\n";
    client.write(head);
    client.write(bodyPayload);
    client.flush();

    QTime t;
    t.start();
    while (recv.finishCount == 0 && t.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(recv.finishCount == 1, "requestFinished must be emitted for 1 MiB body");
    ASSERT_TRUE(recv.receivedBytes == totalBytes, "all 1 MiB body bytes must be read");

    server.stop();
    TEST_PASS();
}

void testAppPortChangeRebindAndRollback()
{
    TEST_CASE("R003 - Advertised and listening port rebind, rollback, and busy check");
    localsend::Application app;
    app.setPort(53317);

    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");
    ASSERT_TRUE(app.httpServer()->isListening(), "server must be listening");
    ASSERT_TRUE(app.httpServer()->serverPort() == 53317, "serverPort must match configured port");
    ASSERT_TRUE(app.localInfo().port == 53317, "localInfo port must match");

    // 1. Successful port change while running
    ASSERT_TRUE(app.setPort(53320, &err), "rebind to 53320 should succeed");
    ASSERT_TRUE(app.httpServer()->isListening(), "server must be listening on new port");
    ASSERT_TRUE(app.httpServer()->serverPort() == 53320, "serverPort must be 53320");
    ASSERT_TRUE(app.localInfo().port == 53320, "advertised port must be 53320");

    // 2. Occupied port failure with rollback
    QTcpServer blocker;
    ASSERT_TRUE(blocker.listen(QHostAddress::Any, 0), "blocker listen failed");
    quint16 blockedPort = blocker.serverPort();

    ASSERT_FALSE(app.setPort(blockedPort, &err), "setPort on occupied port must fail");
    ASSERT_TRUE(app.httpServer()->isListening(), "server must rollback and still listen");
    ASSERT_TRUE(app.httpServer()->serverPort() == 53320, "serverPort must rollback to 53320");
    ASSERT_TRUE(app.localInfo().port == 53320, "advertised port must remain 53320");
    blocker.close();

    // 3. Port change rejected when transfer is active
    ASSERT_TRUE(app.beginTransfer("active-session-1"), "beginTransfer should succeed");
    ASSERT_FALSE(app.setPort(53325, &err), "setPort must fail while transfer is active");
    ASSERT_TRUE(err.contains("transfer is active"), "error message must mention active transfer");
    ASSERT_TRUE(app.httpServer()->serverPort() == 53320, "port must remain 53320");
    app.endTransfer("active-session-1");

    app.stop();
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.remove("network/protocol");
        s.sync();
    }
    TEST_PASS();
}

void testAppUnwritableSettingsReportsError()
{
    TEST_CASE("R004 - Unwritable settings failure reporting and Chinese alias persistence");
    // 1. Unwritable configuration storage reports error
    {
        QSettings unwritable("/proc/localsend_unwritable_test.ini", QSettings::IniFormat);
        localsend::Application app(0, &unwritable);
        QString err;
        ASSERT_FALSE(app.start(&err), "app.start must fail with unwritable settings");
        ASSERT_TRUE(err.contains("Configuration storage error"), "error must report storage failure");
    }

    // 2. Chinese alias persistence round-trip
    {
        QTemporaryFile tempIni;
        ASSERT_TRUE(tempIni.open(), "open temp ini failed");
        QString iniPath = tempIni.fileName();
        tempIni.close();

        QString originalFp;
        {
            QSettings s(iniPath, QSettings::IniFormat);
            localsend::Application app(0, &s);
            originalFp = app.localInfo().fingerprint;
            app.setAlias(QString::fromUtf8("黑莓Passport测试机"));
        }

        {
            QSettings s2(iniPath, QSettings::IniFormat);
            localsend::Application app2(0, &s2);
            ASSERT_TRUE(app2.localInfo().fingerprint == originalFp, "fingerprint must persist across processes");
            ASSERT_TRUE(app2.localInfo().alias == QString::fromUtf8("黑莓Passport测试机"), "Chinese alias must persist intact");
        }
    }

    TEST_PASS();
}

void testMockRegisterPythonCompatibility()
{
    TEST_CASE("R005 - Python mock register response compatibility with False");
    QByteArray mockPythonResponse = "{\"alias\": \"Mock Peer\", \"version\": \"2.2\", \"deviceModel\": \"Python Test Peer\", \"deviceType\": \"desktop\", \"fingerprint\": \"mock-peer-fp\", \"download\": false}";
    PeerDevice target;
    target.address = QHostAddress::LocalHost;
    PeerDevice parsed;
    QString err;
    ASSERT_TRUE(ProtocolJson::decodeRegisterResponse(mockPythonResponse, target, &parsed, &err), "must parse Python mock response");
    ASSERT_TRUE(parsed.info.alias == "Mock Peer", "alias match");
    ASSERT_TRUE(parsed.info.download == false, "download boolean False correctly parsed");
    TEST_PASS();
}

void testReceiveUnwritableDirectoryFailsCleanly()
{
    TEST_CASE("R006 - Unwritable receive destination returns 500, frees slot, and recovers");
    // Set destination to /proc which cannot be written to even by root
    localsend::Application app;
    app.setProtocol("http", 0, false);
    app.setPort(0);
    app.setReceiveDirectory("/proc");

    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");

    FileMeta fm;
    fm.id = "file-unwritable-1";
    fm.fileName = "test.bin";
    fm.size = 1024;
    fm.fileType = "application/octet-stream";
    QByteArray reqJson = ProtocolJson::encodePrepareRequest(app.localInfo(), fm, &err);

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, app.httpServer()->serverPort());
    ASSERT_TRUE(waitSocketConnected(&client), "client connect failed");
    QByteArray httpReq;
    httpReq.append("POST /api/localsend/v2/prepare-upload HTTP/1.1\r\n");
    httpReq.append("Content-Type: application/json\r\n");
    httpReq.append("Content-Length: " + QByteArray::number(reqJson.size()) + "\r\n\r\n");
    httpReq.append(reqJson);
    client.write(httpReq);
    client.flush();

    QTime t;
    t.start();
    while (!app.isTransferActive() && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_TRUE(app.isTransferActive(), "transfer slot occupied during pending offer");

    QString activeReqId = app.activeTransferId();
    app.receiveServer()->accept(activeReqId, "file-unwritable-1");

    ASSERT_TRUE(waitSocketReadyRead(&client), "client wait response failed");
    QByteArray resp = client.readAll();
    ASSERT_TRUE(resp.contains("500 Internal Server Error"), "must respond with 500");
    ASSERT_TRUE(resp.contains("Failed to create destination temporary file"), "error reason match");

    ASSERT_FALSE(app.isTransferActive(), "transfer slot must be released after 500");

    // Subsequent transfer to valid directory must succeed
    QString validDir = "/tmp/localsend_writable_recovered";
    QDir().mkpath(validDir);
    app.setReceiveDirectory(validDir);

    QTcpSocket client2;
    client2.connectToHost(QHostAddress::LocalHost, app.httpServer()->serverPort());
    ASSERT_TRUE(waitSocketConnected(&client2), "client2 connect failed");
    client2.write(httpReq);
    client2.flush();

    t.restart();
    while (!app.isTransferActive() && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_TRUE(app.isTransferActive(), "second transfer slot occupied");

    QString activeReqId2 = app.activeTransferId();
    app.receiveServer()->accept(activeReqId2, "file-unwritable-1");

    ASSERT_TRUE(waitSocketReadyRead(&client2), "client2 wait response failed");
    QByteArray resp2 = client2.readAll();
    ASSERT_TRUE(resp2.contains("200 OK"), "second transfer to valid directory must succeed with 200");

    app.receiveServer()->cancelLocal(activeReqId2);
    app.stop();
    QDir(validDir).removeRecursively();
    TEST_PASS();
}

void testTransferRecoveryAfterCancelAndFailWith10MiB()
{
    TEST_CASE("R007 - Transfer recovery: cancel/fail followed by successful 10 MiB bidirectional transfers");

    QString rxDir = "/tmp/localsend_r007_rx";
    QString txDir = "/tmp/localsend_r007_tx";
    QDir().mkpath(rxDir);
    QDir().mkpath(txDir);

    localsend::Application rxApp;
    rxApp.setProtocol("http", 0, false);
    rxApp.setPort(0);
    rxApp.setReceiveDirectory(rxDir);
    QString err;
    ASSERT_TRUE(rxApp.start(&err), "rxApp start failed");

    localsend::Application txApp;
    txApp.setProtocol("http", 0, false);
    txApp.setPort(0);
    txApp.setReceiveDirectory(txDir);
    ASSERT_TRUE(txApp.start(&err), "txApp start failed");

    PeerDevice rxPeer;
    rxPeer.info = rxApp.localInfo();
    rxPeer.address = QHostAddress::LocalHost;
    rxPeer.info.port = rxApp.httpServer()->serverPort();
    rxPeer.info.protocol = "http";

    // --- PHASE 1: Sender-side cancellation during transfer ---
    {
        QString phase1Path = txDir + "/cancel_test.bin";
        QFile f(phase1Path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly), "file open failed");
        f.write(QByteArray(1024 * 1024, 'C'));
        f.close();

        AutoAcceptOfferHandler autoAccept(rxApp.receiveServer());
        QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                         &autoAccept, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

        TxMidStreamCancelHandler txCancel(txApp.sendManager());
        QObject::connect(rxApp.receiveServer(), SIGNAL(receiveProgress(QString,qint64,qint64)),
                         &txCancel, SLOT(onReceiveProgress(QString,qint64,qint64)));

        TestSendTracker tracker;
        QObject::connect(txApp.sendManager(), SIGNAL(sendStateChanged(QString,QString)), &tracker, SLOT(onStateChanged(QString,QString)));
        QObject::connect(txApp.sendManager(), SIGNAL(sendProgress(QString,qint64,qint64)), &tracker, SLOT(onProgress(QString,qint64,qint64)));
        QObject::connect(txApp.sendManager(), SIGNAL(sendFinished(QString)), &tracker, SLOT(onFinished(QString)));
        QObject::connect(txApp.sendManager(), SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

        QString sendErr;
        bool ok = txApp.sendManager()->sendFile(rxPeer, phase1Path, &sendErr);
        ASSERT_TRUE(ok, "sendFile failed to start");

        QTime t;
        t.start();
        while ((txApp.isTransferActive() || rxApp.isTransferActive()) && t.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }

        ASSERT_TRUE(txCancel.sawUploading, "must reach Uploading state before cancellation");
        ASSERT_TRUE(txCancel.cancelled, "must cancel during active transfer");
        ASSERT_TRUE(txCancel.bytesAtCancel > 0, "receiver body bytes must be delivered (>0) before cancellation");
        ASSERT_FALSE(txApp.isTransferActive(), "tx slot must be released after cancel");
        ASSERT_FALSE(rxApp.isTransferActive(), "rx slot must be released after cancel");
        ASSERT_TRUE(txApp.sendManager()->state() == SendManager::State_Cancelled, "must report Cancelled state");

        QDir rDir(rxDir);
        QStringList parts = rDir.entryList(QStringList() << "*.part" << ".*.part", QDir::Files | QDir::Hidden);
        ASSERT_TRUE(parts.isEmpty(), "no .part files should remain after cancellation");
        f.remove();
    }

    // --- PHASE 1b: Receiver rejection (403) recovery ---
    {
        QString phase1bPath = txDir + "/reject_test.bin";
        QFile f(phase1bPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly), "file open failed");
        f.write(QByteArray(65536, 'R'));
        f.close();

        RejectOfferHandler rejectHandler(rxApp.receiveServer());
        QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                         &rejectHandler, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

        TestSendTracker tracker;
        QObject::connect(txApp.sendManager(), SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

        QString sendErr;
        txApp.sendManager()->sendFile(rxPeer, phase1bPath, &sendErr);

        QTime t;
        t.start();
        while (tracker.failedReason.isEmpty() && t.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }

        ASSERT_TRUE(txApp.sendManager()->state() == SendManager::State_Failed, "sender must fail when rejected");
        ASSERT_FALSE(txApp.isTransferActive(), "tx slot released after reject");
        ASSERT_FALSE(rxApp.isTransferActive(), "rx slot released after reject");
        f.remove();
    }

    // --- PHASE 1c: Receiver-side cancellation mid-stream recovery ---
    {
        QString phase1cPath = txDir + "/rx_cancel_test.bin";
        QFile f(phase1cPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly), "file open failed");
        f.write(QByteArray(1024 * 1024, 'M'));
        f.close();

        AutoAcceptOfferHandler autoAccept(rxApp.receiveServer());
        QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                         &autoAccept, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

        RxMidStreamCancelHandler rxCancel(rxApp.receiveServer());
        QObject::connect(rxApp.receiveServer(), SIGNAL(receiveProgress(QString,qint64,qint64)),
                         &rxCancel, SLOT(onProgress(QString,qint64,qint64)));

        TestSendTracker tracker;
        QObject::connect(txApp.sendManager(), SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

        QString sendErr;
        txApp.sendManager()->sendFile(rxPeer, phase1cPath, &sendErr);

        QTime t;
        t.start();
        while ((txApp.isTransferActive() || rxApp.isTransferActive()) && t.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }

        ASSERT_TRUE(rxCancel.cancelled, "receiver must have received bytes and triggered cancelLocal");
        ASSERT_FALSE(txApp.isTransferActive(), "tx slot released after receiver cancel");
        ASSERT_FALSE(rxApp.isTransferActive(), "rx slot released after receiver cancel");

        QDir rDir(rxDir);
        QStringList parts = rDir.entryList(QStringList() << "*.part" << ".*.part", QDir::Files | QDir::Hidden);
        ASSERT_TRUE(parts.isEmpty(), "no .part files remain after receiver cancellation");
        f.remove();
    }

    // --- PHASE 2: 10 MiB Transfer from txApp -> rxApp with SHA-256 validation ---
    {
        QString bigTxPath = txDir + "/10mb_tx_file.bin";
        QFile f(bigTxPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly), "create 10mb file failed");
        QByteArray chunk(65536, 'X');
        for (int i = 0; i < 160; ++i) { // 160 * 64 KiB = 10,485,760 bytes (10 MiB)
            f.write(chunk);
        }
        f.close();

        QString expectedSha = CryptoUtil::sha256File(bigTxPath);

        AutoAcceptOfferHandler autoAccept(rxApp.receiveServer());
        QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                         &autoAccept, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));
        QObject::connect(rxApp.receiveServer(), SIGNAL(receiveFinished(QString,QString)),
                         &autoAccept, SLOT(onReceiveFinished(QString,QString)));

        TestSendTracker tracker;
        QObject::connect(txApp.sendManager(), SIGNAL(sendStateChanged(QString,QString)), &tracker, SLOT(onStateChanged(QString,QString)));
        QObject::connect(txApp.sendManager(), SIGNAL(sendFinished(QString)), &tracker, SLOT(onFinished(QString)));
        QObject::connect(txApp.sendManager(), SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

        QString sendErr;
        bool ok = txApp.sendManager()->sendFile(rxPeer, bigTxPath, &sendErr);
        ASSERT_TRUE(ok, "sendFile for 10mb file failed");

        QTime t;
        t.start();
        while (!tracker.finished && tracker.failedReason.isEmpty() && t.elapsed() < 15000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }

        ASSERT_TRUE(tracker.finished, "10 MiB transfer must finish successfully");
        ASSERT_FALSE(txApp.isTransferActive(), "tx slot released");
        ASSERT_FALSE(rxApp.isTransferActive(), "rx slot released");

        ASSERT_TRUE(!autoAccept.receivedPath.isEmpty(), "received file path must be captured");
        QString receivedSha = CryptoUtil::sha256File(autoAccept.receivedPath);
        ASSERT_TRUE(receivedSha == expectedSha, "10 MiB SHA-256 checksums must match exactly");

        // Independent SHA-256 verification using QCryptographicHash
        QFile checkF(bigTxPath);
        ASSERT_TRUE(checkF.open(QIODevice::ReadOnly), "open bigTxPath failed");
        QByteArray indepTxSha = QCryptographicHash::hash(checkF.readAll(), QCryptographicHash::Sha256).toHex();
        checkF.close();

        QFile checkRxF(autoAccept.receivedPath);
        ASSERT_TRUE(checkRxF.open(QIODevice::ReadOnly), "open received file failed");
        QByteArray indepRxSha = QCryptographicHash::hash(checkRxF.readAll(), QCryptographicHash::Sha256).toHex();
        checkRxF.close();

        ASSERT_TRUE(indepTxSha == indepRxSha, "independent SHA-256 calculation matches");
        ASSERT_TRUE(expectedSha == QString::fromLatin1(indepTxSha), "CryptoUtil SHA-256 matches independent SHA-256");

        f.remove();
        QFile::remove(autoAccept.receivedPath);
    }

    // --- PHASE 3: 10 MiB Transfer in reverse direction (rxApp -> txApp) ---
    {
        PeerDevice txPeer;
        txPeer.info = txApp.localInfo();
        txPeer.address = QHostAddress::LocalHost;
        txPeer.info.port = txApp.httpServer()->serverPort();
        txPeer.info.protocol = "http";

        QString reversePath = rxDir + "/10mb_reverse_file.bin";
        QFile f(reversePath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly), "create reverse 10mb file failed");
        QByteArray chunk(65536, 'Y');
        for (int i = 0; i < 160; ++i) {
            f.write(chunk);
        }
        f.close();

        QString expectedSha = CryptoUtil::sha256File(reversePath);

        AutoAcceptOfferHandler autoAccept(txApp.receiveServer());
        QObject::connect(txApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                         &autoAccept, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));
        QObject::connect(txApp.receiveServer(), SIGNAL(receiveFinished(QString,QString)),
                         &autoAccept, SLOT(onReceiveFinished(QString,QString)));

        TestSendTracker tracker;
        QObject::connect(rxApp.sendManager(), SIGNAL(sendStateChanged(QString,QString)), &tracker, SLOT(onStateChanged(QString,QString)));
        QObject::connect(rxApp.sendManager(), SIGNAL(sendFinished(QString)), &tracker, SLOT(onFinished(QString)));
        QObject::connect(rxApp.sendManager(), SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

        QString sendErr;
        bool ok = rxApp.sendManager()->sendFile(txPeer, reversePath, &sendErr);
        ASSERT_TRUE(ok, "reverse sendFile failed");

        QTime t;
        t.start();
        while (!tracker.finished && tracker.failedReason.isEmpty() && t.elapsed() < 15000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }

        ASSERT_TRUE(tracker.finished, "reverse 10 MiB transfer must finish successfully");
        ASSERT_FALSE(rxApp.isTransferActive(), "reverse tx slot released");
        ASSERT_FALSE(txApp.isTransferActive(), "reverse rx slot released");

        ASSERT_TRUE(!autoAccept.receivedPath.isEmpty(), "reverse received file path must be captured");
        QString receivedSha = CryptoUtil::sha256File(autoAccept.receivedPath);
        ASSERT_TRUE(receivedSha == expectedSha, "reverse 10 MiB SHA-256 checksums must match exactly");

        // Independent reverse SHA-256 verification
        QFile checkRevF(reversePath);
        ASSERT_TRUE(checkRevF.open(QIODevice::ReadOnly), "open reversePath failed");
        QByteArray indepRevTxSha = QCryptographicHash::hash(checkRevF.readAll(), QCryptographicHash::Sha256).toHex();
        checkRevF.close();

        QFile checkRevRxF(autoAccept.receivedPath);
        ASSERT_TRUE(checkRevRxF.open(QIODevice::ReadOnly), "open reverse received file failed");
        QByteArray indepRevRxSha = QCryptographicHash::hash(checkRevRxF.readAll(), QCryptographicHash::Sha256).toHex();
        checkRevRxF.close();

        ASSERT_TRUE(indepRevTxSha == indepRevRxSha, "reverse independent SHA-256 calculation matches");
        ASSERT_TRUE(expectedSha == QString::fromLatin1(indepRevTxSha), "reverse CryptoUtil SHA-256 matches independent SHA-256");

        f.remove();
        QFile::remove(autoAccept.receivedPath);
    }

    rxApp.stop();
    txApp.stop();
    QDir("/tmp/localsend_r007_rx").removeRecursively();
    QDir("/tmp/localsend_r007_tx").removeRecursively();
    TEST_PASS();
}

void testAppSingleThreadAssertionContract()
{
    TEST_CASE("R008 - Single thread state management contract");
    localsend::Application app;
    ASSERT_TRUE(app.thread() == QThread::currentThread(), "Application affined with current thread");
    ASSERT_TRUE(app.beginTransfer("req-thread-1"), "beginTransfer on owning thread succeeds");
    ASSERT_TRUE(app.isTransferActive(), "transfer slot active");
    app.endTransfer("req-thread-1");
    ASSERT_FALSE(app.isTransferActive(), "transfer slot released");
    TEST_PASS();
}

void testDiscoveryStopWithPendingRegistersSafe()
{
    TEST_CASE("R009 - Discovery stop safely aborts multiple pending registers without crash");
    HttpServer mockHangServer;
    ASSERT_TRUE(mockHangServer.listen(QHostAddress::LocalHost, 0), "mock server listen failed");
    quint16 hangPort = mockHangServer.serverPort();

    QNetworkAccessManager nam;
    DiscoveryManager dm(&nam);
    DeviceInfo info;
    info.fingerprint = "self-fp-r009";
    ASSERT_TRUE(dm.start(info), "discovery start failed");

    // Queue multiple register requests to non-responding server
    dm.registerPeer(QHostAddress::LocalHost, hangPort);

    // Call stop() while requests are pending: must not crash with iterator invalidation segfault
    dm.stop();
    mockHangServer.stop();
    TEST_PASS();
}

void testDiscoveryRegisterTimeout()
{
    TEST_CASE("R011 - Register request aborts within 3-second deadline");
    HttpServer mockHangServer;
    ASSERT_TRUE(mockHangServer.listen(QHostAddress::LocalHost, 0), "mock server listen failed");
    quint16 hangPort = mockHangServer.serverPort();

    QNetworkAccessManager nam;
    DiscoveryManager dm(&nam);
    DeviceInfo info;
    info.fingerprint = "self-fp-r011";
    ASSERT_TRUE(dm.start(info), "discovery start failed");

    TestPeerTracker tracker;
    QObject::connect(&dm, SIGNAL(discoveryError(QString)), &tracker, SLOT(onDiscoveryError(QString)));

    dm.registerPeer(QHostAddress::LocalHost, hangPort);

    QTime t;
    t.start();
    while (tracker.lastError.isEmpty() && t.elapsed() < 4500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        usleep(5000);
    }

    ASSERT_TRUE(t.elapsed() >= 2800 && t.elapsed() <= 4500, "timeout must occur around 3000ms");
    ASSERT_TRUE(!tracker.lastError.isEmpty(), "discoveryError must be emitted on timeout");

    dm.stop();
    mockHangServer.stop();
    TEST_PASS();
}

void testUiControllerDevicePropertiesAndAlias()
{
    TEST_CASE("UiController - Device properties, alias change, and error reporting");
    localsend::Application app;
    app.setPort(0);
    localsend::UiController controller(&app);

    ASSERT_TRUE(controller.deviceAlias() == app.localInfo().alias, "alias matches");
    ASSERT_TRUE(controller.devicePort() == app.localInfo().port, "port matches");
    ASSERT_TRUE(controller.receiveDirectory() == app.receiveDirectory(), "directory matches");

    controller.setDeviceAlias("Passport Tester");
    ASSERT_TRUE(controller.deviceAlias() == "Passport Tester", "alias updated");
    ASSERT_TRUE(app.localInfo().alias == "Passport Tester", "core app alias updated");

    controller.addManualPeer("invalid_ip", 53317);
    ASSERT_TRUE(!controller.lastError().isEmpty(), "invalid IP reported in lastError");
    controller.clearError();
    ASSERT_TRUE(controller.lastError().isEmpty(), "clearError clears lastError");
    TEST_PASS();
}

void testUiControllerFileSelection()
{
    TEST_CASE("UiController - File selection, size formatting, and clearing");
    localsend::Application app;
    localsend::UiController controller(&app);

    ASSERT_FALSE(controller.hasSelectedFile(), "no file selected initially");
    controller.setSelectedFilePath("/non_existent/path/12345.xyz");
    ASSERT_FALSE(controller.hasSelectedFile(), "non-existent file rejected");
    ASSERT_TRUE(!controller.lastError().isEmpty(), "file error reported");

    controller.clearError();
    QTemporaryFile tmp;
    ASSERT_TRUE(tmp.open(), "open tmp file failed");
    tmp.write(QByteArray(2048, 'F'));
    tmp.flush();

    controller.setSelectedFilePath(tmp.fileName());
    ASSERT_TRUE(controller.hasSelectedFile(), "valid file accepted");
    ASSERT_TRUE(controller.selectedFileName() == QFileInfo(tmp.fileName()).fileName(), "file name matches");
    ASSERT_TRUE(controller.selectedFileSize() == "2.0 KB", "file size formatted");

    controller.clearSelectedFile();
    ASSERT_FALSE(controller.hasSelectedFile(), "file cleared");
    ASSERT_TRUE(controller.selectedFileName().isEmpty(), "name cleared");
    TEST_PASS();
}

void testUiControllerPeersModel()
{
    TEST_CASE("UiController - Peer list model exposure as QVariantList");
    localsend::Application app;
    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");
    localsend::UiController controller(&app);

    DeviceInfo info;
    info.alias = "Laptop Peer";
    info.deviceModel = "ThinkPad";
    info.deviceType = "desktop";
    info.fingerprint = "fp-test-peer-123";
    info.port = 53317;
    info.protocol = "http";

    app.discoveryManager()->handleIncomingRegister(info, QHostAddress("192.168.1.150"));

    QVariantList list = controller.peers();
    ASSERT_TRUE(list.size() == 1, "peer list has 1 item");

    QVariantMap map = list.first().toMap();
    ASSERT_TRUE(map["alias"].toString() == "Laptop Peer", "peer alias matches");
    ASSERT_TRUE(map["deviceModel"].toString() == "ThinkPad", "peer model matches");
    ASSERT_TRUE(map["address"].toString() == "192.168.1.150", "peer address matches");
    ASSERT_TRUE(map["port"].toInt() == 53317, "peer port matches");
    ASSERT_TRUE(map["isHttps"].toBool() == false, "peer is not https");

    app.stop();
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.remove("network/protocol");
        s.sync();
    }
    TEST_PASS();
}

void testUiControllerSendFlowAndRejection()
{
    TEST_CASE("UiController - Send flow with peer acknowledgement and busy transfer rejection");
    localsend::Application app;
    localsend::UiController controller(&app);

    // 1. Send without file
    ASSERT_FALSE(controller.sendSelectedFile("any-fp"), "send without file returns false");
    ASSERT_TRUE(controller.lastError().contains("No file selected"), "lastError reports missing file");

    QTemporaryFile tmp;
    ASSERT_TRUE(tmp.open(), "open tmp failed");
    tmp.write("data");
    tmp.flush();
    controller.setSelectedFilePath(tmp.fileName());

    // 2. Send to HTTPS peer rejected
    DeviceInfo httpsInfo;
    httpsInfo.alias = "Https Peer";
    httpsInfo.fingerprint = "fp-https-peer";
    httpsInfo.port = 53317;
    httpsInfo.protocol = "https";
    app.discoveryManager()->handleIncomingRegister(httpsInfo, QHostAddress("192.168.1.151"));

    ASSERT_FALSE(controller.sendSelectedFile("fp-https-peer"), "send to https peer returns false");
    ASSERT_TRUE(controller.lastError().contains("HTTPS"), "lastError reports https unsupported");

    // 3. Normal UI send flow through peer acknowledgement + busy transfer rejection (R012)
    localsend::Application rxApp;
    rxApp.setProtocol("http", 0, false);
    rxApp.setPort(0);
    rxApp.setReceiveDirectory("/tmp/localsend_uicontroller_rx");
    QDir("/tmp/localsend_uicontroller_rx").removeRecursively();
    QString rxErr;
    ASSERT_TRUE(rxApp.start(&rxErr), "rxApp start failed");

    AutoAcceptOfferHandler autoAccept(rxApp.receiveServer());
    QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &autoAccept, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

    DeviceInfo rxInfo = rxApp.localInfo();
    rxInfo.protocol = "http";
    rxInfo.port = rxApp.httpServer()->serverPort();
    app.discoveryManager()->handleIncomingRegister(rxInfo, QHostAddress::LocalHost);

    QTemporaryFile sendFile;
    ASSERT_TRUE(sendFile.open(), "open send file failed");
    sendFile.write(QByteArray(65536, 'U'));
    sendFile.flush();
    controller.setSelectedFilePath(sendFile.fileName());

    bool sendStarted = controller.sendSelectedFile(rxInfo.fingerprint);
    ASSERT_TRUE(sendStarted, "controller.sendSelectedFile should start transfer");

    // Attempting a second send while active must be rejected
    ASSERT_FALSE(controller.sendSelectedFile(rxInfo.fingerprint), "second send while active must return false");
    ASSERT_TRUE(controller.lastError().contains("in progress"), "error reports transfer already in progress");

    QTime t;
    t.start();
    while (controller.transferState() != "Completed" && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(controller.transferState() == "Completed", "transfer state must reach Completed");
    ASSERT_TRUE(controller.transferProgress() == 100, "transfer progress must reach 100%");
    ASSERT_FALSE(controller.transferActive(), "transferActive must be false upon completion");

    rxApp.stop();
    QDir("/tmp/localsend_uicontroller_rx").removeRecursively();
    TEST_PASS();
}

void testUiControllerIncomingOfferAndAccept()
{
    TEST_CASE("UiController - Incoming offer prompt, accept, and cancel states");
    localsend::Application app;
    localsend::UiController controller(&app);

    ASSERT_FALSE(controller.hasIncomingOffer(), "no incoming offer initially");

    PeerDevice peer;
    peer.info.alias = "Sender Phone";
    peer.address = QHostAddress("192.168.1.200");

    QList<FileMeta> files;
    FileMeta fm;
    fm.id = "file-id-abc";
    fm.fileName = "vacation.png";
    fm.size = 1048576; // 1 MB
    files.append(fm);

    // Simulate incoming offer signal from ReceiveServer
    emit app.receiveServer()->incomingOffer("req-999", peer, files);

    ASSERT_TRUE(controller.hasIncomingOffer(), "incoming offer detected");
    ASSERT_TRUE(controller.incomingPeerAlias() == "Sender Phone", "incoming peer alias matches");
    ASSERT_TRUE(controller.incomingFileName() == "vacation.png", "incoming file name matches");
    ASSERT_TRUE(controller.incomingFileSize() == "1.0 MB", "incoming file size formatted");
    ASSERT_TRUE(controller.incomingRequestId() == "req-999", "request id matches");
    ASSERT_TRUE(controller.incomingFileId() == "file-id-abc", "file id matches");

    controller.rejectIncoming("req-999");
    ASSERT_FALSE(controller.hasIncomingOffer(), "incoming offer cleared after reject");

    TEST_PASS();
}

void testUiControllerCancelTransferRoutingR014()
{
    TEST_CASE("R014 - Receive cancellation routes to active session, releases slot, cleans .part, and recovers");

    QString rxDir = "/tmp/localsend_r014_rx";
    QDir().mkpath(rxDir);

    localsend::Application rxApp;
    rxApp.setProtocol("http", 0, false);
    rxApp.setPort(0);
    rxApp.setReceiveDirectory(rxDir);
    QString err;
    ASSERT_TRUE(rxApp.start(&err), "rxApp start failed");
    quint16 rxPort = rxApp.httpServer()->serverPort();

    localsend::UiController rxController(&rxApp);

    // Step 1: Send prepare request from HTTP client
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, rxPort);
    ASSERT_TRUE(waitSocketConnected(&client), "client connect failed");

    PrepareUploadRequest prepReq;
    prepReq.info.alias = "Sender Client";
    prepReq.info.fingerprint = "fp-r014-sender";
    prepReq.info.port = 53317;
    prepReq.info.protocol = "http";

    FileMeta fm;
    fm.id = "file-r014-1";
    fm.fileName = "test_cancel.bin";
    fm.size = 1048576;
    prepReq.files.insert("file-r014-1", fm);

    QString encErr;
    QByteArray prepJson = ProtocolJson::encodePrepareRequest(prepReq.info, fm, &encErr);
    QByteArray httpReq = "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\n";
    httpReq.append("Host: 127.0.0.1\r\nContent-Type: application/json\r\n");
    httpReq.append("Content-Length: " + QByteArray::number(prepJson.size()) + "\r\n\r\n");
    httpReq.append(prepJson);
    client.write(httpReq);
    client.flush();

    QTime t;
    t.start();
    while (!rxController.hasIncomingOffer() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_TRUE(rxController.hasIncomingOffer(), "rxController has incoming offer");
    QString reqId = rxController.incomingRequestId();

    // Step 2: Accept incoming offer
    rxController.acceptIncoming(reqId, "file-r014-1");
    ASSERT_TRUE(waitSocketReadyRead(&client), "client wait prepare response failed");
    QByteArray resp = client.readAll();
    ASSERT_TRUE(resp.contains("200 OK"), "prepare response must be 200 OK");
    ASSERT_TRUE(rxApp.receiveServer()->hasActiveSession(), "rxApp must have active session");
    ASSERT_TRUE(rxApp.isTransferActive(), "rxApp slot must be occupied");

    // Step 3: UI cancelTransfer must stop receive session and free slot
    rxController.cancelTransfer();

    ASSERT_FALSE(rxApp.receiveServer()->hasActiveSession(), "active session must be cleared after cancelTransfer");
    ASSERT_FALSE(rxApp.isTransferActive(), "transfer slot must be released after cancelTransfer");
    ASSERT_TRUE(rxController.transferState() == "Cancelled", "UI state must be Cancelled");

    QDir rDir(rxDir);
    QStringList parts = rDir.entryList(QStringList() << "*.part" << ".*.part", QDir::Files | QDir::Hidden);
    ASSERT_TRUE(parts.isEmpty(), "temporary .part files must be removed upon cancellation");

    // Step 4: Sender failure followed by new incoming offer cancellation
    localsend::Application txApp;
    txApp.setProtocol("http", 0, false);
    txApp.setPort(0);
    ASSERT_TRUE(txApp.start(&err), "txApp start failed");
    localsend::UiController txController(&txApp);

    // Cause a send failure
    PeerDevice nonExistentPeer;
    nonExistentPeer.address = QHostAddress("127.0.0.1");
    nonExistentPeer.info.port = 1; // Unreachable port
    nonExistentPeer.info.fingerprint = "fp-fail";
    txApp.discoveryManager()->handleIncomingRegister(nonExistentPeer.info, nonExistentPeer.address);

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "open tmp file failed");
    tmpFile.write("dummy");
    tmpFile.flush();
    txController.setSelectedFilePath(tmpFile.fileName());
    txController.sendSelectedFile("fp-fail");

    t.restart();
    while (txController.transferState() != "Failed" && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_TRUE(txController.transferState() == "Failed", "txController state is Failed");

    // Now send a new incoming offer to txApp
    QTcpSocket client2;
    client2.connectToHost(QHostAddress::LocalHost, txApp.httpServer()->serverPort());
    ASSERT_TRUE(waitSocketConnected(&client2), "client2 connect failed");
    client2.write(httpReq);
    client2.flush();

    t.restart();
    while (!txController.hasIncomingOffer() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_TRUE(txController.hasIncomingOffer(), "txController has new incoming offer");

    // Cancelling the new offer must cancel the offer and not call sender cancel
    txController.cancelTransfer();
    ASSERT_FALSE(txController.hasIncomingOffer(), "incoming offer must be cleared");
    ASSERT_FALSE(txApp.receiveServer()->hasPendingOffer(), "pending offer in core must be cleared");
    ASSERT_FALSE(txApp.isTransferActive(), "txApp slot must be released");

    rxApp.stop();
    txApp.stop();
    QDir(rxDir).removeRecursively();
    TEST_PASS();
}

void testUiControllerReceiveSyncAndDisconnectR015()
{
    TEST_CASE("R015 - Receive UI state sync: unwritable destination reports failure and peer disconnect clears offer");

    // Case 1: Unwritable destination directory reports failure in UI and frees slot
    {
        localsend::Application app;
        app.setProtocol("http", 0, false);
        app.setPort(0);
        app.setReceiveDirectory("/proc");
        QString err;
        ASSERT_TRUE(app.start(&err), "app start failed");

        localsend::UiController controller(&app);

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, app.httpServer()->serverPort());
        ASSERT_TRUE(waitSocketConnected(&client), "client connect failed");

        PrepareUploadRequest prepReq;
        prepReq.info.alias = "Sender";
        prepReq.info.fingerprint = "fp-r015-1";
        prepReq.info.port = 53317;
        FileMeta fm;
        fm.id = "file-r015-1";
        fm.fileName = "test.bin";
        fm.size = 100;
        prepReq.files.insert("file-r015-1", fm);

        QString encErr;
        QByteArray prepJson = ProtocolJson::encodePrepareRequest(prepReq.info, fm, &encErr);
        QByteArray httpReq = "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\n";
        httpReq.append("Host: 127.0.0.1\r\nContent-Type: application/json\r\n");
        httpReq.append("Content-Length: " + QByteArray::number(prepJson.size()) + "\r\n\r\n");
        httpReq.append(prepJson);
        client.write(httpReq);
        client.flush();

        QTime t;
        t.start();
        while (!controller.hasIncomingOffer() && t.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        ASSERT_TRUE(controller.hasIncomingOffer(), "offer received");

        controller.acceptIncoming(controller.incomingRequestId(), controller.incomingFileId());

        ASSERT_TRUE(controller.transferState() == "Failed", "UI state must be Failed after unwritable destination");
        ASSERT_FALSE(controller.hasIncomingOffer(), "offer must be cleared");
        ASSERT_FALSE(app.isTransferActive(), "core transfer slot must be released");
        ASSERT_TRUE(!controller.lastError().isEmpty(), "controller reports last error");

        app.stop();
    }

    // Case 2: Peer disconnects before consent clears offer and resets UI to Idle
    {
        localsend::Application app;
        app.setProtocol("http", 0, false);
        app.setPort(0);
        QString err;
        ASSERT_TRUE(app.start(&err), "app start failed");

        localsend::UiController controller(&app);

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, app.httpServer()->serverPort());
        ASSERT_TRUE(waitSocketConnected(&client), "client connect failed");

        PrepareUploadRequest prepReq;
        prepReq.info.alias = "Sender Disconnecting";
        prepReq.info.fingerprint = "fp-r015-disc";
        prepReq.info.port = 53317;
        FileMeta fm;
        fm.id = "file-r015-disc";
        fm.fileName = "disc.bin";
        fm.size = 500;
        prepReq.files.insert("file-r015-disc", fm);

        QString encErr;
        QByteArray prepJson = ProtocolJson::encodePrepareRequest(prepReq.info, fm, &encErr);
        QByteArray httpReq = "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\n";
        httpReq.append("Host: 127.0.0.1\r\nContent-Type: application/json\r\n");
        httpReq.append("Content-Length: " + QByteArray::number(prepJson.size()) + "\r\n\r\n");
        httpReq.append(prepJson);
        client.write(httpReq);
        client.flush();

        QTime t;
        t.start();
        while (!controller.hasIncomingOffer() && t.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        ASSERT_TRUE(controller.hasIncomingOffer(), "offer received in UI");
        ASSERT_TRUE(app.isTransferActive(), "slot occupied during offer");

        // Remote peer disconnects abruptly
        client.abort();

        t.restart();
        while (controller.hasIncomingOffer() && t.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }

        ASSERT_FALSE(controller.hasIncomingOffer(), "UI offer must be cleared after peer disconnect");
        ASSERT_TRUE(controller.transferState() == "Idle", "UI state must return to Idle");
        ASSERT_FALSE(app.isTransferActive(), "core slot must be freed after peer disconnect");

        app.stop();
    }

    TEST_PASS();
}

void testUiControllerMultiFileCandidateSelectionR016()
{
    TEST_CASE("R016 - Multi-file candidate selection allows choosing and accepting second file");

    localsend::Application app;
    app.setProtocol("http", 0, false);
    app.setPort(0);
    app.setReceiveDirectory("/tmp/localsend_r016_rx");
    QDir("/tmp/localsend_r016_rx").removeRecursively();
    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");

    localsend::UiController controller(&app);

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, app.httpServer()->serverPort());
    ASSERT_TRUE(waitSocketConnected(&client), "client connect failed");

    PrepareUploadRequest prepReq;
    prepReq.info.alias = "Sender Multi";
    prepReq.info.fingerprint = "fp-r016";
    prepReq.info.port = 53317;

    FileMeta f1;
    f1.id = "file-id-1";
    f1.fileName = "photo_first.png";
    f1.size = 1000;
    prepReq.files.insert("file-id-1", f1);

    FileMeta f2;
    f2.id = "file-id-2";
    f2.fileName = "contract_second.pdf";
    f2.size = 2000;
    prepReq.files.insert("file-id-2", f2);

    QByteArray prepJson = "{\n"
        "  \"info\": {\n"
        "    \"alias\": \"Sender Multi\",\n"
        "    \"version\": \"2.2\",\n"
        "    \"deviceModel\": \"TestModel\",\n"
        "    \"deviceType\": \"mobile\",\n"
        "    \"fingerprint\": \"fp-r016\",\n"
        "    \"port\": 53317,\n"
        "    \"protocol\": \"http\",\n"
        "    \"download\": false\n"
        "  },\n"
        "  \"files\": {\n"
        "    \"file-id-1\": {\n"
        "      \"id\": \"file-id-1\",\n"
        "      \"fileName\": \"photo_first.png\",\n"
        "      \"size\": 1000,\n"
        "      \"fileType\": \"image/png\"\n"
        "    },\n"
        "    \"file-id-2\": {\n"
        "      \"id\": \"file-id-2\",\n"
        "      \"fileName\": \"contract_second.pdf\",\n"
        "      \"size\": 2000,\n"
        "      \"fileType\": \"application/pdf\"\n"
        "    }\n"
        "  }\n"
        "}";
    QByteArray httpReq = "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\n";
    httpReq.append("Host: 127.0.0.1\r\nContent-Type: application/json\r\n");
    httpReq.append("Content-Length: " + QByteArray::number(prepJson.size()) + "\r\n\r\n");
    httpReq.append(prepJson);
    client.write(httpReq);
    client.flush();

    QTime t;
    t.start();
    while (!controller.hasIncomingOffer() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    ASSERT_TRUE(controller.hasIncomingOffer(), "offer received");
    ASSERT_TRUE(controller.incomingFiles().size() == 2, "incomingFiles exposes 2 candidate files");

    // By default, first file is selected
    ASSERT_TRUE(controller.selectedIncomingFileId() == "file-id-1", "file-id-1 selected by default");

    // User selects second file
    controller.selectIncomingFile("file-id-2");
    ASSERT_TRUE(controller.selectedIncomingFileId() == "file-id-2", "file-id-2 selected after selection");
    ASSERT_TRUE(controller.incomingFileName() == "contract_second.pdf", "file name updated to second candidate");
    ASSERT_TRUE(controller.incomingFileSize() == "2.0 KB", "file size updated to second candidate");

    // Accept with selected candidate
    controller.acceptIncoming(controller.incomingRequestId(), controller.selectedIncomingFileId());

    ASSERT_TRUE(waitSocketReadyRead(&client), "client wait prepare response failed");
    QByteArray resp = client.readAll();
    ASSERT_TRUE(resp.contains("200 OK"), "must respond 200 OK");

    // Parse prepare response JSON: must contain file-id-2 token and NOT file-id-1 token
    int jsonStart = resp.indexOf("\r\n\r\n");
    ASSERT_TRUE(jsonStart != -1, "response headers terminator found");
    QByteArray respBody = resp.mid(jsonStart + 4);

    PrepareUploadResponse prepResp;
    QString decErr;
    ASSERT_TRUE(ProtocolJson::decodePrepareResponse(respBody, &prepResp, &decErr), "decode prepare response JSON");
    ASSERT_TRUE(prepResp.fileTokens.contains("file-id-2"), "must contain token for selected second file");
    ASSERT_FALSE(prepResp.fileTokens.contains("file-id-1"), "must NOT contain token for unselected first file");

    app.receiveServer()->cancelLocal();
    app.stop();
    QDir("/tmp/localsend_r016_rx").removeRecursively();
    TEST_PASS();
}

void testUiControllerCollisionRenamingAndSavedPathR017()
{
    TEST_CASE("R017 - Collision renaming retains both files and updates lastSavedFilePath");

    QString rxDir = "/tmp/localsend_r017_rx";
    QDir().mkpath(rxDir);

    localsend::Application rxApp;
    rxApp.setProtocol("http", 0, false);
    rxApp.setPort(0);
    rxApp.setReceiveDirectory(rxDir);
    QString err;
    ASSERT_TRUE(rxApp.start(&err), "rxApp start failed");

    localsend::UiController rxController(&rxApp);

    AutoAcceptOfferHandler autoAccept(rxApp.receiveServer());
    QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &autoAccept, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

    localsend::Application txApp;
    txApp.setProtocol("http", 0, false);
    txApp.setPort(0);
    ASSERT_TRUE(txApp.start(&err), "txApp start failed");

    PeerDevice rxPeer;
    rxPeer.info = rxApp.localInfo();
    rxPeer.address = QHostAddress::LocalHost;
    rxPeer.info.port = rxApp.httpServer()->serverPort();
    rxPeer.info.protocol = "http";

    // Transfer 1: send "notes.txt" with content "Alpha"
    QString txDir = "/tmp/localsend_r017_tx";
    QDir(txDir).removeRecursively();
    QDir().mkpath(txDir);
    QString file1Path = txDir + "/notes.txt";
    QFile f1(file1Path);
    ASSERT_TRUE(f1.open(QIODevice::WriteOnly), "create file1 failed");
    f1.write("Alpha");
    f1.close();

    TestSendTracker tracker1;
    QObject::connect(txApp.sendManager(), SIGNAL(sendFinished(QString)), &tracker1, SLOT(onFinished(QString)));
    txApp.sendManager()->sendFile(rxPeer, file1Path);

    QTime t;
    t.start();
    while (!tracker1.finished && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_TRUE(tracker1.finished, "transfer 1 finished");

    QString firstSavedPath = rxController.lastSavedFilePath();
    ASSERT_TRUE(firstSavedPath.endsWith("notes.txt"), "first saved path ends with notes.txt");
    ASSERT_TRUE(QFile::exists(firstSavedPath), "first file exists on disk");

    // Transfer 2: send another "notes.txt" with content "Beta"
    QString file2Path = txDir + "/notes_sub/notes.txt";
    QDir().mkpath(txDir + "/notes_sub");
    QFile f2(file2Path);
    ASSERT_TRUE(f2.open(QIODevice::WriteOnly), "create file2 failed");
    f2.write("Beta");
    f2.close();

    TestSendTracker tracker2;
    QObject::connect(txApp.sendManager(), SIGNAL(sendFinished(QString)), &tracker2, SLOT(onFinished(QString)));
    txApp.sendManager()->sendFile(rxPeer, file2Path);

    t.restart();
    while (!tracker2.finished && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_TRUE(tracker2.finished, "transfer 2 finished");

    QString secondSavedPath = rxController.lastSavedFilePath();
    ASSERT_TRUE(secondSavedPath.endsWith("notes (1).txt"), "second saved path collision renamed to notes (1).txt");
    ASSERT_TRUE(QFile::exists(secondSavedPath), "second file exists on disk");
    ASSERT_TRUE(firstSavedPath != secondSavedPath, "two saved paths must be distinct");

    // Verify contents of both files
    QFile rf1(firstSavedPath);
    ASSERT_TRUE(rf1.open(QIODevice::ReadOnly), "open rf1 failed");
    ASSERT_TRUE(rf1.readAll() == "Alpha", "first file content matches Alpha");

    QFile rf2(secondSavedPath);
    ASSERT_TRUE(rf2.open(QIODevice::ReadOnly), "open rf2 failed");
    ASSERT_TRUE(rf2.readAll() == "Beta", "second file content matches Beta");

    rxApp.stop();
    txApp.stop();
    QDir(rxDir).removeRecursively();
    QDir(txDir).removeRecursively();
    TEST_PASS();
}

class TerminalSignalCounter : public QObject {
    Q_OBJECT
public:
    int finishedCount;
    int failedCount;
    int cancelledCount;

    TerminalSignalCounter(QObject *parent = 0)
        : QObject(parent), finishedCount(0), failedCount(0), cancelledCount(0) {}

public slots:
    void onFinished(const QString &id, const QString &path) {
        Q_UNUSED(id); Q_UNUSED(path);
        finishedCount++;
    }
    void onFailed(const QString &id, const QString &reason) {
        Q_UNUSED(id); Q_UNUSED(reason);
        failedCount++;
    }
    void onCancelled(const QString &id, const QString &reason) {
        Q_UNUSED(id); Q_UNUSED(reason);
        cancelledCount++;
    }
};

class UiPropertyChangeSpy : public QObject {
    Q_OBJECT
public:
    int transferStateChangedCount;
    UiPropertyChangeSpy(QObject *parent = 0)
        : QObject(parent), transferStateChangedCount(0) {}
public slots:
    void onTransferStateChanged() {
        transferStateChangedCount++;
    }
};

void testReceiveCancelSingleTerminalSignalR007()
{
    TEST_CASE("R007 - Mid-stream receiver cancellation emits exactly one terminal signal");

    QString rxDir = "/tmp/localsend_r007_term_rx";
    QString txDir = "/tmp/localsend_r007_term_tx";
    QDir().mkpath(rxDir);
    QDir().mkpath(txDir);

    localsend::Application rxApp;
    rxApp.setProtocol("http", 0, false);
    rxApp.setPort(0);
    rxApp.setReceiveDirectory(rxDir);
    QString err;
    ASSERT_TRUE(rxApp.start(&err), "rxApp start failed");

    localsend::Application txApp;
    txApp.setProtocol("http", 0, false);
    txApp.setPort(0);
    txApp.setReceiveDirectory(txDir);
    ASSERT_TRUE(txApp.start(&err), "txApp start failed");

    PeerDevice rxPeer;
    rxPeer.info = rxApp.localInfo();
    rxPeer.address = QHostAddress::LocalHost;
    rxPeer.info.port = rxApp.httpServer()->serverPort();
    rxPeer.info.protocol = "http";

    TerminalSignalCounter sigCounter;
    QObject::connect(rxApp.receiveServer(), SIGNAL(receiveFinished(QString,QString)), &sigCounter, SLOT(onFinished(QString,QString)));
    QObject::connect(rxApp.receiveServer(), SIGNAL(receiveFailed(QString,QString)), &sigCounter, SLOT(onFailed(QString,QString)));
    QObject::connect(rxApp.receiveServer(), SIGNAL(receiveCancelled(QString,QString)), &sigCounter, SLOT(onCancelled(QString,QString)));

    QString testFile = txDir + "/term_cancel.bin";
    QFile f(testFile);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly), "file open failed");
    f.write(QByteArray(1024 * 1024, 'T'));
    f.close();

    AutoAcceptOfferHandler autoAccept(rxApp.receiveServer());
    QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &autoAccept, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

    RxMidStreamCancelHandler rxCancel(rxApp.receiveServer());
    QObject::connect(rxApp.receiveServer(), SIGNAL(receiveProgress(QString,qint64,qint64)),
                     &rxCancel, SLOT(onProgress(QString,qint64,qint64)));

    TestSendTracker sendTracker;
    QObject::connect(txApp.sendManager(), SIGNAL(sendFailed(QString,QString)), &sendTracker, SLOT(onFailed(QString,QString)));

    txApp.sendManager()->sendFile(rxPeer, testFile);

    QTime t;
    t.start();
    while ((txApp.isTransferActive() || rxApp.isTransferActive()) && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    ASSERT_TRUE(sigCounter.cancelledCount == 1, "receiveCancelled must be emitted exactly once");
    ASSERT_TRUE(sigCounter.failedCount == 0, "receiveFailed must not be emitted when cancelled");
    ASSERT_TRUE(sigCounter.finishedCount == 0, "receiveFinished must not be emitted when cancelled");

    rxApp.stop();
    txApp.stop();
    QDir(rxDir).removeRecursively();
    QDir(txDir).removeRecursively();
    TEST_PASS();
}

void testReceiveDestinationFlushErrorR020()
{
    TEST_CASE("R020 - Incomplete upload or checksum error emits receiveFailed, returns 4xx/5xx, and cleans temporary file");

    QString rxDir = "/tmp/localsend_r020_rx";
    QDir().mkpath(rxDir);

    localsend::Application rxApp;
    rxApp.setProtocol("http", 0, false);
    rxApp.setPort(0);
    rxApp.setReceiveDirectory(rxDir);
    QString err;
    ASSERT_TRUE(rxApp.start(&err), "rxApp start failed");

    TerminalSignalCounter sigCounter;
    QObject::connect(rxApp.receiveServer(), SIGNAL(receiveFinished(QString,QString)), &sigCounter, SLOT(onFinished(QString,QString)));
    QObject::connect(rxApp.receiveServer(), SIGNAL(receiveFailed(QString,QString)), &sigCounter, SLOT(onFailed(QString,QString)));

    AutoAcceptOfferHandler autoAccept(rxApp.receiveServer());
    QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &autoAccept, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

    DeviceInfo senderInfo;
    senderInfo.alias = "Probe";
    senderInfo.fingerprint = "probe-fp-r020";
    senderInfo.port = 53317;
    senderInfo.protocol = "http";

    FileMeta fm1;
    fm1.id = "f1";
    fm1.fileName = "corrupt.bin";
    fm1.size = 4;
    fm1.fileType = "application/octet-stream";
    fm1.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    QString encErr;
    QByteArray prepareBody1 = ProtocolJson::encodePrepareRequest(senderInfo, fm1, &encErr);
    ASSERT_TRUE(!prepareBody1.isEmpty(), "encode prepare request failed");

    QTcpSocket client1;
    client1.connectToHost("127.0.0.1", rxApp.httpServer()->serverPort());
    ASSERT_TRUE(waitSocketConnected(&client1), "client1 connect failed");

    QByteArray prepReq1 = "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\n";
    prepReq1.append("Host: 127.0.0.1\r\nContent-Type: application/json\r\n");
    prepReq1.append("Content-Length: " + QByteArray::number(prepareBody1.size()) + "\r\n\r\n");
    prepReq1.append(prepareBody1);
    client1.write(prepReq1);
    client1.flush();
    ASSERT_TRUE(waitSocketReadyRead(&client1), "wait prepare response failed");
    QByteArray prepResp1 = client1.readAll();
    ASSERT_TRUE(prepResp1.contains("200 OK"), "prepare-upload 200 OK expected");

    // Extract sessionId and token
    QByteArray prepJson1 = prepResp1.mid(prepResp1.indexOf("\r\n\r\n") + 4);
    PrepareUploadResponse prepData1;
    QString decodeErr;
    ASSERT_TRUE(ProtocolJson::decodePrepareResponse(prepJson1, &prepData1, &decodeErr), "decode prepare response");
    QString sessionId1 = prepData1.sessionId;
    QString token1 = prepData1.fileTokens.value("f1");

    // Now send upload with wrong SHA-256 data: 4 bytes of 'X' on a new connection
    client1.disconnectFromHost();

    QTcpSocket uploadClient1;
    uploadClient1.connectToHost("127.0.0.1", rxApp.httpServer()->serverPort());
    ASSERT_TRUE(waitSocketConnected(&uploadClient1), "uploadClient1 connect failed");

    QString uploadReq1 = QString("POST /api/localsend/v2/upload?sessionId=%1&fileId=f1&token=%2 HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 4\r\n\r\nXXXX")
                         .arg(sessionId1).arg(token1);
    uploadClient1.write(uploadReq1.toUtf8());
    uploadClient1.flush();
    ASSERT_TRUE(waitSocketReadyRead(&uploadClient1), "wait upload response failed");
    QByteArray uploadResp1 = uploadClient1.readAll();
    uploadClient1.disconnectFromHost();

    ASSERT_TRUE(uploadResp1.contains("422"), "upload with wrong SHA-256 must return HTTP 422");
    ASSERT_TRUE(sigCounter.failedCount == 1, "receiveFailed must be emitted for checksum mismatch");
    ASSERT_TRUE(sigCounter.finishedCount == 0, "receiveFinished must not be emitted for checksum mismatch");

    // Verify no destination file was committed and no .part file remains
    QDir dir(rxDir);
    QStringList files1 = dir.entryList(QDir::Files | QDir::Hidden);
    ASSERT_TRUE(files1.size() == 0, "no corrupt file or .part file should remain in receive directory");

    // Phase 2: Verify subsequent transfer succeeds
    FileMeta fm2;
    fm2.id = "f2";
    fm2.fileName = "valid.bin";
    fm2.size = 4;
    fm2.fileType = "application/octet-stream";
    Sha256Context sha2;
    sha2.update("YYYY", 4);
    fm2.sha256 = sha2.finalHex();

    QByteArray prepareBody2 = ProtocolJson::encodePrepareRequest(senderInfo, fm2, &encErr);
    ASSERT_TRUE(!prepareBody2.isEmpty(), "encode prepare request 2 failed");

    QTcpSocket client2;
    client2.connectToHost("127.0.0.1", rxApp.httpServer()->serverPort());
    ASSERT_TRUE(waitSocketConnected(&client2), "client2 connect failed");

    QByteArray prepReq2 = "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\n";
    prepReq2.append("Host: 127.0.0.1\r\nContent-Type: application/json\r\n");
    prepReq2.append("Content-Length: " + QByteArray::number(prepareBody2.size()) + "\r\n\r\n");
    prepReq2.append(prepareBody2);
    client2.write(prepReq2);
    client2.flush();
    ASSERT_TRUE(waitSocketReadyRead(&client2), "wait prepare response 2 failed");
    QByteArray prepResp2 = client2.readAll();
    ASSERT_TRUE(prepResp2.contains("200 OK"), "prepare-upload 2 200 OK expected");
    client2.disconnectFromHost();

    QByteArray prepJson2 = prepResp2.mid(prepResp2.indexOf("\r\n\r\n") + 4);
    PrepareUploadResponse prepData2;
    ASSERT_TRUE(ProtocolJson::decodePrepareResponse(prepJson2, &prepData2, &decodeErr), "decode prepare response 2");
    QString sessionId2 = prepData2.sessionId;
    QString token2 = prepData2.fileTokens.value("f2");

    QTcpSocket uploadClient2;
    uploadClient2.connectToHost("127.0.0.1", rxApp.httpServer()->serverPort());
    ASSERT_TRUE(waitSocketConnected(&uploadClient2), "uploadClient2 connect failed");

    QString uploadReq2 = QString("POST /api/localsend/v2/upload?sessionId=%1&fileId=f2&token=%2 HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 4\r\n\r\nYYYY")
                         .arg(sessionId2).arg(token2);
    uploadClient2.write(uploadReq2.toUtf8());
    uploadClient2.flush();
    ASSERT_TRUE(waitSocketReadyRead(&uploadClient2), "wait upload response 2 failed");
    QByteArray uploadResp2 = uploadClient2.readAll();
    uploadClient2.disconnectFromHost();

    ASSERT_TRUE(uploadResp2.contains("200 OK"), "valid upload must return HTTP 200");
    ASSERT_TRUE(sigCounter.finishedCount == 1, "receiveFinished must be emitted once after successful transfer");
    ASSERT_TRUE(sigCounter.failedCount == 1, "failedCount remains 1");

    QFile validFile(rxDir + "/valid.bin");
    ASSERT_TRUE(validFile.open(QIODevice::ReadOnly), "valid file must open");
    ASSERT_TRUE(validFile.readAll() == "YYYY", "valid file content match");
    validFile.close();

    rxApp.stop();
    QDir(rxDir).removeRecursively();
    TEST_PASS();
}

void testUiControllerTransferActiveNotificationR015()
{
    TEST_CASE("R015 - UiController emits transferStateChanged upon completion, setting transferActive to false");

    QString rxDir = "/tmp/localsend_r015_notif_rx";
    QString txDir = "/tmp/localsend_r015_notif_tx";
    QDir().mkpath(rxDir);
    QDir().mkpath(txDir);

    localsend::Application rxApp;
    rxApp.setProtocol("http", 0, false);
    rxApp.setPort(0);
    rxApp.setReceiveDirectory(rxDir);
    QString err;
    ASSERT_TRUE(rxApp.start(&err), "rxApp start failed");

    localsend::Application txApp;
    txApp.setProtocol("http", 0, false);
    txApp.setPort(0);
    txApp.setReceiveDirectory(txDir);
    ASSERT_TRUE(txApp.start(&err), "txApp start failed");

    localsend::UiController rxController(&rxApp);
    localsend::UiController txController(&txApp);

    UiPropertyChangeSpy rxSpy;
    QObject::connect(&rxController, SIGNAL(transferStateChanged()), &rxSpy, SLOT(onTransferStateChanged()));

    PeerDevice rxPeer;
    rxPeer.info = rxApp.localInfo();
    rxPeer.address = QHostAddress::LocalHost;
    rxPeer.info.port = rxApp.httpServer()->serverPort();
    rxPeer.info.protocol = "http";

    QString testFile = txDir + "/test_notif.txt";
    QFile f(testFile);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly), "file open failed");
    f.write("NotificationTest");
    f.close();

    AutoAcceptOfferHandler autoAccept(rxApp.receiveServer());
    QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &autoAccept, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

    TestSendTracker tracker;
    QObject::connect(txApp.sendManager(), SIGNAL(sendFinished(QString)), &tracker, SLOT(onFinished(QString)));
    txApp.sendManager()->sendFile(rxPeer, testFile);

    QTime t;
    t.start();
    while (!tracker.finished && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_TRUE(tracker.finished, "send finished");

    ASSERT_TRUE(!rxController.transferActive(), "rxController transferActive must be false after completion");
    ASSERT_TRUE(rxSpy.transferStateChangedCount > 0, "transferStateChanged notification must be emitted on slot completion");

    rxApp.stop();
    txApp.stop();
    QDir(rxDir).removeRecursively();
    QDir(txDir).removeRecursively();
    TEST_PASS();
}

void testDiscoveryInterfaceSelectionAndSubnetPrefixR021()
{
    TEST_CASE("R021 - DiscoveryManager interface selection and dynamic subnet prefix");

    localsend::Application app;
    app.setPort(0);
    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");

    DiscoveryManager *dm = app.discoveryManager();
    ASSERT_TRUE(dm != 0, "discovery manager must not be null");

    QString subnet = dm->activeInterfaceSubnet();
    if (!subnet.isEmpty()) {
        ASSERT_TRUE(subnet.contains("/"), "subnet string must contain prefix separator '/'");
        QStringList parts = subnet.split("/");
        ASSERT_TRUE(parts.size() == 2, "subnet string must have network address and prefix");
        int prefix = parts[1].toInt();
        ASSERT_TRUE(prefix >= 8 && prefix <= 32, "prefix length must be valid between 8 and 32");
    }

    app.stop();
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.remove("network/protocol");
        s.sync();
    }
    TEST_PASS();
}

void testDiscoveryUdpBufferAndPacedSweepR025()
{
    TEST_CASE("R025 - DiscoveryManager UDP buffer expansion and paced sweep engine");

    localsend::Application app;
    app.setPort(0);
    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");

    DiscoveryManager *dm = app.discoveryManager();
    ASSERT_TRUE(dm != 0, "discovery manager must not be null");

    // Read back actual socket buffer sizes configured via setsockopt
    int sndBuf = 0, rcvBuf = 0;
    bool ok = dm->getSocketBufferSizes(&sndBuf, &rcvBuf);
    ASSERT_TRUE(ok, "socket buffer sizes must be readable via getsockopt");
    ASSERT_TRUE(sndBuf >= 65536, "SO_SNDBUF must be at least 64 KiB");
    ASSERT_TRUE(rcvBuf >= 65536, "SO_RCVBUF must be at least 64 KiB");

    // Enqueue 10 explicit targets to verify deterministic queue pacing and draining
    for (int i = 1; i <= 10; ++i) {
        dm->queueUdpSweepTarget(QHostAddress(QString("10.254.0.%1").arg(i)));
    }
    ASSERT_TRUE(dm->udpSweepQueue().size() >= 10, "udp sweep queue must be nonempty and contain enqueued targets");

    // Wait for the paced UDP sweep to drain (15ms timer batches)
    QTime timer;
    timer.start();
    while (!dm->udpSweepQueue().isEmpty() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        usleep(5000);
    }
    if (!dm->udpSweepQueue().isEmpty()) {
        qDebug() << "[Test] Remaining udpSweepQueue size:" << dm->udpSweepQueue().size()
                 << "elapsed:" << timer.elapsed();
    }
    ASSERT_TRUE(dm->udpSweepQueue().isEmpty(), "udp sweep queue must drain completely without stalling");

    app.stop();
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.remove("network/protocol");
        s.sync();
    }
    TEST_PASS();
}

void testDiscoveryHistoricalPeerPrioritizationR025()
{
    TEST_CASE("R025 - DiscoveryManager subnet scan prioritizes historical peers");

    localsend::Application app;
    app.setPort(0);
    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");

    DiscoveryManager *dm = app.discoveryManager();
    ASSERT_TRUE(dm != 0, "discovery manager must not be null");

    // Test MRU historical peer ordering deterministically
    QHostAddress hist1("192.168.3.161");
    QHostAddress hist2("192.168.3.123");
    dm->addHistoricalPeer(hist1);
    dm->addHistoricalPeer(hist2);
    ASSERT_TRUE(dm->historicalPeers().first() == hist2, "MRU order: latest added historical peer is at head");
    ASSERT_TRUE(dm->historicalPeers().at(1) == hist1, "MRU order: earlier peer preserved");

    QString ifaceAddr = dm->activeInterfaceAddress();
    if (!ifaceAddr.isEmpty()) {
        QHostAddress localHost(ifaceAddr);
        quint32 localIp = localHost.toIPv4Address();
        // Construct an IP ending in host 161 on same /24 subnet
        quint32 targetIp = (localIp & 0xFFFFFF00) | 161;
        if (targetIp == localIp) {
            targetIp = (localIp & 0xFFFFFF00) | 162;
        }
        QHostAddress targetAddr(targetIp);

        dm->addHistoricalPeer(targetAddr);
        dm->refresh();

        // Target should be prioritized at the head of the sweep queue
        QList<QHostAddress> sweepQueue = dm->udpSweepQueue();
        if (!sweepQueue.isEmpty()) {
            ASSERT_TRUE(sweepQueue.first() == targetAddr,
                        "historical peer in local subnet must be prioritized at the head of sweep queue");
        }
    }

    app.stop();
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.remove("network/protocol");
        s.sync();
    }
    TEST_PASS();
}

void testDiscoveryPacedSweepHeadOfLineBlockingPreventionR025()
{
    TEST_CASE("R025 - P1 Head-of-line blocking prevention: failing target does not starve queue");

    localsend::Application app;
    app.setPort(0);
    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");

    DiscoveryManager *dm = app.discoveryManager();
    ASSERT_TRUE(dm != 0, "discovery manager must not be null");

    // Enqueue an unroutable address (240.0.0.1 is Class E reserved) followed by valid loopback targets
    QHostAddress unroutableHost("240.0.0.1");
    QHostAddress validHost1("127.0.0.2");
    QHostAddress validHost2("127.0.0.3");

    dm->queueUdpSweepTarget(unroutableHost);
    dm->queueUdpSweepTarget(validHost1);
    dm->queueUdpSweepTarget(validHost2);

    ASSERT_TRUE(dm->udpSweepQueue().contains(unroutableHost), "unroutable host in sweep queue");
    ASSERT_TRUE(dm->udpSweepQueue().contains(validHost1), "validHost1 in sweep queue");
    ASSERT_TRUE(dm->udpSweepQueue().contains(validHost2), "validHost2 in sweep queue");

    // Run event loop to allow pumpUdpSweep to process targets
    QTime timer;
    timer.start();
    while (!dm->udpSweepQueue().isEmpty() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        usleep(10000);
    }

    // Queue must drain completely: failing/unroutable targets must not starve subsequent valid targets
    ASSERT_TRUE(dm->udpSweepQueue().isEmpty(), "sweep queue must drain completely and drop permanently failing targets");

    // Assert that each target was actually attempted
    ASSERT_TRUE(dm->udpSweepAttempts(unroutableHost) >= 1, "unroutable host was attempted and classified");
    ASSERT_TRUE(dm->udpSweepAttempts(validHost1) >= 1, "validHost1 was attempted despite unroutable predecessor");
    ASSERT_TRUE(dm->udpSweepAttempts(validHost2) >= 1, "validHost2 was attempted despite unroutable predecessor");

    app.stop();
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.remove("network/protocol");
        s.sync();
    }
    TEST_PASS();
}

void testDiscoveryFallbackPacedEnqueueR025()
{
    TEST_CASE("R025 - P2 HTTP probe failure fallback routes through paced sweep queue via production callback");

    localsend::Application app;
    app.setPort(0);
    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");

    DiscoveryManager *dm = app.discoveryManager();
    ASSERT_TRUE(dm != 0, "discovery manager must not be null");

    // Inject an HTTP scan probe target on loopback where TCP port 53317 is closed/refused
    // (ReceiveServer is listening on dynamic port != 53317).
    // This triggers production pumpScanQueue() -> m_nam->post -> QNetworkReply error
    // -> onProbeReplyFinished() -> queueUdpSweepTarget().
    QHostAddress failedHttpTarget("127.0.0.199");
    dm->injectScanTarget(failedHttpTarget);

    // Wait for the HTTP reply to fail and trigger onProbeReplyFinished()
    QTime timer;
    timer.start();
    bool enqueuedInSweep = false;
    while (timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (dm->udpSweepQueue().contains(failedHttpTarget)) {
            enqueuedInSweep = true;
            break;
        }
        usleep(10000);
    }

    ASSERT_TRUE(enqueuedInSweep, "failed HTTP probe must route fallback UDP announcement into udpSweepQueue via onProbeReplyFinished");

    // Drain the sweep queue
    while (!dm->udpSweepQueue().isEmpty() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        usleep(10000);
    }
    ASSERT_TRUE(dm->udpSweepQueue().isEmpty(), "paced fallback queue must drain completely");

    app.stop();
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.remove("network/protocol");
        s.sync();
    }
    TEST_PASS();
}

void testDiscoveryWholeScanAttemptCapR025()
{
    TEST_CASE("R025 - Whole-scan attempt cap (<= 3 attempts across initial sweep + HTTP fallback)");

    localsend::Application app;
    app.setPort(0);
    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");

    DiscoveryManager *dm = app.discoveryManager();
    ASSERT_TRUE(dm != 0, "discovery manager must not be null");

    QHostAddress target("127.0.0.220");

    // 1. Enqueue target and drain it (Attempt 1)
    dm->queueUdpSweepTarget(target);
    QTime timer;
    timer.start();
    while (!dm->udpSweepQueue().isEmpty() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(dm->udpSweepAttempts(target) == 1, "attempt 1 recorded");

    // 2. Enqueue target and drain it (Attempt 2)
    dm->queueUdpSweepTarget(target);
    timer.restart();
    while (!dm->udpSweepQueue().isEmpty() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(dm->udpSweepAttempts(target) == 2, "attempt 2 recorded");

    // 3. Enqueue target and drain it (Attempt 3)
    dm->queueUdpSweepTarget(target);
    timer.restart();
    while (!dm->udpSweepQueue().isEmpty() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(dm->udpSweepAttempts(target) == 3, "attempt 3 recorded");

    // 4. Target has now exhausted its 3 attempts for this scan.
    // Verify that subsequent fallback enqueue is rejected and not added to the queue!
    int queueSizeBefore = dm->udpSweepQueue().size();
    dm->queueUdpSweepTarget(target);
    ASSERT_TRUE(dm->udpSweepQueue().size() == queueSizeBefore, "exhausted target (>= 3 attempts) must be rejected by queueUdpSweepTarget");
    ASSERT_TRUE(!dm->udpSweepQueue().contains(target), "exhausted target must not be enqueued");
    ASSERT_TRUE(dm->udpSweepAttempts(target) == 3, "attempt count must not exceed 3");

    // 5. Verify that injectScanTarget / onProbeReplyFinished cannot re-exhaust target beyond 3 attempts
    dm->injectScanTarget(target);
    timer.restart();
    while (timer.elapsed() < 1500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    // Attempt count must remain <= 3 across the entire scan, never resetting to 6!
    ASSERT_TRUE(dm->udpSweepAttempts(target) <= 3, "attempt count across whole scan must be <= 3");

    app.stop();
    // After stop, attempts must be cleared
    ASSERT_TRUE(dm->udpSweepAttempts(target) == 0, "attempts cleared after stop");

    TEST_PASS();
}

void testDiscoveryActiveInterfaceResolutionR025()
{
    TEST_CASE("R025 - Active multicast interface resolved during normal Application startup");

    localsend::Application app;
    app.setPort(0);
    QString err;
    ASSERT_TRUE(app.start(&err), "app start failed");

    DiscoveryManager *dm = app.discoveryManager();
    ASSERT_TRUE(dm != 0, "discovery manager must not be null");

    // Check if host has an up, non-loopback IPv4 candidate interface
    QNetworkInterface activeIface = dm->activeInterface();
    QString ifaceAddr = dm->activeInterfaceAddress();

    QList<QNetworkInterface> allIfaces = QNetworkInterface::allInterfaces();
    bool hasCandidate = false;
    for (int i = 0; i < allIfaces.size(); ++i) {
        const QNetworkInterface &it = allIfaces.at(i);
        if ((it.flags() & QNetworkInterface::IsUp) &&
            !(it.flags() & QNetworkInterface::IsLoopBack) &&
            !(it.flags() & QNetworkInterface::IsPointToPoint)) {
            QList<QNetworkAddressEntry> entries = it.addressEntries();
            for (int j = 0; j < entries.size(); ++j) {
                if (entries.at(j).ip().protocol() == QAbstractSocket::IPv4Protocol &&
                    entries.at(j).ip() != QHostAddress::LocalHost) {
                    hasCandidate = true;
                    break;
                }
            }
        }
    }

    if (hasCandidate) {
        ASSERT_TRUE(activeIface.isValid(), "activeInterface must be valid when candidate LAN exists");
        ASSERT_TRUE(!activeIface.name().isEmpty(), "activeInterface name must not be empty");
        ASSERT_TRUE(!ifaceAddr.isEmpty(), "activeInterfaceAddress must not be empty");
        qDebug() << "[Test] Active resolved interface:" << activeIface.name() << "addr:" << ifaceAddr;
    } else {
        qDebug() << "[Test] No non-loopback candidate interface on host (e.g. isolated network container)";
    }

    int snd = 0, rcv = 0;
    ASSERT_TRUE(dm->getSocketBufferSizes(&snd, &rcv), "socket buffer sizes must be readable");
    ASSERT_TRUE(snd > 0 && rcv > 0, "socket buffers must be positive");

    app.stop();
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.remove("network/protocol");
        s.sync();
    }
    TEST_PASS();
}

// ==========================================
// H2 TLS Transport & LocalIdentity Tests
// ==========================================

class TlsSocketSpy : public QObject {
    Q_OBJECT
public:
    bool encryptedReceived;
    bool disconnectedReceived;
    bool errorReceived;
    int lastErrorCode;
    QString lastErrorMsg;
    int readyReadCount;
    qint64 totalBytesWritten;

    TlsSocketSpy(localsend::PosixTlsSocket *sock, QObject *parent = 0)
        : QObject(parent)
        , encryptedReceived(false)
        , disconnectedReceived(false)
        , errorReceived(false)
        , lastErrorCode(POSIX_TLS_OK)
        , readyReadCount(0)
        , totalBytesWritten(0)
    {
        connect(sock, SIGNAL(encrypted()), this, SLOT(onEncrypted()));
        connect(sock, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
        connect(sock, SIGNAL(tlsError(int,QString)), this, SLOT(onTlsError(int,QString)));
        connect(sock, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
        connect(sock, SIGNAL(bytesWritten(qint64)), this, SLOT(onBytesWritten(qint64)));
    }

public slots:
    void onEncrypted() { encryptedReceived = true; }
    void onDisconnected() { disconnectedReceived = true; }
    void onTlsError(int code, const QString &msg) {
        errorReceived = true;
        lastErrorCode = code;
        lastErrorMsg = msg;
    }
    void onReadyRead() { readyReadCount++; }
    void onBytesWritten(qint64 n) { totalBytesWritten += n; }
};

class RawAcceptServer : public QObject {
    Q_OBJECT
public:
    int listenFd;
    quint16 port;
    int acceptedFd;
    QSocketNotifier *notifier;

    RawAcceptServer(QObject *parent = 0)
        : QObject(parent), listenFd(-1), port(0), acceptedFd(-1), notifier(0)
    {
    }

    ~RawAcceptServer()
    {
        closeAll();
    }

    bool start()
    {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) return false;
        int opt = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        int flags = ::fcntl(listenFd, F_GETFL, 0);
        ::fcntl(listenFd, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in addr;
        ::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        if (::listen(listenFd, 4) != 0) {
            return false;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listenFd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
            return false;
        }
        port = ntohs(addr.sin_port);
        notifier = new QSocketNotifier(listenFd, QSocketNotifier::Read, this);
        connect(notifier, SIGNAL(activated(int)), this, SLOT(onAccept()));
        return true;
    }

    int takeAcceptedFd()
    {
        int fd = acceptedFd;
        acceptedFd = -1;
        return fd;
    }

    void closeAll()
    {
        if (notifier) {
            notifier->setEnabled(false);
            delete notifier;
            notifier = 0;
        }
        if (acceptedFd >= 0) {
            ::close(acceptedFd);
            acceptedFd = -1;
        }
        if (listenFd >= 0) {
            ::close(listenFd);
            listenFd = -1;
        }
    }

public slots:
    void onAccept()
    {
        if (acceptedFd >= 0 || listenFd < 0) return;
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int fd = ::accept(listenFd, reinterpret_cast<struct sockaddr*>(&peer), &len);
        if (fd >= 0) {
            acceptedFd = fd;
            if (notifier) notifier->setEnabled(false);
        }
    }
};

void testLocalIdentityPersistenceAndSecurityH2()
{
    TEST_CASE("H2 - LocalIdentity generation, 0600 permissions, fingerprint persistence, and corrupt/partial state rejection");

    QString baseDir = "/tmp/localsend_h2_identity_test";
    QDir(baseDir).removeRecursively();

    // 1. Fresh generation and permission verification
    QString dir1 = baseDir + "/fresh";
    localsend::LocalIdentity id1;
    QString err;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(dir1, &id1, &err), "fresh loadOrCreate must succeed");
    ASSERT_TRUE(id1.isValid(), "generated identity must be valid");
    ASSERT_TRUE(id1.fingerprint().length() == 64, "fingerprint must be 64 hex chars");
    ASSERT_TRUE(localsend::LocalIdentity::normalizeFingerprint(id1.fingerprint()) == id1.fingerprint(),
                "fingerprint must be normalized lowercase hex");

    struct stat keySt;
    ASSERT_TRUE(::stat(QFile::encodeName(id1.keyPath()).constData(), &keySt) == 0, "stat private key must succeed");
    ASSERT_TRUE((keySt.st_mode & 0777) == 0600, "private key file permissions must be strictly 0600");

    // Verify independent fingerprint calculation matches
    QString calcErr;
    QString calcFp = localsend::LocalIdentity::calculateFingerprint(id1.certPem(), &calcErr);
    ASSERT_TRUE(calcFp == id1.fingerprint(), "calculateFingerprint must match persisted identity fingerprint");

    // 2. Reload from disk preserves exact PEMs and fingerprint
    localsend::LocalIdentity idReload;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(dir1, &idReload, &err), "reload loadOrCreate must succeed");
    ASSERT_TRUE(idReload.fingerprint() == id1.fingerprint(), "reloaded fingerprint must be identical");
    ASSERT_TRUE(idReload.certPem() == id1.certPem(), "reloaded certPem must be identical");
    ASSERT_TRUE(idReload.keyPem() == id1.keyPem(), "reloaded keyPem must be identical");

    // 3. Fingerprint normalization helper tests
    QString upperWithColons = id1.fingerprint().toUpper().left(2) + ":" + id1.fingerprint().toUpper().mid(2);
    ASSERT_TRUE(localsend::LocalIdentity::normalizeFingerprint(upperWithColons) == id1.fingerprint(),
                "normalizeFingerprint handles uppercase and colons");
    ASSERT_TRUE(localsend::LocalIdentity::normalizeFingerprint("deadbeef").isEmpty(),
                "normalizeFingerprint rejects short strings");
    QString nonHex = id1.fingerprint();
    nonHex[10] = QChar('g');
    ASSERT_TRUE(localsend::LocalIdentity::normalizeFingerprint(nonHex).isEmpty(),
                "normalizeFingerprint rejects non-hex characters");

    // 4. Partial state (missing key) must fail visibly and preserve existing cert
    QString dirPartial = baseDir + "/partial";
    QDir().mkpath(dirPartial);
    QFile partialCert(dirPartial + "/device_cert.pem");
    ASSERT_TRUE(partialCert.open(QIODevice::WriteOnly), "create partial cert");
    partialCert.write(id1.certPem());
    partialCert.close();

    localsend::LocalIdentity idPartial;
    QString partialErr;
    ASSERT_FALSE(localsend::LocalIdentity::loadOrCreate(dirPartial, &idPartial, &partialErr),
                 "partial state (cert without key) must fail");
    ASSERT_TRUE(partialErr.contains("partial"), "error message reports partial state");
    ASSERT_TRUE(QFile::exists(dirPartial + "/device_cert.pem"), "existing cert must not be deleted on partial failure");
    ASSERT_FALSE(QFile::exists(dirPartial + "/device_key.pem"), "key must not be silently generated on partial failure");

    // 5. Insecure key permissions (0644) must fail visibly without overwriting
    QString dirInsecure = baseDir + "/insecure";
    localsend::LocalIdentity idInsecureInit;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(dirInsecure, &idInsecureInit, &err), "create insecure test dir");
    ASSERT_TRUE(::chmod(QFile::encodeName(idInsecureInit.keyPath()).constData(), 0644) == 0, "chmod 0644 key");

    localsend::LocalIdentity idInsecureReload;
    QString insecureErr;
    ASSERT_FALSE(localsend::LocalIdentity::loadOrCreate(dirInsecure, &idInsecureReload, &insecureErr),
                 "group/world readable private key must be rejected");
    ASSERT_TRUE(insecureErr.contains("0600"), "error message reports required 0600 permissions");

    // Restore 0600 and verify it loads again with the same fingerprint
    ASSERT_TRUE(::chmod(QFile::encodeName(idInsecureInit.keyPath()).constData(), 0600) == 0, "restore chmod 0600");
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(dirInsecure, &idInsecureReload, &insecureErr),
                "restoring 0600 allows loading original identity");
    ASSERT_TRUE(idInsecureReload.fingerprint() == idInsecureInit.fingerprint(),
                "original identity was preserved without replacement");

    // 6. Mismatched cert and key pair must fail visibly without overwriting
    QFile mismatchKey(idInsecureInit.keyPath());
    ASSERT_TRUE(mismatchKey.open(QIODevice::WriteOnly | QIODevice::Truncate), "open key for mismatch");
    mismatchKey.write(id1.keyPem());
    mismatchKey.close();
    ::chmod(QFile::encodeName(idInsecureInit.keyPath()).constData(), 0600);

    localsend::LocalIdentity idMismatch;
    QString mismatchErr;
    ASSERT_FALSE(localsend::LocalIdentity::loadOrCreate(dirInsecure, &idMismatch, &mismatchErr),
                 "mismatched cert and key must fail validation");
    ASSERT_TRUE(!mismatchErr.isEmpty(), "mismatch error reported");

    QDir(baseDir).removeRecursively();
    TEST_PASS();
}

void testPosixTlsSocketLoopbackHandshakeAndTransferH2()
{
    TEST_CASE("H2 - PosixTlsSocket non-blocking TLS 1.3 loopback handshake, pinning, and bidirectional transfer");

    QString idDir = "/tmp/localsend_h2_loopback_id";
    QDir(idDir).removeRecursively();
    localsend::LocalIdentity serverId;
    QString err;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(idDir, &serverId, &err), "server identity creation failed");

    posix_tls_context_t *serverCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    ASSERT_TRUE(serverCtx != 0, "serverCtx create");
    ASSERT_TRUE(posix_tls_context_set_credentials(serverCtx,
                    serverId.certPem().constData(), static_cast<size_t>(serverId.certPem().size()),
                    serverId.keyPem().constData(), static_cast<size_t>(serverId.keyPem().size())) == POSIX_TLS_OK,
                "serverCtx set credentials");

    posix_tls_context_t *clientCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    ASSERT_TRUE(clientCtx != 0, "clientCtx create");
    QByteArray pinAscii = serverId.fingerprint().toLatin1();
    ASSERT_TRUE(posix_tls_context_set_expected_fingerprint(clientCtx, pinAscii.constData()) == POSIX_TLS_OK,
                "clientCtx set expected fingerprint");

    RawAcceptServer rawServer;
    ASSERT_TRUE(rawServer.start(), "rawServer start");

    localsend::PosixTlsSocket clientSock(clientCtx);
    TlsSocketSpy clientSpy(&clientSock);
    ASSERT_TRUE(clientSock.connectToHostEncrypted(QHostAddress::LocalHost, rawServer.port),
                "client connectToHostEncrypted start");

    QTime t;
    t.start();
    while (rawServer.acceptedFd < 0 && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(rawServer.acceptedFd >= 0, "server accepted TCP connection");

    localsend::PosixTlsSocket serverSock(serverCtx);
    TlsSocketSpy serverSpy(&serverSock);
    ASSERT_TRUE(serverSock.startServerEncryption(rawServer.takeAcceptedFd()),
                "server startServerEncryption");

    t.restart();
    while ((!clientSpy.encryptedReceived || !serverSpy.encryptedReceived) &&
           !clientSpy.errorReceived && !serverSpy.errorReceived && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(clientSpy.encryptedReceived && serverSpy.encryptedReceived, "both client and server must complete TLS handshake");
    ASSERT_TRUE(clientSock.negotiatedTlsVersion() == "TLSv1.3", "negotiated protocol must be TLSv1.3");
    ASSERT_TRUE(serverSock.negotiatedTlsVersion() == "TLSv1.3", "server negotiated protocol must be TLSv1.3");
    ASSERT_TRUE(!clientSock.negotiatedCipher().isEmpty(), "negotiated cipher must not be empty");
    ASSERT_TRUE(clientSock.peerFingerprint() == serverId.fingerprint(), "client peerFingerprint must match server fingerprint");

    // Bidirectional multi-chunk payload transfer
    QByteArray clientToServer(131072, 0);
    for (int i = 0; i < clientToServer.size(); ++i) {
        clientToServer[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    QByteArray serverToClient(65536, 0);
    for (int i = 0; i < serverToClient.size(); ++i) {
        serverToClient[i] = static_cast<char>((i * 17 + 3) & 0xFF);
    }

    ASSERT_TRUE(clientSock.write(clientToServer) == clientToServer.size(), "client write 128 KiB");
    ASSERT_TRUE(serverSock.write(serverToClient) == serverToClient.size(), "server write 64 KiB");

    QByteArray rxByServer;
    QByteArray rxByClient;
    t.restart();
    while ((rxByServer.size() < clientToServer.size() || rxByClient.size() < serverToClient.size()) && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (serverSock.bytesAvailable() > 0) {
            rxByServer.append(serverSock.readAll());
        }
        if (clientSock.bytesAvailable() > 0) {
            rxByClient.append(clientSock.readAll());
        }
        usleep(2000);
    }

    ASSERT_TRUE(rxByServer == clientToServer, "server received exact 128 KiB payload");
    ASSERT_TRUE(rxByClient == serverToClient, "client received exact 64 KiB payload");

    clientSock.disconnectFromHost();
    t.restart();
    while ((!clientSpy.disconnectedReceived || !serverSpy.disconnectedReceived) && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (serverSock.bytesAvailable() > 0) rxByServer.append(serverSock.readAll());
        if (clientSock.bytesAvailable() > 0) rxByClient.append(clientSock.readAll());
        usleep(2000);
    }
    ASSERT_TRUE(clientSpy.disconnectedReceived && serverSpy.disconnectedReceived, "clean TLS shutdown completed on both ends");

    posix_tls_context_destroy(clientCtx);
    posix_tls_context_destroy(serverCtx);
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testPosixTlsSocketPinMismatchRejectionH2()
{
    TEST_CASE("H2 - PosixTlsSocket single-hex-digit pin mismatch rejects before any application payload");

    QString idDir = "/tmp/localsend_h2_pinmismatch_id";
    QDir(idDir).removeRecursively();
    localsend::LocalIdentity serverId;
    QString err;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(idDir, &serverId, &err), "server identity creation");

    posix_tls_context_t *serverCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    ASSERT_TRUE(posix_tls_context_set_credentials(serverCtx,
                    serverId.certPem().constData(), static_cast<size_t>(serverId.certPem().size()),
                    serverId.keyPem().constData(), static_cast<size_t>(serverId.keyPem().size())) == POSIX_TLS_OK,
                "serverCtx set credentials");

    // Flip the last hex digit of the expected fingerprint
    QString wrongPin = serverId.fingerprint();
    QChar last = wrongPin.at(63);
    wrongPin[63] = (last == QChar('0')) ? QChar('1') : QChar('0');

    posix_tls_context_t *clientCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    QByteArray wrongPinAscii = wrongPin.toLatin1();
    ASSERT_TRUE(posix_tls_context_set_expected_fingerprint(clientCtx, wrongPinAscii.constData()) == POSIX_TLS_OK,
                "clientCtx set tampered fingerprint");

    RawAcceptServer rawServer;
    ASSERT_TRUE(rawServer.start(), "rawServer start");

    localsend::PosixTlsSocket clientSock(clientCtx);
    TlsSocketSpy clientSpy(&clientSock);
    ASSERT_TRUE(clientSock.connectToHostEncrypted(QHostAddress::LocalHost, rawServer.port),
                "client connectToHostEncrypted start");

    QTime t;
    t.start();
    while (rawServer.acceptedFd < 0 && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(rawServer.acceptedFd >= 0, "server accepted TCP connection");

    localsend::PosixTlsSocket serverSock(serverCtx);
    TlsSocketSpy serverSpy(&serverSock);
    ASSERT_TRUE(serverSock.startServerEncryption(rawServer.takeAcceptedFd()), "server startServerEncryption");

    t.restart();
    while (!clientSpy.errorReceived && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(clientSpy.errorReceived, "client must emit tlsError on pin mismatch");
    ASSERT_FALSE(clientSpy.encryptedReceived, "client must NEVER emit encrypted on pin mismatch");
    ASSERT_TRUE(clientSock.lastTlsErrorCode() == POSIX_TLS_ERR_PIN_MISMATCH,
                "error code must be POSIX_TLS_ERR_PIN_MISMATCH");
    ASSERT_TRUE(clientSock.peerFingerprint() == serverId.fingerprint(),
                "client records actual peer fingerprint on mismatch");
    ASSERT_TRUE(clientSock.write("secret-payload", 14) == -1,
                "writing after pin mismatch failure must be rejected (-1)");
    ASSERT_TRUE(serverSock.bytesAvailable() == 0,
                "server must receive 0 application bytes when client rejects pin");

    clientSock.abort();
    serverSock.abort();
    posix_tls_context_destroy(clientCtx);
    posix_tls_context_destroy(serverCtx);
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testPosixTlsSocketBackpressureAndExactWriteRetryH2()
{
    TEST_CASE("H2 - PosixTlsSocket WANT_WRITE exact buffer retry, read buffer cap backpressure, and multi-frame drain");

    QString idDir = "/tmp/localsend_h2_backpressure_id";
    QDir(idDir).removeRecursively();
    localsend::LocalIdentity serverId;
    QString err;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(idDir, &serverId, &err), "server identity creation");

    posix_tls_context_t *serverCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    posix_tls_context_set_credentials(serverCtx,
        serverId.certPem().constData(), static_cast<size_t>(serverId.certPem().size()),
        serverId.keyPem().constData(), static_cast<size_t>(serverId.keyPem().size()));

    posix_tls_context_t *clientCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    QByteArray pinAscii = serverId.fingerprint().toLatin1();
    posix_tls_context_set_expected_fingerprint(clientCtx, pinAscii.constData());

    RawAcceptServer rawServer;
    ASSERT_TRUE(rawServer.start(), "rawServer start");

    localsend::PosixTlsSocket clientSock(clientCtx);
    TlsSocketSpy clientSpy(&clientSock);
    ASSERT_TRUE(clientSock.connectToHostEncrypted(QHostAddress::LocalHost, rawServer.port), "client connect");

    QTime t;
    t.start();
    while (rawServer.acceptedFd < 0 && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(rawServer.acceptedFd >= 0, "server accepted");

    int sfd = rawServer.takeAcceptedFd();
    int cfd = clientSock.socketDescriptor();
    int smallBuf = 65536;
    ::setsockopt(sfd, SOL_SOCKET, SO_RCVBUF, &smallBuf, sizeof(smallBuf));
    ::setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &smallBuf, sizeof(smallBuf));
    if (cfd >= 0) {
        ::setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &smallBuf, sizeof(smallBuf));
        ::setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &smallBuf, sizeof(smallBuf));
    }

    localsend::PosixTlsSocket serverSock(serverCtx);
    // Cap server decrypted read buffer at 32 KiB to enforce read backpressure
    serverSock.setReadBufferSize(32768);
    TlsSocketSpy serverSpy(&serverSock);
    ASSERT_TRUE(serverSock.startServerEncryption(sfd), "server startServerEncryption");

    t.restart();
    while ((!clientSpy.encryptedReceived || !serverSpy.encryptedReceived) && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(clientSpy.encryptedReceived && serverSpy.encryptedReceived, "handshake completed");

    // Queue 768 KiB on client without draining server yet
    const int payloadSize = 768 * 1024;
    QByteArray payload(payloadSize, 0);
    for (int i = 0; i < payloadSize; ++i) {
        payload[i] = static_cast<char>((i * 73 + 19) & 0xFF);
    }
    Sha256Context txSha;
    txSha.update(payload);
    QString expectedSha = txSha.finalHex();

    ASSERT_TRUE(clientSock.write(payload) == payloadSize, "client queued 768 KiB payload");

    // Pump event loop without reading from serverSock until server buffer hits 32 KiB cap
    t.restart();
    while (serverSock.bytesAvailable() < 32768 && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(1000);
    }

    ASSERT_TRUE(serverSock.bytesAvailable() == 32768,
                "server decrypted read buffer must respect 32 KiB readBufferSize cap under backpressure");
    ASSERT_TRUE(clientSock.wantWriteCount() > 0,
                "client must encounter POSIX_TLS_ERR_WANT_WRITE under backpressure");
    ASSERT_TRUE(clientSock.bytesToWrite() > 0,
                "client must still have pending bytesToWrite while backpressured");

    // Now drain server in chunks; m_drainTimer (onDrainMore) and QSocketNotifier must drain all 768 KiB
    QByteArray received;
    received.reserve(payloadSize);
    t.restart();
    while (received.size() < payloadSize && t.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (serverSock.bytesAvailable() > 0) {
            received.append(serverSock.read(16384));
        } else {
            usleep(1000);
        }
    }

    ASSERT_TRUE(received.size() == payloadSize, "all 768 KiB must be drained after backpressure release");
    Sha256Context rxSha;
    rxSha.update(received);
    ASSERT_TRUE(rxSha.finalHex() == expectedSha, "SHA-256 of 768 KiB backpressured transfer must match");
    ASSERT_TRUE(clientSock.writeRetrySameBufferCount() > 0,
                "PosixTlsSocket must retry WANT_WRITE with the exact same buffer pointer and length");

    clientSock.abort();
    serverSock.abort();
    posix_tls_context_destroy(clientCtx);
    posix_tls_context_destroy(serverCtx);
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testPosixTlsSocketHandshakeDeadlineAndCloseOwnershipH2()
{
    TEST_CASE("H2 - PosixTlsSocket handshake deadline timeout, graceful response flush, and single-close fd ownership");

    QString idDir = "/tmp/localsend_h2_deadline_id";
    QDir(idDir).removeRecursively();
    localsend::LocalIdentity serverId;
    QString err;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(idDir, &serverId, &err), "server identity creation");

    posix_tls_context_t *serverCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    posix_tls_context_set_credentials(serverCtx,
        serverId.certPem().constData(), static_cast<size_t>(serverId.certPem().size()),
        serverId.keyPem().constData(), static_cast<size_t>(serverId.keyPem().size()));

    posix_tls_context_t *clientCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    QByteArray pinAscii = serverId.fingerprint().toLatin1();
    posix_tls_context_set_expected_fingerprint(clientCtx, pinAscii.constData());

    // Part 1: Stalled peer triggers handshake timeout within deadline
    {
        RawAcceptServer stalledServer;
        ASSERT_TRUE(stalledServer.start(), "stalledServer start");

        localsend::PosixTlsSocket clientSock(clientCtx);
        TlsSocketSpy clientSpy(&clientSock);
        ASSERT_TRUE(clientSock.connectToHostEncrypted(QHostAddress::LocalHost, stalledServer.port, QString(), 200),
                    "client connect with 200ms deadline");

        QTime t;
        t.start();
        while (!clientSpy.errorReceived && t.elapsed() < 1500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            usleep(5000);
        }

        ASSERT_TRUE(clientSpy.errorReceived, "stalled handshake must time out and emit tlsError");
        ASSERT_TRUE(clientSpy.lastErrorMsg.contains("deadline"), "error message must report handshake deadline");
        ASSERT_TRUE(clientSock.state() == localsend::PosixTlsSocket::State_Error, "socket state must be State_Error");
    }

    // Part 2: Graceful response flush before close_notify & single-close fd ownership (zero EBADF)
    {
        RawAcceptServer rawServer;
        ASSERT_TRUE(rawServer.start(), "rawServer start");

        localsend::PosixTlsSocket *clientSock = new localsend::PosixTlsSocket(clientCtx);
        TlsSocketSpy clientSpy(clientSock);
        ASSERT_TRUE(clientSock->connectToHostEncrypted(QHostAddress::LocalHost, rawServer.port), "client connect");

        QTime t;
        t.start();
        while (rawServer.acceptedFd < 0 && t.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            usleep(2000);
        }
        int sfd = rawServer.takeAcceptedFd();
        ASSERT_TRUE(sfd >= 0, "accepted sfd valid");

        localsend::PosixTlsSocket *serverSock = new localsend::PosixTlsSocket(serverCtx);
        TlsSocketSpy serverSpy(serverSock);
        ASSERT_TRUE(serverSock->startServerEncryption(sfd), "server startServerEncryption");

        int cfd = clientSock->socketDescriptor();
        ASSERT_TRUE(cfd >= 0, "client fd valid");
        ASSERT_TRUE(::fcntl(sfd, F_GETFD) >= 0, "sfd open during handshake");
        ASSERT_TRUE(::fcntl(cfd, F_GETFD) >= 0, "cfd open during handshake");

        t.restart();
        while ((!clientSpy.encryptedReceived || !serverSpy.encryptedReceived) && t.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            usleep(2000);
        }
        ASSERT_TRUE(clientSpy.encryptedReceived && serverSpy.encryptedReceived, "encrypted");

        // Server writes 64 KiB response and immediately calls disconnectFromHost()
        QByteArray responseData(65536, 'R');
        ASSERT_TRUE(serverSock->write(responseData) == responseData.size(), "server write 64 KiB response");
        serverSock->disconnectFromHost();

        QByteArray clientRead;
        t.restart();
        while ((clientRead.size() < responseData.size() || !clientSpy.disconnectedReceived) && t.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (clientSock->bytesAvailable() > 0) {
                clientRead.append(clientSock->readAll());
            }
            usleep(2000);
        }

        ASSERT_TRUE(clientRead == responseData, "client must receive complete 64 KiB response flushed before close_notify");
        ASSERT_TRUE(clientSpy.disconnectedReceived, "client must observe clean disconnect after close_notify");

        // Both descriptors must already be closed once by posix_tls_connection_destroy
        errno = 0;
        ASSERT_TRUE(::fcntl(sfd, F_GETFD) == -1 && errno == EBADF, "sfd must be closed after disconnect");
        errno = 0;
        ASSERT_TRUE(::fcntl(cfd, F_GETFD) == -1 && errno == EBADF, "cfd must be closed after disconnect");
        ASSERT_TRUE(serverSock->socketDescriptor() == -1, "serverSock descriptor reset to -1");
        ASSERT_TRUE(clientSock->socketDescriptor() == -1, "clientSock descriptor reset to -1");

        // Deleting PosixTlsSocket objects must not attempt a second close
        delete serverSock;
        delete clientSock;
    }

    posix_tls_context_destroy(clientCtx);
    posix_tls_context_destroy(serverCtx);
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testPosixTlsSocketInitAndSyncConnectH2A()
{
    TEST_CASE("H2 - PosixTlsSocket H2-A pre-handshake errors and synchronous connect handling");

    // Case 1: Null context initialization fails cleanly with error signal and state State_Error
    {
        localsend::PosixTlsSocket nullSock(0);
        TlsSocketSpy spy(&nullSock);
        bool ok = nullSock.connectToHostEncrypted(QHostAddress::LocalHost, 8080);
        ASSERT_FALSE(ok, "connect with null context must return false");
        ASSERT_TRUE(nullSock.state() == localsend::PosixTlsSocket::State_Error, "state must be State_Error");
        ASSERT_TRUE(nullSock.lastTlsErrorCode() == POSIX_TLS_ERR_INVALID_ARG, "code must be POSIX_TLS_ERR_INVALID_ARG");
        ASSERT_TRUE(spy.errorReceived, "error signal must be emitted");
        ASSERT_TRUE(spy.lastErrorCode == POSIX_TLS_ERR_INVALID_ARG, "spy captured error code");

        // Subsequent reuse after error
        bool ok2 = nullSock.connectToHostEncrypted(QHostAddress::LocalHost, 8080);
        ASSERT_FALSE(ok2, "reused socket with null context must return false");
        ASSERT_TRUE(nullSock.state() == localsend::PosixTlsSocket::State_Error, "state remains State_Error");
    }

    // Case 2: Invalid address fails cleanly with error signal
    {
        posix_tls_context_t *ctx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
        localsend::PosixTlsSocket sock(ctx);
        TlsSocketSpy spy(&sock);
        bool ok = sock.connectToHostEncrypted(QHostAddress::Null, 8080);
        ASSERT_FALSE(ok, "connect with null address must return false");
        ASSERT_TRUE(sock.state() == localsend::PosixTlsSocket::State_Error, "state must be State_Error");
        ASSERT_TRUE(spy.errorReceived, "error signal must be emitted for invalid address");
        posix_tls_context_destroy(ctx);
    }

    // Case 3: Invalid adopted descriptor fails cleanly and closes nothing invalid
    {
        posix_tls_context_t *ctx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
        localsend::PosixTlsSocket sock(ctx);
        TlsSocketSpy spy(&sock);
        bool ok = sock.startServerEncryption(-1);
        ASSERT_FALSE(ok, "startServerEncryption with fd -1 must return false");
        ASSERT_TRUE(sock.state() == localsend::PosixTlsSocket::State_Error, "state must be State_Error");
        ASSERT_TRUE(spy.errorReceived, "error signal must be emitted for fd -1");
        posix_tls_context_destroy(ctx);
    }

    // Case 4: Synchronous TCP connect proceeds to State_Handshaking with valid descriptor
    {
        RawAcceptServer server;
        ASSERT_TRUE(server.start(), "server start for synchronous connect test");

        posix_tls_context_t *ctx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
        localsend::PosixTlsSocket clientSock(ctx);
        bool ok = clientSock.connectToHostEncrypted(QHostAddress::LocalHost, server.port);
        ASSERT_TRUE(ok, "connectToHostEncrypted must return true on initial TCP connect dispatch");
        ASSERT_TRUE(clientSock.state() == localsend::PosixTlsSocket::State_ConnectingTcp ||
                    clientSock.state() == localsend::PosixTlsSocket::State_Handshaking,
                    "state must be connecting or handshaking");
        ASSERT_TRUE(clientSock.socketDescriptor() >= 0, "socketDescriptor must be valid fd >= 0");

        clientSock.abort();
        ASSERT_TRUE(clientSock.state() == localsend::PosixTlsSocket::State_Closed, "aborted socket state is Closed");
        ASSERT_TRUE(clientSock.socketDescriptor() == -1, "aborted socket descriptor is -1");
        posix_tls_context_destroy(ctx);
    }

    TEST_PASS();
}

void testPosixTlsSocketWriteClosureAndFdReleaseH2B()
{
    TEST_CASE("H2 - PosixTlsSocket H2-B write on closed connection terminates and releases fd");

    QString idDir = "/tmp/localsend_h2_writeclose_id";
    QDir(idDir).removeRecursively();
    localsend::LocalIdentity serverId;
    QString err;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(idDir, &serverId, &err), "identity creation");

    posix_tls_context_t *serverCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    posix_tls_context_set_credentials(serverCtx,
        serverId.certPem().constData(), static_cast<size_t>(serverId.certPem().size()),
        serverId.keyPem().constData(), static_cast<size_t>(serverId.keyPem().size()));

    posix_tls_context_t *clientCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    QByteArray pinAscii = serverId.fingerprint().toLatin1();
    posix_tls_context_set_expected_fingerprint(clientCtx, pinAscii.constData());

    int fds[2];
    ASSERT_TRUE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");

    localsend::PosixTlsSocket serverSock(serverCtx);
    localsend::PosixTlsSocket clientSock(clientCtx);
    TlsSocketSpy clientSpy(&clientSock);

    ASSERT_TRUE(serverSock.startServerEncryption(fds[0]), "server startServerEncryption");
    ASSERT_TRUE(clientSock.startClientEncryption(fds[1]), "client startClientEncryption");

    QTime t;
    t.start();
    while ((!clientSock.isEncrypted() || !serverSock.isEncrypted()) && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(clientSock.isEncrypted() && serverSock.isEncrypted(), "both encrypted");

    // Close server abruptly
    serverSock.abort();

    // Now client writes data. The peer socket is closed, so posix_tls_write triggers POSIX_TLS_ERR_CLOSED
    // PosixTlsSocket must fail the socket with POSIX_TLS_ERR_CLOSED, clear pending write buffer, and release fd
    clientSock.write("hello after peer closed", 23);

    t.restart();
    while (!clientSpy.errorReceived && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(clientSpy.errorReceived, "client must emit tlsError on write to closed peer");
    ASSERT_TRUE(clientSock.state() == localsend::PosixTlsSocket::State_Error, "client state must be State_Error");
    ASSERT_TRUE(clientSock.bytesToWrite() == 0, "client pending write queue must be cleared (0)");
    ASSERT_TRUE(clientSock.socketDescriptor() == -1, "client socket descriptor must be released (-1)");

    clientSock.abort();
    posix_tls_context_destroy(clientCtx);
    posix_tls_context_destroy(serverCtx);
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testPosixTlsSocketFlushTimeoutAndShutdownH2C()
{
    TEST_CASE("H2 - PosixTlsSocket H2-C shutdown timeout with unsent bytes reports error and clears queue");

    QString idDir = "/tmp/localsend_h2_flushtimeout_id";
    QDir(idDir).removeRecursively();
    localsend::LocalIdentity serverId;
    QString err;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(idDir, &serverId, &err), "identity creation");

    posix_tls_context_t *serverCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    posix_tls_context_set_credentials(serverCtx,
        serverId.certPem().constData(), static_cast<size_t>(serverId.certPem().size()),
        serverId.keyPem().constData(), static_cast<size_t>(serverId.keyPem().size()));

    posix_tls_context_t *clientCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    QByteArray pinAscii = serverId.fingerprint().toLatin1();
    posix_tls_context_set_expected_fingerprint(clientCtx, pinAscii.constData());

    int fds[2];
    ASSERT_TRUE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    int smallBuf = 4096;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &smallBuf, sizeof(smallBuf));
    ::setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &smallBuf, sizeof(smallBuf));

    localsend::PosixTlsSocket serverSock(serverCtx);
    localsend::PosixTlsSocket clientSock(clientCtx);
    TlsSocketSpy clientSpy(&clientSock);

    ASSERT_TRUE(serverSock.startServerEncryption(fds[0]), "server start");
    ASSERT_TRUE(clientSock.startClientEncryption(fds[1]), "client start");

    QTime t;
    t.start();
    while ((!clientSock.isEncrypted() || !serverSock.isEncrypted()) && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(clientSock.isEncrypted() && serverSock.isEncrypted(), "both encrypted");

    // Queue 512 KiB data to induce backpressure without server draining
    QByteArray bigData(512 * 1024, 'Z');
    clientSock.write(bigData);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    qint64 pendingBefore = clientSock.bytesToWrite();
    ASSERT_TRUE(pendingBefore > 0, "bytes must remain pending due to backpressure");

    // Disconnect: socket enters State_Closing and starts 2000ms shutdown timer
    clientSock.disconnectFromHost();
    ASSERT_TRUE(clientSock.state() == localsend::PosixTlsSocket::State_Closing, "state must be State_Closing");

    // Pump past the 2000ms shutdown deadline without server reading
    t.restart();
    while (!clientSpy.errorReceived && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        usleep(5000);
    }

    ASSERT_TRUE(clientSpy.errorReceived, "client must emit tlsError on flush deadline exceeded");
    ASSERT_TRUE(clientSock.lastTlsErrorCode() == POSIX_TLS_ERR_IO_FAILED, "code must be POSIX_TLS_ERR_IO_FAILED");
    ASSERT_TRUE(clientSock.bytesToWrite() == 0, "pending bytes must be reset to 0 after failure");
    ASSERT_TRUE(clientSock.state() == localsend::PosixTlsSocket::State_Error, "state must be State_Error");

    clientSock.abort();
    serverSock.abort();
    posix_tls_context_destroy(clientCtx);
    posix_tls_context_destroy(serverCtx);
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testPosixTlsSocketWriteProgressDeadlineH2D()
{
    TEST_CASE("H2 - PosixTlsSocket H2-D bounded write progress deadline expiry");

    QString idDir = "/tmp/localsend_h2_writedeadline_id";
    QDir(idDir).removeRecursively();
    localsend::LocalIdentity serverId;
    QString err;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(idDir, &serverId, &err), "identity creation");

    posix_tls_context_t *serverCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    posix_tls_context_set_credentials(serverCtx,
        serverId.certPem().constData(), static_cast<size_t>(serverId.certPem().size()),
        serverId.keyPem().constData(), static_cast<size_t>(serverId.keyPem().size()));

    posix_tls_context_t *clientCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
    QByteArray pinAscii = serverId.fingerprint().toLatin1();
    posix_tls_context_set_expected_fingerprint(clientCtx, pinAscii.constData());

    int fds[2];
    ASSERT_TRUE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    int smallBuf = 4096;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &smallBuf, sizeof(smallBuf));
    ::setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &smallBuf, sizeof(smallBuf));

    localsend::PosixTlsSocket serverSock(serverCtx);
    localsend::PosixTlsSocket clientSock(clientCtx);
    TlsSocketSpy clientSpy(&clientSock);

    ASSERT_TRUE(serverSock.startServerEncryption(fds[0]), "server start");
    ASSERT_TRUE(clientSock.startClientEncryption(fds[1]), "client start");

    QTime t;
    t.start();
    while ((!clientSock.isEncrypted() || !serverSock.isEncrypted()) && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(clientSock.isEncrypted() && serverSock.isEncrypted(), "both encrypted");

    // Configure a short 150ms write progress deadline
    clientSock.setWriteTimeout(150);
    ASSERT_TRUE(clientSock.writeTimeout() == 150, "writeTimeout is 150ms");

    // Enqueue 512 KiB data while receiver does not read
    QByteArray bigData(512 * 1024, 'W');
    clientSock.write(bigData);

    // Pump past the 150ms deadline
    t.restart();
    while (!clientSpy.errorReceived && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(clientSpy.errorReceived, "write progress deadline expiry must emit tlsError");
    ASSERT_TRUE(clientSock.lastTlsErrorCode() == POSIX_TLS_ERR_IO_FAILED, "code must be POSIX_TLS_ERR_IO_FAILED");
    ASSERT_TRUE(clientSock.errorString().contains("Write progress deadline exceeded"), "error string matched");
    ASSERT_TRUE(clientSock.bytesToWrite() == 0, "pending bytes cleared to 0");
    ASSERT_TRUE(clientSock.socketDescriptor() == -1, "fd released");

    clientSock.abort();
    serverSock.abort();
    posix_tls_context_destroy(clientCtx);
    posix_tls_context_destroy(serverCtx);
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testLocalIdentityFifoRejectionH2E()
{
    TEST_CASE("H2 - LocalIdentity H2-E non-blocking rejection of FIFO certificate/key");

    QString dirPath = "/tmp/localsend_h2_fifo_id";
    QDir(dirPath).removeRecursively();
    QDir().mkpath(dirPath);

    // Create a FIFO at device_cert.pem
    QString fifoPath = dirPath + "/device_cert.pem";
    ASSERT_TRUE(::mkfifo(QFile::encodeName(fifoPath).constData(), 0600) == 0, "create FIFO for cert");

    localsend::LocalIdentity id;
    QString err;
    QTime t;
    t.start();
    bool ok = localsend::LocalIdentity::loadOrCreate(dirPath, &id, &err);
    int elapsed = t.elapsed();

    ASSERT_FALSE(ok, "loadOrCreate must fail on FIFO identity file");
    ASSERT_TRUE(elapsed < 1000, "FIFO check must not hang waiting on open (must be < 1000ms)");
    ASSERT_TRUE(!err.isEmpty(), "error message must be populated");

    ::unlink(QFile::encodeName(fifoPath).constData());
    QDir(dirPath).removeRecursively();
    TEST_PASS();
}

// -------------------------------------------------------------
// Milestone H3: Outgoing HTTPS with Frozen Peer Pin Mock & Tests
// -------------------------------------------------------------

class MockHttpsServer : public RawAcceptServer {
    Q_OBJECT
public:
    localsend::LocalIdentity identity;
    posix_tls_context_t *serverCtx;
    QList<localsend::PosixTlsSocket*> activeSockets;
    QMap<localsend::PosixTlsSocket*, QByteArray> readBuffers;
    QMap<localsend::PosixTlsSocket*, qint64> expectedBodyLens;
    QMap<localsend::PosixTlsSocket*, QString> activePaths;

    int prepareStatusCode;
    int uploadStatusCode;
    bool holdUpload;
    QByteArray receivedUploadData;
    QString lastCancelSessionId;
    int connectionsCount;

    MockHttpsServer(QObject *parent = 0)
        : RawAcceptServer(parent)
        , serverCtx(0)
        , prepareStatusCode(200)
        , uploadStatusCode(200)
        , holdUpload(false)
        , connectionsCount(0)
    {
    }

    virtual ~MockHttpsServer()
    {
        stop();
    }

    void stop()
    {
        closeAll();
        for (int i = 0; i < activeSockets.size(); ++i) {
            localsend::PosixTlsSocket *s = activeSockets.at(i);
            s->disconnect(this);
            s->abort();
            delete s;
        }
        activeSockets.clear();
        readBuffers.clear();
        expectedBodyLens.clear();
        activePaths.clear();
        QCoreApplication::sendPostedEvents(0, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        if (serverCtx) {
            posix_tls_context_destroy(serverCtx);
            serverCtx = 0;
        }
    }

    bool init(const QString &storageDir)
    {
        QDir(storageDir).removeRecursively();
        QString err;
        if (!localsend::LocalIdentity::loadOrCreate(storageDir, &identity, &err)) {
            return false;
        }
        serverCtx = posix_tls_context_create(POSIX_TLS_POLICY_P2P);
        if (!serverCtx) return false;
        if (posix_tls_context_set_credentials(serverCtx,
                identity.certPem().constData(), static_cast<size_t>(identity.certPem().size()),
                identity.keyPem().constData(), static_cast<size_t>(identity.keyPem().size())) != POSIX_TLS_OK) {
            return false;
        }
        return start();
    }

public slots:
    void onAccept()
    {
        if (listenFd < 0) return;
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int fd = ::accept(listenFd, reinterpret_cast<struct sockaddr*>(&peer), &len);
        if (fd < 0) return;

        connectionsCount++;
        localsend::PosixTlsSocket *sock = new localsend::PosixTlsSocket(serverCtx, this);
        activeSockets.append(sock);
        readBuffers.insert(sock, QByteArray());
        expectedBodyLens.insert(sock, -1);

        connect(sock, SIGNAL(readyRead()), this, SLOT(onClientReadyRead()));
        connect(sock, SIGNAL(disconnected()), this, SLOT(onClientDisconnected()));

        sock->startServerEncryption(fd);
    }

    void onClientReadyRead()
    {
        localsend::PosixTlsSocket *sock = qobject_cast<localsend::PosixTlsSocket*>(sender());
        if (!sock) return;

        QByteArray data = sock->readAll();
        QByteArray &buf = readBuffers[sock];
        buf.append(data);

        if (expectedBodyLens[sock] < 0) {
            int idx = buf.indexOf("\r\n\r\n");
            if (idx >= 0) {
                QByteArray headerBlock = buf.left(idx);
                buf = buf.mid(idx + 4);

                QString headerText = QString::fromLatin1(headerBlock.constData(), headerBlock.size());
                QStringList lines = headerText.split(QRegExp("\r?\n"));
                QString reqLine = lines.first();
                QStringList reqParts = reqLine.split(' ');
                QString path = reqParts.size() >= 2 ? reqParts.at(1) : "";
                activePaths[sock] = path;

                qint64 contentLen = 0;
                for (int i = 1; i < lines.size(); ++i) {
                    const QString &l = lines.at(i);
                    int cIdx = l.indexOf(':');
                    if (cIdx > 0 && l.left(cIdx).trimmed().compare("content-length", Qt::CaseInsensitive) == 0) {
                        contentLen = l.mid(cIdx + 1).trimmed().toLongLong();
                    }
                }
                expectedBodyLens[sock] = contentLen;
            }
        }

        qint64 expected = expectedBodyLens[sock];
        if (expected >= 0 && buf.size() >= expected) {
            QString path = activePaths[sock];
            if (path.startsWith("/api/localsend/v2/prepare-upload")) {
                if (prepareStatusCode == 200) {
                    PrepareUploadRequest prepReq;
                    QString pErr;
                    ProtocolJson::decodePrepareRequest(buf.left(expected), &prepReq, &pErr);
                    PrepareUploadResponse resp;
                    resp.sessionId = "https-sess-1";
                    if (!prepReq.files.isEmpty()) {
                        resp.fileTokens.insert(prepReq.files.keys().first(), "https-tok-1");
                    }
                    QByteArray resBody = ProtocolJson::encodePrepareResponse(resp, &pErr);
                    QByteArray reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                     + QByteArray::number(resBody.size()) + "\r\n\r\n" + resBody;
                    sock->write(reply);
                    sock->flush();
                } else if (prepareStatusCode == 204) {
                    sock->write("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    sock->flush();
                } else {
                    sock->write(QString("HTTP/1.1 %1 Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").arg(prepareStatusCode).toLatin1());
                    sock->flush();
                }
            } else if (path.startsWith("/api/localsend/v2/upload")) {
                receivedUploadData = buf.left(expected);
                if (!holdUpload) {
                    if (uploadStatusCode == 200) {
                        sock->write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        sock->flush();
                    } else {
                        sock->write(QString("HTTP/1.1 %1 Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").arg(uploadStatusCode).toLatin1());
                        sock->flush();
                    }
                }
            } else if (path.startsWith("/api/localsend/v2/cancel")) {
                int qIdx = path.indexOf("sessionId=");
                if (qIdx > 0) {
                    lastCancelSessionId = path.mid(qIdx + 10).split('&').first();
                }
                sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                sock->flush();
            } else if (path.startsWith("/api/localsend/v2/register")) {
                DeviceInfo regInfo;
                QString rErr;
                ProtocolJson::decodeRegisterRequest(buf.left(expected), &regInfo, &rErr);
                DeviceInfo myInfo;
                myInfo.alias = "Mock HTTPS Host";
                myInfo.deviceModel = "QNX BB10";
                myInfo.fingerprint = identity.fingerprint();
                myInfo.port = port;
                myInfo.protocol = "https";
                myInfo.version = "2.2";
                QByteArray resBody = ProtocolJson::encodeDeviceResponse(myInfo, &rErr);
                QByteArray reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                 + QByteArray::number(resBody.size()) + "\r\n\r\n" + resBody;
                sock->write(reply);
                sock->flush();
            }
        }
    }

    void onClientDisconnected()
    {
        localsend::PosixTlsSocket *sock = qobject_cast<localsend::PosixTlsSocket*>(sender());
        if (sock) {
            activeSockets.removeAll(sock);
            readBuffers.remove(sock);
            expectedBodyLens.remove(sock);
            activePaths.remove(sock);
            sock->deleteLater();
        }
    }
};

void testSendHttpsPeerMissingPinRejectedH3()
{
    TEST_CASE("H3 - Sender rejects HTTPS peer with missing or empty certificate fingerprint");
    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = 53317;
    peer.info.protocol = "https";
    peer.info.fingerprint = ""; // Missing PIN

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "temporary file open failed");
    tmpFile.write("payload");
    tmpFile.flush();

    QString err;
    bool ok = sm.sendFile(peer, tmpFile.fileName(), &err);
    ASSERT_FALSE(ok, "sendFile must reject HTTPS peer without fingerprint");
    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_PeerPinRequired,
                "failure reason must be Failure_PeerPinRequired");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must remain free");
    TEST_PASS();
}

void testSendHttpsPeerInvalidPinFormatH3()
{
    TEST_CASE("H3 - Sender rejects HTTPS peer with invalid hex pin format");
    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = 53317;
    peer.info.protocol = "https";
    peer.info.fingerprint = "not_a_valid_64_hex_fingerprint_too_short";

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "temporary file open failed");
    tmpFile.write("payload");
    tmpFile.flush();

    QString err;
    bool ok = sm.sendFile(peer, tmpFile.fileName(), &err);
    ASSERT_FALSE(ok, "sendFile must reject malformed fingerprint");
    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_PeerPinRequired,
                "failure reason must be Failure_PeerPinRequired");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must remain free");
    TEST_PASS();
}

void testSendHttpsPinMismatchRejectionH3()
{
    TEST_CASE("H3 - Single hex digit pin mismatch aborts during TLS handshake before sending payload");
    MockHttpsServer mockServer;
    ASSERT_TRUE(mockServer.init("/tmp/localsend_h3_mismatch_srv"), "mock server init failed");

    QString validPin = mockServer.identity.fingerprint();
    QString tamperedPin = validPin;
    tamperedPin[0] = (validPin[0] == 'a') ? 'b' : 'a';

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    TestSendTracker tracker;
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));
    QObject::connect(&sm, SIGNAL(sendFailedWithReason(QString,QString,int)), &tracker, SLOT(onFailedWithReason(QString,QString,int)));

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = mockServer.port;
    peer.info.protocol = "https";
    peer.info.fingerprint = tamperedPin;

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "temporary file open failed");
    tmpFile.write("strictly confidential payload");
    tmpFile.flush();

    QString err;
    bool ok = sm.sendFile(peer, tmpFile.fileName(), &err);
    ASSERT_TRUE(ok, "sendFile accepted valid request format");

    QTime t;
    t.start();
    while (tracker.failedReason.isEmpty() && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(sm.state() == SendManager::State_Failed, "state must transition to Failed on pin mismatch");
    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_PeerPinRequired,
                "failure reason must be Failure_PeerPinRequired on pin mismatch");
    ASSERT_TRUE(mockServer.receivedUploadData.isEmpty(),
                "server must not receive any upload data on pin mismatch");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must be released cleanly");

    mockServer.stop();
    TEST_PASS();
}

void testSendHttpsSuccessfulUploadAndAckH3()
{
    TEST_CASE("H3 - Outgoing HTTPS prepare 200, streaming upload, and peer 200 acknowledgment with Chinese filename");
    MockHttpsServer mockServer;
    ASSERT_TRUE(mockServer.init("/tmp/localsend_h3_success_srv"), "mock server init failed");

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    TestSendTracker tracker;
    QObject::connect(&sm, SIGNAL(sendStateChanged(QString,QString)), &tracker, SLOT(onStateChanged(QString,QString)));
    QObject::connect(&sm, SIGNAL(sendProgress(QString,qint64,qint64)), &tracker, SLOT(onProgress(QString,qint64,qint64)));
    QObject::connect(&sm, SIGNAL(sendFinished(QString)), &tracker, SLOT(onFinished(QString)));
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = mockServer.port;
    peer.info.protocol = "https";
    peer.info.fingerprint = mockServer.identity.fingerprint();

    QByteArray payload(32768, 'X'); // 32 KiB binary payload
    QString testFilePath = "/tmp/localsend_h3_测试文件.bin";
    QFile f(testFilePath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly), "create test file failed");
    f.write(payload);
    f.close();

    QString err;
    bool ok = sm.sendFile(peer, testFilePath, &err);
    ASSERT_TRUE(ok, "sendFile failed to start HTTPS transfer");

    QTime t;
    t.start();
    while (!tracker.finished && tracker.failedReason.isEmpty() && t.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(tracker.finished, "sendFinished must be emitted");
    ASSERT_TRUE(tracker.failedReason.isEmpty(), "send must not fail");
    ASSERT_TRUE(sm.state() == SendManager::State_Completed, "state must be Completed");
    ASSERT_TRUE(mockServer.receivedUploadData == payload, "uploaded data on server must match exact payload");
    ASSERT_TRUE(tracker.lastProgressDone == 32768, "progress done must reach 32768");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must be released after completion");

    f.remove();
    mockServer.stop();
    TEST_PASS();
}

void testSendHttpsActivePinStabilityH3()
{
    TEST_CASE("H3 - Active pin stability: announcement mutation during transfer cannot alter frozen pin");
    MockHttpsServer mockServer;
    ASSERT_TRUE(mockServer.init("/tmp/localsend_h3_pin_stability"), "mock server init failed");

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    TestSendTracker tracker;
    QObject::connect(&sm, SIGNAL(sendFinished(QString)), &tracker, SLOT(onFinished(QString)));
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &tracker, SLOT(onFailed(QString,QString)));

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = mockServer.port;
    peer.info.protocol = "https";
    peer.info.fingerprint = mockServer.identity.fingerprint();

    QByteArray payload(8192, 'S');
    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "create temp file failed");
    tmpFile.write(payload);
    tmpFile.flush();

    QString err;
    bool ok = sm.sendFile(peer, tmpFile.fileName(), &err);
    ASSERT_TRUE(ok, "sendFile start");

    // Frozen pin must match server identity
    ASSERT_TRUE(sm.activePin() == mockServer.identity.fingerprint(), "activePin must be frozen from target peer");

    // Simulate concurrent discovery mutation attempt with different pin
    peer.info.fingerprint = "0000000000000000000000000000000000000000000000000000000000000000";

    // Active transfer pin must stay completely unaffected
    ASSERT_TRUE(sm.activePin() == mockServer.identity.fingerprint(),
                "activePin must not be corrupted by external peer modification");

    QTime t;
    t.start();
    while (!tracker.finished && tracker.failedReason.isEmpty() && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(tracker.finished, "transfer must succeed with frozen pin");
    ASSERT_TRUE(sm.state() == SendManager::State_Completed, "state must reach Completed");

    mockServer.stop();
    TEST_PASS();
}

void testSendHttpsLocalCancellationAndRemoteCancelH3()
{
    TEST_CASE("H3 - Outgoing HTTPS local cancellation, remote cancel delivery, and clean recovery");
    MockHttpsServer mockServer;
    ASSERT_TRUE(mockServer.init("/tmp/localsend_h3_cancel_srv"), "mock server init failed");

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = mockServer.port;
    peer.info.protocol = "https";
    peer.info.fingerprint = mockServer.identity.fingerprint();

    QByteArray bigPayload(262144, 'C'); // 256 KiB
    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "create temp file failed");
    tmpFile.write(bigPayload);
    tmpFile.flush();

    mockServer.holdUpload = true;
    QString err;
    bool ok = sm.sendFile(peer, tmpFile.fileName(), &err);
    ASSERT_TRUE(ok, "sendFile start");

    QTime t;
    t.start();
    while (sm.state() != SendManager::State_Uploading && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(sm.state() == SendManager::State_Uploading, "state must reach Uploading");
    QString activeSessionId = sm.activeSessionId();

    // Cancel locally during active upload
    sm.cancel();
    ASSERT_TRUE(sm.state() == SendManager::State_Cancelled, "state must transition to Cancelled");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must be released immediately on cancel");

    // Pump events for remote cancel HTTP delivery
    t.restart();
    while (mockServer.lastCancelSessionId.isEmpty() && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(mockServer.lastCancelSessionId == "https-sess-1",
                "remote peer must receive cancel request with active sessionId");

    // Recovery test: immediately start a new valid transfer; verify it succeeds completely
    mockServer.holdUpload = false;
    TestSendTracker recoveryTracker;
    QObject::connect(&sm, SIGNAL(sendFinished(QString)), &recoveryTracker, SLOT(onFinished(QString)));
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &recoveryTracker, SLOT(onFailed(QString,QString)));

    QByteArray recoveryPayload(4096, 'R');
    QTemporaryFile recoveryFile;
    ASSERT_TRUE(recoveryFile.open(), "recovery file open");
    recoveryFile.write(recoveryPayload);
    recoveryFile.flush();

    ok = sm.sendFile(peer, recoveryFile.fileName(), &err);
    ASSERT_TRUE(ok, "recovery transfer start must succeed");

    t.restart();
    while (!recoveryTracker.finished && recoveryTracker.failedReason.isEmpty() && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(recoveryTracker.finished, "recovery transfer must finish successfully");
    ASSERT_TRUE(sm.state() == SendManager::State_Completed, "recovery transfer state must be Completed");

    mockServer.stop();
    TEST_PASS();
}

void testSendHttpsFailureStatusResponsesH3()
{
    TEST_CASE("H3 - Outgoing HTTPS server failure responses (403, 409, 422) map to distinct failure reasons");
    MockHttpsServer mockServer;
    ASSERT_TRUE(mockServer.init("/tmp/localsend_h3_fail_srv"), "mock server init failed");

    localsend::Application app;
    QNetworkAccessManager nam;
    SendManager sm(&app, &nam);

    PeerDevice peer;
    peer.address = QHostAddress::LocalHost;
    peer.info.port = mockServer.port;
    peer.info.protocol = "https";
    peer.info.fingerprint = mockServer.identity.fingerprint();

    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "create temp file");
    tmpFile.write("test_payload");
    tmpFile.flush();

    // 1. Prepare 403 Forbidden -> Failure_PeerRejected
    mockServer.prepareStatusCode = 403;
    TestSendTracker t1;
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &t1, SLOT(onFailed(QString,QString)));
    sm.sendFile(peer, tmpFile.fileName());

    QTime t;
    t.start();
    while (t1.failedReason.isEmpty() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_PeerRejected, "prepare 403 must yield Failure_PeerRejected");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot released");

    // 2. Prepare 409 Conflict -> Failure_PeerBusy
    mockServer.prepareStatusCode = 409;
    TestSendTracker t2;
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &t2, SLOT(onFailed(QString,QString)));
    sm.sendFile(peer, tmpFile.fileName());

    t.restart();
    while (t2.failedReason.isEmpty() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_PeerBusy, "prepare 409 must yield Failure_PeerBusy");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot released");

    // 3. Upload 422 Unprocessable Entity -> Failure_ChecksumMismatch
    mockServer.prepareStatusCode = 200;
    mockServer.uploadStatusCode = 422;
    TestSendTracker t3;
    QObject::connect(&sm, SIGNAL(sendFailed(QString,QString)), &t3, SLOT(onFailed(QString,QString)));
    sm.sendFile(peer, tmpFile.fileName());

    t.restart();
    while (t3.failedReason.isEmpty() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(sm.failureReason() == SendManager::Failure_ChecksumMismatch,
                "upload 422 must yield Failure_ChecksumMismatch");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot released");

    mockServer.stop();
    TEST_PASS();
}

class StreamWatcher : public QObject {
    Q_OBJECT
public:
    bool *fin;
    int *code;
    qint64 *prog;
    StreamWatcher(bool *f, int *c, qint64 *p, QObject *parent = 0)
        : QObject(parent), fin(f), code(c), prog(p) {}

public slots:
    void onProgress(qint64 done, qint64 total) { Q_UNUSED(total); if (prog) *prog = done; }
    void onFinished(int c, const QByteArray &b) { Q_UNUSED(b); if (fin) *fin = true; if (code) *code = c; }
};

class RegisterPeerWatcher : public QObject {
    Q_OBJECT
public:
    bool *called;
    PeerDevice *outPeer;
    quint16 port;
    RegisterPeerWatcher(bool *c, PeerDevice *p, quint16 pt, QObject *parent = 0)
        : QObject(parent), called(c), outPeer(p), port(pt) {}

public slots:
    void onPeerUpdated(const PeerDevice &peer) {
        if (peer.address == QHostAddress::LocalHost && peer.info.port == port) {
            *called = true;
            *outPeer = peer;
        }
    }
};

void testPosixTlsHttpClientBoundedStreamingH3()
{
    TEST_CASE("H3 - PosixTlsHttpClient non-blocking bounded streaming and progress tracking");
    MockHttpsServer mockServer;
    ASSERT_TRUE(mockServer.init("/tmp/localsend_h3_client_stream_srv"), "mock server init failed");

    localsend::PosixTlsHttpClient client;

    QByteArray streamData(131072, 'Z'); // 128 KiB
    QBuffer buffer(&streamData);
    ASSERT_TRUE(buffer.open(QIODevice::ReadOnly), "buffer open");

    QMap<QString, QString> headers;
    headers.insert("Content-Type", "application/octet-stream");

    bool finished = false;
    int respCode = 0;
    qint64 lastProgress = 0;

    StreamWatcher watcher(&finished, &respCode, &lastProgress);

    QObject::connect(&client, SIGNAL(uploadProgress(qint64,qint64)),
                     &watcher, SLOT(onProgress(qint64,qint64)));
    QObject::connect(&client, SIGNAL(finished(int,QByteArray)),
                     &watcher, SLOT(onFinished(int,QByteArray)));

    bool ok = client.sendStreamRequest("POST", QHostAddress::LocalHost, mockServer.port,
                                       "/api/localsend/v2/upload",
                                       mockServer.identity.fingerprint(),
                                       headers, &buffer, streamData.size(), 5000);
    ASSERT_TRUE(ok, "sendStreamRequest start");

    QTime t;
    t.start();
    while (!finished && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(finished, "client streaming request must complete");
    ASSERT_TRUE(respCode == 200, "client response code must be 200");
    ASSERT_TRUE(lastProgress == streamData.size(), "progress must reach total stream size");
    ASSERT_TRUE(mockServer.receivedUploadData == streamData, "server must receive exact stream bytes");

    mockServer.stop();
    TEST_PASS();
}

void testDiscoveryManagerOutgoingHttpsRegisterH3()
{
    TEST_CASE("H3 - DiscoveryManager outgoing HTTPS register with pinned posix-tls and pin verification");
    MockHttpsServer mockServer;
    ASSERT_TRUE(mockServer.init("/tmp/localsend_h3_dm_https_srv"), "mock server init failed");

    QNetworkAccessManager nam;
    DiscoveryManager dm(&nam);
    DeviceInfo localInfo;
    localInfo.alias = "Sender Device";
    localInfo.port = 53317;
    localInfo.fingerprint = QString(64, '1');
    dm.start(localInfo);

    // 1. Peer advertised as HTTPS with valid matching pin
    DeviceInfo peerInfo;
    peerInfo.alias = "Advertised Peer";
    peerInfo.port = mockServer.port;
    peerInfo.protocol = "https";
    peerInfo.fingerprint = mockServer.identity.fingerprint();
    dm.handleIncomingRegister(peerInfo, QHostAddress::LocalHost);

    bool peerUpdatedCalled = false;
    PeerDevice updatedPeer;
    RegisterPeerWatcher handler(&peerUpdatedCalled, &updatedPeer, mockServer.port);
    QObject::connect(&dm, SIGNAL(peerUpdated(PeerDevice)), &handler, SLOT(onPeerUpdated(PeerDevice)));

    dm.registerPeer(QHostAddress::LocalHost, mockServer.port);

    QElapsedTimer t;
    t.start();
    while (!peerUpdatedCalled && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(1000);
    }

    ASSERT_TRUE(peerUpdatedCalled, "peerUpdated must be emitted after HTTPS register success");
    ASSERT_TRUE(updatedPeer.info.alias == "Mock HTTPS Host", "alias received from mock server");
    ASSERT_TRUE(updatedPeer.info.protocol == "https", "protocol preserved as https");

    // 2. Peer with mismatched pin must reject TLS and NOT fallback to HTTP
    quint16 altPort = mockServer.port;
    QString wrongPin = mockServer.identity.fingerprint();
    wrongPin[0] = (wrongPin[0] == '0') ? '1' : '0';
    DeviceInfo wrongPeerInfo;
    wrongPeerInfo.alias = "Mismatch Peer";
    wrongPeerInfo.port = altPort;
    wrongPeerInfo.protocol = "https";
    wrongPeerInfo.fingerprint = wrongPin;
    dm.handleIncomingRegister(wrongPeerInfo, QHostAddress::LocalHost);

    peerUpdatedCalled = false;
    dm.registerPeer(QHostAddress::LocalHost, altPort);
    t.restart();
    while (t.elapsed() < 500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(1000);
    }
    ASSERT_FALSE(peerUpdatedCalled, "pin mismatch must fail without updating peer");

    dm.stop();
    mockServer.stop();
    TEST_PASS();
}

// -------------------------------------------------------------
// Milestone H4: Incoming HTTPS Server & Discovery/UI Wiring Tests
// -------------------------------------------------------------

class ClientResponseWatcher : public QObject {
    Q_OBJECT
public:
    bool finished;
    bool failed;
    int statusCode;
    int errorCode;
    QString errorMessage;
    QByteArray body;

    ClientResponseWatcher(QObject *parent = 0)
        : QObject(parent), finished(false), failed(false), statusCode(0), errorCode(0) {}

public slots:
    void onFinished(int code, const QByteArray &respBody) {
        finished = true;
        statusCode = code;
        body = respBody;
    }
    void onFailed(int code, const QString &msg) {
        failed = true;
        errorCode = code;
        errorMessage = msg;
    }
};

class ReceiveServerOfferWatcher : public QObject {
    Q_OBJECT
public:
    ReceiveServer *server;
    bool offerReceived;
    bool shouldAccept;
    QString requestId;
    QString fileId;
    QHostAddress peerAddress;
    bool receiveFinishedEmitted;
    QString savedPath;

    ReceiveServerOfferWatcher(ReceiveServer *srv, bool acceptOffer = true, QObject *parent = 0)
        : QObject(parent), server(srv), offerReceived(false), shouldAccept(acceptOffer), receiveFinishedEmitted(false) {}

public slots:
    void onIncomingOffer(const QString &reqId, const PeerDevice &peer, const QList<FileMeta> &files) {
        peerAddress = peer.address;
        offerReceived = true;
        requestId = reqId;
        if (!files.isEmpty()) {
            fileId = files.first().id;
        }
        if (server) {
            if (shouldAccept) {
                server->accept(reqId, fileId);
            } else {
                server->reject(reqId);
            }
        }
    }
    void onReceiveFinished(const QString &sessionId, const QString &path) {
        Q_UNUSED(sessionId);
        receiveFinishedEmitted = true;
        savedPath = path;
    }
};

class SimpleInfoResponder : public QObject {
    Q_OBJECT
public:
    SimpleInfoResponder(QObject *parent = 0) : QObject(parent), m_conn(0) {}
public slots:
    void onConnectionOpened(HttpConnection *conn) {
        m_conn = conn;
        connect(conn, SIGNAL(headersReady(HttpRequestHead)), this, SLOT(onHeaders(HttpRequestHead)));
    }
    void onHeaders(const HttpRequestHead &) {
        if (m_conn) {
            m_conn->respond(200, "application/json", "{\"status\":\"ok\"}");
        }
    }
private:
    HttpConnection *m_conn;
};

void testHttpServerInboundHttpsHandshakeAndInfoH4()
{
    TEST_CASE("H4 - HttpServer inbound HTTPS handshake, matching pin verification, and /info endpoint");
    QString idDir = "/tmp/localsend_h4_srv_info_id";
    QDir(idDir).removeRecursively();

    localsend::Application app;
    app.setIdentityStorageDirectory(idDir);
    app.setPort(0);
    QString startErr;
    ASSERT_TRUE(app.start(&startErr), "app start failed");
    ASSERT_TRUE(app.httpServer()->isSecure(), "HttpServer must be running in secure HTTPS mode");
    ASSERT_TRUE(app.localInfo().protocol == "https", "protocol advertised must be https");
    ASSERT_TRUE(app.localIdentity().isValid(), "local identity must be valid");

    quint16 srvPort = app.httpServer()->serverPort();
    QString expectedPin = app.localIdentity().fingerprint();

    localsend::PosixTlsHttpClient client;
    ClientResponseWatcher watcher;
    QObject::connect(&client, SIGNAL(finished(int,QByteArray)), &watcher, SLOT(onFinished(int,QByteArray)));
    QObject::connect(&client, SIGNAL(failed(int,QString)), &watcher, SLOT(onFailed(int,QString)));

    QMap<QString, QString> headers;
    bool reqOk = client.sendRequest("GET", QHostAddress::LocalHost, srvPort,
                                    "/api/localsend/v2/info", expectedPin, headers, QByteArray(), 5000);
    ASSERT_TRUE(reqOk, "sendRequest start");

    QElapsedTimer t;
    t.start();
    while (!watcher.finished && !watcher.failed && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(watcher.finished, "client request must complete");
    ASSERT_TRUE(watcher.statusCode == 200, "status code must be 200");
    ASSERT_TRUE(client.peerFingerprint() == expectedPin, "peer certificate fingerprint must match");

    DeviceInfo info;
    QString parseErr;
    ASSERT_TRUE(ProtocolJson::decodeRegisterRequest(watcher.body, &info, &parseErr), "decode /info response");
    ASSERT_TRUE(info.alias == app.localInfo().alias, "alias must match");
    ASSERT_TRUE(info.protocol == "https", "protocol in response must be https");

    app.stop();
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testHttpServerInboundHttpsRegisterH4()
{
    TEST_CASE("H4 - HttpServer inbound HTTPS /register endpoint and discovery update");
    QString idDir = "/tmp/localsend_h4_srv_reg_id";
    QDir(idDir).removeRecursively();

    localsend::Application app;
    app.setIdentityStorageDirectory(idDir);
    app.setPort(0);
    QString startErr;
    ASSERT_TRUE(app.start(&startErr), "app start failed");

    quint16 srvPort = app.httpServer()->serverPort();
    QString expectedPin = app.localIdentity().fingerprint();

    DeviceInfo peerInfo;
    peerInfo.alias = "Inbound HTTPS Peer";
    peerInfo.deviceModel = "Test Laptop";
    peerInfo.deviceType = "desktop";
    peerInfo.fingerprint = QString(64, 'e');
    peerInfo.port = 53317;
    peerInfo.protocol = "https";

    QString err;
    QByteArray reqBody = ProtocolJson::encodeRegisterRequest(peerInfo, &err);
    ASSERT_TRUE(!reqBody.isEmpty(), "encode register request");

    localsend::PosixTlsHttpClient client;
    ClientResponseWatcher watcher;
    QObject::connect(&client, SIGNAL(finished(int,QByteArray)), &watcher, SLOT(onFinished(int,QByteArray)));
    QObject::connect(&client, SIGNAL(failed(int,QString)), &watcher, SLOT(onFailed(int,QString)));

    QMap<QString, QString> headers;
    headers.insert("Content-Type", "application/json");
    bool reqOk = client.sendRequest("POST", QHostAddress::LocalHost, srvPort,
                                    "/api/localsend/v2/register", expectedPin, headers, reqBody, 5000);
    ASSERT_TRUE(reqOk, "sendRequest start");

    QElapsedTimer t;
    t.start();
    while (!watcher.finished && !watcher.failed && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(watcher.finished, "client request must complete");
    ASSERT_TRUE(watcher.statusCode == 200, "status code must be 200");

    PeerDevice recorded = app.discoveryManager()->findPeer(peerInfo.fingerprint);
    ASSERT_TRUE(!recorded.info.fingerprint.isEmpty(), "peer must be recorded in DiscoveryManager");
    ASSERT_TRUE(recorded.info.alias == "Inbound HTTPS Peer", "peer alias matches");
    ASSERT_TRUE(recorded.info.protocol == "https", "peer protocol matches https");

    app.stop();
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testReceiveServerHttpsPrepareAndAsyncConsentH4()
{
    TEST_CASE("H4 - ReceiveServer HTTPS /prepare-upload with asynchronous accept and reject");
    QString idDir = "/tmp/localsend_h4_srv_consent_id";
    QString rxDir = "/tmp/localsend_h4_consent_rx";
    QDir(idDir).removeRecursively();
    QDir(rxDir).removeRecursively();

    localsend::Application app;
    app.setIdentityStorageDirectory(idDir);
    app.setReceiveDirectory(rxDir);
    app.setPort(0);
    QString startErr;
    ASSERT_TRUE(app.start(&startErr), "app start failed");

    quint16 srvPort = app.httpServer()->serverPort();
    QString expectedPin = app.localIdentity().fingerprint();

    // 1. Asynchronous Accept flow
    ReceiveServerOfferWatcher acceptWatcher(app.receiveServer(), true);
    QObject::connect(app.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &acceptWatcher, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

    DeviceInfo clientInfo;
    clientInfo.alias = "Sender Device";
    clientInfo.fingerprint = QString(64, 'f');
    clientInfo.port = 53317;
    clientInfo.protocol = "https";

    FileMeta fm;
    fm.id = "file-consent-1";
    fm.fileName = "consent_test.txt";
    fm.size = 100;
    fm.fileType = "text/plain";

    QString err;
    QByteArray prepJson = ProtocolJson::encodePrepareRequest(clientInfo, fm, &err);

    localsend::PosixTlsHttpClient client1;
    ClientResponseWatcher respWatcher1;
    QObject::connect(&client1, SIGNAL(finished(int,QByteArray)), &respWatcher1, SLOT(onFinished(int,QByteArray)));
    QObject::connect(&client1, SIGNAL(failed(int,QString)), &respWatcher1, SLOT(onFailed(int,QString)));

    QMap<QString, QString> headers;
    headers.insert("Content-Type", "application/json");
    client1.sendRequest("POST", QHostAddress::LocalHost, srvPort,
                        "/api/localsend/v2/prepare-upload", expectedPin, headers, prepJson, 5000);

    QElapsedTimer t;
    t.start();
    while (!respWatcher1.finished && !respWatcher1.failed && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(acceptWatcher.offerReceived, "incomingOffer must be emitted");
    ASSERT_TRUE(!acceptWatcher.peerAddress.isNull(), "observed source address must not be null");
    ASSERT_TRUE(acceptWatcher.peerAddress == QHostAddress("127.0.0.1"), "observed source address must be 127.0.0.1");
    ASSERT_TRUE(respWatcher1.finished, "prepare-upload request must complete");
    ASSERT_TRUE(respWatcher1.statusCode == 200, "prepare-upload accept must return 200");

    PrepareUploadResponse prepResp;
    ASSERT_TRUE(ProtocolJson::decodePrepareResponse(respWatcher1.body, &prepResp, &err), "decode prepare response");
    ASSERT_TRUE(!prepResp.sessionId.isEmpty(), "sessionId must be present");
    ASSERT_TRUE(prepResp.fileTokens.contains("file-consent-1"), "file token must be present");

    // Clean active session from accept flow
    app.receiveServer()->cancelLocal();
    QObject::disconnect(app.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                        &acceptWatcher, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

    // 2. Asynchronous Reject flow
    ReceiveServerOfferWatcher rejectWatcher(app.receiveServer(), false); // shouldAccept = false
    QObject::connect(app.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &rejectWatcher, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

    fm.id = "file-consent-2";
    prepJson = ProtocolJson::encodePrepareRequest(clientInfo, fm, &err);

    localsend::PosixTlsHttpClient client2;
    ClientResponseWatcher respWatcher2;
    QObject::connect(&client2, SIGNAL(finished(int,QByteArray)), &respWatcher2, SLOT(onFinished(int,QByteArray)));
    QObject::connect(&client2, SIGNAL(failed(int,QString)), &respWatcher2, SLOT(onFailed(int,QString)));

    client2.sendRequest("POST", QHostAddress::LocalHost, srvPort,
                        "/api/localsend/v2/prepare-upload", expectedPin, headers, prepJson, 5000);

    t.restart();
    while (!respWatcher2.finished && !respWatcher2.failed && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(rejectWatcher.offerReceived, "incomingOffer must be emitted for reject");
    ASSERT_TRUE(respWatcher2.finished, "prepare-upload request must complete");
    ASSERT_TRUE(respWatcher2.statusCode == 403, "prepare-upload reject must return 403 Forbidden");
    ASSERT_TRUE(!app.isTransferActive(), "transfer slot must be freed after rejection");

    app.stop();
    QDir(idDir).removeRecursively();
    QDir(rxDir).removeRecursively();
    TEST_PASS();
}

void testReceiveServerHttpsStreamingUploadAndSha256H4()
{
    TEST_CASE("H4 - ReceiveServer HTTPS 64 KiB streaming upload and SHA-256 integrity verification");
    QString idDir = "/tmp/localsend_h4_srv_stream_id";
    QString rxDir = "/tmp/localsend_h4_stream_rx";
    QDir(idDir).removeRecursively();
    QDir(rxDir).removeRecursively();

    localsend::Application app;
    app.setIdentityStorageDirectory(idDir);
    app.setReceiveDirectory(rxDir);
    app.setPort(0);
    QString startErr;
    ASSERT_TRUE(app.start(&startErr), "app start failed");

    quint16 srvPort = app.httpServer()->serverPort();
    QString expectedPin = app.localIdentity().fingerprint();

    ReceiveServerOfferWatcher offerWatcher(app.receiveServer(), true);
    QObject::connect(app.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &offerWatcher, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));
    QObject::connect(app.receiveServer(), SIGNAL(receiveFinished(QString,QString)),
                     &offerWatcher, SLOT(onReceiveFinished(QString,QString)));

    // Create 64 KiB test payload
    QByteArray payload(65536, 'S');
    for (int i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    QString payloadSha = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();

    DeviceInfo clientInfo;
    clientInfo.alias = "Streaming Sender";
    clientInfo.fingerprint = QString(64, 'c');
    clientInfo.port = 53317;
    clientInfo.protocol = "https";

    FileMeta fm;
    fm.id = "file-stream-64k";
    fm.fileName = "stream_payload.bin";
    fm.size = payload.size();
    fm.sha256 = payloadSha;
    fm.fileType = "application/octet-stream";

    QString err;
    QByteArray prepJson = ProtocolJson::encodePrepareRequest(clientInfo, fm, &err);

    localsend::PosixTlsHttpClient client;
    ClientResponseWatcher prepRespWatcher;
    QObject::connect(&client, SIGNAL(finished(int,QByteArray)), &prepRespWatcher, SLOT(onFinished(int,QByteArray)));
    QObject::connect(&client, SIGNAL(failed(int,QString)), &prepRespWatcher, SLOT(onFailed(int,QString)));

    QMap<QString, QString> headers;
    headers.insert("Content-Type", "application/json");
    client.sendRequest("POST", QHostAddress::LocalHost, srvPort,
                        "/api/localsend/v2/prepare-upload", expectedPin, headers, prepJson, 5000);

    QElapsedTimer t;
    t.start();
    while (!prepRespWatcher.finished && !prepRespWatcher.failed && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(prepRespWatcher.finished && prepRespWatcher.statusCode == 200, "prepare-upload must succeed");

    PrepareUploadResponse prepResp;
    ASSERT_TRUE(ProtocolJson::decodePrepareResponse(prepRespWatcher.body, &prepResp, &err), "decode prepare response");
    QString sessionId = prepResp.sessionId;
    QString token = prepResp.fileTokens.value(fm.id);

    // Stream upload payload
    QBuffer buffer(&payload);
    ASSERT_TRUE(buffer.open(QIODevice::ReadOnly), "buffer open");

    localsend::PosixTlsHttpClient uploadClient;
    ClientResponseWatcher uploadRespWatcher;
    QObject::connect(&uploadClient, SIGNAL(finished(int,QByteArray)), &uploadRespWatcher, SLOT(onFinished(int,QByteArray)));
    QObject::connect(&uploadClient, SIGNAL(failed(int,QString)), &uploadRespWatcher, SLOT(onFailed(int,QString)));

    QString uploadPath = QString("/api/localsend/v2/upload?sessionId=%1&fileId=%2&token=%3")
                             .arg(sessionId).arg(fm.id).arg(token);

    QMap<QString, QString> uploadHeaders;
    uploadHeaders.insert("Content-Type", "application/octet-stream");
    bool streamOk = uploadClient.sendStreamRequest("POST", QHostAddress::LocalHost, srvPort,
                                                   uploadPath, expectedPin, uploadHeaders,
                                                   &buffer, payload.size(), 8000);
    ASSERT_TRUE(streamOk, "sendStreamRequest start");

    t.restart();
    while ((!uploadRespWatcher.finished || !offerWatcher.receiveFinishedEmitted) && t.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(uploadRespWatcher.finished, "upload must finish");
    ASSERT_TRUE(uploadRespWatcher.statusCode == 200, "upload response must be 200 OK");
    ASSERT_TRUE(offerWatcher.receiveFinishedEmitted, "receiveFinished signal must be emitted");
    ASSERT_TRUE(!offerWatcher.savedPath.isEmpty(), "savedPath must not be empty");

    QFile savedFile(offerWatcher.savedPath);
    ASSERT_TRUE(savedFile.open(QIODevice::ReadOnly), "open saved file");
    QByteArray savedContent = savedFile.readAll();
    savedFile.close();

    ASSERT_TRUE(savedContent.size() == payload.size(), "saved file size must match payload");
    QString savedSha = QCryptographicHash::hash(savedContent, QCryptographicHash::Sha256).toHex();
    ASSERT_TRUE(savedSha == payloadSha, "saved file SHA-256 must match payload SHA-256");

    app.stop();
    QDir(idDir).removeRecursively();
    QDir(rxDir).removeRecursively();
    TEST_PASS();
}

void testHttpServerTlsHandshakeTimeoutAndCapacityH4()
{
    TEST_CASE("H4 - HttpServer TLS handshake timeout and single-stalled slot recovery");
    QString idDir = "/tmp/localsend_h4_srv_tm_id";
    QDir(idDir).removeRecursively();

    localsend::LocalIdentity identity;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(idDir, &identity), "create identity");

    HttpServer server;
    server.setTlsIdentity(identity);
    server.setHandshakeTimeout(300); // 300 ms for fast test
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen");
    quint16 port = server.serverPort();

    // 1. Plain TCP connection that does not send TLS ClientHello must time out
    QTcpSocket stalledSocket;
    stalledSocket.connectToHost(QHostAddress::LocalHost, port);
    ASSERT_TRUE(stalledSocket.waitForConnected(1000), "tcp connected");

    // Server should have 1 handshaking socket
    QElapsedTimer t;
    t.start();
    while (server.handshakingSocketsCount() == 0 && t.elapsed() < 500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(1000);
    }
    ASSERT_TRUE(server.handshakingSocketsCount() == 1, "handshaking count must reach 1");

    // Wait past 300ms deadline
    t.restart();
    while (server.handshakingSocketsCount() > 0 && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }
    ASSERT_TRUE(server.handshakingSocketsCount() == 0, "handshake timeout must purge socket");

    // 2. Server recovers immediately and handles a valid TLS connection
    SimpleInfoResponder responder;
    QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)),
                     &responder, SLOT(onConnectionOpened(HttpConnection*)));

    localsend::PosixTlsHttpClient client;
    ClientResponseWatcher watcher;
    QObject::connect(&client, SIGNAL(finished(int,QByteArray)), &watcher, SLOT(onFinished(int,QByteArray)));
    QObject::connect(&client, SIGNAL(failed(int,QString)), &watcher, SLOT(onFailed(int,QString)));

    QMap<QString, QString> headers;
    client.sendRequest("GET", QHostAddress::LocalHost, port,
                      "/api/localsend/v2/info", identity.fingerprint(), headers, QByteArray(), 3000);

    t.restart();
    while (!watcher.finished && !watcher.failed && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(watcher.finished, "subsequent request must complete after timeout recovery");

    server.stop();
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testDiscoveryManagerHttpsBroadcastAndFingerprintH4()
{
    TEST_CASE("H4 - DiscoveryManager announcement encoding serializes protocol 'https' and certificate fingerprint");
    QString idDir = "/tmp/localsend_h4_dm_bcast_id";
    QDir(idDir).removeRecursively();

    localsend::Application app;
    app.setIdentityStorageDirectory(idDir);
    app.setPort(53317);
    QString startErr;
    ASSERT_TRUE(app.start(&startErr), "app start failed");

    DeviceInfo localInfo = app.localInfo();
    ASSERT_TRUE(localInfo.protocol == "https", "protocol must be https");
    ASSERT_TRUE(localInfo.fingerprint.length() == 64, "fingerprint must be 64-character hex");
    ASSERT_TRUE(localInfo.fingerprint == app.localIdentity().fingerprint(), "fingerprint matches certificate");

    QString encErr;
    QByteArray dgram = ProtocolJson::encodeAnnouncement(localInfo, true, &encErr);
    ASSERT_TRUE(!dgram.isEmpty(), "encode announcement");

    DeviceInfo decoded;
    bool announce = false;
    QString decErr;
    ASSERT_TRUE(ProtocolJson::decodeAnnouncement(dgram, &decoded, &announce, &decErr), "decode announcement");
    ASSERT_TRUE(decoded.protocol == "https", "decoded announcement protocol must be https");
    ASSERT_TRUE(decoded.fingerprint == localInfo.fingerprint, "decoded announcement fingerprint matches");
    ASSERT_TRUE(announce == true, "announce flag is true");

    app.stop();
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testUiControllerHttpsPeerSupportAndCertManagementH4()
{
    TEST_CASE("H4 - UiController certificate fingerprint exposure, HTTPS state, and peer send enablement");
    QString idDir = "/tmp/localsend_h4_uictrl_id";
    QDir(idDir).removeRecursively();

    localsend::Application app;
    app.setIdentityStorageDirectory(idDir);
    app.setPort(0);
    QString startErr;
    ASSERT_TRUE(app.start(&startErr), "app start failed");

    localsend::UiController controller(&app);
    ASSERT_TRUE(controller.isHttps(), "UiController isHttps must be true");
    ASSERT_TRUE(controller.serverProtocol() == "https", "UiController serverProtocol must be 'https'");
    ASSERT_TRUE(!controller.certificateFingerprint().isEmpty(), "UiController certificateFingerprint not empty");
    ASSERT_TRUE(controller.certificateFingerprint() == app.localIdentity().fingerprint(),
                "certificateFingerprint matches local identity");

    // Register a valid HTTPS peer on a controlled loopback endpoint
    QTcpServer dummyPeerServer;
    ASSERT_TRUE(dummyPeerServer.listen(QHostAddress::LocalHost, 0), "dummy peer server listen");
    quint16 dummyPort = dummyPeerServer.serverPort();

    DeviceInfo httpsPeer;
    httpsPeer.alias = "Secure Peer";
    httpsPeer.deviceModel = "Passport";
    httpsPeer.fingerprint = QString(64, 'd');
    httpsPeer.port = dummyPort;
    httpsPeer.protocol = "https";
    app.discoveryManager()->handleIncomingRegister(httpsPeer, QHostAddress::LocalHost);

    // Set a file to send
    QTemporaryFile tmpFile;
    ASSERT_TRUE(tmpFile.open(), "create tmp file");
    tmpFile.write("Hello HTTPS");
    tmpFile.flush();
    controller.setSelectedFilePath(tmpFile.fileName());
    ASSERT_TRUE(controller.hasSelectedFile(), "hasSelectedFile true");

    // sendSelectedFile to HTTPS peer must not return false with "unsupported in MVP"
    bool sendOk = controller.sendSelectedFile(httpsPeer.fingerprint);
    ASSERT_TRUE(sendOk, "sendSelectedFile to valid HTTPS peer must succeed");
    ASSERT_TRUE(controller.transferState() == "Preparing", "transfer state must transition to Preparing");

    controller.cancelTransfer();
    dummyPeerServer.close();
    app.stop();
    QDir(idDir).removeRecursively();
    TEST_PASS();
}

void testHttpServerPlaintextFallbackWhenConfiguredH4()
{
    TEST_CASE("H4 - HttpServer plaintext HTTP regression when configured with protocol 'http'");
    localsend::Application app;
    app.setPort(0);
    QString err;
    ASSERT_TRUE(app.start(&err), "app start");

    // Configure explicit HTTP mode
    ASSERT_TRUE(app.setProtocol("http", &err), "setProtocol http failed");
    ASSERT_TRUE(app.localInfo().protocol == "http", "protocol is http");
    ASSERT_TRUE(!app.httpServer()->isSecure(), "HttpServer secure mode is false");

    quint16 port = app.httpServer()->serverPort();
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, port);
    ASSERT_TRUE(client.waitForConnected(2000), "connectToHost");

    QByteArray rawReq = "GET /api/localsend/v2/info HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    client.write(rawReq);
    client.flush();

    QByteArray resp;
    QElapsedTimer t;
    t.start();
    while (client.isOpen() && t.elapsed() < 3000) {
        client.waitForReadyRead(100);
        QByteArray chunk = client.readAll();
        if (!chunk.isEmpty()) {
            resp.append(chunk);
        }
        if (resp.contains("\r\n\r\n") && !client.bytesAvailable()) {
            int clIdx = resp.indexOf("Content-Length:");
            if (clIdx != -1) {
                int bodyIdx = resp.indexOf("\r\n\r\n") + 4;
                int lineEnd = resp.indexOf("\r\n", clIdx);
                qint64 cl = resp.mid(clIdx + 15, lineEnd - (clIdx + 15)).trimmed().toLongLong();
                if (resp.size() - bodyIdx >= cl) {
                    break;
                }
            }
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    ASSERT_TRUE(resp.startsWith("HTTP/1.1 200 OK"), "plaintext server must return 200 OK");
    ASSERT_TRUE(resp.contains("\"protocol\":\"http\""), "plaintext /info response contains protocol http");

    app.stop();
    {
        QSettings s("LocalSend", "LocalSend-BB10");
        s.remove("network/protocol");
        s.sync();
    }
    TEST_PASS();
}

void testReceiveServerHttpsSourceIpBindingAndCrossSourceIsolationH4A()
{
    TEST_CASE("H4-A - Inbound HTTPS peer IP binding, cross-source upload 403, and idempotent cancel isolation");
    QString idDir = "/tmp/localsend_h4a_ip_bind_id";
    QString rxDir = "/tmp/localsend_h4a_ip_bind_rx";
    QDir(idDir).removeRecursively();
    QDir(rxDir).removeRecursively();

    localsend::Application app;
    app.setIdentityStorageDirectory(idDir);
    app.setReceiveDirectory(rxDir);
    app.setPort(0);
    QString startErr;
    ASSERT_TRUE(app.start(&startErr), "app start failed");

    quint16 srvPort = app.httpServer()->serverPort();
    QString expectedPin = app.localIdentity().fingerprint();

    ReceiveServerOfferWatcher offerWatcher(app.receiveServer(), true);
    QObject::connect(app.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &offerWatcher, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

    DeviceInfo clientInfo;
    clientInfo.alias = "Sender H4-A";
    clientInfo.fingerprint = QString(64, 'a');
    clientInfo.port = 53317;
    clientInfo.protocol = "https";

    QByteArray payload = "hello";
    QString payloadSha = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();

    FileMeta fm;
    fm.id = "file-h4a-1";
    fm.fileName = "hello.txt";
    fm.size = payload.size();
    fm.sha256 = payloadSha;
    fm.fileType = "text/plain";

    QString err;
    QByteArray prepJson = ProtocolJson::encodePrepareRequest(clientInfo, fm, &err);

    localsend::PosixTlsHttpClient client1;
    ClientResponseWatcher respWatcher1;
    QObject::connect(&client1, SIGNAL(finished(int,QByteArray)), &respWatcher1, SLOT(onFinished(int,QByteArray)));
    QObject::connect(&client1, SIGNAL(failed(int,QString)), &respWatcher1, SLOT(onFailed(int,QString)));

    QMap<QString, QString> headers;
    headers.insert("Content-Type", "application/json");
    client1.sendRequest("POST", QHostAddress::LocalHost, srvPort,
                        "/api/localsend/v2/prepare-upload", expectedPin, headers, prepJson, 5000);

    QElapsedTimer t;
    t.start();
    while (!respWatcher1.finished && !respWatcher1.failed && t.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(respWatcher1.finished && respWatcher1.statusCode == 200, "prepare-upload accept must return 200");
    ASSERT_TRUE(offerWatcher.offerReceived, "incomingOffer must be emitted");
    ASSERT_TRUE(!offerWatcher.peerAddress.isNull(), "observed source address must not be null");
    ASSERT_TRUE(offerWatcher.peerAddress == QHostAddress("127.0.0.1"), "observed source address must match 127.0.0.1");

    PrepareUploadResponse prepResp;
    ASSERT_TRUE(ProtocolJson::decodePrepareResponse(respWatcher1.body, &prepResp, &err), "decode prepare response");
    QString sessionId = prepResp.sessionId;
    QString token = prepResp.fileTokens.value(fm.id);

    ASSERT_TRUE(app.receiveServer()->hasActiveSession(), "active session must exist");

    // 1. Cross-source cancel with wrong source IP (127.0.0.2) must NOT cancel the session
    app.receiveServer()->handleCancel(sessionId, QHostAddress("127.0.0.2"));
    ASSERT_TRUE(app.receiveServer()->hasActiveSession(), "cross-source cancel must leave active session intact");
    ASSERT_TRUE(app.isTransferActive(), "app transfer slot must remain active after cross-source cancel");

    // 2. Cross-source upload with wrong source IP (127.0.0.2) must be rejected with 403 and NOT consume session
    HttpRequestHead fakeHead;
    fakeHead.method = "POST";
    fakeHead.target = QUrl(QString("/api/localsend/v2/upload?sessionId=%1&fileId=%2&token=%3")
                           .arg(sessionId).arg(fm.id).arg(token));
    fakeHead.peerAddress = QHostAddress("127.0.0.2");
    fakeHead.contentLength = payload.size();
    fakeHead.isSecure = true;

    int uploadStatus = 0;
    QString uploadErr;
    bool beginOk = app.receiveServer()->beginUpload(fakeHead, &uploadStatus, &uploadErr);
    ASSERT_FALSE(beginOk, "beginUpload from cross-source IP must fail");
    ASSERT_TRUE(uploadStatus == 403, "beginUpload status must be 403");
    ASSERT_TRUE(uploadErr.contains("Source IP mismatch"), "error must report source IP mismatch");
    ASSERT_TRUE(app.receiveServer()->hasActiveSession(), "cross-source upload must not consume session");

    // 3. Wrong-token upload must return 403 and leave session active
    HttpRequestHead wrongTokenHead = fakeHead;
    wrongTokenHead.target = QUrl(QString("/api/localsend/v2/upload?sessionId=%1&fileId=%2&token=wrongtoken")
                                 .arg(sessionId).arg(fm.id));
    wrongTokenHead.peerAddress = QHostAddress("127.0.0.1");
    beginOk = app.receiveServer()->beginUpload(wrongTokenHead, &uploadStatus, &uploadErr);
    ASSERT_FALSE(beginOk, "beginUpload with wrong token must fail");
    ASSERT_TRUE(uploadStatus == 403, "wrong token status must be 403");
    ASSERT_TRUE(app.receiveServer()->hasActiveSession(), "wrong token upload must not consume session");

    // 4. Legitimate upload from matching source IP (127.0.0.1) must succeed
    QBuffer buffer(&payload);
    ASSERT_TRUE(buffer.open(QIODevice::ReadOnly), "buffer open");

    localsend::PosixTlsHttpClient uploadClient;
    ClientResponseWatcher uploadRespWatcher;
    QObject::connect(&uploadClient, SIGNAL(finished(int,QByteArray)), &uploadRespWatcher, SLOT(onFinished(int,QByteArray)));
    QObject::connect(&uploadClient, SIGNAL(failed(int,QString)), &uploadRespWatcher, SLOT(onFailed(int,QString)));

    QString uploadPath = QString("/api/localsend/v2/upload?sessionId=%1&fileId=%2&token=%3")
                             .arg(sessionId).arg(fm.id).arg(token);

    QMap<QString, QString> uploadHeaders;
    uploadHeaders.insert("Content-Type", "application/octet-stream");
    bool streamOk = uploadClient.sendStreamRequest("POST", QHostAddress::LocalHost, srvPort,
                                                   uploadPath, expectedPin, uploadHeaders,
                                                   &buffer, payload.size(), 8000);
    ASSERT_TRUE(streamOk, "sendStreamRequest start");

    t.restart();
    while (!uploadRespWatcher.finished && !uploadRespWatcher.failed && t.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(2000);
    }

    ASSERT_TRUE(uploadRespWatcher.finished, "upload must finish");
    ASSERT_TRUE(uploadRespWatcher.statusCode == 200, "upload response must be 200 OK");
    ASSERT_FALSE(app.receiveServer()->hasActiveSession(), "session must be cleared upon successful finish");
    ASSERT_FALSE(app.isTransferActive(), "transfer slot must be released");

    app.stop();
    QDir(idDir).removeRecursively();
    QDir(rxDir).removeRecursively();
    TEST_PASS();
}

void testHttpServerTlsSetupFailureAndSwitchIsolationH4B()
{
    TEST_CASE("H4-B - TLS configuration failure propagation, startup listener suppression, and switch rollback");

    // 1. HttpServer::setTlsIdentity with invalid identity fails closed
    HttpServer server;
    QString err;
    localsend::LocalIdentity invalidIdentity;
    ASSERT_FALSE(server.setTlsIdentity(invalidIdentity, &err), "setTlsIdentity with invalid identity must fail");
    ASSERT_FALSE(server.isSecure(), "isSecure must be false on invalid identity");
    ASSERT_TRUE(err.contains("Invalid TLS identity"), "error message must report invalid identity");

    // 2. HttpServer::setSecure(true) without credentials context fails closed
    ASSERT_FALSE(server.setSecure(true, &err), "setSecure(true) without context must fail");
    ASSERT_FALSE(server.isSecure(), "isSecure must remain false");
    ASSERT_TRUE(err.contains("Cannot enable TLS"), "error message must report missing context");

    // 3. Application::start() with protocol 'https' and missing/invalid identity fails before listening
    QString tempDir = QDir::tempPath() + QString("/localsend_test_h4b_%1").arg(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(tempDir);
    QString iniPath = tempDir + "/settings.ini";

    QString badIdDir = tempDir + "/bad_id_file";
    QFile badFile(badIdDir);
    ASSERT_TRUE(badFile.open(QIODevice::WriteOnly), "create bad file");
    badFile.write("blocking directory");
    badFile.close();

    QSettings settings(iniPath, QSettings::IniFormat);
    settings.setValue("network/protocol", "https");
    settings.setValue("security/identityDir", badIdDir);
    settings.sync();

    localsend::Application app(0, &settings);
    app.setIdentityStorageDirectory(badIdDir);
    ASSERT_FALSE(app.localIdentity().isValid(), "identity must be invalid");
    app.setPort(0);

    QString startErr;
    bool startOk = app.start(&startErr);
    ASSERT_FALSE(startOk, "start() with https protocol and invalid identity must fail");
    ASSERT_FALSE(app.httpServer()->isListening(), "httpServer must NOT be listening on failed TLS startup");
    ASSERT_FALSE(app.httpServer()->isSecure(), "httpServer must not be marked secure");
    ASSERT_FALSE(startErr.isEmpty(), "startError must not be empty");

    // 4. Application starting with 'http' must succeed even when identity is missing
    settings.setValue("network/protocol", "http");
    settings.setValue("security/identityDir", badIdDir);
    settings.sync();
    localsend::Application httpApp(0, &settings);
    httpApp.setIdentityStorageDirectory(badIdDir);
    ASSERT_FALSE(httpApp.localIdentity().isValid(), "httpApp identity must be invalid");
    httpApp.setPort(0);
    startOk = httpApp.start(&startErr);
    ASSERT_TRUE(startOk, QString("start() with http protocol must succeed without valid identity: %1").arg(startErr).toLocal8Bit().constData());
    ASSERT_TRUE(httpApp.httpServer()->isListening(), "httpServer must be listening in http mode");
    ASSERT_FALSE(httpApp.httpServer()->isSecure(), "httpServer must not be secure");
    ASSERT_TRUE(httpApp.protocol() == "http", "protocol must be http");

    // 5. Switching from HTTP to HTTPS with invalid identity fails and preserves HTTP state
    QString switchErr;
    bool switchOk = httpApp.setProtocol("https", &switchErr, true);
    ASSERT_FALSE(switchOk, "setProtocol('https') with invalid identity must fail");
    ASSERT_FALSE(switchErr.isEmpty(), "switchErr must report reason");
    ASSERT_TRUE(httpApp.protocol() == "http", "protocol must remain http after failed switch");
    ASSERT_TRUE(settings.value("network/protocol").toString() == "http", "stored protocol must remain http");
    ASSERT_TRUE(httpApp.httpServer()->isListening(), "httpServer must remain listening in http mode");
    ASSERT_FALSE(httpApp.httpServer()->isSecure(), "httpServer must remain not secure");

    // 6. Generating valid identity allows switching to HTTPS and back
    QString validIdDir = tempDir + "/valid_identity";
    localsend::LocalIdentity validId;
    ASSERT_TRUE(localsend::LocalIdentity::loadOrCreate(validIdDir, &validId), "create valid identity");
    httpApp.setIdentityStorageDirectory(validIdDir);

    switchOk = httpApp.setProtocol("https", &switchErr, true);
    ASSERT_TRUE(switchOk, "setProtocol('https') with valid identity must succeed");
    ASSERT_TRUE(httpApp.protocol() == "https", "protocol must become https");
    ASSERT_TRUE(settings.value("network/protocol").toString() == "https", "stored protocol must become https");
    ASSERT_TRUE(httpApp.httpServer()->isSecure(), "httpServer must be marked secure");
    ASSERT_TRUE(httpApp.httpServer()->isListening(), "httpServer must be listening");

    // Switching back to HTTP succeeds cleanly
    switchOk = httpApp.setProtocol("http", &switchErr, true);
    ASSERT_TRUE(switchOk, "setProtocol('http') must succeed");
    ASSERT_TRUE(httpApp.protocol() == "http", "protocol must become http");
    ASSERT_TRUE(settings.value("network/protocol").toString() == "http", "stored protocol must become http");
    ASSERT_FALSE(httpApp.httpServer()->isSecure(), "httpServer must no longer be secure");

    // Switch back to HTTPS to test identity rotation failure isolation while running HTTPS
    switchOk = httpApp.setProtocol("https", &switchErr, true);
    ASSERT_TRUE(switchOk, "setProtocol('https') restore must succeed");

    // 7. Identity rotation failure preserves existing valid identity and does not publish unserved pin
    QString badRotateDir = tempDir + "/bad_rotate_dir";
    {
        QFile f(badRotateDir);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly), "create bad rotate file");
        f.write("not a directory");
        f.close();
    }
    QString origFingerprint = httpApp.localInfo().fingerprint;
    ASSERT_FALSE(origFingerprint.isEmpty(), "origFingerprint must not be empty");
    httpApp.setIdentityStorageDirectory(badRotateDir);
    ASSERT_TRUE(httpApp.localInfo().fingerprint == origFingerprint, "failed identity update must preserve existing fingerprint");
    ASSERT_TRUE(httpApp.localIdentity().fingerprint() == origFingerprint, "localIdentity must retain old fingerprint");
    ASSERT_TRUE(settings.value("security/identityDir").toString() == validIdDir, "settings identityDir must retain old valid path");
    ASSERT_TRUE(settings.value("device/fingerprint").toString() == origFingerprint, "settings fingerprint must retain old fingerprint");

    httpApp.stop();
    app.stop();
    QDir(tempDir).removeRecursively();
    TEST_PASS();
}

void testHttpConnectionRequestTimeoutAndSlotReclamationH4C()
{
    TEST_CASE("H4-C - Enforce request header and control body read deadlines and reclaim slots");

    HttpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0), "server listen failed");

    // 1. Idle request: connects but sends nothing
    QTcpSocket idleClient;
    idleClient.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketConnected(&idleClient), "idleClient connect failed");

    QTime t;
    t.start();
    while (server.activeConnectionsCount() < 1 && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(server.activeConnectionsCount() == 1, "server has 1 active connection");

    foreach (HttpConnection *conn, server.findChildren<HttpConnection*>()) {
        conn->setRequestTimeout(50);
    }

    t.start();
    while (server.activeConnectionsCount() > 0 && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(server.activeConnectionsCount() == 0, "idle connection slot must be reclaimed");
    idleClient.close();

    // 2. Partial header: sends incomplete request line/headers and stops
    QTcpSocket partialHeaderClient;
    partialHeaderClient.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketConnected(&partialHeaderClient), "partialHeaderClient connect failed");
    partialHeaderClient.write("GET /api/localsend/v2/info HTTP/1.1\r\nHost: local");
    partialHeaderClient.flush();

    t.start();
    while (server.activeConnectionsCount() < 1 && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(server.activeConnectionsCount() == 1, "server has 1 active connection for partial header");

    foreach (HttpConnection *conn, server.findChildren<HttpConnection*>()) {
        conn->setRequestTimeout(50);
    }

    t.start();
    while (server.activeConnectionsCount() > 0 && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(server.activeConnectionsCount() == 0, "partial header connection slot must be reclaimed");
    partialHeaderClient.close();

    // 3. Partial control body: sends full headers with Content-Length 100 but only sends partial body
    TestHttpReceiver receiver;
    QObject::connect(&server, SIGNAL(connectionOpened(HttpConnection*)),
                     &receiver, SLOT(onConnection(HttpConnection*)));

    QTcpSocket partialBodyClient;
    partialBodyClient.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketConnected(&partialBodyClient), "partialBodyClient connect failed");

    QByteArray headReq = "POST /api/localsend/v2/register HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: 100\r\n\r\n"
                         "{\"alias\":";
    partialBodyClient.write(headReq);
    partialBodyClient.flush();

    t.start();
    while ((!receiver.headersReceived || server.activeConnectionsCount() < 1) && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(receiver.headersReceived, "headers should be received");
    ASSERT_TRUE(receiver.lastConn != 0, "lastConn should not be null");

    receiver.lastConn->allowBody();
    receiver.lastConn->setRequestTimeout(50);

    t.start();
    while (server.activeConnectionsCount() > 0 && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(server.activeConnectionsCount() == 0, "partial body connection slot must be reclaimed");
    partialBodyClient.close();

    // 4. Fill 8 stalled connections, verify 9th is rejected, wait for timeout, verify 9th succeeds
    QList<QTcpSocket*> stalledClients;
    for (int i = 0; i < HttpServer::MAX_ACTIVE_CONNECTIONS; ++i) {
        QTcpSocket *c = new QTcpSocket();
        c->connectToHost(QHostAddress::LocalHost, server.serverPort());
        ASSERT_TRUE(waitSocketConnected(c), "stalled client connect failed");
        c->write("GET /api/localsend/v2/info HTTP/1.1\r\nHost: local");
        c->flush();
        stalledClients.append(c);
    }

    t.start();
    while (server.activeConnectionsCount() < HttpServer::MAX_ACTIVE_CONNECTIONS && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(server.activeConnectionsCount() == HttpServer::MAX_ACTIVE_CONNECTIONS, "all 8 connections filled");

    QTcpSocket client9;
    client9.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketReadyRead(&client9), "client9 must receive 503");
    QByteArray resp503 = client9.readAll();
    ASSERT_TRUE(resp503.contains("503 Service Unavailable"), "expected 503");
    client9.close();

    foreach (HttpConnection *conn, server.findChildren<HttpConnection*>()) {
        conn->setRequestTimeout(50);
    }

    t.start();
    while (server.activeConnectionsCount() > 0 && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(server.activeConnectionsCount() == 0, "all 8 stalled connections must be reclaimed");

    for (int i = 0; i < stalledClients.size(); ++i) {
        delete stalledClients.at(i);
    }

    // Reset receiver state
    receiver.headersReceived = false;
    receiver.requestFinishedReceived = false;
    receiver.lastConn = 0;

    QTcpSocket validClient;
    validClient.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitSocketConnected(&validClient), "valid client connect failed");
    validClient.write("GET /api/localsend/v2/info HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    validClient.flush();

    t.start();
    while (!receiver.requestFinishedReceived && t.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(receiver.requestFinishedReceived, "requestFinished should be received");
    ASSERT_TRUE(receiver.lastConn != 0, "lastConn valid");
    receiver.lastConn->respond(200, "application/json", "{\"status\":\"ok\"}");

    ASSERT_TRUE(waitSocketReadyRead(&validClient), "valid client response");
    QByteArray resp200 = validClient.readAll();
    ASSERT_TRUE(resp200.contains("200 OK"), "valid request must succeed with 200 OK");
    validClient.close();

    server.stop();
    TEST_PASS();
}

void testReceiveServerPendingCancelSlotReleaseH4D()
{
    TEST_CASE("H4-D - Pending local cancellation releases transfer slot and permits immediate next transfer");

    QString tempDir = QString("/tmp/localsend_test_h4d_%1").arg(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(tempDir);

    QSettings settings(tempDir + "/test.ini", QSettings::IniFormat);
    settings.setValue("network/protocol", "http");
    settings.setValue("receive/destinationDir", tempDir);

    localsend::Application app(0, &settings);
    app.setPort(0);

    QString startErr;
    ASSERT_TRUE(app.start(&startErr), "app start failed");

    DeviceInfo senderInfo;
    senderInfo.alias = "Sender";
    senderInfo.version = "2.2";
    senderInfo.deviceModel = "TestClient";
    senderInfo.deviceType = "desktop";
    senderInfo.fingerprint = "client-fingerprint";
    senderInfo.port = 53317;
    senderInfo.protocol = "http";

    FileMeta fm;
    fm.id = "f1";
    fm.fileName = "test.bin";
    fm.size = 5;
    fm.fileType = "application/octet-stream";

    QString encErr;
    QByteArray prepareBody = ProtocolJson::encodePrepareRequest(senderInfo, fm, &encErr);
    ASSERT_TRUE(!prepareBody.isEmpty(), "encode prepare request failed");

    // 1. First prepare request
    QTcpSocket client1;
    client1.connectToHost("127.0.0.1", app.httpServer()->serverPort());
    ASSERT_TRUE(waitSocketConnected(&client1), "client1 connect failed");

    QByteArray prepReq = "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\nContent-Type: application/json\r\n"
                         "Content-Length: " + QByteArray::number(prepareBody.size()) + "\r\n\r\n" + prepareBody;
    client1.write(prepReq);
    client1.flush();

    QTime t;
    t.start();
    while ((!app.receiveServer()->hasPendingOffer() || !app.isTransferActive()) && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(app.receiveServer()->hasPendingOffer(), "server must have pending offer");
    ASSERT_TRUE(app.isTransferActive(), "application transfer slot must be active");

    // 2. Local cancel before consent (using default cancelLocal() with empty ID)
    app.receiveServer()->cancelLocal();

    ASSERT_TRUE(waitSocketReadyRead(&client1), "client1 expected 403 on cancel");
    QByteArray resp1 = client1.readAll();
    ASSERT_TRUE(resp1.contains("403 Forbidden"), "cancelled offer returns 403");
    client1.close();

    // Verify both pending offer and application transfer slot are released
    ASSERT_FALSE(app.receiveServer()->hasPendingOffer(), "pending offer must be cleared");
    ASSERT_FALSE(app.isTransferActive(), "application transfer slot must be released after cancelLocal");

    // 3. Immediate second prepare request on new connection must succeed (no 409 Conflict)
    QTcpSocket client2;
    client2.connectToHost("127.0.0.1", app.httpServer()->serverPort());
    ASSERT_TRUE(waitSocketConnected(&client2), "client2 connect failed");
    client2.write(prepReq);
    client2.flush();

    t.start();
    while ((!app.receiveServer()->hasPendingOffer() || !app.isTransferActive()) && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    ASSERT_TRUE(app.receiveServer()->hasPendingOffer(), "second prepare must create pending offer");
    ASSERT_TRUE(app.isTransferActive(), "application slot must be active for second prepare");

    // 4. Cancel with explicit matching request ID
    QString offerId = app.receiveServer()->pendingRequestId();
    app.receiveServer()->cancelLocal(offerId);

    ASSERT_TRUE(waitSocketReadyRead(&client2), "client2 expected 403 on cancel-id");
    QByteArray resp2 = client2.readAll();
    ASSERT_TRUE(resp2.contains("403 Forbidden"), "cancelled offer returns 403");
    client2.close();

    ASSERT_FALSE(app.receiveServer()->hasPendingOffer(), "pending offer must be cleared after cancelLocal(id)");
    ASSERT_FALSE(app.isTransferActive(), "application transfer slot must be released after cancelLocal(id)");

    app.stop();
    QDir(tempDir).removeRecursively();
    TEST_PASS();
}

void testUiConfiguredHttpsPlainDisplayH4E()
{
    TEST_CASE("H4-E - UI configuration and peer badges display plain HTTPS without hardcoded TLS 1.3");

    QString peerCardPath = "assets/PeerCard.qml";
    if (!QFile::exists(peerCardPath)) {
        peerCardPath = "../assets/PeerCard.qml";
    }
    QString mainQmlPath = "assets/main.qml";
    if (!QFile::exists(mainQmlPath)) {
        mainQmlPath = "../assets/main.qml";
    }

    QFile peerCardFile(peerCardPath);
    ASSERT_TRUE(peerCardFile.open(QIODevice::ReadOnly | QIODevice::Text), "open PeerCard.qml");
    QString peerCardContent = QString::fromUtf8(peerCardFile.readAll());
    peerCardFile.close();

    ASSERT_TRUE(!peerCardContent.contains("HTTPS (TLS 1.3)"), "PeerCard.qml must not hardcode TLS 1.3");
    ASSERT_TRUE(peerCardContent.contains("\"HTTPS\""), "PeerCard.qml must display plain HTTPS");

    QFile mainQmlFile(mainQmlPath);
    ASSERT_TRUE(mainQmlFile.open(QIODevice::ReadOnly | QIODevice::Text), "open main.qml");
    QString mainQmlContent = QString::fromUtf8(mainQmlFile.readAll());
    mainQmlFile.close();

    ASSERT_TRUE(!mainQmlContent.contains("HTTPS (TLS 1.3)"), "main.qml must not hardcode TLS 1.3 in header");
    ASSERT_TRUE(!mainQmlContent.contains("TLS 1.3 Active"), "main.qml must not hardcode TLS 1.3 Active in settings");
    ASSERT_TRUE(mainQmlContent.contains("\"HTTPS • \""), "main.qml must display plain HTTPS header");
    ASSERT_TRUE(mainQmlContent.contains("\"Protocol: \" + (app.isHttps ? \"HTTPS\" : \"HTTP (Plaintext)\")"),
                "main.qml must display plain HTTPS protocol in settings");

    ASSERT_TRUE(mainQmlContent.contains("imageSource: \"asset:///icons/ic_send.png\""),
                "main.qml must set Send tab icon");
    ASSERT_TRUE(mainQmlContent.contains("imageSource: \"asset:///icons/ic_transfers.png\""),
                "main.qml must set Transfers tab icon");
    ASSERT_TRUE(mainQmlContent.contains("imageSource: \"asset:///icons/ic_settings.png\""),
                "main.qml must set Settings tab icon");
    ASSERT_TRUE(mainQmlContent.contains("SHA-256: Verified"),
                "main.qml must display SHA-256 verified badge");
    ASSERT_TRUE(mainQmlContent.contains("app.sha256Verified"),
                "main.qml must check sha256Verified");

    QString iconsDir = "assets/icons/";
    if (!QFile::exists(iconsDir + "ic_send.png")) {
        iconsDir = "../assets/icons/";
    }
    ASSERT_TRUE(QFile::exists(iconsDir + "ic_send.png"), "ic_send.png must exist");
    ASSERT_TRUE(QFile::exists(iconsDir + "ic_transfers.png"), "ic_transfers.png must exist");
    ASSERT_TRUE(QFile::exists(iconsDir + "ic_settings.png"), "ic_settings.png must exist");

    TEST_PASS();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    std::cout << "=== Running Protocol Core Checks (T01 & T02 & T03 & T04) ===" << std::endl;

    // T01 Protocol JSON tests
    testAnnouncementValid();
    testAnnouncementDeviceTypeFallback();
    testAnnouncementVersionIncompatible();
    testAnnouncementInvalidPort();
    testRegisterResponseFieldPreservation();
    testPrepareRequestValidSingleFile();
    testPrepareRequestLargeSize64Bit();
    testPrepareRequestRejectInvalid();
    testOversizedJsonRejected();
    testRoundTripEncodeDecode();

    // T02 HTTP Server & Stream Parser tests
    testHttpOneByteHeaderFragmentation();
    testHttpHeadersAndBodyTogether();
    testHttpContentLengthZero();
    testHttpChunkedTransfer();
    testHttpExpect100Continue();
    testHttpClTeConflictRejected();
    testHttpUnsupportedTransferEncodingRejected();
    testHttpOversizedHeaderRejected();
    testHttpConcurrentConnectionsNonBlocking();
    testHttpConnectionLimit();

    // T03 Application & Identity tests
    testAppIdentityAndSettingsPersistence();
    testAppTransferSlotMutex();
    testAppOccupiedPortStartupFailure();
    testAppLifecycleStartStop();

    // T04 Discovery & UDP multicast tests
    testDiscoverySelfIgnored();
    testDiscoveryRepeatedAnnouncementsOneRow();
    testDiscoveryMalformedAndOversizedUdpSafe();
    testDiscoveryManualRegisterHttp();

    // T04 (Sender Implementation) & End-to-End tests
    testSendNonExistentFileFails();
    testSendHttpsPeerRejected();
    testSendSuccessfulUpload();
    testSendPeerRejected403();
    testSendPeerBusy409();
    testSendPeerChecksumMismatch422();
    testSendPeerServerInternalError500();
    testSendLocalCancellation();
    testEndToEndBidirectionalTransfer();

    // R001 - R011 Review Issue Tests
    testHttpBoundedReadsAndCallbackGuards();
    testHttpSocketLargeBufferedDrain();
    testAppPortChangeRebindAndRollback();
    testAppUnwritableSettingsReportsError();
    testMockRegisterPythonCompatibility();
    testReceiveUnwritableDirectoryFailsCleanly();
    testTransferRecoveryAfterCancelAndFailWith10MiB();
    testReceiveCancelSingleTerminalSignalR007();
    testReceiveDestinationFlushErrorR020();
    testAppSingleThreadAssertionContract();
    testDiscoveryStopWithPendingRegistersSafe();
    testDiscoveryRegisterTimeout();
    testDiscoveryInterfaceSelectionAndSubnetPrefixR021();
    testDiscoveryUdpBufferAndPacedSweepR025();
    testDiscoveryHistoricalPeerPrioritizationR025();
    testDiscoveryPacedSweepHeadOfLineBlockingPreventionR025();
    testDiscoveryFallbackPacedEnqueueR025();
    testDiscoveryWholeScanAttemptCapR025();
    testDiscoveryActiveInterfaceResolutionR025();

    // T09 Cascades UI & Controller Tests
    testUiControllerDevicePropertiesAndAlias();
    testUiControllerFileSelection();
    testUiControllerPeersModel();
    testUiControllerSendFlowAndRejection();
    testUiControllerIncomingOfferAndAccept();
    testUiControllerCancelTransferRoutingR014();
    testUiControllerReceiveSyncAndDisconnectR015();
    testUiControllerTransferActiveNotificationR015();
    testUiControllerMultiFileCandidateSelectionR016();
    testUiControllerCollisionRenamingAndSavedPathR017();

    // H2 TLS Transport & LocalIdentity Tests
    testLocalIdentityPersistenceAndSecurityH2();
    testPosixTlsSocketLoopbackHandshakeAndTransferH2();
    testPosixTlsSocketPinMismatchRejectionH2();
    testPosixTlsSocketBackpressureAndExactWriteRetryH2();
    testPosixTlsSocketHandshakeDeadlineAndCloseOwnershipH2();
    testPosixTlsSocketInitAndSyncConnectH2A();
    testPosixTlsSocketWriteClosureAndFdReleaseH2B();
    testPosixTlsSocketFlushTimeoutAndShutdownH2C();
    testPosixTlsSocketWriteProgressDeadlineH2D();
    testLocalIdentityFifoRejectionH2E();

    // H3 Outgoing HTTPS with Frozen Peer Pin Tests
    testSendHttpsPeerMissingPinRejectedH3();
    testSendHttpsPeerInvalidPinFormatH3();
    testSendHttpsPinMismatchRejectionH3();
    testSendHttpsSuccessfulUploadAndAckH3();
    testSendHttpsActivePinStabilityH3();
    testSendHttpsLocalCancellationAndRemoteCancelH3();
    testSendHttpsFailureStatusResponsesH3();
    testPosixTlsHttpClientBoundedStreamingH3();
    testDiscoveryManagerOutgoingHttpsRegisterH3();

    // H4 Inbound HTTPS Server & Discovery/UI Wiring Tests
    testHttpServerInboundHttpsHandshakeAndInfoH4();
    testHttpServerInboundHttpsRegisterH4();
    testReceiveServerHttpsPrepareAndAsyncConsentH4();
    testReceiveServerHttpsStreamingUploadAndSha256H4();
    testHttpServerTlsHandshakeTimeoutAndCapacityH4();
    testDiscoveryManagerHttpsBroadcastAndFingerprintH4();
    testUiControllerHttpsPeerSupportAndCertManagementH4();
    testHttpServerPlaintextFallbackWhenConfiguredH4();
    testReceiveServerHttpsSourceIpBindingAndCrossSourceIsolationH4A();
    testHttpServerTlsSetupFailureAndSwitchIsolationH4B();
    testHttpConnectionRequestTimeoutAndSlotReclamationH4C();
    testReceiveServerPendingCancelSlotReleaseH4D();
    testUiConfiguredHttpsPlainDisplayH4E();

    std::cout << "================================================" << std::endl;
    std::cout << "Total: " << g_testCount << ", Passed: " << g_passCount << ", Failed: " << g_failCount << std::endl;

    return (g_failCount == 0) ? 0 : 1;
}

#include "core-check.moc"
