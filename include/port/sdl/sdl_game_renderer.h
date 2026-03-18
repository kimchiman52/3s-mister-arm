#ifndef SDL_GAME_RENDERER_H
#define SDL_GAME_RENDERER_H

#include "port/build_config.h"
#include "structs.h"
#include <SDL3/SDL.h>

typedef struct SDLGameRenderer_Vertex {
    struct {
        float x;
        float y;
        float z;
        float w;
    } coord;
    unsigned int color;
    TexCoord tex_coord;
} SDLGameRenderer_Vertex;

typedef struct Quad {
    Vec3 v[4];
} Quad;

typedef struct Sprite {
    Vec3 v[4];
    TexCoord t[4];
    unsigned int tex_code;
} Sprite;

typedef struct Sprite2 {
    Vec3 v[2];
    TexCoord t[2];
    unsigned int vertex_color;
    unsigned int tex_code;
    unsigned int id;
} Sprite2;

extern SDL_Texture* cps3_canvas;

typedef enum SDLGameRenderer_SortStrategy {
    SDL_GAME_RENDERER_SORT_NONE = 0,
    SDL_GAME_RENDERER_SORT_EQUAL_Z_REVERSE = 1,
    SDL_GAME_RENDERER_SORT_INSERTION = 2,
    SDL_GAME_RENDERER_SORT_QSORT = 3,
} SDLGameRenderer_SortStrategy;

typedef enum SDLGameRenderer_TaskSource {
    SDL_GAME_RENDERER_TASK_SOURCE_UNKNOWN = 0,
    SDL_GAME_RENDERER_TASK_SOURCE_PPG = 1,
    SDL_GAME_RENDERER_TASK_SOURCE_MTRANS = 2,
    SDL_GAME_RENDERER_TASK_SOURCE_UI_DIRECT = 3,
    SDL_GAME_RENDERER_TASK_SOURCE_SOLID = 4,
} SDLGameRenderer_TaskSource;

