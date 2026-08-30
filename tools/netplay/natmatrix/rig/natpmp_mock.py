#!/usr/bin/env python3
"""NAT-PMP (RFC 6886) / PCP (RFC 6887) gateway mock for the S8 netns rig.

Why this exists
---------------
Task #121 gives the netplay JOINER a port-mapping probe. A symmetric joiner
that holds an EXPLICIT port mapping stops being symmetric for that port, which
is what converts the (symmetric joiner x port-restricted host) cell. Until now
the rig had no gateway at all, so probe/p2p_probe.c had to write
``netplay-direct-p2p-disable-natpmp = true`` (p2p_probe.c:124) into every cell's
config. This daemon is that gateway.

The seam it plugs into is REAL. src/netplay/natpmp.c:757-763 consults the
``s_hook_gw_ip`` test hook (Natpmp_TestHook_SetGateway, natpmp.h:345) BEFORE the
ENABLE_NETPLAY_TESTS "refuse to touch the real default route" block at :764-785,
so pointing the hook at this daemon makes the PRODUCTION client emit real
NAT-PMP/PCP datagrams at it. Nothing in src/ is modified or reimplemented here;
this is only the box on the other end of the wire.

There is no equivalent hook on the UPnP side (miniupnpc discovers by SSDP
multicast), so UPnP stays disabled in the rig.

Wire format
-----------
Every field below is pinned against the PRODUCTION PARSER in
src/netplay/natpmp.c, not against the RFC text alone -- where the two could be
read differently, the parser wins, because the parser is what the rig has to
satisfy. Citations are to that file.

  * PCP downgrade (default --mode natpmp). natpmp.c:152-170 tests
    ``buf[0] == 0 && buf[3] == NATPMP_PCP_UNSUPP_VERSION`` FIRST, deliberately
    before the R-bit test, and returns NATPMP_PARSE_PCP_IS_NATPMP. The frame we
    emit is RFC 6886 sec 3.5's 8-byte "Unsupported Version" reply --
    Vers=0, OP=0, Result Code=1 (u16), Epoch (u32) -- which satisfies exactly
    that test. Note the minimum length the parser demands for this branch is
    4 bytes (Natpmp_ParsePcpMapResponse, natpmp.c:140-148), so 8 is comfortably
    legal.
  * PCP MAP grant (--mode pcp). 60-byte response; the parser at
    natpmp.c:183-269 requires: len >= 24, <= 1100, len %% 4 == 0; buf[0] == 2;
    R bit set and opcode 1 (so buf[1] == 0x81); nonce echoed at [24:36];
    protocol echoed at [36]; internal port echoed at [40:42]; and the Assigned
    External IP at [44:60] MUST be a full IPv4-mapped IPv6 address
    (get_v4_mapped, natpmp.c:93-104) or the frame is discarded. Lifetime is at
    [4:8], Epoch at [8:12], assigned external port at [42:44].
    natpmp.c:1190-1213 additionally refuses a grant whose external_ip_be is
    zero: the accept branch is guarded on it at :1191 and the else-if at
    :1201-1213 logs the refusal and gives up on PCP.
  * NAT-PMP public address (op 0). 12-byte response,
    Natpmp_ParsePmpAddrResponse at natpmp.c:286-313:
    buf[0] == 0, buf[1] == 128 + 0, result u16 at [2:4], epoch u32 at [4:8],
    external IPv4 at [8:12]. natpmp.c:1255-1264 refuses a success whose
    external_ip_be is 0.0.0.0, so --external-ip must be a real address.
  * NAT-PMP mapping (op 1 UDP / op 2 TCP). 16-byte response,
    Natpmp_ParsePmpMapResponse at natpmp.c:342-385:
    buf[0] == 0, buf[1] == 128 + the opcode the client sent, result u16 at
    [2:4], epoch u32 at [4:8], INTERNAL port echoed at [8:10] and compared
    against req_internal_port -- the only request correlator NAT-PMP has
    (natpmp.c:361-369) -- mapped external port at
    [10:12], granted lifetime u32 at [12:16]. natpmp.c:1288-1289 refuses a
    success whose external_port is zero.

Forwarding rules
----------------
A granted mapping is not just a datagram; it has to change what the NAT
namespace actually does. On every grant we install BOTH directions with -I (head
of chain) so they take precedence over the dynamic rules `apply_nat()`
(tools/netplay/natmatrix/natns.sh:89-194) installs for the declared NAT type:

  inbound   -t nat -I PREROUTING  -i WAN_IF -p PROTO --dport P_ext \\
                   -j DNAT --to-destination INNER:P_int
  outbound  -t nat -I POSTROUTING -o WAN_IF -p PROTO -s INNER --sport P_int \\
                   -j SNAT --to-source EXT_IP:P_ext

The OUTBOUND rule is the whole point for the symmetric cell. The `symmetric)`
arm (natns.sh:143-148) emulates it with ``MASQUERADE --random-fully``, picking a
random external source port per destination tuple; a port-restricted host then
rejects the reply because it never sent to that port. The SNAT rule pins the
external port for that internal port regardless of destination, i.e. the mapping
becomes endpoint-INDEPENDENT for the mapped port only. That is REAL ROUTER
BEHAVIOUR, not a rig cheat: a static/explicit mapping takes precedence over the
dynamic translation pool on every consumer NAT that implements NAT-PMP or PCP --
RFC 6887 sec 11.1 defines a MAP mapping as giving the client "an explicit
mapping" from an external port to its internal one, and RFC 4787 REQ-1's
endpoint-independent-mapping requirement is exactly what an explicit mapping
provides for the port it covers. The rest of the NAT stays symmetric, which is
also what a real router does.

Conntrack
---------
Installing nat rules does NOT retranslate flows that already have a conntrack
entry -- the nat table is consulted once per conntrack entry, on its first
packet. So a mapping installed after the client has already spoken from that
internal port would be inert for those flows. We therefore flush conntrack
entries for the internal endpoint on install (and on teardown). See the VERIFIED
note in flush_conntrack() for the measurement that shows this is load-bearing.

The flush needs conntrack(8), which is an OPTIONAL dependency: it is not in a
base debian image (it is packaged separately, as conntrack). Its absence is
detected once at startup and reported as ``conntrack=absent`` in the ready line
plus one explicit skip message per mapping; it is never allowed to surface as an
exec failure, because a healthy run that merely lacks an optional tool must not
read as a broken one in the log. The flush is not load-bearing for the datagram
protocol -- only for retranslating flows that predate a mapping -- so the rest
of the daemon is unaffected.

Usage
-----
  natpmp_mock.py --listen 10.1.0.1 --external-ip 203.0.113.10 \\
                 --inner-ip 10.1.0.2 --wan-if nAo [--log FILE]

Run it INSIDE the NAT namespace (natns.sh portmap A up does this for you).
Python 3 stdlib only, no third-party imports -- same constraint as
rig/stun_mock.py.
"""

