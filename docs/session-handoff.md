# Session Handoff & Continuity Guide

English | [Simplified Chinese](session-handoff.zh-CN.md)

This briefing provides the complete state and context for continuing development in a fresh Antigravity session.

---

## 1. Current Repository State

- **Branch**: `feature/bb10-discovery-ui-review`
- **BAR Artifact**: `localsend-bb10.bar`
  - **Version**: `0.1.1 (Build 7)` (displays `v0.1.1.7` in top banner and `Build 7` in Settings page)
  - **SHA-256**: `9ad0b10f45818df30f0f5b6bb8fe74d9f65a957070146edd18d11370a381bd1b`
  - **App Icon**: BlackBerry 10 obsidian dark squircle base with 1:1 official LocalSend proportions, featuring radiant teal radar arcs and center deep black BlackBerry 7-dot fruit logo (`144x144` RGBA PNG).
- **Test Suite**: 66/66 PASS in host container (`podman run --rm -v "$(pwd):/workspace:z" localhost/localsend-dev:latest bash -c "cd /workspace/tests && qmake && make clean && make -j\$(nproc) && ./core-check"`), and verified passing under isolated network (`--network none`).
- **Review Cadence**: Synchronized at 15-minute intervals between agy and Codex. Physical device verification complete (12-round scan capture + bidirectional transfers), handed off to Codex for final review and release tagging.

---

## 2. Hardware Validation & Milestone Achievements (Build 6 & Build 7)

Tested and verified on a physical **BlackBerry Passport** running BB10 OS 10.3.x:

- **Bidirectional File Transfer (MVP Core Requirement: PASSED & VERIFIED)**:
  - **BlackBerry Passport -> Fedora PC (`Secret Tomato`)**: Verified with full file integrity!
    - Filename: `Mar 01.pdf`
    - Size: `56,565` bytes (Source and Destination match exactly)
    - SHA-256: `4e6584fff8cf8e7c7e520e8247a13f237e5f63ca72b306b35923d9f67549f390`
    - Destination Path: `/home/ellery/Downloads/Mar 01.pdf`
    - Timestamp: `2026-10-13 14:31` AEDT (logged in Fedora LocalSend HTTP `/register` at `14:32:01.741170`)
  - **BlackBerry Passport -> Fedora PC (`Secret Tomato`)**: Verified with full file integrity!
    - Filename: `IMG_20261010_185044.jpg` (saved as `IMG_20261010_185044 (2).jpg`)
    - Size: `1,640,388` bytes
    - SHA-256: `aea2b9d48ec27243ae25b8d3396c3dfcf182d5ad43a36ead2c48b9300b520ba3`
    - Timestamp: `2026-10-13 12:09:09` AEDT (logged in Fedora LocalSend `shared_preferences.json`)
  - **Fedora PC (`Secret Tomato`) -> BlackBerry Passport**: Verified with full file integrity!
    - Filename: `Screenshot From 2025-10-24 10-33-53.png`
    - Size: `183,144` bytes
    - SHA-256: `ddb76b7dba40101f0d332aa4d832b9f4a5ce01c239c7e972439ea7f8d23589f7`
  - **BlackBerry Passport -> Ubuntu PC (`Fast Onion`, 192.168.3.111)**: Verified with full file integrity!
    - Filename: `IMG_20240202_162742.jpg`
    - Size: `1,138,317` bytes (Source and Destination match exactly)
    - SHA-256: `cbce1646becd8869e86f81e9cbb54823fbd776d03cb62a09f84417b3c28e65da`
    - Destination Path: `/home/ellery/Downloads/IMG_20240202_162742.jpg` on `ellery@ellery-ThinkPad-X1-Carbon-5th`
    - Timestamp: `2026-10-13 15:43` AEDT
  - **Ubuntu PC (`Fast Onion`, 192.168.3.111) -> BlackBerry Passport**: Verified with full file integrity!
    - Filename: `Assessment.pdf`
    - Size: `32,445` bytes (Source and Destination match exactly)
    - SHA-256: `8469fd2112bc581355b4ddf506c12c56f83e82224047f9c4e9a54852fdf90257`
    - Source Path: `/home/ellery/Downloads/Assessment.pdf` on `ellery@ellery-ThinkPad-X1-Carbon-5th`
    - Destination Path: Passport `/accounts/1000/shared/downloads/LocalSend/Assessment.pdf`
    - Timestamp: `2026-10-13 15:47` AEDT
- **Passive 29-Round Scan Packet Capture (`tests/passive_discovery.log`)**:
  - Implemented non-interfering passive UDP sniffer (`tests/capture_passive.py`) bound specifically to multicast group `224.0.0.167:53317`, running concurrently with official Fedora LocalSend (`Secret Tomato`) and official macOS LocalSend (`Good Strawberry`).
  - Passively captured 29 consecutive active scan bursts from BlackBerry Passport (10 cold starts + 19 manual scans) with microsecond timestamps and raw JSON records (`tests/passive_discovery_raw.jsonl`).
  - Validated with interface kernel packet capture `tests/localsend.pcap` (recorded via `tcpdump -i any -s0 'udp port 53317'`, capturing Wi-Fi traffic corresponding to Linux cooked v2 interface index 2 / `wlp3s0`).
  - Observed consistent arrival and dynamic card rendering across bursts; multicast socket semantics guarantee zero unicast interference; native kernel drop counters unmeasured on target.
  - Multi-peer concurrent discovery dynamically populated cards for Fedora PC (`192.168.3.123`), macOS (`192.168.3.161` `Good Strawberry`), Ubuntu PC (`192.168.3.111` `Fast Onion`), and iPhone (`192.168.3.233` `Tidy Peach`).
