/*
 * test_sparse_effect_save.c — Option A round-trip parity tests for the
 * sparse effect-pool save path (game_state.c). Mirrors the gating pattern
 * used by test_event_queue.c / test_room_code.c / test_stun_mock.c.
 * Enable with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS='-DENABLE_NETPLAY_TESTS -DNETPLAY_TEST_HOOKS'"
 * (NETPLAY_TEST_HOOKS is required because this build target links every
 * src/netplay/test_*.c TU, including test_bilateral_punch.c, whose
 * ENABLE_NETPLAY_TESTS branch calls DirectP2P_TestHook_IsLanPeer — a symbol
 * declared only when NETPLAY_TEST_HOOKS is also defined. See direct_p2p.h.)
 *
 * Coverage (see brief, "Test surface (must add — non-negotiable)"):
 *  1. Round-trip parity — synthetic effect-pool data with mixed active /
 *     inactive slots reads back byte-exact for active slots; inactive slots
 *     match the canonical empty pattern (myself=i, before=-1, behind=-1,
 *     rest zero).
 *  2. Boundary cases — 0, 1, SPARSE_CEILING_SLOTS (ceiling), 128 (overflow →
 *     pack itself refuses as of queue.md #148 point 2; see below).
 *  3. Linked-list integrity — head_ix walk through `behind` reaches every
 *     active slot in that priority and terminates at -1.
 *  4. be_flag invariant — every active slot has be_flag != 0 after
 *     round-trip; every inactive slot has be_flag == 0.
 *
 * Also covers queue.md #148 (task-brief hardening, landed alongside the
 * items above):
 *  5. pack_sparse_state's own be_flag!=0 count vs SPARSE_CEILING_SLOTS —
 *     over-ceiling refuses (returns 0, buffer untouched, canary-checked);
 *     exactly-ceiling still packs at the correct length; and the case the
 *     hardening exists for — be_flag set on a slot still on the free list,
 *     so the accounting predicate (EFFECT_MAX - frwctr) and the be_flag
 *     predicate DISAGREE — pack still refuses on its own predicate. That
 *     last case deliberately trips pack_sparse_state's invariant
 *     SDL_assert; see ignore_and_count_handler for how the test swaps
 *     src/main.c's abort-on-assert harness policy for a local
 *     ignore-and-count one just for that call.
 *  6. sparse_state_is_valid() — the predicate load_state_from_event() now
 *     runs before GameState_Load() — rejects short / popcount-mismatched /
 *     wrong-length buffers (mirroring the existing unpack_sparse_state
 *     rejection tests) and accepts pack's own valid output.
 *
 * Implementation: the test calls the pack/unpack helpers directly via the
 * Netplay_Test_PackSparseState / Netplay_Test_UnpackSparseState /
 * Netplay_Test_SparseStateIsValid trampolines exposed at the bottom of
 * game_state.c. We do not exercise the GekkoNet save_state /
 * load_state_from_event callback paths themselves — that requires a
 * session pump and is out of scope for a self-contained unit test.
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/game_state.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "structs.h"
#include "types.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Trampolines exposed by game_state.c when ENABLE_NETPLAY_TESTS is set. */
unsigned int Netplay_Test_PackSparseState(unsigned char* out_buf,
                                          const GameState* gs_src);
bool Netplay_Test_UnpackSparseState(const unsigned char* in_buf,
                                    unsigned int in_len);
/* queue.md #148 point 3: the pure validation predicate load_state_from_event()
 * now checks before calling GameState_Load(). */
bool Netplay_Test_SparseStateIsValid(const unsigned char* in_buf,
                                     unsigned int in_len);

static int fail_count = 0;
static int pass_count = 0;

#define T_FAIL(tag, fmt, ...)                                              \
    do {                                                                   \
        fprintf(stderr, "[test_sparse_effect_save] FAIL: %s: " fmt "\n",   \
                (tag), ##__VA_ARGS__);                                     \
        fail_count++;                                                      \
    } while (0)

#define T_PASS(tag) do { \
    fprintf(stderr, "[test_sparse_effect_save] PASS: %s\n", (tag)); \
    pass_count++; \
} while (0)