typedef struct SDLGameRenderer_FrameStats {
    int render_task_count;
    int software_frame_mode_enabled;
    int software_frame_surface_ready;
    int software_frame_owned;
    int software_frame_direct_present;
    int software_frame_uploaded;
    int software_frame_fallback;
    int software_frame_candidate_tasks;
    Uint64 software_frame_candidate_pixels;
    int software_frame_fallback_tasks;
    Uint64 software_frame_fallback_pixels;
    int software_frame_fast_exact_tasks;
    Uint64 software_frame_fast_exact_pixels;
    int software_frame_fast_exact_clipped_tasks;
    int software_frame_fast_exact_flipped_tasks;
    int software_frame_fast_exact_color_mod_tasks;
    Uint64 software_frame_fast_exact_color_mod_pixels;
    int software_frame_fast_scaled_tasks;
    Uint64 software_frame_fast_scaled_pixels;
    int software_frame_fast_non_integer_tasks;
    Uint64 software_frame_fast_non_integer_pixels;
    Uint64 software_frame_fast_non_integer_lookup_entries;
    int software_frame_fast_non_integer_alpha_only_tasks;
    Uint64 software_frame_fast_non_integer_alpha_only_pixels;
    int software_frame_fast_non_integer_rgb_mod_tasks;
    Uint64 software_frame_fast_non_integer_rgb_mod_pixels;
    int software_frame_generic_textured_tasks;
    Uint64 software_frame_generic_textured_pixels;
    int software_frame_generic_textured_alpha_only_tasks;
    Uint64 software_frame_generic_textured_alpha_only_pixels;
    int software_frame_generic_textured_rgb_mod_tasks;
    Uint64 software_frame_generic_textured_rgb_mod_pixels;
    int software_frame_fast_miss_color_mod;
    int software_frame_fast_miss_non_integer;
    Uint64 software_frame_fast_miss_non_integer_lookup_entries;
    int software_frame_fast_miss_non_integer_ge_256_tasks;
    Uint64 software_frame_fast_miss_non_integer_ge_256_pixels;
    Uint64 software_frame_fast_miss_non_integer_ge_256_lookup_entries;
    int software_frame_fast_miss_non_integer_ge_1024_tasks;
    Uint64 software_frame_fast_miss_non_integer_ge_1024_pixels;
    Uint64 software_frame_fast_miss_non_integer_max_pixels;
    int software_frame_fast_miss_scaled;
    int software_frame_fast_miss_unsupported_flip;
    int software_frame_fast_miss_source_bounds;
    int software_frame_reason_alpha;
    int software_frame_reason_color_mod;
    int software_frame_reason_geometry;
    int software_frame_reason_solid;
    int rect_copy_tasks;
    int batch_runs;
    int batched_task_count;
    int rect_texture_runs;
    int rect_texture_multi_runs;
    int rect_texture_multi_run_tasks;
    int rect_texture_max_run;
    int rect_texture_hstrip_runs;
    int rect_texture_hstrip_tasks;
    int rect_texture_vstrip_runs;
    int rect_texture_vstrip_tasks;
    int rect_texture_run_links;
    int rect_texture_color_breaks;
    int rect_texture_flip_breaks;
    int rect_texture_flipped_tasks;
    int textured_geometry_tasks;
    int textured_geometry_rect_recovered_tasks;
    int textured_geometry_fallback_tasks;
    int set_texture_calls;
    int texture_binding_reuse_hits;
    int texture_cache_hits;
    int texture_cache_misses;
    int texture_cache_miss_dirty_texture_same_frame;
    int texture_cache_miss_dirty_texture_carried;
    int texture_cache_miss_dirty_palette_same_frame;
    int texture_cache_miss_dirty_palette_carried;
    int texture_cache_miss_cold;
    int texture_creates;
    int texture_unlock_calls;
    int palette_unlock_calls;
    int palette_unlock_changed_calls;
    int palette_unlock_unchanged_calls;
    int texture_unlock_dirty_surface_variants;
    int texture_unlock_dirty_surface_variants_max;
    int palette_unlock_dirty_surface_variants;
    int palette_unlock_dirty_surface_variants_max;
    int texture_unlock_locality_index8_tracked;
    int texture_unlock_locality_index8_baseline_skips;
    int texture_unlock_locality_index8_non_index8_skips;
    Uint64 texture_unlock_locality_index8_source_pixels;
    Uint64 texture_unlock_locality_index8_changed_pixels;
    Uint64 texture_unlock_locality_index8_changed_rows;
    Uint64 texture_unlock_locality_index8_changed_bbox_pixels;
    Uint64 texture_unlock_invalidation_ns;
    Uint64 palette_unlock_invalidation_ns;
    int texture_cache_evictions;
    int palette_cache_evictions;
    int software_surface_cache_hits;
    int software_surface_cache_creates;
    int software_surface_cache_refresh_attempts;
    int software_surface_cache_refresh_unique_bindings;
    int software_surface_cache_refresh_repeat_binding_attempts;
    int software_surface_cache_refresh_unique_texture_handles;
    int software_surface_cache_refresh_texture_handle_fanout_max;
    int software_surface_cache_refresh_failures;
    Uint64 software_surface_cache_refresh_ns;
    int software_surface_cache_refresh_palette_set_calls;
    Uint64 software_surface_cache_refresh_palette_set_ns;
    int software_surface_cache_refresh_blit_calls;
    Uint64 software_surface_cache_refresh_blit_ns;
    int software_surface_cache_create_dirty_texture_same_frame;
    int software_surface_cache_create_dirty_texture_carried;
    int software_surface_cache_create_dirty_palette_same_frame;
    int software_surface_cache_create_dirty_palette_carried;
    int software_surface_cache_create_cold;
    int software_surface_cache_texture_evictions;
    int software_surface_cache_palette_evictions;
    int textures_destroy_queued;
    int unknown_tasks;
    int ppg_tasks;
    int mtrans_tasks;
    int ui_direct_tasks;
    int solid_tasks;
    int hybrid_candidate_tasks;
    Uint64 hybrid_candidate_pixels;
    int hybrid_fallback_tasks;
    Uint64 hybrid_fallback_pixels;
    int hybrid_reason_clip;
    int hybrid_reason_alpha;
    int hybrid_reason_color_mod;
    int hybrid_reason_flip;
    int hybrid_reason_geometry;
    int hybrid_reason_solid;
    SDLGameRenderer_SortStrategy sort_strategy;
} SDLGameRenderer_FrameStats;

