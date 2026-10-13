# 构建、部署和验收

[English](build-deploy.md) | 简体中文

当前只写了规划文档。没有检测到常见位置的 BB10 NDK，也没有执行 BB10 构建、BAR 部署、真机或官方客户端互通。以下表格均待实现者填实测结果。

## 环境记录（T00 填写）

| 项目 | 当前值 |
| --- | --- |
| Host OS/CPU | Fedora Linux (x86_64) |
| BB10 NDK 版本/路径 | `~/bbndk` (host_10_3_1_12, target_10_3_1_995) + 容器化 `bb10-toolchain:latest` |
| qcc/qmake/make/打包工具实际路径 | `bb10-toolchain:latest` (qcc, qmake, blackberry-nativepackager 1.11, blackberry-deploy 1.5.0) |
| QNX_TARGET/QNX_HOST/Qt 版本 | 目标机: BB10 Qt 4.8.5 (ARMv7le, Cascades) |
| 目标型号/BB10 OS build | BlackBerry 10 设备 (IP 192.168.3.124) |
| 真机部署方法/开发模式状态 | Wi-Fi 开发模式 (`blackberry-deploy`) 实机部署验证通过 |
| PC OS/官方 LocalSend 发布版本 | Fedora Linux, LocalSend v1.18.2 |
| 规范提交 | 62bd3406ec80d62f2ed46269cdc06c4dcc391083，v2.2 |
| 官方代码参考提交 | c1ce322fb3acf08e44f329b8b7208b8b0d91e244 |
| 局域网/接口/路由器限制 | 同一 IPv4 LAN (192.168.3.0/24)；允许 UDP 组播及 TCP 53317 |
| 双向 HTTP 验收配置 | PC 关闭加密；使用 Upload API 普通收发模式 |

BB10 工具链及部署能力是先验证的工程条件。不能根据其他项目的迁移归档路径推断本机已有工具。密钥/密码/手机凭据不进仓库，不写在公开日志中。

## 构建与打包设计

项目在根目录使用 `localsend-bb10.pro` 和 `bar-descriptor.xml`；交叉编译与打包通过 `scripts/build-bb10.sh` 脚本自动化，部署通过 `scripts/deploy-bb10.sh` 脚本执行。

### 已验证的 qmake 配置 (`localsend-bb10.pro`)

```qmake
APP_NAME = localsend-bb10
TARGET = localsend-bb10
TEMPLATE = app

INCLUDEPATH += src
DEPENDPATH += src

# BlackBerry 10 Cascades 配置
contains(CONFIG, cascades10) | qnx {
    CONFIG += qt warn_on cascades10
    QT += core network declarative
    LIBS += -lbbdata -lbbcascadespickers -lcrypto
} else {
    # 宿主机测试与验证配置
    CONFIG += qt warn_on console
    QT += core network
    LIBS += -lcrypto
}
```

### 应用部署描述符 (`bar-descriptor.xml`)
- 可执行入口资产：`<asset path="%CPU/o/localsend-bb10" entry="true" type="Qnx/Elf">localsend-bb10</asset>`
- Cascades 入口点：`<cascades><entryPoint><name>localsend-bb10</name></entryPoint></cascades>`
- 系统权限声明：`run_native` (system="true"), `access_internet`, `access_shared`
- 打包资产：`assets/main.qml`, `assets/icon.png`
- 动态链接库路径：`LD_LIBRARY_PATH="app/native/lib:/usr/lib/qt4/lib"`

### 可复现的构建与部署命令

```bash
# 1. 为 BlackBerry 10 ARMv7le 交叉编译并打包 BAR 安装包：
./scripts/build-bb10.sh

# 2. 通过 blackberry-deploy 部署至 BlackBerry Passport（开发者模式）：
./scripts/deploy-bb10.sh --device 192.168.1.150 --password <device-password>

# 3. 针对已 Root 设备通过 SSH/sideload 直接旁加载：
./scripts/deploy-bb10.sh --device 192.168.1.150 --method ssh

# 4. 局域网互操作性与测试套件（Python 3 标准库）：
# 生成确定性测试夹具（0 B, 1 KiB, 10 MiB, 中文名及 SHA-256 清单）：
python3 tests/interoperability.py fixtures --output /tmp/localsend-bb10-fixtures

# 校验运行中设备的 HTTP 接口健康状态：
python3 tests/interoperability.py check-http --host 192.168.1.150 --port 53317

# 在局域网 PC 启动模拟 LocalSend 对端节点服务：
python3 tests/interoperability.py serve-peer --bind 0.0.0.0 --port 53317

# 从 PC 直接向黑莓 BB10 设备发送文件（流式传输）：
python3 tests/interoperability.py send --target 192.168.1.150 --port 53317 --file /tmp/localsend-bb10-fixtures/10m.bin
```

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
| A00 | P | 目标 SDK 构建并启动 probe | Qt/API/crypto/file probe 全通过，实际命令可复现 | PASS (bb10-toolchain 容器: liblocalsend-bb10.so ARMv7le 编译通过，localsend-bb10.bar 打包完成，SHA-256: 506913edb8eb78ba8b56ed3be454d0d6cda4f1c44ed3f17f02b7aedf6b9dab38) |
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
