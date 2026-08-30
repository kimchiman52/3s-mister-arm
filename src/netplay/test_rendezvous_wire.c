/*
 * test_rendezvous_wire.c — task #122, JOB 2: the unit guard that would
 * have caught the dead NACK.
 *
 * WHAT WENT WRONG, AND WHY THIS FILE IS THE CHEAPEST FIX. eaf72865
 * shipped the server half of the typed NACK — nine reasons, bounded
 * amplification, 36 protocol tests — and Rendezvous_ParseNack /
 * Rendezvous_NackReasonText on the client to read it. Neither had a
 * single caller: not in production, not in a test. A valid v2 NACK
 * matched no branch in the joiner race loop and was destroyed with the
 * datagram, so every reason still collapsed to
 * P2P_FAIL_RENDEZVOUS_DOWN. `Rendezvous_ParseNack` was pure, non-static
 * and linkable the whole time. One test would have shown that the
 * function nothing calls also parses nothing.
 *
 * So this TU tests the pure rendezvous wire surface AND the verdict each
 * reason produces — because "it parses" was never the property at risk.
 * The end-to-end half (a real socket, a real race, a real NACK) lives in
 * test_connect_observability.c; the induction of each reason from real
 * server conditions lives in tools/rendezvous-server/__test_protocol.js
 * (testNackPerReason). This file is the middle link: reason byte ->
 * verdict.
 *
 * HOW IT CANNOT PASS VACUOUSLY. Three independent devices, each aimed at
 * a failure mode this tree has actually produced:
 *
 *   1. EXPECTED_TESTS — a LITERAL, deliberately not a count of the
 *      registry, so a test that is removed or never registered fails the
 *      run instead of vanishing from it. Lifted from
 *      tools/rendezvous-server/__test_protocol.js:2411.
 *   2. EXPECTED_MIN_CHECKS — an assertion-count floor. A harness with
 *      only a test-count guard still passes when every test body is
 *      compiled out or early-returns; a body that runs no assertions
 *      cannot clear this.
 *   3. Per-sweep case floors — the nine-reason sweep asserts it visited
 *      exactly nine cases, so a table that loses a row is a failure
 *      rather than a smaller (still green) run.
 *
 * And the stub below says "not compiled in", spelled exactly that way,
 * because tools/gates/run-gates.sh greps for that phrase to turn an
 * exit-2 misbuild into a RED gate. test_sparse_effect_save.c said "not
 * compiled with" and evaded it for its whole life.
 *
 * NO -DNETPLAY_TEST_HOOKS NEEDED, and no #error asking for one. Every
 * symbol here comes from rendezvous.c and connect_fail.c, both of which
 * are in the shipped build unconditionally; the DirectP2P_TestHook_*
 * seams that force test_bilateral_punch.c:51-53 to demand the flag are
 * not used. Adding the #error anyway would be a false requirement.
 *
 * Gated behind ENABLE_NETPLAY_TESTS. Enable with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS='-DENABLE_NETPLAY_TESTS -DNETPLAY_TEST_HOOKS'"
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/connect_fail.h"
#include "netplay/rendezvous.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* --- counters: the anti-vacuous machinery ------------------------------ */

static int fail_count = 0;
static int tests_run = 0;
static int checks_run = 0;

/* A LITERAL, never SDL_arraysize(TESTS) — see the header comment. */
#define EXPECTED_TESTS 9

/* Assertion floor. Not an exact count (that would make every edit a
 * two-line edit and invite the habit of "just bump the number"), but far
 * above what any subset of the bodies could produce, so a harness whose
 * cases stopped executing cannot clear it. The real figure at the time of
 * writing is printed in the summary; keep this comfortably below it and
 * comfortably above zero. */
#define EXPECTED_MIN_CHECKS 200

static void check(const char* tag, bool ok, const char* what) {
    checks_run++;
    if (!ok) {
        fail_count++;
        fprintf(stderr, "[test_rendezvous_wire] FAIL: %s: %s\n", tag, what);
    }
}

#define CHECK(tag, cond) check((tag), (cond), #cond)

static void check_eq_int(const char* tag, long got, long want, const char* what) {
    checks_run++;
    if (got != want) {
        fail_count++;
        fprintf(stderr, "[test_rendezvous_wire] FAIL: %s: %s: got %ld, want %ld\n",
                tag, what, got, want);
    }
}

static void check_eq_str(const char* tag, const char* got, const char* want,
                         const char* what) {
    checks_run++;
    if (got == NULL || strcmp(got, want) != 0) {
        fail_count++;
        fprintf(stderr, "[test_rendezvous_wire] FAIL: %s: %s: got \"%s\", want \"%s\"\n",
                tag, what, (got != NULL) ? got : "(null)", want);
    }
}

/* --- wire helpers ------------------------------------------------------
 *
 * Frames are built BYTE BY BYTE here rather than through any encoder in
 * the tree, and that is the point: rendezvous.c has no NACK encoder (only
 * the server emits one), so a helper that shared code with the parser
 * would be testing the parser against itself. These bytes mirror
 * encodeNack() in tools/rendezvous-server/rendezvous-server.js:693-705 —
 * magic, version, type, reason, one reserved byte, the sender's own
 * 16-byte session key at [8..24], and a zero 4-byte tail. */

static const uint8_t k_key[16] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00
};

static const uint8_t k_other_key[16] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01 /* one bit from k_key */
};

