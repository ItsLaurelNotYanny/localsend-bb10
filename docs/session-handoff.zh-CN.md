# 会话交接与上下文指引

[English](session-handoff.md) | Simplified Chinese

本文档为开启全新 Antigravity 对话会话提供完整状态、修复上下文与下一步验证指引。

---

## 1. 当前仓库状态

- **当前分支**：`feature/bb10-discovery-ui-review`
- **BAR 安装包产物**：`localsend-bb10.bar`
  - **版本**：`0.1.1 (Build 7)`（界面顶部横幅显示 `v0.1.1.7`，Settings 页显示 `Build 7`）
  - **SHA-256**：`9ad0b10f45818df30f0f5b6bb8fe74d9f65a957070146edd18d11370a381bd1b`
  - **应用图标**：黑莓深空炭黑质感圆角底托（底托与雷达外延比例与官方 1:1 绝对一致），电光翡翠绿雷达波纹，核心嵌入黑曜石深黑黑莓 7 瓣果标（`144x144` 高清 RGBA PNG）。
- **自动化测试集**：宿主机容器内 66/66 项测试全部通过（`podman run --rm -v "$(pwd):/workspace:z" localhost/localsend-dev:latest bash -c "cd /workspace/tests && qmake && make clean && make -j\$(nproc) && ./core-check"`），并在无网络隔离环境（`--network none`）下验证全量通过。
- **评审轮询节奏**：按用户指示，agy 与 Codex 双方均对齐约 10 分钟周期的同步轮询与反馈节奏。真机实测验证全量完成（12 轮扫描抓包 + 双向文件传输），已交接给 Codex 进行终审及发布打标签。

---

## 2. 真机验证里程碑与成果（Build 6 与 Build 7）

在运行 BB10 OS 10.3.x 的 **BlackBerry Passport** 真机上完成实测验证：

- **双向文件传输（MVP 核心交付要求：验证全量通过）**：
  - **BlackBerry Passport -> Fedora PC（`Secret Tomato`）**：验证通过，文件完整性全量校验一致！
    - 文件名：`Mar 01.pdf`
    - 大小：`56,565` 字节（源端与接收端字节完全一致）
    - SHA-256：`4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390`
    - 保存路径：`/home/ellery/Downloads/Mar 01.pdf`
    - 接收时间：`2026-10-13 14:31` AEDT（确凿记录于 Fedora LocalSend 系统日志 HTTP `/register` 14:32:01.741170）
  - **BlackBerry Passport -> Fedora PC（`Secret Tomato`）**：验证通过，文件完整性全量校验一致！
    - 文件名：`IMG_20261010_185044.jpg`（保存为 `IMG_20261010_185044 (2).jpg`）
    - 大小：`1,640,388` 字节
    - SHA-256：`aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3`
    - 接收时间：`2026-10-13 12:09:09` AEDT（确凿记录于 Fedora LocalSend `shared_preferences.json` 历史数据库）
  - **Fedora PC（`Secret Tomato`） -> BlackBerry Passport**：验证通过，文件完整性全量校验一致！
    - 文件名：`Screenshot From 2025-10-24 10-33-53.png`
    - 大小：`183,144` 字节
    - SHA-256：`ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7`
  - **BlackBerry Passport -> Ubuntu PC（`Fast Onion`，192.168.3.111）**：验证通过，文件完整性全量校验一致！
    - 文件名：`IMG_20240202_162742.jpg`
    - 大小：`1,138,317` 字节（源端与接收端字节完全一致）
    - SHA-256：`cbce1646becd8869e86f81e9cbb54823fbd776d03cb62a09f84417b3c28e65da`
    - 保存路径：`ellery@ellery-ThinkPad-X1-Carbon-5th` 上的 `/home/ellery/Downloads/IMG_20240202_162742.jpg`
    - 接收时间：`2026-10-13 15:43` AEDT
  - **Ubuntu PC（`Fast Onion`，192.168.3.111） -> BlackBerry Passport**：验证通过，文件完整性全量校验一致！
    - 文件名：`Assessment.pdf`
    - 大小：`32,445` 字节（源端与接收端字节完全一致）
    - SHA-256：`8469fd2112bc581355b4ddf506c12c56f83e82224047f9c4e9a54852fdf90257`
    - 源端路径：`ellery@ellery-ThinkPad-X1-Carbon-5th` 上的 `/home/ellery/Downloads/Assessment.pdf`
    - 保存路径：Passport 上的 `/accounts/1000/shared/downloads/Assessment.pdf`
    - 接收时间：`2026-10-13 15:47` AEDT
