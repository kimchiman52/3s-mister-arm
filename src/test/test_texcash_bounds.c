/**
 * @file test_texcash_bounds.c
 * Brick-prevention harness for the ext texture cache -- tasks #59 and #61.
 *
 * Both defects are infinite loops. On MiSTer there is no watchdog, so either
 * one is a hard brick: the user power-cycles. This harness exists to prove
 * they are real, to prove they are the same defect (one is the root of the
 * other), and to give each shipped guard something that goes RED when the
 * guard is reverted.
 *
 * #61 -- src/sf33rd/Source/Game/rendering/mtrans.c, pre-fix :452-456 (and the
 *        two copies at :823-825, :1214-1216):
 *
 *            ix = get_free_patcash_index(mt->cpat);   // 0 when all 64 live
 *            cp = &mt->cpat->patt[ix];
 *            mt->cpat->adr[mt->cpat->kazu] = cp;      // no bound check
 *            mt->cpat->kazu += 1;
 *
 *        PatternCollection is { s16 kazu; PatternInstance* adr[64];
 *        PatternInstance patt[64]; } (include/structs.h:1516-1520), so adr[64]
 *        is &patt[0] to the byte -- SUB_A proves that at runtime rather than
 *        by arithmetic.
 *
 * #59 -- src/sf33rd/Source/Game/rendering/texcash.c, pre-fix :313-320 and
 *        :334-341: `mc[slot].time -= 1; if (mc[slot].time < 0) { ... do { ... }
 *        while (1); }`.
 *
 * The relationship between them is settled by SUB_E, and NOT in the direction
 * this task was briefed with. The brief's theory was that adr[64] aliasing
 * &patt[0] leaves a duplicate entry in the live list, whose double decrement
 * drives a slot refcount negative and lands on #59. It does not: the caller's
 * own next three statements overwrite the bytes adr[64] occupies, so the
 * duplicate never survives. What #61 actually produces is a wild pointer that
 * texture_cash_update() dereferences -- a crash, not a hang. #59's `time < 0`
 * is not reachable through #61, and no other path to it could be constructed;
 * see SUB_E's header comment and the task report for the full accounting.
 *
 * NEUTRALIZATION. Two kinds, both required (see docs/rollback-determinism-
 * harness.md:208-216 and src/test/ldreq_timing_trace.h:38-40 for the house
 * rule -- a green result that cannot be turned red on demand is not evidence):
 *
 *   1. Built in. SUB_A and SUB_E run a verbatim replica of the pre-fix code
 *      as a control. If the control fails to corrupt anything, the harness
 *      has no signal and exits 2 -- not 0.
 *   2. External. Physically reverting a guard in the source must turn a
 *      sub-test red. For #59 and the MAPPING MISS guard the pre-fix code
 *      never returns, so "red" is a timeout: run the reverted binary under
 *      `timeout 20` and require exit 124. That is the brick, observed.
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "structs.h"
#include "types.h"

int Texcash_Test_Bounds(void);

#ifndef ENABLE_NETPLAY_TESTS

int Texcash_Test_Bounds(void) {
    fprintf(stderr,
            "--test-texcash-bounds requires a build with -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS.\n");
    return 2;
}

#else

#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/rendering/texcash.h"

/* Deliberately a literal, not a count of anything the runner can also get
 * wrong. Matches tools/rendezvous-server/__test_protocol.js:2296-2299: if a
 * sub-test is skipped, removed, or aborts the runner, coverage fails loudly
 * instead of reporting a smaller green run. */
#define EXPECTED_SUBTESTS 5

static int g_fail;    /* a real defect survived */
static int g_nosignal; /* a control failed to reproduce the pre-fix damage */
static int g_ran;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            g_fail += 1;                                                                                               \
            printf("  FAIL  %s:%d  ", __func__, __LINE__);                                                             \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* A control assertion. Failing one means the experiment is dead, not that the
 * code under test is good, so it lands in a separate bucket that exits 2. */
#define CONTROL(cond, ...)                                                                                             \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            g_nosignal += 1;                                                                                           \
            printf("  NO-SIGNAL  %s:%d  ", __func__, __LINE__);                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* --------------------------------------------------------------------------
 * Fixtures
 * ----------------------------------------------------------------------- */

