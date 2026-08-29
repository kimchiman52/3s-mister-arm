/**
 * @file texcash.c
 * Texture Cache Manager
 */

#include "sf33rd/Source/Game/rendering/texcash.h"
#include "common.h"
/* ENABLE_PERF_TELEMETRY lives in the generated port/build_config.h. Without
 * it every `#if ENABLE_PERF_TELEMETRY` in this TU silently evaluates to 0
 * (there is no -Wundef in C_FLAGS), so the skip+log diagnostics below would
 * compile away. ramcnt.c and texgroup.c pick it up transitively via main.h;
 * texcash.c does not include main.h, so take it directly. */
#include "port/build_config.h"
#include "sf33rd/AcrSDK/ps2/flps2debug.h"
#include "sf33rd/AcrSDK/ps2/foundaps2.h" /* flLogOut */
#include "sf33rd/Source/Common/PPGFile.h"
#include "sf33rd/Source/Game/debug/Debug.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/rendering/aboutspr.h"
#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/ramcnt.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "structs.h"
#include "mts_hash.h"

#include <SDL3/SDL.h>

typedef struct {
    s32 p16;
    s32 p32;
    s32 gix;
    s16 life16;
    s16 life32;
    s16 type;
    u16 mode;
    u32 attribute;
} MTSBase;

s8* texcash_name[29] = { "    16x16 (tm)  32x32 (tm) GIX      (mn.nw)",
                         "QA",
                         "HT",
                         "1P",
                         "2P",
                         "CA",
                         "SW",
                         "OB",
                         "ED",
                         "DM",
                         "OT",
                         "MJ",
                         "MS",
                         "SL",
                         "EU",
                         "RR",
                         "GI",
                         "xx",
                         "xx",
                         "xx",
                         "xx",
                         "xx",
                         "xx",
                         "xx",
                         "   /   (  )    /   (  )     +    (  .  )",
                         "--- --- --  --- --- --  ---   --  -- --",
                         "--- --- --",
                         "--",
                         "--.--" };

u8* texcash_melt_buffer;
TexturePoolUsed* tpu_free;

#ifdef ENABLE_NETPLAY_TESTS
/* [tasks #59/#61] Test-only observability: how many times a brick-prevention
 * guard in this TU has fired.  --test-texcash-bounds reads it to prove the
 * guard actually engaged rather than the test merely walking a happy path.
 * Deliberately absent from every shipping and gate build so it adds no
 * file-scope state to the rollback-determinism memory image. */
unsigned texcash_guard_hits;
#define TEXCASH_GUARD_HIT() (texcash_guard_hits += 1)
#else
#define TEXCASH_GUARD_HIT() ((void)0)
#endif
s16 mts_ob_curr_stage;

// forward decls
extern const s16 mts_OB_page[22][2];
extern const MTSBase mts_base[24];
void clear_texcash_work(s16 ix);

void disp_texcash_free_area() {
    s16 i;

    if (Debug_w[11]) {
        flPrintColor(0xFF8F8F8F);

        for (i = 0; i < 24; i++) {
            flPrintL(13, i + 8, texcash_name[i]);
            if (i) {
                flPrintL(16, i + 8, texcash_name[24]);
            }
        }

        flPrintColor(0xFFCFCFCF);
        for (i = 1; i < 24; i++) {
            if (mts_ok[i].be) {
                if (mts[i].mltnum16 != 0) {
                    flPrintL(16, i + 8, "%3X", mts_ok[i].min16);
                    flPrintL(20, i + 8, "%3X", mts[i].mltnum16);

                    if (mts[i].mltcshtime16 != 0) {
                        flPrintL(24, i + 8, "%2d", mts[i].mltcshtime16);
                    } else {
                        flPrintL(24, i + 8, texcash_name[27]);
                    }
                } else {
                    flPrintL(16, i + 8, texcash_name[26]);
                }

                if (mts[i].mltnum32 != 0) {
                    flPrintL(28, i + 8, "%3X", mts_ok[i].min32);
                    flPrintL(32, i + 8, "%3X", mts[i].mltnum32);

                    if (mts[i].mltcshtime32 != 0) {
                        flPrintL(36, i + 8, "%2d", mts[i].mltcshtime32);
                    } else {
                        flPrintL(36, i + 8, texcash_name[27]);
                    }
                } else {
                    flPrintL(28, i + 8, texcash_name[26]);
                }

                flPrintL(40, i + 8, "%3X", mts[i].mltgidx16);
                flPrintL(46, i + 8, "%2X", mts[i].mltnum);

                if ((i == 7) && (mts_ob_curr_stage != bg_w.stage)) {
                    flPrintColor(0xFFFF8F8F);
                    flPrintL(11, i + 8, "!?");
                    flPrintColor(0xFFCFCFCF);
                }

                if (mts[i].ext) {
                    flPrintL(50, i + 8, "%2d", 64 - mts_ok[i].mincg);
                    flPrintL(53, i + 8, "%2d", 64 - mts[i].cpat->kazu);
                } else {
                    flPrintL(50, i + 8, texcash_name[28]);
                }
            } else {
                flPrintL(16, i + 8, texcash_name[25]);
            }
        }
    }
}