/* Sized for 128 active slots on 64-bit hosts (worst case in this test):
 *   sizeof(GameState_64) + 328 + 128*3584 ≈ 480 KB. We pad to 600 KB. */
static unsigned char wire_buf[600 * 1024];

/* Snapshot the live effect-pool globals so we can compare a post-roundtrip
 * state against the pre-roundtrip image. */
typedef struct EffectPoolSnapshot {
    s16 frwctr;
    s16 frwctr_min;
    s16 head_ix[8];
    s16 tail_ix[8];
    s16 exec_tm[8];
    s16 frwque[EFFECT_MAX];
    uintptr_t frw[EFFECT_MAX][448];
} EffectPoolSnapshot;

static EffectPoolSnapshot before_snap;
static EffectPoolSnapshot after_snap;

static void snapshot_pool(EffectPoolSnapshot* s) {
    s->frwctr = frwctr;
    s->frwctr_min = frwctr_min;
    SDL_memcpy(s->head_ix, head_ix, sizeof(head_ix));
    SDL_memcpy(s->tail_ix, tail_ix, sizeof(tail_ix));
    SDL_memcpy(s->exec_tm, exec_tm, sizeof(exec_tm));
    SDL_memcpy(s->frwque, frwque, sizeof(frwque));
    SDL_memcpy(s->frw, frw, sizeof(frw));
}

/* Reset the live pool to canonical empty (matches effect_work_init). */
static void reset_pool_canonical(void) {
    SDL_zeroa(frw);
    for (int i = 0; i < EFFECT_MAX; i++) {
        WORK* w = (WORK*)frw[i];
        w->myself = (s16)i;
        w->before = -1;
        w->behind = -1;
    }
    for (int i = 0; i < 8; i++) {
        head_ix[i] = -1;
        tail_ix[i] = -1;
        exec_tm[i] = 0;
    }
    for (int i = 0; i < EFFECT_MAX; i++) {
        frwque[i] = (s16)((EFFECT_MAX - 1) - i);
    }
    frwctr = EFFECT_MAX;
    frwctr_min = EFFECT_MAX;
}

/* Build a synthetic active slot at frw[i]: linearly link it onto list_ix's
 * priority chain. `seed` mixes into the body so each slot is byte-distinct.
 * Returns the slot's `myself` index (== i). */
static s16 install_active_slot(s16 i, s16 list_ix, uint32_t seed) {
    /* Fill the entire 1792-byte slot with a deterministic-but-noisy
     * pattern. xorshift-style scribble. */
    uint8_t* p = (uint8_t*)frw[i];
    uint32_t x = seed ^ 0xA5A5A5A5u ^ (uint32_t)(i * 0x01010101u);
    for (size_t k = 0; k < sizeof(frw[i]); k++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        p[k] = (uint8_t)(x & 0xFF);
    }

    WORK* w = (WORK*)frw[i];
    /* Linked-list correctness: enforce the active invariants on top of
     * the noise. */
    w->be_flag = 1;
    w->myself = i;
    w->listix = list_ix;
    /* Append to tail. */
    if (head_ix[list_ix] == -1) {
        head_ix[list_ix] = i;
        tail_ix[list_ix] = i;
        w->before = -1;
    } else {
        WORK* prev = (WORK*)frw[tail_ix[list_ix]];
        prev->behind = i;
        w->before = tail_ix[list_ix];
        tail_ix[list_ix] = i;
    }
    w->behind = -1;
    /* Take a queue slot — match pull_effect_work semantics so frwctr stays
     * coherent with the linked-list count. */
    if (frwctr > 0) {
        frwctr--;
        if (frwctr_min > frwctr) frwctr_min = frwctr;
    }
    return i;
}

