#include "main.h"
#include "arcade/arcade_balance.h"
#include "args.h"
#include "common.h"
#include "configuration.h"
#include "netplay/direct_p2p.h"
#include "netplay/direct_p2p_handoff.h"
#include "netplay/netplay.h"
#include "netplay/netplay_nav.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/AcrSDK/common/mlPAD.h"
#include "sf33rd/AcrSDK/ps2/flps2debug.h"
#include "sf33rd/AcrSDK/ps2/flps2etc.h"
#include "sf33rd/AcrSDK/ps2/flps2render.h"
#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "sf33rd/Source/Common/MemMan.h"
#include "sf33rd/Source/Common/PPGFile.h"
#include "sf33rd/Source/Common/PPGWork.h"
#include "sf33rd/Source/Compress/zlibApp.h"
#include "sf33rd/Source/Game/debug/Debug.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/init3rd.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/io/ioconv.h"
#include "sf33rd/Source/Game/menu/menu.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "sf33rd/Source/Game/rendering/dc_ghost.h"
#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/rendering/texcash.h"
#include "sf33rd/Source/Game/sound/sound3rd.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/ramcnt.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/sys_sub2.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/frame_data_overlay.h"
#include "sf33rd/Source/Game/ui/frame_trace.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"
#include "structs.h"
#include "test/test_runner.h"

#if DEBUG
#include "sf33rd/Source/Game/debug/debug_config.h"
#endif

#include "port/io/afs.h"
#include "port/linux/console_mode.h"
#include "port/resources.h"

#include <SDL3/SDL.h>

#if _WIN32 && DEBUG
// Including windows.h causes conflicts with the Polygon struct, so I just included the header where
// AllocConsole is and the Windows-specific typedefs that it requires.
#include <windef.h>

#include <ConsoleApi.h>
#endif

#include <memory.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

typedef enum MainPhase {
    MAIN_PHASE_INIT,
    MAIN_PHASE_COPYING_RESOURCES,
    MAIN_PHASE_INITIALIZED,
} MainPhase;

s32 system_init_level;
MPP mpp_w;
Configuration configuration = {
    .test =
        {
            .scene_preset = NULL,
            .characters = { -1, -1 },
            .super_arts = { -1, -1 },
            .initial_super_full = false,
            .training_sa_gauge = -1,
            .preserve_game_transition = false,
            .delay_gameplay_inputs_until_active = false,
            .stage = -1,
        },
};

static u8 dctex_linear_mem[0x800];
static u8 texcash_melt_buffer_mem[0x1000];
static u8 tpu_free_mem[0x2000];
static MainPhase phase = MAIN_PHASE_INIT;

static volatile sig_atomic_t shutdown_signal = 0;
static volatile sig_atomic_t fps_toggle_requested = 0;
static volatile sig_atomic_t arm_clock_cycle_requested = 0;
static volatile sig_atomic_t game_mode_cycle_requested = 0;
static volatile sig_atomic_t hold_to_pause_cycle_requested = 0;

static u8* mppMalloc(u32 size) {
    return flAllocMemory(size);
}

// Signal-safe: emit "[3sx] signal N\n" to stderr. Uses write() + manual
// itoa because fprintf/printf are not async-signal-safe. Logs only once
// per fatal-class signal so we can distinguish wrapper-kill (SIGTERM)
// from internal abort in post-mortem wrapper logs.
#if !defined(_WIN32)
static void log_shutdown_signal_safe(int signo) {
    char buf[32];
    static const char prefix[] = "[3sx] signal ";
    size_t pos = 0;
    memcpy(buf, prefix, sizeof(prefix) - 1);
    pos += sizeof(prefix) - 1;

    int n = signo;
    if (n < 0) { buf[pos++] = '-'; n = -n; }
    char digits[8];
    int d = 0;
    do { digits[d++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0 && d < (int)sizeof(digits));
    while (d > 0) { buf[pos++] = digits[--d]; }
    buf[pos++] = '\n';
    (void)!write(STDERR_FILENO, buf, pos);
}
#endif

static void on_shutdown_signal(int signo) {
    if (signo == SIGUSR1) {
        fps_toggle_requested = 1;
        return;
    }

#ifdef SIGRTMIN
    if (signo == SIGRTMIN + 2) {
        arm_clock_cycle_requested = 1;
        return;
    }

    if (signo == SIGRTMIN + 3) {
        game_mode_cycle_requested = 1;
        return;
    }

    if (signo == SIGRTMIN + 4) {
        hold_to_pause_cycle_requested = 1;
        return;
    }
#endif

#if !defined(_WIN32)
    log_shutdown_signal_safe(signo);
#endif
    shutdown_signal = signo;
}

static void install_shutdown_signal_handlers() {
    struct sigaction action;
    SDL_zero(action);
    action.sa_handler = on_shutdown_signal;
    sigemptyset(&action.sa_mask);

    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGUSR1, &action, NULL);
#ifdef SIGRTMIN
    sigaction(SIGRTMIN + 2, &action, NULL);
    sigaction(SIGRTMIN + 3, &action, NULL);
    sigaction(SIGRTMIN + 4, &action, NULL);
#endif
}

