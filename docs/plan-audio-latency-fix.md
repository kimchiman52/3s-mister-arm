# Audio Latency Fix: Implementation Plan

Three-step fix for audio delay that grows over time. Each step is one
`/implement` invocation: implement, review, fix, verify, commit.

---

## Step 1: Tighten FPGA hurryup thresholds in alsa.sv

**Goal:** Reduce the buffer fill level at which the FPGA starts draining
faster, cutting the worst-case sawtooth latency from ~85ms down to ~43ms.

### Files to modify

- `/Users/sb/Developer/3sx-mister-audio-fix/vendor/Menu_MiSTer/sys/alsa.sv`

### Exact changes

Lines 95-103 of `alsa.sv`, inside the `always @(posedge clk)` block
(the block starting at line 78). Replace the six hurryup threshold lines:

**Before (lines 95-103):**
```verilog
		//ramp up
		if(len[18:14] && (hurryup < 1)) hurryup <= 1;
		if(len[18:16] && (hurryup < 2)) hurryup <= 2;
		if(len[18:17] && (hurryup < 4)) hurryup <= 4;

		//ramp down
		if(!len[18:15] && (hurryup > 2)) hurryup <= 2;
		if(!len[18:13] && (hurryup > 1)) hurryup <= 1;
		if(!len[18:10]) hurryup <= 0;
```

**After:**
```verilog
		//ramp up
		if(len[18:13] && (hurryup < 1)) hurryup <= 1;
		if(len[18:14] && (hurryup < 2)) hurryup <= 2;
		if(len[18:15] && (hurryup < 4)) hurryup <= 4;

		//ramp down
		if(!len[18:14] && (hurryup > 2)) hurryup <= 2;
		if(!len[18:12] && (hurryup > 1)) hurryup <= 1;
		if(!len[18:10]) hurryup <= 0;
```

Threshold summary (all values in 64-bit words / stereo sample pairs):

| Transition | Old bit-range | New bit-range | Old threshold | New threshold |
|---|---|---|---|---|
| 0->1 ramp-up | `len[18:14]` >= 2048 | `len[18:13]` >= 1024 | 85.3ms | 42.7ms |
| 1->2 ramp-up | `len[18:16]` >= 8192 | `len[18:14]` >= 2048 | 341.3ms | 85.3ms |
| 2->4 ramp-up | `len[18:17]` >= 16384 | `len[18:15]` >= 4096 | 682.7ms | 170.7ms |
| 4->2 ramp-down | `!len[18:15]` < 4096 | `!len[18:14]` < 2048 | 170.7ms | 85.3ms |
| 2->1 ramp-down | `!len[18:13]` < 1024 | `!len[18:12]` < 512 | 42.7ms | 21.3ms |
| 1->0 ramp-down | `!len[18:10]` < 128 | `!len[18:10]` < 128 | 5.3ms | 5.3ms (unchanged) |

### What NOT to change

- Do NOT touch the `ce_sample` DDS accumulator (lines 145-155). The
  `48000 + {hurryup,6'd0}` formula and CLK_RATE parameter stay as-is.
- Do NOT change the `hurryup` register width (3 bits), the SPI interface,
  `buf_rptr`/`buf_wptr` logic, or the DMA read state machine.
- Do NOT change any other file in `vendor/Menu_MiSTer/sys/`.

### How to verify

1. **Syntax check:** Confirm the bit-slice ranges are valid: `len` is
   `[18:3]` (16 bits), so `len[18:12]` is 7 bits, `len[18:13]` is 6 bits,
   `len[18:14]` is 5 bits, `len[18:15]` is 4 bits. All valid sub-ranges.
2. **Hysteresis stability:** For each hurryup level, the ramp-up threshold
   must be strictly greater than the ramp-down threshold:
   - Level 1: up at 1024, down at <128 (band = 896 words = 37ms). Safe.
   - Level 2: up at 2048, down at <512 (band = 1536 words = 64ms). Safe.
   - Level 4: up at 4096, down at <2048 (band = 2048 words = 85ms). Safe.
3. **No Quartus build required for review** -- the logic is purely
   combinational threshold comparisons. A full FPGA build is needed for
   deployment but not for correctness review.

### Dependencies

None. This is the first step.

---

## Step 2: Reduce ADX minimum buffer from 400ms to 100ms

**Goal:** Cut 300ms of baseline music latency from the ADX push buffer.

### Files to modify

