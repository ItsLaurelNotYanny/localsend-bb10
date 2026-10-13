# LocalSend for BlackBerry 10

[English](README.md) | 简体中文

一款非官方、基于原生 C++ / Qt Cascades 框架开发的客户端，将现代化的 **[LocalSend](https://github.com/localsend/localsend)** 局域网跨平台文件快传能力带回经典的 **BlackBerry 10** 设备（支持 BlackBerry Passport、Classic、Q10、Z30 等）。

本项目纯粹出于对经典数码硬件的热爱而“为爱发电”——让珍藏的黑莓老设备重新融入现代局域网生态，作为日常无损传图、传文档的实用生产力工具重新焕发生机。

> [!NOTE]
> 本项目为独立的社区开源项目，**与 LocalSend 官方团队无关**。本项目遵循开源的 [LocalSend Protocol v2](https://github.com/localsend/protocol) 协议实现。

---

## 功能特性

- **原生 Cascades 界面**：专为 BB10 定制，原汁原味系统级视觉风格，支持平滑自适应布局与深色/浅色主题。
- **局域网设备自发现**：
  - 支持组播（`224.0.0.167`）与广播 UDP 设备宣告；
  - 智能子网主动探测扫描与节奏化单播回退，轻松跨越路由器 AP 隔离限制；
  - 动态渲染多设备卡片，清晰呈现设备别名与网络状态。
- **流畅的双向文件互传**：
  - **电脑/手机 -> BlackBerry 10**：雷达主页面与传输管理页即时弹出接收横幅，一键轻触确认接收；
  - **BlackBerry 10 -> 电脑/手机**：调用系统原生文件选择器，轻松选取发送照片、文档与各类文件；
  - 基于流式 SHA-256 算法，全流程确保字节级无损完整性校验。
- **个性化设置与诊断**：
  - 支持自定义设备别名与网络端口；
  - 内置诊断日志查看器，支持一键调用系统 Docs To Go 打开查看。

---

## 前置条件与安装说明

由于 BlackBerry 官方签名服务器已于 2022 年正式下线，在 BlackBerry 10 上安装未签名的原生应用需要设备具备 Root 权限。

### 1. 设备 Root 指引
如果您的设备尚未 Root，可以使用黑莓社区开源工具链 [**bb10root / bb10mt**](https://github.com/bb10root/bb10mt)（BlackBerry 10 MultiTool）。
- **安全性说明**：刷机镜像由用户在本地计算机上对官方原版 Autoloader 固件进行解包与打补丁生成，不包含任何来路不明的第三方预装软件，过程透明、安全可靠。

### 2. 安装 LocalSend BAR 软件包
在 Root 后的设备上，可通过 `blackberry-deploy` 或常见的黑莓管理工具直接部署预编译的 `.bar` 格式安装包：
```sh
# 通过 BlackBerry 10 NDK / WebWorks 命令行工具部署
blackberry-deploy -installApp -device <设备IP> -password <设备密码> localsend-bb10.bar
```

---

## 与官方 LocalSend 客户端配合使用

要在 BlackBerry 10 设备与现代设备（Windows、macOS、Linux、Android、iOS）之间互传文件：
1. 确保双方设备连接在同一个 Wi-Fi / 局域网网络下。
2. 打开电脑或手机上的官方 LocalSend 客户端，进入 **“设置 (Settings)”**，确保 **“加密传输 (Encryption / HTTPS)” 已关闭**（保持为标准 HTTP 模式），因为 BB10 客户端采用标准 HTTP 通信。
3. 您的 BlackBerry 手机将自动出现在“附近设备”雷达列表中，即点即发！

---

## 开发文档与详细记录

- [架构设计与协议备忘](docs/protocol-notes.zh-CN.md)
- [构建与部署操作指引](docs/build-deploy.zh-CN.md)
- [双端传输配对测试记录](tests/transfer_paired_table.zh-CN.md)
- [贡献者指引](AGENTS.zh-CN.md)

---

## 开源许可证

本项目基于 [MIT 许可证](LICENSE) 开源。
