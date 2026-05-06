#include "port/sound/spu.h"

#include "common.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(PORT_MISTER)
#include <sched.h>
#endif

#if defined(PORT_MIYOO_MINI_PLUS)
#include "port/sound/mi_ao_backend.h"
/* The MI_AO backend's audio thread invokes the registered 250 Hz tick
 * callback through this trampoline, called from inside SPU_GenerateInto
 * every 192 samples. Kept private to mi_ao_backend.c via a forward
 * declaration here. */
extern void MIAO_FireTimerCB(void);
#endif

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define clamp(val, min, max) (((val) > (max)) ? (max) : (((val) < (min)) ? (min) : (val)))

#define VOICE_COUNT 48

#include "interp_table.inc"

enum {
    ADSR_PHASE_ATTACK,
    ADSR_PHASE_DECAY,
    ADSR_PHASE_SUSTAIN,
    ADSR_PHASE_RELEASE,
    ADSR_PHASE_STOPPED,
};

struct AdsrParamCache {
    bool decr;
    bool exp;
    u8 shift;
    s8 step;
    s32 target;
    bool infinite;
};

struct SPU_Voice {
    bool run;
    bool noise;
    bool endx;
    s16 decodeHist[2];
    u32 counter;
    u16 pitch;
    u16* sample;
    u32 ssa;
    u32 nax;
    u32 lsa;
    bool customLoop;

    s32 envx;
    s32 voll, volr;

    u16 adsr1, adsr2;

    u8 adsr_phase;
    u32 adsr_counter;
    struct AdsrParamCache adsr_param;

    s16 decodeBuf[0x40];
    u32 decRPos, decWPos, decLeft;
};

SDL_Mutex* soundLock;

static void (*timer_cb)();
static SDL_AudioStream* stream;
static struct SPU_Voice voices[VOICE_COUNT];
static u16 ram[(2 * 1024 * 1024) >> 1];
static s16 adpcm_coefs[5][2] = {
    { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 },
};
static u64 active_voices = 0;


static int SPU_Ctz64(u64 mask) {
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_ctzll(mask);
#else
    int bit = 0;
    while ((mask & 1) == 0) {
        mask >>= 1;
        bit++;
    }
    return bit;
#endif
}

static s16 SPU_ApplyVolume(s16 sample, s32 volume) {
    return (sample * volume) >> 15;
}

static void SPU_VoiceCacheADSR(struct SPU_Voice* v) {
    struct AdsrParamCache* pc = &v->adsr_param;

    switch (v->adsr_phase) {
    case ADSR_PHASE_ATTACK:
        pc->decr = false;
        pc->exp = ((v->adsr1 & 0x8000) != 0);
        pc->shift = (v->adsr1 >> 10) & 0x1f;
        pc->step = 7 - ((v->adsr1 >> 8) & 0x3);
        pc->target = 0x7fff;
        pc->infinite = ((v->adsr1 >> 8) & 0x7f) == 0x7f;
        break;
    case ADSR_PHASE_DECAY:
        pc->decr = true;
        pc->exp = true;
        pc->shift = (v->adsr1 >> 4) & 0xf;
        pc->step = -8;
        pc->target = ((v->adsr1 & 0xf) + 1) << 11;
        pc->infinite = ((v->adsr1 >> 4) & 0xf) == 0xf;
        break;
    case ADSR_PHASE_SUSTAIN:
        pc->decr = ((v->adsr2 & 0x4000) != 0);
        pc->exp = ((v->adsr2 & 0x8000) != 0);
        pc->shift = (v->adsr2 >> 8) & 0x1f;
        pc->step = 7 - ((v->adsr2 >> 6) & 0x3);
        pc->target = 0;
        pc->infinite = ((v->adsr2 >> 6) & 0x7f) == 0x7f;

        if (pc->decr) {
            pc->step = ~v->adsr_param.step;
        }
        break;
    case ADSR_PHASE_RELEASE:
        pc->decr = true;
        pc->exp = ((v->adsr2 & 0x20) != 0);
        pc->shift = v->adsr2 & 0x1f;
        pc->step = -8;
        pc->target = 0;
        pc->infinite = (v->adsr2 & 0x1f) == 0x1f;
        break;
    }
}