- **真机 29 轮被动扫描抓包证据（`tests/passive_discovery.log`）**：
  - 在 53317 端口上运行精确定向绑定至组播地址 `224.0.0.167:53317` 的非干扰式被动 UDP 监听器（`tests/capture_passive.py`），与官方 Fedora LocalSend（`Secret Tomato`）和官方 macOS LocalSend（`Good Strawberry`）同时活跃共存。
  - 连续捕获来自 BlackBerry Passport 的 29 轮主动发现扫描周期（10 次冷启动 + 19 次手动扫描），微秒级时间戳与原始 JSON 记录（`tests/passive_discovery_raw.jsonl`）全量归档。
  - 配合网络接口内核原生抓包文件 `tests/localsend.pcap`（通过 `tcpdump -i any -s0 'udp port 53317'` 捕获，记录对应 Linux cooked v2 接口索引 2 / `wlp3s0` 的无线网络流量）验证。
  - 观察到各轮扫描中稳定的数据包到达与卡片动态渲染；组播套接字语义从内核层保证零单播抢占；内核丢包计数未在真机上测量。
  - 多端并发局域网发现正常，多设备卡片动态渲染完整覆盖 Fedora PC（`192.168.3.123`）、macOS（`192.168.3.161` `Good Strawberry`）、Ubuntu PC（`192.168.3.111` `Fast Onion`）与 iPhone（`192.168.3.233` `Tidy Peach`）。
- **主雷达页接收提示横幅**：在 Tab 1（Radar）主界面新增 `Incoming File Transfer!` 浮层横幅与 `[Accept]` / `[Decline]` 按钮，用户无需手动切页即可直接一键接收。
- **手动 IP 接口兼容**：在 `ReceiveServer` 中支持 `/api/localsend/v1/info` 路径并返回 200 JSON，彻底解决对端手动输入 IP 探测时报错 404 的问题。
- **诊断日志真机直读**：日志落地路径更名为 `/accounts/1000/shared/documents/localsend_launch.txt`，支持系统原生文本查看器打开，并在 Settings 页面新增“查看诊断日志”直接调起。
- **问题 1 修复（顶部横幅版本号）**：在 `UiController` 中暴露 `app.appVersion`（`v0.1.1.7`），并在 `assets/main.qml` 中动态绑定。
- **问题 2 修复（节奏化子网发现与缓冲区限流）**：
  - 将无延迟 C++ UDP 广播循环重构为 15 ms 批次的节奏化发送引擎（`pumpUdpSweep()`，每批 5 个数据包），旨在缓解潜在的 QNX 6.5 套接字缓冲区压力（缓冲区耗尽原因在真机测量前保留为假说）。
  - 通过 POSIX 原生 `setsockopt(SO_SNDBUF/SO_RCVBUF)` 将 UDP 收发套接字缓冲区扩容至 64 KiB，并在运行时通过 `getsockopt` 验证并记录诊断日志。
  - 彻底消除 P1 队头阻塞（Head-of-Line Blocking）：记录每个目标尝试次数（上限 3 次），偶发/瞬时网络错误移入队列末尾限流重试，永久性错误直接丢弃并继续处理队列后续目标。
  - 彻底消除 P2 非节奏化回退突发：将 HTTP 探测超时的回退 UDP 广播统一收拢至 `queueUdpSweepTarget()` 节奏化队列，杜绝并发超时瞬间突发未节奏化的原始 UDP 数据包。
  - 通过 `m_udpSocket->setMulticastInterface(m_interface)` 显式绑定组播物理出接口。
  - 纠正了误判 `QUdpSocket::isOpen()` 为 false 的缺陷，在所有广播宣告与扫描路径中改为 Qt 规范的 `isValid() || isOpen()` 判定。
  - 实现历史对端 MRU 优先队列与系统 `/proc/net/arp` 缓存读取，将已知活跃主机排在子网扫描队列的最前列。
  - 将离线 HTTP 探测超时压缩至 450 ms，总扫描截止放宽至 12 秒，并在 TLS 重置时回退 HTTPS 探测。

---

## 3. 网络拓扑与测试设备