static void restore_shutdown_signal_handlers() {
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
#ifdef SIGRTMIN
    signal(SIGRTMIN + 2, SIG_DFL);
    signal(SIGRTMIN + 3, SIG_DFL);
    signal(SIGRTMIN + 4, SIG_DFL);
#endif
}

// Initialization

#if defined(ENABLE_NETPLAY)
/*
 * Step 9 of docs/plan-stun-direct-p2p.md — Resolve the handoff source
 * path (CLI flag takes priority, config-default HANDOFF_PATH is the
 * fallback). Returns NULL when there's nothing to dispatch.
 */
static const char* resolve_direct_p2p_handoff_path(void) {
    if (configuration.netplay.direct_p2p_handoff_set) {
        return configuration.netplay.direct_p2p_handoff_path;
    }
    const char* cfg_path = DirectP2PHandoff_ConfigDefaultPath();
    if (cfg_path == NULL || cfg_path[0] == '\0') {
        return NULL;
    }
    if (!DirectP2PHandoff_FileExists(cfg_path)) {
        return NULL;
    }
    return cfg_path;
}

/*
 * Step 9 dispatch — read the handoff file, consume it (unlink), then
 * invoke DirectP2P_BeginHost / DirectP2P_BeginJoin based on the parsed
 * mode. main.c keeps both call sites visible so the dispatch wiring is
 * greppable; the parsing itself lives in src/netplay/direct_p2p_handoff.c.
 */
static void dispatch_direct_p2p_handoff(void) {
    const char* path = resolve_direct_p2p_handoff_path();
    if (path == NULL) {
        return;
    }

    DirectP2PHandoff handoff;
    if (!DirectP2PHandoff_ReadFile(path, &handoff)) {
        return;
    }

    /* One-shot: drop the file before we kick off a worker thread so a
     * mid-session SIGKILL can't leave a stale handoff on disk. The
     * wrapper's fork/execve ordering guarantees the file was fully
     * written before this child even started, so unlink-then-dispatch
     * is race-free. */
    DirectP2PHandoff_Consume(path);

    switch (handoff.mode) {
    case DIRECT_P2P_HANDOFF_MODE_HOST:
        fprintf(stderr, "[direct_p2p_handoff] dispatching Host (port=%d)\n", handoff.port);
        DirectP2P_BeginHost(handoff.port);
        break;
    case DIRECT_P2P_HANDOFF_MODE_JOIN:
        fprintf(stderr, "[direct_p2p_handoff] dispatching Join (peer_code=%s)\n", handoff.peer_code);
        DirectP2P_BeginJoin(handoff.peer_code);
        break;
    case DIRECT_P2P_HANDOFF_MODE_NONE:
    default:
        break;
    }
}
#endif

static void set_netplay_params() {
#if defined(ENABLE_NETPLAY)
    if (configuration.netplay.p2p_remote_ip != NULL) {
        Netplay_SetParams(configuration.netplay.p2p_local_player, configuration.netplay.p2p_remote_ip);
        /* Netplay_SetParams already wired remote_ip, so the nav state
         * machine's NAV_WAIT_ORCHESTRATOR state will see
         * Netplay_IsRemoteIpSet() true immediately and only gate on
         * the menu-nav frames above it. */
        NetplayNav_Arm();
    } else if (configuration.netplay.matchmaking_ip != NULL) {
        Netplay_SetMatchmakingParams(configuration.netplay.matchmaking_ip, configuration.netplay.matchmaking_port);
    } else {
        /* Direct-P2P dispatch is deferred to the main game loop tick. The
         * orchestrator's worker thread publishes state transitions the
         * overlay renderer reads; if those happen before njUserInit()
         * completes ppg_Initialize the overlay's SSPutStrPro path hits
         * an uninitialized sprite bank and segfaults. Initialize the
         * orchestrator here (no worker spawned) so DirectP2P_Tick has
         * valid state from frame 0; the actual BeginHost/BeginJoin fires
         * once on first tick. See defer_direct_p2p_handoff_tick(). */
        DirectP2P_Init();
        /* Arm nav ONLY when a handoff file actually exists — a normal
         * cold OSD launch with no handoff is indistinguishable from the
         * "netplay requested" case until we check the file system, and
         * arming nav in the plain-boot case makes "CONNECTING..."
         * appear and nav synthesize Start presses even though no peer
         * is coming. resolve_direct_p2p_handoff_path() returns NULL
         * when neither the --direct-p2p-handoff CLI flag was set nor
         * the config default path has a file on disk. */
        if (resolve_direct_p2p_handoff_path() != NULL) {
            NetplayNav_Arm();
        }
    }
#endif
}