void search_texcash_free_area(s16 ix) {
    PatternState* mc;
    s16 i;
    s16 num = 0;

    for (mc = mts[ix].mltcsh16, i = 0; i < mts[ix].mltnum16; i++) {
        if (mc[i].cs.code == -1) {
            num++;
        }
    }

    if (num < mts_ok[ix].min16) {
        mts_ok[ix].min16 = num;
    }

    num = 0;
    for (mc = mts[ix].mltcsh32, i = 0; i < mts[ix].mltnum32; i++) {
        if (mc[i].cs.code == -1) {
            num++;
        }
    }

    if (num < mts_ok[ix].min32) {
        mts_ok[ix].min32 = num;
    }

    if ((mts[ix].ext) && (mts[ix].cpat->kazu > mts_ok[ix].mincg)) {
        mts_ok[ix].mincg = mts[ix].cpat->kazu;
    }
}

void init_texcash_1st() {
    s16 i;

    for (i = 0; i < MULTITEXTURE_MAX; i++) {
        mts_ok[i].be = 0;
        mts_ok[i].mincg = 0;
        mts_ok[i].min16 = 0x7FFF;
        mts_ok[i].min32 = 0x7FFF;
        mts_ok[i].key0 = 0;
        mts_ok[i].key1 = 0;
        mts[i].mode = -1;
    }
}

void init_texcash_before_process() {
    s16 i;

    for (i = 1; i < 24; i++) {
        if ((mts_ok[i].be) && (mts[i].ext)) {
            init_texcash_2nd(i);
        }
    }
}

void init_texcash_2nd(s16 ix) {
    PatternState* mc;
    PatternCollection* cp;
    TexturePoolFree* tf;
    TexturePoolUsed* tu;
    s16 i;

    tf = mts[ix].tpf;
    tu = mts[ix].tpu;
    cp = mts[ix].cpat;
    tf->x32 = 0;
    tf->x16 = 0;
    tu->x32 = 0;
    tu->x16 = 0;
    mc = mts[ix].mltcsh16;

    for (i = mts[ix].mltnum16 - 1; i >= 0; i--) {
        if (mc[i].cs.code == -1) {
            tf->x16_free[tf->x16] = i;
            tf->x16 += 1;
        } else {
            tu->x16_used[tu->x16] = i;
            tu->x16 += 1;
        }
    }

    mc = mts[ix].mltcsh32;

    for (i = mts[ix].mltnum32 - 1; i >= 0; i--) {
        if (mc[i].cs.code == -1) {
            tf->x32_free[tf->x32] = i;
            tf->x32 += 1;
        } else {
            tu->x32_used[tu->x32] = i;
            tu->x32 += 1;
        }
    }

    if (mts[ix].hash16) {
        PatternState* mc16 = mts[ix].mltcsh16;
        mts_hash_clear(mts[ix].hash16);
        mts[ix].free16.top = -1;
        for (i = 0; i < mts[ix].mltnum16; i++) {
            if (mc16[i].cs.code == (u32)-1) {
                mts_freelist_push(&mts[ix].free16, (u16)i);
            } else {
                mts_hash_insert(mts[ix].hash16, mc16[i].cs.code,
                                (u32)(u16)mc16[i].state, (u16)i);
            }
        }
    }
    if (mts[ix].hash32) {
        PatternState* mc32 = mts[ix].mltcsh32;
        mts_hash_clear(mts[ix].hash32);
        mts[ix].free32.top = -1;
        for (i = 0; i < mts[ix].mltnum32; i++) {
            if (mc32[i].cs.code == (u32)-1) {
                mts_freelist_push(&mts[ix].free32, (u16)i);
            } else {
                mts_hash_insert(mts[ix].hash32, mc32[i].cs.code,
                                (u32)(u16)mc32[i].state, (u16)i);
            }
        }
    }

    cp->kazu = 0;

    for (i = 0; i < 0x40; i++) {
        if (cp->patt[i].time) {
            cp->adr[cp->kazu] = &cp->patt[i];
            cp->kazu += 1;
        }
    }
}

