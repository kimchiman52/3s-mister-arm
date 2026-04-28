#!/usr/bin/env python3
import socket, sys, time

if len(sys.argv) != 3:
    print("usage: fake-peer.py <host-public-ip> <host-public-port>", file=sys.stderr)
    sys.exit(2)

host_ip, host_port = sys.argv[1], int(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
print(f"[fake-peer] bound to 0.0.0.0:{s.getsockname()[1]}")

probe = b"3SX_FAKE_PEER_PROBE"
s.sendto(probe, (host_ip, host_port))
print(f"[fake-peer] sent probe to {host_ip}:{host_port}")

s.settimeout(30.0)
start = time.time()
while time.time() - start < 30:
    try:
        data, addr = s.recvfrom(2048)
        print(f"[fake-peer] echo {len(data)}B from {addr}")
        s.sendto(data, addr)
    except socket.timeout:
        break
