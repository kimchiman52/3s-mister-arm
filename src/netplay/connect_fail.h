/*
 * connect_fail.h — S3 of docs/plan-netplay-connection.md: connection-
 * establishment failure taxonomy.
 *
 * Before S3, >=13 distinct root causes collapsed into about two user-
 * facing strings ("Connection failed. Try again." / "Could not connect.
 * Try a different network."), which made field reports unactionable.
 * This module gives every distinguishable cause:
 *
 *   - a stable MACHINE CODE  (ConnectFail_Code — grep-stable, wire-safe
 *     for logs; append-only, never renumber/rename), and
 *   - a distinct USER STRING (ConnectFail_UserText — sized for the
 *     384px overlay status line, <= ~44 chars).
 *
 * The classifiers are PURE functions over evidence the client already
 * receives (and used to throw away):
 *
 *   cause                          | detection evidence
 *   -------------------------------+----------------------------------
 *   DNS dead / no network          | every getaddrinfo failed AND zero
 *                                  | STUN responses (StunResult diag)
 *   STUN blocked                   | sends went out, zero responses
 *   rendezvous server down         | ZERO DELIVER frames for the whole
 *                                  | signaling budget (the server
 *                                  | answers EVERY REGISTER with a
 *                                  | DELIVER — real endpoint or the
 *                                  | 0.0.0.0:0 sentinel — verified in
 *                                  | tools/rendezvous-server/
 *                                  | rendezvous-server.js handleRegister:
 *                                  | "Reply to source with the OTHER
 *                                  | endpoint (or zeroes)"; silence
 *                                  | therefore means server/path down)
 *                                  | HONESTY NOTE (review L-3): a live
 *                                  | server SILENTLY DROPS a REGISTER in
 *                                  | four real cases, all of which this
 *                                  | inference then misfiles as "server
 *                                  | down": (1) the 10 pkt/s/IP rate
 *                                  | limiter (rendezvous-server.js
 *                                  | onMessage) — shared/CGNAT egress IPs
 *                                  | pool that budget across users;
 *                                  | (2) the MAX_NEW_KEYS_PER_IP live-key
 *                                  | quota — reachable by one joiner
 *                                  | retrying >= 5 DISTINCT stale codes
 *                                  | inside the TTL, since each stale
 *                                  | attempt CREATES a key; (3) session
 *                                  | table full of PAIRED sessions;
 *                                  | (4) third-party REGISTER on a fully
 *                                  | paired key. All four are rare and
 *                                  | none is distinguishable client-side
 *                                  | today (a distinct NACK needs a wire
 *                                  | change — S4/S5 territory), so
 *                                  | RENDEZVOUS_DOWN remains the honest
 *                                  | best guess, not a certainty.
 *   host offline / code stale      | DELIVERs arrived but ALL were the
 *                                  | zero-sentinel for the whole budget
 *   host online, NAT-blocked       | a real-endpoint DELIVER arrived,
 *                                  | then the bilateral punch timed out
 *   symmetric-both / needs relay   | as above + StunResult.
 *                                  | port_disagreement (S2 signal)
 *   hairpin / no NAT loopback      | peer public IP == our public IP
 *   host-side router blocks        | host: no UPnP mapping AND no
 *   hosting                        | inbound AND no DELIVER (advisory)
 *   peer rejected (version)        | MIST handshake reject (R-1 path)
 *   timeout at stage N             | S3 deadlines (Part A), stage named
 *
 * No SDL / SDL_net dependencies: everything here is unit-testable with
 * plain ints and structs (exercised from test_bilateral_punch.c and
 * test_stun_mock.c).
 */
#ifndef NETPLAY_CONNECT_FAIL_H
#define NETPLAY_CONNECT_FAIL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ConnectFailCode {
    CONNECT_FAIL_NONE = 0,

    /* Joiner + host discovery failures */
    CONNECT_FAIL_DNS_ALLDOWN,     /* every getaddrinfo failed, no STUN reply  */
    CONNECT_FAIL_STUN_ALLDOWN,    /* sends went out, zero STUN responses      */

    /* Joiner fallback-signaling failures */
    CONNECT_FAIL_RENDEZVOUS_DOWN, /* zero DELIVERs — server/path down         */
    CONNECT_FAIL_HOST_OFFLINE,    /* only zero-sentinel DELIVERs — code stale */
    CONNECT_FAIL_NAT_BLOCKED,     /* real DELIVER, bilateral punch timed out  */
    CONNECT_FAIL_SYMMETRIC_BOTH,  /* as NAT_BLOCKED + port_disagreement       */
    CONNECT_FAIL_HAIRPIN,         /* peer public IP == ours, no NAT loopback  */
    CONNECT_FAIL_PUNCH_AUTH,      /* S4a: peer punched with a bad/missing token
                                     — build mismatch (old peer) or mismatched
                                     room payload; connectivity was fine       */

    /* Host-side advisory (state stays HOST_WAITING) */
    CONNECT_FAIL_HOST_UNMAPPABLE, /* no UPnP + no inbound + no DELIVER        */

    /* Post-handoff / session-gate failures */
    CONNECT_FAIL_PEER_REJECTED,     /* MIST handshake reject (reason text carries detail) */
    CONNECT_FAIL_TIMEOUT_CONNECTING,/* NETPLAY_SESSION_CONNECTING deadline hit  */
    CONNECT_FAIL_TIMEOUT_ORCHESTRATOR, /* nav NAV_WAIT_ORCHESTRATOR deadline    */
    CONNECT_FAIL_USER_ABORT,        /* user held START through the abort window */

    /* Local errors */
    CONNECT_FAIL_INVALID_CODE,    /* room code failed to decode               */
    CONNECT_FAIL_INTERNAL,        /* thread spawn / packet build / config     */
} ConnectFailCode;

