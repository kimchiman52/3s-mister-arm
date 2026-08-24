#include "netplay/netplay.h"
#include "arcade/arcade_balance.h"
#include "main.h"
#include "port/config/config.h"
#include "port/config/draw_players_above_hud.h"
#include "netplay/connect_fail.h"
#include "netplay/direct_p2p.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "netplay/game_state.h"
#include "netplay/matchmaking.h"
#include "netplay/mist_handshake.h"
#include "netplay/net_tuning.h"
#include "netplay/sdl_net_adapter.h"
#include "port/paths.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/grade.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/slowf.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/game.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/io/pulpul.h"
#include "sf33rd/Source/Game/menu/menu.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "sf33rd/Source/Game/rendering/dc_ghost.h"
#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/rendering/texcash.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/utils/djb2_hash.h"
#include "types.h"

#include <stdbool.h>

#include "gekkonet.h"
#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define INPUT_HISTORY_MAX 120
#define FRAME_SKIP_TIMER_MAX 60 // Allow skipping a frame roughly every second
#define STATS_UPDATE_TIMER_MAX 60
#define DELAY_FRAMES 1
#define PLAYER_COUNT 2

// Uncomment to enable packet drops
// #define LOSSY_ADAPTER

// 3SX-private: forward declaration for event queue (defined at end of file).
// Phase 6 Step 2: port of /tmp/3sxtra/src/netplay/netplay.c:70.
static void push_event(NetplayEventType type);
// S3: forward declaration — connect_abort_tick (defined with the S3 state
// above Netplay_Run's helpers) tears down via handle_disconnection.
static void handle_disconnection(void);

static GekkoSession* session = NULL;
static unsigned short local_port = 0;
static unsigned short remote_port = 0;
static char remote_ip_buf[64] = { 0 };
static const char* remote_ip = NULL;
static int player_number = 0;
static int player_handle = 0;
static NetplaySessionState session_state = NETPLAY_SESSION_IDLE;
static char matched_ip[64];
static const char* matchmaking_server_ip = NULL;
static int matchmaking_server_port = 9000;
static bool matchmaking_pending = false;
static bool direct_p2p_pending = false;
static NET_DatagramSocket* p2p_sock = NULL;
// Step 6 (docs/plan-stun-direct-p2p.md): pre-punched STUN socket set by
// Netplay_SetStunSocket(). Mirrors upstream
// /tmp/3sxtra/src/netplay/netplay.c:79. Ownership sits with netplay.c
// after the setter is called; destroyed on session teardown.
static NET_DatagramSocket* stun_socket = NULL;
// Step 6 (plan P-2 #18): fired at the start of session teardown so the
// direct-P2P orchestrator can release its UPnP mapping while the STUN
// socket is still valid (the callback may inspect it via getsockname).
static void (*session_teardown_cb)(void) = NULL;
static u16 input_history[2][INPUT_HISTORY_MAX] = { 0 };
static float frames_behind = 0;
static int frame_skip_timer = 0;
static int transition_ready_frames = 0;

static int stats_update_timer = 0;
static int frame_max_rollback = 0;
static NetworkStats network_stats = { 0 };
// Tracks the most recent non-rolling-back GekkoAdvanceEvent frame, used by
// the per-second heartbeat telemetry in update_network_stats().
static int s_last_advance_frame = 0;

// === Netplay diagnostics state (gated by CFG_KEY_NETPLAY_DIAG_ENABLE) ===
// Captured at session start and used by:
//   - the per-second heartbeat (update_network_stats)
//   - the desync handler (process_session)
//   - the session-end log (Netplay_Run EXITING case)
//
// Refreshed inside configure_gekko() so two consecutive sessions in the
// same game process get distinct UUIDs and accurate UTCs.
static SDL_AtomicInt s_diag_enabled;
static uint32_t s_session_uuid = 0;
static uint64_t s_session_started_unix_ms = 0;
static uint64_t s_session_started_ticks_ms = 0;
// Reason buffer populated by callsites that initiate teardown (peer drop,
// desync, user cancel) and consumed by the EXITING-state session-end log.
static char s_session_end_reason[64] = { 0 };
static int s_session_end_frame = -1;

// === Tier-1 netplay diag — Item 3: previous-snapshot per-packet counters ===
// Item 3 keeps cumulative counters in sdl_net_adapter.c. The heartbeat
// reports deltas; we hold the previous snapshot here so update_network_stats
// can compute (cur - prev) once per second.
static uint64_t s_hb_prev_pkt_tx[8] = { 0 };
static uint64_t s_hb_prev_pkt_rx[8] = { 0 };

// === Tier-1 netplay diag — Item 5: GekkoAdvanceEvent watchdog ===
// Latched UTC ms of the most recent GekkoAdvanceEvent (Phase or rollback
// — both count). If it has been > 250ms since the last advance and we
// haven't yet reported this stall, emit a WATCHDOG line and latch the
// reported flag so we don't spam. Reset the latch on the next advance.
// 250ms ~= 15 frames at 60 Hz; healthy sessions never go that long
// without an advance even on a heavy rollback chain.
static uint64_t s_last_advance_utc_ms = 0;
static bool     s_watchdog_latched = false;

// === Tier-1 netplay diag — Item 6: rollback-depth bucket histogram ===
// Counts of advance batches by `frames_rolled_back` value, reset on each
// heartbeat snapshot. Buckets: 0, 1-2, 3-4, 5-7, 8+.
static uint32_t s_rb_buckets[5] = { 0 };

// === Tier-1 netplay diag — Item 7: buffered netplay log file ===
// One FILE* opened at session start, fflush'd once per second (driven by
// the heartbeat in update_network_stats), closed in the EXITING branch of
// Netplay_Run. All netplay-tagged lines pass through netplay_logf() which
// tees to SDL_Log + the file.
static FILE* s_netplay_log = NULL;

// === Tier-1 netplay diag — Item 9: /proc/net/snmp UDP-drop snapshots ===
// Captured at session start, on peer-disconnect, and at session end. The
// heartbeat doesn't sample this — it's relatively expensive (file read +
// parse) and only useful for before/after deltas.
typedef struct UdpSnmp {
    bool valid;
    uint64_t in_errors;
    uint64_t rcvbuf_errors;
    uint64_t no_ports;
} UdpSnmp;
static UdpSnmp s_udp_snmp_session_start = { 0 };

// Read /proc/net/snmp and pull the InErrors, RcvbufErrors, NoPorts fields
// out of the UDP row. Returns false (and leaves *out untouched apart from
// `valid=false`) on any parse failure or if the file doesn't exist (e.g.
// non-MiSTer host machines don't ship /proc/net/snmp). The format is two
// lines per protocol: a header with field names followed by a values line.
static bool read_proc_net_snmp_udp(UdpSnmp* out) {
    if (out == NULL) {
        return false;
    }
    out->valid = false;
    FILE* f = fopen("/proc/net/snmp", "r");
    if (f == NULL) {
        return false;
    }
    char header[1024];
    char values[1024];
    bool got = false;
    while (fgets(header, sizeof(header), f) != NULL) {
        if (SDL_strncmp(header, "Udp:", 4) != 0) {
            continue;
        }
        if (fgets(values, sizeof(values), f) == NULL) {
            break;
        }
        if (SDL_strncmp(values, "Udp:", 4) != 0) {
            // /proc/net/snmp pairs Udp: header / Udp: values lines.
            // If the second line isn't a values line, give up.
            break;
        }
        // Find indices of InErrors, RcvbufErrors, NoPorts in the header
        // tokens. Token 0 is the literal "Udp:" prefix.
        int idx_inerr = -1, idx_rcvbuf = -1, idx_noports = -1;
        int hdr_n = 0;
        char* save_h = NULL;
        for (char* tok = SDL_strtok_r(header, " \t\r\n", &save_h);
             tok != NULL && hdr_n < 64;
             tok = SDL_strtok_r(NULL, " \t\r\n", &save_h)) {
            if (SDL_strcmp(tok, "InErrors") == 0)      idx_inerr = hdr_n;
            else if (SDL_strcmp(tok, "RcvbufErrors") == 0) idx_rcvbuf = hdr_n;
            else if (SDL_strcmp(tok, "NoPorts") == 0)     idx_noports = hdr_n;
            hdr_n++;
        }
        // Tokenize values; pull by index.
        char* val_tokens[64];
        int val_n = 0;
        char* save_v = NULL;
        for (char* tok = SDL_strtok_r(values, " \t\r\n", &save_v);
             tok != NULL && val_n < 64;
             tok = SDL_strtok_r(NULL, " \t\r\n", &save_v)) {
            val_tokens[val_n++] = tok;
        }
        if (idx_inerr   >= 0 && idx_inerr   < val_n) out->in_errors      = (uint64_t)SDL_strtoull(val_tokens[idx_inerr],   NULL, 10);
        if (idx_rcvbuf  >= 0 && idx_rcvbuf  < val_n) out->rcvbuf_errors  = (uint64_t)SDL_strtoull(val_tokens[idx_rcvbuf],  NULL, 10);
        if (idx_noports >= 0 && idx_noports < val_n) out->no_ports       = (uint64_t)SDL_strtoull(val_tokens[idx_noports], NULL, 10);
        out->valid = (idx_inerr >= 0 || idx_rcvbuf >= 0 || idx_noports >= 0);
        got = true;
        break;
    }
    fclose(f);
    return got;
}

// Tee writer: the heartbeat and event lines go to both the system log
// (via SDL_Log so existing log surfaces still receive them) and the
// per-session netplay log file. Cheap — one fwrite per call once the
// file is open.
static void netplay_log_line(const char* line) {
    SDL_Log("%s", line);
    if (s_netplay_log != NULL) {
        fputs(line, s_netplay_log);
        fputc('\n', s_netplay_log);
    }
}

// Opens <pref>/logs/netplay-<utc_ms>.log for the active session. Block
// buffered (fflush is driven by the heartbeat at 1 Hz) so we avoid the
// per-line fopen+fwrite+fclose pattern in backend_logf. The filename is
// announced via SDL_Log at open time so post-mortem readers can find it
// quickly.
static void netplay_log_open(uint64_t utc_ms) {
    if (s_netplay_log != NULL) {
        return;
    }
    const char* pref = Paths_GetPrefPath();
    if (pref == NULL || pref[0] == '\0') {
        return;
    }
    char logs_dir[512];
    SDL_snprintf(logs_dir, sizeof(logs_dir), "%slogs", pref);
    SDL_CreateDirectory(logs_dir);
    char path[640];
    SDL_snprintf(path, sizeof(path), "%s/netplay-%llu.log",
                 logs_dir, (unsigned long long)utc_ms);
    s_netplay_log = fopen(path, "w");
    if (s_netplay_log != NULL) {
        // Block-buffered with a small buffer; we drive fflush from the heartbeat.
        setvbuf(s_netplay_log, NULL, _IOFBF, 4096);
        SDL_Log("[netplay sess=%08x] netplay log file: %s",
                s_session_uuid, path);
    } else {
        SDL_Log("[netplay sess=%08x] failed to open netplay log: %s",
                s_session_uuid, path);
    }
}

static void netplay_log_close(void) {
    if (s_netplay_log != NULL) {
        fflush(s_netplay_log);
        fclose(s_netplay_log);
        s_netplay_log = NULL;
    }
}

// UTC-ms helper used by the watchdog (Item 5), packet-ring (Item 4), and
// session-start/-end logs. P-1.1 fix: use clock_gettime(CLOCK_REALTIME)
// for full millisecond precision so timestamps cross-correlate exactly
// with sdl_net_adapter.c's now_utc_ms (same source). On Linux this is a
// vDSO-fast read with no syscall, identical perf profile to time(NULL).
static inline uint64_t netplay_utc_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

