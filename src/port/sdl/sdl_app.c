#include "port/sdl/sdl_app.h"
#include "common.h"
#include "main.h"
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
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/pls03.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"

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
static bool show_fps_overlay = false;
static Uint64 fps_overlay_window_start_ns = 0;
static Uint32 fps_overlay_window_frames = 0;
static int fps_overlay_value = 0;
static char fps_overlay_label[16] = "";

static Uint64 frame_deadline = 0;
static Uint64 frame_counter = 0;

static bool should_save_screenshot = false;
static Uint64 last_mouse_motion_time = 0;
static const int mouse_hide_delay_ms = 2000; // 2 seconds
static bool fbdev_presenter_enabled = false;
static bool use_fbdev_only_present = false;
static bool use_native_render_path = false;
static bool software_frame_mode_enabled = false;
#if ENABLE_PERF_TELEMETRY
static bool perf_capture_enabled = false;
static bool perf_capture_completed = false;
static bool perf_capture_basic_mode = false;
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
static Uint64 perf_texture_cache_miss_dirty_texture_same_frame_total = 0;
static Uint64 perf_texture_cache_miss_dirty_texture_carried_total = 0;
static Uint64 perf_texture_cache_miss_dirty_palette_same_frame_total = 0;
static Uint64 perf_texture_cache_miss_dirty_palette_carried_total = 0;
static Uint64 perf_texture_cache_miss_cold_total = 0;
static Uint64 perf_texture_creates_total = 0;
static Uint64 perf_texture_unlock_calls_total = 0;
static Uint64 perf_palette_unlock_calls_total = 0;
static Uint64 perf_palette_unlock_changed_calls_total = 0;
static Uint64 perf_palette_unlock_unchanged_calls_total = 0;
static Uint64 perf_texture_unlock_dirty_surface_variants_total = 0;
static Uint64 perf_texture_unlock_dirty_surface_variants_max_total = 0;
static Uint64 perf_palette_unlock_dirty_surface_variants_total = 0;
static Uint64 perf_palette_unlock_dirty_surface_variants_max_total = 0;
static Uint64 perf_texture_unlock_locality_index8_tracked_total = 0;
static Uint64 perf_texture_unlock_locality_index8_baseline_skips_total = 0;
static Uint64 perf_texture_unlock_locality_index8_non_index8_skips_total = 0;
static Uint64 perf_texture_unlock_locality_index8_source_pixels_total = 0;
static Uint64 perf_texture_unlock_locality_index8_changed_pixels_total = 0;
static Uint64 perf_texture_unlock_locality_index8_changed_rows_total = 0;
static Uint64 perf_texture_unlock_locality_index8_changed_bbox_pixels_total = 0;
static Uint64 perf_texture_unlock_invalidation_ns_total = 0;
static Uint64 perf_palette_unlock_invalidation_ns_total = 0;
static Uint64 perf_texture_cache_evictions_total = 0;
static Uint64 perf_palette_cache_evictions_total = 0;
static Uint64 perf_software_surface_cache_hits_total = 0;
static Uint64 perf_software_surface_cache_creates_total = 0;
static Uint64 perf_software_surface_cache_refresh_attempts_total = 0;
static Uint64 perf_software_surface_cache_refresh_unique_bindings_total = 0;
static Uint64 perf_software_surface_cache_refresh_repeat_binding_attempts_total = 0;
static Uint64 perf_software_surface_cache_refresh_unique_texture_handles_total = 0;
static Uint64 perf_software_surface_cache_refresh_texture_handle_fanout_max_total = 0;
static Uint64 perf_software_surface_cache_refresh_failures_total = 0;
static Uint64 perf_software_surface_cache_refresh_ns_total = 0;
static Uint64 perf_software_surface_cache_refresh_palette_set_calls_total = 0;
static Uint64 perf_software_surface_cache_refresh_palette_set_ns_total = 0;
static Uint64 perf_software_surface_cache_refresh_blit_calls_total = 0;
static Uint64 perf_software_surface_cache_refresh_blit_ns_total = 0;
static Uint64 perf_software_surface_cache_create_dirty_texture_same_frame_total = 0;
static Uint64 perf_software_surface_cache_create_dirty_texture_carried_total = 0;
static Uint64 perf_software_surface_cache_create_dirty_palette_same_frame_total = 0;
static Uint64 perf_software_surface_cache_create_dirty_palette_carried_total = 0;
static Uint64 perf_software_surface_cache_create_cold_total = 0;
static Uint64 perf_software_surface_cache_texture_evictions_total = 0;
static Uint64 perf_software_surface_cache_palette_evictions_total = 0;
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
static Uint64 perf_super_art_stock_available_frames[2] = { 0 };
static int perf_super_art_stock_available_first_frame[2] = { -1, -1 };
static int perf_super_art_stock_max_reached[2] = { 0, 0 };
static int perf_super_art_stock_capacity[2] = { 0, 0 };
static int perf_super_art_gauge_max_value[2] = { 0, 0 };
static int perf_super_art_gauge_capacity[2] = { 0, 0 };
static Uint64 perf_super_art_ready_frames[2] = { 0 };
static int perf_super_art_ready_first_frame[2] = { -1, -1 };
enum {
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_0 = 0,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_1 = 1,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_2 = 2,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_3 = 3,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_4 = 4,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_OTHER = 5,
    PERF_SUPER_ART_READY_ROUTINE1_BUCKET_COUNT = 6,
};
static Uint64 perf_super_art_ready_routine1_frames[2][PERF_SUPER_ART_READY_ROUTINE1_BUCKET_COUNT] = { { 0 } };
static int perf_super_art_ready_first_routine[2][3] = { { -1, -1, -1 }, { -1, -1, -1 } };
static int perf_super_art_ready_last_routine[2][3] = { { -1, -1, -1 }, { -1, -1, -1 } };
static Uint64 perf_super_art_active_frames[2] = { 0 };
static int perf_super_art_active_first_frame[2] = { -1, -1 };
static Uint64 perf_metamorphose_active_frames[2] = { 0 };
static int perf_metamorphose_active_first_frame[2] = { -1, -1 };
static int perf_capture_start_g_no[4] = { 0, 0, 0, 0 };
static int perf_capture_start_e_no[4] = { 0, 0, 0, 0 };
static int perf_capture_start_menu_task_condition = 0;
static int perf_capture_start_menu_task_r_no[4] = { 0, 0, 0, 0 };
static int perf_capture_start_break_into = 0;
static int perf_capture_start_hnc_num = 0;
static int perf_capture_start_exec_wipe = 0;
static int perf_capture_start_active_wipe_type = -1;
static int perf_capture_start_wipe_limit = 0;
static Uint64 perf_break_into_frames_total = 0;
static int perf_break_into_first_frame = -1;
static Uint64 perf_hnc_active_frames_total = 0;
static int perf_hnc_active_first_frame = -1;
static int perf_hnc_max_num = 0;
static Uint64 perf_wipe_type1_active_frames_total = 0;
static int perf_wipe_type1_active_first_frame = -1;
static int perf_wipe_type1_max_limit = 0;
static Uint64 perf_software_frame_mode_enabled_frames = 0;
static Uint64 perf_software_frame_surface_ready_frames = 0;
static Uint64 perf_software_frame_owned_frames = 0;
static Uint64 perf_software_frame_direct_present_frames = 0;
static Uint64 perf_software_frame_uploaded_frames = 0;
static Uint64 perf_software_frame_fallback_frames = 0;
static Uint64 perf_software_frame_candidate_tasks_total = 0;
static Uint64 perf_software_frame_candidate_pixels_total = 0;
static Uint64 perf_software_frame_fallback_tasks_total = 0;
static Uint64 perf_software_frame_fallback_pixels_total = 0;
static Uint64 perf_software_frame_fast_exact_tasks_total = 0;
static Uint64 perf_software_frame_fast_exact_pixels_total = 0;
static Uint64 perf_software_frame_fast_exact_clipped_tasks_total = 0;
static Uint64 perf_software_frame_fast_exact_flipped_tasks_total = 0;
static Uint64 perf_software_frame_fast_exact_color_mod_tasks_total = 0;
static Uint64 perf_software_frame_fast_exact_color_mod_pixels_total = 0;
static Uint64 perf_software_frame_fast_scaled_tasks_total = 0;
static Uint64 perf_software_frame_fast_scaled_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_tasks_total = 0;
static Uint64 perf_software_frame_fast_non_integer_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_lookup_entries_total = 0;
static Uint64 perf_software_frame_fast_non_integer_alpha_only_tasks_total = 0;
static Uint64 perf_software_frame_fast_non_integer_alpha_only_pixels_total = 0;
static Uint64 perf_software_frame_fast_non_integer_rgb_mod_tasks_total = 0;
static Uint64 perf_software_frame_fast_non_integer_rgb_mod_pixels_total = 0;
static Uint64 perf_software_frame_generic_textured_tasks_total = 0;
static Uint64 perf_software_frame_generic_textured_pixels_total = 0;
static Uint64 perf_software_frame_generic_textured_alpha_only_tasks_total = 0;
static Uint64 perf_software_frame_generic_textured_alpha_only_pixels_total = 0;
static Uint64 perf_software_frame_generic_textured_rgb_mod_tasks_total = 0;
static Uint64 perf_software_frame_generic_textured_rgb_mod_pixels_total = 0;
static Uint64 perf_software_frame_fast_miss_color_mod_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_lookup_entries_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_ge_256_tasks_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_ge_256_pixels_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_ge_256_lookup_entries_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_ge_1024_tasks_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_ge_1024_pixels_total = 0;
static Uint64 perf_software_frame_fast_miss_non_integer_max_pixels_total = 0;
static Uint64 perf_software_frame_fast_miss_scaled_total = 0;
static Uint64 perf_software_frame_fast_miss_unsupported_flip_total = 0;
static Uint64 perf_software_frame_fast_miss_source_bounds_total = 0;
static Uint64 perf_software_frame_reason_alpha_total = 0;
static Uint64 perf_software_frame_reason_color_mod_total = 0;
static Uint64 perf_software_frame_reason_geometry_total = 0;
static Uint64 perf_software_frame_reason_solid_total = 0;
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
    double texture_unlock_invalidation_ms;
    double palette_unlock_invalidation_ms;
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
    double software_surface_cache_refresh_ms;
    int software_surface_cache_refresh_palette_set_calls;
    double software_surface_cache_refresh_palette_set_ms;
    int software_surface_cache_refresh_blit_calls;
    double software_surface_cache_refresh_blit_ms;
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
#endif

#if defined(PORT_MISTER)
static const char* legacy_mister_video_driver_order = "kmsdrm,offscreen,dummy";
static const char* recommended_mister_video_driver_order = "evdev,dummy,offscreen";
#endif

#if ENABLE_PERF_TELEMETRY
static void perf_capture_reset_storage(void);
static void perf_capture_write_summary(void);
#endif
static const char* software_frame_mode_name(void);

#if ENABLE_PERF_TELEMETRY
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

static const char* texture_logical_source_kind_name(
    SDLGameRenderer_TextureLogicalSourceKind source_kind) {
    switch (source_kind) {
    case SDL_GAME_RENDERER_TEXTURE_LOGICAL_SOURCE_PPG_SEQS:
        return "ppg-seqs";
    case SDL_GAME_RENDERER_TEXTURE_LOGICAL_SOURCE_PPG_CHUNK:
        return "ppg-chunk";
    case SDL_GAME_RENDERER_TEXTURE_LOGICAL_SOURCE_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char* software_frame_raster_bucket_name(SDLGameRenderer_PerfCaptureRasterBucket bucket) {
    switch (bucket) {
    case SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_FAST_EXACT:
        return "fast_exact";
    case SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_FAST_SCALED:
        return "fast_scaled";
    case SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_FAST_NON_INTEGER:
        return "fast_non_integer";
    case SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_GENERIC_TEXTURED:
        return "generic_textured";
    case SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_SOLID:
        return "solid";
    case SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT:
    default:
        return "unknown";
    }
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
#endif

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

#if ENABLE_PERF_TELEMETRY
static int get_perf_capture_super_art_active(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->ok == -1 ? 1 : 0;
}

static int get_perf_capture_super_art_stock(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->store;
}

static int get_perf_capture_super_art_stock_capacity(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->store_max;
}

static int get_perf_capture_super_art_ready(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->ok == 1 ? 1 : 0;
}

static int get_perf_capture_super_art_gauge_value(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->gauge.s.h;
}

static int get_perf_capture_super_art_gauge_capacity(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || plw[player].sa == NULL) {
        return 0;
    }

    return plw[player].sa->gauge_len;
}

static int get_perf_capture_metamorphose_active(int player) {
    if (!mpp_w.inGame || player < 0 || player >= 2) {
        return 0;
    }

    return plw[player].metamorphose != 0 ? 1 : 0;
}

static int get_perf_capture_player_routine_component(int player, int component) {
    if (!mpp_w.inGame || player < 0 || player >= 2 || component < 1 || component > 3) {
        return -1;
    }

    return plw[player].wu.routine_no[component];
}

static int classify_perf_capture_ready_routine1_bucket(int routine1) {
    if (routine1 >= PERF_SUPER_ART_READY_ROUTINE1_BUCKET_0 && routine1 <= PERF_SUPER_ART_READY_ROUTINE1_BUCKET_4) {
        return routine1;
    }

    return PERF_SUPER_ART_READY_ROUTINE1_BUCKET_OTHER;
}

static void snapshot_perf_capture_transition_start_state(void) {
    const struct _TASK* menu_task = &task[TASK_MENU];

    for (int i = 0; i < 4; i++) {
        perf_capture_start_g_no[i] = G_No[i];
        perf_capture_start_e_no[i] = E_No[i];
        perf_capture_start_menu_task_r_no[i] = menu_task->r_no[i];
    }

    perf_capture_start_menu_task_condition = menu_task->condition;
    perf_capture_start_break_into = Break_Into;
    perf_capture_start_hnc_num = Hnc_Num;
    perf_capture_start_exec_wipe = Exec_Wipe;
    perf_capture_start_active_wipe_type = Active_Wipe_Type;
    perf_capture_start_wipe_limit = WipeLimit;
}

static void note_perf_capture_transition_state(int frame_index) {
    if (Break_Into != 0) {
        perf_break_into_frames_total += 1;
        if (perf_break_into_first_frame < 0) {
            perf_break_into_first_frame = frame_index;
        }
    }

    if (Hnc_Num > 0) {
        perf_hnc_active_frames_total += 1;
        if (perf_hnc_active_first_frame < 0) {
            perf_hnc_active_first_frame = frame_index;
        }
    }

    if (Hnc_Num > perf_hnc_max_num) {
        perf_hnc_max_num = Hnc_Num;
    }

    if (Exec_Wipe != 0 && Active_Wipe_Type == 1 && WipeLimit < 8) {
        perf_wipe_type1_active_frames_total += 1;
        if (perf_wipe_type1_active_first_frame < 0) {
            perf_wipe_type1_active_first_frame = frame_index;
        }
        if (WipeLimit > perf_wipe_type1_max_limit) {
            perf_wipe_type1_max_limit = WipeLimit;
        }
    }
}

static void note_perf_capture_test_state(int frame_index) {
    note_perf_capture_transition_state(frame_index);

    for (int player = 0; player < 2; player++) {
        const int super_art_stock = get_perf_capture_super_art_stock(player);
        const int super_art_stock_capacity = get_perf_capture_super_art_stock_capacity(player);
        const int super_art_gauge_value = get_perf_capture_super_art_gauge_value(player);
        const int super_art_gauge_capacity = get_perf_capture_super_art_gauge_capacity(player);

        if (super_art_stock > 0) {
            perf_super_art_stock_available_frames[player] += 1;
            if (perf_super_art_stock_available_first_frame[player] < 0) {
                perf_super_art_stock_available_first_frame[player] = frame_index;
            }
        }

        if (super_art_stock > perf_super_art_stock_max_reached[player]) {
            perf_super_art_stock_max_reached[player] = super_art_stock;
        }
        if (super_art_stock_capacity > perf_super_art_stock_capacity[player]) {
            perf_super_art_stock_capacity[player] = super_art_stock_capacity;
        }
        if (super_art_gauge_value > perf_super_art_gauge_max_value[player]) {
            perf_super_art_gauge_max_value[player] = super_art_gauge_value;
        }
        if (super_art_gauge_capacity > perf_super_art_gauge_capacity[player]) {
            perf_super_art_gauge_capacity[player] = super_art_gauge_capacity;
        }

        if (get_perf_capture_super_art_ready(player)) {
            const int routine1 = get_perf_capture_player_routine_component(player, 1);
            const int routine2 = get_perf_capture_player_routine_component(player, 2);
            const int routine3 = get_perf_capture_player_routine_component(player, 3);

            perf_super_art_ready_frames[player] += 1;
            perf_super_art_ready_routine1_frames[player][classify_perf_capture_ready_routine1_bucket(routine1)] += 1;
            if (perf_super_art_ready_first_frame[player] < 0) {
                perf_super_art_ready_first_frame[player] = frame_index;
                perf_super_art_ready_first_routine[player][0] = routine1;
                perf_super_art_ready_first_routine[player][1] = routine2;
                perf_super_art_ready_first_routine[player][2] = routine3;
            }
            perf_super_art_ready_last_routine[player][0] = routine1;
            perf_super_art_ready_last_routine[player][1] = routine2;
            perf_super_art_ready_last_routine[player][2] = routine3;
        }

        if (get_perf_capture_super_art_active(player)) {
            perf_super_art_active_frames[player] += 1;
            if (perf_super_art_active_first_frame[player] < 0) {
                perf_super_art_active_first_frame[player] = frame_index;
            }
        }

        if (get_perf_capture_metamorphose_active(player)) {
            perf_metamorphose_active_frames[player] += 1;
            if (perf_metamorphose_active_first_frame[player] < 0) {
                perf_metamorphose_active_first_frame[player] = frame_index;
            }
        }
    }
}

bool SDLApp_RunSoftwareFrameParityCheck(void) {
    return SDLGameRenderer_RunSoftwareFrameParityCheck();
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
    perf_capture_basic_mode = false;
    SDLGameRenderer_SetPerfCaptureLogicalIdentityEnabled(false);
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
    perf_texture_cache_miss_dirty_texture_same_frame_total = 0;
    perf_texture_cache_miss_dirty_texture_carried_total = 0;
    perf_texture_cache_miss_dirty_palette_same_frame_total = 0;
    perf_texture_cache_miss_dirty_palette_carried_total = 0;
    perf_texture_cache_miss_cold_total = 0;
    perf_texture_creates_total = 0;
    perf_texture_unlock_calls_total = 0;
    perf_palette_unlock_calls_total = 0;
    perf_palette_unlock_changed_calls_total = 0;
    perf_palette_unlock_unchanged_calls_total = 0;
    perf_texture_unlock_dirty_surface_variants_total = 0;
    perf_texture_unlock_dirty_surface_variants_max_total = 0;
    perf_palette_unlock_dirty_surface_variants_total = 0;
    perf_palette_unlock_dirty_surface_variants_max_total = 0;
    perf_texture_unlock_locality_index8_tracked_total = 0;
    perf_texture_unlock_locality_index8_baseline_skips_total = 0;
    perf_texture_unlock_locality_index8_non_index8_skips_total = 0;
    perf_texture_unlock_locality_index8_source_pixels_total = 0;
    perf_texture_unlock_locality_index8_changed_pixels_total = 0;
    perf_texture_unlock_locality_index8_changed_rows_total = 0;
    perf_texture_unlock_locality_index8_changed_bbox_pixels_total = 0;
    perf_texture_unlock_invalidation_ns_total = 0;
    perf_palette_unlock_invalidation_ns_total = 0;
    perf_texture_cache_evictions_total = 0;
    perf_palette_cache_evictions_total = 0;
    perf_software_surface_cache_hits_total = 0;
    perf_software_surface_cache_creates_total = 0;
    perf_software_surface_cache_refresh_attempts_total = 0;
    perf_software_surface_cache_refresh_unique_bindings_total = 0;
    perf_software_surface_cache_refresh_repeat_binding_attempts_total = 0;
    perf_software_surface_cache_refresh_unique_texture_handles_total = 0;
    perf_software_surface_cache_refresh_texture_handle_fanout_max_total = 0;
    perf_software_surface_cache_refresh_failures_total = 0;
    perf_software_surface_cache_refresh_ns_total = 0;
    perf_software_surface_cache_refresh_palette_set_calls_total = 0;
    perf_software_surface_cache_refresh_palette_set_ns_total = 0;
    perf_software_surface_cache_refresh_blit_calls_total = 0;
    perf_software_surface_cache_refresh_blit_ns_total = 0;
    perf_software_surface_cache_create_dirty_texture_same_frame_total = 0;
    perf_software_surface_cache_create_dirty_texture_carried_total = 0;
    perf_software_surface_cache_create_dirty_palette_same_frame_total = 0;
    perf_software_surface_cache_create_dirty_palette_carried_total = 0;
    perf_software_surface_cache_create_cold_total = 0;
    perf_software_surface_cache_texture_evictions_total = 0;
    perf_software_surface_cache_palette_evictions_total = 0;
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
    for (int player = 0; player < 2; player++) {
        perf_super_art_stock_available_frames[player] = 0;
        perf_super_art_stock_available_first_frame[player] = -1;
        perf_super_art_stock_max_reached[player] = 0;
        perf_super_art_stock_capacity[player] = 0;
        perf_super_art_gauge_max_value[player] = 0;
        perf_super_art_gauge_capacity[player] = 0;
        perf_super_art_ready_frames[player] = 0;
        perf_super_art_ready_first_frame[player] = -1;
        SDL_memset(perf_super_art_ready_routine1_frames[player], 0, sizeof(perf_super_art_ready_routine1_frames[player]));
        perf_super_art_ready_first_routine[player][0] = -1;
        perf_super_art_ready_first_routine[player][1] = -1;
        perf_super_art_ready_first_routine[player][2] = -1;
        perf_super_art_ready_last_routine[player][0] = -1;
        perf_super_art_ready_last_routine[player][1] = -1;
        perf_super_art_ready_last_routine[player][2] = -1;
        perf_super_art_active_frames[player] = 0;
        perf_super_art_active_first_frame[player] = -1;
        perf_metamorphose_active_frames[player] = 0;
        perf_metamorphose_active_first_frame[player] = -1;
    }
    SDL_zero(perf_capture_start_g_no);
    SDL_zero(perf_capture_start_e_no);
    perf_capture_start_menu_task_condition = 0;
    SDL_zero(perf_capture_start_menu_task_r_no);
    perf_capture_start_break_into = 0;
    perf_capture_start_hnc_num = 0;
    perf_capture_start_exec_wipe = 0;
    perf_capture_start_active_wipe_type = -1;
    perf_capture_start_wipe_limit = 0;
    perf_break_into_frames_total = 0;
    perf_break_into_first_frame = -1;
    perf_hnc_active_frames_total = 0;
    perf_hnc_active_first_frame = -1;
    perf_hnc_max_num = 0;
    perf_wipe_type1_active_frames_total = 0;
    perf_wipe_type1_active_first_frame = -1;
    perf_wipe_type1_max_limit = 0;
    perf_software_frame_mode_enabled_frames = 0;
    perf_software_frame_surface_ready_frames = 0;
    perf_software_frame_owned_frames = 0;
    perf_software_frame_direct_present_frames = 0;
    perf_software_frame_uploaded_frames = 0;
    perf_software_frame_fallback_frames = 0;
    perf_software_frame_candidate_tasks_total = 0;
    perf_software_frame_candidate_pixels_total = 0;
    perf_software_frame_fallback_tasks_total = 0;
    perf_software_frame_fallback_pixels_total = 0;
    perf_software_frame_fast_exact_tasks_total = 0;
    perf_software_frame_fast_exact_pixels_total = 0;
    perf_software_frame_fast_exact_clipped_tasks_total = 0;
    perf_software_frame_fast_exact_flipped_tasks_total = 0;
    perf_software_frame_fast_exact_color_mod_tasks_total = 0;
    perf_software_frame_fast_exact_color_mod_pixels_total = 0;
    perf_software_frame_fast_scaled_tasks_total = 0;
    perf_software_frame_fast_scaled_pixels_total = 0;
    perf_software_frame_fast_non_integer_tasks_total = 0;
    perf_software_frame_fast_non_integer_pixels_total = 0;
    perf_software_frame_fast_non_integer_lookup_entries_total = 0;
    perf_software_frame_fast_non_integer_alpha_only_tasks_total = 0;
    perf_software_frame_fast_non_integer_alpha_only_pixels_total = 0;
    perf_software_frame_fast_non_integer_rgb_mod_tasks_total = 0;
    perf_software_frame_fast_non_integer_rgb_mod_pixels_total = 0;
    perf_software_frame_generic_textured_tasks_total = 0;
    perf_software_frame_generic_textured_pixels_total = 0;
    perf_software_frame_generic_textured_alpha_only_tasks_total = 0;
    perf_software_frame_generic_textured_alpha_only_pixels_total = 0;
    perf_software_frame_generic_textured_rgb_mod_tasks_total = 0;
    perf_software_frame_generic_textured_rgb_mod_pixels_total = 0;
    perf_software_frame_fast_miss_color_mod_total = 0;
    perf_software_frame_fast_miss_non_integer_total = 0;
    perf_software_frame_fast_miss_non_integer_lookup_entries_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_tasks_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_pixels_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_lookup_entries_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_1024_tasks_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_1024_pixels_total = 0;
    perf_software_frame_fast_miss_non_integer_max_pixels_total = 0;
    perf_software_frame_fast_miss_scaled_total = 0;
    perf_software_frame_fast_miss_unsupported_flip_total = 0;
    perf_software_frame_fast_miss_source_bounds_total = 0;
    perf_software_frame_reason_alpha_total = 0;
    perf_software_frame_reason_color_mod_total = 0;
    perf_software_frame_reason_geometry_total = 0;
    perf_software_frame_reason_solid_total = 0;
    SDL_memset(perf_sort_strategy_frames, 0, sizeof(perf_sort_strategy_frames));
    SDL_memset(perf_fbdev_path_frames, 0, sizeof(perf_fbdev_path_frames));
    SDLGameRenderer_ResetPerfCaptureRefreshTelemetry();
    SDLGameRenderer_ResetPerfCaptureUnlockLocalityTelemetry();
    SDLGameRenderer_ResetPerfCaptureTextureRenewTelemetry();
    SDLGameRenderer_ResetPerfCaptureRasterTimingTelemetry();
    PLS03_ResetSuperArtCommandTelemetry();
}

static bool perf_capture_collect_extended_stats(void) {
    return perf_capture_enabled && !perf_capture_completed && !perf_capture_basic_mode;
}

void SDLApp_ConfigurePerfCapture(int frame_count, const char* output_path, const char* scene_name, bool basic_mode) {
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
    perf_capture_basic_mode = basic_mode;
    SDLGameRenderer_SetPerfCaptureLogicalIdentityEnabled(!basic_mode);
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
    perf_texture_cache_miss_dirty_texture_same_frame_total = 0;
    perf_texture_cache_miss_dirty_texture_carried_total = 0;
    perf_texture_cache_miss_dirty_palette_same_frame_total = 0;
    perf_texture_cache_miss_dirty_palette_carried_total = 0;
    perf_texture_cache_miss_cold_total = 0;
    perf_texture_creates_total = 0;
    perf_texture_unlock_calls_total = 0;
    perf_palette_unlock_calls_total = 0;
    perf_palette_unlock_changed_calls_total = 0;
    perf_palette_unlock_unchanged_calls_total = 0;
    perf_texture_unlock_dirty_surface_variants_total = 0;
    perf_texture_unlock_dirty_surface_variants_max_total = 0;
    perf_palette_unlock_dirty_surface_variants_total = 0;
    perf_palette_unlock_dirty_surface_variants_max_total = 0;
    perf_texture_unlock_locality_index8_tracked_total = 0;
    perf_texture_unlock_locality_index8_baseline_skips_total = 0;
    perf_texture_unlock_locality_index8_non_index8_skips_total = 0;
    perf_texture_unlock_locality_index8_source_pixels_total = 0;
    perf_texture_unlock_locality_index8_changed_pixels_total = 0;
    perf_texture_unlock_locality_index8_changed_rows_total = 0;
    perf_texture_unlock_locality_index8_changed_bbox_pixels_total = 0;
    perf_texture_unlock_invalidation_ns_total = 0;
    perf_palette_unlock_invalidation_ns_total = 0;
    perf_texture_cache_evictions_total = 0;
    perf_palette_cache_evictions_total = 0;
    perf_software_surface_cache_hits_total = 0;
    perf_software_surface_cache_creates_total = 0;
    perf_software_surface_cache_refresh_attempts_total = 0;
    perf_software_surface_cache_refresh_unique_bindings_total = 0;
    perf_software_surface_cache_refresh_repeat_binding_attempts_total = 0;
    perf_software_surface_cache_refresh_unique_texture_handles_total = 0;
    perf_software_surface_cache_refresh_texture_handle_fanout_max_total = 0;
    perf_software_surface_cache_refresh_failures_total = 0;
    perf_software_surface_cache_refresh_ns_total = 0;
    perf_software_surface_cache_refresh_palette_set_calls_total = 0;
    perf_software_surface_cache_refresh_palette_set_ns_total = 0;
    perf_software_surface_cache_refresh_blit_calls_total = 0;
    perf_software_surface_cache_refresh_blit_ns_total = 0;
    perf_software_surface_cache_create_dirty_texture_same_frame_total = 0;
    perf_software_surface_cache_create_dirty_texture_carried_total = 0;
    perf_software_surface_cache_create_dirty_palette_same_frame_total = 0;
    perf_software_surface_cache_create_dirty_palette_carried_total = 0;
    perf_software_surface_cache_create_cold_total = 0;
    perf_software_surface_cache_texture_evictions_total = 0;
    perf_software_surface_cache_palette_evictions_total = 0;
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
    for (int player = 0; player < 2; player++) {
        perf_super_art_ready_frames[player] = 0;
        perf_super_art_ready_first_frame[player] = -1;
        SDL_memset(perf_super_art_ready_routine1_frames[player], 0, sizeof(perf_super_art_ready_routine1_frames[player]));
        perf_super_art_ready_first_routine[player][0] = -1;
        perf_super_art_ready_first_routine[player][1] = -1;
        perf_super_art_ready_first_routine[player][2] = -1;
        perf_super_art_ready_last_routine[player][0] = -1;
        perf_super_art_ready_last_routine[player][1] = -1;
        perf_super_art_ready_last_routine[player][2] = -1;
        perf_super_art_active_frames[player] = 0;
        perf_super_art_active_first_frame[player] = -1;
        perf_metamorphose_active_frames[player] = 0;
        perf_metamorphose_active_first_frame[player] = -1;
    }
    perf_software_frame_mode_enabled_frames = 0;
    perf_software_frame_surface_ready_frames = 0;
    perf_software_frame_owned_frames = 0;
    perf_software_frame_direct_present_frames = 0;
    perf_software_frame_uploaded_frames = 0;
    perf_software_frame_fallback_frames = 0;
    perf_software_frame_candidate_tasks_total = 0;
    perf_software_frame_candidate_pixels_total = 0;
    perf_software_frame_fallback_tasks_total = 0;
    perf_software_frame_fallback_pixels_total = 0;
    perf_software_frame_fast_exact_tasks_total = 0;
    perf_software_frame_fast_exact_pixels_total = 0;
    perf_software_frame_fast_exact_clipped_tasks_total = 0;
    perf_software_frame_fast_exact_flipped_tasks_total = 0;
    perf_software_frame_fast_exact_color_mod_tasks_total = 0;
    perf_software_frame_fast_exact_color_mod_pixels_total = 0;
    perf_software_frame_fast_scaled_tasks_total = 0;
    perf_software_frame_fast_scaled_pixels_total = 0;
    perf_software_frame_fast_non_integer_tasks_total = 0;
    perf_software_frame_fast_non_integer_pixels_total = 0;
    perf_software_frame_fast_non_integer_lookup_entries_total = 0;
    perf_software_frame_fast_non_integer_alpha_only_tasks_total = 0;
    perf_software_frame_fast_non_integer_alpha_only_pixels_total = 0;
    perf_software_frame_fast_non_integer_rgb_mod_tasks_total = 0;
    perf_software_frame_fast_non_integer_rgb_mod_pixels_total = 0;
    perf_software_frame_generic_textured_tasks_total = 0;
    perf_software_frame_generic_textured_pixels_total = 0;
    perf_software_frame_generic_textured_alpha_only_tasks_total = 0;
    perf_software_frame_generic_textured_alpha_only_pixels_total = 0;
    perf_software_frame_generic_textured_rgb_mod_tasks_total = 0;
    perf_software_frame_generic_textured_rgb_mod_pixels_total = 0;
    perf_software_frame_fast_miss_color_mod_total = 0;
    perf_software_frame_fast_miss_non_integer_total = 0;
    perf_software_frame_fast_miss_non_integer_lookup_entries_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_tasks_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_pixels_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_256_lookup_entries_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_1024_tasks_total = 0;
    perf_software_frame_fast_miss_non_integer_ge_1024_pixels_total = 0;
    perf_software_frame_fast_miss_non_integer_max_pixels_total = 0;
    perf_software_frame_fast_miss_scaled_total = 0;
    perf_software_frame_fast_miss_unsupported_flip_total = 0;
    perf_software_frame_fast_miss_source_bounds_total = 0;
    perf_software_frame_reason_alpha_total = 0;
    perf_software_frame_reason_color_mod_total = 0;
    perf_software_frame_reason_geometry_total = 0;
    perf_software_frame_reason_solid_total = 0;
    SDL_memset(perf_sort_strategy_frames, 0, sizeof(perf_sort_strategy_frames));
    SDL_memset(perf_fbdev_path_frames, 0, sizeof(perf_fbdev_path_frames));
    SDLGameRenderer_ResetPerfCaptureRefreshTelemetry();
    SDLGameRenderer_ResetPerfCaptureUnlockLocalityTelemetry();
    SDLGameRenderer_ResetPerfCaptureTextureRenewTelemetry();
    SDLGameRenderer_ResetPerfCaptureRasterTimingTelemetry();
    PLS03_ResetSuperArtCommandTelemetry();
    perf_capture_output_path = output_path != NULL ? SDL_strdup(output_path) : NULL;
    perf_capture_scene_name = scene_name != NULL ? SDL_strdup(scene_name) : NULL;
    perf_samples = (PerfFrameSample*)SDL_calloc((size_t)frame_count, sizeof(PerfFrameSample));

    if (perf_samples == NULL) {
        backend_logf("PERF capture disabled: failed to allocate sample buffer for %d frames.", frame_count);
        perf_capture_reset_storage();
        return;
    }

    snapshot_perf_capture_transition_start_state();
    backend_logf("PERF capture enabled: frames=%d output=%s scene=%s detail_mode=%s software_frame_mode=%s",
                 perf_capture_target_frames,
                 perf_capture_output_path != NULL ? perf_capture_output_path : "(auto)",
                 perf_capture_scene_name != NULL ? perf_capture_scene_name : "(none)",
                 perf_capture_basic_mode ? "basic" : "full",
                 software_frame_mode_name());
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

static void io_write_json_int_array(SDL_IOStream* io, const int* values, int count) {
    io_printf(io, "[");
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            io_printf(io, ", ");
        }
        io_printf(io, "%d", values[i]);
    }
    io_printf(io, "]");
}

static void io_write_json_uint64_array(SDL_IOStream* io, const Uint64* values, int count) {
    io_printf(io, "[");
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            io_printf(io, ", ");
        }
        io_printf(io, "%llu", (unsigned long long)values[i]);
    }
    io_printf(io, "]");
}