- `/Users/sb/Developer/3sx-mister-audio-fix/src/port/sound/adx.c`

### Exact changes

Line 28 of `adx.c`. Change the `MIN_QUEUED_DATA_MS` define:

**Before (line 28):**
```c
#define MIN_QUEUED_DATA_MS 400
```

**After:**
```c
#define MIN_QUEUED_DATA_MS 100
```

This single constant controls all buffer target calculations through
`min_queued_data_bytes()` (line 99), which feeds `stream_data_needed()`
(line 103) and `stream_needs_data()` (line 111). Those functions gate
`process_track()` (line 493) and `ADX_ProcessTracks()` (line 654).

The value 100ms provides ~6 frames of runway at 59.6 Hz, sufficient to
cover:
- One late frame: 33.6ms (2x normal frame time)
- Track transition gap: 16.8ms (one frame)
- OS scheduling jitter: 15ms
- Total needed: ~65ms, leaving 35ms of safety margin

### What NOT to change

- Do NOT change `DEFAULT_SAMPLE_RATE`, `N_CHANNELS`, `BYTES_PER_SAMPLE`,
  or any other `#define` in `adx.c`.
- Do NOT change `min_queued_data_bytes()`, `stream_data_needed()`, or
  `stream_needs_data()` -- they automatically adapt to the new constant.
- Do NOT change `process_track()`, `ADX_ProcessTracks()`, or any decoder
  logic.
- Do NOT change `create_audio_stream()` or any SDL API call patterns.
- Do NOT touch `adx.h` -- the public API is unchanged.

### How to verify

1. **Grep impact:** Search for `MIN_QUEUED_DATA_MS` -- it should appear
   only twice: the `#define` and inside `min_queued_data_bytes()`. Confirm
   no other code references it.
2. **Byte calculation:** `100 * 48000 / 1000 * 2 * 2 = 19,200 bytes`.
   This is well within SDL's internal buffer capacity.
3. **Compile check:** Build the ARM binary (`make` or equivalent). This
   is a trivial constant change so compile failure is unlikely, but verify.
4. **Logic review:** `ADX_ProcessTracks()` is called once per frame from
   `SDLApp_EndFrame()` (line 9733 of `sdl_app.c`). At 59.6 Hz, each frame
   drains ~806 samples = ~3,224 bytes. With 19,200 bytes buffered, that is
   ~5.95 frames of data -- enough to survive one missed frame plus jitter.

### Dependencies

None (independent of Step 1), but ordered second because it is slightly
more invasive than the Verilog change.

---

## Step 3: Add SDL3 audio clock recovery (dynamic rate control)

**Goal:** Eliminate clock drift between ARM and FPGA by dynamically
adjusting the SDL audio stream frequency ratio, keeping the pipeline
buffer at a stable fill level.

This is the key fix. Steps 1 and 2 reduce baseline latency. Step 3
prevents latency from growing over time.

### Files to modify

1. `/Users/sb/Developer/3sx-mister-audio-fix/src/port/sound/spu.c`
2. `/Users/sb/Developer/3sx-mister-audio-fix/src/port/sound/adx.c`

### Design: Near/byuu proportional controller

The technique is the same used by RetroArch for audio-video sync. A
proportional controller adjusts the audio production rate based on buffer
fill level:

```
ratio = (1.0 - max_delta) + 2.0 * fill_level * max_delta
```

- `fill_level`: 0.0 (empty) to 1.0 (full)
- `max_delta`: maximum pitch adjustment = 0.005 (0.5%, inaudible)
- When `fill_level = 0.5`: ratio = 1.0 (no adjustment)
- When `fill_level = 0.0`: ratio = 0.995 (produce slower, buffer draining)
- When `fill_level = 1.0`: ratio = 1.005 (produce faster, buffer filling)

`SDL_SetAudioStreamFrequencyRatio(stream, ratio)` tells SDL3 to resample
the stream's output by the given ratio. A ratio > 1.0 makes samples play
faster (higher pitch, drains buffer faster). A ratio < 1.0 makes samples
play slower (lower pitch, drains buffer slower).

### Part A: SPU clock recovery (spu.c)

#### State variables

Add these static variables near the top of `spu.c`, after the existing
`static SDL_AudioStream* stream;` declaration (line 63):

