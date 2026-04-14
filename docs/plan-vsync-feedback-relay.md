# Vsync Feedback Relay — Implementation Plan

**Date:** 2026-04-13
**Status:** Plan
**Approach:** Wrapper relays FPGA frame counter to DDR3 shared memory; game app reads it for closed-loop phase locking
**FPGA changes:** None

## Background

The FPGA has an 8-bit frame counter (`sys_top.v:552`) that increments on every native
video vsync rising edge. The wrapper already reads it via UIO command `0x42` inside
`frame_timer()`. The game app already has DDR3 mapped at `0x3A000000` (via
`NativeVideoWriter_Init()`), and `NativeVideoWriter_ReadFeedback()` exists as dead code
reading from offset `0x40`.

The software PLL in `sdl_app.c` currently runs open-loop: it uses `SCHED_FIFO` + hybrid
sleep/busy-wait + phase correction to maintain frame pacing, but has no feedback from the
actual FPGA vsync. The PLL frequency match is excellent (1.4 uHz error), so the only
issue is *phase* — the ARM frame may drift in phase relative to the CRT vsync, causing
occasional double-buffered stale frames or tearing.

Adding FPGA-side DDR3 writes for vsync feedback caused broken CRT output (documented in
`docs/archive/research-vsync-feedback-fpga-bug.md`). The alternative: have the wrapper
(which already polls the FPGA frame counter) write the counter + timestamp to DDR3, and
the game app reads it. Both processes already have the DDR3 region mapped.

## Memory Layout

The feedback word lives at DDR3 physical address `0x3A000040` (offset `NV_FEEDBACK_OFFSET`
= `0x40` from the native video base). Currently cleared to 0 by `NativeVideoWriter_Init()`.

**New feedback format — two 32-bit words at offset 0x40:**

| Offset | Name | Format |
|--------|------|--------|
| `0x40` | Feedback word | bits[31:8] = ARM `CLOCK_MONOTONIC` timestamp (bottom 24 bits of microseconds), bits[7:0] = FPGA 8-bit frame counter |
| `0x44` | Sequence number | Monotonically increasing 32-bit write counter (for reader to detect torn reads) |

**Design rationale:**

- **24-bit microsecond timestamp**: wraps every ~16.7 seconds, which is far longer than
  any reasonable phase measurement window. Using microseconds (not nanoseconds) keeps the
  value compact. The bottom 24 bits of `clock_gettime(CLOCK_MONOTONIC)` converted to
  microseconds provide sub-us precision.
- **8-bit frame counter**: Passed through verbatim from the FPGA's `frame_cnt`. Wraps at
  256 (~4.3 seconds at 60 Hz). The reader uses modular arithmetic on 8-bit values to
  compute deltas, so wrapping is handled naturally.
- **Sequence number**: The reader does `seq1 = read(0x44); word = read(0x40); seq2 = read(0x44)`.
  If `seq1 == seq2`, the read is consistent. On the Cortex-A9 with uncached/device memory,
  32-bit aligned reads and writes are atomic, but a torn pair (word written, sequence not
  yet written) is possible if the reader interrupts the writer between the two stores.
  The sequence check guards against this.
- **Write ordering note**: The writer writes `fb_word` first, then `fb_seq`. This is the
  opposite of a textbook seqlock (which increments the sequence before *and* after the
  payload write). It works here because: (1) the DDR3 region is mapped with `O_SYNC` +
  `MAP_SHARED`, giving device/strongly-ordered memory semantics on the Cortex-A9, so
  stores are visible in program order without explicit barriers; (2) the reader checks
  `seq1 == seq2` to detect tears — if the reader sees `fb_word` mid-update but catches
  the old `fb_seq`, the next read will see the new `fb_seq` and succeed. A torn read
  (stale word + new seq) cannot happen because the word is written *before* the seq.

**Why not reuse the old format (bits[31:2]=counter, bits[1:0]=status)?**

The old format was designed for the FPGA writer, where the counter was the FPGA's own
vblank count and the status indicated buffer-read state. The wrapper relay has different
needs: the counter is the raw 8-bit FPGA value (no need to shift it), and the timestamp
is the critical new data. A clean format avoids confusion with the old dead-code path.

## Architecture

