# Discovery Diagnosis Handoff for agy

English | [Simplified Chinese](discovery-debug-handoff.zh-CN.md)

Author: Codex. Date: 2026-10-12. Next action owner: agy. This is a diagnostic recommendation, not a device-verified fix or an issue closure.

## Reviewed Source and Corrected Assessment

The working checkout was `7483d3b`. A remote-tip check and fetch found eight newer commits; this handoff reviews `5a1fef174c8b51036a62c63cdaf1557e2513259f` on `feature/bb10-discovery-ui-review`, extracted into an isolated directory. The working checkout was not switched. Earlier comments about JavaScript peer rendering, 32.8-second scanning, terminal-slot notification, flush handling, native packaging and SSH exit status concerned the older snapshot. Do not reapply those recommendations without checking their current status.

The latest source uses native C++ peer cards, a preferred-interface selector, 600 ms HTTP probe deadlines, an 8-second scan deadline, unicast UDP sweeps, HTTPS probe fallback and an on-device log. The user reports that multiple cards now render; the remaining symptom is that an already-running second PC usually appears only after restarting its LocalSend client. This is primarily a discovery diagnosis task. Keep the current card implementation while locating the first failed stage.

## Evidence and Hypotheses

- The latest isolated Linux/Qt5 suite passed **60/60**. This does not test actual BB10 UDP transmission or PC multicast reception.
- For the documented `192.168.3.124/24` topology, current queue construction yields **88 targets**, with `.161` at one-based position **74**. The comment claiming a maximum of 80 does not enforce a cap on the preceding neighbor/common-host stages.
- Running the actual latest `pumpScanQueue()` with synthetic replies that only expire at the configured timeout issued `.161` after **3,701 ms** and completed all 88 after **4,901 ms**. No LAN traffic occurred. This does not establish device timing, but queue order and 600 ms deadlines alone do not explain missing `.161` before the 8-second cutoff.
- `docs/session-handoff.md` asserts QNX 6.5, an approximately 8-9 KiB send buffer and ENOBUFS around packet 30. Neither the reviewed code nor checked-in evidence provides the OS/socket measurement, failed send return values or packet trace needed to establish that root cause. Treat buffer/ARP pressure as a plausible hypothesis.
- Current code ignores every UDP send result, multicast join results and the discovery startup return value. The message `UDP announcement sent` therefore proves only that a code branch ran. Selecting a primary interface for scanning does not select the multicast socket's outgoing interface.
- Restarting the PC reinitializes its sockets and triggers discovery. The PC could reach BB10 through its announcement or a register request. This observation alone does not identify which path succeeded or which earlier path failed.

## First Deliverable: One Diagnostic Commit

Preserve normal behavior and add targeted diagnostics before tuning buffers, retry counts or worker counts. If needed, add development-only controls to pause subnet scanning and run each of the single-target experiments below separately. Keep diagnostic controls out of the ordinary product flow.

Record these events in English, with timestamps and a local scan/request identifier; do not add diagnostic fields to LocalSend JSON:

| Stage | Required record |
| --- | --- |
| Startup | Source commit, build timestamp, actual Qt/QNX versions, running version, selected interface/IP/netmask, HTTP listen port, UDP bind/join outcome |
| UDP transmit | Trigger (startup/Scan/heartbeat/sweep/manual), destination, intended interface, queue index, payload length, `writeDatagram` return value; socket error/code/message on failure |
| UDP receive | Sender address/port, decode success or rejection reason, announce flag, fingerprint, advertised protocol/TCP port |
| HTTP register | Incoming versus outgoing, trigger, endpoint, elapsed time, HTTP status/network error and decode outcome |
| Peer store | Upsert/removal with fingerprint, endpoint and reason; resulting fingerprint list and count |
| UI | Model revision/count and successfully created/added card count |
| Scan end | Elapsed time, issued/remaining requests, completion versus whole-scan cancellation |

Read actual `SO_SNDBUF` with `getsockopt` on the BB10 socket if testing the buffer hypothesis. Capture native errors at the failing operation if available; do not infer ENOBUFS or an OS version from a generic timeout. A successful UDP send return means the local API accepted the datagram, not that the PC received it.

## Controlled Experiments

Use the two existing PCs, same LAN, and unchanged firewall/router configuration. Record each actual PC LocalSend version and encryption setting. The documented `.161` PC has encryption disabled; begin with that HTTP baseline. Synchronize clocks or record explicit action markers. Do not transfer files during discovery capture.

On the affected PC, choose its actual LAN capture interface; on Fedora identify it with `ip route get 192.168.3.124`, on macOS with `route -n get 192.168.3.124`. Then run:

```bash
DISCOVERY_EVIDENCE_DIR="$(mktemp -d /tmp/localsend-discovery.XXXXXX)"
PC_CAPTURE_IFACE="replace-with-actual-LAN-interface"
sudo tcpdump -ni "$PC_CAPTURE_IFACE" -s 0 \
    -w "$DISCOVERY_EVIDENCE_DIR/pc-discovery.pcap" \
    'udp port 53317 or tcp port 53317'
```

Change the filter if either device uses a nondefault port. Preserve the BB10 diagnostic log and the PC LocalSend logs alongside the trace. Verify the deployed artifact by recording the BAR hash and matching runtime build identity. Build 6 has several recorded hashes, so the version label alone is insufficient.