/* One x16 page and one x32 page, so makeup_tpu_free()'s page arguments
 * (mltnum16 / 256, mltnum32 / 64) are both 1 and every slot index used below
 * is inside PatternMap's x16_map[4][16] / x32_map[10][8]. */
#define T_X16_SLOTS 256
#define T_X32_SLOTS 64

static PatternState t_mc16[T_X16_SLOTS];
static PatternState t_mc32[T_X32_SLOTS];
static MultiTexture t_mt;
static TexturePoolUsed t_tpu_free;
static PatternCollection t_pc;
static PatternCollection t_pc_ctl;

static void mt_reset(void) {
    s32 i;

    memset(&t_mt, 0, sizeof t_mt);
    memset(t_mc16, 0, sizeof t_mc16);
    memset(t_mc32, 0, sizeof t_mc32);
    memset(&t_tpu_free, 0, sizeof t_tpu_free);

    /* mlt_obj_trans_init() leaves every slot at time == 0 / cs.code == -1
     * before the first init_texcash_2nd (mtrans.c:2335, slot loops at
     * :2369-2381). Start there. */
    for (i = 0; i < T_X16_SLOTS; i++) {
        t_mc16[i].cs.code = (u32)-1;
    }
    for (i = 0; i < T_X32_SLOTS; i++) {
        t_mc32[i].cs.code = (u32)-1;
    }

    t_mt.mltnum16 = T_X16_SLOTS;
    t_mt.mltnum32 = T_X32_SLOTS;
    t_mt.mltcsh16 = t_mc16;
    t_mt.mltcsh32 = t_mc32;
    /* mts_base[3]: ext (mode 8209 = 0x2011), life16 = life32 = 20,
     * gix = 40 (mts_base[3], texcash.c:747). */
    t_mt.mltcshtime16 = 20;
    t_mt.mltcshtime32 = 20;
    t_mt.mltgidx16 = 40;
    t_mt.mltgidx32 = 41;
    t_mt.hash16 = NULL; /* exercise the linear path; the guards are hash-agnostic */
    t_mt.hash32 = NULL;
    t_mt.ext = 1;
    t_mt.id = 3;

    tpu_free = &t_tpu_free;
}

/* Mark an x16 slot as held by `cp`, exactly the way get_mltbuf16_ext_2()'s
 * fresh-allocation arm does (mtrans.c: x16_mapping_set + cp->x16 += 1 +
 * mc[slot].time = 1). */
static void acquire_x16(PatternInstance* cp, s32 slot, u32 code) {
    u16 bit = (u16)(1 << (slot & 0xF));

    if (!(cp->map.x16_map[slot / 256][(slot % 256) / 16] & bit)) {
        cp->map.x16_map[slot / 256][(slot % 256) / 16] |= bit;
        cp->x16 += 1;
        t_mc16[slot].time += 1;
    }
    t_mc16[slot].cs.code = code;
    t_mc16[slot].state = 0;
}

static void acquire_x32(PatternInstance* cp, s32 slot, u32 code) {
    u8 bit = (u8)(1 << (slot & 7));

    if (!(cp->map.x32_map[slot / 64][(slot % 64) / 8] & bit)) {
        cp->map.x32_map[slot / 64][(slot % 64) / 8] |= bit;
        cp->x32 += 1;
        t_mc32[slot].time += 1;
    }
    t_mc32[slot].cs.code = code;
    t_mc32[slot].state = 0;
}

/* init_texcash_2nd() (texcash.c:210), live-list rebuild at texcash.c:275-282. */
static void rebuild_live_list(PatternCollection* pc) {
    s16 i;

    pc->kazu = 0;
    for (i = 0; i < 0x40; i++) {
        if (pc->patt[i].time) {
            pc->adr[pc->kazu] = &pc->patt[i];
            pc->kazu += 1;
        }
    }
}

/* --------------------------------------------------------------------------
 * The pre-fix code, kept verbatim as a control
 * ----------------------------------------------------------------------- */

