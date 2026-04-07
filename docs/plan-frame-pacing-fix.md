# Frame Pacing Fix: PLL Retune + Vsync Feedback

**Status:** Both parts implemented. Part 1: `dbcf340e`. Part 2: `6e334f53`.

Two-part fix for frame stutter on the FPGA native video path via direct video.

**Problem:** The FPGA runs at 59.6374 Hz (PLL-derived). The CPS3 game targets
59.59949 Hz. The 0.038 Hz mismatch causes the DDR3 double-buffer to show a stale
frame every ~26 seconds. Additionally, the ARM's open-loop `SDL_DelayNS()` timer
has no knowledge of the FPGA's actual vblank phase, so OS scheduling jitter causes
additional sporadic stale frames.

**Solution:**
- **Part 1 — PLL Retune:** Change the PLL from 31.25 MHz (div4) to 15.581 MHz
  (div2), producing 59.59949 Hz — a 34,000x improvement in frequency accuracy.
  Eliminates the systematic drift entirely.
- **Part 2 — Vsync Feedback:** Add a DDR3 feedback channel from FPGA to ARM so
  the ARM can phase-lock its frame delivery to actual FPGA vblank timing.
  Eliminates stutter from OS scheduling jitter.

---

## Part 1: PLL Retune

### 1.1 New PLL Configuration

**Target:** Match the CPS3 original frame rate of 59.59949 Hz as closely as
possible while keeping H_freq within safe CRT range (14,500–16,500 Hz).

**Selected configuration** (from exhaustive computational search):

| Parameter       | Current          | New                            |
|-----------------|------------------|--------------------------------|
| PLL M/N         | (yields 31.25)   | M=373, N=21 → VCO=888.095 MHz |
| PLL C2          | (yields 31.25)   | C2=57 → outclk_2=15.581 MHz   |
| PLL C0          | (yields 100.0)   | C0=9 → outclk_0=98.677 MHz    |
| CLK_VIDEO       | 31.25 MHz        | **15.581 MHz**                 |
| CE_PIXEL        | divide-by-4      | **divide-by-2**                |
| Pixel clock     | 7.8125 MHz       | **7.7903 MHz**                 |
| H_TOTAL         | 500              | **497**                        |
| V_TOTAL         | 262              | **263**                        |
| Frame rate      | 59.6374 Hz       | **59.59949 Hz**                |
| Error           | +0.038 Hz        | **-0.0000014 Hz**              |
| H_freq          | 15,625 Hz        | **15,675 Hz** (+50 Hz, +0.32%) |

**Math verification:**
```
VCO        = 50,000,000 * 373 / 21           = 888,095,238.095... Hz
CLK_VIDEO  = VCO / 57                        = 15,580,618.213... Hz
pixel_clk  = CLK_VIDEO / 2                   = 7,790,309.107... Hz
frame_rate = pixel_clk / (497 * 263)          = 59.59948887... Hz
error      = 59.59948887 - 59.59949           = -0.00000113 Hz
H_freq     = pixel_clk / 497                  = 15,674.67 Hz
```

**clk_sys impact:** Changes from 100.000 MHz to 98.677 MHz (−1.3%). This is safe:
the DDR3 Avalon interface is frequency-flexible, and the SDRAM clearing logic at
boot tolerates minor frequency variation. Timing closure must be verified after
Quartus rebuild.

**YC encoder impact:** Native video bypasses the YC encoder entirely (outputs RGB
directly to DAC). The Menu OSD's YC PHASE_INC is recalculated on-the-fly by
`video.cpp` based on measured CLK_VIDEO, so it auto-adapts. The `yc.txt` override
for the menu core entry would need one line updated (low priority, menu only
visible briefly at boot).

### 1.2 File Changes — PLL Retune

#### 1.2.1 `vendor/Menu_MiSTer/rtl/pll/pll_0002.v`

Change the PLL output frequencies. Quartus will internally select M/N/C counters
to match. The target is VCO ≈ 888.095 MHz, C0=9 (98.677 MHz), C2=57 (15.581 MHz).

**Line 28** — change outclk_0 frequency:
```
OLD: .output_clock_frequency0("100.000000 MHz"),
NEW: .output_clock_frequency0("98.677248 MHz"),
```

