/*
 * room_code.c — Step 2 of docs/plan-stun-direct-p2p.md; format v3 as of
 * the S4 adversarial-review fixes (docs/plan-netplay-connection.md §6.8).
 *
 * See room_code.h for the public contract, the v3 layout, and the
 * rationale behind the 32-bit nonce, the version prefix, and the
 * corrected check-digit recurrence.
 *
 * v3 payload bit layout (80 bits packed MSB-first into 16 Crockford
 * base-32 chars, 5 bits per char):
 *
 *     bits [79..48] : IPv4 as a 32-bit big-endian-interpreted integer
 *                     (octet a is the most significant byte)
 *     bits [47..32] : public_port
 *     bits [31..0]  : nonce
 *
 * Check digit: ISO 7064 MOD 37,36 over the 17 chars preceding it
 * (version char + 16 payload chars) using the 36-character 0-9A-Z
 * alphabet. Crockford chars are a strict subset with the same ordinal
 * mapping, and the version char '3' is a digit, so every input char
 * has a well-defined 0..35 value. The emitted check digit uses the
 * same 0-9A-Z alphabet (may land on I/L/O/U, which the decoder accepts
 * literally at that position — no loose-alias remapping there).
 *
 * Old-format recognition: an 11-char input is checked against the v1
 * layout (10 payload chars over 48 bits + check digit over those 10),
 * and a 14-char input against the v2 layout ('2' + 12 payload chars +
 * check digit over the 13). BOTH use the LEGACY recurrence
 * (iso7064_legacy_compute) because that is what those builds emitted.
 * A legacy-checksum-valid old code returns ROOM_CODE_OLD_FORMAT so the
 * UI can say "code from an older version"; a checksum-INVALID 11- or
 * 14-char string is plain MALFORMED (garbage should not masquerade as
 * a version mismatch).
 */

#include "netplay/room_code.h"

#include "utils/csprng.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Crockford base-32 alphabet, 32 chars (no I, L, O, U). Used for the
 * payload chars.
 */
static const char kCrockfordAlphabet[32] =
    "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/*
 * ISO 7064 MOD 37,36 alphabet: 0-9A-Z. Used for the check digit and
 * for the check-digit computation (payload chars are mapped into this
 * alphabet by their char value — the Crockford alphabet is a subset
 * that happens to have the same ordinal mapping for the 32 chars it
 * actually uses).
 */
static const char kIsoBase36Alphabet[36] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

/*
 * Value of a Crockford base-32 character in [0, 31], or -1 on invalid
 * input. Input must already be upper-case and loose-alias-normalized
 * (I/L → 1, O → 0); this table accepts exactly the 32 canonical
 * Crockford chars.
 */
static int crockford_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'H') return 10 + (c - 'A');     /* A..H → 10..17 */
    if (c == 'J') return 18;
    if (c == 'K') return 19;
    if (c == 'M') return 20;
    if (c == 'N') return 21;
    if (c >= 'P' && c <= 'T') return 22 + (c - 'P');     /* P..T → 22..26 */
    if (c == 'V') return 27;
    if (c == 'W') return 28;
    if (c == 'X') return 29;
    if (c == 'Y') return 30;
    if (c == 'Z') return 31;
    return -1;
}

/*
 * Value in [0, 35] for a char in the ISO 0-9A-Z alphabet. Caller must
 * pre-normalize (upper-case, strip dashes, I/L → 1, O → 0). Returns -1
 * on invalid input. This is the function used by the check-digit math.
 */
static int iso36_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    return -1;
}

/*
 * Compute the ISO 7064 MOD 37,36 check character over `payload`
 * (length-generic — v2 uses 13 chars = version + payload, v1
 * recognition uses 10). Returns -1 if any char fails to map into the
 * 36-char alphabet. On success returns the check char's value in
 * [0, 35].
 */
