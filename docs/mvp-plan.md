# MVP Implementation Tasks for agy

English | [Simplified Chinese](mvp-plan.zh-CN.md)

Status: planning documents only; no implementation or runtime results. Requirements confirmed on 2026-10-08.

Confirmed requirements: BB10 10.3.x with Cascades is the final target; full UI pages may follow protocol validation. When a PC offers several files, the BB10 user may accept just one. **Bidirectional transfers are mandatory for the MVP: implement and verify both BB10 to PC and PC to BB10; neither direction may be deferred.** All tasks follow the [protocol contract](protocol-notes.md); tasks must not redefine wire formats.

Language convention: source code, identifiers, comments, docstrings, build-script comments, logs, and technical messages use English. Maintain a default English `.md` file and a `.zh-CN.md` counterpart for each document, updating both together. Code samples remain English in both versions. See [contributor instructions](../AGENTS.md).

## Architecture and Minimal Directory Layout

The proposed discovery/send/receive/protocol separation is sound. Add HTTP connection parsing, platform build entry points, and test entry points; package QML as assets. Since the MVP has one active single-file sender, SendManager may own all UploadTask responsibilities. This simplifies class organization without removing or deferring uploads. No generic scheduler or repository/service/interface layering is needed.

HttpServer owns TCP/HTTP transport; ReceiveServer owns LocalSend routes and receive state. Application assembles components, bridges QML, and owns configuration, device identity, and the shared active transfer slot. Keep business logic out of QML. DiscoveryManager holds Qt data; adapt it to the UI ArrayDataModel in the bridge without spreading Cascades dependencies into protocol logic.

Target layout for implementation; do not create unused source placeholders now:

```text
localsend-bb10/
  README.md
  README.zh-CN.md
  AGENTS.md
  AGENTS.zh-CN.md
  localsend-bb10.pro              # tracked qmake entry; SDK settings verified in T00
  bar-descriptor.xml             # root descriptor for BB10 tooling
  .gitignore
  docs/
    protocol-notes.md
    protocol-notes.zh-CN.md
    mvp-plan.md
    mvp-plan.zh-CN.md
    build-deploy.md
    build-deploy.zh-CN.md
  src/
    main.cpp
    app/Application.{h,cpp}
    discovery/DiscoveryManager.{h,cpp}
    send/SendManager.{h,cpp}
    receive/HttpServer.{h,cpp}
    receive/HttpConnection.{h,cpp} # independent HTTP parser state per socket
    receive/ReceiveServer.{h,cpp}
    protocol/ProtocolTypes.h
    protocol/ProtocolJson.{h,cpp}
  assets/
    icon.png
    main.qml                       # a single debug page is sufficient before T09
    DeviceListPane.qml             # split only when daily-use UI complexity requires it
    SendPane.qml
    ReceivePane.qml
  tests/
    core-check.cpp                 # one lightweight Qt check program, grouped by feature
    interoperability.py           # stdlib HTTP/socket checks and fixture generation
  build/                           # ignored generated Makefile, objects and BAR packages
```

Do not commit qmake-generated Makefiles. If the SDK needs a handwritten build wrapper, put it in scripts/ or the root and identify it as a handwritten entry point. Package an icon asset; AI generation is unnecessary. Load QML through asset:///main.qml without relying on development-machine source paths.

Following ponytail: keep identity/settings in Application instead of introducing LocalIdentity/SettingsManager. T07/T08 implement UploadTask's streaming, progress, final acknowledgement, and cancellation responsibilities in SendManager. Extract a class later if independent upload retries/concurrent scheduling require it; implement both transfer directions now. Defer full-subnet scanning, background services, history databases, multi-file scheduling, and UI frameworks. Preserve safe file writes, validation, asynchronous acceptance, and cancellation cleanup.

## Delivery Milestones and Dependencies

