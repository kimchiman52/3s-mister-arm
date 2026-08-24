/**
 * @file natpmp.c
 * @brief PCP (RFC 6887) + NAT-PMP (RFC 6886) port-mapping client.
 *
 * S7 of docs/plan-netplay-connection.md §9. See natpmp.h for the API and
 * the per-field RFC citations; this file carries the wire encoding, the
 * gateway lookup, and the bounded request/retransmit loop.
 *
 * Hand-rolled on purpose: the whole client is ~60 bytes of packet and a
 * select() loop, and adding libnatpmp would mean a new entry in
 * build-deps.sh plus a new .so on the MiSTer deploy for something this
 * small (authorized decision, plan §9).
 */

#include "netplay/natpmp.h"

#include "utils/csprng.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int np_socklen_t;
#define NP_INVALID_SOCK INVALID_SOCKET
#define NP_CLOSE(s) closesocket(s)
typedef SOCKET np_sock_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef socklen_t np_socklen_t;
#define NP_INVALID_SOCK (-1)
#define NP_CLOSE(s) close(s)
typedef int np_sock_t;
#endif

/* ====================================================================== */
/* Byte helpers                                                           */
/* ====================================================================== */

/* All multi-byte NAT-PMP fields are "transmitted in the traditional
 * network byte order (i.e., most significant byte first)" (RFC 6886
 * §3.3); PCP is likewise big-endian throughout (RFC 6887 §7.1/§7.2
 * figures). These read/write explicitly rather than memcpy'ing host
 * integers so the encoding is endian-independent and directly
 * comparable to the RFC diagrams. */

static void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint16_t get_u16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t get_u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

/* An IPv4 address inside a PCP 128-bit address field: RFC 6887 §5, "an
 * IPv4-mapped IPv6 address [RFC4291] is used (::ffff:0:0/96). This has
 * the first 80 bits set to zero and the next 16 set to one, while its
 * last 32 bits are filled with the IPv4 address."
 *
 * ip_be is already in network order, so its four bytes go in as-is. */
static void put_v4_mapped(uint8_t* p, uint32_t ip_be) {
    memset(p, 0, 10);
    p[10] = 0xFFu;
    p[11] = 0xFFu;
    memcpy(&p[12], &ip_be, 4);
}

/* RFC 6887 §5: "When checking for an IPv4-mapped IPv6 address, all of
 * the first 96 bits MUST be checked for the pattern -- it is not
 * sufficient to check for ones in bits 81-96." */
