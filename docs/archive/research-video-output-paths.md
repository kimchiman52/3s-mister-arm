# Video Output Path Research

Research notes from investigating nearest-neighbor scaling on HDMI and the
interaction between FPGA native video, the MiSTer FB scaler, and direct video.

**Date:** 2026-03-31
**Status:** Deferred — needs more design work before implementation.

---

## Background

The MiSTer 3SX port has three distinct video output paths. Each uses different
hardware and software mechanisms:

| Path | Hardware | ARM writes to | FPGA reads from | Output connector |
|------|----------|---------------|-----------------|------------------|
| Native video | Analog I/O board | DDR3 (via `/dev/mem` mmap) | DDR3 native video reader (`status[9]`) | VGA/SCART/component on I/O board |
| FB scaler | HDMI | Linux framebuffer (`/dev/fb0`) | HPS FB overlay (`video_fb_enable`) + ascal | HDMI connector |
| Direct video | HDMI-as-VGA | DDR3 (native video) or FB | Core VGA output routed through HDMI connector | HDMI connector (VGA timing) |

## Current behavior (as of commit 2aab3871)

`THREESX_NATIVE_VIDEO` defaults to enabled (env unset → true). This means
`NativeVideoWriter_Init()` always runs and always succeeds (it just mmaps
`/dev/mem`). When `native_video_writer_enabled` is true, the fbdev presenter
is skipped:

```c
// sdl_app.c:9899
if (fbdev_presenter_enabled && !native_video_writer_enabled) {
```

The wrapper's FPGA setup is also mutually exclusive:

```c
if (g_native_video_mode) {
    // Enable FPGA native video reader, do NOT enable FB scaler
    user_io_status_set("[9]", 1);
} else {
    // Enable FB scaler, do NOT enable native video reader
    set_vga_fb(1);
    video_fb_enable(1);
}
```

### Result per configuration

| Setup | `io_type` | `g_native_video_mode` | What happens | Output |
|-------|-----------|----------------------|--------------|--------|
| Analog only | 0 | true | Native video to DDR3 | Correct 15kHz on analog |
| HDMI only | 1 | true | Native video to DDR3, fbdev skipped | **Broken** — HDMI gets nothing, DDR3 writes go nowhere |
| Both connected | 0 | true | Native video to DDR3, fbdev skipped | Analog works, **HDMI gets nothing** |
| Direct video | 1 | true | Native video to DDR3 | **Works** — core VGA output (fed by native video reader) routed through HDMI connector at 15kHz CRT timing |

**Key finding:** HDMI-only users get no video output because `NativeVideoWriter_Init()`
always succeeds (doesn't check for analog hardware) and then blocks the fbdev
presenter. The OSD scale mode option (nearest/native) has no effect.

**Key finding:** Direct video works despite `io_type != 0` because the HDMI
connector in direct video mode outputs the core's raw VGA signals, which ARE
driven by the native video reader. No FB scaler is needed.

## Why dual output (native + fbdev simultaneously) doesn't work

We attempted to run both paths simultaneously so analog users could also get
HDMI output. Two blockers:

### 1. `set_vga_fb(1)` overrides native video on VGA pins

In `sys_top.v`, `vgas_en = vga_fb | vga_scaler`. When `vgas_en` is high, the
VGA pin output mux selects the HDMI scaler output (`vgas_o`) instead of the
core's direct VGA output (`vga_o`). The VGA clock also switches from `clk_vid`
to `hdmi_clk_out`. This means calling `set_vga_fb(1)` destroys the native
video reader's pixel-perfect 15kHz output on the analog pins.

**Fix:** Skip `set_vga_fb(1)` when native video is active. `video_fb_enable(1)`
alone is safe — it only configures the HPS FB overlay for HDMI, and only
internally calls `set_vga_fb()` when `cfg.direct_video` is set.

### 2. Framebuffer dimensions are wrong for HDMI

When analog output is active, `video_fb_config()` sizes the Linux framebuffer
for the analog video mode (384x240 for S-Video/composite). The fbdev presenter
reads these dimensions and computes letterbox rects within them. Since the FB
is already game-resolution, all scale modes produce nearly the same output.
The FPGA's ascal then stretches this tiny FB to fill the HDMI screen.

For HDMI nearest scaling to work, the FB needs to be sized for the HDMI output
resolution (e.g., 960x540 or 1920x1080). But `video_fb_config()` only knows
about one video mode at a time (`v_cur`), and that's set for analog.

**This is the harder problem.** Fixing it requires either:
- Teaching `video_fb_config()` to size the FB for HDMI independently of analog
- Having the fbdev presenter handle the scaling itself without relying on
  framework FB dimensions
- Running the FB at HDMI resolution and having native video bypass it entirely

## Proposed fix: io_type + direct_video gating

The simplest correct fix that doesn't break any existing configuration:

```c
// In set_runtime_environment():
const char *nv_env = getenv("THREESX_NATIVE_VIDEO");
const bool nv_requested = !nv_env || strcmp(nv_env, "0") != 0;
if (nv_requested && startup_scale_mode.io_type != 0 && !startup_scale_mode.direct_video)
{
    setenv("THREESX_NATIVE_VIDEO", "0", 1);
}
```

This disables native video ONLY for HDMI-only users (no analog board, not
direct video). All other configurations remain unchanged:

| Setup | Behavior after fix |
|-------|--------------------|
| Analog only | Unchanged — native video |
| HDMI only | **Fixed** — FB scaler, nearest scaling works |
| Both connected | Unchanged — native video only (no HDMI output) |
| Direct video | Unchanged — native video through HDMI connector |

The "both connected" case remains unsolved — HDMI users with an analog board
won't get HDMI output. Fixing that requires the dual-output work described
above.

## `fpga_get_io_type()` details

- Reads GPI bit 28 from FPGA hardware (`fpga_io.cpp:594-598`)
- Returns 0 for analog I/O board, 1 for digital (no analog board)
- Hardware signal — always fresh, no caching

## Pre-existing build issue: TestRunner stubs

`test_runner.c` wraps all function bodies in `#if DEBUG`, but `args.c` and
`main.c` reference `TestRunner_IsSupportedPhaseName`, `TestRunner_IsPhaseActive`,
and `TestRunner_GetPhaseName` inside `#if ENABLE_PERF_TELEMETRY` blocks. In
Release+telemetry builds (the MiSTer default), this causes linker errors.

Fixed by adding `#else` stubs in `test_runner.c` that provide no-op
implementations for non-DEBUG builds. This fix is independent of the video
output work.

## Files involved

- `vendor/Main_MiSTer/threesx_wrapper.cpp` — `resolve_startup_scale_mode()`, `set_runtime_environment()`, FPGA setup block
- `src/port/sdl/sdl_app.c` — `scale_mode_uses_native_render_path()`, present loop (`SDLApp_EndFrame`)
- `src/port/sdl/fbdev_presenter.c` — `FBDevPresenter_PresentSurface()`, mapped nearest scaling
- `src/port/sdl/native_video_writer.c` — `NativeVideoWriter_Init()`, DDR3 frame writes
- `vendor/Main_MiSTer/video.cpp` — `video_fb_config()`, `video_fb_enable()`, FB dimensions
- `vendor/Main_MiSTer/user_io.cpp` — `set_vga_fb()`, VGA FB flag
- FPGA: `sys_top.v` lines 1483, 1548-1556 — VGA pin mux, `vgas_en` signal
- FPGA: `menu.sv` lines 650-655 — native video reader VGA output