```
FPGA frame_cnt (8-bit, increments on vsync)
    |
    v
Wrapper: frame_timer() polls UIO 0x42 every ~1ms (usleep(1000) loop)
    |
    v  (detects frame_cnt change)
Wrapper: writes {timestamp[23:0], frame_cnt[7:0]} to DDR3 0x3A000040
         writes sequence++ to DDR3 0x3A000044
    |
    v  (DDR3 shared memory, uncached/device, no cache coherence needed)
Game app: NativeVideoWriter_ReadFeedback() reads 0x3A000040 + 0x3A000044
    |
    v
Game app: sdl_app.c frame pacer uses feedback for closed-loop phase lock
```

**Latency budget:** The wrapper polls at ~1 kHz (usleep(1000)). The FPGA vsync occurs
every ~16.78 ms. In the worst case, the wrapper detects the counter change up to 1 ms
after the actual vsync. This is fine — the phase estimator needs to know *when* the vsync
happened (via the ARM timestamp at detection time), and 0-1 ms jitter in that timestamp
is well within the phase correction range.

---

## Step 1: Wrapper — Write FPGA Frame Counter to DDR3

### Why It Matters

This is the data source. Without the wrapper writing the feedback word, the game app has
nothing to read. This step is independently testable: after implementing it, we can verify
the DDR3 word is updating by reading `/dev/mem` from a shell script on the MiSTer.

### Files to Read Before Implementing

All wrapper source files live in `vendor/Main_MiSTer/`. The build system
(`tools/mister-wrapper/build-hps.sh`) clones upstream Main_MiSTer into
`build/mister-wrapper-hps/src/` and overlays files listed in
`tools/mister-wrapper/main-mister-overlay.files`. Files modified here must either already
be in the overlay manifest or be added to it.

- `vendor/Main_MiSTer/frame_timer.cpp` — understand `frame_timer()`, `global_frame_counter`, `fpga_vsync_timer`
- `vendor/Main_MiSTer/frame_timer.h` — understand `FRAME_TICK` macro, extern declarations
- `vendor/Main_MiSTer/thirdsarm_wrapper.cpp` — understand `wait_for_child()` poll loop, `clear_native_video_ddr3_ctrl()`, `g_native_video_mode`
- `vendor/Main_MiSTer/mister_joy_shm.h` — reference for shared-memory struct pattern

### Files to Modify

**`vendor/Main_MiSTer/frame_timer.h`** and **`vendor/Main_MiSTer/frame_timer.cpp`** — No new
variable needed. The existing `global_frame_counter` (`uint64_t`, declared in `frame_timer.h`)
already holds `frcnt & 0xFF` in the FPGA branch. The feedback writer can use
`(uint8_t)global_frame_counter` to extract the raw 8-bit FPGA counter value.

> **Design note:** `global_frame_counter` is `uint64_t` and is also incremented (not set to
> `frcnt & 0xFF`) in the timerfd fallback path. The feedback writer only runs when
> `fpga_vsync_timer` is true, which means we are in the FPGA branch where
> `global_frame_counter = frcnt & 0xFF`. In that branch, casting to `uint8_t` yields the
> raw FPGA counter. A separate `uint8_t fpga_frame_cnt_raw` variable would be marginally
> clearer but adds a global for no functional benefit. The `fpga_vsync_timer` guard on the
> writer ensures the timerfd path (which increments rather than sets) is never active when
> the feedback writer runs.

These files must also be **added to the overlay manifest**
(`tools/mister-wrapper/main-mister-overlay.files`) if any modifications are made, since
they are currently sourced from upstream rather than the overlay. If no modifications are
needed (as in the current plan), no overlay change is required.

**`vendor/Main_MiSTer/thirdsarm_wrapper.cpp`** — Changes:

1. **Add a persistent DDR3 mapping** for the native video region. Currently
   `clear_native_video_ddr3_ctrl()` does a one-shot mmap/munmap each call. Add:
   - A file-scope `static volatile uint8_t* g_nv_ddr3_base = nullptr;` and
     `static int g_nv_ddr3_fd = -1;`.
   - A function `open_native_video_ddr3()` that opens `/dev/mem`, mmaps 4096 bytes at
     `0x3A000000` with `O_RDWR | O_SYNC` and `MAP_SHARED`, and sets `g_nv_ddr3_base`.
   - A function `close_native_video_ddr3()` that munmaps and closes.
   - Refactor `clear_native_video_ddr3_ctrl()` to use `g_nv_ddr3_base` if available,
     falling back to the current one-shot path.

