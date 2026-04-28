#include "netplay/netplay.h"
#include "main.h"
#include "port/config/config.h"
#include "netplay/game_state.h"
#include "netplay/matchmaking.h"
#include "netplay/mist_handshake.h"
#include "netplay/net_tuning.h"
#include "netplay/sdl_net_adapter.h"
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
// Cumulative kb_sent / kb_received from the last heartbeat; used to compute
// per-second deltas without changing GekkoNet's accounting.
static float s_hb_last_kb_sent = 0.0f;
static float s_hb_last_kb_received = 0.0f;
static uint64_t s_hb_last_ticks_ms = 0;
// Reason buffer populated by callsites that initiate teardown (peer drop,
// desync, user cancel) and consumed by the EXITING-state session-end log.
static char s_session_end_reason[64] = { 0 };
static int s_session_end_frame = -1;

// Phase 6 Step 8: lobby-session gate for Layer 3 MIST handshake.
// Dormant since the RmlUi lobby UI was removed — no code path sets
// s_lobby_session = true anymore, so the handshake gate at
// maybe_run_mist_handshake() never fires. The state is retained so the
// MIST handshake machinery (mist_handshake*) stays structurally intact
// and can be re-armed when a future matchmaking surface lands.
static bool s_lobby_session = false;
static bool s_mist_handshake_done = false;
static char s_mist_reject_reason[128] = { 0 };

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
    Present_Mode = MODE_NETWORK;
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

