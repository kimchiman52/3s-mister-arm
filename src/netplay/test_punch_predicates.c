/*
 * test_punch_predicates.c — task #132, priority 1 (items 2 and 3): the
 * pure decisions on the authenticated-punch surface.
 *
 * TWO SUBJECTS, ONE PROPERTY. Everything here answers the same question
 * from a different angle: given some bytes and a source, is this the peer
 * we already authenticated? Get it wrong and the failure is an
 * authentication failure — a redirect oracle, a confirmation oracle, or a
 * forged datagram reaching GekkoNet's deserializer — and every one of
 * those looks EXACTLY like success in a connectivity test.
 *
 *   1. stun.c's classifiers. Stun_IsBindingResponse (stun.c:1028) is
 *      pure, declared in stun.h:221, and had ZERO tests despite being the
 *      gate three receive paths use to decide a datagram is not session
 *      traffic (direct_p2p.c:1060, direct_p2p.c:2230,
 *      sdl_net_adapter.c:343). Stun_IsPunchPayload (stun.c:147) had a
 *      truth table in test_stun_mock.c:861-894 — one wrong token, a
 *      truncation, an oversize, a NULL — but nothing that says WHICH
 *      bytes it compares or that it compares all of them. That is the
 *      part an accumulator-style constant-time compare can lose
 *      silently, so it is swept bit by bit here.
 *
 *   2. late_punch.c's decisions that need no socket.
 *      test_late_punch.c drives accept/reject/relearn over three real
 *      loopback sockets, and that is the right place for delivery. Three
 *      properties are invisible from there, because observing a datagram
 *      at all means advancing the clock past LATE_PUNCH_TX_INTERVAL_MS,
 *      at which point the cadenced keepalive would have sent one anyway:
 *        - a valid-token punch from a FOREIGN IP must not schedule an
 *          answer (answering hands a replayer a confirmation oracle —
 *          s_peer_ip compare at late_punch.c:280 says so, unenforced);
 *        - at the relearn cap the refused move must change NOTHING, and
 *          in particular the send target must stay the LEARNED endpoint
 *          rather than following the datagram's source;
 *        - an accepted relearn must move the target to the new endpoint
 *          and schedule the prompt answer.
 *      LatePunch_TestPeek (late_punch.c, ENABLE_NETPLAY_TESTS only) makes
 *      the send target and the prompt flag readable so those three are
 *      decisions again instead of timing observations.
 *
 * NOT HERE, deliberately: the punch race, the settle window, split brain,
 * the promised listening interval, the reclaim boundary. Those are
 * interaction properties between two endpoints; a mocked unit passes them
 * vacuously while the real defect lives in the timing. They belong to the
 * netns rig and to test_bilateral_punch.c.
 *
 * HOW IT CANNOT PASS VACUOUSLY. The three devices from
 * test_rendezvous_wire.c:73-82 — a LITERAL EXPECTED_TESTS, an assertion
 * floor, and per-sweep case floors on every bit sweep. The stub says
 * "not compiled in", spelled exactly as tools/gates/run-gates.sh:181
 * greps for it.
 *
 * NO -DNETPLAY_TEST_HOOKS NEEDED — stun.c and late_punch.c are both in
 * the shipped netplay build unconditionally. Enable with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS"
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/late_punch.h"
#include "netplay/stun.h"

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* --- counters: the anti-vacuous machinery ------------------------------ */

static int fail_count = 0;
static int tests_run = 0;
static int checks_run = 0;

/* A LITERAL. Never a count of a registry. */
#define EXPECTED_TESTS 10

/* Assertion floor. The real figure is 1561 (task #133 added tests 8-10)
 * and is printed in the summary; this sits comfortably below it and
 * comfortably above what any executing SUBSET could produce. Not an exact
 * count — that invites bumping the number instead of asking why it
 * moved. */
#define EXPECTED_MIN_CHECKS 1400

static void check(const char* tag, bool ok, const char* what) {
    checks_run++;
    if (!ok) {
        fail_count++;
        fprintf(stderr, "[test_punch_predicates] FAIL: %s: %s\n", tag, what);
    }
}

#define CHECK(tag, cond) check((tag), (cond), #cond)

static void check_eq_int(const char* tag, long got, long want, const char* what) {
    checks_run++;
    if (got != want) {
        fail_count++;
        fprintf(stderr,
                "[test_punch_predicates] FAIL: %s: %s: got %ld, want %ld\n",
                tag, what, got, want);
    }
}

static void check_eq_str(const char* tag, const char* got, const char* want,
                         const char* what) {
    checks_run++;
    if (got == NULL || strcmp(got, want) != 0) {
        fail_count++;
        fprintf(stderr,
                "[test_punch_predicates] FAIL: %s: %s: got \"%s\", want \"%s\"\n",
                tag, what, (got != NULL) ? got : "(null)", want);
    }
}

/* --- constants listed INDEPENDENTLY ------------------------------------
 *
 * STUN_BINDING_RESPONSE and STUN_MAGIC_COOKIE are #defines private to
 * stun.c (stun.c:32-34), so this file cannot import them even by
 * accident. They are written out from RFC 5389 instead — §6 fixes the
 * magic cookie at 0x2112A442, and §18.1 registers Binding as method
 * 0x001, which with the success-response class bits (§6, C1=0 C0=1)
 * encodes as message type 0x0101. Testing a constant against itself
 * proves nothing; testing it against the spec is the point. */
#define RFC5389_BINDING_SUCCESS_RESPONSE 0x0101u
#define RFC5389_MAGIC_COOKIE             0x2112A442u

/* Same reasoning for the punch prefix: k_punch_prefix is static in
 * stun.c:133. Written out here from late_punch.h:27, which documents the
 * payload as "3SX_PUNCH" + the 8-byte room-code-derived token. */
static const char k_expected_prefix[] = "3SX_PUNCH";

/* --- helpers ----------------------------------------------------------- */

static void build_stun_header(uint8_t buf[20], uint16_t msg_type,
                              uint32_t cookie, uint16_t msg_len) {
    buf[0] = (uint8_t)(msg_type >> 8);
    buf[1] = (uint8_t)msg_type;
    buf[2] = (uint8_t)(msg_len >> 8);
    buf[3] = (uint8_t)msg_len;
    buf[4] = (uint8_t)(cookie >> 24);
    buf[5] = (uint8_t)(cookie >> 16);
    buf[6] = (uint8_t)(cookie >> 8);
    buf[7] = (uint8_t)cookie;
    for (int i = 8; i < 20; i++) buf[i] = (uint8_t)(0xA0 + i);
}

/* The token the late-punch cases arm with. A literal, not a derivation:
 * Rendezvous_DerivePunchToken's own binding is pinned in
 * test_late_punch.c ("nonce-binds-token"); here the token is just eight
 * bytes that must be compared in full. */
static const uint8_t k_token[STUN_PUNCH_TOKEN_LEN] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
};

/* late_punch BORROWS its socket and dereferences it in exactly one place
 * — LatePunch_Tick's NET_SendDatagram (late_punch.c:178, guarded by the
 * NULL check at :163). Arm needs a non-NULL pointer to arm at all
 * (late_punch.c:55). No test in this file calls Tick, so this sentinel is
 * never dereferenced; the send side is test_late_punch.c's job, over real
 * sockets. */
#define FAKE_SOCK ((struct NET_DatagramSocket*)(uintptr_t)0x1)

#define PEER_IP   "10.0.0.5"
#define PEER_PORT ((uint16_t)5000)

typedef struct {
    char     ip[64];
    uint16_t port;
    bool     prompt;
    int      relearns;
} Peek;

static Peek peek(void) {
    Peek p;
    memset(&p, 0, sizeof(p));
    LatePunch_TestPeek(p.ip, sizeof(p.ip), &p.port, &p.prompt, &p.relearns);
    return p;
}

static bool peek_port_is(uint16_t port) {
    return peek().port == port;
}

/* Task #133: the liveness challenge state. `nonce` is the value the
 * module drew for the pending challenge; a test needs it to forge the
 * CORRECT response, which is the only way to pin that a live peer is
 * still relearnable rather than that everything is refused. */
