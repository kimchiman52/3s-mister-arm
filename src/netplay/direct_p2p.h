/*
 * direct_p2p.h — Step 7 of docs/plan-stun-direct-p2p.md.
 *
 * Asymmetric Host/Join orchestrator for internet direct-P2P sessions.
 * Owns the end-to-end flow from a menu press through STUN discovery,
 * UPnP first-try, room-code display/decode, hole-punch, and socket
 * handoff into netplay.c via Netplay_SetStunSocket + Netplay_BeginDirectP2P.
 *
 * Ordering differs from upstream (user locked-decision #2): UPnP-first,
 * STUN hole-punch as fallback. Upstream runs STUN first and falls back
 * to UPnP; we invert so UPnP-success pairs never touch the public STUN
 * servers.
 *
 * Lobby-unaware (locked decision #1) — this module does not use the
 * matchmaking/lobby server at all. The room code itself is the sole
 * out-of-band exchange mechanism.
 *
 * Lifecycle:
 *   DirectP2P_Init()              — register teardown callback once.
 *   DirectP2P_BeginHost(port)     — Host button press.
 *     Thread runs: UPnP probe (skippable) -> STUN discover -> build and
 *     publish room code -> HOST_WAITING. Tick() polls non-blockingly on
 *     the STUN socket for the first valid inbound datagram; on receipt
 *     hands the socket to netplay.c and enters HANDOFF.
 *   DirectP2P_BeginJoin(code)     — Join button press (after code entry).
 *     Thread runs: decode code -> STUN discover -> Stun_HolePunch
 *     (hairpin-aware) -> hand socket to netplay.c -> HANDOFF. On a
 *     successful Join the code is persisted back to config under
 *     CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE.
 *   DirectP2P_Cancel()            — user aborts; sets the atomic cancel
 *     flag, waits for the worker, drops any UPnP mapping, destroys any
 *     unhanded-off STUN socket, resets to IDLE.
 *   DirectP2P_Tick()              — called once per frame from main.c
 *     alongside the other netplay ticks. Never blocks.
 *
 * Thread model:
 *   Main thread only reads SDL_AtomicInt state, reads const status
 *   strings, and does the final socket handoff. All SDL3_net / miniupnpc
 *   calls run on the worker thread (SDL_CreateThread). The worker
 *   publishes state transitions via SDL_SetAtomicInt and populates the
 *   status-text buffer under a short-lived snprintf — writes are
 *   coarse-grained so a stale-read from main thread is at worst one
 *   frame of visual lag.
 *
 * Config keys (from Step 5):
 *   CFG_KEY_NETPLAY_DIRECT_P2P_HOST_PORT        — passed to BeginHost.
 *   CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP     — skip UPnP first-try.
 *   CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS  — STUN discovery clamp.
 *   CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE   — written on Join success.
 */

#ifndef NETPLAY_DIRECT_P2P_H
#define NETPLAY_DIRECT_P2P_H

/* S3: ConnectFailCode taxonomy (header-only enum — safe for
 * !ENABLE_NETPLAY builds where connect_fail.c is not compiled). */
#include "netplay/connect_fail.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DIRECT_P2P_IDLE = 0,
    DIRECT_P2P_UPNP_PROBE,      /* host: attempting UPnP port mapping */
    DIRECT_P2P_STUN_DISCOVER,   /* both: STUN binding request in flight */
    DIRECT_P2P_HOST_WAITING,    /* host: code published, waiting on first inbound punch */
    DIRECT_P2P_JOIN_PUNCHING,   /* joiner: Stun_HolePunch in flight */
    DIRECT_P2P_HANDOFF,         /* socket handed to netplay.c, session begun */
    DIRECT_P2P_FAILED_SYMMETRIC,/* punch timed out — Symmetric NAT suspected */
    DIRECT_P2P_FAILED_STUN,     /* STUN discovery exhausted all servers */
    DIRECT_P2P_FAILED_PUNCH,    /* hole-punch failed for non-symmetric reason */
    DIRECT_P2P_FALLBACK_SIGNALING,        /* using rendezvous server for endpoint exchange */
    DIRECT_P2P_FALLBACK_BILATERAL_PUNCH,  /* bilateral hole-punch in flight after signaling */
    DIRECT_P2P_FAILED_BILATERAL,          /* bilateral fallback exhausted; truly unreachable */
    DIRECT_P2P_FAILED_HANDSHAKE,          /* R-1: post-handoff MIST handshake reject (see
                                             DirectP2P_NotifySessionRejected) */
} DirectP2PState;

