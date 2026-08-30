#ifndef NETPLAY_LATE_PUNCH_H
#define NETPLAY_LATE_PUNCH_H

/*
 * late_punch — task #119: make disagreement cheap instead of impossible.
 *
 * THE CAPABILITY THIS ADDS. The p2p race can end one-sided: one peer
 * confirms its punch and hands the socket to the netplay session while
 * the other peer's race exhausts (the residual bands: >600 ms RTT pairs
 * past RACE_PUNCH_SETTLE_MS, the asymmetric path §8.10 concedes no grace
 * can reach, and any band not yet discovered). Pre-#119 the connected
 * side then sat out CONNECT_TIMEOUT_CONNECTING_MS (or the MIST retry
 * window) talking to a peer that had already torn down, parked in
 * FAILED_HANDSHAKE, and the room code was burned — the nonce regenerates
 * per hosting attempt (direct_p2p.c, s_work.nonce), so there was no way
 * back. Four prior mechanisms (the settle window, the H-1 confirm tail,
 * the second tail, the host M1 loop) each shrank the disagreement band;
 * none could close it, because commit/abort agreement over a lossy
 * channel is Two Generals. This module stops trying to prevent the
 * disagreement and makes it CHEAP instead: the connected side keeps
 * speaking the authenticated punch protocol until the session actually
 * starts, so a peer that aborted and retried can still find it, and the
 * failure degrades from "15 s hang + dead room" to "delayed connect".
 *
 * WHAT IT DOES, concretely, between handoff and GekkoSessionStarted:
 *   1. keeps SENDING the 17-byte authenticated punch payload
 *      ("3SX_PUNCH" + the 8-byte room-code-derived token, stun.h) to the
 *      current peer endpoint at a low cadence. Receiving our payload is
 *      how the peer's race confirms (Stun_PunchOffer accepts the
 *      byte-identical payload from the expected IP), so this stream IS
 *      the late answer — the punch protocol has no separate answer
 *      frame, an echo and a punch are the same bytes;
 *   2. CONSUMES inbound punch payloads offered to it by the two receive
 *      paths that own the socket post-handoff (the MIST pump drain and
 *      SDLNetAdapter's receive_data), so they never reach GekkoNet as
 *      garbage and a valid one is answered promptly; and
 *   3. RELEARNS the peer's source port when a token-valid punch arrives
 *      from the established peer IP at a NEW port AND that port answers
 *      a liveness challenge (see below). That is the S2 joiner
 *      auto-retry: join_thread_fn's second attempt binds a FRESH socket
 *      (direct_p2p.c JOIN_MAX_ATTEMPTS), so its punches arrive from a new
 *      NAT mapping, and GekkoNet — which registers the remote once, by
 *      "ip:port" string, at configure time, with no relearn API
 *      (gekkonet.h has no address-mutating entry point) — would silently
 *      ignore every datagram it sends. The relearn is surfaced to
 *      netplay.c via LatePunch_TakeRelearn, which retargets the MIST peer
 *      address and the adapter's send/present mapping. GekkoNet itself is
 *      never patched and never sees the change; and
 *   4. ANSWERS a liveness challenge sent to it, because the peer runs
 *      this same check in the other direction (late_punch.c:242-249, and
 *      Stun_PunchOffer for a peer still racing its punch).
 *
 * WHAT THE RELEARN GATE IS, AND THE HOLE IT USED TO HAVE (task #133).
 * Every action above is gated on the exact 17-byte payload check
 * Stun_IsPunchPayload performs (constant-time token compare, stun.c), and
 * S4c return-routability (rendezvous cookies) is upstream of this layer
 * and untouched. Two properties of that gate matter, and until #133 the
 * second one was load-bearing in the wrong direction:
 *
 *   NARROWER ON SOURCE IP than the pre-handoff host gate.
 *   classify_host_datagram (direct_p2p.c:1065) accepts a valid-token
 *   punch from ANY source as the peer; here a datagram is only ever
 *   considered when strcmp(src_ip, s_peer_ip) == 0 (late_punch.c:280),
 *   so a replay from a third-party ADDRESS is consumed, counted, ignored.
 *
 *   WIDER IN TIME. The pre-handoff gate is reachable only from
 *   DIRECT_P2P_HOST_WAITING (direct_p2p.c:5748-5761), so once the host
 *   commits to a peer, a losing racer normally gets NO second attempt.
 *   This module is the only path that grants one, for the whole
 *   pre-session window (up to 40 x 500 ms of MIST retries,
 *   mist_handshake.h:250, plus CONNECT_TIMEOUT_CONNECTING_MS = 15 s,
 *   connect_fail.h:359).
 *
 *   AND AN IP-STRING COMPARE CANNOT SEPARATE THE TWO CASES IT MUST.
 *   "the peer's NAT mapping moved" and "a different host behind the same
 *   public IP" are byte-identical at late_punch.c:280. The token does not
 *   separate them either: it is derived from the room code alone
 *   (SHA-256("3SXR-PT3" || ip || port || nonce)[0..7], rendezvous.c:146-152,
 *   nonce carried in the code's low 32 bits, room_code.h:233-236), so
 *   anyone holding the code can produce it without observing a packet.
 *   That is by design and cannot be fixed here; a same-IP code holder is
 *   inside the token's trust boundary. Nothing downstream narrows it
 *   either — the MIST compat gate (classify_peer_payload,
 *   mist_handshake.c:314) checks only build/ROM constants (arch tag,
 *   platform tag, proto_ver, state_ver, and the ROM balance digest
 *   (ArcadeBalance_Init sets it, arcade_balance.c:112; ArcadeBalance_GetDigest
 *   reads it, :221; build_hash warns, never rejects), and GekkoNet trusts only
 *   the address the adapter already rewrote (sdl_net_adapter.c:288-290, :405).
 *
 * SO WHAT #133 CHANGED. Pre-#133 a single token-valid datagram from a new
 * port WAS the relearn: send it and every outbound session datagram
 * followed you, permanently. That is a session takeover, not a broken
 * connect, and eight of them from eight ports made it deterministic (see
 * LATE_PUNCH_MAX_RELEARNS below). Two changes, together:
 *
 *   THE BUDGET IS ONE, AND ONLY A VERIFIED MOVE SPENDS IT. First verified
 *   mover wins instead of last mover wins, and an endpoint that merely
 *   sends cannot burn the budget or freeze the target.
 *
 *   A NOMINATED PORT MUST ANSWER AN UNPREDICTABLE CHALLENGE BEFORE IT
 *   BECOMES THE SEND TARGET. A token-valid punch from a new port now only
 *   NOMINATES (late_punch.c:304 -> :95). We draw 8 CSPRNG bytes, send
 *   them to the nominee as a 26-byte probe frame (STUN_PUNCH_PROBE_*,
 *   stun.h) and retarget only on a RESPONSE that comes back from that
 *   exact port carrying that exact nonce (late_punch.c:256-258). No
 *   CSPRNG, no challenge, no relearn — fail closed (late_punch.c:116-124).
 *
 *   WHY THE NONCE, AND NOT "a second punch". The token is public to a
 *   code holder, so any check made of things they can compute is a slower
 *   version of the same hole. The nonce is the one value in the exchange
 *   the nominee did not choose and cannot derive from the room code: to
 *   produce it you must have RECEIVED our datagram at the address you
 *   claim. That kills the off-path and blind-spoofed cases outright (a
 *   forged source IP/port that cannot receive can never answer), and it
 *   turns the same-IP case from "fire one datagram, own the session" into
 *   "hold a live socket, win the race against the real peer's own retry,
 *   and complete a round trip inside LATE_PUNCH_CHALLENGE_MS".
 *
 *   WHAT IT DOES NOT DO, stated plainly: a same-public-IP party holding
 *   the room code and running this build can still answer, so it can
 *   still win the single relearn if it gets there first. Raising the cost
 *   is the goal; the token's trust boundary is the room code and cannot
 *   be narrowed from inside this module. See docs/queue.md #133.
 *
 * THE PROMISED LISTENING INTERVAL (direct_p2p.c) is not given a fifth
 * enforcement site by this module because this module cannot end,
 * replace, or re-arm a race leg: it arms only AFTER do_handoff, i.e.
 * after the race returned and every leg was finished by its own rules.
 * It only ever ADDS listening time after the race's promises have been
 * kept in full.
 *
 * THREADING. Main thread only, like the socket it borrows: every caller
 * (Netplay_TickDirectP2P, Netplay_Run's TRANSITIONING/CONNECTING cases,
 * mist_netio_recv, SDLNetAdapter receive_data) runs on the main thread.
 * The module never owns the socket and never closes it.
 *
 * BOUNDED. Armed for at most one pre-session window (MIST retry window
 * + CONNECT_TIMEOUT_CONNECTING_MS); disarmed at GekkoSessionStarted and
 * at session teardown. The send cadence is LATE_PUNCH_TX_INTERVAL_MS,
 * so the worst case is a few hundred 17-byte datagrams per session, plus
 * the probe traffic: at most one challenge in flight at a time (so a
 * flood of nominations cannot multiply it) and at most one parked echo
 * per Tick.
 * Kill switch: CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_LATE_PUNCH (checked
 * by the caller at arm time, not here — this module has no config
 * dependency so the natmatrix probe can drive it directly).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "netplay/stun.h" /* STUN_PUNCH_TOKEN_LEN / _PAYLOAD_LEN / _PROBE_* */