/* mtrans.c:2093-2103 pre-fix. */
static s32 prefix_get_free_patcash_index(PatternCollection* padr) {
    s16 i;

    for (i = 0; i < 0x40; i++) {
        if (padr->patt[i].time == 0) {
            return i;
        }
    }

    return 0; /* cache buffer full -- reuse slot 0 */
}

/* `padr->adr[padr->kazu] = cp` with kazu == 64. Written through a byte
 * pointer rather than the array subscript so the control is well-defined C
 * (and survives a sanitizer build) -- the bytes deposited are identical, and
 * prefix_append_target() lets the caller assert at runtime that the address
 * really is &patt[0] rather than taking the struct arithmetic on trust. */
static unsigned char* prefix_append_target(PatternCollection* padr) {
    return (unsigned char*)padr + offsetof(PatternCollection, adr) +
           (size_t)padr->kazu * sizeof(PatternInstance*);
}

/* Read back a pointer-sized field as the pointer it now holds. Takes a
 * const void* so clang's -Wsizeof-pointer-memaccess (which fires when the
 * source's type happens to equal the sizeof operand) stays out of the way. */
static PatternInstance* read_back_ptr(const void* src) {
    PatternInstance* out;

    memcpy(&out, src, sizeof out);
    return out;
}

static void prefix_append(PatternCollection* padr, PatternInstance* cp) {
    unsigned char* dst = prefix_append_target(padr);

    memcpy(dst, &cp, sizeof cp);
    padr->kazu += 1;
}

/* --------------------------------------------------------------------------
 * SUB_A -- #61: the append cannot reach adr[64], and the control proves
 *               adr[64] really is patt[0]
 * ----------------------------------------------------------------------- */

