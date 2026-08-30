# Native Analog Video: Technical Reference

Authoritative reference for the FPGA native video output system in the 3S-ARM MiSTer
port. Covers the complete signal path from ARM frame production through DDR3
double-buffering, FPGA pixel readout, video timing generation, YC color encoding,
and DAC output. Includes PLL design rationale, frame pacing architecture, S-Video
color fix history, and troubleshooting.

**Last updated:** 2026-04-07

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
- ARM sets `THIRDSARM_NATIVE_VIDEO=1` (default) to enable `NativeVideoWriter_Init()`

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
| `clk_sys` (ddr_clk) | 100 MHz | System PLL (pll_0002) | DDR3 Avalon master, state machine |
| `CLK_VIDEO` (clk_vid) | 31.154 MHz | Video PLL (pll_video_0002) | Timing generator, FIFO read side |
| `CE_PIXEL` | 7.789 MHz | CLK_VIDEO / 4 | Pixel-rate gating for counters and FIFO |

### Signal Flow

1. `native_video_timing` generates H/V counters, sync pulses, blanking, `new_frame`, `new_line`
2. `new_frame` CDC'd to ddr_clk domain triggers control word poll
3. State machine reads control word, detects new frame, reads scanlines via burst DMA
4. 64-bit DDR3 beats pushed into dual-clock dcfifo (256 entries, ~2.67 scanlines)
5. Read side pops 64-bit words, extracts 4 RGB565 pixels each, decodes to RGB888
6. RGB888 output gated by `de` (data enable) and `frame_ready`

---

## 4. PLL Configuration

### Current Configuration

```
Reference:  50 MHz (FPGA_CLK1_50)
System PLL: 50 MHz × 2/1 = 100.000 MHz (clk_sys, pll_0002.v)
Video PLL:  50 MHz × 81/5 = 810.000 MHz VCO, /26 = 31.153846 MHz (CLK_VIDEO, pll_video_0002.v)
CE_PIXEL:   divide-by-4
Pixel clk:  7,788,461.538 Hz
```

### Frame Rate Derivation

```
frame_rate = pixel_clock / (H_TOTAL × V_TOTAL)
           = 7,788,461.538 / (495 × 264)
           = 7,788,461.538 / 130,680
           = 59.59949 Hz

H_freq     = 7,788,461.538 / 495 = 15,734.266 Hz (NTSC standard, exact)

CPS3 TARGET_FPS = 59.59949 Hz
Error           = +0.0000014 Hz (1.4 microhertz)
Phase drift     = 1 stale frame / ~8.2 days
```

The H-freq is mathematically identical to the NTSC standard line rate: both
reduce to the rational number 2,250,000/143 Hz, derived from the NTSC color
subcarrier (315/88 MHz × 2/455).

### Design Space Exploration

The PLL configuration was selected via exhaustive computational search over:
- M: 1–120, N: 1–50 (VCO = 50 × M/N, constrained to 600–1300 MHz)
- C: 1–512 (output divider)
- CE_PIXEL: {1, 2, 4}
- H_TOTAL: 488–520, V_TOTAL: 260–270
- H_freq: within ±30 Hz of 15,734 Hz (NTSC standard)
- Active area time change: < 2% (CRT image width preservation)

M upper limit of 120 determined empirically: M=92 synthesizes in Quartus 17
Lite, M=171 is rejected ("illegal value"). M=81 chosen — smaller than the
proven M=92, guaranteeing Quartus acceptance.

### Previous Configurations

**Dedicated video PLL (pre-NTSC retune):**
```
VCO:       50 MHz × 92/7 = 657.14 MHz, /21 = 31.2925 MHz
CE_PIXEL:  divide-by-4
Pixel clk: 7,823,129 Hz
H_TOTAL:   501, V_TOTAL: 262
Frame rate: 59.5993 Hz (error: -0.00015 Hz)
H_freq:    15,615 Hz (not NTSC — some arcade monitors couldn't sync)
```

**Original shared PLL:**
```
VCO:       50 MHz × 5/8 = 31.25 MHz (outclk_2 of system PLL)
CE_PIXEL:  divide-by-4
Pixel clk: 7,812,500 Hz
H_TOTAL:   500, V_TOTAL: 262
Frame rate: 59.6374 Hz (error: +0.038 Hz → stale frame every ~26 seconds)
```

### Failed Configurations

**M=18/N=1/C=55/CE=2 (reverted — image too narrow):**
Produced exact NTSC timing (520×264) but pixel clock was 8.182 MHz — 4.5%
faster than original. Active area time dropped from 49.15 μs to 46.93 μs,
visibly narrowing the CRT image. Also broke S-Video (hardcoded CLK_VIDEO
in video.cpp). See Section 10 for full details.