/* Give back every x16/x32 slot a just-expired PatternInstance holds.
 *
 * Split out of texture_cash_update() so the --test-texcash-bounds harness
 * can drive the MAPPING MISS guard directly; `num` and `i` are carried only
 * so the diagnostic can name the cache and the live-list position. */
void texcash_release_instance(MultiTexture* mt, PatternInstance* cp, s16 num, s16 i) {
    makeup_tpu_free(mt->mltnum16 / 256, mt->mltnum32 / 64, &cp->map);

    if ((tpu_free->x16 != cp->x16) || (tpu_free->x32 != cp->x32)) {
        Debug_w[11] = 1;
        // The arcade source hung here (do{...}while(1)); log + resync and carry on with the release.
        //
        // [task #61] What the never-returning loop owned: the *release* of
        // this instance's slots.  It fires after the instance's `time` has
        // already been decremented to 0 and before update_with_tpu_free()
        // has handed a single refcount back, so a guard that returned here
        // would strand every slot the instance holds -- mc[slot].time never
        // decremented, cs.code never set to -1, the slot never returned to
        // tpf by init_texcash_2nd.  That is exactly the shape of the
        // 2026-04-29 texgroup.c:216 guard that turned an arcade hang into a
        // leak, so this guard must not skip the release.
        //
        // It resyncs the redundant counters instead.  `cp->map` is the
        // authoritative record of what was acquired: mc[slot].time is
        // incremented exactly when x16_mapping_set/x32_mapping_set flips a
        // bit 0->1 (mtrans.c, get_mltbuf{16,32}_ext_2), so popcount(map) is
        // precisely the number of decrements this instance owes, and
        // makeup_tpu_free() above enumerates precisely that popcount.
        // cp->x16 / cp->x32 are a parallel tally maintained alongside the
        // map and read nowhere but this comparison -- when the two disagree
        // it is the tally that is wrong.  Restoring it from the map leaves
        // the instance self-consistent for search_texcash_free_area() and
        // for its own next expiry.  --test-texcash-bounds SUB_D asserts the
        // release actually happened, which is what forbids the texgroup
        // shape from creeping back in.
#if ENABLE_PERF_TELEMETRY
        flLogOut("[texcash-skip] %s mapping-miss num=%d i=%d map16=%d cp16=%d map32=%d cp32=%d (resynced from map; "
                 "release proceeds)\n",
                 __func__, (int)num, (int)i, (int)tpu_free->x16, (int)cp->x16, (int)tpu_free->x32, (int)cp->x32);
#endif
        TEXCASH_GUARD_HIT();
        cp->x16 = tpu_free->x16;
        cp->x32 = tpu_free->x32;
    }

    update_with_tpu_free(mt);
}

void texture_cash_update() {
    s16 i;
    s16 num;

    for (num = 0; num < 24; num++) {
        if (mts_ok[num].be != 0) {
            if (mts[num].ext) {
                for (i = 0; i < mts[num].cpat->kazu; i++) {
                    if ((--mts[num].cpat->adr[i]->time) == 0) {
                        texcash_release_instance(&mts[num], mts[num].cpat->adr[i], num, i);
                    }
                }
            } else {
                if ((mts[num].mltcshtime16 + mts[num].mltcshtime32) != 0) {
                    mlt_obj_trans_update(&mts[num]);
                }
            }

            if (Debug_w[11]) {
                search_texcash_free_area(num);
            }
        }
    }
    disp_texcash_free_area();
}