static void sub_a_append_bound(void) {
    PatternInstance* seen[0x40];
    PatternInstance snapshot;
    PatternInstance* cp;
    unsigned char* target;
    PatternInstance* clobber_read;
    s16 k;
    s16 j;

    printf("SUB_A  #61 bounded append / no live-instance reuse\n");
    g_ran += 1;

    memset(&t_pc, 0, sizeof t_pc);

    for (k = 0; k < 0x40; k++) {
        s16 free_count = 0;

        cp = patcash_acquire(&t_pc);
        CHECK(cp != NULL, "acquire %d returned NULL with %d instances still dead", (int)k, 0x40 - (int)k);
        if (cp == NULL) {
            return;
        }
        CHECK(cp->time == 0, "acquire %d handed back a LIVE instance (time=%d)", (int)k, (int)cp->time);
        CHECK(cp >= &t_pc.patt[0] && cp <= &t_pc.patt[0x3F], "acquire %d returned a pointer outside patt[]", (int)k);
        CHECK(t_pc.kazu == k + 1, "kazu=%d after acquire %d, expected %d", (int)t_pc.kazu, (int)k, (int)k + 1);
        CHECK(t_pc.adr[k] == cp, "adr[%d] does not point at the instance just acquired", (int)k);

        for (j = 0; j < k; j++) {
            CHECK(seen[j] != cp, "acquire %d handed back the same instance as acquire %d", (int)k, (int)j);
        }
        seen[k] = cp;

        cp->time = 20; /* the caller's `cp->time = mt->mltcshtime16` */

        /* The invariant the guard leans on: kazu + dead == 0x40 for the whole
         * frame, so "kazu is full" and "nothing is dead" are the same
         * condition. Checked at every step, not asserted in a comment. */
        for (j = 0; j < 0x40; j++) {
            if (t_pc.patt[j].time == 0) {
                free_count += 1;
            }
        }
        CHECK(t_pc.kazu + free_count == 0x40, "kazu(%d) + dead(%d) != 64 after acquire %d", (int)t_pc.kazu,
              (int)free_count, (int)k);
    }

    /* The 65th. */
    snapshot = t_pc.patt[0];
    cp = patcash_acquire(&t_pc);
    CHECK(cp == NULL, "65th acquire returned non-NULL on a full collection");
    CHECK(t_pc.kazu == 0x40, "65th acquire advanced kazu to %d", (int)t_pc.kazu);
    CHECK(memcmp(&snapshot, &t_pc.patt[0], sizeof snapshot) == 0, "65th acquire modified patt[0]");

    /* The bound arm's own red. The two refusal arms are redundant on every
     * state the collection actually reaches -- kazu + dead == 0x40 is checked
     * at every step above, so "kazu is full" and "nothing is dead" coincide,
     * and removing either arm alone leaves the other covering it. The one
     * state that separates them is kazu at the limit with instances still
     * dead, which is what an mltcshtime16 of 0 would accumulate: the caller's
     * `cp->time = mt->mltcshtime16` would leave every acquired instance dead
     * while kazu kept advancing. Construct it, because otherwise the bound
     * check is the one thing here that nothing can turn red. */
    t_pc.patt[9].time = 0;
    snapshot = t_pc.patt[0];
    cp = patcash_acquire(&t_pc);
    CHECK(cp == NULL, "acquire at kazu=64 with a dead instance present did not refuse");
    CHECK(t_pc.kazu == 0x40, "refused acquire advanced kazu to %d", (int)t_pc.kazu);
    CHECK(memcmp(&snapshot, &t_pc.patt[0], sizeof snapshot) == 0,
          "the append at kazu=64 wrote over patt[0]");

    /* ---- control: the pre-fix code, same starting state ---- */
    memset(&t_pc_ctl, 0, sizeof t_pc_ctl);
    for (k = 0; k < 0x40; k++) {
        s32 ix = prefix_get_free_patcash_index(&t_pc_ctl);
        PatternInstance* c = &t_pc_ctl.patt[ix];

        prefix_append(&t_pc_ctl, c);
        c->time = 20;
    }
    CONTROL(t_pc_ctl.kazu == 0x40, "control did not reach kazu == 64 (got %d)", (int)t_pc_ctl.kazu);

    /* Defect 1: the append target at kazu == 64 is patt[0] itself. */
    target = prefix_append_target(&t_pc_ctl);
    CONTROL(target == (unsigned char*)&t_pc_ctl.patt[0],
            "adr[64] is not &patt[0] on this ABI -- offset %ld vs %ld; the #61 overlap does not exist here",
            (long)(target - (unsigned char*)&t_pc_ctl), (long)((unsigned char*)&t_pc_ctl.patt[0] - (unsigned char*)&t_pc_ctl));

    /* Defect 2: exhaustion hands back a still-live instance. */
    {
        s32 ix = prefix_get_free_patcash_index(&t_pc_ctl);
        PatternInstance* c = &t_pc_ctl.patt[ix];

        CONTROL(ix == 0, "control: exhausted get_free_patcash_index returned %d, expected the 0 sentinel", (int)ix);
        CONTROL(c->time != 0, "control: patt[0] was not live, so defect 2 cannot be shown");

        prefix_append(&t_pc_ctl, c);
        CONTROL(t_pc_ctl.kazu == 0x41, "control: kazu did not advance past 64 (got %d)", (int)t_pc_ctl.kazu);

        /* patt[0].curr_disp / patt[0].time now hold the two halves of a
         * pointer. Read them back as the pointer they are. */
        clobber_read = read_back_ptr(&t_pc_ctl.patt[0]);
        CONTROL(clobber_read == c, "control: patt[0]'s leading bytes are not the appended pointer");
        CONTROL(t_pc_ctl.patt[0].time != 20,
                "control: patt[0].time survived the append as %d -- no corruption to detect",
                (int)t_pc_ctl.patt[0].time);
    }

    printf("       adr[64] == &patt[0] confirmed at runtime; guarded acquire refused the 65th\n");
}

/* --------------------------------------------------------------------------
 * SUB_B -- #61: refusing does not wedge the collection
 * ----------------------------------------------------------------------- */