/* Walk head_ix[lix] via `behind` and confirm:
 *  - termination at -1
 *  - every visited slot has be_flag != 0
 *  - cycle-free up to EFFECT_MAX hops
 *  - last slot's myself == tail_ix[lix]
 * Returns the count visited, or -1 on failure. */
static int walk_list(int lix, const char* tag) {
    int count = 0;
    s16 prev = -1;
    s16 cur = head_ix[lix];
    while (cur != -1) {
        if (cur < 0 || cur >= EFFECT_MAX) {
            T_FAIL(tag, "list[%d]: out-of-range index %d at hop %d", lix, cur, count);
            return -1;
        }
        const WORK* w = (const WORK*)frw[cur];
        if (w->be_flag == 0) {
            T_FAIL(tag, "list[%d]: visited slot %d has be_flag==0", lix, cur);
            return -1;
        }
        if (w->before != prev) {
            T_FAIL(tag, "list[%d]: slot %d before=%d expected %d", lix, cur, w->before, prev);
            return -1;
        }
        if (w->myself != cur) {
            T_FAIL(tag, "list[%d]: slot %d has myself=%d", lix, cur, w->myself);
            return -1;
        }
        if (++count > EFFECT_MAX) {
            T_FAIL(tag, "list[%d]: walk exceeded EFFECT_MAX (cycle?)", lix);
            return -1;
        }
        prev = cur;
        cur = w->behind;
    }
    if (prev != tail_ix[lix]) {
        T_FAIL(tag, "list[%d]: walked %d slots, last=%d but tail_ix=%d",
               lix, count, prev, tail_ix[lix]);
        return -1;
    }
    return count;
}

/* Round-trip core: pack the live state, wipe it, unpack, and check
 * byte-exact parity for every slot we marked active in `expected_active`. */
static void roundtrip_check(const char* tag,
                            const bool expected_active[EFFECT_MAX]) {
    snapshot_pool(&before_snap);

    /* Use a dummy GameState — pack reads sizeof(GameState) bytes from it
     * but we only care about the EffectState round-trip in this test. */
    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0xCC, sizeof(gs_dummy));

    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);

    /* Wipe live state to a deliberately-wrong pattern so unpack can't
     * accidentally pass via residual values. */
    SDL_memset(frw, 0xEE, sizeof(frw));
    SDL_memset(head_ix, 0xEE, sizeof(head_ix));
    SDL_memset(tail_ix, 0xEE, sizeof(tail_ix));
    SDL_memset(exec_tm, 0xEE, sizeof(exec_tm));
    SDL_memset(frwque,  0xEE, sizeof(frwque));
    frwctr = -1;
    frwctr_min = -1;

    if (!Netplay_Test_UnpackSparseState(wire_buf, len)) {
        T_FAIL(tag, "unpack rejected its own pack output (len=%u)", len);
        return;
    }

    snapshot_pool(&after_snap);

    /* Scalar/array fields must round-trip exactly. */
    if (after_snap.frwctr != before_snap.frwctr) {
        T_FAIL(tag, "frwctr %d != %d", after_snap.frwctr, before_snap.frwctr);
        return;
    }
    if (after_snap.frwctr_min != before_snap.frwctr_min) {
        T_FAIL(tag, "frwctr_min %d != %d", after_snap.frwctr_min, before_snap.frwctr_min);
        return;
    }
    if (SDL_memcmp(after_snap.head_ix, before_snap.head_ix, sizeof(head_ix)) != 0 ||
        SDL_memcmp(after_snap.tail_ix, before_snap.tail_ix, sizeof(tail_ix)) != 0 ||
        SDL_memcmp(after_snap.exec_tm, before_snap.exec_tm, sizeof(exec_tm)) != 0 ||
        SDL_memcmp(after_snap.frwque, before_snap.frwque, sizeof(frwque)) != 0) {
        T_FAIL(tag, "scalar/array header round-trip mismatch");
        return;
    }

    /* Active slots: byte-exact. */
    /* Inactive slots: canonical empty (myself=i, before=behind=-1, rest zero). */
    int active_seen = 0;
    int inactive_seen = 0;
    for (int i = 0; i < EFFECT_MAX; i++) {
        const WORK* w = (const WORK*)after_snap.frw[i];
        if (expected_active[i]) {
            if (SDL_memcmp(after_snap.frw[i], before_snap.frw[i], sizeof(frw[i])) != 0) {
                T_FAIL(tag, "active slot %d: byte-mismatch after round-trip", i);
                return;
            }
            if (w->be_flag == 0) {
                T_FAIL(tag, "active slot %d: be_flag==0 after round-trip", i);
                return;
            }
            active_seen++;
        } else {
            /* Build the canonical empty image for slot i and compare. */
            uintptr_t empty[448];
            SDL_zeroa(empty);
            WORK* ew = (WORK*)empty;
            ew->myself = (s16)i;
            ew->before = -1;
            ew->behind = -1;
            if (SDL_memcmp(after_snap.frw[i], empty, sizeof(frw[i])) != 0) {
                T_FAIL(tag, "inactive slot %d: not canonical empty", i);
                return;
            }
            if (w->be_flag != 0) {
                T_FAIL(tag, "inactive slot %d: be_flag!=0 after round-trip", i);
                return;
            }
            inactive_seen++;
        }
    }

    /* Linked-list integrity: walk every list, count active slots reached. */
    int total_walked = 0;
    for (int lix = 0; lix < 8; lix++) {
        int n = walk_list(lix, tag);
        if (n < 0) return;
        total_walked += n;
    }
    if (total_walked != active_seen) {
        T_FAIL(tag, "linked-list walked %d slots; expected %d active",
               total_walked, active_seen);
        return;
    }

    fprintf(stderr,
            "[test_sparse_effect_save] %s: active=%d inactive=%d wire_bytes=%u\n",
            tag, active_seen, inactive_seen, len);
    T_PASS(tag);
}

