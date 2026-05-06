# Implementation Specification: FPGA Native Video Output

> **Post-Phase-C update (2026-05-03):** `src/port/sdl/fbdev_presenter.{c,h}`
> have been removed. References below to `fbdev_presenter` (file paths,
> `FBDevPresenter_Present*` calls, NEON helpers) are historical context for
> the path this spec replaced. The shipped MiSTer build uses
> `src/port/sdl/native_video_writer.c` for DDR3 direct write and
> `src/port/sdl/fps_overlay_compositor.c` (`FPSOverlay_*` API) for FPS overlay.

## Status: Implementation Spec (Ready for Build)

This spec was derived from `docs/design-fpga-native-video.md` and a thorough
study of the existing codebase. A future agent should be able to implement
this feature using only the information contained here.

---

## 1. Exact Modeline Calculation

### Target Parameters

| Parameter | Value | Source |
|-----------|-------|--------|
| Active horizontal | 384 pixels | CPS3 native |
| Active vertical | 224 lines | CPS3 native |
| Refresh rate | 59.59949 Hz | `include/port/sdl/sdl_app.h` line 8: `#define TARGET_FPS 59.59949` |

### Derivation

The horizontal scan frequency for NTSC CRT must fall in approximately
15.625--15.750 kHz. We target 15,734 Hz (standard NTSC).

```
V_total = H_freq / refresh = 15734 / 59.59949 = 264.00 lines (exact to 4 decimal places)
```

With V_total = 264 lines, we allocate vertical blanking:

| V region | Lines | Notes |
|----------|-------|-------|
| Active | 224 | |
| Front porch | 3 | Standard NTSC-ish VFP |
| Sync | 3 | Standard NTSC-ish VS |
| Back porch | 34 | 264 - 224 - 3 - 3 = 34 |
| **Total** | **264** | |

For horizontal timing, we need H_total such that the pixel clock is achievable
from a 50 MHz reference. Several native MiSTer cores use H_total near 512 for
384-active modes (e.g., CPS1/CPS2 cores).

With H_total = 512:

```
pixel_clock = H_total * V_total * refresh
            = 512 * 264 * 59.59949
            = 8,057,499.2 Hz
            ~= 8.0575 MHz
```

Verification:
```
H_freq = pixel_clock / H_total = 8,057,499.2 / 512 = 15,737.3 Hz
```

15,737.3 Hz is within the 15,625--15,750 Hz CRT acceptance range. Confirmed valid.

Horizontal blanking (H_total - H_active = 128 pixels):

| H region | Pixels | Notes |
|----------|--------|-------|
| Active | 384 | |
| Front porch | 16 | ~2 us at 8 MHz |
| Sync | 32 | ~4 us, standard NTSC HS width |
| Back porch | 80 | 512 - 384 - 16 - 32 = 80 |
| **Total** | **512** | |

### Final Modeline

```
384x224 @ 59.59949 Hz
Pixel clock:  8.057499 MHz
H:  384  400  432  512   (active, HFP_end, HS_end, total)
V:  224  227  230  264   (active, VFP_end, VS_end, total)
H sync: negative polarity
V sync: negative polarity
```

Modeline string (xrandr format):
```
"384x224_59.60" 8.057 384 400 432 512 224 227 230 264 -hsync -vsync
```

### Cyclone V Fractional PLL Parameters

Reference clock: 50 MHz (FPGA_CLK2_50, same as used by the existing PLL in `menu.sv` line 231).

Target VCO frequency range for Cyclone V: 600--1300 MHz.

**Integer-N approach** (simplest, since the existing Menu core PLL uses Integer-N mode):

We need: f_out = 8.057499 MHz

Using the Cyclone V PLL formula:
```
f_VCO = f_ref * M / N
f_out = f_VCO / C
```

With N=1 (direct mode, matching the existing PLL config):
```
f_VCO = 50 * M
f_out = 50 * M / C
```

We need: 50 * M / C = 8.0575

Try M = 13, C = 81:  50 * 13 / 81 = 8.0247 MHz (error: -0.41%)
Try M = 15, C = 93:  50 * 15 / 93 = 8.0645 MHz (error: +0.087%)
Try M = 16, C = 99:  50 * 16 / 99 = 8.0808 MHz (error: +0.29%)

**Fractional-N approach** (better accuracy):

```
f_VCO = f_ref * (M + K/2^32) / N
```

With N=1, target f_VCO = ~805.75 MHz (choosing C=100):
```
M = 16
K = (0.115 * 2^32) = 494,103,962 = 0x1D70A3D6 (approx)
f_VCO = 50 * (16 + 0.115) = 805.75 MHz
f_out = 805.75 / 100 = 8.0575 MHz
```

Verification: 8.0575 / (512 * 264) = 59.600 Hz. Close enough (0.001% error).

Better precision with C=100:
```
Exact: f_out = 8.057499 MHz
f_VCO = 8.057499 * 100 = 805.7499 MHz
M + K/2^32 = 805.7499 / 50 = 16.114998
M = 16
K = 0.114998 * 2^32 = 494,094,565 = 0x1D707ED5
```

**Recommended approach**: Use a fractional PLL with M=15, N=1, C=93. This gives
8.0645 MHz, which results in refresh = 59.651 Hz -- only 0.087% off the 59.59949
target. The CRT will lock to this without issue.

If tighter accuracy is desired, use fractional mode: M=16, K=0x1D707ED5, N=1, C=100.

The PLL will be a new IP instance alongside the existing `pll` (which outputs
100 MHz for `clk_sys` and 20 MHz for `CLK_VIDEO`). The existing PLL must not be
modified -- it drives the entire Menu core logic. The new PLL provides only
`clk_pix` for the native video path.

### ce_pix Divider

The existing menu.sv uses `ce_pix` to divide `CLK_VIDEO` (20 MHz) by 2, producing
an effective 10 MHz pixel rate. For our native video path, we have two options:

**Option A (dedicated PLL)**: `clk_pix` runs at exactly the pixel clock rate.
`ce_pix = 1` always (every cycle is a pixel). This is simpler and recommended.

**Option B (divide from clk_sys)**: Run the timing generator on `clk_sys` (100 MHz)
with `ce_pix` asserted every ~12.4 cycles. This requires a fractional divider.
More complex, not recommended.

**Recommendation**: Option A -- a dedicated fractional PLL output at ~8.06 MHz,
with `ce_pix = 1`.

---

## 2. FPGA Module Specifications

### 2.1 Module: `pll_vid` (PLL IP Instance)

This is an Altera PLL IP core, not a hand-coded Verilog module. It will be
generated via Quartus Platform Designer or MegaWizard.

**Parameters**:
- Reference: 50.0 MHz
- Mode: Fractional-N (if tight accuracy needed) or Integer-N
- Output 0: ~8.065 MHz (pixel clock) -- Integer-N: M=15, N=1, C=93
- Output 1: not used (or same clock for DDR3 reader if desired)

**Files to create**: `rtl/pll_vid.v`, `rtl/pll_vid/pll_vid_0002.v` (auto-generated by Quartus IP).

### 2.2 Module: `native_video_timing`

Generates H/V sync, blanking, and data-enable signals from fixed modeline parameters.

```verilog
module native_video_timing (
    input  wire        clk,        // pixel clock (~8.06 MHz)
    input  wire        ce_pix,     // pixel clock enable (normally 1)
    input  wire        reset,      // synchronous reset

    output reg         hsync,      // active low
    output reg         vsync,      // active low
    output reg         hblank,
    output reg         vblank,
    output reg         de,         // data enable = ~(hblank | vblank)
    output reg  [9:0]  hcount,     // 0..511
    output reg  [8:0]  vcount,     // 0..263
    output reg         new_frame,  // single-cycle pulse at vblank start
    output reg         new_line    // single-cycle pulse at hblank start
);
```

**Behavioral description (cycle by cycle)**:

On every `clk` rising edge where `ce_pix` is high:
1. Increment `hcount`. When `hcount` reaches 511 (H_total-1), reset to 0 and
   increment `vcount`.
2. When `vcount` reaches 263 (V_total-1) and `hcount` wraps, reset `vcount` to 0.
3. Combinational outputs based on counter values:

```
hblank = (hcount >= 384)                    // pixels 384..511
hsync  = (hcount >= 400) && (hcount < 432)  // pixels 400..431, active high internally
         (inverted to active-low at output)
vblank = (vcount >= 224)                    // lines 224..263
vsync  = (vcount >= 227) && (vcount < 230)  // lines 227..229, active high internally
de     = ~hblank & ~vblank
new_frame = (vcount == 224) && (hcount == 0) // single pulse
new_line  = (hcount == 384)                  // single pulse (start of hblank)
```

**Localparam constants** (embedded in module, not ports):

```verilog
localparam H_ACTIVE  = 384;
localparam H_FP      = 16;
localparam H_SYNC    = 32;
localparam H_BP      = 80;
localparam H_TOTAL   = 512;  // 384+16+32+80

localparam V_ACTIVE  = 224;
localparam V_FP      = 3;
localparam V_SYNC    = 3;
localparam V_BP      = 34;
localparam V_TOTAL   = 264;  // 224+3+3+34
```

**Clock domain**: `clk_pix` (~8.06 MHz), single clock domain, no CDC.

**Estimated size**: ~60 lines of Verilog.

### 2.3 Module: `native_video_reader`