#if defined(ENABLE_NETPLAY)
/* One-shot: on the first game-loop tick, read the handoff file and kick
 * off Host/Join. By this point njUserInit() has run and the sprite bank
 * / ppg list is ready, so any state transition the worker publishes can
 * be safely rendered by the overlay. */
static void defer_direct_p2p_handoff_tick(void) {
    static bool dispatched = false;
    if (dispatched) return;
    dispatched = true;
    if (configuration.netplay.matchmaking_ip != NULL) return;
    if (configuration.netplay.p2p_remote_ip != NULL) {
        // LAN/localhost direct-P2P path: Netplay_SetParams already wired
        // remote_ip/local_port/remote_port via set_netplay_params, and
        // set_netplay_params() also armed the nav state machine. The nav
        // module now owns the Netplay_BeginDirectP2P() call — it fires
        // only after Title -> Mode Select -> Versus have played out via
        // injected Start presses so char-select init side-effects run.
        return;
    }
    dispatch_direct_p2p_handoff();
}
#endif

void cpInitTask() {
    memset(&task, 0, sizeof(task));
}

Language Get_Default_Language() {
    int locale_count;
    SDL_Locale** locales = SDL_GetPreferredLocales(&locale_count);

    if (locales == NULL) {
        return LANG_ENGLISH;
    }

    Language language = LANG_ENGLISH;

    for (int i = 0; i < locale_count; i++) {
        if (SDL_strcmp(locales[i]->language, "ja") == 0) {
            language = LANG_JAPANESE;
            break;
        } else if (SDL_strcmp(locales[i]->language, "en") == 0) {
            language = LANG_ENGLISH;
            break;
        }
    }

    SDL_free(locales);
    return language;
}

static void njUserInit() {
    u32 size;

    sysFF = 1;
    mpp_w.sysStop = false;
    mpp_w.inGame = false;
    mpp_w.language = Get_Default_Language();
    mmSystemInitialize();
    flGetFrame(&mpp_w.fmsFrame);
    seqsInitialize(mppMalloc(seqsGetUseMemorySize()));
    ppg_Initialize(mppMalloc(0x60000), 0x60000);
    zlib_Initialize(mppMalloc(0x10000), 0x10000);
    size = flGetSpace();
    mpp_w.ramcntBuff = mppMalloc(size);
    Init_ram_control_work(mpp_w.ramcntBuff, size);

    Interrupt_Timer = 0;
    Disp_Size_H = 100;
    Disp_Size_V = 100;
    Country = 4;

    if (Country == 0) {
        while (1) {}
    }

    Init_sound_system();
    Init_bgm_work();
    sndInitialLoad();
    cpInitTask();
    cpReadyTask(TASK_INIT, Init_Task);
}

static void distributeScratchPadAddress() {
    dctex_linear = (s16*)dctex_linear_mem;
    texcash_melt_buffer = (u8*)texcash_melt_buffer_mem;
    tpu_free = (TexturePoolUsed*)tpu_free_mem;
}

static void sf3_init() {
#if DEBUG
    DebugConfig_Init();
#endif

    flInitialize();
    flSetRenderState(FLRENDER_BACKCOLOR, 0);
    system_init_level = 0;
    ppgWorkInitializeApprication();
    distributeScratchPadAddress();
    njdp2d_init();
    njUserInit();
    palCreateGhost();
    ppgMakeConvTableTexDC();
    appSetupBasePriority();
}

#if _WIN32 && DEBUG
static void init_windows_console() {
    // attaches to an existing console for printouts. Works with windows CMD but not MSYS2
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        // if fails, then allocate a new console
        AllocConsole();
    }
    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
}
#endif

static void initialize_game() {
    SDLApp_FullInit();

#if _WIN32 && DEBUG
    init_windows_console();
#endif

    set_netplay_params();
    ArcadeBalance_Init();
    AFS_Init(Resources_GetAFSPath());
    sf3_init();
}

static void cleanup() {
    AFS_Finish();
    SDLApp_Quit();
}

// Iteration

