#include "port/sdl/sdl_game_renderer.h"
#include "common.h"
#include "port/sdl/software_frame_non_integer.h"
#include "port/sdl/sdl_message_renderer.h"
#include "sf33rd/AcrSDK/ps2/flps2etc.h"
#include "sf33rd/AcrSDK/ps2/flps2render.h"
#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "sf33rd/Source/Game/system/work_sys.h"

#include <libgraph.h>

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RENDER_TASK_MAX 1024
#define RENDER_TASK_VERTEX_MAX (RENDER_TASK_MAX * 4)
#define RENDER_TASK_INDEX_MAX (RENDER_TASK_MAX * 6)

#if ENABLE_PERF_TELEMETRY
#define RENDERER_TELEMETRY(stmt) \
    do {                         \
        stmt;                    \
    } while (0)
#else
#define RENDERER_TELEMETRY(stmt) \
    do {                         \
    } while (0)
#endif

typedef enum RenderTaskType {
    RENDER_TASK_TYPE_GEOMETRY = 0,
    RENDER_TASK_TYPE_TEXTURED_RECT = 1,
} RenderTaskType;

typedef struct RenderTask {
    SDL_Texture* texture;
    RenderTaskType type;
#if ENABLE_PERF_TELEMETRY
    SDLGameRenderer_TaskSource source;
#endif
    unsigned int texture_binding;
    SDL_Surface* software_source_surface;
    SDL_Vertex vertices[4];
    SDL_FRect dst_rect;
    SDL_FRect src_uv_rect;
    SDL_FlipMode flip;
    Uint32 color;
    float z;
    int index;
} RenderTask;

typedef struct RectRunTelemetryState {
    SDL_Texture* texture;
    RenderTask prev_task;
    bool prev_task_valid;
    int run_length;
    int hstrip_length;
    int vstrip_length;
} RectRunTelemetryState;

SDL_Texture* cps3_canvas = NULL;

static const int cps3_width = 384;
static const int cps3_height = 224;
static const Uint64 software_frame_non_integer_lookup_threshold_pixels = 1024u;
enum {
    dirty_tile_size = 16,
    dirty_tile_cols = 24,
    dirty_tile_rows = 14,
    dirty_tile_total = dirty_tile_cols * dirty_tile_rows,
};

static SDL_Renderer* _renderer = NULL;
static SDL_Surface* software_frame_surface = NULL;
static SDL_Texture* software_frame_upload_texture = NULL;
static bool software_frame_mode_active = false;
static bool software_frame_direct_present_requested = false;
static bool software_frame_surface_ready = false;
static bool software_frame_owned = false;
static bool software_frame_uploaded = false;
static SDL_Surface* surfaces[FL_TEXTURE_MAX] = { NULL };
static SDL_Palette* palettes[FL_PALETTE_MAX] = { NULL };
typedef enum CacheDirtyReason {
    CACHE_DIRTY_REASON_NONE = 0,
    CACHE_DIRTY_REASON_TEXTURE_UNLOCK,
    CACHE_DIRTY_REASON_PALETTE_UNLOCK,
} CacheDirtyReason;

typedef struct CacheDirtyState {
    Uint32 generation;
    Uint8 reason;
} CacheDirtyState;

static SDL_Surface* software_surface_cache[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { NULL } };
static CacheDirtyState texture_cache_dirty_state[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { { 0 } } };
static CacheDirtyState software_surface_cache_dirty_state[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { { 0 } } };
static Uint8 texture_cache_runtime_dirty_reason[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { 0 } };
static Uint8 software_surface_cache_runtime_dirty_reason[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { 0 } };
static bool texture_cache_refresh_pending[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { false } };
static Uint16 software_surface_refresh_binding_generation[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { 0 } };
static Uint16 software_surface_refresh_texture_generation[FL_TEXTURE_MAX] = { 0 };
static Uint16 software_surface_refresh_texture_variant_counts[FL_TEXTURE_MAX] = { 0 };
static SDLGameRenderer_PerfCaptureRefreshTelemetry perf_capture_refresh_telemetry = { 0 };
static Uint64 perf_capture_refresh_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint16 perf_capture_refresh_fanout_max_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint32 perf_capture_refresh_source_format_by_texture[FL_TEXTURE_MAX] = { 0 };
static int perf_capture_refresh_width_by_texture[FL_TEXTURE_MAX] = { 0 };
static int perf_capture_refresh_height_by_texture[FL_TEXTURE_MAX] = { 0 };
static bool perf_capture_refresh_shape_mixed_by_texture[FL_TEXTURE_MAX] = { false };
static SDLGameRenderer_PerfCaptureUnlockLocalityTelemetry perf_capture_unlock_locality_telemetry = { 0 };
static Uint64 perf_capture_unlock_locality_tracked_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_baseline_skips_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_non_index8_skips_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_source_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_changed_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_changed_rows_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_changed_bbox_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint32 perf_capture_unlock_locality_source_format_by_texture[FL_TEXTURE_MAX] = { 0 };
static int perf_capture_unlock_locality_width_by_texture[FL_TEXTURE_MAX] = { 0 };
static int perf_capture_unlock_locality_height_by_texture[FL_TEXTURE_MAX] = { 0 };
static bool perf_capture_unlock_locality_shape_mixed_by_texture[FL_TEXTURE_MAX] = { false };
static Uint8* perf_capture_unlock_locality_shadow_pixels[FL_TEXTURE_MAX] = { NULL };
static size_t perf_capture_unlock_locality_shadow_size[FL_TEXTURE_MAX] = { 0 };
static bool perf_capture_unlock_locality_shadow_valid[FL_TEXTURE_MAX] = { false };
static SDL_Rect texture_unlock_dirty_rects[FL_TEXTURE_MAX] = { { 0 } };
static bool texture_unlock_dirty_rect_valid[FL_TEXTURE_MAX] = { false };
static Uint16 software_surface_refresh_tracking_generation = 1;
static Uint32 cache_dirty_generation = 1;
static bool cache_dirty_tracking_active = false;
static SDL_Surface* software_surfaces_to_destroy[1024] = { NULL };
static int software_surfaces_to_destroy_count = 0;
static SDL_Texture* current_texture = NULL;
static SDL_Surface* current_software_source_surface = NULL;
static unsigned int current_texture_binding = 0;
static bool current_texture_binding_valid = false;
static bool cps3_target_bound = false;
static SDL_Texture* texture_cache[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { NULL } };
static SDL_Texture* textures_to_destroy[1024] = { NULL };
static int textures_to_destroy_count = 0;
static RenderTask render_tasks[RENDER_TASK_MAX] = { 0 };
static RenderTask software_frame_resolved_tasks[RENDER_TASK_MAX] = { 0 };
static int render_task_count = 0;
static bool render_tasks_have_z_inversion = false;
static int render_tasks_z_inversion_count = 0;
static const int render_task_indices[6] = { 0, 1, 2, 1, 2, 3 };
static SDL_Vertex render_task_batch_vertices[RENDER_TASK_VERTEX_MAX] = { 0 };
static int render_task_batch_indices[RENDER_TASK_INDEX_MAX] = { 0 };
static bool render_task_batch_indices_initialized = false;
static float rgba8_to_float[256] = { 0.0f };
static Uint32 rgba32_fcolor_cache_pixel = 0;
static SDL_FColor rgba32_fcolor_cache_value = { 0 };
static bool rgba32_fcolor_cache_valid = false;
static const int render_task_insertion_sort_max_inversions = 8;
static const int render_task_insertion_sort_max_tasks = 128;
static Uint8 dirty_tile_map[dirty_tile_total] = { 0 };
static Uint8 current_coverage_tile_map[dirty_tile_total] = { 0 };
static Uint8 previous_coverage_tile_map[dirty_tile_total] = { 0 };
static int dirty_tile_count = dirty_tile_total;
static int current_coverage_tile_count = 0;
static int previous_coverage_tile_count = 0;
static Uint32 previous_frame_clear_color = 0;
static bool previous_frame_clear_color_valid = false;
static SDLGameRenderer_FrameStats frame_stats = { 0 };
static SDL_Texture* submitted_texture_mod = NULL;
static Uint32 submitted_texture_mod_color = 0;
static bool submitted_texture_mod_valid = false;
#if ENABLE_PERF_TELEMETRY
static bool frame_stats_extended_enabled = false;
#else
static const bool frame_stats_extended_enabled = false;
#endif
static const float rect_task_epsilon = 0.001f;
#if ENABLE_PERF_TELEMETRY
static SDLGameRenderer_TaskSource current_task_source = SDL_GAME_RENDERER_TASK_SOURCE_UNKNOWN;
#endif

typedef enum HybridFallbackReason {
    HYBRID_FALLBACK_REASON_NONE = 0,
    HYBRID_FALLBACK_REASON_CLIP,
    HYBRID_FALLBACK_REASON_ALPHA,
    HYBRID_FALLBACK_REASON_COLOR_MOD,
    HYBRID_FALLBACK_REASON_FLIP,
    HYBRID_FALLBACK_REASON_GEOMETRY,
    HYBRID_FALLBACK_REASON_SOLID,
} HybridFallbackReason;

typedef enum SoftwareFrameFallbackReason {
    SOFTWARE_FRAME_FALLBACK_REASON_NONE = 0,
    SOFTWARE_FRAME_FALLBACK_REASON_ALPHA,
    SOFTWARE_FRAME_FALLBACK_REASON_COLOR_MOD,
    SOFTWARE_FRAME_FALLBACK_REASON_GEOMETRY,
    SOFTWARE_FRAME_FALLBACK_REASON_SOLID,
} SoftwareFrameFallbackReason;

typedef enum SoftwareFrameFastCopyResult {
    SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT = 0,
    SOFTWARE_FRAME_FAST_COPY_RESULT_SCALED,
    SOFTWARE_FRAME_FAST_COPY_RESULT_COLOR_MOD,
    SOFTWARE_FRAME_FAST_COPY_RESULT_NON_INTEGER,
    SOFTWARE_FRAME_FAST_COPY_RESULT_UNSUPPORTED_FLIP,
    SOFTWARE_FRAME_FAST_COPY_RESULT_SOURCE_BOUNDS,
} SoftwareFrameFastCopyResult;

typedef struct SoftwareFrameFastCopyPlan {
    int dst_x;
    int dst_y;
    int dst_w;
    int dst_h;
    int dst_x0;
    int dst_y0;
    int visible_w;
    int visible_h;
    int src_x;
    int src_y;
    int src_w;
    int src_h;
    bool color_mod;
    bool flip_h;
    bool flip_v;
    bool clipped;
    bool flipped;
} SoftwareFrameFastCopyPlan;

static int compare_render_tasks(const RenderTask* a, const RenderTask* b);
static void insertion_sort_render_tasks(void);
static void initialize_render_task_batch_indices(void);
static void submit_render_tasks(void);
static bool submit_rect_task(const RenderTask* task);
static int clamp_to_range(int value, int min_value, int max_value);
static bool try_submit_geometry_task_as_rect_copy(const RenderTask* task,
                                                  bool* out_used_rect_path,
                                                  RenderTask* out_submitted_rect_task);
static void flush_rect_texture_run_stats(RectRunTelemetryState* state);
#if ENABLE_PERF_TELEMETRY
static bool can_merge_rect_tasks_horizontally(const RenderTask* prev, const RenderTask* next);
static bool can_merge_rect_tasks_vertically(const RenderTask* prev, const RenderTask* next);
#endif
static void mark_dirty_tiles_for_task(const RenderTask* task);
static void clear_cache_dirty_state(CacheDirtyState* state);
#if ENABLE_PERF_TELEMETRY
static void reset_cache_dirty_tracking(void);
static void begin_cache_dirty_tracking_frame(bool capture_extended_stats);
static void note_texture_cache_miss_provenance(const CacheDirtyState* state);
static void note_software_surface_cache_create_provenance(const CacheDirtyState* state);
#endif
static void mark_cache_dirty_state(CacheDirtyState* state, CacheDirtyReason reason);
static void note_software_surface_dirty_variant_fanout(CacheDirtyReason reason, int dirty_variant_count);
static bool should_keep_dirty_cache_entries(CacheDirtyReason reason);
static bool should_refresh_dirty_cache_entry(Uint8 dirty_reason);
#if ENABLE_PERF_TELEMETRY
static void begin_software_surface_refresh_tracking_frame(bool capture_extended_stats);
#endif
static void note_software_surface_refresh_attempt(unsigned int th);
static void note_perf_capture_refresh_attempt(unsigned int th, const SDL_Surface* source_surface);
static void reset_perf_capture_unlock_locality_shadow_slot(int texture_index);
static void reset_perf_capture_unlock_locality_texture_slot(int texture_index);
static void note_perf_capture_texture_unlock_locality(int texture_handle, const SDL_Surface* source_surface);
static void clear_texture_unlock_dirty_rect_index(int texture_index);
static void clear_texture_unlock_dirty_rect_if_unused(int texture_index);
static bool get_texture_unlock_partial_refresh_rect(unsigned int th,
                                                    Uint8 dirty_reason,
                                                    const SDL_Surface* source_surface,
                                                    SDL_Rect* out_rect);
static bool refresh_software_source_surface_in_place(unsigned int th, SDL_Surface* cached_surface, Uint8 dirty_reason);
static bool refresh_texture_in_place(unsigned int th,
                                     SDL_Texture* texture,
                                     SDL_Surface** inout_software_source_surface);
static int invalidate_texture_cache_for_texture_index(int texture_index, CacheDirtyReason reason);
static int invalidate_texture_cache_for_palette_handle(int palette_handle, CacheDirtyReason reason);
static int invalidate_software_surface_cache_for_texture_index(int texture_index, CacheDirtyReason reason);
static int invalidate_software_surface_cache_for_palette_handle(int palette_handle, CacheDirtyReason reason);
static void fill_palette_colors_from_fl_texture(const FLTexture* fl_palette, SDL_Color* colors, int* out_color_count);
static void read_rgba32_color(Uint32 pixel, SDL_Color* color);
static void read_rgba32_fcolor(Uint32 pixel, SDL_FColor* fcolor);
static bool nearly_equal(float a, float b);
#if ENABLE_PERF_TELEMETRY
static void count_task_source(SDLGameRenderer_TaskSource source);
#endif
static Uint64 render_task_submitted_pixels(const RenderTask* task);
#if ENABLE_PERF_TELEMETRY
static bool rect_task_fits_native_canvas(const RenderTask* task);
static HybridFallbackReason classify_hybrid_fallback_reason(const RenderTask* task);
#endif
static void note_hybrid_eligibility(const RenderTask* task);
static bool try_resolve_geometry_task_as_rect_copy(const RenderTask* task, RenderTask* out_rect_task);
static bool try_resolve_solid_task_as_rect(const RenderTask* task, SDL_FRect* out_rect, Uint32* out_color);
static SoftwareFrameFallbackReason classify_software_frame_fallback_reason(const RenderTask* task);
static void note_software_frame_eligibility(const RenderTask* task, SoftwareFrameFallbackReason reason);
static SoftwareFrameFastCopyResult build_software_frame_fast_copy_plan(const RenderTask* task,
                                                                       const SDL_Surface* dst_surface,
                                                                       const SDL_Surface* src_surface,
                                                                       SoftwareFrameFastCopyPlan* out_plan);
static void note_software_frame_fast_copy_result(const RenderTask* task,
                                                 SoftwareFrameFastCopyResult result,
                                                 const SoftwareFrameFastCopyPlan* plan);
static void note_software_frame_fast_non_integer(const RenderTask* task);
static Uint32 modulate_argb8888(Uint32 pixel, Uint32 color);
static Uint32 blend_argb8888(Uint32 dst_pixel, Uint32 src_pixel);
static SDL_Surface* get_or_create_software_source_surface(unsigned int th);
static bool ensure_software_frame_upload_texture(void);
static bool raster_textured_task_to_software_frame(const RenderTask* task);
static bool raster_solid_task_to_software_frame(const RenderTask* task);
static bool render_frame_to_software_surface(void);
static bool upload_software_frame_to_canvas(void);
static bool ensure_software_frame_canvas_for_frame(void);
static bool try_fast_copy_fast_textured_task_to_software_frame(const RenderTask* task,
                                                               const SoftwareFrameFastCopyPlan* plan,
                                                               SDL_Surface* dst_surface,
                                                               SDL_Surface* src_surface);
static bool try_setup_textured_rect_task(RenderTask* task,
                                         float x0,
                                         float y0,
                                         float x1,
                                         float y1,
                                         float s0,
                                         float t0,
                                         float s1,
                                         float t1,
                                         Uint32 color);
static bool ensure_software_frame_surface(void);

// Debugging

static bool draw_rect_borders = false;
static bool dump_textures = false;

static int texture_index = 0;

static void save_texture(const SDL_Surface* surface, const SDL_Palette* palette) {
    char filename[128];
    sprintf(filename, "textures/%d.tga", texture_index);

    const Uint8* pixels = surface->pixels;
    const int width = surface->w;
    const int height = surface->h;

    FILE* f = fopen(filename, "wb");

    if (!f) {
        return;
    }

    uint8_t header[18] = { 0 };
    header[2] = 2; // uncompressed RGB
    header[12] = width & 0xFF;
    header[13] = (width >> 8) & 0xFF;
    header[14] = height & 0xFF;
    header[15] = (height >> 8) & 0xFF;
    header[16] = 32;   // bits per pixel
    header[17] = 0x20; // top-left origin

    fwrite(header, 1, 18, f);

    // Write pixels in BGRA format
    for (int i = 0; i < width * height; ++i) {
        Uint8 index = pixels[i];

        switch (palette->ncolors) {
        case 16:
            if (i & 1) {
                index >>= 4;
            } else {
                index &= 0xF;
            }

            break;

        case 256:
            break;
        }

        const SDL_Color* color = &palette->colors[index];
        const Uint8 bgr[] = { color->b, color->g, color->r, color->a };
        fwrite(bgr, 1, 4, f);
    }

    fclose(f);
    texture_index += 1;
}

// Textures

static void push_texture(SDL_Texture* texture) {
    current_texture = texture;
}

static SDL_Texture* get_texture() {
    if (current_texture == NULL) {
        fatal_error("No textures to get");
    }

    return current_texture;
}

static void push_texture_to_destroy(SDL_Texture* texture) {
    textures_to_destroy[textures_to_destroy_count] = texture;
    textures_to_destroy_count += 1;
    if (frame_stats_extended_enabled) {
        frame_stats.textures_destroy_queued += 1;
    }
}

static void push_software_surface_to_destroy(SDL_Surface* surface) {
    if (surface == NULL) {
        return;
    }

    software_surfaces_to_destroy[software_surfaces_to_destroy_count] = surface;
    software_surfaces_to_destroy_count += 1;
}

static int invalidate_texture_cache_for_texture_index(int texture_index, CacheDirtyReason reason) {
    const int texture_handle = texture_index + 1;
    const bool keep_dirty_entries = should_keep_dirty_cache_entries(reason);
    int evicted_count = 0;

    if (current_texture_binding_valid && (LO_16_BITS(current_texture_binding) == (unsigned int)texture_handle)) {
        current_texture = NULL;
        current_software_source_surface = NULL;
        current_texture_binding_valid = false;
    }

    for (int i = 0; i < FL_PALETTE_MAX + 1; i++) {
        SDL_Texture** texture_p = &texture_cache[texture_index][i];
        CacheDirtyState* dirty_state = &texture_cache_dirty_state[texture_index][i];
        Uint8* runtime_dirty_reason_p = &texture_cache_runtime_dirty_reason[texture_index][i];

        if (*texture_p == NULL) {
            clear_cache_dirty_state(dirty_state);
            *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
            texture_cache_refresh_pending[texture_index][i] = false;
            continue;
        }

        if (keep_dirty_entries) {
            mark_cache_dirty_state(dirty_state, reason);
            *runtime_dirty_reason_p = (Uint8)reason;
            continue;
        }

        push_texture_to_destroy(*texture_p);
        *texture_p = NULL;
        mark_cache_dirty_state(dirty_state, reason);
        *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
        texture_cache_refresh_pending[texture_index][i] = false;
        evicted_count += 1;
    }

    return evicted_count;
}

