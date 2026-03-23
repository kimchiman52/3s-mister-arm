# Config

3SX supports a config file which allows you to change several useful options.

Config location:
- **Windows**: `C:\Users\<username>\AppData\Roaming\CrowdedStreet\3SX\config`
- **Linux**: `~/.local/share/CrowdedStreet/3SX/config`
- **macOS**: `~/Library/Application Support/CrowdedStreet/3SX/config`

## Options

### `fullscreen`

Whether the game should start in fullscreen mode.

### `window-width` / `window-height`

Window dimensions to use when `fullscreen` is set to `false`.

### `scale-mode`

The way the internal 384x224 buffer is scaled.

Possible values:
- `native`: No scaling. Keeps the internal `384x224` image size and centers it. On MiSTer's 384-native analog TV path, this preserves the game's native width in the framebuffer.
- `nearest`
- `linear`
- `soft-linear`: Produces an image with a balance of sharpness and sizing consistency
- `integer`: Produces a pixel-perfect image, but requires a 4K display (⚠️ WARNING: the image is gonna be cropped if your display resolution is smaller than 2688x2016)
- `square-pixels`: Integer (whole-number) scaling with square pixels

### `software-frame-mode`

Controls whether gameplay uses the 3SX-owned software frame path or the legacy SDL-owned gameplay frame path.

Defaults:
- MiSTer builds: `on`
- Non-MiSTer builds: `off`

Possible values:
- `off`: Keep the existing SDL-owned gameplay frame path
- `on`: Keep the `384x224` gameplay frame in 3SX-owned software memory. On MiSTer, eligible frames present directly through fbdev to avoid SDL readback; when composition or screenshots still need SDL, the frame uploads back to `cps3_canvas`.

### `super-effect-quality`

Controls MiSTer-only visual degradation during the trusted Yun SA3 burst window.

Defaults:
- `full`

Possible values:
- `full`: Keep current behavior
- `simplified`: Snap the hottest Yun SA3 burst sprites to integer destination geometry so more work can use the cheaper exact/scaled software-frame paths
- `minimal`: Apply `simplified` and thin every other hot burst sprite after final sort so only the first visible instance per family is always preserved
- `frame-skip`: Keep gameplay/update cadence, but on alternating trusted Yun SA3 burst frames reuse the previous rendered frame instead of drawing a fresh one when a safe previous frame is available

Notes:
- This setting is only active on MiSTer builds.
- The current Ralph first-pass automated matrix intentionally sweeps only `full`, `simplified`, and `minimal`. Treat `frame-skip` as a separate higher-risk follow-up because it reuses a previous rendered frame during trusted Yun SA3 burst frames.
- Current v1 scope is intentionally narrow: player 1 Yun SA3 onset only.
- On non-MiSTer builds the config key is parsed but behaves like `full`.

### `show-fps`

Whether to draw a small FPS readout while the game is running.

Defaults:
- `false`

Notes:
- On MiSTer fbdev output, the overlay is drawn at the bottom-center of the active picture area so it stays away from overscan-prone corners.
- The overlay is opt-in and uses a lightweight cached label update path instead of perf capture telemetry.

### `video-driver-order`

Comma-separated SDL video backend preference list passed via `SDL_HINT_VIDEO_DRIVER` before SDL init.

Example:
- `kmsdrm,offscreen,dummy`

### `render-driver-order`

Comma-separated SDL renderer backend preference list passed via `SDL_HINT_RENDER_DRIVER` before renderer creation.

Example:
- `software`
