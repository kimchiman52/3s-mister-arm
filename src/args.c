#include "args.h"
#include "main.h"
#include "test/test_runner.h"

#include "argparse/argparse.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define EXIT_CODE_RUNTIME_ERROR 1

static void error_out_with_code(const char* error, int code) {
    fprintf(stderr, "%s Exiting.\n", error);
    exit(code);
}

static void error_out(const char* error) {
    error_out_with_code(error, EXIT_CODE_RUNTIME_ERROR);
}

static bool is_supported_test_stage(int stage) {
    return stage >= 0 && stage <= 19 && stage != 17;
}

static bool is_supported_test_scene_preset(const char* preset) {
    return preset == NULL || SDL_strcmp(preset, "stage-heavy") == 0 || SDL_strcmp(preset, "effect-heavy") == 0 ||
           SDL_strcmp(preset, "super-heavy") == 0 || SDL_strcmp(preset, "yun-sa3-repeat") == 0 ||
           SDL_strcmp(preset, "yun-sa3-repeat-pressure") == 0 || SDL_strcmp(preset, "q-sa1-repeat") == 0 ||
           SDL_strcmp(preset, "q-sa1-repeat-pressure") == 0 || SDL_strcmp(preset, "ken-sa3-repeat") == 0 ||
           SDL_strcmp(preset, "ken-sa3-repeat-pressure") == 0 ||
           SDL_strcmp(preset, "chunli-sa2-repeat") == 0 ||
           SDL_strcmp(preset, "chunli-sa2-repeat-pressure") == 0 ||
           SDL_strcmp(preset, "basic-exchange") == 0 || SDL_strcmp(preset, "pressure-exchange") == 0 ||
           SDL_strcmp(preset, "left-corner-ryu-stage") == 0 || SDL_strcmp(preset, "training-yun-ryu-ryu-stage") == 0;
}

#if ENABLE_PERF_TELEMETRY
static bool is_supported_perf_wait_test_phase(const char* phase_name) {
    return phase_name == NULL ||
           (TestRunner_IsSupportedPhaseName(phase_name) && SDL_strcmp(phase_name, "init") != 0);
}

static bool is_supported_perf_wait_runtime_state(const char* state_name) {
    return state_name == NULL || SDL_strcmp(state_name, "attract-demo-logo") == 0 ||
           SDL_strcmp(state_name, "character-select-super-art") == 0;
}
#endif

