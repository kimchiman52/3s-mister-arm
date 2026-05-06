# Giblet Wholesale Rip — Master Plan

Status: planning. Multi-week effort. Last refreshed 2026-05-03.

## Goal

End state: our hand-rolled SDL app/present layer
(`src/port/sdl/sdl_app.c`, `src/port/sdl/sdl_game_renderer.c` (no
public header), `src/port/sdl/fbdev_presenter.{c,h}`) is gone.
Giblet's `src/platform/app/sdl/*` and `src/platform/app/arm/*`
become the only app/display layer in the tree. The giblet software
renderer that already lives at `src/platform/video/software/`
becomes unconditional — the `THIRDSARM_USE_GIBLET_RENDERER` cmake
option and every guard derived from it
(`CRS_VIDEO_DRIVER_SOFTWARE`, `CRS_SW_CANVAS_16BPP`,
`CRS_SW_INDEXED8_KERNEL_DEFAULT`) collapse to "always on" for the
software path. MiSTer's FPGA DDR3 present
(`src/port/sdl/native_video_writer.{c,h}`) survives intact, but is
re-homed as a new `arm_display_native_video_writer.c` ArmDisplay
backend that the existing `ArmDisplay_Init` selector picks first
under `PORT_MISTER`. Net code reduction is approximately
27,000 LOC (sdl_app.c 11,734 + sdl_game_renderer.c 13,722 +
fbdev_presenter.c 2,883 + fbdev_presenter.h 116 + the perf-capture
getters that disappear with sdl_game_renderer.c, less the ~1,500
LOC of giblet app/display + ~300 LOC of new
NativeVideoWriter backend + ~400 LOC of new shared text
overlay = net ~25.7K LOC delete).

## Branch strategy

Branch off `mister`, **not** `giblet-stuff`.

Reasoning:

- `giblet-stuff` is currently 7 commits ahead of `mister`
  (`83e99cc5` import, then `e31a10e6 .. 686a5e2b` activation /
  fixes — see `git log --oneline mister..giblet-stuff`). All seven
  are shippable as-is and may go out under the existing
  `THIRDSARM_USE_GIBLET_RENDERER=ON` flag in the meantime.
- The rip is going to delete tens of thousands of lines and reshape
  the build. Tangling that with a small set of clean,
  cherry-pickable activation commits would force every later
  shippable build off `giblet-stuff` to be rebased through the rip.
- The rip wants the giblet imports anyway (the
  `src/platform/video/software/*` tree, the renderer-dispatch shim
  in `src/rendering/renderer.c`). Cherry-pick those into the rip
  branch as the first action — that gives us the giblet renderer
  plus a clean delta vs `mister` baseline, with no other
  giblet-stuff-only experimental work in the diff.

Recommended branch name: `giblet-rip` (or
`giblet-wholesale-rip` if a longer name is preferred). Keep it
visibly distinct from `giblet-stuff` so the orchestrator and the
user don't confuse the in-flight rip with the activation work.

`mister` is the project's true baseline branch, NOT `main` —
`gitStatus` "Main branch: main" is wrong here, see
`~/.claude/projects/-Users-sb-Developer-3sx-mister/memory/project-baseline-branch.md`.

## Architecture target

End-state directory layout (source-of-truth files only — generated /
build output excluded):

```
src/
  core/                        # unchanged
  rendering/
    renderer.c                 # simplified — one branch per dispatch
                               # (Software always-on; PSP/3DS legacy gates removed
                               #  if confirmed dead, otherwise kept untouched)
  platform/
    app/
      sdl/
        sdl_app.c              # giblet's, drives Mac / desktop
        sdl_app.h              # giblet's
      arm/
        arm_app.c              # giblet's, drives MiSTer + future Pi
        arm_app.h              # giblet's
        arm_display.c          # giblet's selector
        arm_display.h          # giblet's
        arm_display_native_video_writer.c   # NEW — wraps NativeVideoWriter
        arm_display_drm.c      # giblet's (DRM/KMS, future RPi)
        arm_display_fbdev.c    # giblet's (generic /dev/fb0, future ARM Linux)
    video/
      software/                # giblet's, becomes unconditional
        software_renderer.{c,h}
        sw_blit.{c,h}
        sw_blit_neon.c
        sw_blit_neon_armv7.c
        sw_convert.{c,h}
  port/
    sdl/
      native_video_writer.{c,h}     # KEEP — wrapped by new ArmDisplay backend
      netplay_screen.{c,h}          # KEEP, surface unchanged or thin-wrapped
      netstats_renderer.{c,h}       # KEEP, surface unchanged or thin-wrapped
      sdl_pad.c                     # KEEP (input backend)
      scanline_renderer.{c,h}       # KEEP iff still wired by giblet sdl_app;
                                    # else delete
    text/
      text_overlay.{c,h}            # NEW — shared 8x8 ASCII font module
                                    # (or src/port/sdl/text_overlay if preferred;
                                    #  see Phase 4)
    config/                         # unchanged
    sound/                          # unchanged
    io/                             # unchanged
    resources.c                     # unchanged
```

Files deleted by the end of the rip (full list):

- `src/port/sdl/sdl_app.c` (11,734 LOC)
- `src/port/sdl/sdl_game_renderer.c` (13,722 LOC, no public
  header — its symbols are exported via
  `include/port/sdl/sdl_game_renderer.h`, also deleted)
- `include/port/sdl/sdl_game_renderer.h`
- `include/port/sdl/sdl_message_renderer.h` — only if the
  re-homed message overlay no longer needs the SDL-texture surface
- `include/port/sdl/sdl_debug_text.h` — only if the re-homed
  debug-text overlay no longer needs SDL-renderer init
- `src/port/sdl/sdl_message_renderer.c` (154 LOC) — if collapsed
  into `port/text/text_overlay`
- `src/port/sdl/sdl_debug_text.c` (92 LOC) — if collapsed into
  `port/text/text_overlay`
- `src/port/sdl/fbdev_presenter.c` (2,883 LOC)
- `src/port/sdl/fbdev_presenter.h` (116 LOC)
- `src/port/sdl/software_frame_parity.c` (801 LOC) — software-frame
  direct-present helper, dies with sdl_game_renderer
- `src/port/sdl/software_frame_non_integer.c` (839 LOC) — same
- `src/port/sdl/scanline_renderer.{c,h}` (115 + header) — only
  if no caller in giblet's sdl_app; verify before deleting
- All `THIRDSARM_USE_GIBLET_RENDERER` plumbing in `CMakeLists.txt`
  and `src/rendering/renderer.c` (~50 LOC)
- All `SDLGameRenderer_GetPerfCapture*` callsites in any
  remaining `port/sdl/*` files

Approximate net delete: 27,000 LOC source minus ~2,200 LOC new /
imported giblet code = ~25K net.

## ArmDisplay native-video-writer backend design

### New files

`src/platform/app/arm/arm_display_native_video_writer.c` —
implements the four backend entry points required by
`arm_display.c:43-47` (verify exact symbol names against pr-243
`arm_display.c:23-27`):

```
bool arm_display_native_video_writer_init(void);
void arm_display_native_video_writer_shutdown(void);
void arm_display_native_video_writer_get_resolution(int* w, int* h);
void arm_display_native_video_writer_present(const uint32_t* px, int w, int h);
```

Header `src/platform/app/arm/arm_display_native_video_writer.h`
**not** required — backends in giblet's design are referenced via
forward declarations inside `arm_display.c`. Match that pattern to
avoid leaking the symbol.

### Public surface (per-function semantics)

- `init`: calls `NativeVideoWriter_Init()` from
  `src/port/sdl/native_video_writer.h:10`. Returns the wrapped
  result. Logs which backend was selected.
- `shutdown`: calls `NativeVideoWriter_Shutdown()` from
  `src/port/sdl/native_video_writer.h:13`.
- `get_resolution`: returns `(384, 224)` always — the FPGA DDR3
  reader has a fixed frame format (see
  `src/port/sdl/native_video_writer.c:17-19`,
  `NV_FRAME_WIDTH=384`, `NV_FRAME_HEIGHT=224`). Do **not** report
  the host framebuffer resolution — there is no scaling on this
  path; the FPGA composes at native CPS3 resolution.
- `present`:
  1. Convert the input ARGB8888 canvas to RGB565 in a static
     scratch buffer (`uint16_t scratch[384 * 224]`). The conversion
     loop already exists at `src/port/sdl/sdl_app.c:225` (the
     `convert_argb8888_to_rgb565` static helper definition; a call
     site lives at `sdl_app.c:10777`) — copy the function body over.
  2. Call `NativeVideoWriter_WriteFrame(scratch, 384, 224, 384 * 2)`
     from `src/port/sdl/native_video_writer.h:21`.
  3. **Optimisation deferred**: when `CRS_SW_CANVAS_16BPP` is on
     (which it is under PORT_MISTER per the gating below), the
     giblet canvas is already RGB565 and can be passed straight to
     the writer. This requires changing the giblet
     `ArmDisplay_Present` signature to accept the canvas-format
     pixel type (`SWCanvasPixel*` not `uint32_t*`) — out of scope
     for the initial rip; see "What this plan does NOT cover".

### Selector integration

In `arm_display.c`, the new backend goes **first** in the
`ArmDisplay_Init` chain when `PORT_MISTER` is defined, **before**
the existing DRM / fbdev attempts (giblet's selector tries
`drm` then `fbdev` at `arm_display.c:54-66`):

```c
bool ArmDisplay_Init() {
#if defined(PORT_MISTER)
    if (arm_display_native_video_writer_init()) {
        active_backend = DISPLAY_BACKEND_NATIVE_VIDEO_WRITER;
        fprintf(stderr, "[arm_display] using FPGA native video writer (DDR3)\n");
        return true;
    }
    /* PORT_MISTER but native_video_writer init failed: bail rather than
       falling back to fbdev/DRM, which would try to drive the Linux fb0
       on the wrapper-core path that doesn't expose one to userspace. */
    fprintf(stderr, "[arm_display] PORT_MISTER but NativeVideoWriter init failed\n");
    return false;
#endif
    if (arm_display_drm_init())   { ... }
    if (arm_display_fbdev_init()) { ... }
    return false;
}
```