/* Writes a well-formed NACK of `len` bytes into `out` (len >= 28 for a
 * frame the parser should accept; smaller lengths are the truncation
 * cases). Everything the caller may want wrong is a parameter. */
static void build_nack(uint8_t* out, size_t cap, uint8_t version, uint8_t type,
                       uint8_t reason, const uint8_t key[16]) {
    memset(out, 0, cap);
    out[0] = 0x33; /* '3' */
    out[1] = 0x53; /* 'S' */
    out[2] = 0x58; /* 'X' */
    out[3] = 0x52; /* 'R' */
    out[4] = version;
    out[5] = type;
    out[6] = reason;
    out[7] = 0;
    if (cap >= 24 && key != NULL) {
        memcpy(&out[8], key, 16);
    }
}

/* The canonical, everything-correct NACK for `reason`. */
static void build_good_nack(uint8_t* out, size_t cap, uint8_t reason) {
    build_nack(out, cap, (uint8_t)Rendezvous_WireVersion(),
               (uint8_t)REND_FRAME_NACK, reason, k_key);
}

/* --- the reason table --------------------------------------------------
 *
 * Hand-written, NOT derived from the enum: a table generated from the
 * thing under test proves nothing. Every column is the value a REAL
 * counterpart produces — the wire number the server sends
 * (rendezvous-server.js:484-492), the log anchor
 * Rendezvous_NackReasonText must keep stable, and the verdict the user
 * is shown. */
typedef struct {
    uint8_t         reason;
    const char*     text;
    ConnectFailCode verdict;
} ReasonRow;

static const ReasonRow k_reasons[] = {
    { 1, "BAD_VERSION",  CONNECT_FAIL_RENDEZVOUS_BADFRAME },
    { 2, "BAD_LENGTH",   CONNECT_FAIL_RENDEZVOUS_BADFRAME },
    { 3, "BAD_TYPE",     CONNECT_FAIL_RENDEZVOUS_BADFRAME },
    { 4, "RATE_IP",      CONNECT_FAIL_RENDEZVOUS_BUSY },
    { 5, "RATE_KEY",     CONNECT_FAIL_RENDEZVOUS_BUSY },
    { 6, "RATE_PREGATE", CONNECT_FAIL_RENDEZVOUS_BUSY },
    { 7, "KEY_QUOTA",    CONNECT_FAIL_RENDEZVOUS_BUSY },
    { 8, "TABLE_FULL",   CONNECT_FAIL_RENDEZVOUS_TABLE_FULL },
    { 9, "SESSION_FULL", CONNECT_FAIL_RENDEZVOUS_ROOM_FULL },
};
#define REASON_ROWS 9

/* ======================================================================
 * Test 1 — every reason byte round-trips through the parser
 * ====================================================================== */

static void test1_reason_roundtrip(void) {
    const char* tag = "1-reason-roundtrip";
    tests_run++;
    int cases = 0;

    /* The literal values are pinned FIRST, against the enum, because the
     * whole reason the table above is hand-written is that the wire
     * numbers are shared with a server that deploys separately. A
     * renumber here is a silent misattribution, not a build error. */
    check_eq_int(tag, REND_NACK_BAD_VERSION, 1, "REND_NACK_BAD_VERSION");
    check_eq_int(tag, REND_NACK_BAD_LENGTH, 2, "REND_NACK_BAD_LENGTH");
    check_eq_int(tag, REND_NACK_BAD_TYPE, 3, "REND_NACK_BAD_TYPE");
    check_eq_int(tag, REND_NACK_RATE_IP, 4, "REND_NACK_RATE_IP");
    check_eq_int(tag, REND_NACK_RATE_KEY, 5, "REND_NACK_RATE_KEY");
    check_eq_int(tag, REND_NACK_RATE_PREGATE, 6, "REND_NACK_RATE_PREGATE");
    check_eq_int(tag, REND_NACK_KEY_QUOTA, 7, "REND_NACK_KEY_QUOTA");
    check_eq_int(tag, REND_NACK_TABLE_FULL, 8, "REND_NACK_TABLE_FULL");
    check_eq_int(tag, REND_NACK_SESSION_FULL, 9, "REND_NACK_SESSION_FULL");
    check_eq_int(tag, REND_NACK_NONE, 0, "REND_NACK_NONE stays reserved");
    check_eq_int(tag, REND_FRAME_NACK, 5, "REND_FRAME_NACK wire type");
    check_eq_int(tag, REND_NACK_LEN, 28, "REND_NACK_LEN wire length");

    for (int i = 0; i < REASON_ROWS; i++) {
        uint8_t pkt[REND_NACK_LEN];
        build_good_nack(pkt, sizeof(pkt), k_reasons[i].reason);

        /* The out-parameter is pre-poisoned so "the parser wrote it" and
         * "the parser left it alone" are distinguishable. */
        uint8_t reason = 0xAB;
        const bool ok = Rendezvous_ParseNack(pkt, (int)sizeof(pkt), k_key, &reason);
        check(tag, ok, "a well-formed NACK parses");
        check_eq_int(tag, reason, k_reasons[i].reason, "the reason byte round-trips");
        cases++;
    }
    /* SWEEP FLOOR. A table that loses a row would otherwise make this
     * test smaller and still green. */
    check_eq_int(tag, cases, REASON_ROWS, "every reason row was exercised");
}

