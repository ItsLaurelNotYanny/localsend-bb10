# Fedora 开发与真机验证阶段总结

[English](fedora-development-summary.md) | Simplified Chinese

本文档系统整理并总结了在 Fedora 开发宿主机上，针对黑莓护照机（BlackBerry Passport，BB10 OS 10.3.x）进行 `localsend-bb10` 交叉编译环境建设、真机排障调优、自动化测试以及安全评审的所有关键工作与决策记录。

---

## 1. 容器化打包环境建设（`docker/bb10-toolchain`）

### 1.1 问题与挑战
BlackBerry 10 Native SDK（NDK 10.3.1.995）发布于 2014 年左右，高度依赖 32 位 x86 二进制工具链（`ntoarm-gcc`、`ntoarm-ld`、`qcc` 以及用于打包工具 `blackberry-nativepackager` 的 32 位 Java 运行环境）。现代 64 位 Linux 发行版（如 Fedora 40+）已升级至 glibc 2.39+ 并全面淘汰了旧式 32 位动态库，直接在宿主机运行极易出现动态链接报错、Python 2 缺失以及打包工具段错误。

### 1.2 架构设计与实现
为了确保构建环境 100% 可复现、易迁移，且避免将 BlackBerry 专有 NDK 二进制大文件提交进 Git 仓库：
- **纯净容器底座**：基于 `debian:bookworm-slim` 构建，开启 `i386` 多架构支持，预装 `libc6:i386`、`libstdc++6:i386`、`zlib1g:i386` 和 OpenJDK 运行环境。
- **动态挂载 NDK**：运行时通过 `-v ${HOME}/bbndk:/bbndk:z` 动态挂载本地 NDK，镜像体积控制在 ~765 MB，合规且轻量。
- **Makefile 修复脚本（`sanitize-makefile.py`）**：自动剥离旧版 `qmake` 规范生成的宿主机不兼容参数，确保 `ntoarm-gcc` 能够纯净执行交叉编译。
- **标准化 CLI 工具（`bb10-build`）**：在 `docker/bb10-toolchain/bin/bb10-build` 与 `scripts/build-bb10.sh` 中固化了“编译 ARMv7le ELF 库 -> 剥离符号 -> 生成并签名 `.bar` 安装包”的全流程。后续开发其他 BB10 应用时可直接复用此环境。

---

## 2. 代码更迭与真机问题修复全景（R012 – R018）

在 BlackBerry Passport（IP `192.168.3.124`）真机实测过程中，排查并彻底修复了以下关键问题：

| 缺陷编号 | 现象与问题 | 根因分析 | 修复方案 |
| :--- | :--- | :--- | :--- |
| **R012** | 应用安装后无法启动（提示无入口） | `bar-descriptor.xml` 缺少 Cascades 动态共享库配置及 entry point 声明。 | 配置 `localsend-bb10.so` 为核心资源，设置 `entryPointType` 为 `Qnx/Cascades`，并指定动态入口名称为 `localsend-bb10`。 |
| **R013** | Wi-Fi 部署操作繁琐 | 手工执行 `blackberry-deploy` 参数冗长易错。 | 编写 `scripts/deploy-bb10.sh`，实现一键打包、Wi-Fi 传输安装与自动拉起（`--launch`）。 |
| **R014** | 点击应用图标闪退回桌面 | Cascades QML 引擎是裁剪版，不包含 `Connections` 控件。加载解析中断导致 `createRootObject()` 返回 `NULL`。 | 彻底移除 `Connections`，改用 Cascades 原生支持的属性绑定机制（`property variant peerList: app.peers` 配合 `onPeerListChanged`）。在 `main.cpp` 中增加 QML 解析诊断输出。 |
| **R015** | 接收的文件在文件管理器中不可见 | 文件默认存放在沙盒内部私有目录（`data/received`），BB10 系统自带 Files 应用默认不显示该路径。 | 修改存储路径为公共下载目录 `/accounts/1000/shared/downloads/`，增加自动建目录逻辑并在描述符中启用 `access_shared` 权限。 |
| **R016** | 发现周期极长 / 无法发现局域网设备 | 部分路由器丢弃 UDP 组播（`224.0.0.167`）；且缺乏定期保活广播。 | 采用三通道广播机制（组播 + 有限广播 `255.255.255.255` + 网段定向广播），并引入 30 秒后台心跳定时器（`m_announceTimer`）。 |
| **R017** | 缺少刷新按钮 / ActionBar 按钮冲突 | Cascades `TabbedPane.showTabsOnActionBar` 占满了底部物理动作槽，导致页面动作被折叠进右侧 `⋮` 菜单；界面还存在多余的行内扫描按钮。 | 移除冗余行内扫描按钮，统一将主操作置于顶部导航栏右上角（`TitleBar.acceptAction`），命名为 **Scan**（与官方 LocalSend App 保持一致），确保全局常驻可见。 |
| **R018** | 设备列表存在残留僵尸节点 | 离开局域网的对端设备长期未被清理。 | 实现全效扫描刷新：剔除超过 120 秒未活动的节点、重新加入组播组、重置心跳定时器，并通过 HTTP `/register` 主动探测存活节点。 |