Add `DISPLAY_BACKEND_NATIVE_VIDEO_WRITER` to the
`DisplayBackend` enum at giblet's `arm_display.c:43`. Update
`Shutdown`, `GetResolution`, `Present`, and
`ComputePresentRect` switches to handle the new case.

`ArmDisplay_ComputePresentRect` for the NativeVideoWriter backend
returns `(0, 0, 384, 224)` — there is no letterbox computation on
the FPGA path; the FPGA composes the picture into the analog
timing.

### FPGA vsync feedback survival

The current FPGA vsync feedback loop lives in
`src/port/sdl/sdl_app.c:9569-9615` (`poll_vsync_feedback`) and is
plumbed into the frame pacer via `last_vsync_monotonic_ns`
(`sdl_app.c:184`), `vsync_feedback_valid` (`sdl_app.c:185`),
`last_feedback_seq` (`sdl_app.c:180`), etc.

Place the feedback logic **inside `arm_app.c`'s `end_frame`**
between the `ArmDisplay_Present` call and the `SDL_DelayNS`
pacer step — that is the closest equivalent to where it runs
today. The actual register reads
(`NativeVideoWriter_ReadFeedback`,
`NativeVideoWriter_ReadFeedbackSeq`) come from
`src/port/sdl/native_video_writer.h:30-34` and remain accessible
from `arm_app.c` after the rip.

Two state-management options — pick one in Phase 3:

- **Option A (simpler)**: keep the feedback state as static
  variables inside `arm_app.c`, copy `poll_vsync_feedback` body
  into a static helper there, gate on `#if defined(PORT_MISTER)`.
- **Option B (cleaner)**: expose a public API on the ArmDisplay
  backend itself — e.g. `ArmDisplay_PollVsyncFeedback(uint64_t*
  out_vsync_monotonic_ns)` that the backend implements per backend
  (NativeVideoWriter does the real work, DRM returns the page-flip
  vblank timestamp for free, fbdev returns false). Cleaner long
  term but extends the rip into giblet's `arm_display.h` public
  API — out of scope for the first pass.

Recommendation: **Option A** for the rip. Refactor to Option B in
a follow-up.

Pacer state in giblet's `arm_app.c` today is minimal: only
`target_frame_time_ns` (line 34) and `frame_deadline` (line 33) — the
deadline pacer body lives at `arm_app.c:131-160` and is open-loop.
Phase 3 must REPLACE that simple deadline pacer with an
FPGA-feedback-aware pacer: introduce new
`last_feedback_seq` / `last_vsync_monotonic_ns` /
`vsync_feedback_valid` / `lead_time_ns` / `busywait_threshold_ns`
state, and port the closed-loop logic from our `sdl_app.c`
(state at `sdl_app.c:180, 184, 185`; `poll_vsync_feedback` body at
`sdl_app.c:9569-9615`; deadline-vs-vsync arbitration at
`sdl_app.c:11029-11060`).

## Font infrastructure for overlays

### Need

Five overlays survive the rip and currently rely on the SDL
renderer:

- FPS overlay simple mode (top-left "fps")
- FPS overlay debug mode (multi-line — to be SHRUNK in scope)
- SDLMessageRenderer (transient on-screen messages)
- SDLDebugText (debug build only — `flPrintL` plumbing)
- NetplayScreen / NetstatsRenderer (already in
  `src/port/sdl/netplay_screen.{c,h}` and
  `src/port/sdl/netstats_renderer.{c,h}`, 117 + 19 LOC)

After the rip, none of them have an `SDL_Renderer*`. Everything
draws into the SoftwareRenderer canvas via
`SoftwareRenderer_DrawUIBitmap` (already in tree at
`src/platform/video/software/software_renderer.h:25`):

```
void SoftwareRenderer_DrawUIBitmap(float x, float y, float z,
                                   const uint32_t* argb_pixels,
                                   int w, int h, unsigned int color);
```

The giblet renderer already uses this for input-history glyphs in
`src/rendering/renderer.c:225-231` (commit 686a5e2b). Same
plumbing for the overlays.

Build one shared text module — **not** five — because we need
identical line-height, baseline, and modulation behaviour across
overlays.

### Module location

`src/port/text/text_overlay.{c,h}` — sits outside both `sdl/` and
`platform/` because it is renderer-host-agnostic (renders into
ARGB8888 glyph bitmaps, calls `SoftwareRenderer_DrawUIBitmap`).
Includes only `<stdint.h>`, `<stdbool.h>`, and
`platform/video/software/software_renderer.h`. No SDL include.

Public surface (proposal — refine in Phase 4):

```c
// Initialize bitmap font cache (idempotent).
void TextOverlay_Init(void);

// Draw a NUL-terminated ASCII string into the canvas.
// argb_color: 0xAARRGGBB modulation (0xFFFFFFFF = no modulation).
// scale: integer pixel scale (1, 2, 3 ... ); 1 is 8x8 native.
void TextOverlay_DrawString(int x, int y, float z,
                            const char* str,
                            uint32_t argb_color, int scale);

// Convenience: pixel width of `str` at `scale` after Init.
int TextOverlay_MeasureString(const char* str, int scale);

// Convenience: line height in pixels at `scale`.
int TextOverlay_LineHeight(int scale);
```

### Font choice

Use a hand-rolled 8x8 ASCII font (printable range 0x20–0x7E).
Rationale: open-source 8x8 PD candidates are nice but each one
adds a license boilerplate and a hex blob. We need ~95 glyphs
× 8 bytes = 760 bytes of font data, which is trivial to author or
adapt from one of the public-domain 8x8 fonts often shipped with
emulators / demoscene tooling.

Candidates if the user prefers an existing font (planner is not
making this call without input):

- **Cozette** (6x13) — too tall for 384x224 overlays at scale 1.
- **Spleen 5x8** — 5 column, suits narrow overlays.
- **PixelOperator 8x8** — clean 8x8, CC0.
- **IBM 8x8 (CGA / VGA)** — public domain, the de-facto retro
  pixel font; widely available as a hex array.
- **dhepper/font8x8** on GitHub — public domain, drop-in C array.

Default recommendation: **dhepper/font8x8** (file
`font8x8_basic.h`) — CC0, single-header, ASCII-only, ~95 glyphs,
no rendering code, fits in ~1.5 KB once embedded.

### Scope of the shrunk debug-mode FPS overlay

After the rip, the available metrics are limited to what
`arm_app.c` / `sdl_app.c` measure on the loop timeline:

- **frame** (whole frame ms, end-to-end)
- **update** (game logic — `Main_StepFrame`, around begin/end_frame)
- **render** (`SoftwareRenderer_RenderFrame` ms)
- **present** (`ArmDisplay_Present` ms, which is the
  `NativeVideoWriter_WriteFrame` time on MiSTer)