1. **Original failure, then PC restart:** Keep both PC clients running before starting BB10. Capture the first 10 seconds and a normal Scan once it is available. Record if Scan is disabled. When the second PC is missing, restart only that PC's LocalSend inside the same capture, with an action timestamp. Identify the exact first event that adds it to BB10. Preserve this baseline before manual registration changes peer state.
2. **HTTP-only target control:** In a separate run, with the affected PC continuously running and scan/UDP controls paused, send exactly one BB10 HTTP `/register` request to the affected IP and actual TCP port. Require a 200 response, valid decoding and the expected fingerprint. Current `registerPeer()` sends both UDP and HTTP, so its success cannot establish which transport worked; do not use the existing mixed action as an HTTP-only control.
3. **UDP-only target control:** In a separate run, pause the HTTP sweep and send only a valid `announce:true` UDP datagram to the affected PC's discovery port. Try a small timed series, such as at 0, 250 and 1,000 ms, without an 88-host burst. Record each send result, PC receipt and any PC-to-BB10 `/register`. Derive the actual UDP port from the PC configuration; an advertised TCP port does not by itself establish the UDP port.
4. **Burst versus paced control:** If the single-target test works, compare the existing sweep with a paced sweep using the same targets and capture conditions. Compare attempted/local-accepted/PC-received datagrams and HTTP callbacks, especially for `.161`. Use repeated runs; one successful run after cache priming is insufficient.

## Choose the Smallest Repair from the Evidence

| Observed break | Next action |
| --- | --- |
| Intended target absent from queue | Correct address/interface/range selection or exclusion; log the effective cap. Do not tune sockets for a target never scheduled. |
| UDP send fails during a burst, isolated target succeeds | Use a bounded QTimer queue with paced batches and limited retries for confirmed transient errors. Cancel/stop clears queued sends and timers. Recheck buffer/ARP pressure on device. |
| UDP locally accepted, affected PC sees no packet | Check actual route/interface and packet delivery, including ARP/network filtering. Compare unicast and multicast captures. Increasing the buffer alone is not justified. |
| PC receives valid announcement but sends no register | Inspect PC discovery logs, listener/server state and network settings. Do not keep changing BB10 UI. |
| PC sends register, BB10 does not receive it | Check destination IP/port, HTTP listener and inbound path; distinguish TCP failure from decoder rejection. |
| BB10 accepts register but peer disappears | Trace removal events and pending requests. Avoid erasing a fresh peer solely because one earlier register attempt failed. |
| Peer store/model has two entries, only one card appears | Return to native card population/layout diagnosis with actual created/added counts. |
| `.161` probe starts late only on BB10 | Measure real timer/event-loop delays and in-flight lifetimes before changing scan concurrency or deadlines. |

For a confirmed burst failure, a reasonable initial experiment is 1-2 UDP datagrams per 20 ms timer tick, with at most three attempts per target and a whole-operation budget. Treat those values as experimental settings, not proven QNX limits. The deadline must include queued retries and permit callbacks to arrive. Do not retry every error blindly. Prefer remembered successfully discovered endpoints for fast rechecks, without counting remembered/manual targets as proof of first-time discovery.

Explicitly select the intended multicast outgoing interface using the Qt4-supported `setMulticastInterface` when the route evidence warrants it; use the same selected LAN context for scan and status. Check bind/join/write failures and distinguish discovery degradation from core HTTP availability. Keep HTTP-only MVP transfer policy explicit; HTTPS discovery-only behavior is a separate case. The added HTTPS probe/ignored certificate errors are not a substitute for proving the plaintext baseline.

## Device Acceptance and agy Return Format

After the targeted repair, run at least 10 BB10 cold starts and 10 available Scan actions with both PC clients already running. Do not restart either PC or manually prime the missing peer. Repeat with the affected PC joining later. On this test LAN, both peers must appear within the agreed 10 seconds, remain distinct by fingerprint, render as two cards and support an actual send to the second HTTP peer. Count startup/Scan misses, latency per fingerprint and temporary appearance/removal. This is a bounded reproducibility check, not a universal network guarantee.

Return: exact source commit and BAR/runtime identity; baseline failing trace; the first failed stage; matched repair; before/after device runs; host checks covering the repaired behavior; unresolved limitations. Keep R018/R019 open until this scenario passes. A passing host count or a reboot-assisted success does not close it.

## Reference Sources

- [LocalSend pinned v2.2 discovery protocol](https://github.com/localsend/protocol/blob/62bd3406ec80d62f2ed46269cdc06c4dcc391083/README.md#3-discovery): announcement and register semantics.
- [Official v1.18.2 multicast socket implementation](https://github.com/localsend/localsend/blob/v1.18.2/packages/core/src/multicast/socket.rs): separates group membership from outgoing-interface selection.
- [Qt4.8 UDP API](https://github.com/qt/qt/blob/4.8/src/network/socket/qudpsocket.h): provides `setMulticastInterface`.

Review artifacts in this session: `/tmp/localsend-agy-latest-review-4dxirzi1/review-build/{queue-probe.log,core-check.log}`. Temporary paths are local evidence, not portable SDK/device records. No device connection or PC packet capture was performed by Codex.
