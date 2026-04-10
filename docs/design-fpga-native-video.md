# Design: FPGA Native Video Output for ARM-Rendered Content

## Status: Design Phase

## Problem Statement

3S-ARM runs on the MiSTer ARM CPU (Cortex-A9) and renders frames in software at 384x224 (NTSC, 59.59949 Hz). Currently, frames are presented via the Linux framebuffer (`/dev/fb0`), which the FPGA scaler (ascal) reads asynchronously from DDR3 and outputs to the display.

This architecture has two problems on CRT:

1. **Latency**: The scaler's asynchronous triple-buffer pipeline adds 1-2 frames (~16-33ms) of display latency. Native FPGA cores avoid this entirely on analog output.
2. **Performance overhead**: The ARM must software-scale frames from 384x224 to the Linux framebuffer resolution (e.g., 640x240/480) using per-pixel LUT lookups against uncached DDR3, costing ~0.5-2ms per frame on a tight 16.7ms budget.

## Proposed Solution

Add an FPGA-side module to the Menu core that reads ARM-written pixel data directly from DDR3 and outputs it as native core video (`VGA_R/G/B/HS/VS/DE` on `clk_vid`). This bypasses the scaler entirely for analog output, matching the video path used by every native MiSTer FPGA core (Genesis, SNES, etc.).

The ARM game writes frames to a known DDR3 address. The FPGA reads them and outputs video at native CPS3 timing. From the MiSTer framework's perspective, this is just a core generating its own video — the fact that the pixels originated on the ARM is invisible.

## Reference Implementation: Groovy MiSTer

The Groovy MiSTer core (psakhis/Groovy_MiSTer) solves an analogous problem: displaying externally-sourced pixel data through the FPGA's native video path. It receives frames over Ethernet from a PC, writes them to DDR3, and outputs them as core video. Key components:

- **vga.v** (940 lines): Video timing generator with 6 round-robin FIFO VRAMs (18 dcfifo instances, 16384 entries each). Reads 1 pixel per `ce_pix` cycle, outputs hsync/vsync/DE from modeline parameters.
- **ddram.sv** (123 lines): DDR3 reader wrapper. 64-bit Avalon-MM interface, base address `0x1C000000`, burst up to 128 beats (1024 bytes).
- **pll.v**: Fractional-N PLL with runtime reconfiguration via Altera PLL Reconfig IP. Outputs arbitrary pixel clocks from 50MHz reference.
- **Groovy.sv**: State machine that reads pixel data from DDR3 in bursts, decodes RGB888/RGBA8888/RGB565, and pushes pixels into FIFO VRAMs.
- **hps_ext.v**: ARM-to-FPGA command interface via `EXT_BUS[35:0]`. Commands for blit triggers, modeline changes, status polling.

Most of Groovy's complexity is irrelevant to our use case (UDP networking, LZ4 hardware decompressor, 4-zone compressed buffers, dynamic modeline switching, interlaced field handling). We need roughly 20% of its video pipeline.

## Current Architecture (What We're Replacing)

### Current Pixel Path
```
Game (ARM) -> SDL3 software renderer -> SDL_Surface (384x224 ARGB8888)
    -> fbdev_presenter -> /dev/fb0 (mmap'd DDR3 @ 0x22000000)
        -> ascal reads DDR3 asynchronously on clk_hdmi
            -> yc_out_fb encodes S-Video/composite (or RGB direct)
                -> DAC -> CRT
```

### Key Files (Current)
| File | Purpose |
|------|---------|
| `src/port/sdl/fbdev_presenter.c` | Copies rendered frames to `/dev/fb0` with scaling, dirty tracking, NEON |
| `src/port/sdl/sdl_app.c` | Frame loop, timer-based pacing (`SDL_DelayNS`), presentation dispatch |
| `vendor/Main_MiSTer/video.cpp` | Configures FPGA framebuffer via `UIO_SET_FBUF`, manages scaler |
| `vendor/Main_MiSTer/thirdsarm_wrapper.cpp` | Wrapper process, OSD, ARM clock control |
| `vendor/Menu_MiSTer/sys/sys_top.v` | FPGA top-level: DAC mux, clock selection, scaler instantiation |
| `vendor/Menu_MiSTer/menu.sv` | Menu core: fixed PLL, basic video output, DDRAM interface |

### Why the Scaler Is Always In the Loop

The DAC output mux in `sys_top.v` (lines ~1537-1539):
```verilog
assign VGA_R = vga_fb_yc_en ? yc_fb_o[23:18] :
               vgas_en       ? vgas_o[23:18] :
               vga_o[23:18];
```