static bool get_v4_mapped(const uint8_t* p, uint32_t* out_ip_be) {
    for (int i = 0; i < 10; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    if (p[10] != 0xFFu || p[11] != 0xFFu) {
        return false;
    }
    memcpy(out_ip_be, &p[12], 4);
    return true;
}

/* ====================================================================== */
/* PCP MAP codec — RFC 6887 §7.1, §7.2, §11.1, §11.2                      */
/* ====================================================================== */

bool Natpmp_BuildPcpMapRequest(uint8_t out[NATPMP_PCP_MAP_LEN],
                               const uint8_t nonce[NATPMP_PCP_NONCE_LEN],
                               uint8_t protocol,
                               uint16_t internal_port,
                               uint16_t suggested_external_port,
                               uint32_t suggested_external_ip_be,
                               uint32_t client_ip_be,
                               uint32_t lifetime_s) {
    if (out == NULL || nonce == NULL) {
        return false;
    }
    memset(out, 0, NATPMP_PCP_MAP_LEN);

    /* --- Common Request Header, RFC 6887 §7.1 (24 bytes) --- */
    out[0] = NATPMP_PCP_VERSION;                      /* Version = 2          */
    out[1] = NATPMP_PCP_OPCODE_MAP;                   /* R = 0 (request), MAP */
    /* out[2..4): Reserved, 16 bits, "MUST be zero on transmission".      */
    put_u32(&out[4], lifetime_s);                     /* Requested Lifetime   */
    put_v4_mapped(&out[8], client_ip_be);             /* PCP Client's IP      */

    /* --- MAP opcode-specific, RFC 6887 §11.1 (36 bytes) --- */
    memcpy(&out[24], nonce, NATPMP_PCP_NONCE_LEN);    /* Mapping Nonce, 96b   */
    out[36] = protocol;                               /* Protocol (17 = UDP)  */
    /* out[37..40): Reserved, 24 bits, "MUST be sent as 0".               */
    put_u16(&out[40], internal_port);                 /* Internal Port        */
    put_u16(&out[42], suggested_external_port);       /* Suggested Ext Port   */
    put_v4_mapped(&out[44], suggested_external_ip_be);/* Suggested Ext IP     */
    return true;
}

NatpmpParse Natpmp_ParsePcpMapResponse(const uint8_t* buf, int len,
                                       const uint8_t nonce[NATPMP_PCP_NONCE_LEN],
                                       uint8_t protocol,
                                       uint16_t internal_port,
                                       NatpmpPcpMap* out) {
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (buf == NULL || nonce == NULL || out == NULL || len < 4) {
        return NATPMP_PARSE_NOT_OURS;
    }

    /* THE NAT-PMP DOWNGRADE TEST, AND IT RUNS FIRST.
     *
     * RFC 6886 §3.5 defines a NAT-PMP gateway's reply to a non-zero
     * version as: Vers=0, OP=0, Result Code=1 (16 bits), Epoch (32 bits)
     * — eight bytes. Overlaid on the PCP response header (RFC 6887 §7.2)
     * that reads as version=0, R|Opcode=0, Reserved=0x00, Result Code=1,
     * which is exactly the UNSUPP_VERSION-with-version-zero that RFC 6887
     * §9 step 4 calls out: "If the version number in the UNSUPP_VERSION
     * response is zero then that means this is a NAT-PMP server".
     * RFC 6887 Appendix A describes the same overlap from the server
     * side ("Normal PCP processing will emit a PCP response that is
     * compatible with NAT-PMP").
     *
     * Note the frame's OP byte is 0, i.e. the PCP R bit is CLEAR. Testing
     * R before version would throw away the one frame that tells us to
     * downgrade, and the gateway would look silent. */
    if (buf[0] == 0 && buf[3] == (uint8_t)NATPMP_PCP_UNSUPP_VERSION) {
        return NATPMP_PARSE_PCP_IS_NATPMP;
    }

    if (len < NATPMP_PCP_MAP_LEN) {
        return NATPMP_PARSE_NOT_OURS;
    }
    if (buf[0] != NATPMP_PCP_VERSION) {
        return NATPMP_PARSE_NOT_OURS;
    }
    /* §7.2: "R: Indicates Request (0) or Response (1). All Responses
     * MUST use 1." and "Opcode: The 7-bit Opcode value. The server copies
     * this value from the request." */
    if ((buf[1] & NATPMP_PCP_R_BIT) == 0) {
        return NATPMP_PARSE_NOT_OURS;
    }
    if ((buf[1] & 0x7Fu) != NATPMP_PCP_OPCODE_MAP) {
        return NATPMP_PARSE_NOT_OURS;
    }

    /* §11.2: Mapping Nonce, Protocol and Internal Port are all "Copied
     * from the request". Together they are this protocol's only reply
     * matcher — PCP has no transaction ID — so all three are checked
     * before anything from the frame is believed. An off-path forgery
     * has to guess the 96-bit nonce. */
    if (memcmp(&buf[24], nonce, NATPMP_PCP_NONCE_LEN) != 0) {
        return NATPMP_PARSE_NOT_OURS;
    }
    if (buf[36] != protocol) {
        return NATPMP_PARSE_NOT_OURS;
    }
    if (get_u16(&buf[40]) != internal_port) {
        return NATPMP_PARSE_NOT_OURS;
    }

    const uint8_t result = buf[3];
    if (result != (uint8_t)NATPMP_PCP_SUCCESS) {
        /* Ours, and refused. Report the code; §7.2 says the header
         * Lifetime on an error response is a back-off hint, not a
         * mapping lifetime, so it is deliberately NOT copied out. */
        out->result_code = result;
        return NATPMP_PARSE_REFUSED;
    }

    /* IPv4-only client: an assigned external address that is not
     * IPv4-mapped is unusable to us and must not become a mapping. */
    uint32_t ext_ip_be = 0;
    if (!get_v4_mapped(&buf[44], &ext_ip_be)) {
        return NATPMP_PARSE_NOT_OURS;
    }

    out->result_code = result;
    out->lifetime_s = get_u32(&buf[4]);   /* §7.2 Lifetime  */
    out->epoch_s = get_u32(&buf[8]);      /* §7.2 Epoch     */
    out->internal_port = get_u16(&buf[40]);
    out->external_port = get_u16(&buf[42]); /* §11.2 Assigned External Port */
    out->external_ip_be = ext_ip_be;
    return NATPMP_PARSE_OK;
}

/* ====================================================================== */
/* NAT-PMP codec — RFC 6886 §3.2, §3.3, §3.4                              */
/* ====================================================================== */

bool Natpmp_BuildPmpAddrRequest(uint8_t out[NATPMP_PMP_ADDR_REQ_LEN]) {
    if (out == NULL) {
        return false;
    }
    /* RFC 6886 §3.2: two bytes, "Vers = 0 | OP = 0". */
    out[0] = NATPMP_PMP_VERSION;
    out[1] = NATPMP_PMP_OP_PUBLIC_ADDR;
    return true;
}

NatpmpParse Natpmp_ParsePmpAddrResponse(const uint8_t* buf, int len,
                                        NatpmpPmpAddr* out) {
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (buf == NULL || out == NULL || len < NATPMP_PMP_ADDR_RESP_LEN) {
        return NATPMP_PARSE_NOT_OURS;
    }
    /* §3.2 response: Vers = 0 | OP = 128 + 0 | Result Code (2) |
     * Seconds Since Start of Epoch (4) | External IPv4 Address (4). */
    if (buf[0] != NATPMP_PMP_VERSION) {
        return NATPMP_PARSE_NOT_OURS;
    }
    if (buf[1] != (uint8_t)(NATPMP_PMP_RESP_FLAG + NATPMP_PMP_OP_PUBLIC_ADDR)) {
        return NATPMP_PARSE_NOT_OURS;
    }
    const uint16_t result = get_u16(&buf[2]);
    out->result_code = result;
    out->epoch_s = get_u32(&buf[4]);
    if (result != (uint16_t)NATPMP_PMP_SUCCESS) {
        /* §3.2: "If the result code is non-zero, the value of the
         * External IPv4 Address field is undefined ... MUST be ignored
         * on reception." Leave external_ip_be at 0. */
        return NATPMP_PARSE_REFUSED;
    }
    memcpy(&out->external_ip_be, &buf[8], 4);
    return NATPMP_PARSE_OK;
}

bool Natpmp_BuildPmpMapRequest(uint8_t out[NATPMP_PMP_MAP_REQ_LEN],
                               uint8_t opcode,
                               uint16_t internal_port,
                               uint16_t suggested_external_port,
                               uint32_t lifetime_s) {
    if (out == NULL) {
        return false;
    }
    if (opcode != NATPMP_PMP_OP_MAP_UDP && opcode != NATPMP_PMP_OP_MAP_TCP) {
        /* §3.3: "Opcodes supported: 1 - Map UDP, 2 - Map TCP". Anything
         * else is not a mapping request; refuse to emit it. */
        return false;
    }
    memset(out, 0, NATPMP_PMP_MAP_REQ_LEN);
    out[0] = NATPMP_PMP_VERSION;
    out[1] = opcode;
    /* out[2..4): Reserved, "MUST be set to zero on transmission". */
    put_u16(&out[4], internal_port);
    /* §3.4 deletion: "The Suggested External Port MUST be set to zero by
     * the client on sending". Enforced here rather than trusted from the
     * caller — a delete that carried a port would be a spec violation
     * emitted from a path (teardown) that nobody watches. */
    put_u16(&out[6], lifetime_s == 0 ? 0 : suggested_external_port);
    put_u32(&out[8], lifetime_s);
    return true;
}

NatpmpParse Natpmp_ParsePmpMapResponse(const uint8_t* buf, int len,
                                       uint8_t req_opcode,
                                       uint16_t req_internal_port,
                                       NatpmpPmpMap* out) {
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (buf == NULL || out == NULL || len < NATPMP_PMP_MAP_RESP_LEN) {
        return NATPMP_PARSE_NOT_OURS;
    }
    if (buf[0] != NATPMP_PMP_VERSION) {
        return NATPMP_PARSE_NOT_OURS;
    }
    /* §3.3: "The 'x' in the OP field MUST match what the client
     * requested." A UDP mapping request must never be satisfied by a TCP
     * response. */
    if (buf[1] != (uint8_t)(NATPMP_PMP_RESP_FLAG + req_opcode)) {
        return NATPMP_PARSE_NOT_OURS;
    }
    /* §3.5: "The Internal Port MUST be set to the client's requested
     * Internal Port. This is particularly important, because the client
     * needs this information to identify which request suffered the
     * failure." NAT-PMP carries no transaction ID, so the echoed
     * internal port IS the correlator — checked on the failure path too,
     * which is why it sits above the result-code branch. */
    if (get_u16(&buf[8]) != req_internal_port) {
        return NATPMP_PARSE_NOT_OURS;
    }

    const uint16_t result = get_u16(&buf[2]);
    out->result_code = result;
    out->epoch_s = get_u32(&buf[4]);
    out->internal_port = get_u16(&buf[8]);
    if (result != (uint16_t)NATPMP_PMP_SUCCESS) {
        /* §3.5: on failure "The Mapped External Port and Port Mapping
         * Lifetime MUST be set appropriately -- i.e., zero if no
         * successful port mapping was created." Do not copy them out;
         * a caller that ignores the verdict then cannot act on them. */
        return NATPMP_PARSE_REFUSED;
    }
    out->external_port = get_u16(&buf[10]);
    out->lifetime_s = get_u32(&buf[12]);
    return NATPMP_PARSE_OK;
}

/* ====================================================================== */
/* Gateway discovery                                                      */
/* ====================================================================== */

#ifdef NETPLAY_TEST_HOOKS
static char s_hook_gw_ip[64] = { 0 };
static uint16_t s_hook_gw_port = 0;

void Natpmp_TestHook_SetGateway(const char* ip, uint16_t port) {
    if (ip == NULL || ip[0] == '\0') {
        s_hook_gw_ip[0] = '\0';
        s_hook_gw_port = 0;
        return;
    }
    SDL_strlcpy(s_hook_gw_ip, ip, sizeof(s_hook_gw_ip));
    s_hook_gw_port = port;
}
#endif

/* Flags column of /proc/net/route. Values from the Linux UAPI header
 * include/uapi/linux/route.h:51-52 (v5.15):
 *   #define RTF_UP      0x0001   / * route usable        * /
 *   #define RTF_GATEWAY 0x0002   / * destination is a gateway * /
 * Defined locally rather than #include'ing <linux/route.h>, which
 * collides with <net/route.h> on several toolchains. */
#define NP_RTF_UP 0x0001u
#define NP_RTF_GATEWAY 0x0002u

/*
 * Linux default-gateway lookup via /proc/net/route.
 *
 * The file is produced by fib_route_seq_show() in net/ipv4/fib_trie.c
 * (v5.15:2946-3006), registered as "route" at fib_trie.c:3025. Its
 * columns, in order, are the header it prints at :2955-2957 —
 *   Iface  Destination  Gateway  Flags  RefCnt  Use  Metric  Mask  MTU
 *   Window  IRTT
 * — emitted by the seq_printf at :2984-2994 with the format
 *   "%s\t%08X\t%08X\t%04X\t%d\t%u\t%d\t%08X\t%d\t%u\t%u".
 *
 * BYTE ORDER, because guessing it is how this function goes subtly
 * wrong: Destination, Gateway and Mask are `__be32` values printed with
 * %08X, i.e. the kernel prints the machine's INTEGER READING of the
 * network-order bytes. Parsing that hex back into a uint32_t and storing
 * it into in_addr.s_addr therefore reproduces the original four bytes
 * exactly, on little- and big-endian alike, because it is the same
 * machine doing both halves. No htonl/ntohl belongs anywhere here — one
 * would break it on both endiannesses.
 *
 * A default route is Destination == 0 AND Mask == 0. RefCnt and Use are
 * printed as literal zeros (:2988) and are skipped.
 */
#if defined(__linux__)
static bool discover_gateway_platform(char* out_ip, int ip_buf_size) {
    FILE* f = fopen("/proc/net/route", "r");
    if (f == NULL) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "NAT-PMP: /proc/net/route unavailable (errno %d) — no gateway",
                    errno);
        return false;
    }

    char line[512];
    bool have = false;
    long best_metric = 0;
    uint32_t best_gw = 0;

    /* Header line (fib_trie.c:2955-2957). */
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return false;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char iface[64];
        unsigned long dest = 0, gw = 0, flags = 0, mask = 0;
        long metric = 0;
        if (sscanf(line, "%63s %lx %lx %lx %*u %*u %ld %lx", iface, &dest, &gw, &flags,
                   &metric, &mask) != 6) {
            continue;
        }
        if (dest != 0 || mask != 0) {
            continue; /* not the default route */
        }
        if ((flags & NP_RTF_UP) == 0 || (flags & NP_RTF_GATEWAY) == 0) {
            continue;
        }
        if (gw == 0) {
            continue;
        }
        if (!have || metric < best_metric) {
            have = true;
            best_metric = metric;
            best_gw = (uint32_t)gw;
        }
    }
    fclose(f);

    if (!have) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "NAT-PMP: no default IPv4 gateway in /proc/net/route");
        return false;
    }

    struct in_addr a;
    memset(&a, 0, sizeof(a));
    memcpy(&a.s_addr, &best_gw, sizeof(a.s_addr)); /* see BYTE ORDER above */
    if (inet_ntop(AF_INET, &a, out_ip, (np_socklen_t)ip_buf_size) == NULL) {
        return false;
    }
    return true;
}
#else
/*
 * Non-Linux hosts report NO GATEWAY, deliberately.
 *
 * The shipping target is MiSTer (Linux 5.15, single STMMAC GbE, IPv4
 * only), and /proc/net/route covers it. The only other machine this code
 * runs on is the developer's macOS box, where the alternative would be a
 * BSD sysctl(NET_RT_DUMP) route walk that no shipped configuration would
 * ever execute — untested code on the failure path of a feature that
 * mutates the LAN.
 *
 * The safety property is the point: with no gateway, a macOS test run
 * CANNOT emit a NAT-PMP packet at the developer's real router. Every
 * test reaches the client through Natpmp_TestHook_SetGateway and a
 * localhost mock instead.
 */
