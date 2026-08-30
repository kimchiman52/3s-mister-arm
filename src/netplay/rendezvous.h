/*
 * rendezvous.h — Step 3 of docs/plan-bilateral-hole-punch.md.
 *
 * Pure rendezvous-protocol client. No SDL_net dependencies. Wire format
 * per docs/plan-bilateral-hole-punch.md §Decision 2.
 *
 * This module is intentionally I/O-free: callers own the socket, the
 * thread, the schedule, and the cancellation. The functions here only
 * (a) derive the session key / punch token from the same
 * (ip_be, public_port, nonce) tuple the room code encodes, (b) build
 * the 36-byte v2 REGISTER and POLL packets (8-byte cookie tail, S4c),
 * (c) parse a 32-byte DELIVER or CHALLENGE, and (d) parse the
 * configured `udp://host:port` signal URL.
 *
 * Security note (v3): the session key is
 * SHA-256("3SXR-SK3" || ip[4] || port_be[2] || nonce_be[4])[0..15] —
 * the byte-aligned serialization of the v3 room-code payload including
 * the 32-bit CSPRNG nonce, domain-separated from the S4a punch token
 * ("3SXR-PT3"). It is derived (not transmitted) and not user-typeable.
 *
 * The domain strings carry the room-code format version ("...2" -> "...3")
 * so a build that somehow paired a v2 payload with a v3 derivation
 * cannot land in the same rendezvous slot or produce a matching punch
 * token. The '3SXR' WIRE version is unaffected and stays 2 — the
 * session key is a 16-byte opaque blob to the server.
 */

#ifndef NETPLAY_RENDEZVOUS_H
#define NETPLAY_RENDEZVOUS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Derive the 16-byte rendezvous session key from the public endpoint
 * tuple + nonce that both peers share via the room code (v3).
 *
 * `ip_be` follows the same convention as room_code.c: the uint32_t's
 * in-memory bytes are the four IPv4 octets in network byte order (i.e.
 * the value returned by inet_pton(AF_INET, ...) into `s_addr`).
 * `nonce` is the 32-bit room-code nonce (host order; every value is
 * in range).
 *
 * The derivation is domain-separated and covers the full v3 payload:
 *
 *   key = SHA-256("3SXR-SK3" || ip[4] || port_be[2] || nonce_be[4])[0..15]
 *
 * where nonce_be is the nonce as a 32-bit big-endian value. Because the
 * 32-bit CSPRNG nonce is inside the hash, an attacker who merely
 * knows/guesses (ip, port) can no longer derive the session key to
 * squat or race the rendezvous slot — only someone holding the actual
 * room code can. BREAKING change from the v2 derivation (12-bit nonce,
 * "3SXR-SK2"), shipped together with the v3 room-code format to the
 * whole alpha group.
 *
 * Writes 16 bytes into `out_key`. Returns true on success. On failure
 * (ip_be == 0, indicating an unset / invalid endpoint), zeroes
 * `out_key` and returns false.
 */
bool Rendezvous_DeriveSessionKey(uint32_t ip_be,
                                 uint16_t public_port,
                                 uint32_t nonce,
                                 uint8_t out_key[16]);

/*
 * S4a (docs/plan-netplay-connection.md §6): derive the 8-byte punch
 * authentication token both peers append to the "3SX_PUNCH" hole-punch
 * payload. Same inputs and payload serialization as
 * Rendezvous_DeriveSessionKey, different domain string:
 *
 *   token = SHA-256("3SXR-PT3" || ip[4] || port_be[2] || nonce_be[4])[0..7]
 *
 * so the two derivations can never collide. The token proves knowledge
 * of the room-code payload INCLUDING the nonce: the host only accepts
 * a punch carrying it, so a blind scanner — or an attacker who
 * observed the host's ip:port but not the code — can no longer be
 * captured as "the peer". Anyone who has the full room code can of
 * course derive it — the room code IS the shared secret in this
 * friend-to-friend model.
 *
 * The nonce is 32 bits as of v3. At 12 bits the token had only 4096
 * unguessable variants against a host that answered every guess (accept
 * -> handoff, miss -> silent drop, no cap), which the host receive path
 * could be walked through in under 68 seconds. See room_code.h.
 *
 * Writes 8 bytes into out_token. Returns true on success; on failure
 * (ip_be == 0 or hash failure) zeroes out_token and returns false.
 */