**M=373 (Quartus rejected):**
Would have produced 59.59949 Hz but M=373 exceeded Quartus 17 Lite's PLL
fitter search space.

**M=171 (Quartus rejected):**
Would have produced exact NTSC at H_TOTAL=494 with CE_PIXEL=4, but M=171
also exceeded Quartus's limit. Led to discovering the M=81 solution at
H_TOTAL=495.

### Why Not Fractional-N PLL?

The Cyclone V supports fractional-N PLLs, but the design uses integer-N only.
Fractional-N introduces delta-sigma modulated jitter on the pixel clock, which
causes visible horizontal position jitter on CRT displays. Since the integer-N
search found a configuration with 1.4 μHz error, fractional-N is unnecessary.

### Cyclone V Device

- **Part:** 5CSEBA6U23I7 (SoC, UFBGA-672, Industrial, Speed Grade 7)
- **PLL IP:** Altera PLL v17.0 (`altera_pll` primitive)
- **VCO range:** 600–1300 MHz
- **System PLL file:** `vendor/Menu_MiSTer/rtl/pll/pll_0002.v` (100 MHz only)
- **Video PLL file:** `vendor/Menu_MiSTer/rtl/pll_video/pll_video_0002.v` (31.1538 MHz)

---

## 5. Video Timing Generator

**File:** `vendor/Menu_MiSTer/rtl/native_video_timing.sv`

### Timing Parameters

| Region | H (pixels) | V (lines) |
|--------|-----------|-----------|
| Active | 384 | 224 |
| Front porch | 22 | 16 |
| Sync | 38 | 3 |
| Back porch | 51 | 21 |
| **Total** | **495** | **264** |

### Derived Values

```
H_SYNC_START = 384 + 22        = 406
H_SYNC_END   = 406 + 38        = 444
V_SYNC_START = 224 + 16        = 240
V_SYNC_END   = 240 + 3         = 243
H_freq       = 7,788,462 / 495 = 15,734.27 Hz (NTSC standard, exact)
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
- Current values (H_BP=51, V_BP=21) center the 384×224 image on a standard NTSC CRT

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

### Feedback Word Format (offset 0x0040)

Written by the MiSTer wrapper (not the FPGA) to relay the FPGA's vblank
counter to the game app for closed-loop phase locking.

```
Offset 0x40:  bits[31:8] = ARM CLOCK_MONOTONIC timestamp (bottom 24 bits, microseconds)
              bits[7:0]  = FPGA 8-bit frame counter (from UIO 0x42)
