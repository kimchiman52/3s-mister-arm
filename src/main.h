#ifndef MAIN_H
#define MAIN_H

#include "port/build_config.h"
#include "structs.h"
#include "types.h"

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
    bool basic_first_window_exact_hot_family_alpha_offpath;
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

typedef enum TaskID {
    TASK_INIT = 0,
    TASK_ENTRY = 1,
    TASK_RESET = 2,
    TASK_MENU = 3,
    TASK_PAUSE = 4,
    TASK_GAME = 5,
    TASK_SAVER = 6,
    TASK_DEBUG = 9,
} TaskID;

extern MPP mpp_w;
extern s32 system_init_level;
extern Configuration configuration;

void cpInitTask();
void cpReadyTask(TaskID num, void (*func_adrs)(struct _TASK* task_ptr));
void cpExitTask(TaskID num);
s32 mppGetFavoritePlayerNumber();
void njUserMain();

#endif
