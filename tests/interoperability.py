#!/usr/bin/env python3
"""
LocalSend BlackBerry 10 Interoperability & Fixture Test Suite
Conforms to docs/mvp-plan.md, docs/protocol-notes.md, and docs/build-deploy.md.
Provides reproducible test fixtures, HTTP endpoint verification, mock peer server,
and automated PC-to-device streaming send client without external dependencies.
"""

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.parse
import urllib.request
import urllib.error
from http.server import HTTPServer, BaseHTTPRequestHandler

DEFAULT_PORT = 53317

def generate_fixtures(output_dir: str, include_100m: bool = False):
    """Generate deterministic test fixtures, manifest.json, and manifest.sha256."""
    os.makedirs(output_dir, exist_ok=True)
    print(f"Generating deterministic fixtures in {output_dir}...")

    manifest = {}
    sha256_lines = []

    # Small deterministic fixtures
    fixtures = [
        ("0b.bin", 0, b""),
        ("1k.bin", 1024, bytes([i % 256 for i in range(1024)])),
        ("测试文件_照片.png", 2048, bytes([(i * 13) % 256 for i in range(2048)])),
        ("notes with spaces.txt", 1024, bytes([(i * 7) % 256 for i in range(1024)])),
    ]

    for fname, size, content in fixtures:
        path = os.path.join(output_dir, fname)
        with open(path, "wb") as f:
            f.write(content)
        h = hashlib.sha256(content).hexdigest()
        manifest[fname] = {
            "size": size,
            "sha256": h,
        }
        sha256_lines.append(f"{h}  {fname}\n")
        print(f"  [+] {fname:<24} ({size:>7} B) -> sha256: {h}")

    # Generate exact 10 MiB deterministic fixture: exactly 10,485,760 bytes
    ten_mib_name = "10m.bin"
    ten_mib_size = 10485760 # 10 * 1024 * 1024
    ten_mib_path = os.path.join(output_dir, ten_mib_name)
    hasher = hashlib.sha256()

    print(f"  [+] Generating {ten_mib_name} ({ten_mib_size} bytes)...")
    chunk_pattern = bytes([i % 256 for i in range(65536)]) # 64 KiB recurring pattern
    written = 0
    with open(ten_mib_path, "wb") as f:
        while written < ten_mib_size:
            to_write = min(len(chunk_pattern), ten_mib_size - written)
            chunk = chunk_pattern[:to_write]
            f.write(chunk)
            hasher.update(chunk)
            written += to_write

    ten_mib_hash = hasher.hexdigest()
    manifest[ten_mib_name] = {
        "size": ten_mib_size,
        "sha256": ten_mib_hash,
    }
    sha256_lines.append(f"{ten_mib_hash}  {ten_mib_name}\n")
    print(f"  [+] {ten_mib_name:<24} ({ten_mib_size:>7} B) -> sha256: {ten_mib_hash}")

    # Generate 100 MiB fixture if requested: exactly 104,857,600 bytes
    if include_100m:
        hundred_mib_name = "100m.bin"
        hundred_mib_size = 104857600 # 100 * 1024 * 1024
        hundred_mib_path = os.path.join(output_dir, hundred_mib_name)
        hasher100 = hashlib.sha256()

        print(f"  [+] Generating {hundred_mib_name} ({hundred_mib_size} bytes)...")
        written = 0
        with open(hundred_mib_path, "wb") as f:
            while written < hundred_mib_size:
                to_write = min(len(chunk_pattern), hundred_mib_size - written)
                chunk = chunk_pattern[:to_write]
                f.write(chunk)
                hasher100.update(chunk)
                written += to_write

        hundred_mib_hash = hasher100.hexdigest()
        manifest[hundred_mib_name] = {
            "size": hundred_mib_size,
            "sha256": hundred_mib_hash,
        }
        sha256_lines.append(f"{hundred_mib_hash}  {hundred_mib_name}\n")
        print(f"  [+] {hundred_mib_name:<24} ({hundred_mib_size:>7} B) -> sha256: {hundred_mib_hash}")

    manifest_json_path = os.path.join(output_dir, "manifest.json")
    with open(manifest_json_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
    print(f"Manifest JSON saved to {manifest_json_path}")

    manifest_sha_path = os.path.join(output_dir, "manifest.sha256")
    with open(manifest_sha_path, "w", encoding="utf-8") as f:
        f.writelines(sha256_lines)
    print(f"Manifest SHA-256 saved to {manifest_sha_path}")


def check_http(host: str, port: int):
    """Perform baseline sanity checks against a running LocalSend HTTP server."""
    base_url = f"http://{host}:{port}"
    print(f"Checking LocalSend HTTP endpoints at {base_url}...")
    passes = 0
    failures = 0

    # 1. GET /api/localsend/v2/info
    try:
        url = f"{base_url}/api/localsend/v2/info"
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = resp.read()
            data = json.loads(body.decode("utf-8"))
            alias = data.get("alias", "")
            version = data.get("version", "")
            model = data.get("deviceModel", "")
            if resp.status == 200 and version.startswith("2."):
                print(f"  [PASS] GET /info (HTTP 200): alias='{alias}', version='{version}', model='{model}'")
                passes += 1
            else:
                print(f"  [FAIL] GET /info unexpected content: status={resp.status}, data={data}")
                failures += 1
    except Exception as e:
        print(f"  [FAIL] GET /info error: {e}")
        failures += 1

    # 2. POST /api/localsend/v2/register
    try:
        url = f"{base_url}/api/localsend/v2/register"
        reg_payload = {
            "alias": "Test Rig PC",
            "version": "2.2",
            "deviceModel": "Fedora Linux",
            "deviceType": "desktop",
            "fingerprint": "py-test-rig-fp",
            "port": DEFAULT_PORT,
            "protocol": "http",
            "download": False
        }
        req_data = json.dumps(reg_payload).encode("utf-8")
        req = urllib.request.Request(url, data=req_data, headers={
            "Content-Type": "application/json",
            "Content-Length": str(len(req_data))
        }, method="POST")
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = resp.read()
            data = json.loads(body.decode("utf-8"))
            if resp.status == 200 and "alias" in data:
                print(f"  [PASS] POST /register (HTTP 200): registered with peer '{data.get('alias')}'")
                passes += 1
            else:
                print(f"  [FAIL] POST /register unexpected response: {resp.status}")
                failures += 1
    except Exception as e:
        print(f"  [FAIL] POST /register error: {e}")
        failures += 1

    # 3. POST /api/localsend/v2/cancel?sessionId=test-unknown (idempotent 200 expected)
    try:
        url = f"{base_url}/api/localsend/v2/cancel?sessionId=test-unknown"
        req = urllib.request.Request(url, data=b"", headers={"Content-Length": "0"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as resp:
            if resp.status == 200:
                print(f"  [PASS] POST /cancel unknown session (HTTP 200 idempotent acknowledgment)")
                passes += 1
            else:
                print(f"  [FAIL] POST /cancel status {resp.status}")
                failures += 1
    except Exception as e:
        print(f"  [FAIL] POST /cancel error: {e}")
        failures += 1

    # 4. POST /api/localsend/v2/upload without query parameters (expected 400)
    try:
        url = f"{base_url}/api/localsend/v2/upload"
        req = urllib.request.Request(url, data=b"dummy", headers={"Content-Type": "application/octet-stream"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as resp:
            print(f"  [FAIL] POST /upload without params unexpectedly succeeded: {resp.status}")
            failures += 1
    except urllib.error.HTTPError as e:
        if e.code == 400:
            print(f"  [PASS] POST /upload without params correctly rejected (HTTP 400)")
            passes += 1
        else:
            print(f"  [FAIL] POST /upload returned status {e.code} (expected 400)")
            failures += 1
    except Exception as e:
        print(f"  [FAIL] POST /upload test error: {e}")
        failures += 1

    print(f"HTTP Endpoint Check Completed: {passes} Passed, {failures} Failed.")
    return failures == 0


class MockPeerHandler(BaseHTTPRequestHandler):
    storage_dir = "/tmp/localsend_mock_storage"
    active_sessions = {} # sessionId -> {"files": {fileId: token}, "meta": {fileId: FileMeta}}

    def log_message(self, format, *args):
        print(f"  [MockPeer] {self.address_string()} - {format % args}")

    def do_GET(self):
        if self.path == "/api/localsend/v2/info":
            resp = {
                "alias": "Mock PC Peer (Fedora)",
                "version": "2.2",
                "deviceModel": "PC Test Peer",
                "deviceType": "desktop",
                "fingerprint": "mock-pc-peer-fp",
                "port": self.server.server_port,
                "protocol": "http",
                "download": False
            }
            body = json.dumps(resp).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        content_len = int(self.headers.get("Content-Length", 0))

        if path == "/api/localsend/v2/register":
            req_body = self.rfile.read(content_len) if content_len > 0 else b""
            try:
                reg_data = json.loads(req_body.decode("utf-8"))
                print(f"  [MockPeer] Registered incoming peer: alias='{reg_data.get('alias')}', ip='{self.client_address[0]}'")
            except Exception:
                pass
            resp = {
                "alias": "Mock PC Peer (Fedora)",
                "version": "2.2",
                "deviceModel": "PC Test Peer",
                "deviceType": "desktop",
                "fingerprint": "mock-pc-peer-fp",
                "port": self.server.server_port,
                "protocol": "http",
                "download": False
            }
            body = json.dumps(resp).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        elif path == "/api/localsend/v2/prepare-upload":
            req_body = self.rfile.read(content_len) if content_len > 0 else b""
            try:
                prep_data = json.loads(req_body.decode("utf-8"))
            except Exception as e:
                self.send_response(400)
                self.end_headers()
                return

            session_id = hashlib.md5(f"sess-{time.time()}".encode()).hexdigest()
            files_map = prep_data.get("files", {})
            file_tokens = {}
            for fid in files_map:
                token = hashlib.sha256(f"tok-{fid}-{time.time()}".encode()).hexdigest()[:16]
                file_tokens[fid] = token

            self.active_sessions[session_id] = {
                "files": file_tokens,
                "meta": files_map
            }

            resp = {
                "sessionId": session_id,
                "files": file_tokens
            }
            body = json.dumps(resp).encode("utf-8")
            print(f"  [MockPeer] Accepted prepare-upload for {len(files_map)} files (sessionId: {session_id})")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        elif path == "/api/localsend/v2/upload":
            query = urllib.parse.parse_qs(parsed.query)
            sess_id = query.get("sessionId", [""])[0]
            file_id = query.get("fileId", [""])[0]
            token = query.get("token", [""])[0]

            if not sess_id or not file_id or not token:
                self.send_response(400)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

            sess = self.active_sessions.get(sess_id)
            if not sess or sess["files"].get(file_id) != token:
                self.send_response(403)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

            meta = sess["meta"].get(file_id, {})
            raw_filename = meta.get("fileName", f"upload_{file_id}.bin")
            expected_size = meta.get("size", 0)
            expected_sha = meta.get("sha256", "").strip().lower()

            # Security: Sanitize filename and prevent path traversal
            safe_basename = os.path.basename(raw_filename.replace("\\", "/"))
            if not safe_basename or safe_basename in (".", ".."):
                safe_basename = f"upload_{file_id}.bin"

            os.makedirs(self.storage_dir, exist_ok=True)
            storage_real = os.path.realpath(self.storage_dir)
            save_path = os.path.realpath(os.path.join(storage_real, safe_basename))

            # Path escape containment check
            if not save_path.startswith(storage_real + os.sep) and save_path != storage_real:
                self.send_response(400)
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return

            part_path = save_path + f".{os.getpid()}_{time.time()}.part"
            bytes_received = 0
            hasher = hashlib.sha256()
            start_t = time.time()
            write_failed = False

            try:
                with open(part_path, "wb") as f:
                    remaining = content_len
                    while remaining > 0:
                        read_size = min(65536, remaining)
                        chunk = self.rfile.read(read_size)
                        if not chunk:
                            break
                        f.write(chunk)
                        hasher.update(chunk)
                        bytes_received += len(chunk)
                        remaining -= len(chunk)
                    f.flush()
            except Exception:
                write_failed = True

            if write_failed or bytes_received != expected_size:
                if os.path.exists(part_path):
                    try:
                        os.remove(part_path)
                    except Exception:
                        pass
                self.send_response(400)
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return

            calculated_sha = hasher.hexdigest().lower()
            if expected_sha and calculated_sha != expected_sha:
                if os.path.exists(part_path):
                    try:
                        os.remove(part_path)
                    except Exception:
                        pass
                self.send_response(422)
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return

            # Atomic commit
            os.replace(part_path, save_path)

            elapsed = max(0.001, time.time() - start_t)
            speed_mbps = (bytes_received / (1024 * 1024)) / elapsed

            print(f"  [MockPeer] Upload complete: '{safe_basename}' ({bytes_received} bytes) in {elapsed:.2f}s ({speed_mbps:.2f} MB/s)")
            print(f"             SHA-256: {calculated_sha}")
            print(f"             Saved to: {save_path}")

            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()

        elif path == "/api/localsend/v2/cancel":
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()


def serve_peer(bind_ip: str, port: int, storage_dir: str):
    """Run mock LocalSend PC peer server on LAN for device testing."""
    os.makedirs(storage_dir, exist_ok=True)
    MockPeerHandler.storage_dir = storage_dir
    server = HTTPServer((bind_ip, port), MockPeerHandler)
    server.server_port = port
    print(f"Mock LocalSend PC Peer listening on http://{bind_ip}:{port}")
    print(f"Files received will be saved to: {storage_dir}")
    print("Press Ctrl+C to stop.\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping mock peer.")


def send_file(target_host: str, port: int, file_path: str):
    """Send a file to BlackBerry 10 device using standard LocalSend v2.2 HTTP protocol."""
    if not os.path.exists(file_path):
        print(f"Error: file not found: {file_path}")
        return False

    file_size = os.path.getsize(file_path)
    file_name = os.path.basename(file_path)
    file_id = hashlib.md5(f"{file_name}-{file_size}".encode()).hexdigest()[:12]

    # Pre-calculate file SHA-256
    hasher = hashlib.sha256()
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            hasher.update(chunk)
    file_sha = hasher.hexdigest()

    # Determine protocol scheme (auto-detect HTTPS vs HTTP)
    scheme = "http"
    ssl_context = None
    try:
        import ssl
        test_ctx = ssl._create_unverified_context()
        test_req = urllib.request.Request(f"https://{target_host}:{port}/api/localsend/v2/info")
        with urllib.request.urlopen(test_req, context=test_ctx, timeout=3) as resp:
            if resp.status == 200:
                scheme = "https"
                ssl_context = test_ctx
    except Exception:
        pass

    print(f"Sending '{file_name}' ({file_size} bytes, sha256: {file_sha[:12]}...) to {scheme}://{target_host}:{port}...")

    # Step 1: POST /api/localsend/v2/prepare-upload
    prep_req = {
        "info": {
            "alias": "Test Rig PC",
            "version": "2.2",
            "deviceModel": "Fedora Linux",
            "deviceType": "desktop",
            "fingerprint": "py-sender-rig",
            "port": DEFAULT_PORT,
            "protocol": scheme,
            "download": False
        },
        "files": {
            file_id: {
                "id": file_id,
                "fileName": file_name,
                "size": file_size,
                "fileType": "application/octet-stream",
                "sha256": file_sha
            }
        }
    }

    req_data = json.dumps(prep_req).encode("utf-8")
    prep_url = f"{scheme}://{target_host}:{port}/api/localsend/v2/prepare-upload"

    print("  [1/2] Sending prepare-upload request. Awaiting user consent on BB10 device...")
    try:
        req = urllib.request.Request(prep_url, data=req_data, headers={
            "Content-Type": "application/json",
            "Content-Length": str(len(req_data))
        }, method="POST")
        with urllib.request.urlopen(req, context=ssl_context, timeout=120) as resp:
            resp_body = resp.read()
            prep_resp = json.loads(resp_body.decode("utf-8"))
    except urllib.error.HTTPError as e:
        print(f"  [!] Prepare upload rejected: HTTP {e.code} ({e.reason})")
        return False
    except Exception as e:
        print(f"  [!] Prepare upload error: {e}")
        return False

    session_id = prep_resp.get("sessionId", "")
    file_tokens = prep_resp.get("files", {})
    token = file_tokens.get(file_id, "")

    if not token:
        print(f"  [!] Error: Device did not return a token for file {file_id}. Offered tokens: {file_tokens}")
        return False

    print(f"  [+] Offer accepted! SessionId: {session_id}, Token: {token}")

    # Step 2: POST /api/localsend/v2/upload streaming
    upload_url = f"{scheme}://{target_host}:{port}/api/localsend/v2/upload?sessionId={session_id}&fileId={file_id}&token={token}"
    print(f"  [2/2] Streaming {file_size} bytes to device...")

    start_time = time.time()
    try:
        with open(file_path, "rb") as f_in:
            req = urllib.request.Request(upload_url, data=f_in, headers={
                "Content-Type": "application/octet-stream",
                "Content-Length": str(file_size)
            }, method="POST")
            with urllib.request.urlopen(req, context=ssl_context, timeout=60) as resp:
                if resp.status == 200:
                    elapsed = max(0.001, time.time() - start_time)
                    speed_mbps = (file_size / (1024 * 1024)) / elapsed
                    print(f"  [SUCCESS] Upload completed in {elapsed:.2f}s ({speed_mbps:.2f} MB/s)!")
                    return True
                else:
                    print(f"  [!] Upload failed with HTTP {resp.status}")
                    return False
    except Exception as e:
        print(f"  [!] Streaming upload error: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(description="LocalSend BB10 Interoperability & Test Suite")
    subparsers = parser.add_subparsers(dest="command")

    # Fixtures command
    fix_p = subparsers.add_parser("fixtures", help="Generate deterministic test fixtures and manifest")
    fix_p.add_argument("--output", default="/tmp/localsend-bb10-fixtures", help="Output directory")
    fix_p.add_argument("--include-100m", action="store_true", help="Include 100 MiB fixture")

    # Check-http command
    chk_p = subparsers.add_parser("check-http", help="Check LocalSend HTTP endpoints on target device")
    chk_p.add_argument("--host", required=True, help="Target host / IP")
    chk_p.add_argument("--port", type=int, default=DEFAULT_PORT, help="Target port (default: 53317)")

    # Serve-peer command
    srv_p = subparsers.add_parser("serve-peer", help="Run mock LocalSend PC peer server on LAN")
    srv_p.add_argument("--bind", default="0.0.0.0", help="Bind IP address")
    srv_p.add_argument("--port", type=int, default=DEFAULT_PORT, help="Bind port (default: 53317)")
    srv_p.add_argument("--storage-dir", default="/tmp/localsend_mock_storage", help="Directory to save received files")

    # Send command
    snd_p = subparsers.add_parser("send", help="Send a file to target LocalSend device")
    snd_p.add_argument("--target", required=True, help="Target device IP")
    snd_p.add_argument("--port", type=int, default=DEFAULT_PORT, help="Target port (default: 53317)")
    snd_p.add_argument("--file", required=True, help="File path to send")

    args = parser.parse_args()
    if args.command == "fixtures":
        generate_fixtures(args.output, args.include_100m)
    elif args.command == "check-http":
        ok = check_http(args.host, args.port)
        sys.exit(0 if ok else 1)
    elif args.command == "serve-peer":
        serve_peer(args.bind, args.port, args.storage_dir)
    elif args.command == "send":
        ok = send_file(args.target, args.port, args.file)
        sys.exit(0 if ok else 1)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
