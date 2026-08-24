# Plan — Bilateral Hole Punching Fallback (Option C)

**Status:** Draft plan, awaiting Review-agent sign-off. Do not `/implement` ahead of review.
**Parent branch:** `netplay-direct-only`. Baseline is `mister` (not `main`).
**Pairs with:** [`docs/plan-stun-direct-p2p.md`](plan-stun-direct-p2p.md) (the current direct-P2P orchestrator this plan extends).
**Naming:** Fightcade-style "bilateral hole-punch fallback" for symmetric-NAT pairs. Inside the code we use "rendezvous" for the signaling path and "bilateral punch" for the simultaneous two-sided punch — never "TURN" (that's the next tier, explicitly out of scope).

---

## Preamble

### Goal

Add a hybrid bilateral hole-punching fallback to the existing direct-P2P orchestrator (`src/netplay/direct_p2p.c`). Keep the current room-code / direct-P2P flow as the fast path. When a direct hole-punch attempt fails — the state machine's current `DIRECT_P2P_FAILED_SYMMETRIC` terminal — instead of giving up, **fall back to a lobby-coordinated bilateral simultaneous hole punch**: both peers actively send punches to each other at the same moment, using a small signaling server to exchange `(public_ip, public_port)` tuples. This promotes the host from passive (today: `host_tick_receive` echoes whatever lands) to actively punching outbound toward the joiner's learned endpoint, matching how Fightcade's orchestrator works for symmetric-NAT peers.

### Non-goals (this plan does NOT add)

- TURN / relay traffic. Always direct once punched.
- Full ICE. No STUN/TURN candidate trickling, no Lite mode.
- Encrypted signaling. Rendezvous IDs on the wire are derived from the existing room code; see §Security.
- Port prediction for strict symmetric NATs (sequential or random port-allocation prediction). When bilateral punch fails, we stay at a terminal failure — no guessing.
- 3sxtra lobby integration for matchmaking. The rendezvous server here is endpoint-exchange-only; presence/matchmaking is a separate future track.
- Cross-arch crossplay. Unchanged from `project-netplay-port-strategy.md`.

### Hard requirements

1. **Do not break the direct-P2P fast path.** If the existing 2500ms `Stun_HolePunch` window succeeds, nothing in this plan fires — no rendezvous POST, no bilateral punch, no extra state. Enforced by the state machine: rendezvous is only entered after `Stun_HolePunch` returned false on the joiner side. The host path, which today waits passively, MUST be changed, but the change is gated so "peer shows up within the 2500ms window" still terminates immediately through `host_tick_receive`. Step-level success criterion in Step 4.
2. **Must keep building for MiSTer armv7** via `tools/mister/build-game.sh --flavor telemetry`, and for macOS host via `cmake --build build/host`, both with `-DENABLE_NETPLAY=ON`. Lean deps only. We already link SDL3_net (UDP), miniupnpc, GekkoNet. **We do NOT add libcurl or cJSON** — that dep gate blew 3sxtra's `lobby_server.c` out of scope (see §Decision 1).
3. **Offline / LAN-only must not touch the rendezvous server.** `127.0.0.1` and RFC1918 ranges (`10/8`, `172.16/12`, `192.168/16`, `169.254/16`) skip bilateral-punch entirely. Rendezvous must never be contacted from a LAN-only session. This is enforced at three gates: (a) the CLI/handoff LAN path (`--p2p-remote-ip 192.168.x.x`) never entered the direct-P2P orchestrator in the first place, so already safe; (b) inside the orchestrator we bail out of rendezvous if `s_work.peer_ip` or `s_work.stun.public_ip` is private/loopback; (c) **NAT-hairpin case** — two peers behind the same router report identical public IPs to both sides. At DELIVER-receive time we compare the delivered `peer_ip` to `s_work.stun.public_ip` via `direct_p2p_ip_eq_normalized` (compare normalized — never `strcmp` on possibly-prefixed string forms); if they match, we're on the same LAN-with-broken-hairpin and rendezvous will not help (the router can't loop back the public-IP punch any better than it handled the fast-path punch). Skip the bilateral punch in that case and land on `FAILED_SYMMETRIC` promptly rather than burning the full 11-second budget. On the joiner, the same compare runs after STUN discovery and before the FALLBACK_SIGNALING transition; on the host, it runs at DELIVER receive time. Step-level test in Step 6.
4. **No long-lived daemon on MiSTer.** Signaling is transactional — one UDP round-trip per phase, socket torn down after the bilateral-punch state exits. No keepalives, no SSE, no background thread outliving the orchestrator.
5. **Terminal states must still exist.** If bilateral punch ALSO fails, we land in a new `DIRECT_P2P_FAILED_BILATERAL` terminal. No silent retry loops.
6. **No hooks, no force-pushes, no config mutation** per project AGENTS.md.
7. **Sub-agents MUST `/implement`** each step (enforce-skill-invocation rule) — no shortcutting.

---

## Decisions (the 11 points)

### 1. Signaling transport choice

**Candidates evaluated:**