**Line 31** — change outclk_1 frequency (unused, set to something achievable from
the same VCO, or disable):
```
OLD: .output_clock_frequency1("20.000000 MHz"),
NEW: .output_clock_frequency1("15.580618 MHz"),
```
NOTE: outclk_1 is unused (`clk_20m` in menu.sv, marked "unused, kept for future
use"). Setting it to the same frequency as outclk_2 is safe and avoids a dangling
output counter. Alternatively, disable it by setting to "0 MHz".

**Line 34** — change outclk_2 frequency:
```
OLD: .output_clock_frequency2("31.250000 MHz"),
NEW: .output_clock_frequency2("15.580618 MHz"),
```

**Verification after Quartus compile:** Check the compilation report's PLL summary
to confirm actual output frequencies match targets. If Quartus picks a different
M/N that gives an appreciably different frequency, use the PLL Megawizard GUI in
Quartus 17 to regenerate with explicit M=373, N=21, C0=9, C2=57.

#### 1.2.2 `vendor/Menu_MiSTer/menu.sv`

**Lines 189–197** — Change CE_PIXEL from divide-by-4 to divide-by-2:

Replace:
```verilog
// CE_PIXEL: divide CLK_VIDEO (31.25 MHz) by 4 for ~7.8125 MHz effective pixel rate.
// Integer divider = zero pixel timing jitter.
reg [1:0] ce_div;
wire ce_pix_div4 = (ce_div == 2'd0);
always @(posedge CLK_VIDEO) begin
	if (RESET) ce_div <= 2'd0;
	else ce_div <= ce_div + 2'd1;
end
assign CE_PIXEL = ce_pix_div4;
```

With:
```verilog
// CE_PIXEL: divide CLK_VIDEO (15.581 MHz) by 2 for ~7.790 MHz effective pixel rate.
// PLL config: 50 MHz * 373/21 VCO / 57 C2 = 15.5806 MHz.
// Frame rate: 7,790,309 / (497 * 263) = 59.5995 Hz (matches CPS3 TARGET_FPS).
reg ce_div;
wire ce_pix_div2 = ~ce_div;
always @(posedge CLK_VIDEO) begin
	if (RESET) ce_div <= 1'b0;
	else ce_div <= ~ce_div;
end
assign CE_PIXEL = ce_pix_div2;
```

**Lines 245–246** — Update comments:
```
OLD: wire clk_20m;   // PLL outclk_1 (unused, kept for future use)
     wire clk_pix;   // PLL outclk_2: 31.25 MHz (CLK_VIDEO, divided by 4 for 7.8125 MHz pixels)
NEW: wire clk_20m;   // PLL outclk_1 (unused)
     wire clk_pix;   // PLL outclk_2: 15.581 MHz (CLK_VIDEO, divided by 2 for 7.790 MHz pixels)
```

#### 1.2.3 `vendor/Menu_MiSTer/rtl/native_video_timing.sv`

**Lines 1–25** — Update header comment block:

Replace the entire header (lines 1–25) with:
```verilog
//============================================================================
//
//  Native Video Timing Generator
//
//  384x224 active area @ 59.5995 Hz (497x263 total)
//  CLK_VIDEO: 15.581 MHz, CE_PIXEL: divide-by-2 (7.790 MHz effective)
//
//  H: 384 active + 24 FP + 38 sync + 51 BP = 497 total
//  V: 224 active + 12 FP +  3 sync + 24 BP = 263 total
//
//  PLL: 50 MHz * 373/21 = 888.095 MHz VCO, /57 = 15.5806 MHz CLK_VIDEO.
//  Pixel clock: 15.5806 / 2 (CE_PIXEL) = 7.79031 MHz.
//  Frame rate: 7,790,309 / (497 * 263) = 59.59949 Hz = CPS3 TARGET_FPS.
//  Error vs original: -0.0000014 Hz (phase drift period: ~22 years).
//  H_freq: 7,790,309 / 497 = 15,674.67 Hz (within 15 kHz CRT range).
//
//  Copyright (C) 2026 3SX Project
//  Licensed under GNU General Public License v2+
//
//============================================================================
```

**Lines 43–68** — Update timing constants and comments:

Replace lines 43–68 with:
```verilog
// Timing constants
//
// Image centering notes:
// The CRT positions the image based on sync-to-active timing.
// Larger H_BP shifts image RIGHT, larger V_BP shifts image DOWN.
//
// H_TOTAL=497, V_TOTAL=263: with PLL (50*373/21/57 = 15.581 MHz, CE_DIV=2):
// pixel_clock = 7.790 MHz, H_freq = 15,675 Hz, refresh = 59.5995 Hz.
localparam H_ACTIVE = 384;
localparam H_FP     = 24;
localparam H_SYNC   = 38;
localparam H_BP     = 51;
localparam H_TOTAL  = 497;   // 384+24+38+51

localparam V_ACTIVE = 224;
localparam V_FP     = 12;
localparam V_SYNC   = 3;
localparam V_BP     = 24;
localparam V_TOTAL  = 263;   // 224+12+3+24
```

**Line 37 (hcount width):** Currently `output reg [9:0] hcount`. H_TOTAL=497 fits
in 9 bits (max 511), but 10 bits is fine. **No change needed.**

**Line 38 (vcount width):** Currently `output reg [8:0] vcount`. V_TOTAL=263 fits
in 9 bits (max 511). **No change needed.**

#### 1.2.4 `vendor/Menu_MiSTer/rtl/native_video_top.sv`

**Lines 1–12** — Update header comment:
```verilog
//============================================================================
//
//  Native Video Top-Level Wrapper
//
//  Instantiates the timing generator and DDR3 reader, providing a clean
//  interface to menu.sv. Runs on CLK_VIDEO (15.581 MHz) with integer
//  divide-by-2 ce_pix for 7.790 MHz effective pixel rate.
//
//  Copyright (C) 2026 3SX Project
//  Licensed under GNU General Public License v2+
//
//============================================================================
```

**Lines 16–17, 43–44, 47, 79** — Update clock frequency comments:
```
OLD: // video clock (20 MHz, CLK_VIDEO)
NEW: // video clock (15.581 MHz, CLK_VIDEO)

OLD: // fractional pixel enable (~8.065 MHz)
NEW: // pixel enable, divide-by-2 (~7.790 MHz)

OLD: // Runs on clk_vid (20 MHz) with ce_pix gating at ~8.065 MHz.
NEW: // Runs on clk_vid (15.581 MHz) with ce_pix gating at ~7.790 MHz.

OLD: // Read side: clk_vid (20 MHz) with ce_pix gating at ~8.065 MHz
NEW: // Read side: clk_vid (15.581 MHz) with ce_pix gating at ~7.790 MHz
```

#### 1.2.5 `vendor/Menu_MiSTer/rtl/native_video_reader.sv`

**Lines 21–22** — Update clock frequency comments:
```
OLD: //    Write side: ddr_clk (clk_sys, 100 MHz)
     //    Read side:  clk_vid (CLK_VIDEO, 31.25 MHz) with ce_pix divide-by-4 (7.8125 MHz)
NEW: //    Write side: ddr_clk (clk_sys, ~98.7 MHz)
     //    Read side:  clk_vid (CLK_VIDEO, 15.581 MHz) with ce_pix divide-by-2 (7.790 MHz)
```

**Lines 43–44** — Update port comments:
```
OLD: input  wire        clk_vid,         // video clock (20 MHz, CLK_VIDEO)
     input  wire        ce_pix,          // fractional pixel enable (~8.065 MHz)
NEW: input  wire        clk_vid,         // video clock (15.581 MHz, CLK_VIDEO)
     input  wire        ce_pix,          // pixel enable, divide-by-2 (~7.790 MHz)
```

**Line 96–97** — Update CDC comment:
```
OLD: // CDC: new_frame from clk_vid (20 MHz, ce_pix gated) to ddr_clk (100 MHz)
     // Pulse is one 20 MHz cycle wide (50 ns), safely captured at 100 MHz.
NEW: // CDC: new_frame from clk_vid (15.581 MHz, ce_pix gated) to ddr_clk (~98.7 MHz)
     // Pulse is one 15.581 MHz cycle wide (~64 ns), safely captured at ~98.7 MHz.
```

**Lines 109, 373** — Update similar clock frequency comments in CDC and FIFO
sections. (Change "20 MHz" → "15.581 MHz" and "100 MHz" → "~98.7 MHz" in
comments only.)

#### 1.2.6 `include/port/sdl/sdl_app.h`

Since the PLL now produces essentially the exact CPS3 frame rate, `NV_TARGET_FPS`
becomes equal to `TARGET_FPS`. The override is no longer needed.

**Lines 10–16** — Replace the NV_TARGET_FPS block:

Replace:
```c
// FPGA native video refresh rate: 31.25 MHz PLL / 4 CE_PIXEL / (500 * 262) total pixels
// = 7,812,500 / 131,000 = 59.6374 Hz.  The fixed integer-N PLL and integer H/V totals
// cannot produce exactly 59.59949 Hz; the closest options are 0.005-0.038 Hz off (see
// native_video_timing.sv header for analysis).  ARM frame pacing must target this rate
// when outputting via the FPGA's native video path, otherwise the 0.038 Hz mismatch
// causes the DDR3 double-buffer to show a stale frame every ~26 seconds.
#define NV_TARGET_FPS (7812500.0 / 131000.0)
```

With:
```c
// FPGA native video refresh rate: 15.581 MHz PLL / 2 CE_PIXEL / (497 * 263) total pixels
// = 7,790,309 / 130,711 = 59.59949 Hz.  PLL config (50 MHz * 373/21 / 57 / 2) produces
// a frame rate matching TARGET_FPS to within 0.0000014 Hz (1.4 microhertz), so no
// ARM-side compensation is needed.  NV_TARGET_FPS is retained for clarity but equals
// TARGET_FPS.
#define NV_TARGET_FPS TARGET_FPS
```

#### 1.2.7 `src/port/sdl/sdl_app.c`

**Lines 9310–9319** — Remove the frame pacing override since NV_TARGET_FPS now
equals TARGET_FPS. Replace:

```c
            /* Match ARM frame pacing to the FPGA's actual pixel-clock-derived
               refresh rate (NV_TARGET_FPS = 59.6374 Hz) instead of the CPS3
               original (59.5995 Hz).  The 31.25 MHz integer-N PLL cannot
               produce 59.5995 Hz with any integer H/V total combination, so
               the ARM must adapt.  The 0.063% speed increase is imperceptible
               (~0.06 s over a 99-second round). */
            if (native_video_writer_enabled) {
                target_frame_time_ns = (Uint64)(1000000000.0 / NV_TARGET_FPS);
                backend_logf("Native video: frame pacing adjusted to %.4f Hz (FPGA PLL rate)", NV_TARGET_FPS);
            }
```

With:
```c
            /* FPGA PLL produces 59.59949 Hz natively (50*373/21/57/2 / 497*263),
               matching TARGET_FPS to within 1.4 microhertz.  No frame pacing
               override needed — target_frame_time_ns already matches FPGA rate. */
            if (native_video_writer_enabled) {
                backend_logf("Native video: FPGA rate matches TARGET_FPS (%.4f Hz), no pacing adjustment needed", TARGET_FPS);
            }
```

**Lines 9977–9988** — Update the frame pacing comment block:

Replace:
```c
    // Do frame pacing
    //
    // When native video is active, target_frame_time_ns is set to match the
    // FPGA's PLL-derived refresh rate (NV_TARGET_FPS = 59.6374 Hz) rather than
    // the CPS3 original (59.5995 Hz).  This keeps ARM frame delivery in phase
    // with the FPGA's vblank poll, preventing periodic frame repeats from the
    // DDR3 double-buffer stale-frame path.
    //
    // The ideal pacing source would be the FPGA's own vsync via
    // spi_uio_cmd(UIO_WAIT_VSYNC), but the game runs in a forked child process
    // separate from the MiSTer wrapper which owns the SPI interface.  A future
    // hardware vsync bridge could replace timer pacing for tighter lock.
```

With:
```c
    // Do frame pacing
    //
    // The FPGA PLL produces 59.59949 Hz natively (matching TARGET_FPS), so
    // target_frame_time_ns is the same for both native and non-native paths.
    // The vsync feedback channel (Part 2) provides closed-loop phase locking
    // to eliminate residual jitter from OS scheduling.
```

### 1.3 Build & Test — PLL Retune

1. **Regenerate PLL IP** in the Quartus 17 colima VM:
   - Open the PLL Megawizard for `vendor/Menu_MiSTer/rtl/pll.v`
   - Set outclk_0 to 98.677248 MHz
   - Set outclk_2 to 15.580618 MHz
   - Regenerate → overwrites `pll_0002.v` and `pll.v`
   - **OR** manually edit `pll_0002.v` as described in 1.2.1 and let Quartus
     synthesize the PLL from frequency parameters

2. **Verify PLL parameters** in Quartus compilation report:
   - Expected: M=373, N=21, VCO=888.095 MHz
   - outclk_0: C0=9 → 98.677 MHz
   - outclk_2: C2=57 → 15.581 MHz
   - If Quartus picks different M/N, check actual output frequencies. Any
     configuration that gives frame rate error < 0.01 Hz is acceptable.

3. **Full Quartus rebuild** (compiles bitstream with new PLL + timing)

4. **ARM rebuild** (recompile `sdl_app.c` and `native_video_writer.c`)

5. **Deploy** both `.rbf` and binary to MiSTer

6. **Verification:**
   - Check logs for "no pacing adjustment needed" message
   - FPS overlay should show ~59.60 Hz
   - Run for 10+ minutes — no periodic stutter (the old ~26-second pattern
     should be gone)

---

## Part 2: Vsync Feedback via DDR3 Shared Memory

### 2.1 Overview

Even with perfect frequency matching from Part 1, the ARM and FPGA run on
independent oscillators. OS scheduling jitter (Linux kernel preemption, IRQs) can
delay `SDL_DelayNS()` by milliseconds, causing the ARM's frame write to land after
the FPGA's vblank poll → stale frame displayed.

The fix: the FPGA writes a vblank counter to DDR3 at each vblank. The ARM reads
this counter and adjusts its frame delivery phase to stay ahead of the FPGA's
actual vblank.

### 2.2 DDR3 Memory Layout Update

```
Offset    Size    Purpose              Writer    Reader
0x0000    4B      Control word         ARM       FPGA
0x0004    4B      (reserved)           -         -
...
0x0040    4B      Feedback word        FPGA      ARM
...
0x0100    172KB   Buffer 0             ARM       FPGA
0x2A200   172KB   Buffer 1             ARM       FPGA
```

**Feedback word format** (32-bit, little-endian, at physical 0x3A000040):
```
Bits [31:2]   vblank_counter     Monotonically incrementing (wraps at 2^30)
Bits [1:0]    last_buffer_status  0 or 1 = buffer consumed; 2 = stale re-read
```

**DDR3 qword address** for feedback word:
```
Physical 0x3A000040 >> 3 = 29'h07400008
```

### 2.3 File Changes — Vsync Feedback

#### 2.3.1 `vendor/Menu_MiSTer/rtl/native_video_reader.sv`

This is the largest change. The reader's state machine gains two new states and
DDR3 write capability.

**Step A — Change DDR3 write signal outputs (lines 65–68):**

Replace:
```verilog
// Unused DDR3 write signals
assign ddr_din = 64'd0;
assign ddr_be  = 8'hFF;
assign ddr_we  = 1'b0;
```

With:
```verilog
// DDR3 write signals: active only during feedback write
reg  [63:0] feedback_din;
reg  [7:0]  feedback_be;
reg         feedback_wr;
assign ddr_din = feedback_din;
assign ddr_be  = feedback_be;
assign ddr_we  = feedback_wr;
```

**Step B — Add feedback address constant (after line 77):**

Insert after `localparam [28:0] LINE_STRIDE = 29'd96;`:
```verilog
localparam [28:0] FEEDBACK_ADDR = 29'h07400008;  // 0x3A000040 >> 3
```

**Step C — Add new state definitions (lines 159–166):**

Replace:
```verilog
localparam [3:0] ST_IDLE         = 4'd0;
localparam [3:0] ST_POLL_CTRL    = 4'd1;
localparam [3:0] ST_WAIT_CTRL    = 4'd2;
localparam [3:0] ST_CHECK_CTRL   = 4'd3;
localparam [3:0] ST_READ_LINE    = 4'd4;
localparam [3:0] ST_WAIT_LINE    = 4'd5;
localparam [3:0] ST_LINE_DONE    = 4'd6;
localparam [3:0] ST_WAIT_DISPLAY = 4'd7;
```

With:
```verilog
localparam [3:0] ST_IDLE           = 4'd0;
localparam [3:0] ST_POLL_CTRL      = 4'd1;
localparam [3:0] ST_WAIT_CTRL      = 4'd2;
localparam [3:0] ST_CHECK_CTRL     = 4'd3;
localparam [3:0] ST_READ_LINE      = 4'd4;
localparam [3:0] ST_WAIT_LINE      = 4'd5;
localparam [3:0] ST_LINE_DONE      = 4'd6;
localparam [3:0] ST_WAIT_DISPLAY   = 4'd7;
localparam [3:0] ST_WRITE_FEEDBACK = 4'd8;
localparam [3:0] ST_WAIT_WR_ACK   = 4'd9;
```

**Step D — Add new registers (after line 178, the `timeout_cnt` declaration):**

Insert:
```verilog
reg  [29:0] vblank_counter;      // Incremented each vblank, written to DDR3
reg  [1:0]  last_buffer_status;  // 0/1 = which buffer consumed, 2 = stale
reg         proceed_to_read;     // After feedback write: go to READ_LINE or IDLE?
```

**Step E — Update reset block (lines 199–218):**

Add these lines inside the reset block, after `fifo_aclr_cnt <= 4'd0;` (line 217):
```verilog
        feedback_din        <= 64'd0;
        feedback_be         <= 8'h0F;
        feedback_wr         <= 1'b0;
        vblank_counter      <= 30'd0;
        last_buffer_status  <= 2'd0;
        proceed_to_read     <= 1'b0;
```

**Step F — Add feedback_wr deassert (after line 227):**

After `if (!ddr_busy) ddr_rd <= 1'b0;`, add:
```verilog
        if (!ddr_busy) feedback_wr <= 1'b0;
```

**Step G — Rewrite ST_CHECK_CTRL (lines 277–309):**

Replace the entire ST_CHECK_CTRL case arm:
```verilog
            ST_CHECK_CTRL: begin
                if (ctrl_word[31:2] != prev_frame_counter) begin
                    // New frame available -- NOW clear the FIFO and load it
                    prev_frame_counter <= ctrl_word[31:2];
                    active_buffer      <= ctrl_word[0];
                    stale_vblank_count <= 5'd0;
                    buf_base_addr      <= ctrl_word[0] ? BUF1_ADDR : BUF0_ADDR;
                    cur_line           <= 9'd0;
                    preloading         <= 1'b1;
                    fifo_aclr_cnt      <= 4'd8;
                    state              <= ST_READ_LINE;
                end
                else if (first_frame_loaded) begin
                    // Stale frame but we have a valid previous buffer --
                    // re-read the same buffer so the display shows the last
                    // good frame instead of going black. This handles the
                    // common case where ARM delivery drifts slightly behind
                    // the FPGA's vblank poll.
                    if (stale_vblank_count < 5'd30)
                        stale_vblank_count <= stale_vblank_count + 5'd1;
                    if (stale_vblank_count >= 5'd29)
                        frame_ready_reg <= 1'b0;
                    // Re-read previous buffer (buf_base_addr unchanged)
                    cur_line      <= 9'd0;
                    preloading    <= 1'b1;
                    fifo_aclr_cnt <= 4'd8;
                    state         <= ST_READ_LINE;
                end
                else begin
                    // No frame ever loaded -- just wait
                    state <= ST_IDLE;
                end
            end
```

With:
```verilog
            ST_CHECK_CTRL: begin
                // Always increment vblank counter and prepare feedback
                vblank_counter <= vblank_counter + 30'd1;

                if (ctrl_word[31:2] != prev_frame_counter) begin
                    // New frame available -- clear FIFO and load it
                    prev_frame_counter <= ctrl_word[31:2];
                    active_buffer      <= ctrl_word[0];
                    stale_vblank_count <= 5'd0;
                    last_buffer_status <= {1'b0, ctrl_word[0]};
                    buf_base_addr      <= ctrl_word[0] ? BUF1_ADDR : BUF0_ADDR;
                    cur_line           <= 9'd0;
                    preloading         <= 1'b1;
                    fifo_aclr_cnt      <= 4'd8;
                    proceed_to_read    <= 1'b1;
                    state              <= ST_WRITE_FEEDBACK;
                end
                else if (first_frame_loaded) begin
                    // Stale frame -- re-read previous buffer
                    if (stale_vblank_count < 5'd30)
                        stale_vblank_count <= stale_vblank_count + 5'd1;
                    if (stale_vblank_count >= 5'd29)
                        frame_ready_reg <= 1'b0;
                    last_buffer_status <= 2'd2;  // stale
                    cur_line           <= 9'd0;
                    preloading         <= 1'b1;
                    fifo_aclr_cnt      <= 4'd8;
                    proceed_to_read    <= 1'b1;
                    state              <= ST_WRITE_FEEDBACK;
                end
                else begin
                    // No frame ever loaded -- still write feedback so ARM
                    // knows vblanks are happening
                    last_buffer_status <= 2'd2;
                    proceed_to_read    <= 1'b0;
                    state              <= ST_WRITE_FEEDBACK;
                end
            end
```

**Step H — Add new states (after ST_WAIT_DISPLAY, before `default`):**

Insert before `default: state <= ST_IDLE;` (line 364):
```verilog
            ST_WRITE_FEEDBACK: begin
                if (!ddr_busy) begin
                    ddr_addr     <= FEEDBACK_ADDR;
                    ddr_burstcnt <= 8'd1;
                    feedback_din <= {32'd0, vblank_counter, last_buffer_status};
                    feedback_be  <= 8'h0F;   // Write lower 4 bytes of qword
                    feedback_wr  <= 1'b1;
                    state        <= ST_WAIT_WR_ACK;
                end
            end

            ST_WAIT_WR_ACK: begin
                if (!ddr_busy) begin
                    feedback_wr <= 1'b0;
                    state       <= proceed_to_read ? ST_READ_LINE : ST_IDLE;
                end
            end
```

#### 2.3.2 `vendor/Menu_MiSTer/rtl/native_video_top.sv`

The top module currently discards `ddr_din`, `ddr_be`, `ddr_we` from the reader
(lines 93–95 connect to reader but lines 27–29 output them as wires — the
reader's assigns now drive actual data instead of constants).

**No port changes needed.** The reader's `ddr_din`, `ddr_be`, `ddr_we` are already
wired through the top module to `ddr_din`, `ddr_be`, `ddr_we` output ports (lines
27–29 and 93–95). Since the reader now drives real values instead of constants,
this propagates automatically.

Wait — lines 93–95 in the instantiation are:
```verilog
    .ddr_din        (ddr_din),
    .ddr_be         (ddr_be),
    .ddr_we         (ddr_we),
```

These connect the reader's output ports to the top module's output wires. Since
the top module declares these as `output wire` (lines 27–29), and the reader now
drives them with actual feedback data, the connection works. **No change needed.**

#### 2.3.3 `vendor/Menu_MiSTer/menu.sv`

**Lines 410–412** — Route DDR3 write signals from native video instead of
hardcoded zeros.

First, add wires after `wire nv_ddr_rd;` (line 402):
```verilog
wire [63:0] nv_ddr_din;
wire  [7:0] nv_ddr_be;
wire        nv_ddr_we;
```

Then replace lines 410–412:
```
OLD: assign DDRAM_DIN      = use_nv ? 64'd0           : old_ddr_din;
     assign DDRAM_BE       = use_nv ? 8'hFF           : old_ddr_be;
     assign DDRAM_WE       = use_nv ? 1'b0            : old_ddr_we;

NEW: assign DDRAM_DIN      = use_nv ? nv_ddr_din      : old_ddr_din;
     assign DDRAM_BE       = use_nv ? nv_ddr_be       : old_ddr_be;
     assign DDRAM_WE       = use_nv ? nv_ddr_we       : old_ddr_we;
```

Then connect the wires in the native_video_top instantiation (lines 628–630):
```
OLD: .ddr_din        (),
     .ddr_be         (),
     .ddr_we         (),

NEW: .ddr_din        (nv_ddr_din),
     .ddr_be         (nv_ddr_be),
     .ddr_we         (nv_ddr_we),
```

#### 2.3.4 `src/port/sdl/native_video_writer.h`

Add after `bool NativeVideoWriter_IsActive(void);` (line 24):

```c

/// Read the FPGA's vblank feedback word from DDR3.
/// Returns the raw 32-bit value: bits[31:2] = vblank_counter, bits[1:0] = buffer_status.
/// Returns 0 if the writer is not initialized.
uint32_t NativeVideoWriter_ReadFeedback(void);

/// Extract the vblank counter from a feedback word.
static inline uint32_t NV_FeedbackVblankCounter(uint32_t fb) { return fb >> 2; }

/// Extract the buffer status from a feedback word (0,1 = buffer read; 2 = stale).
static inline uint32_t NV_FeedbackBufferStatus(uint32_t fb) { return fb & 3; }
```

#### 2.3.5 `src/port/sdl/native_video_writer.c`

**Add constant** after `#define NV_BUF1_OFFSET` (line 15):
```c
#define NV_FEEDBACK_OFFSET  0x00000040u
```

**Add to NativeVideoWriter_Init()** — after `active_buf = 0;` (line 48), before
`return true;`:
```c
    /* Clear feedback word so ARM doesn't read stale data from a previous run */
    volatile uint32_t* feedback = (volatile uint32_t*)(ddr_base + NV_FEEDBACK_OFFSET);
    *feedback = 0;
```

**Add ReadFeedback function** — after the `NativeVideoWriter_IsActive()` function
(line 104), before `#else`:
```c

uint32_t NativeVideoWriter_ReadFeedback(void) {
    if (!ddr_base) return 0;
    volatile uint32_t* fb = (volatile uint32_t*)(ddr_base + NV_FEEDBACK_OFFSET);
    return *fb;
}
```

**Add stub in the `#else` (non-MiSTer) section** — after `NativeVideoWriter_IsActive`
stub (line 124):
```c

uint32_t NativeVideoWriter_ReadFeedback(void) {
    return 0;
}
```

#### 2.3.6 `src/port/sdl/sdl_app.c`

**Step A — Add new static variables** after `static Uint64 frame_counter = 0;`
(line 84):

```c
static uint32_t last_fpga_vblank = 0;
static Uint64   last_vblank_observe_ns = 0;
static Uint64   adjusted_frame_time_ns = 0;
static int      vsync_lock_state = 0;  // 0=unsynced, 1=locking, 2=locked
static int      consecutive_stale = 0;
```

**Step B — Initialize adjusted_frame_time_ns** in the native video init block
(after the log message added in Part 1, around line 9318):

Add after the `backend_logf(...)` line:
```c
                adjusted_frame_time_ns = target_frame_time_ns;
```

**Step C — Replace the frame pacing block** (lines 9989–10006):

Replace the existing open-loop pacing:
```c
    Uint64 now = SDL_GetTicksNS();

    if (frame_deadline == 0) {
        frame_deadline = now + target_frame_time_ns;
    }

    if (now < frame_deadline) {
        Uint64 sleep_time = frame_deadline - now;
        SDL_DelayNS(sleep_time);
        now = SDL_GetTicksNS();
    }

    frame_deadline += target_frame_time_ns;

    // If we fell behind by more than one frame, resync to avoid spiraling
    if (now > frame_deadline + target_frame_time_ns) {
        frame_deadline = now + target_frame_time_ns;
    }
```

With:
```c
    Uint64 now = SDL_GetTicksNS();

    if (native_video_writer_enabled) {
        // --- Closed-loop frame pacing via FPGA vblank feedback ---
        //
        // The FPGA writes a vblank counter + buffer status to DDR3 at each
        // vblank.  We read it to:
        //   1) Track the FPGA's true frame period (low-pass filtered) to
        //      compensate for crystal PPM mismatch between ARM and FPGA.
        //   2) Detect stale frames (ARM was late) and nudge our deadline
        //      earlier so subsequent frames land before vblank.

        uint32_t feedback = NativeVideoWriter_ReadFeedback();
        uint32_t fpga_vblank = NV_FeedbackVblankCounter(feedback);
        uint32_t buf_status  = NV_FeedbackBufferStatus(feedback);

        if (fpga_vblank != 0 && fpga_vblank != last_fpga_vblank) {
            Uint64 observe_ns = SDL_GetTicksNS();

            if (vsync_lock_state == 0) {
                // First reference point — start tracking
                last_fpga_vblank      = fpga_vblank;
                last_vblank_observe_ns = observe_ns;
                adjusted_frame_time_ns = target_frame_time_ns;
                frame_deadline         = observe_ns + adjusted_frame_time_ns;
                vsync_lock_state       = 1;
            } else {
                // Compute observed FPGA frame period
                uint32_t delta_vblanks = fpga_vblank - last_fpga_vblank;
                Uint64   delta_ns      = observe_ns - last_vblank_observe_ns;
                Uint64   observed_period = delta_ns / delta_vblanks;

                // Low-pass filter: alpha = 1/16 — rejects jitter, tracks drift
                int64_t period_err = (int64_t)observed_period - (int64_t)adjusted_frame_time_ns;
                adjusted_frame_time_ns = (Uint64)((int64_t)adjusted_frame_time_ns + period_err / 16);

                last_fpga_vblank       = fpga_vblank;
                last_vblank_observe_ns = observe_ns;
                vsync_lock_state       = 2;
            }

            // Phase correction: if FPGA reports stale, we were late
            if (buf_status == 2) {
                consecutive_stale++;
                if (consecutive_stale > 2) {
                    // Systematic lateness: advance deadline by 0.5ms
                    frame_deadline -= 500000;
                }
            } else {
                consecutive_stale = 0;
            }
        }

        // Watchdog: fall back to open-loop if feedback stops updating
        if (last_vblank_observe_ns != 0 &&
            now - last_vblank_observe_ns > 500000000ULL) {
            vsync_lock_state       = 0;
            adjusted_frame_time_ns = target_frame_time_ns;
        }

        // Sleep until deadline
        if (frame_deadline == 0)
            frame_deadline = now + adjusted_frame_time_ns;
        if (now < frame_deadline) {
            SDL_DelayNS(frame_deadline - now);
            now = SDL_GetTicksNS();
        }
        frame_deadline += adjusted_frame_time_ns;
        if (now > frame_deadline + adjusted_frame_time_ns)
            frame_deadline = now + adjusted_frame_time_ns;
    } else {
        // --- Open-loop pacing for non-native-video paths ---
        if (frame_deadline == 0)
            frame_deadline = now + target_frame_time_ns;
        if (now < frame_deadline) {
            SDL_DelayNS(frame_deadline - now);
            now = SDL_GetTicksNS();
        }
        frame_deadline += target_frame_time_ns;
        if (now > frame_deadline + target_frame_time_ns)
            frame_deadline = now + target_frame_time_ns;
    }
```

**Step D — Add include** at top of sdl_app.c (near other native_video includes):
```c
#include "port/sdl/native_video_writer.h"
```
(May already be included — verify before adding.)

### 2.4 Atomicity and Safety Notes

- **DDR3 32-bit aligned write** from FPGA (via Avalon with `ddr_be = 8'h0F`) is
  atomic on the HPS bridge. ARM 32-bit aligned read (via uncached mmap) is also
  atomic. No tearing risk.
- **Read-during-write:** HPS DDR3 controller arbitrates — ARM sees old or new
  value, never partial. ARM retries next frame if it reads stale counter.
- **Feedback write timing:** 1 burst beat (~10ns) during ~2.5ms vblank. No impact
  on scanline read timing.
- **Startup sequence:** ARM clears feedback word in Init(). FPGA starts
  incrementing after `use_nv` asserted. ARM detects `fpga_vblank != 0` to begin
  phase locking. Before that, uses open-loop pacing.
- **Counter overflow:** 30-bit counter wraps every ~207 days. Unsigned delta math
  handles wrap correctly.

### 2.5 Build & Test — Vsync Feedback

1. Build FPGA (Quartus) — includes both Part 1 PLL changes and Part 2 feedback
2. Build ARM — includes all `sdl_app.c` and `native_video_writer.c` changes
3. Deploy both `.rbf` and binary

**Verification:**
- Run the game for 10+ minutes with native video active
- Before Part 2, Part 1 alone eliminates the ~26-second stutter pattern
- Part 2 additionally eliminates sporadic stale frames caused by OS jitter
- Under heavy system load (e.g., SSH file transfers), the vsync feedback should
  keep frame delivery phase-locked despite scheduling interference

---

## Summary of All Changed Files

| # | File | Part | Change Type |
|---|------|------|-------------|
| 1 | `vendor/Menu_MiSTer/rtl/pll/pll_0002.v` | 1 | PLL frequencies |
| 2 | `vendor/Menu_MiSTer/menu.sv` | 1+2 | CE_PIXEL div2, DDR3 write mux |
| 3 | `vendor/Menu_MiSTer/rtl/native_video_timing.sv` | 1 | H/V timing constants |
| 4 | `vendor/Menu_MiSTer/rtl/native_video_top.sv` | 1 | Comment updates |
| 5 | `vendor/Menu_MiSTer/rtl/native_video_reader.sv` | 1+2 | Comments + feedback write states |
| 6 | `include/port/sdl/sdl_app.h` | 1 | NV_TARGET_FPS = TARGET_FPS |
| 7 | `src/port/sdl/sdl_app.c` | 1+2 | Remove pacing override + vsync feedback loop |
| 8 | `src/port/sdl/native_video_writer.h` | 2 | ReadFeedback API |
| 9 | `src/port/sdl/native_video_writer.c` | 2 | ReadFeedback impl + feedback clear |
