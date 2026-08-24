/**
 * @file stun.c
 * @brief Minimal STUN client (RFC 5389) and endpoint encoder/decoder.
 *
 * Performs a STUN Binding Request to discover the public IP:port,
 * and provides 8-character Base64-like encoding for sharing endpoints.
 */
#ifndef _WIN32
#define _GNU_SOURCE // Must be before any includes for getaddrinfo/timeval
#endif
#include "netplay/stun.h"
#include "netplay/net_tuning.h"
#include "utils/csprng.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3_net/SDL_net.h>

// Platform headers for inet_ntop
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif

// STUN message types (RFC 5389)
#define STUN_BINDING_REQUEST 0x0001
#define STUN_BINDING_RESPONSE 0x0101
#define STUN_MAGIC_COOKIE 0x2112A442

// STUN attribute types
#define STUN_ATTR_MAPPED_ADDRESS 0x0001
#define STUN_ATTR_XOR_MAPPED_ADDRESS 0x0020

void Stun_EncodeEndpoint(const char* ip, uint16_t port, uint16_t local_port, char* out_code) {
    if (!ip || !out_code)
        return;
    snprintf(out_code, 64, "%s|%u|%u", ip, port, local_port);
}

bool Stun_DecodeEndpoint(const char* code, char* out_ip, uint16_t* out_port, uint16_t* out_local_port) {
    if (!code || !out_ip || !out_port)
        return false;

    char temp[64];
    SDL_strlcpy(temp, code, sizeof(temp));

    char* sep = strchr(temp, '|');
    if (!sep)
        return false;

    *sep = '\0';
    SDL_strlcpy(out_ip, temp, 64);
    *out_port = (uint16_t)atoi(sep + 1);

    if (out_local_port) {
        char* sep2 = strchr(sep + 1, '|');
        if (sep2) {
            *out_local_port = (uint16_t)atoi(sep2 + 1);
        } else {
            *out_local_port = *out_port; // default to public port if not present
        }
    }

    return true;
}

// Build a 20-byte STUN Binding Request (RFC 5389 §6).
// Returns false when the platform CSPRNG is unavailable — see the
// transaction-ID comment below for why that is fatal rather than
// degradable.
static bool build_binding_request(uint8_t* buf, uint8_t* transaction_id) {
    // Type: Binding Request (0x0001)
    buf[0] = 0x00;
    buf[1] = 0x01;
    // Length: 0 (no attributes)
    buf[2] = 0x00;
    buf[3] = 0x00;
    // Magic Cookie
    buf[4] = 0x21;
    buf[5] = 0x12;
    buf[6] = 0xA4;
    buf[7] = 0x42;
    /* Transaction ID: 12 CSPRNG bytes (S4a). The previous SDL_rand
     * source was NOT "unseeded" — SDL 3.4.4 auto-seeds from the
     * performance counter — but it IS an LCG whose internal state is
     * recoverable from observed outputs, making every future
     * transaction ID predictable to anyone who saw a few. RFC 5389 §6
     * requires txids to be "cryptographically random": a predictable
     * txid lets an off-path attacker forge Binding Responses (feeding
     * us a wrong mapped endpoint, i.e. a dead room code, or steering
     * the S1 rebind-drift path).
     *
     * S4-review L-3 — CSPRNG failure policy is now CONSISTENT AND LOUD.
     * This used to degrade silently to SDL_rand behind a one-time
     * SDL_LogWarn while RoomCode_GenerateNonce hard-failed on the same
     * condition. On a device with no /dev/urandom that split the two
     * roles: the HOST aborted hosting outright, while the JOINER
     * proceeded with predictable txids — reintroducing the exact
     * RFC 5389 §6 response-forgery weakness S4a set out to remove, on
     * the side least able to notice. There is no "slightly weaker
     * netplay" worth having here: fail closed on both sides and say so
     * at ERROR level.
     *
     * (The old one-shot `static bool warned` was also a plain non-atomic
     * bool read/written from the discover worker AND the main-thread
     * keepalive path. It is gone; the CAS below is the same pattern
     * direct_p2p.c's rendezvous-queue drop log uses.) */
    if (!Csprng_Bytes(transaction_id, 12)) {
        static SDL_AtomicInt logged = { 0 };
        if (SDL_CompareAndSwapAtomicInt(&logged, 0, 1)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "STUN: platform CSPRNG unavailable — refusing to send a "
                         "Binding Request with a predictable transaction ID "
                         "(RFC 5389 §6 requires cryptographic randomness; a "
                         "guessable txid permits off-path response forgery). "
                         "Netplay cannot start on this device.");
        }
        memset(transaction_id, 0, 12);
        return false;
    }
    memcpy(&buf[8], transaction_id, 12);
    return true;
}

/* --- S4a authenticated punch payload ----------------------------------- */

static const char k_punch_prefix[STUN_PUNCH_PREFIX_LEN] = { '3', 'S', 'X', '_',
                                                            'P', 'U', 'N', 'C', 'H' };

void Stun_BuildPunchPayload(const uint8_t token[STUN_PUNCH_TOKEN_LEN],
                            uint8_t out[STUN_PUNCH_PAYLOAD_LEN]) {
    memcpy(out, k_punch_prefix, STUN_PUNCH_PREFIX_LEN);
    memcpy(out + STUN_PUNCH_PREFIX_LEN, token, STUN_PUNCH_TOKEN_LEN);
}

bool Stun_HasPunchPrefix(const uint8_t* buf, int len) {
    return buf != NULL && len >= STUN_PUNCH_PREFIX_LEN &&
           memcmp(buf, k_punch_prefix, STUN_PUNCH_PREFIX_LEN) == 0;
}