import argparse
import os
import random
import selectors
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time

# --- wire constants, mirroring src/netplay/natpmp.h:39-110 -----------------
GATEWAY_PORT = 5351            # natpmp.h:43
PCP_VERSION = 2                # natpmp.h:47
PCP_OPCODE_MAP = 1             # natpmp.h:50
PCP_R_BIT = 0x80               # natpmp.h:53
PCP_NONCE_LEN = 12             # natpmp.h:55
PCP_HDR_LEN = 24               # natpmp.h:58
PCP_MAP_LEN = 60               # natpmp.h:59
PROTO_UDP = 17                 # natpmp.h:64
PROTO_TCP = 6

PMP_VERSION = 0                # natpmp.h:67
PMP_OP_PUBLIC_ADDR = 0         # natpmp.h:70
PMP_OP_MAP_UDP = 1             # natpmp.h:71
PMP_OP_MAP_TCP = 2             # natpmp.h:72
PMP_RESP_FLAG = 128            # natpmp.h:75
PMP_ADDR_REQ_LEN = 2           # natpmp.h:79
PMP_ADDR_RESP_LEN = 12         # natpmp.h:80
PMP_MAP_REQ_LEN = 12           # natpmp.h:81
PMP_MAP_RESP_LEN = 16          # natpmp.h:82

# PCP result codes, natpmp.h:85-100
PCP_SUCCESS = 0
PCP_UNSUPP_VERSION = 1
PCP_MALFORMED_REQUEST = 3
PCP_UNSUPP_OPCODE = 4
PCP_NETWORK_FAILURE = 7
PCP_NO_RESOURCES = 8
PCP_UNSUPP_PROTOCOL = 9
PCP_ADDRESS_MISMATCH = 12

# NAT-PMP result codes, natpmp.h:103-110
PMP_SUCCESS = 0
PMP_UNSUPPORTED_VERSION = 1
PMP_NOT_AUTHORIZED = 2
PMP_NETWORK_FAILURE = 3
PMP_OUT_OF_RESOURCES = 4
PMP_UNSUPPORTED_OPCODE = 5

# RFC 6886 sec 3.3 recommends the gateway not hand out ports below 1024 as
# "anonymous" external ports.
EPHEMERAL_LO = 10000
EPHEMERAL_HI = 60000

# How long one idle pass through the main loop blocks. It is also the mapping
# expiry-sweep cadence, and -- only when signal.set_wakeup_fd is unavailable --
# the worst-case delay between a stop signal and the start of teardown.
SELECT_TIMEOUT = 1.0


def v4_mapped(ip_str):
    """RFC 6887 sec 5 / put_v4_mapped, natpmp.c:83-88: ::ffff:0:0/96 + the 4
    IPv4 bytes.

    get_v4_mapped, natpmp.c:93-104 checks all 96 leading bits, so the 10 zero
    bytes and the two 0xFF bytes are both mandatory -- a lazy 12-zero-byte
    prefix would be silently discarded as NOT_OURS.
    """
    return b"\x00" * 10 + b"\xff\xff" + socket.inet_aton(ip_str)