/* Role of the local end in the active session. Defined here (rather than
 * in direct_p2p.c only) so other TUs (overlay, future test hooks) can
 * branch on role via DirectP2P_GetRole without exposing the file-local
 * Work struct. Values are kept stable because the overlay TU compiles
 * separately. */
typedef enum {
    ROLE_NONE = 0,
    ROLE_HOST,
    ROLE_JOIN,
} Role;

/* S4a: classification of an inbound datagram on the host's waiting
 * socket. Produced by the file-local classifier in direct_p2p.c (also
 * exposed to tests via DirectP2P_TestHook_ClassifyHostDatagram).
 * IGNORE is the fail-closed default: an unauthenticated datagram must
 * never consume the host's peer slot. */
typedef enum {
    DP2P_HOST_DGRAM_IGNORE = 0,   /* drop, keep waiting — never consume the slot */
    DP2P_HOST_DGRAM_RENDEZVOUS,   /* '3SXR' frame -> try_handle_deliver */
    DP2P_HOST_DGRAM_STUN,         /* Binding Response -> rebind/drift handler */
    DP2P_HOST_DGRAM_PEER_PUNCH,   /* authenticated punch -> accept as the peer */
} DirectP2PHostDgramClass;

#ifdef ENABLE_NETPLAY

/* One-shot setup: registers the session-teardown callback that releases
 * any in-flight UPnP mapping. Safe to call multiple times — the callback
 * is idempotent. */
void DirectP2P_Init(void);

/* Host-side flow. preferred_port=0 requests OS assignment. Internally
 * reads CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP and
 * CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS. No-op if a session is
 * already in flight. */
void DirectP2P_BeginHost(int preferred_port);

/* Join-side flow. peer_code must be a room-code display form (with or
 * without dashes, any case); accepts both 18-char raw and 20-char
 * dashed (v3 format). No-op on NULL/empty. On success, code is
 * persisted to CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE. A v1
 * (11-char), v2 (14-char) or unknown-version code fails immediately
 * with CONNECT_FAIL_CODE_VERSION_OLDER / _NEWER and an explanatory
 * status. */
void DirectP2P_BeginJoin(const char* peer_code);

/* User cancel. Sets the worker's atomic cancel flag, waits up to a
 * few seconds for the worker to exit, tears down any owned STUN socket
 * or UPnP mapping, returns state to IDLE. Safe to call from any
 * thread. */
void DirectP2P_Cancel(void);

/* Per-frame pump from the game loop. Inspects the worker's published
 * state; on HOST_WAITING drains the STUN socket non-blockingly until
 * a valid inbound datagram arrives; on worker completion transfers
 * the STUN socket into netplay.c. Never blocks. */
void DirectP2P_Tick(void);

/* Current state. Safe to call from any thread. */
DirectP2PState DirectP2P_GetState(void);

/* Current role of the local end. Returns ROLE_NONE when no session is
 * active. Snapshot value — set once per BeginHost/BeginJoin and not
 * mutated thereafter, so no atomic is required. Used by the overlay TU
 * to branch the mode-label between HOSTING / CONNECTING. */
Role DirectP2P_GetRole(void);

/* S3-review M-3: true while a HOST parked in FAILED_STUN still has S2
 * auto-retries left (Tick will re-spawn the worker after backoff) —
 * i.e. the state is NOT yet terminal. False for a joiner, for any
 * other state, and once the host retry budget is exhausted. netplay_nav
 * consults this so its terminal-failure exit fires the moment
 * FAILED_STUN actually becomes terminal instead of waiting out the
 * 150 s orchestrator deadline. Main-thread only. */
bool DirectP2P_HostStunRetryPending(void);

/* Display-form room code (20 visible chars, v3), valid only while
 * state == DIRECT_P2P_HOST_WAITING. Returns "" in all other states.
 * Pointer is into internal static storage — do not free or mutate. */
const char* DirectP2P_GetHostCode(void);

/* Human-readable status for the game-side overlay. Always non-NULL,
 * possibly empty string. Updates on state transitions. */
const char* DirectP2P_GetStatusText(void);

/* R-1: called by netplay.c when the post-handoff MIST handshake rejects
 * the session (peer build incompatible / too old / unreachable). Records
 * the human-readable reason and latches a flag so the session-teardown
 * callback parks the orchestrator in DIRECT_P2P_FAILED_HANDSHAKE instead
 * of IDLE — the overlay then keeps ERROR + reason on screen after the
 * game soft-resets to attract, exactly like the pre-handoff FAILED_*
 * states. Cleared by DirectP2P_Cancel / the next BeginHost|BeginJoin
 * (which on the MiSTer OSD flow is a fresh process anyway). Main-thread
 * only (called from the game thread's session state machine). */
void DirectP2P_NotifySessionRejected(const char* reason);