// R-1: Layer 3 MIST handshake gate. Formerly conditioned on a
// s_lobby_session flag that no live code path ever set (the RmlUi lobby
// UI was removed), which made the handshake dead code on every real
// session — two peers with different GameState layouts would connect,
// sync, and desync mid-match with no explanation. The gate now runs
// unconditionally for every session (direct P2P, matchmaking, LAN CLI)
// on the same socket configure_gekko() hands to GekkoNet.
static bool s_mist_handshake_done = false;
static char s_mist_reject_reason[128] = { 0 };
// Consecutive silent-timeout attempts. Retry policy (peer-skew
// tolerance, 40 x 500 ms cap) lives with MIST_HANDSHAKE_MAX_ATTEMPTS /
// mist_handshake_gate_next in mist_handshake.h — extracted there so it
// is unit-testable (adv-review M-5).
static int s_mist_handshake_attempts = 0;
// R-1 adv-review H-1: session-scoped latch — "the session peer's hello
// passed the full compatibility check this session". Arms the runner's
// implicit-completion path (Gekko-shaped traffic from a classified-clean
// peer completes the handshake; see mist_handshake_run_attempt docs for
// the stranded-completion race and the bypass-safety argument). Must
// persist ACROSS the retry attempts of one session and reset wherever
// s_mist_handshake_attempts resets: session start (direct-P2P tick,
// matchmaking match) and session EXITING.
static bool s_mist_peer_hello_ok = false;

// === S3 Part A (docs/plan-netplay-connection.md §5): CONNECTING deadline
// + user-reachable abort + live connect-status text ===
//
// NETPLAY_SESSION_CONNECTING had NO timeout: exit required a
// GekkoPlayerConnected/SessionStarted event, and GekkoNet's own
// DISCONNECT_TIMEOUT (5000 ms) applies only to actors already Connected —
// an actor stuck Initiating retries SendSyncRequest forever. The netplay
// watchdog (process_session) also only fires while RUNNING, so CONNECTING
// was entirely unwatched. We do NOT patch GekkoNet; instead:
//   - a wall-clock deadline (CONNECT_TIMEOUT_CONNECTING_MS = 15 s) bounds
//     the state and exits with an attributable reason
//     (P2P_FAIL_TIMEOUT_CONNECTING via DirectP2P_NotifySessionFailed);
//   - a 5 s progress log line makes the state log-visible (watchdog gap);
//   - holding START for CONNECT_ABORT_HOLD_FRAMES (~3 s) aborts BOTH the
//     TRANSITIONING (MIST handshake retry window) and CONNECTING states,
//     so nobody is stuck watching a frozen screen;
//   - s_connect_status carries honest progress text for
//     NetplayScreen_Render (replacing the perpetual "Match found!").
// All main-thread (Netplay_Run).
static uint64_t s_connecting_since_ms = 0;
static uint64_t s_connecting_last_log_ms = 0;
static int s_abort_hold_frames = 0;
static char s_connect_status[96] = { 0 };

// Shared abort handler for TRANSITIONING/CONNECTING: counts consecutive
// frames with START held on either pad; on the threshold, logs an
// attributed line and tears the session down as a user cancel (attract
// screen, no ERROR overlay). Returns true when the abort fired and the
// session is now EXITING.
static bool connect_abort_tick(const char* stage) {
    const bool start_down = ((p1sw_buff | p2sw_buff) & SWK_START) != 0;
    s_abort_hold_frames = ConnectFail_AbortHoldTick(s_abort_hold_frames, start_down);
    if (!ConnectFail_AbortHoldFired(s_abort_hold_frames)) {
        return false;
    }
    s_abort_hold_frames = 0;
    char line[192];
    SDL_snprintf(line, sizeof(line),
                 "[netplay-connect] ABORT code=%s stage=%s — user held START "
                 "through the %d-frame window",
                 ConnectFail_Code(CONNECT_FAIL_USER_ABORT), stage,
                 (int)CONNECT_ABORT_HOLD_FRAMES);
    Netplay_LogConnectEvent(line);
    if (s_session_end_reason[0] == '\0') {
        SDL_strlcpy(s_session_end_reason, "user-abort-connect", sizeof(s_session_end_reason));
        s_session_end_frame = s_last_advance_frame;
    }
    push_event(NETPLAY_EVENT_DISCONNECTED);
    handle_disconnection();
    return true;
}

// First-to-X (number of game wins required to close a session).
// Placeholder for Track C / lobby FT negotiation; default 2 = FT2 sessions.
// TODO(track-c): wire from GekkoNet handshake or config once lobby lands.
static int s_negotiated_ft = 2;

#if defined(LOSSY_ADAPTER)
static GekkoNetAdapter* base_adapter = NULL;
static GekkoNetAdapter lossy_adapter = { 0 };

static float random_float() {
    return (float)rand() / RAND_MAX;
}

static void LossyAdapter_SendData(GekkoNetAddress* addr, const char* data, int length) {
    const float number = random_float();

    // Adjust this number to change drop probability
    if (number <= 0.25) {
        return;
    }

    base_adapter->send_data(addr, data, length);
}
#endif

static void clean_input_buffers() {
    p1sw_0 = 0;
    p2sw_0 = 0;
    p1sw_1 = 0;
    p2sw_1 = 0;
    p1sw_buff = 0;
    p2sw_buff = 0;
    SDL_zeroa(PLsw);
    SDL_zeroa(plsw_00);
    SDL_zeroa(plsw_01);
}

/**
 * @netplay_sync THIS IS THE MOST IMPORTANT FUNCTION FOR INITIAL SYNC.
 *
 * Both peers enter netplay from slightly different local states (different
 * menus, timers, button configs, character select progress). This function
 * forces every divergent global to a known identical value so that the first
 * rollback frame starts from the same state on both sides.
 *
 * Ported from 3sxtra netplay.c:158-391 (Track A Phase 2). Substitutions vs
 * 3sxtra verbatim:
 *   - MenuTask_SetPhase(MTP_NETPLAY_IDLE) → task[TASK_MENU].r_no[0] = 5
 *     (we don't have 3sxtra's MenuTask phase enum; r_no routing is equivalent)
 *   - plw[].wu.pl_operator → plw[].wu.wu_operator (we keep the original name;
 *     3sxtra renamed to avoid C++ keyword conflict per research §9.5)
 *   - CFG_KEY_NETPLAY_FT / Config_GetInt → s_negotiated_ft static placeholder
 *     (FT negotiation is Track C lobby scope)
 */