class Mapping(object):
    __slots__ = ("proto", "in_ip", "in_port", "ext_port", "lifetime", "expires")

    def __init__(self, proto, in_ip, in_port, ext_port, lifetime, expires):
        self.proto = proto
        self.in_ip = in_ip
        self.in_port = in_port
        self.ext_port = ext_port
        self.lifetime = lifetime
        self.expires = expires


class Gateway(object):
    def __init__(self, args):
        self.listen = args.listen
        self.port = args.port
        self.ext_ip = args.external_ip
        self.inner_ip = args.inner_ip
        self.wan_if = args.wan_if
        self.mode = args.mode
        self.install_rules = not args.no_rules
        self.conntrack_flush = not args.no_conntrack_flush
        # OPTIONAL dependency, resolved ONCE at startup rather than per call.
        # conntrack(8) ships in its own package and is absent from a base
        # debian image; without this probe every flush attempted three execs
        # that each failed with ENOENT and logged "exec failed: conntrack ...",
        # which made a run that was working correctly look broken. None here
        # means "not installed"; a path means "use exactly this binary", so
        # behaviour where conntrack IS present is unchanged.
        self.conntrack_bin = shutil.which("conntrack")
        self.conntrack_absent_logged = False
        # Shutdown state. Written by request_stop() from signal-handler
        # context, read by serve()'s loop condition. Both are plain
        # attribute stores, which is all a Python signal handler may safely do.
        self.stopping = False
        self.stop_signal = 0
        self.lifetime_cap = args.lifetime_cap
        self.verbose = args.verbose
        self.logf = open(args.log, "a", buffering=1) if args.log else None
        self.start = time.monotonic()
        # key: (proto, in_ip, in_port) -> Mapping ; ext key: (proto, ext_port)
        self.by_inner = {}
        self.by_ext = {}
        self.rng = random.Random(args.seed) if args.seed is not None else random.Random()

    # --- logging -----------------------------------------------------------
    def log(self, msg):
        line = "natpmp_mock[%s]: %s\n" % (self.listen, msg)
        sys.stderr.write(line)
        sys.stderr.flush()
        if self.logf:
            self.logf.write("%.3f %s" % (time.time(), line))

    def vlog(self, msg):
        if self.verbose:
            self.log(msg)

    def epoch(self):
        """RFC 6886 sec 3.6 Seconds Since Start of Epoch. The client's reboot
        estimator (natpmp.c, np_epoch_observe) only ever complains if this goes
        BACKWARDS, so a monotonic counter from daemon start is correct and a
        restart of this daemon correctly looks like a router reboot."""
        return int(time.monotonic() - self.start) & 0xFFFFFFFF

    # --- iptables / conntrack ---------------------------------------------
    def run(self, argv):
        try:
            p = subprocess.run(argv, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        except OSError as e:
            self.log("exec failed: %s (%s)" % (" ".join(argv), e))
            return 127, str(e)
        out = p.stdout.decode("utf-8", "replace").strip()
        if p.returncode != 0:
            self.vlog("rc=%d %s :: %s" % (p.returncode, " ".join(argv), out))
        return p.returncode, out

    def proto_name(self, proto):
        return "tcp" if proto == PROTO_TCP else "udp"

    def rule_specs(self, m):
        pn = self.proto_name(m.proto)
        inbound = ["iptables", "-t", "nat", "PREROUTING",
                   "-i", self.wan_if, "-p", pn, "--dport", str(m.ext_port),
                   "-j", "DNAT",
                   "--to-destination", "%s:%d" % (m.in_ip, m.in_port)]
        outbound = ["iptables", "-t", "nat", "POSTROUTING",
                    "-o", self.wan_if, "-p", pn, "-s", m.in_ip,
                    "--sport", str(m.in_port), "-j", "SNAT",
                    "--to-source", "%s:%d" % (self.ext_ip, m.ext_port)]
        return inbound, outbound

    @staticmethod
    def _with_op(spec, op):
        # spec is [iptables, -t, nat, CHAIN, ...]; splice the -I/-D in front of
        # the chain name so one spec serves install and remove and the two can
        # never drift apart.
        return spec[:3] + [op, spec[3]] + spec[4:]

    def flush_conntrack(self, m):
        """Drop conntrack state for this internal endpoint.

        VERIFIED LOAD-BEARING (see the task report): the nat table is consulted
        only on the FIRST packet of a conntrack entry. If the client has already
        sent from in_port (it has -- STUN discovery runs before the port-mapping
        probe in the cascade), the existing entry carries the old MASQUERADE
        translation and keeps using it for that destination no matter what rules
        we insert. Without this flush the inserted SNAT rule is a no-op for
        every flow that predates it.

        Three selectors, because an entry can already occupy this mapping from
        either end and the three tuples are genuinely different:

          orig-src / orig-port-src   outbound flows the internal endpoint has
                                     already opened -- the case that actually
                                     bites, since STUN discovery runs from the
                                     game socket before the port-mapping probe.
          reply-dst / reply-port-dst a TRANSLATED flow whose reply arrives at
                                     ext_ip:ext_port (i.e. the external port is
                                     already handed out to someone else).
          orig-dst / orig-port-dst   an UNTRANSLATED inbound flow already
                                     addressed to ext_ip:ext_port. Measured, not
                                     assumed: a datagram that hit ext_ip:ext_port
                                     before the mapping existed leaves an entry
                                     with no NAT attached, and the DNAT rule is
                                     then never consulted for that peer -- the
                                     rule's packet counter stays at 0 while the
                                     traffic sails past it.

        --no-conntrack-flush turns all of it off. It exists as the CONTROL for
        the claim above: run the same scenario with and without it and the
        difference is the measurement, not an assertion.

        conntrack(8) itself is OPTIONAL. It is not present in a base debian
        image, and its absence is a property of the image, not a fault of this
        run -- so it is ANNOTATED once and skipped, never allowed to reach the
        log as three "exec failed" lines per mapping. The nat rules are still
        installed either way; what is lost is only the retranslation of flows
        that predate the mapping.
        """
        if not self.conntrack_flush:
            self.vlog("conntrack flush SKIPPED (--no-conntrack-flush) for %s:%d"
                      % (m.in_ip, m.in_port))
            return
        if self.conntrack_bin is None:
            msg = ("conntrack flush SKIPPED for %s:%d -- conntrack(8) is not "
                   "installed in this image. Rules ARE installed; only flows "
                   "that predate this mapping keep their old translation. "
                   "Install the 'conntrack' package for full fidelity."
                   % (m.in_ip, m.in_port))
            if not self.conntrack_absent_logged:
                # Once at full volume, so the annotation is visible in a
                # default run; after that quietly, so a long cell does not
                # bury its real output under a repeated environment note.
                self.conntrack_absent_logged = True
                self.log(msg)
            else:
                self.vlog(msg)
            return
        pn = self.proto_name(m.proto)
        self.run([self.conntrack_bin, "-D", "-p", pn,
                  "--orig-src", m.in_ip, "--orig-port-src", str(m.in_port)])
        self.run([self.conntrack_bin, "-D", "-p", pn,
                  "--reply-dst", self.ext_ip, "--reply-port-dst", str(m.ext_port)])
        self.run([self.conntrack_bin, "-D", "-p", pn,
                  "--orig-dst", self.ext_ip, "--orig-port-dst", str(m.ext_port)])

    def install(self, m):
        if not self.install_rules:
            return True
        inbound, outbound = self.rule_specs(m)
        # -I, not -A: natns.sh has already installed the dynamic rules for the
        # declared NAT type (MASQUERADE --random-fully for symmetric,
        # natns.sh:95-97), and the first matching rule in the nat table wins for
        # a new conntrack entry. An appended rule would never be reached.
        rc1, o1 = self.run(self._with_op(inbound, "-I"))
        rc2, o2 = self.run(self._with_op(outbound, "-I"))
        if rc1 != 0 or rc2 != 0:
            self.log("RULE INSTALL FAILED ext=%d int=%d: in(%d)%s out(%d)%s"
                     % (m.ext_port, m.in_port, rc1, o1, rc2, o2))
            if rc1 == 0:
                self.run(self._with_op(inbound, "-D"))
            if rc2 == 0:
                self.run(self._with_op(outbound, "-D"))
            return False
        self.flush_conntrack(m)
        self.log("rules installed: %s %s:%d <-> %s:%d (wan-if %s)"
                 % (self.proto_name(m.proto), self.ext_ip, m.ext_port,
                    m.in_ip, m.in_port, self.wan_if))
        return True

    def remove(self, m):
        if self.install_rules:
            inbound, outbound = self.rule_specs(m)
            self.run(self._with_op(inbound, "-D"))
            self.run(self._with_op(outbound, "-D"))
            self.flush_conntrack(m)
        self.log("rules removed: %s ext %d -> %s:%d"
                 % (self.proto_name(m.proto), m.ext_port, m.in_ip, m.in_port))

    # --- mapping table -----------------------------------------------------
    def sweep(self):
        now = time.monotonic()
        for key, m in list(self.by_inner.items()):
            if m.expires <= now:
                self.log("mapping expired: ext %d -> %s:%d" % (m.ext_port, m.in_ip, m.in_port))
                self.drop(key)

    def drop(self, key):
        m = self.by_inner.pop(key, None)
        if m is None:
            return
        self.by_ext.pop((m.proto, m.ext_port), None)
        self.remove(m)

    def pick_external(self, proto, in_ip, in_port, suggested):
        """RFC 6886 sec 3.3: honour the Suggested External Port when it is free;
        otherwise the gateway "SHOULD" try the internal port, then any free
        high port."""
        for cand in (suggested, in_port):
            if cand and (proto, cand) not in self.by_ext:
                return cand
        for _ in range(2048):
            cand = self.rng.randrange(EPHEMERAL_LO, EPHEMERAL_HI)
            if (proto, cand) not in self.by_ext:
                return cand
        return 0

    def grant(self, proto, in_ip, in_port, suggested, lifetime):
        """Returns (ext_port, granted_lifetime, err) with err None on success."""
        key = (proto, in_ip, in_port)
        now = time.monotonic()

        if lifetime == 0:
            # RFC 6886 sec 3.4 / RFC 6887 sec 11.1: lifetime 0 means delete.
            if key in self.by_inner:
                self.drop(key)
            return 0, 0, None

        granted = min(lifetime, self.lifetime_cap)
        existing = self.by_inner.get(key)
        if existing is not None:
            # Renewal of an existing mapping: keep the same external port
            # (RFC 6886 sec 3.3 "the NAT gateway ... should assign the same
            # external port"), just push the expiry out.
            existing.lifetime = granted
            existing.expires = now + granted
            self.vlog("renewed ext %d -> %s:%d for %ds"
                      % (existing.ext_port, in_ip, in_port, granted))
            return existing.ext_port, granted, None

        ext = self.pick_external(proto, in_ip, in_port, suggested)
        if ext == 0:
            return 0, 0, "no free external port"
        m = Mapping(proto, in_ip, in_port, ext, granted, now + granted)
        if not self.install(m):
            return 0, 0, "rule install failed"
        self.by_inner[key] = m
        self.by_ext[(proto, ext)] = m
        self.log("mapping granted: %s %s:%d -> %s:%d lifetime=%ds"
                 % (self.proto_name(proto), self.ext_ip, ext, in_ip, in_port, granted))
        return ext, granted, None

    # --- frame builders ----------------------------------------------------
    def pmp_unsupported_version(self):
        """RFC 6886 sec 3.5, 8 bytes: Vers=0, OP=0, Result Code=1, Epoch.

        This is ALSO the PCP downgrade signal. Overlaid on RFC 6887 sec 7.2's
        response header it reads version=0 / R|Opcode=0 / Reserved=0 /
        Result Code=1, i.e. UNSUPP_VERSION carrying version zero, which
        natpmp.c:168-170 turns into NATPMP_PARSE_PCP_IS_NATPMP.
        """
        return struct.pack("!BBHI", PMP_VERSION, 0, PMP_UNSUPPORTED_VERSION,
                           self.epoch())

    def pmp_addr_response(self, result):
        # natpmp.c:294-312
        pkt = struct.pack("!BBHI", PMP_VERSION,
                          PMP_RESP_FLAG + PMP_OP_PUBLIC_ADDR,
                          result, self.epoch())
        pkt += socket.inet_aton(self.ext_ip if result == PMP_SUCCESS else "0.0.0.0")
        assert len(pkt) == PMP_ADDR_RESP_LEN
        return pkt

    def pmp_map_response(self, opcode, result, in_port, ext_port, lifetime):
        # natpmp.c:352-384. The echoed INTERNAL port is the correlator and is
        # checked even on the failure path (natpmp.c:361-369), so it is filled
        # in unconditionally.
        pkt = struct.pack("!BBHIHHI", PMP_VERSION, PMP_RESP_FLAG + opcode,
                          result, self.epoch(), in_port,
                          ext_port if result == PMP_SUCCESS else 0,
                          lifetime if result == PMP_SUCCESS else 0)
        assert len(pkt) == PMP_MAP_RESP_LEN
        return pkt

    def pcp_response(self, opcode, result, lifetime, body=b""):
        """RFC 6887 sec 7.2 common response header (24 bytes) + optional body.

        natpmp.c:183-185 requires 24 <= len <= 1100 and len %% 4 == 0; a
        header-only frame is therefore legal and, per natpmp.c:216-224, is
        interpreted as a REFUSAL when its result code is non-zero (and
        discarded when it is zero, since it cannot be matched).
        """
        hdr = struct.pack("!BBBBII", PCP_VERSION, PCP_R_BIT | opcode, 0, result,
                          lifetime, self.epoch()) + b"\x00" * 12
        assert len(hdr) == PCP_HDR_LEN
        return hdr + body

    def pcp_map_response(self, result, lifetime, nonce, proto, in_port, ext_port):
        # RFC 6887 sec 11.2, parsed at natpmp.c:231-268.
        body = nonce + struct.pack("!B", proto) + b"\x00" * 3 \
            + struct.pack("!HH", in_port, ext_port) \
            + v4_mapped(self.ext_ip if result == PCP_SUCCESS else "0.0.0.0")
        pkt = self.pcp_response(PCP_OPCODE_MAP, result, lifetime, body)
        assert len(pkt) == PCP_MAP_LEN
        return pkt

    # --- request handling --------------------------------------------------
    def handle(self, data, src):
        if len(data) < 2:
            self.vlog("runt %d bytes from %s:%d" % (len(data), src[0], src[1]))
            return None

        ver = data[0]
        if ver == PCP_VERSION:
            return self.handle_pcp(data, src)
        if ver == PMP_VERSION:
            return self.handle_pmp(data, src)
        # RFC 6886 sec 3.5: a version we do not implement gets Unsupported
        # Version. Emitting the NAT-PMP frame here also means a PCP client on a
        # future version still receives a well-formed downgrade signal.
        self.vlog("unsupported version %d from %s:%d" % (ver, src[0], src[1]))
        return self.pmp_unsupported_version()

    def handle_pmp(self, data, src):
        op = data[1]
        if op == PMP_OP_PUBLIC_ADDR:
            if len(data) < PMP_ADDR_REQ_LEN:
                return None
            self.vlog("PMP op0 public-address from %s:%d -> %s"
                      % (src[0], src[1], self.ext_ip))
            return self.pmp_addr_response(PMP_SUCCESS)

        if op in (PMP_OP_MAP_UDP, PMP_OP_MAP_TCP):
            if len(data) < PMP_MAP_REQ_LEN:
                return None
            # RFC 6886 sec 3.3 request: Vers, OP, Reserved(2), Internal Port(2),
            # Suggested External Port(2), Requested Lifetime(4).
            # Built at natpmp.c:315-340.
            in_port, sugg, lifetime = struct.unpack("!HHI", data[4:12])
            proto = PROTO_UDP if op == PMP_OP_MAP_UDP else PROTO_TCP
            if in_port == 0:
                # RFC 6886 has no "malformed request" code (sec 3.5 lists only
                # 0-5), and internal port 0 is not a mapping anyone can use.
                # NOT_AUTHORIZED is the closest defined refusal. natpmp.c never
                # sends this -- Natpmp_AddMapping rejects internal_port 0 at
                # natpmp.c:1126 before a datagram is built -- so this branch
                # exists for hand-crafted frames only.
                return self.pmp_map_response(op, PMP_NOT_AUTHORIZED, 0, 0, 0)
            ext, granted, err = self.grant(proto, src[0], in_port, sugg, lifetime)
            if err is not None:
                self.log("PMP map REFUSED (%s) int=%d" % (err, in_port))
                return self.pmp_map_response(op, PMP_OUT_OF_RESOURCES, in_port, 0, 0)
            if lifetime == 0:
                # RFC 6886 sec 3.4: a successful delete is acknowledged with
                # external port 0 and lifetime 0. natpmp.c does not check the
                # delete's reply (natpmp.c:1398-1411), but emitting the correct
                # frame keeps the mock honest for any future client that does.
                self.vlog("PMP delete acked int=%d" % in_port)
                return struct.pack("!BBHIHHI", PMP_VERSION, PMP_RESP_FLAG + op,
                                   PMP_SUCCESS, self.epoch(), in_port, 0, 0)
            return self.pmp_map_response(op, PMP_SUCCESS, in_port, ext, granted)

        self.vlog("PMP unsupported opcode %d" % op)
        return struct.pack("!BBHI", PMP_VERSION, PMP_RESP_FLAG + (op & 0x7F),
                           PMP_UNSUPPORTED_OPCODE, self.epoch())

    def handle_pcp(self, data, src):
        if self.mode != "pcp":
            # Default rig posture: a NAT-PMP-only gateway. This is the frame the
            # production client is looking for at natpmp.c:168-170.
            self.vlog("PCP request from %s:%d -> UNSUPP_VERSION(0), downgrade"
                      % (src[0], src[1]))
            return self.pmp_unsupported_version()

        if len(data) < PCP_HDR_LEN:
            return self.pcp_response(0, PCP_MALFORMED_REQUEST, 0)
        opcode = data[1] & 0x7F
        if data[1] & PCP_R_BIT:
            return None  # a response, not a request
        if opcode != PCP_OPCODE_MAP:
            return self.pcp_response(opcode, PCP_UNSUPP_OPCODE, 0)
        if len(data) < PCP_MAP_LEN:
            return self.pcp_response(opcode, PCP_MALFORMED_REQUEST, 0)

        # RFC 6887 sec 7.1 / sec 11.1, built at natpmp.c:110-138.
        lifetime = struct.unpack("!I", data[4:8])[0]
        client_ip = data[8:24]
        nonce = data[24:36]
        proto = data[36]
        in_port, sugg = struct.unpack("!HH", data[40:44])

        # RFC 6887 sec 8.1: "If the PCP client's IP address in the request does
        # not match the source address of the request, the PCP server MUST
        # return ADDRESS_MISMATCH". natpmp.c:1059-1065 sources this field from
        # getsockname() on the connected socket, so a mismatch here is a real
        # topology bug worth surfacing rather than papering over.
        if client_ip != v4_mapped(src[0]):
            self.log("PCP ADDRESS_MISMATCH: client field %s vs source %s"
                     % (client_ip.hex(), src[0]))
            return self.pcp_map_response(PCP_ADDRESS_MISMATCH, 0, nonce, proto,
                                         in_port, 0)
        if proto not in (PROTO_UDP, PROTO_TCP):
            return self.pcp_map_response(PCP_UNSUPP_PROTOCOL, 0, nonce, proto,
                                         in_port, 0)

        ext, granted, err = self.grant(proto, src[0], in_port, sugg, lifetime)
        if err is not None:
            self.log("PCP map REFUSED (%s) int=%d" % (err, in_port))
            return self.pcp_map_response(PCP_NO_RESOURCES, 0, nonce, proto,
                                         in_port, 0)
        if lifetime == 0:
            self.vlog("PCP delete acked int=%d" % in_port)
            return self.pcp_map_response(PCP_SUCCESS, 0, nonce, proto, in_port, 0)
        return self.pcp_map_response(PCP_SUCCESS, granted, nonce, proto,
                                     in_port, ext)

    # --- shutdown ----------------------------------------------------------
    def request_stop(self, signum):
        """SIGNAL-HANDLER SAFE stop request: set two attributes, do nothing else.

        No raise, no I/O, no signal.signal() call. Every one of those was tried
        and each has a failure mode this daemon actually hit:

          raise    the KeyboardInterrupt lands wherever the interpreter happens
                   to be. If that is inside serve()'s teardown -- and a second
                   TERM lands there routinely, because natns.sh's `portmap
                   <side> down` sends one to the pidfile and then immediately
                   pkills the same command line -- it escapes the finally,
                   printing a traceback AND abandoning the -I'd rules it was in
                   the middle of removing.
          logging  sys.stderr.write() from a handler can re-enter a write that
                   the interrupted code was already inside.
          SIG_IGN  calling signal.signal() from inside a handler was observed
                   to fail with "OSError: Signal 15 ignored due to race
                   condition" under a burst of signals.

        A flag has none of those. Repeated signals just set it again, which is
        a no-op, so a signal storm is indistinguishable from a single TERM.

        MEASURED, debian:trixie + python 3.13.5, privileged container, one
        granted mapping (two -I'd nat rules) live at stop time:

                                       raising handler      this flag
          natns.sh-shaped kill+pkill   10/10 exit 130,      10/10 exit 0,
          (x10)                        1 rule left,         0 rules left,
                                       traceback            no traceback
          200 x SIGTERM (x10)          10/10 exit 143,      10/10 exit 0,
                                       2 rules left,        0 rules left,
                                       traceback in 6/10,   no traceback
                                       one run lost stderr
          200 x SIGINT                 (same class)         exit 0, 0 left

        The exit statuses are the tell: 130/143 is "died of a signal", which is
        what an intentional teardown had been reporting to run_matrix.sh.
        """
        if not self.stopping:
            self.stopping = True
            self.stop_signal = signum

    # --- main loop ---------------------------------------------------------
    def serve(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.listen, self.port))
        sel = selectors.DefaultSelector()
        sel.register(s, selectors.EVENT_READ)

        # WAKE THE SELECTOR THE INSTANT A SIGNAL ARRIVES.
        #
        # request_stop only sets a flag, and the flag is tested once per loop
        # iteration -- so on its own the daemon would keep running for up to
        # SELECT_TIMEOUT after being asked to stop. natns.sh's `portmap <side>
        # down` does not wait for us: it signals and returns, so that delay is
        # time in which the rules it asked us to remove are still in the nat
        # table while the caller believes they are gone. Measured teardown
        # latency with the pipe in place: 0.079 s from SIGTERM to exit.
        #
        # signal.set_wakeup_fd closes it without running any Python in handler
        # context: the interpreter writes the signal number to this pipe from
        # C, the selector wakes on it, and the flag is acted on immediately.
        # It is main-thread-only, so its absence is tolerated -- the flag is
        # still correct, just noticed one timeout later.
        wake_r = wake_w = None
        prev_wakeup_fd = -1
        try:
            wake_r, wake_w = os.pipe()
            os.set_blocking(wake_r, False)
            os.set_blocking(wake_w, False)
            prev_wakeup_fd = signal.set_wakeup_fd(wake_w)
            sel.register(wake_r, selectors.EVENT_READ)
        except (ValueError, OSError) as e:
            self.vlog("no signal wakeup pipe (%s); the %.0fs select timeout is "
                      "the shutdown poll interval instead" % (e, SELECT_TIMEOUT))
            for fd in (wake_r, wake_w):
                if fd is not None:
                    try:
                        os.close(fd)
                    except OSError:
                        pass
            wake_r = wake_w = None

        # natns.sh's `portmap <side> up` greps this log for the literal
        # "ready on" to know the socket is bound, so that prefix is
        # load-bearing; conntrack= is appended at the END where it cannot
        # disturb that match, and states the optional dependency up front
        # instead of leaving it to be inferred from a missing flush later.
        self.log("ready on %s:%d mode=%s ext=%s inner=%s wan-if=%s rules=%s "
                 "conntrack=%s"
                 % (self.listen, self.port, self.mode, self.ext_ip,
                    self.inner_ip, self.wan_if,
                    "on" if self.install_rules else "off",
                    self.conntrack_bin if self.conntrack_bin else "absent"))
        try:
            while not self.stopping:
                for key, _events in sel.select(timeout=SELECT_TIMEOUT):
                    if key.fd == wake_r:
                        try:
                            os.read(wake_r, 4096)  # drain; the flag is the news
                        except OSError:
                            pass
                        continue
                    try:
                        data, src = s.recvfrom(2048)
                    except OSError:
                        continue
                    try:
                        reply = self.handle(data, src)
                    except Exception as e:  # never let one bad frame kill the rig
                        self.log("handler error: %r" % (e,))
                        continue
                    if reply:
                        try:
                            s.sendto(reply, src)
                        except OSError as e:
                            self.log("send failed: %s" % e)
                self.sweep()
            self.log("stop requested (signal %d) -- tearing down"
                     % self.stop_signal)
        except KeyboardInterrupt:
            # Only reachable for an interrupt raised before main() installed
            # the flag handlers, or by a caller that never installed them.
            self.log("interrupt received -- tearing down")
        finally:
            # Detach the wakeup fd BEFORE closing it: leaving the interpreter
            # pointed at a closed descriptor makes every later signal print a
            # write error from C, which is the same class of cosmetic-looking
            # noise this whole change exists to remove.
            if wake_w is not None:
                try:
                    signal.set_wakeup_fd(prev_wakeup_fd)
                except (ValueError, OSError):
                    pass
                try:
                    sel.unregister(wake_r)
                except (KeyError, ValueError, OSError):
                    pass
                for fd in (wake_r, wake_w):
                    try:
                        os.close(fd)
                    except OSError:
                        pass
            try:
                sel.close()
            except Exception:
                pass
            try:
                s.close()
            except OSError:
                pass
            # THE RULES COME OUT LAST, AND NOTHING MAY INTERRUPT IT.
            #
            # This is the loop the reported traceback died inside: signal ->
            # run() -> remove() -> drop() -> here. It survives now because the
            # handlers raise nothing at all, so however many further TERMs
            # arrive, every drop() runs and every -I'd rule is removed. A
            # teardown that stops halfway leaves the next cell running against
            # a nat table it did not configure, which is a silently WRONG
            # result rather than a failed one.
            for key in list(self.by_inner):
                self.drop(key)
            self.log("stopped")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--listen", required=True,
                    help="LAN-side IP to bind (the gateway address the client "
                         "is pointed at via Natpmp_TestHook_SetGateway)")
    ap.add_argument("--external-ip", required=True,
                    help="the NAT's WAN address; reported by op 0 and used as "
                         "the SNAT source")
    ap.add_argument("--inner-ip", required=True,
                    help="the LAN host behind this NAT; DNAT destination")
    ap.add_argument("--wan-if", required=True,
                    help="WAN-side interface name in this namespace (nAo/nBo)")
    ap.add_argument("--port", type=int, default=GATEWAY_PORT)
    ap.add_argument("--mode", choices=("natpmp", "pcp"), default="natpmp",
                    help="natpmp (default): answer PCP with the RFC 6887 sec 9 "
                         "downgrade signal, then serve NAT-PMP. "
                         "pcp: grant PCP MAP directly.")
    ap.add_argument("--lifetime-cap", type=int, default=3600,
                    help="maximum granted lifetime in seconds (default 3600, "
                         "matching NATPMP_LEASE_SECONDS at natpmp.h:280)")
    ap.add_argument("--no-rules", action="store_true",
                    help="speak the protocol but install no iptables rules "
                         "(protocol-only testing outside a netns)")
    ap.add_argument("--no-conntrack-flush", action="store_true",
                    help="install rules WITHOUT clearing existing conntrack "
                         "state; the control case that shows why the flush is "
                         "needed. Never use this in a real cell.")
    ap.add_argument("--seed", type=int, default=None,
                    help="seed the anonymous-port RNG for reproducible runs")
    ap.add_argument("--log", default=None, help="append a copy of the log here")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()

    gw = Gateway(a)

    # SIGTERM/SIGINT IS THE SUCCESS PATH, not a failure. This daemon has no
    # quit command and no finite run length -- natns.sh's `portmap <side> down`
    # stops it, and it does so TWICE in a row: `kill` on the pidfile, then an
    # unconditional `pkill -f` on the same command line. So "two signals,
    # microseconds apart" is the NORMAL case, not an edge one.
    #
    # The handler therefore does the least it possibly can: set a flag. The
    # previous handler raised KeyboardInterrupt, which is what produced the
    # reported traceback -- the second signal raised inside serve()'s teardown,
    # escaped the finally, and left the -I'd rules in the nat table for the
    # next cell. See Gateway.request_stop for the full list of what a handler
    # must not do here and why.
    #
    # Default disposition matters too: SIGTERM's default is silent termination,
    # which would skip teardown entirely, and SIGINT's default raises
    # KeyboardInterrupt, which is the failure above. Both are replaced.
    def _stop(signum, frame):
        gw.request_stop(signum)
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    try:
        gw.serve()
    except KeyboardInterrupt:
        # Nothing installed above can raise this any more; it remains as the
        # outermost guard for an interrupt in the window before the handlers
        # were installed. Exit status 0, because being asked to stop is not an
        # error and a non-zero status here makes run_matrix.sh's per-cell
        # cleanup look like a failed cell.
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