static void sub_b_recovery(void) {
    PatternInstance* cp;
    s16 k;

    printf("SUB_B  #61 collection recovers once an instance expires\n");
    g_ran += 1;

    memset(&t_pc, 0, sizeof t_pc);
    for (k = 0; k < 0x40; k++) {
        cp = patcash_acquire(&t_pc);
        if (cp == NULL) {
            CHECK(0, "setup acquire %d failed", (int)k);
            return;
        }
        cp->time = 20;
    }
    CHECK(patcash_acquire(&t_pc) == NULL, "full collection did not refuse");

    /* texture_cash_update() expires patt[7]; init_texcash_2nd() rebuilds the
     * live list at the top of the next frame (texcash.c:275-282). */
    t_pc.patt[7].time = 0;
    rebuild_live_list(&t_pc);
    CHECK(t_pc.kazu == 0x3F, "rebuild gave kazu=%d, expected 63", (int)t_pc.kazu);

    cp = patcash_acquire(&t_pc);
    CHECK(cp == &t_pc.patt[7], "acquire after expiry did not reuse the freed instance");
    CHECK(t_pc.kazu == 0x40, "kazu=%d after recovery acquire, expected 64", (int)t_pc.kazu);

    /* The other arm of patcash_acquire's refusal -- "kazu has room but nothing
     * is dead" -- cannot be reached from any state this code produces. SUB_A
     * shows kazu + dead == 0x40 is maintained across a frame, and the only
     * thing that breaks it (an mltcshtime16 of 0, so an acquired instance
     * stays dead) drives kazu UP, never down, so the bound arm always fires
     * first. The arm exists so a future mts_base edit cannot make the
     * collection hand out a live instance. Constructed directly, because a
     * guard nothing can turn red is a guard nobody knows works. */
    for (k = 0; k < 0x40; k++) {
        t_pc.patt[k].time = 20;
    }
    t_pc.kazu = 10;
    CHECK(patcash_acquire(&t_pc) == NULL, "acquire handed back an instance with nothing dead (kazu=%d)",
          (int)t_pc.kazu);
    CHECK(t_pc.kazu == 10, "the refused acquire still advanced kazu to %d", (int)t_pc.kazu);

    printf("       refusal is one frame of missing sprite, not a permanent wedge\n");
}

/* --------------------------------------------------------------------------
 * SUB_C -- #59: the refcount underflow is contained
 * ----------------------------------------------------------------------- */

static void sub_c_underflow(void) {
    PatternInstance cp;
    unsigned hits0;

    printf("SUB_C  #59 x16/x32 refcount underflow clamps instead of hanging\n");
    g_ran += 1;

    mt_reset();
    memset(&cp, 0, sizeof cp);

    acquire_x16(&cp, 5, 0xA001);
    acquire_x16(&cp, 200, 0xA002);
    acquire_x32(&cp, 3, 0xB001);

    makeup_tpu_free(t_mt.mltnum16 / 256, t_mt.mltnum32 / 64, &cp.map);
    CHECK(tpu_free->x16 == 2, "makeup_tpu_free enumerated %d x16 bits, expected 2", (int)tpu_free->x16);
    CHECK(tpu_free->x32 == 1, "makeup_tpu_free enumerated %d x32 bits, expected 1", (int)tpu_free->x32);

    /* Release once: the normal, balanced path. */
    update_with_tpu_free(&t_mt);
    CHECK(t_mc16[5].time == 0 && t_mc16[5].cs.code == (u32)-1, "slot 5 not released cleanly");
    CHECK(t_mc16[200].time == 0 && t_mc16[200].cs.code == (u32)-1, "slot 200 not released cleanly");
    CHECK(t_mc32[3].time == 0 && t_mc32[3].cs.code == (u32)-1, "x32 slot 3 not released cleanly");

    /* Release the same map a second time. This is precisely what #61's
     * duplicated live-list entry produces one frame later (SUB_E derives it);
     * pre-fix, control never returns from here. */
    hits0 = texcash_guard_hits;
    update_with_tpu_free(&t_mt);

    CHECK(texcash_guard_hits == hits0 + 3, "guard fired %u times on the double release, expected 3",
          texcash_guard_hits - hits0);
    CHECK(t_mc16[5].time == 0, "slot 5 left at time=%d, expected the clamp to 0", (int)t_mc16[5].time);
    CHECK(t_mc16[200].time == 0, "slot 200 left at time=%d, expected the clamp to 0", (int)t_mc16[200].time);
    CHECK(t_mc32[3].time == 0, "x32 slot 3 left at time=%d, expected the clamp to 0", (int)t_mc32[3].time);

    /* Why the clamp and not a bare `continue`: the x32 realloc arm increments
     * rather than assigns (mtrans.c get_mltbuf32_ext_2), so a slot left at -1
     * would come back from the free list still negative and trap again. */
    t_mc32[3].time += 1;
    CHECK(t_mc32[3].time == 1, "x32 slot 3 reallocated to time=%d -- the trap would re-arm",
          (int)t_mc32[3].time);

    printf("       3 underflows contained, all slots left at the released value 0\n");
}

