#ifndef PORT_SOUND_MI_AO_BACKEND_H
#define PORT_SOUND_MI_AO_BACKEND_H

/* Direct MI_AO audio backend for the Miyoo Mini Plus / OnionOS port.
 *
 * Bypasses SDL3 audio + the kernel /dev/dsp OSS shim entirely. We talk
 * to the SigmaStar MI_AO HAL via the vendored headers in
 * vendor/sigmastar/inc/ and link against vendor/sigmastar/lib/libmi_ao.so.
 *
 * Why: SDL3-OSS on this device is broken two ways:
 *   - With KillAudioserver=0 + libpadsp preloaded, the OSS device probe
 *     hits EBUSY because audioserver holds /dev/dsp; libpadsp's open()
 *     shim does not relieve this for SDL3.
 *   - With KillAudioserver=1, SDL3 opens /dev/dsp directly but the
 *     kernel shim does not propagate SNDCTL_DSP_SPEED through to MI_AO,
 *     so audio plays at MI_AO's default rate (~half-speed and garbled).
 *
 * Public surface:
 *   - MIAO_Init / MIAO_Shutdown set up and tear down the device.
 *   - MIAO_RegisterTimerCB installs the 250 Hz emlShim tick callback.
 *     The audio thread invokes the callback inline from the SPU
 *     generator (every 192 generated samples at 48 kHz).
 *   - MIAO_PushADX is the producer side of an SPSC ring used by the
 *     ADX path; MIAO_QueuedFramesADX returns the current ring depth in
 *     interleaved-stereo frames.
 *
 * The backend is only meaningful under PORT_MIYOO_MINI_PLUS. The SDL3
 * audio path remains compiled in and is selected at runtime when
 * THIRDSARM_AUDIO_SDL=1 is set, so we can A/B compare in the field.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true when the device was successfully programmed and the
 * audio thread started. A false return means we fell back to the SDL3
 * path (or device init failed) — callers should treat MI_AO as
 * unusable for the rest of the process lifetime. */
bool MIAO_Init(void);

/* Stops the audio thread, joins it, disables the MI_AO channel and
 * device. Safe to call multiple times. Idempotent. */
void MIAO_Shutdown(void);

/* Returns true once MIAO_Init() succeeded; false otherwise. The SDL3
 * audio glue uses this to decide whether to run the SDL3 path or
 * route through the direct backend. */
bool MIAO_IsActive(void);

/* Registers the 250 Hz timer callback that the audio thread invokes
 * once per 192 generated SPU samples. Pass NULL to clear. The pointer
 * is read by the audio thread without a memory barrier — set this
 * BEFORE the first SPU frame is generated (i.e. before SPU_Init
 * returns under PORT_MIYOO_MINI_PLUS). */
void MIAO_RegisterTimerCB(void (*cb)(void));

/* Pushes `frames` worth of interleaved s16le stereo samples into the
 * ADX SPSC ring. `interleaved` points at frames * 2 * sizeof(int16_t)
 * bytes. If the ring is full the oldest frames are silently dropped to
 * make room — this matches what SDL_PutAudioStreamData would do once
 * its internal queue saturates. */
void MIAO_PushADX(const int16_t *interleaved, int frames);

/* Returns the number of interleaved-stereo frames currently queued in
 * the ADX ring, for use by adx.c's pacing math (`MIN_QUEUED_DATA_MS`
 * back-pressure). Multiply by N_CHANNELS * BYTES_PER_SAMPLE (= 4) to
 * get the equivalent of SDL_GetAudioStreamQueued's byte count. */
int MIAO_QueuedFramesADX(void);

/* Clears the ADX SPSC ring (consumer side). Safe to call from the
 * producer thread because the consumer treats the ring as empty when
 * head == tail and a torn read can never produce a positive count.
 * Used by ADX_Stop() to flush whatever the consumer hasn't drained. */
void MIAO_ClearADX(void);

/* Sets a scalar gain applied to the ADX stream during mix-down. Range
 * is the same as SDL_SetAudioStreamGain (0.0f .. >1.0f); 1.0f is unity.
 * Stored as an atomic int interpreted as Q14 fixed-point internally,
 * so the audio thread reads it without a lock. */
void MIAO_SetADXGain(float gain);

/* Returns true when the runtime env has requested the legacy SDL3+OSS
 * audio path (THIRDSARM_AUDIO_SDL=1). Used by ADX_Init and SPU_Init to
 * decide whether to bring up the direct MI_AO backend or stay on SDL3.
 * Centralised here so the env-var contract has one source of truth and
 * the two callers cannot drift. */
bool MIAO_ForceSDLFallback(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_SOUND_MI_AO_BACKEND_H */
