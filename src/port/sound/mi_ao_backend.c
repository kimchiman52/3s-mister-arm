/* Direct MI_AO audio backend for the Miyoo Mini Plus / OnionOS port.
 *
 * See mi_ao_backend.h for the high-level rationale. This TU compiles
 * to an empty object on every other build profile (PORT_MISTER, Mac,
 * Linux, Windows) — the entire body sits inside
 * `#if defined(PORT_MIYOO_MINI_PLUS)`.
 *
 * Threading model:
 *   - One dedicated pthread is the audio thread. It owns the MI_AO
 *     send loop, runs at SCHED_FIFO 50, and is the sole consumer of
 *     the ADX SPSC ring. It is also the sole caller of SPU_Tick /
 *     soundLock from the audio side.
 *   - The game thread is the sole producer to the ADX ring (via
 *     MIAO_PushADX), and the sole consumer of the saturating mix
 *     output through the SPU side (via SPU's existing soundLock).
 *
 * Mixing strategy:
 *   - SPU is generated on demand inside the audio thread per send
 *     frame (no ring), preserving the existing SDL_LockMutex(soundLock)
 *     critical section. This matches how the SDL3 callback used to
 *     work, just driven by our own thread instead of SDL's.
 *   - ADX is producer/consumer via an SPSC ring of 48000 frames
 *     (1 second of stereo s16le @ 48 kHz). Atomic head/tail with
 *     acquire/release ordering — no mutex.
 *   - Per send frame: generate SPU into spu_buf, pop ADX into adx_buf,
 *     saturating-mix into mix_buf, hand mix_buf to MI_AO_SendFrame.
 *
 * timer_cb (250 Hz) fires from inside SPU_GenerateInto every 192
 * samples, so the rate is preserved regardless of the audio thread's
 * wake granularity.
 */

#include "port/sound/mi_ao_backend.h"

#if defined(PORT_MIYOO_MINI_PLUS)

#include "port/sound/spu.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mi_ao.h"
#include "mi_ao_datatype.h"
#include "mi_aio_datatype.h"
#include "mi_common.h"
#include "mi_common_datatype.h"
#include "mi_sys.h"

/* ---------------------------------------------------------------- */
/* Configuration                                                    */
/* ---------------------------------------------------------------- */

/* 48 kHz / stereo / s16le — matches SPU_Tick output, ADX swr target,
 * and the eml-shim 250 Hz cadence (250 * 192 = 48000). */
#define MIAO_SAMPLE_RATE   48000
#define MIAO_CHANNELS      2
#define MIAO_BYTES_PER_SAMPLE 2

/* Initial samples-per-MI_AO-frame. 512 = 10.67 ms latency at 48 kHz.
 * Bump to 768 if device underruns are observed in the field. */
#define MIAO_PT_NUM_PER_FRM 512u

/* MI_AO ring depth on the device side (number of u32PtNumPerFrm-sized
 * frames the HAL buffers internally). 6 = ~64 ms at the default frame
 * size — enough headroom for scheduling jitter, small enough that an
 * audio glitch at startup recovers fast. */
#define MIAO_FRM_NUM        6u

/* Single-producer single-consumer ring for the ADX path. Sized at
 * 1 second of stereo (48000 frames). Power-of-two so head/tail wrap
 * with a mask. */
#define MIAO_ADX_RING_FRAMES (1u << 16) /* 65536 frames, ~1.36 s */
#define MIAO_ADX_RING_MASK   (MIAO_ADX_RING_FRAMES - 1u)

/* Audio thread SCHED_FIFO priority. Mirrors spu.c's existing 50 used
 * on PORT_MISTER. Above the game thread's 49 (when game thread is
 * boosted) so the audio path can preempt to drain MI_AO. */
#define MIAO_THREAD_PRIORITY 50

/* ---------------------------------------------------------------- */
/* Internal state                                                   */
/* ---------------------------------------------------------------- */

static struct {
    pthread_t       thread;
    bool            thread_started;
    atomic_bool     stop;
    atomic_bool     active;

    /* SPSC ring for ADX. Stereo interleaved s16le. */
    int16_t        *adx_ring; /* size = MIAO_ADX_RING_FRAMES * 2 */
    atomic_uint     adx_head; /* producer writes here */
    atomic_uint     adx_tail; /* consumer reads here  */