- **P: Protocol MVP**: T00-T08 plus protocol/device checks in T10 pass. A single debug page or developer test entry point is sufficient; selecting one fileId from an offer must be verifiable. Four full UI pages are not required.
- **U: Everyday MVP**: P plus T09 and the UI/manual checks in T10 pass. Users can select, send, and accept files directly on the phone. The final UI uses Cascades.
- Automatic acceptance in a test entry point is permitted only in explicitly enabled development builds, restricted to a test source IP and one fileId. Production builds must not silently receive files. Before the UI is ready, use explicit test actions without creating a separate production acceptance policy.

Recommended order: `T00 → T01 → T02 → T03 → T04 → T05 → T06 → T07 → T08 → T10(P) → T09 → T10(U)`. Without an SDK, pure protocol code and PC tests may proceed, but P cannot be declared complete. Deliver tasks individually; resolve constraint conflicts with reproducible evidence and the smallest proposed revision.

## Review Priorities

Review priorities confirmed on 2026-10-08: favor fast delivery of protocol MVP P. Give T03, T05, and T07 focused review as they are delivered, including their integration points:

- **T03**: startup/stop behavior, persistent identity/configuration, component ownership, and the shared transfer slot that prevents send/receive state conflicts.
- **T05**: asynchronous consent, selection of one offered file, session/token/source IP authorization, and cleanup after rejection, disconnection, or preparation failure.
- **T07**: streaming file upload, prepare/status/token handling, file/reply lifetime, and success only after the peer's final acknowledgement.

Use lightweight review for other tasks: inspect the relevant changes and existing verification evidence, concentrating on MVP blockers, protocol interoperability, data integrity, bounded resources, and recovery for the next transfer. Escalate review depth when a concrete failure or integration risk warrants it. Do not make style-only changes, speculative architecture, or UI polish prerequisites for protocol progress. Review priority does not change implementation dependencies or acceptance requirements: bidirectional 10 MiB transfers with matching SHA-256 hashes and SDK/device validation remain required for P.

## Shared Types and Function Conventions

These signatures are implementation contracts, not a request for unused abstraction classes. Default to C++03 compatibility and Qt4 SIGNAL/SLOT. Declare custom types with Q_DECLARE_METATYPE in ProtocolTypes.h and register them when crossing queued connections. A bool return means a local operation was accepted/started; signals report network completion.

```cpp
struct DeviceInfo {
    QString alias, version, deviceModel, deviceType, fingerprint;
    quint16 port;
    QString protocol; // "http" / "https"
    bool download;
};
struct PeerDevice {
    DeviceInfo info;
    QHostAddress address;          // transport observation, not JSON
    QDateTime lastSeen;
};
struct FileMeta {
    QString id, fileName, fileType, sha256; // empty sha256 serializes as null
    qint64 size;
};
struct PrepareUploadRequest {
    DeviceInfo info;
    QMap<QString, FileMeta> files; // include all candidates; accept <= 1
};
struct PrepareUploadResponse {
    QString sessionId;
    QMap<QString, QString> fileTokens;
};
struct HttpRequestHead {
    QByteArray method;
    QUrl target;
    QMap<QByteArray, QByteArray> headers; // normalized lowercase, reject ambiguity
    QHostAddress peerAddress;
    qint64 contentLength;          // -1 when absent
    bool chunked;
};
```

Serialize unused preview/metadata as null; ignore valid unknown optional fields on input. Do not add unused UI state to FileMeta. Initialize all scalar members. Validate JSON types before conversion rather than silently accepting QVariant::toString/toLongLong conversions. Use a dedicated register response decoder, not request validation requiring port/protocol.

Common failure signal: `failed(QString requestId, QString code, QString message)`. Use stable English local codes such as disk-error/cancelled/http-403 and English user-facing messages. Pending requests without a sessionId use a local requestId. Do not log tokens or complete query URLs. Report success only after the peer's final acknowledgement or successful local file commit.

### T00 — Toolchain, API, and Device Probe

**Input**: an obtainable BB10 10.3.x NDK, actual device/OS, development mode, and deployment credentials. Do not assume an SDK is installed. The final Cascades UI must work with the selected SDK.

