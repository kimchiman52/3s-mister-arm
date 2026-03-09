#include "main.h"
#include "common.h"
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

#if defined(DEBUG)
#include "sf33rd/Source/Game/debug/debug_config.h"
#endif

#include "port/io/afs.h"
#include "port/linux/console_mode.h"
#include "port/resources.h"

#include "argparse/argparse.h"
#include <SDL3/SDL.h>

#if defined(_WIN32)
#include <windef.h> // including windows.h causes conflicts with the Polygon struct, so I just included the header where AllocConsole is and the Windows-specific typedefs that it requires.

#include <ConsoleApi.h>
#endif

#include <memory.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

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

static bool is_game_initialized = false;
static bool are_resources_checked = false;
static bool is_running_resource_flow = false;
static volatile sig_atomic_t shutdown_signal = 0;

// forward decls
static void game_init();
static void game_step_0();
static void game_step_1();
static void init_windows_console();
static void install_shutdown_signal_handlers();
static void restore_shutdown_signal_handlers();

void distributeScratchPadAddress();
void appCopyKeyData();
u8* mppMalloc(u32 size);
void njUserInit();
void njUserMain();
void cpLoopTask();
void cpInitTask();

#define EXIT_CODE_RUNTIME_ERROR 1
#define EXIT_CODE_MISSING_RESOURCES 20

static void on_shutdown_signal(int signo) {
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
}

static void restore_shutdown_signal_handlers() {
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
}