2. **Add the feedback writer function** `write_vsync_feedback()`:
   ```
   if (!g_nv_ddr3_base || !fpga_vsync_timer) return;

   static uint8_t last_cnt = 0;
   static uint32_t seq = 0;
   uint8_t cnt = (uint8_t)global_frame_counter;
   if (cnt == last_cnt) return;  // no new vsync
   last_cnt = cnt;

   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   uint32_t us = (uint32_t)(ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000) & 0x00FFFFFF;

   volatile uint32_t* fb_word = (volatile uint32_t*)(g_nv_ddr3_base + 0x40);
   volatile uint32_t* fb_seq  = (volatile uint32_t*)(g_nv_ddr3_base + 0x44);

   seq++;
   *fb_word = (us << 8) | cnt;
   *fb_seq  = seq;
   ```

3. **Call `write_vsync_feedback()`** inside `wait_for_child()`, right after `frame_timer()`.
   This call is already inside the `if (is_fpga_ready(1))` guard (line 1815), which ensures
   the FPGA is accessible before any SPI/UIO operations:
   ```cpp
   if (is_fpga_ready(1))
   {
       frame_timer();
       write_vsync_feedback();  // <-- new
       input_poll(0);
       // ... joy mask export ...
   }
   ```

4. **Open the DDR3 mapping** when entering native video mode. In the block after
   `g_native_video_mode` is set to true and before `clear_native_video_ddr3_ctrl()`,
   call `open_native_video_ddr3()`.

5. **Close the DDR3 mapping** in all cleanup paths. Every place that calls
   `clear_native_video_ddr3_ctrl()` followed by `user_io_status_set("[9]", 0)` should
   also call `close_native_video_ddr3()` afterwards.

6. **Handle the restart path** (line ~2185, `g_wrapper_restart_requested`). The restart
   path calls `clear_native_video_ddr3_ctrl()` but does NOT disable native video mode
   (`g_native_video_mode` stays true) — it `continue`s the main loop to re-launch the
   child. In this case, do NOT close the persistent DDR3 mapping. Instead, reset the
   feedback writer's `last_cnt` state so it re-detects the first vsync after restart.
   Add a `reset_vsync_feedback()` function that sets `last_cnt = 0` and `seq = 0`
   (these are file-scope statics, so the reset function needs to be in the same TU).
   Call `reset_vsync_feedback()` in the restart block after `clear_native_video_ddr3_ctrl()`.
   This also clears any stale feedback the new child process might read on startup.

### Success Criteria

1. Wrapper compiles without errors in the Docker cross-compilation build:
   `tools/mister/build-game.sh --flavor telemetry` (which also builds the wrapper).
2. Deploy to MiSTer. With native video active, run:
   ```bash
   # Read the feedback words from DDR3 a few times, 100ms apart
   for i in 1 2 3 4 5; do
     devmem2 0x3A000040 w; devmem2 0x3A000044 w; sleep 0.1
   done
   ```
   The feedback word at 0x40 should show a changing 8-bit counter in the low byte, and
   the sequence number at 0x44 should be monotonically increasing.

### Dependencies

None — this is the first step.

### What NOT to Do

- Do not change any FPGA/RTL files.
- Do not change any game-app-side files (`sdl_app.c`, `native_video_writer.c`).
- Do not add a dedicated thread for the feedback writer. The existing poll loop
  (`wait_for_child`) already runs at ~1 kHz, which is sufficient.
- Do not use `/dev/shm` or any IPC mechanism other than DDR3. Both processes already have
  DDR3 mapped; adding a second IPC channel adds complexity for no benefit.
- Do not add `write_vsync_feedback()` to the `scheduler.cpp` poll loop. That path runs
  `frame_timer()` for the menu core (no game running), where native video feedback is
  irrelevant. The feedback writer only needs to run in `wait_for_child()` while the game
  process is active.

### What to Do If It Fails

- **DDR3 mmap fails**: Check that the wrapper runs as root (it does on MiSTer). Check
  that the physical address `0x3A000000` is correct (it is — same as
  `NativeVideoWriter_Init()`).
