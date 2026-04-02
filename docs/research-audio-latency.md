# Audio Latency Research: Delay That Grows Over Time

## Problem Statement

Users report noticeable audio delay that gets worse over time during gameplay.
This is a classic symptom of clock drift causing buffer accumulation in the
audio pipeline.

## Full Audio Pipeline (ARM to Speaker)

```
SPU (sound effects, callback) ──→ SDL3 stream 1 ──┐
                                                    ├→ SDL3 ALSA backend
ADX (music, push-based)       ──→ SDL3 stream 2 ──┘
        │
        ▼
ALSA "file" plugin → /dev/MrAudio (kernel char device)
        │
        ▼ (SPI write: buf_addr, buf_len, buf_wptr)
DDR3 ring buffer (512 KB, DMA-coherent)
        │
        ▼ (DMA read at ce_sample rate)
FPGA alsa.sv → audio_out.v mixing → I2S / SPDIF / Sigma-Delta DAC
```

### Layer Details

**Layer 1 — Application (ARM userspace)**
- SPU (`src/port/sound/spu.c`): 48-voice ADPCM synthesizer. Callback-based
  SDL3 audio stream at 48000 Hz stereo S16. `SPU_SDL_CB` generates samples on
  demand when SDL requests them. Also runs `workTick()` every 192 samples
  (250 Hz) for voice envelope/LFO updates.
- ADX (`src/port/sound/adx.c`): Background music decoder (Capcom ADX codec).
  Push-based SDL3 audio stream at 48000 Hz stereo S16. `ADX_ProcessTracks()`
  called once per frame from `SDLApp_EndFrame()`. Maintains a minimum buffer
  of `MIN_QUEUED_DATA_MS` (currently **400ms**).

**Layer 2 — SDL3 ALSA backend**
- Audio driver explicitly set to `"alsa"` (`sdl_app.c:684`).
- Two separate `SDL_OpenAudioDeviceStream()` calls (SPU + ADX) go to the
  same default ALSA PCM device.

**Layer 3 — ALSA kernel driver (snd_dummy)**
- MiSTer Linux kernel has `CONFIG_SND_DUMMY=y` with a custom `model_MiSTer`:
  buffer_bytes_max=32768, S16_LE, stereo, 48000 Hz only.
- The `dummy_pcm_copy()` function returns 0 — it does NOT write audio anywhere.
- Exists solely to provide a standard ALSA PCM device for applications.

**Layer 4 — MiSTer audio SPI kernel driver**
- `CONFIG_SND_MISTER_AUDIO=y` builds `sound/drivers/MiSTer-audio-spi.c`.
- Creates `/dev/MrAudio` character device.
- On `write()`: copies PCM data into a 512KB DMA-coherent buffer, advances
  write pointer, sends `Info_t` struct to FPGA via `spi_write()`.
- On `read()`: does `spi_read()` to get `buf_rptr` and `hurryup` from FPGA.
- ALSA `file` plugin in `/etc/asound.conf` redirects PCM output to
  `/dev/MrAudio`.

**Layer 5 — FPGA alsa.sv**
- File: `build/mister-wrapper-core/src/sys/alsa.sv`
- Runs on `clk_audio` (24.576 MHz from `pll_audio`).
- Receives `buf_wptr`, `buf_len`, `buf_addr` from HPS via SPI.
- Sends back `{buf_rptr, hurryup, 8'h00}` via SPI.
- DMA reads 64-bit words from DDR3 at `buf_addr + buf_rptr`.
- Each 64-bit word = 2 stereo sample pairs (L16+R16 × 2).
- Generates `ce_sample` at `48000 + hurryup*64` Hz using a DDS accumulator.

**Layer 6 — audio_out.v**
- Mixes core audio (AUDIO_L/R from MT32 I2S) with ALSA audio (from alsa.sv).
- Outputs via I2S (HDMI), SPDIF, and sigma-delta DAC (analog).

## Root Cause Analysis

### Primary cause: ARM/FPGA clock drift with no compensation

The ARM HPS produces audio at 48000 Hz according to its system clock.
The FPGA consumes audio at 48000 Hz according to its 24.576 MHz PLL.
These are **independent oscillators**. Any PPM difference causes the DDR3
ring buffer to slowly fill or drain.

Typical crystal tolerance is ±50 PPM. Worst case: 100 PPM combined drift =
4.8 extra samples/sec = 17,280 samples/hour ≈ 360ms/hour of accumulating
delay.

