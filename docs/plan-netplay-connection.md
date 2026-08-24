# Netplay connection-establishment program (S1–S8)

Staged program to make direct-P2P connection establishment reliable,
diagnosable, and safe. **This document accompanies the S1 "host
liveness" commit series — S1 is IMPLEMENTED by the commits that land
alongside this file; S2–S8 are planned.**

Citation convention: `path:NNN` refers to the tree as of the S1
series plus its adversarial-review fix commits (H1-H3/M1-M3/L1/L4). Pre-S1 baseline pointers are cited as `path@1b217758:NNN`
(the `upstream-engine-fixes` tip this series branched from). Every
constant below was read from the named line, not recalled.

---

## 1. Measured baseline (pre-S1)

### 1.1 The cascade and its real constants

Host path (`host_thread_fn`, `src/netplay/direct_p2p.c@1b217758:680-787`):

| Step | Budget | Source |
|---|---|---|
| Startup delay (init race workaround) | 200 ms fixed | direct_p2p.c@1b217758:689 |
| UPnP probe (whole attempt) | 6 000 ms wall clock | direct_p2p.c@1b217758:470 |
| — miniupnpc SSDP discover inside it | 2 000 ms | src/netplay/upnp.c:26 `UPNP_DISCOVER_TIMEOUT_MS` |
| — UPnP lease requested | 3 600 s, **never renewed pre-S1** | src/netplay/upnp.c:27 `UPNP_LEASE_DURATION "3600"` |
| STUN discover (4 servers, serial) | ~2.1 s/server worst → ~8.4 s | stun.c server list @ stun.c:190-200; 20×100 ms recv poll @ stun.c:298-302 (pre-S1 numbering 295-302) |
| Rendezvous re-REGISTER loop | 500 ms cadence, **8 000 ms budget, then thread exits permanently** | direct_p2p.c@1b217758:582-611 (`CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_BUDGET_MS` default 8000, config.c@1b217758:90) |

Worst case to a displayed room code ≈ 0.2 + 6 + 8.4 ≈ **14.6 s**;
typical (UPnP answers fast, first STUN server answers) ≈ 2–4 s.

Joiner path (`join_thread_fn`, direct_p2p.c@1b217758:793-1091):

| Step | Budget | Source |
|---|---|---|
| Startup delay | 200 ms | direct_p2p.c@1b217758:797 |
| STUN discover | ≤ ~8.4 s | as above |
| Direct hole-punch | 2 500 ms | direct_p2p.c@1b217758:842-843 |
| Fallback signaling (REGISTER/DELIVER) | 8 000 ms | config.c@1b217758:90 |
| Bilateral hole-punch | 3 000 ms | config.c@1b217758:91 (`BILATERAL_PUNCH_MS`) |

Worst case join ≈ **22.1 s** to terminal failure.

Rendezvous server (`tools/rendezvous-server/rendezvous-server.js@1b217758`):

| Constant | Value | Line |
|---|---|---|
| `SESSION_TTL_MS` | 60 000 ms | :24 (eviction sweep :140-152, interval 5 s :25) |
| Rate limiter | 10 packets / 1 000 ms sliding window / IP | :27-28, enforcement :114-136 |
| Session table cap | **none** pre-S1 | (absence: handleRegister @1b217758:174-222 inserts unconditionally) |

### 1.2 Failure-exit table (states, statuses, and what the user sees)

All from `src/netplay/direct_p2p.c` (current lines); labels from
`direct_p2p_overlay.c:53-82` ("ERROR" for every FAILED_*):

| Terminal state | Status text | Raised at |
|---|---|---|
| `FAILED_STUN` | "Connection failed. Try again." | host: direct_p2p.c:971, 1025; thread-spawn failure paths in Begin* :1872/:1932; joiner: :1097 |
| `FAILED_PUNCH` | "Invalid room code." | BeginJoin decode failures, direct_p2p.c:1891, 1900 |
| `FAILED_SYMMETRIC` | "Could not connect. Try a different network." | joiner bypasses :1152/:1158/:1165 (host side no longer has a terminal gate — same-IP DELIVERs are ignored as stale self-registrations, review H1) |
| `FAILED_BILATERAL` | "Could not connect. Try a different network." | joiner signaling/punch failures :1189-:1336; host: punch-thread spawn failure :1719, retry-budget exhaustion :2130 (review M1: a single host-side punch failure returns to HOST_WAITING) |
| `FAILED_HANDSHAKE` | MIST reject reason | R-1 path, :1435 |

Non-exits (hangs) are catalogued in S3.

### 1.3 The liveness finding (why S1 outranks NAT-type work)

Verified mechanism chain, pre-S1:

1. The host's re-REGISTER thread ran 500 ms × 8 s and then **exited
   permanently** (direct_p2p.c@1b217758:582-611). Nothing else on the
   host sends after that — `host_tick_receive` only receives/echoes.
2. The rendezvous server evicted the idle session at
   `SESSION_TTL_MS = 60 s` (rendezvous-server.js@1b217758:24, sweep
   :140-152). After T+68 s the session key resolves to nothing.
3. The host's NAT mappings (toward the STUN server — for a non-UPnP
   host this is the very mapping the room code advertises — and toward
   the rendezvous server) decay at the router's UDP idle timeout, since
   no outbound traffic refreshes them.
4. Meanwhile the overlay displays the room code indefinitely
   (`DirectP2P_GetHostCode`, valid the whole time state stays
   `HOST_WAITING`).

Real usage: the host reads an 11-char code aloud or pastes it in chat;
the friend types it in **minutes later**. By then the session is
un-pair-able on every path — and this is independent of NAT type.
Demonstrated end-to-end against the real server (see §3.4).

---

## 2. NAT-pair matrix (host × joiner)

Mechanics verified in code; classifications are standard NAT taxonomy.
"UPnP" means the host's router granted the mapping
(`try_upnp`, direct_p2p.c:509-566) and its external IP is not
provably non-public (CGNAT gate, §3.6; review M3).

| Host \ Joiner | Full-cone / restricted | Port-restricted | Symmetric |
|---|---|---|---|
| **UPnP mapped** | direct punch to mapped port succeeds | succeeds (joiner's own mapping opens on first send) | succeeds — mapped port accepts any source |
| **Full/restricted cone, no UPnP** | direct punch succeeds | succeeds | joiner's source port differs per destination → host's echo goes to the STUN-observed (wrong) port; **bilateral fallback required**, host-side learns true port from first inbound (host_tick_receive captures source, direct_p2p.c:1763-1782) |
| **Port-restricted, no UPnP** | succeeds | succeeds (simultaneous send opens both) | fails direct; bilateral gives the host the joiner's fresh mapping via DELIVER — works iff joiner's NAT maps the rendezvous-learned port for the host too (usually not, for true symmetric) |
| **Symmetric, no UPnP** | host's advertised STUN port is per-destination-wrong; joiner's punch lands on a dead mapping. Bilateral: server learns the host's port *toward the server*, still wrong toward the joiner → **fails**; needs relay (S5) | same | **fails — S5 relay is the only path** |
| **CGNAT (any inner type)** | pre-S1: silently broken when UPnP "succeeded" (wrong ip/port pair, §3.3.5); post-S1: behaves as the corresponding no-UPnP row | ↑ | ↑ |

Hairpin (both peers behind one router) is a special row: works only
with router NAT-loopback support. The joiner fails fast with
`FAILED_SYMMETRIC` before ever REGISTERing (join bypass :1163-1166);
because of that bypass a same-IP DELIVER can only be the host's own
stale registration, so the host now IGNORES it and keeps waiting
(review H1, try_handle_deliver self-DELIVER gate :1655-1683).

---

## 3. S1 — Host liveness (THIS commit series)

Fix = keep the host alive for as long as it is advertising.

### 3.1 Persistent re-REGISTER
`host_rendezvous_thread_fn` (direct_p2p.c:760-851) now re-REGISTERs
every `netplay-direct-p2p-register-interval-ms` (default 5 000 ms,
floor 1 000 ms; config.c defaults block) for the **entire duration of
HOST_WAITING** — exit on `s_rendezvous_cancel` or on the state leaving
`HOST_WAITING` (the direct-punch handoff path never raises the cancel
flag; the state check covers it). Server cost ≈ 0.2 pkt/s/host against
the verified 10 pkt/s/IP limiter (rendezvous-server.js:41-42
`RATE_WINDOW_MS`/`RATE_LIMIT_PER_WINDOW`). Spawn remains behind the
`netplay-direct-p2p-disable-bilateral` kill switch
(direct_p2p.c:1063-1070).

Bonus correctness fix: the session key is now derived from the
**advertised** tuple (`Work.advertised_port`, direct_p2p.c:103-115) on
both the register loop (:796-799) and the host DELIVER handler
(:1635-1640). Pre-S1 the host hashed its raw STUN port while the
joiner hashed the room-code port (UPnP external when mapped,
direct_p2p.c@1b217758:739 vs :933-934) — different rendezvous slots
whenever the two ports differed (non-port-preserving NAT), so the
bilateral fallback could never pair those hosts.

### 3.2 STUN rebind keepalive + drift detection
Every `netplay-direct-p2p-stun-keepalive-ms` (default 20 000 ms, ≤ 0
disables) while HOST_WAITING, the main thread re-issues a STUN Binding
Request on the same socket toward the server that answered discovery
(`host_stun_keepalive_tick`, direct_p2p.c:1488-1504;
`Stun_SendKeepalive`, stun.c). The probe refreshes the advertised NAT
mapping; the response is routed through a new STUN gate in
`host_tick_receive` (direct_p2p.c:1752-1762) — which also fixes a
latent pre-S1 bug where a straggler Binding Response from a slower
`Stun_Discover` server arriving during HOST_WAITING was captured as
"the peer" (@1b217758:1295-1322 had no payload validation at all).

**Chosen drift behavior:** if the mapped endpoint differs from the
last known one, the NAT rebound and the displayed code is already
dead. We re-encode and **display the NEW code** with status "Network
changed! Share the NEW code." (`host_handle_stun_rebind`,
direct_p2p.c:1529-1606), and restart the rendezvous loop under the new
session key (cancel+join before mutating the fields it reads).
Review M2: the rewrite is debounced — a drift commits only when two
consecutive keepalives report the same new endpoint, so a NAT that
rebinds every interval never churns the code; and the default
keepalive dropped 20 s -> 15 s to sit below common ~30 s NAT UDP idle
timeouts, holding the mapping so drift rarely happens at all. We do
not silently continue; preserving the old, dead code has no value, and
an explained code change is the least surprising outcome for a user
staring at a code they already shared. A live UPnP mapping pins the
advertised port, so only IP drift can change the code there.

