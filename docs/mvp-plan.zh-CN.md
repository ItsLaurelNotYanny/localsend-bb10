# 可交给 agy 的 MVP 实现任务

[English](mvp-plan.md) | 简体中文

状态：只有规划文档，没有实现或实测结果。需求确认于 2026-10-08。

用户确认：最终目标 BB10 10.3.x + Cascades；协议调通阶段可放宽 UI，完整页面后置。PC 一次发送多个候选时，BB10 用户可以只接受其中一个。**双向收发属于 MVP 的硬性要求：BB10→PC 与 PC→BB10 均须实现并实测，不能后置。** 以下任务沿用 [协议合同](protocol-notes.zh-CN.md)，不得由各任务重新定义线上格式。

语言约定：代码、标识符、注释、文档字符串、构建脚本注释、日志和技术消息全部使用英文。文档采用英文默认 `.md` 与中文 `.zh-CN.md` 两份，修改时同步；两种文档中的代码示例也保持英文。详见 [贡献者指引](../AGENTS.zh-CN.md)。

## 架构判断与精简后的目录

原骨架的 discovery/send/receive/protocol 分工合理。需要补 HTTP 连接解析、平台构建入口、测试入口；QML 应明确作为打包 assets。MVP 单文件只有一个活动发送，可以把 UploadTask 的全部上传职责合入 SendManager；这是类组织精简，不是删除或后置上传功能。不需要泛型调度器、repository/service/interface 分层。

HttpServer 负责 TCP/HTTP，ReceiveServer 负责 LocalSend 路由和接收状态。Application 是装配及 QML bridge，持有配置、设备身份与单活动传输槽位；业务不得写进 QML。DiscoveryManager 保留 Qt 层数据，UI 的 ArrayDataModel 在 bridge 中适配，不将 Cascades 依赖扩散到协议逻辑。

下面是实现阶段应形成的目标树，当前不创建空源码文件：

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

不提交 qmake 生成的 Makefile。如果 SDK 工程确实需要手写的构建 wrapper，则放 scripts/ 或根目录并注明“手写入口”；它与生成 Makefile 不同。图标是打包 assets，不需要 AI 生成。QML 用 asset:///main.qml 加载，禁止依赖开发机源码路径。

按 ponytail 精简：身份及设置先放 Application，不拆 LocalIdentity/SettingsManager；UploadTask 的流式上传、进度、最终确认和取消职责全部由 T07/T08 的 SendManager 实现。未来需要独立上传重试/并发调度时再拆类，双向收发能力现在就实现。不做全子网扫描、后台服务、历史数据库、多文件调度和 UI 框架。文件写入、输入验证、异步确认、取消清理不能省。

## 两个交付点及依赖顺序

- **P：协议 MVP**：T00–T08 + T10 的协议/真机项目通过。允许单页调试面板或开发者测试入口；从元数据列表指定一个 fileId 的功能必须可验证。不要求四个完整页面。
- **U：日常使用 MVP**：P + T09 + T10 的界面/手动操作项目通过，可直接在手机上选择、发送和确认接收文件。最终 UI 为 Cascades。
- 测试入口自动接受只能出现在明确启用的开发构建中，限定测试源 IP 与一个 fileId；发布构建不得无声自动收文件。能触发 UI 前，就用显式测试操作调通路径，不引入另一套生产接收策略。

推荐顺序：`T00 → T01 → T02 → T03 → T04 → T05 → T06 → T07 → T08 → T10(P) → T09 → T10(U)`。T00 无 SDK 时，其余纯协议代码和 PC 测试可推进，但 P 不得宣布完成。实现者逐项交付，遇到约束冲突先给出可复现证据及最小修订方案。

## 评审优先级

评审优先级确认于 2026-10-08：以快速交付协议 MVP P 为目标。T03、T05、T07 交付时重点评审，并检查其集成接口：