typedef struct {
    bool     active;
    uint16_t port;
    uint8_t  nonce[STUN_PUNCH_PROBE_NONCE_LEN];
    bool     echo_pending;
    uint16_t echo_port;
} Chal;

static Chal chal(void) {
    Chal c;
    memset(&c, 0, sizeof(c));
    LatePunch_TestPeekChallenge(&c.active, &c.port, c.nonce, &c.echo_pending,
                                &c.echo_port);
    return c;
}

/* Drive one nominate -> challenge -> correct response round trip and
 * return whether the module promoted. Everything a real peer does, and
 * nothing a room-code holder can do without receiving the challenge. */
static bool answer_challenge(const char* tag, uint16_t port) {
    const Chal c = chal();
    CHECK(tag, c.active);
    check_eq_int(tag, c.port, port, "the challenge went to the nominee");
    uint8_t resp[STUN_PUNCH_PROBE_PAYLOAD_LEN];
    Stun_BuildProbePayload(k_token, STUN_PUNCH_PROBE_RESPONSE, c.nonce, resp);
    return LatePunch_HandleDatagram(resp, (int)sizeof(resp), PEER_IP, port);
}

/* A full accepted move: the punch that nominates, then the response that
 * verifies. Returns true iff the target actually moved. */
static bool move_peer(const char* tag, const uint8_t* payload, uint16_t port) {
    CHECK(tag, LatePunch_HandleDatagram(payload, STUN_PUNCH_PAYLOAD_LEN,
                                        PEER_IP, port));
    CHECK(tag, answer_challenge(tag, port));
    return peek_port_is(port);
}

/* Arm fresh at the established endpoint and assert the arm actually took,
 * so no later case can pass by silently running while disarmed. */
static void arm_fresh(const char* tag) {
    LatePunch_Disarm();
    LatePunch_Arm(FAKE_SOCK, k_token, PEER_IP, PEER_PORT);
    CHECK(tag, LatePunch_IsArmed());
    const Peek p = peek();
    check_eq_str(tag, p.ip, PEER_IP, "armed at the established IP");
    check_eq_int(tag, p.port, PEER_PORT, "armed at the established port");
    check_eq_int(tag, (long)p.prompt, 0, "arm schedules no answer");
    check_eq_int(tag, p.relearns, 0, "arm resets the relearn counter");
}

/* ====================================================================== */
/* 1. Stun_IsBindingResponse — pure, declared, previously untested.       */
/* ====================================================================== */

static void test1_binding_response(void) {
    const char* tag = "binding";
    tests_run++;
    uint8_t buf[64];

    build_stun_header(buf, RFC5389_BINDING_SUCCESS_RESPONSE,
                      RFC5389_MAGIC_COOKIE, 0);
    memset(buf + 20, 0xEE, sizeof(buf) - 20);

    CHECK(tag, Stun_IsBindingResponse(buf, 20));
    CHECK(tag, Stun_IsBindingResponse(buf, 64));
    CHECK(tag, !Stun_IsBindingResponse(NULL, 20));

    /* THE 20-BYTE FLOOR. One byte short must refuse: the cookie read
     * reaches buf[7], and the length check is the only thing that keeps
     * the classifier inside a runt datagram. */
    {
        int shorts = 0;
        for (int n = -4; n < 20; n++) {
            CHECK(tag, !Stun_IsBindingResponse(buf, n));
            shorts++;
        }
        check_eq_int(tag, shorts, 24, "every sub-header length was tried");
    }

    /* Message type: every single-bit deviation from the success response
     * is not a success response. Includes the two that matter in
     * practice — 0x0001 Binding Request (our own outbound frame echoed
     * back) and 0x0111 Binding Error Response. */
    {
        int flips = 0;
        for (int b = 0; b < 16; b++) {
            const uint16_t t = (uint16_t)(RFC5389_BINDING_SUCCESS_RESPONSE ^ (1u << b));
            build_stun_header(buf, t, RFC5389_MAGIC_COOKIE, 0);
            CHECK(tag, !Stun_IsBindingResponse(buf, 20));
            flips++;
        }
        check_eq_int(tag, flips, 16, "all 16 msg_type bits were flipped");

        build_stun_header(buf, 0x0001u, RFC5389_MAGIC_COOKIE, 0);
        CHECK(tag, !Stun_IsBindingResponse(buf, 20));
        build_stun_header(buf, 0x0111u, RFC5389_MAGIC_COOKIE, 0);
        CHECK(tag, !Stun_IsBindingResponse(buf, 20));
        build_stun_header(buf, 0x0011u, RFC5389_MAGIC_COOKIE, 0);
        CHECK(tag, !Stun_IsBindingResponse(buf, 20));
    }

    /* Magic cookie: every single-bit deviation refuses. This is the
     * check that keeps arbitrary UDP noise from being classified as
     * STUN and silently dropped before it reaches the layer that wanted
     * it (sdl_net_adapter.c:343 drops on exactly this verdict). */
    {
        int flips = 0;
        for (int b = 0; b < 32; b++) {
            build_stun_header(buf, RFC5389_BINDING_SUCCESS_RESPONSE,
                              RFC5389_MAGIC_COOKIE ^ (1u << b), 0);
            CHECK(tag, !Stun_IsBindingResponse(buf, 20));
            flips++;
        }
        check_eq_int(tag, flips, 32, "all 32 cookie bits were flipped");
    }

    /* Byte ORDER, both fields: the type is big-endian at [0..1] and the
     * cookie big-endian at [4..7]. A swap in either is a classifier that
     * matches nothing, i.e. netplay that never sees a STUN reply. */
    build_stun_header(buf, RFC5389_BINDING_SUCCESS_RESPONSE,
                      RFC5389_MAGIC_COOKIE, 0);
    check_eq_int(tag, buf[0], 0x01, "msg_type high byte first");
    check_eq_int(tag, buf[1], 0x01, "msg_type low byte second");
    check_eq_int(tag, buf[4], 0x21, "cookie MSB first");
    check_eq_int(tag, buf[7], 0x42, "cookie LSB last");
    {
        uint8_t sw[20];
        memcpy(sw, buf, 20);
        sw[4] = buf[7]; sw[5] = buf[6]; sw[6] = buf[5]; sw[7] = buf[4];
        CHECK(tag, !Stun_IsBindingResponse(sw, 20));
    }

    /* WHAT IT DELIBERATELY DOES NOT CHECK. This is a cheap classifier,
     * not an authenticator: it ignores the transaction ID and the
     * declared message length. Stun_ParseBindingResponse (stun.c:1038)
     * is where the txid is verified, and the split is load-bearing — the
     * receive paths need a verdict on datagrams whose txid they do not
     * have. Pinning it here means a future "hardening" that folds the
     * txid check in cannot land silently and start dropping the replies
     * the keepalive path relies on. */
    build_stun_header(buf, RFC5389_BINDING_SUCCESS_RESPONSE,
                      RFC5389_MAGIC_COOKIE, 0);
    memset(buf + 8, 0x00, 12);
    CHECK(tag, Stun_IsBindingResponse(buf, 20));
    memset(buf + 8, 0xFF, 12);
    CHECK(tag, Stun_IsBindingResponse(buf, 20));
    build_stun_header(buf, RFC5389_BINDING_SUCCESS_RESPONSE,
                      RFC5389_MAGIC_COOKIE, 0xFFFF);
    CHECK(tag, Stun_IsBindingResponse(buf, 20));
}

/* ====================================================================== */
/* 2. Stun_HasPunchPrefix — the "is this punch-shaped" gate.              */
/* ====================================================================== */