static void cpLoopTask() {
#if DEBUG
    disp_ramcnt_free_area();

    if (sysSLOW) {
        if (--Slow_Timer == 0) {
            sysSLOW = 0;
            Game_pause &= 0x7F;
        } else {
            Game_pause |= 0x80;
        }
    }
#endif

    for (int i = 0; i < 11; i++) {
        struct _TASK* task_ptr = &task[i];

        switch (task_ptr->condition) {
        case 1:
            task_ptr->func_adrs(task_ptr);
            break;

        case 2:
            task_ptr->condition = 1;
            break;

        case 3:
            break;
        }
    }
}

static void appCopyKeyData() {
    // FIXME: Should PLsw be saved/restored too?
    PLsw[0][1] = PLsw[0][0];
    PLsw[1][1] = PLsw[1][0];
    PLsw[0][0] = p1sw_buff;
    PLsw[1][0] = p2sw_buff;
}

void njUserMain() {
    CPU_Time_Lag[0] = 0;
    CPU_Time_Lag[1] = 0;
    CPU_Rec[0] = 0;
    CPU_Rec[1] = 0;

    Check_Replay_Status(0, Replay_Status[0]);
    Check_Replay_Status(1, Replay_Status[1]);

    cpLoopTask();

    if ((Game_pause != 0x81) && (Mode_Type == MODE_VERSUS) && (Play_Mode == 1)) {
        if ((plw[0].wu.wu_operator == 0) && (CPU_Rec[0] == 0) && (Replay_Status[0] == 1)) {
            p1sw_0 = 0;

            Check_Replay_Status(0, 1);

            if (Debug_w[0x21]) {
                flPrintColor(0xFFFFFFFF);
                flPrintL(0x10, 0xA, "FAKE REC! PL1");
            }
        }

        if ((plw[1].wu.wu_operator == 0) && (CPU_Rec[1] == 0) && (Replay_Status[1] == 1)) {
            p2sw_0 = 0;

            Check_Replay_Status(1, 1);

            if (Debug_w[0x21]) {
                flPrintColor(0xFFFFFFFF);
                flPrintL(0x10, 0xA, "FAKE REC!     PL2");
            }
        }
    }
}

#if DEBUG
static void configure_slow_timer() {
    if (test_flag) {
        return;
    }

    if (mpp_w.sysStop) {
        sysSLOW = 1;

        switch (io_w.data[1].sw_new) {
        case SWK_LEFT_STICK:
            mpp_w.sysStop = false;
            // fallthrough

        case SWK_LEFT_SHOULDER:
            Slow_Timer = 1;
            break;

        default:
            switch (io_w.data[1].sw & (SWK_LEFT_SHOULDER | SWK_LEFT_TRIGGER)) {
            case SWK_LEFT_SHOULDER | SWK_LEFT_TRIGGER:
                if ((sysFF = Debug_w[1]) == 0) {
                    sysFF = 1;
                }

                sysSLOW = 1;
                Slow_Timer = 1;

                break;

            case SWK_LEFT_TRIGGER:
                if (Slow_Timer == 0) {
                    if ((Slow_Timer = Debug_w[0]) == 0) {
                        Slow_Timer = 1;
                    }

                    sysFF = 1;
                }

                break;

            default:
                Slow_Timer = 2;
                break;
            }

            break;
        }
    } else if (io_w.data[1].sw_new & SWK_LEFT_STICK) {
        mpp_w.sysStop = true;
    }
}
#endif