static void setup_vs_mode() {
    // ====================================================================
    // PHASE 0: Zero per-player and combat subsystem state.
    //
    // When connecting from the network lobby, the game engine may have been
    // running under an overlay (attract mode, demo, etc.), leaving stale data
    // in PLW[] and related player subsystems. We only zero player/combat
    // state — NOT engine globals (G_No, Country, task routing, etc.) because
    // step_game() runs during TRANSITIONING to advance the game state
    // machine (G_No[1]: 12→1).
    // ====================================================================
    SDL_zeroa(plw);
    SDL_zeroa(zanzou_table);
    SDL_zeroa(super_arts);

    // Task timers and scratch data evolve independently per peer during menus.
    // Zero them for deterministic start. DO NOT zero r_no or condition —
    // those are game state machine routing fields set by the engine.
    for (int i = 0; i < 11; i++) {
        task[i].timer = 0;
        SDL_zeroa(task[i].free);
    }

    // Jump to menu idle routine (equivalent of 3sxtra's MenuTask_SetPhase).
    task[TASK_MENU].r_no[0] = 5;
    cpExitTask(TASK_SAVER);
    cpExitTask(TASK_PAUSE);

    // Zero pause flags — if one peer was paused before entering netplay,
    // these would differ on the first synced frame.
    Pause = 0;
    Game_pause = 0;

    // Re-set after zeroing plw — both players must be active for 2P mode.
    plw[0].wu.wu_operator = 1;
    plw[1].wu.wu_operator = 1;
    Operator_Status[0] = 1;
    Operator_Status[1] = 1;
    Clear_Personal_Data(0);
    Clear_Personal_Data(1);
    grade_check_work_1st_init(0, 0);
    grade_check_work_1st_init(0, 1);
    grade_check_work_1st_init(1, 0);
    grade_check_work_1st_init(1, 1);
    Setup_Training_Difficulty();

    // Tear down stale backgrounds, reinitialize the effect pool, and stop
    // any lingering effects. Without this, attract/demo mode effects start
    // diverged between peers — and since we snapshot EffectState for
    // rollback, every restore to an early frame would replay garbage.
    System_all_clear_Level_B();

    G_No[0] = 2;
    E_No[0] = 1;
    Demo_Flag = 1;

    G_No[1] = 12;
    G_No[2] = 1;
    Mode_Type = MODE_NETWORK;
    // Present_Mode is a PresentMode, not a ModeType (upstream #296); NETWORK
    // and NETPLAY happen to share numeric value 2 in both enums, but assign
    // the correctly-typed constant here rather than leaning on that.
    Present_Mode = PRESENT_MODE_NETPLAY;
    Play_Mode = 0;
    Replay_Status[0] = 0;
    Replay_Status[1] = 0;
    cpExitTask(TASK_MENU);

    // Force standard game settings so both peers use identical values
    // regardless of each player's local DIP switch configuration. Without
    // this, save_w[MODE_NETWORK] retains per-player settings that cause
    // gameplay desyncs (different HP, timer, round count).
    save_w[MODE_NETWORK].Time_Limit = 99;
    save_w[MODE_NETWORK].Battle_Number[0] = 2; // Best of 3 (1P vs CPU)
    save_w[MODE_NETWORK].Battle_Number[1] = 2; // Best of 3 (1P vs 2P)
    save_w[MODE_NETWORK].Damage_Level = 0;     // Normal damage
    save_w[MODE_NETWORK].Handicap = 0;
    save_w[MODE_NETWORK].GuardCheck = 0;

    // Balance is no longer forced off here: every netplay entry path is
    // gated at arm time by Netplay_ArmAllowed() (verified-arcade required)
    // and the MIST handshake digest-checks the adapted data, so both peers
    // simulate identical arcade balance. Belt-and-braces: a session
    // reaching this chokepoint without verified arcade means an entry
    // path missed its gate.
    if (!ArcadeBalance_IsEnabled()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "setup_vs_mode reached without verified arcade balance (%s) — "
                     "an entry path missed the Netplay_ArmAllowed gate",
                     ArcadeBalance_GetReason());
    }
    // CFG_DRAW_PLAYERS_ABOVE_HUD's gameplay-affecting reads (effect-table
    // population, scr_trans/scr_calc selection) are still a local config
    // peers never negotiate — suppress for the session at the common
    // session-setup chokepoint (covers matchmaking entries, which skip
    // NetplayNav_Arm). Released when the session finishes tearing down.
    DrawPlayersAboveHud_SetNetplaySuppressed(true);

    E_Timer = 0; // E_Timer can have different values depending on when the session was initiated.

    Deley_Shot_No[0] = 0;
    Deley_Shot_No[1] = 0;
    Deley_Shot_Timer[0] = 15;
    Deley_Shot_Timer[1] = 15;
    Random_ix16 = 0;
    Round_num = 0;
    Game_timer = 0;
    Random_ix32 = 0;
    Clear_Flash_Init(4);

    // Ensure both peers start with identical timer state regardless of local
    // DIP switch settings. Without this, CurrentSave()->Time_Limit can differ
    // per player's config.
    Counter_hi = 99;
    Counter_low = 60;

    // Flash_Complete runs during the character select screen at slightly
    // different speeds per peer depending on when they connected. Zero to sync.
    Flash_Complete[0] = 0;
    Flash_Complete[1] = 0;

    // BG scroll positions and parameters evolve independently during the
    // transition phase before synced gameplay. Zero them so both peers start
    // identical. (These fields were added to GameState in Phase 1.)
    SDL_zeroa(bg_pos);
    SDL_zeroa(fm_pos);
    SDL_zeroa(bg_prm);
    Screen_Switch = 0;
    Screen_Switch_Buffer = 0;
    system_timer = 0;
    Interrupt_Timer = 0;

    // Order[] tracks rendering layer visibility for character select UI
    // elements. Weak_PL picks the weaker CPU during demo/attract mode via
    // random_16(). Both diverge per peer before battle; zero for a clean start.
    SDL_zeroa(Order);
    Weak_PL = 0;

    // Force identity button config for MODE_NETWORK so Convert_User_Setting()
    // is a no-op during simulation. Each player's actual config was already
    // baked into their input by get_inputs() via Remap_Buttons().
    {
        const u8 identity[8] = { 0, 1, 2, 11, 3, 4, 5, 11 };
        for (int p = 0; p < 2; p++) {
            for (int s = 0; s < 8; s++)
                save_w[MODE_NETWORK].Pad_Infor[p].Shot[s] = identity[s];
            save_w[MODE_NETWORK].Pad_Infor[p].Vibration = 0;
        }
    }

    // Apply first-to-X wins: FT controls how many GAME wins are needed for a
    // session (tracked server-side in future). Each individual game is always
    // best-of-3 rounds (Battle_Number = 1, meaning need 2 round wins per game).
    {
        int ft = s_negotiated_ft > 0 ? s_negotiated_ft : 2;
        if (ft < 1)
            ft = 2;
        if (ft > 10)
            ft = 10;
        s_negotiated_ft = ft; // Keep for future match reporting.
        // Rounds per game: always best-of-3 (need 2 round wins).
        save_w[MODE_NETWORK].Battle_Number[0] = 1;
        save_w[MODE_NETWORK].Battle_Number[1] = 1;
    }

    // Check_Buff and Convert_Buff hold per-player button remapping tables.
    // Each peer loads them from their local config, so they differ between
    // players. Zero them so the simulation uses identity mappings.
    SDL_zeroa(Check_Buff);
    SDL_zeroa(Convert_Buff);

    // Timers that evolved independently during menus/transition. Without
    // this, Game_timer and Control_Time diverge immediately.
    Game_timer = 0;
    Control_Time = 0;
    players_timer = 0;
    G_Timer = 0;

    // Peers spend different amounts of time in the menus, and nothing on the
    // way into a fight otherwise clears what that leaves behind.
    Next_Demo = 0;

    // bg_initialize() reinitialises only part of each layer, and l_limit/
    // r_limit are written by the opening alone. scno is one of those: the
    // attract sequence leaves it at 3 and the stage only assigns it on
    // frame 2, so peers that reached the menu at different times disagree
    // on the first two frames of every session.
    SDL_zeroa(bg_w.bgw);
    bg_w.scno = 0;
    bg_w.scrno = 0;

    // Per-player globals that can hold stale values from the previous game
    // session or differ based on who connected first.
    Champion = 0;
    Forbid_Break = 0;
    Connect_Status = 0;
    Stop_SG = 0;
    Exec_Wipe = 0;
    Gap_Timer = 0;
    SDL_zeroa(E_No);

    // State machine routing numbers evolve per-player during character select.
    // Each peer advances C_No/SC_No from its own perspective, causing them
    // to diverge before battle. Zero them so both peers start identical.
    SDL_zeroa(C_No);
    SDL_zeroa(SC_No);

    // ====================================================================
    // PHASE 3: Zero checksummed globals not covered above.
    //
    // save_state() checksums a whitelist of gameplay-critical fields
    // (ported in Phase 3). On LAN, the native menu system's fade-destroy-
    // reinit cycle leaves these at zero already. On INTERNET, the game
    // engine runs under an overlay (attract/demo mode), leaving stale
    // values that differ per peer and cause frame-0 desyncs.
    // ====================================================================

    // Extended RNG indices (attract mode advances these independently).
    Random_ix16_ex = 0;
    Random_ix32_ex = 0;
    Random_ix16_com = 0;
    Random_ix32_com = 0;
    Random_ix16_ex_com = 0;
    Random_ix32_ex_com = 0;

    // Round/match state.
    Round_Level = 0;
    Round_Result = 0;
    SDL_zeroa(PL_Wins);
    Conclusion_Type = 0;
    SDL_zeroa(win_type);

    // Clean up stale attract/demo sequences that mutate start-of-match state.
    Combo_Demo_Flag = 0;
    Select_Demo_Index = 0;
    Demo_Stage_Index = 0;
    Demo_PL_Index = 0;

    // Player identity (set later by character select, but must start clean).
    SDL_zeroa(My_char);
    SDL_zeroa(Super_Arts);

    // Combat flags (stale from previous match or attract mode demo fights).
    SDL_zeroa(Attack_Flag);
    SDL_zeroa(Counter_Attack);
    SDL_zeroa(Guard_Flag);
    SDL_zeroa(Flip_Flag);
    SDL_zeroa(Lie_Flag);
    SDL_zeroa(Attack_Counter);
    SDL_zeroa(Bullet_No);
    SDL_zeroa(Bullet_Counter);
    SDL_zeroa(paring_counter);

    // Game flow.
    VS_Stage = 0;

    // Slow motion.
    SLOW_timer = 0;
    SLOW_flag = 0;
    EXE_flag = 0;

    // Stun gauge / vitality.
    SDL_zeroa(piyori_type);
    Max_vitality = 160; // MAX_VITALITY_DEFAULT — must not be 0 (setup_vitality divides by it).

    clean_input_buffers();
}

#if defined(LOSSY_ADAPTER)
static void configure_lossy_adapter(NET_DatagramSocket* sock) {
    base_adapter = SDLNetAdapter_Create(sock);
    lossy_adapter.send_data = LossyAdapter_SendData;
    lossy_adapter.receive_data = base_adapter->receive_data;
    lossy_adapter.free_data = base_adapter->free_data;
}
#endif

// R-1 — SDL3_net adapter for the extracted handshake runner core
// (mist_handshake_run_attempt). The core owns the loop/classification
// logic; these callbacks bind it to the live session socket. The most
// recently received datagram is kept alive in `last` (destroyed on the
// next recv or after the run) so send_reply_to_last can answer its
// source, exactly as the pre-extraction inline loop did.
typedef struct {
    NET_DatagramSocket* sock;
    NET_Address* peer;
    Uint16 peer_port;
    NET_Datagram* last;
} MistNetIo;

static void mist_netio_send_to_peer(void* vctx, const uint8_t* buf, size_t len) {
    MistNetIo* io = (MistNetIo*)vctx;
    NET_SendDatagram(io->sock, io->peer, io->peer_port, buf, (int)len);
}

static int mist_netio_recv(void* vctx, uint8_t* buf, size_t cap, bool* from_session_peer) {
    MistNetIo* io = (MistNetIo*)vctx;
    if (io->last != NULL) {
        NET_DestroyDatagram(io->last);
        io->last = NULL;
    }
    NET_Datagram* dgram = NULL;
    if (!NET_ReceiveDatagram(io->sock, &dgram) || dgram == NULL) {
        return 0;
    }
    size_t n = (size_t)dgram->buflen;
    if (n > cap) {
        n = cap;
    }
    SDL_memcpy(buf, dgram->buf, n);
    *from_session_peer = (dgram->port == io->peer_port) &&
                         (NET_CompareAddresses(dgram->addr, io->peer) == 0);
    io->last = dgram;
    return (int)n;
}

static void mist_netio_send_reply_to_last(void* vctx, const uint8_t* buf, size_t len) {
    MistNetIo* io = (MistNetIo*)vctx;
    if (io->last == NULL) {
        return;
    }
    NET_SendDatagram(io->sock, io->last->addr, io->last->port, buf, (int)len);
}

static uint64_t mist_netio_now_ms(void* vctx) {
    (void)vctx;
    return (uint64_t)SDL_GetTicks();
}

static void mist_netio_delay_ms(void* vctx, uint32_t ms) {
    (void)vctx;
    SDL_Delay(ms);
}

// R-1 / S3 — the MIST handshake gate runs on the active session socket
// before gekko_create() takes it over (tier-2 §8.2.4). S3 (Part C)
// converted it from a BLOCKING per-attempt runner — which stalled the
// main thread for the full 500 ms budget once per frame, dropping the
// game to ~2 fps with ~2 Hz input sampling for up to ~20-24 s of
// retries — to an incremental per-tick pump over the same MistRunnerIo
// seam: one bounded slice (<= one hello send + a drain of the queued
// datagrams) per frame, terminal results folded through the unchanged
// mist_handshake_gate_next retry policy. The attempt state persists in
// the statics below; the resolved peer address is cached across the
// retry attempts of one session and released by mist_pump_stop().
static NET_DatagramSocket* acquire_active_socket(void); /* defined below */

static bool s_hs_pump_active = false;
static MistPumpState s_hs_pump;
static MistNetIo s_hs_net_io = { NULL, NULL, 0, NULL };
static NET_Address* s_hs_peer_addr = NULL;
static const MistRunnerIo s_hs_io_template = {
    &s_hs_net_io,
    mist_netio_send_to_peer,
    mist_netio_recv,
    mist_netio_send_reply_to_last,
    mist_netio_now_ms,
    mist_netio_delay_ms,
};

// Release everything the pump holds. Idempotent; called on PROCEED,
// hard failure, and session EXITING (covers a mid-handshake abort).
static void mist_pump_stop(void) {
    if (s_hs_net_io.last != NULL) {
        NET_DestroyDatagram(s_hs_net_io.last);
        s_hs_net_io.last = NULL;
    }
    if (s_hs_peer_addr != NULL) {
        NET_UnrefAddress(s_hs_peer_addr);
        s_hs_peer_addr = NULL;
    }
    s_hs_net_io.sock = NULL;
    s_hs_net_io.peer = NULL;
    s_hs_pump_active = false;
}

// Arm one pump attempt: bind the session socket, resolve (once per
// session, cached) the peer address, stamp the 500 ms budget. Returns
// false with the reason cached in s_mist_reject_reason on a local
// transport error — the caller folds that through the gate as a hard
// failure, exactly like the old blocking wrapper did.
static bool mist_pump_start(void) {
    NET_DatagramSocket* sock = acquire_active_socket();
    if (sock == NULL || remote_ip == NULL || remote_port == 0) {
        SDL_strlcpy(s_mist_reject_reason, "missing transport state", sizeof(s_mist_reject_reason));
        return false;
    }
    if (s_hs_peer_addr == NULL) {
        s_hs_peer_addr = NET_ResolveHostname(remote_ip);
        if (s_hs_peer_addr == NULL) {
            SDL_strlcpy(s_mist_reject_reason, "peer resolve failed", sizeof(s_mist_reject_reason));
            return false;
        }
        /* Wait briefly for resolution. One-time per session; remote_ip
         * is a numeric dotted-quad on the orchestrator/matchmaking
         * paths, so this resolves immediately — the bounded poll only
         * matters for a hostname passed via the LAN CLI. */
        for (int spin = 0; spin < 50; spin++) {
            if (NET_GetAddressStatus(s_hs_peer_addr) != NET_WAITING) break;
            SDL_Delay(2);
        }
        if (NET_GetAddressStatus(s_hs_peer_addr) != NET_SUCCESS) {
            NET_UnrefAddress(s_hs_peer_addr);
            s_hs_peer_addr = NULL;
            SDL_strlcpy(s_mist_reject_reason, "peer resolve timed out", sizeof(s_mist_reject_reason));
            return false;
        }
    }
    s_hs_net_io.sock = sock;
    s_hs_net_io.peer = s_hs_peer_addr;
    s_hs_net_io.peer_port = remote_port;
    if (!mist_handshake_pump_begin(&s_hs_pump, &s_hs_io_template,
                                   s_mist_reject_reason, sizeof(s_mist_reject_reason))) {
        return false;
    }
    s_hs_pump_active = true;
    return true;
}