typedef struct SDLGameRenderer_PerfCaptureRefreshTelemetry {
    Uint64 index4_attempts;
    Uint64 index4_pixels;
    Uint64 index8_attempts;
    Uint64 index8_pixels;
    Uint64 abgr1555_attempts;
    Uint64 abgr1555_pixels;
    Uint64 other_attempts;
    Uint64 other_pixels;
    Uint64 partial_attempts;
    Uint64 partial_pixels;
    Uint64 full_attempts;
    Uint64 full_pixels;
    Uint64 full_non_texture_dirty_attempts;
    Uint64 full_ineligible_source_attempts;
    Uint64 full_no_usable_dirty_rect_attempts;
    Uint64 full_oversized_dirty_rect_attempts;
    Uint64 sampled_blit_period;
    Uint64 sampled_blit_calls;
    Uint64 sampled_blit_ns;
    Uint64 sampled_full_blit_calls;
    Uint64 sampled_full_blit_ns;
    Uint64 sampled_partial_blit_calls;
    Uint64 sampled_partial_blit_ns;
    Uint64 sampled_full_non_texture_dirty_blit_calls;
    Uint64 sampled_full_non_texture_dirty_blit_ns;
    Uint64 sampled_full_ineligible_source_blit_calls;
    Uint64 sampled_full_ineligible_source_blit_ns;
    Uint64 sampled_full_no_usable_dirty_rect_blit_calls;
    Uint64 sampled_full_no_usable_dirty_rect_blit_ns;
    Uint64 sampled_full_oversized_dirty_rect_blit_calls;
    Uint64 sampled_full_oversized_dirty_rect_blit_ns;
} SDLGameRenderer_PerfCaptureRefreshTelemetry;

typedef enum SDLGameRenderer_TextureLogicalSourceKind {
    SDL_GAME_RENDERER_TEXTURE_LOGICAL_SOURCE_UNKNOWN = 0,
    SDL_GAME_RENDERER_TEXTURE_LOGICAL_SOURCE_PPG_SEQS,
    SDL_GAME_RENDERER_TEXTURE_LOGICAL_SOURCE_PPG_CHUNK,
} SDLGameRenderer_TextureLogicalSourceKind;

typedef struct SDLGameRenderer_PerfCaptureRefreshHotTexture {
    int texture_handle;
    Uint32 source_format;
    int width;
    int height;
    int source_shape_mixed;
    int logical_identity_known;
    int logical_identity_mixed;
    Uint32 logical_identity_registrations;
    SDLGameRenderer_TextureLogicalSourceKind logical_source_kind;
    int logical_ix_num;
    int logical_ix_num_first;
    int logical_slot_index;
    int logical_chunk_index;
    int logical_texture_total;
    Uint64 source_surface_destroy_calls;
    Uint64 current_lifetime_refresh_attempts;
    Uint64 current_lifetime_partial_refresh_attempts;
    Uint64 current_lifetime_full_refresh_attempts;
    Uint64 current_lifetime_full_refresh_no_usable_dirty_rect_attempts;
    Uint64 refresh_attempts;
    Uint64 refresh_pixels;
    int max_fanout;
} SDLGameRenderer_PerfCaptureRefreshHotTexture;