struct NET_DatagramSocket;

/* Cadence of the keepalive/answer stream. 200 ms matches GekkoNet's own
 * SYNC_MSG_DELAY and the slow punch cadence (STUN_PUNCH_INTERVAL_SLOW_MS)
 * — fast enough that a retrying peer confirms within one of its punch
 * intervals, slow enough to be invisible next to the session traffic. */
#define LATE_PUNCH_TX_INTERVAL_MS 200u

/* THE RELEARN BUDGET (task #133 mitigation). ONE.
 *
 * What it used to be, and why that was backwards. At 8 the cap read as
 * "bound the damage of a flapping source to 8 moves", and it did the
 * opposite. One move is already the whole capability — a single relearn
 * retargets every outbound session datagram (sdl_net_adapter.c:288-290)
 * and presents the new endpoint under the canonical string on the way in
 * (:405), and it survives into the match, because LatePunch_Disarm at
 * GekkoSessionStarted (netplay.c:1798) clears this module's state and not
 * the adapter's. So the cap never bounded damage; and once it was reached
 * the endpoint FROZE and every later move was refused, which made a
 * same-IP capture deterministic rather than a contest: eight token-valid
 * punches from eight source ports left the send target on the attacker's
 * eighth port with no way for the real peer to be relearned back.
 *
 * What it is now. The proven rescue needs exactly ONE move (a real
 * FAILED_HANDSHAKE became a connect at +11.4 s, docs/queue.md:225), so
 * one is what it gets: first VERIFIED mover wins, instead of last mover
 * wins. And the budget is spent by late_punch_promote(), i.e. only by a
 * candidate that answered the liveness challenge below — an endpoint
 * that merely SENDS cannot consume it. That distinction is what stops
 * "cap of 1" from becoming a one-datagram freeze: an unanswered
 * nomination expires and the real peer can still be relearned after it.
 *
 * The cap therefore bounds CAPABILITY (how many times this module may
 * ever move the send target in one pre-session window), not flapping,
 * and it no longer pins the target to whoever spoke last. */