When `vga_fb=1` (ARM framebuffer active), `vgas_en` is true, selecting the scaler output (`vgas_o`). The core's own video (`vga_o`) is never reached. The clock also switches from `clk_vid` (core) to `clk_hdmi` (scaler):
```verilog
cyclonev_clkselect vga_clk_sw(
    .clkselect({1'b1, ~vga_fb & ~vga_scaler}),
    .inclk({clk_vid, hdmi_clk_out, 2'b00}),
    .outclk(vga_tx_clk)
);
```

### Current Presentation Paths (fbdev_presenter.c)

The presenter has multiple paths ordered by preference:
1. **SOFTWARE_FRAME_EXACT**: Zero-copy when game surface matches fb dimensions exactly (384==fb_width). Rare — fb is usually larger.
2. **SOFTWARE_FRAME_INTEGER_SCALE**: Integer-multiple upscale with LUT.
3. **SOFTWARE_FRAME_MAPPED_SCALE**: Non-integer LUT-based per-pixel mapping.
4. **FULLSCREEN_DIRECT_COPY**: Direct memcpy when src matches fb size.
5. **FULLSCREEN_SCALED_LUT**: Per-pixel LUT scaling for arbitrary fb size. Most common fallback.
6. **CURRENT_TARGET_MAPPED_SCALE**: Readback from SDL renderer + scaled copy.

All paths write to uncached mmap'd DDR3 via `/dev/fb0`. The LUT scaling paths are the most CPU-intensive.

### Current Frame Pacing
```c
// Timer-based, ~1-4ms jitter on ARM Linux
if (now < frame_deadline) {
    SDL_DelayNS(sleep_time);
}
frame_deadline += target_frame_time_ns;  // 1e9 / 59.59949
```

No hardware vsync — the ARM and scaler run asynchronously.

## Proposed Architecture

### New Pixel Path
```
Game (ARM) -> SDL3 software renderer -> SDL_Surface (384x224)
    -> Direct DDR3 write (mmap @ 0x3A000000, memcpy or NEON copy)
        -> FPGA pixel reader reads DDR3 in bursts
            -> FIFO line buffers
                -> Video timing generator (384x224 @ 59.59949Hz)
                    -> VGA_R/G/B/HS/VS/DE on clk_vid
                        -> DAC -> CRT (zero scaler)
```

### Key Architectural Decisions

**1. Don't set `vga_fb=1`**

The core outputs video as its own `VGA_R/G/B` signals. The DAC mux selects `vga_o` (core video path). The scaler is not involved. The clock stays on `clk_vid`. This is the same path every native MiSTer core uses for analog output.

**2. Fixed modeline, not dynamic**

Groovy supports arbitrary runtime modeline switching. We have one target: 384x224 @ 59.59949Hz NTSC. The PLL parameters and timing counters can be fixed at compile time (or configured once at init and never changed). This dramatically simplifies the design.

**3. Double-buffer with flag word in DDR3**

The ARM writes a complete frame to one of two DDR3 buffers, then writes a flag word indicating which buffer is ready. The FPGA polls this flag and switches read address. Simple, no SPI command needed per frame.

