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
 *      from the established peer IP at a NEW port. That is the S2
 *      joiner auto-retry: join_thread_fn's second attempt binds a FRESH
 *      socket (direct_p2p.c JOIN_MAX_ATTEMPTS), so its punches arrive
 *      from a new NAT mapping, and GekkoNet — which registers the
 *      remote once, by "ip:port" string, at configure time, with no
 *      relearn API (gekkonet.h has no address-mutating entry point) —
 *      would silently ignore every datagram it sends. The relearn is
 *      surfaced to netplay.c via LatePunch_TakeRelearn, which retargets
 *      the MIST peer address and the adapter's send/present mapping.
 *      GekkoNet itself is never patched and never sees the change.
 *
 * AUTHENTICATION IS NOT WEAKENED. Every action above is gated on the
 * exact 17-byte payload check Stun_IsPunchPayload performs
 * (constant-time token compare, stun.c). The relearn is STRICTER than
 * the pre-handoff host gate: host_tick_receive captures a valid-token
 * punch from ANY source as the peer, while this module only retargets
 * within the peer IP that the race/handoff already established —
 * a recorded-payload replay from a third-party address is consumed,
 * counted, and ignored. S4c return-routability (rendezvous cookies) is
 * upstream of this layer and untouched.
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
 * so the worst case is a few hundred 17-byte datagrams per session.
 * Kill switch: CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_LATE_PUNCH (checked
 * by the caller at arm time, not here — this module has no config
 * dependency so the natmatrix probe can drive it directly).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "netplay/stun.h" /* STUN_PUNCH_TOKEN_LEN / STUN_PUNCH_PAYLOAD_LEN */

struct NET_DatagramSocket;

/* Cadence of the keepalive/answer stream. 200 ms matches GekkoNet's own
 * SYNC_MSG_DELAY and the slow punch cadence (STUN_PUNCH_INTERVAL_SLOW_MS)
 * — fast enough that a retrying peer confirms within one of its punch
 * intervals, slow enough to be invisible next to the session traffic. */
#define LATE_PUNCH_TX_INTERVAL_MS 200u

/* A flapping source could otherwise bounce the retarget every packet.
 * 8 covers every legitimate sequence (one S2 retry moves the port ONCE;
 * a NAT rebind mid-connect adds one more) with a wide margin, and caps
 * the damage of a same-IP replayer at 8 send-target moves per session. */
#define LATE_PUNCH_MAX_RELEARNS 8

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

#endif /* NETPLAY_LATE_PUNCH_H */
