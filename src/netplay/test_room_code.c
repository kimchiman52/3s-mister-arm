/*
 * test_room_code.c — Step 2 test harness for docs/plan-stun-direct-p2p.md.
 *
 * Round-trips a batch of (ip, public_port) tuples through RoomCode_Encode +
 * RoomCode_Decode, then checks that single-character typos and
 * bogus input are rejected. Gated behind ENABLE_NETPLAY_TESTS so
 * release builds don't carry the symbol. Enable with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS='-DENABLE_NETPLAY_TESTS -DNETPLAY_TEST_HOOKS'"
 * (NETPLAY_TEST_HOOKS is required because this build links every
 * src/netplay/test_*.c TU, including test_bilateral_punch.c, which needs
 * that macro to declare DirectP2P_TestHook_IsLanPeer.)
 * Mirrors src/netplay/test_event_queue.c pattern.
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/room_code.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static int fail_count = 0;

static void fail(const char* tag, const char* why) {
    fprintf(stderr, "[test_room_code] FAIL: %s: %s\n", tag, why);
    fail_count++;
}

/* 32-bit IPv4 in network byte order, as returned by inet_aton. */
static uint32_t make_ip_be(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    /* Compose the in-memory bytes as {a, b, c, d}. On every supported
     * host this lays out the four octets in the same order the kernel
     * stores them in sockaddr_in.sin_addr.s_addr. */
    uint8_t buf[4] = { a, b, c, d };
    uint32_t out = 0;
    memcpy(&out, buf, 4);
    return out;
}

static bool ip_eq(uint32_t a_be, uint32_t b_be) {
    return a_be == b_be;
}

static void round_trip(const char* tag,
                       uint32_t ip_be, uint16_t pport) {
    char code[ROOM_CODE_BUF_LEN];
    if (!RoomCode_Encode(ip_be, pport, code)) {
        fail(tag, "encode returned false");
        return;
    }
    if (strlen(code) != ROOM_CODE_DISPLAY_LEN) {
        fprintf(stderr, "[test_room_code] FAIL: %s: wrong display length %zu != %d (code=\"%s\")\n",
                tag, strlen(code), ROOM_CODE_DISPLAY_LEN, code);
        fail_count++;
        return;
    }
    /* Exactly one dash at position 8 (between the 8-char prefix and the
     * 2 payload + 1 check tail). */
    if (code[8] != '-') {
        fprintf(stderr, "[test_room_code] FAIL: %s: dash misplaced in \"%s\"\n", tag, code);
        fail_count++;
        return;
    }

    uint32_t rip = 0;
    uint16_t rpp = 0;
    if (!RoomCode_Decode(code, &rip, &rpp)) {
        fprintf(stderr, "[test_room_code] FAIL: %s: decode rejected \"%s\"\n", tag, code);
        fail_count++;
        return;
    }
    if (!ip_eq(rip, ip_be) || rpp != pport) {
        fprintf(stderr,
                "[test_room_code] FAIL: %s: round-trip mismatch "
                "code=\"%s\" ip=%08x/%08x pp=%u/%u\n",
                tag, code, rip, ip_be, rpp, pport);
        fail_count++;
        return;
    }

    fprintf(stderr, "[test_room_code] %s OK — \"%s\"\n", tag, code);
}

/*
 * Introduce a single character typo at index `pos` in a display-form
 * code by bumping that char's value by +1 in the ISO-36 alphabet.
 * Returns false if the typo happened to produce an invalid character
 * (e.g., a dash position) — caller should pick a different pos.
 */
static bool perturb_one(char* code, size_t pos) {
    char c = code[pos];
    if (c == '-') return false;
    if (c >= '0' && c <= '8') { code[pos] = (char)(c + 1); return true; }
    if (c == '9')              { code[pos] = 'A'; return true; }
    if (c >= 'A' && c <= 'Y') { code[pos] = (char)(c + 1); return true; }
    if (c == 'Z')              { code[pos] = '0'; return true; }
    return false;
}

static void typo_detection(const char* tag,
                           uint32_t ip_be, uint16_t pport) {
    char code[ROOM_CODE_BUF_LEN];
    if (!RoomCode_Encode(ip_be, pport, code)) {
        fail(tag, "encode failed during typo test setup");
        return;
    }

    int checked = 0, missed = 0;
    for (size_t i = 0; i < ROOM_CODE_DISPLAY_LEN; i++) {
        char backup[ROOM_CODE_BUF_LEN];
        memcpy(backup, code, sizeof(backup));
        if (!perturb_one(code, i)) {
            memcpy(code, backup, sizeof(backup));
            continue;
        }
        uint32_t rip = 0;
        uint16_t rpp = 0;
        const bool ok = RoomCode_Decode(code, &rip, &rpp);
        /* A one-char typo might *decode to a different valid tuple* —
         * that's fine and expected for a payload change. What we care
         * about is that the decoder ALSO notices when the typo is in
         * the check-digit position, and that an arbitrary single-char
         * change has at least some chance of being caught. ISO 7064
         * MOD 37,36 detects *every* single-char substitution; so we
         * require ok==false for every typo here (on payload chars the
         * check digit changes, so verification fails). */
        if (ok) {
            /* If the typo happened to produce the same check digit by
             * coincidence, that'd be a collision — ISO 7064 MOD 37,36
             * guarantees this can't happen for single-char substitutions.
             * If we see ok==true, it's a real failure. */
            fprintf(stderr,
                    "[test_room_code] FAIL: %s: typo at pos %zu "
                    "('%c' -> '%c') was NOT detected; decoded tuple "
                    "ip=%08x pp=%u\n",
                    tag, i, backup[i], code[i], rip, rpp);
            missed++;
        }
        checked++;
        memcpy(code, backup, sizeof(backup));
    }

    if (missed > 0) {
        fail_count++;
    } else {
        fprintf(stderr, "[test_room_code] %s typo-detection OK (%d positions)\n",
                tag, checked);
    }
}