static void test2_punch_prefix(void) {
    const char* tag = "prefix";
    tests_run++;

    uint8_t payload[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(k_token, payload);

    /* The encoder really does emit "3SX_PUNCH" then the token verbatim.
     * Checked against the independently written literal, not against
     * stun.c's own static. */
    check_eq_int(tag, (long)strlen(k_expected_prefix), STUN_PUNCH_PREFIX_LEN,
                 "the prefix is nine bytes");
    check_eq_int(tag, memcmp(payload, k_expected_prefix, STUN_PUNCH_PREFIX_LEN), 0,
                 "the payload starts with the documented prefix");
    check_eq_int(tag,
                 memcmp(payload + STUN_PUNCH_PREFIX_LEN, k_token, sizeof(k_token)),
                 0, "and carries the token verbatim after it");
    check_eq_int(tag, STUN_PUNCH_PAYLOAD_LEN,
                 STUN_PUNCH_PREFIX_LEN + STUN_PUNCH_TOKEN_LEN,
                 "payload length is prefix + token");

    CHECK(tag, !Stun_HasPunchPrefix(NULL, STUN_PUNCH_PAYLOAD_LEN));

    /* The 9-byte floor: the prefix alone is enough (this is what makes a
     * bad-token punch CONSUMABLE rather than forwarded to GekkoNet), and
     * anything shorter is not. */
    {
        int shorts = 0;
        for (int n = -2; n < STUN_PUNCH_PREFIX_LEN; n++) {
            CHECK(tag, !Stun_HasPunchPrefix(payload, n));
            shorts++;
        }
        check_eq_int(tag, shorts, STUN_PUNCH_PREFIX_LEN + 2,
                     "every sub-prefix length was tried");
    }
    CHECK(tag, Stun_HasPunchPrefix(payload, STUN_PUNCH_PREFIX_LEN));
    CHECK(tag, Stun_HasPunchPrefix(payload, STUN_PUNCH_PAYLOAD_LEN));
    CHECK(tag, Stun_HasPunchPrefix(payload, 4096)); /* len is a floor, not a shape */

    /* Every bit of every prefix byte, flipped independently. A prefix
     * compare that stopped early — or that only looked at byte 0 — would
     * let GekkoNet-shaped and MIST-shaped traffic be swallowed by the
     * punch path, or vice versa. */
    {
        int flips = 0;
        for (int i = 0; i < STUN_PUNCH_PREFIX_LEN; i++) {
            for (int b = 0; b < 8; b++) {
                uint8_t bad[STUN_PUNCH_PAYLOAD_LEN];
                memcpy(bad, payload, sizeof(bad));
                bad[i] ^= (uint8_t)(1u << b);
                CHECK(tag, !Stun_HasPunchPrefix(bad, STUN_PUNCH_PAYLOAD_LEN));
                flips++;
            }
        }
        check_eq_int(tag, flips, STUN_PUNCH_PREFIX_LEN * 8,
                     "all 72 prefix bits were flipped");
    }

    /* Byte 0 is 0x33 '3' — outside GekkoNet's PacketType range [1,7] and
     * not the MIST magic's 0x4D 'M'. That disjointness is what the
     * post-handoff receive paths rely on to tell the three protocols
     * apart on one socket. */
    check_eq_int(tag, payload[0], 0x33, "punch traffic starts with '3'");
}

/* ====================================================================== */
/* 3. Stun_IsPunchPayload — the token compare, byte by byte.              */
/* ====================================================================== */

static void test3_punch_payload(void) {
    const char* tag = "payload";
    tests_run++;

    uint8_t payload[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(k_token, payload);

    CHECK(tag, Stun_IsPunchPayload(payload, STUN_PUNCH_PAYLOAD_LEN, k_token));
    CHECK(tag, !Stun_IsPunchPayload(NULL, STUN_PUNCH_PAYLOAD_LEN, k_token));
    CHECK(tag, !Stun_IsPunchPayload(payload, STUN_PUNCH_PAYLOAD_LEN, NULL));

    /* EXACT length, not a minimum. A padded or truncated payload is not
     * the payload — the length check is `len != STUN_PUNCH_PAYLOAD_LEN`
     * and it must stay an equality, or a 4096-byte datagram whose first
     * 17 bytes replay a captured punch would authenticate. */
    {
        uint8_t big[64];
        memset(big, 0x77, sizeof(big));
        memcpy(big, payload, sizeof(payload));
        int lens = 0;
        for (int n = -2; n <= 64; n++) {
            const bool got = Stun_IsPunchPayload(big, n, k_token);
            const bool want = (n == STUN_PUNCH_PAYLOAD_LEN);
            if (got != want) {
                checks_run++; fail_count++;
                fprintf(stderr, "[test_punch_predicates] FAIL: %s: len %d: "
                        "got %d want %d\n", tag, n, (int)got, (int)want);
            } else {
                checks_run++;
            }
            lens++;
        }
        check_eq_int(tag, lens, 67, "every length from -2 to 64 was tried");
    }

    /* EVERY BIT OF THE TOKEN. This is the sweep test_stun_mock.c:861-894
     * does not do: it flips one byte and calls it a truth table. An
     * accumulator compare that lost a loop iteration — `i < 7`, or a
     * `sizeof(token)` that became `sizeof(uint8_t*)` — still passes a
     * single-flip test as long as the flipped byte is one it still
     * reads. All 64 is the only sweep that cannot. */
    {
        int flips = 0;
        for (int i = 0; i < STUN_PUNCH_TOKEN_LEN; i++) {
            for (int b = 0; b < 8; b++) {
                uint8_t bad[STUN_PUNCH_PAYLOAD_LEN];
                memcpy(bad, payload, sizeof(bad));
                bad[STUN_PUNCH_PREFIX_LEN + i] ^= (uint8_t)(1u << b);
                CHECK(tag, !Stun_IsPunchPayload(bad, sizeof(bad), k_token));
                flips++;
            }
        }
        check_eq_int(tag, flips, STUN_PUNCH_TOKEN_LEN * 8,
                     "all 64 token bits were flipped");
    }

    /* And every bit of the prefix, through the payload check too — the
     * prefix compare inside Stun_IsPunchPayload is a separate memcmp
     * from Stun_HasPunchPrefix's. */
    {
        int flips = 0;
        for (int i = 0; i < STUN_PUNCH_PREFIX_LEN; i++) {
            for (int b = 0; b < 8; b++) {
                uint8_t bad[STUN_PUNCH_PAYLOAD_LEN];
                memcpy(bad, payload, sizeof(bad));
                bad[i] ^= (uint8_t)(1u << b);
                CHECK(tag, !Stun_IsPunchPayload(bad, sizeof(bad), k_token));
                flips++;
            }
        }
        check_eq_int(tag, flips, STUN_PUNCH_PREFIX_LEN * 8,
                     "all 72 prefix bits were flipped inside the payload check");
    }

    /* THE FOLD TRAP. The compare accumulates per-position differences
     * (`diff |= buf[i] ^ token[i]`, stun.c:158-162). Two flips in
     * DIFFERENT bytes at the same bit position cancel exactly under any
     * compare that xor-folds the token to a checksum before comparing —
     * a plausible "optimisation", and one that would accept a token
     * nobody derived. Under the real per-position accumulator this must
     * still reject. */
    {
        uint8_t bad[STUN_PUNCH_PAYLOAD_LEN];
        memcpy(bad, payload, sizeof(bad));
        bad[STUN_PUNCH_PREFIX_LEN + 0] ^= 0x01;
        bad[STUN_PUNCH_PREFIX_LEN + 1] ^= 0x01;
        uint8_t fold_a = 0, fold_b = 0;
        for (int i = 0; i < STUN_PUNCH_TOKEN_LEN; i++) {
            fold_a = (uint8_t)(fold_a ^ payload[STUN_PUNCH_PREFIX_LEN + i]);
            fold_b = (uint8_t)(fold_b ^ bad[STUN_PUNCH_PREFIX_LEN + i]);
        }
        check_eq_int(tag, fold_a, fold_b,
                     "the two tokens are indistinguishable to an xor fold");
        CHECK(tag, !Stun_IsPunchPayload(bad, sizeof(bad), k_token));
    }

    /* THE ORDER TRAP. Swapping two token bytes preserves any
     * order-insensitive digest (a sum, a sorted compare) but is a
     * different token. */
    {
        uint8_t bad[STUN_PUNCH_PAYLOAD_LEN];
        memcpy(bad, payload, sizeof(bad));
        const uint8_t t = bad[STUN_PUNCH_PREFIX_LEN + 2];
        bad[STUN_PUNCH_PREFIX_LEN + 2] = bad[STUN_PUNCH_PREFIX_LEN + 5];
        bad[STUN_PUNCH_PREFIX_LEN + 5] = t;
        CHECK(tag, !Stun_IsPunchPayload(bad, sizeof(bad), k_token));
    }

    /* No special case for the all-zero token: an unset token still
     * compares byte for byte. (Whether an all-zero token can be DERIVED
     * is Rendezvous_DerivePunchToken's problem, not this predicate's.) */
    {
        static const uint8_t zero[STUN_PUNCH_TOKEN_LEN] = { 0 };
        uint8_t zpay[STUN_PUNCH_PAYLOAD_LEN];
        Stun_BuildPunchPayload(zero, zpay);
        CHECK(tag, Stun_IsPunchPayload(zpay, sizeof(zpay), zero));
        CHECK(tag, !Stun_IsPunchPayload(zpay, sizeof(zpay), k_token));
        CHECK(tag, !Stun_IsPunchPayload(payload, sizeof(payload), zero));
    }

    /* Punch-shaped but not a punch: the prefix passes, the token does
     * not. This is the exact datagram class late_punch consumes without
     * answering, so it must be distinguishable from both a real punch
     * and from non-punch traffic. */
    {
        uint8_t shaped[STUN_PUNCH_PAYLOAD_LEN];
        memcpy(shaped, k_expected_prefix, STUN_PUNCH_PREFIX_LEN);
        memset(shaped + STUN_PUNCH_PREFIX_LEN, 0xA5, STUN_PUNCH_TOKEN_LEN);
        CHECK(tag, Stun_HasPunchPrefix(shaped, sizeof(shaped)));
        CHECK(tag, !Stun_IsPunchPayload(shaped, sizeof(shaped), k_token));
    }
}

/* ====================================================================== */
/* 4. A valid token from a FOREIGN IP does not schedule an answer.        */
/* ====================================================================== */

static void test4_foreign_ip_no_answer(void) {
    const char* tag = "foreign-ip";
    tests_run++;

    uint8_t payload[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(k_token, payload);

    arm_fresh(tag);

    /* Consumed — it is punch-shaped, so it must never reach GekkoNet —
     * but nothing else may move. The s_peer_ip compare at
     * late_punch.c:280: "No answer either way: answering would hand a
     * replayer a confirmation oracle." test_late_punch.c's A5 case
     * cannot see this: it observes the send side two full cadence
     * intervals later, by which time the ordinary keepalive has fired. */
    CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                        "203.0.113.7", 4444));
    {
        const Peek p = peek();
        check_eq_int(tag, (long)p.prompt, 0, "a foreign-IP punch schedules NO answer");
        check_eq_str(tag, p.ip, PEER_IP, "the target IP is unchanged");
        check_eq_int(tag, p.port, PEER_PORT, "the target port is unchanged");
        check_eq_int(tag, p.relearns, 0, "and no relearn was spent");
    }
    CHECK(tag, !LatePunch_TakeRelearn(NULL, 0, NULL));

    /* Same, from a foreign IP that reuses the peer's PORT — the port is
     * not identity. */
    CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                        "198.51.100.9", PEER_PORT));
    check_eq_int(tag, (long)peek().prompt, 0,
                 "a foreign IP on the peer's own port is still foreign");

    /* Near-miss IPs. The compare is strcmp on the peer IP inside
     * LatePunch_HandleDatagram (late_punch.c:212; the two compare
     * sites are at lines 229 and 280), so a
     * source whose text merely EXTENDS or PREFIXES the peer's must not
     * match — "10.0.0.50" is not "10.0.0.5". A compare that used
     * strncmp with the peer's length would accept the first of these and
     * hand a same-subnet attacker the retarget. */
    {
        static const char* const near_miss[] = {
            "10.0.0.50", "10.0.0.", "110.0.0.5", "10.0.0.5 ", "",
        };
        int cases = 0;
        for (size_t i = 0; i < sizeof(near_miss) / sizeof(near_miss[0]); i++) {
            CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                                near_miss[i], 6001));
            const Peek p = peek();
            check_eq_int(tag, (long)p.prompt, 0, "a near-miss IP schedules no answer");
            check_eq_int(tag, p.port, PEER_PORT, "and does not move the target");
            cases++;
        }
        check_eq_int(tag, cases, 5, "every near-miss IP was tried");
    }

    /* The contrast that keeps this test honest: the SAME payload from the
     * established endpoint DOES schedule the answer. Without this, every
     * assertion above would still pass on a build where the prompt flag
     * is simply never set. */
    CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                        PEER_IP, PEER_PORT));
    {
        const Peek p = peek();
        check_eq_int(tag, (long)p.prompt, 1, "the real peer DOES schedule an answer");
        check_eq_int(tag, p.port, PEER_PORT, "at the established port");
        check_eq_int(tag, p.relearns, 0, "with no relearn");
    }

    LatePunch_Disarm();
}