    /* ADX gain as Q14 fixed-point (1.0f -> 16384). */
    atomic_int      adx_gain_q14;

    /* 250 Hz tick callback (eml-shim). Read by the audio thread.
     * Stored as _Atomic so the assignment from MIAO_RegisterTimerCB
     * is a defined cross-thread visibility event; release on store
     * pairs with acquire on load in MIAO_FireTimerCB. */
    void (*_Atomic timer_cb)(void);

    /* Diagnostics. */
    struct timespec init_wall;
    bool            first_frame_logged;
    atomic_uint     adx_underruns;     /* frames of silence due to empty ring */
    atomic_uint     adx_overruns;      /* producer-side dropped frames */
    struct timespec last_underrun_log;

    /* Transient SIGINT/SIGTERM trampoline so we can disable MI_AO
     * before the process dies. We chain the previous handler. */
    struct sigaction prev_sigint;
    struct sigaction prev_sigterm;
    bool             signals_installed;
} s_miao = {
    .adx_gain_q14 = 16384, /* unity */
};

/* SPU generator entry. Defined in spu.c under PORT_MIYOO_MINI_PLUS.
 * Generates `frames` interleaved-stereo samples into out, taking
 * soundLock and ticking the registered 250 Hz callback every 192
 * samples internally. */
extern void SPU_GenerateInto(int16_t *out, int frames);

/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

static double miao_elapsed_ms(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = (double)(now.tv_sec - start->tv_sec) * 1000.0;
    dt += (double)(now.tv_nsec - start->tv_nsec) / 1e6;
    return dt;
}

static int miao_ring_size_frames(unsigned head, unsigned tail) {
    /* head and tail are free-running 32-bit counters; subtract and
     * mask to get the live count within ring capacity. The cast keeps
     * the wrap-around correct. */
    return (int)((unsigned)(head - tail) & MIAO_ADX_RING_MASK);
}

/* ---------------------------------------------------------------- */
/* SPSC ring (ADX)                                                  */
/* ---------------------------------------------------------------- */

void MIAO_PushADX(const int16_t *interleaved, int frames) {
    if (!atomic_load_explicit(&s_miao.active, memory_order_acquire)) {
        return;
    }
    if (frames <= 0 || interleaved == NULL || s_miao.adx_ring == NULL) {
        return;
    }

    unsigned head = atomic_load_explicit(&s_miao.adx_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&s_miao.adx_tail, memory_order_acquire);
    unsigned used = (head - tail) & MIAO_ADX_RING_MASK;
    unsigned free_frames = MIAO_ADX_RING_FRAMES - 1u - used; /* keep one slot empty */

    if ((unsigned)frames > free_frames) {
        /* Drop the overflow at the head — the consumer falls behind
         * gracefully via underrun zero-fill rather than reading torn
         * data. We bump the underrun-overrun diag counter. */
        atomic_fetch_add_explicit(&s_miao.adx_overruns,
                                  (unsigned)frames - free_frames,
                                  memory_order_relaxed);
        frames = (int)free_frames;
        if (frames <= 0) {
            return;
        }
    }

    /* Two-segment copy because the ring may wrap. */
    unsigned head_idx = head & MIAO_ADX_RING_MASK;
    unsigned tail_room = MIAO_ADX_RING_FRAMES - head_idx;
    unsigned first = (unsigned)frames < tail_room ? (unsigned)frames : tail_room;
    memcpy(&s_miao.adx_ring[head_idx * MIAO_CHANNELS],
           interleaved,
           first * MIAO_CHANNELS * sizeof(int16_t));
    if ((unsigned)frames > first) {
        memcpy(&s_miao.adx_ring[0],
               interleaved + first * MIAO_CHANNELS,
               ((unsigned)frames - first) * MIAO_CHANNELS * sizeof(int16_t));
    }

    atomic_store_explicit(&s_miao.adx_head, head + (unsigned)frames,
                          memory_order_release);
}

int MIAO_QueuedFramesADX(void) {
    if (s_miao.adx_ring == NULL) {
        return 0;
    }
    unsigned head = atomic_load_explicit(&s_miao.adx_head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&s_miao.adx_tail, memory_order_acquire);
    return miao_ring_size_frames(head, tail);
}

