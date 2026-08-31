/*
 * room_code.c — Step 2 of docs/plan-stun-direct-p2p.md; format v4 as of
 * task #155 (docs/queue.md #155).
 *
 * See room_code.h for the public contract, the v4 layout, and the
 * rationale behind dropping the nonce, the 2 spare payload bits, the
 * version prefix, and the check-digit recurrence.
 *
 * v4 payload bit layout (50 bits packed MSB-first into 10 Crockford
 * base-32 chars, 5 bits per char — the 48 fixed bits followed by 2
 * zero-filled pad bits):
 *
 *     bits [49..18] : IPv4 as a 32-bit big-endian-interpreted integer
 *                     (octet a is the most significant byte)
 *     bits [17..2]  : public_port
 *     bits [1..0]   : zero-filled pad (see room_code.h "2 spare bits")
 *
 * Check digit: ISO 7064 MOD 37,36 over the 11 chars preceding it
 * (version char + 10 payload chars) using the 36-character 0-9A-Z
 * alphabet. Crockford chars are a strict subset with the same ordinal
 * mapping, and the version char '4' is a digit, so every input char has
 * a well-defined 0..35 value. The emitted check digit uses the same
 * 0-9A-Z alphabet (may land on I/L/O/U, which the decoder accepts
 * literally at that position — no loose-alias remapping there).
 *
 * Old-format recognition: an 11-char input is checked against the v1
 * layout (10 payload chars over 48 bits + check digit over those 10,
 * LEGACY recurrence), a 14-char input against the v2 layout ('2' + 12
 * payload chars + check digit over the 13, LEGACY recurrence), and an
 * 18-char input against the v3 layout ('3' + 16 payload chars + check
 * digit over the 17, CORRECTED recurrence — v3 was already fixed, only
 * its length is legacy now). A legacy-checksum-valid old code returns
 * ROOM_CODE_OLD_FORMAT so the UI can say "code from an older version";
 * a checksum-INVALID 11/14/18-char string is plain MALFORMED (garbage
 * should not masquerade as a version mismatch).
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
 * (length-generic — v4 uses 11 chars = version + payload, v3
 * recognition uses 17, v1 recognition uses 10). Returns -1 if any char
 * fails to map into the 36-char alphabet. On success returns the check
 * char's value in [0, 35].
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
 * as generic garbage. Never used to emit or to validate a v3 or v4
 * code — both of those use iso7064_mod_37_36_compute/_verify below.
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

void RoomCode_Redact(const char* code, char out[ROOM_CODE_BUF_LEN]) {
    if (out == NULL) return;
    out[0] = '\0';
    if (code == NULL) return;

    size_t w = 0;
    for (size_t i = 0; code[i] != '\0' && w + 1 < (size_t)ROOM_CODE_BUF_LEN; i++) {
        out[w++] = code[i];
    }
    out[w] = '\0';

    /* Walk back over the printable (non-dash) tail, masking as we go.
     * Positional rather than index-arithmetic so a format change shows
     * up as an obviously-wrong log string, never a silent leak.
     *
     * The dash skip above only ever sees whatever the copy loop already
     * kept, and that loop is bounded to ROOM_CODE_BUF_LEN-1 = 12 raw
     * characters. An 18-char (v3) or 20-char-displayed legacy code
     * passed through unnormalized is silently TRUNCATED to its first 12
     * characters before this masking loop runs at all — the back half
     * of the legacy code, dashes included, never reaches here. This is
     * bounded and safe (no overflow) and RoomCode_Decode reports
     * OLD_FORMAT for the untruncated code regardless, so nothing
     * downstream trusts the truncated form as a real code — but the
     * dash-skip is only honoring whatever punctuation happens to land
     * inside that 12-char prefix, not preserving the legacy code's
     * structure. */
    int masked = 0;
    for (int i = (int)w - 1; i >= 0 && masked < ROOM_CODE_REDACT_CHARS; i--) {
        if (out[i] == '-') continue;
        out[i] = '*';
        masked++;
    }
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
    /* Every uint32_t is a legal nonce — the mask is the identity here
     * and is applied only to keep the width assumption explicit. */
    *out_nonce = raw & ROOM_CODE_NONCE_MASK;
    return true;
}

/*
 * The v4 payload is 50 bits (48 fixed + 2 zero-filled pad) — wider than
 * one byte-aligned run of the 6 bytes the fixed fields need, so it is
 * carried in a 7-byte (56-bit) buffer: bytes[0..3] = ip_be (network
 * byte order, so a plain memcpy is host-endian-independent), bytes[4..5]
 * = public_port big-endian, byte[6] = 0 (the 2 pad bits are its top 2
 * bits; the low 6 are never read — see room_code_get5's bitpos bound
 * below).
 */
