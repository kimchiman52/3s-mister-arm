# MiSTer ARM Performance Optimization Research

Last updated: 2026-03-28
Status: Research catalog — items to investigate one by one

This document captures all findings from deep research into software-rendered MiSTer
FPGA games, ARM Cortex-A9 optimization techniques, and learnings from the ACGC (Animal
Crossing GameCube) MiSTer port feasibility study. Each item is a potential optimization
for 3SX (Street Fighter III: 3rd Strike on MiSTer).

---

## Table of Contents

1. [Current Performance Baseline](#1-current-performance-baseline)
2. [Other Software-Rendered MiSTer Games](#2-other-software-rendered-mister-games)
3. [Optimization Catalog](#3-optimization-catalog)
   - 3.1 [Morton-Order Texture Swizzling](#31-morton-order-texture-swizzling)
   - 3.2 [Write-Combining for DDR3 Native Video Writes](#32-write-combining-for-ddr3-native-video-writes)
   - 3.3 [NEON RGB565 Conversion with Intrinsics](#33-neon-rgb565-conversion-with-intrinsics)
   - 3.4 [NEON-to-ARM Register Transfer Avoidance](#34-neon-to-arm-register-transfer-avoidance)
   - 3.5 [Integer 16-bit NEON Paths Over Float32](#35-integer-16-bit-neon-paths-over-float32)
   - 3.6 [fastarm Library for Bulk Memory Operations](#36-fastarm-library-for-bulk-memory-operations)
   - 3.7 [Pixman Pipelined NEON Alpha Blending](#37-pixman-pipelined-neon-alpha-blending)
   - 3.8 [FPGA DMA-Assisted Color Conversion](#38-fpga-dma-assisted-color-conversion)
   - 3.9 [Compiler Flags: Single-Precision and Fast-Math](#39-compiler-flags-single-precision-and-fast-math)
   - 3.10 [Render to Cached Buffer, Bulk Copy to Uncached DDR3](#310-render-to-cached-buffer-bulk-copy-to-uncached-ddr3)
   - 3.11 [Cache-Line-Aligned Bulk Copy with PLD Prefetch](#311-cache-line-aligned-bulk-copy-with-pld-prefetch)
   - 3.12 [PMU-Backed Performance Counter Measurement](#312-pmu-backed-performance-counter-measurement)
   - 3.13 [Per-Surface Alpha Sidecar Bitmasks](#313-per-surface-alpha-sidecar-bitmasks)
   - 3.14 [Clustered-Shape Specialization for Genei-Jin Sprites](#314-clustered-shape-specialization-for-genei-jin-sprites)
   - 3.15 [DFPSR NEON Rendering Abstraction](#315-dfpsr-neon-rendering-abstraction)
   - 3.16 [sse2neon for Porting Optimized Routines](#316-sse2neon-for-porting-optimized-routines)
   - 3.17 [Auto-Vectorization Compiler Flag](#317-auto-vectorization-compiler-flag)
   - 3.18 [Separate Code and Data Pages (Minimig Lesson)](#318-separate-code-and-data-pages-minimig-lesson)
   - 3.19 [RGB565 Framebuffer for 2x Bandwidth Reduction](#319-rgb565-framebuffer-for-2x-bandwidth-reduction)
4. [Cortex-A9 Hardware Reference](#4-cortex-a9-hardware-reference)
5. [MiSTer Framebuffer Architecture Reference](#5-mister-framebuffer-architecture-reference)
6. [Master Source Index](#6-master-source-index)

---

## 1. Current Performance Baseline

As of March 2026:

| Scene | FPS | Frame Time | Bottleneck |
|-------|-----|------------|------------|
| Training mode | ~59-60 | ~16.7ms | Near target |
| Control/heavy gameplay | ~58 | ~17.2ms | Minor |
| Remy-Left stage | ~55 | ~18.2ms | Texture compare-dirty refresh |
| Genei-Jin first visible | ~41 | ~24.4ms | Non-integer gather loop |

**Primary bottleneck:** The non-integer gather loop in `software_frame_non_integer.c`.
Each pixel requires a data-dependent indirect load: `dst[col] = src[lookup[col]]`.
ARM NEON lacks scatter/gather instructions, prefetch can't overcome dependent-load
latency, and 150+ optimization iterations have failed to improve it safely.

**Key hot shapes:** 32x32→34-37 sized sprites (top 4 shape families = 30ms of 78ms
total during Genei-Jin).

---

## 2. Other Software-Rendered MiSTer Games

### 2.1 PrBoom-Plus (Doom)

- **Source:** https://github.com/bbond007/MiSTer_PrBoom-Plus
- **Video backend:** SDL 1.2 with Linux framebuffer (`fbdev`)
- **Resolution:** 640x480 with `vga_scaler=1`
- **Performance:** Developer stated: "the weakness of the DE10's ARM CPU has been
  greatly exaggerated." Considerably higher FPS than ao486 DOS Doom.
- **Audio:** FluidSynth for MIDI, soundfonts at `/media/fat/linux/soundfonts/`
- **Multiplayer:** Networking works
- **Build:** Prebuilt ARM binaries, cross-compiled. Installer deploys .deb packages
  for dependencies from `DEBS/` directory.
- **Install path:** `/media/fat/linux/`
- **Takeaway:** Proves 640x480 software rendering is viable at good framerates. One of
  the most successful MiSTer Linux ports.

### 2.2 ScummVM

- **Source:** https://github.com/bbond007/MiSTer_ScummVM
- **Video backend:** SDL, video mode set via `vmode -r 640 480 rgb16` (RGB565)
- **Resolution:** 640x480
- **Performance:** Excellent — SVGA games use only ~25% CPU
- **Purpose:** "Running newer SVGA/TrueColor/Pentium games which are way beyond ao486"
- **Engines:** 50+ supported (SCUMM, SCI, AGI, Kyra, Mohawk, etc.)
- **Install:** Self-contained with bundled shared libraries at `/media/fat/ScummVM/`
- **Recommendation:** Overclock for best experience
- **Takeaway:** Uses **RGB565 (16-bit) for 2x performance boost** over 32-bit modes.
  Confirms 16-bit color is the way to go for MiSTer ARM rendering.

### 2.3 JFDuke3D (Duke Nukem 3D)

- **Forum thread:** https://misterfpga.org/viewtopic.php?t=8555
- **Video backend:** SDL2, pure software rendering, no OpenGL
- **Performance:** "Fast as flip" — good framerate with sound
- **Technique:** Compiled statically with SDL2. Runs on framebuffer after exiting LXDE
  desktop (Ctrl+Alt+F1).
- **Takeaway:** Proves Build engine complexity is achievable on MiSTer ARM.

### 2.4 OpenBOR (Beat-em-up Engine)

- **Source:** https://github.com/SumolX/MiSTer_OpenBOR
- **Video backend:** SDL 1.2 (stuck on older version intentionally)
- **Critical limitation:** "All the drawing is being done by the CPU directly to the
  framebuffer" — performance limited to lower-resolution older games. Scaling kills
  performance.
- **SDL constraint:** OpenBOR deprecated SDL 1.2 support and SDL2's fbdev support is
  broken/abandoned on MiSTer. Forced to use older OpenBOR version.
- **Resolutions:** 320x240 (CRT/analog) and HDMI native
- **Launchers:** `OpenBOR_CRT.sh` (320x240) and `OpenBOR_HDMI.sh`
- **Takeaway:** SDL2 `fbdev` is non-functional — confirms our SDL dummy + direct
  fbdev/DDR3 approach is correct.

### 2.5 Dethrace (Carmageddon)

- **Source:** https://github.com/dethrace-labs/dethrace
- **Forum thread:** https://misterfpga.org/viewtopic.php?p=98014
- **Type:** Reverse-engineered Carmageddon running natively on ARM via SDL
- **Takeaway:** Another pure software renderer on MiSTer ARM.

### 2.6 Descent 1+2 (Dxx Rebirth) — True 3D Polygons

- **Status:** Available on MiSTer ARM
- **Significance:** The **only confirmed true 3D polygon software-rendered game** on
  MiSTer ARM. Not raycasting — actual polygon rasterization.
- **Performance details:** Scarce, but it runs.
- **Takeaway:** Proves the ARM can handle real 3D polygon rendering, not just 2D
  sprites or raycasting.

### 2.7 DOSBox

- **Source:** https://github.com/bbond007/MiSTer_DOSBox
- **Version:** DOSBox 0.74-3
- **Performance:** "80386 level performance". Acknowledged as slow. "DOSBox on an
  overclocked Pi3b can have significant slow downs and wipes the floor with the DE10
  on ARM side."
- **Overclock impact:** ~50% performance improvement at 1.2 GHz
- **Takeaway:** Not a great showcase, but confirms overclock ROI is significant.

### 2.8 Minimig Hybrid Emulation

- **Source:** https://github.com/scrameta/Minimig-AGA_MiSTer_Hybrid
- **Support tools:** https://github.com/scrameta/MiSTer_Hybrid_Support
- **Architecture:** ARM does 68030/040/060 CPU emulation, FPGA handles Amiga AGA
  chipset. Closest architectural parallel to 3sx (ARM computes, FPGA does video/HW).
- **Bridge:** Uses patched Main_MiSTer binary with HPS-FPGA bridge for ARM-to-FPGA
  communication.
- **Critical finding:** Memory page sharing between code and data "kills qemu
  performance" — they ship `AllocP`/`Alloc32P` utilities to separate code and data
  pages.
- **Takeaway:** If hot code and hot data compete for the same cache sets, performance
  degrades silently. See item 3.18.

### 2.9 Other Known Ports

| Project | Type | Notes |
|---------|------|-------|
| PICO-8 | Fantasy console | Runs headless, renders via framebuffer. Min 700MHz, MiSTer has 800. |
| Cave Story (NXEngine) | 2D platformer | "Quite good with some minor slowdowns" |
| Basilisk II | Mac 68k emulator | Framebuffer output, another complex emulation on ARM |
| RetroArch | Emulator frontend | "Only software-based libretro cores". RGUI menu, no shaders. |
| Quake 1-3 | FPS source ports | Quake 1 "barely playable" without optimization |
| Super Tux Kart | 3D racing | Requires OpenGL — likely very slow |
| Pingus, Wesnoth, FreeCiv | 2D strategy | Various states of functionality |

### 2.10 OpenLara (Tomb Raider Engine) — Not on MiSTer, But Relevant

- **Source:** https://github.com/XProger/OpenLara
- **Language:** 61.1% C, 31% C++, 5.1% Assembly
- **What it is:** Complete Tomb Raider engine reimplementation with dedicated software
  rasterizers for platforms without GPUs. Has GBA (ARM assembly), 3DO, 32X, DOS backends.
- **MiSTer status:** Not ported. Community says "entirely software rendered and
  slooooooow" but no one has tried with NEON optimization.
- **GBA rasterizer technical details** (`src/platform/gba/rasterizer.h`, `asm/*.s`):
  - Scanline-based edge-walking triangle rasterization
  - **16.16 fixed-point arithmetic** throughout (no float in inner loop)
  - Gouraud shading with 13-bit precision (8 + 5 extra bits)
  - Affine texture mapping with per-scanline UV derivative interpolation
  - 8-bit indexed color with 64KB pre-computed lightmap (`gLightmap[256*32]`)
  - ARM assembly inner loops: `rasterizeGT.s`, `rasterizeFT.s`, `rasterizeFTA.s`,
    `clearFB.s`, `transformMesh.s`, `transformRoom.s`, `matrixRotate.s`
  - Loop unrolling (8-pixel blocks), conditional execution to minimize branching
  - Lookup-table division for reciprocals
- **Takeaway:** Fixed-point is the way to go for ARM inner loops. The engine manages
  ~20fps on a 16MHz ARM7TDMI (GBA) at 240x160 — our 800MHz A9 with NEON is orders of
  magnitude faster. Key patterns: lookup-table division, conditional execution,
  aggressive unrolling.

### 2.11 Quake II on FPGA — DMA-Assisted Rendering

- **Source:** https://github.com/petrmikheev/endeavour2
- **Blog:** https://blog.mikhe.ch/quake2-on-fpga/
- **Hardware:** DIY FPGA board, RISC-V CPU at 207 MHz (511 DMIPS) — much weaker than
  our Cortex-A9
- **Key technique:** Uses the FPGA's DMA controller as a "primitive 2D GPU":
  - 256-color → RGB565 palette mapping (hardware color lookup)
  - Bilinear upscaling (hardware interpolation)
  - Batched RGB565 mixing (hardware alpha blend)
- **Achieves playable framerates** despite dramatically weaker CPU
- **Takeaway:** See item 3.8. The MiSTer FPGA could offload color conversion and
  scaling from the ARM CPU.

---

## 3. Optimization Catalog

Each item below is a discrete optimization to investigate. They are ordered roughly by
expected impact and feasibility, but should be evaluated individually.

Status key: `TODO` = not started, `INVESTIGATING` = in progress, `DONE` = implemented,
`REJECTED` = tested and rejected with reason.

---

### 3.1 Morton-Order Texture Swizzling

**Status:** TODO
**Expected impact:** Medium-High (directly targets the gather bottleneck)
**Complexity:** Medium
**Relevance:** Primary bottleneck (Genei-Jin non-integer gather loop)

#### Problem

The non-integer scaling loop's core pattern is:
```c
for (int col = 0; col < width; col++) {
    dst[col] = src[lookup[col]];  // scatter/gather: lookup → address → pixel
}
```

Source textures are stored in standard row-major order (pixels in a row are contiguous).
When the lookup indices scatter across non-adjacent rows or columns, each access can
miss L1 cache. The 32x32 sprite tiles that dominate Genei-Jin produce scattered access
patterns across 256x256 source surfaces.

#### Technique

Morton order (Z-order curve) interleaves X and Y coordinate bits so that 2D-spatially-
local pixels are also memory-local:

```
Standard row-major:       Morton order:
0  1  2  3               0  1  4  5
4  5  6  7               2  3  6  7
8  9  10 11              8  9  12 13
12 13 14 15              10 11 14 15
```

For a 32-byte cache line on Cortex-A9:
- **16bpp (RGB565):** 8x2 pixel tiles per cache line
- **32bpp (ARGB8888):** 4x2 pixel tiles per cache line

Morton-order addressing:
```c
// Bits of x and y are interleaved:
// offset = ...y2 x2 y1 x1 y0 x0
// Increment through Morton order (carry propagation through bit "holes"):
offs_x = (offs_x - x_mask) & x_mask;
```

#### Why It Could Help

The non-integer scaling lookup typically accesses a small rectangular sub-region of the
source texture (e.g., 32x32 pixels from a 256x256 surface). In row-major order, these
pixels span 32 cache lines (one per row). In Morton order, the same 32x32 region spans
only ~16 cache lines because spatially-adjacent rows share cache lines.

For the hot 32x32→34-37 sprite shapes, this could reduce L1 misses by ~50%.

#### Implementation Plan

1. Add Morton-order swizzle pass in texture cache refresh (when surface is marked dirty
   and re-converted)
2. Add Morton-order lookup in the non-integer scaling inner loop
3. Benchmark with perf-sampler on Genei-Jin scene

#### Risks

- Swizzle cost at texture cache refresh time adds per-frame overhead
- Morton addressing adds instruction overhead per pixel in the inner loop
- Net effect depends on whether cache miss reduction outweighs addressing overhead
- Only helps if the gather pattern has 2D spatial locality (needs telemetry to confirm)

#### Sources

- Fabian Giesen — Texture tiling and swizzling: https://fgiesen.wordpress.com/2011/01/17/texture-tiling-and-swizzling/
- ACGC MiSTer feasibility doc, Section A.8.3 (Texture Memory Layout)
- ARM Cortex-A9 cache: 32KB L1, 32-byte lines, 4-way associative

---

### 3.2 Write-Combining for DDR3 Native Video Writes

**Status:** TODO
**Expected impact:** Medium (targets frame present, not render)
**Complexity:** Medium-High (may require kernel module)
**Relevance:** Native video output path (`native_video_writer.c`)

#### Problem

`native_video_writer.c` maps DDR3 at physical address `0x3A000000` using:
```c
fd = open("/dev/mem", O_RDWR | O_SYNC);
buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys_addr);
```

`O_SYNC` produces a **strongly-ordered/device memory** mapping. Every single write goes
directly to DDR3 with no caching, no write-combining, no buffering. Each 2-byte pixel
write is a separate bus transaction.

At 384x224 RGB565 = 172,032 bytes per frame. With individual uncached writes, this is
far slower than it needs to be.

#### Technique

Three approaches to enable write-combining:

**Option A: Custom kernel module with `pgprot_writecombine()`**
- Create a minimal character device driver that maps the physical region with
  write-combining page attributes
- User-space mmap via the device node gets WC mapping automatically
- Most performant option

**Option B: `udmabuf` kernel module**
- https://github.com/ikwzm/udmabuf
- Allocates DMA-capable buffer with configurable cache attributes
- Available as loadable kernel module, no kernel rebuild needed
- Can specify `sync_mode` for write-combining behavior

**Option C: Remove O_SYNC, accept coherency risk**
- Without `O_SYNC`, the kernel may map with normal cacheable attributes
- Frame data could sit in cache and never reach DDR3
- Would need explicit cache flush operations (`__builtin___clear_cache` or similar)
- Fragile and platform-dependent

#### Expected Improvement

From Cyclone V SoC benchmark research:
- Write operations with caches ON are **10.9x faster** than caches OFF
- Even write-combining (not fully cached) provides 3-5x improvement over
  strongly-ordered for sequential writes

Frame present is currently ~0.5ms. With WC, could drop to ~0.1ms. Small absolute gain
but frees ARM cycles for rendering.

#### Sources

- Cyclone V SoC time measurements: https://github.com/UviDTE-FPSoC/CycloneVSoC-time-measurements
- udmabuf DMA buffer driver: https://github.com/ikwzm/udmabuf
- Fabian Giesen — Write-combining: https://fgiesen.wordpress.com/2013/01/29/write-combining-is-not-your-friend/
- ACGC MiSTer feasibility doc, Section A.2 (Framebuffer Architecture)

---

### 3.3 NEON RGB565 Conversion with Intrinsics

**Status:** TODO
**Expected impact:** Medium (targets color conversion hot path)
**Complexity:** Low-Medium
**Relevance:** ARGB8888→RGB565 conversion in `sdl_app.c`

#### Problem

Current conversion loop in `sdl_app.c:112-120`:
```c
for (int i = 0; i < pixel_count; i++) {
    uint32_t argb = src[i];
    uint32_t r = (argb >> 16) & 0xFF;
    uint32_t g = (argb >> 8) & 0xFF;
    uint32_t b = argb & 0xFF;
    dst[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
```

This is scalar — processes one pixel per iteration.

#### Technique

NEON can process 8 pixels simultaneously using 128-bit registers:

```c
// Load 8 ARGB8888 pixels (32 bytes) into two Q registers
uint8x16x4_t argb = vld4q_u8((const uint8_t*)src);  // deinterleave into R,G,B,A

// Extract and shift channels
uint16x8_t r = vshrq_n_u16(vmovl_u8(vget_low_u8(argb.val[1])), 3);  // R >> 3
uint16x8_t g = vshrq_n_u16(vmovl_u8(vget_low_u8(argb.val[2])), 2);  // G >> 2
uint16x8_t b = vshrq_n_u16(vmovl_u8(vget_low_u8(argb.val[3])), 3);  // B >> 3

// Combine: (R << 11) | (G << 5) | B
uint16x8_t rgb565 = vorrq_u16(vorrq_u16(vshlq_n_u16(r, 11), vshlq_n_u16(g, 5)), b);

// Store 8 RGB565 pixels (16 bytes)
vst1q_u16(dst, rgb565);
```

An even more efficient approach uses `VSRI` (vector shift right and insert):
```c
// VSRI shifts right and inserts into existing value, avoiding separate OR
uint16x8_t result = vshrq_n_u16(r_wide, 3);           // start with R
result = vsriq_n_u16(result, g_wide, 5);               // insert G
result = vsriq_n_u16(result, b_wide, 11);              // insert B
```

ARM benchmarks show **127% speedup** for NEON RGB888→RGB565 conversion over scalar C.

#### Implementation Plan

1. Add NEON intrinsic version gated behind `#ifdef __ARM_NEON`
2. Process 8 pixels per iteration (16 pixels if double-pumping with two Q registers)
3. Handle tail pixels (count % 8) with scalar fallback
4. Benchmark with perf-sampler

#### Sources

- ARM NEON DirectFB optimization: https://developer.arm.com/community/arm-community-blogs/b/tools-software-ides-blog/posts/optimizing-directfb-with-arm-neon
- ACGC MiSTer feasibility doc, Section A.7 (NEON Optimization Techniques)

---

### 3.4 NEON-to-ARM Register Transfer Avoidance

**Status:** TODO
**Expected impact:** Medium (depends on current code patterns)
**Complexity:** Low (audit and restructure)
**Relevance:** All NEON code paths in `sdl_game_renderer.c`

#### Problem

On Cortex-A9, transferring data between ARM general-purpose registers and NEON registers
costs **>20 cycles per transfer**. A round-trip (ARM→NEON→ARM) costs 40+ cycles. If
NEON pixel processing loops pull values out to ARM for conditional checks and push them
back, this hidden penalty dominates.

#### Technique

Audit all NEON code in `sdl_game_renderer.c` (lines 5755-6109) for patterns like:
```c
// BAD: ARM↔NEON round-trip
uint32x4_t pixels = vld1q_u32(src);
uint32_t first = vgetq_lane_u32(pixels, 0);  // NEON→ARM: >20 cycles
if (first == 0) { ... }                       // ARM conditional
pixels = vsetq_lane_u32(new_val, pixels, 0);  // ARM→NEON: >20 cycles
```

Replace with NEON-native conditionals:
```c
// GOOD: Stay in NEON
uint32x4_t pixels = vld1q_u32(src);
uint32x4_t zero = vdupq_n_u32(0);
uint32x4_t mask = vceqq_u32(pixels, zero);    // compare in NEON
pixels = vbslq_u32(mask, replacement, pixels); // select in NEON
```

#### Implementation Plan

1. Audit `sdl_game_renderer.c` NEON sections for `vgetq_lane_*` / `vget_lane_*` calls
2. Audit for `vmovq_n_*` from ARM scalar variables inside loops
3. Restructure to keep data in NEON registers throughout pixel processing
4. Benchmark before/after

#### Sources

- ACGC MiSTer feasibility doc, Section A.8.2 (NEON on A9 Specifics)
- ARM Cortex-A9 Technical Reference Manual: NEON/VFP register transfer latency
- Cortex-A9 perf data: https://www.7-cpu.com/cpu/Cortex-A9.html

---

### 3.5 Integer 16-bit NEON Paths Over Float32

**Status:** TODO
**Expected impact:** Medium (2x throughput for color ops)
**Complexity:** Medium
**Relevance:** Color modulation and blending in `sdl_game_renderer.c`

#### Problem

NEON on Cortex-A9 has a **64-bit datapath** — 128-bit Q register operations take 2
cycles. This means:
- `float32x4_t` (4 floats): 4 values per 2 cycles = 2 values/cycle
- `int16x8_t` (8 shorts): 8 values per 2 cycles = 4 values/cycle
- `uint8x16_t` (16 bytes): 16 values per 2 cycles = 8 values/cycle

Color channels (0-255) fit perfectly in 16-bit integers. Using `int16x8_t` for color
math processes **2x as many pixels** per cycle as `float32x4_t`.

#### Technique

For alpha blending (the most common color operation):
```c
// Float32 path: 4 pixels per Q register
float32x4_t src_r = vcvtq_f32_u32(src_r_u32);
float32x4_t result = vmulq_f32(src_r, alpha_f);  // 4 pixels, 2 cycles

// Integer 16-bit path: 8 pixels per Q register
uint16x8_t src_r = vmovl_u8(src_r_u8);           // widen 8→16
uint16x8_t result = vmulq_u16(src_r, alpha_u16);  // 8 pixels, 2 cycles
result = vshrq_n_u16(result, 8);                   // >> 8 for fixed-point
```

Additional A9-specific timing:
- NEON integer VMLA (multiply-accumulate): efficient
- NEON float VMLA: 9 cycles (worse than separate VMUL+VADD at 5+6 cycles with
  pipeline fill)
- Prefer integer MLA for color math on A9

#### Sources

- ACGC MiSTer feasibility doc, Section A.8.2 and A.8.5
- ARM Cortex-A9 NEON Reference: https://developer.arm.com/documentation/ddi0409/latest/
- Pandora float optimization: https://pandorawiki.org/Floating_Point_Optimization

---

### 3.6 fastarm Library for Bulk Memory Operations

**Status:** TODO
**Expected impact:** Low-Medium (targets memcpy/memset in fbdev presenter)
**Complexity:** Low (drop-in replacement)
**Relevance:** `fbdev_presenter.c` bulk copies, frame buffer clears

#### Description

`fastarm` is a hand-tuned ARMv7 assembly library (63.6% assembly) for optimized
memcpy, memset, and related operations on Cortex-A8/A9:

- **PLD (prefetch) pipelining:** Prefetches 3-6 cache lines ahead
- **LDM/STM (load/store multiple):** Copies 32 bytes (one cache line) per instruction
  pair
- **Alignment handling:** Detects source/dest alignment and uses optimal path
- **Store buffer awareness:** Writes in cache-line-sized chunks to avoid partial-line
  penalties

On Cortex-A9, NEON does NOT help for memcpy (NEON unit is not tightly integrated).
PLD + LDM/STM is the optimal approach.

#### Why fastarm Over glibc memcpy

MiSTer's Buildroot-based Linux has a generic ARMv7 memcpy that may not be optimally
tuned for the A9's specific cache hierarchy and store buffer behavior. `fastarm`
targets this hardware profile specifically.

#### Implementation Plan

1. Cross-compile `fastarm` for ARM hard-float
2. Replace `memcpy` calls in `fbdev_presenter.c` hot paths
3. Replace `memset32` NEON implementation with `fastarm` equivalent
4. Benchmark frame present time

#### Source

- fastarm: https://github.com/hglm/fastarm
- ACGC MiSTer feasibility doc, Section A.11 (Key Community Resources)

---

### 3.7 Pixman Pipelined NEON Alpha Blending

**Status:** TODO
**Expected impact:** High for blending-heavy scenes
**Complexity:** Medium
**Relevance:** Alpha blending in `sdl_game_renderer.c`

#### Technique

Pixman achieved **~10x speedup** over C for bilinear scaling with pipelined NEON
assembly. The key pattern for alpha blending (division by 255 without actual division):

```asm
// Canonical /255 approximation: (x + (x >> 8)) >> 8
// In NEON assembly:
vmull.u8  q_temp, d_src, d_alpha    // 8-bit × 8-bit → 16-bit (multiply)
vrsra.u16 q_temp, q_temp, #8       // rounded shift-right accumulate (add x>>8)
vrshrn.u16 d_result, q_temp, #8    // rounded shift-right narrow (>> 8 + narrow)
```

This processes 8 pixels of alpha blending in 3 NEON instructions. The `vrsra`
(rounding shift-right accumulate) and `vrshrn` (rounding shift-right narrow)
instructions are specifically designed for this pattern.

#### Pipelined Loop Structure

Pixman uses a head/tail_head/tail pattern to overlap load, process, and store across
loop iterations:

```
Iteration N:     [STORE prev] [PROCESS curr] [LOAD next]
Iteration N+1:   [STORE curr] [PROCESS next] [LOAD next+1]
```

This hides memory latency by ensuring loads are issued well before the data is needed.

#### Deinterleaved Processing

Use `VLD4` to split ARGB into separate R, G, B, A channel registers:
```c
uint8x16x4_t rgba = vld4q_u8(src);  // deinterleave 16 pixels into 4 channel vectors
// Process each channel in parallel
// ...
vst4q_u8(dst, rgba);                // interleave back
```

This avoids per-pixel channel extraction overhead.

#### Sources

- Pixman NEON assembly: https://github.com/servo/pixman/blob/master/pixman/pixman-arm-neon-asm.S
- Pixman NEON asm header (macros): https://github.com/servo/pixman/blob/master/pixman/pixman-arm-neon-asm.h
- NEON alpha blending reference: https://github.com/akulkar4/AlphaBlend
- ACGC MiSTer feasibility doc, Section A.7.2

---

### 3.8 FPGA DMA-Assisted Color Conversion

**Status:** TODO
**Expected impact:** Medium-High (offloads work from ARM entirely)
**Complexity:** High (requires FPGA wrapper changes + Quartus build)
**Relevance:** ARGB8888→RGB565 conversion, native video output

#### Concept

Instead of the ARM CPU converting ARGB8888→RGB565 and writing to DDR3, the FPGA could
do this conversion in hardware:

1. ARM writes ARGB8888 pixels to a DDR3 staging buffer (cached, fast)
2. FPGA reads ARGB8888 from staging buffer via DDR3 bus
3. FPGA converts to RGB565 in real-time during scanout
4. FPGA outputs RGB565 to video DAC/HDMI

This eliminates the CPU conversion loop entirely. The FPGA already reads from DDR3 for
the scaler — adding a format conversion stage is a small addition to existing RTL.

Alternatively, use the HPS-to-FPGA bridge DMA controller:
- ARM triggers DMA transfer from cached ARGB8888 buffer
- FPGA DMA engine reads and converts during transfer
- Zero CPU cycles spent on conversion or uncached writes

#### Precedent

The Quake II on FPGA project (https://github.com/petrmikheev/endeavour2) uses exactly
this pattern on a much weaker CPU (207MHz RISC-V, 511 DMIPS) and achieves playable
framerates. Their FPGA handles:
- 256-color → RGB565 palette mapping (hardware color lookup)
- Bilinear upscaling (hardware interpolation)
- Batched RGB565 mixing (hardware alpha blend)

#### HPS-FPGA Bridge Performance

From Cyclone V benchmarks:

| Bridge | Width | Peak Bandwidth |
|--------|-------|---------------|
| H2F (HPS-to-FPGA) | up to 128-bit | ~3.2 GB/s |
| LW H2F (Lightweight) | 32-bit | Much lower |
| F2H (FPGA-to-HPS) | up to 128-bit | ~3.2 GB/s |

Key finding: "HF128 configuration should always be used, even for 64- or 32-bit data
width peripherals" — 2-12% improvement over same-width bridges.

DMA has ~80% overhead for transfers under 2KB but is superior for transfers >256 bytes
when preparation is pre-computed. Our frame at 172KB (RGB565) or 344KB (ARGB8888) is
well above this threshold.

#### Sources

- Quake II on FPGA: https://github.com/petrmikheev/endeavour2
- Quake II FPGA blog: https://blog.mikhe.ch/quake2-on-fpga/
- Cyclone V bridge measurements: https://github.com/UviDTE-FPSoC/CycloneVSoC-time-measurements
- HPS-FPGA bridge docs: https://www.intel.com/content/www/us/en/docs/programmable/683648/current/hps-fpga-bridge-differences.html
- SpinalVoodoo (3DFX on FPGA, shows RTL rasterization is possible): https://github.com/fayalalebrun/SpinalVoodoo

---

### 3.9 Compiler Flags: Single-Precision and Fast-Math

**Status:** TODO
**Expected impact:** Low-Medium (prevents silent performance traps)
**Complexity:** Very Low (one-line CMake change)
**Relevance:** All floating-point code

#### Problem

Without `-fsingle-precision-constant`, the C compiler promotes float constants to
double:
```c
float x = y * 1.0;   // 1.0 is double → y promoted to double → result demoted to float
float x = y * 1.0f;  // 1.0f is float → no promotion
```

On Cortex-A9 VFP, double-precision operations are significantly slower than
single-precision. This silent promotion can happen throughout the codebase wherever
someone writes `0.5` instead of `0.5f`.

#### Flags

```
-fsingle-precision-constant    # treat unsuffixed float constants as float, not double
-ffast-math                     # allow reordering, no NaN/Inf checks (if precision allows)
```

`-ffast-math` is more aggressive — it enables `-fno-math-errno`, `-funsafe-math-optimizations`,
`-ffinite-math-only`, `-fno-rounding-math`, `-fno-signaling-nans`. This could break
blending precision for some edge cases. Should be tested.

Additional recommendation: always use `sinf()` not `sin()`, `fabsf()` not `fabs()`,
etc. The `f`-suffixed versions avoid implicit double promotion.

#### Implementation Plan

1. Add `-fsingle-precision-constant` to CMake flags (safe, no behavior change for
   correct code)
2. Grep for `sin(`, `cos(`, `fabs(`, `sqrt(` without `f` suffix — replace with
   `f`-suffixed versions
3. Test `-ffast-math` on a separate branch, compare visual output frame-by-frame
4. If `-ffast-math` causes artifacts, try individual sub-flags (`-fno-math-errno`,
   `-funsafe-math-optimizations`) separately

#### Sources

- GCC ARM optimization flags: https://gist.github.com/fm4dd/c663217935dc17f0fc73c9c81b0aa845
- ACGC MiSTer feasibility doc, Section A.8.4 (Recommended Compiler Flags)
- Pandora float optimization guide: https://pandorawiki.org/Floating_Point_Optimization

---

### 3.10 Render to Cached Buffer, Bulk Copy to Uncached DDR3

**Status:** TODO (verify current implementation)
**Expected impact:** High if not already done; None if already done
**Complexity:** Low
**Relevance:** Native video output path

#### Technique

All game rendering should happen in normal `malloc`'d (cached) memory. The uncached
DDR3 write should happen exactly once per frame as a bulk `memcpy`:

```c
// GOOD: Render entirely in cached memory, single bulk copy at end
uint16_t cached_frame[384 * 224];           // in normal heap (L1/L2 cached)
render_game_to(cached_frame);                // all work in cache
memcpy(uncached_ddr3_buffer, cached_frame, sizeof(cached_frame));  // one uncached write
```

```c
// BAD: Rendering directly to uncached memory
uint16_t *frame = (uint16_t *)mmap_uncached_ddr3;
render_game_to(frame);  // every pixel write is an uncached bus transaction
```

From Cyclone V benchmarks: write operations with caches ON are **10.9x faster** than
caches OFF; read operations are **4.4x faster**.

#### Current State

The `native_video_rgb565_scratch[384 * 224]` buffer in `sdl_app.c` appears to be a
cached scratch buffer. Need to verify that:
1. All rendering happens into cached memory
2. The RGB565 conversion writes to the cached scratch buffer
3. Only the final copy to DDR3 touches uncached memory
4. No reads from the uncached DDR3 mapping occur (reads from WC/uncached memory are
   catastrophically slow)

#### Sources

- Cyclone V SoC time measurements: https://github.com/UviDTE-FPSoC/CycloneVSoC-time-measurements
- Fabian Giesen — Write-combining: https://fgiesen.wordpress.com/2013/01/29/write-combining-is-not-your-friend/
  - Key rule: **Never read from write-combined memory**

---

### 3.11 Cache-Line-Aligned Bulk Copy with PLD Prefetch

**Status:** TODO
**Expected impact:** Low-Medium
**Complexity:** Low
**Relevance:** Final frame copy to DDR3, fbdev presenter copies

#### Technique

For the bulk copy from cached buffer to DDR3, optimal approach on Cortex-A9:

1. **Align source buffer** to 32-byte boundary (cache line size)
2. **PLD (prefetch)** 3-6 cache lines ahead (96-192 bytes)
3. **LDM/STM** for 32-byte (cache line) aligned copies
4. **Never read** from the destination (uncached read forces WC buffer flush)

```c
// Pseudo-code for optimal A9 bulk copy to uncached destination
void bulk_copy_to_uncached(void *dst, const void *src, size_t len) {
    // Align to cache line
    // PLD 3 lines ahead
    // LDM 8 registers (32 bytes) from src
    // STM 8 registers (32 bytes) to dst
    // Repeat
}
```

On Cortex-A9, do NOT use NEON for memcpy — use PLD + LDM/STM instead. NEON memcpy
provides no speedup on A9 (confirmed by multiple sources including ACGC research and
ARM community documentation).

#### Sources

- ACGC MiSTer feasibility doc, Section A.8.2 ("NEON is NOT faster for memcpy on A9")
- fastarm implementation: https://github.com/hglm/fastarm
- Raspberry Pi framebuffer optimization: https://forums.raspberrypi.com/viewtopic.php?t=213964

---

### 3.12 PMU-Backed Performance Counter Measurement

**Status:** TODO
**Expected impact:** Diagnostic (enables targeted optimization)
**Complexity:** Medium
**Relevance:** All optimization decisions

#### Problem

Current profiling uses wall-clock timers (`clock_gettime`). This tells us *how long*
things take but not *why*. The Cortex-A9 Performance Monitoring Unit (PMU) provides
hardware counters for:

- L1 data cache misses
- L2 cache misses
- L1 data cache accesses
- Instructions retired
- Branch mispredictions
- Data-dependent stalls (pipeline interlocks)
- NEON/VFP pipeline stalls
- External memory requests

#### Technique

Enable PMU access from userspace (normally requires kernel module or `perf`):

```c
// Enable user-mode PMU access (requires kernel config or module)
// Read cycle counter:
uint32_t cycles;
asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(cycles));

// Read specific event counter:
// First configure PMNx to count desired event, then read
```

Alternatively, use Linux `perf_event_open()` syscall:
```c
struct perf_event_attr pe = {
    .type = PERF_TYPE_HARDWARE,
    .config = PERF_COUNT_HW_CACHE_MISSES,
    .disabled = 1,
    .exclude_kernel = 1,
};
int fd = perf_event_open(&pe, 0, -1, -1, 0);
```

#### What to Measure

For the Genei-Jin bottleneck:
1. L1 data cache miss rate during the non-integer gather loop
2. L2 cache miss rate (determines if data is in L2 or going to DDR3)
3. Instructions per cycle (IPC) — if low, pipeline stalls dominate
4. Data-dependent stall count — proves whether gather-induced stalls are the wall

This data would definitively answer whether the bottleneck is:
- **L1 miss → L2 hit** (23 cycle penalty, addressable with prefetch/swizzling)
- **L2 miss → DDR3** (110+ cycle penalty, harder to fix)
- **Pipeline interlock** (data dependency stall, not cache related)

#### Sources

- ARM Cortex-A9 Technical Reference Manual — PMU chapter
- Cortex-A9 cache/perf data: https://www.7-cpu.com/cpu/Cortex-A9.html
- ACGC MiSTer feasibility doc, Section A.8.1 (Cache Hierarchy)

---

### 3.13 Per-Surface Alpha Sidecar Bitmasks

**Status:** TODO
**Expected impact:** Medium (skip transparent regions without reading pixels)
**Complexity:** Medium
**Relevance:** Non-integer scaling gather loop, Genei-Jin sprites

#### Concept

Cache a compact 1-bit alpha mask alongside each 256x256 ARGB surface in the texture
cache. Each bit indicates whether the corresponding pixel is fully transparent (alpha=0)
or not.

For a 256x256 surface: 256×256 / 8 = 8KB per surface. This fits in L1 cache (32KB)
alongside the working set.

Before entering the expensive gather loop for a sub-rect, check the alpha sidecar:
- If the entire sub-rect is transparent → skip entirely (zero cost)
- If the entire sub-rect is opaque → use fast opaque path (no blending)
- If mixed → fall through to existing gather loop

#### Why Per-Surface Not Per-Row

Previous attempts at whole-row opaque masks (64 words per texture/palette) failed
because the granularity was too coarse for 16-32px sub-rects. Per-surface 1-bit masks
give pixel-level granularity while remaining compact enough for L1 residence.

#### Implementation Plan

1. Add `uint8_t alpha_mask[256 * 256 / 8]` to texture cache entry
2. Populate during texture cache refresh (bitwise OR of alpha channel)
3. Before gather loop: check alpha bits for the source sub-rect bounds
4. If all zero → skip. If all one → opaque fast path. Else → existing path.

#### Risks

- 8KB per surface × number of active surfaces could pressure L2
- Sidecar population adds cost to texture cache refresh
- Net benefit depends on how often sub-rects are fully transparent or fully opaque
- Needs telemetry to determine alpha patterns in Genei-Jin sprites first

#### Sources

- 3sx internal research: `docs/agent-memory/mister-perf-deep-research-2026-03-21.md`
  (listed as open research direction)

---

### 3.14 Clustered-Shape Specialization for Genei-Jin Sprites

**Status:** TODO
**Expected impact:** Medium-High (targets the specific hot shapes)
**Complexity:** Medium
**Relevance:** Top 4 sprite families consuming 30ms of 78ms in Genei-Jin

#### Concept

Instead of generic per-signature optimization, focus exclusively on the 32x32→34-37
sprite shape cohort that dominates Genei-Jin. These shapes have specific properties:

- Known source size (32x32 pixels)
- Known destination size range (34-37 pixels wide)
- Known scaling factor (~1.06-1.16x)
- The lookup table for this narrow range has predictable patterns

Create a **specialized fast path** for this shape family:
1. Detect 32x32→34-37 shape at task submission time
2. Pre-compute the lookup table once (it's deterministic for each scale factor)
3. Use a hand-optimized loop that knows the exact dimensions
4. Potentially unroll the inner loop completely (34-37 iterations is small enough)

#### Why Previous Unrolling Failed

Previous 4x scalar unrolling (Loop 135) showed no gain because it was generic — it
didn't know the exact loop count or lookup pattern. A specialized version for exactly
34-37 iterations with a known lookup pattern could perform differently.

#### Sources

- 3sx internal research: `docs/agent-memory/mister-perf-deep-research-2026-03-21.md`
- 3sx internal research: `docs/agent-memory/mister-geneijin-rendering.md`

---

### 3.15 DFPSR NEON Rendering Abstraction

**Status:** TODO
**Expected impact:** Low (code patterns, not direct integration)
**Complexity:** Low (study and adapt)
**Relevance:** Reference for NEON rendering patterns

#### Description

DFPSR (David's Faster Platform-independent Software Renderer) provides an SSE/AVX/NEON
abstraction layer via `simd.h`. Unlike raw intrinsics, it provides portable SIMD
primitives that compile to optimal code on each platform.

Useful as a **pattern reference** for:
- How to structure NEON rendering loops
- How to handle alignment and tail elements
- How to abstract SIMD operations for readability while maintaining performance

Not recommended for direct integration (dependency overhead), but the source code is
a valuable reference.

#### Source

- DFPSR: https://github.com/Dawoodoz/DFPSR

---

### 3.16 sse2neon for Porting Optimized Routines

**Status:** TODO
**Expected impact:** Variable (depends on what gets ported)
**Complexity:** Low (drop-in header)
**Relevance:** If porting SSE-optimized code from other projects

#### Description

`sse2neon` is a header-only library providing drop-in NEON replacements for SSE/SSE2
intrinsics. Some map 1:1 (e.g., `_mm_loadu_si128` → `vld1q_s32`), but others require
13+ NEON instructions and leave performance on the table vs native NEON.

Useful if porting optimized routines from:
- PPSSPP's software renderer (SSE-optimized pixel processing)
- Other x86-optimized software renderers

PPSSPP's SSE→NEON conversion (PR #16753) showed **~10% FPS improvement** on ARM from
explicit NEON intrinsics over compiler auto-vectorization, confirming that hand-written
NEON does matter.

#### Sources

- sse2neon: https://github.com/DLTcollab/sse2neon
- PPSSPP NEON renderer PR: https://github.com/hrydgard/ppsspp/pull/16753

---

### 3.17 Auto-Vectorization Compiler Flag

**Status:** TODO
**Expected impact:** Low (compiler may already be doing this with -O3)
**Complexity:** Very Low
**Relevance:** All hot loops

#### Flag

```
-mvectorize-with-neon-quad
```

Enables GCC/Clang auto-vectorization using 128-bit NEON quad-word registers. With `-O3`
alone, the compiler may auto-vectorize with 64-bit D registers only. This flag allows
it to use full Q registers.

Note: `-O3` already implies `-ftree-vectorize`. The additional flag just unlocks Q
register usage.

#### Caveat

Auto-vectorization is fragile. Small changes to loop structure can prevent it. For
critical hot paths, explicit NEON intrinsics (items 3.3, 3.5, 3.7) are more reliable.
This flag is a low-effort supplement, not a replacement.

#### Sources

- GCC ARM flags: https://gist.github.com/fm4dd/c663217935dc17f0fc73c9c81b0aa845
- ACGC MiSTer feasibility doc, Section A.8.4

---

### 3.18 Separate Code and Data Pages (Minimig Lesson)

**Status:** TODO
**Expected impact:** Unknown (diagnostic investigation)
**Complexity:** Low (investigation only)
**Relevance:** General performance hygiene

#### Problem

The Minimig Hybrid project found that memory page sharing between code and data
"kills performance" on the Cyclone V SoC. When hot code and hot data map to the same
cache sets, they evict each other continuously.

On Cortex-A9 with 4-way associative 32KB L1 data cache:
- Cache has 256 sets (32KB / 32-byte lines / 4 ways)
- Two addresses conflict if they share the same set index (bits 5-12 of address)
- If a hot data array and a hot code section happen to alias to the same sets, every
  instruction fetch evicts data and vice versa

#### Investigation Plan

1. On MiSTer, examine `/proc/<pid>/maps` to see where code and data sections are mapped
2. Check if any hot data arrays (texture cache, scratch buffers) alias with the `.text`
   section in L1 cache
3. If aliasing found, consider `posix_memalign` with offset to shift data to non-
   conflicting cache sets

#### Sources

- Minimig Hybrid: https://github.com/scrameta/Minimig-AGA_MiSTer_Hybrid
- Minimig Hybrid Support (AllocP utility): https://github.com/scrameta/MiSTer_Hybrid_Support

---

### 3.19 RGB565 Framebuffer for 2x Bandwidth Reduction

**Status:** DONE (already implemented in native video path)
**Expected impact:** Already realized
**Relevance:** Documented for completeness

The MiSTer community confirms: "1555 or 565 color format gives twice the performance
boost" over 32-bit formats. At 384x224 RGB565 = 172KB per frame, the entire frame fits
within the 512KB L2 cache.

The `native_video_rgb565_scratch` buffer and RGB565 conversion in `sdl_app.c` already
implement this.

#### Sources

- MiSTer documentation: https://mister-devel.github.io/MkDocs_MiSTer/developer/emu/
- ScummVM MiSTer port uses `vmode -r 640 480 rgb16` for this reason

---

## 4. Cortex-A9 Hardware Reference

Quick reference for optimization decisions.

### 4.1 Cache Hierarchy

| Level | Size | Line Size | Latency | Associativity |
|-------|------|-----------|---------|---------------|
| L1 Data | 32 KB | 32 bytes | 4 cycles | 4-way |
| L1 Instruction | 32 KB | 32 bytes | 4 cycles | 4-way |
| L2 Shared | 512 KB | 32 bytes | ~23 cycles (L1+L2 total) | — |
| DDR3 | 1 GB (512 MB to Linux) | — | ~110+ cycles | — |

### 4.2 Memory Bandwidth

| Operation | Bandwidth |
|-----------|-----------|
| L2 read | 7 cycles per cache line (32 bytes) |
| L2 write | ~11.5 cycles per cache line |
| RAM sequential read | ~890-1010 MB/s |
| RAM sequential write | ~1600 MB/s |
| RAM random parallel read | ~470 MB/s |
| RAM 32-byte step write | ~725 MB/s |

### 4.3 Store Buffer

- 4-entry, 64-bit merging store buffer
- Sequential L2 writes: 1 cycle per 4 bytes
- 32-byte step writes: 11.5 cycles per cache line

### 4.4 Pipeline

- 9-stage integer pipeline (8-stage in some docs)
- Dual-issue capability
- Partial out-of-order execution
- Branch misprediction: 11-cycle penalty

### 4.5 NEON Specifics

- **Datapath:** 64-bit wide — 128-bit Q register ops take 2 cycles
- **Register file:** 32x 64-bit D registers / 16x 128-bit Q registers
- **Up to 16 ops per instruction** (sixteen 8-bit integer ops in Q register)
- **ARM↔NEON transfer:** >20 cycles (keep data in NEON throughout pixel loops)
- **NEON memcpy:** NOT faster than PLD+LDM/STM on A9 (unlike A8 or A15)
- **VADD latency:** 6 cycles writeback
- **VMUL latency:** 5 execution cycles
- **VMLA (float):** 9 cycles — often worse than separate VMUL+VADD with pipeline fill
- **VMLA (integer):** efficient — prefer for color math
- **VFP:** Scalar VFP matches NEON scalar — NEON advantage is purely vectorization

### 4.6 Working Set Sizes

| Buffer | Size | Fits In |
|--------|------|---------|
| One frame RGB565 (384x224) | 172 KB | L2 (512 KB) |
| One frame ARGB8888 (384x224) | 344 KB | L2 (512 KB) |
| Alpha sidecar (256x256 / 8) | 8 KB | L1 (32 KB) |
| Morton lookup table (256x256) | 128 KB | L2 |
| 32x32 sprite source region | 4 KB (ARGB) / 2 KB (565) | L1 |
| 16x16 dirty tile state | ~1.3 KB | L1 |

Source: https://www.7-cpu.com/cpu/Cortex-A9.html

---

## 5. MiSTer Framebuffer Architecture Reference

### 5.1 Memory Map

| Region | Address | Size | Purpose |
|--------|---------|------|---------|
| DDR3 base (Linux) | `0x00000000` | 512 MB | Linux kernel + userspace |
| Scaler triple-buffer | `0x20000000` | 3 × 8 MB | FPGA scaler reads from here |
| Core framebuffer | `0x20000000 + 24 MB` | 8 MB | Core-generated video |
| Linux FB (Main_MiSTer) | `0x22000000` | ~2 MB | ARM-rendered framebuffer |
| 3SX native video buffer | `0x3A000000` | 384 KB | Custom DDR3 region |

### 5.2 Framebuffer Format Flags

From `video.cpp` in Main_MiSTer:

```
FB_FMT_565  = 0b00100   // 16bpp RGB 5:6:5 — RECOMMENDED
FB_FMT_1555 = 0b01100   // 16bpp ARGB 1:5:5:5
FB_FMT_888  = 0b00101   // 24bpp RGB — "extremely low performance"
FB_FMT_8888 = 0b00110   // 32bpp ARGB
FB_FMT_PAL8 = 0b00011   // 8bpp palette
```

### 5.3 Scaler Architecture

- 128-bit wide DDR3 data bus
- 256-byte burst transfers
- Bandwidth: >1000 MB/s available
- Original-resolution images stored in DDR3, upscaling done on-the-fly by FPGA
- Triple-buffered at the scaler level
- Framebuffer mapped as write-through (`MEMREMAP_WT`), stride is 256-byte aligned
- Resolution set from shell: `vmode -r 640 480 rgb16`

### 5.4 /dev/fb0 Usage

```c
// Standard fbdev API:
int fb = open("/dev/fb0", O_RDWR);
struct fb_var_screeninfo vinfo;
ioctl(fb, FBIOGET_VSCREENINFO, &vinfo);
void *fbmem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
// Write pixels...
ioctl(fb, FBIO_WAITFORVSYNC, &zero);  // vsync
```

Mode parameters: `/sys/module/MiSTer_fb/parameters/mode` as `"format rb width height stride"`

### 5.5 SDL2 on MiSTer

- SDL2 **removed** the `fbcon` video driver entirely
- Stock MiSTer has no X11, Wayland, or KMS/DRM libraries
- All successful MiSTer Linux game ports use either:
  1. SDL 1.2 with native fbdev support (PrBoom, ScummVM, OpenBOR)
  2. SDL2 with `SDL_VIDEODRIVER=dummy` + direct fbdev/DDR3 writes (3sx approach)
- OpenBOR explicitly states: "compatibility with MiSTer video framebuffer via SDL2 is
  not possible"

Sources:
- MiSTer kernel driver: https://github.com/TinyRetroWarehouse/MiSTer_linux
- Main_MiSTer video.cpp: https://github.com/MiSTer-devel/Main_MiSTer/blob/master/video.cpp
- MiSTer EMU docs: https://mister-devel.github.io/MkDocs_MiSTer/developer/emu/
- MiSTer video config: https://mister-devel.github.io/MkDocs_MiSTer/basics/video/

---

## 6. Master Source Index

### 6.1 MiSTer ARM Software Ports

| Project | URL |
|---------|-----|
| PrBoom-Plus (Doom) | https://github.com/bbond007/MiSTer_PrBoom-Plus |
| ScummVM | https://github.com/bbond007/MiSTer_ScummVM |
| DOSBox | https://github.com/bbond007/MiSTer_DOSBox |
| Basilisk II | https://github.com/bbond007/MiSTer_BasiliskII |
| OpenBOR | https://github.com/SumolX/MiSTer_OpenBOR |
| NXEngine (Cave Story) | https://github.com/EXL/NXEngine |
| Dethrace (Carmageddon) | https://github.com/dethrace-labs/dethrace |
| Minimig Hybrid | https://github.com/scrameta/Minimig-AGA_MiSTer_Hybrid |
| Minimig Hybrid Support | https://github.com/scrameta/MiSTer_Hybrid_Support |
| MiSTer Overclock Scripts | https://github.com/coolbho3k/MiSTer-Overclock-Scripts |

### 6.2 Software Rasterizer References

| Project | URL | Notes |
|---------|-----|-------|
| OpenLara (Tomb Raider) | https://github.com/XProger/OpenLara | GBA ARM asm rasterizer |
| EmberGL | https://github.com/EmberGL-org/EmberGL | Tile-based deferred, MCU-targeted |
| DFPSR | https://github.com/Dawoodoz/DFPSR | SSE/AVX/NEON abstraction |
| TinyGL (C-Chads) | https://github.com/C-Chads/tinygl | OpenGL 1.1 subset |
| TinyGL (Cortex-A9) | https://github.com/jonmcdonald/VistaModels/tree/master/ARM/a9/software/tinygl | ARM A9 build |
| TinyGL (jserv fork) | https://github.com/jserv/tinygl | OpenMP, alignment fixes |
| small3dlib | https://github.com/viteo/small3dlib | Integer-only, bare-metal |
| krzosa SIMD rasterizer | https://github.com/krzosa/software_rasterizer | Half-space, 8-wide AVX2 |
| PPSSPP NEON PR | https://github.com/hrydgard/ppsspp/pull/16753 | SSE→NEON conversion |

### 6.3 Dolphin Software Video Backend

| File | URL |
|------|-----|
| Tev.cpp | https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/Software/Tev.cpp |
| Rasterizer.cpp | https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/Software/Rasterizer.cpp |
| TextureSampler.cpp | https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/Software/TextureSampler.cpp |
| EfbInterface.cpp | https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/Software/EfbInterface.cpp |
| Dolphin SW renderer blog | https://dolphin-emu.org/blog/2014/03/15/pixel-processing-problems/ |

### 6.4 NEON / ARM Optimization

| Resource | URL |
|----------|-----|
| sse2neon | https://github.com/DLTcollab/sse2neon |
| fastarm (optimized memcpy) | https://github.com/hglm/fastarm |
| Pixman NEON assembly | https://github.com/servo/pixman/blob/master/pixman/pixman-arm-neon-asm.S |
| Pixman NEON asm macros | https://github.com/servo/pixman/blob/master/pixman/pixman-arm-neon-asm.h |
| NEON alpha blending | https://github.com/akulkar4/AlphaBlend |
| Cortex-A9 perf data | https://www.7-cpu.com/cpu/Cortex-A9.html |
| Pandora float optimization | https://pandorawiki.org/Floating_Point_Optimization |
| ARM NEON DirectFB blog | https://developer.arm.com/community/arm-community-blogs/b/tools-software-ides-blog/posts/optimizing-directfb-with-arm-neon |
| ARM Cortex-A9 NEON TRM | https://developer.arm.com/documentation/ddi0409/latest/ |
| GCC ARM optimization flags | https://gist.github.com/fm4dd/c663217935dc17f0fc73c9c81b0aa845 |

### 6.5 FPGA / DMA / Bridge

| Resource | URL |
|----------|-----|
| Quake II on FPGA (DMA rendering) | https://github.com/petrmikheev/endeavour2 |
| Quake II FPGA blog | https://blog.mikhe.ch/quake2-on-fpga/ |
| SpinalVoodoo (3DFX on FPGA) | https://github.com/fayalalebrun/SpinalVoodoo |
| Cyclone V bridge measurements | https://github.com/UviDTE-FPSoC/CycloneVSoC-time-measurements |
| Cyclone V FPGA design guidelines | https://www.intel.com/content/www/us/en/programmable/documentation/doq1481305867183.html |
| HPS-FPGA bridge docs | https://www.intel.com/content/www/us/en/docs/programmable/683648/current/hps-fpga-bridge-differences.html |
| udmabuf DMA buffer driver | https://github.com/ikwzm/udmabuf |

### 6.6 Architecture / Theory

| Resource | URL |
|----------|-----|
| Fabian Giesen — Rasterizer optimization | https://fgiesen.wordpress.com/2013/02/10/optimizing-the-basic-rasterizer/ |
| Fabian Giesen — Texture tiling/swizzling | https://fgiesen.wordpress.com/2011/01/17/texture-tiling-and-swizzling/ |
| Fabian Giesen — Write-combining | https://fgiesen.wordpress.com/2013/01/29/write-combining-is-not-your-friend/ |
| Larrabee rasterization (Abrash) | https://www.cs.cmu.edu/afs/cs/academic/class/15869-f11/www/readings/abrash09_lrbrast.pdf |
| Tile-based rasterizer (Kayhan) | https://tayfunkayhan.wordpress.com/2019/07/26/chasing-triangles-in-a-tile-based-rasterizer/ |
| GameCube architecture (Copetti) | https://www.copetti.org/writings/consoles/gamecube/ |

### 6.7 MiSTer Platform

| Resource | URL |
|----------|-----|
| MiSTer Wiki | https://github.com/MiSTer-devel/Wiki_MiSTer/wiki |
| MiSTer Linux Kernel | https://github.com/MiSTer-devel/Linux-Kernel_MiSTer |
| MiSTer kernel FB driver | https://github.com/TinyRetroWarehouse/MiSTer_linux |
| Main_MiSTer source | https://github.com/MiSTer-devel/Main_MiSTer |
| MiSTer EMU module docs | https://mister-devel.github.io/MkDocs_MiSTer/developer/emu/ |
| MiSTer video config | https://mister-devel.github.io/MkDocs_MiSTer/basics/video/ |
| MiSTer INI settings | https://mister-devel.github.io/MkDocs_MiSTer/advanced/ini/ |
| MiSTer compiling docs | https://mister-devel.github.io/MkDocs_MiSTer/developer/mistercompile/ |
| MiSTer Desktop Linux wiki | https://github.com/MiSTer-devel/Main_MiSTer/wiki/Desktop-Linux |
| MiSTer Toolchain Docker | https://github.com/raetro-archives/Toolchain_MiSTer |
| MiSTerArch (Arch Linux ARM) | https://github.com/MiSTerArch/PKGBUILDs |
| DE10-Nano specs | https://www.terasic.com.tw/cgi-bin/page/archive.pl?Language=English&CategoryNo=167&No=1046 |

### 6.8 MiSTer Forum Threads

| Topic | URL |
|-------|-----|
| Linux Games | https://misterfpga.org/viewtopic.php?t=1238 |
| Hardware Rendering | https://misterfpga.org/viewtopic.php?t=5871 |
| Hybrid Emulation | https://misterfpga.org/viewtopic.php?t=2397 |
| JFDuke3D | https://misterfpga.org/viewtopic.php?t=8555 |
| Dethrace (Carmageddon) | https://misterfpga.org/viewtopic.php?p=98014 |

### 6.9 Cross-Project References

| Resource | Path |
|----------|------|
| ACGC MiSTer feasibility study | `~/Developer/ACGC-PC-Port/docs/agent-memory/mister-port-feasibility.md` |
| ACGC Phase 0+1 plan | `~/Developer/ACGC-PC-Port/docs/agent-memory/mister-phase0-phase1-plan.md` |
| 3SX perf deep research | `docs/agent-memory/mister-perf-deep-research-2026-03-21.md` |
| 3SX Genei-Jin rendering | `docs/agent-memory/mister-geneijin-rendering.md` |
| 3SX perf opportunities | `docs/agent-memory/mister-perf-opportunities.md` |
