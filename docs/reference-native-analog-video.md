# Native Analog Video: Technical Reference

Authoritative reference for the FPGA native video output system in the 3SX MiSTer
port. Covers the complete signal path from ARM frame production through DDR3
double-buffering, FPGA pixel readout, video timing generation, YC color encoding,
and DAC output. Includes PLL design rationale, frame pacing architecture, S-Video
color fix history, and troubleshooting.

**Last updated:** 2026-03-31

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Video Output Paths](#2-video-output-paths)
3. [FPGA Native Video Pipeline](#3-fpga-native-video-pipeline)
4. [PLL Configuration](#4-pll-configuration)
5. [Video Timing Generator](#5-video-timing-generator)
6. [DDR3 Double-Buffer Protocol](#6-ddr3-double-buffer-protocol)
7. [DDR3 Reader State Machine](#7-ddr3-reader-state-machine)
8. [YC Color Encoding (S-Video/Composite)](#8-yc-color-encoding-s-videocomposite)
9. [S-Video Color Fix History](#9-s-video-color-fix-history)
10. [Frame Pacing](#10-frame-pacing)
11. [384-Native TV Mode](#11-384-native-tv-mode)
12. [Critical Invariants](#12-critical-invariants)
13. [Troubleshooting](#13-troubleshooting)
14. [File Index](#14-file-index)

---

## 1. Architecture Overview

The native video path bypasses the MiSTer Linux framebuffer scaler entirely. The
ARM writes RGB565 frames directly to DDR3 via `/dev/mem` mmap. The FPGA reads
these frames at vblank, converts RGB565 to RGB888, and outputs the pixels through
a timing generator that drives the analog DAC on the I/O board.

```
ARM Process                    DDR3 (shared)              FPGA
-----------                    -------------              ----
CPS3 emulator                  0x3A000000 ctrl word       native_video_reader.sv
  → RGB565 frame               0x3A000100 buffer 0          → dcfifo (64-bit)
  → NativeVideoWriter            0x3A02A200 buffer 1          → RGB565→RGB888
    → memcpy to DDR3            0x3A000040 feedback [P2]      → pixel output
    → write control word                                    native_video_timing.sv
                                                              → hsync/vsync/de
                                                            DAC pins (VGA_R/G/B)
                                                              → I/O board → CRT
```

The YC encoder (`yc_out.sv`) is NOT in the native video path. Native video
outputs plain RGB888 directly to the DAC. The YC encoder only processes the
Menu OSD and game core video when native video is inactive.

---

## 2. Video Output Paths

The system has three distinct video output paths:

| Path | Hardware | ARM writes to | FPGA reads from | Output |
|------|----------|---------------|-----------------|--------|
| **Native video** | Analog I/O board | DDR3 (`/dev/mem` mmap) | DDR3 reader (`status[9]`) | VGA/SCART/component |
| **FB scaler** | HDMI | Linux FB (`/dev/fb0`) | HPS FB overlay + ascal | HDMI connector |
| **Direct video** | HDMI-as-VGA | DDR3 (native video) | Core VGA → HDMI pins | HDMI (VGA timing) |

**Native video activation:**
- `status[9] = 1` enables the FPGA native video reader
- `cfg[15]` (set after HPS boot) gates the DDR3 bus mux to the native video reader
- `use_nv = NATIVE_VID & cfg[15]` controls the mux in `menu.sv`
- ARM sets `THREESX_NATIVE_VIDEO=1` (default) to enable `NativeVideoWriter_Init()`

**DAC output mux** (`menu.sv` lines 650-655):
- When `nv_active`: native video RGB drives VGA_R/G/B
- When `NATIVE_VID_ACTIVE` but not `nv_active`: output black (valid sync, no pixels yet)
- Otherwise: Menu OSD pattern generator

---

## 3. FPGA Native Video Pipeline

### Module Hierarchy

```
menu.sv
  └─ native_video_top.sv
       ├─ native_video_timing.sv    (H/V counters, sync generation)
       └─ native_video_reader.sv    (DDR3 Avalon master, dcfifo, pixel decode)
```

### Clock Domains

| Clock | Frequency | Source | Used by |
|-------|-----------|--------|---------|
| `clk_sys` (ddr_clk) | 100 MHz | PLL outclk_0 (VCO/9) | DDR3 Avalon master, state machine |
| `CLK_VIDEO` (clk_vid) | 16.364 MHz | PLL outclk_2 (VCO/55) | Timing generator, FIFO read side |
| `CE_PIXEL` | 8.182 MHz | CLK_VIDEO / 2 | Pixel-rate gating for counters and FIFO |

### Signal Flow

1. `native_video_timing` generates H/V counters, sync pulses, blanking, `new_frame`, `new_line`
2. `new_frame` CDC'd to ddr_clk domain triggers control word poll
3. State machine reads control word, detects new frame, reads scanlines via burst DMA
4. 64-bit DDR3 beats pushed into dual-clock dcfifo (256 entries, ~2.67 scanlines)
5. Read side pops 64-bit words, extracts 4 RGB565 pixels each, decodes to RGB888
6. RGB888 output gated by `de` (data enable) and `frame_ready`

---

## 4. PLL Configuration

### Current Configuration (commit af65300e)

```
Reference:  50 MHz (FPGA_CLK1_50)
VCO:        50 MHz × 18/1 = 900.000 MHz
outclk_0:   VCO / 9  = 100.000 MHz (clk_sys, exact)
outclk_2:   VCO / 55 = 16.3636 MHz (CLK_VIDEO)
CE_PIXEL:   divide-by-2
Pixel clk:  8,181,818.182 Hz
```

### Frame Rate Derivation

```
frame_rate = pixel_clock / (H_TOTAL × V_TOTAL)
           = 8,181,818.182 / (520 × 264)
           = 8,181,818.182 / 137,280
           = 59.59949106 Hz

CPS3 TARGET_FPS = 59.59949 Hz
Error           = +0.000001 Hz (1 microhertz)
Phase drift     = 1 frame / ~31 years
```

### Design Space Exploration

The PLL configuration was selected via exhaustive computational search over:
- M: 1–512, N: 1–512 (VCO = 50 × M/N, constrained to 600–1300 MHz)
- C: 1–512 (output divider)
- CE_PIXEL: {1, 2, 4, 8}
- H_TOTAL: 450–550, V_TOTAL: 250–280
- H_freq: 14,500–16,500 Hz (CRT 15 kHz compatibility)

170,846 valid combinations found with error < 0.01 Hz and VCO ≤ 1000 MHz.
Initial selection (M=373/N=21) failed Quartus synthesis — M value too large for
the PLL fitter. Final selection M=18/N=1/C=55/CE=2 chosen for:
- Near-zero frame rate error (1 μHz)
- H_freq = 15,734 Hz (NTSC standard — perfect CRT compatibility)
- Trivial VCO (900 MHz, M=18, N=1 — Quartus synthesizes instantly)
- clk_sys = 100 MHz exact (unchanged from original design)

### Previous Configuration (pre-e136fbc2, original)

```
VCO:       50 MHz × 5/8 = 31.25 MHz (via some M/N/C combination)
CE_PIXEL:  divide-by-4
Pixel clk: 7,812,500 Hz
H_TOTAL:   500, V_TOTAL: 262
Frame rate: 59.6374 Hz (error: +0.038 Hz → stale frame every ~26 seconds)
```

### Failed Intermediate Configuration (Quartus rejected)

```
VCO:       50 MHz × 373/21 = 888.095 MHz  (M=373 too large for Quartus fitter)
CE_PIXEL:  divide-by-2
Pixel clk: 7,790,309 Hz
H_TOTAL:   497, V_TOTAL: 263
Would have been: 59.59949 Hz — but Quartus 17 Lite could not synthesize M=373
```

### Why Not Fractional-N PLL?

The Cyclone V supports fractional-N PLLs, but the design uses integer-N only.
Fractional-N introduces delta-sigma modulated jitter on the pixel clock, which
causes visible horizontal position jitter on CRT displays. Since the integer-N
search found a configuration with 1.1 μHz error, fractional-N is unnecessary.

### Cyclone V Device

- **Part:** 5CSEBA6U23I7 (SoC, UFBGA-672, Industrial, Speed Grade 7)
- **PLL IP:** Altera PLL v17.0 (`altera_pll` primitive)
- **VCO range:** 600–1300 MHz
- **File:** `vendor/Menu_MiSTer/rtl/pll/pll_0002.v`

---

## 5. Video Timing Generator

**File:** `vendor/Menu_MiSTer/rtl/native_video_timing.sv`

### Timing Parameters

| Region | H (pixels) | V (lines) |
|--------|-----------|-----------|
| Active | 384 | 224 |
| Front porch | 28 | 14 |
| Sync | 38 | 3 |
| Back porch | 70 | 23 |
| **Total** | **520** | **264** |

### Derived Values

```
H_SYNC_START = 384 + 28        = 412
H_SYNC_END   = 412 + 38        = 450
V_SYNC_START = 224 + 14        = 238
V_SYNC_END   = 238 + 3         = 241
H_freq       = 8,181,818 / 520 = 15,734.27 Hz (NTSC standard)
```

### Sync Polarity

- `hsync`: active low (asserts at H_SYNC_START, deasserts at H_SYNC_END)
- `vsync`: active low (asserts at V_SYNC_START, deasserts at V_SYNC_END)
- Vertical transitions only at end of scanline (when hcount wraps)

### Output Signals

| Signal | Width | Description |
|--------|-------|-------------|
| `hcount` | 10 bits | Horizontal pixel counter (0 to H_TOTAL-1) |
| `vcount` | 9 bits | Vertical line counter (0 to V_TOTAL-1) |
| `hsync` | 1 bit | Horizontal sync, active low |
| `vsync` | 1 bit | Vertical sync, active low |
| `hblank` | 1 bit | Horizontal blanking, active high |
| `vblank` | 1 bit | Vertical blanking, active high |
| `de` | 1 bit | Data enable = ~(hblank | vblank) |
| `new_frame` | 1 bit | Single ce_pix pulse at start of vblank |
| `new_line` | 1 bit | Single ce_pix pulse at start of hblank |

### CRT Image Positioning

The CRT positions the image based on sync-to-active timing:
- Larger `H_BP` shifts image **right**
- Larger `V_BP` shifts image **down**
- Current values (H_BP=70, V_BP=23) center the 384×224 image on a standard NTSC CRT

---

## 6. DDR3 Double-Buffer Protocol

### Memory Layout

```
Physical        Offset    Size       Purpose
0x3A000000      0x0000    4 bytes    Control word (ARM → FPGA)
0x3A000040      0x0040    4 bytes    Feedback word (FPGA → ARM) [Part 2]
0x3A000100      0x0100    172,032B   Buffer 0 (384×224 RGB565)
0x3A02A200      0x2A200   172,032B   Buffer 1 (384×224 RGB565)
```

Total mapped region: 384 KB (`NV_DDR_REGION_SIZE = 0x00060000`).

### Control Word Format (offset 0x0000)

```
Bits [31:2]   frame_counter    Monotonic, incremented by ARM each frame
Bits [1:0]    active_buffer    Which buffer the ARM just wrote (0 or 1)
```

### Write Protocol (ARM side)

1. `memcpy` RGB565 pixel data to the **inactive** buffer
2. Increment `frame_counter`
3. Write control word: `(frame_counter << 2) | (active_buf & 1)`
4. Toggle `active_buf` for next frame

Ordering guarantee: `O_SYNC | MAP_SHARED` mmap ensures all pixel writes complete
before the control word write (ARM strongly-ordered device memory).

### Read Protocol (FPGA side)

1. At each vblank (`new_frame` pulse), poll control word via DDR3 burst read
2. Compare `ctrl_word[31:2]` to `prev_frame_counter`
3. If different: new frame available → clear FIFO, read from indicated buffer
4. If same: stale → re-read previous buffer (shows last good frame)
5. After 30 consecutive stale vblanks: deassert `frame_ready` (signal lost sync)

### Feedback Word Format (offset 0x0040) [Part 2, planned]

```
Bits [31:2]   vblank_counter     FPGA-side monotonic counter
Bits [1:0]    buffer_status      0/1 = consumed buffer, 2 = stale re-read
```

---

## 7. DDR3 Reader State Machine

**File:** `vendor/Menu_MiSTer/rtl/native_video_reader.sv`

### States

| State | Description |
|-------|-------------|
| `ST_IDLE` | Wait for `new_frame_ddr` and `enable_ddr` |
| `ST_POLL_CTRL` | Issue DDR3 burst read for control word |
| `ST_WAIT_CTRL` | Wait for `ddr_dout_ready`, capture control word |
| `ST_CHECK_CTRL` | Compare frame_counter, decide new/stale/no-frame |
| `ST_WRITE_FEEDBACK` | Write vblank feedback to DDR3 [Part 2] |
| `ST_WAIT_WR_ACK` | Wait for feedback write completion [Part 2] |
| `ST_READ_LINE` | Issue DDR3 burst read for one scanline (96 beats) |
| `ST_WAIT_LINE` | Capture DDR3 beats into FIFO, count to LINE_BURST |
| `ST_LINE_DONE` | Advance line counter, decide preload/wait/done |
| `ST_WAIT_DISPLAY` | Wait for `new_line_ddr` to trigger next scanline read |

### Line-on-Demand Design

- **Vblank preload:** Lines 0 and 1 are read immediately (back-to-back)
- **Active display:** Each subsequent line is read when `new_line_ddr` fires
- **FIFO depth:** 256 × 64-bit = 2.67 scanlines of buffer
- **Burst size:** 96 beats × 8 bytes = 768 bytes = 384 pixels × 2 bytes

### Clock Domain Crossings

| Signal | From | To | Method |
|--------|------|----|--------|
| `new_frame` | clk_vid | ddr_clk | 2-FF rising edge detector |
| `new_line` | clk_vid | ddr_clk | 2-FF rising edge detector |
| `vblank` | clk_vid | ddr_clk | 2-FF level synchronizer |
| `enable` | clk_vid | ddr_clk | 2-FF level synchronizer |
| `frame_ready_reg` | ddr_clk | clk_vid | 2-FF level synchronizer |
| FIFO data | ddr_clk | clk_vid | Altera dcfifo (4-stage sync) |

### Pixel Decode

Each 64-bit FIFO word contains 4 RGB565 pixels. The read side extracts pixels
sequentially using a 2-bit sub-pixel counter. RGB565 → RGB888 expansion:
```
R[7:0] = {pixel[15:11], pixel[15:13]}   (5→8 bit, MSB fill)
G[7:0] = {pixel[10:5],  pixel[10:9]}    (6→8 bit, MSB fill)
B[7:0] = {pixel[4:0],   pixel[4:2]}     (5→8 bit, MSB fill)
```

---

## 8. YC Color Encoding (S-Video/Composite)

**File:** `vendor/Menu_MiSTer/sys/yc_out.sv` (~640 lines)

**Important:** The YC encoder is NOT in the native video path. It processes the
Menu OSD and game core video only. Native video outputs plain RGB to the DAC.

### Subcarrier NCO

The color subcarrier is generated via a 40-bit phase accumulator:

```
phase_accum <= phase_accum + PHASE_INC
f_sub = (PHASE_INC / 2^40) × CLK_VIDEO
```

For NTSC: `f_sub = 3.579545 MHz`
For PAL:  `f_sub = 4.43361875 MHz`

PHASE_INC is computed on the ARM side (`video.cpp`) and sent via SPI:
```
PHASE_INC = (CLK_REF / CLK_VIDEO) × 2^40
```

### Colorspace Conversion (ITU-R BT.601)

```
Y = 0.299×R + 0.587×G + 0.114×B
U = 0.492×(B - Y)
V = 0.877×(R - Y)
Chroma = U×sin(ωt) + V×cos(ωt)
```

Output: `{chroma[7:0], luma[7:0], 8'd0}` (24-bit packed)

### PAL Phase Alternation

When `PAL_EN=1`, the V component sign alternates every line (triggered by hsync).
Burst phase alternates between 160° (PAL+) and 96° (PAL-).

### Two Encoder Instances

| Instance | Clock | Purpose |
|----------|-------|---------|
| `yc_out` | `clk_vid` (core video clock) | Menu OSD and game core video |
| `yc_out_fb` | `clk_hdmi` (HDMI scaler clock) | HPS framebuffer analog mode |

Both receive the same PHASE_INC. When CLK_VIDEO changes (e.g., PLL retune),
`video.cpp` recalculates PHASE_INC on-the-fly from the measured clock frequency.

---

## 9. S-Video Color Fix History

Three bugs had to be fixed for S-Video color to work on the HPS framebuffer path
(commit bc77d52a). The native video RGB path was unaffected.

### Bug 1: Wrong DAC Data Source (RTL)

**Root cause:** `vgas_en = vga_fb | vga_scaler`. When `vga_fb=1`, DAC routed to
the scaler path which outputs plain RGB (no YC encoding) → grayscale.

**Fix:** Added `yc_out_fb` instance on `clk_hdmi` domain with gate:
`vga_fb_yc_en = vga_fb & ~vga_scaler & yc_en`

### Bug 2: Wrong Pixel Clock for PHASE_INC (HPS)

**Root cause:** `set_yc_mode()` computed PHASE_INC from the FPGA core clock
(`ptime`). Before game starts, `ptime=0` → garbage PHASE_INC.

**Fix:** When `fb_native_analog_auto=true`, use `v_cur.Fpix` (output clock, e.g.,
12.587 MHz for 640×240 NTSC 15K) instead of core clock.

### Bug 3: Wrong PAL/NTSC Detection (HPS)

**Root cause:** PAL/NTSC derived from `current_video_info.vtime`. When `vtime=0`
(game not started), `fps=0 < 55` → `pal=1` (wrong for NTSC) → PAL subcarrier on
NTSC TV → grayscale.

**Fix:** When `fb_native_analog_auto=true` and `vtime==0`, derive PAL/NTSC from
output timing: `output_fps = v_cur.Fpix / (htotal × vtotal)`.

### Key Insight

Both the FBDEV path and native DDR3 path require `vga_scaler=0` for S-Video color
to work. When `vga_scaler=1`, the DAC routes through the scaler which strips YC
encoding.

---

## 10. Frame Pacing

### Part 1: PLL Retune (reverted — caused regressions)

**Problem:** FPGA ran at 59.6374 Hz vs CPS3's 59.59949 Hz. The 0.038 Hz mismatch
caused the DDR3 double-buffer to show a stale frame every ~26 seconds.

**What was attempted:** Retune PLL from 31.25 MHz/div4 to 16.364 MHz/div2
(VCO=900 MHz, M=18/N=1/C=55), producing 59.59949 Hz (1 μHz error). Timing changed
from 500×262 to 520×264. H_freq moved to 15,734 Hz (NTSC standard).

**What went wrong (three regressions):**

1. **Image 4.5% narrower on CRT.** The pixel clock changed from 7.8125 MHz to
   8.182 MHz. Active area time dropped from 49.15 μs to 46.93 μs. This is
   inherent to any PLL retune — changing the pixel clock changes the active area
   width on CRT. Cannot be fixed by blanking redistribution alone. The image
   visibly "looked narrower with bigger bars."

2. **S-Video color lost (grayscale).** The YC encoder runs on CLK_VIDEO. Changing
   CLK_VIDEO from 31.25 MHz to 16.364 MHz broke the PHASE_INC calculation because
   `video.cpp` had a hardcoded `31.25` for the native video path (line 3030).
   The NCO produced a 1.87 MHz subcarrier instead of 3.58 MHz — CRT color killer
   activated. Fix was to change `31.25` to `900.0/55.0`, but this revealed a
   deeper issue: any PLL change requires updating this hardcoded constant, which
   is fragile and error-prone.

3. **Quartus build failures.** The initial PLL config (M=373, N=21, VCO=888 MHz)
   was rejected by Quartus 17 Lite — M=373 exceeded the PLL fitter's search
   space. Required a second exhaustive search to find M=18/N=1 (trivial for
   Quartus). Multiple build attempts were lost to SSH timeouts killing Quartus
   in the colima VM (documented in `feedback-quartus-nohup.md`).

**Lesson learned:** Changing the PLL has a wide blast radius. It affects pixel
clock (image width on CRT), YC encoder (color), all clock frequency comments in
RTL, the hardcoded CLK_VIDEO in video.cpp, and potentially the colorburst range.
The 0.038 Hz drift it fixes is subtle (~26-second stale frame) while the
regressions are immediately visible. The ARM-side NV_TARGET_FPS compensation
(running 0.063% fast) is the safer approach.

### Part 2: Vsync Feedback (reverted — caused frame rate collapse)

**Problem:** Even with matched frequencies, the ARM and FPGA run on independent
clocks. OS scheduling jitter from Linux can delay frame writes past the FPGA's
vblank boundary, causing sporadic stale frames that look "skippy."

**What was attempted:** FPGA writes a 32-bit feedback word (vblank_counter +
buffer_status) to DDR3 at 0x3A000040 at each vblank. ARM reads this and
phase-locks its frame delivery using a low-pass filter (α=1/16) to track the
FPGA's true frame period, with stale-frame detection that nudges the deadline
earlier.

**What went wrong (frame rate collapsed to ~35 fps):**

The closed-loop algorithm has a **positive feedback instability**. The ARM reads
the FPGA's feedback word at a variable point during its frame processing (after
rendering, during the pacing sleep). The `observe_ns` timestamp captures *when
the ARM reads the feedback*, not *when the FPGA wrote it*. If the ARM reads
early in one frame and late in the next, the observed delta between vblank
counter changes is inflated. The low-pass filter pushes `adjusted_frame_time_ns`
toward this inflated value, causing longer sleeps, which causes even later reads,
which inflates the observed delta further — a runaway positive feedback loop that
collapses the frame rate.

**Root cause:** The fundamental design flaw is using the ARM's own timestamp to
infer FPGA vblank timing. The ARM doesn't know *when* the feedback word was
written — only *when it noticed the counter changed*. The latency between write
and read is variable and depends on where in the frame loop the ARM happens to
check. This variable latency corrupts the period measurement.

**What would fix it:** The FPGA could write its own timestamp (a free-running
counter value) into the feedback word instead of just a vblank counter. The ARM
could then compute the exact vblank-to-vblank interval from the FPGA's own clock,
independent of when the ARM reads it. Alternatively, a simpler approach: skip the
period tracking entirely and just use the stale/fresh status as a binary phase
detector — if stale, deliver earlier next frame; if fresh, hold steady. This
avoids the period measurement problem altogether.

### Pre-Part-1 Frame Pacing (working baseline)

The working configuration before these changes:

```
FPGA:  59.6374 Hz (31.25 MHz PLL, div4, 500×262)
ARM:   targets NV_TARGET_FPS = 59.6374 Hz (matches FPGA)
Game:  runs 0.063% fast (imperceptible)
```

Open-loop pacing with deadline-based sleep:

```c
// sdl_app.c frame pacing loop
if (now < frame_deadline) {
    SDL_DelayNS(frame_deadline - now);
}
frame_deadline += target_frame_time_ns;  // = 1e9 / NV_TARGET_FPS
if (now > frame_deadline + target_frame_time_ns) {
    frame_deadline = now + target_frame_time_ns;  // resync if >1 frame behind
}
```

**Known issues with baseline (not yet fixed):**
- ~26-second periodic stale frame from 0.038 Hz drift (Part 1 target)
- Sporadic stale frames on heavy stages from OS scheduling jitter (Part 2 target)
- Both manifest as "60fps on counter but feels skippy"

---

## 11. 384-Native TV Mode

### Problem

S-Video on the FBDEV path uses a 640×240 framebuffer. The game renders at 384
pixels wide, requiring 384→640 horizontal scaling (1.667×, non-integer). This
creates a visible [1,2] pixel-width alternation pattern (waviness on fine detail).

### Solution

Use a 384-wide TV mode for non-scandoubled native analog output. The framebuffer
matches the game native width, so the presenter does a 1:1 copy with no scaling.

### 384×240 NTSC 15K Timing

```
hact=384  hfp=18  hs=36  hbp=42  htotal=480  Fpix=7.552446593 MHz
vact=240  vfp=6   vs=4   vbp=14  vtotal=264
line_rate = 7,552,447 / 480 = 15,734.26 Hz (NTSC standard exact)
fsc / line_rate = 3,579,545 / 15,734.26 = 227.500 (NTSC half-cycle constraint)
```

### 384×288 PAL 15K Timing

```
hact=384  hfp=18  hs=36  hbp=42  htotal=480  Fpix=7.5 MHz
vact=288  vfp=6   vs=4   vbp=14  vtotal=312
line_rate = 7,500,000 / 480 = 15,625 Hz (PAL standard exact)
```

See `docs/native-analog-svideo-plan.md` for full design rationale.

---

## 12. Critical Invariants

### S-Video Color

1. **`vga_scaler=0` required** in MiSTer INI (both global and `[3SX]` section).
   When `vga_scaler=1`, DAC routes through the scaler → plain RGB → grayscale.
2. **NEVER set `vga_scaler=1`** in any INI configuration. This breaks S-Video
   color AND native video aspect ratio.
3. YC encoder PHASE_INC must match the actual pixel clock. If CLK_VIDEO changes
   (PLL retune), `video.cpp` auto-recalculates — but `yc.txt` overrides may be stale.

### Native Video Activation

1. `status[9]=1` enables FPGA reader. `cfg[15]` gates the DDR3 bus mux.
2. Do NOT set `vga_fb=1` when native video is active (would route DAC to scaler).
3. ARM must write frames at TARGET_FPS (59.59949 Hz) to match FPGA vblank rate.

### Frame Timing

1. ARM writes to inactive buffer, then writes control word atomically.
2. FPGA reads control word at vblank, switches buffer if frame_counter changed.
3. After 30 consecutive stale vblanks, FPGA deasserts `frame_ready` (blanks output).

### DDR3 Access

1. ARM mapping uses `O_SYNC | MAP_SHARED` (uncached, strongly-ordered).
2. 32-bit aligned accesses are atomic on the HPS Avalon bridge.
3. FPGA and ARM use separate DDR3 ports — concurrent access is safe.

---

## 13. Troubleshooting

### No S-Video color (grayscale)

1. Check `vga_scaler=0` in INI (`MiSTer.ini` and `[3SX]` section)
2. Verify `yc_en=1` in YC config (check `video.cpp` logs)
3. Check PHASE_INC matches pixel clock frequency
4. Confirm PAL/NTSC detection correct (fps > 55 → NTSC)

### Horizontal waviness / scaling artifacts

- The FBDEV path scales 384→640 (non-integer 1.667×)
- Solution: Use native video path (DDR3 direct) or 384-native TV mode
- Native video outputs 384 pixels natively — no scaling

### Frame stutter every ~26 seconds

- This was caused by the old PLL running at 59.6374 Hz vs CPS3's 59.59949 Hz
- Fixed by PLL retune (commit e136fbc2)
- If still present: verify the new PLL is compiled into the bitstream

### Sporadic frame drops under system load

- OS scheduling jitter delays `SDL_DelayNS()`, causing ARM to miss FPGA vblank
- Part 2 (vsync feedback) will fix this — not yet implemented
- Workaround: reduce background system load during gameplay

### Black screen with valid sync

- FPGA outputs sync even before first frame arrives (DAC mux outputs black)
- Wait for ARM to start writing frames
- If persistent: check `native_video_writer_enabled` in logs
- Verify `/dev/mem` mmap succeeded and DDR3 region is accessible

### CRT won't sync

- H_freq must be in 14,500–16,500 Hz range for 15 kHz CRTs
- Current: 15,675 Hz (within range)
- Some CRTs may need manual H-hold adjustment for non-standard frequencies

---

## 14. File Index

### FPGA RTL

| File | Lines | Description |
|------|-------|-------------|
| `vendor/Menu_MiSTer/rtl/native_video_timing.sv` | ~184 | H/V counter, sync generation, 520×264 timing |
| `vendor/Menu_MiSTer/rtl/native_video_reader.sv` | ~493 | DDR3 Avalon master, dual-clock FIFO, RGB565 decode |
| `vendor/Menu_MiSTer/rtl/native_video_top.sv` | ~137 | Top-level wrapper (timing + reader) |
| `vendor/Menu_MiSTer/rtl/pll/pll_0002.v` | ~94 | PLL IP (M=18/N=1, outclk_0=100 MHz, outclk_2=16.4 MHz) |
| `vendor/Menu_MiSTer/rtl/pll.v` | ~258 | PLL wrapper |
| `vendor/Menu_MiSTer/menu.sv` | ~658 | Core top-level (PLL inst, CE_PIXEL, DDR3 mux, DAC mux) |
| `vendor/Menu_MiSTer/sys/yc_out.sv` | ~640 | YC/S-Video encoder (NCO, colorspace, burst) |
| `vendor/Menu_MiSTer/sys/sys_top.v` | ~1500+ | System top (YC instances, PHASE_INC regs, DAC routing) |

### ARM C Code

| File | Lines | Description |
|------|-------|-------------|
| `src/port/sdl/native_video_writer.c` | ~127 | DDR3 frame writer (mmap, double-buffer, control word) |
| `src/port/sdl/native_video_writer.h` | ~27 | Writer API |
| `include/port/sdl/sdl_app.h` | ~99 | TARGET_FPS, NV_TARGET_FPS defines |
| `src/port/sdl/sdl_app.c` | ~10000+ | Frame pacing loop (~line 9979), native video init (~line 9300) |

### Documentation

| File | Description |
|------|-------------|
| `docs/reference-native-analog-video.md` | This document |
| `docs/plan-frame-pacing-fix.md` | PLL retune + vsync feedback implementation plan |
| `docs/spec-fpga-native-video.md` | Original FPGA native video design spec |
| `docs/design-fpga-native-video.md` | Architecture overview and design decisions |
| `docs/native-analog-svideo-plan.md` | 384-native TV mode design |
| `docs/research-video-output-paths.md` | Video path routing research |
