#include "port/sound/spu.h"

#include "common.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

// Clock recovery: dynamic rate control to prevent ARM/FPGA drift
#define SPU_CR_TARGET_BYTES (2048 * 2 * sizeof(s16))  // ~42.7ms at 48kHz stereo S16
#define SPU_CR_MAX_DELTA    0.005                       // 0.5% max pitch adjustment
#define SPU_CR_UPDATE_INTERVAL 192                      // Update every 192 callbacks (~4ms each = ~768ms)
                                                        // Smooths out per-callback jitter
static int cr_update_counter = 0;

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
    v->decLeft -= decInc;

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

    header = ram[v->nax & ~0x7];
    if ((header >> 10) & 1) {
        v->lsa = v->nax;
    }

    v->nax = (v->nax + 1) & 0xfffff;
}

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
        int queued = SDL_GetAudioStreamQueued(stream_cb);
        if (queued >= 0) {
            double fill_level = (double)queued / (double)SPU_CR_TARGET_BYTES;
            if (fill_level > 1.0) fill_level = 1.0;
            double ratio = (1.0 - SPU_CR_MAX_DELTA) + 2.0 * fill_level * SPU_CR_MAX_DELTA;
            // Clamp to [1 - max_delta, 1 + max_delta]
            if (ratio < 1.0 - SPU_CR_MAX_DELTA) ratio = 1.0 - SPU_CR_MAX_DELTA;
            if (ratio > 1.0 + SPU_CR_MAX_DELTA) ratio = 1.0 + SPU_CR_MAX_DELTA;
            SDL_SetAudioStreamFrequencyRatio(stream_cb, (float)ratio);
        }
    }
}

static void nullcb() {}

void SPU_Init(void (*cb)()) {
    SDL_AudioSpec spec;

    timer_cb = cb;
    if (!cb) {
        timer_cb = nullcb;
    }

    memset(voices, 0, sizeof(voices));
    active_voices = 0;
    soundLock = SDL_CreateMutex();

    spec.channels = 2;
    spec.format = SDL_AUDIO_S16;
    spec.freq = 48000;

    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, SPU_SDL_CB, NULL);
    if (!stream) {
        SDL_Log("Couldn't create SDL audio stream: %s", SDL_GetError());
        return;
    }

    SDL_ResumeAudioStreamDevice(stream);
    cr_update_counter = 0;
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
