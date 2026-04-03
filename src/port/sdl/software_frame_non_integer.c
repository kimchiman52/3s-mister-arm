#include "port/sdl/software_frame_non_integer.h"

enum {
    software_frame_lookup_max_width = 384,
    software_frame_lookup_max_height = 224,
};

static Uint64 performance_counter_delta_to_ns(Uint64 start, Uint64 end, Uint64 frequency) {
    if ((end <= start) || (frequency == 0u)) {
        return 0;
    }
    return ((end - start) * 1000000000ull) / frequency;
}

static int clamp_to_range(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static Uint64 hash_non_integer_lookup_signature(const int* lookup, int count) {
    if ((lookup == NULL) || (count <= 0)) {
        return 0u;
    }

    Uint64 hash = 1469598103934665603ull;
    hash ^= (Uint64)(Uint32)count;
    hash *= 1099511628211ull;
    for (int i = 0; i < count; i++) {
        hash ^= (Uint64)(Uint32)lookup[i];
        hash *= 1099511628211ull;
    }

    return hash;
}

static Uint32 modulate_argb8888(Uint32 pixel, Uint32 color) {
    const Uint32 src_a = (pixel >> 24) & 0xFFu;
    const Uint32 src_r = (pixel >> 16) & 0xFFu;
    const Uint32 src_g = (pixel >> 8) & 0xFFu;
    const Uint32 src_b = pixel & 0xFFu;
    const Uint32 mod_a = (color >> 24) & 0xFFu;
    const Uint32 mod_r = (color >> 16) & 0xFFu;
    const Uint32 mod_g = (color >> 8) & 0xFFu;
    const Uint32 mod_b = color & 0xFFu;
    const Uint32 out_a = (src_a * mod_a + 128u) >> 8;
    const Uint32 out_r = (src_r * mod_r + 128u) >> 8;
    const Uint32 out_g = (src_g * mod_g + 128u) >> 8;
    const Uint32 out_b = (src_b * mod_b + 128u) >> 8;
    return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static Uint8 blend_argb8888_channel(Uint32 src_c, Uint32 src_a, Uint32 dst_c, Uint32 dst_a, Uint32 out_a) {
    if (out_a == 0u) {
        return 0;
    }

    const Uint32 src_premul = src_c * src_a;
    const Uint32 dst_premul = dst_c * dst_a;
    const Uint32 out_premul = src_premul + ((dst_premul * (255u - src_a) + 128u) >> 8);
    return (Uint8)((out_premul + (out_a / 2u)) / out_a);
}

static Uint32 blend_argb8888(Uint32 dst_pixel, Uint32 src_pixel) {
    const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
    if (src_a == 0u) {
        return dst_pixel;
    }
    if (src_a == 255u) {
        return src_pixel;
    }

    const Uint32 dst_a = (dst_pixel >> 24) & 0xFFu;
    if (dst_a == 255u) {
        const Uint32 inv_src_a = 255u - src_a;
        const Uint32 src_r = (src_pixel >> 16) & 0xFFu;
        const Uint32 src_g = (src_pixel >> 8) & 0xFFu;
        const Uint32 src_b = src_pixel & 0xFFu;
        const Uint32 dst_r = (dst_pixel >> 16) & 0xFFu;
        const Uint32 dst_g = (dst_pixel >> 8) & 0xFFu;
        const Uint32 dst_b = dst_pixel & 0xFFu;
        const Uint32 out_r = ((src_r * src_a) + (dst_r * inv_src_a) + 128u) >> 8;
        const Uint32 out_g = ((src_g * src_a) + (dst_g * inv_src_a) + 128u) >> 8;
        const Uint32 out_b = ((src_b * src_a) + (dst_b * inv_src_a) + 128u) >> 8;
        return 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
    }

    const Uint32 out_a = src_a + ((dst_a * (255u - src_a) + 128u) >> 8);
    const Uint32 src_r = (src_pixel >> 16) & 0xFFu;
    const Uint32 src_g = (src_pixel >> 8) & 0xFFu;
    const Uint32 src_b = src_pixel & 0xFFu;
    const Uint32 dst_r = (dst_pixel >> 16) & 0xFFu;
    const Uint32 dst_g = (dst_pixel >> 8) & 0xFFu;
    const Uint32 dst_b = dst_pixel & 0xFFu;
    const Uint8 out_r = blend_argb8888_channel(src_r, src_a, dst_r, dst_a, out_a);
    const Uint8 out_g = blend_argb8888_channel(src_g, src_a, dst_g, dst_a, out_a);
    const Uint8 out_b = blend_argb8888_channel(src_b, src_a, dst_b, dst_a, out_a);
    return (out_a << 24) | ((Uint32)out_r << 16) | ((Uint32)out_g << 8) | (Uint32)out_b;
}

typedef enum NonIntegerSourceAlphaClass {
    NON_INTEGER_SOURCE_ALPHA_CLASS_NONE = -1,
    NON_INTEGER_SOURCE_ALPHA_CLASS_TRANSPARENT = 0,
    NON_INTEGER_SOURCE_ALPHA_CLASS_OPAQUE = 1,
    NON_INTEGER_SOURCE_ALPHA_CLASS_BLENDED = 2,
} NonIntegerSourceAlphaClass;

typedef struct NonIntegerSubrectAlphaRowTelemetry {
    Uint64 opaque_pixels;
    Uint64 transparent_pixels;
    Uint64 blended_pixels;
    Uint64 opaque_spans;
    Uint64 transparent_spans;
    Uint64 blended_spans;
    int opaque_span_max;
    int transparent_span_max;
    int blended_span_max;
    int current_span_length;
    NonIntegerSourceAlphaClass current_class;
} NonIntegerSubrectAlphaRowTelemetry;

static NonIntegerSourceAlphaClass classify_non_integer_source_alpha(Uint32 src_a) {
    if (src_a == 0u) {
        return NON_INTEGER_SOURCE_ALPHA_CLASS_TRANSPARENT;
    }
    if (src_a == 0xFFu) {
        return NON_INTEGER_SOURCE_ALPHA_CLASS_OPAQUE;
    }
    return NON_INTEGER_SOURCE_ALPHA_CLASS_BLENDED;
}

static void flush_non_integer_subrect_alpha_span(NonIntegerSubrectAlphaRowTelemetry* row_telemetry) {
    if ((row_telemetry == NULL) || (row_telemetry->current_span_length <= 0)) {
        return;
    }

    switch (row_telemetry->current_class) {
    case NON_INTEGER_SOURCE_ALPHA_CLASS_TRANSPARENT:
        row_telemetry->transparent_spans += 1u;
        if (row_telemetry->current_span_length > row_telemetry->transparent_span_max) {
            row_telemetry->transparent_span_max = row_telemetry->current_span_length;
        }
        break;
    case NON_INTEGER_SOURCE_ALPHA_CLASS_OPAQUE:
        row_telemetry->opaque_spans += 1u;
        if (row_telemetry->current_span_length > row_telemetry->opaque_span_max) {
            row_telemetry->opaque_span_max = row_telemetry->current_span_length;
        }
        break;
    case NON_INTEGER_SOURCE_ALPHA_CLASS_BLENDED:
        row_telemetry->blended_spans += 1u;
        if (row_telemetry->current_span_length > row_telemetry->blended_span_max) {
            row_telemetry->blended_span_max = row_telemetry->current_span_length;
        }
        break;
    case NON_INTEGER_SOURCE_ALPHA_CLASS_NONE:
    default:
        break;
    }

    row_telemetry->current_class = NON_INTEGER_SOURCE_ALPHA_CLASS_NONE;
    row_telemetry->current_span_length = 0;
}

static void note_non_integer_subrect_alpha_run(NonIntegerSubrectAlphaRowTelemetry* row_telemetry,
                                               Uint32 src_a,
                                               int run_length) {
    if ((row_telemetry == NULL) || (run_length <= 0)) {
        return;
    }

    const NonIntegerSourceAlphaClass alpha_class = classify_non_integer_source_alpha(src_a);
    switch (alpha_class) {
    case NON_INTEGER_SOURCE_ALPHA_CLASS_TRANSPARENT:
        row_telemetry->transparent_pixels += (Uint64)run_length;
        break;
    case NON_INTEGER_SOURCE_ALPHA_CLASS_OPAQUE:
        row_telemetry->opaque_pixels += (Uint64)run_length;
        break;
    case NON_INTEGER_SOURCE_ALPHA_CLASS_BLENDED:
        row_telemetry->blended_pixels += (Uint64)run_length;
        break;
    case NON_INTEGER_SOURCE_ALPHA_CLASS_NONE:
    default:
        break;
    }

    if (row_telemetry->current_class == alpha_class) {
        row_telemetry->current_span_length += run_length;
        return;
    }

    flush_non_integer_subrect_alpha_span(row_telemetry);
    row_telemetry->current_class = alpha_class;
    row_telemetry->current_span_length = run_length;
}

static void finish_non_integer_subrect_alpha_row(const NonIntegerSubrectAlphaRowTelemetry* row_telemetry,
                                                 SDLSoftwareFrame_NonIntegerTelemetry* telemetry) {
    if ((row_telemetry == NULL) || (telemetry == NULL)) {
        return;
    }

    NonIntegerSubrectAlphaRowTelemetry finalized = *row_telemetry;
    flush_non_integer_subrect_alpha_span(&finalized);

    telemetry->source_alpha_opaque_pixels += finalized.opaque_pixels;
    telemetry->source_alpha_transparent_pixels += finalized.transparent_pixels;
    telemetry->source_alpha_blended_pixels += finalized.blended_pixels;
    telemetry->subrect_rows_total += 1u;
    telemetry->source_alpha_opaque_spans += finalized.opaque_spans;
    telemetry->source_alpha_transparent_spans += finalized.transparent_spans;
    telemetry->source_alpha_blended_spans += finalized.blended_spans;
    if (finalized.opaque_span_max > telemetry->source_alpha_opaque_span_max) {
        telemetry->source_alpha_opaque_span_max = finalized.opaque_span_max;
    }
    if (finalized.transparent_span_max > telemetry->source_alpha_transparent_span_max) {
        telemetry->source_alpha_transparent_span_max = finalized.transparent_span_max;
    }
    if (finalized.blended_span_max > telemetry->source_alpha_blended_span_max) {
        telemetry->source_alpha_blended_span_max = finalized.blended_span_max;
    }

    if (finalized.blended_pixels == 0u) {
        telemetry->subrect_rows_binary_alpha_only += 1u;
        if ((finalized.opaque_pixels > 0u) && (finalized.transparent_pixels > 0u)) {
            telemetry->subrect_rows_binary_mixed += 1u;
        } else if (finalized.opaque_pixels > 0u) {
            telemetry->subrect_rows_all_opaque += 1u;
        } else if (finalized.transparent_pixels > 0u) {
            telemetry->subrect_rows_all_transparent += 1u;
        }
        return;
    }

    telemetry->subrect_rows_with_blended += 1u;
}

static void note_non_integer_row_reuse_telemetry(const int* src_x_lookup,
                                                 int visible_w,
                                                 const Uint32* src_row,
                                                 Uint32 color,
                                                 bool apply_color_mod,
                                                 SDLSoftwareFrame_NonIntegerTelemetry* telemetry,
                                                 bool count_source_alpha_pixels) {
    if ((src_x_lookup == NULL) || (src_row == NULL) || (telemetry == NULL) || (visible_w <= 0)) {
        return;
    }

    bool saw_pair = false;
    int previous_pair_end = 0;
    for (int col = 0; col < visible_w;) {
        const int src_col = src_x_lookup[col];
        int run_end = col + 1;
        while ((run_end < visible_w) && (src_x_lookup[run_end] == src_col)) {
            run_end += 1;
        }

        const int run_length = run_end - col;
        telemetry->same_source_runs += 1u;
        if (run_length > telemetry->same_source_max_run_length) {
            telemetry->same_source_max_run_length = run_length;
        }

        Uint32 src_pixel = src_row[src_col];
        if (apply_color_mod) {
            src_pixel = modulate_argb8888(src_pixel, color);
        }

        const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
        if (count_source_alpha_pixels) {
            if (src_a == 0u) {
                telemetry->source_alpha_transparent_pixels += (Uint64)run_length;
            } else if (src_a == 0xFFu) {
                telemetry->source_alpha_opaque_pixels += (Uint64)run_length;
            } else {
                telemetry->source_alpha_blended_pixels += (Uint64)run_length;
            }
        }

        if (run_length > 1) {
            const Uint64 reused_pixels = (Uint64)(run_length - 1);
            telemetry->same_source_reuse_runs += 1u;
            telemetry->same_source_reused_pixels += reused_pixels;
            if (src_a == 0u) {
                telemetry->same_source_transparent_reused_pixels += reused_pixels;
            } else if (src_a == 0xFFu) {
                telemetry->same_source_opaque_reused_pixels += reused_pixels;
            } else {
                telemetry->same_source_blended_reused_pixels += reused_pixels;
            }
        }

        const int pair_count = run_length / 2;
        for (int pair_index = 0; pair_index < pair_count; pair_index++) {
            const int pair_start = col + (pair_index * 2);
            telemetry->same_source_pair_runs += 1u;
            if (!saw_pair) {
                telemetry->same_source_pair_leading_non_pair_pixels += (Uint64)pair_start;
                saw_pair = true;
            } else {
                const int inter_pair_columns = pair_start - previous_pair_end;
                if (inter_pair_columns <= 0) {
                    telemetry->same_source_pair_gap_0_runs += 1u;
                } else if (inter_pair_columns == 1) {
                    telemetry->same_source_pair_gap_1_runs += 1u;
                } else if (inter_pair_columns == 2) {
                    telemetry->same_source_pair_gap_2_runs += 1u;
                } else {
                    telemetry->same_source_pair_gap_3_plus_runs += 1u;
                }
            }
            previous_pair_end = pair_start + 2;
        }

        col = run_end;
    }

    if (saw_pair && (previous_pair_end < visible_w)) {
        telemetry->same_source_pair_trailing_non_pair_pixels += (Uint64)(visible_w - previous_pair_end);
    }
}

static void populate_non_integer_lookup(int* out_lookup,
                                        int visible_count,
                                        int visible_start,
                                        float dst_origin,
                                        float dst_span,
                                        float src_origin,
                                        float src_span,
                                        int src_limit,
                                        bool flip) {
    for (int i = 0; i < visible_count; i++) {
        const float dst_coordinate = (float)(visible_start + i);
        float coord = ((dst_coordinate + 0.5f) - dst_origin) / dst_span;
        coord = SDL_max(0.0f, SDL_min(coord, 0.999999f));
        if (flip) {
            coord = 1.0f - coord;
            coord = SDL_max(0.0f, SDL_min(coord, 0.999999f));
        }
        out_lookup[i] = clamp_to_range((int)SDL_floorf(src_origin + (coord * src_span)), 0, src_limit);
    }
}

static bool populate_same_source_pair_lookup(Uint8* out_pair_lookup, const int* src_x_lookup, int visible_w) {
    if ((out_pair_lookup == NULL) || (src_x_lookup == NULL) || (visible_w <= 1)) {
        return false;
    }

    SDL_memset(out_pair_lookup, 0, sizeof(*out_pair_lookup) * (size_t)visible_w);
    bool has_pairs = false;
    for (int col = 0; col < (visible_w - 1); col++) {
        if (src_x_lookup[col] != src_x_lookup[col + 1]) {
            continue;
        }
        out_pair_lookup[col] = 1u;
        has_pairs = true;
    }
    return has_pairs;
}

bool SDLSoftwareFrame_AnalyzeNonIntegerSourceAlphaARGB8888(const SDL_FRect* dst_rect,
                                                           const SDL_Rect* src_rect,
                                                           SDL_FlipMode flip,
                                                           Uint32 color,
                                                           const SDL_Surface* src_surface,
                                                           SDLSoftwareFrame_NonIntegerTelemetry* out_telemetry) {
    if ((dst_rect == NULL) || (src_rect == NULL) || (src_surface == NULL) || (out_telemetry == NULL) ||
        (dst_rect->w <= 0.0f) || (dst_rect->h <= 0.0f) || (src_rect->w <= 0) || (src_rect->h <= 0) ||
        (src_rect->x < 0) || (src_rect->y < 0) ||
        ((src_rect->x + src_rect->w) > src_surface->w) || ((src_rect->y + src_rect->h) > src_surface->h) ||
        (src_surface->w <= 0) || (src_surface->h <= 0)) {
        return false;
    }

    const int dst_x0 = (int)SDL_floorf(dst_rect->x);
    const int dst_y0 = (int)SDL_floorf(dst_rect->y);
    const int dst_x1 = (int)SDL_ceilf(dst_rect->x + dst_rect->w);
    const int dst_y1 = (int)SDL_ceilf(dst_rect->y + dst_rect->h);
    const int visible_w = dst_x1 - dst_x0;
    const int visible_h = dst_y1 - dst_y0;
    if ((visible_w <= 0) || (visible_h <= 0)) {
        return false;
    }
    if ((visible_w > software_frame_lookup_max_width) || (visible_h > software_frame_lookup_max_height)) {
        return false;
    }

    SDL_zero(*out_telemetry);

    static int src_x_lookup[software_frame_lookup_max_width];
    static int src_y_lookup[software_frame_lookup_max_height];
    static Uint8 same_source_pair_lookup[software_frame_lookup_max_width];
    populate_non_integer_lookup(src_x_lookup,
                                visible_w,
                                dst_x0,
                                dst_rect->x,
                                dst_rect->w,
                                (float)src_rect->x,
                                (float)src_rect->w,
                                src_surface->w - 1,
                                (flip & SDL_FLIP_HORIZONTAL) != 0);
    populate_non_integer_lookup(src_y_lookup,
                                visible_h,
                                dst_y0,
                                dst_rect->y,
                                dst_rect->h,
                                (float)src_rect->y,
                                (float)src_rect->h,
                                src_surface->h - 1,
                                (flip & SDL_FLIP_VERTICAL) != 0);
    const bool has_same_source_pairs = populate_same_source_pair_lookup(same_source_pair_lookup, src_x_lookup, visible_w);
    const Uint32* src_pixels = (const Uint32*)src_surface->pixels;
    const int src_pitch = src_surface->pitch / (int)sizeof(Uint32);
    const bool apply_color_mod = color != 0xFFFFFFFFu;

    for (int row = 0; row < visible_h; row++) {
        const Uint32* src_row = src_pixels + (src_y_lookup[row] * src_pitch);
        NonIntegerSubrectAlphaRowTelemetry row_telemetry;
        SDL_zero(row_telemetry);
        row_telemetry.current_class = NON_INTEGER_SOURCE_ALPHA_CLASS_NONE;

        if (!apply_color_mod && has_same_source_pairs) {
            for (int col = 0; col < visible_w;) {
                const Uint32 src_pixel = src_row[src_x_lookup[col]];
                const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                const int run_length = (((col + 1) < visible_w) && same_source_pair_lookup[col]) ? 2 : 1;
                note_non_integer_subrect_alpha_run(&row_telemetry, src_a, run_length);
                col += run_length;
            }
            finish_non_integer_subrect_alpha_row(&row_telemetry, out_telemetry);
            continue;
        }

        for (int col = 0; col < visible_w; col++) {
            Uint32 src_pixel = src_row[src_x_lookup[col]];
            if (apply_color_mod) {
                src_pixel = modulate_argb8888(src_pixel, color);
            }
            note_non_integer_subrect_alpha_run(&row_telemetry, (src_pixel >> 24) & 0xFFu, 1);
        }
        finish_non_integer_subrect_alpha_row(&row_telemetry, out_telemetry);
    }

    return true;
}

bool SDLSoftwareFrame_RasterNonIntegerLookupARGB8888(const SDL_FRect* dst_rect,
                                                     const SDL_FRect* src_uv_rect,
                                                     SDL_FlipMode flip,
                                                     Uint32 color,
                                                     SDL_Surface* dst_surface,
                                                     const SDL_Surface* src_surface,
                                                     SDLSoftwareFrame_NonIntegerTelemetry* out_telemetry,
                                                     bool sample_phase_timing,
                                                     bool collect_lookup_signatures,
                                                     bool collect_reuse_telemetry,
                                                     bool collect_subrect_alpha_telemetry) {
    if ((dst_rect == NULL) || (src_uv_rect == NULL) || (dst_surface == NULL) || (src_surface == NULL) ||
        (dst_rect->w <= 0.0f) || (dst_rect->h <= 0.0f) || (src_surface->w <= 0) || (src_surface->h <= 0)) {
        return false;
    }
    if (out_telemetry != NULL) {
        SDL_zero(*out_telemetry);
    }

    const int dst_x0 = clamp_to_range((int)SDL_floorf(dst_rect->x), 0, dst_surface->w);
    const int dst_y0 = clamp_to_range((int)SDL_floorf(dst_rect->y), 0, dst_surface->h);
    const int dst_x1 = clamp_to_range((int)SDL_ceilf(dst_rect->x + dst_rect->w), 0, dst_surface->w);
    const int dst_y1 = clamp_to_range((int)SDL_ceilf(dst_rect->y + dst_rect->h), 0, dst_surface->h);
    const int visible_w = dst_x1 - dst_x0;
    const int visible_h = dst_y1 - dst_y0;
    if ((visible_w <= 0) || (visible_h <= 0)) {
        return true;
    }
    if ((visible_w > software_frame_lookup_max_width) || (visible_h > software_frame_lookup_max_height)) {
        return false;
    }

    static int src_x_lookup[software_frame_lookup_max_width];
    static int src_y_lookup[software_frame_lookup_max_height];
    static Uint8 same_source_pair_lookup[software_frame_lookup_max_width];
    const Uint64 perf_frequency = sample_phase_timing ? SDL_GetPerformanceFrequency() : 0u;
    Uint64 phase_start_counter = sample_phase_timing ? SDL_GetPerformanceCounter() : 0u;
    populate_non_integer_lookup(src_x_lookup,
                                visible_w,
                                dst_x0,
                                dst_rect->x,
                                dst_rect->w,
                                src_uv_rect->x * (float)src_surface->w,
                                src_uv_rect->w * (float)src_surface->w,
                                src_surface->w - 1,
                                (flip & SDL_FLIP_HORIZONTAL) != 0);
    if (sample_phase_timing && (out_telemetry != NULL)) {
        const Uint64 phase_end_counter = SDL_GetPerformanceCounter();
        out_telemetry->sampled_lookup_x_ns =
            performance_counter_delta_to_ns(phase_start_counter, phase_end_counter, perf_frequency);
        phase_start_counter = phase_end_counter;
    }
    populate_non_integer_lookup(src_y_lookup,
                                visible_h,
                                dst_y0,
                                dst_rect->y,
                                dst_rect->h,
                                src_uv_rect->y * (float)src_surface->h,
                                src_uv_rect->h * (float)src_surface->h,
                                src_surface->h - 1,
                                (flip & SDL_FLIP_VERTICAL) != 0);
    if (sample_phase_timing && (out_telemetry != NULL)) {
        const Uint64 phase_end_counter = SDL_GetPerformanceCounter();
        out_telemetry->sampled_lookup_y_ns =
            performance_counter_delta_to_ns(phase_start_counter, phase_end_counter, perf_frequency);
        phase_start_counter = phase_end_counter;
    }
    if ((out_telemetry != NULL) && collect_lookup_signatures) {
        out_telemetry->x_lookup_signature = hash_non_integer_lookup_signature(src_x_lookup, visible_w);
        out_telemetry->y_lookup_signature = hash_non_integer_lookup_signature(src_y_lookup, visible_h);
    }
    const bool has_same_source_pairs = populate_same_source_pair_lookup(same_source_pair_lookup, src_x_lookup, visible_w);
    if (sample_phase_timing && (out_telemetry != NULL)) {
        const Uint64 phase_end_counter = SDL_GetPerformanceCounter();
        out_telemetry->sampled_pair_lookup_ns =
            performance_counter_delta_to_ns(phase_start_counter, phase_end_counter, perf_frequency);
    }

    const Uint32* src_pixels = (const Uint32*)src_surface->pixels;
    Uint32* dst_pixels = (Uint32*)dst_surface->pixels;
    const int src_pitch = src_surface->pitch / (int)sizeof(Uint32);
    const int dst_pitch = dst_surface->pitch / (int)sizeof(Uint32);
    const bool apply_color_mod = color != 0xFFFFFFFFu;
    const bool collect_in_band_alpha_telemetry = collect_subrect_alpha_telemetry && (out_telemetry != NULL);
    const Uint64 row_phase_start_counter = sample_phase_timing ? SDL_GetPerformanceCounter() : 0u;

    for (int row = 0; row < visible_h; row++) {
        const Uint32* src_row = src_pixels + (src_y_lookup[row] * src_pitch);
        Uint32* dst_row = dst_pixels + ((dst_y0 + row) * dst_pitch) + dst_x0;
        if (collect_reuse_telemetry && (out_telemetry != NULL)) {
            const Uint64 telemetry_start_counter = sample_phase_timing ? SDL_GetPerformanceCounter() : 0u;
            note_non_integer_row_reuse_telemetry(
                src_x_lookup,
                visible_w,
                src_row,
                color,
                apply_color_mod,
                out_telemetry,
                !collect_in_band_alpha_telemetry);
            if (sample_phase_timing) {
                const Uint64 telemetry_end_counter = SDL_GetPerformanceCounter();
                out_telemetry->sampled_reuse_telemetry_ns +=
                    performance_counter_delta_to_ns(telemetry_start_counter, telemetry_end_counter, perf_frequency);
            }
        }
        if (!apply_color_mod) {
            if (collect_in_band_alpha_telemetry) {
                NonIntegerSubrectAlphaRowTelemetry row_telemetry;
                SDL_zero(row_telemetry);
                row_telemetry.current_class = NON_INTEGER_SOURCE_ALPHA_CLASS_NONE;

                if (!has_same_source_pairs) {
                    for (int col = 0; col < visible_w; col++) {
                        const Uint32 src_pixel = src_row[src_x_lookup[col]];
                        const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                        note_non_integer_subrect_alpha_run(&row_telemetry, src_a, 1);
                        if (src_a == 0u) {
                            continue;
                        }
                        if (src_a == 0xFFu) {
                            dst_row[col] = src_pixel;
                            continue;
                        }
                        dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
                    }
                    finish_non_integer_subrect_alpha_row(&row_telemetry, out_telemetry);
                    continue;
                }

                for (int col = 0; col < visible_w;) {
                    const Uint32 src_pixel = src_row[src_x_lookup[col]];
                    const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                    const int run_length = (((col + 1) < visible_w) && same_source_pair_lookup[col]) ? 2 : 1;
                    note_non_integer_subrect_alpha_run(&row_telemetry, src_a, run_length);
                    if (run_length == 2) {
                        if (src_a == 0u) {
                            col += 2;
                            continue;
                        }
                        if (src_a == 0xFFu) {
                            dst_row[col] = src_pixel;
                            dst_row[col + 1] = src_pixel;
                            col += 2;
                            continue;
                        }
                        dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
                        dst_row[col + 1] = blend_argb8888(dst_row[col + 1], src_pixel);
                        col += 2;
                        continue;
                    }

                    if (src_a == 0u) {
                        col += 1;
                        continue;
                    }
                    if (src_a == 0xFFu) {
                        dst_row[col] = src_pixel;
                        col += 1;
                        continue;
                    }
                    dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
                    col += 1;
                }
                finish_non_integer_subrect_alpha_row(&row_telemetry, out_telemetry);
                continue;
            }

            if (!has_same_source_pairs) {
                for (int col = 0; col < visible_w; col++) {
                    const Uint32 src_pixel = src_row[src_x_lookup[col]];
                    const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                    if (src_a == 0u) {
                        continue;
                    }
                    if (src_a == 0xFFu) {
                        dst_row[col] = src_pixel;
                        continue;
                    }
                    dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
                }
                continue;
            }

            for (int col = 0; col < visible_w;) {
                const Uint32 src_pixel = src_row[src_x_lookup[col]];
                const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                if (((col + 1) < visible_w) && same_source_pair_lookup[col]) {
                    if (src_a == 0u) {
                        col += 2;
                        continue;
                    }
                    if (src_a == 0xFFu) {
                        dst_row[col] = src_pixel;
                        dst_row[col + 1] = src_pixel;
                        col += 2;
                        continue;
                    }
                    dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
                    dst_row[col + 1] = blend_argb8888(dst_row[col + 1], src_pixel);
                    col += 2;
                    continue;
                }

                if (src_a == 0u) {
                    col += 1;
                    continue;
                }
                if (src_a == 0xFFu) {
                    dst_row[col] = src_pixel;
                    col += 1;
                    continue;
                }
                dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
                col += 1;
            }
            continue;
        }

        if (collect_in_band_alpha_telemetry) {
            NonIntegerSubrectAlphaRowTelemetry row_telemetry;
            SDL_zero(row_telemetry);
            row_telemetry.current_class = NON_INTEGER_SOURCE_ALPHA_CLASS_NONE;
            for (int col = 0; col < visible_w; col++) {
                Uint32 src_pixel = modulate_argb8888(src_row[src_x_lookup[col]], color);
                const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                note_non_integer_subrect_alpha_run(&row_telemetry, src_a, 1);
                if (src_a == 0u) {
                    continue;
                }
                if (src_a == 0xFFu) {
                    dst_row[col] = src_pixel;
                    continue;
                }
                dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
            }
            finish_non_integer_subrect_alpha_row(&row_telemetry, out_telemetry);
            continue;
        }

        for (int col = 0; col < visible_w; col++) {
            Uint32 src_pixel = modulate_argb8888(src_row[src_x_lookup[col]], color);
            const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
            if (src_a == 0u) {
                continue;
            }
            if (src_a == 0xFFu) {
                dst_row[col] = src_pixel;
                continue;
            }
            dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
        }
    }

    if (sample_phase_timing && (out_telemetry != NULL)) {
        const Uint64 row_phase_end_counter = SDL_GetPerformanceCounter();
        const Uint64 row_phase_total_ns =
            performance_counter_delta_to_ns(row_phase_start_counter, row_phase_end_counter, perf_frequency);
        out_telemetry->sampled_row_raster_ns =
            row_phase_total_ns > out_telemetry->sampled_reuse_telemetry_ns
                ? row_phase_total_ns - out_telemetry->sampled_reuse_telemetry_ns
                : 0u;
    }

    return true;
}