### Secondary cause: hurryup thresholds are way too high

The FPGA's adaptive rate mechanism (`hurryup` in alsa.sv) is supposed to
compensate for drift, but its activation thresholds are extremely high:

| Transition | Trigger condition | Buffer fill | Latency before activation |
|---|---|---|---|
| 0→1 ramp-up | `len[18:14]` ≥ 2048 words | 4096 samples | **85.3 ms** |
| 1→2 ramp-up | `len[18:16]` ≥ 8192 words | 16384 samples | **341.3 ms** |
| 2→4 ramp-up | `len[18:17]` ≥ 16384 words | 32768 samples | **682.7 ms** |

Ramp-down thresholds:

| Transition | Trigger condition | Buffer fill | Latency |
|---|---|---|---|
| 4→2 | `!len[18:15]` < 4096 words | < 8192 samples | **170.7 ms** |
| 2→1 | `!len[18:13]` < 1024 words | < 2048 samples | **42.7 ms** |
| 1→0 | `!len[18:10]` < 128 words | < 256 samples | **5.3 ms** |

The hurryup rates:

| hurryup | DDS increment | Effective rate | Boost |
|---|---|---|---|
| 0 | 48,000 | 48,000 Hz | +0.000% |
| 1 | 48,064 | 48,064 Hz | +0.133% |
| 2 | 48,128 | 48,128 Hz | +0.267% |
| 4 | 48,256 | 48,256 Hz | +0.533% |

This creates a **sawtooth latency pattern**: delay ramps from ~5ms to ~85ms
(or higher) over minutes, hurryup briefly drains it, then the cycle repeats.
Users perceive this as delay that "gets worse over time."

### Tertiary cause: ADX 400ms minimum buffer

`MIN_QUEUED_DATA_MS = 400` in adx.c adds 400ms of baseline music latency
in the SDL push buffer, on top of all other buffering stages.

## Fix 1: Reduce alsa.sv hurryup thresholds

Shift all thresholds 2-4x more aggressive while maintaining stable hysteresis.

**Proposed thresholds:**

| Transition | Current | Proposed | Latency change |
|---|---|---|---|
| 0→1 ramp-up | `len[18:14]` (85.3ms) | `len[18:13]` (42.7ms) | 2× better |
| 1→2 ramp-up | `len[18:16]` (341.3ms) | `len[18:14]` (85.3ms) | 4× better |
| 2→4 ramp-up | `len[18:17]` (682.7ms) | `len[18:15]` (170.7ms) | 4× better |
| 4→2 ramp-down | `!len[18:15]` (170.7ms) | `!len[18:14]` (85.3ms) | tighter |
| 2→1 ramp-down | `!len[18:13]` (42.7ms) | `!len[18:12]` (21.3ms) | tighter |
| 1→0 ramp-down | `!len[18:10]` (5.3ms) | `!len[18:10]` (5.3ms) | unchanged |

Hysteresis band stability check:

| Level | Ramp-up | Ramp-down | Band width | Safe? |
|---|---|---|---|---|
| hurryup=1 | 1024 words (43ms) | <128 words (5ms) | 896 words (37ms) | Yes (>21ms ALSA period) |
| hurryup=2 | 2048 words (85ms) | <512 words (21ms) | 1536 words (64ms) | Yes |
| hurryup=4 | 4096 words (171ms) | <2048 words (85ms) | 2048 words (85ms) | Yes |

**Verilog change** (alsa.sv lines 95-103):
```verilog
// BEFORE:
//ramp up
if(len[18:14] && (hurryup < 1)) hurryup <= 1;
if(len[18:16] && (hurryup < 2)) hurryup <= 2;
if(len[18:17] && (hurryup < 4)) hurryup <= 4;

//ramp down
if(!len[18:15] && (hurryup > 2)) hurryup <= 2;
if(!len[18:13] && (hurryup > 1)) hurryup <= 1;
if(!len[18:10]) hurryup <= 0;

// AFTER:
//ramp up
if(len[18:13] && (hurryup < 1)) hurryup <= 1;
if(len[18:14] && (hurryup < 2)) hurryup <= 2;
if(len[18:15] && (hurryup < 4)) hurryup <= 4;

//ramp down
if(!len[18:14] && (hurryup > 2)) hurryup <= 2;
if(!len[18:12] && (hurryup > 1)) hurryup <= 1;
if(!len[18:10]) hurryup <= 0;
```