void update_with_tpu_free(MultiTexture* mt) {
    PatternState* mc16 = mt->mltcsh16;
    PatternState* mc32 = mt->mltcsh32;
    s16 i;
    s16 slot;

    for (i = 0; i < tpu_free->x16; i++) {
        slot = tpu_free->x16_used[i];
        mc16[slot].time -= 1;
        if (mc16[slot].time < 0) {
            Debug_w[11] = 1;
            // The arcade source hung here (do{...}while(1)); log + clamp and carry on.
            //
            // [task #59] What the never-returning loop owned: the invariant
            // `time >= 0 for every slot` -- it asserted it and then refused
            // to continue.  So the guard has to restore it, not just step
            // over it.  Clamping to 0 is the only coherent choice: 0 is what
            // "fully released" means everywhere else in this cache (a slot
            // reaches tpf only via cs.code == -1, and cs.code is set to -1
            // only right below, at time <= 0), and the fall-through below
            // then completes that release normally.
            //
            // Leaving the value negative would re-arm the same trap one
            // generation later.  A slot's next allocation is either
            // `mc[slot].time = 1` (x16, mtrans.c get_mltbuf16_ext_2) which
            // would paper over it, or `mc[slot].time += 1` (x32,
            // get_mltbuf32_ext_2) which would not -- the slot would come
            // back from the free list still negative and trap again on its
            // next release.  The clamp makes both paths safe and removes the
            // dependence on that x16/x32 asymmetry.
            //
            // Debug_w[11] is still set, so texture_cash_update() keeps
            // calling search_texcash_free_area() and the on-screen free-area
            // report still shows the damage.
#if ENABLE_PERF_TELEMETRY
            flLogOut("[texcash-skip] %s x16-refcount-underflow slot=%d time=%d gidx=%d (clamped to 0)\n",
                     __func__, (int)slot, (int)mc16[slot].time, (int)mt->mltgidx16);
#endif
            TEXCASH_GUARD_HIT();
            mc16[slot].time = 0;
        }

        if (mc16[slot].time <= 0) {
            if (mt->hash16) {
                mts_hash_remove(mt->hash16, mc16[slot].cs.code,
                                (u32)(u16)mc16[slot].state, mc16);
            }
            mc16[slot].cs.code = -1;
        }
    }

    for (i = 0; i < tpu_free->x32; i++) {
        slot = tpu_free->x32_used[i];
        mc32[slot].time -= 1;
        if (mc32[slot].time < 0) {
            Debug_w[11] = 1;
            // [task #59] Same guard as the x16 loop above; see the rationale there.
            // This is the arm that actually needs the clamp: the x32 realloc
            // path increments (`mc[slot].time += 1`) rather than assigning,
            // so a slot left negative here would still be negative when it
            // is handed out again.
#if ENABLE_PERF_TELEMETRY
            flLogOut("[texcash-skip] %s x32-refcount-underflow slot=%d time=%d gidx=%d (clamped to 0)\n",
                     __func__, (int)slot, (int)mc32[slot].time, (int)mt->mltgidx32);
#endif
            TEXCASH_GUARD_HIT();
            mc32[slot].time = 0;
        }

        if (mc32[slot].time <= 0) {
            if (mt->hash32) {
                mts_hash_remove(mt->hash32, mc32[slot].cs.code,
                                (u32)(u16)mc32[slot].state, mc32);
            }
            mc32[slot].cs.code = -1;
        }
    }
}

s16 get_my_trans_mode(s16 curr) {
    if (mts_ok[curr].be == 0) {
        return -1;
    }

    return mts[curr].mode;
}

