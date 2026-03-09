#include "port/sdl/sdl_game_renderer.h"
#include "common.h"
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

typedef enum RenderTaskType {
    RENDER_TASK_TYPE_GEOMETRY = 0,
    RENDER_TASK_TYPE_TEXTURED_RECT = 1,
} RenderTaskType;

typedef struct RenderTask {
    SDL_Texture* texture;
    RenderTaskType type;
    SDLGameRenderer_TaskSource source;
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
enum {
    dirty_tile_size = 16,
    dirty_tile_cols = 24,
    dirty_tile_rows = 14,
    dirty_tile_total = dirty_tile_cols * dirty_tile_rows,
};

static SDL_Renderer* _renderer = NULL;
static SDL_Surface* surfaces[FL_TEXTURE_MAX] = { NULL };
static SDL_Palette* palettes[FL_PALETTE_MAX] = { NULL };
static SDL_Texture* current_texture = NULL;
static unsigned int current_texture_binding = 0;
static bool current_texture_binding_valid = false;
static bool cps3_target_bound = false;
static SDL_Texture* texture_cache[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1] = { { NULL } };
static SDL_Texture* textures_to_destroy[1024] = { NULL };
static int textures_to_destroy_count = 0;
static RenderTask render_tasks[RENDER_TASK_MAX] = { 0 };
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
static bool frame_stats_extended_enabled = false;
static const float rect_task_epsilon = 0.001f;
static SDLGameRenderer_TaskSource current_task_source = SDL_GAME_RENDERER_TASK_SOURCE_UNKNOWN;

typedef enum HybridFallbackReason {
    HYBRID_FALLBACK_REASON_NONE = 0,
    HYBRID_FALLBACK_REASON_CLIP,
    HYBRID_FALLBACK_REASON_ALPHA,
    HYBRID_FALLBACK_REASON_COLOR_MOD,
    HYBRID_FALLBACK_REASON_FLIP,
    HYBRID_FALLBACK_REASON_GEOMETRY,
    HYBRID_FALLBACK_REASON_SOLID,
} HybridFallbackReason;

static int compare_render_tasks(const RenderTask* a, const RenderTask* b);
static void insertion_sort_render_tasks(void);
static void initialize_render_task_batch_indices(void);
static void submit_render_tasks(void);
static bool submit_rect_task(const RenderTask* task);
static bool try_submit_geometry_task_as_rect_copy(const RenderTask* task,
                                                  bool* out_used_rect_path,
                                                  RenderTask* out_submitted_rect_task);
static void flush_rect_texture_run_stats(RectRunTelemetryState* state);
static bool can_merge_rect_tasks_horizontally(const RenderTask* prev, const RenderTask* next);
static bool can_merge_rect_tasks_vertically(const RenderTask* prev, const RenderTask* next);
static void mark_dirty_tiles_for_task(const RenderTask* task);
static int invalidate_texture_cache_for_texture_index(int texture_index);
static int invalidate_texture_cache_for_palette_handle(int palette_handle);
static void fill_palette_colors_from_fl_texture(const FLTexture* fl_palette, SDL_Color* colors, int* out_color_count);
static void read_rgba32_color(Uint32 pixel, SDL_Color* color);
static void read_rgba32_fcolor(Uint32 pixel, SDL_FColor* fcolor);
static bool nearly_equal(float a, float b);
static void count_task_source(SDLGameRenderer_TaskSource source);
static Uint64 render_task_submitted_pixels(const RenderTask* task);
static bool rect_task_fits_native_canvas(const RenderTask* task);
static HybridFallbackReason classify_hybrid_fallback_reason(const RenderTask* task);
static void note_hybrid_eligibility(const RenderTask* task);
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

static int invalidate_texture_cache_for_texture_index(int texture_index) {
    const int texture_handle = texture_index + 1;
    int evicted_count = 0;

    if (current_texture_binding_valid && (LO_16_BITS(current_texture_binding) == (unsigned int)texture_handle)) {
        current_texture = NULL;
        current_texture_binding_valid = false;
    }

    for (int i = 0; i < FL_PALETTE_MAX + 1; i++) {
        SDL_Texture** texture_p = &texture_cache[texture_index][i];

        if (*texture_p == NULL) {
            continue;
        }

        push_texture_to_destroy(*texture_p);
        *texture_p = NULL;
        evicted_count += 1;
    }

    return evicted_count;
}

static int invalidate_texture_cache_for_palette_handle(int palette_handle) {
    int evicted_count = 0;

    if (current_texture_binding_valid && (HI_16_BITS(current_texture_binding) == (unsigned int)palette_handle)) {
        current_texture = NULL;
        current_texture_binding_valid = false;
    }

    for (int i = 0; i < FL_TEXTURE_MAX; i++) {
        SDL_Texture** texture_p = &texture_cache[i][palette_handle];

        if (*texture_p == NULL) {
            continue;
        }

        push_texture_to_destroy(*texture_p);
        *texture_p = NULL;
        evicted_count += 1;
    }

    return evicted_count;
}

static void destroy_textures() {
    current_texture = NULL;
    current_texture_binding_valid = false;

    for (int i = 0; i < textures_to_destroy_count; i++) {
        SDL_DestroyTexture(textures_to_destroy[i]);
    }

    textures_to_destroy_count = 0;
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
    count_task_source(render_tasks[insert_index].source);
    render_task_count += 1;
}

static void clear_render_tasks() {
    // Render queue consumers iterate up to `render_task_count`; no need to wipe the full backing array every frame.
    render_task_count = 0;
    render_tasks_have_z_inversion = false;
    render_tasks_z_inversion_count = 0;
}

static void reset_frame_stats(void) {
    SDL_zero(frame_stats);
    frame_stats.sort_strategy = SDL_GAME_RENDERER_SORT_NONE;
    submitted_texture_mod = NULL;
    submitted_texture_mod_color = 0;
    submitted_texture_mod_valid = false;
}

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

    if ((task->source == SDL_GAME_RENDERER_TASK_SOURCE_SOLID) || (task->texture == NULL)) {
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

static void submit_rect_task_as_geometry(const RenderTask* task) {
    SDL_Vertex rect_vertices[4];
    populate_rect_task_vertices(task, rect_vertices);
    SDL_RenderGeometry(_renderer,
                       task->texture,
                       rect_vertices,
                       SDL_arraysize(rect_vertices),
                       render_task_indices,
                       SDL_arraysize(render_task_indices));
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
    float texture_width = 0.0f;
    float texture_height = 0.0f;
    if (!SDL_GetTextureSize(task->texture, &texture_width, &texture_height)) {
        submit_rect_task_as_geometry(task);
        return false;
    }

    if (!set_texture_submit_mod(task->texture, task->color)) {
        submit_rect_task_as_geometry(task);
        return false;
    }

    const SDL_FRect src_rect = { .x = task->src_uv_rect.x * texture_width,
                                 .y = task->src_uv_rect.y * texture_height,
                                 .w = task->src_uv_rect.w * texture_width,
                                 .h = task->src_uv_rect.h * texture_height };
    const bool ok = (task->flip == SDL_FLIP_NONE)
                        ? SDL_RenderTexture(_renderer, task->texture, &src_rect, &task->dst_rect)
                        : SDL_RenderTextureRotated(_renderer, task->texture, &src_rect, &task->dst_rect, 0.0, NULL, task->flip);

    if (!ok) {
        submit_rect_task_as_geometry(task);
        return false;
    }

    frame_stats.rect_copy_tasks += 1;
    return true;
}

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

static void submit_geometry_task_range(int task_start, int run_task_count, SDL_Texture* run_texture) {
    const int vertices_per_task = SDL_arraysize(render_tasks[0].vertices);
    const int indices_per_task = SDL_arraysize(render_task_indices);

    if (run_task_count == 1) {
        const RenderTask* task = &render_tasks[task_start];
        SDL_RenderGeometry(_renderer,
                           run_texture,
                           task->vertices,
                           vertices_per_task,
                           render_task_indices,
                           indices_per_task);
        return;
    }

    // Preserve exact task ordering by batching only contiguous same-texture runs.
    frame_stats.batch_runs += 1;
    frame_stats.batched_task_count += run_task_count;
    const RenderTask* run_task = &render_tasks[task_start];
    SDL_Vertex* dst_vertices = render_task_batch_vertices;
    for (int i = 0; i < run_task_count; i++) {
        SDL_memcpy(dst_vertices, run_task->vertices, sizeof(run_task->vertices));
        dst_vertices += vertices_per_task;
        run_task += 1;
    }

    SDL_RenderGeometry(_renderer,
                       run_texture,
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

void SDLGameRenderer_BeginFrame(bool capture_extended_stats) {
    frame_stats_extended_enabled = capture_extended_stats;
    reset_frame_stats();
    current_task_source = SDL_GAME_RENDERER_TASK_SOURCE_UNKNOWN;
    clear_tile_map(current_coverage_tile_map);
    current_coverage_tile_count = 0;

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
            frame_stats.sort_strategy = SDL_GAME_RENDERER_SORT_INSERTION;
            insertion_sort_render_tasks();
        } else {
            frame_stats.sort_strategy = SDL_GAME_RENDERER_SORT_QSORT;
            qsort(render_tasks, render_task_count, sizeof(RenderTask), compare_render_tasks);
        }
    }

    submit_render_tasks();

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

void SDLGameRenderer_UnlockPalette(unsigned int ph) {
    const int palette_handle = ph;
    const int palette_index = palette_handle - 1;

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
            frame_stats.palette_unlock_calls += 1;
            frame_stats.palette_cache_evictions += invalidate_texture_cache_for_palette_handle(palette_handle);
        } else {
            invalidate_texture_cache_for_palette_handle(palette_handle);
        }
    }
}

void SDLGameRenderer_UnlockTexture(unsigned int th) {
    const int texture_handle = th;
    const int texture_index = texture_handle - 1;

    if ((texture_handle > 0) && (texture_handle <= FL_TEXTURE_MAX)) {
        if (surfaces[texture_index] == NULL) {
            SDLGameRenderer_CreateTexture(th);
            return;
        }

        if (frame_stats_extended_enabled) {
            frame_stats.texture_unlock_calls += 1;
            frame_stats.texture_cache_evictions += invalidate_texture_cache_for_texture_index(texture_index);
        } else {
            invalidate_texture_cache_for_texture_index(texture_index);
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
}

void SDLGameRenderer_DestroyTexture(unsigned int texture_handle) {
    const int texture_index = texture_handle - 1;
    invalidate_texture_cache_for_texture_index(texture_index);

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
    invalidate_texture_cache_for_palette_handle((int)palette_handle);

    SDL_DestroyPalette(palettes[palette_index]);
    palettes[palette_index] = NULL;
}

void SDLGameRenderer_SetTexture(unsigned int th) {
    const int texture_handle = LO_16_BITS(th);
    const SDL_Surface* surface = surfaces[texture_handle - 1];
    const int palette_handle = HI_16_BITS(th);
    const SDL_Palette* palette = palette_handle != 0 ? palettes[palette_handle - 1] : NULL;

    if (frame_stats_extended_enabled) {
        frame_stats.set_texture_calls += 1;
    }

    if (dump_textures) {
        save_texture(surface, palette);
    }

    if (current_texture_binding_valid && (current_texture_binding == th) && (current_texture != NULL)) {
        if (frame_stats_extended_enabled) {
            frame_stats.texture_binding_reuse_hits += 1;
        }
        return;
    }

    SDL_Texture* texture = NULL;
    const SDL_Texture* cached_texture = texture_cache[texture_handle - 1][palette_handle];

    if (cached_texture != NULL) {
        texture = cached_texture;
        frame_stats.texture_cache_hits += 1;
    } else {
        frame_stats.texture_cache_misses += 1;
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
        frame_stats.texture_creates += 1;
    }

    push_texture(texture);
    current_texture_binding = th;
    current_texture_binding_valid = true;
}

void SDLGameRenderer_SetTaskSource(SDLGameRenderer_TaskSource source) {
    current_task_source = source;
}

static RenderTask* begin_quad_task(bool textured, float z) {
    RenderTask* task = &render_tasks[render_task_count];
    task->index = render_task_count;
    task->texture = textured ? get_texture() : NULL;
    task->type = RENDER_TASK_TYPE_GEOMETRY;
    task->source = current_task_source;
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
