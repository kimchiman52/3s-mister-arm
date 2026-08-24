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

## 4. S2 — Punch / STUN mechanics (IMPLEMENTED)

Landed as its own commit series on top of S1. Citations refer to the
post-S2 tree.

- **`Stun_HolePunch` retarget fix** (headline): the loop deliberately
  accepts a punch on source-IP + exact `"3SX_PUNCH"` payload alone
  (port intentionally unmatched — that is what recognizes a symmetric
  peer punching from a translated per-destination port) and learns the
  true endpoint, but every send — including the post-success
  confirmations — still targeted the ORIGINAL port captured into
  `local_peer_port` at function entry. The symmetric peer therefore
  never saw a confirmation and its own punch timed out. Now the accept
  path retargets `local_peer_port` + the send address (a ref on the
  datagram's own source address) and keeps punching the confirmed
  endpoint for ~600 ms at 50 ms cadence (stun.c:694-747; replaces the
  old 3×50 ms burst to the stale port). Unblocks the bilateral-phase
  cells where a cone-family side pairs a symmetric side: the cone side
  now confirms to the symmetric side's real mapping, which that NAT
  accepts because it is the very mapping the symmetric side is punching
  from. (host full/restricted-cone × joiner symmetric, and the mirrored
  symmetric × full/restricted-cone; port-restricted × symmetric still
  needs S5 — the port-restricted filter drops the symmetric side's
  off-port punches before the retarget can trigger.) Regression:
  test_stun_mock.c `run_punch_retarget_test` (fails on the pre-S2
  tree — zero confirmations reach the translated port).
  - *Attribution correction (adversarial review, L-2/L-6)*: the
    host-full-cone × joiner-symmetric cell is flipped on the DIRECT
    path by the **byteswap fix alone** — with the room code carrying a
    live port again, a full-cone host accepts the symmetric joiner's
    punch from any source, no retarget needed; the retarget fix
    carries that cell only when the direct path fails and the
    bilateral phase runs. The retarget fix is strictly necessary for
    the symmetric-HOST cells (symmetric × full/restricted-cone) and
    for restricted-cone × symmetric.
  - *Reviewer INFO note (recorded, no code change)*: the retarget
    accept criteria are source-IP + exact `"3SX_PUNCH"` payload, so
    retargeting at a WRONG source would require a CGNAT neighbour
    sharing the peer's public IP sending that exact payload into our
    punch window. This is not a new surface — the port capture (and
    IP-only matching) predates S2 — and punch authentication is
    already on the S4 backlog.
- **STUN port byteswap bug (unplanned find, worse than the plan
  knew)**: `parse_binding_response` assembled X-Port from the wire
  bytes via shifts (already native) and then applied `SDL_Swap16BE` on
  top — on little-endian hosts (macOS AND MiSTer ARM) every
  STUN-parsed port was byteswapped (55555 → 985). Same in the plain
  MAPPED-ADDRESS branch; the IP branch was unaffected (its swap
  cancels by re-reading through memory bytes). So every non-UPnP room
  code advertised a DEAD port; UPnP hosts and the bilateral path (the
  server reports the port it OBSERVES) masked it. Fixed
  (stun.c:129-137, :171-173); regression via the public
  `Stun_ParseBindingResponse` in test_stun_mock.c `run_wire_test`.
  The parser had zero prior coverage — the original wire test built
  and parsed with its own local helpers.
- **Adaptive cadence**: 50 ms for the first 500 ms of the punch
  window, then the original 200 ms (stun.c:625-635, :672-676).
  Establishment time is dominated by peer start-skew and first-packet
  loss, not RTT.
- **Bilateral window 3000 → 5000 ms**
  (`netplay-direct-p2p-bilateral-punch-ms`, config.c defaults block):
  the two sides' punch windows start skewed by DELIVER arrival (the
  joiner punches the instant its DELIVER parses; the host learns of
  the joiner a Tick later and spawns a worker), and both loops drop
  stray non-punch datagrams (re-verified post-S1), so extra overlap
  costs only failure-case wait.
