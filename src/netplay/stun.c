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

// Build a 20-byte STUN Binding Request (RFC 5389 §6)
static void build_binding_request(uint8_t* buf, uint8_t* transaction_id) {
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
    // Transaction ID (12 random bytes)
    for (int i = 0; i < 12; i++) {
        transaction_id[i] = (uint8_t)(SDL_rand(256));
        buf[8 + i] = transaction_id[i];
    }
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
    build_binding_request(s->request, s->txid);
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

bool Stun_HolePunch(StunResult* local, char* peer_ip, uint16_t* peer_port, int punch_duration_ms,
                    SDL_AtomicInt* cancel_flag) {
    if (!local || !local->socket || !peer_ip || !peer_port)
        return false;

    NET_DatagramSocket* sock = local->socket;

    // Punch packet — a small identifiable payload
    const char punch_msg[] = "3SX_PUNCH";
    /* S2 adaptive cadence: establishment time is dominated by peer
     * start-skew and first-packet loss, not RTT — the two sides rarely
     * enter their punch loops at the same instant, and the first
     * datagram toward a NAT is the one most likely to be dropped while
     * the mapping opens. Burst at 50 ms for the first 500 ms (<= 10
     * datagrams of 9 bytes — negligible traffic), then back off to the
     * original 200 ms steady-state for the rest of the window. */
    const int punch_interval_fast_ms = 50;
    const int punch_interval_slow_ms = 200;
    const int punch_burst_window_ms = 500;

    NET_Address* peer = NET_ResolveHostname(peer_ip);
    if (!peer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to resolve peer IP");
        return false;
    }

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

    SDL_Log("STUN: Hole punching for %dms...", punch_duration_ms);

    uint32_t start = SDL_GetTicks();
    uint32_t last_send = 0;
    bool received_response = false;

    uint16_t local_peer_port = *peer_port; // Host order — NET_SendDatagram expects host order

    while ((int)(SDL_GetTicks() - start) < punch_duration_ms) {
        // Check for cancellation
        if (cancel_flag && SDL_GetAtomicInt(cancel_flag)) {
            SDL_Log("STUN: Hole punch cancelled by caller");
            NET_UnrefAddress(peer);
            return false;
        }
        uint32_t now = SDL_GetTicks();

        // Send punch packet periodically (fast burst early, then back off)
        const int punch_interval_ms = ((int)(now - start) < punch_burst_window_ms)
                                          ? punch_interval_fast_ms
                                          : punch_interval_slow_ms;
        if (now - last_send >= (uint32_t)punch_interval_ms || last_send == 0) {
            if (!NET_SendDatagram(sock, peer, local_peer_port, punch_msg, strlen(punch_msg))) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "STUN: Hole punch send failed: %s", SDL_GetError());
            }
            last_send = now;
        }

        // Try to receive from peer
        NET_Datagram* dgram = NULL;
        NET_ReceiveDatagram(sock, &dgram);

        if (dgram) {
            // Check if it's a punch from our expected peer.
            // NOTE: NET_GetAddressString returns a pointer to an internal
            // static buffer — must copy one result before the second call.
            char recv_addr[64];
            SDL_strlcpy(recv_addr, NET_GetAddressString(dgram->addr), sizeof(recv_addr));
            if (strcmp(recv_addr, NET_GetAddressString(peer)) == 0 && dgram->buflen == strlen(punch_msg) &&
                strncmp((char*)dgram->buf, punch_msg, dgram->buflen) == 0) {
                SDL_Log("STUN: Hole punch SUCCESS — received response from peer");
                received_response = true;

                /* S2 retarget fix (docs/plan-netplay-connection.md §4): a
                 * symmetric-NAT peer punches us from a per-destination
                 * mapping whose port differs from the one we were told
                 * (the rendezvous/room-code port). We deliberately accept
                 * on source-IP + payload only, so we DO learn the true
                 * translated endpoint here — but pre-fix, the confirmation
                 * sends below still targeted the ORIGINAL port captured
                 * into local_peer_port at function entry, so the symmetric
                 * peer never saw our confirmations and ITS punch timed
                 * out. Retarget every subsequent send at the observed
                 * source endpoint before confirming. */
                if (dgram->port != local_peer_port) {
                    SDL_Log("STUN: peer punched from translated port %u (expected %u) — "
                            "retargeting confirmation sends at the observed endpoint",
                            (unsigned)dgram->port, (unsigned)local_peer_port);
                }
                local_peer_port = dgram->port;

                // Update with actual received endpoint (fixes Symmetric NAT port/IP translation)
                *peer_port = dgram->port; // Host order — NET_ReceiveDatagram returns host order

                // Update peer_ip from received address (Symmetric NAT may change it)
                const char* received_ip = NET_GetAddressString(dgram->addr);
                SDL_strlcpy(peer_ip, received_ip, 64);

                /* Keep a ref on the OBSERVED source address (the accept
                 * criteria guarantee it string-matches `peer`'s IP, but
                 * using the datagram's own address is the principled
                 * target). NET_DestroyDatagram drops the datagram's ref,
                 * so take our own before destroying. */
                NET_Address* confirmed = NET_RefAddress(dgram->addr);
                NET_DestroyDatagram(dgram);

                /* Keep punching the confirmed endpoint for ~600 ms at the
                 * fast cadence so the peer — whose own punch loop may have
                 * started late or lost packets — reliably sees at least
                 * one of ours. (Replaces the old 3 x 50 ms burst, which
                 * additionally went to the WRONG port for symmetric
                 * peers.) */
                const uint32_t confirm_start = SDL_GetTicks();
                while ((int)(SDL_GetTicks() - confirm_start) < 600) {
                    if (cancel_flag && SDL_GetAtomicInt(cancel_flag)) {
                        /* Cancel is a caller-initiated abort, not a
                         * success — report false like the main-loop
                         * cancel path above does. (All current call
                         * sites re-check the flag after return, but a
                         * `true` here is a latent trap for any future
                         * caller that trusts the return value.) */
                        SDL_Log("STUN: Hole punch cancelled by caller during confirmation");
                        if (confirmed != NULL) {
                            NET_UnrefAddress(confirmed);
                        }
                        NET_UnrefAddress(peer);
                        return false;
                    }
                    NET_SendDatagram(sock, confirmed != NULL ? confirmed : peer, local_peer_port,
                                     punch_msg, strlen(punch_msg));
                    SDL_Delay(50);
                }
                if (confirmed != NULL) {
                    NET_UnrefAddress(confirmed);
                }
                break;
            }
            NET_DestroyDatagram(dgram);
        } else {
            SDL_Delay(10); // Don't spin too hot if no data
        }
    }

    if (!received_response) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "STUN: Hole punch timed out after %dms. "
                    "Peer may be behind Symmetric NAT.",
                    punch_duration_ms);
    }

    NET_UnrefAddress(peer);

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
    build_binding_request(request, out_txid);
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