```c
// Clock recovery: dynamic rate control to prevent ARM/FPGA drift
#define SPU_CR_TARGET_BYTES (2048 * 2 * sizeof(s16))  // ~42.7ms at 48kHz stereo S16
#define SPU_CR_MAX_DELTA    0.005                       // 0.5% max pitch adjustment
#define SPU_CR_UPDATE_INTERVAL 192                      // Update every 192 callbacks (~4ms each = ~768ms)
                                                        // Smooths out per-callback jitter
static int cr_update_counter = 0;
```

These go after line 68 (after the `static u64 active_voices = 0;` line),
before the `SPU_Ctz64` function.

#### Target buffer size rationale

`SPU_CR_TARGET_BYTES = 2048 * 2 * sizeof(s16) = 8192 bytes` represents
2048 stereo samples = 42.7ms at 48kHz. This matches the new hurryup=1
threshold in alsa.sv (Step 1), so the ARM-side controller keeps the buffer
near the FPGA's first ramp-up point. Fill level 0.5 = half of 8192 = 4096
bytes = 1024 samples = ~21.3ms, which is a comfortable operating point.

#### Update interval rationale

`SPU_CR_UPDATE_INTERVAL = 192` means we recalculate the ratio every 192
callback invocations. Each SPU_SDL_CB invocation processes
`additional_amount / 4` samples (S16 stereo = 4 bytes/sample). SDL3
typically requests ~1024-2048 samples per callback at 48kHz, so 192
callbacks is roughly every 192 * ~1024 / 48000 ~= 4 seconds. This provides
very heavy smoothing to avoid reacting to transient buffer level changes.

**Alternative considered:** Updating every single callback would be more
responsive but risks oscillation since each callback's queued amount
fluctuates depending on when SDL's mixer thread runs relative to the ALSA
drain. The high interval acts as a natural low-pass filter on the
measurement noise.

#### Code change in SPU_SDL_CB

In the `SPU_SDL_CB` function (line 332), add clock recovery logic at the
end of the function, after the existing `while (samples_per_channel)`
loop. The full function becomes:

**Before (lines 332-365):**
```c
void SPU_SDL_CB(void* user, SDL_AudioStream* stream, int additional_amount, int total_amount) {
    u32 samples_per_channel = (additional_amount / sizeof(s16)) >> 1;
    static s16 outbuf[4096] = {};

    // We need to run the eml callbaack at 250hz
    // 48000 / 250 = 192
    static int cb_timer = 192;

    while (samples_per_channel) {
        // Keep audio lock hold time bounded so gameplay-thread calls that take
        // soundLock (voice setup/key-off) do not stall for long stretches.
        u32 batch_count = min(samples_per_channel, 256);

        // TODO consider redesigning this whole system; emlshim and spu should
        // probably run on the same thread so this lock can be removed entirely.
        SDL_LockMutex(soundLock);

        s16* p = outbuf;
        for (int i = 0; i < batch_count; i++) {
            SPU_Tick(p);
            p += 2;

            cb_timer--;
            if (!cb_timer) {
                timer_cb();
                cb_timer = 192;
            }
        }

        SDL_UnlockMutex(soundLock);
        SDL_PutAudioStreamData(stream, outbuf, (batch_count * sizeof(s16)) << 1);
        samples_per_channel -= batch_count;
    }
}
```

**After:**
```c
void SPU_SDL_CB(void* user, SDL_AudioStream* stream_cb, int additional_amount, int total_amount) {
    u32 samples_per_channel = (additional_amount / sizeof(s16)) >> 1;
    static s16 outbuf[4096] = {};

    // We need to run the eml callbaack at 250hz
    // 48000 / 250 = 192
    static int cb_timer = 192;

    while (samples_per_channel) {
        // Keep audio lock hold time bounded so gameplay-thread calls that take
        // soundLock (voice setup/key-off) do not stall for long stretches.
        u32 batch_count = min(samples_per_channel, 256);

        // TODO consider redesigning this whole system; emlshim and spu should
        // probably run on the same thread so this lock can be removed entirely.
        SDL_LockMutex(soundLock);

        s16* p = outbuf;
        for (int i = 0; i < batch_count; i++) {
            SPU_Tick(p);
            p += 2;

            cb_timer--;
            if (!cb_timer) {
                timer_cb();
                cb_timer = 192;
            }
        }

        SDL_UnlockMutex(soundLock);
        SDL_PutAudioStreamData(stream_cb, outbuf, (batch_count * sizeof(s16)) << 1);
        samples_per_channel -= batch_count;
    }

    // Clock recovery: adjust frequency ratio to keep buffer near target fill level.
    // This compensates for ARM/FPGA clock drift that otherwise causes latency to
    // grow over time. Runs every SPU_CR_UPDATE_INTERVAL callbacks to smooth noise.
    cr_update_counter++;
    if (cr_update_counter >= SPU_CR_UPDATE_INTERVAL) {
        cr_update_counter = 0;
        int queued = SDL_GetAudioStreamQueued(stream);
        if (queued >= 0) {
            double fill_level = (double)queued / (double)SPU_CR_TARGET_BYTES;
            if (fill_level > 1.0) fill_level = 1.0;
            double ratio = (1.0 - SPU_CR_MAX_DELTA) + 2.0 * fill_level * SPU_CR_MAX_DELTA;
            // Clamp to [1 - max_delta, 1 + max_delta]
            if (ratio < 1.0 - SPU_CR_MAX_DELTA) ratio = 1.0 - SPU_CR_MAX_DELTA;
            if (ratio > 1.0 + SPU_CR_MAX_DELTA) ratio = 1.0 + SPU_CR_MAX_DELTA;
            SDL_SetAudioStreamFrequencyRatio(stream, (float)ratio);
        }
    }
}
```