- **Incoming Transfer Alert on Radar Tab**: Added `Incoming File Transfer!` prompt with `[Accept]` and `[Decline]` buttons directly on Tab 1 (Radar), allowing users to accept incoming files immediately from any tab.
- **Manual IP Compatibility**: Implemented `/api/localsend/v1/info` route in `ReceiveServer` returning HTTP 200 JSON, resolving 404 errors when peers query the legacy endpoint.
- **Diagnostic Logging**: Logs saved to `/accounts/1000/shared/documents/localsend_launch.txt`, viewable natively on device and accessible via the "Open Diagnostic Log" button in Settings.
- **Issue 1 Fixed (Header Version Banner)**: Exposed `app.appVersion` (`v0.1.1.7`) on `UiController` and bound dynamically in `assets/main.qml`.
- **Issue 2 Resolved (Paced Subnet Discovery & Buffer Pacing)**:
  - Replaced un-paced C++ UDP broadcast loop with a 15 ms paced batch sweep engine (`pumpUdpSweep()`, 5 datagrams/batch) designed to mitigate potential QNX 6.5 socket buffer pressure (buffer exhaustion cause retained as hypothesis unless measured on device).
  - Expanded UDP send and receive socket buffers to 64 KiB via native `setsockopt(SO_SNDBUF/SO_RCVBUF)`, with `getsockopt` verification logged at runtime.
  - Resolved P1 Head-of-line blocking by tracking attempts per target (capped at 3), deferring transient errors behind unattempted targets (queue tail), and dropping permanent errors.
  - Resolved P2 unpaced fallback by routing HTTP probe failure fallback announcements through `queueUdpSweepTarget()` rather than bursting direct raw datagrams.
  - Bound outgoing multicast interface explicitly via `m_udpSocket->setMulticastInterface(m_interface)`.
  - Replaced invalid `QUdpSocket::isOpen()` checks with canonical `isValid() || isOpen()` checks across announcement and sweep routines.
  - Implemented historical peer MRU prioritization and `/proc/net/arp` cache inspection, putting known active hosts at the head of the sweep queue.
  - Optimized HTTP probe timeout to 450 ms with HTTPS probe fallback on TLS resets.

---

## 3. Network Topology & Test Devices

- **BlackBerry Passport Target**: `192.168.3.124:53317` (BB10 10.3.x, ARMv7le, fingerprint `706d97c2-9785-4fb8-948e-947729b96fc2`)
- **Fedora PC Peer**: `192.168.3.123:53317` (`Secret Tomato`, HTTP plaintext)
- **Ubuntu PC Peer**: `192.168.3.111:53317` (`Fast Onion`, official LocalSend client v2.1, ThinkPad-X1-Carbon-5th, Linux desktop, fingerprint `E1D2DEBDE15B82640527BD3C068D52ED545BE62DCE6B86244D0FDF4BFEB8C83D`)
- **macOS Peer**: `192.168.3.161:53317` (`Good Strawberry`, official LocalSend client, encryption disabled for plain HTTP, fingerprint `640A6CBE76F440C93FF915E28666CEA9D1877A679861E2131E57A7B0E73D886C`)
- **iPhone Peer**: `192.168.3.233:53317` (`Tidy Peach`, official iOS LocalSend client, HTTPS/TLS)

---

## 4. Key Verification Findings: Issue 2 Resolution

### Implementation Summary & Hypotheses
1. **QNX 6.5 Socket Buffer Pacing Hypothesis (`io-pkt`)**:
   - Bounded retries defer transient failures and abandon a target after three failed write attempts; transmission pacing via `m_udpSweepTimer` (15 ms intervals, 5 datagrams/batch) and buffer expansion to 64 KiB aim to mitigate pressure, while socket buffer exhaustion causes and delivery effects remain design hypotheses.
2. **QUdpSocket Validity in Qt**:
   - A bound `QUdpSocket` in Qt has `isOpen() == false` because `bind()` does not open the underlying `QIODevice`. Checking `isOpen()` caused UDP writes to silently skip. Replaced with `(m_udpSocket->isValid() || m_udpSocket->isOpen())`.
3. **Target Prioritization**:
   - `hostOrder` in `startSubnetScan()` now checks `m_historicalPeers` and active `/proc/net/arp` entries first, ensuring hosts receive announcements immediately at scan onset.
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
3. **Character Encoding & Non-ASCII / Special Filename Handling**:
   - Audit UTF-8 string encoding across HTTP JSON payloads, QML string bindings, and Cascades UI file offer dialogs to eliminate character garbling observed during file transfers.
4. **System-wide Share Menu Integration (BB10 Invocation Framework)**:
   - Register an `invoke-target` with `bb.action.SHARE` action and MIME filter `*/*` (with `file://` URI scheme) in `bar-descriptor.xml`.
   - Enable LocalSend to appear in the BlackBerry 10 native File Manager, Picture Gallery, and system "Share" menus when users select files.
   - Listen to `bb::system::InvokeManager::invoked` in C++, automatically staging the shared file URI into `UiController::selectFile()` and transitioning to the Radar/Send tab for instant peer selection.

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