static bool discover_gateway_platform(char* out_ip, int ip_buf_size) {
    (void)out_ip;
    (void)ip_buf_size;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "NAT-PMP: gateway discovery is implemented for Linux only "
                "(/proc/net/route); this platform reports no gateway");
    return false;
}
#endif

static bool discover_gateway(char* out_ip, int ip_buf_size, uint16_t* out_port) {
    if (out_ip == NULL || ip_buf_size <= 0 || out_port == NULL) {
        return false;
    }
    out_ip[0] = '\0';
    *out_port = NATPMP_GATEWAY_PORT;
#ifdef NETPLAY_TEST_HOOKS
    if (s_hook_gw_ip[0] != '\0') {
        SDL_strlcpy(out_ip, s_hook_gw_ip, (size_t)ip_buf_size);
        *out_port = s_hook_gw_port != 0 ? s_hook_gw_port : NATPMP_GATEWAY_PORT;
        return true;
    }
#endif
    return discover_gateway_platform(out_ip, ip_buf_size);
}

#ifdef NETPLAY_TEST_HOOKS
bool Natpmp_TestHook_DiscoverGateway(char* out_ip, int ip_buf_size) {
    if (out_ip == NULL || ip_buf_size <= 0) {
        return false;
    }
    out_ip[0] = '\0';
    return discover_gateway_platform(out_ip, ip_buf_size);
}
#endif

