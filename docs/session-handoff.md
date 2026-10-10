# Session Handoff & Continuity Guide

English | [Simplified Chinese](session-handoff.zh-CN.md)

This briefing provides the complete state and context for continuing development in a fresh Antigravity session.

---

## 1. Current Repository State

- **Branch**: `feature/bb10-discovery-ui-review`
- **BAR Artifact**: `localsend-bb10.bar`
  - **Version**: `0.1.1 (Build 6)` (displays `v0.1.1.6` in top banner and `Build 6` in Settings page)
  - **SHA-256**: `b391a9d3201adda9bb8a2255949cfcfffe97e0b35a3457b3183c2653d997f78c`
- **Test Suite**: 60/60 PASS in host container (`podman run --rm -v "$(pwd):/workspace:z" localhost/localsend-dev:latest bash -c "cd /workspace/tests && qmake && make clean && make -j\$(nproc) && ./core-check"`).

---

## 2. Hardware Validation & Milestone Achievements (Build 6)

Tested and verified on a physical **BlackBerry Passport** running BB10 OS 10.3.x:

- **Bidirectional File Transfer (MVP Core Requirement: PASSED)**:
  - **BlackBerry Passport -> macOS (`Good Strawberry`)**: Transfer verified successful with full file integrity!
  - **macOS (`Good Strawberry`) -> BlackBerry Passport**: Transfer verified successful with full file integrity!
- **Incoming Transfer Alert on Radar Tab**: Added `Incoming File Transfer!` prompt with `[Accept]` and `[Decline]` buttons directly on Tab 1 (Radar), allowing users to accept incoming files immediately from any tab.
- **Manual IP Compatibility**: Implemented `/api/localsend/v1/info` route in `ReceiveServer` returning HTTP 200 JSON, resolving 404 errors when peers query the legacy endpoint.
- **Diagnostic Logging**: Logs saved to `/accounts/1000/shared/documents/localsend_launch.txt`, viewable natively on device and accessible via the "Open Diagnostic Log" button in Settings.
- **Issue 1 Fixed (Header Version Banner)**: Exposed `app.appVersion` (`v0.1.1.6`) on `UiController` and replaced the hardcoded `v0.1.1.4` string in `assets/main.qml`.

---

## 3. Network Topology & Test Devices

- **BlackBerry Passport Target**: `192.168.3.124:53317` (BB10 10.3.x, ARMv7le)
- **Fedora PC Peer**: `192.168.3.123:53317` (`Secret Tomato`, HTTP plaintext)
- **macOS Peer**: `192.168.3.161:53317` (`Good Strawberry`, official LocalSend client, encryption disabled for plain HTTP)

---

## 4. Key Remaining Defect: Issue 2 (Active Subnet Scan Misses macOS)

### Symptom
- Active scan initiated from the Passport does not discover the macOS peer (`192.168.3.161:53317`) after 2+ scan rounds.
- Quitting and restarting LocalSend on macOS immediately makes it appear on the Passport.

### Root Cause Analysis
1. **UDP Buffer Overrun on QNX 6.5 (`io-pkt`)**:
   - In `DiscoveryManager::startSubnetScan()`, a unicast UDP announcement is sent in a tight C++ `for` loop across all 88 target IPs in `m_scanQueue`.
   - On QNX 6.5, the non-blocking UDP socket send buffer (`SO_SNDBUF`) is only ~8-9 KiB. Blasting 88 packets with zero delay causes socket buffer exhaustion around packet 30, returning `ENOBUFS` and dropping subsequent packets without transmission. Host `192.168.3.161` (index ~73) was dropped before reaching the network interface.
2. **HTTP Scan Queue Latency & Stalls**:
   - The concurrent HTTP probing pool uses 12 workers with 600 ms timeouts per unreachable host. Inactive hosts before index 73 consume multiple seconds, frequently hitting the 8-second total scan deadline before probing host `.161`.
3. **Why Restarting macOS Works**:
   - When the macOS client launches, it broadcasts UDP multicast (`224.0.0.167:53317`) and broadcast (`255.255.255.255:53317`). Passport's background UDP listener receives the packet directly via `onUdpReadyRead()`, immediately registering `Good Strawberry`.

---

## 5. Roadmap for Next Dedicated Session (Focus: Issue 2)

1. **Pace Unicast UDP Transmission & Expand Socket Buffer**:
   - Call `setsockopt(..., SOL_SOCKET, SO_SNDBUF, &bufSize, ...)` to expand the send buffer to at least 64 KiB.
   - Replace the zero-delay C++ loop in `startSubnetScan()` with paced timer batches (e.g. 5–10 packets every 10 ms) or handle `ENOBUFS` by retrying.
2. **Prioritize Subnet Probe Targets**:
   - Reorder `m_scanQueue` so that known peers, ARP cache entries, and nearby neighbor IP addresses are probed first.
3. **HTTP Probe Optimization & Manual IP Input**:
   - Shorten connection timeout on unresponsive HTTP probe sockets.
   - Provide a direct IP input / manual probe field in the UI for instant single-host registration.

---

## 6. Verification Commands

### Deploy and Launch on BlackBerry Passport
```bash
./scripts/deploy-bb10.sh --device 192.168.3.124 --password '<device-password>' --launch
```

### Inspect Device Launch Logs
```bash
cat /accounts/1000/shared/documents/localsend_launch.txt
```

### Rebuild Package
```bash
./scripts/build-bb10.sh
```

### Run Host Test Suite
```bash
podman run --rm -v "$(pwd):/workspace:z" localhost/localsend-dev:latest bash -c "cd /workspace/tests && qmake && make clean && make -j\$(nproc) && ./core-check"
```
