# Vsync Feedback FPGA Bug — Research Findings

**Date:** 2026-04-08
**Status:** Unresolved — pivoted to software PLL approach
**Related:** docs/plan-software-pll-frame-pacer.md, docs/plan-frame-pacing-fix.md

## The Bug

Adding DDR3 write capability to the native_video_reader FSM (for vsync feedback)
produces broken CRT output on ALL 3 builds tested, despite the RTL logic appearing
correct after exhaustive analysis.

**Symptom:** CRT via S-Video shows the MiSTer menu's normal static pattern but with
slightly different colors, a black bar at the top, and a thick horizontal black band
that scrolls upward continuously. Classic vertical sync rolling.

**What changed (2 files):**
- `vendor/Menu_MiSTer/rtl/native_video_reader.sv` — Added ST_WRITE_FEEDBACK and
  ST_WAIT_WR_ACK states, DDR3 write signals, vblank counter
- `vendor/Menu_MiSTer/menu.sv` — Wired nv_ddr_din/be/we through DDR3 mux (were
  hardcoded to 64'd0/8'hFF/1'b0)

**Ruled out:**
- Timing closure (3 builds, different optimizations, same symptom; working bitstream
  has similar violations)
- Byte enable bug (fixed in commit 9cdac806)
- Build environment (Build 8 was clean from wiped db/)
- Logic bug when use_nv=0 (exhaustively verified — path is provably identical)

## RTL Analysis — Logic Appears Correct

### Video Sync Chain Is Independent of DDR3 Reader

The CRT sync path:
```
native_video_timing (clk_vid, ce_pix)
  → menu.sv mux (NATIVE_VID_ACTIVE ? nv_hs/vs : old HSync/VSync)
    → sys_top.v: sync_fix (polarity auto-detect)
      → scanlines module (passthrough)
        → OSD module (passthrough)
          → csync module (generates composite sync from hs+vs)
            → yc_out module (4-stage pipeline delay for chroma alignment)
              → VGA_HS pin (carries composite sync for S-Video)
              → VGA_VS pin (carries subcarrier for S-Video)
```

**None of these modules are connected to or affected by the DDR3 reader's FSM.**
The timing generator runs freely on clk_vid (31.153846 MHz) with ce_pix gating
(div-4 = 7.788 MHz pixel clock). Its output is determined solely by the counters
and timing constants, which are unchanged between working and broken bitstreams.

### S-Video Specific Path

For S-Video output (csync_en=1, yc_en=1):
- VGA_HS = ~yc_cs (composite sync, inverted — CRT locks to this)
- VGA_VS = subcarrier_out (color subcarrier — not used for sync lock)
- RGB pins carry Y/C encoded luma/chroma from yc_out module

The composite sync embeds vsync via the csync module, which XORs modified hsync
with vsync. The csync module measures horizontal line period on every clk_vid edge
(not gated by ce_pix). If hsync edges shift by even 1 clk_vid cycle due to routing
changes, the composite sync timing shifts, potentially disrupting CRT lock.

### sync_fix Module (Key Detail)

```verilog
module sync_fix(input clk, input sync_in, output sync_out);
assign sync_out = sync_in ^ pol;

reg pol;
always @(posedge clk) begin
    reg [31:0] cnt;
    reg s1, s2;
    s1 <= sync_in;
    s2 <= s1;
    cnt <= s2 ? (cnt - 1) : (cnt + 1);
    if (~s2 & s1) begin
        cnt <= 0;
        pol <= cnt[31];
    end
end
endmodule
```

This runs on every clk_vid edge (31.15 MHz, no ce_pix gating). It auto-detects
sync polarity by counting pulse density. If the fitter places this logic differently
(due to 73 new routing paths from the DDR3 write signals), a marginal timing path
could cause 1-clock glitches on the sync output.

### DDR3 Protocol Analysis

The Avalon write protocol is correctly implemented:
- feedback_wr asserted when !ddr_busy (ST_WRITE_FEEDBACK)
- Held during busy (general deassert gated by !ddr_busy)
- Deasserted when accepted (ST_WAIT_WR_ACK, !ddr_busy)
- feedback_wr and ddr_rd can never overlap (FSM serializes)
- ddr_be: combinational 0x0F when writing, 0xFF when reading (correct)
- ddr_addr: updated to FEEDBACK_ADDR for write, scanline addr for read (no overlap)

### FIFO Timing Budget

Vblank duration: 40 lines × 63.55 µs/line = 2.542 ms
Feedback write overhead: ~0.5-1 µs (1 DDR3 beat)
The 8-cycle FIFO clear countdown runs in parallel with the write states.
Total latency to first scanline read is similar between working and broken code.

### PLL Confirmed Unchanged

- pll_video_0002.v: 31.153846 MHz (CLK_VIDEO) — not modified by feedback commits
- pll_0002.v: 100.000000 MHz (clk_sys) — not modified by feedback commits
- Both builds use identical PLL configurations

### CDC Is Sound

- new_frame, new_line, vblank: clk_vid → ddr_clk via 2-FF synchronizers
- frame_ready: ddr_clk → clk_vid via 2-FF synchronizer
- fifo_aclr: asynchronous (designed for async operation in dcfifo)
- All paths have adequate margin at the operating frequencies

## Symptom Analysis — Critical Contradiction

The user sees "the MiSTer menu's normal static pattern." This pattern is generated
by the old video path in menu.sv (comp_v gradient from cos/LFSR). **This path is
only visible when NATIVE_VID_ACTIVE = 0** (i.e., status[9] = 0, before the game
starts).

When NATIVE_VID_ACTIVE = 0:
- VGA_HS/VS come from the old hc/vc counters in menu.sv
- VGA_R/G/B = comp_v (gradient pattern)
- **These signals are generated entirely within menu.sv's old video path, which
  was NOT changed by the feedback commits**

This means the vsync rolling is happening to code that wasn't modified. The old
hc/vc counters, HSync/VSync generation, and comp_v computation are identical
between working and broken bitstreams.

## Root Cause Hypotheses (Ranked by Likelihood)

### H1: Quartus Fitter Placement Shift (Most Likely)

The addition of 73 new signal routes (64-bit ddr_din + 8-bit ddr_be + 1-bit ddr_we
through native_video_top and the menu.sv mux, plus FSM control logic) changes the
fitter's global placement and routing solution. Even though the old video path's HDL
is unchanged, its logic cells may be placed differently, causing setup/hold
violations on the sync path through sync_fix in sys_top.v.

The sync_fix module processes sync signals on every clk_vid edge (31.15 MHz) without
ce_pix gating. A marginal timing path could cause 1-clock glitches on the sync
output, which would make a CRT with marginal lock tolerance lose sync.

Evidence supporting this hypothesis:
- 3 builds with different optimization settings all fail (consistent with a placement
  issue that's not resolved by timing-driven optimization)
- The working bitstream has similar timing violations (the difference is WHERE the
  violations occur, not WHETHER they occur)

### H2: DDR3 Write Corrupts HPS Bridge State

The HPS FPGA-to-SDRAM bridge may not handle writes correctly after a long period of
read-only access. MiSTer's ddram module does writes during boot (SDRAM clearing), but
after use_nv=1, the native video reader would be issuing the first FPGA-side DDR3
write since boot. If the bridge enters an error state, it could affect ALL DDR3
operations.

### H3: Continuous Idle-State Feedback Writes

Before the game starts (first_frame_loaded = 0), every vblank triggers a full cycle:
IDLE → POLL_CTRL → WAIT_CTRL → CHECK_CTRL → WRITE_FEEDBACK → WAIT_WR_ACK → IDLE.
This does a DDR3 write at ~60 Hz even though no game frames exist. The working code
goes directly CHECK_CTRL → IDLE (no DDR3 write). If these writes interfere with the
HPS system during boot, they could cause indirect disruption.

## Diagnostic Build Strategy (If Revisiting FPGA Approach)

Three builds to isolate the root cause via binary search:

| Build | FSM States | DDR3 Write | menu.sv Mux | What It Tests |
|-------|-----------|------------|-------------|---------------|
| A | Present | Disabled (feedback_wr hardcoded 0) | Old (hardcoded 0s) | FSM logic alone → fitter placement shift |
| B | Present | Disabled | New (nv_ddr_* routed) | 73-bit mux routing → fitter placement |
| C | Present | Enabled, moved to end-of-frame | New | Timing between CHECK_CTRL and READ_LINE |

**Build A is the most important.** If the bug appears with just the FSM states added
(no actual DDR3 write, no mux change), it's a pure fitter placement issue. Fix would
be: add `preserve` constraints on timing generator registers, or simplify the
feedback FSM to use fewer resources.

**If Build A is clean, do Build B.** If B shows the bug, the 73-bit mux routing
alone is enough to shift the fitter solution.

**If both A and B are clean, do Build C.** This moves the feedback write to after the
last scanline read (in ST_LINE_DONE when cur_line == V_ACTIVE-1), eliminating any
possible interference with the critical vblank timing window.

## Alternative FPGA Approaches (Not Pursued)

### Option B: Move Feedback Write to End-of-Frame

Write vblank_counter and last_buffer_status AFTER the last scanline is read, not
before. The critical CHECK_CTRL → READ_LINE path becomes identical to the working
code. The feedback data is already latched in CHECK_CTRL registers, so the value is
valid when written later. Only downside: ~2.5ms additional delay, negligible for ARM
phase estimation.

### Option C: Lightweight HPS Bridge Register

Use the FPGA-to-HPS lightweight bridge (0xFF200000+ address space). Add an Avalon
slave in Platform Designer with a 32-bit register for the vblank counter. ARM reads
via a separate /dev/mem mapping. Zero DDR3 involvement. More invasive (requires
QSys changes) but architecturally cleanest.

## Key File Reference

| File | Role |
|------|------|
| vendor/Menu_MiSTer/rtl/native_video_reader.sv | DDR3 reader FSM — feedback write states (broken) |
| vendor/Menu_MiSTer/menu.sv | DDR3 write mux routing + video output mux |
| vendor/Menu_MiSTer/rtl/native_video_timing.sv | Video timing generator (unchanged, generates sync) |
| vendor/Menu_MiSTer/rtl/native_video_top.sv | Wrapper (port connections) |
| vendor/Menu_MiSTer/sys/sys_top.v | MiSTer framework top — sync_fix, csync, yc_out, analog output chain |
| vendor/Menu_MiSTer/sys/yc_out.sv | S-Video encoder (4-stage sync pipeline, colorburst) |
| vendor/Menu_MiSTer/rtl/pll_video/pll_video_0002.v | Video PLL: 31.153846 MHz (unchanged) |
| vendor/Menu_MiSTer/rtl/pll/pll_0002.v | System PLL: 100 MHz (unchanged) |
| src/port/sdl/sdl_app.c | ARM-side frame pacer (working, falls back to open-loop) |
| src/port/sdl/native_video_writer.c | ARM-side DDR3 frame writer + feedback reader |
| docs/plan-frame-pacing-fix.md | Original two-part plan (Part 2 lines 375-896) |