static void game_step_0() {
    AFS_RunServer();

    /* Reset the engine's per-frame "active hitbox" capture flag before
     * the engine tick runs. set_jugde_area() will set it during the
     * tick if cg_ja.atix != 0 for either player. */
    fd_engine_hitbox_active[0] = 0;
    fd_engine_hitbox_active[1] = 0;
    /* CONTACT-2 Step 1 diagnostics (design.md §1.3 G4): same reset contract
     * as fd_engine_hitbox_active above — zero before njUserMain() runs so
     * check_leap_attack() (pls03.c) can set it fresh this frame only. */
    fd_engine_move_is_uoh[0] = 0;
    fd_engine_move_is_uoh[1] = 0;

#if ENABLE_PERF_TELEMETRY
    /* Per-frame reset for the perf-overlay diagnostic counters (report §4)
       whose producers accumulate within this frame: njdp2d prim peak/drops
       (drained twice per frame) and the training-overlay submit timer (0 on
       frames where the training task doesn't run). */
    Njdp2d_ResetPerf();
    Training_SetPerfDispNs(0);
#endif

    flSetRenderState(FLRENDER_BACKCOLOR, 0xFF000000);

#if DEBUG
    if (Debug_w[0x43]) {
        flSetRenderState(FLRENDER_BACKCOLOR, 0xFF0000FF);
    }
#endif

    appSetupTempPriority();
    flPADGetALL();
    keyConvert();

#if DEBUG
    if (configuration.test.enabled) {
        TestRunner_Prologue();
    }

    configure_slow_timer();
#endif

    /* Drive cold-launch menu navigation for netplay BEFORE p1sw_buff is
     * latched. The nav state machine may inject SWK_START on this tick;
     * if it does the rising-edge comparison ~p*sw_1 & p*sw_0 & SWK_START
     * in Ck_Coin() / Entry_01() / Mode_Select() needs our injected bit
     * to be present in p*sw_0 (the "current" snapshot). */
    NetplayNav_Tick();

    if ((Play_Mode != 3 && Play_Mode != 1) || (Game_pause != 0x81)) {
        p1sw_1 = p1sw_0;
        p2sw_1 = p2sw_0;
        p3sw_1 = p3sw_0;
        p4sw_1 = p4sw_0;
        p1sw_0 = p1sw_buff;
        p2sw_0 = p2sw_buff;
        p3sw_0 = p3sw_buff;
        p4sw_0 = p4sw_buff;

        if ((task[TASK_MENU].condition == 1) && Is_Training_Mode(Mode_Type) && (Play_Mode == 1)) {
            const u16 sw_buff = p2sw_0;
            p2sw_0 = p1sw_0;
            p1sw_0 = sw_buff;
        }
    }

    appCopyKeyData();

    mpp_w.inGame = false;

    if (Netplay_GetSessionState() != NETPLAY_SESSION_IDLE) {
        Netplay_Run();
        // Flush the 2D polygon buffer each frame when the game's normal render
        // loop isn't running, preventing the NJDP2D_PRIM_MAX limit from overflowing.
        njdp2d_draw();
        /* S1 host liveness (docs/plan-netplay-connection.md): keep the
         * orchestrator ticking during the active session so the UPnP
         * lease renewal (half of the 1-hour lease) fires mid-session —
         * the mapping is what carries the peer's traffic. In HANDOFF
         * state this is one atomic state read + one bool check per
         * frame; renewal itself runs on a side thread. */
        DirectP2P_Tick();
    } else {
        njUserMain();
        seqsBeforeProcess();
        njdp2d_draw();
        seqsAfterProcess();
        Netplay_TickMatchmaking();
        Netplay_TickDirectP2P();
#if defined(ENABLE_NETPLAY)
        defer_direct_p2p_handoff_tick();
#endif
        DirectP2P_Tick();
    }

    /* Freeze-boundary probe (fit.md §5), env-gated on FD_SPAWN_PROBE.
     * MUST run here — after the engine tick (njUserMain, which runs
     * effect_13_init and sets fd_engine_proj_spawned) and before
     * frame_data_overlay_tick(), whose consume sites clear the flag. See
     * frame_spawn_probe_tick()'s definition comment for why pre-consume
     * sampling is required to place the 0->1 transition unambiguously. */
    frame_spawn_probe_tick();

    frame_data_overlay_tick();

    /* FD_IDLE_PROBE (diagnostic, env-gated): per-tick idle ledger. MUST run
     * AFTER frame_data_overlay_tick() so each line reports post-engine,
     * post-overlay-latch state. Observation only; inert unless FD_IDLE_PROBE
     * is set (and, like the trace, only in training + overlay-enabled). */
    fd_idle_probe_tick();
#if ENABLE_PERF_TELEMETRY
    {
        const Uint64 _ft0 = SDL_GetTicksNS();
        frame_trace_tick();
        FrameTrace_SetPerfTickNs(SDL_GetTicksNS() - _ft0);
    }
#else
    frame_trace_tick();
#endif

    disp_effect_work();
    flFlip(0);
}

static void game_step_1() {
    Interrupt_Timer += 1;
    Record_Timer += 1;

    Scrn_Renew();
    Irl_Family();
    Irl_Scrn();
    BGM_Server();

#if DEBUG
    if (configuration.test.enabled) {
        TestRunner_Epilogue();
    }
#endif
}

static bool sdl_poll_helper() {
    SDL_Event event;
    bool continue_running = true;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            continue_running = false;
        }
    }

    return continue_running;
}

static void handle_signal_requests() {
    if (fps_toggle_requested != 0) {
        fps_toggle_requested = 0;
        SDLApp_ToggleFPSOverlay();
    }
    if (arm_clock_cycle_requested != 0) {
        arm_clock_cycle_requested = 0;
        SDLApp_CycleArmClock();
    }
    if (game_mode_cycle_requested != 0) {
        game_mode_cycle_requested = 0;
        SDLApp_CycleGameMode();
    }
    if (hold_to_pause_cycle_requested != 0) {
        hold_to_pause_cycle_requested = 0;
        SDLApp_CycleHoldToPause();
    }
}