static void SPU_VoiceRunADSR(struct SPU_Voice* v) {
    struct AdsrParamCache* pc = &v->adsr_param;
    u32 counter_inc = 0x8000 >> max(0, pc->shift - 11);
    s32 level_inc = pc->step << max(0, 11 - pc->shift);

    if (pc->exp && !pc->decr && v->envx >= 0x6000) {
        if (pc->shift < 10) {
            level_inc >>= 2;
        } else if (pc->shift >= 11) {
            counter_inc >>= 2;
        } else {
            counter_inc >>= 1;
            level_inc >>= 1;
        }
    } else if (pc->exp && pc->decr) {
        level_inc = (level_inc * v->envx) >> 15;
    }

    if (!pc->infinite) {
        counter_inc = max(counter_inc, 1);
    }
    v->adsr_counter += counter_inc;

    if (v->adsr_counter & 0x8000) {
        v->adsr_counter = 0;
        v->envx = clamp(v->envx + level_inc, 0, INT16_MAX);
    }

    if (v->adsr_phase == ADSR_PHASE_SUSTAIN) {
        return;
    }

    if ((!pc->decr && v->envx >= pc->target) || ((pc->decr && v->envx <= pc->target))) {
        v->adsr_phase++;
        SPU_VoiceCacheADSR(v);
    }

    if (v->adsr_phase > ADSR_PHASE_RELEASE) {
        v->run = false;
        active_voices &= ~((u64)1 << (u64)(v - voices));
    }
}

static void SPU_VoiceDecode(struct SPU_Voice* v) {
    u32 data;
    u16 header, filter, shift;

    if (v->decLeft >= 16) {
        return;
    }

    data = ram[v->nax];
    header = ram[v->nax & ~0x7];
    shift = header & 0xf;
    filter = (header >> 4) & 7;
    if (filter > 4) filter = 0;

    for (int i = 0; i < 4; i++) {
        s32 sample = (s16)((data & 0xF) << 12);
        sample >>= shift;

        // TODO do the right thing for invalid shift/filter values
        sample += (adpcm_coefs[filter][0] * v->decodeHist[0]) >> 6;
        sample += (adpcm_coefs[filter][1] * v->decodeHist[1]) >> 6;

        // We do get overflow here otherwise, should we?
        sample = clamp(sample, INT16_MIN, INT16_MAX);

        v->decodeHist[1] = v->decodeHist[0];
        v->decodeHist[0] = (s16)sample;

        v->decodeBuf[v->decWPos] = sample;
        v->decodeBuf[v->decWPos | 0x20] = sample;

        v->decWPos = (v->decWPos + 1) & 0x1f;
        v->decLeft++;

        data >>= 4;
    }

    v->nax = (v->nax + 1) & 0xfffff;

    if ((v->nax & 0x7) == 0) {
        if (header & 0x100) {
            v->nax = v->lsa;
            v->endx = true;

            if ((header & 0x200) == 0) {
                if (!v->noise) {
                    v->envx = 0;
                    v->adsr_phase = ADSR_PHASE_STOPPED;
                    v->run = false;
                    active_voices &= ~((u64)1 << (u64)(v - voices));
                }
            }
        }

        header = ram[v->nax & ~0x7];
        if (header & 0x400) {
            v->lsa = v->nax;
        }

        v->nax = (v->nax + 1) & 0xfffff;
    }
}

static void SPU_VoiceTick(struct SPU_Voice* v, s32* output) {
    s32 sample, pitchStep, decInc;
    u32 index;

    SPU_VoiceDecode(v);

    index = (v->counter & 0x0ff0) >> 4;

    sample = 0;
    sample += ((v->decodeBuf[v->decRPos + 0] * interp_table[index][0]) >> 15);
    sample += ((v->decodeBuf[v->decRPos + 1] * interp_table[index][1]) >> 15);
    sample += ((v->decodeBuf[v->decRPos + 2] * interp_table[index][2]) >> 15);
    sample += ((v->decodeBuf[v->decRPos + 3] * interp_table[index][3]) >> 15);

    pitchStep = v->pitch;
    // TODO pitch mod?
    pitchStep = min(pitchStep, 0x3fff);
    v->counter += pitchStep;

    decInc = v->counter >> 12;
    v->counter &= 0xfff;
    v->decRPos = (v->decRPos + decInc) & 0x1f;
    v->decLeft = (decInc > v->decLeft) ? 0 : v->decLeft - decInc;

    sample = SPU_ApplyVolume(sample, v->envx);
    output[0] = SPU_ApplyVolume(sample, v->voll);
    output[1] = SPU_ApplyVolume(sample, v->volr);

    SPU_VoiceRunADSR(v);
}

bool SPU_VoiceIsFinished(int vnum) {
    if (voices[vnum].envx == 0 && voices[vnum].adsr_phase != ADSR_PHASE_ATTACK) {
        return true;
    }

    return false;
}

