#include "port/sdl/fps_overlay_compositor.h"

#include <stdbool.h>
#include <stddef.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define FPS_OVERLAY_HAVE_NEON 1
#else
#define FPS_OVERLAY_HAVE_NEON 0
#endif

/* 256 bytes accommodates the perf-3 two-line debug overlay (line 1 = legacy
 * timing breakdown, line 2 = perf-3 split + new-path hit rates). The two
 * lines are separated by a single '\n' character which the layout/render
 * code below splits on. */
static int fps_overlay_mode = 0; /* 0=off, 1=fps (top-left), 2=debug (bottom-center) */
static char fps_overlay_text[256] = "";

static Uint32* fps_overlay_pixels = NULL;
static int fps_overlay_width = 0;
static int fps_overlay_height = 0;
static int fps_overlay_cached_draw_x = 0;
static int fps_overlay_cached_draw_y = 0;
static int fps_overlay_cached_text_x = 0;
static int fps_overlay_cached_text_y = 0;
static int fps_overlay_cached_scale = 0;
/* Sized to match fps_overlay_text. Holds the last-rendered overlay so the
 * dirty-rect comparison in ensure_rasterized_fps_overlay() short-circuits
 * when nothing has changed. */
static char fps_overlay_cached_text[256] = "";
static bool fps_overlay_cache_valid = false;

typedef struct FpsOverlayLayout {
    int draw_x;
    int draw_y;
    int width;
    int height;
    int text_x;
    int text_y;
    int scale;
} FpsOverlayLayout;

static FpsOverlayLayout fps_overlay_frame_layout = { 0 };
static bool fps_overlay_frame_active = false;

static void memset32(Uint32* dst, Uint32 value, size_t count) {
#if FPS_OVERLAY_HAVE_NEON
    const uint32x4_t fill4 = vdupq_n_u32(value);
    size_t i = 0;
    for (; (i + 4) <= count; i += 4) {
        vst1q_u32((uint32_t*)(dst + i), fill4);
    }
    for (; i < count; i++) {
        dst[i] = value;
    }
#else
    for (size_t i = 0; i < count; i++) {
        dst[i] = value;
    }
#endif
}

static void fill_argb_rect(Uint32* pixels, int width, int height, int stride_pixels, int x, int y, int w, int h, Uint32 color) {
    if ((pixels == NULL) || (width <= 0) || (height <= 0) || (stride_pixels < width) || (w <= 0) || (h <= 0)) {
        return;
    }

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if ((x >= width) || (y >= height)) {
        return;
    }

    if ((x + w) > width) {
        w = width - x;
    }
    if ((y + h) > height) {
        h = height - y;
    }
    if ((w <= 0) || (h <= 0)) {
        return;
    }

    const size_t row_pixels = (size_t)w;
    for (int row = 0; row < h; row++) {
        Uint32* dst_row = pixels + ((size_t)stride_pixels * (size_t)(y + row)) + (size_t)x;
        memset32(dst_row, color, row_pixels);
    }
}

