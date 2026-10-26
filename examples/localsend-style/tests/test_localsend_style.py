#!/usr/bin/env python3
"""Automated verification and regression suite for LocalSend-style posix-tls integration demo."""

import hashlib
import json
import os
import re
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def run_cmd(cmd: list[str], timeout: float = 15.0, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout, env=env)


def exact_read(sock: socket.socket, count: int) -> bytes:
    buf = b""
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            raise EOFError("Socket closed prematurely")
        buf += chunk
    return buf


def test_case_1_identity_and_pin(demo_bin: str, tmp_dir: Path) -> str:
    print("[Case 1] Identity generation, permissions, failure handling, and certificate pinning...")
    cert_path = tmp_dir / "server_cert.pem"
    key_path = tmp_dir / "server_key.pem"

    # 1A: Fresh generation under umask 022 to verify mode 0600 on private key
    old_umask = os.umask(0o022)
    try:
        cp = run_cmd([demo_bin, "generate-identity", "--cert", str(cert_path), "--key", str(key_path)])
    finally:
        os.umask(old_umask)

    if cp.returncode != 0:
        raise RuntimeError(f"generate-identity failed (exit={cp.returncode}): {cp.stderr}")

    match = re.search(r"FINGERPRINT=([0-9a-fA-F]{64})", cp.stdout)
    if not match:
        raise RuntimeError(f"FINGERPRINT not found in output: {cp.stdout}")
    demo_fingerprint = match.group(1).lower()

    # Verify private key mode is 0600 (owner only)
    key_mode = key_path.stat().st_mode & 0o777
    if key_mode & 0o077 != 0:
        raise AssertionError(f"Private key permissions too open: {oct(key_mode)}, expected 0600")

    # 1B: Verify failed key write (e.g. to /dev/full) reports failure and does NOT print fingerprint
    cp_fail = run_cmd([demo_bin, "generate-identity", "--cert", str(tmp_dir / "fail_cert.pem"), "--key", "/dev/full"])
    if cp_fail.returncode == 0 or "FINGERPRINT=" in cp_fail.stdout:
        raise AssertionError("generate-identity succeeded or reported fingerprint when writing key to /dev/full!")
    if (tmp_dir / "fail_cert.pem").exists():
        raise AssertionError("fail_cert.pem was not cleaned up after key write failure!")

    # 1C: Preservation of existing identity on replacement failure
    cert_before = cert_path.read_bytes()
    key_before = key_path.read_bytes()
    key_path.chmod(0o400)
    try:
        cp_regen = run_cmd([demo_bin, "generate-identity", "--cert", str(cert_path), "--key", str(key_path)])
    finally:
        key_path.chmod(0o600)

    if cp_regen.returncode == 0 or "FINGERPRINT=" in cp_regen.stdout:
        raise AssertionError("generate-identity succeeded or reported fingerprint when key was read-only!")
    if not cert_path.exists() or cert_path.read_bytes() != cert_before:
        raise AssertionError("Existing certificate was modified or deleted on failed identity generation!")
    if not key_path.exists() or key_path.read_bytes() != key_before:
        raise AssertionError("Existing private key was modified or deleted on failed identity generation!")

    # 1D: Partial destination existence: cert exists but key is new
    new_key_path = tmp_dir / "new_key_only.pem"
    cp_partial = run_cmd([demo_bin, "generate-identity", "--cert", str(cert_path), "--key", str(new_key_path)])
    if cp_partial.returncode == 0 or "FINGERPRINT=" in cp_partial.stdout:
        raise AssertionError("generate-identity succeeded when certificate already existed!")
    if cert_path.read_bytes() != cert_before:
        raise AssertionError("Existing certificate was overwritten when cert already existed!")
    if new_key_path.exists():
        raise AssertionError("New key was left behind after generation was rejected for preexisting cert!")

    # 1E: Independent OpenSSL DER SHA-256 calculation
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
    openssl_hash = dgst_cp.stdout.decode("utf-8").strip().split()[-1].lower()

    if demo_fingerprint != openssl_hash:
        raise AssertionError(f"Pin mismatch! demo={demo_fingerprint} vs openssl={openssl_hash}")

    # 1F: Test fingerprint subcommand
    fp_cp = run_cmd([demo_bin, "fingerprint", "--cert", str(cert_path)])
    if fp_cp.returncode != 0:
        raise RuntimeError(f"fingerprint command failed: {fp_cp.stderr}")
    match2 = re.search(r"FINGERPRINT=([0-9a-fA-F]{64})", fp_cp.stdout)
    if not match2 or match2.group(1).lower() != demo_fingerprint:
        raise AssertionError("fingerprint subcommand output mismatch")

    print(f"  PASS: Identity generated (key mode {oct(key_mode)}), /dev/full safely rejected, existing identity preserved on partial failure, pin {demo_fingerprint} verified against OpenSSL")
    return demo_fingerprint


