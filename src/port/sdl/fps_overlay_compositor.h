#ifndef PORT_SDL_FPS_OVERLAY_COMPOSITOR_H
#define PORT_SDL_FPS_OVERLAY_COMPOSITOR_H

#include <SDL3/SDL.h>

/// Set the FPS overlay display mode.
/// 0 = off, 1 = top-left (FPS only), 2 = bottom-center (debug).
void FPSOverlay_SetMode(int mode);

/// Update the on-screen FPS overlay label.
void FPSOverlay_SetText(const char* text);

/// Composite the FPS overlay onto an arbitrary ARGB8888 buffer.
/// Used by the native video writer path.
void FPSOverlay_ApplyToBuffer(Uint32* pixels, int width, int height);

/// 565 sibling. Used when the canvas is RGB565 and fed directly to the
/// native video writer.
void FPSOverlay_ApplyToRGB565Buffer(Uint16* pixels, int width, int height);

#endif
