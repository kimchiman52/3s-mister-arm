# Plan: Split FPS overlay into FPS mode and Debug mode

## Summary

Replace the binary on/off FPS overlay with a tri-state:
- **Off** -- no overlay (default)
- **FPS** -- simple FPS counter in the top-left corner, just the number (e.g. "60")
- **Debug** -- current detailed bottom-center overlay with all sub-timers

The config key `show-fps` changes from boolean (`true`/`false`) to string
(`off`/`fps`/`debug`).  The OSD menu cycles Off -> FPS -> Debug -> Off.

Any unrecognized value (including legacy `true`/`on`/`1`) is treated as `off`.

---

## Current architecture

### Data flow

1. **Wrapper** (`threesx_wrapper.cpp`): reads/writes `show-fps` as boolean in
   the config file at `/media/fat/games/3sx/config`.  Global `g_wrapper_fps_enabled`
   (int, 0 or 1).  OSD menu shows "On"/"Off".  On toggle, writes config then
   sends `SIGUSR1` to child.

2. **main.c**: `on_shutdown_signal` sets `fps_toggle_requested = 1` on SIGUSR1.
   `handle_signal_requests()` calls `SDLApp_ToggleFPSOverlay()`.

3. **sdl_app.c**: `show_fps_overlay` (bool) gates everything.
   - `init_show_fps_overlay()` reads via `Config_GetBool(CFG_KEY_SHOW_FPS)`.
   - `SDLApp_ToggleFPSOverlay()` re-reads the config file from disk, parsing
     `true`/`on`/`1` as enabled.
   - `publish_fps_overlay_label()` formats the detailed debug string (e.g.
     `"60 U:2.3(T0.1 G1.0 S0.5 D0.7[t0.1 s0.1]) R:1.2(r0.8) =3.5"`) or
     falls back to `"60 FPS"` when no timing data is available yet.
   - `update_fps_overlay()` computes 250 ms rolling averages.
   - `fps_overlay_accumulate_timing()` feeds per-frame sub-timers (gated by
     `show_fps_overlay`).
   - Three rendering paths all check `show_fps_overlay`:
     a. fbdev presenter path (native framebuffer)
     b. native video writer path (DDR3 bypass)
     c. SDL renderer path (non-MiSTer / fallback)

4. **fbdev_presenter.c**: `fps_overlay_enabled` (bool) + `fps_overlay_text[128]`.
   - `FBDevPresenter_SetFPSOverlayEnabled(bool)` controls the enabled flag.
   - `FBDevPresenter_SetFPSOverlayText(const char*)` sets the label.
   - `compute_fps_overlay_layout()` places text at **bottom-center** of content rect.
   - `FBDevPresenter_ApplyFPSOverlayToBuffer()` does the same for the native
     video writer path (384x224 buffer), also bottom-center.
   - Rasterization uses a 3x5 pixel font with scale 2/3/4 depending on content height.

5. **config.c**: `show-fps` is registered as `CFG_BOOL` with default `false`.
   `Config_GetBool` / `Config_GetString` dispatch based on type; if the stored
   type doesn't match the default's type, the default wins.

### Key variables

| Location | Variable | Type | Purpose |
|---|---|---|---|
| `sdl_app.c:86` | `show_fps_overlay` | `bool` | Master gate for all overlay logic |
| `sdl_app.c:89` | `fps_overlay_value` | `int` | Current measured FPS number |
| `sdl_app.c:90` | `fps_overlay_label[128]` | `char[]` | Formatted text sent to presenter |
| `sdl_app.c:94-117` | `fps_overlay_*_accum/avg` | `Uint64`/`double` | Sub-timer accumulators |
| `fbdev_presenter.c:75` | `fps_overlay_enabled` | `bool` | Presenter-side enabled flag |
| `fbdev_presenter.c:76` | `fps_overlay_text[128]` | `char[]` | Presenter-side label copy |
| `threesx_wrapper.cpp:125` | `g_wrapper_fps_enabled` | `int` | Wrapper-side state (0/1) |
| `config.c:63` | default entry | `CFG_BOOL` | Default type registration |

---

## Implementation steps

### Step 1: Change the config default from CFG_BOOL to CFG_STRING