void make_texcash_work(s16 ix) {
    size_t memreq;
    u8* adrs;
    // For some reason page16 is reused later as a pointer.
    // That's why it's uintptr_t and not u32 like page32.
    // I guess the devs were too lazy to make another var or something.
    uintptr_t page16;
    u32 page32;
    u16 bc16, bc32;

    if (mts_ok[ix].be) {
        if ((Test_ramcnt_key(mts_ok[ix].key0) != 0) && (Test_ramcnt_key(mts_ok[ix].key1) != 0)) {
            return;
        }

        Debug_w[10] = 2;

        while (1) {
            disp_ramcnt_free_area();
            flPrintL(5, 30, "TEXCASH KEY ERROR");
            njWaitVSync_with_N();
        }
    } else {
        if (ix == 7) {
            page16 = mts_OB_page[bg_w.stage][0];
            page32 = mts_OB_page[bg_w.stage][1];
            mts_ob_curr_stage = bg_w.stage;
        } else {
            page16 = mts_base[ix].p16;
            page32 = mts_base[ix].p32;
        }

        mts[ix].mltnum16 = (u32)page16 << 8;
        mts[ix].mltnum32 = page32 << 6;
        mts[ix].mltnum = (u32)page16 + page32;
        mts[ix].mltgidx16 = mts_base[ix].gix;
        mts[ix].mltgidx32 = (u32)page16 + mts_base[ix].gix;
        mts[ix].mltcshtime16 = mts_base[ix].life16;
        mts[ix].mltcshtime32 = mts_base[ix].life32;

        if ((mts[ix].ext = ((mts_base[ix].mode & 0x2000) != 0))) {
            bc16 = mts_hash_bucket_count((u32)mts[ix].mltnum16);
            bc32 = mts_hash_bucket_count((u32)mts[ix].mltnum32);
            memreq = (mts[ix].mltnum16 * 8) + (mts[ix].mltnum32 * 8) + sizeof(PatternCollection) +
                     sizeof(TexturePoolFree) + sizeof(TexturePoolUsed) +
                     sizeof(MtsCacheIndex) * 2 +
                     (bc16 * sizeof(u16)) +
                     (bc32 * sizeof(u16)) +
                     (mts[ix].mltnum16 * sizeof(u16)) +
                     (mts[ix].mltnum32 * sizeof(u16));
            mts_ok[ix].key0 = Pull_ramcnt_key(memreq, mts_base[ix].type, 0, 0);
            adrs = (u8*)Get_ramcnt_address(mts_ok[ix].key0);

            /* Pull_ramcnt_key returns -1 and Get_ramcnt_address(<= 0)
             * returns 0 when the ramcnt pool or key table is exhausted
             * (ramcnt.c:205/237 and :151-160 -- both already skip+log
             * rather than the arcade ERR_STOP). Bail out here instead of
             * carrying the sentinel into pointer arithmetic: leaving
             * mts_ok[ix].be at 0 degrades to a missing texture.
             *
             * MOST consumers tolerate that -- aboutspr.c:265,
             * texcash.c:338/439/720 and PPGWork.c:177 all skip on
             * be == 0. It is not universal: opening.c:223-224 calls
             * make_texcash_work(9) and then mlt_obj_melt2 with no be
             * check, and mlt_obj_melt2 gates on grplds->ok (the texture
             * group) rather than on be. That path needs the very first
             * texcash allocation at boot to fail before it matters, which
             * is not demonstrated reachable; it is called out here so the
             * bail-out is not mistaken for a universally safe degrade. */
            if (mts_ok[ix].key0 < 0 || adrs == NULL) {
#if ENABLE_PERF_TELEMETRY
                flLogOut("[ramcnt-skip] %s key0-alloc-failed ix=%d key0=%d memreq=%zu (texcash slot left uninitialised)\n",
                         __func__, (int)ix, (int)mts_ok[ix].key0, memreq);
#endif
                mts_ok[ix].key0 = 0;
                return;
            }

            mts[ix].mltcsh16 = (PatternState*)adrs;
            adrs += mts[ix].mltnum16 * 8;
            mts[ix].mltcsh32 = (PatternState*)adrs;
            adrs += mts[ix].mltnum32 * 8;
            mts[ix].cpat = (PatternCollection*)adrs;
            adrs += sizeof(PatternCollection);
            mts[ix].tpf = (TexturePoolFree*)adrs;
            adrs += sizeof(TexturePoolFree);
            mts[ix].tpu = (TexturePoolUsed*)adrs;
            adrs += sizeof(TexturePoolUsed);
            mts[ix].hash16 = (MtsCacheIndex*)adrs;
            adrs += sizeof(MtsCacheIndex);
            mts[ix].hash32 = (MtsCacheIndex*)adrs;
            adrs += sizeof(MtsCacheIndex);
            mts[ix].hash16->buckets = (u16*)adrs;
            mts[ix].hash16->bucket_count = bc16;
            mts[ix].hash16->bucket_mask = bc16 - 1;
            adrs += bc16 * sizeof(u16);
            mts[ix].hash32->buckets = (u16*)adrs;
            mts[ix].hash32->bucket_count = bc32;
            mts[ix].hash32->bucket_mask = bc32 - 1;
            adrs += bc32 * sizeof(u16);
            mts[ix].free16.slots = (u16*)adrs;
            adrs += mts[ix].mltnum16 * sizeof(u16);
            mts[ix].free32.slots = (u16*)adrs;
            /* adrs += mts[ix].mltnum32 * sizeof(u16); -- not needed, last item */
            mts_hash_clear(mts[ix].hash16);
            mts_hash_clear(mts[ix].hash32);
            mts[ix].free16.top = -1;
            mts[ix].free32.top = -1;
            SDL_zerop(mts[ix].cpat);
            SDL_zerop(mts[ix].tpf);
            SDL_zerop(mts[ix].tpu);
            init_texcash_2nd(ix);
        } else {
            bc16 = mts_hash_bucket_count((u32)mts[ix].mltnum16);
            bc32 = mts_hash_bucket_count((u32)mts[ix].mltnum32);
            memreq = mts[ix].mltnum16 * 8 + mts[ix].mltnum32 * 8 +
                     sizeof(MtsCacheIndex) * 2 +
                     (bc16 * sizeof(u16)) +
                     (bc32 * sizeof(u16)) +
                     (mts[ix].mltnum16 * sizeof(u16)) +
                     (mts[ix].mltnum32 * sizeof(u16));
            mts_ok[ix].key0 = Pull_ramcnt_key(memreq, mts_base[ix].type, 0, 0);
            adrs = (u8*)Get_ramcnt_address(mts_ok[ix].key0);

            /* Same exhausted-pool bail-out as the ext branch above. */
            if (mts_ok[ix].key0 < 0 || adrs == NULL) {
#if ENABLE_PERF_TELEMETRY
                flLogOut("[ramcnt-skip] %s key0-alloc-failed ix=%d key0=%d memreq=%zu (texcash slot left uninitialised)\n",
                         __func__, (int)ix, (int)mts_ok[ix].key0, memreq);
#endif
                mts_ok[ix].key0 = 0;
                return;
            }

            mts[ix].mltcsh16 = (PatternState*)adrs;
            adrs += mts[ix].mltnum16 * 8;
            mts[ix].mltcsh32 = (PatternState*)adrs;
            adrs += mts[ix].mltnum32 * 8;
            mts[ix].hash16 = (MtsCacheIndex*)adrs;
            adrs += sizeof(MtsCacheIndex);
            mts[ix].hash32 = (MtsCacheIndex*)adrs;
            adrs += sizeof(MtsCacheIndex);
            mts[ix].hash16->buckets = (u16*)adrs;
            mts[ix].hash16->bucket_count = bc16;
            mts[ix].hash16->bucket_mask = bc16 - 1;
            adrs += bc16 * sizeof(u16);
            mts[ix].hash32->buckets = (u16*)adrs;
            mts[ix].hash32->bucket_count = bc32;
            mts[ix].hash32->bucket_mask = bc32 - 1;
            adrs += bc32 * sizeof(u16);
            mts[ix].free16.slots = (u16*)adrs;
            adrs += mts[ix].mltnum16 * sizeof(u16);
            mts[ix].free32.slots = (u16*)adrs;
            /* adrs += mts[ix].mltnum32 * sizeof(u16); -- not needed, last item */
            mts_hash_clear(mts[ix].hash16);
            mts_hash_clear(mts[ix].hash32);
            mts[ix].free16.top = -1;
            mts[ix].free32.top = -1;
        }

        mts[ix].mltbuf = texcash_melt_buffer;
        memreq = ((mts_base[ix].mode & 4) != 0) + 1;
        memreq *= (mts[ix].mltnum << 0x10);
        mts_ok[ix].key1 = Pull_ramcnt_key(memreq, mts_base[ix].type, 0, 0);
        mts[ix].attribute = mts_base[ix].attribute;
        page16 = Get_ramcnt_address(mts_ok[ix].key1);

        /* This is the site that segfaults when the ramcnt pool has been
         * leaked away: page16 == 0 flows into mlt_obj_trans_init ->
         * ppgSetupTexChunkSeqs, which writes adrs[i] = 0 at
         * PPGFile.c:1014. Bail out and hand key0 back rather than leaving
         * it stranded; the caller-visible result is an uninitialised
         * texcash slot (be stays 0), not a crash.
         *
         * The release has to be type-dispatched. ramcnt has two mutually
         * exclusive gates: Push_ramcnt_key rejects type 8/9 (ramcnt.c:57)
         * and Push_ramcnt_key_original rejects everything else
         * (ramcnt.c:77). key0 carries mts_base[ix].type, and that is NOT
         * uniformly 8/9 -- mts_base[0].type is 0 (the table is 1x0, 15x8,
         * 8x9). ix == 0 is not reached today (no make_texcash_work call site
         * passes 0 -- literals are 8/9/12/13/14/16, and TM_num[i] is gated
         * on mto_list[..][i] whose column 0 is zero in all nine rows,
         * mmtmcnt.c:41-49/:86-93), so an unconditional
         * Push_ramcnt_key_original would be a latent trap rather than a
         * live bug -- it would silently refuse the free and leak key0 with
         * its handle already zeroed. Dispatch on the type instead so the
         * bail-out is correct for every index, including ones a future
         * caller might add. purge_texcash_work (texcash.c:719) is the
         * normal release path and can hardcode _original precisely because
         * it only ever runs for slots that reached be != 0. */
        if (mts_ok[ix].key1 < 0 || page16 == 0) {
#if ENABLE_PERF_TELEMETRY
            flLogOut("[ramcnt-skip] %s key1-alloc-failed ix=%d key1=%d memreq=%zu (texcash slot left uninitialised)\n",
                     __func__, (int)ix, (int)mts_ok[ix].key1, memreq);
#endif
            mts_ok[ix].key1 = 0;

            if (mts_ok[ix].key0 > 0) {
                if (mts_base[ix].type == 8 || mts_base[ix].type == 9) {
                    Push_ramcnt_key_original(mts_ok[ix].key0);
                } else {
                    Push_ramcnt_key(mts_ok[ix].key0);
                }

                mts_ok[ix].key0 = 0;
            }

            return;
        }

        mlt_obj_trans_init(&mts[ix], mts_base[ix].mode, (u8*)page16);

        if (mts[ix].ext) {
            init_texcash_2nd(ix);
        }

        mts[ix].id = ix;
        mts[ix].mode = mts_base[ix].mode & 0xFF;
        mts_ok[ix].be = 1;
        mts_ok[ix].mincg = 0;
        mts_ok[ix].min16 = 0x7FFF;
        mts_ok[ix].min32 = 0x7FFF;
    }
}