#define ROOM_CODE_PAYLOAD_BYTES 7

static void room_code_pack_payload(uint32_t ip_be, uint16_t public_port,
                                   uint8_t out[ROOM_CODE_PAYLOAD_BYTES]) {
    memcpy(&out[0], &ip_be, 4);
    out[4] = (uint8_t)((public_port >> 8) & 0xFFu);
    out[5] = (uint8_t)(public_port & 0xFFu);
    /* Zero-fill the 2 spare bits deliberately (room_code.h). */
    out[6] = 0;
}

/* Read the 5-bit group starting at bit `bitpos` (MSB-first) out of the
 * payload buffer. bitpos is always a multiple of 5; for v4 (10 chars)
 * the largest value is 45, which reads p[5] (off=5, >3) combined with
 * p[6] — both in range for the 7-byte v4 buffer. Also reused, with a
 * differently sized buffer, nowhere else in this file — the function
 * itself is buffer-size-agnostic. */
static unsigned room_code_get5(const uint8_t* p, unsigned bitpos) {
    const unsigned byte = bitpos >> 3;
    const unsigned off  = bitpos & 7u;
    if (off <= 3u) {
        return (unsigned)(p[byte] >> (3u - off)) & 0x1Fu;
    }
    return (unsigned)(((unsigned)p[byte] << (off - 3u)) |
                      ((unsigned)p[byte + 1] >> (11u - off))) & 0x1Fu;
}

/* Inverse of room_code_get5 — OR a 5-bit group into the payload. */
static void room_code_put5(uint8_t* p, unsigned bitpos, unsigned v) {
    const unsigned byte = bitpos >> 3;
    const unsigned off  = bitpos & 7u;
    if (off <= 3u) {
        p[byte] |= (uint8_t)((v & 0x1Fu) << (3u - off));
        return;
    }
    p[byte]     |= (uint8_t)((v & 0x1Fu) >> (off - 3u));
    p[byte + 1] |= (uint8_t)(((v & 0x1Fu) << (11u - off)) & 0xFFu);
}

