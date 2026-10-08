# 构建、部署和验收

[English](build-deploy.md) | 简体中文

当前只写了规划文档。没有检测到常见位置的 BB10 NDK，也没有执行 BB10 构建、BAR 部署、真机或官方客户端互通。以下表格均待实现者填实测结果。

## 环境记录（T00 填写）

| 项目 | 当前值 |
| --- | --- |
| Host OS/CPU | Ubuntu 24.04 LTS (x86_64, 无桌面 Linux) |
| BB10 NDK 版本/路径 | 待本地配置；已配置 Docker 伪环境容器 (localsend-dev) 加速开发 |
| qcc/qmake/make/打包工具实际路径 | 宿主机: /usr/bin/gcc, /usr/bin/make; 容器: /usr/bin/qmake |
| QNX_TARGET/QNX_HOST/Qt 版本 | 容器: Qt 5.15.8 (向下兼容 Qt4 代码规范); 目标真机: BB10 Qt 4.8.5 |
| 目标型号/BB10 OS build | BlackBerry Passport (SQW100-1), Oleksandr Pre-rooted BB10 OS v3 (10.3.3.3216) |
| 真机部署方法/开发模式状态 | 已 Root 设备；支持免签名 sideload / bb10mt 或 SSH 直接部署 |
| PC OS/官方 LocalSend 发布版本 | Fedora Linux, LocalSend v1.18.2 |
| 规范提交 | 62bd3406ec80d62f2ed46269cdc06c4dcc391083，v2.2 |
| 官方代码参考提交 | c1ce322fb3acf08e44f329b8b7208b8b0d91e244 |
| 局域网/接口/路由器限制 | 同一 IPv4 LAN；允许 UDP multicast 和 TCP 53317 |
| 双向 HTTP 验收配置 | PC 关闭加密；使用 Upload API 普通收发模式 |

BB10 工具链及部署能力是先验证的工程条件。不能根据其他项目的迁移归档路径推断本机已有工具。密钥/密码/手机凭据不进仓库，不写在公开日志中。

## 构建入口设计

根目录用 localsend-bb10.pro 和 bar-descriptor.xml；生成文件放 ignored build/。以下是 qmake 设置的方向，**不是已经验证可运行的工程**：

```qmake
APP_NAME = localsend-bb10
CONFIG += qt warn_on cascades10
QT += core network declarative
LIBS += -lbbdata
# Link SDK libcrypto for SHA-256/RAND_bytes after T00 verifies availability.
# List SOURCES/HEADERS; avoid Qt5 modules and IDE-specific absolute paths.
```

