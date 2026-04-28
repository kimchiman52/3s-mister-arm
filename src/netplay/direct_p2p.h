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
} DirectP2PState;

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
 * without dashes, any case); accepts both 14-char raw and 17-char
 * dashed. No-op on NULL/empty. On success, code is persisted to
 * CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE. */
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

/* Display-form 17-char room code, valid only while state ==
 * DIRECT_P2P_HOST_WAITING. Returns "" in all other states. Pointer is
 * into internal static storage — do not free or mutate. */
const char* DirectP2P_GetHostCode(void);

/* Human-readable status for the game-side overlay. Always non-NULL,
 * possibly empty string. Updates on state transitions. */
const char* DirectP2P_GetStatusText(void);

/* Step 8 — Per-frame native overlay. Renders three centered lines into
 * the 384x224 game canvas via SSPutStrPro:
 *   line 1: mode label (HOSTING / CONNECTING / CONNECTED / ERROR)
 *   line 2: 17-char room code (host-waiting only)
 *   line 3: DirectP2P_GetStatusText() detail
 * No-op when state == DIRECT_P2P_IDLE. Called from the main SDL render
 * loop via NetplayScreen_Render; see src/port/sdl/netplay_screen.c. */
void DirectP2P_DrawOverlay(void);

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
static inline const char* DirectP2P_GetHostCode(void) { return ""; }
static inline const char* DirectP2P_GetStatusText(void) { return ""; }
static inline void DirectP2P_DrawOverlay(void) { }

#endif /* ENABLE_NETPLAY */

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_DIRECT_P2P_H */
