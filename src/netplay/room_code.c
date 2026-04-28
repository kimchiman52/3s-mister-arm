/*
 * room_code.c — Step 2 of docs/plan-stun-direct-p2p.md.
 *
 * Packs an IPv4 + public_port tuple into a short human-typeable room
 * code. See room_code.h for the public contract and the rationale
 * behind the alphabet choices.
 *
 * Payload bit layout (big-endian throughout — the same order bytes
 * appear on the wire for IPv4 + `htons(port)`):
 *
 *     byte 0..3 : ip_be (already in network byte order)
 *     byte 4..5 : public_port stored big-endian
 *
 * The 6 payload bytes are interpreted as a single 48-bit unsigned
 * integer (MSB = byte 0). We then emit 10 Crockford base-32 chars by
 * peeling off 5 bits at a time from the MSB end. The 1st (most
 * significant) char only has 3 valid bits; the top 2 bits are zero on
 * encode and must be zero on decode.
 *
 * Check digit: ISO 7064 MOD 37,36 over all 10 payload chars using the
 * 36-character 0-9A-Z alphabet. Crockford chars are a strict subset,
 * so each payload char has a well-defined 0..35 value. The emitted
 * check digit uses the same 0-9A-Z alphabet (may land on I/L/O, which
 * the decoder accepts either literally or after the loose-alias
 * normalization).
 */

#include "netplay/room_code.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Crockford base-32 alphabet, 32 chars (no I, L, O, U). Used for the
 * 10 payload chars.
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
 * Compute the ISO 7064 MOD 37,36 check character for `payload`
 * (expected length ROOM_CODE_PAYLOAD_CHARS = 10). Returns -1 if any
 * char fails to map into the 36-char alphabet (shouldn't happen on
 * freshly encoded payloads; used for defence on the decode path too).
 * On success returns the check char's value in [0, 35].
 */
static int iso7064_mod_37_36_compute(const char* payload, size_t len) {
    /* Per ISO/IEC 7064 MOD 37,36 (Wikipedia: "Check digit"):
     *   product = 36
     *   for each digit:
     *       sum     = (product + value(digit)) mod 37
     *       if sum == 0: sum = 37
     *       product = (sum * 2) mod 37
     *   check = (37 - product) mod 36
     */
    int product = 36;
    for (size_t i = 0; i < len; i++) {
        const int v = iso36_value(payload[i]);
        if (v < 0) return -1;
        int sum = (product + v) % 37;
        if (sum == 0) sum = 37;
        product = (sum * 2) % 37;
    }
    const int check = (37 - product) % 36;
    return check;
}

/*
 * Verify the full 11-char code (10 payload + 1 check) is
 * self-consistent. We recompute the expected check digit over the 10
 * payload chars and compare to the 11th character. This is
 * equivalent to the canonical "run recurrence over payload+check,
 * expect product == 1" formulation but avoids a subtle off-by-one
 * between ISO 7064's "hybrid" and "pure" variants by deriving the
 * expected check via the same code path the encoder uses.
 */