- **T03**：启动/停止行为、身份与配置持久化、组件所有权，以及防止收发状态冲突的共享传输槽位。
- **T05**：异步确认、多候选选一、session/token/source IP 授权，以及拒绝、断连或准备失败后的清理。
- **T07**：文件流式上传、prepare/status/token 处理、文件与 reply 生命周期，以及收到对端最终确认后才报告成功。

其他任务采用轻量评审：检查相关变更及已有验证证据，聚焦 MVP 阻塞问题、协议互通、数据完整性、有界资源，以及下一次传输能否恢复。发现具体故障或集成风险时再加深评审。纯风格修改、推测性的架构设计和 UI 打磨不作为协议推进的前置条件。评审优先级不改变实现依赖或验收要求：P 仍须完成双向 10 MiB 传输且 SHA-256 一致，并通过 SDK/真机验证。

## 公共类型与函数约定

以下是实现合同，不要求生成未使用的抽象类。默认 C++03 兼容，使用 Qt4 SIGNAL/SLOT；QObject 相关类型在 ProtocolTypes.h 中 Q_DECLARE_METATYPE，并在跨 queued connection 时注册。签名返回 bool 只表示本地操作已接受/启动，网络完成用 signal。

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

可选 preview/metadata 对发送序列化为 null，接收忽略合法的未知可选字段；FileMeta 不为未使用的 UI 信息增加状态。构造时初始化所有标量。JSON 解析先检查类型再转换，不用 QVariant::toString/toLongLong 默默吞掉错误。register response 用响应专用解析函数，不套请求的必需 port/protocol 检查。

signals 的共同失败合同：`failed(QString requestId, QString code, QString message)`，code 为稳定本地枚举字符串（如 disk-error/cancelled/http-403），message 面向用户并使用英文；没有 sessionId 的 pending 请求使用本地 requestId。日志不输出 token 或带完整 query 的 URL。成功只在对端最终确认或接收文件成功提交后发出。

### T00 — 工具链、API 和真机最小验证

**输入**：可取得的 BB10 10.3.x NDK、具体设备/OS、开发模式及部署凭据；不能假定本机已安装 SDK。最终 Cascades 要能在该 SDK 上运行。

**输出**：根 qmake/descriptor/.gitignore；一个最小 API probe；build-deploy.md 的实际命令与结果。首次检查 QUdpSocket 组播、QTcpServer、QNetworkAccessManager、JsonDataAccess、文件读写、OpenSSL 增量 SHA-256 与 RAND_bytes。优先现有 SDK libcrypto；若不可链接，记录证据再引入最小带许可证的 SHA-256/系统随机源替代，禁止自己设计随机算法。

**关键签名**：

```cpp
int runPlatformProbe(QString *report); // 0 means every required probe passed
// Uses JsonDataAccess::loadFromBuffer / saveToBuffer, not QJsonDocument.
```

**验收用例**：armv7 编译/链接；目标启动日志包含 QT_VERSION_STR；TCP 可被 PC 连接；UDP 组播发/收可见；JSON 的 10485760 和 4294967296 能无损 round-trip；空数据和 abc 的 SHA-256 标准向量正确；生成两份非空随机 token；私有目录创建/读回/删除文件。Cascades 能加载打包内的一张最小 Page；如果 UI 暂缓，记录独立 executable 的实际运行方法。

**完成标准**：另一开发者能按记录的命令构建、安装并在指定 BB10 真机跑 probe；每个必要 API 有实测结论。只有 Linux Qt 编译或未签名 BAR 不算完成。SDK/真机不可用时标记“待环境验证”，不得打勾。

### T01 — 类型、JSON 与协议 fixtures

**输入**：protocol-notes 的固定规范、T00 JSON probe、严格 JSON fixtures。

**输出**：ProtocolTypes.h、ProtocolJson.h/.cpp；公告/注册/prepare 请求响应序列化与校验；一组通过/失败检查。

**关键签名**：

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