/* ====================================================================== */
/* 5. The relearn cap: the refused move changes nothing.                  */
/* ====================================================================== */

static void test5_relearn_cap(void) {
    const char* tag = "relearn-cap";
    tests_run++;

    uint8_t payload[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(k_token, payload);

    arm_fresh(tag);

    /* Spend the budget. Task #133: a move now costs a VERIFIED round trip
     * — the punch only nominates, and the target does not follow until
     * the nominee returns the nonce we drew for it. */
    uint16_t port = PEER_PORT;
    int moves = 0;
    for (int i = 0; i < LATE_PUNCH_MAX_RELEARNS; i++) {
        const uint16_t next = (uint16_t)(PEER_PORT + 1 + i);
        CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                            PEER_IP, next));
        /* The nomination ALONE must change nothing that reaches the wire. */
        check_eq_int(tag, peek().port, port,
                     "a nomination does not move the target by itself");
        check_eq_int(tag, peek().relearns, i,
                     "and does not spend the budget by itself");
        CHECK(tag, !LatePunch_TakeRelearn(NULL, 0, NULL));
        CHECK(tag, answer_challenge(tag, next));
        char ip[64] = { 0 };
        uint16_t got = 0;
        CHECK(tag, LatePunch_TakeRelearn(ip, sizeof(ip), &got));
        check_eq_str(tag, ip, PEER_IP, "the relearn keeps the IP");
        check_eq_int(tag, got, next, "the relearn reports the new port");
        CHECK(tag, !LatePunch_TakeRelearn(NULL, 0, NULL)); /* one-shot */
        const Peek p = peek();
        check_eq_int(tag, p.port, next, "the send target followed");
        check_eq_int(tag, p.relearns, i + 1, "the counter advanced by one");
        check_eq_int(tag, (long)p.prompt, 1, "a verified move schedules the answer");
        CHECK(tag, !chal().active); /* promotion clears the candidate slot */
        port = next;
        moves++;
    }
    check_eq_int(tag, moves, LATE_PUNCH_MAX_RELEARNS,
                 "exactly the budgeted number of moves was spent");
    check_eq_int(tag, peek().relearns, LATE_PUNCH_MAX_RELEARNS,
                 "the counter is at the cap");

    /* THE MOVE PAST THE BUDGET. Consumed — it is a valid, authenticated
     * punch and must not reach GekkoNet — and refused in every other
     * respect. Task #133: refused EARLIER than before, at nomination, so
     * no challenge is even opened; there is therefore no nonce in flight
     * for the source to answer, and no round trip that could move the
     * target (late_punch.c:96-102).
     *
     * The prompt flag is NOT asserted false here, and that is a measured
     * choice rather than an omission. It is sticky: only LatePunch_Tick
     * clears it, at the moment it actually sends, so after the accepted
     * move above an answer is already owed and the flag is legitimately
     * still set. What matters — and what is asserted — is that any owed
     * answer goes to the LEARNED endpoint. Distinguishing "the refusal
     * path did not ALSO raise the flag" would take a Tick, which takes a
     * socket, which is test_late_punch.c's job. */
    {
        const uint16_t rogue = (uint16_t)(PEER_PORT + 100);
        CHECK(tag, rogue != port);
        CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                            PEER_IP, rogue));
        CHECK(tag, !chal().active); /* no challenge opened past the budget */
        CHECK(tag, !LatePunch_TakeRelearn(NULL, 0, NULL));
        const Peek p = peek();
        check_eq_int(tag, p.port, port, "the target is still the LEARNED port");
        CHECK(tag, p.port != rogue);
        check_eq_int(tag, p.relearns, LATE_PUNCH_MAX_RELEARNS,
                     "the counter did not move");
        check_eq_str(tag, p.ip, PEER_IP, "and the target IP is untouched");
    }

    /* Repeating it does not eventually win. */
    {
        int rogue_cases = 0;
        for (int i = 0; i < 25; i++) {
            CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                                PEER_IP, (uint16_t)(6000 + i)));
            CHECK(tag, !chal().active);
            rogue_cases++;
        }
        check_eq_int(tag, rogue_cases, 25, "every rogue repeat ran");
        CHECK(tag, !LatePunch_TakeRelearn(NULL, 0, NULL));
        const Peek p = peek();
        check_eq_int(tag, p.port, port, "25 more attempts still did not move it");
        check_eq_int(tag, p.relearns, LATE_PUNCH_MAX_RELEARNS, "nor the counter");
    }

    /* A capped layer is not a DEAF layer: a punch from the retained
     * endpoint is still consumed and still leaves an answer owed to that
     * endpoint. */
    CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                        PEER_IP, port));
    {
        const Peek p = peek();
        check_eq_int(tag, (long)p.prompt, 1, "an answer is owed");
        check_eq_int(tag, p.port, port, "to the retained endpoint");
    }

    /* Re-arming restores the budget — the cap is per session, not for the
     * life of the process. */
    arm_fresh(tag);
    CHECK(tag, move_peer(tag, payload, (uint16_t)(PEER_PORT + 1)));
    CHECK(tag, LatePunch_TakeRelearn(NULL, 0, NULL));
    check_eq_int(tag, peek().relearns, 1, "a fresh arm starts the budget over");

    LatePunch_Disarm();
}