bool Stun_IsPunchPayload(const uint8_t* buf, int len,
                         const uint8_t token[STUN_PUNCH_TOKEN_LEN]) {
    if (buf == NULL || token == NULL || len != STUN_PUNCH_PAYLOAD_LEN) {
        return false;
    }
    if (memcmp(buf, k_punch_prefix, STUN_PUNCH_PREFIX_LEN) != 0) {
        return false;
    }
    /* Constant-time token compare: a UDP responder that early-exits on
     * the first mismatching byte is a (weak, but free-to-close) timing
     * oracle for guessing the token byte-by-byte. */
    uint8_t diff = 0;
    for (int i = 0; i < STUN_PUNCH_TOKEN_LEN; i++) {
        diff |= (uint8_t)(buf[STUN_PUNCH_PREFIX_LEN + i] ^ token[i]);
    }
    return diff == 0;
}

// Parse STUN Binding Response for XOR-MAPPED-ADDRESS or MAPPED-ADDRESS
static bool parse_binding_response(const uint8_t* buf, int len, const uint8_t* transaction_id, char* out_ip,
                                   int ip_buf_size, uint16_t* out_port) {
    if (len < 20)
        return false;

    // Check message type = Binding Success Response
    uint16_t msg_type = ((uint16_t)buf[0] << 8) | buf[1];
    if (msg_type != STUN_BINDING_RESPONSE)
        return false;

    uint16_t msg_len = ((uint16_t)buf[2] << 8) | buf[3];
    if (20 + msg_len > len)
        return false;

    // Verify magic cookie
    uint32_t cookie = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    if (cookie != STUN_MAGIC_COOKIE)
        return false;

    // Verify transaction ID
    if (memcmp(&buf[8], transaction_id, 12) != 0)
        return false;

    // Walk attributes
    int offset = 20;
    while (offset + 4 <= 20 + (int)msg_len) {
        uint16_t attr_type = ((uint16_t)buf[offset] << 8) | buf[offset + 1];
        uint16_t attr_len = ((uint16_t)buf[offset + 2] << 8) | buf[offset + 3];
        offset += 4;

        if (offset + attr_len > 20 + (int)msg_len)
            break;

        if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS && attr_len >= 8) {
            // Family at offset+1 (skip reserved byte)
            uint8_t family = buf[offset + 1];
            /* X-Port (RFC 5389 §15.2): the wire carries htons(port ^
             * (cookie >> 16)). The shift-assembly below already converts
             * the big-endian wire bytes to a NATIVE value, so XOR'ing the
             * cookie's top 16 bits yields the port directly. The old
             * SDL_Swap16BE here double-converted, byteswapping every
             * mapped port on little-endian hosts (S2 fix; regression in
             * test_stun_mock.c run_wire_test). */
            uint16_t xport = ((uint16_t)buf[offset + 2] << 8) | buf[offset + 3];
            *out_port = (uint16_t)(xport ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16));

            if (family == 0x01) { // IPv4
                uint32_t xaddr = ((uint32_t)buf[offset + 4] << 24) | ((uint32_t)buf[offset + 5] << 16) |
                                 ((uint32_t)buf[offset + 6] << 8) | buf[offset + 7];
                uint32_t decoded_ip = SDL_Swap32BE(xaddr ^ STUN_MAGIC_COOKIE);
                uint8_t* b = (uint8_t*)&decoded_ip;
                snprintf(out_ip, ip_buf_size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                return true;
            } else if (family == 0x02 && attr_len >= 20) { // IPv6
                uint8_t decoded_ipv6[16];
                // XOR first 4 bytes with magic cookie
                decoded_ipv6[0] = buf[offset + 4] ^ (uint8_t)(STUN_MAGIC_COOKIE >> 24);
                decoded_ipv6[1] = buf[offset + 5] ^ (uint8_t)(STUN_MAGIC_COOKIE >> 16);
                decoded_ipv6[2] = buf[offset + 6] ^ (uint8_t)(STUN_MAGIC_COOKIE >> 8);
                decoded_ipv6[3] = buf[offset + 7] ^ (uint8_t)(STUN_MAGIC_COOKIE);
                // XOR remaining 12 bytes with transaction ID
                for (int i = 0; i < 12; i++) {
                    decoded_ipv6[4 + i] = buf[offset + 8 + i] ^ transaction_id[i];
                }

                // Format directly via inet_ntop
#ifdef _WIN32
                // We use Windows-compatible inet_ntop mapping (requires ws2tcpip.h which is included)
                inet_ntop(AF_INET6, decoded_ipv6, out_ip, ip_buf_size);
#else
                inet_ntop(AF_INET6, decoded_ipv6, out_ip, ip_buf_size);
#endif
                return true;
            }
        }

        if (attr_type == STUN_ATTR_MAPPED_ADDRESS && attr_len >= 8) {
            uint8_t family = buf[offset + 1];
            /* Same S2 byte-order fix as X-Port above: the shift assembly
             * already yields the native port value. */
            *out_port = (uint16_t)(((uint16_t)buf[offset + 2] << 8) | buf[offset + 3]);

            if (family == 0x01) {
                uint32_t decoded_ip =
                    SDL_Swap32BE(((uint32_t)buf[offset + 4] << 24) | ((uint32_t)buf[offset + 5] << 16) |
                                 ((uint32_t)buf[offset + 6] << 8) | buf[offset + 7]);
                uint8_t* b = (uint8_t*)&decoded_ip;
                snprintf(out_ip, ip_buf_size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                return true;
            } else if (family == 0x02 && attr_len >= 20) {
#ifdef _WIN32
                inet_ntop(AF_INET6, &buf[offset + 4], out_ip, ip_buf_size);
#else
                inet_ntop(AF_INET6, &buf[offset + 4], out_ip, ip_buf_size);
#endif
                return true;
            }
        }

        // Advance to next attribute (padded to 4-byte boundary)
        offset += (attr_len + 3) & ~3;
    }

    return false;
}