typedef struct SDLGameRenderer_PerfCaptureUnlockLocalityTelemetry {
    Uint64 index8_tracked_unlocks;
    Uint64 index8_zero_delta_unlocks;
    Uint64 index8_baseline_skips;
    Uint64 index8_non_index8_skips;
    Uint64 index8_source_pixels;
    Uint64 index8_changed_pixels;
    Uint64 index8_changed_rows;
    Uint64 index8_changed_bbox_pixels;
} SDLGameRenderer_PerfCaptureUnlockLocalityTelemetry;

typedef struct SDLGameRenderer_PerfCaptureUnlockLocalityHotTexture {
    int texture_handle;
    Uint32 source_format;
    int width;
    int height;
    int source_shape_mixed;
    int logical_identity_known;
    int logical_identity_mixed;
    Uint32 logical_identity_registrations;
    SDLGameRenderer_TextureLogicalSourceKind logical_source_kind;
    int logical_ix_num;
    int logical_ix_num_first;
    int logical_slot_index;
    int logical_chunk_index;
    int logical_texture_total;
    Uint64 tracked_unlocks;
    Uint64 zero_delta_unlocks;
    Uint64 baseline_skips;
    Uint64 non_index8_skips;
    Uint64 source_pixels;
    Uint64 changed_pixels;
    Uint64 changed_rows;
    Uint64 changed_bbox_pixels;
} SDLGameRenderer_PerfCaptureUnlockLocalityHotTexture;

typedef struct SDLGameRenderer_PerfCaptureDirtyRectLifetimeTelemetry {
    Uint64 record_calls;
    Uint64 retained_after_unlock;
    Uint64 clear_stale_before_record;
    Uint64 clear_unlock_unused;
    Uint64 clear_access_unused;
    Uint64 clear_explicit;
} SDLGameRenderer_PerfCaptureDirtyRectLifetimeTelemetry;

typedef struct SDLGameRenderer_PerfCaptureTextureRenewTelemetry {
    Uint64 renew_chunk_calls;
    Uint64 renew_batches;
    Uint64 renew_batches_without_rect;
    Uint64 renew_chunk_pixels;
    Uint64 renew_batch_bbox_pixels;
    Uint64 renew_batch_max_bbox_pixels;
    Uint64 renew_chunk_8x8_calls;
    Uint64 renew_chunk_16x16_calls;
    Uint64 renew_chunk_32x32_calls;
    Uint64 renew_batch_32x32_covered_tiles;
    Uint64 renew_batch_32x32_max_covered_tiles;
    Uint64 renew_batch_32x32_component_count;
    Uint64 renew_batch_32x32_max_component_count;
    Uint64 renew_batch_32x32_multi_component_batches;
    Uint64 renew_batch_32x32_largest_component_tiles;
    Uint64 renew_batch_32x32_max_largest_component_tiles;
} SDLGameRenderer_PerfCaptureTextureRenewTelemetry;

typedef enum SDLGameRenderer_PerfCaptureRasterBucket {
    SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_FAST_EXACT = 0,
    SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_FAST_SCALED,
    SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_FAST_NON_INTEGER,
    SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED,
    SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_SOLID,
    SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT,
} SDLGameRenderer_PerfCaptureRasterBucket;

typedef struct SDLGameRenderer_PerfCaptureRasterBucketTiming {
    SDLGameRenderer_PerfCaptureRasterBucket bucket;
    Uint64 sample_period;
    Uint64 sampled_calls;
    Uint64 sampled_pixels;
    Uint64 sampled_ns;
} SDLGameRenderer_PerfCaptureRasterBucketTiming;

typedef enum SDLGameRenderer_TexturedGeometryFallbackFamilyKind {
    SDL_GAME_RENDERER_TEXTURED_GEOMETRY_FALLBACK_FAMILY_OTHER = 0,
    SDL_GAME_RENDERER_TEXTURED_GEOMETRY_FALLBACK_FAMILY_RECT_UV_OTHER,
    SDL_GAME_RENDERER_TEXTURED_GEOMETRY_FALLBACK_FAMILY_RECT_UV_TRAPEZOID,
    SDL_GAME_RENDERER_TEXTURED_GEOMETRY_FALLBACK_FAMILY_RECT_UV_PARALLELOGRAM,
} SDLGameRenderer_TexturedGeometryFallbackFamilyKind;