static int iso7064_mod_37_36_compute(const char* payload, size_t len) {
    /* ISO/IEC 7064 hybrid system, MOD 37,36 (M = 36):
     *   product = M
     *   for each digit:
     *       sum     = (product + value(digit)) mod M      <-- mod 36
     *       if sum == 0: sum = M
     *       product = (sum * 2) mod (M + 1)               <-- mod 37
     *   check = (M + 1 - product) mod M
     *
     * The intermediate sum is reduced mod M (36), NOT mod M+1 (37).
     * That is the whole point of the "hybrid" construction: it pins
     * sum to [1, 36], hence product = 2*sum mod 37 to [1, 36], hence
     * check = (37 - product) mod 36 to a BIJECTION onto [0, 35].
     *
     * v1/v2 of this file reduced the sum mod 37, which let product also
     * reach 0. Since (37 - 0) % 36 == (37 - 36) % 36 == 1, two distinct
     * product states collapsed onto check char '1' and the "detects
     * every single-character substitution" guarantee was false in
     * practice: measured 2508 undetected out of 1,960,000 substitutions
     * (0.128%). See room_code.h for the worked counterexample and
     * test_room_code.c's full_alphabet_sweep for the regression net. */
    int product = 36;
    for (size_t i = 0; i < len; i++) {
        const int v = iso36_value(payload[i]);
        if (v < 0) return -1;
        int sum = (product + v) % 36;
        if (sum == 0) sum = 36;
        product = (sum * 2) % 37;
    }
    const int check = (37 - product) % 36;
    return check;
}

/*
 * The DEFECTIVE pre-v3 recurrence (sum reduced mod 37). Retained for
 * ONE purpose: recognizing v1 (11-char) and v2 (14-char) codes emitted
 * by older builds so they surface as ROOM_CODE_OLD_FORMAT rather than
 * as generic garbage. Never used to emit or to validate a v3 code.
 */
static int iso7064_legacy_compute(const char* payload, size_t len) {
    int product = 36;
    for (size_t i = 0; i < len; i++) {
        const int v = iso36_value(payload[i]);
        if (v < 0) return -1;
        int sum = (product + v) % 37;
        if (sum == 0) sum = 37;
        product = (sum * 2) % 37;
    }
    return (37 - product) % 36;
}

/* Legacy-recurrence sibling of iso7064_mod_37_36_verify below. */
static bool iso7064_legacy_verify(const char* code_with_check, size_t len) {
    if (len < 2) return false;
    const int expected = iso7064_legacy_compute(code_with_check, len - 1);
    if (expected < 0) return false;
    const int actual = iso36_value(code_with_check[len - 1]);
    if (actual < 0) return false;
    return expected == actual;
}

/*
 * Verify a code of `len` chars whose final char is the check digit for
 * the preceding len-1 chars. We recompute the expected check digit and
 * compare — equivalent to the canonical "run recurrence over
 * payload+check, expect product == 1" formulation but avoids a subtle
 * off-by-one between ISO 7064's "hybrid" and "pure" variants by
 * deriving the expected check via the same code path the encoder uses.
 */
static bool iso7064_mod_37_36_verify(const char* code_with_check, size_t len) {
    if (len < 2) return false;
    const int expected = iso7064_mod_37_36_compute(code_with_check, len - 1);
    if (expected < 0) return false;
    const int actual = iso36_value(code_with_check[len - 1]);
    if (actual < 0) return false;
    return expected == actual;
}

size_t RoomCode_NormalizeInput(const char* in, char* out, size_t out_cap) {
    if (out_cap == 0) return 0;
    if (!in) { out[0] = '\0'; return 0; }

    size_t w = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        char c = in[i];
        /* Skip dashes and whitespace for readability tolerance. */
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;

        /* Upper-case. */
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');

        /* Crockford loose aliases: I/L → 1, O → 0. */
        if (c == 'I' || c == 'L') c = '1';
        else if (c == 'O') c = '0';

        if (w + 1 >= out_cap) break; /* leave room for NUL */
        out[w++] = c;
    }
    out[w] = '\0';
    return w;
}

bool RoomCode_GenerateNonce(uint32_t* out_nonce) {
    if (!out_nonce) return false;
    uint32_t raw = 0;
    if (!Csprng_Bytes(&raw, sizeof(raw))) {
        /* No weak fallback (see room_code.h): a predictable nonce would
         * silently void the guessing protection the nonce exists for. */
        return false;
    }
    /* Every uint32_t is a legal v3 nonce — the mask is the identity
     * here and is applied only to keep the width assumption explicit. */
    *out_nonce = raw & ROOM_CODE_NONCE_MASK;
    return true;
}