/* --------------------------------------------------------------------------
 * SUB_D -- MAPPING MISS resyncs and still performs the release
 * ----------------------------------------------------------------------- */

static void sub_d_mapping_miss(void) {
    PatternInstance cp;
    unsigned hits0;

    printf("SUB_D  MAPPING MISS resyncs from the map and completes the release\n");
    g_ran += 1;

    mt_reset();
    memset(&cp, 0, sizeof cp);

    acquire_x16(&cp, 9, 0xC001);
    acquire_x16(&cp, 10, 0xC002);
    acquire_x16(&cp, 11, 0xC003);
    acquire_x32(&cp, 2, 0xD001);
    CHECK(cp.x16 == 3 && cp.x32 == 1, "fixture built the wrong tally");

    /* Desynchronise the redundant tally, leaving the map (the authoritative
     * record) intact. This is the shape #61's SDL_zero-over-a-live-instance
     * produces. */
    cp.x16 = 2;
    cp.time = 0;

    hits0 = texcash_guard_hits;
    texcash_release_instance(&t_mt, &cp, 3, 0);

    CHECK(texcash_guard_hits == hits0 + 1, "mapping-miss guard fired %u times, expected 1",
          texcash_guard_hits - hits0);
    CHECK(cp.x16 == 3, "cp->x16 = %d after resync, expected 3 (popcount of the map)", (int)cp.x16);
    CHECK(cp.x32 == 1, "cp->x32 = %d after resync, expected 1", (int)cp.x32);

    /* The anti-texgroup.c:216 assertion. A guard written as `return;` would
     * leave every one of these slots held forever: time never decremented,
     * cs.code never -1, never returned to tpf by init_texcash_2nd. That is
     * the 2026-04-29 dup-transfer mistake, and this is what forbids it. */
    CHECK(t_mc16[9].time == 0 && t_mc16[9].cs.code == (u32)-1, "slot 9 leaked (time=%d)", (int)t_mc16[9].time);
    CHECK(t_mc16[10].time == 0 && t_mc16[10].cs.code == (u32)-1, "slot 10 leaked (time=%d)", (int)t_mc16[10].time);
    CHECK(t_mc16[11].time == 0 && t_mc16[11].cs.code == (u32)-1, "slot 11 leaked (time=%d)", (int)t_mc16[11].time);
    CHECK(t_mc32[2].time == 0 && t_mc32[2].cs.code == (u32)-1, "x32 slot 2 leaked (time=%d)", (int)t_mc32[2].time);

    printf("       tally resynced, all 4 slots actually returned (no leak)\n");
}

/* --------------------------------------------------------------------------
 * SUB_E -- what the pre-fix append actually produces: a wild pointer in the
 *          live list, dereferenced by texture_cash_update()
 * ----------------------------------------------------------------------- */

/* This sub-test started life trying to derive #59 from #61 -- the theory
 * being that adr[64] aliases &patt[0], so the live list ends up holding the
 * same instance twice, and the duplicate decrements drive a slot refcount
 * negative one frame later.
 *
 * Running it disproved that. adr[64] does land exactly on &patt[0] (SUB_A
 * proves it at runtime), but the *caller's own next three statements* --
 *
 *     cp->curr_disp = 1;
 *     cp->time      = mt->mltcshtime16;
 *     cp->cg.code   = cc.code;
 *
 * (mtrans.c:462-465 pre-fix) -- write over the very bytes adr[64] occupies:
 * curr_disp+time on a 32-bit ABI, curr_disp+time+cg.code on LP64. By the time
 * texture_cash_update() walks the list, adr[64] is not a duplicate pointer,
 * it is field data reinterpreted as an address. The first draft of this
 * sub-test dereferenced it and took SIGSEGV at `--adr[i]->time`, which is
 * exactly what texture_cash_update() does on a device.
 *
 * So the real #61 consequence is a wild-pointer *write* from
 * texture_cash_update(), not a stuck patt[0], and #59's `time < 0` is NOT
 * reachable through it. The assertions below pin that down; the harness does
 * not dereference the pointer, it proves it is unusable. */