// R-1: single source of truth for "which socket carries this session".
// Was inlined in configure_gekko(); hoisted so the MIST handshake gate in
// Netplay_Run() runs on the exact same socket GekkoNet will take over.
// Idempotent: the direct-P2P CLI/LAN socket is created once and cached in
// p2p_sock (destroyed + NET_Quit'd on session EXITING, unchanged).
static NET_DatagramSocket* acquire_active_socket(void) {
    if (stun_socket != NULL) {
        // Step 6: pre-punched STUN socket (internet play via orchestrator).
        // Takes priority over matchmaking — if the STUN socket is set, the
        // orchestrator has already hole-punched through NAT and the remote
        // peer is expecting the STUN-discovered mapping. Mirrors upstream
        // /tmp/3sxtra/src/netplay/netplay.c:440-444.
        // NetTuning_SetRecvBuf is a plain setsockopt — calling it again on
        // the configure_gekko() pass after the handshake pass is harmless.
        NetTuning_SetRecvBuf(stun_socket, 256 * 1024);
        return stun_socket;
    }
    NET_DatagramSocket* mm_sock = Matchmaking_GetSocket();
    if (mm_sock != NULL) {
        // Matchmaking path: reuse the socket that was registered with the server.
        return mm_sock;
    }
    // Direct P2P CLI/LAN path: create a dedicated UDP socket on local_port.
    if (p2p_sock == NULL) {
        NET_Init();
        p2p_sock = NET_CreateDatagramSocket(NULL, local_port);
    }
    return p2p_sock;
}

static void configure_gekko() {
    GekkoConfig config;
    SDL_zero(config);

    config.num_players = PLAYER_COUNT;
    config.input_size = sizeof(u16);
    /* Sparse effect-pool save (Option A) — `state_size` is the per-slot
     * fixed allocation Gekko's StateStorage performs (storage.cpp:16,
     * `make_unique<u8[]>(state_size)`). It is the *ceiling*, NOT the
     * per-frame size; that's reported separately via GekkoSave.state_len.
     *
     * We keep this at sizeof(State) so the full-state safety net (kill
     * switch OFF or active>SPARSE_CEILING_SLOTS) always fits in a slot
     * without overrunning Gekko's buffer. The savings under Option A
     * come from the per-frame state_len being typically ~143KB (sparse
     * format with ~50 active) instead of ~247KB. That shrinks:
     *   - the user-side memcpy in load_state_from_event,
     *   - the checksum + transmission bytes Gekko walks on each save,
     *   - net bandwidth on rollback resync.
     * Ring-buffer footprint stays at ~4.9MB; Gekko's per-slot allocation
     * does not vary with state_len.
     *
     * If a future refactor makes the full-state fallback unnecessary
     * (e.g. by enforcing the ceiling via the active-slot pool design),
     * lower this to SPARSE_CEILING_BYTES and the ring shrinks to ~2.8MB.
     * The _Static_assert below enforces the correctness invariant. */
    config.state_size = (unsigned int)sizeof(State);
    _Static_assert(sizeof(State) >= SPARSE_CEILING_BYTES,
                   "SPARSE_CEILING_BYTES exceeds sizeof(State) — sparse buffer "
                   "would overflow if state_size were sizeof(State). Re-check "
                   "the SPARSE_CEILING_SLOTS / SPARSE_HEADER_BYTES math in "
                   "game_state.h.");
    config.max_spectators = 0;
    int cfg_pred_window = Config_GetInt(CFG_KEY_NETPLAY_INPUT_PREDICTION_WINDOW);
    if (cfg_pred_window < 1 || cfg_pred_window > 32) cfg_pred_window = 8;
    config.input_prediction_window = (unsigned char)cfg_pred_window;

    // Unconditional: the focused checksum runs in Release too (Phase 3).
    // Research §19 risk 5: shipping without detection means silent desyncs.
    config.desync_detection = true;

    if (gekko_create(&session, GekkoGameSession)) {
        gekko_start(session, &config);
    } else {
        printf("Session is already running! probably incorrect.\n");
    }

    // Diagnostics: refresh per-session identifiers and read the runtime gate
    // once. Captured here so two back-to-back sessions in the same process
    // get distinct UUIDs in the log stream.
    SDL_SetAtomicInt(&s_diag_enabled, Config_GetBool(CFG_KEY_NETPLAY_DIAG_ENABLE) ? 1 : 0);
    {
        // 32-bit tag derived from monotonic ticks XOR PID — not cryptographic,
        // just unique enough within a peer's log stream so log lines from
        // session A vs B don't blur. Two peers will (almost certainly) pick
        // different UUIDs; cross-peer correlation uses session_started_unix_ms
        // (within a few seconds of each other) plus the role/handle.
        uint64_t now_ns = SDL_GetTicksNS();
        s_session_uuid = (uint32_t)(now_ns ^ (now_ns >> 32) ^ (uint32_t)getpid());
        if (s_session_uuid == 0) {
            s_session_uuid = 1;  // 0 reserved as "unset" sentinel.
        }
    }
    s_session_started_unix_ms = netplay_utc_ms();
    s_session_started_ticks_ms = SDL_GetTicks();
    s_session_end_reason[0] = '\0';
    s_session_end_frame = -1;
    // Tier-1 netplay diag — Item 5 (watchdog): seed last-advance to "now" so
    // the watchdog only fires once we've actually seen at least one advance
    // followed by a real stall. Items 3/6: zero per-session counters.
    s_last_advance_utc_ms = netplay_utc_ms();
    s_watchdog_latched = false;
    SDL_zeroa(s_hb_prev_pkt_tx);
    SDL_zeroa(s_hb_prev_pkt_rx);
    SDL_zeroa(s_rb_buckets);

    // Tier-1 netplay diag — Item 7: open the per-session netplay log file.
    // Block-buffered, fflush'd 1 Hz from the heartbeat. Filename includes
    // the session-start unix ms so cross-peer pairing is just matching ms.
    //
    // S3-review L-2: a PRE-session connection failure opens the log lazily
    // via Netplay_LogConnectEvent, and nothing on that path ever closes it
    // (netplay_log_close only runs from the EXITING/flush paths of a live
    // session) — so without this close, a later session's netplay_log_open
    // would early-return on the already-open handle and append the entire
    // session log to the failure-timestamped file. Retire any such
    // leftover log first; this session gets its own file.
    netplay_log_close();
    netplay_log_open(s_session_started_unix_ms);

    // Tier-1 netplay diag — Item 9: capture /proc/net/snmp UDP-row baseline
    // so peer-disconnect / session-end can emit deltas. Zeroed sample on
    // non-MiSTer hosts (read returns false; valid stays false).
    SDL_zero(s_udp_snmp_session_start);
    read_proc_net_snmp_udp(&s_udp_snmp_session_start);

    SDL_Log("[netplay sess=%08x] session-start utc_ms=%llu pred_window=%d state_size=%zu role=%s",
            s_session_uuid,
            (unsigned long long)s_session_started_unix_ms,
            (int)config.input_prediction_window,
            (size_t)sizeof(State),
            (player_number == 0) ? "host" : "joiner");

    NET_DatagramSocket* active_sock = acquire_active_socket();

#if defined(LOSSY_ADAPTER)
    configure_lossy_adapter(active_sock);
    gekko_net_adapter_set(session, &lossy_adapter);
#else
    gekko_net_adapter_set(session, SDLNetAdapter_Create(active_sock));
#endif

    printf("starting a session for player %d at port %hu (remote=%s:%hu)\n",
           player_number, local_port, remote_ip ? remote_ip : "(null)", remote_port);
    fflush(stdout);

    char remote_address_str[100];
    SDL_snprintf(remote_address_str, sizeof(remote_address_str), "%s:%hu", remote_ip, remote_port);
    GekkoNetAddress remote_address = { .data = remote_address_str, .size = strlen(remote_address_str) };

    for (int i = 0; i < PLAYER_COUNT; i++) {
        const bool is_local_player = (i == player_number);

        if (is_local_player) {
            player_handle = gekko_add_actor(session, GekkoLocalPlayer, NULL);
            gekko_set_local_delay(session, player_handle, DELAY_FRAMES);
        } else {
            gekko_add_actor(session, GekkoRemotePlayer, &remote_address);
        }
    }
}

static u16 get_inputs() {
    // The game doesn't differentiate between controllers and players.
    // That's why we OR the inputs of both local controllers together to get
    // local inputs.
    u16 inputs = 0;
    inputs = p1sw_buff | p2sw_buff;
    return inputs;
}

static void note_input(u16 input, int player, int frame) {
    if (frame < 0) {
        return;
    }

    input_history[player][frame % INPUT_HISTORY_MAX] = input;
}

static u16 recall_input(int player, int frame) {
    if (frame < 0) {
        return 0;
    }

    return input_history[player][frame % INPUT_HISTORY_MAX];
}

// Dump the input_history ring buffer to states/desync_F<frame>_pid<pid>_inputs.txt
// alongside the game-state dump. Most desyncs that aren't engine determinism
// bugs trace to "we executed different inputs at frame X" — diffing two peers'
// input histories at the desync frame catches that whole failure axis even
// when the state checksums look identical until the divergent frame surfaces.
static void dump_input_history_on_desync(int frame) {
    SDL_CreateDirectory("states");
    char path[256];
    SDL_snprintf(path, sizeof(path), "states/desync_F%d_pid%d_inputs.txt", frame, (int)getpid());
    FILE* f = fopen(path, "w");
    if (!f) {
        SDL_Log("[desync] ERROR: could not open %s for writing", path);
        return;
    }
    fprintf(f, "=== INPUT HISTORY at desync frame %d ===\n", frame);
    fprintf(f, "session_uuid=0x%08x  pid=%d  ring_size=%d\n\n",
            s_session_uuid, (int)getpid(), INPUT_HISTORY_MAX);
    fprintf(f, "%8s  %6s  %6s%s\n", "frame", "p1", "p2", "");
    // Walk newest-back-to-oldest within the ring (size 120 = 2s @ 60Hz).
    for (int i = 0; i < INPUT_HISTORY_MAX; i++) {
        int f_idx = frame - INPUT_HISTORY_MAX + 1 + i;
        if (f_idx < 0) continue;
        int slot = f_idx % INPUT_HISTORY_MAX;
        const char* marker = (f_idx == frame) ? " <== DESYNC" : "";
        fprintf(f, "%8d  0x%04x  0x%04x%s\n",
                f_idx,
                (unsigned)input_history[0][slot],
                (unsigned)input_history[1][slot],
                marker);
    }
    fclose(f);
    SDL_Log("[desync] Wrote input history (%d frames) to %s", INPUT_HISTORY_MAX, path);
}

// The rollback save/load/checksum pipeline lives in netplay/game_state.c
// (Phase 3 move). See game_state.h for the public API.

static bool game_ready_to_run_character_select() {
    return G_No[1] == 1;
}

static bool need_to_catch_up() {
    return frames_behind >= 1;
}

static void step_game(bool render) {
    No_Trans = !render;

    njUserMain();
    seqsBeforeProcess();
    njdp2d_draw();
    seqsAfterProcess();
}