static int invalidate_software_surface_cache_for_texture_index(int texture_index, CacheDirtyReason reason) {
    const int texture_handle = texture_index + 1;
    const bool keep_dirty_entries = should_keep_dirty_cache_entries(reason);
    int evicted_count = 0;
    int dirty_variant_count = 0;

    if (current_texture_binding_valid && (LO_16_BITS(current_texture_binding) == (unsigned int)texture_handle)) {
        current_software_source_surface = NULL;
    }

    for (int i = 0; i < FL_PALETTE_MAX + 1; i++) {
        SDL_Surface** surface_p = &software_surface_cache[texture_index][i];
        CacheDirtyState* dirty_state = &software_surface_cache_dirty_state[texture_index][i];
        Uint8* runtime_dirty_reason_p = &software_surface_cache_runtime_dirty_reason[texture_index][i];
        if (*surface_p == NULL) {
            clear_cache_dirty_state(dirty_state);
            *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
            continue;
        }

        if (keep_dirty_entries) {
            mark_cache_dirty_state(dirty_state, reason);
            *runtime_dirty_reason_p = (Uint8)reason;
            dirty_variant_count += 1;
            continue;
        }

        push_software_surface_to_destroy(*surface_p);
        *surface_p = NULL;
        mark_cache_dirty_state(dirty_state, reason);
        *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
        evicted_count += 1;
    }

    note_software_surface_dirty_variant_fanout(reason, dirty_variant_count);
    return evicted_count;
}

static int invalidate_texture_cache_for_palette_handle(int palette_handle, CacheDirtyReason reason) {
    const bool keep_dirty_entries = should_keep_dirty_cache_entries(reason);
    int evicted_count = 0;

    if (current_texture_binding_valid && (HI_16_BITS(current_texture_binding) == (unsigned int)palette_handle)) {
        current_texture = NULL;
        current_software_source_surface = NULL;
        current_texture_binding_valid = false;
    }

    for (int i = 0; i < FL_TEXTURE_MAX; i++) {
        SDL_Texture** texture_p = &texture_cache[i][palette_handle];
        CacheDirtyState* dirty_state = &texture_cache_dirty_state[i][palette_handle];
        Uint8* runtime_dirty_reason_p = &texture_cache_runtime_dirty_reason[i][palette_handle];

        if (*texture_p == NULL) {
            clear_cache_dirty_state(dirty_state);
            *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
            texture_cache_refresh_pending[i][palette_handle] = false;
            continue;
        }

        if (keep_dirty_entries) {
            mark_cache_dirty_state(dirty_state, reason);
            *runtime_dirty_reason_p = (Uint8)reason;
            continue;
        }

        push_texture_to_destroy(*texture_p);
        *texture_p = NULL;
        mark_cache_dirty_state(dirty_state, reason);
        *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
        texture_cache_refresh_pending[i][palette_handle] = false;
        evicted_count += 1;
    }

    return evicted_count;
}

static int invalidate_software_surface_cache_for_palette_handle(int palette_handle, CacheDirtyReason reason) {
    const bool keep_dirty_entries = should_keep_dirty_cache_entries(reason);
    int evicted_count = 0;
    int dirty_variant_count = 0;

    if (current_texture_binding_valid && (HI_16_BITS(current_texture_binding) == (unsigned int)palette_handle)) {
        current_software_source_surface = NULL;
    }

    for (int i = 0; i < FL_TEXTURE_MAX; i++) {
        SDL_Surface** surface_p = &software_surface_cache[i][palette_handle];
        CacheDirtyState* dirty_state = &software_surface_cache_dirty_state[i][palette_handle];
        Uint8* runtime_dirty_reason_p = &software_surface_cache_runtime_dirty_reason[i][palette_handle];
        if (*surface_p == NULL) {
            clear_cache_dirty_state(dirty_state);
            *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
            continue;
        }

        if (keep_dirty_entries) {
            mark_cache_dirty_state(dirty_state, reason);
            *runtime_dirty_reason_p = (Uint8)reason;
            dirty_variant_count += 1;
            continue;
        }

        push_software_surface_to_destroy(*surface_p);
        *surface_p = NULL;
        mark_cache_dirty_state(dirty_state, reason);
        *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
        evicted_count += 1;
    }

    note_software_surface_dirty_variant_fanout(reason, dirty_variant_count);
    return evicted_count;
}

static void destroy_textures() {
    current_texture = NULL;
    current_software_source_surface = NULL;
    current_texture_binding_valid = false;
    current_texture_binding = 0;

    for (int i = 0; i < textures_to_destroy_count; i++) {
        SDL_DestroyTexture(textures_to_destroy[i]);
    }
    textures_to_destroy_count = 0;

    for (int i = 0; i < software_surfaces_to_destroy_count; i++) {
        SDL_DestroySurface(software_surfaces_to_destroy[i]);
    }
    software_surfaces_to_destroy_count = 0;
}

static void push_render_task(RenderTask* task) {
    if (No_Trans) {
        printf("⚠️ Requesting a render task when no rendering is allowed is a programmer error!\n");
    }

    RenderTask queued_task = *task;
    int insert_index = render_task_count;

    if (render_task_count > 0) {
        const RenderTask* prev_task = &render_tasks[render_task_count - 1];

        if (prev_task->z < queued_task.z) {
            // Strictly increasing Z is already in comparator order.
        } else if (prev_task->z == queued_task.z) {
            // Keep equal-Z tasks in final draw order as they are queued so the render pass can skip a reversal walk.
            insert_index -= 1;
            while ((insert_index > 0) && (render_tasks[insert_index - 1].z == queued_task.z)) {
                insert_index -= 1;
            }
        } else {
            // Preserve previous fallback behavior for descending or non-ordered Z values.
            render_tasks_have_z_inversion = true;
            render_tasks_z_inversion_count += 1;
        }
    }

    if (insert_index != render_task_count) {
        SDL_memmove(&render_tasks[insert_index + 1],
                    &render_tasks[insert_index],
                    (size_t)(render_task_count - insert_index) * sizeof(render_tasks[0]));
    }

    render_tasks[insert_index] = queued_task;
    mark_dirty_tiles_for_task(&render_tasks[insert_index]);
#if ENABLE_PERF_TELEMETRY
    count_task_source(render_tasks[insert_index].source);
#endif
    render_task_count += 1;
}

static void clear_render_tasks() {
    // Render queue consumers iterate up to `render_task_count`; no need to wipe the full backing array every frame.
    render_task_count = 0;
    render_tasks_have_z_inversion = false;
    render_tasks_z_inversion_count = 0;
}

static void clear_cache_dirty_state(CacheDirtyState* state) {
    if (state == NULL) {
        return;
    }

    state->generation = 0;
    state->reason = CACHE_DIRTY_REASON_NONE;
}

#if ENABLE_PERF_TELEMETRY
static void reset_cache_dirty_tracking(void) {
    SDL_zero(texture_cache_dirty_state);
    SDL_zero(software_surface_cache_dirty_state);
    cache_dirty_generation = 1;
}

static void begin_cache_dirty_tracking_frame(bool capture_extended_stats) {
    if (!capture_extended_stats) {
        cache_dirty_tracking_active = false;
        return;
    }

    if (!cache_dirty_tracking_active) {
        reset_cache_dirty_tracking();
        cache_dirty_tracking_active = true;
        return;
    }

    cache_dirty_generation += 1;
    if (cache_dirty_generation == 0) {
        reset_cache_dirty_tracking();
    }
}
#endif

static void mark_cache_dirty_state(CacheDirtyState* state, CacheDirtyReason reason) {
    if (state == NULL) {
        return;
    }

    if (!frame_stats_extended_enabled || !cache_dirty_tracking_active || (reason == CACHE_DIRTY_REASON_NONE)) {
        clear_cache_dirty_state(state);
        return;
    }

    state->generation = cache_dirty_generation;
    state->reason = (Uint8)reason;
}

#if ENABLE_PERF_TELEMETRY
static void note_texture_cache_miss_provenance(const CacheDirtyState* state) {
    if (!frame_stats_extended_enabled || (state == NULL)) {
        return;
    }

    switch ((CacheDirtyReason)state->reason) {
    case CACHE_DIRTY_REASON_TEXTURE_UNLOCK:
        if (state->generation == cache_dirty_generation) {
            frame_stats.texture_cache_miss_dirty_texture_same_frame += 1;
        } else {
            frame_stats.texture_cache_miss_dirty_texture_carried += 1;
        }
        break;
    case CACHE_DIRTY_REASON_PALETTE_UNLOCK:
        if (state->generation == cache_dirty_generation) {
            frame_stats.texture_cache_miss_dirty_palette_same_frame += 1;
        } else {
            frame_stats.texture_cache_miss_dirty_palette_carried += 1;
        }
        break;
    case CACHE_DIRTY_REASON_NONE:
    default:
        frame_stats.texture_cache_miss_cold += 1;
        break;
    }
}

static void note_software_surface_cache_create_provenance(const CacheDirtyState* state) {
    if (!frame_stats_extended_enabled || (state == NULL)) {
        return;
    }

    switch ((CacheDirtyReason)state->reason) {
    case CACHE_DIRTY_REASON_TEXTURE_UNLOCK:
        if (state->generation == cache_dirty_generation) {
            frame_stats.software_surface_cache_create_dirty_texture_same_frame += 1;
        } else {
            frame_stats.software_surface_cache_create_dirty_texture_carried += 1;
        }
        break;
    case CACHE_DIRTY_REASON_PALETTE_UNLOCK:
        if (state->generation == cache_dirty_generation) {
            frame_stats.software_surface_cache_create_dirty_palette_same_frame += 1;
        } else {
            frame_stats.software_surface_cache_create_dirty_palette_carried += 1;
        }
        break;
    case CACHE_DIRTY_REASON_NONE:
    default:
        frame_stats.software_surface_cache_create_cold += 1;
        break;
    }
}
#endif

static void note_software_surface_dirty_variant_fanout(CacheDirtyReason reason, int dirty_variant_count) {
    if (!frame_stats_extended_enabled || (dirty_variant_count <= 0)) {
        return;
    }

    switch (reason) {
    case CACHE_DIRTY_REASON_TEXTURE_UNLOCK:
        frame_stats.texture_unlock_dirty_surface_variants += dirty_variant_count;
        if (dirty_variant_count > frame_stats.texture_unlock_dirty_surface_variants_max) {
            frame_stats.texture_unlock_dirty_surface_variants_max = dirty_variant_count;
        }
        break;
    case CACHE_DIRTY_REASON_PALETTE_UNLOCK:
        frame_stats.palette_unlock_dirty_surface_variants += dirty_variant_count;
        if (dirty_variant_count > frame_stats.palette_unlock_dirty_surface_variants_max) {
            frame_stats.palette_unlock_dirty_surface_variants_max = dirty_variant_count;
        }
        break;
    case CACHE_DIRTY_REASON_NONE:
    default:
        break;
    }
}

#if ENABLE_PERF_TELEMETRY
static void begin_software_surface_refresh_tracking_frame(bool capture_extended_stats) {
    if (!capture_extended_stats) {
        return;
    }

    SDL_zero(software_surface_refresh_texture_variant_counts);
    software_surface_refresh_tracking_generation += 1;
    if (software_surface_refresh_tracking_generation == 0) {
        SDL_zero(software_surface_refresh_binding_generation);
        SDL_zero(software_surface_refresh_texture_generation);
        software_surface_refresh_tracking_generation = 1;
    }
}
#endif

static void note_perf_capture_refresh_attempt(unsigned int th, const SDL_Surface* source_surface) {
    if (!frame_stats_extended_enabled || (source_surface == NULL)) {
        return;
    }

    const int texture_handle = LO_16_BITS(th);
    if ((texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return;
    }

    const int texture_index = texture_handle - 1;
    const Uint64 refresh_pixels = (Uint64)source_surface->w * (Uint64)source_surface->h;
    switch (source_surface->format) {
    case SDL_PIXELFORMAT_INDEX4LSB:
        perf_capture_refresh_telemetry.index4_attempts += 1;
        perf_capture_refresh_telemetry.index4_pixels += refresh_pixels;
        break;
    case SDL_PIXELFORMAT_INDEX8:
        perf_capture_refresh_telemetry.index8_attempts += 1;
        perf_capture_refresh_telemetry.index8_pixels += refresh_pixels;
        break;
    case SDL_PIXELFORMAT_ABGR1555:
        perf_capture_refresh_telemetry.abgr1555_attempts += 1;
        perf_capture_refresh_telemetry.abgr1555_pixels += refresh_pixels;
        break;
    default:
        perf_capture_refresh_telemetry.other_attempts += 1;
        perf_capture_refresh_telemetry.other_pixels += refresh_pixels;
        break;
    }

    perf_capture_refresh_attempts_by_texture[texture_index] += 1;
    perf_capture_refresh_pixels_by_texture[texture_index] += refresh_pixels;
    if (perf_capture_refresh_attempts_by_texture[texture_index] > 1 &&
        ((perf_capture_refresh_source_format_by_texture[texture_index] != source_surface->format) ||
         (perf_capture_refresh_width_by_texture[texture_index] != source_surface->w) ||
         (perf_capture_refresh_height_by_texture[texture_index] != source_surface->h))) {
        perf_capture_refresh_shape_mixed_by_texture[texture_index] = true;
    }
    perf_capture_refresh_source_format_by_texture[texture_index] = source_surface->format;
    perf_capture_refresh_width_by_texture[texture_index] = source_surface->w;
    perf_capture_refresh_height_by_texture[texture_index] = source_surface->h;
}

static void reset_perf_capture_unlock_locality_shadow_slot(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    SDL_free(perf_capture_unlock_locality_shadow_pixels[texture_index]);
    perf_capture_unlock_locality_shadow_pixels[texture_index] = NULL;
    perf_capture_unlock_locality_shadow_size[texture_index] = 0;
    perf_capture_unlock_locality_shadow_valid[texture_index] = false;
}

static void reset_perf_capture_unlock_locality_texture_slot(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    perf_capture_unlock_locality_tracked_by_texture[texture_index] = 0;
    perf_capture_unlock_locality_baseline_skips_by_texture[texture_index] = 0;
    perf_capture_unlock_locality_non_index8_skips_by_texture[texture_index] = 0;
    perf_capture_unlock_locality_source_pixels_by_texture[texture_index] = 0;
    perf_capture_unlock_locality_changed_pixels_by_texture[texture_index] = 0;
    perf_capture_unlock_locality_changed_rows_by_texture[texture_index] = 0;
    perf_capture_unlock_locality_changed_bbox_pixels_by_texture[texture_index] = 0;
    perf_capture_unlock_locality_source_format_by_texture[texture_index] = SDL_PIXELFORMAT_UNKNOWN;
    perf_capture_unlock_locality_width_by_texture[texture_index] = 0;
    perf_capture_unlock_locality_height_by_texture[texture_index] = 0;
    perf_capture_unlock_locality_shape_mixed_by_texture[texture_index] = false;
    reset_perf_capture_unlock_locality_shadow_slot(texture_index);
}

static void note_perf_capture_texture_unlock_locality(int texture_handle, const SDL_Surface* source_surface) {
    if (!frame_stats_extended_enabled || (source_surface == NULL) || (texture_handle <= 0) ||
        (texture_handle > FL_TEXTURE_MAX)) {
        return;
    }

    const int texture_index = texture_handle - 1;
    const bool have_index8_history = (perf_capture_unlock_locality_tracked_by_texture[texture_index] > 0) ||
                                     (perf_capture_unlock_locality_baseline_skips_by_texture[texture_index] > 0);
    if (source_surface->format != SDL_PIXELFORMAT_INDEX8) {
        frame_stats.texture_unlock_locality_index8_non_index8_skips += 1;
        perf_capture_unlock_locality_telemetry.index8_non_index8_skips += 1;
        perf_capture_unlock_locality_non_index8_skips_by_texture[texture_index] += 1;
        if (have_index8_history) {
            perf_capture_unlock_locality_shape_mixed_by_texture[texture_index] = true;
        }
        perf_capture_unlock_locality_shadow_valid[texture_index] = false;
        return;
    }

    const int width = source_surface->w;
    const int height = source_surface->h;
    if ((width <= 0) || (height <= 0)) {
        return;
    }

    const size_t row_bytes = (size_t)width;
    const size_t shadow_size = row_bytes * (size_t)height;
    const bool have_existing_shape = have_index8_history ||
                                     (perf_capture_unlock_locality_non_index8_skips_by_texture[texture_index] > 0);
    if (have_existing_shape &&
        ((perf_capture_unlock_locality_source_format_by_texture[texture_index] != source_surface->format) ||
         (perf_capture_unlock_locality_width_by_texture[texture_index] != width) ||
         (perf_capture_unlock_locality_height_by_texture[texture_index] != height))) {
        perf_capture_unlock_locality_shape_mixed_by_texture[texture_index] = true;
    }
    perf_capture_unlock_locality_source_format_by_texture[texture_index] = source_surface->format;
    perf_capture_unlock_locality_width_by_texture[texture_index] = width;
    perf_capture_unlock_locality_height_by_texture[texture_index] = height;

    if (perf_capture_unlock_locality_shadow_size[texture_index] != shadow_size) {
        Uint8* resized_shadow = (Uint8*)SDL_realloc(perf_capture_unlock_locality_shadow_pixels[texture_index], shadow_size);
        if (resized_shadow == NULL) {
            perf_capture_unlock_locality_shadow_valid[texture_index] = false;
            return;
        }
        perf_capture_unlock_locality_shadow_pixels[texture_index] = resized_shadow;
        perf_capture_unlock_locality_shadow_size[texture_index] = shadow_size;
        perf_capture_unlock_locality_shadow_valid[texture_index] = false;
    }

    Uint8* shadow = perf_capture_unlock_locality_shadow_pixels[texture_index];
    if (shadow == NULL) {
        return;
    }

    if (!perf_capture_unlock_locality_shadow_valid[texture_index]) {
        const Uint8* source_row = (const Uint8*)source_surface->pixels;
        for (int y = 0; y < height; y++) {
            SDL_memcpy(&shadow[(size_t)y * row_bytes], source_row, row_bytes);
            source_row += source_surface->pitch;
        }
        perf_capture_unlock_locality_shadow_valid[texture_index] = true;
        frame_stats.texture_unlock_locality_index8_baseline_skips += 1;
        perf_capture_unlock_locality_telemetry.index8_baseline_skips += 1;
        perf_capture_unlock_locality_baseline_skips_by_texture[texture_index] += 1;
        return;
    }

    Uint64 changed_pixels = 0;
    Uint64 changed_rows = 0;
    int min_x = width;
    int max_x = -1;
    int min_y = height;
    int max_y = -1;
    const Uint8* source_row = (const Uint8*)source_surface->pixels;

    for (int y = 0; y < height; y++) {
        Uint8* shadow_row = &shadow[(size_t)y * row_bytes];
        if (SDL_memcmp(source_row, shadow_row, row_bytes) == 0) {
            source_row += source_surface->pitch;
            continue;
        }

        int row_min_x = width;
        int row_max_x = -1;
        for (int x = 0; x < width; x++) {
            if (source_row[x] == shadow_row[x]) {
                continue;
            }
            shadow_row[x] = source_row[x];
            changed_pixels += 1;
            if (x < row_min_x) {
                row_min_x = x;
            }
            row_max_x = x;
        }

        if (row_max_x >= 0) {
            changed_rows += 1;
            if (row_min_x < min_x) {
                min_x = row_min_x;
            }
            if (row_max_x > max_x) {
                max_x = row_max_x;
            }
            if (y < min_y) {
                min_y = y;
            }
            max_y = y;
        }
        source_row += source_surface->pitch;
    }

    const Uint64 source_pixels = (Uint64)width * (Uint64)height;
    const Uint64 changed_bbox_pixels =
        changed_rows > 0 ? (Uint64)(max_x - min_x + 1) * (Uint64)(max_y - min_y + 1) : 0;
    frame_stats.texture_unlock_locality_index8_tracked += 1;
    frame_stats.texture_unlock_locality_index8_source_pixels += source_pixels;
    frame_stats.texture_unlock_locality_index8_changed_pixels += changed_pixels;
    frame_stats.texture_unlock_locality_index8_changed_rows += changed_rows;
    frame_stats.texture_unlock_locality_index8_changed_bbox_pixels += changed_bbox_pixels;
    perf_capture_unlock_locality_telemetry.index8_tracked_unlocks += 1;
    perf_capture_unlock_locality_telemetry.index8_source_pixels += source_pixels;
    perf_capture_unlock_locality_telemetry.index8_changed_pixels += changed_pixels;
    perf_capture_unlock_locality_telemetry.index8_changed_rows += changed_rows;
    perf_capture_unlock_locality_telemetry.index8_changed_bbox_pixels += changed_bbox_pixels;
    perf_capture_unlock_locality_tracked_by_texture[texture_index] += 1;
    perf_capture_unlock_locality_source_pixels_by_texture[texture_index] += source_pixels;
    perf_capture_unlock_locality_changed_pixels_by_texture[texture_index] += changed_pixels;
    perf_capture_unlock_locality_changed_rows_by_texture[texture_index] += changed_rows;
    perf_capture_unlock_locality_changed_bbox_pixels_by_texture[texture_index] += changed_bbox_pixels;
}

static void note_software_surface_refresh_attempt(unsigned int th) {
    if (!frame_stats_extended_enabled) {
        return;
    }

    const int texture_handle = LO_16_BITS(th);
    const int palette_handle = HI_16_BITS(th);
    if ((texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX) || (palette_handle < 0) ||
        (palette_handle > FL_PALETTE_MAX)) {
        return;
    }

    const int texture_index = texture_handle - 1;
    const Uint16 generation = software_surface_refresh_tracking_generation;
    if (software_surface_refresh_binding_generation[texture_index][palette_handle] == generation) {
        frame_stats.software_surface_cache_refresh_repeat_binding_attempts += 1;
        return;
    }

    software_surface_refresh_binding_generation[texture_index][palette_handle] = generation;
    frame_stats.software_surface_cache_refresh_unique_bindings += 1;
    if (software_surface_refresh_texture_generation[texture_index] != generation) {
        software_surface_refresh_texture_generation[texture_index] = generation;
        software_surface_refresh_texture_variant_counts[texture_index] = 1;
        frame_stats.software_surface_cache_refresh_unique_texture_handles += 1;
    } else {
        software_surface_refresh_texture_variant_counts[texture_index] += 1;
    }

    const int texture_handle_fanout = (int)software_surface_refresh_texture_variant_counts[texture_index];
    if (texture_handle_fanout > frame_stats.software_surface_cache_refresh_texture_handle_fanout_max) {
        frame_stats.software_surface_cache_refresh_texture_handle_fanout_max = texture_handle_fanout;
    }
    if (texture_handle_fanout > (int)perf_capture_refresh_fanout_max_by_texture[texture_index]) {
        perf_capture_refresh_fanout_max_by_texture[texture_index] = (Uint16)texture_handle_fanout;
    }
}

static bool should_keep_dirty_cache_entries(CacheDirtyReason reason) {
    return software_frame_mode_active &&
           ((reason == CACHE_DIRTY_REASON_TEXTURE_UNLOCK) || (reason == CACHE_DIRTY_REASON_PALETTE_UNLOCK));
}

static bool should_refresh_dirty_cache_entry(Uint8 dirty_reason) {
    return should_keep_dirty_cache_entries((CacheDirtyReason)dirty_reason);
}

static void clear_texture_unlock_dirty_rect_index(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    texture_unlock_dirty_rects[texture_index] = (SDL_Rect){ 0, 0, 0, 0 };
    texture_unlock_dirty_rect_valid[texture_index] = false;
}

static void clear_texture_unlock_dirty_rect_if_unused(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX) || !texture_unlock_dirty_rect_valid[texture_index]) {
        return;
    }

    for (int palette_handle = 0; palette_handle < FL_PALETTE_MAX + 1; palette_handle++) {
        if ((software_surface_cache[texture_index][palette_handle] != NULL) &&
            (software_surface_cache_runtime_dirty_reason[texture_index][palette_handle] ==
             CACHE_DIRTY_REASON_TEXTURE_UNLOCK)) {
            return;
        }
    }

    clear_texture_unlock_dirty_rect_index(texture_index);
}