/* STUN server pool. All are probed IN PARALLEL from the one socket
 * (S2, docs/plan-netplay-connection.md §4); the first parseable
 * Binding Response wins. */
typedef struct {
    const char* host;
    uint16_t port;
} StunServerEntry;

static const StunServerEntry stun_servers[] = {
    { "stun.l.google.com", 19302 },
    { "stun1.l.google.com", 19302 },
    { "stun.cloudflare.com", 3478 },
    { "stun.nextcloud.com", 443 },
};
#define STUN_SERVER_COUNT (int)(sizeof(stun_servers) / sizeof(stun_servers[0]))

/* Numeric-IP fallbacks: used ONLY when DNS has produced nothing by
 * STUN_DNS_FALLBACK_MS — a blackholed resolver must not consume the
 * discovery budget. Best-effort snapshot (dig, 2026-08-23); Cloudflare
 * is anycast so its address is the most durable of the three. When DNS
 * works these are never contacted. */
static const StunServerEntry stun_fallback_servers[] = {
    { "74.125.250.129", 19302 }, /* stun.l.google.com */
    { "162.159.207.0", 3478 },   /* stun.cloudflare.com (anycast) */
    { "46.225.95.169", 443 },    /* stun.nextcloud.com */
};
#define STUN_FALLBACK_COUNT (int)(sizeof(stun_fallback_servers) / sizeof(stun_fallback_servers[0]))

/* Probe scheduling (RFC 5389 §7.2.1 prescribes RTO >= 500 ms doubling;
 * we truncate to 3 sends per server since parallel servers substitute
 * for deeper retransmission). Offsets are from each server's
 * activation (DNS-resolution) time, not from Stun_Discover entry. */
static const uint32_t stun_rto_offsets_ms[] = { 0, 500, 1500 };
#define STUN_MAX_SENDS (int)(sizeof(stun_rto_offsets_ms) / sizeof(stun_rto_offsets_ms[0]))

#define STUN_MAX_SERVERS 8
#define STUN_DNS_FALLBACK_MS 300  /* arm numeric fallbacks if no DNS result by then */
#define STUN_NO_RESPONSE_FALLBACK_MS 1500 /* ...or if nothing has ANSWERED by then */
#define STUN_DISAGREE_GRACE_MS 300 /* post-first-response window to collect the rest */
#define STUN_DEFAULT_TIMEOUT_MS 4000

#ifdef NETPLAY_TEST_HOOKS
/* Test seam: replace the probed server list (numeric-IP mock servers on
 * localhost). While an override is installed the numeric fallback list
 * is NOT armed — tests control the exact endpoint set. */
static const StunServerDesc* s_stun_servers_override = NULL;
static int s_stun_servers_override_count = 0;

void Stun_TestHook_SetServers(const StunServerDesc* servers, int count) {
    if (servers == NULL || count <= 0) {
        s_stun_servers_override = NULL;
        s_stun_servers_override_count = 0;
        return;
    }
    if (count > STUN_MAX_SERVERS) count = STUN_MAX_SERVERS;
    s_stun_servers_override = servers;
    s_stun_servers_override_count = count;
}
#endif /* NETPLAY_TEST_HOOKS */

/* --- concurrent DNS resolution (S2) ------------------------------------ */

/* getaddrinfo has no portable timeout, so it runs on a side thread that
 * publishes per-host dotted-quad results through atomics. Ownership is
 * refcounted (2 owners: worker + caller); whoever drops the last ref
 * frees, so the caller can abandon a resolver stuck on a dead DNS
 * server (detach) without a use-after-free. */
typedef struct {
    int count;
    char host[STUN_MAX_SERVERS][64];
    uint16_t port[STUN_MAX_SERVERS];
    char ip[STUN_MAX_SERVERS][64];      /* valid when done[i] == 1 */
    SDL_AtomicInt done[STUN_MAX_SERVERS]; /* 0 pending, 1 resolved, -1 failed */
    SDL_AtomicInt refs;
} StunDnsJob;

static void stun_dns_job_unref(StunDnsJob* job) {
    if (SDL_AddAtomicInt(&job->refs, -1) == 1) {
        SDL_free(job);
    }
}

static int SDLCALL stun_dns_worker_fn(void* data) {
    StunDnsJob* job = (StunDnsJob*)data;
    for (int i = 0; i < job->count; i++) {
        /* Prefer IPv4 — NAT traversal (UPnP + hole punch) is IPv4-only,
         * and the discovery socket is bound to an IPv4 wildcard. */
        struct addrinfo hints = { 0 };
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        struct addrinfo* ai = NULL;
        if (getaddrinfo(job->host[i], NULL, &hints, &ai) == 0 && ai) {
            struct sockaddr_in* sin = (struct sockaddr_in*)ai->ai_addr;
            inet_ntop(AF_INET, &sin->sin_addr, job->ip[i], sizeof(job->ip[i]));
            freeaddrinfo(ai);
            SDL_SetAtomicInt(&job->done[i], 1);
        } else {
            if (ai) freeaddrinfo(ai);
            SDL_SetAtomicInt(&job->done[i], -1);
        }
    }
    stun_dns_job_unref(job);
    return 0;
}