### 3.3 Server TTL + cap
`SESSION_TTL_MS` 60 s → **10 min** (rendezvous-server.js:30) so a code
shared over chat/voice stays pair-able; `MAX_SESSIONS = 4096` bounds
the memory exposure the longer TTL creates (~1.3-2 MB realistic under
V8). Review H2 replaced drop-on-full (itself a lockout vector) with a
layered policy: `MAX_NEW_KEYS_PER_IP = 4` per-IP live-key quota, and
at cap a new key evicts the oldest UNPAIRED singleton (paired sessions
are never evicted; a live host's <=5 s re-REGISTER keeps it off the
eviction front). Review H1 added same-IP stale-slot reclamation
(`SLOT_STALE_MS = 30 s`) so a cancel-then-re-host client reclaims its
own key instead of poisoning it. Tests: `__test_protocol.js`
`testSessionTtl` / `testSessionCap` / `testPerIpQuota` /
`testSpoofedFloodEviction` / the stale-slot reclaim suite (real sweep +
real packets; aging simulated through the `_sessionMap` hook).

### 3.4 Demonstration (real server, real clock)
`scratchpad/demo-liveness.js` ran both behaviors concurrently:
pre-S1 (60 s-TTL server from `git show 1b217758`, host silent after
8 s) → joiner REGISTER at T+75 s got `0.0.0.0:0` (NOT pair-able);
S1 (worktree server, host re-REGISTERing every 5 s) → joiner at
T+75 s received the host endpoint and the host received the
unsolicited DELIVER push (pair-able). Output is in the S1 task report.

### 3.5 UPnP lease renewal
The 1-hour lease (upnp.c:27) is renewed at half-life (30 min,
`upnp_renew_tick`, direct_p2p.c:646-705), retry at 5 min on failure,
**including mid-session**: `main.c` now ticks the orchestrator from
the active-session branch (main.c, `DirectP2P_Tick` beside
`Netplay_Run`), because the mapping is what carries the peer's
traffic. Renewal runs on a side thread (router HTTP), never touches
the GekkoNet-owned socket; teardown/Cancel reap it before
`Upnp_RemoveMapping` with a bounded 2 s deadline+detach (review H3 —
an unbounded join could block the game thread for the OS SYN timeout
when the router is dead), skipping the router-side removal on detach
so miniupnpc's cached-IGD statics (upnp.c:34-37) are never used
concurrently.

### 3.6 CGNAT blind spot
`UpnpMapping.external_ip` was captured (upnp.c:120) and never read.
Behind CGNAT/double-NAT the inner router reports a private/CGN
external IP while STUN reports the true public IP; the room code
paired the STUN IP with the UPnP port (@1b217758:739-740) — a wrong
pair that silently killed the direct path. Now compared after STUN
discovery (direct_p2p.c:975-1010): the mapping is dropped only when
the router-reported IP parses and is provably non-public (RFC1918 /
CGN 100.64.0.0/10 / loopback / link-local — review M3); a
public-but-different (1:1 NAT / DMZ) or unparseable external IP keeps
the mapping and logs. On drop, advertise the STUN endpoint and let
punch/bilateral carry it.

---

## 4. S2 — Punch / STUN mechanics

