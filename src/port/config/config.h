#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#include <stdbool.h>

#define CFG_KEY_FULLSCREEN "fullscreen"
#define CFG_KEY_WINDOW_WIDTH "window-width"
#define CFG_KEY_WINDOW_HEIGHT "window-height"
#define CFG_KEY_SCALEMODE "scale-mode"
#define CFG_KEY_SCANLINES "scanlines"
#define CFG_DRAW_PLAYERS_ABOVE_HUD "draw-players-above-hud"
#define CFG_ARCADE_BALANCE "arcade-balance"
#define CFG_KEY_SOFTWARE_FRAME_MODE "software-frame-mode"
#define CFG_KEY_SUPER_EFFECT_QUALITY "super-effect-quality"
#define CFG_KEY_SHOW_FPS "show-fps"
#define CFG_KEY_VIDEO_DRIVER_ORDER "video-driver-order"
#define CFG_KEY_RENDER_DRIVER_ORDER "render-driver-order"
#define CFG_KEY_GHOST_RESOLUTION "ghost-resolution"
#define CFG_KEY_GHOST_COUNT "ghost-count"
#define CFG_KEY_ARM_CLOCK "arm-clock"
#define CFG_KEY_GAME_MODE "game-mode"
#define CFG_KEY_HOLD_TO_PAUSE "hold-to-pause"
#define CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED "colorkey-loose-kernel-enabled"
#define CFG_KEY_RGB565_CANVAS_ENABLED "rgb565-canvas-enabled"
/* perf-3 piece-B: 8-pixel packed-store INDEX8->565 kernel kill switch. */
#define CFG_KEY_SOFTWARE_PALETTE_PACKED_8PX_ENABLED "software-palette-packed-8px-enabled"
/* perf-3 piece-C: 16-pixel NEON 565 kernels kill switch. */
#define CFG_KEY_SOFTWARE_NEON_16PX_ENABLED "software-neon-16px-enabled"

/* Direct-P2P (docs/plan-stun-direct-p2p.md Step 5). HOST_PORT == 0 means
 * OS-assigned; DISABLE_UPNP skips the UPnP first-try path; LAST_PEER_CODE
 * is populated at runtime on successful Join; HANDOFF_PATH is the file
 * the wrapper writes into before execve'ing the game (Step 9/11); STUN_
 * TIMEOUT_MS clamps the orchestrator's STUN discovery budget. */
#define CFG_KEY_NETPLAY_DIRECT_P2P_HOST_PORT "netplay-direct-p2p-host-port"
#define CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP "netplay-direct-p2p-disable-upnp"
#define CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE "netplay-direct-p2p-last-peer-code"
#define CFG_KEY_NETPLAY_DIRECT_P2P_HANDOFF_PATH "netplay-direct-p2p-handoff-path"
#define CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS "netplay-direct-p2p-stun-timeout-ms"

/* Rollback prediction window — max frames Gekko will predict ahead of
 * confirmed inputs and, on mispredict, the max rollback depth. Lower
 * values reduce worst-case resim CPU cost (linear in window) at the cost
 * of tolerating less input lag before dropping to stutter. 10 was the
 * upstream GekkoNet default; 6 is our default tuned for MiSTer stock
 * 800 MHz, which has near-zero headroom above 60 fps. */
#define CFG_KEY_NETPLAY_INPUT_PREDICTION_WINDOW "netplay-input-prediction-window"

/* Netplay diagnostics — session-tagged heartbeat extras (UUID, UTC, jitter,
 * cumulative kb_sent/recv) and the post-desync state dump. Recording cost is
 * paid in the production codepath regardless; this gate is an emergency mute
 * for serial-console noise. Default true so a friend running a build for a
 * shared session contributes useful logs without having to opt in. */
#define CFG_KEY_NETPLAY_DIAG_ENABLE "netplay-diag-enable"

/* Sparse effect-pool save (Option A). Default true — only off for A/B
 * parity-testing. When false, save_state writes a full sizeof(State)
 * blob (the legacy path), bypassing the sparse encoding entirely.
 * load_state auto-detects format by buffer length so flipping the flag
 * mid-session is safe (although not exposed via the in-game UI). */
#define CFG_KEY_NETPLAY_SPARSE_EFFECT_SAVE_ENABLED "netplay-sparse-effect-save-enabled"

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

/// Set (or update) the string value associated with the given key in the
/// in-memory entries table.
/// NOTE: the update lives only in memory until Config_Save() is called;
/// Config_Save() is a minimal stub today (see config.c) and does not yet
/// rewrite config.ini on disk. Direct-P2P runtime keys (HOST_PORT,
/// LAST_PEER_CODE) use this path for within-session updates.
void Config_SetString(const char* key, const char* value);

/// Persist the current in-memory config entries to the on-disk config file.
/// See note on Config_SetString — current implementation is a no-op that
/// logs once so callers have a stable symbol to call; real write-back is
/// deferred until a caller actually needs it.
void Config_Save(void);

#endif