/* --- parallel discovery ------------------------------------------------ */

typedef struct {
    NET_Address* addr;    /* resolved numeric address (ref held) */
    uint16_t port;
    char label[64];       /* host string for logging */
    uint8_t request[20];  /* prebuilt Binding Request (stable txid across retransmits) */
    uint8_t txid[12];
    uint32_t armed_at_ms; /* activation time — RTO offsets are relative to this */
    int sends;
    bool responded;
    char resp_ip[64];
    uint16_t resp_port;
} StunProbeSlot;

/* Add a probe slot for a numeric IP, deduping on (ip, port) — e.g.
 * stun.l.google.com and stun1.l.google.com often resolve to one
 * frontend, and a resolved entry may coincide with a fallback entry. */
static void stun_probe_add(StunProbeSlot* slots, int* slot_count, const char* numeric_ip, uint16_t port,
                           const char* label, uint32_t now_ms) {
    if (*slot_count >= STUN_MAX_SERVERS)
        return;
    /* Dedupe by resolved target string + port. */
    for (int i = 0; i < *slot_count; i++) {
        if (slots[i].port == port && slots[i].addr != NULL &&
            strcmp(NET_GetAddressString(slots[i].addr), numeric_ip) == 0) {
            return;
        }
    }
    NET_Address* addr = NET_ResolveHostname(numeric_ip);
    if (!addr)
        return;
    /* Numeric strings resolve synchronously-fast; bounded poll anyway. */
    int wait = 0;
    while (NET_GetAddressStatus(addr) == NET_WAITING && wait < 100) {
        SDL_Delay(1);
        wait++;
    }
    if (NET_GetAddressStatus(addr) != NET_SUCCESS) {
        NET_UnrefAddress(addr);
        return;
    }
    StunProbeSlot* s = &slots[*slot_count];
    memset(s, 0, sizeof(*s));
    s->addr = addr;
    s->port = port;
    SDL_strlcpy(s->label, label, sizeof(s->label));
    if (!build_binding_request(s->request, s->txid)) {
        /* L-3: no CSPRNG => no probe. Leaving the slot unarmed keeps
         * diag_servers_probed at 0, which the caller turns into an
         * INTERNAL failure rather than a connectivity diagnosis. */
        NET_UnrefAddress(addr);
        return;
    }
    s->armed_at_ms = now_ms;
    (*slot_count)++;
}

bool Stun_Discover(StunResult* result, uint16_t local_port, int timeout_ms) {
    if (!result)
        return false;
    memset(result, 0, sizeof(*result));
    result->socket = NULL;
    if (timeout_ms <= 0)
        timeout_ms = STUN_DEFAULT_TIMEOUT_MS;

    /* S4-review L-3: probe the CSPRNG up front and refuse EARLY when it
     * is unavailable. Discovery cannot produce a compliant transaction
     * ID without it (build_binding_request now fails closed), so every
     * slot would silently go unarmed and the caller would misread an
     * all-zeros diag as "no internet connection". This flag routes it
     * to CONNECT_FAIL_INTERNAL instead — nothing ever left the machine.
     * The host side already hard-failed here via RoomCode_GenerateNonce;
     * this makes the JOINER behave identically instead of proceeding
     * with predictable txids. */
    {
        uint8_t probe[12];
        if (!Csprng_Bytes(probe, sizeof(probe))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "STUN: platform CSPRNG unavailable — refusing to start "
                         "discovery (see build_binding_request)");
            result->diag_csprng_fail = true;
            return false;
        }
    }

    // Create socket once — local port stays consistent across server attempts.
    // Force IPv4: MiSTer kernel disables IPv6 (sysctl net.ipv6.conf.*.disable_ipv6=1),
    // and SDL3_net's default NULL bind_addr path tries AF_INET6 first → EAFNOSUPPORT.
    // Resolve "0.0.0.0" synchronously to an IPv4 wildcard NET_Address and bind to it.
    NET_Address* bind_addr = NET_ResolveHostname("0.0.0.0");
    if (bind_addr) {
        int wait = 0;
        while (NET_GetAddressStatus(bind_addr) == NET_WAITING && wait < 100) {
            SDL_Delay(1);
            wait++;
        }
        if (NET_GetAddressStatus(bind_addr) != 1) {
            NET_UnrefAddress(bind_addr);
            bind_addr = NULL;
        }
    }
    NET_DatagramSocket* sock = NET_CreateDatagramSocket(bind_addr, local_port);
    if (bind_addr) {
        NET_UnrefAddress(bind_addr);
    }
    if (!sock) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to create UDP socket: %s", SDL_GetError());
        /* S3-review M-2: local socket failure, not a network condition —
         * all diag counters are zero (memset above) and without this flag
         * the classifier's all-zeros fallback would misreport "No
         * internet connection (DNS failed)". */
        result->diag_socket_fail = true;
        return false;
    }

    // Increase receive buffer to 256KB to absorb bursts when the game loop
    // is busy re-simulating during rollback (inspired by Weyvelength SDK).
    NetTuning_SetRecvBuf(sock, 256 * 1024);

    /* Configured server list (test override replaces it wholesale). */
    const StunServerEntry* servers = stun_servers;
    int server_count = STUN_SERVER_COUNT;
    bool fallbacks_allowed = true;
#ifdef NETPLAY_TEST_HOOKS
    if (s_stun_servers_override != NULL) {
        servers = (const StunServerEntry*)s_stun_servers_override;
        server_count = s_stun_servers_override_count;
        fallbacks_allowed = false;
    }