/*
 * The v3 payload is 80 bits — wider than a uint64_t — so it is carried
 * as the same canonical 10-byte big-endian serialization the key
 * derivations hash (rendezvous.c: ip[4] || port_be[2] || nonce_be[4]),
 * and the 16 base-32 chars are cut straight out of that bitstream,
 * MSB-first. Keeping ONE canonical byte order for the code payload and
 * the SHA-256 input removes a whole class of "which packing did this
 * side use" bugs.
 */
#define ROOM_CODE_PAYLOAD_BYTES (ROOM_CODE_PAYLOAD_BITS / 8)   /* 10 */

static void room_code_pack_payload(uint32_t ip_be, uint16_t public_port,
                                   uint32_t nonce,
                                   uint8_t out[ROOM_CODE_PAYLOAD_BYTES]) {
    /* ip_be is the 32-bit value already in network byte order (as
     * produced by inet_pton into `struct in_addr.s_addr`): its
     * in-memory bytes are the four IPv4 octets in order, so a plain
     * memcpy is host-endian-independent. */
    memcpy(&out[0], &ip_be, 4);
    out[4] = (uint8_t)((public_port >> 8) & 0xFFu);
    out[5] = (uint8_t)(public_port & 0xFFu);
    out[6] = (uint8_t)((nonce >> 24) & 0xFFu);
    out[7] = (uint8_t)((nonce >> 16) & 0xFFu);
    out[8] = (uint8_t)((nonce >> 8) & 0xFFu);
    out[9] = (uint8_t)(nonce & 0xFFu);
}

/* Read the 5-bit group starting at bit `bitpos` (MSB-first) out of the
 * 10-byte payload. bitpos is always a multiple of 5 in [0, 75]; the
 * only groups that straddle a byte boundary have (bitpos % 8) > 3, and
 * the largest such bitpos is 70 (byte 8 + byte 9), so the byte+1 read
 * is always in range. */
static unsigned room_code_get5(const uint8_t p[ROOM_CODE_PAYLOAD_BYTES],
                               unsigned bitpos) {
    const unsigned byte = bitpos >> 3;
    const unsigned off  = bitpos & 7u;
    if (off <= 3u) {
        return (unsigned)(p[byte] >> (3u - off)) & 0x1Fu;
    }
    return (unsigned)(((unsigned)p[byte] << (off - 3u)) |
                      ((unsigned)p[byte + 1] >> (11u - off))) & 0x1Fu;
}

/* Inverse of room_code_get5 — OR a 5-bit group into the payload. */
static void room_code_put5(uint8_t p[ROOM_CODE_PAYLOAD_BYTES],
                           unsigned bitpos, unsigned v) {
    const unsigned byte = bitpos >> 3;
    const unsigned off  = bitpos & 7u;
    if (off <= 3u) {
        p[byte] |= (uint8_t)((v & 0x1Fu) << (3u - off));
        return;
    }
    p[byte]     |= (uint8_t)((v & 0x1Fu) >> (off - 3u));
    p[byte + 1] |= (uint8_t)(((v & 0x1Fu) << (11u - off)) & 0xFFu);
}

