# MiSTer Native Analog CRT S-Video Diagnostics

When to load:
- Load this when revisiting scaler-off analog CRT output for 3S-ARM, especially `svideo`/`cvbs` color loss, `crt-4x3`, or native analog wrapper/video-path cleanup.

Current status:
- **S-Video color is FIXED as of the fix in `video.cpp` (after commit `bc77d52a`).**
- Width/fill is correct on scaler-off native analog S-Video.
- Horizontal scaling still shows uneven/wavy distribution on fine detail (384→640 non-integer expansion) — separate open problem.

## Confirmed Findings

- `crt-4x3` startup selection is live on real scaler-off native analog launches.
- Scaler-off `svideo`/`cvbs` no longer goes through the bogus `1280x720` framebuffer path; real launches now expose `FBDEV: active (640x240 ...)`.
- `video_mode_adjust()` does run on the real OSD launch path.
- `set_yc_mode()` does run on the real OSD launch path.
- The YC packet is definitely sent to FPGA on live 3S-ARM S-Video launches.
- After removing stale remote debug overrides, Menu and 3S-ARM send the same live YC packet on S-Video:
  - `clock_source=core`
  - `phase_inc=196786767482`
  - `colorburst_start=20`
  - `colorburst_end=70`
  - `yc_config=1`
  - `subcarrier_enable=0`
- The first real Menu-vs-3S-ARM divergence is now the wrapper/HPS framebuffer handoff:
  - Menu stays at `vga_fb=0` and `fb_en=0`
  - 3S-ARM does `wrapper_prelaunch_fb`, `set_vga_fb(1)`, `user_io_send_buttons map=0x1208`, and `video_fb_enable(1)`
- A real hardware test that skipped `CONF_VGA_FB` on native analog produced static, not restored color.
- Therefore `CONF_VGA_FB` is required for routing the HPS framebuffer on this path, but is not itself the color fix.
- The strongest remaining suspect is the HPS framebuffer format/programming path in `video_fb_enable()` / `UIO_SET_FBUF`, especially the hardcoded `FB_FMT_RxB | FB_FMT_8888` setup and matching `MiSTer_fb` mode write.
- A real hardware test that forced `8888` with `RxB` disabled still stayed grayscale.
- That makes a simple 32-bit channel-order swap less likely to be the whole issue; the next most useful format candidates are the packed 16-bit paths (`565`, then `1555`).
- A real hardware test that forced packed `565` did not restore color and broke visible video more severely: the game kept running with audio, but the screen fell through to the Linux login console.
- That means packed 16-bit framebuffer formats can be worse than grayscale on this path; treat them as higher-risk experiments than 32-bit channel-order tweaks.
- On a live grayscale 3S-ARM run, `/dev/fb0` itself still contained overwhelmingly non-gray pixels (`149720` non-gray pixels out of `153600` sampled).
- That means the Linux framebuffer is receiving real color data even while the CRT shows grayscale; the remaining bug is downstream of the `/dev/fb0` write and likely in the HPS-framebuffer-to-analog-video path rather than in SDL/fbdev rasterization.
- A real software bug in the native-analog debug/refactor path was fixed: we had been writing `rb=1` to the Linux `MiSTer_fb` mode while accidentally omitting the FPGA-side `RxB` bit from `spi_format`.
- After fixing that mismatch, the live 3S-ARM path now reports `spi_format=0x16` and `fb_fmt=214` (`0xD6`), which is the expected enabled `8888 + RxB` form.
- Color still did not return after that fix, so the `RxB` mismatch was real but not the root cause.
- Changing the final YC packet fields below did not restore color:
  - output-clock-based burst window
  - output-clock-based `PHASE_INC`
  - `yc_config = 3`
  - `subcarrier_enable = 1`
- The remaining waviness lines up with the current `384x224 -> 640x240` non-integer horizontal expansion on the native TV raster.
- The attempted CRT-filter workaround in `93f24235` added extra presenter work and disabled the normal mapped-row fast paths, which is why FPS dropped noticeably.

## Root Cause and Fix (Resolved)

Three bugs combined to cause grayscale. All three were required fixes:

### Bug 1 — Wrong DAC data source (RTL)
`vgas_en = vga_fb | vga_scaler`. When `vga_fb=1` (3S-ARM framebuffer path), the DAC was fed from the ascaler/framebuffer pipeline (`vgas_o`) which had no YC encoder. The original `yc_out` instance operated on the FPGA core video pipeline and its output was bypassed entirely.

**Fix (`bc77d52a`):** Added `yc_out_fb` instance in `sys_top.v` on `clk_hdmi` taking `hdmi_data_osd` input, gated by `vga_fb_yc_en = vga_fb & ~vga_scaler & yc_en`. DAC mux updated to prioritize `yc_out_fb` output when `vga_fb_yc_en=1`.

