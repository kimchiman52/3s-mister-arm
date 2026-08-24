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
 * Security note (v2, S4b): the session key is
 * SHA-256("3SXR-SK2" || ip[4] || port_be[2] || nonce_be[2])[0..15] —
 * the byte-aligned serialization of the v2 room-code payload including
 * the 12-bit CSPRNG nonce, domain-separated from the S4a punch token
 * ("3SXR-PT2"). It is derived (not transmitted) and not user-typeable.
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
 * tuple + nonce that both peers share via the room code (v2, S4b).
 *
 * `ip_be` follows the same convention as room_code.c: the uint32_t's
 * in-memory bytes are the four IPv4 octets in network byte order (i.e.
 * the value returned by inet_pton(AF_INET, ...) into `s_addr`).
 * `nonce` is the 12-bit room-code nonce (host order, <= 0x0FFF).
 *
 * The derivation is domain-separated and covers the full v2 payload:
 *
 *   key = SHA-256("3SXR-SK2" || ip[4] || port_be[2] || nonce_be[2])[0..15]
 *
 * where nonce_be is the nonce as a 16-bit big-endian value (top 4 bits
 * zero). Because the 12-bit CSPRNG nonce is inside the hash, an
 * attacker who merely knows/guesses (ip, port) can no longer derive
 * the session key to squat or race the rendezvous slot (S4b) — only
 * someone holding the actual room code can. BREAKING change from the
 * v1 derivation (SHA-256 over the bare 6-byte payload), shipped
 * together with the v2 room-code format to the whole alpha group.
 *
 * Writes 16 bytes into `out_key`. Returns true on success. On failure
 * (ip_be == 0, indicating an unset / invalid endpoint), zeroes
 * `out_key` and returns false.
 */
bool Rendezvous_DeriveSessionKey(uint32_t ip_be,
                                 uint16_t public_port,
                                 uint16_t nonce,
                                 uint8_t out_key[16]);

/*
 * S4a (docs/plan-netplay-connection.md §6): derive the 8-byte punch
 * authentication token both peers append to the "3SX_PUNCH" hole-punch
 * payload. Same inputs and payload serialization as
 * Rendezvous_DeriveSessionKey, different domain string:
 *
 *   token = SHA-256("3SXR-PT2" || ip[4] || port_be[2] || nonce_be[2])[0..7]
 *
 * so the two derivations can never collide. The token proves knowledge
 * of the room-code payload INCLUDING the nonce: the host only accepts
 * a punch carrying it, so a blind scanner — or an attacker who
 * observed the host's ip:port but not the code — can no longer be
 * captured as "the peer". Anyone who has the full room code can of
 * course derive it — the room code IS the shared secret in this
 * friend-to-friend model. S5's relay will reuse this token.
 *
 * Writes 8 bytes into out_token. Returns true on success; on failure
 * (ip_be == 0 or hash failure) zeroes out_token and returns false.
 */
#define REND_PUNCH_TOKEN_LEN 8
bool Rendezvous_DerivePunchToken(uint32_t ip_be,
                                 uint16_t public_port,
                                 uint16_t nonce,
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
