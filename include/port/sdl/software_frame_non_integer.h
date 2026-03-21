#ifndef SOFTWARE_FRAME_NON_INTEGER_H
#define SOFTWARE_FRAME_NON_INTEGER_H

#include <SDL3/SDL.h>
#include <stdbool.h>

typedef struct SDLSoftwareFrame_NonIntegerTelemetry {
    Uint64 source_alpha_opaque_pixels;
    Uint64 source_alpha_transparent_pixels;
    Uint64 source_alpha_blended_pixels;
    Uint64 same_source_runs;
    Uint64 same_source_reuse_runs;
    Uint64 same_source_reused_pixels;
    Uint64 same_source_opaque_reused_pixels;
    Uint64 same_source_transparent_reused_pixels;
    Uint64 same_source_blended_reused_pixels;
    int same_source_max_run_length;
} SDLSoftwareFrame_NonIntegerTelemetry;

bool SDLSoftwareFrame_RasterNonIntegerLookupARGB8888(const SDL_FRect* dst_rect,
                                                     const SDL_FRect* src_uv_rect,
                                                     SDL_FlipMode flip,
                                                     Uint32 color,
                                                     SDL_Surface* dst_surface,
                                                     const SDL_Surface* src_surface,
                                                     SDLSoftwareFrame_NonIntegerTelemetry* out_telemetry);

#endif