- **Feedback word not updating**: Add a `printf` inside `write_vsync_feedback()` to
  confirm `fpga_vsync_timer` is true and `global_frame_counter` is changing. If
  `fpga_vsync_timer` is false, the core may not be reporting its frame counter — check
  that `spi_uio_cmd(UIO_GET_FR_CNT)` returns bit 8 set.
- **Counter not advancing**: The wrapper's `frame_timer()` might not run during
  `wait_for_child()`. Confirm `service_ui` is true (it is when `!forced`, which is the
  normal path).

---

## Step 2: Game App — Read Feedback and Expose to Frame Pacer

### Why It Matters

This step makes the FPGA vsync feedback available to the frame pacer in `sdl_app.c`. It
reactivates the existing `NativeVideoWriter_ReadFeedback()` function and adds parsing for
the new format. The frame pacer changes come in Step 3.

### Files to Read Before Implementing

- `src/port/sdl/native_video_writer.c` — understand `NativeVideoWriter_ReadFeedback()`, DDR3 mapping, feedback offset
- `src/port/sdl/native_video_writer.h` — understand `NV_FeedbackVblankCounter()`, `NV_FeedbackBufferStatus()` (will be replaced)
- `src/port/sdl/sdl_app.c` lines 131-143 — understand current pacer state variables

### Files to Modify

**`src/port/sdl/native_video_writer.h`** — Replace the old inline helpers with new ones:
```c
/// Feedback format: bits[31:8] = ARM timestamp (us, bottom 24 bits),
///                  bits[7:0]  = FPGA 8-bit frame counter.
/// Second word at offset 0x44: sequence number.

/// Extract the FPGA frame counter from a feedback word.
static inline uint8_t NV_FeedbackFrameCounter(uint32_t fb) { return (uint8_t)(fb & 0xFF); }

/// Extract the ARM timestamp (microseconds, bottom 24 bits) from a feedback word.
static inline uint32_t NV_FeedbackTimestampUs(uint32_t fb) { return fb >> 8; }
```

Add a new function declaration:
```c
/// Read the feedback sequence number from DDR3 (offset 0x44).
uint32_t NativeVideoWriter_ReadFeedbackSeq(void);
```

Remove:
- `NV_FeedbackVblankCounter()` (old format)
- `NV_FeedbackBufferStatus()` (old format)

**`src/port/sdl/native_video_writer.c`** — Changes:

1. **Clear offset 0x44 (sequence number) in `NativeVideoWriter_Init()`**. The existing code
   clears offset 0x40 (feedback word) but not 0x44. Without this, a stale sequence number
   from a previous run could cause the reader to accept a torn read on first boot. Add
   after the existing `*feedback = 0;` line:
   ```c
   volatile uint32_t* feedback_seq = (volatile uint32_t*)(ddr_base + NV_FEEDBACK_OFFSET + 4);
   *feedback_seq = 0;
   ```

2. **Add `NativeVideoWriter_ReadFeedbackSeq()`**:
   ```c
   uint32_t NativeVideoWriter_ReadFeedbackSeq(void) {
       if (!ddr_base) return 0;
       volatile uint32_t* seq = (volatile uint32_t*)(ddr_base + NV_FEEDBACK_OFFSET + 4);
       return *seq;
   }
   ```

Also add a non-PORT_MISTER stub:
```c
uint32_t NativeVideoWriter_ReadFeedbackSeq(void) { return 0; }
```

**`src/port/sdl/sdl_app.c`** — Add new state variables alongside the existing pacer vars:
```c
static uint8_t  last_fpga_frame_cnt = 0;
static uint32_t last_vsync_timestamp_us = 0;  // from feedback word
static Uint64   last_vsync_monotonic_ns = 0;  // SDL_GetTicksNS at time of observation
static uint32_t last_feedback_seq = 0;
static bool     vsync_feedback_valid = false;
```