/* ======================================================================
 * Test 2 — malformed frames are refused, and the out-param is cleared
 * ====================================================================== */

static void test2_reject_malformed(void) {
    const char* tag = "2-reject-malformed";
    tests_run++;

    uint8_t pkt[64];
    uint8_t reason;

    /* --- truncation. REND_NACK_LEN is a MINIMUM, so 27 is the last
     * length that must be refused and 28 the first that must not. This
     * pair is the boundary itself, not a sample near it. */
    build_good_nack(pkt, sizeof(pkt), REND_NACK_SESSION_FULL);
    reason = 0xAB;
    CHECK(tag, !Rendezvous_ParseNack(pkt, REND_NACK_LEN - 1, k_key, &reason));
    check_eq_int(tag, reason, REND_NACK_NONE,
                 "a refused frame clears the reason (never leaves the poison)");
    reason = 0xAB;
    CHECK(tag, Rendezvous_ParseNack(pkt, REND_NACK_LEN, k_key, &reason));
    check_eq_int(tag, reason, REND_NACK_SESSION_FULL, "the boundary length parses");

    /* Shorter truncations, including lengths that pass Rendezvous_HasMagic
     * (>= 4) and the FrameType minimum (>= 6): a runt must not become a
     * reason. */
    for (int len = 0; len <= 8; len++) {
        reason = 0xAB;
        CHECK(tag, !Rendezvous_ParseNack(pkt, len, k_key, &reason));
        check_eq_int(tag, reason, REND_NACK_NONE, "runt clears the reason");
    }

    /* --- OVERSIZE IS ACCEPTED, and that is deliberate, not an
     * oversight. The length test is `len < REND_NACK_LEN`, so a future
     * server that appends fields to the frame still parses here — which
     * is the only way an append-only wire can stay append-only. A test
     * asserting the opposite would freeze the format. */
    build_good_nack(pkt, sizeof(pkt), REND_NACK_TABLE_FULL);
    reason = 0xAB;
    CHECK(tag, Rendezvous_ParseNack(pkt, 40, k_key, &reason));
    check_eq_int(tag, reason, REND_NACK_TABLE_FULL, "an over-long NACK still parses");
    reason = 0xAB;
    CHECK(tag, Rendezvous_ParseNack(pkt, (int)sizeof(pkt), k_key, &reason));
    check_eq_int(tag, reason, REND_NACK_TABLE_FULL, "a 64-byte NACK still parses");

    /* --- wrong magic, one byte at a time. */
    for (int b = 0; b < 4; b++) {
        build_good_nack(pkt, sizeof(pkt), REND_NACK_RATE_IP);
        pkt[b] ^= 0xFFu;
        reason = 0xAB;
        CHECK(tag, !Rendezvous_ParseNack(pkt, REND_NACK_LEN, k_key, &reason));
        check_eq_int(tag, reason, REND_NACK_NONE, "wrong magic clears the reason");
    }

    /* --- wrong version. Both neighbours of ours, so the test says
     * "not our version" rather than "smaller than ours". */
    const int v = Rendezvous_WireVersion();
    build_nack(pkt, sizeof(pkt), (uint8_t)(v - 1), (uint8_t)REND_FRAME_NACK,
               REND_NACK_RATE_KEY, k_key);
    reason = 0xAB;
    CHECK(tag, !Rendezvous_ParseNack(pkt, REND_NACK_LEN, k_key, &reason));
    check_eq_int(tag, reason, REND_NACK_NONE, "older version clears the reason");
    build_nack(pkt, sizeof(pkt), (uint8_t)(v + 1), (uint8_t)REND_FRAME_NACK,
               REND_NACK_RATE_KEY, k_key);
    reason = 0xAB;
    CHECK(tag, !Rendezvous_ParseNack(pkt, REND_NACK_LEN, k_key, &reason));
    check_eq_int(tag, reason, REND_NACK_NONE, "newer version clears the reason");

    /* --- wrong type. Every other allocated type, plus the freed relay
     * range, must be refused by the NACK parser. */
    const uint8_t k_not_nack[] = { 0, 1, 2, 3, 4, 6, 7, 8, 255 };
    for (size_t i = 0; i < sizeof(k_not_nack); i++) {
        build_nack(pkt, sizeof(pkt), (uint8_t)v, k_not_nack[i],
                   REND_NACK_KEY_QUOTA, k_key);
        reason = 0xAB;
        CHECK(tag, !Rendezvous_ParseNack(pkt, REND_NACK_LEN, k_key, &reason));
        check_eq_int(tag, reason, REND_NACK_NONE, "wrong type clears the reason");
    }

    /* --- WRONG SESSION KEY. This is the gate that stops an off-path
     * sender choosing our diagnosis, so it gets the strongest form: a key
     * differing in ONE BIT is still refused. */
    build_good_nack(pkt, sizeof(pkt), REND_NACK_SESSION_FULL);
    reason = 0xAB;
    CHECK(tag, !Rendezvous_ParseNack(pkt, REND_NACK_LEN, k_other_key, &reason));
    check_eq_int(tag, reason, REND_NACK_NONE, "a one-bit key mismatch is refused");

    /* --- NULL arguments. */
    reason = 0xAB;
    CHECK(tag, !Rendezvous_ParseNack(NULL, REND_NACK_LEN, k_key, &reason));
    check_eq_int(tag, reason, REND_NACK_NONE, "NULL pkt clears the reason");
    reason = 0xAB;
    CHECK(tag, !Rendezvous_ParseNack(pkt, REND_NACK_LEN, NULL, &reason));
    check_eq_int(tag, reason, REND_NACK_NONE, "NULL key clears the reason");
    /* No out-param: must not crash and must refuse. */
    CHECK(tag, !Rendezvous_ParseNack(pkt, REND_NACK_LEN, k_key, NULL));
}