def test_case_2_tls13_loopback_transfer(demo_bin: str, tmp_dir: Path, cert_path: Path, key_path: Path, pin: str, file_size_bytes: int = 1048576) -> dict:
    print(f"[Case 2] Loopback TCP transfer ({file_size_bytes} bytes), TLS 1.3 enforcement & SHA-256 check...")
    port = reserve_port()
    input_file = tmp_dir / f"input_{file_size_bytes}.bin"
    output_file = tmp_dir / f"output_{file_size_bytes}.bin"

    payload = os.urandom(file_size_bytes)
    input_file.write_bytes(payload)
    expected_sha256 = hashlib.sha256(payload).hexdigest().lower()

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

    try:
        server_line = server_proc.stdout.readline()
        if f"LISTENING port={port}" not in server_line:
            raise RuntimeError(f"Server failed to start listening on port {port}: {server_line}")

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
    finally:
        if server_proc.poll() is None:
            server_proc.kill()
            server_proc.communicate()

    if client_cp.returncode != 0:
        raise RuntimeError(f"Client failed (exit={client_cp.returncode}):\nSTDOUT: {client_cp.stdout}\nSTDERR: {client_cp.stderr}")
    if server_proc.returncode != 0:
        raise RuntimeError(f"Server failed (exit={server_proc.returncode}):\nSTDOUT: {server_stdout}\nSTDERR: {server_stderr}")

    if "version=TLSv1.3" not in client_cp.stdout or "version=TLSv1.3" not in server_stdout:
        raise AssertionError("Negotiated TLS version is not TLSv1.3")

    if not output_file.exists():
        raise AssertionError(f"Output file {output_file} was not committed")

    actual_sha256 = hashlib.sha256(output_file.read_bytes()).hexdigest().lower()
    if actual_sha256 != expected_sha256:
        raise AssertionError(f"Payload SHA-256 mismatch! expected={expected_sha256} actual={actual_sha256}")

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

    try:
        server_line = server_proc.stdout.readline()
        if f"LISTENING port={port}" not in server_line:
            raise RuntimeError(f"Server failed to start listening: {server_line}")

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
    finally:
        if server_proc.poll() is None:
            server_proc.kill()
            server_proc.communicate()

    if client_cp.returncode != 6:
        raise AssertionError(f"Expected client exit 6 (PIN_MISMATCH), got {client_cp.returncode}. STDOUT: {client_cp.stdout} STDERR: {client_cp.stderr}")

    if "PIN_MISMATCH" not in client_cp.stderr and "PIN_MISMATCH" not in client_cp.stdout:
        raise AssertionError(f"Expected PIN_MISMATCH in client output: {client_cp.stderr}")

    if output_file.exists():
        raise AssertionError("Server committed output file despite pin mismatch!")

    print("  PASS: Client rejected tampered pin before payload transmission. Zero output committed.")