static void advance_game(GekkoGameEvent* event, bool render) {
    const u16* inputs = (u16*)event->data.adv.inputs;
    const int frame = event->data.adv.frame;

    if (render) {
        s_last_advance_frame = frame;
    }

    p1sw_0 = PLsw[0][0] = inputs[0];
    p2sw_0 = PLsw[1][0] = inputs[1];
    p1sw_1 = PLsw[0][1] = recall_input(0, frame - 1);
    p2sw_1 = PLsw[1][1] = recall_input(1, frame - 1);

    note_input(inputs[0], 0, frame);
    note_input(inputs[1], 1, frame);

    step_game(render);
}

static void handle_disconnection() {
    if (session_state == NETPLAY_SESSION_EXITING || session_state == NETPLAY_SESSION_IDLE) {
        return;
    }

    clean_input_buffers();
    Soft_Reset_Sub();
    if (s_session_end_reason[0] == '\0') {
        SDL_strlcpy(s_session_end_reason, "disconnect", sizeof(s_session_end_reason));
        s_session_end_frame = s_last_advance_frame;
    }
    session_state = NETPLAY_SESSION_EXITING;
}

static void process_session() {
    frames_behind = -gekko_frames_ahead(session);

    gekko_network_poll(session);

    u16 local_inputs = get_inputs();
    gekko_add_local_input(session, player_handle, &local_inputs);

    // Tier-1 netplay diag — Item 5: GekkoAdvanceEvent watchdog. Fire once
    // per stall episode if no advance has happened in 250 ms. This is the
    // exact signal we lacked for the 17.6-min disconnect: friend's side
    // stopped advancing, peer-disconnect waited 5s for the timeout, no
    // logging in between. 250 ms = ~15 frames at 60 Hz; even a heavy
    // rollback chain returns to advance well inside that window.
    if (session_state == NETPLAY_SESSION_RUNNING && !s_watchdog_latched) {
        const uint64_t now_ms = netplay_utc_ms();
        if (now_ms > s_last_advance_utc_ms + 250) {
            const uint64_t stall_ms = now_ms - s_last_advance_utc_ms;
            char line[256];
            SDL_snprintf(line, sizeof(line),
                         "[netplay sess=%08x] WATCHDOG no-advance for %llu ms "
                         "(last_frame=%d frames_behind=%.1f recent_rb=%d)",
                         s_session_uuid,
                         (unsigned long long)stall_ms,
                         s_last_advance_frame,
                         (double)frames_behind,
                         (int)network_stats.rollback);
            netplay_log_line(line);
            s_watchdog_latched = true;
        }
    }

    int session_event_count = 0;
    GekkoSessionEvent** session_events = gekko_session_events(session, &session_event_count);

    for (int i = 0; i < session_event_count; i++) {
        const GekkoSessionEvent* event = session_events[i];

        switch (event->type) {
        case GekkoPlayerSyncing:
            printf("🔴 player syncing\n"); fflush(stdout);
            SDL_Log("[netplay sess=%08x] event=syncing", s_session_uuid);
            // FIXME: Show status to the player
            push_event(NETPLAY_EVENT_SYNCHRONIZING);
            break;

        case GekkoPlayerConnected:
            printf("🔴 player connected\n"); fflush(stdout);
            SDL_Log("[netplay sess=%08x] event=connected", s_session_uuid);
            push_event(NETPLAY_EVENT_CONNECTED);
            break;

        case GekkoPlayerDisconnected:
            printf("🔴 player disconnected\n"); fflush(stdout);
            SDL_Log("[netplay sess=%08x] event=peer-disconnected frame=%d",
                    s_session_uuid, s_last_advance_frame);
            SDL_strlcpy(s_session_end_reason, "peer-disconnected", sizeof(s_session_end_reason));
            s_session_end_frame = s_last_advance_frame;

            // Tier-1 netplay diag — Item 4: dump packet ring on disconnect
            // so we capture the most recent ~512 packets when the 5-second
            // GekkoNet timeout fires. Also Item 9: emit UDP-row deltas so
            // we can tell whether the kernel was dropping packets in the
            // silence window.
            {
                UdpSnmp now_snmp;
                SDL_zero(now_snmp);
                read_proc_net_snmp_udp(&now_snmp);
                if (now_snmp.valid && s_udp_snmp_session_start.valid) {
                    char line[256];
                    SDL_snprintf(line, sizeof(line),
                                 "[netplay sess=%08x] kernel-udp-stats: "
                                 "in_errors=%llu rcvbuf_errs=%llu noport_errs=%llu",
                                 s_session_uuid,
                                 (unsigned long long)(now_snmp.in_errors      - s_udp_snmp_session_start.in_errors),
                                 (unsigned long long)(now_snmp.rcvbuf_errors  - s_udp_snmp_session_start.rcvbuf_errors),
                                 (unsigned long long)(now_snmp.no_ports       - s_udp_snmp_session_start.no_ports));
                    netplay_log_line(line);
                }
                SDLNetAdapter_DumpPacketRing(
                    netplay_utc_ms(),
                    (player_number == 0) ? "host" : "joiner",
                    s_session_uuid);
            }

            push_event(NETPLAY_EVENT_DISCONNECTED);
            handle_disconnection();
            break;

        case GekkoSessionStarted:
            printf("🔴 session started\n"); fflush(stdout);
            SDL_Log("[netplay sess=%08x] event=session-started frame=%d",
                    s_session_uuid, s_last_advance_frame);
            session_state = NETPLAY_SESSION_RUNNING;
            // P-2.1 fix: re-seed last-advance now so the watchdog clock
            // starts from the RUNNING transition, not from CONNECTING-entry
            // in configure_gekko(). Multi-second handshakes would otherwise
            // make the first watchdog check after RUNNING fire spuriously.
            s_last_advance_utc_ms = netplay_utc_ms();
            s_watchdog_latched = false;
            break;

        case GekkoDesyncDetected:
        {
            const int frame = event->data.desynced.frame;
            const uint32_t local_cs = event->data.desynced.local_checksum;
            const uint32_t remote_cs = event->data.desynced.remote_checksum;
            printf("⚠️ desync detected at frame %d  local=0x%08x remote=0x%08x\n",
                   frame, local_cs, remote_cs);
            SDL_Log("[netplay sess=%08x] DESYNC frame=%d local=0x%08x remote=0x%08x",
                    s_session_uuid, frame, local_cs, remote_cs);
            // Dump pre-desync ring buffers + binary State to states/ for
            // offline triage. The dump function is post-event so it has no
            // per-frame perf cost; gating on s_diag_enabled lets a user
            // suppress the file writes if the SD card is constrained.
            if (SDL_GetAtomicInt(&s_diag_enabled)) {
                dump_desync_state(frame, local_cs, remote_cs);
                dump_input_history_on_desync(frame);
                // Tier-1 netplay diag — Item 4: also dump the packet ring
                // alongside the state/input dumps so all three artifacts
                // come out together. Useful for "did we miss an Inputs
                // packet right before the divergence?" investigations.
                SDLNetAdapter_DumpPacketRing(
                    netplay_utc_ms(),
                    (player_number == 0) ? "host-desync" : "joiner-desync",
                    s_session_uuid);
            }
            SDL_strlcpy(s_session_end_reason, "desync", sizeof(s_session_end_reason));
            s_session_end_frame = frame;
            // F8 — netplay Track A review finding. Port of 3sxtra's
            // GekkoDesyncDetected handler (/tmp/3sxtra/src/netplay/netplay.c:633-651):
            // treat a detected desync like a disconnect — push DISCONNECTED so
            // the lobby UI can react, then clean input buffers and soft-reset
            // via handle_disconnection(). Event queue arrived in Phase 6 Step 2
            // (see docs/plan-netplay-phase6.md).
            printf("[netplay] desync at frame %d — terminating session\n", frame);
            push_event(NETPLAY_EVENT_DISCONNECTED);
            handle_disconnection();
            break;
        }

        case GekkoEmptySessionEvent:
        case GekkoSpectatorPaused:
        case GekkoSpectatorUnpaused:
            // Do nothing
            break;
        }
    }
}

static void process_events(bool drawing_allowed) {
    int game_event_count = 0;
    GekkoGameEvent** game_events = gekko_update_session(session, &game_event_count);
    int frames_rolled_back = 0;

    for (int i = 0; i < game_event_count; i++) {
        const GekkoGameEvent* event = game_events[i];

        switch (event->type) {
        case GekkoLoadEvent:
#if DEBUG
            /* Black-BG investigation 2026-04-24 — Experiment 5a.
             * Count GekkoLoadEvents (rollback restores). Emit every 10th. */
            {
                static int s_load_count = 0;
                s_load_count++;
                if (s_load_count <= 40 || (s_load_count % 10) == 0) {
                    fprintf(stderr, "[netplay] GekkoLoadEvent #%d (rollback restore)\n", s_load_count);
                }
            }
#endif
            load_state_from_event(event);
            break;

        case GekkoAdvanceEvent:
            const bool rolling_back = event->data.adv.rolling_back;
            advance_game(event, drawing_allowed && !rolling_back);
            frames_rolled_back += rolling_back ? 1 : 0;
            // Tier-1 netplay diag — Item 5: stamp UTC ms of the most recent
            // advance regardless of whether it was the rolling-back leg.
            // Rollback advances are still progress; an absence of any advance
            // for >250 ms is what we want to flag.
            s_last_advance_utc_ms = netplay_utc_ms();
            if (s_watchdog_latched) {
                // Reset the latch so the next stall episode emits.
                s_watchdog_latched = false;
            }
            break;

        case GekkoSaveEvent:
            save_state(event);
            break;

        case GekkoEmptyGameEvent:
            // Do nothing
            break;
        }
    }

    frame_max_rollback = SDL_max(frame_max_rollback, frames_rolled_back);

    // Tier-1 netplay diag — Item 6: bucket per-batch rollback depth. Done
    // here (not inside the per-event switch) so a single update_session()
    // call producing N rolling-back advances is counted as one rollback of
    // depth N, matching what users observe.
    if (frames_rolled_back > 0) {
        // P-2.5 fix: bucket 0 is filled by the else branch below; the chain
        // here only runs when frames_rolled_back >= 1, so a final `else b=0`
        // would be unreachable.
        int b;
        if (frames_rolled_back >= 8)      b = 4;
        else if (frames_rolled_back >= 5) b = 3;
        else if (frames_rolled_back >= 3) b = 2;
        else                              b = 1;  // 1..2
        s_rb_buckets[b]++;
    } else {
        // Note: a "no rollback" advance batch still counts toward bucket 0
        // so the histogram has a meaningful baseline (no-rb % per second).
        s_rb_buckets[0]++;
    }
}

static void step_logic(bool drawing_allowed) {
    process_session();
    process_events(drawing_allowed);
}

