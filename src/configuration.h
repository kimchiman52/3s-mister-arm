#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "port/build_config.h"

#include <stdbool.h>

typedef struct NetplayConfiguration {
    int p2p_local_player;
    const char* p2p_remote_ip;
    const char* matchmaking_ip;
    int matchmaking_port;
} NetplayConfiguration;

typedef struct TestRunnerConfiguration {
    bool enabled;
    const char* states_path;
    const char* scene_preset;
    int characters[2];
    int super_arts[2];
    bool initial_super_full;
    int preserve_game_transition;
    int delay_gameplay_inputs_until_active;
    int stage;
} TestRunnerConfiguration;

#if ENABLE_PERF_TELEMETRY
typedef struct PerfCaptureConfiguration {
    int frame_count;
    const char* output_path;
    const char* scene;
    bool basic_mode;
    bool basic_first_window_family_snapshots;
    bool basic_first_window_render_subphases;
    bool basic_first_window_exact_hot_family_alpha_offpath;
    bool basic_first_window_onset_exact_hot_family_alpha_offpath;
    bool basic_first_window_onset_cluster_alpha_offpath;
    bool fast_non_integer_disable_reuse_telemetry;
    bool fast_non_integer_enable_subrect_alpha_telemetry;
    bool wait_for_gameplay;
    const char* wait_for_test_phase;
    const char* wait_for_runtime_state;
    int gameplay_warmup_frames;
    bool software_frame_parity_check;
} PerfCaptureConfiguration;
#endif

typedef struct Configuration {
    NetplayConfiguration netplay;
    TestRunnerConfiguration test;
#if ENABLE_PERF_TELEMETRY
    PerfCaptureConfiguration perf;
#endif
    bool probe_renderer_only;
    bool headless;
} Configuration;

#endif
