# BlackBerry 10 可复用编译与打包工具链容器

[English](README.md) | 简体中文

本目录提供了一个独立且可复用的容器化工具链，用于将原生 **BlackBerry 10 (ARMv7le, Cascades / Qt 4.8.5)** 应用程序交叉编译并打包为 `.bar` 安装包。

该工具链与 `localsend-bb10` 源码解耦，可直接用于编译与打包现有或未来的任何 BlackBerry 10 C++/Cascades 项目。

---

## 架构与工作原理

由于 BlackBerry 官方已关闭 SDK 更新服务器，且官方 BlackBerry 10 Momentics IDE 高度依赖老旧的 32 位二进制文件与 Java 7，直接在现代 64 位 Linux 系统上运行该工具链常会遇到现代 glibc 或 Java 版本不兼容问题。

该容器方案彻底解决了这些问题：
1. **基于 Debian Bookworm 与 32 位 Wine (`wine32`)**：以无头模式稳定运行 QNX Windows 交叉编译器（`qcc.exe`、`ntoarmv7-gcc.exe`、`ntoarmv7-g++.exe`、`moc.exe`、`qmake.exe`）。
2. **内嵌 JRE 1.7 运行环境**：直接使用 Momentics 自带的 Java 7 运行时调用 `blackberry-nativepackager` 与 `blackberry-deploy`，避开现代 OpenJDK 的 Security Manager 限制。
3. **Makefile 路径转换脚本 (`sanitize-makefile.py`)**：自动将 Windows 驱动器盘符（`z:/`）与路径分隔符转换为标准 Linux POSIX 路径，使 GNU make 能够以原生多核性能并行编译（`make -j$(nproc)`）。
4. **通用自动化构建脚本 (`bb10-build`)**：自动扫描挂载工作区中的 `.pro` 工程文件与 `bar-descriptor.xml`，自动完成 ARM 共享库编译、二进制归档与 `.bar` 文件打包。

---

## 目录结构

```text
docker/bb10-toolchain/
├── Dockerfile                  # 通用 BB10 工具链镜像定义
├── README.md                   # 英文说明文档
├── README.zh-CN.md             # 简体中文说明文档
└── bin/
    ├── bb10-build              # 通用工程编译与打包入口
    └── sanitize-makefile.py    # Makefile 路径转换辅助脚本
```

---

## 构建容器镜像

使用 Podman 或 Docker 仅需构建一次镜像：

```bash
# 使用 Podman 构建：
podman build -t bb10-toolchain:latest docker/bb10-toolchain

# 或使用 Docker 构建：
docker build -t bb10-toolchain:latest docker/bb10-toolchain
```

---

## NDK 目录结构要求

容器期望将 BlackBerry 10 Native SDK (NDK) 挂载至 `/bbndk`。宿主机上的 NDK 目录（例如 `~/bbndk`）应包含以下结构：

```text
~/bbndk/
├── host_10_3_1_12/             # QNX 宿主工具集 (win32/x86/usr/bin/...)
├── target_10_3_1_995/           # QNX 目标机头文件与链接库 (qnx6/...)
├── features/                   # Momentics 插件集 (包含 JRE 1.7)
│   └── com.qnx.tools.jre.linux.x86_64_1.7.0.51/
└── configuration/              # 目标配置信息
```

> **关于 NDK 获取**：BlackBerry 10 离线 SDK 压缩包（`bbndk.win32.tools.10.3.1.12.zip` 与 `bbndk.win32.libraries.10.3.1.995.zip`）及 Momentics Linux 安装包已被保存在 Internet Archive 的 `bbdevtools` 归档集合中。

---

## 使用方法：打包任意 BlackBerry 10 应用

### 1. 单命令全自动编译打包

针对任何包含 `.pro` 文件与 `bar-descriptor.xml` 的应用目录：

```bash
podman run --rm \
    -v "${HOME}/bbndk:/bbndk:z" \
    -v "/path/to/your-app:/workspace:z" \
    bb10-toolchain:latest
```

生成的 `.bar` 包将直接输出在工程根目录（`/path/to/your-app/<app-name>.bar`）。

### 2. 交互式终端与自定义编译

若需进入交互式环境直接使用各个工具：

```bash
podman run --rm -it \
    -v "${HOME}/bbndk:/bbndk:z" \
    -v "/path/to/your-app:/workspace:z" \
    bb10-toolchain:latest bash
```

容器内已预先将常用工具注入 `$PATH`：
- `qmake`：BB10 交叉编译工程配置工具
- `qcc`, `ntoarmv7-gcc`, `ntoarmv7-g++`：QNX C/C++ 交叉编译器
- `moc`：Qt 4.8.5 元对象编译器
- `blackberry-nativepackager`：官方 BAR 打包与签名工具
- `blackberry-deploy`：官方设备部署与令牌管理工具
- `bb10-build`：自动化打包流程脚本

---

## 设备部署

打包生成 `.bar` 文件后，可通过以下方式部署至物理 BlackBerry 10 设备（例如 BlackBerry Passport SQW100-1）：

1. **官方开发者模式**：
   ```bash
   ~/bbndk/host_10_3_1_12/win32/x86/usr/bin/blackberry-deploy \
       -installApp \
       -device <DEVICE_IP> \
       -password <DEVICE_PASSWORD> \
       -package <app-name>.bar
   ```

2. **已 Root 设备 / SSH 旁加载**：
   可直接将 `.bar` 文件通过 SFTP/SSH 上传至 `/accounts/1000/shared/documents/`，再使用设备端工具（如 BB10 Sideload Manager 或 `bb10mt`）完成安装。