Offset 0x44:  32-bit sequence number (monotonically increasing, for torn-read detection)
```

See `docs/archive/summary-vsync-feedback-relay.md` for full implementation details.

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

### Part 1: PLL Retune — NTSC H-freq (current, working)

**Problem:** The original shared PLL ran at 59.6374 Hz (0.038 Hz off CPS3 target),
causing stale frames every ~26 seconds. The dedicated video PLL (92/7/21) improved
V-freq to 59.5993 Hz (0.00015 Hz error) but H-freq was 15,615 Hz — not NTSC
standard. Some arcade monitors (Blast City MS-2930/MS-2931) couldn't sync.

**Solution:** PLL retune to M=81/N=5/C=26 with H_TOTAL=495, V_TOTAL=264,
CE_PIXEL=÷4. This produces:
- H-freq = 15,734.266 Hz — mathematically identical to NTSC standard (2,250,000/143 Hz)
- V-freq = 59.59949 Hz — 1.4 μHz error vs CPS3 target
- Pixel clock = 7.7885 MHz — only 0.45% slower than previous, imperceptible on CRT
- CLK_VIDEO = 31.1538 MHz — same order as previous 31.2925 MHz, no architectural change

**Why this succeeded where the earlier retune (M=18/N=1/C=55/CE=2) failed:**
1. CE_PIXEL stayed at ÷4 (previous attempt changed to ÷2, doubling pixel clock)
2. H_TOTAL=495 (previous used 520 — more blanking forced faster pixel clock)
3. M=81 < proven M=92 (previous M=373 and M=171 were rejected by Quartus)
4. `video.cpp` CLK_VIDEO updated to `405.0/13.0` (exact rational representation)

**Earlier failed retune (M=18/N=1/C=55/CE=2, reverted):**
Produced exact NTSC timing at 520×264 but with CE_PIXEL=÷2, the pixel clock
jumped to 8.182 MHz — 4.5% faster. Active area time dropped from 49.15 μs to
46.93 μs, visibly narrowing the CRT image. Also broke S-Video due to hardcoded
`31.25` in video.cpp. See Section 4 "Failed Configurations" for details.

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

### Current Frame Pacing (working)

```
FPGA:  59.59949 Hz (31.1538 MHz PLL, div4, 495×264)
ARM:   targets NV_TARGET_FPS = TARGET_FPS = 59.59949 Hz
Error: 1.4 μHz — one stale frame per ~8.2 days (effectively zero)
```

Closed-loop phase locking with vsync feedback from the wrapper:

- Wrapper reads FPGA's 8-bit vblank counter via UIO 0x42 (~1 kHz poll rate)
- Writes counter + CLOCK_MONOTONIC timestamp to DDR3 at 0x3A000040
- Game app reads feedback, computes ideal deadline = next_vsync − 2 ms lead time
- Blends frame_deadline toward ideal at 25% per frame (smooth convergence)
- Falls back to open-loop when feedback is stale (>100 ms)
- SCHED_FIFO + mlockall + hybrid sleep/busy-wait for sub-ms precision
- Kill switch: `THIRDSARM_VSYNC_FEEDBACK=0`
- Lead time tuning: `THIRDSARM_LEAD_TIME_US` (default 2000)

See `docs/archive/summary-vsync-feedback-relay.md` for full architecture.

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

1. **`vga_scaler=0` required** in MiSTer INI (both global and `[3S-ARM]` section).
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

1. Check `vga_scaler=0` in INI (`MiSTer.ini` and `[3S-ARM]` section)
2. Verify `yc_en=1` in YC config (check `video.cpp` logs)
3. Check PHASE_INC matches pixel clock frequency
4. Confirm PAL/NTSC detection correct (fps > 55 → NTSC)

### Horizontal waviness / scaling artifacts

- The FBDEV path scales 384→640 (non-integer 1.667×)
- Solution: Use native video path (DDR3 direct) or 384-native TV mode
- Native video outputs 384 pixels natively — no scaling

### Frame stutter every ~26 seconds

- This was caused by the original shared PLL running at 59.6374 Hz vs CPS3's 59.59949 Hz
- Fixed by dedicated video PLL (current: M=81/N=5/C=26, V-freq error = 1.4 μHz)
- If still present: verify the new PLL bitstream (3S-ARM.rbf) is deployed

### Sporadic frame drops under system load

- OS scheduling jitter delays wake-up, causing ARM to miss FPGA vblank
- Fixed by closed-loop vsync phase lock (wrapper relays FPGA frame counter)
- If still present: check backend.log for "closed-loop vsync feedback engaged"
- Kill switch: set `THIRDSARM_VSYNC_FEEDBACK=0` to revert to open-loop

### Black screen with valid sync

- FPGA outputs sync even before first frame arrives (DAC mux outputs black)
- Wait for ARM to start writing frames
- If persistent: check `native_video_writer_enabled` in logs
- Verify `/dev/mem` mmap succeeded and DDR3 region is accessible

### CRT won't sync

- H_freq must be in 14,500–16,500 Hz range for 15 kHz CRTs
- Current: 15,734 Hz (NTSC standard, exact — maximum CRT compatibility)
- If still failing: check composite_sync setting, verify RBF is deployed

---

## 14. File Index

### FPGA RTL

| File | Lines | Description |
|------|-------|-------------|
| `vendor/Menu_MiSTer/rtl/native_video_timing.sv` | ~184 | H/V counter, sync generation, 495×264 timing |
| `vendor/Menu_MiSTer/rtl/native_video_reader.sv` | ~493 | DDR3 Avalon master, dual-clock FIFO, RGB565 decode |
| `vendor/Menu_MiSTer/rtl/native_video_top.sv` | ~137 | Top-level wrapper (timing + reader) |
| `vendor/Menu_MiSTer/rtl/pll/pll_0002.v` | ~88 | System PLL IP (100 MHz clk_sys only) |
| `vendor/Menu_MiSTer/rtl/pll_video/pll_video_0002.v` | ~87 | Video PLL IP (31.1538 MHz CLK_VIDEO) |
| `vendor/Menu_MiSTer/rtl/pll.v` | ~258 | System PLL wrapper |
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
| `docs/archive/spec-fpga-native-video.md` | Original FPGA native video design spec |
| `docs/design-fpga-native-video.md` | Architecture overview and design decisions |
| `docs/native-analog-svideo-plan.md` | 384-native TV mode design |
| `docs/research-video-output-paths.md` | Video path routing research |