**Key details about the callback parameter rename:** The callback receives
a parameter named `stream` which shadows the file-scope `static
SDL_AudioStream* stream`. The existing code uses `stream` in
`SDL_PutAudioStreamData(stream, ...)` which actually refers to the
callback parameter (C scoping rules: inner scope wins). We rename the
callback parameter to `stream_cb` and update the `SDL_PutAudioStreamData`
call to use `stream_cb`. The clock recovery code at the bottom uses the
file-scope `stream` (for `SDL_GetAudioStreamQueued` and
`SDL_SetAudioStreamFrequencyRatio`) which is the same object -- but this
makes the intent explicit and avoids relying on shadowing.

**IMPORTANT:** Verify that `SDL_GetAudioStreamQueued` called on the
file-scope `stream` returns the amount of data queued *for playback* (i.e.,
data that has been put into the stream but not yet consumed by the audio
device). Per SDL3 docs, this is the correct behavior for a playback stream.

#### Edge cases

- **Startup (stream just created):** `queued` will be 0 (or very small).
  `fill_level` = 0.0, `ratio` = 0.995. This slows production slightly,
  which is fine -- the buffer will fill naturally from the callback
  producing data. Since the SPU callback is demand-driven (SDL requests
  data), the ratio adjustment affects how SDL resamples the *output* of
  the stream, not how often the callback fires. At ratio 0.995, SDL
  slightly slows the drain rate, allowing the buffer to build up toward
  the target.

- **Underrun (queued drops to 0):** Same as startup -- ratio goes to
  0.995, slowing drain. No special handling needed because the SPU callback
  generates data on demand; the stream won't starve as long as the callback
  keeps being called.

- **Overflow (queued exceeds target):** `fill_level` is clamped to 1.0,
  `ratio` = 1.005. SDL drains faster. The FPGA's hurryup mechanism
  (Step 1) also kicks in at higher fill levels, providing a secondary
  drain path.

- **Stream is NULL:** `SDL_GetAudioStreamQueued(NULL)` returns 0 per SDL3
  docs. The `queued >= 0` check passes, `fill_level` = 0.0. This is
  harmless -- ratio gets set on a NULL stream which is a no-op.

### Part B: ADX clock recovery (adx.c)

#### State variables

Add these static variables in `adx.c`, after the existing
`static int output_sample_rate = DEFAULT_SAMPLE_RATE;` line (line 89):

```c
// Clock recovery: dynamic rate control to prevent ARM/FPGA drift
#define ADX_CR_MAX_DELTA        0.005   // 0.5% max pitch adjustment
#define ADX_CR_UPDATE_INTERVAL  60      // Update every 60 frames (~1 second at 59.6 Hz)
static int adx_cr_update_counter = 0;
```

#### Target buffer size

For ADX, the target buffer is `min_queued_data_bytes()` (which after
Step 2 returns 19,200 bytes = 100ms). The fill level midpoint (0.5) = 50ms
of buffered data.

#### Code change in ADX_ProcessTracks

Add clock recovery at the end of `ADX_ProcessTracks()` (line 649), after
the existing track processing loop. This runs once per game frame (~59.6
Hz) since `ADX_ProcessTracks()` is called from `SDLApp_EndFrame()`.