/* S3 generalization of NotifySessionRejected: latch ANY post-handoff
 * session failure (MIST reject, CONNECTING-deadline timeout, ...) with
 * its taxonomy code so the teardown callback parks FAILED_HANDSHAKE-
 * style with an attributable status, and DirectP2P_Tick emits ONE
 * machine-coded report line to the netplay log. `reason` may be NULL/
 * empty — the code's ConnectFail_UserText is used then. Main-thread
 * only. */
void DirectP2P_NotifySessionFailed(ConnectFailCode code, const char* reason);

/* Arm-time refusal: park the orchestrator directly in
 * DIRECT_P2P_FAILED_HANDSHAKE with `reason` as the status text, WITHOUT
 * any session having started. Used by Netplay_RefuseArm when the
 * verified-arcade predicate fails — reuses the exact overlay surfacing
 * the MIST reject path uses (ERROR + reason via DrawOverlay). Terminal
 * like the other FAILED_* states; cleared by DirectP2P_Cancel (the
 * network-menu exit calls it) or process restart. Main-thread only.
 * Stamps CONNECT_FAIL_BALANCE_UNAVAILABLE so Tick's terminal-state
 * reporter has an honest taxonomy code to log. */
void DirectP2P_RefuseSession(const char* reason);

/* Step 8 — Per-frame native overlay. Renders three centered lines into
 * the 384x224 game canvas via SSPutStrPro:
 *   line 1: mode label (HOSTING / CONNECTING / CONNECTED / ERROR)
 *   line 2: 20-char display-form room code (host-waiting only)
 *   line 3: DirectP2P_GetStatusText() detail
 * No-op when state == DIRECT_P2P_IDLE. Called from the main SDL render
 * loop via NetplayScreen_Render; see src/port/sdl/netplay_screen.c. */
void DirectP2P_DrawOverlay(void);

#ifdef NETPLAY_TEST_HOOKS
/* Step 6 of docs/plan-bilateral-hole-punch.md — test-only seam. The
 * production translation unit defines these only when NETPLAY_TEST_HOOKS
 * is on; tests in src/netplay/test_bilateral_punch.c override the two
 * setters to install mocks for Stun_HolePunch / Rendezvous_Send without
 * touching the network. The IsLanPeer accessor exposes the file-static
 * direct_p2p_is_lan_peer for the LAN-bypass truth-table test.
 *
 * Including stun.h + SDL_net.h here is gated on NETPLAY_TEST_HOOKS so
 * production builds don't pay the include cost. */
#include "netplay/stun.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3_net/SDL_net.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* S4a: signature includes the 8-byte punch-auth token. */
typedef bool (*DirectP2P_StunHolePunch_fn)(StunResult* local,
                                           char* peer_ip,
                                           uint16_t* peer_port,
                                           const uint8_t punch_token[STUN_PUNCH_TOKEN_LEN],
                                           int punch_duration_ms,
                                           SDL_AtomicInt* cancel_flag);
typedef bool (*DirectP2P_RendezvousSend_fn)(NET_DatagramSocket* sock,
                                            NET_Address* target,
                                            uint16_t target_port,
                                            const uint8_t* pkt,
                                            size_t pkt_len);
/* S2: Stun_Discover seam — signature must match
 *   bool Stun_Discover(StunResult*, uint16_t local_port, int timeout_ms); */
typedef bool (*DirectP2P_StunDiscover_fn)(StunResult* result,
                                          uint16_t local_port,
                                          int timeout_ms);

void DirectP2P_TestHook_SetStunHolePunch(DirectP2P_StunHolePunch_fn fn);
void DirectP2P_TestHook_SetRendezvousSend(DirectP2P_RendezvousSend_fn fn);
void DirectP2P_TestHook_SetStunDiscover(DirectP2P_StunDiscover_fn fn);
bool DirectP2P_TestHook_IsLanPeer(const char* ip);
/* S4a: expose the host-waiting datagram classifier (the routing gate
 * host_tick_receive runs on every inbound datagram) so the truth table
 * — DELIVER / STUN / authenticated-punch / IGNORE — is unit-testable.
 * The pre-S4a behavior was "anything that isn't '3SXR' or STUN is the
 * peer", which made ONE stray/hostile datagram consume the host's peer
 * slot permanently. The DirectP2PHostDgramClass enum is declared above
 * (outside this test-hooks block — production code uses it too). */
DirectP2PHostDgramClass DirectP2P_TestHook_ClassifyHostDatagram(
    const uint8_t* buf, int len,
    const uint8_t token[STUN_PUNCH_TOKEN_LEN], bool token_valid);