void MIAO_ClearADX(void) {
    if (s_miao.adx_ring == NULL) {
        return;
    }
    /* Setting tail = head atomically empties the ring from the
     * consumer's perspective. This is technically a race against the
     * audio thread's own tail update — between our store here and the
     * audio thread's release-store of (old_tail + popped) the audio
     * thread can resurrect a few frames of already-popped audio.
     * That's at most ~10 ms of stale audio after Stop, which we
     * accept rather than pulling in a mutex on the hot send path. */
    unsigned head = atomic_load_explicit(&s_miao.adx_head, memory_order_seq_cst);
    atomic_store_explicit(&s_miao.adx_tail, head, memory_order_seq_cst);
}

/* Pops up to `max_frames` from the ring into `out`. Returns frames
 * actually popped (may be 0 if ring empty). */
static int miao_adx_ring_pop(int16_t *out, int max_frames) {
    unsigned tail = atomic_load_explicit(&s_miao.adx_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&s_miao.adx_head, memory_order_acquire);
    unsigned used = (head - tail) & MIAO_ADX_RING_MASK;
    if (used == 0u) {
        return 0;
    }
    unsigned to_pop = used < (unsigned)max_frames ? used : (unsigned)max_frames;

    unsigned tail_idx = tail & MIAO_ADX_RING_MASK;
    unsigned tail_room = MIAO_ADX_RING_FRAMES - tail_idx;
    unsigned first = to_pop < tail_room ? to_pop : tail_room;
    memcpy(out,
           &s_miao.adx_ring[tail_idx * MIAO_CHANNELS],
           first * MIAO_CHANNELS * sizeof(int16_t));
    if (to_pop > first) {
        memcpy(out + first * MIAO_CHANNELS,
               &s_miao.adx_ring[0],
               (to_pop - first) * MIAO_CHANNELS * sizeof(int16_t));
    }

    atomic_store_explicit(&s_miao.adx_tail, tail + to_pop, memory_order_release);
    return (int)to_pop;
}

/* ---------------------------------------------------------------- */
/* Mixing                                                           */
/* ---------------------------------------------------------------- */