**Before (lines 649-679):**
```c
void ADX_ProcessTracks() {
    if (num_tracks == 0) {
        return;
    }

    if (!stream_needs_data() && !track_exhausted(&tracks[first_track_index])) {
        return;
    }

    const int first_track_index_old = first_track_index;
    const int num_tracks_old = num_tracks;

    for (int i = 0; i < num_tracks_old; i++) {
        const int j = (first_track_index_old + i) % TRACKS_MAX;
        ADXTrack* track = &tracks[j];
        process_track(track);

        if (!track_exhausted(track)) {
            break;
        }

        track_destroy(track);
        num_tracks -= 1;

        if (num_tracks > 0) {
            first_track_index += 1;
        } else {
            first_track_index = 0;
        }
    }
}
```

**After:**
```c
void ADX_ProcessTracks() {
    if (num_tracks == 0) {
        return;
    }

    if (!stream_needs_data() && !track_exhausted(&tracks[first_track_index])) {
        goto clock_recovery;
    }

    {
        const int first_track_index_old = first_track_index;
        const int num_tracks_old = num_tracks;

        for (int i = 0; i < num_tracks_old; i++) {
            const int j = (first_track_index_old + i) % TRACKS_MAX;
            ADXTrack* track = &tracks[j];
            process_track(track);

            if (!track_exhausted(track)) {
                break;
            }

            track_destroy(track);
            num_tracks -= 1;

            if (num_tracks > 0) {
                first_track_index += 1;
            } else {
                first_track_index = 0;
            }
        }
    }

clock_recovery:
    // Clock recovery: adjust frequency ratio to keep buffer near target fill level.
    // Runs every ADX_CR_UPDATE_INTERVAL frames (~1 second) to smooth measurement noise.
    if (stream == NULL) {
        return;
    }
    adx_cr_update_counter++;
    if (adx_cr_update_counter >= ADX_CR_UPDATE_INTERVAL) {
        adx_cr_update_counter = 0;
        int queued = SDL_GetAudioStreamQueued(stream);
        if (queued >= 0) {
            int target = min_queued_data_bytes();
            double fill_level = (target > 0) ? (double)queued / (double)target : 0.5;
            if (fill_level > 1.0) fill_level = 1.0;
            double ratio = (1.0 - ADX_CR_MAX_DELTA) + 2.0 * fill_level * ADX_CR_MAX_DELTA;
            if (ratio < 1.0 - ADX_CR_MAX_DELTA) ratio = 1.0 - ADX_CR_MAX_DELTA;
            if (ratio > 1.0 + ADX_CR_MAX_DELTA) ratio = 1.0 + ADX_CR_MAX_DELTA;
            SDL_SetAudioStreamFrequencyRatio(stream, (float)ratio);
        }
    }
}
```

**Key structural change:** The early `return` on line 654 (when the
buffer does not need data and the first track is not exhausted) is
converted to `goto clock_recovery` so that clock recovery still runs every
frame regardless of whether new audio data was decoded. Without this,
clock recovery would only execute when the buffer drops below the minimum
threshold, defeating the purpose.

The track processing loop is wrapped in a bare block `{ ... }` to contain
the local variable declarations (`first_track_index_old`,
`num_tracks_old`) that would otherwise be skipped by the `goto`.

#### Reset clock recovery state on stream changes

The `adx_cr_update_counter` must be reset when the stream is
recreated or stopped, to avoid stale state. Add a reset in two places:

1. In `create_audio_stream()` (line 123), after `SDL_ResumeAudioStreamDevice(stream);`:

   **Add after line 137:**
   ```c
       adx_cr_update_counter = 0;
   ```

2. In `ADX_Stop()` (line 694), after `SDL_ClearAudioStream(stream);`:

   **Add after line 697:**
   ```c
       adx_cr_update_counter = 0;
   ```

   Also reset the frequency ratio to 1.0 when stopping, so the stream
   starts fresh next time:

   **The ADX_Stop SDL_ClearAudioStream block becomes:**
   ```c
   if (stream != NULL) {
       ADX_Pause(true);
       SDL_ClearAudioStream(stream);
       SDL_SetAudioStreamFrequencyRatio(stream, 1.0f);
       adx_cr_update_counter = 0;
   }
   ```

#### Edge cases

- **No tracks playing (`num_tracks == 0`):** The function returns before
  reaching clock recovery. This is correct -- with no music, there is
  nothing to adjust.