static void update_network_stats() {
    if (stats_update_timer == 0) {
        GekkoNetworkStats net_stats;
        gekko_network_stats(session, player_handle ^ 1, &net_stats);

        network_stats.ping = net_stats.avg_ping;
        network_stats.delay = DELAY_FRAMES;

        if (frame_max_rollback < network_stats.rollback) {
            // Don't decrease the reading by more than a frame to account for
            // the opponent not pressing buttons for 1-2 seconds
            network_stats.rollback -= 1;
        } else {
            network_stats.rollback = frame_max_rollback;
        }

        // Lightweight telemetry: one heartbeat line per second so wrapper
        // logs show where the session was when a drop happened. Only emit
        // while actually in an active session to avoid pre-match noise.
        // The diag gate suppresses the noisy extras (kbps/jitter) but the
        // baseline stats still land — they cost nothing.
        if (session_state == NETPLAY_SESSION_RUNNING) {
            const bool diag = SDL_GetAtomicInt(&s_diag_enabled) != 0;

            // Tier-1 netplay diag — Item 2: kb_sent / kb_received are PER-SECOND
            // rates (gekkonet net.h:136-137 — kb_sent_per_sec / kb_received_per_sec
            // exposed via the GekkoNetworkStats accessor). Emit them as kbps_tx /
            // kbps_rx directly; the previous code subtracted from cached "last"
            // values, but the cached values were already rates, so the subtraction
            // produced spurious zero-or-negative numbers. Drop the s_hb_last_kb_*
            // statics entirely.
            const float kbps_tx = net_stats.kb_sent;
            const float kbps_rx = net_stats.kb_received;

            // Tier-1 netplay diag — Item 3: per-packet-type delta counters.
            uint64_t cur_tx[8] = { 0 }, cur_rx[8] = { 0 };
            SDLNetAdapter_SnapshotCounters(cur_tx, cur_rx);
            uint64_t d_tx[8], d_rx[8];
            for (int k = 0; k < 8; k++) {
                d_tx[k] = cur_tx[k] - s_hb_prev_pkt_tx[k];
                d_rx[k] = cur_rx[k] - s_hb_prev_pkt_rx[k];
            }
            SDL_memcpy(s_hb_prev_pkt_tx, cur_tx, sizeof(s_hb_prev_pkt_tx));
            SDL_memcpy(s_hb_prev_pkt_rx, cur_rx, sizeof(s_hb_prev_pkt_rx));

            // Item 6: snapshot + reset rb_buckets so the heartbeat carries the
            // delta since the last hb (matching the kbps semantics). Buckets
            // 0..4 = rb depth 0, 1-2, 3-4, 5-7, 8+.
            uint32_t rb_snap[5];
            SDL_memcpy(rb_snap, s_rb_buckets, sizeof(rb_snap));
            SDL_zeroa(s_rb_buckets);

            char line[512];
            if (diag) {
                SDL_snprintf(line, sizeof(line),
                        "[netplay sess=%08x] hb f=%d ping=%d jitter=%d "
                        "kbps_tx=%.1f kbps_rx=%.1f rb=%d behind=%.1f "
                        "tx=I:%llu,A:%llu,SH:%llu,NH:%llu "
                        "rx=I:%llu,A:%llu,SH:%llu,NH:%llu "
                        "rb_hist=0:%u,1:%u,2:%u,3:%u,4:%u",
                        s_session_uuid,
                        s_last_advance_frame,
                        (int)network_stats.ping,
                        (int)net_stats.jitter,
                        (double)kbps_tx,
                        (double)kbps_rx,
                        (int)network_stats.rollback,
                        (double)frames_behind,
                        (unsigned long long)d_tx[1],   // Inputs
                        (unsigned long long)d_tx[3],   // InputAck
                        (unsigned long long)d_tx[6],   // SessionHealth
                        (unsigned long long)d_tx[7],   // NetworkHealth
                        (unsigned long long)d_rx[1],
                        (unsigned long long)d_rx[3],
                        (unsigned long long)d_rx[6],
                        (unsigned long long)d_rx[7],
                        rb_snap[0], rb_snap[1], rb_snap[2], rb_snap[3], rb_snap[4]);
            } else {
                SDL_snprintf(line, sizeof(line),
                        "[netplay hb] f=%d ping=%d rb=%d behind=%.1f",
                        s_last_advance_frame,
                        (int)network_stats.ping,
                        (int)network_stats.rollback,
                        (double)frames_behind);
            }
            // Tier-1 netplay diag — Item 7: tee to the netplay log file.
            // Keep stderr surface for tail -f / wrapper-log workflows.
            fputs(line, stderr);
            fputc('\n', stderr);
            fflush(stderr);
            if (s_netplay_log != NULL) {
                fputs(line, s_netplay_log);
                fputc('\n', s_netplay_log);
                fflush(s_netplay_log);
            }
        }

        frame_max_rollback = 0;
        stats_update_timer = STATS_UPDATE_TIMER_MAX;
    }

    stats_update_timer -= 1;
    stats_update_timer = SDL_max(stats_update_timer, 0);
}

static void run_netplay() {
    // Step

    const bool catch_up = need_to_catch_up() && (frame_skip_timer == 0);
    step_logic(!catch_up);

    if (catch_up) {
        step_logic(true);
        frame_skip_timer = FRAME_SKIP_TIMER_MAX;
    }

    frame_skip_timer -= 1;
    frame_skip_timer = SDL_max(frame_skip_timer, 0);

    // Update stats

    update_network_stats();
}

void Netplay_SetParams(int player, const char* ip) {
    SDL_assert(player == 1 || player == 2);
    player_number = player - 1;

    // Callers may pass a stack-local buffer (e.g. direct_p2p's host path
    // copies NET_GetAddressString into `char src_ip[64]` before calling
    // do_handoff). Storing the raw pointer dangles by the time
    // configure_gekko() stringifies remote_address_str on a later frame.
    // Copy the string into module-local storage so remote_ip stays valid
    // for the lifetime of the netplay session.
    if (ip != NULL) {
        SDL_strlcpy(remote_ip_buf, ip, sizeof(remote_ip_buf));
        remote_ip = remote_ip_buf;
    } else {
        remote_ip_buf[0] = '\0';
        remote_ip = NULL;
    }

    if (ip != NULL && SDL_strcmp(ip, "127.0.0.1") == 0) {
        switch (player_number) {
        case 0:
            local_port = 50000;
            remote_port = 50001;
            break;

        case 1:
            local_port = 50001;
            remote_port = 50000;
            break;
        }
    } else {
        local_port = 50000;
        remote_port = 50000;
    }
}

void Netplay_SetRemotePort(unsigned short port) {
    remote_port = port;
}

bool Netplay_IsRemoteIpSet(void) {
    return remote_ip != NULL;
}

void Netplay_BeginDirectP2P() {
    if (remote_ip == NULL) {
        return;
    }
    direct_p2p_pending = true;
}

void Netplay_TickDirectP2P() {
    if (!direct_p2p_pending) {
        return;
    }

    // Cold-launch race: the wrapper-OSD "Host Game" path re-execs the
    // process with --direct-p2p-handoff, and the orchestrator can complete
    // (UPnP + STUN + hole-punch) while Init_Task is still in its
    // Aload/Warning/2nd phase. If setup_vs_mode() fires before
    // Init_Task_End runs, Init_Task_End clobbers G_No[0]=1 and registers
    // TASK_GAME late, dropping the game into Loop_Demo with G_No[1]=12 —
    // which falls through Loop_Demo's default case to Next_Demo_Loop,
    // permanently locking attract mode with session_state=TRANSITIONING
    // and the game engine unable to advance through Game12 → char select.
    // Defer until init finalizes so Game_Task is ready and G_No[0]=2
    // survives setup_vs_mode.
    if (task[TASK_INIT].condition != 0) {
        return;
    }

    direct_p2p_pending = false;
    setup_vs_mode();

    SDL_zeroa(input_history);
    frames_behind = 0;
    frame_skip_timer = 0;
    transition_ready_frames = 0;
    s_mist_handshake_done = false;
    s_mist_handshake_attempts = 0;
    s_mist_peer_hello_ok = false;

    session_state = NETPLAY_SESSION_TRANSITIONING;
}

void Netplay_SetMatchmakingParams(const char* server_ip, int server_port) {
    matchmaking_server_ip = server_ip;
    matchmaking_server_port = server_port;
}

void Netplay_BeginMatchmaking() {
    if (matchmaking_server_ip == NULL) {
        return;
    }
    // session_state stays IDLE so the menu keeps running normally.
    // setup_vs_mode() and the session transition happen in Netplay_TickMatchmaking.
    Matchmaking_Start(matchmaking_server_ip, matchmaking_server_port, 9001);
    matchmaking_pending = true;
}

void Netplay_TickMatchmaking() {
    if (!matchmaking_pending) {
        return;
    }

    Matchmaking_Run();

    const MatchmakingState mm = Matchmaking_GetState();

    if (mm == MATCHMAKING_MATCHED) {
        const MatchResult* r = Matchmaking_GetResult();
        player_number = r->player - 1;
        SDL_strlcpy(matched_ip, r->ip, sizeof(matched_ip));
        remote_ip = matched_ip;
        remote_port = (unsigned short)r->remote_port;
        SDL_zeroa(input_history);
        frames_behind = 0;
        frame_skip_timer = 0;
        transition_ready_frames = 0;
        s_mist_handshake_done = false;
        s_mist_handshake_attempts = 0;
        s_mist_peer_hello_ok = false;
        matchmaking_pending = false;
        setup_vs_mode();
        session_state = NETPLAY_SESSION_TRANSITIONING;
    } else if (mm == MATCHMAKING_ERROR) {
        matchmaking_pending = false;
        Soft_Reset_Sub();
    }
}

bool Netplay_IsMatchmakingPending() {
    // Returns false once matched so cancel is ignored during the display countdown.
    return matchmaking_pending && Matchmaking_GetState() != MATCHMAKING_MATCHED;
}

void Netplay_CancelMatchmaking() {
    if (Matchmaking_GetState() != MATCHMAKING_IDLE) {
        Matchmaking_Reset();
    }
    matchmaking_pending = false;
}