static void io_write_json_super_art_ready_routine_or_null(SDL_IOStream* io, const int routine[3]) {
    if (routine[0] < 0) {
        io_printf(io, "null");
        return;
    }

    io_write_json_int_array(io, routine, 3);
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

    SDLGameRenderer_PerfCaptureRefreshTelemetry refresh_telemetry = { 0 };
    SDLGameRenderer_PerfCaptureRefreshHotTexture refresh_hot_textures[4] = { 0 };
    const int refresh_hot_texture_count = SDLGameRenderer_GetPerfCaptureRefreshTelemetry(
        &refresh_telemetry, refresh_hot_textures, SDL_arraysize(refresh_hot_textures));
    SDLGameRenderer_PerfCaptureUnlockLocalityTelemetry unlock_locality_telemetry = { 0 };
    SDLGameRenderer_PerfCaptureUnlockLocalityHotTexture unlock_locality_hot_textures[4] = { 0 };
    const int unlock_locality_hot_texture_count = SDLGameRenderer_GetPerfCaptureUnlockLocalityTelemetry(
        &unlock_locality_telemetry, unlock_locality_hot_textures, SDL_arraysize(unlock_locality_hot_textures));
    SDLGameRenderer_PerfCaptureDirtyRectLifetimeTelemetry dirty_rect_lifetime_telemetry = { 0 };
    SDLGameRenderer_GetPerfCaptureDirtyRectLifetimeTelemetry(&dirty_rect_lifetime_telemetry);
    SDLGameRenderer_PerfCaptureTextureRenewTelemetry texture_renew_telemetry = { 0 };
    SDLGameRenderer_GetPerfCaptureTextureRenewTelemetry(&texture_renew_telemetry);
    SDLGameRenderer_PerfCaptureRasterBucketTiming raster_bucket_timings[SDL_GAME_RENDERER_PERF_CAPTURE_RASTER_BUCKET_COUNT] = {
        0
    };
    const int raster_bucket_timing_count = SDLGameRenderer_GetPerfCaptureRasterBucketTimings(
        raster_bucket_timings, SDL_arraysize(raster_bucket_timings));
    SDLGameRenderer_PerfCaptureRefreshLocalityCandidate refresh_locality_candidates[8] = { 0 };
    const int refresh_locality_candidate_count = SDLGameRenderer_GetPerfCaptureRefreshLocalityCandidates(
        refresh_locality_candidates, SDL_arraysize(refresh_locality_candidates));
    const Uint64 refresh_attempts_total = refresh_telemetry.index4_attempts + refresh_telemetry.index8_attempts +
                                          refresh_telemetry.abgr1555_attempts + refresh_telemetry.other_attempts;
    const Uint64 refresh_pixels_total = refresh_telemetry.index4_pixels + refresh_telemetry.index8_pixels +
                                        refresh_telemetry.abgr1555_pixels + refresh_telemetry.other_pixels;
    const double sampled_refresh_blit_total_ms = (double)refresh_telemetry.sampled_blit_ns / 1e6;
    const double sampled_refresh_blit_mean_ms =
        refresh_telemetry.sampled_blit_calls > 0
            ? sampled_refresh_blit_total_ms / (double)refresh_telemetry.sampled_blit_calls
            : 0.0;
    const double sampled_refresh_full_blit_total_ms = (double)refresh_telemetry.sampled_full_blit_ns / 1e6;
    const double sampled_refresh_full_blit_mean_ms =
        refresh_telemetry.sampled_full_blit_calls > 0
            ? sampled_refresh_full_blit_total_ms / (double)refresh_telemetry.sampled_full_blit_calls
            : 0.0;
    const double sampled_refresh_full_non_texture_dirty_blit_total_ms =
        (double)refresh_telemetry.sampled_full_non_texture_dirty_blit_ns / 1e6;
    const double sampled_refresh_full_non_texture_dirty_blit_mean_ms =
        refresh_telemetry.sampled_full_non_texture_dirty_blit_calls > 0
            ? sampled_refresh_full_non_texture_dirty_blit_total_ms /
                  (double)refresh_telemetry.sampled_full_non_texture_dirty_blit_calls
            : 0.0;
    const double sampled_refresh_full_ineligible_source_blit_total_ms =
        (double)refresh_telemetry.sampled_full_ineligible_source_blit_ns / 1e6;
    const double sampled_refresh_full_ineligible_source_blit_mean_ms =
        refresh_telemetry.sampled_full_ineligible_source_blit_calls > 0
            ? sampled_refresh_full_ineligible_source_blit_total_ms /
                  (double)refresh_telemetry.sampled_full_ineligible_source_blit_calls
            : 0.0;
    const double sampled_refresh_full_no_usable_dirty_rect_blit_total_ms =
        (double)refresh_telemetry.sampled_full_no_usable_dirty_rect_blit_ns / 1e6;
    const double sampled_refresh_full_no_usable_dirty_rect_blit_mean_ms =
        refresh_telemetry.sampled_full_no_usable_dirty_rect_blit_calls > 0
            ? sampled_refresh_full_no_usable_dirty_rect_blit_total_ms /
                  (double)refresh_telemetry.sampled_full_no_usable_dirty_rect_blit_calls
            : 0.0;
    const double sampled_refresh_full_oversized_dirty_rect_blit_total_ms =
        (double)refresh_telemetry.sampled_full_oversized_dirty_rect_blit_ns / 1e6;
    const double sampled_refresh_full_oversized_dirty_rect_blit_mean_ms =
        refresh_telemetry.sampled_full_oversized_dirty_rect_blit_calls > 0
            ? sampled_refresh_full_oversized_dirty_rect_blit_total_ms /
                  (double)refresh_telemetry.sampled_full_oversized_dirty_rect_blit_calls
            : 0.0;
    const double sampled_refresh_partial_blit_total_ms = (double)refresh_telemetry.sampled_partial_blit_ns / 1e6;
    const double sampled_refresh_partial_blit_mean_ms =
        refresh_telemetry.sampled_partial_blit_calls > 0
            ? sampled_refresh_partial_blit_total_ms / (double)refresh_telemetry.sampled_partial_blit_calls
            : 0.0;
    Uint64 refresh_locality_candidate_refresh_attempts = 0;
    Uint64 refresh_locality_candidate_tracked_unlocks = 0;
    Uint64 refresh_locality_candidate_renew_batches = 0;
    Uint64 refresh_locality_candidate_renew_chunk_calls = 0;
    for (int i = 0; i < refresh_locality_candidate_count; i++) {
        refresh_locality_candidate_refresh_attempts += refresh_locality_candidates[i].refresh_attempts;
        refresh_locality_candidate_tracked_unlocks += refresh_locality_candidates[i].tracked_unlocks;
        refresh_locality_candidate_renew_batches += refresh_locality_candidates[i].renew_batches;
        refresh_locality_candidate_renew_chunk_calls += refresh_locality_candidates[i].renew_chunk_calls;
    }
    const double frame_count = (double)perf_capture_recorded_frames;
    PLS03_SuperArtCommandTelemetry super_art_command_telemetry[2] = { 0 };
    for (int player = 0; player < 2; player++) {
        PLS03_GetSuperArtCommandTelemetry((s16)player, &super_art_command_telemetry[player]);
    }
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
    const double avg_texture_cache_miss_dirty_texture_same_frame =
        (double)perf_texture_cache_miss_dirty_texture_same_frame_total / frame_count;
    const double avg_texture_cache_miss_dirty_texture_carried =
        (double)perf_texture_cache_miss_dirty_texture_carried_total / frame_count;
    const double avg_texture_cache_miss_dirty_palette_same_frame =
        (double)perf_texture_cache_miss_dirty_palette_same_frame_total / frame_count;
    const double avg_texture_cache_miss_dirty_palette_carried =
        (double)perf_texture_cache_miss_dirty_palette_carried_total / frame_count;
    const double avg_texture_cache_miss_cold = (double)perf_texture_cache_miss_cold_total / frame_count;
    const double avg_texture_creates = (double)perf_texture_creates_total / frame_count;
    const double avg_texture_unlock_calls = (double)perf_texture_unlock_calls_total / frame_count;
    const double avg_palette_unlock_calls = (double)perf_palette_unlock_calls_total / frame_count;
    const double avg_palette_unlock_changed_calls = (double)perf_palette_unlock_changed_calls_total / frame_count;
    const double avg_palette_unlock_unchanged_calls =
        (double)perf_palette_unlock_unchanged_calls_total / frame_count;
    const double avg_texture_unlock_dirty_surface_variants =
        (double)perf_texture_unlock_dirty_surface_variants_total / frame_count;
    const double avg_texture_unlock_dirty_surface_variants_max =
        (double)perf_texture_unlock_dirty_surface_variants_max_total / frame_count;
    const double avg_palette_unlock_dirty_surface_variants =
        (double)perf_palette_unlock_dirty_surface_variants_total / frame_count;
    const double avg_palette_unlock_dirty_surface_variants_max =
        (double)perf_palette_unlock_dirty_surface_variants_max_total / frame_count;
    const double avg_texture_unlock_locality_index8_tracked =
        (double)perf_texture_unlock_locality_index8_tracked_total / frame_count;
    const double avg_texture_unlock_locality_index8_baseline_skips =
        (double)perf_texture_unlock_locality_index8_baseline_skips_total / frame_count;
    const double avg_texture_unlock_locality_index8_non_index8_skips =
        (double)perf_texture_unlock_locality_index8_non_index8_skips_total / frame_count;
    const double avg_texture_unlock_locality_index8_source_pixels =
        (double)perf_texture_unlock_locality_index8_source_pixels_total / frame_count;
    const double avg_texture_unlock_locality_index8_changed_pixels =
        (double)perf_texture_unlock_locality_index8_changed_pixels_total / frame_count;
    const double avg_texture_unlock_locality_index8_changed_rows =
        (double)perf_texture_unlock_locality_index8_changed_rows_total / frame_count;
    const double avg_texture_unlock_locality_index8_changed_bbox_pixels =
        (double)perf_texture_unlock_locality_index8_changed_bbox_pixels_total / frame_count;
    const double avg_texture_unlock_invalidation_ms =
        ((double)perf_texture_unlock_invalidation_ns_total / frame_count) / 1e6;
    const double avg_palette_unlock_invalidation_ms =
        ((double)perf_palette_unlock_invalidation_ns_total / frame_count) / 1e6;
    const double avg_texture_cache_evictions = (double)perf_texture_cache_evictions_total / frame_count;
    const double avg_palette_cache_evictions = (double)perf_palette_cache_evictions_total / frame_count;
    const double avg_software_surface_cache_hits = (double)perf_software_surface_cache_hits_total / frame_count;
    const double avg_software_surface_cache_creates = (double)perf_software_surface_cache_creates_total / frame_count;
    const double avg_software_surface_cache_refresh_attempts =
        (double)perf_software_surface_cache_refresh_attempts_total / frame_count;
    const double avg_software_surface_cache_refresh_unique_bindings =
        (double)perf_software_surface_cache_refresh_unique_bindings_total / frame_count;
    const double avg_software_surface_cache_refresh_repeat_binding_attempts =
        (double)perf_software_surface_cache_refresh_repeat_binding_attempts_total / frame_count;
    const double avg_software_surface_cache_refresh_unique_texture_handles =
        (double)perf_software_surface_cache_refresh_unique_texture_handles_total / frame_count;
    const double avg_software_surface_cache_refresh_texture_handle_fanout_max =
        (double)perf_software_surface_cache_refresh_texture_handle_fanout_max_total / frame_count;
    const double avg_software_surface_cache_refresh_failures =
        (double)perf_software_surface_cache_refresh_failures_total / frame_count;
    const double avg_software_surface_cache_refresh_ms =
        ((double)perf_software_surface_cache_refresh_ns_total / frame_count) / 1e6;
    const double avg_software_surface_cache_refresh_palette_set_calls =
        (double)perf_software_surface_cache_refresh_palette_set_calls_total / frame_count;
    const double avg_software_surface_cache_refresh_palette_set_ms =
        ((double)perf_software_surface_cache_refresh_palette_set_ns_total / frame_count) / 1e6;
    const double avg_software_surface_cache_refresh_blit_calls =
        (double)perf_software_surface_cache_refresh_blit_calls_total / frame_count;
    const double avg_software_surface_cache_refresh_blit_ms =
        ((double)perf_software_surface_cache_refresh_blit_ns_total / frame_count) / 1e6;
    const double avg_software_surface_cache_create_dirty_texture_same_frame =
        (double)perf_software_surface_cache_create_dirty_texture_same_frame_total / frame_count;
    const double avg_software_surface_cache_create_dirty_texture_carried =
        (double)perf_software_surface_cache_create_dirty_texture_carried_total / frame_count;
    const double avg_software_surface_cache_create_dirty_palette_same_frame =
        (double)perf_software_surface_cache_create_dirty_palette_same_frame_total / frame_count;
    const double avg_software_surface_cache_create_dirty_palette_carried =
        (double)perf_software_surface_cache_create_dirty_palette_carried_total / frame_count;
    const double avg_software_surface_cache_create_cold =
        (double)perf_software_surface_cache_create_cold_total / frame_count;
    const double avg_software_surface_cache_texture_evictions =
        (double)perf_software_surface_cache_texture_evictions_total / frame_count;
    const double avg_software_surface_cache_palette_evictions =
        (double)perf_software_surface_cache_palette_evictions_total / frame_count;
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
    const double software_frame_mode_enabled_ratio = (double)perf_software_frame_mode_enabled_frames / frame_count;
    const double software_frame_surface_ready_ratio = (double)perf_software_frame_surface_ready_frames / frame_count;
    const double software_frame_owned_ratio = (double)perf_software_frame_owned_frames / frame_count;
    const double software_frame_direct_present_ratio = (double)perf_software_frame_direct_present_frames / frame_count;
    const double software_frame_uploaded_ratio = (double)perf_software_frame_uploaded_frames / frame_count;
    const double software_frame_fallback_ratio = (double)perf_software_frame_fallback_frames / frame_count;
    const double avg_software_frame_candidate_tasks = (double)perf_software_frame_candidate_tasks_total / frame_count;
    const double avg_software_frame_candidate_pixels = (double)perf_software_frame_candidate_pixels_total / frame_count;
    const double avg_software_frame_fallback_tasks = (double)perf_software_frame_fallback_tasks_total / frame_count;
    const double avg_software_frame_fallback_pixels = (double)perf_software_frame_fallback_pixels_total / frame_count;
    const double avg_software_frame_fast_exact_tasks = (double)perf_software_frame_fast_exact_tasks_total / frame_count;
    const double avg_software_frame_fast_exact_pixels =
        (double)perf_software_frame_fast_exact_pixels_total / frame_count;
    const double avg_software_frame_fast_exact_clipped_tasks =
        (double)perf_software_frame_fast_exact_clipped_tasks_total / frame_count;
    const double avg_software_frame_fast_exact_flipped_tasks =
        (double)perf_software_frame_fast_exact_flipped_tasks_total / frame_count;
    const double avg_software_frame_fast_exact_color_mod_tasks =
        (double)perf_software_frame_fast_exact_color_mod_tasks_total / frame_count;
    const double avg_software_frame_fast_exact_color_mod_pixels =
        (double)perf_software_frame_fast_exact_color_mod_pixels_total / frame_count;
    const double avg_software_frame_fast_scaled_tasks =
        (double)perf_software_frame_fast_scaled_tasks_total / frame_count;
    const double avg_software_frame_fast_scaled_pixels =
        (double)perf_software_frame_fast_scaled_pixels_total / frame_count;
    const double avg_software_frame_fast_non_integer_tasks =
        (double)perf_software_frame_fast_non_integer_tasks_total / frame_count;
    const double avg_software_frame_fast_non_integer_pixels =
        (double)perf_software_frame_fast_non_integer_pixels_total / frame_count;
    const double avg_software_frame_fast_non_integer_lookup_entries =
        (double)perf_software_frame_fast_non_integer_lookup_entries_total / frame_count;
    const double avg_software_frame_fast_non_integer_alpha_only_tasks =
        (double)perf_software_frame_fast_non_integer_alpha_only_tasks_total / frame_count;
    const double avg_software_frame_fast_non_integer_alpha_only_pixels =
        (double)perf_software_frame_fast_non_integer_alpha_only_pixels_total / frame_count;
    const double avg_software_frame_fast_non_integer_rgb_mod_tasks =
        (double)perf_software_frame_fast_non_integer_rgb_mod_tasks_total / frame_count;
    const double avg_software_frame_fast_non_integer_rgb_mod_pixels =
        (double)perf_software_frame_fast_non_integer_rgb_mod_pixels_total / frame_count;
    const double avg_software_frame_generic_textured_tasks =
        (double)perf_software_frame_generic_textured_tasks_total / frame_count;
    const double avg_software_frame_generic_textured_pixels =
        (double)perf_software_frame_generic_textured_pixels_total / frame_count;
    const double avg_software_frame_generic_textured_alpha_only_tasks =
        (double)perf_software_frame_generic_textured_alpha_only_tasks_total / frame_count;
    const double avg_software_frame_generic_textured_alpha_only_pixels =
        (double)perf_software_frame_generic_textured_alpha_only_pixels_total / frame_count;
    const double avg_software_frame_generic_textured_rgb_mod_tasks =
        (double)perf_software_frame_generic_textured_rgb_mod_tasks_total / frame_count;
    const double avg_software_frame_generic_textured_rgb_mod_pixels =
        (double)perf_software_frame_generic_textured_rgb_mod_pixels_total / frame_count;
    const double avg_software_frame_fast_miss_color_mod =
        (double)perf_software_frame_fast_miss_color_mod_total / frame_count;
    const double avg_software_frame_fast_miss_non_integer =
        (double)perf_software_frame_fast_miss_non_integer_total / frame_count;
    const double avg_software_frame_fast_miss_non_integer_lookup_entries =
        (double)perf_software_frame_fast_miss_non_integer_lookup_entries_total / frame_count;
    const double avg_software_frame_fast_miss_non_integer_ge_256_tasks =
        (double)perf_software_frame_fast_miss_non_integer_ge_256_tasks_total / frame_count;
    const double avg_software_frame_fast_miss_non_integer_ge_256_pixels =
        (double)perf_software_frame_fast_miss_non_integer_ge_256_pixels_total / frame_count;
    const double avg_software_frame_fast_miss_non_integer_ge_256_lookup_entries =
        (double)perf_software_frame_fast_miss_non_integer_ge_256_lookup_entries_total / frame_count;
    const double avg_software_frame_fast_miss_non_integer_ge_1024_tasks =
        (double)perf_software_frame_fast_miss_non_integer_ge_1024_tasks_total / frame_count;
    const double avg_software_frame_fast_miss_non_integer_ge_1024_pixels =
        (double)perf_software_frame_fast_miss_non_integer_ge_1024_pixels_total / frame_count;
    const double avg_software_frame_fast_miss_non_integer_max_pixels =
        (double)perf_software_frame_fast_miss_non_integer_max_pixels_total / frame_count;
    const double avg_software_frame_fast_miss_scaled = (double)perf_software_frame_fast_miss_scaled_total / frame_count;
    const double avg_software_frame_fast_miss_unsupported_flip =
        (double)perf_software_frame_fast_miss_unsupported_flip_total / frame_count;
    const double avg_software_frame_fast_miss_source_bounds =
        (double)perf_software_frame_fast_miss_source_bounds_total / frame_count;
    const double avg_software_frame_reason_alpha = (double)perf_software_frame_reason_alpha_total / frame_count;
    const double avg_software_frame_reason_color_mod = (double)perf_software_frame_reason_color_mod_total / frame_count;
    const double avg_software_frame_reason_geometry = (double)perf_software_frame_reason_geometry_total / frame_count;
    const double avg_software_frame_reason_solid = (double)perf_software_frame_reason_solid_total / frame_count;
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
    int min_texture_cache_miss_dirty_texture_same_frame = perf_samples[0].texture_cache_miss_dirty_texture_same_frame;
    int max_texture_cache_miss_dirty_texture_same_frame = perf_samples[0].texture_cache_miss_dirty_texture_same_frame;
    int min_texture_cache_miss_dirty_texture_carried = perf_samples[0].texture_cache_miss_dirty_texture_carried;
    int max_texture_cache_miss_dirty_texture_carried = perf_samples[0].texture_cache_miss_dirty_texture_carried;
    int min_texture_cache_miss_dirty_palette_same_frame = perf_samples[0].texture_cache_miss_dirty_palette_same_frame;
    int max_texture_cache_miss_dirty_palette_same_frame = perf_samples[0].texture_cache_miss_dirty_palette_same_frame;
    int min_texture_cache_miss_dirty_palette_carried = perf_samples[0].texture_cache_miss_dirty_palette_carried;
    int max_texture_cache_miss_dirty_palette_carried = perf_samples[0].texture_cache_miss_dirty_palette_carried;
    int min_texture_cache_miss_cold = perf_samples[0].texture_cache_miss_cold;
    int max_texture_cache_miss_cold = perf_samples[0].texture_cache_miss_cold;
    int min_texture_creates = perf_samples[0].texture_creates;
    int max_texture_creates = perf_samples[0].texture_creates;
    int min_texture_unlock_calls = perf_samples[0].texture_unlock_calls;
    int max_texture_unlock_calls = perf_samples[0].texture_unlock_calls;
    int min_palette_unlock_calls = perf_samples[0].palette_unlock_calls;
    int max_palette_unlock_calls = perf_samples[0].palette_unlock_calls;
    int min_palette_unlock_changed_calls = perf_samples[0].palette_unlock_changed_calls;
    int max_palette_unlock_changed_calls = perf_samples[0].palette_unlock_changed_calls;
    int min_palette_unlock_unchanged_calls = perf_samples[0].palette_unlock_unchanged_calls;
    int max_palette_unlock_unchanged_calls = perf_samples[0].palette_unlock_unchanged_calls;
    int min_texture_unlock_dirty_surface_variants = perf_samples[0].texture_unlock_dirty_surface_variants;
    int max_texture_unlock_dirty_surface_variants = perf_samples[0].texture_unlock_dirty_surface_variants;
    int min_texture_unlock_dirty_surface_variants_max = perf_samples[0].texture_unlock_dirty_surface_variants_max;
    int max_texture_unlock_dirty_surface_variants_max = perf_samples[0].texture_unlock_dirty_surface_variants_max;
    int min_palette_unlock_dirty_surface_variants = perf_samples[0].palette_unlock_dirty_surface_variants;
    int max_palette_unlock_dirty_surface_variants = perf_samples[0].palette_unlock_dirty_surface_variants;
    int min_palette_unlock_dirty_surface_variants_max = perf_samples[0].palette_unlock_dirty_surface_variants_max;
    int max_palette_unlock_dirty_surface_variants_max = perf_samples[0].palette_unlock_dirty_surface_variants_max;
    int min_texture_unlock_locality_index8_tracked = perf_samples[0].texture_unlock_locality_index8_tracked;
    int max_texture_unlock_locality_index8_tracked = perf_samples[0].texture_unlock_locality_index8_tracked;
    int min_texture_unlock_locality_index8_baseline_skips =
        perf_samples[0].texture_unlock_locality_index8_baseline_skips;
    int max_texture_unlock_locality_index8_baseline_skips =
        perf_samples[0].texture_unlock_locality_index8_baseline_skips;
    int min_texture_unlock_locality_index8_non_index8_skips =
        perf_samples[0].texture_unlock_locality_index8_non_index8_skips;
    int max_texture_unlock_locality_index8_non_index8_skips =
        perf_samples[0].texture_unlock_locality_index8_non_index8_skips;
    Uint64 min_texture_unlock_locality_index8_source_pixels =
        perf_samples[0].texture_unlock_locality_index8_source_pixels;
    Uint64 max_texture_unlock_locality_index8_source_pixels =
        perf_samples[0].texture_unlock_locality_index8_source_pixels;
    Uint64 min_texture_unlock_locality_index8_changed_pixels =
        perf_samples[0].texture_unlock_locality_index8_changed_pixels;
    Uint64 max_texture_unlock_locality_index8_changed_pixels =
        perf_samples[0].texture_unlock_locality_index8_changed_pixels;
    Uint64 min_texture_unlock_locality_index8_changed_rows =
        perf_samples[0].texture_unlock_locality_index8_changed_rows;
    Uint64 max_texture_unlock_locality_index8_changed_rows =
        perf_samples[0].texture_unlock_locality_index8_changed_rows;
    Uint64 min_texture_unlock_locality_index8_changed_bbox_pixels =
        perf_samples[0].texture_unlock_locality_index8_changed_bbox_pixels;
    Uint64 max_texture_unlock_locality_index8_changed_bbox_pixels =
        perf_samples[0].texture_unlock_locality_index8_changed_bbox_pixels;
    double min_texture_unlock_invalidation_ms = perf_samples[0].texture_unlock_invalidation_ms;
    double max_texture_unlock_invalidation_ms = perf_samples[0].texture_unlock_invalidation_ms;
    double min_palette_unlock_invalidation_ms = perf_samples[0].palette_unlock_invalidation_ms;
    double max_palette_unlock_invalidation_ms = perf_samples[0].palette_unlock_invalidation_ms;
    int min_texture_cache_evictions = perf_samples[0].texture_cache_evictions;
    int max_texture_cache_evictions = perf_samples[0].texture_cache_evictions;
    int min_palette_cache_evictions = perf_samples[0].palette_cache_evictions;
    int max_palette_cache_evictions = perf_samples[0].palette_cache_evictions;
    int min_software_surface_cache_hits = perf_samples[0].software_surface_cache_hits;
    int max_software_surface_cache_hits = perf_samples[0].software_surface_cache_hits;
    int min_software_surface_cache_creates = perf_samples[0].software_surface_cache_creates;
    int max_software_surface_cache_creates = perf_samples[0].software_surface_cache_creates;
    int min_software_surface_cache_refresh_attempts = perf_samples[0].software_surface_cache_refresh_attempts;
    int max_software_surface_cache_refresh_attempts = perf_samples[0].software_surface_cache_refresh_attempts;
    int min_software_surface_cache_refresh_unique_bindings =
        perf_samples[0].software_surface_cache_refresh_unique_bindings;
    int max_software_surface_cache_refresh_unique_bindings =
        perf_samples[0].software_surface_cache_refresh_unique_bindings;
    int min_software_surface_cache_refresh_repeat_binding_attempts =
        perf_samples[0].software_surface_cache_refresh_repeat_binding_attempts;
    int max_software_surface_cache_refresh_repeat_binding_attempts =
        perf_samples[0].software_surface_cache_refresh_repeat_binding_attempts;
    int min_software_surface_cache_refresh_unique_texture_handles =
        perf_samples[0].software_surface_cache_refresh_unique_texture_handles;
    int max_software_surface_cache_refresh_unique_texture_handles =
        perf_samples[0].software_surface_cache_refresh_unique_texture_handles;
    int min_software_surface_cache_refresh_texture_handle_fanout_max =
        perf_samples[0].software_surface_cache_refresh_texture_handle_fanout_max;
    int max_software_surface_cache_refresh_texture_handle_fanout_max =
        perf_samples[0].software_surface_cache_refresh_texture_handle_fanout_max;
    int min_software_surface_cache_refresh_failures = perf_samples[0].software_surface_cache_refresh_failures;
    int max_software_surface_cache_refresh_failures = perf_samples[0].software_surface_cache_refresh_failures;
    double min_software_surface_cache_refresh_ms = perf_samples[0].software_surface_cache_refresh_ms;
    double max_software_surface_cache_refresh_ms = perf_samples[0].software_surface_cache_refresh_ms;
    int min_software_surface_cache_refresh_palette_set_calls =
        perf_samples[0].software_surface_cache_refresh_palette_set_calls;
    int max_software_surface_cache_refresh_palette_set_calls =
        perf_samples[0].software_surface_cache_refresh_palette_set_calls;
    double min_software_surface_cache_refresh_palette_set_ms =
        perf_samples[0].software_surface_cache_refresh_palette_set_ms;
    double max_software_surface_cache_refresh_palette_set_ms =
        perf_samples[0].software_surface_cache_refresh_palette_set_ms;
    int min_software_surface_cache_refresh_blit_calls = perf_samples[0].software_surface_cache_refresh_blit_calls;
    int max_software_surface_cache_refresh_blit_calls = perf_samples[0].software_surface_cache_refresh_blit_calls;
    double min_software_surface_cache_refresh_blit_ms = perf_samples[0].software_surface_cache_refresh_blit_ms;
    double max_software_surface_cache_refresh_blit_ms = perf_samples[0].software_surface_cache_refresh_blit_ms;
    int min_software_surface_cache_create_dirty_texture_same_frame =
        perf_samples[0].software_surface_cache_create_dirty_texture_same_frame;
    int max_software_surface_cache_create_dirty_texture_same_frame =
        perf_samples[0].software_surface_cache_create_dirty_texture_same_frame;
    int min_software_surface_cache_create_dirty_texture_carried =
        perf_samples[0].software_surface_cache_create_dirty_texture_carried;
    int max_software_surface_cache_create_dirty_texture_carried =
        perf_samples[0].software_surface_cache_create_dirty_texture_carried;
    int min_software_surface_cache_create_dirty_palette_same_frame =
        perf_samples[0].software_surface_cache_create_dirty_palette_same_frame;
    int max_software_surface_cache_create_dirty_palette_same_frame =
        perf_samples[0].software_surface_cache_create_dirty_palette_same_frame;
    int min_software_surface_cache_create_dirty_palette_carried =
        perf_samples[0].software_surface_cache_create_dirty_palette_carried;
    int max_software_surface_cache_create_dirty_palette_carried =
        perf_samples[0].software_surface_cache_create_dirty_palette_carried;
    int min_software_surface_cache_create_cold = perf_samples[0].software_surface_cache_create_cold;
    int max_software_surface_cache_create_cold = perf_samples[0].software_surface_cache_create_cold;
    int min_software_surface_cache_texture_evictions = perf_samples[0].software_surface_cache_texture_evictions;
    int max_software_surface_cache_texture_evictions = perf_samples[0].software_surface_cache_texture_evictions;
    int min_software_surface_cache_palette_evictions = perf_samples[0].software_surface_cache_palette_evictions;
    int max_software_surface_cache_palette_evictions = perf_samples[0].software_surface_cache_palette_evictions;
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
    int min_software_frame_candidate_tasks = perf_samples[0].software_frame_candidate_tasks;
    int max_software_frame_candidate_tasks = perf_samples[0].software_frame_candidate_tasks;
    Uint64 min_software_frame_candidate_pixels = perf_samples[0].software_frame_candidate_pixels;
    Uint64 max_software_frame_candidate_pixels = perf_samples[0].software_frame_candidate_pixels;
    int min_software_frame_fallback_tasks = perf_samples[0].software_frame_fallback_tasks;
    int max_software_frame_fallback_tasks = perf_samples[0].software_frame_fallback_tasks;
    Uint64 min_software_frame_fallback_pixels = perf_samples[0].software_frame_fallback_pixels;
    Uint64 max_software_frame_fallback_pixels = perf_samples[0].software_frame_fallback_pixels;
    int min_software_frame_fast_exact_tasks = perf_samples[0].software_frame_fast_exact_tasks;
    int max_software_frame_fast_exact_tasks = perf_samples[0].software_frame_fast_exact_tasks;
    Uint64 min_software_frame_fast_exact_pixels = perf_samples[0].software_frame_fast_exact_pixels;
    Uint64 max_software_frame_fast_exact_pixels = perf_samples[0].software_frame_fast_exact_pixels;
    int min_software_frame_fast_exact_clipped_tasks = perf_samples[0].software_frame_fast_exact_clipped_tasks;
    int max_software_frame_fast_exact_clipped_tasks = perf_samples[0].software_frame_fast_exact_clipped_tasks;
    int min_software_frame_fast_exact_flipped_tasks = perf_samples[0].software_frame_fast_exact_flipped_tasks;
    int max_software_frame_fast_exact_flipped_tasks = perf_samples[0].software_frame_fast_exact_flipped_tasks;
    int min_software_frame_fast_exact_color_mod_tasks = perf_samples[0].software_frame_fast_exact_color_mod_tasks;
    int max_software_frame_fast_exact_color_mod_tasks = perf_samples[0].software_frame_fast_exact_color_mod_tasks;
    Uint64 min_software_frame_fast_exact_color_mod_pixels = perf_samples[0].software_frame_fast_exact_color_mod_pixels;
    Uint64 max_software_frame_fast_exact_color_mod_pixels = perf_samples[0].software_frame_fast_exact_color_mod_pixels;
    int min_software_frame_fast_scaled_tasks = perf_samples[0].software_frame_fast_scaled_tasks;
    int max_software_frame_fast_scaled_tasks = perf_samples[0].software_frame_fast_scaled_tasks;
    Uint64 min_software_frame_fast_scaled_pixels = perf_samples[0].software_frame_fast_scaled_pixels;
    Uint64 max_software_frame_fast_scaled_pixels = perf_samples[0].software_frame_fast_scaled_pixels;
    int min_software_frame_fast_non_integer_tasks = perf_samples[0].software_frame_fast_non_integer_tasks;
    int max_software_frame_fast_non_integer_tasks = perf_samples[0].software_frame_fast_non_integer_tasks;
    Uint64 min_software_frame_fast_non_integer_pixels = perf_samples[0].software_frame_fast_non_integer_pixels;
    Uint64 max_software_frame_fast_non_integer_pixels = perf_samples[0].software_frame_fast_non_integer_pixels;
    Uint64 min_software_frame_fast_non_integer_lookup_entries =
        perf_samples[0].software_frame_fast_non_integer_lookup_entries;
    Uint64 max_software_frame_fast_non_integer_lookup_entries =
        perf_samples[0].software_frame_fast_non_integer_lookup_entries;
    int min_software_frame_fast_non_integer_alpha_only_tasks =
        perf_samples[0].software_frame_fast_non_integer_alpha_only_tasks;
    int max_software_frame_fast_non_integer_alpha_only_tasks =
        perf_samples[0].software_frame_fast_non_integer_alpha_only_tasks;
    Uint64 min_software_frame_fast_non_integer_alpha_only_pixels =
        perf_samples[0].software_frame_fast_non_integer_alpha_only_pixels;
    Uint64 max_software_frame_fast_non_integer_alpha_only_pixels =
        perf_samples[0].software_frame_fast_non_integer_alpha_only_pixels;
    int min_software_frame_fast_non_integer_rgb_mod_tasks =
        perf_samples[0].software_frame_fast_non_integer_rgb_mod_tasks;
    int max_software_frame_fast_non_integer_rgb_mod_tasks =
        perf_samples[0].software_frame_fast_non_integer_rgb_mod_tasks;
    Uint64 min_software_frame_fast_non_integer_rgb_mod_pixels =
        perf_samples[0].software_frame_fast_non_integer_rgb_mod_pixels;
    Uint64 max_software_frame_fast_non_integer_rgb_mod_pixels =
        perf_samples[0].software_frame_fast_non_integer_rgb_mod_pixels;
    int min_software_frame_generic_textured_tasks = perf_samples[0].software_frame_generic_textured_tasks;
    int max_software_frame_generic_textured_tasks = perf_samples[0].software_frame_generic_textured_tasks;
    Uint64 min_software_frame_generic_textured_pixels = perf_samples[0].software_frame_generic_textured_pixels;
    Uint64 max_software_frame_generic_textured_pixels = perf_samples[0].software_frame_generic_textured_pixels;
    int min_software_frame_generic_textured_alpha_only_tasks =
        perf_samples[0].software_frame_generic_textured_alpha_only_tasks;
    int max_software_frame_generic_textured_alpha_only_tasks =
        perf_samples[0].software_frame_generic_textured_alpha_only_tasks;
    Uint64 min_software_frame_generic_textured_alpha_only_pixels =
        perf_samples[0].software_frame_generic_textured_alpha_only_pixels;
    Uint64 max_software_frame_generic_textured_alpha_only_pixels =
        perf_samples[0].software_frame_generic_textured_alpha_only_pixels;
    int min_software_frame_generic_textured_rgb_mod_tasks =
        perf_samples[0].software_frame_generic_textured_rgb_mod_tasks;
    int max_software_frame_generic_textured_rgb_mod_tasks =
        perf_samples[0].software_frame_generic_textured_rgb_mod_tasks;
    Uint64 min_software_frame_generic_textured_rgb_mod_pixels =
        perf_samples[0].software_frame_generic_textured_rgb_mod_pixels;
    Uint64 max_software_frame_generic_textured_rgb_mod_pixels =
        perf_samples[0].software_frame_generic_textured_rgb_mod_pixels;
    int min_software_frame_fast_miss_color_mod = perf_samples[0].software_frame_fast_miss_color_mod;
    int max_software_frame_fast_miss_color_mod = perf_samples[0].software_frame_fast_miss_color_mod;
    int min_software_frame_fast_miss_non_integer = perf_samples[0].software_frame_fast_miss_non_integer;
    int max_software_frame_fast_miss_non_integer = perf_samples[0].software_frame_fast_miss_non_integer;
    Uint64 min_software_frame_fast_miss_non_integer_lookup_entries =
        perf_samples[0].software_frame_fast_miss_non_integer_lookup_entries;
    Uint64 max_software_frame_fast_miss_non_integer_lookup_entries =
        perf_samples[0].software_frame_fast_miss_non_integer_lookup_entries;
    int min_software_frame_fast_miss_non_integer_ge_256_tasks =
        perf_samples[0].software_frame_fast_miss_non_integer_ge_256_tasks;
    int max_software_frame_fast_miss_non_integer_ge_256_tasks =
        perf_samples[0].software_frame_fast_miss_non_integer_ge_256_tasks;
    Uint64 min_software_frame_fast_miss_non_integer_ge_256_pixels =
        perf_samples[0].software_frame_fast_miss_non_integer_ge_256_pixels;
    Uint64 max_software_frame_fast_miss_non_integer_ge_256_pixels =
        perf_samples[0].software_frame_fast_miss_non_integer_ge_256_pixels;
    Uint64 min_software_frame_fast_miss_non_integer_ge_256_lookup_entries =
        perf_samples[0].software_frame_fast_miss_non_integer_ge_256_lookup_entries;
    Uint64 max_software_frame_fast_miss_non_integer_ge_256_lookup_entries =
        perf_samples[0].software_frame_fast_miss_non_integer_ge_256_lookup_entries;
    int min_software_frame_fast_miss_non_integer_ge_1024_tasks =
        perf_samples[0].software_frame_fast_miss_non_integer_ge_1024_tasks;
    int max_software_frame_fast_miss_non_integer_ge_1024_tasks =
        perf_samples[0].software_frame_fast_miss_non_integer_ge_1024_tasks;
    Uint64 min_software_frame_fast_miss_non_integer_ge_1024_pixels =
        perf_samples[0].software_frame_fast_miss_non_integer_ge_1024_pixels;
    Uint64 max_software_frame_fast_miss_non_integer_ge_1024_pixels =
        perf_samples[0].software_frame_fast_miss_non_integer_ge_1024_pixels;
    Uint64 min_software_frame_fast_miss_non_integer_max_pixels =
        perf_samples[0].software_frame_fast_miss_non_integer_max_pixels;
    Uint64 max_software_frame_fast_miss_non_integer_max_pixels =
        perf_samples[0].software_frame_fast_miss_non_integer_max_pixels;
    int min_software_frame_fast_miss_scaled = perf_samples[0].software_frame_fast_miss_scaled;
    int max_software_frame_fast_miss_scaled = perf_samples[0].software_frame_fast_miss_scaled;
    int min_software_frame_fast_miss_unsupported_flip = perf_samples[0].software_frame_fast_miss_unsupported_flip;
    int max_software_frame_fast_miss_unsupported_flip = perf_samples[0].software_frame_fast_miss_unsupported_flip;
    int min_software_frame_fast_miss_source_bounds = perf_samples[0].software_frame_fast_miss_source_bounds;
    int max_software_frame_fast_miss_source_bounds = perf_samples[0].software_frame_fast_miss_source_bounds;
    int min_software_frame_reason_alpha = perf_samples[0].software_frame_reason_alpha;
    int max_software_frame_reason_alpha = perf_samples[0].software_frame_reason_alpha;
    int min_software_frame_reason_color_mod = perf_samples[0].software_frame_reason_color_mod;
    int max_software_frame_reason_color_mod = perf_samples[0].software_frame_reason_color_mod;
    int min_software_frame_reason_geometry = perf_samples[0].software_frame_reason_geometry;
    int max_software_frame_reason_geometry = perf_samples[0].software_frame_reason_geometry;
    int min_software_frame_reason_solid = perf_samples[0].software_frame_reason_solid;
    int max_software_frame_reason_solid = perf_samples[0].software_frame_reason_solid;
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
        if (sample->texture_cache_miss_dirty_texture_same_frame < min_texture_cache_miss_dirty_texture_same_frame) {
            min_texture_cache_miss_dirty_texture_same_frame = sample->texture_cache_miss_dirty_texture_same_frame;
        }
        if (sample->texture_cache_miss_dirty_texture_same_frame > max_texture_cache_miss_dirty_texture_same_frame) {
            max_texture_cache_miss_dirty_texture_same_frame = sample->texture_cache_miss_dirty_texture_same_frame;
        }
        if (sample->texture_cache_miss_dirty_texture_carried < min_texture_cache_miss_dirty_texture_carried) {
            min_texture_cache_miss_dirty_texture_carried = sample->texture_cache_miss_dirty_texture_carried;
        }
        if (sample->texture_cache_miss_dirty_texture_carried > max_texture_cache_miss_dirty_texture_carried) {
            max_texture_cache_miss_dirty_texture_carried = sample->texture_cache_miss_dirty_texture_carried;
        }
        if (sample->texture_cache_miss_dirty_palette_same_frame < min_texture_cache_miss_dirty_palette_same_frame) {
            min_texture_cache_miss_dirty_palette_same_frame = sample->texture_cache_miss_dirty_palette_same_frame;
        }
        if (sample->texture_cache_miss_dirty_palette_same_frame > max_texture_cache_miss_dirty_palette_same_frame) {
            max_texture_cache_miss_dirty_palette_same_frame = sample->texture_cache_miss_dirty_palette_same_frame;
        }
        if (sample->texture_cache_miss_dirty_palette_carried < min_texture_cache_miss_dirty_palette_carried) {
            min_texture_cache_miss_dirty_palette_carried = sample->texture_cache_miss_dirty_palette_carried;
        }
        if (sample->texture_cache_miss_dirty_palette_carried > max_texture_cache_miss_dirty_palette_carried) {
            max_texture_cache_miss_dirty_palette_carried = sample->texture_cache_miss_dirty_palette_carried;
        }
        if (sample->texture_cache_miss_cold < min_texture_cache_miss_cold) {
            min_texture_cache_miss_cold = sample->texture_cache_miss_cold;
        }
        if (sample->texture_cache_miss_cold > max_texture_cache_miss_cold) {
            max_texture_cache_miss_cold = sample->texture_cache_miss_cold;
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
        if (sample->palette_unlock_changed_calls < min_palette_unlock_changed_calls) {
            min_palette_unlock_changed_calls = sample->palette_unlock_changed_calls;
        }
        if (sample->palette_unlock_changed_calls > max_palette_unlock_changed_calls) {
            max_palette_unlock_changed_calls = sample->palette_unlock_changed_calls;
        }
        if (sample->palette_unlock_unchanged_calls < min_palette_unlock_unchanged_calls) {
            min_palette_unlock_unchanged_calls = sample->palette_unlock_unchanged_calls;
        }
        if (sample->palette_unlock_unchanged_calls > max_palette_unlock_unchanged_calls) {
            max_palette_unlock_unchanged_calls = sample->palette_unlock_unchanged_calls;
        }
        if (sample->texture_unlock_dirty_surface_variants < min_texture_unlock_dirty_surface_variants) {
            min_texture_unlock_dirty_surface_variants = sample->texture_unlock_dirty_surface_variants;
        }
        if (sample->texture_unlock_dirty_surface_variants > max_texture_unlock_dirty_surface_variants) {
            max_texture_unlock_dirty_surface_variants = sample->texture_unlock_dirty_surface_variants;
        }
        if (sample->texture_unlock_dirty_surface_variants_max < min_texture_unlock_dirty_surface_variants_max) {
            min_texture_unlock_dirty_surface_variants_max = sample->texture_unlock_dirty_surface_variants_max;
        }
        if (sample->texture_unlock_dirty_surface_variants_max > max_texture_unlock_dirty_surface_variants_max) {
            max_texture_unlock_dirty_surface_variants_max = sample->texture_unlock_dirty_surface_variants_max;
        }
        if (sample->palette_unlock_dirty_surface_variants < min_palette_unlock_dirty_surface_variants) {
            min_palette_unlock_dirty_surface_variants = sample->palette_unlock_dirty_surface_variants;
        }
        if (sample->palette_unlock_dirty_surface_variants > max_palette_unlock_dirty_surface_variants) {
            max_palette_unlock_dirty_surface_variants = sample->palette_unlock_dirty_surface_variants;
        }
        if (sample->palette_unlock_dirty_surface_variants_max < min_palette_unlock_dirty_surface_variants_max) {
            min_palette_unlock_dirty_surface_variants_max = sample->palette_unlock_dirty_surface_variants_max;
        }
        if (sample->palette_unlock_dirty_surface_variants_max > max_palette_unlock_dirty_surface_variants_max) {
            max_palette_unlock_dirty_surface_variants_max = sample->palette_unlock_dirty_surface_variants_max;
        }
        if (sample->texture_unlock_locality_index8_tracked < min_texture_unlock_locality_index8_tracked) {
            min_texture_unlock_locality_index8_tracked = sample->texture_unlock_locality_index8_tracked;
        }
        if (sample->texture_unlock_locality_index8_tracked > max_texture_unlock_locality_index8_tracked) {
            max_texture_unlock_locality_index8_tracked = sample->texture_unlock_locality_index8_tracked;
        }
        if (sample->texture_unlock_locality_index8_baseline_skips <
            min_texture_unlock_locality_index8_baseline_skips) {
            min_texture_unlock_locality_index8_baseline_skips = sample->texture_unlock_locality_index8_baseline_skips;
        }
        if (sample->texture_unlock_locality_index8_baseline_skips >
            max_texture_unlock_locality_index8_baseline_skips) {
            max_texture_unlock_locality_index8_baseline_skips = sample->texture_unlock_locality_index8_baseline_skips;
        }
        if (sample->texture_unlock_locality_index8_non_index8_skips <
            min_texture_unlock_locality_index8_non_index8_skips) {
            min_texture_unlock_locality_index8_non_index8_skips =
                sample->texture_unlock_locality_index8_non_index8_skips;
        }
        if (sample->texture_unlock_locality_index8_non_index8_skips >
            max_texture_unlock_locality_index8_non_index8_skips) {
            max_texture_unlock_locality_index8_non_index8_skips =
                sample->texture_unlock_locality_index8_non_index8_skips;
        }
        if (sample->texture_unlock_locality_index8_source_pixels <
            min_texture_unlock_locality_index8_source_pixels) {
            min_texture_unlock_locality_index8_source_pixels = sample->texture_unlock_locality_index8_source_pixels;
        }
        if (sample->texture_unlock_locality_index8_source_pixels >
            max_texture_unlock_locality_index8_source_pixels) {
            max_texture_unlock_locality_index8_source_pixels = sample->texture_unlock_locality_index8_source_pixels;
        }
        if (sample->texture_unlock_locality_index8_changed_pixels <
            min_texture_unlock_locality_index8_changed_pixels) {
            min_texture_unlock_locality_index8_changed_pixels = sample->texture_unlock_locality_index8_changed_pixels;
        }
        if (sample->texture_unlock_locality_index8_changed_pixels >
            max_texture_unlock_locality_index8_changed_pixels) {
            max_texture_unlock_locality_index8_changed_pixels = sample->texture_unlock_locality_index8_changed_pixels;
        }
        if (sample->texture_unlock_locality_index8_changed_rows < min_texture_unlock_locality_index8_changed_rows) {
            min_texture_unlock_locality_index8_changed_rows = sample->texture_unlock_locality_index8_changed_rows;
        }
        if (sample->texture_unlock_locality_index8_changed_rows > max_texture_unlock_locality_index8_changed_rows) {
            max_texture_unlock_locality_index8_changed_rows = sample->texture_unlock_locality_index8_changed_rows;
        }
        if (sample->texture_unlock_locality_index8_changed_bbox_pixels <
            min_texture_unlock_locality_index8_changed_bbox_pixels) {
            min_texture_unlock_locality_index8_changed_bbox_pixels =
                sample->texture_unlock_locality_index8_changed_bbox_pixels;
        }
        if (sample->texture_unlock_locality_index8_changed_bbox_pixels >
            max_texture_unlock_locality_index8_changed_bbox_pixels) {
            max_texture_unlock_locality_index8_changed_bbox_pixels =
                sample->texture_unlock_locality_index8_changed_bbox_pixels;
        }
        if (sample->texture_unlock_invalidation_ms < min_texture_unlock_invalidation_ms) {
            min_texture_unlock_invalidation_ms = sample->texture_unlock_invalidation_ms;
        }
        if (sample->texture_unlock_invalidation_ms > max_texture_unlock_invalidation_ms) {
            max_texture_unlock_invalidation_ms = sample->texture_unlock_invalidation_ms;
        }
        if (sample->palette_unlock_invalidation_ms < min_palette_unlock_invalidation_ms) {
            min_palette_unlock_invalidation_ms = sample->palette_unlock_invalidation_ms;
        }
        if (sample->palette_unlock_invalidation_ms > max_palette_unlock_invalidation_ms) {
            max_palette_unlock_invalidation_ms = sample->palette_unlock_invalidation_ms;
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
        if (sample->software_surface_cache_hits < min_software_surface_cache_hits) {
            min_software_surface_cache_hits = sample->software_surface_cache_hits;
        }
        if (sample->software_surface_cache_hits > max_software_surface_cache_hits) {
            max_software_surface_cache_hits = sample->software_surface_cache_hits;
        }
        if (sample->software_surface_cache_creates < min_software_surface_cache_creates) {
            min_software_surface_cache_creates = sample->software_surface_cache_creates;
        }
        if (sample->software_surface_cache_creates > max_software_surface_cache_creates) {
            max_software_surface_cache_creates = sample->software_surface_cache_creates;
        }
        if (sample->software_surface_cache_refresh_attempts < min_software_surface_cache_refresh_attempts) {
            min_software_surface_cache_refresh_attempts = sample->software_surface_cache_refresh_attempts;
        }
        if (sample->software_surface_cache_refresh_attempts > max_software_surface_cache_refresh_attempts) {
            max_software_surface_cache_refresh_attempts = sample->software_surface_cache_refresh_attempts;
        }
        if (sample->software_surface_cache_refresh_unique_bindings <
            min_software_surface_cache_refresh_unique_bindings) {
            min_software_surface_cache_refresh_unique_bindings = sample->software_surface_cache_refresh_unique_bindings;
        }
        if (sample->software_surface_cache_refresh_unique_bindings >
            max_software_surface_cache_refresh_unique_bindings) {
            max_software_surface_cache_refresh_unique_bindings = sample->software_surface_cache_refresh_unique_bindings;
        }
        if (sample->software_surface_cache_refresh_repeat_binding_attempts <
            min_software_surface_cache_refresh_repeat_binding_attempts) {
            min_software_surface_cache_refresh_repeat_binding_attempts =
                sample->software_surface_cache_refresh_repeat_binding_attempts;
        }
        if (sample->software_surface_cache_refresh_repeat_binding_attempts >
            max_software_surface_cache_refresh_repeat_binding_attempts) {
            max_software_surface_cache_refresh_repeat_binding_attempts =
                sample->software_surface_cache_refresh_repeat_binding_attempts;
        }
        if (sample->software_surface_cache_refresh_unique_texture_handles <
            min_software_surface_cache_refresh_unique_texture_handles) {
            min_software_surface_cache_refresh_unique_texture_handles =
                sample->software_surface_cache_refresh_unique_texture_handles;
        }
        if (sample->software_surface_cache_refresh_unique_texture_handles >
            max_software_surface_cache_refresh_unique_texture_handles) {
            max_software_surface_cache_refresh_unique_texture_handles =
                sample->software_surface_cache_refresh_unique_texture_handles;
        }
        if (sample->software_surface_cache_refresh_texture_handle_fanout_max <
            min_software_surface_cache_refresh_texture_handle_fanout_max) {
            min_software_surface_cache_refresh_texture_handle_fanout_max =
                sample->software_surface_cache_refresh_texture_handle_fanout_max;
        }
        if (sample->software_surface_cache_refresh_texture_handle_fanout_max >
            max_software_surface_cache_refresh_texture_handle_fanout_max) {
            max_software_surface_cache_refresh_texture_handle_fanout_max =
                sample->software_surface_cache_refresh_texture_handle_fanout_max;
        }
        if (sample->software_surface_cache_refresh_failures < min_software_surface_cache_refresh_failures) {
            min_software_surface_cache_refresh_failures = sample->software_surface_cache_refresh_failures;
        }
        if (sample->software_surface_cache_refresh_failures > max_software_surface_cache_refresh_failures) {
            max_software_surface_cache_refresh_failures = sample->software_surface_cache_refresh_failures;
        }
        if (sample->software_surface_cache_refresh_ms < min_software_surface_cache_refresh_ms) {
            min_software_surface_cache_refresh_ms = sample->software_surface_cache_refresh_ms;
        }
        if (sample->software_surface_cache_refresh_ms > max_software_surface_cache_refresh_ms) {
            max_software_surface_cache_refresh_ms = sample->software_surface_cache_refresh_ms;
        }
        if (sample->software_surface_cache_refresh_palette_set_calls <
            min_software_surface_cache_refresh_palette_set_calls) {
            min_software_surface_cache_refresh_palette_set_calls =
                sample->software_surface_cache_refresh_palette_set_calls;
        }
        if (sample->software_surface_cache_refresh_palette_set_calls >
            max_software_surface_cache_refresh_palette_set_calls) {
            max_software_surface_cache_refresh_palette_set_calls =
                sample->software_surface_cache_refresh_palette_set_calls;
        }
        if (sample->software_surface_cache_refresh_palette_set_ms <
            min_software_surface_cache_refresh_palette_set_ms) {
            min_software_surface_cache_refresh_palette_set_ms = sample->software_surface_cache_refresh_palette_set_ms;
        }
        if (sample->software_surface_cache_refresh_palette_set_ms >
            max_software_surface_cache_refresh_palette_set_ms) {
            max_software_surface_cache_refresh_palette_set_ms = sample->software_surface_cache_refresh_palette_set_ms;
        }
        if (sample->software_surface_cache_refresh_blit_calls < min_software_surface_cache_refresh_blit_calls) {
            min_software_surface_cache_refresh_blit_calls = sample->software_surface_cache_refresh_blit_calls;
        }
        if (sample->software_surface_cache_refresh_blit_calls > max_software_surface_cache_refresh_blit_calls) {
            max_software_surface_cache_refresh_blit_calls = sample->software_surface_cache_refresh_blit_calls;
        }
        if (sample->software_surface_cache_refresh_blit_ms < min_software_surface_cache_refresh_blit_ms) {
            min_software_surface_cache_refresh_blit_ms = sample->software_surface_cache_refresh_blit_ms;
        }
        if (sample->software_surface_cache_refresh_blit_ms > max_software_surface_cache_refresh_blit_ms) {
            max_software_surface_cache_refresh_blit_ms = sample->software_surface_cache_refresh_blit_ms;
        }
        if (sample->software_surface_cache_create_dirty_texture_same_frame <
            min_software_surface_cache_create_dirty_texture_same_frame) {
            min_software_surface_cache_create_dirty_texture_same_frame =
                sample->software_surface_cache_create_dirty_texture_same_frame;
        }
        if (sample->software_surface_cache_create_dirty_texture_same_frame >
            max_software_surface_cache_create_dirty_texture_same_frame) {
            max_software_surface_cache_create_dirty_texture_same_frame =
                sample->software_surface_cache_create_dirty_texture_same_frame;
        }
        if (sample->software_surface_cache_create_dirty_texture_carried <
            min_software_surface_cache_create_dirty_texture_carried) {
            min_software_surface_cache_create_dirty_texture_carried =
                sample->software_surface_cache_create_dirty_texture_carried;
        }
        if (sample->software_surface_cache_create_dirty_texture_carried >
            max_software_surface_cache_create_dirty_texture_carried) {
            max_software_surface_cache_create_dirty_texture_carried =
                sample->software_surface_cache_create_dirty_texture_carried;
        }
        if (sample->software_surface_cache_create_dirty_palette_same_frame <
            min_software_surface_cache_create_dirty_palette_same_frame) {
            min_software_surface_cache_create_dirty_palette_same_frame =
                sample->software_surface_cache_create_dirty_palette_same_frame;
        }
        if (sample->software_surface_cache_create_dirty_palette_same_frame >
            max_software_surface_cache_create_dirty_palette_same_frame) {
            max_software_surface_cache_create_dirty_palette_same_frame =
                sample->software_surface_cache_create_dirty_palette_same_frame;
        }
        if (sample->software_surface_cache_create_dirty_palette_carried <
            min_software_surface_cache_create_dirty_palette_carried) {
            min_software_surface_cache_create_dirty_palette_carried =
                sample->software_surface_cache_create_dirty_palette_carried;
        }
        if (sample->software_surface_cache_create_dirty_palette_carried >
            max_software_surface_cache_create_dirty_palette_carried) {
            max_software_surface_cache_create_dirty_palette_carried =
                sample->software_surface_cache_create_dirty_palette_carried;
        }
        if (sample->software_surface_cache_create_cold < min_software_surface_cache_create_cold) {
            min_software_surface_cache_create_cold = sample->software_surface_cache_create_cold;
        }
        if (sample->software_surface_cache_create_cold > max_software_surface_cache_create_cold) {
            max_software_surface_cache_create_cold = sample->software_surface_cache_create_cold;
        }
        if (sample->software_surface_cache_texture_evictions < min_software_surface_cache_texture_evictions) {
            min_software_surface_cache_texture_evictions = sample->software_surface_cache_texture_evictions;
        }
        if (sample->software_surface_cache_texture_evictions > max_software_surface_cache_texture_evictions) {
            max_software_surface_cache_texture_evictions = sample->software_surface_cache_texture_evictions;
        }
        if (sample->software_surface_cache_palette_evictions < min_software_surface_cache_palette_evictions) {
            min_software_surface_cache_palette_evictions = sample->software_surface_cache_palette_evictions;
        }
        if (sample->software_surface_cache_palette_evictions > max_software_surface_cache_palette_evictions) {
            max_software_surface_cache_palette_evictions = sample->software_surface_cache_palette_evictions;
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
        if (sample->software_frame_candidate_tasks < min_software_frame_candidate_tasks) {
            min_software_frame_candidate_tasks = sample->software_frame_candidate_tasks;
        }
        if (sample->software_frame_candidate_tasks > max_software_frame_candidate_tasks) {
            max_software_frame_candidate_tasks = sample->software_frame_candidate_tasks;
        }
        if (sample->software_frame_candidate_pixels < min_software_frame_candidate_pixels) {
            min_software_frame_candidate_pixels = sample->software_frame_candidate_pixels;
        }
        if (sample->software_frame_candidate_pixels > max_software_frame_candidate_pixels) {
            max_software_frame_candidate_pixels = sample->software_frame_candidate_pixels;
        }
        if (sample->software_frame_fallback_tasks < min_software_frame_fallback_tasks) {
            min_software_frame_fallback_tasks = sample->software_frame_fallback_tasks;
        }
        if (sample->software_frame_fallback_tasks > max_software_frame_fallback_tasks) {
            max_software_frame_fallback_tasks = sample->software_frame_fallback_tasks;
        }
        if (sample->software_frame_fallback_pixels < min_software_frame_fallback_pixels) {
            min_software_frame_fallback_pixels = sample->software_frame_fallback_pixels;
        }
        if (sample->software_frame_fallback_pixels > max_software_frame_fallback_pixels) {
            max_software_frame_fallback_pixels = sample->software_frame_fallback_pixels;
        }
        if (sample->software_frame_fast_exact_tasks < min_software_frame_fast_exact_tasks) {
            min_software_frame_fast_exact_tasks = sample->software_frame_fast_exact_tasks;
        }
        if (sample->software_frame_fast_exact_tasks > max_software_frame_fast_exact_tasks) {
            max_software_frame_fast_exact_tasks = sample->software_frame_fast_exact_tasks;
        }
        if (sample->software_frame_fast_exact_pixels < min_software_frame_fast_exact_pixels) {
            min_software_frame_fast_exact_pixels = sample->software_frame_fast_exact_pixels;
        }
        if (sample->software_frame_fast_exact_pixels > max_software_frame_fast_exact_pixels) {
            max_software_frame_fast_exact_pixels = sample->software_frame_fast_exact_pixels;
        }
        if (sample->software_frame_fast_exact_clipped_tasks < min_software_frame_fast_exact_clipped_tasks) {
            min_software_frame_fast_exact_clipped_tasks = sample->software_frame_fast_exact_clipped_tasks;
        }
        if (sample->software_frame_fast_exact_clipped_tasks > max_software_frame_fast_exact_clipped_tasks) {
            max_software_frame_fast_exact_clipped_tasks = sample->software_frame_fast_exact_clipped_tasks;
        }
        if (sample->software_frame_fast_exact_flipped_tasks < min_software_frame_fast_exact_flipped_tasks) {
            min_software_frame_fast_exact_flipped_tasks = sample->software_frame_fast_exact_flipped_tasks;
        }
        if (sample->software_frame_fast_exact_flipped_tasks > max_software_frame_fast_exact_flipped_tasks) {
            max_software_frame_fast_exact_flipped_tasks = sample->software_frame_fast_exact_flipped_tasks;
        }
        if (sample->software_frame_fast_exact_color_mod_tasks < min_software_frame_fast_exact_color_mod_tasks) {
            min_software_frame_fast_exact_color_mod_tasks = sample->software_frame_fast_exact_color_mod_tasks;
        }
        if (sample->software_frame_fast_exact_color_mod_tasks > max_software_frame_fast_exact_color_mod_tasks) {
            max_software_frame_fast_exact_color_mod_tasks = sample->software_frame_fast_exact_color_mod_tasks;
        }
        if (sample->software_frame_fast_exact_color_mod_pixels < min_software_frame_fast_exact_color_mod_pixels) {
            min_software_frame_fast_exact_color_mod_pixels = sample->software_frame_fast_exact_color_mod_pixels;
        }
        if (sample->software_frame_fast_exact_color_mod_pixels > max_software_frame_fast_exact_color_mod_pixels) {
            max_software_frame_fast_exact_color_mod_pixels = sample->software_frame_fast_exact_color_mod_pixels;
        }
        if (sample->software_frame_fast_scaled_tasks < min_software_frame_fast_scaled_tasks) {
            min_software_frame_fast_scaled_tasks = sample->software_frame_fast_scaled_tasks;
        }
        if (sample->software_frame_fast_scaled_tasks > max_software_frame_fast_scaled_tasks) {
            max_software_frame_fast_scaled_tasks = sample->software_frame_fast_scaled_tasks;
        }
        if (sample->software_frame_fast_scaled_pixels < min_software_frame_fast_scaled_pixels) {
            min_software_frame_fast_scaled_pixels = sample->software_frame_fast_scaled_pixels;
        }
        if (sample->software_frame_fast_scaled_pixels > max_software_frame_fast_scaled_pixels) {
            max_software_frame_fast_scaled_pixels = sample->software_frame_fast_scaled_pixels;
        }
        if (sample->software_frame_fast_non_integer_tasks < min_software_frame_fast_non_integer_tasks) {
            min_software_frame_fast_non_integer_tasks = sample->software_frame_fast_non_integer_tasks;
        }
        if (sample->software_frame_fast_non_integer_tasks > max_software_frame_fast_non_integer_tasks) {
            max_software_frame_fast_non_integer_tasks = sample->software_frame_fast_non_integer_tasks;
        }
        if (sample->software_frame_fast_non_integer_pixels < min_software_frame_fast_non_integer_pixels) {
            min_software_frame_fast_non_integer_pixels = sample->software_frame_fast_non_integer_pixels;
        }
        if (sample->software_frame_fast_non_integer_pixels > max_software_frame_fast_non_integer_pixels) {
            max_software_frame_fast_non_integer_pixels = sample->software_frame_fast_non_integer_pixels;
        }
        if (sample->software_frame_fast_non_integer_lookup_entries <
            min_software_frame_fast_non_integer_lookup_entries) {
            min_software_frame_fast_non_integer_lookup_entries =
                sample->software_frame_fast_non_integer_lookup_entries;
        }
        if (sample->software_frame_fast_non_integer_lookup_entries >
            max_software_frame_fast_non_integer_lookup_entries) {
            max_software_frame_fast_non_integer_lookup_entries =
                sample->software_frame_fast_non_integer_lookup_entries;
        }
        if (sample->software_frame_fast_non_integer_alpha_only_tasks <
            min_software_frame_fast_non_integer_alpha_only_tasks) {
            min_software_frame_fast_non_integer_alpha_only_tasks =
                sample->software_frame_fast_non_integer_alpha_only_tasks;
        }
        if (sample->software_frame_fast_non_integer_alpha_only_tasks >
            max_software_frame_fast_non_integer_alpha_only_tasks) {
            max_software_frame_fast_non_integer_alpha_only_tasks =
                sample->software_frame_fast_non_integer_alpha_only_tasks;
        }
        if (sample->software_frame_fast_non_integer_alpha_only_pixels <
            min_software_frame_fast_non_integer_alpha_only_pixels) {
            min_software_frame_fast_non_integer_alpha_only_pixels =
                sample->software_frame_fast_non_integer_alpha_only_pixels;
        }
        if (sample->software_frame_fast_non_integer_alpha_only_pixels >
            max_software_frame_fast_non_integer_alpha_only_pixels) {
            max_software_frame_fast_non_integer_alpha_only_pixels =
                sample->software_frame_fast_non_integer_alpha_only_pixels;
        }
        if (sample->software_frame_fast_non_integer_rgb_mod_tasks < min_software_frame_fast_non_integer_rgb_mod_tasks) {
            min_software_frame_fast_non_integer_rgb_mod_tasks = sample->software_frame_fast_non_integer_rgb_mod_tasks;
        }
        if (sample->software_frame_fast_non_integer_rgb_mod_tasks > max_software_frame_fast_non_integer_rgb_mod_tasks) {
            max_software_frame_fast_non_integer_rgb_mod_tasks = sample->software_frame_fast_non_integer_rgb_mod_tasks;
        }
        if (sample->software_frame_fast_non_integer_rgb_mod_pixels <
            min_software_frame_fast_non_integer_rgb_mod_pixels) {
            min_software_frame_fast_non_integer_rgb_mod_pixels =
                sample->software_frame_fast_non_integer_rgb_mod_pixels;
        }
        if (sample->software_frame_fast_non_integer_rgb_mod_pixels >
            max_software_frame_fast_non_integer_rgb_mod_pixels) {
            max_software_frame_fast_non_integer_rgb_mod_pixels =
                sample->software_frame_fast_non_integer_rgb_mod_pixels;
        }
        if (sample->software_frame_generic_textured_tasks < min_software_frame_generic_textured_tasks) {
            min_software_frame_generic_textured_tasks = sample->software_frame_generic_textured_tasks;
        }
        if (sample->software_frame_generic_textured_tasks > max_software_frame_generic_textured_tasks) {
            max_software_frame_generic_textured_tasks = sample->software_frame_generic_textured_tasks;
        }
        if (sample->software_frame_generic_textured_pixels < min_software_frame_generic_textured_pixels) {
            min_software_frame_generic_textured_pixels = sample->software_frame_generic_textured_pixels;
        }
        if (sample->software_frame_generic_textured_pixels > max_software_frame_generic_textured_pixels) {
            max_software_frame_generic_textured_pixels = sample->software_frame_generic_textured_pixels;
        }
        if (sample->software_frame_generic_textured_alpha_only_tasks <
            min_software_frame_generic_textured_alpha_only_tasks) {
            min_software_frame_generic_textured_alpha_only_tasks =
                sample->software_frame_generic_textured_alpha_only_tasks;
        }
        if (sample->software_frame_generic_textured_alpha_only_tasks >
            max_software_frame_generic_textured_alpha_only_tasks) {
            max_software_frame_generic_textured_alpha_only_tasks =
                sample->software_frame_generic_textured_alpha_only_tasks;
        }
        if (sample->software_frame_generic_textured_alpha_only_pixels <
            min_software_frame_generic_textured_alpha_only_pixels) {
            min_software_frame_generic_textured_alpha_only_pixels =
                sample->software_frame_generic_textured_alpha_only_pixels;
        }
        if (sample->software_frame_generic_textured_alpha_only_pixels >
            max_software_frame_generic_textured_alpha_only_pixels) {
            max_software_frame_generic_textured_alpha_only_pixels =
                sample->software_frame_generic_textured_alpha_only_pixels;
        }
        if (sample->software_frame_generic_textured_rgb_mod_tasks < min_software_frame_generic_textured_rgb_mod_tasks) {
            min_software_frame_generic_textured_rgb_mod_tasks = sample->software_frame_generic_textured_rgb_mod_tasks;
        }
        if (sample->software_frame_generic_textured_rgb_mod_tasks > max_software_frame_generic_textured_rgb_mod_tasks) {
            max_software_frame_generic_textured_rgb_mod_tasks = sample->software_frame_generic_textured_rgb_mod_tasks;
        }
        if (sample->software_frame_generic_textured_rgb_mod_pixels <
            min_software_frame_generic_textured_rgb_mod_pixels) {
            min_software_frame_generic_textured_rgb_mod_pixels =
                sample->software_frame_generic_textured_rgb_mod_pixels;
        }
        if (sample->software_frame_generic_textured_rgb_mod_pixels >
            max_software_frame_generic_textured_rgb_mod_pixels) {
            max_software_frame_generic_textured_rgb_mod_pixels =
                sample->software_frame_generic_textured_rgb_mod_pixels;
        }
        if (sample->software_frame_fast_miss_color_mod < min_software_frame_fast_miss_color_mod) {
            min_software_frame_fast_miss_color_mod = sample->software_frame_fast_miss_color_mod;
        }
        if (sample->software_frame_fast_miss_color_mod > max_software_frame_fast_miss_color_mod) {
            max_software_frame_fast_miss_color_mod = sample->software_frame_fast_miss_color_mod;
        }
        if (sample->software_frame_fast_miss_non_integer < min_software_frame_fast_miss_non_integer) {
            min_software_frame_fast_miss_non_integer = sample->software_frame_fast_miss_non_integer;
        }
        if (sample->software_frame_fast_miss_non_integer > max_software_frame_fast_miss_non_integer) {
            max_software_frame_fast_miss_non_integer = sample->software_frame_fast_miss_non_integer;
        }
        if (sample->software_frame_fast_miss_non_integer_lookup_entries <
            min_software_frame_fast_miss_non_integer_lookup_entries) {
            min_software_frame_fast_miss_non_integer_lookup_entries =
                sample->software_frame_fast_miss_non_integer_lookup_entries;
        }
        if (sample->software_frame_fast_miss_non_integer_lookup_entries >
            max_software_frame_fast_miss_non_integer_lookup_entries) {
            max_software_frame_fast_miss_non_integer_lookup_entries =
                sample->software_frame_fast_miss_non_integer_lookup_entries;
        }
        if (sample->software_frame_fast_miss_non_integer_ge_256_tasks <
            min_software_frame_fast_miss_non_integer_ge_256_tasks) {
            min_software_frame_fast_miss_non_integer_ge_256_tasks =
                sample->software_frame_fast_miss_non_integer_ge_256_tasks;
        }
        if (sample->software_frame_fast_miss_non_integer_ge_256_tasks >
            max_software_frame_fast_miss_non_integer_ge_256_tasks) {
            max_software_frame_fast_miss_non_integer_ge_256_tasks =
                sample->software_frame_fast_miss_non_integer_ge_256_tasks;
        }
        if (sample->software_frame_fast_miss_non_integer_ge_256_pixels <
            min_software_frame_fast_miss_non_integer_ge_256_pixels) {
            min_software_frame_fast_miss_non_integer_ge_256_pixels =
                sample->software_frame_fast_miss_non_integer_ge_256_pixels;
        }
        if (sample->software_frame_fast_miss_non_integer_ge_256_pixels >
            max_software_frame_fast_miss_non_integer_ge_256_pixels) {
            max_software_frame_fast_miss_non_integer_ge_256_pixels =
                sample->software_frame_fast_miss_non_integer_ge_256_pixels;
        }
        if (sample->software_frame_fast_miss_non_integer_ge_256_lookup_entries <
            min_software_frame_fast_miss_non_integer_ge_256_lookup_entries) {
            min_software_frame_fast_miss_non_integer_ge_256_lookup_entries =
                sample->software_frame_fast_miss_non_integer_ge_256_lookup_entries;
        }
        if (sample->software_frame_fast_miss_non_integer_ge_256_lookup_entries >
            max_software_frame_fast_miss_non_integer_ge_256_lookup_entries) {
            max_software_frame_fast_miss_non_integer_ge_256_lookup_entries =
                sample->software_frame_fast_miss_non_integer_ge_256_lookup_entries;
        }
        if (sample->software_frame_fast_miss_non_integer_ge_1024_tasks <
            min_software_frame_fast_miss_non_integer_ge_1024_tasks) {
            min_software_frame_fast_miss_non_integer_ge_1024_tasks =
                sample->software_frame_fast_miss_non_integer_ge_1024_tasks;
        }
        if (sample->software_frame_fast_miss_non_integer_ge_1024_tasks >
            max_software_frame_fast_miss_non_integer_ge_1024_tasks) {
            max_software_frame_fast_miss_non_integer_ge_1024_tasks =
                sample->software_frame_fast_miss_non_integer_ge_1024_tasks;
        }
        if (sample->software_frame_fast_miss_non_integer_ge_1024_pixels <
            min_software_frame_fast_miss_non_integer_ge_1024_pixels) {
            min_software_frame_fast_miss_non_integer_ge_1024_pixels =
                sample->software_frame_fast_miss_non_integer_ge_1024_pixels;
        }
        if (sample->software_frame_fast_miss_non_integer_ge_1024_pixels >
            max_software_frame_fast_miss_non_integer_ge_1024_pixels) {
            max_software_frame_fast_miss_non_integer_ge_1024_pixels =
                sample->software_frame_fast_miss_non_integer_ge_1024_pixels;
        }
        if (sample->software_frame_fast_miss_non_integer_max_pixels <
            min_software_frame_fast_miss_non_integer_max_pixels) {
            min_software_frame_fast_miss_non_integer_max_pixels =
                sample->software_frame_fast_miss_non_integer_max_pixels;
        }
        if (sample->software_frame_fast_miss_non_integer_max_pixels >
            max_software_frame_fast_miss_non_integer_max_pixels) {
            max_software_frame_fast_miss_non_integer_max_pixels =
                sample->software_frame_fast_miss_non_integer_max_pixels;
        }
        if (sample->software_frame_fast_miss_scaled < min_software_frame_fast_miss_scaled) {
            min_software_frame_fast_miss_scaled = sample->software_frame_fast_miss_scaled;
        }
        if (sample->software_frame_fast_miss_scaled > max_software_frame_fast_miss_scaled) {
            max_software_frame_fast_miss_scaled = sample->software_frame_fast_miss_scaled;
        }
        if (sample->software_frame_fast_miss_unsupported_flip < min_software_frame_fast_miss_unsupported_flip) {
            min_software_frame_fast_miss_unsupported_flip = sample->software_frame_fast_miss_unsupported_flip;
        }
        if (sample->software_frame_fast_miss_unsupported_flip > max_software_frame_fast_miss_unsupported_flip) {
            max_software_frame_fast_miss_unsupported_flip = sample->software_frame_fast_miss_unsupported_flip;
        }
        if (sample->software_frame_fast_miss_source_bounds < min_software_frame_fast_miss_source_bounds) {
            min_software_frame_fast_miss_source_bounds = sample->software_frame_fast_miss_source_bounds;
        }
        if (sample->software_frame_fast_miss_source_bounds > max_software_frame_fast_miss_source_bounds) {
            max_software_frame_fast_miss_source_bounds = sample->software_frame_fast_miss_source_bounds;
        }
        if (sample->software_frame_reason_alpha < min_software_frame_reason_alpha) {
            min_software_frame_reason_alpha = sample->software_frame_reason_alpha;
        }
        if (sample->software_frame_reason_alpha > max_software_frame_reason_alpha) {
            max_software_frame_reason_alpha = sample->software_frame_reason_alpha;
        }
        if (sample->software_frame_reason_color_mod < min_software_frame_reason_color_mod) {
            min_software_frame_reason_color_mod = sample->software_frame_reason_color_mod;
        }
        if (sample->software_frame_reason_color_mod > max_software_frame_reason_color_mod) {
            max_software_frame_reason_color_mod = sample->software_frame_reason_color_mod;
        }
        if (sample->software_frame_reason_geometry < min_software_frame_reason_geometry) {
            min_software_frame_reason_geometry = sample->software_frame_reason_geometry;
        }
        if (sample->software_frame_reason_geometry > max_software_frame_reason_geometry) {
            max_software_frame_reason_geometry = sample->software_frame_reason_geometry;
        }
        if (sample->software_frame_reason_solid < min_software_frame_reason_solid) {
            min_software_frame_reason_solid = sample->software_frame_reason_solid;
        }
        if (sample->software_frame_reason_solid > max_software_frame_reason_solid) {
            max_software_frame_reason_solid = sample->software_frame_reason_solid;
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
    io_printf(io, "  \"schema_version\": 45,\n");
    io_printf(io, "  \"scene\": \"");
    io_write_json_escaped_string(io, perf_capture_scene_name);
    io_printf(io, "\",\n");
    io_printf(io, "  \"detail_mode\": \"%s\",\n", perf_capture_basic_mode ? "basic" : "full");
    io_printf(io, "  \"frames\": %d,\n", perf_capture_recorded_frames);
    io_printf(io,
              "  \"test_state\": {\n"
              "    \"p1_super_art_stock_available_frames_total\": %llu,\n"
              "    \"p1_super_art_stock_available_ratio\": %.6f,\n",
              (unsigned long long)perf_super_art_stock_available_frames[0],
              frame_count > 0 ? (double)perf_super_art_stock_available_frames[0] / frame_count : 0.0);
    if (perf_super_art_stock_available_first_frame[0] >= 0) {
        io_printf(io, "    \"p1_super_art_stock_available_first_frame\": %d,\n",
                  perf_super_art_stock_available_first_frame[0]);
    } else {
        io_printf(io, "    \"p1_super_art_stock_available_first_frame\": null,\n");
    }
    io_printf(io,
              "    \"p1_super_art_max_store\": %d,\n"
              "    \"p1_super_art_store_max\": %d,\n"
              "    \"p1_super_art_gauge_max_value\": %d,\n"
              "    \"p1_super_art_gauge_max_capacity\": %d,\n"
              "    \"p1_super_art_gauge_max_ratio\": %.6f,\n",
              perf_super_art_stock_max_reached[0],
              perf_super_art_stock_capacity[0],
              perf_super_art_gauge_max_value[0],
              perf_super_art_gauge_capacity[0],
              perf_super_art_gauge_capacity[0] > 0
                  ? (double)perf_super_art_gauge_max_value[0] / (double)perf_super_art_gauge_capacity[0]
                  : 0.0);
    io_printf(io,
              "    \"p1_super_art_ready_frames_total\": %llu,\n"
              "    \"p1_super_art_ready_ratio\": %.6f,\n",
              (unsigned long long)perf_super_art_ready_frames[0],
              frame_count > 0 ? (double)perf_super_art_ready_frames[0] / frame_count : 0.0);
    if (perf_super_art_ready_first_frame[0] >= 0) {
        io_printf(io, "    \"p1_super_art_ready_first_frame\": %d,\n", perf_super_art_ready_first_frame[0]);
    } else {
        io_printf(io, "    \"p1_super_art_ready_first_frame\": null,\n");
    }
    io_printf(io, "    \"p1_super_art_ready_routine1_frames\": ");
    io_write_json_uint64_array(
        io, perf_super_art_ready_routine1_frames[0], SDL_arraysize(perf_super_art_ready_routine1_frames[0]));
    io_printf(io, ",\n");
    io_printf(io, "    \"p1_super_art_ready_first_routine\": ");
    io_write_json_super_art_ready_routine_or_null(io, perf_super_art_ready_first_routine[0]);
    io_printf(io, ",\n");
    io_printf(io, "    \"p1_super_art_ready_last_routine\": ");
    io_write_json_super_art_ready_routine_or_null(io, perf_super_art_ready_last_routine[0]);
    io_printf(io, ",\n");
    io_printf(io,
              "    \"p1_super_art_entry_calls_total\": %llu,\n"
              "    \"p1_super_art_entry_cmd_sel_calls_total\": %llu,\n"
              "    \"p1_super_art_entry_cmd_sel_not_ready_total\": %llu,\n"
              "    \"p1_super_art_entry_direct_calls_total\": %llu,\n"
              "    \"p1_super_art_command_check_calls_total\": %llu,\n"
              "    \"p1_super_art_command_ready_checks_total\": %llu,\n"
              "    \"p1_super_art_command_blocked_pcon_dp_total\": %llu,\n"
              "    \"p1_super_art_command_ground_candidate_checks_total\": %llu,\n"
              "    \"p1_super_art_command_ground_precondition_blocked_total\": %llu,\n"
              "    \"p1_super_art_command_ground_no_match_total\": %llu,\n"
              "    \"p1_super_art_command_air_candidate_checks_total\": %llu,\n"
              "    \"p1_super_art_command_air_precondition_blocked_total\": %llu,\n"
              "    \"p1_super_art_command_air_no_match_total\": %llu,\n"
              "    \"p1_super_art_command_matches_total\": %llu,\n",
              (unsigned long long)super_art_command_telemetry[0].entry_calls_total,
              (unsigned long long)super_art_command_telemetry[0].entry_cmd_sel_calls_total,
              (unsigned long long)super_art_command_telemetry[0].entry_cmd_sel_not_ready_total,
              (unsigned long long)super_art_command_telemetry[0].entry_direct_calls_total,
              (unsigned long long)super_art_command_telemetry[0].check_calls_total,
              (unsigned long long)super_art_command_telemetry[0].ready_checks_total,
              (unsigned long long)super_art_command_telemetry[0].blocked_pcon_dp_total,
              (unsigned long long)super_art_command_telemetry[0].ground_candidate_checks_total,
              (unsigned long long)super_art_command_telemetry[0].ground_precondition_blocked_total,
              (unsigned long long)super_art_command_telemetry[0].ground_no_match_total,
              (unsigned long long)super_art_command_telemetry[0].air_candidate_checks_total,
              (unsigned long long)super_art_command_telemetry[0].air_precondition_blocked_total,
              (unsigned long long)super_art_command_telemetry[0].air_no_match_total,
              (unsigned long long)super_art_command_telemetry[0].command_matches_total);
    io_printf(io,
              "    \"p1_super_art_active_frames_total\": %llu,\n"
              "    \"p1_super_art_active_ratio\": %.6f,\n",
              (unsigned long long)perf_super_art_active_frames[0],
              frame_count > 0 ? (double)perf_super_art_active_frames[0] / frame_count : 0.0);
    if (perf_super_art_active_first_frame[0] >= 0) {
        io_printf(io, "    \"p1_super_art_active_first_frame\": %d,\n", perf_super_art_active_first_frame[0]);
    } else {
        io_printf(io, "    \"p1_super_art_active_first_frame\": null,\n");
    }
    io_printf(io,
              "    \"p1_metamorphose_frames_total\": %llu,\n"
              "    \"p1_metamorphose_ratio\": %.6f,\n",
              (unsigned long long)perf_metamorphose_active_frames[0],
              frame_count > 0 ? (double)perf_metamorphose_active_frames[0] / frame_count : 0.0);
    if (perf_metamorphose_active_first_frame[0] >= 0) {
        io_printf(io, "    \"p1_metamorphose_first_frame\": %d,\n", perf_metamorphose_active_first_frame[0]);
    } else {
        io_printf(io, "    \"p1_metamorphose_first_frame\": null,\n");
    }
    io_printf(io,
              "    \"p2_super_art_stock_available_frames_total\": %llu,\n"
              "    \"p2_super_art_stock_available_ratio\": %.6f,\n",
              (unsigned long long)perf_super_art_stock_available_frames[1],
              frame_count > 0 ? (double)perf_super_art_stock_available_frames[1] / frame_count : 0.0);
    if (perf_super_art_stock_available_first_frame[1] >= 0) {
        io_printf(io, "    \"p2_super_art_stock_available_first_frame\": %d,\n",
                  perf_super_art_stock_available_first_frame[1]);
    } else {
        io_printf(io, "    \"p2_super_art_stock_available_first_frame\": null,\n");
    }
    io_printf(io,
              "    \"p2_super_art_max_store\": %d,\n"
              "    \"p2_super_art_store_max\": %d,\n"
              "    \"p2_super_art_gauge_max_value\": %d,\n"
              "    \"p2_super_art_gauge_max_capacity\": %d,\n"
              "    \"p2_super_art_gauge_max_ratio\": %.6f,\n",
              perf_super_art_stock_max_reached[1],
              perf_super_art_stock_capacity[1],
              perf_super_art_gauge_max_value[1],
              perf_super_art_gauge_capacity[1],
              perf_super_art_gauge_capacity[1] > 0
                  ? (double)perf_super_art_gauge_max_value[1] / (double)perf_super_art_gauge_capacity[1]
                  : 0.0);
    io_printf(io,
              "    \"p2_super_art_ready_frames_total\": %llu,\n"
              "    \"p2_super_art_ready_ratio\": %.6f,\n",
              (unsigned long long)perf_super_art_ready_frames[1],
              frame_count > 0 ? (double)perf_super_art_ready_frames[1] / frame_count : 0.0);
    if (perf_super_art_ready_first_frame[1] >= 0) {
        io_printf(io, "    \"p2_super_art_ready_first_frame\": %d,\n", perf_super_art_ready_first_frame[1]);
    } else {
        io_printf(io, "    \"p2_super_art_ready_first_frame\": null,\n");
    }
    io_printf(io, "    \"p2_super_art_ready_routine1_frames\": ");
    io_write_json_uint64_array(
        io, perf_super_art_ready_routine1_frames[1], SDL_arraysize(perf_super_art_ready_routine1_frames[1]));
    io_printf(io, ",\n");
    io_printf(io, "    \"p2_super_art_ready_first_routine\": ");
    io_write_json_super_art_ready_routine_or_null(io, perf_super_art_ready_first_routine[1]);
    io_printf(io, ",\n");
    io_printf(io, "    \"p2_super_art_ready_last_routine\": ");
    io_write_json_super_art_ready_routine_or_null(io, perf_super_art_ready_last_routine[1]);
    io_printf(io, ",\n");
    io_printf(io,
              "    \"p2_super_art_entry_calls_total\": %llu,\n"
              "    \"p2_super_art_entry_cmd_sel_calls_total\": %llu,\n"
              "    \"p2_super_art_entry_cmd_sel_not_ready_total\": %llu,\n"
              "    \"p2_super_art_entry_direct_calls_total\": %llu,\n"
              "    \"p2_super_art_command_check_calls_total\": %llu,\n"
              "    \"p2_super_art_command_ready_checks_total\": %llu,\n"
              "    \"p2_super_art_command_blocked_pcon_dp_total\": %llu,\n"
              "    \"p2_super_art_command_ground_candidate_checks_total\": %llu,\n"
              "    \"p2_super_art_command_ground_precondition_blocked_total\": %llu,\n"
              "    \"p2_super_art_command_ground_no_match_total\": %llu,\n"
              "    \"p2_super_art_command_air_candidate_checks_total\": %llu,\n"
              "    \"p2_super_art_command_air_precondition_blocked_total\": %llu,\n"
              "    \"p2_super_art_command_air_no_match_total\": %llu,\n"
              "    \"p2_super_art_command_matches_total\": %llu,\n",
              (unsigned long long)super_art_command_telemetry[1].entry_calls_total,
              (unsigned long long)super_art_command_telemetry[1].entry_cmd_sel_calls_total,
              (unsigned long long)super_art_command_telemetry[1].entry_cmd_sel_not_ready_total,
              (unsigned long long)super_art_command_telemetry[1].entry_direct_calls_total,
              (unsigned long long)super_art_command_telemetry[1].check_calls_total,
              (unsigned long long)super_art_command_telemetry[1].ready_checks_total,
              (unsigned long long)super_art_command_telemetry[1].blocked_pcon_dp_total,
              (unsigned long long)super_art_command_telemetry[1].ground_candidate_checks_total,
              (unsigned long long)super_art_command_telemetry[1].ground_precondition_blocked_total,
              (unsigned long long)super_art_command_telemetry[1].ground_no_match_total,
              (unsigned long long)super_art_command_telemetry[1].air_candidate_checks_total,
              (unsigned long long)super_art_command_telemetry[1].air_precondition_blocked_total,
              (unsigned long long)super_art_command_telemetry[1].air_no_match_total,
              (unsigned long long)super_art_command_telemetry[1].command_matches_total);
    io_printf(io,
              "    \"p2_super_art_active_frames_total\": %llu,\n"
              "    \"p2_super_art_active_ratio\": %.6f,\n",
              (unsigned long long)perf_super_art_active_frames[1],
              frame_count > 0 ? (double)perf_super_art_active_frames[1] / frame_count : 0.0);
    if (perf_super_art_active_first_frame[1] >= 0) {
        io_printf(io, "    \"p2_super_art_active_first_frame\": %d,\n", perf_super_art_active_first_frame[1]);
    } else {
        io_printf(io, "    \"p2_super_art_active_first_frame\": null,\n");
    }
    io_printf(io,
              "    \"p2_metamorphose_frames_total\": %llu,\n"
              "    \"p2_metamorphose_ratio\": %.6f,\n",
              (unsigned long long)perf_metamorphose_active_frames[1],
              frame_count > 0 ? (double)perf_metamorphose_active_frames[1] / frame_count : 0.0);
    if (perf_metamorphose_active_first_frame[1] >= 0) {
        io_printf(io, "    \"p2_metamorphose_first_frame\": %d\n", perf_metamorphose_active_first_frame[1]);
    } else {
        io_printf(io, "    \"p2_metamorphose_first_frame\": null\n");
    }
    io_printf(io, "  },\n");
    io_printf(io,
              "  \"transition_state\": {\n"
              "    \"capture_start_g_no\": [%d, %d, %d, %d],\n"
              "    \"capture_start_e_no\": [%d, %d, %d, %d],\n"
              "    \"capture_start_menu_task_condition\": %d,\n"
              "    \"capture_start_menu_task_r_no\": [%d, %d, %d, %d],\n"
              "    \"capture_start_break_into\": %d,\n"
              "    \"capture_start_hnc_num\": %d,\n"
              "    \"capture_start_exec_wipe\": %d,\n"
              "    \"capture_start_active_wipe_type\": %d,\n"
              "    \"capture_start_wipe_limit\": %d,\n"
              "    \"break_into_frames_total\": %llu,\n",
              perf_capture_start_g_no[0],
              perf_capture_start_g_no[1],
              perf_capture_start_g_no[2],
              perf_capture_start_g_no[3],
              perf_capture_start_e_no[0],
              perf_capture_start_e_no[1],
              perf_capture_start_e_no[2],
              perf_capture_start_e_no[3],
              perf_capture_start_menu_task_condition,
              perf_capture_start_menu_task_r_no[0],
              perf_capture_start_menu_task_r_no[1],
              perf_capture_start_menu_task_r_no[2],
              perf_capture_start_menu_task_r_no[3],
              perf_capture_start_break_into,
              perf_capture_start_hnc_num,
              perf_capture_start_exec_wipe,
              perf_capture_start_active_wipe_type,
              perf_capture_start_wipe_limit,
              (unsigned long long)perf_break_into_frames_total);
    if (perf_break_into_first_frame >= 0) {
        io_printf(io, "    \"break_into_first_frame\": %d,\n", perf_break_into_first_frame);
    } else {
        io_printf(io, "    \"break_into_first_frame\": null,\n");
    }
    io_printf(io,
              "    \"hnc_active_frames_total\": %llu,\n",
              (unsigned long long)perf_hnc_active_frames_total);
    if (perf_hnc_active_first_frame >= 0) {
        io_printf(io, "    \"hnc_active_first_frame\": %d,\n", perf_hnc_active_first_frame);
    } else {
        io_printf(io, "    \"hnc_active_first_frame\": null,\n");
    }
    io_printf(io,
              "    \"hnc_max_num\": %d,\n"
              "    \"wipe_type1_active_frames_total\": %llu,\n",
              perf_hnc_max_num,
              (unsigned long long)perf_wipe_type1_active_frames_total);
    if (perf_wipe_type1_active_first_frame >= 0) {
        io_printf(io, "    \"wipe_type1_active_first_frame\": %d,\n", perf_wipe_type1_active_first_frame);
    } else {
        io_printf(io, "    \"wipe_type1_active_first_frame\": null,\n");
    }
    io_printf(io, "    \"wipe_type1_max_limit\": %d\n", perf_wipe_type1_max_limit);
    io_printf(io, "  },\n");
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
    io_printf(io, "    \"texture_cache_miss_dirty_texture_same_frame\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_cache_miss_dirty_texture_same_frame,
              min_texture_cache_miss_dirty_texture_same_frame,
              max_texture_cache_miss_dirty_texture_same_frame);
    io_printf(io, "    \"texture_cache_miss_dirty_texture_carried\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_cache_miss_dirty_texture_carried,
              min_texture_cache_miss_dirty_texture_carried,
              max_texture_cache_miss_dirty_texture_carried);
    io_printf(io, "    \"texture_cache_miss_dirty_palette_same_frame\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_cache_miss_dirty_palette_same_frame,
              min_texture_cache_miss_dirty_palette_same_frame,
              max_texture_cache_miss_dirty_palette_same_frame);
    io_printf(io, "    \"texture_cache_miss_dirty_palette_carried\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_cache_miss_dirty_palette_carried,
              min_texture_cache_miss_dirty_palette_carried,
              max_texture_cache_miss_dirty_palette_carried);
    io_printf(io, "    \"texture_cache_miss_cold\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_cache_miss_cold,
              min_texture_cache_miss_cold,
              max_texture_cache_miss_cold);
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
    io_printf(io, "    \"palette_unlock_changed_calls\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_palette_unlock_changed_calls,
              min_palette_unlock_changed_calls,
              max_palette_unlock_changed_calls);
    io_printf(io, "    \"palette_unlock_unchanged_calls\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_palette_unlock_unchanged_calls,
              min_palette_unlock_unchanged_calls,
              max_palette_unlock_unchanged_calls);
    io_printf(io, "    \"texture_unlock_dirty_surface_variants\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_unlock_dirty_surface_variants,
              min_texture_unlock_dirty_surface_variants,
              max_texture_unlock_dirty_surface_variants);
    io_printf(io,
              "    \"texture_unlock_dirty_surface_variants_max\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_unlock_dirty_surface_variants_max,
              min_texture_unlock_dirty_surface_variants_max,
              max_texture_unlock_dirty_surface_variants_max);
    io_printf(io, "    \"palette_unlock_dirty_surface_variants\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_palette_unlock_dirty_surface_variants,
              min_palette_unlock_dirty_surface_variants,
              max_palette_unlock_dirty_surface_variants);
    io_printf(io,
              "    \"palette_unlock_dirty_surface_variants_max\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_palette_unlock_dirty_surface_variants_max,
              min_palette_unlock_dirty_surface_variants_max,
              max_palette_unlock_dirty_surface_variants_max);
    io_printf(io,
              "    \"texture_unlock_locality_index8_tracked\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_unlock_locality_index8_tracked,
              min_texture_unlock_locality_index8_tracked,
              max_texture_unlock_locality_index8_tracked);
    io_printf(io,
              "    \"texture_unlock_locality_index8_baseline_skips\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_unlock_locality_index8_baseline_skips,
              min_texture_unlock_locality_index8_baseline_skips,
              max_texture_unlock_locality_index8_baseline_skips);
    io_printf(io,
              "    \"texture_unlock_locality_index8_non_index8_skips\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_unlock_locality_index8_non_index8_skips,
              min_texture_unlock_locality_index8_non_index8_skips,
              max_texture_unlock_locality_index8_non_index8_skips);
    io_printf(io,
              "    \"texture_unlock_locality_index8_source_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_texture_unlock_locality_index8_source_pixels,
              (unsigned long long)min_texture_unlock_locality_index8_source_pixels,
              (unsigned long long)max_texture_unlock_locality_index8_source_pixels);
    io_printf(io,
              "    \"texture_unlock_locality_index8_changed_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_texture_unlock_locality_index8_changed_pixels,
              (unsigned long long)min_texture_unlock_locality_index8_changed_pixels,
              (unsigned long long)max_texture_unlock_locality_index8_changed_pixels);
    io_printf(io,
              "    \"texture_unlock_locality_index8_changed_rows\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_texture_unlock_locality_index8_changed_rows,
              (unsigned long long)min_texture_unlock_locality_index8_changed_rows,
              (unsigned long long)max_texture_unlock_locality_index8_changed_rows);
    io_printf(
        io,
        "    \"texture_unlock_locality_index8_changed_bbox_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
        avg_texture_unlock_locality_index8_changed_bbox_pixels,
        (unsigned long long)min_texture_unlock_locality_index8_changed_bbox_pixels,
        (unsigned long long)max_texture_unlock_locality_index8_changed_bbox_pixels);
    io_printf(io, "    \"texture_unlock_invalidation\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_texture_unlock_invalidation_ms,
              min_texture_unlock_invalidation_ms,
              max_texture_unlock_invalidation_ms);
    io_printf(io, "    \"palette_unlock_invalidation\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_palette_unlock_invalidation_ms,
              min_palette_unlock_invalidation_ms,
              max_palette_unlock_invalidation_ms);
    io_printf(io, "    \"texture_cache_evictions\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_texture_cache_evictions,
              min_texture_cache_evictions,
              max_texture_cache_evictions);
    io_printf(io, "    \"palette_cache_evictions\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_palette_cache_evictions,
              min_palette_cache_evictions,
              max_palette_cache_evictions);
    io_printf(io, "    \"software_surface_cache_hits\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_hits,
              min_software_surface_cache_hits,
              max_software_surface_cache_hits);
    io_printf(io, "    \"software_surface_cache_creates\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_creates,
              min_software_surface_cache_creates,
              max_software_surface_cache_creates);
    io_printf(io,
              "    \"software_surface_cache_refresh_attempts\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_refresh_attempts,
              min_software_surface_cache_refresh_attempts,
              max_software_surface_cache_refresh_attempts);
    io_printf(io,
              "    \"software_surface_cache_refresh_unique_bindings\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_refresh_unique_bindings,
              min_software_surface_cache_refresh_unique_bindings,
              max_software_surface_cache_refresh_unique_bindings);
    io_printf(io,
              "    \"software_surface_cache_refresh_repeat_binding_attempts\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_refresh_repeat_binding_attempts,
              min_software_surface_cache_refresh_repeat_binding_attempts,
              max_software_surface_cache_refresh_repeat_binding_attempts);
    io_printf(io,
              "    \"software_surface_cache_refresh_unique_texture_handles\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_refresh_unique_texture_handles,
              min_software_surface_cache_refresh_unique_texture_handles,
              max_software_surface_cache_refresh_unique_texture_handles);
    io_printf(
        io,
        "    \"software_surface_cache_refresh_texture_handle_fanout_max\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
        avg_software_surface_cache_refresh_texture_handle_fanout_max,
        min_software_surface_cache_refresh_texture_handle_fanout_max,
        max_software_surface_cache_refresh_texture_handle_fanout_max);
    io_printf(io,
              "    \"software_surface_cache_refresh_failures\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_refresh_failures,
              min_software_surface_cache_refresh_failures,
              max_software_surface_cache_refresh_failures);
    io_printf(io, "    \"software_surface_cache_refresh\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_software_surface_cache_refresh_ms,
              min_software_surface_cache_refresh_ms,
              max_software_surface_cache_refresh_ms);
    io_printf(io,
              "    \"software_surface_cache_refresh_palette_set_calls\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_refresh_palette_set_calls,
              min_software_surface_cache_refresh_palette_set_calls,
              max_software_surface_cache_refresh_palette_set_calls);
    io_printf(io,
              "    \"software_surface_cache_refresh_palette_set\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_software_surface_cache_refresh_palette_set_ms,
              min_software_surface_cache_refresh_palette_set_ms,
              max_software_surface_cache_refresh_palette_set_ms);
    io_printf(io,
              "    \"software_surface_cache_refresh_blit_calls\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_refresh_blit_calls,
              min_software_surface_cache_refresh_blit_calls,
              max_software_surface_cache_refresh_blit_calls);
    io_printf(io,
              "    \"software_surface_cache_refresh_blit\": {\"mean_ms\": %.4f, \"min_ms\": %.4f, \"max_ms\": %.4f},\n",
              avg_software_surface_cache_refresh_blit_ms,
              min_software_surface_cache_refresh_blit_ms,
              max_software_surface_cache_refresh_blit_ms);
    io_printf(io,
              "    \"software_surface_cache_create_dirty_texture_same_frame\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_create_dirty_texture_same_frame,
              min_software_surface_cache_create_dirty_texture_same_frame,
              max_software_surface_cache_create_dirty_texture_same_frame);
    io_printf(io,
              "    \"software_surface_cache_create_dirty_texture_carried\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_create_dirty_texture_carried,
              min_software_surface_cache_create_dirty_texture_carried,
              max_software_surface_cache_create_dirty_texture_carried);
    io_printf(io,
              "    \"software_surface_cache_create_dirty_palette_same_frame\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_create_dirty_palette_same_frame,
              min_software_surface_cache_create_dirty_palette_same_frame,
              max_software_surface_cache_create_dirty_palette_same_frame);
    io_printf(io,
              "    \"software_surface_cache_create_dirty_palette_carried\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_create_dirty_palette_carried,
              min_software_surface_cache_create_dirty_palette_carried,
              max_software_surface_cache_create_dirty_palette_carried);
    io_printf(io, "    \"software_surface_cache_create_cold\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_create_cold,
              min_software_surface_cache_create_cold,
              max_software_surface_cache_create_cold);
    io_printf(io, "    \"software_surface_cache_texture_evictions\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_texture_evictions,
              min_software_surface_cache_texture_evictions,
              max_software_surface_cache_texture_evictions);
    io_printf(io, "    \"software_surface_cache_palette_evictions\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_surface_cache_palette_evictions,
              min_software_surface_cache_palette_evictions,
              max_software_surface_cache_palette_evictions);
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
    io_printf(io, "    \"software_frame_mode_enabled\": {\"count\": %llu, \"ratio\": %.6f},\n",
              (unsigned long long)perf_software_frame_mode_enabled_frames,
              software_frame_mode_enabled_ratio);
    io_printf(io, "    \"software_frame_surface_ready\": {\"count\": %llu, \"ratio\": %.6f},\n",
              (unsigned long long)perf_software_frame_surface_ready_frames,
              software_frame_surface_ready_ratio);
    io_printf(io, "    \"software_frame_active_frames\": {\"count\": %llu, \"ratio\": %.6f},\n",
              (unsigned long long)perf_software_frame_owned_frames,
              software_frame_owned_ratio);
    io_printf(io, "    \"software_frame_owned_frames\": {\"count\": %llu, \"ratio\": %.6f},\n",
              (unsigned long long)perf_software_frame_owned_frames,
              software_frame_owned_ratio);
    io_printf(io, "    \"software_frame_direct_present_frames\": {\"count\": %llu, \"ratio\": %.6f},\n",
              (unsigned long long)perf_software_frame_direct_present_frames,
              software_frame_direct_present_ratio);
    io_printf(io, "    \"software_frame_upload_frames\": {\"count\": %llu, \"ratio\": %.6f},\n",
              (unsigned long long)perf_software_frame_uploaded_frames,
              software_frame_uploaded_ratio);
    io_printf(io, "    \"software_frame_fallback_frames\": {\"count\": %llu, \"ratio\": %.6f},\n",
              (unsigned long long)perf_software_frame_fallback_frames,
              software_frame_fallback_ratio);
    io_printf(io, "    \"software_frame_candidate_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_candidate_tasks,
              min_software_frame_candidate_tasks,
              max_software_frame_candidate_tasks);
    io_printf(io, "    \"software_frame_candidate_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_candidate_pixels,
              (unsigned long long)min_software_frame_candidate_pixels,
              (unsigned long long)max_software_frame_candidate_pixels);
    io_printf(io, "    \"software_frame_fallback_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fallback_tasks,
              min_software_frame_fallback_tasks,
              max_software_frame_fallback_tasks);
    io_printf(io, "    \"software_frame_fallback_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fallback_pixels,
              (unsigned long long)min_software_frame_fallback_pixels,
              (unsigned long long)max_software_frame_fallback_pixels);
    io_printf(io, "    \"software_frame_fast_exact_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_exact_tasks,
              min_software_frame_fast_exact_tasks,
              max_software_frame_fast_exact_tasks);
    io_printf(io, "    \"software_frame_fast_exact_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_exact_pixels,
              (unsigned long long)min_software_frame_fast_exact_pixels,
              (unsigned long long)max_software_frame_fast_exact_pixels);
    io_printf(io,
              "    \"software_frame_fast_exact_clipped_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_exact_clipped_tasks,
              min_software_frame_fast_exact_clipped_tasks,
              max_software_frame_fast_exact_clipped_tasks);
    io_printf(io,
              "    \"software_frame_fast_exact_flipped_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_exact_flipped_tasks,
              min_software_frame_fast_exact_flipped_tasks,
              max_software_frame_fast_exact_flipped_tasks);
    io_printf(io,
              "    \"software_frame_fast_exact_color_mod_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_exact_color_mod_tasks,
              min_software_frame_fast_exact_color_mod_tasks,
              max_software_frame_fast_exact_color_mod_tasks);
    io_printf(io,
              "    \"software_frame_fast_exact_color_mod_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_exact_color_mod_pixels,
              (unsigned long long)min_software_frame_fast_exact_color_mod_pixels,
              (unsigned long long)max_software_frame_fast_exact_color_mod_pixels);
    io_printf(io, "    \"software_frame_fast_scaled_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_scaled_tasks,
              min_software_frame_fast_scaled_tasks,
              max_software_frame_fast_scaled_tasks);
    io_printf(io, "    \"software_frame_fast_scaled_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_scaled_pixels,
              (unsigned long long)min_software_frame_fast_scaled_pixels,
              (unsigned long long)max_software_frame_fast_scaled_pixels);
    io_printf(io,
              "    \"software_frame_fast_non_integer_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_non_integer_tasks,
              min_software_frame_fast_non_integer_tasks,
              max_software_frame_fast_non_integer_tasks);
    io_printf(io,
              "    \"software_frame_fast_non_integer_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_non_integer_pixels,
              (unsigned long long)min_software_frame_fast_non_integer_pixels,
              (unsigned long long)max_software_frame_fast_non_integer_pixels);
    io_printf(io,
              "    \"software_frame_fast_non_integer_lookup_entries\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_non_integer_lookup_entries,
              (unsigned long long)min_software_frame_fast_non_integer_lookup_entries,
              (unsigned long long)max_software_frame_fast_non_integer_lookup_entries);
    io_printf(io,
              "    \"software_frame_fast_non_integer_alpha_only_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_non_integer_alpha_only_tasks,
              min_software_frame_fast_non_integer_alpha_only_tasks,
              max_software_frame_fast_non_integer_alpha_only_tasks);
    io_printf(io,
              "    \"software_frame_fast_non_integer_alpha_only_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_non_integer_alpha_only_pixels,
              (unsigned long long)min_software_frame_fast_non_integer_alpha_only_pixels,
              (unsigned long long)max_software_frame_fast_non_integer_alpha_only_pixels);
    io_printf(io,
              "    \"software_frame_fast_non_integer_rgb_mod_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_non_integer_rgb_mod_tasks,
              min_software_frame_fast_non_integer_rgb_mod_tasks,
              max_software_frame_fast_non_integer_rgb_mod_tasks);
    io_printf(io,
              "    \"software_frame_fast_non_integer_rgb_mod_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_non_integer_rgb_mod_pixels,
              (unsigned long long)min_software_frame_fast_non_integer_rgb_mod_pixels,
              (unsigned long long)max_software_frame_fast_non_integer_rgb_mod_pixels);
    io_printf(io,
              "    \"software_frame_generic_textured_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_generic_textured_tasks,
              min_software_frame_generic_textured_tasks,
              max_software_frame_generic_textured_tasks);
    io_printf(io,
              "    \"software_frame_generic_textured_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_generic_textured_pixels,
              (unsigned long long)min_software_frame_generic_textured_pixels,
              (unsigned long long)max_software_frame_generic_textured_pixels);
    io_printf(io,
              "    \"software_frame_generic_textured_alpha_only_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_generic_textured_alpha_only_tasks,
              min_software_frame_generic_textured_alpha_only_tasks,
              max_software_frame_generic_textured_alpha_only_tasks);
    io_printf(io,
              "    \"software_frame_generic_textured_alpha_only_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_generic_textured_alpha_only_pixels,
              (unsigned long long)min_software_frame_generic_textured_alpha_only_pixels,
              (unsigned long long)max_software_frame_generic_textured_alpha_only_pixels);
    io_printf(io,
              "    \"software_frame_generic_textured_rgb_mod_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_generic_textured_rgb_mod_tasks,
              min_software_frame_generic_textured_rgb_mod_tasks,
              max_software_frame_generic_textured_rgb_mod_tasks);
    io_printf(io,
              "    \"software_frame_generic_textured_rgb_mod_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_generic_textured_rgb_mod_pixels,
              (unsigned long long)min_software_frame_generic_textured_rgb_mod_pixels,
              (unsigned long long)max_software_frame_generic_textured_rgb_mod_pixels);
    io_printf(io, "    \"software_frame_fast_miss_color_mod\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_miss_color_mod,
              min_software_frame_fast_miss_color_mod,
              max_software_frame_fast_miss_color_mod);
    io_printf(io, "    \"software_frame_fast_miss_non_integer\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_miss_non_integer,
              min_software_frame_fast_miss_non_integer,
              max_software_frame_fast_miss_non_integer);
    io_printf(io,
              "    \"software_frame_fast_miss_non_integer_lookup_entries\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_miss_non_integer_lookup_entries,
              (unsigned long long)min_software_frame_fast_miss_non_integer_lookup_entries,
              (unsigned long long)max_software_frame_fast_miss_non_integer_lookup_entries);
    io_printf(io,
              "    \"software_frame_fast_miss_non_integer_ge_256_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_miss_non_integer_ge_256_tasks,
              min_software_frame_fast_miss_non_integer_ge_256_tasks,
              max_software_frame_fast_miss_non_integer_ge_256_tasks);
    io_printf(io,
              "    \"software_frame_fast_miss_non_integer_ge_256_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_miss_non_integer_ge_256_pixels,
              (unsigned long long)min_software_frame_fast_miss_non_integer_ge_256_pixels,
              (unsigned long long)max_software_frame_fast_miss_non_integer_ge_256_pixels);
    io_printf(io,
              "    \"software_frame_fast_miss_non_integer_ge_256_lookup_entries\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_miss_non_integer_ge_256_lookup_entries,
              (unsigned long long)min_software_frame_fast_miss_non_integer_ge_256_lookup_entries,
              (unsigned long long)max_software_frame_fast_miss_non_integer_ge_256_lookup_entries);
    io_printf(io,
              "    \"software_frame_fast_miss_non_integer_ge_1024_tasks\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_miss_non_integer_ge_1024_tasks,
              min_software_frame_fast_miss_non_integer_ge_1024_tasks,
              max_software_frame_fast_miss_non_integer_ge_1024_tasks);
    io_printf(io,
              "    \"software_frame_fast_miss_non_integer_ge_1024_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_miss_non_integer_ge_1024_pixels,
              (unsigned long long)min_software_frame_fast_miss_non_integer_ge_1024_pixels,
              (unsigned long long)max_software_frame_fast_miss_non_integer_ge_1024_pixels);
    io_printf(io,
              "    \"software_frame_fast_miss_non_integer_max_pixels\": {\"mean\": %.2f, \"min\": %llu, \"max\": %llu},\n",
              avg_software_frame_fast_miss_non_integer_max_pixels,
              (unsigned long long)min_software_frame_fast_miss_non_integer_max_pixels,
              (unsigned long long)max_software_frame_fast_miss_non_integer_max_pixels);
    io_printf(io, "    \"software_frame_fast_miss_scaled\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_miss_scaled,
              min_software_frame_fast_miss_scaled,
              max_software_frame_fast_miss_scaled);
    io_printf(io,
              "    \"software_frame_fast_miss_unsupported_flip\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_miss_unsupported_flip,
              min_software_frame_fast_miss_unsupported_flip,
              max_software_frame_fast_miss_unsupported_flip);
    io_printf(io,
              "    \"software_frame_fast_miss_source_bounds\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_fast_miss_source_bounds,
              min_software_frame_fast_miss_source_bounds,
              max_software_frame_fast_miss_source_bounds);
    io_printf(io, "    \"software_frame_reason_alpha\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_reason_alpha,
              min_software_frame_reason_alpha,
              max_software_frame_reason_alpha);
    io_printf(io, "    \"software_frame_reason_color_mod\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_reason_color_mod,
              min_software_frame_reason_color_mod,
              max_software_frame_reason_color_mod);
    io_printf(io, "    \"software_frame_reason_geometry\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_reason_geometry,
              min_software_frame_reason_geometry,
              max_software_frame_reason_geometry);
    io_printf(io, "    \"software_frame_reason_solid\": {\"mean\": %.2f, \"min\": %d, \"max\": %d},\n",
              avg_software_frame_reason_solid,
              min_software_frame_reason_solid,
              max_software_frame_reason_solid);
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
              "\"software_frame_exact\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"software_frame_integer_scale\": {\"count\": %llu, \"ratio\": %.6f}, "
              "\"software_frame_mapped_scale\": {\"count\": %llu, \"ratio\": %.6f}, "
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
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_EXACT],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_EXACT] / frame_count,
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_INTEGER_SCALE],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_INTEGER_SCALE] / frame_count,
              (unsigned long long)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_MAPPED_SCALE],
              (double)perf_fbdev_path_frames[FBDEV_PRESENTER_PATH_SOFTWARE_FRAME_MAPPED_SCALE] / frame_count,
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
    io_printf(io,
              "    \"software_surface_cache_refresh_source_formats\": {"
              "\"index4lsb\": {\"attempts_total\": %llu, \"attempts_mean\": %.4f, \"pixels_total\": %llu, "
              "\"pixels_mean\": %.2f}, "
              "\"index8\": {\"attempts_total\": %llu, \"attempts_mean\": %.4f, \"pixels_total\": %llu, "
              "\"pixels_mean\": %.2f}, "
              "\"abgr1555\": {\"attempts_total\": %llu, \"attempts_mean\": %.4f, \"pixels_total\": %llu, "
              "\"pixels_mean\": %.2f}, "
              "\"other\": {\"attempts_total\": %llu, \"attempts_mean\": %.4f, \"pixels_total\": %llu, "
              "\"pixels_mean\": %.2f}},\n",
              (unsigned long long)refresh_telemetry.index4_attempts,
              (double)refresh_telemetry.index4_attempts / frame_count,
              (unsigned long long)refresh_telemetry.index4_pixels,
              (double)refresh_telemetry.index4_pixels / frame_count,
              (unsigned long long)refresh_telemetry.index8_attempts,
              (double)refresh_telemetry.index8_attempts / frame_count,
              (unsigned long long)refresh_telemetry.index8_pixels,
              (double)refresh_telemetry.index8_pixels / frame_count,
              (unsigned long long)refresh_telemetry.abgr1555_attempts,
              (double)refresh_telemetry.abgr1555_attempts / frame_count,
              (unsigned long long)refresh_telemetry.abgr1555_pixels,
              (double)refresh_telemetry.abgr1555_pixels / frame_count,
              (unsigned long long)refresh_telemetry.other_attempts,
              (double)refresh_telemetry.other_attempts / frame_count,
              (unsigned long long)refresh_telemetry.other_pixels,
              (double)refresh_telemetry.other_pixels / frame_count);
    io_printf(io,
              "    \"software_surface_cache_refresh_actual_work\": {"
              "\"partial_attempts_total\": %llu, \"partial_attempts_mean\": %.4f, "
              "\"partial_attempt_ratio\": %.6f, \"partial_pixels_total\": %llu, "
              "\"partial_pixels_mean\": %.2f, "
              "\"full_attempts_total\": %llu, \"full_attempts_mean\": %.4f, "
              "\"full_attempt_ratio\": %.6f, \"full_pixels_total\": %llu, "
              "\"full_pixels_mean\": %.2f, "
              "\"full_non_texture_dirty_attempts_total\": %llu, "
              "\"full_non_texture_dirty_attempts_mean\": %.4f, "
              "\"full_ineligible_source_attempts_total\": %llu, "
              "\"full_ineligible_source_attempts_mean\": %.4f, "
              "\"full_no_usable_dirty_rect_attempts_total\": %llu, "
              "\"full_no_usable_dirty_rect_attempts_mean\": %.4f, "
              "\"full_oversized_dirty_rect_attempts_total\": %llu, "
              "\"full_oversized_dirty_rect_attempts_mean\": %.4f},\n",
              (unsigned long long)refresh_telemetry.partial_attempts,
              (double)refresh_telemetry.partial_attempts / frame_count,
              refresh_attempts_total > 0 ? (double)refresh_telemetry.partial_attempts / (double)refresh_attempts_total
                                         : 0.0,
              (unsigned long long)refresh_telemetry.partial_pixels,
              (double)refresh_telemetry.partial_pixels / frame_count,
              (unsigned long long)refresh_telemetry.full_attempts,
              (double)refresh_telemetry.full_attempts / frame_count,
              refresh_attempts_total > 0 ? (double)refresh_telemetry.full_attempts / (double)refresh_attempts_total
                                         : 0.0,
              (unsigned long long)refresh_telemetry.full_pixels,
              (double)refresh_telemetry.full_pixels / frame_count,
              (unsigned long long)refresh_telemetry.full_non_texture_dirty_attempts,
              (double)refresh_telemetry.full_non_texture_dirty_attempts / frame_count,
              (unsigned long long)refresh_telemetry.full_ineligible_source_attempts,
              (double)refresh_telemetry.full_ineligible_source_attempts / frame_count,
              (unsigned long long)refresh_telemetry.full_no_usable_dirty_rect_attempts,
              (double)refresh_telemetry.full_no_usable_dirty_rect_attempts / frame_count,
              (unsigned long long)refresh_telemetry.full_oversized_dirty_rect_attempts,
              (double)refresh_telemetry.full_oversized_dirty_rect_attempts / frame_count);
    io_printf(io,
              "    \"software_surface_cache_refresh_blit_sampling\": {"
              "\"period\": %llu, "
              "\"sampled_blit_calls_total\": %llu, \"sampled_blit_calls_mean\": %.4f, "
              "\"sampled_blit_attempt_ratio\": %.6f, "
              "\"sampled_blit_total_ms\": %.4f, \"sampled_blit_mean_ms_per_sample\": %.6f, "
              "\"sampled_full_blit_calls_total\": %llu, "
              "\"sampled_full_blit_attempt_ratio\": %.6f, "
              "\"sampled_full_blit_total_ms\": %.4f, "
              "\"sampled_full_blit_mean_ms_per_sample\": %.6f, "
              "\"sampled_full_non_texture_dirty_blit_calls_total\": %llu, "
              "\"sampled_full_non_texture_dirty_blit_attempt_ratio\": %.6f, "
              "\"sampled_full_non_texture_dirty_blit_total_ms\": %.4f, "
              "\"sampled_full_non_texture_dirty_blit_mean_ms_per_sample\": %.6f, "
              "\"sampled_full_ineligible_source_blit_calls_total\": %llu, "
              "\"sampled_full_ineligible_source_blit_attempt_ratio\": %.6f, "
              "\"sampled_full_ineligible_source_blit_total_ms\": %.4f, "
              "\"sampled_full_ineligible_source_blit_mean_ms_per_sample\": %.6f, "
              "\"sampled_full_no_usable_dirty_rect_blit_calls_total\": %llu, "
              "\"sampled_full_no_usable_dirty_rect_blit_attempt_ratio\": %.6f, "
              "\"sampled_full_no_usable_dirty_rect_blit_total_ms\": %.4f, "
              "\"sampled_full_no_usable_dirty_rect_blit_mean_ms_per_sample\": %.6f, "
              "\"sampled_full_oversized_dirty_rect_blit_calls_total\": %llu, "
              "\"sampled_full_oversized_dirty_rect_blit_attempt_ratio\": %.6f, "
              "\"sampled_full_oversized_dirty_rect_blit_total_ms\": %.4f, "
              "\"sampled_full_oversized_dirty_rect_blit_mean_ms_per_sample\": %.6f, "
              "\"sampled_partial_blit_calls_total\": %llu, "
              "\"sampled_partial_blit_attempt_ratio\": %.6f, "
              "\"sampled_partial_blit_total_ms\": %.4f, "
              "\"sampled_partial_blit_mean_ms_per_sample\": %.6f},\n",
              (unsigned long long)refresh_telemetry.sampled_blit_period,
              (unsigned long long)refresh_telemetry.sampled_blit_calls,
              (double)refresh_telemetry.sampled_blit_calls / frame_count,
              refresh_attempts_total > 0 ? (double)refresh_telemetry.sampled_blit_calls / (double)refresh_attempts_total
                                         : 0.0,
              sampled_refresh_blit_total_ms,
              sampled_refresh_blit_mean_ms,
              (unsigned long long)refresh_telemetry.sampled_full_blit_calls,
              refresh_telemetry.full_attempts > 0
                  ? (double)refresh_telemetry.sampled_full_blit_calls / (double)refresh_telemetry.full_attempts
                  : 0.0,
              sampled_refresh_full_blit_total_ms,
              sampled_refresh_full_blit_mean_ms,
              (unsigned long long)refresh_telemetry.sampled_full_non_texture_dirty_blit_calls,
              refresh_telemetry.full_non_texture_dirty_attempts > 0
                  ? (double)refresh_telemetry.sampled_full_non_texture_dirty_blit_calls /
                        (double)refresh_telemetry.full_non_texture_dirty_attempts
                  : 0.0,
              sampled_refresh_full_non_texture_dirty_blit_total_ms,
              sampled_refresh_full_non_texture_dirty_blit_mean_ms,
              (unsigned long long)refresh_telemetry.sampled_full_ineligible_source_blit_calls,
              refresh_telemetry.full_ineligible_source_attempts > 0
                  ? (double)refresh_telemetry.sampled_full_ineligible_source_blit_calls /
                        (double)refresh_telemetry.full_ineligible_source_attempts
                  : 0.0,
              sampled_refresh_full_ineligible_source_blit_total_ms,
              sampled_refresh_full_ineligible_source_blit_mean_ms,
              (unsigned long long)refresh_telemetry.sampled_full_no_usable_dirty_rect_blit_calls,
              refresh_telemetry.full_no_usable_dirty_rect_attempts > 0
                  ? (double)refresh_telemetry.sampled_full_no_usable_dirty_rect_blit_calls /
                        (double)refresh_telemetry.full_no_usable_dirty_rect_attempts
                  : 0.0,
              sampled_refresh_full_no_usable_dirty_rect_blit_total_ms,
              sampled_refresh_full_no_usable_dirty_rect_blit_mean_ms,
              (unsigned long long)refresh_telemetry.sampled_full_oversized_dirty_rect_blit_calls,
              refresh_telemetry.full_oversized_dirty_rect_attempts > 0
                  ? (double)refresh_telemetry.sampled_full_oversized_dirty_rect_blit_calls /
                        (double)refresh_telemetry.full_oversized_dirty_rect_attempts
                  : 0.0,
              sampled_refresh_full_oversized_dirty_rect_blit_total_ms,
              sampled_refresh_full_oversized_dirty_rect_blit_mean_ms,
              (unsigned long long)refresh_telemetry.sampled_partial_blit_calls,
              refresh_telemetry.partial_attempts > 0
                  ? (double)refresh_telemetry.sampled_partial_blit_calls / (double)refresh_telemetry.partial_attempts
                  : 0.0,
              sampled_refresh_partial_blit_total_ms,
              sampled_refresh_partial_blit_mean_ms);
    io_printf(io, "    \"software_frame_raster_bucket_sampling\": [");
    if (raster_bucket_timing_count > 0) {
        io_printf(io, "\n");
        for (int i = 0; i < raster_bucket_timing_count; i++) {
            const SDLGameRenderer_PerfCaptureRasterBucketTiming* entry = &raster_bucket_timings[i];
            const double sampled_total_ms = (double)entry->sampled_ns / 1e6;
            const double sampled_mean_ms =
                entry->sampled_calls > 0 ? sampled_total_ms / (double)entry->sampled_calls : 0.0;
            const double sampled_mean_pixels =
                entry->sampled_calls > 0 ? (double)entry->sampled_pixels / (double)entry->sampled_calls : 0.0;
            io_printf(io,
                      "      {\"bucket\": \"%s\", \"sample_period\": %llu, "
                      "\"sampled_calls_total\": %llu, \"sampled_calls_mean\": %.4f, "
                      "\"sampled_pixels_total\": %llu, \"sampled_pixels_mean_per_sample\": %.2f, "
                      "\"sampled_total_ms\": %.4f, \"sampled_mean_ms_per_sample\": %.6f}%s\n",
                      software_frame_raster_bucket_name(entry->bucket),
                      (unsigned long long)entry->sample_period,
                      (unsigned long long)entry->sampled_calls,
                      (double)entry->sampled_calls / frame_count,
                      (unsigned long long)entry->sampled_pixels,
                      sampled_mean_pixels,
                      sampled_total_ms,
                      sampled_mean_ms,
                      (i + 1) < raster_bucket_timing_count ? "," : "");
        }
        io_printf(io, "    ],\n");
    } else {
        io_printf(io, "],\n");
    }
    io_printf(io,
              "    \"texture_unlock_dirty_rect_lifetime\": {"
              "\"record_calls_total\": %llu, \"record_calls_mean\": %.4f, "
              "\"retained_after_unlock_total\": %llu, \"retained_after_unlock_mean\": %.4f, "
              "\"retained_after_unlock_record_ratio\": %.6f, "
              "\"clear_stale_before_record_total\": %llu, \"clear_stale_before_record_mean\": %.4f, "
              "\"clear_unlock_unused_total\": %llu, \"clear_unlock_unused_mean\": %.4f, "
              "\"clear_access_unused_total\": %llu, \"clear_access_unused_mean\": %.4f, "
              "\"clear_explicit_total\": %llu, \"clear_explicit_mean\": %.4f},\n",
              (unsigned long long)dirty_rect_lifetime_telemetry.record_calls,
              (double)dirty_rect_lifetime_telemetry.record_calls / frame_count,
              (unsigned long long)dirty_rect_lifetime_telemetry.retained_after_unlock,
              (double)dirty_rect_lifetime_telemetry.retained_after_unlock / frame_count,
              dirty_rect_lifetime_telemetry.record_calls > 0
                  ? (double)dirty_rect_lifetime_telemetry.retained_after_unlock /
                        (double)dirty_rect_lifetime_telemetry.record_calls
                  : 0.0,
              (unsigned long long)dirty_rect_lifetime_telemetry.clear_stale_before_record,
              (double)dirty_rect_lifetime_telemetry.clear_stale_before_record / frame_count,
              (unsigned long long)dirty_rect_lifetime_telemetry.clear_unlock_unused,
              (double)dirty_rect_lifetime_telemetry.clear_unlock_unused / frame_count,
              (unsigned long long)dirty_rect_lifetime_telemetry.clear_access_unused,
              (double)dirty_rect_lifetime_telemetry.clear_access_unused / frame_count,
              (unsigned long long)dirty_rect_lifetime_telemetry.clear_explicit,
              (double)dirty_rect_lifetime_telemetry.clear_explicit / frame_count);
    io_printf(io,
              "    \"ppg_texture_renew_direct_write\": {"
              "\"renew_chunk_calls_total\": %llu, \"renew_chunk_calls_mean\": %.4f, "
              "\"renew_batches_total\": %llu, \"renew_batches_mean\": %.4f, "
              "\"renew_batches_without_rect_total\": %llu, \"renew_batches_without_rect_mean\": %.4f, "
              "\"renew_batches_without_rect_ratio\": %.6f, "
              "\"renew_chunk_pixels_total\": %llu, \"renew_chunk_pixels_mean\": %.2f, "
              "\"renew_batch_bbox_pixels_total\": %llu, \"renew_batch_bbox_pixels_mean\": %.2f, "
              "\"renew_batch_bbox_mean_pixels_per_batch\": %.2f, "
              "\"renew_batch_max_bbox_pixels\": %llu, "
              "\"renew_chunk_8x8_calls_total\": %llu, \"renew_chunk_8x8_calls_mean\": %.4f, "
              "\"renew_chunk_16x16_calls_total\": %llu, \"renew_chunk_16x16_calls_mean\": %.4f, "
              "\"renew_chunk_32x32_calls_total\": %llu, \"renew_chunk_32x32_calls_mean\": %.4f, "
              "\"renew_batch_32x32_covered_tiles_total\": %llu, "
              "\"renew_batch_32x32_covered_tiles_mean_per_batch\": %.2f, "
              "\"renew_batch_32x32_max_covered_tiles\": %llu, "
              "\"renew_batch_32x32_component_count_total\": %llu, "
              "\"renew_batch_32x32_component_count_mean_per_batch\": %.2f, "
              "\"renew_batch_32x32_max_component_count\": %llu, "
              "\"renew_batch_32x32_multi_component_batches_total\": %llu, "
              "\"renew_batch_32x32_multi_component_batch_ratio\": %.6f, "
              "\"renew_batch_32x32_largest_component_tiles_total\": %llu, "
              "\"renew_batch_32x32_largest_component_tiles_mean_per_batch\": %.2f, "
              "\"renew_batch_32x32_max_largest_component_tiles\": %llu},\n",
              (unsigned long long)texture_renew_telemetry.renew_chunk_calls,
              (double)texture_renew_telemetry.renew_chunk_calls / frame_count,
              (unsigned long long)texture_renew_telemetry.renew_batches,
              (double)texture_renew_telemetry.renew_batches / frame_count,
              (unsigned long long)texture_renew_telemetry.renew_batches_without_rect,
              (double)texture_renew_telemetry.renew_batches_without_rect / frame_count,
              texture_renew_telemetry.renew_batches > 0
                  ? (double)texture_renew_telemetry.renew_batches_without_rect /
                        (double)texture_renew_telemetry.renew_batches
                  : 0.0,
              (unsigned long long)texture_renew_telemetry.renew_chunk_pixels,
              (double)texture_renew_telemetry.renew_chunk_pixels / frame_count,
              (unsigned long long)texture_renew_telemetry.renew_batch_bbox_pixels,
              (double)texture_renew_telemetry.renew_batch_bbox_pixels / frame_count,
              texture_renew_telemetry.renew_batches > 0
                  ? (double)texture_renew_telemetry.renew_batch_bbox_pixels /
                        (double)texture_renew_telemetry.renew_batches
                  : 0.0,
              (unsigned long long)texture_renew_telemetry.renew_batch_max_bbox_pixels,
              (unsigned long long)texture_renew_telemetry.renew_chunk_8x8_calls,
              (double)texture_renew_telemetry.renew_chunk_8x8_calls / frame_count,
              (unsigned long long)texture_renew_telemetry.renew_chunk_16x16_calls,
              (double)texture_renew_telemetry.renew_chunk_16x16_calls / frame_count,
              (unsigned long long)texture_renew_telemetry.renew_chunk_32x32_calls,
              (double)texture_renew_telemetry.renew_chunk_32x32_calls / frame_count,
              (unsigned long long)texture_renew_telemetry.renew_batch_32x32_covered_tiles,
              texture_renew_telemetry.renew_batches > 0
                  ? (double)texture_renew_telemetry.renew_batch_32x32_covered_tiles /
                        (double)texture_renew_telemetry.renew_batches
                  : 0.0,
              (unsigned long long)texture_renew_telemetry.renew_batch_32x32_max_covered_tiles,
              (unsigned long long)texture_renew_telemetry.renew_batch_32x32_component_count,
              texture_renew_telemetry.renew_batches > 0
                  ? (double)texture_renew_telemetry.renew_batch_32x32_component_count /
                        (double)texture_renew_telemetry.renew_batches
                  : 0.0,
              (unsigned long long)texture_renew_telemetry.renew_batch_32x32_max_component_count,
              (unsigned long long)texture_renew_telemetry.renew_batch_32x32_multi_component_batches,
              texture_renew_telemetry.renew_batches > 0
                  ? (double)texture_renew_telemetry.renew_batch_32x32_multi_component_batches /
                        (double)texture_renew_telemetry.renew_batches
                  : 0.0,
              (unsigned long long)texture_renew_telemetry.renew_batch_32x32_largest_component_tiles,
              texture_renew_telemetry.renew_batches > 0
                  ? (double)texture_renew_telemetry.renew_batch_32x32_largest_component_tiles /
                        (double)texture_renew_telemetry.renew_batches
                  : 0.0,
              (unsigned long long)texture_renew_telemetry.renew_batch_32x32_max_largest_component_tiles);
    io_printf(io,
              "    \"texture_unlock_locality_index8_zero_delta\": {\"total\": %llu, \"mean\": %.4f, "
              "\"tracked_unlock_ratio\": %.6f},\n",
              (unsigned long long)unlock_locality_telemetry.index8_zero_delta_unlocks,
              (double)unlock_locality_telemetry.index8_zero_delta_unlocks / frame_count,
              unlock_locality_telemetry.index8_tracked_unlocks > 0
                  ? (double)unlock_locality_telemetry.index8_zero_delta_unlocks /
                        (double)unlock_locality_telemetry.index8_tracked_unlocks
                  : 0.0);
    io_printf(io, "    \"software_surface_cache_refresh_hot_textures\": [");
    if (refresh_hot_texture_count > 0) {
        io_printf(io, "\n");
        for (int i = 0; i < refresh_hot_texture_count; i++) {
            const SDLGameRenderer_PerfCaptureRefreshHotTexture* entry = &refresh_hot_textures[i];
            const double refresh_attempt_ratio =
                refresh_attempts_total > 0 ? (double)entry->refresh_attempts / (double)refresh_attempts_total : 0.0;
            const double refresh_pixel_ratio =
                refresh_pixels_total > 0 ? (double)entry->refresh_pixels / (double)refresh_pixels_total : 0.0;
            const double current_lifetime_refresh_attempts_mean =
                (double)entry->current_lifetime_refresh_attempts / frame_count;
            const double current_lifetime_partial_refresh_attempts_mean =
                (double)entry->current_lifetime_partial_refresh_attempts / frame_count;
            const double current_lifetime_full_refresh_attempts_mean =
                (double)entry->current_lifetime_full_refresh_attempts / frame_count;
            const double current_lifetime_full_no_usable_dirty_rect_attempt_ratio =
                entry->current_lifetime_full_refresh_attempts > 0
                    ? (double)entry->current_lifetime_full_refresh_no_usable_dirty_rect_attempts /
                          (double)entry->current_lifetime_full_refresh_attempts
                    : 0.0;
            io_printf(io,
                      "      {\"texture_handle\": %d, \"source_format\": \"%s\", \"width\": %d, \"height\": %d, "
                      "\"source_shape_mixed\": %s, "
                      "\"logical_identity_known\": %s, "
                      "\"logical_identity_mixed\": %s, "
                      "\"logical_identity_registrations_total\": %u, "
                      "\"logical_source_kind\": \"%s\", "
                      "\"logical_ix_num\": %d, "
                      "\"logical_ix_num_first\": %d, "
                      "\"logical_slot_index\": %d, "
                      "\"logical_chunk_index\": %d, "
                      "\"logical_texture_total\": %d, "
                      "\"source_surface_destroy_calls_total\": %llu, "
                      "\"source_surface_destroy_calls_mean\": %.4f, "
                      "\"current_lifetime_refresh_attempts_total\": %llu, "
                      "\"current_lifetime_refresh_attempts_mean\": %.4f, "
                      "\"current_lifetime_partial_refresh_attempts_total\": %llu, "
                      "\"current_lifetime_partial_refresh_attempts_mean\": %.4f, "
                      "\"current_lifetime_full_refresh_attempts_total\": %llu, "
                      "\"current_lifetime_full_refresh_attempts_mean\": %.4f, "
                      "\"current_lifetime_full_refresh_no_usable_dirty_rect_attempts_total\": %llu, "
                      "\"current_lifetime_full_refresh_no_usable_dirty_rect_attempt_ratio\": %.6f, "
                      "\"refresh_attempts_total\": %llu, \"refresh_attempts_mean\": %.4f, "
                      "\"refresh_attempt_ratio\": %.6f, \"refresh_pixels_total\": %llu, "
                      "\"refresh_pixels_mean\": %.2f, \"refresh_pixel_ratio\": %.6f, "
                      "\"max_fanout\": %d}%s\n",
                      entry->texture_handle,
                      pixel_format_name_safe(entry->source_format),
                      entry->width,
                      entry->height,
                      entry->source_shape_mixed ? "true" : "false",
                      entry->logical_identity_known ? "true" : "false",
                      entry->logical_identity_mixed ? "true" : "false",
                      entry->logical_identity_registrations,
                      texture_logical_source_kind_name(entry->logical_source_kind),
                      entry->logical_ix_num,
                      entry->logical_ix_num_first,
                      entry->logical_slot_index,
                      entry->logical_chunk_index,
                      entry->logical_texture_total,
                      (unsigned long long)entry->source_surface_destroy_calls,
                      (double)entry->source_surface_destroy_calls / frame_count,
                      (unsigned long long)entry->current_lifetime_refresh_attempts,
                      current_lifetime_refresh_attempts_mean,
                      (unsigned long long)entry->current_lifetime_partial_refresh_attempts,
                      current_lifetime_partial_refresh_attempts_mean,
                      (unsigned long long)entry->current_lifetime_full_refresh_attempts,
                      current_lifetime_full_refresh_attempts_mean,
                      (unsigned long long)entry->current_lifetime_full_refresh_no_usable_dirty_rect_attempts,
                      current_lifetime_full_no_usable_dirty_rect_attempt_ratio,
                      (unsigned long long)entry->refresh_attempts,
                      (double)entry->refresh_attempts / frame_count,
                      refresh_attempt_ratio,
                      (unsigned long long)entry->refresh_pixels,
                      (double)entry->refresh_pixels / frame_count,
                      refresh_pixel_ratio,
                      entry->max_fanout,
                      (i + 1) < refresh_hot_texture_count ? "," : "");
        }
        io_printf(io, "    ],\n");
    } else {
        io_printf(io, "],\n");
    }
    io_printf(io, "    \"texture_unlock_locality_hot_textures\": [");
    if (unlock_locality_hot_texture_count > 0) {
        io_printf(io, "\n");
        for (int i = 0; i < unlock_locality_hot_texture_count; i++) {
            const SDLGameRenderer_PerfCaptureUnlockLocalityHotTexture* entry = &unlock_locality_hot_textures[i];
            const double tracked_unlock_ratio = unlock_locality_telemetry.index8_tracked_unlocks > 0
                                                    ? (double)entry->tracked_unlocks /
                                                          (double)unlock_locality_telemetry.index8_tracked_unlocks
                                                    : 0.0;
            const double zero_delta_unlock_ratio =
                entry->tracked_unlocks > 0 ? (double)entry->zero_delta_unlocks / (double)entry->tracked_unlocks : 0.0;
            const double changed_pixel_ratio =
                entry->source_pixels > 0 ? (double)entry->changed_pixels / (double)entry->source_pixels : 0.0;
            const double changed_bbox_ratio = entry->source_pixels > 0
                                                  ? (double)entry->changed_bbox_pixels / (double)entry->source_pixels
                                                  : 0.0;
            io_printf(io,
                      "      {\"texture_handle\": %d, \"source_format\": \"%s\", \"width\": %d, \"height\": %d, "
                      "\"source_shape_mixed\": %s, "
                      "\"logical_identity_known\": %s, "
                      "\"logical_identity_mixed\": %s, "
                      "\"logical_identity_registrations_total\": %u, "
                      "\"logical_source_kind\": \"%s\", "
                      "\"logical_ix_num\": %d, "
                      "\"logical_ix_num_first\": %d, "
                      "\"logical_slot_index\": %d, "
                      "\"logical_chunk_index\": %d, "
                      "\"logical_texture_total\": %d, "
                      "\"tracked_unlocks_total\": %llu, \"tracked_unlocks_mean\": %.4f, "
                      "\"tracked_unlock_ratio\": %.6f, \"zero_delta_unlocks_total\": %llu, "
                      "\"zero_delta_unlocks_mean\": %.4f, \"zero_delta_unlock_ratio\": %.6f, "
                      "\"baseline_skips_total\": %llu, "
                      "\"non_index8_skips_total\": %llu, "
                      "\"source_pixels_total\": %llu, \"source_pixels_mean\": %.2f, "
                      "\"changed_pixels_total\": %llu, \"changed_pixels_mean\": %.2f, "
                      "\"changed_pixel_ratio\": %.6f, \"changed_rows_total\": %llu, "
                      "\"changed_rows_mean\": %.2f, \"changed_bbox_pixels_total\": %llu, "
                      "\"changed_bbox_pixels_mean\": %.2f, \"changed_bbox_ratio\": %.6f}%s\n",
                      entry->texture_handle,
                      pixel_format_name_safe(entry->source_format),
                      entry->width,
                      entry->height,
                      entry->source_shape_mixed ? "true" : "false",
                      entry->logical_identity_known ? "true" : "false",
                      entry->logical_identity_mixed ? "true" : "false",
                      entry->logical_identity_registrations,
                      texture_logical_source_kind_name(entry->logical_source_kind),
                      entry->logical_ix_num,
                      entry->logical_ix_num_first,
                      entry->logical_slot_index,
                      entry->logical_chunk_index,
                      entry->logical_texture_total,
                      (unsigned long long)entry->tracked_unlocks,
                      (double)entry->tracked_unlocks / frame_count,
                      tracked_unlock_ratio,
                      (unsigned long long)entry->zero_delta_unlocks,
                      (double)entry->zero_delta_unlocks / frame_count,
                      zero_delta_unlock_ratio,
                      (unsigned long long)entry->baseline_skips,
                      (unsigned long long)entry->non_index8_skips,
                      (unsigned long long)entry->source_pixels,
                      (double)entry->source_pixels / frame_count,
                      (unsigned long long)entry->changed_pixels,
                      (double)entry->changed_pixels / frame_count,
                      changed_pixel_ratio,
                      (unsigned long long)entry->changed_rows,
                      (double)entry->changed_rows / frame_count,
                      (unsigned long long)entry->changed_bbox_pixels,
                      (double)entry->changed_bbox_pixels / frame_count,
                      changed_bbox_ratio,
                      (i + 1) < unlock_locality_hot_texture_count ? "," : "");
        }
        io_printf(io, "    ],\n");
    } else {
        io_printf(io, "],\n");
    }
    io_printf(io,
              "    \"software_surface_cache_refresh_locality_candidates_coverage\": "
              "{\"candidate_count\": %d, \"refresh_attempt_ratio\": %.6f, \"tracked_unlock_ratio\": %.6f, "
              "\"renew_batch_ratio\": %.6f, \"renew_chunk_call_ratio\": %.6f},\n",
              refresh_locality_candidate_count,
              refresh_attempts_total > 0
                  ? (double)refresh_locality_candidate_refresh_attempts / (double)refresh_attempts_total
                  : 0.0,
              unlock_locality_telemetry.index8_tracked_unlocks > 0
                  ? (double)refresh_locality_candidate_tracked_unlocks /
                        (double)unlock_locality_telemetry.index8_tracked_unlocks
                  : 0.0,
              texture_renew_telemetry.renew_batches > 0
                  ? (double)refresh_locality_candidate_renew_batches / (double)texture_renew_telemetry.renew_batches
                  : 0.0,
              texture_renew_telemetry.renew_chunk_calls > 0
                  ? (double)refresh_locality_candidate_renew_chunk_calls /
                        (double)texture_renew_telemetry.renew_chunk_calls
                  : 0.0);
    io_printf(io, "    \"software_surface_cache_refresh_locality_candidates\": [");
    if (refresh_locality_candidate_count > 0) {
        io_printf(io, "\n");
        for (int i = 0; i < refresh_locality_candidate_count; i++) {
            const SDLGameRenderer_PerfCaptureRefreshLocalityCandidate* entry = &refresh_locality_candidates[i];
            const double refresh_attempt_ratio =
                refresh_attempts_total > 0 ? (double)entry->refresh_attempts / (double)refresh_attempts_total : 0.0;
            const double refresh_pixel_ratio =
                refresh_pixels_total > 0 ? (double)entry->refresh_pixels / (double)refresh_pixels_total : 0.0;
            const double current_lifetime_refresh_attempts_mean =
                (double)entry->current_lifetime_refresh_attempts / frame_count;
            const double current_lifetime_partial_refresh_attempts_mean =
                (double)entry->current_lifetime_partial_refresh_attempts / frame_count;
            const double current_lifetime_full_refresh_attempts_mean =
                (double)entry->current_lifetime_full_refresh_attempts / frame_count;
            const double current_lifetime_full_no_usable_dirty_rect_attempt_ratio =
                entry->current_lifetime_full_refresh_attempts > 0
                    ? (double)entry->current_lifetime_full_refresh_no_usable_dirty_rect_attempts /
                          (double)entry->current_lifetime_full_refresh_attempts
                    : 0.0;
            const double tracked_unlock_ratio = unlock_locality_telemetry.index8_tracked_unlocks > 0
                                                    ? (double)entry->tracked_unlocks /
                                                          (double)unlock_locality_telemetry.index8_tracked_unlocks
                                                    : 0.0;
            const double zero_delta_unlock_ratio =
                entry->tracked_unlocks > 0 ? (double)entry->zero_delta_unlocks / (double)entry->tracked_unlocks : 0.0;
            const double partial_refresh_attempt_ratio =
                entry->refresh_attempts > 0
                    ? (double)entry->partial_refresh_attempts / (double)entry->refresh_attempts
                    : 0.0;
            const double full_refresh_attempt_ratio =
                entry->refresh_attempts > 0 ? (double)entry->full_refresh_attempts / (double)entry->refresh_attempts
                                            : 0.0;
            const double sampled_blit_attempt_ratio =
                entry->refresh_attempts > 0 ? (double)entry->sampled_blit_calls / (double)entry->refresh_attempts : 0.0;
            const double sampled_blit_total_ms = (double)entry->sampled_blit_ns / 1e6;
            const double sampled_blit_mean_ms =
                entry->sampled_blit_calls > 0 ? sampled_blit_total_ms / (double)entry->sampled_blit_calls : 0.0;
            const double sampled_full_blit_attempt_ratio =
                entry->full_refresh_attempts > 0
                    ? (double)entry->sampled_full_blit_calls / (double)entry->full_refresh_attempts
                    : 0.0;
            const double sampled_full_blit_total_ms = (double)entry->sampled_full_blit_ns / 1e6;
            const double sampled_full_blit_mean_ms = entry->sampled_full_blit_calls > 0
                                                         ? sampled_full_blit_total_ms /
                                                               (double)entry->sampled_full_blit_calls
                                                         : 0.0;
            const double sampled_full_non_texture_dirty_blit_attempt_ratio =
                entry->full_refresh_non_texture_dirty_attempts > 0
                    ? (double)entry->sampled_full_non_texture_dirty_blit_calls /
                          (double)entry->full_refresh_non_texture_dirty_attempts
                    : 0.0;
            const double sampled_full_non_texture_dirty_blit_total_ms =
                (double)entry->sampled_full_non_texture_dirty_blit_ns / 1e6;
            const double sampled_full_non_texture_dirty_blit_mean_ms =
                entry->sampled_full_non_texture_dirty_blit_calls > 0
                    ? sampled_full_non_texture_dirty_blit_total_ms /
                          (double)entry->sampled_full_non_texture_dirty_blit_calls
                    : 0.0;
            const double sampled_full_ineligible_source_blit_attempt_ratio =
                entry->full_refresh_ineligible_source_attempts > 0
                    ? (double)entry->sampled_full_ineligible_source_blit_calls /
                          (double)entry->full_refresh_ineligible_source_attempts
                    : 0.0;
            const double sampled_full_ineligible_source_blit_total_ms =
                (double)entry->sampled_full_ineligible_source_blit_ns / 1e6;
            const double sampled_full_ineligible_source_blit_mean_ms =
                entry->sampled_full_ineligible_source_blit_calls > 0
                    ? sampled_full_ineligible_source_blit_total_ms /
                          (double)entry->sampled_full_ineligible_source_blit_calls
                    : 0.0;
            const double sampled_full_no_usable_dirty_rect_blit_attempt_ratio =
                entry->full_refresh_no_usable_dirty_rect_attempts > 0
                    ? (double)entry->sampled_full_no_usable_dirty_rect_blit_calls /
                          (double)entry->full_refresh_no_usable_dirty_rect_attempts
                    : 0.0;
            const double sampled_full_no_usable_dirty_rect_blit_total_ms =
                (double)entry->sampled_full_no_usable_dirty_rect_blit_ns / 1e6;
            const double sampled_full_no_usable_dirty_rect_blit_mean_ms =
                entry->sampled_full_no_usable_dirty_rect_blit_calls > 0
                    ? sampled_full_no_usable_dirty_rect_blit_total_ms /
                          (double)entry->sampled_full_no_usable_dirty_rect_blit_calls
                    : 0.0;
            const double sampled_full_oversized_dirty_rect_blit_attempt_ratio =
                entry->full_refresh_oversized_dirty_rect_attempts > 0
                    ? (double)entry->sampled_full_oversized_dirty_rect_blit_calls /
                          (double)entry->full_refresh_oversized_dirty_rect_attempts
                    : 0.0;
            const double sampled_full_oversized_dirty_rect_blit_total_ms =
                (double)entry->sampled_full_oversized_dirty_rect_blit_ns / 1e6;
            const double sampled_full_oversized_dirty_rect_blit_mean_ms =
                entry->sampled_full_oversized_dirty_rect_blit_calls > 0
                    ? sampled_full_oversized_dirty_rect_blit_total_ms /
                          (double)entry->sampled_full_oversized_dirty_rect_blit_calls
                    : 0.0;
            const double sampled_partial_blit_attempt_ratio =
                entry->partial_refresh_attempts > 0
                    ? (double)entry->sampled_partial_blit_calls / (double)entry->partial_refresh_attempts
                    : 0.0;
            const double sampled_partial_blit_total_ms = (double)entry->sampled_partial_blit_ns / 1e6;
            const double sampled_partial_blit_mean_ms =
                entry->sampled_partial_blit_calls > 0
                    ? sampled_partial_blit_total_ms / (double)entry->sampled_partial_blit_calls
                    : 0.0;
            const double software_surface_access_dirty_texture_same_frame_mean =
                (double)entry->software_surface_access_dirty_texture_same_frame / frame_count;
            const double software_surface_access_dirty_texture_carried_mean =
                (double)entry->software_surface_access_dirty_texture_carried / frame_count;
            const double software_surface_access_dirty_palette_same_frame_mean =
                (double)entry->software_surface_access_dirty_palette_same_frame / frame_count;
            const double software_surface_access_dirty_palette_carried_mean =
                (double)entry->software_surface_access_dirty_palette_carried / frame_count;
            const double software_surface_access_dirty_palette_changed_same_frame_mean =
                (double)entry->software_surface_access_dirty_palette_changed_same_frame / frame_count;
            const double software_surface_access_dirty_palette_changed_carried_mean =
                (double)entry->software_surface_access_dirty_palette_changed_carried / frame_count;
            const double software_surface_access_dirty_palette_unchanged_same_frame_mean =
                (double)entry->software_surface_access_dirty_palette_unchanged_same_frame / frame_count;
            const double software_surface_access_dirty_palette_unchanged_carried_mean =
                (double)entry->software_surface_access_dirty_palette_unchanged_carried / frame_count;
            const double software_surface_access_cold_mean = (double)entry->software_surface_access_cold / frame_count;
            const double current_lifetime_software_surface_access_dirty_texture_same_frame_mean =
                (double)entry->current_lifetime_software_surface_access_dirty_texture_same_frame / frame_count;
            const double current_lifetime_software_surface_access_dirty_texture_carried_mean =
                (double)entry->current_lifetime_software_surface_access_dirty_texture_carried / frame_count;
            const double current_lifetime_software_surface_access_dirty_palette_same_frame_mean =
                (double)entry->current_lifetime_software_surface_access_dirty_palette_same_frame / frame_count;
            const double current_lifetime_software_surface_access_dirty_palette_carried_mean =
                (double)entry->current_lifetime_software_surface_access_dirty_palette_carried / frame_count;
            const double current_lifetime_software_surface_access_dirty_palette_changed_same_frame_mean =
                (double)entry->current_lifetime_software_surface_access_dirty_palette_changed_same_frame / frame_count;
            const double current_lifetime_software_surface_access_dirty_palette_changed_carried_mean =
                (double)entry->current_lifetime_software_surface_access_dirty_palette_changed_carried / frame_count;
            const double current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame_mean =
                (double)entry->current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame / frame_count;
            const double current_lifetime_software_surface_access_dirty_palette_unchanged_carried_mean =
                (double)entry->current_lifetime_software_surface_access_dirty_palette_unchanged_carried / frame_count;
            const double current_lifetime_software_surface_access_cold_mean =
                (double)entry->current_lifetime_software_surface_access_cold / frame_count;
            const double dirty_rect_retained_record_ratio =
                entry->dirty_rect_record_calls > 0
                    ? (double)entry->dirty_rect_retained_after_unlock / (double)entry->dirty_rect_record_calls
                    : 0.0;
            const double compare_dirty_rect_refresh_attempts_mean =
                (double)entry->compare_dirty_rect_refresh_attempts / frame_count;
            const double compare_dirty_rect_partial_candidate_refresh_attempt_ratio =
                entry->compare_dirty_rect_refresh_attempts > 0
                    ? (double)entry->compare_dirty_rect_partial_candidate_refresh_attempts /
                          (double)entry->compare_dirty_rect_refresh_attempts
                    : 0.0;
            const double compare_dirty_rect_oversized_candidate_refresh_attempt_ratio =
                entry->compare_dirty_rect_refresh_attempts > 0
                    ? (double)entry->compare_dirty_rect_oversized_candidate_refresh_attempts /
                          (double)entry->compare_dirty_rect_refresh_attempts
                    : 0.0;
            const double compare_dirty_rect_no_usable_candidate_refresh_attempt_ratio =
                entry->compare_dirty_rect_refresh_attempts > 0
                    ? (double)entry->compare_dirty_rect_no_usable_candidate_refresh_attempts /
                          (double)entry->compare_dirty_rect_refresh_attempts
                    : 0.0;
            const double compare_dirty_rect_refresh_bbox_pixels_mean =
                (double)entry->compare_dirty_rect_refresh_bbox_pixels / frame_count;
            const double compare_dirty_rect_refresh_bbox_mean_pixels_per_attempt =
                entry->compare_dirty_rect_refresh_attempts > 0
                    ? (double)entry->compare_dirty_rect_refresh_bbox_pixels /
                          (double)entry->compare_dirty_rect_refresh_attempts
                    : 0.0;
            const double compare_dirty_rect_refresh_pending_unlocks_mean =
                (double)entry->compare_dirty_rect_refresh_pending_unlocks / frame_count;
            const double compare_dirty_rect_refresh_pending_unlocks_mean_per_attempt =
                entry->compare_dirty_rect_refresh_attempts > 0
                    ? (double)entry->compare_dirty_rect_refresh_pending_unlocks /
                          (double)entry->compare_dirty_rect_refresh_attempts
                    : 0.0;
            const double compare_dirty_rect_refresh_32x32_covered_tiles_mean_per_attempt =
                entry->compare_dirty_rect_refresh_attempts > 0
                    ? (double)entry->compare_dirty_rect_refresh_32x32_covered_tiles /
                          (double)entry->compare_dirty_rect_refresh_attempts
                    : 0.0;
            const double compare_dirty_rect_refresh_32x32_component_count_mean_per_attempt =
                entry->compare_dirty_rect_refresh_attempts > 0
                    ? (double)entry->compare_dirty_rect_refresh_32x32_component_count /
                          (double)entry->compare_dirty_rect_refresh_attempts
                    : 0.0;
            const double compare_dirty_rect_refresh_32x32_multi_component_refresh_attempt_ratio =
                entry->compare_dirty_rect_refresh_attempts > 0
                    ? (double)entry->compare_dirty_rect_refresh_32x32_multi_component_refresh_attempts /
                          (double)entry->compare_dirty_rect_refresh_attempts
                    : 0.0;
            const double compare_dirty_rect_refresh_32x32_largest_component_tiles_mean_per_attempt =
                entry->compare_dirty_rect_refresh_attempts > 0
                    ? (double)entry->compare_dirty_rect_refresh_32x32_largest_component_tiles /
                          (double)entry->compare_dirty_rect_refresh_attempts
                    : 0.0;
            const double renew_batches_without_rect_ratio =
                entry->renew_batches > 0 ? (double)entry->renew_batches_without_rect / (double)entry->renew_batches : 0.0;
            const double renew_batch_bbox_mean_pixels =
                entry->renew_batches > 0 ? (double)entry->renew_batch_bbox_pixels / (double)entry->renew_batches : 0.0;
            const double renew_batch_32x32_covered_tiles_mean_per_batch =
                entry->renew_batches > 0
                    ? (double)entry->renew_batch_32x32_covered_tiles / (double)entry->renew_batches
                    : 0.0;
            const double renew_batch_32x32_component_count_mean_per_batch =
                entry->renew_batches > 0
                    ? (double)entry->renew_batch_32x32_component_count / (double)entry->renew_batches
                    : 0.0;
            const double renew_batch_32x32_multi_component_batch_ratio =
                entry->renew_batches > 0
                    ? (double)entry->renew_batch_32x32_multi_component_batches / (double)entry->renew_batches
                    : 0.0;
            const double renew_batch_32x32_largest_component_tiles_mean_per_batch =
                entry->renew_batches > 0
                    ? (double)entry->renew_batch_32x32_largest_component_tiles / (double)entry->renew_batches
                    : 0.0;
            const double changed_pixel_ratio =
                entry->source_pixels > 0 ? (double)entry->changed_pixels / (double)entry->source_pixels : 0.0;
            const double changed_bbox_ratio = entry->source_pixels > 0
                                                  ? (double)entry->changed_bbox_pixels / (double)entry->source_pixels
                                                  : 0.0;
            const double whole_capture_tracked_unlock_ratio =
                unlock_locality_telemetry.index8_tracked_unlocks > 0
                    ? (double)entry->whole_capture_tracked_unlocks /
                          (double)unlock_locality_telemetry.index8_tracked_unlocks
                    : 0.0;
            const double whole_capture_zero_delta_unlock_ratio =
                entry->whole_capture_tracked_unlocks > 0
                    ? (double)entry->whole_capture_zero_delta_unlocks /
                          (double)entry->whole_capture_tracked_unlocks
                    : 0.0;
            const double whole_capture_changed_pixel_ratio =
                entry->whole_capture_source_pixels > 0
                    ? (double)entry->whole_capture_changed_pixels / (double)entry->whole_capture_source_pixels
                    : 0.0;
            const double whole_capture_changed_bbox_ratio =
                entry->whole_capture_source_pixels > 0
                    ? (double)entry->whole_capture_changed_bbox_pixels / (double)entry->whole_capture_source_pixels
                    : 0.0;
            const Uint64 compare_dirty_rect_source_pixels =
                (entry->width > 0) && (entry->height > 0) ? (Uint64)entry->width * (Uint64)entry->height : 0;
            const double compare_dirty_rect_refresh_bbox_mean_ratio =
                (compare_dirty_rect_source_pixels > 0) && (entry->compare_dirty_rect_refresh_attempts > 0)
                    ? (double)entry->compare_dirty_rect_refresh_bbox_pixels /
                          ((double)compare_dirty_rect_source_pixels *
                           (double)entry->compare_dirty_rect_refresh_attempts)
                    : 0.0;
            const double compare_dirty_rect_refresh_max_bbox_ratio =
                compare_dirty_rect_source_pixels > 0
                    ? (double)entry->compare_dirty_rect_refresh_max_bbox_pixels /
                          (double)compare_dirty_rect_source_pixels
                    : 0.0;
            io_printf(io,
                      "      {\"texture_handle\": %d, \"source_format\": \"%s\", \"width\": %d, \"height\": %d, "
                      "\"source_shape_mixed\": %s, "
                      "\"logical_identity_known\": %s, "
                      "\"logical_identity_mixed\": %s, "
                      "\"logical_identity_registrations_total\": %u, "
                      "\"logical_source_kind\": \"%s\", "
                      "\"logical_ix_num\": %d, "
                      "\"logical_ix_num_first\": %d, "
                      "\"logical_slot_index\": %d, "
                      "\"logical_chunk_index\": %d, "
                      "\"logical_texture_total\": %d, "
                      "\"source_surface_destroy_calls_total\": %llu, "
                      "\"source_surface_destroy_calls_mean\": %.4f, "
                      "\"current_lifetime_refresh_attempts_total\": %llu, "
                      "\"current_lifetime_refresh_attempts_mean\": %.4f, "
                      "\"current_lifetime_partial_refresh_attempts_total\": %llu, "
                      "\"current_lifetime_partial_refresh_attempts_mean\": %.4f, "
                      "\"current_lifetime_full_refresh_attempts_total\": %llu, "
                      "\"current_lifetime_full_refresh_attempts_mean\": %.4f, "
                      "\"current_lifetime_full_refresh_no_usable_dirty_rect_attempts_total\": %llu, "
                      "\"current_lifetime_full_refresh_no_usable_dirty_rect_attempt_ratio\": %.6f, "
                      "\"refresh_attempts_total\": %llu, \"refresh_attempts_mean\": %.4f, "
                      "\"refresh_attempt_ratio\": %.6f, \"refresh_pixels_total\": %llu, "
                      "\"refresh_pixels_mean\": %.2f, \"refresh_pixel_ratio\": %.6f, "
                      "\"max_fanout\": %d, \"partial_refresh_attempts_total\": %llu, "
                      "\"partial_refresh_attempts_mean\": %.4f, "
                      "\"partial_refresh_attempt_ratio\": %.6f, "
                      "\"partial_refresh_pixels_total\": %llu, "
                      "\"partial_refresh_pixels_mean\": %.2f, "
                      "\"full_refresh_attempts_total\": %llu, \"full_refresh_attempts_mean\": %.4f, "
                      "\"full_refresh_attempt_ratio\": %.6f, "
                      "\"full_refresh_pixels_total\": %llu, \"full_refresh_pixels_mean\": %.2f, "
                      "\"full_refresh_non_texture_dirty_attempts_total\": %llu, "
                      "\"full_refresh_ineligible_source_attempts_total\": %llu, "
                      "\"full_refresh_no_usable_dirty_rect_attempts_total\": %llu, "
                      "\"full_refresh_oversized_dirty_rect_attempts_total\": %llu, "
                      "\"sampled_blit_calls_total\": %llu, "
                      "\"sampled_blit_attempt_ratio\": %.6f, "
                      "\"sampled_blit_total_ms\": %.4f, "
                      "\"sampled_blit_mean_ms_per_sample\": %.6f, "
                      "\"sampled_full_blit_calls_total\": %llu, "
                      "\"sampled_full_blit_attempt_ratio\": %.6f, "
                      "\"sampled_full_blit_total_ms\": %.4f, "
                      "\"sampled_full_blit_mean_ms_per_sample\": %.6f, "
                      "\"sampled_full_non_texture_dirty_blit_calls_total\": %llu, "
                      "\"sampled_full_non_texture_dirty_blit_attempt_ratio\": %.6f, "
                      "\"sampled_full_non_texture_dirty_blit_total_ms\": %.4f, "
                      "\"sampled_full_non_texture_dirty_blit_mean_ms_per_sample\": %.6f, "
                      "\"sampled_full_ineligible_source_blit_calls_total\": %llu, "
                      "\"sampled_full_ineligible_source_blit_attempt_ratio\": %.6f, "
                      "\"sampled_full_ineligible_source_blit_total_ms\": %.4f, "
                      "\"sampled_full_ineligible_source_blit_mean_ms_per_sample\": %.6f, "
                      "\"sampled_full_no_usable_dirty_rect_blit_calls_total\": %llu, "
                      "\"sampled_full_no_usable_dirty_rect_blit_attempt_ratio\": %.6f, "
                      "\"sampled_full_no_usable_dirty_rect_blit_total_ms\": %.4f, "
                      "\"sampled_full_no_usable_dirty_rect_blit_mean_ms_per_sample\": %.6f, "
                      "\"sampled_full_oversized_dirty_rect_blit_calls_total\": %llu, "
                      "\"sampled_full_oversized_dirty_rect_blit_attempt_ratio\": %.6f, "
                      "\"sampled_full_oversized_dirty_rect_blit_total_ms\": %.4f, "
                      "\"sampled_full_oversized_dirty_rect_blit_mean_ms_per_sample\": %.6f, "
                      "\"sampled_partial_blit_calls_total\": %llu, "
                      "\"sampled_partial_blit_attempt_ratio\": %.6f, "
                      "\"sampled_partial_blit_total_ms\": %.4f, "
                      "\"sampled_partial_blit_mean_ms_per_sample\": %.6f, "
                      "\"software_surface_access_dirty_texture_same_frame_total\": %llu, "
                      "\"software_surface_access_dirty_texture_same_frame_mean\": %.4f, "
                      "\"software_surface_access_dirty_texture_carried_total\": %llu, "
                      "\"software_surface_access_dirty_texture_carried_mean\": %.4f, "
                      "\"software_surface_access_dirty_palette_same_frame_total\": %llu, "
                      "\"software_surface_access_dirty_palette_same_frame_mean\": %.4f, "
                      "\"software_surface_access_dirty_palette_carried_total\": %llu, "
                      "\"software_surface_access_dirty_palette_carried_mean\": %.4f, "
                      "\"software_surface_access_dirty_palette_changed_same_frame_total\": %llu, "
                      "\"software_surface_access_dirty_palette_changed_same_frame_mean\": %.4f, "
                      "\"software_surface_access_dirty_palette_changed_carried_total\": %llu, "
                      "\"software_surface_access_dirty_palette_changed_carried_mean\": %.4f, "
                      "\"software_surface_access_dirty_palette_unchanged_same_frame_total\": %llu, "
                      "\"software_surface_access_dirty_palette_unchanged_same_frame_mean\": %.4f, "
                      "\"software_surface_access_dirty_palette_unchanged_carried_total\": %llu, "
                      "\"software_surface_access_dirty_palette_unchanged_carried_mean\": %.4f, "
                      "\"software_surface_access_cold_total\": %llu, "
                      "\"software_surface_access_cold_mean\": %.4f, "
                      "\"current_lifetime_software_surface_access_dirty_texture_same_frame_total\": %llu, "
                      "\"current_lifetime_software_surface_access_dirty_texture_same_frame_mean\": %.4f, "
                      "\"current_lifetime_software_surface_access_dirty_texture_carried_total\": %llu, "
                      "\"current_lifetime_software_surface_access_dirty_texture_carried_mean\": %.4f, "
                      "\"current_lifetime_software_surface_access_dirty_palette_same_frame_total\": %llu, "
                      "\"current_lifetime_software_surface_access_dirty_palette_same_frame_mean\": %.4f, "
                      "\"current_lifetime_software_surface_access_dirty_palette_carried_total\": %llu, "
                      "\"current_lifetime_software_surface_access_dirty_palette_carried_mean\": %.4f, "
                      "\"current_lifetime_software_surface_access_dirty_palette_changed_same_frame_total\": %llu, "
                      "\"current_lifetime_software_surface_access_dirty_palette_changed_same_frame_mean\": %.4f, "
                      "\"current_lifetime_software_surface_access_dirty_palette_changed_carried_total\": %llu, "
                      "\"current_lifetime_software_surface_access_dirty_palette_changed_carried_mean\": %.4f, "
                      "\"current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame_total\": %llu, "
                      "\"current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame_mean\": %.4f, "
                      "\"current_lifetime_software_surface_access_dirty_palette_unchanged_carried_total\": %llu, "
                      "\"current_lifetime_software_surface_access_dirty_palette_unchanged_carried_mean\": %.4f, "
                      "\"current_lifetime_software_surface_access_cold_total\": %llu, "
                      "\"current_lifetime_software_surface_access_cold_mean\": %.4f, "
                      "\"tracked_unlocks_total\": %llu, "
                      "\"tracked_unlocks_mean\": %.4f, \"tracked_unlock_ratio\": %.6f, "
                      "\"zero_delta_unlocks_total\": %llu, \"zero_delta_unlocks_mean\": %.4f, "
                      "\"zero_delta_unlock_ratio\": %.6f, "
                      "\"whole_capture_tracked_unlocks_total\": %llu, "
                      "\"whole_capture_tracked_unlocks_mean\": %.4f, "
                      "\"whole_capture_tracked_unlock_ratio\": %.6f, "
                      "\"whole_capture_zero_delta_unlocks_total\": %llu, "
                      "\"whole_capture_zero_delta_unlocks_mean\": %.4f, "
                      "\"whole_capture_zero_delta_unlock_ratio\": %.6f, "
                      "\"dirty_rect_record_calls_total\": %llu, "
                      "\"dirty_rect_retained_after_unlock_total\": %llu, "
                      "\"dirty_rect_retained_after_unlock_record_ratio\": %.6f, "
                      "\"dirty_rect_clear_stale_before_record_total\": %llu, "
                      "\"dirty_rect_clear_unlock_unused_total\": %llu, "
                      "\"dirty_rect_clear_access_unused_total\": %llu, "
                      "\"dirty_rect_clear_explicit_total\": %llu, "
                      "\"compare_dirty_rect_refresh_attempts_total\": %llu, "
                      "\"compare_dirty_rect_refresh_attempts_mean\": %.4f, "
                      "\"compare_dirty_rect_partial_candidate_refresh_attempts_total\": %llu, "
                      "\"compare_dirty_rect_partial_candidate_refresh_attempt_ratio\": %.6f, "
                      "\"compare_dirty_rect_oversized_candidate_refresh_attempts_total\": %llu, "
                      "\"compare_dirty_rect_oversized_candidate_refresh_attempt_ratio\": %.6f, "
                      "\"compare_dirty_rect_no_usable_candidate_refresh_attempts_total\": %llu, "
                      "\"compare_dirty_rect_no_usable_candidate_refresh_attempt_ratio\": %.6f, "
                      "\"compare_dirty_rect_refresh_bbox_pixels_total\": %llu, "
                      "\"compare_dirty_rect_refresh_bbox_pixels_mean\": %.2f, "
                      "\"compare_dirty_rect_refresh_bbox_mean_pixels_per_attempt\": %.2f, "
                      "\"compare_dirty_rect_refresh_max_bbox_pixels\": %llu, "
                      "\"compare_dirty_rect_refresh_bbox_mean_ratio\": %.6f, "
                      "\"compare_dirty_rect_refresh_max_bbox_ratio\": %.6f, "
                      "\"compare_dirty_rect_refresh_pending_unlocks_total\": %llu, "
                      "\"compare_dirty_rect_refresh_pending_unlocks_mean\": %.4f, "
                      "\"compare_dirty_rect_refresh_pending_unlocks_mean_per_attempt\": %.4f, "
                      "\"compare_dirty_rect_refresh_max_pending_unlocks\": %llu, "
                      "\"compare_dirty_rect_refresh_32x32_covered_tiles_total\": %llu, "
                      "\"compare_dirty_rect_refresh_32x32_covered_tiles_mean_per_attempt\": %.2f, "
                      "\"compare_dirty_rect_refresh_32x32_max_covered_tiles\": %llu, "
                      "\"compare_dirty_rect_refresh_32x32_component_count_total\": %llu, "
                      "\"compare_dirty_rect_refresh_32x32_component_count_mean_per_attempt\": %.2f, "
                      "\"compare_dirty_rect_refresh_32x32_max_component_count\": %llu, "
                      "\"compare_dirty_rect_refresh_32x32_multi_component_refresh_attempts_total\": %llu, "
                      "\"compare_dirty_rect_refresh_32x32_multi_component_refresh_attempt_ratio\": %.6f, "
                      "\"compare_dirty_rect_refresh_32x32_largest_component_tiles_total\": %llu, "
                      "\"compare_dirty_rect_refresh_32x32_largest_component_tiles_mean_per_attempt\": %.2f, "
                      "\"compare_dirty_rect_refresh_32x32_max_largest_component_tiles\": %llu, "
                      "\"renew_chunk_calls_total\": %llu, "
                      "\"renew_chunk_calls_mean\": %.4f, "
                      "\"renew_batches_total\": %llu, "
                      "\"renew_batches_mean\": %.4f, "
                      "\"renew_batches_without_rect_total\": %llu, "
                      "\"renew_batches_without_rect_ratio\": %.6f, "
                      "\"renew_chunk_pixels_total\": %llu, "
                      "\"renew_chunk_pixels_mean\": %.2f, "
                      "\"renew_batch_bbox_pixels_total\": %llu, "
                      "\"renew_batch_bbox_pixels_mean\": %.2f, "
                      "\"renew_batch_bbox_mean_pixels_per_batch\": %.2f, "
                      "\"renew_batch_max_bbox_pixels\": %llu, "
                      "\"renew_chunk_8x8_calls_total\": %llu, "
                      "\"renew_chunk_16x16_calls_total\": %llu, "
                      "\"renew_chunk_32x32_calls_total\": %llu, "
                      "\"renew_batch_32x32_covered_tiles_total\": %llu, "
                      "\"renew_batch_32x32_covered_tiles_mean_per_batch\": %.2f, "
                      "\"renew_batch_32x32_max_covered_tiles\": %llu, "
                      "\"renew_batch_32x32_component_count_total\": %llu, "
                      "\"renew_batch_32x32_component_count_mean_per_batch\": %.2f, "
                      "\"renew_batch_32x32_max_component_count\": %llu, "
                      "\"renew_batch_32x32_multi_component_batches_total\": %llu, "
                      "\"renew_batch_32x32_multi_component_batch_ratio\": %.6f, "
                      "\"renew_batch_32x32_largest_component_tiles_total\": %llu, "
                      "\"renew_batch_32x32_largest_component_tiles_mean_per_batch\": %.2f, "
                      "\"renew_batch_32x32_max_largest_component_tiles\": %llu, "
                      "\"baseline_skips_total\": %llu, \"non_index8_skips_total\": %llu, "
                      "\"source_pixels_total\": %llu, \"source_pixels_mean\": %.2f, "
                      "\"changed_pixels_total\": %llu, \"changed_pixels_mean\": %.2f, "
                      "\"changed_pixel_ratio\": %.6f, \"changed_rows_total\": %llu, "
                      "\"changed_rows_mean\": %.2f, \"changed_bbox_pixels_total\": %llu, "
                      "\"changed_bbox_pixels_mean\": %.2f, \"changed_bbox_ratio\": %.6f, "
                      "\"whole_capture_baseline_skips_total\": %llu, "
                      "\"whole_capture_non_index8_skips_total\": %llu, "
                      "\"whole_capture_source_pixels_total\": %llu, "
                      "\"whole_capture_source_pixels_mean\": %.2f, "
                      "\"whole_capture_changed_pixels_total\": %llu, "
                      "\"whole_capture_changed_pixels_mean\": %.2f, "
                      "\"whole_capture_changed_pixel_ratio\": %.6f, "
                      "\"whole_capture_changed_rows_total\": %llu, "
                      "\"whole_capture_changed_rows_mean\": %.2f, "
                      "\"whole_capture_changed_bbox_pixels_total\": %llu, "
                      "\"whole_capture_changed_bbox_pixels_mean\": %.2f, "
                      "\"whole_capture_changed_bbox_ratio\": %.6f}%s\n",
                      entry->texture_handle,
                      pixel_format_name_safe(entry->source_format),
                      entry->width,
                      entry->height,
                      entry->source_shape_mixed ? "true" : "false",
                      entry->logical_identity_known ? "true" : "false",
                      entry->logical_identity_mixed ? "true" : "false",
                      entry->logical_identity_registrations,
                      texture_logical_source_kind_name(entry->logical_source_kind),
                      entry->logical_ix_num,
                      entry->logical_ix_num_first,
                      entry->logical_slot_index,
                      entry->logical_chunk_index,
                      entry->logical_texture_total,
                      (unsigned long long)entry->source_surface_destroy_calls,
                      (double)entry->source_surface_destroy_calls / frame_count,
                      (unsigned long long)entry->current_lifetime_refresh_attempts,
                      current_lifetime_refresh_attempts_mean,
                      (unsigned long long)entry->current_lifetime_partial_refresh_attempts,
                      current_lifetime_partial_refresh_attempts_mean,
                      (unsigned long long)entry->current_lifetime_full_refresh_attempts,
                      current_lifetime_full_refresh_attempts_mean,
                      (unsigned long long)entry->current_lifetime_full_refresh_no_usable_dirty_rect_attempts,
                      current_lifetime_full_no_usable_dirty_rect_attempt_ratio,
                      (unsigned long long)entry->refresh_attempts,
                      (double)entry->refresh_attempts / frame_count,
                      refresh_attempt_ratio,
                      (unsigned long long)entry->refresh_pixels,
                      (double)entry->refresh_pixels / frame_count,
                      refresh_pixel_ratio,
                      entry->max_fanout,
                      (unsigned long long)entry->partial_refresh_attempts,
                      (double)entry->partial_refresh_attempts / frame_count,
                      partial_refresh_attempt_ratio,
                      (unsigned long long)entry->partial_refresh_pixels,
                      (double)entry->partial_refresh_pixels / frame_count,
                      (unsigned long long)entry->full_refresh_attempts,
                      (double)entry->full_refresh_attempts / frame_count,
                      full_refresh_attempt_ratio,
                      (unsigned long long)entry->full_refresh_pixels,
                      (double)entry->full_refresh_pixels / frame_count,
                      (unsigned long long)entry->full_refresh_non_texture_dirty_attempts,
                      (unsigned long long)entry->full_refresh_ineligible_source_attempts,
                      (unsigned long long)entry->full_refresh_no_usable_dirty_rect_attempts,
                      (unsigned long long)entry->full_refresh_oversized_dirty_rect_attempts,
                      (unsigned long long)entry->sampled_blit_calls,
                      sampled_blit_attempt_ratio,
                      sampled_blit_total_ms,
                      sampled_blit_mean_ms,
                      (unsigned long long)entry->sampled_full_blit_calls,
                      sampled_full_blit_attempt_ratio,
                      sampled_full_blit_total_ms,
                      sampled_full_blit_mean_ms,
                      (unsigned long long)entry->sampled_full_non_texture_dirty_blit_calls,
                      sampled_full_non_texture_dirty_blit_attempt_ratio,
                      sampled_full_non_texture_dirty_blit_total_ms,
                      sampled_full_non_texture_dirty_blit_mean_ms,
                      (unsigned long long)entry->sampled_full_ineligible_source_blit_calls,
                      sampled_full_ineligible_source_blit_attempt_ratio,
                      sampled_full_ineligible_source_blit_total_ms,
                      sampled_full_ineligible_source_blit_mean_ms,
                      (unsigned long long)entry->sampled_full_no_usable_dirty_rect_blit_calls,
                      sampled_full_no_usable_dirty_rect_blit_attempt_ratio,
                      sampled_full_no_usable_dirty_rect_blit_total_ms,
                      sampled_full_no_usable_dirty_rect_blit_mean_ms,
                      (unsigned long long)entry->sampled_full_oversized_dirty_rect_blit_calls,
                      sampled_full_oversized_dirty_rect_blit_attempt_ratio,
                      sampled_full_oversized_dirty_rect_blit_total_ms,
                      sampled_full_oversized_dirty_rect_blit_mean_ms,
                      (unsigned long long)entry->sampled_partial_blit_calls,
                      sampled_partial_blit_attempt_ratio,
                      sampled_partial_blit_total_ms,
                      sampled_partial_blit_mean_ms,
                      (unsigned long long)entry->software_surface_access_dirty_texture_same_frame,
                      software_surface_access_dirty_texture_same_frame_mean,
                      (unsigned long long)entry->software_surface_access_dirty_texture_carried,
                      software_surface_access_dirty_texture_carried_mean,
                      (unsigned long long)entry->software_surface_access_dirty_palette_same_frame,
                      software_surface_access_dirty_palette_same_frame_mean,
                      (unsigned long long)entry->software_surface_access_dirty_palette_carried,
                      software_surface_access_dirty_palette_carried_mean,
                      (unsigned long long)entry->software_surface_access_dirty_palette_changed_same_frame,
                      software_surface_access_dirty_palette_changed_same_frame_mean,
                      (unsigned long long)entry->software_surface_access_dirty_palette_changed_carried,
                      software_surface_access_dirty_palette_changed_carried_mean,
                      (unsigned long long)entry->software_surface_access_dirty_palette_unchanged_same_frame,
                      software_surface_access_dirty_palette_unchanged_same_frame_mean,
                      (unsigned long long)entry->software_surface_access_dirty_palette_unchanged_carried,
                      software_surface_access_dirty_palette_unchanged_carried_mean,
                      (unsigned long long)entry->software_surface_access_cold,
                      software_surface_access_cold_mean,
                      (unsigned long long)entry->current_lifetime_software_surface_access_dirty_texture_same_frame,
                      current_lifetime_software_surface_access_dirty_texture_same_frame_mean,
                      (unsigned long long)entry->current_lifetime_software_surface_access_dirty_texture_carried,
                      current_lifetime_software_surface_access_dirty_texture_carried_mean,
                      (unsigned long long)entry->current_lifetime_software_surface_access_dirty_palette_same_frame,
                      current_lifetime_software_surface_access_dirty_palette_same_frame_mean,
                      (unsigned long long)entry->current_lifetime_software_surface_access_dirty_palette_carried,
                      current_lifetime_software_surface_access_dirty_palette_carried_mean,
                      (unsigned long long)entry->current_lifetime_software_surface_access_dirty_palette_changed_same_frame,
                      current_lifetime_software_surface_access_dirty_palette_changed_same_frame_mean,
                      (unsigned long long)entry->current_lifetime_software_surface_access_dirty_palette_changed_carried,
                      current_lifetime_software_surface_access_dirty_palette_changed_carried_mean,
                      (unsigned long long)entry->current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame,
                      current_lifetime_software_surface_access_dirty_palette_unchanged_same_frame_mean,
                      (unsigned long long)entry->current_lifetime_software_surface_access_dirty_palette_unchanged_carried,
                      current_lifetime_software_surface_access_dirty_palette_unchanged_carried_mean,
                      (unsigned long long)entry->current_lifetime_software_surface_access_cold,
                      current_lifetime_software_surface_access_cold_mean,
                      (unsigned long long)entry->tracked_unlocks,
                      (double)entry->tracked_unlocks / frame_count,
                      tracked_unlock_ratio,
                      (unsigned long long)entry->zero_delta_unlocks,
                      (double)entry->zero_delta_unlocks / frame_count,
                      zero_delta_unlock_ratio,
                      (unsigned long long)entry->whole_capture_tracked_unlocks,
                      (double)entry->whole_capture_tracked_unlocks / frame_count,
                      whole_capture_tracked_unlock_ratio,
                      (unsigned long long)entry->whole_capture_zero_delta_unlocks,
                      (double)entry->whole_capture_zero_delta_unlocks / frame_count,
                      whole_capture_zero_delta_unlock_ratio,
                      (unsigned long long)entry->dirty_rect_record_calls,
                      (unsigned long long)entry->dirty_rect_retained_after_unlock,
                      dirty_rect_retained_record_ratio,
                      (unsigned long long)entry->dirty_rect_clear_stale_before_record,
                      (unsigned long long)entry->dirty_rect_clear_unlock_unused,
                      (unsigned long long)entry->dirty_rect_clear_access_unused,
                      (unsigned long long)entry->dirty_rect_clear_explicit,
                      (unsigned long long)entry->compare_dirty_rect_refresh_attempts,
                      compare_dirty_rect_refresh_attempts_mean,
                      (unsigned long long)entry->compare_dirty_rect_partial_candidate_refresh_attempts,
                      compare_dirty_rect_partial_candidate_refresh_attempt_ratio,
                      (unsigned long long)entry->compare_dirty_rect_oversized_candidate_refresh_attempts,
                      compare_dirty_rect_oversized_candidate_refresh_attempt_ratio,
                      (unsigned long long)entry->compare_dirty_rect_no_usable_candidate_refresh_attempts,
                      compare_dirty_rect_no_usable_candidate_refresh_attempt_ratio,
                      (unsigned long long)entry->compare_dirty_rect_refresh_bbox_pixels,
                      compare_dirty_rect_refresh_bbox_pixels_mean,
                      compare_dirty_rect_refresh_bbox_mean_pixels_per_attempt,
                      (unsigned long long)entry->compare_dirty_rect_refresh_max_bbox_pixels,
                      compare_dirty_rect_refresh_bbox_mean_ratio,
                      compare_dirty_rect_refresh_max_bbox_ratio,
                      (unsigned long long)entry->compare_dirty_rect_refresh_pending_unlocks,
                      compare_dirty_rect_refresh_pending_unlocks_mean,
                      compare_dirty_rect_refresh_pending_unlocks_mean_per_attempt,
                      (unsigned long long)entry->compare_dirty_rect_refresh_max_pending_unlocks,
                      (unsigned long long)entry->compare_dirty_rect_refresh_32x32_covered_tiles,
                      compare_dirty_rect_refresh_32x32_covered_tiles_mean_per_attempt,
                      (unsigned long long)entry->compare_dirty_rect_refresh_32x32_max_covered_tiles,
                      (unsigned long long)entry->compare_dirty_rect_refresh_32x32_component_count,
                      compare_dirty_rect_refresh_32x32_component_count_mean_per_attempt,
                      (unsigned long long)entry->compare_dirty_rect_refresh_32x32_max_component_count,
                      (unsigned long long)entry->compare_dirty_rect_refresh_32x32_multi_component_refresh_attempts,
                      compare_dirty_rect_refresh_32x32_multi_component_refresh_attempt_ratio,
                      (unsigned long long)entry->compare_dirty_rect_refresh_32x32_largest_component_tiles,
                      compare_dirty_rect_refresh_32x32_largest_component_tiles_mean_per_attempt,
                      (unsigned long long)entry->compare_dirty_rect_refresh_32x32_max_largest_component_tiles,
                      (unsigned long long)entry->renew_chunk_calls,
                      (double)entry->renew_chunk_calls / frame_count,
                      (unsigned long long)entry->renew_batches,
                      (double)entry->renew_batches / frame_count,
                      (unsigned long long)entry->renew_batches_without_rect,
                      renew_batches_without_rect_ratio,
                      (unsigned long long)entry->renew_chunk_pixels,
                      (double)entry->renew_chunk_pixels / frame_count,
                      (unsigned long long)entry->renew_batch_bbox_pixels,
                      (double)entry->renew_batch_bbox_pixels / frame_count,
                      renew_batch_bbox_mean_pixels,
                      (unsigned long long)entry->renew_batch_max_bbox_pixels,
                      (unsigned long long)entry->renew_chunk_8x8_calls,
                      (unsigned long long)entry->renew_chunk_16x16_calls,
                      (unsigned long long)entry->renew_chunk_32x32_calls,
                      (unsigned long long)entry->renew_batch_32x32_covered_tiles,
                      renew_batch_32x32_covered_tiles_mean_per_batch,
                      (unsigned long long)entry->renew_batch_32x32_max_covered_tiles,
                      (unsigned long long)entry->renew_batch_32x32_component_count,
                      renew_batch_32x32_component_count_mean_per_batch,
                      (unsigned long long)entry->renew_batch_32x32_max_component_count,
                      (unsigned long long)entry->renew_batch_32x32_multi_component_batches,
                      renew_batch_32x32_multi_component_batch_ratio,
                      (unsigned long long)entry->renew_batch_32x32_largest_component_tiles,
                      renew_batch_32x32_largest_component_tiles_mean_per_batch,
                      (unsigned long long)entry->renew_batch_32x32_max_largest_component_tiles,
                      (unsigned long long)entry->baseline_skips,
                      (unsigned long long)entry->non_index8_skips,
                      (unsigned long long)entry->source_pixels,
                      (double)entry->source_pixels / frame_count,
                      (unsigned long long)entry->changed_pixels,
                      (double)entry->changed_pixels / frame_count,
                      changed_pixel_ratio,
                      (unsigned long long)entry->changed_rows,
                      (double)entry->changed_rows / frame_count,
                      (unsigned long long)entry->changed_bbox_pixels,
                      (double)entry->changed_bbox_pixels / frame_count,
                      changed_bbox_ratio,
                      (unsigned long long)entry->whole_capture_baseline_skips,
                      (unsigned long long)entry->whole_capture_non_index8_skips,
                      (unsigned long long)entry->whole_capture_source_pixels,
                      (double)entry->whole_capture_source_pixels / frame_count,
                      (unsigned long long)entry->whole_capture_changed_pixels,
                      (double)entry->whole_capture_changed_pixels / frame_count,
                      whole_capture_changed_pixel_ratio,
                      (unsigned long long)entry->whole_capture_changed_rows,
                      (double)entry->whole_capture_changed_rows / frame_count,
                      (unsigned long long)entry->whole_capture_changed_bbox_pixels,
                      (double)entry->whole_capture_changed_bbox_pixels / frame_count,
                      whole_capture_changed_bbox_ratio,
                      (i + 1) < refresh_locality_candidate_count ? "," : "");
        }
        io_printf(io, "    ],\n");
    } else {
        io_printf(io, "],\n");
    }
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
                  "\"texture_cache_hits\": %d, \"texture_cache_misses\": %d, "
                  "\"texture_cache_miss_dirty_texture_same_frame\": %d, "
                  "\"texture_cache_miss_dirty_texture_carried\": %d, "
                  "\"texture_cache_miss_dirty_palette_same_frame\": %d, "
                  "\"texture_cache_miss_dirty_palette_carried\": %d, "
                  "\"texture_cache_miss_cold\": %d, \"texture_creates\": %d, "
                  "\"texture_unlock_calls\": %d, \"palette_unlock_calls\": %d, "
                  "\"texture_unlock_dirty_surface_variants\": %d, "
                  "\"texture_unlock_dirty_surface_variants_max\": %d, "
                  "\"palette_unlock_dirty_surface_variants\": %d, "
                  "\"palette_unlock_dirty_surface_variants_max\": %d, "
                  "\"texture_unlock_locality_index8_tracked\": %d, "
                  "\"texture_unlock_locality_index8_baseline_skips\": %d, "
                  "\"texture_unlock_locality_index8_non_index8_skips\": %d, "
                  "\"texture_unlock_locality_index8_source_pixels\": %llu, "
                  "\"texture_unlock_locality_index8_changed_pixels\": %llu, "
                  "\"texture_unlock_locality_index8_changed_rows\": %llu, "
                  "\"texture_unlock_locality_index8_changed_bbox_pixels\": %llu, "
                  "\"texture_unlock_invalidation_ms\": %.4f, "
                  "\"palette_unlock_invalidation_ms\": %.4f, "
                  "\"texture_cache_evictions\": %d, \"palette_cache_evictions\": %d, "
                  "\"software_surface_cache_hits\": %d, \"software_surface_cache_creates\": %d, "
                  "\"software_surface_cache_refresh_attempts\": %d, "
                  "\"software_surface_cache_refresh_unique_bindings\": %d, "
                  "\"software_surface_cache_refresh_repeat_binding_attempts\": %d, "
                  "\"software_surface_cache_refresh_unique_texture_handles\": %d, "
                  "\"software_surface_cache_refresh_texture_handle_fanout_max\": %d, "
                  "\"software_surface_cache_refresh_failures\": %d, "
                  "\"software_surface_cache_refresh_ms\": %.4f, "
                  "\"software_surface_cache_refresh_palette_set_calls\": %d, "
                  "\"software_surface_cache_refresh_palette_set_ms\": %.4f, "
                  "\"software_surface_cache_refresh_blit_calls\": %d, "
                  "\"software_surface_cache_refresh_blit_ms\": %.4f, "
                  "\"software_surface_cache_create_dirty_texture_same_frame\": %d, "
                  "\"software_surface_cache_create_dirty_texture_carried\": %d, "
                  "\"software_surface_cache_create_dirty_palette_same_frame\": %d, "
                  "\"software_surface_cache_create_dirty_palette_carried\": %d, "
                  "\"software_surface_cache_create_cold\": %d, "
                  "\"software_surface_cache_texture_evictions\": %d, "
                  "\"software_surface_cache_palette_evictions\": %d, \"textures_destroy_queued\": %d, "
                  "\"unknown_tasks\": %d, \"ppg_tasks\": %d, \"mtrans_tasks\": %d, "
                  "\"ui_direct_tasks\": %d, \"solid_tasks\": %d, "
                  "\"software_frame_mode_enabled\": %d, \"software_frame_surface_ready\": %d, "
                  "\"software_frame_active\": %d, \"software_frame_owned\": %d, "
                  "\"software_frame_direct_present\": %d, \"software_frame_uploaded\": %d, "
                  "\"software_frame_fallback\": %d, "
                  "\"software_frame_candidate_tasks\": %d, \"software_frame_candidate_pixels\": %llu, "
                  "\"software_frame_fallback_tasks\": %d, \"software_frame_fallback_pixels\": %llu, "
                  "\"software_frame_fast_exact_tasks\": %d, \"software_frame_fast_exact_pixels\": %llu, "
                  "\"software_frame_fast_exact_clipped_tasks\": %d, "
                  "\"software_frame_fast_exact_flipped_tasks\": %d, "
                  "\"software_frame_fast_exact_color_mod_tasks\": %d, "
                  "\"software_frame_fast_exact_color_mod_pixels\": %llu, "
                  "\"software_frame_fast_scaled_tasks\": %d, "
                  "\"software_frame_fast_scaled_pixels\": %llu, "
                  "\"software_frame_fast_non_integer_tasks\": %d, "
                  "\"software_frame_fast_non_integer_pixels\": %llu, "
                  "\"software_frame_fast_non_integer_lookup_entries\": %llu, "
                  "\"software_frame_fast_non_integer_alpha_only_tasks\": %d, "
                  "\"software_frame_fast_non_integer_alpha_only_pixels\": %llu, "
                  "\"software_frame_fast_non_integer_rgb_mod_tasks\": %d, "
                  "\"software_frame_fast_non_integer_rgb_mod_pixels\": %llu, "
                  "\"software_frame_generic_textured_tasks\": %d, "
                  "\"software_frame_generic_textured_pixels\": %llu, "
                  "\"software_frame_generic_textured_alpha_only_tasks\": %d, "
                  "\"software_frame_generic_textured_alpha_only_pixels\": %llu, "
                  "\"software_frame_generic_textured_rgb_mod_tasks\": %d, "
                  "\"software_frame_generic_textured_rgb_mod_pixels\": %llu, "
                  "\"software_frame_fast_miss_color_mod\": %d, "
                  "\"software_frame_fast_miss_non_integer\": %d, "
                  "\"software_frame_fast_miss_non_integer_lookup_entries\": %llu, "
                  "\"software_frame_fast_miss_non_integer_ge_256_tasks\": %d, "
                  "\"software_frame_fast_miss_non_integer_ge_256_pixels\": %llu, "
                  "\"software_frame_fast_miss_non_integer_ge_256_lookup_entries\": %llu, "
                  "\"software_frame_fast_miss_non_integer_ge_1024_tasks\": %d, "
                  "\"software_frame_fast_miss_non_integer_ge_1024_pixels\": %llu, "
                  "\"software_frame_fast_miss_non_integer_max_pixels\": %llu, "
                  "\"software_frame_fast_miss_scaled\": %d, "
                  "\"software_frame_fast_miss_unsupported_flip\": %d, "
                  "\"software_frame_fast_miss_source_bounds\": %d, "
                  "\"software_frame_reason_alpha\": %d, \"software_frame_reason_color_mod\": %d, "
                  "\"software_frame_reason_geometry\": %d, \"software_frame_reason_solid\": %d, "
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
                  sample->texture_cache_miss_dirty_texture_same_frame,
                  sample->texture_cache_miss_dirty_texture_carried,
                  sample->texture_cache_miss_dirty_palette_same_frame,
                  sample->texture_cache_miss_dirty_palette_carried,
                  sample->texture_cache_miss_cold,
                  sample->texture_creates,
                  sample->texture_unlock_calls,
                  sample->palette_unlock_calls,
                  sample->texture_unlock_dirty_surface_variants,
                  sample->texture_unlock_dirty_surface_variants_max,
                  sample->palette_unlock_dirty_surface_variants,
                  sample->palette_unlock_dirty_surface_variants_max,
                  sample->texture_unlock_locality_index8_tracked,
                  sample->texture_unlock_locality_index8_baseline_skips,
                  sample->texture_unlock_locality_index8_non_index8_skips,
                  (unsigned long long)sample->texture_unlock_locality_index8_source_pixels,
                  (unsigned long long)sample->texture_unlock_locality_index8_changed_pixels,
                  (unsigned long long)sample->texture_unlock_locality_index8_changed_rows,
                  (unsigned long long)sample->texture_unlock_locality_index8_changed_bbox_pixels,
                  sample->texture_unlock_invalidation_ms,
                  sample->palette_unlock_invalidation_ms,
                  sample->texture_cache_evictions,
                  sample->palette_cache_evictions,
                  sample->software_surface_cache_hits,
                  sample->software_surface_cache_creates,
                  sample->software_surface_cache_refresh_attempts,
                  sample->software_surface_cache_refresh_unique_bindings,
                  sample->software_surface_cache_refresh_repeat_binding_attempts,
                  sample->software_surface_cache_refresh_unique_texture_handles,
                  sample->software_surface_cache_refresh_texture_handle_fanout_max,
                  sample->software_surface_cache_refresh_failures,
                  sample->software_surface_cache_refresh_ms,
                  sample->software_surface_cache_refresh_palette_set_calls,
                  sample->software_surface_cache_refresh_palette_set_ms,
                  sample->software_surface_cache_refresh_blit_calls,
                  sample->software_surface_cache_refresh_blit_ms,
                  sample->software_surface_cache_create_dirty_texture_same_frame,
                  sample->software_surface_cache_create_dirty_texture_carried,
                  sample->software_surface_cache_create_dirty_palette_same_frame,
                  sample->software_surface_cache_create_dirty_palette_carried,
                  sample->software_surface_cache_create_cold,
                  sample->software_surface_cache_texture_evictions,
                  sample->software_surface_cache_palette_evictions,
                  sample->textures_destroy_queued,
                  sample->unknown_tasks,
                  sample->ppg_tasks,
                  sample->mtrans_tasks,
                  sample->ui_direct_tasks,
                  sample->solid_tasks,
                  sample->software_frame_mode_enabled,
                  sample->software_frame_surface_ready,
                  sample->software_frame_owned,
                  sample->software_frame_owned,
                  sample->software_frame_direct_present,
                  sample->software_frame_uploaded,
                  sample->software_frame_fallback,
                  sample->software_frame_candidate_tasks,
                  (unsigned long long)sample->software_frame_candidate_pixels,
                  sample->software_frame_fallback_tasks,
                  (unsigned long long)sample->software_frame_fallback_pixels,
                  sample->software_frame_fast_exact_tasks,
                  (unsigned long long)sample->software_frame_fast_exact_pixels,
                  sample->software_frame_fast_exact_clipped_tasks,
                  sample->software_frame_fast_exact_flipped_tasks,
                  sample->software_frame_fast_exact_color_mod_tasks,
                  (unsigned long long)sample->software_frame_fast_exact_color_mod_pixels,
                  sample->software_frame_fast_scaled_tasks,
                  (unsigned long long)sample->software_frame_fast_scaled_pixels,
                  sample->software_frame_fast_non_integer_tasks,
                  (unsigned long long)sample->software_frame_fast_non_integer_pixels,
                  (unsigned long long)sample->software_frame_fast_non_integer_lookup_entries,
                  sample->software_frame_fast_non_integer_alpha_only_tasks,
                  (unsigned long long)sample->software_frame_fast_non_integer_alpha_only_pixels,
                  sample->software_frame_fast_non_integer_rgb_mod_tasks,
                  (unsigned long long)sample->software_frame_fast_non_integer_rgb_mod_pixels,
                  sample->software_frame_generic_textured_tasks,
                  (unsigned long long)sample->software_frame_generic_textured_pixels,
                  sample->software_frame_generic_textured_alpha_only_tasks,
                  (unsigned long long)sample->software_frame_generic_textured_alpha_only_pixels,
                  sample->software_frame_generic_textured_rgb_mod_tasks,
                  (unsigned long long)sample->software_frame_generic_textured_rgb_mod_pixels,
                  sample->software_frame_fast_miss_color_mod,
                  sample->software_frame_fast_miss_non_integer,
                  (unsigned long long)sample->software_frame_fast_miss_non_integer_lookup_entries,
                  sample->software_frame_fast_miss_non_integer_ge_256_tasks,
                  (unsigned long long)sample->software_frame_fast_miss_non_integer_ge_256_pixels,
                  (unsigned long long)sample->software_frame_fast_miss_non_integer_ge_256_lookup_entries,
                  sample->software_frame_fast_miss_non_integer_ge_1024_tasks,
                  (unsigned long long)sample->software_frame_fast_miss_non_integer_ge_1024_pixels,
                  (unsigned long long)sample->software_frame_fast_miss_non_integer_max_pixels,
                  sample->software_frame_fast_miss_scaled,
                  sample->software_frame_fast_miss_unsupported_flip,
                  sample->software_frame_fast_miss_source_bounds,
                  sample->software_frame_reason_alpha,
                  sample->software_frame_reason_color_mod,
                  sample->software_frame_reason_geometry,
                  sample->software_frame_reason_solid,
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

    backend_logf("PERF capture complete: frames=%d frame_time_ms=%.3f render_ms=%.3f present_ms=%.3f present_readback_ms=%.3f present_convert_ms=%.3f present_copy_ms=%.3f present_clear_ms=%.3f dominant_present_path=%s readback_format=%s readback_size=%dx%d copy_bytes=%.2f dirty_ratio=%.4f dirty_hit_rate=%.4f full_copy_fallback_ratio=%.4f rect_runs=%.2f rect_multi_runs=%.2f rect_multi_run_tasks=%.2f rect_max_run=%.2f rect_hstrip_runs=%.2f rect_hstrip_tasks=%.2f rect_vstrip_runs=%.2f rect_vstrip_tasks=%.2f rect_run_links=%.2f rect_color_breaks=%.2f rect_flip_breaks=%.2f rect_flipped_tasks=%.2f textured_geometry_tasks=%.2f textured_geometry_recovered=%.2f textured_geometry_fallback=%.2f set_texture_calls=%.2f binding_reuse=%.2f texture_unlocks=%.2f palette_unlocks=%.2f texture_evictions=%.2f palette_evictions=%.2f destroy_queue=%.2f source_ppg=%.2f source_mtrans=%.2f source_ui=%.2f source_solid=%.2f source_unknown=%.2f software_frame_mode_enabled_ratio=%.4f software_frame_surface_ready_ratio=%.4f software_frame_active_ratio=%.4f software_frame_owned_ratio=%.4f software_frame_direct_present_ratio=%.4f software_frame_uploaded_ratio=%.4f software_frame_fallback_ratio=%.4f software_frame_candidate_tasks=%.2f software_frame_candidate_pixels=%.2f software_frame_fallback_tasks=%.2f software_frame_fallback_pixels=%.2f software_frame_fast_exact_tasks=%.2f software_frame_fast_exact_pixels=%.2f software_frame_fast_exact_clipped_tasks=%.2f software_frame_fast_exact_flipped_tasks=%.2f software_frame_fast_exact_color_mod_tasks=%.2f software_frame_fast_exact_color_mod_pixels=%.2f software_frame_fast_scaled_tasks=%.2f software_frame_fast_scaled_pixels=%.2f software_frame_fast_non_integer_tasks=%.2f software_frame_fast_non_integer_pixels=%.2f software_frame_fast_non_integer_lookup_entries=%.2f software_frame_generic_textured_tasks=%.2f software_frame_generic_textured_pixels=%.2f software_frame_fast_miss_color_mod=%.2f software_frame_fast_miss_non_integer=%.2f software_frame_fast_miss_non_integer_lookup_entries=%.2f software_frame_fast_miss_non_integer_ge_256_tasks=%.2f software_frame_fast_miss_non_integer_ge_256_pixels=%.2f software_frame_fast_miss_non_integer_ge_256_lookup_entries=%.2f software_frame_fast_miss_non_integer_ge_1024_tasks=%.2f software_frame_fast_miss_non_integer_ge_1024_pixels=%.2f software_frame_fast_miss_non_integer_max_pixels=%.2f software_frame_fast_miss_scaled=%.2f software_frame_fast_miss_unsupported_flip=%.2f software_frame_fast_miss_source_bounds=%.2f software_frame_reason_alpha=%.2f software_frame_reason_color_mod=%.2f software_frame_reason_geometry=%.2f software_frame_reason_solid=%.2f hybrid_candidate_tasks=%.2f hybrid_candidate_pixels=%.2f hybrid_fallback_tasks=%.2f hybrid_fallback_pixels=%.2f hybrid_reason_clip=%.2f hybrid_reason_alpha=%.2f hybrid_reason_color_mod=%.2f hybrid_reason_flip=%.2f hybrid_reason_geometry=%.2f hybrid_reason_solid=%.2f fps=%.2f output=%s",
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
                 software_frame_mode_enabled_ratio,
                 software_frame_surface_ready_ratio,
                 software_frame_owned_ratio,
                 software_frame_owned_ratio,
                 software_frame_direct_present_ratio,
                 software_frame_uploaded_ratio,
                 software_frame_fallback_ratio,
                 avg_software_frame_candidate_tasks,
                 avg_software_frame_candidate_pixels,
                 avg_software_frame_fallback_tasks,
                 avg_software_frame_fallback_pixels,
                 avg_software_frame_fast_exact_tasks,
                 avg_software_frame_fast_exact_pixels,
                 avg_software_frame_fast_exact_clipped_tasks,
                 avg_software_frame_fast_exact_flipped_tasks,
                 avg_software_frame_fast_exact_color_mod_tasks,
                 avg_software_frame_fast_exact_color_mod_pixels,
                 avg_software_frame_fast_scaled_tasks,
                 avg_software_frame_fast_scaled_pixels,
                 avg_software_frame_fast_non_integer_tasks,
                 avg_software_frame_fast_non_integer_pixels,
                 avg_software_frame_fast_non_integer_lookup_entries,
                 avg_software_frame_generic_textured_tasks,
                 avg_software_frame_generic_textured_pixels,
                 avg_software_frame_fast_miss_color_mod,
                 avg_software_frame_fast_miss_non_integer,
                 avg_software_frame_fast_miss_non_integer_lookup_entries,
                 avg_software_frame_fast_miss_non_integer_ge_256_tasks,
                 avg_software_frame_fast_miss_non_integer_ge_256_pixels,
                 avg_software_frame_fast_miss_non_integer_ge_256_lookup_entries,
                 avg_software_frame_fast_miss_non_integer_ge_1024_tasks,
                 avg_software_frame_fast_miss_non_integer_ge_1024_pixels,
                 avg_software_frame_fast_miss_non_integer_max_pixels,
                 avg_software_frame_fast_miss_scaled,
                 avg_software_frame_fast_miss_unsupported_flip,
                 avg_software_frame_fast_miss_source_bounds,
                 avg_software_frame_reason_alpha,
                 avg_software_frame_reason_color_mod,
                 avg_software_frame_reason_geometry,
                 avg_software_frame_reason_solid,
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
    for (int i = 0; i < raster_bucket_timing_count; i++) {
        const SDLGameRenderer_PerfCaptureRasterBucketTiming* entry = &raster_bucket_timings[i];
        const double sampled_total_ms = (double)entry->sampled_ns / 1e6;
        const double sampled_mean_ms =
            entry->sampled_calls > 0 ? sampled_total_ms / (double)entry->sampled_calls : 0.0;
        backend_logf("PERF capture raster bucket: bucket=%s sample_period=%llu sampled_calls=%llu sampled_pixels=%llu sampled_total_ms=%.4f sampled_mean_ms=%.6f",
                     software_frame_raster_bucket_name(entry->bucket),
                     (unsigned long long)entry->sample_period,
                     (unsigned long long)entry->sampled_calls,
                     (unsigned long long)entry->sampled_pixels,
                     sampled_total_ms,
                     sampled_mean_ms);
    }
    SDL_free(output_path);
}
#else
static bool perf_capture_collect_extended_stats(void) {
    return false;
}
#endif

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