/* ======================================================================
 * Test 3 — an unknown reason is surfaced, never coerced onto a name
 * ====================================================================== */

static void test3_unknown_reason(void) {
    const char* tag = "3-unknown-reason";
    tests_run++;
    int cases = 0;

    /* rendezvous.h states the contract explicitly: the reason byte is NOT
     * range-checked, because a server newer than us may name a reason we
     * have no word for and dropping the frame (or renaming the reason)
     * would be worse than saying "unknown". */
    const uint8_t k_unknown[] = { 0, 10, 11, 42, 200, 254, 255 };
    for (size_t i = 0; i < sizeof(k_unknown); i++) {
        uint8_t pkt[REND_NACK_LEN];
        build_good_nack(pkt, sizeof(pkt), k_unknown[i]);
        uint8_t reason = 0xAB;
        CHECK(tag, Rendezvous_ParseNack(pkt, (int)sizeof(pkt), k_key, &reason));
        check_eq_int(tag, reason, k_unknown[i], "the raw byte reaches the caller");
        check_eq_str(tag, Rendezvous_NackReasonText(k_unknown[i]), "unknown",
                     "an unnamed reason renders as \"unknown\"");
        check_eq_int(tag, ConnectFail_ClassifyNackReason(k_unknown[i]),
                     CONNECT_FAIL_RENDEZVOUS_REFUSED,
                     "an unnamed reason maps to the honest catch-all");
        cases++;
    }
    check_eq_int(tag, cases, (int)sizeof(k_unknown), "every unknown case ran");

    /* And the catch-all is a REFUSAL, never a success: a bug that let
     * CONNECT_FAIL_NONE out of here would report a failed join as
     * connected. */
    for (int r = 0; r < 256; r++) {
        CHECK(tag, ConnectFail_ClassifyNackReason((uint8_t)r) != CONNECT_FAIL_NONE);
    }
}

/* ======================================================================
 * Test 4 — reason -> verdict, the mapping this whole task is about
 * ====================================================================== */

static void test4_reason_to_verdict(void) {
    const char* tag = "4-reason-to-verdict";
    tests_run++;
    int cases = 0;

    for (int i = 0; i < REASON_ROWS; i++) {
        const ReasonRow* row = &k_reasons[i];
        check_eq_int(tag, ConnectFail_ClassifyNackReason(row->reason), row->verdict,
                     row->text);
        check_eq_str(tag, Rendezvous_NackReasonText(row->reason), row->text,
                     "the log anchor is stable");
        /* Every verdict must be a real code with a real user string. */
        const char* mc = ConnectFail_Code(row->verdict);
        CHECK(tag, mc != NULL && mc[0] != '\0');
        CHECK(tag, strcmp(mc, "P2P_FAIL_UNKNOWN") != 0);
        const char* ut = ConnectFail_UserText(row->verdict);
        CHECK(tag, ut != NULL && ut[0] != '\0');
        CHECK(tag, strcmp(ut, "Connection failed.") != 0);
        /* The overlay status line is 384px wide and centres with
         * SSPutStrPro; connect_fail.h's contract is <= ~44 chars. */
        CHECK(tag, strlen(ut) <= 44);
        cases++;
    }
    check_eq_int(tag, cases, REASON_ROWS, "every reason row was mapped");

    /* THE ATTRIBUTION ARGUMENT, ASSERTED. The four rate reasons share one
     * verdict on purpose (none of them says WHO spent the budget); the
     * two "full" reasons do NOT, because a new room code fixes one and
     * cannot fix the other. Both halves are pinned so a later "tidy-up"
     * that merges or splits them has to argue with a test. */
    check_eq_int(tag, ConnectFail_ClassifyNackReason(REND_NACK_RATE_IP),
                 ConnectFail_ClassifyNackReason(REND_NACK_RATE_KEY),
                 "RATE_IP and RATE_KEY share the load verdict");
    check_eq_int(tag, ConnectFail_ClassifyNackReason(REND_NACK_RATE_PREGATE),
                 ConnectFail_ClassifyNackReason(REND_NACK_KEY_QUOTA),
                 "RATE_PREGATE and KEY_QUOTA share the load verdict");
    CHECK(tag, ConnectFail_ClassifyNackReason(REND_NACK_TABLE_FULL) !=
                   ConnectFail_ClassifyNackReason(REND_NACK_SESSION_FULL));
    /* ...and the two "full" strings must not read the same to a user
     * either, or the split is cosmetic. */
    CHECK(tag, strcmp(ConnectFail_UserText(CONNECT_FAIL_RENDEZVOUS_TABLE_FULL),
                      ConnectFail_UserText(CONNECT_FAIL_RENDEZVOUS_ROOM_FULL)) != 0);

    /* No NACK verdict may be RENDEZVOUS_DOWN. That code means "we heard
     * nothing", and a NACK is a thing we heard — this is the exact
     * collapse #122 exists to end. */
    for (int r = 0; r < 256; r++) {
        CHECK(tag, ConnectFail_ClassifyNackReason((uint8_t)r) !=
                       CONNECT_FAIL_RENDEZVOUS_DOWN);
    }

    /* The five new codes are mutually distinct as machine strings. (7f in
     * test_bilateral_punch.c sweeps the whole enum for this; repeated
     * here narrowly so a failure points at #122 rather than at "some code
     * somewhere collided".) */
    const ConnectFailCode k_new[] = {
        CONNECT_FAIL_RENDEZVOUS_ROOM_FULL,
        CONNECT_FAIL_RENDEZVOUS_TABLE_FULL,
        CONNECT_FAIL_RENDEZVOUS_BUSY,
        CONNECT_FAIL_RENDEZVOUS_BADFRAME,
        CONNECT_FAIL_RENDEZVOUS_REFUSED,
    };
    for (size_t a = 0; a < sizeof(k_new) / sizeof(k_new[0]); a++) {
        for (size_t b = a + 1; b < sizeof(k_new) / sizeof(k_new[0]); b++) {
            CHECK(tag, strcmp(ConnectFail_Code(k_new[a]),
                              ConnectFail_Code(k_new[b])) != 0);
        }
    }
}

