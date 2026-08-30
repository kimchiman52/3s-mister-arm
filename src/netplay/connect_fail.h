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
 *   symmetric-both (our NAT        | as above + StunResult.
 *   reassigns ports)               | port_disagreement (S2 signal).
 *                                  | TERMINAL: the relay rung that used
 *                                  | to follow it was removed, so the
 *                                  | remedy this reports is the HOST
 *                                  | forwarding a port (see the user
 *                                  | strings in connect_fail.c)
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
    CONNECT_FAIL_RENDEZVOUS_DOWN, /* zero DELIVERs AND zero CHALLENGEs —
                                     server/path down                          */
    CONNECT_FAIL_COOKIE_REJECTED, /* S4c: server CHALLENGEd us (alive!) and
                                     RE-challenged after we echoed a cookie —
                                     the echo never verified: auth / version
                                     trouble, not connectivity                 */
    /* Task #105. Split out of COOKIE_REJECTED, which used to absorb every
     * "challenged but never DELIVERed" outcome and then told the user
     * "Matchmaking auth failed. Update the game."
     *
     * That inference was unsound. The server challenges ONLY when
     * cookieValid() fails, and the joiner carries the cookie on every
     * later REGISTER, so an ACCEPTED cookie produces exactly ONE challenge
     * per source port and then silence-or-DELIVERs. Seeing one challenge
     * and zero DELIVERs therefore does NOT mean the cookie was refused —
     * it means the cookie BOUND and we were never paired. Measured causes:
     * the DELIVERs were lost in transit, or the server dropped our
     * correctly-cookied REGISTER for a non-auth reason (a full session
     * slot, the per-IP cookied bucket, the per-key cap).
     *
     * Telling a user to update the game because a datagram was lost costs
     * us the bug report, which is what #44's tester log depends on. */
    CONNECT_FAIL_RENDEZVOUS_NOPAIR, /* server alive, our REGISTER was
                                       accepted (cookie bound, no
                                       re-challenge), yet zero DELIVERs   */
    CONNECT_FAIL_HOST_OFFLINE,    /* only zero-sentinel DELIVERs — code stale */
    CONNECT_FAIL_NAT_BLOCKED,     /* real DELIVER, bilateral punch timed out  */
    /* Both of these are TERMINAL. The S5 relay rung that once followed
     * them was deleted, so there is no further rung to try and the user
     * string has to carry the remedy itself.
     *
     * The remedy the EVIDENCE supports is the HOST forwarding a port,
     * not the joiner enabling UPnP: try_portmap() has exactly one call
     * site, inside host_thread_fn (direct_p2p.c), so the joiner never
     * attempts UPnP/NAT-PMP at all — and reaching either of these codes
     * requires a real-endpoint DELIVER, i.e. the host's advertised
     * endpoint (carrying its port mapping when it has one) was already
     * tried and failed.
     *
     * SYMMETRIC_BOTH is distinguished from NAT_BLOCKED solely by
     * ConnectJoinEvidence.port_disagreement — our OWN StunResult signal
     * that our router handed different external ports to different STUN
     * servers. That is the one thing measured about our own side, hence
     * "your NAT reassigns ports". */
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
    /* S4-review L-4: the room code is a valid but WRONG-VERSION format —
     * the two builds must match; NOT a typo. Split into two machine
     * codes because "older" and "newer" send the two ends of a failed
     * pairing to OPPOSITE actions: with _OLDER the code's CREATOR needs
     * to update, with _NEWER the person typing it does. Collapsed into
     * one code, log triage could not tell which side was stale — which
     * is the entire question a support thread has to answer. */
    CONNECT_FAIL_CODE_VERSION_OLDER, /* v1 (11-char) or v2 (14-char) code:
                                        the code's CREATOR is behind       */
    CONNECT_FAIL_CODE_VERSION_NEWER, /* current length, unrecognized version
                                        char: WE are behind (or the code is
                                        corrupt)                           */
    CONNECT_FAIL_INTERNAL,        /* thread spawn / packet build / config     */
    CONNECT_FAIL_BALANCE_UNAVAILABLE, /* arm-time refusal: this build could not
                                     reach verified-arcade balance (CPS3 ROM
                                     absent, or the 20-character adaptation
                                     did not fully succeed), and netplay arms
                                     ONLY in that state — see
                                     Netplay_ArmAllowed. A local environment
                                     fact, NOT an internal error and NOT a
                                     connectivity failure: nothing was ever
                                     sent.                                    */

    /* Append new codes ABOVE this marker and bump the bound in
     * test_bilateral_punch.c test 7f (which sweeps NONE..LAST proving
     * every code has a distinct machine string).
     *
     * NOTE: the four CONNECT_FAIL_RELAY_* codes that used to sit here
     * were deleted with the relay rung. They were the LAST entries, so
     * their removal renumbered nothing — every surviving code keeps the
     * numeric value it had, which is what the append-only rule at the
     * top of this file protects. */
    CONNECT_FAIL_LAST_ = CONNECT_FAIL_BALANCE_UNAVAILABLE,
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
    bool challenge_any;      /* S4c: >=1 CHALLENGE frame received — the server
                                is provably alive even with zero DELIVERs */
    /* Task #105: a CHALLENGE arrived while we ALREADY held a cookie for
     * this race, i.e. we echoed one and were challenged again. This is the
     * only on-wire proof that the echo did not verify, and it is what
     * separates a genuine cookie rejection from "cookie accepted, never
     * paired". It needs no threshold: the server stops challenging the
     * moment cookieValid() succeeds, and a cookie cannot expire inside one
     * race (it is honoured for the current AND previous 60 s rotation
     * slot, while the race budget is at most 15 s). */
    bool cookie_rechallenged;
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
/* S4c: `challenge_any` — the server CHALLENGEd us at least once, so it
 * is alive; zero DELIVERs then means our cookie echo never bound
 * (COOKIE_REJECTED advisory) rather than a dead server. */