/* ====================================================================== */
/* 6. The answer targets the LEARNED endpoint, in both directions.        */
/* ====================================================================== */

static void test6_answer_targets_learned(void) {
    const char* tag = "learned-target";
    tests_run++;

    uint8_t payload[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(k_token, payload);

    /* Direction 1 — an ACCEPTED move: the target becomes the source, and
     * only then. The order matters: if the answer were scheduled against
     * a target that had not yet been updated, the confirming payload
     * would go to the peer's DEAD port (the S2 retry closed it, which is
     * why the peer moved) and the rescue would silently never land. */
    arm_fresh(tag);
    {
        const uint16_t moved = (uint16_t)(PEER_PORT + 7);
        CHECK(tag, move_peer(tag, payload, moved));
        const Peek p = peek();
        check_eq_int(tag, p.port, moved, "the target is the new source port");
        CHECK(tag, p.port != PEER_PORT);
        check_eq_int(tag, (long)p.prompt, 1, "and the answer is scheduled");
        char ip[64] = { 0 };
        uint16_t got = 0;
        CHECK(tag, LatePunch_TakeRelearn(ip, sizeof(ip), &got));
        check_eq_int(tag, got, moved,
                     "and the retarget handed to netplay.c is the same port");
        check_eq_int(tag, p.port, got,
                     "the send target and the reported retarget agree");
    }

    /* Direction 2 — a REFUSED move, reached by exhausting the budget:
     * the target stays learned while the source is something else. This
     * is the case that discriminates "answer the learned endpoint" from
     * "answer whoever just spoke", because in direction 1 the two are
     * the same value. */
    {
        const uint16_t port = (uint16_t)(PEER_PORT + 7);
        check_eq_int(tag, peek().relearns, LATE_PUNCH_MAX_RELEARNS,
                     "the budget is spent");
        const uint16_t source = (uint16_t)(PEER_PORT + 900);
        CHECK(tag, source != port);
        CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                            PEER_IP, source));
        const Peek p = peek();
        check_eq_int(tag, p.port, port, "the target is the LEARNED port");
        CHECK(tag, p.port != source); /* ...and NOT the datagram's source */
    }

    LatePunch_Disarm();
}

/* ====================================================================== */
/* 7. Bad tokens, non-punch traffic, and the armed/disarmed boundary.     */
/* ====================================================================== */

static void test7_bad_token_and_lifecycle(void) {
    const char* tag = "lifecycle";
    tests_run++;

    uint8_t payload[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(k_token, payload);

    /* Disarmed: nothing is consumed and nothing accrues. */
    LatePunch_Disarm();
    CHECK(tag, !LatePunch_IsArmed());
    CHECK(tag, !LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                         PEER_IP, PEER_PORT));
    CHECK(tag, !LatePunch_TakeRelearn(NULL, 0, NULL));

    arm_fresh(tag);

    /* A WRONG TOKEN FROM THE PEER'S OWN ENDPOINT. Consumed, because it is
     * punch-shaped and must not reach GekkoNet — but it must NOT schedule
     * an answer. Answering a wrong-token punch turns the layer into an
     * oracle: a guesser learns "that byte was right" from whether a reply
     * comes back, and it can guess from the peer's own address. Every bit
     * of the token is swept, because a compare that reads seven of the
     * eight bytes answers one guess in 256. */
    {
        int flips = 0;
        for (int i = 0; i < STUN_PUNCH_TOKEN_LEN; i++) {
            for (int b = 0; b < 8; b++) {
                uint8_t bad[STUN_PUNCH_PAYLOAD_LEN];
                memcpy(bad, payload, sizeof(bad));
                bad[STUN_PUNCH_PREFIX_LEN + i] ^= (uint8_t)(1u << b);
                CHECK(tag, LatePunch_HandleDatagram(bad, (int)sizeof(bad),
                                                    PEER_IP, PEER_PORT));
                flips++;
            }
        }
        check_eq_int(tag, flips, STUN_PUNCH_TOKEN_LEN * 8,
                     "all 64 wrong-token variants were offered");
        const Peek p = peek();
        check_eq_int(tag, (long)p.prompt, 0,
                     "not one wrong token scheduled an answer");
        check_eq_int(tag, p.relearns, 0, "nor spent a relearn");
        check_eq_int(tag, p.port, PEER_PORT, "nor moved the target");
    }

    /* A wrong token from a NEW port must not retarget either — the
     * redirect oracle test_late_punch.c's A6 names, swept over all 64
     * variants and now also asserting the answer schedule. */
    {
        int flips = 0;
        for (int i = 0; i < STUN_PUNCH_TOKEN_LEN; i++) {
            for (int b = 0; b < 8; b++) {
                uint8_t bad[STUN_PUNCH_PAYLOAD_LEN];
                memcpy(bad, payload, sizeof(bad));
                bad[STUN_PUNCH_PREFIX_LEN + i] ^= (uint8_t)(1u << b);
                CHECK(tag, LatePunch_HandleDatagram(bad, (int)sizeof(bad),
                                                    PEER_IP, (uint16_t)(7000 + flips)));
                flips++;
            }
        }
        check_eq_int(tag, flips, 64, "all 64 were offered from moving ports");
        CHECK(tag, !LatePunch_TakeRelearn(NULL, 0, NULL));
        const Peek p = peek();
        check_eq_int(tag, p.port, PEER_PORT, "the target never moved");
        check_eq_int(tag, (long)p.prompt, 0, "and nothing was scheduled");
    }

    /* NON-punch traffic is NOT consumed — it belongs to MIST and
     * GekkoNet, and swallowing it here would starve the handshake. */
    {
        static const uint8_t mist_hello[] = { 'M', 'I', 'S', 'T', 0x01, 0x00, 0x04 };
        CHECK(tag, !LatePunch_HandleDatagram(mist_hello, (int)sizeof(mist_hello),
                                             PEER_IP, PEER_PORT));
        int gekko_cases = 0;
        for (int t = 1; t <= 7; t++) {
            uint8_t g[24];
            memset(g, 0, sizeof(g));
            g[0] = (uint8_t)t;
            CHECK(tag, !LatePunch_HandleDatagram(g, (int)sizeof(g),
                                                 PEER_IP, PEER_PORT));
            gekko_cases++;
        }
        check_eq_int(tag, gekko_cases, 7, "every GekkoNet packet type was offered");
        CHECK(tag, !LatePunch_HandleDatagram(NULL, STUN_PUNCH_PAYLOAD_LEN,
                                             PEER_IP, PEER_PORT));
        CHECK(tag, !LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                             NULL, PEER_PORT));
        check_eq_int(tag, (long)peek().prompt, 0,
                     "none of it scheduled an answer");
    }

    /* Truncated and oversized punch-shaped datagrams: consumed (the
     * prefix is there) but never authenticated. */
    {
        CHECK(tag, LatePunch_HandleDatagram(payload, STUN_PUNCH_PAYLOAD_LEN - 1,
                                            PEER_IP, PEER_PORT));
        uint8_t big[STUN_PUNCH_PAYLOAD_LEN + 8];
        memset(big, 0, sizeof(big));
        memcpy(big, payload, sizeof(payload));
        CHECK(tag, LatePunch_HandleDatagram(big, (int)sizeof(big),
                                            PEER_IP, PEER_PORT));
        CHECK(tag, !LatePunch_HandleDatagram(payload, STUN_PUNCH_PREFIX_LEN - 1,
                                             PEER_IP, PEER_PORT));
        check_eq_int(tag, (long)peek().prompt, 0,
                     "no length-mangled punch is answered");
    }

    /* Disarm drops a PENDING relearn on the floor: netplay.c must never
     * apply a retarget belonging to a session that has ended. */
    CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                        PEER_IP, (uint16_t)(PEER_PORT + 3)));
    LatePunch_Disarm();
    CHECK(tag, !LatePunch_IsArmed());
    CHECK(tag, !LatePunch_TakeRelearn(NULL, 0, NULL));

    /* Arm refuses degenerate arguments and stays disarmed. */
    LatePunch_Arm(NULL, k_token, PEER_IP, PEER_PORT);
    CHECK(tag, !LatePunch_IsArmed());
    LatePunch_Arm(FAKE_SOCK, NULL, PEER_IP, PEER_PORT);
    CHECK(tag, !LatePunch_IsArmed());
    LatePunch_Arm(FAKE_SOCK, k_token, NULL, PEER_PORT);
    CHECK(tag, !LatePunch_IsArmed());
    LatePunch_Arm(FAKE_SOCK, k_token, "", PEER_PORT);
    CHECK(tag, !LatePunch_IsArmed());
    LatePunch_Arm(FAKE_SOCK, k_token, PEER_IP, 0);
    CHECK(tag, !LatePunch_IsArmed());
    CHECK(tag, !LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                         PEER_IP, PEER_PORT));

    LatePunch_Disarm();
}

