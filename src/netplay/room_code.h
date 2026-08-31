/*
 * room_code.h — Step 2 of docs/plan-stun-direct-p2p.md; format v4 as of
 * task #155 (docs/queue.md #155, 2026-08-31).
 *
 * Packs the (ip_be, public_port) pair into a short human-typeable
 * identifier:
 *
 *   version char '4'                                       (1 char)
 *   48-bit payload: ip(32) << 16 | port(16), zero-padded to 50 bits
 *     -> 10 Crockford base-32 chars, MSB first                (10 chars)
 *   ISO 7064 MOD 37,36 check digit over the preceding 11    (1 char)
 *
 * = 12 alphanumeric chars, no hyphen groups — the whole point of v4 is
 * that this fits on one line read aloud or typed on a controller pad.
 *
 * ---------------------------------------------------------------------
 * v4 — why: v3 was 18 chars (20 displayed, with two hyphen groups), and
 * that was hurting the one thing the code exists for — being read aloud
 * to a friend and typed on an on-screen keyboard. Where the length had
 * gone (v1 was 11 = 10 payload + 1 check):
 *
 *   +6 payload chars — the 32-bit CSPRNG nonce (ROOM_CODE_NONCE_BITS).
 *   +1 char          — the version prefix.
 *   +0                — the check digit; it was already there in v1.
 *
 * v4 removes the nonce from the code. The version prefix and the check
 * digit both STAY — see the two sections below for why each one earns
 * its character.
 *
 * ---------------------------------------------------------------------
 * v4 — the nonce leaves the code, the accepted risk this creates.
 *
 * The rendezvous session key and the S4a punch token
 * (Rendezvous_DeriveSessionKey / Rendezvous_DerivePunchToken,
 * rendezvous.h) are DERIVED from the code payload. Every v4 caller now
 * passes ROOM_CODE_V4_FIXED_NONCE (0) in the nonce slot those functions
 * still take, so both derivations are a PURE FUNCTION of the public
 * (ip, port) tuple — the same exposure v1 had, before S4b added the
 * nonce specifically to close it: someone who knows or scans the host's
 * IP can derive the session key and squat or race the pairing.
 *
 * ACCEPTED for friend-to-friend P2P, where the ceiling is "a stranger
 * joins your match" or "your friend has to retry", and — unlike v1 —
 * this is now backstopped by the host-side punch-gate MUTE
 * (S4-review HIGH-1b, direct_p2p.c host_punch_gate_note_bad /
 * host_punch_gate_is_muted): a source that fails the punch-token check
 * repeatedly is muted for HOST_PUNCH_MUTE_MS and learns nothing from a
 * correct guess. Not accepted silently: if that mute is ever weakened
 * or removed, this decision reopens.
 *
 * Consequence for the punch-gate RE-ROLL (the other half of HIGH-1b):
 * under v3 a "re-roll" drew a fresh nonce, which changed the token
 * without changing the endpoint. Under v4 the token is `f(ip, port)`
 * with a FIXED nonce, so there is no longer anything to roll — the only
 * way to invalidate a v4 token is to change the advertised endpoint,
 * which nothing here does on demand. `host_reroll_room_code` and the
 * session-total escalation that called it are REMOVED; the per-source
 * mute is the sole backstop from here.
 *
 * Why the nonce machinery (RoomCode_GenerateNonce, the Csprng_Bytes
 * hard-fail path, the "3SXR-SK4"/"3SXR-PT4" domain-separated
 * derivations in rendezvous.c) stays COMPILED AND UNIT-TESTED despite
 * being unused by the code string: a room-LIST feature changes the
 * delivery channel. Today the nonce would have to ride in the code
 * because the joiner derives the punch token before any contact and the
 * session key is what INDEXES the rendezvous slot, so the server cannot
 * hand it out. A list breaks that circularity — pick a room, receive the
 * nonce out-of-band — restoring unguessable derivations at zero
 * character cost. Parking this behind `#if` was tried once already, by
 * a different subsystem: `LOSSY_ADAPTER` silently stopped compiling
 * when the warning set tightened (see `4ca4c0de`), and the rot cost this
 * project its only local way to exercise rollback. Dormant-but-built,
 * not dormant-and-uncompiled, is the requirement here too.
 *
 * ---------------------------------------------------------------------
 * v4 — the 2 spare payload bits.
 *
 * 10 base-32 chars carry 50 bits; the fixed payload (ip 32 + port 16) is
 * only 48. The 2 spare bits are ZERO-FILLED, placed as the low 2 bits of
 * the 50-bit stream (i.e. the last payload char's low 2 bits) — the
 * simplest option, and there is no present use for them: a real 49th/
 * 50th payload bit would need a reason to exist, and "reserve them at
 * zero, decode-ignore them" costs nothing.
 *
 * The decoder does NOT validate them (a nonzero pad is not rejected) —
 * see room_code.c room_code_pack_payload for where that is implemented.
 * What that tolerance actually buys: NOT a version bump. A real
 * successor format (what the section below calls v5) changes the
 * version char, and RoomCode_Decode rejects an unrecognized version
 * char as FUTURE_VERSION before it ever reaches the pad bits — so this
 * decoder would have to change for a v5 regardless of what these 2 bits
 * end up meaning, and pad tolerance buys it nothing there. The narrow
 * case it DOES cover is a same-version-char ('4') refinement that finds
 * a use for the 2 already-allocated bits without forcing a version
 * bump — there is no concrete plan for one today, but reserving the
 * option costs nothing and this is the only shape of future format it
 * actually helps.
 *
 * ---------------------------------------------------------------------
 * v4 — why the version char and length dispatch STAY (questioned by the
 * maintainer, deliberately kept).
 *
 * They are not what stops an incompatible BUILD from pairing — the MIST
 * handshake's state_ver/proto_ver/arch/digest gate does that
 * (mist_handshake.c), and a room-code format change implies a build
 * change, so the handshake would reject the session anyway once two
 * peers actually talk. What the version char and length dispatch buy is
 * earlier and more honest: a misparse produces a WRONG ADDRESS, not a
 * wrong peer — there is nobody there to hand off to, so there is no
 * handshake to defer the check to. Length dispatch already makes a
 * different-length string fail clean as MALFORMED; the version char
 * covers the same-length case (a same-length code from a hypothetical
 * v4-successor format). Cheap, so both stay.
 *
 * v4 breaks pairing with v3 regardless: the punch-token DERIVATION
 * changed (fixed nonce vs. a carried one), not merely the string, so a
 * v4 client and a v3 client cannot find each other at the rendezvous
 * layer or validate each other's punch even if a code somehow crossed
 * formats. MIST_PROTO_VER (mist_handshake.h) is UNCHANGED by this task:
 * it gates the handshake MESSAGE FORMAT, and this task changes zero
 * bytes of it. Two builds that disagree on room-code format never reach
 * a handshake to disagree about — the incompatibility is caught earlier,
 * by the version char / rendezvous-slot mismatch above, the same way a
 * length mismatch always was.
 *
 * ---------------------------------------------------------------------
 * v4 — why the check digit STAYS, and stays free.
 *
 * It costs nothing: v1 already had one. Without it the payload is
 * dense — every well-formed 10-char string decodes to SOME real
 * ip:port, so a typo silently becomes a code for somewhere else.
 *
 * v3 fixed the check-digit arithmetic: v1/v2 computed the ISO 7064
 * hybrid recurrence with the intermediate sum taken mod 37 (M+1) instead
 * of mod 36 (M). That let the running product reach 0 as well as 36, and
 * `(37 - product) % 36` maps BOTH to check char '1' — collapsing 37
 * product states onto 36 outputs, so "every single-character
 * substitution is detected" was false in practice: a measured 2508 of
 * 1,960,000 substitutions (0.128%, ~1 in 781) decoded silently to a
 * DIFFERENT endpoint. Worked example against the v2 codec:
 * "248BVXBAA4DNM1" and "2E8BVXBAA4DNM1" BOTH returned ROOM_CODE_OK, for
 * 34.23.190.173 and 114.23.190.173 respectively.
 *
 * v4 keeps v3's corrected recurrence (sum mod M, a bijection onto
 * [0, 35]) over version char + 10 payload chars (11 covered positions,
 * down from v3's 17). Re-measured over the new geometry:
 * test_room_code.c full_alphabet_sweep — see that file for the current
 * count.
 *
 * ---------------------------------------------------------------------
 * Length dispatch — collision-free, every prior length still
 * recognized:
 *
 *   11 chars -> v1 legacy   (LEGACY sum-mod-37 recurrence)
 *   12 chars -> v4, CURRENT (corrected sum-mod-36 recurrence)
 *   14 chars -> v2 legacy   (LEGACY sum-mod-37 recurrence)
 *   18 chars -> v3 legacy   (corrected sum-mod-36 recurrence — v3 was
 *                            already fixed; only its LENGTH is now
 *                            legacy, not its arithmetic)
 *
 * A legacy-checksum-valid v1/v2/v3 code decodes to ROOM_CODE_OLD_FORMAT
 * (the UI says "this code is from an older version") rather than
 * failing as garbage; a checksum-INVALID string of one of those lengths
 * is plain MALFORMED. An 18-char v3 code IS recognized (version char
 * '3', the CORRECT recurrence) but its tuple is never decoded — a v3
 * nonce cannot seed a v4 (nonce-less) derivation, so OLD_FORMAT is the
 * only honest answer regardless.
 *
 * Alphabets (unchanged from v1):
 *   Payload (the 10 payload chars):
 *     Crockford base-32 — 0-9, A-H, J-K, M-N, P-T, V-Z (no I/L/O/U).
 *     Decoder applies the Crockford loose aliases (I→1, L→1, O→0) to
 *     these positions so a misread 'I' or 'O' is still accepted.
 *   Version char: '4' (a digit — unaffected by aliasing).
 *   Check digit (the 12th char):
 *     ISO 7064 MOD 37,36 base-36 alphabet — 0-9A-Z. The encoder can
 *     emit any of the 36 chars here, INCLUDING I/L/O/U. For this
 *     position the decoder does NOT apply the Crockford loose aliases
 *     — the user must type the check digit literally as displayed.
 *     This preserves the ISO 7064 single-char-typo detection
 *     guarantee; aliasing the check digit would silently collapse
 *     three of the 36 output slots and break detection.
 *
 * ---------------------------------------------------------------------
 * v4 — redaction (RoomCode_Redact) no longer hides a secret.
 *
 * Pre-S4b (v1) the code was a reversible (ip, port) encoding and was
 * never redacted — "logging it in full leaked nothing the ip:port on
 * the same line did not" (the original rationale). S4b added the nonce
 * and, with it, RoomCode_Redact, because the nonce fed key material
 * (the session key and the punch token) that was NOT otherwise visible
 * in the log line.
 *
 * v4 is back in v1's situation: the payload IS the public (ip, port)
 * tuple, the session key and punch token are a pure function of it (see
 * the accepted-risk section above), and there is no longer any secret
 * bit in the code to protect. ROOM_CODE_REDACT_CHARS is kept anyway, at
 * a much smaller width, purely as a HYGIENE measure — so a shared log or
 * screenshot is not a literal ready-to-paste code — not as a
 * confidentiality control: the (ip, port) it encodes remains fully
 * visible in the same log line and is not what is being protected. See
 * ROOM_CODE_REDACT_CHARS below.
 */