#endif
    if (server_count > STUN_MAX_SERVERS)
        server_count = STUN_MAX_SERVERS;

    /* Kick off concurrent DNS for every configured server. */
    StunDnsJob* dns = (StunDnsJob*)SDL_calloc(1, sizeof(StunDnsJob));
    SDL_Thread* dns_thread = NULL;
    if (dns != NULL) {
        dns->count = server_count;
        for (int i = 0; i < server_count; i++) {
            SDL_strlcpy(dns->host[i], servers[i].host, sizeof(dns->host[i]));
            dns->port[i] = servers[i].port;
            SDL_SetAtomicInt(&dns->done[i], 0);
        }
        SDL_SetAtomicInt(&dns->refs, 2); /* worker + this function */
        dns_thread = SDL_CreateThread(stun_dns_worker_fn, "StunDns", dns);
        if (dns_thread == NULL) {
            /* No worker — drop its ref and run without DNS (numeric
             * fallbacks below still carry the attempt). */
            SDL_SetAtomicInt(&dns->refs, 1);
            stun_dns_job_unref(dns);
            dns = NULL;
        }
    }

    StunProbeSlot slots[STUN_MAX_SERVERS];
    int slot_count = 0;
    bool dns_consumed[STUN_MAX_SERVERS] = { false };
    bool fallbacks_armed = false;
    /* S3 evidence counters — published into result->diag_* on every
     * exit path so a failed discovery is still classifiable. */
    int dns_failures = 0;
    int sends_ok = 0;

    const uint32_t start = SDL_GetTicks();
    int first_responder = -1;
    uint32_t first_response_at = 0;

    /* Elapsed-subtraction guard (wrap-safe, matches Stun_HolePunch's
     * pattern) rather than an absolute-deadline compare. */
    while ((int)(SDL_GetTicks() - start) < timeout_ms) {
        const uint32_t now = SDL_GetTicks();

        /* 1) Absorb newly resolved DNS entries into probe slots. */
        if (dns != NULL) {
            for (int i = 0; i < dns->count; i++) {
                if (dns_consumed[i])
                    continue;
                const int st = SDL_GetAtomicInt(&dns->done[i]);
                if (st == 1) {
                    dns_consumed[i] = true;
                    stun_probe_add(slots, &slot_count, dns->ip[i], dns->port[i], dns->host[i], now);
                } else if (st == -1) {
                    dns_consumed[i] = true;
                    dns_failures++;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to resolve %s", dns->host[i]);
                }
            }
        }

        /* 2) Arm the numeric fallbacks when DNS produced nothing in time
         * (blackholed resolver, empty /etc/resolv.conf, ...) — OR when
         * DNS produced SOMETHING but no server has answered by a later
         * checkpoint. The second arm covers a partially-dead resolver:
         * getaddrinfo answers the first host then blackholes (the DNS
         * worker resolves serially), leaving one probed-but-dead slot —
         * slot_count != 0, so the 300 ms checkpoint alone would never
         * arm the fallbacks and discovery would burn the whole budget
         * (review L-3). 1500 ms = the last retransmit offset, so the
         * first resolved server has had its full send ladder before we
         * conclude it's dead. */
        if (fallbacks_allowed && !fallbacks_armed &&
            ((slot_count == 0 && (now - start) >= STUN_DNS_FALLBACK_MS) ||
             (first_responder < 0 && (now - start) >= STUN_NO_RESPONSE_FALLBACK_MS))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "STUN: no %s after %u ms — probing numeric fallback servers",
                        slot_count == 0 ? "DNS result" : "server response",
                        (unsigned)(now - start));
            for (int i = 0; i < STUN_FALLBACK_COUNT; i++) {
                stun_probe_add(slots, &slot_count, stun_fallback_servers[i].host,
                               stun_fallback_servers[i].port, stun_fallback_servers[i].host, now);
            }
            fallbacks_armed = true;
        }

        /* 3) Scheduled sends: initial + retransmits at 0/500/1500 ms per
         * server (relative to that server's activation). */
        for (int i = 0; i < slot_count; i++) {
            StunProbeSlot* s = &slots[i];
            if (s->responded || s->sends >= STUN_MAX_SENDS)
                continue;
            if ((now - s->armed_at_ms) >= stun_rto_offsets_ms[s->sends]) {
                if (!NET_SendDatagram(sock, s->addr, s->port, s->request, 20)) {
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to send to %s:%u: %s",
                                 s->label, (unsigned)s->port, SDL_GetError());
                } else {
                    sends_ok++;
                }
                s->sends++;
            }
        }

        /* 4) Drain responses; first parseable Binding Response wins. */
        NET_Datagram* dgram = NULL;
        while (NET_ReceiveDatagram(sock, &dgram) && dgram != NULL) {
            for (int i = 0; i < slot_count; i++) {
                StunProbeSlot* s = &slots[i];
                if (s->responded)
                    continue;
                char ip[64] = { 0 };
                uint16_t port = 0;
                if (parse_binding_response((const uint8_t*)dgram->buf, dgram->buflen, s->txid, ip, sizeof(ip),
                                           &port)) {
                    s->responded = true;
                    SDL_strlcpy(s->resp_ip, ip, sizeof(s->resp_ip));
                    s->resp_port = port;
                    if (first_responder < 0) {
                        first_responder = i;
                        first_response_at = now;
                    }
                    break;
                }
            }
            NET_DestroyDatagram(dgram);
            dgram = NULL;
        }

        /* 5) Exit: once someone answered, linger briefly (bounded by the
         * budget) to collect the remaining responses — server DISAGREE-
         * ment on the mapped port is the symmetric-NAT signal S3 wants
         * for failure attribution. Break early when every probed server
         * has answered. */
        if (first_responder >= 0) {
            bool all_answered = true;
            for (int i = 0; i < slot_count; i++) {
                if (slots[i].sends > 0 && !slots[i].responded) {
                    all_answered = false;
                    break;
                }
            }
            if (all_answered || (now - first_response_at) >= STUN_DISAGREE_GRACE_MS)
                break;
        }

        SDL_Delay(10);
    }

    /* Reap the DNS worker: join when it already finished, otherwise
     * detach and let the refcount free the job — a resolver stuck on a
     * dead DNS server must not block us here either. */
    if (dns_thread != NULL) {
        if (SDL_GetThreadState(dns_thread) == SDL_THREAD_COMPLETE) {
            SDL_WaitThread(dns_thread, NULL);
        } else {
            SDL_DetachThread(dns_thread);
        }
    }
    /* S3: publish the evidence counters on EVERY exit path — the
     * failure classifier (ConnectFail_ClassifyStunDiscover) reads these
     * off the StunResult even when we return false. dns_all_failed is
     * only claimed when a DNS job actually ran and every configured
     * hostname came back -1 (a job that never spawned proves nothing). */
    result->diag_servers_probed = slot_count;
    result->diag_sends_ok = sends_ok;
    result->diag_dns_all_failed =
        (dns != NULL && dns->count > 0 && dns_failures == dns->count);

    if (dns != NULL) {
        stun_dns_job_unref(dns);
    }

    if (first_responder < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "STUN: no response from any of %d server(s) within %d ms", slot_count, timeout_ms);
        for (int i = 0; i < slot_count; i++) {
            NET_UnrefAddress(slots[i].addr);
        }
        NET_DestroyDatagramSocket(sock);
        return false;
    }

    /* Success — the FIRST responder defines the advertised endpoint and
     * is retained for S1 keepalives (ref transferred to result). */
    StunProbeSlot* win = &slots[first_responder];
    SDL_strlcpy(result->public_ip, win->resp_ip, sizeof(result->public_ip));
    result->public_port = win->resp_port;
    result->socket = sock; // Keep open for hole punching!
    result->server_addr = (struct NET_Address*)win->addr;
    result->server_port = win->port;

    /* S2: record whether any other server observed a DIFFERENT mapped
     * port — per-destination translation, i.e. a symmetric NAT. S3
     * consumes this for failure attribution; no UX here. */
    int responders = 0;
    for (int i = 0; i < slot_count; i++) {
        if (!slots[i].responded)
            continue;
        responders++;
        if (slots[i].resp_port != win->resp_port) {
            result->port_disagreement = true;
            SDL_Log("STUN: servers disagree on our mapped port (%s:%u says %u, %s:%u says %u) — "
                    "symmetric NAT likely",
                    win->label, (unsigned)win->port, (unsigned)win->resp_port,
                    slots[i].label, (unsigned)slots[i].port, (unsigned)slots[i].resp_port);
        }
    }
    result->diag_servers_answered = responders; /* S3 evidence */

    // Query the ACTUAL OS-assigned local port via getsockname.
    // The STUN public port may differ from the local port on
    // non-port-preserving NATs. The hairpin bypass needs the real
    // local port so localhost connections target the correct socket.
    {
        const NetTuningDgramMirror* m = (const NetTuningDgramMirror*)sock;
        result->local_port = result->public_port; // Fallback: assume port-preserving NAT
        for (int h = 0; h < m->num_handles; h++) {
            struct sockaddr_storage sa;
            int sa_len = sizeof(sa);
            if (getsockname((int)m->handles[h].handle, (struct sockaddr*)&sa, &sa_len) == 0) {
                if (sa.ss_family == AF_INET) {
                    result->local_port = ntohs(((struct sockaddr_in*)&sa)->sin_port);
                    break;
                } else if (sa.ss_family == AF_INET6) {
                    result->local_port = ntohs(((struct sockaddr_in6*)&sa)->sin6_port);
                    // Keep looking for IPv4; prefer it since STUN used IPv4
                }
            }
        }
    }

    /* Release the non-winning slot addresses. */
    for (int i = 0; i < slot_count; i++) {
        if (i != first_responder) {
            NET_UnrefAddress(slots[i].addr);
        }
    }

    SDL_Log("STUN: Discovered public endpoint via %s in %u ms (%d/%d servers answered, local port %u%s)",
            win->label, (unsigned)(SDL_GetTicks() - start), responders, slot_count,
            (unsigned)result->local_port,
            result->port_disagreement ? ", PORT DISAGREEMENT" : "");

    return true;
}