static void verify_configuration(Configuration* configuration) {
    const TestRunnerConfiguration* test = &configuration->test;

#if ENABLE_PERF_TELEMETRY
    if (configuration->perf.frame_count < 0) {
        error_out_with_code("--perf-capture must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }

    if (configuration->perf.basic_first_window_family_snapshots && !configuration->perf.basic_mode) {
        error_out_with_code("--perf-basic-first-window-families requires --perf-basic.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (configuration->perf.basic_first_window_render_subphases &&
        (!configuration->perf.basic_mode || !configuration->perf.basic_first_window_family_snapshots)) {
        error_out_with_code("--perf-basic-first-window-render-subphases requires --perf-basic-first-window-families.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (configuration->perf.basic_first_window_exact_hot_family_alpha_offpath &&
        (!configuration->perf.basic_mode || !configuration->perf.basic_first_window_family_snapshots)) {
        error_out_with_code("--perf-basic-first-window-exact-hot-family-alpha-offpath requires --perf-basic-first-window-families.",
                            EXIT_CODE_RUNTIME_ERROR);
    }
    if (configuration->perf.basic_first_window_onset_exact_hot_family_alpha_offpath &&
        (!configuration->perf.basic_mode || !configuration->perf.basic_first_window_family_snapshots)) {
        error_out_with_code("--perf-basic-first-window-onset-exact-hot-family-alpha-offpath requires --perf-basic-first-window-families.",
                            EXIT_CODE_RUNTIME_ERROR);
    }
    if (configuration->perf.basic_first_window_onset_cluster_alpha_offpath &&
        (!configuration->perf.basic_mode || !configuration->perf.basic_first_window_family_snapshots)) {
        error_out_with_code("--perf-basic-first-window-onset-cluster-alpha-offpath requires --perf-basic-first-window-families.",
                            EXIT_CODE_RUNTIME_ERROR);
    }
    if ((configuration->perf.basic_first_window_exact_hot_family_alpha_offpath ? 1 : 0) +
            (configuration->perf.basic_first_window_onset_exact_hot_family_alpha_offpath ? 1 : 0) +
            (configuration->perf.basic_first_window_onset_cluster_alpha_offpath ? 1 : 0) >
        1) {
        error_out_with_code("--perf-basic-first-window-exact-hot-family-alpha-offpath, --perf-basic-first-window-onset-exact-hot-family-alpha-offpath, and --perf-basic-first-window-onset-cluster-alpha-offpath cannot be used together.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (!is_supported_perf_wait_test_phase(configuration->perf.wait_for_test_phase)) {
        error_out_with_code("--perf-wait-test-phase must be one of title, menu, "
                            "character-select-transition, character-select, game-transition, game, "
                            "game-input-active, p1-super-art-active, p1-super-art-active-2, or "
                            "wipe-transition-type1.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (!is_supported_perf_wait_runtime_state(configuration->perf.wait_for_runtime_state)) {
        error_out_with_code("--perf-wait-runtime-state must be attract-demo-logo or character-select-super-art.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

    if (configuration->perf.gameplay_warmup_frames < 0) {
        error_out_with_code("--perf-warmup must be >= 0.", EXIT_CODE_RUNTIME_ERROR);
    }
#endif

    if (test->characters[0] != -1 && (test->characters[0] < 0 || test->characters[0] > 19)) {
        error_out_with_code("--test-p1-character must be between 0 and 19.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->characters[1] != -1 && (test->characters[1] < 0 || test->characters[1] > 19)) {
        error_out_with_code("--test-p2-character must be between 0 and 19.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->super_arts[0] != -1 && (test->super_arts[0] < 0 || test->super_arts[0] > 2)) {
        error_out_with_code("--test-p1-super-art must be between 0 and 2.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->super_arts[1] != -1 && (test->super_arts[1] < 0 || test->super_arts[1] > 2)) {
        error_out_with_code("--test-p2-super-art must be between 0 and 2.", EXIT_CODE_RUNTIME_ERROR);
    }
    if (test->stage != -1 && !is_supported_test_stage(test->stage)) {
        error_out_with_code("--test-stage must be between 0 and 19 (excluding 17).", EXIT_CODE_RUNTIME_ERROR);
    }
    if (!is_supported_test_scene_preset(test->scene_preset)) {
        error_out_with_code("--test-scene-preset must be one of stage-heavy, effect-heavy, super-heavy, "
                            "yun-sa3-repeat, yun-sa3-repeat-pressure, q-sa1-repeat, q-sa1-repeat-pressure, "
                            "ken-sa3-repeat, ken-sa3-repeat-pressure, chunli-sa2-repeat, chunli-sa2-repeat-pressure, "
                            "basic-exchange, pressure-exchange, left-corner-ryu-stage, or training-yun-ryu-ryu-stage.",
                            EXIT_CODE_RUNTIME_ERROR);
    }

#if defined(ENABLE_NETPLAY)
    {
        const NetplayConfiguration* netplay = &configuration->netplay;
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
    }
#endif
}

void read_args(int argc, const char* argv[], Configuration* configuration) {
    struct argparse_option options[] = {
        OPT_HELP(),

#if defined(ENABLE_NETPLAY)
        OPT_GROUP("Netplay"),
        OPT_INTEGER(0,
                    "p2p-local-player",
                    &configuration->netplay.p2p_local_player,
                    "Number of the local player (1 or 2).",
                    NULL,
                    0,
                    0),
        OPT_STRING(0, "p2p-remote-ip", &configuration->netplay.p2p_remote_ip, "Remote player IP.", NULL, 0, 0),
        OPT_STRING(0, "matchmaking-ip", &configuration->netplay.matchmaking_ip, "Matchmaking server IP.", NULL, 0, 0),
        OPT_INTEGER(
            0, "matchmaking-port", &configuration->netplay.matchmaking_port, "Matchmaking server port.", NULL, 0, 0),
#endif

        OPT_GROUP("Diagnostics"),
        OPT_BOOLEAN(0,
                    "probe-renderer-only",
                    &configuration->probe_renderer_only,
                    "Probe SDL video/render backends and exit.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0, "headless", &configuration->headless, "Run with non-interactive event handling.", NULL, 0, 0),
#if ENABLE_PERF_TELEMETRY
        OPT_GROUP("Performance"),
        OPT_INTEGER(0,
                    "perf-capture",
                    &configuration->perf.frame_count,
                    "Capture perf metrics for N frames, then exit.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic",
                    &configuration->perf.basic_mode,
                    "Capture low-overhead frame/update/render/present timings; lightweight test-state metadata may still be exported.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic-first-window-families",
                    &configuration->perf.basic_first_window_family_snapshots,
                    "When used with --perf-basic, also export first-window fast/generic family summaries without full shape/lookup telemetry.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic-first-window-render-subphases",
                    &configuration->perf.basic_first_window_render_subphases,
                    "When used with --perf-basic-first-window-families, also export first-window raster-bucket timing plus fast non-integer phase totals.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic-first-window-exact-hot-family-alpha-offpath",
                    &configuration->perf.basic_first_window_exact_hot_family_alpha_offpath,
                    "When used with --perf-basic-first-window-families, analyze the proven 57/58 + 391-394 hot-family alpha structure off the hot raster path after the first window snapshot.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic-first-window-onset-exact-hot-family-alpha-offpath",
                    &configuration->perf.basic_first_window_onset_exact_hot_family_alpha_offpath,
                    "When used with --perf-basic-first-window-families, analyze the exact proven first-visible Yun onset hot-family alpha structure off the hot raster path after the first window snapshot.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-basic-first-window-onset-cluster-alpha-offpath",
                    &configuration->perf.basic_first_window_onset_cluster_alpha_offpath,
                    "When used with --perf-basic-first-window-families, analyze the proven Loop 172 first-visible Yun onset cluster alpha structure off the hot raster path after the first window snapshot.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-fast-non-integer-no-reuse-telemetry",
                    &configuration->perf.fast_non_integer_disable_reuse_telemetry,
                    "Keep full perf capture enabled but skip fast non-integer row-reuse bookkeeping to reduce capture distortion.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "perf-fast-non-integer-subrect-alpha-telemetry",
                    &configuration->perf.fast_non_integer_enable_subrect_alpha_telemetry,
                    "Keep full perf capture enabled and classify fast non-integer sampled subrect alpha rows/spans in-band during raster.",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "perf-output",
                   &configuration->perf.output_path,
                   "Path to perf capture JSON output.",
                   NULL,
                   0,
                   0),
        OPT_STRING(
            0, "scene", &configuration->perf.scene, "Optional perf scene label for capture metadata.", NULL, 0, 0),
        OPT_BOOLEAN(0,
                    "perf-wait-in-game",
                    &configuration->perf.wait_for_gameplay,
                    "Delay perf capture until gameplay is active.",
                    NULL,
                    0,
                    0),
        OPT_STRING(0,
                   "perf-wait-test-phase",
                   &configuration->perf.wait_for_test_phase,
                   "Delay perf capture until the test runner reaches the named phase.",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "perf-wait-runtime-state",
                   &configuration->perf.wait_for_runtime_state,
                   "Delay perf capture until the runtime reaches the named state.",
                   NULL,
                   0,
                   0),
        OPT_INTEGER(0,
                    "perf-warmup",
                    &configuration->perf.gameplay_warmup_frames,
                    "Warmup frames to skip after the selected perf wait condition becomes active before starting perf capture.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "software-frame-parity-check",
                    &configuration->perf.software_frame_parity_check,
                    "Run the offline software-frame parity self-check and exit.",
                    NULL,
                    0,
                    0),
#endif
        OPT_GROUP("Test runner"),
        OPT_BOOLEAN(0, "test-enable", &configuration->test.enabled, "Enable test runner.", NULL, 0, 0),
        OPT_STRING(0,
                   "test-states",
                   &configuration->test.states_path,
                   "Optional path to captured test states for scripted in-game inputs.",
                   NULL,
                   0,
                   0),
        OPT_STRING(0,
                   "test-scene-preset",
                   &configuration->test.scene_preset,
                   "Optional named scripted gameplay preset (stage-heavy, effect-heavy, super-heavy, yun-sa3-repeat, yun-sa3-repeat-pressure, q-sa1-repeat, q-sa1-repeat-pressure, ken-sa3-repeat, ken-sa3-repeat-pressure, chunli-sa2-repeat, chunli-sa2-repeat-pressure, basic-exchange, pressure-exchange, left-corner-ryu-stage, training-yun-ryu-ryu-stage).",
                   NULL,
                   0,
                   0),
        OPT_INTEGER(0,
                    "test-p1-character",
                    &configuration->test.characters[0],
                    "Override player 1 character for the default test runner path (0-19).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-p2-character",
                    &configuration->test.characters[1],
                    "Override player 2 character for the default test runner path (0-19).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-p1-super-art",
                    &configuration->test.super_arts[0],
                    "Override player 1 super art for the default test runner path (0-2).",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-p2-super-art",
                    &configuration->test.super_arts[1],
                    "Override player 2 super art for the default test runner path (0-2).",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-p1-super-full",
                    &configuration->test.initial_super_full,
                    "Start player 1 with a full super meter on the first gameplay frame of the test runner path.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-preserve-game-transition",
                    &configuration->test.preserve_game_transition,
                    "Preserve the full pre-game transition in the test runner instead of mashing attacks to skip it.",
                    NULL,
                    0,
                    0),
        OPT_BOOLEAN(0,
                    "test-delay-gameplay-inputs-until-active",
                    &configuration->test.delay_gameplay_inputs_until_active,
                    "Delay scripted gameplay inputs and first-frame super bootstrap until both players reach gameplay/input-active state.",
                    NULL,
                    0,
                    0),
        OPT_INTEGER(0,
                    "test-stage",
                    &configuration->test.stage,
                    "Override stage for the default test runner path (0-19, excluding 17).",
                    NULL,
                    0,
                    0),

        OPT_END(),
    };

    struct argparse argparse;
    argparse_init(&argparse, options, NULL, 0);
    argparse_parse(&argparse, argc, argv);

    verify_configuration(configuration);
}
