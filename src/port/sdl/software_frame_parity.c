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
    if (dst_a == 255u) {
        const Uint32 inv_src_a = 255u - src_a;
        const Uint32 src_r = (src_pixel >> 16) & 0xFFu;
        const Uint32 src_g = (src_pixel >> 8) & 0xFFu;
        const Uint32 src_b = src_pixel & 0xFFu;
        const Uint32 dst_r = (dst_pixel >> 16) & 0xFFu;
        const Uint32 dst_g = (dst_pixel >> 8) & 0xFFu;
        const Uint32 dst_b = dst_pixel & 0xFFu;
        const Uint32 out_r = ((src_r * src_a) + (dst_r * inv_src_a) + 127u) / 255u;
        const Uint32 out_g = ((src_g * src_a) + (dst_g * inv_src_a) + 127u) / 255u;
        const Uint32 out_b = ((src_b * src_a) + (dst_b * inv_src_a) + 127u) / 255u;
        return 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
    }

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

static bool surfaces_match(const char* case_name, const SDL_Surface* expected, const SDL_Surface* actual) {
    const Uint32* expected_pixels = (const Uint32*)expected->pixels;
    const Uint32* actual_pixels = (const Uint32*)actual->pixels;
    const int expected_pitch = expected->pitch / (int)sizeof(Uint32);
    const int actual_pitch = actual->pitch / (int)sizeof(Uint32);

    for (int y = 0; y < expected->h; y++) {
        for (int x = 0; x < expected->w; x++) {
            const Uint32 expected_pixel = expected_pixels[y * expected_pitch + x];
            const Uint32 actual_pixel = actual_pixels[y * actual_pitch + x];
            if (expected_pixel != actual_pixel) {
                SDL_Log("Software-frame parity mismatch in %s at (%d,%d): expected=0x%08x actual=0x%08x",
                        case_name,
                        x,
                        y,
                        expected_pixel,
                        actual_pixel);
                return false;
            }
        }
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
        fill_test_destination(expected, cases[i].opaque_destination);
        fill_test_destination(actual, cases[i].opaque_destination);
        raster_reference_non_integer_task(&cases[i], expected, source);
        if (!SDLSoftwareFrame_RasterNonIntegerLookupARGB8888(&cases[i].dst_rect,
                                                             &cases[i].src_uv_rect,
                                                             cases[i].flip,
                                                             cases[i].color,
                                                             actual,
                                                             source,
                                                             NULL,
                                                             false,
                                                             false)) {
            SDL_Log("Software-frame parity lookup helper rejected case: %s", cases[i].name);
            SDL_DestroySurface(actual);
            SDL_DestroySurface(expected);
            SDL_DestroySurface(source);
            return false;
        }
        if (!surfaces_match(cases[i].name, expected, actual)) {
            SDL_DestroySurface(actual);
            SDL_DestroySurface(expected);
            SDL_DestroySurface(source);
            return false;
        }
    }

    SDL_Log("Software-frame parity check passed: %d cases", (int)SDL_arraysize(cases));
    SDL_DestroySurface(actual);
    SDL_DestroySurface(expected);
    SDL_DestroySurface(source);
    return run_software_source_refresh_parity_check();
}