static void read_args(int argc, const char* argv[]) {
    struct argparse_option options[] = {
        OPT_HELP(),

#if defined(ENABLE_NETPLAY)
        OPT_GROUP("Netplay"),
        OPT_INTEGER(0,
                    "p2p-local-player",
                    &configuration.netplay.p2p_local_player,
                    "Number of the local player (1 or 2).",
                    NULL,
                    0,
                    0),
        OPT_STRING(0, "p2p-remote-ip", &configuration.netplay.p2p_remote_ip, "Remote player IP.", NULL, 0, 0),
        OPT_STRING(0, "matchmaking-ip", &configuration.netplay.matchmaking_ip, "Matchmaking server IP.", NULL, 0, 0),
        OPT_INTEGER(
            0, "matchmaking-port", &configuration.netplay.matchmaking_port, "Matchmaking server port.", NULL, 0, 0),
#endif

        OPT_GROUP("Diagnostics"),
        OPT_BOOLEAN(0,
                    "probe-renderer-only",
                    &configuration.probe_renderer_only,
                    "Probe SDL video/render backends and exit.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0, "headless", &configuration.headless, "Run with non-interactive event handling.", NULL, 0, 0),
#if ENABLE_PERF_TELEMETRY
        OPT_GROUP("Performance"),
        OPT_INTEGER(0,
                    "perf-capture",
                    &configuration.perf.frame_count,
                    "Capture perf metrics for N frames, then exit.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic",
                    &configuration.perf.basic_mode,
                    "Capture low-overhead frame/update/render/present timings; lightweight test-state metadata may still be exported.",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "perf-output",
                   &configuration.perf.output_path,
                   "Path to perf capture JSON output.",
                   NULL,
                   0,
                   0),
        OPT_STRING(
            0, "scene", &configuration.perf.scene, "Optional perf scene label for capture metadata.", NULL, 0, 0),
        OPT_BOOLEAN(0,
                    "perf-wait-in-game",
                    &configuration.perf.wait_for_gameplay,
                    "Delay perf capture until gameplay is active.",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "perf-wait-test-phase",
                   &configuration.perf.wait_for_test_phase,
                   "Delay perf capture until the test runner reaches the named phase.",
                   NULL,
                   0,
                   0),
        OPT_INTEGER(0,
                    "perf-warmup",
                    &configuration.perf.gameplay_warmup_frames,
                    "Warmup frames to skip after the selected perf wait condition becomes active before starting perf capture.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "software-frame-parity-check",
                    &configuration.perf.software_frame_parity_check,
                    "Run the offline software-frame parity self-check and exit.",
                    NULL,
                    0,
                    0),
#endif
        OPT_GROUP("Test runner"),
        OPT_BOOLEAN(0, "test-enable", &configuration.test.enabled, "Enable test runner.", NULL, 0, 0),
        OPT_STRING(0,
                   "test-states",
                   &configuration.test.states_path,
                   "Optional path to captured test states for scripted in-game inputs.",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "test-scene-preset",
                   &configuration.test.scene_preset,
                   "Optional named scripted gameplay preset (stage-heavy, effect-heavy, super-heavy).",
                   NULL,
                   0,
                   0),
        OPT_INTEGER(0,
                    "test-p1-character",
                    &configuration.test.characters[0],
                    "Override player 1 character for the default test runner path (0-19).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-p2-character",
                    &configuration.test.characters[1],
                    "Override player 2 character for the default test runner path (0-19).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-p1-super-art",
                    &configuration.test.super_arts[0],
                    "Override player 1 super art for the default test runner path (0-2).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-p2-super-art",
                    &configuration.test.super_arts[1],
                    "Override player 2 super art for the default test runner path (0-2).",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-p1-super-full",
                    &configuration.test.initial_super_full,
                    "Start player 1 with a full super meter on the first gameplay frame of the test runner path.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-preserve-game-transition",
                    &configuration.test.preserve_game_transition,
                    "Preserve the full pre-game transition in the test runner instead of mashing attacks to skip it.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-delay-gameplay-inputs-until-active",
                    &configuration.test.delay_gameplay_inputs_until_active,
                    "Delay scripted gameplay inputs and first-frame super bootstrap until both players reach gameplay/input-active state.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-stage",
                    &configuration.test.stage,
                    "Override stage for the default test runner path (0-19, excluding 17).",
                    NULL,
                    0,
                    0),

        OPT_END(),
    };

    struct argparse argparse;
    argparse_init(&argparse, options, NULL, 0);
    argparse_parse(&argparse, argc, argv);
}

static void error_out_with_code(const char* error, int code) {
    fprintf(stderr, "%s Exiting.\n", error);
    exit(code);
}

static bool is_supported_test_stage(int stage) {
    return stage >= 0 && stage <= 19 && stage != 17;
}

static bool is_supported_test_scene_preset(const char* preset) {
    return preset == NULL || SDL_strcmp(preset, "stage-heavy") == 0 || SDL_strcmp(preset, "effect-heavy") == 0 ||
           SDL_strcmp(preset, "super-heavy") == 0;
}

#if ENABLE_PERF_TELEMETRY
static bool is_supported_perf_wait_test_phase(const char* phase_name) {
    return phase_name == NULL ||
           (TestRunner_IsSupportedPhaseName(phase_name) && SDL_strcmp(phase_name, "init") != 0);
}
#endif

#if defined(ENABLE_NETPLAY)
static void error_out(const char* error) {
    error_out_with_code(error, EXIT_CODE_RUNTIME_ERROR);
}
#endif

static void verify_args() {
    const TestRunnerConfiguration* test = &configuration.test;

#if ENABLE_PERF_TELEMETRY
    if (configuration.perf.frame_count < 0) {
        error_out_with_code("--perf-capture must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }

    if (!is_supported_perf_wait_test_phase(configuration.perf.wait_for_test_phase)) {
        error_out_with_code("--perf-wait-test-phase must be one of title, menu, character-select-transition, "
                            "character-select, game-transition, game, game-input-active, or wipe-transition-type1.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (configuration.perf.wait_for_gameplay && configuration.perf.wait_for_test_phase != NULL) {
        error_out_with_code("--perf-wait-in-game cannot be combined with --perf-wait-test-phase.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (configuration.perf.gameplay_warmup_frames < 0) {
        error_out_with_code("--perf-warmup must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }
#endif

    for (int player = 0; player < 2; player++) {
        if (test->characters[player] < -1 || test->characters[player] > 19) {
            error_out_with_code("--test-p1-character/--test-p2-character must be between 0 and 19.", EXIT_CODE_RUNTIME_ERROR);
        }

        if (test->super_arts[player] < -1 || test->super_arts[player] > 2) {
            error_out_with_code("--test-p1-super-art/--test-p2-super-art must be between 0 and 2.", EXIT_CODE_RUNTIME_ERROR);
        }
    }

    if (test->stage != -1 && !is_supported_test_stage(test->stage)) {
        error_out_with_code("--test-stage must be one of 0-19 excluding 17.", EXIT_CODE_RUNTIME_ERROR);
    }

    if (!is_supported_test_scene_preset(test->scene_preset)) {
        error_out_with_code(
            "--test-scene-preset must be one of stage-heavy, effect-heavy, or super-heavy.", EXIT_CODE_RUNTIME_ERROR);
    }

    if (test->scene_preset != NULL && !test->enabled) {
        error_out_with_code("--test-scene-preset requires --test-enable.", EXIT_CODE_RUNTIME_ERROR);
    }

    if (test->delay_gameplay_inputs_until_active && !test->enabled) {
        error_out_with_code("--test-delay-gameplay-inputs-until-active requires --test-enable.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

#if ENABLE_PERF_TELEMETRY
    if (configuration.perf.wait_for_test_phase != NULL && !test->enabled) {
        error_out_with_code("--perf-wait-test-phase requires --test-enable.", EXIT_CODE_RUNTIME_ERROR);
    }
#endif

    if (test->scene_preset != NULL && test->states_path != NULL && test->states_path[0] != '\0') {
        error_out_with_code("--test-scene-preset cannot be combined with --test-states.", EXIT_CODE_RUNTIME_ERROR);
    }

    if (test->delay_gameplay_inputs_until_active && test->states_path != NULL && test->states_path[0] != '\0') {
        error_out_with_code("--test-delay-gameplay-inputs-until-active cannot be combined with --test-states.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

#if defined(ENABLE_NETPLAY)
    const NetplayConfiguration* netplay = &configuration.netplay;
    const bool p2p_specified = netplay->p2p_local_player > 0 || netplay->p2p_remote_ip != NULL;
    const bool matchmaking_specified = netplay->matchmaking_ip != NULL || netplay->matchmaking_port != 0;

    if (p2p_specified && matchmaking_specified) {
        error_out("Can't specify P2P and matchmaking at the same time.");
    }

    if (p2p_specified) {
        if (netplay->p2p_local_player != 1 && netplay->p2p_local_player != 2) {
            error_out("Local player must be 1 or 2.");
        }

        if (netplay->p2p_remote_ip == NULL) {
            error_out("You must specify --p2p-remote-ip.");
        }
    }

    if (matchmaking_specified) {
        if (netplay->matchmaking_ip == NULL) {
            error_out("You must specify --matchmaking-ip.");
        }

        if (netplay->matchmaking_port == 0) {
            error_out("You must specify --matchmaking-port.");
        }
    }
#endif
}

static void set_netplay_params() {
#if defined(ENABLE_NETPLAY)
    if (configuration.netplay.p2p_remote_ip != NULL) {
        Netplay_SetParams(configuration.netplay.p2p_local_player, configuration.netplay.p2p_remote_ip);
    } else if (configuration.netplay.matchmaking_ip != NULL) {
        Netplay_SetMatchmakingParams(configuration.netplay.matchmaking_ip, configuration.netplay.matchmaking_port);
    }
#endif
}

static void verify_required_resources_or_exit() {
#if !defined(ENABLE_ISO_IMPORT)
    are_resources_checked = Resources_CheckIfPresent();

    if (!are_resources_checked) {
        char* expected_path = Resources_GetPath("SF33RD.AFS");
        char* message = NULL;
        SDL_asprintf(&message,
                     "Missing required resource file at '%s'. Copy SF33RD.AFS there and relaunch.",
                     expected_path);
        SDL_free(expected_path);
        error_out_with_code(message, EXIT_CODE_MISSING_RESOURCES);
    }
#endif
}

/// @brief Makes sure resources are present.
/// @return `true` if resources are present and execution can proceed, `false` otherwise.
static bool run_resource_flow() {
    if (are_resources_checked) {
        return true;
    }

    if (!is_running_resource_flow) {
        are_resources_checked = Resources_CheckIfPresent();

        if (are_resources_checked) {
            return true;
        }

        is_running_resource_flow = true;
    }

    are_resources_checked = Resources_RunResourceCopyingFlow();

    if (are_resources_checked) {
        // Cleanup
        is_running_resource_flow = false;
    }

    return are_resources_checked;
}

static void afs_init() {
    char* file_path = Resources_GetPath("SF33RD.AFS");
    AFS_Init(file_path);
    SDL_free(file_path);
}

static void step_0() {
    if (!run_resource_flow()) {
        return;
    }

    if (!is_game_initialized) {
        afs_init();
        game_init();
        is_game_initialized = true;
    }

    if (is_game_initialized) {
        AFS_RunServer();
        game_step_0();
    }
}

static void step_1() {
    if (!run_resource_flow() || !is_game_initialized) {
        return;
    }

    game_step_1();
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

#if ENABLE_PERF_TELEMETRY
    if (configuration.perf.software_frame_parity_check) {
        return SDLApp_RunSoftwareFrameParityCheck() ? 0 : EXIT_CODE_RUNTIME_ERROR;
    }
#endif

    init_windows_console();
    if (!configuration.probe_renderer_only) {
        verify_required_resources_or_exit();
        console_mode_entered = ConsoleMode_Enter();
#if defined(PORT_MISTER) && defined(__linux__)
        if (!console_mode_entered) {
            error_out_with_code(
                "Failed to acquire Linux console (KD_GRAPHICS). Run from MiSTer OSD/local console, not over SSH.",
                EXIT_CODE_RUNTIME_ERROR);
        }
#endif
        install_shutdown_signal_handlers();
        shutdown_handlers_installed = true;
    }
    const int sdl_init_result = SDLApp_Init();

    if (sdl_init_result != 0) {
        if (shutdown_handlers_installed) {
            restore_shutdown_signal_handlers();
        }
        if (console_mode_entered) {
            ConsoleMode_Exit();
        }
        return sdl_init_result;
    }

    if (configuration.probe_renderer_only) {
        SDLApp_Quit();
        return 0;
    }

#if ENABLE_PERF_TELEMETRY
    if ((configuration.perf.frame_count > 0) && !configuration.perf.basic_mode &&
        (configuration.perf.wait_for_gameplay || configuration.perf.wait_for_test_phase != NULL)) {
        // Deferred full captures need pre-trigger registrations to preserve stable texture identities.
        SDLGameRenderer_SetPerfCaptureLogicalIdentityEnabled(true);
    }
    if (configuration.perf.frame_count > 0 && !configuration.perf.wait_for_gameplay &&
        configuration.perf.wait_for_test_phase == NULL) {
        SDLApp_ConfigurePerfCapture(configuration.perf.frame_count,
                                    configuration.perf.output_path,
                                    configuration.perf.scene,
                                    configuration.perf.basic_mode);
        perf_capture_started = true;
    }
#endif

    set_netplay_params();

    while (is_running && shutdown_signal == 0) {
        is_running = SDLApp_PollEvents();
        SDLApp_BeginFrame();
        step_0();
        SDLApp_EndFrame();
        step_1();

#if ENABLE_PERF_TELEMETRY
        if (!perf_capture_started && configuration.perf.frame_count > 0 &&
            (configuration.perf.wait_for_gameplay || configuration.perf.wait_for_test_phase != NULL)) {
            const bool wait_condition_met = configuration.perf.wait_for_gameplay
                                                ? mpp_w.inGame
                                                : TestRunner_IsPhaseActive(configuration.perf.wait_for_test_phase);

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
                            "p1_super_art=%d p2_super_art=%d test_phase=%s wait_test_phase=%s "
                            "g_no=%d/%d/%d/%d e_no=%d/%d/%d/%d menu_task_condition=%d menu_r_no=%d/%d/%d/%d "
                            "break_into=%d hnc_num=%d exec_wipe=%d active_wipe_type=%d wipe_limit=%d",
                            mpp_w.inGame ? 1 : 0,
                            configuration.perf.gameplay_warmup_frames,
                            configuration.perf.scene != NULL ? configuration.perf.scene : "(none)",
                            configuration.perf.basic_mode ? "basic" : "full",
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
                                                configuration.perf.basic_mode);
                    perf_capture_started = true;
                }
            } else {
                perf_wait_warmup_remaining = configuration.perf.gameplay_warmup_frames;
            }
        }
#endif
    }

    AFS_Finish();
    SDLApp_Quit();

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
    read_args(argc, argv);
    verify_args();
    return loop();
}

static void init_windows_console() {
#if defined(_WIN32)
    // attaches to an existing console for printouts. Works with windows CMD but not MSYS2
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        // if fails, then allocate a new console
        AllocConsole();
    }
    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
#endif
}

static void game_init() {
#if defined(DEBUG)
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

static void game_step_0() {
    flSetRenderState(FLRENDER_BACKCOLOR, 0xFF000000);

    if (Debug_w[0x43]) {
        flSetRenderState(FLRENDER_BACKCOLOR, 0xFF0000FF);
    }

    appSetupTempPriority();
    flPADGetALL();
    keyConvert();

    if (configuration.test.enabled) {
        TestRunner_Prologue();
    }

#if defined(DEBUG)
    if (!test_flag) {
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

    if (configuration.test.enabled) {
        TestRunner_Epilogue();
    }
}

u8 dctex_linear_mem[0x800];
u8 texcash_melt_buffer_mem[0x1000];
u8 tpu_free_mem[0x2000];

void distributeScratchPadAddress() {
    dctex_linear = (s16*)dctex_linear_mem;
    texcash_melt_buffer = (u8*)texcash_melt_buffer_mem;
    tpu_free = (TexturePoolUsed*)tpu_free_mem;
}

s32 mppGetFavoritePlayerNumber() {
    s32 i;
    s32 max = 1;
    s32 num = 0;

    if (Debug_w[0x2D]) {
        return Debug_w[0x2D] - 1;
    }

    for (i = 0; i < 0x14; i++) {
        if (max <= mpp_w.useChar[i]) {
            max = mpp_w.useChar[i];
            num = i + 1;
        }
    }

    return num;
}

void appCopyKeyData() {
    // FIXME: Should PLsw be saved/restored too?
    PLsw[0][1] = PLsw[0][0];
    PLsw[1][1] = PLsw[1][0];
    PLsw[0][0] = p1sw_buff;
    PLsw[1][0] = p2sw_buff;
}

u8* mppMalloc(u32 size) {
    return flAllocMemory(size);
}

void njUserInit() {
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

void cpLoopTask() {
    disp_ramcnt_free_area();

#if defined(DEBUG)
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

void cpInitTask() {
    memset(&task, 0, sizeof(task));
}

void cpReadyTask(TaskID num, void* func_adrs) {
    struct _TASK* task_ptr = &task[num];

    memset(task_ptr, 0, sizeof(struct _TASK));

    task_ptr->func_adrs = func_adrs;
    task_ptr->condition = 2;
}

void cpExitTask(TaskID num) {
    SDL_zero(task[num]);
}
