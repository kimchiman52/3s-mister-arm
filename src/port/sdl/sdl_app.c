#include "port/sdl/sdl_app.h"
#include "common.h"
#include "port/config/config.h"
#include "port/config/config_helpers.h"
#include "port/config/keymap.h"
#include "port/paths.h"
#include "port/sdl/netplay_screen.h"
#include "port/sdl/netstats_renderer.h"
#include "port/sdl/sdl_debug_text.h"
#include "port/sdl/fbdev_presenter.h"
#include "port/sdl/sdl_game_renderer.h"
#include "port/sdl/sdl_message_renderer.h"
#include "port/sdl/sdl_pad.h"
#include "port/sound/adx.h"
#include "sf33rd/AcrSDK/ps2/foundaps2.h"

#include <SDL3/SDL.h>

#include <stdarg.h>

typedef enum ScaleMode {
    SCALEMODE_NATIVE,
    SCALEMODE_NEAREST,
    SCALEMODE_LINEAR,
    SCALEMODE_SOFT_LINEAR,
    SCALEMODE_SQUARE_PIXELS,
    SCALEMODE_INTEGER,
} ScaleMode;

static const char* app_name = "Street Fighter III: 3rd Strike";
static const float display_target_ratio = 4.0 / 3.0;
static const int native_game_width = 384;
static const int native_game_height = 224;
#if defined(PORT_MISTER)
static const int window_min_width = 320;
static const int window_min_height = 240;
#else
static const int window_min_width = 384;
static const int window_min_height = (int)(window_min_width / display_target_ratio);
#endif
static const Uint64 target_frame_time_ns = 1000000000.0 / TARGET_FPS;

SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Texture* screen_texture = NULL;
static ScaleMode scale_mode = SCALEMODE_SOFT_LINEAR;
static SDL_FRect native_output_rect = { 0 };
static bool native_output_rect_dirty = true;
static bool native_output_has_bars = true;
static int native_output_width = 0;
static int native_output_height = 0;
static SDL_FRect screen_output_rect = { 0 };
static bool screen_output_rect_dirty = true;
static bool screen_output_has_letterbox = true;
static int screen_output_width = 0;
static int screen_output_height = 0;

static Uint64 frame_deadline = 0;
#if defined(DEBUG)
#define FRAME_END_TIMES_MAX 30
static Uint64 frame_end_times[FRAME_END_TIMES_MAX];
static int frame_end_times_index = 0;
static bool frame_end_times_filled = false;
static double fps = 0;
#endif
static Uint64 frame_counter = 0;

static bool should_save_screenshot = false;
static Uint64 last_mouse_motion_time = 0;
static const int mouse_hide_delay_ms = 2000; // 2 seconds
static bool fbdev_presenter_enabled = false;
static bool use_fbdev_only_present = false;
static bool use_native_render_path = false;
static bool perf_capture_enabled = false;
static bool perf_capture_completed = false;
static int perf_capture_target_frames = 0;
static int perf_capture_recorded_frames = 0;
static char* perf_capture_output_path = NULL;
static char* perf_capture_scene_name = NULL;
static Uint64 perf_frame_start_ns = 0;
static Uint64 perf_update_start_ns = 0;
static Uint64 perf_update_ns_total = 0;
static Uint64 perf_render_ns_total = 0;
static Uint64 perf_present_ns_total = 0;
static Uint64 perf_frame_work_ns_total = 0;
static Uint64 perf_present_readback_ns_total = 0;
static Uint64 perf_present_convert_ns_total = 0;
static Uint64 perf_present_copy_ns_total = 0;
static Uint64 perf_present_clear_ns_total = 0;
static size_t perf_copy_bytes_total = 0;
static Uint64 perf_dirty_tiles_total = 0;
static double perf_dirty_hit_rate_total = 0.0;
static int perf_full_copy_fallback_frames = 0;
static Uint64 perf_render_task_count_total = 0;
static Uint64 perf_rect_copy_tasks_total = 0;
static Uint64 perf_batch_runs_total = 0;
static Uint64 perf_batched_task_count_total = 0;
static Uint64 perf_rect_texture_runs_total = 0;
static Uint64 perf_rect_texture_multi_runs_total = 0;
static Uint64 perf_rect_texture_multi_run_tasks_total = 0;
static Uint64 perf_rect_texture_max_run_total = 0;
static Uint64 perf_rect_texture_hstrip_runs_total = 0;
static Uint64 perf_rect_texture_hstrip_tasks_total = 0;
static Uint64 perf_rect_texture_vstrip_runs_total = 0;
static Uint64 perf_rect_texture_vstrip_tasks_total = 0;
static Uint64 perf_rect_texture_run_links_total = 0;
static Uint64 perf_rect_texture_color_breaks_total = 0;
static Uint64 perf_rect_texture_flip_breaks_total = 0;
static Uint64 perf_rect_texture_flipped_tasks_total = 0;
static Uint64 perf_textured_geometry_tasks_total = 0;
static Uint64 perf_textured_geometry_rect_recovered_tasks_total = 0;
static Uint64 perf_textured_geometry_fallback_tasks_total = 0;
static Uint64 perf_set_texture_calls_total = 0;
static Uint64 perf_texture_binding_reuse_hits_total = 0;
static Uint64 perf_texture_cache_hits_total = 0;
static Uint64 perf_texture_cache_misses_total = 0;
static Uint64 perf_texture_creates_total = 0;
static Uint64 perf_texture_unlock_calls_total = 0;
static Uint64 perf_palette_unlock_calls_total = 0;
static Uint64 perf_texture_cache_evictions_total = 0;
static Uint64 perf_palette_cache_evictions_total = 0;
static Uint64 perf_textures_destroy_queued_total = 0;
static Uint64 perf_unknown_tasks_total = 0;
static Uint64 perf_ppg_tasks_total = 0;
static Uint64 perf_mtrans_tasks_total = 0;
static Uint64 perf_ui_direct_tasks_total = 0;
static Uint64 perf_solid_tasks_total = 0;
static Uint64 perf_hybrid_candidate_tasks_total = 0;
static Uint64 perf_hybrid_candidate_pixels_total = 0;
static Uint64 perf_hybrid_fallback_tasks_total = 0;
static Uint64 perf_hybrid_fallback_pixels_total = 0;
static Uint64 perf_hybrid_reason_clip_total = 0;
static Uint64 perf_hybrid_reason_alpha_total = 0;
static Uint64 perf_hybrid_reason_color_mod_total = 0;
static Uint64 perf_hybrid_reason_flip_total = 0;
static Uint64 perf_hybrid_reason_geometry_total = 0;
static Uint64 perf_hybrid_reason_solid_total = 0;
static Uint64 perf_sort_strategy_frames[SDL_GAME_RENDERER_SORT_QSORT + 1] = { 0 };
static Uint64 perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_COUNT] = { 0 };

typedef struct PerfFrameSample {
    double frame_time_ms;
    double update_ms;
    double render_ms;
    double present_ms;
    double present_readback_ms;
    double present_convert_ms;
    double present_copy_ms;
    double present_clear_ms;
    size_t copy_bytes;
    int dirty_tiles;
    double dirty_ratio;
    int presenter_tiles_total;
    int presenter_tiles_copied;
    double dirty_hit_rate;
    bool full_copy_fallback;
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
    FBDevPresenterPath fbdev_path;
    Uint32 readback_format;
    int readback_width;
    int readback_height;
} PerfFrameSample;

static PerfFrameSample* perf_samples = NULL;

#if defined(PORT_MISTER)
static const char* legacy_mister_video_driver_order = "kmsdrm,offscreen,dummy";
static const char* recommended_mister_video_driver_order = "evdev,dummy,offscreen";
#endif

static void perf_capture_reset_storage(void);
static void perf_capture_write_summary(void);

static const char* render_sort_strategy_name(SDLGameRenderer_SortStrategy strategy) {
    switch (strategy) {
    case SDL_GAME_RENDERER_SORT_NONE:
        return "none";
    case SDL_GAME_RENDERER_SORT_EQUAL_Z_REVERSE:
        return "equal_z_reverse";
    case SDL_GAME_RENDERER_SORT_INSERTION:
        return "insertion";
    case SDL_GAME_RENDERER_SORT_QSORT:
        return "qsort";
    }

    return "unknown";
}

static const char* fbdev_present_path_name(FBDevPresenterPath path) {
    return FBDevPresenter_PathName(path);
}

static const char* pixel_format_name_safe(Uint32 format) {
    if (format == SDL_PIXELFORMAT_UNKNOWN) {
        return "SDL_PIXELFORMAT_UNKNOWN";
    }

    const char* name = SDL_GetPixelFormatName((SDL_PixelFormat)format);
    if ((name == NULL) || (name[0] == '\0')) {
        return "SDL_PIXELFORMAT_UNKNOWN";
    }

    return name;
}

static FBDevPresenterPath dominant_fbdev_present_path(void) {
    Uint64 best_count = 0;
    FBDevPresenterPath best_path = FBDEV_PRESENTER_PATH_NONE;

    for (int path = FBDEV_PRESENTER_PATH_NONE; path < FBDEV_PRESENTER_PATH_COUNT; path++) {
        if (perf_fbdev_path_frames[path] > best_count) {
            best_count = perf_fbdev_path_frames[path];
            best_path = (FBDevPresenterPath)path;
        }
    }

    return best_path;
}

static void append_backend_log_line(const char* line) {
    const char* pref_path = Paths_GetPrefPath();
    char* logs_dir = NULL;
    char* log_path = NULL;
    SDL_asprintf(&logs_dir, "%slogs", pref_path);
    SDL_CreateDirectory(logs_dir);
    SDL_asprintf(&log_path, "%s/backend.log", logs_dir);

    SDL_IOStream* io = SDL_IOFromFile(log_path, "a");

    if (io != NULL) {
        SDL_WriteIO(io, line, SDL_strlen(line));
        SDL_WriteIO(io, "\n", 1);
        SDL_CloseIO(io);
    }

    SDL_free(logs_dir);
    SDL_free(log_path);
}

static void backend_logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char* buf = NULL;
    SDL_vasprintf(&buf, fmt, args);
    va_end(args);

    if (buf == NULL) {
        return;
    }

    SDL_Log("%s", buf);
    append_backend_log_line(buf);
    SDL_free(buf);
}

static void perf_capture_reset_storage(void) {
    if (perf_samples != NULL) {
        SDL_free(perf_samples);
        perf_samples = NULL;
    }

    if (perf_capture_output_path != NULL) {
        SDL_free(perf_capture_output_path);
        perf_capture_output_path = NULL;
    }

    if (perf_capture_scene_name != NULL) {
        SDL_free(perf_capture_scene_name);
        perf_capture_scene_name = NULL;
    }

    perf_capture_enabled = false;
    perf_capture_completed = false;
    perf_capture_target_frames = 0;
    perf_capture_recorded_frames = 0;
    perf_frame_start_ns = 0;
    perf_update_start_ns = 0;
    perf_update_ns_total = 0;
    perf_render_ns_total = 0;
    perf_present_ns_total = 0;
    perf_frame_work_ns_total = 0;
    perf_present_readback_ns_total = 0;
    perf_present_convert_ns_total = 0;
    perf_present_copy_ns_total = 0;
    perf_present_clear_ns_total = 0;
    perf_copy_bytes_total = 0;
    perf_dirty_tiles_total = 0;
    perf_dirty_hit_rate_total = 0.0;
    perf_full_copy_fallback_frames = 0;
    perf_render_task_count_total = 0;
    perf_rect_copy_tasks_total = 0;
    perf_batch_runs_total = 0;
    perf_batched_task_count_total = 0;
    perf_rect_texture_runs_total = 0;
    perf_rect_texture_multi_runs_total = 0;
    perf_rect_texture_multi_run_tasks_total = 0;
    perf_rect_texture_max_run_total = 0;
    perf_rect_texture_hstrip_runs_total = 0;
    perf_rect_texture_hstrip_tasks_total = 0;
    perf_rect_texture_vstrip_runs_total = 0;
    perf_rect_texture_vstrip_tasks_total = 0;
    perf_rect_texture_run_links_total = 0;
    perf_rect_texture_color_breaks_total = 0;
    perf_rect_texture_flip_breaks_total = 0;
    perf_rect_texture_flipped_tasks_total = 0;
    perf_textured_geometry_tasks_total = 0;
    perf_textured_geometry_rect_recovered_tasks_total = 0;
    perf_textured_geometry_fallback_tasks_total = 0;
    perf_set_texture_calls_total = 0;
    perf_texture_binding_reuse_hits_total = 0;
    perf_texture_cache_hits_total = 0;
    perf_texture_cache_misses_total = 0;
    perf_texture_creates_total = 0;
    perf_texture_unlock_calls_total = 0;
    perf_palette_unlock_calls_total = 0;
    perf_texture_cache_evictions_total = 0;
    perf_palette_cache_evictions_total = 0;
    perf_textures_destroy_queued_total = 0;
    perf_unknown_tasks_total = 0;
    perf_ppg_tasks_total = 0;
    perf_mtrans_tasks_total = 0;
    perf_ui_direct_tasks_total = 0;
    perf_solid_tasks_total = 0;
    perf_hybrid_candidate_tasks_total = 0;
    perf_hybrid_candidate_pixels_total = 0;
    perf_hybrid_fallback_tasks_total = 0;
    perf_hybrid_fallback_pixels_total = 0;
    perf_hybrid_reason_clip_total = 0;
    perf_hybrid_reason_alpha_total = 0;
    perf_hybrid_reason_color_mod_total = 0;
    perf_hybrid_reason_flip_total = 0;
    perf_hybrid_reason_geometry_total = 0;
    perf_hybrid_reason_solid_total = 0;
    SDL_memset(perf_sort_strategy_frames, 0, sizeof(perf_sort_strategy_frames));
    SDL_memset(perf_fbdev_path_frames, 0, sizeof(perf_fbdev_path_frames));
}