- **`Stun_HolePunch` retarget bug**: `local_peer_port` is captured
  once (stun.c:407) and every send — including the three
  post-success confirmation sends (stun.c:450) — targets it and the
  original `peer` address, even after the loop learned the peer's TRUE
  translated endpoint (`*peer_port = dgram->port` / `peer_ip`
  overwrite, stun.c:442-446). Against a symmetric peer the
  confirmations go to the stale port, so the *other* side may never
  see our punch and time out. Fix: re-resolve/re-target after the
  endpoint update.
- **Adaptive cadence**: fixed 200 ms punch interval (stun.c:381);
  start faster (50 ms) and back off.
- **Parallel STUN with RFC 5389 retransmit**: `Stun_Discover` probes
  4 servers serially at ~2.1 s each (stun.c:242-302); probe in
  parallel on the one socket, RFC 5389 §7.2.1 retransmit timers.
- **Wire the dead key**: `netplay-direct-p2p-stun-timeout-ms`
  (default 4000, config.c:85) is read nowhere (only comments,
  direct_p2p.c:398-404) — `Stun_Discover` needs a timeout parameter.

## 5. S3 — No hangs + failure taxonomy

- `NAV_WAIT_ORCHESTRATOR` has **no timeout**
  (src/netplay/netplay_nav.c:309-321): if the orchestrator never sets
  remote_ip, nav waits forever.
- `NETPLAY_SESSION_CONNECTING` has no timeout in netplay.c
  (netplay.c:1564-1567 just runs `run_netplay()`; exit requires a
  Gekko `GekkoPlayerConnected` event, netplay.c:1052). The design
  study's claim that GekkoNet's 'Initiating' phase retries forever
  could NOT be verified locally — GekkoNet ships prebuilt
  (third_party/GekkoNet/build/{include,lib}, no sources); verify
  against upstream GekkoNet source before building S3.
- Deliverable: every waiting state gets a bounded timer + a distinct
  terminal state, and the failure-exit table (§1.2) grows a
  machine-readable reason code for telemetry.

## 6. S4 — Security

- The host treats **any** non-'3SXR', non-STUN datagram as the peer
  and hands the session off to its source
  (host_tick_receive peer capture, direct_p2p.c:1763-1791; pre-S1
  @1b217758:1295-1322). S1 narrowed this (STUN responses gated) but a
  blind attacker who guesses/observes ip:port still gets a handoff.
  Add a punch-payload check + a code-derived token.
- The room code is a plaintext (ip, port) encoding
  (room_code.h:59-84) — anyone seeing the code learns the endpoint.
  Acceptable for friend-to-friend, but note it; MIST handshake
  (netplay.c R-1 path) is the backstop.

## 7. S5 — Custom '3SXR' relay for symmetric-NAT pairs

Symmetric×symmetric (and most symmetric×port-restricted) pairs cannot
punch (matrix §2). **Not coturn**: `do_handoff` transfers a bare
`NET_DatagramSocket` into netplay
(Netplay_SetStunSocket, direct_p2p.c:1449-1459) and GekkoNet then owns
plain UDP send/recv on it — a TURN allocation needs TURN framing on
the wire, which GekkoNet will not speak. A minimal relay extension to
the existing rendezvous protocol (same '3SXR' magic, new RELAY type
that forwards raw payloads between the two registered endpoints)
keeps the socket bare. Budget/abuse controls inherit the S1 limiter +
session cap.

## 8. S6 — Joiner candidate racing

Joiner currently tries exactly one (ip, port) tuple. Race candidates
concurrently on the one socket: room-code endpoint, rendezvous
DELIVER endpoint, LAN-local addresses (hairpin), first responder
wins. Removes the serial 2.5 s + 8 s + 3 s worst case (§1.1).

## 9. S7 — NAT-PMP / PCP

miniupnpc only speaks UPnP IGD. Many routers (esp. Apple/BSD-based)
speak NAT-PMP/PCP instead — add libnatpmp (or a ~200-line PCP client)
as a second mapping backend behind the same `UpnpMapping` interface
(upnp.h:11-16), tried when IGD discovery fails.

## 10. S8 — netns verification harness

Reproduce the matrix (§2) deterministically on Linux: network
namespaces + nftables masquerade variants (full-cone via `fullconenat`
or nft `masquerade persistent`, symmetric via random port masquerade),
one namespace per peer + one for the rendezvous/relay server; drive
two headless builds with `--test-*`-style CLI entry points
(pattern: src/args.c test flags block) and assert each cell's
expected outcome. This is the regression net for S2–S7.

---

### Stage status

| Stage | Status |
|---|---|
| S1 host liveness | **implemented (this series)** |
| S2–S8 | planned above |