bool RoomCode_Encode(uint32_t ip_be, uint16_t public_port, uint32_t nonce,
                     char out_code[ROOM_CODE_BUF_LEN]) {
    if (!out_code) return false;

    uint8_t payload[ROOM_CODE_PAYLOAD_BYTES];
    room_code_pack_payload(ip_be, public_port, nonce, payload);

    /* Version char + 16 Crockford chars, MSB-first over the 80-bit
     * payload bitstream. */
    char body[ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS + 1];
    body[0] = ROOM_CODE_VERSION_CHAR;
    for (int i = 0; i < ROOM_CODE_PAYLOAD_CHARS; i++) {
        const unsigned idx = room_code_get5(payload, (unsigned)(i * 5));
        body[ROOM_CODE_VERSION_CHARS + i] = kCrockfordAlphabet[idx];
    }
    body[ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS] = '\0';

    /* Check digit over version char + payload chars (17 chars). */
    const int check = iso7064_mod_37_36_compute(
        body, ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS);
    if (check < 0 || check >= 36) return false;

    /* Display form XXXXXX-XXXXXX-XXXXXX: dashes after the 6th and 12th
     * chars (three 6-char groups of the 18-char code). */
    size_t w = 0;
    for (int i = 0; i < ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS; i++) {
        if (i == 6 || i == 12) {
            out_code[w++] = '-';
        }
        out_code[w++] = body[i];
    }
    out_code[w++] = kIsoBase36Alphabet[check];
    out_code[w] = '\0';
    /* Sanity: w must equal ROOM_CODE_DISPLAY_LEN. */
    return w == ROOM_CODE_DISPLAY_LEN;
}

/* v1 (pre-S4b) recognition: 10 Crockford payload chars over 48 bits
 * (top 2 bits of the first char zero) + ISO 7064 check digit over
 * those 10, under the LEGACY recurrence. Returns true when
 * `norm`/`literal` (11 chars each) form a legacy-checksum-valid v1
 * code — used only to distinguish OLD_FORMAT from MALFORMED; the tuple
 * itself is deliberately not decoded (a v1 code has no nonce, so it
 * cannot pair with a v3 build anyway). */
static bool room_code_is_valid_v1(const char* norm, const char* literal) {
    /* Payload chars from the alias-normalized form; check char from the
     * literal form (v1 emitted I/L/O/U-capable check digits). */
    char verify_buf[ROOM_CODE_V1_CHAR_LEN + 1];
    memcpy(verify_buf, norm, ROOM_CODE_V1_PAYLOAD_CHARS);
    verify_buf[ROOM_CODE_V1_PAYLOAD_CHARS] = literal[ROOM_CODE_V1_PAYLOAD_CHARS];
    verify_buf[ROOM_CODE_V1_CHAR_LEN] = '\0';
    if (!iso7064_legacy_verify(verify_buf, ROOM_CODE_V1_CHAR_LEN)) {
        return false;
    }
    /* v1 constraint: the first char covered bits [49..45] of a 50-bit
     * space whose top 2 bits were zero — Crockford value in [0, 7]. */
    const int top_group = crockford_value(norm[0]);
    if (top_group < 0 || (top_group & 0x18) != 0) return false;
    /* All payload chars must be canonical Crockford. */
    for (int i = 0; i < ROOM_CODE_V1_PAYLOAD_CHARS; i++) {
        if (crockford_value(norm[i]) < 0) return false;
    }
    return true;
}

/* v2 (S4b) recognition: version char '2' + 12 Crockford payload chars +
 * ISO 7064 check digit over the 13, under the LEGACY recurrence. Same
 * contract as room_code_is_valid_v1 — recognition only, no tuple
 * decode (a v2 code's 12-bit nonce cannot seed a v3 derivation). */
static bool room_code_is_valid_v2(const char* norm, const char* literal) {
    if (norm[0] != ROOM_CODE_V2_VERSION_CHAR) return false;
    char verify_buf[ROOM_CODE_V2_CHAR_LEN + 1];
    memcpy(verify_buf, norm, ROOM_CODE_V2_CHAR_LEN - 1);
    verify_buf[ROOM_CODE_V2_CHAR_LEN - 1] = literal[ROOM_CODE_V2_CHAR_LEN - 1];
    verify_buf[ROOM_CODE_V2_CHAR_LEN] = '\0';
    if (!iso7064_legacy_verify(verify_buf, ROOM_CODE_V2_CHAR_LEN)) {
        return false;
    }
    /* All 12 payload chars must be canonical Crockford. */
    for (int i = 1; i < ROOM_CODE_V2_CHAR_LEN - 1; i++) {
        if (crockford_value(norm[i]) < 0) return false;
    }
    return true;
}