static int loop() {
    bool is_running = true;
    bool console_mode_entered = false;
    bool shutdown_handlers_installed = false;
#if ENABLE_PERF_TELEMETRY
    bool perf_capture_started = false;
    int perf_wait_warmup_remaining = configuration.perf.gameplay_warmup_frames;
#endif
    int exit_code = 0;

    shutdown_signal = 0;
    fps_toggle_requested = 0;
    arm_clock_cycle_requested = 0;
    game_mode_cycle_requested = 0;

    while (is_running && shutdown_signal == 0) {
        switch (phase) {
        case MAIN_PHASE_INIT:
            if (Resources_Check()) {
                /* Resources verified: enter console mode and initialize.
                   ConsoleMode switches the VT to KD_GRAPHICS (black screen).
                   Doing this AFTER Resources_Check means the SHA256 hash runs
                   while the screen is still visible, avoiding a long unexplained
                   black screen before the game starts. */
                console_mode_entered = ConsoleMode_Enter();
#if defined(PORT_MISTER) && defined(__linux__)
                if (!console_mode_entered) {
                    fprintf(stderr,
                            "Failed to acquire Linux console (KD_GRAPHICS). "
                            "Run from MiSTer OSD/local console, not over SSH.\n");
                    exit(1);
                }
#endif
                install_shutdown_signal_handlers();
                shutdown_handlers_installed = true;
                SDLApp_PreInit();
                initialize_game();
                phase = MAIN_PHASE_INITIALIZED;

#if ENABLE_PERF_TELEMETRY
                if (configuration.perf.frame_count > 0 && !configuration.perf.wait_for_gameplay &&
                    configuration.perf.wait_for_test_phase == NULL &&
                    configuration.perf.wait_for_runtime_state == NULL) {
                    SDLApp_ConfigurePerfCapture(configuration.perf.frame_count,
                                                configuration.perf.output_path,
                                                configuration.perf.scene,
                                                configuration.perf.basic_mode,
                                                configuration.perf.basic_first_window_family_snapshots,
                                                configuration.perf.basic_first_window_render_subphases,
                                                configuration.perf.basic_first_window_exact_hot_family_alpha_offpath,
                                                configuration.perf.basic_first_window_onset_exact_hot_family_alpha_offpath,
                                                configuration.perf.basic_first_window_onset_cluster_alpha_offpath,
                                                configuration.perf.fast_non_integer_disable_reuse_telemetry,
                                                configuration.perf.fast_non_integer_enable_subrect_alpha_telemetry);
                    perf_capture_started = true;
                }
#endif
            } else {
                phase = MAIN_PHASE_COPYING_RESOURCES;
            }

            break;

        case MAIN_PHASE_COPYING_RESOURCES:
            is_running = sdl_poll_helper();

            if (!is_running) {
                break;
            }

            SDL_Delay(16);

            const bool resource_flow_ended = Resources_RunResourceCopyingFlow();

            if (resource_flow_ended) {
                initialize_game();
                phase = MAIN_PHASE_INITIALIZED;
            }

            break;

        case MAIN_PHASE_INITIALIZED:
            handle_signal_requests();

            is_running = SDLApp_PollEvents();

            if (!is_running) {
                break;
            }

            SDLApp_BeginFrame();
            game_step_0();
            SDLApp_EndFrame();
            game_step_1();

#if ENABLE_PERF_TELEMETRY
            if (!perf_capture_started && configuration.perf.frame_count > 0 &&
                (configuration.perf.wait_for_gameplay || configuration.perf.wait_for_test_phase != NULL ||
                 configuration.perf.wait_for_runtime_state != NULL)) {
                const bool wait_condition_met =
                    configuration.perf.wait_for_gameplay
                        ? mpp_w.inGame
                        : ((configuration.perf.wait_for_test_phase != NULL)
                               ? TestRunner_IsPhaseActive(configuration.perf.wait_for_test_phase)
                               : SDLApp_IsPerfRuntimeStateActive(configuration.perf.wait_for_runtime_state));

                if (wait_condition_met) {
                    if (perf_wait_warmup_remaining > 0) {
                        perf_wait_warmup_remaining -= 1;
                    } else {
                        const int perf_stage_id = mpp_w.inGame ? bg_w.stage : -1;
                        const int perf_p1_character = mpp_w.inGame ? My_char[0] : -1;
                        const int perf_p2_character = mpp_w.inGame ? My_char[1] : -1;
                        const int perf_p1_super_art = mpp_w.inGame ? Super_Arts[0] : -1;
                        const int perf_p2_super_art = mpp_w.inGame ? Super_Arts[1] : -1;
                        SDL_Log("PERF capture start: in_game=%d warmup_frames=%d scene=%s detail_mode=%s stage_id=%d "
                                "test_stage_override=%d test_scene_preset=%s p1_character=%d p2_character=%d "
                                "p1_super_art=%d p2_super_art=%d test_phase=%s wait_test_phase=%s wait_runtime_state=%s "
                                "fast_non_integer_reuse_telemetry=%s basic_first_window_family_snapshots=%s "
                                "basic_first_window_render_subphases=%s "
                                "basic_first_window_exact_hot_family_alpha_offpath=%s "
                                "basic_first_window_onset_exact_hot_family_alpha_offpath=%s "
                                "basic_first_window_onset_cluster_alpha_offpath=%s "
                                "fast_non_integer_subrect_alpha_telemetry=%s "
                                "g_no=%d/%d/%d/%d e_no=%d/%d/%d/%d menu_task_condition=%d menu_r_no=%d/%d/%d/%d "
                                "break_into=%d hnc_num=%d exec_wipe=%d active_wipe_type=%d wipe_limit=%d",
                                mpp_w.inGame ? 1 : 0,
                                configuration.perf.gameplay_warmup_frames,
                                configuration.perf.scene != NULL ? configuration.perf.scene : "(none)",
                                configuration.perf.basic_mode
                                    ? (configuration.perf.basic_first_window_family_snapshots
                                           ? "basic-first-window-families"
                                           : "basic")
                                    : "full",
                                perf_stage_id,
                                configuration.test.stage,
                                configuration.test.scene_preset != NULL ? configuration.test.scene_preset : "(none)",
                                perf_p1_character,
                                perf_p2_character,
                                perf_p1_super_art,
                                perf_p2_super_art,
                                TestRunner_GetPhaseName(),
                                configuration.perf.wait_for_test_phase != NULL ? configuration.perf.wait_for_test_phase
                                                                               : "(none)",
                                configuration.perf.wait_for_runtime_state != NULL ? configuration.perf.wait_for_runtime_state
                                                                                 : "(none)",
                                !configuration.perf.fast_non_integer_disable_reuse_telemetry
                                    ? "on"
                                    : "off",
                                (configuration.perf.basic_mode &&
                                 configuration.perf.basic_first_window_family_snapshots)
                                    ? "on"
                                    : "off",
                                (configuration.perf.basic_mode &&
                                 configuration.perf.basic_first_window_render_subphases)
                                    ? "on"
                                    : "off",
                                (configuration.perf.basic_mode &&
                                 configuration.perf.basic_first_window_exact_hot_family_alpha_offpath)
                                    ? "on"
                                    : "off",
                                (configuration.perf.basic_mode &&
                                 configuration.perf.basic_first_window_onset_exact_hot_family_alpha_offpath)
                                    ? "on"
                                    : "off",
                                (configuration.perf.basic_mode &&
                                 configuration.perf.basic_first_window_onset_cluster_alpha_offpath)
                                    ? "on"
                                    : "off",
                                (!configuration.perf.basic_mode &&
                                 configuration.perf.fast_non_integer_enable_subrect_alpha_telemetry)
                                    ? "on"
                                    : "off",
                                G_No[0],
                                G_No[1],
                                G_No[2],
                                G_No[3],
                                E_No[0],
                                E_No[1],
                                E_No[2],
                                E_No[3],
                                task[TASK_MENU].condition,
                                task[TASK_MENU].r_no[0],
                                task[TASK_MENU].r_no[1],
                                task[TASK_MENU].r_no[2],
                                task[TASK_MENU].r_no[3],
                                Break_Into,
                                Hnc_Num,
                                Exec_Wipe,
                                Active_Wipe_Type,
                                WipeLimit);
                        SDLApp_ConfigurePerfCapture(configuration.perf.frame_count,
                                                    configuration.perf.output_path,
                                                    configuration.perf.scene,
                                                    configuration.perf.basic_mode,
                                                    configuration.perf.basic_first_window_family_snapshots,
                                                    configuration.perf.basic_first_window_render_subphases,
                                                    configuration.perf.basic_first_window_exact_hot_family_alpha_offpath,
                                                    configuration.perf.basic_first_window_onset_exact_hot_family_alpha_offpath,
                                                    configuration.perf.basic_first_window_onset_cluster_alpha_offpath,
                                                    configuration.perf.fast_non_integer_disable_reuse_telemetry,
                                                    configuration.perf.fast_non_integer_enable_subrect_alpha_telemetry);
                        perf_capture_started = true;
                    }
                } else {
                    perf_wait_warmup_remaining = configuration.perf.gameplay_warmup_frames;
                }
            }
#endif
            break;
        }
    }

    // Tier-1 netplay diag — Item 10: SIGTERM flush hook. Runs after the
    // main loop exits but before cleanup() (which tears SDL down). If a
    // netplay session was active we dump the packet ring, capture a final
    // /proc/net/snmp UDP-row delta, and close the per-session log file.
    // No-op when no session was active.
#if defined(ENABLE_NETPLAY)
    Netplay_FlushDiagnostics();
#endif

    cleanup();

    if (shutdown_handlers_installed) {
        restore_shutdown_signal_handlers();
    }
    if (console_mode_entered) {
        ConsoleMode_Exit();
    }

    if (shutdown_signal != 0) {
        exit_code = 128 + shutdown_signal;
    }

    return exit_code;
}