/* --- S6 non-blocking punch leg ----------------------------------------
 *
 * docs/plan-netplay-connection.md §8. Extracted from the body of
 * Stun_HolePunch so the joiner can RACE several candidate endpoints (and
 * the rendezvous / relay legs) on one thread and one socket. The wire
 * behaviour is unchanged: same 17-byte authenticated payload, same S2
 * adaptive cadence, same source-IP + exact-payload accept criteria with
 * the port deliberately unmatched (S2 symmetric retarget), same ~600 ms
 * confirmation tail. Stun_HolePunch below is now a blocking driver over
 * this stepper, so its existing regression tests cover both.
 */

/* S2 adaptive cadence: establishment time is dominated by peer start-skew
 * and first-packet loss, not RTT — the two sides rarely enter their punch
 * loops at the same instant, and the first datagram toward a NAT is the
 * one most likely to be dropped while the mapping opens. Burst at 50 ms
 * for the first 500 ms (<= 10 datagrams of 17 bytes — negligible
 * traffic), then back off to the original 200 ms steady state. */
#define STUN_PUNCH_INTERVAL_FAST_MS 50
#define STUN_PUNCH_INTERVAL_SLOW_MS 200
#define STUN_PUNCH_BURST_WINDOW_MS 500

bool Stun_PunchBegin(StunPunchLeg* leg, const char* peer_ip, uint16_t peer_port,
                     const uint8_t punch_token[STUN_PUNCH_TOKEN_LEN],
                     uint32_t now_ms) {
    if (!leg || !peer_ip || !punch_token)
        return false;
    memset(leg, 0, sizeof(*leg));

    NET_Address* peer = NET_ResolveHostname(peer_ip);
    if (!peer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to resolve peer IP");
        return false;
    }
    /* Numeric dotted-quads (the only thing a room code or a DELIVER ever
     * yields) resolve without ever entering NET_WAITING; the bounded poll
     * is retained verbatim from Stun_HolePunch for the hostname case. */
    int wait_attempts = 0;
    while (NET_GetAddressStatus(peer) == NET_WAITING && wait_attempts < 300) {
        SDL_Delay(10);
        wait_attempts++;
    }
    if (NET_GetAddressStatus(peer) != NET_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to resolve peer IP");
        NET_UnrefAddress(peer);
        return false;
    }

    leg->target = peer;
    leg->target_port = peer_port;
    leg->start_ms = now_ms;
    leg->last_send_ms = 0;
    leg->sent_any = false;
    leg->confirmed = false;
    memcpy(leg->token, punch_token, STUN_PUNCH_TOKEN_LEN);
    /* S4a: authenticated punch payload — "3SX_PUNCH" + the 8-byte token
     * derived from the room-code payload. Sent with every punch; REQUIRED
     * on every accepted datagram. */
    Stun_BuildPunchPayload(punch_token, leg->msg);
    SDL_strlcpy(leg->peer_ip, NET_GetAddressString(peer), sizeof(leg->peer_ip));
    return true;
}

