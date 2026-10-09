# Build, Deployment, and Acceptance

English | [Simplified Chinese](build-deploy.zh-CN.md)

Only planning documents exist. No BB10 NDK was found in common locations, and no BB10 build, BAR deployment, device test, or official-client interoperability test has run. Implementers must fill the tables with actual results.

## Environment Record (T00)

| Item | Current value |
| --- | --- |
| Host OS/CPU | Fedora Linux (x86_64) |
| BB10 NDK version/path | `~/bbndk` (host_10_3_1_12, target_10_3_1_995) + containerized `bb10-toolchain:latest` |
| Actual qcc/qmake/make/package-tool paths | `bb10-toolchain:latest` (qcc, qmake, blackberry-nativepackager 1.11, blackberry-deploy 1.5.0) |
| QNX_TARGET/QNX_HOST/Qt version | Target: BB10 Qt 4.8.5 (ARMv7le, Cascades) |
| Device model/BB10 OS build | BlackBerry 10 device (IP 192.168.3.124) |
| Deployment method/development mode | Development Mode over Wi-Fi (`blackberry-deploy`) verified |
| PC OS/official LocalSend release | Fedora Linux, LocalSend v1.18.2 |
| Specification commit | 62bd3406ec80d62f2ed46269cdc06c4dcc391083, v2.2 |
| Official code reference commit | c1ce322fb3acf08e44f329b8b7208b8b0d91e244 |
| LAN/interface/router restrictions | Same IPv4 LAN (192.168.3.0/24); UDP multicast 53317 and TCP 53317 |
| Bidirectional HTTP test settings | PC encryption disabled; ordinary Upload API transfer mode |

Validate the toolchain and deployment method first. Archive paths from other projects do not establish locally installed tools. Keep keys, passwords, and device credentials out of the repository and public logs.

## Build and Packaging Design

The project uses `localsend-bb10.pro` and `bar-descriptor.xml` at the repository root, with cross-compilation and packaging automated via `scripts/build-bb10.sh` and deployment via `scripts/deploy-bb10.sh`.

### Verified qmake Configuration (`localsend-bb10.pro`)

```qmake
APP_NAME = localsend-bb10
TARGET = localsend-bb10
TEMPLATE = app

INCLUDEPATH += src
DEPENDPATH += src

# BlackBerry 10 Cascades configuration
contains(CONFIG, cascades10) | qnx {
    CONFIG += qt warn_on cascades10
    QT += core network declarative
    LIBS += -lbbdata -lbbcascadespickers -lcrypto
} else {
    # Host testing / verification configuration
    CONFIG += qt warn_on console
    QT += core network
    LIBS += -lcrypto
}
```

### Application Descriptor (`bar-descriptor.xml`)
- Executable entry asset: `<asset path="%CPU/o/localsend-bb10" entry="true" type="Qnx/Elf">localsend-bb10</asset>`
- Cascades entry point: `<cascades><entryPoint><name>localsend-bb10</name></entryPoint></cascades>`
- System permissions: `run_native` (system="true"), `access_internet`, `access_shared`
- Assets packaged: `assets/main.qml`, `assets/icon.png`
- Library search path: `LD_LIBRARY_PATH="app/native/lib:/usr/lib/qt4/lib"`

### Reproducible Build & Deployment Commands

```bash
# 1. Cross-compile and package BAR for BlackBerry 10 ARMv7le:
./scripts/build-bb10.sh

# 2. Deploy to BlackBerry Passport via blackberry-deploy (Development Mode):
./scripts/deploy-bb10.sh --device 192.168.1.150 --password <device-password>

# 3. Alternative deploy to pre-rooted device via SSH/sideload:
./scripts/deploy-bb10.sh --device 192.168.1.150 --method ssh

# 4. Interoperability & LAN Test Rig (Python 3 stdlib):
# Generate test fixtures (0 B, 1 KiB, 10 MiB, Chinese name, SHA-256 manifest):
python3 tests/interoperability.py fixtures --output /tmp/localsend-bb10-fixtures

# Verify running device HTTP endpoints:
python3 tests/interoperability.py check-http --host 192.168.1.150 --port 53317

# Run mock LocalSend PC peer on LAN:
python3 tests/interoperability.py serve-peer --bind 0.0.0.0 --port 53317

# Send file from PC to BB10 device:
python3 tests/interoperability.py send --target 192.168.1.150 --port 53317 --file /tmp/localsend-bb10-fixtures/10m.bin
```

## Fixtures and Evidence

- Fix 10 MiB at 10,485,760 bytes and 100 MiB at 104,857,600 bytes. Use these exact counts for the requested 10MB acceptance target.
- Generate deterministic binary data, NUL/non-UTF-8 bytes, 0 B/1 KiB/10 MiB/100 MiB files, and Chinese/space-containing names with Python stdlib. Keep large fixtures in /tmp, outside Git.
- Independently compute file SHA-256 on PC, for example with hashlib/sha256sum, and use verified target-side hashing. Reading files back or transferring them back allows independent comparison; matching size/name alone is insufficient.
- Host checks cover JSON, TCP fragmentation, statuses, and state. Device checks cover platform APIs, resources/permissions, and LAN behavior. The official PC client establishes interoperability. Record these results separately.
- Use PASS/FAIL/NOT RUN. Each PASS includes versions, commands/manual steps, and actual output or hashes. Failures include a minimal reproduction. Do not record tokens or complete URLs containing token queries.
- This release supports foreground operation. Background/lock-screen transfers are not guaranteed; detect invalid state and clean up on return. Foreground results do not establish background-service support.

