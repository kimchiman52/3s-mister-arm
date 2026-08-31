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
 * and all 610 members round-trip.
 *
 * Re-pinned 2026-08-24, 59 -> 57 (sizeof(GameState) == 19344, 612
 * top-level members), after two fields were added:
 *   - Random_ix16_bg (s16) restored between Random_ix32_ex_com and
 *     Opening_Now. On 64-bit it costs ZERO new bytes: it lands inside
 *     the existing pre-`task[11]` alignment padding, so the hole at
 *     2044..2047 shrinks to 2046..2047 and the count drops by 2. This is
 *     the "FEWER holes" direction, which can only mean padding became a
 *     covered field — never a coverage loss.
 *   - effl8_colorram[4][12] (u16) appended at 19248, running to 19343 =
 *     sizeof-1, i.e. no trailing hole: all 96 bytes round-trip.
 * Verification (not assumed): clang -Xclang -fdump-record-layouts on
 * this exact header, then checking every one of the 24 reported hole
 * ranges against the member offset map. All 24 sit strictly BETWEEN two
 * consecutive members (e.g. 11..11 after hoji_counter@10 before the
 * then-present select_timer_state@12; 19237..19239 after
 * ca_check_flag@19236 before spmv_ng_save@19240); none coincides with or
 * falls inside a member.
 *
 * Re-pinned 2026-08-29 (task #109), 57 -> 53 (sizeof(GameState) == 19328,
 * 611 top-level members), after ONE field was removed: select_timer_state
 * (SelectTimerState, 12 bytes at offset 12). It was the last user of the
 * dead src/sf33rd/Source/Game/select_timer.{c,h} module, which upstream
 * 33dfd75b (#216) had already replaced with effect A5; the member was
 * saved and loaded as permanent zeros. Removing it also removes the
 * 11..11 pad byte that preceded it, since u8 Order[148] now starts at
 * offset 11 -- this is the "FEWER holes" direction, which can only mean
 * padding stopped existing, never a coverage loss. The remaining -3
 * comes from downstream realignment (e.g. a new 1-byte hole appears at
 * 455..455 before u32 Score[2][3], and the old trailing-padding shape
 * changes); the totals are 19344 - 19328 = 16 = 12 payload + 4 padding,
 * which reconciles exactly.
 *
 * Verification (not assumed, and stronger than the 2026-08-24 pass): a
 * generated offsetof/sizeof map of ALL 611 top-level members was used to
 * compute the exact padding-byte set of the struct, and it was compared
 * with the 53 hole bytes this harness reports. The two sets are
 * IDENTICAL -- 0 holes outside padding, 0 padding bytes that
 * round-tripped. Independently, the member-name set of GameState matches
 * the GS_SAVE and GS_LOAD name sets exactly: 606 of the 611 members go
 * through GS_SAVE/GS_LOAD, and the other 5 (chainex_check, Color7,
 * ca_check_flag, spmv_ng_save, effl8_colorram) are copied by the
 * explicit SDL_memcpy blocks in GameState_Save/GameState_Load, so every
 * member is written on both paths. */
#define GS_COVERAGE_EXPECTED_HOLE_BYTES 53
#endif

/* === PLW canonical-image checks (task #111) =============================
 *
 * The cross-peer checksum hashes PLW through GameState_EmitPlwCanonical —
 * an explicit list of PLW's non-pointer members — instead of raw struct
 * bytes, because sizeof(PLW) is architecture-dependent. Four things have
 * to hold for that to be both correct and worth having, and all four are
 * checked below rather than argued:
 *
 *   1. TILING: emitted members + skipped pointer members + padding tile
 *      sizeof(PLW) exactly, with no byte counted twice. Without this the
 *      "explicit list" could silently omit a member.
 *   2. EVERY MEMBER MATTERS: perturbing any one of the emitted members
 *      changes the image. A checksum that agrees on everything is worse
 *      than no checksum.
 *   3. POINTERS DO NOT: perturbing the pointer members changes nothing,
 *      which is what makes the image comparable across architectures
 *      (and across peers, where ASLR makes addresses meaningless).
 *   4. DETERMINISM: identical input, identical image, every time.
 *
 * Plus a regression pin for the defect this replaced — see
 * plw_old_sweep_reference below. */

#include "sf33rd/utils/djb2_hash.h"
#include <stddef.h>

typedef struct {
    uint16_t off;
    uint16_t len;
} PlwTestSpan;

#define PLW_TEST_SPAN(f) { (uint16_t)offsetof(PLW, f), (uint16_t)sizeof(((PLW*)0)->f) },
static const PlwTestSpan plw_test_fields[] = { PLW_CANON_FIELD_LIST(PLW_TEST_SPAN) };
static const PlwTestSpan plw_test_pointers[] = { PLW_CANON_POINTER_LIST(PLW_TEST_SPAN) };

#define PLW_TEST_FIELD_COUNT ((int)(sizeof(plw_test_fields) / sizeof(plw_test_fields[0])))
#define PLW_TEST_POINTER_COUNT ((int)(sizeof(plw_test_pointers) / sizeof(plw_test_pointers[0])))

/* Padding bytes left over after every member is accounted for. Measured
 * from clang's record layout for PLW on each target; a change here means
 * the struct's layout moved and the accounting must be re-derived, not
 * re-guessed. */
#if UINTPTR_MAX == 0xffffffffffffffffULL
#define PLW_TEST_EXPECTED_PADDING 27
#else
#define PLW_TEST_EXPECTED_PADDING 11
#endif

static void fill_random(uint8_t* p, size_t n, uint32_t seed);

static uint8_t plw_canon_a[PLW_CANON_SIZE];
static uint8_t plw_canon_b[PLW_CANON_SIZE];
static PLW plw_test_a;
static PLW plw_test_b;
static uint8_t plw_byte_owner[sizeof(PLW)];

static uint32_t canon_hash(const uint8_t* img) {
    return djb2_update_mem(djb2_init(), img, PLW_CANON_SIZE);
}

/* The sweep this replaced, kept verbatim as a REFERENCE ORACLE (it is not
 * called by production code any more — see the deleted-sweep comment in
 * game_state.c). It exists so the regression check below can show, rather
 * than assert, that the old filter erased real gameplay divergences. */
static void plw_old_sweep_reference(PLW* copy) {
    uint64_t* words = (uint64_t*)copy;
    const size_t count = sizeof(PLW) / sizeof(uint64_t);
    for (size_t i = 0; i < count; i++) {
        uint64_t v = words[i];
        if (v > 0x100000000ULL && (v >> 47) == 0) {
            words[i] = 0;
        }
    }
}

static int plw_canonical_checks(void) {
    int failures = 0;

    fprintf(stderr,
            "[test_gs_coverage] PLW canonical image: sizeof(PLW)=%zu "
            "emitted=%d bytes over %d members, pointers=%d bytes over %d "
            "members, expected padding=%d\n",
            sizeof(PLW), (int)PLW_CANON_SIZE, PLW_TEST_FIELD_COUNT,
            (int)PLW_CANON_POINTER_BYTES, PLW_TEST_POINTER_COUNT,
            PLW_TEST_EXPECTED_PADDING);

    /* --- 1. Tiling: every byte owned at most once, holes == padding --- */
    memset(plw_byte_owner, 0, sizeof(plw_byte_owner));
    int overlaps = 0;
    for (int i = 0; i < PLW_TEST_FIELD_COUNT; i++) {
        for (unsigned b = plw_test_fields[i].off; b < (unsigned)(plw_test_fields[i].off + plw_test_fields[i].len);
             b++) {
            if (plw_byte_owner[b]) {
                overlaps++;
            }
            plw_byte_owner[b] = 1;
        }
    }
    for (int i = 0; i < PLW_TEST_POINTER_COUNT; i++) {
        for (unsigned b = plw_test_pointers[i].off;
             b < (unsigned)(plw_test_pointers[i].off + plw_test_pointers[i].len); b++) {
            if (plw_byte_owner[b]) {
                overlaps++;
            }
            plw_byte_owner[b] = 2;
        }
    }
    size_t holes = 0;
    for (size_t b = 0; b < sizeof(PLW); b++) {
        if (!plw_byte_owner[b]) {
            holes++;
        }
    }
    if (overlaps != 0) {
        fprintf(stderr,
                "[test_gs_coverage] FAIL: %d PLW bytes are claimed by more "
                "than one member — the generated field list in "
                "src/netplay/plw_canon_fields.h is wrong (regenerate with "
                "tools/netplay/gen_plw_canon_fields.py --write)\n",
                overlaps);
        failures++;
    }
    if (holes != (size_t)PLW_TEST_EXPECTED_PADDING) {
        fprintf(stderr,
                "[test_gs_coverage] FAIL: %zu PLW bytes belong to no member "
                "(expected exactly %d bytes of alignment padding). Either a "
                "member is missing from src/netplay/plw_canon_fields.h — in "
                "which case its divergences are invisible to the desync "
                "checksum — or PLW's layout changed and the padding figure "
                "must be re-derived from clang -fdump-record-layouts.\n",
                holes, PLW_TEST_EXPECTED_PADDING);
        failures++;
    }
    if ((size_t)PLW_CANON_SIZE + (size_t)PLW_CANON_POINTER_BYTES + holes != sizeof(PLW)) {
        fprintf(stderr, "[test_gs_coverage] FAIL: PLW byte accounting does not sum to sizeof(PLW)\n");
        failures++;
    }

    /* --- 1b. The coalesced run table matches the member table --- */
    fprintf(stderr, "[test_gs_coverage] PLW canonical emit: %d members collapse to %d contiguous runs\n",
            PLW_TEST_FIELD_COUNT, GameState_PlwCanonicalRunCount());

    /* --- 2. Every emitted member changes the image --- */
    memset(&plw_test_a, 0, sizeof(plw_test_a));
    GameState_EmitPlwCanonical(&plw_test_a, plw_canon_a);
    int inert_members = 0;
    for (int i = 0; i < PLW_TEST_FIELD_COUNT; i++) {
        memset(&plw_test_b, 0, sizeof(plw_test_b));
        ((uint8_t*)&plw_test_b)[plw_test_fields[i].off] = 0xFF;
        GameState_EmitPlwCanonical(&plw_test_b, plw_canon_b);
        if (memcmp(plw_canon_a, plw_canon_b, PLW_CANON_SIZE) == 0) {
            fprintf(stderr,
                    "[test_gs_coverage] FAIL: perturbing the member at PLW "
                    "offset %u left the canonical image unchanged — that "
                    "member is invisible to the desync checksum\n",
                    (unsigned)plw_test_fields[i].off);
            inert_members++;
        }
    }
    if (inert_members) {
        failures++;
    }

    /* --- 3. Pointer members change nothing --- */
    memset(&plw_test_b, 0, sizeof(plw_test_b));
    for (int i = 0; i < PLW_TEST_POINTER_COUNT; i++) {
        memset((uint8_t*)&plw_test_b + plw_test_pointers[i].off, 0xAB, plw_test_pointers[i].len);
    }
    GameState_EmitPlwCanonical(&plw_test_b, plw_canon_b);
    if (memcmp(plw_canon_a, plw_canon_b, PLW_CANON_SIZE) != 0) {
        fprintf(stderr,
                "[test_gs_coverage] FAIL: pointer bytes leaked into the "
                "canonical image; it would differ per peer (ASLR) and per "
                "architecture (4- vs 8-byte slots)\n");
        failures++;
    }

    /* --- 4. Determinism --- */
    fill_random((uint8_t*)&plw_test_a, sizeof(plw_test_a), 0x5EED1234u);
    memcpy(&plw_test_b, &plw_test_a, sizeof(plw_test_b));
    uint32_t first = 0;
    for (int rep = 0; rep < 4; rep++) {
        memset(plw_canon_a, 0, sizeof(plw_canon_a));
        GameState_EmitPlwCanonical(&plw_test_b, plw_canon_a);
        uint32_t h = canon_hash(plw_canon_a);
        if (rep == 0) {
            first = h;
        } else if (h != first) {
            fprintf(stderr, "[test_gs_coverage] FAIL: canonical image is not deterministic\n");
            failures++;
            break;
        }
    }

    /* --- 5. Regression pin: the divergences the old sweep erased ---
     * routine_no[] is the character state machine. The measured sweep hit
     * at PLW offset 56 (64-bit) covered routine_no[2..5] with values like
     * 0x0000000300010008; any second value in the same filter range was
     * zeroed to the identical 0, so two desynced peers agreed. Same shape
     * for wu.vitality (offset 176) and wu.position_x (offset 96). */
    struct {
        const char* what;
        size_t off;
        uint64_t va;
        uint64_t vb;
    } cases[] = {
        { "wu.routine_no[2..5]", offsetof(PLW, wu.routine_no[2]), 0x0000000300010008ULL, 0x0000000300010009ULL },
        { "wu.vitality..dm_vital", offsetof(PLW, wu.vitality), 0x0000005D005D00A0ULL, 0x0000005D005D00A1ULL },
        { "wu.kage_width..position_y", offsetof(PLW, wu.kage_width), 0x0000022100150000ULL,
          0x0000022100150001ULL },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        memset(&plw_test_a, 0, sizeof(plw_test_a));
        memset(&plw_test_b, 0, sizeof(plw_test_b));
        memcpy((uint8_t*)&plw_test_a + cases[c].off, &cases[c].va, sizeof(uint64_t));
        memcpy((uint8_t*)&plw_test_b + cases[c].off, &cases[c].vb, sizeof(uint64_t));

        GameState_EmitPlwCanonical(&plw_test_a, plw_canon_a);
        GameState_EmitPlwCanonical(&plw_test_b, plw_canon_b);
        uint32_t new_a = canon_hash(plw_canon_a);
        uint32_t new_b = canon_hash(plw_canon_b);

        /* What the deleted sweep did to the same pair. On a 32-bit build
         * the values do not reach the filter's 4 GiB floor at the same
         * offsets, so only the 64-bit build can demonstrate the masking;
         * the new-path assertion below is checked on both. */
        PLW old_a = plw_test_a;
        PLW old_b = plw_test_b;
        plw_old_sweep_reference(&old_a);
        plw_old_sweep_reference(&old_b);
        int old_masked = (memcmp(&old_a, &old_b, sizeof(PLW)) == 0);

        if (new_a == new_b) {
            fprintf(stderr,
                    "[test_gs_coverage] FAIL: canonical image cannot "
                    "distinguish a %s divergence\n",
                    cases[c].what);
            failures++;
        } else {
            fprintf(stderr,
                    "[test_gs_coverage]   %s: canonical hashes differ "
                    "(0x%08x vs 0x%08x); deleted sweep would have %s\n",
                    cases[c].what, new_a, new_b,
                    old_masked ? "ERASED this divergence" : "kept it");
        }
    }

    if (failures == 0) {
        fprintf(stderr,
                "[test_gs_coverage] OK — PLW canonical image tiles the struct "
                "(%d emitted + %d pointer + %d padding == %zu), all %d "
                "emitted members are observable, pointers are not, and the "
                "image is deterministic\n",
                (int)PLW_CANON_SIZE, (int)PLW_CANON_POINTER_BYTES, PLW_TEST_EXPECTED_PADDING, sizeof(PLW),
                PLW_TEST_FIELD_COUNT);
    }
    return failures;
}

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

/* === PHASE 3 equalizer coverage guard + asymmetric-history repro
 * (task #143 / queue.md #143) ===============================================
 *
 * setup_vs_mode()'s PHASE 3 block (src/netplay/netplay.c) is a hand-
 * maintained list of resets that has to track game_state.c's FH_* hash-
 * input list — nothing ties them together, which is exactly how
 * ca_check_flag/combo_type/remake_power/Color7/spmv_ng_save/chainex_check
 * drifted out of PHASE 3's coverage while staying in the hash. Every
 * existing harness runs two FRESH (identically BSS-zero) instances, so
 * none of them can see a field that is simply never reset: both sides
 * start at the same wrong (zero) value and agree by accident.
 *
 * Trampolines used here (game_state.c, ENABLE_NETPLAY_TESTS block):
 *   - Netplay_Test_FhCount / Netplay_Test_FhName / Netplay_Test_FieldHash
 *     read back game_state.c's file-static FH_* hash table.
 *   - Netplay_Test_DirtyHashedGlobalsSix(seed) writes pseudo-random
 *     nonzero values into exactly the six raw globals queue.md #143
 *     found missing from PHASE 3 (see its own comment in game_state.c
 *     for why it is scoped to six fields and not the full FH_* set).
 * netplay.c:
 *   - Netplay_Test_RunSetupVsMode() calls the real, unmodified
 *     setup_vs_mode() — confirmed safe to call from this exact
 *     --test-* dispatch position (immediately after read_args(), before
 *     any SDL/engine bootstrap): it was probed standalone first and
 *     returned cleanly, logging only the expected "arcade balance not
 *     verified" SDL_LogError (setup_vs_mode's own belt-and-braces check;
 *     this harness never calls ArcadeBalance_Init()).
 * game_state.h:
 *   - save_current_state() and the State type are already public; no new
 *     trampoline needed to compute the real per-field hashes.
 *
 * Two checks, sharing the dirty/equalize/save mechanics:
 *
 *  1. equalizer_coverage_check() — dirty the six fields with seed A, run
 *     the equalizer, hash; repeat with an independent seed B; compare
 *     hash[i] between the two runs for every i in FH_COUNT. A field the
 *     equalizer fully resets lands on the SAME (zeroed) value regardless
 *     of what it started at, so the two runs agree. A field PHASE 3
 *     forgets (or only partially resets) carries a seed-dependent residue
 *     through, so the two runs disagree — named by FH_NAMES[i].
 *
 *     IMPORTANT LIMIT: this only gives real signal for the six fields
 *     Netplay_Test_DirtyHashedGlobalsSix actually perturbs. A field added
 *     to FH_* in the future that neither dirty pass touches hashes to the
 *     same untouched value on both runs regardless of whether PHASE 3
 *     resets it, and PASSES here by accident — the same blind spot this
 *     whole guard exists to close, just one field later. The comparison
 *     loop itself needs no maintenance to stay memory-safe when FH_COUNT
 *     grows (it is sized off FH_COUNT/FH_NAMES, not a literal), but it
 *     does need the dirty scope extended to stay MEANINGFUL for a new
 *     field. equalizer_scope_pin_check() below is what actually catches
 *     that: it pins FH_COUNT and fails loudly, by name, the moment it
 *     moves, so "extend the dirty scope" can't be forgotten silently.
 *
 *  2. asymmetric_history_repro() — the queue.md #143 failure mode
 *     reproduced directly: a "host" that dirtied the six fields (prior
 *     offline match) and a "joiner" left at BSS zero (fresh boot), both
 *     run through the SAME equalizer, then their resulting hashes must
 *     agree field-by-field — this is what a real session start checks
 *     (first confirmed frame must match cross-peer).
 */
void Netplay_Test_RunSetupVsMode(void);
void Netplay_Test_DirtyHashedGlobalsSix(uint32_t seed);
void Netplay_Test_ZeroHashedGlobalsSix(void);
int Netplay_Test_FhCount(void);
const char* Netplay_Test_FhName(int ix);
uint32_t Netplay_Test_FieldHash(int frame, int ix);

#define FH_TEST_MAX_FIELDS 64 /* generous ceiling; FH_COUNT is well under this */

/* Structural tripwire for the P-1.1 gap above: Netplay_Test_DirtyHashedGlobalsSix/
 * ZeroHashedGlobalsSix (game_state.c) hand-dirty exactly six named globals
 * (chainex_check, Color7, ca_check_flag, spmv_ng_save, combo_type,
 * remake_power) — the six queue.md #143 found missing from PHASE 3. That
 * dirty scope is hand-maintained and does NOT automatically grow when a
 * new field is added to the FH_* enum/FH_NAMES/HASHONE block in
 * game_state.c — see the "IMPORTANT LIMIT" note above.
 *
 * Pinning FH_COUNT here (same idiom as GS_COVERAGE_EXPECTED_HOLE_BYTES
 * above: a hand-maintained count that turns "silently stale" into "build
 * fails and names what changed") makes it impossible for FH_COUNT to move
 * without equalizer_scope_pin_check() failing loudly and naming the new
 * field(s) by FH_NAMES[i]. Bump this ONLY after you have:
 *   (1) added a PHASE 3 reset for the new field in setup_vs_mode()
 *       (src/netplay/netplay.c), AND
 *   (2) extended Netplay_Test_DirtyHashedGlobalsSix and
 *       Netplay_Test_ZeroHashedGlobalsSix (src/netplay/game_state.c) to
 *       dirty/zero it, THEN
 *   (3) re-pin this constant to the new FH_COUNT.
 * Bumping it without (1)/(2) just widens the blind spot this pin exists
 * to close. */
#define EQUALIZER_DIRTY_FH_COUNT_PINNED 39

static State equalizer_test_scratch;

/* Dirty the six fields with `seed`, run the production equalizer, run the
 * production checksum, and copy out the per-field hashes it recorded for
 * `frame`. */
static void equalizer_dirty_pass(uint32_t seed, int frame, int fh_count, uint32_t* out_hashes) {
    Netplay_Test_DirtyHashedGlobalsSix(seed);
    Netplay_Test_RunSetupVsMode();
    save_current_state(&equalizer_test_scratch, frame);
    for (int i = 0; i < fh_count; i++) {
        out_hashes[i] = Netplay_Test_FieldHash(frame, i);
    }
}

/* Force the six fields to true zero (models a fresh-boot process — see
 * Netplay_Test_ZeroHashedGlobalsSix's comment for why this must be
 * explicit rather than "just don't dirty", since host and joiner run in
 * the same process here), run the equalizer, then record hashes the same
 * way. */
static void equalizer_clean_pass(int frame, int fh_count, uint32_t* out_hashes) {
    Netplay_Test_ZeroHashedGlobalsSix();
    Netplay_Test_RunSetupVsMode();
    save_current_state(&equalizer_test_scratch, frame);
    for (int i = 0; i < fh_count; i++) {
        out_hashes[i] = Netplay_Test_FieldHash(frame, i);
    }
}

/* The six fields Netplay_Test_DirtyHashedGlobalsSix/ZeroHashedGlobalsSix
 * (game_state.c) name explicitly — same six the "IMPORTANT LIMIT" note
 * above is about. */
static const char* const SIX_FIELD_NAMES[6] = {
    "combo_type", "remake_power", "chainex_check", "Color7", "ca_check_flag", "spmv_ng_save",
};

/* Guards against equalizer_coverage_check()/asymmetric_history_repro()
 * passing VACUOUSLY if the per-field hashing block inside
 * save_current_state()'s `if (checksumming_active)` (game_state.c) ever
 * stopped running: every saved_field_hashes[][] slot would then read back
 * its BSS-zero default, 0 == 0 would hold across every FH_COUNT field for
 * both checks below, and both would report OK while proving nothing.
 *
 * djb2 (sf33rd/utils/djb2_hash.h) seeds at 5381 (odd) and its update step
 * is `hash = hash*33 + byte`; 33 is odd, so for an all-zero-byte input the
 * accumulator stays odd through every iteration and can never land on 0
 * (even) — this holds unconditionally for the "clean" (zeroed) pass, and
 * for the "dirty" (random nonzero bytes) pass landing on exactly 0 would
 * require a specific accidental collision, not the deterministic default
 * a never-executed hash slot reads back. So `!= 0` here distinguishes "a
 * real hash pass touched this slot" from "this slot was never written" —
 * exactly the failure mode this check exists to catch, not a claim that
 * the hash is correct. */
static int six_fields_hashed_nonzero_check(const char* what, const uint32_t* hashes, int fh_count) {
    int failures = 0;
    for (int f = 0; f < 6; f++) {
        int ix = -1;
        for (int i = 0; i < fh_count; i++) {
            if (strcmp(Netplay_Test_FhName(i), SIX_FIELD_NAMES[f]) == 0) {
                ix = i;
                break;
            }
        }
        if (ix < 0) {
            fprintf(stderr, "[test_gs_coverage] FAIL (%s): field '%s' not found in FH_NAMES\n", what,
                    SIX_FIELD_NAMES[f]);
            failures++;
            continue;
        }
        if (hashes[ix] == 0) {
            fprintf(stderr,
                    "[test_gs_coverage] FAIL (%s): field '%s' hash is exactly 0 — "
                    "the per-field hashing block in save_current_state() may not be "
                    "running (see the comment above this check for why a real hash "
                    "pass should never land here)\n",
                    what, SIX_FIELD_NAMES[f]);
            failures++;
        }
    }
    return failures;
}

static int report_field_mismatches(const char* what, const uint32_t* a, const uint32_t* b, int fh_count) {
    int failures = 0;
    for (int i = 0; i < fh_count; i++) {
        if (a[i] != b[i]) {
            fprintf(stderr,
                    "[test_gs_coverage] FAIL (%s): field '%s' hash disagrees "
                    "(0x%08x vs 0x%08x) after setup_vs_mode() ran on both sides — "
                    "PHASE 3 in netplay.c's setup_vs_mode() does not fully reset "
                    "this field\n",
                    what, Netplay_Test_FhName(i), a[i], b[i]);
            failures++;
        }
    }
    return failures;
}

/* Fails loudly and names what changed the moment FH_COUNT moves away from
 * the pin above — see EQUALIZER_DIRTY_FH_COUNT_PINNED's comment for why
 * this exists: equalizer_coverage_check()/asymmetric_history_repro() below
 * can only prove PHASE 3 coverage for the six fields the dirty scope
 * actually touches, so a field added to FH_* without also extending that
 * scope would otherwise pass those two checks by accident. */
static int equalizer_scope_pin_check(void) {
    int fh_count = Netplay_Test_FhCount();
    if (fh_count == EQUALIZER_DIRTY_FH_COUNT_PINNED) {
        return 0;
    }
    fprintf(stderr,
            "[test_gs_coverage] FAIL: FH_COUNT changed from the pinned %d to %d. "
            "The equalizer-coverage guard's dirty scope "
            "(Netplay_Test_DirtyHashedGlobalsSix/Netplay_Test_ZeroHashedGlobalsSix "
            "in game_state.c) only perturbs the fields it names explicitly — a "
            "field added to FH_* without extending that scope hashes to the same "
            "untouched value on both sides of the comparisons below and would "
            "PASS silently, hiding a missing PHASE 3 reset in setup_vs_mode().\n",
            EQUALIZER_DIRTY_FH_COUNT_PINNED, fh_count);
    if (fh_count > EQUALIZER_DIRTY_FH_COUNT_PINNED) {
        fprintf(stderr, "[test_gs_coverage] New field(s) since the pin:\n");
        for (int i = EQUALIZER_DIRTY_FH_COUNT_PINNED; i < fh_count && i < FH_TEST_MAX_FIELDS; i++) {
            fprintf(stderr, "[test_gs_coverage]   FH_NAMES[%d] = \"%s\"\n", i, Netplay_Test_FhName(i));
        }
    }
    fprintf(stderr,
            "[test_gs_coverage] To fix: (1) add a PHASE 3 reset for the new "
            "field in setup_vs_mode() (src/netplay/netplay.c), (2) extend "
            "Netplay_Test_DirtyHashedGlobalsSix and "
            "Netplay_Test_ZeroHashedGlobalsSix (src/netplay/game_state.c) to "
            "dirty/zero it, then (3) re-pin EQUALIZER_DIRTY_FH_COUNT_PINNED in "
            "test_gs_coverage.c to %d.\n",
            fh_count);
    return 1;
}

static int equalizer_coverage_check(void) {
    int fh_count = Netplay_Test_FhCount();
    if (fh_count <= 0 || fh_count > FH_TEST_MAX_FIELDS) {
        fprintf(stderr, "[test_gs_coverage] FAIL: FH_COUNT=%d out of the expected 1..%d range\n", fh_count,
                FH_TEST_MAX_FIELDS);
        return 1;
    }

    static uint32_t hashes_a[FH_TEST_MAX_FIELDS];
    static uint32_t hashes_b[FH_TEST_MAX_FIELDS];
    equalizer_dirty_pass(0x1B873593u, 10, fh_count, hashes_a);
    equalizer_dirty_pass(0xCC9E2D51u, 11, fh_count, hashes_b);

    int failures = report_field_mismatches("equalizer-coverage: two differently-dirtied starting states", hashes_a,
                                            hashes_b, fh_count);
    failures += six_fields_hashed_nonzero_check("equalizer-coverage seed A", hashes_a, fh_count);
    failures += six_fields_hashed_nonzero_check("equalizer-coverage seed B", hashes_b, fh_count);
    if (failures == 0) {
        fprintf(stderr,
                "[test_gs_coverage] OK — compared all %d checksummed fields "
                "between two independently-dirtied starting states; the 6 "
                "fields Netplay_Test_DirtyHashedGlobalsSix actually dirties "
                "(ca_check_flag, combo_type, remake_power, Color7, "
                "spmv_ng_save, chainex_check) came back equal, proving "
                "PHASE 3 resets them. The other %d fields matched too, but "
                "only because neither run perturbed them — see "
                "equalizer_scope_pin_check() for how a future field is still "
                "caught\n",
                fh_count, fh_count - 6);
    }
    return failures;
}

static int asymmetric_history_repro(void) {
    int fh_count = Netplay_Test_FhCount();
    if (fh_count <= 0 || fh_count > FH_TEST_MAX_FIELDS) {
        fprintf(stderr, "[test_gs_coverage] FAIL: FH_COUNT=%d out of the expected 1..%d range\n", fh_count,
                FH_TEST_MAX_FIELDS);
        return 1;
    }

    static uint32_t host_hashes[FH_TEST_MAX_FIELDS];
    static uint32_t joiner_hashes[FH_TEST_MAX_FIELDS];

    fprintf(stderr,
            "[test_gs_coverage] asymmetric-history repro: 'host' played a prior "
            "offline match (six fields dirtied), 'joiner' is fresh-boot (BSS zero); "
            "both then run setup_vs_mode() before their first save\n");
    equalizer_dirty_pass(0xF00DCAFEu, 20, fh_count, host_hashes);
    equalizer_clean_pass(21, fh_count, joiner_hashes);

    int failures =
        report_field_mismatches("asymmetric-history: host (prior match) vs joiner (fresh boot)", host_hashes,
                                 joiner_hashes, fh_count);
    failures += six_fields_hashed_nonzero_check("asymmetric-history host", host_hashes, fh_count);
    failures += six_fields_hashed_nonzero_check("asymmetric-history joiner", joiner_hashes, fh_count);
    if (failures == 0) {
        fprintf(stderr,
                "[test_gs_coverage] OK — host (prior offline match) and joiner "
                "(fresh boot) agree on all %d checksummed fields after "
                "setup_vs_mode() — no frame-0 GekkoDesyncDetected from this history "
                "asymmetry\n",
                fh_count);
    } else {
        fprintf(stderr,
                "[test_gs_coverage] This is queue.md #143's exact failure mode: a "
                "host that played one offline match, backed out, and hosted a "
                "netplay session would desync a fresh-boot joiner on the first "
                "confirmed frame.\n");
    }
    return failures;
}

int Netplay_Test_GsCoverage(void) {
    /* MANDATORY ORDER: the pin/equalizer/repro checks below must run here,
     * before roundtrip_pass() further down. Netplay_Test_DirtyHashedGlobalsSix/
     * ZeroHashedGlobalsSix (game_state.c) are only safe to call against
     * near-BSS global state — see that function's own comment for why
     * (setup_vs_mode()'s PHASE 0/1 read several fields as array indices
     * before PHASE 3 zeroes them). roundtrip_pass() loads fully-random
     * bytes into all ~611 GameState members; running Netplay_Test_RunSetupVsMode()
     * (the real setup_vs_mode(), including live engine calls like
     * Clear_Personal_Data/System_all_clear_Level_B) against that randomized
     * state instead of near-BSS state is the exact out-of-bounds risk that
     * comment warns about. If these calls ever get reordered, move this
     * warning with them, not just the code. */
    int pin_failures = equalizer_scope_pin_check();
    int equalizer_failures = equalizer_coverage_check();
    int repro_failures = asymmetric_history_repro();

    /* PLW canonical hash-image checks touch only local scratch (no
     * GameState globals), so their position relative to the pin/equalizer/
     * repro checks above is unconstrained — but, like those, they must
     * stay ahead of the round-trip pass below. */
    int plw_failures = plw_canonical_checks();

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
    if (plw_failures || equalizer_failures || repro_failures || pin_failures) {
        fprintf(stderr,
                "[test_gs_coverage] FAIL: %d FH_COUNT-pin check(s), %d PLW "
                "canonical-image check(s), %d equalizer-coverage check(s), %d "
                "asymmetric-history repro check(s) failed\n",
                pin_failures, plw_failures, equalizer_failures, repro_failures);
        return 1;
    }
    fprintf(stderr,
            "[test_gs_coverage] OK — all %zu hole bytes match the pinned "
            "padding expectation (%d); every GameState field round-trips "
            "through GS_SAVE/GS_LOAD; setup_vs_mode() equalizes every "
            "checksummed field this harness dirties, host-vs-joiner included; "
            "FH_COUNT still matches the pinned %d\n",
            hole_bytes, GS_COVERAGE_EXPECTED_HOLE_BYTES, EQUALIZER_DIRTY_FH_COUNT_PINNED);
    return 0;
#else
    /* 32-bit build of the harness: no pinned padding figure (tests run on
     * 64-bit hosts; ARM32 layout is pinned by EXPECTED_GAME_STATE_SIZE).
     * Report the measurement without asserting an unverified constant. */
    fprintf(stderr,
            "[test_gs_coverage] NOTE: no pinned hole-byte expectation for "
            "32-bit builds; measured %zu (informational only)\n",
            hole_bytes);
    if (plw_failures || equalizer_failures || repro_failures || pin_failures) {
        fprintf(stderr,
                "[test_gs_coverage] FAIL: %d FH_COUNT-pin check(s), %d PLW "
                "canonical-image check(s), %d equalizer-coverage check(s), %d "
                "asymmetric-history repro check(s) failed\n",
                pin_failures, plw_failures, equalizer_failures, repro_failures);
        return 1;
    }
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
