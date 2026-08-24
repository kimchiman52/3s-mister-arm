/*
 * room_code.h — Step 2 of docs/plan-stun-direct-p2p.md; format v2 as of
 * S4b (docs/plan-netplay-connection.md §6).
 *
 * Packs the (ip_be, public_port, nonce) tuple into a short
 * human-typeable identifier:
 *
 *   version char '2'                                      (1 char)
 *   60-bit payload: ip(32) << 28 | port(16) << 12 | nonce(12)
 *     -> 12 Crockford base-32 chars, MSB first              (12 chars)
 *   ISO 7064 MOD 37,36 check digit over the preceding 13    (1 char)
 *
 * = 14 alphanumeric chars, display-formatted with a single dash after
 * the 7th char: `XXXXXXX-XXXXXXX` (15 visible characters).
 *
 * S4b rationale — why the nonce (BREAKING format change, deliberate):
 * the v1 code was a bare reversible (ip, port) encoding. Anyone who
 * saw the code learned the host's home IP, and — because the
 * rendezvous session key and the S4a punch token are DERIVED from the
 * code payload — anyone who could GUESS the payload (ip:port scans of
 * plausible ranges) could derive the session key and squat/race the
 * pairing. The 12-bit CSPRNG nonce mixes 4096 unguessable variants
 * into every derivation: the payload can no longer be reconstructed
 * from the observable (ip, port) alone. The code still necessarily
 * CONTAINS the ip (the joiner must reach the host) — the nonce
 * protects the derived key material, not ip privacy for someone
 * holding the full code.
 *
 * Version prefix: the leading '2' makes format changes detectable. An
 * 11-char v1 code (checksum-valid) decodes to ROOM_CODE_OLD_FORMAT so
 * the UI can say "this code is from an older version" instead of a
 * mysterious failure; a 14-char code with an unknown version char
 * reports ROOM_CODE_FUTURE_VERSION. The version char is validated
 * BEFORE the check digit, so a future format (whose checksum scheme we
 * cannot know) is reported as a version mismatch, not "invalid code".
 * Trade-off: a single-char typo AT the version position reports
 * "different version" rather than "typo" — still an error, never a
 * silently wrong tuple.
 *
 * Alphabets (unchanged from v1):
 *   Payload (the 12 payload chars):
 *     Crockford base-32 — 0-9, A-H, J-K, M-N, P-T, V-Z (no I/L/O/U).
 *     Decoder applies the Crockford loose aliases (I→1, L→1, O→0) to
 *     these positions so a misread 'I' or 'O' is still accepted.
 *   Version char: '2' (a digit — unaffected by aliasing).
 *   Check digit (the 14th char):
 *     ISO 7064 MOD 37,36 base-36 alphabet — 0-9A-Z. The encoder can
 *     emit any of the 36 chars here, INCLUDING I/L/O/U. For this
 *     position the decoder does NOT apply the Crockford loose aliases
 *     — the user must type the check digit literally as displayed.
 *     This preserves the ISO 7064 single-char-typo detection
 *     guarantee; aliasing the check digit would silently collapse
 *     three of the 36 output slots and break detection.
 *
 * The check digit is computed over version char + 12 payload chars, so
 * every single-character substitution in those 13 positions (and the
 * check position itself) is detected (payload typos), or surfaces as a
 * version mismatch (version-char typos).
 */

#ifndef NETPLAY_ROOM_CODE_H
#define NETPLAY_ROOM_CODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Format version character for the current (v2, S4b) layout. */
#define ROOM_CODE_VERSION_CHAR '2'

/* Nonce width: 12 CSPRNG bits (4096 variants). Encode rejects values
 * above the mask. */
#define ROOM_CODE_NONCE_BITS 12
#define ROOM_CODE_NONCE_MASK 0x0FFFu

/* 32-bit IPv4 + 16-bit port + 12-bit nonce = 60 payload bits. */
#define ROOM_CODE_PAYLOAD_BITS 60

/* 12 base-32 chars encode exactly 60 bits (12 * 5). */
#define ROOM_CODE_PAYLOAD_CHARS 12