/* === Boundary cases =================================================== */

static void test_zero_active(void) {
    bool act[EFFECT_MAX];
    SDL_memset(act, 0, sizeof(act));
    reset_pool_canonical();
    roundtrip_check("zero-active", act);
}

static void test_one_active(void) {
    bool act[EFFECT_MAX];
    SDL_memset(act, 0, sizeof(act));
    reset_pool_canonical();
    install_active_slot(/*i=*/3, /*list_ix=*/2, /*seed=*/0xDEADBEEF);
    act[3] = true;
    roundtrip_check("one-active", act);
}

static void test_mixed_active(void) {
    bool act[EFFECT_MAX];
    SDL_memset(act, 0, sizeof(act));
    reset_pool_canonical();
    /* 25 active spread across all 8 priority lists — exercises mid-list
     * inserts and varied seeds. */
    static const int slots[] = {
        0, 5, 9, 13, 21, 27, 33, 41, 47, 50, 58, 63, 71, 77, 84,
        88, 92, 96, 100, 105, 110, 115, 119, 123, 127,
    };
    for (size_t k = 0; k < sizeof(slots)/sizeof(slots[0]); k++) {
        s16 ix = (s16)slots[k];
        install_active_slot(ix, (s16)(ix & 7), 0xC0FFEE00u + (uint32_t)k);
        act[ix] = true;
    }
    roundtrip_check("mixed-active-25", act);
}

static void test_ceiling_active(void) {
    bool act[EFFECT_MAX];
    SDL_memset(act, 0, sizeof(act));
    reset_pool_canonical();
    /* SPARSE_CEILING_SLOTS active exactly. Use slots 0..(N-1); spread
     * across all 8 lists. */
    for (int i = 0; i < SPARSE_CEILING_SLOTS; i++) {
        install_active_slot((s16)i, (s16)(i & 7), 0x12345678u + (uint32_t)i);
        act[i] = true;
    }
    roundtrip_check("ceiling-active", act);
}

