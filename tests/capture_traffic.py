#!/usr/bin/env python3
import socket
import struct
import threading
import json
import time
import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler

LOG_FILE = "tests/discovery_capture.log"

def log_event(category, msg):
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    line = f"[{ts}] [{category}] {msg}\n"
    print(line, end="", flush=True)
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(line)

class PeerHttpHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass # Suppress default logging

    def do_GET(self):
        log_event("HTTP_GET", f"From {self.client_address[0]}:{self.client_address[1]} -> {self.path}")
        if self.path in ["/api/localsend/v2/info", "/api/localsend/v1/info"]:
            resp = {
                "alias": "Fedora PC",
                "version": "2.2",
                "deviceModel": "Fedora Linux",
                "deviceType": "desktop",
                "fingerprint": "fedora-pc-fingerprint",
                "port": 53317,
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
            log_event("HTTP_RESP", f"Returned 200 info to {self.client_address[0]}")
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        content_len = int(self.headers.get("Content-Length", 0))
        req_body = self.rfile.read(content_len) if content_len > 0 else b""
        log_event("HTTP_POST", f"From {self.client_address[0]}:{self.client_address[1]} -> {self.path} ({len(req_body)} bytes)")
        
        try:
            parsed = json.loads(req_body.decode("utf-8"))
            log_event("HTTP_BODY", f"Payload: {parsed}")
        except Exception as e:
            log_event("HTTP_BODY", f"Raw: {req_body[:120]}")

        if self.path == "/api/localsend/v2/register":
            resp = {
                "alias": "Fedora PC",
                "version": "2.2",
                "deviceModel": "Fedora Linux",
                "deviceType": "desktop",
                "fingerprint": "fedora-pc-fingerprint",
                "port": 53317,
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
            log_event("HTTP_RESP", f"Returned 200 register confirmation to {self.client_address[0]}")
        else:
            self.send_response(200)
            self.end_headers()

def run_udp_listener():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except Exception:
        pass
    s.bind(('', 53317))

    # Join multicast 224.0.0.167
    try:
        mreq = struct.pack('4sl', socket.inet_aton('224.0.0.167'), socket.INADDR_ANY)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        log_event("UDP_INIT", "Joined multicast group 224.0.0.167 on 0.0.0.0:53317")
    except Exception as e:
        log_event("UDP_INIT", f"Multicast join error: {e}")

    log_event("UDP_INIT", "Listening for UDP datagrams on port 53317...")
    while True:
        try:
            data, addr = s.recvfrom(65535)
            try:
                parsed = json.loads(data.decode("utf-8"))
                alias = parsed.get("alias", "<unknown>")
                fp = parsed.get("fingerprint", "<none>")
                ann = parsed.get("announce", False)
                proto = parsed.get("protocol", "http")
                port = parsed.get("port", 53317)
                log_event("UDP_RECV", f"From {addr[0]}:{addr[1]} ({len(data)} B): alias='{alias}' ann={ann} fp={fp[:8]}... proto={proto}:{port}")
            except Exception:
                log_event("UDP_RECV", f"From {addr[0]}:{addr[1]} ({len(data)} B): raw={data[:80]}")
        except Exception as e:
            log_event("UDP_ERR", f"Error: {e}")

def main():
    with open(LOG_FILE, "w", encoding="utf-8") as f:
        f.write(f"=== LocalSend Discovery Packet & Request Capture Started {datetime.datetime.now()} ===\n")
    log_event("START", "Capture logger active on Fedora (192.168.3.123)")

    udp_t = threading.Thread(target=run_udp_listener, daemon=True)
    udp_t.start()

    log_event("HTTP_INIT", "Starting HTTP server on 0.0.0.0:53317...")
    httpd = HTTPServer(('0.0.0.0', 53317), PeerHttpHandler)
    httpd.serve_forever()

if __name__ == '__main__':
    main()