**Output**: root qmake/descriptor/.gitignore, a minimal API probe, and actual commands/results in build-deploy.md. Probe QUdpSocket multicast, QTcpServer, QNetworkAccessManager, JsonDataAccess, file IO, incremental OpenSSL SHA-256, and RAND_bytes. Prefer SDK libcrypto; if linking fails, record evidence before introducing a minimal licensed SHA-256/system randomness alternative. Do not invent a random algorithm.

**Key signatures/entry points**:

```cpp
int runPlatformProbe(QString *report); // 0 means every required probe passed
// Uses JsonDataAccess::loadFromBuffer / saveToBuffer, not QJsonDocument.
```

**Acceptance cases**: ARMv7 compile/link; target logs QT_VERSION_STR; PC can connect to TCP; UDP multicast send/receive is visible; JSON round-trips 10485760 and 4294967296 without loss; SHA-256 standard vectors for empty input and abc match; generate two nonempty random tokens; create/read/delete a private file. Load a minimal packaged Cascades Page, or record the independent executable launch method when UI is deferred.

**Definition of done**: another developer can build, install, and run the probe on the specified BB10 device using recorded commands. Each required API has runtime evidence. A Linux Qt build or unsigned BAR alone is insufficient. Mark missing SDK/device validation pending, not passed.

### T01 — Types, JSON, and Protocol Fixtures

**Input**: pinned protocol specification, T00 JSON probe, and strict JSON fixtures.

**Output**: ProtocolTypes.h and ProtocolJson.h/.cpp, announcement/register/prepare serialization and validation, and passing/failing checks.

**Key signatures/entry points**:

```cpp
bool decodeAnnouncement(const QByteArray&, DeviceInfo*, bool *announce, QString *error);
bool decodeRegisterRequest(const QByteArray&, DeviceInfo*, QString *error);
bool decodeRegisterResponse(const QByteArray&, const PeerDevice &target,
                            PeerDevice*, QString *error);
bool decodePrepareRequest(const QByteArray&, PrepareUploadRequest*, QString *error);
bool decodePrepareResponse(const QByteArray&, PrepareUploadResponse*, QString *error);
QByteArray encodeAnnouncement(const DeviceInfo&, bool announce, QString *error);
QByteArray encodeRegisterRequest(const DeviceInfo&, QString *error);
QByteArray encodeDeviceResponse(const DeviceInfo&, QString *error);
QByteArray encodePrepareRequest(const DeviceInfo&, const FileMeta&, QString *error);
QByteArray encodePrepareResponse(const PrepareUploadResponse&, QString *error);
```

**Acceptance cases**: Chinese/space-containing names, nullable fields, missing download defaults to false, unknown device type may fall back to desktop, additional fields, 2.0/2.1/2.2, register responses without port/protocol; sizes 0/10 MiB/above 32 bits; reject negative/fractional/string sizes, invalid ports, missing id, mismatched id/key, files arrays, and oversized JSON. Parse output with independent Python stdlib JSON checks; implementation-only round-trips are insufficient.

**Definition of done**: all fixture checks pass, wire fields follow the specification, and the Qt4 build uses no Qt5 JSON/MIME/query API. Valid metadata above the product size limit can be decoded and rejected separately by policy.

### T02 — Bounded HTTP Server with Deferred Responses

**Input**: TCP byte streams, route body permission/final responses, and resource budgets.

**Output**: HttpServer/HttpConnection with independent parser state per socket, body chunk signals, and deferred responses. HttpServer has no session/token/fileId business knowledge.

**Key signatures/entry points**:

```cpp
bool HttpServer::listen(const QHostAddress&, quint16 port, QString *error);
void HttpServer::stop();
// HttpConnection owns its socket; emitted chunks are consumed synchronously.
void HttpConnection::allowBody(); // send 100 only if requested and accepted
void HttpConnection::respond(int status, const QByteArray &contentType,
                             const QByteArray &body);
void HttpConnection::abort();
signals: void connectionOpened(HttpConnection *connection); // server
signals: void headersReady(const HttpRequestHead&);          // connection
signals: void bodyChunk(const QByteArray&);
signals: void requestFinished();
signals: void disconnected();
```

**Acceptance cases**: one-byte header fragmentation; headers/body together; body arrival before allowBody; Content-Length 0; arbitrarily split chunked bodies, valid trailers, malformed chunks; Expect 100 wait; conflicting/duplicate lengths and CL+TE rejection; oversized headers/JSON; client disconnect; another register completes while a response is pending. Bound socket reads and accumulated JSON; never readAll a complete file.

**Definition of done**: stdlib raw-socket cases pass; final responses contain correct Content-Length, CRLF, and Connection: close. Pending prepare does not freeze UI/other connections. Parser memory does not grow with file size; stop cleans all sockets. Standard clients can reconnect without keep-alive.

### T03 — Identity, Configuration, and Application Assembly

**Input**: alias, persisted fingerprint, receive directory, TCP/UDP settings, and lifecycle events.

**Output**: Application owns a shared QNetworkAccessManager, HTTP server, discovery/send/receive components, and one active transfer slot. Announce only after startup succeeds.

**Key signatures/entry points**:

```cpp
bool Application::start(QString *error);
void Application::stop();
DeviceInfo Application::localInfo() const;
bool Application::beginTransfer(const QString &requestId); // shared single slot
void Application::endTransfer(const QString &requestId);   // matching owner only
```

**Acceptance cases**: restart preserves fingerprint; installations have distinct identities; alias persists; occupied port 53317 reports startup failure without advertising reception; changed ports appear correctly in announcements; private destination creation failure reports an error; stop/start does not duplicate signal connections or leave sockets.

**Definition of done**: identity/settings are reproducible; only the matching owner can release the slot; register remains responsive during transfers; sending and receiving cannot overwrite each other's state. Avoid conflicts with bb::cascades::Application through a namespace or ApplicationController name.

### T04 — UDP and Bidirectional Register Discovery

**Input**: localInfo, Wi-Fi IPv4 interface, UDP sender information, and register requests/responses.

**Output**: peer list, last discovery time, protocol capabilities, refresh, and manual target entry.

**Key signatures/entry points**:

```cpp
bool DiscoveryManager::start(const DeviceInfo&, const QNetworkInterface&, QString *error);
void DiscoveryManager::stop();
void DiscoveryManager::refresh();
void DiscoveryManager::registerPeer(const QHostAddress&, quint16 port); // manual HTTP
void DiscoveryManager::onRegistered(const DeviceInfo&, const QHostAddress&);
signals: void peerUpdated(const PeerDevice&);
signals: void discoveryError(const QString&);
```

**Acceptance cases**: both startup orders; repeated UDP/register for a fingerprint produce one row; UDP fallback after HTTP response failure; announce false causes no response; no self-discovery; malformed/oversized UDP cannot crash; IP updates; nondefault peer TCP ports; manual PC IP recovery after group-join failure; HTTPS peers show sending unavailable with no plaintext sent to their TLS port.

**Definition of done**: on a multicast-enabled LAN, the official PC client sees BB within 10 s after BB launch/manual refresh, and BB discovers PC. Packet evidence shows no response loops and correct source IP/advertised TCP port usage. Manual register works on restricted LANs. The 10 s target applies to the fixed test environment, not the protocol generally.

### T05 — Incoming Offers, Asynchronous Consent, and Single-File Authorization

**Input**: prepare JSON, TCP source IP, fileId selected by UI/test entry, destination directory, and active transfer slot.

**Output**: offered metadata, asynchronous accept/reject result, one session and file token, and prepared temporary file. No whole-file buffering.

**Key signatures/entry points**:

```cpp
void ReceiveServer::attach(HttpServer *server);
void ReceiveServer::accept(const QString &requestId, const QString &fileId);
void ReceiveServer::reject(const QString &requestId);
signals: void incomingOffer(const QString &requestId, const PeerDevice&,
                            const QList<FileMeta>&);
```

**Acceptance cases**: single-file acceptance returns 200/token; rejection 403; second concurrent prepare 409; selecting the second of several offers returns only its token; uploading an unselected fileId returns 403; reject traversal/directories/oversized files; temporary-file creation failure 500; discovery continues after 30 s awaiting consent; pending disconnect leaves no prompt/lock; 120 s decision timeout releases the slot using project rejection policy.

**Definition of done**: the official PC client waits for acceptance, reports rejection when BB declines, and uploads only the selected file. Tokens use secure randomness; local requestId and wire sessionId have distinct roles. Every terminal path releases resources. T09 may provide the full list UI later; explicitly choosing a fileId through a test entry must already verify the protocol path.

### T06 — Streaming Reception, Checksum, and File Commit

**Input**: upload query, source IP, accepted metadata, and decoded body chunks.

**Output**: saved file, receive progress, and final HTTP 200/error; no final/current temporary file on failure.

**Key signatures/entry points**:

```cpp
bool ReceiveServer::beginUpload(const HttpRequestHead&, int *httpStatus, QString *error);
bool ReceiveServer::appendUpload(const QByteArray&, int *httpStatus, QString *error);
bool ReceiveServer::finishUpload(QString *savedPath, int *httpStatus, QString *error);
signals: void receiveProgress(const QString &sessionId, qint64 done, qint64 total);
signals: void receiveFinished(const QString &sessionId, const QString &savedPath);
```

**Acceptance cases**: PC to BB 10 MiB arbitrary binary with matching SHA-256; 0 B; Chinese/space-containing names; two preserved copies for name collisions; chunked without Content-Length; correct/incorrect supplied checksum yields 200/422; missing parameters 400, wrong token/IP 403, duplicate upload rejection; too long/short body cannot succeed; disk-full/permission/rename failures clean up; reconnect after interrupted transfer.

**Definition of done**: both endpoints acknowledge a 10 MiB transfer and hashes match; acknowledgement follows flush/close/rename. Target less than 8 MiB difference in peak RSS growth between 10 MiB and 100 MiB transfers and record measurement methods. This bounds file-size-dependent growth, not Cascades total RSS. If unavailable, document bounded buffers/runtime logs and keep performance verification pending.

### T07 — Single-File Sending and Final Acknowledgement

**Input**: peer address/port/protocol, readable ordinary file path, and localInfo.

**Output**: prepare, waiting for acceptance, streaming upload, final peer acknowledgement, local progress, and failure reason.

**Key signatures/entry points**:

```cpp
bool SendManager::sendFile(const PeerDevice&, const QString &path, QString *error);
void SendManager::cancel();
signals: void sendStateChanged(const QString &requestId, const QString &state);
signals: void sendProgress(const QString &requestId, qint64 done, qint64 total);
signals: void sendFinished(const QString &requestId);
```

**Implementation contract**: keep QFile open/alive throughout the request and use QNAM::post(request, file). Set correct Content-Length/Content-Type, send a basename, use application/octet-stream, and encode query parameters with Qt4 QUrl. On reply finished, inspect HTTP status before interpreting network errors. Upload progress reaching 100% means bytes were sent, not success; wait for final confirmation. Abort on source changes/read failures rather than reporting success.

**Acceptance cases**: BB to official PC 10 MiB with matching hash; 0 B/Chinese name; rejection 403, busy 409, 401, 429, 500; prepare 204 skips upload; malformed 200 JSON/missing selected file token skips upload; upload 422 reports failure; peer 500 after 100% cannot show success; source truncation during transfer; reject HTTPS peer before requesting it.

**Definition of done**: a BB device sends 10 MiB successfully to PC with identical content. No multipart/base64 or whole-file readAll. QFile/reply cleanup and cancellation are safe, progress updates continuously, and UI/control actions remain responsive.