Reads pixel data from DDR3, buffers in line FIFOs, and outputs pixels
synchronized to the timing generator.

```verilog
module native_video_reader (
    // DDR3 Avalon-MM master (directly drives DDRAM_ ports)
    input  wire        ddr_clk,         // DDRAM_CLK (= clk_sys = 100 MHz)
    input  wire        ddr_busy,        // DDRAM_BUSY
    output reg   [7:0] ddr_burstcnt,    // DDRAM_BURSTCNT
    output reg  [28:0] ddr_addr,        // DDRAM_ADDR
    input  wire [63:0] ddr_dout,        // DDRAM_DOUT
    input  wire        ddr_dout_ready,  // DDRAM_DOUT_READY
    output reg         ddr_rd,          // DDRAM_RD
    output wire [63:0] ddr_din,         // DDRAM_DIN (unused, tie to 0)
    output wire  [7:0] ddr_be,          // DDRAM_BE (all 1s for reads)
    output wire        ddr_we,          // DDRAM_WE (unused, tie to 0)

    // Pixel output (clk_pix domain)
    input  wire        clk_pix,         // pixel clock
    input  wire        ce_pix,          // pixel clock enable
    input  wire        reset,

    // Timing inputs (from native_video_timing, clk_pix domain)
    input  wire        de,              // data enable
    input  wire        hblank,
    input  wire        vblank,
    input  wire        new_frame,       // pulse at start of vblank
    input  wire        new_line,        // pulse at start of hblank
    input  wire  [8:0] vcount,          // current line number

    // Pixel output
    output reg   [7:0] r_out,
    output reg   [7:0] g_out,
    output reg   [7:0] b_out,

    // Enable/status
    input  wire        enable,          // master enable from ARM config
    output wire        frame_ready      // indicates valid data being output
);
```

**State machine (DDR3 read side, clk_sys domain)**:

States:
```
IDLE        -> POLL_CTRL     (when enable & start of vblank detected via CDC)
POLL_CTRL   -> WAIT_CTRL     (issue DDR3 read of control word)
WAIT_CTRL   -> CHECK_CTRL    (when ddr_dout_ready)
CHECK_CTRL  -> READ_LINE     (if new frame detected; compute buffer base)
             -> IDLE          (if no new frame)
READ_LINE   -> WAIT_LINE     (issue burst reads for current scanline)
WAIT_LINE   -> READ_LINE     (continue if more bursts needed)
             -> LINE_DONE     (when full line read into FIFO)
LINE_DONE   -> READ_LINE     (advance to next line, if more lines in frame)
             -> IDLE          (when all 224 lines read)
```

**Startup blanking**: The `frame_ready` output must remain deasserted (and the
pixel output must be black/zero) until the first complete frame has been read
from DDR3 and loaded into the FIFO. Without this, the timing generator begins
producing DE pulses immediately on enable, and the FIFO read side would pop from
an empty FIFO, producing garbage for the first frame(s). The integration in
menu.sv should gate the VGA output: when `~frame_ready`, output black regardless
of the FIFO state.

**Stale frame timeout**: If the ARM stops writing frames (crash, exit, hang), the
reader should detect this by checking `frame_counter` in the control word. If the
counter has not changed for N consecutive vblanks (e.g., N=4, ~67ms), the module
should deassert `frame_ready` and blank the output to black rather than displaying
a frozen stale frame indefinitely. This provides a clean visual indication that the
game process has stopped.

**DDR3 burst parameters**:

One scanline = 384 pixels * 2 bytes = 768 bytes.
DDR3 bus width = 64 bits = 8 bytes per beat.
768 / 8 = 96 beats per scanline.

Maximum burst count for MiSTer's DDR3 controller = 128 beats.
So one scanline can be read in a single burst of 96 beats.

```verilog
ddr_burstcnt = 8'd96;  // 96 beats * 8 bytes = 768 bytes = 384 RGB565 pixels
```

**DDR3 address computation**:

The existing `ddram.sv` in the Menu core (lines 50-52) applies a fixed transform:
```verilog
assign DDRAM_ADDR = {3'b001, ram_address[28:3]}; // RAM at 0x20000000
```

This means the core's `ram_address` byte address is shifted right by 3 (since the
bus is 64-bit = 8-byte aligned) and has 3'b001 prepended (maps to 0x20000000 base
in physical DDR3).

**However**, for native video we do NOT use the existing `ddram.sv` module (which is
an 8-bit byte-at-a-time wrapper with caching, unsuitable for burst video reads).
Instead, we connect our reader directly to the `DDRAM_*` ports, replacing the
existing `ddram` instance. The existing `ddram` module in the Menu core is only
used for SDRAM clearing during boot (lines 347-368 of menu.sv), which we must
preserve.

**Approach**: Multiplex the DDRAM port between the existing `ddram` module (SDRAM
clear, boot-time only) and our video reader. The SDRAM clear completes during
`cfg[15]` initialization (state machine in menu.sv lines 269-345). After that,
the video reader takes over.

The native reader drives DDRAM_ADDR directly in the format expected by `sysmem`:
```verilog
// Physical DDR3 address for our buffer region:
// ARM physical address 0x3C000000 (in 1GB space)
// sysmem expects address[28:0] where bits[28:26] = 3'b001 for 0x20000000 region
// Physical 0x3C000000 = 0x20000000 + 0x1C000000
// In sysmem address space: base = 0x1C000000
// DDRAM_ADDR = {3'b001, byte_addr[28:3]} but we need:
//   byte_addr = 0x1C000000 + offset
//   DDRAM_ADDR = {3'b001, (0x1C000000 + offset)[28:3]}
//              = {3'b001, 0x03800000[25:0] + offset[28:3]}
//
// For control word at offset 0:
//   DDRAM_ADDR = 29'h0_E000000  (= {3'b001, 26'h3800000})
//
// More simply, from the ARM perspective 0x3C000000:
//   DDRAM_ADDR[28:3] = 0x3C000000[28:3] = 0x07800000
//   DDRAM_ADDR = {3'b001, 26'h7800000} -- wait, that's wrong.
```

Let me recalculate carefully. The `sysmem` Avalon-MM slave maps the 1GB DDR3.
The Avalon address is a 29-bit *word* address (64-bit words = 8-byte aligned):

```
Avalon address = physical_byte_address / 8
```

But looking at the actual `sysmem.sv` port `ram1_address[28:0]`, this is directly
the Avalon byte address with the top 3 bits indicating the region. Looking at the
existing `ddram.sv`:

```verilog
assign DDRAM_ADDR = {3'b001, ram_address[28:3]};
```

Here `ram_address` is a byte address relative to 0. The `{3'b001, ...}` maps it to
physical 0x20000000. So DDRAM_ADDR is a **qword address** (8-byte aligned) in the
full 32-bit address space:

```
DDRAM_ADDR = physical_byte_address / 8
```

Wait, 29 bits. Let's check: 2^29 = 512M entries * 8 bytes = 4GB. That covers the
full DE10-nano address space. The 3'b001 prefix = address range starting at
0x20000000/8 = 0x04000000 in qword space.

For our DDR3 buffer at physical address 0x3C000000:
```
DDRAM_ADDR = 0x3C000000 / 8 = 0x07800000
```
In 29-bit binary: `0_0111_1000_0000_0000_0000_0000_0000_0` = 29'h07800000.

For a scanline at offset `line * 768` from buffer base:
```
DDRAM_ADDR = (0x3C000000 + ctrl_size + buf_offset + line * 768) / 8
```

**FIFO specification**:

We use an Altera `dcfifo` (dual-clock FIFO) to bridge from the DDR3 clock domain
(clk_sys = 100 MHz) to the pixel clock domain (~8 MHz).

| Parameter | Value | Justification |
|-----------|-------|---------------|
| Width | 48 bits | 16-bit RGB565 in, expanded to {R8, G8, B8} = 24 bits per pixel; pack 3 pixels per 48-bit word. **Simpler**: 24 bits wide, one pixel per entry. |
| Depth | 1024 entries | Must hold at least 2 scanlines (2 * 384 = 768 pixels). 1024 is the next power of 2. |
| Write clock | clk_sys (100 MHz) | DDR3 read side |
| Read clock | clk_pix (~8 MHz) | Video output side |

Actually, the simplest approach: 24-bit wide FIFO, 1024 deep. Write side decodes
RGB565 to RGB888 and pushes one pixel per write. Read side pops one pixel per
`ce_pix` during `de`.

**RGB565 to RGB888 decode** (in DDR3 reader, write side of FIFO):

```verilog
// DDR3 delivers 64 bits = 4 RGB565 pixels per beat
wire [15:0] pixel_565 = ddr_dout[{pixel_index[1:0], 4'b0000} +: 16];
wire [7:0] r8 = {pixel_565[15:11], pixel_565[15:13]};  // 5-bit to 8-bit
wire [7:0] g8 = {pixel_565[10:5],  pixel_565[10:9]};   // 6-bit to 8-bit
wire [7:0] b8 = {pixel_565[4:0],   pixel_565[4:2]};    // 5-bit to 8-bit
```

This replicates MSBs into LSBs for proper 5/6-to-8-bit expansion. RGB565 layout
(matching Linux/SDL convention):