#ifndef NETPLAY_ROOM_CODE_H
#define NETPLAY_ROOM_CODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Format version character for the current (v4) layout. */
#define ROOM_CODE_VERSION_CHAR '4'

/* v3's nonce width. The code payload no longer carries a nonce (see the
 * "nonce leaves the code" section above), but RoomCode_GenerateNonce
 * below is kept compiled and unit-tested, and these macros document the
 * width it still draws. */
#define ROOM_CODE_NONCE_BITS 32
#define ROOM_CODE_NONCE_MASK 0xFFFFFFFFu

/* v4: the rendezvous session key and S4a punch token derivations
 * (Rendezvous_DeriveSessionKey / Rendezvous_DerivePunchToken,
 * rendezvous.h) still take a nonce parameter — kept for the future
 * room-list feature described above — but every v4 room-code caller
 * passes this fixed value in it. ACCEPTED RISK: both derivations are
 * therefore a pure function of the public (ip, port) tuple. Not secret;
 * not meant to be — see the accepted-risk section above for what
 * backstops it. */
#define ROOM_CODE_V4_FIXED_NONCE 0u

/* v4: 32-bit IPv4 + 16-bit port = 48 fixed payload bits. No nonce. */
#define ROOM_CODE_PAYLOAD_BITS 48

