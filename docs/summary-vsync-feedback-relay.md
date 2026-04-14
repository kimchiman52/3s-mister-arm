# Vsync Feedback Relay — Implementation Summary

**Date:** 2026-04-13
**Status:** Deployed, validating

## The Problem

The ARM game app writes frames to DDR3 double buffers. The FPGA reads them at
vblank. The software PLL matches the ARM's frame rate to the FPGA's within
1.4 µHz (1 frame drift per 8.3 days), so the *frequency* is essentially perfect.

But the ARM has no knowledge of the FPGA's *phase* — the exact moment within the
~16.78 ms frame period when vblank occurs. The ARM picks a deadline based on its
own clock and hopes it lands in the right spot. If the ARM's write phase drifts
near the boundary of when the FPGA polls the control word at vblank:

- Some frames: ARM writes just before FPGA polls → consumed immediately (smooth)
- Some frames: ARM writes just after FPGA polls → FPGA re-reads stale buffer,
  displays the new frame one vblank late (skip feel)

This creates intermittent "skippy" matches — stages that feel frame-skippy even
though the debug overlay shows zero dropped frames and zero phase correction.
The ARM thinks everything is fine because it's measuring against its own deadline,
not the FPGA's actual vblank. The symptom is infrequent because the phase drifts
slowly and only causes problems when it's near the critical boundary.

## The Previous Attempt (FPGA DDR3 Write — Failed)

### What We Tried

Added DDR3 write capability directly to the FPGA's `native_video_reader.sv` FSM.
Two new states (`ST_WRITE_FEEDBACK`, `ST_WAIT_WR_ACK`) wrote a 32-bit feedback
word (vblank counter + buffer status) to DDR3 at physical address `0x3A000040`
every vblank. The ARM would read it and phase-lock.

### What Changed in the FPGA

- `native_video_reader.sv`: Added 2 FSM states, DDR3 write signals (`feedback_din`,
  `feedback_be`, `feedback_wr`), vblank counter, buffer status tracking
- `menu.sv`: Changed the DDR3 mux from hardcoded zeros to routing the new write
  signals (`nv_ddr_din`, `nv_ddr_be`, `nv_ddr_we`) — 73 new signal routes through
  the mux (64-bit data + 8-bit byte enables + 1-bit write enable + FSM control)

### Why It Failed

All 3 test builds (different Quartus optimization settings) produced broken CRT
output: vertical sync rolling, black bar at top, wrong colors. The RTL logic was
exhaustively verified as correct. The video sync chain (timing generator → sync_fix
→ csync → yc_out → CRT) is architecturally independent of the DDR3 reader, but
the 73 new signal routes changed the Quartus fitter's global placement solution.

The `sync_fix` module in `sys_top.v` auto-detects sync polarity by counting pulse
density on every `clk_vid` edge (31.15 MHz, no `ce_pix` gating). It's sensitive to
routing changes because marginal timing paths can cause 1-clock glitches on the
sync output. Even though the sync HDL was unchanged, its logic cells got placed
differently, causing the CRT to lose sync.

The root cause was never definitively confirmed — the planned diagnostic builds
(A: FSM only, B: FSM + mux, C: write at end-of-frame) were never executed before
we pivoted to a software PLL approach.

### Why We Chose That Path

At the time, direct FPGA feedback seemed like the obvious architecture: the FPGA
knows exactly when vblank happens, and DDR3 is the shared memory between FPGA and
ARM. Writing a 32-bit word at vblank is a trivial operation. The ARM-side reader
(`NativeVideoWriter_ReadFeedback()`) was straightforward. The problem was entirely
in the Quartus fitter's reaction to the new routing — something that's very hard
to predict or control without iterative builds.

## The New Approach (Wrapper Relay — Working)

### Key Insight

The FPGA already exposes a frame counter to the ARM through an existing mechanism
that requires zero FPGA changes:

1. **`frame_cnt`** (`sys_top.v:552`): An 8-bit counter that increments on every
   rising edge of `vs_fix` (the sync-fixed vsync output). When native video is
   active, `vs_fix` comes from `nv_vs` (the native video timing generator's
   vsync), so this counter already tracks our native video vblanks.

2. **UIO command 0x42** (`sys_top.v:399`): Returns `{1'b1, frame_cnt}` — a flag
   bit (indicating the counter is valid) plus the 8-bit counter. This is a
   standard MiSTer framework feature. Already implemented, already wired.

3. **The MiSTer wrapper** (`frame_timer.cpp:122-128`): Already reads this counter
   via `spi_uio_cmd(UIO_GET_FR_CNT)` every ~1 ms in its poll loop. It was already
   using it for its own frame timing. The data was flowing but nobody was relaying
   it to the game app.

The architecture has three processes:
- **FPGA**: Counts vblanks in hardware (always has)
- **Wrapper** (`MiSTer_3S-ARM`): Reads the counter via SPI/UIO protocol
- **Game app** (`3s-arm`): Runs the game, writes frames to DDR3

The wrapper and game app are separate processes. They already share DDR3 (both
map `0x3A000000` via `/dev/mem`) and joystick state (via `/dev/shm`). The wrapper
has SPI access to the FPGA; the game app does not (SPI is not safe to share
between two processes without locking).

### What We Built

**Wrapper side** (`thirdsarm_wrapper.cpp`):
- Persistent DDR3 mapping at `0x3A000000` (was one-shot open/close per call)
- After each `frame_timer()` poll that detects a new counter value, writes a
  feedback word to DDR3 at offset `0x40`:
  - bits[31:8] = 24-bit `CLOCK_MONOTONIC` timestamp in microseconds
  - bits[7:0] = raw 8-bit FPGA frame counter
