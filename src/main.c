#include "main.h"
#include "arcade/arcade_balance.h"
#include "args.h"
#include "common.h"
#include "configuration.h"
#include "netplay/netplay.h"
#include "port/sdl/sdl_app.h"
#include "port/sdl/sdl_game_renderer.h"
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
#include "sf33rd/Source/Game/ui/sc_sub.h"
#include "sf33rd/Source/PS2/mc/knjsub.h"
#include "sf33rd/Source/PS2/mc/mcsub.h"
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
static volatile sig_atomic_t super_effect_quality_cycle_requested = 0;
static volatile sig_atomic_t ghost_resolution_cycle_requested = 0;
static volatile sig_atomic_t ghost_count_cycle_requested = 0;
static volatile sig_atomic_t arm_clock_cycle_requested = 0;

static u8* mppMalloc(u32 size) {
    return flAllocMemory(size);
}

static void on_shutdown_signal(int signo) {
    if (signo == SIGUSR1) {
        fps_toggle_requested = 1;
        return;
    }

    if (signo == SIGUSR2) {
        super_effect_quality_cycle_requested = 1;
        return;
    }

#ifdef SIGRTMIN
    if (signo == SIGRTMIN) {
        ghost_resolution_cycle_requested = 1;
        return;
    }

    if (signo == SIGRTMIN + 1) {
        ghost_count_cycle_requested = 1;
        return;
    }

    if (signo == SIGRTMIN + 2) {
        arm_clock_cycle_requested = 1;
        return;
    }
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
    sigaction(SIGUSR2, &action, NULL);
#ifdef SIGRTMIN
    sigaction(SIGRTMIN, &action, NULL);
    sigaction(SIGRTMIN + 1, &action, NULL);
    sigaction(SIGRTMIN + 2, &action, NULL);
#endif
}

static void restore_shutdown_signal_handlers() {
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
#ifdef SIGRTMIN
    signal(SIGRTMIN, SIG_DFL);
    signal(SIGRTMIN + 1, SIG_DFL);
    signal(SIGRTMIN + 2, SIG_DFL);
#endif
}

// Initialization

static void set_netplay_params() {
#if defined(ENABLE_NETPLAY)
    if (configuration.netplay.p2p_remote_ip != NULL) {
        Netplay_SetParams(configuration.netplay.p2p_local_player, configuration.netplay.p2p_remote_ip);
    } else if (configuration.netplay.matchmaking_ip != NULL) {
        Netplay_SetMatchmakingParams(configuration.netplay.matchmaking_ip, configuration.netplay.matchmaking_port);
    }
#endif
}

void cpInitTask() {
    memset(&task, 0, sizeof(task));
}

static void njUserInit() {
    s32 i;
    u32 size;

    sysFF = 1;
    mpp_w.sysStop = false;
    mpp_w.inGame = false;
    mpp_w.language = 0;
    mmSystemInitialize();
    flGetFrame(&mpp_w.fmsFrame);
    seqsInitialize(mppMalloc(seqsGetUseMemorySize()));
    ppg_Initialize(mppMalloc(0x60000), 0x60000);
    zlib_Initialize(mppMalloc(0x10000), 0x10000);
    size = flGetSpace();
    mpp_w.ramcntBuff = mppMalloc(size);
    Init_ram_control_work(mpp_w.ramcntBuff, size);

    for (i = 0; i < 0x14; i++) {
        mpp_w.useChar[i] = 0;
    }

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
    MemcardInit();
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
        if ((plw[0].wu.operator == 0) && (CPU_Rec[0] == 0) && (Replay_Status[0] == 1)) {
            p1sw_0 = 0;

            Check_Replay_Status(0, 1);

            if (Debug_w[0x21]) {
                flPrintColor(0xFFFFFFFF);
                flPrintL(0x10, 0xA, "FAKE REC! PL1");
            }
        }

        if ((plw[1].wu.operator == 0) && (CPU_Rec[1] == 0) && (Replay_Status[1] == 1)) {
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

    if ((Play_Mode != 3 && Play_Mode != 1) || (Game_pause != 0x81)) {
        p1sw_1 = p1sw_0;
        p2sw_1 = p2sw_0;
        p3sw_1 = p3sw_0;
        p4sw_1 = p4sw_0;
        p1sw_0 = p1sw_buff;
        p2sw_0 = p2sw_buff;
        p3sw_0 = p3sw_buff;
        p4sw_0 = p4sw_buff;

        if ((task[TASK_MENU].condition == 1) && (Mode_Type == MODE_PARRY_TRAINING) && (Play_Mode == 1)) {
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
        // loop isn't running, preventing the 100-item limit from overflowing.
        njdp2d_draw();
    } else {
        njUserMain();
        seqsBeforeProcess();
        njdp2d_draw();
        seqsAfterProcess();
        Netplay_TickMatchmaking();
        Netplay_TickDirectP2P();
    }

    KnjFlush();
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
    if (super_effect_quality_cycle_requested != 0) {
        super_effect_quality_cycle_requested = 0;
        SDLApp_CycleSuperEffectQualityMode();
    }
    if (ghost_resolution_cycle_requested != 0) {
        ghost_resolution_cycle_requested = 0;
        SDLApp_CycleGhostResolutionMode();
    }
    if (ghost_count_cycle_requested != 0) {
        ghost_count_cycle_requested = 0;
        SDLApp_CycleGhostCountMax();
    }
    if (arm_clock_cycle_requested != 0) {
        arm_clock_cycle_requested = 0;
        SDLApp_CycleArmClock();
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
    super_effect_quality_cycle_requested = 0;
    ghost_resolution_cycle_requested = 0;
    ghost_count_cycle_requested = 0;
    arm_clock_cycle_requested = 0;

#if ENABLE_PERF_TELEMETRY
    if (configuration.perf.frame_count > 0 && !configuration.perf.basic_mode &&
        (configuration.perf.wait_for_gameplay || configuration.perf.wait_for_test_phase != NULL ||
         configuration.perf.wait_for_runtime_state != NULL)) {
        SDLGameRenderer_SetPerfCaptureLogicalIdentityEnabled(true);
    }
#endif

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

int main(int argc, const char* argv[]) {
    read_args(argc, argv, &configuration);
    return loop();
}

s32 mppGetFavoritePlayerNumber() {
    s32 i;
    s32 max = 1;
    s32 num = 0;

#if DEBUG
    if (Debug_w[0x2D]) {
        return Debug_w[0x2D] - 1;
    }
#endif

    for (i = 0; i < 0x14; i++) {
        if (max <= mpp_w.useChar[i]) {
            max = mpp_w.useChar[i];
            num = i + 1;
        }
    }

    return num;
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