/* ======================================================================
 * Test 5 — ClassifyJoin precedence: where a NACK wins, and where it must
 *          not
 * ====================================================================== */

static void test5_classify_join_precedence(void) {
    const char* tag = "5-classify-precedence";
    tests_run++;
    int cases = 0;

    /* --- 5a. THE DEFECT ITSELF. Silence except for a NACK: every reason
     * must reach its own verdict instead of RENDEZVOUS_DOWN. */
    for (int i = 0; i < REASON_ROWS; i++) {
        ConnectJoinEvidence ev;
        memset(&ev, 0, sizeof(ev));
        ev.nack_any = true;
        ev.nack_reason = k_reasons[i].reason;
        const ConnectFailCode got = ConnectFail_ClassifyJoin(&ev);
        check_eq_int(tag, got, k_reasons[i].verdict, k_reasons[i].text);
        CHECK(tag, got != CONNECT_FAIL_RENDEZVOUS_DOWN);
        cases++;
    }
    check_eq_int(tag, cases, REASON_ROWS, "every reason drove ClassifyJoin");

    /* Control: the SAME evidence without the NACK is still
     * RENDEZVOUS_DOWN. Without this the test above could pass on a
     * classifier that had stopped returning RENDEZVOUS_DOWN at all. */
    {
        ConnectJoinEvidence ev;
        memset(&ev, 0, sizeof(ev));
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_RENDEZVOUS_DOWN,
                     "true silence is still RENDEZVOUS_DOWN");
    }

    /* nack_any is the gate on nack_reason: a stale reason byte with the
     * flag clear must change nothing. */
    {
        ConnectJoinEvidence ev;
        memset(&ev, 0, sizeof(ev));
        ev.nack_any = false;
        ev.nack_reason = REND_NACK_SESSION_FULL;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_RENDEZVOUS_DOWN,
                     "nack_reason without nack_any is ignored");
    }

    /* --- 5b. A NACK outranks the two CHALLENGE inferences. A challenge
     * proves the server is alive; a NACK proves that AND names why. */
    {
        ConnectJoinEvidence ev;
        memset(&ev, 0, sizeof(ev));
        ev.challenge_any = true;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_RENDEZVOUS_NOPAIR,
                     "control: challenge without a NACK is NOPAIR");
        ev.nack_any = true;
        ev.nack_reason = REND_NACK_RATE_KEY;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_RENDEZVOUS_BUSY,
                     "a NACK outranks the NOPAIR inference");

        memset(&ev, 0, sizeof(ev));
        ev.challenge_any = true;
        ev.cookie_rechallenged = true;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_COOKIE_REJECTED,
                     "control: a re-challenge without a NACK is COOKIE_REJECTED");
        ev.nack_any = true;
        ev.nack_reason = REND_NACK_KEY_QUOTA;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_RENDEZVOUS_BUSY,
                     "a NACK outranks the COOKIE_REJECTED inference");
    }

    /* --- 5c. HAIRPIN still outranks everything. It is a fact about our
     * own router, measured on our own socket, and a rendezvous refusal
     * says nothing about it. */
    {
        ConnectJoinEvidence ev;
        memset(&ev, 0, sizeof(ev));
        ev.hairpin = true;
        ev.nack_any = true;
        ev.nack_reason = REND_NACK_SESSION_FULL;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_HAIRPIN,
                     "hairpin still outranks a NACK");
    }

    /* --- 5d. THE NARROW ARM. With sentinel DELIVERs (server alive, host
     * never registered) only SESSION_FULL displaces HOST_OFFLINE, because
     * only SESSION_FULL contradicts it: the server DOES hold two live
     * endpoints for the key. Everything else leaves HOST_OFFLINE
     * standing, so one mangled datagram that drew a BAD_LENGTH cannot
     * turn a correct "code stale" into "update the game". */
    {
        ConnectJoinEvidence ev;
        memset(&ev, 0, sizeof(ev));
        ev.deliver_any = true;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_HOST_OFFLINE,
                     "control: sentinel-only DELIVERs are HOST_OFFLINE");

        ev.nack_any = true;
        ev.nack_reason = REND_NACK_SESSION_FULL;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev),
                     CONNECT_FAIL_RENDEZVOUS_ROOM_FULL,
                     "SESSION_FULL contradicts HOST_OFFLINE and wins");

        int narrow_cases = 0;
        const uint8_t k_not_session_full[] = {
            REND_NACK_BAD_VERSION, REND_NACK_BAD_LENGTH, REND_NACK_BAD_TYPE,
            REND_NACK_RATE_IP, REND_NACK_RATE_KEY, REND_NACK_RATE_PREGATE,
            REND_NACK_KEY_QUOTA, REND_NACK_TABLE_FULL, 200u
        };
        for (size_t i = 0; i < sizeof(k_not_session_full); i++) {
            ev.nack_reason = k_not_session_full[i];
            check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_HOST_OFFLINE,
                         "a non-SESSION_FULL NACK leaves HOST_OFFLINE standing");
            narrow_cases++;
        }
        check_eq_int(tag, narrow_cases, (int)sizeof(k_not_session_full),
                     "every non-SESSION_FULL reason was tried against HOST_OFFLINE");
    }

    /* --- 5e. Past a REAL DELIVER the punch verdicts own the failure. The
     * rendezvous conversation SUCCEEDED and handed us the host; a refusal
     * about some other datagram must not send a NAT-blocked user to the
     * matchmaking server. */
    {
        ConnectJoinEvidence ev;
        memset(&ev, 0, sizeof(ev));
        ev.deliver_any = true;
        ev.deliver_real = true;
        ev.nack_any = true;
        ev.nack_reason = REND_NACK_SESSION_FULL;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_NAT_BLOCKED,
                     "a NACK does not displace NAT_BLOCKED");
        ev.port_disagreement = true;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_SYMMETRIC_BOTH,
                     "a NACK does not displace SYMMETRIC_BOTH");
        ev.port_disagreement = false;
        ev.punch_bad_token = true;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_PUNCH_AUTH,
                     "a NACK does not displace PUNCH_AUTH");
        ev.punch_bad_token = false;
        ev.bilateral_punched = true;
        check_eq_int(tag, ConnectFail_ClassifyJoin(&ev), CONNECT_FAIL_NONE,
                     "a NACK cannot turn a SUCCESSFUL join into a failure");
    }

    /* NULL evidence is still an internal error, not a NACK verdict. */
    check_eq_int(tag, ConnectFail_ClassifyJoin(NULL), CONNECT_FAIL_INTERNAL,
                 "NULL evidence is INTERNAL");
}

