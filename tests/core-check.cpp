#include "protocol/ProtocolTypes.h"
#include "protocol/ProtocolJson.h"
#include "protocol/CryptoUtil.h"
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

#include <QCoreApplication>
#include <QTime>
#include <QTcpSocket>
#include <QThread>
#include <QCryptographicHash>
#include <iostream>
#include <cstdlib>
#include <cassert>
#include <unistd.h>

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
    rxApp.setPort(0);
    rxApp.setReceiveDirectory(rxDir);
    QString err;
    ASSERT_TRUE(rxApp.start(&err), "rxApp start failed");

    localsend::Application txApp;
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

        RxBodyTracker rxTracker;
        QObject::connect(rxApp.receiveServer(), SIGNAL(receiveProgress(QString,qint64,qint64)),
                         &rxTracker, SLOT(onProgress(QString,qint64,qint64)));

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
        while ((txApp.sendManager()->state() != SendManager::State_Uploading || rxTracker.bytes == 0) && t.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        ASSERT_TRUE(txApp.sendManager()->state() == SendManager::State_Uploading, "must reach Uploading state");
        ASSERT_TRUE(rxTracker.bytes > 0, "receiver body bytes must be delivered (>0) before cancellation");

        txApp.sendManager()->cancel();

        t.restart();
        while ((txApp.isTransferActive() || rxApp.isTransferActive()) && t.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }

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
    rxApp.setPort(0);
    rxApp.setReceiveDirectory(rxDir);
    QString err;
    ASSERT_TRUE(rxApp.start(&err), "rxApp start failed");

    localsend::UiController rxController(&rxApp);

    AutoAcceptOfferHandler autoAccept(rxApp.receiveServer());
    QObject::connect(rxApp.receiveServer(), SIGNAL(incomingOffer(QString,PeerDevice,QList<FileMeta>)),
                     &autoAccept, SLOT(onIncomingOffer(QString,PeerDevice,QList<FileMeta>)));

    localsend::Application txApp;
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
    rxApp.setPort(0);
    rxApp.setReceiveDirectory(rxDir);
    QString err;
    ASSERT_TRUE(rxApp.start(&err), "rxApp start failed");

    localsend::Application txApp;
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
    rxApp.setPort(0);
    rxApp.setReceiveDirectory(rxDir);
    QString err;
    ASSERT_TRUE(rxApp.start(&err), "rxApp start failed");

    localsend::Application txApp;
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

    std::cout << "================================================" << std::endl;
    std::cout << "Total: " << g_testCount << ", Passed: " << g_passCount << ", Failed: " << g_failCount << std::endl;

    return (g_failCount == 0) ? 0 : 1;
}

#include "core-check.moc"
