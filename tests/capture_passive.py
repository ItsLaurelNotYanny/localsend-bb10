#!/usr/bin/env python3
import socket
import struct
import json
import datetime
import sys

LOG_FILE = "tests/passive_discovery.log"

def log_event(category, msg):
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")
    line = f"[{ts}] [{category}] {msg}\n"
    print(line, end="", flush=True)
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(line)

def run_sniffer():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 53317))

    mreq = struct.pack("4sl", socket.inet_aton("224.0.0.167"), socket.INADDR_ANY)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    log_event("START", "Passive UDP Discovery Sniffer started on 0.0.0.0:53317 (non-interfering)")

    scan_round = 0
    last_peer_packet_time = {}

    while True:
        try:
            data, addr = s.recvfrom(65535)
            src_ip, src_port = addr
            now = datetime.datetime.now()

            try:
                payload = json.loads(data.decode("utf-8"))
                alias = payload.get("alias", "unknown")
                ann = payload.get("announcement", False)
                proto = payload.get("protocol", "http")
                port = payload.get("port", 53317)
                fp = payload.get("fingerprint", "none")[:8]
                model = payload.get("deviceModel", "unknown")

                # Detect scan burst initiation from Passport
                if src_ip == "192.168.3.124":
                    last_time = last_peer_packet_time.get(src_ip)
                    if last_time is None or (now - last_time).total_seconds() > 2.0:
                        scan_round += 1
                        log_event("SCAN_BURST", f"=== Passport Scan Round #{scan_round} Detected ===")
                    last_peer_packet_time[src_ip] = now

                log_event("UDP_PACKET", f"{src_ip}:{src_port} -> alias='{alias}' ann={ann} model='{model}' fp={fp} proto={proto}:{port} ({len(data)}B)")
            except Exception:
                log_event("RAW_UDP", f"{src_ip}:{src_port} ({len(data)} bytes): {data[:60]}")
        except KeyboardInterrupt:
            break
        except Exception as e:
            log_event("ERROR", str(e))

if __name__ == "__main__":
    run_sniffer()