static Uint8 overlay_glyph_row(char ch, int row) {
    static const Uint8 glyph_blank[5] = { 0, 0, 0, 0, 0 };
    static const Uint8 glyph_0[5] = { 0x7, 0x5, 0x5, 0x5, 0x7 };
    static const Uint8 glyph_1[5] = { 0x2, 0x6, 0x2, 0x2, 0x7 };
    static const Uint8 glyph_2[5] = { 0x7, 0x1, 0x7, 0x4, 0x7 };
    static const Uint8 glyph_3[5] = { 0x7, 0x1, 0x7, 0x1, 0x7 };
    static const Uint8 glyph_4[5] = { 0x5, 0x5, 0x7, 0x1, 0x1 };
    static const Uint8 glyph_5[5] = { 0x7, 0x4, 0x7, 0x1, 0x7 };
    static const Uint8 glyph_6[5] = { 0x7, 0x4, 0x7, 0x5, 0x7 };
    static const Uint8 glyph_7[5] = { 0x7, 0x1, 0x1, 0x1, 0x1 };
    static const Uint8 glyph_8[5] = { 0x7, 0x5, 0x7, 0x5, 0x7 };
    static const Uint8 glyph_9[5] = { 0x7, 0x5, 0x7, 0x1, 0x7 };
    static const Uint8 glyph_F[5] = { 0x7, 0x4, 0x6, 0x4, 0x4 };
    static const Uint8 glyph_P[5] = { 0x6, 0x5, 0x6, 0x4, 0x4 };
    static const Uint8 glyph_S[5] = { 0x7, 0x4, 0x7, 0x1, 0x7 };
    static const Uint8 glyph_U[5] = { 0x5, 0x5, 0x5, 0x5, 0x7 };
    static const Uint8 glyph_R[5] = { 0x6, 0x5, 0x6, 0x5, 0x5 };
    static const Uint8 glyph_T[5] = { 0x7, 0x2, 0x2, 0x2, 0x2 };
    static const Uint8 glyph_G[5] = { 0x7, 0x4, 0x5, 0x5, 0x7 };
    static const Uint8 glyph_D[5] = { 0x6, 0x5, 0x5, 0x5, 0x6 };
    static const Uint8 glyph_r[5] = { 0x0, 0x5, 0x6, 0x4, 0x4 };
    static const Uint8 glyph_t[5] = { 0x4, 0x7, 0x4, 0x4, 0x3 };
    static const Uint8 glyph_s[5] = { 0x0, 0x3, 0x2, 0x6, 0x0 };
    static const Uint8 glyph_lbracket[5] = { 0x6, 0x4, 0x4, 0x4, 0x6 };
    static const Uint8 glyph_rbracket[5] = { 0x3, 0x1, 0x1, 0x1, 0x3 };
    static const Uint8 glyph_colon[5] = { 0x0, 0x2, 0x0, 0x2, 0x0 };
    static const Uint8 glyph_dot[5] = { 0x0, 0x0, 0x0, 0x0, 0x2 };
    static const Uint8 glyph_eq[5] = { 0x0, 0x7, 0x0, 0x7, 0x0 };
    static const Uint8 glyph_lparen[5] = { 0x2, 0x4, 0x4, 0x4, 0x2 };
    static const Uint8 glyph_rparen[5] = { 0x2, 0x1, 0x1, 0x1, 0x2 };
    static const Uint8 glyph_slash[5] = { 0x1, 0x1, 0x2, 0x4, 0x4 };
    static const Uint8 glyph_pct[5] = { 0x5, 0x1, 0x2, 0x4, 0x5 };
    static const Uint8 glyph_C[5] = { 0x7, 0x4, 0x4, 0x4, 0x7 };
    static const Uint8 glyph_L[5] = { 0x4, 0x4, 0x4, 0x4, 0x7 };
    static const Uint8 glyph_O[5] = { 0x7, 0x5, 0x5, 0x5, 0x7 };
    static const Uint8 glyph_e[5] = { 0x0, 0x7, 0x5, 0x6, 0x3 };
    static const Uint8 glyph_j[5] = { 0x1, 0x0, 0x1, 0x5, 0x2 };
    static const Uint8 glyph_a[5] = { 0x0, 0x6, 0x5, 0x5, 0x7 };
    /* perf-3 line-2 glyphs (Z:resolve P:raster_pass pkN nN qN). The lowercase
     * shapes drop the top row to distinguish them from the uppercase variants
     * already present (P, K not added since K wasn't used before; n is the
     * lowercase 'n' counterpart). */
    static const Uint8 glyph_Z[5] = { 0x7, 0x1, 0x2, 0x4, 0x7 };
    static const Uint8 glyph_K[5] = { 0x5, 0x5, 0x6, 0x5, 0x5 };
    static const Uint8 glyph_p[5] = { 0x0, 0x6, 0x5, 0x6, 0x4 };
    static const Uint8 glyph_k[5] = { 0x4, 0x5, 0x6, 0x5, 0x5 };
    static const Uint8 glyph_n[5] = { 0x0, 0x6, 0x5, 0x5, 0x5 };
    static const Uint8 glyph_q[5] = { 0x0, 0x7, 0x5, 0x7, 0x1 };

    const Uint8* glyph = glyph_blank;
    switch (ch) {
    case '0': glyph = glyph_0; break;
    case '1': glyph = glyph_1; break;
    case '2': glyph = glyph_2; break;
    case '3': glyph = glyph_3; break;
    case '4': glyph = glyph_4; break;
    case '5': glyph = glyph_5; break;
    case '6': glyph = glyph_6; break;
    case '7': glyph = glyph_7; break;
    case '8': glyph = glyph_8; break;
    case '9': glyph = glyph_9; break;
    case 'F': glyph = glyph_F; break;
    case 'P': glyph = glyph_P; break;
    case 'S': glyph = glyph_S; break;
    case 'U': glyph = glyph_U; break;
    case 'R': glyph = glyph_R; break;
    case 'T': glyph = glyph_T; break;
    case 'G': glyph = glyph_G; break;
    case 'D': glyph = glyph_D; break;
    case 'r': glyph = glyph_r; break;
    case 't': glyph = glyph_t; break;
    case 's': glyph = glyph_s; break;
    case '[': glyph = glyph_lbracket; break;
    case ']': glyph = glyph_rbracket; break;
    case '(': glyph = glyph_lparen; break;
    case ')': glyph = glyph_rparen; break;
    case ':': glyph = glyph_colon; break;
    case '.': glyph = glyph_dot; break;
    case '=': glyph = glyph_eq; break;
    case '/': glyph = glyph_slash; break;
    case '%': glyph = glyph_pct; break;
    case 'C': glyph = glyph_C; break;
    case 'L': glyph = glyph_L; break;
    case 'O': glyph = glyph_O; break;
    case 'e': glyph = glyph_e; break;
    case 'j': glyph = glyph_j; break;
    case 'a': glyph = glyph_a; break;
    case 'Z': glyph = glyph_Z; break;
    case 'K': glyph = glyph_K; break;
    case 'p': glyph = glyph_p; break;
    case 'k': glyph = glyph_k; break;
    case 'n': glyph = glyph_n; break;
    case 'q': glyph = glyph_q; break;
    case ' ':
    default:
        glyph = glyph_blank;
        break;
    }

    return glyph[row];
}

