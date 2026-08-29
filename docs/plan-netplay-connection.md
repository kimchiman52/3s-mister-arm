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
| `FAILED_STUN` | "Connection failed. Try again." | host: direct_p2p.c:279, 1025; thread-spawn failure paths in Begin* :1872/:1932; joiner: :1097 |
| `FAILED_PUNCH` | "Invalid room code." | BeginJoin decode failures, direct_p2p.c:4389, 4402 |
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

Real usage: the host reads the code aloud or pastes it in chat (11 chars
at the time this was measured; 18 as of v3, §6.3);
the friend types it in **minutes later**. By then the session is
un-pair-able on every path — and this is independent of NAT type.
Demonstrated end-to-end against the real server (see §3.4).

---

## 2. NAT-pair matrix (host × joiner)

Mechanics verified in code; classifications are standard NAT taxonomy.
"UPnP" means the host's router granted the mapping
(`try_portmap`, direct_p2p.c:2158-2367) and its external IP is not
provably non-public (CGNAT gate, §3.6; review M3).

| Host \ Joiner | Full-cone / restricted | Port-restricted | Symmetric |
|---|---|---|---|
| **UPnP mapped** | direct punch to mapped port succeeds | succeeds (joiner's own mapping opens on first send) | succeeds — mapped port accepts any source |
| **Full/restricted cone, no UPnP** | direct punch succeeds | succeeds | joiner's source port differs per destination → host's echo goes to the STUN-observed (wrong) port; **bilateral fallback required**, host-side learns true port from first inbound (host_tick_receive captures source, direct_p2p.c:1763-1782) |
| **Port-restricted, no UPnP** | succeeds | succeeds (simultaneous send opens both) | fails direct; bilateral gives the host the joiner's fresh mapping via DELIVER — works iff joiner's NAT maps the rendezvous-learned port for the host too (usually not, for true symmetric) |
| **Symmetric, no UPnP** | host's advertised STUN port is per-destination-wrong; joiner's punch lands on a dead mapping. Bilateral: server learns the host's port *toward the server*, still wrong toward the joiner → punching fails; **carried by the S5 relay (§7)** | same | punching **cannot** work here by construction; **carried by the S5 relay (§7)** — this was the one cell with no path at all before S5 |
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
(`host_stun_keepalive_tick`, direct_p2p.c:3917-3933;
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
`upnp_renew_tick`, direct_p2p.c:2425-2572), retry at 5 min on failure,
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
`UpnpMapping.external_ip` was captured (upnp.c:177) and never read.
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
  (direct_p2p.c:1101) wraps the extracted `join_attempt()`
  (direct_p2p.c:1110) and interposes exactly ONE automatic full retry
  on any terminal failure before surfacing it; each attempt re-runs
  discovery on local_port 0 with the previous socket closed, so the
  retry binds a FRESH local port (dodges stuck conntrack/NAT state;
  also covers host-still-in-UPnP-probe start-skew). (b) host —
  Tick's FAILED_STUN case (direct_p2p.c:3180) re-spawns
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
  down as a user cancel (`P2P_ABORT_USER`). Note (review L-5):
  TRANSITIONING's *loading* phase (waiting on
  `game_ready_to_run_character_select`) has no wall-clock deadline —
  the START-hold abort is the ONLY bound on it, and its status line
  advertises the abort ("Match found! Loading... START quits").

### 5.2 Failure taxonomy (Part B) — src/netplay/connect_fail.{h,c}

Machine codes are stable log-grep anchors; user strings fit the
overlay status line. Detection evidence, as implemented:

| # | cause | detection evidence | machine code | user string |
|---|---|---|---|---|
| 1 | no network / DNS dead | every getaddrinfo failed AND zero STUN replies (`StunResult.diag_*`) | `P2P_FAIL_DNS_ALLDOWN` | "No internet connection (DNS failed)." |
| 2 | STUN blocked | sends succeeded, zero responses from all servers | `P2P_FAIL_STUN_ALLDOWN` | "No STUN reply (UDP blocked or net down)." (hedged, review L-4: also covers ISP-down-LAN-up) |
| 3 | rendezvous server down | ZERO DELIVER frames for the whole signaling budget — the server answers EVERY REGISTER with a DELIVER (real or 0.0.0.0:0 sentinel; rendezvous-server.js handleRegister), so silence = server/path down. Requires the `Rendezvous_ParseDeliverEx` tri-state split (MALFORMED vs EMPTY vs PEER). Review L-3 honesty note: four live-server silent-drop cases (per-IP rate limit, per-IP live-key quota, paired-table-full, third-party drop) also land here — documented in connect_fail.h; a distinguishable NACK needs a wire change | `P2P_FAIL_RENDEZVOUS_DOWN` | "Matchmaking server unreachable." |
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

## 6. S4 — Security (IMPLEMENTED)

Three sub-stages, all landed. As-built below.

### 6.1 The three defects (as originally found)

1. **Anyone could be the opponent.** The host treated **any**
   non-'3SXR', non-STUN datagram as the peer, captured its source,
   echoed the payload, and handed the session off. The punch payload
   was the fixed literal `"3SX_PUNCH"` — no secret. So a blind attacker
   who guessed or observed the advertised `ip:port` got a handoff, and
   **one** stray datagram (a port scan) permanently consumed the host's
   only peer slot; the real joiner then failed and, after the MIST
   silence window, was told "opponent build may be too old".
2. **The room code determined every secret.** It was a bare reversible
   `(ip, port)` encoding, and BOTH derived secrets — the rendezvous
   session key and the punch token — were SHA-256 over that same
   6-byte payload. Anyone who could *guess* a plausible `(ip, port)`
   pair, with no code sighting at all, could derive the session key and
   squat or race the pairing.
3. **The rendezvous server never validated the sender's address.** It
   answered any well-formed packet, so a source-spoofed REGISTER bound
   a real slot (squat a victim's key; or take the joiner slot and have
   the server DELIVER the host a bogus endpoint, steering the victim's
   punch traffic at a third party), and the per-IP token bucket was
   bypassed for free by spoofing.

### 6.2 S4a — punch authentication (`7c9ae11b`)

- Punch payload is `"3SX_PUNCH"` + an 8-byte token derived from the
  room-code payload, domain-separated from the session key
  (`Rendezvous_DerivePunchToken`, rendezvous.c). 17 bytes total.
- `classify_host_datagram` (direct_p2p.c:787) is the single routing
  decision for every inbound datagram on the waiting host's socket:
  '3SXR' frame / STUN Binding Response / **authenticated** punch /
  IGNORE. **Fail closed** — no valid token, no acceptance. The IGNORE
  arm drops the datagram, does not echo, and **keeps waiting**
  (host_tick_receive, direct_p2p.c:2358-2380): the peer slot is never
  consumed. Pre-S4a that arm was "anything else IS the peer".
- The host's echo (which authenticates the host back to the joiner)
  only ever carries an already-validated payload.
- `Stun_HolePunch` accepts only source-IP + exact authenticated payload
  (constant-time token compare), with the port deliberately unmatched so
  the S2 symmetric retarget still works — now under auth. A punch-shaped
  datagram from the expected IP that fails the token check raises
  `StunResult.diag_punch_bad_token`.
- STUN transaction IDs now come from the platform CSPRNG
  (src/utils/csprng.c). SDL_rand was *not* unseeded; the real weakness
  is that it is an LCG whose state is recoverable from observed output,
  making txids predictable and Binding-Response forgery possible for an
  off-path attacker. RFC 5389 §6 requires cryptographic randomness.
- New cause `CONNECT_FAIL_PUNCH_AUTH` (connect_fail.h:115). It outranks
  the NAT diagnoses in the classifier: the peer was *reached*, so
  blaming NAT would send users to their router settings for nothing.

### 6.3 S4b — room code v2, superseded by **v3** (BREAKING, authorized)

**As shipped today (v3 — see §6.8 for why v2 was not enough):**

- 18 chars, displayed `XXXXXX-XXXXXX-XXXXXX`: version char `'3'` +
  80-bit payload `ip(32)<<48 | port(16)<<32 | nonce(32)` in 16 Crockford
  chars + ISO 7064 MOD 37,36 check digit over the 17 preceding chars.
- The **32-bit** nonce comes from the CSPRNG (`RoomCode_GenerateNonce`,
  room_code.c) and **hard-fails** when unavailable — no weak
  fallback, since a predictable nonce silently voids the entire point.
  It is mixed into the code *and* both derivations, domain-separated
  over the canonical 10-byte `ip[4]||port_be[2]||nonce_be[4]`:
  session key = `SHA-256("3SXR-SK3" || payload10)[0..15]`,
  punch token = `SHA-256("3SXR-PT3" || payload10)[0..7]`
  (rendezvous.c). `(ip, port)` alone no longer determines either.
- The '3SXR' **wire** version is unaffected and stays 2 — the session
  key is a 16-byte opaque blob to the server, so the server needs no
  change for a room-code format bump.
- The payload bitstream and the hash input are now the **same 10 bytes**
  (`room_code_pack_payload`), removing the "which packing did this side
  use" class of bug.

*(v2, for the record: 14 chars, `'2'` + `ip(32)<<28 | port(16)<<12 |
nonce(12)`, 8-byte payload, `"3SXR-SK2"` / `"3SXR-PT2"`. A
legacy-checksum-valid v1 (11-char) or v2 (14-char) code now decodes to
`ROOM_CODE_OLD_FORMAT`.)*
- **Honest scope**: the code still necessarily *contains* the host IP —
  the joiner has to reach it. The nonce protects the derived key
  material against guessing; it does not hide the IP from someone
  holding the whole code. Sharing a code on stream still exposes an IP.