/* ====================================================================== */
/* Bounded request / retransmit loop                                      */
/* ====================================================================== */

/*
 * DELIBERATE DEVIATION FROM RFC 6886 §3.1.
 *
 * The spec's ladder is: send, wait 250 ms, retransmit, wait 500 ms,
 * "with the interval between attempts doubling each time", up to a
 * ninth attempt followed by a 64-second wait before concluding the
 * gateway does not speak NAT-PMP. That is 250+500+...+64000 ms ≈ 127
 * seconds.
 *
 * This client runs on the host's "Host Game" click, immediately after a
 * UPnP attempt that is itself capped at 6 s of wall clock
 * (direct_p2p.c try_upnp). Two minutes of silent retransmission behind a
 * button press is not a behaviour this program can ship, and the
 * information gained after the third attempt is small: a gateway that
 * has ignored three requests over 1.75 s on a switched LAN is not going
 * to answer the fourth.
 *
 * So the ladder is TRUNCATED to its first three intervals — 250, 500,
 * 1000 ms, keeping the doubling shape — and every wait is additionally
 * clamped by an absolute wall-clock deadline the caller sets. The cost
 * of being wrong is one missed port mapping on a very slow gateway,
 * which degrades to exactly today's behaviour: fall through to STUN.
 */
static const int k_ladder_ms[] = { 250, 500, 1000 };
#define NP_LADDER_STEPS ((int)(sizeof(k_ladder_ms) / sizeof(k_ladder_ms[0])))