**Why it matters**: The config system uses type matching.  Currently `show-fps`
is `CFG_BOOL` (default `false`).  If the file contains a string like `"fps"`,
`dict_iterator` will parse it as `CFG_STRING`.  Then `Config_GetBool` will find
that the read type != default type and fall back to the default (`false`), so
the new string values would be silently ignored.  Changing the default to
`CFG_STRING` with value `"off"` makes `Config_GetString(CFG_KEY_SHOW_FPS)`
return the correct value.

**Files to read first**: `src/port/config/config.c` (full file, ~239 lines),
`src/port/sdl/sdl_app.c` lines 8571-8580 (`startup_generated_defaults` array)

**Files to modify**:

1. **`src/port/config/config.c`** line 63:
   - Change `{ .key = CFG_KEY_SHOW_FPS, .type = CFG_BOOL, .value.b = false }`
     to `{ .key = CFG_KEY_SHOW_FPS, .type = CFG_STRING, .value.s = "off" }`

2. **`src/port/sdl/sdl_app.c`** line 8577 — `startup_generated_defaults` array:
   - Change `{ CFG_KEY_SHOW_FPS, "false" }` to `{ CFG_KEY_SHOW_FPS, "off" }`.
   This array lists the expected default values for a "is this config file
   still at defaults?" check.  It must match the new config default.

**Success criteria**: `Config_GetString(CFG_KEY_SHOW_FPS)` returns `"off"` with
no config file entry, `"fps"` or `"debug"` when the file says so, and `"false"`
(the literal string) when the file has a legacy `show-fps = false` since the
parser interprets that as a bool, type-mismatches against the STRING default,
and returns the default `"off"`.

Wait -- actually re-reading the config parser more carefully: `dict_iterator`
(line 150; the bool/int/string parse logic is at lines 159-171) checks
`"true"` and `"false"` first, and stores them as `CFG_BOOL`.
So if the config file says `show-fps = false`, it will be stored as
`CFG_BOOL/false`.  Then `Config_GetString` will see the read entry has type
`CFG_BOOL` but the default has type `CFG_STRING`, so it falls back to the
default `"off"`.  If the file says `show-fps = true`, same thing -- falls back
to default `"off"`.  This matches the requirement that legacy `true` is treated
as `off`.

For `show-fps = fps`, the parser stores it as `CFG_STRING/"fps"` which matches
the default type, so `Config_GetString` returns `"fps"`.  Correct.

For `show-fps = 1`, the parser stores it as `CFG_INT/1`, type-mismatches the
default `CFG_STRING`, falls back to `"off"`.  Legacy `1` treated as `off`.
Correct.

**Dependencies**: None

**What NOT to do**: Don't add a new `Config_GetStringWithDefault` API.  The
existing type-mismatch-falls-back-to-default behavior handles everything.

**What to do if it fails**: If some code path still calls `Config_GetBool` for
this key, it will get `false` (the `entry->type != CFG_BOOL` check fails,
returns `false`).  This is safe -- it means the overlay stays off until the
callers are updated in Step 2.

---

### Step 2: Replace `show_fps_overlay` bool with a tri-state enum in sdl_app.c

**Why it matters**: The single `bool` cannot express three states.  Every
check of `show_fps_overlay` must become aware of the mode to decide whether
to accumulate timers, format a simple label vs debug label, and choose the
rendering position.

**Files to read first**:
- `src/port/sdl/sdl_app.c` lines 86-117 (state variables), lines 9026-9241
  (all FPS overlay functions), lines 9505-9508 (init call), lines 9815-9846
  (renderer overlay), lines 10078-10192 (native video path + accumulate)

**Files to modify**:

1. **`src/port/sdl/sdl_app.c`**:

   a. Add an enum near line 86 (before the state variables):
   ```c
   typedef enum FpsOverlayMode {
       FPS_OVERLAY_OFF = 0,
       FPS_OVERLAY_FPS = 1,
       FPS_OVERLAY_DEBUG = 2,
   } FpsOverlayMode;
   ```

   b. Replace `static bool show_fps_overlay = false;` (line 86) with:
   ```c
   static FpsOverlayMode fps_overlay_mode = FPS_OVERLAY_OFF;
   ```

   c. Add a helper to parse the config string:
   ```c
   static FpsOverlayMode parse_fps_overlay_mode(const char* value) {
       if (value == NULL) return FPS_OVERLAY_OFF;
       if (SDL_strcasecmp(value, "fps") == 0) return FPS_OVERLAY_FPS;
       if (SDL_strcasecmp(value, "debug") == 0) return FPS_OVERLAY_DEBUG;
       return FPS_OVERLAY_OFF;
   }
   ```

   d. **`init_show_fps_overlay()`** (line 9026-9056): change from
   `show_fps_overlay = Config_GetBool(CFG_KEY_SHOW_FPS)` to
   `fps_overlay_mode = parse_fps_overlay_mode(Config_GetString(CFG_KEY_SHOW_FPS))`.
   Rest of function (zeroing accumulators) stays the same.

   e. **`publish_fps_overlay_label()`** (line 9058-9087): change the guard
   from `if (!show_fps_overlay)` to `if (fps_overlay_mode == FPS_OVERLAY_OFF)`.
   Split the format logic:
   - If `fps_overlay_mode == FPS_OVERLAY_FPS`: format as just the number, e.g.
     `SDL_snprintf(fps_overlay_label, ..., "%d", fps_overlay_value)`.
   - If `fps_overlay_mode == FPS_OVERLAY_DEBUG`: keep the existing detailed
     format string with sub-timers (the current long format), or `"%d FPS"` if
     no timing data yet.

   f. **`update_fps_overlay()`** (line 9108-9162): change guard from
   `if (!show_fps_overlay)` to `if (fps_overlay_mode == FPS_OVERLAY_OFF)`.

   g. **`SDLApp_ToggleFPSOverlay()`** (line 9199-9241): change the config
   re-read logic.  Instead of parsing `true`/`on`/`1` into a bool, read the
   raw value and call `parse_fps_overlay_mode()`:
   ```c
   // After reading the value string:
   fps_overlay_mode = parse_fps_overlay_mode(val);
   ```

   h. Update the presenter call in the toggle function (line 9237) to use
   the new API and pass the enum value directly:
   ```c
   FBDevPresenter_SetFPSOverlayMode(fps_overlay_mode);
   ```
   **Important**: pass `fps_overlay_mode` (the int), NOT a boolean expression
   like `fps_overlay_mode != FPS_OVERLAY_OFF`.  The presenter needs the actual
   mode (0/1/2) to choose the correct layout position.

   i. Update the log message (line 9240) to show the mode name:
   ```c
   static const char* mode_names[] = { "off", "fps", "debug" };
   backend_logf("FPS overlay: %s", mode_names[fps_overlay_mode]);
   ```

   j. Update the init path at line 9507 to use the new API and pass the
   enum value directly (same rationale as step 2h):
   ```c
   FBDevPresenter_SetFPSOverlayMode(fps_overlay_mode);
   ```
   Note: `init_show_fps_overlay()` is called at line 9590 (before fbdev
   init at line 9505), so `fps_overlay_mode` is already populated by the
   time this call executes.  The ordering is correct.

   k. **All remaining `show_fps_overlay` references**:
   - Line 9817 (`render_renderer_fps_overlay`): change to
     `fps_overlay_mode == FPS_OVERLAY_OFF`
   - Line 10085 (native video writer overlay apply): change to
     `fps_overlay_mode != FPS_OVERLAY_OFF`
   - Line 10178 (accumulate timing, inside `#if ENABLE_PERF_TELEMETRY`):
     change to `fps_overlay_mode != FPS_OVERLAY_OFF`

   Note: the sub-timer accumulators are always accumulated regardless of mode
   when the overlay is active.  In FPS mode we don't *display* them, but
   accumulating them is cheap and means switching from FPS to Debug
   mid-session doesn't start from zero.  Alternatively, gate accumulation
   on `fps_overlay_mode == FPS_OVERLAY_DEBUG` only.  The latter is slightly
   cleaner since FPS mode doesn't need them.  **Decision**: accumulate only
   in debug mode.  The guard at line 10178 becomes
   `if (fps_overlay_mode == FPS_OVERLAY_DEBUG)`.

   Note: the entire accumulate block (lines 10155-10193) is inside
   `#if ENABLE_PERF_TELEMETRY`.  This is an existing compile-time gate
   and does not need to change, but the implementer should be aware that
   these lines are conditionally compiled.

