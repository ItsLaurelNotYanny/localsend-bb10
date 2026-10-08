# Fedora Test Environment & Context Handoff Guide

English | [Simplified Chinese](fedora-handoff.zh-CN.md)

This document provides a complete transition briefing for running the Antigravity CLI agent (`agy`) on Fedora to set up the LocalSend test environment, execute real-device validation with BlackBerry Passport, and prepare for T09/T10 development.

---

## 1. Project Context & Current Status

- **Project**: `localsend-bb10` — Native BlackBerry 10 (BB10 10.3.x) implementation of LocalSend.
- **Hardware Target**: BlackBerry Passport (1440x1440 square display, Qualcomm Snapdragon 801, ARMv7le).
- **Toolchain / Baseline**: Qt 4.8.5, C++03, `armv7le-qcc`, BlackBerry Native SDK 10.3.x.
- **Protocol**: LocalSend Protocol v2.2 (HTTP/1.1 JSON REST + UDP Multicast discovery on `224.0.0.167:53317`).
- **Development Language Rules**:
  - Code, comments, identifiers, logs: 100% English.
  - User conversation: Primarily Chinese (中文).
  - Technical documentation: Bilingual (`<NAME>.md` and `<NAME>.zh-CN.md`).

### Milestone Completion (T01–T08 Complete)
- **T01 (Protocol JSON)**: Native JSON encode/decode in `src/protocol/ProtocolJson.cpp`.
- **T02 (HTTP Server & Streaming Parser)**: Non-blocking HTTP/1.1 chunked/fixed parser in `src/receive/HttpServer.cpp` & `HttpConnection.cpp`.
- **T03 (Application Engine & Mutex)**: `Application.cpp` managing settings, device identity, port rebinding, and single-transfer slot mutex.
- **T04 (Discovery & Peer Management)**: UDP multicast listener + HTTP `/register` fallback in `src/discovery/DiscoveryManager.cpp`.
- **T05 (Receiver Implementation)**: `/prepare-upload`, `/upload`, `/cancel` with bounded streaming file write (`.part` files) in `src/receive/ReceiveServer.cpp`.
- **T06 (Sender Implementation)**: Streaming `QFile` upload client in `src/send/SendManager.cpp`.
- **T07/T08 (Integration, Cancellation, Recovery)**: 47 automated tests passing in Docker (`localsend-dev:latest`), validating 10 MiB bidirectional transfers, UTF-8 Chinese filenames, flow-control backpressure, and cancel cleanup.
- **Codex Peer Review**: All 11 issues (R001–R011) in `docs/review-issues.md` are verified and **CLOSED**.

---

## 2. Fedora Environment Setup Tasks

When running on Fedora, `agy@fedora` should guide or execute the following setup steps:

### Task 1: Verify Code & Pseudo-Environment in Docker
Ensure Docker / Podman is available on Fedora and verify the 47 core tests:
```bash
docker build -t localsend-dev:latest -f Dockerfile.dev .
docker run --rm -v $(pwd):/workspace -w /workspace/tests localsend-dev:latest sh -c "make && ./core-check"
```
Ensure all 47 tests output `PASS`.

### Task 2: Configure Fedora LocalSend Peer & Firewall
1. **Firewall (firewalld)**:
   ```bash
   sudo firewall-cmd --add-port=53317/tcp --zone=public --permanent
   sudo firewall-cmd --add-port=53317/udp --zone=public --permanent
   sudo firewall-cmd --reload
   ```
2. **Official LocalSend Desktop Client**:
   Install via Flatpak:
   ```bash
   flatpak install flathub org.localsend.localsend_app
   flatpak run org.localsend.localsend_app
   ```
   Alternatively, run the built-in mock server:
   ```bash
   python3 tests/interoperability.py --port 53317
   ```

### Task 3: BlackBerry Passport (Rooted BB10 10.3.x) Real-Device Validation
1. **Network**: Ensure Fedora PC and Passport are connected to the same Wi-Fi subnet (no AP isolation).
2. **Binary Deployment**:
   - Cross-compile for ARMv7 using BB10 NDK (or deploy test binary to `/accounts/1000/shared/misc/`).
   - Run executable via SSH on Passport.
3. **Validation Test Cases**:
   - **Case 1 (Discovery)**: Verify Fedora and Passport discover each other via multicast.
   - **Case 2 (Fedora -> Passport)**: Send small file, Chinese filename (`测试_中文.txt`), and 50 MiB large file.
   - **Case 3 (Passport -> Fedora)**: Send file from Passport to Fedora.
   - **Case 4 (Cancellation & Cleanup)**: Cancel in-flight transfer and verify `.part` files are removed.
   - **Case 5 (SHA-256 Consistency)**: Verify matching `sha256sum` on both ends.

---

## 3. Next Steps After Fedora Validation

Once real-device validation is verified:
1. **T09 (Cascades UI Implementation)**: Build QML UI matching BlackBerry Passport 1:1 screen (1440x1440).
2. **T10 (Packaging & Integration)**: Configure `bar-descriptor.xml` and package release `.bar` archive.