ConnectFailCode ConnectFail_ClassifyHostWaiting(bool upnp_active,
                                                bool deliver_any,
                                                bool challenge_any,
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

/* --- #36: attribution confidence ---------------------------------------
 *
 * #36: does the recorded evidence actually SUPPORT the classified code, or
 * is the attribution ambiguous? A log that confidently blames the wrong
 * leg is worse than no log (the H-1 misattribution).
 *
 * This is a SECOND field beside the code, never a replacement for it: it
 * DOES NOT change the returned ConnectFailCode, does not change any
 * existing classifier, and does not change any user-facing string. Every
 * existing test keeps passing unchanged. */
typedef enum ConnectAttribution {
    CONNECT_ATTRIB_OK = 0,          /* success                               */
    CONNECT_ATTRIB_SUPPORTED,       /* evidence supports the code            */
    CONNECT_ATTRIB_AMBIG_CONFIRM,   /* NAT blamed, but a punch DID confirm   */
    CONNECT_ATTRIB_AMBIG_VERSION,   /* rendezvous silent: dead vs version-skew*/
    /* NOT ambiguous — the opposite. The server answered with a '3SXR'
     * frame carrying a protocol version this build cannot parse, so the
     * silence that produced RENDEZVOUS_DOWN has a MEASURED cause. The
     * governing principle cuts both ways: where the evidence is
     * genuinely ambiguous say so, and where it is definite do not
     * pretend otherwise. */
    CONNECT_ATTRIB_VERSION_SKEW
} ConnectAttribution;

/* Pure. Two pieces of race evidence, and the rule order between them
 * matters (see the implementation):
 *
 *   `race_confirm_seen` — RaceResult.confirm_seen carried out through
 *      s_work: "a punch leg reached CONFIRMED at some point during the
 *      race", including confirms that never settled (those are precisely
 *      the ones the race discards).
 *   `race_badver_n` — RaceResult.badver_n: how many '3SXR' frames FROM
 *      THE RENDEZVOUS ENDPOINT carried a protocol version we do not
 *      speak. Source-gated in p2p_race precisely because it is
 *      load-bearing here; an ungated counter would let an off-path
 *      spoofer manufacture the definite verdict below. */
ConnectAttribution ConnectFail_Attribute(ConnectFailCode code,
                                         bool race_confirm_seen,
                                         unsigned race_badver_n);
/* Stable machine string for logs: "ok" / "supported" /
 * "ambiguous-punch-confirmed" / "ambiguous-rendezvous-silent" /
 * "version-skew". Never NULL. */
const char* ConnectFail_AttributionText(ConnectAttribution a);
/* Human sentence explaining what the code alone does not say — WHY the
 * attribution is ambiguous, or (VERSION_SKEW) what the measured cause
 * actually is. "" when there is nothing to add (OK / SUPPORTED).
 * Never NULL. */
const char* ConnectFail_AttributionNote(ConnectAttribution a);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_CONNECT_FAIL_H */
