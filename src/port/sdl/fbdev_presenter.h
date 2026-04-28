#ifndef PORT_SDL_FBDEV_PRESENTER_H
#define PORT_SDL_FBDEV_PRESENTER_H

#include <stdbool.h>
#include <stddef.h>

#include <SDL3/SDL.h>

typedef enum FBDevPresenterPath {
    FBDEV_PRESENTER_PATH_NONE = 0,
    FBDEV_PRESENTER_PATH_READBACK_RECT,
    FBDEV_PRESENTER_PATH_CURRENT_TARGET_EXACT,
    FBDEV_PRESENTER_PATH_CURRENT_TARGET_INTEGER_SCALE,
    FBDEV_PRESENTER_PATH_CURRENT_TARGET_MAPPED_SCALE,
    FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_EXACT,
    FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_INTEGER_SCALE,
    FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_MAPPED_SCALE,
    FBDEV_PRESENTER_PATH_FULLSCREEN_STAGING,
    FBDEV_PRESENTER_PATH_FULLSCREEN_DIRECT_COPY,
    FBDEV_PRESENTER_PATH_FULLSCREEN_SCALED_LUT,
    FBDEV_PRESENTER_PATH_FULLSCREEN_SCALED_DIV,
    FBDEV_PRESENTER_PATH_COUNT
} FBDevPresenterPath;

typedef struct FBDevPresenter_FrameStats {
    Uint64 readback_ns;
    Uint64 convert_ns;
    Uint64 copy_ns;
    Uint64 clear_ns;
    Uint64 mapped_changed_rows;
    Uint64 mapped_row_runs;
    Uint64 mapped_row_runs_max;
    Uint64 mapped_repeat_rows;
    Uint64 mapped_repeat_run_copies;
    Uint64 mapped_repeat_dense_rows;
    Uint64 mapped_repeat_gap_pixels;
    Uint64 mapped_repeat_template_rows;
    Uint64 mapped_repeat_template_run_copies;
    Uint64 mapped_repeat_template_dense_rows;
    Uint64 mapped_first_row_ns;
    Uint64 mapped_repeat_row_ns;
    Uint32 readback_format;
    int readback_width;
    int readback_height;
    FBDevPresenterPath path;
} FBDevPresenter_FrameStats;

/// Initialize fbdev presenter that mirrors rendered frames to /dev/fb0.
/// Returns true when presenter is active.
bool FBDevPresenter_Init(void);

/// True if fbdev presenter is active.
bool FBDevPresenter_IsActive(void);

/// Framebuffer width in pixels, or 0 when inactive.
int FBDevPresenter_GetWidth(void);

/// Framebuffer height in pixels, or 0 when inactive.
int FBDevPresenter_GetHeight(void);

/// Copy current SDL renderer frame to /dev/fb0.
/// If `content_rect` is non-NULL, presenter may read only that region and
/// black-fill everything outside it.
void FBDevPresenter_Present(SDL_Renderer* renderer, const SDL_FRect* content_rect);

/// Copy the renderer's current render target to /dev/fb0 at `dst_rect`.
/// Intended for fbdev-only native-path presentation where the current target
/// already contains the game buffer and the presenter can crop or nearest-scale
/// it directly into the framebuffer.
bool FBDevPresenter_PresentCurrentTarget(SDL_Renderer* renderer, const SDL_FRect* dst_rect);

/// Copy a caller-owned surface directly to /dev/fb0 at `dst_rect`.
/// Intended for future software-owned game-frame presentation without
/// requiring an SDL render-target readback first.
bool FBDevPresenter_PresentSurface(const SDL_Surface* surface, const SDL_FRect* dst_rect);

/// Reset per-frame presenter stats.
void FBDevPresenter_BeginFrameStats(bool capture_breakdown);

/// Bytes copied/cleared by fbdev presenter during current frame.
size_t FBDevPresenter_GetFrameCopyBytes(void);

/// Per-frame presenter breakdown stats.
void FBDevPresenter_GetFrameStats(FBDevPresenter_FrameStats* out_stats);

/// Number of tiles considered by the staging presenter in current frame.
int FBDevPresenter_GetFrameTilesTotal(void);

/// Number of tiles copied by the staging presenter in current frame.
int FBDevPresenter_GetFrameTilesCopied(void);

/// True when the presenter skipped tile-diff and forced a full-copy update.
bool FBDevPresenter_UsedFullCopyFallback(void);

/// Set the FPS overlay display mode.
/// 0 = off, 1 = top-left (FPS only), 2 = bottom-center (debug).
void FBDevPresenter_SetFPSOverlayMode(int mode);

/// Update the on-screen FPS overlay label.
void FBDevPresenter_SetFPSOverlayText(const char* text);

/// Composite the FPS overlay onto an arbitrary ARGB8888 buffer.
/// Used by the native video writer path which bypasses the fbdev presenter.
void FBDevPresenter_ApplyFPSOverlayToBuffer(Uint32* pixels, int width, int height);

/// perf-2: 565 sibling. Used when the canvas is RGB565 and fed directly to
/// the native video writer.
void FBDevPresenter_ApplyFPSOverlayToRGB565Buffer(Uint16* pixels, int width, int height);

/// Human-readable presenter path name for perf output.
const char* FBDevPresenter_PathName(FBDevPresenterPath path);

/// Release fbdev presenter resources.
void FBDevPresenter_Quit(void);

#endif