```
Bit:  15 14 13 12 11 | 10  9  8  7  6  5 |  4  3  2  1  0
      R4 R3 R2 R1 R0 | G5 G4 G3 G2 G1 G0 | B4 B3 B2 B1 B0
```

**Clock domain crossing**: The `new_frame` and `new_line` signals from the timing
generator (clk_pix domain) must be synchronized to clk_sys for the DDR3 reader
state machine. Use standard 2-FF synchronizers and edge detectors:

```verilog
reg [1:0] new_frame_sync;
always @(posedge ddr_clk) new_frame_sync <= {new_frame_sync[0], new_frame};
wire new_frame_ddr = new_frame_sync[0] & ~new_frame_sync[1]; // rising edge
```

This is safe because the crossing is slow-to-fast (~8 MHz → 100 MHz): a single-
cycle pulse at ~8 MHz is ~124 ns wide, which is guaranteed to be captured by
multiple 100 MHz sample edges (10 ns period). The reverse direction (fast-to-slow)
would NOT be safe with this approach.

The `enable` signal from ARM (set once at init) only needs a 2-FF synchronizer.

**Estimated size**: ~200 lines of Verilog.

### 2.4 Module: `native_video_top`

Top-level wrapper that instantiates the PLL, timing generator, DDR3 reader,
and FIFO, and provides clean interfaces to menu.sv.

```verilog
module native_video_top (
    input  wire        clk_50m,         // reference clock for PLL
    input  wire        clk_sys,         // system clock for DDR3 interface
    input  wire        reset,

    // DDR3 Avalon-MM master
    input  wire        ddr_busy,
    output wire  [7:0] ddr_burstcnt,
    output wire [28:0] ddr_addr,
    input  wire [63:0] ddr_dout,
    input  wire        ddr_dout_ready,
    output wire        ddr_rd,
    output wire [63:0] ddr_din,
    output wire  [7:0] ddr_be,
    output wire        ddr_we,

    // Video output
    output wire        clk_vid,         // pixel clock output (for CLK_VIDEO)
    output wire        ce_pix,          // pixel clock enable (always 1)
    output wire  [7:0] vga_r,
    output wire  [7:0] vga_g,
    output wire  [7:0] vga_b,
    output wire        vga_hs,
    output wire        vga_vs,
    output wire        vga_de,

    // Control
    input  wire        enable,          // from ARM: activate native video
    output wire        active,          // indicates module is outputting video
    output wire        vsync_out        // active-high vsync for frame sync
);
```

**Estimated size**: ~80 lines (instantiation and wiring).

---

## 3. DDR3 Memory Map

### Physical Addresses (ARM view)

The design document proposes base address 0x3C000000 (see reference to Groovy's
0x1C000000 relative to DDR3 region). Let me clarify the addressing:

The MiSTer DE10-nano has 1GB DDR3 mapped at physical address 0x00000000--0x3FFFFFFF.
Linux uses the lower portion. The FPGA-visible region starts at 0x20000000 (512MB
mark). The existing framebuffer lives at:

```
FB_ADDR = 0x20000000 + (32 * 1024 * 1024) = 0x22000000
```
(from `video.cpp` line 36)

Groovy uses base address 0x1C000000 relative to the DDR3 0x20000000 region, which is
physical 0x3C000000.

**We use a different region to avoid collision with both the framebuffer and any
Groovy region.** Proposed layout:

```
Physical address  | Purpose               | Size
------------------|-----------------------|--------
0x20000000        | Core framebuffer      | 32 MB
0x22000000        | Linux FB (3 buffers)  | ~24 MB (1920*1080*4*3)
0x3A000000        | Native video buffers  | 512 KB
  +0x000          |   Control word        | 256 bytes (aligned)
  +0x100          |   Buffer 0            | 172,032 bytes
  +0x2A300        |   Buffer 1            | 172,032 bytes
  +0x54500        |   (end)               | ~344 KB total used
```

**Rationale for 0x3A000000**: Leaves headroom above the Linux FB region
(~24MB from 0x22000000 = ends at ~0x39A00000), and sits well below 0x3C000000
(Groovy region), and well below 0x3FFFFFFF (1GB limit).

### Control Word Layout (at base + 0x000)

```
Byte offset  | Bits    | Field          | Description
-------------|---------|----------------|---------------------------
0x00         | [1:0]   | active_buffer  | 0 = buffer 0, 1 = buffer 1
             | [31:2]  | frame_counter  | Increments each frame (ARM writes)
0x04..0xFF   |         | reserved       | Pad to 256-byte alignment
```

The ARM writes the control word atomically (single 32-bit write). The FPGA polls
it once per vblank.

### Buffer Layout

Each buffer holds one complete frame in RGB565 format:

```
Buffer size = 384 * 224 * 2 = 172,032 bytes = 0x2A000 bytes
```

Alignment: buffers must be 8-byte aligned (DDR3 bus width). 0x100 and 0x2A300
are both 8-byte aligned. However, for burst efficiency, align to 256 bytes
(cache line size). Adjusted:

```
Control word: base + 0x000000 (256 bytes reserved)
Buffer 0:    base + 0x000100 (172,032 bytes)
Buffer 1:    base + 0x02A200 (next 256-byte boundary after buf0 end)
             buf0 end = 0x100 + 0x2A000 = 0x2A100, next 256 = 0x2A200
```

### DDRAM Address Computation (FPGA side)

Physical address 0x3A000000:
```
DDRAM_ADDR (29-bit) = 0x3A000000 / 8 = 0x07400000
```

For control word read:
```
ddr_addr = 29'h07400000;
ddr_burstcnt = 8'd1;  // single 8-byte read (contains the 4-byte control word)
```

For scanline N of buffer B:
```
buf_base_offset = (B == 0) ? 0x100 : 0x2A200;  // byte offsets from region base
scanline_offset = N * 768;                       // 384 * 2 bytes per scanline
byte_addr = 0x3A000000 + buf_base_offset + scanline_offset;
ddr_addr = byte_addr / 8;  // = byte_addr[28:3]
ddr_burstcnt = 8'd96;      // 768 / 8 = 96 qwords per scanline
```

**Note on DDRAM_ADDR format**: Looking at how `sysmem_lite` connects to the
HPS DDR3 controller, `ram1_address[28:0]` is an Avalon byte address in the range
0x00000000--0x1FFFFFFF (512MB). The physical DDR3 address is at offset 0x20000000
(bit 29 set). So:

```
ram1_address = physical_address - 0x20000000
```

For our base 0x3A000000:
```
ram1_address = 0x3A000000 - 0x20000000 = 0x1A000000
```

**Revised**: The existing `ddram.sv` constructs DDRAM_ADDR as
`{3'b001, ram_address[28:3]}` where `ram_address` is a raw byte address starting
from 0. The `3'b001` adds 0x20000000 to map into the DDR3 region. So a
`ram_address` of 0x1A000000 would produce:

```
DDRAM_ADDR = {3'b001, 0x1A000000[28:3]} = {3'b001, 26'h0_6800000}
           = 29'h0E800000
```

But we are NOT using `ddram.sv` for video reads -- we drive DDRAM_ADDR directly.
The correct formula is:

```
DDRAM_ADDR = physical_byte_address[31:3]
```

For 0x3A000000: `DDRAM_ADDR = 0x3A000000[31:3]` but DDRAM_ADDR is only 29 bits.
We need: `DDRAM_ADDR = 0x3A000000 >> 3 = 0x07400000`.

Let me verify with the existing code. `ddram.sv` line 52:
```
assign DDRAM_ADDR = {3'b001, ram_address[28:3]};
```
If `ram_address = 0x1A000000`:
- `ram_address[28:3]` = `0x1A000000[28:3]` = 26-bit value = 0x3400000
- DDRAM_ADDR = `{3'b001, 26'h3400000}` = 29'h07400000

So both approaches give `29'h07400000`. Confirmed.

**Summary of key addresses (DDRAM_ADDR values)**:

| Purpose | Physical address | DDRAM_ADDR (29-bit) |
|---------|-----------------|---------------------|
| Control word | 0x3A000000 | 29'h07400000 |
| Buffer 0 start | 0x3A000100 | 29'h07400020 |
| Buffer 1 start | 0x3A02A200 | 29'h07405440 |
| Buffer 0, line N | 0x3A000100 + N*768 | 29'h07400020 + N*96 |
| Buffer 1, line N | 0x3A02A200 + N*768 | 29'h07405440 + N*96 |

(Because 768 / 8 = 96 = 0x60 in DDRAM_ADDR units.)

---

## 4. ARM-Side Implementation Details

### 4.1 DDR3 Writer Module

**Header** (new file `src/port/sdl/native_video_writer.h`):

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool NativeVideoWriter_Init(void);
void NativeVideoWriter_Shutdown(void);
void NativeVideoWriter_WriteFrame(const void* pixels_rgb565, int width, int height, int pitch);
bool NativeVideoWriter_IsActive(void);
```

**Implementation** (new file `src/port/sdl/native_video_writer.c`):

```c
#include "native_video_writer.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#define NV_DDR_PHYS_BASE    0x3A000000u
#define NV_DDR_REGION_SIZE  0x00060000u  // 384KB, covers both buffers + control
#define NV_CTRL_OFFSET      0x00000000u
#define NV_BUF0_OFFSET      0x00000100u
#define NV_BUF1_OFFSET      0x0002A200u
#define NV_FRAME_BYTES      (384 * 224 * 2)  // 172,032