bool Stun_PunchActive(const StunPunchLeg* leg) {
    return leg != NULL && leg->target != NULL;
}

bool Stun_PunchConfirmed(const StunPunchLeg* leg) {
    return leg != NULL && leg->confirmed;
}

void Stun_PunchPump(StunPunchLeg* leg, NET_DatagramSocket* sock, uint32_t now_ms) {
    if (!Stun_PunchActive(leg) || sock == NULL)
        return;

    int interval_ms;
    if (leg->confirmed) {
        /* Confirmation tail: keep punching the CONFIRMED endpoint at the
         * fast cadence so a peer whose own loop started late, or which
         * lost our earlier datagrams, reliably sees at least one. */
        interval_ms = STUN_PUNCH_INTERVAL_FAST_MS;
    } else {
        interval_ms = ((int)(now_ms - leg->start_ms) < STUN_PUNCH_BURST_WINDOW_MS)
                          ? STUN_PUNCH_INTERVAL_FAST_MS
                          : STUN_PUNCH_INTERVAL_SLOW_MS;
    }
    if (leg->sent_any && (now_ms - leg->last_send_ms) < (uint32_t)interval_ms)
        return;

    if (!NET_SendDatagram(sock, leg->target, leg->target_port, leg->msg, sizeof(leg->msg))) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "STUN: Hole punch send failed: %s",
                     SDL_GetError());
    }
    leg->last_send_ms = now_ms;
    leg->sent_any = true;
}

bool Stun_PunchOffer(StunPunchLeg* leg, StunResult* local,
                     const uint8_t* buf, int len,
                     const char* src_ip, uint16_t src_port, uint32_t now_ms) {
    if (!Stun_PunchActive(leg) || buf == NULL || src_ip == NULL)
        return false;

    /* Accept criteria (S4a, unchanged): source IP + the exact 17-byte
     * authenticated payload. The PORT is deliberately NOT matched — that
     * is what recognises a symmetric peer punching from a translated
     * per-destination mapping. */
    if (strcmp(src_ip, NET_GetAddressString(leg->target)) != 0)
        return false;

    if (!Stun_HasPunchPrefix(buf, len))
        return false;

    if (!Stun_IsPunchPayload(buf, len, leg->token)) {
        /* S4a: a datagram from the expected peer IP that SPEAKS the punch
         * protocol but fails the token check (a legacy 9-byte payload from
         * an older build, or a token derived from a different room
         * payload) is decisive version-mismatch evidence — record it for
         * the failure classifier, then drop it like any other stranger. */
        if (local != NULL) {
            local->diag_punch_bad_token = true;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "STUN: punch from %s:%u has a bad/missing auth token (len=%d) — "
                    "peer build mismatch? Ignoring.",
                    src_ip, (unsigned)src_port, len);
        return true; /* consumed: it is ours, it is just not acceptable */
    }

    if (!leg->confirmed) {
        SDL_Log("STUN: Hole punch SUCCESS — received response from peer");
        /* S2 retarget fix (docs/plan-netplay-connection.md §4): a
         * symmetric-NAT peer punches us from a mapping whose port differs
         * from the one we were told. Pre-fix the confirmation sends still
         * went to the ORIGINAL port, so the symmetric peer never saw them
         * and its own punch timed out. Retarget every subsequent send at
         * the observed source endpoint before confirming. */
        if (src_port != leg->target_port) {
            SDL_Log("STUN: peer punched from translated port %u (expected %u) — "
                    "retargeting confirmation sends at the observed endpoint",
                    (unsigned)src_port, (unsigned)leg->target_port);
        }
        leg->confirmed = true;
        leg->confirm_ms = now_ms;
        /* Re-send immediately at the new target rather than waiting out
         * the current interval. */
        leg->sent_any = false;
    }
    leg->target_port = src_port;
    SDL_strlcpy(leg->peer_ip, src_ip, sizeof(leg->peer_ip));
    return true;
}

