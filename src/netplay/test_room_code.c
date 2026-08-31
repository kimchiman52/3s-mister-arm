/*
 * test_room_code.c — Step 2 test harness for docs/plan-stun-direct-p2p.md,
 * extended for the v4 format (docs/queue.md #155).
 *
 * Covers: (ip, port) round-trips, display-format invariants, an
 * EXHAUSTIVE full-alphabet single-substitution sweep (the MEDIUM-1
 * regression net — see full_alphabet_sweep), OLD_FORMAT recognition of
 * legacy-checksum-valid v1 (11-char), v2 (14-char) and v3 (18-char)
 * codes, FUTURE_VERSION detection, the nonce MACHINERY (kept compiled
 * and exercised here even though the v4 code string no longer carries
 * one — see room_code.h), and bogus-input rejection.
 * Gated behind ENABLE_NETPLAY_TESTS. Enable with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS='-DENABLE_NETPLAY_TESTS -DNETPLAY_TEST_HOOKS'"
 * (NETPLAY_TEST_HOOKS is required because this build links every
 * src/netplay/test_*.c TU, including test_bilateral_punch.c, which needs
 * that macro to declare DirectP2P_TestHook_IsLanPeer.)
 * Mirrors the other Phase 6 test harnesses' pattern: gated behind
 * ENABLE_NETPLAY_TESTS, with an #else stub that logs "not compiled in"
 * and returns 2.
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

static void round_trip(const char* tag, uint32_t ip_be, uint16_t pport) {
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
    /* v4: no dash groups — the display form IS the bare 12-char code. */
    if (code[0] != ROOM_CODE_VERSION_CHAR) {
        fprintf(stderr, "[test_room_code] FAIL: %s: version char missing in \"%s\"\n", tag, code);
        fail_count++;
        return;
    }

    uint32_t rip = 0;
    uint16_t rpp = 0;
    const RoomCodeDecodeResult dr = RoomCode_Decode(code, &rip, &rpp);
    if (dr != ROOM_CODE_OK) {
        fprintf(stderr, "[test_room_code] FAIL: %s: decode rejected \"%s\" (result=%d)\n",
                tag, code, (int)dr);
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

static void typo_detection(const char* tag, uint32_t ip_be, uint16_t pport) {
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
        const RoomCodeDecodeResult dr = RoomCode_Decode(code, &rip, &rpp);
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
                    "ip=%08x pp=%u\n",
                    tag, i, backup[i], code[i], rip, rpp);
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
    /* ISO 7064 MOD 37,36 over the 10 chars, LEGACY (defective, sum mod
     * 37) recurrence — what v1 actually emitted. */
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

/* Minimal LOCAL v2 (S4b) encoder — same reason as make_v1_code above:
 * the production v2 encoder is gone, but the decoder must still map a
 * legacy-checksum-valid 14-char code to OLD_FORMAT. '2' + 12 Crockford
 * chars over the 60-bit ip|port|nonce12 packing + the LEGACY (sum mod
 * 37) check digit — byte-for-byte what pre-v3 RoomCode_Encode emitted,
 * minus the display dash. */
static bool make_v2_code(uint32_t ip_be, uint16_t pport, uint16_t nonce12,
                         char out[15]) {
    static const char kCrock[32] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    static const char kIso[36] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint8_t oct[4];
    memcpy(oct, &ip_be, 4);
    const uint32_t ip32 = ((uint32_t)oct[0] << 24) | ((uint32_t)oct[1] << 16) |
                          ((uint32_t)oct[2] << 8) | (uint32_t)oct[3];
    const uint64_t x = ((uint64_t)ip32 << 28) | ((uint64_t)pport << 12) |
                       (uint64_t)(nonce12 & 0x0FFFu);
    out[0] = '2';
    for (int i = 0; i < 12; i++) {
        out[1 + i] = kCrock[(x >> (55 - i * 5)) & 0x1F];
    }
    /* LEGACY ISO 7064 recurrence (sum mod 37) over the 13 chars. */
    int product = 36;
    for (int i = 0; i < 13; i++) {
        const char c = out[i];
        const int v = (c >= '0' && c <= '9') ? c - '0' : 10 + (c - 'A');
        int sum = (product + v) % 37;
        if (sum == 0) sum = 37;
        product = (sum * 2) % 37;
    }
    out[13] = kIso[(37 - product) % 36];
    out[14] = '\0';
    return true;
}

/* Minimal LOCAL v3 (task #142-era) encoder — the production v3 encoder
 * is gone as of task #155, but the decoder must still map a legacy-
 * checksum-valid 18-char code to OLD_FORMAT. '3' + 16 Crockford chars
 * over the 80-bit ip|port|nonce32 packing + the CORRECTED (sum mod 36)
 * check digit over the 17 — v3 was ALREADY fixed (room_code.h), so this
 * is NOT the legacy-recurrence sibling make_v1_code/make_v2_code are. */
static bool make_v3_code(uint32_t ip_be, uint16_t pport, uint32_t nonce32,
                         char out[19]) {
    static const char kCrock[32] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    static const char kIso[36] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint8_t payload[10];
    memcpy(&payload[0], &ip_be, 4);
    payload[4] = (uint8_t)((pport >> 8) & 0xFFu);
    payload[5] = (uint8_t)(pport & 0xFFu);
    payload[6] = (uint8_t)((nonce32 >> 24) & 0xFFu);
    payload[7] = (uint8_t)((nonce32 >> 16) & 0xFFu);
    payload[8] = (uint8_t)((nonce32 >> 8) & 0xFFu);
    payload[9] = (uint8_t)(nonce32 & 0xFFu);

    out[0] = '3';
    for (int i = 0; i < 16; i++) {
        const unsigned bitpos = (unsigned)(i * 5);
        const unsigned byte = bitpos >> 3;
        const unsigned off = bitpos & 7u;
        unsigned v;
        if (off <= 3u) {
            v = (unsigned)(payload[byte] >> (3u - off)) & 0x1Fu;
        } else {
            v = (unsigned)(((unsigned)payload[byte] << (off - 3u)) |
                           ((unsigned)payload[byte + 1] >> (11u - off))) & 0x1Fu;
        }
        out[1 + i] = kCrock[v];
    }
    /* CORRECTED ISO 7064 recurrence (sum mod 36) over the 17 chars. */
    int product = 36;
    for (int i = 0; i < 17; i++) {
        const char c = out[i];
        const int v = (c >= '0' && c <= '9') ? c - '0' : 10 + (c - 'A');
        int sum = (product + v) % 36;
        if (sum == 0) sum = 36;
        product = (sum * 2) % 37;
    }
    out[17] = kIso[(37 - product) % 36];
    out[18] = '\0';
    return true;
}

/* ---------------------------------------------------------------------
 * MEDIUM-1 regression net: EXHAUSTIVE single-substitution sweep.
 *
 * The previous typo_detection() applied exactly ONE hardcoded
 * substitution per position (+1 in the ISO-36 alphabet), so it could
 * not see that the v1/v2 check-digit recurrence left ~1 in 781
 * substitutions undetected: two distinct running-product states (0 and
 * 36) collapsed onto check char '1'. Worked counterexample against the
 * v2 codec — "248BVXBAA4DNM1" and "2E8BVXBAA4DNM1" BOTH decoded OK, to
 * 34.23.190.173 and 114.23.190.173.
 *
 * This sweep replaces EVERY position with EVERY other character of the
 * full 0-9A-Z alphabet and asserts that not one of them decodes to a
 * DIFFERENT endpoint. Anything that still decodes OK must decode to the
 * IDENTICAL tuple (only possible via the decoder's Crockford loose
 * aliases at payload positions: I/L -> 1, O -> 0).
 *
 * v4 geometry: 12 positions (no dash positions to skip — v4 has none),
 * 35 alternatives each = 420 substitutions per code.
 * ------------------------------------------------------------------ */
static void full_alphabet_sweep(int code_count) {
    static const char kIso[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    /* Deterministic LCG — the sweep must be reproducible when it fires. */
    uint32_t rng = 0x5A17C0DEu;
    long total = 0, undetected = 0, alias_ok = 0;

    for (int k = 0; k < code_count; k++) {
        rng = rng * 1664525u + 1013904223u;
        const uint32_t ip32 = rng;
        rng = rng * 1664525u + 1013904223u;
        const uint16_t pport = (uint16_t)(rng >> 16);

        uint8_t oct[4] = { (uint8_t)(ip32 >> 24), (uint8_t)(ip32 >> 16),
                           (uint8_t)(ip32 >> 8),  (uint8_t)ip32 };
        uint32_t ip_be = 0;
        memcpy(&ip_be, oct, 4);

        char code[ROOM_CODE_BUF_LEN];
        if (!RoomCode_Encode(ip_be, pport, code)) {
            fail("sweep", "encode failed during sweep setup");
            return;
        }
        uint32_t gip = 0; uint16_t gpp = 0;
        if (RoomCode_Decode(code, &gip, &gpp) != ROOM_CODE_OK) {
            fail("sweep", "self-decode of a freshly encoded code failed");
            return;
        }

        for (size_t pos = 0; pos < ROOM_CODE_DISPLAY_LEN; pos++) {
            const char orig = code[pos];
            for (int a = 0; a < 36; a++) {
                if (kIso[a] == orig) continue;
                code[pos] = kIso[a];
                uint32_t rip = 0; uint16_t rpp = 0;
                const RoomCodeDecodeResult dr = RoomCode_Decode(code, &rip, &rpp);
                total++;
                if (dr == ROOM_CODE_OK) {
                    if (rip == gip && rpp == gpp) {
                        /* Crockford loose alias — same tuple, harmless. */
                        alias_ok++;
                    } else {
                        if (undetected < 5) {
                            char good[ROOM_CODE_BUF_LEN];
                            memcpy(good, code, sizeof(good));
                            good[pos] = orig;
                            fprintf(stderr,
                                    "[test_room_code] FAIL: sweep: UNDETECTED "
                                    "substitution pos=%zu '%c'->'%c'  \"%s\" -> "
                                    "\"%s\"  (%08x:%u  vs  %08x:%u)\n",
                                    pos, orig, kIso[a], good, code,
                                    gip, gpp, rip, rpp);
                        }
                        undetected++;
                    }
                }
                code[pos] = orig;
            }
        }
    }

    if (undetected != 0) {
        fprintf(stderr,
                "[test_room_code] FAIL: sweep: %ld of %ld single-char "
                "substitutions decoded to a DIFFERENT endpoint (%.4f%%)\n",
                undetected, total, 100.0 * (double)undetected / (double)total);
        fail_count++;
        return;
    }
    fprintf(stderr,
            "[test_room_code] full-alphabet sweep OK — 0 undetected of %ld "
            "substitutions across %d codes (%ld benign Crockford aliases)\n",
            total, code_count, alias_ok);
}

static void old_format_detection(void) {
    uint32_t ip = 0;
    uint16_t pp = 0;

    /* A checksum-valid v1 code must be recognized as OLD_FORMAT — the
     * "this code is from an older version" UI path. */
    char v1[12];
    make_v1_code(make_ip_be(203, 0, 113, 77), 3478, v1);
    RoomCodeDecodeResult dr = RoomCode_Decode(v1, &ip, &pp);
    if (dr != ROOM_CODE_OLD_FORMAT) {
        fprintf(stderr,
                "[test_room_code] FAIL: old-format: valid v1 code \"%s\" reported %d, "
                "expected OLD_FORMAT\n", v1, (int)dr);
        fail_count++;
    }
    if (ip != 0 || pp != 0) {
        fail("old-format", "outputs not zeroed on v1 OLD_FORMAT");
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
    if (RoomCode_Decode(dashed, &ip, &pp) != ROOM_CODE_OLD_FORMAT) {
        fail("old-format", "dashed v1 code not recognized as OLD_FORMAT");
    }

    /* 11 chars of checksum-INVALID garbage must be MALFORMED, not
     * OLD_FORMAT — garbage should not masquerade as a version issue. */
    char bad[12];
    memcpy(bad, v1, sizeof(bad));
    bad[10] = (bad[10] == 'A') ? 'B' : 'A'; /* break the v1 check digit */
    if (RoomCode_Decode(bad, &ip, &pp) != ROOM_CODE_MALFORMED) {
        fail("old-format", "checksum-invalid 11-char string reported OLD_FORMAT");
    }

    /* A legacy-checksum-valid 14-char v2 code must ALSO be OLD_FORMAT —
     * "code is from an older version" is the only honest thing to say
     * about a code whose 12-bit nonce cannot seed a v4 derivation. */
    char v2[15];
    make_v2_code(make_ip_be(198, 51, 100, 7), 6000, 0x0AB, v2);
    ip = 0; pp = 0;
    if (RoomCode_Decode(v2, &ip, &pp) != ROOM_CODE_OLD_FORMAT) {
        fprintf(stderr,
                "[test_room_code] FAIL: old-format: valid v2 code \"%s\" not "
                "reported as OLD_FORMAT\n", v2);
        fail_count++;
    }
    if (ip != 0 || pp != 0) {
        fail("old-format", "outputs not zeroed on v2 OLD_FORMAT");
    }

    /* ...and 14 chars of checksum-INVALID garbage stays MALFORMED. */
    char v2bad[15];
    memcpy(v2bad, v2, sizeof(v2bad));
    v2bad[13] = (v2bad[13] == 'A') ? 'B' : 'A';
    if (RoomCode_Decode(v2bad, &ip, &pp) != ROOM_CODE_MALFORMED) {
        fail("old-format", "checksum-invalid 14-char string reported OLD_FORMAT");
    }

    /* Task #155 addition: a legacy-checksum-valid 18-char v3 code must
     * ALSO be OLD_FORMAT — v3 is now a legacy LENGTH even though its
     * arithmetic was already correct (room_code.h). */
    char v3[19];
    make_v3_code(make_ip_be(192, 0, 2, 55), 7000, 0xABCDEF01u, v3);
    ip = 0; pp = 0;
    if (RoomCode_Decode(v3, &ip, &pp) != ROOM_CODE_OLD_FORMAT) {
        fprintf(stderr,
                "[test_room_code] FAIL: old-format: valid v3 code \"%s\" not "
                "reported as OLD_FORMAT\n", v3);
        fail_count++;
    }
    if (ip != 0 || pp != 0) {
        fail("old-format", "outputs not zeroed on v3 OLD_FORMAT");
    }

    /* ...and 18 chars of checksum-INVALID garbage stays MALFORMED. */
    char v3bad[19];
    memcpy(v3bad, v3, sizeof(v3bad));
    v3bad[17] = (v3bad[17] == 'A') ? 'B' : 'A';
    if (RoomCode_Decode(v3bad, &ip, &pp) != ROOM_CODE_MALFORMED) {
        fail("old-format", "checksum-invalid 18-char string reported OLD_FORMAT");
    }

    /* The reviewer's MEDIUM-1 counterexample pair: under the v2 codec
     * these two ONE-CHARACTER-APART strings both decoded ROOM_CODE_OK,
     * to 34.23.190.173 and 114.23.190.173. Under v4 they are 14-char
     * strings that are NOT recognized as the current format (14 != 12);
     * they land on the v2 legacy-length path, which is exactly what a
     * v2 host would have printed, so they land on OLD_FORMAT. Either
     * way the one thing that must never happen again is BOTH of them
     * decoding OK to different endpoints. */
    {
        const char* a = "248BVXBAA4DNM1";
        const char* b = "2E8BVXBAA4DNM1";
        uint32_t aip = 0, bip = 0;
        uint16_t app = 0, bpp = 0;
        const RoomCodeDecodeResult ra = RoomCode_Decode(a, &aip, &app);
        const RoomCodeDecodeResult rb = RoomCode_Decode(b, &bip, &bpp);
        if (ra == ROOM_CODE_OK && rb == ROOM_CODE_OK &&
            (aip != bip || app != bpp)) {
            fprintf(stderr,
                    "[test_room_code] FAIL: MEDIUM-1 counterexample: \"%s\" and "
                    "\"%s\" both decoded OK to DIFFERENT endpoints\n", a, b);
            fail_count++;
        }
    }

    if (fail_count == 0) {
        fprintf(stderr,
                "[test_room_code] old-format detection OK (v1 \"%s\", v2 \"%s\", "
                "v3 \"%s\" -> OLD_FORMAT)\n", v1, v2, v3);
    }
}

static void future_version_detection(void) {
    uint32_t ip = 0;
    uint16_t pp = 0;

    char code[ROOM_CODE_BUF_LEN];
    if (!RoomCode_Encode(make_ip_be(198, 51, 100, 7), 6000, code)) {
        fail("future-version", "encode failed during setup");
        return;
    }
    /* Flip the version char to an unknown value: must be reported as
     * FUTURE_VERSION (checked BEFORE the check digit — a future
     * format's checksum scheme is unknowable). '5' is chosen because it
     * is not any recognized version char at any recognized length and
     * is unaffected by the Crockford loose-alias normalization. */
    code[0] = '5';
    if (RoomCode_Decode(code, &ip, &pp) != ROOM_CODE_FUTURE_VERSION) {
        fail("future-version", "unknown version char not reported as FUTURE_VERSION");
        return;
    }
    if (ip != 0 || pp != 0) {
        fail("future-version", "outputs not zeroed on FUTURE_VERSION");
        return;
    }
    fprintf(stderr, "[test_room_code] future-version detection OK\n");
}

/*
 * v4 (task #155): the room code no longer carries a nonce, so this no
 * longer round-trips one through RoomCode_Encode/Decode. What stays —
 * per room_code.h's "keep the nonce machinery alive" requirement — is
 * direct exercise of RoomCode_GenerateNonce itself: the width macro and
 * the CSPRNG's actual variability. A future room-list feature is the
 * intended consumer; this is the regression net that keeps
 * RoomCode_GenerateNonce from silently rotting the way LOSSY_ADAPTER did
 * (4ca4c0de).
 */
static void nonce_machinery_behavior(void) {
    if (ROOM_CODE_NONCE_BITS < 32) {
        fail("nonce", "ROOM_CODE_NONCE_BITS regressed below 32");
    }

    /* CSPRNG nonce generation: actually varying, and actually using the
     * upper half of the range. 16 draws all landing on one value has
     * probability 2^-480 — a stuck/constant source is the only
     * realistic way this fires. Separately, 16 draws that all leave the
     * top 20 bits zero has probability 2^-320, so `any_high` catches a
     * source that was silently narrowed to a 12-bit width. */
    uint32_t first = 0;
    bool any_diff = false;
    bool any_high = false;
    for (int i = 0; i < 16; i++) {
        uint32_t nn = 0;
        if (!RoomCode_GenerateNonce(&nn)) {
            fail("nonce", "RoomCode_GenerateNonce failed (CSPRNG unavailable?)");
            return;
        }
        if (nn > ROOM_CODE_NONCE_MASK) {
            fail("nonce", "RoomCode_GenerateNonce produced a value outside the mask");
        }
        if ((nn & 0xFFFFF000u) != 0) any_high = true;
        if (i == 0) first = nn;
        else if (nn != first) any_diff = true;
    }
    if (!any_diff) {
        fail("nonce", "16 generated nonces were all identical — CSPRNG not varying");
        return;
    }
    if (!any_high) {
        fail("nonce", "16 generated nonces all fit in 12 bits — the CSPRNG "
                      "nonce is still being masked to a narrow width");
        return;
    }
    fprintf(stderr, "[test_room_code] nonce machinery OK (32-bit width + "
                    "CSPRNG variability; not used by the v4 code string — "
                    "kept for the future room-list feature, room_code.h)\n");
}

static void reject_bogus(void) {
    uint32_t ip = 0;
    uint16_t pp = 0;

    /* Empty. */
    if (RoomCode_Decode("", &ip, &pp) != ROOM_CODE_MALFORMED)
        fail("bogus-empty", "empty string not MALFORMED");
    /* Too short. */
    if (RoomCode_Decode("ABCD", &ip, &pp) != ROOM_CODE_MALFORMED)
        fail("bogus-short", "4-char not MALFORMED");
    /* Wrong lengths bracketing the valid ones. 11, 14 and 18 are the
     * legacy lengths (handled by old_format_detection); 12 is current.
     * 10, 13, 15, 16, 17 and 19 must all be MALFORMED. Built
     * length-parametrically so they do not silently rot the next time
     * the format grows. */
    {
        static const size_t kBadLens[] = { 10, 13, 15, 16, 17, 19 };
        char buf[32];
        for (size_t bi = 0; bi < sizeof(kBadLens) / sizeof(kBadLens[0]); bi++) {
            const size_t L = kBadLens[bi];
            buf[0] = ROOM_CODE_VERSION_CHAR;
            for (size_t i = 1; i < L; i++) buf[i] = 'A';
            buf[L] = '\0';
            if (RoomCode_Decode(buf, &ip, &pp) != ROOM_CODE_MALFORMED) {
                fprintf(stderr,
                        "[test_room_code] FAIL: bogus-len: %zu-char \"%s\" not "
                        "MALFORMED\n", L, buf);
                fail_count++;
            }
        }
    }
    /* Contains U in a payload position (not in the Crockford alphabet;
     * not remapped by RoomCode_NormalizeInput either). Build a valid
     * code and inject the U so length/version/check-order is right. */
    {
        char code[ROOM_CODE_BUF_LEN];
        if (RoomCode_Encode(make_ip_be(10, 0, 0, 1), 65535, code)) {
            code[2] = 'U'; /* a payload position */
            if (RoomCode_Decode(code, &ip, &pp) == ROOM_CODE_OK)
                fail("bogus-U", "U in payload accepted");
        } else {
            fail("bogus-U", "encode failed during setup");
        }
    }
    /* NULL code pointer. */
    if (RoomCode_Decode(NULL, &ip, &pp) != ROOM_CODE_MALFORMED)
        fail("bogus-null", "NULL not MALFORMED");

    fprintf(stderr, "[test_room_code] bogus-input rejection OK\n");
}

/* --- S4-review MEDIUM-4: log redaction, re-scoped for v4 --------------
 *
 * v4 has no independent secret to protect in the redacted tail (the
 * payload IS the public (ip, port) tuple — see room_code.h's "redaction
 * no longer hides a secret"). What's asserted here instead: exactly
 * ROOM_CODE_REDACT_CHARS trailing characters are masked, the version
 * char and length survive, two different codes still redact
 * differently (the redaction is not blanking everything), and
 * degenerate inputs are handled safely.
 */
static void redaction_behavior(void) {
    char code[ROOM_CODE_BUF_LEN];
    if (!RoomCode_Encode(make_ip_be(198, 51, 100, 7), 51234, code)) {
        fail("redact", "encode failed during setup");
        return;
    }
    char red[ROOM_CODE_BUF_LEN];
    RoomCode_Redact(code, red);

    if (strlen(red) != strlen(code)) {
        fail("redact", "redaction changed the code's length");
        return;
    }
    if (strcmp(red, code) == 0) {
        fail("redact", "redaction returned the code unchanged");
        return;
    }
    if (red[0] != ROOM_CODE_VERSION_CHAR) {
        fail("redact", "redaction destroyed the version char");
        return;
    }

    /* Exactly ROOM_CODE_REDACT_CHARS masked characters, and they are the
     * TAIL. */
    {
        int stars = 0;
        for (size_t i = 0; red[i] != '\0'; i++) {
            if (red[i] == '*') stars++;
            else if (i + (size_t)ROOM_CODE_REDACT_CHARS < strlen(red) && red[i] != code[i]) {
                fail("redact", "a non-tail character was altered");
            }
        }
        if (stars != ROOM_CODE_REDACT_CHARS) {
            fprintf(stderr,
                    "[test_room_code] FAIL: redact: %d masked chars, expected "
                    "%d (\"%s\")\n", stars, ROOM_CODE_REDACT_CHARS, red);
            fail_count++;
            return;
        }
    }

    /* A DIFFERENT (ip, port) must still be distinguishable — redaction
     * must not throw away the half the log is for. */
    {
        char other[ROOM_CODE_BUF_LEN];
        char other_red[ROOM_CODE_BUF_LEN];
        if (!RoomCode_Encode(make_ip_be(203, 0, 113, 77), 3478, other)) {
            fail("redact", "encode failed for the distinctness case");
            return;
        }
        RoomCode_Redact(other, other_red);
        if (strcmp(other_red, red) == 0) {
            fail("redact", "two different endpoints redact identically — the "
                           "redaction is destroying the ip:port half too");
            return;
        }
    }

    /* Degenerate inputs must not write past the buffer or leave it
     * unterminated. */
    {
        char red2[ROOM_CODE_BUF_LEN];
        RoomCode_Redact(NULL, red2);
        if (red2[0] != '\0') fail("redact", "NULL input did not yield \"\"");
        RoomCode_Redact("", red2);
        if (red2[0] != '\0') fail("redact", "empty input did not yield \"\"");
        RoomCode_Redact("2AB", red2);
        if (strlen(red2) != 3) fail("redact", "short input changed length");
    }

    fprintf(stderr, "[test_room_code] redaction OK — \"%s\" -> \"%s\" "
                    "(%d chars masked; hygiene only as of v4, not "
                    "confidentiality — see room_code.h)\n",
                    code, red, ROOM_CODE_REDACT_CHARS);
}

static void normalization_sanity(void) {
    /* Encode, convert display form to lower-case, decode. */
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
        /* Lower-case ASCII letters — a pure case-fold round-trip. */
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        aliased[w++] = c;
    }
    aliased[w] = '\0';

    uint32_t rip = 0; uint16_t rpp = 0;
    if (RoomCode_Decode(aliased, &rip, &rpp) != ROOM_CODE_OK) {
        fail("norm-lower", "decode rejected lower-case round-trip");
        return;
    }
    if (!ip_eq(rip, ip_be) || rpp != 80) {
        fail("norm-lower", "decode returned wrong tuple");
        return;
    }
    fprintf(stderr, "[test_room_code] norm lower-case OK — \"%s\"\n", aliased);

    /* A stray dash/whitespace in the input is still tolerated even
     * though v4 never DISPLAYS one (legacy codes did, and a user could
     * still paste one in by habit). */
    char dashed[ROOM_CODE_BUF_LEN + 4];
    size_t dw = 0;
    for (size_t i = 0; code[i]; i++) {
        if (i == 6) dashed[dw++] = '-';
        dashed[dw++] = code[i];
    }
    dashed[dw] = '\0';
    rip = 0; rpp = 0;
    if (RoomCode_Decode(dashed, &rip, &rpp) != ROOM_CODE_OK) {
        fail("norm-dash", "decode rejected a stray-dash input");
        return;
    }
    if (!ip_eq(rip, ip_be) || rpp != 80) {
        fail("norm-dash", "decode returned wrong tuple");
        return;
    }
    fprintf(stderr, "[test_room_code] norm stray-dash OK — \"%s\"\n", dashed);
}

int Netplay_Test_RoomCode(void) {
    fail_count = 0;

    /* Edge cases across both fields. */
    round_trip("all-zeros",     make_ip_be(0,0,0,0),            0);
    round_trip("all-ones",      make_ip_be(255,255,255,255), 0xFFFF);
    round_trip("loopback",      make_ip_be(127,0,0,1),       54321);
    round_trip("dns-google",    make_ip_be(8,8,8,8),            80);
    round_trip("lan",           make_ip_be(192,168,1,171),   55123);
    round_trip("high-port",     make_ip_be(10,0,0,1),        65535);
    round_trip("asymmetric",    make_ip_be(203,0,113,77),     3478);

    /* Typo detection — ISO 7064 MOD 37,36 over version+payload+check
     * guarantees 100% detection of single-character substitutions in
     * the covered positions; the version position surfaces as
     * FUTURE_VERSION. */
    typo_detection("lan-typo",     make_ip_be(192,168,1,171), 55123);
    typo_detection("zero-typo",    make_ip_be(0,0,0,0),           0);
    typo_detection("ones-typo",    make_ip_be(255,255,255,255), 0xFFFF);

    /* MEDIUM-1: the exhaustive version of the above. 4000 codes x 12
     * positions x 35 alternatives = 1,680,000 substitutions, all of
     * which must be either rejected or decode to the IDENTICAL tuple. */
    full_alphabet_sweep(4000);

    /* Version handling. */
    old_format_detection();
    future_version_detection();

    /* Nonce machinery — kept alive, exercised directly (see room_code.h). */
    nonce_machinery_behavior();

    /* Bogus input rejection. */
    reject_bogus();

    /* Normalization sanity (case fold, stray dash). */
    normalization_sanity();

    /* S4-review MEDIUM-4, re-scoped for v4: redaction hygiene. */
    redaction_behavior();

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