static int mem_fd = -1;
static volatile uint8_t* ddr_base = NULL;
static uint32_t frame_counter = 0;
static int active_buf = 0;

bool NativeVideoWriter_Init(void) {
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) { perror("NativeVideoWriter: open /dev/mem"); return false; }

    ddr_base = (volatile uint8_t*)mmap(NULL, NV_DDR_REGION_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, NV_DDR_PHYS_BASE);
    if (ddr_base == MAP_FAILED) {
        perror("NativeVideoWriter: mmap");
        ddr_base = NULL;
        close(mem_fd); mem_fd = -1;
        return false;
    }

    // Clear both buffers and control word
    memset((void*)(ddr_base + NV_BUF0_OFFSET), 0, NV_FRAME_BYTES);
    memset((void*)(ddr_base + NV_BUF1_OFFSET), 0, NV_FRAME_BYTES);
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = 0;
    frame_counter = 0;
    active_buf = 0;
    return true;
}

void NativeVideoWriter_Shutdown(void) {
    if (ddr_base) { munmap((void*)ddr_base, NV_DDR_REGION_SIZE); ddr_base = NULL; }
    if (mem_fd >= 0) { close(mem_fd); mem_fd = -1; }
}

void NativeVideoWriter_WriteFrame(const void* pixels_rgb565, int width, int height, int pitch) {
    if (!ddr_base || width != 384 || height != 224) return;

    uint32_t buf_offset = (active_buf == 0) ? NV_BUF0_OFFSET : NV_BUF1_OFFSET;
    volatile uint8_t* dst = ddr_base + buf_offset;

    if (pitch == 384 * 2) {
        // Contiguous: single memcpy
        memcpy((void*)dst, pixels_rgb565, NV_FRAME_BYTES);
    } else {
        // Row-by-row copy
        const uint8_t* src = (const uint8_t*)pixels_rgb565;
        for (int y = 0; y < 224; y++) {
            memcpy((void*)(dst + y * 384 * 2), src + y * pitch, 384 * 2);
        }
    }

    // Write ordering: on strongly-ordered/device memory (O_SYNC + MAP_SHARED),
    // ARM guarantees all prior writes (pixel data) complete before this write.
    // If this is ever changed to use cached memory + explicit flushes, a DSB
    // barrier must be inserted here before the control word write.
    frame_counter++;
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = (frame_counter << 2) | (active_buf & 1);

    // Swap buffer for next frame
    active_buf ^= 1;
}