typedef enum {
    NP_TX_ANSWERED = 0, /* a verdict was reached (see *out_verdict) */
    NP_TX_TIMEOUT,      /* ladder or deadline exhausted, no answer  */
    NP_TX_ERROR,        /* socket-level failure; stop trying        */
} NpTxOutcome;

typedef NatpmpParse (*NpParseFn)(const uint8_t* buf, int len, void* ctx);

static int np_remaining_ms(uint64_t deadline_ms) {
    const uint64_t now = SDL_GetTicks();
    if (now >= deadline_ms) {
        return 0;
    }
    const uint64_t left = deadline_ms - now;
    return left > 2000000000u ? 2000000000 : (int)left;
}

/* Send `req`, then wait for an answer we accept, retransmitting on the
 * truncated §3.1 ladder and never running past `deadline_ms`.
 *
 * The socket is connect()ed to the gateway, so the kernel already
 * enforces RFC 6886 §3.2/§3.3's "client MUST check the source IP
 * address, and silently discard the packet if the address is not the
 * address of the gateway to which the request was sent" — datagrams from
 * anywhere else are never delivered here. Payload-level correlation
 * (nonce / opcode / internal port) is the parse callback's job, and a
 * NOT_OURS verdict does NOT end the wait: a frame we cannot attribute
 * must neither become a mapping nor consume our timeout. */
static NpTxOutcome np_transact(np_sock_t sock, const uint8_t* req, int req_len,
                               uint64_t deadline_ms, NpParseFn parse, void* ctx,
                               NatpmpParse* out_verdict) {
    for (int step = 0; step < NP_LADDER_STEPS; step++) {
        if (np_remaining_ms(deadline_ms) <= 0) {
            return NP_TX_TIMEOUT;
        }
#ifdef _WIN32
        const int sent = send(sock, (const char*)req, req_len, 0);
#else
        const ssize_t sent = send(sock, req, (size_t)req_len, 0);
#endif
        if (sent != (int)req_len) {
            /* On a connected UDP socket an "ICMP Port Unreachable" from
             * the gateway surfaces as an error on a later call. RFC 6886
             * §3.1: on that ICMP the client "can skip any remaining
             * retransmissions and conclude immediately that the gateway
             * does not support NAT-PMP". Any other send error is equally
             * terminal for this attempt. */
            return NP_TX_ERROR;
        }

        uint64_t step_deadline = SDL_GetTicks() + (uint64_t)k_ladder_ms[step];
        if (step_deadline > deadline_ms) {
            step_deadline = deadline_ms;
        }

        for (;;) {
            const int wait_ms = np_remaining_ms(step_deadline);
            if (wait_ms <= 0) {
                break; /* retransmit (or fall out of the ladder) */
            }
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            struct timeval tv;
            tv.tv_sec = wait_ms / 1000;
            tv.tv_usec = (wait_ms % 1000) * 1000;
            const int sel = select((int)sock + 1, &rfds, NULL, NULL, &tv);
            if (sel < 0) {
#ifndef _WIN32
                if (errno == EINTR) {
                    continue;
                }
#endif
                return NP_TX_ERROR;
            }
            if (sel == 0) {
                break; /* this rung elapsed */
            }
            uint8_t buf[512];
#ifdef _WIN32
            const int got = recv(sock, (char*)buf, (int)sizeof(buf), 0);
#else
            const ssize_t got = recv(sock, buf, sizeof(buf), 0);
#endif
            if (got <= 0) {
                /* See the send() comment: ECONNREFUSED here is the ICMP
                 * unreachable, and §3.1 says stop. */
                return NP_TX_ERROR;
            }
            const NatpmpParse verdict = parse(buf, (int)got, ctx);
            if (verdict == NATPMP_PARSE_NOT_OURS) {
                continue; /* keep waiting inside this rung */
            }
            *out_verdict = verdict;
            return NP_TX_ANSWERED;
        }
    }
    return NP_TX_TIMEOUT;
}