RoomCodeDecodeResult RoomCode_Decode(const char* code,
                                     uint32_t* ip_be,
                                     uint16_t* public_port,
                                     uint32_t* nonce) {
    if (!ip_be || !public_port || !nonce) return ROOM_CODE_MALFORMED;
    *ip_be = 0;
    *public_port = 0;
    *nonce = 0;
    if (!code) return ROOM_CODE_MALFORMED;

    /* Normalize: strip dashes/whitespace, upper-case, apply Crockford
     * loose aliases (I/L → 1, O → 0). Safe for the payload positions
     * (the encoder never emits I/L/O/U there) but NOT for the check
     * digit, which may legally be any of 0-9A-Z — so also build a
     * "literal" form (strip + upper-case only) for that position. */
    char norm[ROOM_CODE_BUF_LEN];
    const size_t n = RoomCode_NormalizeInput(code, norm, sizeof(norm));

    char literal[ROOM_CODE_BUF_LEN];
    size_t lw = 0;
    for (size_t i = 0; code[i] != '\0' && lw + 1 < sizeof(literal); i++) {
        char c = code[i];
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        literal[lw++] = c;
    }
    literal[lw] = '\0';
    if (lw != n) return ROOM_CODE_MALFORMED; /* both strip identically */

    /* Length dispatch: 11 chars = possibly a v1 code, 14 chars =
     * possibly a v2 code (both old-format detection); 18 chars = the
     * current format; anything else is malformed. */
    if (n == ROOM_CODE_V1_CHAR_LEN) {
        return room_code_is_valid_v1(norm, literal) ? ROOM_CODE_OLD_FORMAT
                                                    : ROOM_CODE_MALFORMED;
    }
    if (n == ROOM_CODE_V2_CHAR_LEN) {
        return room_code_is_valid_v2(norm, literal) ? ROOM_CODE_OLD_FORMAT
                                                    : ROOM_CODE_MALFORMED;
    }
    if (n != ROOM_CODE_CHAR_LEN) {
        return ROOM_CODE_MALFORMED;
    }

    /* Version gate BEFORE the check digit: a future format's checksum
     * scheme is unknowable, so an unrecognized version char must
     * surface as a version mismatch, not "invalid code". ('3' is a
     * digit — unaffected by the alias normalization above.) */
    if (norm[0] != ROOM_CODE_VERSION_CHAR) {
        return ROOM_CODE_FUTURE_VERSION;
    }

    /* Verify ISO 7064 MOD 37,36 over version + payload + check. The
     * 17 covered chars come from the alias-normalized form (encoder
     * never emits I/L/O there); the check position uses the literal
     * form so an emitted 'I'/'L'/'O'/'U' check digit validates. */
    char verify_buf[ROOM_CODE_CHAR_LEN + 1];
    memcpy(verify_buf, norm, ROOM_CODE_CHAR_LEN - 1);
    verify_buf[ROOM_CODE_CHAR_LEN - 1] = literal[ROOM_CODE_CHAR_LEN - 1];
    verify_buf[ROOM_CODE_CHAR_LEN] = '\0';
    if (!iso7064_mod_37_36_verify(verify_buf, ROOM_CODE_CHAR_LEN)) {
        return ROOM_CODE_MALFORMED;
    }

    /* Decode the 16 payload chars back into the 80-bit payload
     * bitstream, MSB-first (mirror of the encode path). */
    uint8_t payload[ROOM_CODE_PAYLOAD_BYTES];
    memset(payload, 0, sizeof(payload));
    for (int i = 0; i < ROOM_CODE_PAYLOAD_CHARS; i++) {
        const int v = crockford_value(norm[ROOM_CODE_VERSION_CHARS + i]);
        if (v < 0) return ROOM_CODE_MALFORMED;
        room_code_put5(payload, (unsigned)(i * 5), (unsigned)v);
    }

    /* Mirror the encode path: ip_be is 32 bits in network byte order,
     * so copy the four in-order octets back. */
    memcpy(ip_be, &payload[0], 4);
    *public_port = (uint16_t)(((uint16_t)payload[4] << 8) | payload[5]);
    *nonce = ((uint32_t)payload[6] << 24) | ((uint32_t)payload[7] << 16) |
             ((uint32_t)payload[8] << 8)  |  (uint32_t)payload[9];
    return ROOM_CODE_OK;
}