static void test_overflow_128_pack_refuses(void) {
    reset_pool_canonical();
    /* All 128 slots active — the pool's absolute worst case, and well past
     * SPARSE_CEILING_SLOTS(100). Historically (pre queue.md #148 point 2)
     * pack_sparse_state did NOT enforce the ceiling itself — that was the
     * caller's job (save_state's accounting check) — so this buffer packed
     * fine and only the caller-side guard mattered "by inspection". After
     * the hardening, pack_sparse_state enforces SPARSE_CEILING_SLOTS on its
     * OWN be_flag!=0 count regardless of caller behaviour, so this is now
     * a pack-refuses case rather than a round-trip case — folding this
     * boundary into test_beflag_over_ceiling_pack_refuses's coverage at
     * the pool's actual maximum (EFFECT_MAX, not just ceiling+1). */
    for (int i = 0; i < EFFECT_MAX; i++) {
        install_active_slot((s16)i, (s16)(i & 7), 0xFEEDBEEFu + (uint32_t)i);
    }

    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0xCC, sizeof(gs_dummy));
    SDL_memset(wire_buf, 0xAB, sizeof(wire_buf));
    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);
    if (len != 0) {
        T_FAIL("all-128-active-refuses",
               "pack_sparse_state packed %u bytes for all %d slots active "
               "(SPARSE_CEILING_SLOTS=%d)",
               len, EFFECT_MAX, SPARSE_CEILING_SLOTS);
        return;
    }
    for (size_t k = 0; k < sizeof(wire_buf); k++) {
        if (wire_buf[k] != 0xAB) {
            T_FAIL("all-128-active-refuses",
                   "wire_buf byte %zu changed to 0x%02x -- pack wrote despite "
                   "returning 0 (canary check)",
                   k, (unsigned)wire_buf[k]);
            return;
        }
    }
    T_PASS("all-128-active-refuses");
}

/* Targeted: confirm unpack rejects a corrupted active_count vs popcount. */
static void test_unpack_rejects_bad_count(void) {
    reset_pool_canonical();
    install_active_slot(7, 1, 0xBADC0FFE);

    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0, sizeof(gs_dummy));
    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);

    /* Smear active_count to a bogus value — must not match popcount(mask). */
    /* The active_count lives at offset sizeof(GameState) + 324 in the wire. */
    size_t cnt_off = sizeof(GameState) + 324;
    uint16_t bogus = 99;
    SDL_memcpy(wire_buf + cnt_off, &bogus, sizeof(bogus));

    if (Netplay_Test_UnpackSparseState(wire_buf, len)) {
        T_FAIL("reject-bad-count", "unpack accepted corrupted active_count");
        return;
    }
    T_PASS("reject-bad-count");
}

static void test_unpack_rejects_short_buf(void) {
    reset_pool_canonical();
    install_active_slot(11, 0, 0xABCDEF01);
    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0, sizeof(gs_dummy));
    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);
    /* Truncate by 100 bytes. */
    if (len < 200) { T_FAIL("reject-short-buf", "len too small to truncate"); return; }
    if (Netplay_Test_UnpackSparseState(wire_buf, len - 100)) {
        T_FAIL("reject-short-buf", "unpack accepted truncated buffer");
        return;
    }
    T_PASS("reject-short-buf");
}

/* === queue.md #148 point 2: pack_sparse_state hardening =================
 * pack_sparse_state now counts be_flag!=0 slots ITSELF, before writing
 * anything, and refuses (returns 0, writes nothing) if that count exceeds
 * SPARSE_CEILING_SLOTS -- independent of the accounting predicate
 * (EFFECT_MAX - frwctr) the caller (save_state) already checked. Three
 * required cases:
 *   (a) beflag-count > SPARSE_CEILING_SLOTS -> refuses, buffer untouched.
 *   (b) beflag-count == SPARSE_CEILING_SLOTS -> still packs, correct length.
 *   (c) beflag-count and the accounting count DISAGREE (be_flag set on a
 *       slot still on the free list, frwctr not decremented) -> pack still
 *       refuses on its OWN predicate. This is the whole point of the change
 *       -- the harness CAN express it (see below).
 */