### T08 — Cancellation, Timeouts, Network Loss, and Final States

**Input**: local cancellation, peer cancel, socket disconnect, timers, application exit, and network changes.

**Output**: one terminal state; clean up current file/socket/reply/slot so new transfers can start. Preserve initiator/receiver reasons without overriding confirmed success.

**Key signatures/entry points**:

```cpp
void ReceiveServer::cancelLocal(const QString &requestId);
void ReceiveServer::handleCancel(const QString &sessionId, const QHostAddress &peer);
void SendManager::handleRemoteCancel(const QString &sessionId, const QHostAddress &peer);
void SendManager::onNetworkLost();
void ReceiveServer::onNetworkLost();
```

**Implementation contract**: sender cancellation aborts its reply and sends cancel to the receiver if sessionId exists. Receiver cancellation cleans up first, then sends best-effort cancel to the known HTTP sender's advertised port. Route callbacks to send state only when sessionId and peer match. Unrelated cancel requests cannot kill active transfers. Abort the pending socket while waiting for prepare. Finalization is idempotent; unknown completed sessions may receive no-op 200 without a history database, or use a short bounded record.

**Acceptance cases**: cancel while awaiting consent/upload and during transfer; cancellation from both endpoints; repeated cancel; wrong IP/session cannot affect a transfer; notification failure still cleans locally; release after 30 s without progress while slow continuous transfers may exceed 30 s; Wi-Fi loss/switch, exit/restart; cancellation racing with final bytes emits one terminal result.

**Definition of done**: stop local IO/UI updates within 2 s, with a separate best-effort notification timeout of 3 s. No partial final files remain; another 10 MiB transfer succeeds after cancellation/network recovery. Every wait has a deadline and each operation emits exactly one completion/failure signal.

### T09 — Everyday Cascades UI (May Follow Protocol Validation)

**Input**: peers, offered metadata, state/progress, native FilePicker, and destination permissions.

**Output**: native peers/refresh/manual IP, single-file selection/send, offered-file single selection/accept/reject, cancel, saved location, and error messages. Four QML pages are a layout reference; one main Page plus Sheet may suffice.

**Key signatures/entry points**:

```cpp
Q_INVOKABLE void refreshDevices();
Q_INVOKABLE void addManualPeer(const QString &ipv4, int port);
Q_INVOKABLE void chooseFile();
Q_INVOKABLE void sendSelectedFile(const QString &fingerprint);
Q_INVOKABLE void acceptIncoming(const QString &requestId, const QString &fileId);
Q_INVOKABLE void rejectIncoming(const QString &requestId);
Q_INVOKABLE void cancelTransfer();
// Q_PROPERTY exposes device/candidate models, transfer state/progress, savedPath.
```

**Acceptance cases**: bidirectional 10 MiB through touch actions only; picker cancellation sends no request; at most one candidate selected; disable sending without peer/file; explain unsupported HTTPS peers; private fallback after shared permission denial displays actual location; verify at least one target screen such as Q10/Passport; refresh/cancel remain responsive during transfer; background behavior is explained within the foreground-only scope.

**Definition of done**: native Cascades without Qt5 QtQuick.Controls; everyday transfers work without test entry points/manual path edits; errors are understandable and saved files are locatable. If deferred, leave T09 incomplete and deliver P only, not U.

**Implementation Status (Completed)**:
- C++ ViewModel/Controller implemented in `src/ui/UiController.h` and `src/ui/UiController.cpp`. Exposes all required `Q_INVOKABLE` methods and `Q_PROPERTY` attributes.
- Native Cascades QML UI implemented in `assets/main.qml` tailored for BlackBerry Passport 1:1 square display (1440x1440) using `TabbedPane` (Send & Radar, Transfers & Incoming Prompt, Settings & Storage).
- Dual-mode `src/main.cpp` entry point: initializes `bb::cascades::Application` with `assets/main.qml` on BB10/QNX, and provides CLI/daemon fallback on desktop/test environments.
- 5 unit/integration test suites added in `tests/core-check.cpp` (device properties, file selection validation & formatting, peer list model exposure, send flow rejection, incoming offer handling & accept/reject). Total test count increased to 52 (52/52 PASS).