## Fix 2: Reduce ADX MIN_QUEUED_DATA_MS

**Current value:** 400ms (76,800 bytes, ~24 frames of runway)

**Analysis of minimum required buffer:**
- ADX_ProcessTracks runs once per frame at ~59.6 Hz (~16.78ms between calls)
- Per-frame drain: ~806 samples = ~3,224 bytes
- Worst realistic scenario: late frame (34ms) + track transition gap (17ms)
  + OS jitter (15ms) = ~66ms needed
- Safety margin: 34ms

**Proposed value:** 100ms (19,200 bytes, ~6 frames of runway)

If testing reveals glitches during heavy gameplay + track transitions,
increase to 150ms. Going above 150ms is over-provisioned.

**Code change** (adx.c line 28):
```c
// BEFORE:
#define MIN_QUEUED_DATA_MS 400

// AFTER:
#define MIN_QUEUED_DATA_MS 100
```

## Fix 4: SDL3 Audio Clock Recovery

The real solve. Uses `SDL_SetAudioStreamFrequencyRatio()` to dynamically
adjust the audio production rate, keeping the pipeline buffer at a stable
target level.

### Approach: Near/byuu's dynamic rate control

This is the same technique used by Libretro/RetroArch for audio-video sync.

**Core formula (proportional control):**
```c
// fill_level: 0.0 (empty) to 1.0 (full), measured from SDL_GetAudioStreamQueued()
// max_delta: maximum pitch adjustment (0.005 = 0.5%, inaudible)
// target: 0.5 (aim for buffer 50% full)

double ratio = (1.0 - max_delta) + 2.0 * fill_level * max_delta;
SDL_SetAudioStreamFrequencyRatio(stream, ratio);
```

When `fill_level = 0.5`: ratio = 1.0 (no adjustment)
When `fill_level = 0.0`: ratio = 0.995 (slow down, buffer is draining)
When `fill_level = 1.0`: ratio = 1.005 (speed up, buffer is filling)

**Implementation location:** SPU audio callback in `spu.c` and ADX processing
in `adx.c`. Run the adjustment once per frame (~60 Hz) or every N callbacks.

**Target buffer size for fill_level calculation:**
- SPU: use `SDL_GetAudioStreamQueued()` relative to a target of ~2048 samples
  (~42ms, matching the new alsa.sv hurryup=1 threshold)
- ADX: use `SDL_GetAudioStreamQueued()` relative to `min_queued_data_bytes()`

**Startup/resync:**
- Let buffer fill to ~50% target before enabling playback
- If buffer underruns (drains to 0), pause briefly to refill to 50%
- Clamp ratio to [0.995, 1.005] to avoid audible pitch artifacts

### Advanced option: Read FPGA buf_rptr directly

If SDL-side measurement is insufficient, `/dev/MrAudio` supports `read()`
which returns `buf_rptr` and `hurryup` from the FPGA via SPI. This gives
the true FPGA-side consumption state for a tighter feedback loop.

```c
int fd = open("/dev/MrAudio", O_RDONLY);
uint32_t rptr_data;
read(fd, &rptr_data, 4);
// rptr_data contains {buf_rptr[31:16], hurryup[15:13], zeros[12:0]}
```

This is a Phase 2 enhancement if Phase 1 (SDL-only) proves insufficient.

## Key Source Files

| File | Role |
|---|---|
| `src/port/sound/spu.c` | SPU synth, SDL callback, clock recovery target |
| `src/port/sound/adx.c` | ADX music decoder, push-based SDL stream |
| `src/port/sound/emlShim.c` | Voice management bridge (workTick at 250 Hz) |
| `src/port/sdl/sdl_app.c` | Frame pacing, ADX_ProcessTracks call site |
| `include/port/sdl/sdl_app.h` | TARGET_FPS = 59.59949 |
| `build/mister-wrapper-core/src/sys/alsa.sv` | FPGA audio consumer, hurryup |
| `build/mister-wrapper-core/src/sys/audio_out.v` | FPGA audio mixer/DAC |
| `build/mister-wrapper-core/src/sys/sys_top.v` | ALSA instantiation (L1656-1694) |
| `build/mister-wrapper-core/src/3SX.sv` | Core audio routing (AUDIO_L/R = MT32 I2S) |
| `vendor/Main_MiSTer/audio.cpp` | Volume/filter config (SPI commands only) |