static void test_beflag_over_ceiling_pack_refuses(void) {
    reset_pool_canonical();
    for (int i = 0; i < SPARSE_CEILING_SLOTS + 1; i++) {
        install_active_slot((s16)i, (s16)(i & 7), 0x11111100u + (uint32_t)i);
    }

    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0xCC, sizeof(gs_dummy));
    SDL_memset(wire_buf, 0xAB, sizeof(wire_buf));
    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);
    if (len != 0) {
        T_FAIL("reject-beflag-over-ceiling",
               "pack_sparse_state packed %u bytes for %d active slots "
               "(SPARSE_CEILING_SLOTS=%d)",
               len, SPARSE_CEILING_SLOTS + 1, SPARSE_CEILING_SLOTS);
        return;
    }
    for (size_t k = 0; k < sizeof(wire_buf); k++) {
        if (wire_buf[k] != 0xAB) {
            T_FAIL("reject-beflag-over-ceiling",
                   "wire_buf byte %zu changed to 0x%02x -- pack wrote despite "
                   "returning 0 (canary check)",
                   k, (unsigned)wire_buf[k]);
            return;
        }
    }
    T_PASS("reject-beflag-over-ceiling");
}

static void test_ceiling_active_packs_nonzero_correct_length(void) {
    reset_pool_canonical();
    for (int i = 0; i < SPARSE_CEILING_SLOTS; i++) {
        install_active_slot((s16)i, (s16)(i & 7), 0x22222200u + (uint32_t)i);
    }

    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0xCC, sizeof(gs_dummy));
    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);
    const unsigned int expected = (unsigned int)(sizeof(GameState) + SPARSE_HEADER_BYTES +
                                                  (size_t)SPARSE_CEILING_SLOTS * SPARSE_FRW_SLOT_BYTES);
    if (len != expected) {
        T_FAIL("ceiling-active-nonzero-len",
               "len=%u, expected %u for exactly SPARSE_CEILING_SLOTS=%d active",
               len, expected, SPARSE_CEILING_SLOTS);
        return;
    }
    T_PASS("ceiling-active-nonzero-len");
}

/* Temporary assertion handler for (c): src/main.c installs an
 * abort-on-assert handler (test_harness_assert_handler) for every
 * --test-* harness including this one, specifically so a tripped assert
 * terminates loudly instead of hanging on an interactive prompt. That
 * policy is exactly right for an UNEXPECTED assert, but this test
 * DELIBERATELY trips the new SDL_assert(beflag_count <= EFFECT_MAX -
 * frwctr) inside pack_sparse_state, so it swaps in a local handler that
 * returns SDL_ASSERTION_IGNORE (lets execution continue) and counts hits,
 * then restores whatever handler was previously installed. Verifying
 * s_ignore_handler_hits > 0 also proves the "LOUD in debug/test builds"
 * requirement is actually wired up, not just that the fallback silently
 * absorbed the disagreement. */
static int s_ignore_handler_hits = 0;
static SDL_AssertState SDLCALL ignore_and_count_handler(const SDL_AssertData* data, void* userdata) {
    (void)userdata;
    s_ignore_handler_hits++;
    fprintf(stderr,
            "[test_sparse_effect_save] expected assertion trip: '%s' at %s:%d (%s)\n",
            data->condition, data->filename, data->linenum, data->function);
    return SDL_ASSERTION_IGNORE;
}

