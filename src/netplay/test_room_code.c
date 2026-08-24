/*
 * test_room_code.c — Step 2 test harness for docs/plan-stun-direct-p2p.md,
 * extended for the S4b v2 format (docs/plan-netplay-connection.md §6).
 *
 * Covers: (ip, port, nonce) round-trips, display-format invariants,
 * ISO 7064 check-digit rejection of single-char typos, OLD_FORMAT
 * recognition of checksum-valid v1 (11-char) codes, FUTURE_VERSION
 * detection, nonce range enforcement, CSPRNG nonce variability, and
 * bogus-input rejection. Gated behind ENABLE_NETPLAY_TESTS. Enable with:
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
                       uint32_t ip_be, uint16_t pport, uint16_t nonce) {
    char code[ROOM_CODE_BUF_LEN];
    if (!RoomCode_Encode(ip_be, pport, nonce, code)) {
        fail(tag, "encode returned false");
        return;
    }
    if (strlen(code) != ROOM_CODE_DISPLAY_LEN) {
        fprintf(stderr, "[test_room_code] FAIL: %s: wrong display length %zu != %d (code=\"%s\")\n",
                tag, strlen(code), ROOM_CODE_DISPLAY_LEN, code);
        fail_count++;
        return;
    }
    /* Exactly one dash at position 7 (two 7-char halves of the 14-char
     * code), and the version prefix char in position 0. */
    if (code[7] != '-') {
        fprintf(stderr, "[test_room_code] FAIL: %s: dash misplaced in \"%s\"\n", tag, code);
        fail_count++;
        return;
    }
    if (code[0] != ROOM_CODE_VERSION_CHAR) {
        fprintf(stderr, "[test_room_code] FAIL: %s: version char missing in \"%s\"\n", tag, code);
        fail_count++;
        return;
    }

    uint32_t rip = 0;
    uint16_t rpp = 0;
    uint16_t rn = 0;
    const RoomCodeDecodeResult dr = RoomCode_Decode(code, &rip, &rpp, &rn);
    if (dr != ROOM_CODE_OK) {
        fprintf(stderr, "[test_room_code] FAIL: %s: decode rejected \"%s\" (result=%d)\n",
                tag, code, (int)dr);
        fail_count++;
        return;
    }
    if (!ip_eq(rip, ip_be) || rpp != pport || rn != nonce) {
        fprintf(stderr,
                "[test_room_code] FAIL: %s: round-trip mismatch "
                "code=\"%s\" ip=%08x/%08x pp=%u/%u nonce=%03x/%03x\n",
                tag, code, rip, ip_be, rpp, pport, rn, nonce);
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
                           uint32_t ip_be, uint16_t pport, uint16_t nonce) {
    char code[ROOM_CODE_BUF_LEN];
    if (!RoomCode_Encode(ip_be, pport, nonce, code)) {
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
        uint16_t rn = 0;
        const RoomCodeDecodeResult dr = RoomCode_Decode(code, &rip, &rpp, &rn);
        /* No single-char substitution may EVER decode to ROOM_CODE_OK:
         * payload/check typos are caught by ISO 7064 MOD 37,36 (100%
         * single-substitution detection over the covered chars); a typo
         * at the VERSION position (index 0) is reported as
         * FUTURE_VERSION — still an error, never a silently wrong
         * tuple (see room_code.h for the trade-off note). */
        if (dr == ROOM_CODE_OK) {
            fprintf(stderr,
                    "[test_room_code] FAIL: %s: typo at pos %zu "
                    "('%c' -> '%c') was NOT detected; decoded tuple "
                    "ip=%08x pp=%u nonce=%03x\n",
                    tag, i, backup[i], code[i], rip, rpp, rn);
            missed++;
        }
        if (i == 0 && dr != ROOM_CODE_FUTURE_VERSION && dr != ROOM_CODE_OK) {
            /* Version-char typos must be reported as a version issue,
             * not lumped into MALFORMED. */
            fprintf(stderr,
                    "[test_room_code] FAIL: %s: version-char typo reported %d, "
                    "expected FUTURE_VERSION\n", tag, (int)dr);
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

/* --- v1 (pre-S4b) code recognition ------------------------------------- */

/* Minimal LOCAL v1 encoder (duplicated on purpose — the production v1
 * encoder is gone): 6-byte payload (ip|port_be) -> 48-bit int -> 10
 * Crockford chars (top 2 bits zero) -> ISO 7064 MOD 37,36 check digit
 * over the 10. Byte-for-byte what the pre-S4b RoomCode_Encode emitted,
 * minus the display dash (the decoder strips dashes anyway). */
static bool make_v1_code(uint32_t ip_be, uint16_t pport, char out[12]) {
    static const char kCrock[32] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    static const char kIso[36] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint8_t raw[6];
    memcpy(&raw[0], &ip_be, 4);
    raw[4] = (uint8_t)((pport >> 8) & 0xFF);
    raw[5] = (uint8_t)(pport & 0xFF);
    uint64_t x = 0;
    for (int i = 0; i < 6; i++) x = (x << 8) | raw[i];
    for (int i = 0; i < 10; i++) {
        out[i] = kCrock[(x >> (45 - i * 5)) & 0x1F];
    }
    /* ISO 7064 MOD 37,36 over the 10 chars. */
    int product = 36;
    for (int i = 0; i < 10; i++) {
        const char c = out[i];
        const int v = (c >= '0' && c <= '9') ? c - '0' : 10 + (c - 'A');
        int sum = (product + v) % 37;
        if (sum == 0) sum = 37;
        product = (sum * 2) % 37;
    }
    out[10] = kIso[(37 - product) % 36];
    out[11] = '\0';
    return true;
}

static void old_format_detection(void) {
    uint32_t ip = 0;
    uint16_t pp = 0;
    uint16_t nn = 0;

    /* A checksum-valid v1 code must be recognized as OLD_FORMAT — the
     * "this code is from an older version" UI path. */
    char v1[12];
    make_v1_code(make_ip_be(203, 0, 113, 77), 3478, v1);
    RoomCodeDecodeResult dr = RoomCode_Decode(v1, &ip, &pp, &nn);
    if (dr != ROOM_CODE_OLD_FORMAT) {
        fprintf(stderr,
                "[test_room_code] FAIL: old-format: valid v1 code \"%s\" reported %d, "
                "expected OLD_FORMAT\n", v1, (int)dr);
        fail_count++;
    }
    if (ip != 0 || pp != 0 || nn != 0) {
        fail("old-format", "outputs not zeroed on OLD_FORMAT");
    }

    /* A second v1 tuple, dashed the way v1 displayed it (dash after the
     * 8th char) — normalization must not change the verdict. */
    char v1b[12];
    make_v1_code(make_ip_be(8, 8, 8, 8), 80, v1b);
    char dashed[16];
    memcpy(dashed, v1b, 8);
    dashed[8] = '-';
    memcpy(&dashed[9], &v1b[8], 3);
    dashed[12] = '\0';
    if (RoomCode_Decode(dashed, &ip, &pp, &nn) != ROOM_CODE_OLD_FORMAT) {
        fail("old-format", "dashed v1 code not recognized as OLD_FORMAT");
    }

    /* 11 chars of checksum-INVALID garbage must be MALFORMED, not
     * OLD_FORMAT — garbage should not masquerade as a version issue. */
    char bad[12];
    memcpy(bad, v1, sizeof(bad));
    bad[10] = (bad[10] == 'A') ? 'B' : 'A'; /* break the v1 check digit */
    if (RoomCode_Decode(bad, &ip, &pp, &nn) != ROOM_CODE_MALFORMED) {
        fail("old-format", "checksum-invalid 11-char string reported OLD_FORMAT");
    }

    if (fail_count == 0) {
        fprintf(stderr, "[test_room_code] old-format detection OK (\"%s\" -> OLD_FORMAT)\n", v1);
    }
}

static void future_version_detection(void) {
    uint32_t ip = 0;
    uint16_t pp = 0;
    uint16_t nn = 0;

    char code[ROOM_CODE_BUF_LEN];
    if (!RoomCode_Encode(make_ip_be(198, 51, 100, 7), 6000, 0x0AB, code)) {
        fail("future-version", "encode failed during setup");
        return;
    }
    /* Flip the version char to an unknown value: must be reported as
     * FUTURE_VERSION (checked BEFORE the check digit — a future
     * format's checksum scheme is unknowable). */
    code[0] = '3';
    if (RoomCode_Decode(code, &ip, &pp, &nn) != ROOM_CODE_FUTURE_VERSION) {
        fail("future-version", "unknown version char not reported as FUTURE_VERSION");
        return;
    }
    if (ip != 0 || pp != 0 || nn != 0) {
        fail("future-version", "outputs not zeroed on FUTURE_VERSION");
        return;
    }
    fprintf(stderr, "[test_room_code] future-version detection OK\n");
}

static void nonce_behavior(void) {
    /* Encode rejects out-of-range nonces. */
    char code[ROOM_CODE_BUF_LEN];
    if (RoomCode_Encode(make_ip_be(8, 8, 8, 8), 80, (uint16_t)(ROOM_CODE_NONCE_MASK + 1), code)) {
        fail("nonce", "encode accepted a nonce above ROOM_CODE_NONCE_MASK");
    }

    /* Same (ip, port), different nonces -> different codes. */
    char c1[ROOM_CODE_BUF_LEN];
    char c2[ROOM_CODE_BUF_LEN];
    if (!RoomCode_Encode(make_ip_be(8, 8, 8, 8), 80, 0x001, c1) ||
        !RoomCode_Encode(make_ip_be(8, 8, 8, 8), 80, 0x002, c2)) {
        fail("nonce", "encode failed");
        return;
    }
    if (strcmp(c1, c2) == 0) {
        fail("nonce", "different nonces produced the SAME code");
        return;
    }

    /* CSPRNG nonce generation: in range, and actually varying. 16 draws
     * all landing on one value has probability 4096^-15 — a stuck/
     * constant source is the only realistic way this fires. */
    uint16_t first = 0;
    bool any_diff = false;
    for (int i = 0; i < 16; i++) {
        uint16_t nn = 0xFFFF;
        if (!RoomCode_GenerateNonce(&nn)) {
            fail("nonce", "RoomCode_GenerateNonce failed (CSPRNG unavailable?)");
            return;
        }
        if (nn > ROOM_CODE_NONCE_MASK) {
            fail("nonce", "generated nonce above the 12-bit mask");
            return;
        }
        if (i == 0) first = nn;
        else if (nn != first) any_diff = true;
    }
    if (!any_diff) {
        fail("nonce", "16 generated nonces were all identical — CSPRNG not varying");
        return;
    }
    fprintf(stderr, "[test_room_code] nonce behavior OK (range + variability)\n");
}

static void reject_bogus(void) {
    uint32_t ip = 0;
    uint16_t pp = 0;
    uint16_t nn = 0;

    /* Empty. */
    if (RoomCode_Decode("", &ip, &pp, &nn) != ROOM_CODE_MALFORMED)
        fail("bogus-empty", "empty string not MALFORMED");
    /* Too short. */
    if (RoomCode_Decode("ABCD", &ip, &pp, &nn) != ROOM_CODE_MALFORMED)
        fail("bogus-short", "4-char not MALFORMED");
    /* Wrong lengths bracketing the valid ones (12, 13, 15 chars). */
    if (RoomCode_Decode("2AAAAAAAAAAA", &ip, &pp, &nn) != ROOM_CODE_MALFORMED)
        fail("bogus-12", "12-char not MALFORMED");
    if (RoomCode_Decode("2AAAAAAAAAAAA", &ip, &pp, &nn) != ROOM_CODE_MALFORMED)
        fail("bogus-13", "13-char not MALFORMED");
    if (RoomCode_Decode("2AAAAAAAAAAAAAA", &ip, &pp, &nn) != ROOM_CODE_MALFORMED)
        fail("bogus-15", "15-char not MALFORMED");
    /* Contains U in a payload position (not in the Crockford alphabet;
     * not remapped by RoomCode_NormalizeInput either). Build a valid
     * code and inject the U so length/version/check-order is right. */
    {
        char code[ROOM_CODE_BUF_LEN];
        if (RoomCode_Encode(make_ip_be(10, 0, 0, 1), 65535, 0x3FF, code)) {
            code[2] = 'U'; /* payload position (post-version, pre-dash) */
            if (RoomCode_Decode(code, &ip, &pp, &nn) == ROOM_CODE_OK)
                fail("bogus-U", "U in payload accepted");
        } else {
            fail("bogus-U", "encode failed during setup");
        }
    }
    /* NULL code pointer. */
    if (RoomCode_Decode(NULL, &ip, &pp, &nn) != ROOM_CODE_MALFORMED)
        fail("bogus-null", "NULL not MALFORMED");

    fprintf(stderr, "[test_room_code] bogus-input rejection OK\n");
}

static void normalization_sanity(void) {
    /* Encode, convert display form to lower-case, decode. */
    char code[ROOM_CODE_BUF_LEN];
    const uint32_t ip_be = make_ip_be(8, 8, 8, 8);
    if (!RoomCode_Encode(ip_be, 80, 0x123, code)) {
        fail("norm", "encode failed");
        return;
    }

    char aliased[ROOM_CODE_BUF_LEN * 2];
    size_t w = 0;
    for (size_t i = 0; code[i]; i++) {
        char c = code[i];
        /* Lower-case ASCII letters — a pure case-fold round-trip. */
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        aliased[w++] = c;
    }
    aliased[w] = '\0';

    uint32_t rip = 0; uint16_t rpp = 0; uint16_t rn = 0;
    if (RoomCode_Decode(aliased, &rip, &rpp, &rn) != ROOM_CODE_OK) {
        fail("norm-lower", "decode rejected lower-case round-trip");
        return;
    }
    if (!ip_eq(rip, ip_be) || rpp != 80 || rn != 0x123) {
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
    rip = 0; rpp = 0; rn = 0;
    if (RoomCode_Decode(nodash, &rip, &rpp, &rn) != ROOM_CODE_OK) {
        fail("norm-nodash", "decode rejected dash-free form");
        return;
    }
    if (!ip_eq(rip, ip_be) || rpp != 80 || rn != 0x123) {
        fail("norm-nodash", "decode returned wrong tuple");
        return;
    }
    fprintf(stderr, "[test_room_code] norm no-dash OK — \"%s\"\n", nodash);
}

int Netplay_Test_RoomCode(void) {
    fail_count = 0;

    /* Edge cases across all three fields. */
    round_trip("all-zeros",     make_ip_be(0,0,0,0),            0, 0x000);
    round_trip("all-ones",      make_ip_be(255,255,255,255), 0xFFFF, ROOM_CODE_NONCE_MASK);
    round_trip("loopback",      make_ip_be(127,0,0,1),       54321, 0x001);
    round_trip("dns-google",    make_ip_be(8,8,8,8),            80, 0x800);
    round_trip("lan",           make_ip_be(192,168,1,171),   55123, 0x5A5);
    round_trip("high-port",     make_ip_be(10,0,0,1),        65535, 0x0FF);
    round_trip("asymmetric",    make_ip_be(203,0,113,77),     3478, 0xABC);
    round_trip("nonce-zero",    make_ip_be(203,0,113,77),     3478, 0x000);

    /* Typo detection — ISO 7064 MOD 37,36 over version+payload+check
     * guarantees 100% detection of single-character substitutions in
     * the covered positions; the version position surfaces as
     * FUTURE_VERSION. */
    typo_detection("lan-typo",     make_ip_be(192,168,1,171), 55123, 0x5A5);
    typo_detection("zero-typo",    make_ip_be(0,0,0,0),           0, 0x000);
    typo_detection("ones-typo",    make_ip_be(255,255,255,255), 0xFFFF, ROOM_CODE_NONCE_MASK);

    /* S4b: version handling. */
    old_format_detection();
    future_version_detection();

    /* S4b: nonce range + CSPRNG variability. */
    nonce_behavior();

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
