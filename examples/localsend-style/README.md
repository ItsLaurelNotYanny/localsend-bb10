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

Run the complete 5-case test suite covering:
1. Self-signed identity generation and independent verification against OpenSSL leaf DER SHA-256.
2. Real loopback TCP transfer under TLS 1.3 with SHA-256 payload integrity check.
3. Single-hex-digit fingerprint tampering rejection (`POSIX_TLS_ERR_PIN_MISMATCH`).
4. Partial writes, mid-stream peer disconnect, deadline timeouts, and clean recovery.
5. Performance and throughput measurements for 1 MiB and 10 MiB payloads.

```bash
python3 tests/test_localsend_style.py
```

Or run the full example gate (host build + tests + QNX cross-compile + symbol isolation):

```bash
./check.sh
```