/* S3-review HIGH-1: override the joiner's fallback-signaling budget
 * (ms; <= 0 restores the config value). The self-DELIVER regression
 * test must wait the FULL budget out — its fix removes the loop's
 * early exit — so it shrinks the budget rather than sleeping 2 x 8 s. */
void DirectP2P_TestHook_SetSignalBudgetMs(int ms);
/* S3-review HIGH-2: invoke the registered session-teardown callback
 * (the same one netplay.c's EXITING pass fires) so a harness can drive
 * notify-failure -> teardown -> FAILED_HANDSHAKE -> one FAIL report. */
void DirectP2P_TestHook_RunTeardown(void);

/* S4-review HIGH-1b: the host punch-gate throttle. The gate accounting
 * is deliberately free of s_work and of thread lifecycle so it can be
 * driven deterministically with an injected clock — the production path
 * passes SDL_GetTicks().
 *
 *   Reset       — clear every source tally, mute and session counter.
 *   NoteBad     — charge ONE bad-token punch-shaped datagram to src_ip.
 *                 Returns true when that charge crossed the session
 *                 re-roll threshold (production then re-rolls the code).
 *   IsMuted     — is src_ip currently refused an ACCEPT?
 *   Counters    — session totals for assertions; any pointer may be NULL.
 *   ClearMutes  — the side effect a re-roll has on the table.
 *
 * The thresholds are exported so a test asserts against the shipped
 * numbers instead of hardcoding a copy that can silently drift. */
/* S4-review MEDIUM-2: multiply the host-waiting elapsed clock so the
 * 30 s CONNECT_HOST_ADVISORY_MS boundary is reached in 30 s / scale of
 * real time. Scales the CLOCK, not the threshold, so the classifier
 * still runs against the shipped constant. 1 (or less) disables. */
void DirectP2P_TestHook_SetHostAdvisoryScale(int scale);

/* S5 relay rung seam. The rung's whole client-side contract is "call the
 * EXISTING do_handoff with the RELAY endpoint", so the assertion that
 * matters is on do_handoff's own arguments — not on internal state a
 * rung could set correctly while never reaching the handoff. `out_count`
 * separates "no handoff at all" from "a handoff to 0.0.0.0:0". Any
 * pointer may be NULL. */
void DirectP2P_TestHook_LastHandoff(char* out_ip, int ip_cap, uint16_t* out_port,
                                    int* out_player, bool* out_relay, int* out_count);
void DirectP2P_TestHook_ResetHandoff(void);

void DirectP2P_TestHook_PunchGateReset(void);
bool DirectP2P_TestHook_PunchGateNoteBad(const char* src_ip, uint32_t now_ms);
bool DirectP2P_TestHook_PunchGateIsMuted(const char* src_ip, uint32_t now_ms);
void DirectP2P_TestHook_PunchGateClearMutes(void);
void DirectP2P_TestHook_PunchGateCounters(int* bad_total, int* rerolls);
void DirectP2P_TestHook_PunchGateLimits(int* src_max_bad, uint32_t* mute_ms,
                                        int* total_reroll, int* reroll_max,
                                        int* src_table);
#endif /* NETPLAY_TEST_HOOKS */

#else /* !ENABLE_NETPLAY */

/* When netplay is compiled out, direct_p2p.c is excluded from the build
 * by the CMake src/netplay glob filter (see CMakeLists.txt). To keep
 * call sites (main.c per-frame Tick, future Step 8 overlay reads)
 * link-clean without needing a separate stub TU, expose header-local
 * no-op inlines mirroring the full API. */
static inline void DirectP2P_Init(void) { }
static inline void DirectP2P_BeginHost(int preferred_port) { (void)preferred_port; }
static inline void DirectP2P_BeginJoin(const char* peer_code) { (void)peer_code; }
static inline void DirectP2P_Cancel(void) { }
static inline void DirectP2P_Tick(void) { }
static inline DirectP2PState DirectP2P_GetState(void) { return DIRECT_P2P_IDLE; }
static inline Role DirectP2P_GetRole(void) { return ROLE_NONE; }
static inline bool DirectP2P_HostStunRetryPending(void) { return false; }
static inline const char* DirectP2P_GetHostCode(void) { return ""; }
static inline const char* DirectP2P_GetStatusText(void) { return ""; }
static inline void DirectP2P_NotifySessionRejected(const char* reason) { (void)reason; }
static inline void DirectP2P_NotifySessionFailed(ConnectFailCode code, const char* reason) { (void)code; (void)reason; }
static inline void DirectP2P_RefuseSession(const char* reason) { (void)reason; }
static inline void DirectP2P_DrawOverlay(void) { }

#endif /* ENABLE_NETPLAY */

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_DIRECT_P2P_H */
