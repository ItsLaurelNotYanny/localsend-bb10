# MVP Protocol Constraints



Verified on 2026-10-08. Specification baseline: [LocalSend Protocol v2.2](https://github.com/localsend/protocol/blob/62bd3406ec80d62f2ed46269cdc06c4dcc391083/README.md), commit `62bd3406ec80d62f2ed46269cdc06c4dcc391083`. The repository's v3 protocol is outside this release.

The specification defines wire fields and endpoints. The timeouts, capacities, file count limits, and memory budgets below are project MVP decisions, not LocalSend standards. The official implementation reference is pinned to `c1ce322fb3acf08e44f329b8b7208b8b0d91e244`; interoperability testing must also record the actual PC release version.

## 1. Scope and Compatibility

- Advertise `version: "2.2"`, `protocol: "http"`, `deviceType: "mobile"`, and `download: false`.
- Accept unknown optional fields within protocol major 2. Do not confuse the protocol minor version with the application version. Display incompatible major versions as unavailable and do not send v2 requests to them.
- Obtain network addresses from the UDP sender or TCP socket peer address. Do not add an IP field to JSON.
- Discover peers advertising `https`, but do not request HTTPS endpoints or silently switch them to HTTP. An `announce:false` UDP response can answer their announcement.
- BB10 receives files through the Upload API. `download:false` only means the reverse Download API is unavailable.
- No receiver PIN is enabled. On `401`, the sender reports that the receiver requires a PIN and this MVP does not support PIN entry, then ends the request. Never report false success.
- Use IPv4 on one LAN, with one active transfer, either sending or receiving. Discovery and control endpoints remain available during transfers.
- Send one file per transfer. An incoming offer may contain multiple files; the user accepts at most one through protocol partial acceptance. The user has confirmed this decision.

## 2. Device Discovery

Default UDP multicast: `224.0.0.167:53317`. Default TCP port: `53317`. A peer's TCP port may differ; use its announcement/register request `port`. The UDP source port is not the peer's TCP port.

An HTTP fingerprint is a random string. Generate it on first launch, persist it in QSettings, and reuse it across restarts. It supports deduplication and self-discovery prevention, not HTTP authentication.

Project announcement example; serialize strict JSON without comments or trailing commas:

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

Receive flow: validate, ignore self, update the peer by fingerprint using the observed source IP, and respond only to `announce:true`. Prefer `POST /api/localsend/v2/register` for HTTP peers; on failure, multicast local information with `announce:false`. Do not answer `announce:false` or register back inside a register handler, which would create loops.

Register requests contain the same device information without `announce`. A successful response is `200` with `alias/version/deviceModel/deviceType/fingerprint/download`; the specified response does not require `port/protocol`. Preserve the request target's IP, port, and protocol when decoding a response instead of guessing missing fields.

Allow manual IPv4 and port entry followed by register as the smallest recovery path when multicast fails. This is not the specification's full-subnet Legacy scan; automatic subnet scanning is outside this MVP. Do not invent an online TTL: show the last discovery time, rebuild each refresh's results, and handle actual connection failures when sending.

## 3. Upload API

| Method/endpoint | Request | Normal response |
| --- | --- | --- |
| `POST /api/localsend/v2/register` | Device JSON | `200`, device response JSON |
| `POST /api/localsend/v2/prepare-upload` | `info` and a `files` object keyed by fileId | `200`, sessionId and accepted file token map; `204` when no transfer is needed |
| `POST /api/localsend/v2/upload?sessionId=...&fileId=...&token=...` | Raw file bytes | `200`, empty body |
| `POST /api/localsend/v2/cancel?sessionId=...` | Empty body | `200`, empty body |
| `GET /api/localsend/v2/info` | No body | `200`, device response JSON; debugging only |

Normal upload/cancel `200` responses match the pinned official [v2 server](https://github.com/localsend/localsend/blob/c1ce322fb3acf08e44f329b8b7208b8b0d91e244/packages/core/src/http/server/v2.rs) and [v2 client](https://github.com/localsend/localsend/blob/c1ce322fb3acf08e44f329b8b7208b8b0d91e244/packages/core/src/http/client/v2.rs). The specification does not list cancel error codes; do not present project cancel policies as protocol requirements.

Project single-file preparation example:

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

Acceptance response: `{"sessionId":"session-a","files":{"file-a":"random-token"}}`. `files` is an object, not an array; each key must match the inner id. Tokens authorize individual files and must not be replaced with fingerprints. Never return a local path.

Keep the prepare HTTP request pending asynchronously while the UI decides; do not block the Qt event loop. Reserve the active transfer slot while waiting. Issue a token only after acceptance and successful preparation of the destination file. Explicit rejection returns `403`; `204` must not disguise rejection or completion of a zero-byte file. Zero-byte files still use upload.

| Phase | Specified response | MVP handling |
| --- | --- | --- |
| prepare | `204` | Do not parse JSON or upload; report that the peer requires no transfer |
| prepare | `400` | Invalid metadata |
| prepare | `401` | Sender reports unsupported PIN entry; local PIN remains disabled |
| prepare | `403` | User rejection or project acceptance policy |
| prepare | `409` | Another session occupies the slot |
| prepare | `429` | Sender reports rate limiting; server may use it for requests exceeding capacity |
| prepare | `500` | Receiver internal error, such as destination creation failure |
| upload | `400` | Missing query parameters; project also uses this for invalid length/framing |
| upload | `403` | Invalid token, file authorization, or source IP |
| upload | `409` | Blocked by another session or the file is already uploading |
| upload | `422` | Declared SHA-256 does not match the received content |
| upload | `500` | Internal errors, including write/commit failures |

If sha256 is supplied, verify it incrementally before final acknowledgement. On mismatch, return `422` and remove the temporary file. The MVP sender may send `sha256:null` to avoid a preliminary file scan; the receiver must still honor checksums supplied by peers. Accept 64 hexadecimal digits and compare without case sensitivity.

The specification requires the sender to notify the receiver on cancel. The pinned implementation also lets a receiver call back to the sender's cancel endpoint. Support this behavior by matching source IP and active sessionId before routing to the appropriate send/receive manager. Unknown or ended sessions receive an idempotent empty `200`, matching the reference behavior, without affecting other transfers. Before prepare finishes, the sender has no sessionId; abort the pending connection. Do not depend on the undocumented cancel extension without a sessionId.

## 4. HTTP, Files, and Resource Contract

- HTTP/1.1 only, one request per connection, final response with `Connection: close`. Keep-alive/pipelining is outside this MVP; communicate closure correctly.
- Support Content-Length and chunked bodies, with bounded chunk endings/trailer parsing. Handle case-insensitive headers, arbitrary TCP fragmentation, and headers/body in the same packet. Reject ambiguous lengths, CL+TE, negative/overflow values, and conflicting duplicate headers.
- For `Expect: 100-continue`, send interim `100` after route, authorization, and length validation; otherwise send a final rejection. JSON body permission does not wait for the user's acceptance decision. Reject unsupported Expect values using a standard HTTP error and close.
- Use Qt4 QUrl encoding/decoding for query parameters. Do not concatenate token query strings manually. Parse path and query separately. File bodies are neither multipart/form-data nor base64.
- Project budgets: 16 KiB headers, 64 KiB JSON, at most 64 offered files, at most 8 active sockets, and a 256 KiB application buffer target per connection. HTTP `413/431` protections for oversized JSON/headers are general HTTP policies, not protocol-specific error codes.
- Initial file limit: 100 MiB. Use qint64 for sizes/counts; JSON numbers must be nonnegative integers within a safe representation range. Reject otherwise valid files exceeding the product limit during prepare with policy `403`. Zero-byte files are valid.
- Received byte count must equal the prepare size. Stop immediately on excess data; truncated/interrupted transfers cannot succeed. Reject CL/metadata size disagreement before writing; compare decoded byte counts for chunked bodies.
- Treat file names as display/basename data, never arbitrary paths. Reject `../`, absolute paths, separators, NUL/control characters, empty names, and directory entries. Reject symlink destinations; write only under the configured receive root.
- Default to private `data/received`; shared storage is optional, with explicit fallback/errors on permission failure. Create a random `.part` temporary file in the final directory, then rename there only after all writes, flush/close, and checksum verification succeed. Qt4 has no QSaveFile; incomplete files must not occupy final names.
- Preserve existing names by selecting alternatives such as `name (1).ext`. Uniqueness checks include symlinks. Never overwrite a destination that appeared before commit; clean up/report failure. Show the actual saved name and location.
- Every failure, disconnection, cancellation, or timeout removes this transfer's `.part` and releases its slot. At startup, clean only application-owned remnants in the dedicated temporary subdirectory, not unrelated files.
- QUuid may supply fingerprints. Generate session/token values with a system random source validated by T00, preferably SDK OpenSSL RAND_bytes. Refuse session creation on entropy failure; never use qrand, timestamps, or fixed tokens.
- Initial timeouts: register 3 s, user decision 120 s, accepted-but-not-started upload 60 s, upload without progress 30 s, cancel notification 3 s. Refresh upload timers on progress instead of limiting total transfer time to 30 s. The sender may abort and report timeout after 120 s awaiting acceptance; network changes may fail requests earlier.
- Use the foreground event loop and Qt4 SIGNAL/SLOT connections. Bound each read and throttle UI progress to about 5 Hz. No thread pool/task queue for this MVP; add a single IO worker only if measured device write blocking prevents acceptable UI responsiveness, recording the reason.

## 5. Qt4 API Review

| Capability | Qt4/BB10 approach | Conclusion |
| --- | --- | --- |
| UDP and joinMulticastGroup | QUdpSocket + QNetworkInterface | Present in Qt4.8; verify SDK feature flags and device Wi-Fi |
| TCP server | QTcpServer + QTcpSocket | TCP is available; add a small HTTP parser |
| HTTP client | QNetworkAccessManager/QNetworkReply | Available; `post(request, QIODevice*)` streams QFile |
| JSON | bb::data::JsonDataAccess + QVariantMap/List | Native replacement for QJsonDocument; link bbdata; verify UTF-8 and qint64 round-trip |
| SHA-256 | SDK libcrypto incremental API | Standard Qt4.8 QCryptographicHash lacks Sha256; link/runtime verification is a T00 gate |
| Files | QFile/QFileInfo/QDir/QTemporaryFile | Available; implement temporary-file commit without QSaveFile |
| URL query | Qt4 QUrl query API | Available; no Qt5 QUrlQuery |
| MIME | application/octet-stream for this MVP | No Qt5 QMimeDatabase dependency |
| UI/file selection | bb::cascades + bb::cascades::pickers::FilePicker | Native APIs; no QtQuick.Controls 2 |
| Lifecycle/settings | QObject/QTimer/QSettings/QUuid | Available; verify foreground/network lifecycle on a device |

Sources: [Qt4.8 UDP header](https://github.com/qt/qt/blob/4.8/src/network/socket/qudpsocket.h), [Qt4.8 HTTP client header](https://github.com/qt/qt/blob/4.8/src/network/access/qnetworkaccessmanager.h), [Qt4.8 hash enum](https://github.com/qt/qt/blob/4.8/src/corelib/tools/qcryptographichash.h), [QJsonDocument introduced in Qt5.0](https://doc.qt.io/archives/qt-5.15/qjsondocument.html), and [official BlackBerry JSON example](https://github.com/blackberry/Cascades-Samples/blob/master/jsonreadwrite/src/app.cpp). These establish the API baseline, not verification of a specific BB10 SDK.