// Phase 6 Step 2: forward-decl of the netplay event-queue test harness
// (src/netplay/test_event_queue.c). Not in netplay.h — test-only symbol.
// Only defined when ENABLE_NETPLAY is on; otherwise the CLI flag prints a
// diagnostic and exits.
#ifdef ENABLE_NETPLAY
int Netplay_Test_EventQueue(void);
// Phase 6 Step 8: forward-decl of the MIST handshake test harness
// (src/netplay/test_mist_handshake.c). Same gating as above.
int Netplay_Test_MistHandshake(void);
// STUN direct P2P Step 2 (docs/plan-stun-direct-p2p.md): forward-decl
// of the room-code codec test harness (src/netplay/test_room_code.c).
// Same gating pattern as the other Phase 6 tests — ENABLE_NETPLAY gates
// TU inclusion, ENABLE_NETPLAY_TESTS inside the TU gates the real body.
int Netplay_Test_RoomCode(void);
// STUN direct P2P Step 12 (docs/plan-stun-direct-p2p.md): forward-decl
// of the STUN mock-server test harness (src/netplay/test_stun_mock.c).
// Spawns a localhost UDP listener that echoes a crafted Binding Response
// with XOR-MAPPED-ADDRESS, then verifies the client parses it correctly.
// Also round-trips Stun_EncodeEndpoint / Stun_DecodeEndpoint.
int Netplay_Test_StunMock(void);
// perf(netplay) Option A: forward-decl of the sparse effect-pool save
// round-trip parity test harness (src/netplay/test_sparse_effect_save.c).
// Same gating pattern as the other Phase 6 tests.
int Netplay_Test_SparseEffectSave(void);
// Step 6 of docs/plan-bilateral-hole-punch.md: forward-decl of the
// bilateral hole-punch protocol test harness
// (src/netplay/test_bilateral_punch.c). Same gating pattern as the
// other Phase 6 tests. Spawns a localhost UDP rendezvous mock and
// exercises the REGISTER/POLL/DELIVER round-trip plus the LAN-bypass
// table; no external network dep.
int Netplay_Test_BilateralPunch(void);
#endif