**验收用例**：中文/空格名、nullable 字段、缺 download 默认为 false、未知设备类型可回退 desktop、额外字段、2.0/2.1/2.2、无 port/protocol 的 register 响应；size 为 0/10 MiB/超过 32 位；拒绝负数、小数、字符串 size、非法端口、缺 id、id/key 不同、files 数组及超限 JSON。可用独立 stdlib Python JSON 解析输出，不能只做同一实现的自我 round-trip。

**完成标准**：fixtures 检查全过，线上字段与规范一致，Qt4 构建无 Qt5 JSON/MIME/URL query API；大于产品文件上限但合法的元数据可解析，再由策略拒绝。

### T02 — 有界、可延迟响应的 HTTP 服务端

**输入**：TCP socket 字节流、路由返回的 body 许可/最终响应、资源预算。

**输出**：HttpServer/HttpConnection；每 socket 独立解析状态、body 分块信号、延迟响应能力。HttpServer 不知道 session/token/fileId 业务。

**关键签名**：

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

**验收用例**：header 按 1 B 分片；header+body 同包；body 在 allowBody 前已经到达；Content-Length 0；chunked 任意切块/合法 trailer/错误 chunk；Expect 100 等待；冲突/重复长度与 CL+TE 被拒；超长 header/JSON；client 中途关闭；等待响应时另一个 socket 能完成 register。读 socket 和累计 JSON 均有上限，不使用文件级 readAll。

**完成标准**：tests 中 stdlib raw-socket cases 全过；有最终 Content-Length、CRLF 和 Connection: close；prepare 可以挂起而 UI/其他连接不冻结；大文件 parser 内存不随文件大小增加；stop 清理全部 socket。未实现 keep-alive 不影响标准 client 重连。

### T03 — 身份、配置与应用装配

**输入**：alias、已保存的 fingerprint、收件目录、TCP/UDP 配置、启动/退出事件。

**输出**：Application 管理共享 QNetworkAccessManager、HTTP server、discovery/send/receive；一次活动传输槽位；启动成功后才发公告。

**关键签名**：

```cpp
bool Application::start(QString *error);
void Application::stop();
DeviceInfo Application::localInfo() const;
bool Application::beginTransfer(const QString &requestId); // shared single slot
void Application::endTransfer(const QString &requestId);   // matching owner only
```

**验收用例**：重启 fingerprint 不变；两个安装各自身份不同；alias 持久化；53317 被占时明确启动错误且不发“能接收”公告；测试改端口后公告携带实际 TCP 端口；private 收件目录失败时有错误；stop/start 不重复连接 signals/不留下 socket。

**完成标准**：配置/身份可复现，单槽位只有匹配所有者能释放；传输期间 register 继续响应；接收与发送不能互相覆盖状态。为兼容手机命名现有 app 类最好避免与 bb::cascades::Application 命名冲突（使用命名空间或 ApplicationController）。

### T04 — UDP 与双向 register 发现

**输入**：localInfo、Wi-Fi IPv4 接口、UDP 消息源、register 请求/响应。

**输出**：发现设备列表、最近发现时间、peer protocol 能力和刷新/手动目标入口。

**关键签名**：

```cpp
bool DiscoveryManager::start(const DeviceInfo&, const QNetworkInterface&, QString *error);
void DiscoveryManager::stop();
void DiscoveryManager::refresh();
void DiscoveryManager::registerPeer(const QHostAddress&, quint16 port); // manual HTTP
void DiscoveryManager::onRegistered(const DeviceInfo&, const QHostAddress&);
signals: void peerUpdated(const PeerDevice&);
signals: void discoveryError(const QString&);
```

**验收用例**：PC 先开/BB 先开均能互相出现；同一 fingerprint 重复 UDP/register 只一行；屏蔽 HTTP 回应后 UDP fallback；announce false 不触发回应；自己不会出现；恶意/超大 UDP 不崩溃；设备 IP 更新；peer 广告非默认 TCP 端口；group 加入失败后可手填 PC IP；HTTPS peer 显示不可发送且不发明文到 TLS 端口。

