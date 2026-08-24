/*
 * test_gs_coverage.c — GameState save/load field-coverage guard (M-3).
 *
 * Problem this closes: nothing ties GameState struct fields to their
 * GS_SAVE/GS_LOAD lines. The _Static_assert in game_state.c only pins the
 * TOTAL sizeof(GameState), so a future field addition that re-pins the
 * size but forgets the save/load line compiles clean and silently loses
 * rollback coverage — exactly how the chainex_check / eff79 /
 * ca_check_flag / Color7 / spmv_ng_save bug class kept reappearing.
 *
 * Mechanism: randomized load->save round-trip over the whole struct.
 *   1. Fill a GameState-sized buffer with deterministic PRNG bytes.
 *   2. GameState_Load(&src)  — pushes the random values into every global
 *      that has a GS_LOAD line (both save and load are pure memcpy — no
 *      transformation — so covered bytes must round-trip exactly).
 *   3. memset(&dst, 0), GameState_Save(&dst) — reads the globals back.
 *   4. Any byte where dst != src was NOT written by the load/save pair.
 *   Two passes with independent seeds: a byte counts as a HOLE if it
 *   fails to round-trip under EITHER seed (covered[] is cleared on any
 *   mismatch and never set back). A genuinely-uncovered byte evades
 *   detection only when the dst byte it never round-tripped through
 *   (memset zero, or a stale global echoed by a save-only line) happens
 *   to equal the random src byte under BOTH seeds — per-byte chance
 *   2^-16, so a multi-byte forgotten field is effectively impossible
 *   to miss.
 *
 * Holes = struct padding + forgotten fields. Padding is stable for a
 * given layout, so the total is pinned below; when a field is added:
 *   - correctly (field + GS_SAVE + GS_LOAD): hole count changes only if
 *     padding shifted — re-pin consciously, the failure output lists the
 *     exact byte offsets so you can confirm every hole is padding;
 *   - forgetting GS_SAVE and/or GS_LOAD: the field's bytes appear as NEW
 *     HOLES at its offset and this harness fails loudly, naming the
 *     offsets and telling you to add the missing lines.
 *
 * Why this over an X-macro field table: GameState has ~600 fields with
 * two ~700-line save/load bodies and extensive per-field comments;
 * regenerating struct + bodies from one list would be a huge, high-risk
 * rewrite. This harness adds the same guarantee (any unsaved field fails
 * CI loudly) with zero changes to the struct or the save/load code, and
 * follows the established --test-* harness pattern.
 *
 * Gating mirrors test_room_code.c: TU always linked when ENABLE_NETPLAY,
 * real body only with -DENABLE_NETPLAY_TESTS (stub returns 2 otherwise).
 * Run via: 3S-ARM --test-gs-coverage
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/game_state.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <string.h>

/* Expected number of non-round-tripping bytes (= padding holes) in the
 * current GameState layout, measured empirically on 64-bit hosts (the
 * only place this harness runs; the MiSTer/ARM32 layout is guarded by
 * game_state.c's EXPECTED_GAME_STATE_SIZE _Static_assert instead).
 * If this fails after you touched GameState:
 *   - MORE holes than expected at your new field's offset => you forgot
 *     the GS_SAVE and/or GS_LOAD line. Add them.
 *   - count changed but every listed hole is alignment padding => layout
 *     shift; re-pin this constant in the same commit that changed the
 *     struct, and say so in the commit message. */
#if UINTPTR_MAX == 0xffffffffffffffffULL
/* Measured 2026-08-23 on macOS arm64 host (sizeof(GameState) == 19248):
 * 59 hole bytes across 24 ranges, cross-checked against a clang
 * -fdump-record-layouts offsetof/sizeof map of all 610 top-level members
 * — every hole byte lies in inter-member or trailing alignment padding,
 * and all 610 members round-trip. */
#define GS_COVERAGE_EXPECTED_HOLE_BYTES 59
#endif

static uint32_t rng_state;

static uint32_t xorshift32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void fill_random(uint8_t* p, size_t n, uint32_t seed) {
    rng_state = seed ? seed : 0xA5A5A5A5u;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(xorshift32() >> 24);
    }
}

