#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#include <stdbool.h>

#define CFG_KEY_FULLSCREEN "fullscreen"
#define CFG_KEY_WINDOW_WIDTH "window-width"
#define CFG_KEY_WINDOW_HEIGHT "window-height"
#define CFG_KEY_SCALEMODE "scale-mode"
#define CFG_KEY_SOFTWARE_FRAME_MODE "software-frame-mode"
#define CFG_KEY_SUPER_EFFECT_QUALITY "super-effect-quality"
#define CFG_KEY_SHOW_FPS "show-fps"
#define CFG_KEY_VIDEO_DRIVER_ORDER "video-driver-order"
#define CFG_KEY_RENDER_DRIVER_ORDER "render-driver-order"
#define CFG_KEY_GHOST_RESOLUTION "ghost-resolution"
#define CFG_KEY_GHOST_COUNT "ghost-count"

/// Initialize config system
void Config_Init();

/// Destroy resources used by config system
void Config_Destroy();

/// Get the value associated with the given key as a `bool`
/// @return The value associated with `key` if `key` is among entries and the value's type is `bool`, `false` otherwise
bool Config_GetBool(const char* key);

/// Get the value associated with the given key as an `int`
/// @return The value associated with `key` if `key` is among entries and the value's type is `int`, `0` otherwise
int Config_GetInt(const char* key);

/// Get the value associated with the given key as a `string`
/// @return The value associated with `key` if `key` is among entries and the value's type is `string`, `NULL` otherwise
const char* Config_GetString(const char* key);

/// Check whether the config file explicitly provided the given key
bool Config_HasExplicitKey(const char* key);

#endif