void SPU_VoiceKeyOff(int vnum) {
    if (voices[vnum].adsr_phase < ADSR_PHASE_RELEASE) {
        voices[vnum].adsr_phase = ADSR_PHASE_RELEASE;
        SPU_VoiceCacheADSR(&voices[vnum]);
    }
}

void SPU_VoiceStop(int vnum) {
    voices[vnum].envx = 0;
    voices[vnum].adsr_phase = ADSR_PHASE_STOPPED;
    voices[vnum].run = false;
    active_voices &= ~((u64)1 << (u64)vnum);
}

void SPU_VoiceGetConf(int vnum, struct SPUVConf* conf) {
    struct SPU_Voice* v = &voices[vnum];

    conf->pitch = v->pitch;
    conf->voll = v->voll;
    conf->volr = v->volr;
    conf->adsr1 = v->adsr1;
    conf->adsr2 = v->adsr2;
}

void SPU_VoiceSetConf(int vnum, struct SPUVConf* conf) {
    struct SPU_Voice* v = &voices[vnum];

    v->pitch = conf->pitch;
    v->voll = conf->voll << 1;
    v->volr = conf->volr << 1;
    v->adsr1 = conf->adsr1;
    v->adsr2 = conf->adsr2;
}

void SPU_VoiceStart(int vnum, u32 start_addr) {
    struct SPU_Voice* v = &voices[vnum];
    u16 header;

    v->ssa = start_addr;
    v->lsa = start_addr;
    v->nax = v->ssa;
    v->run = true;
    active_voices |= ((u64)1 << (u64)vnum);
    v->envx = 0;

    v->adsr_counter = 0;
    v->adsr_phase = ADSR_PHASE_ATTACK;
    SPU_VoiceCacheADSR(v);

    v->decodeHist[0] = 0;
    v->decodeHist[1] = 0;
    v->counter = 0;
    v->decRPos = 0;
    v->decWPos = 0;
    v->decLeft = 0;
    v->endx = false;
    memset(v->decodeBuf, 0, sizeof(v->decodeBuf));

    header = ram[v->nax & ~0x7];
    if ((header >> 10) & 1) {
        v->lsa = v->nax;
    }

    v->nax = (v->nax + 1) & 0xfffff;
}

/* Generate `frames` interleaved-stereo s16 samples into `out`. Honours
 * the soundLock contract used by emlShim and ticks the registered
 * 250 Hz callback every 192 samples. Used by both the SDL3 audio
 * callback (legacy / Mac / fallback) and the direct MI_AO audio thread
 * on PORT_MIYOO_MINI_PLUS. */
void SPU_GenerateInto(s16* out, int frames) {
    static int cb_timer = 192;

    int remaining = frames;
    s16* p = out;

    while (remaining > 0) {
        // Keep audio lock hold time bounded so gameplay-thread calls that take
        // soundLock (voice setup/key-off) do not stall for long stretches.
        int batch_count = remaining < 256 ? remaining : 256;

        SDL_LockMutex(soundLock);

        for (int i = 0; i < batch_count; i++) {
            SPU_Tick(p);
            p += 2;

            cb_timer--;
            if (!cb_timer) {
#if defined(PORT_MIYOO_MINI_PLUS)
                /* Under the direct MI_AO backend the registered cb is
                 * dispatched through MIAO_FireTimerCB so the audio
                 * thread doesn't have to know about timer_cb's storage.
                 * The backend's MIAO_RegisterTimerCB sets the same
                 * function pointer that nullcb / SPU_Init's caller
                 * provided, so this is a wash on the SDL3 path. */
                MIAO_FireTimerCB();
#else
                timer_cb();
#endif
                cb_timer = 192;
            }
        }

        SDL_UnlockMutex(soundLock);
        remaining -= batch_count;
    }
}

void SPU_SDL_CB(void* user, SDL_AudioStream* stream_cb, int additional_amount, int total_amount) {
#if defined(PORT_MISTER)
    /* Boost audio thread to SCHED_FIFO priority 50 (above game thread's 49)
       so it can preempt the game to fill audio buffers and release soundLock.
       Without this, SCHED_FIFO on the game thread causes priority inversion. */
    static bool audio_thread_boosted = false;
    if (!audio_thread_boosted) {
        struct sched_param sp = { .sched_priority = 50 };
        sched_setscheduler(0, SCHED_FIFO, &sp);
        audio_thread_boosted = true;
    }
#endif

    u32 samples_per_channel = (additional_amount / sizeof(s16)) >> 1;
    static s16 outbuf[4096] = {};

    while (samples_per_channel) {
        u32 batch_count = min(samples_per_channel, 256);

        SPU_GenerateInto(outbuf, (int)batch_count);
        SDL_PutAudioStreamData(stream_cb, outbuf, (batch_count * sizeof(s16)) << 1);
        samples_per_channel -= batch_count;
    }
}