- **Parallel STUN with RFC 5389 retransmit** (`Stun_Discover`
  rewrite, stun.c:355-616): all 4 servers probed in parallel from the
  one socket, distinct txid + prebuilt request per server; per-server
  retransmits at 0/500/1500 ms (§7.2.1 RTO≥500 ms doubling, truncated
  to 3 sends — parallel servers substitute for deeper
  retransmission); first parseable Binding Response wins and its
  server is the one retained in `server_addr` for the S1 keepalives.
  DNS for all servers resolves concurrently on a refcounted side
  thread (getaddrinfo has no portable timeout; the thread is detached
  if stuck), with a numeric-IP fallback list (dig snapshot
  2026-08-23) armed when DNS produced nothing within 300 ms, or —
  post-review (L-3) — when no server has ANSWERED within 1500 ms
  (covers a resolver that answers the first host then blackholes);
  neither a blackholed nor a partially-dead resolver can eat the
  budget.
- **Dead key wired**: `netplay-direct-p2p-stun-timeout-ms` (default
  4000) is now the overall discovery budget — new `timeout_ms`
  parameter on `Stun_Discover`, threaded from both discovery sites via
  `stun_budget_ms()` (clamped 1000–15000 ms post-review L-4:
  `Stun_Discover` is not cancellable mid-run and `DirectP2P_Cancel`
  joins the worker on the game thread, so the budget bounds Cancel's
  block). The stale "out of scope for Step 7" NOTE is gone.
- **Symmetric-NAT signal for S3**: after the first response, discovery
  lingers ≤300 ms (early-out once all probed servers answered) to
  collect the rest; `StunResult.port_disagreement` (stun.h:31) is set
  and logged when servers disagree on the mapped port. S3 consumes
  this for failure attribution; no UX in S2 by design.
- **Auto-retry policy**: (a) joiner — join_thread_fn
  (direct_p2p.c:1401) wraps the extracted `join_attempt()`
  (direct_p2p.c:1110) and interposes exactly ONE automatic full retry
  on any terminal failure before surfacing it; each attempt re-runs
  discovery on local_port 0 with the previous socket closed, so the
  retry binds a FRESH local port (dodges stuck conntrack/NAT state;
  also covers host-still-in-UPnP-probe start-skew). (b) host —
  Tick's FAILED_STUN case (direct_p2p.c:2113) re-spawns
  host_thread_fn after a 5 s backoff, ≤3 retries per hosting session,
  instead of parking terminal; composes with (and does not touch) the
  S1 bilateral-failure return-to-HOST_WAITING path.
- **Tests**: test_stun_mock.c gains `run_punch_retarget_test` plus,
  via the `Stun_TestHook_SetServers` seam, `run_discover_parallel_test`
  (dead server first + live server → success in ~340 ms, live server
  retained), `run_discover_retransmit_test` (first packet dropped →
  recovered by the 500 ms retransmit), and
  `run_discover_disagreement_test` (40000 vs 40001 → flag set).
  test_bilateral_punch.c gains test 6 (`test_joiner_fresh_socket_retry`)
  driving the REAL BeginJoin offline through the new
  `DirectP2P_TestHook_SetStunDiscover` seam: double failure = exactly
  2 attempts on 2 different local ports; punch-succeeds-on-attempt-2 =
  HANDOFF.

Plan-drift note: the pre-S2 pointers above originally cited stun.c:407
(“captured port”), :450 (confirmation sends), :442-446 (endpoint
update) — those matched the pre-S1 numbering and have been superseded
by the citations in this section.

## 5. S3 — No hangs + failure taxonomy (IMPLEMENTED)

Landed as its own commit series on top of S2. Citations refer to the
post-S3 tree.

### 5.1 Hangs closed (Part A)