---

## 3. 自动化互操作测试（`tests/interoperability.py`）

为验证与官方 LocalSend 客户端（v1.18.2）的协议兼容性，开发并执行了全套 Python 自动化测试：
- **测试夹具生成**：生成空文件（`0b.bin`）、小文件（`1k.bin`）、10 MiB 大文件（`10m.bin`）、带空格文件名（`notes with spaces.txt`）以及 UTF-8 中文名文件（`测试文件_照片.png`），并生成 SHA-256 校验清单。
- **HTTP 端点校验**：对 PC 官方客户端运行测试，验证 `/api/localsend/v2/info`、`/register`、`/prepare-upload`、`/upload` 4 个核心端点，全部通过（4/4 PASS）。
- **模拟对端流式传输**：测试分块传输、背压流量控制与哈希完整性校验，确保无论 ASCII 还是中文文件名均能稳定传输。

---

## 4. 阶段性代码合回与 Codex 评审方案（安全架构分析）

针对“如何让外部专家或 Codex 进行代码评审”，对比分析了两种路径：

### 方案 A：开放本地 Fedora 机器 Shell / SSH 权限给外部服务（不可行 / 存在高风险）
- **违反最小权限原则**：外部服务或插件直接获得本地操作系统的执行权，存在越权访问宿主机家目录、本地局域网设备的重大风险。
- **凭据泄露风险**：黑莓开发证书签名私钥（`author.p12`）、真机连接密码、宿主机 SSH 密钥可能面临暴露。
- **缺乏审计追踪**：外部执行的修改不易形成不可逆的审查审计日志。

### 方案 B：独立 Git 分支 + Pull Request 评审模式（推荐 / 已实施）
- **绝对安全**：任何证书、私钥或内网地址均严格排除在提交之外（已通过 `.gitignore` 保护），外部仅接触干净源码。
- **评审范围明确**：评审者只需比对 `master...feature/bb10-discovery-ui-review` 的代码 Diff。
- **标准工程实践**：代码审查意见和修改记录在 Git 历史上永久可追溯，确认无误后再合回 `master`。

---

## 5. 当前分支结构

所有修改已按关注点分离原则，在分支 `feature/bb10-discovery-ui-review` 上划分为清晰的原子提交：

1. **`436c048`** - `build: add containerized BB10 toolchain and build/deploy automation scripts`
2. **`0be0de6`** - `feat(bb10): improve discovery heartbeat, public save directory, and Cascades QML UI`
3. **`182b75a`** - `test,docs: expand interoperability tests and update review records R012-R018`