static inline int16_t miao_sat_add(int32_t a, int32_t b) {
    int32_t s = a + b;
    if (s >  32767) return  32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

static void miao_mix(const int16_t *spu, int16_t *adx, int16_t *out, int frames,
                     int adx_gain_q14) {
    int n = frames * MIAO_CHANNELS;
    if (adx_gain_q14 == 16384) {
        for (int i = 0; i < n; i++) {
            out[i] = miao_sat_add(spu[i], adx[i]);
        }
    } else {
        for (int i = 0; i < n; i++) {
            int32_t a = ((int32_t)adx[i] * adx_gain_q14) >> 14;
            out[i] = miao_sat_add(spu[i], a);
        }
    }
}

/* ---------------------------------------------------------------- */
/* Audio thread                                                     */
/* ---------------------------------------------------------------- */

static void miao_log_underruns_if_due(unsigned underruns) {
    /* Rate-limit to once every 5 seconds. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s_miao.last_underrun_log.tv_sec == 0 ||
        (now.tv_sec - s_miao.last_underrun_log.tv_sec) >= 5) {
        unsigned overruns = atomic_load_explicit(&s_miao.adx_overruns,
                                                 memory_order_relaxed);
        if (underruns > 0 || overruns > 0) {
            fprintf(stderr,
                    "[MIAO] ADX ring stats: underrun_frames=%u overrun_frames=%u\n",
                    underruns, overruns);
        }
        s_miao.last_underrun_log = now;
    }
}

static void *miao_audio_thread(void *arg) {
    (void)arg;

    /* Boost to SCHED_FIFO. Failure is non-fatal: the device still
     * works at SCHED_OTHER, just with more underruns under load. */
    {
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = MIAO_THREAD_PRIORITY;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            fprintf(stderr,
                    "[MIAO] pthread_setschedparam(SCHED_FIFO,%d) failed: errno=%d\n",
                    MIAO_THREAD_PRIORITY, errno);
        }
    }

    /* Per-iteration buffers. Static so we don't blow the thread stack
     * if MIAO_PT_NUM_PER_FRM grows. */
    static int16_t spu_buf[MIAO_PT_NUM_PER_FRM * MIAO_CHANNELS];
    static int16_t adx_buf[MIAO_PT_NUM_PER_FRM * MIAO_CHANNELS];
    static int16_t mix_buf[MIAO_PT_NUM_PER_FRM * MIAO_CHANNELS];

    while (!atomic_load_explicit(&s_miao.stop, memory_order_acquire)) {
        const int frames = (int)MIAO_PT_NUM_PER_FRM;

        /* SPU: generate into spu_buf. SPU_GenerateInto handles the
         * soundLock internally and ticks timer_cb every 192 samples. */
        SPU_GenerateInto(spu_buf, frames);

        /* ADX: pop from ring; zero-fill any underrun. */
        int got = miao_adx_ring_pop(adx_buf, frames);
        if (got < frames) {
            memset(&adx_buf[got * MIAO_CHANNELS], 0,
                   (size_t)(frames - got) * MIAO_CHANNELS * sizeof(int16_t));
            atomic_fetch_add_explicit(&s_miao.adx_underruns,
                                      (unsigned)(frames - got),
                                      memory_order_relaxed);
        }

        int gain_q14 = atomic_load_explicit(&s_miao.adx_gain_q14,
                                            memory_order_relaxed);
        miao_mix(spu_buf, adx_buf, mix_buf, frames, gain_q14);

        /* Submit to MI_AO. */
        MI_AUDIO_Frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.eBitwidth  = E_MI_AUDIO_BIT_WIDTH_16;
        frame.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
        frame.apVirAddr[0] = mix_buf;
        frame.apVirAddr[1] = NULL;
        /* u32Len: total interleaved bytes. The header comment says
         * "data lenth per channel in frame" but the working
         * steward-fu impl passes the total interleaved byte count
         * — keep that contract; first-frame log will confirm. */
        frame.u32Len = (MI_U32)(frames * MIAO_CHANNELS * (int)sizeof(int16_t));

        /* Timeout=-1: block until the device buffer has space for our
         * frame. Replaces the old (timeout=1ms + nanosleep) self-pacing
         * scheme, which had only ~500us of safety margin against
         * processing/send overhead and produced continuous mid-stream
         * glitches when the audio thread slipped behind realtime. With
         * blocking semantics, MI_AO paces us at exactly device speed. */
        MI_S32 rc = MI_AO_SendFrame(0, 0, &frame, -1);

        if (!s_miao.first_frame_logged) {
            double init_to_first_ms = miao_elapsed_ms(&s_miao.init_wall);
            fprintf(stderr,
                    "[MIAO] first MI_AO_SendFrame: rc=0x%08x u32Len=%u "
                    "frames=%d init_to_first_ms=%.3f\n",
                    (unsigned)rc, (unsigned)frame.u32Len, frames,
                    init_to_first_ms);
            s_miao.first_frame_logged = true;
        }

        if (rc != 0) {
            /* MI_AO_ERR_BUF_FULL is the expected back-pressure signal.
             * Anything else is real. We rate-limit logs to first-error
             * + once-per-5s thereafter (mirrors miao_log_underruns_if_due)
             * so a low-rate intermittent error isn't silently swallowed
             * for hundreds of iterations before the next log fires. */
            static struct timespec last_err_log = {0, 0};
            static MI_S32 last_err_rc = 0;
            static unsigned err_count_since_log = 0;
            err_count_since_log++;
            struct timespec err_now;
            clock_gettime(CLOCK_MONOTONIC, &err_now);
            bool first_or_new_rc =
                (last_err_log.tv_sec == 0) || (rc != last_err_rc);
            bool due = (err_now.tv_sec - last_err_log.tv_sec) >= 5;
            if (first_or_new_rc || due) {
                fprintf(stderr,
                        "[MIAO] MI_AO_SendFrame rc=0x%08x (count=%u since last log)\n",
                        (unsigned)rc, err_count_since_log);
                last_err_log = err_now;
                last_err_rc = rc;
                err_count_since_log = 0;
            }
        }

        /* No explicit pacing sleep here — MI_AO_SendFrame above blocks
         * until the device buffer has space, which paces this loop
         * naturally at exactly the playback rate. */

        unsigned underruns = atomic_load_explicit(&s_miao.adx_underruns,
                                                  memory_order_relaxed);
        miao_log_underruns_if_due(underruns);
    }

    return NULL;
}