**完成标准**：同一允许组播的 LAN 上 PC 官方 LocalSend 在 BB 启动/手动刷新后 10 s 内看到 BB，BB 也有 PC；抓包可证无回应循环，使用来源 IP 和公告 TCP port；受限 LAN 手动 register 恢复可工作。10 s 是固定测试环境验收目标，不是协议保证。

### T05 — 接收元数据、异步确认及单文件授权

**输入**：prepare JSON、TCP 来源 IP、UI/测试入口选择的 fileId、目标目录、传输槽位。

**输出**：候选元数据、异步接受/拒绝结果、一个 session + 一个 file token、已准备的临时文件；不把整文件读入内存。

**关键签名**：

```cpp
void ReceiveServer::attach(HttpServer *server);
void ReceiveServer::accept(const QString &requestId, const QString &fileId);
void ReceiveServer::reject(const QString &requestId);
signals: void incomingOffer(const QString &requestId, const PeerDevice&,
                            const QList<FileMeta>&);
```

**验收用例**：单文件接受返回 200+token；拒绝 403；同时第二个 prepare 409；多个文件选择第二个，仅返回其 token；上传未选择的 fileId 403；拒绝 path traversal/目录/过大文件；无法建临时文件 500；用户决定等待 30 s 时发现仍工作；断开 pending 请求后不留确认框/锁；120 s 决定超时释放槽位，响应采用本项目拒绝策略。

**完成标准**：PC 官方客户端能弹出等待接收，BB 拒绝时 PC 显示拒绝，选择一个时 PC 只上传该文件；token 使用安全随机源；requestId 与线上 sessionId 分工明确；每种终止路径释放本次资源。完整列表 UI 可以 T09 实现，协议阶段先用明确选择 fileId 的测试入口验证。

### T06 — 接收流、校验与文件提交

**输入**：upload query、源 IP、已接受的文件元数据、已解码 body chunks。

**输出**：实际保存文件、接收进度、最终 HTTP 200/错误；失败时无 final file/本次临时文件。

**关键签名**（ReceiveServer 内部，不额外引入单实现的接口）：

```cpp
bool ReceiveServer::beginUpload(const HttpRequestHead&, int *httpStatus, QString *error);
bool ReceiveServer::appendUpload(const QByteArray&, int *httpStatus, QString *error);
bool ReceiveServer::finishUpload(QString *savedPath, int *httpStatus, QString *error);
signals: void receiveProgress(const QString &sessionId, qint64 done, qint64 total);
signals: void receiveFinished(const QString &sessionId, const QString &savedPath);
```

**验收用例**：PC→BB 的 10 MiB 任意二进制 SHA-256 与源相同；0 B；含中文/空格名称；同名文件保留两份；无 Content-Length 的 chunked；已提供正确 checksum/错误 checksum 分别 200/422；缺参数 400、wrong token/IP 403、duplicate upload 被拒；body 比声明多/少均不成功；磁盘满/无权限/rename 失败清理；断网中断后能重新发起。

**完成标准**：10 MiB 双方最终状态成功且文件 hash 相同；确认成功在 flush/close/rename 之后；10 MiB 与 100 MiB 传输时进程峰值 RSS 增量差建议小于 8 MiB，并记录测量方法。该预算针对大小相关的增长，不限制 Cascades 本身总 RSS；不可测时以有界缓存检查和运行日志说明，性能验证仍标待测。

### T07 — 单文件发送与最终确认

**输入**：peer 地址/端口/协议、可读普通文件路径、localInfo。

**输出**：prepare→等待接收→流式 upload→对端最终确认状态；本地进度、失败原因。

**关键签名**：

```cpp
bool SendManager::sendFile(const PeerDevice&, const QString &path, QString *error);
void SendManager::cancel();
signals: void sendStateChanged(const QString &requestId, const QString &state);
signals: void sendProgress(const QString &requestId, qint64 done, qint64 total);
signals: void sendFinished(const QString &requestId);
```