/* ====================================================================== */
/* 8. Task #133: the liveness challenge is what moves the target — not    */
/*    the token, which a room-code holder already has.                    */
/* ====================================================================== */

static void test8_liveness_gate(void) {
    const char* tag = "liveness";
    tests_run++;

    uint8_t payload[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(k_token, payload);

    /* (a) A NOMINEE THAT NEVER ANSWERS NEVER BECOMES THE TARGET. This is
     * the pin: pre-#133 this single datagram WAS the retarget. */
    arm_fresh(tag);
    {
        const uint16_t rogue = (uint16_t)(PEER_PORT + 11);
        CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                            PEER_IP, rogue));
        const Peek p = peek();
        check_eq_int(tag, p.port, PEER_PORT, "the target did NOT move");
        CHECK(tag, p.port != rogue);
        check_eq_int(tag, p.relearns, 0, "and no budget was spent");
        check_eq_int(tag, (long)p.prompt, 0,
                     "an unverified nominee is not answered either");
        CHECK(tag, !LatePunch_TakeRelearn(NULL, 0, NULL));
        const Chal c = chal();
        CHECK(tag, c.active);
        check_eq_int(tag, c.port, rogue, "but it IS being challenged");
    }

    /* (b) A WRONG NONCE IS NOT AN ANSWER. The nominee holds the token —
     * it is room-code-derived — so the token cannot be what separates the
     * cases; only the nonce can. */
    {
        const uint16_t rogue = (uint16_t)(PEER_PORT + 11);
        uint8_t guess[STUN_PUNCH_PROBE_NONCE_LEN];
        const Chal c = chal();
        memcpy(guess, c.nonce, sizeof(guess));
        int flips = 0;
        for (size_t bit = 0; bit < STUN_PUNCH_PROBE_NONCE_LEN * 8; bit++) {
            guess[bit / 8] ^= (uint8_t)(1u << (bit % 8)); /* one bit wrong */
            uint8_t resp[STUN_PUNCH_PROBE_PAYLOAD_LEN];
            Stun_BuildProbePayload(k_token, STUN_PUNCH_PROBE_RESPONSE, guess,
                                   resp);
            CHECK(tag, LatePunch_HandleDatagram(resp, (int)sizeof(resp),
                                                PEER_IP, rogue));
            CHECK(tag, peek().port == PEER_PORT);
            CHECK(tag, peek().relearns == 0);
            guess[bit / 8] ^= (uint8_t)(1u << (bit % 8)); /* restore */
            flips++;
        }
        check_eq_int(tag, flips, 64, "every single-bit nonce error was tried");
        CHECK(tag, chal().active); /* a wrong answer does not close it */
    }

    /* (c) THE RIGHT NONCE FROM THE WRONG PORT IS NOT AN ANSWER EITHER.
     * The challenge went to one endpoint; return-routability means the
     * answer has to come back from it. */
    {
        const Chal c = chal();
        uint8_t resp[STUN_PUNCH_PROBE_PAYLOAD_LEN];
        Stun_BuildProbePayload(k_token, STUN_PUNCH_PROBE_RESPONSE, c.nonce,
                               resp);
        int wrong_ports = 0;
        const uint16_t others[] = { (uint16_t)(c.port + 1),
                                    (uint16_t)(c.port - 1), PEER_PORT, 1u,
                                    65535u };
        for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
            CHECK(tag, LatePunch_HandleDatagram(resp, (int)sizeof(resp),
                                                PEER_IP, others[i]));
            check_eq_int(tag, peek().port, PEER_PORT, "still not moved");
            check_eq_int(tag, peek().relearns, 0, "still no budget spent");
            wrong_ports++;
        }
        check_eq_int(tag, wrong_ports, 5, "every wrong source port was tried");

        /* And from a FOREIGN IP with the right nonce and the right port. */
        CHECK(tag, LatePunch_HandleDatagram(resp, (int)sizeof(resp),
                                            "203.0.113.9", c.port));
        check_eq_int(tag, peek().port, PEER_PORT, "a foreign IP cannot answer");
        check_eq_int(tag, peek().relearns, 0, "nor spend the budget");
    }

    /* (d) THE LEGITIMATE PEER IS STILL RELEARNABLE. The whole point of
     * the layer survives: the endpoint that actually receives the
     * challenge and answers it does become the target. Without this the
     * rescue is dead and the other three assertions are worthless. */
    {
        const uint16_t rogue = (uint16_t)(PEER_PORT + 11);
        CHECK(tag, answer_challenge(tag, rogue));
        const Peek p = peek();
        check_eq_int(tag, p.port, rogue, "the verified nominee IS the target");
        check_eq_int(tag, p.relearns, 1, "and THAT is what spends the budget");
        check_eq_int(tag, (long)p.prompt, 1, "and it is answered");
        char ip[64] = { 0 };
        uint16_t got = 0;
        CHECK(tag, LatePunch_TakeRelearn(ip, sizeof(ip), &got));
        check_eq_int(tag, got, rogue, "and netplay.c is told to retarget");
        CHECK(tag, !chal().active);
    }

    /* (d2) THE NONCE MUST ACTUALLY VARY. Every assertion above still
     * passes against a module that "draws" a constant, because the test
     * reads the value the module chose. A fixed nonce is exactly the
     * vulnerability this whole mechanism exists to close — a room-code
     * holder would learn it once and answer forever — so vary it and
     * check, the same way test_room_code.c checks RoomCode_GenerateNonce. */
    {
        uint8_t seen[16][STUN_PUNCH_PROBE_NONCE_LEN];
        int drawn = 0;
        for (int i = 0; i < 16; i++) {
            arm_fresh(tag);
            CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                                PEER_IP,
                                                (uint16_t)(PEER_PORT + 60 + i)));
            const Chal c = chal();
            CHECK(tag, c.active);
            memcpy(seen[drawn], c.nonce, STUN_PUNCH_PROBE_NONCE_LEN);
            drawn++;
        }
        check_eq_int(tag, drawn, 16, "sixteen challenges were drawn");
        int distinct_pairs = 0;
        int identical_pairs = 0;
        for (int a = 0; a < drawn; a++) {
            for (int b = a + 1; b < drawn; b++) {
                if (memcmp(seen[a], seen[b], STUN_PUNCH_PROBE_NONCE_LEN) == 0) {
                    identical_pairs++;
                } else {
                    distinct_pairs++;
                }
            }
        }
        check_eq_int(tag, distinct_pairs + identical_pairs, 120,
                     "every pair was compared");
        check_eq_int(tag, identical_pairs, 0,
                     "no two challenges reused a nonce — the draw is not a "
                     "constant and not a counter that repeats");
        /* An all-zero draw is the shape a fail-open CSPRNG wrapper leaves. */
        int nonzero = 0;
        for (int a = 0; a < drawn; a++) {
            for (size_t k = 0; k < STUN_PUNCH_PROBE_NONCE_LEN; k++) {
                if (seen[a][k] != 0) {
                    nonzero++;
                    break;
                }
            }
        }
        check_eq_int(tag, nonzero, 16, "no challenge was all zeroes");
    }

    /* (e) REPLAY: the same response cannot be used twice. The candidate
     * slot is closed, so a recorded response is a stale response. */
    {
        arm_fresh(tag);
        const uint16_t moved = (uint16_t)(PEER_PORT + 31);
        CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                            PEER_IP, moved));
        const Chal c = chal();
        uint8_t resp[STUN_PUNCH_PROBE_PAYLOAD_LEN];
        Stun_BuildProbePayload(k_token, STUN_PUNCH_PROBE_RESPONSE, c.nonce,
                               resp);
        CHECK(tag, LatePunch_HandleDatagram(resp, (int)sizeof(resp), PEER_IP,
                                            moved));
        check_eq_int(tag, peek().port, moved, "the first answer landed");
        check_eq_int(tag, peek().relearns, 1, "and spent the budget");
        /* Replay it. */
        CHECK(tag, LatePunch_HandleDatagram(resp, (int)sizeof(resp), PEER_IP,
                                            moved));
        check_eq_int(tag, peek().relearns, 1, "the replay bought nothing");
    }

    LatePunch_Disarm();
}