/* ====================================================================== */
/* Client                                                                 */
/* ====================================================================== */

typedef struct {
    uint8_t nonce[NATPMP_PCP_NONCE_LEN];
    uint16_t internal_port;
    NatpmpPcpMap map;
} PcpCtx;

static NatpmpParse pcp_parse_cb(const uint8_t* buf, int len, void* ctx) {
    PcpCtx* c = (PcpCtx*)ctx;
    return Natpmp_ParsePcpMapResponse(buf, len, c->nonce, NATPMP_PROTO_UDP,
                                      c->internal_port, &c->map);
}

typedef struct {
    NatpmpPmpAddr addr;
} PmpAddrCtx;

static NatpmpParse pmp_addr_parse_cb(const uint8_t* buf, int len, void* ctx) {
    PmpAddrCtx* c = (PmpAddrCtx*)ctx;
    return Natpmp_ParsePmpAddrResponse(buf, len, &c->addr);
}

typedef struct {
    uint16_t internal_port;
    NatpmpPmpMap map;
} PmpMapCtx;

static NatpmpParse pmp_map_parse_cb(const uint8_t* buf, int len, void* ctx) {
    PmpMapCtx* c = (PmpMapCtx*)ctx;
    return Natpmp_ParsePmpMapResponse(buf, len, NATPMP_PMP_OP_MAP_UDP,
                                      c->internal_port, &c->map);
}

/* Open a UDP socket connect()ed to the gateway and report the source
 * address the kernel picked for it. RFC 6887 §7.1 defines the PCP
 * Client's IP Address field as "The source IPv4 or IPv6 address in the
 * IP header used by the PCP client when sending this PCP request", so it
 * has to come from the socket, not from a guess about which interface is
 * "the" LAN interface. A mismatch is a diagnosable error (§7.4
 * ADDRESS_MISMATCH), not something to paper over. */
static np_sock_t open_gateway_socket(const char* gw_ip, uint16_t gw_port,
                                     uint32_t* out_client_ip_be) {
    struct sockaddr_in gw;
    memset(&gw, 0, sizeof(gw));
    gw.sin_family = AF_INET;
    gw.sin_port = htons(gw_port);
    if (inet_pton(AF_INET, gw_ip, &gw.sin_addr) != 1) {
        return NP_INVALID_SOCK;
    }

    np_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == NP_INVALID_SOCK) {
        return NP_INVALID_SOCK;
    }
    if (connect(s, (struct sockaddr*)&gw, (np_socklen_t)sizeof(gw)) != 0) {
        NP_CLOSE(s);
        return NP_INVALID_SOCK;
    }
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    np_socklen_t sl = (np_socklen_t)sizeof(local);
    if (getsockname(s, (struct sockaddr*)&local, &sl) != 0 || local.sin_family != AF_INET) {
        NP_CLOSE(s);
        return NP_INVALID_SOCK;
    }
    memcpy(out_client_ip_be, &local.sin_addr.s_addr, 4);
    return s;
}

static void fill_mapping(UpnpMapping* out, PortMapBackend backend,
                         uint16_t internal_port, uint16_t external_port,
                         uint32_t external_ip_be, uint32_t lifetime_s) {
    struct in_addr a;
    memset(&a, 0, sizeof(a));
    memcpy(&a.s_addr, &external_ip_be, 4);
    if (external_ip_be != 0) {
        inet_ntop(AF_INET, &a, out->external_ip, (np_socklen_t)sizeof(out->external_ip));
    }
    out->internal_port = internal_port;
    out->external_port = external_port;
    out->lifetime_s = lifetime_s;
    out->backend = backend;
    out->active = true;
}