### T10 — Interoperability Regression, Evidence, and agy Delivery

**Input**: BB10 BAR/executable, recorded official PC release, LAN, test scripts, fixture files, and hashes.

**Output**: runnable tests/interoperability.py, JSON/HTTP checks, reproducible build/deployment commands, actual acceptance results, artifact SHA-256, and minimal packet/log evidence. Script end-to-end checks alone do not establish official PC interoperability.

**Key signatures/entry points**:

```text
python3 tests/interoperability.py fixtures --output /tmp/localsend-bb10-fixtures
python3 tests/interoperability.py check-http --host <bb-ip> --port 53317
python3 tests/interoperability.py serve-peer --bind <pc-lan-ip> --port <test-port>
```

check-http and serve-peer only access developer-specified endpoints, without network scanning. Production clients do not depend on Python. Generate deterministic 0 B/1 KiB/10 MiB/100 MiB files, Chinese names, and independent SHA-256 manifests without committing large binaries.

**Acceptance cases**: execute the build-deploy.md matrix, including token/source IP, chunked, Expect, partial selection, checksum mismatch, official bidirectional 10 MiB transfers, and retry/cleanup paths. Record OS/SDK/PC versions, disabled encryption, network, and deployment method.

**Definition of done**: all applicable P items have PASS and evidence; U also passes UI items. Mark unexecuted checks NOT RUN and failures FAIL with a minimal reproduction. Predicted behavior is not evidence. Without SDK/device access, deliver source and host checks while explicitly identifying missing P acceptance.

## Execution Contract to Hand to agy

> Implement protocol MVP P in localsend-bb10 following docs/mvp-plan.md. Read README, protocol-notes, build-deploy, and AGENTS first. Deliver T00-T08 and T10 individually. Bidirectional 10 MiB transfers from BB10 to PC and PC to BB10 are mandatory and may not be deferred. If full UI delays protocol validation, keep only required test actions and defer T09, while retaining selection of one offered file. Use BB10 Qt4.8/C++03 and platform JSON; do not use QJsonDocument/QUrlQuery/QMimeDatabase/QSaveFile/QtQuick.Controls 2. Preserve raw binary HTTP uploads, bounded memory, asynchronous consent, session/token/source IP checks, temporary-file commit, and v2.2 checksum mismatch 422. Do not add HTTPS, multi-file scheduling, background services, Download API, or third-party UI frameworks. Keep code, comments, and technical text in English; maintain full English and Simplified Chinese documentation pairs together. Record changed files, commands, results, and unverified items for every task. Do not claim P completion without SDK/device validation. If T00 finds incompatible tooling/libraries, provide evidence and the smallest viable alternative. Do not commit generated Makefiles, credentials, signing keys, or large fixtures.

## Skills and Open Implementation Details

Read [ponytail](/home/ellery/.config/antigravity/skills/ponytail/SKILL.md): prefer native APIs, keep single-file UploadTask responsibilities in SendManager, avoid speculative frameworks, and preserve full bidirectional transfers, safe data handling, and validation.

Read [grill-me](/home/ellery/.config/antigravity/skills/grill-me/SKILL.md). Its only body instruction is `Call the Skill tool with "grilling".` This session lacks that tool, and no local grilling instructions were found, so its specified workflow was not completed. Two actual clarification questions established deferrable UI and acceptance of one offered file; these are not a claim that grilling ran.

Implementation details still to confirm: available SDK/deployment tools and actual device/OS; PC release; whether shared storage should be the default; suitability of the initial 100 MiB limit and timeouts. These do not block this plan. Verify/adjust them in T00/T09 and synchronize both language versions and acceptance cases.