### Bug 2 — Wrong pixel clock source for PHASE_INC (HPS)
Before the fix, `set_yc_mode()` always computed `PHASE_INC` from the FPGA core pixel clock (`ctime/ptime`). In 3S-ARM mode before the game starts, the FPGA reports `ptime=0` → `core_CLK_VIDEO=NaN` → garbage PHASE_INC.

**Fix (`bc77d52a`):** Added `fb_native_analog_auto` condition in `video.cpp`: when `vga_fb=1` and native TV mode is active and output clock is available, use `v_cur.Fpix` (output pixel clock, 12.587 MHz) as `CLK_VIDEO`.

### Bug 3 — Wrong PAL/NTSC detection (HPS)
Even with the correct clock, `pal` was derived from `current_video_info.vtime`. When `video_refresh_yc_mode()` is called at `thirdsarm_wrapper.cpp:1075` (before the game starts), `vtime=0` → `fps=0 < 55 → pal=1` → `CLK_REF=4.43 MHz` (PAL) → `PHASE_INC=387,289,675,374` → 4.43 MHz subcarrier on NTSC TV → colorburst PLL cannot lock → grayscale.

**Fix (`video.cpp`):** After `fb_native_analog_auto` is determined, if `vtime==0`, derive `pal` from `v_cur` output timing (htotal/vtotal/Fpix). For NTSC 15K: `output_fps = 12.587e6 / (800×262) ≈ 60.05 Hz > 55 → pal=0 → CLK_REF=3.579545 → PHASE_INC≈312,741,000,000 → 3.58 MHz NTSC → color confirmed working on real hardware.`

Confirmed working log values after all three fixes:
- `clock_source=output-fb-auto`
- `phase_inc≈312,741,000,000`
- `yc_config=0x1` (`pal_en=0, cvbs=0, yc_en=1`)
- Real CRT: **full color**

## Commit Trail For This Native Analog Track

- `31e34f5b` `add crt-4x3 startup mode`
- `34b67969` `use TV modes for scaler-off svideo/cvbs`
- `742e7b38` `native-analog: widen crt-4x3 and retime YC`
- `93f24235` `native-analog: preserve YC clock and filter CRT TV scaling` (preserved experiment, not kept on mainline)
- `076852fb` `add 3S-ARM native analog YC fallback`
- `beaa1d9e` `instrument YC packet logging`

These commits are the primary candidates to review/revert if the native-analog branch is unwound later.

## Current Branch Cleanup State

The branch now keeps only the functional native-analog fixes in the shipping code path.

Any temporary packet-override config experiments such as `/media/fat/games/3s-arm/yc-debug.conf` should remain local to hardware investigation and should not become default runtime behavior.

## Real-Hardware Attempt Log

### 1. Startup mode plumbing

Change:
- Added `crt-4x3` startup mode selection for native analog auto-detect.

Result:
- Worked as intended for startup selection.
- Did not fix grayscale output by itself.

What it proved:
- The config/wrapper startup mode selection was not the main remaining color blocker.

### 2. Scaler-off TV mode selection

Change:
- Forced scaler-off `svideo`/`cvbs` onto TV-native video modes instead of inherited `1280x720`.

Result:
- Worked.
- Real launches moved to `640x240` fbdev.

What it proved:
- The old `1280x720` native-analog path was wrong.
- Native analog mode selection itself was not enough to restore color.

### 3. `crt-4x3` width/fill fixes

Change:
- Adjusted the native analog presentation so the image filled the screen correctly instead of being extremely narrow and centered.

Result:
- Width/fill improved.
- Scaling quality still looked imperfect on real hardware.
- Fine detail still showed uneven/wavy horizontal distribution.
- Color remained grayscale.

What it proved:
- Geometry and color are separate problems here.
- Filling the full native TV raster is the right general direction, but the current `384 -> 640` expansion still needs a CRT-specific resampling strategy.

### 3a. CRT filter / smoothing experiment

Change:
- `93f24235` added a narrow CRT-only filtered resample in `src/port/sdl/fbdev_presenter.c` for the native `640x240/288/480/576` TV framebuffer family while also preserving the YC-clock experiment in `video.cpp`.

Result:
- Real hardware still stayed grayscale.
- Wavy scaling may have been slightly better, but not decisively fixed.
- FPS dropped noticeably enough to reject this as the next mainline direction.

Why FPS dropped:
- The filter adds extra per-destination-pixel work inside the hottest native analog presenter path.
- It also disables the existing mapped-row/repeat-run shortcuts when the filter is active, so the renderer gives up the cheap dirty-row fast paths and falls back to heavier whole-row work.