bool Natpmp_AddMapping(UpnpMapping* out, uint16_t internal_port,
                       uint16_t suggested_external, PortMapBackend backend_hint,
                       int budget_ms) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (internal_port == 0) {
        return false;
    }

    char gw_ip[64] = { 0 };
    uint16_t gw_port = NATPMP_GATEWAY_PORT;
    if (!discover_gateway(gw_ip, (int)sizeof(gw_ip), &gw_port)) {
        return false;
    }

    uint32_t client_ip_be = 0;
    np_sock_t sock = open_gateway_socket(gw_ip, gw_port, &client_ip_be);
    if (sock == NP_INVALID_SOCK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "NAT-PMP: cannot open a UDP socket to gateway %s:%u", gw_ip,
                    (unsigned)gw_port);
        return false;
    }

    if (budget_ms <= 0) {
        budget_ms = NATPMP_PROBE_BUDGET_MS;
    }
    const uint64_t deadline_ms = SDL_GetTicks() + (uint64_t)budget_ms;
    bool try_pcp = (backend_hint == PORTMAP_BACKEND_NONE || backend_hint == PORTMAP_BACKEND_PCP);
    bool try_pmp = (backend_hint == PORTMAP_BACKEND_NONE || backend_hint == PORTMAP_BACKEND_NATPMP);
    bool ok = false;

    /* ---- PCP first. RFC 6887 Appendix A: "A client supporting both
     * NAT-PMP and PCP SHOULD send its request using the PCP packet
     * format." ---- */
    if (try_pcp) {
        PcpCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.internal_port = internal_port;
        if (!Csprng_Bytes(ctx.nonce, sizeof(ctx.nonce))) {
            /* RFC 6887 §18.1: the Mapping Nonce is what stops an off-path
             * attacker from deleting or hijacking our mapping. A
             * predictable one voids that, so fail closed — the same
             * policy stun.c and room_code.c already apply. */
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PCP: platform CSPRNG unavailable — refusing to send a MAP "
                         "request with a predictable Mapping Nonce (RFC 6887 §11.1)");
            try_pcp = false;
        } else {
            uint8_t req[NATPMP_PCP_MAP_LEN];
            Natpmp_BuildPcpMapRequest(req, ctx.nonce, NATPMP_PROTO_UDP, internal_port,
                                      suggested_external, 0, client_ip_be,
                                      NATPMP_LEASE_SECONDS);
            NatpmpParse verdict = NATPMP_PARSE_NOT_OURS;
            const NpTxOutcome tx = np_transact(sock, req, (int)sizeof(req), deadline_ms,
                                               pcp_parse_cb, &ctx, &verdict);
            if (tx == NP_TX_ANSWERED && verdict == NATPMP_PARSE_OK) {
                fill_mapping(out, PORTMAP_BACKEND_PCP, ctx.map.internal_port,
                             ctx.map.external_port, ctx.map.external_ip_be,
                             ctx.map.lifetime_s);
                SDL_Log("PCP: mapping granted (external %s:%u -> internal %u, lifetime %us)",
                        out->external_ip, (unsigned)out->external_port,
                        (unsigned)out->internal_port, (unsigned)out->lifetime_s);
                ok = true;
            } else if (tx == NP_TX_ANSWERED && verdict == NATPMP_PARSE_REFUSED) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "PCP: gateway %s refused the MAP request (result code %u, "
                            "RFC 6887 §7.4)",
                            gw_ip, (unsigned)ctx.map.result_code);
                /* An answered-and-refused PCP server is a PCP server.
                 * Falling back to NAT-PMP would ask the same box the same
                 * question in an older dialect. */
                try_pmp = false;
            } else if (tx == NP_TX_ANSWERED && verdict == NATPMP_PARSE_PCP_IS_NATPMP) {
                SDL_Log("PCP: gateway %s answered UNSUPP_VERSION with version 0 — it is a "
                        "NAT-PMP server (RFC 6887 §9), downgrading",
                        gw_ip);
                /* try_pmp stays as the hint set it. */
            } else if (tx == NP_TX_ERROR) {
                try_pmp = false; /* RFC 6886 §3.1 ICMP-unreachable shortcut */
            }
        }
    }

    /* ---- NAT-PMP fallback. Requests are issued SERIALLY: RFC 6886 §3.1,
     * "clients SHOULD NOT issue multiple concurrent requests ... it
     * SHOULD queue them and issue them serially, one at a time."
     *
     * The public-address request (§3.2) goes first for two reasons: it is
     * the cheapest possible liveness probe (2 bytes), and it is the ONLY
     * way to learn the external IP on this protocol — unlike PCP's MAP
     * response (§11.2), a NAT-PMP mapping response carries no external
     * address. That address is what the caller's CGNAT gate compares
     * against STUN, so a mapping without it would be a mapping the gate
     * cannot judge. ---- */
    if (!ok && try_pmp) {
        PmpAddrCtx actx;
        memset(&actx, 0, sizeof(actx));
        uint8_t areq[NATPMP_PMP_ADDR_REQ_LEN];
        Natpmp_BuildPmpAddrRequest(areq);
        NatpmpParse averdict = NATPMP_PARSE_NOT_OURS;
        const NpTxOutcome atx = np_transact(sock, areq, (int)sizeof(areq), deadline_ms,
                                            pmp_addr_parse_cb, &actx, &averdict);
        if (atx != NP_TX_ANSWERED || averdict != NATPMP_PARSE_OK) {
            if (atx == NP_TX_ANSWERED && averdict == NATPMP_PARSE_REFUSED) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "NAT-PMP: gateway %s refused the public-address request "
                            "(result code %u, RFC 6886 §3.5)",
                            gw_ip, (unsigned)actx.addr.result_code);
            } else {
                SDL_Log("NAT-PMP: gateway %s did not answer the public-address request "
                        "within budget — treating it as not NAT-PMP capable",
                        gw_ip);
            }
        } else {
            PmpMapCtx mctx;
            memset(&mctx, 0, sizeof(mctx));
            mctx.internal_port = internal_port;
            uint8_t mreq[NATPMP_PMP_MAP_REQ_LEN];
            Natpmp_BuildPmpMapRequest(mreq, NATPMP_PMP_OP_MAP_UDP, internal_port,
                                      suggested_external, NATPMP_LEASE_SECONDS);
            NatpmpParse mverdict = NATPMP_PARSE_NOT_OURS;
            const NpTxOutcome mtx = np_transact(sock, mreq, (int)sizeof(mreq), deadline_ms,
                                                pmp_map_parse_cb, &mctx, &mverdict);
            if (mtx == NP_TX_ANSWERED && mverdict == NATPMP_PARSE_OK &&
                mctx.map.external_port != 0) {
                fill_mapping(out, PORTMAP_BACKEND_NATPMP, mctx.map.internal_port,
                             mctx.map.external_port, actx.addr.external_ip_be,
                             mctx.map.lifetime_s);
                SDL_Log("NAT-PMP: mapping granted (external %s:%u -> internal %u, "
                        "lifetime %us)",
                        out->external_ip, (unsigned)out->external_port,
                        (unsigned)out->internal_port, (unsigned)out->lifetime_s);
                ok = true;
            } else if (mtx == NP_TX_ANSWERED && mverdict == NATPMP_PARSE_REFUSED) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "NAT-PMP: gateway %s refused the mapping (result code %u, "
                            "RFC 6886 §3.5)",
                            gw_ip, (unsigned)mctx.map.result_code);
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "NAT-PMP: gateway %s answered the address request but not the "
                            "mapping request within budget",
                            gw_ip);
            }
        }
    }

    NP_CLOSE(sock);
    if (!ok) {
        memset(out, 0, sizeof(*out)); /* fail closed: never a half-filled mapping */
    }
    return ok;
}

