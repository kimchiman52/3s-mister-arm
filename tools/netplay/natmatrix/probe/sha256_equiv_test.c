/* Proves the probe's PSA shim computes the same SHA-256 as the real
 * tf-psa-crypto backend.
 *
 * Built TWICE from identical source -- once against third_party/tf-psa-crypto
 * (the shipped backend) and once against tools/netplay/natmatrix/probe/psa_shim.c
 * -- and the two outputs are diffed. Identical output proves the substitution is
 * byte-transparent to src/netplay/rendezvous.c's key derivation.
 *
 * Uses ONLY the src/utils/sha256.h API, so both builds exercise the unmodified
 * src/utils/sha256.c.
 *
 * exit 0 = all published vectors matched; exit 1 = a vector mismatched.
 */
#include "utils/sha256.h"

#include <stdio.h>
#include <string.h>

static int check(const char* label, const void* data, size_t len, const char* expect) {
    sha256 s;
    char hex[SHA256_HEX_SIZE];
    if (!sha256_init(&s)) { printf("%-28s INIT_FAILED\n", label); return 1; }
    if (!sha256_append(&s, data, len)) { printf("%-28s APPEND_FAILED\n", label); return 1; }
    if (!sha256_finalize_hex(&s, hex)) { printf("%-28s FINAL_FAILED\n", label); return 1; }
    int bad = expect && strcmp(hex, expect) != 0;
    printf("%-28s %s%s\n", label, hex, bad ? "  MISMATCH" : "");
    return bad ? 1 : 0;
}

int main(void) {
    int rc = 0;

    /* FIPS 180-4 / NIST published SHA-256 vectors. */
    rc |= check("empty",
                "", 0,
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    rc |= check("abc",
                "abc", 3,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    rc |= check("448-bit",
                "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    rc |= check("896-bit",
                "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112,
                "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    /* Multi-chunk streaming must equal the single-shot digest -- this is how
     * rendezvous.c actually calls it (prefix, then the 10-byte payload). */
    {
        sha256 s; char hex[SHA256_HEX_SIZE];
        sha256_init(&s);
        sha256_append(&s, "abcdbcdecdefdefgefghfghighijhijk", 32);
        sha256_append(&s, "ijkljklmklmnlmnomnopnopq", 24);
        sha256_finalize_hex(&s, hex);
        int bad = strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") != 0;
        printf("%-28s %s%s\n", "448-bit-split", hex, bad ? "  MISMATCH" : "");
        rc |= bad;
    }

    /* Block-boundary cases: exactly 55, 56, 64 and 65 bytes exercise every
     * padding branch. No published vector -- the two builds are diffed instead,
     * which is the actual equivalence claim. */
    {
        static unsigned char buf[200];
        for (int i = 0; i < 200; i++) buf[i] = (unsigned char)i;
        const size_t lens[] = { 55, 56, 57, 63, 64, 65, 119, 128, 200 };
        for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
            char label[40];
            snprintf(label, sizeof(label), "boundary-%zu", lens[i]);
            rc |= check(label, buf, lens[i], NULL);
        }
    }

    /* The exact shape rendezvous.c hashes: an 8-byte domain-separation prefix
     * followed by the 10-byte packed payload (ip[4] || port_be[2] || nonce_be[4]).
     * That layout is stated verbatim at src/netplay/rendezvous.c:81-87
     * (rend_derive's contract, "ip[4] || port_be[2] || nonce_be[4]"), packed
     * into `payload` (`REND_KEY_PAYLOAD_LEN` bytes) at
     * src/netplay/rendezvous.c:104-111, and hashed domain-first at
     * rendezvous.c:118-122. The previous citation, rendezvous.c:130-149,
     * landed on `memcpy(out, digest, out_len);` and the two public wrappers --
     * past the packing it claimed to point at. */
    {
        unsigned char payload[10] = { 203, 0, 113, 10, 0x1b, 0x58, 0xde, 0xad, 0xbe, 0xef };
        sha256 s; char hex[SHA256_HEX_SIZE];
        sha256_init(&s);
        sha256_append(&s, "3SXR-SK3", 8);
        sha256_append(&s, payload, sizeof(payload));
        sha256_finalize_hex(&s, hex);
        printf("%-28s %s\n", "rendezvous-sk3", hex);
        sha256_init(&s);
        sha256_append(&s, "3SXR-PT3", 8);
        sha256_append(&s, payload, sizeof(payload));
        sha256_finalize_hex(&s, hex);
        printf("%-28s %s\n", "rendezvous-pt3", hex);
    }

    printf(rc ? "SHA256_EQUIV_TEST: FAIL\n" : "SHA256_EQUIV_TEST: PASS\n");
    return rc;
}