typedef struct SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily {
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
    SDLGameRenderer_TexturedGeometryFallbackFamilyKind family_kind;
    int uniform_color;
    int opaque_color;
    int rgb_mod;
    int integer_positions;
    int integer_source_rect;
    int full_texture_source_rect;
    Uint64 task_count;
    Uint64 submitted_pixels;
    int source_x_min;
    int source_x_max;
    int source_y_min;
    int source_y_max;
    int source_w_min;
    int source_w_max;
    int source_h_min;
    int source_h_max;
    int dst_height_min;
    int dst_height_max;
    int dst_top_width_min;
    int dst_top_width_max;
    int dst_bottom_width_min;
    int dst_bottom_width_max;
    int dst_left_dx_min;
    int dst_left_dx_max;
    int dst_right_dx_min;
    int dst_right_dx_max;
} SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily;

typedef struct SDLGameRenderer_PerfCaptureTexturedRectFamily {
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
    int integer_positions;
    int integer_source_rect;
    int full_texture_source_rect;
    int clipped;
    int flip_h;
    int flip_v;
    Uint64 task_count;
    Uint64 submitted_pixels;
    Uint64 lookup_entries;
    int source_x_min;
    int source_x_max;
    int source_y_min;
    int source_y_max;
    int source_w_min;
    int source_w_max;
    int source_h_min;
    int source_h_max;
    int dst_x_min;
    int dst_x_max;
    int dst_y_min;
    int dst_y_max;
    int dst_w_min;
    int dst_w_max;
    int dst_h_min;
    int dst_h_max;
    int visible_w_min;
    int visible_w_max;
    int visible_h_min;
    int visible_h_max;
} SDLGameRenderer_PerfCaptureTexturedRectFamily;

