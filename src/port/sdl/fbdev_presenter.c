#include "port/sdl/fbdev_presenter.h"
#include "port/sdl/sdl_game_renderer.h"

#if defined(PORT_MISTER)

#include <linux/fb.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define FBDEV_HAVE_NEON 1
#else
#define FBDEV_HAVE_NEON 0
#endif

static int fb_fd = -1;
static Uint8* fb_map = NULL;
static size_t fb_map_len = 0;
static int fb_width = 0;
static int fb_height = 0;
static int fb_stride = 0;
static bool fbdev_active = false;
static size_t frame_copy_bytes = 0;
static int* scale_x_lut = NULL;
static int* scale_y_lut = NULL;
static int scale_lut_src_w = 0;
static int scale_lut_src_h = 0;
static int scale_lut_raw_x0 = 0;
static int scale_lut_raw_y0 = 0;
static int scale_lut_raw_w = 0;
static int scale_lut_raw_h = 0;
static bool rect_bar_clear_valid = false;
static int rect_bar_clear_x0 = 0;
static int rect_bar_clear_y0 = 0;
static int rect_bar_clear_x1 = 0;
static int rect_bar_clear_y1 = 0;
static Uint32* staging_pixels = NULL;
static Uint32* previous_pixels = NULL;
static int staging_width = 0;
static int staging_height = 0;
static bool previous_pixels_valid = false;
static int frame_tiles_total = 0;
static int frame_tiles_copied = 0;
static bool frame_full_copy_fallback = false;
static FBDevPresenter_FrameStats frame_stats = { 0 };
static bool fps_overlay_enabled = false;
static char fps_overlay_text[16] = "";
#if ENABLE_PERF_TELEMETRY
static bool frame_stats_breakdown_enabled = false;
#else
static const bool frame_stats_breakdown_enabled = false;
#endif

enum {
    staging_tile_size = 16,
    staging_full_copy_guardrail_percent = 80,
};

static int clamp_to_range(int value, int min, int max) {
    if (value < min) {
        return min;
    }

    if (value > max) {
        return max;
    }

    return value;
}

static Uint64 frame_stats_now(void) {
    if (!frame_stats_breakdown_enabled) {
        return 0;
    }

    return SDL_GetTicksNS();
}

static void frame_stats_add_duration(Uint64* total, Uint64 start_ns) {
    if (!frame_stats_breakdown_enabled || (start_ns == 0) || (total == NULL)) {
        return;
    }

    const Uint64 end_ns = SDL_GetTicksNS();
    if (end_ns > start_ns) {
        *total += end_ns - start_ns;
    }
}

static void frame_stats_note_readback_surface(const SDL_Surface* surface) {
    if (!frame_stats_breakdown_enabled || (surface == NULL)) {
        return;
    }

    frame_stats.readback_format = surface->format;
    frame_stats.readback_width = surface->w;
    frame_stats.readback_height = surface->h;
}

static void frame_stats_note_path(FBDevPresenterPath path) {
#if ENABLE_PERF_TELEMETRY
    frame_stats.path = path;
#else
    (void)path;
#endif
}

static void reset_scale_lut() {
    SDL_free(scale_x_lut);
    SDL_free(scale_y_lut);
    scale_x_lut = NULL;
    scale_y_lut = NULL;
    scale_lut_src_w = 0;
    scale_lut_src_h = 0;
    scale_lut_raw_x0 = 0;
    scale_lut_raw_y0 = 0;
    scale_lut_raw_w = 0;
    scale_lut_raw_h = 0;
}

static void invalidate_rect_bar_clear_cache() {
    rect_bar_clear_valid = false;
}

static void mark_rect_bar_clear_cache(int x0, int y0, int x1, int y1) {
    rect_bar_clear_valid = true;
    rect_bar_clear_x0 = x0;
    rect_bar_clear_y0 = y0;
    rect_bar_clear_x1 = x1;
    rect_bar_clear_y1 = y1;
}

static void reset_staging_buffers() {
    SDL_free(staging_pixels);
    SDL_free(previous_pixels);
    staging_pixels = NULL;
    previous_pixels = NULL;
    staging_width = 0;
    staging_height = 0;
    previous_pixels_valid = false;
}

static bool ensure_staging_buffers(int width, int height) {
    if ((width <= 0) || (height <= 0)) {
        return false;
    }

    if ((staging_pixels != NULL) && (previous_pixels != NULL) && (staging_width == width) && (staging_height == height)) {
        return true;
    }

    reset_staging_buffers();

    const size_t pixel_count = (size_t)width * (size_t)height;
    staging_pixels = (Uint32*)SDL_malloc(pixel_count * sizeof(Uint32));
    previous_pixels = (Uint32*)SDL_malloc(pixel_count * sizeof(Uint32));
    if ((staging_pixels == NULL) || (previous_pixels == NULL)) {
        reset_staging_buffers();
        return false;
    }

    staging_width = width;
    staging_height = height;
    previous_pixels_valid = false;
    return true;
}