- `NAV_WAIT_ORCHESTRATOR` (netplay_nav.c) was the only nav state with
  no `s_frames_in_state` deadline. Now: bails to NAV_DONE on terminal
  orchestrator FAILED_* (except host FAILED_STUN, whose S2 auto-retry
  is live), on orchestrator-IDLE with no remote ip (5 s debounce), or
  on a 150 s overall deadline that RE-ARMS while the orchestrator sits
  in HOST_WAITING (unbounded by design). Deadline expiry logs
  `P2P_FAIL_TIMEOUT_ORCHESTRATOR`.
- `NETPLAY_SESSION_CONNECTING` (netplay.c) had no timeout — exit
  required a Gekko event; GekkoNet's `DISCONNECT_TIMEOUT` (5000 ms)
  applies only to actors already `Connected` (an actor stuck
  `Initiating` retries `SendSyncRequest` every 200 ms forever), and the
  netplay watchdog only fires while RUNNING. GekkoNet is NOT patched;
  our own wall-clock deadline (`CONNECT_TIMEOUT_CONNECTING_MS` = 15 s,
  connect_fail.h) exits to EXITING with
  `P2P_FAIL_TIMEOUT_CONNECTING`, surfaced via
  `DirectP2P_NotifySessionFailed` (overlay ERROR + reason on every
  entry path), plus a 5 s "still CONNECTING" log line.
- `HOST_WAITING` stays unbounded by design but is now informative:
  minute-cadence elapsed status ("Waiting for player 2... (3 min)") +
  liveness log, and the cause-8 advisory (below) after 30 s of silence.
- User-reachable abort: holding START ~3 s
  (`CONNECT_ABORT_HOLD_FRAMES`) in TRANSITIONING or CONNECTING tears
  down as a user cancel (`P2P_ABORT_USER`).

### 5.2 Failure taxonomy (Part B) — src/netplay/connect_fail.{h,c}

Machine codes are stable log-grep anchors; user strings fit the
overlay status line. Detection evidence, as implemented:

| # | cause | detection evidence | machine code | user string |
|---|---|---|---|---|
| 1 | no network / DNS dead | every getaddrinfo failed AND zero STUN replies (`StunResult.diag_*`) | `P2P_FAIL_DNS_ALLDOWN` | "No internet connection (DNS failed)." |
| 2 | STUN blocked | sends succeeded, zero responses from all servers | `P2P_FAIL_STUN_ALLDOWN` | "Network blocks UDP (no STUN reply)." |
| 3 | rendezvous server down | ZERO DELIVER frames for the whole signaling budget — the server answers EVERY REGISTER with a DELIVER (real or 0.0.0.0:0 sentinel; rendezvous-server.js handleRegister), so silence = server/path down. Requires the `Rendezvous_ParseDeliverEx` tri-state split (MALFORMED vs EMPTY vs PEER) | `P2P_FAIL_RENDEZVOUS_DOWN` | "Matchmaking server unreachable." |
| 4 | host offline / code stale | DELIVERs arrived but ALL were the zero-sentinel — a DELIVER carrying the joiner's OWN public IP counts as a sentinel too (review HIGH-1: it is the server echoing the joiner's attempt-1 registration back after the S2 fresh-socket retry re-REGISTERed a never-hosted key; a legitimate same-IP host is impossible because the hairpin bypass fails same-IP codes before the joiner ever REGISTERs) | `P2P_FAIL_HOST_OFFLINE` | "Host not found. Code stale or host offline." |
| 5 | host online, NAT-blocked | real-endpoint DELIVER arrived, bilateral punch timed out | `P2P_FAIL_NAT_BLOCKED` | "Host found, but NAT blocked the link." |
| 6 | symmetric-both / needs relay | as (5) + `StunResult.port_disagreement` (S2) | `P2P_FAIL_SYMMETRIC_BOTH` | "Both networks too strict (needs relay)." |
| 7 | hairpin / no NAT loopback | peer public IP == our public IP | `P2P_FAIL_HAIRPIN` | "Same network as host. Router lacks loopback." |
| 8 | host router blocks hosting | host advisory: no UPnP AND no inbound AND no DELIVER after 30 s (with UPnP: same silence logs `P2P_FAIL_RENDEZVOUS_DOWN` advisory, direct joins still work) | `P2P_FAIL_HOST_UNMAPPABLE` | "Router may be blocking hosting." |
| 9 | peer rejected (version) | MIST handshake reject (R-1), now routed through the same latch/report path | `P2P_FAIL_PEER_REJECTED` | MIST reason text |
| 10 | timeout at stage N | Part A deadlines, stage named | `P2P_FAIL_TIMEOUT_CONNECTING` / `P2P_FAIL_TIMEOUT_ORCHESTRATOR` | "Opponent never synced. Gave up." / "Connection setup timed out." |
| — | user abort / invalid code / local error | START hold, room-code decode, spawn/build failures | `P2P_ABORT_USER` / `P2P_FAIL_INVALID_CODE` / `P2P_FAIL_INTERNAL` | "Cancelled." / "Invalid room code." / "Internal error. See log." |