#define ROOM_CODE_VERSION_CHARS 1

/* +1 ISO 7064 MOD 37,36 check digit. */
#define ROOM_CODE_CHECK_CHARS   1

/* Total printable characters in the code itself, no dashes: 14. */
#define ROOM_CODE_CHAR_LEN \
    (ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS + ROOM_CODE_CHECK_CHARS)

/* v1 codes were 11 chars (10 payload + 1 check, no version char); the
 * decoder recognizes checksum-valid 11-char input as OLD_FORMAT. */
#define ROOM_CODE_V1_CHAR_LEN  11
#define ROOM_CODE_V1_PAYLOAD_CHARS 10

/* Display form: single dash after the 7th char -> `XXXXXXX-XXXXXXX`
 * = 14 + 1 = 15 visible characters. One dash (not more) keeps the
 * code inside the wrapper OSD's 16-byte entry/history buffers while
 * still splitting the read-aloud into two 7-char halves. */
#define ROOM_CODE_DISPLAY_LEN  (ROOM_CODE_CHAR_LEN + 1)

/* Buffer size (bytes) required to hold the NUL-terminated display form. */
#define ROOM_CODE_BUF_LEN      (ROOM_CODE_DISPLAY_LEN + 1)

/* Alias so callers can pass-through the expected buffer size without
 * recomputing. */
enum { RoomCode_DisplayBufLen = ROOM_CODE_BUF_LEN };

/*
 * Decode outcome. Deliberately NOT a bool: OLD_FORMAT and
 * FUTURE_VERSION must reach the UI as distinct, explainable outcomes
 * (S4b), and an enum return forces every caller to handle them. There
 * is no bool-returning decode API anymore — a stale caller fails to
 * compile instead of silently inverting the check.
 */
typedef enum RoomCodeDecodeResult {
    ROOM_CODE_OK = 0,
    ROOM_CODE_MALFORMED,      /* wrong length/char/check digit — a typo'd
                                 or garbage code */
    ROOM_CODE_OLD_FORMAT,     /* checksum-valid 11-char v1 code — the code
                                 CREATOR runs a pre-S4b build */
    ROOM_CODE_FUTURE_VERSION, /* 14-char code whose version char is not
                                 '2' — created by a newer (or corrupted)
                                 build */
} RoomCodeDecodeResult;

/*
 * Encode (ip_be, public_port, nonce) into the display-form room code.
 * `ip_be` is already in network byte order (big-endian); `public_port`
 * is passed in host byte order; `nonce` must be <= ROOM_CODE_NONCE_MASK
 * (typically from RoomCode_GenerateNonce). Returns false on NULL
 * buffer or out-of-range nonce.
 */
bool RoomCode_Encode(uint32_t ip_be, uint16_t public_port, uint16_t nonce,
                     char out_code[ROOM_CODE_BUF_LEN]);

/*
 * Decode a display-form room code (with or without dashes, any case)
 * back into the tuple. Validates the version char first, then the
 * ISO 7064 MOD 37,36 check digit. Outputs are written only on
 * ROOM_CODE_OK; they are zeroed on every other result.
 */
RoomCodeDecodeResult RoomCode_Decode(const char* code,
                                     uint32_t* ip_be,
                                     uint16_t* public_port,
                                     uint16_t* nonce);

/*
 * Draw a fresh 12-bit nonce from the platform CSPRNG (utils/csprng.h).
 * Returns false — WITHOUT writing a weak fallback value — when the
 * CSPRNG is unavailable; callers must treat that as a hard error (a
 * predictable nonce would silently void the S4b guessing protection).
 */
bool RoomCode_GenerateNonce(uint16_t* out_nonce);

/*
 * Normalize user input: strip '-' and whitespace, upper-case letters,
 * apply Crockford's loose-alias mappings (I→1, L→1, O→0). Writes up to
 * out_cap-1 chars and always NUL-terminates when out_cap > 0. Returns
 * the number of normalized chars written (excluding the NUL).
 */
size_t RoomCode_NormalizeInput(const char* in, char* out, size_t out_cap);

#endif /* NETPLAY_ROOM_CODE_H */
