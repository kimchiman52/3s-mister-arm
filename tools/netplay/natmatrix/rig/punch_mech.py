#!/usr/bin/env python3
"""punch_mech.py -- wire-level mechanism probe for the S8 NAT matrix.

The full cascade (p2p_probe) answers "did this pairing connect". It cannot
answer "was the datagram admitted by the NAT", because a failure there is
indistinguishable from a failure in the state machine. This probe strips the
cascade down to exactly the datagrams the cascade puts on the wire, in exactly
the order the cascade puts them there, and reports which ones ARRIVED.

Cascade order being modelled (src/netplay/direct_p2p.c):
  1. Both peers learn their own public endpoint from the server (STUN).
     -> the HOST's endpoint becomes the room code (room_code.c), so the joiner
        holds it before anyone has punched.
     -> the JOINER's endpoint is what the rendezvous server DELIVERs to the
        host (tools/rendezvous-server/rendezvous-server.js:1077 builds the
        endpoint from rinfo, the SERVER-OBSERVED source, not the client's
        claim; :1252 is the unsolicited push that carries it).
  2. The joiner punches the host's endpoint immediately (race leg 0,
     direct_p2p.c:1734).
  3. The host punches the joiner's DELIVERed endpoint about one signalling
     round trip later (direct_p2p.c:2068, race_arm_punch leg 1, armed the
        instant the DELIVER parses).

Roles:
    punch_mech.py observer --bind IP:PORT [--bind IP:PORT ...]
    punch_mech.py peer --name NAME --bind-port N --srv IP:PORT \
                       --ext-out FILE [--peer-ext-file FILE] \
                       --start-delay-ms N --duration-ms N --json
"""
import argparse, json, os, socket, sys, time

MAGIC = b"S8PM"
PUNCH = MAGIC + b"PUNCH"


def _now_ms():
    return int(time.monotonic() * 1000)


def run_observer(a):
    import selectors
    socks = []
    for spec in a.bind:
        ip, _, port = spec.rpartition(":")
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((ip, int(port)))
        socks.append(s)
    sel = selectors.DefaultSelector()
    for s in socks:
        sel.register(s, selectors.EVENT_READ)
    sys.stderr.write("observer ready\n")
    sys.stderr.flush()
    while True:
        for key, _ in sel.select(timeout=1.0):
            s = key.fileobj
            try:
                data, src = s.recvfrom(2048)
            except OSError:
                continue
            if data.startswith(MAGIC + b"PROBE"):
                s.sendto(MAGIC + f"SEEN {src[0]} {src[1]}".encode(), src)


def _discover(sock, srv, tries=8, wait=0.35):
    for _ in range(tries):
        sock.sendto(MAGIC + b"PROBE", srv)
        deadline = time.time() + wait
        while time.time() < deadline:
            sock.settimeout(max(0.01, deadline - time.time()))
            try:
                data, _src = sock.recvfrom(2048)
            except (socket.timeout, OSError):
                break
            if data.startswith(MAGIC + b"SEEN"):
                f = data[len(MAGIC):].decode().split()
                return (f[1], int(f[2]))
    return None


def _read_endpoint(path, deadline):
    while time.time() < deadline:
        try:
            with open(path) as fh:
                txt = fh.read().strip()
            if txt:
                ip, _, port = txt.rpartition(":")
                return (ip, int(port))
        except OSError:
            pass
        time.sleep(0.05)
    return None


def run_peer(a):
    srv_ip, _, srv_port = a.srv.rpartition(":")
    srv = (srv_ip, int(srv_port))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", a.bind_port))
    local_port = sock.getsockname()[1]

    out = {"name": a.name, "ran": True, "local_port": local_port}

    ext = _discover(sock, srv)
    out["ext"] = f"{ext[0]}:{ext[1]}" if ext else None
    if ext is None:
        out["error"] = "no server reply -- cannot learn own public endpoint"
        print(json.dumps(out))
        return 20

    tmp = a.ext_out + ".tmp"
    with open(tmp, "w") as fh:
        fh.write(f"{ext[0]}:{ext[1]}\n")
    os.replace(tmp, a.ext_out)

    deadline = time.time() + a.peer_wait_s
    peer = _read_endpoint(a.peer_ext_file, deadline)
    out["target"] = f"{peer[0]}:{peer[1]}" if peer else None
    if peer is None:
        out["error"] = "peer never published an endpoint"
        print(json.dumps(out))
        return 20

    t0 = _now_ms()
    sent = 0
    recv = []
    first_from_target_ms = None
    sock.settimeout(0.01)
    last_send = -10**9
    while True:
        now = _now_ms()
        el = now - t0
        if el > a.start_delay_ms + a.duration_ms:
            break
        if el >= a.start_delay_ms and (now - last_send) >= a.interval_ms:
            try:
                sock.sendto(PUNCH + f" {a.name} {sent}".encode(), peer)
                sent += 1
            except OSError as e:
                out.setdefault("send_errors", []).append(f"{el}:{e}")
            last_send = now
        try:
            data, src = sock.recvfrom(2048)
        except (socket.timeout, OSError):
            continue
        rec = {"ms": el, "from": f"{src[0]}:{src[1]}",
               "punch": data.startswith(PUNCH)}
        if len(recv) < 40:
            recv.append(rec)
        if data.startswith(PUNCH) and src[0] == peer[0] and \
                first_from_target_ms is None:
            first_from_target_ms = el
            out["first_punch_src"] = f"{src[0]}:{src[1]}"
            out["first_punch_src_port_matches_target"] = (src[1] == peer[1])

    out["sent"] = sent
    out["recv_sample"] = recv
    out["recv_total"] = len(recv)
    out["first_punch_from_target_ms"] = first_from_target_ms
    out["heard_peer"] = first_from_target_ms is not None
    print(json.dumps(out))
    return 0 if out["heard_peer"] else 10


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="role", required=True)

    o = sub.add_parser("observer")
    o.add_argument("--bind", action="append", required=True)

    p = sub.add_parser("peer")
    p.add_argument("--name", required=True)
    p.add_argument("--bind-port", type=int, default=0)
    p.add_argument("--srv", required=True)
    p.add_argument("--ext-out", required=True)
    p.add_argument("--peer-ext-file", required=True)
    p.add_argument("--peer-wait-s", type=float, default=20.0)
    p.add_argument("--start-delay-ms", type=int, default=0)
    p.add_argument("--duration-ms", type=int, default=5000)
    p.add_argument("--interval-ms", type=int, default=50)

    a = ap.parse_args()
    return run_observer(a) if a.role == "observer" else run_peer(a)


if __name__ == "__main__":
    sys.exit(main())