#define LATE_PUNCH_MAX_RELEARNS 1

/* How long a nominated endpoint has to answer its challenge, and how
 * often the challenge is repeated inside that window. 1500 ms covers the
 * >600 ms RTT residual band with a retransmit to spare; the retransmit
 * cadence matches the punch protocol's slow interval. One candidate is
 * held at a time, so the total challenge traffic for a pre-session window
 * is bounded by (window / LATE_PUNCH_CHALLENGE_MS) x (1500/200) 26-byte
 * datagrams. */
#define LATE_PUNCH_CHALLENGE_MS 1500u
#define LATE_PUNCH_CHALLENGE_RETX_MS 200u

/* Arm for the pre-session window. `sock` is BORROWED (owned by
 * netplay.c); `token` is the S4a punch token both sides derived from the
 * room-code payload; `peer_ip:peer_port` is the endpoint do_handoff
 * committed. Re-arming replaces the previous state. */
void LatePunch_Arm(struct NET_DatagramSocket* sock,
                   const uint8_t token[STUN_PUNCH_TOKEN_LEN],
                   const char* peer_ip, uint16_t peer_port);

/* Idempotent. Drops the borrowed socket pointer and the resolved peer
 * address; safe to call with the module already disarmed. */
void LatePunch_Disarm(void);

bool LatePunch_IsArmed(void);

/* Offer one inbound datagram. Returns true if the datagram was a punch
 * payload (valid token or punch-shaped garbage) and is CONSUMED — the
 * caller must not forward it to MIST or GekkoNet. Returns false for
 * everything else, including when disarmed. May raise the pending
 * relearn and the prompt-answer flag; sends nothing itself. */
bool LatePunch_HandleDatagram(const uint8_t* buf, int len,
                              const char* src_ip, uint16_t src_port);

/* Drive the send side: at most one 17-byte payload per call, cadenced,
 * or immediately when HandleDatagram flagged a prompt answer. */
void LatePunch_Tick(uint32_t now_ms);

/* One-shot: true exactly once per relearn, copying the NEW peer endpoint
 * out. The caller applies it to its own send paths (MIST peer address,
 * adapter retarget). */
bool LatePunch_TakeRelearn(char* ip_out, size_t ip_cap, uint16_t* port_out);

#ifdef ENABLE_NETPLAY_TESTS
/* Task #132, test-only: the current send target, the pending prompt-answer
 * flag, and the relearn counter. Read-only; see the comment on the
 * definition in late_punch.c for why the send target has to be observable
 * without a socket. Any pointer may be NULL. */
void LatePunch_TestPeek(char* ip_out, size_t ip_cap, uint16_t* port_out,
                        bool* tx_prompt_out, int* relearn_count_out);

/* Task #133, test-only: the pending liveness challenge (whether one is
 * open, which port it was sent to, and its nonce) and the parked echo.
 * The nonce is handed out so a test can forge a CORRECT response — the
 * only way to pin that a live peer IS still relearnable. Read-only; any
 * pointer may be NULL. */
void LatePunch_TestPeekChallenge(bool* active_out, uint16_t* port_out,
                                 uint8_t nonce_out[STUN_PUNCH_PROBE_NONCE_LEN],
                                 bool* echo_pending_out,
                                 uint16_t* echo_port_out);
#endif

#endif /* NETPLAY_LATE_PUNCH_H */