typedef struct SDLGameRenderer_PerfCaptureRefreshLocalityCandidate {
    int texture_handle;
    Uint32 source_format;
    int width;
    int height;
    int source_shape_mixed;
    int logical_identity_known;
    int logical_identity_mixed;
    Uint32 logical_identity_registrations;
    SDLGameRenderer_TextureLogicalSourceKind logical_source_kind;
    int logical_ix_num;
    int logical_ix_num_first;
    int logical_slot_index;
    int logical_chunk_index;
    int logical_texture_total;
    Uint64 source_surface_destroy_calls;
    Uint64 refresh_attempts;
    Uint64 refresh_pixels;
    int max_fanout;
    Uint64 current_lifetime_refresh_attempts;
    Uint64 current_lifetime_partial_refresh_attempts;
    Uint64 current_lifetime_full_refresh_attempts;
    Uint64 current_lifetime_full_refresh_no_usable_dirty_rect_attempts;
    Uint64 partial_refresh_attempts;
    Uint64 partial_refresh_pixels;
    Uint64 full_refresh_attempts;
    Uint64 full_refresh_pixels;
    Uint64 full_refresh_non_texture_dirty_attempts;
    Uint64 full_refresh_ineligible_source_attempts;
    Uint64 full_refresh_no_usable_dirty_rect_attempts;
    Uint64 full_refresh_oversized_dirty_rect_attempts;
    Uint64 sampled_blit_calls;
    Uint64 sampled_blit_ns;
    Uint64 sampled_full_blit_calls;
    Uint64 sampled_full_blit_ns;
    Uint64 sampled_partial_blit_calls;
    Uint64 sampled_partial_blit_ns;
    Uint64 sampled_full_non_texture_dirty_blit_calls;
    Uint64 sampled_full_non_texture_dirty_blit_ns;
    Uint64 sampled_full_ineligible_source_blit_calls;
    Uint64 sampled_full_ineligible_source_blit_ns;
    Uint64 sampled_full_no_usable_dirty_rect_blit_calls;
    Uint64 sampled_full_no_usable_dirty_rect_blit_ns;
    Uint64 sampled_full_oversized_dirty_rect_blit_calls;
    Uint64 sampled_full_oversized_dirty_rect_blit_ns;
    Uint64 software_surface_access_dirty_texture_same_frame;
    Uint64 software_surface_access_dirty_texture_carried;
    Uint64 software_surface_access_dirty_palette_same_frame;
    Uint64 software_surface_access_dirty_palette_carried;
    Uint64 software_surface_access_dirty_palette_changed_same_frame;
    Uint64 software_surface_access_dirty_palette_changed_carried;
    Uint64 software_surface_access_dirty_palette_unchanged_same_frame;
    Uint64 software_surface_access_dirty_palette_unchanged_carried;
    Uint64 software_surface_access_cold;
    Uint64 current_lifetime_software_surface_access_dirty_texture_same_frame;
    Uint64 current_lifetime_software_surface_access_dirty_texture_carried;
    Uint64 current_lifetime_software_surface_access_dirty_palette_same_frame;
    Uint64 current_lifetime_software_surface_access_dirty_palette_carried;
    Uint64 current_lifetime_software_surface_access_dirty_palette_changed_same_frame;
    Uint64 current_lifetime_software_surface_access_dirty_palette_changed_carried;
    Uint64 current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame;
    Uint64 current_lifetime_software_surface_access_dirty_palette_unchanged_carried;
    Uint64 current_lifetime_software_surface_access_cold;
    Uint64 tracked_unlocks;
    Uint64 zero_delta_unlocks;
    Uint64 baseline_skips;
    Uint64 non_index8_skips;
    Uint64 source_pixels;
    Uint64 changed_pixels;
    Uint64 changed_rows;
    Uint64 changed_bbox_pixels;
    Uint64 whole_capture_tracked_unlocks;
    Uint64 whole_capture_zero_delta_unlocks;
    Uint64 whole_capture_baseline_skips;
    Uint64 whole_capture_non_index8_skips;
    Uint64 whole_capture_source_pixels;
    Uint64 whole_capture_changed_pixels;
    Uint64 whole_capture_changed_rows;
    Uint64 whole_capture_changed_bbox_pixels;
    Uint64 dirty_rect_record_calls;
    Uint64 dirty_rect_retained_after_unlock;
    Uint64 dirty_rect_clear_stale_before_record;
    Uint64 dirty_rect_clear_unlock_unused;
    Uint64 dirty_rect_clear_access_unused;
    Uint64 dirty_rect_clear_explicit;
    Uint64 compare_dirty_rect_refresh_attempts;
    Uint64 compare_dirty_rect_partial_candidate_refresh_attempts;
    Uint64 compare_dirty_rect_oversized_candidate_refresh_attempts;
    Uint64 compare_dirty_rect_no_usable_candidate_refresh_attempts;
    Uint64 compare_dirty_rect_refresh_bbox_pixels;
    Uint64 compare_dirty_rect_refresh_max_bbox_pixels;
    Uint64 compare_dirty_rect_refresh_pending_unlocks;
    Uint64 compare_dirty_rect_refresh_max_pending_unlocks;
    Uint64 compare_dirty_rect_refresh_32x32_covered_tiles;
    Uint64 compare_dirty_rect_refresh_32x32_max_covered_tiles;
    Uint64 compare_dirty_rect_refresh_32x32_component_count;
    Uint64 compare_dirty_rect_refresh_32x32_max_component_count;
    Uint64 compare_dirty_rect_refresh_32x32_multi_component_refresh_attempts;
    Uint64 compare_dirty_rect_refresh_32x32_largest_component_tiles;
    Uint64 compare_dirty_rect_refresh_32x32_max_largest_component_tiles;
    Uint64 renew_chunk_calls;
    Uint64 renew_batches;
    Uint64 renew_batches_without_rect;
    Uint64 renew_chunk_pixels;
    Uint64 renew_batch_bbox_pixels;
    Uint64 renew_batch_max_bbox_pixels;
    Uint64 renew_chunk_8x8_calls;
    Uint64 renew_chunk_16x16_calls;
    Uint64 renew_chunk_32x32_calls;
    Uint64 renew_batch_32x32_covered_tiles;
    Uint64 renew_batch_32x32_max_covered_tiles;
    Uint64 renew_batch_32x32_component_count;
    Uint64 renew_batch_32x32_max_component_count;
    Uint64 renew_batch_32x32_multi_component_batches;
    Uint64 renew_batch_32x32_largest_component_tiles;
    Uint64 renew_batch_32x32_max_largest_component_tiles;
} SDLGameRenderer_PerfCaptureRefreshLocalityCandidate;

