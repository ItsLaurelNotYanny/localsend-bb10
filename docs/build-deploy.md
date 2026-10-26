# Building and Deploying LocalSend for BlackBerry 10

This document provides instructions for compiling, testing, and deploying LocalSend for BlackBerry 10.

## 1. Prerequisites and Development Environment

### Host Requirements
- Linux (x86_64) host environment
- Docker or Podman container runtime
- Python 3 (stdlib only, for test tooling)

### Building the Host Test Container
The automated host test suite runs inside a Qt container. Build the container image using the provided `Dockerfile.dev`:

```bash
docker build -t localsend-dev:latest -f Dockerfile.dev .
```

### Running the Project Gate & Test Suite
Run the standard project gate before commits and releases:

```bash
# Fast mode (read-only build check)
./check.sh --fast

# Full mode (full clean build and automated unit tests)
./check.sh
```

All 71 core protocol, state-machine, HTTP parser, discovery, persistent local TLS identity, and non-blocking `PosixTlsSocket` unit tests run automatically without external network access.

## 2. Cross-Compilation for BlackBerry 10

The project uses `CMakeLists.txt`, `localsend-bb10.pro`, and `bar-descriptor.xml` at the repository root, linking against the sibling `posix-tls` SDK (`POSIX_TLS_SDK_DIR`).

### BlackBerry 10 Target Architecture
- OS Version: BlackBerry 10 OS 10.3.x
- Architecture: ARMv7le (`armle-v7`)
- UI Framework: BlackBerry 10 Cascades / Qt 4.8.5

### Verifying QNX ARMv7 Cross-Compilation and Symbol Isolation
To cross-compile the main `localsend-bb10` executable for QNX ARMv7 and verify that zero private OpenSSL symbols leak into `.dynsym`:

```bash
./scripts/build_qnx.sh
```

### Building the BAR Application Package
To build and package the native application into a deployable `.bar` archive:

```bash
./scripts/build-bb10.sh
```

This compiles the C++ binaries and packages QML assets and application icons into `localsend-bb10.bar`.

## 3. Device Deployment

Ensure the BlackBerry 10 device has **Development Mode** enabled under *Settings -> Security and Privacy -> Development Mode*.

### Deployment via Development Mode (Wi-Fi)
```bash
./scripts/deploy-bb10.sh --device <device-ip> --password <device-password>
```

### Deployment via SSH (Pre-rooted Devices)
```bash
./scripts/deploy-bb10.sh --device <device-ip> --method ssh
```

## 4. LAN Interoperability Testing

A Python 3 test harness (`tests/interoperability.py`) is provided for testing transfers between a host PC and the device:

```bash
# 1. Generate standard test fixtures (0 B, 1 KiB, 10 MiB binary data):
python3 tests/interoperability.py fixtures --output /tmp/localsend-bb10-fixtures

# 2. Probe device HTTP endpoints:
python3 tests/interoperability.py check-http --host <device-ip> --port 53317

# 3. Run mock LocalSend PC peer on LAN:
python3 tests/interoperability.py serve-peer --bind 0.0.0.0 --port 53317

# 4. Send test file to BlackBerry 10 device:
python3 tests/interoperability.py send --target <device-ip> --port 53317 --file /tmp/localsend-bb10-fixtures/10m.bin
```