**Success criteria**: Compile succeeds.  With `show-fps = off` in config, no
overlay.  With `show-fps = fps`, the overlay label is just the number.
With `show-fps = debug`, the overlay label is the full debug string.

**Dependencies**: Step 1 (config type change)

**What NOT to do**:
- Don't rename the signal (`SIGUSR1`) or the signal handler function name.
  The signal still means "re-read overlay config" -- the semantics are the same.
- Don't change `fps_overlay_accumulate_timing()`'s signature or parameters.
- Don't touch the 250 ms window logic in `update_fps_overlay()`.

**What to do if it fails**: If compile errors appear from missed `show_fps_overlay`
references, grep for `show_fps_overlay` and update each site.  The variable is
only used in `sdl_app.c`.

---

### Step 3: Add top-left layout mode to fbdev_presenter.c

**Why it matters**: FPS mode needs the overlay in the top-left corner (small,
unobtrusive).  Debug mode keeps the current bottom-center position.  The
presenter currently hardcodes bottom-center in `compute_fps_overlay_layout()`.

**Files to read first**:
- `src/port/sdl/fbdev_presenter.c` lines 972-1032 (`compute_fps_overlay_layout`),
  lines 2498-2542 (`FBDevPresenter_ApplyFPSOverlayToBuffer`)
- `src/port/sdl/fbdev_presenter.h` lines 95-103

**Files to modify**:

1. **`src/port/sdl/fbdev_presenter.h`**:

   a. Add a mode enum or change the `SetFPSOverlayEnabled` API to convey
   position.  The simplest approach: replace `SetFPSOverlayEnabled(bool)` with
   a new function that takes an int mode:
   ```c
   /// Set the FPS overlay display mode.
   /// 0 = off, 1 = top-left (FPS), 2 = bottom-center (debug).
   void FBDevPresenter_SetFPSOverlayMode(int mode);
   ```
   Keep the old `SetFPSOverlayEnabled` as a thin wrapper or remove it.
   **Decision**: replace it.  There are only 3 call sites (sdl_app.c toggle,
   sdl_app.c init, and the `#else` stub).

2. **`src/port/sdl/fbdev_presenter.c`**:

   a. Replace `static bool fps_overlay_enabled = false;` (line 75) with:
   ```c
   static int fps_overlay_mode = 0;  /* 0=off, 1=fps (top-left), 2=debug (bottom-center) */
   ```

   b. All checks of `fps_overlay_enabled` change to `fps_overlay_mode != 0`:
   - Line 973 (`compute_fps_overlay_layout`)
   - Line 2499 (`ApplyFPSOverlayToBuffer`)

   c. **`compute_fps_overlay_layout()`** (line 972-1032): add a branch based
   on `fps_overlay_mode`:
   - **Mode 1 (FPS, top-left)**: position the text at `x0 + safe_margin`,
     `y0 + safe_margin` instead of the current centered-bottom calculation.
   - **Mode 2 (Debug, bottom-center)**: keep the existing logic unchanged.

   Specifically for mode 1:
   ```c
   if (fps_overlay_mode == 1) {
       /* Top-left corner */
       const int draw_x = x0 + safe_margin;
       const int draw_y = y0 + safe_margin;
       // ... rest identical (bg_pad, clamping, etc.)
   } else {
       /* Bottom-center (existing logic) */
       const int draw_x = x0 + ((content_w - text_w) / 2);
       const int draw_y = y1 - glyph_h - safe_margin;
   }
   ```

   d. **`FBDevPresenter_SetFPSOverlayMode(int mode)`**: replace
   `FBDevPresenter_SetFPSOverlayEnabled`:
   ```c
   void FBDevPresenter_SetFPSOverlayMode(int mode) {
       fps_overlay_mode = mode;
       if (mode == 0) {
           fps_overlay_text[0] = '\0';
       }
   }
   ```

   e. **`FBDevPresenter_ApplyFPSOverlayToBuffer()`** (line 2498-2542): the
   layout computation here is standalone (for 384x224 native video buffer).
   Add the same top-left / bottom-center branch based on `fps_overlay_mode`:
   - Mode 1: `layout.draw_x = bg_pad; layout.draw_y = bg_pad;`
   - Mode 2: existing centered-bottom logic.

   f. Update the `#else` stub (line 2610) to match the new signature:
   ```c
   void FBDevPresenter_SetFPSOverlayMode(int mode) {
       (void)mode;
   }
   ```