/* ======================================================================
 * Test 6 — attribution: a refusal is SUPPORTED, never a hedge
 * ====================================================================== */

static void test6_attribution(void) {
    const char* tag = "6-attribution";
    tests_run++;
    int cases = 0;

    const ConnectFailCode k_new[] = {
        CONNECT_FAIL_RENDEZVOUS_ROOM_FULL,
        CONNECT_FAIL_RENDEZVOUS_TABLE_FULL,
        CONNECT_FAIL_RENDEZVOUS_BUSY,
        CONNECT_FAIL_RENDEZVOUS_BADFRAME,
        CONNECT_FAIL_RENDEZVOUS_REFUSED,
    };
    for (size_t i = 0; i < sizeof(k_new) / sizeof(k_new[0]); i++) {
        /* The server SAID it. There is nothing to hedge about, so the
         * verdict is SUPPORTED with no note — even with a punch confirm
         * in hand, because a confirm is a fact about the PEER and a NACK
         * is a fact about the SERVER (the COOKIE_REJECTED argument in
         * connect_fail.c, one counterparty over). */
        check_eq_int(tag, ConnectFail_Attribute(k_new[i], false, 0u),
                     CONNECT_ATTRIB_SUPPORTED, "a refusal is SUPPORTED");
        check_eq_int(tag, ConnectFail_Attribute(k_new[i], true, 0u),
                     CONNECT_ATTRIB_SUPPORTED, "a punch confirm does not hedge it");
        check_eq_str(tag, ConnectFail_AttributionNote(
                              ConnectFail_Attribute(k_new[i], false, 0u)),
                     "", "a supported refusal prints no note");
        cases++;
    }
    check_eq_int(tag, cases, (int)(sizeof(k_new) / sizeof(k_new[0])),
                 "every new code was attributed");

    /* The #36 rules the new codes must NOT have disturbed. */
    check_eq_int(tag, ConnectFail_Attribute(CONNECT_FAIL_RENDEZVOUS_DOWN, false, 0u),
                 CONNECT_ATTRIB_AMBIG_VERSION, "true silence stays ambiguous");
    check_eq_int(tag, ConnectFail_Attribute(CONNECT_FAIL_RENDEZVOUS_DOWN, false, 3u),
                 CONNECT_ATTRIB_VERSION_SKEW, "a skew witness stays definite");
    check_eq_int(tag, ConnectFail_Attribute(CONNECT_FAIL_NAT_BLOCKED, true, 0u),
                 CONNECT_ATTRIB_AMBIG_CONFIRM, "a confirm still contradicts NAT_BLOCKED");
    check_eq_int(tag, ConnectFail_Attribute(CONNECT_FAIL_NONE, false, 0u),
                 CONNECT_ATTRIB_OK, "success is OK");
}

/* ======================================================================
 * Test 7 — Rendezvous_WireVersion / FrameType / HasMagic
 * ====================================================================== */