/* ---------------------------------------------------------------- */
/* Signal handlers (best-effort exit cleanliness)                   */
/* ---------------------------------------------------------------- */

static void miao_signal_trampoline(int sig) {
    /* Best-effort: tear down MI_AO so /dev/MI_AO is released for the
     * next launch. Do NOT join the thread (signal-handler context),
     * just disable the channel/device synchronously.
     *
     * Strictly speaking these MI_AO_* calls are not async-signal-safe
     * — the vendor library may take internal mutexes — but the
     * alternative is leaving the device wedged across SIGTERM/SIGINT
     * so next launch hits EBUSY. We accept the unsafety, gate on a
     * sig_atomic_t flag so we only do it once even if the prev
     * handler chains back to us, and keep the body minimal. */
    static volatile sig_atomic_t miao_disabled_by_signal = 0;
    if (!miao_disabled_by_signal) {
        miao_disabled_by_signal = 1;
        MI_AO_DisableChn(0, 0);
        MI_AO_Disable(0);
    }

    /* Chain to whatever was registered before us. If there was none,
     * the default disposition for SIGINT/SIGTERM is to terminate.
     *
     * Two callback shapes can be installed via sigaction:
     *   - sa_handler   (legacy void(int)) when SA_SIGINFO is unset,
     *   - sa_sigaction (void(int, siginfo_t*, void*)) when SA_SIGINFO
     *     is set.
     * SDL3 happens to install sa_sigaction-form handlers, so chaining
     * only sa_handler-form would skip SDL3's exit logic and fall
     * through to SIG_DFL, terminating the process before SDL3 gets
     * to run its cleanup. We pass NULL/NULL for the siginfo and
     * ucontext args because we don't have them in this trampoline
     * context — the chained handler should not deref them blindly,
     * but the major chainees (SDL3, Rust panic handlers) tolerate
     * this. */
    struct sigaction *prev = NULL;
    if (sig == SIGINT)  prev = &s_miao.prev_sigint;
    if (sig == SIGTERM) prev = &s_miao.prev_sigterm;
    if (prev) {
        if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction) {
            prev->sa_sigaction(sig, NULL, NULL);
            return;
        }
        if (prev->sa_handler &&
            prev->sa_handler != SIG_DFL &&
            prev->sa_handler != SIG_IGN) {
            prev->sa_handler(sig);
            return;
        }
    }
    /* Fall through to default termination. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void miao_install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = miao_signal_trampoline;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT,  &sa, &s_miao.prev_sigint)  != 0) return;
    if (sigaction(SIGTERM, &sa, &s_miao.prev_sigterm) != 0) {
        /* roll back SIGINT to what it was */
        sigaction(SIGINT, &s_miao.prev_sigint, NULL);
        return;
    }
    s_miao.signals_installed = true;
}

static void miao_uninstall_signal_handlers(void) {
    if (!s_miao.signals_installed) {
        return;
    }
    sigaction(SIGINT,  &s_miao.prev_sigint,  NULL);
    sigaction(SIGTERM, &s_miao.prev_sigterm, NULL);
    s_miao.signals_installed = false;
}

static void miao_atexit(void) {
    MIAO_Shutdown();
}

/* ---------------------------------------------------------------- */
/* Public init / shutdown                                           */
/* ---------------------------------------------------------------- */

bool MIAO_IsActive(void) {
    return atomic_load_explicit(&s_miao.active, memory_order_acquire);
}

bool MIAO_ForceSDLFallback(void) {
    /* Runtime A/B toggle. Centralised here so ADX_Init and SPU_Init
     * (the two entry points that bring up the audio stack) cannot
     * drift on the env-var contract. */
    const char *force_sdl = getenv("THIRDSARM_AUDIO_SDL");
    return force_sdl != NULL && force_sdl[0] == '1';
}

void MIAO_RegisterTimerCB(void (*cb)(void)) {
    atomic_store_explicit(&s_miao.timer_cb, cb, memory_order_release);
}

/* Used by SPU_GenerateInto to fire the 250 Hz tick. Lives here so the
 * audio thread doesn't have to expose its private state. */