bool Stun_PunchSettled(const StunPunchLeg* leg, uint32_t now_ms) {
    return leg != NULL && leg->confirmed &&
           (int)(now_ms - leg->confirm_ms) >= STUN_PUNCH_CONFIRM_MS;
}

void Stun_PunchEndpoint(const StunPunchLeg* leg, char* out_ip, int ip_buf_size,
                        uint16_t* out_port) {
    if (leg == NULL)
        return;
    if (out_ip != NULL && ip_buf_size > 0) {
        SDL_strlcpy(out_ip, leg->peer_ip, (size_t)ip_buf_size);
    }
    if (out_port != NULL) {
        *out_port = leg->target_port;
    }
}

void Stun_PunchEnd(StunPunchLeg* leg) {
    if (leg == NULL || leg->target == NULL)
        return;
    NET_UnrefAddress(leg->target);
    leg->target = NULL;
}

bool Stun_HolePunch(StunResult* local, char* peer_ip, uint16_t* peer_port,
                    const uint8_t punch_token[STUN_PUNCH_TOKEN_LEN],
                    int punch_duration_ms, SDL_AtomicInt* cancel_flag) {
    if (!local || !local->socket || !peer_ip || !peer_port || !punch_token)
        return false;

    NET_DatagramSocket* sock = local->socket;

    StunPunchLeg leg;
    if (!Stun_PunchBegin(&leg, peer_ip, *peer_port, punch_token, SDL_GetTicks())) {
        return false;
    }

    SDL_Log("STUN: Hole punching for %dms...", punch_duration_ms);

    const uint32_t start = leg.start_ms;

    while ((int)(SDL_GetTicks() - start) < punch_duration_ms ||
           (leg.confirmed && !Stun_PunchSettled(&leg, SDL_GetTicks()))) {
        if (cancel_flag && SDL_GetAtomicInt(cancel_flag)) {
            /* Cancel is a caller-initiated abort, not a success — report
             * false. (All current call sites re-check the flag after
             * return, but a `true` here is a latent trap for any future
             * caller that trusts the return value.) */
            SDL_Log(leg.confirmed ? "STUN: Hole punch cancelled by caller during confirmation"
                                  : "STUN: Hole punch cancelled by caller");
            Stun_PunchEnd(&leg);
            return false;
        }
        const uint32_t now = SDL_GetTicks();
        Stun_PunchPump(&leg, sock, now);

        NET_Datagram* dgram = NULL;
        NET_ReceiveDatagram(sock, &dgram);
        if (dgram) {
            /* NOTE: NET_GetAddressString returns a pointer to an internal
             * static buffer — copy this result before Stun_PunchOffer
             * calls it again on the leg's own address. */
            char recv_addr[64];
            SDL_strlcpy(recv_addr, NET_GetAddressString(dgram->addr), sizeof(recv_addr));
            (void)Stun_PunchOffer(&leg, local, dgram->buf, dgram->buflen, recv_addr,
                                  dgram->port, now);
            NET_DestroyDatagram(dgram);
        } else {
            SDL_Delay(10); /* don't spin too hot if no data */
        }

        if (Stun_PunchSettled(&leg, SDL_GetTicks())) {
            break;
        }
    }

    const bool received_response = leg.confirmed;
    if (received_response) {
        /* Update with the actual received endpoint (this is what fixes
         * symmetric-NAT port/IP translation for the caller). */
        Stun_PunchEndpoint(&leg, peer_ip, 64, peer_port);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "STUN: Hole punch timed out after %dms. "
                    "Peer may be behind Symmetric NAT.",
                    punch_duration_ms);
    }

    Stun_PunchEnd(&leg);
    return received_response;
}

void Stun_CloseSocket(StunResult* result) {
    if (result && result->socket != NULL) {
        NET_DestroyDatagramSocket(result->socket);
        result->socket = NULL;
    }
    Stun_ReleaseServerAddr(result);
}

void Stun_ReleaseServerAddr(StunResult* result) {
    if (result && result->server_addr != NULL) {
        NET_UnrefAddress((NET_Address*)result->server_addr);
        result->server_addr = NULL;
        result->server_port = 0;
    }
}

bool Stun_SendKeepalive(StunResult* result, uint8_t out_txid[12]) {
    if (!result || !result->socket || !result->server_addr || !out_txid)
        return false;

    uint8_t request[20];
    if (!build_binding_request(request, out_txid)) {
        return false; /* L-3: no CSPRNG => no keepalive, and say so */
    }
    if (!NET_SendDatagram(result->socket, (NET_Address*)result->server_addr, result->server_port, request, 20)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "STUN: keepalive send failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool Stun_IsBindingResponse(const uint8_t* buf, int len) {
    if (!buf || len < 20)
        return false;
    uint16_t msg_type = ((uint16_t)buf[0] << 8) | buf[1];
    if (msg_type != STUN_BINDING_RESPONSE)
        return false;
    uint32_t cookie = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    return cookie == STUN_MAGIC_COOKIE;
}

bool Stun_ParseBindingResponse(const uint8_t* buf, int len, const uint8_t txid[12], char* out_ip, int ip_buf_size,
                               uint16_t* out_port) {
    if (!buf || !txid || !out_ip || !out_port)
        return false;
    return parse_binding_response(buf, len, txid, out_ip, ip_buf_size, out_port);
}