bool RoomCode_Encode(uint32_t ip_be, uint16_t public_port,
                     char out_code[ROOM_CODE_BUF_LEN]) {
    if (!out_code) return false;

    uint8_t payload[ROOM_CODE_PAYLOAD_BYTES];
    room_code_pack_payload(ip_be, public_port, payload);

    /* Version char + 10 Crockford chars, MSB-first over the 50-bit
     * payload bitstream (48 fixed bits + 2 zero pad bits). */
    char body[ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS + 1];
    body[0] = ROOM_CODE_VERSION_CHAR;
    for (int i = 0; i < ROOM_CODE_PAYLOAD_CHARS; i++) {
        const unsigned idx = room_code_get5(payload, (unsigned)(i * 5));
        body[ROOM_CODE_VERSION_CHARS + i] = kCrockfordAlphabet[idx];
    }
    body[ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS] = '\0';

    /* Check digit over version char + payload chars (11 chars). */
    const int check = iso7064_mod_37_36_compute(
        body, ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS);
    if (check < 0 || check >= 36) return false;

    /* Display form: no hyphen groups (room_code.h) — the 11 body chars
     * followed by the check digit. */
    size_t w = 0;
    for (int i = 0; i < ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS; i++) {
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
 * itself is deliberately not decoded. */
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
 * decode (a v2 code's 12-bit nonce cannot seed a v4 derivation). */
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

/* v3 recognition: version char '3' + 16 Crockford payload chars + ISO
 * 7064 check digit over the 17, under the CORRECTED (sum-mod-36)
 * recurrence — v3 already used the fixed arithmetic (room_code.h), so
 * this is NOT the legacy-recurrence sibling room_code_is_valid_v1/v2
 * are. Recognition only, no tuple decode (a v3 code's 32-bit nonce
 * cannot seed a v4, nonce-less derivation). */
static bool room_code_is_valid_v3(const char* norm, const char* literal) {
    if (norm[0] != ROOM_CODE_V3_VERSION_CHAR) return false;
    char verify_buf[ROOM_CODE_V3_CHAR_LEN + 1];
    memcpy(verify_buf, norm, ROOM_CODE_V3_CHAR_LEN - 1);
    verify_buf[ROOM_CODE_V3_CHAR_LEN - 1] = literal[ROOM_CODE_V3_CHAR_LEN - 1];
    verify_buf[ROOM_CODE_V3_CHAR_LEN] = '\0';
    if (!iso7064_mod_37_36_verify(verify_buf, ROOM_CODE_V3_CHAR_LEN)) {
        return false;
    }
    /* All 16 payload chars must be canonical Crockford. */
    for (int i = 1; i < ROOM_CODE_V3_CHAR_LEN - 1; i++) {
        if (crockford_value(norm[i]) < 0) return false;
    }
    return true;
}

RoomCodeDecodeResult RoomCode_Decode(const char* code,
                                     uint32_t* ip_be,
                                     uint16_t* public_port) {
    if (!ip_be || !public_port) return ROOM_CODE_MALFORMED;
    *ip_be = 0;
    *public_port = 0;
    if (!code) return ROOM_CODE_MALFORMED;

    /* Normalize: strip dashes/whitespace, upper-case, apply Crockford
     * loose aliases (I/L → 1, O → 0). Safe for the payload positions
     * (the encoder never emits I/L/O/U there) but NOT for the check
     * digit, which may legally be any of 0-9A-Z — so also build a
     * "literal" form (strip + upper-case only) for that position. */
    /* Sized 32, NOT ROOM_CODE_BUF_LEN: ROOM_CODE_BUF_LEN is now the
     * CURRENT (v4, 12-char) size, and the longest input this function
     * must still recognize is the legacy 18-char v3 code (plus dash/
     * whitespace slack). A too-long garbage input still truncates
     * safely here, same as before. */
    char norm[32];
    const size_t n = RoomCode_NormalizeInput(code, norm, sizeof(norm));

    char literal[32];
    size_t lw = 0;
    for (size_t i = 0; code[i] != '\0' && lw + 1 < sizeof(literal); i++) {
        char c = code[i];
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        literal[lw++] = c;
    }
    literal[lw] = '\0';
    if (lw != n) return ROOM_CODE_MALFORMED; /* both strip identically */

    /* Length dispatch: 11/14/18 chars = a possible legacy v1/v2/v3 code
     * (OLD_FORMAT detection); 12 chars = the current v4 format; anything
     * else is malformed. */
    if (n == ROOM_CODE_V1_CHAR_LEN) {
        return room_code_is_valid_v1(norm, literal) ? ROOM_CODE_OLD_FORMAT
                                                    : ROOM_CODE_MALFORMED;
    }
    if (n == ROOM_CODE_V2_CHAR_LEN) {
        return room_code_is_valid_v2(norm, literal) ? ROOM_CODE_OLD_FORMAT
                                                    : ROOM_CODE_MALFORMED;
    }
    if (n == ROOM_CODE_V3_CHAR_LEN) {
        return room_code_is_valid_v3(norm, literal) ? ROOM_CODE_OLD_FORMAT
                                                    : ROOM_CODE_MALFORMED;
    }
    if (n != ROOM_CODE_CHAR_LEN) {
        return ROOM_CODE_MALFORMED;
    }

    /* Version gate BEFORE the check digit: a future format's checksum
     * scheme is unknowable, so an unrecognized version char must
     * surface as a version mismatch, not "invalid code". ('4' is a
     * digit — unaffected by the alias normalization above.) */
    if (norm[0] != ROOM_CODE_VERSION_CHAR) {
        return ROOM_CODE_FUTURE_VERSION;
    }

    /* Verify ISO 7064 MOD 37,36 over version + payload + check. The
     * 11 covered chars come from the alias-normalized form (encoder
     * never emits I/L/O there); the check position uses the literal
     * form so an emitted 'I'/'L'/'O'/'U' check digit validates. */
    char verify_buf[ROOM_CODE_CHAR_LEN + 1];
    memcpy(verify_buf, norm, ROOM_CODE_CHAR_LEN - 1);
    verify_buf[ROOM_CODE_CHAR_LEN - 1] = literal[ROOM_CODE_CHAR_LEN - 1];
    verify_buf[ROOM_CODE_CHAR_LEN] = '\0';
    if (!iso7064_mod_37_36_verify(verify_buf, ROOM_CODE_CHAR_LEN)) {
        return ROOM_CODE_MALFORMED;
    }

    /* Decode the 10 payload chars back into the 50-bit payload
     * bitstream, MSB-first (mirror of the encode path). The 2 low pad
     * bits are intentionally NOT validated — see room_code_pack_payload
     * and room_code.h's "2 spare payload bits": a nonzero pad is
     * tolerated rather than rejected. This does NOT cover a version
     * bump — the version-char check above (ROOM_CODE_FUTURE_VERSION)
     * already rejects an unrecognized version before any pad bit is
     * read, so a real v5 needs this decoder to change regardless of pad
     * tolerance. What tolerating a nonzero pad actually buys is a
     * same-version-char ('4') refinement finding a use for these bits
     * without forcing a version bump. */
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
    return ROOM_CODE_OK;
}