static bool iso7064_mod_37_36_verify(const char* code_with_check, size_t len) {
    if (len != ROOM_CODE_CHAR_LEN) return false;
    const int expected =
        iso7064_mod_37_36_compute(code_with_check, ROOM_CODE_PAYLOAD_CHARS);
    if (expected < 0) return false;
    const int actual =
        iso36_value(code_with_check[ROOM_CODE_PAYLOAD_CHARS]);
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

bool RoomCode_Encode(uint32_t ip_be, uint16_t public_port,
                     char out_code[ROOM_CODE_BUF_LEN]) {
    if (!out_code) return false;

    /* Pack 6 payload bytes. ip_be is treated as the 32-bit value
     * already in network byte order (as produced by inet_aton or
     * obtained from sockaddr_in.sin_addr.s_addr). On a little-endian
     * host, the in-memory bytes of ip_be are already the four IPv4
     * octets in order, so we copy them byte-for-byte. (memcpy is
     * trivially optimized by every compiler we target.) public_port
     * is passed in host byte order and converted to big-endian inline
     * so the payload is byte-order-stable regardless of host. */
    uint8_t raw[ROOM_CODE_RAW_LEN];
    memcpy(&raw[0], &ip_be, 4);
    raw[4] = (uint8_t)((public_port >> 8) & 0xFF);
    raw[5] = (uint8_t)((public_port >> 0) & 0xFF);

    /* Interpret as a 48-bit integer, big-endian (stored in a uint64_t,
     * bits [47..0] populated; bits [63..48] zero). */
    uint64_t x = 0;
    for (int i = 0; i < ROOM_CODE_RAW_LEN; i++) {
        x = (x << 8) | raw[i];
    }

    /* Emit 10 Crockford base-32 chars. 10 * 5 = 50 bits of address
     * space; our 48-bit payload occupies bits [47..0] with implicit
     * bits 48, 49 = 0. So char[0] covers bits [49..45] with the top
     * two bits clear (always Crockford value in [0, 7], i.e. '0'..'7'),
     * char[1] covers [44..40], ..., char[9] covers [4..0].
     *
     * Shift amounts: 45, 40, 35, 30, 25, 20, 15, 10, 5, 0. */
    char payload[ROOM_CODE_PAYLOAD_CHARS + 1];
    for (int i = 0; i < ROOM_CODE_PAYLOAD_CHARS; i++) {
        const unsigned shift = (unsigned)(45 - i * 5);
        const unsigned idx = (unsigned)((x >> shift) & 0x1F);
        payload[i] = kCrockfordAlphabet[idx];
    }
    payload[ROOM_CODE_PAYLOAD_CHARS] = '\0';

    /* Compute check digit over the 10 payload chars. */
    const int check = iso7064_mod_37_36_compute(payload, ROOM_CODE_PAYLOAD_CHARS);
    if (check < 0 || check >= 36) return false;

    /* Lay out the display form ABCDEFGH-IJK: dash after the 8th payload
     * char so the visible split is 8 + (2 payload + 1 check). This keeps
     * the total at 12 visible characters (11 alnum + 1 dash). */
    size_t w = 0;
    for (int i = 0; i < ROOM_CODE_PAYLOAD_CHARS; i++) {
        if (i == 8) {
            out_code[w++] = '-';
        }
        out_code[w++] = payload[i];
    }
    out_code[w++] = kIsoBase36Alphabet[check];
    out_code[w] = '\0';
    /* Sanity: w must equal ROOM_CODE_DISPLAY_LEN. */
    return w == ROOM_CODE_DISPLAY_LEN;
}

bool RoomCode_Decode(const char* code,
                     uint32_t* ip_be, uint16_t* public_port) {
    if (!code || !ip_be || !public_port) return false;

    /* Normalize: strip dashes/whitespace, upper-case, apply Crockford
     * loose aliases. RoomCode_NormalizeInput maps I/L→1 and O→0; this
     * is safe for the 10 payload chars (where the encoder never emits
     * I/L/O/U). For the check digit position, the ISO 7064 MOD 37,36
     * alphabet CAN emit any of 0-9A-Z, so I/L/O/U are legal check
     * values — we must not silently remap them. We therefore also
     * capture the ORIGINAL (pre-alias, but upper-cased + dash-stripped)
     * form so the check-digit verify can consult it. */
    char norm[ROOM_CODE_BUF_LEN];
    const size_t n = RoomCode_NormalizeInput(code, norm, sizeof(norm));
    if (n != ROOM_CODE_CHAR_LEN) return false;

    /* Also build a "literal" form: same strip + upper-case but WITHOUT
     * the I/L → 1 and O → 0 remapping. Used for the check-digit
     * position so a literal 'I'/'L'/'O' in the emitted check char
     * validates. Limit: 12 visible chars + NUL. */
    char literal[ROOM_CODE_BUF_LEN];
    size_t lw = 0;
    for (size_t i = 0; code[i] != '\0' && lw + 1 < sizeof(literal); i++) {
        char c = code[i];
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        literal[lw++] = c;
    }
    literal[lw] = '\0';
    if (lw != ROOM_CODE_CHAR_LEN) return false;

    /* The 10 payload chars are compared using the alias-normalized form
     * (so a user-typed 'I' in the payload works — encoder never emits
     * it there). The check char uses the literal form (so an emitted
     * 'I' validates against a literal 'I' from the user). We splice:
     * payload comes from norm[0..9], check char from literal[10].
     *
     * But the ISO recurrence over payload must use the values
     * corresponding to the CANONICAL emitted form. Since the encoder
     * never produces I/L/O in the payload, norm[0..9] — which has
     * I/L → 1 and O → 0 applied — exactly matches what the encoder
     * would have emitted even if the user typed 'I' or 'O'. Good.
     * The check position uses literal[10] as-is. */
    char verify_buf[ROOM_CODE_CHAR_LEN + 1];
    memcpy(verify_buf, norm, ROOM_CODE_PAYLOAD_CHARS);
    verify_buf[ROOM_CODE_PAYLOAD_CHARS] = literal[ROOM_CODE_PAYLOAD_CHARS];
    verify_buf[ROOM_CODE_CHAR_LEN] = '\0';

    /* Verify ISO 7064 MOD 37,36 check digit over all 11 chars. */
    if (!iso7064_mod_37_36_verify(verify_buf, ROOM_CODE_CHAR_LEN)) return false;

    /* The first char of the 10-char payload addresses bits [49..45] of
     * a 50-bit space; bits 48, 49 are implicitly zero (the full tuple
     * is only 48 bits). So the top Crockford value must be in [0, 7] —
     * reject otherwise. This keeps the round-trip bijective. */
    const int top_group = crockford_value(norm[0]);
    if (top_group < 0 || (top_group & 0x18) != 0) return false;

    /* Decode the 10 payload chars. x accumulates MSB-first, matching
     * the encode path (char[0] shifted to bits [49..45] of a 50-bit
     * logical value, which is bits [47..45] + implicit 0s at bits
     * 48, 49). */
    uint64_t x = 0;
    for (int i = 0; i < ROOM_CODE_PAYLOAD_CHARS; i++) {
        const int v = crockford_value(norm[i]);
        if (v < 0) return false;
        x = (x << 5) | (uint64_t)v;
    }

    const uint8_t raw[ROOM_CODE_RAW_LEN] = {
        (uint8_t)((x >> 40) & 0xFF),
        (uint8_t)((x >> 32) & 0xFF),
        (uint8_t)((x >> 24) & 0xFF),
        (uint8_t)((x >> 16) & 0xFF),
        (uint8_t)((x >>  8) & 0xFF),
        (uint8_t)((x >>  0) & 0xFF),
    };

    /* Mirror the encode path: ip_be is treated as 32 bits in network
     * byte order, so we copy the four in-order octets back. */
    memcpy(ip_be, &raw[0], 4);
    *public_port = (uint16_t)(((uint16_t)raw[4] << 8) | raw[5]);
    return true;
}
