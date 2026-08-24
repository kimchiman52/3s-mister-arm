#ifndef NETPLAY_H
#define NETPLAY_H

#include <stdbool.h>

/* Forward declaration so netplay.h does not pull in <SDL3_net/SDL_net.h>.
 * Mirrors upstream /tmp/3sxtra/src/netplay/netplay.h typedef hygiene. */
struct NET_DatagramSocket;

typedef struct NetworkStats {
    int delay;
    int ping;
    int rollback;
} NetworkStats;

typedef enum NetplaySessionState {
    NETPLAY_SESSION_IDLE,
    NETPLAY_SESSION_TRANSITIONING,
    NETPLAY_SESSION_CONNECTING,
    NETPLAY_SESSION_RUNNING,
    NETPLAY_SESSION_EXITING,
} NetplaySessionState;

void Netplay_SetParams(int player, const char* ip);
// Override the remote UDP port chosen by Netplay_SetParams. Needed on the
// direct-P2P orchestrator path where the STUN-translated peer port is only
// known after hole-punch, not at SetParams time. See do_handoff() in
// src/netplay/direct_p2p.c.
void Netplay_SetRemotePort(unsigned short port);
// True once remote_ip has been wired via Netplay_SetParams (LAN/localhost
// path) OR via do_handoff() after the direct-P2P orchestrator completes
// its UPnP+STUN hole-punch. Used by netplay_nav to gate NAV_START_NETPLAY
// so the nav state machine doesn't call Netplay_BeginDirectP2P before
// remote_ip is populated.
bool Netplay_IsRemoteIpSet(void);
void Netplay_BeginDirectP2P();
void Netplay_TickDirectP2P();
// Step 6 (docs/plan-stun-direct-p2p.md): pre-punched STUN socket handoff.
// Mirrors upstream /tmp/3sxtra/src/netplay/netplay.c:808-814. Ownership
// transfers to netplay.c; destroyed on session teardown. Calling twice
// without an intervening teardown destroys the previously held socket
// before replacing it. Pass NULL to clear.
void Netplay_SetStunSocket(struct NET_DatagramSocket* socket);
// Step 6 (docs/plan-stun-direct-p2p.md, P-2 #18): register a single
// callback fired at the top of NETPLAY_SESSION_EXITING teardown, before
// the STUN socket is destroyed. Used by the direct-P2P orchestrator
// (Step 7) to release its UPnP mapping. Pass NULL to clear. The slot is
// not cleared automatically — the callback persists across sessions.
void Netplay_SetSessionTeardownCallback(void (*cb)(void));
// S3 (docs/plan-netplay-connection.md §5): append one line to the
// per-session netplay log (<pref>/logs/netplay-<utc_ms>.log), opening it
// lazily when no session has opened it yet — connection failures happen
// before configure_gekko() normally opens the file, and the attributed
// failure line + stage timings must survive to disk for field reports.
// Also tees to SDL_Log. MAIN THREAD ONLY.
void Netplay_LogConnectEvent(const char* line);
void Netplay_SetMatchmakingParams(const char* server_ip, int server_port);
void Netplay_BeginMatchmaking();
void Netplay_TickMatchmaking();
bool Netplay_IsMatchmakingPending(); // true while searching, false once matched or idle
void Netplay_CancelMatchmaking();
void Netplay_Run();
NetplaySessionState Netplay_GetSessionState();
// S3: honest connect-phase progress text ("Verifying opponent (3s)...",
// "Syncing with opponent (7s)... START quits") for the netplay screen.
// Returns "" outside TRANSITIONING/CONNECTING. Never NULL.
const char* Netplay_GetConnectStatusText(void);
void Netplay_HandleMenuExit();

// Arm-time predicate: netplay arms ONLY in verified-arcade balance state
// (balance auto-selects at boot and is fixed for the process). Every
// session entry path — NetplayNav_Arm, the direct-P2P handoff dispatch,
// the matchmaking CLI, and the in-game network menu — must consult this
// before starting anything, and call Netplay_RefuseArm() on false, which
// logs and routes the human-readable reason to the direct-P2P overlay
// (the same surfacing mechanism the MIST handshake reject path uses).
bool Netplay_ArmAllowed(void);
void Netplay_RefuseArm(void);
void Netplay_GetNetworkStats(NetworkStats* stats);

// === 3SX-private extensions ===
// Phase 6 Step 2: port of the 8-slot event queue from 3sxtra
// (/tmp/3sxtra/src/netplay/netplay.h:37-51). See docs/plan-netplay-phase6.md
// Step 2.

typedef enum {
    NETPLAY_EVENT_NONE = 0,
    NETPLAY_EVENT_SYNCHRONIZING,
    NETPLAY_EVENT_CONNECTED,
    NETPLAY_EVENT_DISCONNECTED,
} NetplayEventType;

typedef struct {
    NetplayEventType type;
} NetplayEvent;

bool Netplay_PollEvent(NetplayEvent* out);

// === Tier-1 netplay diag — Item 10: SIGTERM flush hook ===
// Called from src/main.c after the main loop exits but before SDL teardown
// so we capture diagnostics for wrapper-initiated SIGTERM scenarios. If
// no session was active, this is a no-op. If one was active, it:
//   - emits a final heartbeat-shaped line (best-effort, no per-second wait)
//   - dumps the per-packet ring buffer to <pref>/states/
//   - captures /proc/net/snmp UDP-row deltas
//   - flushes and closes the per-session netplay log file
// All file writes are guarded so a partial pref-path setup can't crash.
void Netplay_FlushDiagnostics(void);

#endif