static const char* software_frame_mode_name(void) {
    return software_frame_mode_enabled ? "on" : "off";
}

static void init_software_frame_mode(void) {
    const char* raw_mode = Config_GetString(CFG_KEY_SOFTWARE_FRAME_MODE);
    software_frame_mode_enabled = false;

    if ((raw_mode == NULL) || (raw_mode[0] == '\0')) {
        return;
    }

    if ((SDL_strcasecmp(raw_mode, "on") == 0) || (SDL_strcasecmp(raw_mode, "true") == 0)) {
        software_frame_mode_enabled = true;
    }
}

static void init_show_fps_overlay(void) {
    show_fps_overlay = Config_GetBool(CFG_KEY_SHOW_FPS);
    fps_overlay_window_start_ns = 0;
    fps_overlay_window_frames = 0;
    fps_overlay_value = 0;
    fps_overlay_label[0] = '\0';
}

static void publish_fps_overlay_label(void) {
    if (!show_fps_overlay) {
        fps_overlay_label[0] = '\0';
        if (fbdev_presenter_enabled) {
            FBDevPresenter_SetFPSOverlayText(NULL);
        }
        return;
    }

    SDL_snprintf(fps_overlay_label, sizeof(fps_overlay_label), "%d FPS", fps_overlay_value);
    if (fbdev_presenter_enabled) {
        FBDevPresenter_SetFPSOverlayText(fps_overlay_label);
    }
}

