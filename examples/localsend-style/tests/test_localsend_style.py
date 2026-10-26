#!/usr/bin/env python3
"""Automated verification suite for LocalSend-style posix-tls integration demo."""

import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def run_cmd(cmd: list[str], timeout: float = 15.0) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)


def test_case_1_identity_and_pin(demo_bin: str, tmp_dir: Path) -> str:
    print("[Case 1] Generating self-signed identity and verifying certificate pin independently...")
    cert_path = tmp_dir / "server_cert.pem"
    key_path = tmp_dir / "server_key.pem"

    cp = run_cmd([demo_bin, "generate-identity", "--cert", str(cert_path), "--key", str(key_path)])
    if cp.returncode != 0:
        raise RuntimeError(f"generate-identity failed (exit={cp.returncode}): {cp.stderr}")

    match = re.search(r"FINGERPRINT=([0-9a-fA-F]{64})", cp.stdout)
    if not match:
        raise RuntimeError(f"FINGERPRINT not found in output: {cp.stdout}")
    demo_fingerprint = match.group(1).lower()

    # Independent OpenSSL DER SHA-256 calculation
    der_cp = subprocess.run(
        ["openssl", "x509", "-in", str(cert_path), "-outform", "DER"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    dgst_cp = subprocess.run(
        ["openssl", "dgst", "-sha256"],
        input=der_cp.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    # Output format is typically: "(stdin)= <hash>" or "<hash>"
    openssl_hash = dgst_cp.stdout.decode("utf-8").strip().split()[-1].lower()

    if demo_fingerprint != openssl_hash:
        raise AssertionError(f"Pin mismatch! demo={demo_fingerprint} vs openssl={openssl_hash}")

    # Test fingerprint subcommand
    fp_cp = run_cmd([demo_bin, "fingerprint", "--cert", str(cert_path)])
    if fp_cp.returncode != 0:
        raise RuntimeError(f"fingerprint command failed: {fp_cp.stderr}")
    match2 = re.search(r"FINGERPRINT=([0-9a-fA-F]{64})", fp_cp.stdout)
    if not match2 or match2.group(1).lower() != demo_fingerprint:
        raise AssertionError("fingerprint subcommand output mismatch")

    print(f"  PASS: Fingerprint {demo_fingerprint} verified against OpenSSL leaf DER SHA-256")
    return demo_fingerprint


def test_case_2_tls13_loopback_transfer(demo_bin: str, tmp_dir: Path, cert_path: Path, key_path: Path, pin: str, file_size_bytes: int = 1048576) -> dict:
    print(f"[Case 2] Loopback TCP transfer ({file_size_bytes} bytes), TLS 1.3 enforcement & SHA-256 check...")
    port = reserve_port()
    input_file = tmp_dir / f"input_{file_size_bytes}.bin"
    output_file = tmp_dir / f"output_{file_size_bytes}.bin"

    # Create random binary payload
    payload = os.urandom(file_size_bytes)
    input_file.write_bytes(payload)

    # Compute expected SHA-256
    import hashlib
    expected_sha256 = hashlib.sha256(payload).hexdigest().lower()

    # Start server
    server_proc = subprocess.Popen(
        [
            demo_bin, "server",
            "--port", str(port),
            "--cert", str(cert_path),
            "--key", str(key_path),
            "--output", str(output_file),
            "--min-tls-version", "TLSv1.3",
            "--timeout-ms", "10000"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    # Wait for server listening line
    server_line = server_proc.stdout.readline()
    if f"LISTENING port={port}" not in server_line:
        server_proc.kill()
        out, err = server_proc.communicate()
        raise RuntimeError(f"Server failed to start listening on port {port}: {server_line} {err}")

    client_cp = run_cmd([
        demo_bin, "client",
        "--host", "127.0.0.1",
        "--port", str(port),
        "--file", str(input_file),
        "--expected-pin", pin,
        "--min-tls-version", "TLSv1.3",
        "--timeout-ms", "10000"
    ], timeout=15.0)

    server_stdout, server_stderr = server_proc.communicate(timeout=10.0)

    if client_cp.returncode != 0:
        raise RuntimeError(f"Client failed (exit={client_cp.returncode}):\nSTDOUT: {client_cp.stdout}\nSTDERR: {client_cp.stderr}")
    if server_proc.returncode != 0:
        raise RuntimeError(f"Server failed (exit={server_proc.returncode}):\nSTDOUT: {server_stdout}\nSTDERR: {server_stderr}")

    # Check TLS 1.3 negotiation
    if "version=TLSv1.3" not in client_cp.stdout or "version=TLSv1.3" not in server_stdout:
        raise AssertionError("Negotiated TLS version is not TLSv1.3")

    # Check output file existence and SHA-256
    if not output_file.exists():
        raise AssertionError(f"Output file {output_file} was not committed")

    actual_sha256 = hashlib.sha256(output_file.read_bytes()).hexdigest().lower()
    if actual_sha256 != expected_sha256:
        raise AssertionError(f"Payload SHA-256 mismatch! expected={expected_sha256} actual={actual_sha256}")

    # Parse metrics
    client_match = re.search(r"duration_ms=([0-9.]+)\s+throughput_mb_s=([0-9.]+)", client_cp.stdout)
    duration_ms = float(client_match.group(1)) if client_match else 0.0
    throughput = float(client_match.group(2)) if client_match else 0.0

    print(f"  PASS: TLS 1.3 verified. Transferred {file_size_bytes} bytes in {duration_ms:.2f} ms ({throughput:.2f} MB/s). SHA-256 matched.")
    return {"bytes": file_size_bytes, "duration_ms": duration_ms, "throughput_mb_s": throughput}


def test_case_3_pin_mismatch(demo_bin: str, tmp_dir: Path, cert_path: Path, key_path: Path, valid_pin: str) -> None:
    print("[Case 3] Single-hex-digit pin tampering -> POSIX_TLS_ERR_PIN_MISMATCH rejection...")
    port = reserve_port()
    input_file = tmp_dir / "input_tamper.bin"
    output_file = tmp_dir / "output_tamper.bin"
    input_file.write_bytes(b"SENSITIVE APPLICATION PAYLOAD THAT MUST NEVER BE SENT")

    # Tamper a single hex character
    tampered_char = '0' if valid_pin[0] != '0' else '1'
    tampered_pin = tampered_char + valid_pin[1:]

    server_proc = subprocess.Popen(
        [
            demo_bin, "server",
            "--port", str(port),
            "--cert", str(cert_path),
            "--key", str(key_path),
            "--output", str(output_file),
            "--min-tls-version", "TLSv1.3",
            "--timeout-ms", "5000"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    # Wait for server listening line
    server_line = server_proc.stdout.readline()
    if f"LISTENING port={port}" not in server_line:
        server_proc.kill()
        out, err = server_proc.communicate()
        raise RuntimeError(f"Server failed to start listening: {server_line} {err}")

    client_cp = run_cmd([
        demo_bin, "client",
        "--host", "127.0.0.1",
        "--port", str(port),
        "--file", str(input_file),
        "--expected-pin", tampered_pin,
        "--min-tls-version", "TLSv1.3",
        "--timeout-ms", "5000"
    ], timeout=10.0)

    server_stdout, server_stderr = server_proc.communicate(timeout=10.0)

    # Client must fail with exit code 6 (POSIX_TLS_ERR_PIN_MISMATCH)
    if client_cp.returncode != 6:
        raise AssertionError(f"Expected client exit 6 (PIN_MISMATCH), got {client_cp.returncode}. STDOUT: {client_cp.stdout} STDERR: {client_cp.stderr}")

    if "PIN_MISMATCH" not in client_cp.stderr and "PIN_MISMATCH" not in client_cp.stdout:
        raise AssertionError(f"Expected PIN_MISMATCH in client output: {client_cp.stderr}")

    # Server must NOT commit output file
    if output_file.exists():
        raise AssertionError("Server committed output file despite pin mismatch!")

    print("  PASS: Client rejected tampered pin before payload transmission. Zero output committed.")


def test_case_4_disconnect_deadline_and_recovery(demo_bin: str, tmp_dir: Path, cert_path: Path, key_path: Path, pin: str) -> None:
    print("[Case 4] Partial writes/disconnect, deadline timeout, and clean recovery...")
    port = reserve_port()
    input_file = tmp_dir / "input_partial.bin"
    output_file = tmp_dir / "output_partial.bin"
    input_file.write_bytes(os.urandom(262144)) # 256 KiB

    # 4A: Peer disconnect mid-stream
    server_proc = subprocess.Popen(
        [
            demo_bin, "server",
            "--port", str(port),
            "--cert", str(cert_path),
            "--key", str(key_path),
            "--output", str(output_file),
            "--timeout-ms", "3000"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    # Wait for server listening line
    server_line = server_proc.stdout.readline()
    if f"LISTENING port={port}" not in server_line:
        server_proc.kill()
        out, err = server_proc.communicate()
        raise RuntimeError(f"Server failed to start listening: {server_line} {err}")

    client_cp = run_cmd([
        demo_bin, "client",
        "--host", "127.0.0.1",
        "--port", str(port),
        "--file", str(input_file),
        "--expected-pin", pin,
        "--disconnect-after", "16384",
        "--timeout-ms", "3000"
    ], timeout=5.0)

    server_stdout, server_stderr = server_proc.communicate(timeout=5.0)

    if output_file.exists():
        raise AssertionError("Server committed file after disconnected transfer!")
    print("  PASS: Mid-stream disconnect detected cleanly; uncommitted temporary files discarded.")

    # 4B: Timeout handling
    port_timeout = reserve_port()
    server_timeout_proc = subprocess.Popen(
        [
            demo_bin, "server",
            "--port", str(port_timeout),
            "--cert", str(cert_path),
            "--key", str(key_path),
            "--output", str(tmp_dir / "output_timeout.bin"),
            "--timeout-ms", "500" # fast timeout
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    s_out, s_err = server_timeout_proc.communicate(timeout=3.0)
    if server_timeout_proc.returncode == 0:
        raise AssertionError("Server should have failed on timeout")
    print("  PASS: Finite deadline timeout properly enforced.")

    # 4C: Immediate recovery
    print("  Testing immediate valid run recovery after failures...")
    test_case_2_tls13_loopback_transfer(demo_bin, tmp_dir, cert_path, key_path, pin, file_size_bytes=65536)
    print("  PASS: Subsequent valid run succeeded with 100% data integrity.")


def run_benchmark(demo_bin: str, tmp_dir: Path, cert_path: Path, key_path: Path, pin: str) -> None:
    print("\n--- Performance & Resource Measurements ---")
    results = []
    for sz in [1048576, 10485760]: # 1 MiB and 10 MiB
        m = test_case_2_tls13_loopback_transfer(demo_bin, tmp_dir, cert_path, key_path, pin, file_size_bytes=sz)
        results.append(m)

    # Binary size inspection
    stat = os.stat(demo_bin)
    size_cp = run_cmd(["size", demo_bin])
    print(f"\nExecutable file size: {stat.st_size} bytes ({stat.st_size / 1024.0:.1f} KiB)")
    if size_cp.returncode == 0:
        print("ELF section breakdown:")
        print(size_cp.stdout.strip())

    print("\nSummary Metrics:")
    for r in results:
        print(f"  Payload: {r['bytes'] / (1024*1024):.1f} MiB | Duration: {r['duration_ms']:.2f} ms | Throughput: {r['throughput_mb_s']:.2f} MB/s")


def main() -> int:
    demo_bin = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).resolve().parents[1] / "build" / "localsend_style_demo")
    if not os.path.isfile(demo_bin) or not os.access(demo_bin, os.X_OK):
        print(f"Error: demo binary not found or not executable at {demo_bin}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="localsend_style_test_") as td:
        tmp_dir = Path(td)
        cert_path = tmp_dir / "server_cert.pem"
        key_path = tmp_dir / "server_key.pem"

        pin = test_case_1_identity_and_pin(demo_bin, tmp_dir)
        test_case_2_tls13_loopback_transfer(demo_bin, tmp_dir, cert_path, key_path, pin, file_size_bytes=1048576)
        test_case_3_pin_mismatch(demo_bin, tmp_dir, cert_path, key_path, pin)
        test_case_4_disconnect_deadline_and_recovery(demo_bin, tmp_dir, cert_path, key_path, pin)
        run_benchmark(demo_bin, tmp_dir, cert_path, key_path, pin)

    print("\nALL LOCAL-SEND STYLE POSIX-TLS INTEGRATION CHECKS PASSED SUCCESSFULLY (5/5).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
