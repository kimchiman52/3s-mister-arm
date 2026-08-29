/*
 * rendezvous.c — Step 3 of docs/plan-bilateral-hole-punch.md.
 *
 * Pure rendezvous-protocol client. No SDL_net dependencies. Wire format
 * per docs/plan-bilateral-hole-punch.md §Decision 2.
 */

#include "netplay/rendezvous.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>      /* unused at present, kept for symmetry / future logging */
#include <stdlib.h>     /* strtoul */
#include <string.h>     /* memcpy / memset / memcmp / strchr / strncmp */

#include <arpa/inet.h>  /* inet_ntop */

#include "utils/sha256.h"

/* '3SXR' big-endian on the wire. */
#define REND_MAGIC            0x33535852u
/* S4c: protocol v2 — REGISTER/POLL grew an 8-byte return-routability
 * cookie tail (36 bytes, was 28) and the server answers uncookied
 * requests with a CHALLENGE (type 4) instead of binding state. The
 * version bump is deliberate and breaking: a v1 client is cleanly
 * dropped (and logged) by the v2 server; see the interlock note in
 * docs/plan-netplay-connection.md §6. */
#define REND_VERSION          2
#define REND_TYPE_REGISTER    1
#define REND_TYPE_DELIVER     2
#define REND_TYPE_POLL        3
#define REND_TYPE_CHALLENGE   4
/* Types 5-8 were the S5 relay rung (RELAY_REQ / RELAY_GRANT / RELAY_PIN
 * / RELAY_PIN_ACK). The rung was removed; this client neither sends nor
 * parses them. The version byte stays 2 — see rendezvous.h. */

#define REND_REGISTER_LEN     36
#define REND_POLL_LEN         36
#define REND_DELIVER_MIN_LEN  32
#define REND_CHALLENGE_LEN    32

#define REND_KEY_LEN          16

/* The 10-byte canonical serialization of the v3 room-code payload that
 * both peers hash to derive the session key and the punch token:
 * 4 IPv4 octets in network byte order, the public port big-endian,
 * then the 32-bit nonce big-endian. This is byte-for-byte the same
 * bitstream room_code.c cuts its 16 base-32 payload chars out of
 * (room_code_pack_payload) — one canonical byte order for the code and
 * the hash input. */
#define REND_KEY_PAYLOAD_LEN  10

/* Big-endian byte-stream helpers — explicit reads/writes avoid any
 * struct-cast / alignment / host-endian dependency. */
static void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static void write_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
            (uint32_t)p[3];
}

static uint16_t read_be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Shared derivation core for the session key and the S4a punch token.
 * Hashes the domain-separation string followed by the 10-byte canonical
 * v3 payload (ip[4] || port_be[2] || nonce_be[4]), then copies the
 * first out_len digest bytes out. Zeroes the output and returns false
 * on any failure. */
static bool rend_derive(const char* domain,
                        uint32_t ip_be, uint16_t public_port, uint32_t nonce,
                        uint8_t* out, size_t out_len) {
    if (!out) {
        return false;
    }
    /* v3: every uint32_t nonce is in range, so ip_be == 0 (an unset /
     * invalid endpoint) is the only reject. */
    if (ip_be == 0) {
        memset(out, 0, out_len);
        return false;
    }

    /* Match room_code.c: ip_be is a uint32_t whose in-memory bytes are
     * the IPv4 octets in network byte order (as produced by inet_pton
     * into `struct in_addr.s_addr`). memcpy reads those four bytes in
     * order, byte-for-byte, regardless of host endianness. Port and
     * nonce are serialized big-endian. */
    uint8_t payload[REND_KEY_PAYLOAD_LEN];
    memcpy(&payload[0], &ip_be, 4);
    payload[4] = (uint8_t)((public_port >> 8) & 0xFFu);
    payload[5] = (uint8_t)(public_port & 0xFFu);
    payload[6] = (uint8_t)((nonce >> 24) & 0xFFu);
    payload[7] = (uint8_t)((nonce >> 16) & 0xFFu);
    payload[8] = (uint8_t)((nonce >> 8) & 0xFFu);
    payload[9] = (uint8_t)(nonce & 0xFFu);

    sha256 sha;
    if (!sha256_init(&sha)) {
        memset(out, 0, out_len);
        return false;
    }
    if (!sha256_append(&sha, domain, strlen(domain))) {
        memset(out, 0, out_len);
        return false;
    }
    if (!sha256_append(&sha, payload, sizeof(payload))) {
        memset(out, 0, out_len);
        return false;
    }
    uint8_t digest[SHA256_BYTES_SIZE];
    if (!sha256_finalize_bytes(&sha, digest)) {
        memset(out, 0, out_len);
        return false;
    }
    memcpy(out, digest, out_len);
    return true;
}