void Netplay_Run() {
    switch (session_state) {
    case NETPLAY_SESSION_TRANSITIONING:
        /* S3: user-reachable abort — the MIST retry window below can
         * legitimately spend up to ~20 s waiting on a slow-booting peer,
         * and pre-S3 there was no way out but killing the process. Check
         * BEFORE clean_input_buffers() so the pad state keyConvert()
         * captured this frame is still visible. */
        if (connect_abort_tick("transitioning")) {
            break;
        }
        if (game_ready_to_run_character_select()) {
            transition_ready_frames += 1;
        } else {
            transition_ready_frames = 0;
            /* Review L-5: the hold-START abort armed above is live during
             * this loading phase too — say so, like the verifying/syncing
             * texts do. (This phase is bounded ONLY by that abort: it
             * waits on game_ready_to_run_character_select with no
             * wall-clock deadline.) */
            SDL_snprintf(s_connect_status, sizeof(s_connect_status),
                         "Match found! Loading... START quits");
            clean_input_buffers();
            step_game(true);
        }

        if (transition_ready_frames >= 2) {
            // R-1 — Layer 3 MIST handshake, unconditional for every live
            // session path (direct-P2P punch, matchmaking, LAN CLI). Runs
            // AFTER the punch/handoff produced the session socket and
            // BEFORE gekko_create() takes it over, on the exact socket
            // configure_gekko() will hand to GekkoNet. Non-MIST frames
            // (stray punch keepalives, early GekkoNet packets from a
            // legacy peer) are dropped inside the runner by the "MIST"
            // magic gate. See mist_handshake.h for wire format.
            if (!s_mist_handshake_done) {
                SDL_snprintf(s_connect_status, sizeof(s_connect_status),
                             "Verifying opponent (%ds)... START quits",
                             s_mist_handshake_attempts / 2);
                // S3: incremental pump — one bounded slice per frame
                // instead of blocking the whole 500 ms attempt budget
                // (see the s_hs_pump block comment). The game keeps
                // rendering + sampling input at full frame rate for the
                // entire retry window.
                MistHandshakeResult hs = MIST_HS_FAIL;
                bool slice_pending = false;
                if (!s_hs_pump_active && !mist_pump_start()) {
                    // Local transport error — reason already cached;
                    // fold through the gate as a hard failure.
                    mist_pump_stop();
                } else {
                    const MistPumpStatus ps = mist_handshake_pump(
                        &s_hs_pump, &s_hs_io_template, &s_mist_peer_hello_ok,
                        s_mist_reject_reason, sizeof(s_mist_reject_reason));
                    if (ps == MIST_PUMP_PENDING) {
                        slice_pending = true;
                    } else {
                        s_hs_pump_active = false; // this attempt concluded
                        hs = (ps == MIST_PUMP_OK)        ? MIST_HS_OK
                             : (ps == MIST_PUMP_TIMEOUT) ? MIST_HS_TIMEOUT
                                                         : MIST_HS_FAIL;
                    }
                }
                if (slice_pending) {
                    break; // slice done; stay TRANSITIONING at full rate
                }
                // Retry policy (peer-skew tolerance, attempt cap, the
                // exhaustion message) lives in mist_handshake_gate_next
                // — extracted for unit testability (adv-review M-5).
                const MistGateAction act = mist_handshake_gate_next(
                    hs, &s_mist_handshake_attempts, MIST_HANDSHAKE_MAX_ATTEMPTS,
                    s_mist_reject_reason, sizeof(s_mist_reject_reason));
                if (act == MIST_GATE_PROCEED) {
                    s_mist_handshake_done = true;
                    mist_pump_stop();
                } else if (act == MIST_GATE_RETRY) {
                    // Silent peer — likely still booting toward its own
                    // gate (cold-launch skew). Stay in TRANSITIONING; the
                    // next tick re-arms the pump (the resolved peer
                    // address stays cached in s_hs_peer_addr). Each
                    // attempt keeps the 500 ms budget. An explicit
                    // reject never lands here.
                    break;
                } else {
                    mist_pump_stop();
                    printf("[netplay] MIST handshake failed: %s\n",
                           s_mist_reject_reason);
                    // Surface the reason on screen: latch it into the
                    // direct-P2P overlay BEFORE entering EXITING — the
                    // teardown callback that runs there converts the
                    // latch into DIRECT_P2P_FAILED_HANDSHAKE so the
                    // overlay shows ERROR + reason instead of a silent
                    // drop back to attract mode.
                    DirectP2P_NotifySessionRejected(s_mist_reject_reason);
                    push_event(NETPLAY_EVENT_DISCONNECTED);
                    session_state = NETPLAY_SESSION_EXITING;
                    clean_input_buffers();
                    Soft_Reset_Sub();
                    break;
                }
            }
            configure_gekko();
            session_state = NETPLAY_SESSION_CONNECTING;
            /* S3: arm the CONNECTING deadline + progress clock. */
            s_connecting_since_ms = SDL_GetTicks();
            s_connecting_last_log_ms = s_connecting_since_ms;
            s_abort_hold_frames = 0;
        }

        break;

    case NETPLAY_SESSION_CONNECTING: {
        /* S3 Part A: CONNECTING is now (a) user-abortable, (b) bounded
         * by a wall-clock deadline with an attributable reason, and
         * (c) log-visible via a 5 s progress line — see the S3 comment
         * block at the top of the file for why GekkoNet cannot be
         * trusted to time this state out itself. */
        if (connect_abort_tick("connecting")) {
            break;
        }
        const uint64_t now = SDL_GetTicks();
        if (s_connecting_since_ms == 0) { /* safety: entered without arming */
            s_connecting_since_ms = now;
            s_connecting_last_log_ms = now;
        }
        if (ConnectFail_DeadlineExpired(now, s_connecting_since_ms,
                                        CONNECT_TIMEOUT_CONNECTING_MS)) {
            // S3-review HIGH-2: tagged DEADLINE, not FAIL — the ONE
            // attributed "[netplay-connect] FAIL code=..." line for this
            // outcome is emitted by the orchestrator's FAILED_HANDSHAKE
            // report after the NotifySessionFailed latch below parks it
            // there at teardown. A second FAIL-tagged line here would
            // double-report the same terminal outcome.
            char line[192];
            SDL_snprintf(line, sizeof(line),
                         "[netplay-connect] DEADLINE code=%s stage=connecting — no "
                         "GekkoSessionStarted within %u ms (peer never synced); "
                         "attributed FAIL line follows from the teardown path",
                         ConnectFail_Code(CONNECT_FAIL_TIMEOUT_CONNECTING),
                         (unsigned)CONNECT_TIMEOUT_CONNECTING_MS);
            Netplay_LogConnectEvent(line);
            if (s_session_end_reason[0] == '\0') {
                SDL_strlcpy(s_session_end_reason, "connect-timeout",
                            sizeof(s_session_end_reason));
                s_session_end_frame = s_last_advance_frame;
            }
            /* Surface through the taxonomy path: the latch parks the
             * orchestrator in FAILED_HANDSHAKE at teardown so the
             * overlay shows ERROR + the reason on every entry path. */
            DirectP2P_NotifySessionFailed(CONNECT_FAIL_TIMEOUT_CONNECTING, NULL);
            push_event(NETPLAY_EVENT_DISCONNECTED);
            handle_disconnection();
            break;
        }
        const unsigned elapsed_s = (unsigned)((now - s_connecting_since_ms) / 1000u);
        if (now - s_connecting_last_log_ms >= 5000) {
            s_connecting_last_log_ms = now;
            SDL_Log("[netplay sess=%08x] still CONNECTING after %u s — waiting for "
                    "GekkoNet sync (deadline %u s)",
                    s_session_uuid, elapsed_s,
                    (unsigned)(CONNECT_TIMEOUT_CONNECTING_MS / 1000u));
        }
        SDL_snprintf(s_connect_status, sizeof(s_connect_status),
                     "Syncing with opponent (%us)... START quits", elapsed_s);
        run_netplay();
        break;
    }

    case NETPLAY_SESSION_RUNNING:
        /* S3: leaving CONNECTING — retire its bookkeeping/status. */
        if (s_connecting_since_ms != 0) {
            s_connecting_since_ms = 0;
            s_abort_hold_frames = 0;
            s_connect_status[0] = '\0';
        }
        run_netplay();
        break;

    case NETPLAY_SESSION_EXITING:
        // S3: retire the CONNECTING bookkeeping + status text so the next
        // session (and the idle screen) starts clean.
        s_connecting_since_ms = 0;
        s_connecting_last_log_ms = 0;
        s_abort_hold_frames = 0;
        s_connect_status[0] = '\0';
        // Final session-end summary. Logged once per session; gives offline
        // log readers a clean correlation point — pairing two friends' logs
        // is just matching session_started_unix_ms + the role-distinguished
        // UUID. Reason is set by whichever callsite initiated teardown
        // (peer-disconnected / desync / user-canceled / unknown if none).
        if (s_session_uuid != 0) {
            uint64_t dur_ms = (uint64_t)SDL_GetTicks() - s_session_started_ticks_ms;
            GekkoNetworkStats final_stats;
            SDL_zero(final_stats);
            if (session != NULL) {
                gekko_network_stats(session, player_handle ^ 1, &final_stats);
            }
            // Tier-1 netplay diag — Item 2: kb_sent / kb_received are PER-SECOND
            // rates (gekkonet net.h:136-137), not session totals. Label the
            // session-end fields explicitly as last-window rates so log
            // consumers don't confuse them with cumulative bytes.
            char line[512];
            SDL_snprintf(line, sizeof(line),
                    "[netplay sess=%08x] session-end reason=%s frame=%d duration_ms=%llu "
                    "final_ping=%d final_jitter=%d "
                    "last_window_kbps_tx=%.1f last_window_kbps_rx=%.1f",
                    s_session_uuid,
                    s_session_end_reason[0] ? s_session_end_reason : "unknown",
                    s_session_end_frame,
                    (unsigned long long)dur_ms,
                    (int)final_stats.avg_ping,
                    (int)final_stats.jitter,
                    (double)final_stats.kb_sent,
                    (double)final_stats.kb_received);
            netplay_log_line(line);

            // Tier-1 netplay diag — Item 9: final UDP-row delta on session-end.
            // P-1.2 fix: the peer-disconnect handler (process_session) already
            // emits this exact SNMP delta line just before transitioning to
            // EXITING. Skip it here to avoid double-logging the same numbers.
            // For all other end reasons (desync, user-canceled, unknown) the
            // disconnect handler did not run, so we still emit on this path.
            if (SDL_strcmp(s_session_end_reason, "peer-disconnected") != 0) {
                UdpSnmp end_snmp;
                SDL_zero(end_snmp);
                if (read_proc_net_snmp_udp(&end_snmp) && end_snmp.valid && s_udp_snmp_session_start.valid) {
                    char snmp_line[256];
                    SDL_snprintf(snmp_line, sizeof(snmp_line),
                                 "[netplay sess=%08x] kernel-udp-stats: "
                                 "in_errors=%llu rcvbuf_errs=%llu noport_errs=%llu",
                                 s_session_uuid,
                                 (unsigned long long)(end_snmp.in_errors      - s_udp_snmp_session_start.in_errors),
                                 (unsigned long long)(end_snmp.rcvbuf_errors  - s_udp_snmp_session_start.rcvbuf_errors),
                                 (unsigned long long)(end_snmp.no_ports       - s_udp_snmp_session_start.no_ports));
                    netplay_log_line(snmp_line);
                }
            }

            // Tier-1 netplay diag — Item 7: close the netplay log file once
            // we've emitted session-end + the final SNMP delta.
            netplay_log_close();

            s_session_uuid = 0;  // Mark closed so a stray re-entry doesn't double-log.
        }

        // Step 6 (plan P-2 #18): fire the orchestrator-registered teardown
        // callback before we touch the socket. The callback needs the STUN
        // socket (or at least its port via getsockname) valid to release
        // the UPnP mapping cleanly.
        if (session_teardown_cb != NULL) {
            session_teardown_cb();
        }

        if (session != NULL) {
            gekko_destroy(&session);
            SDLNetAdapter_Destroy();
        }

        // Step 6: destroy the pre-punched STUN socket (if any) now that
        // GekkoNet and the adapter have released their references. Mirrors
        // upstream /tmp/3sxtra/src/netplay/netplay.c:1009-1012.
        if (stun_socket != NULL) {
            NET_DestroyDatagramSocket(stun_socket);
            stun_socket = NULL;
        }

        if (p2p_sock != NULL) {
            NET_DestroyDatagramSocket(p2p_sock);
            p2p_sock = NULL;
            NET_Quit();
        }

        Netplay_CancelMatchmaking();
        // R-1: clear handshake state so the next session (direct P2P,
        // LAN, etc.) starts without carrying stale flags.
        // S3: also release anything the incremental pump still holds
        // (cached peer address / last datagram) — a hold-START abort or
        // menu exit can land here mid-handshake.
        mist_pump_stop();
        s_mist_handshake_done = false;
        s_mist_handshake_attempts = 0;
        s_mist_peer_hello_ok = false;

        // C1 fix: setup_vs_mode() (above) zeroes Convert_Buff/Check_Buff for
        // the duration of the session so the netplay simulation runs with
        // identity button mappings. Since Save_Game_Data() (sys_sub.c) reads
        // settings FROM Convert_Buff, any save that happens while those
        // buffers are still zeroed would persist zeroed settings
        // (Difficulty/pad mappings/volumes) to disk.
        //
        // The clean-exit path here has no reload-from-disk of its own
        // (unlike disconnect, which forces a hard Soft_Reset_Sub ->
        // TASK_INIT -> settings reload). NOTE (corrected 2026-08-23,
        // review C1 addendum): game.c Game06's post-game auto-save
        // (SaveInit(SAVE_FILE_SETTINGS, SAVE_MODE_SAVE), case 5) is NOT
        // reachable for a netplay session and so was never actually at risk
        // here — proven via manage.c: Game_Manage_1st (C_No[0]==0, called
        // every round from Game2_1 -> Game_Management()) sets
        // Round_Operator[p] = (plw[p].wu.wu_operator != 0) every round
        // (manage.c:194-203); netplay.c:371-372 unconditionally forces both
        // plw[].wu.wu_operator = 1, so Round_Operator[WINNER] is always true
        // in netplay; Game_Manage_10th's routing gate
        // (manage.c:1143, `Mode_Type == MODE_VERSUS || Mode_Type == 5 ||
        // Round_Operator[WINNER]`) is therefore always true for netplay
        // regardless of Mode_Type, so it always takes G_No[1]=3 (Game03,
        // whose own switch(Mode_Type) explicitly handles MODE_NETWORK via
        // the VS_Result menu screen, game.c ~774-798) and never
        // G_No[1]=4 (Game04, the arcade-only continue-screen flow that is
        // the only route to Game07/Game08, the sole two G_No[1]=6/Game06
        // writers, game.c:1251/1296). Game_Manage_10th's Check_Ending()
        // call is also unreachable for netplay for the same reason
        // (Play_Type==1, forced by both wu_operator being set, short-
        // circuits it at manage.c:1202 before it can route to Game08
        // either).
        //
        // The real, still-current motivation for this restore: leaving
        // Convert_Buff/Check_Buff zeroed here persists forward to the NEXT
        // real settings save after the player returns to the main menu —
        // e.g. Option_Select (menu.c:778), or Game06's auto-save the next
        // time the player plays Arcade/Versus locally (Game06 *is*
        // reachable then). Restore both buffers from the current save_w[1]
        // here, mirroring the same restore done at the other safe call
        // sites (boot: sys_sub.c Setup_IO_ConvData -> Copy_Save_w();
        // settings load: savesub.c deserialize_settings; menu defaults:
        // menu.c).
        Copy_Save_w();
        Copy_Check_w();

        // Release the netplay-session suppression of the draw-players-
        // above-HUD gameplay reads: the session is fully torn down, so
        // subsequent LOCAL play gets the user's configured value back.
        // (The old ForceDisable latch was never cleared — a menu-initiated
        // netplay session left the suppression on until relaunch.)
        DrawPlayersAboveHud_SetNetplaySuppressed(false);

        session_state = NETPLAY_SESSION_IDLE;
        break;

    case NETPLAY_SESSION_IDLE:
        break;
    }
}