static void draw_overlay_glyph(Uint32* pixels,
                               int width,
                               int height,
                               int stride_pixels,
                               int x,
                               int y,
                               int scale,
                               char ch,
                               Uint32 color) {
    const int glyph_w = 3;
    const int glyph_h = 5;

    for (int row = 0; row < glyph_h; row++) {
        const Uint8 bits = overlay_glyph_row(ch, row);
        for (int col = 0; col < glyph_w; col++) {
            if ((bits & (1u << (glyph_w - 1 - col))) == 0) {
                continue;
            }

            fill_argb_rect(
                pixels, width, height, stride_pixels, x + (col * scale), y + (row * scale), scale, scale, color);
        }
    }
}

static bool ensure_fps_overlay_buffer(int width, int height) {
    if ((width <= 0) || (height <= 0)) {
        return false;
    }

    if ((fps_overlay_pixels != NULL) && (fps_overlay_width == width) && (fps_overlay_height == height)) {
        return true;
    }

    SDL_free(fps_overlay_pixels);
    fps_overlay_pixels = (Uint32*)SDL_malloc(sizeof(Uint32) * (size_t)width * (size_t)height);
    if (fps_overlay_pixels == NULL) {
        fps_overlay_width = 0;
        fps_overlay_height = 0;
        fps_overlay_cache_valid = false;
        return false;
    }

    fps_overlay_width = width;
    fps_overlay_height = height;
    fps_overlay_cache_valid = false;
    return true;
}

static bool ensure_rasterized_fps_overlay(const FpsOverlayLayout* layout) {
    if ((layout == NULL) || !ensure_fps_overlay_buffer(layout->width, layout->height)) {
        return false;
    }

    const bool layout_changed = !fps_overlay_cache_valid || (fps_overlay_cached_draw_x != layout->draw_x) ||
                                (fps_overlay_cached_draw_y != layout->draw_y) || (fps_overlay_cached_text_x != layout->text_x) ||
                                (fps_overlay_cached_text_y != layout->text_y) || (fps_overlay_cached_scale != layout->scale) ||
                                (SDL_strcmp(fps_overlay_cached_text, fps_overlay_text) != 0);
    if (!layout_changed) {
        return true;
    }

    fill_argb_rect(
        fps_overlay_pixels, fps_overlay_width, fps_overlay_height, fps_overlay_width, 0, 0, fps_overlay_width, fps_overlay_height, 0xFF000000u);

    const int glyph_w = 3 * layout->scale;
    const int glyph_h = 5 * layout->scale;
    const int char_gap = layout->scale;
    const int line_gap = layout->scale;
    const int text_len = (int)SDL_strlen(fps_overlay_text);
    /* Walk the overlay text glyph-by-glyph; on '\n' advance the y offset by
     * glyph_h + line_gap and reset the column index. */
    int line_index = 0;
    int col_index = 0;
    for (int i = 0; i < text_len; i++) {
        const char ch = fps_overlay_text[i];
        if (ch == '\n') {
            line_index += 1;
            col_index = 0;
            continue;
        }
        const int glyph_x = layout->text_x + (col_index * (glyph_w + char_gap));
        const int glyph_y = layout->text_y + (line_index * (glyph_h + line_gap));
        draw_overlay_glyph(fps_overlay_pixels,
                           fps_overlay_width,
                           fps_overlay_height,
                           fps_overlay_width,
                           glyph_x + 1,
                           glyph_y + 1,
                           layout->scale,
                           ch,
                           0xFF000000u);
        draw_overlay_glyph(fps_overlay_pixels,
                           fps_overlay_width,
                           fps_overlay_height,
                           fps_overlay_width,
                           glyph_x,
                           glyph_y,
                           layout->scale,
                           ch,
                           0xFFFFFFFFu);
        col_index += 1;
    }

    fps_overlay_cached_draw_x = layout->draw_x;
    fps_overlay_cached_draw_y = layout->draw_y;
    fps_overlay_cached_text_x = layout->text_x;
    fps_overlay_cached_text_y = layout->text_y;
    fps_overlay_cached_scale = layout->scale;
    SDL_snprintf(fps_overlay_cached_text, sizeof(fps_overlay_cached_text), "%s", fps_overlay_text);
    fps_overlay_cache_valid = true;
    return true;
}