static void sub_e_wild_pointer(void) {
    PatternInstance* cp;
    PatternInstance* dup;
    const unsigned char* pc_lo;
    const unsigned char* pc_hi;
    s16 released;
    s16 i;

    printf("SUB_E  #61 pre-fix append leaves a wild pointer in the live list\n");
    g_ran += 1;

    /* ---- part 1: the shipped guard refuses, and the walk stays safe ---- */
    mt_reset();
    memset(&t_pc, 0, sizeof t_pc);

    t_pc.patt[0].cg.code = 0xA000;
    t_pc.patt[0].time = 1;
    acquire_x16(&t_pc.patt[0], 5, 0xA000);
    for (i = 1; i < 0x40; i++) {
        t_pc.patt[i].time = 20;
    }
    rebuild_live_list(&t_pc);
    CHECK(t_pc.kazu == 0x40, "fixture kazu=%d, expected a full collection", (int)t_pc.kazu);

    cp = patcash_acquire(&t_pc); /* a 65th distinct cg code arrives */
    CHECK(cp == NULL, "guarded acquire did not refuse on the exhausted collection");
    CHECK(t_pc.kazu == 0x40, "guarded acquire advanced kazu to %d", (int)t_pc.kazu);
    CHECK(t_pc.patt[0].cg.code == 0xA000 && t_pc.patt[0].time == 1,
          "guarded acquire disturbed the live patt[0]");
    CHECK(t_mc16[5].time == 1, "guarded acquire disturbed patt[0]'s slot refcount");

    /* texture_cash_update()'s ext walk over the guarded list: every adr[i] is
     * a real instance, patt[0] expires and gives slot 5 back. */
    released = 0;
    for (i = 0; i < t_pc.kazu; i++) {
        CHECK(t_pc.adr[i] >= &t_pc.patt[0] && t_pc.adr[i] <= &t_pc.patt[0x3F],
              "guarded live list entry %d is not an instance", (int)i);
        if ((--t_pc.adr[i]->time) == 0) {
            texcash_release_instance(&t_mt, t_pc.adr[i], 3, i);
            released += 1;
        }
    }
    CHECK(released == 1, "%d instances expired, expected 1", (int)released);
    CHECK(t_mc16[5].time == 0 && t_mc16[5].cs.code == (u32)-1, "slot 5 was not returned");

    /* ---- part 2: the pre-fix path, identical starting state ---- */
    mt_reset();
    memset(&t_pc_ctl, 0, sizeof t_pc_ctl);

    t_pc_ctl.patt[0].cg.code = 0xA000;
    t_pc_ctl.patt[0].time = 1;
    acquire_x16(&t_pc_ctl.patt[0], 5, 0xA000);
    for (i = 1; i < 0x40; i++) {
        t_pc_ctl.patt[i].time = 20;
    }
    rebuild_live_list(&t_pc_ctl);

    {
        s32 ix = prefix_get_free_patcash_index(&t_pc_ctl);

        CONTROL(ix == 0, "control: expected the exhaustion sentinel, got %d", (int)ix);
        cp = &t_pc_ctl.patt[ix];
        CONTROL(cp->time != 0, "control: patt[0] was not live, so defect 2 cannot be shown");

        prefix_append(&t_pc_ctl, cp);
        CONTROL(t_pc_ctl.kazu == 0x41, "control: kazu did not reach 65");

        /* At this instant adr[64] IS &patt[0]. */
        dup = read_back_ptr(&t_pc_ctl.patt[0]);
        CONTROL(dup == cp, "control: adr[64] did not land on patt[0]");

        /* Now the caller's own lines, verbatim from mtrans.c:462-467 pre-fix. */
        cp->curr_disp = 1;
        cp->time = (s16)t_mt.mltcshtime16;
        cp->cg.code = 0xB000; /* the new sprite */
        cp->x16 = 0;
        cp->x32 = 0;
        memset(&cp->map, 0, sizeof cp->map); /* SDL_zero(cp->map) */
    }

    /* Defect 2, observed rather than argued: the map wipe discarded patt[0]'s
     * reference to slot 5 without decrementing it. Nothing in the collection
     * points at slot 5 any more, and its refcount is still 1 -- leaked for the
     * rest of the cache's life. Fully deterministic; no pointer garbage
     * involved. */
    CONTROL(t_mc16[5].time == 1, "control: slot 5 was not leaked (time=%d)", (int)t_mc16[5].time);
    CONTROL((t_pc_ctl.patt[0].map.x16_map[0][0] & (1 << 5)) == 0, "control: the map was not wiped");

    /* Defect 1, as it really behaves: adr[64] no longer holds the pointer that
     * was written into it. The caller's field writes turned it into field data.
     * Read it back and prove it is not a usable instance address. */
    dup = read_back_ptr((unsigned char*)&t_pc_ctl + offsetof(PatternCollection, adr) +
                        0x40 * sizeof(PatternInstance*));
    CONTROL(dup != &t_pc_ctl.patt[0], "control: adr[64] survived the caller's writes as a valid pointer -- "
                                      "the aliasing this whole task is about does not exist on this ABI");

    pc_lo = (const unsigned char*)&t_pc_ctl;
    pc_hi = pc_lo + sizeof t_pc_ctl;
    CONTROL((const unsigned char*)dup < pc_lo || (const unsigned char*)dup >= pc_hi,
            "control: adr[64] still points inside the collection (%p)", (void*)dup);

    /* And it is exactly the aliased fields, not an address: on any ABI the low
     * 16 bits are curr_disp (1) and the next 16 are time (mltcshtime16 = 20). */
    CONTROL(((uintptr_t)dup & 0xFFFFu) == 1u, "control: adr[64] low half is %u, expected patt[0].curr_disp == 1",
            (unsigned)((uintptr_t)dup & 0xFFFFu));
    CONTROL((((uintptr_t)dup >> 16) & 0xFFFFu) == 20u, "control: adr[64] second half is %u, expected "
                                                       "patt[0].time == 20",
            (unsigned)(((uintptr_t)dup >> 16) & 0xFFFFu));

    /* texture_cash_update() would now execute `--adr[64]->time`, i.e. a
     * read-modify-write at (char*)dup + offsetof(PatternInstance, time).
     * Deliberately not performed: an earlier draft did, and took
     * EXC_BAD_ACCESS at address 0xb00000000003. That segfault is the
     * pre-fix behaviour, reproduced -- it is a wild write, not a hang. */
    printf("       pre-fix adr[64] = %p (curr_disp|time bytes), outside the collection; "
           "texture_cash_update would write through it\n",
           (void*)dup);
    printf("       shipped guard refused the append, walked 64 real entries, released 1\n");
}

/* --------------------------------------------------------------------------
 * Runner
 * ----------------------------------------------------------------------- */

int Texcash_Test_Bounds(void) {
    printf("=== texcash bounds harness (tasks #59, #61) ===\n");

    sub_a_append_bound();
    sub_b_recovery();
    sub_c_underflow();
    sub_d_mapping_miss();
    sub_e_wild_pointer();

    if (g_ran != EXPECTED_SUBTESTS) {
        printf("COVERAGE FAIL: ran %d sub-test(s), expected exactly %d\n", g_ran, EXPECTED_SUBTESTS);
        return 2;
    }

    if (g_nosignal != 0) {
        printf("RESULT: NO-SIGNAL (%d control assertion(s) failed)\n", g_nosignal);
        printf("The pre-fix replica did not reproduce the defect, so a green run would mean nothing.\n");
        return 2;
    }

    if (g_fail != 0) {
        printf("RESULT: FAIL (%d assertion(s))\n", g_fail);
        return 1;
    }

    printf("RESULT: PASS (%d sub-tests, %u guard hits observed)\n", g_ran, texcash_guard_hits);
    return 0;
}

#endif /* ENABLE_NETPLAY_TESTS */