int main(int argc, const char* argv[]) {
    read_args(argc, argv, &configuration);

    if (configuration.test_netplay_event_queue) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_EventQueue();
#else
        fprintf(stderr,
                "--test-netplay-event-queue requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_mist_handshake) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_MistHandshake();
#else
        fprintf(stderr,
                "--test-mist-handshake requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_room_code) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_RoomCode();
#else
        fprintf(stderr,
                "--test-room-code requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_stun_mock) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_StunMock();
#else
        fprintf(stderr,
                "--test-stun-mock requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_sparse_effect_save) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_SparseEffectSave();
#else
        fprintf(stderr,
                "--test-sparse-effect-save requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    if (configuration.test_bilateral_punch) {
#ifdef ENABLE_NETPLAY
        return Netplay_Test_BilateralPunch();
#else
        fprintf(stderr,
                "--test-bilateral-punch requires a build with ENABLE_NETPLAY=ON.\n");
        return 2;
#endif
    }

    return loop();
}

// Tasks

void cpReadyTask(TaskID num, void (*func_adrs)(struct _TASK* task_ptr)) {
    struct _TASK* task_ptr = &task[num];

    memset(task_ptr, 0, sizeof(struct _TASK));

    task_ptr->func_adrs = func_adrs;
    task_ptr->condition = 2;
}

void cpExitTask(TaskID num) {
    SDL_zero(task[num]);
}
