# Fedora 测试环境与上下文交接指南

[English](fedora-handoff.md) | 简体中文

本文档为在 Fedora 宿主机上运行 Antigravity CLI（`agy`）提供完整的上下文交接说明，指导搭建 LocalSend 测试环境、执行 BlackBerry Passport 真机联调，并为后续 T09/T10 开发做好准备。

---

## 1. 项目背景与当前进展

- **项目名称**：`localsend-bb10` — 面向 BlackBerry 10（BB10 10.3.x）的原生 LocalSend 客户端。
- **硬件目标**：BlackBerry Passport（1440x1440 正方形屏幕、高通骁龙 801、ARMv7le）。
- **工具链 / 基线**：Qt 4.8.5、C++03、`armv7le-qcc`、BlackBerry Native SDK 10.3.x。
- **通信协议**：LocalSend Protocol v2.2（HTTP/1.1 JSON REST 传输 + `224.0.0.167:53317` UDP 组播发现）。
- **开发语言规范**：
  - 源码、注释、标识符、构建脚本、日志：100% 英文。
  - 用户对话：以中文为主。
  - 技术文档：严格中英文双语独立维护（`<NAME>.md` 与 `<NAME>.zh-CN.md`）。

### 里程碑完成情况（T01–T08 全部完成）
- **T01（协议 JSON 解析）**：原生 JSON 编解码实现于 `src/protocol/ProtocolJson.cpp`。
- **T02（HTTP 服务与流式解析）**：非阻塞 HTTP/1.1 chunked/定长解析实现于 `src/receive/HttpServer.cpp` 与 `HttpConnection.cpp`。
- **T03（核心引擎与槽位互斥）**：`Application.cpp` 管理配置、设备指纹身份、动态端口重绑定与单一传输槽位互斥。
- **T04（设备发现与节点管理）**：UDP 组播监听 + HTTP `/register` 路由实现于 `src/discovery/DiscoveryManager.cpp`。
- **T05（接收端引擎）**：`/prepare-upload`、`/upload`、`/cancel` 配合流式 `.part` 写入实现于 `src/receive/ReceiveServer.cpp`。
- **T06（发送端引擎）**：流式 `QFile` 上传客户端实现于 `src/send/SendManager.cpp`。
- **T07/T08（端到端集成、取消与恢复）**：Docker 伪环境内 47 项自动化测试全部通过，覆盖 10 MiB 双向传输、UTF-8 中文文件名、流控背压排空与异常取消清理。
- **Codex 独立代码评审**：`docs/review-issues.md` 中所有 11 个问题（R001–R011）经复核已全部 **关闭（CLOSED）**。

---

## 2. Fedora 环境搭建与测试任务

在 Fedora 宿主机上启动 `agy` 后，`agy@fedora` 应指导或执行以下任务：

### 任务 1：在 Fedora 容器中验证代码与测试套件
确认 Fedora 上已安装 Docker 或 Podman，运行全套 47 项核心测试：
```bash
docker build -t localsend-dev:latest -f Dockerfile.dev .
docker run --rm -v $(pwd):/workspace -w /workspace/tests localsend-dev:latest sh -c "make && ./core-check"
```
确认 47/47 测试全部输出 `PASS`。

### 任务 2：配置 Fedora LocalSend 测试节点与防火墙
1. **开放防火墙端口（firewalld）**：
   ```bash
   sudo firewall-cmd --add-port=53317/tcp --zone=public --permanent
   sudo firewall-cmd --add-port=53317/udp --zone=public --permanent
   sudo firewall-cmd --reload
   ```
2. **安装官方 LocalSend 桌面客户端**：
   通过 Flatpak 安装并运行：
   ```bash
   flatpak install flathub org.localsend.localsend_app
   flatpak run org.localsend.localsend_app
   ```
   或启动内置的 Python mock 测试节点：
   ```bash
   python3 tests/interoperability.py --port 53317
   ```

### 任务 3：BlackBerry Passport（已 Root，BB10 10.3.x）真机联调
1. **网络连接**：确保 Fedora PC 与 Passport 处于同一 Wi-Fi 网段（无 AP 隔离）。
2. **二进制部署**：
   - 使用 BB10 NDK 交叉编译为 ARMv7 二进制（或部署测试程序至 `/accounts/1000/shared/misc/`）。
   - 通过 SSH 在 Passport 终端启动运行。
3. **联调验证用例清单**：
   - **用例 1（设备互相发现）**：验证 Fedora 官方客户端与 Passport 能通过组播互相识别。
   - **用例 2（Fedora -> Passport 发送）**：依次测试小文件、中文名文件（`测试_中文.txt`）以及 50 MiB 大文件传输。
   - **用例 3（Passport -> Fedora 发送）**：测试从 Passport 向 Fedora 官方客户端发送文件。
   - **用例 4（传输取消与临时文件清理）**：传输中途取消，确认 Passport 目标目录无残留 `.part` 临时文件。
   - **用例 5（数据完整性校验）**：在两端分别执行 `sha256sum` 比对哈希值完全一致。

---

## 3. Fedora 验证后的后续开发

真机联调闭环后：
1. **T09（Cascades UI 开发）**：适配 BlackBerry Passport 1:1 正方形屏幕（1440x1440）的 QML 界面。
2. **T10（打包与集成发布）**：配置 `bar-descriptor.xml` 资产与权限，打包最终 release `.bar` 安装包。
