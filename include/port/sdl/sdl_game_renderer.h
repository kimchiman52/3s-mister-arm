#ifndef SDL_GAME_RENDERER_H
#define SDL_GAME_RENDERER_H

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
    int texture_creates;
    int texture_unlock_calls;
    int palette_unlock_calls;
    int texture_cache_evictions;
    int palette_cache_evictions;
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

void SDLGameRenderer_Init(SDL_Renderer* renderer);
void SDLGameRenderer_BeginFrame(bool capture_extended_stats);
void SDLGameRenderer_RenderFrame();
void SDLGameRenderer_EndFrame();

int SDLGameRenderer_GetDirtyTileCount(void);
int SDLGameRenderer_GetDirtyTileTotal(void);
const Uint8* SDLGameRenderer_GetDirtyTileMap(int* out_cols, int* out_rows, int* out_tile_size);
void SDLGameRenderer_GetFrameStats(SDLGameRenderer_FrameStats* out_stats);

void SDLGameRenderer_CreateTexture(unsigned int th);
void SDLGameRenderer_DestroyTexture(unsigned int texture_handle);
void SDLGameRenderer_UnlockTexture(unsigned int th);
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