**实现合同**：QFile 在网络请求期间保持打开/存活，QNAM::post(request, file)；设置正确 Content-Length/Content-Type；文件名只取 basename，fileType 用 application/octet-stream；用 Qt4 QUrl query API；reply 收到 finished 后先看 HTTP status 再分辨网络错误。上传进度 100% 只表示数据送出，不能提前 sendFinished。发送文件中途变化、读盘失败时终止，不能偷偷声明成功。

**验收用例**：BB→PC 官方客户端成功 10 MiB 且 hash 一致；0 B/中文名；接收拒绝 403、busy 409、401、429、500；prepare 204 不发 upload；200 malformed JSON/缺本 file token 不发 upload；upload 422 明确失败；100% 后对端回 500 不能显示成功；传输途中源文件截断；HTTPS peer 在请求前给出不支持提示。

**完成标准**：真机 BB 将 10 MiB 文件传到 PC，PC 显示成功，保存内容一致；无 multipart/base64，无整文件 readAll；QFile/reply 清理与取消安全，进度持续更新且界面/控制入口响应。

### T08 — 取消、超时、掉网与状态收口

**输入**：本地取消、对端 cancel、socket 中断、QTimer、应用退出/网络变化。

**输出**：统一 terminal state，清理当前文件/socket/reply/槽位；新传输可开始。保留发起方/接受方原因，不覆盖已确认成功的结果。

**关键签名**：

```cpp
void ReceiveServer::cancelLocal(const QString &requestId);
void ReceiveServer::handleCancel(const QString &sessionId, const QHostAddress &peer);
void SendManager::handleRemoteCancel(const QString &sessionId, const QHostAddress &peer);
void SendManager::onNetworkLost();
void ReceiveServer::onNetworkLost();
```

**实现合同**：sender cancel abort reply 后有 sessionId 时向 receiver 发 cancel；receiver 本地取消先清理，再向已知 HTTP sender 的公告端口发 best-effort cancel。HTTP router 通过匹配的 sessionId+peer 将回调送往发送状态；不得让任意 cancel 杀死无关会话。等待 prepare 时 abort socket；终止幂等，短时安全保留已结束 ID 的信息用于幂等响应也可以用无操作 200，无需历史库。

**验收用例**：等待确认/等待 upload/传输中分别取消；双方发起取消；连续重复 cancel；wrong IP/session cancel 对实际活动传输无影响；取消通知无法送达时本地仍清理；30 s 无进展释放，持续低速传输可超过 30 s；Wi-Fi 关闭/切换、应用退出/重启；取消与最后一块同时到达时只有一个 terminal result。

**完成标准**：本地取消 2 s 内停止 IO/UI 状态更新（best-effort 通知独立最多 3 s）；不留 partial final files；取消/断网后的下一次 10 MiB 传输成功；所有等待都有期限；每个活动操作仅发一次完成或失败信号。

### T09 — Cascades 日常操作界面（可后置）

**输入**：设备列表、候选元数据、状态/进度、Cascades FilePicker/接收目录权限。

**输出**：原生设备列表/刷新/手动 IP、单文件选择与发送、接收候选选一/接受拒绝、取消、保存位置与错误提示。四张 QML 页面是上限参考，可用一个主 Page + Sheet 实现。

**关键签名**（Application bridge）：

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

**验收用例**：只靠手机触屏完成双向 10 MiB；系统 file picker 取消不产生请求；多候选只能单选；无设备/无文件时禁用发送；HTTPS peer 有明确不可发送说明；shared 权限拒绝时 private 路径可用且显示实际位置；Q10/Passport 等目标设备尺寸至少选一个真机验证；传输中刷新/取消不冻结；后台暂停提示符合当前前台范围。

**完成标准**：原生 Cascades、无 Qt5 QtQuick.Controls，无测试入口/手改路径也能完成日常收发；错误能理解，完成能找到文件。暂缓时标 T09 未完成，交付名称只能是 P，不声称 U。

