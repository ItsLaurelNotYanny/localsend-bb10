# LocalSend-Style P2P TLS 1.3 Transfer Demo

This example demonstrates peer-to-peer (P2P) binary file transfer using negotiated TLS 1.3 with certificate fingerprint pinning, consuming the external `posix-tls` SDK.

## Overview

The demo models LocalSend's encrypted transport architecture:
- **P2P Trust & Pinning**: Uses self-signed EC P-256 certificates. Peer authentication relies strictly on SHA-256 fingerprint pinning of the leaf certificate DER representation rather than a public CA hierarchy.
- **TLS 1.3 Negotiation**: Enforces TLS 1.3 protocol version over standard loopback/network TCP sockets.
- **Binary Stream Framing**: Transfers raw binary payload with SHA-256 integrity check and server acknowledgement.
- **Strict Pin Mismatch Protection**: If a certificate fingerprint does not match the pinned expectation (even by a single hex digit), `posix_tls_connect` rejects the connection during handshake with `POSIX_TLS_ERR_PIN_MISMATCH` before transmitting any application payload.
- **Clean Disconnect & Timeout Handling**: Incomplete transfers or timeouts cleanly discard temporary data without committing partial files.

## Prerequisites

- CMake 3.16 or newer.
- C++98 / C++03 compliant compiler (e.g. GCC, Clang, or BB10 Clang/QCC).
- `posix-tls` installed SDK build:
  - Host: `../../posix-tls/build/sdk-host` (or custom `-DPOSIX_TLS_SDK_DIR=<path>`)
  - QNX ARMv7: `../../posix-tls/build/sdk-qnx`

## Building

### Host (Linux)

```bash
bash scripts/build_host.sh
```

Or manually:

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

### BlackBerry 10 / QNX ARMv7 (Cross-Compilation)

Using the hermetic `bb10-native-dev:local` container:

```bash
bash scripts/build_qnx.sh
```

The resulting ARM binary (`build-qnx/localsend_style_demo`) is a 32-bit ARM ELF executable linked with `-Wl,--exclude-libs,ALL` to ensure zero internal OpenSSL symbols leak into `.dynsym`.

## Usage

### 1. Generate Identity & Inspect Pin

Generate a new self-signed EC P-256 certificate and private key, and output the certificate fingerprint:

```bash
./build/localsend_style_demo generate-identity \
    --cert cert.pem \
    --key key.pem
```

Output:
```text
FINGERPRINT=b4672b77f16e47ca4d7ccb1c8a2a1b581bdd18ed545bb9abd7a87dc9b4a02f70
```

To calculate the fingerprint of an existing certificate:

```bash
./build/localsend_style_demo fingerprint --cert cert.pem
```

### 2. Start Receiver (Server)

```bash
./build/localsend_style_demo server \
    --port 8443 \
    --cert cert.pem \
    --key key.pem \
    --output received_file.bin \
    --min-tls-version TLSv1.3
```

### 3. Send File (Client)

```bash
./build/localsend_style_demo client \
    --host 127.0.0.1 \
    --port 8443 \
    --file sample.bin \
    --expected-pin <SERVER_FINGERPRINT> \
    --min-tls-version TLSv1.3
```

## Running Automated Verification

Run the comprehensive 7-case test suite covering:
1. Self-signed identity generation, owner-only mode 0600 on keys, `/dev/full` failure rejection, existing identity preservation on replacement failure, and independent OpenSSL leaf DER SHA-256 fingerprint verification.
2. Real loopback TCP transfer under TLS 1.3 with SHA-256 payload integrity check.
3. Single-hex-digit fingerprint tampering rejection (`POSIX_TLS_ERR_PIN_MISMATCH`).
4. Robustness & edge cases: delayed header handling within deadline, bounded ACK wait timeout, fragmented ACK frame assembly, wrong-digest ACK rejection, mid-payload disconnect rejection with temp file cleanup, checksum mismatch rejection with temp cleanup, temporary file isolation (symlink protection via `mkstemp`), and clean recovery transfer.
5. Socket descriptor ownership verification via `strace` (verifying 0 `EBADF` double-close calls).
6. Fault injection and backpressure: injected `fsync`/`close` aborts without committing, injected `rename` failure preserves preexisting destination symlink and victim data, injected ACK socket `EPIPE` failure reports non-zero exit (`TRANSFER_COMMITTED_BUT_ACK_FAILED`) and suppresses `TRANSFER_OK` while preserving committed file, and forced write backpressure (`SO_SNDBUF=4096`) triggers nonblocking `EAGAIN` retry with successful transfer and digest match.
7. Performance benchmark over 5 iterations per payload, host CPU/RAM and RSS memory reporting, ELF section breakdown, and explicit demarcation of unmeasured device metrics.

```bash
python3 tests/test_localsend_style.py
```

Or run the full example gate (host build + tests + QNX cross-compile + mandatory symbol isolation):

```bash
./check.sh
```