#define REND_PUNCH_TOKEN_LEN 8
bool Rendezvous_DerivePunchToken(uint32_t ip_be,
                                 uint16_t public_port,
                                 uint32_t nonce,
                                 uint8_t out_token[REND_PUNCH_TOKEN_LEN]);

/* S4c protocol v2 sizes: REGISTER/POLL carry an 8-byte return-
 * routability cookie tail (see Rendezvous_ParseChallenge). */
#define REND_COOKIE_LEN 8
#define REND_REGISTER_PKT_LEN 36

/*
 * Build a 36-byte v2 REGISTER packet (type=1) for the rendezvous
 * server.
 *
 * `my_public_port` is the caller's STUN-observed public port in HOST
 * byte order (matches StunResult.public_port). The encoder writes it
 * big-endian on the wire. `session_key` is the 16-byte key from
 * Rendezvous_DeriveSessionKey. `cookie` is the 8-byte server cookie
 * from the most recent CHALLENGE, or NULL for the initial uncookied
 * REGISTER (encoded as all-zeros — the server replies with a CHALLENGE
 * instead of binding a slot). `out_pkt` MUST point to a 36-byte buffer.
 *
 * Returns true on success, false on NULL key/buffer.
 */
bool Rendezvous_BuildRegister(uint16_t my_public_port,
                              const uint8_t session_key[16],
                              const uint8_t cookie[REND_COOKIE_LEN],
                              uint8_t out_pkt[REND_REGISTER_PKT_LEN]);

/*
 * Build a 36-byte v2 POLL packet (type=3) for the rendezvous server.
 * Same shape as REGISTER but with a zero port field. `out_pkt` MUST
 * point to a 36-byte buffer.
 *
 * Returns true on success, false on NULL key/buffer.
 */
bool Rendezvous_BuildPoll(const uint8_t session_key[16],
                          const uint8_t cookie[REND_COOKIE_LEN],
                          uint8_t out_pkt[REND_REGISTER_PKT_LEN]);

/*
 * Parse a 32-byte v2 CHALLENGE packet (type=4, server -> client). The
 * server answers any REGISTER/POLL whose cookie is missing/stale with
 * a CHALLENGE carrying a cookie bound to the request's SOURCE address
 * (return-routability, S4c): only a client that can actually RECEIVE
 * at its claimed source learns the cookie, so a source-spoofed
 * REGISTER can no longer occupy a slot or steer a victim's punch
 * traffic. Layout: magic(4) ver(1) type(1) reserved(2) key(16)
 * cookie(8).
 *
 * Validates magic/version/type and that the embedded session key
 * matches `expected_session_key` (cross-talk + forgery gate — the key
 * embeds the S4b nonce, which an off-path attacker cannot know). On
 * success writes the 8-byte cookie into out_cookie and returns true;
 * on any mismatch zeroes out_cookie and returns false.
 */
bool Rendezvous_ParseChallenge(const uint8_t* pkt, int len,
                               const uint8_t expected_session_key[16],
                               uint8_t out_cookie[REND_COOKIE_LEN]);

/*
 * Tri-state DELIVER parse result (S3 failure taxonomy,
 * docs/plan-netplay-connection.md). The rendezvous server answers EVERY
 * REGISTER with a DELIVER — a real endpoint when the peer is paired, or
 * the 0.0.0.0:0 zero-sentinel when it is not — so callers counting
 * DELIVER frames can distinguish "server down" (no DELIVERs at all)
 * from "host offline / code stale" (only zero-sentinel DELIVERs). The
 * pre-S3 bool API conflated MALFORMED with EMPTY, throwing that
 * evidence away.
 */
typedef enum RendezvousDeliverResult {
    REND_DELIVER_MALFORMED = 0, /* bad magic/version/type/key/length — drop */
    REND_DELIVER_EMPTY,         /* valid, zero-sentinel: peer not yet registered */
    REND_DELIVER_PEER,          /* valid, real peer endpoint in the outputs */
} RendezvousDeliverResult;

/*
 * Parse a DELIVER packet (type=2, 32 bytes) received from the
 * rendezvous server.
 *
 * Validates magic, version, type, and that the embedded session_key
 * matches `expected_session_key` (rejecting cross-talk between sessions
 * that share a UDP socket). On REND_DELIVER_PEER writes the peer's IPv4
 * address as a NUL-terminated dotted-quad string into `out_peer_ip`
 * (which must be at least 64 bytes) and the peer's public port (host
 * order) into `*out_peer_port`. On REND_DELIVER_EMPTY (server's
 * "peer not yet registered" 0.0.0.0:0 sentinel) and on
 * REND_DELIVER_MALFORMED, `out_peer_ip` is set to "" and
 * `*out_peer_port` to 0.
 */