### T10 — 互通回归、复现实录与 agy 交付

**输入**：BB10 BAR/executable、官方 PC LocalSend 已记录的版本、同一 LAN、测试脚本、测试文件与校验值。

**输出**：可运行 tests/interoperability.py，JSON/HTTP checks、构建/部署复现命令、验收表实际结果、产物 SHA-256；附最小抓包/日志证据。不把测试脚本端到端通过当作 PC 官方互通已通过。

**关键入口**（实现时形成；脚本用 Python stdlib）：

```text
python3 tests/interoperability.py fixtures --output /tmp/localsend-bb10-fixtures
python3 tests/interoperability.py check-http --host <bb-ip> --port 53317
python3 tests/interoperability.py serve-peer --bind <pc-lan-ip> --port <test-port>
```

check-http 与 serve-peer 只访问开发者指定端点，不扫描网络；生产客户端不得依赖 Python。fixtures 创建确定性 0 B/1 KiB/10 MiB/100 MiB 文件、中文名字和外部 SHA-256 清单，避免大量二进制进 Git。

**验收用例**：build-deploy.md 的矩阵逐项执行，核心异常路径包含 token/source IP、chunked、Expect、partial selection、checksum mismatch；双向官方互通 10 MiB；各重试/清理结果；记录 OS/SDK/PC 版本、加密关闭、网络和部署方法。

**完成标准**：P 所有适用项有 PASS+证据，U 再加界面项；未测用 NOT RUN，失败用 FAIL+最小复现；报告不能用“预计能工作”替代实测。没有 SDK/手机时只交源码及 host 验证，明确缺少 P 验收。

## 直接交给 agy 的执行约定

> 在 localsend-bb10 实现 docs/mvp-plan.md 的协议 MVP P。先读 README、protocol-notes、build-deploy 和 AGENTS。按 T00–T08、T10 顺序逐项交付；BB10→PC 和 PC→BB10 的双向 10 MiB 收发是强制验收项，绝不能后置。若完整 UI 拖慢协议调通，仅做必要测试操作入口，T09 后置，但多候选选一功能不能省。使用 BB10 Qt4.8 API/C++03 和平台 JSON，不使用 QJsonDocument/QUrlQuery/QMimeDatabase/QSaveFile/QtQuick.Controls 2。保持原始 HTTP 二进制传输、有界内存、异步确认、session/token/source IP 校验及临时文件提交；遵守 v2.2 checksum 422。不要加 HTTPS、多文件调度、后台服务、Download API 或第三方 UI 框架。代码、注释及技术文本统一英文，文档维护完整中英文两份并同步更新。每完成一个任务记录文件变更、执行命令、结果和未验证项；没有真机/SDK不得宣称 P 完成。T00 发现环境/库不兼容时先提供证据及最小可行替代，再继续。不要提交生成 Makefile、密码、签名私钥或大量文件 fixtures。

## skill 使用及待确认项

已读取 [ponytail](/home/ellery/.config/antigravity/skills/ponytail/SKILL.md)：应用原生 API 优先、将单文件 UploadTask 的职责合入 SendManager、不做未来框架，同时保留完整双向收发、数据安全与输入校验。

已读取 [grill-me](/home/ellery/.config/antigravity/skills/grill-me/SKILL.md)。其唯一正文指令为 `Call the Skill tool with "grilling".`；当前会话没有该 Skill 调用工具，本机也未找到 grilling 指令，故未完成其指定流程。已用两项实际需求澄清落实 UI 可以后置、多候选只接受一个；这不等同于宣称运行了 grilling。

不阻塞本份规划的待确认信息：可用 SDK/部署手段及 BB 真机型号/OS；实际 PC 客户端版本；是否需要默认共享收件目录；100 MiB 上限和超时初值是否满足使用需求。T00/T09 在实现时验证或调整这些项目决策，修改需同步文档和验收。