/* 10 base-32 chars encode 50 bits — 2 more than the 48 needed; see the
 * "2 spare payload bits" section above. */
#define ROOM_CODE_PAYLOAD_CHARS 10

#define ROOM_CODE_VERSION_CHARS 1

/* +1 ISO 7064 MOD 37,36 check digit. */
#define ROOM_CODE_CHECK_CHARS   1

/* Total printable characters in the code: 12. */
#define ROOM_CODE_CHAR_LEN \
    (ROOM_CODE_VERSION_CHARS + ROOM_CODE_PAYLOAD_CHARS + ROOM_CODE_CHECK_CHARS)

/* Legacy lengths. v1 (11 = 10 payload + 1 check, no version char) and v2
 * (14 = '2' + 12 payload + 1 check) used the LEGACY (defective,
 * sum-mod-37) recurrence. v3 (18 = '3' + 16 payload + 1 check) used the
 * CORRECTED (sum-mod-36) recurrence — the same one v4 uses — so its
 * recognition path verifies differently from v1/v2's; see room_code.c.
 * The decoder recognizes all three, when checksum-valid, as
 * OLD_FORMAT. */
#define ROOM_CODE_V1_CHAR_LEN  11
#define ROOM_CODE_V1_PAYLOAD_CHARS 10
#define ROOM_CODE_V2_CHAR_LEN  14
#define ROOM_CODE_V2_VERSION_CHAR '2'
#define ROOM_CODE_V3_CHAR_LEN  18
#define ROOM_CODE_V3_PAYLOAD_CHARS 16
#define ROOM_CODE_V3_VERSION_CHAR '3'