bool NativeVideoWriter_IsActive(void) {
    return ddr_base != NULL;
}
```

**mmap parameters**:
- Address: `0x3A000000` (physical DDR3 address)
- Size: `0x60000` (384KB -- covers control + both buffers with margin)
- Flags: `MAP_SHARED` (visible to FPGA), `O_SYNC` on fd (uncached for coherency)

**Performance note**: With `O_SYNC`, the mmap'd region is strongly-ordered/device
memory, meaning all writes bypass the ARM L1/L2 caches and go directly to DDR3.
Sequential uncached writes on Cortex-A9 achieve ~100-200 MB/s, so the 172KB frame
write takes ~0.9-1.7ms (not the ~0.2ms sometimes estimated for cached memcpy).
This is comparable to the existing fbdev_presenter's uncached `/dev/fb0` write,
so the net performance gain comes from eliminating the per-pixel LUT scaling work,
not from faster writes. The ARGB8888-to-RGB565 conversion into a cached staging
buffer is fast (~0.1ms) and should always be done before the uncached copy.

### 4.2 Integration with Frame Presentation Dispatch

**File**: `src/port/sdl/sdl_app.c`

The presentation dispatch lives at approximately lines 9700--9870 (line numbers
shift as the file evolves; search for `fbdev_presenter_enabled`). Currently:

```c
if (fbdev_presenter_enabled) {
    if (fbdev_present_software_frame) {
        // ... FBDevPresenter_PresentSurface(...)
    } else if (fbdev_present_current_target) {
        // ... FBDevPresenter_PresentCurrentTarget(...)
    } else {
        FBDevPresenter_Present(renderer, fbdev_readback_rect);
    }
}
```

**Change**: Before the `fbdev_presenter_enabled` check, add a native video path:

At approximately line 9745, insert:

```c
if (native_video_writer_enabled) {
    // Get the software-rendered frame surface (384x224, already available)
    const SDL_Surface* frame = SDLGameRenderer_GetSoftwareFrameSurface();
    if (frame && frame->pixels) {
        // Convert ARGB8888 to RGB565 into a scratch buffer, then write
        // OR: if renderer already produces RGB565, write directly
        NativeVideoWriter_WriteFrame(rgb565_scratch, 384, 224, 384 * 2);
    }
}
```

The software renderer currently produces frames as `SDL_Surface` in ARGB8888 format
(384x224). We need an ARGB8888-to-RGB565 conversion step. This can be done with
a simple loop or NEON intrinsics:

```c
// ARGB8888 to RGB565 conversion (NEON-accelerated version exists in fbdev_presenter)
static void convert_argb8888_to_rgb565(const uint32_t* src, uint16_t* dst, int pixel_count) {
    for (int i = 0; i < pixel_count; i++) {
        uint32_t argb = src[i];
        uint8_t r = (argb >> 16) & 0xFF;
        uint8_t g = (argb >> 8) & 0xFF;
        uint8_t b = argb & 0xFF;
        dst[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
}
```

A NEON version can process 8 pixels at a time. At 384*224 = 86,016 pixels, even
the scalar version takes <0.2ms on the Cortex-A9.

**Scratch buffer**: Allocate a static 172,032-byte buffer for the RGB565 conversion
target. This is cached ARM memory, so the conversion runs at full speed, and then
a single `memcpy` to the uncached DDR3 mmap region.

### 4.3 Integration with Vsync-Based Pacing

**File**: `src/port/sdl/sdl_app.c` (approximately lines 9880--9900; search for
`frame_deadline` — line numbers shift as the file evolves)

Current timer-based pacing:
```c
// Do frame pacing
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
```

**Change for native video mode**: Replace the `SDL_DelayNS` sleep with a
`UIO_WAIT_VSYNC` SPI command that blocks until the FPGA's actual vblank signal.

```c
if (native_video_writer_enabled) {
    // Block until FPGA vblank (hardware-locked frame pacing)
    spi_uio_cmd(UIO_WAIT_VSYNC);
    now = SDL_GetTicksNS();
    frame_deadline = now + target_frame_time_ns;
} else {
    // Existing timer-based pacing
    if (now < frame_deadline) {
        Uint64 sleep_time = frame_deadline - now;
        SDL_DelayNS(sleep_time);
        now = SDL_GetTicksNS();
    }
    frame_deadline += target_frame_time_ns;
}
```

**Important**: `UIO_WAIT_VSYNC` (command `0x30`) is handled in `sys_top.v` line 382:
```verilog
if(io_din[7:0] == 'h30) vs_wait <= 1;
```

When `vs_wait` is set, the SPI IO acknowledge is held (line 259):
```verilog
if(~(io_wait | vs_wait) | io_strobe) begin
    rack <= io_clk;
    io_ack <= rack;
end
```

It clears on the next HDMI vsync (line 536):
```verilog
if(~vs_d2 & vs_d1) vs_wait <= 0;
```

**Issue**: `vs_wait` currently clears on `HDMI_TX_VS` (the HDMI output vsync).
When native video is active and `vga_fb=0`, the HDMI output still goes through
the scaler, which will be displaying the core's video output. Two problems:

1. The HDMI scaler vsync may have a phase offset relative to the native video
   vsync, causing the ARM to wake at the wrong point in the frame.
2. If no HDMI display is connected, the HDMI scaler may not generate vsync at
   all, causing the ARM to block indefinitely.

**Fix**: When native video is active, `vs_wait` should clear on the **core's own
vsync** (`vs_fix`), not on HDMI vsync. Use a mutually exclusive selection:

```verilog
// In sys_top.v, around line 536:
// Detect core vsync rising edge (clk_vid domain -> clk_sys CDC)
reg vs_core_d0, vs_core_d1, vs_core_d2;
vs_core_d0 <= vs_fix;
if(vs_core_d0 == vs_fix) vs_core_d1 <= vs_core_d0;
vs_core_d2 <= vs_core_d1;
wire core_vs_edge = ~vs_core_d2 & vs_core_d1;
wire hdmi_vs_edge = ~vs_d2 & vs_d1;

// Select based on native video mode (NATIVE_VID comes from cfg/status)
if((native_vid_active & core_vs_edge) | (~native_vid_active & hdmi_vs_edge))
    vs_wait <= 0;
```

This ensures the ARM unblocks on the correct vsync for the active video path.

### 4.4 Changes in thirdsarm_wrapper.cpp

**File**: `vendor/Main_MiSTer/thirdsarm_wrapper.cpp`

**Current code** (lines 1669--1677):
```cpp
if (!forced)
{
    disable_wrapper_osd();
    video_fb_clear(0);
    set_vga_fb(1);                // line 1673 - enables scaler path
    if (runtime_vt != active_vt) video_chvt(runtime_vt);
    video_fb_enable(1);           // line 1675 - enables FB in FPGA
    video_refresh_yc_mode();
}
```

**Change for native video mode**: When native video is requested, do NOT call
`set_vga_fb(1)` or `video_fb_enable(1)`. Instead:

```cpp
if (!forced)
{
    disable_wrapper_osd();
    if (native_video_mode) {
        // Do NOT set vga_fb=1, do NOT enable FB scaler path
        // The core will output video directly via VGA_R/G/B
        // set_vga_fb(0) is already the default
        if (runtime_vt != active_vt) video_chvt(runtime_vt);
        // Signal FPGA to start native video reader (via status bits or SPI cmd)
        // The native_video_writer on the game side handles the rest
    } else {
        video_fb_clear(0);
        set_vga_fb(1);
        if (runtime_vt != active_vt) video_chvt(runtime_vt);
        video_fb_enable(1);
    }
    video_refresh_yc_mode();
}
```

**Shutdown path** (lines 1437--1439 and 1491--1493):
```cpp
set_vga_fb(0);
video_fb_enable(0);
```

In native video mode, these calls are fine (they're already setting vga_fb=0, and
video_fb_enable(0) is harmless).

**How to detect native video mode**: Add an environment variable or configuration
option, e.g., `THIRDSARM_NATIVE_VIDEO=1` or a `[3s-arm]` section in `MiSTer.ini`.
The wrapper reads this before launching the game and sets it in the environment
for the game process.

**Caution: `user_io_send_buttons()` re-entrancy**: The `cfg` register in sys_top.v
(which carries `vga_fb` as bit 12) is written by `user_io_send_buttons()`. This
function is called from many places in the MiSTer framework, and each call
transmits the full button/config map including `CONF_VGA_FB`. In native video mode,
`set_vga_fb(0)` must be called before any path that triggers
`user_io_send_buttons()`, and the `vga_fb` flag must stay cleared. If any code
path accidentally sets the flag and calls `user_io_send_buttons()`, the scaler
path will be re-enabled and native video will silently stop reaching the DAC.

### 4.5 Changes in video.cpp

**File**: `vendor/Main_MiSTer/video.cpp`

**Minimal changes needed**. The key insight is: when native video mode is active,
we simply do NOT call `video_fb_enable(1)`. The existing `video_fb_enable(0)` call
on shutdown is harmless.

The only change needed is making `video_refresh_yc_mode()` aware that when native
video is active on the core path, the YC/S-Video encoding happens via `yc_out`
(core path, `clk_vid`), not `yc_out_fb` (scaler path, `clk_hdmi`). The phase
increment calculation in `video_refresh_yc_mode()` (around line 3018) currently
uses `output_CLK_VIDEO` from the scaler config (`v_cur.Fpix`). For native video,
it should use the pixel clock from our PLL (~8.06 MHz).

Line 3024:
```cpp
const double core_CLK_VIDEO = current_video_info.ctime * 100.f / current_video_info.ptime;
```

For native video mode, override:
```cpp
const double core_CLK_VIDEO = native_video_enabled ? 8.0645 : (current_video_info.ctime * 100.f / current_video_info.ptime);
```

This ensures the S-Video color subcarrier phase increment is calculated correctly
for the actual pixel clock.

---

## 5. sys_top.v Integration

### 5.1 DAC Mux Logic Analysis

The DAC output mux (lines 1537--1539, in the `MISTER_DISABLE_YC` ifndef branch):

```verilog
assign VGA_R = av_dis ? 6'bZZZZZZ : vga_fb_yc_en ? yc_fb_o[23:18] : vgas_en ? vgas_o[23:18] : VGA_DISABLE ? 6'd0 : vga_o[23:18];
```

The selection priority is:
1. `av_dis` = high-Z (analog video disabled)
2. `vga_fb_yc_en` = YC-encoded framebuffer (S-Video/CVBS via scaler)
3. `vgas_en` = scaler output (vga_fb | vga_scaler)
4. `VGA_DISABLE` = black
5. `vga_o` = core video output (this is what we want)

When `vga_fb=0` and `vga_scaler=0`:
- `vgas_en = 0`
- `vga_fb_yc_en = 0`
- Selection falls through to `vga_o` -- **the core's own video output**

This is exactly what we need. **No changes to the DAC mux logic.**

### 5.2 Clock Selection

VGA clock mux (line 1332--1337):
```verilog
cyclonev_clkselect vga_clk_sw
(
    .clkselect({1'b1, ~vga_fb & ~vga_scaler}),
    .inclk({clk_vid, hdmi_clk_out, 2'b00}),
    .outclk(vga_tx_clk)
);
```

When `vga_fb=0` and `vga_scaler=0`:
- `clkselect = {1'b1, 1'b1} = 2'b11`
- Selected clock: `inclk[3]` = `clk_vid`

So the VGA DAC clock is `clk_vid`, which is the core's video clock. Currently in
the Menu core, `CLK_VIDEO` comes from the existing PLL output 1 at 20 MHz.

**Change needed**: When native video is active, `CLK_VIDEO` must be our pixel clock
(~8.06 MHz) instead of 20 MHz. This means the `emu` module's `CLK_VIDEO` output
must switch between the menu's 20 MHz clock and our ~8.06 MHz pixel clock.

In `menu.sv`, modify the `CLK_VIDEO` assignment:

Current (line 186):
```verilog
assign CE_PIXEL  = ce_pix;
```

Current PLL (lines 231-238):
```verilog
pll pll
(
    .refclk(CLK_50M),
    .rst(0),
    .outclk_0(clk_sys),    // 100 MHz
    .outclk_1(CLK_VIDEO),  // 20 MHz
    .locked(locked)
);
```

**Modification**: The native video module provides its own clock. When enabled:

```verilog
// In menu.sv:
wire clk_pix;  // from native video PLL
wire native_vid_active;

assign CLK_VIDEO = native_vid_active ? clk_pix : clk_20m;
assign CE_PIXEL  = native_vid_active ? 1'b1 : ce_pix;
```

However, **dynamically switching CLK_VIDEO with a mux is not safe** -- the
`clk_vid` net is used as a clock throughout `sys_top.v` (OSD, scanlines, vga_out,
etc.). Switching clocks glitch-free requires the `cyclonev_clkselect` primitive
which is already used for the VGA DAC clock.

**Better approach**: Use `cyclonev_clkselect` inside menu.sv for CLK_VIDEO:

```verilog
wire clk_20m;  // existing PLL output 1
wire clk_pix;  // native video PLL output

pll pll
(
    .refclk(CLK_50M),
    .rst(0),
    .outclk_0(clk_sys),
    .outclk_1(clk_20m),
    .locked(locked)
);

pll_vid pll_vid_inst
(
    .refclk(CLK_50M),
    .rst(0),
    .outclk_0(clk_pix),
    .locked(pll_vid_locked)
);

wire clk_video_sel;
cyclonev_clkselect vid_clk_sw
(
    .clkselect({1'b1, native_vid_active}),
    .inclk({clk_pix, clk_20m, 2'b00}),
    .outclk(clk_video_sel)
);

assign CLK_VIDEO = clk_video_sel;
```

### 5.3 Mode Toggle (Init-Time Switch)

The ARM signals native video mode via a status register bit. The simplest mechanism
uses the existing `status` register already available via `hps_io`.

**Important**: `NATIVE_VID` should only be set/cleared during initialization (before
game launch or during shutdown), not toggled at runtime. The `cyclonev_clkselect`
mux for `CLK_VIDEO` is glitch-free only when both clocks are running and the select
input is stable. Toggling during active video output may cause a brief clock glitch
(~2 clock cycles) that propagates through all downstream logic clocked by `clk_vid`
(scanlines, OSD, YC encoder, DAC output).

Current `menu.sv` line 480-481:
```verilog
wire PAL = status[4];
wire FB  = status[5];
```

Add a new status bit:
```verilog
wire NATIVE_VID = status[9];  // Native video enable
```

The ARM sets this via `user_io_status_set("[9]", 1)` in the wrapper before
launching the game.

**`native_vid_active`** should only be asserted when both the ARM has requested it
AND the native video PLL is locked AND valid frame data has been detected:

```verilog
wire native_vid_active = NATIVE_VID & pll_vid_locked & frame_data_valid;
```

### 5.4 HDMI Output Behavior

When native video is active (`vga_fb=0`, `vga_scaler=0`):

The HDMI output path is:
```
core VGA_R/G/B/HS/VS/DE (menu.sv outputs)
  -> sync_fix (line 1731-1732)
  -> scanlines VGA_scanlines (line 1371)
  -> vga_data_sl -> assigned to hr_out, hg_out, hb_out (lines 1738-1743)
  -> ascal input (i_r, i_g, i_b, i_hs, i_vs, i_de) (lines 739-745)
  -> ascal output -> hdmi_data (lines 754-756)
  -> hdmi_osd (line 1171)
  -> HDMI_TX_D (line 1325)
```

The `ascal` scaler sees the core's own video output and scales it to the HDMI
resolution. This works automatically -- the scaler handles arbitrary input
resolutions including our 384x224 @ ~60Hz.

HDMI clock mux (line 1252):
```verilog
cyclonev_clkselect hdmi_clk_sw
(
    .clkselect({1'b1, ~vga_fb & direct_video}),
    .inclk({clk_vid, hdmi_clk_out, 2'b00}),
    .outclk(hdmi_tx_clk)
);
```

The `inclk` array maps as: `inclk[3]=clk_vid`, `inclk[2]=hdmi_clk_out`,
`inclk[1]=0`, `inclk[0]=0`.

When `vga_fb=0` and `direct_video=0` (we do NOT set direct_video):
- `clkselect = {1'b1, 1'b0} = 2'b10`
- Selected clock: `inclk[2]` = `hdmi_clk_out` (the HDMI PLL clock)

When `vga_fb=0` and `direct_video=1`:
- `clkselect = {1'b1, 1'b1} = 2'b11`
- Selected clock: `inclk[3]` = `clk_vid` (the core's pixel clock, ~8 MHz)

HDMI uses `hdmi_clk_out` for normal HDMI output. The scaler operates on
`clk_hdmi` and outputs to the HDMI PLL clock. For `direct_video=1`, HDMI
gets the same raw signal as analog — the existing framework handles this.

**HDMI output works correctly with no changes.**

The only consideration: if `direct_video` is 1 (MiSTer.ini setting for direct
analog video), the HDMI mux selects `clk_vid` (the core's clock) for HDMI output.
In that mode, HDMI gets the same signal as analog. This is already handled by the
existing framework and is not affected by our changes.

### 5.5 OSD Overlay Signal Path

**Status: CONFIRMED — OSD works with no FPGA or ARM-side changes.**

#### Complete Analog Signal Chain with OSD

When `vga_fb=0` (native video active), the full signal path is:

```
┌─────────────────────────────────────────────────────────────┐
│ emu module (menu.sv)                                         │
│  .VGA_R(r_out), .VGA_G(g_out), .VGA_B(b_out)   [8-bit RGB] │
│  .VGA_HS(hs_emu), .VGA_VS(vs_emu), .VGA_DE(de_emu)         │
│  .CLK_VIDEO → clk_vid, .CE_PIXEL → ce_pix                  │
└────────────────────────┬────────────────────────────────────┘
                         │ clk_vid domain
                         ↓
        ┌────────────────────────────────────────────┐
        │ sync_fix (metastability guard)             │
        │  hs_emu → hs_fix, vs_emu → vs_fix         │
        └────────────────┬───────────────────────────┘
                         ↓
        ┌────────────────────────────────────────────┐
        │ scanlines #(0) VGA_scanlines  (line 1371)  │
        │  .clk(clk_vid)                             │
        │  .din(de_emu ? {r_out,g_out,b_out} : 24'd0)│
        │  .hs_in(hs_fix), .vs_in(vs_fix)           │
        │  .de_in(de_emu)                            │
        │  → vga_data_sl[23:0], vga_hs_sl, vga_vs_sl│
        │    vga_de_sl, vga_ce_sl                    │
        └────────────────┬───────────────────────────┘
                         ↓
        ┌─────────────────────────────────────────────────┐
        │ osd vga_osd  (line 1391)                        │
        │  .clk_sys(clk_sys)     ← ARM SPI commands (CDC)│
        │  .clk_video(clk_vid)   ← pixel clock domain    │
        │  .io_osd(io_osd_vga)   ← SPI select: ss1 & ~ss2│
        │  .io_strobe(io_strobe), .io_din(io_din[15:0])  │
        │                                                  │
        │  .din(vga_data_sl)     ← scanlines output       │
        │  .hs_in(vga_hs_sl), .vs_in(vga_vs_sl)          │
        │  .de_in(vga_de_sl)                              │
        │                                                  │
        │  Compositing: 4KB bitmap buffer, 8x8 char glyphs│
        │  3-stage pipeline (~375ns at 8MHz)              │
        │                                                  │
        │  → vga_data_osd[23:0]  ← composited RGB         │
        │  → vga_hs_osd, vga_vs_osd, vga_de_osd          │
        └────────────────┬────────────────────────────────┘
                         ↓
        ┌─────────────────────────────────────────────────┐
        │ csync  (line 1413)                              │
        │  → vga_cs_osd (composite sync)                  │
        └────────────────┬────────────────────────────────┘
                         ↓
        ┌─────────────────────────────────────────────────┐
        │ IF yc_en: yc_out (line 1426) — Y/C encoder     │
        │   .din(vga_data_osd), .hsync/.vsync/.de         │
        │   → yc_o[23:0] (S-Video/CVBS)                  │
        │ ELSE: vga_out (line 1509) — DC restoration      │
        │   .din(vga_data_osd)                            │
        │   → vga_o[23:0] (RGB)                           │
        └────────────────┬────────────────────────────────┘
                         ↓
        ┌──────────────────────────────────────────┐
        │ DAC output mux  (line 1537)              │
        │ vga_fb=0, vgas_en=0 → selects vga_o     │
        │  VGA_R[5:0] = vga_o[23:18]              │
        │  VGA_G[5:0] = vga_o[15:10]              │
        │  VGA_B[5:0] = vga_o[7:2]                │
        └──────────────────────────────────────────┘
```

#### Why It Works Without Changes

1. **Hardwired in pipeline**: `vga_osd` is always between scanlines and
   the DAC when `vga_fb=0`. There is no bypass. It composites on whatever
   RGB data flows through, regardless of pixel origin.

2. **Clock-agnostic**: The OSD module takes `clk_video` as input. At our
   ~8MHz pixel clock it functions identically to any other clock rate.
   The `ce_pix` enable is not used by the OSD — it processes every
   `clk_video` edge where `de_in` is valid.

3. **Two independent instances**: `vga_osd` (clocked `clk_vid`, for analog)
   and `hdmi_osd` (clocked `clk_hdmi`, for HDMI) operate independently.
   SPI chip selects: `io_osd_vga = io_ss1 & ~io_ss2`,
   `io_osd_hdmi = io_ss1 & ~io_ss0`.

4. **CDC handled internally**: OSD bitmap is written via SPI on `clk_sys`
   domain. The OSD module has both `clk_sys` and `clk_video` inputs and
   handles the clock domain crossing internally (dual-port RAM with
   separate read/write clocks).

5. **ARM-side code unchanged**: `OsdEnable()`, `OsdDisable()`, `OsdWrite()`,
   `OsdUpdate()` in `osd.cpp` send SPI commands that address the OSD module
   directly via `EnableOsd()`/`DisableOsd()` chip select control in `spi.cpp`.
   The wrapper menu (`draw_wrapper_menu()`, `service_wrapper_menu()` in
   `thirdsarm_wrapper.cpp`) works identically.

6. **`osd_target` defaults to `OSD_ALL`**: Both VGA and HDMI OSD instances
   receive data simultaneously. No code changes this default.

#### OSD Sizing at Native Resolution

The OSD renders at a fixed character size: 8x8 pixel glyphs, 256 bytes wide
per line, up to 19 lines. The OSD uses `de_in` to detect the visible area
and centers itself within it.

At 384x224 (vs the previous ~640x480 scaler output), the OSD will appear
**proportionally larger** — covering more of the game area. This is the same
behavior as native FPGA cores at low resolutions (e.g., NES at 256x240).

The wrapper currently uses `OsdSetSize(11)` for the menu. At 224 lines tall,
an 11-line OSD (11 × 8 = 88 pixels) covers ~39% of the vertical space.
This is functional but visually prominent.

**Mitigation options** (if needed, all cosmetic):
- Reduce menu to fewer lines via `OsdSetSize()` (e.g., 8 lines = 64px = 29%)
- Accept it as-is (matches native core behavior, users expect this)
- No FPGA changes needed for either option

#### Implementation Requirements

**FPGA side**: None. The existing `vga_osd` instantiation in `sys_top.v`
works as-is. Our native video module outputs `VGA_R/G/B/HS/VS/DE` from the
`emu` module, which feeds into `r_out/g_out/b_out` → scanlines → `vga_osd`
automatically.

**ARM side**: None. The wrapper's OSD calls (`osd.cpp`, `thirdsarm_wrapper.cpp`)
work identically. No SPI protocol changes, no chip select changes, no
rendering changes.

**Testing**: Verify OSD renders correctly at 384x224 by opening the wrapper
menu during gameplay. Check that text is legible and properly positioned
within the visible area.

### 5.6 YC / S-Video Behavior

When native video is active with `vga_fb=0`:
- `vga_fb_yc_en = vga_fb & ~vga_scaler & yc_en = 0` (line 1424)
- The `yc_out` module (core path, line 1426) processes `vga_data_osd` on `clk_vid`
- The `yc_out_fb` module (scaler path, line 1445) is not used for DAC output

The `yc_out` module receives the OSD-composited video and generates YC-encoded
output for S-Video/CVBS. When `yc_en=1`, the DAC mux selects `yc_o` instead of
`vga_o_t` (line 1526):
```verilog
assign {vga_o, vga_hs, vga_vs, vga_cs, vga_de} =
    ~yc_en ? {vga_o_t, ...} : {yc_o, yc_hs, yc_vs, yc_cs, yc_de};
```

**S-Video/CVBS works correctly on the core path** -- the existing YC encoder
processes whatever video the core outputs, running on `clk_vid`. Our pixel clock
change is transparent to it, as long as the phase increment (subcarrier frequency)
is calculated for the correct pixel clock. That's handled by the ARM-side
`video_refresh_yc_mode()` change described in section 4.5.

---

## 6. Detailed menu.sv Integration

### 6.1 What to Add

The following changes are needed in `menu.sv`:

**After the existing PLL (line 238)**, add the native video PLL and clock mux:

```verilog
// --- Native video clock ---
wire clk_pix;
wire pll_vid_locked;
pll_vid pll_vid_inst
(
    .refclk(CLK_50M),
    .rst(0),
    .outclk_0(clk_pix),
    .locked(pll_vid_locked)
);

wire clk_20m;
// NOTE: Modify existing PLL to name output 1 as clk_20m instead of CLK_VIDEO directly.
// Then select between clk_20m and clk_pix:

wire NATIVE_VID = status[9];

cyclonev_clkselect vid_clk_sw
(
    .clkselect({1'b1, NATIVE_VID & pll_vid_locked}),
    .inclk({clk_pix, clk_20m, 2'b00}),
    .outclk(CLK_VIDEO)
);
```

**Modify the existing PLL instantiation** (lines 231--238):

```verilog
wire clk_20m;
pll pll
(
    .refclk(CLK_50M),
    .rst(0),
    .outclk_0(clk_sys),
    .outclk_1(clk_20m),       // was CLK_VIDEO directly
    .locked(locked)
);
```

**Modify CE_PIXEL** (line 185):

```verilog
assign CE_PIXEL = NATIVE_VID ? 1'b1 : ce_pix;
```

**Modify the video output section** (lines 475--558). When `NATIVE_VID` is active,
the VGA outputs come from the native video reader instead of the random pattern
generator.

Replace (lines 553--558):
```verilog
assign VGA_DE  = ~(HBlank | VBlank);
assign VGA_HS  = HSync;
assign VGA_VS  = VSync;
assign VGA_G   = comp_v;
assign VGA_R   = comp_v;
assign VGA_B   = comp_v;
```

With:
```verilog
// Native video signals
wire [7:0] nv_r, nv_g, nv_b;
wire nv_hs, nv_vs, nv_de;
wire nv_active;

native_video_top native_video
(
    .clk_50m(CLK_50M),
    .clk_sys(clk_sys),
    .reset(RESET),

    // DDR3 -- directly wired to DDRAM ports (shared with existing ddram module)
    .ddr_busy(DDRAM_BUSY),
    .ddr_burstcnt(nv_ddr_burstcnt),
    .ddr_addr(nv_ddr_addr),
    .ddr_dout(DDRAM_DOUT),
    .ddr_dout_ready(DDRAM_DOUT_READY),
    .ddr_rd(nv_ddr_rd),
    .ddr_din(),
    .ddr_be(),
    .ddr_we(),

    // Video output
    .clk_vid(clk_pix),
    .ce_pix(),
    .vga_r(nv_r),
    .vga_g(nv_g),
    .vga_b(nv_b),
    .vga_hs(nv_hs),
    .vga_vs(nv_vs),
    .vga_de(nv_de),

    .enable(NATIVE_VID),
    .active(nv_active),
    .vsync_out()
);

assign VGA_DE  = NATIVE_VID ? nv_de    : ~(HBlank | VBlank);
assign VGA_HS  = NATIVE_VID ? nv_hs    : HSync;
assign VGA_VS  = NATIVE_VID ? nv_vs    : VSync;
assign VGA_R   = NATIVE_VID ? nv_r     : comp_v;
assign VGA_G   = NATIVE_VID ? nv_g     : comp_v;
assign VGA_B   = NATIVE_VID ? nv_b     : comp_v;
```

### 6.2 DDR3 Port Sharing

The existing `ddram` module (lines 347--355) uses the DDRAM port for SDRAM clearing
during boot. It has these connections:

```verilog
ddram ddr
(
    .*,
    .reset(RESET),
    .dout(),
    .din(0),
    .rd(0),
    .ready()
);
```

The `.*` auto-connects `DDRAM_CLK`, `DDRAM_BUSY`, `DDRAM_BURSTCNT`, `DDRAM_ADDR`,
etc. But looking carefully, the existing `ddram` module only writes (for SDRAM
clearing) -- its `rd` input is tied to 0 and `din` is tied to 0. The writes happen
during the boot state machine (lines 360-368).

**Sharing approach**: Since the boot-time SDRAM clearing (`ddram` module) completes
before native video starts (the game hasn't even launched yet), we can use a simple
priority mux:

```verilog
wire [7:0]  old_ddr_burstcnt;
wire [28:0] old_ddr_addr;
wire        old_ddr_rd;
wire [63:0] old_ddr_din;
wire [7:0]  old_ddr_be;
wire        old_ddr_we;

wire [7:0]  nv_ddr_burstcnt;
wire [28:0] nv_ddr_addr;
wire        nv_ddr_rd;

// Existing ddram module (for SDRAM clearing)
ddram ddr
(
    .DDRAM_CLK(clk_sys),
    .DDRAM_BUSY(DDRAM_BUSY),
    .DDRAM_BURSTCNT(old_ddr_burstcnt),
    .DDRAM_ADDR(old_ddr_addr),
    .DDRAM_DOUT(DDRAM_DOUT),
    .DDRAM_DOUT_READY(DDRAM_DOUT_READY),
    .DDRAM_RD(old_ddr_rd),
    .DDRAM_DIN(old_ddr_din),
    .DDRAM_BE(old_ddr_be),
    .DDRAM_WE(old_ddr_we),
    .reset(RESET),
    .dout(),
    .din(0),
    .rd(0),
    .ready()
);

// Mux: native video reader takes over after boot
wire use_nv = NATIVE_VID & cfg[15];  // cfg[15] = boot complete

assign DDRAM_CLK      = clk_sys;
assign DDRAM_BURSTCNT = use_nv ? nv_ddr_burstcnt : old_ddr_burstcnt;
assign DDRAM_ADDR     = use_nv ? nv_ddr_addr     : old_ddr_addr;
assign DDRAM_RD       = use_nv ? nv_ddr_rd       : old_ddr_rd;
assign DDRAM_DIN      = use_nv ? 64'd0           : old_ddr_din;
assign DDRAM_BE       = use_nv ? 8'hFF           : old_ddr_be;
assign DDRAM_WE       = use_nv ? 1'b0            : old_ddr_we;
```

Note: `DDRAM_CLK = clk_sys` is already assigned at line 184 -- no change needed.
The `.*` auto-connection on the `ddram` instance must be replaced with explicit
connections.

---

## 7. Build System

### 7.1 ARM Side (CMakeLists.txt)

The new source files need to be added:

```
src/port/sdl/native_video_writer.c
include/port/sdl/native_video_writer.h
```

Add to the source file list in `CMakeLists.txt` alongside the existing
`fbdev_presenter.c` entry (search for `fbdev_presenter` in the CMakeLists.txt).
These files should be guarded with `#if defined(PORT_MISTER)` like `fbdev_presenter.c`.

No additional libraries are needed -- the code uses only `/dev/mem` mmap and
standard memcpy.

### 7.2 FPGA Side (Quartus Project)

**Files to add to `files.qip`**:

```
set_global_assignment -name SYSTEMVERILOG_FILE rtl/native_video_top.sv
set_global_assignment -name SYSTEMVERILOG_FILE rtl/native_video_timing.sv
set_global_assignment -name SYSTEMVERILOG_FILE rtl/native_video_reader.sv
set_global_assignment -name QIP_FILE rtl/pll_vid.qip
set_global_assignment -name QIP_FILE rtl/dcfifo_native.qip
```

**PLL IP generation**: Use Quartus MegaWizard or Platform Designer to create
`pll_vid` with:
- Input: 50 MHz
- Output 0: 8.064516 MHz (Integer-N: M=15, N=1, C=93) or the fractional equivalent
- Device: Cyclone V 5CSEBA6U23I7 (DE10-nano FPGA)

**DCFIFO IP generation**: Use Quartus MegaWizard to create a dual-clock FIFO:
- Data width: 24 bits (RGB888)
- Depth: 1024 entries (next power of 2 above 2 * 384)
- Write clock: clk_sys (100 MHz)
- Read clock: clk_pix (~8 MHz)
- Show-ahead mode (read data available before rdreq)

### 7.3 Quartus Build Command

The Menu core is built with the standard MiSTer Quartus flow. The `menu.qsf`
already sources `files.qip`. Adding new files to `files.qip` is sufficient.

Build: `quartus_sh --flow compile menu` from the `vendor/Menu_MiSTer` directory.

---

## 8. Testing Plan

### 8.1 Modeline Verification (CRT acceptance)

1. Build the FPGA bitstream with native video enabled, but outputting a test pattern
   (e.g., color bars or checkerboard) instead of DDR3 pixel data.
2. Connect to a 15kHz CRT (PVM, BVM, or consumer CRT via RGB).
3. Verify the CRT locks to the signal (stable picture, no rolling).
4. Measure with oscilloscope:
   - H sync frequency: should be ~15,737 Hz
   - V sync frequency: should be ~59.60 Hz
   - H sync pulse width: ~4 us (32 pixels / 8.06 MHz)
5. If the CRT does not lock, adjust H_TOTAL and V_TOTAL by +/- 1 to fine-tune.

### 8.2 Pixel Data Integrity and Endianness

1. ARM writes a known test pattern to DDR3 (e.g., vertical color bars at specific
   RGB565 values).
2. Capture the analog output with an oscilloscope or video capture card.
3. Verify that pixel values match: correct colors, no shifted columns, no garbled
   lines.
4. Test double-buffer switching: alternate between two different patterns and verify
   no tearing or mixed frames.
5. **Endianness verification**: Write specific RGB565 values and verify the FPGA
   outputs the correct color channels:
   - Pure red: `0xF800` → R=255, G=0, B=0 on DAC
   - Pure green: `0x07E0` → R=0, G=255, B=0 on DAC
   - Pure blue: `0x001F` → R=0, G=0, B=255 on DAC
   ARM Cortex-A9 is little-endian; the Cyclone V f2sdram interface delivers data
   in the byte order it was written. Both sides are little-endian, so
   `DDRAM_DOUT[15:0]` should contain the first pixel, `DDRAM_DOUT[31:16]` the
   second, etc. This test catches any byte-order mismatch empirically.

### 8.3 Latency Measurement

1. Use a photodiode/LED circuit connected to a button input and CRT screen.
2. On button press, change the screen to white.
3. Measure the time from button press to light detection.
4. Compare with the existing scaler path (should be ~15-30ms improvement).
5. Alternative: use the MiSTer's built-in latency test if available, or MiSTer
   latency tester hardware.

### 8.4 Frame Pacing Verification

1. Enable the FPS overlay (`SDLApp_ToggleFPSOverlay()`).
2. With native video + vsync pacing: frame time should be rock-solid at 16.78ms
   (1/59.60) with <0.1ms jitter.
3. Compare with timer-based pacing: should show 1-4ms jitter reduction.
4. Run a scene with constant scrolling (e.g., bonus stage) and observe for
   micro-stutter visually.

### 8.5 Regression Testing

1. **HDMI output**: With native video active on analog, verify HDMI still works
   (picture appears, correct resolution, OSD visible).
2. **Scaler path fallback**: Toggle native video off at runtime; verify the
   framebuffer/scaler path resumes correctly.
3. **OSD**: Open the wrapper OSD menu (menu button) while in native video mode:
   - Verify menu text renders and is legible over the game video.
   - Verify menu navigation (up/down/select) works correctly.
   - Verify OSD dismiss (resume / menu button again) removes the overlay cleanly.
   - Note: at 384x224, the 11-line OSD (88px) covers ~39% of vertical space.
     This is expected and matches native core behavior at low resolutions.
     If too large, `OsdSetSize()` in wrapper can be reduced (ARM-only change).
4. **S-Video/CVBS**: If using an S-Video or composite connection, verify correct
   color encoding (no grayscale, no wrong color phase).
5. **Audio**: Verify audio path is unaffected (no glitches, no sync issues).

### 8.6 Fallback/Rollback Procedure

The native video path is activated only when:
1. The `NATIVE_VID` status bit is set (controlled by ARM)
2. The native video PLL is locked
3. Valid frame data is detected in DDR3

If any condition fails, the module is inactive and the Menu core's default video
output (random pattern) is used. If the game crashes or doesn't write frames,
the display shows black (zero-initialized buffers), not garbage.

To rollback completely:
1. Remove `NATIVE_VID` status bit setting from the wrapper
2. Rebuild ARM code only -- the FPGA side is harmless when disabled
3. Or: rebuild FPGA without the native video modules (remove from files.qip)

The existing scaler path (`video_fb_enable(1)` / `set_vga_fb(1)`) remains fully
functional and is the default when native video is not enabled.

---

## Appendix A: Key File References

| File | Key Lines | Purpose |
|------|-----------|---------|
| `vendor/Menu_MiSTer/menu.sv` | 23-178 | Module ports (VGA, DDRAM) |
| `vendor/Menu_MiSTer/menu.sv` | 184-186 | DDRAM_CLK, CE_PIXEL assignments |
| `vendor/Menu_MiSTer/menu.sv` | 231-238 | PLL instantiation (clk_sys=100MHz, CLK_VIDEO=20MHz) |
| `vendor/Menu_MiSTer/menu.sv` | 347-355 | ddram module instantiation |
| `vendor/Menu_MiSTer/menu.sv` | 475-558 | Video timing and output (to be augmented) |
| `vendor/Menu_MiSTer/rtl/ddram.sv` | 27-132 | DDR3 byte-access wrapper (to be shared/replaced) |
| `vendor/Menu_MiSTer/rtl/pll.v` | entire | Existing PLL IP (100MHz + 20MHz outputs) |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 288-296 | `vga_fb`, `direct_video`, `vga_scaler` definitions |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 331-382 | `vs_wait` (UIO_WAIT_VSYNC) implementation |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 592-638 | sysmem DDR3 port connections (ram1 = core, ram2 = audio) |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 702-730 | ascal scaler instantiation |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 1029-1037 | HDMI PLL instantiation |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 1171-1189 | HDMI OSD instantiation |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 1252-1257 | HDMI clock mux (cyclonev_clkselect) |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 1332-1337 | VGA clock mux (cyclonev_clkselect) |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 1369-1410 | VGA scanlines + OSD chain |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 1424-1462 | YC out (core path) and YC out FB (scaler path) |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 1470 | `vgas_en = vga_fb \| vga_scaler` |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 1534-1554 | DAC output mux (VGA_R/G/B/HS/VS) |
| `vendor/Menu_MiSTer/sys/sys_top.v` | 1784-1814 | `emu` module instantiation |
| `vendor/Menu_MiSTer/sys/hps_io.sv` | 174-194 | EXT_BUS interface |
| `vendor/Menu_MiSTer/files.qip` | entire | Source file list for Quartus |
| `vendor/Main_MiSTer/video.cpp` | 35-36 | FB_SIZE, FB_ADDR definitions |
| `vendor/Main_MiSTer/video.cpp` | 3277-3346 | `video_fb_enable()` function |
| `vendor/Main_MiSTer/video.cpp` | 3018-3036 | YC mode refresh (pixel clock calculation) |
| `vendor/Main_MiSTer/user_io.cpp` | 2928-2938 | `set_vga_fb()` / `get_vga_fb()` |
| `vendor/Main_MiSTer/user_io.cpp` | 2965 | CONF_VGA_FB bit in buttons map |
| `vendor/Main_MiSTer/user_io.h` | 58 | `UIO_WAIT_VSYNC = 0x30` |
| `vendor/Main_MiSTer/user_io.h` | 152 | `CONF_VGA_FB = 0b0001000000000000` |
| `vendor/Main_MiSTer/thirdsarm_wrapper.cpp` | 1669-1677 | Video init (set_vga_fb, video_fb_enable) |
| `vendor/Main_MiSTer/thirdsarm_wrapper.cpp` | 1437-1439 | Video shutdown path |
| `vendor/Main_MiSTer/shmem.cpp` | 18-28 | `shmem_map()` via /dev/mem mmap |
| `vendor/Main_MiSTer/fpga_io.cpp` | 26-27 | FPGA_REG_BASE address |
| `vendor/Main_MiSTer/spi.cpp` | 106-117 | `spi_uio_cmd_cont()`, `spi_uio_cmd()` |
| `src/port/sdl/sdl_app.c` | 52 | `target_frame_time_ns` definition |
| `src/port/sdl/sdl_app.c` | ~9700-9870 | `SDLApp_EndFrame()` presentation dispatch (search `fbdev_presenter_enabled`) |
| `src/port/sdl/sdl_app.c` | ~9880-9900 | Frame pacing (search `frame_deadline`) |
| `include/port/sdl/sdl_app.h` | 8 | `TARGET_FPS 59.59949` |
| `src/port/sdl/fbdev_presenter.c` | 1-80 | Current fbdev presenter (to be bypassed) |

## Appendix B: Summary of All New Files

| File | Language | Approx. Lines | Purpose |
|------|----------|---------------|---------|
| `vendor/Menu_MiSTer/rtl/native_video_top.sv` | SystemVerilog | ~80 | Top-level wrapper |
| `vendor/Menu_MiSTer/rtl/native_video_timing.sv` | SystemVerilog | ~60 | H/V counters and sync |
| `vendor/Menu_MiSTer/rtl/native_video_reader.sv` | SystemVerilog | ~200 | DDR3 reader + FIFO control |
| `vendor/Menu_MiSTer/rtl/pll_vid.v` | Verilog (IP) | ~100 | Pixel clock PLL (auto-generated) |
| `vendor/Menu_MiSTer/rtl/pll_vid/pll_vid_0002.v` | Verilog (IP) | ~80 | PLL inner module (auto-generated) |
| `vendor/Menu_MiSTer/rtl/dcfifo_native.v` | Verilog (IP) | ~50 | Dual-clock FIFO (auto-generated) |
| `src/port/sdl/native_video_writer.c` | C | ~80 | ARM DDR3 writer |
| `include/port/sdl/native_video_writer.h` | C | ~10 | Header |

## Appendix C: Summary of All Modified Files

| File | Nature of Change |
|------|------------------|
| `vendor/Menu_MiSTer/menu.sv` | Add PLL, clock mux, native video module, DDR3 mux, output mux |
| `vendor/Menu_MiSTer/files.qip` | Add new source files |
| `vendor/Main_MiSTer/thirdsarm_wrapper.cpp` | Conditional skip of set_vga_fb/video_fb_enable |
| `vendor/Main_MiSTer/video.cpp` | YC mode pixel clock override for native video |
| `src/port/sdl/sdl_app.c` | Native video frame write + vsync pacing |
| `CMakeLists.txt` | Add native_video_writer.c to build |
| `vendor/Menu_MiSTer/sys/sys_top.v` | Optional: add core vsync to vs_wait clear path |