void Natpmp_RemoveMapping(UpnpMapping* mapping) {
    if (mapping == NULL || !mapping->active) {
        return;
    }
    const PortMapBackend backend = mapping->backend;
    const uint16_t internal_port = mapping->internal_port;
    /* Clear first: every exit below is a completed teardown from this
     * process's point of view, and RFC 6886 §3.4 makes the router-side
     * half advisory ("it is always possible for client software to
     * crash ... NAT gateways already need to cope with this case"). */
    mapping->active = false;

    if (backend != PORTMAP_BACKEND_NATPMP && backend != PORTMAP_BACKEND_PCP) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "NAT-PMP: refusing to remove a mapping owned by backend %d",
                    (int)backend);
        return;
    }

    char gw_ip[64] = { 0 };
    uint16_t gw_port = NATPMP_GATEWAY_PORT;
    if (!discover_gateway(gw_ip, (int)sizeof(gw_ip), &gw_port)) {
        return;
    }
    uint32_t client_ip_be = 0;
    np_sock_t sock = open_gateway_socket(gw_ip, gw_port, &client_ip_be);
    if (sock == NP_INVALID_SOCK) {
        return;
    }

    /* RFC 6886 §3.1: deletion requests "are in some sense advisory ... it
     * may be acceptable for a client to retry only once or twice before
     * giving up ... but a client SHOULD always send at least one deletion
     * request whenever possible". One ladder rung is that. */
    const uint64_t deadline_ms = SDL_GetTicks() + (uint64_t)k_ladder_ms[0];

    if (backend == PORTMAP_BACKEND_PCP) {
        /* RFC 6887 §11.1: "Requested lifetime ... The value 0 indicates
         * 'delete'." A fresh nonce is fine — §11.1 calls the nonce a
         * random value chosen by the client, and the delete is
         * self-identifying via protocol + internal port. If the CSPRNG is
         * down we still send: an all-zero nonce is "a legal value" (§11.1)
         * and losing the teardown is worse than a predictable delete of a
         * mapping we are abandoning anyway. */
        uint8_t nonce[NATPMP_PCP_NONCE_LEN];
        if (!Csprng_Bytes(nonce, sizeof(nonce))) {
            memset(nonce, 0, sizeof(nonce));
        }
        uint8_t req[NATPMP_PCP_MAP_LEN];
        Natpmp_BuildPcpMapRequest(req, nonce, NATPMP_PROTO_UDP, internal_port, 0, 0,
                                  client_ip_be, 0u);
        PcpCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        memcpy(ctx.nonce, nonce, sizeof(nonce));
        ctx.internal_port = internal_port;
        NatpmpParse verdict = NATPMP_PARSE_NOT_OURS;
        (void)np_transact(sock, req, (int)sizeof(req), deadline_ms, pcp_parse_cb, &ctx,
                          &verdict);
        SDL_Log("PCP: delete sent for internal port %u", (unsigned)internal_port);
    } else {
        /* RFC 6886 §3.4: lifetime 0, and the builder forces Suggested
         * External Port to 0 as the section requires. */
        uint8_t req[NATPMP_PMP_MAP_REQ_LEN];
        Natpmp_BuildPmpMapRequest(req, NATPMP_PMP_OP_MAP_UDP, internal_port, 0, 0u);
        PmpMapCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.internal_port = internal_port;
        NatpmpParse verdict = NATPMP_PARSE_NOT_OURS;
        (void)np_transact(sock, req, (int)sizeof(req), deadline_ms, pmp_map_parse_cb, &ctx,
                          &verdict);
        SDL_Log("NAT-PMP: delete sent for internal port %u", (unsigned)internal_port);
    }

    NP_CLOSE(sock);
}