/* Stable machine code string, e.g. "P2P_FAIL_STUN_ALLDOWN". Never NULL.
 * These are log-grep anchors: append-only, never rename. */
const char* ConnectFail_Code(ConnectFailCode code);

/* Distinct user-facing string for the overlay status line (<= ~44 chars
 * so SSPutStrPro centering on the 384px canvas fits). Never NULL.
 * CONNECT_FAIL_PEER_REJECTED returns a generic string — callers pass
 * the MIST reject reason text through instead when they have it. */
const char* ConnectFail_UserText(ConnectFailCode code);

/* --- classifiers ------------------------------------------------------- */

/* Classify a failed Stun_Discover from its diagnostic counters
 * (StunResult.diag_* — filled by stun.c on success AND failure):
 *   - dns_all_failed && answered==0        -> DNS_ALLDOWN (no net / DNS dead)
 *   - probed>0 && sends_ok>0 && answered==0 -> STUN_ALLDOWN (UDP filtered)
 *   - anything else with answered==0        -> DNS_ALLDOWN (no net at all —
 *     nothing was even probed/sent)
 *   - answered>0                            -> NONE (not a discovery failure)
 */
ConnectFailCode ConnectFail_ClassifyStunDiscover(int servers_probed,
                                                 int servers_answered,
                                                 int sends_ok,
                                                 bool dns_all_failed);

/* Evidence collected across one joiner attempt (direct punch + fallback
 * signaling + bilateral punch). See join_attempt() in direct_p2p.c. */
typedef struct ConnectJoinEvidence {
    bool hairpin;            /* peer public IP == our public IP */
    bool deliver_any;        /* >=1 DELIVER frame received (incl. zero-sentinel) */
    bool deliver_real;       /* >=1 DELIVER carried a real peer endpoint */
    bool bilateral_punched;  /* bilateral Stun_HolePunch succeeded */
    bool port_disagreement;  /* StunResult.port_disagreement (S2 symmetric signal) */
    bool punch_bad_token;    /* S4a: StunResult.diag_punch_bad_token — peer spoke
                                the punch protocol but failed the token check */
} ConnectJoinEvidence;

/* Classify a failed joiner fallback. Precedence: hairpin > rendezvous
 * silence > host absent > punch failure (bad-token evidence first —
 * the peer was REACHED but failed auth, so NAT is exonerated — then
 * symmetric vs plain NAT block).
 * Returns NONE when the evidence says the attempt succeeded. */
ConnectFailCode ConnectFail_ClassifyJoin(const ConnectJoinEvidence* ev);

/* Host-side advisory classifier, evaluated periodically while
 * HOST_WAITING. The rendezvous server answers every REGISTER with a
 * DELIVER (zero-sentinel when unpaired), so a host that has been
 * re-REGISTERing for `waited_ms` with zero DELIVERs has a dead server
 * path; combined with no UPnP mapping (and no inbound punch — implied
 * by still being in HOST_WAITING) the room is likely unjoinable.
 * Returns:
 *   HOST_UNMAPPABLE  — !upnp && !deliver_any after the threshold
 *   RENDEZVOUS_DOWN  —  upnp && !deliver_any after the threshold
 *                       (direct joins still work; log-only advisory)
 *   NONE             — otherwise. */
#define CONNECT_HOST_ADVISORY_MS 30000u
ConnectFailCode ConnectFail_ClassifyHostWaiting(bool upnp_active,
                                                bool deliver_any,
                                                uint32_t waited_ms);

/* --- deadline / abort policy helpers (Part A) -------------------------- */

/* Wall-clock budget for NETPLAY_SESSION_CONNECTING (GekkoNet sync).
 * GekkoNet's own DISCONNECT_TIMEOUT applies only to actors already
 * Connected; an actor stuck Initiating retries SendSyncRequest forever,
 * so this is OUR bound on the whole CONNECTING state. */
#define CONNECT_TIMEOUT_CONNECTING_MS 15000u

/* Frames the user must hold START to abort a stuck connection attempt
 * (~3 s at 60 fps — long enough that menu mashing can't trigger it). */
#define CONNECT_ABORT_HOLD_FRAMES 180

/* Wrap-safe elapsed check: true when now-since >= budget. `since` == 0
 * means "not armed" and never expires. */
bool ConnectFail_DeadlineExpired(uint64_t now_ms, uint64_t since_ms,
                                 uint32_t budget_ms);

/* Consecutive-hold counter step: returns the new held-frame count.
 * Release resets to 0. */
int ConnectFail_AbortHoldTick(int held_frames, bool button_down);

/* True once the hold counter has crossed the abort threshold. */
bool ConnectFail_AbortHoldFired(int held_frames);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_CONNECT_FAIL_H */