Alternative: SPI command per frame (like Groovy's `SET_BLIT`). More explicit but adds ARM-side SPI overhead.

**4. HDMI fallback via scaler**

When `vga_fb` is not set, HDMI output still works via the standard scaler path (ascal reads the core's video output from the `video_mixer` output). CRT users get the direct analog path; HDMI users get the scaler path as usual. No regression.

**5. RGB565 pixel format**

384x224x2 = 172KB per frame vs 344KB for ARGB8888. Halves the DDR3 write bandwidth from the ARM and the DDR3 read bandwidth on the FPGA side. RGB565 is sufficient for CPS3's color depth. The FPGA reader expands to 8-bit per channel for the DAC.

## FPGA Modules Needed

### 1. DDR3 Pixel Reader

Reads pixel data from a known DDR3 address in bursts and pushes decoded pixels into line FIFOs.

**Interface**: Uses the existing DDRAM Avalon-MM interface already available in `menu.sv` (lines 124-133). 64-bit data bus, burst support.

**DDR3 memory layout** (relative to base address):
```
0x000000: Control word (4 bytes)
          [1:0]   active_buffer (0 or 1)
          [31:2]  frame_counter
0x000100: Buffer 0 (384 * 224 * 2 = 172,032 bytes, RGB565)
0x02A200: Buffer 1 (172,032 bytes, RGB565)
```

**State machine**:
1. Poll control word from DDR3 (1 read)
2. If new frame detected (frame_counter changed): switch to new buffer address
3. At start of each visible line: burst-read one scanline (384 pixels * 2 bytes = 768 bytes = 12 bursts of 64 bytes)
4. Decode RGB565 to 8-bit RGB and push into line FIFO

**Bandwidth**: 172KB * 60fps = ~10.3MB/s. DDR3 interface can do several hundred MB/s. No contention concern.

### 2. Line FIFO Buffers

Dual-clock FIFO (Altera `dcfifo`) bridging DDR3 read clock to pixel output clock.

**Sizing**: Groovy uses 6 FIFOs x 16384 entries for 720x576. For 384x224 we need far less. Two FIFOs of 512 entries each (double-buffered scanlines) would suffice. One is being written (from DDR3), the other is being read (to DAC).

**Write side**: Clocked by `clk_sys` (or DDR3 interface clock). Receives decoded pixels from the reader.
**Read side**: Clocked by pixel clock (or `clk_sys` with `ce_pix` enable). Outputs one pixel per `ce_pix`.

### 3. Video Timing Generator

Generates hsync, vsync, hblank, vblank, DE from fixed modeline parameters.

**Target modeline** (384x224 @ 59.59949Hz, CPS3 NTSC):
```
Horizontal: 384 active + HFP + HS + HBP = total (TBD: derive from CPS3 specs or calculate for 15kHz)
Vertical:   224 active + VFP + VS + VBP = total
Pixel clock: H_total * V_total * 59.59949 Hz
```

Exact values need to be calculated. For reference, typical 15kHz NTSC timings:
- H total ~512 pixels (including blanking), V total ~262 lines
- Pixel clock = 512 * 262 * 59.94 ~= 8.05 MHz (approximate, needs exact CPS3 values)

**Implementation**: Two counters (h_cnt, v_cnt), combinational logic for sync/blank/DE. ~80 lines of Verilog. Reference: Groovy's `vga.v` lines 664-741, but stripped down to fixed parameters.

### 4. PLL Configuration

**Option A (simpler)**: Calculate the M, C, K fractional PLL parameters for the target pixel clock at compile time. Set them in the `.qsf` or PLL IP configuration. No runtime reconfiguration needed.

**Option B (more flexible)**: Use Altera PLL Reconfig IP (as Groovy does) to program the PLL at init time via SPI command from the ARM. Allows the ARM to specify exact timing. More complex but future-proof.

Recommend Option A for initial implementation, with Option B as a future enhancement if needed.

## ARM-Side Changes

### 1. Replace fbdev_presenter with direct DDR3 writer

Instead of writing to `/dev/fb0`, open `/dev/mem` and mmap the DDR3 region where the FPGA reader expects pixels.

```c
// Pseudocode
int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
void* ddr_base = mmap(NULL, DDR_REGION_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, mem_fd, DDR_BASE_ADDR);

// Per frame:
uint32_t* ctrl = (uint32_t*)ddr_base;
void* buf = (active_buf == 0) ? ddr_base + BUF0_OFFSET : ddr_base + BUF1_OFFSET;
memcpy(buf, game_surface->pixels, 384 * 224 * 2);  // RGB565
ctrl[0] = (frame_counter << 2) | (active_buf & 1);
active_buf ^= 1;
```

This replaces all of `fbdev_presenter.c`'s complexity (staging buffers, dirty tracking, LUT scaling, tile comparison, readback paths) with a single memcpy.

### 2. Replace timer-based pacing with vsync

Use `UIO_WAIT_VSYNC` (SPI command `0x30`) to block until the FPGA's actual vblank:

```c
// Instead of SDL_DelayNS(sleep_time):
spi_uio_cmd(UIO_WAIT_VSYNC);  // blocks until FPGA vblank
```

This gives deterministic frame timing locked to the display, eliminating timer jitter.

**Note**: `UIO_WAIT_VSYNC` currently works with the standard framework. Need to verify it works when the core is generating its own video timing (it should, as it's a core-side signal).

### 3. Wrapper changes

The wrapper (`thirdsarm_wrapper.cpp`) currently calls `video_fb_enable(1)` and `set_vga_fb(1)` to activate the framebuffer/scaler path. For native video mode:
- Don't call `video_fb_enable(1)` — leave `vga_fb=0` so the DAC mux selects core video
- Instead, send an init command to the FPGA module (via SPI or `hps_ext`) to start the pixel reader
- Or simply have the FPGA module start automatically when it detects valid frame data in DDR3

### 4. SDL renderer changes

The game renderer currently targets an SDL texture/surface. Two options:

**Option A**: Render to SDL_Surface (software), then memcpy to DDR3. This is the simplest change — replace the fbdev_presenter call with a DDR3 write.

**Option B**: Render directly into the DDR3 mmap'd buffer (true zero-copy). The game renderer writes pixels directly to the FPGA-visible DDR3 region. Eliminates one memcpy. More invasive to the renderer but saves ~0.3ms (172KB uncached write).

Recommend Option A for initial implementation.

## Modeline Calculation

The exact CPS3 NTSC modeline needs to be derived. Key constraints:
- 384 active horizontal pixels
- 224 active vertical lines
- 59.59949 Hz refresh rate
- 15kHz horizontal scan rate (for CRT compatibility)
- Standard NTSC-ish blanking intervals

Approximate calculation:
```
V_total = 224 + VFP + VS + VBP
H_total = 384 + HFP + HS + HBP

Constraint: H_total * V_total * 59.59949 = pixel_clock
Constraint: 59.59949 * V_total = H_freq (should be ~15.7kHz for NTSC CRT)

V_total = 15734 / 59.59949 ~= 264 lines
H_total = pixel_clock / (V_total * 59.59949)

Typical blanking for 15kHz:
  V: 224 active + 3 front porch + 3 sync + 34 back porch = 264 total
  H: 384 active + ~16 FP + ~32 HS + ~80 BP = 512 total

Pixel clock = 512 * 264 * 59.59949 ~= 8.057 MHz
```

These values need validation against actual CPS3 hardware timing and CRT compatibility. The Groovy project's `switchres` library could be used as a reference for calculating CRT-compatible modelines.

## Expected Benefits

### Latency
- **Before**: 1-2 frames (16-33ms) through scaler triple-buffer
- **After**: ~4-30 scanlines (~0.3-2ms) — same as native FPGA cores on analog
- **Improvement**: ~15-30ms reduction

### Performance (CPU time per frame)
- **Before**: 0.5-2ms in fbdev_presenter (LUT scaling, dirty tracking, readback)
- **After**: ~1.0-1.3ms (ARGB8888-to-RGB565 conversion in cached memory + uncached memcpy of 172KB to DDR3). Uncached writes on Cortex-A9 are ~100-200 MB/s for sequential access, so the 172KB DDR3 write alone is ~0.9-1.7ms. The conversion to a cached staging buffer is fast (~0.1ms).
- **Improvement**: Eliminates LUT scaling overhead entirely. Net savings depend on the current presentation path but the uncached DDR3 write replaces the uncached fbdev write, so the real gain is removing the per-pixel LUT work.

### Frame Pacing
- **Before**: Timer-based (`SDL_DelayNS`), ~1-4ms jitter
- **After**: Hardware vsync (`UIO_WAIT_VSYNC`), deterministic
- **Improvement**: Eliminates frame jitter, more consistent visual smoothness

## Risks and Tradeoffs

### What We Lose
1. ~~**MiSTer OSD overlay**~~: **RESOLVED — OSD works with no changes.** See "OSD Behavior" section below.
2. **HDMI scaling quality**: HDMI users would still go through the scaler (via `video_mixer` output), but the path is different. Needs testing.
3. **Scaler filters**: Polyphase filtering, shadow masks, etc. from the scaler are not available on the direct analog path. Native cores don't have these on analog either — this is expected behavior.
4. **Compatibility with MiSTer.ini video settings**: Some settings (e.g., `vsync_adjust`, `video_mode`) may behave differently or be irrelevant. `vga_scaler` is not supported and must be off — when `vga_scaler=1`, `vgas_en=1` and the DAC mux selects scaler output, bypassing core video entirely.

### Risks
1. **PLL stability**: Getting the exact pixel clock right is critical. Wrong timing = no picture or unstable picture on CRT.
2. **DDR3 arbitration**: The FPGA DDR3 reads for video must coexist with other DDR3 users (scaler for HDMI, any core memory access). MiSTer's DDR3 controller handles arbitration, but needs testing for latency spikes.
3. **CRT compatibility**: The modeline must produce valid 15kHz NTSC timing that CRTs accept. May need adjustment per display.
4. **Verilog complexity**: Modifying the Menu core's FPGA design is higher-risk than ARM-only changes. FPGA builds are slow to iterate on.

### Fallback
The current scaler-based path remains fully functional. The native video path can be a compile-time option or runtime toggle (ARM sends SPI command to enable/disable the FPGA pixel reader, and sets/clears `vga_fb` accordingly).

## Scope Estimate

### FPGA (Verilog)
- DDR3 pixel reader state machine: ~150 lines
- Line FIFOs (dcfifo instantiation + control): ~80 lines
- Video timing generator: ~80 lines
- Integration into menu.sv (port wiring, mode selection): ~50 lines
- PLL configuration: IP parameterization (compile-time) or ~60 lines (runtime reconfig)
- **Total: ~350-420 lines of new Verilog**

### ARM (C)
- DDR3 mmap writer (replaces fbdev_presenter for this path): ~80 lines
- Vsync-based frame pacing: ~30 lines
- Wrapper init/teardown changes: ~40 lines
- Runtime mode toggle (optional): ~30 lines
- **Total: ~150-180 lines of new C**

### Removed/Simplified
- fbdev_presenter.c scaling paths: not removed, but bypassed (kept for HDMI fallback)
- Timer-based frame pacing: replaced by vsync when native video active

## OSD Behavior (Resolved)

The MiSTer OSD **works automatically with no changes** on the native video path. This was a key risk that has been fully retired.

### Why It Works

The analog video signal chain in `sys_top.v` is hardwired as:

```
Core video (r_out, g_out, b_out, hs_emu, vs_emu, de_emu)
    → scanlines module (line 1371)
        → vga_osd module (line 1391)     ← OSD always composites here
            → csync (line 1413)
                → yc_out / vga_out (line 1426/1509)
                    → DAC pins (line 1537)
```

The `vga_osd` instance sits between scanlines and the DAC and is **always in the signal path** when `vga_fb=0`. It composites on whatever RGB data flows through, regardless of origin.

### Key Details

- **Two independent OSD instances**: `vga_osd` (clocked on `clk_vid`) and `hdmi_osd` (clocked on `clk_hdmi`). They receive SPI data via separate chip selects: `io_osd_vga = io_ss1 & ~io_ss2`, `io_osd_hdmi = io_ss1 & ~io_ss0`.
- **OSD is independent of `vga_fb`**: Different SPI chip selects, different control flags. `OsdEnable()`/`OsdDisable()`/`OsdWrite()`/`OsdUpdate()` all work identically.
- **ARM-side code unchanged**: The wrapper's menu rendering (`draw_wrapper_menu()`, `service_wrapper_menu()` in `thirdsarm_wrapper.cpp`) and the OSD library (`osd.cpp`) require zero modifications.
- **Clock-agnostic**: The OSD module takes `clk_video` as input and adapts to any pixel clock rate. At our ~8MHz pixel clock it will function correctly. The `clk_sys` domain handles SPI command reception with internal CDC.
- **3-cycle pipeline latency**: Negligible (~375ns at 8MHz).
- **`osd_target` defaults to `OSD_ALL`**: OSD data goes to both VGA and HDMI instances simultaneously. No wrapper code changes this.

### One Consideration: OSD Sizing at Native Resolution

The OSD overlay renders at a fixed character size (8x8 pixel glyphs, 256 bytes per line, up to 19 lines). At the current scaler output resolution (e.g., 640x480), the OSD occupies a moderate portion of the screen. At native 384x224, the OSD will appear **proportionally larger** because the total pixel count is smaller.

The OSD uses `de_in` to detect the visible area boundary and positions itself within it, so it will adapt automatically — but menu text may cover more of the game area. This is cosmetic, not functional, and matches the behavior of native FPGA cores running at low resolutions (e.g., NES at 256x240).

If the OSD sizing becomes problematic, the `OsdSetSize()` call in the wrapper could be adjusted to use fewer lines, but this is unlikely to be needed.

## Open Questions

1. **Exact CPS3 modeline**: What are the precise H/V blanking values? Should we match CPS3 hardware exactly, or calculate an optimal 15kHz-compatible modeline?
2. **RGB565 vs ARGB8888**: RGB565 halves bandwidth. Is CPS3's color palette fully representable in RGB565 (65K colors)? CPS3 uses 16-bit color internally, so likely yes.
3. **Double-buffer sync mechanism**: DDR3 flag word polling vs SPI command per frame? Flag word is simpler but adds a few scanlines of polling latency. SPI is more explicit.
4. ~~**OSD behavior**~~: **RESOLVED.** OSD composites automatically on core video path. No changes needed. See "OSD Behavior" section above.
5. **HDMI path**: When native video is active, does HDMI output still work via the `video_mixer` → ascal path? Need to verify that the scaler can still capture the core's video output for HDMI while analog gets the direct path.
6. **Audio**: Audio path is separate (currently handled by the ARM via ALSA or SDL_Audio). No changes expected, but confirm no interaction.
