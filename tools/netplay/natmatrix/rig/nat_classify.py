#!/usr/bin/env python3
"""RFC 4787 NAT behaviour classifier for the S8 netns rig.

This exists because the iptables rule sets in natns.sh are only a *claim* about
what NAT type is being emulated. This measures what the kernel actually does, so
every matrix cell can be reported with its OBSERVED type. If the observed type
does not match the declared one, the cell's result is untrustworthy and is
flagged rather than silently reported.

Method
------
Mapping behaviour: from ONE fixed local port, probe three distinct server
endpoints -- (IP1,P1), (IP1,P2), (IP2,P1) -- and compare the external source
port the server observes for each.
    all three equal                      -> endpoint-independent mapping (EIM)
    differs when only the port changed   -> port-dependent mapping   (symmetric)
    differs when only the address changed-> address-dependent mapping (symmetric)

Filtering behaviour: from a FRESH local port, send to (IP1,P1) only, then have
the server reflect unsolicited datagrams back to the observed external endpoint.
    (IP3,P1) arrives -> endpoint-independent filtering  -> full cone
    (IP1,P2) arrives -> address-dependent filtering     -> address-restricted
    neither arrives  -> address-and-port-dependent      -> port-restricted

IP3 is a THIRD server address that the mapping phase never contacts, and it is
only ever a reflection SOURCE. Using IP2 here silently mismeasures an
address-restricted NAT as a full cone: the mapping phase already sent to IP2, so
an address-restricted NAT is entitled to admit IP2's unsolicited reply, and the
filtering verdict is contaminated by the earlier probe. Observed on this rig --
declared addr-restricted measured as fullcone until IP3 was introduced.

Roles:
    nat_classify.py observer --ip1 A --ip2 B --ip3 C --p1 N --p2 M
    nat_classify.py prober   --ip1 A --ip2 B --ip3 C --p1 N --p2 M [--json]
"""
import argparse, json, socket, sys, time

MAGIC = b"S8NC"