**Success criteria**: With mode 1, the overlay renders at top-left.  With mode 2,
it renders at bottom-center (unchanged from today).  With mode 0, no overlay.
The native video writer path also respects the position.

**Dependencies**: Step 2 (so the callers pass mode instead of bool)

**What NOT to do**:
- Don't change the glyph font data or the rasterization logic.
- Don't change the 3x5 glyph dimensions or the scale computation.
- Don't change the overlay caching mechanism.
- Don't add a separate buffer for each mode; reuse the same overlay buffer.

**What to do if it fails**: If the top-left position clips off-screen on some
resolutions, add bounds checking.  The existing `clamp_to_range` calls should
handle this, but verify with the 384x224 native video buffer (smallest case).

---

### Step 4: Update SDL renderer FPS overlay path (non-fbdev fallback)

**Why it matters**: `render_renderer_fps_overlay()` in sdl_app.c (line 9816)
renders the overlay via `SDL_RenderDebugText` when fbdev is not active.  This
path also needs to respect the mode and position.

**Files to read first**:
- `src/port/sdl/sdl_app.c` lines 9815-9846 (`render_renderer_fps_overlay`)

**Files to modify**:

1. **`src/port/sdl/sdl_app.c`** `render_renderer_fps_overlay()`:

   a. Change the guard from `!show_fps_overlay` to
   `fps_overlay_mode == FPS_OVERLAY_OFF`.

   b. Change the positioning:
   - **FPS mode** (`fps_overlay_mode == FPS_OVERLAY_FPS`): draw at top-left
     of the content rect with a small margin.
     ```c
     const float draw_x = draw_rect.x + (float)margin;
     const float draw_y = draw_rect.y + (float)margin;
     ```
   - **Debug mode** (`fps_overlay_mode == FPS_OVERLAY_DEBUG`): keep the existing
     bottom-center positioning.

**Success criteria**: On non-MiSTer platforms (or when fbdev is disabled), the
FPS mode renders at top-left and debug mode renders at bottom-center.

**Dependencies**: Step 2

**What NOT to do**: Don't change the SDL_RenderDebugText calls or the scale
logic.  Only change the position calculation.

**What to do if it fails**: This path is only used when `fbdev_presenter_enabled`
is false, which doesn't happen on MiSTer.  Low risk.

---

### Step 5: Update the wrapper's tri-state cycling

**Why it matters**: The wrapper manages the OSD menu and persists the config.
It needs to cycle Off -> FPS -> Debug -> Off instead of toggling On/Off.

**Files to read first**:
- `vendor/Main_MiSTer/threesx_wrapper.cpp` lines 65-77 (enums), lines 125
  (global), lines 1169-1240 (read/write), lines 1255-1290 (menu draw),
  lines 1364-1376 (toggle action), lines 1437-1441 (reset defaults),
  line 1718 (startup init)

**Files to modify**:

1. **`vendor/Main_MiSTer/threesx_wrapper.cpp`**:

   a. Add FPS mode enum constants (near the other enums, around line 64):
   ```cpp
   enum FpsOverlayMode
   {
       kFpsOverlayOff = 0,
       kFpsOverlayFps = 1,
       kFpsOverlayDebug = 2,
       kFpsOverlayModeCount = 3,
   };
   ```

   b. Change `g_wrapper_fps_enabled` (line 125) from `int` (0/1) to represent
   the mode.  Rename to `g_wrapper_fps_mode` for clarity:
   ```cpp
   int g_wrapper_fps_mode = kFpsOverlayOff;
   ```

   c. **`read_runtime_fps_default()`** (line 1169-1174): change return type
   to `int` and parse string values:
   ```cpp
   int read_runtime_fps_default()
   {
       char value[64] = {};
       if (!read_runtime_config_value("show-fps", value, sizeof(value)))
           return kFpsOverlayOff;
       if (!strcasecmp(value, "fps")) return kFpsOverlayFps;
       if (!strcasecmp(value, "debug")) return kFpsOverlayDebug;
       return kFpsOverlayOff;
   }
   ```

   d. **`write_runtime_fps_default()`** (line 1176-1240): change parameter
   from `bool enabled` to `int mode`.  Map mode to string:
   ```cpp
   bool write_runtime_fps_default(int mode)
   {
       const char *mode_str = "off";
       if (mode == kFpsOverlayFps) mode_str = "fps";
       else if (mode == kFpsOverlayDebug) mode_str = "debug";

       // ... same file-rewrite logic, but replace the format string:
       fprintf(out, "show-fps = %s\n", mode_str);
       // ... (both the in-place replacement and the append-if-missing)
   }
   ```

   e. Add a label function:
   ```cpp
   const char *runtime_fps_mode_label(int mode)
   {
       switch (mode)
       {
       case kFpsOverlayFps: return "FPS";
       case kFpsOverlayDebug: return "Debug";
       default: return "Off";
       }
   }
   ```

   f. **`draw_wrapper_menu()`** (line 1255-1290): change the fps_line format:
   ```cpp
   format_wrapper_value_line(fps_line, sizeof(fps_line), "FPS",
                             runtime_fps_mode_label(g_wrapper_fps_mode));
   ```

   g. **Menu action handler** (line 1366-1376): change from toggle to cycle:
   ```cpp
   if (g_wrapper_menu_selected == kMenuFpsToggle)
   {
       const int next_mode = (g_wrapper_fps_mode + 1) % kFpsOverlayModeCount;
       if (write_runtime_fps_default(next_mode))
       {
           g_wrapper_fps_mode = next_mode;
           (void)kill(child, kRuntimeFpsToggleSignal);
           draw_wrapper_menu(g_wrapper_menu_selected);
       }
       return;
   }
   ```

   h. **Reset to defaults** (line 1437-1441): change to:
   ```cpp
   write_runtime_fps_default(kFpsOverlayOff);
   if (g_wrapper_fps_mode != kFpsOverlayOff)
       (void)kill(child, kRuntimeFpsToggleSignal);
   g_wrapper_fps_mode = kFpsOverlayOff;
   ```
   Note: sending one signal is fine because the child re-reads the config file
   from disk on every signal.  It doesn't toggle; it reads the current value.

   i. **Startup init** (line 1718): change to:
   ```cpp
   g_wrapper_fps_mode = read_runtime_fps_default();
   ```

**Success criteria**: The OSD menu shows "Off" / "FPS" / "Debug" for the FPS
row.  Pressing enter cycles through all three.  The config file gets the correct
string value.  Reset to defaults sets it to "off".

**Dependencies**: Steps 1-2 (the child must be able to parse the new string
values, or the signal will be a no-op)

**What NOT to do**:
- Don't add a new signal.  The existing `SIGUSR1` / `kRuntimeFpsToggleSignal`
  is fine because the child re-reads config from disk each time.
- Don't rename the menu enum `kMenuFpsToggle` -- it still toggles (cycles).

**What to do if it fails**: If the wrapper can't write the config file, the
`write_runtime_fps_default` call returns false and the state doesn't change
(existing error handling already covers this).

---

### Step 6: Verify the native video writer path

**Why it matters**: The native video writer path at sdl_app.c line 10082 calls
`FBDevPresenter_ApplyFPSOverlayToBuffer()` to composite onto the 384x224 frame
before converting to RGB565.  This path must correctly show the FPS number at
top-left (FPS mode) or the debug string at bottom-center (debug mode).

**Files to read first**:
- `src/port/sdl/sdl_app.c` lines 10078-10092 (native video overlay application)
- `src/port/sdl/fbdev_presenter.c` lines 2498-2542 (`ApplyFPSOverlayToBuffer`)

**Files to modify**: None -- this should work automatically if Steps 2 and 3
are correct, because:
- The `show_fps_overlay` guard at line 10085 is changed to
  `fps_overlay_mode != FPS_OVERLAY_OFF` in Step 2.