Add a function `poll_vsync_feedback()` (in the PORT_MISTER section):
```c
static void poll_vsync_feedback(void) {
    if (!native_video_writer_enabled) return;

    uint32_t seq1 = NativeVideoWriter_ReadFeedbackSeq();
    uint32_t word = NativeVideoWriter_ReadFeedback();
    uint32_t seq2 = NativeVideoWriter_ReadFeedbackSeq();

    if (seq1 != seq2 || seq1 == 0) return;  // torn read or no data yet
    if (seq1 == last_feedback_seq) return;   // no new data

    last_feedback_seq = seq1;

    uint8_t cnt = NV_FeedbackFrameCounter(word);
    uint32_t ts_us = NV_FeedbackTimestampUs(word);

    if (cnt == last_fpga_frame_cnt) return;  // same counter, skip

    // Compute the ARM monotonic time when this vsync was observed by the wrapper.
    // The wrapper wrote a 24-bit microsecond timestamp. We need to reconstruct
    // the full monotonic time. Since we're reading within ~16ms of the write,
    // the 24-bit window (~16.7s) is more than adequate.
    Uint64 now_ns = SDL_GetTicksNS();
    uint32_t now_us = (uint32_t)(now_ns / 1000) & 0x00FFFFFF;
    int32_t delta_us = (int32_t)((now_us - ts_us) & 0x00FFFFFF);
    if (delta_us > 0x00800000) delta_us -= 0x01000000;  // handle wrap

    Uint64 vsync_time_ns;
    if (delta_us >= 0 && delta_us < 5000000) {  // sane: 0 to 5 seconds ago
        vsync_time_ns = now_ns - (Uint64)delta_us * 1000;
    } else {
        return;  // stale or bogus, skip
    }

    last_fpga_frame_cnt = cnt;
    last_vsync_timestamp_us = ts_us;
    last_vsync_monotonic_ns = vsync_time_ns;
    vsync_feedback_valid = true;
}
```

### Success Criteria

1. Game app compiles without errors: `tools/mister/build-game.sh --flavor telemetry`.
2. After deploying and running the game in native video mode, add a temporary
   `backend_logf()` inside `poll_vsync_feedback()` to log when `vsync_feedback_valid`
   becomes true and what `last_vsync_monotonic_ns` looks like. Verify it logs valid
   timestamps advancing at ~60 Hz cadence.

### Dependencies

Step 1 (wrapper must be writing the feedback word).

### What NOT to Do