static void nullcb() {}

/* Initialise the bits of SPU state that the audio thread needs in
 * order to call SPU_Tick safely: the voices array, the active mask,
 * and the sound lock. Idempotent. Called at the top of SPU_Init and
 * also (under PORT_MIYOO_MINI_PLUS) from MIAO_Init so the MI_AO
 * audio thread can safely run before SPU_Init's own init code under
 * the boot ordering where ADX_Init runs first.
 *
 * Does NOT install timer_cb — that one is set from SPU_Init proper
 * once the engine knows what callback to use. The audio thread
 * tolerates timer_cb == nullcb (default initialiser pointed below).
 *
 * Thread-safety contract: this function is called only from the boot
 * thread, before MIAO_Init starts the audio thread. The static `done`
 * guard is therefore safe without a mutex — no other thread is alive
 * yet on either of the two real call paths (SPU_Init -> SPU_PreInit
 * before any thread spawn, or MIAO_Init -> SPU_PreInit before
 * pthread_create). Do not call from a thread other than the boot
 * thread or `done` becomes a data race. */
void SPU_PreInit(void) {
    static bool done = false;
    if (done) {
        return;
    }
    timer_cb = nullcb;
    memset(voices, 0, sizeof(voices));
    active_voices = 0;
    soundLock = SDL_CreateMutex();
    done = true;
}

void SPU_Init(void (*cb)()) {
    SPU_PreInit();
    timer_cb = cb;
    if (!cb) {
        timer_cb = nullcb;
    }

#if defined(PORT_MIYOO_MINI_PLUS)
    /* Direct MI_AO backend is the default on Miyoo Mini Plus / OnionOS.
     * The kernel /dev/dsp shim doesn't translate sample rate into MI_AO
     * (audio plays at the device default ~ half-speed) and SDL3+OSS
     * fights audioserver for the device — both broken. We bypass them
     * entirely.
     *
     * Set THIRDSARM_AUDIO_SDL=1 in the launch env to force the legacy
     * SDL3 path for A/B comparison. The check is centralised in
     * MIAO_ForceSDLFallback() so this and ADX_Init can't drift. */
    if (!MIAO_ForceSDLFallback()) {
        MIAO_RegisterTimerCB(timer_cb);
        if (MIAO_Init()) {
            SDL_Log("SPU: using direct MI_AO backend "
                    "(THIRDSARM_AUDIO_SDL unset)");
            return;
        }
        SDL_Log("SPU: MIAO_Init failed; falling back to SDL3 audio");
    } else {
        SDL_Log("SPU: THIRDSARM_AUDIO_SDL=1 — forcing SDL3 audio path");
    }
#endif

    SDL_AudioSpec spec;
    spec.channels = 2;
    spec.format = SDL_AUDIO_S16;
    spec.freq = 48000;

    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, SPU_SDL_CB, NULL);
    if (!stream) {
        SDL_Log("Couldn't create SDL audio stream: %s", SDL_GetError());
        return;
    }

    SDL_ResumeAudioStreamDevice(stream);

#if defined(PORT_MIYOO_MINI_PLUS)
    {
        SDL_AudioSpec src_spec, dst_spec;
        if (SDL_GetAudioStreamFormat(stream, &src_spec, &dst_spec)) {
            SDL_Log("SPU stream: src=%dHz/%dch/fmt=0x%x  dst=%dHz/%dch/fmt=0x%x",
                    src_spec.freq, src_spec.channels, (unsigned)src_spec.format,
                    dst_spec.freq, dst_spec.channels, (unsigned)dst_spec.format);
        }
    }
#endif
}

void SPU_Upload(u32 dst, void* src, u32 size) {
    SDL_LockMutex(soundLock);

    memcpy(&ram[dst >> 1], src, size);

    SDL_UnlockMutex(soundLock);
}

void SPU_Tick(s16* output) {
    s32 acc[2] = {};
    s32 vout[2] = {};
    u64 mask = active_voices;

    while (mask != 0) {
        const int i = SPU_Ctz64(mask);
        mask &= (mask - 1);

        struct SPU_Voice* v = &voices[i];
        if (!v->run) {
            continue;
        }

        SPU_VoiceTick(v, vout);
        acc[0] += vout[0];
        acc[1] += vout[1];
    }

    output[0] = clamp(acc[0], INT16_MIN, INT16_MAX);
    output[1] = clamp(acc[1], INT16_MIN, INT16_MAX);
}