/* ====================================================================== */
/* 9. Task #133: a SECOND distinct-port move is refused, and a flood of   */
/*    nominations cannot displace the one being challenged.               */
/* ====================================================================== */

static void test9_second_move_refused(void) {
    const char* tag = "second-move";
    tests_run++;

    uint8_t payload[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(k_token, payload);

    /* The capture shape, run end to end. One verified move is the whole
     * budget (LATE_PUNCH_MAX_RELEARNS == 1), so the port that answered
     * first owns the session; a later port cannot take it, and — this is
     * the part the old cap got backwards — the later port cannot even
     * open a challenge, so there is no round trip for it to win. */
    check_eq_int(tag, LATE_PUNCH_MAX_RELEARNS, 1,
                 "the budget is ONE verified move");

    arm_fresh(tag);
    const uint16_t first = (uint16_t)(PEER_PORT + 3);
    CHECK(tag, move_peer(tag, payload, first));
    check_eq_int(tag, peek().relearns, 1, "the budget is spent");
    (void)LatePunch_TakeRelearn(NULL, 0, NULL);

    int refused = 0;
    for (int i = 0; i < 8; i++) {
        const uint16_t next = (uint16_t)(PEER_PORT + 40 + i);
        CHECK(tag, next != first);
        CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                            PEER_IP, next));
        CHECK(tag, !chal().active); /* no challenge is opened at all */
        check_eq_int(tag, peek().port, first, "the target stays with the first");
        check_eq_int(tag, peek().relearns, 1, "and the budget stays spent");
        CHECK(tag, !LatePunch_TakeRelearn(NULL, 0, NULL));
        refused++;
    }
    check_eq_int(tag, refused, 8,
                 "all eight of the ports that used to WIN under the old cap "
                 "were refused");

    /* FIRST COME holds the candidate slot: while one nominee is being
     * challenged, a different port cannot displace it, so a flood cannot
     * make the module challenge whoever spoke last. */
    arm_fresh(tag);
    {
        const uint16_t held = (uint16_t)(PEER_PORT + 5);
        CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                            PEER_IP, held));
        check_eq_int(tag, chal().port, held, "the first nominee is challenged");
        uint8_t held_nonce[STUN_PUNCH_PROBE_NONCE_LEN];
        memcpy(held_nonce, chal().nonce, sizeof(held_nonce));
        int floods = 0;
        for (int i = 0; i < 16; i++) {
            CHECK(tag, LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                                PEER_IP,
                                                (uint16_t)(7000 + i)));
            check_eq_int(tag, chal().port, held, "the slot did not move");
            CHECK(tag, memcmp(chal().nonce, held_nonce,
                              STUN_PUNCH_PROBE_NONCE_LEN) == 0);
            floods++;
        }
        check_eq_int(tag, floods, 16, "every displacement attempt ran");
        check_eq_int(tag, peek().port, PEER_PORT, "and nothing moved");
    }

    LatePunch_Disarm();
}

/* ====================================================================== */
/* 10. Task #133: the probe frames themselves — who gets answered, and    */
/*     what never reaches GekkoNet.                                       */
/* ====================================================================== */