/* v4: no hyphen groups. 12 chars is short enough to read and type as one
 * block, which is the entire point of this format — see the top-of-file
 * rationale. Display form == the bare code.
 * NOTE: the wrapper OSD's peer-code entry/history buffers must be at
 * least ROOM_CODE_BUF_LEN — see the DP2P_CODE_CHAR_LEN /
 * g_dp2p_code_buf sizing in tools/mister-wrapper/main-mister-full-menu.patch,
 * which is kept in lockstep with these constants. */
#define ROOM_CODE_DISPLAY_LEN  ROOM_CODE_CHAR_LEN

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
    ROOM_CODE_OLD_FORMAT,     /* legacy-checksum-valid 11-char v1,
                                 14-char v2, or 18-char v3 code — the
                                 code CREATOR runs an older build */
    ROOM_CODE_FUTURE_VERSION, /* 12-char code whose version char is not
                                 '4' — created by a newer (or corrupted)
                                 build */
} RoomCodeDecodeResult;

/*
 * Encode (ip_be, public_port) into the display-form room code. `ip_be`
 * is already in network byte order (big-endian); `public_port` is
 * passed in host byte order. Returns false on NULL buffer.
 */
bool RoomCode_Encode(uint32_t ip_be, uint16_t public_port,
                     char out_code[ROOM_CODE_BUF_LEN]);

/*
 * Decode a display-form room code (any case, with or without stray
 * dashes/whitespace — legacy display forms had them) back into the
 * (ip, port) pair. Validates the version char first, then the ISO 7064
 * MOD 37,36 check digit. Outputs are written only on ROOM_CODE_OK; they
 * are zeroed on every other result.
 */
RoomCodeDecodeResult RoomCode_Decode(const char* code,
                                     uint32_t* ip_be,
                                     uint16_t* public_port);

/*
 * Draw a fresh 32-bit nonce from the platform CSPRNG (utils/csprng.h).
 * Returns false — WITHOUT writing a weak fallback value — when the
 * CSPRNG is unavailable; callers must treat that as a hard error.
 *
 * v4: no production caller feeds this into RoomCode_Encode or a
 * rendezvous derivation anymore (see the "nonce leaves the code"
 * section above) — it is kept compiled and exercised directly by
 * test_room_code.c for the future room-list feature described there.
 */
bool RoomCode_GenerateNonce(uint32_t* out_nonce);

/*
 * S4-review MEDIUM-4 (v1-v3), re-scoped for v4: write a rendering of
 * `code` into `out` with the trailing ROOM_CODE_REDACT_CHARS characters
 * replaced by '*'.
 *
 * v4 no longer has a secret to protect this way — see the "redaction no
 * longer hides a secret" section above. This is now a hygiene measure
 * (never print a literal ready-to-paste code), not a confidentiality
 * one: the (ip, port) the redacted code encodes is fully recoverable
 * from the SAME log line's surrounding text either way.
 *
 * `out` must be at least ROOM_CODE_BUF_LEN; it is always NUL-terminated.
 * Accepts dashed or dash-free input (legacy display forms had dashes).
 * NULL/short input yields an empty string.
 */
#define ROOM_CODE_REDACT_CHARS 3
void RoomCode_Redact(const char* code, char out[ROOM_CODE_BUF_LEN]);

/*
 * Normalize user input: strip '-' and whitespace, upper-case letters,
 * apply Crockford's loose-alias mappings (I→1, L→1, O→0). Writes up to
 * out_cap-1 chars and always NUL-terminates when out_cap > 0. Returns
 * the number of normalized chars written (excluding the NUL).
 */
size_t RoomCode_NormalizeInput(const char* in, char* out, size_t out_cap);

#endif /* NETPLAY_ROOM_CODE_H */
