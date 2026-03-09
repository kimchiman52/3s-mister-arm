#include "port/sdl/software_frame_non_integer.h"

enum {
    software_frame_lookup_max_width = 384,
    software_frame_lookup_max_height = 224,
};

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

bool SDLSoftwareFrame_RasterNonIntegerLookupARGB8888(const SDL_FRect* dst_rect,
                                                     const SDL_FRect* src_uv_rect,
                                                     SDL_FlipMode flip,
                                                     Uint32 color,
                                                     SDL_Surface* dst_surface,
                                                     const SDL_Surface* src_surface) {
    if ((dst_rect == NULL) || (src_uv_rect == NULL) || (dst_surface == NULL) || (src_surface == NULL) ||
        (dst_rect->w <= 0.0f) || (dst_rect->h <= 0.0f) || (src_surface->w <= 0) || (src_surface->h <= 0)) {
        return false;
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

    int src_x_lookup[software_frame_lookup_max_width];
    int src_y_lookup[software_frame_lookup_max_height];
    populate_non_integer_lookup(src_x_lookup,
                                visible_w,
                                dst_x0,
                                dst_rect->x,
                                dst_rect->w,
                                src_uv_rect->x * (float)src_surface->w,
                                src_uv_rect->w * (float)src_surface->w,
                                src_surface->w - 1,
                                (flip & SDL_FLIP_HORIZONTAL) != 0);
    populate_non_integer_lookup(src_y_lookup,
                                visible_h,
                                dst_y0,
                                dst_rect->y,
                                dst_rect->h,
                                src_uv_rect->y * (float)src_surface->h,
                                src_uv_rect->h * (float)src_surface->h,
                                src_surface->h - 1,
                                (flip & SDL_FLIP_VERTICAL) != 0);

    const Uint32* src_pixels = (const Uint32*)src_surface->pixels;
    Uint32* dst_pixels = (Uint32*)dst_surface->pixels;
    const int src_pitch = src_surface->pitch / (int)sizeof(Uint32);
    const int dst_pitch = dst_surface->pitch / (int)sizeof(Uint32);

    for (int row = 0; row < visible_h; row++) {
        const Uint32* src_row = src_pixels + (src_y_lookup[row] * src_pitch);
        Uint32* dst_row = dst_pixels + ((dst_y0 + row) * dst_pitch) + dst_x0;
        for (int col = 0; col < visible_w; col++) {
            Uint32 src_pixel = src_row[src_x_lookup[col]];
            if (color != 0xFFFFFFFFu) {
                src_pixel = modulate_argb8888(src_pixel, color);
            }
            dst_row[col] = blend_argb8888(dst_row[col], src_pixel);
        }
    }

    return true;
}