def _sock(bind_ip, bind_port, timeout=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((bind_ip, bind_port))
    if timeout is not None:
        s.settimeout(timeout)
    return s


def run_observer(a):
    """Four sockets. (ip3,p1) is reflection-only -- never a probe destination."""
    socks = {
        "11": _sock(a.ip1, a.p1),
        "12": _sock(a.ip1, a.p2),
        "21": _sock(a.ip2, a.p1),
        "31": _sock(a.ip3, a.p1),
    }
    import selectors
    sel = selectors.DefaultSelector()
    for tag, s in socks.items():
        sel.register(s, selectors.EVENT_READ, tag)
    sys.stderr.write("observer ready\n")
    sys.stderr.flush()
    while True:
        for key, _ in sel.select(timeout=1.0):
            tag = key.data
            s = key.fileobj
            try:
                data, src = s.recvfrom(2048)
            except OSError:
                continue
            if not data.startswith(MAGIC):
                continue
            parts = data[len(MAGIC):].decode("ascii", "replace").split()
            cmd = parts[0] if parts else ""
            if cmd == "PROBE":
                # Report back, on the same socket, what external endpoint we saw.
                token = parts[1] if len(parts) > 1 else "-"
                s.sendto(
                    MAGIC + f"SEEN {token} {tag} {src[0]} {src[1]}".encode(), src
                )
            elif cmd == "REFLECT":
                # Unsolicited sends from all three sockets to the observed endpoint.
                # These are what the NAT's filtering rule will or will not admit.
                for rtag, rs in socks.items():
                    try:
                        rs.sendto(MAGIC + f"REFL {rtag}".encode(), src)
                    except OSError as e:
                        sys.stderr.write(f"reflect {rtag} failed: {e}\n")


def _probe_once(sock, dst, token, tries=6, wait=0.35):
    """Send PROBE, return (observed_ip, observed_port) or None."""
    for _ in range(tries):
        sock.sendto(MAGIC + f"PROBE {token}".encode(), dst)
        deadline = time.time() + wait
        while time.time() < deadline:
            sock.settimeout(max(0.01, deadline - time.time()))
            try:
                data, _src = sock.recvfrom(2048)
            except (socket.timeout, OSError):
                break
            if data.startswith(MAGIC + b"SEEN"):
                f = data[len(MAGIC):].decode().split()
                if len(f) >= 5 and f[1] == token:
                    return (f[3], int(f[4]))
    return None


def run_prober(a):
    res = {"mapping": None, "filtering": None, "nat_type": None, "detail": {}}

    # ---- mapping ----------------------------------------------------------
    m = _sock("0.0.0.0", a.map_port, timeout=0.5)
    e11 = _probe_once(m, (a.ip1, a.p1), "m11")
    e12 = _probe_once(m, (a.ip1, a.p2), "m12")
    e21 = _probe_once(m, (a.ip2, a.p1), "m21")
    m.close()
    res["detail"]["map_ip1p1"] = e11
    res["detail"]["map_ip1p2"] = e12
    res["detail"]["map_ip2p1"] = e21

    if e11 is None:
        res["mapping"] = "unreachable"
        res["nat_type"] = "unreachable"
        print(json.dumps(res) if a.json else res)
        return 3
    if e12 is None and e21 is None:
        # Server reachable on one endpoint only: cannot classify mapping.
        res["mapping"] = "indeterminate"
    elif e11 == e12 == e21:
        res["mapping"] = "endpoint-independent"
    else:
        res["mapping"] = "endpoint-dependent"

    # ---- filtering (fresh local port, single destination) -----------------
    f = _sock("0.0.0.0", a.filt_port, timeout=0.5)
    ext = _probe_once(f, (a.ip1, a.p1), "f11")
    res["detail"]["filt_ext"] = ext
    got = set()
    if ext is not None:
        f.sendto(MAGIC + b"REFLECT", (a.ip1, a.p1))
        deadline = time.time() + a.reflect_wait
        while time.time() < deadline:
            f.settimeout(max(0.01, deadline - time.time()))
            try:
                data, _src = f.recvfrom(2048)
            except (socket.timeout, OSError):
                break
            if data.startswith(MAGIC + b"REFL"):
                got.add(data[len(MAGIC):].decode().split()[1])
    f.close()
    res["detail"]["reflect_received"] = sorted(got)

    if ext is None:
        res["filtering"] = "unreachable"
    elif "31" in got:
        res["filtering"] = "endpoint-independent"
    elif "12" in got:
        res["filtering"] = "address-dependent"
    elif "11" in got:
        res["filtering"] = "address-and-port-dependent"
    else:
        res["filtering"] = "no-reply-at-all"

    # ---- classify ---------------------------------------------------------
    if res["mapping"] == "endpoint-dependent":
        res["nat_type"] = "symmetric"
    elif res["mapping"] == "endpoint-independent":
        res["nat_type"] = {
            "endpoint-independent": "fullcone",
            "address-dependent": "addr-restricted",
            "address-and-port-dependent": "port-restricted",
        }.get(res["filtering"], "unknown")
    else:
        res["nat_type"] = "unknown"

    print(json.dumps(res, indent=None) if a.json else res)
    return 0 if res["nat_type"] not in ("unknown", "unreachable") else 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("role", choices=["observer", "prober"])
    ap.add_argument("--ip1", default="203.0.113.100")
    ap.add_argument("--ip2", default="203.0.113.101")
    ap.add_argument("--ip3", default="203.0.113.102")
    ap.add_argument("--p1", type=int, default=19301)
    ap.add_argument("--p2", type=int, default=19302)
    ap.add_argument("--map-port", type=int, default=7100)
    ap.add_argument("--filt-port", type=int, default=7101)
    ap.add_argument("--reflect-wait", type=float, default=2.0)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    return run_observer(a) if a.role == "observer" else run_prober(a)


if __name__ == "__main__":
    sys.exit(main())
