#!/usr/bin/env python3
"""
LocalSend BlackBerry 10 Interoperability & Fixture Test Suite
Conforms to docs/mvp-plan.md and docs/protocol-notes.md.
"""

import argparse
import hashlib
import json
import os
import sys
import urllib.request
import urllib.error
from http.server import HTTPServer, BaseHTTPRequestHandler

def generate_fixtures(output_dir: str):
    """Generate deterministic test fixtures and manifest.json."""
    os.makedirs(output_dir, exist_ok=True)
    print(f"Generating fixtures in {output_dir}...")

    fixtures = [
        ("0b.bin", 0, b""),
        ("1k.bin", 1024, bytes([i % 256 for i in range(1024)])),
        ("测试 文件.bin", 1024, bytes([(i * 7) % 256 for i in range(1024)])),
    ]

    manifest = {}

    for fname, size, content in fixtures:
        path = os.path.join(output_dir, fname)
        with open(path, "wb") as f:
            f.write(content)
        h = hashlib.sha256(content).hexdigest()
        manifest[fname] = {
            "size": size,
            "sha256": h,
        }
        print(f"  [+] {fname} ({size} B) -> sha256: {h}")

    # Generate 10 MiB deterministic fixture: exactly 10,485,760 bytes
    ten_mib_name = "10m.bin"
    ten_mib_size = 10485760
    ten_mib_path = os.path.join(output_dir, ten_mib_name)
    hasher = hashlib.sha256()

    print(f"  [+] Generating {ten_mib_name} ({ten_mib_size} bytes)...")
    chunk_pattern = bytes([i % 256 for i in range(65536)]) # 64 KiB pattern
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
    print(f"  [+] {ten_mib_name} complete -> sha256: {ten_mib_hash}")

    manifest_path = os.path.join(output_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
    print(f"Manifest saved to {manifest_path}")

def check_http(host: str, port: int):
    """Perform baseline sanity checks against a running LocalSend HTTP server."""
    base_url = f"http://{host}:{port}"
    print(f"Testing LocalSend HTTP endpoints at {base_url}...")

    # 1. GET /api/localsend/v2/info
    try:
        url = f"{base_url}/api/localsend/v2/info"
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=5) as resp:
            status = resp.status
            body = resp.read()
            print(f"  [INFO] GET /info -> {status}")
            data = json.loads(body.decode("utf-8"))
            print(f"         Device alias: {data.get('alias')}, version: {data.get('version')}")
    except Exception as e:
        print(f"  [WARN] GET /info request failed: {e}")

    # 2. POST /api/localsend/v2/cancel?sessionId=test-unknown (idempotent 200 expected)
    try:
        url = f"{base_url}/api/localsend/v2/cancel?sessionId=test-unknown"
        req = urllib.request.Request(url, data=b"", headers={"Content-Length": "0"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as resp:
            print(f"  [CANCEL] POST /cancel -> {resp.status} (expected 200)")
    except Exception as e:
        print(f"  [WARN] POST /cancel request failed: {e}")

    # 3. POST /api/localsend/v2/upload without query parameters (expected 400)
    try:
        url = f"{base_url}/api/localsend/v2/upload"
        req = urllib.request.Request(url, data=b"hello", headers={"Content-Type": "application/octet-stream"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as resp:
            print(f"  [UPLOAD] Unexpected success: {resp.status}")
    except urllib.error.HTTPError as e:
        print(f"  [UPLOAD] Missing query params -> {e.code} (expected 400)")
    except Exception as e:
        print(f"  [WARN] Upload error: {e}")

class PeerRequestHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        content_len = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_len) if content_len > 0 else b""
        print(f"[PEER] {self.command} {self.path} ({len(body)} bytes)")

        if self.path == "/api/localsend/v2/register":
            resp = {
                "alias": "Mock Peer",
                "version": "2.2",
                "deviceModel": "Python Test Peer",
                "deviceType": "desktop",
                "fingerprint": "mock-peer-fp",
                "download": False
            }
            res_bytes = json.dumps(resp).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(res_bytes)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(res_bytes)
        else:
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()

def serve_peer(bind_ip: str, port: int):
    """Run mock LocalSend peer server for development testing."""
    server = HTTPServer((bind_ip, port), PeerRequestHandler)
    print(f"Mock peer listening on http://{bind_ip}:{port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping mock peer.")

def main():
    parser = argparse.ArgumentParser(description="LocalSend BB10 Interoperability Suite")
    subparsers = parser.add_subparsers(dest="command")

    # Fixtures command
    fix_p = subparsers.add_parser("fixtures", help="Generate deterministic test fixtures")
    fix_p.add_argument("--output", default="/tmp/localsend-bb10-fixtures", help="Output directory")

    # Check-http command
    chk_p = subparsers.add_parser("check-http", help="Check HTTP server endpoints")
    chk_p.add_argument("--host", required=True, help="Target host")
    chk_p.add_argument("--port", type=int, default=53317, help="Target port")

    # Serve-peer command
    srv_p = subparsers.add_parser("serve-peer", help="Run mock peer server")
    srv_p.add_argument("--bind", default="0.0.0.0", help="Bind IP")
    srv_p.add_argument("--port", type=int, default=53317, help="Port")

    args = parser.parse_args()
    if args.command == "fixtures":
        generate_fixtures(args.output)
    elif args.command == "check-http":
        check_http(args.host, args.port)
    elif args.command == "serve-peer":
        serve_peer(args.bind, args.port)
    else:
        parser.print_help()
        sys.exit(1)

if __name__ == "__main__":
    main()