static void test_beflag_accounting_mismatch_pack_refuses(void) {
    reset_pool_canonical();

    /* Deliberately break the invariant under test: flip be_flag directly on
     * SPARSE_CEILING_SLOTS+1 slots WITHOUT going through pull_effect_work,
     * so frwctr (and the accounting predicate EFFECT_MAX - frwctr) never
     * moves off "0 active" -- accounting alone would say "nothing to worry
     * about" while the real content disagrees. This is exactly the
     * scenario queue.md #148 point 2 describes: pack must refuse on its
     * OWN predicate, not trust the caller's accounting. */
    for (int i = 0; i < SPARSE_CEILING_SLOTS + 1; i++) {
        WORK* w = (WORK*)frw[i];
        w->be_flag = 1;
        w->myself = (s16)i;
    }
    int accounting_active = EFFECT_MAX - frwctr;
    if (accounting_active > SPARSE_CEILING_SLOTS) {
        T_FAIL("reject-beflag-accounting-mismatch",
               "test setup bug: accounting_active=%d already exceeds the "
               "ceiling -- the disagreement this test needs is not being "
               "exercised",
               accounting_active);
        return;
    }

    void* prev_userdata = NULL;
    SDL_AssertionHandler prev_handler = SDL_GetAssertionHandler(&prev_userdata);
    s_ignore_handler_hits = 0;
    SDL_SetAssertionHandler(ignore_and_count_handler, NULL);

    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0xCC, sizeof(gs_dummy));
    SDL_memset(wire_buf, 0xAB, sizeof(wire_buf));
    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);

    SDL_SetAssertionHandler(prev_handler, prev_userdata);

    if (len != 0) {
        T_FAIL("reject-beflag-accounting-mismatch",
               "pack_sparse_state packed %u bytes despite beflag_count > "
               "SPARSE_CEILING_SLOTS with accounting_active=%d (disagreement)",
               len, accounting_active);
        return;
    }
    for (size_t k = 0; k < sizeof(wire_buf); k++) {
        if (wire_buf[k] != 0xAB) {
            T_FAIL("reject-beflag-accounting-mismatch",
                   "wire_buf byte %zu changed to 0x%02x -- pack wrote despite "
                   "returning 0 (canary check)",
                   k, (unsigned)wire_buf[k]);
            return;
        }
    }
    if (s_ignore_handler_hits == 0) {
        T_FAIL("reject-beflag-accounting-mismatch",
               "pack refused correctly but the invariant SDL_assert never "
               "fired -- the 'LOUD in debug/test builds' requirement is not "
               "actually wired up");
        return;
    }
    T_PASS("reject-beflag-accounting-mismatch");
}

/* === queue.md #148 point 3: sparse_state_is_valid() predicate tests =====
 * load_state_from_event() now validates a sparse buffer BEFORE calling
 * GameState_Load(), via the pure predicate sparse_state_is_valid(). These
 * exercise the predicate directly (Netplay_Test_SparseStateIsValid), the
 * same three shapes test_unpack_rejects_bad_count /
 * test_unpack_rejects_short_buf already prove unpack_sparse_state rejects
 * -- confirming the split moved the logic without changing it. */

static void test_predicate_rejects_short_buf(void) {
    reset_pool_canonical();
    install_active_slot(5, 0, 0x5A5A5A5Au);
    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0, sizeof(gs_dummy));
    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);
    if (len < 200) { T_FAIL("predicate-reject-short-buf", "len too small to truncate"); return; }
    if (Netplay_Test_SparseStateIsValid(wire_buf, len - 100)) {
        T_FAIL("predicate-reject-short-buf", "predicate accepted truncated buffer");
        return;
    }
    T_PASS("predicate-reject-short-buf");
}

static void test_predicate_rejects_popcount_mismatch(void) {
    reset_pool_canonical();
    install_active_slot(9, 2, 0x9A9A9A9Au);
    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0, sizeof(gs_dummy));
    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);
    /* Corrupt a MASK bit, not active_count: active_count (1) and `len` (sized
     * for 1 active slot) must stay mutually consistent, or the length check
     * earlier in sparse_state_is_valid() rejects first and this test never
     * reaches the popcount branch it's named for. Slot 9's bit lives in mask
     * byte 1 (9/8), bit 1 (9%8); flip slot 0's bit in byte 0 instead --
     * unrelated to slot 9, so popcount(mask) becomes 2 while active_count
     * stays 1. */
    size_t mask_off = sizeof(GameState) + 308;
    wire_buf[mask_off] |= 0x01;
    if (Netplay_Test_SparseStateIsValid(wire_buf, len)) {
        T_FAIL("predicate-reject-popcount-mismatch", "predicate accepted a mask/active_count popcount mismatch");
        return;
    }
    T_PASS("predicate-reject-popcount-mismatch");
}