- Do not change the frame pacer logic yet (that's Step 3).
- Do not remove the old `NativeVideoWriter_ReadFeedback()` function signature (it stays).
- Do not add any FPGA changes.

### What to Do If It Fails

- **Torn reads every time**: The writer and reader are on different CPU cores (wrapper
  pinned to CPU 0, game uses CPU 1). On uncached memory, 32-bit aligned writes are
  atomic, but the two-word write (feedback + seq) is not atomic. If torn reads are
  frequent, increase the poll tolerance: retry once in `poll_vsync_feedback()` instead of
  returning immediately.
- **`delta_us` always out of range**: The 24-bit microsecond timestamp from the wrapper
  may be measured on a different clock. Verify both sides use `CLOCK_MONOTONIC`. SDL3's
  `SDL_GetTicksNS()` on Linux uses `CLOCK_MONOTONIC`, and the wrapper uses
  `clock_gettime(CLOCK_MONOTONIC)`, so they should agree.
- **Feedback word always 0**: The wrapper may not have the DDR3 mapping open, or
  `fpga_vsync_timer` may be false. Check Step 1 diagnostics.

---

## Step 3: Closed-Loop Phase Locking in the Frame Pacer

### Why It Matters

This is the payoff. The open-loop pacer has no knowledge of where the CRT beam actually
is. With vsync feedback, the pacer can align ARM frame delivery to arrive just before the
FPGA's vsync, minimizing display latency and eliminating phase drift.

### Files to Read Before Implementing

- `src/port/sdl/sdl_app.c` lines 10542-10616 — current frame pacer (open-loop software PLL)
- `src/port/sdl/sdl_app.c` lines 9461-9507 — `setup_realtime_scheduling()`, `precise_delay_ns()`
- `src/port/sdl/sdl_app.c` lines 131-143 — pacer state variables
- `src/port/sdl/sdl_app.c` lines 9240-9260 — pacer stats snapshot for overlay
- `include/port/sdl/sdl_app.h` — `TARGET_FPS`, `NV_TARGET_FPS`
- `docs/archive/plan-software-pll-frame-pacer.md` — understand the open-loop design rationale

### Files to Modify

**`src/port/sdl/sdl_app.c`** — Modify the native video frame pacing block (lines 10551-10603):

The new algorithm:

```
// Called every frame in SDLApp_EndFrame(), after rendering is complete.

poll_vsync_feedback();   // <-- new, updates last_vsync_monotonic_ns

now = SDL_GetTicksNS()

if (frame_deadline == 0):
    frame_deadline = now + target_frame_time_ns

// --- Closed-loop phase correction (when feedback is available) ---
if vsync_feedback_valid:
    // The wrapper observed a vsync at last_vsync_monotonic_ns.
    // The FPGA will display the next frame at:
    //   next_vsync = last_vsync_monotonic_ns + target_frame_time_ns
    // We want to finish rendering and write our frame to DDR3 *before* next_vsync,
    // ideally arriving ~2ms early so the FPGA reads our fresh buffer.
    //
    // Compute where the next vsync is, accounting for possibly multiple vsyncs
    // having passed since last_vsync_monotonic_ns.

    elapsed_since_vsync = now - last_vsync_monotonic_ns
    frames_since = elapsed_since_vsync / target_frame_time_ns
    next_vsync = last_vsync_monotonic_ns + (frames_since + 1) * target_frame_time_ns

    // Our ideal deadline: LEAD_TIME_NS before the next vsync.
    // LEAD_TIME_NS = 2,000,000 (2ms) — enough time for the DDR3 write + FPGA read.
    ideal_deadline = next_vsync - LEAD_TIME_NS

    // If ideal_deadline is in the past (we're late), target the one after.
    if ideal_deadline <= now:
        ideal_deadline += target_frame_time_ns

    // Blend toward ideal: don't jump instantly (causes visible stutter).
    // Move frame_deadline 25% toward ideal each frame.
    error = (int64_t)(ideal_deadline - frame_deadline)
    if abs(error) > target_frame_time_ns:
        frame_deadline = ideal_deadline    // too far off, snap
    else:
        frame_deadline += error / 4        // smooth convergence (~4 frames)

// Sleep until deadline
if (now < frame_deadline):
    precise_delay_ns(frame_deadline)
    now = SDL_GetTicksNS()

// ... existing jitter stats tracking (unchanged) ...

// Advance deadline for next frame
frame_deadline += target_frame_time_ns

// Guard: if >1 frame behind, reset
if (now > frame_deadline + target_frame_time_ns):
    frame_deadline = now + target_frame_time_ns
```

**Key design decisions:**

- **`LEAD_TIME_NS = 2,000,000` (2ms)**: The FPGA's native video reader reads one scanline
  per horizontal line period (~63.5 us). A full 224-line frame takes ~14.2 ms to read. The
  reader starts reading at the first active line after vblank. If our DDR3 write completes
  before vblank starts (~2.5 ms before the next vsync), the FPGA will read our fresh frame
  on the next scanout. 2 ms lead time gives 0.5 ms margin.

- **25% blend (error/4)**: Provides smooth convergence over ~4 frames without visible
  stutter. A sudden 5 ms deadline shift would cause one noticeably short or long frame.
  Blending spreads it out. After 8 frames, the error is reduced by (3/4)^8 = 10%.
  Full convergence takes ~16 frames (~267 ms).

- **Snap threshold**: If the error exceeds one full frame time, the phase is so far off
  that blending would take too long. Snap immediately. This handles startup, pause/resume,
  and dropped frames.

- **Graceful fallback**: When `vsync_feedback_valid` is false (wrapper not writing
  feedback, non-native path, etc.), the existing open-loop pacer behavior is preserved
  exactly. The closed-loop code only activates when feedback data is available.

**Remove the old phase correction heuristic** (pacer_late_streak / pacer_ontime_streak /
pacer_phase_correction). The closed-loop phase lock replaces this entirely. Keep the
jitter stats tracking (pacer_max_jitter_ns, etc.) since it's useful for the overlay.

**Update the FPS overlay** to show closed-loop status:
- When `vsync_feedback_valid`: show `CL:` prefix instead of `P:` prefix, and show the
  phase error in microseconds instead of the old phase correction.
- Format: `CL:e<error>us j<max>/<avg>us L<pct>%`
- When `vsync_feedback_valid` is false: show the existing open-loop format.

### Success Criteria

1. Game app compiles without errors.
2. On MiSTer with native video, the FPS overlay shows `CL:` prefix, indicating
   closed-loop mode is active.
3. The phase error converges to near zero (within a few hundred microseconds) after
   startup and stays there during gameplay.
4. CRT output is smooth — no visible stutter, judder, or sync loss.
5. After 5+ minutes of gameplay, phase error remains stable (no cumulative drift).
6. The `THIRDSARM_BUSYWAIT_US` env var still works for tuning the busy-wait threshold.

### Dependencies

Step 1 (wrapper writing feedback) and Step 2 (game app reading feedback).

### What NOT to Do

- Do not change the FPGA or RTL.
- Do not change the non-native-video pacing path (the `else` branch at line 10604).
- Do not remove `precise_delay_ns()` or `setup_realtime_scheduling()` — they're still
  needed for the closed-loop pacer.
- Do not attempt to adjust the ARM PLL frequency. The frequency match is already excellent;
  only the phase needs correction.
- Do not add a timestamp to the game-app-side DDR3 write (the control word at offset 0x00).
  That would require the FPGA to interpret a new format, violating the zero-FPGA-changes
  constraint.

### What to Do If It Fails

- **CRT stutter during phase convergence**: The 25% blend rate may be too aggressive.
  Reduce to `error / 8` (12.5% per frame, convergence in ~32 frames). If still bad, try
  `error / 16`.
- **Phase oscillation** (error bounces back and forth): The wrapper poll jitter (~0-1 ms)
  introduces noise in `last_vsync_monotonic_ns`. Add a low-pass filter: maintain a running
  average of the last 4 observed vsync times (modulo frame period) and use that for the
  phase estimate.
- **Phase converges but to wrong value**: The 2 ms `LEAD_TIME_NS` may not be correct.
  Make it configurable via env var `THIRDSARM_LEAD_TIME_US` (default 2000). Test with
  values from 500 to 5000 to find the optimal lead.
- **Feedback stops arriving** (wrapper crashes, exits): `vsync_feedback_valid` stays true
  but `last_vsync_monotonic_ns` goes stale. Add a staleness check: if the feedback
  timestamp is >100 ms old, set `vsync_feedback_valid = false` and fall back to open-loop.
  This should be added to `poll_vsync_feedback()`.
- **Complete failure — CRT rolling or blank**: Revert to open-loop by setting
  `THIRDSARM_VSYNC_FEEDBACK=0` (env var kill switch, checked at init). This bypasses
  `poll_vsync_feedback()` entirely and preserves the working open-loop behavior.

---

## Step 4: Diagnostics, Kill Switch, and Overlay Polish

### Why It Matters

Production resilience. The closed-loop pacer must degrade gracefully if something goes
wrong, and the user/developer needs visibility into its behavior.

### Files to Read Before Implementing

- `src/port/sdl/sdl_app.c` — the full pacer section (post-Step-3)
- `src/port/sdl/sdl_app.c` lines 9150-9165 — FPS overlay format string

### Files to Modify

**`src/port/sdl/sdl_app.c`** — Add:

1. **Environment variable kill switch**: At init (in `SDLApp_FullInit` or the busywait
   threshold read block), read `THIRDSARM_VSYNC_FEEDBACK`. If set to `"0"`, set a flag
   `vsync_feedback_disabled = true` that prevents `poll_vsync_feedback()` from running.
   Default: enabled.

2. **Lead time tuning**: Read `THIRDSARM_LEAD_TIME_US` at init. Default 2000. Store as
   `static Uint64 lead_time_ns = 2000000;`.

3. **Staleness detection**: In `poll_vsync_feedback()`, after updating
   `last_vsync_monotonic_ns`, record the current time. In the pacer block, before using
   `vsync_feedback_valid`, check that the last feedback update was within 100 ms. If
   stale, set `vsync_feedback_valid = false`.

4. **Overlay polish**: Update `update_fps_overlay()` and `publish_fps_overlay_label()`:
   - Closed-loop active: `CL:e<phase_error_us>us j<max>/<avg>us L<pct>%`
   - Open-loop fallback: `OL:j<max>/<avg>us L<pct>% ph<phase_us>`
   - This makes it immediately obvious which mode is active.

5. **Startup log**: When closed-loop first engages (vsync_feedback_valid transitions
   false -> true), log: `"Frame pacer: closed-loop vsync feedback engaged (lead_time=%d us)"`.
   When it disengages (staleness), log: `"Frame pacer: vsync feedback stale, falling back to open-loop"`.

### Success Criteria

1. `THIRDSARM_VSYNC_FEEDBACK=0` disables closed-loop: overlay shows `OL:` and behavior
   matches the pre-change open-loop pacer.
2. `THIRDSARM_LEAD_TIME_US=3000` changes the lead time to 3 ms.
3. If the wrapper is killed while the game runs (simulated via `kill -9 <wrapper_pid>`),
   the game detects stale feedback within ~100 ms, falls back to open-loop, and continues
   running without stutter.
4. The log shows transition messages for engage/disengage.

### Dependencies

Steps 1, 2, and 3.

### What NOT to Do

- Do not add new OSD menu items for these settings. They're developer-facing env vars.
- Do not change the wrapper.
- Do not change any FPGA files.

### What to Do If It Fails

- **Kill switch doesn't work**: Verify the env var is passed through from the wrapper's
  `execve()` to the game process. The wrapper sets `environ` before `execve()`, so
  `THIRDSARM_VSYNC_FEEDBACK=0` must be in the game's environment. Add it via
  `setenv("THIRDSARM_VSYNC_FEEDBACK", "0", 0)` in the wrapper or in the game's own
  init.
- **Staleness detection triggers too early**: The 100 ms threshold assumes the wrapper
  polls at ~1 kHz and the game polls feedback every frame (~16.7 ms). If the wrapper is
  busy with OSD/menu work and pauses polling for >100 ms, false staleness triggers. Raise
  the threshold to 200 ms or add a counter (require N consecutive stale reads).

---

## Implementation Order and Risk Summary

| Step | Risk | Reversibility | Estimated Size |
|------|------|---------------|----------------|
| 1: Wrapper DDR3 write | Low — mirrors existing joy SHM pattern | Full (remove write call) | ~60 lines changed |
| 2: Game feedback read | Low — read-only, no behavior change | Full (remove poll call) | ~80 lines changed |
| 3: Closed-loop pacer | Medium — changes frame timing | Full (env kill switch) | ~60 lines changed |
| 4: Diagnostics/polish | Very low — cosmetic + safety | N/A | ~40 lines changed |

Total: ~240 lines of C/C++ across 4 files, zero FPGA changes, fully reversible via env var.

## 8-Bit Counter Wrapping Analysis

The FPGA `frame_cnt` is 8 bits, wrapping every 256 frames (~4.27 seconds at 59.6 Hz).
The plan handles this correctly at each layer:

1. **Wrapper**: Compares `cnt == last_cnt` using uint8_t, which handles wrapping naturally.
   Writes the raw 8-bit value to DDR3.

2. **Game app**: Compares `cnt == last_fpga_frame_cnt` using uint8_t. Only cares about
   "did the counter change" (yes/no), not the magnitude of change. The timestamp carries
   the actual timing information, not the counter value.

3. **The counter value is not used for frame counting** in the game app. The game app
   counts its own frames. The feedback counter is solely a change-detection signal that
   triggers a timestamp observation.

## Wrapper Poll Rate Considerations

The wrapper's `wait_for_child()` loop runs `usleep(1000)` (1 ms) per iteration, but the
actual poll rate varies due to:
- `frame_timer()` SPI transaction latency (~100-200 us)
- `input_poll()` processing (~100-500 us depending on connected devices)
- `HandleUI()` / `OsdUpdate()` (variable, can spike during OSD interaction)

Realistic poll rate: 500-900 Hz (1.1-2 ms per iteration). This means:
- A vsync at any point in the 16.78 ms frame will be detected within 1-2 ms.
- The timestamp accuracy is +0 to +2 ms from the actual vsync edge.
- The phase estimator in Step 3 accounts for this by using the timestamp (not "now") and
  blending over multiple frames to average out the jitter.

Worst case: During heavy OSD work, a single iteration could take 5-10 ms, causing one
feedback sample to be delayed. The staleness check (Step 4) handles the extreme case.
For normal jitter, the 25% blend rate smooths it out.
