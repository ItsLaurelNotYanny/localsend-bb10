# MVP 协议约束

[English](protocol-notes.md) | 简体中文

核对日期：2026-10-08。规范基线：[LocalSend Protocol v2.2](https://github.com/localsend/protocol/blob/62bd3406ec80d62f2ed46269cdc06c4dcc391083/README.md)，提交 `62bd3406ec80d62f2ed46269cdc06c4dcc391083`。仓库中的 v3 不属于本期目标。

规范决定线上字段和端点；以下超时、容量、文件数、内存预算是本项目的 MVP 决策，不是 LocalSend 标准。官方实现参考固定于提交 `c1ce322fb3acf08e44f329b8b7208b8b0d91e244`；最终互通还须记录实测 PC 发布版本。

## 1. 范围及兼容边界

- 自己公告 `version: "2.2"`、`protocol: "http"`、`deviceType: "mobile"`、`download: false`。
- 对同一 major 的 2.x 接受未知可选字段；不得把 minor 版本与应用版本混淆。major 不兼容的设备仅显示不可用，不向它发送 v2 请求。
- 网络地址来自 UDP 数据报来源或 TCP socket 的 peer address；JSON 中不增加 IP 字段。
- 接收广播中的 `https` 设备信息，但本期不向 HTTPS 端点发请求，不静默改为 HTTP。对该公告可用 `announce:false` UDP 回应。
- BB10 接收用 Upload API；`download:false` 只说明未开放反向 Download API。
- 没有接收端 PIN；发送端遇到 `401` 显示“接收设备需要 PIN，MVP 暂不支持”，结束本次请求。无需向对端谎称成功。
- IPv4 同一 LAN，单个活动传输（发送或接收）；发现和控制端点在传输期间继续工作。
- 发送一次一个文件；接收候选列表可以包含多个文件，用户最多选一个，按规范部分接受。此决策已经用户确认。

## 2. 设备发现

UDP 默认组播 `224.0.0.167:53317`，TCP 默认端口 `53317`。TCP peer 的实际端口可以不同，使用公告/注册请求的 `port`；UDP 来源端口不等于 peer 的 TCP 端口。

组播数据包会在所有支持组播的网卡上加入并发送。子网广播严格限制在活跃的物理 LAN 网卡及其有效定向广播地址（如 `192.168.x.255`）。明确排除点对点/VPN 接口（例如 Tailscale TUN 网卡）和回环接口，以避免系统路由错误、Socket 内核报错以及广播泄漏。跨子网或处于 Tailscale 网络中的对端，通过手动输入 IP 发起单播 HTTP 注册来进行发现和通信。

HTTP 模式的 fingerprint 是随机字符串。本项目首次启动生成、写入 QSettings，后续重启复用；它用于去重及避免自发现，不作为 HTTP 身份认证。

本机公告的项目示例（必须输出严格 JSON，无注释/尾逗号）：

```json
{
  "alias": "BlackBerry LocalSend",
  "version": "2.2",
  "deviceModel": "BlackBerry",
  "deviceType": "mobile",
  "fingerprint": "persisted-random-device-id",
  "port": 53317,
  "protocol": "http",
  "download": false,
  "announce": true
}
```

收报流程：校验 → 忽略自己 → 用 fingerprint 更新设备与实际来源 IP → 仅 `announce:true` 触发回应。HTTP peer 优先 `POST /api/localsend/v2/register`；失败后组播本机信息且 `announce:false`。不回应 `announce:false`，不在 register handler 中反向注册，避免循环。

register 请求使用同样的设备信息但不带 `announce`。响应是 `200` 和设备信息：`alias/version/deviceModel/deviceType/fingerprint/download`，规范响应不要求 `port/protocol`。解析响应时沿用此次请求的目标 IP、端口及协议，不猜测缺失字段。

手动输入 IPv4 和端口后向该端点 register，作为组播失败的最小恢复办法。它不是规范所述的全子网 Legacy 扫描；本期明确不实现自动扫全网。设备不设置虚假的在线 TTL：显示“最近发现”，每次刷新重建本轮结果，发起传输时处理实际连接失败。

## 3. Upload API

| 方法/端点 | 请求 | 正常响应 |
| --- | --- | --- |
| `POST /api/localsend/v2/register` | 设备 JSON | `200`，设备响应 JSON |
| `POST /api/localsend/v2/prepare-upload` | `info` + 以 fileId 为键的 `files` 对象 | `200`，sessionId + 接受文件的 token map；无需传输则 `204` |
| `POST /api/localsend/v2/upload?sessionId=...&fileId=...&token=...` | 原始文件字节 | `200`，空 body |
| `POST /api/localsend/v2/cancel?sessionId=...` | 空 body | `200`，空 body |
| `GET /api/localsend/v2/info` | 无 body | `200`，设备响应 JSON；只供调试 |

Upload/cancel 的正常 `200` 与固定参考官方实现一致：[v2 server](https://github.com/localsend/localsend/blob/c1ce322fb3acf08e44f329b8b7208b8b0d91e244/packages/core/src/http/server/v2.rs)、[v2 client](https://github.com/localsend/localsend/blob/c1ce322fb3acf08e44f329b8b7208b8b0d91e244/packages/core/src/http/client/v2.rs)。规范未给 cancel 列出错误码，不应把项目自己的 cancel 错误策略说成协议要求。

单文件 prepare 的本项目示例：

```json
{
  "info": {
    "alias": "BlackBerry LocalSend",
    "version": "2.2",
    "deviceModel": "BlackBerry",
    "deviceType": "mobile",
    "fingerprint": "persisted-random-device-id",
    "port": 53317,
    "protocol": "http",
    "download": false
  },
  "files": {
    "file-a": {
      "id": "file-a",
      "fileName": "\u6d4b\u8bd5 file.bin",
      "size": 10485760,
      "fileType": "application/octet-stream",
      "sha256": null,
      "preview": null,
      "metadata": null
    }
  }
}
```

接受响应：`{"sessionId":"session-a","files":{"file-a":"random-token"}}`。files 不是数组；键与内部 id 必须一致；token 是每个文件独立的授权字符串，不能用 fingerprint 替代。不返回本地路径。

prepare handler 保持 HTTP 请求异步等待 UI 决定；不阻塞 Qt 事件循环。等待期间占住活动传输槽位。只在用户接受且目标文件已可创建后发 token。没有选择并点击拒绝返回 `403`，不能用 `204` 假装拒绝或零字节文件已完成。0 B 文件仍走 upload。

| 阶段 | 规范响应 | 本期处理 |
| --- | --- | --- |
| prepare | `204` | 发送方不解析 JSON、不 upload，显示对端无需传输 |
| prepare | `400` | 元数据无效 |
| prepare | `401` | 发送方提示 PIN 不支持；本机不启用 PIN |
| prepare | `403` | 用户拒绝/本项目策略不接受 |
| prepare | `409` | 被另一个会话占用 |
| prepare | `429` | 发送方明确显示限流；服务端可对超出容量请求使用 |
| prepare | `500` | 接收端内部错误，例如创建目标文件失败 |
| upload | `400` | 缺 query 参数；本项目也用于长度/请求格式错误 |
| upload | `403` | token、文件授权或源 IP 无效 |
| upload | `409` | 另一个会话阻塞/本文件已在上传 |
| upload | `422` | 声明的 SHA-256 与实际内容不同 |
| upload | `500` | 写盘/落盘失败等内部错误 |

若有 sha256，必须增量验证，在最终确认前比较；不一致返回 `422` 并删除临时文件。发送端 MVP 可以发 `sha256:null`，避免预先扫一遍文件；这不能成为接收端忽略对端 checksum 的理由。接收 checksum 接受 64 位十六进制，比较时不区分大小写。

cancel 的协议要求是发送者通知接收者。固定参考官方实现还支持接收方回调发送方的 cancel 端点；本项目兼容这一行为：匹配 source IP + 活动 sessionId 后路由到相应 send/receive manager。错误或已结束会话采用参考实现的幂等 `200` 空响应，但不影响其他会话。等待 prepare 时发送方尚无 sessionId，最小实现通过断开 pending 请求取消；本项目不依赖未写入规范的无 sessionId cancel 扩展。

## 4. HTTP、文件和资源合同

- 仅 HTTP/1.1、每连接一个请求，最终响应 `Connection: close`。不实现 keep-alive/pipelining，但正确告知客户端关闭。
- 支持 Content-Length 和 chunked body，chunked 结束/trailer 有界解析；大小写不敏感 headers、TCP 任意分片、header/body 同包都必须正确。拒绝有歧义的长度、CL+TE、负值/溢出、重复冲突头。
- `Expect: 100-continue`：路由和授权/长度校验通过后发送 interim `100`，失败直接最终响应；JSON 的 100 不等待用户接收决定。未知 Expect 使用标准 HTTP 错误并关闭。
- Query 使用 Qt4 QUrl 的编码/解码接口，禁止字符串手拼 token；路径与 query 分开解析。不接受 multipart/form-data 或 base64 包装的文件内容。
- 项目预算：header 16 KiB、JSON 64 KiB、候选文件最多 64 项、活动 socket 最多 8 个、应用单连接缓冲目标 256 KiB。JSON/headers 超上限是 HTTP `413/431` 等通用保护，不冒充协议专属错误码。
- 单文件暂定最大 100 MiB；qint64 表示 size/计数，JSON 数字必须是非负整数且在安全表示范围。超产品文件大小上限在 prepare 阶段作为策略拒绝 `403`；零字节有效。
- 接收计数必须等于 prepare 的 size；过长立即停止，过短或中断绝不能成功。CL 与声明 size 不一致在写盘前拒绝；chunked 解码后计数核对。
- 文件名只作展示/生成 basename，不能拼入任意路径：拒绝 `../`、绝对路径、分隔符、NUL/控制字符、空名及目录文件。拒绝 symlink 目标，文件只写入已确定的接收根目录。
- 默认写 app 私有 `data/received`，共享文件路径作为可选收件目录，权限失败时明确回退/报错。临时文件建在最终目录，使用随机 `.part` 名；全部写完、flush/close 成功且 hash 合格后同目录 rename。Qt4 无 QSaveFile，不允许未完成文件占用最终文件名。
- 同名文件不覆盖，使用 `name (1).ext` 等新名字；唯一目标检查包含符号链接，提交时不覆盖已出现的目标，失败需清理/报错。成功 UI 显示实际保存名和位置。
- 所有失败/断网/取消/超时清理本次 `.part` 并释放槽位；启动时只清理专用临时子目录中应用自己创建的残留，不扫描删除其他文件。
- fingerprint 可用 QUuid；session/token 用经 T00 验证的系统随机源（优先 SDK OpenSSL RAND_bytes），失败则拒绝建立会话，不使用 qrand、时间戳或固定 token。
- 项目超时初值：register 3 s、用户决定 120 s、已接受但未上传 60 s、上传无进展 30 s、取消通知 3 s。上传按持续进展刷新计时，不能用 30 s 总时长限制慢速 10 MiB 文件。发送端等待确认 120 s 到期可断开并报告超时；网络变化时均可提前失败。
- 前台事件循环负责网络；Qt4 SIGNAL/SLOT 连接。每轮读取有界，节流 UI progress 到约 5 Hz；不为 MVP 引入线程池/任务队列。如果真机写盘阻塞导致 UI 不达标，再增加单独 IO worker 并记录原因。

## 5. Qt4 API 核对

| 能力 | Qt4/BB10 路线 | 结论 |
| --- | --- | --- |
| UDP、joinMulticastGroup | QUdpSocket + QNetworkInterface | Qt4.8 有；BB10 SDK feature flags/真机 Wi-Fi 须验证 |
| TCP 服务端 | QTcpServer + QTcpSocket | 有 TCP，无现成 HTTP 服务端，需小型 HTTP 解析层 |
| HTTP 客户端 | QNetworkAccessManager/QNetworkReply | 有；`post(request, QIODevice*)` 可流式发送 QFile |
| JSON | bb::data::JsonDataAccess + QVariantMap/List | 原生替代 QJsonDocument，链接 bbdata，UTF-8 转换与 qint64 round-trip 须验证 |
| SHA-256 | SDK libcrypto 的增量 SHA-256 API | 标准 Qt4.8 QCryptographicHash 没有 Sha256；实际链接/运行是 T00 门槛 |
| 文件 | QFile/QFileInfo/QDir/QTemporaryFile | 有；无 QSaveFile，自己做临时文件提交 |
| URL query | QUrl 的 Qt4 query API | 有；没有 Qt5 QUrlQuery |
| 文件 MIME | 本期统一 application/octet-stream | 不依赖 Qt5 QMimeDatabase |
| UI/文件选择 | bb::cascades + bb::cascades::pickers::FilePicker | 原生平台 API，不能写 QtQuick.Controls 2 |
| 生命周期/配置 | QObject/QTimer/QSettings/QUuid | 可用；监听前台/网络变化，在真机验证 |

依据：[Qt4.8 UDP 头文件](https://github.com/qt/qt/blob/4.8/src/network/socket/qudpsocket.h)、[Qt4.8 网络客户端头文件](https://github.com/qt/qt/blob/4.8/src/network/access/qnetworkaccessmanager.h)、[Qt4.8 hash 枚举](https://github.com/qt/qt/blob/4.8/src/corelib/tools/qcryptographichash.h)、[QJsonDocument 从 Qt5.0 引入](https://doc.qt.io/archives/qt-5.15/qjsondocument.html)、[BlackBerry 官方 JSON 示例](https://github.com/blackberry/Cascades-Samples/blob/master/jsonreadwrite/src/app.cpp)。这些说明 API 基线，不替代特定 BB10 SDK 的编译验证。
