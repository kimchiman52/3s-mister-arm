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

Real usage: the host reads the code aloud or pastes it in chat (11 chars
at the time this was measured; 18 as of v3, §6.3);
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
- `classify_host_datagram` (direct_p2p.c:547) is the single routing
  decision for every inbound datagram on the waiting host's socket:
  '3SXR' frame / STUN Binding Response / **authenticated** punch /
  IGNORE. **Fail closed** — no valid token, no acceptance. The IGNORE
  arm drops the datagram, does not echo, and **keeps waiting**
  (host_tick_receive, direct_p2p.c:2351-2373): the peer slot is never
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
- New cause `CONNECT_FAIL_PUNCH_AUTH` (connect_fail.h:97). It outranks
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
8-byte cookie crosses via a seqlock (`signal_cookie_publish` /
`signal_cookie_snapshot`, direct_p2p.c:355/370) and the main thread also
echoes immediately (`host_handle_challenge`, direct_p2p.c:2272).
`Rendezvous_ParseChallenge` (rendezvous.c:184) validates magic, version,
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

**New cause** `CONNECT_FAIL_COOKIE_REJECTED` (connect_fail.h:90,
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
exists for. A pair that *can* punch never reaches this code, so an
enabled relay costs a connectable pair nothing.

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
2. **The relay leg does not arm until `RACE_RELAY_ARM_MS` (2 500 ms).**
   §7.4 promised that "a pair that can punch never reaches this code, so
   an enabled relay costs a connectable pair nothing". Naive racing
   breaks that promise outright — every pair would request a pool port
   from the 100-port range. 2 500 ms is *exactly* the window the pre-S6
   direct punch had entirely to itself, so nothing that the pre-S6 code
   would have connected at that stage is diverted to a relay; and by then
   the DELIVER candidate has usually been punching for ~2 s as well.
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
  the OK and FAIL lines carry it.
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

Four new cases in `test_bilateral_punch.c` (18-21) and one in
`test_stun_mock.c`, each chosen so a serial cascade **cannot** satisfy it.
Every one was proven red by neutralising the code under test and observing
the failure — not by assertion:

| test | neutralisation | observed red |
|---|---|---|
| 19 `test_race_deliver_overlaps_seed` | the DELIVER endpoint is never armed as a punch candidate | *"test19: no handoff (state=11 seed_arms=2 deliver_arms=0)"* |
| 19 + 18 | **N5**: the legs run serially again (DELIVER candidate deferred until the seed finishes; relay deferred until every punch leg has) | *"test19: handoff took 5226 ms; the DELIVER candidate must be punched CONCURRENTLY ... (expected < 2000 ms)"* and *"test18: full-cascade join took 16219 ms (~8109 ms/attempt), expected < 14000 ms"* |
| 20B `test_race_punch_beats_relay` | the `RACE_RELAY_ARM_MS` delay removed (naive racing) | *"test20B: 1 RELAY_REQ(s) inside a 1500 ms race; the relay leg must not arm before RACE_RELAY_ARM_MS (2500 ms)"* |
| 21 `test_race_not_paired_is_transient` | `NOT_PAIRED` terminal again (the pre-fix rule) | *"test21: no handoff after two NOT_PAIRED refusals ... relay_reqs=2 grants=2 notpaired=2"* |
| `run_punch_leg_offer_test` | the stepper's source-IP gate removed | 4 reds, incl. *"a valid payload from the WRONG source IP confirmed the leg"* and *"a wrong-token punch CONFIRMED the leg (S4a fail-closed broken)"* |

Notes on what each test is *for*:

- **18** is the timing regression net, and its scenario-B bound (14 000 ms)
  sits between the two **measured** numbers in §8.5 — 27% above the S6
  measurement, 28% below the pre-S6 one. Its scenario-A bound is
  deliberately loose; scenario A is a *measurement*, scenario B is the
  assertion that bites. This was found by neutralising: an earlier
  version of the test used only scenario A and stayed green under N5,
  i.e. it could not fail. A test that cannot fail is worse than no test.
- **19**'s assertion is on `do_handoff`'s **own arguments**
  (`DirectP2P_TestHook_LastHandoff`), not on internal state — a race that
  populated `s_work` correctly but never reached the handoff would pass an
  `s_work` assertion and cannot pass this one.
- **20** has two acts because 20A alone does not isolate the arm delay:
  its punch confirms on the first pump, so the "a confirmed punch drops
  the relay leg" rule would keep `relay_reqs` at 0 even with the delay
  removed. 20B uses a punch that never confirms and a race bounded to
  1 500 ms — below `RACE_RELAY_ARM_MS` — so the delay is the only thing
  that can keep the count at zero.
- **`run_punch_leg_offer_test`** covers the one decision the blocking
  driver never makes and the race depends on completely: the race offers
  each non-'3SXR' datagram to every live leg in turn and stops at the
  first that consumes it, so a leg that consumed datagrams which are not
  its own would let one candidate's noise confirm another candidate's leg.

### 8.9 Residuals, stated rather than hidden

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
- **A pair that needs longer than `RACE_RELAY_ARM_MS` to punch may end up
  relayed** where the pre-S6 code would have kept punching for the full
  bilateral window first. The arm delay bounds this to pairs that failed
  to punch for 2.5 s across *both* candidates; a confirmed punch still
  wins if it lands while the relay leg is mid-flight, because the punch
  check runs first in every iteration and tears the relay leg down.
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
| S4 security | **implemented + adversarially reviewed** (see §6; S4a punch auth, S4b room code — now v3, S4c rendezvous return-routability — the last two are breaking wire/format changes, authorized. Review fixes as-built in §6.8) |
| S5 relay for symmetric-NAT pairs | **implemented + adversarially reviewed** (see §7; custom '3SXR' relay on the existing port, NOT coturn. Closes the one §2 matrix cell that could not connect at all. Pure wire EXTENSION — protocol version stays 2. Review fixes as-built in §7.2/§7.3: CRITICAL-1 session-TTL teardown, HIGH-1 pin source binding, HIGH-2 over-budget liveness, MEDIUM-1..5, LOW-1..2) |
| S6 joiner candidate racing | **implemented** (see §8; one interleaved race on the existing worker thread — no new threads, no new locks. Full-cascade worst case measured 9 677 -> 5 114 ms per attempt) |
| S7–S8 | planned above |