static void test_predicate_rejects_wrong_length(void) {
    reset_pool_canonical();
    install_active_slot(13, 4, 0x13131313u);
    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0, sizeof(gs_dummy));
    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);
    /* One byte over a legal sparse length is neither sizeof(State) nor a
     * length any active_count produces -- must be rejected. */
    if (Netplay_Test_SparseStateIsValid(wire_buf, len + 1)) {
        T_FAIL("predicate-reject-wrong-length", "predicate accepted a length one byte over legal");
        return;
    }
    T_PASS("predicate-reject-wrong-length");
}

static void test_predicate_accepts_its_own_pack_output(void) {
    bool act[EFFECT_MAX];
    SDL_memset(act, 0, sizeof(act));
    reset_pool_canonical();
    install_active_slot(17, 5, 0x17171717u);
    act[17] = true;
    GameState gs_dummy;
    SDL_memset(&gs_dummy, 0, sizeof(gs_dummy));
    unsigned int len = Netplay_Test_PackSparseState(wire_buf, &gs_dummy);
    if (!Netplay_Test_SparseStateIsValid(wire_buf, len)) {
        T_FAIL("predicate-accepts-own-output", "predicate rejected pack's own valid output");
        return;
    }
    T_PASS("predicate-accepts-own-output");
}

int Netplay_Test_SparseEffectSave(void) {
    fail_count = 0;
    pass_count = 0;

    fprintf(stderr,
            "[test_sparse_effect_save] EFFECT_MAX=%d SPARSE_CEILING_SLOTS=%d "
            "SPARSE_FRW_SLOT_BYTES=%zu SPARSE_HEADER_BYTES=%d "
            "SPARSE_CEILING_BYTES=%zu sizeof(State)=%zu\n",
            EFFECT_MAX, SPARSE_CEILING_SLOTS, (size_t)SPARSE_FRW_SLOT_BYTES,
            SPARSE_HEADER_BYTES, (size_t)SPARSE_CEILING_BYTES, sizeof(State));

    test_zero_active();
    test_one_active();
    test_mixed_active();
    test_ceiling_active();
    test_overflow_128_pack_refuses();
    test_unpack_rejects_bad_count();
    test_unpack_rejects_short_buf();

    /* queue.md #148 point 2: pack_sparse_state hardening */
    test_beflag_over_ceiling_pack_refuses();
    test_ceiling_active_packs_nonzero_correct_length();
    test_beflag_accounting_mismatch_pack_refuses();

    /* queue.md #148 point 3: sparse_state_is_valid() predicate */
    test_predicate_rejects_short_buf();
    test_predicate_rejects_popcount_mismatch();
    test_predicate_rejects_wrong_length();
    test_predicate_accepts_its_own_pack_output();

    fprintf(stderr,
            "[test_sparse_effect_save] summary: %d passed, %d failed\n",
            pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_SparseEffectSave(void) {
    /* "not compiled in", spelled exactly that way. This stub used to say
     * "not compiled WITH", which reads identically to a human and is
     * invisible to tools/gates/run-gates.sh, whose harness gate greps for
     * the literal phrase "not compiled in" to turn an exit-2 misbuild
     * into RED. For this harness's whole life a netplay-tests-off build
     * would have exited 2 and been annotated as a plain non-zero exit
     * with no cause. Found while adding test_rendezvous_wire.c (#122). */
    fprintf(stderr,
            "[test_sparse_effect_save] not compiled in; rebuild with "
            "EXTRA_CMAKE_ARGS=\"-DENABLE_NETPLAY=ON -DCMAKE_C_FLAGS='-DENABLE_NETPLAY_TESTS "
            "-DNETPLAY_TEST_HOOKS'\".\n");
    return 2;
}

#endif