- Also writes a sequence number to offset `0x44` for torn-read detection
- Lifecycle management: opens mapping when entering native video mode, closes
  on all exit paths, resets (without closing) on game restart

**Game app side** (`native_video_writer.h/.c`, `sdl_app.c`):
- Replaced old feedback format helpers with new ones matching the wrapper's format
- Added `NativeVideoWriter_ReadFeedbackSeq()` for the sequence number
- Clears offset `0x44` in `NativeVideoWriter_Init()` to prevent stale reads
- `poll_vsync_feedback()`: reads the two DDR3 words with torn-read detection,
  reconstructs the wrapper's monotonic timestamp relative to SDL time using
  `clock_gettime(CLOCK_MONOTONIC)` for the delta calculation, exposes
  `last_vsync_monotonic_ns` and `vsync_feedback_valid` to the frame pacer

**Closed-loop phase lock** (`sdl_app.c`):
- Replaces the old open-loop heuristic (late_streak / ontime_streak /
  phase_correction) which could only nudge deadlines later
- Computes `ideal_deadline` = predicted next vsync minus `LEAD_TIME_NS` (2 ms)
- Blends `frame_deadline` toward `ideal_deadline` at 25% per frame for smooth
  convergence without visible stutter
- Snaps immediately if error exceeds one full frame period (startup, pause, etc.)
- Falls back to open-loop when feedback is unavailable or stale (>100 ms)
- Jitter measured against pre-blend deadline so phase corrections don't register
  as false frame drops

**Diagnostics** (`sdl_app.c`):
- `THIRDSARM_VSYNC_FEEDBACK=0` environment variable disables closed-loop entirely
- `THIRDSARM_LEAD_TIME_US=<value>` tunes the lead time (default 2000)
- Overlay shows `CL:` (closed-loop) or `OL:` (open-loop) prefix with phase error
- Log messages on engage/disengage transitions
- 100 ms staleness detection for graceful fallback

### Why This Path Is Better

1. **Zero FPGA changes.** No Quartus rebuild. No risk of fitter placement shift.
   The entire feature is ARM-only C/C++ code.

2. **The data was already there.** The FPGA was already counting vblanks. The
   wrapper was already reading the counter. We just connected the last mile:
   wrapper → DDR3 → game app.

3. **No new DDR3 write signals in the FPGA.** The previous attempt failed because
   73 new signal routes through the DDR3 mux shifted the fitter's placement of
   unrelated sync logic. This approach has zero new FPGA signals.

4. **Uses proven infrastructure.** Both processes already had DDR3 mapped.
   `NativeVideoWriter_ReadFeedback()` already existed as dead code. The UIO
   command was already implemented and being called. We activated existing
   plumbing.

5. **Fully reversible.** The `THIRDSARM_VSYNC_FEEDBACK=0` kill switch disables
   the entire feature and falls back to the previous open-loop behavior. No
   FPGA state to revert.

### Latency Budget

The wrapper polls at ~1 kHz (`usleep(1000)` loop). A vsync at any point in the
16.78 ms frame will be detected within 1-2 ms. The timestamp records when the
wrapper observed the counter change (not the exact vsync edge), introducing
0-2 ms of jitter. The 25% blend rate averages this out over ~4 frames. The
2 ms lead time provides margin for the DDR3 write to complete before the FPGA
starts reading at the next vblank.

## Bug Fix During Deployment

The initial deployment didn't engage closed-loop feedback. Root cause:
`SDL_GetTicksNS()` returns nanoseconds since SDL initialization, not since boot.
The wrapper writes `clock_gettime(CLOCK_MONOTONIC)` timestamps (nanoseconds since
boot). These are completely different time bases, so the delta calculation always
produced out-of-range values and every feedback sample was silently rejected.

Fix: use `clock_gettime(CLOCK_MONOTONIC)` in the game app for the timestamp
comparison, then convert the result to SDL time for the frame pacer. Also required
setting `_POSIX_C_SOURCE=200809L` in CMakeLists.txt (was undefined, which made
`CLOCK_MONOTONIC` unavailable to the C compiler).

## Files Changed

| File | What Changed |
|------|-------------|
| `vendor/Main_MiSTer/thirdsarm_wrapper.cpp` | Persistent DDR3 mapping, feedback writer, reset, lifecycle |
| `src/port/sdl/native_video_writer.h` | New feedback format helpers (replaced old FPGA-oriented ones) |
| `src/port/sdl/native_video_writer.c` | `ReadFeedbackSeq()`, clear offset 0x44 in Init() |
| `src/port/sdl/sdl_app.c` | `poll_vsync_feedback()`, closed-loop phase lock, diagnostics, jitter fix |
| `CMakeLists.txt` | `_POSIX_C_SOURCE=200809L` for `clock_gettime` |

## What's Still Needed From the Software PLL

Everything except the old phase correction heuristic:

| Component | Status | Why It's Still Needed |
|-----------|--------|----------------------|
| PLL retune (59.5995 Hz) | Unchanged | Frequency matching — keeps clocks within 1.4 µHz |
| `SCHED_FIFO` + `mlockall` | Unchanged | Sub-ms wake precision for the sleep/busy-wait loop |
| `precise_delay_ns` (hybrid sleep/busy-wait) | Unchanged | Accurate deadline targeting |
| `target_frame_time_ns` | Unchanged | Frame period target derived from NV_TARGET_FPS |
| Audio thread priority boost | Unchanged | Prevents priority inversion on soundLock |
| Late streak / phase correction heuristic | **Removed** | Replaced by closed-loop phase lock |
