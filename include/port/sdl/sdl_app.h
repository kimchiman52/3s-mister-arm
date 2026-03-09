#ifndef SDL_APP_H
#define SDL_APP_H

#include <SDL3/SDL.h>

#define TARGET_FPS 59.59949

extern SDL_Window* window;

int SDLApp_Init();
void SDLApp_Quit();

/// Configure optional frame-stage perf capture.
/// `frame_count` <= 0 disables capture.
void SDLApp_ConfigurePerfCapture(int frame_count, const char* output_path, const char* scene_name);

/// @brief Poll SDL events.
/// @return `true` if the main loop should continue running, `false` otherwise.
bool SDLApp_PollEvents();

void SDLApp_BeginFrame();
void SDLApp_EndFrame();
void SDLApp_Exit();

#endif