void Clear_texcash_work() {
    s16 i;

    for (i = 1; i < 24; i++) {
        clear_texcash_work(i);
    }
}

void clear_texcash_work(s16 ix) {
    s16 i;

    if (((mts_ok[ix].be) != 0) && ((mts_base[ix].mode & 0x20) == 0)) {
        for (i = 0; i < mts[ix].mltnum16; i++) {
            mts[ix].mltcsh16[i].time = 0;
            mts[ix].mltcsh16[i].cs.code = -1;
        }

        for (i = 0; i < mts[ix].mltnum32; i++) {
            mts[ix].mltcsh32[i].time = 0;
            mts[ix].mltcsh32[i].cs.code = -1;
        }

        if (mts[ix].hash16) {
            mts_hash_clear(mts[ix].hash16);
            mts[ix].free16.top = mts[ix].mltnum16 - 1;
            for (i = 0; i < mts[ix].mltnum16; i++)
                mts[ix].free16.slots[i] = (u16)(mts[ix].mltnum16 - 1 - i);
        }
        if (mts[ix].hash32) {
            mts_hash_clear(mts[ix].hash32);
            mts[ix].free32.top = mts[ix].mltnum32 - 1;
            for (i = 0; i < mts[ix].mltnum32; i++)
                mts[ix].free32.slots[i] = (u16)(mts[ix].mltnum32 - 1 - i);
        }

        if (mts[ix].ext) {
            SDL_zerop(mts[ix].cpat);
            SDL_zerop(mts[ix].tpf);
            SDL_zerop(mts[ix].tpu);
            init_texcash_2nd(ix);
        }

        mts_ok[ix].mincg = 0;
        mts_ok[ix].min16 = 0x7FFF;
        mts_ok[ix].min32 = 0x7FFF;
    }
}