RendezvousDeliverResult Rendezvous_ParseDeliverEx(const uint8_t* pkt, int len,
                                                  const uint8_t expected_session_key[16],
                                                  char out_peer_ip[64],
                                                  uint16_t* out_peer_port);

/*
 * Legacy bool wrapper around Rendezvous_ParseDeliverEx: returns true
 * only for REND_DELIVER_PEER. Kept for call sites and tests that only
 * care about "did the server hand me a live peer endpoint".
 */
bool Rendezvous_ParseDeliver(const uint8_t* pkt, int len,
                             const uint8_t expected_session_key[16],
                             char out_peer_ip[64],
                             uint16_t* out_peer_port);

/* Server->client frame types this client understands. REGISTER (1) and
 * POLL (3) are client->server.
 *
 * Types 6-8 (RELAY_GRANT / RELAY_PIN / RELAY_PIN_ACK) were the S5 relay
 * rung and are GONE from this client. The '3SXR' wire version
 * deliberately stays 2: a client that never sends a RELAY_REQ was always
 * indistinguishable from a pre-S5 one, so ceasing to send it is not a
 * protocol change and no interlock row in §6.5 moves.
 *
 * Task #122: type 5 (once RELAY_REQ) is re-allocated as NACK — a typed
 * server refusal. Version still stays 2 for the same reason: every frame
 * that existed before is byte-for-byte unchanged, and a build that does
 * not know type 5 ignores it exactly as it ignored the relay types. */
#define REND_FRAME_DELIVER      2
#define REND_FRAME_CHALLENGE    4
#define REND_FRAME_NACK         5

/* Task #122: NACK wire length. 28 bytes, deliberately SMALLER than the
 * 36-byte REGISTER/POLL it answers — the server must stay a net
 * attenuator on every frame so it is never worth aiming at a victim.
 * See the amplification note in tools/rendezvous-server/
 * rendezvous-server.js. */
#define REND_NACK_LEN 28

/*
 * Task #122 — why this enum exists.
 *
 * The rendezvous server refuses a request for NINE distinguishable
 * reasons and, before #122, EVERY ONE of them looked identical on the
 * wire: nothing at all. The client's own honesty note (connect_fail.h,
 * CONNECT_FAIL_RENDEZVOUS_DOWN) lists four of them and says outright
 * that separating them "needs a wire change — S4/S5 territory". This is
 * that wire change; CHALLENGE used to be the only server condition a
 * client could positively observe.
 *
 * WIRE VALUES ARE APPEND-ONLY. They are shared with
 * tools/rendezvous-server/rendezvous-server.js (NACK_REASON_*) and the
 * two files DEPLOY INDEPENDENTLY — the server is a long-lived VPS
 * process, the client ships in a release ZIP — so a renumber on either
 * side is a silent misattribution, which is the exact failure H-1 was
 * about. Never renumber, never reuse; 0 is reserved so an all-zero byte
 * is never a valid reason.
 */
/* The "pre"/"post" column is the return-routability class: a PRE-cookie
 * reason describes only the sender's own frame or its own per-IP budget
 * and carries no session-derived information, which is what makes it
 * safe to send to a source that has not yet proven it receives at its
 * claimed address. A POST-cookie reason is only ever sent to a source
 * that has proven exactly that. */
typedef enum RendezvousNackReason {
    REND_NACK_NONE         = 0, /*      not a reason; reserved              */
    REND_NACK_BAD_VERSION  = 1, /* pre  server speaks a different version   */
    REND_NACK_BAD_LENGTH   = 2, /* pre  REGISTER/POLL was the wrong length  */
    REND_NACK_BAD_TYPE     = 3, /* pre  unallocated type byte — e.g. a
                                 *      relay-era build sending types 6-8   */
    REND_NACK_RATE_IP      = 4, /* post our IP's cookied budget is spent —
                                 *      typically a shared/CGNAT egress IP  */
    REND_NACK_RATE_KEY     = 5, /* post this room's budget is spent         */
    REND_NACK_RATE_PREGATE = 6, /* pre  our IP's uncookied first-contact
                                 *      budget is spent                     */
    REND_NACK_KEY_QUOTA    = 7, /* post our IP already holds the maximum
                                 *      live keys — reachable by retrying
                                 *      several stale room codes            */
    REND_NACK_TABLE_FULL   = 8, /* post server session table saturated      */
    REND_NACK_SESSION_FULL = 9  /* post the room code is already taken by
                                 *      two other endpoints                 */
} RendezvousNackReason;

