#ifndef SDL_APP_H
#define SDL_APP_H

#include "port/build_config.h"
#include <SDL3/SDL.h>

#define TARGET_FPS 59.59949

extern SDL_Window* window;

int SDLApp_Init();
void SDLApp_Quit();

static inline bool SDLApp_HasPerfTelemetry(void) {
    return ENABLE_PERF_TELEMETRY != 0;
}

#if ENABLE_PERF_TELEMETRY
bool SDLApp_RunSoftwareFrameParityCheck(void);

/// Configure optional frame-stage perf capture.
/// `frame_count` <= 0 disables capture.
void SDLApp_ConfigurePerfCapture(int frame_count, const char* output_path, const char* scene_name, bool basic_mode);
#else
static inline bool SDLApp_RunSoftwareFrameParityCheck(void) {
    return false;
}

static inline void
SDLApp_ConfigurePerfCapture(int frame_count, const char* output_path, const char* scene_name, bool basic_mode) {
    (void)frame_count;
    (void)output_path;
    (void)scene_name;
    (void)basic_mode;
}
#endif

/// @brief Poll SDL events.
/// @return `true` if the main loop should continue running, `false` otherwise.
bool SDLApp_PollEvents();

void SDLApp_BeginFrame();
void SDLApp_EndFrame();
void SDLApp_Exit();

#endif