void SDLApp_ConfigurePerfCapture(int frame_count, const char* output_path, const char* scene_name) {
    if (frame_count <= 0) {
        perf_capture_reset_storage();
        return;
    }

    if (perf_samples != NULL) {
        SDL_free(perf_samples);
        perf_samples = NULL;
    }

    if (perf_capture_output_path != NULL) {
        SDL_free(perf_capture_output_path);
        perf_capture_output_path = NULL;
    }

    if (perf_capture_scene_name != NULL) {
        SDL_free(perf_capture_scene_name);
        perf_capture_scene_name = NULL;
    }

    perf_capture_target_frames = frame_count;
    perf_capture_recorded_frames = 0;
    perf_capture_completed = false;
    perf_capture_enabled = true;
    perf_frame_start_ns = 0;
    perf_update_start_ns = 0;
    perf_update_ns_total = 0;
    perf_render_ns_total = 0;
    perf_present_ns_total = 0;
    perf_frame_work_ns_total = 0;
    perf_present_readback_ns_total = 0;
    perf_present_convert_ns_total = 0;
    perf_present_copy_ns_total = 0;
    perf_present_clear_ns_total = 0;
    perf_copy_bytes_total = 0;
    perf_dirty_tiles_total = 0;
    perf_dirty_hit_rate_total = 0.0;
    perf_full_copy_fallback_frames = 0;
    perf_render_task_count_total = 0;
    perf_rect_copy_tasks_total = 0;
    perf_batch_runs_total = 0;
    perf_batched_task_count_total = 0;
    perf_rect_texture_runs_total = 0;
    perf_rect_texture_multi_runs_total = 0;
    perf_rect_texture_multi_run_tasks_total = 0;
    perf_rect_texture_max_run_total = 0;
    perf_rect_texture_hstrip_runs_total = 0;
    perf_rect_texture_hstrip_tasks_total = 0;
    perf_rect_texture_vstrip_runs_total = 0;
    perf_rect_texture_vstrip_tasks_total = 0;
    perf_rect_texture_run_links_total = 0;
    perf_rect_texture_color_breaks_total = 0;
    perf_rect_texture_flip_breaks_total = 0;
    perf_rect_texture_flipped_tasks_total = 0;
    perf_textured_geometry_tasks_total = 0;
    perf_textured_geometry_rect_recovered_tasks_total = 0;
    perf_textured_geometry_fallback_tasks_total = 0;
    perf_set_texture_calls_total = 0;
    perf_texture_binding_reuse_hits_total = 0;
    perf_texture_cache_hits_total = 0;
    perf_texture_cache_misses_total = 0;
    perf_texture_creates_total = 0;
    perf_texture_unlock_calls_total = 0;
    perf_palette_unlock_calls_total = 0;
    perf_texture_cache_evictions_total = 0;
    perf_palette_cache_evictions_total = 0;
    perf_textures_destroy_queued_total = 0;
    perf_unknown_tasks_total = 0;
    perf_ppg_tasks_total = 0;
    perf_mtrans_tasks_total = 0;
    perf_ui_direct_tasks_total = 0;
    perf_solid_tasks_total = 0;
    perf_hybrid_candidate_tasks_total = 0;
    perf_hybrid_candidate_pixels_total = 0;
    perf_hybrid_fallback_tasks_total = 0;
    perf_hybrid_fallback_pixels_total = 0;
    perf_hybrid_reason_clip_total = 0;
    perf_hybrid_reason_alpha_total = 0;
    perf_hybrid_reason_color_mod_total = 0;
    perf_hybrid_reason_flip_total = 0;
    perf_hybrid_reason_geometry_total = 0;
    perf_hybrid_reason_solid_total = 0;
    SDL_memset(perf_sort_strategy_frames, 0, sizeof(perf_sort_strategy_frames));
    SDL_memset(perf_fbdev_path_frames, 0, sizeof(perf_fbdev_path_frames));
    perf_capture_output_path = output_path != NULL ? SDL_strdup(output_path) : NULL;
    perf_capture_scene_name = scene_name != NULL ? SDL_strdup(scene_name) : NULL;
    perf_samples = (PerfFrameSample*)SDL_calloc((size_t)frame_count, sizeof(PerfFrameSample));

    if (perf_samples == NULL) {
        backend_logf("PERF capture disabled: failed to allocate sample buffer for %d frames.", frame_count);
        perf_capture_reset_storage();
        return;
    }

    backend_logf("PERF capture enabled: frames=%d output=%s scene=%s",
                 perf_capture_target_frames,
                 perf_capture_output_path != NULL ? perf_capture_output_path : "(auto)",
                 perf_capture_scene_name != NULL ? perf_capture_scene_name : "(none)");
}

static void io_write_json_escaped_string(SDL_IOStream* io, const char* value) {
    const char* s = value != NULL ? value : "";
    while (*s != '\0') {
        const unsigned char c = (unsigned char)(*s);
        switch (c) {
        case '\\':
            SDL_WriteIO(io, "\\\\", 2);
            break;
        case '"':
            SDL_WriteIO(io, "\\\"", 2);
            break;
        case '\n':
            SDL_WriteIO(io, "\\n", 2);
            break;
        case '\r':
            SDL_WriteIO(io, "\\r", 2);
            break;
        case '\t':
            SDL_WriteIO(io, "\\t", 2);
            break;
        default:
            if (c < 0x20) {
                io_printf(io, "\\u%04x", (unsigned int)c);
            } else {
                SDL_WriteIO(io, s, 1);
            }
            break;
        }
        s += 1;
    }
}

