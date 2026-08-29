#!/usr/bin/env python3
"""Minimal RFC 5389 STUN Binding responder for the S8 netns rig.

Why this exists: the production STUN server list is HARDCODED at
src/netplay/stun.c:280-285 with no config or env override. The only redirection
mechanism is Stun_TestHook_SetServers() (src/netplay/stun.h:247), which p2p_probe
calls. The netns has no route to the internet, so we serve STUN locally.

It binds SEVERAL endpoints on purpose. src/netplay/stun.c probes every server in
parallel from one socket and sets StunResult.port_disagreement when two servers
report DIFFERENT mapped ports -- which is exactly what a symmetric NAT produces,
since each distinct destination gets its own external port. Serving only one
endpoint would make symmetric NAT look like a cone NAT to the client and would
silently destroy the most important column of the matrix.

Responds to Binding Requests (type 0x0001) with a Binding Response (0x0101)
carrying XOR-MAPPED-ADDRESS (0x0020).
"""
import argparse, selectors, socket, struct, sys

MAGIC_COOKIE = 0x2112A442
BINDING_REQUEST = 0x0001
BINDING_RESPONSE = 0x0101
ATTR_XOR_MAPPED_ADDRESS = 0x0020


def build_binding_response(txid: bytes, ip: str, port: int) -> bytes:
    xport = port ^ (MAGIC_COOKIE >> 16)
    addr = struct.unpack("!I", socket.inet_aton(ip))[0]
    xaddr = addr ^ MAGIC_COOKIE
    # reserved(1) | family(1)=IPv4 | x-port(2) | x-address(4)
    value = struct.pack("!BBHI", 0, 0x01, xport, xaddr)
    attr = struct.pack("!HH", ATTR_XOR_MAPPED_ADDRESS, len(value)) + value
    header = struct.pack("!HHI", BINDING_RESPONSE, len(attr), MAGIC_COOKIE) + txid
    return header + attr


def parse_binding_request(data: bytes):
    """Return the 12-byte transaction id, or None if not a Binding Request."""
    if len(data) < 20:
        return None
    mtype, mlen, cookie = struct.unpack("!HHI", data[:8])
    if mtype != BINDING_REQUEST or cookie != MAGIC_COOKIE:
        return None
    return data[8:20]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--bind", action="append", required=True,
        help="ip:port to serve STUN on; repeat for multiple servers",
    )
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    sel = selectors.DefaultSelector()
    for spec in a.bind:
        ip, _, port = spec.rpartition(":")
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((ip, int(port)))
        sel.register(s, selectors.EVENT_READ, spec)

    sys.stderr.write("stun_mock ready on %s\n" % ", ".join(a.bind))
    sys.stderr.flush()

    while True:
        for key, _ in sel.select(timeout=1.0):
            s = key.fileobj
            try:
                data, src = s.recvfrom(2048)
            except OSError:
                continue
            txid = parse_binding_request(data)
            if txid is None:
                continue
            try:
                s.sendto(build_binding_response(txid, src[0], src[1]), src)
            except OSError as e:
                sys.stderr.write("stun_mock send failed: %s\n" % e)
                continue
            if a.verbose:
                sys.stderr.write("stun_mock %s <- %s:%d\n" % (key.data, src[0], src[1]))
                sys.stderr.flush()


if __name__ == "__main__":
    main()