static bool get_texture_unlock_partial_refresh_rect(unsigned int th,
                                                    Uint8 dirty_reason,
                                                    const SDL_Surface* source_surface,
                                                    SDL_Rect* out_rect) {
    if ((dirty_reason != CACHE_DIRTY_REASON_TEXTURE_UNLOCK) || (source_surface == NULL) || (out_rect == NULL)) {
        return false;
    }

    const int texture_handle = LO_16_BITS(th);
    if ((texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return false;
    }

    const int texture_index = texture_handle - 1;
    if (!texture_unlock_dirty_rect_valid[texture_index]) {
        return false;
    }

    if ((source_surface->format != SDL_PIXELFORMAT_INDEX8) || (source_surface->w != 256) || (source_surface->h != 256)) {
        return false;
    }

    const SDL_Rect dirty_rect = texture_unlock_dirty_rects[texture_index];
    if ((dirty_rect.w <= 0) || (dirty_rect.h <= 0) || (dirty_rect.x < 0) || (dirty_rect.y < 0) ||
        (dirty_rect.x + dirty_rect.w > source_surface->w) || (dirty_rect.y + dirty_rect.h > source_surface->h)) {
        return false;
    }

    const Uint64 dirty_pixels = (Uint64)dirty_rect.w * (Uint64)dirty_rect.h;
    const Uint64 max_partial_pixels = ((Uint64)source_surface->w * (Uint64)source_surface->h) / 4u;
    if ((dirty_pixels == 0) || (dirty_pixels > max_partial_pixels)) {
        return false;
    }

    *out_rect = dirty_rect;
    return true;
}

static bool refresh_software_source_surface_in_place(unsigned int th, SDL_Surface* cached_surface, Uint8 dirty_reason) {
    const int texture_handle = LO_16_BITS(th);
    const int palette_handle = HI_16_BITS(th);
    const Uint64 refresh_start_ns = frame_stats_extended_enabled ? SDL_GetTicksNS() : 0;
    bool success = false;

    note_software_surface_refresh_attempt(th);

    if ((cached_surface == NULL) || (texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        goto done;
    }

    SDL_Surface* surface = surfaces[texture_handle - 1];
    if (surface == NULL) {
        goto done;
    }

    if ((cached_surface->w != surface->w) || (cached_surface->h != surface->h) ||
        (cached_surface->format != SDL_PIXELFORMAT_ARGB8888)) {
        goto done;
    }

    note_perf_capture_refresh_attempt(th, surface);

    SDL_Palette* palette = palette_handle != 0 ? palettes[palette_handle - 1] : NULL;
    if (palette != NULL) {
        if (frame_stats_extended_enabled) {
            const Uint64 palette_set_start_ns = SDL_GetTicksNS();
            frame_stats.software_surface_cache_refresh_palette_set_calls += 1;
            if (!SDL_SetSurfacePalette(surface, palette)) {
                frame_stats.software_surface_cache_refresh_palette_set_ns += SDL_GetTicksNS() - palette_set_start_ns;
                goto done;
            }
            frame_stats.software_surface_cache_refresh_palette_set_ns += SDL_GetTicksNS() - palette_set_start_ns;
        } else if (!SDL_SetSurfacePalette(surface, palette)) {
            goto done;
        }
    }

    SDL_Rect partial_rect = { 0, 0, 0, 0 };
    const bool use_partial_refresh = get_texture_unlock_partial_refresh_rect(th, dirty_reason, surface, &partial_rect);

    if (frame_stats_extended_enabled) {
        const Uint64 blit_start_ns = SDL_GetTicksNS();
        frame_stats.software_surface_cache_refresh_blit_calls += 1;
        success = use_partial_refresh ? SDL_BlitSurface(surface, &partial_rect, cached_surface, &partial_rect)
                                      : SDL_BlitSurface(surface, NULL, cached_surface, NULL);
        frame_stats.software_surface_cache_refresh_blit_ns += SDL_GetTicksNS() - blit_start_ns;
    } else {
        success = use_partial_refresh ? SDL_BlitSurface(surface, &partial_rect, cached_surface, &partial_rect)
                                      : SDL_BlitSurface(surface, NULL, cached_surface, NULL);
    }

done:
    if (frame_stats_extended_enabled) {
        frame_stats.software_surface_cache_refresh_attempts += 1;
        frame_stats.software_surface_cache_refresh_ns += SDL_GetTicksNS() - refresh_start_ns;
        if (!success) {
            frame_stats.software_surface_cache_refresh_failures += 1;
        }
    }

    return success;
}

static bool refresh_texture_in_place(unsigned int th,
                                     SDL_Texture* texture,
                                     SDL_Surface** inout_software_source_surface) {
    const int texture_handle = LO_16_BITS(th);
    const int palette_handle = HI_16_BITS(th);
    const void* pixels = NULL;
    int pitch = 0;

    if ((texture == NULL) || (texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return false;
    }

    SDL_Surface* source_surface = surfaces[texture_handle - 1];
    if (source_surface == NULL) {
        return false;
    }

    SDL_PropertiesID texture_props = SDL_GetTextureProperties(texture);
    if (texture_props == 0) {
        return false;
    }

    const SDL_PixelFormat texture_format =
        (SDL_PixelFormat)SDL_GetNumberProperty(texture_props, SDL_PROP_TEXTURE_FORMAT_NUMBER, SDL_PIXELFORMAT_UNKNOWN);
    if (texture_format == SDL_PIXELFORMAT_UNKNOWN) {
        return false;
    }

    if (texture_format == source_surface->format) {
        SDL_Palette* palette = palette_handle != 0 ? palettes[palette_handle - 1] : NULL;
        if ((palette != NULL) && !SDL_SetSurfacePalette(source_surface, palette)) {
            return false;
        }
        pixels = source_surface->pixels;
        pitch = source_surface->pitch;
    } else {
        SDL_Surface* software_source_surface =
            inout_software_source_surface != NULL ? *inout_software_source_surface : NULL;
        if (software_source_surface == NULL) {
            software_source_surface = get_or_create_software_source_surface(th);
            if (inout_software_source_surface != NULL) {
                *inout_software_source_surface = software_source_surface;
            }
        }
        if ((software_source_surface == NULL) || (texture_format != software_source_surface->format)) {
            return false;
        }
        pixels = software_source_surface->pixels;
        pitch = software_source_surface->pitch;
    }

    return SDL_UpdateTexture(texture, NULL, pixels, pitch);
}

static void reset_frame_stats(void) {
#if ENABLE_PERF_TELEMETRY
    SDL_zero(frame_stats);
    frame_stats.sort_strategy = SDL_GAME_RENDERER_SORT_NONE;
#endif
    submitted_texture_mod = NULL;
    submitted_texture_mod_color = 0;
    submitted_texture_mod_valid = false;
}

static bool ensure_software_frame_surface(void) {
    if (software_frame_surface != NULL) {
        return true;
    }

    software_frame_surface = SDL_CreateSurface(cps3_width, cps3_height, SDL_PIXELFORMAT_ARGB8888);
    return software_frame_surface != NULL;
}

static bool ensure_software_frame_upload_texture(void) {
    if (software_frame_upload_texture != NULL) {
        return true;
    }

    software_frame_upload_texture = SDL_CreateTexture(
        _renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, cps3_width, cps3_height);
    if (software_frame_upload_texture == NULL) {
        return false;
    }

    SDL_SetTextureScaleMode(software_frame_upload_texture, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(software_frame_upload_texture, SDL_BLENDMODE_NONE);
    return true;
}

static SDL_Texture* get_submit_texture_for_task(const RenderTask* task) {
    if (task == NULL) {
        return NULL;
    }

    const unsigned int th = task->texture_binding;
    if (th == 0) {
        return task->texture;
    }

    const int texture_handle = LO_16_BITS(th);
    const int palette_handle = HI_16_BITS(th);
    if ((texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return task->texture;
    }

    SDL_Texture** texture_p = &texture_cache[texture_handle - 1][palette_handle];
    CacheDirtyState* dirty_state = &texture_cache_dirty_state[texture_handle - 1][palette_handle];
    Uint8* runtime_dirty_reason_p = &texture_cache_runtime_dirty_reason[texture_handle - 1][palette_handle];
    SDL_Texture* texture = *texture_p;

    if ((texture != NULL) && texture_cache_refresh_pending[texture_handle - 1][palette_handle]) {
        SDL_Surface* software_source_surface = task->software_source_surface;
        if (refresh_texture_in_place(th, texture, &software_source_surface)) {
            texture_cache_refresh_pending[texture_handle - 1][palette_handle] = false;
            clear_cache_dirty_state(dirty_state);
            return texture;
        }

        push_texture_to_destroy(texture);
        if (*texture_p == texture) {
            *texture_p = NULL;
        }
        texture = NULL;
        texture_cache_refresh_pending[texture_handle - 1][palette_handle] = false;
    }

    if (texture != NULL) {
        clear_cache_dirty_state(dirty_state);
        return texture;
    }

    SDL_Surface* surface = surfaces[texture_handle - 1];
    if (surface == NULL) {
        return NULL;
    }

    SDL_Palette* palette = palette_handle != 0 ? palettes[palette_handle - 1] : NULL;
    if (palette != NULL) {
        SDL_SetSurfacePalette(surface, palette);
    }

    texture = SDL_CreateTextureFromSurface(_renderer, surface);
    if (texture == NULL) {
        fatal_error("Failed to create SDL texture for handle 0x%08X: %s", th, SDL_GetError());
    }
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    *texture_p = texture;
    *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
    texture_cache_refresh_pending[texture_handle - 1][palette_handle] = false;
    clear_cache_dirty_state(dirty_state);
    if (frame_stats_extended_enabled) {
        RENDERER_TELEMETRY(frame_stats.texture_creates += 1);
    }
    return texture;
}

static SDL_Surface* get_or_create_software_source_surface(unsigned int th) {
    const int texture_handle = LO_16_BITS(th);
    const int palette_handle = HI_16_BITS(th);
    if ((texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return NULL;
    }

    CacheDirtyState* dirty_state = &software_surface_cache_dirty_state[texture_handle - 1][palette_handle];
    Uint8* runtime_dirty_reason_p = &software_surface_cache_runtime_dirty_reason[texture_handle - 1][palette_handle];
    SDL_Surface* cached_surface = software_surface_cache[texture_handle - 1][palette_handle];
    if (cached_surface != NULL) {
        if (should_refresh_dirty_cache_entry(*runtime_dirty_reason_p)) {
            if (refresh_software_source_surface_in_place(th, cached_surface, *runtime_dirty_reason_p)) {
                RENDERER_TELEMETRY({
                    if (frame_stats_extended_enabled) {
                        frame_stats.software_surface_cache_creates += 1;
                        note_software_surface_cache_create_provenance(dirty_state);
                    }
                });
                *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
                clear_cache_dirty_state(dirty_state);
                clear_texture_unlock_dirty_rect_if_unused(texture_handle - 1);
                return cached_surface;
            }

            push_software_surface_to_destroy(cached_surface);
            software_surface_cache[texture_handle - 1][palette_handle] = NULL;
            cached_surface = NULL;
            *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
            clear_texture_unlock_dirty_rect_if_unused(texture_handle - 1);
        }
    }

    if (cached_surface != NULL) {
        clear_cache_dirty_state(dirty_state);
        clear_texture_unlock_dirty_rect_if_unused(texture_handle - 1);
        RENDERER_TELEMETRY({
            if (frame_stats_extended_enabled) {
                frame_stats.software_surface_cache_hits += 1;
            }
        });
        return cached_surface;
    }

    SDL_Surface* surface = surfaces[texture_handle - 1];
    if (surface == NULL) {
        return NULL;
    }

    SDL_Palette* palette = palette_handle != 0 ? palettes[palette_handle - 1] : NULL;
    if (palette != NULL) {
        SDL_SetSurfacePalette(surface, palette);
    }

    cached_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
    if (cached_surface == NULL) {
        return NULL;
    }

    RENDERER_TELEMETRY({
        if (frame_stats_extended_enabled) {
            frame_stats.software_surface_cache_creates += 1;
            note_software_surface_cache_create_provenance(dirty_state);
        }
    });
    software_surface_cache[texture_handle - 1][palette_handle] = cached_surface;
    *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
    clear_cache_dirty_state(dirty_state);
    return cached_surface;
}

#if ENABLE_PERF_TELEMETRY
static void count_task_source(SDLGameRenderer_TaskSource source) {
    switch (source) {
    case SDL_GAME_RENDERER_TASK_SOURCE_PPG:
        frame_stats.ppg_tasks += 1;
        break;
    case SDL_GAME_RENDERER_TASK_SOURCE_MTRANS:
        frame_stats.mtrans_tasks += 1;
        break;
    case SDL_GAME_RENDERER_TASK_SOURCE_UI_DIRECT:
        frame_stats.ui_direct_tasks += 1;
        break;
    case SDL_GAME_RENDERER_TASK_SOURCE_SOLID:
        frame_stats.solid_tasks += 1;
        break;
    case SDL_GAME_RENDERER_TASK_SOURCE_UNKNOWN:
    default:
        frame_stats.unknown_tasks += 1;
        break;
    }
}
#endif

static Uint64 render_task_submitted_pixels(const RenderTask* task) {
    if (task == NULL) {
        return 0;
    }

    if (task->type == RENDER_TASK_TYPE_TEXTURED_RECT) {
        const double area = (double)task->dst_rect.w * (double)task->dst_rect.h;
        return area > 0.0 ? (Uint64)llround(area) : 0;
    }

    const SDL_FPoint p0 = { task->vertices[0].position.x, task->vertices[0].position.y };
    const SDL_FPoint p1 = { task->vertices[1].position.x, task->vertices[1].position.y };
    const SDL_FPoint p2 = { task->vertices[2].position.x, task->vertices[2].position.y };
    const SDL_FPoint p3 = { task->vertices[3].position.x, task->vertices[3].position.y };
    const double tri0_area =
        SDL_fabs((double)(p1.x - p0.x) * (double)(p2.y - p0.y) - (double)(p2.x - p0.x) * (double)(p1.y - p0.y)) * 0.5;
    const double tri1_area =
        SDL_fabs((double)(p2.x - p1.x) * (double)(p3.y - p1.y) - (double)(p3.x - p1.x) * (double)(p2.y - p1.y)) * 0.5;
    const double area = tri0_area + tri1_area;
    return area > 0.0 ? (Uint64)llround(area) : 0;
}

#if ENABLE_PERF_TELEMETRY
static bool rect_task_fits_native_canvas(const RenderTask* task) {
    if ((task == NULL) || (task->type != RENDER_TASK_TYPE_TEXTURED_RECT)) {
        return false;
    }

    const float x0 = task->dst_rect.x;
    const float y0 = task->dst_rect.y;
    const float x1 = x0 + task->dst_rect.w;
    const float y1 = y0 + task->dst_rect.h;
    return (x0 >= -rect_task_epsilon) && (y0 >= -rect_task_epsilon) && (x1 <= (float)cps3_width + rect_task_epsilon) &&
           (y1 <= (float)cps3_height + rect_task_epsilon);
}

static HybridFallbackReason classify_hybrid_fallback_reason(const RenderTask* task) {
    if (task == NULL) {
        return HYBRID_FALLBACK_REASON_GEOMETRY;
    }

    if (task->texture == NULL) {
        return HYBRID_FALLBACK_REASON_SOLID;
    }

    if (task->type != RENDER_TASK_TYPE_TEXTURED_RECT) {
        return HYBRID_FALLBACK_REASON_GEOMETRY;
    }

    if (!rect_task_fits_native_canvas(task)) {
        return HYBRID_FALLBACK_REASON_CLIP;
    }

    if (((task->color >> 24) & 0xFFu) != 0xFFu) {
        return HYBRID_FALLBACK_REASON_ALPHA;
    }

    if ((task->color & 0x00FFFFFFu) != 0x00FFFFFFu) {
        return HYBRID_FALLBACK_REASON_COLOR_MOD;
    }

    if (task->flip != SDL_FLIP_NONE) {
        return HYBRID_FALLBACK_REASON_FLIP;
    }

    return HYBRID_FALLBACK_REASON_NONE;
}
static void note_hybrid_eligibility(const RenderTask* task) {
    if (!frame_stats_extended_enabled || (task == NULL)) {
        return;
    }

    const Uint64 submitted_pixels = render_task_submitted_pixels(task);
    const HybridFallbackReason reason = classify_hybrid_fallback_reason(task);

    if (reason == HYBRID_FALLBACK_REASON_NONE) {
        frame_stats.hybrid_candidate_tasks += 1;
        frame_stats.hybrid_candidate_pixels += submitted_pixels;
        return;
    }

    frame_stats.hybrid_fallback_tasks += 1;
    frame_stats.hybrid_fallback_pixels += submitted_pixels;

    switch (reason) {
    case HYBRID_FALLBACK_REASON_CLIP:
        frame_stats.hybrid_reason_clip += 1;
        break;
    case HYBRID_FALLBACK_REASON_ALPHA:
        frame_stats.hybrid_reason_alpha += 1;
        break;
    case HYBRID_FALLBACK_REASON_COLOR_MOD:
        frame_stats.hybrid_reason_color_mod += 1;
        break;
    case HYBRID_FALLBACK_REASON_FLIP:
        frame_stats.hybrid_reason_flip += 1;
        break;
    case HYBRID_FALLBACK_REASON_GEOMETRY:
        frame_stats.hybrid_reason_geometry += 1;
        break;
    case HYBRID_FALLBACK_REASON_SOLID:
        frame_stats.hybrid_reason_solid += 1;
        break;
    case HYBRID_FALLBACK_REASON_NONE:
    default:
        break;
    }
}
#else
static void note_hybrid_eligibility(const RenderTask* task) {
    (void)task;
}
#endif

static bool try_resolve_solid_task_as_rect(const RenderTask* task, SDL_FRect* out_rect, Uint32* out_color) {
    if ((task == NULL) || (task->type != RENDER_TASK_TYPE_GEOMETRY) || (task->texture != NULL)) {
        return false;
    }

    const SDL_FColor color0 = task->vertices[0].color;
    for (int i = 1; i < 4; i++) {
        const SDL_FColor color = task->vertices[i].color;
        if ((color.r != color0.r) || (color.g != color0.g) || (color.b != color0.b) || (color.a != color0.a)) {
            return false;
        }
    }

    float min_x = task->vertices[0].position.x;
    float max_x = min_x;
    float min_y = task->vertices[0].position.y;
    float max_y = min_y;
    for (int i = 1; i < 4; i++) {
        min_x = SDL_min(min_x, task->vertices[i].position.x);
        max_x = SDL_max(max_x, task->vertices[i].position.x);
        min_y = SDL_min(min_y, task->vertices[i].position.y);
        max_y = SDL_max(max_y, task->vertices[i].position.y);
    }

    if ((max_x - min_x) <= 0.0f || (max_y - min_y) <= 0.0f) {
        return false;
    }

    for (int i = 0; i < 4; i++) {
        const SDL_Vertex* vertex = &task->vertices[i];
        const bool left = nearly_equal(vertex->position.x, min_x);
        const bool right = nearly_equal(vertex->position.x, max_x);
        const bool top = nearly_equal(vertex->position.y, min_y);
        const bool bottom = nearly_equal(vertex->position.y, max_y);
        if (!((left || right) && !(left && right) && (top || bottom) && !(top && bottom))) {
            return false;
        }
    }

    if (out_rect != NULL) {
        out_rect->x = min_x;
        out_rect->y = min_y;
        out_rect->w = max_x - min_x;
        out_rect->h = max_y - min_y;
    }
    if (out_color != NULL) {
        *out_color = (((Uint32)SDL_roundf(color0.a * 255.0f)) << 24) | (((Uint32)SDL_roundf(color0.r * 255.0f)) << 16) |
                     (((Uint32)SDL_roundf(color0.g * 255.0f)) << 8) | ((Uint32)SDL_roundf(color0.b * 255.0f));
    }
    return true;
}

static SoftwareFrameFallbackReason classify_software_frame_fallback_reason(const RenderTask* task) {
    if (task == NULL) {
        return SOFTWARE_FRAME_FALLBACK_REASON_GEOMETRY;
    }

    if (task->type == RENDER_TASK_TYPE_TEXTURED_RECT) {
        return task->software_source_surface != NULL ? SOFTWARE_FRAME_FALLBACK_REASON_NONE
                                                     : SOFTWARE_FRAME_FALLBACK_REASON_GEOMETRY;
    }

    if (task->texture == NULL) {
        SDL_FRect solid_rect;
        Uint32 solid_color = 0;
        return try_resolve_solid_task_as_rect(task, &solid_rect, &solid_color) ? SOFTWARE_FRAME_FALLBACK_REASON_NONE
                                                                                : SOFTWARE_FRAME_FALLBACK_REASON_SOLID;
    }

    return SOFTWARE_FRAME_FALLBACK_REASON_GEOMETRY;
}

#if ENABLE_PERF_TELEMETRY
static void note_software_frame_eligibility(const RenderTask* task, SoftwareFrameFallbackReason reason) {
    if (!frame_stats_extended_enabled || (task == NULL)) {
        return;
    }

    const Uint64 submitted_pixels = render_task_submitted_pixels(task);
    if (reason == SOFTWARE_FRAME_FALLBACK_REASON_NONE) {
        frame_stats.software_frame_candidate_tasks += 1;
        frame_stats.software_frame_candidate_pixels += submitted_pixels;
        return;
    }

    frame_stats.software_frame_fallback_tasks += 1;
    frame_stats.software_frame_fallback_pixels += submitted_pixels;
    switch (reason) {
    case SOFTWARE_FRAME_FALLBACK_REASON_ALPHA:
        frame_stats.software_frame_reason_alpha += 1;
        break;
    case SOFTWARE_FRAME_FALLBACK_REASON_COLOR_MOD:
        frame_stats.software_frame_reason_color_mod += 1;
        break;
    case SOFTWARE_FRAME_FALLBACK_REASON_SOLID:
        frame_stats.software_frame_reason_solid += 1;
        break;
    case SOFTWARE_FRAME_FALLBACK_REASON_GEOMETRY:
        frame_stats.software_frame_reason_geometry += 1;
        break;
    case SOFTWARE_FRAME_FALLBACK_REASON_NONE:
    default:
        break;
    }
}
#else
static void note_software_frame_eligibility(const RenderTask* task, SoftwareFrameFallbackReason reason) {
    (void)task;
    (void)reason;
}
#endif

static SoftwareFrameFastCopyResult build_software_frame_fast_copy_plan(const RenderTask* task,
                                                                       const SDL_Surface* dst_surface,
                                                                       const SDL_Surface* src_surface,
                                                                       SoftwareFrameFastCopyPlan* out_plan) {
    if ((task == NULL) || (dst_surface == NULL) || (src_surface == NULL)) {
        return SOFTWARE_FRAME_FAST_COPY_RESULT_SOURCE_BOUNDS;
    }

    if ((task->flip != SDL_FLIP_NONE) && (task->flip != SDL_FLIP_HORIZONTAL) && (task->flip != SDL_FLIP_VERTICAL) &&
        (task->flip != SDL_FLIP_HORIZONTAL_AND_VERTICAL)) {
        return SOFTWARE_FRAME_FAST_COPY_RESULT_UNSUPPORTED_FLIP;
    }

    const int dst_x = (int)SDL_roundf(task->dst_rect.x);
    const int dst_y = (int)SDL_roundf(task->dst_rect.y);
    const int dst_w = (int)SDL_roundf(task->dst_rect.w);
    const int dst_h = (int)SDL_roundf(task->dst_rect.h);
    if (!nearly_equal(task->dst_rect.x, (float)dst_x) || !nearly_equal(task->dst_rect.y, (float)dst_y) ||
        !nearly_equal(task->dst_rect.w, (float)dst_w) || !nearly_equal(task->dst_rect.h, (float)dst_h) || (dst_w <= 0) ||
        (dst_h <= 0)) {
        return SOFTWARE_FRAME_FAST_COPY_RESULT_NON_INTEGER;
    }

    const float src_x_start_f = task->src_uv_rect.x * (float)src_surface->w;
    const float src_y_start_f = task->src_uv_rect.y * (float)src_surface->h;
    const float src_x_span_f = task->src_uv_rect.w * (float)src_surface->w;
    const float src_y_span_f = task->src_uv_rect.h * (float)src_surface->h;
    const int src_x = (int)SDL_roundf(src_x_start_f);
    const int src_y = (int)SDL_roundf(src_y_start_f);
    const int src_w = (int)SDL_roundf(src_x_span_f);
    const int src_h = (int)SDL_roundf(src_y_span_f);

    if (!nearly_equal(src_x_start_f, (float)src_x) || !nearly_equal(src_y_start_f, (float)src_y) ||
        !nearly_equal(src_x_span_f, (float)src_w) || !nearly_equal(src_y_span_f, (float)src_h) || (src_w <= 0) ||
        (src_h <= 0)) {
        return SOFTWARE_FRAME_FAST_COPY_RESULT_NON_INTEGER;
    }

    if ((src_x < 0) || (src_y < 0) || ((src_x + src_w) > src_surface->w) || ((src_y + src_h) > src_surface->h)) {
        return SOFTWARE_FRAME_FAST_COPY_RESULT_SOURCE_BOUNDS;
    }

    const bool exact_copy = (src_w == dst_w) && (src_h == dst_h);
    const bool color_mod = task->color != 0xFFFFFFFFu;
    if (color_mod && !exact_copy) {
        return SOFTWARE_FRAME_FAST_COPY_RESULT_COLOR_MOD;
    }

    if (out_plan != NULL) {
        const int dst_x0 = clamp_to_range(dst_x, 0, dst_surface->w);
        const int dst_y0 = clamp_to_range(dst_y, 0, dst_surface->h);
        const int dst_x1 = clamp_to_range(dst_x + dst_w, 0, dst_surface->w);
        const int dst_y1 = clamp_to_range(dst_y + dst_h, 0, dst_surface->h);
        const bool flip_h = (task->flip & SDL_FLIP_HORIZONTAL) != 0;
        const bool flip_v = (task->flip & SDL_FLIP_VERTICAL) != 0;

        out_plan->dst_x = dst_x;
        out_plan->dst_y = dst_y;
        out_plan->dst_w = dst_w;
        out_plan->dst_h = dst_h;
        out_plan->dst_x0 = dst_x0;
        out_plan->dst_y0 = dst_y0;
        out_plan->visible_w = dst_x1 - dst_x0;
        out_plan->visible_h = dst_y1 - dst_y0;
        out_plan->src_x = src_x;
        out_plan->src_y = src_y;
        out_plan->src_w = src_w;
        out_plan->src_h = src_h;
        out_plan->color_mod = color_mod;
        out_plan->flip_h = flip_h;
        out_plan->flip_v = flip_v;
        out_plan->clipped = (dst_x0 != dst_x) || (dst_y0 != dst_y) || (dst_x1 != (dst_x + dst_w)) ||
                            (dst_y1 != (dst_y + dst_h));
        out_plan->flipped = flip_h || flip_v;
    }

    return exact_copy ? SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT : SOFTWARE_FRAME_FAST_COPY_RESULT_SCALED;
}

#if ENABLE_PERF_TELEMETRY
static void note_software_frame_fast_copy_result(const RenderTask* task,
                                                 SoftwareFrameFastCopyResult result,
                                                 const SoftwareFrameFastCopyPlan* plan) {
    if (!frame_stats_extended_enabled || (task == NULL)) {
        return;
    }

    const Uint64 submitted_pixels = render_task_submitted_pixels(task);
    if (result == SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT) {
        frame_stats.software_frame_fast_exact_tasks += 1;
        frame_stats.software_frame_fast_exact_pixels += submitted_pixels;
        if ((plan != NULL) && plan->clipped) {
            frame_stats.software_frame_fast_exact_clipped_tasks += 1;
        }
        if ((plan != NULL) && plan->flipped) {
            frame_stats.software_frame_fast_exact_flipped_tasks += 1;
        }
        if ((plan != NULL) && plan->color_mod) {
            frame_stats.software_frame_fast_exact_color_mod_tasks += 1;
            frame_stats.software_frame_fast_exact_color_mod_pixels += submitted_pixels;
        }
        return;
    }
    if (result == SOFTWARE_FRAME_FAST_COPY_RESULT_SCALED) {
        frame_stats.software_frame_fast_scaled_tasks += 1;
        frame_stats.software_frame_fast_scaled_pixels += submitted_pixels;
        return;
    }

    frame_stats.software_frame_generic_textured_tasks += 1;
    frame_stats.software_frame_generic_textured_pixels += submitted_pixels;
    switch (result) {
    case SOFTWARE_FRAME_FAST_COPY_RESULT_COLOR_MOD:
        frame_stats.software_frame_fast_miss_color_mod += 1;
        break;
    case SOFTWARE_FRAME_FAST_COPY_RESULT_NON_INTEGER:
        frame_stats.software_frame_fast_miss_non_integer += 1;
        if (submitted_pixels >= 256u) {
            frame_stats.software_frame_fast_miss_non_integer_ge_256_tasks += 1;
            frame_stats.software_frame_fast_miss_non_integer_ge_256_pixels += submitted_pixels;
        }
        if (submitted_pixels >= 1024u) {
            frame_stats.software_frame_fast_miss_non_integer_ge_1024_tasks += 1;
            frame_stats.software_frame_fast_miss_non_integer_ge_1024_pixels += submitted_pixels;
        }
        if (submitted_pixels > frame_stats.software_frame_fast_miss_non_integer_max_pixels) {
            frame_stats.software_frame_fast_miss_non_integer_max_pixels = submitted_pixels;
        }
        break;
    case SOFTWARE_FRAME_FAST_COPY_RESULT_UNSUPPORTED_FLIP:
        frame_stats.software_frame_fast_miss_unsupported_flip += 1;
        break;
    case SOFTWARE_FRAME_FAST_COPY_RESULT_SOURCE_BOUNDS:
        frame_stats.software_frame_fast_miss_source_bounds += 1;
        break;
    case SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT:
    case SOFTWARE_FRAME_FAST_COPY_RESULT_SCALED:
    default:
        break;
    }
}
#else
static void note_software_frame_fast_copy_result(const RenderTask* task,
                                                 SoftwareFrameFastCopyResult result,
                                                 const SoftwareFrameFastCopyPlan* plan) {
    (void)task;
    (void)result;
    (void)plan;
}
#endif

#if ENABLE_PERF_TELEMETRY
static void note_software_frame_fast_non_integer(const RenderTask* task) {
    if (!frame_stats_extended_enabled || (task == NULL)) {
        return;
    }

    const Uint64 submitted_pixels = render_task_submitted_pixels(task);
    frame_stats.software_frame_fast_non_integer_tasks += 1;
    frame_stats.software_frame_fast_non_integer_pixels += submitted_pixels;
}
#else
static void note_software_frame_fast_non_integer(const RenderTask* task) {
    (void)task;
}
#endif

static void populate_scaled_lookup_table(int* out_lookup,
                                         int visible_count,
                                         int dst_origin,
                                         int dst_start,
                                         int dst_span,
                                         int src_origin,
                                         int src_span,
                                         bool flip) {
    for (int i = 0; i < visible_count; i++) {
        const int dst_offset = (dst_start + i) - dst_origin;
        const int src_offset = (((dst_offset * 2) + 1) * src_span) / (dst_span * 2);
        out_lookup[i] = flip ? (src_origin + src_span - 1 - src_offset) : (src_origin + src_offset);
    }
}

static bool try_fast_copy_fast_textured_task_to_software_frame(const RenderTask* task,
                                                               const SoftwareFrameFastCopyPlan* plan,
                                                               SDL_Surface* dst_surface,
                                                               SDL_Surface* src_surface) {
    if ((task == NULL) || (plan == NULL) || (dst_surface == NULL) || (src_surface == NULL)) {
        return false;
    }

    if ((plan->visible_w <= 0) || (plan->visible_h <= 0)) {
        return true;
    }

    const Uint32* src_pixels = (const Uint32*)src_surface->pixels;
    Uint32* dst_pixels = (Uint32*)dst_surface->pixels;
    const int src_pitch = src_surface->pitch / (int)sizeof(Uint32);
    const int dst_pitch = dst_surface->pitch / (int)sizeof(Uint32);

    if ((plan->src_w == plan->dst_w) && (plan->src_h == plan->dst_h)) {
        const int clip_left = plan->dst_x0 - plan->dst_x;
        const int clip_top = plan->dst_y0 - plan->dst_y;
        const int src_x_step = plan->flip_h ? -1 : 1;
        const int src_y_step = plan->flip_v ? -1 : 1;
        const int src_row0_x = plan->flip_h ? (plan->src_x + plan->src_w - 1 - clip_left) : (plan->src_x + clip_left);
        const int src_row0_y = plan->flip_v ? (plan->src_y + plan->src_h - 1 - clip_top) : (plan->src_y + clip_top);

        if (plan->color_mod) {
            const Uint32 color = task->color;
            const Uint32 mod_a = (color >> 24) & 0xFFu;
            if (mod_a == 0u) {
                return true;
            }

            for (int row = 0; row < plan->visible_h; row++) {
                const Uint32* src_row = src_pixels + ((src_row0_y + (row * src_y_step)) * src_pitch) + src_row0_x;
                Uint32* dst_row = dst_pixels + ((plan->dst_y0 + row) * dst_pitch) + plan->dst_x0;
                const Uint32* src_pixel_ptr = src_row;
                for (int col = 0; col < plan->visible_w; col++) {
                    Uint32 src_pixel = modulate_argb8888(*src_pixel_ptr, color);
                    const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                    if (src_a == 0u) {
                        src_pixel_ptr += src_x_step;
                        continue;
                    }
                    if (src_a == 0xFFu) {
                        dst_row[col] = src_pixel;
                        src_pixel_ptr += src_x_step;
                        continue;
                    }
                    dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
                    src_pixel_ptr += src_x_step;
                }
            }
            return true;
        }

        for (int row = 0; row < plan->visible_h; row++) {
            const Uint32* src_row = src_pixels + ((src_row0_y + (row * src_y_step)) * src_pitch) + src_row0_x;
            Uint32* dst_row = dst_pixels + ((plan->dst_y0 + row) * dst_pitch) + plan->dst_x0;
            const Uint32* src_pixel_ptr = src_row;
            for (int col = 0; col < plan->visible_w; col++) {
                const Uint32 src_pixel = *src_pixel_ptr;
                const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                if (src_a == 0u) {
                    src_pixel_ptr += src_x_step;
                    continue;
                }
                if (src_a == 0xFFu) {
                    dst_row[col] = src_pixel;
                    src_pixel_ptr += src_x_step;
                    continue;
                }
                dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
                src_pixel_ptr += src_x_step;
            }
        }
        return true;
    }

    int src_x_lookup[cps3_width];
    int src_y_lookup[cps3_height];
    populate_scaled_lookup_table(
        src_x_lookup, plan->visible_w, plan->dst_x, plan->dst_x0, plan->dst_w, plan->src_x, plan->src_w, plan->flip_h);
    populate_scaled_lookup_table(
        src_y_lookup, plan->visible_h, plan->dst_y, plan->dst_y0, plan->dst_h, plan->src_y, plan->src_h, plan->flip_v);

    for (int row = 0; row < plan->visible_h; row++) {
        const Uint32* src_row = src_pixels + (src_y_lookup[row] * src_pitch);
        Uint32* dst_row = dst_pixels + ((plan->dst_y0 + row) * dst_pitch) + plan->dst_x0;
        for (int col = 0; col < plan->visible_w; col++) {
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
    }

    return true;
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
    const Uint32 out_a = (src_a * mod_a + 127u) / 255u;
    const Uint32 out_r = (src_r * mod_r + 127u) / 255u;
    const Uint32 out_g = (src_g * mod_g + 127u) / 255u;
    const Uint32 out_b = (src_b * mod_b + 127u) / 255u;
    return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static Uint8 blend_argb8888_channel(Uint32 src_c, Uint32 src_a, Uint32 dst_c, Uint32 dst_a, Uint32 out_a) {
    if (out_a == 0u) {
        return 0;
    }

    const Uint32 src_premul = src_c * src_a;
    const Uint32 dst_premul = dst_c * dst_a;
    const Uint32 out_premul = src_premul + ((dst_premul * (255u - src_a) + 127u) / 255u);
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
    const Uint32 out_a = src_a + ((dst_a * (255u - src_a) + 127u) / 255u);
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

static void fill_argb8888_span(Uint32* dst_pixels, int pixel_count, Uint32 color) {
    if ((dst_pixels == NULL) || (pixel_count <= 0)) {
        return;
    }

    int x = 0;
    for (; (x + 4) <= pixel_count; x += 4) {
        dst_pixels[x] = color;
        dst_pixels[x + 1] = color;
        dst_pixels[x + 2] = color;
        dst_pixels[x + 3] = color;
    }
    for (; x < pixel_count; x++) {
        dst_pixels[x] = color;
    }
}

static Uint32 blend_solid_argb8888(Uint32 dst_pixel,
                                   Uint32 src_a,
                                   Uint32 inv_src_a,
                                   Uint32 src_r_premul,
                                   Uint32 src_g_premul,
                                   Uint32 src_b_premul) {
    const Uint32 dst_a = (dst_pixel >> 24) & 0xFFu;
    const Uint32 dst_r = (dst_pixel >> 16) & 0xFFu;
    const Uint32 dst_g = (dst_pixel >> 8) & 0xFFu;
    const Uint32 dst_b = dst_pixel & 0xFFu;

    if (dst_a == 255u) {
        const Uint32 out_r = (src_r_premul + (dst_r * inv_src_a) + 127u) / 255u;
        const Uint32 out_g = (src_g_premul + (dst_g * inv_src_a) + 127u) / 255u;
        const Uint32 out_b = (src_b_premul + (dst_b * inv_src_a) + 127u) / 255u;
        return 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
    }

    const Uint32 out_a = src_a + ((dst_a * inv_src_a + 127u) / 255u);
    if (out_a == 0u) {
        return 0u;
    }

    const Uint32 dst_r_premul = dst_r * dst_a;
    const Uint32 dst_g_premul = dst_g * dst_a;
    const Uint32 dst_b_premul = dst_b * dst_a;
    const Uint32 out_r_premul = src_r_premul + ((dst_r_premul * inv_src_a + 127u) / 255u);
    const Uint32 out_g_premul = src_g_premul + ((dst_g_premul * inv_src_a + 127u) / 255u);
    const Uint32 out_b_premul = src_b_premul + ((dst_b_premul * inv_src_a + 127u) / 255u);
    const Uint32 out_r = (out_r_premul + (out_a / 2u)) / out_a;
    const Uint32 out_g = (out_g_premul + (out_a / 2u)) / out_a;
    const Uint32 out_b = (out_b_premul + (out_a / 2u)) / out_a;
    return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static int compare_render_tasks(const RenderTask* a, const RenderTask* b) {
    if (a->z < b->z) {
        return -1;
    } else if (a->z > b->z) {
        return 1;
    } else {
        // This eliminates z-fighting
        if (a->index < b->index) {
            return 1;
        } else if (a->index > b->index) {
            return -1;
        } else {
            return 0;
        }
    }
}

static void insertion_sort_render_tasks(void) {
    for (int i = 1; i < render_task_count; i++) {
        RenderTask task = render_tasks[i];
        int j = i - 1;

        while ((j >= 0) && (compare_render_tasks(&render_tasks[j], &task) > 0)) {
            render_tasks[j + 1] = render_tasks[j];
            j -= 1;
        }

        render_tasks[j + 1] = task;
    }

    render_tasks_have_z_inversion = false;
}

static void initialize_render_task_batch_indices(void) {
    const int vertices_per_task = SDL_arraysize(render_tasks[0].vertices);
    const int indices_per_task = SDL_arraysize(render_task_indices);

    for (int task_index = 0; task_index < RENDER_TASK_MAX; task_index++) {
        const int vertex_base = task_index * vertices_per_task;
        const int index_base = task_index * indices_per_task;

        for (int i = 0; i < indices_per_task; i++) {
            render_task_batch_indices[index_base + i] = vertex_base + render_task_indices[i];
        }
    }

    render_task_batch_indices_initialized = true;
}

static bool set_texture_submit_mod(SDL_Texture* texture, Uint32 color) {
    if (submitted_texture_mod_valid && (submitted_texture_mod == texture) && (submitted_texture_mod_color == color)) {
        return true;
    }

    SDL_Color mod;
    read_rgba32_color(color, &mod);

    if (!SDL_SetTextureColorMod(texture, mod.r, mod.g, mod.b)) {
        return false;
    }
    if (!SDL_SetTextureAlphaMod(texture, mod.a)) {
        return false;
    }

    submitted_texture_mod = texture;
    submitted_texture_mod_color = color;
    submitted_texture_mod_valid = true;
    return true;
}

static void populate_rect_task_vertices(const RenderTask* task, SDL_Vertex out_vertices[4]) {
    const float x0 = task->dst_rect.x;
    const float y0 = task->dst_rect.y;
    const float x1 = x0 + task->dst_rect.w;
    const float y1 = y0 + task->dst_rect.h;
    const float s0 = (task->flip & SDL_FLIP_HORIZONTAL) ? (task->src_uv_rect.x + task->src_uv_rect.w) : task->src_uv_rect.x;
    const float t0 = (task->flip & SDL_FLIP_VERTICAL) ? (task->src_uv_rect.y + task->src_uv_rect.h) : task->src_uv_rect.y;
    const float s1 = (task->flip & SDL_FLIP_HORIZONTAL) ? task->src_uv_rect.x : (task->src_uv_rect.x + task->src_uv_rect.w);
    const float t1 = (task->flip & SDL_FLIP_VERTICAL) ? task->src_uv_rect.y : (task->src_uv_rect.y + task->src_uv_rect.h);
    SDL_FColor fcolor;
    read_rgba32_fcolor(task->color, &fcolor);

    out_vertices[0].position.x = x0;
    out_vertices[0].position.y = y0;
    out_vertices[0].tex_coord.x = s0;
    out_vertices[0].tex_coord.y = t0;
    out_vertices[0].color = fcolor;

    out_vertices[1].position.x = x1;
    out_vertices[1].position.y = y0;
    out_vertices[1].tex_coord.x = s1;
    out_vertices[1].tex_coord.y = t0;
    out_vertices[1].color = fcolor;

    out_vertices[2].position.x = x0;
    out_vertices[2].position.y = y1;
    out_vertices[2].tex_coord.x = s0;
    out_vertices[2].tex_coord.y = t1;
    out_vertices[2].color = fcolor;

    out_vertices[3].position.x = x1;
    out_vertices[3].position.y = y1;
    out_vertices[3].tex_coord.x = s1;
    out_vertices[3].tex_coord.y = t1;
    out_vertices[3].color = fcolor;
}

static void submit_rect_task_as_geometry_with_texture(const RenderTask* task, SDL_Texture* texture) {
    SDL_Vertex rect_vertices[4];
    populate_rect_task_vertices(task, rect_vertices);
    SDL_RenderGeometry(_renderer,
                       texture,
                       rect_vertices,
                       SDL_arraysize(rect_vertices),
                       render_task_indices,
                       SDL_arraysize(render_task_indices));
}

static bool try_resolve_geometry_task_as_rect_copy(const RenderTask* task, RenderTask* out_rect_task) {
    if (out_rect_task != NULL) {
        SDL_zero(*out_rect_task);
    }
    if ((task->type != RENDER_TASK_TYPE_GEOMETRY) || (task->texture == NULL)) {
        return false;
    }

    const SDL_FColor color0 = task->vertices[0].color;
    for (int i = 1; i < 4; i++) {
        const SDL_FColor color = task->vertices[i].color;
        if ((color.r != color0.r) || (color.g != color0.g) || (color.b != color0.b) || (color.a != color0.a)) {
            return false;
        }
    }

    float texture_width = 0.0f;
    float texture_height = 0.0f;
    if (!SDL_GetTextureSize(task->texture, &texture_width, &texture_height) || (texture_width <= 0.0f) ||
        (texture_height <= 0.0f)) {
        return false;
    }

    float min_x = task->vertices[0].position.x;
    float max_x = min_x;
    float min_y = task->vertices[0].position.y;
    float max_y = min_y;
    float min_u = task->vertices[0].tex_coord.x;
    float max_u = min_u;
    float min_v = task->vertices[0].tex_coord.y;
    float max_v = min_v;

    for (int i = 1; i < 4; i++) {
        min_x = SDL_min(min_x, task->vertices[i].position.x);
        max_x = SDL_max(max_x, task->vertices[i].position.x);
        min_y = SDL_min(min_y, task->vertices[i].position.y);
        max_y = SDL_max(max_y, task->vertices[i].position.y);
        min_u = SDL_min(min_u, task->vertices[i].tex_coord.x);
        max_u = SDL_max(max_u, task->vertices[i].tex_coord.x);
        max_v = SDL_max(max_v, task->vertices[i].tex_coord.y);
        min_v = SDL_min(min_v, task->vertices[i].tex_coord.y);
    }

    if ((max_x - min_x) <= 0.0f || (max_y - min_y) <= 0.0f || (max_u - min_u) <= 0.0f || (max_v - min_v) <= 0.0f) {
        return false;
    }

    float norm_min_u = min_u;
    float norm_max_u = max_u;
    float norm_min_v = min_v;
    float norm_max_v = max_v;
    const bool normalized_uv = (min_u >= 0.0f) && (min_v >= 0.0f) && (max_u <= 1.0f) && (max_v <= 1.0f);
    const bool pixel_uv = (min_u >= 0.0f) && (min_v >= 0.0f) && (max_u <= texture_width) && (max_v <= texture_height);
    if (!normalized_uv) {
        if (!pixel_uv) {
            return false;
        }
        norm_min_u /= texture_width;
        norm_max_u /= texture_width;
        norm_min_v /= texture_height;
        norm_max_v /= texture_height;
    }

    bool flip_h = false;
    bool flip_v = false;
    bool flip_h_set = false;
    bool flip_v_set = false;
    for (int i = 0; i < 4; i++) {
        const SDL_Vertex* vertex = &task->vertices[i];
        const bool left = nearly_equal(vertex->position.x, min_x);
        const bool right = nearly_equal(vertex->position.x, max_x);
        const bool top = nearly_equal(vertex->position.y, min_y);
        const bool bottom = nearly_equal(vertex->position.y, max_y);
        float u = vertex->tex_coord.x;
        float v = vertex->tex_coord.y;
        if (!normalized_uv) {
            u /= texture_width;
            v /= texture_height;
        }
        const bool low_u = nearly_equal(u, norm_min_u);
        const bool high_u = nearly_equal(u, norm_max_u);
        const bool low_v = nearly_equal(v, norm_min_v);
        const bool high_v = nearly_equal(v, norm_max_v);

        if (!((left || right) && !(left && right) && (top || bottom) && !(top && bottom) && (low_u || high_u) &&
              !(low_u && high_u) && (low_v || high_v) && !(low_v && high_v))) {
            return false;
        }

        const bool vertex_flip_h = right != high_u;
        const bool vertex_flip_v = bottom != high_v;
        if (!flip_h_set) {
            flip_h = vertex_flip_h;
            flip_h_set = true;
        } else if (flip_h != vertex_flip_h) {
            return false;
        }
        if (!flip_v_set) {
            flip_v = vertex_flip_v;
            flip_v_set = true;
        } else if (flip_v != vertex_flip_v) {
            return false;
        }
    }

    RenderTask rect_task = *task;
    rect_task.type = RENDER_TASK_TYPE_TEXTURED_RECT;
    rect_task.dst_rect.x = min_x;
    rect_task.dst_rect.y = min_y;
    rect_task.dst_rect.w = max_x - min_x;
    rect_task.dst_rect.h = max_y - min_y;
    rect_task.src_uv_rect.x = norm_min_u;
    rect_task.src_uv_rect.y = norm_min_v;
    rect_task.src_uv_rect.w = norm_max_u - norm_min_u;
    rect_task.src_uv_rect.h = norm_max_v - norm_min_v;
    rect_task.flip = SDL_FLIP_NONE;
    if (flip_h) {
        rect_task.flip |= SDL_FLIP_HORIZONTAL;
    }
    if (flip_v) {
        rect_task.flip |= SDL_FLIP_VERTICAL;
    }
    rect_task.color = (((Uint32)SDL_roundf(color0.a * 255.0f)) << 24) | (((Uint32)SDL_roundf(color0.r * 255.0f)) << 16) |
                      (((Uint32)SDL_roundf(color0.g * 255.0f)) << 8) | ((Uint32)SDL_roundf(color0.b * 255.0f));
    if (out_rect_task != NULL) {
        *out_rect_task = rect_task;
    }
    return true;
}

static bool try_submit_geometry_task_as_rect_copy(const RenderTask* task,
                                                  bool* out_used_rect_path,
                                                  RenderTask* out_submitted_rect_task) {
    if (out_used_rect_path != NULL) {
        *out_used_rect_path = false;
    }
    if (out_submitted_rect_task != NULL) {
        SDL_zero(*out_submitted_rect_task);
    }

    RenderTask rect_task;
    if (!try_resolve_geometry_task_as_rect_copy(task, &rect_task)) {
        return false;
    }

    const bool used_rect_path = submit_rect_task(&rect_task);
    if (out_used_rect_path != NULL) {
        *out_used_rect_path = used_rect_path;
    }
    if (used_rect_path && (out_submitted_rect_task != NULL)) {
        *out_submitted_rect_task = rect_task;
    }
    return true;
}

static bool submit_rect_task(const RenderTask* task) {
    SDL_Texture* texture = get_submit_texture_for_task(task);
    float texture_width = 0.0f;
    float texture_height = 0.0f;
    if (!SDL_GetTextureSize(texture, &texture_width, &texture_height)) {
        submit_rect_task_as_geometry_with_texture(task, texture);
        return false;
    }

    if (!set_texture_submit_mod(texture, task->color)) {
        submit_rect_task_as_geometry_with_texture(task, texture);
        return false;
    }

    const SDL_FRect src_rect = { .x = task->src_uv_rect.x * texture_width,
                                 .y = task->src_uv_rect.y * texture_height,
                                 .w = task->src_uv_rect.w * texture_width,
                                 .h = task->src_uv_rect.h * texture_height };
    const bool ok = (task->flip == SDL_FLIP_NONE)
                        ? SDL_RenderTexture(_renderer, texture, &src_rect, &task->dst_rect)
                        : SDL_RenderTextureRotated(_renderer, texture, &src_rect, &task->dst_rect, 0.0, NULL, task->flip);

    if (!ok) {
        submit_rect_task_as_geometry_with_texture(task, texture);
        return false;
    }

    RENDERER_TELEMETRY(frame_stats.rect_copy_tasks += 1);
    return true;
}

static bool raster_textured_task_to_software_frame(const RenderTask* task) {
    if ((task == NULL) || (software_frame_surface == NULL) || (task->software_source_surface == NULL) ||
        (task->dst_rect.w <= 0.0f) || (task->dst_rect.h <= 0.0f)) {
        return false;
    }

    SDL_Surface* dst_surface = software_frame_surface;
    SDL_Surface* src_surface = task->software_source_surface;
    bool src_locked = false;
    if (SDL_MUSTLOCK(src_surface)) {
        if (!SDL_LockSurface(src_surface)) {
            return false;
        }
        src_locked = true;
    }

    const int dst_x0 = clamp_to_range((int)SDL_floorf(task->dst_rect.x), 0, dst_surface->w);
    const int dst_y0 = clamp_to_range((int)SDL_floorf(task->dst_rect.y), 0, dst_surface->h);
    const int dst_x1 = clamp_to_range((int)SDL_ceilf(task->dst_rect.x + task->dst_rect.w), 0, dst_surface->w);
    const int dst_y1 = clamp_to_range((int)SDL_ceilf(task->dst_rect.y + task->dst_rect.h), 0, dst_surface->h);
    if ((dst_x1 <= dst_x0) || (dst_y1 <= dst_y0)) {
        if (src_locked) {
            SDL_UnlockSurface(src_surface);
        }
        return true;
    }

    SoftwareFrameFastCopyPlan fast_copy_plan = { 0 };
    const SoftwareFrameFastCopyResult fast_copy_result =
        build_software_frame_fast_copy_plan(task, dst_surface, src_surface, &fast_copy_plan);
    if ((fast_copy_result == SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT) ||
        (fast_copy_result == SOFTWARE_FRAME_FAST_COPY_RESULT_SCALED)) {
        if (try_fast_copy_fast_textured_task_to_software_frame(task, &fast_copy_plan, dst_surface, src_surface)) {
            note_software_frame_fast_copy_result(task, fast_copy_result, &fast_copy_plan);
            if (src_locked) {
                SDL_UnlockSurface(src_surface);
            }
            return true;
        }
    } else if (fast_copy_result == SOFTWARE_FRAME_FAST_COPY_RESULT_NON_INTEGER) {
        const Uint64 submitted_pixels = render_task_submitted_pixels(task);
        if ((submitted_pixels >= software_frame_non_integer_lookup_threshold_pixels) &&
            SDLSoftwareFrame_RasterNonIntegerLookupARGB8888(
                &task->dst_rect, &task->src_uv_rect, task->flip, task->color, dst_surface, src_surface)) {
            note_software_frame_fast_non_integer(task);
            if (src_locked) {
                SDL_UnlockSurface(src_surface);
            }
            return true;
        }
    }

    if ((fast_copy_result != SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT) &&
        (fast_copy_result != SOFTWARE_FRAME_FAST_COPY_RESULT_SCALED)) {
        note_software_frame_fast_copy_result(task, fast_copy_result, NULL);
    }

    const float src_x_start = task->src_uv_rect.x * (float)src_surface->w;
    const float src_y_start = task->src_uv_rect.y * (float)src_surface->h;
    const float src_x_span = task->src_uv_rect.w * (float)src_surface->w;
    const float src_y_span = task->src_uv_rect.h * (float)src_surface->h;
    const Uint32* src_pixels = (const Uint32*)src_surface->pixels;
    Uint32* dst_pixels = (Uint32*)dst_surface->pixels;
    const int src_pitch = src_surface->pitch / (int)sizeof(Uint32);
    const int dst_pitch = dst_surface->pitch / (int)sizeof(Uint32);
    const bool flip_h = (task->flip & SDL_FLIP_HORIZONTAL) != 0;
    const bool flip_v = (task->flip & SDL_FLIP_VERTICAL) != 0;

    for (int y = dst_y0; y < dst_y1; y++) {
        float v = (((float)y + 0.5f) - task->dst_rect.y) / task->dst_rect.h;
        v = SDL_max(0.0f, SDL_min(v, 0.999999f));
        if (flip_v) {
            v = 1.0f - v;
            v = SDL_max(0.0f, SDL_min(v, 0.999999f));
        }

        const int src_y = clamp_to_range((int)SDL_floorf(src_y_start + (v * src_y_span)), 0, src_surface->h - 1);
        const Uint32* src_row = src_pixels + (src_y * src_pitch);
        Uint32* dst_row = dst_pixels + (y * dst_pitch);
        for (int x = dst_x0; x < dst_x1; x++) {
            float u = (((float)x + 0.5f) - task->dst_rect.x) / task->dst_rect.w;
            u = SDL_max(0.0f, SDL_min(u, 0.999999f));
            if (flip_h) {
                u = 1.0f - u;
                u = SDL_max(0.0f, SDL_min(u, 0.999999f));
            }

            const int src_x =
                clamp_to_range((int)SDL_floorf(src_x_start + (u * src_x_span)), 0, src_surface->w - 1);
            Uint32 src_pixel = src_row[src_x];
            if (task->color != 0xFFFFFFFFu) {
                src_pixel = modulate_argb8888(src_pixel, task->color);
            }
            dst_row[x] = blend_argb8888(dst_row[x], src_pixel);
        }
    }

    if (src_locked) {
        SDL_UnlockSurface(src_surface);
    }
    return true;
}

static bool raster_solid_task_to_software_frame(const RenderTask* task) {
    if ((task == NULL) || (software_frame_surface == NULL) || (task->dst_rect.w <= 0.0f) || (task->dst_rect.h <= 0.0f)) {
        return false;
    }

    const int dst_x0 = clamp_to_range((int)SDL_floorf(task->dst_rect.x), 0, software_frame_surface->w);
    const int dst_y0 = clamp_to_range((int)SDL_floorf(task->dst_rect.y), 0, software_frame_surface->h);
    const int dst_x1 = clamp_to_range((int)SDL_ceilf(task->dst_rect.x + task->dst_rect.w), 0, software_frame_surface->w);
    const int dst_y1 = clamp_to_range((int)SDL_ceilf(task->dst_rect.y + task->dst_rect.h), 0, software_frame_surface->h);
    if ((dst_x1 <= dst_x0) || (dst_y1 <= dst_y0)) {
        return true;
    }

    Uint32* dst_pixels = (Uint32*)software_frame_surface->pixels;
    const int dst_pitch = software_frame_surface->pitch / (int)sizeof(Uint32);
    const Uint32 src_a = (task->color >> 24) & 0xFFu;
    if (src_a == 0u) {
        return true;
    }

    const int fill_width = dst_x1 - dst_x0;
    if (src_a == 255u) {
        for (int y = dst_y0; y < dst_y1; y++) {
            Uint32* dst_row = dst_pixels + (y * dst_pitch) + dst_x0;
            fill_argb8888_span(dst_row, fill_width, task->color);
        }
        return true;
    }

    const Uint32 inv_src_a = 255u - src_a;
    const Uint32 src_r = (task->color >> 16) & 0xFFu;
    const Uint32 src_g = (task->color >> 8) & 0xFFu;
    const Uint32 src_b = task->color & 0xFFu;
    const Uint32 src_r_premul = src_r * src_a;
    const Uint32 src_g_premul = src_g * src_a;
    const Uint32 src_b_premul = src_b * src_a;
    for (int y = dst_y0; y < dst_y1; y++) {
        Uint32* dst_row = dst_pixels + (y * dst_pitch) + dst_x0;
        for (int x = 0; x < fill_width; x++) {
            dst_row[x] =
                blend_solid_argb8888(dst_row[x], src_a, inv_src_a, src_r_premul, src_g_premul, src_b_premul);
        }
    }
    return true;
}

static bool render_frame_to_software_surface(void) {
    if ((software_frame_surface == NULL) || !software_frame_surface_ready) {
        return false;
    }

    bool frame_supported = true;
    for (int i = 0; i < render_task_count; i++) {
        const RenderTask* task = &render_tasks[i];
        RenderTask resolved_task = *task;

        if ((task->type == RENDER_TASK_TYPE_GEOMETRY) && (task->texture != NULL)) {
            if (!try_resolve_geometry_task_as_rect_copy(task, &resolved_task)) {
                note_software_frame_eligibility(task, SOFTWARE_FRAME_FALLBACK_REASON_GEOMETRY);
                frame_supported = false;
                continue;
            }
        } else if ((task->type == RENDER_TASK_TYPE_GEOMETRY) && (task->texture == NULL)) {
            SDL_FRect solid_rect;
            Uint32 solid_color = 0;
            if (!try_resolve_solid_task_as_rect(task, &solid_rect, &solid_color)) {
                note_software_frame_eligibility(task, SOFTWARE_FRAME_FALLBACK_REASON_SOLID);
                frame_supported = false;
                continue;
            }
            resolved_task.dst_rect = solid_rect;
            resolved_task.color = solid_color;
        }

        const SoftwareFrameFallbackReason reason = classify_software_frame_fallback_reason(&resolved_task);
        note_software_frame_eligibility(&resolved_task, reason);
        software_frame_resolved_tasks[i] = resolved_task;
        if (reason != SOFTWARE_FRAME_FALLBACK_REASON_NONE) {
            frame_supported = false;
        }
    }

    if (!frame_supported) {
        return false;
    }

    bool dst_locked = false;
    if (SDL_MUSTLOCK(software_frame_surface)) {
        if (!SDL_LockSurface(software_frame_surface)) {
            return false;
        }
        dst_locked = true;
    }

    for (int i = 0; i < render_task_count; i++) {
        const RenderTask* task = &software_frame_resolved_tasks[i];
        const bool ok = (task->type == RENDER_TASK_TYPE_TEXTURED_RECT) ? raster_textured_task_to_software_frame(task)
                                                                       : raster_solid_task_to_software_frame(task);
        if (!ok) {
            if (dst_locked) {
                SDL_UnlockSurface(software_frame_surface);
            }
            return false;
        }
    }

    if (dst_locked) {
        SDL_UnlockSurface(software_frame_surface);
    }
    return true;
}

static bool upload_software_frame_to_canvas(void) {
    if ((software_frame_surface == NULL) || !ensure_software_frame_upload_texture()) {
        return false;
    }

    if (!SDL_UpdateTexture(
            software_frame_upload_texture, NULL, software_frame_surface->pixels, software_frame_surface->pitch)) {
        return false;
    }

    SDLMessageRenderer_InvalidateTargetBindCache();
    cps3_target_bound = SDL_SetRenderTarget(_renderer, cps3_canvas);
    if (!cps3_target_bound) {
        return false;
    }

    const SDL_FRect dst_rect = { .x = 0.0f, .y = 0.0f, .w = (float)cps3_width, .h = (float)cps3_height };
    if (!SDL_RenderTexture(_renderer, software_frame_upload_texture, NULL, &dst_rect)) {
        return false;
    }

    software_frame_uploaded = true;
    RENDERER_TELEMETRY(frame_stats.software_frame_uploaded = 1);
    return true;
}

static bool ensure_software_frame_canvas_for_frame(void) {
    if (!software_frame_owned || software_frame_uploaded) {
        return true;
    }

    if (upload_software_frame_to_canvas()) {
        RENDERER_TELEMETRY(frame_stats.software_frame_fallback = 0);
        return true;
    }

    software_frame_owned = false;
    RENDERER_TELEMETRY({
        frame_stats.software_frame_owned = 0;
        frame_stats.software_frame_direct_present = 0;
        frame_stats.software_frame_fallback = 1;
    });
    SDL_SetRenderTarget(_renderer, cps3_canvas);
    SDL_RenderClear(_renderer);
    submit_render_tasks();
    return true;
}

#if ENABLE_PERF_TELEMETRY
static void flush_rect_texture_strip_group_stats(int* strip_length, int* strip_runs, int* strip_tasks) {
    if ((strip_length == NULL) || (*strip_length <= 1)) {
        if (strip_length != NULL) {
            *strip_length = 0;
        }
        return;
    }

    if ((strip_runs != NULL) && (strip_tasks != NULL)) {
        *strip_runs += 1;
        *strip_tasks += *strip_length;
    }

    *strip_length = 0;
}

static bool rect_tasks_have_matching_strip_basics(const RenderTask* prev, const RenderTask* next) {
    return (prev != NULL) && (next != NULL) && (prev->type == RENDER_TASK_TYPE_TEXTURED_RECT) &&
           (next->type == RENDER_TASK_TYPE_TEXTURED_RECT) && (prev->texture == next->texture) &&
           (prev->color == next->color) && (prev->flip == next->flip);
}

static float rect_task_source_axis_start(const RenderTask* task, bool horizontal) {
    if (horizontal) {
        return (task->flip & SDL_FLIP_HORIZONTAL) ? (task->src_uv_rect.x + task->src_uv_rect.w) : task->src_uv_rect.x;
    }

    return (task->flip & SDL_FLIP_VERTICAL) ? (task->src_uv_rect.y + task->src_uv_rect.h) : task->src_uv_rect.y;
}

static float rect_task_source_axis_end(const RenderTask* task, bool horizontal) {
    if (horizontal) {
        return (task->flip & SDL_FLIP_HORIZONTAL) ? task->src_uv_rect.x : (task->src_uv_rect.x + task->src_uv_rect.w);
    }

    return (task->flip & SDL_FLIP_VERTICAL) ? task->src_uv_rect.y : (task->src_uv_rect.y + task->src_uv_rect.h);
}

static bool matching_rect_axis_scale(float dst_a, float src_a, float dst_b, float src_b) {
    return (src_a > 0.0f) && (src_b > 0.0f) && nearly_equal(dst_a * src_b, dst_b * src_a);
}

static bool rect_task_source_edges_touch(const RenderTask* prev, const RenderTask* next, bool horizontal) {
    float texture_width = 0.0f;
    float texture_height = 0.0f;
    if (!SDL_GetTextureSize(prev->texture, &texture_width, &texture_height)) {
        return false;
    }

    const float axis_texels = horizontal ? texture_width : texture_height;
    if (axis_texels <= 0.0f) {
        return false;
    }

    const float prev_edge = rect_task_source_axis_end(prev, horizontal);
    const float next_edge = rect_task_source_axis_start(next, horizontal);
    const float half_texel = 0.5f / axis_texels;
    return SDL_fabsf(prev_edge - next_edge) < half_texel;
}

static bool can_merge_rect_tasks_horizontally(const RenderTask* prev, const RenderTask* next) {
    if (!rect_tasks_have_matching_strip_basics(prev, next)) {
        return false;
    }

    return nearly_equal(prev->dst_rect.y, next->dst_rect.y) && nearly_equal(prev->dst_rect.h, next->dst_rect.h) &&
           nearly_equal(prev->src_uv_rect.y, next->src_uv_rect.y) &&
           nearly_equal(prev->src_uv_rect.h, next->src_uv_rect.h) &&
           matching_rect_axis_scale(prev->dst_rect.w, prev->src_uv_rect.w, next->dst_rect.w, next->src_uv_rect.w) &&
           nearly_equal(prev->dst_rect.x + prev->dst_rect.w, next->dst_rect.x) &&
           rect_task_source_edges_touch(prev, next, true);
}

static bool can_merge_rect_tasks_vertically(const RenderTask* prev, const RenderTask* next) {
    if (!rect_tasks_have_matching_strip_basics(prev, next)) {
        return false;
    }

    return nearly_equal(prev->dst_rect.x, next->dst_rect.x) && nearly_equal(prev->dst_rect.w, next->dst_rect.w) &&
           nearly_equal(prev->src_uv_rect.x, next->src_uv_rect.x) &&
           nearly_equal(prev->src_uv_rect.w, next->src_uv_rect.w) &&
           matching_rect_axis_scale(prev->dst_rect.h, prev->src_uv_rect.h, next->dst_rect.h, next->src_uv_rect.h) &&
           nearly_equal(prev->dst_rect.y + prev->dst_rect.h, next->dst_rect.y) &&
           rect_task_source_edges_touch(prev, next, false);
}
#endif

#if ENABLE_PERF_TELEMETRY
static void flush_rect_texture_run_stats(RectRunTelemetryState* state) {
    if (state == NULL) {
        return;
    }

    flush_rect_texture_strip_group_stats(&state->hstrip_length,
                                         &frame_stats.rect_texture_hstrip_runs,
                                         &frame_stats.rect_texture_hstrip_tasks);
    flush_rect_texture_strip_group_stats(&state->vstrip_length,
                                         &frame_stats.rect_texture_vstrip_runs,
                                         &frame_stats.rect_texture_vstrip_tasks);

    if (frame_stats_extended_enabled && (state->run_length > 0)) {
        frame_stats.rect_texture_runs += 1;
        if (state->run_length > 1) {
            frame_stats.rect_texture_multi_runs += 1;
            frame_stats.rect_texture_multi_run_tasks += state->run_length;
        }
        if (state->run_length > frame_stats.rect_texture_max_run) {
            frame_stats.rect_texture_max_run = state->run_length;
        }
    }

    state->texture = NULL;
    state->prev_task_valid = false;
    state->run_length = 0;
    state->hstrip_length = 0;
    state->vstrip_length = 0;
}
#else
static void flush_rect_texture_run_stats(RectRunTelemetryState* state) {
    (void)state;
}
#endif

#if ENABLE_PERF_TELEMETRY
static void note_rect_texture_submit(RectRunTelemetryState* state, const RenderTask* task) {
    if ((state == NULL) || (task == NULL)) {
        return;
    }

    if (!frame_stats_extended_enabled) {
        return;
    }

    if (task->flip != SDL_FLIP_NONE) {
        frame_stats.rect_texture_flipped_tasks += 1;
    }

    if ((state->run_length <= 0) || (state->texture != task->texture)) {
        flush_rect_texture_run_stats(state);
        state->texture = task->texture;
        state->prev_task = *task;
        state->prev_task_valid = true;
        state->run_length = 1;
        return;
    }

    state->run_length += 1;
    frame_stats.rect_texture_run_links += 1;

    if (state->prev_task.color != task->color) {
        frame_stats.rect_texture_color_breaks += 1;
    }
    if (state->prev_task.flip != task->flip) {
        frame_stats.rect_texture_flip_breaks += 1;
    }

    if (state->prev_task_valid && can_merge_rect_tasks_horizontally(&state->prev_task, task)) {
        state->hstrip_length = state->hstrip_length > 0 ? (state->hstrip_length + 1) : 2;
    } else {
        flush_rect_texture_strip_group_stats(&state->hstrip_length,
                                             &frame_stats.rect_texture_hstrip_runs,
                                             &frame_stats.rect_texture_hstrip_tasks);
    }

    if (state->prev_task_valid && can_merge_rect_tasks_vertically(&state->prev_task, task)) {
        state->vstrip_length = state->vstrip_length > 0 ? (state->vstrip_length + 1) : 2;
    } else {
        flush_rect_texture_strip_group_stats(&state->vstrip_length,
                                             &frame_stats.rect_texture_vstrip_runs,
                                             &frame_stats.rect_texture_vstrip_tasks);
    }

    state->prev_task = *task;
    state->prev_task_valid = true;
}
#else
static void note_rect_texture_submit(RectRunTelemetryState* state, const RenderTask* task) {
    (void)state;
    (void)task;
}
#endif

static void submit_geometry_task_range(int task_start, int run_task_count, SDL_Texture* run_texture) {
    const int vertices_per_task = SDL_arraysize(render_tasks[0].vertices);
    const int indices_per_task = SDL_arraysize(render_task_indices);
    SDL_Texture* effective_run_texture = run_texture;

    if (run_task_count > 0) {
        effective_run_texture = get_submit_texture_for_task(&render_tasks[task_start]);
    }

    if (run_task_count == 1) {
        const RenderTask* task = &render_tasks[task_start];
        SDL_RenderGeometry(_renderer,
                           effective_run_texture,
                           task->vertices,
                           vertices_per_task,
                           render_task_indices,
                           indices_per_task);
        return;
    }

    // Preserve exact task ordering by batching only contiguous same-texture runs.
    RENDERER_TELEMETRY({
        frame_stats.batch_runs += 1;
        frame_stats.batched_task_count += run_task_count;
    });
    const RenderTask* run_task = &render_tasks[task_start];
    SDL_Vertex* dst_vertices = render_task_batch_vertices;
    for (int i = 0; i < run_task_count; i++) {
        SDL_memcpy(dst_vertices, run_task->vertices, sizeof(run_task->vertices));
        dst_vertices += vertices_per_task;
        run_task += 1;
    }

    SDL_RenderGeometry(_renderer,
                       effective_run_texture,
                       render_task_batch_vertices,
                       run_task_count * vertices_per_task,
                       render_task_batch_indices,
                       run_task_count * indices_per_task);
}

static void submit_render_tasks(void) {
    int task_start = 0;
    RectRunTelemetryState rect_run_state = { 0 };

    while (task_start < render_task_count) {
        const RenderTask* task = &render_tasks[task_start];

        if (task->type == RENDER_TASK_TYPE_TEXTURED_RECT) {
            if (submit_rect_task(task)) {
                if (frame_stats_extended_enabled) {
                    note_hybrid_eligibility(task);
                }
                note_rect_texture_submit(&rect_run_state, task);
            } else {
                if (frame_stats_extended_enabled) {
                    RenderTask submitted_geometry_task = *task;
                    submitted_geometry_task.type = RENDER_TASK_TYPE_GEOMETRY;
                    note_hybrid_eligibility(&submitted_geometry_task);
                }
                flush_rect_texture_run_stats(&rect_run_state);
            }
            task_start += 1;
            continue;
        }
        bool used_rect_path = false;
        RenderTask submitted_rect_task;
        if (try_submit_geometry_task_as_rect_copy(task, &used_rect_path, &submitted_rect_task)) {
            if (frame_stats_extended_enabled && (task->texture != NULL)) {
                frame_stats.textured_geometry_tasks += 1;
            }
            if (used_rect_path) {
                if (frame_stats_extended_enabled) {
                    frame_stats.textured_geometry_rect_recovered_tasks += 1;
                    note_hybrid_eligibility(&submitted_rect_task);
                }
                note_rect_texture_submit(&rect_run_state, &submitted_rect_task);
            } else {
                if (frame_stats_extended_enabled && (task->texture != NULL)) {
                    frame_stats.textured_geometry_fallback_tasks += 1;
                    note_hybrid_eligibility(task);
                }
                flush_rect_texture_run_stats(&rect_run_state);
            }
            task_start += 1;
            continue;
        }

        flush_rect_texture_run_stats(&rect_run_state);
        SDL_Texture* run_texture = task->texture;
        int run_task_count = 1;
        while (((task_start + run_task_count) < render_task_count) &&
               (render_tasks[task_start + run_task_count].type == RENDER_TASK_TYPE_GEOMETRY) &&
               (render_tasks[task_start + run_task_count].texture == run_texture)) {
            run_task_count += 1;
        }

        if (frame_stats_extended_enabled && (run_texture != NULL)) {
            frame_stats.textured_geometry_tasks += run_task_count;
            frame_stats.textured_geometry_fallback_tasks += run_task_count;
        }
        if (frame_stats_extended_enabled) {
            for (int i = 0; i < run_task_count; i++) {
                note_hybrid_eligibility(&render_tasks[task_start + i]);
            }
        }
        submit_geometry_task_range(task_start, run_task_count, run_texture);
        task_start += run_task_count;
    }

    flush_rect_texture_run_stats(&rect_run_state);
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

static void clear_tile_map(Uint8* tile_map) {
    SDL_memset(tile_map, 0, dirty_tile_total);
}

static void fill_tile_map(Uint8* tile_map) {
    SDL_memset(tile_map, 1, dirty_tile_total);
}

static void mark_dirty_tiles_for_bounds(float min_x, float min_y, float max_x, float max_y) {
    const int px0 = clamp_to_range((int)SDL_floorf(min_x), 0, cps3_width);
    const int py0 = clamp_to_range((int)SDL_floorf(min_y), 0, cps3_height);
    const int px1 = clamp_to_range((int)SDL_ceilf(max_x), 0, cps3_width);
    const int py1 = clamp_to_range((int)SDL_ceilf(max_y), 0, cps3_height);

    if ((px1 <= px0) || (py1 <= py0)) {
        return;
    }

    const int tx0 = px0 / dirty_tile_size;
    const int ty0 = py0 / dirty_tile_size;
    const int tx1 = (px1 - 1) / dirty_tile_size;
    const int ty1 = (py1 - 1) / dirty_tile_size;

    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            const int tile_index = (ty * dirty_tile_cols) + tx;
            if (!current_coverage_tile_map[tile_index]) {
                current_coverage_tile_map[tile_index] = 1;
                current_coverage_tile_count += 1;
            }
            if (!dirty_tile_map[tile_index]) {
                dirty_tile_map[tile_index] = 1;
                dirty_tile_count += 1;
            }
        }
    }
}

static bool nearly_equal(float a, float b) {
    return SDL_fabsf(a - b) <= rect_task_epsilon;
}

static void mark_dirty_tiles_for_task(const RenderTask* task) {
    if (task->type == RENDER_TASK_TYPE_TEXTURED_RECT) {
        mark_dirty_tiles_for_bounds(task->dst_rect.x,
                                    task->dst_rect.y,
                                    task->dst_rect.x + task->dst_rect.w,
                                    task->dst_rect.y + task->dst_rect.h);
        return;
    }

    float min_x = task->vertices[0].position.x;
    float max_x = min_x;
    float min_y = task->vertices[0].position.y;
    float max_y = min_y;

    for (int i = 1; i < 4; i++) {
        const float x = task->vertices[i].position.x;
        const float y = task->vertices[i].position.y;
        if (x < min_x) {
            min_x = x;
        }
        if (x > max_x) {
            max_x = x;
        }
        if (y < min_y) {
            min_y = y;
        }
        if (y > max_y) {
            max_y = y;
        }
    }

    mark_dirty_tiles_for_bounds(min_x, min_y, max_x, max_y);
}

// Colors

#define clut_shuf(x) (((x) & ~0x18) | ((((x) & 0x08) << 1) | (((x) & 0x10) >> 1)))

static void read_rgba32_color(Uint32 pixel, SDL_Color* color) {
    color->b = pixel & 0xFF;
    color->g = (pixel >> 8) & 0xFF;
    color->r = (pixel >> 16) & 0xFF;
    color->a = (pixel >> 24) & 0xFF;
}

static void read_rgba32_fcolor(Uint32 pixel, SDL_FColor* fcolor) {
    if (rgba32_fcolor_cache_valid && (pixel == rgba32_fcolor_cache_pixel)) {
        *fcolor = rgba32_fcolor_cache_value;
        return;
    }

    fcolor->b = rgba8_to_float[pixel & 0xFF];
    fcolor->g = rgba8_to_float[(pixel >> 8) & 0xFF];
    fcolor->r = rgba8_to_float[(pixel >> 16) & 0xFF];
    fcolor->a = rgba8_to_float[(pixel >> 24) & 0xFF];

    rgba32_fcolor_cache_pixel = pixel;
    rgba32_fcolor_cache_value = *fcolor;
    rgba32_fcolor_cache_valid = true;
}

static void read_rgba16_color(Uint16 pixel, SDL_Color* color) {
    color->r = (pixel & 0x1F) * 255 / 31;
    color->g = ((pixel >> 5) & 0x1F) * 255 / 31;
    color->b = ((pixel >> 10) & 0x1F) * 255 / 31;
    color->a = (pixel & 0x8000) ? 255 : 0;
}

static void read_color(void* pixels, int index, size_t color_size, SDL_Color* color) {
    switch (color_size) {
    case 2: {
        const Uint16* rgba16_colors = (Uint16*)pixels;
        read_rgba16_color(rgba16_colors[index], color);
        break;
    }

    case 4: {
        const Uint32* rgba32_colors = (Uint32*)pixels;
        read_rgba32_color(rgba32_colors[index], color);
        break;
    }
    }
}

static void fill_palette_colors_from_fl_texture(const FLTexture* fl_palette, SDL_Color* colors, int* out_color_count) {
    const void* pixels = flPS2GetSystemBuffAdrs(fl_palette->mem_handle);
    const int color_count = fl_palette->width * fl_palette->height;
    size_t color_size = 0;

    switch (fl_palette->format) {
    case SCE_GS_PSMCT32:
        color_size = 4;
        break;

    case SCE_GS_PSMCT16:
        color_size = 2;
        break;

    default:
        fatal_error("Unhandled pixel format: %d", fl_palette->format);
        break;
    }

    switch (color_count) {
    case 16:
        for (int i = 0; i < 16; i++) {
            read_color((void*)pixels, i, color_size, &colors[i]);
        }

        break;

    case 256:
        for (int i = 0; i < 256; i++) {
            const int color_index = clut_shuf(i);
            read_color((void*)pixels, color_index, color_size, &colors[i]);
        }

        break;

    default:
        fatal_error("Unhandled palette dimensions: %dx%d", fl_palette->width, fl_palette->height);
        break;
    }

    if (out_color_count != NULL) {
        *out_color_count = color_count;
    }
}

#define LERP_FLOAT(a, b, x) ((a) * (1 - (x)) + (b) * (x))

static void lerp_fcolors(SDL_FColor* dest, const SDL_FColor* a, const SDL_FColor* b, float x) {
    dest->r = LERP_FLOAT(a->r, b->r, x);
    dest->g = LERP_FLOAT(a->g, b->g, x);
    dest->b = LERP_FLOAT(a->b, b->b, x);
    dest->a = LERP_FLOAT(a->a, b->a, x);
}

// Lifecycle

void SDLGameRenderer_Init(SDL_Renderer* renderer) {
    _renderer = renderer;
    if (!render_task_batch_indices_initialized) {
        initialize_render_task_batch_indices();
    }

    for (int i = 0; i < SDL_arraysize(rgba8_to_float); i++) {
        rgba8_to_float[i] = (float)i / 255.0f;
    }

    cps3_canvas =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, cps3_width, cps3_height);
    SDL_SetTextureScaleMode(cps3_canvas, SDL_SCALEMODE_NEAREST);
}

void SDLGameRenderer_SetSoftwareFrameMode(bool enabled) {
    software_frame_mode_active = enabled;
    if (software_frame_mode_active) {
        ensure_software_frame_surface();
        ensure_software_frame_upload_texture();
    } else {
        for (int texture_index = 0; texture_index < FL_TEXTURE_MAX; texture_index++) {
            for (int palette_handle = 0; palette_handle < FL_PALETTE_MAX + 1; palette_handle++) {
                if (!texture_cache_refresh_pending[texture_index][palette_handle]) {
                    continue;
                }
                if (texture_cache[texture_index][palette_handle] != NULL) {
                    SDL_DestroyTexture(texture_cache[texture_index][palette_handle]);
                    texture_cache[texture_index][palette_handle] = NULL;
                }
                texture_cache_refresh_pending[texture_index][palette_handle] = false;
                texture_cache_runtime_dirty_reason[texture_index][palette_handle] = CACHE_DIRTY_REASON_NONE;
                clear_cache_dirty_state(&texture_cache_dirty_state[texture_index][palette_handle]);
            }
        }
        current_texture = NULL;
        current_software_source_surface = NULL;
        current_texture_binding_valid = false;
        current_texture_binding = 0;
    }
}

void SDLGameRenderer_SetSoftwareFrameDirectPresentMode(bool enabled) {
    software_frame_direct_present_requested = enabled;
}

bool SDLGameRenderer_IsSoftwareFrameModeEnabled(void) {
    return software_frame_mode_active;
}

bool SDLGameRenderer_HasSoftwareOwnedFrame(void) {
    return software_frame_owned;
}

const SDL_Surface* SDLGameRenderer_GetSoftwareFrameSurface(void) {
    return software_frame_surface;
}

bool SDLGameRenderer_EnsureSoftwareFrameCanvas(void) {
    return ensure_software_frame_canvas_for_frame();
}

void SDLGameRenderer_NoteSoftwareFrameDirectPresent(void) {
    if (software_frame_owned) {
        RENDERER_TELEMETRY({
            frame_stats.software_frame_direct_present = 1;
            frame_stats.software_frame_fallback = 0;
        });
    }
}

void SDLGameRenderer_BeginFrame(bool capture_extended_stats) {
#if ENABLE_PERF_TELEMETRY
    frame_stats_extended_enabled = capture_extended_stats;
    begin_cache_dirty_tracking_frame(capture_extended_stats);
    begin_software_surface_refresh_tracking_frame(capture_extended_stats);
#else
    (void)capture_extended_stats;
#endif
    reset_frame_stats();
    software_frame_direct_present_requested = false;
#if ENABLE_PERF_TELEMETRY
    current_task_source = SDL_GAME_RENDERER_TASK_SOURCE_UNKNOWN;
#endif
    software_frame_surface_ready = false;
    software_frame_owned = false;
    software_frame_uploaded = false;
    clear_tile_map(current_coverage_tile_map);
    current_coverage_tile_count = 0;
    RENDERER_TELEMETRY({
        frame_stats.software_frame_mode_enabled = software_frame_mode_active ? 1 : 0;
        frame_stats.software_frame_fallback = software_frame_mode_active ? 1 : 0;
    });

    if (!previous_frame_clear_color_valid || (previous_frame_clear_color != flPs2State.FrameClearColor)) {
        fill_tile_map(dirty_tile_map);
        dirty_tile_count = dirty_tile_total;
    } else {
        SDL_memcpy(dirty_tile_map, previous_coverage_tile_map, dirty_tile_total);
        dirty_tile_count = previous_coverage_tile_count;
    }

    // Clear canvas
    const Uint8 r = (flPs2State.FrameClearColor >> 16) & 0xFF;
    const Uint8 g = (flPs2State.FrameClearColor >> 8) & 0xFF;
    const Uint8 b = flPs2State.FrameClearColor & 0xFF;
    const Uint8 a = flPs2State.FrameClearColor >> 24;

    if (software_frame_mode_active && ensure_software_frame_surface()) {
        const Uint8 clear_r = a != SDL_ALPHA_TRANSPARENT ? r : 0;
        const Uint8 clear_g = a != SDL_ALPHA_TRANSPARENT ? g : 0;
        const Uint8 clear_b = a != SDL_ALPHA_TRANSPARENT ? b : 0;
        const Uint8 clear_a = a != SDL_ALPHA_TRANSPARENT ? a : SDL_ALPHA_OPAQUE;
        const Uint32 clear_pixel =
            SDL_MapSurfaceRGBA(software_frame_surface, clear_r, clear_g, clear_b, clear_a);
        SDL_FillSurfaceRect(software_frame_surface, NULL, clear_pixel);
        software_frame_surface_ready = true;
        RENDERER_TELEMETRY(frame_stats.software_frame_surface_ready = 1);
    }

    if (a != SDL_ALPHA_TRANSPARENT) {
        SDL_SetRenderDrawColor(_renderer, r, g, b, a);
    } else {
        SDL_SetRenderDrawColor(_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    }

    cps3_target_bound = SDL_SetRenderTarget(_renderer, cps3_canvas);
    SDL_RenderClear(_renderer);
}

void SDLGameRenderer_RenderFrame() {
    if (!cps3_target_bound || SDLMessageRenderer_HasContent()) {
        // Message rendering can switch to the message canvas; restore the game canvas only when needed.
        SDLMessageRenderer_InvalidateTargetBindCache();
        cps3_target_bound = SDL_SetRenderTarget(_renderer, cps3_canvas);
    }

    if ((render_task_count > 1) && render_tasks_have_z_inversion) {
        if ((render_task_count <= render_task_insertion_sort_max_tasks) &&
            (render_tasks_z_inversion_count <= render_task_insertion_sort_max_inversions)) {
            RENDERER_TELEMETRY(frame_stats.sort_strategy = SDL_GAME_RENDERER_SORT_INSERTION);
            insertion_sort_render_tasks();
        } else {
            RENDERER_TELEMETRY(frame_stats.sort_strategy = SDL_GAME_RENDERER_SORT_QSORT);
            qsort(render_tasks, render_task_count, sizeof(RenderTask), compare_render_tasks);
        }
    }

    if (software_frame_mode_active && render_frame_to_software_surface()) {
        software_frame_owned = true;
        RENDERER_TELEMETRY({
            frame_stats.software_frame_owned = 1;
            frame_stats.software_frame_fallback = 0;
        });
        if (!software_frame_direct_present_requested) {
            ensure_software_frame_canvas_for_frame();
        }
    } else {
        submit_render_tasks();
    }

    if (draw_rect_borders) {
        const SDL_FColor red = { .r = 1, .g = 0, .b = 0, .a = SDL_ALPHA_OPAQUE_FLOAT };
        const SDL_FColor green = { .r = 0, .g = 1, .b = 0, .a = SDL_ALPHA_OPAQUE_FLOAT };
        SDL_FColor border_color;

        for (int i = 0; i < render_task_count; i++) {
            const RenderTask* task = &render_tasks[i];
            const float x0 =
                task->type == RENDER_TASK_TYPE_TEXTURED_RECT ? task->dst_rect.x : task->vertices[0].position.x;
            const float y0 =
                task->type == RENDER_TASK_TYPE_TEXTURED_RECT ? task->dst_rect.y : task->vertices[0].position.y;
            const float x1 = task->type == RENDER_TASK_TYPE_TEXTURED_RECT ? (task->dst_rect.x + task->dst_rect.w)
                                                                           : task->vertices[3].position.x;
            const float y1 = task->type == RENDER_TASK_TYPE_TEXTURED_RECT ? (task->dst_rect.y + task->dst_rect.h)
                                                                           : task->vertices[3].position.y;
            const SDL_FRect border_rect = { .x = x0, .y = y0, .w = (x1 - x0), .h = (y1 - y0) };

            const float lerp_factor = (float)i / (float)(render_task_count - 1);
            lerp_fcolors(&border_color, &red, &green, lerp_factor);

            SDL_SetRenderDrawColorFloat(_renderer, border_color.r, border_color.g, border_color.b, border_color.a);
            SDL_RenderRect(_renderer, &border_rect);
        }
    }
}

void SDLGameRenderer_EndFrame() {
    SDL_memcpy(previous_coverage_tile_map, current_coverage_tile_map, dirty_tile_total);
    previous_coverage_tile_count = current_coverage_tile_count;
    previous_frame_clear_color = flPs2State.FrameClearColor;
    previous_frame_clear_color_valid = true;

    cps3_target_bound = false;
    destroy_textures();
    clear_render_tasks();
}

void SDLGameRenderer_ResetPerfCaptureRefreshTelemetry(void) {
    SDL_zero(perf_capture_refresh_telemetry);
    SDL_zero(perf_capture_refresh_attempts_by_texture);
    SDL_zero(perf_capture_refresh_pixels_by_texture);
    SDL_zero(perf_capture_refresh_fanout_max_by_texture);
    SDL_zero(perf_capture_refresh_source_format_by_texture);
    SDL_zero(perf_capture_refresh_width_by_texture);
    SDL_zero(perf_capture_refresh_height_by_texture);
    SDL_zero(perf_capture_refresh_shape_mixed_by_texture);
}

void SDLGameRenderer_ResetPerfCaptureUnlockLocalityTelemetry(void) {
    SDL_zero(perf_capture_unlock_locality_telemetry);
    SDL_zero(perf_capture_unlock_locality_tracked_by_texture);
    SDL_zero(perf_capture_unlock_locality_baseline_skips_by_texture);
    SDL_zero(perf_capture_unlock_locality_non_index8_skips_by_texture);
    SDL_zero(perf_capture_unlock_locality_source_pixels_by_texture);
    SDL_zero(perf_capture_unlock_locality_changed_pixels_by_texture);
    SDL_zero(perf_capture_unlock_locality_changed_rows_by_texture);
    SDL_zero(perf_capture_unlock_locality_changed_bbox_pixels_by_texture);
    SDL_zero(perf_capture_unlock_locality_source_format_by_texture);
    SDL_zero(perf_capture_unlock_locality_width_by_texture);
    SDL_zero(perf_capture_unlock_locality_height_by_texture);
    SDL_zero(perf_capture_unlock_locality_shape_mixed_by_texture);
    for (int texture_index = 0; texture_index < FL_TEXTURE_MAX; texture_index++) {
        reset_perf_capture_unlock_locality_texture_slot(texture_index);
    }
}

int SDLGameRenderer_GetPerfCaptureRefreshTelemetry(SDLGameRenderer_PerfCaptureRefreshTelemetry* out_telemetry,
                                                   SDLGameRenderer_PerfCaptureRefreshHotTexture* out_hot_textures,
                                                   int max_hot_textures) {
    if (out_telemetry != NULL) {
        *out_telemetry = perf_capture_refresh_telemetry;
    }

    if ((out_hot_textures == NULL) || (max_hot_textures <= 0)) {
        return 0;
    }

    int hot_texture_count = 0;
    for (int slot = 0; slot < max_hot_textures; slot++) {
        int best_texture_index = -1;
        for (int texture_index = 0; texture_index < FL_TEXTURE_MAX; texture_index++) {
            const Uint64 refresh_attempts = perf_capture_refresh_attempts_by_texture[texture_index];
            if (refresh_attempts == 0) {
                continue;
            }

            bool already_selected = false;
            for (int existing = 0; existing < hot_texture_count; existing++) {
                if (out_hot_textures[existing].texture_handle == (texture_index + 1)) {
                    already_selected = true;
                    break;
                }
            }
            if (already_selected) {
                continue;
            }

            if (best_texture_index < 0) {
                best_texture_index = texture_index;
                continue;
            }

            const Uint64 best_attempts = perf_capture_refresh_attempts_by_texture[best_texture_index];
            if ((refresh_attempts > best_attempts) ||
                ((refresh_attempts == best_attempts) &&
                 (perf_capture_refresh_pixels_by_texture[texture_index] >
                  perf_capture_refresh_pixels_by_texture[best_texture_index])) ||
                ((refresh_attempts == best_attempts) &&
                 (perf_capture_refresh_pixels_by_texture[texture_index] ==
                  perf_capture_refresh_pixels_by_texture[best_texture_index]) &&
                 (texture_index < best_texture_index))) {
                best_texture_index = texture_index;
            }
        }

        if (best_texture_index < 0) {
            break;
        }

        SDLGameRenderer_PerfCaptureRefreshHotTexture* entry = &out_hot_textures[hot_texture_count];
        entry->texture_handle = best_texture_index + 1;
        entry->source_shape_mixed = perf_capture_refresh_shape_mixed_by_texture[best_texture_index] ? 1 : 0;
        entry->source_format =
            entry->source_shape_mixed ? SDL_PIXELFORMAT_UNKNOWN : perf_capture_refresh_source_format_by_texture[best_texture_index];
        entry->width = entry->source_shape_mixed ? 0 : (int)perf_capture_refresh_width_by_texture[best_texture_index];
        entry->height = entry->source_shape_mixed ? 0 : (int)perf_capture_refresh_height_by_texture[best_texture_index];
        entry->refresh_attempts = perf_capture_refresh_attempts_by_texture[best_texture_index];
        entry->refresh_pixels = perf_capture_refresh_pixels_by_texture[best_texture_index];
        entry->max_fanout = (int)perf_capture_refresh_fanout_max_by_texture[best_texture_index];
        hot_texture_count += 1;
    }

    return hot_texture_count;
}

int SDLGameRenderer_GetPerfCaptureUnlockLocalityTelemetry(
    SDLGameRenderer_PerfCaptureUnlockLocalityTelemetry* out_telemetry,
    SDLGameRenderer_PerfCaptureUnlockLocalityHotTexture* out_hot_textures,
    int max_hot_textures) {
    if (out_telemetry != NULL) {
        *out_telemetry = perf_capture_unlock_locality_telemetry;
    }

    if ((out_hot_textures == NULL) || (max_hot_textures <= 0)) {
        return 0;
    }

    int hot_texture_count = 0;
    for (int slot = 0; slot < max_hot_textures; slot++) {
        int best_texture_index = -1;
        for (int texture_index = 0; texture_index < FL_TEXTURE_MAX; texture_index++) {
            const Uint64 tracked_unlocks = perf_capture_unlock_locality_tracked_by_texture[texture_index];
            if (tracked_unlocks == 0) {
                continue;
            }

            bool already_selected = false;
            for (int existing = 0; existing < hot_texture_count; existing++) {
                if (out_hot_textures[existing].texture_handle == (texture_index + 1)) {
                    already_selected = true;
                    break;
                }
            }
            if (already_selected) {
                continue;
            }

            if (best_texture_index < 0) {
                best_texture_index = texture_index;
                continue;
            }

            const Uint64 changed_pixels = perf_capture_unlock_locality_changed_pixels_by_texture[texture_index];
            const Uint64 best_changed_pixels = perf_capture_unlock_locality_changed_pixels_by_texture[best_texture_index];
            if ((changed_pixels > best_changed_pixels) ||
                ((changed_pixels == best_changed_pixels) &&
                 (tracked_unlocks > perf_capture_unlock_locality_tracked_by_texture[best_texture_index])) ||
                ((changed_pixels == best_changed_pixels) &&
                 (tracked_unlocks == perf_capture_unlock_locality_tracked_by_texture[best_texture_index]) &&
                 (texture_index < best_texture_index))) {
                best_texture_index = texture_index;
            }
        }

        if (best_texture_index < 0) {
            break;
        }

        SDLGameRenderer_PerfCaptureUnlockLocalityHotTexture* entry = &out_hot_textures[hot_texture_count];
        entry->texture_handle = best_texture_index + 1;
        entry->source_shape_mixed = perf_capture_unlock_locality_shape_mixed_by_texture[best_texture_index] ? 1 : 0;
        entry->source_format = entry->source_shape_mixed
                                   ? SDL_PIXELFORMAT_UNKNOWN
                                   : perf_capture_unlock_locality_source_format_by_texture[best_texture_index];
        entry->width =
            entry->source_shape_mixed ? 0 : perf_capture_unlock_locality_width_by_texture[best_texture_index];
        entry->height =
            entry->source_shape_mixed ? 0 : perf_capture_unlock_locality_height_by_texture[best_texture_index];
        entry->tracked_unlocks = perf_capture_unlock_locality_tracked_by_texture[best_texture_index];
        entry->baseline_skips = perf_capture_unlock_locality_baseline_skips_by_texture[best_texture_index];
        entry->non_index8_skips = perf_capture_unlock_locality_non_index8_skips_by_texture[best_texture_index];
        entry->source_pixels = perf_capture_unlock_locality_source_pixels_by_texture[best_texture_index];
        entry->changed_pixels = perf_capture_unlock_locality_changed_pixels_by_texture[best_texture_index];
        entry->changed_rows = perf_capture_unlock_locality_changed_rows_by_texture[best_texture_index];
        entry->changed_bbox_pixels = perf_capture_unlock_locality_changed_bbox_pixels_by_texture[best_texture_index];
        hot_texture_count += 1;
    }

    return hot_texture_count;
}

int SDLGameRenderer_GetDirtyTileCount(void) {
    return dirty_tile_count;
}

int SDLGameRenderer_GetDirtyTileTotal(void) {
    return dirty_tile_total;
}

const Uint8* SDLGameRenderer_GetDirtyTileMap(int* out_cols, int* out_rows, int* out_tile_size) {
    if (out_cols != NULL) {
        *out_cols = dirty_tile_cols;
    }
    if (out_rows != NULL) {
        *out_rows = dirty_tile_rows;
    }
    if (out_tile_size != NULL) {
        *out_tile_size = dirty_tile_size;
    }

    return dirty_tile_map;
}

void SDLGameRenderer_GetFrameStats(SDLGameRenderer_FrameStats* out_stats) {
    if (out_stats == NULL) {
        return;
    }

    *out_stats = frame_stats;
    out_stats->render_task_count = render_task_count;
}

void SDLGameRenderer_RecordTextureUnlockDirtyRect(unsigned int texture_handle,
                                                  int min_x,
                                                  int min_y,
                                                  int max_x,
                                                  int max_y) {
    if ((texture_handle == 0) || (texture_handle > FL_TEXTURE_MAX) || (min_x > max_x) || (min_y > max_y)) {
        return;
    }

    const int texture_index = (int)texture_handle - 1;
    clear_texture_unlock_dirty_rect_if_unused(texture_index);

    SDL_Rect dirty_rect = { min_x, min_y, max_x - min_x + 1, max_y - min_y + 1 };
    if ((dirty_rect.w <= 0) || (dirty_rect.h <= 0)) {
        return;
    }

    if (!texture_unlock_dirty_rect_valid[texture_index]) {
        texture_unlock_dirty_rects[texture_index] = dirty_rect;
        texture_unlock_dirty_rect_valid[texture_index] = true;
        return;
    }

    SDL_Rect* accumulated_rect = &texture_unlock_dirty_rects[texture_index];
    const int union_x0 = SDL_min(accumulated_rect->x, dirty_rect.x);
    const int union_y0 = SDL_min(accumulated_rect->y, dirty_rect.y);
    const int union_x1 = SDL_max(accumulated_rect->x + accumulated_rect->w - 1, dirty_rect.x + dirty_rect.w - 1);
    const int union_y1 = SDL_max(accumulated_rect->y + accumulated_rect->h - 1, dirty_rect.y + dirty_rect.h - 1);
    accumulated_rect->x = union_x0;
    accumulated_rect->y = union_y0;
    accumulated_rect->w = union_x1 - union_x0 + 1;
    accumulated_rect->h = union_y1 - union_y0 + 1;
}

void SDLGameRenderer_ClearTextureUnlockDirtyRect(unsigned int texture_handle) {
    if ((texture_handle == 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return;
    }

    clear_texture_unlock_dirty_rect_index((int)texture_handle - 1);
}

void SDLGameRenderer_UnlockPalette(unsigned int ph) {
    const int palette_handle = ph;
    const int palette_index = palette_handle - 1;
    const CacheDirtyReason dirty_reason = CACHE_DIRTY_REASON_PALETTE_UNLOCK;

    if ((palette_handle > 0) && (palette_handle <= FL_PALETTE_MAX)) {
        if (palettes[palette_index] == NULL) {
            SDLGameRenderer_CreatePalette(ph << 16);
        } else {
            SDL_Color colors[256];
            int color_count = 0;
            fill_palette_colors_from_fl_texture(&flPalette[palette_index], colors, &color_count);
            SDL_SetPaletteColors(palettes[palette_index], colors, 0, color_count);
        }
        if (frame_stats_extended_enabled) {
            const Uint64 invalidation_start_ns = SDL_GetTicksNS();
            frame_stats.palette_unlock_calls += 1;
            frame_stats.palette_cache_evictions +=
                invalidate_texture_cache_for_palette_handle(palette_handle, dirty_reason);
            frame_stats.software_surface_cache_palette_evictions +=
                invalidate_software_surface_cache_for_palette_handle(palette_handle, dirty_reason);
            frame_stats.palette_unlock_invalidation_ns += SDL_GetTicksNS() - invalidation_start_ns;
        } else {
            invalidate_texture_cache_for_palette_handle(palette_handle, dirty_reason);
            invalidate_software_surface_cache_for_palette_handle(palette_handle, dirty_reason);
        }
    }
}

void SDLGameRenderer_UnlockTexture(unsigned int th) {
    const int texture_handle = th;
    const int texture_index = texture_handle - 1;
    const CacheDirtyReason dirty_reason = CACHE_DIRTY_REASON_TEXTURE_UNLOCK;

    if ((texture_handle > 0) && (texture_handle <= FL_TEXTURE_MAX)) {
        if (surfaces[texture_index] == NULL) {
            SDLGameRenderer_CreateTexture(th);
            return;
        }

        note_perf_capture_texture_unlock_locality(texture_handle, surfaces[texture_index]);

        if (frame_stats_extended_enabled) {
            const Uint64 invalidation_start_ns = SDL_GetTicksNS();
            frame_stats.texture_unlock_calls += 1;
            frame_stats.texture_cache_evictions +=
                invalidate_texture_cache_for_texture_index(texture_index, dirty_reason);
            frame_stats.software_surface_cache_texture_evictions +=
                invalidate_software_surface_cache_for_texture_index(texture_index, dirty_reason);
            frame_stats.texture_unlock_invalidation_ns += SDL_GetTicksNS() - invalidation_start_ns;
        } else {
            invalidate_texture_cache_for_texture_index(texture_index, dirty_reason);
            invalidate_software_surface_cache_for_texture_index(texture_index, dirty_reason);
        }
        clear_texture_unlock_dirty_rect_if_unused(texture_index);
    }
}

void SDLGameRenderer_CreateTexture(unsigned int th) {
    const int texture_index = LO_16_BITS(th) - 1;
    const FLTexture* fl_texture = &flTexture[texture_index];
    const void* pixels = flPS2GetSystemBuffAdrs(fl_texture->mem_handle);
    SDL_PixelFormat pixel_format = SDL_PIXELFORMAT_UNKNOWN;
    int pitch = 0;

    if (surfaces[texture_index] != NULL) {
        fatal_error("Overwriting an existing texture");
    }

    switch (fl_texture->format) {
    case SCE_GS_PSMT8:
        pixel_format = SDL_PIXELFORMAT_INDEX8;
        pitch = fl_texture->width;
        break;

    case SCE_GS_PSMT4:
        pixel_format = SDL_PIXELFORMAT_INDEX4LSB;
        pitch = fl_texture->width / 2;
        break;

    case SCE_GS_PSMCT16:
        pixel_format = SDL_PIXELFORMAT_ABGR1555;
        pitch = fl_texture->width * 2;
        break;

    default:
        fatal_error("Unhandled pixel format: %d", fl_texture->format);
        break;
    }

    const SDL_Surface* surface =
        SDL_CreateSurfaceFrom(fl_texture->width, fl_texture->height, pixel_format, pixels, pitch);
    surfaces[texture_index] = surface;
    clear_texture_unlock_dirty_rect_index(texture_index);
    reset_perf_capture_unlock_locality_texture_slot(texture_index);
}

void SDLGameRenderer_DestroyTexture(unsigned int texture_handle) {
    const int texture_index = texture_handle - 1;
    invalidate_texture_cache_for_texture_index(texture_index, CACHE_DIRTY_REASON_NONE);
    invalidate_software_surface_cache_for_texture_index(texture_index, CACHE_DIRTY_REASON_NONE);
    clear_texture_unlock_dirty_rect_index(texture_index);
    reset_perf_capture_unlock_locality_texture_slot(texture_index);

    SDL_DestroySurface(surfaces[texture_index]);
    surfaces[texture_index] = NULL;
}

void SDLGameRenderer_CreatePalette(unsigned int ph) {
    const int palette_index = HI_16_BITS(ph) - 1;
    const FLTexture* fl_palette = &flPalette[palette_index];
    SDL_Color colors[256];
    int color_count = 0;

    if (palettes[palette_index] != NULL) {
        fatal_error("Overwriting an existing palette");
    }
    fill_palette_colors_from_fl_texture(fl_palette, colors, &color_count);

    SDL_Palette* palette = SDL_CreatePalette(color_count);
    SDL_SetPaletteColors(palette, colors, 0, color_count);
    palettes[palette_index] = palette;
}

void SDLGameRenderer_DestroyPalette(unsigned int palette_handle) {
    const int palette_index = palette_handle - 1;
    invalidate_texture_cache_for_palette_handle((int)palette_handle, CACHE_DIRTY_REASON_NONE);
    invalidate_software_surface_cache_for_palette_handle((int)palette_handle, CACHE_DIRTY_REASON_NONE);

    SDL_DestroyPalette(palettes[palette_index]);
    palettes[palette_index] = NULL;
}

void SDLGameRenderer_SetTexture(unsigned int th) {
    const int texture_handle = LO_16_BITS(th);
    const SDL_Surface* surface = surfaces[texture_handle - 1];
    const int palette_handle = HI_16_BITS(th);
    const SDL_Palette* palette = palette_handle != 0 ? palettes[palette_handle - 1] : NULL;
    CacheDirtyState* dirty_state = &texture_cache_dirty_state[texture_handle - 1][palette_handle];
    Uint8* runtime_dirty_reason_p = &texture_cache_runtime_dirty_reason[texture_handle - 1][palette_handle];
    SDL_Surface* software_source_surface = NULL;
    bool miss_counted = false;

    RENDERER_TELEMETRY({
        if (frame_stats_extended_enabled) {
            frame_stats.set_texture_calls += 1;
        }
    });

    if (dump_textures) {
        save_texture(surface, palette);
    }

    if (current_texture_binding_valid && (current_texture_binding == th) && (current_texture != NULL) &&
        (!software_frame_mode_active || (current_software_source_surface != NULL))) {
        RENDERER_TELEMETRY({
            if (frame_stats_extended_enabled) {
                frame_stats.texture_binding_reuse_hits += 1;
            }
        });
        return;
    }

    SDL_Texture* texture = NULL;
    SDL_Texture* cached_texture = texture_cache[texture_handle - 1][palette_handle];

    if (cached_texture != NULL) {
        if (should_refresh_dirty_cache_entry(*runtime_dirty_reason_p)) {
            RENDERER_TELEMETRY({
                if (frame_stats_extended_enabled) {
                    frame_stats.texture_cache_misses += 1;
                    note_texture_cache_miss_provenance(dirty_state);
                }
            });
            miss_counted = true;
            // The software-frame raster uses the cached source surface directly, so defer SDL texture refresh
            // until the SDL submission path actually needs this cache entry.
            texture = cached_texture;
            texture_cache_refresh_pending[texture_handle - 1][palette_handle] = true;
            *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
            clear_cache_dirty_state(dirty_state);
        } else {
            texture = cached_texture;
            clear_cache_dirty_state(dirty_state);
            RENDERER_TELEMETRY(frame_stats.texture_cache_hits += 1);
        }
    }

    if (texture == NULL) {
        if (!miss_counted) {
            RENDERER_TELEMETRY({
                frame_stats.texture_cache_misses += 1;
                note_texture_cache_miss_provenance(dirty_state);
            });
        }
        if (palette != NULL) {
            // Palette only impacts CPU surface -> texture conversion; skip this work on cache hits.
            SDL_SetSurfacePalette(surface, palette);
        }

        texture = SDL_CreateTextureFromSurface(_renderer, surface);
        if (texture == NULL) {
            fatal_error("Failed to create SDL texture for handle 0x%08X: %s", th, SDL_GetError());
        }
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        texture_cache[texture_handle - 1][palette_handle] = texture;
        *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
        clear_cache_dirty_state(dirty_state);
        RENDERER_TELEMETRY(frame_stats.texture_creates += 1);
    }

    push_texture(texture);
    current_software_source_surface =
        software_frame_mode_active ? (software_source_surface != NULL ? software_source_surface
                                                                      : get_or_create_software_source_surface(th))
                                   : NULL;
    current_texture_binding = th;
    current_texture_binding_valid = true;
}

void SDLGameRenderer_SetTaskSource(SDLGameRenderer_TaskSource source) {
#if ENABLE_PERF_TELEMETRY
    current_task_source = source;
#else
    (void)source;
#endif
}

static RenderTask* begin_quad_task(bool textured, float z) {
    RenderTask* task = &render_tasks[render_task_count];
    task->index = render_task_count;
    task->texture = textured ? get_texture() : NULL;
    task->type = RENDER_TASK_TYPE_GEOMETRY;
#if ENABLE_PERF_TELEMETRY
    task->source = current_task_source;
#endif
    task->texture_binding = textured && current_texture_binding_valid ? current_texture_binding : 0;
    task->software_source_surface = textured ? current_software_source_surface : NULL;
    task->z = flPS2ConvScreenFZ(z);

    return task;
}

static bool try_setup_textured_rect_task(RenderTask* task,
                                         float x0,
                                         float y0,
                                         float x1,
                                         float y1,
                                         float s0,
                                         float t0,
                                         float s1,
                                         float t1,
                                         Uint32 color) {
    if (!current_texture_binding_valid) {
        return false;
    }

    const int texture_handle = LO_16_BITS(current_texture_binding);
    const SDL_Surface* surface = surfaces[texture_handle - 1];
    if (surface == NULL) {
        return false;
    }

    const bool finite_rect = isfinite(x0) && isfinite(y0) && isfinite(x1) && isfinite(y1) && isfinite(s0) &&
                             isfinite(t0) && isfinite(s1) && isfinite(t1);
    if (!finite_rect) {
        return false;
    }

    const float dst_x0 = SDL_min(x0, x1);
    const float dst_y0 = SDL_min(y0, y1);
    const float dst_w = SDL_fabsf(x1 - x0);
    const float dst_h = SDL_fabsf(y1 - y0);
    if ((dst_w <= 0.0f) || (dst_h <= 0.0f)) {
        return false;
    }

    float norm_s0 = s0;
    float norm_t0 = t0;
    float norm_s1 = s1;
    float norm_t1 = t1;
    const float tex_w = (float)surface->w;
    const float tex_h = (float)surface->h;
    const float uv_x0 = SDL_min(s0, s1);
    const float uv_y0 = SDL_min(t0, t1);
    const float uv_x1 = SDL_max(s0, s1);
    const float uv_y1 = SDL_max(t0, t1);
    const bool normalized_uv = (uv_x0 >= 0.0f) && (uv_y0 >= 0.0f) && (uv_x1 <= 1.0f) && (uv_y1 <= 1.0f);
    const bool pixel_uv = (uv_x0 >= 0.0f) && (uv_y0 >= 0.0f) && (uv_x1 <= tex_w) && (uv_y1 <= tex_h);

    if (!normalized_uv) {
        if (!pixel_uv || (tex_w <= 0.0f) || (tex_h <= 0.0f)) {
            return false;
        }
        norm_s0 = s0 / tex_w;
        norm_t0 = t0 / tex_h;
        norm_s1 = s1 / tex_w;
        norm_t1 = t1 / tex_h;
    }

    const float src_x0 = SDL_min(norm_s0, norm_s1);
    const float src_y0 = SDL_min(norm_t0, norm_t1);
    const float src_w = SDL_fabsf(norm_s1 - norm_s0);
    const float src_h = SDL_fabsf(norm_t1 - norm_t0);
    const bool within_normalized_uv = (src_x0 >= 0.0f) && (src_y0 >= 0.0f) && ((src_x0 + src_w) <= 1.0f) &&
                                      ((src_y0 + src_h) <= 1.0f);
    if (!within_normalized_uv || (src_w <= 0.0f) || (src_h <= 0.0f)) {
        return false;
    }

    task->type = RENDER_TASK_TYPE_TEXTURED_RECT;
    task->dst_rect.x = dst_x0;
    task->dst_rect.y = dst_y0;
    task->dst_rect.w = dst_w;
    task->dst_rect.h = dst_h;
    task->src_uv_rect.x = src_x0;
    task->src_uv_rect.y = src_y0;
    task->src_uv_rect.w = src_w;
    task->src_uv_rect.h = src_h;
    task->flip = SDL_FLIP_NONE;
    if ((x1 < x0) != (norm_s1 < norm_s0)) {
        task->flip |= SDL_FLIP_HORIZONTAL;
    }
    if ((y1 < y0) != (norm_t1 < norm_t0)) {
        task->flip |= SDL_FLIP_VERTICAL;
    }
    task->color = color;
    return true;
}

static void draw_sprite_rect(float x0,
                             float y0,
                             float x1,
                             float y1,
                             float z,
                             float s0,
                             float t0,
                             float s1,
                             float t1,
                             unsigned int color) {
    RenderTask* task = begin_quad_task(true, z);
    if (try_setup_textured_rect_task(task, x0, y0, x1, y1, s0, t0, s1, t1, color)) {
        push_render_task(task);
        return;
    }

    SDL_FColor fcolor;
    read_rgba32_fcolor(color, &fcolor);

    task->vertices[0].position.x = x0;
    task->vertices[0].position.y = y0;
    task->vertices[0].tex_coord.x = s0;
    task->vertices[0].tex_coord.y = t0;
    task->vertices[0].color = fcolor;

    task->vertices[1].position.x = x1;
    task->vertices[1].position.y = y0;
    task->vertices[1].tex_coord.x = s1;
    task->vertices[1].tex_coord.y = t0;
    task->vertices[1].color = fcolor;

    task->vertices[2].position.x = x0;
    task->vertices[2].position.y = y1;
    task->vertices[2].tex_coord.x = s0;
    task->vertices[2].tex_coord.y = t1;
    task->vertices[2].color = fcolor;

    task->vertices[3].position.x = x1;
    task->vertices[3].position.y = y1;
    task->vertices[3].tex_coord.x = s1;
    task->vertices[3].tex_coord.y = t1;
    task->vertices[3].color = fcolor;

    push_render_task(task);
}

void SDLGameRenderer_DrawTexturedQuad(const Sprite* sprite, unsigned int color) {
    RenderTask* task = begin_quad_task(true, sprite->v[0].z);
    const bool axis_aligned_rect = nearly_equal(sprite->v[0].y, sprite->v[1].y) &&
                                   nearly_equal(sprite->v[2].y, sprite->v[3].y) &&
                                   nearly_equal(sprite->v[0].x, sprite->v[2].x) &&
                                   nearly_equal(sprite->v[1].x, sprite->v[3].x) &&
                                   nearly_equal(sprite->t[0].t, sprite->t[1].t) &&
                                   nearly_equal(sprite->t[2].t, sprite->t[3].t) &&
                                   nearly_equal(sprite->t[0].s, sprite->t[2].s) &&
                                   nearly_equal(sprite->t[1].s, sprite->t[3].s);
    if (axis_aligned_rect &&
        try_setup_textured_rect_task(task,
                                     sprite->v[0].x,
                                     sprite->v[0].y,
                                     sprite->v[1].x,
                                     sprite->v[2].y,
                                     sprite->t[0].s,
                                     sprite->t[0].t,
                                     sprite->t[1].s,
                                     sprite->t[2].t,
                                     color)) {
        push_render_task(task);
        return;
    }

    SDL_FColor fcolor;
    read_rgba32_fcolor(color, &fcolor);

    for (int i = 0; i < 4; i++) {
        task->vertices[i].position.x = sprite->v[i].x;
        task->vertices[i].position.y = sprite->v[i].y;
        task->vertices[i].tex_coord.x = sprite->t[i].s;
        task->vertices[i].tex_coord.y = sprite->t[i].t;
        task->vertices[i].color = fcolor;
    }

    push_render_task(task);
}

void SDLGameRenderer_DrawSolidQuad(const Quad* sprite, unsigned int color) {
    RenderTask* task = begin_quad_task(false, sprite->v[0].z);
    SDL_FColor fcolor;
    read_rgba32_fcolor(color, &fcolor);

    for (int i = 0; i < 4; i++) {
        task->vertices[i].position.x = sprite->v[i].x;
        task->vertices[i].position.y = sprite->v[i].y;
        task->vertices[i].tex_coord.x = 0.0f;
        task->vertices[i].tex_coord.y = 0.0f;
        task->vertices[i].color = fcolor;
    }

    push_render_task(task);
}

void SDLGameRenderer_DrawSprite(const Sprite* sprite, unsigned int color) {
    draw_sprite_rect(sprite->v[0].x,
                     sprite->v[0].y,
                     sprite->v[3].x,
                     sprite->v[3].y,
                     sprite->v[0].z,
                     sprite->t[0].s,
                     sprite->t[0].t,
                     sprite->t[3].s,
                     sprite->t[3].t,
                     color);
}

void SDLGameRenderer_DrawSprite2(const Sprite2* sprite2) {
    draw_sprite_rect(sprite2->v[0].x,
                     sprite2->v[0].y,
                     sprite2->v[1].x,
                     sprite2->v[1].y,
                     sprite2->v[0].z,
                     sprite2->t[0].s,
                     sprite2->t[0].t,
                     sprite2->t[1].s,
                     sprite2->t[1].t,
                     sprite2->vertex_color);
}
