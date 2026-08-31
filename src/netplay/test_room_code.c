/*
 * test_room_code.c — Step 2 test harness for docs/plan-stun-direct-p2p.md,
 * extended for the v3 format (docs/plan-netplay-connection.md §6.8).
 *
 * Covers: (ip, port, nonce) round-trips, display-format invariants,
 * an EXHAUSTIVE full-alphabet single-substitution sweep (the MEDIUM-1
 * regression net — see full_alphabet_sweep), OLD_FORMAT recognition of
 * legacy-checksum-valid v1 (11-char) and v2 (14-char) codes,
 * FUTURE_VERSION detection, 32-bit nonce fidelity, CSPRNG nonce
 * variability, and bogus-input rejection.
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

static void round_trip(const char* tag,
                       uint32_t ip_be, uint16_t pport, uint32_t nonce) {
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
    /* Exactly two dashes, at positions 6 and 13 (three 6-char groups of
     * the 18-char code), and the version prefix char in position 0. */
    if (code[6] != '-' || code[13] != '-') {
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
    uint32_t rn = 0;
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
                "code=\"%s\" ip=%08x/%08x pp=%u/%u nonce=%08x/%08x\n",
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
                           uint32_t ip_be, uint16_t pport, uint32_t nonce) {
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
        uint32_t rn = 0;
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
                    "ip=%08x pp=%u nonce=%08x\n",
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
        rng = rng * 1664525u + 1013904223u;
        const uint32_t nonce = rng;

        uint8_t oct[4] = { (uint8_t)(ip32 >> 24), (uint8_t)(ip32 >> 16),
                           (uint8_t)(ip32 >> 8),  (uint8_t)ip32 };
        uint32_t ip_be = 0;
        memcpy(&ip_be, oct, 4);

        char code[ROOM_CODE_BUF_LEN];
        if (!RoomCode_Encode(ip_be, pport, nonce, code)) {
            fail("sweep", "encode failed during sweep setup");
            return;
        }
        uint32_t gip = 0; uint16_t gpp = 0; uint32_t gn = 0;
        if (RoomCode_Decode(code, &gip, &gpp, &gn) != ROOM_CODE_OK) {
            fail("sweep", "self-decode of a freshly encoded code failed");
            return;
        }

        for (size_t pos = 0; pos < ROOM_CODE_DISPLAY_LEN; pos++) {
            if (code[pos] == '-') continue;   /* dashes are stripped anyway */
            const char orig = code[pos];
            for (int a = 0; a < 36; a++) {
                if (kIso[a] == orig) continue;
                code[pos] = kIso[a];
                uint32_t rip = 0; uint16_t rpp = 0; uint32_t rn = 0;
                const RoomCodeDecodeResult dr =
                    RoomCode_Decode(code, &rip, &rpp, &rn);
                total++;
                if (dr == ROOM_CODE_OK) {
                    if (rip == gip && rpp == gpp && rn == gn) {
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
                                    "\"%s\"  (%08x:%u/%08x  vs  %08x:%u/%08x)\n",
                                    pos, orig, kIso[a], good, code,
                                    gip, gpp, gn, rip, rpp, rn);
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
    uint32_t nn = 0;

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

    /* v3 addition: a legacy-checksum-valid 14-char v2 code must ALSO be
     * OLD_FORMAT — the whole alpha group is mid-upgrade, and "code is
     * from an older version" is the only honest thing to say about a
     * code whose 12-bit nonce cannot seed a v3 derivation. */
    char v2[15];
    make_v2_code(make_ip_be(198, 51, 100, 7), 6000, 0x0AB, v2);
    ip = 0; pp = 0; nn = 0;
    if (RoomCode_Decode(v2, &ip, &pp, &nn) != ROOM_CODE_OLD_FORMAT) {
        fprintf(stderr,
                "[test_room_code] FAIL: old-format: valid v2 code \"%s\" not "
                "reported as OLD_FORMAT\n", v2);
        fail_count++;
    }
    if (ip != 0 || pp != 0 || nn != 0) {
        fail("old-format", "outputs not zeroed on v2 OLD_FORMAT");
    }

    /* ...and 14 chars of checksum-INVALID garbage stays MALFORMED. */
    char v2bad[15];
    memcpy(v2bad, v2, sizeof(v2bad));
    v2bad[13] = (v2bad[13] == 'A') ? 'B' : 'A';
    if (RoomCode_Decode(v2bad, &ip, &pp, &nn) != ROOM_CODE_MALFORMED) {
        fail("old-format", "checksum-invalid 14-char string reported OLD_FORMAT");
    }

    /* The reviewer's MEDIUM-1 counterexample pair: under the v2 codec
     * these two ONE-CHARACTER-APART strings both decoded ROOM_CODE_OK,
     * to 34.23.190.173 and 114.23.190.173. Under v3 they are 14-char
     * strings that are NOT legacy-checksum-valid... they are, in fact,
     * exactly what a v2 host would have printed, so they land on
     * OLD_FORMAT. Either way the one thing that must never happen again
     * is BOTH of them decoding OK to different endpoints. */
    {
        const char* a = "248BVXBAA4DNM1";
        const char* b = "2E8BVXBAA4DNM1";
        uint32_t aip = 0, bip = 0, an = 0, bn = 0;
        uint16_t app = 0, bpp = 0;
        const RoomCodeDecodeResult ra = RoomCode_Decode(a, &aip, &app, &an);
        const RoomCodeDecodeResult rb = RoomCode_Decode(b, &bip, &bpp, &bn);
        if (ra == ROOM_CODE_OK && rb == ROOM_CODE_OK &&
            (aip != bip || app != bpp || an != bn)) {
            fprintf(stderr,
                    "[test_room_code] FAIL: MEDIUM-1 counterexample: \"%s\" and "
                    "\"%s\" both decoded OK to DIFFERENT endpoints\n", a, b);
            fail_count++;
        }
    }

    if (fail_count == 0) {
        fprintf(stderr,
                "[test_room_code] old-format detection OK (v1 \"%s\" and v2 "
                "\"%s\" -> OLD_FORMAT)\n", v1, v2);
    }
}

static void future_version_detection(void) {
    uint32_t ip = 0;
    uint16_t pp = 0;
    uint32_t nn = 0;

    char code[ROOM_CODE_BUF_LEN];
    if (!RoomCode_Encode(make_ip_be(198, 51, 100, 7), 6000, 0x0ABCDEF1u, code)) {
        fail("future-version", "encode failed during setup");
        return;
    }
    /* Flip the version char to an unknown value: must be reported as
     * FUTURE_VERSION (checked BEFORE the check digit — a future
     * format's checksum scheme is unknowable). '4' is chosen because
     * '2' would be a v2-length claim at v3 length, which is a different
     * (also-correct) rejection path. */
    code[0] = '4';
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
    /* v3: the nonce is a full 32 bits. Assert the WIDTH itself — this is
     * the HIGH-1(a) regression net. If someone narrows the nonce back to
     * 12/16 bits (or masks it anywhere in the encode/derive chain), the
     * high-bit round-trip below fails immediately. */
    if (ROOM_CODE_NONCE_BITS < 32) {
        fail("nonce", "ROOM_CODE_NONCE_BITS regressed below 32 — the punch "
                      "token's only unguessable input got narrower");
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

    /* Two nonces that differ ONLY above bit 11 must produce different
     * codes and must round-trip exactly. Under the v2 12-bit format
     * these were the same value. */
    const uint32_t hi_a = 0x00000222u;
    const uint32_t hi_b = 0xDEAD0222u;
    char ch1[ROOM_CODE_BUF_LEN];
    char ch2[ROOM_CODE_BUF_LEN];
    if (!RoomCode_Encode(make_ip_be(8, 8, 8, 8), 80, hi_a, ch1) ||
        !RoomCode_Encode(make_ip_be(8, 8, 8, 8), 80, hi_b, ch2)) {
        fail("nonce", "encode failed for high-bit nonces");
        return;
    }
    if (strcmp(ch1, ch2) == 0) {
        fail("nonce", "nonces differing only above bit 11 produced the SAME code");
        return;
    }
    uint32_t rip = 0; uint16_t rpp = 0; uint32_t rn = 0;
    if (RoomCode_Decode(ch2, &rip, &rpp, &rn) != ROOM_CODE_OK || rn != hi_b) {
        fprintf(stderr,
                "[test_room_code] FAIL: nonce: high-bit nonce round-trip "
                "%08x -> %08x\n", hi_b, rn);
        fail_count++;
        return;
    }
    /* Full-width extremes. */
    if (!RoomCode_Encode(make_ip_be(8, 8, 8, 8), 80, 0xFFFFFFFFu, ch1) ||
        RoomCode_Decode(ch1, &rip, &rpp, &rn) != ROOM_CODE_OK ||
        rn != 0xFFFFFFFFu) {
        fail("nonce", "all-ones 32-bit nonce did not round-trip");
        return;
    }

    /* CSPRNG nonce generation: actually varying, and actually using the
     * upper half of the range. 16 draws all landing on one value has
     * probability 2^-480 — a stuck/constant source is the only
     * realistic way this fires. Separately, 16 draws that all leave the
     * top 20 bits zero has probability 2^-320, so `any_high` catches a
     * source that was silently masked back down to 12 bits. */
    uint32_t first = 0;
    bool any_diff = false;
    bool any_high = false;
    for (int i = 0; i < 16; i++) {
        uint32_t nn = 0;
        if (!RoomCode_GenerateNonce(&nn)) {
            fail("nonce", "RoomCode_GenerateNonce failed (CSPRNG unavailable?)");
            return;
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
                      "nonce is still being masked to the v2 width");
        return;
    }
    fprintf(stderr, "[test_room_code] nonce behavior OK (32-bit width + variability)\n");
}

static void reject_bogus(void) {
    uint32_t ip = 0;
    uint16_t pp = 0;
    uint32_t nn = 0;

    /* Empty. */
    if (RoomCode_Decode("", &ip, &pp, &nn) != ROOM_CODE_MALFORMED)
        fail("bogus-empty", "empty string not MALFORMED");
    /* Too short. */
    if (RoomCode_Decode("ABCD", &ip, &pp, &nn) != ROOM_CODE_MALFORMED)
        fail("bogus-short", "4-char not MALFORMED");
    /* Wrong lengths bracketing the valid ones. 11 and 14 are the legacy
     * lengths (handled by old_format_detection); 12, 13, 15, 16, 17 and
     * 19 must all be MALFORMED. Built length-parametrically so they do
     * not silently rot the next time the format grows. */
    {
        static const size_t kBadLens[] = { 12, 13, 15, 16, 17, 19 };
        char buf[32];
        for (size_t bi = 0; bi < sizeof(kBadLens) / sizeof(kBadLens[0]); bi++) {
            const size_t L = kBadLens[bi];
            buf[0] = ROOM_CODE_VERSION_CHAR;
            for (size_t i = 1; i < L; i++) buf[i] = 'A';
            buf[L] = '\0';
            if (RoomCode_Decode(buf, &ip, &pp, &nn) != ROOM_CODE_MALFORMED) {
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
        if (RoomCode_Encode(make_ip_be(10, 0, 0, 1), 65535, 0x3FFu, code)) {
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

/* --- S4-review MEDIUM-4: log redaction --------------------------------
 *
 * The property that matters is not "some characters are stars" but
 * "ZERO nonce bits survive". So: encode the SAME (ip, port) under many
 * different nonces and assert every redaction is byte-identical. If any
 * nonce bit leaked into a surviving character the strings would differ.
 * Also assert the surviving prefix still identifies the version and is
 * genuinely shorter than the whole code (a redactor that blanked
 * everything would pass the first test trivially).
 */
static void redaction_behavior(void) {
    const uint32_t ip_be = make_ip_be(198, 51, 100, 7);
    const uint16_t pport = 51234;

    char first[ROOM_CODE_BUF_LEN] = { 0 };
    uint32_t rng = 0xC0FFEE11u;
    for (int i = 0; i < 512; i++) {
        rng = rng * 1664525u + 1013904223u;
        char code[ROOM_CODE_BUF_LEN];
        if (!RoomCode_Encode(ip_be, pport, rng, code)) {
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
        if (i == 0) {
            memcpy(first, red, sizeof(first));
        } else if (strcmp(first, red) != 0) {
            fprintf(stderr,
                    "[test_room_code] FAIL: redact: nonce bits SURVIVED "
                    "redaction — \"%s\" vs \"%s\" for the same ip:port\n",
                    first, red);
            fail_count++;
            return;
        }
    }

    /* Exactly ROOM_CODE_REDACT_CHARS masked characters, and they are the
     * TAIL (the nonce end), not a scatter. */
    {
        int stars = 0;
        for (size_t i = 0; first[i] != '\0'; i++) {
            if (first[i] == '*') stars++;
        }
        if (stars != ROOM_CODE_REDACT_CHARS) {
            fprintf(stderr,
                    "[test_room_code] FAIL: redact: %d masked chars, expected "
                    "%d (\"%s\")\n", stars, ROOM_CODE_REDACT_CHARS, first);
            fail_count++;
            return;
        }
    }

    /* A DIFFERENT (ip, port) must still be distinguishable — redaction
     * must not throw away the half the log is for. */
    {
        char other[ROOM_CODE_BUF_LEN];
        char other_red[ROOM_CODE_BUF_LEN];
        if (!RoomCode_Encode(make_ip_be(203, 0, 113, 77), 3478, 1u, other)) {
            fail("redact", "encode failed for the distinctness case");
            return;
        }
        RoomCode_Redact(other, other_red);
        if (strcmp(other_red, first) == 0) {
            fail("redact", "two different endpoints redact identically — the "
                           "redaction is destroying the ip:port half too");
            return;
        }
    }

    /* Degenerate inputs must not write past the buffer or leave it
     * unterminated. */
    {
        char red[ROOM_CODE_BUF_LEN];
        RoomCode_Redact(NULL, red);
        if (red[0] != '\0') fail("redact", "NULL input did not yield \"\"");
        RoomCode_Redact("", red);
        if (red[0] != '\0') fail("redact", "empty input did not yield \"\"");
        RoomCode_Redact("2AB", red);
        if (strlen(red) != 3) fail("redact", "short input changed length");
    }

    fprintf(stderr, "[test_room_code] redaction OK — \"%s\" (0 nonce bits "
                    "survive across 512 nonces)\n", first);
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

    uint32_t rip = 0; uint16_t rpp = 0; uint32_t rn = 0;
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
    round_trip("all-zeros",     make_ip_be(0,0,0,0),            0, 0x00000000u);
    round_trip("all-ones",      make_ip_be(255,255,255,255), 0xFFFF, ROOM_CODE_NONCE_MASK);
    round_trip("loopback",      make_ip_be(127,0,0,1),       54321, 0x00000001u);
    round_trip("dns-google",    make_ip_be(8,8,8,8),            80, 0x80000000u);
    round_trip("lan",           make_ip_be(192,168,1,171),   55123, 0x5A5A5A5Au);
    round_trip("high-port",     make_ip_be(10,0,0,1),        65535, 0x000000FFu);
    round_trip("asymmetric",    make_ip_be(203,0,113,77),     3478, 0xABCDEF01u);
    round_trip("nonce-zero",    make_ip_be(203,0,113,77),     3478, 0x00000000u);
    round_trip("nonce-msb",     make_ip_be(203,0,113,77),     3478, 0x80000000u);

    /* Typo detection — ISO 7064 MOD 37,36 over version+payload+check
     * guarantees 100% detection of single-character substitutions in
     * the covered positions; the version position surfaces as
     * FUTURE_VERSION. */
    typo_detection("lan-typo",     make_ip_be(192,168,1,171), 55123, 0x5A5A5A5Au);
    typo_detection("zero-typo",    make_ip_be(0,0,0,0),           0, 0x00000000u);
    typo_detection("ones-typo",    make_ip_be(255,255,255,255), 0xFFFF, ROOM_CODE_NONCE_MASK);

    /* MEDIUM-1: the exhaustive version of the above. 4000 codes x 18
     * positions x 35 alternatives = 2,520,000 substitutions, all of
     * which must be either rejected or decode to the IDENTICAL tuple.
     * Against the v1/v2 recurrence this reported ~2500 undetected. */
    full_alphabet_sweep(4000);

    /* S4b: version handling. */
    old_format_detection();
    future_version_detection();

    /* S4b: nonce range + CSPRNG variability. */
    nonce_behavior();

    /* Bogus input rejection. */
    reject_bogus();

    /* Normalization sanity (case fold, dash strip). */
    normalization_sanity();

    /* S4-review MEDIUM-4: the code is key material — logs must not
     * carry the nonce. */
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