void purge_texcash_work(s16 ix) {
    if (mts_ok[ix].be == 0) {
        return;
    }

    if ((Test_ramcnt_key(mts_ok[ix].key0) != 0) && (Test_ramcnt_key(mts_ok[ix].key1) != 0)) {
        Push_ramcnt_key_original(mts_ok[ix].key0);
        Push_ramcnt_key_original(mts_ok[ix].key1);
    } else {
        Debug_w[10] = 2;

        while (1) {
            disp_ramcnt_free_area();
            flPrintL(5, 30, "TEXCASH KEY ERROR");
            njWaitVSync_with_N();
        }
    }

    ppgReleaseTextureHandle(&mts[ix].tex, -1);
    mts_ok[ix].be = 0;
    mts_ok[ix].key0 = 0;
    mts_ok[ix].key1 = 0;
}

const MTSBase mts_base[24] = {
    { .p16 = 0, .p32 = 0, .gix = 0, .life16 = 0, .life32 = 0, .type = 0, .mode = 0, .attribute = 0 },
    { .p16 = 1, .p32 = 1, .gix = 20, .life16 = 0, .life32 = 0, .type = 8, .mode = 4114, .attribute = 1 },
    { .p16 = 2, .p32 = 4, .gix = 30, .life16 = 8, .life32 = 8, .type = 8, .mode = 4113, .attribute = 1 },
    { .p16 = 3, .p32 = 6, .gix = 40, .life16 = 20, .life32 = 20, .type = 9, .mode = 8209, .attribute = 1 },
    { .p16 = 3, .p32 = 6, .gix = 50, .life16 = 20, .life32 = 20, .type = 9, .mode = 8209, .attribute = 1 },
    { .p16 = 1, .p32 = 4, .gix = 60, .life16 = 2, .life32 = 2, .type = 9, .mode = 8210, .attribute = 1 },
    { .p16 = 1, .p32 = 5, .gix = 70, .life16 = 0, .life32 = 0, .type = 8, .mode = 4113, .attribute = 1 },
    { .p16 = 1, .p32 = 1, .gix = 80, .life16 = 12, .life32 = 12, .type = 9, .mode = 8210, .attribute = 1 },
    { .p16 = 2, .p32 = 8, .gix = 1200, .life16 = 16, .life32 = 16, .type = 9, .mode = 4114, .attribute = 1 },
    { .p16 = 4, .p32 = 34, .gix = 500, .life16 = 20, .life32 = 20, .type = 9, .mode = 4129, .attribute = 1 },
    { .p16 = 4, .p32 = 8, .gix = 80, .life16 = 4, .life32 = 4, .type = 9, .mode = 4113, .attribute = 1 },
    { .p16 = 1, .p32 = 2, .gix = 1000, .life16 = 0, .life32 = 0, .type = 8, .mode = 4113, .attribute = 1 },
    { .p16 = 1, .p32 = 0, .gix = 1020, .life16 = 10, .life32 = 10, .type = 9, .mode = 4114, .attribute = 1 },
    { .p16 = 1, .p32 = 6, .gix = 1030, .life16 = 2, .life32 = 2, .type = 8, .mode = 8210, .attribute = 1 },
    { .p16 = 2, .p32 = 6, .gix = 1100, .life16 = 4, .life32 = 4, .type = 8, .mode = 8210, .attribute = 1 },
    { .p16 = 1, .p32 = 2, .gix = 1120, .life16 = 2, .life32 = 2, .type = 8, .mode = 4113, .attribute = 1 },
    { .p16 = 1, .p32 = 2, .gix = 960, .life16 = 2, .life32 = 2, .type = 8, .mode = 4113, .attribute = 1 },
    { .p16 = 1, .p32 = 1, .gix = 1140, .life16 = 2, .life32 = 2, .type = 8, .mode = 4114, .attribute = 1 },
    { .p16 = 1, .p32 = 1, .gix = 1100, .life16 = 0, .life32 = 0, .type = 8, .mode = 4116, .attribute = 1 },
    { .p16 = 1, .p32 = 1, .gix = 1100, .life16 = 0, .life32 = 0, .type = 8, .mode = 4116, .attribute = 1 },
    { .p16 = 1, .p32 = 1, .gix = 1100, .life16 = 0, .life32 = 0, .type = 8, .mode = 4116, .attribute = 1 },
    { .p16 = 1, .p32 = 1, .gix = 1100, .life16 = 0, .life32 = 0, .type = 8, .mode = 4116, .attribute = 1 },
    { .p16 = 1, .p32 = 1, .gix = 1100, .life16 = 0, .life32 = 0, .type = 8, .mode = 4116, .attribute = 1 },
    { .p16 = 1, .p32 = 1, .gix = 1100, .life16 = 0, .life32 = 0, .type = 8, .mode = 4116, .attribute = 1 }
};

const s16 mts_OB_page[22][2] = { { 1, 1 }, { 1, 3 }, { 1, 2 }, { 1, 1 }, { 1, 2 }, { 1, 1 }, { 1, 2 }, { 1, 2 },
                                 { 1, 2 }, { 1, 1 }, { 1, 1 }, { 1, 2 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 2 },
                                 { 1, 2 }, { 1, 4 }, { 1, 2 }, { 1, 4 }, { 1, 1 }, { 1, 2 } };
