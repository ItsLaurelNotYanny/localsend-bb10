# Session Handoff & Continuity Guide

English | [Simplified Chinese](session-handoff.zh-CN.md)

This briefing provides the complete state and context for continuing development in a fresh Antigravity session.

---

## 1. Current Repository State

- **Branch**: `feature/bb10-discovery-ui-review`
- **Latest Commit**: `6ebce9c` (`fix(qml): replace unsupported Connections element with native Cascades property bindings to prevent launch crash`)
- **Git Remote**: `origin/feature/bb10-discovery-ui-review` (in sync, clean working tree)
- **BAR Artifact**: `localsend-bb10.bar`
  - **Version**: `0.1.1 (Build 3)` (displays `v0.1.1.3` on header banner and Settings About page)
  - **SHA-256**: `c6a0ffe89aadebc9d472aa4ee36ea4f18c2f33efc6fa82980ab1e231770bc19a`
- **Test Suite**: 60/60 PASS in host container (`podman run --rm -v "$(pwd):/work:Z" -w /work/tests localhost/localsend-dev bash -c "make && ./core-check"`).

---

## 2. Network Topology & Test Devices

- **BlackBerry Passport Target**: `192.168.3.124:53317` (BB10 10.3.x, ARMv7le)
- **Fedora PC Peer**: `192.168.3.123:53317` (`Secret Tomato`, HTTP plaintext)
- **macOS Peer**: `192.168.3.161:53317` (`Good Strawberry`, official LocalSend client)
  - *Note*: Official LocalSend enables HTTPS by default. Discovery probe automatically identifies it via fallback HTTPS probe. For MVP bidirectional file transfers, disable encryption in macOS LocalSend (**Settings -> Advanced -> Encryption: Off**).

---

## 3. Key Issues Diagnosed & Resolved in Latest Build

### A. Multi-Peer Rendering Bug (R019)
- **Symptom**: Nearby count displayed `(2)`, but only 1 card (`Secret Tomato`) was rendered on screen.
- **Root Cause**: In Cascades Qt 4.8.5, an inlined `ComponentDefinition` in QML without an external source file does not instantiate unique controls on `createObject()`; it reused and mutated the same inline node, so the second device overwrote the first.
- **Fix**: Replaced dynamic QML instantiation with native C++ `UiController::populatePeers(QObject *containerObj)`. Synchronously creates `bb::cascades::Container`, `Label`, `Button` and attaches to the layout. Renders arbitrary N peer cards reliably.

### B. Subnet Active Sweep HTTPS Fallback
- **Symptom**: macOS peer wasn't discovered during active scan until restarted.
- **Root Cause**: macOS client uses HTTPS by default on port 53317. Plaintext HTTP probe failed with closed socket. In addition, `192.168.3.161` was outside the previous `delta <= 15` limit.
- **Fix**: Expanded scan neighborhood delta to `±40` (up to 80 hosts), and added HTTPS register probe fallback (`probeHttpsHost()`) using `reply->ignoreSslErrors()`.

### C. Launch Crash / Return to Home Screen (R014 regression)
- **Symptom**: Newly packaged BAR exits immediately upon launch and returns to desktop.
- **Root Cause**: Cascades QML namespace does not contain the `Connections` element. When present in `assets/main.qml`, `createRootObject()` returned `NULL`, causing `main()` to exit with return code 1.
- **Fix**: Replaced `Connections` with native Cascades declarative property bindings on `peersContainer` (`property int peersRevision: app.peersRevision`, `onPeersRevisionChanged`, etc.).

---

## 4. Verification Commands

### Deploy and Launch on BlackBerry Passport
```bash
./scripts/deploy-bb10.sh --device 192.168.3.124 --password '<device-password>' --launch
```

### Rebuild Package (if further modifications needed)
```bash
./scripts/build-bb10.sh
```

### Run Host Test Suite
```bash
podman run --rm -v "$(pwd):/work:Z" -w /work/tests localhost/localsend-dev bash -c "make && ./core-check"
```