static void test7_frame_routing(void) {
    const char* tag = "7-frame-routing";
    tests_run++;

    const int v = Rendezvous_WireVersion();
    check_eq_int(tag, v, 2, "this build speaks '3SXR' v2");

    uint8_t pkt[REND_NACK_LEN];

    /* The accessor exists so a caller can compare against the SAME
     * constant FrameType uses. Prove they agree in both directions rather
     * than trusting the comment. */
    for (int t = 0; t < 256; t++) {
        build_nack(pkt, sizeof(pkt), (uint8_t)v, (uint8_t)t, 0, k_key);
        check_eq_int(tag, Rendezvous_FrameType(pkt, (int)sizeof(pkt)), t,
                     "FrameType returns the type byte at our version");
    }
    build_nack(pkt, sizeof(pkt), (uint8_t)(v + 1), (uint8_t)REND_FRAME_NACK, 0, k_key);
    check_eq_int(tag, Rendezvous_FrameType(pkt, (int)sizeof(pkt)), 0,
                 "FrameType refuses a foreign version");
    CHECK(tag, Rendezvous_HasMagic(pkt, (int)sizeof(pkt)));

    /* THE RUNT/SKEW DISCRIMINATOR, which is why WireVersion is public:
     * FrameType returns 0 for a 4- or 5-byte magic-matched runt AND for a
     * skew, so a receive loop that cannot see the version byte cannot
     * tell them apart (direct_p2p.c's badver arm). */
    build_good_nack(pkt, sizeof(pkt), REND_NACK_BAD_TYPE);
    CHECK(tag, Rendezvous_HasMagic(pkt, 4));
    check_eq_int(tag, Rendezvous_FrameType(pkt, 4), 0, "a 4-byte runt has no type");
    check_eq_int(tag, Rendezvous_FrameType(pkt, 5), 0, "a 5-byte runt has no type");
    check_eq_int(tag, Rendezvous_FrameType(pkt, 6), REND_FRAME_NACK,
                 "6 bytes is the first length that carries a type");

    /* HasMagic is version-BLIND on purpose (the straggler drop). */
    build_nack(pkt, sizeof(pkt), 99u, (uint8_t)REND_FRAME_NACK, 0, k_key);
    CHECK(tag, Rendezvous_HasMagic(pkt, (int)sizeof(pkt)));
    check_eq_int(tag, Rendezvous_FrameType(pkt, (int)sizeof(pkt)), 0,
                 "...while FrameType is not");

    CHECK(tag, !Rendezvous_HasMagic(pkt, 3));
    CHECK(tag, !Rendezvous_HasMagic(NULL, 28));
    check_eq_int(tag, Rendezvous_FrameType(NULL, 28), 0, "NULL has no frame type");
    pkt[0] ^= 0xFFu;
    CHECK(tag, !Rendezvous_HasMagic(pkt, (int)sizeof(pkt)));
}

/* ======================================================================
 * Test 8 — Rendezvous_NackReasonText, as a set of log-grep anchors
 * ====================================================================== */

static void test8_reason_text(void) {
    const char* tag = "8-reason-text";
    tests_run++;
    int cases = 0;

    /* Never NULL, for any byte. This function is called from a log
     * formatter on a failure path; a NULL here is a crash in the middle
     * of reporting the crash. */
    for (int r = 0; r < 256; r++) {
        CHECK(tag, Rendezvous_NackReasonText((uint8_t)r) != NULL);
    }

    /* The nine names are distinct — they are how a triager greps a
     * tester's log, so a collision silently merges two causes. */
    for (int i = 0; i < REASON_ROWS; i++) {
        for (int j = i + 1; j < REASON_ROWS; j++) {
            CHECK(tag, strcmp(Rendezvous_NackReasonText(k_reasons[i].reason),
                              Rendezvous_NackReasonText(k_reasons[j].reason)) != 0);
        }
        CHECK(tag, strcmp(Rendezvous_NackReasonText(k_reasons[i].reason),
                          "unknown") != 0);
        cases++;
    }
    check_eq_int(tag, cases, REASON_ROWS, "every name was compared");
    check_eq_str(tag, Rendezvous_NackReasonText(0), "unknown",
                 "reason 0 is reserved and unnamed");
}

/* ======================================================================
 * Test 9 — Rendezvous_ParseSignalUrl
 * ====================================================================== */

