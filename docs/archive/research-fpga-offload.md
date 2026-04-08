# Research: FPGA-Offloaded Rendering for 3SX

Last updated: 2026-04-01
Status: Initial research — to be expanded with benchmarks and deep investigation

---

## Background

The MiSTer DE10-Nano's Cyclone V SoC has two compute resources: an ARM Cortex-A9
(800 MHz, dual-core) and an FPGA fabric. Currently 3SX uses the ARM for all game
logic and rendering, with the FPGA only handling video output (timing, DDR3
framebuffer readout, DAC). The ARM is the bottleneck — Genei-Jin drops to ~41 FPS
due to the non-integer sprite scaling gather loop.

A hybrid approach offloads pixel-level work to the FPGA so the ARM can focus on
game logic. This is proven viable by:

- **Minimig Hybrid** (scrameta/Minimig-AGA_MiSTer_Hybrid): ARM emulates 68030/040/060
  CPU, FPGA handles Amiga AGA chipset. Closest architectural parallel to 3SX.
- **Quake II on FPGA** (petrmikheev/endeavour2): 207 MHz RISC-V (much weaker than our
  A9) achieves playable Quake II by offloading palette mapping, bilinear scaling, and
  alpha blending to FPGA hardware via DMA.
- **Discord discussion**: "A hybrid approach where the fpga is handling rendering could
  be more responsive, very passable id imagine" / "But the next step of talking to the
  fpga silicon and developing a renderer that uses sd ram/ddr to access the graphic data"

This document covers two incremental FPGA offload opportunities that build on
our existing `native_video_top.sv` infrastructure without touching the game engine's
rendering pipeline.

---

## Table of Contents