static void update_fps_overlay(Uint64 frame_end_ns) {
    if (!show_fps_overlay) {
        return;
    }

    if (fps_overlay_window_start_ns == 0) {
        fps_overlay_window_start_ns = frame_end_ns;
        fps_overlay_window_frames = 1;
        return;
    }

    fps_overlay_window_frames += 1;
    const Uint64 elapsed_ns = frame_end_ns - fps_overlay_window_start_ns;
    if (elapsed_ns < 250000000ULL) {
        return;
    }

    const int measured_fps =
        (int)((((double)fps_overlay_window_frames * 1000000000.0) / (double)elapsed_ns) + 0.5);
    fps_overlay_window_start_ns = frame_end_ns;
    fps_overlay_window_frames = 0;

    if ((measured_fps == fps_overlay_value) && (fps_overlay_label[0] != '\0')) {
        return;
    }

    fps_overlay_value = measured_fps;
    publish_fps_overlay_label();
}

int SDLApp_Init() {
    Config_Init();
    Keymap_Init();
    init_scalemode();
    init_software_frame_mode();
    init_show_fps_overlay();

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
    FBDevPresenter_SetFPSOverlayEnabled(show_fps_overlay);
    backend_logf("FPS overlay: %s", show_fps_overlay ? "enabled" : "disabled");

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
    SDLGameRenderer_SetSoftwareFrameMode(software_frame_mode_enabled);
    backend_logf("Software frame mode: %s", software_frame_mode_name());

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
#if ENABLE_PERF_TELEMETRY
    perf_capture_reset_storage();
#endif
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
#if ENABLE_PERF_TELEMETRY
    perf_frame_start_ns = SDL_GetTicksNS();
    perf_update_start_ns = perf_frame_start_ns;
#endif

    if (fbdev_presenter_enabled) {
        FBDevPresenter_BeginFrameStats(perf_capture_collect_extended_stats());
    }

    SDLMessageRenderer_BeginFrame();
    SDLGameRenderer_BeginFrame(perf_capture_collect_extended_stats());
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

static void render_renderer_fps_overlay(const SDL_FRect* content_rect) {
    if (!show_fps_overlay || fbdev_presenter_enabled || (fps_overlay_label[0] == '\0')) {
        return;
    }

    int output_w = 0;
    int output_h = 0;
    if (!SDL_GetRenderOutputSize(renderer, &output_w, &output_h)) {
        return;
    }

    SDL_FRect draw_rect = { 0.0f, 0.0f, (float)output_w, (float)output_h };
    if ((content_rect != NULL) && (content_rect->w > 0.0f) && (content_rect->h > 0.0f)) {
        draw_rect = *content_rect;
    }

    const int scale = draw_rect.h >= 420.0f ? 2 : 1;
    const int text_len = (int)SDL_strlen(fps_overlay_label);
    const int text_w = text_len * 8 * scale;
    const int text_h = 8 * scale;
    const int margin = SDL_max(10, scale * 4);
    const float draw_x = draw_rect.x + ((draw_rect.w - (float)text_w) * 0.5f);
    const float draw_y = (draw_rect.y + draw_rect.h) - (float)text_h - (float)margin;

    SDL_SetRenderScale(renderer, (float)scale, (float)scale);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderDebugText(renderer, (draw_x + 1.0f) / (float)scale, (draw_y + 1.0f) / (float)scale, fps_overlay_label);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, draw_x / (float)scale, draw_y / (float)scale, fps_overlay_label);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

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
#if ENABLE_PERF_TELEMETRY
    const Uint64 render_start_ns = SDL_GetTicksNS();
    const Uint64 update_ns = render_start_ns > perf_update_start_ns ? (render_start_ns - perf_update_start_ns) : 0;
#endif

    // Run sound processing
    ADX_ProcessTracks();

    // Render

    // This should come before SDLGameRenderer_RenderFrame,
    // because NetstatsRenderer uses the existing SFIII rendering pipeline.
#if defined(ENABLE_NETPLAY)
    NetplayScreen_Render();
    NetstatsRenderer_Render();
#endif
    const bool has_message_content = SDLMessageRenderer_HasContent();
#if defined(DEBUG)
    const bool force_full_fbdev_readback = true;
#else
    const bool force_full_fbdev_readback = false;
#endif
    const bool prefer_software_frame_direct_present =
        use_native_render_path && fbdev_presenter_enabled && use_fbdev_only_present && !has_message_content &&
        !should_save_screenshot;
    SDLGameRenderer_SetSoftwareFrameDirectPresentMode(prefer_software_frame_direct_present);
    SDLGameRenderer_RenderFrame();

    if (should_save_screenshot && SDLGameRenderer_HasSoftwareOwnedFrame()) {
        SDLGameRenderer_EnsureSoftwareFrameCanvas();
    }
    if (should_save_screenshot) {
        save_texture(cps3_canvas, "screenshot_cps3.bmp");
    }

    const SDL_FRect* fbdev_readback_rect = NULL;
    const SDL_FRect* onscreen_content_rect = NULL;
    SDL_FRect fbdev_readback_rect_value = { 0 };
    bool fbdev_present_current_target = false;
    bool fbdev_present_software_frame = false;
    const SDL_Surface* fbdev_software_frame_surface = NULL;

    if (use_native_render_path) {
        refresh_native_output_rect();
        onscreen_content_rect = &native_output_rect;
        const bool use_fbdev_software_frame_direct =
            prefer_software_frame_direct_present && SDLGameRenderer_HasSoftwareOwnedFrame();
        const bool use_fbdev_native_direct_target =
            fbdev_presenter_enabled && use_fbdev_only_present && !has_message_content && !force_full_fbdev_readback &&
            !use_fbdev_software_frame_direct;
        const bool use_fbdev_native_readback_rect =
            fbdev_presenter_enabled && use_fbdev_only_present && native_output_has_bars && !force_full_fbdev_readback &&
            !use_fbdev_native_direct_target;
        if (use_fbdev_software_frame_direct) {
            fbdev_present_software_frame = true;
            fbdev_software_frame_surface = SDLGameRenderer_GetSoftwareFrameSurface();
        } else if (use_fbdev_native_direct_target) {
            fbdev_present_current_target = true;
        } else if (use_fbdev_native_readback_rect) {
            fbdev_readback_rect_value = native_output_rect;
            fbdev_readback_rect = &fbdev_readback_rect_value;
        }

        if (!fbdev_present_current_target && !fbdev_present_software_frame) {
            if (SDLGameRenderer_HasSoftwareOwnedFrame()) {
                SDLGameRenderer_EnsureSoftwareFrameCanvas();
            }
            render_native_output_to_present_target(has_message_content, native_output_has_bars && !use_fbdev_native_readback_rect);
        }
    } else {
        SDL_SetRenderTarget(renderer, screen_texture);
        refresh_screen_output_rect();

        const SDL_FRect* dst_rect = &screen_output_rect;
        onscreen_content_rect = dst_rect;
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
#endif
    render_renderer_fps_overlay(onscreen_content_rect);

#if ENABLE_PERF_TELEMETRY
    const Uint64 present_start_ns = SDL_GetTicksNS();
#endif

    if (fbdev_presenter_enabled) {
        if (fbdev_present_software_frame) {
            if (!FBDevPresenter_PresentSurface(fbdev_software_frame_surface, &native_output_rect)) {
                backend_logf("FBDEV direct software-frame present failed; falling back to current-target/readback");
                fbdev_present_software_frame = false;
                SDLGameRenderer_EnsureSoftwareFrameCanvas();
                SDL_SetRenderTarget(renderer, cps3_canvas);
                if (!FBDevPresenter_PresentCurrentTarget(renderer, &native_output_rect)) {
                    backend_logf("FBDEV direct current-target present failed after software-frame fallback; falling back to composited readback");
                    if (native_output_has_bars) {
                        fbdev_readback_rect_value = native_output_rect;
                        fbdev_readback_rect = &fbdev_readback_rect_value;
                    } else {
                        fbdev_readback_rect = NULL;
                    }
                    render_native_output_to_present_target(false, native_output_has_bars && (fbdev_readback_rect == NULL));
                    FBDevPresenter_BeginFrameStats(perf_capture_collect_extended_stats());
                    FBDevPresenter_Present(renderer, fbdev_readback_rect);
                }
            } else {
                SDLGameRenderer_NoteSoftwareFrameDirectPresent();
            }
        } else if (fbdev_present_current_target) {
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
                FBDevPresenter_BeginFrameStats(perf_capture_collect_extended_stats());
                FBDevPresenter_Present(renderer, fbdev_readback_rect);
            }
        } else {
            FBDevPresenter_Present(renderer, fbdev_readback_rect);
        }
    }

    if (!use_fbdev_only_present) {
        SDL_RenderPresent(renderer);
    }
#if ENABLE_PERF_TELEMETRY
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
#endif

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
    update_fps_overlay(now);

#if ENABLE_PERF_TELEMETRY
    if (perf_capture_enabled && !perf_capture_completed && perf_capture_recorded_frames < perf_capture_target_frames) {
        PerfFrameSample* sample = &perf_samples[perf_capture_recorded_frames];
        note_perf_capture_test_state(perf_capture_recorded_frames);
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
        sample->texture_cache_miss_dirty_texture_same_frame = render_stats.texture_cache_miss_dirty_texture_same_frame;
        sample->texture_cache_miss_dirty_texture_carried = render_stats.texture_cache_miss_dirty_texture_carried;
        sample->texture_cache_miss_dirty_palette_same_frame = render_stats.texture_cache_miss_dirty_palette_same_frame;
        sample->texture_cache_miss_dirty_palette_carried = render_stats.texture_cache_miss_dirty_palette_carried;
        sample->texture_cache_miss_cold = render_stats.texture_cache_miss_cold;
        sample->texture_creates = render_stats.texture_creates;
        sample->texture_unlock_calls = render_stats.texture_unlock_calls;
        sample->palette_unlock_calls = render_stats.palette_unlock_calls;
        sample->palette_unlock_changed_calls = render_stats.palette_unlock_changed_calls;
        sample->palette_unlock_unchanged_calls = render_stats.palette_unlock_unchanged_calls;
        sample->texture_unlock_dirty_surface_variants = render_stats.texture_unlock_dirty_surface_variants;
        sample->texture_unlock_dirty_surface_variants_max = render_stats.texture_unlock_dirty_surface_variants_max;
        sample->palette_unlock_dirty_surface_variants = render_stats.palette_unlock_dirty_surface_variants;
        sample->palette_unlock_dirty_surface_variants_max = render_stats.palette_unlock_dirty_surface_variants_max;
        sample->texture_unlock_locality_index8_tracked = render_stats.texture_unlock_locality_index8_tracked;
        sample->texture_unlock_locality_index8_baseline_skips =
            render_stats.texture_unlock_locality_index8_baseline_skips;
        sample->texture_unlock_locality_index8_non_index8_skips =
            render_stats.texture_unlock_locality_index8_non_index8_skips;
        sample->texture_unlock_locality_index8_source_pixels =
            render_stats.texture_unlock_locality_index8_source_pixels;
        sample->texture_unlock_locality_index8_changed_pixels =
            render_stats.texture_unlock_locality_index8_changed_pixels;
        sample->texture_unlock_locality_index8_changed_rows =
            render_stats.texture_unlock_locality_index8_changed_rows;
        sample->texture_unlock_locality_index8_changed_bbox_pixels =
            render_stats.texture_unlock_locality_index8_changed_bbox_pixels;
        sample->texture_unlock_invalidation_ms = (double)render_stats.texture_unlock_invalidation_ns / 1e6;
        sample->palette_unlock_invalidation_ms = (double)render_stats.palette_unlock_invalidation_ns / 1e6;
        sample->texture_cache_evictions = render_stats.texture_cache_evictions;
        sample->palette_cache_evictions = render_stats.palette_cache_evictions;
        sample->software_surface_cache_hits = render_stats.software_surface_cache_hits;
        sample->software_surface_cache_creates = render_stats.software_surface_cache_creates;
        sample->software_surface_cache_refresh_attempts = render_stats.software_surface_cache_refresh_attempts;
        sample->software_surface_cache_refresh_unique_bindings =
            render_stats.software_surface_cache_refresh_unique_bindings;
        sample->software_surface_cache_refresh_repeat_binding_attempts =
            render_stats.software_surface_cache_refresh_repeat_binding_attempts;
        sample->software_surface_cache_refresh_unique_texture_handles =
            render_stats.software_surface_cache_refresh_unique_texture_handles;
        sample->software_surface_cache_refresh_texture_handle_fanout_max =
            render_stats.software_surface_cache_refresh_texture_handle_fanout_max;
        sample->software_surface_cache_refresh_failures = render_stats.software_surface_cache_refresh_failures;
        sample->software_surface_cache_refresh_ms = (double)render_stats.software_surface_cache_refresh_ns / 1e6;
        sample->software_surface_cache_refresh_palette_set_calls =
            render_stats.software_surface_cache_refresh_palette_set_calls;
        sample->software_surface_cache_refresh_palette_set_ms =
            (double)render_stats.software_surface_cache_refresh_palette_set_ns / 1e6;
        sample->software_surface_cache_refresh_blit_calls = render_stats.software_surface_cache_refresh_blit_calls;
        sample->software_surface_cache_refresh_blit_ms =
            (double)render_stats.software_surface_cache_refresh_blit_ns / 1e6;
        sample->software_surface_cache_create_dirty_texture_same_frame =
            render_stats.software_surface_cache_create_dirty_texture_same_frame;
        sample->software_surface_cache_create_dirty_texture_carried =
            render_stats.software_surface_cache_create_dirty_texture_carried;
        sample->software_surface_cache_create_dirty_palette_same_frame =
            render_stats.software_surface_cache_create_dirty_palette_same_frame;
        sample->software_surface_cache_create_dirty_palette_carried =
            render_stats.software_surface_cache_create_dirty_palette_carried;
        sample->software_surface_cache_create_cold = render_stats.software_surface_cache_create_cold;
        sample->software_surface_cache_texture_evictions = render_stats.software_surface_cache_texture_evictions;
        sample->software_surface_cache_palette_evictions = render_stats.software_surface_cache_palette_evictions;
        sample->textures_destroy_queued = render_stats.textures_destroy_queued;
        sample->unknown_tasks = render_stats.unknown_tasks;
        sample->ppg_tasks = render_stats.ppg_tasks;
        sample->mtrans_tasks = render_stats.mtrans_tasks;
        sample->ui_direct_tasks = render_stats.ui_direct_tasks;
        sample->solid_tasks = render_stats.solid_tasks;
        sample->software_frame_mode_enabled = render_stats.software_frame_mode_enabled;
        sample->software_frame_surface_ready = render_stats.software_frame_surface_ready;
        sample->software_frame_owned = render_stats.software_frame_owned;
        sample->software_frame_direct_present = render_stats.software_frame_direct_present;
        sample->software_frame_uploaded = render_stats.software_frame_uploaded;
        sample->software_frame_fallback = render_stats.software_frame_fallback;
        sample->software_frame_candidate_tasks = render_stats.software_frame_candidate_tasks;
        sample->software_frame_candidate_pixels = render_stats.software_frame_candidate_pixels;
        sample->software_frame_fallback_tasks = render_stats.software_frame_fallback_tasks;
        sample->software_frame_fallback_pixels = render_stats.software_frame_fallback_pixels;
        sample->software_frame_fast_exact_tasks = render_stats.software_frame_fast_exact_tasks;
        sample->software_frame_fast_exact_pixels = render_stats.software_frame_fast_exact_pixels;
        sample->software_frame_fast_exact_clipped_tasks = render_stats.software_frame_fast_exact_clipped_tasks;
        sample->software_frame_fast_exact_flipped_tasks = render_stats.software_frame_fast_exact_flipped_tasks;
        sample->software_frame_fast_exact_color_mod_tasks = render_stats.software_frame_fast_exact_color_mod_tasks;
        sample->software_frame_fast_exact_color_mod_pixels = render_stats.software_frame_fast_exact_color_mod_pixels;
        sample->software_frame_fast_scaled_tasks = render_stats.software_frame_fast_scaled_tasks;
        sample->software_frame_fast_scaled_pixels = render_stats.software_frame_fast_scaled_pixels;
        sample->software_frame_fast_non_integer_tasks = render_stats.software_frame_fast_non_integer_tasks;
        sample->software_frame_fast_non_integer_pixels = render_stats.software_frame_fast_non_integer_pixels;
        sample->software_frame_fast_non_integer_lookup_entries =
            render_stats.software_frame_fast_non_integer_lookup_entries;
        sample->software_frame_fast_non_integer_alpha_only_tasks =
            render_stats.software_frame_fast_non_integer_alpha_only_tasks;
        sample->software_frame_fast_non_integer_alpha_only_pixels =
            render_stats.software_frame_fast_non_integer_alpha_only_pixels;
        sample->software_frame_fast_non_integer_rgb_mod_tasks =
            render_stats.software_frame_fast_non_integer_rgb_mod_tasks;
        sample->software_frame_fast_non_integer_rgb_mod_pixels =
            render_stats.software_frame_fast_non_integer_rgb_mod_pixels;
        sample->software_frame_generic_textured_tasks = render_stats.software_frame_generic_textured_tasks;
        sample->software_frame_generic_textured_pixels = render_stats.software_frame_generic_textured_pixels;
        sample->software_frame_generic_textured_alpha_only_tasks =
            render_stats.software_frame_generic_textured_alpha_only_tasks;
        sample->software_frame_generic_textured_alpha_only_pixels =
            render_stats.software_frame_generic_textured_alpha_only_pixels;
        sample->software_frame_generic_textured_rgb_mod_tasks =
            render_stats.software_frame_generic_textured_rgb_mod_tasks;
        sample->software_frame_generic_textured_rgb_mod_pixels =
            render_stats.software_frame_generic_textured_rgb_mod_pixels;
        sample->software_frame_fast_miss_color_mod = render_stats.software_frame_fast_miss_color_mod;
        sample->software_frame_fast_miss_non_integer = render_stats.software_frame_fast_miss_non_integer;
        sample->software_frame_fast_miss_non_integer_lookup_entries =
            render_stats.software_frame_fast_miss_non_integer_lookup_entries;
        sample->software_frame_fast_miss_non_integer_ge_256_tasks =
            render_stats.software_frame_fast_miss_non_integer_ge_256_tasks;
        sample->software_frame_fast_miss_non_integer_ge_256_pixels =
            render_stats.software_frame_fast_miss_non_integer_ge_256_pixels;
        sample->software_frame_fast_miss_non_integer_ge_256_lookup_entries =
            render_stats.software_frame_fast_miss_non_integer_ge_256_lookup_entries;
        sample->software_frame_fast_miss_non_integer_ge_1024_tasks =
            render_stats.software_frame_fast_miss_non_integer_ge_1024_tasks;
        sample->software_frame_fast_miss_non_integer_ge_1024_pixels =
            render_stats.software_frame_fast_miss_non_integer_ge_1024_pixels;
        sample->software_frame_fast_miss_non_integer_max_pixels =
            render_stats.software_frame_fast_miss_non_integer_max_pixels;
        sample->software_frame_fast_miss_scaled = render_stats.software_frame_fast_miss_scaled;
        sample->software_frame_fast_miss_unsupported_flip = render_stats.software_frame_fast_miss_unsupported_flip;
        sample->software_frame_fast_miss_source_bounds = render_stats.software_frame_fast_miss_source_bounds;
        sample->software_frame_reason_alpha = render_stats.software_frame_reason_alpha;
        sample->software_frame_reason_color_mod = render_stats.software_frame_reason_color_mod;
        sample->software_frame_reason_geometry = render_stats.software_frame_reason_geometry;
        sample->software_frame_reason_solid = render_stats.software_frame_reason_solid;
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
        perf_texture_cache_miss_dirty_texture_same_frame_total +=
            (Uint64)render_stats.texture_cache_miss_dirty_texture_same_frame;
        perf_texture_cache_miss_dirty_texture_carried_total +=
            (Uint64)render_stats.texture_cache_miss_dirty_texture_carried;
        perf_texture_cache_miss_dirty_palette_same_frame_total +=
            (Uint64)render_stats.texture_cache_miss_dirty_palette_same_frame;
        perf_texture_cache_miss_dirty_palette_carried_total +=
            (Uint64)render_stats.texture_cache_miss_dirty_palette_carried;
        perf_texture_cache_miss_cold_total += (Uint64)render_stats.texture_cache_miss_cold;
        perf_texture_creates_total += (Uint64)render_stats.texture_creates;
        perf_texture_unlock_calls_total += (Uint64)render_stats.texture_unlock_calls;
        perf_palette_unlock_calls_total += (Uint64)render_stats.palette_unlock_calls;
        perf_palette_unlock_changed_calls_total += (Uint64)render_stats.palette_unlock_changed_calls;
        perf_palette_unlock_unchanged_calls_total += (Uint64)render_stats.palette_unlock_unchanged_calls;
        perf_texture_unlock_dirty_surface_variants_total +=
            (Uint64)render_stats.texture_unlock_dirty_surface_variants;
        perf_texture_unlock_dirty_surface_variants_max_total +=
            (Uint64)render_stats.texture_unlock_dirty_surface_variants_max;
        perf_palette_unlock_dirty_surface_variants_total +=
            (Uint64)render_stats.palette_unlock_dirty_surface_variants;
        perf_palette_unlock_dirty_surface_variants_max_total +=
            (Uint64)render_stats.palette_unlock_dirty_surface_variants_max;
        perf_texture_unlock_locality_index8_tracked_total +=
            (Uint64)render_stats.texture_unlock_locality_index8_tracked;
        perf_texture_unlock_locality_index8_baseline_skips_total +=
            (Uint64)render_stats.texture_unlock_locality_index8_baseline_skips;
        perf_texture_unlock_locality_index8_non_index8_skips_total +=
            (Uint64)render_stats.texture_unlock_locality_index8_non_index8_skips;
        perf_texture_unlock_locality_index8_source_pixels_total +=
            render_stats.texture_unlock_locality_index8_source_pixels;
        perf_texture_unlock_locality_index8_changed_pixels_total +=
            render_stats.texture_unlock_locality_index8_changed_pixels;
        perf_texture_unlock_locality_index8_changed_rows_total +=
            render_stats.texture_unlock_locality_index8_changed_rows;
        perf_texture_unlock_locality_index8_changed_bbox_pixels_total +=
            render_stats.texture_unlock_locality_index8_changed_bbox_pixels;
        perf_texture_unlock_invalidation_ns_total += render_stats.texture_unlock_invalidation_ns;
        perf_palette_unlock_invalidation_ns_total += render_stats.palette_unlock_invalidation_ns;
        perf_texture_cache_evictions_total += (Uint64)render_stats.texture_cache_evictions;
        perf_palette_cache_evictions_total += (Uint64)render_stats.palette_cache_evictions;
        perf_software_surface_cache_hits_total += (Uint64)render_stats.software_surface_cache_hits;
        perf_software_surface_cache_creates_total += (Uint64)render_stats.software_surface_cache_creates;
        perf_software_surface_cache_refresh_attempts_total +=
            (Uint64)render_stats.software_surface_cache_refresh_attempts;
        perf_software_surface_cache_refresh_unique_bindings_total +=
            (Uint64)render_stats.software_surface_cache_refresh_unique_bindings;
        perf_software_surface_cache_refresh_repeat_binding_attempts_total +=
            (Uint64)render_stats.software_surface_cache_refresh_repeat_binding_attempts;
        perf_software_surface_cache_refresh_unique_texture_handles_total +=
            (Uint64)render_stats.software_surface_cache_refresh_unique_texture_handles;
        perf_software_surface_cache_refresh_texture_handle_fanout_max_total +=
            (Uint64)render_stats.software_surface_cache_refresh_texture_handle_fanout_max;
        perf_software_surface_cache_refresh_failures_total +=
            (Uint64)render_stats.software_surface_cache_refresh_failures;
        perf_software_surface_cache_refresh_ns_total += render_stats.software_surface_cache_refresh_ns;
        perf_software_surface_cache_refresh_palette_set_calls_total +=
            (Uint64)render_stats.software_surface_cache_refresh_palette_set_calls;
        perf_software_surface_cache_refresh_palette_set_ns_total +=
            render_stats.software_surface_cache_refresh_palette_set_ns;
        perf_software_surface_cache_refresh_blit_calls_total +=
            (Uint64)render_stats.software_surface_cache_refresh_blit_calls;
        perf_software_surface_cache_refresh_blit_ns_total += render_stats.software_surface_cache_refresh_blit_ns;
        perf_software_surface_cache_create_dirty_texture_same_frame_total +=
            (Uint64)render_stats.software_surface_cache_create_dirty_texture_same_frame;
        perf_software_surface_cache_create_dirty_texture_carried_total +=
            (Uint64)render_stats.software_surface_cache_create_dirty_texture_carried;
        perf_software_surface_cache_create_dirty_palette_same_frame_total +=
            (Uint64)render_stats.software_surface_cache_create_dirty_palette_same_frame;
        perf_software_surface_cache_create_dirty_palette_carried_total +=
            (Uint64)render_stats.software_surface_cache_create_dirty_palette_carried;
        perf_software_surface_cache_create_cold_total += (Uint64)render_stats.software_surface_cache_create_cold;
        perf_software_surface_cache_texture_evictions_total +=
            (Uint64)render_stats.software_surface_cache_texture_evictions;
        perf_software_surface_cache_palette_evictions_total +=
            (Uint64)render_stats.software_surface_cache_palette_evictions;
        perf_textures_destroy_queued_total += (Uint64)render_stats.textures_destroy_queued;
        perf_unknown_tasks_total += (Uint64)render_stats.unknown_tasks;
        perf_ppg_tasks_total += (Uint64)render_stats.ppg_tasks;
        perf_mtrans_tasks_total += (Uint64)render_stats.mtrans_tasks;
        perf_ui_direct_tasks_total += (Uint64)render_stats.ui_direct_tasks;
        perf_solid_tasks_total += (Uint64)render_stats.solid_tasks;
        perf_software_frame_mode_enabled_frames += (Uint64)render_stats.software_frame_mode_enabled;
        perf_software_frame_surface_ready_frames += (Uint64)render_stats.software_frame_surface_ready;
        perf_software_frame_owned_frames += (Uint64)render_stats.software_frame_owned;
        perf_software_frame_direct_present_frames += (Uint64)render_stats.software_frame_direct_present;
        perf_software_frame_uploaded_frames += (Uint64)render_stats.software_frame_uploaded;
        perf_software_frame_fallback_frames += (Uint64)render_stats.software_frame_fallback;
        perf_software_frame_candidate_tasks_total += (Uint64)render_stats.software_frame_candidate_tasks;
        perf_software_frame_candidate_pixels_total += render_stats.software_frame_candidate_pixels;
        perf_software_frame_fallback_tasks_total += (Uint64)render_stats.software_frame_fallback_tasks;
        perf_software_frame_fallback_pixels_total += render_stats.software_frame_fallback_pixels;
        perf_software_frame_fast_exact_tasks_total += (Uint64)render_stats.software_frame_fast_exact_tasks;
        perf_software_frame_fast_exact_pixels_total += render_stats.software_frame_fast_exact_pixels;
        perf_software_frame_fast_exact_clipped_tasks_total +=
            (Uint64)render_stats.software_frame_fast_exact_clipped_tasks;
        perf_software_frame_fast_exact_flipped_tasks_total +=
            (Uint64)render_stats.software_frame_fast_exact_flipped_tasks;
        perf_software_frame_fast_exact_color_mod_tasks_total +=
            (Uint64)render_stats.software_frame_fast_exact_color_mod_tasks;
        perf_software_frame_fast_exact_color_mod_pixels_total +=
            render_stats.software_frame_fast_exact_color_mod_pixels;
        perf_software_frame_fast_scaled_tasks_total += (Uint64)render_stats.software_frame_fast_scaled_tasks;
        perf_software_frame_fast_scaled_pixels_total += render_stats.software_frame_fast_scaled_pixels;
        perf_software_frame_fast_non_integer_tasks_total +=
            (Uint64)render_stats.software_frame_fast_non_integer_tasks;
        perf_software_frame_fast_non_integer_pixels_total += render_stats.software_frame_fast_non_integer_pixels;
        perf_software_frame_fast_non_integer_lookup_entries_total +=
            render_stats.software_frame_fast_non_integer_lookup_entries;
        perf_software_frame_fast_non_integer_alpha_only_tasks_total +=
            (Uint64)render_stats.software_frame_fast_non_integer_alpha_only_tasks;
        perf_software_frame_fast_non_integer_alpha_only_pixels_total +=
            render_stats.software_frame_fast_non_integer_alpha_only_pixels;
        perf_software_frame_fast_non_integer_rgb_mod_tasks_total +=
            (Uint64)render_stats.software_frame_fast_non_integer_rgb_mod_tasks;
        perf_software_frame_fast_non_integer_rgb_mod_pixels_total +=
            render_stats.software_frame_fast_non_integer_rgb_mod_pixels;
        perf_software_frame_generic_textured_tasks_total +=
            (Uint64)render_stats.software_frame_generic_textured_tasks;
        perf_software_frame_generic_textured_pixels_total += render_stats.software_frame_generic_textured_pixels;
        perf_software_frame_generic_textured_alpha_only_tasks_total +=
            (Uint64)render_stats.software_frame_generic_textured_alpha_only_tasks;
        perf_software_frame_generic_textured_alpha_only_pixels_total +=
            render_stats.software_frame_generic_textured_alpha_only_pixels;
        perf_software_frame_generic_textured_rgb_mod_tasks_total +=
            (Uint64)render_stats.software_frame_generic_textured_rgb_mod_tasks;
        perf_software_frame_generic_textured_rgb_mod_pixels_total +=
            render_stats.software_frame_generic_textured_rgb_mod_pixels;
        perf_software_frame_fast_miss_color_mod_total += (Uint64)render_stats.software_frame_fast_miss_color_mod;
        perf_software_frame_fast_miss_non_integer_total +=
            (Uint64)render_stats.software_frame_fast_miss_non_integer;
        perf_software_frame_fast_miss_non_integer_lookup_entries_total +=
            render_stats.software_frame_fast_miss_non_integer_lookup_entries;
        perf_software_frame_fast_miss_non_integer_ge_256_tasks_total +=
            (Uint64)render_stats.software_frame_fast_miss_non_integer_ge_256_tasks;
        perf_software_frame_fast_miss_non_integer_ge_256_pixels_total +=
            render_stats.software_frame_fast_miss_non_integer_ge_256_pixels;
        perf_software_frame_fast_miss_non_integer_ge_256_lookup_entries_total +=
            render_stats.software_frame_fast_miss_non_integer_ge_256_lookup_entries;
        perf_software_frame_fast_miss_non_integer_ge_1024_tasks_total +=
            (Uint64)render_stats.software_frame_fast_miss_non_integer_ge_1024_tasks;
        perf_software_frame_fast_miss_non_integer_ge_1024_pixels_total +=
            render_stats.software_frame_fast_miss_non_integer_ge_1024_pixels;
        perf_software_frame_fast_miss_non_integer_max_pixels_total +=
            render_stats.software_frame_fast_miss_non_integer_max_pixels;
        perf_software_frame_fast_miss_scaled_total += (Uint64)render_stats.software_frame_fast_miss_scaled;
        perf_software_frame_fast_miss_unsupported_flip_total +=
            (Uint64)render_stats.software_frame_fast_miss_unsupported_flip;
        perf_software_frame_fast_miss_source_bounds_total +=
            (Uint64)render_stats.software_frame_fast_miss_source_bounds;
        perf_software_frame_reason_alpha_total += (Uint64)render_stats.software_frame_reason_alpha;
        perf_software_frame_reason_color_mod_total += (Uint64)render_stats.software_frame_reason_color_mod;
        perf_software_frame_reason_geometry_total += (Uint64)render_stats.software_frame_reason_geometry;
        perf_software_frame_reason_solid_total += (Uint64)render_stats.software_frame_reason_solid;
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
            backend_logf("PERF frame=%d frame_time_ms=%.3f update_ms=%.3f render_ms=%.3f present_ms=%.3f present_readback_ms=%.3f present_convert_ms=%.3f present_copy_ms=%.3f present_clear_ms=%.3f present_path=%s readback_format=%s readback_size=%dx%d copy_bytes=%llu dirty_tiles=%d dirty_ratio=%.4f dirty_hit_rate=%.4f full_copy_fallback=%s render_tasks=%d rect_tasks=%d batch_runs=%d rect_runs=%d rect_multi_runs=%d rect_multi_run_tasks=%d rect_max_run=%d rect_hstrip_runs=%d rect_hstrip_tasks=%d rect_vstrip_runs=%d rect_vstrip_tasks=%d rect_run_links=%d rect_color_breaks=%d rect_flip_breaks=%d rect_flipped_tasks=%d textured_geometry_tasks=%d textured_geometry_recovered=%d textured_geometry_fallback=%d set_texture_calls=%d binding_reuse=%d cache_hits=%d cache_misses=%d cache_creates=%d texture_unlocks=%d palette_unlocks=%d texture_evictions=%d palette_evictions=%d destroy_queue=%d source_ppg=%d source_mtrans=%d source_ui=%d source_solid=%d source_unknown=%d software_frame_mode_enabled=%d software_frame_surface_ready=%d software_frame_active=%d software_frame_owned=%d software_frame_direct_present=%d software_frame_uploaded=%d software_frame_fallback=%d software_frame_candidate_tasks=%d software_frame_candidate_pixels=%llu software_frame_fallback_tasks=%d software_frame_fallback_pixels=%llu software_frame_fast_exact_tasks=%d software_frame_fast_exact_pixels=%llu software_frame_fast_exact_clipped_tasks=%d software_frame_fast_exact_flipped_tasks=%d software_frame_fast_exact_color_mod_tasks=%d software_frame_fast_exact_color_mod_pixels=%llu software_frame_fast_scaled_tasks=%d software_frame_fast_scaled_pixels=%llu software_frame_fast_non_integer_tasks=%d software_frame_fast_non_integer_pixels=%llu software_frame_fast_non_integer_lookup_entries=%llu software_frame_generic_textured_tasks=%d software_frame_generic_textured_pixels=%llu software_frame_fast_miss_color_mod=%d software_frame_fast_miss_non_integer=%d software_frame_fast_miss_non_integer_lookup_entries=%llu software_frame_fast_miss_non_integer_ge_256_tasks=%d software_frame_fast_miss_non_integer_ge_256_pixels=%llu software_frame_fast_miss_non_integer_ge_256_lookup_entries=%llu software_frame_fast_miss_non_integer_ge_1024_tasks=%d software_frame_fast_miss_non_integer_ge_1024_pixels=%llu software_frame_fast_miss_non_integer_max_pixels=%llu software_frame_fast_miss_scaled=%d software_frame_fast_miss_unsupported_flip=%d software_frame_fast_miss_source_bounds=%d software_frame_reason_alpha=%d software_frame_reason_color_mod=%d software_frame_reason_geometry=%d software_frame_reason_solid=%d hybrid_candidate_tasks=%d hybrid_candidate_pixels=%llu hybrid_fallback_tasks=%d hybrid_fallback_pixels=%llu hybrid_reason_clip=%d hybrid_reason_alpha=%d hybrid_reason_color_mod=%d hybrid_reason_flip=%d hybrid_reason_geometry=%d hybrid_reason_solid=%d sort=%s",
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
                         sample->software_frame_mode_enabled,
                         sample->software_frame_surface_ready,
                         sample->software_frame_owned,
                         sample->software_frame_owned,
                         sample->software_frame_direct_present,
                         sample->software_frame_uploaded,
                         sample->software_frame_fallback,
                         sample->software_frame_candidate_tasks,
                         (unsigned long long)sample->software_frame_candidate_pixels,
                         sample->software_frame_fallback_tasks,
                         (unsigned long long)sample->software_frame_fallback_pixels,
                         sample->software_frame_fast_exact_tasks,
                         (unsigned long long)sample->software_frame_fast_exact_pixels,
                         sample->software_frame_fast_exact_clipped_tasks,
                         sample->software_frame_fast_exact_flipped_tasks,
                         sample->software_frame_fast_exact_color_mod_tasks,
                         (unsigned long long)sample->software_frame_fast_exact_color_mod_pixels,
                         sample->software_frame_fast_scaled_tasks,
                         (unsigned long long)sample->software_frame_fast_scaled_pixels,
                         sample->software_frame_fast_non_integer_tasks,
                         (unsigned long long)sample->software_frame_fast_non_integer_pixels,
                         (unsigned long long)sample->software_frame_fast_non_integer_lookup_entries,
                         sample->software_frame_generic_textured_tasks,
                         (unsigned long long)sample->software_frame_generic_textured_pixels,
                         sample->software_frame_fast_miss_color_mod,
                         sample->software_frame_fast_miss_non_integer,
                         (unsigned long long)sample->software_frame_fast_miss_non_integer_lookup_entries,
                         sample->software_frame_fast_miss_non_integer_ge_256_tasks,
                         (unsigned long long)sample->software_frame_fast_miss_non_integer_ge_256_pixels,
                         (unsigned long long)sample->software_frame_fast_miss_non_integer_ge_256_lookup_entries,
                         sample->software_frame_fast_miss_non_integer_ge_1024_tasks,
                         (unsigned long long)sample->software_frame_fast_miss_non_integer_ge_1024_pixels,
                         (unsigned long long)sample->software_frame_fast_miss_non_integer_max_pixels,
                         sample->software_frame_fast_miss_scaled,
                         sample->software_frame_fast_miss_unsupported_flip,
                         sample->software_frame_fast_miss_source_bounds,
                         sample->software_frame_reason_alpha,
                         sample->software_frame_reason_color_mod,
                         sample->software_frame_reason_geometry,
                         sample->software_frame_reason_solid,
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
            SDLGameRenderer_SetPerfCaptureLogicalIdentityEnabled(false);
            SDLApp_Exit();
        }
    }
#endif
}

void SDLApp_Exit() {
    SDL_Event quit_event;
    quit_event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit_event);
}