- **(A) Run our own tiny UDP rendezvous server** on a cheap VPS (Hetzner/Oracle Free Tier). Protocol: 1-packet REGISTER, 1-packet POLL, 1-packet DELIVER. ~150–300 LOC of Node.js on the server (same runtime as 3sxtra's lobby), ~250 LOC of SDL3_net UDP client. No TLS, no auth beyond a shared secret used to rate-limit. No new MiSTer deps.
- **(B) Piggyback on 3sxtra's lobby at `http://152.67.75.184:3000`**. Per `~/.claude/projects/-Users-sb-Developer-3sx-mister/memory/project-netplay-port-strategy.md` (which captured the verification when the upstream checkout was present), 3sxtra's lobby `/presence` endpoint stored `player_id, display_name, region, room_code, connect_to, rtt_ms, connection_type, ft`. It does **not** store `public_ip` or `public_port` directly — their server stores the `room_code` as an opaque string field; the format is not visible in the local checkout. The local `/tmp/3sxtra/` checkout has since been cleared (verified 2026-04-26: zero `.c`/`.h` files anywhere under `/tmp/3sxtra/` — `tools/lobby-server/`, `src/netplay/`, and the entire upstream tree are empty), so this paragraph's claims are sourced from the prior-pass project memory, not from re-readable code. `Stun_EncodeEndpoint` exists in our repo at `src/netplay/stun.c:39` but we cannot confirm upstream uses the same encoding without reverse-engineering or coordinating with upstream. We can't piggyback on the room_code as an endpoint carrier without that confirmation. The prior memory note documents the pair-rejection rule rejected empty `room_code` strings; we'd be coupling on undocumented schema either way.
- **(C) Hybrid: own UDP rendezvous now, 3sxtra-lobby integration as a future layer.** Small today, room to expand.

**Recommendation: (A), scoped to a single UDP endpoint-exchange service, written in Node.js (same runtime as 3sxtra's lobby so future merge is cheap).**

**Rationale:**

- **B is infeasible without heavy deps.** 3sxtra's lobby client requires libcurl + cJSON + HMAC-SHA256 (see `lobby_server.c:19-24, :264`). Our MiSTer armv7 target currently links SDL3_net / miniupnpc / GekkoNet only. Cross-compiling libcurl + OpenSSL for this target is a multi-day side quest and bloats the binary by ~1MB. Rolling our own HTTP-over-TCP client in SDL3_net would be ~500 LOC of date-header + HMAC work we do not want to own.
- **B's API doesn't fit bilateral punch.** Their `/presence` POST is keyed on `player_id` with a string `room_code` that both peers publish. For our bilateral flow we need an atomic **exchange**: "here is my `(ip, port)`; give me my peer's `(ip, port)` the instant they publish theirs." `/presence` + SSE implements this indirectly via match-propose, which would require us to also port the SSE streaming client (`lobby_server.c` ~500 more LOC), register as a player, understand the Glicko-2 state model, and avoid polluting their ranked pool. All for what is a 6-byte UDP round-trip in Option A.
- **B's model mismatch: peers are known.** In 3sxtra's ranked flow the server picks your opponent. In our model, peers already know each other's typed-out room code — they just need the `(ip, port)` that the room code encodes plus the symmetric NAT's extra punch. Option A fits that shape exactly.
- **B would force us to fork the lobby protocol.** If we add `public_ip/public_port` to `/presence` we're modifying 3sxtra's server — the memory note `feedback-releases-are-ours.md` says we don't push there. So we'd be running a fork anyway.
- **A is cheap.** The server is a 150-line Node.js file; we already run Oracle Free Tier–class VPS infra for the existing 3sxtra lobby in the same geography. We can host our rendezvous at a sibling port/subdomain.
- **A keeps the deps gate clean.** Client is pure SDL3_net UDP using the same socket we already own — no new libraries. Same socket means post-exchange the bilateral punch proceeds on the STUN-bound port (preserving the NAT mapping), which is exactly what `Stun_HolePunch` needs.

**Fallback plan if A's VPS is unreachable:** client falls back to today's `DIRECT_P2P_FAILED_SYMMETRIC` behavior unchanged. Player sees "Cannot reach peer. Possible Symmetric NAT." Nothing worse than status quo. A kill-switch config key (`disable-bilateral-punch`) lets us deploy a fix without rebuilding MiSTer clients.

**Future-compat with B:** our rendezvous protocol is deliberately single-purpose (endpoint exchange only). If 3sxtra's author ever adds `public_endpoint` fields to `/presence` and we later port `lobby_server.c` anyway (for full matchmaking per `project-netplay-port-strategy.md`), we can migrate the bilateral-punch backend without changing the game-side state machine — the `disable-bilateral-punch`-family config keys gate the whole code path.

---

### 2. Protocol for endpoint exchange

**Server:** single UDP endpoint listening on port 3478 (same as STUN; memorable and not colliding with our 55438 or RetroArch's 55435). Running at a URL configured via `CFG_KEY_NETPLAY_BILATERAL_SIGNAL_URL` (see §6). Config-default hard-coded.

**Wire format (binary, 4-byte aligned):** all multi-byte integer fields are network byte order (big-endian). Port fields come from `StunResult.public_port` which is host order (`stun.h:15-16`), so the encoder must apply a `htons`-equivalent byte swap when serializing; the decoder applies the reverse when populating host-order fields. IPv4 addresses are sent as 4 raw bytes in network byte order; IPv6 is out of scope (per `reference-mister-network-stack.md` — MiSTer kernel has IPv6 disabled).

```
REGISTER (client -> server), 28 bytes:
  offset  size  field
  0       u32   magic           = 0x33535852  // '3SXR'
  4       u8    version         = 1
  5       u8    type            = 1 (REGISTER)
  6       u16   reserved        = 0
  8       u8[16] session_key                  // derived from room code (see below)
  24      u16   my_public_port (BE)           // host's / joiner's STUN-observed public port
  26      u16   reserved2       = 0
  // NOTE: server reads source IP + source port from the UDP packet,
  // NOT from fields. This guarantees the endpoint the server records
  // is the one that actually reached it (post-NAT). my_public_port
  // is sent anyway as a sanity check; server logs a warning if they
  // differ (indicates hairpin or weird NAT).

DELIVER (server -> client), 32 bytes:
  offset  size  field
  0       u32   magic           = 0x33535852
  4       u8    version         = 1
  5       u8    type            = 2 (DELIVER)
  6       u16   reserved        = 0
  8       u8[16] session_key
  24      u8[4] peer_ip                        // raw IPv4 in network byte order
  28      u16   peer_public_port (BE)
  30      u16   reserved2       = 0

POLL (client -> server), 28 bytes:
  offset  size  field
  0       u32   magic           = 0x33535852
  4       u8    version         = 1
  5       u8    type            = 3 (POLL)
  6       u16   reserved        = 0
  8       u8[16] session_key
  24      u32   reserved3       = 0
```

**Server state:** in-memory `map<session_key, {endpoint_A, endpoint_B, registered_at}>`. No disk. Entry expires 60 seconds after creation.

**Server bind / address parsing:** server binds via `dgram.createSocket('udp4')` so `rinfo.address` is always dotted-quad (never `::ffff:a.b.c.d`). Server parses with `inet_pton(AF_INET, rinfo.address, ...)` (or Node equivalent) to populate the 4-byte `peer_ip` field. Clients reverse via `inet_ntop(AF_INET, ...)` to get a guaranteed dotted-quad string for the hairpin gate.

**Flow:**

1. **Derive session_key.** Both peers independently compute `SHA-256(room_code_payload)[0..16]` where `room_code_payload` is the 6-byte raw payload (`ip_be | public_port_be`) recovered from the 11-char display code. *[Superseded by S4b/v3: the code is 18 chars, the payload is 10 bytes (`ip[4] ‖ port_be[2] ‖ nonce_be[4]`), and the derivation is domain-separated — `SHA-256("3SXR-SK3" ‖ payload10)[0..15]`. See `docs/plan-netplay-connection.md` §6.3. The design intent below — derived, never typed, identical on both sides — is unchanged.]* Host has this from its own STUN discovery; joiner has it from `RoomCode_Decode`. Same input → same key, no extra user-typing, and the key is not transmissible-by-accident as a room code (it's 16 raw bytes, not typeable). **We use the existing in-tree SHA-256 at `src/utils/sha256.{c,h}`** (API: `sha256_init` / `sha256_append` / `sha256_finalize_bytes`), which is already linked against tf-psa-crypto and used today by `src/port/resources.c` for asset hashing. No new SHA implementation to vendor — tf-psa-crypto is already a shipped dep per `CMakeLists.txt:246`.
2. **Host REGISTERs** when `Stun_HolePunch` has NOT yet been attempted by the joiner — specifically, host enters rendezvous immediately after its STUN discovery completes, in parallel with the existing `DIRECT_P2P_HOST_WAITING` echo-loop. REGISTER contains `session_key = SHA256(payload)[0..16]` and `my_public_port = s_work.stun.public_port`. Server records `endpoint_A = <source of this packet>`. Server replies with DELIVER containing `peer_ip/peer_public_port = 0/0` if the other side hasn't registered yet; host POLLs every 500ms for up to 8 s.
3. **Joiner enters rendezvous only after its first direct punch failed.** Today's join path: `Stun_HolePunch(2500ms)` → on failure, `DIRECT_P2P_FAILED_SYMMETRIC`. New path: on that failure, transition to `DIRECT_P2P_FALLBACK_SIGNALING`, REGISTER with its own `session_key` and `my_public_port = s_work.stun.public_port`. Server records `endpoint_B` and replies with DELIVER containing `peer_ip/peer_public_port = endpoint_A`. At the same moment the server pushes an unsolicited DELIVER to the host's registered endpoint with `peer_ip/peer_public_port = endpoint_B`.
4. **On DELIVER with non-zero peer, both sides transition to `DIRECT_P2P_FALLBACK_BILATERAL_PUNCH`** and call `Stun_HolePunch` (unchanged — our existing `Stun_HolePunch` is already bidirectional: sends every 200ms, listens, accepts the echo). New second window: 3000 ms. Host also runs `Stun_HolePunch` now, which wasn't happening before — the old `host_tick_receive` was pure-receive. The punch is against the DELIVER-supplied endpoint.
5. **Retry schedule:**
   - REGISTER: initial send, then resend every 500ms until a DELIVER arrives or the phase budget (8 s) expires. Each resend is cheap (28 bytes) and covers lost packets.
   - POLL: only used if the client already got a "peer not yet registered" DELIVER — the server answers every REGISTER with a DELIVER (empty if peer missing), so POLL is rare. POLL every 500ms for up to the remaining phase budget.
   - Phase budget: 8 s total. Symmetric NAT pairs typically register within a few seconds; if neither peer arrives in 8 s the assumption is the other side isn't actually running bilateral-punch code or is truly unreachable.
6. **Race conditions:**
   - Both peers REGISTER simultaneously: server sees both arrive with the same session_key, fills both slots, sends DELIVERs to both. Safe.
   - One peer REGISTERs before the other even starts STUN discovery: first arrival sits in the map; second arrival fills the other slot and triggers both DELIVERs. Safe.
   - DELIVER to host is lost: host POLLs after 500 ms and gets a fresh DELIVER. Safe.
   - Host already succeeded direct-P2P before joiner failed: host is in `DIRECT_P2P_HANDOFF`, no longer polling. Server expires entry after 60 s. Joiner's REGISTER arrives too late; server replies with a DELIVER whose peer fields are zero (entry expired). Joiner sees "Cannot reach peer"; host is already in-session. This is the correct behavior — once the host is handed off, no amount of signaling fixes that the fast path already won on host's side but not joiner's. If we hit this in the wild we can revisit by extending server entry lifetime or having host REGISTER include a "I'm in session, don't wait for me" bit. Deferred.

**User-typeability:** session_key is derived, not typed. The room code stays 11 chars (unchanged). The rendezvous protocol is invisible to the user. *[Superseded: the room code was later widened to 14 chars (v2) and then 18 chars (v3, `XXXXXX-XXXXXX-XXXXXX`) to carry a 32-bit nonce — see `docs/plan-netplay-connection.md` §6.3/§6.8. Bilateral hole-punching did not cause either bump, and "derived, not typed" still holds.]*

---

### 3. Host-side active punch loop

**Today** (`src/netplay/direct_p2p.c:516-552`): host is passive — `host_tick_receive` does a non-blocking `NET_ReceiveDatagram` each frame waiting for the joiner's `3SX_PUNCH`, echoes it back, and hands off. Host never sends outbound packets until after the first inbound arrives.

**New behavior:**

- Host still runs the passive receive path as the fast path. We do NOT change that — if the joiner has non-symmetric NAT and its punch arrives, we hand off exactly as today.
- **In parallel**, after `DIRECT_P2P_HOST_WAITING` is published, the host starts the rendezvous REGISTER+POLL loop on a *separate thread* (`host_rendezvous_thread_fn`) sharing the same STUN socket. Why a new thread: (a) existing `host_tick_receive` runs on the main thread and must stay non-blocking; a blocking UDP recv on the rendezvous endpoint can't share it cleanly without epoll/select. (b) putting it on the worker thread that publishes `HOST_WAITING` would require redesigning that worker's exit contract (today the worker exits immediately after publishing `HOST_WAITING`). A dedicated thread is simpler.
- **Socket sharing between two readers is the key risk.** `host_tick_receive` does `NET_ReceiveDatagram` on `s_work.stun.socket`; the rendezvous thread and the bilateral-punch thread must never also read that socket concurrently. SDL3_net's `NET_ReceiveDatagram` is not re-entrant-safe on the same socket. **Cross-thread send is also unsafe**: `NET_SendDatagram` and `NET_ReceiveDatagram` both call `PumpDatagramSocket` (`/private/tmp/sdl_net_ref/src/SDL_net.c:2015`), which mutates `sock->pending_output`, `sock->pending_output_len`, `sock->pending_output_allocation` from any thread that calls send or recv, with no socket-level lock. So a rendezvous thread that "only sends" while the main thread "only reads" on the same socket would still race on the unsynchronized `pending_output` queue — corrupting the heap pointer queue, not just losing packets. Cross-thread send/recv on the same SDL3_net socket is therefore unsafe regardless of POSIX UDP socket-level guarantees. **Mitigation: the rendezvous thread does NOT touch the STUN socket at all.** It enqueues onto an SPSC ring `s_rendezvous_send_q`; `DirectP2P_Tick` drains the queue inline at the same point it currently calls `host_tick_receive` — drain first, then receive. Concrete spec:
  - **Slot count:** 8 (covers REGISTER + 1-2 POLLs + safety margin; resend cadence 500ms × 60Hz tick = no realistic depth >2).
  - **Slot type:** `struct { NET_Address* target; uint16_t target_port; uint8_t payload[28]; uint8_t payload_len; }`. The address is owned by the slot — producer MUST `NET_RefAddress` before enqueue, drain MUST `NET_UnrefAddress` after send (or after drop on overflow).
  - **Atomics:** `SDL_AtomicInt s_q_head` (consumer-write, producer-read), `SDL_AtomicInt s_q_tail` (producer-write, consumer-read). Standard SPSC ring. No mutex.
  - **Overflow policy:** producer's enqueue returns false if `(tail+1) % 8 == head`. Producer drops the packet, increments a `s_q_drops` counter for telemetry, logs once per session via `SDL_Log`. Drop is preferred over block: the rendezvous thread is best-effort.
  - **Drain rate:** `DirectP2P_Tick` drains up to 4 slots per tick (60Hz × 4 = 240 sends/s headroom; far above any legitimate rate). **Two phases with distinct socket owners:**
  - **Phase REGISTER/POLL (state = `HOST_WAITING`):** main thread is the sole socket I/O actor. The rendezvous thread enqueues REGISTER/POLL packets onto `s_rendezvous_send_q`; main-thread `DirectP2P_Tick` drains the queue (calling `NET_SendDatagram` itself) and then runs `host_tick_receive` for inbound. The main-thread `host_tick_receive` is extended to recognize incoming packets by shape: if a datagram's `buflen >= 32` AND first 4 bytes are the `'3SXR'` magic, it's a rendezvous DELIVER → hand to a `try_handle_deliver()` helper; otherwise treat as today (3SX_PUNCH echo + handoff). This is a clean 4-byte dispatch on the main thread.
  - **Phase BILATERAL_PUNCH (state = `FALLBACK_BILATERAL_PUNCH`):** socket ownership hands off to `host_bilateral_punch_thread_fn`, which calls `Stun_HolePunch` — that function internally does `NET_ReceiveDatagram` on the socket (see `stun.c:424`). **For the lifetime of `Stun_HolePunch`, the main thread MUST NOT call `host_tick_receive` AND MUST NOT drain `s_rendezvous_send_q`.** `DirectP2P_Tick`'s `FALLBACK_BILATERAL_PUNCH` case therefore does no socket I/O; it only polls the bilateral-punch thread for completion. When the thread exits (success → HANDOFF, failure → FAILED_BILATERAL), socket ownership returns to the main thread (but at that point we're transitioning out of this state either way).
  - **Net effect:** the STUN socket always has exactly one actor (main thread or bilateral-punch thread, never both). Phase transition is the hand-off point. The rule is: all socket I/O — sends and receives — stays on a single, phase-appropriate thread. The rendezvous thread never calls `NET_SendDatagram` or `NET_ReceiveDatagram` directly; it only enqueues.
- **Once DELIVER arrives and host has the joiner's `(peer_ip, peer_port)`**: main thread signals (via atomic) the rendezvous thread to stop REGISTERing, transitions state to `DIRECT_P2P_FALLBACK_BILATERAL_PUNCH` (at which point `DirectP2P_Tick` stops calling `host_tick_receive` per the rule above), and spawns a short-lived `host_bilateral_punch_thread_fn` that runs `Stun_HolePunch(s_work.stun, peer_ip, &peer_port, 3000ms, &s_cancel)`. On thread completion, main-thread `DirectP2P_Tick` observes the result and either runs `do_handoff(1, peer_ip, peer_port)` (success → HANDOFF) or transitions to `DIRECT_P2P_FAILED_BILATERAL`.

**Cadence:**
- REGISTER: first send immediately on thread start, then every 500ms.
- Kill condition: `s_cancel` atomic set, OR main thread observes `DIRECT_P2P_HANDOFF` / `DIRECT_P2P_FALLBACK_BILATERAL_PUNCH` transition.
- Hard budget: 8 s. After that the thread exits and state stays at `DIRECT_P2P_HOST_WAITING` (the passive receive path keeps running in case a late-arriving direct punch still works).

**Interaction with existing `host_tick_receive` echo:**
- A `3SX_PUNCH` still triggers the existing echo + handoff path. If the joiner's direct punch arrives before rendezvous DELIVER, the host short-circuits rendezvous by setting `s_cancel_rendezvous` and letting the thread exit.
- A rendezvous DELIVER never triggers handoff directly — it only gives the host the joiner's endpoint so the bilateral punch can run. The handoff happens only after `Stun_HolePunch` returns success OR a subsequent `3SX_PUNCH` arrives.

**Backoff:** linear 500 ms resend, no exponential. The whole phase is 8 s; exponential doesn't help.

---

### 4. Joiner-side continues to punch — convergence and player-number consistency

**Today** (`src/netplay/direct_p2p.c:372-458`): joiner's worker does STUN discover → `Stun_HolePunch(2500ms)` → on success transition to `HANDOFF`, on failure transition to `FAILED_SYMMETRIC`.

**New behavior:**
- On initial punch failure: transition to `DIRECT_P2P_FALLBACK_SIGNALING`, REGISTER+POLL exactly as host does (same thread model — but joiner's existing worker thread is still alive at this point, so we can run rendezvous inline on that worker without spawning a new thread).
- On DELIVER with non-zero peer: transition to `DIRECT_P2P_FALLBACK_BILATERAL_PUNCH`, update `s_work.peer_ip` / `s_work.peer_public_port` with the DELIVER-supplied tuple (NOT the original room-code-decoded tuple — the DELIVER is authoritative because it reflects the host's actual source IP as seen by the rendezvous server, which may differ from the room code under some NAT topologies), and call `Stun_HolePunch(s_work.stun, peer_ip, &peer_port, 3000ms, &s_cancel)`. On success, transition to `HANDOFF` exactly as today. On failure, transition to `DIRECT_P2P_FAILED_BILATERAL`.

**How the two punches converge:**
- Both peers are now running `Stun_HolePunch` simultaneously. That function already sends `3SX_PUNCH` every 200ms AND listens for a matching packet from the peer. When either side's packet lands on the other side, `Stun_HolePunch` returns true on the receiving side, which then sends 3 follow-up punches to shove the corresponding hole open for the sender side. So within ~one round-trip, both sides return true.
- "Connected" from each side: `Stun_HolePunch` returns true. Same definition as today.

**Player-number consistency:**
- Host is player 1 (`do_handoff(1, ...)` at `direct_p2p.c:550`).
- Joiner is player 2 (`do_handoff(2, ...)` at `direct_p2p.c:558`).
- These come from `Role` (ROLE_HOST / ROLE_JOIN) which is set in `BeginHost`/`BeginJoin`. Bilateral punch doesn't touch Role. **Invariant preserved.**

---

### 5. State machine changes

**Additions to `DirectP2PState` enum in `src/netplay/direct_p2p.h`:**

```c
DIRECT_P2P_FALLBACK_SIGNALING,       // REGISTER/POLL to rendezvous server
DIRECT_P2P_FALLBACK_BILATERAL_PUNCH, // running Stun_HolePunch after DELIVER
DIRECT_P2P_FAILED_BILATERAL,         // bilateral punch timed out — terminal
```

**Transitions:**

```
(host side — gates checked first; default arrow fires if no gate matches)
HOST_WAITING --(DELIVER with peer AND direct_p2p_ip_eq_normalized(peer_ip, stun.public_ip))--> FAILED_SYMMETRIC  [gate: host-side hairpin]
HOST_WAITING --(DELIVER with peer AND direct_p2p_is_lan_peer(peer_ip))--> HOST_WAITING                            [gate: stay; LAN punch is a separate concern]
HOST_WAITING --(rendezvous DNS resolve fails)--> HOST_WAITING                                                     [gate: stay; passive receive still running]
HOST_WAITING --(rendezvous register thread 8s budget expired)--> HOST_WAITING                                     [gate: stay; passive echo still running]
HOST_WAITING --(rendezvous DELIVER with peer)--> FALLBACK_BILATERAL_PUNCH                                         [default]
HOST_WAITING --(peer 3SX_PUNCH arrives)--> HANDOFF                                                                [default; unchanged]

(host side — bilateral-punch terminal; no gates)
FALLBACK_BILATERAL_PUNCH --(Stun_HolePunch succeeds)--> HANDOFF
FALLBACK_BILATERAL_PUNCH --(Stun_HolePunch fails)--> FAILED_BILATERAL

(join side — gates checked first; default arrow fires if no gate matches)
JOIN_PUNCHING --(Stun_HolePunch fails AND kill-switch=true)--> FAILED_SYMMETRIC                                                [gate: bilateral disabled]
JOIN_PUNCHING --(Stun_HolePunch fails AND direct_p2p_is_lan_peer(peer_ip))--> FAILED_SYMMETRIC                                 [gate: LAN]
JOIN_PUNCHING --(Stun_HolePunch fails AND direct_p2p_ip_eq_normalized(peer_ip, stun.public_ip))--> FAILED_SYMMETRIC            [gate: joiner-side hairpin]
JOIN_PUNCHING --(Stun_HolePunch fails)--> FALLBACK_SIGNALING                                                                   [default; was FAILED_SYMMETRIC]

(join side — fallback-signaling and bilateral-punch terminals; no further gates)
FALLBACK_SIGNALING --(rendezvous DNS resolve fails)--> FAILED_BILATERAL
FALLBACK_SIGNALING --(REGISTER never answered after 8s)--> FAILED_BILATERAL
FALLBACK_SIGNALING --(DELIVER with peer)--> FALLBACK_BILATERAL_PUNCH
FALLBACK_BILATERAL_PUNCH --(Stun_HolePunch succeeds)--> HANDOFF
FALLBACK_BILATERAL_PUNCH --(Stun_HolePunch fails)--> FAILED_BILATERAL
```

**What happens to `DIRECT_P2P_FAILED_SYMMETRIC`?**
The joiner's path previously landed there on punch failure; now it lands on `FALLBACK_SIGNALING` instead. **But `FAILED_SYMMETRIC` is NOT renamed/removed** — it's still a valid terminal when the user has the kill-switch config key set (bilateral disabled) OR when rendezvous is unreachable (REGISTER can't resolve the server / all sends fail). Rule of thumb: `FAILED_SYMMETRIC` means "direct punch failed and we chose not to try bilateral." `FAILED_BILATERAL` means "we tried bilateral and that also failed."

Rename is tempting for clarity but blocks graceful degradation: a joiner with the kill-switch enabled (or an older build) still wants to show a meaningful terminal. Keep both.

**`FALLBACK_SIGNALING` is joiner-only.** The host's REGISTER/POLL phase happens while the state remains `HOST_WAITING` — the rendezvous thread runs in parallel; the main thread continues receiving via `host_tick_receive`. The host transitions directly from `HOST_WAITING` to `FALLBACK_BILATERAL_PUNCH` on DELIVER. There is no host-side `set_state(FALLBACK_SIGNALING)` call. The diagram below reflects this.

---

### 6. Config keys

All new keys follow the existing `CFG_KEY_NETPLAY_DIRECT_P2P_*` and `netplay-direct-p2p-*` convention (see `src/port/config/config.h:29-33`).

```c
// src/port/config/config.h
#define CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL \
    "netplay-direct-p2p-disable-bilateral"
#define CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL \
    "netplay-direct-p2p-signal-url"
#define CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_BUDGET_MS \
    "netplay-direct-p2p-signal-budget-ms"
#define CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS \
    "netplay-direct-p2p-bilateral-punch-ms"
```

**Defaults** (registered in `default_entries[]` in `src/port/config/config.c`):

| Key | Type | Default | Purpose |
|-----|------|---------|---------|
| `netplay-direct-p2p-disable-bilateral` | BOOL | `false` | Kill switch — forces direct-only behavior (today's `FAILED_SYMMETRIC` path). Use when rendezvous server is down or for testing. |
| `netplay-direct-p2p-signal-url` | STRING | `"udp://rendezvous.3s-arm.example:3478"` | Rendezvous server URL. UDP-only; `udp://host:port` form. Placeholder hostname — replaced once infrastructure stands up (see Step 1). |
| `netplay-direct-p2p-signal-budget-ms` | INT | `8000` | Total REGISTER/POLL phase budget. |
| `netplay-direct-p2p-bilateral-punch-ms` | INT | `3000` | `Stun_HolePunch` window for bilateral phase. Longer than direct-P2P's 2500 because bilateral starts after one side's NAT mapping may already be stale. |

**Help text** (`docs/config.md` update — a single subsection under Netplay):

```
netplay-direct-p2p-disable-bilateral (default: false)
  If true, disables the bilateral hole-punch fallback. Direct-P2P will
  terminate with "Cannot reach peer. Possible Symmetric NAT." on punch
  timeout, matching behavior before the bilateral feature landed.

netplay-direct-p2p-signal-url (default: udp://rendezvous.3s-arm.example:3478)
  Rendezvous server URL for bilateral hole-punch endpoint exchange.
  Only used when the direct punch fails; never contacted on LAN sessions.
  UDP scheme only.

netplay-direct-p2p-signal-budget-ms (default: 8000)
  Maximum wall-clock time spent waiting for the rendezvous server to
  pair us with our peer before giving up and reporting bilateral failure.

netplay-direct-p2p-bilateral-punch-ms (default: 3000)
  Length of the bilateral Stun_HolePunch window, in milliseconds.
  Longer than the initial direct punch window to accommodate post-
  signaling clock skew between peers.
```

Kill switch confirmed explicitly in the list.

---

### 7. Testing strategy

**New test TU:** `src/netplay/test_bilateral_punch.c`. Gated identically to `test_stun_mock.c` (`ENABLE_NETPLAY_TESTS`), registered in `default_entries` comment + `src/args.c`'s test flag registry + `src/main.c`'s dispatch. CLI flag: `--test-bilateral-punch`.

**Tests (in-process, ~2 seconds total):**

1. **Rendezvous wire-protocol round-trip.** Mirror `test_stun_mock.c`'s pattern: spin up a mock rendezvous server on a localhost UDP socket in a helper thread. Client side does REGISTER → expect DELIVER with zero peer → mock gets a second REGISTER from a different localhost port with the same session_key → mock emits DELIVER with peer filled. Assert both clients receive the other's endpoint in the DELIVER. Verifies: magic bytes, version, session-key routing, source-IP-from-packet logic.
2. **Session-key derivation stability.** Given the same room-code payload, SHA-256[0..16] must be identical across runs and across peers. Compare against a known-value vector.
3. **LAN bypass rejection.** `direct_p2p_is_lan_peer("127.0.0.1")` → true. Same for `10.0.0.1`, `172.16.0.1`, `192.168.1.1`, `169.254.1.1`. Same for `8.8.8.8` → false. Ensures rendezvous gate works.
4. **State-machine timing.** Drive `direct_p2p.c`'s state machine with a mock `Stun_HolePunch` that always fails on the first call and succeeds on the second. Assert: joiner transitions JOIN_PUNCHING → FALLBACK_SIGNALING → FALLBACK_BILATERAL_PUNCH → HANDOFF. This will need some test-only hooks; prefer lightweight function pointer injection via a `#ifdef NETPLAY_TEST_HOOKS` seam rather than rewriting the module.
5. **Kill-switch honored.** With `disable-bilateral` true, joiner lands on `FAILED_SYMMETRIC` unchanged. No rendezvous traffic emitted (verify by mock server receiving zero packets).
6. **DNS-fail fast-fail.** Configure `signal-url=udp://invalid.example:3478`; assert state lands on `FAILED_BILATERAL` within 1 second on the joiner (host stays HOST_WAITING per §Step 5b). Verifies the rendezvous hostname-resolve happens once and bails immediately on failure rather than burning the 8-second budget.

**What requires humans:** actual bilateral punch over two separate symmetric NATs. That's a two-house test that no CI can do; documented in `docs/direct-p2p-smoke-plan.md` as a new section ("Bilateral smoke — two-home test"). Humans run it by: (a) both peers boot with `disable-bilateral=false` and a real rendezvous URL; (b) at least one peer is verifiably behind symmetric NAT (existing direct-P2P smoke plan documents how to verify); (c) observe `FALLBACK_BILATERAL_PUNCH` → `HANDOFF` on both sides via the overlay. Smoke-plan writeup is part of Step 7.

**CI vs. human split:**
- CI (runs on push to `netplay-direct-only`): `--test-bilateral-punch` covers tests 1–6 above. Under 3 seconds total. No net.
- Human (before merging to `mister`): two-home smoke per docs/direct-p2p-smoke-plan.md. Already an existing gating for direct-P2P; we extend it.

---

### 8. Security

**Threat model assumption:** adversary can read traffic to/from the rendezvous server (no TLS on MVP), can guess or brute-force session_keys, can DoS either a player or the server. This is casual fighting-game netplay, not banking.

> *[Superseded in part by S4 — read `docs/plan-netplay-connection.md` §6.3, §6.4 and §6.8 for the as-built security posture. Three of this section's premises no longer hold: (a) the room code is 18 chars and its payload is 10 bytes including a 32-bit CSPRNG nonce, so the derivation input is 80 bits and is no longer reconstructable from an observable `(ip, port)` pair; (b) both derivations are domain-separated (`"3SXR-SK3"` / `"3SXR-PT3"`) rather than a bare SHA-256 over the payload; (c) the server now requires a return-routability cookie before it binds anything, so a sender that merely knows a session key still cannot squat a slot from a spoofed address. The paragraphs below are kept as the record of the MVP threat model as it stood.]*

**Rendezvous-key leakage.** Anyone who knows a player's 11-char room code can derive the session_key (SHA-256 of the 6-byte payload is public) and REGISTER as if they're a peer. Consequences:
- If attacker REGISTERs first, server records attacker's endpoint as `endpoint_A`. Real host REGISTERs, gets told the peer is at `attacker_ip`. Host then tries to `Stun_HolePunch` against attacker. Attacker's NAT either drops it (nothing happens) or attacker runs a matching punch loop (harmless: attacker speaks `3SX_PUNCH` and suddenly has a GekkoNet session with the host — attacker can desync the game and make it crash).
- **Mitigation: the room code is already a shared secret shown only to the intended peer.** If someone else has it, they have it — the same "attack" works on the direct-P2P fast path today (impersonate the joiner by punching first). Bilateral doesn't make this worse.
- Not-mitigated: someone who sniffs the REGISTER packet on the way to the server gets the session_key and can hijack from another network. Acceptable for casual FG netplay. **Document this in `docs/plan-bilateral-hole-punch.md` §Security (this section).** If someone wants to secure it later they can add an HMAC over the room code + registration timestamp using a server-side shared secret; out of scope now.

**Collision probability.** The input space is 48 bits (4-byte IPv4 + 2-byte port), so two distinct rooms can only collide at the session-key level if they share the same `(ip, port)` tuple — which is the problem the room code itself already has. The 128-bit truncation is only to make the key non-typeable; it does NOT add entropy beyond the 48-bit input. Non-issue for collisions between legitimate sessions (two peers with the same public IP+port would already collide at the room-code layer).

**Key expiry.** Server entries expire 60 s after `last_touch` (refresh on any REGISTER/POLL for that key). A host that sits on `HOST_WAITING` for 10 minutes sees its entry evicted periodically; rendezvous thread keeps refreshing while it runs, but the thread itself exits after 8 s. If no joiner arrives in 8 s the entry decays naturally. This bounds the server's live-entry count by `requests-per-minute * 60 / 8s * 2 peers` — a single server machine easily handles 10k concurrent sessions under this model.

**Rate limits.** Server enforces per-source-IP: max 10 REGISTER/POLL packets per second. Implemented as a sliding-window counter in the Node.js server. Exceeded packets are silently dropped (no ICMP, no reply) so attackers can't amplify.

**DoS exposure.**
- Against the server: a small VPS with a $5 bandwidth budget handles our whole user base. Attacker wanting to flood the server hurts nobody but us — we take it offline for an hour, clients gracefully degrade to `FAILED_SYMMETRIC`. Kill switch in the config lets us tell users to disable signaling entirely if needed.
- Against a player: attacker who knows the room code can trick the target into sending `3SX_PUNCH` toward an arbitrary IP. This is UDP reflection at a volume capped by `Stun_HolePunch`'s 200ms interval for 3000 ms = 15 packets × 9 bytes = 135 bytes. Uninteresting to any real attacker; well below residential uplink.

**Threat model verdict:** Acceptable for casual FG netplay. Called out explicitly in §Security of this plan; review-agent should reject the plan if it disagrees.

---

### 9. UX

**Status strings (rendered by `DirectP2P_GetStatusText` through `src/port/sdl/netplay_screen.c` → `DirectP2P_DrawOverlay`):**

| State | Status text |
|-------|-------------|
| `DIRECT_P2P_HOST_WAITING` | (unchanged) `"Waiting for peer..."` |
| `DIRECT_P2P_HOST_WAITING` (after rendezvous-thread budget expires with no DELIVER) | `"Waiting for peer (no peer detected - check that they're using a recent build)."` |
| `DIRECT_P2P_JOIN_PUNCHING` | (unchanged) `"Connecting to peer..."` |
| `DIRECT_P2P_FALLBACK_SIGNALING` | `"Symmetric NAT - coordinating via rendezvous..."` |
| `DIRECT_P2P_FALLBACK_BILATERAL_PUNCH` | `"Symmetric NAT - simultaneous hole punch..."` |
| `DIRECT_P2P_FAILED_SYMMETRIC` | (unchanged) `"Cannot reach peer. Possible Symmetric NAT."` |
| `DIRECT_P2P_FAILED_BILATERAL` | `"Could not reach peer after fallback. Try another network."` |

**Mode labels (line 1 in `DirectP2P_DrawOverlay`):**
- Line 1 already displays `HOSTING / CONNECTING / CONNECTED / ERROR` per `direct_p2p_overlay.c`. The mapping branches on the active `Role` (the existing role field set in `BeginHost`/`BeginJoin`) so the host's overlay never flips from `HOSTING` → `CONNECTING` mid-session. `s_work` is `static` to `direct_p2p.c` (`direct_p2p.c:115`), so `direct_p2p_overlay.c` cannot read `s_work.role` directly. We expose a tiny accessor `Role DirectP2P_GetRole(void)` from `src/netplay/direct_p2p.h` that returns `s_work.role` as a snapshot value (no atomic needed; role is set once per `BeginHost`/`BeginJoin` at `direct_p2p.c:581` and `:633` and not mutated thereafter). `dp2p_overlay_mode_label` in `direct_p2p_overlay.c:45-67` calls `DirectP2P_GetRole()` to branch the mode label per role per the table below.
  - **If role is `ROLE_HOST`:** `HOST_WAITING / FALLBACK_SIGNALING / FALLBACK_BILATERAL_PUNCH` all map to `"HOSTING"`. `HANDOFF` maps to `"CONNECTED"`. `FAILED_*` (including `FAILED_BILATERAL`) map to `"ERROR"`.
  - **If role is `ROLE_JOIN`:** `JOIN_PUNCHING / FALLBACK_SIGNALING / FALLBACK_BILATERAL_PUNCH` all map to `"CONNECTING"`. `HANDOFF` maps to `"CONNECTED"`. `FAILED_*` (including `FAILED_BILATERAL`) map to `"ERROR"`.
- The Status text strings (line 2) stay the same — those are fine to be NAT-aware regardless of role. No new mode labels are introduced.

**Rationale for copy:** users who hit symmetric NAT know what it means (the word appeared in the old failure message). Leaving "Symmetric NAT" in the fallback status tells them the fallback engaged and why. Not a beginner-facing term, but beginners would have failed anyway under the old flow. Netplay screen (`src/port/sdl/netplay_screen.c`) needs no changes beyond what `DirectP2P_DrawOverlay` consumes — overlay reads from `DirectP2P_GetStatusText()`, unchanged.

**Glyph constraint — ASCII only.** The native game text path (`SSPutStrPro`) renders SF3's glyph table; em-dashes (U+2014) and other non-ASCII characters render as missing-glyph boxes. Existing ASCII-only status strings at `direct_p2p.c:425, :434` set the precedent. **All status strings emitted by the bilateral feature MUST use ASCII hyphen (`-`), not em-dash (`—`).** This applies to every string passed to `set_status()` or any other text-emit path throughout the new code.

**Host code (line 2) stays as-is** — displayed during HOST_WAITING and invisible during fallback phases (already the behavior: `DirectP2P_GetHostCode()` returns `""` outside `HOST_WAITING`).

---

### 10. Rollout / migration

**Scenario matrix:**

| Host version | Joiner version | Host symmetric? | Joiner symmetric? | Outcome |
|--------------|---------------|----------------|-------------------|---------|
| New | New | No | No | Fast path succeeds (unchanged) |
| New | New | Yes | No | Joiner fails direct punch → FALLBACK_SIGNALING; host doesn't see joiner's punch because joiner's NAT ate it → host's rendezvous thread posts REGISTER, server pairs endpoints, both do bilateral punch, succeed. |
| New | New | Yes | Yes | Both fail direct; both fall back; server pairs; bilateral succeeds (success rate depends on how tightly symmetric the NATs are — "full cone" vs "port-restricted" vs "strict port-allocation." Document that we can't fix the strict-port-allocation case; that's where TURN begins.) |
| New | Old | Yes | Any | Old joiner doesn't speak REGISTER and doesn't fall back — lands on FAILED_SYMMETRIC. New host's rendezvous thread REGISTERs but never gets a peer; after 8 s rendezvous exits, host stays on HOST_WAITING forever until user cancels. Graceful: no incorrect state. **UX gap** — host doesn't know the joiner gave up. Acceptable for MVP, documented. |
| Old | New | Yes | Yes | New joiner falls back, REGISTERs; old host doesn't REGISTER, so server waits 60 s then evicts joiner. Joiner lands on FAILED_BILATERAL after 8 s. Correct graceful-degradation. |
| Old | Old | Any | Any | Pre-bilateral behavior. Unchanged. |

**Compat handshake?** Not needed — the bilateral path is invisible to an old peer. Old code doesn't understand the new protocol and doesn't care. New code doesn't need to detect old peers because the old peer's failure case is exactly the same as "peer doesn't exist."

**What "graceful degradation" means here:** a session that would have succeeded under the old code keeps succeeding. A session that would have failed under the old code either succeeds (if the new fallback works) or fails in the same user-facing way (if bilateral times out). No session that previously worked gets worse.

**Deployment order:**
- Server (Step 1) before clients (Step 3+). A new client hitting a stale/missing server just lands on FAILED_BILATERAL — no worse than today's FAILED_SYMMETRIC.
- Kill-switch config key defaults to false but we can flip it remotely via the docs if the server melts down — users edit `config.ini` and restart. Not ideal; out of scope to wire a remote-config mechanism.

---

### 11. Scope boundaries — out of scope explicit list

For future-agent clarity, the following are **NOT** covered by this plan and should not be added by a drifting `/implement` step:

- TURN / relay traffic. Full stop. Fightcade uses TURN for true-symmetric pairs; we're letting those pairs fail for now. That's tier 3 (future).
- Full ICE trickling, ICE Lite, or candidate prioritization. We have one candidate per peer and send it.
- Encrypted signaling (DTLS, QUIC, anything over TLS). Rendezvous traffic is plaintext UDP.
- Port prediction for symmetric NATs that allocate sequential or random source ports. Beyond scope; belongs alongside TURN.
- Matchmaking / lobby integration (3sxtra shared lobby). Covered in a separate track (`project-netplay-port-strategy.md`). This plan does not touch matchmaking.
- Cross-arch crossplay. Blocked upstream by `SessionHealthMsg` checksum; out of scope.
- Per-match disconnect reporting, Glicko-2, leaderboards. Not relevant to endpoint exchange.
- RmlUi UI changes for bilateral-punch status. We render through the existing `DirectP2P_DrawOverlay` 384x224 canvas path; no RmlUi required.
- Changes to `src/port/sdl/netplay_screen.c`. It reads `DirectP2P_GetStatusText()` unchanged.
- UPnP changes. Still happens on the host side pre-STUN; unchanged.
- Signaling rate-limit bypass tokens, captcha, or any registration flow. The session_key is our only auth.
- Server-side match history or persistent entry logging. Map is in-memory; restarting the server drops state with no user impact.
- Telemetry reporting back to us. No analytics.
- The wrapper OSD. Wrapper handoff writes a room code for Host or reads one for Join; the new protocol is all game-side and doesn't require wrapper changes.

---

## Phases — each step independently `/implement`-able

Ordered by dependency. Steps 1 and 2 are deployable independently (infra + a new config key). Steps 3–5 are the core client work. Steps 6–8 round out tests, docs, and smoke.

---

### Step 1 — Stand up the rendezvous server **[DONE 2026-04-26]**

**Why:** the server must exist before any client code can be tested end-to-end. A single-host Node.js service is the cheapest path and matches the runtime used by 3sxtra (we do NOT ship our own lobby server in this repo — see `project-netplay-port-strategy.md`; we piggyback on 3sxtra's hosted lobby for matchmaking). This step introduces a NEW top-level tooling directory `tools/rendezvous-server/`.

**Read first:**
- `/Users/sb/Developer/3sx-mister/docs/plan-netplay-port.md` — §8 for the general Node-service deployment recipe (matchmaking lobby, not bilateral — just shape)
- `/Users/sb/Developer/3sx-mister/docs/plan-bilateral-hole-punch.md` §Decision 2 (wire format)
- No Node.js reference is available locally (the upstream `tools/lobby-server/` directory in `/tmp/3sxtra/` is empty as of 2026-04-26 — `ls /tmp/3sxtra/tools/lobby-server/` returns no files). Design from §Decision 2's wire format directly. For systemd unit and `deploy.sh` idioms, use any standard reference (e.g., the systemd `man systemd.service` documentation).

**Changes:**
- Create `tools/rendezvous-server/rendezvous-server.js` — Node.js UDP server implementing the REGISTER/POLL/DELIVER protocol in §Decision 2. Bind via `dgram.createSocket('udp4')` so `rinfo.address` is always dotted-quad. In-memory `Map<session_key, {endpoint_a, endpoint_b, last_touch}>`; 60 s TTL sweep every 5 s. Per-source-IP rate limit (10 pkt/s, sliding-window counter; entries swept every 60s, same TTL as the session map — otherwise per-IP counters leak forever and a /16 scan fills RAM).
- Create `tools/rendezvous-server/package.json` — single dep: `node`. No npm deps. Compatible with Node 18+.
- Create `tools/rendezvous-server/README.md` — how to run, how to deploy to a cheap VPS.
- Create `tools/rendezvous-server/deploy.sh` — minimal: rsync the `.js` file + `.service` unit to the VPS, `systemctl daemon-reload && systemctl restart rendezvous-server`.
- Create `tools/rendezvous-server/rendezvous-server.service` — systemd unit. Concrete spec:
  ```
  [Unit]
  Description=3SX rendezvous server
  After=network-online.target
  Wants=network-online.target

  [Service]
  ExecStart=/usr/bin/node /opt/rendezvous-server/rendezvous-server.js 3478
  Restart=always
  User=rendezvous
  NoNewPrivileges=true
  PrivateTmp=true
  ProtectSystem=strict

  [Install]
  WantedBy=multi-user.target
  ```
  Without `[Install]` the deploy script's `systemctl enable` fails.
- Create `tools/rendezvous-server/__test_protocol.js` — pure Node test: drive REGISTER/POLL/DELIVER via two mock UDP clients. Run with `node __test_protocol.js`.

**Success criteria:**
- `node tools/rendezvous-server/__test_protocol.js` exits 0 with "protocol test passed".
- `tools/rendezvous-server/rendezvous-server.js` runs locally (`node rendezvous-server.js 3478`) and answers a manually-sent REGISTER packet with a DELIVER.
- No external deps beyond Node 18 stdlib (no npm install required).

**Depends on:** none.

**Do NOT:**
- Add matchmaking, presence-with-`display_name`, SSE, HTTP, auth tokens, or any 3sxtra-lobby-like surface. This server does one thing.
- Vendor or import any 3sxtra lobby server code. We do not ship our own lobby (see `project-netplay-port-strategy.md`); the rendezvous server is purely a transport-layer endpoint exchange.

**If it fails:**
- Node version: check `node --version` is 18+.
- Port binding: 3478 may be in use on the local dev box; test with port 0 and log the chosen port.
- Rate limiter false-positives in tests: verify the limiter uses monotonic time and resets per-IP counter not per-key.

---

### Step 2 — Config keys + signal URL wiring **[DONE 2026-04-26]**

**Why:** the client reads the signaling URL and kill-switch from config. Defaults must land before Step 3's rendezvous code is written so the client can `Config_GetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL)` on day one. Also provides the kill-switch that gracefully degrades the whole feature.

**Read first:**
- `/Users/sb/Developer/3sx-mister/src/port/config/config.h` — existing `CFG_KEY_NETPLAY_DIRECT_P2P_*` macros
- `/Users/sb/Developer/3sx-mister/src/port/config/config.c` — `default_entries[]`
- `/Users/sb/Developer/3sx-mister/docs/config.md` — doc convention

**Changes:**
- `src/port/config/config.h` — add four new macros per §Decision 6.
- `src/port/config/config.c` — add four new entries to `default_entries[]`. Default for `signal-url` is a sentinel hostname (`udp://rendezvous.3s-arm.example:3478`) — documented as to-be-replaced before first real user test.
- `docs/config.md` — extend the Netplay section with the four new keys + descriptions from §Decision 6.

**Success criteria:**
- `tools/mister/build-game.sh --flavor telemetry` builds clean.
- `grep -n 'netplay-direct-p2p-disable-bilateral\|netplay-direct-p2p-signal-url\|netplay-direct-p2p-signal-budget-ms\|netplay-direct-p2p-bilateral-punch-ms' src/port/config/config.{c,h}` prints 8+ lines.
- `docs/config.md` has all four keys documented.

**Depends on:** none.

**Do NOT:**
- Modify `src/args.c` — no new CLI flags in this step. If someone wants `--disable-bilateral-punch`, defer to a later step or just set the config key.
- Touch any `src/netplay/` file — this is a pure config step.

**If it fails:**
- If `Config_Init()` log prints `entries table full`, `CONFIG_ENTRIES_MAX` (128) is close to its limit. Count current entries; if we're at 127, bump the limit by 16 in the same step.

---

### Step 3 — Rendezvous client (pure, no state-machine integration) **[DONE 2026-04-26]**

**Why:** build and unit-test the rendezvous protocol client in isolation before wiring it into `direct_p2p.c`. Keeps Steps 5a/5b/5c small and gives us a test harness early.

**Read first:**
- `/Users/sb/Developer/3sx-mister/src/netplay/stun.c` — the socket bind / resolve / send pattern on SDL3_net
- `/Users/sb/Developer/3sx-mister/src/netplay/net_tuning.h` — recvbuf sizing convention
- `/Users/sb/Developer/3sx-mister/src/utils/sha256.h` — in-tree SHA-256 API used for session-key derivation
- `/Users/sb/Developer/3sx-mister/src/port/resources.c` lines 110-140 — reference usage pattern for `sha256_init` / `sha256_append` / `sha256_finalize_*`
- `/Users/sb/Developer/3sx-mister/docs/plan-bilateral-hole-punch.md` §Decision 2

**Changes:**
- Create `src/netplay/rendezvous.c` and `src/netplay/rendezvous.h` — new module:
  - `bool Rendezvous_DeriveSessionKey(uint32_t ip_be, uint16_t public_port, uint8_t out_key[16])` — pack semantics: internally pack `payload[0..3] = ip_be` (already in network byte order; copy bytes verbatim) and `payload[4..5] = htons(public_port)` (input is host order). Then SHA-256 the 6-byte buffer using `src/utils/sha256.h`: `sha256_init(&sha); sha256_append(&sha, payload, 6); uint8_t digest[32]; sha256_finalize_bytes(&sha, digest); memcpy(out_key, digest, 16);`. Returns `false` if `ip_be == 0` (caller must check). No new SHA implementation — we link the existing in-tree tf-psa-crypto-backed module that `src/port/resources.c` already uses.
  - **Caller responsibilities.** Host derives `ip_be` via `ipv4_str_to_be(s_work.stun.public_ip)` (already exists at `direct_p2p.c:276-283`); host MUST abort REGISTER and skip rendezvous entirely if `ip_be == 0` (STUN didn't yield a routable public IPv4). Otherwise all peers offline collide on `SHA-256(0x00000000:port)` — a real-world denial-of-service / cross-talk hazard. Joiner gets `(ip_be, public_port)` directly from `RoomCode_Decode` (`room_code.h:204`); the room code's payload is already the same raw bytes the derivation hashes. **Corrected for the as-built code:** `ROOM_CODE_RAW_LEN` no longer exists and the payload is **10 bytes**, not 6 — `ip[4] ‖ port_be[2] ‖ nonce_be[4]`, packed by `room_code_pack_payload` (`room_code.c:251`) and mirrored as `REND_KEY_PAYLOAD_LEN 10` (`rendezvous.c:48`). `RoomCode_Decode` now also yields a `nonce` out-param and returns a `RoomCodeDecodeResult` enum rather than a bool. See `docs/plan-netplay-connection.md` §6.3.
  - `bool Rendezvous_BuildRegister(uint16_t my_public_port, const uint8_t session_key[16], uint8_t out_pkt[28])` — wire format encoder for the 28-byte REGISTER packet. `my_public_port` is host-order on input; encoder emits it in network byte order (see §Decision 2) with an explicit `htons`-equivalent shift.
  - `bool Rendezvous_BuildPoll(const uint8_t session_key[16], uint8_t out_pkt[28])` — 28-byte POLL packet.
  - `bool Rendezvous_ParseDeliver(const uint8_t* pkt, int len, const uint8_t expected_session_key[16], char out_peer_ip[64], uint16_t* out_peer_port)` — validates `len >= 32` + magic + version + key-match, writes peer tuple (`peer_ip` decoded from the 4-byte raw IPv4 field via `inet_ntop(AF_INET, ...)` for guaranteed dotted-quad form; port decoded from network to host order); returns false if peer fields are zero (meaning "server has nothing yet").
  - `bool Rendezvous_ParseSignalUrl(const char* url, char out_host[64], uint16_t* out_port)` — parses `udp://host:port` (reject anything else; error if no scheme, non-udp scheme, missing port).
  - `Rendezvous_Send` (the production send wrapper) is declared as a `static` helper in `direct_p2p.c` per the SDL_net-purity constraint; introduced in Step 5a.
  - Pure functions except for the SHA call, which transitively uses tf-psa-crypto via `sha256.h`. No SDL_net dependencies — for unit-testing. Actual network I/O stays in `direct_p2p.c`.
- Update `CMakeLists.txt` — no change needed; glob picks up new `src/netplay/*.c` files automatically. tf-psa-crypto path is declared at `CMakeLists.txt:246`; the actual link line is at `CMakeLists.txt:278` (`target_link_libraries(... "${TF_PSA_CRYPTO_ROOT}/lib/libtfpsacrypto.a")`), so no new link dependency.

**Success criteria:**
- `tools/mister/build-game.sh --flavor telemetry` builds clean.
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DNETPLAY_TEST_HOOKS=ON -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS" tools/mister/build-game.sh --flavor telemetry && build/mister-telemetry-install/bin/3s-arm --test-bilateral-punch` — but we haven't added the test harness yet. For this step the only build-visible check is the targeted grep and a clean build. (**Both** flags are required and they are spelled differently: `NETPLAY_TEST_HOOKS` is a real CMake `option()` at `CMakeLists.txt:47`, while `ENABLE_NETPLAY_TESTS` is not an option at all and only reaches the compiler through `CMAKE_C_FLAGS`. Omitting `-DNETPLAY_TEST_HOOKS=ON` configures fine and then fails the build with ~20 compile errors in `test_bilateral_punch.c`.)
- `grep -n 'Rendezvous_DeriveSessionKey\|Rendezvous_BuildRegister\|Rendezvous_ParseDeliver' src/netplay/rendezvous.h` prints 3+ lines.
- `grep -n 'utils/sha256.h' src/netplay/rendezvous.c` prints 1 line (confirm we pulled in the in-tree module).
- Header-only static check: `#include "netplay/rendezvous.h"` from a scratch `.c` file compiles.

**Depends on:** none. Step 5c will consume the config key declared in Step 2, but Step 3's deliverable (the pure `rendezvous.{c,h}` module) does not import from `config.h` — `Rendezvous_ParseSignalUrl` takes the URL as a `const char*` argument.

**Do NOT:**
- Add any `SDL_net` calls from inside `rendezvous.c`. Keep it pure (bytes in, bytes out, plus a URL parser). Networking lives in Steps 5a/5b/5c's state-machine integration.
- Integrate with `direct_p2p.c` — that's Steps 5a/5b/5c.
- Create a new `src/netplay/sha256.{c,h}` or vendor one from `/tmp/3sxtra/`. The in-tree `src/utils/sha256.{c,h}` (tf-psa-crypto) is the only SHA we link.
- Import any 3sxtra file. No `identity.c`, no `lobby_server.c`, no `discovery.c`, no `sha256.{c,h}`.

**If it fails:**
- If `#include "utils/sha256.h"` fails to resolve, confirm the rendezvous translation unit has `src/` as an include root (it does — matches `src/port/resources.c:3`'s `#include "utils/sha256.h"`).
- URL-parse edge cases: add tests for missing port, missing scheme, IPv6 literals in brackets (out of scope — reject with clear error).

---

### Step 4 — Harden the host passive-receive for rendezvous DELIVERs (no bilateral yet) **[DONE 2026-04-26]**

**Why:** before spawning rendezvous threads, extend `host_tick_receive` to recognize rendezvous-shaped packets on the STUN socket without disturbing the `3SX_PUNCH` fast path. Split in its own step so a reviewer can confirm the fast path still works before any new threads appear.

**Read first:**
- `/Users/sb/Developer/3sx-mister/src/netplay/direct_p2p.c` — `host_tick_receive` at line 516
- `/Users/sb/Developer/3sx-mister/src/netplay/rendezvous.h` — from Step 3

**Changes:**
- Modify `src/netplay/direct_p2p.c`:
  - `host_tick_receive` — after reading a datagram, examine it:
    - If `buflen >= 32` AND first 4 bytes == `'3SXR'` magic: hand to `try_handle_deliver()` (stub for now — just log and discard; full handling comes in Step 5b). Minimum 32 matches DELIVER's wire size per §Decision 2; hosts never receive REGISTER (28B) or POLL (28B) packets — those are client→server only.
    - Otherwise: today's `3SX_PUNCH` echo + handoff path, unchanged.
  - Add `static bool try_handle_deliver(const uint8_t* pkt, int len)` stub that logs the magic/version/session-key match and returns true. Do NOT yet transition state or cancel rendezvous thread — this step only proves we can dispatch the packet type.

**Success criteria:**
- `tools/mister/build-game.sh --flavor telemetry` builds clean.
- `grep -n 'try_handle_deliver\|0x33535852' src/netplay/direct_p2p.c` prints ≥ 2 lines.
- Run existing direct-P2P smoke (`docs/direct-p2p-smoke-plan.md`) against any peer — the passive-receive path still works for `3SX_PUNCH` (non-symmetric fast path regression test).

**Depends on:** Step 3.

**Do NOT:**
- Add `DIRECT_P2P_FALLBACK_*` states yet — that's Step 5a.
- Spawn any new threads.
- Modify `join_thread_fn`.

**If it fails:**
- If smoke shows `3SX_PUNCH` no longer echoing, verify the magic check runs AFTER extracting the echo copy, or confirm the echo still fires on the non-`3SXR` branch. Revert to the pre-change `host_tick_receive` and re-add the magic-dispatch behind an explicit else-branch, not by mutating the existing control flow.

---

### Step 5a — Foundation: enums, helpers, accessors, lifecycle rework (no behavior change) **[DONE 2026-04-26]**

**Why:** lay down the foundation pieces before any new threads or fallback transitions are wired up. Splitting Step 5 into 5a/5b/5c keeps each `/implement` cycle small enough to land cleanly. Step 5a is purely additive — it compiles green, but no fallback transitions fire yet, so existing direct-P2P fast-path behavior is unchanged.

**Read first:**
- `/Users/sb/Developer/3sx-mister/src/netplay/direct_p2p.h` — enum, Role, public API
- `/Users/sb/Developer/3sx-mister/src/netplay/direct_p2p.c` — `s_work` declaration (`:115`), `s_thread` declaration (`:117`), spawn sites (`:603-604` host, `:644-645` join), `DirectP2P_Cancel` (`:659-661` spin loop), teardown (`:466-478`)
- `/Users/sb/Developer/3sx-mister/src/netplay/direct_p2p_overlay.c` — `dp2p_overlay_mode_label` switch (`:45-67`)
- `/Users/sb/Developer/3sx-mister/src/netplay/stun.c` — cancel-honor pattern (`:407-411`), 10ms loop (`:454`)
- §Decisions 3, 4, 5, 9 of this plan

**Changes:**
- `src/netplay/direct_p2p.h`:
  - Add `DIRECT_P2P_FALLBACK_SIGNALING`, `DIRECT_P2P_FALLBACK_BILATERAL_PUNCH`, `DIRECT_P2P_FAILED_BILATERAL` to the `DirectP2PState` enum, in that order, after the existing `DIRECT_P2P_FAILED_PUNCH` entry. Do NOT renumber existing values (additive only).
  - Export `Role DirectP2P_GetRole(void);` accessor (the existing `Role` enum at `:79-81` with `ROLE_HOST` / `ROLE_JOIN`). Used by `direct_p2p_overlay.c` so the mode-label switch can branch on role without depending on the file-local `s_work` (which is `static` to `direct_p2p.c:115`).
- `src/netplay/direct_p2p.c`:
  - Add `Role DirectP2P_GetRole(void) { return s_work.role; }` — trivial accessor returning a snapshot value. No atomic needed; `s_work.role` is set once per `BeginHost`/`BeginJoin` (`direct_p2p.c:581` host / `:633` join) and not mutated thereafter.
  - Add `static bool direct_p2p_is_lan_peer(const char* ip)` — checks `127.0.0.1`, `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, `169.254.0.0/16`. Returns true if LAN.
  - Add `static bool direct_p2p_ip_eq_normalized(const char* a, const char* b)` — uses `inet_pton(AF_INET, ...)` on both, compares the resulting `uint32_t`; returns false if either fails to parse. Used by the hairpin gate to defend against form mismatches (e.g., `"1.2.3.4"` vs `"::ffff:1.2.3.4"`) even though the wire format already guarantees DELIVER `peer_ip` is dotted-quad.
  - Introduce `static bool Rendezvous_Send(NET_DatagramSocket* sock, NET_Address* target, uint16_t target_port, const uint8_t* pkt, size_t pkt_len)` — a thin wrapper around `NET_SendDatagram`. Lives in `direct_p2p.c` (the SDL_net-purity constraint on `rendezvous.{c,h}` keeps that module SDL_net-free). Single named call site for production sends and the unit-test interpose point (Step 6 swaps the function pointer to a mock under `NETPLAY_TEST_HOOKS`). The same symbol is used on both host (queue-drain) and joiner (inline) send paths — no callers in 5a, but the symbol exists.
  - Add atomics `s_rendezvous_cancel` and `s_bilateral_punch_cancel` (separate from `s_cancel` so cancellation of one phase doesn't accidentally signal another), thread handles `s_rendezvous_thread` and `s_bilateral_punch_thread`, and a parsed rendezvous endpoint cache in `s_work` (parsed `signal_url` → `(host, port)`). All three thread handles (`s_thread`, `s_rendezvous_thread`, `s_bilateral_punch_thread`) are mutually exclusive on the user-action axis (host vs. join) but coexist in time on the host path during the FALLBACK_BILATERAL_PUNCH phase. Cancel and teardown must check all three (wired in this step; no spawns yet).
  - **Cancel semantics — switch from detach to wait.** Drop `SDL_DetachThread(s_thread); s_thread = NULL;` at both `direct_p2p.c:603-604` (host spawn site, inside `DirectP2P_BeginHost`) and `direct_p2p.c:644-645` (join spawn site, inside `DirectP2P_BeginJoin`). Keep the handle in `s_thread` (declared as a single static handle at `direct_p2p.c:117` — host and join paths are mutually exclusive on the user-action axis, so one slot is sufficient). Modify `DirectP2P_Cancel` (currently spinning for IDLE at `direct_p2p.c:659-661`): set `s_cancel`, then `if (s_thread) { SDL_WaitThread(s_thread, NULL); s_thread = NULL; }`, then teardown. The same `DirectP2P_Cancel` must also set `s_rendezvous_cancel` and `s_bilateral_punch_cancel`, and `SDL_WaitThread` each of `s_rendezvous_thread` / `s_bilateral_punch_thread` if non-NULL. This eliminates a pre-existing race where the worker writes `s_work` after Cancel's `memset` at `direct_p2p.c:674`, and prepares the lifecycle for the new threads added in 5b/5c.
  - **Natural-success exit — join `s_thread` on next BeginHost/BeginJoin and on teardown.** With `SDL_DetachThread` removed from the spawn sites, nothing nulls `s_thread` when the worker returns normally (success → `set_state(HANDOFF)` → return). A second `BeginHost`/`BeginJoin` call would then overwrite the handle without joining, leaking SDL3 thread state. Mitigation in two places: (1) at the start of `DirectP2P_BeginHost` and `DirectP2P_BeginJoin`, before `SDL_CreateThread`, do `if (s_thread != NULL) { SDL_WaitThread(s_thread, NULL); s_thread = NULL; }`. (2) `direct_p2p_on_teardown` joins `s_thread` if non-NULL alongside (the new) `s_rendezvous_thread` and `s_bilateral_punch_thread` — the worker may have published its terminal state via `set_state` and returned, leaving the handle un-joined.
  - **Extend `direct_p2p_on_teardown`.** Set `s_rendezvous_cancel` and `s_bilateral_punch_cancel`, then `SDL_WaitThread` each of `s_thread` / `s_rendezvous_thread` / `s_bilateral_punch_thread` if non-NULL. Each new thread (added in 5b/5c) honors its cancel flag at the next loop iteration (rendezvous thread: between sends, max ~500ms; bilateral-punch thread: inside `Stun_HolePunch` cancel check at `stun.c:407-411`, which runs every iteration with `SDL_Delay(10)` between, so max ~10ms). Total teardown blocking is bounded at ~510ms in the worst case. If this proves too long for the calling render thread, gate the wait with a 1-second deadline and log+continue on timeout (worker memory leaks rather than races).
  - Add `DirectP2P_Tick` skeleton cases for `FALLBACK_SIGNALING` (joiner-only — falls through to no-op, since joiner inlines the loop into its worker thread) and `FALLBACK_BILATERAL_PUNCH` (no-op skeleton; 5b will fill in the host bilateral-punch thread completion check). These cases compile but are unreachable in 5a because no transitions to those states fire yet.
- `src/netplay/direct_p2p_overlay.c`:
  - Map `DIRECT_P2P_FALLBACK_SIGNALING`, `DIRECT_P2P_FALLBACK_BILATERAL_PUNCH`, `DIRECT_P2P_FAILED_BILATERAL` per §Decision 9. The `dp2p_overlay_mode_label` switch (`:45-67`) calls `DirectP2P_GetRole()` to branch the mode label per role: `ROLE_HOST` → `"HOSTING"` for `HOST_WAITING / FALLBACK_SIGNALING / FALLBACK_BILATERAL_PUNCH`; `ROLE_JOIN` → `"CONNECTING"` for `JOIN_PUNCHING / FALLBACK_SIGNALING / FALLBACK_BILATERAL_PUNCH`. Both → `"ERROR"` for `FAILED_BILATERAL`.

**Success criteria:**
- `tools/mister/build-game.sh --flavor telemetry` builds clean.
- `cmake --build build/host` builds clean (host).
- `grep -n 'DirectP2P_GetRole' src/netplay/direct_p2p.{c,h}` prints ≥ 2 lines (one in the header, one in the implementation).
- `grep -n 'direct_p2p_is_lan_peer\|direct_p2p_ip_eq_normalized\|Rendezvous_Send' src/netplay/direct_p2p.c` prints ≥ 3 lines.
- `grep -n 'DIRECT_P2P_FALLBACK_SIGNALING\|DIRECT_P2P_FALLBACK_BILATERAL_PUNCH\|DIRECT_P2P_FAILED_BILATERAL' src/netplay/direct_p2p.h` prints exactly 3 lines (enum members).
- `grep -n 'SDL_DetachThread' src/netplay/direct_p2p.c` returns 0 lines (the two detach sites are removed).
- Existing direct-P2P fast path still succeeds: non-symmetric peers connect end-to-end. No `FALLBACK_*` state appears in any log line (because nothing transitions to those states yet).

**Depends on:** Steps 2, 3, 4.

**Do NOT:**
- Spawn any new thread in 5a — `host_rendezvous_thread_fn` and `host_bilateral_punch_thread_fn` arrive in 5b. The thread handles and cancel atomics are declared in 5a but stay NULL/0 throughout.
- Add any host-side fallback transition (`HOST_WAITING → FALLBACK_BILATERAL_PUNCH`); that's 5b.
- Add any joiner-side fallback transition (`JOIN_PUNCHING → FALLBACK_SIGNALING`); that's 5c.
- Modify `host_tick_receive`'s dispatch logic (Step 4 already added the magic-byte stub).
- Touch `src/netplay/stun.c`, `src/netplay/netplay.c`, `src/netplay/netplay_nav.c`, or `vendor/Main_MiSTer/`.

**If it fails:**
- Thread join races at `DirectP2P_Cancel`: confirm `SDL_WaitThread` is called only when `s_thread != NULL`, and that the natural-success exit path doesn't double-join (the handle is nulled inside the wait branch).
- Overlay regression: if direct-P2P sessions show blank or wrong mode labels, confirm `DirectP2P_GetRole()` is being called and that 5a's switch update still maps the existing pre-fallback states (`HOST_WAITING`, `JOIN_PUNCHING`, `HANDOFF`, `FAILED_*`) the same way they were before.
- Build error from `inet_pton` unavailable: include `<arpa/inet.h>` on POSIX, `<ws2tcpip.h>` on Windows. Match what `src/netplay/stun.c` already includes for the same primitive.

---

### Step 5b — Host fallback path: rendezvous thread, send queue, DELIVER handler, bilateral-punch thread **[DONE 2026-04-26]**

**Why:** with the foundation in place, wire up the host's fallback path so a host that fails the direct passive-receive can REGISTER with the rendezvous server, receive a DELIVER, and run a bilateral `Stun_HolePunch` against the joiner's endpoint. After 5b, the host fallback path is implemented and reachable; the joiner is still unchanged from today (joiner's fallback comes in 5c), so end-to-end fallback won't function until 5c lands.

**Read first:**
- `/Users/sb/Developer/3sx-mister/src/netplay/direct_p2p.c` — `host_thread_fn` (`:288-365`), `host_tick_receive` (`:516-552`), `try_handle_deliver` stub (added in Step 4), `do_handoff` (`:483`), `set_state(DIRECT_P2P_HANDOFF)` (`:547`)
- `/Users/sb/Developer/3sx-mister/src/netplay/stun.c` — `Stun_HolePunch` (`:421` direct call site, `:438-442` peer in/out param overwrite, `:454` 10ms loop), `NET_GetAddressStatus` polling pattern (`:272-275` 100ms-bounded variant)
- `/Users/sb/Developer/3sx-mister/src/netplay/rendezvous.h` — from Step 3
- §Decisions 2, 3, 5, 9 of this plan

**Changes:**
- `src/netplay/direct_p2p.c`:
  - Add the `s_rendezvous_send_q` SPSC ring per §Decision 3's concrete spec: 8 slots; slot type `struct { NET_Address* target; uint16_t target_port; uint8_t payload[28]; uint8_t payload_len; }`; producer `NET_RefAddress` before enqueue, drain `NET_UnrefAddress` after send (or after drop on overflow); `SDL_AtomicInt s_q_head` (consumer-write) and `SDL_AtomicInt s_q_tail` (producer-write); producer drops on overflow (returns false, increments `s_q_drops`, logs once per session via `SDL_Log`); drain rate up to 4 slots per tick.
  - Add `static int SDLCALL host_rendezvous_thread_fn(void* data)` — parses signal URL, resolves the rendezvous hostname **once** at thread start using the existing 100ms-bounded `NET_GetAddressStatus` polling pattern from `stun.c:272-275` (`stun.c:385-389` uses a longer 3000ms variant for peer addresses; we want the 100ms server-hostname variant.) If resolution fails (NULL or status != `NET_SUCCESS`), exit thread immediately (host: stay HOST_WAITING; do not retry — the 8-second budget is for peer-pairing, not DNS). Then the thread builds REGISTER/POLL packets and enqueues `(NET_RefAddress(target), payload[28])` tuples onto `s_rendezvous_send_q`. The thread never calls `NET_SendDatagram` directly.
  - **Post-budget status update.** When `host_rendezvous_thread_fn` exits with no DELIVER received (8-second budget expired), it must set status to `"Waiting for peer (no peer detected - check that they're using a recent build)."` before exiting. **Do NOT change state** — host stays at `DIRECT_P2P_HOST_WAITING` so the passive receive path keeps running in case a late-arriving direct punch still works. Per §Decision 9, this is a status-only update.
  - Wire `DirectP2P_Tick`'s `HOST_WAITING` branch to drain up to 4 slots from `s_rendezvous_send_q`, invoking `Rendezvous_Send(s_work.stun.socket, target, target_port, pkt, 28)` per slot, then `NET_UnrefAddress(target)`. Drain happens BEFORE `host_tick_receive` so the main thread is the sole socket-I/O actor during this phase. Per §Decision 3, sending REGISTER from the STUN socket is required (the server records the packet's source endpoint, which must be the same STUN-visible `(ip, port)` the joiner will punch toward; a separate ephemeral socket would register the wrong endpoint).
  - Update `host_thread_fn` to spawn `host_rendezvous_thread_fn` after publishing `HOST_WAITING` (unless `CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL` is true). Per §Decision 5, the host stays in `HOST_WAITING` during REGISTER/POLL — there is no host-side `set_state(FALLBACK_SIGNALING)` call.
  - Flesh out `try_handle_deliver` from Step 4: parse with `Rendezvous_ParseDeliver`; if peer fields are non-zero and state is `HOST_WAITING`:
    - Set `s_work.peer_ip/peer_public_port` from DELIVER.
    - Signal `s_rendezvous_cancel` to stop the rendezvous thread.
    - If `direct_p2p_is_lan_peer(peer_ip)`: log, do NOT enter bilateral (we should have seen a direct punch already; something is weird. Stay in HOST_WAITING.)
    - **If `direct_p2p_ip_eq_normalized(peer_ip, s_work.stun.public_ip)`** (NAT-hairpin case — same public IP means same LAN behind broken loopback): log, transition to `FAILED_SYMMETRIC`. Rendezvous won't help because the router will fail to loop back the bilateral punch the same way it failed the fast path (per §Hard requirement 3(c)). Compare normalized — never `strcmp` on possibly-prefixed string forms.
    - Else: transition to `FALLBACK_BILATERAL_PUNCH`, spawn a short-lived `host_bilateral_punch_thread_fn` that calls `Stun_HolePunch`. Before calling `Stun_HolePunch`, the bilateral-punch thread copies `s_work.peer_ip` and `s_work.peer_public_port` into stack-local buffers (mirror `direct_p2p.c:418-420`'s pattern). `Stun_HolePunch` overwrites its in-out parameters at `stun.c:438,442` post-receive; passing `&s_work.peer_ip[0]` directly would race with `do_handoff`'s read of the same field on the main thread.
      - **Writeback ordering on success.** When `Stun_HolePunch` returns true, copy the (possibly-updated) local `peer_ip` and `peer_port` BACK to `s_work.peer_ip` and `s_work.peer_public_port` BEFORE calling `set_state(DIRECT_P2P_HANDOFF)`. The main thread's `do_handoff` reads `s_work.peer_ip` at `direct_p2p.c:558`; if writeback happens after `set_state`, `do_handoff` reads the pre-punch endpoint and the connection will be misrouted. Mirror the join-side ordering at `direct_p2p.c:445-446` (writeback) → `:456` (set_state).
  - Fill in `DirectP2P_Tick`'s `FALLBACK_BILATERAL_PUNCH` case (skeleton from 5a): check if `host_bilateral_punch_thread_fn` has completed and handoff / fail accordingly. **This case must NOT call `host_tick_receive`** — per §Decision 3, the bilateral-punch thread exclusively owns `s_work.stun.socket` for reads during its `Stun_HolePunch` lifetime, so concurrent `NET_ReceiveDatagram` on the main thread would race. In concrete terms: the case is thread-wait-only (check `SDL_WaitThreadTimeout` or an atomic "done" flag); it does not touch the STUN socket. **No host-side `FALLBACK_SIGNALING` Tick case is needed** — per §Decision 5, the host stays in `HOST_WAITING` during the REGISTER/POLL phase; the existing `HOST_WAITING` Tick branch (drains `s_rendezvous_send_q` then runs `host_tick_receive`) already covers it.
  - Update status strings via `set_status` on the host transitions per §Decision 9 (`HOST_WAITING` post-budget; `FALLBACK_BILATERAL_PUNCH`).

**Success criteria:**
- `tools/mister/build-game.sh --flavor telemetry` builds clean.
- `cmake --build build/host` builds clean (host).
- `grep -n 'host_rendezvous_thread_fn\|host_bilateral_punch_thread_fn\|s_rendezvous_send_q' src/netplay/direct_p2p.c` prints ≥ 3 lines.
- Manual probe: with a (real or mock) rendezvous server reachable, host on a node behind symmetric NAT and confirm `[direct_p2p] DELIVER received peer=<ip>:<port>`, `[direct_p2p] entering FALLBACK_BILATERAL_PUNCH`, `STUN: Hole punch SUCCESS`, `[direct_p2p] Handoff to netplay` log lines fire on the host side. Joiner-side fallback is still pre-5c, so end-to-end fallback succeeds only if the joiner is on a non-symmetric NAT (host's bilateral punch reaches them before they time out).
- Direct-P2P fast path still succeeds (non-symmetric peers): no `FALLBACK_BILATERAL_PUNCH` log line appears.

**Depends on:** Step 5a.

**Do NOT:**
- Touch `join_thread_fn` — joiner fallback comes in 5c.
- Change `src/netplay/stun.c` or `stun.h`. `Stun_HolePunch` already handles bilateral correctly.
- Change `src/netplay/netplay.c` — socket handoff is unchanged.
- Change `src/netplay/netplay_nav.c` — nav just waits for `Netplay_IsRemoteIpSet`. That still fires at the exact same point (`do_handoff` call).
- Touch any wrapper file in `vendor/Main_MiSTer/`. Wrapper knows nothing about bilateral punch.

**If it fails:**
- Send-queue overflow: `s_rendezvous_send_q` has 8 slots and a per-tick drain bound of 4 (per §Decision 3 concrete spec). At a 500ms-cadence REGISTER/POLL loop the steady-state depth is ≤2; overflow would indicate the rendezvous thread is enqueuing faster than the main thread is draining. The producer drops on overflow (incrementing `s_q_drops` and logging once per session) rather than blocking, so a stuck drain doesn't deadlock the worker — verify the drain bound and the thread's send cadence in that case.
- Concurrent `NET_ReceiveDatagram` on STUN socket during `FALLBACK_BILATERAL_PUNCH`: confirm `DirectP2P_Tick`'s case for that state does NOT call `host_tick_receive` and does NOT drain the send queue.
- Thread join races on the new `s_rendezvous_thread` / `s_bilateral_punch_thread`: confirm 5a's `DirectP2P_Cancel` and `direct_p2p_on_teardown` extensions wait on each non-NULL handle.

---

### Step 5c — Joiner fallback path: inline REGISTER/POLL, hairpin gate, bilateral punch, status strings **[DONE 2026-04-26]**

**Why:** finish the bilateral path by wiring the joiner's fallback branch. After 5c lands, both peers in a symmetric-NAT pair fall back through rendezvous → bilateral punch → handoff. End-to-end fallback works.

**Read first:**
- `/Users/sb/Developer/3sx-mister/src/netplay/direct_p2p.c` — `join_thread_fn` (`:372-458`), today's `Stun_HolePunch` failure path landing at `:435` (`set_state(DIRECT_P2P_FAILED_SYMMETRIC)`), worker exit at `:456-458`
- `/Users/sb/Developer/3sx-mister/src/netplay/stun.c` — `NET_GetAddressStatus` polling pattern (`:272-275` 100ms-bounded variant)
- `/Users/sb/Developer/3sx-mister/src/netplay/rendezvous.h` — from Step 3
- `/Users/sb/Developer/3sx-mister/src/netplay/direct_p2p_overlay.c` — confirm the role-branching mode-label switch from 5a
- §Decisions 4, 5, 9 of this plan

**Changes:**
- `src/netplay/direct_p2p.c`:
  - Update `join_thread_fn`: on `Stun_HolePunch` failure, instead of immediate transition to `FAILED_SYMMETRIC`:
    - If `CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL` is true OR `direct_p2p_is_lan_peer(s_work.peer_ip)` OR `direct_p2p_ip_eq_normalized(s_work.peer_ip, s_work.stun.public_ip)`: transition to `FAILED_SYMMETRIC` (unchanged).
      - The third disjunct is the joiner-side hairpin gate, mirroring the host-side check at 5b's DELIVER handler. Hard Requirement 3(c) applies symmetrically to both sides: on the joiner, the gate fires post-STUN-discovery, post-`Stun_HolePunch`-failure, before the FALLBACK_SIGNALING transition (so `s_work.stun.public_ip` is populated and the direct punch has already been attempted); on the host, it fires at DELIVER receive time.
    - Else: transition to `FALLBACK_SIGNALING`; resolve the rendezvous hostname **once** at the start of this branch using the existing 100ms-bounded `NET_GetAddressStatus` polling pattern from `stun.c:272-275`. If resolution fails (NULL or status != `NET_SUCCESS`), transition immediately to `FAILED_BILATERAL` (do not retry — the 8-second budget is for peer-pairing, not DNS). Then inline-run the REGISTER/POLL loop for the configured budget; on DELIVER, transition to `FALLBACK_BILATERAL_PUNCH` and run `Stun_HolePunch` with `CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS` budget; on DELIVER-never-arrived or second punch failure, `FAILED_BILATERAL`; on success, `HANDOFF`.
      - The joiner's inline REGISTER/POLL loop calls `Rendezvous_Send(s_work.stun.socket, ...)` directly from the existing `join_thread_fn` worker thread. Joiner does not need a queue: its `s_work.stun.socket` has only one reader/writer (the worker itself). The same `Rendezvous_Send` symbol is used on both sides, so Step 6's interpose covers both.
    - **Worker-lifetime note:** this extends `join_thread_fn`'s runtime from today's "publish state then exit immediately" contract (`direct_p2p.c:456-457`) by up to `signal_budget_ms + bilateral_punch_ms` (default 8000 + 3000 = 11 s) in the fallback path. Cancel responsiveness comes from `SDL_WaitThread` after the cancel atomic is set (5a's lifecycle rework); the inline REGISTER/POLL loop must check `SDL_GetAtomicInt(&s_cancel)` between sends (no busier than every 100ms) so the worker exits within ~100ms of cancellation. `Stun_HolePunch` already honors `&s_cancel` between iterations at `stun.c:407-411`; the cancel-flag check fires once per loop iteration with `SDL_Delay(10)` between iterations (`stun.c:454`), so worst-case latency is ~10ms after the cancel atomic is set, well within the bounded `SDL_WaitThread` deadline (1 second per §Decision 3 / `direct_p2p_on_teardown`). The host worker (`direct_p2p.c:288, exits at :361-365`) similarly publishes HOST_WAITING and exits today; with this plan it remains short-lived (the rendezvous and bilateral-punch threads are spawned separately, not extensions of the host worker). Cancel/teardown waits on `s_rendezvous_thread` and `s_bilateral_punch_thread` independently of `s_thread`.
  - Update status strings via `set_status` on joiner transitions per §Decision 9 (`FALLBACK_SIGNALING`, `FALLBACK_BILATERAL_PUNCH`, `FAILED_BILATERAL`).
- `src/netplay/direct_p2p_overlay.c`:
  - Re-verify the role-branching mode-label switch from 5a covers all the new states for `ROLE_JOIN`. `JOIN_PUNCHING / FALLBACK_SIGNALING / FALLBACK_BILATERAL_PUNCH` → `"CONNECTING"`; `HANDOFF` → `"CONNECTED"`; `FAILED_*` (including `FAILED_BILATERAL`) → `"ERROR"`. No new code expected here in 5c if 5a covered it; sanity-grep.

**Success criteria:**
- `tools/mister/build-game.sh --flavor telemetry` builds clean.
- `cmake --build build/host` builds clean (host).
- `grep -n 'DIRECT_P2P_FALLBACK_SIGNALING\|DIRECT_P2P_FALLBACK_BILATERAL_PUNCH\|DIRECT_P2P_FAILED_BILATERAL' src/netplay/direct_p2p.c` prints ≥ 9 lines (decl + 3+ uses each — counting host and join paths together).
- Manual end-to-end probe in `docs/direct-p2p-smoke-plan.md` §Bilateral smoke — two-home test (this section is authored in Step 7; early Step 5c runs can log-grep manually for the five lines below): two-home test with one symmetric NAT shows logs `[direct_p2p] entering FALLBACK_SIGNALING`, `[direct_p2p] DELIVER received peer=<ip>:<port>`, `[direct_p2p] entering FALLBACK_BILATERAL_PUNCH`, `STUN: Hole punch SUCCESS`, `[direct_p2p] Handoff to netplay`.
- Direct-P2P fast path still succeeds (non-symmetric peers): no `FALLBACK_SIGNALING` log line appears.
- Kill-switch honored: with `netplay-direct-p2p-disable-bilateral=true`, joiner lands on `FAILED_SYMMETRIC` exactly as today.

**Depends on:** Steps 5a, 5b.

**Do NOT:**
- Spawn a signaling thread on the joiner side — joiner's existing worker thread handles rendezvous inline. Only host needs new threads because host's existing worker exits after publishing `HOST_WAITING`.
- Change `src/netplay/stun.c` or `stun.h`. `Stun_HolePunch` already handles bilateral correctly.
- Change `src/netplay/netplay.c` — socket handoff is unchanged.
- Change `src/netplay/netplay_nav.c` — nav just waits for `Netplay_IsRemoteIpSet`. That still fires at the exact same point (`do_handoff` call).
- Touch any wrapper file in `vendor/Main_MiSTer/`. Wrapper knows nothing about bilateral punch.

**If it fails:**
- If the state machine drops into `FALLBACK_SIGNALING` on LAN traffic: double-check `direct_p2p_is_lan_peer` is called before the fallback transition.
- If the joiner stays in `FALLBACK_SIGNALING` past the 8-second budget without transitioning to `FAILED_BILATERAL`: confirm the inline REGISTER/POLL loop honors the budget timer and that DNS resolution did not silently spin.
- Hairpin gate failing-open on form mismatch: confirm the joiner's third-disjunct check uses `direct_p2p_ip_eq_normalized` and not `strcmp`.
- Worker-lifetime cancel propagation: confirm the inline REGISTER/POLL loop checks `SDL_GetAtomicInt(&s_cancel)` between sends so a `DirectP2P_Cancel` during fallback exits within ~100ms.

---

### Step 6 — Test harness (`--test-bilateral-punch`) **[DONE 2026-04-26]**

**Why:** automated tests for the rendezvous client and state-machine transitions. Covers Decision 7's CI split.

**Read first:**
- `/Users/sb/Developer/3sx-mister/src/netplay/test_stun_mock.c` — the test pattern we're mirroring
- `/Users/sb/Developer/3sx-mister/src/main.c` lines 937-1000 — existing test dispatch
- `/Users/sb/Developer/3sx-mister/src/args.c` lines 216-240 — existing CLI flag registration

**Changes:**
- Create `src/netplay/test_bilateral_punch.c` — gated by `ENABLE_NETPLAY_TESTS`. Expose `int Netplay_Test_BilateralPunch(void)`.
- Tests 1-6 from §Decision 7:
  1. Mock UDP rendezvous server + two mock clients verifying REGISTER → DELIVER flow.
  2. Session-key derivation stability (known vector).
  3. LAN bypass rejection table-driven over the 5 CIDR ranges + public IP.
  4. State-machine drive using a test-only injection seam. Add a minimal seam in `direct_p2p.c` behind `#ifdef NETPLAY_TEST_HOOKS`: function pointer for `Stun_HolePunch` override (today a direct call at `direct_p2p.c:421`) + a `Rendezvous_Send` override (the `static` helper defined in `direct_p2p.c`, introduced in Step 5a). Both pointers default to the production implementations; tests swap them under `NETPLAY_TEST_HOOKS` to record calls without touching the network.
  5. Kill-switch honored: set `Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL, "true")`, drive `BeginJoin`, observe terminal state is `FAILED_SYMMETRIC`, assert mock rendezvous server received zero packets.
- **Modify `src/netplay/direct_p2p.c`** to add the `NETPLAY_TEST_HOOKS` seam that Test 4 depends on. Specifically: replace the direct `Stun_HolePunch(&s_work.stun, ...)` calls (at `direct_p2p.c:421` and the new host/join bilateral-punch call sites added in Steps 5b/5c) with an indirection through a static function pointer that defaults to `Stun_HolePunch`. Under `-DNETPLAY_TEST_HOOKS`, the test harness sets the pointer to a mock. Same pattern for the `Rendezvous_Send` `static` helper introduced in Step 5a. This is additive — production builds compile to an unchanged direct call when `NETPLAY_TEST_HOOKS` is undefined.
- Update `src/args.c` — register `--test-bilateral-punch` alongside existing `--test-*` flags. Add a new bool field `test_bilateral_punch` on the top-level `Configuration` struct (same pattern as `configuration->test_stun_mock`, `test_room_code`, `test_mist_handshake` at `src/args.c:217-243`).
- Update `src/configuration.h` — add a new bool field `test_bilateral_punch` to the `Configuration` struct in `src/configuration.h:61-99`, mirroring the existing `test_stun_mock` / `test_room_code` / `test_mist_handshake` / `test_netplay_event_queue` pattern.
- Update `src/main.c` — dispatch `Netplay_Test_BilateralPunch()` alongside the existing four test dispatchers at `:962-1000` (forward-decls begin at `:938`; the `if (configuration.test_*)` blocks start at `:962`).
- Forward-declare `int Netplay_Test_BilateralPunch(void);` in the test-dispatch block per existing pattern.

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DNETPLAY_TEST_HOOKS=ON -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS" tools/mister/build-game.sh --flavor telemetry` builds clean. `-DNETPLAY_TEST_HOOKS=ON` is not optional here — it is the CMake `option()` at `CMakeLists.txt:47` that compiles in the seam this step's harness links against; without it the configure still succeeds and the build then fails with ~20 compile errors.
- Running the resulting binary with `--test-bilateral-punch` exits 0 and prints `[test_bilateral_punch] OK`.
- `grep -n 'Netplay_Test_BilateralPunch\|test_bilateral_punch\|--test-bilateral-punch' src/ -r` prints ≥ 5 lines.
- Test run completes in < 3 seconds (no real network I/O).

**Depends on:** Steps 2, 3, 5c.

**Do NOT:**
- Add network I/O against a live rendezvous server from tests. Use only in-process mock sockets on localhost.
- Add test code that requires the Docker cross-build. All tests run locally (host or MiSTer binary), not in CI image builds.
- Couple the tests to real `docs/direct-p2p-smoke-plan.md` procedures — those are human-driven.

**If it fails:**
- If `NETPLAY_TEST_HOOKS` injection seam causes the non-test build to regress, gate the seam entirely behind the macro: production build compiles to a direct call, test build compiles to an indirect through a function pointer.
- If localhost UDP mock server flakes on macOS (port reuse), add `SO_REUSEADDR` per the existing `test_stun_mock.c` pattern.

---

### Step 7 — Smoke plan + two-home test doc **[DONE 2026-04-26]**

**Why:** Decision 7's human-driven verification needs a written procedure. Adds a section to the existing `docs/direct-p2p-smoke-plan.md` so it shows up when someone asks "how do I test netplay".

**Read first:**
- `/Users/sb/Developer/3sx-mister/docs/direct-p2p-smoke-plan.md` — the existing smoke doc we're extending
- `/Users/sb/Developer/3sx-mister/docs/plan-bilateral-hole-punch.md` §Rollout

**Changes:**
- Extend `docs/direct-p2p-smoke-plan.md` with a new section "Bilateral smoke — two-home test":
  - Pre-reqs: two MiSTer units on different residential networks. At least one behind a known-symmetric NAT (confirmed via pre-existing direct-P2P smoke procedure).
  - Steps: boot both, Host on one, enter code on the other, observe the overlay transitions through `FALLBACK_SIGNALING` → `FALLBACK_BILATERAL_PUNCH` → `CONNECTED`.
  - Expected log lines on both devices (specific SDL_Log messages from Step 5b/5c's code).
  - Failure diagnostics: what to capture if `FALLBACK_BILATERAL` fires (tcpdump on the rendezvous server or on the client uplink, time-correlated client logs).
  - Kill-switch verification: set `netplay-direct-p2p-disable-bilateral=true` on one side, confirm the other side lands on `FAILED_BILATERAL` promptly (because the disabled side never REGISTERs and the server never pairs them).

**Success criteria:**
- `docs/direct-p2p-smoke-plan.md` has a new "Bilateral smoke" section.
- Procedure is concrete enough that a non-author can follow it.
- `grep -n 'Bilateral smoke\|FALLBACK_SIGNALING' docs/direct-p2p-smoke-plan.md` prints ≥ 3 lines.

**Depends on:** Step 5c.

**Do NOT:**
- Rewrite existing sections of `docs/direct-p2p-smoke-plan.md` — only extend.
- Add automated smoke scripts that require two MiSTer units. The test is inherently human-driven; don't fake automation.

**If it fails:**
- If the smoke procedure changes based on what actually worked in Steps 5a/5b/5c (e.g., overlay text differs slightly), update the doc to match the code — code is ground truth.

---

### Step 8 — End-to-end doc updates + memory note **[DONE 2026-04-26]**

**Why:** the strategy memo and user-facing docs need to reflect the new fallback tier. Also adds a memory entry so future agents know bilateral exists.

**Read first:**
- `/Users/sb/.claude/projects/-Users-sb-Developer-3sx-mister/memory/project-netplay-port-strategy.md`
- `/Users/sb/Developer/3sx-mister/docs/STUN-PORT-STATUS.md`
- `/Users/sb/Developer/3sx-mister/docs/config.md` (already updated in Step 2)

**Changes:**
- Add a new memory entry `project-bilateral-hole-punch.md` under `/Users/sb/.claude/projects/-Users-sb-Developer-3sx-mister/memory/` — one paragraph summary of: feature intent, where it lives in the state machine, kill switch, what's NOT implemented (TURN, port prediction).
- Update the memory index at `/Users/sb/.claude/projects/-Users-sb-Developer-3sx-mister/memory/MEMORY.md` with a new line for the bilateral entry.
- Update `docs/STUN-PORT-STATUS.md` to note that `FAILED_SYMMETRIC` now has a bilateral-punch fallback (with a forward reference to `docs/plan-bilateral-hole-punch.md`).
- Do NOT update `project-netplay-port-strategy.md` — that memo is about matchmaking, not transport fallback; cross-reference only.

**Success criteria:**
- `ls /Users/sb/.claude/projects/-Users-sb-Developer-3sx-mister/memory/project-bilateral-hole-punch.md` exists.
- `grep -n 'project-bilateral-hole-punch' /Users/sb/.claude/projects/-Users-sb-Developer-3sx-mister/memory/MEMORY.md` prints 1 line.
- `grep -n 'bilateral\|FAILED_BILATERAL' docs/STUN-PORT-STATUS.md` prints ≥ 1 line.

**Depends on:** Step 5c (must be complete and functional — docs follow code).

**Do NOT:**
- Create per-state blog-post-length docs. Memory entries are 3-8 lines.
- Update any other memory note that doesn't directly concern transport fallback.

**If it fails:**
- If the memory path doesn't exist (e.g., fresh machine without the user-auto-memory dir), create it — the index at `/Users/sb/.claude/projects/-Users-sb-Developer-3sx-mister/memory/MEMORY.md` already exists per the system reminder.

---

## Open questions deferred to review

- ~~Does SDL3_net's `NET_ReceiveDatagram` truly tolerate concurrent sends from another thread on the same socket?~~ **Resolved by audit: SDL3_net's `pending_output` queue is unsynchronized (`/private/tmp/sdl_net_ref/src/SDL_net.c:2015`); design uses a main-thread send-drain queue (see §Decision 3, §Step 5b).**
- Rendezvous server IP for the default config: placeholder `rendezvous.3s-arm.example` — real DNS name must exist before Step 5c is shippable to users. Review should confirm we have infra budget / plan.
- Should we also support rendezvous over IPv6? Rejected for MVP (MiSTer kernel has IPv6 disabled per `reference-mister-network-stack.md`), but review should confirm macOS host builds are OK sticking with IPv4-only signaling.
- Kill-switch default (`false`) means bilateral is on by default from first deploy. Review: should we ship with `true` for the first public build so we can soak the rendezvous server, then flip to `false` via a later release?

---

## Dependency graph

```
Step 1 (server)   ──┐
                    ├─→ Step 3 (client) ─→ Step 4 (dispatch) ─→ Step 5a (foundation) ─→ Step 5b (host) ─→ Step 5c (joiner) ─→ Step 6 (tests)
Step 2 (config) ────┘                                                                                                       ├─→ Step 7 (smoke doc)
                                                                                                                            └─→ Step 8 (memory + docs)
```

Steps 1 and 2 run in parallel. Steps 3 and 4 are serial. Step 5a depends on 2/3/4; 5b depends on 5a; 5c depends on 5a/5b. Steps 6, 7, 8 depend on 5c and can run in parallel — but Step 7 (smoke) needs a running rendezvous server (Step 1 done and deployed), so scheduling-wise Step 7 lands last in practice even if Step 6 finishes first.

---

## Build matrix per step

| Step | `build-game.sh` needed? | `build-hps.sh` needed? | On-device smoke? |
|------|--------------------------|--------------------------|-------------------|
| 1 | No | No | No (server) |
| 2 | Yes | No | No |
| 3 | Yes | No | No |
| 4 | Yes | No | Yes (regression check only) |
| 5a | Yes | No | Yes (regression: fast path still works; no fallback transitions yet) |
| 5b | Yes | No | Yes (host fallback reachable; full end-to-end requires non-symmetric joiner) |
| 5c | Yes | No | Yes (two-home; full bilateral path) |
| 6 | Yes (with `ENABLE_NETPLAY_TESTS`) | No | No |
| 7 | No | No | Yes (two-home) |
| 8 | No | No | No |

Wrapper (`build-hps.sh`) is not touched by any step — per §Scope boundaries, bilateral punch is fully game-side.

---

## Review disposition

Paper trail of how every P-1, P-2, and substantive Nit from `docs/plan-bilateral-hole-punch-review.md` was handled.

### P-1

| Item | Disposition | Note |
|------|-------------|------|
| P-1.1 SHA-256 already in-tree at `src/utils/sha256.{c,h}` | **Addressed** | §Decision 2 and §Step 3 rewritten to use the in-tree `sha256_init`/`sha256_append`/`sha256_finalize_bytes` API; dropped `src/netplay/sha256.{c,h}` creation; "Do NOT" list in Step 3 updated to forbid vendoring from `/tmp/3sxtra/`. |
| P-1.2 `tools/lobby-server/` not in our repo | **Addressed** | §Step 1 "Why", "Read first", and "Changes" now cite `/tmp/3sxtra/tools/lobby-server/*` as upstream reference-only; added `project-netplay-port-strategy.md` cross-reference; "Do NOT" explicitly forbids copying that tree into our repo. |
| P-1.3 Socket ownership during FALLBACK_BILATERAL_PUNCH | **Addressed** | §Decision 3 restructured into two phases (REGISTER/POLL = main-thread reader; BILATERAL_PUNCH = bilateral-thread exclusive reader) with explicit hand-off rule. §Step 5 `DirectP2P_Tick` bullet now states the `FALLBACK_BILATERAL_PUNCH` case must NOT call `host_tick_receive`. §Step 5 `host_rendezvous_thread_fn` bullet rewritten to drop the self-contradiction (send-only, shares STUN socket, main thread consumes DELIVER). |

### P-2

| Item | Disposition | Note |
|------|-------------|------|
| P-2.1 NAT-hairpin case not covered by RFC1918 bypass | **Addressed** | §Hard requirement 3 extended with third gate (c): compare DELIVER-supplied `peer_ip` to `s_work.stun.public_ip` and skip bilateral if they match. §Step 5 DELIVER-handler gains the `strcmp(peer_ip, s_work.stun.public_ip) == 0` short-circuit transitioning to `FAILED_SYMMETRIC`. |
| P-2.2 48-bit input entropy, not 128-bit | **Addressed** | §Security "Collision probability" rewritten to reflect 48-bit input ceiling and note that 128-bit truncation is only for non-typeability, not entropy. |
| P-2.3 `direct_p2p.c` needs `NETPLAY_TEST_HOOKS` seam in Step 6 | **Addressed** | §Step 6 "Changes" now explicitly lists `src/netplay/direct_p2p.c` as a modified file, cites the `Stun_HolePunch` call site at `direct_p2p.c:421`, and documents the function-pointer indirection pattern. |
| P-2.4 `buflen >= 24` vs. DELIVER's 32 bytes | **Addressed** | §Step 4 criterion changed to `buflen >= 32` with a comment explaining hosts never receive REGISTER/POLL. |
| P-2.5 Kill-switch host-side behavior | **Skipped (verified correct)** | Reviewer explicitly marked this as no-change-needed after verifying the kill switch matches today's behavior. No plan edit required. |
| P-2.6 Join-side worker lifetime extension | **Addressed** | §Step 5 join-thread bullet gains an explicit "Worker-lifetime note" citing the 11 s worst case and confirming `DirectP2P_Cancel`'s 500 ms grace is adequate via `stun.c:407-411`'s cancel-flag honor. |
| P-2.7 `Configuration` struct name | **Addressed** | §Step 6 now states the field is a simple `bool test_bilateral_punch` on the top-level `Configuration` struct, matching the `test_stun_mock`/`test_room_code`/`test_mist_handshake` pattern at `src/args.c:217-243`. |

### Nits

| Item | Disposition | Note |
|------|-------------|------|
| §Decision 2 byte-order wording ambiguity | **Addressed** | Header line above the wire-format block now explicitly states multi-byte fields are network byte order and that ports require `htons`-equivalent swap on encode (and reverse on decode). §Step 3 `Rendezvous_BuildRegister` bullet notes the same. |
| §Decision 3 host-worker-exit contract at `direct_p2p.c:361-365` | **Skipped (verified correct)** | Reviewer verified the accuracy; no plan edit needed. |
| §Step 1 "Node.js/Go" stray alternative | **Addressed** | Dropped "/Go"; §Decision 1 locked in Node.js. |
| §Step 5 forward reference to §Bilateral smoke | **Addressed** | Added parenthetical "(this section is authored in Step 7; early Step 5 runs can log-grep manually for the five lines below)". |
| §Step 8 MEMORY.md path | **Addressed** | Full absolute path now cited. |
| Overlay mode-label mapping trivially small | **Skipped (trivial)** | No plan edit needed; §Step 5 already directs overlay changes per §Decision 9. |
| `join_thread_fn` cited as `:372-457` vs. actual `:372-458` | **Addressed** | Corrected to `:372-458` in §Decision 4. |

## Re-verification 2026-04-26

A second deep verification pass surfaced 12 additional findings. Each is addressed in this revision of the plan; corresponding details land in the §Re-verification 2026-04-26 section of `plan-bilateral-hole-punch-review.md`.

| Item | Disposition | Note |
|------|-------------|------|
| NEW-1 Wire-format byte counts and IPv4 encoding inconsistent | **Addressed** | §Decision 2 wire format rewritten: REGISTER/POLL = 28 bytes, DELIVER = 32 bytes. `peer_ip` collapsed to 4 raw bytes IPv4 in network byte order; ambiguous IPv4-mapped-IPv6 alternation dropped. Server bind/parse spec added (`dgram.createSocket('udp4')` + `inet_pton`/`inet_ntop`). Resend size note updated (28 bytes). |
| NEW-2 Threading model: SDL3_net unsynchronized `pending_output` | **Addressed** | §Decision 3 rewritten to specify a main-thread send-drain queue (`s_rendezvous_send_q`); rendezvous thread never touches the STUN socket. §Step 5 `host_rendezvous_thread_fn` reflects the queue. §Open question 1 (cross-thread send safety) struck and marked resolved with `/private/tmp/sdl_net_ref/src/SDL_net.c:2015` citation. §Step 5 "If it fails" updated. |
| NEW-3 Hairpin gate normalization | **Addressed** | §Step 5 DELIVER handler uses `direct_p2p_ip_eq_normalized` (compares `inet_pton`-parsed `uint32_t`s); helper added to §Step 5 Changes. §Hard requirement 3(c) updated to "compare normalized — never `strcmp` on possibly-prefixed string forms." |
| NEW-4 Cancel semantics: `SDL_DetachThread` → `SDL_WaitThread` | **Addressed** | §Step 5 adds a "Cancel semantics" item: drop `SDL_DetachThread` at `direct_p2p.c:603-604 (host) and :644-645 (join)`; switch `DirectP2P_Cancel` from spin-for-IDLE to `SDL_WaitThread` then teardown. Eliminates the pre-existing race the bilateral path's longer worker lifetime would aggravate. Worker-lifetime note cites `stun.c:407-411` cancel honor and `stun.c:454` 10ms loop. |
| NEW-5 Step 1 lobby-server citations stale (directory empty) | **Addressed** | §Step 1 "Read first" drops the four `/tmp/3sxtra/tools/lobby-server/*` references; replaces with a note that the directory is empty as of 2026-04-26 and design comes from §Decision 2 directly. §Step 1 "Changes" replaces "same shape as `tools/lobby-server/deploy.sh`" with concrete systemd-unit spec. |
| NEW-6 DNS fast-fail in rendezvous resolve | **Addressed** | §Step 5 adds resolve-once-at-thread-start using the existing `NET_GetAddressStatus` polling pattern from `stun.c:272-275`; on resolve failure, host stays HOST_WAITING, joiner transitions to `FAILED_BILATERAL`. §Decision 7 Test 6 added. |
| NEW-7 Server rate-limit Map sweeper leak | **Addressed** | §Step 1 "Changes" rewrites the rate-limit line to specify a sliding-window counter swept every 60 s with the same TTL as the session map. |
| NEW-8 Em-dash → ASCII in status strings | **Addressed** | §Decision 9 status table replaces em-dashes with ASCII hyphens. New "Glyph constraint - ASCII only" subsection in §Decision 9 cites `direct_p2p.c:425, :434` precedent and `SSPutStrPro` glyph table. |
| NEW-9 Idle UX after rendezvous-thread budget | **Addressed** | §Decision 9 status table adds a row for post-budget HOST_WAITING. §Step 5 `host_rendezvous_thread_fn` bullet gains a "Post-budget status update" item. |
| NEW-10 Session-key zero-IP guard + pack semantics | **Addressed** | §Step 3 `Rendezvous_DeriveSessionKey` bullet adds explicit pack semantics (`payload[0..3] = ip_be`, `payload[4..5] = htons(public_port)`) and caller responsibilities (host MUST abort REGISTER if `ipv4_str_to_be(public_ip) == 0`). |
| NEW-11 §Decision 1 `Stun_EncodeEndpoint` claim unverifiable | **Addressed** | §Decision 1 (B-evaluation) softened: room_code is opaque from our POV; `tools/lobby-server/lobby-server.js:362` citation dropped (file missing). |
| NEW-12 Stale line-number citations | **Addressed** | `main.c` test dispatch corrected to `:937-1000`; `join_thread_fn` corrected to `:372-458`. (`Stun_CloseSocket` `stun.h:41` citation belongs to the review file, fixed there.) |

## Re-verification 2026-04-26 (round 2)

A third deep verification pass surfaced 19 additional findings (4 P-1, 7 P-2, 5 consistency, 3 nits). All addressed in this revision; per-item summary below cites the section that changed.

| Item | Disposition | Note |
|------|-------------|------|
| P-1.A Joiner-side hairpin gate missing | **Addressed in this round** | §Step 5 join_thread_fn fallback bullet extended to a three-way OR (kill-switch / LAN / `direct_p2p_ip_eq_normalized(s_work.peer_ip, s_work.stun.public_ip)`); §Hard requirement 3(c) updated to note the same compare runs symmetrically on host (DELIVER receive time) and joiner (post-STUN, pre-FALLBACK). |
| P-1.B Host/join label swap on `SDL_DetachThread` + three-thread cleanup | **Addressed in this round** | §Step 5 cancel-semantics bullet now cites `direct_p2p.c:603-604` (host, in `DirectP2P_BeginHost`) and `:644-645` (join, in `DirectP2P_BeginJoin`); spells out three-thread cleanup model (`s_thread`, `s_rendezvous_thread`, `s_bilateral_punch_thread` — coexist on host path during FALLBACK_BILATERAL_PUNCH). NEW-4 disposition row updated to match. Adds separate `s_bilateral_punch_cancel` atomic. |
| P-1.C `Rendezvous_Send` helper introduced as test seam | **Addressed in this round** | `Rendezvous_Send` is declared as a `static` helper in `direct_p2p.c` (per the SDL_net-purity constraint on `rendezvous.c`). §Step 3 forward-references it; §Step 5 introduces it next to the rendezvous queue/thread code; §Step 5 host_rendezvous_thread_fn bullet routes the main-thread drain through `Rendezvous_Send` and the queue tuple carries `(NET_RefAddress(target), payload[28])`; §Step 5 join-side bullet calls `Rendezvous_Send` directly (no queue needed). §Step 6 Test 4 description references the helper. |
| P-1.D Drop unverifiable `/tmp/3sxtra/src/netplay/lobby_server.{c,h}` citations | **Addressed in this round** | §Decision 1 B-evaluation block reframed: cites `~/.claude/projects/-Users-sb-Developer-3sx-mister/memory/project-netplay-port-strategy.md` instead of unreadable upstream files; explicitly notes `/tmp/3sxtra/` recursively cleared (zero `.c`/`.h` files anywhere underneath). Review file gains an append-only correction in "Things verified correct" section noting the lobby_server.c / sha256.{c,h} entries are no longer reproducible. |
| P-2.1 Concretize `s_rendezvous_send_q` spec | **Addressed in this round** | §Decision 3 queue paragraph rewritten with concrete spec: 8-slot SPSC ring; slot type carries `(NET_Address* target, uint16_t target_port, uint8_t payload[28], uint8_t payload_len)` with explicit `NET_RefAddress` / `NET_UnrefAddress` ownership rules; `SDL_AtomicInt` head/tail; producer drops on overflow with `s_q_drops` telemetry; drain rate up to 4 slots per tick. §Step 5 "If it fails" wording reconciled. |
| P-2.2 `stun.c:386` "same idiom" misclaim | **Addressed in this round** | §Step 5 host_rendezvous_thread_fn DNS-resolve bullet replaces the parenthetical with a correct note: `stun.c:385-389` uses a longer 3000ms variant for peer addresses; we want the 100ms server-hostname variant from `stun.c:272-275`. |
| P-2.3 Add `src/configuration.h` to Step 6 file list | **Addressed in this round** | §Step 6 "Changes" gains a bullet for `src/configuration.h` to add the `bool test_bilateral_punch` field at `:61-99` mirroring existing `test_*` fields. |
| P-2.4 Bilateral-punch thread local-copy discipline | **Addressed in this round** | §Step 5 host_bilateral_punch_thread_fn note: copy `s_work.peer_ip` / `s_work.peer_public_port` to stack-locals before `Stun_HolePunch` (mirror `direct_p2p.c:418-420` pattern) to avoid race with main-thread `do_handoff` reads against `Stun_HolePunch`'s in-place overwrites at `stun.c:438,442`. |
| P-2.5 Teardown blocking analysis | **Addressed in this round** | §Step 5 `direct_p2p_on_teardown` bullet rewritten to specify cancel-flag → `SDL_WaitThread` per non-NULL handle; documents worst-case ~510ms blocking (rendezvous between sends ~500ms + bilateral inside `Stun_HolePunch`'s 10ms loop) with a 1-second deadline fallback that logs+continues. |
| P-2.6 Step 1 systemd-unit completeness | **Addressed in this round** | §Step 1 systemd-unit description expanded to a full unit including `[Unit] After=network-online.target`, `[Service]` body, and `[Install] WantedBy=multi-user.target`. Without `[Install]` the deploy script's `systemctl enable` would fail. |
| P-2.7 Step 3 spurious dependency on Step 2 | **Addressed in this round** | §Step 3 "Depends on:" line changed from "Step 2" to "none"; clarified that Step 5 consumes the config key but Step 3's pure module takes the URL via `const char*` argument. |
| IC-1 / IC-2 `/tmp/3sxtra/` recursive empty state | **Addressed in this round** | Covered by P-1.D — both edits explicitly say the empty state covers `/tmp/3sxtra/` recursively, not just `tools/lobby-server/`. |
| IC-3 Reconcile drain-rate vs. queue-depth | **Addressed in this round** | Covered by P-2.1 — queue depth = 8, drain rate up to 4 per tick. §Step 5 "If it fails" line updated to drop "max 4 outstanding" wording in favor of the new spec. |
| IC-4 Strip Decision 3's revision metadata from operational text | **Addressed in this round** | §Decision 3 "Net effect" sentence rewritten: dropped "Decision 3's earlier wording … was incomplete" framing; corrected rule stands on its own. |
| IC-5 Worker-lifetime note covers join only | **Addressed in this round** | §Step 5 "Worker-lifetime note" gains a parallel sentence for the host worker (`direct_p2p.c:288, exits at :361-365`); confirms the host worker remains short-lived and that cancel/teardown must wait on `s_rendezvous_thread` and `s_bilateral_punch_thread` independently of `s_thread`. |
| Nit-1 tf-psa-crypto link-line citation | **Addressed in this round** | §Step 3 CMakeLists.txt update bullet now distinguishes path declaration (`:246`) from link line (`:278`). |
| Nit-2 Test URL reserved TLD | **Addressed in this round** | §Decision 7 Test 6 changed `udp://invalid.tld.example:3478` to `udp://invalid.example:3478` (RFC 2606 reserved TLD). |
| Nit-3 Original review's `Stun_HolePunch` / `Stun_CloseSocket` swap | **Addressed in this round** | Per append-only convention, original review entry untouched; appended one-line nit to review's existing "Re-verification 2026-04-26" section noting `Stun_CloseSocket` is at `stun.h:28` and `Stun_HolePunch` at `stun.h:41-42`. |

## Re-verification 2026-04-26 (round 4)

Round-4 cold-read review surfaced 3 P-1 inconsistencies and 6 P-2 cleanup items against the post-round-3 plan; all addressed in this revision. Item labels are P-1.A..C / P-2.A..F to distinguish from earlier rounds. The big structural change in this round is splitting Step 5 into 5a / 5b / 5c (three flat steps; no umbrella).

| Item | Disposition | Note |
|------|-------------|------|
| P-1.A `direct_p2p_overlay.c` cannot read `s_work.role`; access path unspecified | **Addressed in this round** | Added `Role DirectP2P_GetRole(void)` accessor to §Step 5a "Changes" (declared in `src/netplay/direct_p2p.h`, implemented in `src/netplay/direct_p2p.c` as a trivial snapshot returning `s_work.role`). §Decision 9 prose updated to specify `dp2p_overlay_mode_label` calls `DirectP2P_GetRole()` rather than reading `s_work.role` directly. New success-criterion grep added in §Step 5a (`grep -n 'DirectP2P_GetRole' src/netplay/direct_p2p.{c,h}` ≥ 2 lines). |
| P-1.B Stale "500ms grace window" reference | **Addressed in this round** | Struck "well under the existing 500ms grace window" clause from the worker-lifetime note; replaced with "well within the bounded `SDL_WaitThread` deadline (1 second per §Decision 3 / `direct_p2p_on_teardown`)". The 500ms grace loop was deleted by the round-2 cancel-semantics rework, so the framing was stale. |
| P-1.C Decision 5 host-side diagram inconsistent with §Step 5 host `FALLBACK_SIGNALING` Tick case | **Addressed in this round** | Applied Option B (host stays in `HOST_WAITING` during REGISTER/POLL): dropped the host-side `FALLBACK_SIGNALING` Tick case from §Step 5b; added clarifying prose to §Decision 5 that "`FALLBACK_SIGNALING` is joiner-only; the host's REGISTER/POLL phase happens while the state remains `HOST_WAITING` (rendezvous thread is parallel; main thread continues receiving via `host_tick_receive`)." The diagram already lacked a `HOST_WAITING → FALLBACK_SIGNALING` arrow, so Option B was self-consistent with the existing edges; the surrounding rendering was reordered separately per P-2.B (gates before defaults). |
| P-2.A Step 5 size — split into 5a / 5b / 5c | **Addressed in this round** | Step 5 replaced with three flat steps (no umbrella): 5a foundation (enums, helpers, accessor, lifecycle rework — no behavior change), 5b host fallback (rendezvous thread, send queue, DELIVER handler, bilateral-punch thread), 5c joiner fallback (inline REGISTER/POLL, hairpin gate, bilateral punch, status strings). Each substep has all 8 required fields. Steps 6/7/8 re-scoped to depend on 5c. Dependency graph and build matrix updated to reflect the split. |
| P-2.B Decision 5 diagram visual priority of gates over default arrows | **Addressed in this round** | Reordered the host-side and joiner-side transition tables so gated arrows appear before defaults within each origin state. Added a one-line annotation at the top of each cluster: "(gates checked first; default arrow fires if no gate matches)". |
| P-2.C Overlapping worker-lifetime notes at `:591` and `:595` | **Addressed in this round** | Consolidated into a single Worker-lifetime note in §Step 5c (the inheritor of the join-side text). The unique content from the old `:595` bullet (`stun.c:454` cite, `SDL_Delay(10)` framing) is folded in; the redundant standalone bullet is removed. The "500ms grace window" framing dropped per P-1.B during consolidation. |
| P-2.D Joiner-side hairpin gate firing-point description | **Addressed in this round** | Reworded §Step 5c (formerly §Step 5) joiner hairpin disjunct: "fires post-STUN-discovery, post-`Stun_HolePunch`-failure, before the FALLBACK_SIGNALING transition (so `s_work.stun.public_ip` is populated and the direct punch has already been attempted)." |
| P-2.E Writeback ordering line cite `:455-457` → `:456` | **Addressed in this round** | Corrected the `set_state(DIRECT_P2P_HANDOFF)` cite in §Step 5b's writeback-ordering bullet to `direct_p2p.c:456` (verified). |
| P-2.F Step 6 dispatch line cite "around 932+" → ":962-1000" | **Addressed in this round** | Corrected to ":962-1000" in §Step 6 main.c bullet (forward-decls begin at `:938`; the `if (configuration.test_*)` blocks start at `:962`). |