- **Stream paused:** `SDL_GetAudioStreamQueued` still returns the queued
  amount even when paused. The ratio adjustment is set but has no effect
  until playback resumes. This is harmless.

- **Track transition:** When `ADX_Stop()` is called followed by a new
  `ADX_Start*()`, the counter resets and ratio resets to 1.0. The new
  track starts with a fresh fill-up to `min_queued_data_bytes()` and
  clock recovery gradually adjusts from there.

- **Sample rate change:** If a new track has a different sample rate,
  `create_audio_stream()` destroys and recreates the stream, which resets
  the counter. The new stream starts with ratio 1.0.

### What NOT to change

- Do NOT add any new public API to `spu.h` or `adx.h`.
- Do NOT modify `sdl_app.c` -- `ADX_ProcessTracks()` is already called
  at the right place (line 9733 of `SDLApp_EndFrame()`).
- Do NOT add any logging, printf, or SDL_Log calls in the hot path (the
  clock recovery runs every N callbacks/frames and must be zero-overhead).
- Do NOT read `/dev/MrAudio` or add any FPGA-side feedback -- that is a
  Phase 2 enhancement documented in the research but out of scope here.
- Do NOT change the frame pacing logic in `sdl_app.c` (lines 9973-9996).
- Do NOT modify `audio_out.v`, `sys_top.v`, or any other FPGA file.
- Do NOT change `emlShim.c` or the `workTick` 250Hz timer mechanism.

### How to verify

1. **Compile check:** Build the ARM binary. The new code uses only:
   - `SDL_GetAudioStreamQueued()` -- already used in `adx.c` line 108/120
   - `SDL_SetAudioStreamFrequencyRatio()` -- new SDL3 API call, verify it
     exists in the project's SDL3 headers
   - Basic floating-point math (`double` arithmetic)

2. **API verification:** Confirm `SDL_SetAudioStreamFrequencyRatio` is
   available in the project's SDL3 version:
   ```
   grep -r "SDL_SetAudioStreamFrequencyRatio" vendor/SDL/
   ```
   This was added in SDL 3.0.0 and takes `(SDL_AudioStream*, float)`.

3. **Logic review checklist:**
   - SPU: `cr_update_counter` increments every callback, resets at 192.
     `fill_level` is `queued / 8192`. Ratio range: [0.995, 1.005].
   - ADX: `adx_cr_update_counter` increments every frame, resets at 60.
     `fill_level` is `queued / min_queued_data_bytes()`. Same ratio range.
   - Both clamp `fill_level` to [0.0, 1.0] and ratio to
     [0.995, 1.005].
   - Counter resets happen in `create_audio_stream()` and `ADX_Stop()`.

4. **Behavioral review:**
   - If ARM clock is 10 PPM fast (produces 48000.48 Hz vs FPGA's 48000):
     buffer slowly fills, `fill_level` rises above 0.5, ratio rises above
     1.0, SDL resamples output faster, effective drain rate increases,
     buffer stabilizes.
   - If ARM clock is 10 PPM slow: buffer slowly drains, `fill_level`
     drops below 0.5, ratio drops below 1.0, SDL slows drain, buffer
     stabilizes.
   - At ±50 PPM worst case: 2.4 samples/sec drift. The proportional
     controller at 0.5% max adjustment can compensate up to 240
     samples/sec -- 100x headroom.

### Dependencies

- Depends on Step 2 (ADX `MIN_QUEUED_DATA_MS = 100`) because the ADX
  clock recovery target is `min_queued_data_bytes()` which uses that
  constant.
- Logically benefits from Step 1 (tighter FPGA thresholds) because the
  SPU target (42.7ms) is chosen to match the new hurryup=1 threshold.
  However, the code will work correctly even if Step 1 is not yet deployed
  to the FPGA -- the ARM-side controller operates independently.

---

## Summary

| Step | Risk | Files | Key change |
|---|---|---|---|
| 1 | Low | `alsa.sv` | Shift 6 bit-slice constants by 1-2 bits |
| 2 | Low | `adx.c` | Change one `#define` from 400 to 100 |
| 3 | Medium | `spu.c`, `adx.c` | Add ~25 lines of proportional controller |

Expected latency improvement:
- Step 1: Peak sawtooth reduced from ~85ms to ~43ms
- Step 2: ADX baseline reduced from 400ms to 100ms
- Step 3: Drift eliminated -- buffer stays near target indefinitely