bool Rendezvous_DeriveSessionKey(uint32_t ip_be,
                                 uint16_t public_port,
                                 uint32_t nonce,
                                 uint8_t out_key[16]) {
    /* Domain-separated v3 derivation (see rendezvous.h). BREAKING vs
     * the v2 "3SXR-SK2"/12-bit-nonce hash — shipped with the v3
     * room-code format. */
    return rend_derive("3SXR-SK3", ip_be, public_port, nonce,
                       out_key, REND_KEY_LEN);
}

bool Rendezvous_DerivePunchToken(uint32_t ip_be,
                                 uint16_t public_port,
                                 uint32_t nonce,
                                 uint8_t out_token[REND_PUNCH_TOKEN_LEN]) {
    return rend_derive("3SXR-PT3", ip_be, public_port, nonce,
                       out_token, REND_PUNCH_TOKEN_LEN);
}

bool Rendezvous_BuildRegister(uint16_t my_public_port,
                              const uint8_t session_key[16],
                              const uint8_t cookie[REND_COOKIE_LEN],
                              uint8_t out_pkt[REND_REGISTER_PKT_LEN]) {
    if (!session_key || !out_pkt) {
        return false;
    }
    memset(out_pkt, 0, REND_REGISTER_LEN);
    write_be32(&out_pkt[0], REND_MAGIC);
    out_pkt[4] = (uint8_t)REND_VERSION;
    out_pkt[5] = (uint8_t)REND_TYPE_REGISTER;
    /* reserved [6..7] = 0 (already zeroed by memset) */
    memcpy(&out_pkt[8], session_key, REND_KEY_LEN);
    write_be16(&out_pkt[24], my_public_port);
    /* reserved2 [26..27] = 0 (already zeroed by memset) */
    /* S4c cookie tail [28..35]: all-zero (memset) = "no cookie yet" —
     * the server answers with a CHALLENGE instead of binding a slot. */
    if (cookie != NULL) {
        memcpy(&out_pkt[28], cookie, REND_COOKIE_LEN);
    }
    return true;
}

bool Rendezvous_BuildPoll(const uint8_t session_key[16],
                          const uint8_t cookie[REND_COOKIE_LEN],
                          uint8_t out_pkt[REND_REGISTER_PKT_LEN]) {
    if (!session_key || !out_pkt) {
        return false;
    }
    memset(out_pkt, 0, REND_POLL_LEN);
    write_be32(&out_pkt[0], REND_MAGIC);
    out_pkt[4] = (uint8_t)REND_VERSION;
    out_pkt[5] = (uint8_t)REND_TYPE_POLL;
    /* reserved [6..7] = 0 */
    memcpy(&out_pkt[8], session_key, REND_KEY_LEN);
    /* port field [24..25] unused for POLL; reserved [26..27] = 0 */
    if (cookie != NULL) {
        memcpy(&out_pkt[28], cookie, REND_COOKIE_LEN);
    }
    return true;
}

bool Rendezvous_ParseChallenge(const uint8_t* pkt, int len,
                               const uint8_t expected_session_key[16],
                               uint8_t out_cookie[REND_COOKIE_LEN]) {
    if (out_cookie) {
        memset(out_cookie, 0, REND_COOKIE_LEN);
    }
    if (!pkt || len < REND_CHALLENGE_LEN || !expected_session_key || !out_cookie) {
        return false;
    }
    if (read_be32(&pkt[0]) != REND_MAGIC) {
        return false;
    }
    if (pkt[4] != REND_VERSION) {
        return false;
    }
    if (pkt[5] != REND_TYPE_CHALLENGE) {
        return false;
    }
    /* Session-key match: a CHALLENGE not carrying OUR key is cross-talk
     * or a forgery — an off-path attacker cannot know the key (it
     * embeds the S4b nonce), so this also authenticates the challenge
     * source as "someone on the path of our own REGISTER". */
    if (memcmp(&pkt[8], expected_session_key, REND_KEY_LEN) != 0) {
        return false;
    }
    memcpy(out_cookie, &pkt[24], REND_COOKIE_LEN);
    return true;
}