void SDLGameRenderer_Init(SDL_Renderer* renderer);
void SDLGameRenderer_SetSoftwareFrameMode(bool enabled);
void SDLGameRenderer_SetSoftwareFrameDirectPresentMode(bool enabled);
bool SDLGameRenderer_IsSoftwareFrameModeEnabled(void);
bool SDLGameRenderer_IsPerfCaptureExtendedStatsEnabled(void);
bool SDLGameRenderer_HasSoftwareOwnedFrame(void);
const SDL_Surface* SDLGameRenderer_GetSoftwareFrameSurface(void);
bool SDLGameRenderer_EnsureSoftwareFrameCanvas(void);
void SDLGameRenderer_NoteSoftwareFrameDirectPresent(void);
void SDLGameRenderer_SetPerfCaptureLogicalIdentityEnabled(bool enabled);
void SDLGameRenderer_BeginFrame(bool capture_extended_stats);
void SDLGameRenderer_RenderFrame();
void SDLGameRenderer_EndFrame();
bool SDLGameRenderer_RunSoftwareFrameParityCheck(void);
void SDLGameRenderer_ResetPerfCaptureRefreshTelemetry(void);
void SDLGameRenderer_ResetPerfCaptureUnlockLocalityTelemetry(void);
void SDLGameRenderer_ResetPerfCaptureTextureRenewTelemetry(void);
void SDLGameRenderer_ResetPerfCaptureRasterTimingTelemetry(void);
void SDLGameRenderer_ResetPerfCaptureFastNonIntegerFamilyTelemetry(void);
void SDLGameRenderer_ResetPerfCaptureGenericTexturedFamilyTelemetry(void);
void SDLGameRenderer_ResetPerfCaptureTexturedGeometryRecoveredTelemetry(void);
void SDLGameRenderer_ResetPerfCaptureTexturedGeometryFallbackTelemetry(void);
int SDLGameRenderer_GetPerfCaptureRefreshTelemetry(SDLGameRenderer_PerfCaptureRefreshTelemetry* out_telemetry,
                                                   SDLGameRenderer_PerfCaptureRefreshHotTexture* out_hot_textures,
                                                   int max_hot_textures);
int SDLGameRenderer_GetPerfCaptureUnlockLocalityTelemetry(
    SDLGameRenderer_PerfCaptureUnlockLocalityTelemetry* out_telemetry,
    SDLGameRenderer_PerfCaptureUnlockLocalityHotTexture* out_hot_textures,
    int max_hot_textures);
void SDLGameRenderer_GetPerfCaptureDirtyRectLifetimeTelemetry(
    SDLGameRenderer_PerfCaptureDirtyRectLifetimeTelemetry* out_telemetry);
void SDLGameRenderer_GetPerfCaptureTextureRenewTelemetry(
    SDLGameRenderer_PerfCaptureTextureRenewTelemetry* out_telemetry);
int SDLGameRenderer_GetPerfCaptureRasterBucketTimings(
    SDLGameRenderer_PerfCaptureRasterBucketTiming* out_timings,
    int max_timings);
int SDLGameRenderer_GetPerfCaptureFastNonIntegerFamilies(SDLGameRenderer_PerfCaptureTexturedRectFamily* out_families,
                                                         int max_families);
