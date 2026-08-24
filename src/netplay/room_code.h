/*
 * room_code.h — Step 2 of docs/plan-stun-direct-p2p.md; format v3 as of
 * the S4 adversarial-review fixes (docs/plan-netplay-connection.md §6.8).
 *
 * Packs the (ip_be, public_port, nonce) tuple into a short
 * human-typeable identifier:
 *
 *   version char '3'                                      (1 char)
 *   80-bit payload: ip(32) << 48 | port(16) << 32 | nonce(32)
 *     -> 16 Crockford base-32 chars, MSB first              (16 chars)
 *   ISO 7064 MOD 37,36 check digit over the preceding 17    (1 char)
 *
 * = 18 alphanumeric chars, display-formatted with dashes after the 6th
 * and 12th chars: `XXXXXX-XXXXXX-XXXXXX` (20 visible characters).
 *
 * ---------------------------------------------------------------------
 * v3 rationale #1 — the nonce is now 32 bits (was 12).
 *
 * The S4a punch token is `SHA-256("3SXR-PT3" || payload10)[0..7]`, and
 * an attacker who knows the advertised ip:port (which the code reveals,
 * by necessity) knows every derivation input EXCEPT the nonce. At 12
 * bits that is a 4096-value search against a host that is a perfect
 * oracle (a hit is accepted and handed off, a miss is silently dropped
 * and the host keeps waiting forever). The host's receive path drains
 * roughly 60 datagrams/second, so the whole 12-bit space was
 * exhaustible in <= 68 s by one unprivileged sender — and with a UPnP
 * mapping the advertised port is STABLE, so a past opponent could do it
 * months later. 32 bits turns that 68 s into >= 2.2 years of sustained
 * flooding; combined with the host-side attempt cap (direct_p2p.c,
 * host_punch_gate_note_drop) it is unreachable.
 *
 * Why the entropy MUST live in the code: the joiner derives the punch
 * token from the decoded code ALONE, before (and often without) any
 * rendezvous exchange — the direct-punch attempt is the first step of
 * the cascade. Carrying the nonce out-of-band via the rendezvous server
 * would leave the direct path with nothing to authenticate, and the
 * session key is what INDEXES the rendezvous slot, so it cannot be
 * delivered by the thing it indexes. 5 bits/char means 32 nonce bits
 * cost exactly 4 more payload chars (48 fixed bits + 32 = 80 = 16 * 5),
 * which is the minimum possible for the requirement.
 *
 * Cost, stated plainly: 4 more on-screen-keyboard presses and one more
 * dash group when reading the code aloud.
 *
 * v3 rationale #2 — the check digit now actually detects every single-
 * character typo.
 *
 * v1/v2 computed the ISO 7064 hybrid recurrence with the intermediate
 * sum taken mod 37 (M+1) instead of mod 36 (M). That let the running
 * product reach 0 as well as 36, and `(37 - product) % 36` maps BOTH to
 * check char '1' — collapsing 37 product states onto 36 outputs. The
 * documented "every single-character substitution is detected"
 * guarantee was therefore false: a measured 2508 of 1,960,000
 * substitutions (0.128%, ~1 in 781) decoded silently to a DIFFERENT
 * endpoint. Worked example against the v2 codec: "248BVXBAA4DNM1" and
 * "2E8BVXBAA4DNM1" BOTH returned ROOM_CODE_OK, for 34.23.190.173 and
 * 114.23.190.173 respectively — a one-character typo aimed 15 s of UDP
 * punch traffic at an uninvolved third party and then reported
 * HOST_OFFLINE, never "Invalid room code".
 *
 * v3 uses the correct ISO/IEC 7064 hybrid recurrence (sum mod M), whose
 * product range is [1, 36] and whose check map is therefore a bijection.
 * Measured over the full alphabet at every position: 0 undetected
 * substitutions in 2,520,000 (see test_room_code.c full_alphabet_sweep).
 *
 * ---------------------------------------------------------------------
 * S4b rationale — why there is a nonce at all (unchanged from v2):
 * the v1 code was a bare reversible (ip, port) encoding. Anyone who
 * saw the code learned the host's home IP, and — because the
 * rendezvous session key and the S4a punch token are DERIVED from the
 * code payload — anyone who could GUESS the payload (ip:port scans of
 * plausible ranges) could derive the session key and squat/race the
 * pairing. The CSPRNG nonce mixes unguessable variants into every
 * derivation: the payload can no longer be reconstructed from the
 * observable (ip, port) alone. The code still necessarily CONTAINS the
 * ip (the joiner must reach the host) — the nonce protects the derived
 * key material, not ip privacy for someone holding the full code.
 *
 * Version prefix: the leading '3' makes format changes detectable. An
 * 11-char v1 code or a 14-char v2 code (each checksum-valid under the
 * LEGACY recurrence) decodes to ROOM_CODE_OLD_FORMAT so the UI can say
 * "this code is from an older version" instead of a mysterious failure;
 * an 18-char code with an unknown version char reports
 * ROOM_CODE_FUTURE_VERSION. The version char is validated BEFORE the
 * check digit, so a future format (whose checksum scheme we cannot
 * know) is reported as a version mismatch, not "invalid code".
 * Trade-off: a single-char typo AT the version position reports
 * "different version" rather than "typo" — still an error, never a
 * silently wrong tuple.
 *
 * Alphabets (unchanged from v1):
 *   Payload (the 16 payload chars):
 *     Crockford base-32 — 0-9, A-H, J-K, M-N, P-T, V-Z (no I/L/O/U).
 *     Decoder applies the Crockford loose aliases (I→1, L→1, O→0) to
 *     these positions so a misread 'I' or 'O' is still accepted.
 *   Version char: '3' (a digit — unaffected by aliasing).
 *   Check digit (the 18th char):
 *     ISO 7064 MOD 37,36 base-36 alphabet — 0-9A-Z. The encoder can
 *     emit any of the 36 chars here, INCLUDING I/L/O/U. For this
 *     position the decoder does NOT apply the Crockford loose aliases
 *     — the user must type the check digit literally as displayed.
 *     This preserves the ISO 7064 single-char-typo detection
 *     guarantee; aliasing the check digit would silently collapse
 *     three of the 36 output slots and break detection.
 *
 * The check digit is computed over version char + 16 payload chars, so
 * every single-character substitution in those 17 positions (and the
 * check position itself) is detected (payload typos), or surfaces as a
 * version mismatch (version-char typos).
 */