1. [FPGA-Side Format Conversion (ARGB8888 → RGB888)](#1-fpga-side-format-conversion)
2. [FPGA Hardware Scaler](#2-fpga-hardware-scaler)
3. [DMA-Assisted Pixel Operations (Future)](#3-dma-assisted-pixel-operations-future)
4. [Current Architecture Reference](#4-current-architecture-reference)
5. [Open Questions](#5-open-questions)
6. [Sources](#6-sources)

---

## 1. FPGA-Side Format Conversion

### Status: TODO

### Problem

The ARM currently does two things to present a frame:

1. Renders the game scene into an SDL_Surface at 384x224 ARGB8888
2. Converts ARGB8888 → RGB565 and writes to DDR3 for the FPGA native video reader

Step 2 is unnecessary work. The FPGA already decodes pixels during scanout — it
could just as easily decode ARGB8888 as RGB565. The ARM could skip conversion
entirely and write its native ARGB8888 format directly to DDR3.

### Current Pixel Path

```
Game render (ARGB8888)
  → ARM CPU converts ARGB8888 → RGB565        ← eliminate this
    → ARM writes RGB565 to DDR3 (172 KB/frame)
      → FPGA native_video_reader reads RGB565
        → FPGA decodes RGB565 → RGB888
          → DAC → CRT
```

### Proposed Pixel Path

```
Game render (ARGB8888)
  → ARM writes ARGB8888 to DDR3 (344 KB/frame)
    → FPGA native_video_reader reads ARGB8888
      → FPGA strips alpha, outputs RGB888
        → DAC → CRT
```

### What Changes

**ARM side:**
- `fbdev_presenter.c` (or native video write path): write the SDL_Surface pixel
  buffer directly to DDR3 via memcpy/NEON copy. No format conversion loop.
- Buffer size doubles: 384 * 224 * 4 = 344,064 bytes per buffer.
- Update control word format if needed (add pixel format flag).

**FPGA side (`native_video_reader.sv`):**
- Change pixel decode from RGB565 (2 bytes/pixel, 4 pixels per 64-bit beat) to
  ARGB8888 (4 bytes/pixel, 2 pixels per 64-bit beat).
- `LINE_BURST` doubles: 96 → 192 beats per scanline (768 bytes → 1536 bytes).
  This fits the Avalon-MM max burst of 256 beats.
- FIFO depth may need to increase from 256 to 512 entries to hold 2+ scanlines
  at the new beat count.
- Pixel decode simplifies — just extract bytes, no bit-field expansion:

```verilog
// Current: RGB565 decode (bit replication for 5→8 / 6→8 expansion)
wire [7:0] dec_r = {cur_pix[15:11], cur_pix[15:13]};
wire [7:0] dec_g = {cur_pix[10:5],  cur_pix[10:9]};
wire [7:0] dec_b = {cur_pix[4:0],   cur_pix[4:2]};

// Proposed: ARGB8888 decode (just extract bytes, ignore alpha)
wire [31:0] cur_pix32 = pixel_word[{pixel_sub[0], 5'b00000} +: 32];
wire [7:0]  dec_r = cur_pix32[23:16];
wire [7:0]  dec_g = cur_pix32[15:8];
wire [7:0]  dec_b = cur_pix32[7:0];
// cur_pix32[31:24] = alpha (discarded)
```

### Trade-offs

| Factor | RGB565 (current) | ARGB8888 (proposed) |
|--------|-------------------|---------------------|
| ARM CPU cost | Conversion loop (~0.3-0.5 ms/frame) | Zero (direct memcpy) |
| DDR3 write bandwidth | 172 KB/frame | 344 KB/frame |
| DDR3 read bandwidth | 172 KB/frame | 344 KB/frame |
| FIFO depth needed | 256 entries (2.7 lines) | 512 entries (2.7 lines) |
| FPGA decode complexity | Bit-field expansion | Byte extraction (simpler) |
| Pixel fidelity | 16-bit color (65K colors) | 24-bit color (16M colors) |

**Key consideration:** DDR3 bandwidth. The Cyclone V DDR3 controller provides
~3.2 GB/s peak. At 60 FPS, ARGB8888 needs 344 KB * 60 = ~20 MB/s read bandwidth
— less than 1% of peak. Bandwidth is not a concern.

### Estimated Effort

Low-Medium. The RTL change to `native_video_reader.sv` is ~30 lines (pixel decode,
burst count, address stride). The ARM-side change is removing conversion code and
adjusting the DDR3 write address/size. Quartus rebuild required.

### Possible Optimization: Configurable Format

Add a control register bit so the FPGA can accept either RGB565 or ARGB8888. The
ARM sets the format in the control word. This preserves backward compatibility and
lets us benchmark both paths.

```
Control word layout (proposed):
  [1:0]   active_buffer (0 or 1)
  [2]     pixel_format (0 = RGB565, 1 = ARGB8888)
  [31:3]  frame_counter
```

---

## 2. FPGA Hardware Scaler

### Status: TODO

### Problem

The native video path outputs 384x224 at ~59.6 Hz directly to CRT. This works
perfectly for analog output. But two scenarios need scaling:

1. **HDMI output** — HDMI sinks typically expect ≥640x480. The MiSTer scaler
   (ascal) handles this today, but adds 1-2 frames of latency. A simpler FPGA
   integer scaler could do 2x (768x448) with much less latency.

2. **Non-integer sprite scaling** — The game's internal renderer scales sprites
   from various source sizes to the 384x224 framebuffer using a per-pixel LUT
   gather loop (`software_frame_non_integer.c`). This is the primary performance
   bottleneck at ~7 ms during Genei-Jin. If the FPGA could do the upscale, the
   ARM would write smaller source surfaces and the FPGA would scale during
   scanout.

Scenario 1 is straightforward. Scenario 2 is a much larger architectural change
and is covered separately.

### Scenario 1: Integer Display Upscale

Add an optional 2x integer scaler between the native video reader and the video
output. The FPGA reads 384x224 from DDR3 (as today) and outputs 768x448 by
doubling each pixel horizontally and vertically.

**Timing change:**
- Current: 384x224 @ 59.6 Hz, ~7.8 MHz pixel clock
- Scaled: 768x448 @ 59.6 Hz, ~31.25 MHz pixel clock (4x pixel clock)
- H_TOTAL = 1000, V_TOTAL = 524 (doubled)

**RTL approach:**
- Add a `scale_2x` control bit
- When enabled, each pixel from the reader is output twice horizontally
  (duplicate on consecutive ce_pix cycles)
- Each scanline is output twice vertically (re-read same FIFO data)
- Timing generator switches to doubled parameters

**Alternative: Scanline filter.** Instead of raw 2x, insert black lines or
attenuated lines between doubled scanlines for a CRT scanline effect. This is
essentially what `scandoubler.sv` in the MiSTer sys/ framework does — could
potentially reuse that module.

**Benefit for HDMI users:** Lower latency than the ascal scaler path, since
the integer scaler is purely combinational/1-cycle pipeline with no frame
buffering.

### Scenario 2: FPGA-Assisted Sprite/Surface Scaling

This is the more ambitious idea: offload the non-integer gather loop to the FPGA.
Instead of the ARM doing per-pixel `dst[col] = src[lookup[col]]`, the ARM would
write source surface data + a scale command to DDR3, and the FPGA would produce
the scaled result.

**Concept:**
```
ARM writes to DDR3:
  - Source surface pixels (e.g., 32x32 RGB565)
  - Scale parameters (src_w, src_h, dst_w, dst_h)
  - Destination address in DDR3

FPGA reads source, applies nearest-neighbor or bilinear scale,
writes result to destination address in DDR3.

ARM composites the pre-scaled surfaces into the final 384x224 framebuffer.
```

**This is what the Quake II FPGA project does** — the FPGA acts as a DMA-based
2D blitter with scaling. Their implementation handles:
- Palette lookup (8-bit indexed → RGB565)
- Bilinear 2x upscale
- Alpha-blended mixing of RGB565 surfaces

**Why this is hard for 3SX:**
- The game's rendering pipeline is deeply interleaved — sprites, backgrounds,
  and effects are composited in a specific order with palette lookups, alpha
  blending, and effect transforms (Genei-Jin metamorphose). Extracting individual
  scale operations into FPGA commands requires refactoring the render pipeline.
- The gather loop's bottleneck is the data-dependent indirect load pattern, not
  raw compute. The FPGA's advantage would be its ability to do multiple DDR3
  reads in burst without stalling on dependent loads.
- Round-trip latency: ARM writes command → FPGA executes → ARM reads result.
  If the ARM has to wait for each scaled surface before compositing the next,
  the pipeline stalls. Need a command queue / async model.

**Investigation needed:**
- Profile how much of the 7 ms Genei-Jin bottleneck is in the gather loop vs.
  surface setup and compositing
- Determine if the hot sprite shapes (32x32 → 34-37 scaled) can be batched into
  a single FPGA DMA command
- Study the Quake II FPGA DMA controller RTL for the command interface pattern
- Measure HPS-to-FPGA bridge round-trip latency for a single DMA blit operation

### Estimated Effort

- **Scenario 1 (integer display upscale):** Low-Medium. Modify timing generator
  parameters and add pixel/line doubling logic. ~100-200 lines of RTL.
- **Scenario 2 (sprite scaling offload):** High. Requires new FPGA DMA blitter
  module, ARM-side command queue, and render pipeline refactoring. Research
  project scope.

---

## 3. DMA-Assisted Pixel Operations (Future)

Beyond format conversion and scaling, the FPGA could offload other per-pixel
operations that currently run on the ARM. These are further out but worth
tracking as potential future work.

### 3a. Hardware Alpha Blending

The game composites multiple layers with alpha. The ARM does this in software
with NEON. An FPGA blitter could read two surfaces from DDR3, blend them with
a specified alpha, and write the result back.

**Quake II precedent:** Their FPGA does "batched RGB565 mixing" — hardware alpha
blend of two surfaces in a single DMA operation.

**Applicability to 3SX:** High — the Genei-Jin effect layers multiple
semi-transparent copies of the character sprite. If the FPGA could blend them
in hardware, the ARM just submits draw commands.

### 3b. Hardware Palette Lookup

CPS3 uses indexed color with palette indirection. If the FPGA held the active
palette in BRAM, it could convert indexed pixels to RGB during DMA/scanout,
eliminating ARM-side palette application.

**Applicability to 3SX:** Medium — the decompiled PS2 code has already resolved
palettes to ARGB8888 in many code paths. Would need to audit which paths still
use indexed color.

### 3c. Hardware Solid Fill / Rect Clear

The ARM sometimes clears large framebuffer regions or fills solid-color rects.
An FPGA DMA fill operation could do this at DDR3 bus speed (~3.2 GB/s) vs. the
ARM's cached write + flush path.

**Applicability to 3SX:** Low — not a significant bottleneck currently.

---

## 4. Current Architecture Reference

### Existing FPGA Native Video Modules

| File | Purpose | Key Parameters |
|------|---------|----------------|
| `native_video_top.sv` | Top wrapper, instantiates timing + reader | clk_sys=100MHz, clk_vid=20MHz |
| `native_video_reader.sv` | DDR3 burst reader, RGB565 decode, dcfifo | 96-beat burst, 256-deep FIFO |
| `native_video_timing.sv` | H/V counter, sync gen, 384x224 @ 59.6Hz | H_TOTAL=500, V_TOTAL=262 |

### DDR3 Memory Map

```
0x3A000000 + 0x000   : Control word (frame_counter[31:2], active_buffer[1:0])
0x3A000000 + 0x100   : Buffer 0 (384 * 224 * 2 = 172,032 bytes, RGB565)
0x3A000000 + 0x2A200 : Buffer 1 (172,032 bytes, RGB565)
```

### ARM Render Pipeline

```
Game logic (AcrSDK, effects, AI)
  → Software renderer (384x224 ARGB8888 SDL_Surface)
    → Sprite scaling via per-pixel LUT (non-integer gather loop)  ← bottleneck
      → Format conversion (ARGB8888 → RGB565)
        → DDR3 write (native video buffer)
          → FPGA scanout
```

### Performance Baseline (March 2026)

| Scene | FPS | Frame Time | Notes |
|-------|-----|------------|-------|
| Training mode | ~59-60 | ~16.7ms | Near target |
| Heavy gameplay | ~58 | ~17.2ms | Minor drops |
| Remy-Left stage | ~55 | ~18.2ms | Texture dirty refresh |
| Genei-Jin visible | ~41 | ~24.4ms | Non-integer gather loop |

### HPS-FPGA Bridge Bandwidth

| Bridge | Width | Peak Bandwidth |
|--------|-------|----------------|
| H2F (HPS-to-FPGA) | up to 128-bit | ~3.2 GB/s |
| LW H2F (Lightweight) | 32-bit | Much lower |
| F2H (FPGA-to-HPS) | up to 128-bit | ~3.2 GB/s |

At 60 FPS: 344 KB/frame (ARGB8888) = ~20 MB/s — well under 1% of peak.

---

## 5. Open Questions

- [ ] What is the actual ARM CPU cost of ARGB8888 → RGB565 conversion? Need
  PMU measurement to quantify the savings from format conversion offload.
- [ ] Does doubling FIFO depth (256 → 512) fit within Cyclone V M10K budget?
  Current M10K usage from last Quartus build needs checking.
- [ ] For sprite scaling offload: how many distinct scale operations happen per
  frame during Genei-Jin? Is it 5-10 (batchable) or 50+ (pipeline concern)?
- [ ] What is the HPS-to-FPGA bridge round-trip latency for a 1 KB DMA transfer?
  This determines whether per-surface offload is viable or only bulk operations.
- [ ] Can we reuse the MiSTer framework's `scandoubler.sv` for integer 2x output,
  or does our native timing generator need its own implementation?
- [ ] Does the Quake II FPGA DMA blitter RTL (endeavour2 repo) use the same
  Avalon-MM interface as our DDR3 controller? Could we adapt it directly?

---

## 6. Sources

| Source | Relevance |
|--------|-----------|
| petrmikheev/endeavour2 (Quake II on FPGA) | DMA blitter pattern, hardware palette/scale/blend |
| scrameta/Minimig-AGA_MiSTer_Hybrid | ARM CPU + FPGA chipset hybrid architecture |
| psakhis/Groovy_MiSTer | DDR3 framebuffer reader, Avalon-MM burst pattern |
| Cyclone V bridge measurements (UviDTE-FPSoC) | HPS-FPGA bandwidth numbers |
| SpinalVoodoo (3DFX on FPGA) | Shows RTL rasterization is possible on Cyclone V |
| Section 3.8 of research-mister-arm-perf-optimizations.md | FPGA DMA-assisted color conversion research |
| Discord (community discussion) | "hybrid approach where fpga handles rendering" |
