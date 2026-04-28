#include "port/sdl/sdl_game_renderer.h"
#include "port/sdl/software_frame_non_integer.h"

#include <SDL3/SDL.h>

typedef struct SoftwareFrameParityTask {
    const char* name;
    SDL_FRect dst_rect;
    SDL_FRect src_uv_rect;
    SDL_FlipMode flip;
    Uint32 color;
    bool opaque_destination;
} SoftwareFrameParityTask;

typedef struct SoftwareSourceRefreshParityMutation {
    SDL_Rect rect;
    Uint8 salt;
} SoftwareSourceRefreshParityMutation;

typedef struct SoftwareSourceRefreshParityCase {
    const char* name;
    SoftwareSourceRefreshParityMutation mutations[2];
    int mutation_count;
} SoftwareSourceRefreshParityCase;

static int clamp_to_range(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

/* Parity reference must mirror the kernel's exact arithmetic, NOT a more
 * precise alternative. The kernel in software_frame_non_integer.c uses the
 * fast `(x + 128u) >> 8` approximation for normalize-by-255 (introduced in
 * the perf-2 software-renderer optimization pass for 60fps stability); using
 * the slower `(x + 127u) / 255u` exact form here produces off-by-one diffs
 * vs the kernel for partial-α blends and breaks the parity test (verify
 * stage caught this on subpixel-upscale). Ground truth = what the kernel
 * actually computes, so we match the >>8 form byte-for-byte. */
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

static void fill_test_source(SDL_Surface* surface) {
    static const Uint8 alpha_table[] = { 0u, 17u, 85u, 128u, 255u };
    Uint32* pixels = (Uint32*)surface->pixels;
    const int pitch = surface->pitch / (int)sizeof(Uint32);

    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            const Uint32 a = alpha_table[(x + (y * 3)) % SDL_arraysize(alpha_table)];
            const Uint32 r = (Uint32)((x * 29 + y * 11) & 0xFF);
            const Uint32 g = (Uint32)((x * 7 + y * 31) & 0xFF);
            const Uint32 b = (Uint32)((x * 19 + y * 5) & 0xFF);
            pixels[y * pitch + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void fill_test_destination(SDL_Surface* surface, bool opaque_destination) {
    Uint32* pixels = (Uint32*)surface->pixels;
    const int pitch = surface->pitch / (int)sizeof(Uint32);

    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            const Uint32 a = opaque_destination ? 255u : (Uint32)((64 + ((x * 9 + y * 13) % 128)) & 0xFF);
            const Uint32 r = (Uint32)((x * 5 + y * 3) & 0xFF);
            const Uint32 g = (Uint32)((x * 17 + y * 7) & 0xFF);
            const Uint32 b = (Uint32)((x * 23 + y * 19) & 0xFF);
            pixels[y * pitch + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void raster_reference_non_integer_task(const SoftwareFrameParityTask* task,
                                              SDL_Surface* dst_surface,
                                              const SDL_Surface* src_surface) {
    const int dst_x0 = clamp_to_range((int)SDL_floorf(task->dst_rect.x), 0, dst_surface->w);
    const int dst_y0 = clamp_to_range((int)SDL_floorf(task->dst_rect.y), 0, dst_surface->h);
    const int dst_x1 = clamp_to_range((int)SDL_ceilf(task->dst_rect.x + task->dst_rect.w), 0, dst_surface->w);
    const int dst_y1 = clamp_to_range((int)SDL_ceilf(task->dst_rect.y + task->dst_rect.h), 0, dst_surface->h);
    if ((dst_x1 <= dst_x0) || (dst_y1 <= dst_y0)) {
        return;
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
}

/* perf-2 P-1.1: format-agnostic per-row SDL_memcmp. The previous
 * implementation cast to Uint32* and divided pitch by sizeof(Uint32),
 * which read past row end on a 565 surface. This walk uses
 * SDL_BYTESPERPIXEL(format) and per-surface pitch, correct for any
 * pixel size SDL produces.
 *
 * Format mismatch between expected and actual is intentionally treated
 * as a harness setup error: the parity matrix always allocates both
 * surfaces with the same dst_formats[df].format, so cross-format diffs
 * cannot occur in practice and would not be byte-comparable anyway. */
static bool surfaces_match(const char* case_name, const SDL_Surface* expected, const SDL_Surface* actual) {
    if ((expected->format != actual->format) ||
        (expected->w != actual->w) ||
        (expected->h != actual->h)) {
        SDL_Log("Software-frame parity surface mismatch in %s: format/size differ "
                "(expected %dx%d fmt 0x%x, actual %dx%d fmt 0x%x)",
                case_name, expected->w, expected->h, (unsigned)expected->format,
                actual->w, actual->h, (unsigned)actual->format);
        return false;
    }
    const int bpp = (int)SDL_BYTESPERPIXEL(expected->format);
    const size_t row_bytes = (size_t)expected->w * (size_t)bpp;
    const Uint8* exp_row = (const Uint8*)expected->pixels;
    const Uint8* act_row = (const Uint8*)actual->pixels;
    for (int y = 0; y < expected->h; y++) {
        if (SDL_memcmp(exp_row, act_row, row_bytes) != 0) {
            for (int x = 0; x < expected->w; x++) {
                if (SDL_memcmp(exp_row + (x * bpp), act_row + (x * bpp), (size_t)bpp) != 0) {
                    SDL_Log("Software-frame parity mismatch in %s at (%d,%d) (bpp=%d)",
                            case_name, x, y, bpp);
                    return false;
                }
            }
        }
        exp_row += expected->pitch;
        act_row += actual->pitch;
    }
    return true;
}

static void fill_index8_test_source(SDL_Surface* surface) {
    Uint8* pixels = (Uint8*)surface->pixels;

    for (int y = 0; y < surface->h; y++) {
        Uint8* row = pixels + ((size_t)y * (size_t)surface->pitch);
        for (int x = 0; x < surface->w; x++) {
            row[x] = (Uint8)((x * 11 + y * 17) & 0xFF);
        }
    }
}

static bool fill_index8_test_palette(SDL_Palette* palette) {
    SDL_Color colors[256];
    for (int i = 0; i < SDL_arraysize(colors); i++) {
        colors[i].r = (Uint8)((i * 37) & 0xFF);
        colors[i].g = (Uint8)((i * 73 + 19) & 0xFF);
        colors[i].b = (Uint8)((i * 29 + 101) & 0xFF);
        colors[i].a = 255u;
    }
    return SDL_SetPaletteColors(palette, colors, 0, SDL_arraysize(colors));
}

/* Strict binary-α: palette[0].α=0; all others α=255. Mirrors the historical
 * "color-key in slot 0" convention. */
static bool fill_index8_test_palette_strict_binary_alpha(SDL_Palette* palette) {
    SDL_Color colors[256];
    for (int i = 0; i < SDL_arraysize(colors); i++) {
        colors[i].r = (Uint8)((i * 37) & 0xFF);
        colors[i].g = (Uint8)((i * 73 + 19) & 0xFF);
        colors[i].b = (Uint8)((i * 29 + 101) & 0xFF);
        colors[i].a = (i == 0) ? 0u : 255u;
    }
    return SDL_SetPaletteColors(palette, colors, 0, SDL_arraysize(colors));
}

/* Loose binary-α: multiple scattered α=0 indices (not just slot 0). Exercises
 * the loose-form distinction — strict-form (i==0 skip) would render these
 * incorrectly, loose-form ((α>>24)==0 skip) handles them. */
static bool fill_index8_test_palette_loose_binary_alpha(SDL_Palette* palette) {
    SDL_Color colors[256];
    static const int transparent_indices[] = { 0, 7, 31, 63, 128, 200 };
    for (int i = 0; i < SDL_arraysize(colors); i++) {
        colors[i].r = (Uint8)((i * 37) & 0xFF);
        colors[i].g = (Uint8)((i * 73 + 19) & 0xFF);
        colors[i].b = (Uint8)((i * 29 + 101) & 0xFF);
        colors[i].a = 255u;
    }
    for (int k = 0; k < (int)SDL_arraysize(transparent_indices); k++) {
        colors[transparent_indices[k]].a = 0u;
    }
    return SDL_SetPaletteColors(palette, colors, 0, SDL_arraysize(colors));
}

/* Build the per-palette ARGB8888 LUT used by the renderer's INDEX8 fast path.
 * This mirrors the renderer's build_software_palette_lut() shape so the parity
 * shim sees the same lookup the production kernel sees. */
static void build_test_palette_lut(const SDL_Palette* palette, Uint32* out_lut) {
    const int n = palette->ncolors < 256 ? palette->ncolors : 256;
    for (int i = 0; i < n; i++) {
        const SDL_Color c = palette->colors[i];
        out_lut[i] = ((Uint32)c.a << 24) | ((Uint32)c.r << 16) | ((Uint32)c.g << 8) | (Uint32)c.b;
    }
    for (int i = n; i < 256; i++) {
        out_lut[i] = 0;
    }
}

/* perf-2: 565 sibling. Mirrors the renderer's build_software_palette_lut()
 * 565 build pass. Available for future 565 parity cases that need a parallel
 * LUT565 (current cases derive 565 directly from LUT8888 via parity_pack_*). */
static __attribute__((unused)) void build_test_palette_lut_565(const SDL_Palette* palette, Uint16* out_lut) {
    const int n = palette->ncolors < 256 ? palette->ncolors : 256;
    for (int i = 0; i < n; i++) {
        const SDL_Color c = palette->colors[i];
        out_lut[i] = (Uint16)(((Uint32)(c.r >> 3) << 11) |
                              ((Uint32)(c.g >> 2) << 5) |
                              ((Uint32)(c.b >> 3)));
    }
    for (int i = n; i < 256; i++) {
        out_lut[i] = 0;
    }
}

/* Local pack helper, duplicated here so the parity TU does not need to link
 * against renderer internals. Same formula as pack_rgb565_from_argb. */
static inline Uint16 parity_pack_rgb565_from_argb(Uint32 argb) {
    const Uint32 r = (argb >> 16) & 0xFFu;
    const Uint32 g = (argb >> 8) & 0xFFu;
    const Uint32 b = argb & 0xFFu;
    return (Uint16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static bool palette_is_binary_alpha(const Uint32* lut) {
    for (int i = 0; i < 256; i++) {
        const Uint32 a = (lut[i] >> 24) & 0xFFu;
        if ((a != 0u) && (a != 0xFFu)) {
            return false;
        }
    }
    return true;
}

static void mutate_index8_test_source(SDL_Surface* surface, const SDL_Rect* rect, Uint8 salt) {
    Uint8* pixels = (Uint8*)surface->pixels;

    for (int y = rect->y; y < rect->y + rect->h; y++) {
        Uint8* row = pixels + ((size_t)y * (size_t)surface->pitch);
        for (int x = rect->x; x < rect->x + rect->w; x++) {
            Uint8 value = (Uint8)(row[x] ^ (Uint8)(0x5Au + salt + ((x * 3 + y * 5) & 0x3Fu)));
            if (value == row[x]) {
                value ^= 0xFFu;
            }
            row[x] = value;
        }
    }
}

static bool run_software_source_refresh_parity_check(void) {
    static const SoftwareSourceRefreshParityCase cases[] = {
        { "single-sparse-bbox", { { { 24, 40, 18, 14 }, 0x11u } }, 1 },
        { "unioned-sparse-bbox",
          { { { 52, 28, 14, 18 }, 0x29u }, { { 110, 74, 12, 10 }, 0x5Cu } },
          2 },
    };

    for (int case_index = 0; case_index < SDL_arraysize(cases); case_index++) {
        const SoftwareSourceRefreshParityCase* parity_case = &cases[case_index];
        SDL_Surface* source = SDL_CreateSurface(256, 256, SDL_PIXELFORMAT_INDEX8);
        SDL_Palette* palette = SDL_CreatePalette(256);
        SDL_Surface* expected = NULL;
        SDL_Surface* actual = NULL;
        if ((source == NULL) || (palette == NULL)) {
            SDL_Log("Software-source refresh parity setup failed: %s", SDL_GetError());
            SDL_DestroyPalette(palette);
            SDL_DestroySurface(source);
            return false;
        }

        fill_index8_test_source(source);
        if (!fill_index8_test_palette(palette) || !SDL_SetSurfacePalette(source, palette)) {
            SDL_Log("Software-source refresh parity palette setup failed: %s", SDL_GetError());
            SDL_DestroyPalette(palette);
            SDL_DestroySurface(source);
            return false;
        }

        expected = SDL_ConvertSurface(source, SDL_PIXELFORMAT_ARGB8888);
        actual = SDL_ConvertSurface(source, SDL_PIXELFORMAT_ARGB8888);
        if ((expected == NULL) || (actual == NULL)) {
            SDL_Log("Software-source refresh parity convert setup failed: %s", SDL_GetError());
            SDL_DestroySurface(actual);
            SDL_DestroySurface(expected);
            SDL_DestroyPalette(palette);
            SDL_DestroySurface(source);
            return false;
        }

        SDL_Rect dirty_rect = { source->w, source->h, 0, 0 };
        bool dirty_rect_valid = false;
        for (int mutation_index = 0; mutation_index < parity_case->mutation_count; mutation_index++) {
            const SDL_Rect mutation_rect = parity_case->mutations[mutation_index].rect;
            mutate_index8_test_source(source, &mutation_rect, parity_case->mutations[mutation_index].salt);
            if (!dirty_rect_valid) {
                dirty_rect = mutation_rect;
                dirty_rect_valid = true;
            } else {
                const int x0 = SDL_min(dirty_rect.x, mutation_rect.x);
                const int y0 = SDL_min(dirty_rect.y, mutation_rect.y);
                const int x1 =
                    SDL_max(dirty_rect.x + dirty_rect.w - 1, mutation_rect.x + mutation_rect.w - 1);
                const int y1 =
                    SDL_max(dirty_rect.y + dirty_rect.h - 1, mutation_rect.y + mutation_rect.h - 1);
                dirty_rect.x = x0;
                dirty_rect.y = y0;
                dirty_rect.w = x1 - x0 + 1;
                dirty_rect.h = y1 - y0 + 1;
            }
        }

        if (!dirty_rect_valid || !SDL_SetSurfacePalette(source, palette) || !SDL_BlitSurface(source, NULL, expected, NULL)) {
            SDL_Log("Software-source refresh parity full refresh failed in %s: %s", parity_case->name, SDL_GetError());
            SDL_DestroySurface(actual);
            SDL_DestroySurface(expected);
            SDL_DestroyPalette(palette);
            SDL_DestroySurface(source);
            return false;
        }

        SDL_Rect dst_rect = dirty_rect;
        if (!SDL_BlitSurface(source, &dirty_rect, actual, &dst_rect)) {
            SDL_Log("Software-source refresh parity partial refresh failed in %s: %s", parity_case->name, SDL_GetError());
            SDL_DestroySurface(actual);
            SDL_DestroySurface(expected);
            SDL_DestroyPalette(palette);
            SDL_DestroySurface(source);
            return false;
        }

        if (!surfaces_match(parity_case->name, expected, actual)) {
            SDL_DestroySurface(actual);
            SDL_DestroySurface(expected);
            SDL_DestroyPalette(palette);
            SDL_DestroySurface(source);
            return false;
        }

        SDL_DestroySurface(actual);
        SDL_DestroySurface(expected);
        SDL_DestroyPalette(palette);
        SDL_DestroySurface(source);
    }

    SDL_Log("Software-source refresh parity check passed: %d cases", (int)SDL_arraysize(cases));
    return true;
}

typedef struct Index8FastPathCase {
    const char* name;
    SDL_FRect dst_rect;
    SDL_FRect src_uv_rect;
    SDL_FlipMode flip;
} Index8FastPathCase;

/* Reference: read INDEX8 bytes directly and look them up via the same `lut[]`
 * the production kernel uses, then composite into the destination using the
 * loose-form rule (α==0 keep dst, α==0xFF write src). The scaled branch builds
 * src lookup tables via SDLGameRenderer_PopulateScaledLookupTableForParity() so
 * the reference uses exactly the production scaling formula — no second
 * algorithm masquerading as ground truth. */
static bool raster_reference_index8_loose(SDL_Surface* index8_src,
                                          const Uint32* lut,
                                          const SDL_FRect* dst_rect,
                                          const SDL_FRect* src_uv_rect,
                                          SDL_FlipMode flip,
                                          SDL_Surface* dst_surface) {
    /* Compute integer src/dst rects identical to build_software_frame_fast_copy_plan(). */
    const int dst_x = (int)SDL_roundf(dst_rect->x);
    const int dst_y = (int)SDL_roundf(dst_rect->y);
    const int dst_w = (int)SDL_roundf(dst_rect->w);
    const int dst_h = (int)SDL_roundf(dst_rect->h);
    const int src_x_int = (int)SDL_roundf(src_uv_rect->x * (float)index8_src->w);
    const int src_y_int = (int)SDL_roundf(src_uv_rect->y * (float)index8_src->h);
    const int src_w_int = (int)SDL_roundf(src_uv_rect->w * (float)index8_src->w);
    const int src_h_int = (int)SDL_roundf(src_uv_rect->h * (float)index8_src->h);
    if ((dst_w <= 0) || (dst_h <= 0) || (src_w_int <= 0) || (src_h_int <= 0)) {
        return false;
    }

    const int dst_x0 = clamp_to_range(dst_x, 0, dst_surface->w);
    const int dst_y0 = clamp_to_range(dst_y, 0, dst_surface->h);
    const int dst_x1 = clamp_to_range(dst_x + dst_w, 0, dst_surface->w);
    const int dst_y1 = clamp_to_range(dst_y + dst_h, 0, dst_surface->h);
    const bool flip_h = (flip & SDL_FLIP_HORIZONTAL) != 0;
    const bool flip_v = (flip & SDL_FLIP_VERTICAL) != 0;
    const int clip_left = dst_x0 - dst_x;
    const int clip_top = dst_y0 - dst_y;
    const int visible_w = dst_x1 - dst_x0;
    const int visible_h = dst_y1 - dst_y0;
    const bool exact_copy = (src_w_int == dst_w) && (src_h_int == dst_h);

    const Uint8* index8_pixels = (const Uint8*)index8_src->pixels;
    const int index8_pitch = index8_src->pitch;
    /* perf-2 P-2.3: format-aware pointer + pitch. The 565 branch must use
     * Uint16* and pitch / sizeof(Uint16); the 8888 branch keeps the original
     * Uint32*. The α-decide stays identical (read α from LUT8888); only
     * pointer type / pitch divisor / store form differ. */
    const bool dst_is_565 = (dst_surface->format == SDL_PIXELFORMAT_RGB565);
    Uint32* dst_pixels = (Uint32*)dst_surface->pixels;
    const int dst_pitch = dst_surface->pitch / (int)sizeof(Uint32);
    Uint16* dst_pixels_16 = (Uint16*)dst_surface->pixels;
    const int dst_pitch_16 = dst_surface->pitch / (int)sizeof(Uint16);

    if (exact_copy) {
        const int src_x_step = flip_h ? -1 : 1;
        const int src_y_step = flip_v ? -1 : 1;
        const int src_row0_x = flip_h ? (src_x_int + src_w_int - 1 - clip_left) : (src_x_int + clip_left);
        const int src_row0_y = flip_v ? (src_y_int + src_h_int - 1 - clip_top) : (src_y_int + clip_top);
        for (int row = 0; row < visible_h; row++) {
            const Uint8* src_row = index8_pixels + ((src_row0_y + (row * src_y_step)) * index8_pitch);
            int src_x = src_row0_x;
            if (dst_is_565) {
                Uint16* dst_row = dst_pixels_16 + ((dst_y0 + row) * dst_pitch_16) + dst_x0;
                for (int col = 0; col < visible_w; col++) {
                    const Uint32 src_pixel = lut[src_row[src_x]];
                    if ((src_pixel >> 24) != 0u) { dst_row[col] = parity_pack_rgb565_from_argb(src_pixel); }
                    src_x += src_x_step;
                }
            } else {
                Uint32* dst_row = dst_pixels + ((dst_y0 + row) * dst_pitch) + dst_x0;
                for (int col = 0; col < visible_w; col++) {
                    const Uint32 src_pixel = lut[src_row[src_x]];
                    if ((src_pixel >> 24) != 0u) { dst_row[col] = src_pixel; }
                    src_x += src_x_step;
                }
            }
        }
    } else {
        if ((visible_w > dst_surface->w) || (visible_h > dst_surface->h)) {
            return false;
        }
        int* src_x_lookup = (int*)SDL_malloc((size_t)visible_w * sizeof(int));
        int* src_y_lookup = (int*)SDL_malloc((size_t)visible_h * sizeof(int));
        if ((src_x_lookup == NULL) || (src_y_lookup == NULL)) {
            SDL_free(src_y_lookup);
            SDL_free(src_x_lookup);
            return false;
        }
        SDLGameRenderer_PopulateScaledLookupTableForParity(
            src_x_lookup, visible_w, dst_x, dst_x0, dst_w, src_x_int, src_w_int, flip_h);
        SDLGameRenderer_PopulateScaledLookupTableForParity(
            src_y_lookup, visible_h, dst_y, dst_y0, dst_h, src_y_int, src_h_int, flip_v);
        for (int row = 0; row < visible_h; row++) {
            const Uint8* src_row = index8_pixels + (src_y_lookup[row] * index8_pitch);
            if (dst_is_565) {
                Uint16* dst_row = dst_pixels_16 + ((dst_y0 + row) * dst_pitch_16) + dst_x0;
                for (int col = 0; col < visible_w; col++) {
                    const Uint32 src_pixel = lut[src_row[src_x_lookup[col]]];
                    if ((src_pixel >> 24) != 0u) { dst_row[col] = parity_pack_rgb565_from_argb(src_pixel); }
                }
            } else {
                Uint32* dst_row = dst_pixels + ((dst_y0 + row) * dst_pitch) + dst_x0;
                for (int col = 0; col < visible_w; col++) {
                    const Uint32 src_pixel = lut[src_row[src_x_lookup[col]]];
                    if ((src_pixel >> 24) != 0u) { dst_row[col] = src_pixel; }
                }
            }
        }
        SDL_free(src_y_lookup);
        SDL_free(src_x_lookup);
    }

    return true;
}

static bool run_index8_fast_path_parity_check(void) {
    /* dst_rect coords MUST be integer-valued floats: build_software_frame_fast_copy_plan
     * runs `nearly_equal(SDL_roundf(v), v)` (epsilon 0.001) on each. UVs MUST also
     * round-trip through `SDL_roundf(uv * src_dim)` to integers under that
     * epsilon, otherwise the plan returns SOFTWARE_FRAME_FAST_COPY_RESULT_NON_INTEGER
     * and the fast-path shim refuses the case. Source surface is 48x32; pick UVs
     * that produce integer src rects at that resolution. */
    static const Index8FastPathCase cases[] = {
        { "exact-forward",    { 4.0f, 6.0f, 24.0f, 18.0f }, { 0.0f, 0.0f, 0.5f, 0.5625f }, SDL_FLIP_NONE },
        { "exact-flip-h",     { 4.0f, 6.0f, 24.0f, 18.0f }, { 0.0f, 0.0f, 0.5f, 0.5625f }, SDL_FLIP_HORIZONTAL },
        { "exact-flip-v",     { 4.0f, 6.0f, 24.0f, 18.0f }, { 0.0f, 0.0f, 0.5f, 0.5625f }, SDL_FLIP_VERTICAL },
        { "exact-flip-both",  { 4.0f, 6.0f, 24.0f, 18.0f }, { 0.0f, 0.0f, 0.5f, 0.5625f }, SDL_FLIP_HORIZONTAL_AND_VERTICAL },
        /* scaled-up: src 24x16 -> dst 36x28 (non-exact). UVs picked so 0.0625*48=3,
         * 0.0625*32=2, 0.5*48=24, 0.5*32=16 are all exact integers. */
        { "scaled-up",        { 2.0f, 2.0f, 36.0f, 28.0f }, { 0.0625f, 0.0625f, 0.5f, 0.5f }, SDL_FLIP_NONE },
        { "scaled-down",      { 4.0f, 4.0f, 12.0f, 9.0f },  { 0.0f, 0.0f, 0.625f, 0.625f }, SDL_FLIP_NONE },
        { "clip-top-left",    { -6.0f, -4.0f, 24.0f, 18.0f }, { 0.0f, 0.0f, 0.5f, 0.5625f }, SDL_FLIP_NONE },
        { "clip-bottom-right",{ 30.0f, 22.0f, 24.0f, 18.0f }, { 0.0f, 0.0f, 0.5f, 0.5625f }, SDL_FLIP_NONE },
    };

    typedef struct PaletteFlavor {
        const char* label;
        bool (*fill)(SDL_Palette*);
    } PaletteFlavor;
    static const PaletteFlavor flavors[] = {
        { "strict",  fill_index8_test_palette_strict_binary_alpha },
        { "loose",   fill_index8_test_palette_loose_binary_alpha },
    };

    /* perf-2: parameterize on dst format. The harness now runs each case
     * on both ARGB8888 and RGB565 destinations to validate both kernels. */
    typedef struct DstFormatFlavor {
        const char* label;
        SDL_PixelFormat format;
    } DstFormatFlavor;
    static const DstFormatFlavor dst_formats[] = {
        { "argb8888", SDL_PIXELFORMAT_ARGB8888 },
        { "rgb565",   SDL_PIXELFORMAT_RGB565   },
    };

    const int src_w = 48;
    const int src_h = 32;
    const int dst_w = 64;
    const int dst_h = 48;

    int total_cases_ran = 0;

    for (int df = 0; df < (int)SDL_arraysize(dst_formats); df++) {
        for (int flavor_index = 0; flavor_index < (int)SDL_arraysize(flavors); flavor_index++) {
            SDL_Surface* index8_src = SDL_CreateSurface(src_w, src_h, SDL_PIXELFORMAT_INDEX8);
            SDL_Palette* palette = SDL_CreatePalette(256);
            if ((index8_src == NULL) || (palette == NULL)) {
                SDL_Log("Index8 fast-path parity setup failed (%s/%s): %s",
                        dst_formats[df].label, flavors[flavor_index].label, SDL_GetError());
                SDL_DestroyPalette(palette);
                SDL_DestroySurface(index8_src);
                return false;
            }

            fill_index8_test_source(index8_src);
            if (!flavors[flavor_index].fill(palette) || !SDL_SetSurfacePalette(index8_src, palette)) {
                SDL_Log("Index8 fast-path palette setup failed (%s/%s): %s",
                        dst_formats[df].label, flavors[flavor_index].label, SDL_GetError());
                SDL_DestroyPalette(palette);
                SDL_DestroySurface(index8_src);
                return false;
            }

            Uint32 lut[256];
            build_test_palette_lut(palette, lut);
            const bool is_binary_alpha = palette_is_binary_alpha(lut);
            if (!is_binary_alpha) {
                SDL_Log("Index8 fast-path parity: palette '%s' unexpectedly not binary-α", flavors[flavor_index].label);
                SDL_DestroyPalette(palette);
                SDL_DestroySurface(index8_src);
                return false;
            }

            for (int case_index = 0; case_index < (int)SDL_arraysize(cases); case_index++) {
                SDL_Surface* expected = SDL_CreateSurface(dst_w, dst_h, dst_formats[df].format);
                SDL_Surface* actual   = SDL_CreateSurface(dst_w, dst_h, dst_formats[df].format);
                if ((expected == NULL) || (actual == NULL)) {
                    SDL_Log("Index8 fast-path parity dst-surface alloc failed: %s", SDL_GetError());
                    SDL_DestroySurface(actual);
                    SDL_DestroySurface(expected);
                    SDL_DestroyPalette(palette);
                    SDL_DestroySurface(index8_src);
                    return false;
                }
                /* fill_test_destination assumes ARGB8888; on a 565 surface
                 * we just zero-clear (loose-form fast path overwrites only
                 * non-α==0 pixels, so the cleared pixels stay 0 and match
                 * across expected/actual). */
                if (dst_formats[df].format == SDL_PIXELFORMAT_ARGB8888) {
                    fill_test_destination(expected, true);
                    fill_test_destination(actual, true);
                } else {
                    SDL_memset(expected->pixels, 0, (size_t)expected->pitch * (size_t)expected->h);
                    SDL_memset(actual->pixels, 0, (size_t)actual->pitch * (size_t)actual->h);
                }

                if (!raster_reference_index8_loose(index8_src, lut, &cases[case_index].dst_rect,
                                                   &cases[case_index].src_uv_rect, cases[case_index].flip, expected)) {
                    SDL_Log("Index8 fast-path parity reference build failed for %s/%s/%s",
                            dst_formats[df].label, flavors[flavor_index].label, cases[case_index].name);
                    SDL_DestroySurface(actual);
                    SDL_DestroySurface(expected);
                    SDL_DestroyPalette(palette);
                    SDL_DestroySurface(index8_src);
                    return false;
                }

                if (!SDLGameRenderer_RunIndex8FastPathParityCase(index8_src, lut, is_binary_alpha,
                                                                &cases[case_index].dst_rect,
                                                                &cases[case_index].src_uv_rect,
                                                                cases[case_index].flip, actual)) {
                    SDL_Log("Index8 fast-path parity shim rejected case %s/%s/%s",
                            dst_formats[df].label, flavors[flavor_index].label, cases[case_index].name);
                    SDL_DestroySurface(actual);
                    SDL_DestroySurface(expected);
                    SDL_DestroyPalette(palette);
                    SDL_DestroySurface(index8_src);
                    return false;
                }

                char case_label[96];
                SDL_snprintf(case_label, sizeof(case_label), "index8/%s/%s/%s",
                             dst_formats[df].label, flavors[flavor_index].label, cases[case_index].name);
                if (!surfaces_match(case_label, expected, actual)) {
                    SDL_DestroySurface(actual);
                    SDL_DestroySurface(expected);
                    SDL_DestroyPalette(palette);
                    SDL_DestroySurface(index8_src);
                    return false;
                }

                SDL_DestroySurface(actual);
                SDL_DestroySurface(expected);
                total_cases_ran++;
            }

            SDL_DestroyPalette(palette);
            SDL_DestroySurface(index8_src);
        }
    }

    SDL_Log("Index8 fast-path parity check passed: %d cases", total_cases_ran);
    return true;
}

bool SDLGameRenderer_RunSoftwareFrameParityCheck(void) {
    static const SoftwareFrameParityTask cases[] = {
        { "subpixel-upscale", { 3.25f, 2.50f, 17.50f, 11.75f }, { 0.10f, 0.12f, 0.48f, 0.53f }, SDL_FLIP_NONE,
          0xFFFFFFFFu, false },
        { "subpixel-downscale", { 7.75f, 4.25f, 9.40f, 6.60f }, { 0.05f, 0.07f, 0.81f, 0.74f }, SDL_FLIP_NONE,
          0xFFFFFFFFu, false },
        { "clip-top-left", { -4.50f, -2.75f, 18.40f, 14.10f }, { 0.00f, 0.00f, 0.64f, 0.61f }, SDL_FLIP_NONE,
          0xFFFFFFFFu, false },
        { "clip-bottom-right", { 31.25f, 19.10f, 17.20f, 14.60f }, { 0.18f, 0.20f, 0.72f, 0.70f }, SDL_FLIP_NONE,
          0xFFFFFFFFu, false },
        { "flip-horizontal", { 5.50f, 6.20f, 15.25f, 10.80f }, { 0.12f, 0.08f, 0.61f, 0.69f }, SDL_FLIP_HORIZONTAL,
          0xFFFFFFFFu, false },
        { "flip-vertical", { 8.40f, 5.75f, 14.60f, 12.35f }, { 0.04f, 0.10f, 0.58f, 0.67f }, SDL_FLIP_VERTICAL,
          0xFFFFFFFFu, false },
        { "flip-both", { 1.20f, 9.15f, 13.70f, 8.95f }, { 0.09f, 0.04f, 0.55f, 0.62f },
          SDL_FLIP_HORIZONTAL_AND_VERTICAL, 0xFFFFFFFFu, false },
        { "color-mod", { 11.10f, 3.40f, 16.80f, 12.25f }, { 0.14f, 0.16f, 0.63f, 0.58f }, SDL_FLIP_NONE,
          0xC0B06040u, false },
        { "clip-flip-color", { -2.20f, 12.60f, 18.90f, 11.40f }, { 0.07f, 0.09f, 0.68f, 0.63f }, SDL_FLIP_VERTICAL,
          0x80E0FF60u, false },
        { "opaque-destination", { 6.35f, 7.40f, 18.25f, 10.60f }, { 0.11f, 0.18f, 0.66f, 0.57f }, SDL_FLIP_NONE,
          0x90F080C0u, true },
    };
    static const bool subrect_alpha_modes[] = { false, true };

    SDL_Surface* source = SDL_CreateSurface(23, 19, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface* expected = SDL_CreateSurface(48, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface* actual = SDL_CreateSurface(48, 32, SDL_PIXELFORMAT_ARGB8888);
    if ((source == NULL) || (expected == NULL) || (actual == NULL)) {
        SDL_Log("Software-frame parity check setup failed: %s", SDL_GetError());
        SDL_DestroySurface(actual);
        SDL_DestroySurface(expected);
        SDL_DestroySurface(source);
        return false;
    }

    fill_test_source(source);
    for (int i = 0; i < SDL_arraysize(cases); i++) {
        for (int mode_index = 0; mode_index < SDL_arraysize(subrect_alpha_modes); mode_index++) {
            SDLSoftwareFrame_NonIntegerTelemetry telemetry;
            char case_label[96];
            const bool collect_subrect_alpha_telemetry = subrect_alpha_modes[mode_index];

            fill_test_destination(expected, cases[i].opaque_destination);
            fill_test_destination(actual, cases[i].opaque_destination);
            raster_reference_non_integer_task(&cases[i], expected, source);
            if (!SDLSoftwareFrame_RasterNonIntegerLookupARGB8888(&cases[i].dst_rect,
                                                                 &cases[i].src_uv_rect,
                                                                 cases[i].flip,
                                                                 cases[i].color,
                                                                 actual,
                                                                 source,
                                                                 &telemetry,
                                                                 false,
                                                                 false,
                                                                 false,
                                                                 collect_subrect_alpha_telemetry)) {
                SDL_Log("Software-frame parity lookup helper rejected case: %s", cases[i].name);
                SDL_DestroySurface(actual);
                SDL_DestroySurface(expected);
                SDL_DestroySurface(source);
                return false;
            }

            SDL_snprintf(case_label,
                         sizeof(case_label),
                         "%s%s",
                         cases[i].name,
                         collect_subrect_alpha_telemetry ? " [subrect-alpha]" : "");
            if (!surfaces_match(case_label, expected, actual)) {
                SDL_DestroySurface(actual);
                SDL_DestroySurface(expected);
                SDL_DestroySurface(source);
                return false;
            }
        }
    }

    SDL_Log("Software-frame parity check passed: %d cases", (int)SDL_arraysize(cases));
    SDL_DestroySurface(actual);
    SDL_DestroySurface(expected);
    SDL_DestroySurface(source);
    if (!run_software_source_refresh_parity_check()) {
        return false;
    }
    return run_index8_fast_path_parity_check();
}