static void apply_rasterized_fps_overlay_to_argb_buffer(Uint32* pixels, int width, int height, int stride_pixels) {
    if (!fps_overlay_frame_active || (pixels == NULL) || (fps_overlay_pixels == NULL) || (stride_pixels < width)) {
        return;
    }

    const FpsOverlayLayout* layout = &fps_overlay_frame_layout;
    const size_t row_bytes = (size_t)layout->width * sizeof(Uint32);
    for (int row = 0; row < layout->height; row++) {
        const int dst_y = layout->draw_y + row;
        if ((dst_y < 0) || (dst_y >= height)) {
            continue;
        }

        const Uint8* src_row = (const Uint8*)(fps_overlay_pixels + ((size_t)fps_overlay_width * (size_t)row));
        Uint8* dst_row = (Uint8*)(pixels + ((size_t)stride_pixels * (size_t)dst_y)) + ((size_t)layout->draw_x * sizeof(Uint32));
        SDL_memcpy(dst_row, src_row, row_bytes);
    }
}

/* Per-pixel ARGB->565 pack at FPS-overlay rate is trivial CPU (overlay is at
 * most ~32x7 px); avoids carrying a separate 565 atlas cache field. */
static void apply_rasterized_fps_overlay_to_rgb565_buffer(Uint16* pixels, int width, int height, int stride_pixels) {
    if (!fps_overlay_frame_active || (pixels == NULL) || (fps_overlay_pixels == NULL) || (stride_pixels < width)) {
        return;
    }

    const FpsOverlayLayout* layout = &fps_overlay_frame_layout;
    for (int row = 0; row < layout->height; row++) {
        const int dst_y = layout->draw_y + row;
        if ((dst_y < 0) || (dst_y >= height)) {
            continue;
        }
        const Uint32* src_row = fps_overlay_pixels + ((size_t)fps_overlay_width * (size_t)row);
        Uint16* dst_row = pixels + ((size_t)stride_pixels * (size_t)dst_y) + (size_t)layout->draw_x;
        for (int x = 0; x < layout->width; x++) {
            const Uint32 argb = src_row[x];
            const Uint32 r = (argb >> 16) & 0xFFu;
            const Uint32 g = (argb >> 8) & 0xFFu;
            const Uint32 b = argb & 0xFFu;
            dst_row[x] = (Uint16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

/* Count lines (separated by '\n') in fps_overlay_text and the widest line's
 * char count. Both Apply* layout helpers below use this to size the bg rect
 * for a multi-line debug overlay; without it line 2 renders into an
 * undersized buffer and is clipped offscreen. */
static void compute_fps_overlay_text_dims(int* out_max_line_chars, int* out_line_count) {
    const int text_len = (int)SDL_strlen(fps_overlay_text);
    int line_count = 1;
    int max_line_chars = 0;
    int current_line_chars = 0;
    for (int i = 0; i < text_len; i++) {
        if (fps_overlay_text[i] == '\n') {
            if (current_line_chars > max_line_chars) {
                max_line_chars = current_line_chars;
            }
            current_line_chars = 0;
            line_count += 1;
        } else {
            current_line_chars += 1;
        }
    }
    if (current_line_chars > max_line_chars) {
        max_line_chars = current_line_chars;
    }
    if (out_max_line_chars != NULL) {
        *out_max_line_chars = max_line_chars;
    }
    if (out_line_count != NULL) {
        *out_line_count = line_count;
    }
}

void FPSOverlay_SetMode(int mode) {
    fps_overlay_mode = mode;
    if (mode == 0) {
        fps_overlay_text[0] = '\0';
    }
}

void FPSOverlay_SetText(const char* text) {
    if (text == NULL) {
        fps_overlay_text[0] = '\0';
        return;
    }

    SDL_snprintf(fps_overlay_text, sizeof(fps_overlay_text), "%s", text);
}

void FPSOverlay_ApplyToBuffer(Uint32* pixels, int width, int height) {
    if ((fps_overlay_mode == 0) || (fps_overlay_text[0] == '\0') || (pixels == NULL) || (width <= 0) || (height <= 0)) {
        return;
    }

    /* Compute layout for the given buffer dimensions (standalone, not
     * dependent on fb_width/fb_height). */
    int scale = 1;
    if (height >= 700) {
        scale = 4;
    } else if (height >= 420) {
        scale = 3;
    } else if (height >= 350) {
        scale = 2;
    }

    const int glyph_w = 3 * scale;
    const int glyph_h = 5 * scale;
    const int char_gap = scale;
    const int line_gap = scale;
    int max_line_chars = 0;
    int line_count = 1;
    compute_fps_overlay_text_dims(&max_line_chars, &line_count);
    if (max_line_chars <= 0) {
        return;
    }

    const int text_w = (max_line_chars * glyph_w) + ((max_line_chars - 1) * char_gap);
    const int total_h = (line_count * glyph_h) + ((line_count - 1) * line_gap);
    const int safe_margin = SDL_max(4, scale * 2);
    const int bg_pad = SDL_max(1, scale);

    FpsOverlayLayout layout;
    if (fps_overlay_mode == 1) {
        /* FPS mode: top-left corner */
        layout.draw_x = SDL_max(0, safe_margin - bg_pad);
        layout.draw_y = SDL_max(0, safe_margin - bg_pad);
    } else {
        /* Debug mode: bottom-center, anchored so the bottom edge stays at
         * the same y as the legacy single-line overlay (extra lines extend
         * upward). */
        layout.draw_x = SDL_max(0, (width - text_w) / 2 - bg_pad);
        layout.draw_y = SDL_max(0, height - total_h - safe_margin - bg_pad);
    }
    layout.width = SDL_min(text_w + 2 * bg_pad, width - layout.draw_x);
    layout.height = SDL_min(total_h + 2 * bg_pad, height - layout.draw_y);
    layout.text_x = bg_pad;
    layout.text_y = bg_pad;
    layout.scale = scale;

    if (!ensure_rasterized_fps_overlay(&layout)) {
        return;
    }

    fps_overlay_frame_layout = layout;
    fps_overlay_frame_active = true;
    apply_rasterized_fps_overlay_to_argb_buffer(pixels, width, height, width);
    fps_overlay_frame_active = false;
}

void FPSOverlay_ApplyToRGB565Buffer(Uint16* pixels, int width, int height) {
    if ((fps_overlay_mode == 0) || (fps_overlay_text[0] == '\0') || (pixels == NULL) || (width <= 0) || (height <= 0)) {
        return;
    }

    int scale = 1;
    if (height >= 700) {
        scale = 4;
    } else if (height >= 420) {
        scale = 3;
    } else if (height >= 350) {
        scale = 2;
    }

    const int glyph_w = 3 * scale;
    const int glyph_h = 5 * scale;
    const int char_gap = scale;
    const int line_gap = scale;
    int max_line_chars = 0;
    int line_count = 1;
    compute_fps_overlay_text_dims(&max_line_chars, &line_count);
    if (max_line_chars <= 0) {
        return;
    }

    const int text_w = (max_line_chars * glyph_w) + ((max_line_chars - 1) * char_gap);
    const int total_h = (line_count * glyph_h) + ((line_count - 1) * line_gap);
    const int safe_margin = SDL_max(4, scale * 2);
    const int bg_pad = SDL_max(1, scale);

    FpsOverlayLayout layout;
    if (fps_overlay_mode == 1) {
        layout.draw_x = SDL_max(0, safe_margin - bg_pad);
        layout.draw_y = SDL_max(0, safe_margin - bg_pad);
    } else {
        layout.draw_x = SDL_max(0, (width - text_w) / 2 - bg_pad);
        layout.draw_y = SDL_max(0, height - total_h - safe_margin - bg_pad);
    }
    layout.width = SDL_min(text_w + 2 * bg_pad, width - layout.draw_x);
    layout.height = SDL_min(total_h + 2 * bg_pad, height - layout.draw_y);
    layout.text_x = bg_pad;
    layout.text_y = bg_pad;
    layout.scale = scale;

    if (!ensure_rasterized_fps_overlay(&layout)) {
        return;
    }

    fps_overlay_frame_layout = layout;
    fps_overlay_frame_active = true;
    apply_rasterized_fps_overlay_to_rgb565_buffer(pixels, width, height, width);
    fps_overlay_frame_active = false;
}