static void perf_capture_write_summary(void) {
    if (!perf_capture_enabled || (perf_capture_recorded_frames <= 0)) {
        return;
    }

    char* output_path = NULL;
    if (perf_capture_output_path != NULL && perf_capture_output_path[0] != '\0') {
        output_path = SDL_strdup(perf_capture_output_path);
    } else {
        const char* pref_path = Paths_GetPrefPath();
        char* logs_dir = NULL;
        SDL_asprintf(&logs_dir, "%slogs", pref_path);
        SDL_CreateDirectory(logs_dir);
        SDL_asprintf(&output_path, "%s/perf-capture.json", logs_dir);
        SDL_free(logs_dir);
    }

    if (output_path == NULL) {
        backend_logf("PERF capture: failed to resolve output path.");
        return;
    }

    SDL_IOStream* io = SDL_IOFromFile(output_path, "w");
    if (io == NULL) {
        backend_logf("PERF capture: failed to open output file '%s': %s", output_path, SDL_GetError());
        SDL_free(output_path);
        return;
    }

    const double frame_count = (double)perf_capture_recorded_frames;
    const double avg_frame_ms = ((double)perf_frame_work_ns_total / frame_count) / 1e6;
    const double avg_update_ms = ((double)perf_update_ns_total / frame_count) / 1e6;
    const double avg_render_ms = ((double)perf_render_ns_total / frame_count) / 1e6;
    const double avg_present_ms = ((double)perf_present_ns_total / frame_count) / 1e6;
    const double avg_present_readback_ms = ((double)perf_present_readback_ns_total / frame_count) / 1e6;
    const double avg_present_convert_ms = ((double)perf_present_convert_ns_total / frame_count) / 1e6;
    const double avg_present_copy_ms = ((double)perf_present_copy_ns_total / frame_count) / 1e6;
    const double avg_present_clear_ms = ((double)perf_present_clear_ns_total / frame_count) / 1e6;
    const double avg_copy_bytes = (double)perf_copy_bytes_total / frame_count;
    const double avg_dirty_tiles = (double)perf_dirty_tiles_total / frame_count;
    const double avg_render_task_count = (double)perf_render_task_count_total / frame_count;
    const double avg_rect_copy_tasks = (double)perf_rect_copy_tasks_total / frame_count;
    const double avg_batch_runs = (double)perf_batch_runs_total / frame_count;
    const double avg_batched_task_count = (double)perf_batched_task_count_total / frame_count;
    const double avg_rect_texture_runs = (double)perf_rect_texture_runs_total / frame_count;
    const double avg_rect_texture_multi_runs = (double)perf_rect_texture_multi_runs_total / frame_count;
    const double avg_rect_texture_multi_run_tasks = (double)perf_rect_texture_multi_run_tasks_total / frame_count;
    const double avg_rect_texture_max_run = (double)perf_rect_texture_max_run_total / frame_count;
    const double avg_rect_texture_hstrip_runs = (double)perf_rect_texture_hstrip_runs_total / frame_count;
    const double avg_rect_texture_hstrip_tasks = (double)perf_rect_texture_hstrip_tasks_total / frame_count;
    const double avg_rect_texture_vstrip_runs = (double)perf_rect_texture_vstrip_runs_total / frame_count;
    const double avg_rect_texture_vstrip_tasks = (double)perf_rect_texture_vstrip_tasks_total / frame_count;
    const double avg_rect_texture_run_links = (double)perf_rect_texture_run_links_total / frame_count;
    const double avg_rect_texture_color_breaks = (double)perf_rect_texture_color_breaks_total / frame_count;
    const double avg_rect_texture_flip_breaks = (double)perf_rect_texture_flip_breaks_total / frame_count;
    const double avg_rect_texture_flipped_tasks = (double)perf_rect_texture_flipped_tasks_total / frame_count;
    const double avg_textured_geometry_tasks = (double)perf_textured_geometry_tasks_total / frame_count;
    const double avg_textured_geometry_rect_recovered_tasks =
        (double)perf_textured_geometry_rect_recovered_tasks_total / frame_count;
    const double avg_textured_geometry_fallback_tasks = (double)perf_textured_geometry_fallback_tasks_total / frame_count;
    const double avg_set_texture_calls = (double)perf_set_texture_calls_total / frame_count;
    const double avg_texture_binding_reuse_hits = (double)perf_texture_binding_reuse_hits_total / frame_count;
    const double avg_texture_cache_hits = (double)perf_texture_cache_hits_total / frame_count;
    const double avg_texture_cache_misses = (double)perf_texture_cache_misses_total / frame_count;
    const double avg_texture_creates = (double)perf_texture_creates_total / frame_count;
    const double avg_texture_unlock_calls = (double)perf_texture_unlock_calls_total / frame_count;
    const double avg_palette_unlock_calls = (double)perf_palette_unlock_calls_total / frame_count;
    const double avg_texture_cache_evictions = (double)perf_texture_cache_evictions_total / frame_count;
    const double avg_palette_cache_evictions = (double)perf_palette_cache_evictions_total / frame_count;
    const double avg_textures_destroy_queued = (double)perf_textures_destroy_queued_total / frame_count;
    const double avg_unknown_tasks = (double)perf_unknown_tasks_total / frame_count;
    const double avg_ppg_tasks = (double)perf_ppg_tasks_total / frame_count;
    const double avg_mtrans_tasks = (double)perf_mtrans_tasks_total / frame_count;
    const double avg_ui_direct_tasks = (double)perf_ui_direct_tasks_total / frame_count;
    const double avg_solid_tasks = (double)perf_solid_tasks_total / frame_count;
    const double avg_hybrid_candidate_tasks = (double)perf_hybrid_candidate_tasks_total / frame_count;
    const double avg_hybrid_candidate_pixels = (double)perf_hybrid_candidate_pixels_total / frame_count;
    const double avg_hybrid_fallback_tasks = (double)perf_hybrid_fallback_tasks_total / frame_count;
    const double avg_hybrid_fallback_pixels = (double)perf_hybrid_fallback_pixels_total / frame_count;
    const double avg_hybrid_reason_clip = (double)perf_hybrid_reason_clip_total / frame_count;
    const double avg_hybrid_reason_alpha = (double)perf_hybrid_reason_alpha_total / frame_count;
    const double avg_hybrid_reason_color_mod = (double)perf_hybrid_reason_color_mod_total / frame_count;
    const double avg_hybrid_reason_flip = (double)perf_hybrid_reason_flip_total / frame_count;
    const double avg_hybrid_reason_geometry = (double)perf_hybrid_reason_geometry_total / frame_count;
    const double avg_hybrid_reason_solid = (double)perf_hybrid_reason_solid_total / frame_count;
    const int dirty_tile_total = SDLGameRenderer_GetDirtyTileTotal();
    const double avg_dirty_ratio = dirty_tile_total > 0 ? avg_dirty_tiles / (double)dirty_tile_total : 0.0;
    const double avg_dirty_hit_rate = perf_dirty_hit_rate_total / frame_count;
    const double full_copy_fallback_ratio = (double)perf_full_copy_fallback_frames / frame_count;
    const double fps = avg_frame_ms > 0.0 ? 1000.0 / avg_frame_ms : 0.0;

    double min_frame_ms = perf_samples[0].frame_time_ms;
    double max_frame_ms = perf_samples[0].frame_time_ms;
    double min_update_ms = perf_samples[0].update_ms;
    double max_update_ms = perf_samples[0].update_ms;
    double min_render_ms = perf_samples[0].render_ms;
    double max_render_ms = perf_samples[0].render_ms;
    double min_present_ms = perf_samples[0].present_ms;
    double max_present_ms = perf_samples[0].present_ms;
    double min_present_readback_ms = perf_samples[0].present_readback_ms;
    double max_present_readback_ms = perf_samples[0].present_readback_ms;
    double min_present_convert_ms = perf_samples[0].present_convert_ms;
    double max_present_convert_ms = perf_samples[0].present_convert_ms;
    double min_present_copy_ms = perf_samples[0].present_copy_ms;
    double max_present_copy_ms = perf_samples[0].present_copy_ms;
    double min_present_clear_ms = perf_samples[0].present_clear_ms;
    double max_present_clear_ms = perf_samples[0].present_clear_ms;
    size_t min_copy_bytes = perf_samples[0].copy_bytes;
    size_t max_copy_bytes = perf_samples[0].copy_bytes;
    int min_dirty_tiles = perf_samples[0].dirty_tiles;
    int max_dirty_tiles = perf_samples[0].dirty_tiles;
    int min_render_task_count = perf_samples[0].render_task_count;
    int max_render_task_count = perf_samples[0].render_task_count;
    int min_rect_copy_tasks = perf_samples[0].rect_copy_tasks;
    int max_rect_copy_tasks = perf_samples[0].rect_copy_tasks;
    int min_batch_runs = perf_samples[0].batch_runs;
    int max_batch_runs = perf_samples[0].batch_runs;
    int min_batched_task_count = perf_samples[0].batched_task_count;
    int max_batched_task_count = perf_samples[0].batched_task_count;
    int min_rect_texture_runs = perf_samples[0].rect_texture_runs;
    int max_rect_texture_runs = perf_samples[0].rect_texture_runs;
    int min_rect_texture_multi_runs = perf_samples[0].rect_texture_multi_runs;
    int max_rect_texture_multi_runs = perf_samples[0].rect_texture_multi_runs;
    int min_rect_texture_multi_run_tasks = perf_samples[0].rect_texture_multi_run_tasks;
    int max_rect_texture_multi_run_tasks = perf_samples[0].rect_texture_multi_run_tasks;
    int min_rect_texture_max_run = perf_samples[0].rect_texture_max_run;
    int max_rect_texture_max_run = perf_samples[0].rect_texture_max_run;
    int min_rect_texture_hstrip_runs = perf_samples[0].rect_texture_hstrip_runs;
    int max_rect_texture_hstrip_runs = perf_samples[0].rect_texture_hstrip_runs;
    int min_rect_texture_hstrip_tasks = perf_samples[0].rect_texture_hstrip_tasks;
    int max_rect_texture_hstrip_tasks = perf_samples[0].rect_texture_hstrip_tasks;
    int min_rect_texture_vstrip_runs = perf_samples[0].rect_texture_vstrip_runs;
    int max_rect_texture_vstrip_runs = perf_samples[0].rect_texture_vstrip_runs;
    int min_rect_texture_vstrip_tasks = perf_samples[0].rect_texture_vstrip_tasks;
    int max_rect_texture_vstrip_tasks = perf_samples[0].rect_texture_vstrip_tasks;
    int min_rect_texture_run_links = perf_samples[0].rect_texture_run_links;
    int max_rect_texture_run_links = perf_samples[0].rect_texture_run_links;
    int min_rect_texture_color_breaks = perf_samples[0].rect_texture_color_breaks;
    int max_rect_texture_color_breaks = perf_samples[0].rect_texture_color_breaks;
    int min_rect_texture_flip_breaks = perf_samples[0].rect_texture_flip_breaks;
    int max_rect_texture_flip_breaks = perf_samples[0].rect_texture_flip_breaks;
    int min_rect_texture_flipped_tasks = perf_samples[0].rect_texture_flipped_tasks;
    int max_rect_texture_flipped_tasks = perf_samples[0].rect_texture_flipped_tasks;
    int min_textured_geometry_tasks = perf_samples[0].textured_geometry_tasks;
    int max_textured_geometry_tasks = perf_samples[0].textured_geometry_tasks;
    int min_textured_geometry_rect_recovered_tasks = perf_samples[0].textured_geometry_rect_recovered_tasks;
    int max_textured_geometry_rect_recovered_tasks = perf_samples[0].textured_geometry_rect_recovered_tasks;
    int min_textured_geometry_fallback_tasks = perf_samples[0].textured_geometry_fallback_tasks;
    int max_textured_geometry_fallback_tasks = perf_samples[0].textured_geometry_fallback_tasks;
    int min_set_texture_calls = perf_samples[0].set_texture_calls;
    int max_set_texture_calls = perf_samples[0].set_texture_calls;
    int min_texture_binding_reuse_hits = perf_samples[0].texture_binding_reuse_hits;
    int max_texture_binding_reuse_hits = perf_samples[0].texture_binding_reuse_hits;
    int min_texture_cache_hits = perf_samples[0].texture_cache_hits;
    int max_texture_cache_hits = perf_samples[0].texture_cache_hits;
    int min_texture_cache_misses = perf_samples[0].texture_cache_misses;
    int max_texture_cache_misses = perf_samples[0].texture_cache_misses;
    int min_texture_creates = perf_samples[0].texture_creates;
    int max_texture_creates = perf_samples[0].texture_creates;
    int min_texture_unlock_calls = perf_samples[0].texture_unlock_calls;
    int max_texture_unlock_calls = perf_samples[0].texture_unlock_calls;
    int min_palette_unlock_calls = perf_samples[0].palette_unlock_calls;
    int max_palette_unlock_calls = perf_samples[0].palette_unlock_calls;
    int min_texture_cache_evictions = perf_samples[0].texture_cache_evictions;
    int max_texture_cache_evictions = perf_samples[0].texture_cache_evictions;
    int min_palette_cache_evictions = perf_samples[0].palette_cache_evictions;
    int max_palette_cache_evictions = perf_samples[0].palette_cache_evictions;
    int min_textures_destroy_queued = perf_samples[0].textures_destroy_queued;
    int max_textures_destroy_queued = perf_samples[0].textures_destroy_queued;
    int min_unknown_tasks = perf_samples[0].unknown_tasks;
    int max_unknown_tasks = perf_samples[0].unknown_tasks;
    int min_ppg_tasks = perf_samples[0].ppg_tasks;
    int max_ppg_tasks = perf_samples[0].ppg_tasks;
    int min_mtrans_tasks = perf_samples[0].mtrans_tasks;
    int max_mtrans_tasks = perf_samples[0].mtrans_tasks;
    int min_ui_direct_tasks = perf_samples[0].ui_direct_tasks;
    int max_ui_direct_tasks = perf_samples[0].ui_direct_tasks;
    int min_solid_tasks = perf_samples[0].solid_tasks;
    int max_solid_tasks = perf_samples[0].solid_tasks;
    int min_hybrid_candidate_tasks = perf_samples[0].hybrid_candidate_tasks;
    int max_hybrid_candidate_tasks = perf_samples[0].hybrid_candidate_tasks;
    Uint64 min_hybrid_candidate_pixels = perf_samples[0].hybrid_candidate_pixels;
    Uint64 max_hybrid_candidate_pixels = perf_samples[0].hybrid_candidate_pixels;
    int min_hybrid_fallback_tasks = perf_samples[0].hybrid_fallback_tasks;
    int max_hybrid_fallback_tasks = perf_samples[0].hybrid_fallback_tasks;
    Uint64 min_hybrid_fallback_pixels = perf_samples[0].hybrid_fallback_pixels;
    Uint64 max_hybrid_fallback_pixels = perf_samples[0].hybrid_fallback_pixels;
    int min_hybrid_reason_clip = perf_samples[0].hybrid_reason_clip;
    int max_hybrid_reason_clip = perf_samples[0].hybrid_reason_clip;
    int min_hybrid_reason_alpha = perf_samples[0].hybrid_reason_alpha;
    int max_hybrid_reason_alpha = perf_samples[0].hybrid_reason_alpha;
    int min_hybrid_reason_color_mod = perf_samples[0].hybrid_reason_color_mod;
    int max_hybrid_reason_color_mod = perf_samples[0].hybrid_reason_color_mod;
    int min_hybrid_reason_flip = perf_samples[0].hybrid_reason_flip;
    int max_hybrid_reason_flip = perf_samples[0].hybrid_reason_flip;
    int min_hybrid_reason_geometry = perf_samples[0].hybrid_reason_geometry;
    int max_hybrid_reason_geometry = perf_samples[0].hybrid_reason_geometry;
    int min_hybrid_reason_solid = perf_samples[0].hybrid_reason_solid;
    int max_hybrid_reason_solid = perf_samples[0].hybrid_reason_solid;
    double min_dirty_ratio = perf_samples[0].dirty_ratio;
    double max_dirty_ratio = perf_samples[0].dirty_ratio;
    double min_dirty_hit_rate = perf_samples[0].dirty_hit_rate;
    double max_dirty_hit_rate = perf_samples[0].dirty_hit_rate;
    Uint32 summary_readback_format = perf_samples[0].readback_format;
    bool mixed_readback_format = false;
    int summary_readback_width = perf_samples[0].readback_width;
    int summary_readback_height = perf_samples[0].readback_height;
    bool mixed_readback_size = false;

    for (int i = 1; i < perf_capture_recorded_frames; i++) {
        const PerfFrameSample* sample = &perf_samples[i];
        if (sample->frame_time_ms < min_frame_ms) {
            min_frame_ms = sample->frame_time_ms;
        }
        if (sample->frame_time_ms > max_frame_ms) {
            max_frame_ms = sample->frame_time_ms;
        }
        if (sample->update_ms < min_update_ms) {
            min_update_ms = sample->update_ms;
        }
        if (sample->update_ms > max_update_ms) {
            max_update_ms = sample->update_ms;
        }
        if (sample->render_ms < min_render_ms) {
            min_render_ms = sample->render_ms;
        }
        if (sample->render_ms > max_render_ms) {
            max_render_ms = sample->render_ms;
        }
        if (sample->present_ms < min_present_ms) {
            min_present_ms = sample->present_ms;
        }
        if (sample->present_ms > max_present_ms) {
            max_present_ms = sample->present_ms;
        }
        if (sample->present_readback_ms < min_present_readback_ms) {
            min_present_readback_ms = sample->present_readback_ms;
        }
        if (sample->present_readback_ms > max_present_readback_ms) {
            max_present_readback_ms = sample->present_readback_ms;
        }
        if (sample->present_convert_ms < min_present_convert_ms) {
            min_present_convert_ms = sample->present_convert_ms;
        }
        if (sample->present_convert_ms > max_present_convert_ms) {
            max_present_convert_ms = sample->present_convert_ms;
        }
        if (sample->present_copy_ms < min_present_copy_ms) {
            min_present_copy_ms = sample->present_copy_ms;
        }
        if (sample->present_copy_ms > max_present_copy_ms) {
            max_present_copy_ms = sample->present_copy_ms;
        }
        if (sample->present_clear_ms < min_present_clear_ms) {
            min_present_clear_ms = sample->present_clear_ms;
        }
        if (sample->present_clear_ms > max_present_clear_ms) {
            max_present_clear_ms = sample->present_clear_ms;
        }
        if (sample->copy_bytes < min_copy_bytes) {
            min_copy_bytes = sample->copy_bytes;
        }
        if (sample->copy_bytes > max_copy_bytes) {
            max_copy_bytes = sample->copy_bytes;
        }
        if (sample->dirty_tiles < min_dirty_tiles) {
            min_dirty_tiles = sample->dirty_tiles;
        }
        if (sample->dirty_tiles > max_dirty_tiles) {
            max_dirty_tiles = sample->dirty_tiles;
        }
        if (sample->render_task_count < min_render_task_count) {
            min_render_task_count = sample->render_task_count;
        }
        if (sample->render_task_count > max_render_task_count) {
            max_render_task_count = sample->render_task_count;
        }
        if (sample->rect_copy_tasks < min_rect_copy_tasks) {
            min_rect_copy_tasks = sample->rect_copy_tasks;
        }
        if (sample->rect_copy_tasks > max_rect_copy_tasks) {
            max_rect_copy_tasks = sample->rect_copy_tasks;
        }
        if (sample->batch_runs < min_batch_runs) {
            min_batch_runs = sample->batch_runs;
        }
        if (sample->batch_runs > max_batch_runs) {
            max_batch_runs = sample->batch_runs;
        }
        if (sample->batched_task_count < min_batched_task_count) {
            min_batched_task_count = sample->batched_task_count;
        }
        if (sample->batched_task_count > max_batched_task_count) {
            max_batched_task_count = sample->batched_task_count;
        }
        if (sample->rect_texture_runs < min_rect_texture_runs) {
            min_rect_texture_runs = sample->rect_texture_runs;
        }
        if (sample->rect_texture_runs > max_rect_texture_runs) {
            max_rect_texture_runs = sample->rect_texture_runs;
        }
        if (sample->rect_texture_multi_runs < min_rect_texture_multi_runs) {
            min_rect_texture_multi_runs = sample->rect_texture_multi_runs;
        }
        if (sample->rect_texture_multi_runs > max_rect_texture_multi_runs) {
            max_rect_texture_multi_runs = sample->rect_texture_multi_runs;
        }
        if (sample->rect_texture_multi_run_tasks < min_rect_texture_multi_run_tasks) {
            min_rect_texture_multi_run_tasks = sample->rect_texture_multi_run_tasks;
        }
        if (sample->rect_texture_multi_run_tasks > max_rect_texture_multi_run_tasks) {
            max_rect_texture_multi_run_tasks = sample->rect_texture_multi_run_tasks;
        }
        if (sample->rect_texture_max_run < min_rect_texture_max_run) {
            min_rect_texture_max_run = sample->rect_texture_max_run;
        }
        if (sample->rect_texture_max_run > max_rect_texture_max_run) {
            max_rect_texture_max_run = sample->rect_texture_max_run;
        }
        if (sample->rect_texture_hstrip_runs < min_rect_texture_hstrip_runs) {
            min_rect_texture_hstrip_runs = sample->rect_texture_hstrip_runs;
        }
        if (sample->rect_texture_hstrip_runs > max_rect_texture_hstrip_runs) {
            max_rect_texture_hstrip_runs = sample->rect_texture_hstrip_runs;
        }
        if (sample->rect_texture_hstrip_tasks < min_rect_texture_hstrip_tasks) {
            min_rect_texture_hstrip_tasks = sample->rect_texture_hstrip_tasks;
        }
        if (sample->rect_texture_hstrip_tasks > max_rect_texture_hstrip_tasks) {
            max_rect_texture_hstrip_tasks = sample->rect_texture_hstrip_tasks;
        }
        if (sample->rect_texture_vstrip_runs < min_rect_texture_vstrip_runs) {
            min_rect_texture_vstrip_runs = sample->rect_texture_vstrip_runs;
        }
        if (sample->rect_texture_vstrip_runs > max_rect_texture_vstrip_runs) {
            max_rect_texture_vstrip_runs = sample->rect_texture_vstrip_runs;
        }
        if (sample->rect_texture_vstrip_tasks < min_rect_texture_vstrip_tasks) {
            min_rect_texture_vstrip_tasks = sample->rect_texture_vstrip_tasks;
        }
        if (sample->rect_texture_vstrip_tasks > max_rect_texture_vstrip_tasks) {
            max_rect_texture_vstrip_tasks = sample->rect_texture_vstrip_tasks;
        }
        if (sample->rect_texture_run_links < min_rect_texture_run_links) {
            min_rect_texture_run_links = sample->rect_texture_run_links;
        }
        if (sample->rect_texture_run_links > max_rect_texture_run_links) {
            max_rect_texture_run_links = sample->rect_texture_run_links;
        }
        if (sample->rect_texture_color_breaks < min_rect_texture_color_breaks) {
            min_rect_texture_color_breaks = sample->rect_texture_color_breaks;
        }
        if (sample->rect_texture_color_breaks > max_rect_texture_color_breaks) {
            max_rect_texture_color_breaks = sample->rect_texture_color_breaks;
        }
        if (sample->rect_texture_flip_breaks < min_rect_texture_flip_breaks) {
            min_rect_texture_flip_breaks = sample->rect_texture_flip_breaks;
        }
        if (sample->rect_texture_flip_breaks > max_rect_texture_flip_breaks) {
            max_rect_texture_flip_breaks = sample->rect_texture_flip_breaks;
        }
        if (sample->rect_texture_flipped_tasks < min_rect_texture_flipped_tasks) {
            min_rect_texture_flipped_tasks = sample->rect_texture_flipped_tasks;
        }
        if (sample->rect_texture_flipped_tasks > max_rect_texture_flipped_tasks) {
            max_rect_texture_flipped_tasks = sample->rect_texture_flipped_tasks;
        }
        if (sample->textured_geometry_tasks < min_textured_geometry_tasks) {
            min_textured_geometry_tasks = sample->textured_geometry_tasks;
        }
        if (sample->textured_geometry_tasks > max_textured_geometry_tasks) {
            max_textured_geometry_tasks = sample->textured_geometry_tasks;
        }
        if (sample->textured_geometry_rect_recovered_tasks < min_textured_geometry_rect_recovered_tasks) {
            min_textured_geometry_rect_recovered_tasks = sample->textured_geometry_rect_recovered_tasks;
        }
        if (sample->textured_geometry_rect_recovered_tasks > max_textured_geometry_rect_recovered_tasks) {
            max_textured_geometry_rect_recovered_tasks = sample->textured_geometry_rect_recovered_tasks;
        }
        if (sample->textured_geometry_fallback_tasks < min_textured_geometry_fallback_tasks) {
            min_textured_geometry_fallback_tasks = sample->textured_geometry_fallback_tasks;
        }
        if (sample->textured_geometry_fallback_tasks > max_textured_geometry_fallback_tasks) {
            max_textured_geometry_fallback_tasks = sample->textured_geometry_fallback_tasks;
        }
        if (sample->set_texture_calls < min_set_texture_calls) {
            min_set_texture_calls = sample->set_texture_calls;
        }
        if (sample->set_texture_calls > max_set_texture_calls) {
            max_set_texture_calls = sample->set_texture_calls;
        }
        if (sample->texture_binding_reuse_hits < min_texture_binding_reuse_hits) {
            min_texture_binding_reuse_hits = sample->texture_binding_reuse_hits;
        }
        if (sample->texture_binding_reuse_hits > max_texture_binding_reuse_hits) {
            max_texture_binding_reuse_hits = sample->texture_binding_reuse_hits;
        }
        if (sample->texture_cache_hits < min_texture_cache_hits) {
            min_texture_cache_hits = sample->texture_cache_hits;
        }
        if (sample->texture_cache_hits > max_texture_cache_hits) {
            max_texture_cache_hits = sample->texture_cache_hits;
        }
        if (sample->texture_cache_misses < min_texture_cache_misses) {
            min_texture_cache_misses = sample->texture_cache_misses;
        }
        if (sample->texture_cache_misses > max_texture_cache_misses) {
            max_texture_cache_misses = sample->texture_cache_misses;
        }
        if (sample->texture_creates < min_texture_creates) {
            min_texture_creates = sample->texture_creates;
        }
        if (sample->texture_creates > max_texture_creates) {
            max_texture_creates = sample->texture_creates;
        }
        if (sample->texture_unlock_calls < min_texture_unlock_calls) {
            min_texture_unlock_calls = sample->texture_unlock_calls;
        }
        if (sample->texture_unlock_calls > max_texture_unlock_calls) {
            max_texture_unlock_calls = sample->texture_unlock_calls;
        }
        if (sample->palette_unlock_calls < min_palette_unlock_calls) {
            min_palette_unlock_calls = sample->palette_unlock_calls;
        }
        if (sample->palette_unlock_calls > max_palette_unlock_calls) {
            max_palette_unlock_calls = sample->palette_unlock_calls;
        }
        if (sample->texture_cache_evictions < min_texture_cache_evictions) {
            min_texture_cache_evictions = sample->texture_cache_evictions;
        }
        if (sample->texture_cache_evictions > max_texture_cache_evictions) {
            max_texture_cache_evictions = sample->texture_cache_evictions;
        }
        if (sample->palette_cache_evictions < min_palette_cache_evictions) {
            min_palette_cache_evictions = sample->palette_cache_evictions;
        }
        if (sample->palette_cache_evictions > max_palette_cache_evictions) {
            max_palette_cache_evictions = sample->palette_cache_evictions;
        }
        if (sample->textures_destroy_queued < min_textures_destroy_queued) {
            min_textures_destroy_queued = sample->textures_destroy_queued;
        }
        if (sample->textures_destroy_queued > max_textures_destroy_queued) {
            max_textures_destroy_queued = sample->textures_destroy_queued;
        }
        if (sample->unknown_tasks < min_unknown_tasks) {
            min_unknown_tasks = sample->unknown_tasks;
        }
        if (sample->unknown_tasks > max_unknown_tasks) {
            max_unknown_tasks = sample->unknown_tasks;
        }
        if (sample->ppg_tasks < min_ppg_tasks) {
            min_ppg_tasks = sample->ppg_tasks;
        }
        if (sample->ppg_tasks > max_ppg_tasks) {
            max_ppg_tasks = sample->ppg_tasks;
        }
        if (sample->mtrans_tasks < min_mtrans_tasks) {
            min_mtrans_tasks = sample->mtrans_tasks;
        }
        if (sample->mtrans_tasks > max_mtrans_tasks) {
            max_mtrans_tasks = sample->mtrans_tasks;
        }
        if (sample->ui_direct_tasks < min_ui_direct_tasks) {
            min_ui_direct_tasks = sample->ui_direct_tasks;
        }
        if (sample->ui_direct_tasks > max_ui_direct_tasks) {
            max_ui_direct_tasks = sample->ui_direct_tasks;
        }
        if (sample->solid_tasks < min_solid_tasks) {
            min_solid_tasks = sample->solid_tasks;
        }
        if (sample->solid_tasks > max_solid_tasks) {
            max_solid_tasks = sample->solid_tasks;
        }
        if (sample->hybrid_candidate_tasks < min_hybrid_candidate_tasks) {
            min_hybrid_candidate_tasks = sample->hybrid_candidate_tasks;
        }
        if (sample->hybrid_candidate_tasks > max_hybrid_candidate_tasks) {
            max_hybrid_candidate_tasks = sample->hybrid_candidate_tasks;
        }
        if (sample->hybrid_candidate_pixels < min_hybrid_candidate_pixels) {
            min_hybrid_candidate_pixels = sample->hybrid_candidate_pixels;
        }
        if (sample->hybrid_candidate_pixels > max_hybrid_candidate_pixels) {
            max_hybrid_candidate_pixels = sample->hybrid_candidate_pixels;
        }
        if (sample->hybrid_fallback_tasks < min_hybrid_fallback_tasks) {
            min_hybrid_fallback_tasks = sample->hybrid_fallback_tasks;
        }
        if (sample->hybrid_fallback_tasks > max_hybrid_fallback_tasks) {
            max_hybrid_fallback_tasks = sample->hybrid_fallback_tasks;
        }
        if (sample->hybrid_fallback_pixels < min_hybrid_fallback_pixels) {
            min_hybrid_fallback_pixels = sample->hybrid_fallback_pixels;
        }
        if (sample->hybrid_fallback_pixels > max_hybrid_fallback_pixels) {
            max_hybrid_fallback_pixels = sample->hybrid_fallback_pixels;
        }
        if (sample->hybrid_reason_clip < min_hybrid_reason_clip) {
            min_hybrid_reason_clip = sample->hybrid_reason_clip;
        }
        if (sample->hybrid_reason_clip > max_hybrid_reason_clip) {
            max_hybrid_reason_clip = sample->hybrid_reason_clip;
        }
        if (sample->hybrid_reason_alpha < min_hybrid_reason_alpha) {
            min_hybrid_reason_alpha = sample->hybrid_reason_alpha;
        }
        if (sample->hybrid_reason_alpha > max_hybrid_reason_alpha) {
            max_hybrid_reason_alpha = sample->hybrid_reason_alpha;
        }
        if (sample->hybrid_reason_color_mod < min_hybrid_reason_color_mod) {
            min_hybrid_reason_color_mod = sample->hybrid_reason_color_mod;
        }
        if (sample->hybrid_reason_color_mod > max_hybrid_reason_color_mod) {
            max_hybrid_reason_color_mod = sample->hybrid_reason_color_mod;
        }
        if (sample->hybrid_reason_flip < min_hybrid_reason_flip) {
            min_hybrid_reason_flip = sample->hybrid_reason_flip;
        }
        if (sample->hybrid_reason_flip > max_hybrid_reason_flip) {
            max_hybrid_reason_flip = sample->hybrid_reason_flip;
        }
        if (sample->hybrid_reason_geometry < min_hybrid_reason_geometry) {
            min_hybrid_reason_geometry = sample->hybrid_reason_geometry;
        }
        if (sample->hybrid_reason_geometry > max_hybrid_reason_geometry) {
            max_hybrid_reason_geometry = sample->hybrid_reason_geometry;
        }
        if (sample->hybrid_reason_solid < min_hybrid_reason_solid) {
            min_hybrid_reason_solid = sample->hybrid_reason_solid;
        }
        if (sample->hybrid_reason_solid > max_hybrid_reason_solid) {
            max_hybrid_reason_solid = sample->hybrid_reason_solid;
        }
        if (sample->dirty_ratio < min_dirty_ratio) {
            min_dirty_ratio = sample->dirty_ratio;
        }
        if (sample->dirty_ratio > max_dirty_ratio) {
            max_dirty_ratio = sample->dirty_ratio;
        }
        if (sample->dirty_hit_rate < min_dirty_hit_rate) {
            min_dirty_hit_rate = sample->dirty_hit_rate;
        }
        if (sample->dirty_hit_rate > max_dirty_hit_rate) {
            max_dirty_hit_rate = sample->dirty_hit_rate;
        }
        if (sample->readback_format != summary_readback_format) {
            mixed_readback_format = true;
        }
        if ((sample->readback_width != summary_readback_width) || (sample->readback_height != summary_readback_height)) {
            mixed_readback_size = true;
        }
    }

    io_printf(io, "{\n");
    io_printf(io, "  \"schema_version\": 6,\n");
    io_printf(io, "  \"scene\": \"");
    io_write_json_escaped_string(io, perf_capture_scene_name);
    io_printf(io, "\",\n");
    io_printf(io, "  \"frames\": %d,\n", perf_capture_recorded_frames);
    io_printf(io, "  \"metrics\": {\n");
    io_printf(io, "    \"frame_time\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_frame_ms,
              min_frame_ms,
              max_frame_ms);
    io_printf(io, "    \"update\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_update_ms,
              min_update_ms,
              max_update_ms);
    io_printf(io, "    \"render\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_render_ms,
              min_render_ms,
              max_render_ms);
    io_printf(io, "    \"present\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_present_ms,
              min_present_ms,
              max_present_ms);
    io_printf(io, "    \"present_readback\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_present_readback_ms,
              min_present_readback_ms,
              max_present_readback_ms);
    io_printf(io, "    \"present_convert\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_present_convert_ms,
              min_present_convert_ms,
              max_present_convert_ms);
    io_printf(io, "    \"present_copy\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_present_copy_ms,
              min_present_copy_ms,
              max_present_copy_ms);
    io_printf(io, "    \"present_clear\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_present_clear_ms,
              min_present_clear_ms,
              max_present_clear_ms);
    io_printf(io, "    \"copy_bytes\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_copy_bytes,
              (unsigned long long)min_copy_bytes,
              (unsigned long long)max_copy_bytes);
    io_printf(io, "    \"dirty_tiles\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_dirty_tiles,
              min_dirty_tiles,
              max_dirty_tiles);
    io_printf(io, "    \"render_task_count\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_render_task_count,
              min_render_task_count,
              max_render_task_count);
    io_printf(io, "    \"rect_copy_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_copy_tasks,
              min_rect_copy_tasks,
              max_rect_copy_tasks);
    io_printf(io, "    \"batch_runs\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_batch_runs,
              min_batch_runs,
              max_batch_runs);
    io_printf(io, "    \"batched_task_count\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_batched_task_count,
              min_batched_task_count,
              max_batched_task_count);
    io_printf(io, "    \"rect_texture_runs\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_runs,
              min_rect_texture_runs,
              max_rect_texture_runs);
    io_printf(io, "    \"rect_texture_multi_runs\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_multi_runs,
              min_rect_texture_multi_runs,
              max_rect_texture_multi_runs);
    io_printf(io, "    \"rect_texture_multi_run_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_multi_run_tasks,
              min_rect_texture_multi_run_tasks,
              max_rect_texture_multi_run_tasks);
    io_printf(io, "    \"rect_texture_max_run\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_max_run,
              min_rect_texture_max_run,
              max_rect_texture_max_run);
    io_printf(io, "    \"rect_texture_hstrip_runs\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_hstrip_runs,
              min_rect_texture_hstrip_runs,
              max_rect_texture_hstrip_runs);
    io_printf(io, "    \"rect_texture_hstrip_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_hstrip_tasks,
              min_rect_texture_hstrip_tasks,
              max_rect_texture_hstrip_tasks);
    io_printf(io, "    \"rect_texture_vstrip_runs\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_vstrip_runs,
              min_rect_texture_vstrip_runs,
              max_rect_texture_vstrip_runs);
    io_printf(io, "    \"rect_texture_vstrip_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_vstrip_tasks,
              min_rect_texture_vstrip_tasks,
              max_rect_texture_vstrip_tasks);
    io_printf(io, "    \"rect_texture_run_links\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_run_links,
              min_rect_texture_run_links,
              max_rect_texture_run_links);
    io_printf(io, "    \"rect_texture_color_breaks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_color_breaks,
              min_rect_texture_color_breaks,
              max_rect_texture_color_breaks);
    io_printf(io, "    \"rect_texture_flip_breaks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_flip_breaks,
              min_rect_texture_flip_breaks,
              max_rect_texture_flip_breaks);
    io_printf(io, "    \"rect_texture_flipped_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_rect_texture_flipped_tasks,
              min_rect_texture_flipped_tasks,
              max_rect_texture_flipped_tasks);
    io_printf(io, "    \"textured_geometry_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_textured_geometry_tasks,
              min_textured_geometry_tasks,
              max_textured_geometry_tasks);
    io_printf(io, "    \"textured_geometry_rect_recovered_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_textured_geometry_rect_recovered_tasks,
              min_textured_geometry_rect_recovered_tasks,
              max_textured_geometry_rect_recovered_tasks);
    io_printf(io, "    \"textured_geometry_fallback_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_textured_geometry_fallback_tasks,
              min_textured_geometry_fallback_tasks,
              max_textured_geometry_fallback_tasks);
    io_printf(io, "    \"set_texture_calls\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_set_texture_calls,
              min_set_texture_calls,
              max_set_texture_calls);
    io_printf(io, "    \"texture_binding_reuse_hits\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_binding_reuse_hits,
              min_texture_binding_reuse_hits,
              max_texture_binding_reuse_hits);
    io_printf(io, "    \"texture_cache_hits\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_cache_hits,
              min_texture_cache_hits,
              max_texture_cache_hits);
    io_printf(io, "    \"texture_cache_misses\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_cache_misses,
              min_texture_cache_misses,
              max_texture_cache_misses);
    io_printf(io, "    \"texture_creates\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_creates,
              min_texture_creates,
              max_texture_creates);
    io_printf(io, "    \"texture_unlock_calls\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_unlock_calls,
              min_texture_unlock_calls,
              max_texture_unlock_calls);
    io_printf(io, "    \"palette_unlock_calls\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_palette_unlock_calls,
              min_palette_unlock_calls,
              max_palette_unlock_calls);
    io_printf(io, "    \"texture_cache_evictions\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_cache_evictions,
              min_texture_cache_evictions,
              max_texture_cache_evictions);
    io_printf(io, "    \"palette_cache_evictions\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_palette_cache_evictions,
              min_palette_cache_evictions,
              max_palette_cache_evictions);
    io_printf(io, "    \"textures_destroy_queued\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_textures_destroy_queued,
              min_textures_destroy_queued,
              max_textures_destroy_queued);
    io_printf(io, "    \"unknown_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_unknown_tasks,
              min_unknown_tasks,
              max_unknown_tasks);
    io_printf(io, "    \"ppg_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_ppg_tasks,
              min_ppg_tasks,
              max_ppg_tasks);
    io_printf(io, "    \"mtrans_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_mtrans_tasks,
              min_mtrans_tasks,
              max_mtrans_tasks);
    io_printf(io, "    \"ui_direct_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_ui_direct_tasks,
              min_ui_direct_tasks,
              max_ui_direct_tasks);
    io_printf(io, "    \"solid_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_solid_tasks,
              min_solid_tasks,
              max_solid_tasks);
    io_printf(io, "    \"hybrid_candidate_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_hybrid_candidate_tasks,
              min_hybrid_candidate_tasks,
              max_hybrid_candidate_tasks);
    io_printf(io, "    \"hybrid_candidate_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_hybrid_candidate_pixels,
              (unsigned long long)min_hybrid_candidate_pixels,
              (unsigned long long)max_hybrid_candidate_pixels);
    io_printf(io, "    \"hybrid_fallback_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_hybrid_fallback_tasks,
              min_hybrid_fallback_tasks,
              max_hybrid_fallback_tasks);
    io_printf(io, "    \"hybrid_fallback_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_hybrid_fallback_pixels,
              (unsigned long long)min_hybrid_fallback_pixels,
              (unsigned long long)max_hybrid_fallback_pixels);
    io_printf(io, "    \"hybrid_reason_clip\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_hybrid_reason_clip,
              min_hybrid_reason_clip,
              max_hybrid_reason_clip);
    io_printf(io, "    \"hybrid_reason_alpha\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_hybrid_reason_alpha,
              min_hybrid_reason_alpha,
              max_hybrid_reason_alpha);
    io_printf(io, "    \"hybrid_reason_color_mod\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_hybrid_reason_color_mod,
              min_hybrid_reason_color_mod,
              max_hybrid_reason_color_mod);
    io_printf(io, "    \"hybrid_reason_flip\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_hybrid_reason_flip,
              min_hybrid_reason_flip,
              max_hybrid_reason_flip);
    io_printf(io, "    \"hybrid_reason_geometry\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_hybrid_reason_geometry,
              min_hybrid_reason_geometry,
              max_hybrid_reason_geometry);
    io_printf(io, "    \"hybrid_reason_solid\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_hybrid_reason_solid,
              min_hybrid_reason_solid,
              max_hybrid_reason_solid);
    io_printf(io, "    \"dirty_ratio\": {\"mean\": %.6f, \"min\": %.6f, \"max\": %.6f},\n",
              avg_dirty_ratio,
              min_dirty_ratio,
              max_dirty_ratio);
    io_printf(io, "    \"dirty_hit_rate\": {\"mean\": %.6f, \"min\": %.6f, \"max\": %.6f},\n",
              avg_dirty_hit_rate,
              min_dirty_hit_rate,
              max_dirty_hit_rate);
    io_printf(io, "    \"full_copy_fallback\": {\"count\": %d, \"ratio\": %.6f},\n",
              perf_full_copy_fallback_frames,
              full_copy_fallback_ratio);
    io_printf(io,
              "    \"sort_strategy\": {\"none\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"equal_z_reverse\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"insertion\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"qsort\": {\"count\": %llu, \"ratio\": %.6f}},\n",
              (unsigned long long)perf_sort_strategy_frames[SDL_GAME_RENDERER_SORT_NONE],
              (double)perf_sort_strategy_frames[SDL_GAME_RENDERER_SORT_NONE] / frame_count,
              (unsigned long long)perf_sort_strategy_frames[SDL_GAME_RENDERER_SORT_EQUAL_Z_REVERSE],
              (double)perf_sort_strategy_frames[SDL_GAME_RENDERER_SORT_EQUAL_Z_REVERSE] / frame_count,
              (unsigned long long)perf_sort_strategy_frames[SDL_GAME_RENDERER_SORT_INSERTION],
              (double)perf_sort_strategy_frames[SDL_GAME_RENDERER_SORT_INSERTION] / frame_count,
              (unsigned long long)perf_sort_strategy_frames[SDL_GAME_RENDERER_SORT_QSORT],
              (double)perf_sort_strategy_frames[SDL_GAME_RENDERER_SORT_QSORT] / frame_count);
    io_printf(io,
              "    \"fbdev_present_path\": {\"none\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"readback_rect\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"current_target_exact\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"current_target_integer_scale\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"current_target_mapped_scale\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"fullscreen_staging\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"fullscreen_direct_copy\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"fullscreen_scaled_lut\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"fullscreen_scaled_div\": {\"count\": %llu, \"ratio\": %.6f}},\n",
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_NONE],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_NONE] / frame_count,
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_READBACK_RECT],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_READBACK_RECT] / frame_count,
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_CURRENT_TARGET_EXACT],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_CURRENT_TARGET_EXACT] / frame_count,
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_CURRENT_TARGET_INTEGER_SCALE],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_CURRENT_TARGET_INTEGER_SCALE] / frame_count,
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_CURRENT_TARGET_MAPPED_SCALE],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_CURRENT_TARGET_MAPPED_SCALE] / frame_count,
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_FULLSCREEN_STAGING],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_FULLSCREEN_STAGING] / frame_count,
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_FULLSCREEN_DIRECT_COPY],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_FULLSCREEN_DIRECT_COPY] / frame_count,
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_FULLSCREEN_SCALED_LUT],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_FULLSCREEN_SCALED_LUT] / frame_count,
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_FULLSCREEN_SCALED_DIV],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_FULLSCREEN_SCALED_DIV] / frame_count);
    io_printf(io,
              "    \"readback_surface\": {\"format\": \"%s\", \"mixed_format\": %s, \"width\": %d, \"height\": %d, "
              "\"mixed_size\": %s},\n",
              pixel_format_name_safe(summary_readback_format),
              mixed_readback_format ? "true" : "false",
              summary_readback_width,
              summary_readback_height,
              mixed_readback_size ? "true" : "false");
    io_printf(io, "    \"fps\": {\"mean\": %.4f}\n", fps);
    io_printf(io, "  },\n");
    io_printf(io, "  \"samples\": [\n");
    for (int i = 0; i < perf_capture_recorded_frames; i++) {
        const PerfFrameSample* sample = &perf_samples[i];
        io_printf(io,
                  "    {\"frame\": %d, \"frame_time_ms\": %.4f, \"update_ms\": %.4f, \"render_ms\": %.4f, "
                  "\"present_ms\": %.4f, \"present_readback_ms\": %.4f, \"present_convert_ms\": %.4f, "
                  "\"present_copy_ms\": %.4f, \"present_clear_ms\": %.4f, \"fbdev_present_path\": \"%s\", "
                  "\"readback_format\": \"%s\", \"readback_width\": %d, \"readback_height\": %d, "
                  "\"copy_bytes\": %llu, \"dirty_tiles\": %d, \"dirty_ratio\": %.6f, "
                  "\"tiles_total\": %d, \"tiles_copied\": %d, \"dirty_hit_rate\": %.6f, \"full_copy_fallback\": %s, "
                  "\"render_task_count\": %d, \"rect_copy_tasks\": %d, \"batch_runs\": %d, \"batched_task_count\": %d, "
                  "\"rect_texture_runs\": %d, \"rect_texture_multi_runs\": %d, \"rect_texture_multi_run_tasks\": %d, "
                  "\"rect_texture_max_run\": %d, \"rect_texture_hstrip_runs\": %d, "
                  "\"rect_texture_hstrip_tasks\": %d, \"rect_texture_vstrip_runs\": %d, "
                  "\"rect_texture_vstrip_tasks\": %d, \"rect_texture_run_links\": %d, "
                  "\"rect_texture_color_breaks\": %d, \"rect_texture_flip_breaks\": %d, "
                  "\"rect_texture_flipped_tasks\": %d, \"textured_geometry_tasks\": %d, "
                  "\"textured_geometry_rect_recovered_tasks\": %d, \"textured_geometry_fallback_tasks\": %d, "
                  "\"set_texture_calls\": %d, \"texture_binding_reuse_hits\": %d, "
                  "\"texture_cache_hits\": %d, \"texture_cache_misses\": %d, \"texture_creates\": %d, "
                  "\"texture_unlock_calls\": %d, \"palette_unlock_calls\": %d, "
                  "\"texture_cache_evictions\": %d, \"palette_cache_evictions\": %d, \"textures_destroy_queued\": %d, "
                  "\"unknown_tasks\": %d, \"ppg_tasks\": %d, \"mtrans_tasks\": %d, "
                  "\"ui_direct_tasks\": %d, \"solid_tasks\": %d, "
                  "\"hybrid_candidate_tasks\": %d, \"hybrid_candidate_pixels\": %llu, "
                  "\"hybrid_fallback_tasks\": %d, \"hybrid_fallback_pixels\": %llu, "
                  "\"hybrid_reason_clip\": %d, \"hybrid_reason_alpha\": %d, "
                  "\"hybrid_reason_color_mod\": %d, \"hybrid_reason_flip\": %d, "
                  "\"hybrid_reason_geometry\": %d, \"hybrid_reason_solid\": %d, "
                  "\"sort_strategy\": \"%s\"}%s\n",
                  i + 1,
                  sample->frame_time_ms,
                  sample->update_ms,
                  sample->render_ms,
                  sample->present_ms,
                  sample->present_readback_ms,
                  sample->present_convert_ms,
                  sample->present_copy_ms,
                  sample->present_clear_ms,
                  fbdev_present_path_name(sample->fbdev_path),
                  pixel_format_name_safe(sample->readback_format),
                  sample->readback_width,
                  sample->readback_height,
                  (unsigned long long)sample->copy_bytes,
                  sample->dirty_tiles,
                  sample->dirty_ratio,
                  sample->presenter_tiles_total,
                  sample->presenter_tiles_copied,
                  sample->dirty_hit_rate,
                  sample->full_copy_fallback ? "true" : "false",
                  sample->render_task_count,
                  sample->rect_copy_tasks,
                  sample->batch_runs,
                  sample->batched_task_count,
                  sample->rect_texture_runs,
                  sample->rect_texture_multi_runs,
                  sample->rect_texture_multi_run_tasks,
                  sample->rect_texture_max_run,
                  sample->rect_texture_hstrip_runs,
                  sample->rect_texture_hstrip_tasks,
                  sample->rect_texture_vstrip_runs,
                  sample->rect_texture_vstrip_tasks,
                  sample->rect_texture_run_links,
                  sample->rect_texture_color_breaks,
                  sample->rect_texture_flip_breaks,
                  sample->rect_texture_flipped_tasks,
                  sample->textured_geometry_tasks,
                  sample->textured_geometry_rect_recovered_tasks,
                  sample->textured_geometry_fallback_tasks,
                  sample->set_texture_calls,
                  sample->texture_binding_reuse_hits,
                  sample->texture_cache_hits,
                  sample->texture_cache_misses,
                  sample->texture_creates,
                  sample->texture_unlock_calls,
                  sample->palette_unlock_calls,
                  sample->texture_cache_evictions,
                  sample->palette_cache_evictions,
                  sample->textures_destroy_queued,
                  sample->unknown_tasks,
                  sample->ppg_tasks,
                  sample->mtrans_tasks,
                  sample->ui_direct_tasks,
                  sample->solid_tasks,
                  sample->hybrid_candidate_tasks,
                  (unsigned long long)sample->hybrid_candidate_pixels,
                  sample->hybrid_fallback_tasks,
                  (unsigned long long)sample->hybrid_fallback_pixels,
                  sample->hybrid_reason_clip,
                  sample->hybrid_reason_alpha,
                  sample->hybrid_reason_color_mod,
                  sample->hybrid_reason_flip,
                  sample->hybrid_reason_geometry,
                  sample->hybrid_reason_solid,
                  render_sort_strategy_name(sample->sort_strategy),
                  (i + 1) < perf_capture_recorded_frames ? "," : "");
    }
    io_printf(io, "  ]\n");
    io_printf(io, "}\n");
    SDL_CloseIO(io);

    backend_logf("PERF capture complete: frames=%d frame_time_ms=%.3f render_ms=%.3f present_ms=%.3f present_readback_ms=%.3f present_convert_ms=%.3f present_copy_ms=%.3f present_clear_ms=%.3f dominant_present_path=%s readback_format=%s readback_size=%dx%d copy_bytes=%.2f dirty_ratio=%.4f dirty_hit_rate=%.4f full_copy_fallback_ratio=%.4f rect_runs=%.2f rect_multi_runs=%.2f rect_multi_run_tasks=%.2f rect_max_run=%.2f rect_hstrip_runs=%.2f rect_hstrip_tasks=%.2f rect_vstrip_runs=%.2f rect_vstrip_tasks=%.2f rect_run_links=%.2f rect_color_breaks=%.2f rect_flip_breaks=%.2f rect_flipped_tasks=%.2f textured_geometry_tasks=%.2f textured_geometry_recovered=%.2f textured_geometry_fallback=%.2f set_texture_calls=%.2f binding_reuse=%.2f texture_unlocks=%.2f palette_unlocks=%.2f texture_evictions=%.2f palette_evictions=%.2f destroy_queue=%.2f source_ppg=%.2f source_mtrans=%.2f source_ui=%.2f source_solid=%.2f source_unknown=%.2f hybrid_candidate_tasks=%.2f hybrid_candidate_pixels=%.2f hybrid_fallback_tasks=%.2f hybrid_fallback_pixels=%.2f hybrid_reason_clip=%.2f hybrid_reason_alpha=%.2f hybrid_reason_color_mod=%.2f hybrid_reason_flip=%.2f hybrid_reason_geometry=%.2f hybrid_reason_solid=%.2f fps=%.2f output=%s",
                 perf_capture_recorded_frames,
                 avg_frame_ms,
                 avg_render_ms,
                 avg_present_ms,
                 avg_present_readback_ms,
                 avg_present_convert_ms,
                 avg_present_copy_ms,
                 avg_present_clear_ms,
                 fbdev_present_path_name(dominant_fbdev_present_path()),
                 pixel_format_name_safe(summary_readback_format),
                 summary_readback_width,
                 summary_readback_height,
                 avg_copy_bytes,
                 avg_dirty_ratio,
                 avg_dirty_hit_rate,
                 full_copy_fallback_ratio,
                 avg_rect_texture_runs,
                 avg_rect_texture_multi_runs,
                 avg_rect_texture_multi_run_tasks,
                 avg_rect_texture_max_run,
                 avg_rect_texture_hstrip_runs,
                 avg_rect_texture_hstrip_tasks,
                 avg_rect_texture_vstrip_runs,
                 avg_rect_texture_vstrip_tasks,
                 avg_rect_texture_run_links,
                 avg_rect_texture_color_breaks,
                 avg_rect_texture_flip_breaks,
                 avg_rect_texture_flipped_tasks,
                 avg_textured_geometry_tasks,
                 avg_textured_geometry_rect_recovered_tasks,
                 avg_textured_geometry_fallback_tasks,
                 avg_set_texture_calls,
                 avg_texture_binding_reuse_hits,
                 avg_texture_unlock_calls,
                 avg_palette_unlock_calls,
                 avg_texture_cache_evictions,
                 avg_palette_cache_evictions,
                 avg_textures_destroy_queued,
                 avg_ppg_tasks,
                 avg_mtrans_tasks,
                 avg_ui_direct_tasks,
                 avg_solid_tasks,
                 avg_unknown_tasks,
                 avg_hybrid_candidate_tasks,
                 avg_hybrid_candidate_pixels,
                 avg_hybrid_fallback_tasks,
                 avg_hybrid_fallback_pixels,
                 avg_hybrid_reason_clip,
                 avg_hybrid_reason_alpha,
                 avg_hybrid_reason_color_mod,
                 avg_hybrid_reason_flip,
                 avg_hybrid_reason_geometry,
                 avg_hybrid_reason_solid,
                 fps,
                 output_path);
    SDL_free(output_path);
}