RendezvousDeliverResult Rendezvous_ParseDeliverEx(const uint8_t* pkt, int len,
                                                  const uint8_t expected_session_key[16],
                                                  char out_peer_ip[64],
                                                  uint16_t* out_peer_port) {
    if (out_peer_ip) {
        out_peer_ip[0] = '\0';
    }
    if (out_peer_port) {
        *out_peer_port = 0;
    }
    if (!pkt || len < REND_DELIVER_MIN_LEN || !expected_session_key ||
        !out_peer_ip || !out_peer_port) {
        return REND_DELIVER_MALFORMED;
    }
    if (read_be32(&pkt[0]) != REND_MAGIC) {
        return REND_DELIVER_MALFORMED;
    }
    if (pkt[4] != REND_VERSION) {
        return REND_DELIVER_MALFORMED;
    }
    if (pkt[5] != REND_TYPE_DELIVER) {
        return REND_DELIVER_MALFORMED;
    }
    if (memcmp(&pkt[8], expected_session_key, REND_KEY_LEN) != 0) {
        return REND_DELIVER_MALFORMED;
    }

    /* peer_ip raw 4 bytes at pkt[24..27], peer port BE at pkt[28..29]. */
    if (!inet_ntop(AF_INET, &pkt[24], out_peer_ip, 64)) {
        out_peer_ip[0] = '\0';
        return REND_DELIVER_MALFORMED;
    }
    *out_peer_port = read_be16(&pkt[28]);

    /* Server's "peer not yet registered" sentinel: 0.0.0.0:0. The plan
     * (§Decision 2 step 2) explicitly uses zeros to mean "the other side
     * hasn't registered yet — keep polling". S3: this is a distinct,
     * MEANINGFUL result — a zero-sentinel DELIVER proves the server is
     * alive and reachable, which the failure taxonomy needs to separate
     * "rendezvous down" from "host offline". Clear the outputs so no
     * caller can mistake the sentinel for an endpoint. */
    if (strcmp(out_peer_ip, "0.0.0.0") == 0 && *out_peer_port == 0) {
        out_peer_ip[0] = '\0';
        *out_peer_port = 0;
        return REND_DELIVER_EMPTY;
    }
    return REND_DELIVER_PEER;
}

bool Rendezvous_ParseDeliver(const uint8_t* pkt, int len,
                             const uint8_t expected_session_key[16],
                             char out_peer_ip[64],
                             uint16_t* out_peer_port) {
    return Rendezvous_ParseDeliverEx(pkt, len, expected_session_key,
                                     out_peer_ip, out_peer_port) == REND_DELIVER_PEER;
}

/* --- inbound frame routing --------------------------------------------- */

bool Rendezvous_HasMagic(const uint8_t* pkt, int len) {
    /* Magic ONLY — no version, no type. See the header for why the
     * straggler drop must not be version-gated (review LOW-1). */
    if (!pkt || len < 4) {
        return false;
    }
    return read_be32(&pkt[0]) == REND_MAGIC;
}

int Rendezvous_FrameType(const uint8_t* pkt, int len) {
    if (!pkt || len < 6) {
        return 0;
    }
    if (read_be32(&pkt[0]) != REND_MAGIC) {
        return 0;
    }
    if (pkt[4] != REND_VERSION) {
        return 0;
    }
    return (int)pkt[5];
}

bool Rendezvous_ParseSignalUrl(const char* url,
                               char out_host[64],
                               uint16_t* out_port) {
    if (!url || !out_host || !out_port) {
        return false;
    }
    out_host[0] = '\0';
    *out_port = 0;

    static const char SCHEME[] = "udp://";
    const size_t SCHEME_LEN = sizeof(SCHEME) - 1;
    if (strncmp(url, SCHEME, SCHEME_LEN) != 0) {
        return false;
    }

    const char* rest = url + SCHEME_LEN;

    /* Reject IPv6 literal (per plan §Decision 2 — IPv6 out of scope). */
    if (rest[0] == '[') {
        return false;
    }

    /* Find the host/port separator. Reject empty host. */
    const char* colon = strchr(rest, ':');
    if (!colon || colon == rest) {
        return false;
    }

    const size_t hostlen = (size_t)(colon - rest);
    if (hostlen >= 64) {
        return false;
    }

    /* Reject whitespace / control chars in host. */
    for (size_t i = 0; i < hostlen; ++i) {
        const unsigned char c = (unsigned char)rest[i];
        if (c <= 0x20 || c == 0x7F) {
            return false;
        }
    }

    memcpy(out_host, rest, hostlen);
    out_host[hostlen] = '\0';

    /* Strict decimal port parse. */
    const char* port_str = colon + 1;
    if (*port_str == '\0') {
        out_host[0] = '\0';
        return false;
    }

    char* endp = NULL;
    const unsigned long p = strtoul(port_str, &endp, 10);
    if (endp == port_str || *endp != '\0' || p < 1 || p > 65535) {
        out_host[0] = '\0';
        return false;
    }

    *out_port = (uint16_t)p;
    return true;
}