// Phase 6 Step 8 — run the MIST handshake on our SDL_net matchmaking
// socket before gekko_create() takes it over. Mirrors tier-2 §8.2.4.
// Returns true on ack; false on reject or timeout (reason cached in
// s_mist_reject_reason).
static bool run_mist_handshake_on_net_sock(NET_DatagramSocket* sock) {
    if (sock == NULL || remote_ip == NULL || remote_port == 0) {
        SDL_strlcpy(s_mist_reject_reason, "missing transport state", sizeof(s_mist_reject_reason));
        return false;
    }
    NET_Address* peer = NET_ResolveHostname(remote_ip);
    if (peer == NULL) {
        SDL_strlcpy(s_mist_reject_reason, "peer resolve failed", sizeof(s_mist_reject_reason));
        return false;
    }
    /* Wait briefly for resolution. */
    for (int spin = 0; spin < 50; spin++) {
        if (NET_GetAddressStatus(peer) != NET_WAITING) break;
        SDL_Delay(2);
    }
    if (NET_GetAddressStatus(peer) != NET_SUCCESS) {
        NET_UnrefAddress(peer);
        SDL_strlcpy(s_mist_reject_reason, "peer resolve timed out", sizeof(s_mist_reject_reason));
        return false;
    }

    uint8_t hello[MIST_FRAME_MAX];
    const size_t hello_len = mist_handshake_build_hello(hello, sizeof(hello));
    if (hello_len == 0) {
        NET_UnrefAddress(peer);
        SDL_strlcpy(s_mist_reject_reason, "hello build failed", sizeof(s_mist_reject_reason));
        return false;
    }

    const Uint64 start_ms = SDL_GetTicks();
    const Uint64 deadline_ms = start_ms + (Uint64)MIST_DEFAULT_TIMEOUT_MS;
    int sends = 0;
    Uint64 next_send_ms = start_ms;

    for (;;) {
        const Uint64 now = SDL_GetTicks();
        if (now >= deadline_ms) {
            SDL_strlcpy(s_mist_reject_reason, "timeout (peer did not respond)",
                        sizeof(s_mist_reject_reason));
            NET_UnrefAddress(peer);
            return false;
        }
        if (now >= next_send_ms && sends < MIST_RETRANSMIT_COUNT) {
            NET_SendDatagram(sock, peer, remote_port, hello, (int)hello_len);
            sends++;
            next_send_ms = now + MIST_RETRANSMIT_INTERVAL_MS;
        }

        /* Drain pending datagrams. */
        NET_Datagram* dgram = NULL;
        while (NET_ReceiveDatagram(sock, &dgram) && dgram) {
            const int cls = mist_handshake_parse_response((const uint8_t*)dgram->buf,
                                                          (size_t)dgram->buflen);
            if (cls == 1) {
                NET_DestroyDatagram(dgram);
                NET_UnrefAddress(peer);
                return true;
            }
            if (cls == -1) {
                SDL_strlcpy(s_mist_reject_reason, mist_handshake_last_reject_reason(),
                            sizeof(s_mist_reject_reason));
                NET_DestroyDatagram(dgram);
                NET_UnrefAddress(peer);
                return false;
            }
            if (cls == 0) {
                /* Peer also called send_and_wait — reply with ack/reject. */
                uint8_t reply[MIST_FRAME_MAX];
                const size_t reply_len = mist_handshake_build_reply(
                    (const uint8_t*)dgram->buf + MIST_HEADER_LEN,
                    (size_t)dgram->buflen - MIST_HEADER_LEN,
                    reply, sizeof(reply));
                if (reply_len > 0) {
                    NET_SendDatagram(sock, dgram->addr, dgram->port, reply, (int)reply_len);
                }
            }
            /* cls == -2 → not a MIST frame; drop and keep listening. */
            NET_DestroyDatagram(dgram);
        }

        SDL_Delay(5); /* keep the loop from burning 100% CPU */
    }
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
    s_session_started_unix_ms = (uint64_t)time(NULL) * 1000ULL;
    s_session_started_ticks_ms = SDL_GetTicks();
    s_hb_last_kb_sent = 0.0f;
    s_hb_last_kb_received = 0.0f;
    s_hb_last_ticks_ms = s_session_started_ticks_ms;
    s_session_end_reason[0] = '\0';
    s_session_end_frame = -1;
    SDL_Log("[netplay sess=%08x] session-start utc_ms=%llu pred_window=%d state_size=%zu role=%s",
            s_session_uuid,
            (unsigned long long)s_session_started_unix_ms,
            cfg_pred_window,
            (size_t)sizeof(State),
            (player_number == 0) ? "host" : "joiner");

    NET_DatagramSocket* mm_sock = Matchmaking_GetSocket();
    NET_DatagramSocket* active_sock;
    if (stun_socket != NULL) {
        // Step 6: pre-punched STUN socket (internet play via orchestrator).
        // Takes priority over matchmaking — if the STUN socket is set, the
        // orchestrator has already hole-punched through NAT and the remote
        // peer is expecting the STUN-discovered mapping. Mirrors upstream
        // /tmp/3sxtra/src/netplay/netplay.c:440-444.
        NetTuning_SetRecvBuf(stun_socket, 256 * 1024);
        active_sock = stun_socket;
    } else if (mm_sock != NULL) {
        // Matchmaking path: reuse the socket that was registered with the server.
        active_sock = mm_sock;
    } else {
        // Direct P2P path: create a dedicated UDP socket on local_port.
        NET_Init();
        p2p_sock = NET_CreateDatagramSocket(NULL, local_port);
        active_sock = p2p_sock;
    }

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
            push_event(NETPLAY_EVENT_DISCONNECTED);
            handle_disconnection();
            break;

        case GekkoSessionStarted:
            printf("🔴 session started\n"); fflush(stdout);
            SDL_Log("[netplay sess=%08x] event=session-started frame=%d",
                    s_session_uuid, s_last_advance_frame);
            session_state = NETPLAY_SESSION_RUNNING;
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
            uint64_t now_ms = SDL_GetTicks();
            float dt_s = (now_ms > s_hb_last_ticks_ms)
                ? (float)(now_ms - s_hb_last_ticks_ms) / 1000.0f
                : 1.0f;
            float kbps_tx = (net_stats.kb_sent - s_hb_last_kb_sent) / dt_s;
            float kbps_rx = (net_stats.kb_received - s_hb_last_kb_received) / dt_s;
            s_hb_last_kb_sent = net_stats.kb_sent;
            s_hb_last_kb_received = net_stats.kb_received;
            s_hb_last_ticks_ms = now_ms;

            if (diag) {
                fprintf(stderr,
                        "[netplay sess=%08x] hb f=%d ping=%d jitter=%d kbps_tx=%.1f kbps_rx=%.1f rb=%d behind=%.1f\n",
                        s_session_uuid,
                        s_last_advance_frame,
                        (int)network_stats.ping,
                        (int)net_stats.jitter,
                        (double)kbps_tx,
                        (double)kbps_rx,
                        (int)network_stats.rollback,
                        (double)frames_behind);
            } else {
                fprintf(stderr,
                        "[netplay hb] f=%d ping=%d rb=%d behind=%.1f\n",
                        s_last_advance_frame,
                        (int)network_stats.ping,
                        (int)network_stats.rollback,
                        (double)frames_behind);
            }
            fflush(stderr);
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
        if (game_ready_to_run_character_select()) {
            transition_ready_frames += 1;
        } else {
            transition_ready_frames = 0;
            clean_input_buffers();
            step_game(true);
        }

        if (transition_ready_frames >= 2) {
            // Phase 6 Step 8 — Layer 3 MIST handshake. Only runs for
            // lobby-matched sessions; offline / direct P2P / LAN skip.
            // See docs/plan-netplay-port.md §8.2.4 for wire format.
            if (s_lobby_session && !s_mist_handshake_done) {
                NET_DatagramSocket* mm_sock = Matchmaking_GetSocket();
                if (!run_mist_handshake_on_net_sock(mm_sock)) {
                    printf("[netplay] MIST handshake failed: %s\n",
                           s_mist_reject_reason);
                    push_event(NETPLAY_EVENT_DISCONNECTED);
                    session_state = NETPLAY_SESSION_EXITING;
                    clean_input_buffers();
                    Soft_Reset_Sub();
                    break;
                }
                s_mist_handshake_done = true;
            }
            configure_gekko();
            session_state = NETPLAY_SESSION_CONNECTING;
        }

        break;

    case NETPLAY_SESSION_CONNECTING:
    case NETPLAY_SESSION_RUNNING:
        run_netplay();
        break;

    case NETPLAY_SESSION_EXITING:
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
            SDL_Log("[netplay sess=%08x] session-end reason=%s frame=%d duration_ms=%llu "
                    "final_ping=%d final_jitter=%d kb_sent=%.1f kb_received=%.1f",
                    s_session_uuid,
                    s_session_end_reason[0] ? s_session_end_reason : "unknown",
                    s_session_end_frame,
                    (unsigned long long)dur_ms,
                    (int)final_stats.avg_ping,
                    (int)final_stats.jitter,
                    (double)final_stats.kb_sent,
                    (double)final_stats.kb_received);
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
        // Phase 6 Step 8: clear lobby/handshake state so the next session
        // (direct P2P, LAN, etc.) starts without carrying stale flags.
        s_lobby_session = false;
        s_mist_handshake_done = false;
        session_state = NETPLAY_SESSION_IDLE;
        break;

    case NETPLAY_SESSION_IDLE:
        break;
    }
}

NetplaySessionState Netplay_GetSessionState() {
    return session_state;
}

void Netplay_HandleMenuExit() {
    Netplay_CancelMatchmaking();

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
