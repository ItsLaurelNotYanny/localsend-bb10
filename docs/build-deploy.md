# Build, Deployment, and Acceptance

English | [Simplified Chinese](build-deploy.zh-CN.md)

Only planning documents exist. No BB10 NDK was found in common locations, and no BB10 build, BAR deployment, device test, or official-client interoperability test has run. Implementers must fill the tables with actual results.

## Environment Record (T00)

| Item | Current value |
| --- | --- |
| Host OS/CPU | Ubuntu 24.04 LTS (x86_64, headless Linux) |
| BB10 NDK version/path | Pending local install; pseudo-environment Docker container (localsend-dev) configured |
| Actual qcc/qmake/make/package-tool paths | Host: /usr/bin/gcc, /usr/bin/make; Container: /usr/bin/qmake |
| QNX_TARGET/QNX_HOST/Qt version | Container: Qt 5.15.8 (Qt4-compatible codebase); Target: BB10 Qt 4.8.5 |
| Device model/BB10 OS build | BlackBerry Passport (SQW100-1), Oleksandr Pre-rooted BB10 OS v3 (10.3.3.3216) |
| Deployment method/development mode | Rooted device; unsigned sideload / direct installation via bb10mt or SSH |
| PC OS/official LocalSend release | Fedora Linux, LocalSend v1.18.2 |
| Specification commit | 62bd3406ec80d62f2ed46269cdc06c4dcc391083, v2.2 |
| Official code reference commit | c1ce322fb3acf08e44f329b8b7208b8b0d91e244 |
| LAN/interface/router restrictions | Same IPv4 LAN; UDP multicast and TCP 53317 allowed |
| Bidirectional HTTP test settings | PC encryption disabled; ordinary Upload API transfer mode |

Validate the toolchain and deployment method first. Archive paths from other projects do not establish locally installed tools. Keep keys, passwords, and device credentials out of the repository and public logs.

## Build Entry Design

Use localsend-bb10.pro and bar-descriptor.xml at the root; generated files belong in ignored build/. This qmake fragment indicates the intended configuration; **it is not a verified runnable project**:

```qmake
APP_NAME = localsend-bb10
CONFIG += qt warn_on cascades10
QT += core network declarative
LIBS += -lbbdata
# Link SDK libcrypto for SHA-256/RAND_bytes after T00 verifies availability.
# List SOURCES/HEADERS; avoid Qt5 modules and IDE-specific absolute paths.
```

If protocol validation starts with an independent QtCore executable, use separate probe/core-check targets. The Cascades application entry uses bb::cascades::Application; do not reuse QCoreApplication as the final UI entry. Refer to the [official JSON project](https://github.com/blackberry/Cascades-Samples/tree/master/jsonreadwrite), whose config.pri links bbdata. T00 must verify SDK settings, release entry point, architecture, and asset paths instead of blindly copying older samples.

Verify run_native and access_internet in the descriptor. Selecting/writing shared files requires access_shared. Package assets/main.qml, the icon, and every dependent asset. Do not report successful shared-directory saving when permissions are missing. Private `data/received` is the default operational destination; the daily UI displays the actual location and offers shared storage when available.

After T00, record actual reproducible commands for environment initialization, shadow build, ARMv7 compilation, packaging, the working install/launch method, and log retrieval. Do not present blackberry-deploy, debug token, or signing workflows as operational before verification. Record the tools and credential method that actually work.

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
| A00 | P | Build/start probe with target SDK | All Qt/API/crypto/file probes pass; commands reproducible | NOT RUN |
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