#ifndef NETPLAY_ROOM_CODE_H
#define NETPLAY_ROOM_CODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Format version character for the current (v3) layout. */
#define ROOM_CODE_VERSION_CHAR '3'

/* Nonce width: 32 CSPRNG bits (4.29e9 variants). The full uint32_t
 * range is representable, so there is no encode-time range check to
 * perform — the mask is kept for callers/tests that want the bound
 * symbolically. */
#define ROOM_CODE_NONCE_BITS 32
#define ROOM_CODE_NONCE_MASK 0xFFFFFFFFu

/* 32-bit IPv4 + 16-bit port + 32-bit nonce = 80 payload bits. */
#define ROOM_CODE_PAYLOAD_BITS 80

/* 16 base-32 chars encode exactly 80 bits (16 * 5). */
#define ROOM_CODE_PAYLOAD_CHARS 16

#define ROOM_CODE_VERSION_CHARS 1

/* +1 ISO 7064 MOD 37,36 check digit. */
#define ROOM_CODE_CHECK_CHARS   1

/* Total printable characters in the code itself, no dashes: 18. */
#define ROOM_CODE_CHAR_LEN \
    (ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS + ROOM_CODE_CHECK_CHARS)

/* v1 codes were 11 chars (10 payload + 1 check, no version char); v2
 * codes were 14 chars ('2' + 12 payload + 1 check). The decoder
 * recognizes either — when checksum-valid under the LEGACY (pre-v3)
 * recurrence — as OLD_FORMAT. */
#define ROOM_CODE_V1_CHAR_LEN  11
#define ROOM_CODE_V1_PAYLOAD_CHARS 10
#define ROOM_CODE_V2_CHAR_LEN  14
#define ROOM_CODE_V2_VERSION_CHAR '2'

/* Display form: dashes after the 6th and 12th chars ->
 * `XXXXXX-XXXXXX-XXXXXX` = 18 + 2 = 20 visible characters. Three
 * 6-char groups read aloud more reliably than two 9-char ones.
 * NOTE: the wrapper OSD's peer-code entry/history buffers must be at
 * least ROOM_CODE_BUF_LEN — see the DP2P_CODE_CHAR_LEN /
 * g_dp2p_code_buf sizing in tools/mister-wrapper/main-mister-full-menu.patch,
 * which is kept in lockstep with these constants. */
#define ROOM_CODE_DISPLAY_LEN  (ROOM_CODE_CHAR_LEN + 2)

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
    ROOM_CODE_OLD_FORMAT,     /* legacy-checksum-valid 11-char v1 or
                                 14-char v2 code — the code CREATOR runs
                                 an older build */
    ROOM_CODE_FUTURE_VERSION, /* 18-char code whose version char is not
                                 '3' — created by a newer (or corrupted)
                                 build */
} RoomCodeDecodeResult;

/*
 * Encode (ip_be, public_port, nonce) into the display-form room code.
 * `ip_be` is already in network byte order (big-endian); `public_port`
 * is passed in host byte order; `nonce` is a full 32-bit value
 * (typically from RoomCode_GenerateNonce — every uint32_t is in range).
 * Returns false on NULL buffer.
 */
bool RoomCode_Encode(uint32_t ip_be, uint16_t public_port, uint32_t nonce,
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
                                     uint32_t* nonce);

/*
 * Draw a fresh 32-bit nonce from the platform CSPRNG (utils/csprng.h).
 * Returns false — WITHOUT writing a weak fallback value — when the
 * CSPRNG is unavailable; callers must treat that as a hard error (a
 * predictable nonce would silently void the S4b guessing protection).
 */
bool RoomCode_GenerateNonce(uint32_t* out_nonce);

/*
 * Normalize user input: strip '-' and whitespace, upper-case letters,
 * apply Crockford's loose-alias mappings (I→1, L→1, O→0). Writes up to
 * out_cap-1 chars and always NUL-terminates when out_cap > 0. Returns
 * the number of normalized chars written (excluding the NUL).
 */
size_t RoomCode_NormalizeInput(const char* in, char* out, size_t out_cap);

#endif /* NETPLAY_ROOM_CODE_H */
