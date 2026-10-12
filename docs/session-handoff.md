# Session Handoff & Continuity Guide

English | [Simplified Chinese](session-handoff.zh-CN.md)

This briefing provides the complete state and context for continuing development in a fresh Antigravity session.

---

## 1. Current Repository State

- **Branch**: `feature/bb10-discovery-ui-review`
- **BAR Artifact**: `localsend-bb10.bar`
  - **Version**: `0.1.1 (Build 7)` (displays `v0.1.1.7` in top banner and `Build 7` in Settings page)
  - **SHA-256**: `0b89e6760b9f8766b49b75a1aa381bacdd26994100b5ff0ef82a70a02d7b2683`
  - **App Icon**: BlackBerry 10 obsidian dark squircle base with 1:1 official LocalSend proportions, featuring radiant teal radar arcs and center deep black BlackBerry 7-dot fruit logo (`144x144` RGBA PNG).
- **Test Suite**: 64/64 PASS in host container (`podman run --rm -v "$(pwd):/workspace:z" localhost/localsend-dev:latest bash -c "cd /workspace/tests && qmake && make clean && make -j\$(nproc) && ./core-check"`), and verified passing under isolated network (`--network none`).

---

## 2. Hardware Validation & Milestone Achievements (Build 6 & Build 7)

Tested and verified on a physical **BlackBerry Passport** running BB10 OS 10.3.x:

- **Bidirectional File Transfer (MVP Core Requirement: PASSED)**:
  - **BlackBerry Passport -> macOS (`Good Strawberry`)**: Transfer verified successful with full file integrity!
  - **macOS (`Good Strawberry`) -> BlackBerry Passport**: Transfer verified successful with full file integrity!
- **Incoming Transfer Alert on Radar Tab**: Added `Incoming File Transfer!` prompt with `[Accept]` and `[Decline]` buttons directly on Tab 1 (Radar), allowing users to accept incoming files immediately from any tab.
- **Manual IP Compatibility**: Implemented `/api/localsend/v1/info` route in `ReceiveServer` returning HTTP 200 JSON, resolving 404 errors when peers query the legacy endpoint.
- **Diagnostic Logging**: Logs saved to `/accounts/1000/shared/documents/localsend_launch.txt`, viewable natively on device and accessible via the "Open Diagnostic Log" button in Settings.
- **Issue 1 Fixed (Header Version Banner)**: Exposed `app.appVersion` (`v0.1.1.7`) on `UiController` and bound dynamically in `assets/main.qml`.
- **Issue 2 Resolved (Paced Subnet Discovery & QNX Buffer Overrun Protection)**:
  - Replaced un-paced C++ UDP broadcast loop with a 15 ms paced batch sweep engine (`pumpUdpSweep()`, 5 datagrams/batch) preventing QNX 6.5 `io-pkt` `ENOBUFS` socket buffer drops.
  - Expanded UDP send and receive socket buffers to 64 KiB via native `setsockopt(SO_SNDBUF/SO_RCVBUF)`, with `getsockopt` verification logged at runtime.
  - Resolved P1 Head-of-line blocking by tracking attempts per target (capped at 3), deferring transient errors behind unattempted targets (queue tail), and dropping permanent errors.
  - Resolved P2 unpaced fallback by routing HTTP probe failure fallback announcements through `queueUdpSweepTarget()` rather than bursting direct raw datagrams.
  - Bound outgoing multicast interface explicitly via `m_udpSocket->setMulticastInterface(m_interface)`.
  - Replaced invalid `QUdpSocket::isOpen()` checks with canonical `isValid() || isOpen()` checks across announcement and sweep routines.
  - Implemented historical peer MRU prioritization and `/proc/net/arp` cache inspection, putting known active hosts at the head of the sweep queue.
  - Optimized HTTP probe timeout to 450 ms with HTTPS probe fallback on TLS resets.

---

## 3. Network Topology & Test Devices

- **BlackBerry Passport Target**: `192.168.3.124:53317` (BB10 10.3.x, ARMv7le)
- **Fedora PC Peer**: `192.168.3.123:53317` (`Secret Tomato`, HTTP plaintext)
- **macOS Peer**: `192.168.3.161:53317` (`Good Strawberry`, official LocalSend client, encryption disabled for plain HTTP)

---

## 4. Key Verification Findings: Issue 2 Resolution

### Root Cause and Implementation Summary
1. **UDP Buffer Overrun on QNX 6.5 (`io-pkt`)**:
   - Resolved by expanding socket buffers to 64 KiB and pacing transmission via `m_udpSweepTimer` (15 ms intervals, 5 datagrams/batch). Any transient `ENOBUFS` condition requeues the target without dropping it.
2. **QUdpSocket Validity in Qt**:
   - A bound `QUdpSocket` in Qt has `isOpen() == false` because `bind()` does not open the underlying `QIODevice`. Checking `isOpen()` caused UDP writes to silently skip. Replaced with `(m_udpSocket->isValid() || m_udpSocket->isOpen())`.
3. **Target Prioritization**:
   - `hostOrder` in `startSubnetScan()` now checks `m_historicalPeers` and active `/proc/net/arp` entries first, ensuring hosts like `192.168.3.161` receive announcements immediately at scan onset.
4. **HTTP Scan Queue Bounds**:
   - Probes timeout after 450 ms instead of 600 ms; whole scan is bounded by a 12-second deadline timer with HTTPS fallback for TLS resets.

---

## 5. Next Milestone Roadmap (Build 8 / v0.1.2)

1. **Theme System & Dark Mode**:
   - Introduce `ThemeManager` supporting Light, Dark, and Follow-System modes.
   - Decouple hardcoded QML and C++ ARGB colors into semantic palettes (`theme.background`, `theme.cardBackground`, `theme.textPrimary`, `theme.textSecondary`).
   - Switch application icon to the white series matching official LocalSend desktop aesthetics.
2. **HTTPS Support (One-way or Bidirectional)**:
   - Investigate OpenSSL 1.0.0 / 1.0.2 capabilities and TLS certificate management on BlackBerry 10 Cascades.
   - Prioritize unidirectional HTTPS client support (connecting securely to official iOS/Android/macOS peers running TLS mode), evaluating feasibility of embedded server TLS termination.

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