/*
 * Parse a server NACK. Returns true and writes *out_reason only for a
 * well-formed NACK of THIS protocol version carrying OUR session key.
 *
 * The session-key gate is the same one Rendezvous_ParseChallenge uses
 * and it is load-bearing for the same reason: the key embeds the S4b
 * nonce, so an off-path attacker that learns our port cannot forge a
 * NACK we would believe. A NACK is diagnostic input, and a diagnosis an
 * attacker can choose is worse than no diagnosis (H-1).
 *
 * The reason byte is NOT range-checked against the enum above: a server
 * newer than us may send a reason we have no name for, and the honest
 * handling is to surface the raw number rather than silently drop the
 * frame or, worse, coerce it onto a name that is wrong.
 * Rendezvous_NackReasonText renders those as "unknown".
 */
bool Rendezvous_ParseNack(const uint8_t* pkt, int len,
                          const uint8_t expected_session_key[16],
                          uint8_t* out_reason);

/* Stable machine string for logs, e.g. "SESSION_FULL". Never NULL;
 * unrecognised values render as "unknown". Log-grep anchors: append-only,
 * never rename. */
const char* Rendezvous_NackReasonText(uint8_t reason);

/*
 * Cheap router for an inbound datagram: returns the '3SXR' frame type
 * byte when `pkt` is a well-formed frame of THIS protocol version, or 0
 * otherwise (wrong magic, wrong version, or too short to carry a type).
 * Receive loops use this instead of re-deriving the magic/version test —
 * before this existed every call site open-coded
 * `buf[0]==0x33 && ... && buf[5]==N`.
 */
int Rendezvous_FrameType(const uint8_t* pkt, int len);

/*
 * True when `pkt` carries the '3SXR' magic, REGARDLESS of version.
 *
 * This is the straggler test, and it is deliberately weaker than
 * Rendezvous_FrameType (review LOW-1). The GekkoNet straggler drop in
 * sdl_net_adapter.c used to test `Rendezvous_FrameType(...) != 0`, which
 * returns 0 for ANY version != REND_VERSION — so a non-v2 '3SXR' frame
 * would have passed straight through with data[0] == 0x33 and been
 * miscounted as an InputAck (0x33 & 7 == 3) before reaching GekkoNet as
 * a packet of type 51, which is exactly the bug that guard exists to
 * close. Unreachable today, because this build only ever emits v2 —
 * but the guard's comment claimed "every '3SXR' frame", and now that is
 * true. The magic alone is still an EXACT test rather than a heuristic:
 * a GekkoNet type byte is 1..7 by construction and can never be 0x33.
 */
bool Rendezvous_HasMagic(const uint8_t* pkt, int len);

/*
 * The '3SXR' wire version THIS build speaks — i.e. the exact byte
 * Rendezvous_FrameType compares pkt[4] against.
 *
 * #36: a receive loop that wants to tell "version skew" apart from
 * "runt" cannot do it from Rendezvous_FrameType's return value alone —
 * FrameType returns 0 for len < 6, wrong magic AND wrong version alike,
 * so a 4- or 5-byte magic-matched runt is indistinguishable from a
 * genuinely version-skewed peer. Discriminating needs the version byte,
 * and the caller must compare against the SAME constant FrameType uses
 * or the two can drift apart silently. Hence an accessor rather than a
 * second copy of the literal at each call site.
 */
int Rendezvous_WireVersion(void);

/*
 * Parse a `udp://host:port` configuration URL into its host and port
 * components. Hostname may be a dotted-quad or a DNS name; IPv6
 * literals are rejected (per plan §Decision 2 — IPv6 out of scope).
 * Whitespace and control characters in the host are rejected. Port
 * must parse as a strict decimal integer in [1, 65535].
 *
 * Writes the host into `out_host` (must be at least 64 bytes) NUL-
 * terminated, and the port (host order) into `*out_port`. On any
 * malformed input returns false and zeroes the outputs.
 */
bool Rendezvous_ParseSignalUrl(const char* url,
                               char out_host[64],
                               uint16_t* out_port);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_RENDEZVOUS_H */