static GameState gs_orig;
static GameState gs_src;
static GameState gs_dst;
static uint8_t covered[sizeof(GameState)];

/* One load->save round trip; clears covered[] where bytes didn't survive. */
static void roundtrip_pass(uint32_t seed) {
    fill_random((uint8_t*)&gs_src, sizeof(gs_src), seed);
    GameState_Load(&gs_src);
    memset(&gs_dst, 0, sizeof(gs_dst));
    GameState_Save(&gs_dst);

    const uint8_t* s = (const uint8_t*)&gs_src;
    const uint8_t* d = (const uint8_t*)&gs_dst;
    for (size_t i = 0; i < sizeof(GameState); i++) {
        if (s[i] != d[i]) {
            covered[i] = 0;
        }
    }
}

int Netplay_Test_GsCoverage(void) {
    /* Snapshot live globals so we can put them back before returning. */
    GameState_Save(&gs_orig);

    memset(covered, 1, sizeof(covered));
    roundtrip_pass(0x1B873593u);
    roundtrip_pass(0xCC9E2D51u);

    GameState_Load(&gs_orig);

    /* Collect holes as offset ranges for readable output. */
    size_t hole_bytes = 0;
    int range_open = 0;
    size_t range_start = 0;
    fprintf(stderr,
            "[test_gs_coverage] sizeof(GameState) = %zu on this host\n",
            sizeof(GameState));
    for (size_t i = 0; i <= sizeof(GameState); i++) {
        int is_hole = (i < sizeof(GameState)) && !covered[i];
        if (is_hole) {
            hole_bytes++;
            if (!range_open) {
                range_open = 1;
                range_start = i;
            }
        } else if (range_open) {
            fprintf(stderr,
                    "[test_gs_coverage]   hole: bytes %zu..%zu (%zu byte%s) "
                    "not written by GameState_Load/GameState_Save\n",
                    range_start, i - 1, i - range_start,
                    (i - range_start) == 1 ? "" : "s");
            range_open = 0;
        }
    }
    fprintf(stderr,
            "[test_gs_coverage] total non-round-tripping bytes: %zu\n",
            hole_bytes);

#ifdef GS_COVERAGE_EXPECTED_HOLE_BYTES
    if (hole_bytes != GS_COVERAGE_EXPECTED_HOLE_BYTES) {
        fprintf(stderr,
                "[test_gs_coverage] FAIL: expected exactly %d hole bytes "
                "(struct padding), found %zu.\n"
                "[test_gs_coverage] If you added a GameState field: every "
                "field needs BOTH a GS_SAVE line in GameState_Save and a "
                "GS_LOAD line in GameState_Load (src/netplay/game_state.c) "
                "— a field missing either one shows up as new hole bytes at "
                "its offset above, and its state is silently lost on netplay "
                "rollback (the chainex_check/spmv_ng_save desync class).\n"
                "[test_gs_coverage] Only if you have verified every hole "
                "listed above is alignment padding may you re-pin "
                "GS_COVERAGE_EXPECTED_HOLE_BYTES in test_gs_coverage.c.\n",
                GS_COVERAGE_EXPECTED_HOLE_BYTES, hole_bytes);
        return 1;
    }
    fprintf(stderr,
            "[test_gs_coverage] OK — all %zu hole bytes match the pinned "
            "padding expectation (%d); every GameState field round-trips "
            "through GS_SAVE/GS_LOAD\n",
            hole_bytes, GS_COVERAGE_EXPECTED_HOLE_BYTES);
    return 0;
#else
    /* 32-bit build of the harness: no pinned padding figure (tests run on
     * 64-bit hosts; ARM32 layout is pinned by EXPECTED_GAME_STATE_SIZE).
     * Report the measurement without asserting an unverified constant. */
    fprintf(stderr,
            "[test_gs_coverage] NOTE: no pinned hole-byte expectation for "
            "32-bit builds; measured %zu (informational only)\n",
            hole_bytes);
    return 0;
#endif
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_GsCoverage(void) {
    fprintf(stderr,
            "[test_gs_coverage] not compiled in; rebuild with "
            "-DENABLE_NETPLAY_TESTS to enable.\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