static const char* get_effective_video_driver_order(const char* configured_order) {
#if defined(PORT_MISTER)
    if (configured_order != NULL && SDL_strcmp(configured_order, legacy_mister_video_driver_order) == 0) {
        return recommended_mister_video_driver_order;
    }
#endif
    return configured_order;
}

static void apply_backend_hints() {
    const char* configured_video_driver_order = Config_GetString(CFG_KEY_VIDEO_DRIVER_ORDER);
    const char* video_driver_order = get_effective_video_driver_order(configured_video_driver_order);
    const char* render_driver_order = Config_GetString(CFG_KEY_RENDER_DRIVER_ORDER);

    if (video_driver_order != NULL && video_driver_order[0] != '\0') {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, video_driver_order);
    }

    if (render_driver_order != NULL && render_driver_order[0] != '\0') {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, render_driver_order);
    }
}

static void log_backend_diagnostics() {
    const char* configured_video_order = Config_GetString(CFG_KEY_VIDEO_DRIVER_ORDER);
    const char* effective_video_order = get_effective_video_driver_order(configured_video_order);
    const char* render_order = Config_GetString(CFG_KEY_RENDER_DRIVER_ORDER);

    backend_logf("===== SDL backend probe start =====");
    backend_logf("Platform: %s", SDL_GetPlatform());
    backend_logf("Config video-driver-order: %s", configured_video_order != NULL ? configured_video_order : "(null)");
    backend_logf("Effective video-driver-order: %s", effective_video_order != NULL ? effective_video_order : "(null)");
    backend_logf("Config render-driver-order: %s", render_order != NULL ? render_order : "(null)");

    const int video_driver_count = SDL_GetNumVideoDrivers();
    backend_logf("Available video drivers (%d):", video_driver_count);
    for (int i = 0; i < video_driver_count; i++) {
        const char* name = SDL_GetVideoDriver(i);
        backend_logf("  video[%d]=%s", i, name != NULL ? name : "(null)");
    }

    const int render_driver_count = SDL_GetNumRenderDrivers();
    backend_logf("Available render drivers (%d):", render_driver_count);
    for (int i = 0; i < render_driver_count; i++) {
        const char* name = SDL_GetRenderDriver(i);
        backend_logf("  render[%d]=%s", i, name != NULL ? name : "(null)");
    }
}

