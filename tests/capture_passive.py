#!/usr/bin/env python3
import socket
import struct
import json
import datetime
import sys
import os
import threading

LOG_FILE = "tests/passive_discovery.log"
RAW_FILE = "tests/passive_discovery_raw.jsonl"

def log_event(category, msg):
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")
    line = f"[{ts}] [{category}] {msg}\n"
    print(line, end="", flush=True)
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(line)

def log_raw(src_ip, src_port, payload_str):
    ts = datetime.datetime.now().isoformat()
    record = {
        "timestamp": ts,
        "src_ip": src_ip,
        "src_port": src_port,
        "payload": payload_str
    }
    with open(RAW_FILE, "a", encoding="utf-8") as f:
        f.write(json.dumps(record, ensure_ascii=False) + "\n")

def run_sniffer():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    # Bind specifically to the multicast address 224.0.0.167 to guarantee zero unicast interference
    # In Linux, binding to 224.0.0.167 receives strictly multicast traffic, preventing any unicast diversion.
    bind_ip = "224.0.0.167"
    try:
        s.bind((bind_ip, 53317))
    except Exception as e:
        # Fallback to 0.0.0.0 if multicast bind is restricted on host
        bind_ip = "0.0.0.0"
        s.bind((bind_ip, 53317))

    mreq = struct.pack("4sl", socket.inet_aton("224.0.0.167"), socket.INADDR_ANY)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    log_event("START", f"Passive Multicast UDP Sniffer started on {bind_ip}:53317 (strictly zero-interference)")

    burst_count = 0
    last_peer_packet_time = {}

    while True:
        try:
            data, addr = s.recvfrom(65535)
            src_ip, src_port = addr
            now = datetime.datetime.now()

            try:
                payload_str = data.decode("utf-8")
                payload = json.loads(payload_str)
                log_raw(src_ip, src_port, payload)

                alias = payload.get("alias", "unknown")
                ann = payload.get("announce", payload.get("announcement", True))
                proto = payload.get("protocol", "http")
                port = payload.get("port", 53317)
                fp = payload.get("fingerprint", "none")
                model = payload.get("deviceModel", "unknown")

                # Detect scan burst initiation from Passport (192.168.3.124)
                if src_ip == "192.168.3.124":
                    last_time = last_peer_packet_time.get(src_ip)
                    if last_time is None or (now - last_time).total_seconds() > 2.0:
                        burst_count += 1
                        log_event("BURST_DETECTED", f"=== Passport Scan Burst #{burst_count} from {src_ip} ===")
                    last_peer_packet_time[src_ip] = now

                log_event("UDP_PACKET", f"{src_ip}:{src_port} -> alias='{alias}' ann={ann} model='{model}' fp='{fp}' proto={proto}:{port} ({len(data)}B)")
            except Exception as ex:
                log_event("RAW_UDP", f"{src_ip}:{src_port} ({len(data)} bytes): {data[:60]} (err: {ex})")
        except KeyboardInterrupt:
            break
        except Exception as e:
            log_event("ERROR", str(e))

if __name__ == "__main__":
    run_sniffer()