What it proved:
- The waviness is real and tied to the native `384 -> 640` expansion.
- A brute-force per-pixel filter is too expensive to be the default fix on MiSTer CPU present.
- Do not reopen this exact filtered-presenter approach unless a future experiment can prove a much cheaper implementation with measured FPS recovery.

### 4. 3S-ARM native analog YC fallback

Change:
- Added a 3S-ARM-specific fallback borrowing the `MENU_59.8` / `MENU_50.2` phase idea.

Result:
- No color improvement.

What it proved:
- Missing per-core phase fallback alone is not the full root cause.

### 5. YC instrumentation

Change:
- Added persistent YC tracing under `/media/fat/games/3s-arm/logs/yc-debug.log`.
- Added broader trace breadcrumbs through wrapper/video/user-io paths.

Result:
- Worked.

What it proved:
- Real 3S-ARM native analog launches do reach `video_mode_adjust()` and `set_yc_mode()`.
- The problem is not “YC packet never gets sent.”

### 5a. Stale debug-config cleanup

Change:
- Removed the temporary `yc-debug-*` keys from the live `/media/fat/games/3s-arm/config`.
- Moved local debug-override loading to a dedicated `/media/fat/games/3s-arm/yc-debug.conf` path so stock Menu runs are not polluted by 3S-ARM-specific packet experiments.

Result:
- Menu color returned immediately on the same `alt_1` S-Video profile.
- 3S-ARM still stayed grayscale.

What it proved:
- The earlier grayscale Menu result was a debug-config pollution bug, not the underlying native-analog root cause.
- Clean Menu-vs-3S-ARM comparison is now possible and trustworthy.

### 6. Output-clock burst-window test

Config:
- `yc-debug-clock-source = output`

Result:
- Packet changed from core-clock-derived burst timing to output-clock-derived burst timing.
- Still grayscale.

What it proved:
- Burst window mismatch alone is not the whole problem.

### 7. Output-clock `PHASE_INC` test

Config:
- `yc-debug-clock-source = output`
- `yc-debug-phase-inc = 312683830272`

Result:
- Packet used output-clock-derived phase and burst values.
- Still grayscale.

What it proved:
- Final phase math alone is not the whole problem.

### 8. `yc_config = 3` test

Config:
- `yc-debug-clock-source = output`
- `yc-debug-phase-inc = 312683830272`
- `yc-debug-yc-config = 3`

Result:
- Packet used `yc_config = 3`.
- Still grayscale.

What it proved:
- The current `yc_config` bit choice is not the only remaining issue.

### 9. `subcarrier_enable = 1` test

Config:
- `yc-debug-clock-source = output`
- `yc-debug-phase-inc = 312683830272`
- `yc-debug-yc-config = 3`
- `yc-debug-subcarrier-enable = 1`

Result:
- Packet used `subcarrier_enable = 1`.
- Still grayscale.

What it proved:
- The main final packet knobs appear exhausted without restoring color.

### 10. Clean Menu-vs-3S-ARM packet comparison

Change:
- Re-ran the same `alt_1` S-Video profile after removing stale `yc-debug-*` config pollution.
- Compared real OSD Menu startup against a real 3S-ARM launch using the shared YC trace.

Result:
- Menu stayed in color.
- 3S-ARM stayed grayscale.
- The traced `set_yc_mode()` packet was the same between Menu and 3S-ARM.

What it proved:
- The final YC packet is no longer the differentiator.
- The remaining bug is earlier or adjacent to the HPS framebuffer handoff.

### 11. Native-analog `CONF_VGA_FB` removal test

Change:
- Patched the wrapper so native analog TV output kept the HPS framebuffer path but skipped asserting `CONF_VGA_FB` before launch.

Result:
- Real hardware showed static instead of restored color.

What it proved:
- `CONF_VGA_FB` is required to route the image on this path.
- The next likely bug is deeper in the HPS framebuffer format/programming path, not the route bit itself.

### 12. HPS framebuffer `8888` without `RxB`

Change:
- Kept the native analog HPS path and YC packet untouched.
- Added a dedicated debug override file at `/media/fat/games/3s-arm/yc-debug.conf`.
- Forced the HPS framebuffer path to use `8888` with `RxB` disabled on both the FPGA side and the Linux `MiSTer_fb` mode write.

Result:
- Real hardware still stayed grayscale.
- Live YC trace showed the override was active during the gray run, so this was not a config-loading miss.

What it proved:
- The remaining color bug is probably not just a trivial red/blue channel-order swap on the existing 32-bit framebuffer path.
- The next narrow experiments should focus on alternate packed framebuffer formats before reopening broader YC or presenter theories.

### 13. HPS framebuffer `565`