void MIAO_FireTimerCB(void);
void MIAO_FireTimerCB(void) {
    void (*cb)(void) = atomic_load_explicit(&s_miao.timer_cb, memory_order_acquire);
    if (cb) {
        cb();
    }
}

void MIAO_SetADXGain(float gain) {
    if (gain < 0.0f) gain = 0.0f;
    int q14 = (int)(gain * 16384.0f + 0.5f);
    atomic_store_explicit(&s_miao.adx_gain_q14, q14, memory_order_relaxed);
}

bool MIAO_Init(void) {
    if (atomic_load_explicit(&s_miao.active, memory_order_acquire)) {
        return true;
    }

    /* SPU's voices array, active mask, and soundLock must be live
     * before the audio thread calls SPU_GenerateInto. The engine
     * boot order calls ADX_Init (where we initialise MIAO) BEFORE
     * SPU_Init / cseInitSndDrv, so SPU's own constructor hasn't
     * run yet. SPU_PreInit is idempotent — the later SPU_Init's
     * call is a no-op. */
    SPU_PreInit();

    clock_gettime(CLOCK_MONOTONIC, &s_miao.init_wall);

    /* MI_SYS_Init is required before any MI_* call. The MI_GFX
     * presenter at src/platform/app/arm/arm_display_mi_gfx.c also
     * calls MI_SYS_Init() at startup; the SDK is reference-counted
     * (calls return success when already initialised) so calling it
     * here too is safe. We do it explicitly because audio init may
     * run before display init in some boot orderings. */
    {
        MI_S32 rc = MI_SYS_Init();
        if (rc != 0) {
            fprintf(stderr, "[MIAO] MI_SYS_Init failed: rc=0x%08x\n",
                    (unsigned)rc);
            /* Continue anyway — if display already inited it returns
             * non-zero on Miyoo BSPs; MI_AO calls below will fail
             * loud if MI_SYS truly isn't ready. */
        } else {
            fprintf(stderr, "[MIAO] MI_SYS_Init rc=0x%08x\n", (unsigned)rc);
        }
    }

    /* Allocate the SPSC ring. */
    s_miao.adx_ring = (int16_t *)calloc(MIAO_ADX_RING_FRAMES * MIAO_CHANNELS,
                                        sizeof(int16_t));
    if (s_miao.adx_ring == NULL) {
        fprintf(stderr, "[MIAO] failed to allocate ADX ring\n");
        return false;
    }
    atomic_store_explicit(&s_miao.adx_head, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_miao.adx_tail, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_miao.adx_underruns, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_miao.adx_overruns,  0u, memory_order_relaxed);

    /* Configure MI_AO public attributes. Reset first so a stale
     * configuration from a prior process can't interfere. */
    MI_AO_ClrPubAttr(0);

    MI_AUDIO_Attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.eSamplerate    = E_MI_AUDIO_SAMPLE_RATE_48000;
    attr.eBitwidth      = E_MI_AUDIO_BIT_WIDTH_16;
    attr.eWorkmode      = E_MI_AUDIO_MODE_I2S_MASTER;
    attr.eSoundmode     = E_MI_AUDIO_SOUND_MODE_STEREO;
    attr.u32FrmNum      = MIAO_FRM_NUM;
    attr.u32PtNumPerFrm = MIAO_PT_NUM_PER_FRM;
    attr.u32ChnCnt      = 2;

    {
        MI_S32 rc = MI_AO_SetPubAttr(0, &attr);
        if (rc != 0) {
            fprintf(stderr, "[MIAO] MI_AO_SetPubAttr failed: rc=0x%08x\n",
                    (unsigned)rc);
            free(s_miao.adx_ring);
            s_miao.adx_ring = NULL;
            return false;
        }
    }

    /* Diagnostic: read back the programmed attributes. */
    {
        MI_AUDIO_Attr_t got;
        memset(&got, 0, sizeof(got));
        MI_S32 rc = MI_AO_GetPubAttr(0, &got);
        fprintf(stderr,
                "[MIAO] MI_AO_GetPubAttr rc=0x%08x rate=%d bitw=%d mode=%d "
                "sndmode=%d frm=%u pt=%u chn=%u\n",
                (unsigned)rc,
                (int)got.eSamplerate, (int)got.eBitwidth,
                (int)got.eWorkmode, (int)got.eSoundmode,
                (unsigned)got.u32FrmNum,
                (unsigned)got.u32PtNumPerFrm,
                (unsigned)got.u32ChnCnt);
    }

    {
        MI_S32 rc = MI_AO_Enable(0);
        if (rc != 0) {
            fprintf(stderr, "[MIAO] MI_AO_Enable failed: rc=0x%08x\n",
                    (unsigned)rc);
            free(s_miao.adx_ring);
            s_miao.adx_ring = NULL;
            return false;
        }
    }

    {
        MI_S32 rc = MI_AO_EnableChn(0, 0);
        if (rc != 0) {
            fprintf(stderr, "[MIAO] MI_AO_EnableChn failed: rc=0x%08x\n",
                    (unsigned)rc);
            MI_AO_Disable(0);
            free(s_miao.adx_ring);
            s_miao.adx_ring = NULL;
            return false;
        }
    }

    /* 0 dB. MI_AO_SetVolume takes dB on this BSP, and 0 means unity. */
    {
        MI_S32 rc = MI_AO_SetVolume(0, 0);
        if (rc != 0) {
            fprintf(stderr, "[MIAO] MI_AO_SetVolume(0) rc=0x%08x (non-fatal)\n",
                    (unsigned)rc);
        }
    }

    /* Mark active before launching the thread so the producer
     * (game thread calling MIAO_PushADX) doesn't drop frames in the
     * tiny window between thread creation and the first send. */
    atomic_store_explicit(&s_miao.stop, false, memory_order_release);
    atomic_store_explicit(&s_miao.active, true, memory_order_release);

    if (pthread_create(&s_miao.thread, NULL, miao_audio_thread, NULL) != 0) {
        fprintf(stderr, "[MIAO] pthread_create failed: errno=%d\n", errno);
        atomic_store_explicit(&s_miao.active, false, memory_order_release);
        MI_AO_DisableChn(0, 0);
        MI_AO_Disable(0);
        free(s_miao.adx_ring);
        s_miao.adx_ring = NULL;
        return false;
    }
    s_miao.thread_started = true;

    /* Hook clean-up paths. Best-effort — still works if any of these
     * fail. */
    miao_install_signal_handlers();
    atexit(miao_atexit);

    fprintf(stderr,
            "[MIAO] MIAO_Init OK rate=%u chn=%u pt=%u frm=%u ring_frames=%u\n",
            (unsigned)MIAO_SAMPLE_RATE, (unsigned)MIAO_CHANNELS,
            (unsigned)MIAO_PT_NUM_PER_FRM, (unsigned)MIAO_FRM_NUM,
            (unsigned)MIAO_ADX_RING_FRAMES);

    return true;
}