static SDL_ScaleMode screen_texture_scale_mode() {
    switch (scale_mode) {
    case SCALEMODE_LINEAR:
    case SCALEMODE_SOFT_LINEAR:
        return SDL_SCALEMODE_LINEAR;

    case SCALEMODE_NATIVE:
    case SCALEMODE_NEAREST:
    case SCALEMODE_SQUARE_PIXELS:
    case SCALEMODE_INTEGER:
        return SDL_SCALEMODE_NEAREST;
    }

    return SDL_SCALEMODE_NEAREST;
}

static SDL_Point screen_texture_size() {
    SDL_Point size;
    SDL_GetRenderOutputSize(renderer, &size.x, &size.y);

    if (scale_mode == SCALEMODE_SOFT_LINEAR) {
        size.x *= 2;
        size.y *= 2;
    }

    return size;
}

static void create_screen_texture() {
    if (use_native_render_path) {
        if (screen_texture != NULL) {
            SDL_DestroyTexture(screen_texture);
            screen_texture = NULL;
        }
        native_output_rect_dirty = true;
        native_output_has_bars = true;
        native_output_width = 0;
        native_output_height = 0;
        screen_output_rect_dirty = true;
        screen_output_has_letterbox = true;
        screen_output_width = 0;
        screen_output_height = 0;
        return;
    }

    if (screen_texture != NULL) {
        SDL_DestroyTexture(screen_texture);
    }

    const SDL_Point size = screen_texture_size();
    screen_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB32, SDL_TEXTUREACCESS_TARGET, size.x, size.y);
    SDL_SetTextureScaleMode(screen_texture, screen_texture_scale_mode());
    native_output_rect_dirty = true;
    native_output_has_bars = true;
    native_output_width = 0;
    native_output_height = 0;
    screen_output_rect_dirty = true;
    screen_output_has_letterbox = true;
    screen_output_width = 0;
    screen_output_height = 0;
}