## Acceptance Matrix

Every item starts as NOT RUN. P is the mandatory protocol MVP; U adds the everyday UI. Both sending and receiving are hard gates for P.

| ID | Milestone | Procedure | Required passing result | Current result |
| --- | --- | --- | --- | --- |
| A00 | P | Build/start probe with target SDK | All Qt/API/crypto/file probes pass; commands reproducible | PASS (bb10-toolchain container: liblocalsend-bb10.so ARMv7le built, localsend-bb10.bar packaged, SHA-256: 506913edb8eb78ba8b56ed3be454d0d6cda4f1c44ed3f17f02b7aedf6b9dab38) |
| A01 | P | Start PC before BB, then reverse order | Peers discover each other once within 10 s; no self-discovery | Host PASS (core-check: self-discovery ignored); Target pending |
| A02 | P | Repeat announcements/register 100 times and announce:false | No duplicate peers/response loops; observed IP/port correct | Host PASS (core-check: announce:false, single row deduplication); Target pending |
| A03 | P | Disable multicast, specify PC IPv4/port | Manual register discovers peer and HTTP transfers work | Host PASS (core-check: registerPeer HTTP manual register); Target pending |
| A04 | P | Official PC to BB, 10 MiB | PC final success; BB final file 10,485,760 B; SHA-256 matches | NOT RUN |
| A05 | P | BB to official PC, 10 MiB | BB receives final 200; PC final success; SHA-256 matches | NOT RUN |
| A06 | P | Bidirectional 0 B and Chinese/space-containing names | Correct data/names; zero-byte prepare is not mistaken for 204 | NOT RUN |
| A07 | P | Receive same name twice | Both contents preserved without overwrite; actual paths correct | NOT RUN |
| A08 | P | PC offers multiple files; accept second | Prepare returns only selected token; others are not saved | NOT RUN |
| A09 | P | Reject/second session/401/429/500/204 fixtures | Correct statuses; 204 does not parse JSON or upload | Host PASS (core-check: 403 reject, 409 busy, 422 checksum mismatch, 500 error, 204 skip); Target pending |
| A10 | P | Wrong token/source IP, unselected fileId, missing query | Rejected without damaging active session/saved files | NOT RUN |
| A11 | P | Raw fragmented requests, chunked/trailers, Expect 100 | Valid requests pass; malformed requests fail within bounds without deadlock | Host PASS (core-check: 1B fragmentation, chunked trailers, Expect 100, CL+TE 400, header 431); Target pending |
| A12 | P | Correct/incorrect declared checksum | Correct succeeds; incorrect returns 422 without final file | NOT RUN |
| A13 | P | Actual size larger/smaller than declared; abort body | No success; temporary file/slot cleaned | NOT RUN |
| A14 | P | Both endpoints cancel during consent/upload wait/transfer | Local IO stops within 2 s; next 10 MiB transfer succeeds | Host PASS (core-check: upload cancellation, remote notification, slot release); Target pending |
| A15 | P | No progress for 30 s; continuous slow progress beyond 30 s | Stalled upload times out; progressing upload does not | NOT RUN |
| A16 | P | Wi-Fi loss/exit, then recovery or restart | No invalid final file; only own temporary remnants cleaned; transfers resume | NOT RUN |
| A17 | P | Traversal, absolute paths, symlinks, write/rename failures | No path escape, overwrite, or false success | NOT RUN |
| A18 | P | Compare 10 MiB/100 MiB peak RSS; poll register | Bounded buffers; file-size-dependent extra RSS target <8 MiB; control remains responsive | NOT RUN |
| A19 | P | PC encryption enabled; HTTPS announcement | HTTPS unsupported is visible; no plaintext requests to that TLS endpoint | Host PASS (core-check: sendFile rejects HTTPS peer; UDP responder fallback); Target pending |
| A20 | U | Phone picker, select PC, send | Complete A05 without developer commands; picker cancellation sends nothing | NOT RUN |
| A21 | U | Phone candidate selection, accept/reject/cancel | Complete A04/A08/A14 without test configuration | NOT RUN |
| A22 | U | Permission denial/offline peers/screen adaptation | Clear errors/actual saved location; responsive UI | NOT RUN |

## agy Delivery Format

```text
Task: Txx
Changes: files and final behavior
Verification: commands/manual steps, actual results, evidence paths
Unverified: missing SDK/device/PC checks, never counted as PASS
Limitations: changed release decisions; synchronize protocol and plan if needed
Next task: whether dependencies are satisfied
```

Deliver P only when every applicable A00-A19 item has passing evidence; add A20-A22 for U. No core matrix item is currently optional. If a test needs another source IP/device or additional conditions, arrange the environment or mark NOT RUN; do not silently declare it inapplicable. Deferring UI never relaxes the mandatory bidirectional transfers.
