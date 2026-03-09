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
    int software_frame_generic_textured_tasks;
    Uint64 software_frame_generic_textured_pixels;
    int software_frame_fast_miss_color_mod;
    int software_frame_fast_miss_non_integer;
    int software_frame_fast_miss_non_integer_ge_256_tasks;
    Uint64 software_frame_fast_miss_non_integer_ge_256_pixels;
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
} SDLGameRenderer_PerfCaptureRefreshTelemetry;

typedef struct SDLGameRenderer_PerfCaptureRefreshHotTexture {
    int texture_handle;
    Uint32 source_format;
    int width;
    int height;
    int source_shape_mixed;
    Uint64 refresh_attempts;
    Uint64 refresh_pixels;
    int max_fanout;
} SDLGameRenderer_PerfCaptureRefreshHotTexture;

typedef struct SDLGameRenderer_PerfCaptureUnlockLocalityTelemetry {
    Uint64 index8_tracked_unlocks;
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
    Uint64 tracked_unlocks;
    Uint64 baseline_skips;
    Uint64 non_index8_skips;
    Uint64 source_pixels;
    Uint64 changed_pixels;
    Uint64 changed_rows;
    Uint64 changed_bbox_pixels;
} SDLGameRenderer_PerfCaptureUnlockLocalityHotTexture;

void SDLGameRenderer_Init(SDL_Renderer* renderer);
void SDLGameRenderer_SetSoftwareFrameMode(bool enabled);
void SDLGameRenderer_SetSoftwareFrameDirectPresentMode(bool enabled);
bool SDLGameRenderer_IsSoftwareFrameModeEnabled(void);
bool SDLGameRenderer_HasSoftwareOwnedFrame(void);
const SDL_Surface* SDLGameRenderer_GetSoftwareFrameSurface(void);
bool SDLGameRenderer_EnsureSoftwareFrameCanvas(void);
void SDLGameRenderer_NoteSoftwareFrameDirectPresent(void);
void SDLGameRenderer_BeginFrame(bool capture_extended_stats);
void SDLGameRenderer_RenderFrame();
void SDLGameRenderer_EndFrame();
bool SDLGameRenderer_RunSoftwareFrameParityCheck(void);
void SDLGameRenderer_ResetPerfCaptureRefreshTelemetry(void);
void SDLGameRenderer_ResetPerfCaptureUnlockLocalityTelemetry(void);
int SDLGameRenderer_GetPerfCaptureRefreshTelemetry(SDLGameRenderer_PerfCaptureRefreshTelemetry* out_telemetry,
                                                   SDLGameRenderer_PerfCaptureRefreshHotTexture* out_hot_textures,
                                                   int max_hot_textures);
int SDLGameRenderer_GetPerfCaptureUnlockLocalityTelemetry(
    SDLGameRenderer_PerfCaptureUnlockLocalityTelemetry* out_telemetry,
    SDLGameRenderer_PerfCaptureUnlockLocalityHotTexture* out_hot_textures,
    int max_hot_textures);

int SDLGameRenderer_GetDirtyTileCount(void);
int SDLGameRenderer_GetDirtyTileTotal(void);
const Uint8* SDLGameRenderer_GetDirtyTileMap(int* out_cols, int* out_rows, int* out_tile_size);
void SDLGameRenderer_GetFrameStats(SDLGameRenderer_FrameStats* out_stats);

void SDLGameRenderer_CreateTexture(unsigned int th);
void SDLGameRenderer_DestroyTexture(unsigned int texture_handle);
void SDLGameRenderer_UnlockTexture(unsigned int th);
void SDLGameRenderer_RecordTextureUnlockDirtyRect(unsigned int texture_handle,
                                                  int min_x,
                                                  int min_y,
                                                  int max_x,
                                                  int max_y);
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
