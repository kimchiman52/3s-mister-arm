#if CRS_VIDEO_DRIVER_SOFTWARE

#ifndef SOFTWARE_RENDERER_H
#define SOFTWARE_RENDERER_H

#include "core/render_primitives.h"
#include "platform/video/software/sw_blit.h"
#include "rendering/game_renderer.h"

#include <stdbool.h>
#include <stdint.h>

// Public draw surface — Renderer_* declarations live in
// include/rendering/game_renderer.h. The software backend defines them in
// software_renderer.c.

// Internal

bool SoftwareRenderer_Init(bool nearest_filter, int scale);
void SoftwareRenderer_Quit();
void SoftwareRenderer_RenderFrame();
/* Drain queued geometry without changing the completed canvas -- or, when a
 * base was snapshotted after the last RenderFrame, restore that base so the
 * caller can composite this frame's overlays over it. Returns the number of
 * quads discarded. */
int SoftwareRenderer_HoldLastFrame();
/* Capture the canvas as the held base (call after a game pass, before the
 * overlay pass). Invalidated by the next RenderFrame. */
void SoftwareRenderer_SnapshotHeldBase();
/* Rasterize the queued geometry over the canvas as it stands (no clear). */
void SoftwareRenderer_RenderOverlay();
/* True when a held frame has a base buffer to be restored from, i.e. when it is
 * safe to composite this frame's overlays over it with RenderOverlay. False only
 * when the Init allocation failed; compositing then has no clean canvas to start
 * from and each frame's text would accumulate on the last one's, so the caller
 * must hold the frame WITHOUT an overlay pass (the pre-0f45de57 behaviour). */
bool SoftwareRenderer_HoldCanComposite(void);
#ifdef NETPLAY_TEST_HOOKS
/* Test-only: free the held base to reach the degraded path above. */
void SoftwareRenderer_TestHook_DropHeldBase(void);
#endif
int SoftwareRenderer_GetPerfPeakQuads(void);

// Canvas accessor for the host app driver to present (SDL streaming texture, DRM dumb buffer, etc.).
// Pixel layout is ARGB8888 (default) or RGB565 (with CRS_SW_CANVAS_16BPP) — see SWCanvasPixel in
// sw_blit.h. Tightly packed (pitch == width * sizeof(SWCanvasPixel)) unless pitch_bytes reports otherwise.
const SWCanvasPixel* SoftwareRenderer_GetCanvas(int* out_width, int* out_height, int* out_pitch_bytes);
bool SoftwareRenderer_UsesNearestFilter();

#endif

#endif // CRS_VIDEO_DRIVER_SOFTWARE