NetplaySessionState Netplay_GetSessionState() {
    return session_state;
}

// S3 — honest connect-phase progress text for NetplayScreen_Render.
// Non-empty only while TRANSITIONING/CONNECTING have something to say;
// replaces the old perpetual "Match found!" (which stayed on screen for
// the entire MIST retry window and CONNECTING phase, lying about what
// was happening).
const char* Netplay_GetConnectStatusText(void) {
    if (session_state != NETPLAY_SESSION_TRANSITIONING &&
        session_state != NETPLAY_SESSION_CONNECTING) {
        return "";
    }
    return s_connect_status;
}

// Arm-time predicate: netplay arms ONLY in verified-arcade balance state.
// Balance auto-selects once at boot (ArcadeBalance_Init) and is fixed for
// the process, so this answer cannot change under a live session. Peers
// that both pass the gate still cross-check the adapted data via the MIST
// handshake balance digest.
bool Netplay_ArmAllowed(void) {
    return ArcadeBalance_IsEnabled();
}

// Refusal path shared by every entry site (nav arm, direct-P2P handoff
// dispatch, matchmaking CLI, in-game network menu): log, then surface the
// reason on screen through the existing direct-P2P overlay mechanism —
// the same route the MIST handshake reject path uses (ERROR + status
// text, rendered by NetplayScreen_Render -> DirectP2P_DrawOverlay).
void Netplay_RefuseArm(void) {
    char msg[120];
    SDL_snprintf(msg, sizeof(msg), "Netplay needs arcade balance - %s", ArcadeBalance_GetReason());
    // SDL_Log, not a bare printf: this fires during set_netplay_params()
    // at boot, long before any of the fflush(stdout) pairings elsewhere
    // in this file run, so a block-buffered stdout (any redirected or
    // piped run — i.e. every field log we actually receive) would swallow
    // the only record of WHY netplay refused. SDL_Log is unbuffered to
    // stderr and matches DirectP2P_RefuseSession's own line below it.
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[netplay] refusing to arm: %s", msg);
    DirectP2P_Init();
    DirectP2P_RefuseSession(msg);
}

void Netplay_HandleMenuExit() {
    Netplay_CancelMatchmaking();

    // A refused arm (or a post-session MIST reject) parks the direct-P2P
    // overlay in the terminal FAILED_HANDSHAKE state so the reason stays
    // readable. Leaving the network menu is the user acknowledging it —
    // clear the overlay so it does not sit over subsequent local play.
    // Only when no session exists: live-session teardown owns its own path.
    if (session_state == NETPLAY_SESSION_IDLE && DirectP2P_GetState() == DIRECT_P2P_FAILED_HANDSHAKE) {
        DirectP2P_Cancel();
    }

    switch (session_state) {
    case NETPLAY_SESSION_IDLE:
    case NETPLAY_SESSION_EXITING:
        break;

    case NETPLAY_SESSION_TRANSITIONING:
    case NETPLAY_SESSION_CONNECTING:
    case NETPLAY_SESSION_RUNNING:
        if (s_session_end_reason[0] == '\0') {
            SDL_strlcpy(s_session_end_reason, "user-canceled", sizeof(s_session_end_reason));
            s_session_end_frame = s_last_advance_frame;
        }
        session_state = NETPLAY_SESSION_EXITING;
        break;
    }
}

void Netplay_GetNetworkStats(NetworkStats* stats) {
    SDL_copyp(stats, &network_stats);
}

// Step 6 (docs/plan-stun-direct-p2p.md) — pre-punched STUN socket handoff.
// Mirrors /tmp/3sxtra/src/netplay/netplay.c:808-814 bodily. Ownership
// transfers from the caller (Step 7's orchestrator) into netplay.c. If we
// already hold a STUN socket and the caller is installing a different
// one, destroy the prior socket first to avoid a leak.
void Netplay_SetStunSocket(struct NET_DatagramSocket* socket) {
    NET_DatagramSocket* sock = (NET_DatagramSocket*)socket;
    if (stun_socket != NULL && stun_socket != sock) {
        NET_DestroyDatagramSocket(stun_socket);
    }
    stun_socket = sock;
}

// Step 6 (plan P-2 #18) — single-slot teardown callback.
void Netplay_SetSessionTeardownCallback(void (*cb)(void)) {
    session_teardown_cb = cb;
}

// S3 (docs/plan-netplay-connection.md §5) — connection-establishment
// event logger. Most connection failures happen BEFORE configure_gekko()
// opens the per-session netplay log, so this opens it lazily: a failed
// attempt still leaves a netplay-<utc_ms>.log on disk carrying the
// attributed failure line + stage timings, which is exactly what a field
// report needs. Tees to SDL_Log via netplay_log_line and flushes
// immediately (failure paths have no heartbeat to drive the 1 Hz flush).
// MAIN THREAD ONLY — the log FILE* is unguarded; direct_p2p.c calls this
// exclusively from DirectP2P_Tick's reporting path.
void Netplay_LogConnectEvent(const char* line) {
    if (line == NULL || line[0] == '\0') {
        return;
    }
    if (s_netplay_log == NULL) {
        netplay_log_open(netplay_utc_ms());
    }
    netplay_log_line(line);
    if (s_netplay_log != NULL) {
        fflush(s_netplay_log);
    }
}

// === Tier-1 netplay diag — Item 10: SIGTERM flush hook ===
// main.c calls this after its main loop exits but before SDL teardown so
// the diagnostics survive wrapper-SIGTERM. Idempotent: a guard on
// s_session_uuid keeps it from double-running if the regular EXITING path
// already cleaned up.
void Netplay_FlushDiagnostics(void) {
    // No active session — nothing to flush. The s_session_uuid==0 sentinel
    // is set in the EXITING branch after final-summary emission.
    if (s_session_uuid == 0 && s_netplay_log == NULL) {
        return;
    }

    if (s_session_uuid != 0) {
        // Best-effort final heartbeat. We don't have fresh GekkoNetworkStats
        // (the session may already be torn down or partially gone), so just
        // record the last observed advance frame + frames_behind.
        char line[256];
        SDL_snprintf(line, sizeof(line),
                     "[netplay sess=%08x] sigterm-flush last_frame=%d "
                     "frames_behind=%.1f reason=%s",
                     s_session_uuid,
                     s_last_advance_frame,
                     (double)frames_behind,
                     s_session_end_reason[0] ? s_session_end_reason : "sigterm");
        netplay_log_line(line);

        // Item 9: SNMP UDP-row delta on shutdown.
        UdpSnmp end_snmp;
        SDL_zero(end_snmp);
        if (read_proc_net_snmp_udp(&end_snmp) && end_snmp.valid && s_udp_snmp_session_start.valid) {
            char snmp_line[256];
            SDL_snprintf(snmp_line, sizeof(snmp_line),
                         "[netplay sess=%08x] kernel-udp-stats: "
                         "in_errors=%llu rcvbuf_errs=%llu noport_errs=%llu",
                         s_session_uuid,
                         (unsigned long long)(end_snmp.in_errors      - s_udp_snmp_session_start.in_errors),
                         (unsigned long long)(end_snmp.rcvbuf_errors  - s_udp_snmp_session_start.rcvbuf_errors),
                         (unsigned long long)(end_snmp.no_ports       - s_udp_snmp_session_start.no_ports));
            netplay_log_line(snmp_line);
        }

        // Item 4: dump packet ring with a "sigterm" role tag so post-mortem
        // readers can distinguish the dump source from peer-disconnect /
        // desync dumps.
        SDLNetAdapter_DumpPacketRing(
            netplay_utc_ms(),
            (player_number == 0) ? "host-sigterm" : "joiner-sigterm",
            s_session_uuid);
    }

    // Item 7: flush + close the netplay log file.
    netplay_log_close();

    // P-2.2 fix: clear the session-uuid sentinel so a second call to this
    // function is a no-op. Mirrors the EXITING branch's post-flush zeroing.
    s_session_uuid = 0;
}

// === 3SX-private extensions ===
// Phase 6 Step 2: port of /tmp/3sxtra/src/netplay/netplay.c:1088-1121.
// Single-producer (netplay thread) / single-consumer (main game-loop poll)
// contract inherited from upstream; no mutex.

#define EVENT_QUEUE_MAX 8
static NetplayEvent event_queue[EVENT_QUEUE_MAX];
static int event_queue_count = 0;

static void push_event(NetplayEventType type) {
    if (event_queue_count < EVENT_QUEUE_MAX) {
        event_queue[event_queue_count].type = type;
        event_queue_count++;
    }
}

bool Netplay_PollEvent(NetplayEvent* out) {
    if (!out || event_queue_count == 0)
        return false;
    *out = event_queue[0];
    // shift queue
    for (int i = 1; i < event_queue_count; i++) {
        event_queue[i - 1] = event_queue[i];
    }
    event_queue_count--;
    return true;
}

#ifdef ENABLE_NETPLAY_TESTS
// Test-only hook so src/netplay/test_event_queue.c can reach the static
// push_event(). Not declared in netplay.h; test TU forward-declares it.
// Gated behind ENABLE_NETPLAY_TESTS so release builds don't carry the
// symbol. Enable with
// EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS".
void Netplay_Test_PushEvent(NetplayEventType type) {
    push_event(type);
}
#endif