Change:
- Kept the native analog HPS path and YC packet untouched.
- Forced the HPS framebuffer path to use packed `565` on both the FPGA side and the Linux `MiSTer_fb` mode write.

Result:
- Real hardware did not restore color.
- 3S-ARM kept running with audio in the background, but the visible display dropped to the Linux login console instead of showing usable game video.

What it proved:
- The native analog path is sensitive to framebuffer format compatibility beyond simple color interpretation.
- `565` is not a safe next-step format for this path; it can break visible video more severely than the grayscale baseline.
- Any further framebuffer-format experiments should be treated as recovery-risky and should prefer the least-disruptive candidates first.

### 14. Live `/dev/fb0` color sample on a grayscale run

Change:
- Sampled `/dev/fb0` directly on a real live 3S-ARM grayscale S-Video session.
- Counted non-black and non-gray pixels from the first `640x240x4` bytes and captured a few representative pixel values.

Result:
- The framebuffer was not grayscale internally:
  - `pixels=153600`
  - `nonblack=150703`
  - `nongray=149720`
- Example live pixels included unequal RGB values such as `R=74 G=98 B=57`.

What it proved:
- The runtime and Linux framebuffer path are still generating real color.
- The grayscale failure is now most likely downstream of `/dev/fb0`, in the HPS-framebuffer-to-analog-video path.
- This strongly lowers the value of more blind framebuffer-format experiments and raises the value of comparing the wrapper/core analog video path against a known-good HPS framebuffer test on the same S-Video profile.

### 15. FPGA-side `RxB` mismatch fix

Change:
- Fixed `resolve_yc_debug_fb_mode()` so the default native-analog path no longer returned before applying the FPGA-side `RxB` bit.
- This corrected a real mismatch where Linux saw `8888 rb=1` but the FPGA-side `spi_format` was still plain `0x6`.

Result:
- The live 3S-ARM grayscale run now reports:
  - `video_fb_enable_mode ... spi_format=0x16`
  - `get_video_info ... fb_fmt=214`
  - `fb_write_module_params_result ... applied_mode=8888 1 640 240 2560`
- Color still remained grayscale on real S-Video hardware.
- `/dev/fb0` still contained overwhelmingly non-gray pixels (`152307` non-gray pixels out of `153600`) even after the fix.

What it proved:
- The `RxB` mismatch was a genuine bug and is now corrected.
- But the corrected `8888 + RxB` HPS framebuffer path still loses chroma somewhere after `/dev/fb0`.
- The remaining problem is downstream of both SDL rendering and the Linux-side framebuffer mode write.

## Final Tested Packet Variants

Baseline live packet before temporary overrides:
- `clock_source=core`
- `phase_inc=196786767482`
- `colorburst_start=20`
- `colorburst_end=70`
- `yc_config=1`
- `subcarrier_enable=0`

Most aggressive tested packet:
- `clock_source=output`
- `phase_inc=312683830272`
- `colorburst_start=13`
- `colorburst_end=44`
- `yc_config=3`
- `subcarrier_enable=1`

Outcome:
- Both grayscale on real S-Video hardware.

## Current Remote Diagnostic Config

At the point this note was updated:
- `/media/fat/games/3s-arm/config` no longer carries `yc-debug-*` keys.
- Any future packet/framebuffer debug overrides should live in `/media/fat/games/3s-arm/yc-debug.conf`, not in the normal player-facing config file.

## Useful Commands

Live wrapper status:

```bash
MISTER_PASSWORD=1 tools/mister/misterctl.sh wrapper-status
```

Read persistent YC trace:

```bash
MISTER_PASSWORD=1 tools/mister/misterctl.sh yc-log --tail 120
MISTER_PASSWORD=1 tools/mister/misterctl.sh yc-log --all
```

## Open Problems

- **Horizontal scaling waviness**: The `384→640` non-integer horizontal expansion on the native TV raster still produces uneven/wavy fine detail. The brute-force per-pixel CRT filter in `93f24235` was too expensive (FPS drop). A cheaper CRT-appropriate resampler is the next direction here.
- Keep the scaling artifact as a separate second-stage problem:
  - first solve the grayscale/color failure with measured Menu-vs-3S-ARM init diffs
  - then revisit `384 -> 640` CRT resampling in `src/port/sdl/fbdev_presenter.c`
  - avoid the preserved `93f24235` full filter path as the default next attempt unless a cheaper measured variant is identified

## Cleanup Guidance

If abandoning this track, unwind in this order:

1. Remove any temporary `/media/fat/games/3s-arm/yc-debug.conf` file or debug-only keys written there.
2. Re-evaluate whether the committed native-analog branch commits listed above should be reverted wholesale or split into:
   - keepable geometry/startup work
   - discardable YC/debug experiments