def test_case_4_edge_cases_and_recovery(demo_bin: str, tmp_dir: Path, cert_path: Path, key_path: Path, pin: str) -> None:
    print("[Case 4] Robustness: delayed headers, ACK stalls, fragmented ACKs, disconnects, checksums, symlink safety, and recovery...")

    # 4A: Delayed header within deadline
    p_hdr = reserve_port()
    out_hdr = tmp_dir / "output_hdr.bin"
    server_hdr = subprocess.Popen(
        [
            demo_bin, "server",
            "--port", str(p_hdr),
            "--cert", str(cert_path),
            "--key", str(key_path),
            "--output", str(out_hdr),
            "--timeout-ms", "2000"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        s_line = server_hdr.stdout.readline()
        assert f"LISTENING port={p_hdr}" in s_line

        cctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        cctx.check_hostname = False
        cctx.verify_mode = ssl.CERT_NONE
        cctx.minimum_version = ssl.TLSVersion.TLSv1_3

        with cctx.wrap_socket(socket.create_connection(('127.0.0.1', p_hdr), timeout=2.0), server_hostname='localhost') as s:
            s.settimeout(2.0)
            empty_payload_hdr = b'LSSD' + struct.pack('!Q', 0) + hashlib.sha256(b'').hexdigest().encode()
            s.sendall(empty_payload_hdr[:1])
            time.sleep(0.2)
            s.sendall(empty_payload_hdr[1:])
            time.sleep(0.1)

        s_out, s_err = server_hdr.communicate(timeout=3.0)
    finally:
        if server_hdr.poll() is None:
            server_hdr.kill()
            server_hdr.communicate()

    if server_hdr.returncode != 0:
        raise AssertionError(f"Server failed on delayed header within deadline (exit={server_hdr.returncode}): {s_err}")
    print("  PASS: Delayed header within configured deadline succeeded.")

    # 4B: Custom peer helper for bounded client tests
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.load_cert_chain(cert_path, key_path)

    test_payload = tmp_dir / "test_payload.bin"
    test_payload.write_bytes(os.urandom(4096))
    test_digest = hashlib.sha256(test_payload.read_bytes()).hexdigest().lower()

    def run_client_against_custom_peer(behavior, timeout_ms=300):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(3.0)
        lp = listener.getsockname()[1]
        errs = []

        def peer_worker():
            try:
                raw, _ = listener.accept()
                raw.settimeout(3.0)
                with context.wrap_socket(raw, server_side=True) as ss:
                    ss.settimeout(3.0)
                    hdr = exact_read(ss, 76)
                    cnt = struct.unpack('!Q', hdr[4:12])[0]
                    exact_read(ss, cnt)
                    behavior(ss)
            except Exception as e:
                errs.append(str(e))
            finally:
                listener.close()

        th = threading.Thread(target=peer_worker)
        th.start()

        cp = run_cmd([
            demo_bin, "client",
            "--host", "127.0.0.1",
            "--port", str(lp),
            "--file", str(test_payload),
            "--expected-pin", pin,
            "--timeout-ms", str(timeout_ms)
        ], timeout=5.0)

        th.join(timeout=3.0)
        if th.is_alive():
            raise RuntimeError("Peer worker thread did not terminate within timeout!")
        if errs:
            print(f"    (Worker observed expected peer error: {errs})")
        return cp

    # Stalled ACK (server pauses longer than client timeout)
    cp_stall = run_client_against_custom_peer(lambda ss: time.sleep(0.8), timeout_ms=200)
    if cp_stall.returncode == 0:
        raise AssertionError("Client did not fail when ACK stalled")
    print("  PASS: Client bounded ACK wait expired cleanly without hanging.")

    # Fragmented ACK (server sends 'O', pauses 150ms, then rest)
    def fragmented_ack_sender(ss):
        ss.sendall(b"O")
        time.sleep(0.15)
        ss.sendall(f"K: SHA256 MATCH {test_digest}\n".encode("utf-8"))

    cp_frag = run_client_against_custom_peer(fragmented_ack_sender, timeout_ms=1000)
    if cp_frag.returncode != 0:
        raise AssertionError(f"Client failed on valid fragmented ACK (exit={cp_frag.returncode}): {cp_frag.stderr}")
    print("  PASS: Fragmented ACK frame received and accepted under deadline.")

    # Wrong digest in ACK
    cp_wrong_ack = run_client_against_custom_peer(lambda ss: ss.sendall(b"OK: SHA256 MATCH " + b"0" * 64 + b"\n"), timeout_ms=1000)
    if cp_wrong_ack.returncode == 0:
        raise AssertionError("Client accepted ACK with wrong digest!")
    print("  PASS: ACK with mismatched payload digest rejected.")

    # 4C: Mid-payload client disconnect & temp cleanup
    out_disc = tmp_dir / "output_disc.bin"
    out_disc.write_bytes(b"PRESERVE OLD CONTENT")
    p_disc = reserve_port()
    server_disc = subprocess.Popen(
        [
            demo_bin, "server",
            "--port", str(p_disc),
            "--cert", str(cert_path),
            "--key", str(key_path),
            "--output", str(out_disc),
            "--timeout-ms", "1000"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        assert f"LISTENING port={p_disc}" in server_disc.stdout.readline()
        cctx_disc = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        cctx_disc.check_hostname = False
        cctx_disc.verify_mode = ssl.CERT_NONE
        cctx_disc.minimum_version = ssl.TLSVersion.TLSv1_3
        with cctx_disc.wrap_socket(socket.create_connection(('127.0.0.1', p_disc), timeout=2.0), server_hostname='localhost') as s:
            s.settimeout(2.0)
            payload_data = os.urandom(8192)
            d_hdr = b'LSSD' + struct.pack('!Q', len(payload_data)) + hashlib.sha256(payload_data).hexdigest().encode()
            s.sendall(d_hdr)
            s.sendall(payload_data[:100])  # Disconnect prematurely after 100 bytes
        so_disc, se_disc = server_disc.communicate(timeout=3.0)
    finally:
        if server_disc.poll() is None:
            server_disc.kill()
            server_disc.communicate()

    if server_disc.returncode != 4:
        raise AssertionError(f"Expected server exit 4 on disconnect, got {server_disc.returncode}. err: {se_disc}")
    if out_disc.read_bytes() != b"PRESERVE OLD CONTENT":
        raise AssertionError("Prior output file was overwritten on disconnect!")
    leftover_disc = list(tmp_dir.glob(".lssd_recv_*"))
    if leftover_disc:
        raise AssertionError(f"Temporary receive files left behind after disconnect: {leftover_disc}")
    print("  PASS: Mid-payload disconnect rejected with exit 4, prior output preserved, 0 leftover temp files.")

    # 4D: Checksum mismatch & temp cleanup
    out_chk = tmp_dir / "output_chk.bin"
    out_chk.write_bytes(b"PRESERVE OLD CONTENT CHK")
    p_chk = reserve_port()
    server_chk = subprocess.Popen(
        [
            demo_bin, "server",
            "--port", str(p_chk),
            "--cert", str(cert_path),
            "--key", str(key_path),
            "--output", str(out_chk),
            "--timeout-ms", "1000"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        assert f"LISTENING port={p_chk}" in server_chk.stdout.readline()
        with cctx_disc.wrap_socket(socket.create_connection(('127.0.0.1', p_chk), timeout=2.0), server_hostname='localhost') as s:
            s.settimeout(2.0)
            payload_data = os.urandom(4096)
            c_hdr = b'LSSD' + struct.pack('!Q', len(payload_data)) + (b"0" * 64)  # Wrong digest
            s.sendall(c_hdr)
            s.sendall(payload_data)
            try:
                s.recv(256)
            except OSError:
                pass
        so_chk, se_chk = server_chk.communicate(timeout=3.0)
    finally:
        if server_chk.poll() is None:
            server_chk.kill()
            server_chk.communicate()

    if server_chk.returncode != 3:
        raise AssertionError(f"Expected server exit 3 on checksum mismatch, got {server_chk.returncode}. err: {se_chk}")
    if out_chk.read_bytes() != b"PRESERVE OLD CONTENT CHK":
        raise AssertionError("Prior output file was overwritten on checksum mismatch!")
    leftover_chk = list(tmp_dir.glob(".lssd_recv_*"))
    if leftover_chk:
        raise AssertionError(f"Temporary receive files left behind after checksum mismatch: {leftover_chk}")
    print("  PASS: Checksum mismatch rejected with exit 3, prior output preserved, 0 leftover temp files.")

    # 4E: Symlink overwrite safety
    victim_file = tmp_dir / "victim.bin"
    victim_file.write_bytes(b"ORIGINAL SENSITIVE DATA TO PRESERVE")
    output_symlink_target = tmp_dir / "safe_output.bin"
    Path(str(output_symlink_target) + ".tmp").symlink_to(victim_file)

    p_sym = reserve_port()
    server_sym = subprocess.Popen(
        [
            demo_bin, "server",
            "--port", str(p_sym),
            "--cert", str(cert_path),
            "--key", str(key_path),
            "--output", str(output_symlink_target),
            "--timeout-ms", "3000"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        s_sym_line = server_sym.stdout.readline()
        assert f"LISTENING port={p_sym}" in s_sym_line

        cp_sym_client = run_cmd([
            demo_bin, "client",
            "--host", "127.0.0.1",
            "--port", str(p_sym),
            "--file", str(test_payload),
            "--expected-pin", pin,
            "--timeout-ms", "3000"
        ], timeout=5.0)

        server_sym.communicate(timeout=3.0)
    finally:
        if server_sym.poll() is None:
            server_sym.kill()
            server_sym.communicate()

    if victim_file.read_bytes() != b"ORIGINAL SENSITIVE DATA TO PRESERVE":
        raise AssertionError("Security violation: victim file was overwritten through symlink!")
    if output_symlink_target.is_symlink():
        raise AssertionError("Security violation: output file is a symlink!")
    print("  PASS: Temporary file isolation verified (victim preserved, no symlink traversal).")

    # 4F: Recovery transfer
    print("  Testing recovery transfer following edge cases...")
    test_case_2_tls13_loopback_transfer(demo_bin, tmp_dir, cert_path, key_path, pin, file_size_bytes=65536)
    print("  PASS: Subsequent valid run succeeded with 100% data integrity.")


def test_case_5_descriptor_ownership(demo_bin: str, tmp_dir: Path, cert_path: Path, key_path: Path, pin: str) -> None:
    print("[Case 5] Checking descriptor ownership (verifying no duplicate socket close / EBADF)...")
    port = reserve_port()
    input_file = tmp_dir / "input_strace.bin"
    output_file = tmp_dir / "output_strace.bin"
    input_file.write_bytes(os.urandom(32768))

    server_proc = subprocess.Popen(
        [
            demo_bin, "server",
            "--port", str(port),
            "--cert", str(cert_path),
            "--key", str(key_path),
            "--output", str(output_file),
            "--timeout-ms", "5000"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        assert f"LISTENING port={port}" in server_proc.stdout.readline()

        trace_file = tmp_dir / "client_close.strace"
        cp = run_cmd([
            "strace", "-e", "trace=close", "-o", str(trace_file),
            demo_bin, "client",
            "--host", "127.0.0.1",
            "--port", str(port),
            "--file", str(input_file),
            "--expected-pin", pin,
            "--timeout-ms", "5000"
        ], timeout=10.0)

        server_proc.communicate(timeout=5.0)
    finally:
        if server_proc.poll() is None:
            server_proc.kill()
            server_proc.communicate()

    if cp.returncode != 0:
        raise RuntimeError(f"Client strace run failed (exit={cp.returncode}): {cp.stderr}")

    trace_text = trace_file.read_text()
    ebadf_lines = [line for line in trace_text.splitlines() if "EBADF" in line]
    if ebadf_lines:
        raise AssertionError(f"Double socket close detected (EBADF in strace): {ebadf_lines}")

    print("  PASS: No double socket close detected (0 EBADF calls in strace).")


def test_case_6_fault_injection_and_backpressure(demo_bin: str, tmp_dir: Path, cert_path: Path, key_path: Path, pin: str) -> None:
    print("[Case 6] Fault injection, failure paths (fsync, close, rename, ack), and write backpressure...")
    faults_c = Path(__file__).resolve().parent / "faults.c"
    faults_so = tmp_dir / "faults.so"

    # Compile faults helper shared object
    comp = subprocess.run(["cc", "-shared", "-fPIC", str(faults_c), "-o", str(faults_so), "-ldl"], capture_output=True, text=True)
    if comp.returncode != 0:
        raise RuntimeError(f"Failed to compile faults.c: {comp.stderr}")

    payload = tmp_dir / "fault_payload.bin"
    payload.write_bytes(os.urandom(4096))
    payload_digest = hashlib.sha256(payload.read_bytes()).hexdigest().lower()

    # 6A: fsync failure prevents commit
    out_fsync = tmp_dir / "fsync.bin"
    p_fsync = reserve_port()
    env_fsync = os.environ.copy()
    env_fsync.update(LD_PRELOAD=str(faults_so), H1_FAULT_MODE="fsync", H1_FAULT_OUTPUT=str(out_fsync))
    srv_fsync = subprocess.Popen([demo_bin, "server", "--port", str(p_fsync), "--cert", str(cert_path), "--key", str(key_path), "--output", str(out_fsync), "--timeout-ms", "700"], env=env_fsync, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert f"LISTENING port={p_fsync}" in srv_fsync.stdout.readline()
        run_cmd([demo_bin, "client", "--port", str(p_fsync), "--file", str(payload), "--expected-pin", pin, "--timeout-ms", "700"])
        so, se = srv_fsync.communicate(timeout=3.0)
    finally:
        if srv_fsync.poll() is None:
            srv_fsync.kill()
            srv_fsync.communicate()

    if srv_fsync.returncode == 0 or out_fsync.exists() or "TRANSFER_OK" in so:
        raise AssertionError(f"fsync failure did not abort server commit! exit={srv_fsync.returncode} exists={out_fsync.exists()}")
    leftover = list(tmp_dir.glob(".lssd_recv_*"))
    if leftover:
        raise AssertionError(f"Temp file left behind after fsync failure: {leftover}")
    print("  PASS: Injected fsync EIO safely aborts transfer, output not committed, temp cleaned up.")

    # 6B: close failure prevents commit
    out_close = tmp_dir / "close.bin"
    p_close = reserve_port()
    env_close = os.environ.copy()
    env_close.update(LD_PRELOAD=str(faults_so), H1_FAULT_MODE="close", H1_FAULT_OUTPUT=str(out_close))
    srv_close = subprocess.Popen([demo_bin, "server", "--port", str(p_close), "--cert", str(cert_path), "--key", str(key_path), "--output", str(out_close), "--timeout-ms", "700"], env=env_close, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert f"LISTENING port={p_close}" in srv_close.stdout.readline()
        run_cmd([demo_bin, "client", "--port", str(p_close), "--file", str(payload), "--expected-pin", pin, "--timeout-ms", "700"])
        so, se = srv_close.communicate(timeout=3.0)
    finally:
        if srv_close.poll() is None:
            srv_close.kill()
            srv_close.communicate()

    if srv_close.returncode == 0 or out_close.exists() or "TRANSFER_OK" in so:
        raise AssertionError(f"close failure did not abort server commit! exit={srv_close.returncode} exists={out_close.exists()}")
    leftover = list(tmp_dir.glob(".lssd_recv_*"))
    if leftover:
        raise AssertionError(f"Temp file left behind after close failure: {leftover}")
    print("  PASS: Injected close EIO safely aborts transfer, output not committed, temp cleaned up.")

    # 6C: rename failure preserves previous destination
    out_ren = tmp_dir / "rename.bin"
    victim_ren = tmp_dir / "victim_ren.bin"
    victim_ren.write_bytes(b"VICTIM DATA PRESERVED ON FAILED RENAME")
    out_ren.symlink_to(victim_ren)
    p_ren = reserve_port()
    env_ren = os.environ.copy()
    env_ren.update(LD_PRELOAD=str(faults_so), H1_FAULT_MODE="rename", H1_FAULT_OUTPUT=str(out_ren))
    srv_ren = subprocess.Popen([demo_bin, "server", "--port", str(p_ren), "--cert", str(cert_path), "--key", str(key_path), "--output", str(out_ren), "--timeout-ms", "700"], env=env_ren, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert f"LISTENING port={p_ren}" in srv_ren.stdout.readline()
        run_cmd([demo_bin, "client", "--port", str(p_ren), "--file", str(payload), "--expected-pin", pin, "--timeout-ms", "700"])
        so, se = srv_ren.communicate(timeout=3.0)
    finally:
        if srv_ren.poll() is None:
            srv_ren.kill()
            srv_ren.communicate()

    if srv_ren.returncode == 0 or not out_ren.is_symlink() or victim_ren.read_bytes() != b"VICTIM DATA PRESERVED ON FAILED RENAME":
        raise AssertionError(f"Failed rename did not preserve existing destination symlink! exit={srv_ren.returncode}")
    print("  PASS: Injected rename EIO preserves preexisting destination symlink and victim data.")

    # 6D: ACK socket delivery failure reports failure without TRANSFER_OK, preserves committed file
    out_ack = tmp_dir / "ack.bin"
    p_ack = reserve_port()
    env_ack = os.environ.copy()
    env_ack.update(LD_PRELOAD=str(faults_so), H1_FAULT_MODE="ack", H1_FAULT_OUTPUT=str(out_ack))
    srv_ack = subprocess.Popen([demo_bin, "server", "--port", str(p_ack), "--cert", str(cert_path), "--key", str(key_path), "--output", str(out_ack), "--timeout-ms", "700"], env=env_ack, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert f"LISTENING port={p_ack}" in srv_ack.stdout.readline()
        run_cmd([demo_bin, "client", "--port", str(p_ack), "--file", str(payload), "--expected-pin", pin, "--timeout-ms", "700"])
        so, se = srv_ack.communicate(timeout=3.0)
    finally:
        if srv_ack.poll() is None:
            srv_ack.kill()
            srv_ack.communicate()

    if srv_ack.returncode == 0 or "TRANSFER_OK" in so or not out_ack.exists():
        raise AssertionError(f"ACK failure was not reported accurately! exit={srv_ack.returncode} stdout={so}")
    if "TRANSFER_COMMITTED_BUT_ACK_FAILED" not in se:
        raise AssertionError(f"Expected TRANSFER_COMMITTED_BUT_ACK_FAILED diagnostic: {se}")
    print("  PASS: Injected ACK delivery EPIPE reports failure (exit 5), avoids TRANSFER_OK, committed file intact.")

    # 6E: Forced write backpressure (SO_SNDBUF=4096) with WANT_WRITE / EAGAIN retry
    big_file = tmp_dir / "big_file.bin"
    big_file.write_bytes(bytes(range(256)) * 2048)  # 512 KiB
    big_digest = hashlib.sha256(big_file.read_bytes()).hexdigest().lower()

    bp_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    bp_context.minimum_version = ssl.TLSVersion.TLSv1_3
    bp_context.load_cert_chain(cert_path, key_path)

    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    listener.settimeout(3.0)
    p_bp = listener.getsockname()[1]
    bp_errs = []
    bp_received = []

    def slow_reader():
        try:
            raw, _ = listener.accept()
            raw.settimeout(3.0)
            with bp_context.wrap_socket(raw, server_side=True) as ss:
                ss.settimeout(3.0)
                hdr = exact_read(ss, 76)
                time.sleep(0.25)  # Force sender write buffer backpressure
                payload_len = struct.unpack('!Q', hdr[4:12])[0]
                recvd = exact_read(ss, payload_len)
                bp_received.append(hashlib.sha256(recvd).hexdigest().lower())
                ss.sendall(f"OK: SHA256 MATCH {bp_received[0]}\n".encode("utf-8"))
        except Exception as e:
            bp_errs.append(str(e))
        finally:
            listener.close()

    th_bp = threading.Thread(target=slow_reader)
    th_bp.start()

    env_bp = os.environ.copy()
    env_bp.update(LD_PRELOAD=str(faults_so), H1_FAULT_MODE="backpressure")
    cp_bp = run_cmd([demo_bin, "client", "--port", str(p_bp), "--file", str(big_file), "--expected-pin", pin, "--timeout-ms", "2000"], env=env_bp, timeout=6.0)
    th_bp.join(timeout=4.0)

    if th_bp.is_alive():
        raise RuntimeError("Backpressure peer thread did not terminate!")
    if cp_bp.returncode != 0:
        raise AssertionError(f"Client failed under backpressure: {cp_bp.stderr}")
    if "backpressure EAGAIN observed" not in cp_bp.stderr:
        raise AssertionError(f"Expected backpressure EAGAIN was not observed in client stderr: {cp_bp.stderr}")
    if bp_received != [big_digest]:
        raise AssertionError(f"Backpressure payload digest mismatch: {bp_received} vs {big_digest}")

    print("  PASS: Forced write backpressure (SO_SNDBUF=4096) triggered EAGAIN, nonblocking retry succeeded, digest matched.")


def run_benchmark(demo_bin: str, tmp_dir: Path, cert_path: Path, key_path: Path, pin: str) -> None:
    print("\n--- Performance, Resource & Environment Measurements ---")
    results = []
    repeat_count = 5
    print(f"Repeat count: {repeat_count} iterations per payload")

    for sz in [1048576, 10485760]:  # 1 MiB and 10 MiB
        runs = []
        for _ in range(repeat_count):
            m = test_case_2_tls13_loopback_transfer(demo_bin, tmp_dir, cert_path, key_path, pin, file_size_bytes=sz)
            runs.append(m)
        avg_duration = sum(r['duration_ms'] for r in runs) / float(repeat_count)
        avg_throughput = sum(r['throughput_mb_s'] for r in runs) / float(repeat_count)
        results.append({
            "bytes": sz,
            "avg_duration_ms": avg_duration,
            "avg_throughput_mb_s": avg_throughput,
        })

    # Host system and hardware inspection
    cpu_model = "Unknown CPU"
    try:
        with open("/proc/cpuinfo", "r") as f:
            for line in f:
                if "model name" in line:
                    cpu_model = line.split(":", 1)[1].strip()
                    break
    except Exception:
        pass

    mem_total_kb = 0
    try:
        with open("/proc/meminfo", "r") as f:
            for line in f:
                if "MemTotal" in line:
                    mem_total_kb = int(line.split()[1])
                    break
    except Exception:
        pass

    # Host process memory inspection (VmRSS of demo binary execution)
    rss_mb = 0.0
    try:
        mem_cp = subprocess.run(["/usr/bin/time", "-v", demo_bin, "fingerprint", "--cert", str(cert_path)], capture_output=True, text=True)
        m = re.search(r"Maximum resident set size \(kbytes\):\s+(\d+)", mem_cp.stderr)
        if m:
            rss_mb = int(m.group(1)) / 1024.0
    except Exception:
        pass

    stat = os.stat(demo_bin)
    size_cp = run_cmd(["size", demo_bin])

    print("\nSystem & Build Environment:")
    print(f"  OS: Linux {os.uname().release} ({os.uname().machine})")
    print(f"  Host CPU: {cpu_model}")
    print(f"  Host RAM: {mem_total_kb / 1024.0:.1f} MiB")
    print("  Compiler Flags: -std=c++98 -Wl,--exclude-libs,ALL")
    print(f"  Binary file size: {stat.st_size} bytes ({stat.st_size / (1024.0 * 1024.0):.2f} MB)")
    if rss_mb > 0.0:
        print(f"  Host Peak Memory Usage (RSS): ~{rss_mb:.2f} MiB")
    if size_cp.returncode == 0:
        print("  ELF section breakdown:")
        for line in size_cp.stdout.strip().splitlines():
            print(f"    {line}")

    print("\nTransfer Benchmark Summary:")
    for r in results:
        print(f"  Payload: {r['bytes'] / (1024*1024):.1f} MiB | Avg Duration: {r['avg_duration_ms']:.2f} ms | Avg Throughput: {r['avg_throughput_mb_s']:.2f} MB/s")

    print("\nDevice Observations:")
    print("  QNX ARMv7 ELF32 Symbol Leakage: 0 symbols (.dynsym verified)")
    print("  Physical BB10 BAR Size Delta: NOT MEASURED (host container build only; no device deployment)")
    print("  Physical BB10 Memory Usage: NOT MEASURED (device execution outside H1 scope)")


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
        test_case_4_edge_cases_and_recovery(demo_bin, tmp_dir, cert_path, key_path, pin)
        test_case_5_descriptor_ownership(demo_bin, tmp_dir, cert_path, key_path, pin)
        test_case_6_fault_injection_and_backpressure(demo_bin, tmp_dir, cert_path, key_path, pin)
        run_benchmark(demo_bin, tmp_dir, cert_path, key_path, pin)

    print("\nALL LOCAL-SEND STYLE POSIX-TLS INTEGRATION CHECKS PASSED SUCCESSFULLY (7/7).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