- Decode validates the version char **before** the check digit (a future
  format's checksum scheme is unknowable) and returns a forced-handling
  enum — `OK` / `MALFORMED` / `OLD_FORMAT` (legacy-checksum-valid
  11-char v1 or 14-char v2) / `FUTURE_VERSION`. The bool API is gone, so
  a stale caller fails to compile rather than silently inverting. Version
  outcomes map to `CONNECT_FAIL_CODE_VERSION_OLDER` /
  `CONNECT_FAIL_CODE_VERSION_NEWER` (connect_fail.h; split from a single
  code by review finding L-4, §6.8) with explicit older/newer text.
  11- or 14-char input that does **not** pass
  the legacy checksum stays `MALFORMED`: garbage must never masquerade as
  a version issue.
- Host keeps the nonce stable across an S1 drift re-encode and draws a
  fresh one per hosting attempt.

### 6.4 S4c — rendezvous return-routability (`977ed7b7`)

Protocol **v2** on the '3SXR' wire. REGISTER/POLL are 36 bytes (was 28),
the extra 8 being a cookie tail; new server→client type 4 `CHALLENGE`
(32 bytes: magic(4) ver(1) type(1) reserved(2) key(16) cookie(8)).

**The gate.** `returnRoutabilityGate` (rendezvous-server.js:553) runs
between "the frame is well-formed" and "we touch any state", for both
REGISTER and POLL. An uncookied or invalid-cookied request is answered
with exactly one CHALLENGE and **binds nothing** — no session slot, no
creator quota, no rate budget, no table eviction, no endpoint
disclosure. Only the cookie **echo** binds. A spoofing sender has the
CHALLENGE delivered to the address it is impersonating, so it never
learns the cookie and can never bind.

**Cookie construction.**

| property | value |
|---|---|
| formula | `SHA-256(secret ‖ "addr:port:slot")[0..7]` (rendezvous-server.js:191) |
| secret | 32 bytes from `crypto.randomBytes` at process start, never leaves the process |
| inputs | source **address**, source **port**, rotation slot |
| size | 8 bytes |
| slot | `floor(Date.now() / 60000)` |
| accepted | current **and** previous slot ⇒ 60–120 s lifetime |
| compare | `crypto.timingSafeEqual` (rendezvous-server.js:205) |
| expiry cost | one extra challenge round; the C client answers within one RTT |
| restart | invalidates outstanding cookies; same one-round cost per live client |

**Replay, honestly.** The cookie is *deliberately* replayable by
whoever holds it, for as long as it lives. It proves **receipt, not
identity**. What bounds it: it is bound to `(address, port)`, so a
captured cookie is useless from any other endpoint; only an **on-path**
attacker can observe one, and an on-path attacker at endpoint E already
satisfies return routability for E by definition, so there is nothing
left to prove; rotation caps a leaked cookie at ≤120 s; and an off-path
attacker cannot forge one without the 32-byte secret. Cookies are **not**
an authentication mechanism — peer auth is S4a's punch token and the
S4b nonce-derived session key. A per-request nonce would add nothing
against the threat model and would cost the server its statelessness.

**Amplification.** CHALLENGE is 32 bytes for a 36-byte request:
factor **0.89**, a net attenuator, never worth aiming at a victim. It
is emitted under the existing per-IP bucket, so a spoofed flood at one
victim is capped at `RATE_LIMIT_PER_WINDOW` (10) challenges/second.

**Per-key rate cap.** `keyRateAllow` (rendezvous-server.js:311), 10/s
per session key, enforced **after** the cookie check so spoofed traffic
cannot burn a victim key's budget. This closes the "many real,
cookie-capable source IPs all hammering ONE key" bypass that the per-IP
bucket cannot see. A legitimate pair peaks around 2.5 pkt/s.

**Client side.** The joiner answers a CHALLENGE inline in its signaling
loop (one RTT to bind, instead of waiting out the 500 ms resend
cadence). The host receives CHALLENGEs on the **main** thread while
REGISTER resends are built on the rendezvous **worker** thread, so the
8-byte cookie crosses via a seqlock (`signal_cookie_publish`, direct_p2p.c:544;
`signal_cookie_snapshot`, direct_p2p.c:559) and the main thread also
echoes immediately (`host_handle_challenge`, direct_p2p.c:4300).
`Rendezvous_ParseChallenge` (rendezvous.c:195) validates magic, version,
type **and** that the frame carries *our* session key (cross-talk +
forgery gate — the key embeds the S4b nonce), and **zeroes its output on
every reject**, so a caller that ignores the return value cannot echo
attacker-chosen bytes.

**Cap-policy note.** The S1/H2 reasoning above (§ MAX_SESSIONS in
rendezvous-server.js) was written assuming spoofed floods were free.
They no longer are. `MAX_NEW_KEYS_PER_IP` and the
evict-oldest-unpaired-singleton policy remain load-bearing against a
real **botnet** whose nodes do receive at their own addresses and
therefore pass the cookie gate.

**New cause** `CONNECT_FAIL_COOKIE_REJECTED` (connect_fail.h:100,
`"P2P_FAIL_COOKIE_REJECTED"`, "Matchmaking auth failed. Update the
game."). A CHALLENGE is proof the server is alive, so
challenges-with-zero-DELIVERs is an auth/version problem — not the dead
server `RENDEZVOUS_DOWN` used to claim. Carried as
`ConnectJoinEvidence.challenge_any` into `ConnectFail_ClassifyJoin` and
as a new parameter to `ConnectFail_ClassifyHostWaiting`. Any DELIVER
outranks it (the cookie did bind, so the failure is downstream), and
hairpin still outranks everything.

### 6.5 Version interlock

`VERSION = 2` on both sides (rendezvous.c:28, rendezvous-server.js).
The server checks the version byte **before** the cookie gate, so a
mismatch never reaches state. Every combination:

| client | server | behavior |
|---|---|---|
| v2 | v2 | CHALLENGE → echo → DELIVER. Normal path. |
| **v1** | **v2** | Server drops on the version byte: logged, **no reply, no state, no timer, no hang**. The v1 client sees silence and its existing budget expiry reports `P2P_FAIL_RENDEZVOUS_DOWN`. |
| **v2** | **v1** | Symmetric — the v1 server drops version=2 and stays silent; the v2 client reports `RENDEZVOUS_DOWN` (it never sees a CHALLENGE, so it does **not** claim `COOKIE_REJECTED`). |
| v2 | v2, never challenges | Works. The client sends an uncookied (all-zero-tail) REGISTER and consumes a direct DELIVER. This is what the C test mocks do, and it is why the cookie tail must be zeroed rather than left as stack garbage. |
| v2 | v2, always challenges but never accepts | Client is challenged repeatedly, zero DELIVERs, budget expires → `P2P_FAIL_COOKIE_REJECTED` (not "server down"). |
| v3+ | v2 | Dropped on the version byte, same clean path as v1. |

This is a deliberate breaking change, explicitly authorized: the whole
alpha group ships the v2 client together, so old and new never need to
pair. There is no mixed-version window to support and no negotiation.

**Residual, stated plainly**: `RENDEZVOUS_DOWN` remains the catch-all
for every *silent* server-side drop — rate limiter, per-IP key quota,
per-key cap, paired-table-full, third-party drop, and version mismatch
all look identical to a client (total silence). Distinguishing them
needs a client-visible NACK, i.e. another wire change. The `CHALLENGE`
frame is the first crack in that: it is the one server-side condition a
client can now positively observe, which is exactly what makes
`COOKIE_REJECTED` reportable.

### 6.6 Tests

Each of the three sub-stages has at least one test proven to go **red**
against the pre-fix behavior (verified by neutralizing the fix, not by
assertion):

> **Correction (§6.8, HIGH-3).** The S4c red counts below were collected
> with a harness that aborted the whole run on the first thrown
> exception, so some tests recorded as red had in fact not executed. The
> per-test isolation and the `EXPECTED_TESTS` coverage literal landed in
> `77af7787`; re-measured counts are in §6.8. The S4b "four
> neutralizations" bullet also predates MEDIUM-1 — the check-digit
> recurrence it was exercising was itself defective.

- **S4a** — `test_bilateral_punch.c` test 10 (host datagram gate truth
  table) + `test_stun_mock.c` `run_punch_payload_test` /
  `run_punch_token_reject_test`. Restoring the pre-S4a final arm
  (`return DP2P_HOST_DGRAM_PEER_PUNCH`) turns 4 assertions red:
  wrong-token, legacy 9-byte punch, arbitrary garbage, and the
  no-token fail-closed case are all accepted as "the peer".
- **S4b** — `test_room_code.c`. Four independent neutralizations, four
  independent reds: check-digit verify → typo detection collapses;
  v1 recognition → `OLD_FORMAT` cases fail; constant nonce → the
  variability check fires; nonce dropped from the payload → round-trip
  mismatch on every nonce-bearing case.
- **S4c** — `tools/rendezvous-server/__test_protocol.js` (challenge
  required, cookie bound to source, spoofed source cannot bind, per-key
  cap, rotation window, v1 interlock) plus `test_bilateral_punch.c`
  test 11 (v2 cookie tail + ParseChallenge reject table) and tests
  7c2/7d2 (classifier truth tables). Disabling the cookie gate turns 19
  assertions red; disabling only `keyRateAllow` turns the cap
  assertion red (18 replies vs ≤11); disabling the version check turns
  the interlock assertion red; dropping the session-key gate in
  `Rendezvous_ParseChallenge`, the cookie-tail write, or the
  `challenge_any` classifier branches each turn their own assertions
  red.

**Harness defect fixed in passing.** `__test_protocol.js` exited 0 no
matter how many assertions failed — its forced-exit timer is `.unref()`'d
(so node drained and exited 0 before it fired) and the `_shutdown` path
it used ends in `socket.close(() => process.exit(0))`, whose hardcoded 0
raced ahead of the real code. The file could not report failure at the
shell level at all, which is how it stayed green through a v1→v2
protocol change. It now sets `process.exitCode` eagerly and closes the
socket directly. This is the reason the "prove it can go red" step is
worth doing on the harness itself and not only on the code under test.

### 6.7 Not addressed by S4

- The room code still contains the host's public IP, by necessity.
- The MIST handshake (netplay.c R-1 path) remains the backstop for a
  peer that gets past the punch gate.
- ~~Symmetric×symmetric pairs still cannot connect at all — that is S5.~~
  **Closed by S5** (§7): the relay rung now carries that cell.

### 6.8 S4 adversarial review — as-built fixes

S4 was re-read adversarially after landing. The three HIGH findings and
MEDIUM-1 / MEDIUM-3 are described below as shipped, across three commits
(`3b92669d`, `77af7787`, `adb63c3a`); the remaining findings are listed at
the end of this section.

**HIGH-1 — the punch gate was 12 bits deep and had no cap.** Two halves,
two commits.

- *Nonce width* (`3b92669d`). The S4a punch token is
  `SHA-256("3SXR-PT3" ‖ payload10)[0..7]`; an attacker who knows the
  advertised `ip:port` — which the code necessarily reveals — knows every
  derivation input except the nonce. At `ROOM_CODE_NONCE_BITS 12` that was
  a 4096-value search against a host that is a perfect oracle, drained at
  the ~60 datagrams/s `host_tick_receive` accepts: the whole space in
  ≤ 68 s, and under UPnP the advertised port is stable, so a past opponent
  could grind it months later. Now 32 bits (≥ 2.2 years at the same rate).
  The entropy has to live in the code: the joiner derives the token from
  the decoded code alone on the first direct punch, before any rendezvous
  exchange, and the session key is what *indexes* the rendezvous slot, so
  it cannot be delivered by the thing it indexes. At 5 bits/char, 32 nonce
  bits cost exactly 4 more payload chars (48 + 32 = 80 = 16 × 5) — the
  minimum possible. Format details in §6.3.
- *Host-side cap* (`adb63c3a`). `s_host_unauth_drops` was a log counter
  only: no per-source throttle, no backoff, no wall-clock budget, while
  the host answered every guess. Two levels, both in `direct_p2p.c`'s
  `host_punch_*` block. Per source: `HOST_PUNCH_SRC_MAX_BAD = 24` bad
  punches mutes that IP for `HOST_PUNCH_MUTE_MS = 60 s`, and a muted
  source is dropped **on the accept path** — no echo, no handoff, even
  when its token compare passes — so a correct guess stays
  indistinguishable from a wrong one. Per session:
  `HOST_PUNCH_TOTAL_REROLL = 64` bad punches re-rolls the nonce,
  re-encodes the code, re-derives the token, clears every mute, and
  restarts the rendezvous worker (whose session key is derived from the
  nonce), capped at `HOST_PUNCH_REROLL_MAX = 3` per hosting session. It
  fails **closed**: if the CSPRNG or a derivation is unavailable nothing
  changes, because advertising a code whose token we cannot derive would
  ignore every punch including the real joiner's.
- *Accounting rule*: only punch-shaped datagrams that **fail** the token
  check are charged. A joiner holding the right code is accepted on its
  first punch and is never charged, so no threshold here is reachable by a
  peer that can actually connect. Arbitrary garbage is logged but
  deliberately not charged — it teaches an attacker nothing and would let
  unrelated noise trip the re-roll.
- *What the user sees*: nothing for a mute (log only — a status line per
  blocked port-scanner is noise). On a re-roll, the code on overlay line 2
  changes and line 3 reads "Code was being probed. Share the NEW code.",
  the same mechanism and wording shape as the existing STUN-drift
  "Network changed! Share the NEW code."
- *Deliberate departure* from the review's literal "stop classifying that
  source for the session": the mute **expires** and a re-roll clears it. A
  permanent mute converts this defence into a self-inflicted lockout —
  friend typos the code, floods 24 bad punches, gets muted, is then read
  the code correctly and is dropped forever with no diagnosis. It is also
  arithmetically sufficient: 24 per 60 s is 0.4/s against an unthrottled
  60/s, so 32 bits needs ~340 years per source IP, and a 10,000-node
  botnet still needs ~12 days against a code that exists only while the
  OSD screen is up. Eviction picks the entry with the fewest strikes that
  is **not** currently muted, so rotating source addresses cannot clear a
  mute for free.

**HIGH-2 — a spoofer could lock a named host out of matchmaking for
~2.9 kbit/s** (`77af7787`). `rateLimitAllow` was the first statement in
`onMessage`, ahead of length/magic/version/gate and keyed on
`rinfo.address` only, which contradicted the file's own comments claiming
an uncookied request consumes no rate budget. Spoofed 36-byte REGISTERs
carrying the victim's IP exhausted `RATE_LIMIT_PER_WINDOW`, and the
victim's own correctly-cookied REGISTER then got zero replies and bound
nothing — the user sees `RENDEZVOUS_DOWN` and goes to check their
internet. `onMessage` now orders length → magic → version → type/length →
`returnRoutabilityGate` → handler, so nothing is charged to an IP until
the frame is well-formed. The gate charges **cookied** requests to
`rateLimitAllow` and **uncookied first contact** to a separate, much
larger `preGateAllow` bucket (`PREGATE_LIMIT_PER_WINDOW = 100`/s/IP) — a
different bucket, not a bigger one, so cookied traffic is structurally
immune to uncookied flooding rather than merely better funded. 100 is
sized off the 0.89 amplification factor: at the cap the victim receives
100 × 32 B/s while the attacker spends 100 × 36 B/s to elicit it, so the
attacker always pays more than the victim receives; legitimate need is one
challenge round at startup plus one per ≥ 60 s cookie rotation, which
covers ~6000 clients behind one NAT.

Consequence the review did not call out, caused by the reordering itself:
moving the limiter behind the frame checks removed the implicit 10/s/IP
cap that was throttling the pre-validation `logWarn` calls, turning a junk
flood into an unbounded synchronous console write — one denial of service
traded for another. Pre-validation logs now go through `noteThrottled()`,
keyed on a fixed set of reason strings (never attacker-supplied, so that
map cannot grow), at most one aggregated line per reason per 10 s.
**Operational note:** the per-packet `[CHALLENGE]` log line is now an
aggregated count every 10 s, not one line per packet.

**HIGH-3 — the protocol harness silently skipped tests** (`77af7787`).
`__test_protocol.js` wrapped every test in one `try`/`catch`, so the first
thrown "recv timeout" unwound the runner and skipped all remaining tests
while printing a misleadingly small failure count. Demonstrated: with the
server's version check disabled the run printed "1 assertion(s) failed"
while everything from `testLengthReject` through `testSweepHook` and
`testV1ClientInterlock` never executed — which means one "proven red"
claim in §6.6 was wrong, and the prove-it-can-fail methodology was
unreliable after the first throw. Each test now runs under
`runTest(name, fn)` with its own `try`/`catch`, records assertion failures
and throws separately, and resets server rate/session state pass-or-throw
so a dead test cannot poison its successors. `testsRun` is asserted
against a **literal** `EXPECTED_TESTS` (26), not `TESTS.length`, so a
silently skipped or quietly deleted test is itself a hard failure
(`COVERAGE FAIL`). The review said 25 tests; there were 23, plus 3 new.

**MEDIUM-1 — the check digit did not detect all single-character typos**
(`3b92669d`). v1/v2 computed the ISO 7064 hybrid recurrence with the
intermediate sum reduced mod 37 (M+1) instead of mod 36 (M), letting the
running product reach 0 as well as 36; `(37 - product) % 36` maps both to
check char `'1'`, collapsing 37 product states onto 36 outputs. The
documented "every single-character substitution is detected" guarantee in
`room_code.h` was therefore false. Measured against the shipped v2 codec:
**2508 of 1,960,000 substitutions (0.128%, ~1 in 781)** decoded silently
to a *different* endpoint. Worked counterexample, both `ROOM_CODE_OK`:

| code | decodes to |
|---|---|
| `248BVXBAA4DNM1` | 34.23.190.173 |
| `2E8BVXBAA4DNM1` | 114.23.190.173 |

i.e. a one-character typo aimed ~15 s of UDP punch traffic at an
uninvolved third party and then reported `HOST_OFFLINE`, never "Invalid
room code". v3 solves the check character correctly (sum mod M), whose
product range is [1, 36] and whose check map is a bijection. The old
recurrence is retained as `iso7064_legacy_compute` for exactly one
purpose — recognizing v1/v2 codes as `ROOM_CODE_OLD_FORMAT`. The
regression net is `test_room_code.c full_alphabet_sweep()`, which replaces
every position with every other character of the full 0-9A-Z alphabet:
0 undetected of 2,520,000 post-fix; 3484 of 2,520,000 (0.1383%) with the
recurrence neutralized back to mod 37. The previous `typo_detection` test
applied one hardcoded substitution per position and could never have seen
the collapse.

**MEDIUM-3 — unbounded spoofable pre-validation allocation** (`77af7787`).
`rateLimitAllow` created a `Map` entry before length/magic/version/gate;
measured, 1-byte junk from 5000 spoofed IPs retained 5000 entries ≥ 60 s,
which at 100k pps is ~6M live entries/minute → V8 heap exhaustion. Fixed
from both ends: the HIGH-2 reordering means junk now allocates nothing at
any rate, and all three buckets (`rateMap`, `preGateMap`, `keyRateMap`)
are hard-capped at `MAX_RATE_ENTRIES = 2 * MAX_SESSIONS = 8192` with O(1)
LRU eviction (JS `Map` insertion order *is* recency order when every hit
re-inserts, so `map.keys().next()` is the victim; a scan-for-oldest would
be O(n) per admit and would bolt a second DoS onto the fix for the first).
Eviction fails **open**, which is the correct direction: these buckets are
a courtesy throttle and the cookie is the authorization gate, so a flood
can dilute the throttle but can never use eviction to deny a real client.

**Not a wire change.** The '3SXR' protocol version stays **2** through all
of this. The session key is a 16-byte opaque blob to the server, so a
room-code format bump needs no server change; only the derivation domains
moved (`"3SXR-SK2"`/`"3SXR-PT2"` → `"3SXR-SK3"`/`"3SXR-PT3"`), which is
what stops a v2 payload landing in a v3 slot.

**Residuals, stated rather than hidden.**

- Any per-source pre-gate budget remains spoof-starvable for **first
  contact**, because first contact is by definition unauthenticated (see
  the comment at the `rendezvous-server.js` PREGATE block). What is now
  impossible is starving an established, cookie-holding client — the
  lockout the review actually found.
- A bounded mute is a deliberate trade: it caps the oracle without being
  able to permanently exclude anyone, so a sufficiently patient attacker
  is slowed by ~150×, not stopped.
- §6.7 still applies unchanged: the code contains the host IP by
  necessity, and symmetric×symmetric is still S5.

**Remaining findings.**

- **MEDIUM-4** (`bc8694cc`) — the room code became key material at S4b
  (it seeds both the session key and the punch token) but was still
  logged in cleartext, and alpha testers routinely hand log files over.
  `RoomCode_Redact` now masks the final `ROOM_CODE_REDACT_CHARS = 8`
  non-dash characters, which covers every one of the 32 nonce bits (the
  nonce is the low 32 bits of the 80-bit payload = the last 7 payload
  chars, plus the check digit that is a function of them). The version
  char and the ip+port characters survive — the half the joiner must know
  anyway, already printed in the clear on the same log line. Applied at
  the HOST_WAITING publish, the drift re-encode, and the handoff Join
  dispatch. The full code stays on the overlay.
- **L-3** (`bc8694cc`) — CSPRNG failure policy was inconsistent in the
  worst direction: `RoomCode_GenerateNonce` hard-failed while `stun.c`
  degraded silently to `SDL_rand`. On a device without `/dev/urandom` the
  HOST aborted hosting while the JOINER carried on with predictable
  transaction IDs — reintroducing exactly the RFC 5389 §6 forgery
  weakness S4a set out to remove, on the side least able to notice. Both
  now fail closed and loud; `Stun_Discover` refuses early with a new
  `diag_csprng_fail` that classifies as `CONNECT_FAIL_INTERNAL` rather
  than a connectivity diagnosis (nothing ever left the machine). The
  `static bool warned` one-shot, previously raced between the discover
  worker and the main-thread keepalive, is now a CAS.
- **L-4** (`bc8694cc`) — `P2P_FAIL_CODE_VERSION` split into
  `..._OLDER` / `..._NEWER`. The two answers send opposite people to
  opposite actions (with OLDER the code's *creator* must update; with
  NEWER the person typing it must), which is the entire question a
  support thread has to answer.
- **L-5** — room-code length/nonce drift across the older plan docs;
  documentation-only, corrected or annotated as superseded.
- **MEDIUM-5** — the documented netplay-test build recipe omitted
  `NETPLAY_TEST_HOOKS` and therefore did not compile (configures clean,
  then ~20 errors). Corrected in the four affected docs. The source-side
  half: `test_stun_mock.c` used to print one quiet line, still exit 0 and
  still claim "+ discover passed" when built without the hooks, so four
  `run_discover_*` tests validated nothing while looking green — it now
  reports INCOMPLETE and fails.
- **MEDIUM-2** — integration coverage for the S4c client cookie
  handshake, on both roles; lands in a sibling commit.

## 7. S5 — Custom '3SXR' relay for symmetric-NAT pairs (IMPLEMENTED)

Symmetric×symmetric is the one cell of the §2 matrix that cannot
connect at all: neither side can predict the other's per-destination
port, so no amount of punching works and the flow dead-ends at
`FAILED_BILATERAL`. A relay is the only fix. Landed as its own commit
series on top of S4; citations refer to the post-S5 tree.

### 7.1 Why a custom relay and not coturn

`do_handoff` (direct_p2p.c) transfers a **bare** `NET_DatagramSocket`
into netplay.c via `Netplay_SetStunSocket`, after which GekkoNet owns
plain send/recv on it through `sdl_net_adapter.c`. A TURN allocation
interposes Allocate / CreatePermission / ChannelData framing on **every
packet**, plus refresh timers — on a 60 Hz rollback game's hot path,
through an adapter that does nothing but copy bytes. GekkoNet does not
speak any of it. A relay that forwards **raw datagrams** keeps the
socket bare, so the relayed socket is behaviourally identical to a
punched one and neither GekkoNet nor the MIST handshake changed a line.
Secondary reason: coturn would add a scanner-recognisable public
service to a VPS that also runs unrelated infrastructure.

That identity claim was **verified, not assumed**:

| link | evidence |
|---|---|
| `do_handoff` never pins a peer at the socket layer | it calls only `Netplay_SetParams(player, ip)` / `Netplay_SetRemotePort(port)` / `Netplay_SetStunSocket(sock)` — no `connect()` |
| outbound goes to exactly one endpoint | netplay.c stringifies `"remote_ip:remote_port"` for GekkoNet (`configure_gekko`); `sdl_net_adapter.c` `send_data` resolves that string ONCE into `cached_remote`/`cached_port` |
| inbound is source-agnostic | `receive_data` accepts datagrams from any source and labels each with the datagram's OWN source string |
| so the addresses match | the relay forwards **from** the endpoint we send **to**, so the address GekkoNet sees is the one it expects |
| MIST handshake unaffected | `netplay.c`'s `MistRunnerIo` sends to `remote_ip:remote_port` and replies to `io->last->addr/port` — the same shape |

One guard was needed to make the claim airtight: a `RELAY_PIN_ACK`
still in flight when `do_handoff` transfers the socket would land in
`receive_data`. Its first byte is `0x33`, and `0x33 & 7 == 3`, so it
would be miscounted as an `InputAck` in the diag counters and then
handed to GekkoNet as a packet of type 51 — outside the 1..7
`PacketType` range (`third_party/GekkoNet/build/include/net.h:28-36`).
`receive_data` now drops every '3SXR' frame, mirroring the existing
`Stun_IsBindingResponse` straggler drop. The test is **exact, not
heuristic**: a GekkoNet type byte is 1..7 by construction and can never
be `0x33`, so this can only ever catch our own control traffic.

The drop is on the **magic alone** (`Rendezvous_HasMagic`), and that
correction is review LOW-1. It used to test
`Rendezvous_FrameType(...) != 0`, and `Rendezvous_FrameType` returns 0
for **any** version ≠ 2 — so a non-v2 '3SXR' frame would have passed
through the guard with `data[0] == 0x33` and been miscounted as an
`InputAck`, which is precisely the bug the guard exists to close.
Unreachable today (this build only ever emits v2), but the word "every"
above is now true rather than aspirational.

### 7.2 Protocol

Four new types on the existing '3SXR' wire, same UDP port, same process,
same systemd unit. **The wire VERSION stays 2** — a client that never
sends a `RELAY_REQ` is indistinguishable from a pre-S5 one, so this is a
pure extension and no row of the §6.5 interlock matrix moves.

| type | dir | len | layout |
|---|---|---|---|
| 5 `RELAY_REQ` | client → main port | 36 | byte-identical to `REGISTER` apart from the type byte |
| 6 `RELAY_GRANT` | server → client | 36 | magic(4) ver(1) type(1) slot(1) status(1) key(16) port_be(2) rsv(2) token(8) |
| 7 `RELAY_PIN` | client → relay port | 20 | magic(4) ver(1) type(1) slot(1) rsv(1) token(8) rsv2(4) |
| 8 `RELAY_PIN_ACK` | relay port → client | 12 | magic(4) ver(1) type(1) slot(1) peer_pinned(1) rsv(4) |

`RELAY_REQ` is byte-identical to `REGISTER` in the fields the S4c
return-routability gate reads (key at `[8..24)`, cookie at `[28..36)`,
length 36) **on purpose**: it rides the same gate rather than needing a
second one. The client builds it *through* `Rendezvous_BuildRegister`
and retypes byte 5, so the two can never drift apart.

`RELAY_GRANT` carries **no relay IP**. The client uses the address the
GRANT arrived at — which is the address it was already talking to.
36-for-36 makes the amplification factor exactly **1.0**; `PIN_ACK` is
12-for-20, an attenuator. Neither can make this server a reflector.

Refusals reuse the GRANT frame: `status` is `GRANTED` (0),
`POOL_EXHAUSTED` (1) or `NOT_PAIRED` (2), with port and token zeroed and
`slot = 0xFF`. An explicit refusal exists because a client that cannot
tell "refused" from "server gone" is exactly the reporting defect §6.5
documents.

**Token.**

| property | value |
|---|---|
| formula | `HMAC-SHA256(secret, "relay:<hexKey>:<side>:<slot>")[0..7]` |
| secret | the **same** 32-byte `cookieSecret` S4c already draws at process start — one secret, not a second scheme |
| domain separation | the literal `"relay:"` prefix, so a cookie can never be replayed as a token or vice versa |
| slot | `floor(Date.now() / RELAY_TOKEN_ROTATE_MS)`, 60 s |
| accepted | current **and** previous slot ⇒ 60–120 s. **That pair of slots IS the expiry** |
| side | `0` = slot A (host), `1` = slot B (joiner), and it is **inside the HMAC** — a token cannot be moved across sides |
| compare | `crypto.timingSafeEqual` |

**Admission — no open reflector.** `handleRelayReq` runs after the S4c
gate, so the source has already proven return routability. Then: the key
must exist; the source must **be** one of that session's two registered
slots (which is what identifies *which* side is asking, and is
unsatisfiable without having completed normal pairing); and the session
must be **paired**. Unknown key and non-slot source get **silence** —
answering would confirm to a scanner which keys exist. An unpaired
session gets a `NOT_PAIRED` refusal, because that requester is provably
a participant.

**Pin source binding (review HIGH-1, fixed as-built).** A token is a
capability for `(hexKey, side, slot)` and **nothing else** — it says who
told you, not who is holding you. `RELAY_PIN` originally recorded
`rinfo.address/port` verbatim, so a party legitimately holding a side-0
token (trivially arranged: create your own session with two of your own
sockets) could present it from a **spoofed source**; the relay pinned
side 0 to an arbitrary victim and then forwarded everything the attacker
sent as side 1 to that victim at up to `RELAY_BYTES_PER_SEC`.
Reproduced: 200×1200 B offered → **54 datagrams / 64800 B** delivered to
the victim, plus an unsolicited `PIN_ACK`. 1:1, so source-laundering and
VPS-uplink burn rather than classic amplification — an off-path-drivable
reflector all the same. The relay now requires the pin source to be at
the **registered slot IP** for that side (live `sessionMap` entry first,
with a snapshot taken at grant time as the fallback for a relay that has
outlived its session). An off-path attacker cannot put a victim's IP in
a slot: that requires answering a `CHALLENGE` delivered to the victim.

**The match is IP-only and must stay that way.** A symmetric NAT hands
out a *different* mapping toward the relay port than toward the
rendezvous port — which is precisely the case this whole stage exists
for. Requiring the port to match would break S5 for exactly the users it
was built for. Both halves are asserted:
`testRelayPinSourceBoundToSlotIp` fails if a non-slot IP can pin **and**
fails if the slot IP from a different port cannot. Neutralising the fix
into the port-matching version reproduces the second failure verbatim.
Running the check **before** the HMAC is also review MEDIUM-3's cheap
path: an unknown source now costs a Map lookup and a string compare
instead of up to two HMAC-SHA256 plus a `timingSafeEqual`.

**Pinning.** The relay pins the first token-bearing source on each side.
The same endpoint re-pinning is idempotent and re-ACKed — that is how a
client learns its peer arrived, via the `peer_pinned` flag, with no
extra frame. A valid token from a **different** endpoint for an
already-pinned side is refused: holding a token cannot hijack a live
side. Everything that is not a valid `RELAY_PIN` is forwarded verbatim
or dropped, never interpreted — a `'3SXR'`-magic application payload is
forwarded like any other bytes (asserted by `relayGrantAndForward`).

### 7.3 Port pool, bandwidth, reclaim

| policy | value | rationale |
|---|---|---|
| port pool | UDP **34000–34099**, one port per relayed **session** | the port IS the session identifier, which is what keeps the forward path a bare "send these bytes to the other pinned endpoint" with no header of our own |
| bind failure | port blocklisted for `RELAY_PORT_BLOCK_MS` (5 min), scan **retries for the same request** | see below — nothing is granted for a port that is not listening, and the blocklist is not permanent |
| capacity | 100 concurrent relayed sessions; at cap → `POOL_EXHAUSTED` | |
| bandwidth | **64 KiB/s per session**, token bucket, one-second burst | ~**7.8×** headroom over a real match; bounds what one session can cost the box |
| over budget | **drop the datagram**, and still refresh liveness | never teardown: rollback netcode absorbs loss, a mid-match teardown is unrecoverable |
| idle reclaim | 30 s with no pin and no forwarded datagram, on the existing 5 s sweep | 100 ports is small enough that holding dead entries for the 10-minute `SESSION_TTL_MS` would exhaust the pool |
| session release | frees its relay **only if that relay is already idle** | see below — `sweepRelays` owns relay lifetime exclusively |

**The bandwidth headroom is ~7.8×, not ~12×** (review MEDIUM-5). The old
figure compared the wrong quantities: `relayBandwidthAllow` is **one**
bucket per session charged for **both** directions, and the ~5 kB/s
input was an estimate. Shipped telemetry says **~4.2 kB/s per
direction** (`kbps_tx=4.2` in the heartbeat sample at
docs/netplay-diagnostics.md:32, from `net_stats.kb_sent` at
src/netplay/netplay.c:1372-1373) → ~8.4 kB/s against 65536 B/s =
**~7.8×**. Still ample — a relayed match uses about an eighth of its
budget — but the number should be the measured one.

**The relay ports are rate-limited too** (review MEDIUM-3, fixed
as-built). They had **no** limiter of any kind while the main port has
three layers, and every PIN-shaped datagram cost up to **two**
HMAC-SHA256 (`relayTokenValid` iterates the current and previous slot)
plus a `timingSafeEqual`, unmetered, from any source that found the port
— across 100 discoverable open UDP ports. Two mechanisms, in order:
the HIGH-1 source check runs **before** the HMAC, so a scanner costs a
Map lookup and a string compare rather than a hash; and what survives it
draws on a `RELAY_PIN_RATE_PER_SEC` (40/s) token bucket **per relay**.
Per relay, not per source, on purpose: a per-source bucket is keyed on
an attacker-chosen, spoofable value and is its own unbounded allocator —
the exact mistake the main port already had to unlearn
(`MAX_RATE_ENTRIES`). Sizing: the client pins at `RELAY_PIN_RESEND_MS`
= 150 ms per side, ~13.3/s across both, so 40/s is ~3× the real peak;
worst case for the box is 100 × 40 × 2 = 8000 HMAC-SHA256/s over
~40-byte inputs. Over budget drops, never tears down.

**Bind failure: the blocklist is time-boxed and the grant waits for the
socket** (review MEDIUM-1 + MEDIUM-2, fixed as-built). Two coupled
defects on the same path.

*MEDIUM-1* — `relayPortBlocked` was a `Set` and therefore **monotonic**.
Its only `clear()` lived inside the `_resetRelays` **test hook**: zero
production clear sites. Any transient bind failure — including the
plausible release→re-allocate `EADDRINUSE`, since libuv defers the real
close past `socket.close()` — removed that port for the lifetime of the
process, so over a long-lived systemd unit the pool ratcheted toward
zero and everyone eventually got `POOL_EXHAUSTED`. It also fired on
**any** `socket.on('error')`, not just bind errors, so a runtime error
on a perfectly good port threw that port away too. It is now a `Map` of
expiry timestamps re-tried after `RELAY_PORT_BLOCK_MS`, written only for
a pre-`listening` `EADDRINUSE` / `EACCES`.

*MEDIUM-2* — the documented recovery ("the client's next `RELAY_REQ`
resend, 300 ms cadence, then draws a different port") **does not exist
in the shipped client**: `direct_p2p.c` breaks its phase-1 loop the
instant `granted` is set, and phase 2 only ever sends `RELAY_PIN`. A
bind failure therefore handed out a `GRANT` for a port that would never
listen, the client burned its whole pin budget against it, and the rung
reported `RELAY_PIN_TIMEOUT` → *"Relay unreachable (firewall?)"* —
pointing the user at their own router for a server-side bind failure,
exactly the misreporting class §6.5 exists to prevent. `relayAllocate`
is now callback-based: nothing is granted until the socket's
`listening` event fires, and a bind failure re-enters the port scan for
the waiting request (the failed port is blocklisted, so the retry
necessarily draws a different one; the finite pool bounds the chain at
`POOL_EXHAUSTED`). The old round-trip argument for granting early bought
nothing — bind resolves on the next event-loop turn, far inside the same
round trip it was reasoning about. Fixed server-side on purpose: the
client keeps its single-request/single-answer shape and did not change.
`testRelayBindFailureStillGrantsAWorkingPort` occupies the first pool
port for real and asserts the client gets one grant, for a different
port, that answers a `PIN` and carries bytes.

**"Drop, never teardown" is now actually true** (review HIGH-2, fixed
as-built). `relay.lastActivity` was refreshed *after* the budget check,
so the `dropCap` path returned first and a dropped datagram did not
count as life. A session persistently over `RELAY_BYTES_PER_SEC` for
`RELAY_IDLE_MS` was therefore reclaimed by `sweepRelays` as "idle" while
carrying constant traffic — a mid-match teardown by the exact mechanism
the drop policy exists to avoid. Reproduced. The refresh now happens
before the budget check, for any datagram between two pinned endpoints:
a datagram is evidence of life whether or not we choose to carry it. It
stays *below* the "peer has not pinned yet" check on purpose, so a
half-open relay still ages out on `RELAY_IDLE_MS` — which is what the
idle-reclaim row says, and what stops a single client parking a pool
port indefinitely now that the session TTL no longer bounds relay
lifetime.

**Relay lifetime is the relay's own clock, and only its own clock**
(review CRITICAL-1, fixed as-built). `releaseSession` used to call
`relayRelease` unconditionally, which killed **every relayed match at
exactly `SESSION_TTL_MS`**. Nothing refreshes a session's `lastTouch`
during a relayed match: only `handleRegister` / `handlePoll` /
`handleRelayReq` touch it, and after the handoff neither client ever
speaks to the main rendezvous port again (the joiner returns
`DIRECT_P2P_HANDOFF` and its worker ends; the host raises
`s_bilateral_handoff_pending` and its punch worker returns, the
rendezvous worker having already exited). So `lastTouch` froze at the
last `RELAY_REQ` — at *setup* — and ten minutes into gameplay the 5 s
sweep closed the relay socket and returned the port to the pool. Both
clients hold NAT mappings only toward that relay endpoint, so both went
instantly silent, mid-match, unrecoverably. Reproduced against the real
module: at the boundary the relay's own `lastActivity` was **2 ms** old,
so the idle reclaim would never have fired; this was purely the session
TTL. `releaseSession` now skips `relayRelease` while
`now - lastActivity < RELAY_IDLE_MS`, so the pool costs at most 30 s of
one port and the correct clock — the one that forwarded traffic
refreshes ~120 times a second — is the only one that can end a match.
The irony worth not repeating: the idle-reclaim rationale directly above
reasons about `SESSION_TTL_MS` and still missed the reverse coupling.
Check **both** directions of a lifetime dependency.

**Deployment**: the firewall must allow inbound UDP on the pool range in
addition to the rendezvous port. `start()` logs the range at boot.

### 7.4 The client rung

Position in the cascade: the **last** rung, after the bilateral punch
fails, on **both** roles.

- **Joiner** — `join_attempt`, the `!bilateral_punched` arm. Runs inline
  on the join worker, the socket's sole actor, like the signaling loop
  above it. `signal_addr` is now kept ref'd past the bilateral punch (the
  rung needs it); every exit from that point Unrefs exactly once.
- **Host** — `host_bilateral_punch_thread_fn`, the `!punched` arm. **Not
  Tick**: that thread already owns the socket exclusively for the whole
  `FALLBACK_BILATERAL_PUNCH` phase (§Decision 3), and the rung blocks for
  up to its budget while `DirectP2P_Tick` must never block. It rebuilds
  the signal endpoint and the advertised-tuple session key locally (the
  rendezvous worker has exited by then) and reads the S4c cookie through
  the existing seqlock.

**`do_handoff` is reused verbatim.** Both success paths write the relay
endpoint into `s_work.peer_ip` / `peer_public_port` exactly where the
punch-success path writes the peer endpoint — the joiner returns
`HANDOFF`, the host raises `s_bilateral_handoff_pending` — so Tick's
existing main-thread handoff carries it with **no new plumbing at all**.
The only difference is which endpoint was written.

**Not gated on `port_disagreement`.** That S2 flag is a hint, not a
diagnosis (a symmetric NAT that happens to hand two STUN servers the
same port never raises it), and a relay works for any pair that can
reach the server. Gating on it would strand exactly the users the rung
exists for.

> **Superseded by S6 (review finding L-1).** This section used to end
> "A pair that *can* punch never reaches this code, so an enabled relay
> costs a connectable pair nothing." That was true of the S5 **serial**
> cascade, where the rung only ran after the punch had spent its whole
> window. It is **not** true after S6: the relay leg now RACES the punch
> legs, so a pair that punches slowly does reach this code and does spend
> a pool port. §8.4 rules 2 and 2b bound how much: the relay may not arm
> until every live punch candidate has had a window of its own, and even
> once it is ready to hand off it waits `RACE_RELAY_GRACE_MS` for a punch
> to land. What survives of the original promise is the measurable part —
> a pair that punches inside those windows still costs the pool nothing
> (test 20A asserts zero `RELAY_REQ`s) — and the honest residual is in
> §8.10.

**Relay address provenance.** Taken from the `PIN_ACK`'s **own source
address**, not from `signal_addr`. `signal_addr` may have come from a
hostname, and netplay.c re-resolves `remote_ip` later — a round-robin
answer would then point at a box that never pinned us, and the relay
would drop everything as unpinned traffic.

**Pin-then-handoff.** We hand off once our OWN pin is acked, and keep
pinning until the ACK reports the peer pinned too (or the budget ends).
Requiring `peer_pinned` as a hard precondition would deadlock the
symmetric case *both* sides are in — each waiting for the other. Handing
off early is safe: the relay drops our frames until the peer pins,
GekkoNet retransmits `SyncRequest` every 200 ms, and the S3
`CONNECT_TIMEOUT_CONNECTING_MS` deadline reports honestly if the peer
never arrives.

**Config.**

| key | default | purpose |
|---|---|---|
| `netplay-direct-p2p-disable-relay` | `false` | kill switch, mirrors `disable-bilateral` |
| `netplay-direct-p2p-force-relay` | `false` | **test override, `NETPLAY_TEST_HOOKS` builds only** (review LOW-2): no-ops both hole punches and skips the bad-token / kill-switch / LAN / hairpin bypasses (joiner) and the LAN DELIVER bypass (host), so the relay path can be exercised on demand without arranging two symmetric NATs. A shipping build ignores the key and logs once — it was a user-settable knob that disabled **all** direct connectivity, same-LAN included, and routed every match through a European VPS; there is no player-facing reason to set it and several ways to set it by accident. The key stays registered in every build so a config file carrying it still parses. |
| `netplay-direct-p2p-relay-budget-ms` | `4000` | whole-rung wall clock, clamped [500, 20000]; half to REQ→GRANT, the rest (≥ 500 ms) to PIN→ACK |

### 7.5 Taxonomy and what the user sees

`CONNECT_FAIL_SYMMETRIC_BOTH` keeps its meaning ("needs relay") but is
**no longer a dead end** — it now leads to an ATTEMPT, and is reported
only when the rung is switched off by configuration. Three new causes for
the rung's own failures, kept separate because they send the reader to
three different places:

| # | cause | machine code | user string | evidence |
|---|---|---|---|---|
| 11 | no relay available | `P2P_FAIL_RELAY_UNAVAILABLE` | "No relay available. Try again later." | `RELAY_REQ` never answered (relay off server-side, older server, or we are no longer a registered slot) |
| 12 | relay allocation refused | `P2P_FAIL_RELAY_REFUSED` | "Relay is full. Try again shortly." | a GRANT carrying an explicit refusal; transient and retryable, unlike a NAT verdict |
| 13 | relay pinning timed out | `P2P_FAIL_RELAY_PIN_TIMEOUT` | "Relay unreachable (firewall?)." | granted a port, then no `PIN_ACK`: the relay port RANGE is unreachable from here — the rendezvous port demonstrably worked |

A rung that RAN and failed reports **its own** cause rather than the NAT
verdict. The NAT diagnosis stays true but stops being actionable once the
last rung has been tried, and sending a user to their router because the
relay pool was full would be a lie. The NAT evidence survives in the
report line's `portdis=` field. The host's M1 retry cap applies the same
preference via `s_work.relay_fail_code`.

**On screen** (S3 status line, overlay line 3):

- `"Connecting via relay..."` while the rung runs;
- `"Connected via relay (higher ping)."` at handoff.

Honest by design: the VPS is in Europe, so two US players on the relay
see noticeably worse ping than a direct link, and they should know which
one they got.

**In the log**, both report lines gained:

- `via_relay=0|1` — which rung won. Named apart from the `relay=` **stage
  timing** in the same line so a grep cannot confuse them.
- `relay_fail=<code>` — distinguishes "the rung never ran" (`P2P_OK`)
  from "it ran and failed like this", which the top-level code alone
  cannot say once the host's M1 retry loop has folded several attempts
  into one terminal verdict.
- a `relay=` entry in the `t_ms` stage block.

Without these, a relayed session's structurally worse ping would make
every field latency complaint unattributable.

### 7.6 Tests

**Server** — `tools/rendezvous-server/__test_protocol.js`, six new cases
following the file's per-test `runTest` structure (`EXPECTED_TESTS`
26 → **32**; the reset hook now also clears relays so a leak cannot
poison a later test). Each is **proven red by neutralising the code under
test**, not by assertion:

| neutralisation | red |
|---|---|
| `RELAY_REQ` dispatch arm removed (true pre-S5 server) | all 6 |
| non-slot admission gate removed | `relayRequiresPairedSession` (2) |
| forward `send` removed | `relayGrantAndForward` (recv timeout) |
| PIN token check removed | `relayPinTokenRequired` (7) |
| bandwidth cap removed | `relayBandwidthCap` (2) |
| pool-size cap removed | `relayPoolExhaustion` (4) |
| idle reclaim removed | `relayIdleReclaim` (3) |

The adversarial review added **six more** (`EXPECTED_TESTS` 32 → **38**),
each proven red the same way — and two of them by neutralising the fix
into the *plausible wrong* fix rather than into the pre-fix code, which
is the stronger proof:

| test | neutralisation | red |
|---|---|---|
| `relaySurvivesSessionTtl` (CRITICAL-1) | the skip-active-relay guard removed (true pre-fix) | 4, including both end-to-end forwarding assertions |
| `relayPinSourceBoundToSlotIp` (HIGH-1) | source gate removed (true pre-fix) | 6, incl. *"reached the victim ZERO times (actual=54)"* |
| " | **the wrong fix**: require the port to match too | 4, incl. *"the slot IP from a DIFFERENT PORT pinned side 0 (symmetric NAT must still work)"*, plus 3 in `relayBandwidthCap` |
| `relayBandwidthCap` +3rd act (HIGH-2) | the original `lastActivity` ordering restored | 2 |
| `relayPortBlocklistExpires` (MEDIUM-1) | monotonic blocklist | 2, incl. *"an EXPIRED block returns the port to the pool (actual=34001 expected=34000)"* |
| " | blocklist on **any** socket error | 1 |
| `relayBindFailureStillGrantsAWorkingPort` (MEDIUM-2) | the synchronous grant restored | 4, incl. *"the grant names a port that is NOT the unbindable one (got 34000)"* and *"the granted port answered a PIN"* — i.e. the `RELAY_PIN_TIMEOUT` misreport itself |
| `relayPinRateCap` (MEDIUM-3) | the pin bucket removed | 2 |
| `keyBudgetCoversTheRelayRung` (MEDIUM-4) | `KEY_RATE_LIMIT_PER_WINDOW` back to 10 | 3, incl. *"every one of the 8 RELAY_REQs ... was answered (actual=7 expected=8)"* |

`relayBindFailureStillGrantsAWorkingPort` occupies pool port 34000 with
a real socket, so the bind failure it recovers from is a real libuv
`EADDRINUSE`, not a mock.

**Client** — `test_bilateral_punch.c` **test 16**, driving the REAL
`BeginJoin` through the whole cascade against a mock server that also
speaks the relay extension: 16A grant+ack → HANDOFF, 16B silent →
`RELAY_UNAVAILABLE`, 16C refused → `RELAY_REFUSED`, 16D granted-then-
silent → `RELAY_PIN_TIMEOUT`. The load-bearing assertion is on
**`do_handoff`'s own arguments** (`DirectP2P_TestHook_LastHandoff`), not
on internal state — a rung that populated `s_work` correctly but never
reached the handoff would pass an `s_work` assertion and cannot pass this
one. Both punches are failed through the **force-relay override**, which
simultaneously exercises that shipped knob and proves it really no-ops
them (`s_mock_punch_calls == 0`). Five neutralisations, five reds:

| neutralisation | red |
|---|---|
| `relay_enabled()` → false (pre-S5 client) | 16A/B/C/D; B/C/D show the exact pre-S5 dead end, `code=P2P_FAIL_NAT_BLOCKED` |
| `relay_forced()` → false | `16A-punches-skipped` |
| relay endpoint writeback removed | 16A: *"do_handoff got 198.51.100.7:6000, expected the relay endpoint 127.0.0.1:58184"* — i.e. it handed off to the unreachable peer, the precise bug the rung exists to prevent |
| `set_fail(rc)` → `set_fail(NAT_BLOCKED)` | 16B/C/D on both the status string and `code=` |
| `via_relay=` dropped from the report lines | 16A on the OK line, 16B/C/D on `via_relay=0` |

Test 17 (relay codec vs literal bytes) gained a `Rendezvous_HasMagic`
block for review LOW-1, red under the old version-gated semantics on
`17-magic-wrong-version-still-dropped` — the exact frame that used to
slip past the straggler drop.

**Where the review's two "unshippable" verdicts landed.** The reviewer
passed the core correctness question — the relayed socket really is
behaviourally identical to a punched one, verified link by link, and no
relay frame can reach GekkoNet's deserializer — and the two teardown
defects it found (CRITICAL-1, HIGH-2) are both now regression-tested
against the *behaviour a player experiences*, not against internal
state: "bytes still cross after ten minutes" and "a session over budget
is not reclaimed as idle".

### 7.7 Residuals, stated rather than hidden

- **The S2 fresh-socket retry can strand the rung.** The joiner's
  automatic retry binds a fresh local port, so its attempt-2 REGISTER
  arrives from a new source endpoint while attempt-1's slot B is still
  inside `SLOT_STALE_MS` (30 s) — the server files it as a third party
  and drops it. That joiner is then not a registered slot, so its
  `RELAY_REQ` is (correctly) met with silence and classified
  `RELAY_UNAVAILABLE`. Attempt 1's relay rung is the one that matters,
  and it is unaffected. Pre-existing S2 × slot-policy interaction, not
  introduced here; fixing it belongs with the slot policy.
- **Latency is worse, by construction.** Two US players relayed through a
  European VPS pay the detour. That is the trade for connecting at all,
  and §7.5 makes sure they can see it.
- **The relay trusts the pin, not the payload.** Once both sides are
  pinned the relay forwards raw bytes without inspection — which is the
  entire point, and means an **on-path** attacker who can spoof a pinned
  source endpoint can inject into the stream. That is the same exposure a
  punched UDP link already has; peer authentication remains S4a's punch
  token and the MIST handshake. This residual is **on-path only**, and
  said so imprecisely enough before review HIGH-1 that it read as
  covering the off-path case too. It never did: the off-path
  *establishment* attack — spoofing the PIN itself to aim the relay at a
  third party — was a real defect, and is fixed above by binding the pin
  source to the registered slot IP. What remains is strictly injection
  into an already-established stream by someone already on its path.
- **`RELAY_PIN_ACK` is not session-key-authenticated.** The ephemeral
  relay port is the session identifier and the ACK steers nothing —
  forging one (having guessed the port) only makes a client hand off a
  few hundred ms early, a state GekkoNet's retransmit and the S3
  CONNECTING deadline already cover.

## 8. S6 — Joiner candidate racing (IMPLEMENTED)

Landed as its own commit series on top of S5. Citations refer to the
post-S6 tree.

### 8.1 The defect: everything was serial

The joiner ran its establishment phases strictly one after another, so a
join that ended in failure cost the **sum** of every budget:

| phase | budget | source (pre-S6 tree, `9eadde33`) |
|---|---|---|
| direct punch | 2 500 ms | `join_attempt`, literal `2500` |
| rendezvous REGISTER/DELIVER | 8 000 ms | `CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_BUDGET_MS`, config.c default |
| bilateral punch | 5 000 ms | `CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS`, config.c default |
| relay rung | 4 000 ms | `CFG_KEY_NETPLAY_DIRECT_P2P_RELAY_BUDGET_MS`, config.c default |

19 500 ms per attempt, and the S2 auto-retry runs the whole thing twice:
**39 000 ms** before the user is told anything. The host was serial too —
bilateral punch 5 000 then relay 4 000 = 9 000 ms.

Worse than the arithmetic: the bilateral punch could not **start** until
the direct punch had burned its entire window, even though the DELIVER
carrying its endpoint had typically arrived hundreds of milliseconds in.

### 8.2 Why not ICE

Decision on file, unchanged: **do not build ICE.** The MiSTer has one
interface; the kernel is built without `CONFIG_IPV6`, so there is no v6
escape from NAT; there are at most three candidate routes; and a
rendezvous signalling channel already exists. Full ICE — candidate
gathering, priorities, connectivity-check pacing, nomination — would buy
nothing those four facts do not already decide, and would add a state
machine larger than the whole of `direct_p2p.c`.

What was actually needed is the *one* thing ICE has that we lacked:
candidates tried **concurrently** rather than in sequence.

### 8.3 As built: one interleaved loop, no new threads

`p2p_race()` (direct_p2p.c) drives up to four legs from a single loop on
**the same worker thread that already owned the socket exclusively** —
the join worker, or the host's bilateral-punch worker during
`FALLBACK_BILATERAL_PUNCH` (§Decision 3, unchanged and still
load-bearing). **No new threads, no new locks, no new races**: every leg
is a state machine pumped from that loop, and the socket keeps exactly
one reader/writer, exactly as before.

| leg | role | what it is |
|---|---|---|
| `punch[0]` | both | the endpoint we already hold — the joiner's room-code endpoint, or the host's DELIVER-supplied joiner endpoint |
| `punch[1]` | joiner | the endpoint a DELIVER teaches us mid-race. This is the old "bilateral" punch, except it now starts **the instant the DELIVER parses** |
| `signal` | joiner | the REGISTER / CHALLENGE / DELIVER conversation. The host is already paired by the time it punches and its rendezvous worker has exited, so it has no signal leg |
| `relay` | both | the S5 `RELAY_REQ → GRANT → PIN → ACK` rung |

The blocking `Stun_HolePunch` was split into a non-blocking stepper —
`Stun_PunchBegin` / `Pump` / `Offer` / `Settled` / `End` (stun.h,
stun.c) — and `Stun_HolePunch` **re-implemented as a thin blocking driver
over it**, so its wire behaviour, its accept criteria (source IP + the
exact 17-byte authenticated payload, port deliberately unmatched for the
S2 symmetric retarget), its adaptive cadence and its ~600 ms confirmation
tail are unchanged, and its existing regression tests
(`run_punch_retarget_test`, `run_punch_token_reject_test`) now cover the
stepper too.

### 8.4 The four ordering rules, and why each is load-bearing

1. **A confirmed punch always beats the relay.** Checked first in every
   iteration; the first confirm tears the relay leg down. A relayed
   session detours through a European VPS (§7.5), so silently preferring
   it over a working direct link would be a latency regression disguised
   as a connectivity win.

   **1b — and the relay's own handoff waits `RACE_RELAY_GRACE_MS`
   (600 ms) PAST THE END OF THE PUNCH (S6 review H-3; re-anchored and
   re-sized by the second review's H-A and H-B).** Rule 1 as originally
   shipped was a purely LOCAL decision: the relay leg pinned, the race
   ended on the spot, and nothing asked what the *other* peer had
   decided. Two peers confirm their punches about one one-way delay
   apart, so a start skew of a few tens of milliseconds was enough for
   one side to relay while the other punched. GekkoNet resolves inbound
   packets by source address and `netplay.c` registers the remote
   **once** at configure time with no relearn path, so that pair then
   sits in `CONNECTING` for the whole 15 s
   `CONNECT_TIMEOUT_CONNECTING_MS` and fails with no recovery.

   **The first fix moved the band; it did not close it.** Held from the
   instant the relay became *ready*, the grace is still a LOCAL timer,
   and a local timer cannot resolve a disagreement about timing — a peer
   starting late enough always confirms after we have committed. The
   second review reproduced that on shipped defaults, and the residual
   sat ~200 ms above the top of the range the suite ever looked at
   (sweep upper bound 2 750, pinned point 2 450). Measured on the
   two-peer rig, three configurations:

   | `RACE_RELAY_GRACE_MS` | one-way delay | band CENTRE (= arm + grace) | measured divergence band | span |
   |---|---|---|---|---|
   | 0 | 150 ms | 2 500 ms | 2 350 – 2 600 / 2 625 / 2 625 ms (3 runs) | 250–275 ms |
   | 600 (as shipped) | 150 ms | 3 100 ms | 2 950 – 3 250 ms | 300 ms |
   | 600 | 300 ms | 3 100 ms | 2 800 – 3 300 ms (10 points) | 500 ms |
   | 600 | 400 ms | 3 100 ms | 2 700 – 3 350 ms (14 points) | 650 ms |
   | 1 200 | 300 ms | 3 700 ms | 3 450 – 3 800 ms | 350 ms |

   Two things are visible and neither was in the record before. The band
   **CENTRE tracks `RACE_RELAY_ARM_MS + RACE_RELAY_GRACE_MS` exactly** —
   the grace translates the defect, it does not shrink it. And the band
   **WIDTH grows with the one-way delay**, roughly 2× it: it is one round
   trip, not "a 150 ms window equal to the injected one-way delay", which
   is what this section claimed and what the 600 ms was sized against.
   The claimed 4× headroom was therefore about 2×. That is the second
   review's H-B; the corrected derivation is below.

   **The suite reported success throughout.** The owd 300 and owd 400
   runs above both exit **0** and both print
   *"test 24 OK — two real peers converged on the same rung"* while the
   sweep beside them lists 10 and 14 split-brain skews. A pinned point of
   2 450 ms and a sweep ceiling of 2 750 ms cannot see a band that moves.

   **What closes it.** The grace is now held from the LATER of "the
   relay became ready" and "the last punch candidate stopped sending",
   and a candidate that has stopped sending is kept on the RECEIVE path
   until the relay commits instead of being torn down. The decision
   stops being "how long since MY relay was ready" and becomes "is the
   punch provably over on BOTH sides" — a quantity the two peers
   genuinely share, because each punches for `punch_leg_ms` from its own
   start and each listens until it commits. A confirmation on either
   side resets `sent_any` in `Stun_PunchOffer` and resumes that side's
   confirmation tail, which is what confirms the other side in turn.
   Both halves are load-bearing and each has its own probe point in
   test 24: without the re-anchor the band stays at the relay-commit
   instant, and without the listen-past-send rule it simply moves to the
   punch-send-end instant.

   **Sizing, corrected (H-B), and the exact condition it buys.** With
   start skew *s* and one-way delay *d*, the late peer's punch reaches us
   at *s + d* and our answering tail reaches it at *s + 2d*; convergence
   therefore needs the grace to cover **one round trip (2*d*), not one
   one-way delay**. Stated as a condition:

   > the two peers converge at **every** start skew exactly when
   > `RACE_RELAY_GRACE_MS >= 2 * one-way delay`, i.e. when the grace
   > covers the pair's round-trip time. Where it does not, the residual
   > band is `2d - grace` wide and sits just past the punch send end.

   That is a *prediction*, so it was tested in both directions on the
   post-fix tree rather than only where it passes:

   | one-way delay | RTT | grace | predicted | measured |
   |---|---|---|---|---|
   | 150 ms | 300 ms | 600 | closed | **0 splits** / 97 races, skew 1 800–6 600 |
   | 250 ms | 500 ms | 600 | closed | **0 splits** / 34 races, skew 2 600–5 900 |
   | 400 ms | 800 ms | 600 | **reopens**, 200 ms wide, just past the punch send end | **splits at skew 5 200 and 5 300** — 200 ms, exactly there |
   | 400 ms | 800 ms | 1 200 | closed again (grace now covers the RTT) | **0 splits** — skew 5 200 and 5 300, which split at grace 600, both converge PUNCHED |

   **This is what separates the new anchor from the old one, and it is
   the same experiment run against both.** Take the grace as the lever
   and watch what it does:

   | | shipped H-3 anchor (relay-ready) | this anchor (punch send end) |
   |---|---|---|
   | grace 0 → 600 → 1 200 | band **MOVES**: centres 2 500 → 3 100 → 3 700 ms, never absent | — |
   | grace 600, owd 50 / 150 / 250 | band present at every delay | **absent** |
   | grace 600, owd 400 | present | present, 200 ms, at the predicted skew |
   | grace 600 → 1 200 at owd 400 | band moves, still present | band **CLOSES** |

   Under the shipped fix no value of the grace removed the band — that is
   the second review's H-A, and it is why "a locally-timed decision
   cannot resolve a disagreement about timing" was the right diagnosis.
   Under the new anchor the band is a **bounded function of the grace**:
   absent whenever the grace covers the round trip, and the very same
   lever that only relocated the defect before now removes it. What
   remains is a stated coverage limit with a formula and a knob, not an
   unbounded defect.

   600 ms therefore covers every pair up to a **600 ms RTT**. A real
   transatlantic RTT is 70–180 ms; a pair above 600 ms RTT is not
   playable at three-frame rollback regardless. The number is sized
   against a network quantity, with the derivation written next to it in
   `direct_p2p.c`, and **not** against `SB6_OWD_MS` or any other harness
   constant — sizing a production margin against a test literal is what
   produced H-B in the first place.

   Bounded three ways: by the grace, by the punch send window that now
   anchors it, and by the race budget, which commits the relay rather
   than losing it. The cost is stated in §8.10.
2. **The relay leg does not arm until `RACE_RELAY_ARM_MS` (2 500 ms)...**
   Naive racing would have every pair request a pool port from the
   100-port range. 2 500 ms is *exactly* the window the pre-S6 direct
   punch had entirely to itself, so nothing that the pre-S6 code would
   have connected at that stage is diverted to a relay.

   **2b — ...and not until every LIVE CANDIDATE has had
   `RACE_PUNCH_MIN_WINDOW_MS` (2 500 ms) of its own (S6 review, H-4).**
   The original rule measured its delay from RACE START, which makes the
   justification above true of `punch[0]` — armed at `t0` — and false of
   the DELIVER candidate, which is armed at DELIVER time *D* and pre-S6
   got its own full `BILATERAL_PUNCH_MS`. With *D* = 1 200 ms it got
   ~1.3 s instead of 2.5 s, so a US-US pair needing ~2 s from DELIVER was
   diverted onto a European relay it did not need — a direct link
   replaced by a transatlantic one for people who connect fine today. The
   deferral is **capped** at `race_budget − relay_budget` (4 000 ms on
   the defaults) so the relay always keeps its own budget: a relay that
   arms too late to finish is worse than one that arms slightly early.
3. **The relay leg also needs a real DELIVER (joiner).** Without one we
   are provably not paired server-side and `handleRelayReq` refuses an
   unpaired session, so arming would spend the budget to be told
   something we already know. The host is paired by construction.
4. **A `NOT_PAIRED` refusal is transient, not terminal.** Serially the
   rung ran after the signalling phase had definitely completed, so a
   refusal could only mean a durable condition. Racing can now ask while
   the peer's own REGISTER is still in flight, and treating that first
   answer as final would **invent** a failure for exactly the symmetric
   pairs the relay exists to carry. Only `POOL_EXHAUSTED` ends the leg
   early.

### 8.5 Measured, not estimated

Both numbers come from the **same probe** — the REAL `BeginJoin` driven
to terminal failure with a mocked STUN discovery and offline punch legs
— run against the pre-S6 tree in a separate worktree at `9eadde33` and
against the post-S6 tree. Two scenarios, because they stress different
parts of the cascade:

| scenario | pre-S6 (`9eadde33`) | post-S6 | change |
|---|---|---|---|
| **A** rendezvous black hole (no DELIVER ever arrives) | 21 326 ms total, **10 663 ms/attempt** | 16 222 ms total, **8 111 ms/attempt** | −24% |
| **B** full cascade (DELIVER arrives, relay silent — every leg runs) | 19 354 ms total, **9 677 ms/attempt** | 10 228 ms total, **5 114 ms/attempt** | **−47%** |

Scenario A is bounded by the 8 000 ms signalling budget on both trees —
racing cannot shorten a leg that is the longest thing running. Scenario B
is where seriality actually cost: pre-S6 the bilateral punch and the
relay rung could only begin after the direct punch had expired.

The **arithmetic** worst case — a DELIVER arriving just before the
signalling budget expires, so all four phases run at full length — is
19 500 ms/attempt pre-S6 against a `RACE_BUDGET_MS`-bounded 8 000 ms
post-S6. That case is stated from the code, **not** measured: neither
probe scenario reproduces a late DELIVER.

Sanity check on the mid-race overlap, measured directly: with a DELIVER
arriving at once and only the DELIVER endpoint answering, the handoff
completes in **~230 ms** (test 19). Pre-S6 the earliest possible handoff
on that path was > 2 500 ms, because the direct punch had to expire
first.

**What the race is now hard-bounded by (S6 review, H-1).** The overall
deadline is `race_budget_ms`, **plus at most one `STUN_PUNCH_CONFIRM_MS`
(600 ms) confirmation tail** when a leg has already confirmed and is
still owing its peer that tail — see §8.9. Worst case per attempt is
therefore 8 600 ms on the defaults, 17 200 ms for the joiner's two
attempts. Verified headroom against the callers:

- `CONNECT_TIMEOUT_CONNECTING_MS` (15 000 ms, `connect_fail.h:267`) does
  **not** bound the race: it is armed on entry to
  `NETPLAY_SESSION_CONNECTING` (`netplay.c:1776-1791`), i.e. *after* the
  handoff, and bounds GekkoNet's sync, not establishment.
- The bound that does apply is nav's `NAV_WAIT_ORCH_TIMEOUT_FRAMES`
  (`netplay_nav.c:120` = `150 * 60` frames = 150 000 ms, enforced at
  `netplay_nav.c:391`). 17 200 ms of race against 150 000 ms is **8.7x
  headroom**, and the +1 200 ms the tail exemption adds across two
  attempts is 0.8% of it.
- The suite's own bound, `S6_WORST_CASE_BOUND_MS` = 22 000 ms, is met:
  the black-hole probe measured **16 228 ms** total post-fix.

### 8.6 Config

| key | default | purpose |
|---|---|---|
| `netplay-direct-p2p-race-budget-ms` | `8000` | the WHOLE post-STUN establishment wall clock on both roles, clamped [2000, 30000]. It replaces the old serial sum as the thing that bounds a failing attempt |

The per-leg keys keep their meaning and now bound their own legs *inside*
the race: `signal-budget-ms` the rendezvous leg, `bilateral-punch-ms`
each punch leg's lifetime, `relay-budget-ms` the relay leg. None became a
dead key.

### 8.7 Behaviour changes worth calling out

- **The three fallback bypasses moved up front.** Kill switch, LAN peer
  and hairpin used to be evaluated *after* the direct punch failed,
  because that was the only point at which the code decided whether to
  enter the fallback. In a race there is no such point — the signalling
  leg starts at t=0 — so the question they answer ("is the rendezvous
  fallback worth running at all for this peer?") is answered before the
  race is configured. **No verdict changes**: none of the three depends
  on the punch outcome, and each still yields `FAILED_SYMMETRIC` with its
  pre-S6 status text. The fourth bypass, `diag_punch_bad_token`, genuinely
  does depend on the punch and stays after the race.
- **A dead rendezvous URL no longer kills the direct punch.** Pre-S6 a
  missing/malformed signal URL or a failed resolve returned
  `FAILED_BILATERAL` from inside the fallback, after the direct punch had
  run. Now the setup happens before the race, and a failure only drops
  the fallback *legs* — the punch leg still runs and can still win. The
  failure is reported only if nothing connects.
- **The report line's stage timings now OVERLAP.** `punch=`, `signal=`,
  `bilateral=` and `relay=` are leg **lifetimes**, so they no longer sum
  to the elapsed time. A new `race=` field carries the wall clock of the
  whole post-STUN cascade and is the number to compare across builds. Both
  the OK and FAIL lines carry it. The OK line also gained `relay_fail=`,
  which §7.5 always specified but the code never emitted there (§8.9).
- **The `DirectP2P_TestHook_SetStunHolePunch` seam is gone**, replaced by
  `DirectP2P_TestHook_SetPunchOracle` (direct_p2p.h). With no blocking
  punch left to substitute, the seam moved to the decision the tests were
  really making — "does THIS candidate ever confirm?" — consulted once per
  candidate at arm time. The **rename is deliberate**: a stale caller must
  fail to compile rather than silently behave differently, the same
  discipline §6.3 applied to the room-code bool API. A leg under an oracle
  override puts nothing on the wire, which is what keeps the offline
  harnesses offline while still consuming the leg's real wall time.

### 8.8 Tests

Cases 18, 19, 20 (three acts), 20C and 21 in `test_bilateral_punch.c`
came with the stage, plus one in `test_stun_mock.c`. **23-31 came out of
the S6 adversarial review and the second review's H-A/H-C follow-up**,
which between them found that the stage's own tests could not see three
of its four alpha blockers — and then that the fix for one of those
blockers had moved the defect rather than closed it. Every case below
was proven red by neutralising the code under test and observing a
**non-zero harness exit** — not by assertion.

(Two exceptions, both corrected here rather than left standing. **Row 27's
N5** deletes the duplicate-endpoint guard, which *is* genuinely uncovered
code — but that guard is byte-identical to the pre-fix tree
(`git show 26deb2fc:src/netplay/direct_p2p.c | sed -n '1210,1215p'` and
`sed -n '1313,1318p' src/netplay/direct_p2p.c` both md5
`d1f6c2bdeb1ee0d52b98b3fffc9fc17c`), so it was never the M-2 defect;
restoring the pre-fix memset-then-validate order *and* deleting the
`race_finish_punch` call left the whole pre-H-C suite green. Tests 30 and
31 are what observe M-2. **Row 24's N2** was red, but against a pinned
skew of 2 450 ms and a sweep that stopped at 2 750 ms, neither of which
could reach the 2 950–3 250 ms band the fix left behind; see §8.4 rule 1b
and the N13/N14 rows.)

**The structural finding first (H-2).** Across a full pristine run the
line `S6 race: punching candidate` appeared **27 times** and
`Hole punch SUCCESS` **zero** times. Every candidate in 18-21 is
oracle-driven, and `race_punch_settled()` returns `true` unconditionally
for a non-`DP2P_PUNCH_REAL` oracle — so the `Stun_PunchSettled` branch
the shipping path *always* takes was executed by no test at all. That is
why H-1, H-4 and M-2 all survived a green suite. Tests 23-29 use **no
oracle** — none of them installs a punch oracle, so any leg they arm is
`DP2P_PUNCH_REAL` and goes through the real stepper. But "no oracle" is
not the same as "confirms on the wire", and only **four of the seven**
actually complete a punch. A pristine run **of the post-23-29 tree**
emits `Hole punch SUCCESS` **7 times** (the "zero" above is the
pre-23-29 state this paragraph opened with), and interleaving those
lines with the per-test banners attributes every one of them:

| test | `Hole punch SUCCESS` | what it puts on the wire |
|---|---|---|
| **23** | **1** | one real leg against the echo peer; confirms |
| **24** | **many** | two concurrent real races per probe point, each with a real leg. The second review replaced the single pinned skew with seven DERIVED points plus the 1 000 ms control, so the count scales with the probe list rather than being a fixed 4 |
| **25** | **1** | one real DELIVER-armed leg against the echo peer; confirms |
| **26** | **1** | one real leg against the echo peer; confirms |
| 27 | **0** | arms exactly one real leg and punches it, but the target is a **bound, silent sink** — `sink_sock` at `test_bilateral_punch.c:8560` — with `seed_port = 0` at `test_bilateral_punch.c:8567` — nothing ever answers, the race ends EXHAUSTED, and `27-no-punch-from-a-silent-sink` asserts precisely that |
| 28 | **0** | **runs no race at all**: it calls `DirectP2P_TestHook_RaceBudgetExpired` as a pure function and compares `STUN_PUNCH_CONFIRM_MS` against the literal 600 (`test_bilateral_punch.c:8695`). No socket, no thread, no stepper |
| 29 | **0** | **relay-only**: `seed_port = 0` (`test_bilateral_punch.c:8805`, "no punch leg at all") and `signal_leg = false` (`test_bilateral_punch.c:8805`), so no candidate is ever armed and the only leg is the relay |

So the sentence that matters is narrower than "23-29": **tests 23, 24,
25 and 26** are what drive a real 17-byte punch to a real loopback peer
and back, and they are therefore the only tests in which the S4a token
check, the source-IP gate and the 600 ms confirmation tail are actually
executed on the accept path. **27** exercises the *send* half and
`race_arm_punch`'s duplicate guard but never the accept half; **28** and
**29** touch neither. The rig for 23-26 is a **punch
echo peer** that returns the exact 17 bytes it received — verbatim, so
it passes `Stun_PunchOffer`'s prefix and constant-time token compare by
construction rather than by bypassing them — with a configurable delay
measured from the first punch it sees, which is what lets a test place
the confirmation instant relative to the race's own `t0`.

Four `NETPLAY_TEST_HOOKS`-only seams make that possible:

- **`DirectP2P_TestHook_RunRace`** runs ONE `p2p_race` against
  caller-supplied endpoints. Every other seam drives the race through
  `BeginJoin` or the host worker, which own process-wide state, so only
  one can be live at a time — and the split brain is by construction a
  TWO-peer property. `p2p_race` takes everything by argument, so test 24
  runs two of them concurrently on two threads, punching each other
  through a UDP delay line that injects a fixed one-way delay.
- **`DirectP2P_TestHook_RaceBudgetExpired`** exposes the deadline
  predicate as a pure function, because the wrap defect needs a 49.7-day
  clock and this build has no clock-injection seam.
- **`DirectP2P_TestHook_SetArmFailEndpoint`** nominates one endpoint whose
  `Stun_PunchBegin` must fail, by handing it an unresolvable 144-character
  DNS label instead of the peer IP. `race_arm_punch` has exactly one
  failure mode past its up-front `ip`/`port` guard — `Stun_PunchBegin`
  (`stun.c:781-798`) — and the only slot the race ever RE-arms takes its IP
  from `Rendezvous_ParseDeliverEx`, which emits `inet_ntop` output and
  therefore always a resolvable dotted quad (`rendezvous.c:251-254`). The
  wire cannot produce the case M-2 half (i) is about; the seam can, and the
  failure it produces is the real `NET_ResolveHostname` one.
- **`DirectP2P_TestHook_RaceRelayArmMs` / `..._RaceRelayGraceMs`** expose the
  two production constants that place the relay-vs-punch decision in time,
  so test 24's probe points and its sweep range are DERIVED from them
  instead of hard-coded. The direction is deliberate and one-way: a test may
  derive its search space from a production constant; a production margin may
  never be sized against a test constant. Sizing the grace against
  `SB6_OWD_MS` is precisely what the second review's H-B found.

| test | neutralisation | observed red (harness exit 1) |
|---|---|---|
| 19 `test_race_deliver_overlaps_seed` | the DELIVER endpoint is never armed as a punch candidate | *"test19: no handoff (state=11 seed_arms=2 deliver_arms=0)"* |
| 19 + 18 | **N10**: the legs run serially again — see the exact diff in §8.8.1 | *"test19: no handoff (state=11 seed_arms=2 deliver_arms=0)"* and *"test18: full-cascade join took 14225 ms (~7112 ms/attempt), expected < 12000 ms — the punch, signalling and relay legs must OVERLAP (measured pre-S6 serial: 19354 ms)"*. Exit **1**. N10 also reds `15A-echo-cookie`, *"test20C: no handoff (state=11 relay_reqs=2 grants=2 pins_ok=14)"* and two `test26` lines — serialising the legs stops the relay ever being IN FLIGHT when a punch lands, which is what 20C and 26 exist to observe |
| 20B `test_race_punch_beats_relay` | **N9**: `RACE_RELAY_ARM_MS` → 0 | *"test20B: 1 RELAY_REQ(s) inside a 1500 ms race; the relay leg must not arm before RACE_RELAY_ARM_MS (2500 ms)"* |
| 21 `test_race_not_paired_is_transient` | `NOT_PAIRED` terminal again | *"test21: no handoff after two NOT_PAIRED refusals ... relay_reqs=2 grants=2 notpaired=2"* |
| 20C `test_race_punch_beats_inflight_relay` | the `RACE_PUNCHED` exclusion removed from the end-of-race relay-fail fallback | *"test20C: OK report line does not carry relay_fail=P2P_OK ... relay_fail=P2P_FAIL_RELAY_UNAVAILABLE"* |
| **23** `test_race_confirm_at_budget_edge` (H-1, H-2) | **N1**: the confirmation-tail exemption deleted from `race_budget_expired` | *"test23: outcome=3 after 4003 ms, expected PUNCHED(0) — a punch that confirms inside the last 600 ms of the 4000 ms budget must NOT be discarded (H-1)"* |
| **24** `test_race_split_brain` (H-3) | **N2**: `RACE_RELAY_GRACE_MS` → 0 | *"test24: SPLIT BRAIN at skew=2450 ms — peer A ended PUNCHED and peer B ended RELAYED"* |
| **25** `test_race_relay_defers_per_candidate` (H-4) | **N3**: `RACE_PUNCH_MIN_WINDOW_MS` → 0 | *"test25: outcome=RELAYED peer=127.0.0.1:&lt;port&gt; after **3119 / 3126 / 3123** ms — the DELIVER candidate armed at t+1200 ms confirms at ~t+3200 ms and must win; the relay (which would grant instantly) must not steal it. relay_reqs=1"* (three consecutive runs; exit **1** each), plus a second red, `25-real-punch-confirmed: expected true: s_sb6_punch_success >= 1` — with the per-candidate window gone the punch never confirms at all. The time is ~3120 ms, **not** the relay-*ready* instant of ~2520 ms: the relay becomes ready one `RACE_RELAY_GRACE_MS` (600 ms) earlier and only commits after the grace |
| **26** `test_race_punch_drops_relay_leg` (H-7) | **N4**: `relay_state = RELAY_LEG_DONE;` deleted from ordering rule 1 | *"test26: the relay port was still being pinned 508 ms after the punch confirmed (tear-down logged at t=73488, last pin at t=73996, 7 pins total). Ordering rule 1 must set relay_state = RELAY_LEG_DONE, not merely log that it did"* |
| **27** `test_race_duplicate_candidate_guard` (the duplicate-endpoint guard — **not** M-2; see 30/31) | **N5**: the duplicate-endpoint guard deleted | *"test27: 5 candidates were armed against 5 identical DELIVERs, expected exactly 1 — race_arm_punch's duplicate-endpoint guard must reject an endpoint already being punched"* |
| **28** `test_race_budget_wrap_safety` (M-1) | **N6**: `race_budget_expired` restored to `now >= t0 + budget` | *"28-wrap-first-iteration: expected false: DirectP2P_TestHook_RaceBudgetExpired(t0, t0, budget, false)"* |
| **28** (M-3) | **N7**: `STUN_PUNCH_CONFIRM_MS` 600 → 0 | *"test28: STUN_PUNCH_CONFIRM_MS is 0, expected the shipped literal 600"* |
| **29** `test_relay_not_paired_named_separately` (L-1) | **N8**: `NOT_PAIRED` collapsed back into `CONNECT_FAIL_RELAY_REFUSED` | *"test29: relay_fail=P2P_FAIL_RELAY_REFUSED after 4 NOT_PAIRED refusal(s), expected P2P_FAIL_RELAY_NOT_PAIRED"* |
| **29** (arm delay) | **N9**: `RACE_RELAY_ARM_MS` → 0 | *"test29: the relay leg armed at t+2 ms with no punch candidate in the race; it must not arm before RACE_RELAY_ARM_MS (2500 ms)"* |
| `run_punch_leg_offer_test` — **`--test-stun-mock`, not this harness** | the stepper's source-IP gate removed — the `NET_GetAddressString` compare at `stun.c:859` and its `return false` at `stun.c:860` | `--test-bilateral-punch` stays at exit **0** with zero harness FAILs: every leg it punches is loopback-to-loopback, so the source IP always matches and the gate is never the thing that rejects. The 4 reds are all in **`--test-stun-mock`** (exit **1**), from `test_stun_mock.c:739`, driven at `:1377`, in this order: *"a valid payload from the WRONG source IP was consumed"*, *"a valid payload from the WRONG source IP confirmed the leg"*, *"a wrong-token punch CONFIRMED the leg (S4a fail-closed broken)"*, *"leg settled BEFORE the confirmation tail elapsed"* — the last two are knock-ons: the leg is already confirmed (and its `confirm_ms` already stamped) by the stranger's datagram |
| **30** `test_race_failed_rearm_keeps_live_candidate` (M-2 half i) | **N11**: `race_arm_punch` restored to memset-then-validate, the `race_finish_punch` call kept | *"test30: endpoint B stopped being punched 482 ms after it started (10 datagrams), expected at least 2000 ms — a re-arm that FAILS must leave the live candidate armed. race_arm_punch memset the slot BEFORE letting Stun_PunchBegin fail"*. Exit **1**, 1 failure; test 27 and test 31 stay green |
| **31** `test_race_rearm_releases_address_ref` (M-2 half ii) | **N12**: the `race_finish_punch(c, now);` call deleted from `race_arm_punch`, ordering kept | *"test31: +93 live allocations survived a race that re-armed slot 1 31 time(s), bound is 12. Each un-released NET_Address is 3 live allocations, so this is ~31 leaked address(es)"*. Exit **1**, 1 failure; test 27 and test 30 stay green |
| **24** (second review, H-A: the re-anchor) | **N13**: the commit grace anchored back at `relay_ready_ms` alone (`grace_anchor` and `punch_still_sending` discarded) | *"test24: SPLIT BRAIN at skew=2950 ms (one owd BEFORE the relay-commit instant) — peer A ended PUNCHED and peer B ended RELAYED"*, then RELAYED-instead-of-PUNCHED at 3100 and 3250. Exit **1**, **6** failures. The punch-send-end probe points stay GREEN — this half and N14 are independently load-bearing |
| **24** (second review, H-A: listen past send) | **N14**: the deferral deleted — `race_finish_punch` called at `punch_leg_ms` whether or not a relay leg is in play | *"test24: SPLIT BRAIN at skew=4850 ms (one owd BEFORE the punch send window ends)"* and the same at 5000 ms. Exit **1**, **3** failures. The relay-commit probe points stay GREEN: without this half the band does not close, it moves to `punch_leg_ms` |
| **25** (second review, H-A follow-on) | **N3** re-run on the POST-H-A tree | the outcome assertion no longer fires — the re-anchor lets the punch win anyway — so this row would have gone vacuous. The surviving promise is rule 2b's *"a pair that can punch never requests a pool port"*: *"test25: the relay leg sent 1 RELAY_REQ(s) for a pair that punched successfully"*. Exit **1**, **exactly 1** failure, and it is that assertion |

Notes on what each test is *for*:

- **18** is the timing regression net. Its scenario-B bound was
  **14 000 ms**, and the review measured the neutralised (serialised) run
  at **14 216 ms** — 216 ms, **1.5%**, of margin. A slightly faster
  machine would have left the assertion unable to fail, which is the
  same defect the bound was introduced to avoid. Re-measured on this
  tree with the same probe: passing **10 235 ms**, neutralised
  **14 216 ms**. The bound is now **12 000 ms**, sitting between the two
  measured numbers with **17.3%** of headroom over the passing run and
  **18.5%** of margin under the neutralised one. Its scenario-A bound
  stays deliberately loose; A is a *measurement*, B is the assertion that
  bites.
- **19**'s assertion is on `do_handoff`'s **own arguments**
  (`DirectP2P_TestHook_LastHandoff`), not on internal state — a race that
  populated `s_work` correctly but never reached the handoff would pass
  an `s_work` assertion and cannot pass this one.
- **20** has two acts because 20A alone does not isolate the arm delay:
  its punch confirms on the first pump, so the "a confirmed punch drops
  the relay leg" rule would keep `relay_reqs` at 0 even with the delay
  removed. 20B uses a punch that never confirms and a race bounded to
  1 500 ms, so the delay is the only thing that can keep the count at
  zero.
- **20C, honestly re-scoped (review H-7).** What it covers: a relay leg
  that really armed (1 `RELAY_REQ` in flight), the punch still winning,
  and the reporting rule that an abandoned relay leg is not a failed one
  (`relay_fail=P2P_OK` on the OK line). What it does **not** cover: on a
  passing run its relay leg lives ~6 ms, never leaves `RELAY_LEG_REQ`,
  never receives a GRANT, and its mock is `MOCK_RELAY_NO_ACK`, whose own
  comment says it "cannot win" — so deleting the tear-down leaves it
  green. Test 26 covers the tear-down. It also carried an empty
  *"NEUTRALISATION CHECK for the arm-delay"* comment with **no code under
  it**; that is resolved by **deleting** it rather than by writing the
  check, because the check it described cannot fail there — 20C's
  DELIVER is delayed to 3 000 ms by construction and the relay's
  `paired` gate *is* that DELIVER, so the leg cannot arm before 2 500 ms
  whatever the arm rule says. 20B and test 29 cover the arm delay where
  it *can* fail. 20C now also sets `relay-budget-ms` explicitly instead
  of inheriting 800 ms from an earlier case, because rule 2b's cap is
  computed from `(race_budget − relay_budget)`.
- **23** asserts that the race ran **longer** than its budget, not merely
  that it succeeded. That is what proves the confirmation landed inside
  the exempted window rather than comfortably before it — without it the
  test could pass for a reason unrelated to H-1. Observed on a passing
  run: 4 348 ms against a 4 000 ms budget, hard cap 4 600 ms.
- **24** is the only two-peer case in the suite, and the second review
  rewrote how it chooses what to probe. Its control point (skew
  1 000 ms) must punch on both sides *even on the pre-fix code*; if the
  control ever reds, the rig is broken, not the fix.

  **What was wrong with it.** Both the pinned point (`SB6_SPLIT_SKEW_MS`
  = 2 450) and the sweep range (2 150–2 750, step 25) were compile-time
  literals chosen where the fix worked. The residual band sat at
  2 950–3 250 — ~200 ms **above the top of the range the project ever
  looked at** — so the suite was structurally unable to see the defect
  it existed to catch. That is the defect underneath the defect, and it
  is why the second review's H-A asked for the sweep to be widened and
  the delay parameterised before anything was fixed.

  **What it does now.** A race can only diverge at an instant where one
  peer's decision changes, and there are exactly two: the **relay
  commit** (`RACE_RELAY_ARM_MS + RACE_RELAY_GRACE_MS`) and the **punch
  send end** (`punch_leg_ms`). The pinned points are derived from those
  two constants — each probed at −owd, +0 and +owd, plus one point past
  everything where both peers must agree to RELAY — via
  `DirectP2P_TestHook_RaceRelayArmMs` / `...RaceRelayGraceMs`, so
  widening the grace or the arm delay moves the probe with it. Each
  point carries its own expected outcome, so a point that flips from
  PUNCHED to RELAYED is a red rather than a silent pass. The one-way
  delay, the punch window, the race budget and the sweep bounds are all
  runtime knobs now (`S6_SPLIT_OWD_MS`, `S6_SPLIT_PUNCH_LEG_MS`,
  `S6_SPLIT_BUDGET_MS`, `S6_SPLIT_LO/HI/STEP`), which is what let the
  bands in §8.4 rule 1b be measured at three grace values and three
  delays instead of one. The sweep (`S6_SPLIT_SWEEP=1`) is still off by
  default because it costs ~100 races; its default range is now derived
  from the same two constants rather than hard-coded.
- **26** is the tear-down test, and the mechanism is only observable as
  an **absence**: the relay grants immediately, the leg reaches
  `RELAY_LEG_PIN` and resends `RELAY_PIN` every 150 ms, and the mock
  records the arrival time of the last pin. With the tear-down the pins
  stop at the confirm (observed: last pin **130 ms before** the tear-down
  line); without it they keep coming for the whole 600 ms tail (observed:
  **508 ms after**). The threshold is 200 ms, so the margins are 330 ms
  on the passing side and 308 ms on the red side — both over twice the
  150 ms pin period.
- **`run_punch_leg_offer_test`** covers the one decision the blocking
  driver never makes and the race depends on completely: the race offers
  each non-'3SXR' datagram to every live leg in turn and stops at the
  first that consumes it, so a leg that consumed datagrams which are not
  its own would let one candidate's noise confirm another candidate's leg.
  It lives in **`test_stun_mock.c`** (`:739`, driven at `:1377`) and reds
  `--test-stun-mock`, not `--test-bilateral-punch` — see the last row of
  the table above.

#### 8.8.1 N10, exactly

"The legs run serially again" is ambiguous on its own, so the patch that
produced the N10 numbers above is recorded verbatim. It is **additive**:
it keeps the existing arm delay and AND-s the serialisation on top,
because replacing the delay outright also removes the
`RACE_RELAY_ARM_MS` floor that test 29 pins, which reds tests that have
no seed leg at all for an unrelated reason.

```diff
--- a/src/netplay/direct_p2p.c
+++ b/src/netplay/direct_p2p.c
@@ p2p_race — 3) arm the relay leg
+            bool all_punch_legs_finished = true; /* N10 */
+            for (int i = 0; i < RACE_PUNCH_LEGS; i++) {
+                if (cands[i].armed && !cands[i].finished) all_punch_legs_finished = false;
+            }
             const bool delay_done =
-                cfg->no_punch ||
-                (int)(now - t0) >= relay_defer_cap_ms ||
-                ((int)(now - t0) >= (int)RACE_RELAY_ARM_MS && candidate_windows_done);
+                all_punch_legs_finished && /* N10 */
+                (cfg->no_punch ||
+                 (int)(now - t0) >= relay_defer_cap_ms ||
+                 ((int)(now - t0) >= (int)RACE_RELAY_ARM_MS && candidate_windows_done));
@@ p2p_race — DELIVER handling
-                        if (!cfg->no_punch) {
+                        if (!cfg->no_punch &&
+                            (!cands[0].armed || cands[0].finished)) { /* N10 */
                             (void)race_arm_punch(cands, RACE_PUNCH_LEGS, 1, cfg,
                                                  parsed_ip, parsed_port, now);
                         }
```

The `!cands[0].armed ||` term is what keeps the neutralisation honest: a
race configured with `seed_port = 0` (tests 23, 25, 27) has no seed leg
to wait for, and gating on `cands[0].finished` alone would stop its
DELIVER candidate arming at all — reddening those tests for a reason
that has nothing to do with serialisation.

### 8.9 Defects found by the gates, and by the review of the gates

Recorded because each was found by a check that could easily have been
skipped, and two of them by a build configuration rather than by a test.
The last entry is the adversarial review of this stage, which found
four alpha blockers the stage's own tests could not see; §8.8 carries
the neutralisation record.

- **The punch-mode enum was inside the `NETPLAY_TEST_HOOKS` block.**
  `p2p_race` stores a `DirectP2PPunchOracleResult` on every candidate in
  every build, so a plain Release or Debug build did not compile — 10
  errors. The test-hooks build was green throughout. Caught only by
  running the Release and plain-Debug gates, which is exactly why they are
  gates. The enum now lives in the production part of direct_p2p.h; only
  the function-pointer typedef and the setter stay test-only.
- **The overall race deadline was wrap-unsafe.** The loop tested both
  `(int)(now - t0) >= budget` (wrap-safe) and `now >= t0 + budget` (not).
  It was redundant with the first, so it is deleted rather than repaired;
  subtract-then-cast is the form the rest of the file already uses (S3
  deadline wrap-safety).

  **The recorded reasoning for that fix was BACKWARDS, and the wrong
  reasoning is what justified shipping it untested (S6 review, M-1).**
  This entry used to say that across the wrap "the race would have run
  until every leg finished with no overall bound". It could not have: the
  condition was an OR of a wrap-safe term and a wrap-unsafe one, so the
  wrap-safe term bounded the loop on its own. The real symptom is the
  exact opposite. `t0 + budget` evaluated in `uint32_t` **overflows to a
  small number** whenever `t0` is within `budget` of the wrap, and `now`
  — still just below the wrap — is already past it, so the deadline fires
  on the **first iteration**: every join attempted in the ~8 s before a
  49.7-day wrap failed instantly. "Unbounded" and "instantly bounded" are
  opposite defects, and the harmless-sounding one is why no test was
  written. `SDL_GetTicks` cannot be moved to the wrap and this build has
  no clock-injection seam, so the deadline is now a pure function
  (`race_budget_expired`) exposed as
  `DirectP2P_TestHook_RaceBudgetExpired`, and **test 28 drives it at
  `t0 = 0xFFFFFF00`**. The arithmetic *is* the whole defect.
- **An abandoned relay leg was reported as a failed one.** When a punch
  confirms we tear the relay leg down (§8.4 rule 1). The end-of-race
  fallback that fills in `relay_fail` for a leg that armed but never
  concluded did not exclude that case, so a SUCCESSFUL direct connection
  carried `relay_fail=P2P_FAIL_RELAY_UNAVAILABLE`. §7.5 defines
  `relay_fail=` as "it ran and failed like this"; an abandonment is not a
  failure, and the field would otherwise mean two different things
  depending on the verdict on the same line. Fixed, and covered by test
  20C.

  Writing that test surfaced a second, **pre-existing** defect: §7.5 says
  "both report lines gained ... `relay_fail=<code>`", but the OK line
  never carried it — only the FAIL line did. The OK line now carries it,
  which is what makes the abandonment case observable at all.

- **The S6 adversarial review (H-1 … L-1).** In order of what they cost
  a real pair:

  | finding | what it cost | where it is fixed |
  |---|---|---|
  | **H-1** | a punch confirming in the last 600 ms of the budget was discarded and reported as a NAT block — 7.5% of every race, on both attempts | `race_budget_expired`, §8.5 |
  | **H-2** | the suite never put a punch on the wire, which is why H-1/H-4/M-2 survived it | tests 23-31, §8.8 |
  | **H-3** | split brain: one peer relays, the other punches, 15 s hard failure. Measured band 150 ms wide | §8.4 rule 1b |
  | **H-4** | the relay stole candidates that had not had a window of their own | §8.4 rule 2b |
  | **H-7** | ordering rule 1's tear-down was untested and 20C overstated its coverage | test 26, §8.8 |
  | **M-1** | the wrap-safety reasoning was backwards, which justified writing no test | this section, test 28 |
  | **M-2** | `race_arm_punch` memset the slot before `Stun_PunchBegin` could fail: a failed re-arm destroyed a live, still-punching candidate, AND the memset leaked the outgoing leg's `NET_Address` ref | `race_arm_punch`, tests **30 and 31** — *not* test 27, which neutralises a guard that is byte-identical to the pre-fix tree (second review, H-C) |
  | **M-3** | `STUN_PUNCH_CONFIRM_MS` was unpinned | test 28 |
  | **M-4** | test 18's serialisation red had 1.5% of margin | §8.8 |
  | **L-1** | a persistent `NOT_PAIRED` was reported as "the relay port pool is exhausted" | `CONNECT_FAIL_RELAY_NOT_PAIRED`, test 29 |

  What the review explicitly **cleared**, and what therefore was not
  touched: S4a's fail-closed punch authentication (the token check, the
  source-IP gate, the punch prefix, the bad-token evidence), the
  deliberately-unmatched port that recognises the S2 symmetric retarget,
  the adaptive cadence, and the 600 ms tail itself.

- **The SECOND adversarial review (H-A, H-B, H-C), which falsified the
  first round's headline claim.** The first round said H-3 was fixed.
  It was not; it had been moved.

  | finding | what it was | where it is fixed |
  |---|---|---|
  | **H-A** | the split brain was **relocated by one grace window, not closed**. Reproduced on shipped defaults at skew 2 950–3 250 ms — and the suite could not see it, because its sweep stopped at 2 750 and its pinned point was 2 450 | §8.4 rule 1b (grace re-anchored at the punch send end + candidates kept on the receive path), test 24 rewritten |
  | **H-B** | the measurement that sized the grace did not reproduce. §8.4 claimed a "150 ms window, equal to the injected one-way delay"; the band is ~2× the one-way delay — one **round trip** — so the claimed 4× headroom was ~2× | §8.4 rule 1b sizing paragraph, and the `RACE_RELAY_GRACE_MS` comment |
  | **H-C** | one row of the table above was vacuous: the duplicate-endpoint guard test 27 neutralises is **byte-identical to the pre-fix tree**, so it was never the M-2 defect. The real fix is the validate-then-memset reorder plus the `race_finish_punch` ref release, and restoring the pre-fix form left the suite green | M-2's row re-attributed; a test that observes the ref release |
  | **M-1 (2nd)** | `natpmp.c` refuses the real default gateway in a harness build; `upnp.c` had no equivalent, and `-DHAVE_UPNP` is in the harness's own `flags.make`. Only a config flag stood between the suite and the developer's router | `upnp_ensure_cached`, test 23e |

  **The structural lesson, recorded because it is the one that
  generalises.** Both H-A and H-B trace to the same mistake: a
  production margin sized against a harness constant, and a harness
  range chosen where the fix worked. `SB6_SPLIT_SKEW_MS` and the sweep
  bounds were literals picked from a passing run, so the suite's search
  space was defined by the answer rather than by the mechanism. The
  probe points and the sweep range are now DERIVED from the production
  constants that place the decision (`DirectP2P_TestHook_RaceRelayArmMs`,
  `...RaceRelayGraceMs`, `punch_leg_ms`), and the grace is sized from a
  network quantity — one peer-to-peer round trip — with the derivation
  written beside it. The direction of the dependency matters and is now
  one-way: tests may derive from production constants; production
  margins may never derive from test constants.

### 8.10 Residuals, stated rather than hidden

- **Scenario A is still 8 s/attempt**, because the 8 000 ms signalling
  budget is the longest single leg and racing cannot shorten it. Cutting
  that number means cutting `SIGNAL_BUDGET_MS`, which is a separate
  judgement about how long to wait for a host that may simply be slow to
  register — not a racing problem.
- **The S2 auto-retry still doubles everything.** One attempt is ~5-8 s;
  the user-visible worst case is ~10-16 s because a terminal failure is
  retried once with a fresh local port. Removing the retry is a different
  trade (it demonstrably rescues joins — test 6 part B) and was not made
  here.
- **How often a real pair lands in the band was NOT measured — only
  bounded from the code on both sides.** The band is a function of start
  skew, so its field rate is a function of the skew distribution. What
  the two implementations say:

  - The host enters the race **only** from `try_handle_deliver`, and
    only while still in `HOST_WAITING`
    (`src/netplay/direct_p2p.c:4062-4067`, spawn at `:4162`).
  - The server sets `pairedPeer` only on the REGISTER that fills the
    joiner slot and then pushes **one** unsolicited DELIVER to the host
    (`tools/rendezvous-server/rendezvous-server.js:1256-1258` and
    `:1283-1286`). It is a bare `socket.send` — **no retransmit**.
  - So when that push arrives, the host starts roughly one server
    one-way delay plus one frame behind the joiner: far below every band
    measured in §8.4 rule 1b.
  - When it is **lost**, the host learns only from the reply DELIVER to
    its own next REGISTER (`rendezvous-server.js:1279-1280`), and that
    interval is **5 000 ms** (`src/port/config/config.c:111`, loop at
    `src/netplay/direct_p2p.c:2686-2733`). One lost datagram therefore
    places the host's race start at a roughly uniform point in the next
    5 s — a range that spans every band measured.

  Both the loss rate and the resulting distribution are unmeasured;
  there is no real-network capture of the pairing path. The rate is
  small and it is not zero. The fix does not depend on the answer — it
  converged at all 97 sweep points from 1 800 to 6 600 ms — which is the
  reason to prefer it over sizing a timer against a guess about how
  skewed a real pair can get.
- **Above a 600 ms RTT the split brain is still reachable, and the band
  is `2*owd - RACE_RELAY_GRACE_MS` wide.** Measured on the post-fix tree
  at a 400 ms one-way delay: splits at skew 5 200 and 5 300 ms, 200 ms
  wide, immediately past the punch send end — the width and the location
  the condition in §8.4 rule 1b predicts. It is stated here rather than
  hidden because the fix's guarantee is conditional and the condition is
  checkable: raise `RACE_RELAY_GRACE_MS` above the pair's RTT and it
  closes. That is the difference from the shipped H-3 grace, which no
  value closed. Not fixed by raising the default because 600 ms already
  covers roughly 3x the worst transatlantic RTT and every extra
  millisecond is paid by *every* relayed pair, on the connection path.
- **The grace has to fit inside the race budget or it is not a grace.**
  If `race_budget_ms - (punch send end) < RACE_RELAY_GRACE_MS` the budget
  fires part-way through the grace, the early peer commits inside a
  window it promised to spend listening, and the band returns —
  reproduced at `S6_SPLIT_BUDGET_MS=5200` with a split at skew 5 050 ms.
  The punch SEND window is therefore capped so a full grace always fits
  (`direct_p2p.c`, section 2 of `p2p_race`); re-running the same probe
  after the cap gives **0 splits across 41 races**. On the shipped
  defaults the cap never binds for `punch[0]` (5 000 + 600 against an
  8 000 ms budget); it binds only for a DELIVER candidate armed later
  than `race_budget - grace - punch_leg` = 2 400 ms, and it trades some
  of that candidate's punch window for a decision both peers can agree
  on.
- **A relayed pair now waits ~2.5 s longer for its handoff.** This is
  the price of the H-A re-anchor and it is paid by exactly the pairs
  that end up on the relay. The relay used to commit one grace window
  after it became *ready* (~3.1 s into the race on the defaults); it now
  commits one grace window after the last punch candidate stops sending
  (`punch_leg_ms` + `RACE_RELAY_GRACE_MS` = 5 000 + 600 = **5 600 ms**
  for `punch[0]`). Nothing else about the relay leg moved: it still
  ARMS at 2 500 ms and still does its REQ/GRANT/PIN work concurrently
  with the punch, which is where S6's measured win came from — pre-S6
  the same pair paid 5 000 ms of punch **plus** up to 4 000 ms of relay
  serially. The pure-failure path is unchanged: with no relay leg in
  play there is nothing to diverge about, so candidates are torn down at
  `punch_leg_ms` exactly as before and §8.5's scenario-A/B timings and
  test 18's cascade bound are untouched.
- **A pair that needs longer than `punch_leg_ms` to punch is still
  relayed** where the pre-S6 code would have punched for the full
  bilateral window on both sides first. That bound is now the *punch's
  own* window rather than `RACE_RELAY_ARM_MS`, so it is strictly more
  generous than what shipped, but it is still a bound. Closing the gap
  entirely means letting the relay leg finish and then *upgrading* to a
  direct link mid-session, which needs a GekkoNet remote-address relearn
  path that does not exist (`backend.h`; the remote is registered once
  at configure time in `netplay.c`). Named here rather than hidden.
- **A genuinely ASYMMETRIC punch path still splits the two peers.** If
  one side's datagrams traverse and the other's do not, one peer confirms
  and the other never can, so one punches and one relays no matter how
  long the grace window is. This is **not** new in S6 — the pre-S6 serial
  cascade diverged the same way, because each side decided locally there
  too — and the S6 fix deliberately targets the *timing* case, which S6
  did create. The mid-session relearn above is the only real answer to
  the asymmetric case.
- **The overlapping stage timings are a diagnostic regression** for anyone
  grepping old report lines: `punch + signal + bilateral + relay` no
  longer approximates the elapsed time. `race=` is the replacement, and
  §8.7 says so, but a reader of an old log and a new log side by side will
  see the shapes differ.
- **No on-device measurement.** Every number in §8.5 is from the macOS
  host build. The MiSTer is a 800 MHz ARM with a single GbE interface; the
  race loop's cost is one non-blocking recv and a handful of 17-byte sends
  per 5 ms, so it is not expected to behave differently, but that is
  reasoning, not a measurement.
- **(L-4) The H-3 relay grace is suppressed in exactly the case it was
  written for — a slow relay.** The commit condition is
  `if (!grace_done && !budget_done) goto relay_grace_pending;`, with
  `budget_done` computed at `direct_p2p.c:2024` and the deferral to
  `relay_grace_pending` at `direct_p2p.c:2096` — the two bounds are
  OR'd, so `budget_done` short-circuits the grace.
  A relay that becomes ready with less than `RACE_RELAY_GRACE_MS`
  (600 ms) left in the race budget therefore commits on the spot with no
  hold at all, which is the pre-H-3 behaviour and can still split the two
  peers. This is a deliberate priority — never lose a relay we already
  hold to a punch that has not confirmed — and it is **not** a
  regression; H-3's guarantee is simply bounded by it, and that bound is
  not stated anywhere else. The code comment at that site now says so
  too. Not measured: no test places the relay-ready instant inside the
  last 600 ms of the budget.

## 9. S7 — NAT-PMP / PCP (IMPLEMENTED)

miniupnpc only speaks UPnP IGD. Routers whose firmware is Apple- or
BSD-derived speak NAT-PMP (RFC 6886) or its successor PCP (RFC 6887)
instead, and on those a host got **no port mapping at all** — it fell
straight through to STUN and lived or died on the punch. Landed as
`src/netplay/natpmp.{c,h}`, a third mapping backend behind the same
`UpnpMapping` interface (upnp.h:26-39), tried when UPnP fails.
Citations refer to the post-S7 tree.

**Merge note (S6 + S7).** S6 and S7 were implemented in parallel on
separate branches and merged. Two collisions, both resolved in favour of
keeping everything:

- Both stages added a "test 18" to `test_bilateral_punch.c`. S6 owns
  18-21; **the S7 test is test 22** (`test_natpmp_pcp`), and its assertion
  tags were renumbered `18-*` -> `22-*` to match. The neutralisation
  evidence in §9.6 was recorded against the pre-merge `18-*` tags.
- Every test site that sets `netplay-direct-p2p-disable-upnp` now also
  sets `netplay-direct-p2p-disable-natpmp`, including the S6 sites, which
  predate S7. Without that pairing a Linux test run would install a real
  1-hour NAT-PMP mapping on the developer's router — the S7 backend is a
  second, independent path to the LAN and `disable-upnp` does not cover
  it.

### 9.1 Hand-rolled, and no new dependency

The original sketch said "add libnatpmp (or a ~200-line PCP client)".
It is the client, not the library, and that was the authorized call:
the entire protocol surface we need is a 60-byte packet, a 12-byte
packet, a 2-byte packet and a `select()` loop. A library would mean a
new `build-deps.sh` entry, a new cross-compiled `.so` on the MiSTer
deploy, and a new thing to keep in the `--delete` rsync shield — for
code smaller than the CMake needed to find it. `natpmp.c` uses only
the POSIX socket API the tree already links (Winsock behind `_WIN32`,
same shape as `stun.c`). CMakeLists gained a comment saying so and
nothing else; the file is picked up by the existing
`GLOB_RECURSE CONFIGURE_DEPENDS src/*.c`.

### 9.2 Protocol, and the ordering that matters

PCP first, NAT-PMP on downgrade — RFC 6887 Appendix A: "A client
supporting both NAT-PMP and PCP SHOULD send its request using the PCP
packet format." Both live on UDP 5351 (RFC 6886 §3.1, RFC 6887 §19.1),
so one socket serves both and the first octet selects the dialect.

| field | source |
|---|---|
| PCP common request header, 24 B: Version=2, R\|Opcode, Reserved(16), Requested Lifetime(32), Client IP(128) | RFC 6887 §7.1 |
| PCP common response header, 24 B: Version, R\|Opcode, Reserved(8), Result Code(8), Lifetime(32), Epoch(32), Reserved(96) | §7.2 |
| PCP MAP block, 36 B: Nonce(96), Protocol(8), Reserved(**24**), Internal Port, Suggested/Assigned External Port, Suggested/Assigned External IP(128) | §11.1, §11.2 |
| MAP Opcode = 1 | §19.2 opcode registry |
| Protocol 17 = UDP | §11.1 ("This field contains 17 (UDP) if the Opcode is intended to create a UDP mapping") |
| IPv4 carried as IPv4-mapped IPv6; **all 96 leading bits** must be checked on receive | §5 |
| the all-zeros IPv4 address is `::ffff:0:0`, **not** `::` | §5 — see below |
| PCP result codes 0..13 | §7.4 |
| NAT-PMP public address req/resp (2 B / 12 B) | RFC 6886 §3.2 |
| NAT-PMP mapping req/resp (12 B / 16 B), OP 1 = UDP, 2 = TCP, all multi-byte fields network order | §3.3 |
| deletion = lifetime 0, and Suggested External Port **MUST** be 0 | §3.4 |
| NAT-PMP result codes 0..5 | §3.5 |

Two details are load-bearing and were each nearly got wrong:

**The downgrade signal has the R bit clear.** A NAT-PMP-only gateway
answers a PCP request with RFC 6886 §3.5's "Unsupported Version" frame:
`Vers=0 | OP=0 | Result Code=1 | Epoch` — eight bytes. Overlaid on the
PCP response header that reads as version 0, Reserved 0, Result Code 1,
i.e. exactly the version-zero `UNSUPP_VERSION` that RFC 6887 §9 step 4
defines as "this is a NAT-PMP server". But its OP byte is 0, so the PCP
**R bit is CLEAR**. A parser that tests R before version discards the
one frame that tells it to downgrade, and the gateway looks silent. The
version test therefore runs first, before the length check and before
the R check (natpmp.c:152-176), with the reasoning in the comment so
nobody "tidies" it into the obvious order.

**`::ffff:0:0`, not `::`.** RFC 6887 §11.1 says a client with no
external-address preference "MUST use the address-family-specific
all-zeros address (see Section 5)". §5 is explicit that the all-zeros
*IPv4* address is "80 bits of zeros, 16 bits of ones, and 32 bits of
zeros (`::ffff:0:0`)" — only the all-zeros *IPv6* address is 16 zero
bytes. The test's hand-built expectation had this wrong and the code
right; §5 settled it, and the test now carries the citation.

The PCP Client's IP Address field is the socket's **actual** source
address, read back with `getsockname()` after `connect()`
(natpmp.c:678-706), because §7.1 defines it as "the source IPv4 or IPv6
address in the IP header used by the PCP client when sending this PCP
request" and §7.4's `ADDRESS_MISMATCH` (12) exists precisely to catch a
client that guessed. The `connect()` also buys RFC 6886 §3.2/§3.3's
"client MUST check the source IP address, and silently discard the
packet if the address is not the address of the gateway" for free, at
the kernel.

Within the NAT-PMP phase the public-address request goes **first and
serially** (§3.1: "clients SHOULD NOT issue multiple concurrent
requests … it SHOULD queue them and issue them serially"). It is the
cheapest possible liveness probe, and it is the only source of the
external IP on this protocol — a NAT-PMP mapping response carries none,
unlike PCP's (§11.2). That address is what §3.6's CGNAT gate compares
against STUN, so a mapping without it would be a mapping the gate
cannot judge.

### 9.3 The retransmit ladder is deliberately truncated

RFC 6886 §3.1 specifies: send, wait 250 ms, retransmit, wait 500 ms,
"with the interval between attempts doubling each time", up to a ninth
attempt and a final 64-second wait before concluding the gateway does
not speak NAT-PMP. That is ~127 seconds. PCP's own default (RFC 6887
§8.1.1: IRT 3 s, MRC 0, MRT 1024 s) is unbounded by design.

Neither is shippable behind a "Host Game" click that is already capped
at 6 s for UPnP. The ladder is cut to its **first three rungs** —
250/500/1000 ms, doubling shape preserved — and every wait is
additionally clamped by an absolute wall-clock deadline the caller sets.

#### 9.3.1 The PCP ladder deviates from RFC 6887 §8.1.1 too — four SHOULDs

The NAT-PMP truncation above was disclosed from the start. The PCP one
was not, and it is larger. §8.1.1 specifies:

| §8.1.1 | Spec | Shipped | Effect |
|---|---|---|---|
| `IRT: Initial retransmission time, SHOULD be 3 seconds` | 3000 ms | 250 ms | first retransmit 12× sooner |
| `MRC: Maximum retransmission count, SHOULD be 0 (0 indicates no maximum)` | unlimited | 3 | gives up after three sends |
| `MRD: Maximum retransmission duration, SHOULD be 0 (0 indicates no maximum)` | unlimited | 1750 ms/phase | gives up inside two seconds |
| `Each of the computations of a new RT include a new randomization factor (RAND), which is a random number chosen with a uniform distribution between -0.1 and +0.1.` | required | **absent** | no de-synchronisation between clients |

The first three are the same shippability argument as §3.1: PCP's
defaults describe a daemon maintaining a mapping indefinitely, not a
one-shot probe behind a button press. The **missing RAND is a genuine
gap, not a trade**: §8.1.1 states its purpose is "to minimize
synchronization of messages transmitted by PCP clients", and a LAN full
of 3S instances rebooting together would retransmit in lockstep. The
mitigating facts are that a household runs one or two of these, and
that the one place where synchronised retransmission is actually
plausible — every device on a LAN reacting to a gateway reboot — IS
randomised, because RFC 6886 §3.7's mandatory 0-5 s pre-renewal jitter
is implemented (§9.4). Adding RAND to the probe ladder is cheap and
remains open.

One §8.1.1 MUST **is** honoured: "The retransmissions MUST use the same
Mapping Nonce value (see Sections 11.1 and 12.1)" — see §9.4's nonce
persistence, which extends that to renewals and deletes as well.

#### 9.3.2 Each phase gets its own budget (review H-6)

A probe runs up to **three** ladders in sequence: the PCP MAP, then —
after a §9 downgrade — the NAT-PMP public-address request, then the
NAT-PMP mapping request. These originally shared ONE absolute deadline
computed once at the top of `Natpmp_AddMapping`. Measured consequence
on a mock gateway that answers at a fixed latency:

| Gateway latency | Mapping | Elapsed | Map requests seen by the gateway |
|---|---|---|---|
| 300 ms | yes | ~1.2 s | 1 |
| 700 ms | **no** | 3896 ms | **0** |

RFC 6886 §3.1 names "a slow NAT gateway that takes perhaps half a
second to respond to a NAT-PMP request" as normal. The cliff sat below
that. Each phase now takes a fresh `NATPMP_PHASE_BUDGET_MS` at phase
start, still clamped by the caller's overall ceiling.

That alone was not enough, because the failure was a **queue**, not a
clock: three rungs per phase put three datagrams into a single-threaded
router's backlog and then timed out waiting for the reply to the first.
So the ladder now also **stops retransmitting once the gateway has
answered anything at all** — including a datagram this phase rejects as
NOT_OURS, since the socket is `connect()`ed and only the gateway's
datagrams are delivered. That is §3.1's own instruction: "the client
SHOULD respect this and allow the NAT gateway to operate at the pace it
can manage, and not overload it by issuing requests faster than the
rate it's answering them." With both fixes a 700 ms gateway maps in
~2.8 s and the gateway sees 2 PCP + 1 address + 1 mapping request.

Residual: an attacker who can spoof the gateway's source address can
latch that flag early and cost us the retransmits. The worst outcome is
one missed mapping and a fall through to STUN — identical to a silent
router — because a forged frame still has to pass §11.4's nonce /
protocol / internal-port matcher to become a mapping.

#### 9.3.3 Budgets, as shipped

- `NATPMP_PHASE_BUDGET_MS` = 1750 ms, **derived in natpmp.c from the
  ladder table** rather than written twice, so lengthening the ladder
  cannot silently leave the budget behind.
- `NATPMP_PROBE_BUDGET_MS` = 3 × phase = **5250 ms**. Reached only when
  a gateway answers the PCP downgrade and the address request and then
  goes silent on the mapping request; a *wholly* silent gateway costs
  two phases (3500 ms), because the address request timing out ends the
  attempt.
- `NATPMP_RENEW_BUDGET_MS` = 2 × phase = **3500 ms** (a renewal knows
  its backend: PCP runs one phase, NAT-PMP runs address-then-mapping).
  This deliberately **exceeds** `upnp_renew_join_and_discard`'s 2 s join
  budget, reversing the earlier choice. A renewal caught in flight by
  teardown is *detached*, which is an already-handled, already-bounded
  path; a renewal budget too short for a slow gateway loses the mapping
  **mid-session**, which is not.

The cost of being wrong is one missed mapping on a very slow gateway,
which degrades to exactly today's behaviour: fall through to STUN. The
worst case for the whole probe is `UPNP_PROBE_BUDGET_MS` (6 s) +
`NATPMP_PROBE_BUDGET_MS` (5.25 s) = **11.25 s**, and it is reached only
when **both** protocols are dead silent. The case S7 exists for — a
NAT-PMP router with no IGD — costs miniupnpc's own 2 s SSDP timeout
plus one LAN round-trip, because a router that speaks NAT-PMP answers
at once.

### 9.4 One mapping struct, three backends

`UpnpMapping` gained two fields (upnp.h:14-39): `backend`
(`PortMapBackend` NONE/UPNP/NATPMP/PCP) and `lifetime_s`.

- **Teardown dispatches** through `portmap_remove` (direct_p2p.c:2353),
  and every removal site goes through it. `Upnp_RemoveMapping` and
  `Natpmp_RemoveMapping` each additionally **refuse** a mapping they do
  not own. This is not defensive decoration: on a router that speaks
  both, deleting a NAT-PMP mapping through the IGD would remove
  whatever unrelated IGD entry happens to sit on that external port.
- **Renewal renews the backend that holds the mapping.** The S1
  half-life timer is unchanged in shape; `upnp_worker_fn` now takes a
  backend hint, so a renewal runs exactly one ladder instead of
  re-running UPnP discovery against a NAT-PMP-only router every
  half-lease. Both specs also want the renewal to carry the
  **assigned** external port so a rebooted gateway can recreate the
  same mapping (RFC 6886 §3.3, RFC 6887 §11.2.1); that is what the
  existing `preferred_external = s_upnp_mapping.external_port` line
  already did.
- **The renewal interval now follows the granted lease**
  (`portmap_renew_interval_ms`, direct_p2p.c:2566). RFC 6886 §3.3: "The
  NAT gateway MAY reduce the lifetime from what the client requested."
  A router granting 120 s against our 3600 s request would have
  silently lost the mapping 28 minutes before a fixed half-hour timer
  fired. Half-life satisfies §3.3 ("halfway to expiry time, like DHCP")
  and sits inside RFC 6887 §11.2.1's recommended 1/2-to-5/8 window,
  floored at 4 s because §11.2.1 forbids renewals less than four
  seconds apart. UPnP is untouched: miniupnpc reports no granted lease,
  so `lifetime_s` stays 0 and the old constant applies.
- **The §3.6 CGNAT gate is NOT duplicated.** It keys off
  `external_ip`, which all three backends fill, so a NAT-PMP mapping
  whose external IP is `100.64/10` is dropped exactly as a UPnP one is.
  The only S7 change inside the gate is that the release dispatches on
  the backend.
- **The gate fails CLOSED on an absent external address** (review
  M-5.4). `direct_p2p_ip_is_nonpublic("")` used to return false —
  "cannot prove anything, keep the mapping" — but an *empty* string is
  not an unclassifiable address, it is the absence of one, and the
  gate's entire job is to compare that address against STUN's. It now
  returns true for NULL/empty while still returning false for
  present-but-unparseable text (which really does prove nothing) and
  for a public-but-different address (1:1 NAT / DMZ). Upstream of it,
  `Natpmp_AddMapping` now refuses outright to build a mapping from a
  gateway that reports `0.0.0.0`, on either dialect.

#### 9.4.1 The PCP Mapping Nonce is persisted (review H-5, ALPHA BLOCKER)

RFC 6887 §11.3: *"If operating in the Simple Threat Model (Section
18.1), and the internal port, protocol, and internal address match an
existing explicit dynamic mapping, but the mapping nonce does not
match, the request MUST be rejected with a NOT_AUTHORIZED error with
the lifetime of the error indicating duration of that existing
mapping."* §18.1 is the model consumer NAT boxes run in, and §8.1.1
says the same for retransmits: *"The retransmissions MUST use the same
Mapping Nonce value."*

As originally shipped, every PCP call minted a **fresh** nonce —
creation, half-lease renewal, and teardown delete alike. On a
conforming gateway that means the renewal is refused, the delete is
refused, and neither refusal is checked. The mapping could be neither
kept alive nor removed; it simply expired, while `active` stayed true
and the room code kept advertising its port. **This was a regression
against pre-S7 on PCP routers**, where the host advertised the STUN
endpoint it was actively maintaining. It presents to the user as "it
worked, and then it went dead."

The nonce now lives in `natpmp.c`, keyed by internal port: minted on
first use, reused by every renewal and by the delete, and forgotten
after the delete so the next creation draws a new one. It is *not* a
field on `UpnpMapping`, because `Natpmp_AddMapping` memsets its
out-parameter on entry and the renewal path hands it a zeroed struct —
a struct field would have to be threaded through `UpnpJob` to have any
effect. The program holds exactly one mapping (`s_upnp_mapping`), so
the port key is sufficient; the single-writer invariant that makes the
module-global safe is spelled out at its definition, together with what
would have to change if a second concurrent mapping ever appeared.

#### 9.4.2 A mapping that cannot be renewed is DROPPED (review H-5, M-5.5)

The downstream half of the same chain. Three fixes, all in
`upnp_renew_tick`:

- **The lease is tracked.** `s_portmap_lease_expiry_ms` is armed from
  the granted `lifetime_s` and re-armed on every successful renewal
  (0 = "no deadline known", the UPnP case). Arming it from the first
  main-thread sighting rather than from the grant instant errs a frame
  or two LATE, which can only ever keep a mapping marginally longer
  than the router does — never drop a live one.
- **A renewal that fails past expiry drops the mapping** and
  re-publishes the STUN endpoint through `host_commit_endpoint` (a
  small extraction from the drift handler, which needed exactly the
  same re-encode / re-derive / restart-rendezvous sequence). The user
  gets "Share the NEW code." instead of a code that silently stopped
  working.
- **The drift re-encode tests `portmap_mapping_usable()`**, not
  `.active`, so a mapping past its lease can no longer pin the
  advertised port.
- **The retry scales to the lease** (review M-5.2). It was a flat five
  minutes; against a router that grants 120 s — permitted by §3.3, "The
  NAT gateway MAY reduce the lifetime from what the client requested" —
  the one retry that mattered landed four and a half minutes after the
  mapping was already gone. It is now a quarter of the lease, floored
  by §11.2.1's four seconds and capped at the old five minutes so UPnP
  behaves exactly as before.

#### 9.4.3 Gateway reboot detection, RFC 6886 §3.6 (review M-5.1)

§3.6's MUST is on the **client**: *"Whenever a client receives any
packet from the NAT gateway ... the client computes its own
conservative estimate of the expected SSSoE value by taking the SSSoE
value in the last packet it received from the gateway and adding 7/8
(87.5%) of the time elapsed according to the client's local clock since
that packet was received. If the SSSoE in the newly received packet is
less than the client's conservative estimate by more than 2 seconds,
then the client concludes that the NAT gateway has undergone a reboot
or other loss of port mapping state, and the client MUST immediately
renew all its active port mapping leases."*

The epoch was parsed into all three response structs and read by
nobody, so a router reboot emptied the mapping table and this client
went on advertising a port that forwarded nothing until the lease ran
out. The estimator is now implemented exactly as written, fed by every
response the matcher proved is ours (an unattributable datagram's clock
is not evidence, and letting one drive the estimator would hand an
off-path attacker a free "your mappings are gone" signal). On a
detected reboot the renewal deadline is pulled in to now + a random
0-5 s delay, which is §3.7's own mandatory jitter: *"the client MUST
first delay by a random amount of time selected with uniform random
distribution in the range 0 to 5 seconds, and then send its first port
mapping request."*

#### 9.4.4 Short PCP error responses are processed (review M-5.3)

RFC 6887 §8.3: *"Responses shorter than 24 octets, longer than 1100
octets, or not a multiple of 4 octets are invalid and ignored."* The
parser demanded the 60-octet MAP response, so a gateway refusing with
only the §7.2 common header was indistinguishable from a dead one and
cost the caller the whole ladder. The floor is now §8.3's 24, and the
two length rules it states alongside are enforced as well. A short
**success** is still dropped — §11.4 matches a MAP response on "the
protocol, the internal port, and the mapping nonce", none of which a
header-only frame carries, and an unmatched success would install a
mapping whose external port is not even present in it. A short
**error** is reported, so a refusing gateway fails fast. Residual: an
attacker who can spoof the gateway's source address can abort one probe
that way; the fallback is STUN, exactly as if the router were silent.

### 9.5 Config

New kill switch `netplay-direct-p2p-disable-natpmp` (bool, default
false), **deliberately separate** from `disable-upnp`. The latter
exists for one specific defect — libminiupnpc 2.2.1's `upnpDiscover()`
segfaulting on MiSTer when a Realtek 8821cu USB WiFi adapter sits
alongside eth0 — and a user who sets it to dodge that crash should
still get a mapping from a router that speaks NAT-PMP. The two backends
share no code, so folding the switches together would take a working
mapping away for no reason. `docs/config.md` updated.

### 9.6 Tests

Test **22** in `test_bilateral_punch.c` (`test_natpmp_pcp`), in three
parts. (It is registered as test 22; earlier drafts of this section
called it 18 and cited stale line numbers — the function name is the
stable handle.)

- **Codec vs literal RFC bytes**, in the spirit of test 17. The
  expectations are hand-built from the §7.1/§11.1 and §3.2/§3.3
  diagrams, **not** produced by `natpmp.c` — an encoder and a test that
  agreed on a wrong offset would both be wrong together. Reject tables
  cover wrong version, R bit clear, wrong opcode, wrong 96-bit nonce
  (first and last byte), wrong Protocol, wrong Internal Port, a
  non-IPv4-mapped external address, and every truncation from 0 bytes
  up. Deletion shape is pinned on both protocols, including that the
  NAT-PMP builder **forces** Suggested External Port to 0 rather than
  trusting the caller.
- **The client against a localhost mock gateway**: PCP success, the §9
  downgrade to NAT-PMP, a refusal, silence, and a single-backend
  renewal. Silence asserts the elapsed wall clock is inside budget and
  that a retransmit actually happened. That was originally described as
  "the assertion that fails if the §3.1 ladder ever stops being
  truncated"; **it is not**, and the review proved it — the phase budget
  clamps a nine-rung ladder to the same elapsed time. Test 23b pins the
  shape instead.
- **The CGNAT gate end to end** on the real host state machine, run
  **twice**: with a `100.64.5.9` external IP the advertised port must
  be STUN's, with a `198.51.100.30` one it must be the mapped port. The
  public-IP run is the control — a drop-only assertion would also pass
  if the NAT-PMP mapping had never been created at all.

#### 9.6.1 Tests 23a-23d, and why they exist

The original S7 claim was that every assertion had been proven able to
go red. An adversarial re-derivation found **five of those
neutralisations vacuous** — the reviewer reverted the guarded behaviour
and the suite stayed green. Tests 23a-23d exist to close exactly those
holes, and the patch-to-red table below is inlined here rather than
cited to a document that does not exist.

- **23a — the DISABLE_UPNP / DISABLE_NATPMP pairing.** No test may
  install a mapping on a real router. Thirteen sites in
  `test_bilateral_punch.c` disable UPnP before driving the host state
  machine, and each is hand-paired with a `disable-natpmp`. That
  discipline is now *asserted*, by scanning `__FILE__` for every
  DISABLE_UPNP-true site and requiring a DISABLE_NATPMP-true within the
  next two non-blank lines. Failing to open the file is a FAILURE, and
  the scan must find at least 11 sites, so a scanner that silently
  matched nothing cannot pass.
- **23b — the review fixes**, each keyed to the reversion it catches:
  the ladder shape (via a test hook *and* a behavioural rung-timing
  observation), the 700 ms gateway measurement, the Mapping Nonce
  observed on the wire across create/renew/delete, §8.3 short errors,
  §3.6 epoch rollback, the no-external-address refusal, the renewal
  interval and retry as functions of the granted lease, and the
  backend-ownership refusal *with a control* proving that a mapping
  this backend does own really does emit a delete down the same path.
- **23c — the `disable-natpmp` kill switch, end to end**, in both
  polarities. `try_portmap` used to hold a *second* copy of the check
  that short-circuited before the worker ran, which is precisely why
  deleting the worker's copy left the suite green; enforcement is now
  in one observable place. The enabled case is the control.
- **23d — a lost mapping stops being advertised.** An 8 s lease, then
  the gateway goes silent; the advertised port must revert from the
  mapped one to the STUN-observed one. Measured: 41200 → 40000 after
  11.5 s. The pre-drop assertion that the code carried the *mapped*
  port is the control that stops this passing when no mapping existed.

#### 9.6.2 Patch-to-red table

Every row was applied to the tree, rebuilt, and run against the full
`--test-bilateral-punch` harness; the recorded exit code is the true
process exit. The first five are the reviewer's own vacuous
neutralisations, now RED.

| # | Patch applied | Exit | First failing assertion |
|---|---|---|---|
| R1 | PCP builder: swap the Internal-Port and Suggested-External-Port writes | **1** | `22-pcp-req-internal-port`: octets 40-41 carry 40001, expected 54321 |
| R2 | Restore RFC 6886 §3.1's full nine-rung ladder | **1** | `23b-ladder-steps`: 9 rungs, expected 3 (+ `23b-ladder-count`: 5 requests, expected 3) |
| R3 | Pin `portmap_renew_interval_for` to a flat 30 minutes | **1** | `23b-renew-interval-120`: 1800000 ms, expected 60000 ms |
| R4 | Delete the `disable-natpmp` check in `upnp_worker_fn` | **1** | **two** failing assertions, in this order: `23c-disabled` first — *"advertised port 41100, expected 40000 (netplay-direct-p2p-disable-natpmp=1)"* — then `23c-disabled-silent`: *"switch was SET and the gateway still received 3 datagram(s) (1 PCP, 1 addr, 1 map)"*. The port assertion fires first because it is checked first: the un-gated worker completes the mapping, so the room code carries the mapped port **before** the datagram count is looked at |
| R5 | Delete the backend-ownership refusal in `Natpmp_RemoveMapping` | **1** | `23b-own-backend1`: sent 1 NAT-PMP datagram for a UPnP-owned mapping |
| R6 | H-6: give all three phases the one shared deadline again | **1** | `23b-noise-starved`: 0 address + 0 mapping requests in 5260 ms |
| R7 | H-6: retransmit even after the gateway has answered | **1** | `23b-slow-gw-700`: no mapping, 3880 ms, 3 address requests |
| R8 | H-5: mint a fresh PCP nonce on every call | **1** | `23b-nonce-renew-differs` (and `23b-nonce-del-differs`) |
| R9 | M-5.3: discard PCP responses shorter than 60 octets | **1** | `23b-short-error`: parsed NOT_OURS, expected REFUSED (+ 3510 ms to give up) |
| R10 | M-5.1: make the §3.6 epoch estimator a no-op | **1** | `23b-epoch-reset`: SSSoE fell 100003 → 3 and the client did not notice |
| R11 | M-5.4: return false for an empty external IP in the CGNAT gate | **1** | `23b-gate-empty-closed` |
| R12 | M-5.2: flat five-minute retry regardless of lease | **1** | `23b-renew-retry-120`: 300000 ms, expected 30000 ms |
| R13 | M-5.5: drop the lease-expiry check from the renewal tick | **1** | `23d-still-advertised`: still port 41200 after 25 s on an 8 s lease |
| R14 | M-5.4: accept a mapping with a `0.0.0.0` external address | **1** | `23b-noextip-pcp` |
| R15 | Harness guard: consult the real default route with no mock set | **0** | **none — see below** |
| R16 | 23a scanner: add an unpaired DISABLE_UPNP site | **1** | `23a-unpaired`, naming the offending file and line |
| R17 | H-6 "BEFORE": all three reverted together — shared deadline, no suppression, and the original 4000 ms probe budget | **1** | `23b-slow-gw-700`: no mapping, **3886 ms**, 3 address requests |

R17 is the pre-fix state reconstructed exactly, and it reproduces the
independently reported measurement: 700 ms → no mapping in 3886 ms with
three public-address requests (the review reported 3896 ms and three
address requests). The 300 ms control still mapped, in 1534 ms. After
the fix the same gateway maps in ~2.8 s having been sent 2 PCP + 1
address + 1 mapping request.

**R15 is GREEN, and that is reported rather than hidden.** On the
development host (macOS) `discover_gateway_platform` is the deliberate
no-op stub (natpmp.c), so removing the harness guard changes nothing
observable: the platform already reports no gateway. The guard is
defense-in-depth whose value appears only on Linux, where discovery is
real — and no macOS-observable neutralisation for it exists, because
constructing one would mean faking a gateway, which is the exact thing
the guard prevents. The assertion it sits behind
(`22-addmapping-without-gateway`) is NOT vacuous in general: it goes red
if `Natpmp_AddMapping` ever stops failing closed without a gateway. What
cannot be demonstrated here is that the *new* guard is the thing
producing that outcome on this platform.

**R6 and R7 are separately load-bearing, and neither subsumes the
other.** R7 (retransmit suppression removed, per-phase budgets kept)
fails the 700 ms measurement. R6 (per-phase budgets removed, suppression
kept) passes it — 700 ms still maps — and is caught instead by the noise
case, where an unmatchable-PCP phase with suppression in force waits out
a *shared* deadline and starves the NAT-PMP fallback of every datagram.
Had only the 700 ms test been written, the per-phase budget would have
been another vacuous guard.

Nothing here can reach a real router. Three independent guards now:
every client call goes through `Natpmp_TestHook_SetGateway` at a
127.0.0.1 mock; gateway discovery is Linux-only so a macOS run cannot
find a real gateway even with the hook forgotten; and — new — a build
with both `NETPLAY_TEST_HOOKS` and `ENABLE_NETPLAY_TESTS` (the harness
binary, and nothing else) **refuses** to consult the default route when
no mock is configured, which is what makes the claim hold when the suite
runs on Linux.

### 9.7 Residuals, stated rather than hidden

- **Gateway discovery is Linux-only.** `/proc/net/route` covers the
  shipping target (MiSTer, kernel 5.15, single STMMAC GbE, IPv4 only),
  and the column layout and byte order were taken from the kernel's own
  printer (`net/ipv4/fib_trie.c:2984-2994`, v5.15) rather than guessed —
  Destination/Gateway/Mask are `__be32` printed with `%08X`, so parsing
  the hex straight back into `in_addr.s_addr` reproduces the original
  bytes on either endianness and no `htonl` belongs anywhere near it.
  Non-Linux hosts report **no gateway**, on purpose: a BSD
  `sysctl(NET_RT_DUMP)` walk would be untested code on the failure path
  of a feature that mutates the LAN, and reporting nothing is what
  makes the macOS test box physically unable to install a mapping on
  the developer's router. Consequence: on macOS this backend is only
  ever exercised through the test hook.
- **Never exercised against real NAT-PMP or PCP hardware.** Every
  gateway this code has met is the mock in test 18. The mock is a
  second implementation written from the same RFC text by the same
  hand, which is exactly the blind spot §7.6 calls out for the relay
  codec — the codec half is pinned to literal RFC bytes for that
  reason, but the *client* half (does a real Airport/pfSense/OpenWrt
  box actually answer this?) is unproven until someone hosts behind one.
- **The worst case is 11.25 s**, if unlikely: a router that answers
  neither SSDP nor 5351 costs `UPNP_PROBE_BUDGET_MS` (6 s) +
  `NATPMP_PROBE_BUDGET_MS` (5.25 s) before the STUN fallback, instead of
  the pre-S7 6 s. (It was 10 s before review H-6 widened the NAT-PMP
  half from one shared 4 s deadline to three 1.75 s phases.) A *wholly*
  silent gateway actually costs 3.5 s of that, not 5.25, because the
  address request timing out ends the attempt before the mapping phase.
  S6's candidate racing does not help here — this is the host's probe,
  not the joiner's connect.
- **The PCP retransmit ladder deviates from four RFC 6887 §8.1.1
  SHOULDs**, one of which (the missing randomisation factor) is a gap
  rather than a trade. Disclosed in full in §9.3.1.
- **A gateway slower than ~1 s per request still gets no mapping.** The
  three-phase budget totals 5.25 s and each phase's ladder is 1.75 s, so
  a *serialising* router taking 1.5 s per datagram runs out of phase
  before its reply arrives. 700 ms — RFC 6886 §3.1's own "perhaps half a
  second" neighbourhood — is covered and measured (§9.3.2); beyond that,
  the degradation is still the same fall-through to STUN.
- **The lease clock is armed from the main thread's first sighting of
  the mapping, not from the instant the gateway granted it.** That is a
  frame or two of optimism in the safe direction (we may keep a mapping
  marginally longer than the router does, never drop a live one), but it
  is an approximation and it is not measured against the router.
- **Nothing verifies that a gateway ACCEPTED a delete or a renewal.**
  Both are sent and the reply is parsed only far enough to feed the
  §3.6 epoch estimator; a NOT_AUTHORIZED on a delete is not surfaced.
  The lease-expiry drop (§9.4.2) is what limits the damage: a mapping
  that could not be renewed stops being advertised whether or not we
  understood why.
- **PCP options are not implemented.** No `PREFER_FAILURE`, no
  `FILTER`, no `THIRD_PARTY` (RFC 6887 §13). Consequence: per §11.2 a
  normal MAP request "will return an available external port" rather
  than failing when our suggested one is taken, so the gateway may hand
  back a port we did not ask for — which is fine, the room code carries
  whatever we got.
- **Version negotiation stops at 2 and 0.** RFC 6887 §9 step 5 says a
  client receiving `UNSUPP_VERSION` with a version it does not support
  SHOULD try the next-lower one and, having exhausted them, retry in 30
  minutes. We implement version 2 and NAT-PMP's 0 and nothing between,
  and we do not arm the 30-minute retry — the probe is one-shot per
  hosting attempt.
- **The ICMP-unreachable shortcut is inferred from errno, not from the
  ICMP packet.** RFC 6886 §3.1 says an ICMP Port Unreachable for 5351
  lets a client "skip any remaining retransmissions". On a connected
  UDP socket that surfaces as an error on a later `send`/`recv`, which
  is what `np_transact` treats as terminal — but any other socket error
  is treated the same way, and the mapping from ICMP to errno was not
  verified on the MiSTer kernel.
- **(L-3) The PCP nonce globals are single-writer by convention, and a
  detached straggler followed by a SECOND PROBE breaks the convention.**
  The THREADING invariant at `natpmp.c:392-459` argues that a probe
  worker is either joined or detached-and-abandoned, and that a detached
  straggler can never be followed by a renewal. That covers a *renewal*.
  It does not cover a second *probe*, and the second probe is reachable:
  `SDL_DetachThread` (`direct_p2p.c:2476`) abandons the timed-out
  worker, and `try_portmap` returns false **without** adopting the
  result — the `s_upnp_mapping` assignment is on the joined path only
  (`direct_p2p.c:2426`) — so `s_upnp_mapping.active` stays false; the
  FAILED_STUN auto-retry re-spawns `host_thread_fn`
  (`direct_p2p.c:4834`); the "reuse the live mapping" shortcut is gated
  on that same `s_upnp_mapping` flag (`direct_p2p.c:3196`) and is
  therefore skipped; and `try_portmap` runs again (`direct_p2p.c:3203`),
  spawning a second worker into `Natpmp_AddMapping` while the straggler
  may still be inside it. `s_pcp_nonce` / `s_pcp_nonce_valid` /
  `s_pcp_nonce_port` (`s_pcp_nonce` is at `natpmp.c:496`) and
  `s_epoch_reset_pending` (`natpmp.c:563`) then have two unsynchronised
  writers, and `pcp_nonce_acquire`'s check-then-mint is not atomic.
  **Worst case:**
  the persisted nonce and the nonce the router has on file disagree —
  either torn (one thread copying out while the other writes in) or lost
  (both mint, only the last is kept). RFC 6887 §11.3 answers a MAP whose
  internal port/protocol/address match an existing dynamic mapping under
  a *different* nonce with NOT_AUTHORIZED, and a delete is a MAP with
  lifetime 0, so the mapping fails to be created or lingers until its
  lease expires. That is the pre-H-5 behaviour: a lost mapping and a
  fall-through to STUN. It is **not** memory unsafety — every access is a
  fixed-size `memcpy` over a static 12-byte array — and reply matching is
  unaffected, because `Natpmp_ParsePcpMapResponse` compares against the
  request's local copy. **Not bounded in code.** Bounding it means either
  joining the straggler before the second probe or putting a mutex round
  the block; neither was done. Documented at the site.

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
| S4 security | **implemented + adversarially reviewed** (see §6; S4a punch auth, S4b room code — now v3, S4c rendezvous return-routability — the last two are breaking wire/format changes, authorized. Review fixes as-built in §6.8) |
| S5 relay for symmetric-NAT pairs | **implemented + adversarially reviewed** (see §7; custom '3SXR' relay on the existing port, NOT coturn. Closes the one §2 matrix cell that could not connect at all. Pure wire EXTENSION — protocol version stays 2. Review fixes as-built in §7.2/§7.3: CRITICAL-1 session-TTL teardown, HIGH-1 pin source binding, HIGH-2 over-budget liveness, MEDIUM-1..5, LOW-1..2) |
| S6 joiner candidate racing | **implemented + adversarially reviewed** (see §8; one interleaved race on the existing worker thread — no new threads, no new locks. Full-cascade worst case measured 9 677 -> 5 114 ms per attempt. Review fixes as-built in §8.4 rules 1b/2b, §8.5, §8.8 and §8.9: H-1 the confirmation-tail budget exemption, H-2 real-wire punch legs in the suite, H-3 the split brain — measured 150 ms band — H-4 per-candidate punch windows, H-7 ordering-rule-1 tear-down coverage, M-1..M-4, L-1) |
| S7 NAT-PMP / PCP | **implemented + adversarially reviewed** (see §9; hand-rolled RFC 6887 PCP client with RFC 6886 NAT-PMP downgrade, NO new library, as a third backend behind `UpnpMapping`. RFC 6886 §3.1's ~127 s retransmit ladder is deliberately truncated to 250/500/1000 ms per PHASE under an absolute ceiling — §9.3, and the PCP ladder's four §8.1.1 deviations are disclosed in §9.3.1. Review fixes as-built: H-5 the PCP Mapping Nonce is persisted so renewals and deletes are not NOT_AUTHORIZED'd (§9.4.1) and a mapping that cannot be renewed is dropped instead of advertised forever (§9.4.2); H-6 per-phase retransmit budgets plus §3.1's "do not overload it" rule, measured 700 ms gateway no-mapping → mapping (§9.3.2); H-7 five vacuous neutralisations replaced by tests 23a-23d with an inline patch-to-red table (§9.6); M-5.1 §3.6 epoch reboot detection implemented, M-5.2 lease-scaled renewal retry, M-5.3 §8.3 short error responses, M-5.4 the CGNAT gate fails closed on an absent external address, M-5.5 lost mappings stop being advertised. Residuals in §9.7, chiefly: gateway discovery is Linux-only, the client has never met real NAT-PMP/PCP hardware, and a gateway slower than ~1 s per request is still out of budget) |
| S8 netns verification harness | planned above |
