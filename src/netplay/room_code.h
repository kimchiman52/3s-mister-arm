/*
 * room_code.h — Step 2 of docs/plan-stun-direct-p2p.md.
 *
 * Packs the (ip_be, public_port) STUN endpoint tuple into a short
 * human-typeable identifier: 6 raw bytes → 10 Crockford base-32
 * characters → 1 ISO 7064 MOD 37,36 check digit → display-formatted as
 * `ABCDEFGH-IJK` (single dash for readability). The decoder strips
 * dashes, case-folds to upper, and applies the Crockford loose-alias
 * normalizations (I/i→1, L/l→1, O/o→0). The check digit rejects the
 * single-character typos and adjacent-transpositions humans make when
 * reading a code over voice chat.
 *
 * The peer's LAN local_port is intentionally NOT encoded. Earlier revs
 * included it to support NAT-hairpin routing via 127.0.0.1:local_port,
 * but that bet was both fragile (requires the joiner to learn the
 * host's LAN port) and unnecessary: any router that forwards a UDP
 * port to itself (NAT loopback / hairpin) will deliver the STUN-probed
 * public address back to the host. Routers that don't support NAT
 * loopback fail the same way they always did — the hole-punch times
 * out and the user sees "Cannot reach peer". Dropping local_port
 * shrinks the code from 14 → 11 chars.
 *
 * Alphabets:
 *   Payload (encode/decode of the 10 payload chars):
 *     Crockford base-32 alphabet — 0-9, A-H, J-K, M-N, P-T, V-Z (no I/L/O/U).
 *     Decoder applies the Crockford loose aliases (I→1, L→1, O→0) to
 *     these 10 positions so a misread 'I' or 'O' is still accepted.
 *   Check digit (the 11th char):
 *     ISO 7064 MOD 37,36 base-36 alphabet — 0-9A-Z. The encoder can
 *     emit any of the 36 chars here, INCLUDING I/L/O/U. For this
 *     position the decoder does NOT apply the Crockford loose aliases
 *     — user must type the check digit literally as displayed. This
 *     preserves the ISO 7064 single-char-typo detection guarantee;
 *     aliasing the check digit would silently collapse three of the
 *     36 output slots and break detection. Payload-position loose
 *     aliasing remains because the encoder never emits I/L/O there.
 *
 * API contracts (see prototypes below for the canonical signatures):
 *   encode: writes ROOM_CODE_DISPLAY_LEN printable chars plus a NUL
 *     terminator into the caller's buffer (which must be at least
 *     ROOM_CODE_BUF_LEN bytes). Returns false only if the buffer is
 *     NULL.
 *   decode: accepts input with or without dashes, any case, with
 *     Crockford loose aliases. Writes the decoded tuple through the
 *     out params. Returns false on malformed input (wrong length
 *     after normalization, unknown character, or check-digit
 *     mismatch).
 *   normalize: strips dashes and whitespace, upper-cases, maps I/L→1
 *     and O→0, then NUL-terminates. Writes up to out_cap-1 chars and
 *     always NUL-terminates if out_cap > 0. Returns the number of
 *     normalized chars written (excluding the terminator).
 */

#ifndef NETPLAY_ROOM_CODE_H
#define NETPLAY_ROOM_CODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 4 bytes IPv4 (big-endian) + 2 bytes public_port (host order, stored BE
 * inside the payload) = 6 bytes of payload. */
#define ROOM_CODE_RAW_LEN      6

/* 10 base-32 chars encode 6 bytes (6*8 = 48 bits, 10 chars * 5 bits = 50
 * bits — the top 2 bits of the 1st char are unused/zero on encode and
 * rejected if non-zero on decode). */
#define ROOM_CODE_PAYLOAD_CHARS 10

/* +1 ISO 7064 MOD 37,36 check digit. */
#define ROOM_CODE_CHECK_CHARS   1

/* Total printable characters in the code itself, no dashes. */
#define ROOM_CODE_CHAR_LEN     (ROOM_CODE_PAYLOAD_CHARS + ROOM_CODE_CHECK_CHARS)

/* Display form: single dash between the 8-char payload prefix and the
 * remaining 2 payload chars + check digit → "ABCDEFGH-IJK" = 11 + 1 = 12
 * visible characters. We keep one dash (not zero) because a single
 * separator meaningfully improves read-aloud accuracy without pushing
 * the code across a 12-char visual budget. */
#define ROOM_CODE_DISPLAY_LEN  (ROOM_CODE_CHAR_LEN + 1)

/* Buffer size (bytes) required to hold the NUL-terminated display form. */
#define ROOM_CODE_BUF_LEN      (ROOM_CODE_DISPLAY_LEN + 1)

/* Alias so callers can pass-through the expected buffer size without
 * recomputing. */
enum { RoomCode_DisplayBufLen = ROOM_CODE_BUF_LEN };

/*
 * Encode an IPv4 + public port tuple into the display-form room code.
 * `ip_be` is already in network byte order (big-endian); `public_port`
 * is passed in host byte order and stored big-endian inside the payload
 * so the wire-format is stable across byte orders.
 */
bool RoomCode_Encode(uint32_t ip_be, uint16_t public_port,
                     char out_code[ROOM_CODE_BUF_LEN]);

/*
 * Decode a display-form room code (with or without dashes, any case)
 * back into the tuple. Validates the ISO 7064 MOD 37,36 check digit and
 * returns false if it does not match.
 */
bool RoomCode_Decode(const char* code,
                     uint32_t* ip_be, uint16_t* public_port);

/*
 * Normalize user input: strip '-' and whitespace, upper-case letters,
 * apply Crockford's loose-alias mappings (I→1, L→1, O→0). Writes up to
 * out_cap-1 chars and always NUL-terminates when out_cap > 0. Returns
 * the number of normalized chars written (excluding the NUL).
 */
size_t RoomCode_NormalizeInput(const char* in, char* out, size_t out_cap);

#endif /* NETPLAY_ROOM_CODE_H */