void SDLGameRenderer_GetPerfCaptureFastNonIntegerFamilyTotals(Uint64* out_task_total,
                                                              Uint64* out_pixel_total,
                                                              Uint64* out_lookup_entry_total,
                                                              int* out_family_count);
int SDLGameRenderer_GetPerfCaptureGenericTexturedFamilies(SDLGameRenderer_PerfCaptureTexturedRectFamily* out_families,
                                                          int max_families);
void SDLGameRenderer_GetPerfCaptureGenericTexturedFamilyTotals(Uint64* out_task_total,
                                                               Uint64* out_pixel_total,
                                                               Uint64* out_lookup_entry_total,
                                                               int* out_family_count);
int SDLGameRenderer_GetPerfCaptureTexturedGeometryRecoveredFamilies(
    SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* out_families,
    int max_families);
void SDLGameRenderer_GetPerfCaptureTexturedGeometryRecoveredTotals(Uint64* out_task_total,
                                                                   Uint64* out_pixel_total,
                                                                   int* out_family_count);
int SDLGameRenderer_GetPerfCaptureTexturedGeometryFallbackFamilies(
    SDLGameRenderer_PerfCaptureTexturedGeometryFallbackFamily* out_families,
    int max_families);
void SDLGameRenderer_GetPerfCaptureTexturedGeometryFallbackTotals(Uint64* out_task_total,
                                                                  Uint64* out_pixel_total,
                                                                  int* out_family_count);
int SDLGameRenderer_GetPerfCaptureRefreshLocalityCandidates(
    SDLGameRenderer_PerfCaptureRefreshLocalityCandidate* out_candidates,
    int max_candidates);

int SDLGameRenderer_GetDirtyTileCount(void);
int SDLGameRenderer_GetDirtyTileTotal(void);
const Uint8* SDLGameRenderer_GetDirtyTileMap(int* out_cols, int* out_rows, int* out_tile_size);
void SDLGameRenderer_GetFrameStats(SDLGameRenderer_FrameStats* out_stats);

void SDLGameRenderer_CreateTexture(unsigned int th);
void SDLGameRenderer_DestroyTexture(unsigned int texture_handle);
void SDLGameRenderer_UnlockTexture(unsigned int th);
void SDLGameRenderer_RecordTextureLogicalIdentity(unsigned int texture_handle,
                                                  SDLGameRenderer_TextureLogicalSourceKind source_kind,
                                                  int ix_num,
                                                  int ix_num_first,
                                                  int slot_index,
                                                  int chunk_index,
                                                  int texture_total);
void SDLGameRenderer_RecordTextureUnlockDirtyRect(unsigned int texture_handle,
                                                  int min_x,
                                                  int min_y,
                                                  int max_x,
                                                  int max_y);
void SDLGameRenderer_RecordTextureUnlockDirtyTileMask(unsigned int texture_handle, Uint64 tile_mask);
void SDLGameRenderer_RecordTextureRenewChunk(unsigned int texture_handle, int x, int y, int w, int h);
void SDLGameRenderer_RecordTextureRenewBatch(unsigned int texture_handle);
void SDLGameRenderer_ClearTextureUnlockDirtyRect(unsigned int texture_handle);
void SDLGameRenderer_CreatePalette(unsigned int ph);
void SDLGameRenderer_DestroyPalette(unsigned int palette_handle);
void SDLGameRenderer_UnlockPalette(unsigned int ph);
void SDLGameRenderer_SetTexture(unsigned int th);
void SDLGameRenderer_SetTaskSource(SDLGameRenderer_TaskSource source);
void SDLGameRenderer_DrawTexturedQuad(const Sprite* sprite, unsigned int color);
void SDLGameRenderer_DrawSolidQuad(const Quad* vertices, unsigned int color);
void SDLGameRenderer_DrawSprite(const Sprite* sprite, unsigned int color);
void SDLGameRenderer_DrawSprite2(const Sprite2* sprite2);

#endif
