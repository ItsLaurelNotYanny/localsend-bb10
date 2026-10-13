# BlackBerry 10 Reusable Build & Packaging Toolchain Container



This directory provides an independent, reusable container toolchain for cross-compiling and packaging native **BlackBerry 10 (ARMv7le, Cascades / Qt 4.8.5)** applications into `.bar` packages.

It is decoupled from the `localsend-bb10` source tree and can be reused to build and package any existing or future BlackBerry 10 C++/Cascades project.

---

## Architecture & How It Works

Because BlackBerry closed down their SDK update servers and the official BlackBerry 10 Momentics IDE relies on legacy 32-bit host binaries and Java 7, running the toolchain on modern 64-bit Linux host systems directly often encounters modern glibc or Java incompatibilities.

This container solves these issues cleanly:
1. **Debian Bookworm base with 32-bit Wine (`wine32`)**: Seamlessly runs QNX's Windows-based cross-compilers (`qcc.exe`, `ntoarmv7-gcc.exe`, `ntoarmv7-g++.exe`, `moc.exe`, `qmake.exe`) headless.
2. **Bundled JRE 1.7 execution**: Directly invokes `blackberry-nativepackager` and `blackberry-deploy` using the bundled Java 7 runtime, bypassing modern OpenJDK Security Manager incompatibilities.
3. **Makefile path sanitizer (`sanitize-makefile.py`)**: Automatically converts Windows drive letters (`z:/`) and path separators into standard Linux POSIX paths so that GNU make can build at native multi-core speed (`make -j$(nproc)`).
4. **Universal builder script (`bb10-build`)**: Automatically discovers any `.pro` and `bar-descriptor.xml` file in the mounted workspace, compiles the ARM shared object, stages the binary, and produces a deployable `.bar` package.

---

## Directory Structure

```text
docker/bb10-toolchain/
├── Dockerfile                  # Generic BB10 toolchain image definition
├── README.md                   # English documentation
├── README.zh-CN.md             # Simplified Chinese documentation
└── bin/
    ├── bb10-build              # Universal project build & packaging entrypoint
    └── sanitize-makefile.py    # Makefile path translation helper
```

---

## Building the Container Image

Build the container image once using Podman or Docker:

```bash
# Using Podman:
podman build -t bb10-toolchain:latest docker/bb10-toolchain

# Or using Docker:
docker build -t bb10-toolchain:latest docker/bb10-toolchain
```

---

## NDK Directory Layout

The container expects the BlackBerry 10 Native SDK (NDK) to be mounted at `/bbndk`. The host NDK folder (e.g. `~/bbndk`) should have the following layout:

```text
~/bbndk/
├── host_10_3_1_12/             # QNX host tools (win32/x86/usr/bin/...)
├── target_10_3_1_995/           # QNX target libraries and headers (qnx6/...)
├── features/                   # Bundled Momentics features (including JRE 1.7)
│   └── com.qnx.tools.jre.linux.x86_64_1.7.0.51/
└── configuration/              # Target configuration files
```

> **Note on NDK acquisition**: BlackBerry 10 offline SDK zip archives (`bbndk.win32.tools.10.3.1.12.zip` and `bbndk.win32.libraries.10.3.1.995.zip`) and the Momentics Linux installer are preserved in the Internet Archive `bbdevtools` collection.

---

## Usage: Packaging Any BlackBerry 10 App

### 1. Automated One-Command Build

To build and package any project containing a `.pro` file and `bar-descriptor.xml`:

```bash
podman run --rm \
    -v "${HOME}/bbndk:/bbndk:z" \
    -v "/path/to/your-app:/workspace:z" \
    bb10-toolchain:latest
```

The output `.bar` file will be generated in the root of your project directory (`/path/to/your-app/<app-name>.bar`).

### 2. Interactive Shell / Custom Build Steps

To enter an interactive environment with all QNX tools and wrappers in `$PATH`:

```bash
podman run --rm -it \
    -v "${HOME}/bbndk:/bbndk:z" \
    -v "/path/to/your-app:/workspace:z" \
    bb10-toolchain:latest bash
```

Available commands inside the container:
- `qmake`: Cross-compiling qmake for BB10
- `qcc`, `ntoarmv7-gcc`, `ntoarmv7-g++`: QNX C/C++ cross-compilers
- `moc`: Qt 4.8.5 Meta-Object Compiler
- `blackberry-nativepackager`: Official BAR packager and signer
- `blackberry-deploy`: Official deployment and debug token utility
- `bb10-build`: Automated build script

---

## Deployment to Device

Once packaged into a `.bar` file, you can deploy the app to a physical BlackBerry 10 device (e.g. BlackBerry Passport SQW100-1) using either:

1. **Official Development Mode**:
   ```bash
   ~/bbndk/host_10_3_1_12/win32/x86/usr/bin/blackberry-deploy \
       -installApp \
       -device <DEVICE_IP> \
       -password <DEVICE_PASSWORD> \
       -package <app-name>.bar
   ```

2. **Pre-rooted / SSH Sideload**:
   Directly upload the `.bar` package or transfer via SFTP/SSH to `/accounts/1000/shared/documents/` and install using on-device tools (such as BB10 Sideload Manager or `bb10mt`).