static void test10_probe_frames(void) {
    const char* tag = "probe-frames";
    tests_run++;

    static const uint8_t k_nonce[STUN_PUNCH_PROBE_NONCE_LEN] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04
    };
    uint8_t challenge[STUN_PUNCH_PROBE_PAYLOAD_LEN];
    Stun_BuildProbePayload(k_token, STUN_PUNCH_PROBE_CHALLENGE, k_nonce,
                           challenge);

    /* The frame is punch-PREFIXED on purpose: every receive path in the
     * tree already drops punch-prefixed traffic before GekkoNet
     * (sdl_net_adapter.c:386-393), and the host gate demands the exact
     * 17-byte payload so it still ignores this (direct_p2p.c:1063-1066). */
    CHECK(tag, Stun_HasPunchPrefix(challenge, (int)sizeof(challenge)));
    CHECK(tag, !Stun_IsPunchPayload(challenge, (int)sizeof(challenge), k_token));
    check_eq_int(tag, (long)sizeof(challenge), 26, "the probe frame is 26 bytes");

    /* Round-trip the codec, including the kind byte. */
    {
        uint8_t kind = 0;
        uint8_t got[STUN_PUNCH_PROBE_NONCE_LEN];
        CHECK(tag, Stun_ParseProbePayload(challenge, (int)sizeof(challenge),
                                          k_token, &kind, got));
        check_eq_int(tag, kind, STUN_PUNCH_PROBE_CHALLENGE, "kind survives");
        CHECK(tag, memcmp(got, k_nonce, sizeof(got)) == 0);

        /* Length is EXACT, like Stun_IsPunchPayload's. */
        int lens = 0;
        for (int n = -4; n <= 40; n++) {
            if (n == STUN_PUNCH_PROBE_PAYLOAD_LEN) {
                continue;
            }
            CHECK(tag, !Stun_ParseProbePayload(challenge, n, k_token, NULL,
                                               NULL));
            lens++;
        }
        check_eq_int(tag, lens, 44, "every other length was tried");

        /* Every single-bit token error is rejected. */
        int bits = 0;
        for (size_t bit = 0; bit < STUN_PUNCH_TOKEN_LEN * 8; bit++) {
            uint8_t bad[STUN_PUNCH_PROBE_PAYLOAD_LEN];
            memcpy(bad, challenge, sizeof(bad));
            bad[STUN_PUNCH_PREFIX_LEN + bit / 8] ^= (uint8_t)(1u << (bit % 8));
            CHECK(tag, !Stun_ParseProbePayload(bad, (int)sizeof(bad), k_token,
                                               NULL, NULL));
            bits++;
        }
        check_eq_int(tag, bits, 64, "every token bit was flipped");

        /* An unknown kind byte is not a probe frame. */
        int kinds = 0;
        for (int k = 0; k < 256; k++) {
            if (k == STUN_PUNCH_PROBE_CHALLENGE || k == STUN_PUNCH_PROBE_RESPONSE) {
                continue;
            }
            uint8_t bad[STUN_PUNCH_PROBE_PAYLOAD_LEN];
            memcpy(bad, challenge, sizeof(bad));
            bad[STUN_PUNCH_PAYLOAD_LEN] = (uint8_t)k;
            CHECK(tag, !Stun_ParseProbePayload(bad, (int)sizeof(bad), k_token,
                                               NULL, NULL));
            kinds++;
        }
        check_eq_int(tag, kinds, 254, "every other kind byte was tried");
    }

    /* Constant-time nonce compare, exercised on every single-bit error. */
    {
        uint8_t other[STUN_PUNCH_PROBE_NONCE_LEN];
        memcpy(other, k_nonce, sizeof(other));
        CHECK(tag, Stun_ProbeNonceEqual(k_nonce, other));
        int bits = 0;
        for (size_t bit = 0; bit < STUN_PUNCH_PROBE_NONCE_LEN * 8; bit++) {
            other[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            CHECK(tag, !Stun_ProbeNonceEqual(k_nonce, other));
            other[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            bits++;
        }
        check_eq_int(tag, bits, 64, "every nonce bit was flipped");

        /* FOLD TRAP. Every single-bit flip above is also caught by an
         * accumulator that XORs instead of ORs; two flips at the SAME bit
         * position in different bytes are not (they cancel). Same trap as
         * the token compare's, and the same reason: `diff |=` -> `diff ^=`
         * is a one-character mutation that survives a pure bit sweep. */
        memcpy(other, k_nonce, sizeof(other));
        other[0] ^= 0x01u;
        other[3] ^= 0x01u;
        CHECK(tag, !Stun_ProbeNonceEqual(k_nonce, other));

        /* ORDER TRAP. A compare that sums or sorts bytes instead of
         * comparing them positionally passes both sweeps above. */
        memcpy(other, k_nonce, sizeof(other));
        {
            const uint8_t swap = other[0];
            other[0] = other[1];
            other[1] = swap;
        }
        CHECK(tag, k_nonce[0] != k_nonce[1]); /* the swap is observable */
        CHECK(tag, !Stun_ProbeNonceEqual(k_nonce, other));
    }

    /* A CHALLENGE from the established peer parks an answer aimed at the
     * SOURCE port it arrived from — that is the mirror of what we demand
     * of the peer, and it is what keeps the rescue working when the other
     * side is the one that has handed off. */
    arm_fresh(tag);
    {
        CHECK(tag, LatePunch_HandleDatagram(challenge, (int)sizeof(challenge),
                                            PEER_IP, (uint16_t)(PEER_PORT + 2)));
        const Chal c = chal();
        CHECK(tag, c.echo_pending);
        check_eq_int(tag, c.echo_port, (uint16_t)(PEER_PORT + 2),
                     "the answer goes back to where the challenge came from");
        CHECK(tag, !c.active); /* a challenge is not a nomination */
        check_eq_int(tag, peek().port, PEER_PORT,
                     "and answering a challenge moves nothing");
        check_eq_int(tag, peek().relearns, 0, "and spends no budget");
    }

    /* A CHALLENGE from a foreign IP is consumed and NOT answered — the
     * same no-oracle rule the foreign-IP punch path follows. */
    arm_fresh(tag);
    {
        CHECK(tag, LatePunch_HandleDatagram(challenge, (int)sizeof(challenge),
                                            "203.0.113.7", 4444));
        CHECK(tag, !chal().echo_pending);
        check_eq_int(tag, (long)peek().prompt, 0, "and no punch is scheduled");
    }

    /* A RESPONSE is never echoed — that is what stops two peers that both
     * probe from ping-ponging one nonce forever. */
    arm_fresh(tag);
    {
        uint8_t resp[STUN_PUNCH_PROBE_PAYLOAD_LEN];
        Stun_BuildProbePayload(k_token, STUN_PUNCH_PROBE_RESPONSE, k_nonce,
                               resp);
        CHECK(tag, LatePunch_HandleDatagram(resp, (int)sizeof(resp), PEER_IP,
                                            (uint16_t)(PEER_PORT + 2)));
        const Chal c = chal();
        CHECK(tag, !c.echo_pending);
        CHECK(tag, !c.active);
        check_eq_int(tag, peek().port, PEER_PORT,
                     "an unsolicited response moves nothing");
        check_eq_int(tag, peek().relearns, 0, "and spends no budget");
    }

    /* A probe frame carrying a DIFFERENT room's token is not a probe at
     * all: it falls through to the bad-token path, is consumed, and is
     * never answered. */
    arm_fresh(tag);
    {
        static const uint8_t k_other_token[STUN_PUNCH_TOKEN_LEN] = {
            0x99, 0x98, 0x97, 0x96, 0x95, 0x94, 0x93, 0x92
        };
        uint8_t foreign[STUN_PUNCH_PROBE_PAYLOAD_LEN];
        Stun_BuildProbePayload(k_other_token, STUN_PUNCH_PROBE_CHALLENGE,
                               k_nonce, foreign);
        CHECK(tag, LatePunch_HandleDatagram(foreign, (int)sizeof(foreign),
                                            PEER_IP, PEER_PORT));
        const Chal c = chal();
        CHECK(tag, !c.echo_pending);
        CHECK(tag, !c.active);
        check_eq_int(tag, (long)peek().prompt, 0, "and nothing is owed to it");
    }

    LatePunch_Disarm();
}

/* ====================================================================== */

int Netplay_Test_PunchPredicates(void) {
    /* late_punch.c logs a relearn through Netplay_LogConnectEvent, which
     * opens a file under the pref path. SDL_Init(0) initialises no
     * subsystem but leaves SDL's own state consistent for that path. */
    (void)SDL_Init(0);

    test1_binding_response();
    test2_punch_prefix();
    test3_punch_payload();
    test4_foreign_ip_no_answer();
    test5_relearn_cap();
    test6_answer_targets_learned();
    test7_bad_token_and_lifecycle();
    test8_liveness_gate();
    test9_second_move_refused();
    test10_probe_frames();

    LatePunch_Disarm();

    /* --- the anti-vacuous gates, evaluated LAST. Hard failures. */
    if (tests_run != EXPECTED_TESTS) {
        fprintf(stderr,
                "[test_punch_predicates] COVERAGE FAIL: ran %d test(s), expected "
                "exactly %d — a test was removed, skipped, or never registered\n",
                tests_run, EXPECTED_TESTS);
        fail_count++;
    }
    if (checks_run < EXPECTED_MIN_CHECKS) {
        fprintf(stderr,
                "[test_punch_predicates] COVERAGE FAIL: only %d assertion(s) ran, "
                "floor is %d — the bodies are not executing\n",
                checks_run, EXPECTED_MIN_CHECKS);
        fail_count++;
    }

    fprintf(stderr,
            "[test_punch_predicates] summary: %d test(s), %d assertion(s), "
            "%d failure(s)\n",
            tests_run, checks_run, fail_count);
    if (fail_count == 0) {
        fprintf(stderr, "[test_punch_predicates] OK\n");
    }
    return fail_count == 0 ? 0 : 1;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_PunchPredicates(void) {
    /* "not compiled in", spelled exactly that way: tools/gates/run-gates.sh
     * greps for that phrase so an exit-2 misbuild is recorded RED instead
     * of being mistaken for a pass. */
    fprintf(stderr,
            "[test_punch_predicates] not compiled in; rebuild with "
            "EXTRA_CMAKE_ARGS=\"-DENABLE_NETPLAY=ON "
            "-DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS\".\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