如果协议阶段先做独立 QtCore executable，则使用单独 probe/core-check 构建目标；Cascades 应用入口使用 bb::cascades::Application，不复用 QCoreApplication 为最终界面入口。BB10 样例构建参照 [官方 JSON 项目](https://github.com/blackberry/Cascades-Samples/tree/master/jsonreadwrite)；其 config.pri 用 bbdata。实际 SDK 配置、release entrypoint、architecture 与资产路径必须由 T00 核对，不能机械复制旧样例。

descriptor 至少核对 run_native、access_internet；选取/写入 shared 文件需要 access_shared。打包 assets/main.qml、icon 及所有依赖资源。权限不足不得显示“已成功保存到共享目录”。默认 private `data/received` 保证接收路径可操作；日常 UI 应显示保存位置，并在可用时提供共享目录。

T00 完成后补充实际可复制命令：环境初始化 → shadow build → ARMv7 构建 → 打包 → 当前可用的安装/启动方法 → 日志获取。不要在未验证阶段把 blackberry-deploy、debug token/签名流程写成已可用事实；使用实际可工作的工具及凭据方式记录。

## 测试文件和证据规则

- 固定“10 MiB”=10,485,760 bytes；100 MiB=104,857,600 bytes。用户所说 10MB 的验收统一按这组更明确的字节数执行。
- fixture generator 用 Python stdlib，生成确定性二进制、NUL/非 UTF-8 字节、0 B、1 KiB、10 MiB、100 MiB、中文/空格名字。大文件放 /tmp，不提交仓库。
- 文件内容的 SHA-256 在 PC 上独立计算（如 hashlib/sha256sum），目标侧校验使用已验证的 SHA-256。通过读回或再次传回文件可做独立比对；不能只比较大小/显示名。
- host 自动测试用于 JSON、TCP 分片、错误码和状态验证；真机验证用于平台 API、资源/权限和 LAN；官方 PC 客户端用于互通。三者分别记录结果。
- 结果填 PASS/FAIL/NOT RUN。每个 PASS 至少有版本、命令/手工步骤、实际输出或文件 hash；失败有最小复现。不记录 token 或完整带 token query URL。
- 本期允许前台运行；后台/锁屏不作为成功保证，恢复后必须检测失效状态和清理。不要将前台测试推断为后台服务已支持。

## 验收矩阵

所有项目初始 NOT RUN。P 为协议 MVP 必须项，U 为日常界面附加项；P 的发送与接收都是硬性门槛。

| ID | 交付点 | 步骤 | 明确通过条件 | 当前结果 |
| --- | --- | --- | --- | --- |
| A00 | P | 目标 SDK 构建并启动 probe | Qt/API/crypto/file probe 全通过，实际命令可复现 | NOT RUN |
| A01 | P | PC 先启动，再启动 BB；反向启动顺序重复 | 同 LAN 双方 10 s 内发现一次、无自发现 | 宿主机 PASS (core-check: 忽略自发现)；真机待测 |
| A02 | P | 100 次重复公告/注册及 announce:false | 无重复 peer/无回应循环，最近来源 IP/端口正确 | 宿主机 PASS (core-check: announce:false，单行去重)；真机待测 |
| A03 | P | 禁用组播后指定 PC IPv4+port | 手动 register 发现且可继续 HTTP 收发 | 宿主机 PASS (core-check: registerPeer HTTP 手动注册)；真机待测 |
| A04 | P | 官方 PC→BB 10 MiB | PC 最终成功、BB final file 10,485,760 B、SHA-256 一致 | NOT RUN |
| A05 | P | BB→官方 PC 10 MiB | BB 收到最终 200、PC 最终成功、SHA-256 一致 | NOT RUN |
| A06 | P | 0 B 和中文/空格文件名双向 | 数据/名字正确，不将 0 B prepare 误作 204 | NOT RUN |
| A07 | P | 同名文件连续接收两次 | 两份内容保留、无覆盖、实际保存位置正确 | NOT RUN |
| A08 | P | PC 一次提议多个文件，接受其中第二个 | prepare 只返回该 file token；其余不落盘 | NOT RUN |
| A09 | P | 拒绝/第二会话/401/429/500/204 fixtures | 对应状态正确；204 不解析 JSON/不发送 upload | 宿主机 PASS (core-check: 403 拒绝、409 繁忙、422 校验不匹配、500 错误、204 跳过)；真机待测 |
| A10 | P | wrong token、wrong source IP、未授权 fileId、missing query | 拒绝且活动会话/已存文件不受干扰 | NOT RUN |
| A11 | P | raw socket 分片、chunked、trailer、Expect 100 | 有效请求成功；错误格式有界失败、不死锁 | 宿主机 PASS (core-check: 1B 分片、分块尾部、Expect 100、CL+TE 400、超大头 431)；真机待测 |
| A12 | P | 声明正确/错误 checksum | 正确成功；错误 422、不产生 final file | NOT RUN |
| A13 | P | 声明 size 比实际大/小、取消 body | 不成功；清理 .part/槽位 | NOT RUN |
| A14 | P | 等待接收/等待 upload/上传中，双方分别取消 | 本地 2 s 内停止；下一次 10 MiB 成功 | 宿主机 PASS (core-check: 上传取消、远端通知、释放槽位)；真机待测 |
| A15 | P | 30 s 无进展，另测有进展的慢速传输 >30 s | 无进展超时；慢速正常进展不会误超时 | NOT RUN |
| A16 | P | 断 Wi-Fi/应用退出，再恢复或重启 | 无错误 final file；残留只清理自己临时文件；能再次传输 | NOT RUN |
| A17 | P | traversal、绝对路径、symlink、写盘/rename 失败 | 不越界、不覆盖、不假成功 | NOT RUN |
| A18 | P | 10 MiB/100 MiB，测峰值 RSS、轮询 register | 缓存有界；随文件增大额外 RSS 目标 <8 MiB；控制端点保持响应 | NOT RUN |
| A19 | P | PC 加密开启并发 HTTPS 公告 | 显示 HTTPS 不支持；不将明文请求发往该 TLS 端点 | 宿主机 PASS (core-check: sendFile 拒绝 HTTPS 节点；UDP 组播回退)；真机待测 |
| A20 | U | 手机 file picker→选择 PC→发送 | 无开发者命令即可完成 A05，取消 picker 无请求 | NOT RUN |
| A21 | U | 手机候选单选→接收/拒绝/取消 | 无测试配置即可完成 A04/A08/A14 | NOT RUN |
| A22 | U | 权限拒绝/设备离线/屏幕适配 | 明确错误/实际保存位置，UI 持续响应 | NOT RUN |

## agy 每次交付的格式

```text
Task: Txx
Changes: files and final behavior
Verification: commands/manual steps, actual results, evidence paths
Unverified: missing SDK/device/PC checks, never counted as PASS
Limitations: changed release decisions; synchronize protocol and plan if needed
Next task: whether dependencies are satisfied
```

只有 A00–A19 全部适用项有通过证据，才交付 P；A20–A22 完成后交付 U。当前计划没有可省略的核心矩阵项。若某项测试需另一源 IP/设备等额外条件，应安排测试环境或标 NOT RUN，不能自行当作不适用。UI 后置不影响双向传递的强制要求。
