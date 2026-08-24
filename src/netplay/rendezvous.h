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
 * friend-to-friend model. S5's relay will reuse this token.
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

/* --- S5 relay (docs/plan-netplay-connection.md §7) ---------------------
 *
 * Symmetric x symmetric is the one NAT pair that cannot punch: neither
 * side can predict the other's per-destination port. The relay is the
 * only fix. It is a minimal extension of THIS protocol on the SAME UDP
 * port rather than TURN, because do_handoff transfers a BARE datagram
 * socket into netplay.c and GekkoNet then owns plain send/recv on it —
 * TURN framing on every packet, on a 60 Hz rollback game's hot path, is
 * not something GekkoNet can speak. The relay forwards raw datagrams,
 * so a relayed socket behaves exactly like a punched one.
 *
 * The '3SXR' WIRE VERSION IS UNCHANGED at 2: a client that never sends a
 * RELAY_REQ is indistinguishable from a pre-S5 one, so this is a pure
 * extension and no interlock row in §6.5 changes.
 *
 * Flow: RELAY_REQ (main port) -> RELAY_GRANT carries a per-session relay
 * PORT and a per-side TOKEN -> the client sends RELAY_PIN datagrams to
 * (signal host, relay port) until it sees a RELAY_PIN_ACK -> the relay
 * has both endpoints pinned and forwards raw bytes between them.
 */

/* Server->client frame types this client understands. REGISTER (1) and
 * POLL (3) are client->server; RELAY_PIN (7) is client->relay. */
#define REND_FRAME_DELIVER      2
#define REND_FRAME_CHALLENGE    4
#define REND_FRAME_RELAY_GRANT  6
#define REND_FRAME_RELAY_PIN_ACK 8

/*
 * Cheap router for an inbound datagram: returns the '3SXR' frame type
 * byte when `pkt` is a well-formed frame of THIS protocol version, or 0
 * otherwise (wrong magic, wrong version, or too short to carry a type).
 * Receive loops use this instead of re-deriving the magic/version test —
 * before S5 every call site open-coded `buf[0]==0x33 && ... && buf[5]==N`.
 */
int Rendezvous_FrameType(const uint8_t* pkt, int len);

#define REND_RELAY_TOKEN_LEN   8
/* RELAY_REQ is byte-identical to REGISTER apart from the type byte —
 * deliberately, so it passes through the server's return-routability
 * gate (which reads the key at [8..24) and the cookie at [28..36))
 * without a second code path. Same 36-byte length. */
#define REND_RELAY_REQ_PKT_LEN REND_REGISTER_PKT_LEN
#define REND_RELAY_GRANT_LEN   36
#define REND_RELAY_PIN_LEN     20
#define REND_RELAY_PIN_ACK_LEN 12

/* Slot value the server puts in a REFUSED grant. */
#define REND_RELAY_SLOT_NONE   0xFFu

typedef enum RendezvousRelayStatus {
    REND_RELAY_GRANTED = 0,
    REND_RELAY_POOL_EXHAUSTED = 1, /* every relay port is in use          */
    REND_RELAY_NOT_PAIRED = 2,     /* the peer never registered — nothing
                                      to relay to                         */
} RendezvousRelayStatus;

typedef struct RendezvousRelayGrant {
    uint8_t  slot;    /* 0 = slot A (host), 1 = slot B (joiner);
                         REND_RELAY_SLOT_NONE on refusal                  */
    uint8_t  status;  /* RendezvousRelayStatus                            */
    uint16_t relay_port;                       /* host order; 0 on refusal */
    uint8_t  token[REND_RELAY_TOKEN_LEN];      /* zeros on refusal         */
} RendezvousRelayGrant;

/*
 * Build a 36-byte RELAY_REQ (type=5). Same arguments and same wire shape
 * as Rendezvous_BuildRegister — including the cookie tail, which the
 * server's gate requires exactly as it does for a REGISTER.
 *
 * Returns true on success, false on NULL key/buffer.
 */
bool Rendezvous_BuildRelayReq(uint16_t my_public_port,
                              const uint8_t session_key[16],
                              const uint8_t cookie[REND_COOKIE_LEN],
                              uint8_t out_pkt[REND_RELAY_REQ_PKT_LEN]);

/*
 * Parse a 36-byte RELAY_GRANT (type=6, server->client). Layout:
 * magic(4) ver(1) type(1) slot(1) status(1) key(16) port_be(2)
 * reserved(2) token(8).
 *
 * Validates magic/version/type and that the frame carries OUR session
 * key (the same cross-talk + forgery gate Rendezvous_ParseChallenge
 * applies — the key embeds the room-code nonce, which an off-path
 * attacker cannot know). A GRANTED status carrying port 0 is rejected as
 * malformed: fail closed rather than hand the caller an unconnectable
 * endpoint. `*out` is ZEROED on every reject, so a caller that ignores
 * the return value cannot act on attacker-chosen bytes.
 *
 * A REFUSED grant (status != REND_RELAY_GRANTED) returns TRUE with the
 * status set — a refusal is a valid, informative frame and the caller
 * must be able to distinguish it from silence.
 */
bool Rendezvous_ParseRelayGrant(const uint8_t* pkt, int len,
                                const uint8_t expected_session_key[16],
                                RendezvousRelayGrant* out);

/*
 * Build a 20-byte RELAY_PIN (type=7) for the per-session relay port:
 * magic(4) ver(1) type(1) slot(1) reserved(1) token(8) reserved2(4).
 *
 * The relay pins the first token-bearing source on each side and
 * forwards raw datagrams between the two thereafter. `slot` and `token`
 * both come from the RELAY_GRANT; the slot index is inside the server's
 * token HMAC, so a token cannot be presented for the other side.
 */
bool Rendezvous_BuildRelayPin(uint8_t slot,
                              const uint8_t token[REND_RELAY_TOKEN_LEN],
                              uint8_t out_pkt[REND_RELAY_PIN_LEN]);

/*
 * Parse a 12-byte RELAY_PIN_ACK (type=8, relay->client):
 * magic(4) ver(1) type(1) slot(1) peer_pinned(1) reserved(4).
 *
 * Validates magic/version/type and that the slot echo matches
 * `expect_slot`. Writes whether the OTHER side is pinned yet into
 * `*out_peer_pinned` (false on any reject).
 *
 * Deliberately NOT session-key-authenticated: the relay port itself is
 * the session identifier and the ACK steers nothing — it only tells the
 * client "your pin landed" and "your peer has/hasn't arrived". Forging
 * one requires knowing the ephemeral relay port, and the only thing it
 * buys is making a client hand off a few hundred ms early — a state
 * GekkoNet's own retransmit/timeout already covers.
 */
bool Rendezvous_ParseRelayPinAck(const uint8_t* pkt, int len,
                                 uint8_t expect_slot,
                                 bool* out_peer_pinned);

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