static void init_scalemode() {
    const char* raw_scalemode = Config_GetString(CFG_KEY_SCALEMODE);

    if (raw_scalemode == NULL) {
        return;
    }

    if (SDL_strcmp(raw_scalemode, "nearest") == 0) {
        scale_mode = SCALEMODE_NEAREST;
    } else if (SDL_strcmp(raw_scalemode, "native") == 0) {
        scale_mode = SCALEMODE_NATIVE;
    } else if (SDL_strcmp(raw_scalemode, "linear") == 0) {
        scale_mode = SCALEMODE_LINEAR;
    } else if (SDL_strcmp(raw_scalemode, "soft-linear") == 0) {
        scale_mode = SCALEMODE_SOFT_LINEAR;
    } else if (SDL_strcmp(raw_scalemode, "square-pixels") == 0) {
        scale_mode = SCALEMODE_SQUARE_PIXELS;
    } else if (SDL_strcmp(raw_scalemode, "integer") == 0) {
        scale_mode = SCALEMODE_INTEGER;
    }

#if defined(PORT_MISTER)
    // Soft-linear doubles internal target size and is too expensive on MiSTer CPU rendering path.
    if (scale_mode == SCALEMODE_SOFT_LINEAR) {
        scale_mode = SCALEMODE_NEAREST;
    }
#endif
}

int SDLApp_Init() {
    Config_Init();
    Keymap_Init();
    init_scalemode();

    SDL_SetAppMetadata(app_name, "0.1", NULL);
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_PREFER_LIBDECOR, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#if defined(PORT_MISTER)
    // On MiSTer we often run without a focused desktop window; keep controller input active.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Prefer /dev/input/js* interface on lightweight Linux userspace where evdev focus can be problematic.
    SDL_SetHint(SDL_HINT_JOYSTICK_LINUX_CLASSIC, "1");
    // Ensure console key events do not bleed into the underlying Linux VT while the game is running.
    SDL_SetHint(SDL_HINT_MUTE_CONSOLE_KEYBOARD, "1");
#endif
    apply_backend_hints();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("Couldn't initialize SDL video/gamepad: %s", SDL_GetError());
        backend_logf("SDL_Init video/gamepad failed: %s", SDL_GetError());
        return 1;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        const char* error_with_audio = SDL_GetError();
        SDL_Log("Couldn't initialize SDL audio: %s", error_with_audio);
        backend_logf("SDL_InitSubSystem audio failed: %s", error_with_audio);
        backend_logf("Continuing without SDL audio subsystem");
    }

    log_backend_diagnostics();

    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    if (Config_GetBool(CFG_KEY_FULLSCREEN)) {
        window_flags |= SDL_WINDOW_FULLSCREEN;
    }

    int window_width = Config_GetInt(CFG_KEY_WINDOW_WIDTH);

    if (window_width < window_min_width) {
        window_width = window_min_width;
    }

    int window_height = Config_GetInt(CFG_KEY_WINDOW_HEIGHT);

    if (window_height < window_min_height) {
        window_height = window_min_height;
    }

    if (!SDL_CreateWindowAndRenderer(app_name, window_width, window_height, window_flags, &window, &renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        backend_logf("SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        return 1;
    }

    const char* selected_video_driver = SDL_GetCurrentVideoDriver();
    const char* selected_renderer = SDL_GetRendererName(renderer);
    backend_logf("Selected video driver: %s", selected_video_driver != NULL ? selected_video_driver : "(null)");
    backend_logf("Selected renderer: %s", selected_renderer != NULL ? selected_renderer : "(null)");

#if defined(PORT_MISTER)
    fbdev_presenter_enabled = FBDevPresenter_Init();
    backend_logf("FBDEV presenter: %s", fbdev_presenter_enabled ? "enabled" : "disabled");

    if (fbdev_presenter_enabled && selected_video_driver != NULL && SDL_strcmp(selected_video_driver, "dummy") == 0) {
        const int fb_w = FBDevPresenter_GetWidth();
        const int fb_h = FBDevPresenter_GetHeight();

        if (fb_w > 0 && fb_h > 0) {
            SDL_SetWindowSize(window, fb_w, fb_h);
            backend_logf("Adjusted dummy window to framebuffer size: %dx%d", fb_w, fb_h);
        }

        use_fbdev_only_present = true;
    }
#endif

    if ((scale_mode == SCALEMODE_NATIVE) || (scale_mode == SCALEMODE_SQUARE_PIXELS)) {
        use_native_render_path = true;
        backend_logf("Native render path: enabled (scale-mode=%s)", scale_mode == SCALEMODE_NATIVE ? "native" : "square-pixels");
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Initialize rendering subsystems
    SDLMessageRenderer_Initialize(renderer);
    SDLGameRenderer_Init(renderer);

#if defined(DEBUG)
    SDLDebugText_Initialize(renderer);
#endif

    // Initialize screen texture
    create_screen_texture();

    // Initialize pads
    SDLPad_Init();

    return 0;
}

void SDLApp_Quit() {
    Config_Destroy();
    FBDevPresenter_Quit();
    SDL_DestroyTexture(screen_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    perf_capture_reset_storage();
    SDL_Quit();
}

static void set_screenshot_flag_if_needed(SDL_KeyboardEvent* event) {
    if ((event->key == SDLK_GRAVE) && event->down && !event->repeat) {
        should_save_screenshot = true;
    }
}

static void handle_fullscreen_toggle(SDL_KeyboardEvent* event) {
    const bool is_alt_enter = (event->key == SDLK_RETURN) && (event->mod & SDL_KMOD_ALT);
    const bool is_f11 = (event->key == SDLK_F11);
    const bool correct_key = (is_alt_enter || is_f11);

    if (!correct_key || !event->down || event->repeat) {
        return;
    }

    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);

    if (flags & SDL_WINDOW_FULLSCREEN) {
        SDL_SetWindowFullscreen(window, false);
    } else {
        SDL_SetWindowFullscreen(window, true);
    }

    native_output_rect_dirty = true;
    screen_output_rect_dirty = true;
}

static void handle_mouse_motion() {
    last_mouse_motion_time = SDL_GetTicks();
    SDL_ShowCursor();
}

static void hide_cursor_if_needed() {
    const Uint64 now = SDL_GetTicks();

    if ((last_mouse_motion_time > 0) && ((now - last_mouse_motion_time) > mouse_hide_delay_ms)) {
        SDL_HideCursor();
    }
}

bool SDLApp_PollEvents() {
    SDL_Event event;
    bool continue_running = true;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            SDLPad_HandleGamepadDeviceEvent(&event.gdevice);
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            set_screenshot_flag_if_needed(&event.key);
            handle_fullscreen_toggle(&event.key);
            break;

        case SDL_EVENT_MOUSE_MOTION:
            handle_mouse_motion();
            break;

        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            create_screen_texture();
            break;

        case SDL_EVENT_QUIT:
            continue_running = false;
            break;
        }
    }

    return continue_running;
}

void SDLApp_BeginFrame() {
    perf_frame_start_ns = SDL_GetTicksNS();
    perf_update_start_ns = perf_frame_start_ns;

    if (fbdev_presenter_enabled) {
        FBDevPresenter_BeginFrameStats(perf_capture_enabled && !perf_capture_completed);
    }

    SDLMessageRenderer_BeginFrame();
    SDLGameRenderer_BeginFrame(perf_capture_enabled && !perf_capture_completed);
}