static void reject_bogus(void) {
    uint32_t ip = 0;
    uint16_t pp = 0;

    /* Empty. */
    if (RoomCode_Decode("", &ip, &pp)) fail("bogus-empty", "empty string accepted");
    /* Too short. */
    if (RoomCode_Decode("ABCD", &ip, &pp)) fail("bogus-short", "4-char accepted");
    /* Too long (original 14-char / 17-dashed form shouldn't parse now). */
    if (RoomCode_Decode("ABCD-EFGH-JKMN-PQ", &ip, &pp)) fail("bogus-long", "too long accepted");
    /* Contains U (not in Crockford payload alphabet; not remapped by
     * RoomCode_NormalizeInput either). Place at payload position 0. */
    if (RoomCode_Decode("UBCDEFGH-JKM", &ip, &pp)) fail("bogus-U", "U in payload accepted");
    /* NULL code pointer. */
    if (RoomCode_Decode(NULL, &ip, &pp)) fail("bogus-null", "NULL accepted");

    fprintf(stderr, "[test_room_code] bogus-input rejection OK\n");
}

static void normalization_sanity(void) {
    /* Encode, convert display form to lower-case with aliases, decode. */
    char code[ROOM_CODE_BUF_LEN];
    const uint32_t ip_be = make_ip_be(8, 8, 8, 8);
    if (!RoomCode_Encode(ip_be, 80, code)) {
        fail("norm", "encode failed");
        return;
    }

    char aliased[ROOM_CODE_BUF_LEN * 2];
    size_t w = 0;
    for (size_t i = 0; code[i]; i++) {
        char c = code[i];
        /* Lower-case ASCII letters. */
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        /* Inject loose aliases where possible (only safe for
         * characters that the decoder's normalizer maps back; we
         * aren't allowed to corrupt data). This is just a case-fold
         * test — we keep the chars semantic-identical. */
        aliased[w++] = c;
    }
    aliased[w] = '\0';

    uint32_t rip = 0; uint16_t rpp = 0;
    if (!RoomCode_Decode(aliased, &rip, &rpp)) {
        fail("norm-lower", "decode rejected lower-case round-trip");
        return;
    }
    if (!ip_eq(rip, ip_be) || rpp != 80) {
        fail("norm-lower", "decode returned wrong tuple");
        return;
    }
    fprintf(stderr, "[test_room_code] norm lower-case OK — \"%s\"\n", aliased);

    /* Dash-stripped form still decodes. */
    char nodash[ROOM_CODE_BUF_LEN];
    size_t nw = 0;
    for (size_t i = 0; code[i]; i++) {
        if (code[i] != '-') nodash[nw++] = code[i];
    }
    nodash[nw] = '\0';
    rip = 0; rpp = 0;
    if (!RoomCode_Decode(nodash, &rip, &rpp)) {
        fail("norm-nodash", "decode rejected dash-free form");
        return;
    }
    if (!ip_eq(rip, ip_be) || rpp != 80) {
        fail("norm-nodash", "decode returned wrong tuple");
        return;
    }
    fprintf(stderr, "[test_room_code] norm no-dash OK — \"%s\"\n", nodash);
}

int Netplay_Test_RoomCode(void) {
    fail_count = 0;

    /* Edge cases. */
    round_trip("all-zeros",     make_ip_be(0,0,0,0),            0);
    round_trip("all-ones",      make_ip_be(255,255,255,255), 0xFFFF);
    round_trip("loopback",      make_ip_be(127,0,0,1),       54321);
    round_trip("dns-google",    make_ip_be(8,8,8,8),            80);
    round_trip("lan",           make_ip_be(192,168,1,171),   55123);
    round_trip("high-port",     make_ip_be(10,0,0,1),        65535);
    round_trip("asymmetric",    make_ip_be(203,0,113,77),     3478);

    /* Typo detection — ISO 7064 MOD 37,36 guarantees 100% detection of
     * single-character substitutions. */
    typo_detection("lan-typo",     make_ip_be(192,168,1,171), 55123);
    typo_detection("zero-typo",    make_ip_be(0,0,0,0),           0);

    /* Bogus input rejection. */
    reject_bogus();

    /* Normalization sanity (case fold, dash strip). */
    normalization_sanity();

    if (fail_count > 0) {
        fprintf(stderr, "[test_room_code] %d failure(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "[test_room_code] OK — all cases passed\n");
    return 0;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_RoomCode(void) {
    fprintf(stderr,
            "[test_room_code] not compiled in; rebuild with "
            "-DENABLE_NETPLAY_TESTS to enable.\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