- The `FBDevPresenter_ApplyFPSOverlayToBuffer` function uses
  `fps_overlay_mode` to pick the position in Step 3.

**Success criteria**: On native video output (DDR3 bypass path), FPS mode shows
the number at top-left of the 384x224 frame, debug mode shows the full breakdown
at bottom-center.

**Dependencies**: Steps 2, 3

**What NOT to do**: Don't modify `native_video_writer.c` -- it doesn't handle
FPS overlay directly; it's done in sdl_app.c.

**What to do if it fails**: If the text clips on the 384x224 buffer in FPS mode
(unlikely for a 2-digit number), the scale=1 path in
`ApplyFPSOverlayToBuffer` should handle it.  The debug string may be too wide
for 384 pixels at small scale; if so, consider truncating, but this is the
existing behavior and not a regression.

---

### Step 7: Build, deploy, and test

**Why it matters**: All paths must be verified on real hardware.

**Files to read first**: None

**Actions**:

1. Build the emulator binary (ARM cross-compile).
2. Deploy to MiSTer via rsync (no `--delete`).
3. Test each mode:
   - Set `show-fps = off` in config, verify no overlay.
   - Set `show-fps = fps` in config, verify top-left FPS number only.
   - Set `show-fps = debug` in config, verify bottom-center debug breakdown.
4. Test OSD menu cycling: Off -> FPS -> Debug -> Off.
5. Test legacy values: set `show-fps = true`, verify treated as off.
   Set `show-fps = 1`, verify treated as off.
6. Test "Reset to Default" in OSD menu sets overlay to off.
7. Verify both video paths:
   - fbdev presenter (native framebuffer)
   - native video writer (DDR3 bypass) -- check THREESX_NATIVE_VIDEO=0 to
     force fbdev, then re-enable to test native path.

**Success criteria**: All 7 sub-tests pass.

**Dependencies**: Steps 1-6

**What NOT to do**: Don't use `rsync --delete`.  Don't use headless perf
captures for visual verification.

**What to do if it fails**: Check the log output for the "FPS overlay: ..."
message to verify the mode was parsed correctly.  If the overlay appears in
the wrong position, check which presenter path is active (the log says
"FBDEV presenter: enabled/disabled" and "Native video writer: enabled/disabled").

---

## Files changed (summary)

| File | Change |
|---|---|
| `src/port/config/config.c` | Default type `CFG_BOOL` -> `CFG_STRING`, value `"off"` |
| `src/port/sdl/sdl_app.c` | `bool show_fps_overlay` -> `FpsOverlayMode fps_overlay_mode` enum; update all guards, label formatting, config parsing, 3 rendering paths, and `startup_generated_defaults` entry (`"false"` -> `"off"`) |
| `src/port/sdl/fbdev_presenter.h` | `SetFPSOverlayEnabled(bool)` -> `SetFPSOverlayMode(int)` |
| `src/port/sdl/fbdev_presenter.c` | `bool fps_overlay_enabled` -> `int fps_overlay_mode`; top-left vs bottom-center layout branch in `compute_fps_overlay_layout` and `ApplyFPSOverlayToBuffer`; update `#else` stub |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | `g_wrapper_fps_enabled` -> `g_wrapper_fps_mode` tri-state; read/write string values; cycle Off->FPS->Debug->Off; menu label |

## Risks

1. **Config migration**: Users with `show-fps = true` in their config will see
   the overlay turn off.  This is by design (the requirement says legacy values
   are treated as off).  On next menu cycle, it will write a valid string value.

2. **Build-time type mismatch**: The config system's `dict_iterator` auto-detects
   types.  The value `"off"` is not `"true"`/`"false"` and not an integer, so it
   will be stored as `CFG_STRING`.  The default is also `CFG_STRING`.  No
   mismatch.  The value `"false"` (legacy) is detected as `CFG_BOOL`, mismatches
   the `CFG_STRING` default, falls back to `"off"`.  Correct.

3. **Signal semantics**: The signal is not a "toggle" in the new design -- it
   means "re-read config".  This is already how it works today (the handler
   re-reads the file), so no semantic change.