static void center_rect(SDL_FRect* rect, int win_w, int win_h) {
    rect->x = (win_w - rect->w) / 2;
    rect->y = (win_h - rect->h) / 2;
}

static SDL_FRect fit_4_by_3_rect(int win_w, int win_h) {
    SDL_FRect rect;
    rect.w = win_w;
    rect.h = win_w / display_target_ratio;

    if (rect.h > win_h) {
        rect.h = win_h;
        rect.w = win_h * display_target_ratio;
    }

    center_rect(&rect, win_w, win_h);
    return rect;
}

static SDL_FRect fit_native_rect(int win_w, int win_h) {
    SDL_FRect rect;
    rect.w = native_game_width;
    rect.h = native_game_height;
    center_rect(&rect, win_w, win_h);
    return rect;
}

static SDL_FRect fit_integer_rect(int win_w, int win_h, int pixel_w, int pixel_h) {
    const int virtual_w = win_w / pixel_w;
    const int virtual_h = win_h / pixel_h;
    const int scale_w = virtual_w / native_game_width;
    const int scale_h = virtual_h / native_game_height;
    int scale = (scale_h < scale_w) ? scale_h : scale_w;

    // Better to show a cropped image than nothing at all
    if (scale < 1) {
        scale = 1;
    }

    SDL_FRect rect;
    rect.w = scale * native_game_width * pixel_w;
    rect.h = scale * native_game_height * pixel_h;
    center_rect(&rect, win_w, win_h);
    return rect;
}

static SDL_FRect get_letterbox_rect(int win_w, int win_h) {
    switch (scale_mode) {
    case SCALEMODE_NATIVE:
        return fit_native_rect(win_w, win_h);

    case SCALEMODE_NEAREST:
    case SCALEMODE_LINEAR:
    case SCALEMODE_SOFT_LINEAR:
        return fit_4_by_3_rect(win_w, win_h);

    case SCALEMODE_INTEGER:
        // In order to scale a 384x224 buffer to 4:3 we need to stretch the image vertically by 9 / 7
        return fit_integer_rect(win_w, win_h, 7, 9);

    case SCALEMODE_SQUARE_PIXELS:
        return fit_integer_rect(win_w, win_h, 1, 1);
    }

    return fit_4_by_3_rect(win_w, win_h);
}

static bool rect_has_letterbox(const SDL_FRect* rect, int target_w, int target_h) {
    return (rect->x > 0.0f) || (rect->y > 0.0f) || ((rect->x + rect->w) < (float)target_w) ||
           ((rect->y + rect->h) < (float)target_h);
}

#if defined(DEBUG)
static void note_frame_end_time() {
    frame_end_times[frame_end_times_index] = SDL_GetTicksNS();
    frame_end_times_index += 1;
    frame_end_times_index %= FRAME_END_TIMES_MAX;

    if (frame_end_times_index == 0) {
        frame_end_times_filled = true;
    }
}

static void update_fps() {
    if (!frame_end_times_filled) {
        return;
    }

    double total_frame_time_ms = 0;

    for (int i = 0; i < FRAME_END_TIMES_MAX - 1; i++) {
        const int cur = (frame_end_times_index + i) % FRAME_END_TIMES_MAX;
        const int next = (cur + 1) % FRAME_END_TIMES_MAX;
        total_frame_time_ms += (double)(frame_end_times[next] - frame_end_times[cur]) / 1e6;
    }

    double average_frame_time_ms = total_frame_time_ms / (FRAME_END_TIMES_MAX - 1);
    fps = 1000 / average_frame_time_ms;
}
#else
static void note_frame_end_time() {
}

static void update_fps() {
}
#endif

static bool get_native_output_size(int* out_w, int* out_h) {
#if defined(PORT_MISTER)
    if (use_fbdev_only_present && fbdev_presenter_enabled) {
        const int fb_w = FBDevPresenter_GetWidth();
        const int fb_h = FBDevPresenter_GetHeight();

        if ((fb_w > 0) && (fb_h > 0)) {
            *out_w = fb_w;
            *out_h = fb_h;
            return true;
        }

        return false;
    }
#endif

    return SDL_GetRenderOutputSize(renderer, out_w, out_h);
}

static void refresh_native_output_rect() {
    if (!use_native_render_path) {
        return;
    }

    int render_w = 0;
    int render_h = 0;
    if (!get_native_output_size(&render_w, &render_h)) {
        // Safe fallback: keep clearing until output size can be queried again.
        native_output_has_bars = true;
        return;
    }

    const bool output_size_changed = (render_w != native_output_width) || (render_h != native_output_height);
    if (!native_output_rect_dirty && !output_size_changed) {
        return;
    }

    native_output_width = render_w;
    native_output_height = render_h;
    native_output_rect = get_letterbox_rect(render_w, render_h);
    native_output_has_bars = rect_has_letterbox(&native_output_rect, render_w, render_h);
    native_output_rect_dirty = false;
}

static void refresh_screen_output_rect() {
    if (use_native_render_path || (screen_texture == NULL)) {
        return;
    }

    const bool output_size_changed =
        (screen_texture->w != screen_output_width) || (screen_texture->h != screen_output_height);
    if (!screen_output_rect_dirty && !output_size_changed) {
        return;
    }

    screen_output_width = screen_texture->w;
    screen_output_height = screen_texture->h;
    screen_output_rect = get_letterbox_rect(screen_output_width, screen_output_height);
    screen_output_has_letterbox = rect_has_letterbox(&screen_output_rect, screen_output_width, screen_output_height);
    screen_output_rect_dirty = false;
}

static void render_native_output_to_present_target(bool has_message_content, bool clear_bars) {
    SDL_SetRenderTarget(renderer, NULL);
    if (clear_bars) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); // black bars
        SDL_RenderClear(renderer);
    }

    SDL_RenderTexture(renderer, cps3_canvas, NULL, &native_output_rect);
    if (has_message_content) {
        SDL_RenderTexture(renderer, message_canvas, NULL, &native_output_rect);
    }
}

static void save_texture(SDL_Texture* texture, const char* filename) {
    SDL_SetRenderTarget(renderer, texture);
    const SDL_Surface* rendered_surface = SDL_RenderReadPixels(renderer, NULL);
    SDL_SaveBMP(rendered_surface, filename);
    SDL_DestroySurface(rendered_surface);
}

void SDLApp_EndFrame() {
    const Uint64 render_start_ns = SDL_GetTicksNS();
    const Uint64 update_ns = render_start_ns > perf_update_start_ns ? (render_start_ns - perf_update_start_ns) : 0;

    // Run sound processing
    ADX_ProcessTracks();

    // Render

    // This should come before SDLGameRenderer_RenderFrame,
    // because NetstatsRenderer uses the existing SFIII rendering pipeline.
#if defined(ENABLE_NETPLAY)
    NetplayScreen_Render();
    NetstatsRenderer_Render();
#endif
    SDLGameRenderer_RenderFrame();

    if (should_save_screenshot) {
        save_texture(cps3_canvas, "screenshot_cps3.bmp");
    }

    const bool has_message_content = SDLMessageRenderer_HasContent();
    const SDL_FRect* fbdev_readback_rect = NULL;
    SDL_FRect fbdev_readback_rect_value = { 0 };
    bool fbdev_present_current_target = false;
#if defined(DEBUG)
    const bool force_full_fbdev_readback = true;
#else
    const bool force_full_fbdev_readback = false;
#endif

    if (use_native_render_path) {
        refresh_native_output_rect();
        const bool use_fbdev_native_direct_target =
            fbdev_presenter_enabled && use_fbdev_only_present && !has_message_content && !force_full_fbdev_readback;
        const bool use_fbdev_native_readback_rect =
            fbdev_presenter_enabled && use_fbdev_only_present && native_output_has_bars && !force_full_fbdev_readback &&
            !use_fbdev_native_direct_target;
        if (use_fbdev_native_direct_target) {
            fbdev_present_current_target = true;
        } else if (use_fbdev_native_readback_rect) {
            fbdev_readback_rect_value = native_output_rect;
            fbdev_readback_rect = &fbdev_readback_rect_value;
        }

        if (!fbdev_present_current_target) {
            render_native_output_to_present_target(has_message_content, native_output_has_bars && !use_fbdev_native_readback_rect);
        }
    } else {
        SDL_SetRenderTarget(renderer, screen_texture);
        refresh_screen_output_rect();

        const SDL_FRect* dst_rect = &screen_output_rect;
        const bool has_letterbox = screen_output_has_letterbox;
        const bool use_fbdev_letterbox_readback_rect =
            fbdev_presenter_enabled && use_fbdev_only_present && has_letterbox && !force_full_fbdev_readback;
        if (use_fbdev_letterbox_readback_rect) {
            fbdev_readback_rect_value = *dst_rect;
            fbdev_readback_rect = &fbdev_readback_rect_value;
        }

        if (has_letterbox && (!use_fbdev_letterbox_readback_rect || should_save_screenshot)) {
            // Render window background only when bars are actually visible.
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); // black bars
            SDL_RenderClear(renderer);
        }

        // Render content
        SDL_RenderTexture(renderer, cps3_canvas, NULL, dst_rect);
        if (has_message_content) {
            SDL_RenderTexture(renderer, message_canvas, NULL, dst_rect);
        }

        if (!use_fbdev_only_present) {
            // Render screen texture to screen
            SDL_SetRenderTarget(renderer, NULL);
            SDL_RenderTexture(renderer, screen_texture, NULL, NULL);
        }
    }

    if (should_save_screenshot && screen_texture != NULL) {
        save_texture(screen_texture, "screenshot_screen.bmp");
    }

#if defined(DEBUG)
    // Render debug text
    SDLDebugText_Render();

    // Render metrics
    // int window_width;
    // SDL_GetRenderOutputSize(renderer, &window_width, NULL);
    // SDL_SetRenderDrawColor(renderer, 255, 255, 255, SDL_ALPHA_OPAQUE);
    // SDL_SetRenderScale(renderer, 2, 2);
    // SDL_RenderDebugTextFormat(renderer, (window_width / 2) - 88, 2, "FPS: %.3f", fps);
    // SDL_SetRenderScale(renderer, 1, 1);