static void test9_parse_signal_url(void) {
    const char* tag = "9-parse-signal-url";
    tests_run++;

    char host[64];
    uint16_t port;

    struct { const char* url; const char* host; uint16_t port; } k_ok[] = {
        { "udp://203.0.113.7:7000", "203.0.113.7", 7000 },
        { "udp://rendezvous.example.com:1", "rendezvous.example.com", 1 },
        { "udp://a:65535", "a", 65535 },
        { "udp://127.0.0.1:00080", "127.0.0.1", 80 }, /* strtoul eats leading zeros */
    };
    int ok_cases = 0;
    for (size_t i = 0; i < sizeof(k_ok) / sizeof(k_ok[0]); i++) {
        memset(host, 0xAB, sizeof(host));
        port = 0xABCD;
        CHECK(tag, Rendezvous_ParseSignalUrl(k_ok[i].url, host, &port));
        check_eq_str(tag, host, k_ok[i].host, "host");
        check_eq_int(tag, port, k_ok[i].port, "port");
        ok_cases++;
    }
    check_eq_int(tag, ok_cases, (int)(sizeof(k_ok) / sizeof(k_ok[0])),
                 "every accept case ran");

    /* Rejections, each named so a failure says which rule stopped
     * holding. The outputs must be ZEROED on every one of them — a caller
     * that ignores the bool would otherwise dial a half-parsed host. */
    static const char* const k_bad[] = {
        "",                              /* empty                              */
        "udp://",                        /* scheme only                        */
        "udp://:7000",                   /* empty host                         */
        "udp://host",                    /* no port                            */
        "udp://host:",                   /* empty port                         */
        "udp://host:0",                  /* port 0                             */
        "udp://host:65536",              /* port out of range                  */
        "udp://host:99999999999",        /* port overflows                     */
        "udp://host:-1",                 /* negative                           */
        "udp://host:70a0",               /* trailing junk in the port          */
        "udp://host:7000x",              /* trailing junk                      */
        "udp://ho st:7000",              /* space in host                      */
        "udp://ho\tst:7000",             /* control char in host               */
        "udp://[::1]:7000",              /* IPv6 literal — out of scope        */
        "tcp://host:7000",               /* wrong scheme                       */
        "UDP://host:7000",               /* scheme is case-sensitive           */
        "host:7000",                     /* no scheme                          */
        " udp://host:7000",              /* leading space                      */
        "udp://0123456789012345678901234567890123456789012345678901234567890123:1",
                                         /* 64-char host: >= the 64-byte buffer */
    };
    int bad_cases = 0;
    for (size_t i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); i++) {
        memset(host, 0xAB, sizeof(host));
        port = 0xABCD;
        if (Rendezvous_ParseSignalUrl(k_bad[i], host, &port)) {
            checks_run++;
            fail_count++;
            fprintf(stderr,
                    "[test_rendezvous_wire] FAIL: %s: accepted a malformed URL: \"%s\" "
                    "-> host=\"%s\" port=%u\n",
                    tag, k_bad[i], host, (unsigned)port);
        } else {
            checks_run++;
            check_eq_int(tag, port, 0, "a rejected URL zeroes the port");
            check_eq_int(tag, host[0], 0, "a rejected URL zeroes the host");
        }
        bad_cases++;
    }
    check_eq_int(tag, bad_cases, (int)(sizeof(k_bad) / sizeof(k_bad[0])),
                 "every reject case ran");

    /* 63 host chars is the last accepted length — the boundary, not a
     * sample near it. */
    {
        char url[128];
        char long_host[64];
        memset(long_host, 'h', 63);
        long_host[63] = '\0';
        snprintf(url, sizeof(url), "udp://%s:7000", long_host);
        memset(host, 0xAB, sizeof(host));
        port = 0;
        CHECK(tag, Rendezvous_ParseSignalUrl(url, host, &port));
        check_eq_str(tag, host, long_host, "a 63-char host is the boundary");
        check_eq_int(tag, port, 7000, "and its port survives");
    }

    /* NULL arguments must refuse rather than crash. */
    CHECK(tag, !Rendezvous_ParseSignalUrl(NULL, host, &port));
    CHECK(tag, !Rendezvous_ParseSignalUrl("udp://host:7000", NULL, &port));
    CHECK(tag, !Rendezvous_ParseSignalUrl("udp://host:7000", host, NULL));
}

/* ====================================================================== */

int Netplay_Test_RendezvousWire(void) {
    test1_reason_roundtrip();
    test2_reject_malformed();
    test3_unknown_reason();
    test4_reason_to_verdict();
    test5_classify_join_precedence();
    test6_attribution();
    test7_frame_routing();
    test8_reason_text();
    test9_parse_signal_url();

    /* --- the anti-vacuous gates, evaluated LAST ------------------------
     * Both are hard failures, not warnings. A harness that reports "0
     * failures" after running nothing is the exact shape this file exists
     * to make impossible. */
    if (tests_run != EXPECTED_TESTS) {
        fprintf(stderr,
                "[test_rendezvous_wire] COVERAGE FAIL: ran %d test(s), expected "
                "exactly %d — a test was removed, skipped, or never registered\n",
                tests_run, EXPECTED_TESTS);
        fail_count++;
    }
    if (checks_run < EXPECTED_MIN_CHECKS) {
        fprintf(stderr,
                "[test_rendezvous_wire] COVERAGE FAIL: only %d assertion(s) ran, "
                "floor is %d — the bodies are not executing\n",
                checks_run, EXPECTED_MIN_CHECKS);
        fail_count++;
    }

    fprintf(stderr,
            "[test_rendezvous_wire] summary: %d test(s), %d assertion(s), "
            "%d failure(s)\n",
            tests_run, checks_run, fail_count);
    if (fail_count == 0) {
        fprintf(stderr, "[test_rendezvous_wire] OK\n");
    }
    return fail_count == 0 ? 0 : 1;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_RendezvousWire(void) {
    /* "not compiled in", spelled exactly that way: tools/gates/run-gates.sh
     * greps for that phrase so an exit-2 misbuild is recorded RED instead
     * of being mistaken for a pass. */
    fprintf(stderr,
            "[test_rendezvous_wire] not compiled in; rebuild with "
            "EXTRA_CMAKE_ARGS=\"-DENABLE_NETPLAY=ON "
            "-DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS\".\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