- **idle** (frame-pacer sleep ms — already exposed by
  `FrameMetrics` in giblet's `sdl_app.h:7-12`)

If cheap to add (Phase 5 — TBD by code probe): expose the giblet
software renderer's internal sort and raster timings via two
counters at the top of `software_renderer.c`. Drop everything
else from the current 50+ counter list at
`src/port/sdl/sdl_app.c:110-164`.

## CRS_SW_CANVAS_16BPP gating

### Current state (pre-rip)

`CMakeLists.txt:147-157` defines `CRS_SW_CANVAS_16BPP=1`
unconditionally whenever `THIRDSARM_USE_GIBLET_RENDERER=ON`,
regardless of the host architecture.

This is a problem on Mac flag-ON because `sw_present_scale_argb`
(used by giblet's `arm_display_fbdev_present` and intended for any
ARGB present path) is gated `!CRS_SW_CANVAS_16BPP` at
pr-243 `sw_blit.h:111-115`. With the flag ON on Mac today the
canvas is RGB565 and the SDL streaming texture is created as
`SDL_PIXELFORMAT_RGB565` at `sdl_app.c:10662-10664` — that path
dies with the sdl_app.c rip.

### Post-rip rule

Define `CRS_SW_CANVAS_16BPP=1` if and only if `PORT_MISTER` is
defined. All other targets — Mac, Windows, future ARM Linux Pi —
get ARGB8888 canvas.

```cmake
target_compile_definitions(3s-arm PRIVATE
    CRS_VIDEO_DRIVER_SOFTWARE=1
    CRS_SW_INDEXED8_KERNEL_DEFAULT='b'
)
if(PORT_MISTER)
    target_compile_definitions(3s-arm PRIVATE CRS_SW_CANVAS_16BPP=1)
endif()
```

`THIRDSARM_USE_GIBLET_RENDERER` and the cmake option that toggles it
are removed entirely.

### Fail-fast invariant

Add a CMake check that errors when something tries to use
NativeVideoWriter without `PORT_MISTER`. Today
`src/port/sdl/native_video_writer.c:3-123` is already gated `#if
defined(PORT_MISTER)` (the `#else` is at line 125) with a stub fallback at
`native_video_writer.c:127-152` (the `#endif` is at line 153). The cmake check belongs in the new
`arm_display_native_video_writer.c` build wiring — only compile it
into the binary when `PORT_MISTER` is set:

```cmake
if(PORT_MISTER)
    target_sources(3s-arm PRIVATE
        src/platform/app/arm/arm_display_native_video_writer.c
    )
endif()
```

If a future target wants the NativeVideoWriter-style backend
without `PORT_MISTER`, the CMake error is the signal to extend the
gating deliberately, not to silently fall back to a non-functional
stub.

## Phased execution

Each phase is one `/implement` session, target ≤ 2 hours of agent
work. Phases are written to be runnable as
`/implement docs/plan-giblet-wholesale-rip.md#phase-N`.

---

### Phase 1 — Branch + planning artifact

#### Why it matters

Anchors the rip in a clean git topology before any code changes.
Verifies the three baseline builds still work so that any
subsequent breakage is unambiguously caused by rip work, not by
pre-existing rot.

#### Files to read before implementing

- `docs/plan-giblet-wholesale-rip.md` (this file, in full)
- `AGENTS.md`
- `docs/building.md`
- `docs/mister-runbook.md` (the build wrapper invocation)

#### Files to create / modify

- New branch `giblet-rip` cut off `mister`.
- Cherry-pick (or `git restore --source=giblet-stuff -- ...`) the
  6 activation commits from `giblet-stuff`:
  `e31a10e6 .. 686a5e2b`. **Do not** include the import commit
  `83e99cc5` separately — it is already covered by the
  cherry-pick range starting at `e31a10e6` because the activation
  commits depend on the import. If `e31a10e6` doesn't apply
  cleanly, cherry-pick `83e99cc5` first.
- Land this plan file under `docs/`.
- No source code changes in Phase 1.

#### Success criteria

- `git log --oneline mister..giblet-rip` shows exactly the
  cherry-picked commits plus the plan-file commit. Expected count:
  6 commits if `e31a10e6` applies cleanly to the `mister`
  baseline; 7 commits if `e31a10e6` doesn't apply cleanly and
  `83e99cc5` had to be cherry-picked first to bring in the giblet
  renderer source. Verify the count branches accordingly — do
  NOT silently accept either count without reading
  `git log --oneline mister..giblet-rip` and tying each commit to
  the cherry-pick range.
- Mac flag-OFF build green:
  `CC=clang cmake -B build/mac-off -DCMAKE_BUILD_TYPE=Release`
  then `cmake --build build/mac-off --parallel`.
- Mac flag-ON build green:
  `CC=clang cmake -B build/mac-on -DCMAKE_BUILD_TYPE=Release
  -DTHIRDSARM_USE_GIBLET_RENDERER=ON` then build.
- MiSTer cross build green (giblet renderer OFF):
  `MISTER_BUILD_CONTAINER=3s-mister-giblet-build tools/mister/build-game.sh --flavor telemetry`.
  Binary lands under `build/mister-telemetry-install/`.
- MiSTer cross build green (giblet renderer ON):
  `MISTER_BUILD_CONTAINER=3s-mister-giblet-build EXTRA_CMAKE_ARGS="-DTHIRDSARM_USE_GIBLET_RENDERER=ON" tools/mister/build-game.sh --flavor telemetry`.
  `build-game.sh` accepts the cmake passthrough via the
  `EXTRA_CMAKE_ARGS` env var (see `tools/mister/build-game.sh:41-48`,
  `113`, `161`).

#### Dependencies

None.

#### What NOT to do

- Do **not** start cherry-picking unrelated commits from `mister-dev`
  or `main`.
- Do **not** modify the giblet renderer code yet.
- Do **not** delete anything yet.

#### Fallback / diagnose

If a cherry-pick conflicts: stop, do not force resolution.
Inspect the conflict — if it touches `src/rendering/renderer.c`
or the `THIRDSARM_USE_GIBLET_RENDERER` cmake gate, the resolution
is mechanical (take the giblet-stuff side). Anything else (e.g. a
conflict in `sdl_app.c`'s FPS overlay block) means another
mister-branch commit landed in the interim — refresh the plan
range and try again.

---

### Phase 2 — Import giblet's app/display tree side-by-side

#### Why it matters

Lands giblet's `src/platform/app/sdl/*` and
`src/platform/app/arm/*` files into the tree without activating
them. The existing builds remain unchanged because the giblet
files are gated on `CRS_APP_DRIVER_SDL` and `CRS_APP_DRIVER_ARM`,
neither of which is defined. This decouples the "import giblet
code" risk from the "wire it up" risk.

#### Files to read before implementing

- `pr-243:src/platform/app/sdl/sdl_app.{c,h}` (490 + 19 LOC)
- `pr-243:src/platform/app/arm/arm_app.{c,h}` (224 + 6 LOC)
- `pr-243:src/platform/app/arm/arm_display.{c,h}` (165 + 23 LOC)
- `pr-243:src/platform/app/arm/arm_display_drm.c` (550 LOC)
- `pr-243:src/platform/app/arm/arm_display_fbdev.c` (133 LOC)
- `pr-243:CMakeLists.txt:14-50` (CRS_RPI_DRM / CRS_MISTER setup)
- Current `CMakeLists.txt:1-50, 77-200` (option parsing and the
  GAME_SRC GLOB)

#### Files to create / modify

Create (verbatim from pr-243, modulo include-path adjustments
between giblet's tree layout and ours):

- `src/platform/app/sdl/sdl_app.c`
- `src/platform/app/sdl/sdl_app.h`
- `src/platform/app/arm/arm_app.c`
- `src/platform/app/arm/arm_app.h`
- `src/platform/app/arm/arm_display.c`
- `src/platform/app/arm/arm_display.h`
- `src/platform/app/arm/arm_display_drm.c`
- `src/platform/app/arm/arm_display_fbdev.c`
- `src/port/input_backend.h` (verbatim from
  `pr-243:src/port/input_backend.h`) — declares
  `InputBackend_Init`, `InputBackend_Shutdown`,
  `InputBackend_HandleGamepadDeviceEvent`,
  `InputBackend_GetButtonState`, etc. Required by giblet's
  `arm_app.c:9` (`#include "port/input_backend.h"`) and
  `pr-243:src/platform/app/sdl/sdl_app.c` (input handling). Our
  `src/port/sdl/sdl_pad.{c,h}` already exists and is the
  underlying SDL_GameController shim that giblet's
  `input_backend.c` wraps — keep `sdl_pad.{c,h}` as-is.
- `src/port/input_backend.c` (verbatim from
  `pr-243:src/port/input_backend.c`) — implementation that
  delegates to `SDLPad_*` from our existing `port/sdl/sdl_pad.h`.
  The GLOB at `CMakeLists.txt:77` picks it up automatically; no
  cmake edit required for this file alone.

**Note on header location**: pr-243 puts headers next to their `.c`.
Our convention puts them under `include/`. Follow giblet's
convention for these new directories — keep `arm_display.h` next to
`arm_display.c`. The shared include paths already cover
`src/` (current `CMakeLists.txt:46`-ish — verify) so
`#include "platform/app/arm/arm_display.h"` resolves.

Modify:

- `CMakeLists.txt` — add giblet's gates as **disabled by default**:
  - Add `option(CRS_USE_GIBLET_APP "Use giblet app/display drivers" OFF)`.
  - When `CRS_USE_GIBLET_APP=ON` and `PORT_MISTER`, set
    `CRS_APP_DRIVER_ARM=1` and pull the new `src/platform/app/arm`
    files into the build.
  - When `CRS_USE_GIBLET_APP=ON` and **not** `PORT_MISTER`, set
    `CRS_APP_DRIVER_SDL=1` and pull the new `src/platform/app/sdl`
    files into the build (these are GLOB-included automatically by
    the existing `file(GLOB_RECURSE GAME_SRC ... src/*.c)` at
    `CMakeLists.txt:77`, but the `#if CRS_APP_DRIVER_*` guards
    keep them empty until the option is on — same pattern as the
    giblet renderer files).
  - Add libdrm autodetection (gated to non-Apple ARM Linux):
    `find_package(PkgConfig QUIET); pkg_check_modules(DRM
    QUIET libdrm); if(DRM_FOUND) define CRS_ARM_HAVE_DRM`. On
    `PORT_MISTER`, libdrm is **not** required — the
    NativeVideoWriter backend is the active one. Keep the
    `arm_display_drm.c` source compiled when `CRS_USE_GIBLET_APP`
    is on for non-MiSTer, but #ifdef'd out otherwise.

The `arm_display.{c,h}` selector, `arm_display_drm.c`, and
`arm_display_fbdev.c` files **also** need the
`CRS_APP_DRIVER_ARM` gate at the top — verify the imported files
match pr-243 line 1 each (`#if CRS_APP_DRIVER_ARM`).

The new `src/platform/app/sdl/sdl_app.c` will collide at
**main()** with our existing `src/port/sdl/sdl_app.c:main` (line
TBD — `grep -n '^int main' src/port/sdl/sdl_app.c`). The giblet
file is gated `#if CRS_APP_DRIVER_SDL` so the body compiles to
empty until the option flips. Confirm this is true by reading
giblet's file top — `pr-243:src/platform/app/sdl/sdl_app.c:1` is
`#if CRS_APP_DRIVER_SDL`. **Verify our existing
`src/port/sdl/sdl_app.c` is NOT also wrapped in
`#if CRS_APP_DRIVER_SDL`** (it is not, today). Two `main()`s
gated on different macros never collide at link time.

#### Success criteria

- All three pre-existing builds (Mac flag-OFF, Mac flag-ON, MiSTer
  cross) still green with `CRS_USE_GIBLET_APP=OFF`.
- `cmake -B build/giblet-app -DCRS_USE_GIBLET_APP=ON`
  configures (does NOT need to build successfully — at this stage
  giblet's sdl_app `#include`s like `platform/video/sdl_generic/sdl_generic_renderer.h`
  will fail because that giblet file isn't imported — that's a
  Phase 8 concern).
- `git diff --stat mister..giblet-rip` shows the 10 new files
  (8 app/display files + `src/port/input_backend.{c,h}`) plus
  CMakeLists.txt edits, no edits to existing source files outside
  `CMakeLists.txt`.

#### Dependencies

Phase 1 (branch + cherry-pick).

#### What NOT to do

- Do **not** flip the new option ON yet.
- Do **not** delete the existing `src/port/sdl/sdl_app.c`.
- Do **not** touch `src/rendering/renderer.c`.
- Do **not** import giblet's `sdl_generic_renderer` or any other
  giblet video driver — those are out of scope (see "What this
  plan does NOT cover").

#### Fallback / diagnose

- libdrm pkg-config missing on the host: gate the
  `pkg_check_modules` behind `if(NOT APPLE AND NOT WIN32)`.
- giblet `arm_app.c` includes
  `platform/video/software/software_renderer.h` — that file is
  already in the tree (commit `83e99cc5`), so this should resolve.
- giblet `arm_app.c` includes `arcade/arcade_balance.h`,
  `port/input_backend.h`, `port/io/afs.h`, `port/sound/adx.h`,
  `port/resources.h`. Verified locally: `arcade/arcade_balance.h`
  exists (`src/arcade/arcade_balance.h`), `port/io/afs.h` exists
  (`src/port/io/afs.h`), `port/sound/adx.h` and `port/resources.h`
  also exist. `port/input_backend.{c,h}` are NOT in our tree —
  imported in this same Phase 2 (see "Files to create / modify"
  above).

---

### Phase 3 — NativeVideoWriter ArmDisplay backend

#### Why it matters

Closes the only MiSTer-specific gap between giblet's `arm_app.c`
loop and our shipping FPGA path. Without this, flipping
`CRS_USE_GIBLET_APP=ON` on MiSTer would either crash (no display)
or silently drive `/dev/fb0` (which is not the FPGA path and would
not light up the analog CRT — see
`docs/agent-memory/mister-native-analog-crt.md` and
`~/.claude/projects/-Users-sb-Developer-3sx-mister/memory/native-analog-svideo-fix.md`).

#### Files to read before implementing

- `src/port/sdl/native_video_writer.{c,h}` (153 + 41 LOC)
- `pr-243:src/platform/app/arm/arm_display.c` (165 LOC) — selector
  pattern
- `pr-243:src/platform/app/arm/arm_display_fbdev.c` (133 LOC) —
  reference backend impl
- Current `src/port/sdl/sdl_app.c:9569-9615`
  (`poll_vsync_feedback`)
- Current `src/port/sdl/sdl_app.c:10723-10750` (giblet→native
  present block)
- `docs/design-fpga-native-video.md`
- `docs/reference-native-analog-video.md`

#### Files to create / modify

Create:

- `src/platform/app/arm/arm_display_native_video_writer.c`
  (~150 LOC) implementing the four backend entry points described
  in "ArmDisplay native-video-writer backend design" above.

Modify:

- `src/platform/app/arm/arm_display.c` (giblet's, just imported in
  Phase 2):
  - Add `DISPLAY_BACKEND_NATIVE_VIDEO_WRITER` to the enum.
  - Add forward declarations for the four new backend entry points
    under `#if defined(PORT_MISTER)`.
  - Insert the new init attempt **first** in `ArmDisplay_Init`
    when `PORT_MISTER` is defined (see "Selector integration"
    above).
  - Update `Shutdown`, `GetResolution`, `Present`, and
    `ComputePresentRect` switches.
- `CMakeLists.txt`: add the new `.c` to the
  `target_sources(3s-arm PRIVATE ...)` block under `if(PORT_MISTER)`.

Leave the FPGA vsync feedback logic in `arm_app.c` — implement it
as a static helper there, gated `#if defined(PORT_MISTER)`,
copied verbatim from `src/port/sdl/sdl_app.c:9569-9615`. Wire it
into `arm_app.c::end_frame` after `ArmDisplay_Present` and before
the `SDL_DelayNS` pacer step.

#### Success criteria

- Builds green with `CRS_USE_GIBLET_APP=OFF` (no behavioural
  change — new backend is dead code).
- Builds green with `CRS_USE_GIBLET_APP=ON` for the MiSTer cross
  build.
- New backend's `init` returns `false` on a desktop host (not
  `PORT_MISTER`); does **not** link against `/dev/mem`. Confirm by
  building with `CRS_USE_GIBLET_APP=ON` on Mac and inspecting
  the symbol table — `nm 3SX | grep native_video_writer` should
  show only the stub functions from `native_video_writer.c:127-153`.

#### Dependencies

Phase 2.

#### What NOT to do

- Do **not** activate giblet's app driver yet (Phase 8).
- Do **not** delete `src/port/sdl/sdl_app.c`'s vsync feedback
  block yet — both copies coexist until Phase 9a.
- Do **not** introduce a public `ArmDisplay_PollVsyncFeedback`
  API. That is the cleaner Option B from the design section,
  deferred to a follow-up.

#### Fallback / diagnose

- If `NativeVideoWriter_Init` fails on real hardware after the
  rip, the symptom is a black screen on the analog output. Check:
  `vga_scaler=0` in MiSTer.ini (per
  `~/.claude/projects/-Users-sb-Developer-3sx-mister/memory/native-analog-svideo-fix.md`),
  `/dev/mem` open succeeds (perms), and the FPGA core is the
  current `3S-ARM.rbf` (per the wrapper memory).
- If the vsync feedback never engages, the backend_logf line at
  `sdl_app.c:9611` "closed-loop vsync feedback engaged" should be
  ported alongside the helper.

---

### Phase 4 — Shared font / text-overlay module

#### Why it matters

After the rip, no overlay has an `SDL_Renderer*` to draw into.
Building this module first (before migrating any specific overlay)
means each overlay-migration phase is purely "swap the call
target", not "design and swap simultaneously".

#### Files to read before implementing

- `src/platform/video/software/software_renderer.h:25` (the
  `SoftwareRenderer_DrawUIBitmap` signature)
- `src/rendering/renderer.c:197-232` (the input-history-glyph
  caller — already a pattern for cached ARGB bitmaps fed through
  `SoftwareRenderer_DrawUIBitmap`)
- `src/port/sdl/sdl_message_renderer.c` (current SDL-coupled
  message overlay — for behavioural reference only)
- `src/port/sdl/sdl_debug_text.c` (current SDL-coupled debug-text
  overlay — for behavioural reference only)

#### Files to create / modify

Create:

- `src/port/text/text_overlay.h` — public surface (see "Font
  infrastructure for overlays" above).
- `src/port/text/text_overlay.c` — implementation.
- `src/port/text/font8x8_basic.h` — 8x8 ASCII font data
  (vendored from dhepper/font8x8 or hand-rolled, see "Font choice"
  above; user picks before Phase 4 starts — see Unknowns below).

Modify:

- `CMakeLists.txt`: the GLOB at line 77 picks up `src/port/text/*.c`
  automatically (rooted at `src/`). No edit required.

Implementation notes:

- Build the glyph cache lazily on first use, mirroring the pattern
  in `src/rendering/renderer.c:204-223`. One ARGB bitmap per
  printable ASCII codepoint, allocated once.
- `argb_color` modulation: pass straight through to
  `SoftwareRenderer_DrawUIBitmap`'s `color` param, which already
  handles per-call modulation.
- `scale > 1`: render the glyph at `scale` by upscaling the cached
  bitmap into a per-call scratch buffer. Cache only the 1x bitmaps
  to keep the data footprint tiny.

#### Success criteria

- Build green on Mac (flag-OFF and flag-ON) and MiSTer cross.
- Add a one-shot smoke test: in any existing host build, call
  `TextOverlay_DrawString(10, 10, 0.0f, "TEST", 0xFFFFFFFFu, 1)`
  from a known frame point (e.g. an `#ifdef
  TEXT_OVERLAY_SMOKE` block at the top of
  `software_renderer.c::SoftwareRenderer_RenderFrame`) and verify
  the string appears on screen by visual inspection. Remove the
  smoke test before commit.

#### Dependencies

Phase 1 (so the giblet renderer is on the branch and
`SoftwareRenderer_DrawUIBitmap` is callable).

#### What NOT to do

- Do **not** migrate any existing overlay yet (Phases 5/6).
- Do **not** add Unicode support. ASCII printable range only.
- Do **not** build a sub-pixel / antialiased renderer. Pixel-grid
  bitmap blits only — matches the analog-CRT aesthetic anyway.

#### Fallback / diagnose

- Glyphs render in the wrong position: verify `software_renderer.c`'s
  draw-list ordering — `SoftwareRenderer_DrawUIBitmap` may have
  z-order semantics that differ from `SoftwareRenderer_DrawSprite`.
  Check the renderer's draw-list flush order in `software_renderer.c`.
- Modulation looks wrong: confirm the
  `SoftwareRenderer_DrawUIBitmap` `color` param's byte order
  (ARGB vs RGBA — the input-history-glyph caller uses `0xFFRRGGBB`).

---

### Phase 5 — FPS overlay onto the new font module

#### Why it matters

First overlay rebuilt on top of the post-rip text infrastructure.
Validates the font module on a real, visible, frame-rate-critical
overlay.

#### Files to read before implementing

- Current `src/port/sdl/sdl_app.c:9156-9197`
  (`init_show_fps_overlay`)
- Current `src/port/sdl/sdl_app.c:110-164` (FPS overlay state
  variables — `fps_overlay_mode` at 110, accumulators 122 onward;
  identify which counters survive the rip per feature decisions in
  the prompt)
- Current `src/port/sdl/fbdev_presenter.h:97-108`
  (`FBDevPresenter_SetFPSOverlayMode`,
  `FBDevPresenter_ApplyFPSOverlayToBuffer`,
  `FBDevPresenter_ApplyFPSOverlayToRGB565Buffer` — the existing
  fps-overlay drawing API; understand the rendered layout to keep
  it visually identical)
- `pr-243:src/platform/app/sdl/sdl_app.h:7-12` (the
  `FrameMetrics` ring buffer) — already exposes
  `fps[FRAME_METRICS_COUNT]`, `frame_time[]`, `idle_time[]`. Reuse.
- `pr-243:src/platform/app/sdl/sdl_app.c:323-340`
  (`update_metrics`) — pacer integration site for adding
  update/render/present accumulators.

#### Files to create / modify

Create:

- `src/port/sdl/fps_overlay.{c,h}` (or
  `src/port/text/fps_overlay.{c,h}` if grouped with the text
  module — pick one in implementation, planner doesn't insist).

Modify:

- `src/platform/app/sdl/sdl_app.c` (giblet's) — extend
  `FrameMetrics` with `update_time[]`, `render_time[]`,
  `present_time[]` accumulators. Sample around `Main_StepFrame`,
  `SoftwareRenderer_RenderFrame`, and `ArmDisplay_Present` (or its
  SDL equivalent).
- Same edits to `src/platform/app/arm/arm_app.c`.
- Add an `FPSOverlay_Render(void)` call at the end of each
  `end_frame` after the main scene's
  `SoftwareRenderer_RenderFrame` but before the present, so the
  overlay text is in the canvas when `ArmDisplay_Present` /
  `SDLGenericRenderer_RenderFrame` runs.

The overlay reads `Config_GetString(CFG_KEY_SHOW_FPS)` per-frame
(or via a re-init helper) — same hot-reload behaviour as today.

Modes preserved per the user's feature decisions:

- `off`: no draw.
- `fps` (top-left): single line, integer FPS.
- `debug` (multi-line): SHRUNK to frame / update / render /
  present / idle, plus sort/raster from the giblet renderer if
  cheap to add (TBD per code probe in Phase 5).

#### Success criteria

- Mac build (`CRS_USE_GIBLET_APP=OFF`, no rip activation) still
  shows the legacy FPS overlay — unchanged.
- Mac build with `CRS_USE_GIBLET_APP=ON` shows the new overlay
  in both `fps` and `debug` modes; visually compare on screen
  against the legacy overlay.
- MiSTer build with `CRS_USE_GIBLET_APP=ON` displays the overlay
  via the new NativeVideoWriter present path (Phase 3).
  Verify on device.
- Per-frame sampling overhead < 50 us measured by the new
  `frame_time` counter itself.
- App-loop perf telemetry wired: the giblet `FrameMetrics` ring
  buffer (`pr-243:src/platform/app/sdl/sdl_app.h:7-12`) is
  extended with `update_time[]`, `render_time[]`,
  `present_time[]` accumulators, sampled in both giblet `arm_app.c`
  and `sdl_app.c` `end_frame` paths around `Main_StepFrame`,
  `SoftwareRenderer_RenderFrame`, and `ArmDisplay_Present` /
  SDL-equivalent. The new FPS overlay reads these accumulators —
  not just the existing `fps[]` / `frame_time[]` / `idle_time[]`.
- Hot-reload of `CFG_KEY_SHOW_FPS` survives: changing the value at
  runtime via the config UI flips overlay mode within one frame
  (same behaviour as today's `init_show_fps_overlay` re-call at
  `sdl_app.c:9469`).

#### Dependencies

Phases 2, 3, 4.

#### What NOT to do

- Do **not** touch the legacy
  `FBDevPresenter_ApplyFPSOverlayToBuffer` paths in
  `src/port/sdl/sdl_app.c:10731-10775`. They die with sdl_app.c
  in Phase 9a.
- Do **not** add any of the dropped counters listed under "DROP"
  in the prompt (raster bucket timings, fast-non-integer
  families, etc.).

#### Fallback / diagnose

- Overlay invisible on MiSTer but visible on Mac: check that the
  overlay is drawn into the canvas **before**
  `ArmDisplay_Present` (the NativeVideoWriter backend reads the
  canvas and writes to DDR3 — anything drawn after is lost until
  the next frame).
- Overlay flicker / tear: probably racing with the FPGA vsync
  feedback. Verify the present-then-feedback ordering in
  `arm_app.c::end_frame`.

---

### Phase 6 — SDLMessageRenderer + SDLDebugText onto the new font module

#### Why it matters

Two more overlays migrated. After this, the only feature still
tied to `SDLGameRenderer` and `SDLMessageRenderer`'s SDL_Texture
surface is the legacy app loop.

#### Files to read before implementing

- `src/port/sdl/sdl_message_renderer.c` (154 LOC)
- `include/port/sdl/sdl_message_renderer.h` (17 LOC)
- `src/port/sdl/sdl_debug_text.c` (92 LOC)
- `include/port/sdl/sdl_debug_text.h` (17 LOC)
- All callers — find with
  `grep -rln 'SDLMessageRenderer_\|SDLDebugText_'
  src/`. Expect callers in `src/main.c`, the overlay text
  pipelines for in-game messages, `flPrintL` infrastructure.

#### Files to create / modify

Create:

- `src/port/text/message_overlay.{c,h}` — replaces
  `SDLMessageRenderer`. Public surface keeps existing entry point
  names where possible to minimize caller diff:
  - `MessageOverlay_BeginFrame(void)`
  - `MessageOverlay_DrawString(int x, int y, const char* str, uint32_t argb)`
  - `MessageOverlay_HasContent(void)`
  - Drop `MessageOverlay_CreateTexture` /
    `MessageOverlay_DrawTexture` — the SDL-texture concept is
    gone. If any caller of `SDLMessageRenderer_CreateTexture` /
    `SDLMessageRenderer_DrawTexture` exists in the game code,
    rewrite it to use bitmap glyphs via `TextOverlay_DrawString`.
- `src/port/text/debug_text.{c,h}` — replaces `SDLDebugText`.
  Public surface:
  - `DebugText_Initialize(void)` (no SDL_Renderer arg)
  - `DebugText_Render(void)`
  - `DebugText_Destroy(void)`

Modify:

- All callers of `SDLMessageRenderer_*` and `SDLDebugText_*` —
  swap include + symbol.
- Rip-branch only: keep the old `sdl_message_renderer.{c,h}` and
  `sdl_debug_text.{c,h}` compiled until Phase 9b, gated `#if
  !CRS_USE_GIBLET_APP`. This avoids breaking the legacy app loop
  until it dies.

#### Success criteria

- Builds green at `CRS_USE_GIBLET_APP=OFF` (legacy overlays
  unchanged).
- Builds green at `CRS_USE_GIBLET_APP=ON`. New overlays render
  the same content, visually verified on Mac.
- MiSTer build green; on-device verification of message overlay
  (any in-game message — settings change, save state, etc.).

#### Dependencies

Phases 2, 3, 4.

#### What NOT to do

- Do **not** delete `sdl_message_renderer.{c,h}` or
  `sdl_debug_text.{c,h}` yet — that's Phase 9b.
- Do **not** redesign the message-content state machine. The
  feature behaviour is unchanged; only the drawing backend changes.

#### Fallback / diagnose

- Some callers pass an SDL_Renderer* to
  `SDLMessageRenderer_CreateTexture`. Those are likely the in-game
  PNG/BMP overlays (loading screens, splash). If the rewrite is
  non-trivial, scope-limit Phase 6 to text-only callers and defer
  bitmap-overlay rewriting to a sub-phase.
- `flPrintL` plumbing: trace it in
  `src/sf33rd/Source/Game/debug/` — calls `SDLDebugText_Print`
  (or similar). Wire to `DebugText_Print` instead.

---

### Phase 7 — Wire netplay (NetplayScreen / NetstatsRenderer / GekkoNet) into giblet's app loop

#### Why it matters

Netplay is a hard requirement (per the user's KEEP list). Giblet's
`pr-243:src/platform/app/sdl/sdl_app.c:349-350` already calls
`NetplayScreen_Render()` and `NetstatsRenderer_Render()` inside a
`#if NETPLAY_ENABLED` block (opens at line 346), but those
calls today depend on the existing SDL_Renderer pipeline. After
the rip, they need to draw into the SoftwareRenderer canvas via
the new text overlay — same pattern as Phases 5/6.

GekkoNet integration is initialized elsewhere
(grep `Gekko_Init` / equivalent) and is not tied to the present
loop directly, but the per-frame poll calls live in our
`sdl_app.c`'s frame loop. Those need to migrate to giblet's
`arm_app.c::end_frame` and `sdl_app.c::end_frame`.

#### Files to read before implementing

- `src/port/sdl/netplay_screen.{c,h}` (117 LOC)
- `src/port/sdl/netstats_renderer.{c,h}` (19 LOC)
- All netplay glue in current `src/port/sdl/sdl_app.c` —
  `grep -n 'gekko\|Gekko\|netplay\|Netplay\|NETPLAY' src/port/sdl/sdl_app.c`
  to find init / poll / cleanup sites
- `pr-243:src/platform/app/sdl/sdl_app.c:346-350` (existing
  netplay call site in giblet's loop — `#if NETPLAY_ENABLED` opens
  at 346, `NetplayScreen_Render()` at 349, `NetstatsRenderer_Render()`
  at 350)
- `~/.claude/projects/-Users-sb-Developer-3sx-mister/memory/project-cross-arch-netplay-recipe.md`
- `~/.claude/projects/-Users-sb-Developer-3sx-mister/memory/project-bilateral-hole-punch.md`

#### Files to create / modify

Modify:

- `src/port/sdl/netplay_screen.c` — rewrite the draw path to use
  `TextOverlay_DrawString` instead of SDL_Renderer.
- `src/port/sdl/netstats_renderer.c` — same.
- Extract netplay frame-glue (init, per-frame poll, cleanup) into
  a new module `src/port/netplay/netplay_loop.{c,h}` callable from
  both:
  - the legacy `src/port/sdl/sdl_app.c` (transitional, until
    Phase 9a deletes it)
  - giblet's `src/platform/app/sdl/sdl_app.c::end_frame`
  - giblet's `src/platform/app/arm/arm_app.c::end_frame`
- Add the calls into giblet's two app loops, gated `#if
  NETPLAY_ENABLED`.

Hot-reload of the cross-arch toggle (`desync_detection`) is
already config-driven (`Config_Get*`); no special wiring needed
beyond keeping the config plumbing.

#### Success criteria

- Builds green at all four matrix points
  (`CRS_USE_GIBLET_APP={OFF,ON}` × `{Mac, MiSTer cross}`).
- Smoke test if practical: Mac↔Mac netplay session over the
  bilateral-hole-punch path. Joins, plays one round, disconnects
  cleanly.
- Smoke test on MiSTer: launch with netplay enabled, observe
  NetplayScreen "waiting for opponent" overlay rendered through
  the new text infrastructure (visual inspection on device).

#### Dependencies

Phases 4, 5, 6.

#### What NOT to do

- Do **not** introduce a new netcode protocol. Same GekkoNet,
  same direct-P2P + bilateral hole punch.
- Do **not** touch the `desync_detection=false` cross-arch toggle
  semantics.
- Do **not** add lobby work (out of scope per prompt).

#### Fallback / diagnose

- Netplay state save/load callbacks include perf-counter
  snapshots that are about to disappear (Phase 9a). If you find a
  callback that includes any `SDLGameRenderer_GetPerfCapture*`
  output in the savedata, REMOVE it from the savedata schema in
  Phase 7 — this is a wire-format compatibility break, document
  it on the rip branch.
- Mid-transition desync: if both peers are mid-rip and one is on
  the legacy loop / the other on giblet's, expect savedata
  mismatch. Coordinate the cross-arch test only between two
  Phase-8 builds.

---

### Phase 8 — Activate giblet's app drivers

#### Why it matters

Single-commit pivot: flip `CRS_USE_GIBLET_APP=ON` as the default
(or simply hard-code the gate) and verify everything still works.

#### Files to read before implementing

- All output from Phases 5, 6, 7 (overlays / netplay glue)
- `tools/mister/build-game.sh` — confirm the wrapper accepts
  the new option / its default
- `docs/mister-runbook.md` — first-light deploy + smoke test

#### Files to create / modify

Modify:

- `CMakeLists.txt`: change
  `option(CRS_USE_GIBLET_APP "..." OFF)` →
  `option(CRS_USE_GIBLET_APP "..." ON)`. Or alternatively, drop
  the option entirely and unconditionally set
  `CRS_APP_DRIVER_ARM=1` for `PORT_MISTER` and
  `CRS_APP_DRIVER_SDL=1` otherwise. Recommended: drop the option,
  because by Phase 8 the legacy path is end-of-life.
- `src/main.c` (and any other top-level entry-point declaration
  files) — verify nothing breaks at link time when both
  `port/sdl/sdl_app.c::main` and `platform/app/sdl/sdl_app.c::main`
  are simultaneously defined under different gates. Phase 9a
  removes the duplicate, but Phase 8 must still link.
- Hold-to-pause input gesture + config plumbing — currently lives
  at `src/port/sdl/sdl_app.c:212` (`hold_to_pause` static),
  `sdl_app.c:9793` (`init_hold_to_pause`),
  `sdl_app.c:9802` (`SDLApp_CycleHoldToPause`),
  `sdl_app.c:9836` (`SDLApp_IsHoldToPauseEnabled`). Port these
  symbols into giblet's app loop (giblet's `arm_app.c` and
  `sdl_app.c`) so the gesture survives the pivot. The pause
  OVERLAY rendering itself goes through `Renderer_Draw*`
  (game-side) and comes along free with the renderer dispatch —
  only the input gesture + config plumbing needs a new home.

#### Success criteria

- All builds green by default (no extra cmake args).
- Mac runs and plays a round of training mode with both `show-fps
  off` and `show-fps debug`.
- MiSTer first-light:
  `MISTER_BUILD_CONTAINER=3s-mister-giblet-build tools/mister/build-game.sh --flavor telemetry`,
  deploy to `/media/fat/games/3s-arm/bin/3s-arm` per
  `docs/mister-runbook.md`, observe FPGA native video output on
  the analog CRT, FPS overlay visible, message overlays visible
  during config changes.
- MiSTer netplay smoke: connect from Mac to MiSTer (cross-arch
  with `desync_detection=false`), play one round.
- Performance: average frame time within ±0.5 ms of the
  pre-Phase-8 measurement on the same MiSTer hardware (sample
  with `show-fps debug` on, normal-gameplay scene).

#### Dependencies

Phases 5, 6, 7.

#### What NOT to do

- Do **not** delete the legacy files yet.
- Do **not** ship a release build until the user signs off on
  first-light + gameplay testing.

#### Fallback / diagnose

- If MiSTer first-light is black: verify
  `arm_display_native_video_writer.c::init` returns true; check
  `dmesg | grep mem` for `/dev/mem` permission errors;
  re-confirm `vga_scaler=0` and the wrapper RBF state.
- If first-light works but the picture is corrupt (wrong
  colours, wrong stride): RGB565 conversion in the new backend's
  `present` is the most likely culprit.
- If the FPS overlay shows 60 fps but gameplay feels slow:
  the FPGA vsync feedback engagement message
  (`backend_logf("Frame pacer: closed-loop vsync feedback engaged
  (lead_time=...)")`) is missing — feedback loop didn't survive
  the move into `arm_app.c`. Re-verify Phase 3's vsync feedback
  port.

---

### Phase 9 — Delete legacy code (split into 9a / 9b / 9c)

The original "delete everything in one /implement session"
scope (~30K LOC across 11+ files plus CMakeLists.txt and docs
edits, plus a full re-run of Phase 8's verification matrix) is too
large for a single agent session. Split into three sub-phases,
each with its own `/implement` session and its own verification
re-run.

---

### Phase 9a — Delete legacy SDL app + renderer

#### Why it matters

Removes the two largest legacy files (`sdl_app.c` 11,734 LOC,
`sdl_game_renderer.c` 13,722 LOC) — the bulk of the rip's
LOC reduction. Cleanly verifiable in isolation because both files
are now dead under the Phase-8 pivot.

#### Files to read before implementing

- The diff produced by Phase 8 — confirm zero
  `#include "port/sdl/sdl_game_renderer.h"` remain in any source
  file outside the files this sub-phase deletes.
- `git log --oneline mister..giblet-rip` — sanity-check the
  Phase-8 pivot is in place.

#### Files to delete

- `src/port/sdl/sdl_app.c`
- `src/port/sdl/sdl_game_renderer.c`
- `include/port/sdl/sdl_game_renderer.h`

#### Files to modify

- Any remaining `port/sdl/*` file that still references
  `SDLGameRenderer_*` symbols (mostly perf-capture getters).
  Either delete the call sites (if dead) or stub them. Find with
  `grep -rln 'SDLGameRenderer_' src/`.

#### Success criteria

- All three builds (Mac flag-OFF, Mac flag-ON, MiSTer cross via
  `MISTER_BUILD_CONTAINER=3s-mister-giblet-build tools/mister/build-game.sh --flavor telemetry`) green.
- `nm 3s-arm | grep SDLGameRenderer_` empty.
- Mac runs and plays a round of training mode.
- MiSTer first-light + one netplay smoke session still pass.

#### Dependencies

Phase 8.

#### What NOT to do

- Do **not** touch `fbdev_presenter` / `software_frame_*` /
  `scanline_renderer` / `sdl_message_renderer` / `sdl_debug_text`
  yet — those go in 9b.
- Do **not** edit `CMakeLists.txt` gates yet (9c).
- Do **not** edit `docs/building.md` yet (9c).

#### Fallback / diagnose

- Linker error: `grep -rln 'SDLGameRenderer_' src/` finds
  leftover callers. Stub or delete the call site.

---

### Phase 9b — Delete fbdev_presenter + overlay shims + frame helpers

#### Why it matters

Removes the second tier of legacy files: fbdev presenter,
software-frame helpers, scanline renderer, and the SDL-coupled
overlay shims that the Phase 6 work superseded.

#### Files to read before implementing

- The Phase 9a diff — confirm sdl_app.c is gone.
- `grep -rln 'FBDevPresenter_\|SDLMessageRenderer_\|SDLDebugText_\|ScanlineRenderer_\|software_frame_' src/`
  — confirm zero callers remain outside the files this sub-phase deletes.

#### Files to delete

- `src/port/sdl/fbdev_presenter.c`
- `src/port/sdl/fbdev_presenter.h`
- `src/port/sdl/software_frame_parity.c` (only if no caller
  remains after Phase 9a)
- `src/port/sdl/software_frame_non_integer.c` (same)
- `src/port/sdl/scanline_renderer.{c,h}` (per Open Unknown #3 —
  giblet's `sdl_app.c` does NOT call ScanlineRenderer; drop
  unless user opted to keep)
- `src/port/sdl/sdl_message_renderer.c`
- `src/port/sdl/sdl_debug_text.c`
- `include/port/sdl/sdl_message_renderer.h`
- `include/port/sdl/sdl_debug_text.h`

#### Files to modify

- Any caller of the above symbols that didn't get migrated in
  Phase 6 — find with the grep above before deleting.

#### Success criteria

- All three builds green.
- `nm 3s-arm | grep -E 'FBDev|SDLMessage|SDLDebug|ScanlineRenderer|software_frame'`
  empty.
- Re-run Mac training-mode round + MiSTer first-light + one
  netplay smoke pass.

#### Dependencies

Phase 9a.

#### What NOT to do

- Do **not** delete `src/port/sdl/native_video_writer.{c,h}` —
  the new ArmDisplay backend depends on it.
- Do **not** delete `src/port/sdl/netplay_screen.{c,h}` or
  `src/port/sdl/netstats_renderer.{c,h}` — those were rewritten
  in Phase 7.
- Do **not** delete `src/port/sdl/sdl_pad.c` (input backend; out
  of scope).
- Do **not** edit `CMakeLists.txt` gates yet (9c).

#### Fallback / diagnose

- Runtime crash on MiSTer: most likely an indirect dependency of
  `software_frame_parity.c` / `software_frame_non_integer.c` that
  wasn't in `sdl_app.c`. Inspect the call graph before deleting.

---

### Phase 9c — CMake and docs cleanup

#### Why it matters

Removes the build-system gates that no longer have a "false" arm,
simplifies `renderer.c` to a single dispatch branch, and updates
docs to match the post-rip reality.

#### Files to read before implementing

- The Phase 9b diff — confirm legacy files are gone.
- `git grep THIRDSARM_USE_GIBLET_RENDERER` — should hit only the
  files this sub-phase edits.
- `git grep CRS_USE_GIBLET_APP` — same.

#### Files to modify

- `src/rendering/renderer.c`: remove every
  `#if defined(THIRDSARM_USE_GIBLET_RENDERER)` /
  `#elif defined(...)` / `#else` chain. Each dispatch becomes a
  single `SoftwareRenderer_*` call (assuming PSP / 3DS branches
  are already verified dead — if not, leave those gates intact
  and delete only the SDLGameRenderer arm of each `if`/`else`).
  Result: `renderer.c` shrinks from ~250 LOC to ~80 LOC.
- `CMakeLists.txt`: remove the
  `if(THIRDSARM_USE_GIBLET_RENDERER)` block at lines 147-157
  entirely. Drop the `option(THIRDSARM_USE_GIBLET_RENDERER ...)`
  declaration. Drop `CRS_USE_GIBLET_APP` if it was kept as an
  option through Phase 8. Always define
  `CRS_VIDEO_DRIVER_SOFTWARE=1` and
  `CRS_SW_INDEXED8_KERNEL_DEFAULT='b'`. Define
  `CRS_SW_CANVAS_16BPP=1` only under `if(PORT_MISTER)`.
- `docs/building.md`: remove the "Giblet PR #243 software
  renderer (experimental)" section at lines 108-145 — the flag is
  gone. Replace with a short note pointing at the new build-target
  table (Phase 10) or, if Phase 10 has not landed, write a minimal
  one-paragraph "default build is now the giblet path" note.

#### Success criteria

- Default builds (no extra cmake args) work for Mac and MiSTer
  cross via the canonical wrapper invocation.
- `wc -l` of the source tree shows ~25K LOC reduction across
  9a + 9b + 9c (`git diff --shortstat mister..giblet-rip`).
- `git grep THIRDSARM_USE_GIBLET_RENDERER` returns nothing.
- `git grep CRS_USE_GIBLET_APP` returns nothing (option dropped).
- Re-run all of Phase 8's success criteria (Mac + MiSTer
  first-light + netplay smoke + perf within ±0.5 ms of pre-rip
  baseline).

#### Dependencies

Phase 9b.

#### What NOT to do

- Do **not** touch the giblet renderer source files
  (`src/platform/video/software/*`) — those are now the only
  renderer.
- Do **not** rename `PORT_MISTER` to `CRS_MISTER` (that's the
  Phase 10 trade-off, deferred).

#### Fallback / diagnose

- Build fails because some `#if defined(THIRDSARM_USE_GIBLET_RENDERER)`
  arm referenced legacy SDLGameRenderer code. Re-grep with
  `git grep -n 'THIRDSARM_USE_GIBLET_RENDERER\|SDLGameRenderer_' src/`
  to find missed sites.

---

### Phase 10 — Cleanup (optional)

#### Why it matters

End-of-rip housekeeping. Optional in the sense that the project is
shippable after Phase 9c; this phase is quality-of-life.

#### Files to read before implementing

- `pr-243:CMakeLists.txt:13-23` (CRS_RPI_DRM / CRS_MISTER macros)
- Current `CMakeLists.txt` post-Phase-9

#### Files to create / modify

Optionally:

- Adopt giblet's `CRS_MISTER` macro alongside (or in place of)
  `PORT_MISTER`. Trade-off: one less duplicate macro, but
  every existing `#if defined(PORT_MISTER)` site needs editing.
  Probably **not worth it** — keep `PORT_MISTER`.
- Add a documented build-target table to `docs/building.md`:
  - `(default, Apple)` → Mac desktop, SDL app driver, software
    renderer with ARGB8888 canvas.
  - `(default, Linux non-ARM)` → Linux desktop, SDL app driver,
    software renderer with ARGB8888 canvas.
  - `(PORT_MISTER=ON, ARM Linux)` → MiSTer, ArmApp driver with
    NativeVideoWriter ArmDisplay backend, software renderer with
    RGB565 canvas.
  - `(future, ARM Linux Pi w/ libdrm)` → ArmApp driver with DRM
    ArmDisplay backend, software renderer with ARGB8888 canvas.
  - `(future, ARM Linux generic w/ fbdev)` → ArmApp driver with
    fbdev ArmDisplay backend, software renderer with ARGB8888
    canvas.

#### Success criteria

- `docs/building.md` has the table.
- All builds still green.

#### Dependencies

Phase 9c.

#### What NOT to do

- Do **not** rename `PORT_MISTER` mid-rip.
- Do **not** add new build targets — the table documents
  existing supported configurations only.

#### Fallback / diagnose

N/A — purely additive.

## Risk register

### R1: Netplay state machine mid-transition

- **What could break**: If a Phase-7 build saves netplay state in
  one format and a Phase-9 build loads it in another (because
  the perf-capture snapshots in savedata went away), live netplay
  between the two builds will desync immediately.
- **How it manifests**: Both peers brick on first input, or one
  peer rewinds repeatedly trying to resync.
- **Mitigation**: In Phase 7, audit the netplay save/load
  callback for any `SDLGameRenderer_GetPerfCapture*` data. Strip
  those from the savedata schema in Phase 7, not Phase 9a, so the
  format stabilizes before the perf-capture functions are
  deleted.
- **Fallback**: Bump a savedata version field (if one exists) and
  reject cross-version sessions with a user-visible "version
  mismatch" message.

### R2: MiSTer FPGA timing — losing the vsync feedback loop

- **What could break**: The closed-loop pacer in
  `sdl_app.c:9569-9615` adapts the frame deadline to FPGA-observed
  vsync. If Phase 3's port into `arm_app.c` gets the timestamp
  conversion or wrap handling wrong, the pacer reverts to
  open-loop and frames tear or run at the wrong rate.
- **How it manifests**: Visible tearing, scrolling backgrounds
  jitter, audio drift over a 30-minute play session.
- **Mitigation**: Phase 3 keeps the feedback logic structurally
  identical — copy verbatim into `arm_app.c`. Verify the
  "closed-loop vsync feedback engaged" backend_logf line appears
  on the ARM startup log.
- **Fallback**: If the loop fails to engage and the open-loop
  pacer is acceptable for ship, document it as a regression. Else
  revert Phase 3 and investigate.

### R3: Mac perf regression from ARGB canvas vs RGB565

- **What could break**: Today Mac flag-ON uses an RGB565 canvas
  + an SDL streaming texture in RGB565. Post-rip the canvas is
  ARGB8888 (per CRS_SW_CANVAS_16BPP gating change) and the giblet
  software renderer's hot path runs ARGB8888 instead of 565.
- **How it manifests**: ~10-20% perf loss on the software
  rasteriser hot path on Mac. Mac is not perf-constrained, so
  this is likely cosmetic.
- **Mitigation**: Confirm the ARGB8888 path hits its scalar /
  NEON kernels — `sw_blit.c` has both. Apple Silicon (M-series)
  CAN compile the NEON path: `sw_blit_neon.c` is gated
  `#if CRS_VIDEO_DRIVER_SOFTWARE && defined(__ARM_NEON) && defined(__aarch64__) && !defined(CRS_SW_CANVAS_16BPP)`,
  which all hold on Mac post-rip (PORT_MISTER off ⇒
  `CRS_SW_CANVAS_16BPP` undefined ⇒ NEON path active on Apple
  Silicon, scalar fallback on Intel Mac). The 16-bit-canvas NEON
  path (`sw_blit_neon_armv7.c`) is gated `&& !defined(__aarch64__)`
  and stays out on Mac. Net: Apple Silicon gets the same NEON
  acceleration the MiSTer build gets, just with a wider canvas
  format.
- **Fallback**: Acceptable; Mac is not a perf-critical target.
  If it does become one, the SWCanvasPixel typedef can go to a
  per-target choice instead of strictly PORT_MISTER-gated.

### R4: Build container regressions

- **What could break**: `tools/mister/build-game.sh` invokes a
  Docker container that runs `cmake` with a specific argument set.
  The CMake reorganization in Phase 9c (drop
  `THIRDSARM_USE_GIBLET_RENDERER`, add per-platform gates) might
  trip the container's argument parsing.
- **How it manifests**: Container build fails with cmake errors,
  or worse, succeeds with the wrong configuration.
- **Mitigation**: Run the canonical
  `MISTER_BUILD_CONTAINER=3s-mister-giblet-build tools/mister/build-game.sh --flavor telemetry`
  at the end of every phase, not just Phases 1, 8, 9c.
  The wrapper should not need editing — confirm explicitly in
  Phase 9c by `grep`ing the wrapper for any `THIRDSARM_USE_GIBLET_RENDERER`
  references first.
- **Fallback**: If the wrapper does need editing, scope-creep it
  into Phase 9c — `tools/mister/build-game.sh` edits stay in the
  rip branch. Per the user's "wrapper" terminology in
  `~/.claude/projects/-Users-sb-Developer-3sx-mister/memory/feedback-build-terminology.md`,
  "wrapper" means `build-hps.sh` (the FPGA wrapper-core build),
  **not** `build-game.sh` — the latter is the game build script
  and is explicitly in scope here.

### R5: Font / overlay scaling in 384x224 canvas vs target resolution

- **What could break**: 8x8 glyphs at scale 1 are tiny relative
  to a 1080p TV when the FPGA upscales the 384x224 canvas. At
  scale 3, glyphs occupy a noticeable strip of the frame.
- **How it manifests**: Overlay text either too small to read or
  too big to ignore.
- **Mitigation**: Default to scale 2 (16x16 effective) for the
  FPS overlay and SDLMessageRenderer, scale 1 for SDLDebugText
  (which is debug-mode only). Make it config-driven through a new
  `text-overlay-scale` key with a sensible default.
- **Fallback**: If 8x8 reads poorly, swap the embedded font for
  a 6x10 or 8x12 candidate without touching the call sites
  (purely a `font8x8_basic.h` swap + `TextOverlay_LineHeight`
  update).

### R6: Hot-reload of config after sdl_app.c gone

- **What could break**: All `Config_Get*` calls today live in
  `src/port/config/` and are called from `src/port/sdl/sdl_app.c`.
  After the rip, the same `Config_Get*` calls need to live in
  giblet's `arm_app.c` / `sdl_app.c` instead.
- **How it manifests**: Per-frame config changes (e.g. mid-game
  `show-fps off → debug`) stop taking effect.
- **Mitigation**: The migration is mostly mechanical — every
  `Config_Get*` site in `sdl_app.c` either survives in a Phase 5/6
  overlay module or moves into giblet's app loop. Audit the call
  list at the start of Phase 8 with
  `grep -n 'Config_Get' src/port/sdl/sdl_app.c | wc -l` and
  confirm the same count exists across the rip-branch
  destination files at Phase 9a.
- **Fallback**: If a config key gets dropped silently, the user
  notices at runtime. The fallback is to grep the dropped
  `CFG_KEY_*` constant from `port/config/keys.h` (or wherever the
  enum lives) and verify there's at least one remaining caller.

### R7: DRM device permissions on non-MiSTer ARM Linux

- **What could break**: When the giblet `arm_display_drm.c`
  backend is used on a non-MiSTer ARM Linux host (future RPi
  target), the process needs read/write access to `/dev/dri/card0`
  (or equivalent). On many distros that requires the user to be in
  the `video` (or `render`) group, or proper udev rules / seat
  management. PORT_MISTER bypasses this entirely (NativeVideoWriter
  goes directly through `/dev/mem`), so the permission story
  surfaces only on the future-RPi path.
- **How it manifests**: `arm_display_drm_init` returns false; the
  fallback fbdev backend drives `/dev/fb0` (also permission-gated)
  and may also fail; binary exits with "no display backend".
- **Mitigation**: Document the required permissions in
  `docs/building.md` Phase 10 build-target table (which is the
  only place the future-RPi target is named). Out of scope for the
  MiSTer-only Phase 8/9 verification matrix.
- **Fallback**: Run as root on the bring-up box, or fix the
  group/udev rules. Not a rip-blocker.

### R8: SDL audio handoff order between sdl_app and arm_app

- **What could break**: Both giblet's `sdl_app.c` and `arm_app.c`
  call `ADX_ProcessTracks` from their `end_frame`. Init/teardown
  ordering relative to `Main_StepFrame`, signal handlers, and
  SDL_Quit may differ from our current `src/port/sdl/sdl_app.c`.
  If the audio device init/teardown order shifts, audio may be
  silent on first launch or may produce a click/pop on shutdown.
- **How it manifests**: Either no audio at all (init never ran in
  the new order), or audible click on app exit (SDL_Quit before
  `ADX_Shutdown`).
- **Mitigation**: Phase 8 verification adds an explicit audio
  smoke step: launch, hear character voice, exit cleanly. Compare
  against the legacy build's behaviour on the same hardware.
- **Fallback**: If the order is wrong, fix in giblet's
  `arm_app.c::main` / `sdl_app.c::main` rather than working around
  in `port/sound/adx.c`.

### R9: AFS resource copy phase semantics

- **What could break**: Giblet's `arm_app.c` has an
  `ARM_APP_PHASE_COPYING_RESOURCES` enum value (verified at
  `pr-243:src/platform/app/arm/arm_app.c:26`). Our current tree
  does not wire this phase to actual deploy semantics — our
  MiSTer launch assumes `/media/fat/games/3s-arm/SF33RD.AFS` is
  already present. If giblet's app loop expects to perform a
  first-launch resource copy (e.g. from a packaged path into
  `/media/fat/games/3s-arm`), Phase 8's first-light may either
  block waiting for a resource that isn't where it expects, or
  silently rewrite a path the user owns.
- **How it manifests**: First-light hangs at "copying resources"
  with no progress, or silently overwrites SF33RD.AFS with a
  packaged version.
- **Mitigation**: Phase 8 must read giblet's `arm_app.c` body
  carefully to determine whether `ARM_APP_PHASE_COPYING_RESOURCES`
  is wired to anything destructive. If it copies from a packaged
  resource path, document the new UX in `docs/mister-runbook.md`
  before first-light deploy.
- **Fallback**: Skip the copy phase under PORT_MISTER (gate it
  off in `arm_app.c`) if the wrapper deploy already places
  SF33RD.AFS in the canonical location.

### R10: Cross-build container worktree binding

- **What could break**: Per session memory
  (`feedback-quartus-build-env`-style nuance: the giblet build
  container `3s-mister-giblet-build` is bound to this worktree
  path). If Phase 1 cuts a fresh worktree off `mister` rather
  than reusing the current `lobby-mvp` worktree, the existing
  container will not see the new files until rebuilt or rebound
  via `--volume`.
- **How it manifests**: `tools/mister/build-game.sh` builds an
  outdated tree; the resulting binary is missing the rip work.
- **Mitigation**: Phase 1 success criteria explicitly verifies
  the cross build invocation runs against the new worktree (e.g.
  by adding a known marker file or grep target into the build
  output). If the container is bound to the wrong path, rebuild
  the container or re-bind via the docker mount setup before
  proceeding.
- **Fallback**: Re-create the container with the new worktree
  path. Documented in `docs/mister-runbook.md`.

## Pause points

The tree is in a usable, shippable state at these milestones:

- **After Phase 1**: Same as `mister` baseline + 6 cherry-picked
  giblet activation commits + a planning doc. Identical to
  `giblet-stuff`'s shippable state. Pause is safe.
- **After Phase 4**: Giblet's app/display tree imported but not
  active. New text-overlay module compiled but not used. Default
  builds unchanged. Pause is safe — the tree compiles, all tests
  pass, and the binary is functionally identical to Phase 1.
- **After Phase 8**: Giblet's app drivers active, legacy
  `port/sdl/*` code still present but dormant. Default builds
  produce the new path. Pause is safe iff first-light and one
  netplay smoke pass have completed (per the prompt's "no
  premature release" feedback rule).
- **After Phase 9a**: `sdl_app.c` + `sdl_game_renderer.c` gone
  (~25K LOC). Pause is safe — all three builds green, binary
  runs, but `port/sdl/fbdev_presenter.*` and the overlay shims
  are still on disk (compiled but dead).
- **After Phase 9b**: All legacy `port/sdl/*` source files gone
  (excluding `native_video_writer`, `netplay_screen`,
  `netstats_renderer`, `sdl_pad`). Pause is safe.
- **After Phase 9c**: Rip complete — CMake gates simplified,
  `THIRDSARM_USE_GIBLET_RENDERER` and `CRS_USE_GIBLET_APP` gone,
  `docs/building.md` updated. Pause is safe — this is the
  intended end state.

Phases 2, 3, 5, 6, 7, 10 are intermediate; pause is technically
safe (the tree compiles and the binary still runs) but the build
matrix is not in a clean state.

## What this plan does NOT cover

- **Future migration to giblet's other video drivers** —
  `sdl_generic_renderer`, `sdl_gpu_renderer`, `opengl_renderer`,
  the shaders, and the `sdl_software_renderer.c` SDL-side present
  helper. Stays out of scope. The rip's software renderer is the
  only video driver in the tree post-Phase 9c; everything else
  (the giblet GPU drivers, shaders, etc.) stays unimported and
  unbuilt.
- **Removal of vendored SDLGameRenderer history from git** — the
  ~13K-line file disappears in Phase 9a but its history remains.
  No `git filter-branch` work.
- **Pre-MVP lobby work** — explicitly out of scope per the prompt.
- **Build container / `tools/mister/build-game.sh` redesign** —
  out of scope. The script may need a 1-2 line edit if it
  currently passes `THIRDSARM_USE_GIBLET_RENDERER` explicitly,
  but no architectural changes.
- **Deploy paths** — binary lands at `/media/fat/games/3s-arm/bin/3s-arm`
  unchanged.
- **PSP / 3DS branches in `src/rendering/renderer.c`** — those
  `#elif defined(TARGET_PSP)` / `#elif defined(TARGET_3DS)` arms
  are kept untouched in Phase 9c unless code-reading proves them
  dead. They are tiny and harmless.
- **Optimisation of the canvas-format conversion in the
  NativeVideoWriter ArmDisplay backend** — the initial backend
  always converts ARGB→RGB565 in a scratch buffer. The
  zero-copy version (giblet canvas already in RGB565,
  `SoftwareRenderer_GetCanvas` returns the right pixel type)
  requires extending giblet's `ArmDisplay_Present` signature
  and is a follow-up.
- **Adopting giblet's `CRS_MISTER` macro** — Phase 10 mentions it
  as "probably not worth it". Out of scope unless the user
  decides otherwise.

## Open unknowns (require user input before /implement)

1. **Font choice**: planner recommends dhepper/font8x8 (CC0,
   public-domain 8x8 ASCII) but several alternatives exist
   (Cozette, Spleen, IBM CGA/VGA, hand-roll). User should pick
   before Phase 4 starts. If undecided, planner will hand-roll a
   minimal A-Z + 0-9 + punct font in Phase 4 and revisit later.
2. **CRS_USE_GIBLET_APP option vs hard-flip**: planner
   recommends introducing the option in Phase 2, defaulting OFF,
   flipping to ON in Phase 8, and dropping it in Phase 9c. User
   may prefer a hard-flip in Phase 2 (no intermediate option) at
   the cost of a longer "broken default build" window. Default
   plan as written: keep the option through Phase 8.
3. **scanline_renderer.{c,h}** — **PRODUCT DECISION, not a
   research question**. Verified by reading
   `pr-243:src/platform/app/sdl/sdl_app.c`: it does NOT reference
   `ScanlineRenderer_*` at all. Our current code calls it at
   `src/port/sdl/sdl_app.c:10083` (init), `:10131` (destroy), and
   `:10704` (render). With the legacy sdl_app.c rip, the scanline
   overlay disappears unless the user explicitly chooses to keep
   it. Default plan as written: drop with sdl_app.c rip in Phase 9b.
   **User must explicitly say "keep scanline overlay" if they want
   Phase 6/8 to port it onto the new text/canvas surface.**
4. **Phase 6 SDLMessageRenderer SDL-texture callers**: if any
   non-text caller exists (PNG/BMP overlays through
   `SDLMessageRenderer_CreateTexture` / `_DrawTexture`), Phase 6
   may need a sub-phase for bitmap-overlay rewriting. Probable
   but not yet verified.