#endif

    const Uint64 present_start_ns = SDL_GetTicksNS();

    if (fbdev_presenter_enabled) {
        if (fbdev_present_current_target) {
            SDL_SetRenderTarget(renderer, cps3_canvas);
            if (!FBDevPresenter_PresentCurrentTarget(renderer, &native_output_rect)) {
                backend_logf("FBDEV direct current-target present failed; falling back to composited readback");
                fbdev_present_current_target = false;
                if (native_output_has_bars) {
                    fbdev_readback_rect_value = native_output_rect;
                    fbdev_readback_rect = &fbdev_readback_rect_value;
                } else {
                    fbdev_readback_rect = NULL;
                }
                render_native_output_to_present_target(false, native_output_has_bars && (fbdev_readback_rect == NULL));
                FBDevPresenter_BeginFrameStats(perf_capture_enabled && !perf_capture_completed);
                FBDevPresenter_Present(renderer, fbdev_readback_rect);
            }
        } else {
            FBDevPresenter_Present(renderer, fbdev_readback_rect);
        }
    }

    if (!use_fbdev_only_present) {
        SDL_RenderPresent(renderer);
    }
    const Uint64 present_end_ns = SDL_GetTicksNS();
    const Uint64 render_ns = present_start_ns > render_start_ns ? (present_start_ns - render_start_ns) : 0;
    const Uint64 present_ns = present_end_ns > present_start_ns ? (present_end_ns - present_start_ns) : 0;
    const Uint64 frame_work_ns = present_end_ns > perf_frame_start_ns ? (present_end_ns - perf_frame_start_ns) : 0;
    FBDevPresenter_FrameStats presenter_stats = { 0 };
    if (fbdev_presenter_enabled) {
        FBDevPresenter_GetFrameStats(&presenter_stats);
    }
    const size_t copy_bytes = fbdev_presenter_enabled ? FBDevPresenter_GetFrameCopyBytes() : 0;
    const int dirty_tiles = SDLGameRenderer_GetDirtyTileCount();
    const int total_tiles = SDLGameRenderer_GetDirtyTileTotal();
    const double dirty_ratio = total_tiles > 0 ? (double)dirty_tiles / (double)total_tiles : 0.0;
    SDLGameRenderer_FrameStats render_stats = { 0 };
    SDLGameRenderer_GetFrameStats(&render_stats);
    const int presenter_tiles_total = fbdev_presenter_enabled ? FBDevPresenter_GetFrameTilesTotal() : 0;
    const int presenter_tiles_copied = fbdev_presenter_enabled ? FBDevPresenter_GetFrameTilesCopied() : 0;
    const bool full_copy_fallback = fbdev_presenter_enabled ? FBDevPresenter_UsedFullCopyFallback() : false;
    const double dirty_hit_rate =
        presenter_tiles_total > 0 ? 1.0 - ((double)presenter_tiles_copied / (double)presenter_tiles_total) : 0.0;

    // Cleanup
    SDLGameRenderer_EndFrame();
    should_save_screenshot = false;

    // Handle cursor hiding
    hide_cursor_if_needed();

    // Do frame pacing
    Uint64 now = SDL_GetTicksNS();

    if (frame_deadline == 0) {
        frame_deadline = now + target_frame_time_ns;
    }

    if (now < frame_deadline) {
        Uint64 sleep_time = frame_deadline - now;
        SDL_DelayNS(sleep_time);
        now = SDL_GetTicksNS();
    }

    frame_deadline += target_frame_time_ns;

    // If we fell behind by more than one frame, resync to avoid spiraling
    if (now > frame_deadline + target_frame_time_ns) {
        frame_deadline = now + target_frame_time_ns;
    }

    // Measure
    frame_counter += 1;
    note_frame_end_time();
    update_fps();

    if (perf_capture_enabled && !perf_capture_completed && perf_capture_recorded_frames < perf_capture_target_frames) {
        PerfFrameSample* sample = &perf_samples[perf_capture_recorded_frames];
        sample->frame_time_ms = (double)frame_work_ns / 1e6;
        sample->update_ms = (double)update_ns / 1e6;
        sample->render_ms = (double)render_ns / 1e6;
        sample->present_ms = (double)present_ns / 1e6;
        sample->present_readback_ms = (double)presenter_stats.readback_ns / 1e6;
        sample->present_convert_ms = (double)presenter_stats.convert_ns / 1e6;
        sample->present_copy_ms = (double)presenter_stats.copy_ns / 1e6;
        sample->present_clear_ms = (double)presenter_stats.clear_ns / 1e6;
        sample->copy_bytes = copy_bytes;
        sample->dirty_tiles = dirty_tiles;
        sample->dirty_ratio = dirty_ratio;
        sample->presenter_tiles_total = presenter_tiles_total;
        sample->presenter_tiles_copied = presenter_tiles_copied;
        sample->dirty_hit_rate = dirty_hit_rate;
        sample->full_copy_fallback = full_copy_fallback;
        sample->render_task_count = render_stats.render_task_count;
        sample->rect_copy_tasks = render_stats.rect_copy_tasks;
        sample->batch_runs = render_stats.batch_runs;
        sample->batched_task_count = render_stats.batched_task_count;
        sample->rect_texture_runs = render_stats.rect_texture_runs;
        sample->rect_texture_multi_runs = render_stats.rect_texture_multi_runs;
        sample->rect_texture_multi_run_tasks = render_stats.rect_texture_multi_run_tasks;
        sample->rect_texture_max_run = render_stats.rect_texture_max_run;
        sample->rect_texture_hstrip_runs = render_stats.rect_texture_hstrip_runs;
        sample->rect_texture_hstrip_tasks = render_stats.rect_texture_hstrip_tasks;
        sample->rect_texture_vstrip_runs = render_stats.rect_texture_vstrip_runs;
        sample->rect_texture_vstrip_tasks = render_stats.rect_texture_vstrip_tasks;
        sample->rect_texture_run_links = render_stats.rect_texture_run_links;
        sample->rect_texture_color_breaks = render_stats.rect_texture_color_breaks;
        sample->rect_texture_flip_breaks = render_stats.rect_texture_flip_breaks;
        sample->rect_texture_flipped_tasks = render_stats.rect_texture_flipped_tasks;
        sample->textured_geometry_tasks = render_stats.textured_geometry_tasks;
        sample->textured_geometry_rect_recovered_tasks = render_stats.textured_geometry_rect_recovered_tasks;
        sample->textured_geometry_fallback_tasks = render_stats.textured_geometry_fallback_tasks;
        sample->set_texture_calls = render_stats.set_texture_calls;
        sample->texture_binding_reuse_hits = render_stats.texture_binding_reuse_hits;
        sample->texture_cache_hits = render_stats.texture_cache_hits;
        sample->texture_cache_misses = render_stats.texture_cache_misses;
        sample->texture_creates = render_stats.texture_creates;
        sample->texture_unlock_calls = render_stats.texture_unlock_calls;
        sample->palette_unlock_calls = render_stats.palette_unlock_calls;
        sample->texture_cache_evictions = render_stats.texture_cache_evictions;
        sample->palette_cache_evictions = render_stats.palette_cache_evictions;
        sample->textures_destroy_queued = render_stats.textures_destroy_queued;
        sample->unknown_tasks = render_stats.unknown_tasks;
        sample->ppg_tasks = render_stats.ppg_tasks;
        sample->mtrans_tasks = render_stats.mtrans_tasks;
        sample->ui_direct_tasks = render_stats.ui_direct_tasks;
        sample->solid_tasks = render_stats.solid_tasks;
        sample->hybrid_candidate_tasks = render_stats.hybrid_candidate_tasks;
        sample->hybrid_candidate_pixels = render_stats.hybrid_candidate_pixels;
        sample->hybrid_fallback_tasks = render_stats.hybrid_fallback_tasks;
        sample->hybrid_fallback_pixels = render_stats.hybrid_fallback_pixels;
        sample->hybrid_reason_clip = render_stats.hybrid_reason_clip;
        sample->hybrid_reason_alpha = render_stats.hybrid_reason_alpha;
        sample->hybrid_reason_color_mod = render_stats.hybrid_reason_color_mod;
        sample->hybrid_reason_flip = render_stats.hybrid_reason_flip;
        sample->hybrid_reason_geometry = render_stats.hybrid_reason_geometry;
        sample->hybrid_reason_solid = render_stats.hybrid_reason_solid;
        sample->sort_strategy = render_stats.sort_strategy;
        sample->fbdev_path = presenter_stats.path;
        sample->readback_format = presenter_stats.readback_format;
        sample->readback_width = presenter_stats.readback_width;
        sample->readback_height = presenter_stats.readback_height;

        perf_update_ns_total += update_ns;
        perf_render_ns_total += render_ns;
        perf_present_ns_total += present_ns;
        perf_frame_work_ns_total += frame_work_ns;
        perf_present_readback_ns_total += presenter_stats.readback_ns;
        perf_present_convert_ns_total += presenter_stats.convert_ns;
        perf_present_copy_ns_total += presenter_stats.copy_ns;
        perf_present_clear_ns_total += presenter_stats.clear_ns;
        perf_copy_bytes_total += copy_bytes;
        perf_dirty_tiles_total += (Uint64)dirty_tiles;
        perf_render_task_count_total += (Uint64)render_stats.render_task_count;
        perf_rect_copy_tasks_total += (Uint64)render_stats.rect_copy_tasks;
        perf_batch_runs_total += (Uint64)render_stats.batch_runs;
        perf_batched_task_count_total += (Uint64)render_stats.batched_task_count;
        perf_rect_texture_runs_total += (Uint64)render_stats.rect_texture_runs;
        perf_rect_texture_multi_runs_total += (Uint64)render_stats.rect_texture_multi_runs;
        perf_rect_texture_multi_run_tasks_total += (Uint64)render_stats.rect_texture_multi_run_tasks;
        perf_rect_texture_max_run_total += (Uint64)render_stats.rect_texture_max_run;
        perf_rect_texture_hstrip_runs_total += (Uint64)render_stats.rect_texture_hstrip_runs;
        perf_rect_texture_hstrip_tasks_total += (Uint64)render_stats.rect_texture_hstrip_tasks;
        perf_rect_texture_vstrip_runs_total += (Uint64)render_stats.rect_texture_vstrip_runs;
        perf_rect_texture_vstrip_tasks_total += (Uint64)render_stats.rect_texture_vstrip_tasks;
        perf_rect_texture_run_links_total += (Uint64)render_stats.rect_texture_run_links;
        perf_rect_texture_color_breaks_total += (Uint64)render_stats.rect_texture_color_breaks;
        perf_rect_texture_flip_breaks_total += (Uint64)render_stats.rect_texture_flip_breaks;
        perf_rect_texture_flipped_tasks_total += (Uint64)render_stats.rect_texture_flipped_tasks;
        perf_textured_geometry_tasks_total += (Uint64)render_stats.textured_geometry_tasks;
        perf_textured_geometry_rect_recovered_tasks_total +=
            (Uint64)render_stats.textured_geometry_rect_recovered_tasks;
        perf_textured_geometry_fallback_tasks_total += (Uint64)render_stats.textured_geometry_fallback_tasks;
        perf_set_texture_calls_total += (Uint64)render_stats.set_texture_calls;
        perf_texture_binding_reuse_hits_total += (Uint64)render_stats.texture_binding_reuse_hits;
        perf_texture_cache_hits_total += (Uint64)render_stats.texture_cache_hits;
        perf_texture_cache_misses_total += (Uint64)render_stats.texture_cache_misses;
        perf_texture_creates_total += (Uint64)render_stats.texture_creates;
        perf_texture_unlock_calls_total += (Uint64)render_stats.texture_unlock_calls;
        perf_palette_unlock_calls_total += (Uint64)render_stats.palette_unlock_calls;
        perf_texture_cache_evictions_total += (Uint64)render_stats.texture_cache_evictions;
        perf_palette_cache_evictions_total += (Uint64)render_stats.palette_cache_evictions;
        perf_textures_destroy_queued_total += (Uint64)render_stats.textures_destroy_queued;
        perf_unknown_tasks_total += (Uint64)render_stats.unknown_tasks;
        perf_ppg_tasks_total += (Uint64)render_stats.ppg_tasks;
        perf_mtrans_tasks_total += (Uint64)render_stats.mtrans_tasks;
        perf_ui_direct_tasks_total += (Uint64)render_stats.ui_direct_tasks;
        perf_solid_tasks_total += (Uint64)render_stats.solid_tasks;
        perf_hybrid_candidate_tasks_total += (Uint64)render_stats.hybrid_candidate_tasks;
        perf_hybrid_candidate_pixels_total += render_stats.hybrid_candidate_pixels;
        perf_hybrid_fallback_tasks_total += (Uint64)render_stats.hybrid_fallback_tasks;
        perf_hybrid_fallback_pixels_total += render_stats.hybrid_fallback_pixels;
        perf_hybrid_reason_clip_total += (Uint64)render_stats.hybrid_reason_clip;
        perf_hybrid_reason_alpha_total += (Uint64)render_stats.hybrid_reason_alpha;
        perf_hybrid_reason_color_mod_total += (Uint64)render_stats.hybrid_reason_color_mod;
        perf_hybrid_reason_flip_total += (Uint64)render_stats.hybrid_reason_flip;
        perf_hybrid_reason_geometry_total += (Uint64)render_stats.hybrid_reason_geometry;
        perf_hybrid_reason_solid_total += (Uint64)render_stats.hybrid_reason_solid;
        perf_dirty_hit_rate_total += dirty_hit_rate;
        if (full_copy_fallback) {
            perf_full_copy_fallback_frames += 1;
        }
        if ((render_stats.sort_strategy >= SDL_GAME_RENDERER_SORT_NONE) &&
            (render_stats.sort_strategy <= SDL_GAME_RENDERER_SORT_QSORT)) {
            perf_sort_strategy_frames[render_stats.sort_strategy] += 1;
        }
        if ((presenter_stats.path >= FBDEV_PRESENTER_PATH_NONE) && (presenter_stats.path < FBDEV_PRESENTER_PATH_COUNT)) {
            perf_fbdev_path_frames[presenter_stats.path] += 1;
        }
        perf_capture_recorded_frames += 1;

        if ((perf_capture_recorded_frames % 60) == 0 || (perf_capture_recorded_frames == perf_capture_target_frames)) {
            backend_logf("PERF frame=%d frame_time_ms=%.3f update_ms=%.3f render_ms=%.3f present_ms=%.3f present_readback_ms=%.3f present_convert_ms=%.3f present_copy_ms=%.3f present_clear_ms=%.3f present_path=%s readback_format=%s readback_size=%dx%d copy_bytes=%llu dirty_tiles=%d dirty_ratio=%.4f dirty_hit_rate=%.4f full_copy_fallback=%s render_tasks=%d rect_tasks=%d batch_runs=%d rect_runs=%d rect_multi_runs=%d rect_multi_run_tasks=%d rect_max_run=%d rect_hstrip_runs=%d rect_hstrip_tasks=%d rect_vstrip_runs=%d rect_vstrip_tasks=%d rect_run_links=%d rect_color_breaks=%d rect_flip_breaks=%d rect_flipped_tasks=%d textured_geometry_tasks=%d textured_geometry_recovered=%d textured_geometry_fallback=%d set_texture_calls=%d binding_reuse=%d cache_hits=%d cache_misses=%d cache_creates=%d texture_unlocks=%d palette_unlocks=%d texture_evictions=%d palette_evictions=%d destroy_queue=%d source_ppg=%d source_mtrans=%d source_ui=%d source_solid=%d source_unknown=%d hybrid_candidate_tasks=%d hybrid_candidate_pixels=%llu hybrid_fallback_tasks=%d hybrid_fallback_pixels=%llu hybrid_reason_clip=%d hybrid_reason_alpha=%d hybrid_reason_color_mod=%d hybrid_reason_flip=%d hybrid_reason_geometry=%d hybrid_reason_solid=%d sort=%s",
                         perf_capture_recorded_frames,
                         sample->frame_time_ms,
                         sample->update_ms,
                         sample->render_ms,
                         sample->present_ms,
                         sample->present_readback_ms,
                         sample->present_convert_ms,
                         sample->present_copy_ms,
                         sample->present_clear_ms,
                         fbdev_present_path_name(sample->fbdev_path),
                         pixel_format_name_safe(sample->readback_format),
                         sample->readback_width,
                         sample->readback_height,
                         (unsigned long long)sample->copy_bytes,
                         sample->dirty_tiles,
                         sample->dirty_ratio,
                         sample->dirty_hit_rate,
                         sample->full_copy_fallback ? "true" : "false",
                         sample->render_task_count,
                         sample->rect_copy_tasks,
                         sample->batch_runs,
                         sample->rect_texture_runs,
                         sample->rect_texture_multi_runs,
                         sample->rect_texture_multi_run_tasks,
                         sample->rect_texture_max_run,
                         sample->rect_texture_hstrip_runs,
                         sample->rect_texture_hstrip_tasks,
                         sample->rect_texture_vstrip_runs,
                         sample->rect_texture_vstrip_tasks,
                         sample->rect_texture_run_links,
                         sample->rect_texture_color_breaks,
                         sample->rect_texture_flip_breaks,
                         sample->rect_texture_flipped_tasks,
                         sample->textured_geometry_tasks,
                         sample->textured_geometry_rect_recovered_tasks,
                         sample->textured_geometry_fallback_tasks,
                         sample->set_texture_calls,
                         sample->texture_binding_reuse_hits,
                         sample->texture_cache_hits,
                         sample->texture_cache_misses,
                         sample->texture_creates,
                         sample->texture_unlock_calls,
                         sample->palette_unlock_calls,
                         sample->texture_cache_evictions,
                         sample->palette_cache_evictions,
                         sample->textures_destroy_queued,
                         sample->ppg_tasks,
                         sample->mtrans_tasks,
                         sample->ui_direct_tasks,
                         sample->solid_tasks,
                         sample->unknown_tasks,
                         sample->hybrid_candidate_tasks,
                         (unsigned long long)sample->hybrid_candidate_pixels,
                         sample->hybrid_fallback_tasks,
                         (unsigned long long)sample->hybrid_fallback_pixels,
                         sample->hybrid_reason_clip,
                         sample->hybrid_reason_alpha,
                         sample->hybrid_reason_color_mod,
                         sample->hybrid_reason_flip,
                         sample->hybrid_reason_geometry,
                         sample->hybrid_reason_solid,
                         render_sort_strategy_name(sample->sort_strategy));
        }

        if (perf_capture_recorded_frames >= perf_capture_target_frames) {
            perf_capture_write_summary();
            perf_capture_completed = true;
            SDLApp_Exit();
        }
    }
}

void SDLApp_Exit() {
    SDL_Event quit_event;
    quit_event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit_event);
}