Every terminal outcome emits ONE attributed line
(`[netplay-connect] FAIL code=... msg=... t_ms upnp/stun/punch/signal/
bilateral ... deliver=any,real` — or `OK` with the same stage timings)
into the per-session netplay log, which `Netplay_LogConnectEvent` opens
LAZILY when the failure precedes `configure_gekko` (netplay.c
`netplay_log_open`, `{pref}/logs/netplay-<utc_ms>.log`).

### 5.3 UX defects fixed (Part C, inherited from the R-1 review)

- The MIST handshake runner no longer blocks the main thread for its
  500 ms per-attempt budget (~2 fps + ~2 Hz input for up to ~20-24 s of
  retries): it is an incremental per-tick pump over the `MistRunnerIo`
  seam (`mist_handshake_pump_begin`/`mist_handshake_pump`, one bounded
  slice per frame, never sleeps; `mist_handshake_run_attempt` remains
  as the blocking wrapper for its unit tests). On-screen text is now
  honest ("Verifying opponent (Ns)... START quits" / "Syncing with
  opponent (Ns)... START quits") instead of a perpetual "Match found!".
- Reject/failure reasons surface on ALL THREE entry paths (direct-P2P,
  matchmaking, LAN CLI): `DirectP2P_Init` — which registers the
  teardown callback converting the failure latch into
  `DIRECT_P2P_FAILED_HANDSHAKE` — now runs unconditionally in
  `set_netplay_params` (main.c).
- `DIRECT_P2P_FAILED_HANDSHAKE` (drawn right after a `Soft_Reset_Sub`)
  now sits behind the same `task[TASK_INIT].condition == 0` guard the
  nav overlay always had (netplay_screen.c) — no text draw during the
  unverified texcash re-init frames; FAILED_* states are sticky so the
  overlay appears as soon as init settles.

### 5.4 Tests

- test_bilateral_punch.c test 7: DELIVER tri-state split, full
  classifier truth tables (causes 1-8), deadline wrap-safety, abort-
  hold counter, machine-code uniqueness.
- test_stun_mock.c `run_discover_alldead_diag_test`: failed discovery
  carries evidence and classifies `P2P_FAIL_STUN_ALLDOWN`.
- test_mist_handshake.c (p1)-(p3): pump completes across ticks without
  ever sleeping; silent-peer timeout at 16 ms frame cadence with the
  full retransmit ladder; reject parity with the blocking runner.

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
| S2 punch / STUN mechanics | **implemented** (see §4; includes the unplanned STUN port-byteswap fix) |
| S3 no-hangs + failure taxonomy | **implemented** (see §5) |
| S4–S8 | planned above |