- **BlackBerry Passport 目标设备**：`192.168.3.124:53317`（BB10 10.3.x，ARMv7le，指纹 `706d97c2-9785-4fb8-948e-947729b96fc2`）
- **Fedora PC 对端**：`192.168.3.123:53317`（`Secret Tomato`，明文 HTTP）
- **Ubuntu PC 对端**：`192.168.3.111:53317`（`Fast Onion`，官方 LocalSend 客户端 v2.1，ThinkPad-X1-Carbon-5th，桌面 Linux，指纹 `E1D2DEBDE15B82640527BD3C068D52ED545BE62DCE6B86244D0FDF4BFEB8C83D`）
- **macOS 对端**：`192.168.3.161:53317`（`Good Strawberry`，官方 LocalSend 客户端，已关闭传输加密，指纹 `640A6CBE76F440C93FF915E28666CEA9D1877A679861E2131E57A7B0E73D886C`）
- **iPhone 对端**：`192.168.3.233:53317`（`Tidy Peach`，官方 iOS LocalSend 客户端，HTTPS/TLS）

---

## 4. 关键验证成果：问题 2 解决方案

### 根因与实现总结
1. **QNX 6.5 套接字缓冲区限流设计假说（`io-pkt`）**：
   - 扩容套接字缓冲区至 64 KiB，并通过 `m_udpSweepTimer` 按照 15 ms / 每批 5 个数据包节奏化发送缓解突发饱和。遇到偶发 `ENOBUFS` 自动重新入队并限流等待下一个定时器 tick，绝不丢弃数据包（套接字发送缓冲区耗尽保留为设计假说，未在真机上测量物理计数器）。
2. **Qt 中 QUdpSocket 有效性判定**：
   - 绑定的 `QUdpSocket` 在 Qt 中 `isOpen() == false`（因为 `bind()` 不会调用 `QIODevice::open()`），原代码直接检查 `isOpen()` 导致 UDP 写入被误跳过。修复为 `(m_udpSocket->isValid() || m_udpSocket->isOpen())`。
3. **目标优先级重排**：
   - `startSubnetScan()` 中的 `hostOrder` 优先加入 `m_historicalPeers` 与活跃 `/proc/net/arp` 记录，确保各活跃对端在扫描初始阶段即收到宣告。
4. **HTTP 扫描队列超时控制**：
   - 探测超时缩短至 450 ms，整体扫描设有 12 秒总截止保护，并在 TLS 协议重置时回退 HTTPS 探测。

---

## 5. 下一阶段路线图规划（Build 8 / v0.1.2）

1. **主题系统与暗色模式（Dark Mode）**：
   - 引入 `ThemeManager` 支持浅色、暗色（Dark）及跟随系统的主题切换；
   - 将 QML 与 C++ 中硬编码的 30 余处色彩解耦为语义化调色盘（`theme.background`、`theme.cardBackground`、`theme.textPrimary`、`theme.textSecondary`）；
   - 正式切换应用图标至纯白系底托，更契合官方桌面质感。
2. **HTTPS 安全传输支持（单向或双向待定）**：
   - 调研并梳理 BlackBerry 10 Cascades 系统内置 OpenSSL 1.0.0 / 1.0.2 的加密套件兼容性与证书管理机制；
   - 优先攻关客户端单向 HTTPS 支持（使 Passport 能够主动向开启 TLS 加密的官方 iOS / Android / macOS 设备发起安全互传），并评估嵌入式服务端启用 TLS 终止的可行性。
3. **字符编码与非 ASCII / 特殊字符文件名支持**：
   - 全面审查 HTTP JSON 负载、QML 字符串绑定及 Cascades UI 文件提示弹窗的 UTF-8 编解码链路，彻底收敛传输提示中的文字乱码现象。

---

## 6. 验证操作命令

### 在 BlackBerry Passport 上部署并启动
```bash
./scripts/deploy-bb10.sh --device 192.168.3.124 --password '<设备密码>' --launch
```

### 查看真机运行日志
```bash
cat /accounts/1000/shared/documents/localsend_launch.txt
```

### 重新构建打包
```bash
./scripts/build-bb10.sh
```

### 运行宿主机自动化测试
```bash
podman run --rm -v "$(pwd):/workspace:z" localhost/localsend-dev:latest bash -c "cd /workspace/tests && qmake && make clean && make -j\$(nproc) && ./core-check"
```
