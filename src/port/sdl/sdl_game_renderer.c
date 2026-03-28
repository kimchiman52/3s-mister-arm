#include "port/sdl/sdl_game_renderer.h"
#include "common.h"
#include "port/sdl/software_frame_non_integer.h"
#include "port/sdl/sdl_message_renderer.h"
#include "sf33rd/AcrSDK/ps2/flps2etc.h"
#include "sf33rd/AcrSDK/ps2/flps2render.h"
#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "sf33rd/Source/Game/system/work_sys.h"

/* From bg_data.h — zoom scroll compensation offsets.  Declared here
   to avoid pulling in the full bg_data.h dependency chain. */
extern short scrn_adgjust_x;
extern short scrn_adgjust_y;

#include <libgraph.h>

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(PORT_MISTER) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define RENDERER_HAVE_NEON 1
#else
#define RENDERER_HAVE_NEON 0
#endif

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
static const int software_frame_full_height_diagonal_strip_max_width_pixels = 8;
static const Uint64 software_frame_affine_quad_max_submitted_pixels = 4096u;
static const Uint64 software_frame_full_texture_affine_quad_max_submitted_pixels = 196608u;
static const Uint64 software_frame_non_integer_lookup_threshold_pixels = 384u;
static const Uint64 software_surface_refresh_blit_sample_period = 32u;
static const Uint64 software_frame_raster_sample_periods[SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT] = {
    32u,
    8u,
    1u,
    1u,
    8u,
};
enum {
    dirty_tile_size = 16,
    dirty_tile_cols = 24,
    dirty_tile_rows = 14,
    dirty_tile_total = dirty_tile_cols * dirty_tile_rows,
    software_source_surface_row_mask_words = 4,
    texture_unlock_multi_rect_max = 4,
    perf_capture_renew_tile_size = 32,
    perf_capture_renew_tile_grid_dim = 8,
    perf_capture_renew_tile_count = perf_capture_renew_tile_grid_dim * perf_capture_renew_tile_grid_dim,
};

static SDL_Renderer* _renderer = NULL;
static SDL_Surface* software_frame_surface = NULL;
static SDL_Texture* software_frame_upload_texture = NULL;
static SDL_Surface* sa_bg_cache_surface = NULL;
static bool sa_bg_cache_surface_valid = false;
static int sa_bg_cache_snapshot_at_index = -1;
static float sa_bg_cache_saved_scr_sc = 1.0f;
static short sa_bg_cache_saved_adgjust_x = 0;
static short sa_bg_cache_saved_adgjust_y = 0;
static bool software_frame_mode_active = false;
static bool software_frame_direct_present_requested = false;
static bool software_frame_surface_ready = false;
static bool software_frame_owned = false;
static bool software_frame_uploaded = false;
static SDLGameRenderer_SuperEffectQualityMode super_effect_quality_mode =
    SDL_GAME_RENDERER_SUPER_EFFECT_QUALITY_FULL;
static SDLGameRenderer_GhostResolutionMode ghost_resolution_mode =
    SDL_GAME_RENDERER_GHOST_RESOLUTION_FULL;
static int sa_bg_cache_frames_remaining = 0;
static bool perf_capture_logical_identity_enabled = false;
static bool perf_capture_fast_non_integer_reuse_telemetry_enabled = true;
static bool perf_capture_fast_non_integer_subrect_alpha_telemetry_enabled = false;
static SDL_Surface* surfaces[FL_TEXTURE_MAX] = { NULL };
static SDL_Palette* palettes[FL_PALETTE_MAX] = { NULL };
typedef enum CacheDirtyReason {
    CACHE_DIRTY_REASON_NONE = 0,
    CACHE_DIRTY_REASON_TEXTURE_UNLOCK,
    CACHE_DIRTY_REASON_PALETTE_UNLOCK,
} CacheDirtyReason;

typedef enum CacheDirtyDetail {
    CACHE_DIRTY_DETAIL_NONE = 0,
    CACHE_DIRTY_DETAIL_PALETTE_CHANGED,
    CACHE_DIRTY_DETAIL_PALETTE_UNCHANGED,
} CacheDirtyDetail;

typedef struct CacheDirtyState {
    Uint32 generation;
    Uint8 reason;
    Uint8 detail;
} CacheDirtyState;

typedef enum TextureUnlockRefreshDecision {
    TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL = 0,
    TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NON_TEXTURE_DIRTY,
    TEXTURE_UNLOCK_REFRESH_DECISION_FULL_INELIGIBLE_SOURCE,
    TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NO_USABLE_DIRTY_RECT,
    TEXTURE_UNLOCK_REFRESH_DECISION_FULL_OVERSIZED_DIRTY_RECT,
} TextureUnlockRefreshDecision;

typedef enum TextureUnlockDirtyRectClearReason {
    TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_STALE_BEFORE_RECORD = 0,
    TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_UNLOCK_UNUSED,
    TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_ACCESS_UNUSED,
    TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_EXPLICIT,
    TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_NONE,
} TextureUnlockDirtyRectClearReason;

typedef struct TextureUnlockRefreshPlan {
    SDL_Rect rects[texture_unlock_multi_rect_max];
    int rect_count;
} TextureUnlockRefreshPlan;

typedef struct TextureLogicalIdentity {
    SDLGameRenderer_TextureLogicalSourceKind source_kind;
    bool valid;
    int ix_num;
    int ix_num_first;
    int slot_index;
    int chunk_index;
    int texture_total;
} TextureLogicalIdentity;

typedef struct PerfCaptureTextureLogicalIdentity {
    TextureLogicalIdentity identity;
    bool mixed;
    Uint32 registrations;
    Uint32 last_seen_serial;
} PerfCaptureTextureLogicalIdentity;

static SDL_Surface* software_surface_cache[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { NULL } };
static CacheDirtyState texture_cache_dirty_state[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { { 0 } } };
static CacheDirtyState software_surface_cache_dirty_state[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { { 0 } } };
static Uint8 texture_cache_runtime_dirty_reason[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { 0 } };
static Uint8 software_surface_cache_runtime_dirty_reason[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { 0 } };
static Uint16 software_surface_cache_texture_unlock_dirty_variant_counts[FL_TEXTURE_MAX] = { 0 };
static bool texture_cache_refresh_pending[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { false } };
static Uint64 software_surface_full_opaque_row_masks[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1]
                                                    [software_source_surface_row_mask_words] = { { { 0 } } };
static bool software_surface_full_opaque_row_masks_valid[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { false } };
static Uint16 software_surface_refresh_binding_generation[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { 0 } };
static Uint16 software_surface_refresh_texture_generation[FL_TEXTURE_MAX] = { 0 };
static Uint16 software_surface_refresh_texture_variant_counts[FL_TEXTURE_MAX] = { 0 };
static SDLGameRenderer_PerfCaptureRefreshTelemetry perf_capture_refresh_telemetry = { 0 };
static Uint64 perf_capture_refresh_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint16 perf_capture_refresh_fanout_max_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_partial_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_partial_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_non_texture_dirty_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_ineligible_source_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_no_usable_dirty_rect_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_oversized_dirty_rect_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_refresh_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_partial_refresh_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_full_refresh_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_full_no_usable_dirty_rect_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_blit_sample_counter = 0;
static Uint64 perf_capture_refresh_blit_sample_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_blit_sample_ns_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_blit_sample_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_blit_sample_ns_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_partial_blit_sample_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_partial_blit_sample_ns_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_non_texture_dirty_blit_sample_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_non_texture_dirty_blit_sample_ns_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_ineligible_source_blit_sample_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_ineligible_source_blit_sample_ns_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_no_usable_dirty_rect_blit_sample_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_no_usable_dirty_rect_blit_sample_ns_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_oversized_dirty_rect_blit_sample_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_refresh_full_oversized_dirty_rect_blit_sample_ns_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint32 perf_capture_refresh_source_format_by_texture[FL_TEXTURE_MAX] = { 0 };
static int perf_capture_refresh_width_by_texture[FL_TEXTURE_MAX] = { 0 };
static int perf_capture_refresh_height_by_texture[FL_TEXTURE_MAX] = { 0 };
static bool perf_capture_refresh_shape_mixed_by_texture[FL_TEXTURE_MAX] = { false };
static Uint64 perf_capture_source_surface_destroy_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_software_surface_access_dirty_texture_same_frame_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_software_surface_access_dirty_texture_carried_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_software_surface_access_dirty_palette_same_frame_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_software_surface_access_dirty_palette_carried_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_software_surface_access_dirty_palette_changed_same_frame_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_software_surface_access_dirty_palette_changed_carried_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_software_surface_access_dirty_palette_unchanged_same_frame_by_texture[FL_TEXTURE_MAX] = {
    0
};
static Uint64 perf_capture_software_surface_access_dirty_palette_unchanged_carried_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_software_surface_access_cold_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_software_surface_access_dirty_texture_same_frame_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_software_surface_access_dirty_texture_carried_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_software_surface_access_dirty_palette_same_frame_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_software_surface_access_dirty_palette_carried_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_software_surface_access_dirty_palette_changed_same_frame_by_texture
    [FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_software_surface_access_dirty_palette_changed_carried_by_texture
    [FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame_by_texture
    [FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_software_surface_access_dirty_palette_unchanged_carried_by_texture
    [FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_current_lifetime_software_surface_access_cold_by_texture[FL_TEXTURE_MAX] = { 0 };
static TextureLogicalIdentity current_texture_logical_identity_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint32 current_texture_logical_identity_serial_by_texture[FL_TEXTURE_MAX] = { 0 };
static PerfCaptureTextureLogicalIdentity perf_capture_texture_logical_identity_by_texture[FL_TEXTURE_MAX] = { 0 };
static SDLGameRenderer_PerfCaptureUnlockLocalityTelemetry perf_capture_unlock_locality_telemetry = { 0 };
static Uint64 perf_capture_unlock_locality_tracked_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_zero_delta_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_baseline_skips_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_non_index8_skips_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_source_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_changed_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_changed_rows_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_changed_bbox_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_whole_capture_tracked_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_whole_capture_zero_delta_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_whole_capture_baseline_skips_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_whole_capture_non_index8_skips_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_whole_capture_source_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_whole_capture_changed_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_whole_capture_changed_rows_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_unlock_locality_whole_capture_changed_bbox_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint32 perf_capture_unlock_locality_source_format_by_texture[FL_TEXTURE_MAX] = { 0 };
static int perf_capture_unlock_locality_width_by_texture[FL_TEXTURE_MAX] = { 0 };
static int perf_capture_unlock_locality_height_by_texture[FL_TEXTURE_MAX] = { 0 };
static bool perf_capture_unlock_locality_shape_mixed_by_texture[FL_TEXTURE_MAX] = { false };
static Uint8* perf_capture_unlock_locality_shadow_pixels[FL_TEXTURE_MAX] = { NULL };
static size_t perf_capture_unlock_locality_shadow_size[FL_TEXTURE_MAX] = { 0 };
static bool perf_capture_unlock_locality_shadow_valid[FL_TEXTURE_MAX] = { false };
static SDL_Rect texture_unlock_dirty_rects[FL_TEXTURE_MAX] = { { 0 } };
static bool texture_unlock_dirty_rect_valid[FL_TEXTURE_MAX] = { false };
static Uint64 texture_unlock_dirty_tile_masks[FL_TEXTURE_MAX] = { 0 };
static bool texture_unlock_dirty_tile_mask_valid[FL_TEXTURE_MAX] = { false };
static Uint64 perf_capture_dirty_rect_record_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_dirty_rect_retained_after_unlock_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_dirty_rect_clear_stale_before_record_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_dirty_rect_clear_unlock_unused_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_dirty_rect_clear_access_unused_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_dirty_rect_clear_explicit_by_texture[FL_TEXTURE_MAX] = { 0 };
static SDL_Rect perf_capture_compare_dirty_rect_pending_rects[FL_TEXTURE_MAX] = { { 0 } };
static bool perf_capture_compare_dirty_rect_pending_valid[FL_TEXTURE_MAX] = { false };
static Uint32 perf_capture_compare_dirty_rect_pending_unlock_counts[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_pending_tile_masks[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_row_mask_pending_tile_masks[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_refresh_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_partial_candidate_refresh_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_oversized_candidate_refresh_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_no_usable_candidate_refresh_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_refresh_bbox_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_refresh_max_bbox_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_refresh_pending_unlocks_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_refresh_max_pending_unlocks_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_refresh_32x32_covered_tiles_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_refresh_32x32_max_covered_tiles_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_refresh_32x32_component_count_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_refresh_32x32_max_component_count_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_rect_refresh_32x32_multi_component_refresh_attempts_by_texture[FL_TEXTURE_MAX] = {
    0
};
static Uint64 perf_capture_compare_dirty_rect_refresh_32x32_largest_component_tiles_by_texture[FL_TEXTURE_MAX] = {
    0
};
static Uint64 perf_capture_compare_dirty_rect_refresh_32x32_max_largest_component_tiles_by_texture[FL_TEXTURE_MAX] = {
    0
};
static Uint64 perf_capture_compare_dirty_row_mask_no_usable_candidate_refresh_attempts_by_texture[FL_TEXTURE_MAX] = {
    0
};
static Uint64 perf_capture_compare_dirty_row_mask_partial_candidate_refresh_attempts_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_row_mask_half_cap_partial_candidate_refresh_attempts_by_texture[FL_TEXTURE_MAX] = {
    0
};
static Uint64 perf_capture_compare_dirty_row_mask_plan_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_row_mask_max_plan_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_row_mask_32x32_covered_tiles_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_row_mask_32x32_max_covered_tiles_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_row_mask_32x32_component_count_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_row_mask_32x32_max_component_count_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_row_mask_32x32_multi_component_refresh_attempts_by_texture[FL_TEXTURE_MAX] = {
    0
};
static Uint64 perf_capture_compare_dirty_row_mask_32x32_largest_component_tiles_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_compare_dirty_row_mask_32x32_max_largest_component_tiles_by_texture[FL_TEXTURE_MAX] = {
    0
};
static SDL_Rect perf_capture_texture_renew_pending_rects[FL_TEXTURE_MAX] = { { 0 } };
static bool perf_capture_texture_renew_pending_rect_valid[FL_TEXTURE_MAX] = { false };
static Uint64 perf_capture_texture_renew_pending_tile_masks[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_chunk_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batches_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batches_without_rect_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_chunk_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batch_bbox_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batch_max_bbox_pixels_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_chunk_8x8_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_chunk_16x16_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_chunk_32x32_calls_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batch_32x32_covered_tiles_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batch_32x32_max_covered_tiles_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batch_32x32_component_count_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batch_32x32_max_component_count_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batch_32x32_multi_component_batches_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batch_32x32_largest_component_tiles_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_texture_renew_batch_32x32_max_largest_component_tiles_by_texture[FL_TEXTURE_MAX] = { 0 };
static Uint64 perf_capture_raster_sample_counters[SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT] = { 0 };
static Uint64 perf_capture_raster_sample_calls[SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT] = { 0 };
static Uint64 perf_capture_raster_sample_pixels[SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT] = { 0 };
static Uint64 perf_capture_raster_sample_ns[SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT] = { 0 };
static bool perf_capture_basic_first_window_family_snapshots_enabled = false;
static bool perf_capture_basic_first_window_render_subphases_enabled = false;
static bool perf_capture_basic_first_window_exact_hot_family_alpha_offpath_enabled = false;
static bool perf_capture_basic_first_window_onset_exact_hot_family_alpha_offpath_enabled = false;
static bool perf_capture_basic_first_window_onset_cluster_alpha_offpath_enabled = false;
static SDLGameRenderer_PerfCaptureFastNonIntegerPhaseTotals perf_capture_fast_non_integer_phase_totals = { 0 };
typedef struct PerfCaptureBasicFirstWindowAlphaOffpathShape {
    int texture_handle;
    int palette_handle;
    int source_x;
    int source_y;
    int source_w;
    int source_h;
    float dst_x;
    float dst_y;
    float dst_w;
    float dst_h;
    int visible_w;
    int visible_h;
    Uint64 task_count;
    Uint64 submitted_pixels;
} PerfCaptureBasicFirstWindowAlphaOffpathShape;
static PerfCaptureBasicFirstWindowAlphaOffpathShape perf_capture_basic_first_window_alpha_offpath_shapes[512] = {
    0
};
static int perf_capture_basic_first_window_alpha_offpath_shape_count = 0;
static SDLGameRenderer_PerfCaptureTexturedRectFamily perf_capture_fast_exact_families[16] = { 0 };
static int perf_capture_fast_exact_family_count = 0;
static SDLGameRenderer_PerfCaptureTexturedRectFamily perf_capture_fast_non_integer_families[64] = { 0 };
static int perf_capture_fast_non_integer_family_count = 0;
static SDLGameRenderer_PerfCaptureTexturedRectExactShape perf_capture_fast_non_integer_shapes[512] = { 0 };
static int perf_capture_fast_non_integer_shape_count = 0;
typedef struct PerfCaptureSABurstEffectSample {
    int texture_handle;
    int palette_handle;
    Uint32 source_format;
    int source_width;
    int source_height;
    int logical_identity_known;
    int logical_identity_mixed;
    Uint32 logical_identity_registrations;
    SDLGameRenderer_TextureLogicalSourceKind logical_source_kind;
    int logical_ix_num;
    int logical_ix_num_first;
    int logical_slot_index;
    int logical_chunk_index;
    int logical_texture_total;
    int alpha_only;
    int rgb_mod;
    int opaque_color;
    int clipped;
    int flip_h;
    int flip_v;
    int source_x;
    int source_y;
    int source_w;
    int source_h;
    int visible_w;
    int visible_h;
    int center_x;
    int center_y;
    int dst_w;
    int dst_h;
    float raw_dst_x_first;
    float raw_dst_y_first;
    float raw_dst_w_first;
    float raw_dst_h_first;
    float raw_dst_x_last;
    float raw_dst_y_last;
    float raw_dst_w_last;
    float raw_dst_h_last;
    Uint64 task_count;
    Uint64 submitted_pixels;
    Uint64 sampled_ns;
} PerfCaptureSABurstEffectSample;
static PerfCaptureSABurstEffectSample perf_capture_sa_burst_effect_samples[128] = { 0 };
static int perf_capture_sa_burst_effect_sample_count = 0;
typedef struct PerfCaptureFastNonIntegerLookupPatternExactProfile {
    int texture_handle;
    int palette_handle;
    Uint32 source_format;
    int source_width;
    int source_height;
    int alpha_only;
    int rgb_mod;
    int opaque_color;
    int integer_positions;
    int integer_source_rect;
    int full_texture_source_rect;
    int clipped;
    int flip_h;
    int flip_v;
    int source_w;
    int source_h;
    int visible_w;
    int visible_h;
    Uint64 x_lookup_signature;
    Uint64 y_lookup_signature;
    Uint64 task_count;
    Uint64 submitted_pixels;
    Uint64 sampled_ns;
} PerfCaptureFastNonIntegerLookupPatternExactProfile;
static PerfCaptureFastNonIntegerLookupPatternExactProfile perf_capture_fast_non_integer_lookup_patterns[512] = { 0 };
static int perf_capture_fast_non_integer_lookup_pattern_count = 0;
static SDLGameRenderer_PerfCaptureTexturedRectFamily perf_capture_generic_textured_families[16] = { 0 };
static int perf_capture_generic_textured_family_count = 0;
static SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily perf_capture_textured_geometry_recovered_families[16] = {
    0
};
static int perf_capture_textured_geometry_recovered_family_count = 0;
static SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily perf_capture_textured_geometry_fallback_families[16] = {
    0
};
static int perf_capture_textured_geometry_fallback_family_count = 0;
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
static const int render_task_insertion_sort_max_tasks = 512;
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

typedef struct SoftwareFrameSolidDiagonalStrip {
    int top_y;
    int bottom_y;
    int top_left_x;
    int top_right_x;
    Uint32 color;
} SoftwareFrameSolidDiagonalStrip;

typedef struct SoftwareFrameTexturedParallelogram {
    int top_y;
    int bottom_y;
    int top_left_x;
    int bottom_left_x;
    int src_x;
    int src_y;
    int src_w;
    int src_h;
} SoftwareFrameTexturedParallelogram;

typedef struct SoftwareFrameTexturedFloatParallelogram {
    float top_y;
    float bottom_y;
    float top_left_x;
    float top_right_x;
    float bottom_left_x;
    float bottom_right_x;
    int src_x;
    int src_y;
    int src_w;
    int src_h;
} SoftwareFrameTexturedFloatParallelogram;

typedef struct SoftwareFrameTexturedAffineQuad {
    int dst_x[4];
    int dst_y[4];
    float src_u[4];
    float src_v[4];
} SoftwareFrameTexturedAffineQuad;

typedef struct SoftwareFrameTexturedFloatAffineQuad {
    float dst_x[4];
    float dst_y[4];
    float src_u[4];
    float src_v[4];
} SoftwareFrameTexturedFloatAffineQuad;

typedef struct SoftwareFrameTexturedTranslatedQuad {
    int dst_x[4];
    int dst_y[4];
    int src_dx;
    int src_dy;
} SoftwareFrameTexturedTranslatedQuad;

typedef enum SoftwareFrameRectStripMergeAxis {
    SOFTWARE_FRAME_RECT_STRIP_MERGE_AXIS_NONE = 0,
    SOFTWARE_FRAME_RECT_STRIP_MERGE_AXIS_HORIZONTAL,
    SOFTWARE_FRAME_RECT_STRIP_MERGE_AXIS_VERTICAL,
} SoftwareFrameRectStripMergeAxis;

typedef struct TexturedGeometryFallbackFamilyProfile {
    int texture_handle;
    int palette_handle;
    Uint32 source_format;
    int source_width;
    int source_height;
    SDLGameRenderer_TexturedGeometryFallbackFamilyKind family_kind;
    bool uniform_color;
    bool opaque_color;
    bool rgb_mod;
    bool integer_positions;
    bool integer_source_rect;
    bool full_texture_source_rect;
    int source_x;
    int source_y;
    int source_w;
    int source_h;
    int dst_height;
    int dst_top_width;
    int dst_bottom_width;
    int dst_left_dx;
    int dst_right_dx;
    Uint64 submitted_pixels;
} TexturedGeometryFallbackFamilyProfile;

typedef struct TexturedRectFamilyProfile {
    int texture_handle;
    int palette_handle;
    Uint32 source_format;
    int source_width;
    int source_height;
    bool alpha_only;
    bool rgb_mod;
    bool opaque_color;
    bool integer_positions;
    bool integer_source_rect;
    bool full_texture_source_rect;
    bool clipped;
    bool flip_h;
    bool flip_v;
    int source_x;
    int source_y;
    int source_w;
    int source_h;
    int dst_x;
    int dst_y;
    int dst_w;
    int dst_h;
    int visible_w;
    int visible_h;
    float raw_dst_x;
    float raw_dst_y;
    float raw_dst_w;
    float raw_dst_h;
    Uint64 submitted_pixels;
} TexturedRectFamilyProfile;

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
static bool can_merge_rect_tasks_horizontally(const RenderTask* prev, const RenderTask* next);
static bool can_merge_rect_tasks_vertically(const RenderTask* prev, const RenderTask* next);
static void mark_dirty_tiles_for_task(const RenderTask* task);
static void clear_cache_dirty_state(CacheDirtyState* state);
#if ENABLE_PERF_TELEMETRY
static void reset_cache_dirty_tracking(void);
static void begin_cache_dirty_tracking_frame(bool capture_extended_stats);
static void note_texture_cache_miss_provenance(const CacheDirtyState* state);
static void note_software_surface_cache_create_provenance(const CacheDirtyState* state);
#endif
static void mark_cache_dirty_state_with_detail(CacheDirtyState* state,
                                               CacheDirtyReason reason,
                                               CacheDirtyDetail detail);
static void mark_cache_dirty_state(CacheDirtyState* state, CacheDirtyReason reason);
static void note_software_surface_dirty_variant_fanout(CacheDirtyReason reason, int dirty_variant_count);
static bool should_keep_dirty_cache_entries(CacheDirtyReason reason);
static bool should_refresh_dirty_cache_entry(Uint8 dirty_reason);
static void clear_software_surface_full_opaque_row_mask(int texture_index, int palette_handle);
static const Uint64* get_software_surface_full_opaque_row_mask(unsigned int th, const SDL_Surface* surface);
#if ENABLE_PERF_TELEMETRY
static void begin_software_surface_refresh_tracking_frame(bool capture_extended_stats);
#endif
static void note_software_surface_refresh_attempt(unsigned int th);
static void note_perf_capture_refresh_attempt(unsigned int th, const SDL_Surface* source_surface);
static void note_perf_capture_refresh_path(unsigned int th,
                                           const SDL_Surface* source_surface,
                                           TextureUnlockRefreshDecision decision,
                                           const TextureUnlockRefreshPlan* partial_plan);
static bool should_sample_perf_capture_refresh_blit(void);
static Uint64 perf_capture_counter_delta_to_ns(Uint64 start_counter, Uint64 end_counter);
static void note_perf_capture_refresh_blit_sample(unsigned int th,
                                                  TextureUnlockRefreshDecision decision,
                                                  Uint64 elapsed_ns);
static Uint64 begin_perf_capture_raster_bucket_sample(SDLGameRenderer_PerfCaptureRasterBucket bucket);
static void note_perf_capture_raster_bucket_sample(SDLGameRenderer_PerfCaptureRasterBucket bucket,
                                                   const RenderTask* task,
                                                   Uint64 elapsed_ns);
#if ENABLE_PERF_TELEMETRY
static void note_perf_capture_textured_geometry_fallback_family(const RenderTask* task);
#endif
static bool textured_geometry_task_has_rect_uv(const RenderTask* task,
                                               float min_u,
                                               float max_u,
                                               float min_v,
                                               float max_v);
static void reset_perf_capture_unlock_locality_shadow_slot(int texture_index);
static void reset_perf_capture_refresh_lifetime_slot(int texture_index);
static void reset_perf_capture_unlock_locality_texture_slot(int texture_index);
static void reset_perf_capture_texture_renew_slot(int texture_index);
static Uint64 make_perf_capture_renew_tile_mask(int x, int y, int w, int h);
static int count_perf_capture_renew_tile_components(Uint64 mask, int* out_largest_component_tiles);
static Uint64 texture_unlock_refresh_plan_pixels(const TextureUnlockRefreshPlan* plan);
static bool build_texture_unlock_refresh_plan_from_tile_mask(Uint64 tile_mask, TextureUnlockRefreshPlan* out_plan);
static bool texture_unlock_refresh_rect_is_valid(const SDL_Rect* rect, const SDL_Surface* source_surface);
static Uint64 texture_unlock_refresh_rect_pixels(const SDL_Rect* rect);
static bool compare_dirty_row_mask_plan_fits_relaxed_partial_exception(const TextureUnlockRefreshPlan* plan,
                                                                       const SDL_Surface* source_surface,
                                                                       Uint64 max_partial_pixels);
static void clear_texture_logical_identity(TextureLogicalIdentity* identity);
static void clear_current_texture_logical_identity_slot(int texture_index);
static void reset_perf_capture_texture_logical_identity_slot(int texture_index);
static bool texture_logical_identity_equals(const TextureLogicalIdentity* lhs, const TextureLogicalIdentity* rhs);
static void note_perf_capture_texture_logical_identity(int texture_index);
static void copy_perf_capture_texture_logical_identity(int texture_index,
                                                       int* out_known,
                                                       int* out_mixed,
                                                       Uint32* out_registrations,
                                                       SDLGameRenderer_TextureLogicalSourceKind* out_source_kind,
                                                       int* out_ix_num,
                                                       int* out_ix_num_first,
                                                       int* out_slot_index,
                                                       int* out_chunk_index,
                                                       int* out_texture_total);
static void note_perf_capture_texture_unlock_locality(int texture_handle, const SDL_Surface* source_surface);
static void note_perf_capture_compare_dirty_rect_candidate(int texture_index,
                                                           const SDL_Rect* dirty_rect,
                                                           Uint64 row_tile_mask);
static void note_perf_capture_dirty_rect_record(int texture_index);
static void note_perf_capture_dirty_rect_retained_after_unlock(int texture_index);
static void note_perf_capture_dirty_rect_clear(int texture_index, TextureUnlockDirtyRectClearReason reason);
#if ENABLE_PERF_TELEMETRY
static void note_perf_capture_software_surface_access_provenance(int texture_index, const CacheDirtyState* state);
#endif
static void clear_perf_capture_compare_dirty_rect_pending_index(int texture_index);
static bool software_surface_cache_slot_has_texture_unlock_dirty_reason(int texture_index, int palette_handle);
static bool texture_index_has_software_surface_cache_variants(int texture_index);
static void set_software_surface_cache_slot_state(int texture_index,
                                                  int palette_handle,
                                                  SDL_Surface* surface,
                                                  Uint8 runtime_dirty_reason);
static void clear_texture_unlock_dirty_rect_index(int texture_index, TextureUnlockDirtyRectClearReason reason);
static void clear_texture_unlock_dirty_rect_if_unused(int texture_index, TextureUnlockDirtyRectClearReason reason);
static void note_perf_capture_compare_dirty_rect_refresh_candidate(unsigned int th,
                                                                   Uint8 dirty_reason,
                                                                   const SDL_Surface* source_surface);
static void note_perf_capture_compare_dirty_row_mask_refresh_candidate(int texture_index,
                                                                       const SDL_Surface* source_surface,
                                                                       Uint64 current_max_partial_pixels);
static TextureUnlockRefreshDecision classify_texture_unlock_refresh_decision(unsigned int th,
                                                                             Uint8 dirty_reason,
                                                                             const SDL_Surface* source_surface,
                                                                             TextureUnlockRefreshPlan* out_plan);
static bool refresh_software_source_surface_in_place(unsigned int th, SDL_Surface* cached_surface, Uint8 dirty_reason);
static bool refresh_texture_in_place(unsigned int th,
                                     SDL_Texture* texture,
                                     SDL_Surface** inout_software_source_surface);
static int invalidate_texture_cache_for_texture_index(int texture_index, CacheDirtyReason reason);
static int invalidate_texture_cache_for_palette_handle(int palette_handle,
                                                       CacheDirtyReason reason,
                                                       CacheDirtyDetail detail);
static int invalidate_software_surface_cache_for_texture_index(int texture_index, CacheDirtyReason reason);
static int invalidate_software_surface_cache_for_palette_handle(int palette_handle,
                                                                CacheDirtyReason reason,
                                                                CacheDirtyDetail detail);
static void fill_palette_colors_from_fl_texture(const FLTexture* fl_palette, SDL_Color* colors, int* out_color_count);
static bool palette_colors_equal(const SDL_Palette* palette, const SDL_Color* colors, int color_count);
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
static bool analyze_textured_geometry_fallback_task(const RenderTask* task,
                                                    TexturedGeometryFallbackFamilyProfile* out_profile);
#if ENABLE_PERF_TELEMETRY
static bool analyze_textured_rect_family_task(const RenderTask* task,
                                              const SDL_Surface* dst_surface,
                                              TexturedRectFamilyProfile* out_profile);
#endif
#if ENABLE_PERF_TELEMETRY
static bool perf_capture_basic_first_window_exact_hot_family_matches_profile(const TexturedRectFamilyProfile* profile);
static bool perf_capture_basic_first_window_onset_exact_hot_family_matches_profile(
    const TexturedRectFamilyProfile* profile);
static void note_perf_capture_basic_first_window_alpha_offpath_shape(const TexturedRectFamilyProfile* profile);
static bool perf_capture_sa_burst_effect_candidate_matches_profile(
    const TexturedRectFamilyProfile* profile);
static void note_perf_capture_sa_burst_effect_sample(const TexturedRectFamilyProfile* profile,
                                                                  Uint64 sampled_ns);
#endif
static void note_hybrid_eligibility(const RenderTask* task);
static bool try_resolve_geometry_task_as_rect_copy(const RenderTask* task, RenderTask* out_rect_task);
static bool try_resolve_solid_task_as_rect(const RenderTask* task, SDL_FRect* out_rect, Uint32* out_color);
static bool try_resolve_solid_task_as_full_height_diagonal_strip(const RenderTask* task,
                                                                 SoftwareFrameSolidDiagonalStrip* out_strip);
static bool try_resolve_geometry_task_as_software_frame_parallelogram(
    const RenderTask* task, SoftwareFrameTexturedParallelogram* out_parallelogram);
static bool try_resolve_geometry_task_as_software_frame_float_parallelogram(
    const RenderTask* task, SoftwareFrameTexturedFloatParallelogram* out_parallelogram);
static bool try_resolve_geometry_task_as_software_frame_full_texture_affine_quad(
    const RenderTask* task, SoftwareFrameTexturedFloatAffineQuad* out_quad);
static bool try_resolve_geometry_task_as_software_frame_translated_quad(
    const RenderTask* task, SoftwareFrameTexturedTranslatedQuad* out_quad);
static bool try_resolve_geometry_task_as_software_frame_affine_quad(
    const RenderTask* task, SoftwareFrameTexturedAffineQuad* out_quad);
static SoftwareFrameFallbackReason classify_software_frame_fallback_reason(const RenderTask* task);
static void note_software_frame_eligibility(const RenderTask* task, SoftwareFrameFallbackReason reason);
static SoftwareFrameFastCopyResult build_software_frame_fast_copy_plan(const RenderTask* task,
                                                                       const SDL_Surface* dst_surface,
                                                                       const SDL_Surface* src_surface,
                                                                       SoftwareFrameFastCopyPlan* out_plan);
static void note_software_frame_fast_copy_result(const RenderTask* task,
                                                 SoftwareFrameFastCopyResult result,
                                                 const SoftwareFrameFastCopyPlan* plan,
                                                 const SDL_Surface* dst_surface,
                                                 Uint64 non_integer_lookup_entries,
                                                 Uint64 sampled_ns);
#if ENABLE_PERF_TELEMETRY
static void note_perf_capture_fast_exact_family(const RenderTask* task,
                                                const SDL_Surface* dst_surface,
                                                Uint64 lookup_entries,
                                                Uint64 sampled_ns);
#endif
static void note_software_frame_fast_non_integer(const RenderTask* task,
                                                 const SDL_Surface* dst_surface,
                                                 Uint64 lookup_entries,
                                                 const SDLSoftwareFrame_NonIntegerTelemetry* non_integer_telemetry,
                                                 Uint64 sampled_ns);
static Uint32 modulate_argb8888(Uint32 pixel, Uint32 color);
static bool is_blue_tint_color(Uint32 color);
static Uint32 modulate_argb8888_blue_tint(Uint32 pixel, Uint32 rg_factor, Uint32 mod_a);
static bool is_ghost_sprite_color(Uint32 color);
static Uint32 blend_argb8888(Uint32 dst_pixel, Uint32 src_pixel);
static bool raster_full_height_diagonal_strip_to_software_frame(const SoftwareFrameSolidDiagonalStrip* strip,
                                                                SDL_Surface* dst_surface);
static bool raster_textured_parallelogram_to_software_frame(const RenderTask* task);
static bool raster_textured_float_parallelogram_to_software_frame(const RenderTask* task);
static bool raster_textured_full_texture_affine_quad_to_software_frame(const RenderTask* task);
static bool raster_textured_translated_quad_to_software_frame(const RenderTask* task);
static bool raster_textured_affine_quad_to_software_frame(const RenderTask* task);
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
static bool try_merge_software_frame_rect_tasks(RenderTask* merged_task,
                                                const RenderTask* next_task,
                                                const SDL_Surface* dst_surface,
                                                SoftwareFrameRectStripMergeAxis* inout_axis);
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
static void apply_super_effect_burst_reduction_after_sort(void);
#if ENABLE_PERF_TELEMETRY
static int get_perf_capture_fast_non_integer_shared_shapes_from_exact_shapes(
    const SDLGameRenderer_PerfCaptureTexturedRectExactShape* shapes,
    int shape_count,
    SDLGameRenderer_PerfCaptureFastNonIntegerSharedShape* out_shapes,
    int max_shapes,
    Uint64* out_task_total,
    Uint64* out_pixel_total,
    Uint64* out_sampled_ns_total,
    int* out_shape_count);
static int get_perf_capture_fast_non_integer_lookup_profiles_from_patterns(
    const PerfCaptureFastNonIntegerLookupPatternExactProfile* patterns,
    int pattern_count,
    SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile* out_profiles,
    int max_profiles,
    Uint64* out_task_total,
    Uint64* out_pixel_total,
    Uint64* out_sampled_ns_total,
    int* out_profile_count);
#endif
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
        if (*surface_p == NULL) {
            clear_software_surface_full_opaque_row_mask(texture_index, i);
            clear_cache_dirty_state(dirty_state);
            set_software_surface_cache_slot_state(texture_index, i, NULL, CACHE_DIRTY_REASON_NONE);
            continue;
        }

        if (keep_dirty_entries) {
            clear_software_surface_full_opaque_row_mask(texture_index, i);
            mark_cache_dirty_state(dirty_state, reason);
            set_software_surface_cache_slot_state(texture_index, i, *surface_p, (Uint8)reason);
            dirty_variant_count += 1;
            continue;
        }

        push_software_surface_to_destroy(*surface_p);
        clear_software_surface_full_opaque_row_mask(texture_index, i);
        mark_cache_dirty_state(dirty_state, reason);
        set_software_surface_cache_slot_state(texture_index, i, NULL, CACHE_DIRTY_REASON_NONE);
        evicted_count += 1;
    }

    note_software_surface_dirty_variant_fanout(reason, dirty_variant_count);
    return evicted_count;
}

static int invalidate_texture_cache_for_palette_handle(int palette_handle,
                                                       CacheDirtyReason reason,
                                                       CacheDirtyDetail detail) {
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
            mark_cache_dirty_state_with_detail(dirty_state, reason, detail);
            *runtime_dirty_reason_p = (Uint8)reason;
            continue;
        }

        push_texture_to_destroy(*texture_p);
        *texture_p = NULL;
        mark_cache_dirty_state_with_detail(dirty_state, reason, detail);
        *runtime_dirty_reason_p = CACHE_DIRTY_REASON_NONE;
        texture_cache_refresh_pending[i][palette_handle] = false;
        evicted_count += 1;
    }

    return evicted_count;
}

static int invalidate_software_surface_cache_for_palette_handle(int palette_handle,
                                                                CacheDirtyReason reason,
                                                                CacheDirtyDetail detail) {
    const bool keep_dirty_entries = should_keep_dirty_cache_entries(reason);
    int evicted_count = 0;
    int dirty_variant_count = 0;

    if (current_texture_binding_valid && (HI_16_BITS(current_texture_binding) == (unsigned int)palette_handle)) {
        current_software_source_surface = NULL;
    }

    for (int i = 0; i < FL_TEXTURE_MAX; i++) {
        SDL_Surface** surface_p = &software_surface_cache[i][palette_handle];
        CacheDirtyState* dirty_state = &software_surface_cache_dirty_state[i][palette_handle];
        if (*surface_p == NULL) {
            clear_software_surface_full_opaque_row_mask(i, palette_handle);
            clear_cache_dirty_state(dirty_state);
            set_software_surface_cache_slot_state(i, palette_handle, NULL, CACHE_DIRTY_REASON_NONE);
            continue;
        }

        if (keep_dirty_entries) {
            clear_software_surface_full_opaque_row_mask(i, palette_handle);
            mark_cache_dirty_state_with_detail(dirty_state, reason, detail);
            set_software_surface_cache_slot_state(i, palette_handle, *surface_p, (Uint8)reason);
            dirty_variant_count += 1;
            continue;
        }

        push_software_surface_to_destroy(*surface_p);
        clear_software_surface_full_opaque_row_mask(i, palette_handle);
        mark_cache_dirty_state_with_detail(dirty_state, reason, detail);
        set_software_surface_cache_slot_state(i, palette_handle, NULL, CACHE_DIRTY_REASON_NONE);
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

    // Track Z-inversions so the post-accumulation sort knows whether it needs to run.
    // The task data is already written at render_tasks[render_task_count] by begin_quad_task,
    // so just append in place — the post-accumulation sort (qsort or insertion sort) uses the
    // index field as a tiebreaker within equal-Z groups to produce the correct final order.
    if (render_task_count > 0) {
        if (render_tasks[render_task_count - 1].z >= task->z) {
            render_tasks_have_z_inversion = true;
            render_tasks_z_inversion_count += 1;
        }
    }

    mark_dirty_tiles_for_task(task);
#if ENABLE_PERF_TELEMETRY
    count_task_source(task->source);
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
    state->detail = CACHE_DIRTY_DETAIL_NONE;
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

static void mark_cache_dirty_state_with_detail(CacheDirtyState* state,
                                               CacheDirtyReason reason,
                                               CacheDirtyDetail detail) {
    if (state == NULL) {
        return;
    }

    if (!frame_stats_extended_enabled || !cache_dirty_tracking_active || (reason == CACHE_DIRTY_REASON_NONE)) {
        clear_cache_dirty_state(state);
        return;
    }

    state->generation = cache_dirty_generation;
    state->reason = (Uint8)reason;
    state->detail = (Uint8)detail;
}

static void mark_cache_dirty_state(CacheDirtyState* state, CacheDirtyReason reason) {
    mark_cache_dirty_state_with_detail(state, reason, CACHE_DIRTY_DETAIL_NONE);
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

#if ENABLE_PERF_TELEMETRY
static void note_perf_capture_software_surface_access_provenance(int texture_index, const CacheDirtyState* state) {
    if (!frame_stats_extended_enabled || (state == NULL) || (texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    switch ((CacheDirtyReason)state->reason) {
    case CACHE_DIRTY_REASON_TEXTURE_UNLOCK:
        if (state->generation == cache_dirty_generation) {
            perf_capture_software_surface_access_dirty_texture_same_frame_by_texture[texture_index] += 1;
            perf_capture_current_lifetime_software_surface_access_dirty_texture_same_frame_by_texture[texture_index] += 1;
        } else {
            perf_capture_software_surface_access_dirty_texture_carried_by_texture[texture_index] += 1;
            perf_capture_current_lifetime_software_surface_access_dirty_texture_carried_by_texture[texture_index] += 1;
        }
        break;
    case CACHE_DIRTY_REASON_PALETTE_UNLOCK:
        if (state->generation == cache_dirty_generation) {
            perf_capture_software_surface_access_dirty_palette_same_frame_by_texture[texture_index] += 1;
            perf_capture_current_lifetime_software_surface_access_dirty_palette_same_frame_by_texture[texture_index] += 1;
            if (state->detail == CACHE_DIRTY_DETAIL_PALETTE_UNCHANGED) {
                perf_capture_software_surface_access_dirty_palette_unchanged_same_frame_by_texture[texture_index] += 1;
                perf_capture_current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame_by_texture
                    [texture_index] += 1;
            } else {
                perf_capture_software_surface_access_dirty_palette_changed_same_frame_by_texture[texture_index] += 1;
                perf_capture_current_lifetime_software_surface_access_dirty_palette_changed_same_frame_by_texture
                    [texture_index] += 1;
            }
        } else {
            perf_capture_software_surface_access_dirty_palette_carried_by_texture[texture_index] += 1;
            perf_capture_current_lifetime_software_surface_access_dirty_palette_carried_by_texture[texture_index] += 1;
            if (state->detail == CACHE_DIRTY_DETAIL_PALETTE_UNCHANGED) {
                perf_capture_software_surface_access_dirty_palette_unchanged_carried_by_texture[texture_index] += 1;
                perf_capture_current_lifetime_software_surface_access_dirty_palette_unchanged_carried_by_texture
                    [texture_index] += 1;
            } else {
                perf_capture_software_surface_access_dirty_palette_changed_carried_by_texture[texture_index] += 1;
                perf_capture_current_lifetime_software_surface_access_dirty_palette_changed_carried_by_texture
                    [texture_index] += 1;
            }
        }
        break;
    case CACHE_DIRTY_REASON_NONE:
    default:
        perf_capture_software_surface_access_cold_by_texture[texture_index] += 1;
        perf_capture_current_lifetime_software_surface_access_cold_by_texture[texture_index] += 1;
        break;
    }
}
#endif
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
    note_perf_capture_texture_logical_identity(texture_index);
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
    perf_capture_current_lifetime_refresh_attempts_by_texture[texture_index] += 1;
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

static void note_perf_capture_refresh_path(unsigned int th,
                                           const SDL_Surface* source_surface,
                                           TextureUnlockRefreshDecision decision,
                                           const TextureUnlockRefreshPlan* partial_plan) {
    if (!frame_stats_extended_enabled || (source_surface == NULL)) {
        return;
    }

    const int texture_handle = LO_16_BITS(th);
    if ((texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return;
    }

    const int texture_index = texture_handle - 1;
    const Uint64 source_pixels = (Uint64)source_surface->w * (Uint64)source_surface->h;
    const Uint64 partial_pixels = texture_unlock_refresh_plan_pixels(partial_plan);

    if (decision == TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL) {
        perf_capture_refresh_telemetry.partial_attempts += 1;
        perf_capture_refresh_telemetry.partial_pixels += partial_pixels;
        perf_capture_refresh_partial_attempts_by_texture[texture_index] += 1;
        perf_capture_refresh_partial_pixels_by_texture[texture_index] += partial_pixels;
        perf_capture_current_lifetime_partial_refresh_attempts_by_texture[texture_index] += 1;
        return;
    }

    perf_capture_refresh_telemetry.full_attempts += 1;
    perf_capture_refresh_telemetry.full_pixels += source_pixels;
    perf_capture_refresh_full_attempts_by_texture[texture_index] += 1;
    perf_capture_refresh_full_pixels_by_texture[texture_index] += source_pixels;
    perf_capture_current_lifetime_full_refresh_attempts_by_texture[texture_index] += 1;

    switch (decision) {
    case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NON_TEXTURE_DIRTY:
        perf_capture_refresh_telemetry.full_non_texture_dirty_attempts += 1;
        perf_capture_refresh_full_non_texture_dirty_attempts_by_texture[texture_index] += 1;
        break;
    case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_INELIGIBLE_SOURCE:
        perf_capture_refresh_telemetry.full_ineligible_source_attempts += 1;
        perf_capture_refresh_full_ineligible_source_attempts_by_texture[texture_index] += 1;
        break;
    case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NO_USABLE_DIRTY_RECT:
        perf_capture_refresh_telemetry.full_no_usable_dirty_rect_attempts += 1;
        perf_capture_refresh_full_no_usable_dirty_rect_attempts_by_texture[texture_index] += 1;
        perf_capture_current_lifetime_full_no_usable_dirty_rect_attempts_by_texture[texture_index] += 1;
        break;
    case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_OVERSIZED_DIRTY_RECT:
        perf_capture_refresh_telemetry.full_oversized_dirty_rect_attempts += 1;
        perf_capture_refresh_full_oversized_dirty_rect_attempts_by_texture[texture_index] += 1;
        break;
    case TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL:
    default:
        break;
    }
}

static bool should_sample_perf_capture_refresh_blit(void) {
    if (!frame_stats_extended_enabled || (software_surface_refresh_blit_sample_period == 0)) {
        return false;
    }

    perf_capture_refresh_blit_sample_counter += 1;
    return (perf_capture_refresh_blit_sample_counter % software_surface_refresh_blit_sample_period) == 0;
}

static Uint64 perf_capture_counter_delta_to_ns(Uint64 start_counter, Uint64 end_counter) {
    if ((start_counter == 0) || (end_counter <= start_counter)) {
        return 0;
    }

    const Uint64 frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0) {
        return 0;
    }

    const Uint64 delta = end_counter - start_counter;
    return (Uint64)(((double)delta * 1000000000.0) / (double)frequency);
}

static void note_perf_capture_refresh_blit_sample(unsigned int th,
                                                  TextureUnlockRefreshDecision decision,
                                                  Uint64 elapsed_ns) {
    if (!frame_stats_extended_enabled) {
        return;
    }

    perf_capture_refresh_telemetry.sampled_blit_period = software_surface_refresh_blit_sample_period;
    perf_capture_refresh_telemetry.sampled_blit_calls += 1;
    perf_capture_refresh_telemetry.sampled_blit_ns += elapsed_ns;
    if (decision == TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL) {
        perf_capture_refresh_telemetry.sampled_partial_blit_calls += 1;
        perf_capture_refresh_telemetry.sampled_partial_blit_ns += elapsed_ns;
    } else {
        perf_capture_refresh_telemetry.sampled_full_blit_calls += 1;
        perf_capture_refresh_telemetry.sampled_full_blit_ns += elapsed_ns;
        switch (decision) {
        case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NON_TEXTURE_DIRTY:
            perf_capture_refresh_telemetry.sampled_full_non_texture_dirty_blit_calls += 1;
            perf_capture_refresh_telemetry.sampled_full_non_texture_dirty_blit_ns += elapsed_ns;
            break;
        case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_INELIGIBLE_SOURCE:
            perf_capture_refresh_telemetry.sampled_full_ineligible_source_blit_calls += 1;
            perf_capture_refresh_telemetry.sampled_full_ineligible_source_blit_ns += elapsed_ns;
            break;
        case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NO_USABLE_DIRTY_RECT:
            perf_capture_refresh_telemetry.sampled_full_no_usable_dirty_rect_blit_calls += 1;
            perf_capture_refresh_telemetry.sampled_full_no_usable_dirty_rect_blit_ns += elapsed_ns;
            break;
        case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_OVERSIZED_DIRTY_RECT:
            perf_capture_refresh_telemetry.sampled_full_oversized_dirty_rect_blit_calls += 1;
            perf_capture_refresh_telemetry.sampled_full_oversized_dirty_rect_blit_ns += elapsed_ns;
            break;
        case TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL:
        default:
            break;
        }
    }

    const int texture_handle = LO_16_BITS(th);
    if ((texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return;
    }

    const int texture_index = texture_handle - 1;
    perf_capture_refresh_blit_sample_calls_by_texture[texture_index] += 1;
    perf_capture_refresh_blit_sample_ns_by_texture[texture_index] += elapsed_ns;
    if (decision == TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL) {
        perf_capture_refresh_partial_blit_sample_calls_by_texture[texture_index] += 1;
        perf_capture_refresh_partial_blit_sample_ns_by_texture[texture_index] += elapsed_ns;
        return;
    }

    perf_capture_refresh_full_blit_sample_calls_by_texture[texture_index] += 1;
    perf_capture_refresh_full_blit_sample_ns_by_texture[texture_index] += elapsed_ns;
    switch (decision) {
    case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NON_TEXTURE_DIRTY:
        perf_capture_refresh_full_non_texture_dirty_blit_sample_calls_by_texture[texture_index] += 1;
        perf_capture_refresh_full_non_texture_dirty_blit_sample_ns_by_texture[texture_index] += elapsed_ns;
        break;
    case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_INELIGIBLE_SOURCE:
        perf_capture_refresh_full_ineligible_source_blit_sample_calls_by_texture[texture_index] += 1;
        perf_capture_refresh_full_ineligible_source_blit_sample_ns_by_texture[texture_index] += elapsed_ns;
        break;
    case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NO_USABLE_DIRTY_RECT:
        perf_capture_refresh_full_no_usable_dirty_rect_blit_sample_calls_by_texture[texture_index] += 1;
        perf_capture_refresh_full_no_usable_dirty_rect_blit_sample_ns_by_texture[texture_index] += elapsed_ns;
        break;
    case TEXTURE_UNLOCK_REFRESH_DECISION_FULL_OVERSIZED_DIRTY_RECT:
        perf_capture_refresh_full_oversized_dirty_rect_blit_sample_calls_by_texture[texture_index] += 1;
        perf_capture_refresh_full_oversized_dirty_rect_blit_sample_ns_by_texture[texture_index] += elapsed_ns;
        break;
    case TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL:
    default:
        break;
    }
}

static Uint64 begin_perf_capture_raster_bucket_sample(SDLGameRenderer_PerfCaptureRasterBucket bucket) {
    if ((!frame_stats_extended_enabled && !perf_capture_basic_first_window_render_subphases_enabled) ||
        (bucket < 0) || (bucket >= SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT)) {
        return 0;
    }

    const Uint64 sample_period = software_frame_raster_sample_periods[bucket];
    if (sample_period == 0) {
        return 0;
    }

    perf_capture_raster_sample_counters[bucket] += 1;
    return (perf_capture_raster_sample_counters[bucket] % sample_period) == 0 ? SDL_GetPerformanceCounter() : 0;
}

static void note_perf_capture_raster_bucket_sample(SDLGameRenderer_PerfCaptureRasterBucket bucket,
                                                   const RenderTask* task,
                                                   Uint64 elapsed_ns) {
    if ((!frame_stats_extended_enabled && !perf_capture_basic_first_window_render_subphases_enabled) ||
        (bucket < 0) || (bucket >= SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT) || (task == NULL) ||
        (elapsed_ns == 0)) {
        return;
    }

    perf_capture_raster_sample_calls[bucket] += 1;
    perf_capture_raster_sample_pixels[bucket] += render_task_submitted_pixels(task);
    perf_capture_raster_sample_ns[bucket] += elapsed_ns;
}

#if ENABLE_PERF_TELEMETRY
static void update_textured_geometry_fallback_range(int* min_value, int* max_value, int value) {
    if ((min_value == NULL) || (max_value == NULL)) {
        return;
    }
    if (*min_value > value) {
        *min_value = value;
    }
    if (*max_value < value) {
        *max_value = value;
    }
}
#endif

static bool textured_geometry_task_has_rect_uv(const RenderTask* task,
                                               float min_u,
                                               float max_u,
                                               float min_v,
                                               float max_v) {
    if (task == NULL) {
        return false;
    }

    bool has_min_u = false;
    bool has_max_u = false;
    bool has_min_v = false;
    bool has_max_v = false;
    for (int vertex_index = 0; vertex_index < 4; vertex_index++) {
        const SDL_Vertex* vertex = &task->vertices[vertex_index];
        const bool at_min_u = nearly_equal(vertex->tex_coord.x, min_u);
        const bool at_max_u = nearly_equal(vertex->tex_coord.x, max_u);
        const bool at_min_v = nearly_equal(vertex->tex_coord.y, min_v);
        const bool at_max_v = nearly_equal(vertex->tex_coord.y, max_v);
        if (!((at_min_u || at_max_u) && (at_min_v || at_max_v))) {
            return false;
        }
        has_min_u |= at_min_u;
        has_max_u |= at_max_u;
        has_min_v |= at_min_v;
        has_max_v |= at_max_v;
    }

    return has_min_u && has_max_u && has_min_v && has_max_v;
}

static bool analyze_textured_geometry_fallback_task(const RenderTask* task,
                                                    TexturedGeometryFallbackFamilyProfile* out_profile) {
    if ((task == NULL) || (out_profile == NULL) || (task->type != RENDER_TASK_TYPE_GEOMETRY) || (task->texture == NULL)) {
        return false;
    }

    SDL_zero(*out_profile);
    out_profile->texture_handle = LO_16_BITS(task->texture_binding);
    out_profile->palette_handle = HI_16_BITS(task->texture_binding);
    out_profile->family_kind = SDL_GAME_RENDERER_TEXTURED_GEOMETRY_FALLBACK_FAMILY_OTHER;
    out_profile->source_x = -1;
    out_profile->source_y = -1;
    out_profile->source_w = -1;
    out_profile->source_h = -1;
    out_profile->dst_height = -1;
    out_profile->dst_top_width = -1;
    out_profile->dst_bottom_width = -1;
    out_profile->dst_left_dx = -1;
    out_profile->dst_right_dx = -1;
    out_profile->submitted_pixels = render_task_submitted_pixels(task);

    const int texture_index = out_profile->texture_handle - 1;
    if ((texture_index >= 0) && (texture_index < FL_TEXTURE_MAX) && (surfaces[texture_index] != NULL)) {
        out_profile->source_format = surfaces[texture_index]->format;
        out_profile->source_width = surfaces[texture_index]->w;
        out_profile->source_height = surfaces[texture_index]->h;
    } else {
        out_profile->source_format = SDL_PIXELFORMAT_UNKNOWN;
    }

    const SDL_FColor color0 = task->vertices[0].color;
    out_profile->uniform_color = true;
    out_profile->opaque_color = nearly_equal(color0.a, 1.0f);
    out_profile->rgb_mod = !(nearly_equal(color0.r, 1.0f) && nearly_equal(color0.g, 1.0f) && nearly_equal(color0.b, 1.0f));
    out_profile->integer_positions = true;
    for (int i = 1; i < 4; i++) {
        const SDL_FColor color = task->vertices[i].color;
        if (!nearly_equal(color.r, color0.r) || !nearly_equal(color.g, color0.g) || !nearly_equal(color.b, color0.b) ||
            !nearly_equal(color.a, color0.a)) {
            out_profile->uniform_color = false;
        }
        if (!nearly_equal(color.a, 1.0f)) {
            out_profile->opaque_color = false;
        }
        if (!nearly_equal(color.r, 1.0f) || !nearly_equal(color.g, 1.0f) || !nearly_equal(color.b, 1.0f)) {
            out_profile->rgb_mod = true;
        }
    }

    float min_y = task->vertices[0].position.y;
    float max_y = min_y;
    float min_u = task->vertices[0].tex_coord.x;
    float max_u = min_u;
    float min_v = task->vertices[0].tex_coord.y;
    float max_v = min_v;
    for (int i = 0; i < 4; i++) {
        const SDL_Vertex* vertex = &task->vertices[i];
        const int px = (int)SDL_roundf(vertex->position.x);
        const int py = (int)SDL_roundf(vertex->position.y);
        if (!nearly_equal(vertex->position.x, (float)px) || !nearly_equal(vertex->position.y, (float)py)) {
            out_profile->integer_positions = false;
        }
        min_y = SDL_min(min_y, vertex->position.y);
        max_y = SDL_max(max_y, vertex->position.y);
        min_u = SDL_min(min_u, vertex->tex_coord.x);
        max_u = SDL_max(max_u, vertex->tex_coord.x);
        min_v = SDL_min(min_v, vertex->tex_coord.y);
        max_v = SDL_max(max_v, vertex->tex_coord.y);
    }

    const bool has_source_shape = (out_profile->source_width > 0) && (out_profile->source_height > 0);
    if (has_source_shape) {
        const float src_x_f = min_u * (float)out_profile->source_width;
        const float src_y_f = min_v * (float)out_profile->source_height;
        const float src_w_f = (max_u - min_u) * (float)out_profile->source_width;
        const float src_h_f = (max_v - min_v) * (float)out_profile->source_height;
        const int src_x = (int)SDL_roundf(src_x_f);
        const int src_y = (int)SDL_roundf(src_y_f);
        const int src_w = (int)SDL_roundf(src_w_f);
        const int src_h = (int)SDL_roundf(src_h_f);
        out_profile->integer_source_rect =
            nearly_equal(src_x_f, (float)src_x) && nearly_equal(src_y_f, (float)src_y) &&
            nearly_equal(src_w_f, (float)src_w) && nearly_equal(src_h_f, (float)src_h) && (src_w > 0) && (src_h > 0);
        if (out_profile->integer_source_rect) {
            out_profile->source_x = src_x;
            out_profile->source_y = src_y;
            out_profile->source_w = src_w;
            out_profile->source_h = src_h;
            out_profile->full_texture_source_rect =
                (src_x == 0) && (src_y == 0) && (src_w == out_profile->source_width) &&
                (src_h == out_profile->source_height);
        }
    }

    const bool rect_uv = textured_geometry_task_has_rect_uv(task, min_u, max_u, min_v, max_v);

    float top_xs[2] = { 0.0f, 0.0f };
    float bottom_xs[2] = { 0.0f, 0.0f };
    int top_count = 0;
    int bottom_count = 0;
    for (int i = 0; i < 4; i++) {
        const SDL_Vertex* vertex = &task->vertices[i];
        if (nearly_equal(vertex->position.y, min_y)) {
            if (top_count < SDL_arraysize(top_xs)) {
                top_xs[top_count++] = vertex->position.x;
            }
        } else if (nearly_equal(vertex->position.y, max_y)) {
            if (bottom_count < SDL_arraysize(bottom_xs)) {
                bottom_xs[bottom_count++] = vertex->position.x;
            }
        }
    }

    if (rect_uv) {
        out_profile->family_kind = SDL_GAME_RENDERER_TEXTURED_GEOMETRY_FALLBACK_FAMILY_RECT_UV_OTHER;
    }

    if ((top_count != SDL_arraysize(top_xs)) || (bottom_count != SDL_arraysize(bottom_xs))) {
        return true;
    }

    if (!out_profile->integer_positions) {
        return true;
    }

    if (top_xs[0] > top_xs[1]) {
        const float swap = top_xs[0];
        top_xs[0] = top_xs[1];
        top_xs[1] = swap;
    }
    if (bottom_xs[0] > bottom_xs[1]) {
        const float swap = bottom_xs[0];
        bottom_xs[0] = bottom_xs[1];
        bottom_xs[1] = swap;
    }

    const int top_y = (int)SDL_roundf(min_y);
    const int bottom_y = (int)SDL_roundf(max_y);
    const int top_left_x = (int)SDL_roundf(top_xs[0]);
    const int top_right_x = (int)SDL_roundf(top_xs[1]);
    const int bottom_left_x = (int)SDL_roundf(bottom_xs[0]);
    const int bottom_right_x = (int)SDL_roundf(bottom_xs[1]);
    if (!nearly_equal(min_y, (float)top_y) || !nearly_equal(max_y, (float)bottom_y) ||
        !nearly_equal(top_xs[0], (float)top_left_x) || !nearly_equal(top_xs[1], (float)top_right_x) ||
        !nearly_equal(bottom_xs[0], (float)bottom_left_x) || !nearly_equal(bottom_xs[1], (float)bottom_right_x)) {
        return true;
    }

    out_profile->dst_height = bottom_y - top_y;
    out_profile->dst_top_width = top_right_x - top_left_x;
    out_profile->dst_bottom_width = bottom_right_x - bottom_left_x;
    out_profile->dst_left_dx = bottom_left_x - top_left_x;
    out_profile->dst_right_dx = bottom_right_x - top_right_x;

    if (!rect_uv || (out_profile->dst_height <= 0) || (out_profile->dst_top_width <= 0) ||
        (out_profile->dst_bottom_width <= 0)) {
        return true;
    }

    if (out_profile->dst_left_dx == out_profile->dst_right_dx) {
        out_profile->family_kind = SDL_GAME_RENDERER_TEXTURED_GEOMETRY_FALLBACK_FAMILY_RECT_UV_PARALLELOGRAM;
    } else {
        out_profile->family_kind = SDL_GAME_RENDERER_TEXTURED_GEOMETRY_FALLBACK_FAMILY_RECT_UV_TRAPEZOID;
    }

    return true;
}

#if ENABLE_PERF_TELEMETRY
static bool textured_geometry_fallback_family_matches(
    const SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* entry,
    const TexturedGeometryFallbackFamilyProfile* profile) {
    if ((entry == NULL) || (profile == NULL)) {
        return false;
    }

    return (entry->texture_handle == profile->texture_handle) && (entry->palette_handle == profile->palette_handle) &&
           (entry->source_format == profile->source_format) && (entry->source_width == profile->source_width) &&
           (entry->source_height == profile->source_height) && (entry->family_kind == profile->family_kind) &&
           (entry->uniform_color == (profile->uniform_color ? 1 : 0)) &&
           (entry->opaque_color == (profile->opaque_color ? 1 : 0)) &&
           (entry->rgb_mod == (profile->rgb_mod ? 1 : 0)) &&
           (entry->integer_positions == (profile->integer_positions ? 1 : 0)) &&
           (entry->integer_source_rect == (profile->integer_source_rect ? 1 : 0)) &&
           (entry->full_texture_source_rect == (profile->full_texture_source_rect ? 1 : 0));
}
#endif

#if ENABLE_PERF_TELEMETRY
static void note_perf_capture_textured_geometry_family(
    const RenderTask* task,
    SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* families,
    int* family_count) {
    if (!frame_stats_extended_enabled || (task == NULL) || (families == NULL) || (family_count == NULL)) {
        return;
    }

    TexturedGeometryFallbackFamilyProfile profile;
    if (!analyze_textured_geometry_fallback_task(task, &profile)) {
        return;
    }

    SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* entry = NULL;
    for (int i = 0; i < *family_count; i++) {
        if (textured_geometry_fallback_family_matches(&families[i], &profile)) {
            entry = &families[i];
            break;
        }
    }

    if ((entry == NULL) && (*family_count < (int)SDL_arraysize(perf_capture_textured_geometry_recovered_families))) {
        entry = &families[*family_count];
        *family_count += 1;
        SDL_zero(*entry);
        entry->texture_handle = profile.texture_handle;
        entry->palette_handle = profile.palette_handle;
        entry->source_format = profile.source_format;
        entry->source_width = profile.source_width;
        entry->source_height = profile.source_height;
        entry->family_kind = profile.family_kind;
        entry->uniform_color = profile.uniform_color ? 1 : 0;
        entry->opaque_color = profile.opaque_color ? 1 : 0;
        entry->rgb_mod = profile.rgb_mod ? 1 : 0;
        entry->integer_positions = profile.integer_positions ? 1 : 0;
        entry->integer_source_rect = profile.integer_source_rect ? 1 : 0;
        entry->full_texture_source_rect = profile.full_texture_source_rect ? 1 : 0;
        entry->source_x_min = entry->source_x_max = profile.source_x;
        entry->source_y_min = entry->source_y_max = profile.source_y;
        entry->source_w_min = entry->source_w_max = profile.source_w;
        entry->source_h_min = entry->source_h_max = profile.source_h;
        entry->dst_height_min = entry->dst_height_max = profile.dst_height;
        entry->dst_top_width_min = entry->dst_top_width_max = profile.dst_top_width;
        entry->dst_bottom_width_min = entry->dst_bottom_width_max = profile.dst_bottom_width;
        entry->dst_left_dx_min = entry->dst_left_dx_max = profile.dst_left_dx;
        entry->dst_right_dx_min = entry->dst_right_dx_max = profile.dst_right_dx;
    }

    if (entry == NULL) {
        return;
    }

    entry->task_count += 1;
    entry->submitted_pixels += profile.submitted_pixels;
    update_textured_geometry_fallback_range(&entry->source_x_min, &entry->source_x_max, profile.source_x);
    update_textured_geometry_fallback_range(&entry->source_y_min, &entry->source_y_max, profile.source_y);
    update_textured_geometry_fallback_range(&entry->source_w_min, &entry->source_w_max, profile.source_w);
    update_textured_geometry_fallback_range(&entry->source_h_min, &entry->source_h_max, profile.source_h);
    update_textured_geometry_fallback_range(&entry->dst_height_min, &entry->dst_height_max, profile.dst_height);
    update_textured_geometry_fallback_range(&entry->dst_top_width_min, &entry->dst_top_width_max, profile.dst_top_width);
    update_textured_geometry_fallback_range(
        &entry->dst_bottom_width_min, &entry->dst_bottom_width_max, profile.dst_bottom_width);
    update_textured_geometry_fallback_range(&entry->dst_left_dx_min, &entry->dst_left_dx_max, profile.dst_left_dx);
    update_textured_geometry_fallback_range(&entry->dst_right_dx_min, &entry->dst_right_dx_max, profile.dst_right_dx);
}

static void note_perf_capture_textured_geometry_recovered_family(const RenderTask* task) {
    note_perf_capture_textured_geometry_family(
        task, perf_capture_textured_geometry_recovered_families, &perf_capture_textured_geometry_recovered_family_count);
}

static void note_perf_capture_textured_geometry_fallback_family(const RenderTask* task) {
    note_perf_capture_textured_geometry_family(
        task, perf_capture_textured_geometry_fallback_families, &perf_capture_textured_geometry_fallback_family_count);
}
#endif

static void reset_perf_capture_unlock_locality_shadow_slot(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    SDL_free(perf_capture_unlock_locality_shadow_pixels[texture_index]);
    perf_capture_unlock_locality_shadow_pixels[texture_index] = NULL;
    perf_capture_unlock_locality_shadow_size[texture_index] = 0;
    perf_capture_unlock_locality_shadow_valid[texture_index] = false;
}

static void clear_texture_logical_identity(TextureLogicalIdentity* identity) {
    if (identity == NULL) {
        return;
    }

    *identity = (TextureLogicalIdentity){
        .source_kind = SDL_GAME_RENDERER_TEXTURE_LOGICAL_SOURCE_UNKNOWN,
        .valid = false,
        .ix_num = -1,
        .ix_num_first = -1,
        .slot_index = -1,
        .chunk_index = -1,
        .texture_total = -1,
    };
}

static void clear_current_texture_logical_identity_slot(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    clear_texture_logical_identity(&current_texture_logical_identity_by_texture[texture_index]);
}

static void reset_perf_capture_texture_logical_identity_slot(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    PerfCaptureTextureLogicalIdentity* capture_identity = &perf_capture_texture_logical_identity_by_texture[texture_index];
    clear_texture_logical_identity(&capture_identity->identity);
    capture_identity->mixed = false;
    capture_identity->registrations = 0;
    capture_identity->last_seen_serial = 0;
}

static bool texture_logical_identity_equals(const TextureLogicalIdentity* lhs, const TextureLogicalIdentity* rhs) {
    if ((lhs == NULL) || (rhs == NULL)) {
        return false;
    }

    if (lhs->valid != rhs->valid) {
        return false;
    }
    if (!lhs->valid) {
        return true;
    }

    return (lhs->source_kind == rhs->source_kind) && (lhs->ix_num == rhs->ix_num) &&
           (lhs->ix_num_first == rhs->ix_num_first) && (lhs->slot_index == rhs->slot_index) &&
           (lhs->chunk_index == rhs->chunk_index) && (lhs->texture_total == rhs->texture_total);
}

static void note_perf_capture_texture_logical_identity(int texture_index) {
    if (!frame_stats_extended_enabled || (texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    const TextureLogicalIdentity* current_identity = &current_texture_logical_identity_by_texture[texture_index];
    const Uint32 current_serial = current_texture_logical_identity_serial_by_texture[texture_index];
    if (!current_identity->valid || (current_serial == 0)) {
        return;
    }

    PerfCaptureTextureLogicalIdentity* capture_identity = &perf_capture_texture_logical_identity_by_texture[texture_index];
    if (capture_identity->last_seen_serial == current_serial) {
        return;
    }

    capture_identity->last_seen_serial = current_serial;
    capture_identity->registrations += 1;
    if (capture_identity->mixed) {
        return;
    }

    if (!capture_identity->identity.valid) {
        capture_identity->identity = *current_identity;
        return;
    }

    if (!texture_logical_identity_equals(&capture_identity->identity, current_identity)) {
        capture_identity->mixed = true;
        clear_texture_logical_identity(&capture_identity->identity);
    }
}

static void copy_perf_capture_texture_logical_identity(int texture_index,
                                                       int* out_known,
                                                       int* out_mixed,
                                                       Uint32* out_registrations,
                                                       SDLGameRenderer_TextureLogicalSourceKind* out_source_kind,
                                                       int* out_ix_num,
                                                       int* out_ix_num_first,
                                                       int* out_slot_index,
                                                       int* out_chunk_index,
                                                       int* out_texture_total) {
    const bool valid_slot = (texture_index >= 0) && (texture_index < FL_TEXTURE_MAX);
    const PerfCaptureTextureLogicalIdentity* capture_identity =
        valid_slot ? &perf_capture_texture_logical_identity_by_texture[texture_index] : NULL;
    const bool known = (capture_identity != NULL) && capture_identity->identity.valid;

    if (out_known != NULL) {
        *out_known = known ? 1 : 0;
    }
    if (out_mixed != NULL) {
        *out_mixed = ((capture_identity != NULL) && capture_identity->mixed) ? 1 : 0;
    }
    if (out_registrations != NULL) {
        *out_registrations = capture_identity != NULL ? capture_identity->registrations : 0;
    }
    if (out_source_kind != NULL) {
        *out_source_kind =
            known ? capture_identity->identity.source_kind : SDL_GAME_RENDERER_TEXTURE_LOGICAL_SOURCE_UNKNOWN;
    }
    if (out_ix_num != NULL) {
        *out_ix_num = known ? capture_identity->identity.ix_num : -1;
    }
    if (out_ix_num_first != NULL) {
        *out_ix_num_first = known ? capture_identity->identity.ix_num_first : -1;
    }
    if (out_slot_index != NULL) {
        *out_slot_index = known ? capture_identity->identity.slot_index : -1;
    }
    if (out_chunk_index != NULL) {
        *out_chunk_index = known ? capture_identity->identity.chunk_index : -1;
    }
    if (out_texture_total != NULL) {
        *out_texture_total = known ? capture_identity->identity.texture_total : -1;
    }
}

static void reset_perf_capture_refresh_lifetime_slot(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    perf_capture_current_lifetime_refresh_attempts_by_texture[texture_index] = 0;
    perf_capture_current_lifetime_partial_refresh_attempts_by_texture[texture_index] = 0;
    perf_capture_current_lifetime_full_refresh_attempts_by_texture[texture_index] = 0;
    perf_capture_current_lifetime_full_no_usable_dirty_rect_attempts_by_texture[texture_index] = 0;
    perf_capture_current_lifetime_software_surface_access_dirty_texture_same_frame_by_texture[texture_index] = 0;
    perf_capture_current_lifetime_software_surface_access_dirty_texture_carried_by_texture[texture_index] = 0;
    perf_capture_current_lifetime_software_surface_access_dirty_palette_same_frame_by_texture[texture_index] = 0;
    perf_capture_current_lifetime_software_surface_access_dirty_palette_carried_by_texture[texture_index] = 0;
    perf_capture_current_lifetime_software_surface_access_dirty_palette_changed_same_frame_by_texture[texture_index] = 0;
    perf_capture_current_lifetime_software_surface_access_dirty_palette_changed_carried_by_texture[texture_index] = 0;
    perf_capture_current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame_by_texture[texture_index] =
        0;
    perf_capture_current_lifetime_software_surface_access_dirty_palette_unchanged_carried_by_texture[texture_index] =
        0;
    perf_capture_current_lifetime_software_surface_access_cold_by_texture[texture_index] = 0;
}

static void reset_perf_capture_unlock_locality_texture_slot(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    perf_capture_unlock_locality_tracked_by_texture[texture_index] = 0;
    perf_capture_unlock_locality_zero_delta_by_texture[texture_index] = 0;
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
    perf_capture_dirty_rect_record_calls_by_texture[texture_index] = 0;
    perf_capture_dirty_rect_retained_after_unlock_by_texture[texture_index] = 0;
    perf_capture_dirty_rect_clear_stale_before_record_by_texture[texture_index] = 0;
    perf_capture_dirty_rect_clear_unlock_unused_by_texture[texture_index] = 0;
    perf_capture_dirty_rect_clear_access_unused_by_texture[texture_index] = 0;
    perf_capture_dirty_rect_clear_explicit_by_texture[texture_index] = 0;
    clear_perf_capture_compare_dirty_rect_pending_index(texture_index);
    reset_perf_capture_unlock_locality_shadow_slot(texture_index);
}

static void reset_perf_capture_texture_renew_slot(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    perf_capture_texture_renew_pending_rects[texture_index] = (SDL_Rect){ 0, 0, 0, 0 };
    perf_capture_texture_renew_pending_rect_valid[texture_index] = false;
    perf_capture_texture_renew_pending_tile_masks[texture_index] = 0;
    perf_capture_texture_renew_chunk_calls_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batches_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batches_without_rect_by_texture[texture_index] = 0;
    perf_capture_texture_renew_chunk_pixels_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batch_bbox_pixels_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batch_max_bbox_pixels_by_texture[texture_index] = 0;
    perf_capture_texture_renew_chunk_8x8_calls_by_texture[texture_index] = 0;
    perf_capture_texture_renew_chunk_16x16_calls_by_texture[texture_index] = 0;
    perf_capture_texture_renew_chunk_32x32_calls_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batch_32x32_covered_tiles_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batch_32x32_max_covered_tiles_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batch_32x32_component_count_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batch_32x32_max_component_count_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batch_32x32_multi_component_batches_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batch_32x32_largest_component_tiles_by_texture[texture_index] = 0;
    perf_capture_texture_renew_batch_32x32_max_largest_component_tiles_by_texture[texture_index] = 0;
}

static Uint64 make_perf_capture_renew_tile_mask(int x, int y, int w, int h) {
    // The renew and compare-dirty candidates we measure here both stay on 256x256 source surfaces, so an 8x8 32px
    // grid is enough for the coarse coverage/component telemetry.
    const int tile_x0 = x / perf_capture_renew_tile_size;
    const int tile_y0 = y / perf_capture_renew_tile_size;
    const int tile_x1 = (x + w - 1) / perf_capture_renew_tile_size;
    const int tile_y1 = (y + h - 1) / perf_capture_renew_tile_size;
    if ((tile_x0 < 0) || (tile_y0 < 0) || (tile_x1 >= perf_capture_renew_tile_grid_dim) ||
        (tile_y1 >= perf_capture_renew_tile_grid_dim)) {
        return 0;
    }

    Uint64 mask = 0;
    for (int tile_y = tile_y0; tile_y <= tile_y1; tile_y++) {
        for (int tile_x = tile_x0; tile_x <= tile_x1; tile_x++) {
            mask |= ((Uint64)1) << (tile_y * perf_capture_renew_tile_grid_dim + tile_x);
        }
    }

    return mask;
}

static int count_perf_capture_renew_tile_components(Uint64 mask, int* out_largest_component_tiles) {
    int component_count = 0;
    int largest_component_tiles = 0;
    bool visited[perf_capture_renew_tile_count] = { false };

    for (int seed = 0; seed < perf_capture_renew_tile_count; seed++) {
        if ((((mask >> seed) & ((Uint64)1)) == 0) || visited[seed]) {
            continue;
        }

        component_count += 1;
        int stack[perf_capture_renew_tile_count];
        int stack_count = 0;
        int component_tiles = 0;
        stack[stack_count++] = seed;
        visited[seed] = true;

        while (stack_count > 0) {
            const int tile_index = stack[--stack_count];
            const int tile_x = tile_index % perf_capture_renew_tile_grid_dim;
            const int tile_y = tile_index / perf_capture_renew_tile_grid_dim;
            component_tiles += 1;

            if (tile_x > 0) {
                const int west = tile_index - 1;
                if ((((mask >> west) & ((Uint64)1)) != 0) && !visited[west]) {
                    visited[west] = true;
                    stack[stack_count++] = west;
                }
            }
            if ((tile_x + 1) < perf_capture_renew_tile_grid_dim) {
                const int east = tile_index + 1;
                if ((((mask >> east) & ((Uint64)1)) != 0) && !visited[east]) {
                    visited[east] = true;
                    stack[stack_count++] = east;
                }
            }
            if (tile_y > 0) {
                const int north = tile_index - perf_capture_renew_tile_grid_dim;
                if ((((mask >> north) & ((Uint64)1)) != 0) && !visited[north]) {
                    visited[north] = true;
                    stack[stack_count++] = north;
                }
            }
            if ((tile_y + 1) < perf_capture_renew_tile_grid_dim) {
                const int south = tile_index + perf_capture_renew_tile_grid_dim;
                if ((((mask >> south) & ((Uint64)1)) != 0) && !visited[south]) {
                    visited[south] = true;
                    stack[stack_count++] = south;
                }
            }
        }

        if (component_tiles > largest_component_tiles) {
            largest_component_tiles = component_tiles;
        }
    }

    if (out_largest_component_tiles != NULL) {
        *out_largest_component_tiles = largest_component_tiles;
    }
    return component_count;
}

static Uint64 texture_unlock_refresh_plan_pixels(const TextureUnlockRefreshPlan* plan) {
    Uint64 pixels = 0;

    if (plan == NULL) {
        return 0;
    }

    for (int rect_index = 0; rect_index < plan->rect_count; rect_index++) {
        const SDL_Rect rect = plan->rects[rect_index];
        if ((rect.w <= 0) || (rect.h <= 0)) {
            continue;
        }
        pixels += (Uint64)rect.w * (Uint64)rect.h;
    }

    return pixels;
}

static Uint64 texture_unlock_partial_refresh_max_pixels(const SDL_Surface* source_surface) {
    if (source_surface == NULL) {
        return 0;
    }

    return ((Uint64)source_surface->w * (Uint64)source_surface->h) / 4u;
}

static Uint64 compare_dirty_rect_partial_refresh_max_pixels(const SDL_Surface* source_surface) {
    if (source_surface == NULL) {
        return 0;
    }

    // Keep compare-dirty fallback slightly broader than explicit renew metadata: current native Remy telemetry leaves
    // seq 81 inside 3/8 while the still-riskier seq 82 residue remains above it.
    return ((Uint64)source_surface->w * (Uint64)source_surface->h * 3u) / 8u;
}

static bool texture_unlock_refresh_rect_is_valid(const SDL_Rect* rect, const SDL_Surface* source_surface) {
    if ((rect == NULL) || (source_surface == NULL) || (rect->w <= 0) || (rect->h <= 0) || (rect->x < 0) ||
        (rect->y < 0) || (rect->x + rect->w > source_surface->w) || (rect->y + rect->h > source_surface->h)) {
        return false;
    }

    return true;
}

static Uint64 texture_unlock_refresh_rect_pixels(const SDL_Rect* rect) {
    if ((rect == NULL) || (rect->w <= 0) || (rect->h <= 0)) {
        return 0;
    }

    return (Uint64)rect->w * (Uint64)rect->h;
}

static bool compare_dirty_row_mask_plan_fits_relaxed_partial_exception(const TextureUnlockRefreshPlan* plan,
                                                                       const SDL_Surface* source_surface,
                                                                       Uint64 max_partial_pixels) {
    if ((plan == NULL) || (source_surface == NULL) || (plan->rect_count != 2)) {
        return false;
    }

    const Uint64 max_relaxed_pixels =
        max_partial_pixels + ((Uint64)perf_capture_renew_tile_size * (Uint64)perf_capture_renew_tile_size);
    if (texture_unlock_refresh_plan_pixels(plan) > max_relaxed_pixels) {
        return false;
    }

    for (int rect_index = 0; rect_index < plan->rect_count; rect_index++) {
        const SDL_Rect* rect = &plan->rects[rect_index];
        if (!texture_unlock_refresh_rect_is_valid(rect, source_surface)) {
            return false;
        }
        if (texture_unlock_refresh_rect_pixels(rect) > max_partial_pixels) {
            return false;
        }
    }

    return true;
}

static bool build_texture_unlock_refresh_plan_from_tile_mask(Uint64 tile_mask, TextureUnlockRefreshPlan* out_plan) {
    if ((tile_mask == 0) || (out_plan == NULL)) {
        return false;
    }

    TextureUnlockRefreshPlan plan = { 0 };
    bool visited[perf_capture_renew_tile_count] = { false };

    for (int seed = 0; seed < perf_capture_renew_tile_count; seed++) {
        if ((((tile_mask >> seed) & ((Uint64)1)) == 0) || visited[seed]) {
            continue;
        }
        if (plan.rect_count >= texture_unlock_multi_rect_max) {
            return false;
        }

        int stack[perf_capture_renew_tile_count];
        int stack_count = 0;
        int min_tile_x = perf_capture_renew_tile_grid_dim;
        int min_tile_y = perf_capture_renew_tile_grid_dim;
        int max_tile_x = 0;
        int max_tile_y = 0;
        stack[stack_count++] = seed;
        visited[seed] = true;

        while (stack_count > 0) {
            const int tile_index = stack[--stack_count];
            const int tile_x = tile_index % perf_capture_renew_tile_grid_dim;
            const int tile_y = tile_index / perf_capture_renew_tile_grid_dim;
            min_tile_x = SDL_min(min_tile_x, tile_x);
            min_tile_y = SDL_min(min_tile_y, tile_y);
            max_tile_x = SDL_max(max_tile_x, tile_x);
            max_tile_y = SDL_max(max_tile_y, tile_y);

            if (tile_x > 0) {
                const int west = tile_index - 1;
                if ((((tile_mask >> west) & ((Uint64)1)) != 0) && !visited[west]) {
                    visited[west] = true;
                    stack[stack_count++] = west;
                }
            }
            if ((tile_x + 1) < perf_capture_renew_tile_grid_dim) {
                const int east = tile_index + 1;
                if ((((tile_mask >> east) & ((Uint64)1)) != 0) && !visited[east]) {
                    visited[east] = true;
                    stack[stack_count++] = east;
                }
            }
            if (tile_y > 0) {
                const int north = tile_index - perf_capture_renew_tile_grid_dim;
                if ((((tile_mask >> north) & ((Uint64)1)) != 0) && !visited[north]) {
                    visited[north] = true;
                    stack[stack_count++] = north;
                }
            }
            if ((tile_y + 1) < perf_capture_renew_tile_grid_dim) {
                const int south = tile_index + perf_capture_renew_tile_grid_dim;
                if ((((tile_mask >> south) & ((Uint64)1)) != 0) && !visited[south]) {
                    visited[south] = true;
                    stack[stack_count++] = south;
                }
            }
        }

        plan.rects[plan.rect_count++] = (SDL_Rect){
            min_tile_x * perf_capture_renew_tile_size,
            min_tile_y * perf_capture_renew_tile_size,
            (max_tile_x - min_tile_x + 1) * perf_capture_renew_tile_size,
            (max_tile_y - min_tile_y + 1) * perf_capture_renew_tile_size,
        };
    }

    *out_plan = plan;
    return plan.rect_count > 0;
}

static void note_perf_capture_texture_unlock_locality(int texture_handle, const SDL_Surface* source_surface) {
    if ((source_surface == NULL) || (texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return;
    }

    const bool capture = frame_stats_extended_enabled;
    const int texture_index = texture_handle - 1;
    if (capture) {
        note_perf_capture_texture_logical_identity(texture_index);
    }
    if (source_surface->format != SDL_PIXELFORMAT_INDEX8) {
        if (capture) {
            const bool have_index8_history = (perf_capture_unlock_locality_tracked_by_texture[texture_index] > 0) ||
                                             (perf_capture_unlock_locality_baseline_skips_by_texture[texture_index] > 0);
            frame_stats.texture_unlock_locality_index8_non_index8_skips += 1;
            perf_capture_unlock_locality_telemetry.index8_non_index8_skips += 1;
            perf_capture_unlock_locality_non_index8_skips_by_texture[texture_index] += 1;
            perf_capture_unlock_locality_whole_capture_non_index8_skips_by_texture[texture_index] += 1;
            if (have_index8_history) {
                perf_capture_unlock_locality_shape_mixed_by_texture[texture_index] = true;
            }
        }
        perf_capture_unlock_locality_shadow_valid[texture_index] = false;
        return;
    }

    const int width = source_surface->w;
    const int height = source_surface->h;
    if ((width <= 0) || (height <= 0)) {
        return;
    }

    const bool track_runtime_compare_dirty =
        software_frame_mode_active && (width == 256) && (height == 256) &&
        texture_index_has_software_surface_cache_variants(texture_index);
    if (!capture && !track_runtime_compare_dirty) {
        return;
    }

    const size_t row_bytes = (size_t)width;
    const size_t shadow_size = row_bytes * (size_t)height;
    if (capture) {
        const bool have_existing_shape =
            (perf_capture_unlock_locality_tracked_by_texture[texture_index] > 0) ||
            (perf_capture_unlock_locality_baseline_skips_by_texture[texture_index] > 0) ||
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
    }

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
        if (capture) {
            frame_stats.texture_unlock_locality_index8_baseline_skips += 1;
            perf_capture_unlock_locality_telemetry.index8_baseline_skips += 1;
            perf_capture_unlock_locality_baseline_skips_by_texture[texture_index] += 1;
            perf_capture_unlock_locality_whole_capture_baseline_skips_by_texture[texture_index] += 1;
        }
        return;
    }

    Uint64 changed_pixels = 0;
    Uint64 changed_rows = 0;
    int min_x = width;
    int max_x = -1;
    int min_y = height;
    int max_y = -1;
    Uint64 compare_dirty_row_tile_mask = 0;
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
            if (track_runtime_compare_dirty) {
                compare_dirty_row_tile_mask |=
                    make_perf_capture_renew_tile_mask(row_min_x, y, row_max_x - row_min_x + 1, 1);
            }
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
    if (capture) {
        frame_stats.texture_unlock_locality_index8_tracked += 1;
        if (changed_pixels == 0) {
            perf_capture_unlock_locality_telemetry.index8_zero_delta_unlocks += 1;
            perf_capture_unlock_locality_zero_delta_by_texture[texture_index] += 1;
            perf_capture_unlock_locality_whole_capture_zero_delta_by_texture[texture_index] += 1;
        }
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
        perf_capture_unlock_locality_whole_capture_tracked_by_texture[texture_index] += 1;
        perf_capture_unlock_locality_whole_capture_source_pixels_by_texture[texture_index] += source_pixels;
        perf_capture_unlock_locality_whole_capture_changed_pixels_by_texture[texture_index] += changed_pixels;
        perf_capture_unlock_locality_whole_capture_changed_rows_by_texture[texture_index] += changed_rows;
        perf_capture_unlock_locality_whole_capture_changed_bbox_pixels_by_texture[texture_index] += changed_bbox_pixels;
    }

    if (track_runtime_compare_dirty && (changed_rows > 0)) {
        const SDL_Rect dirty_rect = { min_x, min_y, max_x - min_x + 1, max_y - min_y + 1 };
        note_perf_capture_compare_dirty_rect_candidate(texture_index, &dirty_rect, compare_dirty_row_tile_mask);
    }
}

static void note_perf_capture_compare_dirty_rect_candidate(int texture_index,
                                                           const SDL_Rect* dirty_rect,
                                                           Uint64 row_tile_mask) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX) || (dirty_rect == NULL) || (dirty_rect->w <= 0) ||
        (dirty_rect->h <= 0)) {
        return;
    }

    perf_capture_compare_dirty_rect_pending_tile_masks[texture_index] |=
        make_perf_capture_renew_tile_mask(dirty_rect->x, dirty_rect->y, dirty_rect->w, dirty_rect->h);
    if (row_tile_mask != 0) {
        perf_capture_compare_dirty_row_mask_pending_tile_masks[texture_index] |= row_tile_mask;
    }

    if (!perf_capture_compare_dirty_rect_pending_valid[texture_index]) {
        perf_capture_compare_dirty_rect_pending_rects[texture_index] = *dirty_rect;
        perf_capture_compare_dirty_rect_pending_valid[texture_index] = true;
        perf_capture_compare_dirty_rect_pending_unlock_counts[texture_index] = 1;
        return;
    }

    SDL_Rect* accumulated_rect = &perf_capture_compare_dirty_rect_pending_rects[texture_index];
    const int union_x0 = SDL_min(accumulated_rect->x, dirty_rect->x);
    const int union_y0 = SDL_min(accumulated_rect->y, dirty_rect->y);
    const int union_x1 = SDL_max(accumulated_rect->x + accumulated_rect->w - 1, dirty_rect->x + dirty_rect->w - 1);
    const int union_y1 = SDL_max(accumulated_rect->y + accumulated_rect->h - 1, dirty_rect->y + dirty_rect->h - 1);
    accumulated_rect->x = union_x0;
    accumulated_rect->y = union_y0;
    accumulated_rect->w = union_x1 - union_x0 + 1;
    accumulated_rect->h = union_y1 - union_y0 + 1;
    perf_capture_compare_dirty_rect_pending_unlock_counts[texture_index] += 1;
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

static void note_perf_capture_dirty_rect_record(int texture_index) {
    if (!frame_stats_extended_enabled || (texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    perf_capture_dirty_rect_record_calls_by_texture[texture_index] += 1;
}

static void note_perf_capture_dirty_rect_retained_after_unlock(int texture_index) {
    if (!frame_stats_extended_enabled || (texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    perf_capture_dirty_rect_retained_after_unlock_by_texture[texture_index] += 1;
}

static void note_perf_capture_dirty_rect_clear(int texture_index, TextureUnlockDirtyRectClearReason reason) {
    if (!frame_stats_extended_enabled || (texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    switch (reason) {
    case TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_STALE_BEFORE_RECORD:
        perf_capture_dirty_rect_clear_stale_before_record_by_texture[texture_index] += 1;
        break;
    case TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_UNLOCK_UNUSED:
        perf_capture_dirty_rect_clear_unlock_unused_by_texture[texture_index] += 1;
        break;
    case TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_ACCESS_UNUSED:
        perf_capture_dirty_rect_clear_access_unused_by_texture[texture_index] += 1;
        break;
    case TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_EXPLICIT:
        perf_capture_dirty_rect_clear_explicit_by_texture[texture_index] += 1;
        break;
    default:
        break;
    }
}

static void clear_perf_capture_compare_dirty_rect_pending_index(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    perf_capture_compare_dirty_rect_pending_rects[texture_index] = (SDL_Rect){ 0, 0, 0, 0 };
    perf_capture_compare_dirty_rect_pending_valid[texture_index] = false;
    perf_capture_compare_dirty_rect_pending_unlock_counts[texture_index] = 0;
    perf_capture_compare_dirty_rect_pending_tile_masks[texture_index] = 0;
    perf_capture_compare_dirty_row_mask_pending_tile_masks[texture_index] = 0;
}

static bool software_surface_cache_slot_has_texture_unlock_dirty_reason(int texture_index, int palette_handle) {
    return (texture_index >= 0) && (texture_index < FL_TEXTURE_MAX) && (palette_handle >= 0) &&
           (palette_handle <= FL_PALETTE_MAX) && (software_surface_cache[texture_index][palette_handle] != NULL) &&
           (software_surface_cache_runtime_dirty_reason[texture_index][palette_handle] == CACHE_DIRTY_REASON_TEXTURE_UNLOCK);
}

static bool texture_index_has_software_surface_cache_variants(int texture_index) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return false;
    }

    for (int palette_handle = 0; palette_handle <= FL_PALETTE_MAX; palette_handle++) {
        if (software_surface_cache[texture_index][palette_handle] != NULL) {
            return true;
        }
    }

    return false;
}

static void set_software_surface_cache_slot_state(int texture_index,
                                                  int palette_handle,
                                                  SDL_Surface* surface,
                                                  Uint8 runtime_dirty_reason) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX) || (palette_handle < 0) ||
        (palette_handle > FL_PALETTE_MAX)) {
        return;
    }

    const bool was_texture_unlock_dirty =
        software_surface_cache_slot_has_texture_unlock_dirty_reason(texture_index, palette_handle);
    software_surface_cache[texture_index][palette_handle] = surface;
    software_surface_cache_runtime_dirty_reason[texture_index][palette_handle] = runtime_dirty_reason;
    const bool is_texture_unlock_dirty =
        (surface != NULL) && (runtime_dirty_reason == CACHE_DIRTY_REASON_TEXTURE_UNLOCK);

    if (was_texture_unlock_dirty == is_texture_unlock_dirty) {
        return;
    }

    if (is_texture_unlock_dirty) {
        software_surface_cache_texture_unlock_dirty_variant_counts[texture_index] += 1;
    } else if (software_surface_cache_texture_unlock_dirty_variant_counts[texture_index] > 0) {
        software_surface_cache_texture_unlock_dirty_variant_counts[texture_index] -= 1;
    }
}

static bool should_keep_dirty_cache_entries(CacheDirtyReason reason) {
    return software_frame_mode_active &&
           ((reason == CACHE_DIRTY_REASON_TEXTURE_UNLOCK) || (reason == CACHE_DIRTY_REASON_PALETTE_UNLOCK));
}

static bool should_refresh_dirty_cache_entry(Uint8 dirty_reason) {
    return should_keep_dirty_cache_entries((CacheDirtyReason)dirty_reason);
}

static void clear_software_surface_full_opaque_row_mask(int texture_index, int palette_handle) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX) || (palette_handle < 0) ||
        (palette_handle > FL_PALETTE_MAX)) {
        return;
    }

    SDL_zero(software_surface_full_opaque_row_masks[texture_index][palette_handle]);
    software_surface_full_opaque_row_masks_valid[texture_index][palette_handle] = false;
}

static const Uint64* get_software_surface_full_opaque_row_mask(unsigned int th, const SDL_Surface* surface) {
    const int texture_handle = LO_16_BITS(th);
    const int palette_handle = HI_16_BITS(th);
    if ((texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX) || (palette_handle < 0) ||
        (palette_handle > FL_PALETTE_MAX) || (surface == NULL) || (surface->format != SDL_PIXELFORMAT_ARGB8888) ||
        (surface->w != 256) || (surface->h != 256)) {
        return NULL;
    }

    const int texture_index = texture_handle - 1;
    Uint64* row_mask = software_surface_full_opaque_row_masks[texture_index][palette_handle];
    if (!software_surface_full_opaque_row_masks_valid[texture_index][palette_handle]) {
        SDL_memset(row_mask, 0, sizeof(software_surface_full_opaque_row_masks[texture_index][palette_handle]));
        const Uint32* src_pixels = (const Uint32*)surface->pixels;
        const int src_pitch = surface->pitch / (int)sizeof(Uint32);
        for (int row = 0; row < surface->h; row++) {
            const Uint32* src_row = src_pixels + (row * src_pitch);
            bool full_opaque = true;
            for (int col = 0; col < surface->w; col++) {
                if (((src_row[col] >> 24) & 0xFFu) != 0xFFu) {
                    full_opaque = false;
                    break;
                }
            }
            if (full_opaque) {
                row_mask[row >> 6] |= ((Uint64)1) << (row & 63);
            }
        }
        software_surface_full_opaque_row_masks_valid[texture_index][palette_handle] = true;
    }

    return row_mask;
}

static void clear_texture_unlock_dirty_rect_index(int texture_index, TextureUnlockDirtyRectClearReason reason) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    if (texture_unlock_dirty_rect_valid[texture_index] || texture_unlock_dirty_tile_mask_valid[texture_index]) {
        note_perf_capture_dirty_rect_clear(texture_index, reason);
    }
    texture_unlock_dirty_rects[texture_index] = (SDL_Rect){ 0, 0, 0, 0 };
    texture_unlock_dirty_rect_valid[texture_index] = false;
    texture_unlock_dirty_tile_masks[texture_index] = 0;
    texture_unlock_dirty_tile_mask_valid[texture_index] = false;
    clear_perf_capture_compare_dirty_rect_pending_index(texture_index);
}

static void clear_texture_unlock_dirty_rect_if_unused(int texture_index, TextureUnlockDirtyRectClearReason reason) {
    if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX) ||
        (!texture_unlock_dirty_rect_valid[texture_index] && !texture_unlock_dirty_tile_mask_valid[texture_index] &&
         !perf_capture_compare_dirty_rect_pending_valid[texture_index])) {
        return;
    }

    if (software_surface_cache_texture_unlock_dirty_variant_counts[texture_index] > 0) {
        return;
    }

    clear_texture_unlock_dirty_rect_index(texture_index, reason);
}

static void note_perf_capture_compare_dirty_rect_refresh_candidate(unsigned int th,
                                                                   Uint8 dirty_reason,
                                                                   const SDL_Surface* source_surface) {
    if (!frame_stats_extended_enabled || (source_surface == NULL) || (dirty_reason != CACHE_DIRTY_REASON_TEXTURE_UNLOCK)) {
        return;
    }

    if ((source_surface->format != SDL_PIXELFORMAT_INDEX8) || (source_surface->w != 256) || (source_surface->h != 256)) {
        return;
    }

    const int texture_handle = LO_16_BITS(th);
    if ((texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return;
    }

    const int texture_index = texture_handle - 1;
    perf_capture_compare_dirty_rect_refresh_attempts_by_texture[texture_index] += 1;

    if (!perf_capture_compare_dirty_rect_pending_valid[texture_index]) {
        perf_capture_compare_dirty_rect_no_usable_candidate_refresh_attempts_by_texture[texture_index] += 1;
        return;
    }

    const SDL_Rect dirty_rect = perf_capture_compare_dirty_rect_pending_rects[texture_index];
    if ((dirty_rect.w <= 0) || (dirty_rect.h <= 0) || (dirty_rect.x < 0) || (dirty_rect.y < 0) ||
        (dirty_rect.x + dirty_rect.w > source_surface->w) || (dirty_rect.y + dirty_rect.h > source_surface->h)) {
        perf_capture_compare_dirty_rect_no_usable_candidate_refresh_attempts_by_texture[texture_index] += 1;
        return;
    }

    const Uint64 dirty_pixels = (Uint64)dirty_rect.w * (Uint64)dirty_rect.h;
    const Uint64 max_partial_pixels = compare_dirty_rect_partial_refresh_max_pixels(source_surface);
    perf_capture_compare_dirty_rect_refresh_bbox_pixels_by_texture[texture_index] += dirty_pixels;
    perf_capture_compare_dirty_rect_refresh_pending_unlocks_by_texture[texture_index] +=
        perf_capture_compare_dirty_rect_pending_unlock_counts[texture_index];
    if (dirty_pixels > perf_capture_compare_dirty_rect_refresh_max_bbox_pixels_by_texture[texture_index]) {
        perf_capture_compare_dirty_rect_refresh_max_bbox_pixels_by_texture[texture_index] = dirty_pixels;
    }
    if ((Uint64)perf_capture_compare_dirty_rect_pending_unlock_counts[texture_index] >
        perf_capture_compare_dirty_rect_refresh_max_pending_unlocks_by_texture[texture_index]) {
        perf_capture_compare_dirty_rect_refresh_max_pending_unlocks_by_texture[texture_index] =
            (Uint64)perf_capture_compare_dirty_rect_pending_unlock_counts[texture_index];
    }

    const Uint64 pending_tile_mask = perf_capture_compare_dirty_rect_pending_tile_masks[texture_index];
    const Uint64 covered_tiles = (Uint64)__builtin_popcountll((unsigned long long)pending_tile_mask);
    int largest_component_tiles = 0;
    const int component_count = count_perf_capture_renew_tile_components(pending_tile_mask, &largest_component_tiles);
    perf_capture_compare_dirty_rect_refresh_32x32_covered_tiles_by_texture[texture_index] += covered_tiles;
    if (covered_tiles > perf_capture_compare_dirty_rect_refresh_32x32_max_covered_tiles_by_texture[texture_index]) {
        perf_capture_compare_dirty_rect_refresh_32x32_max_covered_tiles_by_texture[texture_index] = covered_tiles;
    }
    perf_capture_compare_dirty_rect_refresh_32x32_component_count_by_texture[texture_index] += (Uint64)component_count;
    if ((Uint64)component_count >
        perf_capture_compare_dirty_rect_refresh_32x32_max_component_count_by_texture[texture_index]) {
        perf_capture_compare_dirty_rect_refresh_32x32_max_component_count_by_texture[texture_index] =
            (Uint64)component_count;
    }
    if (component_count > 1) {
        perf_capture_compare_dirty_rect_refresh_32x32_multi_component_refresh_attempts_by_texture[texture_index] += 1;
    }
    perf_capture_compare_dirty_rect_refresh_32x32_largest_component_tiles_by_texture[texture_index] +=
        (Uint64)largest_component_tiles;
    if ((Uint64)largest_component_tiles >
        perf_capture_compare_dirty_rect_refresh_32x32_max_largest_component_tiles_by_texture[texture_index]) {
        perf_capture_compare_dirty_rect_refresh_32x32_max_largest_component_tiles_by_texture[texture_index] =
            (Uint64)largest_component_tiles;
    }

    if ((dirty_pixels > 0) && (dirty_pixels <= max_partial_pixels)) {
        perf_capture_compare_dirty_rect_partial_candidate_refresh_attempts_by_texture[texture_index] += 1;
    } else {
        perf_capture_compare_dirty_rect_oversized_candidate_refresh_attempts_by_texture[texture_index] += 1;
    }

    note_perf_capture_compare_dirty_row_mask_refresh_candidate(texture_index, source_surface, max_partial_pixels);
}

static void note_perf_capture_compare_dirty_row_mask_refresh_candidate(int texture_index,
                                                                       const SDL_Surface* source_surface,
                                                                       Uint64 current_max_partial_pixels) {
    if ((source_surface == NULL) || (texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return;
    }

    const Uint64 pending_row_mask = perf_capture_compare_dirty_row_mask_pending_tile_masks[texture_index];
    if (pending_row_mask == 0) {
        perf_capture_compare_dirty_row_mask_no_usable_candidate_refresh_attempts_by_texture[texture_index] += 1;
        return;
    }

    const Uint64 covered_tiles = (Uint64)__builtin_popcountll((unsigned long long)pending_row_mask);
    int largest_component_tiles = 0;
    const int component_count = count_perf_capture_renew_tile_components(pending_row_mask, &largest_component_tiles);
    perf_capture_compare_dirty_row_mask_32x32_covered_tiles_by_texture[texture_index] += covered_tiles;
    if (covered_tiles > perf_capture_compare_dirty_row_mask_32x32_max_covered_tiles_by_texture[texture_index]) {
        perf_capture_compare_dirty_row_mask_32x32_max_covered_tiles_by_texture[texture_index] = covered_tiles;
    }
    perf_capture_compare_dirty_row_mask_32x32_component_count_by_texture[texture_index] += (Uint64)component_count;
    if ((Uint64)component_count >
        perf_capture_compare_dirty_row_mask_32x32_max_component_count_by_texture[texture_index]) {
        perf_capture_compare_dirty_row_mask_32x32_max_component_count_by_texture[texture_index] = (Uint64)component_count;
    }
    if (component_count > 1) {
        perf_capture_compare_dirty_row_mask_32x32_multi_component_refresh_attempts_by_texture[texture_index] += 1;
    }
    perf_capture_compare_dirty_row_mask_32x32_largest_component_tiles_by_texture[texture_index] +=
        (Uint64)largest_component_tiles;
    if ((Uint64)largest_component_tiles >
        perf_capture_compare_dirty_row_mask_32x32_max_largest_component_tiles_by_texture[texture_index]) {
        perf_capture_compare_dirty_row_mask_32x32_max_largest_component_tiles_by_texture[texture_index] =
            (Uint64)largest_component_tiles;
    }

    TextureUnlockRefreshPlan row_plan = { 0 };
    if (!build_texture_unlock_refresh_plan_from_tile_mask(pending_row_mask, &row_plan)) {
        perf_capture_compare_dirty_row_mask_no_usable_candidate_refresh_attempts_by_texture[texture_index] += 1;
        return;
    }

    const Uint64 row_plan_pixels = texture_unlock_refresh_plan_pixels(&row_plan);
    if (row_plan_pixels == 0) {
        perf_capture_compare_dirty_row_mask_no_usable_candidate_refresh_attempts_by_texture[texture_index] += 1;
        return;
    }

    perf_capture_compare_dirty_row_mask_plan_pixels_by_texture[texture_index] += row_plan_pixels;
    if (row_plan_pixels > perf_capture_compare_dirty_row_mask_max_plan_pixels_by_texture[texture_index]) {
        perf_capture_compare_dirty_row_mask_max_plan_pixels_by_texture[texture_index] = row_plan_pixels;
    }

    if (row_plan_pixels <= current_max_partial_pixels) {
        perf_capture_compare_dirty_row_mask_partial_candidate_refresh_attempts_by_texture[texture_index] += 1;
    }

    const Uint64 half_cap_pixels = ((Uint64)source_surface->w * (Uint64)source_surface->h) / 2u;
    if (row_plan_pixels <= half_cap_pixels) {
        perf_capture_compare_dirty_row_mask_half_cap_partial_candidate_refresh_attempts_by_texture[texture_index] += 1;
    }
}

static TextureUnlockRefreshDecision classify_compare_dirty_rect_refresh_decision(int texture_index,
                                                                                 const SDL_Surface* source_surface,
                                                                                 TextureUnlockRefreshPlan* out_plan) {
    if ((source_surface == NULL) || (out_plan == NULL) || (texture_index < 0) || (texture_index >= FL_TEXTURE_MAX)) {
        return TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NO_USABLE_DIRTY_RECT;
    }

    SDL_zero(*out_plan);
    const Uint64 max_partial_pixels = compare_dirty_rect_partial_refresh_max_pixels(source_surface);
    if (perf_capture_compare_dirty_row_mask_pending_tile_masks[texture_index] != 0) {
        TextureUnlockRefreshPlan row_mask_plan = { 0 };
        if (build_texture_unlock_refresh_plan_from_tile_mask(perf_capture_compare_dirty_row_mask_pending_tile_masks[texture_index],
                                                             &row_mask_plan)) {
            const Uint64 row_mask_pixels = texture_unlock_refresh_plan_pixels(&row_mask_plan);
            if ((row_mask_pixels > 0) &&
                ((row_mask_pixels <= max_partial_pixels) ||
                 compare_dirty_row_mask_plan_fits_relaxed_partial_exception(
                     &row_mask_plan, source_surface, max_partial_pixels))) {
                *out_plan = row_mask_plan;
                return TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL;
            }
        }
    }

    if (perf_capture_compare_dirty_rect_pending_tile_masks[texture_index] != 0) {
        TextureUnlockRefreshPlan multi_rect_plan = { 0 };
        if (build_texture_unlock_refresh_plan_from_tile_mask(perf_capture_compare_dirty_rect_pending_tile_masks[texture_index],
                                                             &multi_rect_plan)) {
            const Uint64 multi_rect_pixels = texture_unlock_refresh_plan_pixels(&multi_rect_plan);
            if ((multi_rect_pixels > 0) && (multi_rect_pixels <= max_partial_pixels)) {
                *out_plan = multi_rect_plan;
                return TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL;
            }
        }
    }

    if (!perf_capture_compare_dirty_rect_pending_valid[texture_index]) {
        return TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NO_USABLE_DIRTY_RECT;
    }

    const SDL_Rect dirty_rect = perf_capture_compare_dirty_rect_pending_rects[texture_index];
    if ((dirty_rect.w <= 0) || (dirty_rect.h <= 0) || (dirty_rect.x < 0) || (dirty_rect.y < 0) ||
        (dirty_rect.x + dirty_rect.w > source_surface->w) || (dirty_rect.y + dirty_rect.h > source_surface->h)) {
        return TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NO_USABLE_DIRTY_RECT;
    }

    const Uint64 dirty_pixels = (Uint64)dirty_rect.w * (Uint64)dirty_rect.h;
    if ((dirty_pixels == 0) || (dirty_pixels > max_partial_pixels)) {
        return TEXTURE_UNLOCK_REFRESH_DECISION_FULL_OVERSIZED_DIRTY_RECT;
    }

    out_plan->rects[0] = dirty_rect;
    out_plan->rect_count = 1;
    return TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL;
}

static TextureUnlockRefreshDecision classify_texture_unlock_refresh_decision(unsigned int th,
                                                                             Uint8 dirty_reason,
                                                                             const SDL_Surface* source_surface,
                                                                             TextureUnlockRefreshPlan* out_plan) {
    if ((source_surface == NULL) || (out_plan == NULL)) {
        return TEXTURE_UNLOCK_REFRESH_DECISION_FULL_INELIGIBLE_SOURCE;
    }
    SDL_zero(*out_plan);

    if (dirty_reason != CACHE_DIRTY_REASON_TEXTURE_UNLOCK) {
        return TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NON_TEXTURE_DIRTY;
    }

    const int texture_handle = LO_16_BITS(th);
    if ((texture_handle <= 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return TEXTURE_UNLOCK_REFRESH_DECISION_FULL_INELIGIBLE_SOURCE;
    }

    if ((source_surface->format != SDL_PIXELFORMAT_INDEX8) || (source_surface->w != 256) || (source_surface->h != 256)) {
        return TEXTURE_UNLOCK_REFRESH_DECISION_FULL_INELIGIBLE_SOURCE;
    }

    const int texture_index = texture_handle - 1;
    const Uint64 max_partial_pixels = texture_unlock_partial_refresh_max_pixels(source_surface);
    if (texture_unlock_dirty_tile_mask_valid[texture_index]) {
        TextureUnlockRefreshPlan multi_rect_plan = { 0 };
        if (build_texture_unlock_refresh_plan_from_tile_mask(texture_unlock_dirty_tile_masks[texture_index],
                                                             &multi_rect_plan)) {
            const Uint64 multi_rect_pixels = texture_unlock_refresh_plan_pixels(&multi_rect_plan);
            if ((multi_rect_pixels > 0) && (multi_rect_pixels <= max_partial_pixels)) {
                *out_plan = multi_rect_plan;
                return TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL;
            }
        }
    }

    if (texture_unlock_dirty_rect_valid[texture_index]) {
        const SDL_Rect dirty_rect = texture_unlock_dirty_rects[texture_index];
        if ((dirty_rect.w <= 0) || (dirty_rect.h <= 0) || (dirty_rect.x < 0) || (dirty_rect.y < 0) ||
            (dirty_rect.x + dirty_rect.w > source_surface->w) || (dirty_rect.y + dirty_rect.h > source_surface->h)) {
            return TEXTURE_UNLOCK_REFRESH_DECISION_FULL_NO_USABLE_DIRTY_RECT;
        }

        const Uint64 dirty_pixels = (Uint64)dirty_rect.w * (Uint64)dirty_rect.h;
        if ((dirty_pixels == 0) || (dirty_pixels > max_partial_pixels)) {
            return TEXTURE_UNLOCK_REFRESH_DECISION_FULL_OVERSIZED_DIRTY_RECT;
        }

        out_plan->rects[0] = dirty_rect;
        out_plan->rect_count = 1;
        return TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL;
    }

    return classify_compare_dirty_rect_refresh_decision(texture_index, source_surface, out_plan);
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

    TextureUnlockRefreshPlan partial_plan = { 0 };
    note_perf_capture_compare_dirty_rect_refresh_candidate(th, dirty_reason, surface);
    const TextureUnlockRefreshDecision refresh_decision =
        classify_texture_unlock_refresh_decision(th, dirty_reason, surface, &partial_plan);
    const bool use_partial_refresh = refresh_decision == TEXTURE_UNLOCK_REFRESH_DECISION_PARTIAL;
    note_perf_capture_refresh_path(th, surface, refresh_decision, use_partial_refresh ? &partial_plan : NULL);
    const bool sample_blit = should_sample_perf_capture_refresh_blit();
    Uint64 sampled_blit_start_counter = 0;

    if (frame_stats_extended_enabled) {
        const Uint64 blit_start_ns = SDL_GetTicksNS();
        sampled_blit_start_counter = sample_blit ? SDL_GetPerformanceCounter() : 0;
        frame_stats.software_surface_cache_refresh_blit_calls += 1;
        if (use_partial_refresh) {
            success = true;
            for (int rect_index = 0; rect_index < partial_plan.rect_count; rect_index++) {
                const SDL_Rect* rect = &partial_plan.rects[rect_index];
                if (!SDL_BlitSurface(surface, rect, cached_surface, rect)) {
                    success = SDL_BlitSurface(surface, NULL, cached_surface, NULL);
                    break;
                }
            }
        } else {
            success = SDL_BlitSurface(surface, NULL, cached_surface, NULL);
        }
        frame_stats.software_surface_cache_refresh_blit_ns += SDL_GetTicksNS() - blit_start_ns;
    } else {
        if (use_partial_refresh) {
            success = true;
            for (int rect_index = 0; rect_index < partial_plan.rect_count; rect_index++) {
                const SDL_Rect* rect = &partial_plan.rects[rect_index];
                if (!SDL_BlitSurface(surface, rect, cached_surface, rect)) {
                    success = SDL_BlitSurface(surface, NULL, cached_surface, NULL);
                    break;
                }
            }
        } else {
            success = SDL_BlitSurface(surface, NULL, cached_surface, NULL);
        }
    }
    if (sample_blit) {
        note_perf_capture_refresh_blit_sample(
            th, refresh_decision, perf_capture_counter_delta_to_ns(sampled_blit_start_counter, SDL_GetPerformanceCounter()));
    }

done:
    if (success) {
        clear_software_surface_full_opaque_row_mask(texture_handle - 1, palette_handle);
    }
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

#if defined(PORT_MISTER)
static bool ensure_sa_bg_cache_surface(void) {
    if (sa_bg_cache_surface != NULL) {
        return true;
    }

    sa_bg_cache_surface = SDL_CreateSurface(cps3_width, cps3_height, SDL_PIXELFORMAT_ARGB8888);
    return sa_bg_cache_surface != NULL;
}
#endif

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
                        note_perf_capture_software_surface_access_provenance(texture_handle - 1, dirty_state);
                    }
                });
                set_software_surface_cache_slot_state(
                    texture_handle - 1, palette_handle, cached_surface, CACHE_DIRTY_REASON_NONE);
                clear_cache_dirty_state(dirty_state);
                clear_texture_unlock_dirty_rect_if_unused(
                    texture_handle - 1, TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_ACCESS_UNUSED);
                return cached_surface;
            }

            push_software_surface_to_destroy(cached_surface);
            clear_software_surface_full_opaque_row_mask(texture_handle - 1, palette_handle);
            cached_surface = NULL;
            set_software_surface_cache_slot_state(texture_handle - 1, palette_handle, NULL, CACHE_DIRTY_REASON_NONE);
            clear_texture_unlock_dirty_rect_if_unused(
                texture_handle - 1, TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_ACCESS_UNUSED);
        }
    }

    if (cached_surface != NULL) {
        clear_cache_dirty_state(dirty_state);
        clear_texture_unlock_dirty_rect_if_unused(
            texture_handle - 1, TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_ACCESS_UNUSED);
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
            note_perf_capture_software_surface_access_provenance(texture_handle - 1, dirty_state);
        }
    });
    clear_software_surface_full_opaque_row_mask(texture_handle - 1, palette_handle);
    set_software_surface_cache_slot_state(
        texture_handle - 1, palette_handle, cached_surface, CACHE_DIRTY_REASON_NONE);
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

/* Nearest-neighbor scaled blit from sa_bg_cache_surface to
   software_frame_surface.  Both surfaces must already be locked.

   The game's BgMATRIX transform chain is:
     njScale(scr_sc) → njTranslate(0,224) → njScale(1,-1,1) → njTranslate(-h_shift,-v_shift)
   where h_shift includes +scrn_adgjust_x and v_shift includes -scrn_adgjust_y.

   For a world point W, screen position is:
     screen_x = (W_x - bg_h_shift) * scr_sc
     screen_y = (224 - W_y + bg_v_shift) * scr_sc

   Solving for the source pixel in the cached surface that shows the
   same world content as a destination pixel in the current frame:
     src = dst * (cached_sc / current_sc) + scroll_delta * cached_sc

   The scrn_adgjust delta inherently handles zoom centering — no
   additional center-screen correction is needed.

   Source coordinates are clamped to edge pixels.  Integer-only inner
   loop for ARM performance. */
static void sa_bg_cache_restore_background_scaled(float cached_sc, float current_sc,
                                                 int scroll_dx, int scroll_dy) {
    const int w = software_frame_surface->w;
    const int h = software_frame_surface->h;
    const int w_max = w - 1;
    const int h_max = h - 1;
    const float inv_rel = cached_sc / current_sc; /* map dst→src scale */

    /* Fixed-point 16.16: src = dst * inv_rel + scroll_delta * cached_sc */
    const int inv_rel_fp = (int)(inv_rel * 65536.0f);
    const int offset_x_fp = (int)((float)scroll_dx * cached_sc * 65536.0f);
    const int offset_y_fp = (int)((float)scroll_dy * cached_sc * 65536.0f);

    const Uint32* src_pixels = (const Uint32*)sa_bg_cache_surface->pixels;
    Uint32* dst_pixels = (Uint32*)software_frame_surface->pixels;
    const int src_pitch4 = sa_bg_cache_surface->pitch / 4;
    const int dst_pitch4 = software_frame_surface->pitch / 4;

    for (int y = 0; y < h; y++) {
        int src_y = (y * inv_rel_fp + offset_y_fp) >> 16;
        if (src_y < 0) src_y = 0;
        else if (src_y > h_max) src_y = h_max;
        Uint32* dst_row = dst_pixels + y * dst_pitch4;
        const Uint32* src_row = src_pixels + src_y * src_pitch4;
        int sx_fp = offset_x_fp;
        for (int x = 0; x < w; x++) {
            int src_x = sx_fp >> 16;
            if (src_x < 0) src_x = 0;
            else if (src_x > w_max) src_x = w_max;
            dst_row[x] = src_row[src_x];
            sx_fp += inv_rel_fp;
        }
    }
}

static void apply_super_effect_burst_reduction_after_sort(void) {
#if defined(PORT_MISTER)
    /* Background caching with zoom support: during any super art's
       cinematic freeze (sa_stop_check), render the background ONCE on
       the first activation frame, then reuse the cached background for
       all subsequent frames.  If the camera zoom (scr_sc) changes
       between frames, apply a nearest-neighbor scaled blit to the cached
       surface instead of a plain memcpy.  Characters, effects, and HUD
       render fresh every frame at 60fps.

       The scaled blit costs ~0.5ms (86K pixels, integer math) vs ~10ms
       for re-rendering 100+ background tiles.  Quality is slightly soft
       during the brief zoom animation but acceptable given the flashy
       effects on screen. */
    sa_bg_cache_snapshot_at_index = -1;
    if ((super_effect_quality_mode != SDL_GAME_RENDERER_SUPER_EFFECT_QUALITY_CACHED_BG) ||
        (sa_bg_cache_frames_remaining <= 0) || (render_task_count <= 1)) {
        return;
    }

    /* Identify contiguous background tasks at the bottom of the Z-sorted
       array.  Background tiles use palette indices >= 256 (bgPalCodeOffset
       = 0x12C = 300).  Scan from index 0 upward; stop at the first
       textured non-background task (character/effect).  Non-textured tasks
       (solid geometry / shadows) are skipped over but NOT counted as
       background — they're cheap and will re-render each frame. */
    int bg_end = 0;
    for (int i = 0; i < render_task_count; i++) {
        const int pal = HI_16_BITS(render_tasks[i].texture_binding);
        if (pal >= 256) {
            bg_end = i + 1;
        } else if (render_tasks[i].texture != NULL) {
            break; /* First character/effect textured task — stop here. */
        }
        /* Solid geometry (shadows, pal==0, no texture): skip over. */
    }

    /* If no background tasks found at the bottom, nothing to cache. */
    if (bg_end < 1) {
        return;
    }

    /* No cached background yet — render everything this frame and
       request a mid-render snapshot after background tasks complete. */
    if (!sa_bg_cache_surface_valid || (sa_bg_cache_surface == NULL)) {
        sa_bg_cache_snapshot_at_index = bg_end;
        sa_bg_cache_saved_scr_sc = scr_sc;
        sa_bg_cache_saved_adgjust_x = scrn_adgjust_x;
        sa_bg_cache_saved_adgjust_y = scrn_adgjust_y;
        return;
    }

    /* Restore the cached background surface as the base layer.
       If zoom or scroll has changed since caching, use a scaled blit
       to match the current zoom/scroll.  Otherwise use a fast memcpy. */
    if ((software_frame_surface != NULL) && software_frame_surface_ready) {
        const int scroll_dx = (int)scrn_adgjust_x - (int)sa_bg_cache_saved_adgjust_x;
        const int scroll_dy = (int)scrn_adgjust_y - (int)sa_bg_cache_saved_adgjust_y;
        const bool needs_transform = (sa_bg_cache_saved_scr_sc != scr_sc) ||
                                     (scroll_dx != 0) || (scroll_dy != 0);
        SDL_LockSurface(sa_bg_cache_surface);
        SDL_LockSurface(software_frame_surface);
        if (needs_transform) {
            sa_bg_cache_restore_background_scaled(sa_bg_cache_saved_scr_sc, scr_sc,
                                                scroll_dx, scroll_dy);
        } else {
            SDL_memcpy(software_frame_surface->pixels, sa_bg_cache_surface->pixels,
                       (size_t)software_frame_surface->pitch * software_frame_surface->h);
        }
        SDL_UnlockSurface(software_frame_surface);
        SDL_UnlockSurface(sa_bg_cache_surface);
    }

    /* Drop background tasks.  Characters/effects/HUD render fresh
       on top of cached background. */
    const int upper_count = render_task_count - bg_end;
    SDL_memmove(&render_tasks[0], &render_tasks[bg_end], (size_t)upper_count * sizeof(RenderTask));
    render_task_count = upper_count;
    return;

#endif
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

static bool try_resolve_solid_task_as_full_height_diagonal_strip(const RenderTask* task,
                                                                 SoftwareFrameSolidDiagonalStrip* out_strip) {
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

    float min_y = task->vertices[0].position.y;
    float max_y = min_y;
    for (int i = 1; i < 4; i++) {
        min_y = SDL_min(min_y, task->vertices[i].position.y);
        max_y = SDL_max(max_y, task->vertices[i].position.y);
    }

    const int top_y = (int)SDL_roundf(min_y);
    const int bottom_y = (int)SDL_roundf(max_y);
    if (!nearly_equal(min_y, (float)top_y) || !nearly_equal(max_y, (float)bottom_y) || (top_y != 0) ||
        (bottom_y != cps3_height)) {
        return false;
    }

    float top_xs[2];
    float bottom_xs[2];
    int top_count = 0;
    int bottom_count = 0;
    for (int i = 0; i < 4; i++) {
        const SDL_Vertex* vertex = &task->vertices[i];
        if (nearly_equal(vertex->position.y, min_y)) {
            if (top_count >= SDL_arraysize(top_xs)) {
                return false;
            }
            top_xs[top_count++] = vertex->position.x;
        } else if (nearly_equal(vertex->position.y, max_y)) {
            if (bottom_count >= SDL_arraysize(bottom_xs)) {
                return false;
            }
            bottom_xs[bottom_count++] = vertex->position.x;
        } else {
            return false;
        }
    }

    if ((top_count != SDL_arraysize(top_xs)) || (bottom_count != SDL_arraysize(bottom_xs))) {
        return false;
    }

    if (top_xs[0] > top_xs[1]) {
        const float swap = top_xs[0];
        top_xs[0] = top_xs[1];
        top_xs[1] = swap;
    }
    if (bottom_xs[0] > bottom_xs[1]) {
        const float swap = bottom_xs[0];
        bottom_xs[0] = bottom_xs[1];
        bottom_xs[1] = swap;
    }

    const int top_left_x = (int)SDL_roundf(top_xs[0]);
    const int top_right_x = (int)SDL_roundf(top_xs[1]);
    const int bottom_left_x = (int)SDL_roundf(bottom_xs[0]);
    const int bottom_right_x = (int)SDL_roundf(bottom_xs[1]);
    if (!nearly_equal(top_xs[0], (float)top_left_x) || !nearly_equal(top_xs[1], (float)top_right_x) ||
        !nearly_equal(bottom_xs[0], (float)bottom_left_x) || !nearly_equal(bottom_xs[1], (float)bottom_right_x)) {
        return false;
    }

    const int strip_height = bottom_y - top_y;
    const int top_width = top_right_x - top_left_x;
    const int bottom_width = bottom_right_x - bottom_left_x;
    if ((strip_height != cps3_height) || (top_width <= 0) ||
        (top_width > software_frame_full_height_diagonal_strip_max_width_pixels) || (bottom_width != top_width) ||
        ((bottom_left_x - top_left_x) != strip_height) || ((bottom_right_x - top_right_x) != strip_height)) {
        return false;
    }

    if (out_strip != NULL) {
        out_strip->top_y = top_y;
        out_strip->bottom_y = bottom_y;
        out_strip->top_left_x = top_left_x;
        out_strip->top_right_x = top_right_x;
        out_strip->color = (((Uint32)SDL_roundf(color0.a * 255.0f)) << 24) |
                           (((Uint32)SDL_roundf(color0.r * 255.0f)) << 16) |
                           (((Uint32)SDL_roundf(color0.g * 255.0f)) << 8) | ((Uint32)SDL_roundf(color0.b * 255.0f));
    }
    return true;
}

static bool try_resolve_geometry_task_as_software_frame_parallelogram(
    const RenderTask* task, SoftwareFrameTexturedParallelogram* out_parallelogram) {
    if ((task == NULL) || (task->type != RENDER_TASK_TYPE_GEOMETRY) || (task->texture == NULL) ||
        (task->software_source_surface == NULL)) {
        return false;
    }

    TexturedGeometryFallbackFamilyProfile profile;
    if (!analyze_textured_geometry_fallback_task(task, &profile)) {
        return false;
    }

    if ((profile.family_kind != SDL_GAME_RENDERER_TEXTURED_GEOMETRY_FALLBACK_FAMILY_RECT_UV_PARALLELOGRAM) ||
        !profile.uniform_color || !profile.opaque_color || profile.rgb_mod || !profile.integer_positions ||
        !profile.integer_source_rect || !profile.full_texture_source_rect || (profile.source_x != 0) ||
        (profile.source_y != 0) || (profile.source_w != 256) || (profile.source_h != 256) ||
        (profile.dst_height != 256) || (profile.dst_top_width != 256) || (profile.dst_bottom_width != 256) ||
        (profile.dst_left_dx != profile.dst_right_dx)) {
        return false;
    }

    const SDL_Surface* src_surface = task->software_source_surface;
    if ((src_surface->w != profile.source_w) || (src_surface->h != profile.source_h)) {
        return false;
    }

    float min_u = task->vertices[0].tex_coord.x;
    float max_u = min_u;
    float min_v = task->vertices[0].tex_coord.y;
    float max_v = min_v;
    float min_y = task->vertices[0].position.y;
    float max_y = min_y;
    const SDL_Vertex* top_vertices[2] = { NULL, NULL };
    const SDL_Vertex* bottom_vertices[2] = { NULL, NULL };
    int top_count = 0;
    int bottom_count = 0;

    for (int i = 0; i < 4; i++) {
        const SDL_Vertex* vertex = &task->vertices[i];
        min_u = SDL_min(min_u, vertex->tex_coord.x);
        max_u = SDL_max(max_u, vertex->tex_coord.x);
        min_v = SDL_min(min_v, vertex->tex_coord.y);
        max_v = SDL_max(max_v, vertex->tex_coord.y);
        min_y = SDL_min(min_y, vertex->position.y);
        max_y = SDL_max(max_y, vertex->position.y);
    }

    for (int i = 0; i < 4; i++) {
        const SDL_Vertex* vertex = &task->vertices[i];
        if (nearly_equal(vertex->position.y, min_y)) {
            if (top_count >= SDL_arraysize(top_vertices)) {
                return false;
            }
            top_vertices[top_count++] = vertex;
        } else if (nearly_equal(vertex->position.y, max_y)) {
            if (bottom_count >= SDL_arraysize(bottom_vertices)) {
                return false;
            }
            bottom_vertices[bottom_count++] = vertex;
        } else {
            return false;
        }
    }

    if ((top_count != SDL_arraysize(top_vertices)) || (bottom_count != SDL_arraysize(bottom_vertices))) {
        return false;
    }

    if (top_vertices[0]->position.x > top_vertices[1]->position.x) {
        const SDL_Vertex* swap = top_vertices[0];
        top_vertices[0] = top_vertices[1];
        top_vertices[1] = swap;
    }
    if (bottom_vertices[0]->position.x > bottom_vertices[1]->position.x) {
        const SDL_Vertex* swap = bottom_vertices[0];
        bottom_vertices[0] = bottom_vertices[1];
        bottom_vertices[1] = swap;
    }

    const int top_y = (int)SDL_roundf(min_y);
    const int bottom_y = (int)SDL_roundf(max_y);
    const int top_left_x = (int)SDL_roundf(top_vertices[0]->position.x);
    const int top_right_x = (int)SDL_roundf(top_vertices[1]->position.x);
    const int bottom_left_x = (int)SDL_roundf(bottom_vertices[0]->position.x);
    const int bottom_right_x = (int)SDL_roundf(bottom_vertices[1]->position.x);
    if (!nearly_equal(min_y, (float)top_y) || !nearly_equal(max_y, (float)bottom_y) ||
        !nearly_equal(top_vertices[0]->position.x, (float)top_left_x) ||
        !nearly_equal(top_vertices[1]->position.x, (float)top_right_x) ||
        !nearly_equal(bottom_vertices[0]->position.x, (float)bottom_left_x) ||
        !nearly_equal(bottom_vertices[1]->position.x, (float)bottom_right_x)) {
        return false;
    }

    if (!nearly_equal(top_vertices[0]->tex_coord.x, min_u) || !nearly_equal(top_vertices[0]->tex_coord.y, min_v) ||
        !nearly_equal(top_vertices[1]->tex_coord.x, max_u) || !nearly_equal(top_vertices[1]->tex_coord.y, min_v) ||
        !nearly_equal(bottom_vertices[0]->tex_coord.x, min_u) ||
        !nearly_equal(bottom_vertices[0]->tex_coord.y, max_v) ||
        !nearly_equal(bottom_vertices[1]->tex_coord.x, max_u) ||
        !nearly_equal(bottom_vertices[1]->tex_coord.y, max_v)) {
        return false;
    }

    if ((bottom_y - top_y) != profile.source_h || (top_right_x - top_left_x) != profile.source_w ||
        (bottom_right_x - bottom_left_x) != profile.source_w) {
        return false;
    }

    if (out_parallelogram != NULL) {
        out_parallelogram->top_y = top_y;
        out_parallelogram->bottom_y = bottom_y;
        out_parallelogram->top_left_x = top_left_x;
        out_parallelogram->bottom_left_x = bottom_left_x;
        out_parallelogram->src_x = profile.source_x;
        out_parallelogram->src_y = profile.source_y;
        out_parallelogram->src_w = profile.source_w;
        out_parallelogram->src_h = profile.source_h;
    }
    return true;
}

static bool try_resolve_geometry_task_as_software_frame_float_parallelogram(
    const RenderTask* task, SoftwareFrameTexturedFloatParallelogram* out_parallelogram) {
    if ((task == NULL) || (task->type != RENDER_TASK_TYPE_GEOMETRY) || (task->texture == NULL) ||
        (task->software_source_surface == NULL) ||
        (render_task_submitted_pixels(task) > software_frame_full_texture_affine_quad_max_submitted_pixels)) {
        return false;
    }

    TexturedGeometryFallbackFamilyProfile profile;
    if (!analyze_textured_geometry_fallback_task(task, &profile) || !profile.uniform_color || !profile.opaque_color ||
        profile.rgb_mod || !profile.integer_source_rect || !profile.full_texture_source_rect ||
        (profile.source_format != SDL_PIXELFORMAT_INDEX8) || (profile.source_x != 0) || (profile.source_y != 0) ||
        (profile.source_w != 256) || (profile.source_h != 256)) {
        return false;
    }

    const SDL_Surface* src_surface = task->software_source_surface;
    if ((src_surface->w != profile.source_w) || (src_surface->h != profile.source_h)) {
        return false;
    }

    float min_u = task->vertices[0].tex_coord.x;
    float max_u = min_u;
    float min_v = task->vertices[0].tex_coord.y;
    float max_v = min_v;
    float min_y = task->vertices[0].position.y;
    float max_y = min_y;
    const SDL_Vertex* top_vertices[2] = { NULL, NULL };
    const SDL_Vertex* bottom_vertices[2] = { NULL, NULL };
    int top_count = 0;
    int bottom_count = 0;

    for (int i = 0; i < 4; i++) {
        const SDL_Vertex* vertex = &task->vertices[i];
        min_u = SDL_min(min_u, vertex->tex_coord.x);
        max_u = SDL_max(max_u, vertex->tex_coord.x);
        min_v = SDL_min(min_v, vertex->tex_coord.y);
        max_v = SDL_max(max_v, vertex->tex_coord.y);
        min_y = SDL_min(min_y, vertex->position.y);
        max_y = SDL_max(max_y, vertex->position.y);
    }

    for (int i = 0; i < 4; i++) {
        const SDL_Vertex* vertex = &task->vertices[i];
        if (nearly_equal(vertex->position.y, min_y)) {
            if (top_count >= SDL_arraysize(top_vertices)) {
                return false;
            }
            top_vertices[top_count++] = vertex;
        } else if (nearly_equal(vertex->position.y, max_y)) {
            if (bottom_count >= SDL_arraysize(bottom_vertices)) {
                return false;
            }
            bottom_vertices[bottom_count++] = vertex;
        } else {
            return false;
        }
    }

    if ((top_count != SDL_arraysize(top_vertices)) || (bottom_count != SDL_arraysize(bottom_vertices))) {
        return false;
    }

    if (top_vertices[0]->position.x > top_vertices[1]->position.x) {
        const SDL_Vertex* swap = top_vertices[0];
        top_vertices[0] = top_vertices[1];
        top_vertices[1] = swap;
    }
    if (bottom_vertices[0]->position.x > bottom_vertices[1]->position.x) {
        const SDL_Vertex* swap = bottom_vertices[0];
        bottom_vertices[0] = bottom_vertices[1];
        bottom_vertices[1] = swap;
    }

    if (!nearly_equal(top_vertices[0]->tex_coord.x, min_u) || !nearly_equal(top_vertices[0]->tex_coord.y, min_v) ||
        !nearly_equal(top_vertices[1]->tex_coord.x, max_u) || !nearly_equal(top_vertices[1]->tex_coord.y, min_v) ||
        !nearly_equal(bottom_vertices[0]->tex_coord.x, min_u) ||
        !nearly_equal(bottom_vertices[0]->tex_coord.y, max_v) ||
        !nearly_equal(bottom_vertices[1]->tex_coord.x, max_u) ||
        !nearly_equal(bottom_vertices[1]->tex_coord.y, max_v)) {
        return false;
    }

    const float top_left_x = top_vertices[0]->position.x;
    const float top_right_x = top_vertices[1]->position.x;
    const float bottom_left_x = bottom_vertices[0]->position.x;
    const float bottom_right_x = bottom_vertices[1]->position.x;
    const float top_width = top_right_x - top_left_x;
    const float bottom_width = bottom_right_x - bottom_left_x;
    const float left_dx = bottom_left_x - top_left_x;
    const float right_dx = bottom_right_x - top_right_x;
    if ((max_y - min_y) <= rect_task_epsilon || (top_width <= rect_task_epsilon) || (bottom_width <= rect_task_epsilon) ||
        (SDL_fabsf(top_width - bottom_width) > 0.25f) || (SDL_fabsf(left_dx - right_dx) > 0.25f)) {
        return false;
    }

    if (out_parallelogram != NULL) {
        out_parallelogram->top_y = min_y;
        out_parallelogram->bottom_y = max_y;
        out_parallelogram->top_left_x = top_left_x;
        out_parallelogram->top_right_x = top_right_x;
        out_parallelogram->bottom_left_x = bottom_left_x;
        out_parallelogram->bottom_right_x = bottom_right_x;
        out_parallelogram->src_x = profile.source_x;
        out_parallelogram->src_y = profile.source_y;
        out_parallelogram->src_w = profile.source_w;
        out_parallelogram->src_h = profile.source_h;
    }
    return true;
}

static bool try_resolve_geometry_task_as_software_frame_full_texture_affine_quad(
    const RenderTask* task, SoftwareFrameTexturedFloatAffineQuad* out_quad) {
    if ((task == NULL) || (task->type != RENDER_TASK_TYPE_GEOMETRY) || (task->texture == NULL) ||
        (task->software_source_surface == NULL) ||
        (render_task_submitted_pixels(task) > software_frame_full_texture_affine_quad_max_submitted_pixels)) {
        return false;
    }

    TexturedGeometryFallbackFamilyProfile profile;
    if (!analyze_textured_geometry_fallback_task(task, &profile) || !profile.uniform_color || !profile.opaque_color ||
        profile.rgb_mod || !profile.integer_source_rect || !profile.full_texture_source_rect ||
        (profile.source_format != SDL_PIXELFORMAT_INDEX8) || (profile.source_x != 0) || (profile.source_y != 0) ||
        (profile.source_w != 256) || (profile.source_h != 256)) {
        return false;
    }

    const SDL_Surface* src_surface = task->software_source_surface;
    if ((src_surface->w != profile.source_w) || (src_surface->h != profile.source_h)) {
        return false;
    }

    // The Ibuki stage SA activation regression is the same full-texture background family as the
    // earlier stage-7 shear fix, but with fractional vertex positions that miss the exact row-copy
    // path. Keep this admission narrow instead of opening generic geometry.
    float min_u = task->vertices[0].tex_coord.x;
    float max_u = min_u;
    float min_v = task->vertices[0].tex_coord.y;
    float max_v = min_v;
    for (int i = 1; i < 4; i++) {
        min_u = SDL_min(min_u, task->vertices[i].tex_coord.x);
        max_u = SDL_max(max_u, task->vertices[i].tex_coord.x);
        min_v = SDL_min(min_v, task->vertices[i].tex_coord.y);
        max_v = SDL_max(max_v, task->vertices[i].tex_coord.y);
    }
    if (!textured_geometry_task_has_rect_uv(task, min_u, max_u, min_v, max_v)) {
        return false;
    }

    if (out_quad != NULL) {
        for (int i = 0; i < 4; i++) {
            out_quad->dst_x[i] = task->vertices[i].position.x;
            out_quad->dst_y[i] = task->vertices[i].position.y;
            out_quad->src_u[i] = task->vertices[i].tex_coord.x * (float)src_surface->w;
            out_quad->src_v[i] = task->vertices[i].tex_coord.y * (float)src_surface->h;
        }
    }

    return true;
}

static bool try_resolve_geometry_task_as_software_frame_affine_quad(
    const RenderTask* task, SoftwareFrameTexturedAffineQuad* out_quad) {
    if ((task == NULL) || (task->type != RENDER_TASK_TYPE_GEOMETRY) || (task->texture == NULL) ||
        (task->software_source_surface == NULL) ||
        (render_task_submitted_pixels(task) > software_frame_affine_quad_max_submitted_pixels)) {
        return false;
    }

    TexturedGeometryFallbackFamilyProfile profile;
    if (!analyze_textured_geometry_fallback_task(task, &profile) || !profile.uniform_color || !profile.opaque_color ||
        profile.rgb_mod || !profile.integer_positions) {
        return false;
    }

    const SDL_Surface* src_surface = task->software_source_surface;
    for (int i = 0; i < 4; i++) {
        const SDL_Vertex* vertex = &task->vertices[i];
        const int dst_x = (int)SDL_roundf(vertex->position.x);
        const int dst_y = (int)SDL_roundf(vertex->position.y);
        const float src_u = vertex->tex_coord.x * (float)src_surface->w;
        const float src_v = vertex->tex_coord.y * (float)src_surface->h;
        const int src_u_int = (int)SDL_roundf(src_u);
        const int src_v_int = (int)SDL_roundf(src_v);
        if (!nearly_equal(vertex->position.x, (float)dst_x) || !nearly_equal(vertex->position.y, (float)dst_y) ||
            !nearly_equal(src_u, (float)src_u_int) || !nearly_equal(src_v, (float)src_v_int) || (src_u < 0.0f) ||
            (src_v < 0.0f) || (src_u > (float)src_surface->w) || (src_v > (float)src_surface->h)) {
            return false;
        }
        if (out_quad != NULL) {
            out_quad->dst_x[i] = dst_x;
            out_quad->dst_y[i] = dst_y;
            out_quad->src_u[i] = src_u;
            out_quad->src_v[i] = src_v;
        }
    }

    return true;
}

static bool try_resolve_geometry_task_as_software_frame_translated_quad(
    const RenderTask* task, SoftwareFrameTexturedTranslatedQuad* out_quad) {
    SoftwareFrameTexturedAffineQuad affine_quad;
    if (!try_resolve_geometry_task_as_software_frame_affine_quad(task, &affine_quad)) {
        return false;
    }

    const int src_dx = (int)SDL_roundf(affine_quad.src_u[0]) - affine_quad.dst_x[0];
    const int src_dy = (int)SDL_roundf(affine_quad.src_v[0]) - affine_quad.dst_y[0];
    for (int i = 1; i < 4; i++) {
        if (((int)SDL_roundf(affine_quad.src_u[i]) - affine_quad.dst_x[i]) != src_dx ||
            ((int)SDL_roundf(affine_quad.src_v[i]) - affine_quad.dst_y[i]) != src_dy) {
            return false;
        }
    }

    if (out_quad != NULL) {
        SDL_memcpy(out_quad->dst_x, affine_quad.dst_x, sizeof(out_quad->dst_x));
        SDL_memcpy(out_quad->dst_y, affine_quad.dst_y, sizeof(out_quad->dst_y));
        out_quad->src_dx = src_dx;
        out_quad->src_dy = src_dy;
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

    if ((task->type == RENDER_TASK_TYPE_GEOMETRY) && (task->texture != NULL)) {
        return (try_resolve_geometry_task_as_software_frame_parallelogram(task, NULL) ||
                try_resolve_geometry_task_as_software_frame_float_parallelogram(task, NULL) ||
                try_resolve_geometry_task_as_software_frame_full_texture_affine_quad(task, NULL) ||
                try_resolve_geometry_task_as_software_frame_translated_quad(task, NULL) ||
                try_resolve_geometry_task_as_software_frame_affine_quad(task, NULL))
                   ? SOFTWARE_FRAME_FALLBACK_REASON_NONE
                   : SOFTWARE_FRAME_FALLBACK_REASON_GEOMETRY;
    }

    if (task->texture == NULL) {
        SDL_FRect solid_rect;
        Uint32 solid_color = 0;
        if (try_resolve_solid_task_as_rect(task, &solid_rect, &solid_color) ||
            try_resolve_solid_task_as_full_height_diagonal_strip(task, NULL)) {
            return SOFTWARE_FRAME_FALLBACK_REASON_NONE;
        }
        return SOFTWARE_FRAME_FALLBACK_REASON_SOLID;
    }

    return SOFTWARE_FRAME_FALLBACK_REASON_GEOMETRY;
}

#if ENABLE_PERF_TELEMETRY
static bool software_frame_color_is_alpha_only(Uint32 color) {
    return (((color >> 24) & 0xFFu) != 0xFFu) && ((color & 0x00FFFFFFu) == 0x00FFFFFFu);
}

static bool software_frame_color_has_rgb_mod(Uint32 color) {
    return (color & 0x00FFFFFFu) != 0x00FFFFFFu;
}

static bool analyze_textured_rect_family_task(const RenderTask* task,
                                              const SDL_Surface* dst_surface,
                                              TexturedRectFamilyProfile* out_profile) {
    if ((task == NULL) || (dst_surface == NULL) || (out_profile == NULL) ||
        (task->type != RENDER_TASK_TYPE_TEXTURED_RECT) || (task->texture == NULL)) {
        return false;
    }

    SDL_zero(*out_profile);
    out_profile->texture_handle = LO_16_BITS(task->texture_binding);
    out_profile->palette_handle = HI_16_BITS(task->texture_binding);
    out_profile->alpha_only = software_frame_color_is_alpha_only(task->color);
    out_profile->rgb_mod = software_frame_color_has_rgb_mod(task->color);
    out_profile->opaque_color = ((task->color >> 24) & 0xFFu) == 0xFFu;
    out_profile->flip_h = (task->flip & SDL_FLIP_HORIZONTAL) != 0;
    out_profile->flip_v = (task->flip & SDL_FLIP_VERTICAL) != 0;
    out_profile->submitted_pixels = render_task_submitted_pixels(task);

    const int texture_index = out_profile->texture_handle - 1;
    const SDL_Surface* src_surface = task->software_source_surface;
    if ((src_surface == NULL) && (texture_index >= 0) && (texture_index < FL_TEXTURE_MAX)) {
        src_surface = surfaces[texture_index];
    }
    if (src_surface != NULL) {
        out_profile->source_format = src_surface->format;
        out_profile->source_width = src_surface->w;
        out_profile->source_height = src_surface->h;
    } else {
        out_profile->source_format = SDL_PIXELFORMAT_UNKNOWN;
        out_profile->source_width = 0;
        out_profile->source_height = 0;
    }

    const int dst_x = (int)SDL_roundf(task->dst_rect.x);
    const int dst_y = (int)SDL_roundf(task->dst_rect.y);
    const int dst_w = (int)SDL_roundf(task->dst_rect.w);
    const int dst_h = (int)SDL_roundf(task->dst_rect.h);
    out_profile->raw_dst_x = task->dst_rect.x;
    out_profile->raw_dst_y = task->dst_rect.y;
    out_profile->raw_dst_w = task->dst_rect.w;
    out_profile->raw_dst_h = task->dst_rect.h;
    out_profile->integer_positions =
        nearly_equal(task->dst_rect.x, (float)dst_x) && nearly_equal(task->dst_rect.y, (float)dst_y) &&
        nearly_equal(task->dst_rect.w, (float)dst_w) && nearly_equal(task->dst_rect.h, (float)dst_h) && (dst_w > 0) &&
        (dst_h > 0);
    if (out_profile->integer_positions) {
        out_profile->dst_x = dst_x;
        out_profile->dst_y = dst_y;
        out_profile->dst_w = dst_w;
        out_profile->dst_h = dst_h;
    } else {
        out_profile->dst_x = -1;
        out_profile->dst_y = -1;
        out_profile->dst_w = -1;
        out_profile->dst_h = -1;
    }

    if ((out_profile->source_width > 0) && (out_profile->source_height > 0)) {
        const float src_x_f = task->src_uv_rect.x * (float)out_profile->source_width;
        const float src_y_f = task->src_uv_rect.y * (float)out_profile->source_height;
        const float src_w_f = task->src_uv_rect.w * (float)out_profile->source_width;
        const float src_h_f = task->src_uv_rect.h * (float)out_profile->source_height;
        const int src_x = (int)SDL_roundf(src_x_f);
        const int src_y = (int)SDL_roundf(src_y_f);
        const int src_w = (int)SDL_roundf(src_w_f);
        const int src_h = (int)SDL_roundf(src_h_f);
        out_profile->integer_source_rect =
            nearly_equal(src_x_f, (float)src_x) && nearly_equal(src_y_f, (float)src_y) &&
            nearly_equal(src_w_f, (float)src_w) && nearly_equal(src_h_f, (float)src_h) && (src_w > 0) && (src_h > 0);
        if (out_profile->integer_source_rect) {
            out_profile->source_x = src_x;
            out_profile->source_y = src_y;
            out_profile->source_w = src_w;
            out_profile->source_h = src_h;
            out_profile->full_texture_source_rect =
                (src_x == 0) && (src_y == 0) && (src_w == out_profile->source_width) &&
                (src_h == out_profile->source_height);
        } else {
            out_profile->source_x = -1;
            out_profile->source_y = -1;
            out_profile->source_w = -1;
            out_profile->source_h = -1;
        }
    } else {
        out_profile->source_x = -1;
        out_profile->source_y = -1;
        out_profile->source_w = -1;
        out_profile->source_h = -1;
    }

    const int unclamped_x0 = (int)SDL_floorf(task->dst_rect.x);
    const int unclamped_y0 = (int)SDL_floorf(task->dst_rect.y);
    const int unclamped_x1 = (int)SDL_ceilf(task->dst_rect.x + task->dst_rect.w);
    const int unclamped_y1 = (int)SDL_ceilf(task->dst_rect.y + task->dst_rect.h);
    const int dst_x0 = clamp_to_range(unclamped_x0, 0, dst_surface->w);
    const int dst_y0 = clamp_to_range(unclamped_y0, 0, dst_surface->h);
    const int dst_x1 = clamp_to_range(unclamped_x1, 0, dst_surface->w);
    const int dst_y1 = clamp_to_range(unclamped_y1, 0, dst_surface->h);
    out_profile->visible_w = dst_x1 - dst_x0;
    out_profile->visible_h = dst_y1 - dst_y0;
    out_profile->clipped = (dst_x0 != unclamped_x0) || (dst_y0 != unclamped_y0) || (dst_x1 != unclamped_x1) ||
                           (dst_y1 != unclamped_y1);
    return true;
}

static bool textured_rect_family_matches(const SDLGameRenderer_PerfCaptureTexturedRectFamily* entry,
                                         const TexturedRectFamilyProfile* profile) {
    if ((entry == NULL) || (profile == NULL)) {
        return false;
    }

    return (entry->texture_handle == profile->texture_handle) && (entry->palette_handle == profile->palette_handle) &&
           (entry->source_format == profile->source_format) && (entry->source_width == profile->source_width) &&
           (entry->source_height == profile->source_height) && (entry->alpha_only == (profile->alpha_only ? 1 : 0)) &&
           (entry->rgb_mod == (profile->rgb_mod ? 1 : 0)) &&
           (entry->opaque_color == (profile->opaque_color ? 1 : 0)) &&
           (entry->integer_positions == (profile->integer_positions ? 1 : 0)) &&
           (entry->integer_source_rect == (profile->integer_source_rect ? 1 : 0)) &&
           (entry->full_texture_source_rect == (profile->full_texture_source_rect ? 1 : 0)) &&
           (entry->clipped == (profile->clipped ? 1 : 0)) && (entry->flip_h == (profile->flip_h ? 1 : 0)) &&
           (entry->flip_v == (profile->flip_v ? 1 : 0));
}

static void note_perf_capture_textured_rect_family(const RenderTask* task,
                                                   const SDL_Surface* dst_surface,
                                                   Uint64 lookup_entries,
                                                   const SDLSoftwareFrame_NonIntegerTelemetry* non_integer_telemetry,
                                                   Uint64 sampled_ns,
                                                   SDLGameRenderer_PerfCaptureTexturedRectFamily* families,
                                                   int* family_count,
                                                   int family_capacity,
                                                   TexturedRectFamilyProfile* out_profile) {
    if ((task == NULL) || (dst_surface == NULL) || (families == NULL) || (family_count == NULL) ||
        (family_capacity <= 0) ||
        (!frame_stats_extended_enabled && !perf_capture_basic_first_window_family_snapshots_enabled)) {
        return;
    }

    TexturedRectFamilyProfile profile;
    if (!analyze_textured_rect_family_task(task, dst_surface, &profile)) {
        return;
    }
    if (out_profile != NULL) {
        *out_profile = profile;
    }
    if (!frame_stats_extended_enabled &&
        (perf_capture_basic_first_window_exact_hot_family_alpha_offpath_enabled ||
         perf_capture_basic_first_window_onset_exact_hot_family_alpha_offpath_enabled ||
         perf_capture_basic_first_window_onset_cluster_alpha_offpath_enabled)) {
        note_perf_capture_basic_first_window_alpha_offpath_shape(&profile);
    }

    SDLGameRenderer_PerfCaptureTexturedRectFamily* entry = NULL;
    for (int i = 0; i < *family_count; i++) {
        if (textured_rect_family_matches(&families[i], &profile)) {
            entry = &families[i];
            break;
        }
    }

    if ((entry == NULL) && (*family_count < family_capacity)) {
        entry = &families[*family_count];
        *family_count += 1;
        SDL_zero(*entry);
        entry->texture_handle = profile.texture_handle;
        entry->palette_handle = profile.palette_handle;
        entry->source_format = profile.source_format;
        entry->source_width = profile.source_width;
        entry->source_height = profile.source_height;
        entry->alpha_only = profile.alpha_only ? 1 : 0;
        entry->rgb_mod = profile.rgb_mod ? 1 : 0;
        entry->opaque_color = profile.opaque_color ? 1 : 0;
        entry->integer_positions = profile.integer_positions ? 1 : 0;
        entry->integer_source_rect = profile.integer_source_rect ? 1 : 0;
        entry->full_texture_source_rect = profile.full_texture_source_rect ? 1 : 0;
        entry->clipped = profile.clipped ? 1 : 0;
        entry->flip_h = profile.flip_h ? 1 : 0;
        entry->flip_v = profile.flip_v ? 1 : 0;
        entry->source_x_min = entry->source_x_max = profile.source_x;
        entry->source_y_min = entry->source_y_max = profile.source_y;
        entry->source_w_min = entry->source_w_max = profile.source_w;
        entry->source_h_min = entry->source_h_max = profile.source_h;
        entry->dst_x_min = entry->dst_x_max = profile.dst_x;
        entry->dst_y_min = entry->dst_y_max = profile.dst_y;
        entry->dst_w_min = entry->dst_w_max = profile.dst_w;
        entry->dst_h_min = entry->dst_h_max = profile.dst_h;
        entry->visible_w_min = entry->visible_w_max = profile.visible_w;
        entry->visible_h_min = entry->visible_h_max = profile.visible_h;
        copy_perf_capture_texture_logical_identity(profile.texture_handle - 1,
                                                   &entry->logical_identity_known,
                                                   &entry->logical_identity_mixed,
                                                   &entry->logical_identity_registrations,
                                                   &entry->logical_source_kind,
                                                   &entry->logical_ix_num,
                                                   &entry->logical_ix_num_first,
                                                   &entry->logical_slot_index,
                                                   &entry->logical_chunk_index,
                                                   &entry->logical_texture_total);
    }

    if (entry == NULL) {
        return;
    }

    entry->task_count += 1;
    entry->submitted_pixels += profile.submitted_pixels;
    entry->lookup_entries += lookup_entries;
    if (sampled_ns > 0) {
        entry->sampled_calls += 1;
        entry->sampled_pixels += profile.submitted_pixels;
        entry->sampled_ns += sampled_ns;
    }
    if (non_integer_telemetry != NULL) {
        entry->sampled_lookup_x_ns += non_integer_telemetry->sampled_lookup_x_ns;
        entry->sampled_lookup_y_ns += non_integer_telemetry->sampled_lookup_y_ns;
        entry->sampled_pair_lookup_ns += non_integer_telemetry->sampled_pair_lookup_ns;
        entry->sampled_reuse_telemetry_ns += non_integer_telemetry->sampled_reuse_telemetry_ns;
        entry->sampled_row_raster_ns += non_integer_telemetry->sampled_row_raster_ns;
        entry->source_alpha_opaque_pixels += non_integer_telemetry->source_alpha_opaque_pixels;
        entry->source_alpha_transparent_pixels += non_integer_telemetry->source_alpha_transparent_pixels;
        entry->source_alpha_blended_pixels += non_integer_telemetry->source_alpha_blended_pixels;
        entry->subrect_rows_total += non_integer_telemetry->subrect_rows_total;
        entry->subrect_rows_all_opaque += non_integer_telemetry->subrect_rows_all_opaque;
        entry->subrect_rows_all_transparent += non_integer_telemetry->subrect_rows_all_transparent;
        entry->subrect_rows_binary_alpha_only += non_integer_telemetry->subrect_rows_binary_alpha_only;
        entry->subrect_rows_binary_mixed += non_integer_telemetry->subrect_rows_binary_mixed;
        entry->subrect_rows_with_blended += non_integer_telemetry->subrect_rows_with_blended;
        entry->source_alpha_opaque_spans += non_integer_telemetry->source_alpha_opaque_spans;
        entry->source_alpha_transparent_spans += non_integer_telemetry->source_alpha_transparent_spans;
        entry->source_alpha_blended_spans += non_integer_telemetry->source_alpha_blended_spans;
        if (non_integer_telemetry->source_alpha_opaque_span_max > entry->source_alpha_opaque_span_max) {
            entry->source_alpha_opaque_span_max = non_integer_telemetry->source_alpha_opaque_span_max;
        }
        if (non_integer_telemetry->source_alpha_transparent_span_max > entry->source_alpha_transparent_span_max) {
            entry->source_alpha_transparent_span_max = non_integer_telemetry->source_alpha_transparent_span_max;
        }
        if (non_integer_telemetry->source_alpha_blended_span_max > entry->source_alpha_blended_span_max) {
            entry->source_alpha_blended_span_max = non_integer_telemetry->source_alpha_blended_span_max;
        }
        entry->same_source_runs += non_integer_telemetry->same_source_runs;
        entry->same_source_reuse_runs += non_integer_telemetry->same_source_reuse_runs;
        entry->same_source_reused_pixels += non_integer_telemetry->same_source_reused_pixels;
        entry->same_source_opaque_reused_pixels += non_integer_telemetry->same_source_opaque_reused_pixels;
        entry->same_source_transparent_reused_pixels += non_integer_telemetry->same_source_transparent_reused_pixels;
        entry->same_source_blended_reused_pixels += non_integer_telemetry->same_source_blended_reused_pixels;
        entry->same_source_pair_runs += non_integer_telemetry->same_source_pair_runs;
        entry->same_source_pair_leading_non_pair_pixels +=
            non_integer_telemetry->same_source_pair_leading_non_pair_pixels;
        entry->same_source_pair_trailing_non_pair_pixels +=
            non_integer_telemetry->same_source_pair_trailing_non_pair_pixels;
        entry->same_source_pair_gap_0_runs += non_integer_telemetry->same_source_pair_gap_0_runs;
        entry->same_source_pair_gap_1_runs += non_integer_telemetry->same_source_pair_gap_1_runs;
        entry->same_source_pair_gap_2_runs += non_integer_telemetry->same_source_pair_gap_2_runs;
        entry->same_source_pair_gap_3_plus_runs += non_integer_telemetry->same_source_pair_gap_3_plus_runs;
        if (non_integer_telemetry->same_source_max_run_length > entry->same_source_max_run_length) {
            entry->same_source_max_run_length = non_integer_telemetry->same_source_max_run_length;
        }
    }
    update_textured_geometry_fallback_range(&entry->source_x_min, &entry->source_x_max, profile.source_x);
    update_textured_geometry_fallback_range(&entry->source_y_min, &entry->source_y_max, profile.source_y);
    update_textured_geometry_fallback_range(&entry->source_w_min, &entry->source_w_max, profile.source_w);
    update_textured_geometry_fallback_range(&entry->source_h_min, &entry->source_h_max, profile.source_h);
    update_textured_geometry_fallback_range(&entry->dst_x_min, &entry->dst_x_max, profile.dst_x);
    update_textured_geometry_fallback_range(&entry->dst_y_min, &entry->dst_y_max, profile.dst_y);
    update_textured_geometry_fallback_range(&entry->dst_w_min, &entry->dst_w_max, profile.dst_w);
    update_textured_geometry_fallback_range(&entry->dst_h_min, &entry->dst_h_max, profile.dst_h);
    update_textured_geometry_fallback_range(&entry->visible_w_min, &entry->visible_w_max, profile.visible_w);
    update_textured_geometry_fallback_range(&entry->visible_h_min, &entry->visible_h_max, profile.visible_h);
}

static bool perf_capture_basic_first_window_exact_hot_family_matches_profile(const TexturedRectFamilyProfile* profile) {
    if (profile == NULL) {
        return false;
    }

    return (profile->texture_handle == 57 || profile->texture_handle == 58) &&
           (profile->palette_handle >= 391) && (profile->palette_handle <= 394) &&
           (profile->source_format == SDL_PIXELFORMAT_ARGB8888) && (profile->source_width == 256) &&
           (profile->source_height == 256) && !profile->alpha_only && !profile->rgb_mod && profile->opaque_color &&
           profile->integer_source_rect && !profile->full_texture_source_rect && !profile->clipped && !profile->flip_h &&
           !profile->flip_v && (profile->source_x >= 0) && (profile->source_y >= 0) && (profile->source_w > 0) &&
           (profile->source_h > 0) && (profile->visible_w > 0) && (profile->visible_h > 0);
}

static bool perf_capture_basic_first_window_onset_exact_hot_family_matches_profile(
    const TexturedRectFamilyProfile* profile) {
    if (profile == NULL) {
        return false;
    }

    return (((profile->texture_handle == 57) &&
             ((profile->palette_handle == 391) || (profile->palette_handle == 394) ||
              (profile->palette_handle == 393) || (profile->palette_handle == 329))) ||
            ((profile->texture_handle == 58) && (profile->palette_handle == 393)) ||
            ((profile->texture_handle == 18) && (profile->palette_handle == 37))) &&
           (profile->source_format == SDL_PIXELFORMAT_ARGB8888) && (profile->source_width == 256) &&
           (profile->source_height == 256) && !profile->alpha_only && !profile->rgb_mod && profile->opaque_color &&
           profile->integer_source_rect && !profile->full_texture_source_rect && !profile->clipped && !profile->flip_h &&
           !profile->flip_v && (profile->source_x >= 0) && (profile->source_y >= 0) && (profile->source_w > 0) &&
           (profile->source_h > 0) && (profile->visible_w > 0) && (profile->visible_h > 0);
}

static bool perf_capture_basic_first_window_onset_cluster_shape_matches_dimensions(int source_w,
                                                                                   int source_h,
                                                                                   int visible_w,
                                                                                   int visible_h) {
    return ((source_w == 32) && (source_h == 32) && (visible_w == visible_h) &&
            ((visible_w == 34) || (visible_w == 35) || (visible_w == 36) || (visible_w == 37))) ||
           ((source_w == 32) && (source_h == 16) &&
            (((visible_w == 35) && (visible_h == 18)) || ((visible_w == 37) && (visible_h == 19)))) ||
           ((source_w == 16) && (source_h == 32) && (visible_w == 18) && (visible_h == 35));
}

static bool perf_capture_basic_first_window_onset_cluster_alpha_offpath_matches_profile(
    const TexturedRectFamilyProfile* profile) {
    if (profile == NULL) {
        return false;
    }

    return perf_capture_basic_first_window_onset_exact_hot_family_matches_profile(profile) &&
           perf_capture_basic_first_window_onset_cluster_shape_matches_dimensions(
               profile->source_w,
               profile->source_h,
               profile->visible_w,
               profile->visible_h);
}

static void note_perf_capture_basic_first_window_alpha_offpath_shape(const TexturedRectFamilyProfile* profile) {
    if (profile == NULL) {
        return;
    }

    const bool matches_exact =
        perf_capture_basic_first_window_exact_hot_family_alpha_offpath_enabled &&
        perf_capture_basic_first_window_exact_hot_family_matches_profile(profile);
    const bool matches_onset =
        perf_capture_basic_first_window_onset_exact_hot_family_alpha_offpath_enabled &&
        perf_capture_basic_first_window_onset_exact_hot_family_matches_profile(profile);
    const bool matches_onset_cluster =
        perf_capture_basic_first_window_onset_cluster_alpha_offpath_enabled &&
        perf_capture_basic_first_window_onset_cluster_alpha_offpath_matches_profile(profile);
    if (!matches_exact && !matches_onset && !matches_onset_cluster) {
        return;
    }

    PerfCaptureBasicFirstWindowAlphaOffpathShape* entry = NULL;
    for (int i = 0; i < perf_capture_basic_first_window_alpha_offpath_shape_count; i++) {
        PerfCaptureBasicFirstWindowAlphaOffpathShape* candidate = &perf_capture_basic_first_window_alpha_offpath_shapes[i];
        if ((candidate->texture_handle == profile->texture_handle) &&
            (candidate->palette_handle == profile->palette_handle) && (candidate->source_x == profile->source_x) &&
            (candidate->source_y == profile->source_y) && (candidate->source_w == profile->source_w) &&
            (candidate->source_h == profile->source_h) && (candidate->visible_w == profile->visible_w) &&
            (candidate->visible_h == profile->visible_h) && nearly_equal(candidate->dst_x, profile->raw_dst_x) &&
            nearly_equal(candidate->dst_y, profile->raw_dst_y) && nearly_equal(candidate->dst_w, profile->raw_dst_w) &&
            nearly_equal(candidate->dst_h, profile->raw_dst_h)) {
            entry = candidate;
            break;
        }
    }

    if ((entry == NULL) &&
        (perf_capture_basic_first_window_alpha_offpath_shape_count <
         (int)SDL_arraysize(perf_capture_basic_first_window_alpha_offpath_shapes))) {
        entry = &perf_capture_basic_first_window_alpha_offpath_shapes[perf_capture_basic_first_window_alpha_offpath_shape_count];
        perf_capture_basic_first_window_alpha_offpath_shape_count += 1;
        SDL_zero(*entry);
        entry->texture_handle = profile->texture_handle;
        entry->palette_handle = profile->palette_handle;
        entry->source_x = profile->source_x;
        entry->source_y = profile->source_y;
        entry->source_w = profile->source_w;
        entry->source_h = profile->source_h;
        entry->dst_x = profile->raw_dst_x;
        entry->dst_y = profile->raw_dst_y;
        entry->dst_w = profile->raw_dst_w;
        entry->dst_h = profile->raw_dst_h;
        entry->visible_w = profile->visible_w;
        entry->visible_h = profile->visible_h;
    }

    if (entry == NULL) {
        return;
    }

    entry->task_count += 1u;
    entry->submitted_pixels += profile->submitted_pixels;
}

static bool perf_capture_sa_burst_effect_candidate_matches_profile(
    const TexturedRectFamilyProfile* profile) {
    if ((profile == NULL) || !frame_stats_extended_enabled || (sa_bg_cache_frames_remaining <= 0)) {
        return false;
    }

    if ((profile->texture_handle <= 0) || (profile->palette_handle < 0) || (profile->source_format != SDL_PIXELFORMAT_ARGB8888) ||
        (profile->source_width <= 0) || (profile->source_height <= 0) || !profile->integer_source_rect ||
        profile->full_texture_source_rect || profile->rgb_mod || (profile->source_w <= 0) || (profile->source_h <= 0) ||
        (profile->visible_w <= 0) || (profile->visible_h <= 0)) {
        return false;
    }

    const bool supported_source_surface =
        ((profile->source_width == 128) && (profile->source_height == 128)) ||
        ((profile->source_width == 256) && (profile->source_height == 256));
    if (!supported_source_surface) {
        return false;
    }

    const bool small_candidate =
        (profile->source_w <= 48) && (profile->source_h <= 48) && (profile->visible_w <= 64) && (profile->visible_h <= 64);
    const bool wide_or_tall_candidate =
        (profile->source_w >= 96) || (profile->source_h >= 64) || (profile->visible_w >= 96) || (profile->visible_h >= 64);
    return small_candidate || wide_or_tall_candidate;
}

static bool perf_capture_sa_burst_effect_sample_matches_profile(
    const PerfCaptureSABurstEffectSample* entry,
    const TexturedRectFamilyProfile* profile,
    int center_x,
    int center_y,
    int dst_w,
    int dst_h) {
    if ((entry == NULL) || (profile == NULL)) {
        return false;
    }

    return (entry->texture_handle == profile->texture_handle) && (entry->palette_handle == profile->palette_handle) &&
           (entry->source_format == profile->source_format) && (entry->source_width == profile->source_width) &&
           (entry->source_height == profile->source_height) && (entry->alpha_only == (profile->alpha_only ? 1 : 0)) &&
           (entry->rgb_mod == (profile->rgb_mod ? 1 : 0)) && (entry->opaque_color == (profile->opaque_color ? 1 : 0)) &&
           (entry->clipped == (profile->clipped ? 1 : 0)) && (entry->flip_h == (profile->flip_h ? 1 : 0)) &&
           (entry->flip_v == (profile->flip_v ? 1 : 0)) && (entry->source_x == profile->source_x) &&
           (entry->source_y == profile->source_y) && (entry->source_w == profile->source_w) &&
           (entry->source_h == profile->source_h) && (entry->visible_w == profile->visible_w) &&
           (entry->visible_h == profile->visible_h) && (entry->center_x == center_x) && (entry->center_y == center_y) &&
           (entry->dst_w == dst_w) && (entry->dst_h == dst_h);
}

static void note_perf_capture_sa_burst_effect_sample(const TexturedRectFamilyProfile* profile,
                                                                  Uint64 sampled_ns) {
    if (!perf_capture_sa_burst_effect_candidate_matches_profile(profile)) {
        return;
    }

    const int center_x = (int)SDL_roundf(profile->raw_dst_x + (profile->raw_dst_w * 0.5f));
    const int center_y = (int)SDL_roundf(profile->raw_dst_y + (profile->raw_dst_h * 0.5f));
    const int dst_w = (int)SDL_roundf(profile->raw_dst_w);
    const int dst_h = (int)SDL_roundf(profile->raw_dst_h);

    PerfCaptureSABurstEffectSample* entry = NULL;
    for (int i = 0; i < perf_capture_sa_burst_effect_sample_count; i++) {
        if (perf_capture_sa_burst_effect_sample_matches_profile(
                &perf_capture_sa_burst_effect_samples[i], profile, center_x, center_y, dst_w, dst_h)) {
            entry = &perf_capture_sa_burst_effect_samples[i];
            break;
        }
    }

    if ((entry == NULL) &&
        (perf_capture_sa_burst_effect_sample_count <
         (int)SDL_arraysize(perf_capture_sa_burst_effect_samples))) {
        entry = &perf_capture_sa_burst_effect_samples[perf_capture_sa_burst_effect_sample_count];
        perf_capture_sa_burst_effect_sample_count += 1;
        SDL_zero(*entry);
        entry->texture_handle = profile->texture_handle;
        entry->palette_handle = profile->palette_handle;
        entry->source_format = profile->source_format;
        entry->source_width = profile->source_width;
        entry->source_height = profile->source_height;
        entry->alpha_only = profile->alpha_only ? 1 : 0;
        entry->rgb_mod = profile->rgb_mod ? 1 : 0;
        entry->opaque_color = profile->opaque_color ? 1 : 0;
        entry->clipped = profile->clipped ? 1 : 0;
        entry->flip_h = profile->flip_h ? 1 : 0;
        entry->flip_v = profile->flip_v ? 1 : 0;
        entry->source_x = profile->source_x;
        entry->source_y = profile->source_y;
        entry->source_w = profile->source_w;
        entry->source_h = profile->source_h;
        entry->visible_w = profile->visible_w;
        entry->visible_h = profile->visible_h;
        entry->center_x = center_x;
        entry->center_y = center_y;
        entry->dst_w = dst_w;
        entry->dst_h = dst_h;
        entry->raw_dst_x_first = profile->raw_dst_x;
        entry->raw_dst_y_first = profile->raw_dst_y;
        entry->raw_dst_w_first = profile->raw_dst_w;
        entry->raw_dst_h_first = profile->raw_dst_h;
        copy_perf_capture_texture_logical_identity(profile->texture_handle - 1,
                                                   &entry->logical_identity_known,
                                                   &entry->logical_identity_mixed,
                                                   &entry->logical_identity_registrations,
                                                   &entry->logical_source_kind,
                                                   &entry->logical_ix_num,
                                                   &entry->logical_ix_num_first,
                                                   &entry->logical_slot_index,
                                                   &entry->logical_chunk_index,
                                                   &entry->logical_texture_total);
    }

    if (entry == NULL) {
        return;
    }

    entry->task_count += 1u;
    entry->submitted_pixels += profile->submitted_pixels;
    entry->sampled_ns += sampled_ns;
    entry->raw_dst_x_last = profile->raw_dst_x;
    entry->raw_dst_y_last = profile->raw_dst_y;
    entry->raw_dst_w_last = profile->raw_dst_w;
    entry->raw_dst_h_last = profile->raw_dst_h;
}

static bool textured_rect_exact_shape_matches_profile(const SDLGameRenderer_PerfCaptureTexturedRectExactShape* entry,
                                                      const TexturedRectFamilyProfile* profile) {
    if ((entry == NULL) || (profile == NULL)) {
        return false;
    }

    return (entry->texture_handle == profile->texture_handle) && (entry->palette_handle == profile->palette_handle) &&
           (entry->source_format == profile->source_format) && (entry->source_width == profile->source_width) &&
           (entry->source_height == profile->source_height) && (entry->alpha_only == (profile->alpha_only ? 1 : 0)) &&
           (entry->rgb_mod == (profile->rgb_mod ? 1 : 0)) &&
           (entry->opaque_color == (profile->opaque_color ? 1 : 0)) &&
           (entry->integer_positions == (profile->integer_positions ? 1 : 0)) &&
           (entry->integer_source_rect == (profile->integer_source_rect ? 1 : 0)) &&
           (entry->full_texture_source_rect == (profile->full_texture_source_rect ? 1 : 0)) &&
           (entry->clipped == (profile->clipped ? 1 : 0)) && (entry->flip_h == (profile->flip_h ? 1 : 0)) &&
           (entry->flip_v == (profile->flip_v ? 1 : 0)) && (entry->source_w == profile->source_w) &&
           (entry->source_h == profile->source_h) && (entry->visible_w == profile->visible_w) &&
           (entry->visible_h == profile->visible_h);
}

static bool fast_non_integer_shared_shape_matches_exact_shape(
    const SDLGameRenderer_PerfCaptureFastNonIntegerSharedShape* entry,
    const SDLGameRenderer_PerfCaptureTexturedRectExactShape* shape) {
    if ((entry == NULL) || (shape == NULL)) {
        return false;
    }

    return (entry->source_format == shape->source_format) && (entry->source_width == shape->source_width) &&
           (entry->source_height == shape->source_height) && (entry->alpha_only == shape->alpha_only) &&
           (entry->rgb_mod == shape->rgb_mod) && (entry->opaque_color == shape->opaque_color) &&
           (entry->integer_positions == shape->integer_positions) &&
           (entry->integer_source_rect == shape->integer_source_rect) &&
           (entry->full_texture_source_rect == shape->full_texture_source_rect) &&
           (entry->clipped == shape->clipped) && (entry->flip_h == shape->flip_h) &&
           (entry->flip_v == shape->flip_v) && (entry->source_w == shape->source_w) &&
           (entry->source_h == shape->source_h) && (entry->visible_w == shape->visible_w) &&
           (entry->visible_h == shape->visible_h);
}

static bool fast_non_integer_shared_shape_matches_profile(
    const SDLGameRenderer_PerfCaptureFastNonIntegerSharedShape* a,
    const SDLGameRenderer_PerfCaptureFastNonIntegerSharedShape* b) {
    if ((a == NULL) || (b == NULL)) {
        return false;
    }

    return (a->source_format == b->source_format) && (a->source_width == b->source_width) &&
           (a->source_height == b->source_height) && (a->alpha_only == b->alpha_only) &&
           (a->rgb_mod == b->rgb_mod) && (a->opaque_color == b->opaque_color) &&
           (a->integer_positions == b->integer_positions) &&
           (a->integer_source_rect == b->integer_source_rect) &&
           (a->full_texture_source_rect == b->full_texture_source_rect) && (a->clipped == b->clipped) &&
           (a->flip_h == b->flip_h) && (a->flip_v == b->flip_v) && (a->source_w == b->source_w) &&
           (a->source_h == b->source_h) && (a->visible_w == b->visible_w) && (a->visible_h == b->visible_h);
}

static bool perf_capture_fast_non_integer_lookup_pattern_matches_profile(
    const PerfCaptureFastNonIntegerLookupPatternExactProfile* entry,
    const TexturedRectFamilyProfile* profile,
    Uint64 x_lookup_signature,
    Uint64 y_lookup_signature) {
    if ((entry == NULL) || (profile == NULL)) {
        return false;
    }

    return (entry->texture_handle == profile->texture_handle) && (entry->palette_handle == profile->palette_handle) &&
           (entry->source_format == profile->source_format) && (entry->source_width == profile->source_width) &&
           (entry->source_height == profile->source_height) && (entry->alpha_only == (profile->alpha_only ? 1 : 0)) &&
           (entry->rgb_mod == (profile->rgb_mod ? 1 : 0)) &&
           (entry->opaque_color == (profile->opaque_color ? 1 : 0)) &&
           (entry->integer_positions == (profile->integer_positions ? 1 : 0)) &&
           (entry->integer_source_rect == (profile->integer_source_rect ? 1 : 0)) &&
           (entry->full_texture_source_rect == (profile->full_texture_source_rect ? 1 : 0)) &&
           (entry->clipped == (profile->clipped ? 1 : 0)) && (entry->flip_h == (profile->flip_h ? 1 : 0)) &&
           (entry->flip_v == (profile->flip_v ? 1 : 0)) && (entry->source_w == profile->source_w) &&
           (entry->source_h == profile->source_h) && (entry->visible_w == profile->visible_w) &&
           (entry->visible_h == profile->visible_h) && (entry->x_lookup_signature == x_lookup_signature) &&
           (entry->y_lookup_signature == y_lookup_signature);
}

static bool fast_non_integer_lookup_profile_matches_pattern(
    const SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile* entry,
    const PerfCaptureFastNonIntegerLookupPatternExactProfile* pattern) {
    if ((entry == NULL) || (pattern == NULL)) {
        return false;
    }

    return (entry->source_format == pattern->source_format) && (entry->source_width == pattern->source_width) &&
           (entry->source_height == pattern->source_height) && (entry->alpha_only == pattern->alpha_only) &&
           (entry->rgb_mod == pattern->rgb_mod) && (entry->opaque_color == pattern->opaque_color) &&
           (entry->integer_positions == pattern->integer_positions) &&
           (entry->integer_source_rect == pattern->integer_source_rect) &&
           (entry->full_texture_source_rect == pattern->full_texture_source_rect) &&
           (entry->clipped == pattern->clipped) && (entry->flip_h == pattern->flip_h) &&
           (entry->flip_v == pattern->flip_v) && (entry->source_w == pattern->source_w) &&
           (entry->source_h == pattern->source_h) && (entry->visible_w == pattern->visible_w) &&
           (entry->visible_h == pattern->visible_h) && (entry->x_lookup_signature == pattern->x_lookup_signature) &&
           (entry->y_lookup_signature == pattern->y_lookup_signature);
}

static bool fast_non_integer_lookup_profile_matches_profile(
    const SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile* a,
    const SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile* b) {
    if ((a == NULL) || (b == NULL)) {
        return false;
    }

    return (a->source_format == b->source_format) && (a->source_width == b->source_width) &&
           (a->source_height == b->source_height) && (a->alpha_only == b->alpha_only) &&
           (a->rgb_mod == b->rgb_mod) && (a->opaque_color == b->opaque_color) &&
           (a->integer_positions == b->integer_positions) &&
           (a->integer_source_rect == b->integer_source_rect) &&
           (a->full_texture_source_rect == b->full_texture_source_rect) && (a->clipped == b->clipped) &&
           (a->flip_h == b->flip_h) && (a->flip_v == b->flip_v) && (a->source_w == b->source_w) &&
           (a->source_h == b->source_h) && (a->visible_w == b->visible_w) && (a->visible_h == b->visible_h) &&
           (a->x_lookup_signature == b->x_lookup_signature) && (a->y_lookup_signature == b->y_lookup_signature);
}

static int get_perf_capture_fast_non_integer_shared_shapes_from_exact_shapes(
    const SDLGameRenderer_PerfCaptureTexturedRectExactShape* shapes,
    int shape_count,
    SDLGameRenderer_PerfCaptureFastNonIntegerSharedShape* out_shapes,
    int max_shapes,
    Uint64* out_task_total,
    Uint64* out_pixel_total,
    Uint64* out_sampled_ns_total,
    int* out_shape_count) {
    SDLGameRenderer_PerfCaptureFastNonIntegerSharedShape aggregated[SDL_arraysize(perf_capture_fast_non_integer_shapes)] = {
        0
    };
    int aggregated_count = 0;
    Uint64 task_total = 0;
    Uint64 pixel_total = 0;
    Uint64 sampled_ns_total = 0;

    for (int i = 0; i < shape_count; i++) {
        const SDLGameRenderer_PerfCaptureTexturedRectExactShape* shape = &shapes[i];
        if (shape->task_count == 0) {
            continue;
        }

        task_total += shape->task_count;
        pixel_total += shape->submitted_pixels;
        sampled_ns_total += shape->sampled_ns;

        SDLGameRenderer_PerfCaptureFastNonIntegerSharedShape* entry = NULL;
        for (int existing = 0; existing < aggregated_count; existing++) {
            if (fast_non_integer_shared_shape_matches_exact_shape(&aggregated[existing], shape)) {
                entry = &aggregated[existing];
                break;
            }
        }

        if ((entry == NULL) && (aggregated_count < (int)SDL_arraysize(aggregated))) {
            entry = &aggregated[aggregated_count];
            aggregated_count += 1;
            SDL_zero(*entry);
            entry->source_format = shape->source_format;
            entry->source_width = shape->source_width;
            entry->source_height = shape->source_height;
            entry->alpha_only = shape->alpha_only;
            entry->rgb_mod = shape->rgb_mod;
            entry->opaque_color = shape->opaque_color;
            entry->integer_positions = shape->integer_positions;
            entry->integer_source_rect = shape->integer_source_rect;
            entry->full_texture_source_rect = shape->full_texture_source_rect;
            entry->clipped = shape->clipped;
            entry->flip_h = shape->flip_h;
            entry->flip_v = shape->flip_v;
            entry->source_w = shape->source_w;
            entry->source_h = shape->source_h;
            entry->visible_w = shape->visible_w;
            entry->visible_h = shape->visible_h;
        }

        if (entry == NULL) {
            continue;
        }

        entry->contributing_family_count += 1;
        entry->task_count += shape->task_count;
        entry->submitted_pixels += shape->submitted_pixels;
        entry->sampled_ns += shape->sampled_ns;
    }

    if (out_task_total != NULL) {
        *out_task_total = task_total;
    }
    if (out_pixel_total != NULL) {
        *out_pixel_total = pixel_total;
    }
    if (out_sampled_ns_total != NULL) {
        *out_sampled_ns_total = sampled_ns_total;
    }
    if (out_shape_count != NULL) {
        *out_shape_count = aggregated_count;
    }

    if ((out_shapes == NULL) || (max_shapes <= 0)) {
        return 0;
    }

    int selected_count = 0;
    for (int slot = 0; slot < max_shapes; slot++) {
        int best_index = -1;
        for (int i = 0; i < aggregated_count; i++) {
            const SDLGameRenderer_PerfCaptureFastNonIntegerSharedShape* candidate = &aggregated[i];
            if (candidate->task_count == 0) {
                continue;
            }

            bool already_selected = false;
            for (int existing = 0; existing < selected_count; existing++) {
                if (fast_non_integer_shared_shape_matches_profile(&out_shapes[existing], candidate)) {
                    already_selected = true;
                    break;
                }
            }
            if (already_selected) {
                continue;
            }

            if (best_index < 0) {
                best_index = i;
                continue;
            }

            const SDLGameRenderer_PerfCaptureFastNonIntegerSharedShape* best = &aggregated[best_index];
            if ((candidate->sampled_ns > best->sampled_ns) ||
                ((candidate->sampled_ns == best->sampled_ns) &&
                 (candidate->submitted_pixels > best->submitted_pixels)) ||
                ((candidate->sampled_ns == best->sampled_ns) &&
                 (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->task_count > best->task_count)) ||
                ((candidate->sampled_ns == best->sampled_ns) &&
                 (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->task_count == best->task_count) &&
                 (candidate->contributing_family_count > best->contributing_family_count)) ||
                ((candidate->sampled_ns == best->sampled_ns) &&
                 (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->task_count == best->task_count) &&
                 (candidate->contributing_family_count == best->contributing_family_count) &&
                 (candidate->source_w > best->source_w)) ||
                ((candidate->sampled_ns == best->sampled_ns) &&
                 (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->task_count == best->task_count) &&
                 (candidate->contributing_family_count == best->contributing_family_count) &&
                 (candidate->source_w == best->source_w) && (candidate->source_h > best->source_h))) {
                best_index = i;
            }
        }

        if (best_index < 0) {
            break;
        }

        out_shapes[selected_count] = aggregated[best_index];
        selected_count += 1;
    }

    return selected_count;
}

static int get_perf_capture_fast_non_integer_lookup_profiles_from_patterns(
    const PerfCaptureFastNonIntegerLookupPatternExactProfile* patterns,
    int pattern_count,
    SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile* out_profiles,
    int max_profiles,
    Uint64* out_task_total,
    Uint64* out_pixel_total,
    Uint64* out_sampled_ns_total,
    int* out_profile_count) {
    SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile
        aggregated[SDL_arraysize(perf_capture_fast_non_integer_lookup_patterns)] = { 0 };
    int aggregated_count = 0;
    Uint64 task_total = 0;
    Uint64 pixel_total = 0;
    Uint64 sampled_ns_total = 0;

    for (int i = 0; i < pattern_count; i++) {
        const PerfCaptureFastNonIntegerLookupPatternExactProfile* pattern = &patterns[i];
        int existing = -1;
        for (int j = 0; j < aggregated_count; j++) {
            if (fast_non_integer_lookup_profile_matches_pattern(&aggregated[j], pattern)) {
                existing = j;
                break;
            }
        }

        if (existing < 0) {
            if (aggregated_count >= (int)SDL_arraysize(aggregated)) {
                continue;
            }

            existing = aggregated_count++;
            SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile* entry = &aggregated[existing];
            SDL_zero(*entry);
            entry->source_format = pattern->source_format;
            entry->source_width = pattern->source_width;
            entry->source_height = pattern->source_height;
            entry->alpha_only = pattern->alpha_only;
            entry->rgb_mod = pattern->rgb_mod;
            entry->opaque_color = pattern->opaque_color;
            entry->integer_positions = pattern->integer_positions;
            entry->integer_source_rect = pattern->integer_source_rect;
            entry->full_texture_source_rect = pattern->full_texture_source_rect;
            entry->clipped = pattern->clipped;
            entry->flip_h = pattern->flip_h;
            entry->flip_v = pattern->flip_v;
            entry->source_w = pattern->source_w;
            entry->source_h = pattern->source_h;
            entry->visible_w = pattern->visible_w;
            entry->visible_h = pattern->visible_h;
            entry->x_lookup_signature = pattern->x_lookup_signature;
            entry->y_lookup_signature = pattern->y_lookup_signature;
        }

        SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile* entry = &aggregated[existing];
        entry->task_count += pattern->task_count;
        entry->submitted_pixels += pattern->submitted_pixels;
        entry->sampled_ns += pattern->sampled_ns;
        task_total += pattern->task_count;
        pixel_total += pattern->submitted_pixels;
        sampled_ns_total += pattern->sampled_ns;
    }

    for (int i = 0; i < aggregated_count; i++) {
        for (int j = 0; j < pattern_count; j++) {
            const PerfCaptureFastNonIntegerLookupPatternExactProfile* pattern = &patterns[j];
            if (!fast_non_integer_lookup_profile_matches_pattern(&aggregated[i], pattern)) {
                continue;
            }

            bool already_counted = false;
            for (int k = 0; k < j; k++) {
                const PerfCaptureFastNonIntegerLookupPatternExactProfile* prior = &patterns[k];
                if (!fast_non_integer_lookup_profile_matches_pattern(&aggregated[i], prior)) {
                    continue;
                }
                if ((prior->texture_handle == pattern->texture_handle) &&
                    (prior->palette_handle == pattern->palette_handle)) {
                    already_counted = true;
                    break;
                }
            }
            if (!already_counted) {
                aggregated[i].contributing_family_count += 1;
            }
        }
    }

    if (out_task_total != NULL) {
        *out_task_total = task_total;
    }
    if (out_pixel_total != NULL) {
        *out_pixel_total = pixel_total;
    }
    if (out_sampled_ns_total != NULL) {
        *out_sampled_ns_total = sampled_ns_total;
    }
    if (out_profile_count != NULL) {
        *out_profile_count = aggregated_count;
    }
    if ((out_profiles == NULL) || (max_profiles <= 0) || (aggregated_count <= 0)) {
        return 0;
    }

    int selected_count = 0;
    while ((selected_count < max_profiles) && (selected_count < aggregated_count)) {
        int best_index = -1;
        for (int i = 0; i < aggregated_count; i++) {
            const SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile* candidate = &aggregated[i];

            bool already_selected = false;
            for (int j = 0; j < selected_count; j++) {
                if (fast_non_integer_lookup_profile_matches_profile(candidate, &out_profiles[j])) {
                    already_selected = true;
                    break;
                }
            }
            if (already_selected) {
                continue;
            }

            if (best_index < 0) {
                best_index = i;
                continue;
            }

            const SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile* best = &aggregated[best_index];
            if ((candidate->sampled_ns > best->sampled_ns) ||
                ((candidate->sampled_ns == best->sampled_ns) &&
                 (candidate->submitted_pixels > best->submitted_pixels)) ||
                ((candidate->sampled_ns == best->sampled_ns) &&
                 (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->task_count > best->task_count)) ||
                ((candidate->sampled_ns == best->sampled_ns) &&
                 (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->task_count == best->task_count) &&
                 (candidate->contributing_family_count > best->contributing_family_count)) ||
                ((candidate->sampled_ns == best->sampled_ns) &&
                 (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->task_count == best->task_count) &&
                 (candidate->contributing_family_count == best->contributing_family_count) &&
                 (candidate->source_w > best->source_w)) ||
                ((candidate->sampled_ns == best->sampled_ns) &&
                 (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->task_count == best->task_count) &&
                 (candidate->contributing_family_count == best->contributing_family_count) &&
                 (candidate->source_w == best->source_w) && (candidate->source_h > best->source_h))) {
                best_index = i;
            }
        }

        if (best_index < 0) {
            break;
        }

        out_profiles[selected_count] = aggregated[best_index];
        selected_count += 1;
    }

    return selected_count;
}

static void note_perf_capture_fast_non_integer_shape(const TexturedRectFamilyProfile* profile, Uint64 sampled_ns) {
    if (!frame_stats_extended_enabled || (profile == NULL)) {
        return;
    }

    SDLGameRenderer_PerfCaptureTexturedRectExactShape* entry = NULL;
    for (int i = 0; i < perf_capture_fast_non_integer_shape_count; i++) {
        if (textured_rect_exact_shape_matches_profile(&perf_capture_fast_non_integer_shapes[i], profile)) {
            entry = &perf_capture_fast_non_integer_shapes[i];
            break;
        }
    }

    if ((entry == NULL) &&
        (perf_capture_fast_non_integer_shape_count < (int)SDL_arraysize(perf_capture_fast_non_integer_shapes))) {
        entry = &perf_capture_fast_non_integer_shapes[perf_capture_fast_non_integer_shape_count];
        perf_capture_fast_non_integer_shape_count += 1;
        SDL_zero(*entry);
        entry->texture_handle = profile->texture_handle;
        entry->palette_handle = profile->palette_handle;
        entry->source_format = profile->source_format;
        entry->source_width = profile->source_width;
        entry->source_height = profile->source_height;
        entry->alpha_only = profile->alpha_only ? 1 : 0;
        entry->rgb_mod = profile->rgb_mod ? 1 : 0;
        entry->opaque_color = profile->opaque_color ? 1 : 0;
        entry->integer_positions = profile->integer_positions ? 1 : 0;
        entry->integer_source_rect = profile->integer_source_rect ? 1 : 0;
        entry->full_texture_source_rect = profile->full_texture_source_rect ? 1 : 0;
        entry->clipped = profile->clipped ? 1 : 0;
        entry->flip_h = profile->flip_h ? 1 : 0;
        entry->flip_v = profile->flip_v ? 1 : 0;
        entry->source_w = profile->source_w;
        entry->source_h = profile->source_h;
        entry->visible_w = profile->visible_w;
        entry->visible_h = profile->visible_h;
    }

    if (entry == NULL) {
        return;
    }

    entry->task_count += 1;
    entry->submitted_pixels += profile->submitted_pixels;
    entry->sampled_ns += sampled_ns;
}

static void note_perf_capture_fast_non_integer_lookup_pattern(
    const TexturedRectFamilyProfile* profile,
    const SDLSoftwareFrame_NonIntegerTelemetry* non_integer_telemetry,
    Uint64 sampled_ns) {
    if (!frame_stats_extended_enabled || (profile == NULL) || (non_integer_telemetry == NULL)) {
        return;
    }

    PerfCaptureFastNonIntegerLookupPatternExactProfile* entry = NULL;
    for (int i = 0; i < perf_capture_fast_non_integer_lookup_pattern_count; i++) {
        if (perf_capture_fast_non_integer_lookup_pattern_matches_profile(&perf_capture_fast_non_integer_lookup_patterns[i],
                                                                         profile,
                                                                         non_integer_telemetry->x_lookup_signature,
                                                                         non_integer_telemetry->y_lookup_signature)) {
            entry = &perf_capture_fast_non_integer_lookup_patterns[i];
            break;
        }
    }

    if ((entry == NULL) &&
        (perf_capture_fast_non_integer_lookup_pattern_count <
         (int)SDL_arraysize(perf_capture_fast_non_integer_lookup_patterns))) {
        entry = &perf_capture_fast_non_integer_lookup_patterns[perf_capture_fast_non_integer_lookup_pattern_count];
        perf_capture_fast_non_integer_lookup_pattern_count += 1;
        SDL_zero(*entry);
        entry->texture_handle = profile->texture_handle;
        entry->palette_handle = profile->palette_handle;
        entry->source_format = profile->source_format;
        entry->source_width = profile->source_width;
        entry->source_height = profile->source_height;
        entry->alpha_only = profile->alpha_only ? 1 : 0;
        entry->rgb_mod = profile->rgb_mod ? 1 : 0;
        entry->opaque_color = profile->opaque_color ? 1 : 0;
        entry->integer_positions = profile->integer_positions ? 1 : 0;
        entry->integer_source_rect = profile->integer_source_rect ? 1 : 0;
        entry->full_texture_source_rect = profile->full_texture_source_rect ? 1 : 0;
        entry->clipped = profile->clipped ? 1 : 0;
        entry->flip_h = profile->flip_h ? 1 : 0;
        entry->flip_v = profile->flip_v ? 1 : 0;
        entry->source_w = profile->source_w;
        entry->source_h = profile->source_h;
        entry->visible_w = profile->visible_w;
        entry->visible_h = profile->visible_h;
        entry->x_lookup_signature = non_integer_telemetry->x_lookup_signature;
        entry->y_lookup_signature = non_integer_telemetry->y_lookup_signature;
    }

    if (entry == NULL) {
        return;
    }

    entry->task_count += 1;
    entry->submitted_pixels += profile->submitted_pixels;
    entry->sampled_ns += sampled_ns;
}

static void note_perf_capture_fast_exact_family(const RenderTask* task,
                                                const SDL_Surface* dst_surface,
                                                Uint64 lookup_entries,
                                                Uint64 sampled_ns) {
    note_perf_capture_textured_rect_family(
        task,
        dst_surface,
        lookup_entries,
        NULL,
        sampled_ns,
        perf_capture_fast_exact_families,
        &perf_capture_fast_exact_family_count,
        (int)SDL_arraysize(perf_capture_fast_exact_families),
        NULL);
}

static void note_perf_capture_fast_non_integer_family(const RenderTask* task,
                                                      const SDL_Surface* dst_surface,
                                                      Uint64 lookup_entries,
                                                      const SDLSoftwareFrame_NonIntegerTelemetry* non_integer_telemetry,
                                                      Uint64 sampled_ns) {
    TexturedRectFamilyProfile profile = { 0 };
    profile.texture_handle = -1;
    note_perf_capture_textured_rect_family(
        task,
        dst_surface,
        lookup_entries,
        non_integer_telemetry,
        sampled_ns,
        perf_capture_fast_non_integer_families,
        &perf_capture_fast_non_integer_family_count,
        (int)SDL_arraysize(perf_capture_fast_non_integer_families),
        &profile);
    if (frame_stats_extended_enabled && (profile.texture_handle >= 0)) {
        note_perf_capture_fast_non_integer_shape(&profile, sampled_ns);
        note_perf_capture_fast_non_integer_lookup_pattern(&profile, non_integer_telemetry, sampled_ns);
        note_perf_capture_sa_burst_effect_sample(&profile, sampled_ns);
    }
}

static void note_perf_capture_generic_textured_family(const RenderTask* task,
                                                      const SDL_Surface* dst_surface,
                                                      Uint64 lookup_entries,
                                                      Uint64 sampled_ns) {
    note_perf_capture_textured_rect_family(
        task,
        dst_surface,
        lookup_entries,
        NULL,
        sampled_ns,
        perf_capture_generic_textured_families,
        &perf_capture_generic_textured_family_count,
        (int)SDL_arraysize(perf_capture_generic_textured_families),
        NULL);
}

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
        if (task->texture != NULL) {
            note_perf_capture_textured_geometry_fallback_family(task);
        }
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
                                                 const SoftwareFrameFastCopyPlan* plan,
                                                 const SDL_Surface* dst_surface,
                                                 Uint64 non_integer_lookup_entries,
                                                 Uint64 sampled_ns) {
    if ((task == NULL) ||
        (!frame_stats_extended_enabled && !perf_capture_basic_first_window_family_snapshots_enabled)) {
        return;
    }

    const Uint64 submitted_pixels = render_task_submitted_pixels(task);
    if (result == SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT) {
        if (!frame_stats_extended_enabled) {
            return;
        }
        frame_stats.software_frame_fast_exact_tasks += 1;
        frame_stats.software_frame_fast_exact_pixels += submitted_pixels;
        note_perf_capture_fast_exact_family(task, dst_surface, 0, sampled_ns);
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
        if (!frame_stats_extended_enabled) {
            return;
        }
        frame_stats.software_frame_fast_scaled_tasks += 1;
        frame_stats.software_frame_fast_scaled_pixels += submitted_pixels;
        return;
    }

    note_perf_capture_generic_textured_family(
        task,
        dst_surface,
        result == SOFTWARE_FRAME_FAST_COPY_RESULT_NON_INTEGER ? non_integer_lookup_entries : 0,
        sampled_ns);
    if (!frame_stats_extended_enabled) {
        return;
    }
    frame_stats.software_frame_generic_textured_tasks += 1;
    frame_stats.software_frame_generic_textured_pixels += submitted_pixels;
    if (software_frame_color_is_alpha_only(task->color)) {
        frame_stats.software_frame_generic_textured_alpha_only_tasks += 1;
        frame_stats.software_frame_generic_textured_alpha_only_pixels += submitted_pixels;
    } else if (software_frame_color_has_rgb_mod(task->color)) {
        frame_stats.software_frame_generic_textured_rgb_mod_tasks += 1;
        frame_stats.software_frame_generic_textured_rgb_mod_pixels += submitted_pixels;
    }
    switch (result) {
    case SOFTWARE_FRAME_FAST_COPY_RESULT_COLOR_MOD:
        frame_stats.software_frame_fast_miss_color_mod += 1;
        break;
    case SOFTWARE_FRAME_FAST_COPY_RESULT_NON_INTEGER:
        frame_stats.software_frame_fast_miss_non_integer += 1;
        frame_stats.software_frame_fast_miss_non_integer_lookup_entries += non_integer_lookup_entries;
        if (submitted_pixels >= 256u) {
            frame_stats.software_frame_fast_miss_non_integer_ge_256_tasks += 1;
            frame_stats.software_frame_fast_miss_non_integer_ge_256_pixels += submitted_pixels;
            frame_stats.software_frame_fast_miss_non_integer_ge_256_lookup_entries += non_integer_lookup_entries;
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
                                                 const SoftwareFrameFastCopyPlan* plan,
                                                 const SDL_Surface* dst_surface,
                                                 Uint64 non_integer_lookup_entries,
                                                 Uint64 sampled_ns) {
    (void)task;
    (void)result;
    (void)plan;
    (void)dst_surface;
    (void)non_integer_lookup_entries;
    (void)sampled_ns;
}
#endif

#if ENABLE_PERF_TELEMETRY
static void note_software_frame_fast_non_integer(const RenderTask* task,
                                                 const SDL_Surface* dst_surface,
                                                 Uint64 lookup_entries,
                                                 const SDLSoftwareFrame_NonIntegerTelemetry* non_integer_telemetry,
                                                 Uint64 sampled_ns) {
    if ((task == NULL) ||
        (!frame_stats_extended_enabled && !perf_capture_basic_first_window_family_snapshots_enabled)) {
        return;
    }

    const Uint64 submitted_pixels = render_task_submitted_pixels(task);
    if (frame_stats_extended_enabled) {
        frame_stats.software_frame_fast_non_integer_tasks += 1;
        frame_stats.software_frame_fast_non_integer_pixels += submitted_pixels;
        frame_stats.software_frame_fast_non_integer_lookup_entries += lookup_entries;
        if (non_integer_telemetry != NULL) {
            frame_stats.software_frame_fast_non_integer_source_alpha_opaque_pixels +=
                non_integer_telemetry->source_alpha_opaque_pixels;
            frame_stats.software_frame_fast_non_integer_source_alpha_transparent_pixels +=
                non_integer_telemetry->source_alpha_transparent_pixels;
            frame_stats.software_frame_fast_non_integer_source_alpha_blended_pixels +=
                non_integer_telemetry->source_alpha_blended_pixels;
            frame_stats.software_frame_fast_non_integer_same_source_runs += non_integer_telemetry->same_source_runs;
            frame_stats.software_frame_fast_non_integer_same_source_reuse_runs +=
                non_integer_telemetry->same_source_reuse_runs;
            frame_stats.software_frame_fast_non_integer_same_source_reused_pixels +=
                non_integer_telemetry->same_source_reused_pixels;
            frame_stats.software_frame_fast_non_integer_same_source_opaque_reused_pixels +=
                non_integer_telemetry->same_source_opaque_reused_pixels;
            frame_stats.software_frame_fast_non_integer_same_source_transparent_reused_pixels +=
                non_integer_telemetry->same_source_transparent_reused_pixels;
            frame_stats.software_frame_fast_non_integer_same_source_blended_reused_pixels +=
                non_integer_telemetry->same_source_blended_reused_pixels;
            if (non_integer_telemetry->same_source_max_run_length >
                frame_stats.software_frame_fast_non_integer_same_source_max_run_length) {
                frame_stats.software_frame_fast_non_integer_same_source_max_run_length =
                    non_integer_telemetry->same_source_max_run_length;
            }
        }
    }
    if (perf_capture_basic_first_window_render_subphases_enabled && (non_integer_telemetry != NULL) &&
        (sampled_ns > 0)) {
        perf_capture_fast_non_integer_phase_totals.sampled_calls += 1u;
        perf_capture_fast_non_integer_phase_totals.lookup_x_ns += non_integer_telemetry->sampled_lookup_x_ns;
        perf_capture_fast_non_integer_phase_totals.lookup_y_ns += non_integer_telemetry->sampled_lookup_y_ns;
        perf_capture_fast_non_integer_phase_totals.pair_lookup_ns += non_integer_telemetry->sampled_pair_lookup_ns;
        perf_capture_fast_non_integer_phase_totals.reuse_telemetry_ns +=
            non_integer_telemetry->sampled_reuse_telemetry_ns;
        perf_capture_fast_non_integer_phase_totals.row_raster_ns += non_integer_telemetry->sampled_row_raster_ns;
    }
    note_perf_capture_fast_non_integer_family(task, dst_surface, lookup_entries, non_integer_telemetry, sampled_ns);
    if (!frame_stats_extended_enabled) {
        return;
    }
    if (software_frame_color_is_alpha_only(task->color)) {
        frame_stats.software_frame_fast_non_integer_alpha_only_tasks += 1;
        frame_stats.software_frame_fast_non_integer_alpha_only_pixels += submitted_pixels;
    } else if (software_frame_color_has_rgb_mod(task->color)) {
        frame_stats.software_frame_fast_non_integer_rgb_mod_tasks += 1;
        frame_stats.software_frame_fast_non_integer_rgb_mod_pixels += submitted_pixels;
    }
}
#else
static void note_software_frame_fast_non_integer(const RenderTask* task,
                                                 const SDL_Surface* dst_surface,
                                                 Uint64 lookup_entries,
                                                 const SDLSoftwareFrame_NonIntegerTelemetry* non_integer_telemetry,
                                                 Uint64 sampled_ns) {
    (void)task;
    (void)dst_surface;
    (void)lookup_entries;
    (void)non_integer_telemetry;
    (void)sampled_ns;
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

static bool try_merge_software_frame_rect_tasks(RenderTask* merged_task,
                                                const RenderTask* next_task,
                                                const SDL_Surface* dst_surface,
                                                SoftwareFrameRectStripMergeAxis* inout_axis) {
    if ((merged_task == NULL) || (next_task == NULL) || (inout_axis == NULL) ||
        (dst_surface == NULL) ||
        (merged_task->type != RENDER_TASK_TYPE_TEXTURED_RECT) || (next_task->type != RENDER_TASK_TYPE_TEXTURED_RECT) ||
        (merged_task->texture == NULL) || (next_task->texture == NULL) ||
        (merged_task->software_source_surface != next_task->software_source_surface) ||
        (merged_task->flip != SDL_FLIP_NONE) || (next_task->flip != SDL_FLIP_NONE)) {
        return false;
    }

    SoftwareFrameRectStripMergeAxis axis = *inout_axis;
    const SoftwareFrameFastCopyResult merged_result =
        build_software_frame_fast_copy_plan(merged_task, dst_surface, merged_task->software_source_surface, NULL);
    if ((axis == SOFTWARE_FRAME_RECT_STRIP_MERGE_AXIS_NONE) && (merged_result != SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT)) {
        return false;
    }
    const SoftwareFrameFastCopyResult next_result =
        build_software_frame_fast_copy_plan(next_task, dst_surface, next_task->software_source_surface, NULL);
    if (next_result != SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT) {
        return false;
    }

    if (axis == SOFTWARE_FRAME_RECT_STRIP_MERGE_AXIS_NONE) {
        if (can_merge_rect_tasks_horizontally(merged_task, next_task)) {
            axis = SOFTWARE_FRAME_RECT_STRIP_MERGE_AXIS_HORIZONTAL;
        } else if (can_merge_rect_tasks_vertically(merged_task, next_task)) {
            axis = SOFTWARE_FRAME_RECT_STRIP_MERGE_AXIS_VERTICAL;
        } else {
            return false;
        }
    } else if ((axis == SOFTWARE_FRAME_RECT_STRIP_MERGE_AXIS_HORIZONTAL)
               ? !can_merge_rect_tasks_horizontally(merged_task, next_task)
               : !can_merge_rect_tasks_vertically(merged_task, next_task)) {
        return false;
    }

    RenderTask candidate_task = *merged_task;
    if (axis == SOFTWARE_FRAME_RECT_STRIP_MERGE_AXIS_HORIZONTAL) {
        candidate_task.dst_rect.w = (next_task->dst_rect.x + next_task->dst_rect.w) - candidate_task.dst_rect.x;
        candidate_task.src_uv_rect.w = (next_task->src_uv_rect.x + next_task->src_uv_rect.w) - candidate_task.src_uv_rect.x;
    } else {
        candidate_task.dst_rect.h = (next_task->dst_rect.y + next_task->dst_rect.h) - candidate_task.dst_rect.y;
        candidate_task.src_uv_rect.h = (next_task->src_uv_rect.y + next_task->src_uv_rect.h) - candidate_task.src_uv_rect.y;
    }

    const SoftwareFrameFastCopyResult candidate_result = build_software_frame_fast_copy_plan(
        &candidate_task, dst_surface, candidate_task.software_source_surface, NULL);
    if (candidate_result != SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT) {
        return false;
    }

    *merged_task = candidate_task;
    *inout_axis = axis;
    return true;
}

#if RENDERER_HAVE_NEON
/* --- Optimization A: NEON-vectorized modulate for 4 pixels at a time ---
 * The modulation color is constant across all pixels in a render task,
 * so we precompute the color channel vector once and apply it to batches
 * of 4 pixels using NEON widening multiply + narrowing shift.
 *
 * ARGB8888 layout in memory (little-endian ARM): byte 0=B, 1=G, 2=R, 3=A
 * When loaded as uint32, bit layout: [31:24]=A [23:16]=R [15:8]=G [7:0]=B
 *
 * We treat each uint32 pixel as 4 bytes via vreinterpret and process all
 * 16 bytes (4 pixels x 4 channels) in one NEON operation. */

typedef struct NeonModulateState {
    uint8x8_t color_lo;  /* color channels for pixels 0-1 (8 bytes) */
    uint8x8_t color_hi;  /* color channels for pixels 2-3 (8 bytes) */
} NeonModulateState;

static NeonModulateState neon_modulate_init(Uint32 color) {
    /* Splat the color to all 4 pixel positions within a 16-byte register */
    const uint32x4_t color_vec = vdupq_n_u32(color);
    const uint8x16_t color_bytes = vreinterpretq_u8_u32(color_vec);
    NeonModulateState state;
    state.color_lo = vget_low_u8(color_bytes);
    state.color_hi = vget_high_u8(color_bytes);
    return state;
}

/* Modulate 4 ARGB8888 pixels by a constant color using NEON.
 * Uses (a * b + 128) >> 8 as a fast approximation of (a * b + 127) / 255.
 * Max error vs exact: 1 LSB, visually imperceptible. */
static inline void neon_modulate_4pixels(
    const Uint32* src, Uint32* dst, const NeonModulateState* state) {
    /* Load 4 source pixels = 16 bytes */
    const uint8x16_t src_bytes = vreinterpretq_u8_u32(vld1q_u32(src));

    /* Widening multiply: u8 x u8 -> u16, for low 8 bytes (pixels 0-1) */
    const uint16x8_t prod_lo = vmull_u8(vget_low_u8(src_bytes), state->color_lo);
    /* Widening multiply for high 8 bytes (pixels 2-3) */
    const uint16x8_t prod_hi = vmull_u8(vget_high_u8(src_bytes), state->color_hi);

    /* Add rounding bias of 128 */
    const uint16x8_t biased_lo = vaddq_u16(prod_lo, vdupq_n_u16(128));
    const uint16x8_t biased_hi = vaddq_u16(prod_hi, vdupq_n_u16(128));

    /* Narrow with >>8: u16 -> u8 */
    const uint8x8_t result_lo = vshrn_n_u16(biased_lo, 8);
    const uint8x8_t result_hi = vshrn_n_u16(biased_hi, 8);

    /* Combine and store as 4 uint32s */
    const uint8x16_t result_bytes = vcombine_u8(result_lo, result_hi);
    vst1q_u32(dst, vreinterpretq_u32_u8(result_bytes));
}

/* --- NEON-vectorized alpha blending for 4 pixels at a time ---
 * Implements the dst_a==255 fast path from blend_argb8888():
 *   out_ch = (src_ch * src_a + dst_ch * (255 - src_a) + 128) >> 8
 * with branchless handling of alpha==0 (keep dst) and alpha==255 (keep src).
 *
 * ARGB8888 little-endian byte layout: [B, G, R, A] at byte offsets 0-3.
 * We broadcast each pixel's alpha byte to all 4 channel positions using
 * vtbl1_u8 with a shuffle index table, then do the blend math on all
 * channels simultaneously. Output alpha is forced to 0xFF. */

static inline __attribute__((unused)) void neon_blend_4pixels(const Uint32* src, Uint32* dst) {
    /* Load 4 source and 4 destination pixels */
    const uint8x16_t src_bytes = vreinterpretq_u8_u32(vld1q_u32(src));
    const uint8x16_t dst_bytes = vreinterpretq_u8_u32(vld1q_u32(dst));

    /* Shuffle table to broadcast alpha (byte 3) to all 4 channels within each pixel.
     * For 8-byte half (2 pixels): pixel0 alpha is byte 3, pixel1 alpha is byte 7.
     * vtbl1_u8 uses the index to pick bytes from the source register. */
    static const uint8_t alpha_shuffle_tbl[8] = { 3, 3, 3, 3, 7, 7, 7, 7 };
    const uint8x8_t shuffle = vld1_u8(alpha_shuffle_tbl);

    /* Extract alpha broadcast for low 2 pixels and high 2 pixels */
    const uint8x8_t src_lo = vget_low_u8(src_bytes);
    const uint8x8_t src_hi = vget_high_u8(src_bytes);
    const uint8x8_t alpha_lo = vtbl1_u8(src_lo, shuffle);
    const uint8x8_t alpha_hi = vtbl1_u8(src_hi, shuffle);

    /* inv_alpha = 255 - src_alpha */
    const uint8x8_t ones8 = vdup_n_u8(255);
    const uint8x8_t inv_alpha_lo = vsub_u8(ones8, alpha_lo);
    const uint8x8_t inv_alpha_hi = vsub_u8(ones8, alpha_hi);

    /* Blend: result = (src * alpha + dst * inv_alpha + 128) >> 8 */
    const uint8x8_t dst_lo = vget_low_u8(dst_bytes);
    const uint8x8_t dst_hi = vget_high_u8(dst_bytes);

    /* Low 2 pixels */
    uint16x8_t acc_lo = vmull_u8(src_lo, alpha_lo);
    acc_lo = vmlal_u8(acc_lo, dst_lo, inv_alpha_lo);
    acc_lo = vaddq_u16(acc_lo, vdupq_n_u16(128));
    const uint8x8_t blend_lo = vshrn_n_u16(acc_lo, 8);

    /* High 2 pixels */
    uint16x8_t acc_hi = vmull_u8(src_hi, alpha_hi);
    acc_hi = vmlal_u8(acc_hi, dst_hi, inv_alpha_hi);
    acc_hi = vaddq_u16(acc_hi, vdupq_n_u16(128));
    const uint8x8_t blend_hi = vshrn_n_u16(acc_hi, 8);

    /* Combine blended result */
    uint8x16_t result = vcombine_u8(blend_lo, blend_hi);

    /* Force output alpha to 0xFF (byte 3 of each pixel in LE = lane 3,7,11,15) */
    static const uint8_t alpha_mask_tbl[16] = {
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF
    };
    const uint8x16_t alpha_mask = vld1q_u8(alpha_mask_tbl);
    result = vorrq_u8(result, alpha_mask);

    /* Branchless edge-case handling:
     * - alpha==0:   keep dst (select dst)
     * - alpha==255: keep src (select src)
     * - otherwise:  keep blended result
     * Build per-pixel masks from the alpha bytes. */
    const uint8x16_t alpha_full = vcombine_u8(alpha_lo, alpha_hi);
    const uint8x16_t zero_vec = vdupq_n_u8(0);
    const uint8x16_t ff_vec = vdupq_n_u8(255);

    /* mask_zero: 0xFF in all lanes where alpha==0 */
    const uint8x16_t mask_zero = vceqq_u8(alpha_full, zero_vec);
    /* mask_opaque: 0xFF in all lanes where alpha==255 */
    const uint8x16_t mask_opaque = vceqq_u8(alpha_full, ff_vec);

    /* Start with blended result, replace with dst where alpha==0 */
    result = vbslq_u8(mask_zero, dst_bytes, result);
    /* Then replace with src where alpha==255 */
    result = vbslq_u8(mask_opaque, src_bytes, result);

    vst1q_u32(dst, vreinterpretq_u32_u8(result));
}

/* Fused modulate+blend: modulate src by constant color, then alpha-blend onto dst.
 * Avoids intermediate store/load between modulate and blend stages.
 * Used for ghost sprites and other semi-transparent color-modulated content. */
static inline void neon_blend_modulate_4pixels(
    const Uint32* src, Uint32* dst, const NeonModulateState* mod) {
    /* --- Stage 1: Modulate src by constant color --- */
    const uint8x16_t src_bytes = vreinterpretq_u8_u32(vld1q_u32(src));

    /* Widening multiply: src * color -> u16 */
    const uint16x8_t mod_lo = vmull_u8(vget_low_u8(src_bytes), mod->color_lo);
    const uint16x8_t mod_hi = vmull_u8(vget_high_u8(src_bytes), mod->color_hi);

    /* Add rounding bias and narrow: (prod + 128) >> 8 */
    const uint16x8_t bias = vdupq_n_u16(128);
    const uint8x8_t modulated_lo = vshrn_n_u16(vaddq_u16(mod_lo, bias), 8);
    const uint8x8_t modulated_hi = vshrn_n_u16(vaddq_u16(mod_hi, bias), 8);

    /* --- Stage 2: Alpha-blend modulated src onto dst --- */
    const uint8x16_t dst_bytes = vreinterpretq_u8_u32(vld1q_u32(dst));

    /* Broadcast modulated alpha to all channels per pixel */
    static const uint8_t alpha_shuffle_tbl[8] = { 3, 3, 3, 3, 7, 7, 7, 7 };
    const uint8x8_t shuffle = vld1_u8(alpha_shuffle_tbl);
    const uint8x8_t alpha_lo = vtbl1_u8(modulated_lo, shuffle);
    const uint8x8_t alpha_hi = vtbl1_u8(modulated_hi, shuffle);

    /* inv_alpha = 255 - alpha */
    const uint8x8_t ones8 = vdup_n_u8(255);
    const uint8x8_t inv_alpha_lo = vsub_u8(ones8, alpha_lo);
    const uint8x8_t inv_alpha_hi = vsub_u8(ones8, alpha_hi);

    /* Blend: (modulated * alpha + dst * inv_alpha + 128) >> 8 */
    const uint8x8_t dst_lo = vget_low_u8(dst_bytes);
    const uint8x8_t dst_hi = vget_high_u8(dst_bytes);

    uint16x8_t acc_lo = vmull_u8(modulated_lo, alpha_lo);
    acc_lo = vmlal_u8(acc_lo, dst_lo, inv_alpha_lo);
    acc_lo = vaddq_u16(acc_lo, bias);
    const uint8x8_t blend_lo = vshrn_n_u16(acc_lo, 8);

    uint16x8_t acc_hi = vmull_u8(modulated_hi, alpha_hi);
    acc_hi = vmlal_u8(acc_hi, dst_hi, inv_alpha_hi);
    acc_hi = vaddq_u16(acc_hi, bias);
    const uint8x8_t blend_hi = vshrn_n_u16(acc_hi, 8);

    /* Combine blended result */
    uint8x16_t result = vcombine_u8(blend_lo, blend_hi);

    /* Force output alpha to 0xFF */
    static const uint8_t alpha_mask_tbl[16] = {
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF
    };
    result = vorrq_u8(result, vld1q_u8(alpha_mask_tbl));

    /* Branchless edge cases: alpha==0 -> keep dst, alpha==255 -> keep modulated src */
    const uint8x16_t alpha_full = vcombine_u8(alpha_lo, alpha_hi);
    const uint8x16_t modulated_full = vcombine_u8(modulated_lo, modulated_hi);
    const uint8x16_t mask_zero = vceqq_u8(alpha_full, vdupq_n_u8(0));
    const uint8x16_t mask_opaque = vceqq_u8(alpha_full, vdupq_n_u8(255));

    result = vbslq_u8(mask_zero, dst_bytes, result);
    result = vbslq_u8(mask_opaque, modulated_full, result);

    vst1q_u32(dst, vreinterpretq_u32_u8(result));
}
#endif /* RENDERER_HAVE_NEON */

/* --- Optimization B: Ghost sprite detection for half-resolution rendering ---
 * Ghost/after-image sprites use bright_type[3] (blue tint) which produces
 * colors with B==0xFF, R==G, R<0xFF.  These semi-transparent blue overlays
 * can be rendered at half X resolution (process every other pixel, write
 * the same value twice) cutting expensive modulate+blend calls in half
 * with negligible visual impact since they're already translucent blurs. */
static bool is_ghost_sprite_color(Uint32 color) {
    /* Ghost sprites: blue-tinted (B=0xFF, R==G, R<0xFF) from bright_type[3]. */
    return is_blue_tint_color(color);
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

            /* Optimization B+D: detect ghost sprites (blue-tint from bright_type[3]).
             * These get half-X-resolution rendering (when ghost-resolution=half)
             * AND the fast blue-tint modulate. */
            const bool ghost_half_res = (ghost_resolution_mode == SDL_GAME_RENDERER_GHOST_RESOLUTION_HALF) &&
                                        is_ghost_sprite_color(color);
            const bool blue_tint = is_blue_tint_color(color);
            const Uint32 rg_factor = (color >> 16) & 0xFFu; /* R == G for blue tint */

#if RENDERER_HAVE_NEON
            /* Optimization A: NEON fast path for non-flipped forward scan.
             * Process 4 pixels at a time with NEON widening multiply.
             * Ghost sprites additionally skip every other pixel (half-res). */
            if (src_x_step == 1) {
                const NeonModulateState neon_state = neon_modulate_init(color);
                const int visible_w = plan->visible_w;
                /* For ghost half-res: process every other row too (fill from row above) */
                const int row_step = ghost_half_res ? 2 : 1;

                for (int row = 0; row < plan->visible_h; row += row_step) {
                    const Uint32* src_row = src_pixels + ((src_row0_y + (row * src_y_step)) * src_pitch) + src_row0_x;
                    Uint32* dst_row = dst_pixels + ((plan->dst_y0 + row) * dst_pitch) + plan->dst_x0;

                    /* Prefetch next iteration's source and destination rows to hide L1/L2 miss latency. */
                    if ((row + row_step) < plan->visible_h) {
                        __builtin_prefetch(src_pixels + ((src_row0_y + ((row + row_step) * src_y_step)) * src_pitch) + src_row0_x, 0, 0);
                        __builtin_prefetch(dst_pixels + ((plan->dst_y0 + row + row_step) * dst_pitch) + plan->dst_x0, 1, 0);
                    }

                    int col = 0;

                    if (ghost_half_res) {
                        /* Half-res X: process every other pixel pair, duplicate results.
                         * NEON processes 4 source pixels, but we step by 8 in source
                         * and write each result pixel twice (covering 8 dst columns). */
                        Uint32 modulated[4];
                        for (; (col + 7) < visible_w; col += 8) {
                            /* Load 4 pixels at positions col, col+2, col+4, col+6 */
                            const Uint32 sampled[4] = {
                                src_row[col], src_row[col + 2],
                                src_row[col + 4], src_row[col + 6]
                            };
                            neon_modulate_4pixels(sampled, modulated, &neon_state);
                            /* Write each modulated pixel twice (col, col+1), (col+2, col+3), ... */
                            for (int k = 0; k < 4; k++) {
                                const Uint32 src_pixel = modulated[k];
                                const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                                const int dst_col = col + (k * 2);
                                if (src_a == 0u) {
                                    continue;
                                }
                                if (src_a == 0xFFu) {
                                    dst_row[dst_col] = src_pixel;
                                    dst_row[dst_col + 1] = src_pixel;
                                    continue;
                                }
                                const Uint32 blended = blend_argb8888(dst_row[dst_col], src_pixel);
                                dst_row[dst_col] = blended;
                                dst_row[dst_col + 1] = blended;
                            }
                        }
                        /* Scalar tail for remaining pixels */
                        for (; col < visible_w; col += 2) {
                            Uint32 src_pixel = blue_tint
                                ? modulate_argb8888_blue_tint(src_row[col], rg_factor, mod_a)
                                : modulate_argb8888(src_row[col], color);
                            const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                            if (src_a == 0u) {
                                continue;
                            }
                            if (src_a == 0xFFu) {
                                dst_row[col] = src_pixel;
                                if ((col + 1) < visible_w) { dst_row[col + 1] = src_pixel; }
                                continue;
                            }
                            const Uint32 blended = blend_argb8888(dst_row[col], src_pixel);
                            dst_row[col] = blended;
                            if ((col + 1) < visible_w) { dst_row[col + 1] = blended; }
                        }
                    } else {
                        /* Full-res NEON path: fused modulate+blend, 4 contiguous pixels at a time.
                         * neon_blend_modulate_4pixels handles alpha==0/255 edge cases branchlessly. */
                        for (; (col + 3) < visible_w; col += 4) {
                            neon_blend_modulate_4pixels(src_row + col, dst_row + col, &neon_state);
                        }
                        /* Scalar tail */
                        for (; col < visible_w; col++) {
                            Uint32 src_pixel = blue_tint
                                ? modulate_argb8888_blue_tint(src_row[col], rg_factor, mod_a)
                                : modulate_argb8888(src_row[col], color);
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

                    /* Ghost half-res Y: duplicate this row to the next row */
                    if (ghost_half_res && ((row + 1) < plan->visible_h)) {
                        Uint32* next_dst_row = dst_pixels + ((plan->dst_y0 + row + 1) * dst_pitch) + plan->dst_x0;
                        SDL_memcpy(next_dst_row, dst_row, (size_t)visible_w * sizeof(Uint32));
                    }
                }
                return true;
            }
#endif /* RENDERER_HAVE_NEON */

            /* Scalar path: blue-tint fast path (Optimization D) or generic modulate.
             * Also applies ghost half-res (Optimization B) when applicable. */
            {
                const int row_step = ghost_half_res ? 2 : 1;
                const int col_step = ghost_half_res ? 2 : 1;

                for (int row = 0; row < plan->visible_h; row += row_step) {
                    const Uint32* src_row = src_pixels + ((src_row0_y + (row * src_y_step)) * src_pitch) + src_row0_x;
                    Uint32* dst_row = dst_pixels + ((plan->dst_y0 + row) * dst_pitch) + plan->dst_x0;

                    /* Prefetch next iteration's source and destination rows to hide L1/L2 miss latency. */
                    if ((row + row_step) < plan->visible_h) {
                        __builtin_prefetch(src_pixels + ((src_row0_y + ((row + row_step) * src_y_step)) * src_pitch) + src_row0_x, 0, 0);
                        __builtin_prefetch(dst_pixels + ((plan->dst_y0 + row + row_step) * dst_pitch) + plan->dst_x0, 1, 0);
                    }

                    const Uint32* src_pixel_ptr = src_row;
                    for (int col = 0; col < plan->visible_w; col += col_step) {
                        Uint32 src_pixel = blue_tint
                            ? modulate_argb8888_blue_tint(*src_pixel_ptr, rg_factor, mod_a)
                            : modulate_argb8888(*src_pixel_ptr, color);
                        const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                        if (src_a == 0u) {
                            src_pixel_ptr += src_x_step * col_step;
                            continue;
                        }
                        if (ghost_half_res) {
                            if (src_a == 0xFFu) {
                                dst_row[col] = src_pixel;
                                if ((col + 1) < plan->visible_w) { dst_row[col + 1] = src_pixel; }
                            } else {
                                const Uint32 blended = blend_argb8888(dst_row[col], src_pixel);
                                dst_row[col] = blended;
                                if ((col + 1) < plan->visible_w) { dst_row[col + 1] = blended; }
                            }
                        } else {
                            if (src_a == 0xFFu) {
                                dst_row[col] = src_pixel;
                            } else {
                                dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
                            }
                        }
                        src_pixel_ptr += src_x_step * col_step;
                    }

                    /* Ghost half-res Y: duplicate row */
                    if (ghost_half_res && ((row + 1) < plan->visible_h)) {
                        Uint32* next_dst_row = dst_pixels + ((plan->dst_y0 + row + 1) * dst_pitch) + plan->dst_x0;
                        SDL_memcpy(next_dst_row, dst_row, (size_t)plan->visible_w * sizeof(Uint32));
                    }
                }
            }
            return true;
        }

        for (int row = 0; row < plan->visible_h; row++) {
            const Uint32* src_row = src_pixels + ((src_row0_y + (row * src_y_step)) * src_pitch) + src_row0_x;
            Uint32* dst_row = dst_pixels + ((plan->dst_y0 + row) * dst_pitch) + plan->dst_x0;

            /* Prefetch next iteration's source and destination rows to hide L1/L2 miss latency. */
            if ((row + 1) < plan->visible_h) {
                __builtin_prefetch(src_pixels + ((src_row0_y + ((row + 1) * src_y_step)) * src_pitch) + src_row0_x, 0, 0);
                __builtin_prefetch(dst_pixels + ((plan->dst_y0 + row + 1) * dst_pitch) + plan->dst_x0, 1, 0);
            }

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

        /* Prefetch next iteration's source and destination rows to hide L1/L2 miss latency. */
        if ((row + 1) < plan->visible_h) {
            __builtin_prefetch(src_pixels + (src_y_lookup[row + 1] * src_pitch), 0, 0);
            __builtin_prefetch(dst_pixels + ((plan->dst_y0 + row + 1) * dst_pitch) + plan->dst_x0, 1, 0);
        }

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
    const Uint32 out_a = (src_a * mod_a + 128u) >> 8;
    const Uint32 out_r = (src_r * mod_r + 128u) >> 8;
    const Uint32 out_g = (src_g * mod_g + 128u) >> 8;
    const Uint32 out_b = (src_b * mod_b + 128u) >> 8;
    return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

/* --- Optimization D: Fast blue-tint modulate path ---
 * The bright_type[3] table (blue ghost tint) has B=0xFF and R==G.
 * For this case we skip the B multiply (it's a no-op) and use a single
 * factor for R and G since they're equal.  Returns true if color matches
 * the blue-tint pattern. */
static bool is_blue_tint_color(Uint32 color) {
    const Uint32 b = color & 0xFFu;
    const Uint32 g = (color >> 8) & 0xFFu;
    const Uint32 r = (color >> 16) & 0xFFu;
    return (b == 0xFFu) && (r == g) && (r < 0xFFu);
}

static Uint32 modulate_argb8888_blue_tint(Uint32 pixel, Uint32 rg_factor, Uint32 mod_a) {
    /* B channel: no-op (mod_b == 0xFF).
     * R and G channels: multiply by the same factor (rg_factor).
     * A channel: multiply by mod_a.
     * Uses >>8 approximation instead of /255 for speed — max error is 1 LSB. */
    const Uint32 src_a = (pixel >> 24) & 0xFFu;
    const Uint32 src_r = (pixel >> 16) & 0xFFu;
    const Uint32 src_g = (pixel >> 8) & 0xFFu;
    const Uint32 src_b = pixel & 0xFFu;
    const Uint32 out_a = (src_a * mod_a) >> 8;
    const Uint32 out_r = (src_r * rg_factor) >> 8;
    const Uint32 out_g = (src_g * rg_factor) >> 8;
    /* out_b = (src_b * 0xFF) >> 8 ≈ src_b, just pass through */
    return (out_a << 24) | (out_r << 16) | (out_g << 8) | src_b;
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
        const Uint32 out_r = (src_r_premul + (dst_r * inv_src_a) + 128u) >> 8;
        const Uint32 out_g = (src_g_premul + (dst_g * inv_src_a) + 128u) >> 8;
        const Uint32 out_b = (src_b_premul + (dst_b * inv_src_a) + 128u) >> 8;
        return 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
    }

    const Uint32 out_a = src_a + ((dst_a * inv_src_a + 128u) >> 8);
    if (out_a == 0u) {
        return 0u;
    }

    const Uint32 dst_r_premul = dst_r * dst_a;
    const Uint32 dst_g_premul = dst_g * dst_a;
    const Uint32 dst_b_premul = dst_b * dst_a;
    const Uint32 out_r_premul = src_r_premul + ((dst_r_premul * inv_src_a + 128u) >> 8);
    const Uint32 out_g_premul = src_g_premul + ((dst_g_premul * inv_src_a + 128u) >> 8);
    const Uint32 out_b_premul = src_b_premul + ((dst_b_premul * inv_src_a + 128u) >> 8);
    const Uint32 out_r = (out_r_premul + (out_a / 2u)) / out_a;
    const Uint32 out_g = (out_g_premul + (out_a / 2u)) / out_a;
    const Uint32 out_b = (out_b_premul + (out_a / 2u)) / out_a;
    return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static bool raster_full_height_diagonal_strip_to_software_frame(const SoftwareFrameSolidDiagonalStrip* strip,
                                                                SDL_Surface* dst_surface) {
    if ((strip == NULL) || (dst_surface == NULL) || (strip->bottom_y <= strip->top_y)) {
        return false;
    }

    Uint32* dst_pixels = (Uint32*)dst_surface->pixels;
    const int dst_pitch = dst_surface->pitch / (int)sizeof(Uint32);
    const int dst_y0 = clamp_to_range(strip->top_y, 0, dst_surface->h);
    const int dst_y1 = clamp_to_range(strip->bottom_y, 0, dst_surface->h);
    if (dst_y1 <= dst_y0) {
        return true;
    }

    const Uint32 src_a = (strip->color >> 24) & 0xFFu;
    if (src_a == 0u) {
        return true;
    }

    if (src_a == 255u) {
        for (int y = dst_y0; y < dst_y1; y++) {
            const int row_offset = y - strip->top_y;
            const int dst_x0 = clamp_to_range(strip->top_left_x + row_offset, 0, dst_surface->w);
            const int dst_x1 = clamp_to_range(strip->top_right_x + row_offset, 0, dst_surface->w);
            if (dst_x1 <= dst_x0) {
                continue;
            }

            Uint32* dst_row = dst_pixels + (y * dst_pitch) + dst_x0;
            fill_argb8888_span(dst_row, dst_x1 - dst_x0, strip->color);
        }
        return true;
    }

    const Uint32 inv_src_a = 255u - src_a;
    const Uint32 src_r = (strip->color >> 16) & 0xFFu;
    const Uint32 src_g = (strip->color >> 8) & 0xFFu;
    const Uint32 src_b = strip->color & 0xFFu;
    const Uint32 src_r_premul = src_r * src_a;
    const Uint32 src_g_premul = src_g * src_a;
    const Uint32 src_b_premul = src_b * src_a;
    for (int y = dst_y0; y < dst_y1; y++) {
        const int row_offset = y - strip->top_y;
        const int dst_x0 = clamp_to_range(strip->top_left_x + row_offset, 0, dst_surface->w);
        const int dst_x1 = clamp_to_range(strip->top_right_x + row_offset, 0, dst_surface->w);
        if (dst_x1 <= dst_x0) {
            continue;
        }

        Uint32* dst_row = dst_pixels + (y * dst_pitch) + dst_x0;
        for (int x = 0; x < (dst_x1 - dst_x0); x++) {
            dst_row[x] = blend_solid_argb8888(dst_row[x], src_a, inv_src_a, src_r_premul, src_g_premul, src_b_premul);
        }
    }
    return true;
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
    const Uint64 non_integer_lookup_entries = (Uint64)(dst_x1 - dst_x0) + (Uint64)(dst_y1 - dst_y0);

    SoftwareFrameFastCopyPlan fast_copy_plan = { 0 };
    SoftwareFrameFastCopyResult fast_copy_result =
        build_software_frame_fast_copy_plan(task, dst_surface, src_surface, &fast_copy_plan);
    const RenderTask* fast_copy_task = task;
    if ((fast_copy_result == SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT) ||
        (fast_copy_result == SOFTWARE_FRAME_FAST_COPY_RESULT_SCALED)) {
        const SDLGameRenderer_PerfCaptureRasterBucket raster_bucket =
            fast_copy_result == SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT
                ? SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_FAST_EXACT
                : SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_FAST_SCALED;
        const Uint64 sample_start_counter = begin_perf_capture_raster_bucket_sample(raster_bucket);
        if (try_fast_copy_fast_textured_task_to_software_frame(
                fast_copy_task, &fast_copy_plan, dst_surface, src_surface)) {
            const Uint64 sampled_ns =
                perf_capture_counter_delta_to_ns(sample_start_counter, SDL_GetPerformanceCounter());
            note_perf_capture_raster_bucket_sample(
                raster_bucket, fast_copy_task, sampled_ns);
            note_software_frame_fast_copy_result(
                fast_copy_task, fast_copy_result, &fast_copy_plan, dst_surface, 0, sampled_ns);
            if (src_locked) {
                SDL_UnlockSurface(src_surface);
            }
            return true;
        }
    } else if (fast_copy_result == SOFTWARE_FRAME_FAST_COPY_RESULT_NON_INTEGER) {
        const Uint64 submitted_pixels = render_task_submitted_pixels(task);
        const bool collect_phase_timing =
            frame_stats_extended_enabled || perf_capture_basic_first_window_render_subphases_enabled;
        const Uint64 sample_start_counter =
            submitted_pixels >= software_frame_non_integer_lookup_threshold_pixels
                ? begin_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_FAST_NON_INTEGER)
                : 0;
        SDLSoftwareFrame_NonIntegerTelemetry non_integer_telemetry;
        SDLSoftwareFrame_NonIntegerTelemetry* non_integer_telemetry_ptr =
            collect_phase_timing ? &non_integer_telemetry : NULL;
        if ((submitted_pixels >= software_frame_non_integer_lookup_threshold_pixels) &&
            SDLSoftwareFrame_RasterNonIntegerLookupARGB8888(
                &task->dst_rect,
                &task->src_uv_rect,
                task->flip,
                task->color,
                dst_surface,
                src_surface,
                non_integer_telemetry_ptr,
                collect_phase_timing && (sample_start_counter != 0),
                frame_stats_extended_enabled,
                perf_capture_fast_non_integer_reuse_telemetry_enabled,
                perf_capture_fast_non_integer_subrect_alpha_telemetry_enabled)) {
            const Uint64 sampled_ns =
                perf_capture_counter_delta_to_ns(sample_start_counter, SDL_GetPerformanceCounter());
            note_perf_capture_raster_bucket_sample(
                SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_FAST_NON_INTEGER,
                task,
                sampled_ns);
            note_software_frame_fast_non_integer(
                task, dst_surface, non_integer_lookup_entries, non_integer_telemetry_ptr, sampled_ns);
            if (src_locked) {
                SDL_UnlockSurface(src_surface);
            }
            return true;
        }
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
    const bool apply_color_mod = task->color != 0xFFFFFFFFu;
    /* Optimization D: precompute blue-tint state for the generic path */
    const bool generic_blue_tint = apply_color_mod && is_blue_tint_color(task->color);
    const Uint32 generic_rg_factor = (task->color >> 16) & 0xFFu;
    const Uint32 generic_mod_a = (task->color >> 24) & 0xFFu;
    const Uint64 generic_sample_start_counter =
        begin_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED);

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

        /* Prefetch next iteration's source and destination rows to hide L1/L2 miss latency. */
        if ((y + 1) < dst_y1) {
            float next_v = (((float)(y + 1) + 0.5f) - task->dst_rect.y) / task->dst_rect.h;
            next_v = SDL_max(0.0f, SDL_min(next_v, 0.999999f));
            if (flip_v) {
                next_v = 1.0f - next_v;
                next_v = SDL_max(0.0f, SDL_min(next_v, 0.999999f));
            }
            const int next_src_y = clamp_to_range((int)SDL_floorf(src_y_start + (next_v * src_y_span)), 0, src_surface->h - 1);
            __builtin_prefetch(src_pixels + (next_src_y * src_pitch), 0, 0);
            __builtin_prefetch(dst_pixels + ((y + 1) * dst_pitch) + dst_x0, 1, 0);
        }

        if (!apply_color_mod) {
            for (int x = dst_x0; x < dst_x1; x++) {
                float u = (((float)x + 0.5f) - task->dst_rect.x) / task->dst_rect.w;
                u = SDL_max(0.0f, SDL_min(u, 0.999999f));
                if (flip_h) {
                    u = 1.0f - u;
                    u = SDL_max(0.0f, SDL_min(u, 0.999999f));
                }

                const int src_x =
                    clamp_to_range((int)SDL_floorf(src_x_start + (u * src_x_span)), 0, src_surface->w - 1);
                const Uint32 src_pixel = src_row[src_x];
                const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
                if (src_a == 0u) {
                    continue;
                }
                if (src_a == 0xFFu) {
                    dst_row[x] = src_pixel;
                    continue;
                }
                dst_row[x] = blend_argb8888(dst_row[x], src_pixel);
            }
            continue;
        }

        for (int x = dst_x0; x < dst_x1; x++) {
            float u = (((float)x + 0.5f) - task->dst_rect.x) / task->dst_rect.w;
            u = SDL_max(0.0f, SDL_min(u, 0.999999f));
            if (flip_h) {
                u = 1.0f - u;
                u = SDL_max(0.0f, SDL_min(u, 0.999999f));
            }

            const int src_x =
                clamp_to_range((int)SDL_floorf(src_x_start + (u * src_x_span)), 0, src_surface->w - 1);
            Uint32 src_pixel = generic_blue_tint
                ? modulate_argb8888_blue_tint(src_row[src_x], generic_rg_factor, generic_mod_a)
                : modulate_argb8888(src_row[src_x], task->color);
            const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
            if (src_a == 0u) {
                continue;
            }
            if (src_a == 0xFFu) {
                dst_row[x] = src_pixel;
                continue;
            }
            dst_row[x] = blend_argb8888(dst_row[x], src_pixel);
        }
    }
    const Uint64 generic_sampled_ns = perf_capture_counter_delta_to_ns(
        generic_sample_start_counter, SDL_GetPerformanceCounter());
    note_perf_capture_raster_bucket_sample(
        SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED, task, generic_sampled_ns);
    if ((fast_copy_result != SOFTWARE_FRAME_FAST_COPY_RESULT_EXACT) &&
        (fast_copy_result != SOFTWARE_FRAME_FAST_COPY_RESULT_SCALED)) {
        note_software_frame_fast_copy_result(
            task, fast_copy_result, NULL, dst_surface, non_integer_lookup_entries, generic_sampled_ns);
    }

    if (src_locked) {
        SDL_UnlockSurface(src_surface);
    }
    return true;
}

static bool raster_textured_parallelogram_to_software_frame(const RenderTask* task) {
    if ((task == NULL) || (software_frame_surface == NULL) || (task->software_source_surface == NULL)) {
        return false;
    }

    SoftwareFrameTexturedParallelogram parallelogram;
    if (!try_resolve_geometry_task_as_software_frame_parallelogram(task, &parallelogram)) {
        return false;
    }

    SDL_Surface* src_surface = task->software_source_surface;
    bool src_locked = false;
    if (SDL_MUSTLOCK(src_surface)) {
        if (!SDL_LockSurface(src_surface)) {
            return false;
        }
        src_locked = true;
    }

    const Uint32* src_pixels = (const Uint32*)src_surface->pixels;
    Uint32* dst_pixels = (Uint32*)software_frame_surface->pixels;
    const int src_pitch = src_surface->pitch / (int)sizeof(Uint32);
    const int dst_pitch = software_frame_surface->pitch / (int)sizeof(Uint32);
    const int shear_dx_total = parallelogram.bottom_left_x - parallelogram.top_left_x;
    const Uint64* full_opaque_row_mask = get_software_surface_full_opaque_row_mask(task->texture_binding, src_surface);
    const Uint64 sample_start_counter =
        begin_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED);

    // The recovered Kyoto stage family is a same-size full-texture shear, so each visible row can
    // reuse the cached ARGB source row directly and only adjust the destination x offset.
    for (int row = 0; row < parallelogram.src_h; row++) {
        const int dst_y = parallelogram.top_y + row;
        if ((dst_y < 0) || (dst_y >= software_frame_surface->h)) {
            continue;
        }

        const float row_start_f = (float)parallelogram.top_left_x +
                                  ((((float)row + 0.5f) * (float)shear_dx_total) / (float)parallelogram.src_h);
        const int unclipped_dst_x0 = (int)SDL_roundf(row_start_f);
        const int unclipped_dst_x1 = unclipped_dst_x0 + parallelogram.src_w;
        const int dst_x0 = clamp_to_range(unclipped_dst_x0, 0, software_frame_surface->w);
        const int dst_x1 = clamp_to_range(unclipped_dst_x1, 0, software_frame_surface->w);
        if (dst_x1 <= dst_x0) {
            continue;
        }

        const int src_x0 = parallelogram.src_x + (dst_x0 - unclipped_dst_x0);
        const int visible_w = dst_x1 - dst_x0;
        const Uint32* src_row = src_pixels + ((parallelogram.src_y + row) * src_pitch) + src_x0;
        Uint32* dst_row = dst_pixels + (dst_y * dst_pitch) + dst_x0;
        if ((full_opaque_row_mask != NULL) &&
            (((full_opaque_row_mask[row >> 6] >> (row & 63)) & ((Uint64)1)) != 0)) {
            SDL_memcpy(dst_row, src_row, (size_t)visible_w * sizeof(Uint32));
            continue;
        }
        for (int col = 0; col < visible_w; col++) {
            const Uint32 src_pixel = src_row[col];
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

    note_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED,
                                           task,
                                           perf_capture_counter_delta_to_ns(
                                               sample_start_counter, SDL_GetPerformanceCounter()));
    if (src_locked) {
        SDL_UnlockSurface(src_surface);
    }
    return true;
}

static bool raster_textured_float_parallelogram_to_software_frame(const RenderTask* task) {
    if ((task == NULL) || (software_frame_surface == NULL) || (task->software_source_surface == NULL)) {
        return false;
    }

    SoftwareFrameTexturedFloatParallelogram parallelogram;
    if (!try_resolve_geometry_task_as_software_frame_float_parallelogram(task, &parallelogram)) {
        return false;
    }

    SDL_Surface* src_surface = task->software_source_surface;
    bool src_locked = false;
    if (SDL_MUSTLOCK(src_surface)) {
        if (!SDL_LockSurface(src_surface)) {
            return false;
        }
        src_locked = true;
    }

    const Uint32* src_pixels = (const Uint32*)src_surface->pixels;
    Uint32* dst_pixels = (Uint32*)software_frame_surface->pixels;
    const int src_pitch = src_surface->pitch / (int)sizeof(Uint32);
    const int dst_pitch = software_frame_surface->pitch / (int)sizeof(Uint32);
    const float height = parallelogram.bottom_y - parallelogram.top_y;
    const float left_dx = parallelogram.bottom_left_x - parallelogram.top_left_x;
    const float right_dx = parallelogram.bottom_right_x - parallelogram.top_right_x;
    const int start_y = clamp_to_range((int)SDL_ceilf(parallelogram.top_y - 0.5f), 0, software_frame_surface->h - 1);
    const int end_y =
        clamp_to_range((int)SDL_floorf(parallelogram.bottom_y - 0.5f), 0, software_frame_surface->h - 1);
    const Uint64 sample_start_counter =
        begin_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED);

    for (int dst_y = start_y; dst_y <= end_y; dst_y++) {
        const float py = (float)dst_y + 0.5f;
        float v = (py - parallelogram.top_y) / height;
        v = SDL_max(0.0f, SDL_min(v, 0.999999f));

        const float left_x = parallelogram.top_left_x + (v * left_dx);
        const float right_x = parallelogram.top_right_x + (v * right_dx);
        const float row_width = right_x - left_x;
        if (row_width <= rect_task_epsilon) {
            continue;
        }

        const int dst_x0 = clamp_to_range((int)SDL_ceilf(left_x - 0.5f), 0, software_frame_surface->w);
        const int dst_x1 = clamp_to_range((int)SDL_floorf(right_x - 0.5f) + 1, 0, software_frame_surface->w);
        if (dst_x1 <= dst_x0) {
            continue;
        }

        const int src_y = parallelogram.src_y +
                          clamp_to_range((int)SDL_floorf(v * (float)parallelogram.src_h), 0, parallelogram.src_h - 1);
        const Uint32* src_row = src_pixels + (src_y * src_pitch);
        Uint32* dst_row = dst_pixels + (dst_y * dst_pitch);
        const float src_step = (float)parallelogram.src_w / row_width;
        float src_x_f = ((((float)dst_x0 + 0.5f) - left_x) * src_step);
        for (int dst_x = dst_x0; dst_x < dst_x1; dst_x++) {
            const int src_x = parallelogram.src_x +
                              clamp_to_range((int)SDL_floorf(src_x_f), 0, parallelogram.src_w - 1);
            const Uint32 src_pixel = src_row[src_x];
            const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
            if (src_a == 0u) {
                src_x_f += src_step;
                continue;
            }
            if (src_a == 0xFFu) {
                dst_row[dst_x] = src_pixel;
            } else {
                dst_row[dst_x] = blend_argb8888(dst_row[dst_x], src_pixel);
            }
            src_x_f += src_step;
        }
    }

    note_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED,
                                           task,
                                           perf_capture_counter_delta_to_ns(
                                               sample_start_counter, SDL_GetPerformanceCounter()));
    if (src_locked) {
        SDL_UnlockSurface(src_surface);
    }
    return true;
}

static bool raster_textured_float_triangle_to_software_frame(const SDL_Surface* src_surface,
                                                             const SoftwareFrameTexturedFloatAffineQuad* quad,
                                                             int i0,
                                                             int i1,
                                                             int i2) {
    if ((src_surface == NULL) || (software_frame_surface == NULL) || (quad == NULL)) {
        return false;
    }

    const float x0 = quad->dst_x[i0];
    const float y0 = quad->dst_y[i0];
    const float x1 = quad->dst_x[i1];
    const float y1 = quad->dst_y[i1];
    const float x2 = quad->dst_x[i2];
    const float y2 = quad->dst_y[i2];
    const float area = ((x1 - x0) * (y2 - y0)) - ((y1 - y0) * (x2 - x0));
    if (SDL_fabsf(area) <= rect_task_epsilon) {
        return true;
    }

    const int min_x = clamp_to_range((int)SDL_floorf(SDL_min(SDL_min(x0, x1), x2)), 0, cps3_width - 1);
    const int max_x = clamp_to_range((int)SDL_ceilf(SDL_max(SDL_max(x0, x1), x2)) - 1, 0, cps3_width - 1);
    const int min_y = clamp_to_range((int)SDL_floorf(SDL_min(SDL_min(y0, y1), y2)), 0, cps3_height - 1);
    const int max_y = clamp_to_range((int)SDL_ceilf(SDL_max(SDL_max(y0, y1), y2)) - 1, 0, cps3_height - 1);
    if ((max_x < min_x) || (max_y < min_y)) {
        return true;
    }

    const float inv_area = 1.0f / area;
    const Uint32* src_pixels = (const Uint32*)src_surface->pixels;
    Uint32* dst_pixels = (Uint32*)software_frame_surface->pixels;
    const int src_pitch = src_surface->pitch / (int)sizeof(Uint32);
    const int dst_pitch = software_frame_surface->pitch / (int)sizeof(Uint32);
    for (int y = min_y; y <= max_y; y++) {
        Uint32* dst_row = dst_pixels + (y * dst_pitch);
        const float py = (float)y + 0.5f;
        for (int x = min_x; x <= max_x; x++) {
            const float px = (float)x + 0.5f;
            const float w0 = ((x1 - px) * (y2 - py)) - ((y1 - py) * (x2 - px));
            const float w1 = ((x2 - px) * (y0 - py)) - ((y2 - py) * (x0 - px));
            const float w2 = ((x0 - px) * (y1 - py)) - ((y0 - py) * (x1 - px));
            if (((area > 0.0f) && ((w0 < 0.0f) || (w1 < 0.0f) || (w2 < 0.0f))) ||
                ((area < 0.0f) && ((w0 > 0.0f) || (w1 > 0.0f) || (w2 > 0.0f)))) {
                continue;
            }

            const float alpha0 = w0 * inv_area;
            const float alpha1 = w1 * inv_area;
            const float alpha2 = w2 * inv_area;
            const int src_x = clamp_to_range((int)SDL_floorf((alpha0 * quad->src_u[i0]) + (alpha1 * quad->src_u[i1]) +
                                                             (alpha2 * quad->src_u[i2])),
                                             0,
                                             src_surface->w - 1);
            const int src_y = clamp_to_range((int)SDL_floorf((alpha0 * quad->src_v[i0]) + (alpha1 * quad->src_v[i1]) +
                                                             (alpha2 * quad->src_v[i2])),
                                             0,
                                             src_surface->h - 1);
            const Uint32 src_pixel = src_pixels[(src_y * src_pitch) + src_x];
            const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
            if (src_a == 0u) {
                continue;
            }
            if (src_a == 0xFFu) {
                dst_row[x] = src_pixel;
                continue;
            }
            dst_row[x] = blend_argb8888(dst_row[x], src_pixel);
        }
    }

    return true;
}

static bool raster_textured_triangle_to_software_frame(const SDL_Surface* src_surface,
                                                       const SoftwareFrameTexturedAffineQuad* quad,
                                                       int i0,
                                                       int i1,
                                                       int i2) {
    if ((src_surface == NULL) || (software_frame_surface == NULL) || (quad == NULL)) {
        return false;
    }

    const float x0 = (float)quad->dst_x[i0];
    const float y0 = (float)quad->dst_y[i0];
    const float x1 = (float)quad->dst_x[i1];
    const float y1 = (float)quad->dst_y[i1];
    const float x2 = (float)quad->dst_x[i2];
    const float y2 = (float)quad->dst_y[i2];
    const float area = ((x1 - x0) * (y2 - y0)) - ((y1 - y0) * (x2 - x0));
    if (SDL_fabsf(area) <= rect_task_epsilon) {
        return true;
    }

    const int min_x = clamp_to_range(SDL_min(SDL_min(quad->dst_x[i0], quad->dst_x[i1]), quad->dst_x[i2]), 0, cps3_width);
    const int max_x =
        clamp_to_range(SDL_max(SDL_max(quad->dst_x[i0], quad->dst_x[i1]), quad->dst_x[i2]), 0, cps3_width - 1);
    const int min_y = clamp_to_range(SDL_min(SDL_min(quad->dst_y[i0], quad->dst_y[i1]), quad->dst_y[i2]), 0, cps3_height);
    const int max_y =
        clamp_to_range(SDL_max(SDL_max(quad->dst_y[i0], quad->dst_y[i1]), quad->dst_y[i2]), 0, cps3_height - 1);
    if ((max_x < min_x) || (max_y < min_y)) {
        return true;
    }

    const float inv_area = 1.0f / area;
    const Uint32* src_pixels = (const Uint32*)src_surface->pixels;
    Uint32* dst_pixels = (Uint32*)software_frame_surface->pixels;
    const int src_pitch = src_surface->pitch / (int)sizeof(Uint32);
    const int dst_pitch = software_frame_surface->pitch / (int)sizeof(Uint32);
    for (int y = min_y; y <= max_y; y++) {
        Uint32* dst_row = dst_pixels + (y * dst_pitch);
        const float py = (float)y + 0.5f;
        for (int x = min_x; x <= max_x; x++) {
            const float px = (float)x + 0.5f;
            const float w0 = ((x1 - px) * (y2 - py)) - ((y1 - py) * (x2 - px));
            const float w1 = ((x2 - px) * (y0 - py)) - ((y2 - py) * (x0 - px));
            const float w2 = ((x0 - px) * (y1 - py)) - ((y0 - py) * (x1 - px));
            if (((area > 0.0f) && ((w0 < 0.0f) || (w1 < 0.0f) || (w2 < 0.0f))) ||
                ((area < 0.0f) && ((w0 > 0.0f) || (w1 > 0.0f) || (w2 > 0.0f)))) {
                continue;
            }

            const float alpha0 = w0 * inv_area;
            const float alpha1 = w1 * inv_area;
            const float alpha2 = w2 * inv_area;
            const int src_x = clamp_to_range((int)SDL_floorf((alpha0 * quad->src_u[i0]) + (alpha1 * quad->src_u[i1]) +
                                                             (alpha2 * quad->src_u[i2])),
                                             0,
                                             src_surface->w - 1);
            const int src_y = clamp_to_range((int)SDL_floorf((alpha0 * quad->src_v[i0]) + (alpha1 * quad->src_v[i1]) +
                                                             (alpha2 * quad->src_v[i2])),
                                             0,
                                             src_surface->h - 1);
            const Uint32 src_pixel = src_pixels[(src_y * src_pitch) + src_x];
            const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
            if (src_a == 0u) {
                continue;
            }
            if (src_a == 0xFFu) {
                dst_row[x] = src_pixel;
                continue;
            }
            dst_row[x] = blend_argb8888(dst_row[x], src_pixel);
        }
    }

    return true;
}

static bool raster_textured_translated_triangle_to_software_frame(const SDL_Surface* src_surface,
                                                                  const SoftwareFrameTexturedTranslatedQuad* quad,
                                                                  int i0,
                                                                  int i1,
                                                                  int i2) {
    if ((src_surface == NULL) || (software_frame_surface == NULL) || (quad == NULL)) {
        return false;
    }

    const int min_y = clamp_to_range(
        SDL_min(SDL_min(quad->dst_y[i0], quad->dst_y[i1]), quad->dst_y[i2]), 0, cps3_height - 1);
    const int max_y = clamp_to_range(
        SDL_max(SDL_max(quad->dst_y[i0], quad->dst_y[i1]), quad->dst_y[i2]), 0, cps3_height - 1);
    if (max_y < min_y) {
        return true;
    }

    const Uint32* src_pixels = (const Uint32*)src_surface->pixels;
    Uint32* dst_pixels = (Uint32*)software_frame_surface->pixels;
    const int src_pitch = src_surface->pitch / (int)sizeof(Uint32);
    const int dst_pitch = software_frame_surface->pitch / (int)sizeof(Uint32);
    const int indices[3] = { i0, i1, i2 };
    for (int y = min_y; y <= max_y; y++) {
        const float py = (float)y + 0.5f;
        float intersections[2] = { 0.0f, 0.0f };
        int intersection_count = 0;
        for (int edge_index = 0; edge_index < 3; edge_index++) {
            const int start = indices[edge_index];
            const int end = indices[(edge_index + 1) % 3];
            const float y0 = (float)quad->dst_y[start];
            const float y1 = (float)quad->dst_y[end];
            if (nearly_equal(y0, y1)) {
                continue;
            }
            const float edge_min_y = SDL_min(y0, y1);
            const float edge_max_y = SDL_max(y0, y1);
            if ((py < edge_min_y) || (py >= edge_max_y)) {
                continue;
            }
            const float t = (py - y0) / (y1 - y0);
            intersections[intersection_count++] =
                (float)quad->dst_x[start] + (t * (float)(quad->dst_x[end] - quad->dst_x[start]));
            if (intersection_count == 2) {
                break;
            }
        }
        if (intersection_count < 2) {
            continue;
        }
        if (intersections[0] > intersections[1]) {
            const float swap = intersections[0];
            intersections[0] = intersections[1];
            intersections[1] = swap;
        }

        const int dst_x0 = clamp_to_range((int)SDL_ceilf(intersections[0] - 0.5f), 0, cps3_width);
        const int dst_x1 = clamp_to_range((int)SDL_floorf(intersections[1] - 0.5f) + 1, 0, cps3_width);
        if (dst_x1 <= dst_x0) {
            continue;
        }

        const int src_y = y + quad->src_dy;
        if ((src_y < 0) || (src_y >= src_surface->h)) {
            continue;
        }
        const Uint32* src_row = src_pixels + (src_y * src_pitch);
        Uint32* dst_row = dst_pixels + (y * dst_pitch);
        for (int x = dst_x0; x < dst_x1; x++) {
            const int src_x = x + quad->src_dx;
            if ((src_x < 0) || (src_x >= src_surface->w)) {
                continue;
            }
            const Uint32 src_pixel = src_row[src_x];
            const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
            if (src_a == 0u) {
                continue;
            }
            if (src_a == 0xFFu) {
                dst_row[x] = src_pixel;
                continue;
            }
            dst_row[x] = blend_argb8888(dst_row[x], src_pixel);
        }
    }

    return true;
}

static bool raster_textured_translated_quad_to_software_frame(const RenderTask* task) {
    if ((task == NULL) || (software_frame_surface == NULL) || (task->software_source_surface == NULL)) {
        return false;
    }

    SoftwareFrameTexturedTranslatedQuad quad;
    if (!try_resolve_geometry_task_as_software_frame_translated_quad(task, &quad)) {
        return false;
    }

    SDL_Surface* src_surface = task->software_source_surface;
    bool src_locked = false;
    if (SDL_MUSTLOCK(src_surface)) {
        if (!SDL_LockSurface(src_surface)) {
            return false;
        }
        src_locked = true;
    }

    const Uint64 sample_start_counter =
        begin_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED);
    const bool ok = raster_textured_translated_triangle_to_software_frame(src_surface, &quad, 0, 1, 2) &&
                    raster_textured_translated_triangle_to_software_frame(src_surface, &quad, 1, 2, 3);
    note_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED,
                                           task,
                                           perf_capture_counter_delta_to_ns(
                                               sample_start_counter, SDL_GetPerformanceCounter()));
    if (src_locked) {
        SDL_UnlockSurface(src_surface);
    }
    return ok;
}

static bool raster_textured_full_texture_affine_quad_to_software_frame(const RenderTask* task) {
    if ((task == NULL) || (software_frame_surface == NULL) || (task->software_source_surface == NULL)) {
        return false;
    }

    SoftwareFrameTexturedFloatAffineQuad quad;
    if (!try_resolve_geometry_task_as_software_frame_full_texture_affine_quad(task, &quad)) {
        return false;
    }

    SDL_Surface* src_surface = task->software_source_surface;
    bool src_locked = false;
    if (SDL_MUSTLOCK(src_surface)) {
        if (!SDL_LockSurface(src_surface)) {
            return false;
        }
        src_locked = true;
    }

    const Uint64 sample_start_counter =
        begin_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED);
    const bool ok = raster_textured_float_triangle_to_software_frame(src_surface, &quad, 0, 1, 2) &&
                    raster_textured_float_triangle_to_software_frame(src_surface, &quad, 1, 2, 3);
    note_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED,
                                           task,
                                           perf_capture_counter_delta_to_ns(
                                               sample_start_counter, SDL_GetPerformanceCounter()));
    if (src_locked) {
        SDL_UnlockSurface(src_surface);
    }
    return ok;
}

static bool raster_textured_affine_quad_to_software_frame(const RenderTask* task) {
    if ((task == NULL) || (software_frame_surface == NULL) || (task->software_source_surface == NULL)) {
        return false;
    }

    SoftwareFrameTexturedAffineQuad quad;
    if (!try_resolve_geometry_task_as_software_frame_affine_quad(task, &quad)) {
        return false;
    }

    SDL_Surface* src_surface = task->software_source_surface;
    bool src_locked = false;
    if (SDL_MUSTLOCK(src_surface)) {
        if (!SDL_LockSurface(src_surface)) {
            return false;
        }
        src_locked = true;
    }

    const Uint64 sample_start_counter =
        begin_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED);
    const bool ok = raster_textured_triangle_to_software_frame(src_surface, &quad, 0, 1, 2) &&
                    raster_textured_triangle_to_software_frame(src_surface, &quad, 1, 2, 3);
    note_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED,
                                           task,
                                           perf_capture_counter_delta_to_ns(
                                               sample_start_counter, SDL_GetPerformanceCounter()));
    if (src_locked) {
        SDL_UnlockSurface(src_surface);
    }
    return ok;
}

static bool raster_solid_task_to_software_frame(const RenderTask* task) {
    if ((task == NULL) || (software_frame_surface == NULL)) {
        return false;
    }

    SoftwareFrameSolidDiagonalStrip diagonal_strip;
    if (try_resolve_solid_task_as_full_height_diagonal_strip(task, &diagonal_strip)) {
        const Uint64 sample_start_counter =
            begin_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_SOLID);
        const bool ok = raster_full_height_diagonal_strip_to_software_frame(&diagonal_strip, software_frame_surface);
        note_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_SOLID,
                                               task,
                                               perf_capture_counter_delta_to_ns(
                                                   sample_start_counter, SDL_GetPerformanceCounter()));
        return ok;
    }

    if ((task->dst_rect.w <= 0.0f) || (task->dst_rect.h <= 0.0f)) {
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

    const Uint64 sample_start_counter =
        begin_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_SOLID);
    const int fill_width = dst_x1 - dst_x0;
    if (src_a == 255u) {
        for (int y = dst_y0; y < dst_y1; y++) {
            Uint32* dst_row = dst_pixels + (y * dst_pitch) + dst_x0;
            fill_argb8888_span(dst_row, fill_width, task->color);
        }
        note_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_SOLID,
                                               task,
                                               perf_capture_counter_delta_to_ns(
                                                   sample_start_counter, SDL_GetPerformanceCounter()));
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
    note_perf_capture_raster_bucket_sample(SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_SOLID,
                                           task,
                                           perf_capture_counter_delta_to_ns(sample_start_counter, SDL_GetPerformanceCounter()));
    return true;
}

static bool render_frame_to_software_surface(void) {
    if ((software_frame_surface == NULL) || !software_frame_surface_ready) {
        return false;
    }

    bool frame_supported = true;
    for (int i = 0; i < render_task_count; i++) {
        const RenderTask* task = &render_tasks[i];

        // Fast path: TEXTURED_RECT tasks need no geometry resolution — classify directly
        // from the original task and copy once to the resolved array, avoiding the
        // intermediate resolved_task struct copy (~200 bytes per task).
        if (task->type == RENDER_TASK_TYPE_TEXTURED_RECT) {
            const SoftwareFrameFallbackReason reason = classify_software_frame_fallback_reason(task);
            note_software_frame_eligibility(task, reason);
            software_frame_resolved_tasks[i] = *task;
            if (reason != SOFTWARE_FRAME_FALLBACK_REASON_NONE) {
                frame_supported = false;
            }
            continue;
        }

        RenderTask resolved_task = *task;

        if ((task->type == RENDER_TASK_TYPE_GEOMETRY) && (task->texture != NULL)) {
            RenderTask rect_task;
            if (try_resolve_geometry_task_as_rect_copy(task, &rect_task)) {
                resolved_task = rect_task;
            } else if (!try_resolve_geometry_task_as_software_frame_parallelogram(task, NULL) &&
                       !try_resolve_geometry_task_as_software_frame_float_parallelogram(task, NULL) &&
                       !try_resolve_geometry_task_as_software_frame_full_texture_affine_quad(task, NULL) &&
                       !try_resolve_geometry_task_as_software_frame_translated_quad(task, NULL) &&
                       !try_resolve_geometry_task_as_software_frame_affine_quad(task, NULL)) {
                note_software_frame_eligibility(task, SOFTWARE_FRAME_FALLBACK_REASON_GEOMETRY);
                frame_supported = false;
                continue;
            }
        } else if ((task->type == RENDER_TASK_TYPE_GEOMETRY) && (task->texture == NULL)) {
            SDL_FRect solid_rect;
            Uint32 solid_color = 0;
            if (try_resolve_solid_task_as_rect(task, &solid_rect, &solid_color)) {
                resolved_task.dst_rect = solid_rect;
                resolved_task.color = solid_color;
            } else {
                SoftwareFrameSolidDiagonalStrip diagonal_strip;
                if (!try_resolve_solid_task_as_full_height_diagonal_strip(task, &diagonal_strip)) {
                    note_software_frame_eligibility(task, SOFTWARE_FRAME_FALLBACK_REASON_SOLID);
                    frame_supported = false;
                    continue;
                }
                resolved_task.color = diagonal_strip.color;
            }
        }

        const SoftwareFrameFallbackReason reason = classify_software_frame_fallback_reason(&resolved_task);
        note_software_frame_eligibility(&resolved_task, reason);
        if ((reason == SOFTWARE_FRAME_FALLBACK_REASON_NONE) && (task->type == RENDER_TASK_TYPE_GEOMETRY) &&
            (task->texture != NULL) && (resolved_task.type == RENDER_TASK_TYPE_GEOMETRY)) {
#if ENABLE_PERF_TELEMETRY
            note_perf_capture_textured_geometry_recovered_family(task);
#endif
        }
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

    for (int i = 0; i < render_task_count;) {
        RenderTask merged_task = software_frame_resolved_tasks[i];
        SoftwareFrameRectStripMergeAxis merge_axis = SOFTWARE_FRAME_RECT_STRIP_MERGE_AXIS_NONE;
        int next_task_index = i + 1;

        if ((merged_task.type == RENDER_TASK_TYPE_TEXTURED_RECT) && (merged_task.texture != NULL)) {
            while ((next_task_index < render_task_count) &&
                   try_merge_software_frame_rect_tasks(
                       &merged_task, &software_frame_resolved_tasks[next_task_index], software_frame_surface, &merge_axis)) {
                next_task_index += 1;
            }
        }

        const RenderTask* task = &merged_task;
        const bool ok = (task->type == RENDER_TASK_TYPE_TEXTURED_RECT)
                            ? raster_textured_task_to_software_frame(task)
                            : ((task->texture != NULL)
                                   ? (raster_textured_parallelogram_to_software_frame(task) ||
                                      raster_textured_float_parallelogram_to_software_frame(task) ||
                                      raster_textured_full_texture_affine_quad_to_software_frame(task) ||
                                      raster_textured_translated_quad_to_software_frame(task) ||
                                      raster_textured_affine_quad_to_software_frame(task))
                                   : raster_solid_task_to_software_frame(task));
        if (!ok) {
            if (dst_locked) {
                SDL_UnlockSurface(software_frame_surface);
            }
            return false;
        }

        i = next_task_index;

#if defined(PORT_MISTER)
        /* Mid-render background snapshot: after rendering the last background
           task (index < split point) and before the first character task,
           save the surface.  This captures background-only pixels so skip
           frames can restore them without stale character positions. */
        if ((sa_bg_cache_snapshot_at_index >= 0) &&
            (i >= sa_bg_cache_snapshot_at_index) &&
            !sa_bg_cache_surface_valid &&
            ensure_sa_bg_cache_surface()) {
            if (SDL_LockSurface(sa_bg_cache_surface)) {
                SDL_memcpy(sa_bg_cache_surface->pixels,
                           software_frame_surface->pixels,
                           (size_t)software_frame_surface->pitch * software_frame_surface->h);
                SDL_UnlockSurface(sa_bg_cache_surface);
                sa_bg_cache_surface_valid = true;
            }
            sa_bg_cache_snapshot_at_index = -1;
        }
#endif
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
#endif

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

static bool palette_colors_equal(const SDL_Palette* palette, const SDL_Color* colors, int color_count) {
    if ((palette == NULL) || (colors == NULL) || (color_count <= 0) || (palette->colors == NULL)) {
        return false;
    }

    if (palette->ncolors != color_count) {
        return false;
    }

    return SDL_memcmp(palette->colors, colors, (size_t)color_count * sizeof(colors[0])) == 0;
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

void SDLGameRenderer_SetSuperEffectQualityMode(SDLGameRenderer_SuperEffectQualityMode mode) {
    super_effect_quality_mode = mode;
}

void SDLGameRenderer_SetGhostResolutionMode(SDLGameRenderer_GhostResolutionMode mode) {
    ghost_resolution_mode = mode;
}

void SDLGameRenderer_SetSABgCacheFramesRemaining(int frames_remaining) {
    sa_bg_cache_frames_remaining = frames_remaining > 0 ? frames_remaining : 0;
#if defined(PORT_MISTER)
    if (sa_bg_cache_frames_remaining <= 0) {
        sa_bg_cache_surface_valid = false;
    }
#endif
}

void SDLGameRenderer_InvalidateSABgCache(void) {
#if defined(PORT_MISTER)
    sa_bg_cache_surface_valid = false;
#endif
}


bool SDLGameRenderer_IsSoftwareFrameModeEnabled(void) {
    return software_frame_mode_active;
}

bool SDLGameRenderer_IsPerfCaptureExtendedStatsEnabled(void) {
    return frame_stats_extended_enabled;
}

void SDLGameRenderer_SetPerfCaptureLogicalIdentityEnabled(bool enabled) {
    perf_capture_logical_identity_enabled = enabled;
}

void SDLGameRenderer_SetPerfCaptureBasicFirstWindowFamilySnapshotsEnabled(bool enabled) {
    perf_capture_basic_first_window_family_snapshots_enabled = enabled;
}

void SDLGameRenderer_SetPerfCaptureBasicFirstWindowRenderSubphasesEnabled(bool enabled) {
    perf_capture_basic_first_window_render_subphases_enabled = enabled;
}

void SDLGameRenderer_SetPerfCaptureBasicFirstWindowExactHotFamilyAlphaOffpathEnabled(bool enabled) {
    perf_capture_basic_first_window_exact_hot_family_alpha_offpath_enabled = enabled;
}

void SDLGameRenderer_SetPerfCaptureBasicFirstWindowOnsetExactHotFamilyAlphaOffpathEnabled(bool enabled) {
    perf_capture_basic_first_window_onset_exact_hot_family_alpha_offpath_enabled = enabled;
}

void SDLGameRenderer_SetPerfCaptureBasicFirstWindowOnsetClusterAlphaOffpathEnabled(bool enabled) {
    perf_capture_basic_first_window_onset_cluster_alpha_offpath_enabled = enabled;
}

void SDLGameRenderer_SetPerfCaptureFastNonIntegerReuseTelemetryEnabled(bool enabled) {
    perf_capture_fast_non_integer_reuse_telemetry_enabled = enabled;
}

void SDLGameRenderer_SetPerfCaptureFastNonIntegerSubrectAlphaTelemetryEnabled(bool enabled) {
    perf_capture_fast_non_integer_subrect_alpha_telemetry_enabled = enabled;
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

    apply_super_effect_burst_reduction_after_sort();

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
    perf_capture_refresh_telemetry.sampled_blit_period = software_surface_refresh_blit_sample_period;
    SDL_zero(perf_capture_refresh_attempts_by_texture);
    SDL_zero(perf_capture_refresh_pixels_by_texture);
    SDL_zero(perf_capture_refresh_fanout_max_by_texture);
    SDL_zero(perf_capture_refresh_partial_attempts_by_texture);
    SDL_zero(perf_capture_refresh_partial_pixels_by_texture);
    SDL_zero(perf_capture_refresh_full_attempts_by_texture);
    SDL_zero(perf_capture_refresh_full_pixels_by_texture);
    SDL_zero(perf_capture_refresh_full_non_texture_dirty_attempts_by_texture);
    SDL_zero(perf_capture_refresh_full_ineligible_source_attempts_by_texture);
    SDL_zero(perf_capture_refresh_full_no_usable_dirty_rect_attempts_by_texture);
    SDL_zero(perf_capture_refresh_full_oversized_dirty_rect_attempts_by_texture);
    perf_capture_refresh_blit_sample_counter = 0;
    SDL_zero(perf_capture_refresh_blit_sample_calls_by_texture);
    SDL_zero(perf_capture_refresh_blit_sample_ns_by_texture);
    SDL_zero(perf_capture_refresh_full_blit_sample_calls_by_texture);
    SDL_zero(perf_capture_refresh_full_blit_sample_ns_by_texture);
    SDL_zero(perf_capture_refresh_partial_blit_sample_calls_by_texture);
    SDL_zero(perf_capture_refresh_partial_blit_sample_ns_by_texture);
    SDL_zero(perf_capture_refresh_full_non_texture_dirty_blit_sample_calls_by_texture);
    SDL_zero(perf_capture_refresh_full_non_texture_dirty_blit_sample_ns_by_texture);
    SDL_zero(perf_capture_refresh_full_ineligible_source_blit_sample_calls_by_texture);
    SDL_zero(perf_capture_refresh_full_ineligible_source_blit_sample_ns_by_texture);
    SDL_zero(perf_capture_refresh_full_no_usable_dirty_rect_blit_sample_calls_by_texture);
    SDL_zero(perf_capture_refresh_full_no_usable_dirty_rect_blit_sample_ns_by_texture);
    SDL_zero(perf_capture_refresh_full_oversized_dirty_rect_blit_sample_calls_by_texture);
    SDL_zero(perf_capture_refresh_full_oversized_dirty_rect_blit_sample_ns_by_texture);
    SDL_zero(perf_capture_refresh_source_format_by_texture);
    SDL_zero(perf_capture_refresh_width_by_texture);
    SDL_zero(perf_capture_refresh_height_by_texture);
    SDL_zero(perf_capture_refresh_shape_mixed_by_texture);
    SDL_zero(perf_capture_source_surface_destroy_calls_by_texture);
    SDL_zero(perf_capture_software_surface_access_dirty_texture_same_frame_by_texture);
    SDL_zero(perf_capture_software_surface_access_dirty_texture_carried_by_texture);
    SDL_zero(perf_capture_software_surface_access_dirty_palette_same_frame_by_texture);
    SDL_zero(perf_capture_software_surface_access_dirty_palette_carried_by_texture);
    SDL_zero(perf_capture_software_surface_access_dirty_palette_changed_same_frame_by_texture);
    SDL_zero(perf_capture_software_surface_access_dirty_palette_changed_carried_by_texture);
    SDL_zero(perf_capture_software_surface_access_dirty_palette_unchanged_same_frame_by_texture);
    SDL_zero(perf_capture_software_surface_access_dirty_palette_unchanged_carried_by_texture);
    SDL_zero(perf_capture_software_surface_access_cold_by_texture);
    SDL_zero(perf_capture_current_lifetime_refresh_attempts_by_texture);
    SDL_zero(perf_capture_current_lifetime_partial_refresh_attempts_by_texture);
    SDL_zero(perf_capture_current_lifetime_full_refresh_attempts_by_texture);
    SDL_zero(perf_capture_current_lifetime_full_no_usable_dirty_rect_attempts_by_texture);
    SDL_zero(perf_capture_current_lifetime_software_surface_access_dirty_texture_same_frame_by_texture);
    SDL_zero(perf_capture_current_lifetime_software_surface_access_dirty_texture_carried_by_texture);
    SDL_zero(perf_capture_current_lifetime_software_surface_access_dirty_palette_same_frame_by_texture);
    SDL_zero(perf_capture_current_lifetime_software_surface_access_dirty_palette_carried_by_texture);
    SDL_zero(perf_capture_current_lifetime_software_surface_access_dirty_palette_changed_same_frame_by_texture);
    SDL_zero(perf_capture_current_lifetime_software_surface_access_dirty_palette_changed_carried_by_texture);
    SDL_zero(perf_capture_current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame_by_texture);
    SDL_zero(perf_capture_current_lifetime_software_surface_access_dirty_palette_unchanged_carried_by_texture);
    SDL_zero(perf_capture_current_lifetime_software_surface_access_cold_by_texture);
    for (int texture_index = 0; texture_index < FL_TEXTURE_MAX; texture_index++) {
        reset_perf_capture_texture_logical_identity_slot(texture_index);
    }
}

void SDLGameRenderer_ResetPerfCaptureUnlockLocalityTelemetry(void) {
    SDL_zero(perf_capture_unlock_locality_telemetry);
    SDL_zero(perf_capture_unlock_locality_tracked_by_texture);
    SDL_zero(perf_capture_unlock_locality_zero_delta_by_texture);
    SDL_zero(perf_capture_unlock_locality_baseline_skips_by_texture);
    SDL_zero(perf_capture_unlock_locality_non_index8_skips_by_texture);
    SDL_zero(perf_capture_unlock_locality_source_pixels_by_texture);
    SDL_zero(perf_capture_unlock_locality_changed_pixels_by_texture);
    SDL_zero(perf_capture_unlock_locality_changed_rows_by_texture);
    SDL_zero(perf_capture_unlock_locality_changed_bbox_pixels_by_texture);
    SDL_zero(perf_capture_unlock_locality_whole_capture_tracked_by_texture);
    SDL_zero(perf_capture_unlock_locality_whole_capture_zero_delta_by_texture);
    SDL_zero(perf_capture_unlock_locality_whole_capture_baseline_skips_by_texture);
    SDL_zero(perf_capture_unlock_locality_whole_capture_non_index8_skips_by_texture);
    SDL_zero(perf_capture_unlock_locality_whole_capture_source_pixels_by_texture);
    SDL_zero(perf_capture_unlock_locality_whole_capture_changed_pixels_by_texture);
    SDL_zero(perf_capture_unlock_locality_whole_capture_changed_rows_by_texture);
    SDL_zero(perf_capture_unlock_locality_whole_capture_changed_bbox_pixels_by_texture);
    SDL_zero(perf_capture_unlock_locality_source_format_by_texture);
    SDL_zero(perf_capture_unlock_locality_width_by_texture);
    SDL_zero(perf_capture_unlock_locality_height_by_texture);
    SDL_zero(perf_capture_unlock_locality_shape_mixed_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_pending_rects);
    SDL_zero(perf_capture_compare_dirty_rect_pending_valid);
    SDL_zero(perf_capture_compare_dirty_rect_pending_unlock_counts);
    SDL_zero(perf_capture_compare_dirty_rect_pending_tile_masks);
    SDL_zero(perf_capture_compare_dirty_row_mask_pending_tile_masks);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_attempts_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_partial_candidate_refresh_attempts_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_oversized_candidate_refresh_attempts_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_no_usable_candidate_refresh_attempts_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_bbox_pixels_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_max_bbox_pixels_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_pending_unlocks_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_max_pending_unlocks_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_32x32_covered_tiles_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_32x32_max_covered_tiles_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_32x32_component_count_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_32x32_max_component_count_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_32x32_multi_component_refresh_attempts_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_32x32_largest_component_tiles_by_texture);
    SDL_zero(perf_capture_compare_dirty_rect_refresh_32x32_max_largest_component_tiles_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_no_usable_candidate_refresh_attempts_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_partial_candidate_refresh_attempts_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_half_cap_partial_candidate_refresh_attempts_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_plan_pixels_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_max_plan_pixels_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_32x32_covered_tiles_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_32x32_max_covered_tiles_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_32x32_component_count_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_32x32_max_component_count_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_32x32_multi_component_refresh_attempts_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_32x32_largest_component_tiles_by_texture);
    SDL_zero(perf_capture_compare_dirty_row_mask_32x32_max_largest_component_tiles_by_texture);
    for (int texture_index = 0; texture_index < FL_TEXTURE_MAX; texture_index++) {
        reset_perf_capture_unlock_locality_texture_slot(texture_index);
    }
}

void SDLGameRenderer_ResetPerfCaptureTextureRenewTelemetry(void) {
    for (int texture_index = 0; texture_index < FL_TEXTURE_MAX; texture_index++) {
        reset_perf_capture_texture_renew_slot(texture_index);
    }
}

void SDLGameRenderer_ResetPerfCaptureRasterTimingTelemetry(void) {
    SDL_zero(perf_capture_raster_sample_counters);
    SDL_zero(perf_capture_raster_sample_calls);
    SDL_zero(perf_capture_raster_sample_pixels);
    SDL_zero(perf_capture_raster_sample_ns);
    SDL_zero(perf_capture_fast_non_integer_phase_totals);
}

void SDLGameRenderer_ResetPerfCaptureFastExactFamilyTelemetry(void) {
    SDL_zero(perf_capture_fast_exact_families);
    perf_capture_fast_exact_family_count = 0;
}

void SDLGameRenderer_ResetPerfCaptureFastNonIntegerFamilyTelemetry(void) {
    SDL_zero(perf_capture_fast_non_integer_families);
    perf_capture_fast_non_integer_family_count = 0;
    SDL_zero(perf_capture_fast_non_integer_shapes);
    perf_capture_fast_non_integer_shape_count = 0;
    SDL_zero(perf_capture_fast_non_integer_lookup_patterns);
    perf_capture_fast_non_integer_lookup_pattern_count = 0;
    SDL_zero(perf_capture_basic_first_window_alpha_offpath_shapes);
    perf_capture_basic_first_window_alpha_offpath_shape_count = 0;
    SDL_zero(perf_capture_sa_burst_effect_samples);
    perf_capture_sa_burst_effect_sample_count = 0;
}

void SDLGameRenderer_ResetPerfCaptureGenericTexturedFamilyTelemetry(void) {
    SDL_zero(perf_capture_generic_textured_families);
    perf_capture_generic_textured_family_count = 0;
}

static int get_perf_capture_textured_geometry_families(
    const SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* families,
    int family_count,
    SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* out_families,
    int max_families) {
    if ((out_families == NULL) || (max_families <= 0) || (families == NULL) || (family_count <= 0)) {
        return 0;
    }

    int selected_count = 0;
    for (int slot = 0; slot < max_families; slot++) {
        int best_index = -1;
        for (int i = 0; i < family_count; i++) {
            const SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* candidate = &families[i];
            if (candidate->task_count == 0) {
                continue;
            }

            bool already_selected = false;
            for (int existing = 0; existing < selected_count; existing++) {
                if (out_families[existing].texture_handle == candidate->texture_handle &&
                    out_families[existing].palette_handle == candidate->palette_handle &&
                    out_families[existing].family_kind == candidate->family_kind &&
                    out_families[existing].uniform_color == candidate->uniform_color &&
                    out_families[existing].opaque_color == candidate->opaque_color &&
                    out_families[existing].rgb_mod == candidate->rgb_mod &&
                    out_families[existing].integer_positions == candidate->integer_positions &&
                    out_families[existing].integer_source_rect == candidate->integer_source_rect &&
                    out_families[existing].full_texture_source_rect == candidate->full_texture_source_rect &&
                    out_families[existing].source_width == candidate->source_width &&
                    out_families[existing].source_height == candidate->source_height &&
                    out_families[existing].source_format == candidate->source_format) {
                    already_selected = true;
                    break;
                }
            }
            if (already_selected) {
                continue;
            }

            if (best_index < 0) {
                best_index = i;
                continue;
            }

            const SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* best = &families[best_index];
            if ((candidate->task_count > best->task_count) ||
                ((candidate->task_count == best->task_count) && (candidate->submitted_pixels > best->submitted_pixels)) ||
                ((candidate->task_count == best->task_count) && (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->texture_handle < best->texture_handle))) {
                best_index = i;
            }
        }

        if (best_index < 0) {
            break;
        }

        SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* entry = &out_families[selected_count];
        *entry = families[best_index];
        const int texture_index = entry->texture_handle - 1;
        if ((texture_index >= 0) && (texture_index < FL_TEXTURE_MAX)) {
            copy_perf_capture_texture_logical_identity(texture_index,
                                                       &entry->logical_identity_known,
                                                       &entry->logical_identity_mixed,
                                                       &entry->logical_identity_registrations,
                                                       &entry->logical_source_kind,
                                                       &entry->logical_ix_num,
                                                       &entry->logical_ix_num_first,
                                                       &entry->logical_slot_index,
                                                       &entry->logical_chunk_index,
                                                       &entry->logical_texture_total);
        }
        selected_count += 1;
    }

    return selected_count;
}

static void get_perf_capture_textured_geometry_family_totals(
    const SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* families,
    int family_count,
    Uint64* out_task_total,
    Uint64* out_pixel_total,
    int* out_family_count) {
    Uint64 task_total = 0;
    Uint64 pixel_total = 0;
    for (int i = 0; i < family_count; i++) {
        task_total += families[i].task_count;
        pixel_total += families[i].submitted_pixels;
    }

    if (out_task_total != NULL) {
        *out_task_total = task_total;
    }
    if (out_pixel_total != NULL) {
        *out_pixel_total = pixel_total;
    }
    if (out_family_count != NULL) {
        *out_family_count = family_count;
    }
}

void SDLGameRenderer_ResetPerfCaptureTexturedGeometryRecoveredTelemetry(void) {
    SDL_zero(perf_capture_textured_geometry_recovered_families);
    perf_capture_textured_geometry_recovered_family_count = 0;
}

void SDLGameRenderer_ResetPerfCaptureTexturedGeometryFallbackTelemetry(void) {
    SDL_zero(perf_capture_textured_geometry_fallback_families);
    perf_capture_textured_geometry_fallback_family_count = 0;
}

int SDLGameRenderer_GetPerfCaptureTexturedGeometryRecoveredFamilies(
    SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* out_families,
    int max_families) {
    return get_perf_capture_textured_geometry_families(perf_capture_textured_geometry_recovered_families,
                                                       perf_capture_textured_geometry_recovered_family_count,
                                                       out_families,
                                                       max_families);
}

int SDLGameRenderer_GetPerfCaptureTexturedGeometryFallbackFamilies(
    SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* out_families,
    int max_families) {
    return get_perf_capture_textured_geometry_families(perf_capture_textured_geometry_fallback_families,
                                                       perf_capture_textured_geometry_fallback_family_count,
                                                       out_families,
                                                       max_families);
}

void SDLGameRenderer_GetPerfCaptureTexturedGeometryRecoveredTotals(Uint64* out_task_total,
                                                                   Uint64* out_pixel_total,
                                                                   int* out_family_count) {
    get_perf_capture_textured_geometry_family_totals(perf_capture_textured_geometry_recovered_families,
                                                     perf_capture_textured_geometry_recovered_family_count,
                                                     out_task_total,
                                                     out_pixel_total,
                                                     out_family_count);
}

void SDLGameRenderer_GetPerfCaptureTexturedGeometryFallbackTotals(Uint64* out_task_total,
                                                                  Uint64* out_pixel_total,
                                                                  int* out_family_count) {
    get_perf_capture_textured_geometry_family_totals(perf_capture_textured_geometry_fallback_families,
                                                     perf_capture_textured_geometry_fallback_family_count,
                                                     out_task_total,
                                                     out_pixel_total,
                                                     out_family_count);
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
        copy_perf_capture_texture_logical_identity(best_texture_index,
                                                   &entry->logical_identity_known,
                                                   &entry->logical_identity_mixed,
                                                   &entry->logical_identity_registrations,
                                                   &entry->logical_source_kind,
                                                   &entry->logical_ix_num,
                                                   &entry->logical_ix_num_first,
                                                   &entry->logical_slot_index,
                                                   &entry->logical_chunk_index,
                                                   &entry->logical_texture_total);
        entry->current_lifetime_refresh_attempts =
            perf_capture_current_lifetime_refresh_attempts_by_texture[best_texture_index];
        entry->current_lifetime_partial_refresh_attempts =
            perf_capture_current_lifetime_partial_refresh_attempts_by_texture[best_texture_index];
        entry->current_lifetime_full_refresh_attempts =
            perf_capture_current_lifetime_full_refresh_attempts_by_texture[best_texture_index];
        entry->current_lifetime_full_refresh_no_usable_dirty_rect_attempts =
            perf_capture_current_lifetime_full_no_usable_dirty_rect_attempts_by_texture[best_texture_index];
        entry->refresh_attempts = perf_capture_refresh_attempts_by_texture[best_texture_index];
        entry->refresh_pixels = perf_capture_refresh_pixels_by_texture[best_texture_index];
        entry->max_fanout = (int)perf_capture_refresh_fanout_max_by_texture[best_texture_index];
        entry->source_surface_destroy_calls = perf_capture_source_surface_destroy_calls_by_texture[best_texture_index];
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
        copy_perf_capture_texture_logical_identity(best_texture_index,
                                                   &entry->logical_identity_known,
                                                   &entry->logical_identity_mixed,
                                                   &entry->logical_identity_registrations,
                                                   &entry->logical_source_kind,
                                                   &entry->logical_ix_num,
                                                   &entry->logical_ix_num_first,
                                                   &entry->logical_slot_index,
                                                   &entry->logical_chunk_index,
                                                   &entry->logical_texture_total);
        entry->tracked_unlocks = perf_capture_unlock_locality_tracked_by_texture[best_texture_index];
        entry->zero_delta_unlocks = perf_capture_unlock_locality_zero_delta_by_texture[best_texture_index];
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

void SDLGameRenderer_GetPerfCaptureDirtyRectLifetimeTelemetry(
    SDLGameRenderer_PerfCaptureDirtyRectLifetimeTelemetry* out_telemetry) {
    if (out_telemetry == NULL) {
        return;
    }

    SDL_zero(*out_telemetry);
    for (int texture_index = 0; texture_index < FL_TEXTURE_MAX; texture_index++) {
        out_telemetry->record_calls += perf_capture_dirty_rect_record_calls_by_texture[texture_index];
        out_telemetry->retained_after_unlock += perf_capture_dirty_rect_retained_after_unlock_by_texture[texture_index];
        out_telemetry->clear_stale_before_record +=
            perf_capture_dirty_rect_clear_stale_before_record_by_texture[texture_index];
        out_telemetry->clear_unlock_unused += perf_capture_dirty_rect_clear_unlock_unused_by_texture[texture_index];
        out_telemetry->clear_access_unused += perf_capture_dirty_rect_clear_access_unused_by_texture[texture_index];
        out_telemetry->clear_explicit += perf_capture_dirty_rect_clear_explicit_by_texture[texture_index];
    }
}

void SDLGameRenderer_GetPerfCaptureTextureRenewTelemetry(
    SDLGameRenderer_PerfCaptureTextureRenewTelemetry* out_telemetry) {
    if (out_telemetry == NULL) {
        return;
    }

    SDL_zero(*out_telemetry);
    for (int texture_index = 0; texture_index < FL_TEXTURE_MAX; texture_index++) {
        out_telemetry->renew_chunk_calls += perf_capture_texture_renew_chunk_calls_by_texture[texture_index];
        out_telemetry->renew_batches += perf_capture_texture_renew_batches_by_texture[texture_index];
        out_telemetry->renew_batches_without_rect += perf_capture_texture_renew_batches_without_rect_by_texture[texture_index];
        out_telemetry->renew_chunk_pixels += perf_capture_texture_renew_chunk_pixels_by_texture[texture_index];
        out_telemetry->renew_batch_bbox_pixels += perf_capture_texture_renew_batch_bbox_pixels_by_texture[texture_index];
        if (perf_capture_texture_renew_batch_max_bbox_pixels_by_texture[texture_index] >
            out_telemetry->renew_batch_max_bbox_pixels) {
            out_telemetry->renew_batch_max_bbox_pixels =
                perf_capture_texture_renew_batch_max_bbox_pixels_by_texture[texture_index];
        }
        out_telemetry->renew_chunk_8x8_calls += perf_capture_texture_renew_chunk_8x8_calls_by_texture[texture_index];
        out_telemetry->renew_chunk_16x16_calls += perf_capture_texture_renew_chunk_16x16_calls_by_texture[texture_index];
        out_telemetry->renew_chunk_32x32_calls += perf_capture_texture_renew_chunk_32x32_calls_by_texture[texture_index];
        out_telemetry->renew_batch_32x32_covered_tiles +=
            perf_capture_texture_renew_batch_32x32_covered_tiles_by_texture[texture_index];
        if (perf_capture_texture_renew_batch_32x32_max_covered_tiles_by_texture[texture_index] >
            out_telemetry->renew_batch_32x32_max_covered_tiles) {
            out_telemetry->renew_batch_32x32_max_covered_tiles =
                perf_capture_texture_renew_batch_32x32_max_covered_tiles_by_texture[texture_index];
        }
        out_telemetry->renew_batch_32x32_component_count +=
            perf_capture_texture_renew_batch_32x32_component_count_by_texture[texture_index];
        if (perf_capture_texture_renew_batch_32x32_max_component_count_by_texture[texture_index] >
            out_telemetry->renew_batch_32x32_max_component_count) {
            out_telemetry->renew_batch_32x32_max_component_count =
                perf_capture_texture_renew_batch_32x32_max_component_count_by_texture[texture_index];
        }
        out_telemetry->renew_batch_32x32_multi_component_batches +=
            perf_capture_texture_renew_batch_32x32_multi_component_batches_by_texture[texture_index];
        out_telemetry->renew_batch_32x32_largest_component_tiles +=
            perf_capture_texture_renew_batch_32x32_largest_component_tiles_by_texture[texture_index];
        if (perf_capture_texture_renew_batch_32x32_max_largest_component_tiles_by_texture[texture_index] >
            out_telemetry->renew_batch_32x32_max_largest_component_tiles) {
            out_telemetry->renew_batch_32x32_max_largest_component_tiles =
                perf_capture_texture_renew_batch_32x32_max_largest_component_tiles_by_texture[texture_index];
        }
    }
}

int SDLGameRenderer_GetPerfCaptureRasterBucketTimings(SDLGameRenderer_PerfCaptureRasterBucketTiming* out_timings,
                                                      int max_timings) {
    if ((out_timings == NULL) || (max_timings <= 0)) {
        return 0;
    }

    const int timing_count = SDL_min(max_timings, SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT);
    for (int bucket = 0; bucket < timing_count; bucket++) {
        SDLGameRenderer_PerfCaptureRasterBucketTiming* entry = &out_timings[bucket];
        entry->bucket = (SDLGameRenderer_PerfCaptureRasterBucket)bucket;
        entry->sample_period = software_frame_raster_sample_periods[bucket];
        entry->sampled_calls = perf_capture_raster_sample_calls[bucket];
        entry->sampled_pixels = perf_capture_raster_sample_pixels[bucket];
        entry->sampled_ns = perf_capture_raster_sample_ns[bucket];
    }

    return timing_count;
}

void SDLGameRenderer_GetPerfCaptureFastNonIntegerPhaseTotals(
    SDLGameRenderer_PerfCaptureFastNonIntegerPhaseTotals* out_totals) {
    if (out_totals == NULL) {
        return;
    }

    *out_totals = perf_capture_fast_non_integer_phase_totals;
}

static int get_perf_capture_textured_rect_families(const SDLGameRenderer_PerfCaptureTexturedRectFamily* families,
                                                   int family_count,
                                                   SDLGameRenderer_PerfCaptureTexturedRectFamily* out_families,
                                                   int max_families) {
    if ((out_families == NULL) || (max_families <= 0) || (families == NULL) || (family_count <= 0)) {
        return 0;
    }

    int selected_count = 0;
    for (int slot = 0; slot < max_families; slot++) {
        int best_index = -1;
        for (int i = 0; i < family_count; i++) {
            const SDLGameRenderer_PerfCaptureTexturedRectFamily* candidate = &families[i];
            if (candidate->task_count == 0) {
                continue;
            }

            bool already_selected = false;
            for (int existing = 0; existing < selected_count; existing++) {
                const SDLGameRenderer_PerfCaptureTexturedRectFamily* existing_entry = &out_families[existing];
                if ((existing_entry->texture_handle == candidate->texture_handle) &&
                    (existing_entry->palette_handle == candidate->palette_handle) &&
                    (existing_entry->source_format == candidate->source_format) &&
                    (existing_entry->source_width == candidate->source_width) &&
                    (existing_entry->source_height == candidate->source_height) &&
                    (existing_entry->alpha_only == candidate->alpha_only) &&
                    (existing_entry->rgb_mod == candidate->rgb_mod) &&
                    (existing_entry->opaque_color == candidate->opaque_color) &&
                    (existing_entry->integer_positions == candidate->integer_positions) &&
                    (existing_entry->integer_source_rect == candidate->integer_source_rect) &&
                    (existing_entry->full_texture_source_rect == candidate->full_texture_source_rect) &&
                    (existing_entry->clipped == candidate->clipped) &&
                    (existing_entry->flip_h == candidate->flip_h) && (existing_entry->flip_v == candidate->flip_v)) {
                    already_selected = true;
                    break;
                }
            }
            if (already_selected) {
                continue;
            }

            if (best_index < 0) {
                best_index = i;
                continue;
            }

            const SDLGameRenderer_PerfCaptureTexturedRectFamily* best = &families[best_index];
            if ((candidate->task_count > best->task_count) ||
                ((candidate->task_count == best->task_count) &&
                 (candidate->submitted_pixels > best->submitted_pixels)) ||
                ((candidate->task_count == best->task_count) &&
                 (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->lookup_entries > best->lookup_entries)) ||
                ((candidate->task_count == best->task_count) &&
                 (candidate->submitted_pixels == best->submitted_pixels) &&
                 (candidate->lookup_entries == best->lookup_entries) &&
                 (candidate->texture_handle < best->texture_handle))) {
                best_index = i;
            }
        }

        if (best_index < 0) {
            break;
        }

        SDLGameRenderer_PerfCaptureTexturedRectFamily* entry = &out_families[selected_count];
        *entry = families[best_index];
        const int texture_index = entry->texture_handle - 1;
        if ((texture_index >= 0) && (texture_index < FL_TEXTURE_MAX)) {
            copy_perf_capture_texture_logical_identity(texture_index,
                                                       &entry->logical_identity_known,
                                                       &entry->logical_identity_mixed,
                                                       &entry->logical_identity_registrations,
                                                       &entry->logical_source_kind,
                                                       &entry->logical_ix_num,
                                                       &entry->logical_ix_num_first,
                                                       &entry->logical_slot_index,
                                                       &entry->logical_chunk_index,
                                                       &entry->logical_texture_total);
        }
        selected_count += 1;
    }

    return selected_count;
}

static void get_perf_capture_textured_rect_family_totals(const SDLGameRenderer_PerfCaptureTexturedRectFamily* families,
                                                         int family_count,
                                                         Uint64* out_task_total,
                                                         Uint64* out_pixel_total,
                                                         Uint64* out_lookup_entry_total,
                                                         int* out_family_count) {
    Uint64 task_total = 0;
    Uint64 pixel_total = 0;
    Uint64 lookup_entry_total = 0;
    for (int i = 0; i < family_count; i++) {
        task_total += families[i].task_count;
        pixel_total += families[i].submitted_pixels;
        lookup_entry_total += families[i].lookup_entries;
    }

    if (out_task_total != NULL) {
        *out_task_total = task_total;
    }
    if (out_pixel_total != NULL) {
        *out_pixel_total = pixel_total;
    }
    if (out_lookup_entry_total != NULL) {
        *out_lookup_entry_total = lookup_entry_total;
    }
    if (out_family_count != NULL) {
        *out_family_count = family_count;
    }
}

static bool perf_capture_fast_non_integer_shape_matches_family(
    const SDLGameRenderer_PerfCaptureTexturedRectExactShape* shape,
    const SDLGameRenderer_PerfCaptureTexturedRectFamily* family) {
    if ((shape == NULL) || (family == NULL)) {
        return false;
    }

    return (shape->texture_handle == family->texture_handle) && (shape->palette_handle == family->palette_handle) &&
           (shape->source_format == family->source_format) && (shape->source_width == family->source_width) &&
           (shape->source_height == family->source_height) && (shape->alpha_only == family->alpha_only) &&
           (shape->rgb_mod == family->rgb_mod) && (shape->opaque_color == family->opaque_color) &&
           (shape->integer_positions == family->integer_positions) &&
           (shape->integer_source_rect == family->integer_source_rect) &&
           (shape->full_texture_source_rect == family->full_texture_source_rect) &&
           (shape->clipped == family->clipped) && (shape->flip_h == family->flip_h) &&
           (shape->flip_v == family->flip_v);
}

static void annotate_fast_non_integer_family_dominant_shape(SDLGameRenderer_PerfCaptureTexturedRectFamily* family) {
    if (family == NULL) {
        return;
    }

    family->exact_shape_variant_count = 0;
    family->dominant_shape_source_w = -1;
    family->dominant_shape_source_h = -1;
    family->dominant_shape_visible_w = -1;
    family->dominant_shape_visible_h = -1;
    family->dominant_shape_task_count = 0;
    family->dominant_shape_submitted_pixels = 0;
    family->dominant_shape_sampled_ns = 0;

    const SDLGameRenderer_PerfCaptureTexturedRectExactShape* dominant_shape = NULL;
    for (int i = 0; i < perf_capture_fast_non_integer_shape_count; i++) {
        const SDLGameRenderer_PerfCaptureTexturedRectExactShape* candidate = &perf_capture_fast_non_integer_shapes[i];
        if (!perf_capture_fast_non_integer_shape_matches_family(candidate, family)) {
            continue;
        }

        family->exact_shape_variant_count += 1;
        if ((dominant_shape == NULL) || (candidate->task_count > dominant_shape->task_count) ||
            ((candidate->task_count == dominant_shape->task_count) &&
             (candidate->submitted_pixels > dominant_shape->submitted_pixels)) ||
            ((candidate->task_count == dominant_shape->task_count) &&
             (candidate->submitted_pixels == dominant_shape->submitted_pixels) &&
             (candidate->sampled_ns > dominant_shape->sampled_ns)) ||
            ((candidate->task_count == dominant_shape->task_count) &&
             (candidate->submitted_pixels == dominant_shape->submitted_pixels) &&
             (candidate->sampled_ns == dominant_shape->sampled_ns) &&
             (candidate->source_w > dominant_shape->source_w)) ||
            ((candidate->task_count == dominant_shape->task_count) &&
             (candidate->submitted_pixels == dominant_shape->submitted_pixels) &&
             (candidate->sampled_ns == dominant_shape->sampled_ns) &&
             (candidate->source_w == dominant_shape->source_w) &&
             (candidate->source_h > dominant_shape->source_h))) {
            dominant_shape = candidate;
        }
    }

    if (dominant_shape == NULL) {
        return;
    }

    family->dominant_shape_source_w = dominant_shape->source_w;
    family->dominant_shape_source_h = dominant_shape->source_h;
    family->dominant_shape_visible_w = dominant_shape->visible_w;
    family->dominant_shape_visible_h = dominant_shape->visible_h;
    family->dominant_shape_task_count = dominant_shape->task_count;
    family->dominant_shape_submitted_pixels = dominant_shape->submitted_pixels;
    family->dominant_shape_sampled_ns = dominant_shape->sampled_ns;
}

int SDLGameRenderer_GetPerfCaptureFastNonIntegerFamilies(SDLGameRenderer_PerfCaptureTexturedRectFamily* out_families,
                                                         int max_families) {
    const int family_count = get_perf_capture_textured_rect_families(
        perf_capture_fast_non_integer_families, perf_capture_fast_non_integer_family_count, out_families, max_families);
    for (int i = 0; i < family_count; i++) {
        annotate_fast_non_integer_family_dominant_shape(&out_families[i]);
    }
    return family_count;
}

static void accumulate_perf_capture_basic_first_window_alpha_offpath(
    SDLGameRenderer_PerfCaptureTexturedRectFamily* family,
    const SDLSoftwareFrame_NonIntegerTelemetry* telemetry,
    Uint64 weight) {
    if ((family == NULL) || (telemetry == NULL) || (weight == 0u)) {
        return;
    }

    family->source_alpha_opaque_pixels += telemetry->source_alpha_opaque_pixels * weight;
    family->source_alpha_transparent_pixels += telemetry->source_alpha_transparent_pixels * weight;
    family->source_alpha_blended_pixels += telemetry->source_alpha_blended_pixels * weight;
    family->subrect_rows_total += telemetry->subrect_rows_total * weight;
    family->subrect_rows_all_opaque += telemetry->subrect_rows_all_opaque * weight;
    family->subrect_rows_all_transparent += telemetry->subrect_rows_all_transparent * weight;
    family->subrect_rows_binary_alpha_only += telemetry->subrect_rows_binary_alpha_only * weight;
    family->subrect_rows_binary_mixed += telemetry->subrect_rows_binary_mixed * weight;
    family->subrect_rows_with_blended += telemetry->subrect_rows_with_blended * weight;
    family->source_alpha_opaque_spans += telemetry->source_alpha_opaque_spans * weight;
    family->source_alpha_transparent_spans += telemetry->source_alpha_transparent_spans * weight;
    family->source_alpha_blended_spans += telemetry->source_alpha_blended_spans * weight;
    if (telemetry->source_alpha_opaque_span_max > family->source_alpha_opaque_span_max) {
        family->source_alpha_opaque_span_max = telemetry->source_alpha_opaque_span_max;
    }
    if (telemetry->source_alpha_transparent_span_max > family->source_alpha_transparent_span_max) {
        family->source_alpha_transparent_span_max = telemetry->source_alpha_transparent_span_max;
    }
    if (telemetry->source_alpha_blended_span_max > family->source_alpha_blended_span_max) {
        family->source_alpha_blended_span_max = telemetry->source_alpha_blended_span_max;
    }
}

static bool perf_capture_basic_first_window_exact_hot_family_matches_family(
    const SDLGameRenderer_PerfCaptureTexturedRectFamily* family) {
    if (family == NULL) {
        return false;
    }

    return (family->texture_handle == 57 || family->texture_handle == 58) &&
           (family->palette_handle >= 391) && (family->palette_handle <= 394) &&
           (family->source_format == SDL_PIXELFORMAT_ARGB8888) && (family->source_width == 256) &&
           (family->source_height == 256) && !family->alpha_only && !family->rgb_mod && family->opaque_color &&
           family->integer_source_rect && !family->full_texture_source_rect && !family->clipped && !family->flip_h &&
           !family->flip_v;
}

static bool perf_capture_basic_first_window_onset_exact_hot_family_matches_family(
    const SDLGameRenderer_PerfCaptureTexturedRectFamily* family) {
    if (family == NULL) {
        return false;
    }

    return (((family->texture_handle == 57) &&
             ((family->palette_handle == 391) || (family->palette_handle == 394) ||
              (family->palette_handle == 393) || (family->palette_handle == 329))) ||
            ((family->texture_handle == 58) && (family->palette_handle == 393)) ||
            ((family->texture_handle == 18) && (family->palette_handle == 37))) &&
           (family->source_format == SDL_PIXELFORMAT_ARGB8888) && (family->source_width == 256) &&
           (family->source_height == 256) && !family->alpha_only && !family->rgb_mod && family->opaque_color &&
           family->integer_source_rect && !family->full_texture_source_rect && !family->clipped && !family->flip_h &&
           !family->flip_v;
}

static bool perf_capture_basic_first_window_onset_cluster_alpha_offpath_matches_family(
    const SDLGameRenderer_PerfCaptureTexturedRectFamily* family) {
    if (family == NULL) {
        return false;
    }

    return perf_capture_basic_first_window_onset_exact_hot_family_matches_family(family);
}

typedef bool (*PerfCaptureBasicFirstWindowAlphaOffpathFamilyMatcher)(
    const SDLGameRenderer_PerfCaptureTexturedRectFamily* family);

static void apply_perf_capture_basic_first_window_alpha_offpath(
    SDLGameRenderer_PerfCaptureTexturedRectFamily* families,
    int family_count,
    PerfCaptureBasicFirstWindowAlphaOffpathFamilyMatcher matches_family) {
    if ((families == NULL) || (family_count <= 0) || (matches_family == NULL)) {
        return;
    }

    for (int i = 0; i < family_count; i++) {
        SDLGameRenderer_PerfCaptureTexturedRectFamily* family = &families[i];
        if (!matches_family(family)) {
            continue;
        }

        const int texture_index = family->texture_handle - 1;
        const int palette_handle = family->palette_handle;
        if ((texture_index < 0) || (texture_index >= FL_TEXTURE_MAX) || (palette_handle < 0) ||
            (palette_handle > FL_PALETTE_MAX)) {
            continue;
        }

        const SDL_Surface* src_surface = software_surface_cache[texture_index][palette_handle];
        if ((src_surface == NULL) || (src_surface->format != SDL_PIXELFORMAT_ARGB8888)) {
            continue;
        }

        SDLGameRenderer_PerfCaptureTexturedRectFamily replay = *family;
        replay.source_alpha_opaque_pixels = 0;
        replay.source_alpha_transparent_pixels = 0;
        replay.source_alpha_blended_pixels = 0;
        replay.subrect_rows_total = 0;
        replay.subrect_rows_all_opaque = 0;
        replay.subrect_rows_all_transparent = 0;
        replay.subrect_rows_binary_alpha_only = 0;
        replay.subrect_rows_binary_mixed = 0;
        replay.subrect_rows_with_blended = 0;
        replay.source_alpha_opaque_spans = 0;
        replay.source_alpha_transparent_spans = 0;
        replay.source_alpha_blended_spans = 0;
        replay.source_alpha_opaque_span_max = 0;
        replay.source_alpha_transparent_span_max = 0;
        replay.source_alpha_blended_span_max = 0;
        replay.exact_shape_variant_count = 0;
        replay.dominant_shape_source_w = -1;
        replay.dominant_shape_source_h = -1;
        replay.dominant_shape_visible_w = -1;
        replay.dominant_shape_visible_h = -1;
        replay.dominant_shape_task_count = 0;
        replay.dominant_shape_submitted_pixels = 0;
        replay.dominant_shape_sampled_ns = 0;
        int replayed_shape_count = 0;

        for (int shape_index = 0; shape_index < perf_capture_basic_first_window_alpha_offpath_shape_count;
             shape_index++) {
            const PerfCaptureBasicFirstWindowAlphaOffpathShape* shape =
                &perf_capture_basic_first_window_alpha_offpath_shapes[shape_index];
            if ((shape->texture_handle != family->texture_handle) || (shape->palette_handle != family->palette_handle)) {
                continue;
            }

            const SDL_FRect dst_rect = { shape->dst_x, shape->dst_y, shape->dst_w, shape->dst_h };
            SDL_Rect source_rect = { shape->source_x, shape->source_y, shape->source_w, shape->source_h };
            SDLSoftwareFrame_NonIntegerTelemetry telemetry;
            if (!SDLSoftwareFrame_AnalyzeNonIntegerSourceAlphaARGB8888(
                    &dst_rect, &source_rect, SDL_FLIP_NONE, 0xFFFFFFFFu, src_surface, &telemetry)) {
                continue;
            }

            accumulate_perf_capture_basic_first_window_alpha_offpath(&replay, &telemetry, shape->task_count);
            replay.exact_shape_variant_count += 1;
            replayed_shape_count += 1;
            if ((shape->task_count > replay.dominant_shape_task_count) ||
                ((shape->task_count == replay.dominant_shape_task_count) &&
                 (shape->submitted_pixels > replay.dominant_shape_submitted_pixels))) {
                replay.dominant_shape_source_w = shape->source_w;
                replay.dominant_shape_source_h = shape->source_h;
                replay.dominant_shape_visible_w = shape->visible_w;
                replay.dominant_shape_visible_h = shape->visible_h;
                replay.dominant_shape_task_count = shape->task_count;
                replay.dominant_shape_submitted_pixels = shape->submitted_pixels;
                replay.dominant_shape_sampled_ns = 0;
            }
        }

        if (replayed_shape_count > 0) {
            family->source_alpha_opaque_pixels = replay.source_alpha_opaque_pixels;
            family->source_alpha_transparent_pixels = replay.source_alpha_transparent_pixels;
            family->source_alpha_blended_pixels = replay.source_alpha_blended_pixels;
            family->subrect_rows_total = replay.subrect_rows_total;
            family->subrect_rows_all_opaque = replay.subrect_rows_all_opaque;
            family->subrect_rows_all_transparent = replay.subrect_rows_all_transparent;
            family->subrect_rows_binary_alpha_only = replay.subrect_rows_binary_alpha_only;
            family->subrect_rows_binary_mixed = replay.subrect_rows_binary_mixed;
            family->subrect_rows_with_blended = replay.subrect_rows_with_blended;
            family->source_alpha_opaque_spans = replay.source_alpha_opaque_spans;
            family->source_alpha_transparent_spans = replay.source_alpha_transparent_spans;
            family->source_alpha_blended_spans = replay.source_alpha_blended_spans;
            family->source_alpha_opaque_span_max = replay.source_alpha_opaque_span_max;
            family->source_alpha_transparent_span_max = replay.source_alpha_transparent_span_max;
            family->source_alpha_blended_span_max = replay.source_alpha_blended_span_max;
            family->exact_shape_variant_count = replay.exact_shape_variant_count;
            family->dominant_shape_source_w = replay.dominant_shape_source_w;
            family->dominant_shape_source_h = replay.dominant_shape_source_h;
            family->dominant_shape_visible_w = replay.dominant_shape_visible_w;
            family->dominant_shape_visible_h = replay.dominant_shape_visible_h;
            family->dominant_shape_task_count = replay.dominant_shape_task_count;
            family->dominant_shape_submitted_pixels = replay.dominant_shape_submitted_pixels;
            family->dominant_shape_sampled_ns = replay.dominant_shape_sampled_ns;
        }
    }
}

void SDLGameRenderer_ApplyPerfCaptureBasicFirstWindowExactHotFamilyAlphaOffpath(
    SDLGameRenderer_PerfCaptureTexturedRectFamily* families,
    int family_count) {
    if (!perf_capture_basic_first_window_exact_hot_family_alpha_offpath_enabled) {
        return;
    }

    apply_perf_capture_basic_first_window_alpha_offpath(
        families, family_count, perf_capture_basic_first_window_exact_hot_family_matches_family);
}

void SDLGameRenderer_ApplyPerfCaptureBasicFirstWindowOnsetExactHotFamilyAlphaOffpath(
    SDLGameRenderer_PerfCaptureTexturedRectFamily* families,
    int family_count) {
    if (!perf_capture_basic_first_window_onset_exact_hot_family_alpha_offpath_enabled) {
        return;
    }

    apply_perf_capture_basic_first_window_alpha_offpath(
        families, family_count, perf_capture_basic_first_window_onset_exact_hot_family_matches_family);
}

void SDLGameRenderer_ApplyPerfCaptureBasicFirstWindowOnsetClusterAlphaOffpath(
    SDLGameRenderer_PerfCaptureTexturedRectFamily* families,
    int family_count) {
    if (!perf_capture_basic_first_window_onset_cluster_alpha_offpath_enabled) {
        return;
    }

    apply_perf_capture_basic_first_window_alpha_offpath(
        families, family_count, perf_capture_basic_first_window_onset_cluster_alpha_offpath_matches_family);
}

int SDLGameRenderer_GetPerfCaptureFastExactFamilies(SDLGameRenderer_PerfCaptureTexturedRectFamily* out_families,
                                                    int max_families) {
    return get_perf_capture_textured_rect_families(
        perf_capture_fast_exact_families, perf_capture_fast_exact_family_count, out_families, max_families);
}

void SDLGameRenderer_GetPerfCaptureFastExactFamilyTotals(Uint64* out_task_total,
                                                         Uint64* out_pixel_total,
                                                         Uint64* out_lookup_entry_total,
                                                         int* out_family_count) {
    get_perf_capture_textured_rect_family_totals(perf_capture_fast_exact_families,
                                                 perf_capture_fast_exact_family_count,
                                                 out_task_total,
                                                 out_pixel_total,
                                                 out_lookup_entry_total,
                                                 out_family_count);
}

void SDLGameRenderer_GetPerfCaptureFastNonIntegerFamilyTotals(Uint64* out_task_total,
                                                              Uint64* out_pixel_total,
                                                              Uint64* out_lookup_entry_total,
                                                              int* out_family_count) {
    get_perf_capture_textured_rect_family_totals(perf_capture_fast_non_integer_families,
                                                 perf_capture_fast_non_integer_family_count,
                                                 out_task_total,
                                                 out_pixel_total,
                                                 out_lookup_entry_total,
                                                 out_family_count);
}

int SDLGameRenderer_GetPerfCaptureFastNonIntegerSharedShapes(
    SDLGameRenderer_PerfCaptureFastNonIntegerSharedShape* out_shapes,
    int max_shapes) {
#if ENABLE_PERF_TELEMETRY
    return get_perf_capture_fast_non_integer_shared_shapes_from_exact_shapes(
        perf_capture_fast_non_integer_shapes,
        perf_capture_fast_non_integer_shape_count,
        out_shapes,
        max_shapes,
        NULL,
        NULL,
        NULL,
        NULL);
#else
    (void)out_shapes;
    (void)max_shapes;
    return 0;
#endif
}

void SDLGameRenderer_GetPerfCaptureFastNonIntegerSharedShapeTotals(Uint64* out_task_total,
                                                                   Uint64* out_pixel_total,
                                                                   Uint64* out_sampled_ns_total,
                                                                   int* out_shape_count) {
#if ENABLE_PERF_TELEMETRY
    get_perf_capture_fast_non_integer_shared_shapes_from_exact_shapes(perf_capture_fast_non_integer_shapes,
                                                                      perf_capture_fast_non_integer_shape_count,
                                                                      NULL,
                                                                      0,
                                                                      out_task_total,
                                                                      out_pixel_total,
                                                                      out_sampled_ns_total,
                                                                      out_shape_count);
#else
    if (out_task_total != NULL) {
        *out_task_total = 0;
    }
    if (out_pixel_total != NULL) {
        *out_pixel_total = 0;
    }
    if (out_sampled_ns_total != NULL) {
        *out_sampled_ns_total = 0;
    }
    if (out_shape_count != NULL) {
        *out_shape_count = 0;
    }
#endif
}

int SDLGameRenderer_GetPerfCaptureFastNonIntegerLookupProfiles(
    SDLGameRenderer_PerfCaptureFastNonIntegerLookupProfile* out_profiles,
    int max_profiles) {
#if ENABLE_PERF_TELEMETRY
    return get_perf_capture_fast_non_integer_lookup_profiles_from_patterns(
        perf_capture_fast_non_integer_lookup_patterns,
        perf_capture_fast_non_integer_lookup_pattern_count,
        out_profiles,
        max_profiles,
        NULL,
        NULL,
        NULL,
        NULL);
#else
    (void)out_profiles;
    (void)max_profiles;
    return 0;
#endif
}

void SDLGameRenderer_GetPerfCaptureFastNonIntegerLookupProfileTotals(Uint64* out_task_total,
                                                                     Uint64* out_pixel_total,
                                                                     Uint64* out_sampled_ns_total,
                                                                     int* out_profile_count) {
#if ENABLE_PERF_TELEMETRY
    get_perf_capture_fast_non_integer_lookup_profiles_from_patterns(
        perf_capture_fast_non_integer_lookup_patterns,
        perf_capture_fast_non_integer_lookup_pattern_count,
        NULL,
        0,
        out_task_total,
        out_pixel_total,
        out_sampled_ns_total,
        out_profile_count);
#else
    if (out_task_total != NULL) {
        *out_task_total = 0;
    }
    if (out_pixel_total != NULL) {
        *out_pixel_total = 0;
    }
    if (out_sampled_ns_total != NULL) {
        *out_sampled_ns_total = 0;
    }
    if (out_profile_count != NULL) {
        *out_profile_count = 0;
    }
#endif
}

int SDLGameRenderer_GetPerfCaptureSABurstEffectSamples(
    PerfCaptureSABurstEffectSample* out_samples,
    int max_samples) {
#if ENABLE_PERF_TELEMETRY
    if ((out_samples == NULL) || (max_samples <= 0)) {
        return 0;
    }

    int selected_count = 0;
    for (int slot = 0; slot < max_samples; slot++) {
        int best_index = -1;
        for (int i = 0; i < perf_capture_sa_burst_effect_sample_count; i++) {
            const PerfCaptureSABurstEffectSample* candidate =
                &perf_capture_sa_burst_effect_samples[i];
            if (candidate->task_count == 0) {
                continue;
            }

            bool already_selected = false;
            for (int existing = 0; existing < selected_count; existing++) {
                const PerfCaptureSABurstEffectSample* selected = &out_samples[existing];
                if ((selected->texture_handle == candidate->texture_handle) &&
                    (selected->palette_handle == candidate->palette_handle) &&
                    (selected->source_x == candidate->source_x) && (selected->source_y == candidate->source_y) &&
                    (selected->source_w == candidate->source_w) && (selected->source_h == candidate->source_h) &&
                    (selected->visible_w == candidate->visible_w) && (selected->visible_h == candidate->visible_h) &&
                    (selected->center_x == candidate->center_x) && (selected->center_y == candidate->center_y) &&
                    (selected->dst_w == candidate->dst_w) && (selected->dst_h == candidate->dst_h)) {
                    already_selected = true;
                    break;
                }
            }
            if (already_selected) {
                continue;
            }

            if ((best_index < 0) || (candidate->task_count > perf_capture_sa_burst_effect_samples[best_index].task_count) ||
                ((candidate->task_count == perf_capture_sa_burst_effect_samples[best_index].task_count) &&
                 (candidate->submitted_pixels >
                  perf_capture_sa_burst_effect_samples[best_index].submitted_pixels)) ||
                ((candidate->task_count == perf_capture_sa_burst_effect_samples[best_index].task_count) &&
                 (candidate->submitted_pixels ==
                  perf_capture_sa_burst_effect_samples[best_index].submitted_pixels) &&
                 (candidate->sampled_ns > perf_capture_sa_burst_effect_samples[best_index].sampled_ns))) {
                best_index = i;
            }
        }

        if (best_index < 0) {
            break;
        }

        out_samples[selected_count] = perf_capture_sa_burst_effect_samples[best_index];
        selected_count += 1;
    }

    return selected_count;
#else
    (void)out_samples;
    (void)max_samples;
    return 0;
#endif
}

void SDLGameRenderer_GetPerfCaptureSABurstEffectSampleTotals(Uint64* out_task_total,
                                                                        Uint64* out_pixel_total,
                                                                        Uint64* out_sampled_ns_total,
                                                                        int* out_sample_count) {
#if ENABLE_PERF_TELEMETRY
    Uint64 task_total = 0;
    Uint64 pixel_total = 0;
    Uint64 sampled_ns_total = 0;
    int sample_count = 0;
    for (int i = 0; i < perf_capture_sa_burst_effect_sample_count; i++) {
        const PerfCaptureSABurstEffectSample* entry = &perf_capture_sa_burst_effect_samples[i];
        if (entry->task_count == 0) {
            continue;
        }
        task_total += entry->task_count;
        pixel_total += entry->submitted_pixels;
        sampled_ns_total += entry->sampled_ns;
        sample_count += 1;
    }
    if (out_task_total != NULL) {
        *out_task_total = task_total;
    }
    if (out_pixel_total != NULL) {
        *out_pixel_total = pixel_total;
    }
    if (out_sampled_ns_total != NULL) {
        *out_sampled_ns_total = sampled_ns_total;
    }
    if (out_sample_count != NULL) {
        *out_sample_count = sample_count;
    }
#else
    if (out_task_total != NULL) {
        *out_task_total = 0;
    }
    if (out_pixel_total != NULL) {
        *out_pixel_total = 0;
    }
    if (out_sampled_ns_total != NULL) {
        *out_sampled_ns_total = 0;
    }
    if (out_sample_count != NULL) {
        *out_sample_count = 0;
    }
#endif
}

int SDLGameRenderer_GetPerfCaptureGenericTexturedFamilies(SDLGameRenderer_PerfCaptureTexturedRectFamily* out_families,
                                                          int max_families) {
    return get_perf_capture_textured_rect_families(
        perf_capture_generic_textured_families, perf_capture_generic_textured_family_count, out_families, max_families);
}

void SDLGameRenderer_GetPerfCaptureGenericTexturedFamilyTotals(Uint64* out_task_total,
                                                               Uint64* out_pixel_total,
                                                               Uint64* out_lookup_entry_total,
                                                               int* out_family_count) {
    get_perf_capture_textured_rect_family_totals(perf_capture_generic_textured_families,
                                                 perf_capture_generic_textured_family_count,
                                                 out_task_total,
                                                 out_pixel_total,
                                                 out_lookup_entry_total,
                                                 out_family_count);
}

void SDLGameRenderer_RefreshPerfCaptureTexturedRectFamilyLogicalIdentity(
    SDLGameRenderer_PerfCaptureTexturedRectFamily* families,
    int family_count) {
    if ((families == NULL) || (family_count <= 0)) {
        return;
    }

    for (int i = 0; i < family_count; i++) {
        SDLGameRenderer_PerfCaptureTexturedRectFamily* entry = &families[i];
        const int texture_index = entry->texture_handle - 1;
        copy_perf_capture_texture_logical_identity(texture_index,
                                                   &entry->logical_identity_known,
                                                   &entry->logical_identity_mixed,
                                                   &entry->logical_identity_registrations,
                                                   &entry->logical_source_kind,
                                                   &entry->logical_ix_num,
                                                   &entry->logical_ix_num_first,
                                                   &entry->logical_slot_index,
                                                   &entry->logical_chunk_index,
                                                   &entry->logical_texture_total);
    }
}

int SDLGameRenderer_GetPerfCaptureRefreshLocalityCandidates(
    SDLGameRenderer_PerfCaptureRefreshLocalityCandidate* out_candidates,
    int max_candidates) {
    if ((out_candidates == NULL) || (max_candidates <= 0)) {
        return 0;
    }

    int candidate_count = 0;
    for (int slot = 0; slot < max_candidates; slot++) {
        int best_texture_index = -1;
        for (int texture_index = 0; texture_index < FL_TEXTURE_MAX; texture_index++) {
            const Uint64 refresh_attempts = perf_capture_refresh_attempts_by_texture[texture_index];
            const Uint64 tracked_unlocks = perf_capture_unlock_locality_tracked_by_texture[texture_index];
            if ((refresh_attempts == 0) && (tracked_unlocks == 0)) {
                continue;
            }

            bool already_selected = false;
            for (int existing = 0; existing < candidate_count; existing++) {
                if (out_candidates[existing].texture_handle == (texture_index + 1)) {
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

            const Uint64 best_refresh_attempts = perf_capture_refresh_attempts_by_texture[best_texture_index];
            const Uint64 best_tracked_unlocks = perf_capture_unlock_locality_tracked_by_texture[best_texture_index];
            if ((refresh_attempts > best_refresh_attempts) ||
                ((refresh_attempts == best_refresh_attempts) && (tracked_unlocks > best_tracked_unlocks)) ||
                ((refresh_attempts == best_refresh_attempts) && (tracked_unlocks == best_tracked_unlocks) &&
                 (perf_capture_refresh_pixels_by_texture[texture_index] >
                  perf_capture_refresh_pixels_by_texture[best_texture_index])) ||
                ((refresh_attempts == best_refresh_attempts) && (tracked_unlocks == best_tracked_unlocks) &&
                 (perf_capture_refresh_pixels_by_texture[texture_index] ==
                  perf_capture_refresh_pixels_by_texture[best_texture_index]) &&
                 (texture_index < best_texture_index))) {
                best_texture_index = texture_index;
            }
        }

        if (best_texture_index < 0) {
            break;
        }

        const Uint64 refresh_attempts = perf_capture_refresh_attempts_by_texture[best_texture_index];
        const Uint64 tracked_unlocks = perf_capture_unlock_locality_tracked_by_texture[best_texture_index];
        const bool refresh_has_shape = refresh_attempts > 0;
        const bool locality_has_shape = tracked_unlocks > 0;
        const Uint32 refresh_source_format = perf_capture_refresh_source_format_by_texture[best_texture_index];
        const Uint32 locality_source_format = perf_capture_unlock_locality_source_format_by_texture[best_texture_index];
        const int refresh_width = perf_capture_refresh_width_by_texture[best_texture_index];
        const int refresh_height = perf_capture_refresh_height_by_texture[best_texture_index];
        const int locality_width = perf_capture_unlock_locality_width_by_texture[best_texture_index];
        const int locality_height = perf_capture_unlock_locality_height_by_texture[best_texture_index];

        bool source_shape_mixed = perf_capture_refresh_shape_mixed_by_texture[best_texture_index] ||
                                  perf_capture_unlock_locality_shape_mixed_by_texture[best_texture_index];
        Uint32 source_format = SDL_PIXELFORMAT_UNKNOWN;
        int width = 0;
        int height = 0;

        if (refresh_has_shape) {
            source_format = refresh_source_format;
            width = refresh_width;
            height = refresh_height;
        }
        if (locality_has_shape) {
            if (!refresh_has_shape) {
                source_format = locality_source_format;
                width = locality_width;
                height = locality_height;
            } else if ((refresh_source_format != locality_source_format) || (refresh_width != locality_width) ||
                       (refresh_height != locality_height)) {
                source_shape_mixed = true;
            }
        }
        if (source_shape_mixed) {
            source_format = SDL_PIXELFORMAT_UNKNOWN;
            width = 0;
            height = 0;
        }

        SDLGameRenderer_PerfCaptureRefreshLocalityCandidate* entry = &out_candidates[candidate_count];
        entry->texture_handle = best_texture_index + 1;
        entry->source_format = source_format;
        entry->width = width;
        entry->height = height;
        entry->source_shape_mixed = source_shape_mixed ? 1 : 0;
        copy_perf_capture_texture_logical_identity(best_texture_index,
                                                   &entry->logical_identity_known,
                                                   &entry->logical_identity_mixed,
                                                   &entry->logical_identity_registrations,
                                                   &entry->logical_source_kind,
                                                   &entry->logical_ix_num,
                                                   &entry->logical_ix_num_first,
                                                   &entry->logical_slot_index,
                                                   &entry->logical_chunk_index,
                                                   &entry->logical_texture_total);
        entry->refresh_attempts = refresh_attempts;
        entry->refresh_pixels = perf_capture_refresh_pixels_by_texture[best_texture_index];
        entry->max_fanout = (int)perf_capture_refresh_fanout_max_by_texture[best_texture_index];
        entry->source_surface_destroy_calls = perf_capture_source_surface_destroy_calls_by_texture[best_texture_index];
        entry->current_lifetime_refresh_attempts =
            perf_capture_current_lifetime_refresh_attempts_by_texture[best_texture_index];
        entry->current_lifetime_partial_refresh_attempts =
            perf_capture_current_lifetime_partial_refresh_attempts_by_texture[best_texture_index];
        entry->current_lifetime_full_refresh_attempts =
            perf_capture_current_lifetime_full_refresh_attempts_by_texture[best_texture_index];
        entry->current_lifetime_full_refresh_no_usable_dirty_rect_attempts =
            perf_capture_current_lifetime_full_no_usable_dirty_rect_attempts_by_texture[best_texture_index];
        entry->partial_refresh_attempts = perf_capture_refresh_partial_attempts_by_texture[best_texture_index];
        entry->partial_refresh_pixels = perf_capture_refresh_partial_pixels_by_texture[best_texture_index];
        entry->full_refresh_attempts = perf_capture_refresh_full_attempts_by_texture[best_texture_index];
        entry->full_refresh_pixels = perf_capture_refresh_full_pixels_by_texture[best_texture_index];
        entry->full_refresh_non_texture_dirty_attempts =
            perf_capture_refresh_full_non_texture_dirty_attempts_by_texture[best_texture_index];
        entry->full_refresh_ineligible_source_attempts =
            perf_capture_refresh_full_ineligible_source_attempts_by_texture[best_texture_index];
        entry->full_refresh_no_usable_dirty_rect_attempts =
            perf_capture_refresh_full_no_usable_dirty_rect_attempts_by_texture[best_texture_index];
        entry->full_refresh_oversized_dirty_rect_attempts =
            perf_capture_refresh_full_oversized_dirty_rect_attempts_by_texture[best_texture_index];
        entry->sampled_blit_calls = perf_capture_refresh_blit_sample_calls_by_texture[best_texture_index];
        entry->sampled_blit_ns = perf_capture_refresh_blit_sample_ns_by_texture[best_texture_index];
        entry->sampled_full_blit_calls = perf_capture_refresh_full_blit_sample_calls_by_texture[best_texture_index];
        entry->sampled_full_blit_ns = perf_capture_refresh_full_blit_sample_ns_by_texture[best_texture_index];
        entry->sampled_partial_blit_calls =
            perf_capture_refresh_partial_blit_sample_calls_by_texture[best_texture_index];
        entry->sampled_partial_blit_ns = perf_capture_refresh_partial_blit_sample_ns_by_texture[best_texture_index];
        entry->sampled_full_non_texture_dirty_blit_calls =
            perf_capture_refresh_full_non_texture_dirty_blit_sample_calls_by_texture[best_texture_index];
        entry->sampled_full_non_texture_dirty_blit_ns =
            perf_capture_refresh_full_non_texture_dirty_blit_sample_ns_by_texture[best_texture_index];
        entry->sampled_full_ineligible_source_blit_calls =
            perf_capture_refresh_full_ineligible_source_blit_sample_calls_by_texture[best_texture_index];
        entry->sampled_full_ineligible_source_blit_ns =
            perf_capture_refresh_full_ineligible_source_blit_sample_ns_by_texture[best_texture_index];
        entry->sampled_full_no_usable_dirty_rect_blit_calls =
            perf_capture_refresh_full_no_usable_dirty_rect_blit_sample_calls_by_texture[best_texture_index];
        entry->sampled_full_no_usable_dirty_rect_blit_ns =
            perf_capture_refresh_full_no_usable_dirty_rect_blit_sample_ns_by_texture[best_texture_index];
        entry->sampled_full_oversized_dirty_rect_blit_calls =
            perf_capture_refresh_full_oversized_dirty_rect_blit_sample_calls_by_texture[best_texture_index];
        entry->sampled_full_oversized_dirty_rect_blit_ns =
            perf_capture_refresh_full_oversized_dirty_rect_blit_sample_ns_by_texture[best_texture_index];
        entry->software_surface_access_dirty_texture_same_frame =
            perf_capture_software_surface_access_dirty_texture_same_frame_by_texture[best_texture_index];
        entry->software_surface_access_dirty_texture_carried =
            perf_capture_software_surface_access_dirty_texture_carried_by_texture[best_texture_index];
        entry->software_surface_access_dirty_palette_same_frame =
            perf_capture_software_surface_access_dirty_palette_same_frame_by_texture[best_texture_index];
        entry->software_surface_access_dirty_palette_carried =
            perf_capture_software_surface_access_dirty_palette_carried_by_texture[best_texture_index];
        entry->software_surface_access_dirty_palette_changed_same_frame =
            perf_capture_software_surface_access_dirty_palette_changed_same_frame_by_texture[best_texture_index];
        entry->software_surface_access_dirty_palette_changed_carried =
            perf_capture_software_surface_access_dirty_palette_changed_carried_by_texture[best_texture_index];
        entry->software_surface_access_dirty_palette_unchanged_same_frame =
            perf_capture_software_surface_access_dirty_palette_unchanged_same_frame_by_texture[best_texture_index];
        entry->software_surface_access_dirty_palette_unchanged_carried =
            perf_capture_software_surface_access_dirty_palette_unchanged_carried_by_texture[best_texture_index];
        entry->software_surface_access_cold = perf_capture_software_surface_access_cold_by_texture[best_texture_index];
        entry->current_lifetime_software_surface_access_dirty_texture_same_frame =
            perf_capture_current_lifetime_software_surface_access_dirty_texture_same_frame_by_texture[best_texture_index];
        entry->current_lifetime_software_surface_access_dirty_texture_carried =
            perf_capture_current_lifetime_software_surface_access_dirty_texture_carried_by_texture[best_texture_index];
        entry->current_lifetime_software_surface_access_dirty_palette_same_frame =
            perf_capture_current_lifetime_software_surface_access_dirty_palette_same_frame_by_texture[best_texture_index];
        entry->current_lifetime_software_surface_access_dirty_palette_carried =
            perf_capture_current_lifetime_software_surface_access_dirty_palette_carried_by_texture[best_texture_index];
        entry->current_lifetime_software_surface_access_dirty_palette_changed_same_frame =
            perf_capture_current_lifetime_software_surface_access_dirty_palette_changed_same_frame_by_texture
                [best_texture_index];
        entry->current_lifetime_software_surface_access_dirty_palette_changed_carried =
            perf_capture_current_lifetime_software_surface_access_dirty_palette_changed_carried_by_texture
                [best_texture_index];
        entry->current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame =
            perf_capture_current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame_by_texture
                [best_texture_index];
        entry->current_lifetime_software_surface_access_dirty_palette_unchanged_carried =
            perf_capture_current_lifetime_software_surface_access_dirty_palette_unchanged_carried_by_texture
                [best_texture_index];
        entry->current_lifetime_software_surface_access_cold =
            perf_capture_current_lifetime_software_surface_access_cold_by_texture[best_texture_index];
        entry->tracked_unlocks = tracked_unlocks;
        entry->zero_delta_unlocks = perf_capture_unlock_locality_zero_delta_by_texture[best_texture_index];
        entry->baseline_skips = perf_capture_unlock_locality_baseline_skips_by_texture[best_texture_index];
        entry->non_index8_skips = perf_capture_unlock_locality_non_index8_skips_by_texture[best_texture_index];
        entry->source_pixels = perf_capture_unlock_locality_source_pixels_by_texture[best_texture_index];
        entry->changed_pixels = perf_capture_unlock_locality_changed_pixels_by_texture[best_texture_index];
        entry->changed_rows = perf_capture_unlock_locality_changed_rows_by_texture[best_texture_index];
        entry->changed_bbox_pixels = perf_capture_unlock_locality_changed_bbox_pixels_by_texture[best_texture_index];
        entry->whole_capture_tracked_unlocks =
            perf_capture_unlock_locality_whole_capture_tracked_by_texture[best_texture_index];
        entry->whole_capture_zero_delta_unlocks =
            perf_capture_unlock_locality_whole_capture_zero_delta_by_texture[best_texture_index];
        entry->whole_capture_baseline_skips =
            perf_capture_unlock_locality_whole_capture_baseline_skips_by_texture[best_texture_index];
        entry->whole_capture_non_index8_skips =
            perf_capture_unlock_locality_whole_capture_non_index8_skips_by_texture[best_texture_index];
        entry->whole_capture_source_pixels =
            perf_capture_unlock_locality_whole_capture_source_pixels_by_texture[best_texture_index];
        entry->whole_capture_changed_pixels =
            perf_capture_unlock_locality_whole_capture_changed_pixels_by_texture[best_texture_index];
        entry->whole_capture_changed_rows =
            perf_capture_unlock_locality_whole_capture_changed_rows_by_texture[best_texture_index];
        entry->whole_capture_changed_bbox_pixels =
            perf_capture_unlock_locality_whole_capture_changed_bbox_pixels_by_texture[best_texture_index];
        entry->dirty_rect_record_calls = perf_capture_dirty_rect_record_calls_by_texture[best_texture_index];
        entry->dirty_rect_retained_after_unlock =
            perf_capture_dirty_rect_retained_after_unlock_by_texture[best_texture_index];
        entry->dirty_rect_clear_stale_before_record =
            perf_capture_dirty_rect_clear_stale_before_record_by_texture[best_texture_index];
        entry->dirty_rect_clear_unlock_unused =
            perf_capture_dirty_rect_clear_unlock_unused_by_texture[best_texture_index];
        entry->dirty_rect_clear_access_unused = perf_capture_dirty_rect_clear_access_unused_by_texture[best_texture_index];
        entry->dirty_rect_clear_explicit = perf_capture_dirty_rect_clear_explicit_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_attempts =
            perf_capture_compare_dirty_rect_refresh_attempts_by_texture[best_texture_index];
        entry->compare_dirty_rect_partial_candidate_refresh_attempts =
            perf_capture_compare_dirty_rect_partial_candidate_refresh_attempts_by_texture[best_texture_index];
        entry->compare_dirty_rect_oversized_candidate_refresh_attempts =
            perf_capture_compare_dirty_rect_oversized_candidate_refresh_attempts_by_texture[best_texture_index];
        entry->compare_dirty_rect_no_usable_candidate_refresh_attempts =
            perf_capture_compare_dirty_rect_no_usable_candidate_refresh_attempts_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_bbox_pixels =
            perf_capture_compare_dirty_rect_refresh_bbox_pixels_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_max_bbox_pixels =
            perf_capture_compare_dirty_rect_refresh_max_bbox_pixels_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_pending_unlocks =
            perf_capture_compare_dirty_rect_refresh_pending_unlocks_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_max_pending_unlocks =
            perf_capture_compare_dirty_rect_refresh_max_pending_unlocks_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_32x32_covered_tiles =
            perf_capture_compare_dirty_rect_refresh_32x32_covered_tiles_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_32x32_max_covered_tiles =
            perf_capture_compare_dirty_rect_refresh_32x32_max_covered_tiles_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_32x32_component_count =
            perf_capture_compare_dirty_rect_refresh_32x32_component_count_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_32x32_max_component_count =
            perf_capture_compare_dirty_rect_refresh_32x32_max_component_count_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_32x32_multi_component_refresh_attempts =
            perf_capture_compare_dirty_rect_refresh_32x32_multi_component_refresh_attempts_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_32x32_largest_component_tiles =
            perf_capture_compare_dirty_rect_refresh_32x32_largest_component_tiles_by_texture[best_texture_index];
        entry->compare_dirty_rect_refresh_32x32_max_largest_component_tiles =
            perf_capture_compare_dirty_rect_refresh_32x32_max_largest_component_tiles_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_no_usable_candidate_refresh_attempts =
            perf_capture_compare_dirty_row_mask_no_usable_candidate_refresh_attempts_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_partial_candidate_refresh_attempts =
            perf_capture_compare_dirty_row_mask_partial_candidate_refresh_attempts_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_half_cap_partial_candidate_refresh_attempts =
            perf_capture_compare_dirty_row_mask_half_cap_partial_candidate_refresh_attempts_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_plan_pixels =
            perf_capture_compare_dirty_row_mask_plan_pixels_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_max_plan_pixels =
            perf_capture_compare_dirty_row_mask_max_plan_pixels_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_32x32_covered_tiles =
            perf_capture_compare_dirty_row_mask_32x32_covered_tiles_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_32x32_max_covered_tiles =
            perf_capture_compare_dirty_row_mask_32x32_max_covered_tiles_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_32x32_component_count =
            perf_capture_compare_dirty_row_mask_32x32_component_count_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_32x32_max_component_count =
            perf_capture_compare_dirty_row_mask_32x32_max_component_count_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_32x32_multi_component_refresh_attempts =
            perf_capture_compare_dirty_row_mask_32x32_multi_component_refresh_attempts_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_32x32_largest_component_tiles =
            perf_capture_compare_dirty_row_mask_32x32_largest_component_tiles_by_texture[best_texture_index];
        entry->compare_dirty_row_mask_32x32_max_largest_component_tiles =
            perf_capture_compare_dirty_row_mask_32x32_max_largest_component_tiles_by_texture[best_texture_index];
        entry->renew_chunk_calls = perf_capture_texture_renew_chunk_calls_by_texture[best_texture_index];
        entry->renew_batches = perf_capture_texture_renew_batches_by_texture[best_texture_index];
        entry->renew_batches_without_rect =
            perf_capture_texture_renew_batches_without_rect_by_texture[best_texture_index];
        entry->renew_chunk_pixels = perf_capture_texture_renew_chunk_pixels_by_texture[best_texture_index];
        entry->renew_batch_bbox_pixels = perf_capture_texture_renew_batch_bbox_pixels_by_texture[best_texture_index];
        entry->renew_batch_max_bbox_pixels =
            perf_capture_texture_renew_batch_max_bbox_pixels_by_texture[best_texture_index];
        entry->renew_chunk_8x8_calls = perf_capture_texture_renew_chunk_8x8_calls_by_texture[best_texture_index];
        entry->renew_chunk_16x16_calls = perf_capture_texture_renew_chunk_16x16_calls_by_texture[best_texture_index];
        entry->renew_chunk_32x32_calls = perf_capture_texture_renew_chunk_32x32_calls_by_texture[best_texture_index];
        entry->renew_batch_32x32_covered_tiles =
            perf_capture_texture_renew_batch_32x32_covered_tiles_by_texture[best_texture_index];
        entry->renew_batch_32x32_max_covered_tiles =
            perf_capture_texture_renew_batch_32x32_max_covered_tiles_by_texture[best_texture_index];
        entry->renew_batch_32x32_component_count =
            perf_capture_texture_renew_batch_32x32_component_count_by_texture[best_texture_index];
        entry->renew_batch_32x32_max_component_count =
            perf_capture_texture_renew_batch_32x32_max_component_count_by_texture[best_texture_index];
        entry->renew_batch_32x32_multi_component_batches =
            perf_capture_texture_renew_batch_32x32_multi_component_batches_by_texture[best_texture_index];
        entry->renew_batch_32x32_largest_component_tiles =
            perf_capture_texture_renew_batch_32x32_largest_component_tiles_by_texture[best_texture_index];
        entry->renew_batch_32x32_max_largest_component_tiles =
            perf_capture_texture_renew_batch_32x32_max_largest_component_tiles_by_texture[best_texture_index];
        candidate_count += 1;
    }

    return candidate_count;
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
#if ENABLE_PERF_TELEMETRY
    {
        extern int charsel_active_effect_count;
        extern int charsel_frame_portrait_tiles;
        extern int charsel_frame_plate_tiles;
        out_stats->charsel_active_effects = charsel_active_effect_count;
        out_stats->charsel_portrait_tiles = charsel_frame_portrait_tiles;
        out_stats->charsel_plate_tiles = charsel_frame_plate_tiles;
    }
#endif
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
    clear_texture_unlock_dirty_rect_if_unused(texture_index, TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_STALE_BEFORE_RECORD);

    SDL_Rect dirty_rect = { min_x, min_y, max_x - min_x + 1, max_y - min_y + 1 };
    if ((dirty_rect.w <= 0) || (dirty_rect.h <= 0)) {
        return;
    }

    note_perf_capture_dirty_rect_record(texture_index);
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

void SDLGameRenderer_RecordTextureUnlockDirtyTileMask(unsigned int texture_handle, Uint64 tile_mask) {
    if ((texture_handle == 0) || (texture_handle > FL_TEXTURE_MAX) || (tile_mask == 0)) {
        return;
    }

    const int texture_index = (int)texture_handle - 1;
    clear_texture_unlock_dirty_rect_if_unused(texture_index, TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_STALE_BEFORE_RECORD);
    texture_unlock_dirty_tile_masks[texture_index] = tile_mask;
    texture_unlock_dirty_tile_mask_valid[texture_index] = true;
}

void SDLGameRenderer_RecordTextureRenewChunk(unsigned int texture_handle, int x, int y, int w, int h) {
    if (!frame_stats_extended_enabled || (texture_handle == 0) || (texture_handle > FL_TEXTURE_MAX) || (w <= 0) ||
        (h <= 0) || (x < 0) || (y < 0)) {
        return;
    }

    const int texture_index = (int)texture_handle - 1;
    const SDL_Surface* source_surface = surfaces[texture_index];
    if ((source_surface != NULL) && ((x + w > source_surface->w) || (y + h > source_surface->h))) {
        return;
    }

    perf_capture_texture_renew_chunk_calls_by_texture[texture_index] += 1;
    perf_capture_texture_renew_chunk_pixels_by_texture[texture_index] += (Uint64)w * (Uint64)h;
    if ((w == 8) && (h == 8)) {
        perf_capture_texture_renew_chunk_8x8_calls_by_texture[texture_index] += 1;
    } else if ((w == 16) && (h == 16)) {
        perf_capture_texture_renew_chunk_16x16_calls_by_texture[texture_index] += 1;
    } else if ((w == 32) && (h == 32)) {
        perf_capture_texture_renew_chunk_32x32_calls_by_texture[texture_index] += 1;
    }
    perf_capture_texture_renew_pending_tile_masks[texture_index] |= make_perf_capture_renew_tile_mask(x, y, w, h);

    const SDL_Rect chunk_rect = { x, y, w, h };
    if (!perf_capture_texture_renew_pending_rect_valid[texture_index]) {
        perf_capture_texture_renew_pending_rects[texture_index] = chunk_rect;
        perf_capture_texture_renew_pending_rect_valid[texture_index] = true;
        return;
    }

    SDL_Rect* pending_rect = &perf_capture_texture_renew_pending_rects[texture_index];
    const int union_x0 = SDL_min(pending_rect->x, chunk_rect.x);
    const int union_y0 = SDL_min(pending_rect->y, chunk_rect.y);
    const int union_x1 = SDL_max(pending_rect->x + pending_rect->w - 1, chunk_rect.x + chunk_rect.w - 1);
    const int union_y1 = SDL_max(pending_rect->y + pending_rect->h - 1, chunk_rect.y + chunk_rect.h - 1);
    pending_rect->x = union_x0;
    pending_rect->y = union_y0;
    pending_rect->w = union_x1 - union_x0 + 1;
    pending_rect->h = union_y1 - union_y0 + 1;
}

void SDLGameRenderer_RecordTextureRenewBatch(unsigned int texture_handle) {
    if (!frame_stats_extended_enabled || (texture_handle == 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return;
    }

    const int texture_index = (int)texture_handle - 1;
    perf_capture_texture_renew_batches_by_texture[texture_index] += 1;
    if (!perf_capture_texture_renew_pending_rect_valid[texture_index]) {
        perf_capture_texture_renew_batches_without_rect_by_texture[texture_index] += 1;
        return;
    }

    const SDL_Rect pending_rect = perf_capture_texture_renew_pending_rects[texture_index];
    const Uint64 bbox_pixels = (Uint64)pending_rect.w * (Uint64)pending_rect.h;
    perf_capture_texture_renew_batch_bbox_pixels_by_texture[texture_index] += bbox_pixels;
    if (bbox_pixels > perf_capture_texture_renew_batch_max_bbox_pixels_by_texture[texture_index]) {
        perf_capture_texture_renew_batch_max_bbox_pixels_by_texture[texture_index] = bbox_pixels;
    }
    const Uint64 pending_tile_mask = perf_capture_texture_renew_pending_tile_masks[texture_index];
    const Uint64 covered_tiles = (Uint64)__builtin_popcountll((unsigned long long)pending_tile_mask);
    int largest_component_tiles = 0;
    const int component_count = count_perf_capture_renew_tile_components(pending_tile_mask, &largest_component_tiles);
    perf_capture_texture_renew_batch_32x32_covered_tiles_by_texture[texture_index] += covered_tiles;
    if (covered_tiles > perf_capture_texture_renew_batch_32x32_max_covered_tiles_by_texture[texture_index]) {
        perf_capture_texture_renew_batch_32x32_max_covered_tiles_by_texture[texture_index] = covered_tiles;
    }
    perf_capture_texture_renew_batch_32x32_component_count_by_texture[texture_index] += (Uint64)component_count;
    if ((Uint64)component_count > perf_capture_texture_renew_batch_32x32_max_component_count_by_texture[texture_index]) {
        perf_capture_texture_renew_batch_32x32_max_component_count_by_texture[texture_index] = (Uint64)component_count;
    }
    if (component_count > 1) {
        perf_capture_texture_renew_batch_32x32_multi_component_batches_by_texture[texture_index] += 1;
    }
    perf_capture_texture_renew_batch_32x32_largest_component_tiles_by_texture[texture_index] +=
        (Uint64)largest_component_tiles;
    if ((Uint64)largest_component_tiles >
        perf_capture_texture_renew_batch_32x32_max_largest_component_tiles_by_texture[texture_index]) {
        perf_capture_texture_renew_batch_32x32_max_largest_component_tiles_by_texture[texture_index] =
            (Uint64)largest_component_tiles;
    }
    perf_capture_texture_renew_pending_rects[texture_index] = (SDL_Rect){ 0, 0, 0, 0 };
    perf_capture_texture_renew_pending_rect_valid[texture_index] = false;
    perf_capture_texture_renew_pending_tile_masks[texture_index] = 0;
}

void SDLGameRenderer_ClearTextureUnlockDirtyRect(unsigned int texture_handle) {
    if ((texture_handle == 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return;
    }

    clear_texture_unlock_dirty_rect_index((int)texture_handle - 1, TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_EXPLICIT);
}

void SDLGameRenderer_UnlockPalette(unsigned int ph) {
    const int palette_handle = ph;
    const int palette_index = palette_handle - 1;
    const CacheDirtyReason dirty_reason = CACHE_DIRTY_REASON_PALETTE_UNLOCK;

    if ((palette_handle > 0) && (palette_handle <= FL_PALETTE_MAX)) {
        SDL_Color colors[256];
        int color_count = 0;
        fill_palette_colors_from_fl_texture(&flPalette[palette_index], colors, &color_count);
        const bool palette_changed =
            (palettes[palette_index] == NULL) || !palette_colors_equal(palettes[palette_index], colors, color_count);
        const CacheDirtyDetail dirty_detail =
            palette_changed ? CACHE_DIRTY_DETAIL_PALETTE_CHANGED : CACHE_DIRTY_DETAIL_PALETTE_UNCHANGED;
        // Unchanged palette colors keep existing cached pixels valid on the software-frame path, so avoid
        // converting them into dirty refresh work when the resulting texture/surface contents would be identical.
        const bool skip_cache_invalidation = software_frame_mode_active && !palette_changed;

        if (palettes[palette_index] == NULL) {
            SDLGameRenderer_CreatePalette(ph << 16);
        } else {
            SDL_SetPaletteColors(palettes[palette_index], colors, 0, color_count);
        }
        if (frame_stats_extended_enabled) {
            const Uint64 invalidation_start_ns = SDL_GetTicksNS();
            frame_stats.palette_unlock_calls += 1;
            if (palette_changed) {
                frame_stats.palette_unlock_changed_calls += 1;
            } else {
                frame_stats.palette_unlock_unchanged_calls += 1;
            }
            if (!skip_cache_invalidation) {
                frame_stats.palette_cache_evictions +=
                    invalidate_texture_cache_for_palette_handle(palette_handle, dirty_reason, dirty_detail);
                frame_stats.software_surface_cache_palette_evictions +=
                    invalidate_software_surface_cache_for_palette_handle(palette_handle, dirty_reason, dirty_detail);
            }
            frame_stats.palette_unlock_invalidation_ns += SDL_GetTicksNS() - invalidation_start_ns;
        } else {
            if (!skip_cache_invalidation) {
                invalidate_texture_cache_for_palette_handle(palette_handle, dirty_reason, dirty_detail);
                invalidate_software_surface_cache_for_palette_handle(palette_handle, dirty_reason, dirty_detail);
            }
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
        clear_texture_unlock_dirty_rect_if_unused(texture_index, TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_UNLOCK_UNUSED);
        if (texture_unlock_dirty_rect_valid[texture_index] || texture_unlock_dirty_tile_mask_valid[texture_index]) {
            note_perf_capture_dirty_rect_retained_after_unlock(texture_index);
        }
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
    clear_current_texture_logical_identity_slot(texture_index);
    clear_texture_unlock_dirty_rect_index(texture_index, TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_NONE);
    reset_perf_capture_refresh_lifetime_slot(texture_index);
    reset_perf_capture_unlock_locality_texture_slot(texture_index);
    reset_perf_capture_texture_renew_slot(texture_index);
}

void SDLGameRenderer_DestroyTexture(unsigned int texture_handle) {
    const int texture_index = texture_handle - 1;
    if (frame_stats_extended_enabled) {
        perf_capture_source_surface_destroy_calls_by_texture[texture_index] += 1;
    }
    invalidate_texture_cache_for_texture_index(texture_index, CACHE_DIRTY_REASON_NONE);
    invalidate_software_surface_cache_for_texture_index(texture_index, CACHE_DIRTY_REASON_NONE);
    clear_current_texture_logical_identity_slot(texture_index);
    clear_texture_unlock_dirty_rect_index(texture_index, TEXTURE_UNLOCK_DIRTY_RECT_CLEAR_REASON_NONE);
    reset_perf_capture_refresh_lifetime_slot(texture_index);
    reset_perf_capture_unlock_locality_texture_slot(texture_index);
    reset_perf_capture_texture_renew_slot(texture_index);

    SDL_DestroySurface(surfaces[texture_index]);
    surfaces[texture_index] = NULL;
}

void SDLGameRenderer_RecordTextureLogicalIdentity(unsigned int texture_handle,
                                                  SDLGameRenderer_TextureLogicalSourceKind source_kind,
                                                  int ix_num,
                                                  int ix_num_first,
                                                  int slot_index,
                                                  int chunk_index,
                                                  int texture_total) {
    if ((texture_handle == 0) || (texture_handle > FL_TEXTURE_MAX)) {
        return;
    }
    if (!perf_capture_logical_identity_enabled) {
        return;
    }

    const int texture_index = (int)texture_handle - 1;
    TextureLogicalIdentity* current_identity = &current_texture_logical_identity_by_texture[texture_index];
    *current_identity = (TextureLogicalIdentity){
        .source_kind = source_kind,
        .valid = (source_kind != SDL_GAME_RENDERER_TEXTURE_LOGICAL_SOURCE_UNKNOWN),
        .ix_num = ix_num,
        .ix_num_first = ix_num_first,
        .slot_index = slot_index,
        .chunk_index = chunk_index,
        .texture_total = texture_total,
    };

    current_texture_logical_identity_serial_by_texture[texture_index] += 1;
    if (current_texture_logical_identity_serial_by_texture[texture_index] == 0) {
        current_texture_logical_identity_serial_by_texture[texture_index] = 1;
    }
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
    invalidate_texture_cache_for_palette_handle(
        (int)palette_handle, CACHE_DIRTY_REASON_NONE, CACHE_DIRTY_DETAIL_NONE);
    invalidate_software_surface_cache_for_palette_handle(
        (int)palette_handle, CACHE_DIRTY_REASON_NONE, CACHE_DIRTY_DETAIL_NONE);

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
            // Fast path: if the software surface cache also has a clean entry for this
            // texture+palette, grab it now to avoid calling get_or_create_software_source_surface
            // which redundantly looks up the same arrays and dirty state.
            if (software_frame_mode_active) {
                SDL_Surface* cached_sw = software_surface_cache[texture_handle - 1][palette_handle];
                if ((cached_sw != NULL) &&
                    (software_surface_cache_runtime_dirty_reason[texture_handle - 1][palette_handle] ==
                     CACHE_DIRTY_REASON_NONE)) {
                    software_source_surface = cached_sw;
                }
            }
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
