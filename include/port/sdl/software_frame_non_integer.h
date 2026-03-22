#ifndef SOFTWARE_FRAME_NON_INTEGER_H
#define SOFTWARE_FRAME_NON_INTEGER_H

#include <SDL3/SDL.h>
#include <stdbool.h>

typedef struct SDLSoftwareFrame_NonIntegerTelemetry {
    Uint64 sampled_lookup_x_ns;
    Uint64 sampled_lookup_y_ns;
    Uint64 sampled_pair_lookup_ns;
    Uint64 sampled_reuse_telemetry_ns;
    Uint64 sampled_row_raster_ns;
    Uint64 x_lookup_signature;
    Uint64 y_lookup_signature;
    Uint64 source_alpha_opaque_pixels;
    Uint64 source_alpha_transparent_pixels;
    Uint64 source_alpha_blended_pixels;
    Uint64 subrect_rows_total;
    Uint64 subrect_rows_all_opaque;
    Uint64 subrect_rows_all_transparent;
    Uint64 subrect_rows_binary_alpha_only;
    Uint64 subrect_rows_binary_mixed;
    Uint64 subrect_rows_with_blended;
    Uint64 source_alpha_opaque_spans;
    Uint64 source_alpha_transparent_spans;
    Uint64 source_alpha_blended_spans;
    int source_alpha_opaque_span_max;
    int source_alpha_transparent_span_max;
    int source_alpha_blended_span_max;
    Uint64 same_source_runs;
    Uint64 same_source_reuse_runs;
    Uint64 same_source_reused_pixels;
    Uint64 same_source_opaque_reused_pixels;
    Uint64 same_source_transparent_reused_pixels;
    Uint64 same_source_blended_reused_pixels;
    Uint64 same_source_pair_runs;
    Uint64 same_source_pair_leading_non_pair_pixels;
    Uint64 same_source_pair_trailing_non_pair_pixels;
    Uint64 same_source_pair_gap_0_runs;
    Uint64 same_source_pair_gap_1_runs;
    Uint64 same_source_pair_gap_2_runs;
    Uint64 same_source_pair_gap_3_plus_runs;
    int same_source_max_run_length;
} SDLSoftwareFrame_NonIntegerTelemetry;

bool SDLSoftwareFrame_RasterNonIntegerLookupARGB8888(const SDL_FRect* dst_rect,
                                                     const SDL_FRect* src_uv_rect,
                                                     SDL_FlipMode flip,
                                                     Uint32 color,
                                                     SDL_Surface* dst_surface,
                                                     const SDL_Surface* src_surface,
                                                     SDLSoftwareFrame_NonIntegerTelemetry* out_telemetry,
                                                     bool sample_phase_timing,
                                                     bool collect_reuse_telemetry,
                                                     bool collect_subrect_alpha_telemetry);

#endif