static void memset32(Uint32* dst, Uint32 value, size_t count) {
#if FBDEV_HAVE_NEON
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

static bool get_content_rect_bounds(const SDL_FRect* content_rect, int* out_x0, int* out_y0, int* out_x1, int* out_y1) {
    if (content_rect == NULL) {
        return false;
    }

    const int x0 = clamp_to_range((int)SDL_floorf(content_rect->x), 0, fb_width);
    const int y0 = clamp_to_range((int)SDL_floorf(content_rect->y), 0, fb_height);
    const int x1 = clamp_to_range((int)SDL_ceilf(content_rect->x + content_rect->w), 0, fb_width);
    const int y1 = clamp_to_range((int)SDL_ceilf(content_rect->y + content_rect->h), 0, fb_height);

    if ((x1 <= x0) || (y1 <= y0)) {
        return false;
    }

    *out_x0 = x0;
    *out_y0 = y0;
    *out_x1 = x1;
    *out_y1 = y1;
    return true;
}

static void close_fb() {
    reset_scale_lut();
    invalidate_rect_bar_clear_cache();
    reset_staging_buffers();

    if (fb_map != NULL) {
        munmap(fb_map, fb_map_len);
        fb_map = NULL;
        fb_map_len = 0;
    }

    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
}

static bool ensure_scale_lut(int src_w, int src_h, int raw_x0, int raw_y0, int raw_w, int raw_h) {
    if ((src_w <= 0) || (src_h <= 0) || (raw_w <= 0) || (raw_h <= 0) || (fb_width <= 0) || (fb_height <= 0)) {
        return false;
    }

    if ((scale_x_lut != NULL) && (scale_y_lut != NULL) && (scale_lut_src_w == src_w) && (scale_lut_src_h == src_h) &&
        (scale_lut_raw_x0 == raw_x0) && (scale_lut_raw_y0 == raw_y0) && (scale_lut_raw_w == raw_w) &&
        (scale_lut_raw_h == raw_h)) {
        return true;
    }

    if (scale_x_lut == NULL) {
        scale_x_lut = (int*)SDL_malloc(sizeof(int) * (size_t)fb_width);
        if (scale_x_lut == NULL) {
            return false;
        }
    }

    if (scale_y_lut == NULL) {
        scale_y_lut = (int*)SDL_malloc(sizeof(int) * (size_t)fb_height);
        if (scale_y_lut == NULL) {
            return false;
        }
    }

    const int lut_x0 = clamp_to_range(raw_x0, 0, fb_width);
    const int lut_x1 = clamp_to_range(raw_x0 + raw_w, 0, fb_width);
    for (int x = lut_x0; x < lut_x1; x++) {
        scale_x_lut[x] = (int)(((Sint64)(x - raw_x0) * (Sint64)src_w) / (Sint64)raw_w);
    }

    const int lut_y0 = clamp_to_range(raw_y0, 0, fb_height);
    const int lut_y1 = clamp_to_range(raw_y0 + raw_h, 0, fb_height);
    for (int y = lut_y0; y < lut_y1; y++) {
        scale_y_lut[y] = (int)(((Sint64)(y - raw_y0) * (Sint64)src_h) / (Sint64)raw_h);
    }

    scale_lut_src_w = src_w;
    scale_lut_src_h = src_h;
    scale_lut_raw_x0 = raw_x0;
    scale_lut_raw_y0 = raw_y0;
    scale_lut_raw_w = raw_w;
    scale_lut_raw_h = raw_h;
    return true;
}

static void copy_argb_surface_rect_to_fb_offset(const SDL_Surface* argb,
                                                int src_x,
                                                int src_y,
                                                int width,
                                                int height,
                                                int dst_x,
                                                int dst_y) {
    const size_t row_bytes = (size_t)width * sizeof(Uint32);
    const Uint8* src = ((const Uint8*)argb->pixels) + ((size_t)argb->pitch * (size_t)src_y) + ((size_t)src_x * sizeof(Uint32));
    Uint8* dst = fb_map + ((size_t)fb_stride * (size_t)dst_y) + ((size_t)dst_x * sizeof(Uint32));

    if ((src_x == 0) && (dst_x == 0) && (width == fb_width) &&
        (argb->pitch == (int)row_bytes) && (fb_stride == (int)row_bytes)) {
        SDL_memcpy(dst, src, row_bytes * (size_t)height);
        frame_copy_bytes += row_bytes * (size_t)height;
        return;
    }

    for (int y = 0; y < height; y++) {
        const Uint8* src_row = src + ((size_t)argb->pitch * (size_t)y);
        Uint8* dst_row = dst + ((size_t)fb_stride * (size_t)y);
        SDL_memcpy(dst_row, src_row, row_bytes);
    }

    frame_copy_bytes += row_bytes * (size_t)height;
}

static void copy_argb_surface_to_fb_offset(const SDL_Surface* argb, int dst_x, int dst_y) {
    copy_argb_surface_rect_to_fb_offset(argb, 0, 0, argb->w, argb->h, dst_x, dst_y);
}

static void copy_argb_surface_to_fb(const SDL_Surface* argb) {
    copy_argb_surface_to_fb_offset(argb, 0, 0);
}

static void clear_fb_rect(int x, int y, int w, int h) {
    if ((w <= 0) || (h <= 0)) {
        return;
    }

    const size_t row_pixels = (size_t)w;
    for (int row = 0; row < h; row++) {
        Uint32* dst_row = (Uint32*)(fb_map + ((size_t)fb_stride * (size_t)(y + row)) + ((size_t)x * sizeof(Uint32)));
        memset32(dst_row, 0, row_pixels);
    }

    frame_copy_bytes += row_pixels * sizeof(Uint32) * (size_t)h;
}

static void fill_fb_rect(int x, int y, int w, int h, Uint32 color) {
    if ((w <= 0) || (h <= 0)) {
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
    if ((x >= fb_width) || (y >= fb_height)) {
        return;
    }

    if ((x + w) > fb_width) {
        w = fb_width - x;
    }
    if ((y + h) > fb_height) {
        h = fb_height - y;
    }
    if ((w <= 0) || (h <= 0)) {
        return;
    }

    const size_t row_pixels = (size_t)w;
    for (int row = 0; row < h; row++) {
        Uint32* dst_row = (Uint32*)(fb_map + ((size_t)fb_stride * (size_t)(y + row)) + ((size_t)x * sizeof(Uint32)));
        memset32(dst_row, color, row_pixels);
    }

    frame_copy_bytes += row_pixels * sizeof(Uint32) * (size_t)h;
}

static void clear_fb_outside_rect(int x0, int y0, int x1, int y1) {
    // Top and bottom bars.
    clear_fb_rect(0, 0, fb_width, y0);
    clear_fb_rect(0, y1, fb_width, fb_height - y1);

    // Left and right bars in the active vertical span.
    clear_fb_rect(0, y0, x0, y1 - y0);
    clear_fb_rect(x1, y0, fb_width - x1, y1 - y0);
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

    const Uint8* glyph = glyph_blank;
    switch (ch) {
    case '0':
        glyph = glyph_0;
        break;
    case '1':
        glyph = glyph_1;
        break;
    case '2':
        glyph = glyph_2;
        break;
    case '3':
        glyph = glyph_3;
        break;
    case '4':
        glyph = glyph_4;
        break;
    case '5':
        glyph = glyph_5;
        break;
    case '6':
        glyph = glyph_6;
        break;
    case '7':
        glyph = glyph_7;
        break;
    case '8':
        glyph = glyph_8;
        break;
    case '9':
        glyph = glyph_9;
        break;
    case 'F':
        glyph = glyph_F;
        break;
    case 'P':
        glyph = glyph_P;
        break;
    case 'S':
        glyph = glyph_S;
        break;
    case ' ':
    default:
        glyph = glyph_blank;
        break;
    }

    return glyph[row];
}

static void draw_overlay_glyph(int x, int y, int scale, char ch, Uint32 color) {
    const int glyph_w = 3;
    const int glyph_h = 5;

    for (int row = 0; row < glyph_h; row++) {
        const Uint8 bits = overlay_glyph_row(ch, row);
        for (int col = 0; col < glyph_w; col++) {
            if ((bits & (1u << (glyph_w - 1 - col))) == 0) {
                continue;
            }

            fill_fb_rect(x + (col * scale), y + (row * scale), scale, scale, color);
        }
    }
}

static void maybe_draw_fps_overlay(const SDL_FRect* content_rect) {
    if (!fps_overlay_enabled || (fps_overlay_text[0] == '\0') || !fbdev_active || (fb_map == NULL)) {
        return;
    }

    int x0 = 0;
    int y0 = 0;
    int x1 = fb_width;
    int y1 = fb_height;
    if (!get_content_rect_bounds(content_rect, &x0, &y0, &x1, &y1)) {
        x0 = 0;
        y0 = 0;
        x1 = fb_width;
        y1 = fb_height;
    }

    const int content_w = x1 - x0;
    const int content_h = y1 - y0;
    if ((content_w <= 0) || (content_h <= 0)) {
        return;
    }

    int scale = 2;
    if (content_h >= 700) {
        scale = 4;
    } else if (content_h >= 420) {
        scale = 3;
    }

    const int glyph_w = 3 * scale;
    const int glyph_h = 5 * scale;
    const int char_gap = scale;
    const int text_len = (int)SDL_strlen(fps_overlay_text);
    if (text_len <= 0) {
        return;
    }

    const int text_w = (text_len * glyph_w) + ((text_len - 1) * char_gap);
    const int safe_margin = SDL_max(10, scale * 4);
    const int draw_x = x0 + ((content_w - text_w) / 2);
    const int draw_y = y1 - glyph_h - safe_margin;

    for (int i = 0; i < text_len; i++) {
        const int glyph_x = draw_x + (i * (glyph_w + char_gap));
        draw_overlay_glyph(glyph_x + 1, draw_y + 1, scale, fps_overlay_text[i], 0xFF000000u);
        draw_overlay_glyph(glyph_x, draw_y, scale, fps_overlay_text[i], 0xFFFFFFFFu);
    }
}

static void clear_previous_rect(int x, int y, int w, int h) {
    if (!previous_pixels_valid || (previous_pixels == NULL) || (w <= 0) || (h <= 0)) {
        return;
    }

    for (int row = 0; row < h; row++) {
        Uint32* row_pixels = previous_pixels + ((size_t)staging_width * (size_t)(y + row)) + (size_t)x;
        memset32(row_pixels, 0, (size_t)w);
    }
}

static void sync_previous_outside_rect_clear(int x0, int y0, int x1, int y1) {
    if (!previous_pixels_valid || (previous_pixels == NULL) || (staging_width != fb_width) || (staging_height != fb_height)) {
        return;
    }

    clear_previous_rect(0, 0, fb_width, y0);
    clear_previous_rect(0, y1, fb_width, fb_height - y1);
    clear_previous_rect(0, y0, x0, y1 - y0);
    clear_previous_rect(x1, y0, fb_width - x1, y1 - y0);
}

static bool copy_surface_to_fb_offset(const SDL_Surface* src, int dst_x, int dst_y) {
    if ((src->w <= 0) || (src->h <= 0)) {
        return false;
    }

    if ((dst_x < 0) || (dst_y < 0) || ((dst_x + src->w) > fb_width) || ((dst_y + src->h) > fb_height)) {
        return false;
    }

    if (src->format == SDL_PIXELFORMAT_ARGB8888) {
        const Uint64 copy_start_ns = frame_stats_now();
        copy_argb_surface_to_fb_offset(src, dst_x, dst_y);
        frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
        return true;
    }

    Uint8* dst = fb_map + ((size_t)fb_stride * (size_t)dst_y) + ((size_t)dst_x * sizeof(Uint32));
    Uint64 convert_start_ns = frame_stats_now();
    if (SDL_ConvertPixels(src->w,
                          src->h,
                          src->format,
                          src->pixels,
                          src->pitch,
                          SDL_PIXELFORMAT_ARGB8888,
                          dst,
                          fb_stride)) {
        frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
        frame_copy_bytes += ((size_t)src->w * (size_t)src->h) * sizeof(Uint32);
        return true;
    }
    frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);

    convert_start_ns = frame_stats_now();
    SDL_Surface* argb = SDL_ConvertSurface(src, SDL_PIXELFORMAT_ARGB8888);
    frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
    if (argb == NULL) {
        return false;
    }

    const Uint64 copy_start_ns = frame_stats_now();
    copy_argb_surface_to_fb_offset(argb, dst_x, dst_y);
    frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
    SDL_DestroySurface(argb);
    return true;
}

static bool copy_surface_rect_to_fb_offset(const SDL_Surface* src,
                                           int src_x,
                                           int src_y,
                                           int width,
                                           int height,
                                           int dst_x,
                                           int dst_y) {
    if ((width <= 0) || (height <= 0)) {
        return false;
    }

    if ((src_x < 0) || (src_y < 0) || ((src_x + width) > src->w) || ((src_y + height) > src->h)) {
        return false;
    }

    if ((dst_x < 0) || (dst_y < 0) || ((dst_x + width) > fb_width) || ((dst_y + height) > fb_height)) {
        return false;
    }

    if (src->format == SDL_PIXELFORMAT_ARGB8888) {
        const Uint64 copy_start_ns = frame_stats_now();
        copy_argb_surface_rect_to_fb_offset(src, src_x, src_y, width, height, dst_x, dst_y);
        frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
        return true;
    }

    if (!SDL_ISPIXELFORMAT_FOURCC(src->format)) {
        const int src_bpp = SDL_BYTESPERPIXEL(src->format);
        if (src_bpp > 0) {
            const Uint8* src_pixels =
                ((const Uint8*)src->pixels) + ((size_t)src->pitch * (size_t)src_y) + ((size_t)src_x * (size_t)src_bpp);
            Uint8* dst_pixels = fb_map + ((size_t)fb_stride * (size_t)dst_y) + ((size_t)dst_x * sizeof(Uint32));
            Uint64 convert_start_ns = frame_stats_now();
            if (SDL_ConvertPixels(width,
                                  height,
                                  src->format,
                                  src_pixels,
                                  src->pitch,
                                  SDL_PIXELFORMAT_ARGB8888,
                                  dst_pixels,
                                  fb_stride)) {
                frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
                frame_copy_bytes += ((size_t)width * (size_t)height) * sizeof(Uint32);
                return true;
            }
            frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
        }
    }

    Uint64 convert_start_ns = frame_stats_now();
    SDL_Surface* argb = SDL_ConvertSurface(src, SDL_PIXELFORMAT_ARGB8888);
    frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
    if (argb == NULL) {
        return false;
    }

    const Uint64 copy_start_ns = frame_stats_now();
    copy_argb_surface_rect_to_fb_offset(argb, src_x, src_y, width, height, dst_x, dst_y);
    frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
    SDL_DestroySurface(argb);
    return true;
}

static bool copy_argb_surface_scaled_to_fb_mapped_rect(const SDL_Surface* argb,
                                                       int raw_x0,
                                                       int raw_y0,
                                                       int raw_w,
                                                       int raw_h,
                                                       int clip_x0,
                                                       int clip_y0,
                                                       int clip_x1,
                                                       int clip_y1) {
    if ((raw_w <= 0) || (raw_h <= 0) || (clip_x1 <= clip_x0) || (clip_y1 <= clip_y0)) {
        return false;
    }

    const size_t row_bytes = (size_t)(clip_x1 - clip_x0) * sizeof(Uint32);
    const bool have_lut = ensure_scale_lut(argb->w, argb->h, raw_x0, raw_y0, raw_w, raw_h);
    int prev_src_y = -1;
    Uint8* prev_dst_row = NULL;

    for (int y = clip_y0; y < clip_y1; y++) {
        int src_y = have_lut ? scale_y_lut[y] : (int)(((Sint64)(y - raw_y0) * (Sint64)argb->h) / (Sint64)raw_h);
        src_y = clamp_to_range(src_y, 0, argb->h - 1);

        Uint8* dst_row_bytes = fb_map + ((size_t)fb_stride * (size_t)y) + ((size_t)clip_x0 * sizeof(Uint32));
        if ((src_y == prev_src_y) && (prev_dst_row != NULL)) {
            SDL_memcpy(dst_row_bytes, prev_dst_row, row_bytes);
            frame_copy_bytes += row_bytes;
            continue;
        }

        const Uint32* src_row = (const Uint32*)(((const Uint8*)argb->pixels) + ((size_t)argb->pitch * (size_t)src_y));
        Uint32* dst_row = (Uint32*)dst_row_bytes;

        for (int x = clip_x0; x < clip_x1; x++) {
            int src_x = have_lut ? scale_x_lut[x] : (int)(((Sint64)(x - raw_x0) * (Sint64)argb->w) / (Sint64)raw_w);
            src_x = clamp_to_range(src_x, 0, argb->w - 1);
            dst_row[x - clip_x0] = src_row[src_x];
        }

        frame_copy_bytes += row_bytes;
        prev_src_y = src_y;
        prev_dst_row = dst_row_bytes;
    }

    return true;
}

static bool copy_argb_surface_integer_scaled_to_fb_rect(const SDL_Surface* argb,
                                                        int raw_x0,
                                                        int raw_y0,
                                                        int raw_w,
                                                        int raw_h,
                                                        int clip_x0,
                                                        int clip_y0,
                                                        int clip_x1,
                                                        int clip_y1) {
    // Exact integer expansion avoids the generic path's per-pixel divides on common MiSTer square-pixels output.
    if ((argb == NULL) || (argb->w <= 0) || (argb->h <= 0)) {
        return false;
    }

    if ((raw_x0 != clip_x0) || (raw_y0 != clip_y0) || ((raw_x0 + raw_w) != clip_x1) || ((raw_y0 + raw_h) != clip_y1)) {
        return false;
    }

    if ((raw_w % argb->w) != 0 || (raw_h % argb->h) != 0) {
        return false;
    }

    const int scale_x = raw_w / argb->w;
    const int scale_y = raw_h / argb->h;
    if ((scale_x <= 0) || (scale_y <= 0)) {
        return false;
    }

    const size_t row_bytes = (size_t)raw_w * sizeof(Uint32);

    for (int src_y = 0; src_y < argb->h; src_y++) {
        const Uint32* src_row = (const Uint32*)(((const Uint8*)argb->pixels) + ((size_t)argb->pitch * (size_t)src_y));
        Uint8* dst_row_bytes =
            fb_map + ((size_t)fb_stride * ((size_t)raw_y0 + ((size_t)src_y * (size_t)scale_y))) +
            ((size_t)raw_x0 * sizeof(Uint32));
        Uint32* dst_row = (Uint32*)dst_row_bytes;
        int dst_x = 0;

        for (int src_x = 0; src_x < argb->w; src_x++) {
            const Uint32 pixel = src_row[src_x];
            for (int repeat_x = 0; repeat_x < scale_x; repeat_x++) {
                dst_row[dst_x++] = pixel;
            }
        }

        frame_copy_bytes += row_bytes;

        for (int repeat_y = 1; repeat_y < scale_y; repeat_y++) {
            Uint8* repeated_row = dst_row_bytes + ((size_t)fb_stride * (size_t)repeat_y);
            SDL_memcpy(repeated_row, dst_row_bytes, row_bytes);
            frame_copy_bytes += row_bytes;
        }
    }

    return true;
}

static bool copy_surface_to_fb_mapped_rect_with_paths(const SDL_Surface* src,
                                                      const SDL_FRect* dst_rect,
                                                      FBDevPresenterPath exact_path,
                                                      FBDevPresenterPath integer_scale_path,
                                                      FBDevPresenterPath mapped_scale_path) {
    if ((src == NULL) || (dst_rect == NULL) || (src->w <= 0) || (src->h <= 0)) {
        return false;
    }

    const int raw_x0 = (int)SDL_floorf(dst_rect->x);
    const int raw_y0 = (int)SDL_floorf(dst_rect->y);
    const int raw_x1 = (int)SDL_ceilf(dst_rect->x + dst_rect->w);
    const int raw_y1 = (int)SDL_ceilf(dst_rect->y + dst_rect->h);
    const int raw_w = raw_x1 - raw_x0;
    const int raw_h = raw_y1 - raw_y0;
    if ((raw_w <= 0) || (raw_h <= 0)) {
        return false;
    }

    const int clip_x0 = clamp_to_range(raw_x0, 0, fb_width);
    const int clip_y0 = clamp_to_range(raw_y0, 0, fb_height);
    const int clip_x1 = clamp_to_range(raw_x1, 0, fb_width);
    const int clip_y1 = clamp_to_range(raw_y1, 0, fb_height);
    if ((clip_x1 <= clip_x0) || (clip_y1 <= clip_y0)) {
        return false;
    }

    if ((raw_w == src->w) && (raw_h == src->h)) {
        const int src_x = clip_x0 - raw_x0;
        const int src_y = clip_y0 - raw_y0;
        frame_stats_note_path(exact_path);
        return copy_surface_rect_to_fb_offset(
            src, src_x, src_y, clip_x1 - clip_x0, clip_y1 - clip_y0, clip_x0, clip_y0);
    }

    const SDL_Surface* argb = src;
    SDL_Surface* converted = NULL;
    if (src->format != SDL_PIXELFORMAT_ARGB8888) {
        const Uint64 convert_start_ns = frame_stats_now();
        converted = SDL_ConvertSurface(src, SDL_PIXELFORMAT_ARGB8888);
        frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
        if (converted == NULL) {
            return false;
        }
        argb = converted;
    }

    bool copied = false;
    if ((raw_x0 == clip_x0) && (raw_y0 == clip_y0) && ((raw_x0 + raw_w) == clip_x1) && ((raw_y0 + raw_h) == clip_y1) &&
        ((raw_w % argb->w) == 0) && ((raw_h % argb->h) == 0)) {
        frame_stats_note_path(integer_scale_path);
    } else {
        frame_stats_note_path(mapped_scale_path);
    }
    Uint64 copy_start_ns = frame_stats_now();
    copied =
        copy_argb_surface_integer_scaled_to_fb_rect(argb, raw_x0, raw_y0, raw_w, raw_h, clip_x0, clip_y0, clip_x1, clip_y1);
    if (!copied) {
        copied =
            copy_argb_surface_scaled_to_fb_mapped_rect(argb, raw_x0, raw_y0, raw_w, raw_h, clip_x0, clip_y0, clip_x1, clip_y1);
    }
    frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
    SDL_DestroySurface(converted);
    return copied;
}

static bool copy_surface_to_fb_mapped_rect(const SDL_Surface* src, const SDL_FRect* dst_rect) {
    return copy_surface_to_fb_mapped_rect_with_paths(src,
                                                     dst_rect,
                                                     FBDEV_PRESENTER_PATH_CURRENT_TARGET_EXACT,
                                                     FBDEV_PRESENTER_PATH_CURRENT_TARGET_INTEGER_SCALE,
                                                     FBDEV_PRESENTER_PATH_CURRENT_TARGET_MAPPED_SCALE);
}

static bool present_readback_rect(SDL_Renderer* renderer, const SDL_FRect* content_rect) {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (!get_content_rect_bounds(content_rect, &x0, &y0, &x1, &y1)) {
        return false;
    }

    const SDL_Rect read_rect = { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
    const Uint64 readback_start_ns = frame_stats_now();
    SDL_Surface* src = SDL_RenderReadPixels(renderer, &read_rect);
    frame_stats_add_duration(&frame_stats.readback_ns, readback_start_ns);
    if (src == NULL) {
        return false;
    }
    frame_stats_note_readback_surface(src);
    frame_stats_note_path(FBDEV_PRESENTER_PATH_READBACK_RECT);

    if (!rect_bar_clear_valid || (rect_bar_clear_x0 != x0) || (rect_bar_clear_y0 != y0) || (rect_bar_clear_x1 != x1) ||
        (rect_bar_clear_y1 != y1)) {
        const Uint64 clear_start_ns = frame_stats_now();
        clear_fb_outside_rect(x0, y0, x1, y1);
        frame_stats_add_duration(&frame_stats.clear_ns, clear_start_ns);
        mark_rect_bar_clear_cache(x0, y0, x1, y1);
    }

    const bool copied = copy_surface_to_fb_offset(src, x0, y0);
    SDL_DestroySurface(src);
    if (copied) {
        previous_pixels_valid = false;
    }
    return copied;
}

bool FBDevPresenter_PresentCurrentTarget(SDL_Renderer* renderer, const SDL_FRect* dst_rect) {
    if (!fbdev_active || (renderer == NULL) || (dst_rect == NULL)) {
        return false;
    }

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (!get_content_rect_bounds(dst_rect, &x0, &y0, &x1, &y1)) {
        return false;
    }

    const Uint64 readback_start_ns = frame_stats_now();
    SDL_Surface* src = SDL_RenderReadPixels(renderer, NULL);
    frame_stats_add_duration(&frame_stats.readback_ns, readback_start_ns);
    if (src == NULL) {
        return false;
    }
    frame_stats_note_readback_surface(src);

    if (!rect_bar_clear_valid || (rect_bar_clear_x0 != x0) || (rect_bar_clear_y0 != y0) || (rect_bar_clear_x1 != x1) ||
        (rect_bar_clear_y1 != y1)) {
        const Uint64 clear_start_ns = frame_stats_now();
        clear_fb_outside_rect(x0, y0, x1, y1);
        frame_stats_add_duration(&frame_stats.clear_ns, clear_start_ns);
        mark_rect_bar_clear_cache(x0, y0, x1, y1);
    }

    const bool copied = copy_surface_to_fb_mapped_rect(src, dst_rect);
    SDL_DestroySurface(src);
    if (copied) {
        previous_pixels_valid = false;
        maybe_draw_fps_overlay(dst_rect);
    }
    return copied;
}

bool FBDevPresenter_PresentSurface(const SDL_Surface* surface, const SDL_FRect* dst_rect) {
    if (!fbdev_active || (surface == NULL) || (dst_rect == NULL)) {
        return false;
    }

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (!get_content_rect_bounds(dst_rect, &x0, &y0, &x1, &y1)) {
        return false;
    }

    if (!rect_bar_clear_valid || (rect_bar_clear_x0 != x0) || (rect_bar_clear_y0 != y0) || (rect_bar_clear_x1 != x1) ||
        (rect_bar_clear_y1 != y1)) {
        const Uint64 clear_start_ns = frame_stats_now();
        clear_fb_outside_rect(x0, y0, x1, y1);
        frame_stats_add_duration(&frame_stats.clear_ns, clear_start_ns);
        mark_rect_bar_clear_cache(x0, y0, x1, y1);
    }

    const bool copied = copy_surface_to_fb_mapped_rect_with_paths(surface,
                                                                  dst_rect,
                                                                  FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_EXACT,
                                                                  FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_INTEGER_SCALE,
                                                                  FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_MAPPED_SCALE);
    if (copied) {
        previous_pixels_valid = false;
        maybe_draw_fps_overlay(dst_rect);
    }
    return copied;
}

static bool convert_surface_to_staging(const SDL_Surface* src) {
    const int staging_pitch = staging_width * (int)sizeof(Uint32);
    if ((src->format == SDL_PIXELFORMAT_ARGB8888) && (src->pitch == staging_pitch)) {
        const Uint64 copy_start_ns = frame_stats_now();
        SDL_memcpy(staging_pixels, src->pixels, (size_t)staging_pitch * (size_t)staging_height);
        frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
        return true;
    }

    Uint64 convert_start_ns = frame_stats_now();
    if (SDL_ConvertPixels(src->w,
                          src->h,
                          src->format,
                          src->pixels,
                          src->pitch,
                          SDL_PIXELFORMAT_ARGB8888,
                          staging_pixels,
                          staging_pitch)) {
        frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
        return true;
    }
    frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);

    convert_start_ns = frame_stats_now();
    SDL_Surface* argb = SDL_ConvertSurface(src, SDL_PIXELFORMAT_ARGB8888);
    frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
    if (argb == NULL) {
        return false;
    }

    if (argb->pitch == staging_pitch) {
        const Uint64 copy_start_ns = frame_stats_now();
        SDL_memcpy(staging_pixels, argb->pixels, (size_t)staging_pitch * (size_t)staging_height);
        frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
    } else {
        const Uint64 copy_start_ns = frame_stats_now();
        for (int y = 0; y < staging_height; y++) {
            const Uint8* src_row = ((const Uint8*)argb->pixels) + ((size_t)argb->pitch * (size_t)y);
            Uint32* dst_row = staging_pixels + ((size_t)staging_width * (size_t)y);
            SDL_memcpy(dst_row, src_row, (size_t)staging_width * sizeof(Uint32));
        }
        frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
    }

    SDL_DestroySurface(argb);
    return true;
}

static bool staging_tile_changed(int x, int y, int w, int h) {
    const size_t row_bytes = (size_t)w * sizeof(Uint32);
    for (int row = 0; row < h; row++) {
        const Uint32* src_row = staging_pixels + ((size_t)staging_width * (size_t)(y + row)) + (size_t)x;
        const Uint32* prev_row = previous_pixels + ((size_t)staging_width * (size_t)(y + row)) + (size_t)x;
        if (SDL_memcmp(src_row, prev_row, row_bytes) != 0) {
            return true;
        }
    }

    return false;
}

static void copy_staging_tile_to_fb(int x, int y, int w, int h) {
    const Uint64 copy_start_ns = frame_stats_now();
    const size_t row_bytes = (size_t)w * sizeof(Uint32);
    for (int row = 0; row < h; row++) {
        const Uint32* src_row = staging_pixels + ((size_t)staging_width * (size_t)(y + row)) + (size_t)x;
        Uint8* dst_fb_row = fb_map + ((size_t)fb_stride * (size_t)(y + row)) + ((size_t)x * sizeof(Uint32));
        Uint32* dst_prev_row = previous_pixels + ((size_t)staging_width * (size_t)(y + row)) + (size_t)x;
        SDL_memcpy(dst_fb_row, src_row, row_bytes);
        SDL_memcpy(dst_prev_row, src_row, row_bytes);
    }

    frame_copy_bytes += row_bytes * (size_t)h;
    frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
}

static void copy_full_staging_to_fb(void) {
    const Uint64 copy_start_ns = frame_stats_now();
    const size_t row_bytes = (size_t)staging_width * sizeof(Uint32);
    for (int y = 0; y < staging_height; y++) {
        const Uint32* src_row = staging_pixels + ((size_t)staging_width * (size_t)y);
        Uint8* dst_fb_row = fb_map + ((size_t)fb_stride * (size_t)y);
        Uint32* dst_prev_row = previous_pixels + ((size_t)staging_width * (size_t)y);
        SDL_memcpy(dst_fb_row, src_row, row_bytes);
        SDL_memcpy(dst_prev_row, src_row, row_bytes);
    }

    frame_copy_bytes += row_bytes * (size_t)staging_height;
    frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
}

static void present_staging_tiles(void) {
    const int tiles_x = (staging_width + (staging_tile_size - 1)) / staging_tile_size;
    const int tiles_y = (staging_height + (staging_tile_size - 1)) / staging_tile_size;
    frame_tiles_total = tiles_x * tiles_y;

    const int renderer_dirty_tiles = SDLGameRenderer_GetDirtyTileCount();
    const int renderer_total_tiles = SDLGameRenderer_GetDirtyTileTotal();
    const bool guardrail_force_full_copy = previous_pixels_valid && (renderer_total_tiles > 0) &&
                                           (renderer_dirty_tiles * 100 >= renderer_total_tiles * staging_full_copy_guardrail_percent);

    if (!previous_pixels_valid || guardrail_force_full_copy) {
        copy_full_staging_to_fb();
        frame_tiles_copied = frame_tiles_total;
        frame_full_copy_fallback = guardrail_force_full_copy;
        previous_pixels_valid = true;
        return;
    }

    int dirty_cols = 0;
    int dirty_rows = 0;
    int dirty_tile_size_px = 0;
    const Uint8* dirty_map = SDLGameRenderer_GetDirtyTileMap(&dirty_cols, &dirty_rows, &dirty_tile_size_px);
    const bool dirty_map_compatible = (dirty_map != NULL) && (dirty_cols > 0) && (dirty_rows > 0) && (dirty_tile_size_px > 0) &&
                                      ((dirty_cols * dirty_tile_size_px) == staging_width) &&
                                      ((dirty_rows * dirty_tile_size_px) == staging_height);
    if (dirty_map_compatible) {
        frame_tiles_total = dirty_cols * dirty_rows;
        for (int ty = 0; ty < dirty_rows; ty++) {
            const int y = ty * dirty_tile_size_px;
            const int h = SDL_min(dirty_tile_size_px, staging_height - y);
            for (int tx = 0; tx < dirty_cols; tx++) {
                const int tile_index = (ty * dirty_cols) + tx;
                if (!dirty_map[tile_index]) {
                    continue;
                }
                const int x = tx * dirty_tile_size_px;
                const int w = SDL_min(dirty_tile_size_px, staging_width - x);
                if (staging_tile_changed(x, y, w, h)) {
                    copy_staging_tile_to_fb(x, y, w, h);
                    frame_tiles_copied += 1;
                }
            }
        }
        previous_pixels_valid = true;
        return;
    }

    for (int y = 0; y < staging_height; y += staging_tile_size) {
        const int h = SDL_min(staging_tile_size, staging_height - y);
        for (int x = 0; x < staging_width; x += staging_tile_size) {
            const int w = SDL_min(staging_tile_size, staging_width - x);
            if (staging_tile_changed(x, y, w, h)) {
                copy_staging_tile_to_fb(x, y, w, h);
                frame_tiles_copied += 1;
            }
        }
    }

    previous_pixels_valid = true;
}

bool FBDevPresenter_Init(void) {
    struct fb_var_screeninfo var = { 0 };
    struct fb_fix_screeninfo fix = { 0 };

    if (fbdev_active) {
        return true;
    }

#if defined(O_CLOEXEC)
    fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
#else
    fb_fd = open("/dev/fb0", O_RDWR);
#endif

    if (fb_fd < 0) {
        SDL_Log("FBDEV: failed to open /dev/fb0");
        return false;
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        SDL_Log("FBDEV: failed to query framebuffer info");
        close_fb();
        return false;
    }

    if (var.bits_per_pixel != 32) {
        SDL_Log("FBDEV: unsupported bits_per_pixel=%u", var.bits_per_pixel);
        close_fb();
        return false;
    }

    fb_width = (int)var.xres;
    fb_height = (int)var.yres;
    fb_stride = (int)fix.line_length;
    fb_map_len = (size_t)fix.smem_len;
    fb_map = mmap(NULL, fb_map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);

    if (fb_map == MAP_FAILED) {
        fb_map = NULL;
        SDL_Log("FBDEV: mmap failed");
        close_fb();
        return false;
    }

    fbdev_active = true;
    SDL_Log("FBDEV: active (%dx%d stride=%d bpp=%u)", fb_width, fb_height, fb_stride, var.bits_per_pixel);
    return true;
}

bool FBDevPresenter_IsActive(void) {
    return fbdev_active;
}

int FBDevPresenter_GetWidth(void) {
    if (!fbdev_active) {
        return 0;
    }

    return fb_width;
}

int FBDevPresenter_GetHeight(void) {
    if (!fbdev_active) {
        return 0;
    }

    return fb_height;
}

void FBDevPresenter_Present(SDL_Renderer* renderer, const SDL_FRect* content_rect) {
    if (!fbdev_active || renderer == NULL) {
        return;
    }

    int fallback_x0 = 0;
    int fallback_y0 = 0;
    int fallback_x1 = 0;
    int fallback_y1 = 0;
    const bool fallback_rect_valid =
        get_content_rect_bounds(content_rect, &fallback_x0, &fallback_y0, &fallback_x1, &fallback_y1);
    const bool fallback_rect_is_full =
        fallback_rect_valid && (fallback_x0 == 0) && (fallback_y0 == 0) && (fallback_x1 == fb_width) &&
        (fallback_y1 == fb_height);

    // Keep the targeted readback fast path for letterboxed content even when staging is available.
    if (fallback_rect_valid && !fallback_rect_is_full && present_readback_rect(renderer, content_rect)) {
        maybe_draw_fps_overlay(content_rect);
        return;
    }

    invalidate_rect_bar_clear_cache();

    const Uint64 readback_start_ns = frame_stats_now();
    SDL_Surface* src = SDL_RenderReadPixels(renderer, NULL);
    frame_stats_add_duration(&frame_stats.readback_ns, readback_start_ns);

    if (src == NULL) {
        return;
    }
    frame_stats_note_readback_surface(src);

    if (src->w == fb_width && src->h == fb_height) {
        const bool staging_ready = ensure_staging_buffers(fb_width, fb_height);
        if (staging_ready && convert_surface_to_staging(src)) {
            frame_stats_note_path(FBDEV_PRESENTER_PATH_FULLSCREEN_STAGING);
            present_staging_tiles();
            SDL_DestroySurface(src);
            if (fallback_rect_valid) {
                const Uint64 clear_start_ns = frame_stats_now();
                clear_fb_outside_rect(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
                frame_stats_add_duration(&frame_stats.clear_ns, clear_start_ns);
                sync_previous_outside_rect_clear(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
                mark_rect_bar_clear_cache(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
            }
            maybe_draw_fps_overlay(content_rect);
            return;
        }

        if (fallback_rect_valid) {
            const int fallback_w = fallback_x1 - fallback_x0;
            const int fallback_h = fallback_y1 - fallback_y0;
            frame_stats_note_path(FBDEV_PRESENTER_PATH_FULLSCREEN_DIRECT_COPY);
            if (copy_surface_rect_to_fb_offset(
                    src, fallback_x0, fallback_y0, fallback_w, fallback_h, fallback_x0, fallback_y0)) {
                SDL_DestroySurface(src);
                const Uint64 clear_start_ns = frame_stats_now();
                clear_fb_outside_rect(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
                frame_stats_add_duration(&frame_stats.clear_ns, clear_start_ns);
                mark_rect_bar_clear_cache(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
                previous_pixels_valid = false;
                maybe_draw_fps_overlay(content_rect);
                return;
            }
        }

        if (src->format == SDL_PIXELFORMAT_ARGB8888) {
            frame_stats_note_path(FBDEV_PRESENTER_PATH_FULLSCREEN_DIRECT_COPY);
            const Uint64 copy_start_ns = frame_stats_now();
            copy_argb_surface_to_fb(src);
            frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
            SDL_DestroySurface(src);
            if (fallback_rect_valid) {
                const Uint64 clear_start_ns = frame_stats_now();
                clear_fb_outside_rect(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
                frame_stats_add_duration(&frame_stats.clear_ns, clear_start_ns);
                mark_rect_bar_clear_cache(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
            }
            previous_pixels_valid = false;
            maybe_draw_fps_overlay(content_rect);
            return;
        }

        frame_stats_note_path(FBDEV_PRESENTER_PATH_FULLSCREEN_DIRECT_COPY);
        Uint64 convert_start_ns = frame_stats_now();
        if (SDL_ConvertPixels(src->w,
                              src->h,
                              src->format,
                              src->pixels,
                              src->pitch,
                              SDL_PIXELFORMAT_ARGB8888,
                              fb_map,
                              fb_stride)) {
            frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
            const size_t converted_bytes = ((size_t)src->w * sizeof(Uint32)) * (size_t)src->h;
            SDL_DestroySurface(src);
            frame_copy_bytes += converted_bytes;
            if (fallback_rect_valid) {
                const Uint64 clear_start_ns = frame_stats_now();
                clear_fb_outside_rect(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
                frame_stats_add_duration(&frame_stats.clear_ns, clear_start_ns);
                mark_rect_bar_clear_cache(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
            }
            previous_pixels_valid = false;
            maybe_draw_fps_overlay(content_rect);
            return;
        }
        frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);

        convert_start_ns = frame_stats_now();
        SDL_Surface* argb = SDL_ConvertSurface(src, SDL_PIXELFORMAT_ARGB8888);
        frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
        SDL_DestroySurface(src);
        if (argb == NULL) {
            return;
        }
        const Uint64 copy_start_ns = frame_stats_now();
        copy_argb_surface_to_fb(argb);
        frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);
        SDL_DestroySurface(argb);
        if (fallback_rect_valid) {
            const Uint64 clear_start_ns = frame_stats_now();
            clear_fb_outside_rect(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
            frame_stats_add_duration(&frame_stats.clear_ns, clear_start_ns);
            mark_rect_bar_clear_cache(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
        }
        previous_pixels_valid = false;
        maybe_draw_fps_overlay(content_rect);
        return;
    }

    SDL_Surface* argb = src;
    if (src->format != SDL_PIXELFORMAT_ARGB8888) {
        const Uint64 convert_start_ns = frame_stats_now();
        argb = SDL_ConvertSurface(src, SDL_PIXELFORMAT_ARGB8888);
        frame_stats_add_duration(&frame_stats.convert_ns, convert_start_ns);
        SDL_DestroySurface(src);

        if (argb == NULL) {
            return;
        }
    }

    const size_t row_bytes = (size_t)fb_width * sizeof(Uint32);

    const Uint64 copy_start_ns = frame_stats_now();
    if (ensure_scale_lut(argb->w, argb->h, 0, 0, fb_width, fb_height)) {
        frame_stats_note_path(FBDEV_PRESENTER_PATH_FULLSCREEN_SCALED_LUT);
        int prev_src_y = -1;
        Uint8* prev_dst_row = NULL;

        for (int y = 0; y < fb_height; y++) {
            const int src_y = scale_y_lut[y];
            Uint8* dst_row_bytes = fb_map + (fb_stride * y);

            if ((src_y == prev_src_y) && (prev_dst_row != NULL)) {
                SDL_memcpy(dst_row_bytes, prev_dst_row, row_bytes);
                frame_copy_bytes += row_bytes;
                continue;
            }

            const Uint32* src_row = (const Uint32*)(((const Uint8*)argb->pixels) + (argb->pitch * src_y));
            Uint32* dst_row = (Uint32*)dst_row_bytes;

            for (int x = 0; x < fb_width; x++) {
                dst_row[x] = src_row[scale_x_lut[x]];
            }

            frame_copy_bytes += row_bytes;

            prev_src_y = src_y;
            prev_dst_row = dst_row_bytes;
        }
    } else {
        frame_stats_note_path(FBDEV_PRESENTER_PATH_FULLSCREEN_SCALED_DIV);
        int prev_src_y = -1;
        Uint8* prev_dst_row = NULL;

        for (int y = 0; y < fb_height; y++) {
            const int src_y = (y * argb->h) / fb_height;
            const Uint32* src_row = (const Uint32*)(((const Uint8*)argb->pixels) + (argb->pitch * src_y));
            Uint8* dst_row_bytes = fb_map + (fb_stride * y);

            if ((src_y == prev_src_y) && (prev_dst_row != NULL)) {
                SDL_memcpy(dst_row_bytes, prev_dst_row, row_bytes);
                frame_copy_bytes += row_bytes;
                continue;
            }

            Uint32* dst_row = (Uint32*)dst_row_bytes;

            for (int x = 0; x < fb_width; x++) {
                const int src_x = (x * argb->w) / fb_width;
                dst_row[x] = src_row[src_x];
            }

            frame_copy_bytes += row_bytes;

            prev_src_y = src_y;
            prev_dst_row = dst_row_bytes;
        }
    }
    frame_stats_add_duration(&frame_stats.copy_ns, copy_start_ns);

    SDL_DestroySurface(argb);
    if (fallback_rect_valid) {
        const Uint64 clear_start_ns = frame_stats_now();
        clear_fb_outside_rect(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
        frame_stats_add_duration(&frame_stats.clear_ns, clear_start_ns);
        mark_rect_bar_clear_cache(fallback_x0, fallback_y0, fallback_x1, fallback_y1);
    }
    previous_pixels_valid = false;
    maybe_draw_fps_overlay(content_rect);
}

void FBDevPresenter_SetFPSOverlayEnabled(bool enabled) {
    fps_overlay_enabled = enabled;
    if (!enabled) {
        fps_overlay_text[0] = '\0';
    }
}

void FBDevPresenter_SetFPSOverlayText(const char* text) {
    if (text == NULL) {
        fps_overlay_text[0] = '\0';
        return;
    }

    SDL_snprintf(fps_overlay_text, sizeof(fps_overlay_text), "%s", text);
}

void FBDevPresenter_BeginFrameStats(bool capture_breakdown) {
    frame_copy_bytes = 0;
    frame_tiles_total = 0;
    frame_tiles_copied = 0;
    frame_full_copy_fallback = false;
#if ENABLE_PERF_TELEMETRY
    SDL_memset(&frame_stats, 0, sizeof(frame_stats));
    frame_stats_breakdown_enabled = capture_breakdown;
#else
    (void)capture_breakdown;
#endif
}

size_t FBDevPresenter_GetFrameCopyBytes(void) {
    return frame_copy_bytes;
}

void FBDevPresenter_GetFrameStats(FBDevPresenter_FrameStats* out_stats) {
    if (out_stats == NULL) {
        return;
    }

    *out_stats = frame_stats;
}

int FBDevPresenter_GetFrameTilesTotal(void) {
    return frame_tiles_total;
}

int FBDevPresenter_GetFrameTilesCopied(void) {
    return frame_tiles_copied;
}

bool FBDevPresenter_UsedFullCopyFallback(void) {
    return frame_full_copy_fallback;
}

const char* FBDevPresenter_PathName(FBDevPresenterPath path) {
    switch (path) {
    case FBDEV_PRESENTER_PATH_NONE:
        return "none";
    case FBDEV_PRESENTER_PATH_READBACK_RECT:
        return "readback_rect";
    case FBDEV_PRESENTER_PATH_CURRENT_TARGET_EXACT:
        return "current_target_exact";
    case FBDEV_PRESENTER_PATH_CURRENT_TARGET_INTEGER_SCALE:
        return "current_target_integer_scale";
    case FBDEV_PRESENTER_PATH_CURRENT_TARGET_MAPPED_SCALE:
        return "current_target_mapped_scale";
    case FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_EXACT:
        return "software_frame_exact";
    case FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_INTEGER_SCALE:
        return "software_frame_integer_scale";
    case FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_MAPPED_SCALE:
        return "software_frame_mapped_scale";
    case FBDEV_PRESENTER_PATH_FULLSCREEN_STAGING:
        return "fullscreen_staging";
    case FBDEV_PRESENTER_PATH_FULLSCREEN_DIRECT_COPY:
        return "fullscreen_direct_copy";
    case FBDEV_PRESENTER_PATH_FULLSCREEN_SCALED_LUT:
        return "fullscreen_scaled_lut";
    case FBDEV_PRESENTER_PATH_FULLSCREEN_SCALED_DIV:
        return "fullscreen_scaled_div";
    case FBDEV_PRESENTER_PATH_COUNT:
        break;
    }

    return "unknown";
}

void FBDevPresenter_Quit(void) {
    fbdev_active = false;
    close_fb();
}

#else

bool FBDevPresenter_Init(void) {
    return false;
}

bool FBDevPresenter_IsActive(void) {
    return false;
}

int FBDevPresenter_GetWidth(void) {
    return 0;
}

int FBDevPresenter_GetHeight(void) {
    return 0;
}

void FBDevPresenter_Present(SDL_Renderer* renderer, const SDL_FRect* content_rect) {
    (void)renderer;
    (void)content_rect;
}

bool FBDevPresenter_PresentCurrentTarget(SDL_Renderer* renderer, const SDL_FRect* dst_rect) {
    (void)renderer;
    (void)dst_rect;
    return false;
}

bool FBDevPresenter_PresentSurface(const SDL_Surface* surface, const SDL_FRect* dst_rect) {
    (void)surface;
    (void)dst_rect;
    return false;
}

void FBDevPresenter_BeginFrameStats(bool capture_breakdown) {
    (void)capture_breakdown;
}

size_t FBDevPresenter_GetFrameCopyBytes(void) {
    return 0;
}

void FBDevPresenter_GetFrameStats(FBDevPresenter_FrameStats* out_stats) {
    if (out_stats != NULL) {
        SDL_memset(out_stats, 0, sizeof(*out_stats));
    }
}

int FBDevPresenter_GetFrameTilesTotal(void) {
    return 0;
}

int FBDevPresenter_GetFrameTilesCopied(void) {
    return 0;
}

bool FBDevPresenter_UsedFullCopyFallback(void) {
    return false;
}

void FBDevPresenter_SetFPSOverlayEnabled(bool enabled) {
    (void)enabled;
}

void FBDevPresenter_SetFPSOverlayText(const char* text) {
    (void)text;
}

const char* FBDevPresenter_PathName(FBDevPresenterPath path) {
    (void)path;
    return "none";
}

void FBDevPresenter_Quit(void) {
}

#endif
