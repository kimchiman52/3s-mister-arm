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

typedef enum PerfUpdateScope {
    PERF_UPDATE_SCOPE_SELECT_TIMER = 0,
    PERF_UPDATE_SCOPE_SELECT_PLAYER,
    PERF_UPDATE_SCOPE_SEL_PL_CONTROL,
    PERF_UPDATE_SCOPE_PLAYER_SELECT_CONTROL,
    PERF_UPDATE_SCOPE_SEL_ARTS_SUB,
    PERF_UPDATE_SCOPE_TASK_ENTRY,
    PERF_UPDATE_SCOPE_TASK_MENU,
    PERF_UPDATE_SCOPE_TASK_GAME,
    PERF_UPDATE_SCOPE_TASK_DEBUG,
    PERF_UPDATE_SCOPE_GAME_TASK_MAIN_DISPATCH,
    PERF_UPDATE_SCOPE_GAME_TASK_INIT_TEXCASH_BEFORE_PROCESS,
    PERF_UPDATE_SCOPE_GAME_TASK_SEQS_BEFORE_PROCESS,
    PERF_UPDATE_SCOPE_GAME_TASK_SEQS_AFTER_PROCESS,
    PERF_UPDATE_SCOPE_GAME_TASK_SEQS_AFTER_RENEW,
    PERF_UPDATE_SCOPE_GAME_TASK_SEQS_AFTER_SUBMIT,
    PERF_UPDATE_SCOPE_GAME_TASK_SEQS_AFTER_SUBMIT_STATE_CHANGE,
    PERF_UPDATE_SCOPE_GAME_TASK_SEQS_AFTER_SUBMIT_ENQUEUE,
    PERF_UPDATE_SCOPE_GAME_TASK_TEXTURE_CASH_UPDATE,
    PERF_UPDATE_SCOPE_GAME_TASK_MOVE_PULPUL_WORK,
    PERF_UPDATE_SCOPE_GAME_TASK_CHECK_LDREQ_QUEUE,
    PERF_UPDATE_SCOPE_GAME_TASK_POST_FRAME_HOOKS,
    PERF_UPDATE_SCOPE_GAME01_TOTAL,
    PERF_UPDATE_SCOPE_GAME01_BG_DRAW_SYSTEM,
    PERF_UPDATE_SCOPE_GAME01_SETUP_PLAY_TYPE,
    PERF_UPDATE_SCOPE_BASIC_SUB_EFFECT_LIST_0,
    PERF_UPDATE_SCOPE_BASIC_SUB_EFFECT_LIST_1,
    PERF_UPDATE_SCOPE_BASIC_SUB_EFFECT_LIST_2,
    PERF_UPDATE_SCOPE_BASIC_SUB_EFFECT_LIST_3,
    PERF_UPDATE_SCOPE_BASIC_SUB_EFFECT_LIST_4,
    PERF_UPDATE_SCOPE_BASIC_SUB_EFFECT_LIST_5,
    PERF_UPDATE_SCOPE_EFFECT_D8,
    PERF_UPDATE_SCOPE_EFFECT_38,
    PERF_UPDATE_SCOPE_EFFECT_39,
    PERF_UPDATE_SCOPE_EFFECT_52,
    PERF_UPDATE_SCOPE_EFFECT_79,
    PERF_UPDATE_SCOPE_EFFECT_80,
    PERF_UPDATE_SCOPE_COUNT,
} PerfUpdateScope;

typedef struct PerfUpdateBreakdownCapture {
    uint64_t total_ns[PERF_UPDATE_SCOPE_COUNT];
    uint64_t total_calls[PERF_UPDATE_SCOPE_COUNT];
} PerfUpdateBreakdownCapture;

#if ENABLE_PERF_TELEMETRY
typedef struct PerfCaptureConfiguration {
    int frame_count;
    const char* output_path;
    const char* scene;
    bool basic_mode;
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

#if ENABLE_PERF_TELEMETRY
extern bool perf_update_breakdown_enabled;

static inline bool PerfUpdateBreakdown_IsEnabled(void) {
    return perf_update_breakdown_enabled;
}

void PerfUpdateBreakdown_SetEnabled(bool enabled);
void PerfUpdateBreakdown_ResetCapture(void);
uint64_t PerfUpdateBreakdown_Begin(PerfUpdateScope scope);
void PerfUpdateBreakdown_End(PerfUpdateScope scope, uint64_t start_ns);
bool PerfUpdateBreakdown_IsGameTaskSeqsAfterActive(void);
void PerfUpdateBreakdown_SetGameTaskSeqsAfterActive(bool active);
void PerfUpdateBreakdown_GetCapture(PerfUpdateBreakdownCapture* out_capture);
const char* PerfUpdateBreakdown_ScopeName(PerfUpdateScope scope);
#else
static inline bool PerfUpdateBreakdown_IsEnabled(void) {
    return false;
}

static inline void PerfUpdateBreakdown_SetEnabled(bool enabled) {
    (void)enabled;
}

static inline void PerfUpdateBreakdown_ResetCapture(void) {}

static inline uint64_t PerfUpdateBreakdown_Begin(PerfUpdateScope scope) {
    (void)scope;
    return 0;
}

static inline void PerfUpdateBreakdown_End(PerfUpdateScope scope, uint64_t start_ns) {
    (void)scope;
    (void)start_ns;
}

static inline bool PerfUpdateBreakdown_IsGameTaskSeqsAfterActive(void) {
    return false;
}

static inline void PerfUpdateBreakdown_SetGameTaskSeqsAfterActive(bool active) {
    (void)active;
}

static inline void PerfUpdateBreakdown_GetCapture(PerfUpdateBreakdownCapture* out_capture) {
    (void)out_capture;
}

static inline const char* PerfUpdateBreakdown_ScopeName(PerfUpdateScope scope) {
    (void)scope;
    return "";
}
#endif

#endif
