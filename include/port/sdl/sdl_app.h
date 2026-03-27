#ifndef SDL_APP_H
#define SDL_APP_H

#include "port/build_config.h"
#include "port/sdl/sdl_game_renderer.h"
#include <SDL3/SDL.h>

#define TARGET_FPS 59.59949

extern SDL_Window* window;

int SDLApp_Init();
void SDLApp_Quit();
void SDLApp_ToggleFPSOverlay(void);
SDLGameRenderer_SuperEffectQualityMode SDLApp_GetSuperEffectQualityMode(void);
void SDLApp_SetSuperEffectQualityMode(SDLGameRenderer_SuperEffectQualityMode mode);
void SDLApp_CycleSuperEffectQualityMode(void);
SDLGameRenderer_GhostResolutionMode SDLApp_GetGhostResolutionMode(void);
void SDLApp_CycleGhostResolutionMode(void);
int SDLApp_GetGhostCountMax(void);
void SDLApp_CycleGhostCountMax(void);
int SDLApp_GetArmClock(void);
void SDLApp_CycleArmClock(void);

static inline bool SDLApp_HasPerfTelemetry(void) {
    return ENABLE_PERF_TELEMETRY != 0;
}

#if ENABLE_PERF_TELEMETRY
bool SDLApp_RunSoftwareFrameParityCheck(void);

/// Configure optional frame-stage perf capture.
/// `frame_count` <= 0 disables capture.
void SDLApp_ConfigurePerfCapture(
    int frame_count,
    const char* output_path,
    const char* scene_name,
    bool basic_mode,
    bool basic_first_window_family_snapshots,
    bool basic_first_window_render_subphases,
    bool basic_first_window_exact_hot_family_alpha_offpath,
    bool basic_first_window_onset_exact_hot_family_alpha_offpath,
    bool basic_first_window_onset_cluster_alpha_offpath,
    bool disable_reuse_telemetry,
    bool enable_subrect_alpha_telemetry);
bool SDLApp_IsPerfRuntimeStateActive(const char* runtime_state_name);
#else
static inline bool SDLApp_RunSoftwareFrameParityCheck(void) {
    return false;
}

static inline void
SDLApp_ConfigurePerfCapture(int frame_count,
                            const char* output_path,
                            const char* scene_name,
                            bool basic_mode,
                            bool basic_first_window_family_snapshots,
                            bool basic_first_window_render_subphases,
                            bool basic_first_window_exact_hot_family_alpha_offpath,
                            bool basic_first_window_onset_exact_hot_family_alpha_offpath,
                            bool basic_first_window_onset_cluster_alpha_offpath,
                            bool disable_reuse_telemetry,
                            bool enable_subrect_alpha_telemetry) {
    (void)frame_count;
    (void)output_path;
    (void)scene_name;
    (void)basic_mode;
    (void)basic_first_window_family_snapshots;
    (void)basic_first_window_render_subphases;
    (void)basic_first_window_exact_hot_family_alpha_offpath;
    (void)basic_first_window_onset_exact_hot_family_alpha_offpath;
    (void)basic_first_window_onset_cluster_alpha_offpath;
    (void)disable_reuse_telemetry;
    (void)enable_subrect_alpha_telemetry;
}

static inline bool SDLApp_IsPerfRuntimeStateActive(const char* runtime_state_name) {
    (void)runtime_state_name;
    return false;
}
#endif

/// @brief Poll SDL events.
/// @return `true` if the main loop should continue running, `false` otherwise.
bool SDLApp_PollEvents();

void SDLApp_BeginFrame();
void SDLApp_EndFrame();
void SDLApp_Exit();

#endif
