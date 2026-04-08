# Software PLL Frame Pacer — Implementation Plan

**Date:** 2026-04-08
**Status:** Implemented (commit e65b51a8)
**Approach:** ARM-only software PLL — zero FPGA changes

## Background

The FPGA DDR3 vsync feedback approach (Part 2) produced broken video output on all
3 builds. After exhaustive RTL analysis, the logic appears correct but the Quartus
fitter placement shift from 73 new signal routes is the most likely cause.

The Part 1 PLL retune already achieved a frequency match of 1.4 µHz (59.5995 Hz FPGA
vs 59.59949 Hz CPS3). At this precision, phase drift is 1 frame per 8.3 days. The only
remaining frame pacing issue is OS scheduling jitter on the ARM side.

## Solution: Software PLL with RT Scheduling

Replace the broken FPGA feedback approach with ARM-side mitigations:
1. SCHED_FIFO real-time scheduling to minimize Linux preemption jitter
2. mlockall() to prevent page faults during gameplay
3. Hybrid sleep/busy-wait for sub-millisecond precision
4. Phase tracking to detect and compensate systematic lateness

## Changes — Single File: `src/port/sdl/sdl_app.c`

### Step 1: Add Headers

Under the existing `#if defined(PORT_MISTER)` include block (~line 36):

```c
#include <sched.h>
#include <sys/mman.h>
```

### Step 2: RT Scheduling Setup/Teardown

Add in the PORT_MISTER section (after `sysfs_read()`, before `apply_arm_clock()`):

**`setup_realtime_scheduling()`:**
- `mlockall(MCL_CURRENT | MCL_FUTURE)` — pin pages, prevent page faults
- `sched_setscheduler(getpid(), SCHED_FIFO, {.sched_priority = 49})` — RT priority
  - Priority 49: below kernel irq threads (50+), above all userspace
  - MiSTer runs as root, so this will succeed
- Both are best-effort: log warning on failure, continue

**`teardown_realtime_scheduling()`:**
- Restore `SCHED_OTHER` (normal scheduling)
- `munlockall()`

**Call sites:**
- Setup: end of `SDLApp_FullInit()` (after ARM clock setup), guarded by PORT_MISTER
- Teardown: start of `SDLApp_Quit()` (before ARM clock reset)

### Step 3: Hybrid Sleep/Busy-Wait Helper

Add `precise_delay_ns(Uint64 target_wakeup_ns)`:

```
remaining = target - now
if remaining > BUSYWAIT_THRESHOLD:
    SDL_DelayNS(remaining - BUSYWAIT_THRESHOLD)   // yield CPU
while now < target:
    __asm__ volatile("yield")                      // spin with power hint
```

- Default BUSYWAIT_THRESHOLD: 1.5ms (1,500,000 ns)
- Tunable via env var `THREESX_BUSYWAIT_US` (read once at init)
- 1.5ms busy-wait absorbs worst-case sleep overshoot on the A9's tickless kernel
- `yield` instruction reduces power during spin, hints to second core

### Step 4: Software PLL Frame Pacer

Replace the frame pacing block in `SDLApp_EndFrame()` (~lines 10231-10322).

**State variables** (replace 5 dead closed-loop vars):
```c
static Uint64 frame_deadline = 0;
static Uint64 pacer_frame_time_ns = 0;
static int    pacer_late_streak = 0;
static Uint64 pacer_phase_correction = 0;  // cumulative early-nudge (ns)
```

**Algorithm for native video path:**
```
now = SDL_GetTicksNS()

if frame_deadline == 0:
    pacer_frame_time_ns = target_frame_time_ns
    frame_deadline = now + pacer_frame_time_ns

if now < frame_deadline:
    precise_delay_ns(frame_deadline)
    now = SDL_GetTicksNS()

jitter_ns = now - frame_deadline

// Phase tracking: detect systematic lateness
if jitter_ns > 500000:   // >0.5ms late
    pacer_late_streak++
    if pacer_late_streak >= 3 && pacer_phase_correction < 2000000:
        pacer_phase_correction += 250000    // nudge 0.25ms earlier
else:
    pacer_late_streak = 0

// Advance deadline
frame_deadline += pacer_frame_time_ns - pacer_phase_correction

// Guard: if >1 frame behind, reset
if now > frame_deadline + pacer_frame_time_ns:
    frame_deadline = now + pacer_frame_time_ns - pacer_phase_correction
```

**Key design decisions:**
- No frequency adjustment — PLL error is 1.4 µHz, irrelevant
- Phase correction is one-directional (earlier only), caps at 2ms
- 250µs per step, requires 3 consecutive late frames to trigger
- 0.5ms lateness threshold: below this, jitter is absorbed by double-buffering

### Step 5: Jitter Stats in FPS Debug Overlay

In `update_fps_overlay()` and `publish_fps_overlay_label()`:
- Track: max jitter, avg jitter, late frame %, phase correction
- Display in DEBUG mode overlay: `P:j<max>/<avg>us L<pct>%`
- Reset stats each 250ms window (matching existing FPS measurement cycle)

### Step 6: Dead Code Cleanup

Remove these now-unused variables:
- `last_fpga_vblank`, `last_vblank_observe_ns`, `adjusted_frame_time_ns`
- `vsync_lock_state`, `consecutive_stale`
- The `NativeVideoWriter_ReadFeedback()` call in the pacing block

**Keep:** `NativeVideoWriter_ReadFeedback()` in native_video_writer.c/h — harmless,
useful for future FPGA experiments.

## Files Changed

| File | Change |
|------|--------|
| `src/port/sdl/sdl_app.c` | All changes: RT setup, hybrid sleep, pacer rewrite, overlay stats |

No changes to: native_video_writer.c/h, sdl_app.h, any vendor/ files.

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| SCHED_FIFO priority inversion | Medium | Wrapper kills child on timeout; prctl(PR_SET_PDEATHSIG) already set |
| mlockall memory pressure | Low | Game RSS ~30-50MB, MiSTer has 512MB-1GB; failure is non-fatal |
| Busy-wait power | Low | 1.5ms/16.78ms = 9% one core; yield instruction; tunable via env var |
| Phase correction overshoot | Low | Caps at 2ms; arriving early is harmless (double-buffered) |

## Testing

1. Deploy to MiSTer, verify 60fps on FPS overlay
2. Debug overlay: check max jitter <2ms, late% <5%
3. Stress test: run load on second core, verify phase correction engages
4. Duration: 30+ min on CRT, observe for stutter
5. Fallback: THREESX_BUSYWAIT_US=0 disables busy-wait