void MIAO_Shutdown(void) {
    if (!atomic_load_explicit(&s_miao.active, memory_order_acquire)) {
        return;
    }
    atomic_store_explicit(&s_miao.active, false, memory_order_release);
    atomic_store_explicit(&s_miao.stop, true, memory_order_release);

    if (s_miao.thread_started) {
        pthread_join(s_miao.thread, NULL);
        s_miao.thread_started = false;
    }

    MI_AO_DisableChn(0, 0);
    MI_AO_Disable(0);

    miao_uninstall_signal_handlers();

    if (s_miao.adx_ring) {
        free(s_miao.adx_ring);
        s_miao.adx_ring = NULL;
    }
    fprintf(stderr, "[MIAO] MIAO_Shutdown done\n");
}

#else  /* !PORT_MIYOO_MINI_PLUS */

/* Stub TU on non-Miyoo profiles. The header is still includable but
 * every entry point is a no-op so callers can be unconditional. */

#include <stdbool.h>
#include <stdint.h>

bool MIAO_Init(void)                                  { return false; }
void MIAO_Shutdown(void)                              { }
bool MIAO_IsActive(void)                              { return false; }
void MIAO_RegisterTimerCB(void (*cb)(void))           { (void)cb; }
void MIAO_PushADX(const int16_t *p, int n)            { (void)p; (void)n; }
int  MIAO_QueuedFramesADX(void)                       { return 0; }
void MIAO_ClearADX(void)                              { }
void MIAO_SetADXGain(float gain)                      { (void)gain; }
bool MIAO_ForceSDLFallback(void)                      { return false; }

#endif /* PORT_MIYOO_MINI_PLUS */
