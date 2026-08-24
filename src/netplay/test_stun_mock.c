/*
 * test_stun_mock.c — Step 12 test harness for docs/plan-stun-direct-p2p.md.
 *
 * Covers two things in one TU:
 *
 *   (1) STUN wire-protocol round-trip against a mock server bound on
 *       127.0.0.1. The test spawns a peer thread that waits for a STUN
 *       Binding Request (RFC 5389: 20-byte header, magic cookie
 *       0x2112A442, 12-byte transaction ID), then responds with a
 *       Binding Response containing an XOR-MAPPED-ADDRESS attribute
 *       claiming the client is at 1.2.3.4:55555. The test driver
 *       builds its own Binding Request (so this TU does not need to
 *       link the hidden stun.c internals — build_binding_request /
 *       parse_binding_response are static there) and parses the
 *       returned XOR-MAPPED-ADDRESS to verify public_ip + public_port
 *       match what the mock server encoded. This is intentionally a
 *       protocol conformance test, not a live-network test — it runs
 *       in ~1 second with no external dependency.
 *
 *   (2) Stun_EncodeEndpoint / Stun_DecodeEndpoint round-trip. These
 *       are the public endpoint-codec primitives exported from
 *       stun.h (the 3-tuple "ip|public_port|local_port" pipe format).
 *       Plan §Step 12 explicitly names this round-trip as a valid
 *       fallback unit test; we run it alongside the wire-protocol
 *       test so one harness covers both surfaces.
 *
 * Gated behind ENABLE_NETPLAY_TESTS; under #else the entry point
 * prints "not compiled in" and returns 2. Mirrors the pattern used by
 * test_mist_handshake.c and test_room_code.c.
 *
 * Enable with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS='-DENABLE_NETPLAY_TESTS -DNETPLAY_TEST_HOOKS'"
 * (NETPLAY_TEST_HOOKS is required because this build links every
 * src/netplay/test_*.c TU, including test_bilateral_punch.c, which needs
 * that macro to declare DirectP2P_TestHook_IsLanPeer.)
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/connect_fail.h"
#include "netplay/net_tuning.h"
#include "netplay/stun.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((in_addr_t)0x7F000001)
#endif

#define STUN_BINDING_REQUEST 0x0001
#define STUN_BINDING_RESPONSE 0x0101
#define STUN_MAGIC_COOKIE 0x2112A442
#define STUN_ATTR_XOR_MAPPED_ADDRESS 0x0020

/* The mapped endpoint the mock server will claim the client sits at.
 * Arbitrary non-loopback IP so the test cannot accidentally pass by
 * the parser defaulting to 127.0.0.1. */
#define MOCK_MAPPED_IP_A 1
#define MOCK_MAPPED_IP_B 2
#define MOCK_MAPPED_IP_C 3
#define MOCK_MAPPED_IP_D 4
#define MOCK_MAPPED_PORT 55555

static int fail_count = 0;

static void fail(const char* tag, const char* why) {
    fprintf(stderr, "[test_stun_mock] FAIL: %s: %s\n", tag, why);
    fail_count++;
}

static int open_udp_on_localhost(unsigned short* out_port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }
    socklen_t sl = sizeof(a);
    getsockname(s, (struct sockaddr*)&a, &sl);
    *out_port = ntohs(a.sin_port);
    return s;
}

static void close_sock(int s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

/* Write a 20-byte STUN Binding Request header with the given transaction
 * ID. Zero body length (no attributes). Matches the layout that
 * stun.c's build_binding_request emits. */
static void build_request(uint8_t out[20], uint8_t txid[12]) {
    out[0] = 0x00; out[1] = 0x01;  /* type: Binding Request */
    out[2] = 0x00; out[3] = 0x00;  /* body length: 0 */
    out[4] = 0x21; out[5] = 0x12;  /* magic cookie */
    out[6] = 0xA4; out[7] = 0x42;
    for (int i = 0; i < 12; i++) {
        txid[i] = (uint8_t)(i * 17 + 3); /* deterministic for the test */
        out[8 + i] = txid[i];
    }
}

/*
 * Build a STUN Binding Response carrying an XOR-MAPPED-ADDRESS (IPv4)
 * attribute. Mirrors the wire format defined in RFC 5389 §15.2.
 *
 * Layout:
 *   hdr:        20 bytes (type=0x0101, len=12, cookie, txid)
 *   attr type:   2 bytes (0x0020)
 *   attr len:    2 bytes (8)
 *   reserved:    1 byte (0)
 *   family:      1 byte (0x01 for IPv4)
 *   xport:       2 bytes (port XOR cookie[0..1])
 *   xaddr:       4 bytes (addr XOR cookie)
 */
static int build_response_ex(uint8_t out[36], const uint8_t txid[12], uint16_t mapped_port);

static int build_response(uint8_t out[36], const uint8_t txid[12]) {
    return build_response_ex(out, txid, MOCK_MAPPED_PORT);
}

static int build_response_ex(uint8_t out[36], const uint8_t txid[12], uint16_t mapped_port) {
    /* Header */
    out[0] = 0x01; out[1] = 0x01;  /* type: Binding Response */
    out[2] = 0x00; out[3] = 0x0C;  /* body length: 12 bytes (one attr, 4+8) */
    out[4] = 0x21; out[5] = 0x12;  /* magic cookie */
    out[6] = 0xA4; out[7] = 0x42;
    memcpy(&out[8], txid, 12);

    /* Attribute: XOR-MAPPED-ADDRESS, length 8 (IPv4 family) */
    out[20] = 0x00; out[21] = 0x20;
    out[22] = 0x00; out[23] = 0x08;
    out[24] = 0x00;                 /* reserved */
    out[25] = 0x01;                 /* family: IPv4 */

    /* xport = port (host order) XOR top 16 bits of cookie, written BE */
    const uint16_t xport = mapped_port
                         ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
    out[26] = (uint8_t)(xport >> 8);
    out[27] = (uint8_t)(xport & 0xFF);

    /* xaddr = addr-bytes-in-network-order XOR cookie-bytes-in-network-order.
     * The stun.c parser reconstructs the address via
     *   decoded_ip = byteswap(xaddr ^ STUN_MAGIC_COOKIE)
     * and then reads (b[0], b[1], b[2], b[3]) as dotted quads from
     * the resulting 32-bit big-endian number. To produce octets
     * (1,2,3,4) on the wire we therefore set:
     *   native32 = (1<<24) | (2<<16) | (3<<8) | 4
     *   xaddr_native = native32 XOR cookie
     * then emit xaddr_native in big-endian. */
    const uint32_t native = ((uint32_t)MOCK_MAPPED_IP_A << 24)
                          | ((uint32_t)MOCK_MAPPED_IP_B << 16)
                          | ((uint32_t)MOCK_MAPPED_IP_C << 8)
                          | (uint32_t)MOCK_MAPPED_IP_D;
    const uint32_t xaddr = native ^ (uint32_t)STUN_MAGIC_COOKIE;
    out[28] = (uint8_t)(xaddr >> 24);
    out[29] = (uint8_t)(xaddr >> 16);
    out[30] = (uint8_t)(xaddr >> 8);
    out[31] = (uint8_t)(xaddr & 0xFF);

    return 32; /* 20 hdr + 12 attr body */
}

/* Decode XOR-MAPPED-ADDRESS from a Binding Response. Returns true on
 * success. Kept intentionally minimal — only the subset the mock
 * server emits (single IPv4 XOR-MAPPED-ADDRESS). */
static bool parse_response(const uint8_t* buf, int len,
                           const uint8_t* txid,
                           uint8_t out_octets[4], uint16_t* out_port) {
    if (len < 32) return false;

    uint16_t msg_type = ((uint16_t)buf[0] << 8) | buf[1];
    if (msg_type != STUN_BINDING_RESPONSE) return false;

    uint16_t body_len = ((uint16_t)buf[2] << 8) | buf[3];
    if (20 + (int)body_len > len) return false;

    uint32_t cookie = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16)
                    | ((uint32_t)buf[6] << 8)  | (uint32_t)buf[7];
    if (cookie != STUN_MAGIC_COOKIE) return false;

    if (memcmp(&buf[8], txid, 12) != 0) return false;

    int o = 20;
    while (o + 4 <= 20 + (int)body_len) {
        uint16_t at = ((uint16_t)buf[o] << 8) | buf[o + 1];
        uint16_t al = ((uint16_t)buf[o + 2] << 8) | buf[o + 3];
        o += 4;
        if (o + al > 20 + (int)body_len) break;

        if (at == STUN_ATTR_XOR_MAPPED_ADDRESS && al >= 8 && buf[o + 1] == 0x01) {
            uint16_t xport = ((uint16_t)buf[o + 2] << 8) | buf[o + 3];
            *out_port = xport ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);

            uint32_t xaddr = ((uint32_t)buf[o + 4] << 24) | ((uint32_t)buf[o + 5] << 16)
                           | ((uint32_t)buf[o + 6] << 8)  | (uint32_t)buf[o + 7];
            uint32_t native = xaddr ^ (uint32_t)STUN_MAGIC_COOKIE;
            out_octets[0] = (uint8_t)(native >> 24);
            out_octets[1] = (uint8_t)(native >> 16);
            out_octets[2] = (uint8_t)(native >> 8);
            out_octets[3] = (uint8_t)(native & 0xFF);
            return true;
        }
        o += (al + 3) & ~3;
    }
    return false;
}

/* Mock server thread: awaits a single Binding Request, echoes back a
 * canned Binding Response. Exits after one exchange or 2-second timeout. */
typedef struct {
    int sock;
    volatile bool stop;
} ServerCtx;

static int SDLCALL server_thread(void* arg) {
    ServerCtx* ctx = (ServerCtx*)arg;
    const long long start = (long long)time(NULL);

    for (;;) {
        if (ctx->stop) return 0;
        if ((long long)time(NULL) - start > 2) return 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->sock, &rfds);
        struct timeval tv = { 0, 50 * 1000 };
        const int rc = select(ctx->sock + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) continue;

        uint8_t req[64];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        const int n = (int)recvfrom(ctx->sock, (char*)req, sizeof(req), 0,
                                    (struct sockaddr*)&src, &sl);
        if (n < 20) continue; /* too small for a STUN header */

        /* Validate it's actually a STUN Binding Request before replying. */
        uint16_t msg_type = ((uint16_t)req[0] << 8) | req[1];
        uint32_t cookie = ((uint32_t)req[4] << 24) | ((uint32_t)req[5] << 16)
                        | ((uint32_t)req[6] << 8)  | (uint32_t)req[7];
        if (msg_type != STUN_BINDING_REQUEST || cookie != STUN_MAGIC_COOKIE) {
            continue;
        }

        /* Echo the client's transaction ID back inside a Binding
         * Response. This is how a real STUN server behaves and is
         * what the client checks to correlate replies. */
        uint8_t txid[12];
        memcpy(txid, &req[8], 12);

        uint8_t reply[64];
        int rl = build_response(reply, txid);
        sendto(ctx->sock, (const char*)reply, rl, 0,
               (struct sockaddr*)&src, sl);
        return 0; /* one exchange is enough */
    }
}

static int run_wire_test(void) {
    unsigned short server_port = 0, client_port = 0;
    int server_sock = open_udp_on_localhost(&server_port);
    int client_sock = open_udp_on_localhost(&client_port);
    if (server_sock < 0 || client_sock < 0) {
        fail("wire", "failed to bind localhost UDP sockets");
        if (server_sock >= 0) close_sock(server_sock);
        if (client_sock >= 0) close_sock(client_sock);
        return 1;
    }

    ServerCtx ctx = { server_sock, false };
    SDL_Thread* tid = SDL_CreateThread(server_thread, "stun_mock_srv", &ctx);
    if (!tid) {
        fail("wire", "SDL_CreateThread failed");
        close_sock(server_sock);
        close_sock(client_sock);
        return 1;
    }

    /* Send a Binding Request to the mock server. */
    uint8_t req[20];
    uint8_t txid[12];
    build_request(req, txid);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(server_port);

    if (sendto(client_sock, (const char*)req, 20, 0,
               (struct sockaddr*)&dst, sizeof(dst)) != 20) {
        fail("wire", "sendto to mock server failed");
        ctx.stop = true;
        SDL_WaitThread(tid, NULL);
        close_sock(server_sock);
        close_sock(client_sock);
        return 1;
    }

    /* Wait up to ~1s for the response. */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(client_sock, &rfds);
    struct timeval tv = { 1, 0 };
    const int rc = select(client_sock + 1, &rfds, NULL, NULL, &tv);
    if (rc <= 0) {
        fail("wire", "timeout waiting for mock server response");
        ctx.stop = true;
        SDL_WaitThread(tid, NULL);
        close_sock(server_sock);
        close_sock(client_sock);
        return 1;
    }

    uint8_t resp[128];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const int n = (int)recvfrom(client_sock, (char*)resp, sizeof(resp), 0,
                                (struct sockaddr*)&from, &fl);
    ctx.stop = true;
    SDL_WaitThread(tid, NULL);
    close_sock(server_sock);
    close_sock(client_sock);

    if (n < 32) {
        fail("wire", "short response from mock server");
        return 1;
    }

    uint8_t octets[4] = { 0, 0, 0, 0 };
    uint16_t port = 0;
    if (!parse_response(resp, n, txid, octets, &port)) {
        fail("wire", "failed to parse XOR-MAPPED-ADDRESS");
        return 1;
    }

    /* Also drive the PRODUCTION parser (public since S1 via
     * Stun_ParseBindingResponse) over the same RFC 5389 §15.2 response.
     * Regression for the S2 byte-order fix: the parser used to apply
     * SDL_Swap16BE to the already-native port value, byteswapping every
     * mapped port on little-endian hosts (55555 = 0xD903 came back as
     * 0x03D9 = 985) — so every non-UPnP room code advertised a wrong
     * port unless the port was a palindrome. The IPv4 branch was
     * unaffected (its swap is cancelled by re-reading through memory
     * bytes). */
    {
        char prod_ip[64] = { 0 };
        uint16_t prod_port = 0;
        if (!Stun_ParseBindingResponse(resp, n, txid, prod_ip, sizeof(prod_ip), &prod_port)) {
            fail("wire", "production Stun_ParseBindingResponse rejected a valid response");
            return 1;
        }
        if (prod_port != MOCK_MAPPED_PORT) {
            fprintf(stderr,
                    "[test_stun_mock] FAIL: wire: production parser returned port %u, expected %u "
                    "(byteswapped? %u)\n",
                    prod_port, MOCK_MAPPED_PORT,
                    (unsigned)((MOCK_MAPPED_PORT >> 8) | ((MOCK_MAPPED_PORT & 0xFF) << 8)));
            fail_count++;
            return 1;
        }
        if (strcmp(prod_ip, "1.2.3.4") != 0) {
            fprintf(stderr, "[test_stun_mock] FAIL: wire: production parser returned ip %s, expected 1.2.3.4\n",
                    prod_ip);
            fail_count++;
            return 1;
        }
    }

    if (octets[0] != MOCK_MAPPED_IP_A || octets[1] != MOCK_MAPPED_IP_B
     || octets[2] != MOCK_MAPPED_IP_C || octets[3] != MOCK_MAPPED_IP_D
     || port != MOCK_MAPPED_PORT) {
        fprintf(stderr,
                "[test_stun_mock] FAIL: wire: got %u.%u.%u.%u:%u, expected %u.%u.%u.%u:%u\n",
                octets[0], octets[1], octets[2], octets[3], port,
                MOCK_MAPPED_IP_A, MOCK_MAPPED_IP_B, MOCK_MAPPED_IP_C, MOCK_MAPPED_IP_D,
                MOCK_MAPPED_PORT);
        fail_count++;
        return 1;
    }

    fprintf(stderr,
            "[test_stun_mock] wire OK — parsed %u.%u.%u.%u:%u from mock XOR-MAPPED-ADDRESS\n",
            octets[0], octets[1], octets[2], octets[3], port);
    return 0;
}

/* Exercise the public endpoint codec. Plan §Step 12 allows this as a
 * standalone unit test; we run it here alongside the wire test. */
static int run_codec_test(void) {
    struct {
        const char* tag;
        const char* ip;
        uint16_t public_port;
        uint16_t local_port;
    } cases[] = {
        { "loopback",   "127.0.0.1",      54321, 54321 },
        { "public-v4",  "1.2.3.4",        55555, 54321 },
        { "high-port",  "8.8.8.8",        65535, 1     },
        { "low-port",   "203.0.113.77",   1,     65535 },
        { "equal",      "10.0.0.1",       34567, 34567 },
    };
    const int N = (int)(sizeof(cases) / sizeof(cases[0]));
    int fails_here = 0;

    for (int i = 0; i < N; i++) {
        char code[64] = { 0 };
        Stun_EncodeEndpoint(cases[i].ip, cases[i].public_port, cases[i].local_port, code);
        if (code[0] == '\0') {
            fprintf(stderr, "[test_stun_mock] FAIL: %s: encode produced empty string\n",
                    cases[i].tag);
            fails_here++;
            continue;
        }

        char dip[64] = { 0 };
        uint16_t dpp = 0, dlp = 0;
        if (!Stun_DecodeEndpoint(code, dip, &dpp, &dlp)) {
            fprintf(stderr, "[test_stun_mock] FAIL: %s: decode rejected \"%s\"\n",
                    cases[i].tag, code);
            fails_here++;
            continue;
        }
        if (strcmp(dip, cases[i].ip) != 0
         || dpp != cases[i].public_port
         || dlp != cases[i].local_port) {
            fprintf(stderr,
                    "[test_stun_mock] FAIL: %s: round-trip mismatch "
                    "code=\"%s\" ip=%s/%s pp=%u/%u lp=%u/%u\n",
                    cases[i].tag, code, dip, cases[i].ip,
                    dpp, cases[i].public_port, dlp, cases[i].local_port);
            fails_here++;
            continue;
        }
        fprintf(stderr, "[test_stun_mock] codec %s OK — \"%s\"\n", cases[i].tag, code);
    }

    /* Reject bogus input: no pipe separator. */
    {
        char dip[64] = { 0 };
        uint16_t dpp = 0, dlp = 0;
        if (Stun_DecodeEndpoint("garbage-no-pipes", dip, &dpp, &dlp)) {
            fprintf(stderr, "[test_stun_mock] FAIL: codec: bogus input accepted\n");
            fails_here++;
        } else {
            fprintf(stderr, "[test_stun_mock] codec bogus-reject OK\n");
        }
    }

    /* NULL code must be rejected. */
    {
        char dip[64] = { 0 };
        uint16_t dpp = 0, dlp = 0;
        if (Stun_DecodeEndpoint(NULL, dip, &dpp, &dlp)) {
            fprintf(stderr, "[test_stun_mock] FAIL: codec: NULL code accepted\n");
            fails_here++;
        }
    }

    if (fails_here > 0) {
        fail_count += fails_here;
        return 1;
    }
    return 0;
}

/* --- Hole-punch retarget regression (S2) ------------------------------- */

/*
 * S2 retarget fix (docs/plan-netplay-connection.md §4): when a punch
 * datagram arrives from a TRANSLATED source port (symmetric-NAT peer),
 * Stun_HolePunch must (a) update *peer_port to the observed port and
 * (b) send its confirmation punches to THAT port — not the original
 * port captured at function entry. Pre-fix, (a) happened but (b) did
 * not, so the symmetric peer never received a confirmation and its own
 * punch timed out.
 *
 * Simulation on localhost:
 *   - `local`: a real SDL3_net datagram socket (the puncher).
 *   - `wrong`: a bound throwaway that plays the stale/advertised port —
 *     the puncher initially targets it; it never answers. Post-fix it
 *     must stop receiving once the retarget happens.
 *   - `mock peer`: a raw UDP socket on a DIFFERENT port that sends
 *     "3SX_PUNCH" to the puncher (same source IP 127.0.0.1, translated
 *     port — exactly what a symmetric peer's punch looks like) and then
 *     counts the punch datagrams it receives back.
 *
 * PASS requires the mock peer to receive >= 1 punch datagram. Pre-fix
 * this test FAILS: every send goes to `wrong`, the mock peer receives
 * nothing (verified against the pre-S2 tree).
 */

/* Resolve the OS-assigned local port of an SDL3_net datagram socket via
 * the net_tuning.h layout mirror (same technique as stun.c). */
static uint16_t sdlnet_local_port(NET_DatagramSocket* sock) {
    const NetTuningDgramMirror* m = (const NetTuningDgramMirror*)sock;
    for (int h = 0; h < m->num_handles; h++) {
        struct sockaddr_storage sa;
        socklen_t sl = sizeof(sa);
        if (getsockname((int)m->handles[h].handle, (struct sockaddr*)&sa, &sl) == 0) {
            if (sa.ss_family == AF_INET) {
                return ntohs(((struct sockaddr_in*)&sa)->sin_port);
            }
        }
    }
    return 0;
}

typedef struct {
    int sock;                 /* mock peer's raw UDP socket */
    uint16_t target_port;     /* the puncher's local port */
    volatile bool stop;
    volatile int punches_received; /* "3SX_PUNCH" datagrams seen by the peer */
} PunchPeerCtx;

static int SDLCALL punch_peer_thread(void* arg) {
    PunchPeerCtx* ctx = (PunchPeerCtx*)arg;
    const char punch_msg[] = "3SX_PUNCH";

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(ctx->target_port);

    uint32_t last_send = 0;
    while (!ctx->stop) {
        const uint32_t now = SDL_GetTicks();
        if (last_send == 0 || now - last_send >= 30) {
            sendto(ctx->sock, punch_msg, strlen(punch_msg), 0,
                   (struct sockaddr*)&dst, sizeof(dst));
            last_send = now;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->sock, &rfds);
        struct timeval tv = { 0, 10 * 1000 };
        if (select(ctx->sock + 1, &rfds, NULL, NULL, &tv) > 0) {
            uint8_t buf[64];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            const int n = (int)recvfrom(ctx->sock, (char*)buf, sizeof(buf), 0,
                                        (struct sockaddr*)&from, &fl);
            if (n == (int)strlen(punch_msg) &&
                memcmp(buf, punch_msg, (size_t)n) == 0) {
                ctx->punches_received++;
            }
        }
    }
    return 0;
}

static int run_punch_retarget_test(void) {
    fprintf(stderr, "[test_stun_mock] retarget: punch from a translated port must retarget sends\n");

    if (!NET_Init()) {
        fail("retarget", "NET_Init failed");
        return 1;
    }

    /* Throwaway socket standing in for the stale advertised port. */
    unsigned short wrong_port = 0;
    int wrong_sock = open_udp_on_localhost(&wrong_port);
    /* Mock peer at its own ("translated") port. */
    unsigned short peer_port_bound = 0;
    int peer_sock = open_udp_on_localhost(&peer_port_bound);
    if (wrong_sock < 0 || peer_sock < 0) {
        fail("retarget", "failed to bind localhost UDP sockets");
        if (wrong_sock >= 0) close_sock(wrong_sock);
        if (peer_sock >= 0) close_sock(peer_sock);
        return 1;
    }

    /* Puncher socket — same construction as Stun_Discover (IPv4 wildcard). */
    NET_Address* bind_addr = NET_ResolveHostname("0.0.0.0");
    if (bind_addr) {
        int wait = 0;
        while (NET_GetAddressStatus(bind_addr) == NET_WAITING && wait < 100) {
            SDL_Delay(1);
            wait++;
        }
    }
    NET_DatagramSocket* punch_sock = NET_CreateDatagramSocket(bind_addr, 0);
    if (bind_addr) NET_UnrefAddress(bind_addr);
    if (punch_sock == NULL) {
        fail("retarget", "NET_CreateDatagramSocket failed");
        close_sock(wrong_sock);
        close_sock(peer_sock);
        return 1;
    }
    const uint16_t punch_local_port = sdlnet_local_port(punch_sock);
    if (punch_local_port == 0) {
        fail("retarget", "could not resolve puncher's local port");
        NET_DestroyDatagramSocket(punch_sock);
        close_sock(wrong_sock);
        close_sock(peer_sock);
        return 1;
    }

    StunResult local;
    memset(&local, 0, sizeof(local));
    local.socket = punch_sock;

    PunchPeerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = peer_sock;
    ctx.target_port = punch_local_port;

    SDL_Thread* tid = SDL_CreateThread(punch_peer_thread, "punch_peer", &ctx);
    if (!tid) {
        fail("retarget", "SDL_CreateThread failed");
        NET_DestroyDatagramSocket(punch_sock);
        close_sock(wrong_sock);
        close_sock(peer_sock);
        return 1;
    }

    /* Punch toward the WRONG (stale) port; the peer's actual punches
     * arrive from peer_port_bound. */
    char peer_ip[64];
    SDL_strlcpy(peer_ip, "127.0.0.1", sizeof(peer_ip));
    uint16_t observed_port = wrong_port;
    const bool punched = Stun_HolePunch(&local, peer_ip, &observed_port, 3000, NULL);

    /* Give the confirmation burst a moment to land, then stop the peer. */
    SDL_Delay(100);
    ctx.stop = true;
    SDL_WaitThread(tid, NULL);

    int rc = 0;
    if (!punched) {
        fail("retarget", "Stun_HolePunch did not accept the translated-port punch");
        rc = 1;
    }
    if (observed_port != peer_port_bound) {
        fprintf(stderr,
                "[test_stun_mock] FAIL: retarget: *peer_port=%u, expected observed source %u\n",
                (unsigned)observed_port, (unsigned)peer_port_bound);
        fail_count++;
        rc = 1;
    }
    if (ctx.punches_received < 1) {
        fail("retarget",
             "peer at the translated port received ZERO confirmation punches — "
             "sends still target the stale endpoint (pre-S2 bug)");
        rc = 1;
    }

    if (rc == 0) {
        fprintf(stderr,
                "[test_stun_mock] retarget OK — peer_port %u -> %u, %d confirmations "
                "reached the translated endpoint\n",
                (unsigned)wrong_port, (unsigned)peer_port_bound, ctx.punches_received);
    }

    NET_DestroyDatagramSocket(punch_sock);
    close_sock(wrong_sock);
    close_sock(peer_sock);
    return rc;
}

/* --- S2 parallel Stun_Discover tests ----------------------------------- */

#ifdef NETPLAY_TEST_HOOKS

/*
 * These drive the REAL Stun_Discover against in-process mock STUN
 * servers on 127.0.0.1, installed via the Stun_TestHook_SetServers
 * seam (which also disables the production numeric-IP fallback list so
 * the endpoint set is exactly what each test configures).
 */

/* Persistent mock STUN server: answers every valid Binding Request with
 * an XOR-MAPPED-ADDRESS response claiming `mapped_port`, optionally
 * ignoring the first `ignore_first` requests (packet-loss simulation).
 * Runs until stop. */
typedef struct {
    int sock;
    volatile bool stop;
    int ignore_first;
    uint16_t mapped_port;
    volatile int requests_seen;
} StunSrvCtx;

static int SDLCALL stun_srv_thread(void* arg) {
    StunSrvCtx* ctx = (StunSrvCtx*)arg;
    while (!ctx->stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->sock, &rfds);
        struct timeval tv = { 0, 20 * 1000 };
        if (select(ctx->sock + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        uint8_t req[64];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        const int n = (int)recvfrom(ctx->sock, (char*)req, sizeof(req), 0,
                                    (struct sockaddr*)&src, &sl);
        if (n < 20) continue;
        const uint16_t msg_type = ((uint16_t)req[0] << 8) | req[1];
        const uint32_t cookie = ((uint32_t)req[4] << 24) | ((uint32_t)req[5] << 16)
                              | ((uint32_t)req[6] << 8)  | (uint32_t)req[7];
        if (msg_type != STUN_BINDING_REQUEST || cookie != STUN_MAGIC_COOKIE) continue;

        ctx->requests_seen++;
        if (ctx->requests_seen <= ctx->ignore_first) continue; /* simulated loss */

        uint8_t txid[12];
        memcpy(txid, &req[8], 12);
        uint8_t reply[64];
        const int rl = build_response_ex(reply, txid, ctx->mapped_port);
        sendto(ctx->sock, (const char*)reply, rl, 0, (struct sockaddr*)&src, sl);
    }
    return 0;
}

/* One dead server (bound, never answers) + one live server: discovery
 * must succeed fast off the live one instead of serially burning ~2 s
 * on the dead one first (pre-S2 behavior), and must retain the LIVE
 * server for S1 keepalives. */
static int run_discover_parallel_test(void) {
    fprintf(stderr, "[test_stun_mock] discover-parallel: one dead server must not stall discovery\n");

    unsigned short dead_port = 0, live_port = 0;
    int dead_sock = open_udp_on_localhost(&dead_port);
    int live_sock = open_udp_on_localhost(&live_port);
    if (dead_sock < 0 || live_sock < 0) {
        fail("discover-parallel", "failed to bind localhost UDP sockets");
        if (dead_sock >= 0) close_sock(dead_sock);
        if (live_sock >= 0) close_sock(live_sock);
        return 1;
    }

    StunSrvCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = live_sock;
    ctx.mapped_port = 47001;
    SDL_Thread* tid = SDL_CreateThread(stun_srv_thread, "stun_srv_live", &ctx);
    if (!tid) {
        fail("discover-parallel", "SDL_CreateThread failed");
        close_sock(dead_sock);
        close_sock(live_sock);
        return 1;
    }

    static StunServerDesc servers[2];
    servers[0] = (StunServerDesc){ "127.0.0.1", 0 };
    servers[0].port = dead_port;   /* listed FIRST — serial code would stall here */
    servers[1] = (StunServerDesc){ "127.0.0.1", 0 };
    servers[1].port = live_port;
    Stun_TestHook_SetServers(servers, 2);

    StunResult res;
    const uint32_t t0 = SDL_GetTicks();
    const bool ok = Stun_Discover(&res, 0, 4000);
    const uint32_t elapsed = SDL_GetTicks() - t0;

    Stun_TestHook_SetServers(NULL, 0);
    ctx.stop = true;
    SDL_WaitThread(tid, NULL);

    int rc = 0;
    if (!ok) {
        fail("discover-parallel", "Stun_Discover failed with a live server present");
        rc = 1;
    } else {
        if (elapsed >= 1500) {
            fprintf(stderr,
                    "[test_stun_mock] FAIL: discover-parallel: took %u ms — dead server was "
                    "probed serially, not in parallel\n", (unsigned)elapsed);
            fail_count++;
            rc = 1;
        }
        if (strcmp(res.public_ip, "1.2.3.4") != 0 || res.public_port != 47001) {
            fprintf(stderr, "[test_stun_mock] FAIL: discover-parallel: got %s:%u, expected 1.2.3.4:47001\n",
                    res.public_ip, (unsigned)res.public_port);
            fail_count++;
            rc = 1;
        }
        if (res.server_port != live_port) {
            fprintf(stderr,
                    "[test_stun_mock] FAIL: discover-parallel: retained server port %u, expected the "
                    "LIVE server %u (S1 keepalive target)\n",
                    (unsigned)res.server_port, (unsigned)live_port);
            fail_count++;
            rc = 1;
        }
        if (res.port_disagreement) {
            fail("discover-parallel", "port_disagreement set with a single responder");
            rc = 1;
        }
        Stun_CloseSocket(&res);
    }

    close_sock(dead_sock);
    close_sock(live_sock);
    if (rc == 0) {
        fprintf(stderr, "[test_stun_mock] discover-parallel OK — %u ms, live server won and was retained\n",
                (unsigned)elapsed);
    }
    return rc;
}

/* S3 failure attribution: an all-dead server pool must fail WITH
 * evidence — the diag counters on the StunResult have to say "we
 * probed, we sent, nobody answered" so ConnectFail_ClassifyStunDiscover
 * lands on P2P_FAIL_STUN_ALLDOWN (UDP filtered) rather than the
 * no-network bucket. Pre-S3 a failed discovery carried nothing. */
static int run_discover_alldead_diag_test(void) {
    fprintf(stderr, "[test_stun_mock] discover-alldead-diag: failure must carry classification evidence\n");

    unsigned short dead_port_a = 0, dead_port_b = 0;
    int dead_a = open_udp_on_localhost(&dead_port_a);
    int dead_b = open_udp_on_localhost(&dead_port_b);
    if (dead_a < 0 || dead_b < 0) {
        fail("discover-alldead-diag", "failed to bind localhost UDP sockets");
        if (dead_a >= 0) close_sock(dead_a);
        if (dead_b >= 0) close_sock(dead_b);
        return 1;
    }

    static StunServerDesc servers[2];
    servers[0] = (StunServerDesc){ "127.0.0.1", 0 };
    servers[0].port = dead_port_a;
    servers[1] = (StunServerDesc){ "127.0.0.1", 0 };
    servers[1].port = dead_port_b;
    Stun_TestHook_SetServers(servers, 2);

    StunResult res;
    const bool ok = Stun_Discover(&res, 0, 1200);

    Stun_TestHook_SetServers(NULL, 0);
    close_sock(dead_a);
    close_sock(dead_b);

    int rc = 0;
    if (ok) {
        fail("discover-alldead-diag", "Stun_Discover succeeded against dead-only servers");
        Stun_CloseSocket(&res);
        return 1;
    }
    /* Note: both slots dedupe-survive (same IP, different ports). */
    if (res.diag_servers_probed != 2) {
        fprintf(stderr, "[test_stun_mock] FAIL: discover-alldead-diag: probed=%d expected 2\n",
                res.diag_servers_probed);
        fail_count++;
        rc = 1;
    }
    if (res.diag_servers_answered != 0) {
        fail("discover-alldead-diag", "answered != 0 on the failure path");
        rc = 1;
    }
    if (res.diag_sends_ok <= 0) {
        fail("discover-alldead-diag", "sends_ok not recorded (localhost sends must succeed)");
        rc = 1;
    }
    if (res.diag_dns_all_failed) {
        fail("discover-alldead-diag", "dns_all_failed set — numeric 127.0.0.1 resolves");
        rc = 1;
    }
    const ConnectFailCode code = ConnectFail_ClassifyStunDiscover(
        res.diag_servers_probed, res.diag_servers_answered,
        res.diag_sends_ok, res.diag_dns_all_failed);
    if (code != CONNECT_FAIL_STUN_ALLDOWN) {
        fprintf(stderr,
                "[test_stun_mock] FAIL: discover-alldead-diag: classified %s, expected "
                "P2P_FAIL_STUN_ALLDOWN\n", ConnectFail_Code(code));
        fail_count++;
        rc = 1;
    }
    if (rc == 0) {
        fprintf(stderr,
                "[test_stun_mock] discover-alldead-diag OK — probed=%d sends_ok=%d -> %s\n",
                res.diag_servers_probed, res.diag_sends_ok, ConnectFail_Code(code));
    }
    return rc;
}

/* Single server that drops the first request: the 500 ms retransmit
 * must recover discovery (pre-S2 there was NO retransmit — one lost
 * packet burned the server's whole 2 s window). */
static int run_discover_retransmit_test(void) {
    fprintf(stderr, "[test_stun_mock] discover-retransmit: first-packet loss must be retransmitted\n");

    unsigned short srv_port = 0;
    int srv_sock = open_udp_on_localhost(&srv_port);
    if (srv_sock < 0) {
        fail("discover-retransmit", "failed to bind localhost UDP socket");
        return 1;
    }

    StunSrvCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = srv_sock;
    ctx.mapped_port = 47002;
    ctx.ignore_first = 1; /* drop the initial request */
    SDL_Thread* tid = SDL_CreateThread(stun_srv_thread, "stun_srv_lossy", &ctx);
    if (!tid) {
        fail("discover-retransmit", "SDL_CreateThread failed");
        close_sock(srv_sock);
        return 1;
    }

    static StunServerDesc servers[1];
    servers[0] = (StunServerDesc){ "127.0.0.1", 0 };
    servers[0].port = srv_port;
    Stun_TestHook_SetServers(servers, 1);

    StunResult res;
    const uint32_t t0 = SDL_GetTicks();
    const bool ok = Stun_Discover(&res, 0, 4000);
    const uint32_t elapsed = SDL_GetTicks() - t0;

    Stun_TestHook_SetServers(NULL, 0);
    ctx.stop = true;
    SDL_WaitThread(tid, NULL);

    int rc = 0;
    if (!ok) {
        fail("discover-retransmit", "Stun_Discover failed — first-packet loss was not retransmitted");
        rc = 1;
    } else {
        if (ctx.requests_seen < 2) {
            fprintf(stderr, "[test_stun_mock] FAIL: discover-retransmit: server saw %d request(s), expected >= 2\n",
                    ctx.requests_seen);
            fail_count++;
            rc = 1;
        }
        if (elapsed < 400) {
            fprintf(stderr,
                    "[test_stun_mock] FAIL: discover-retransmit: succeeded in %u ms — impossible if the "
                    "first packet was dropped and the RTO is 500 ms\n", (unsigned)elapsed);
            fail_count++;
            rc = 1;
        }
        if (res.public_port != 47002) {
            fprintf(stderr, "[test_stun_mock] FAIL: discover-retransmit: mapped port %u, expected 47002\n",
                    (unsigned)res.public_port);
            fail_count++;
            rc = 1;
        }
        Stun_CloseSocket(&res);
    }

    close_sock(srv_sock);
    if (rc == 0) {
        fprintf(stderr, "[test_stun_mock] discover-retransmit OK — %d requests, recovered in %u ms\n",
                ctx.requests_seen, (unsigned)elapsed);
    }
    return rc;
}

/* Two live servers reporting DIFFERENT mapped ports: the symmetric-NAT
 * signal must be recorded (S3 consumes it for failure attribution). */
static int run_discover_disagreement_test(void) {
    fprintf(stderr, "[test_stun_mock] discover-disagree: differing mapped ports must set the flag\n");

    unsigned short port_a = 0, port_b = 0;
    int sock_a = open_udp_on_localhost(&port_a);
    int sock_b = open_udp_on_localhost(&port_b);
    if (sock_a < 0 || sock_b < 0) {
        fail("discover-disagree", "failed to bind localhost UDP sockets");
        if (sock_a >= 0) close_sock(sock_a);
        if (sock_b >= 0) close_sock(sock_b);
        return 1;
    }

    StunSrvCtx ctx_a, ctx_b;
    memset(&ctx_a, 0, sizeof(ctx_a));
    memset(&ctx_b, 0, sizeof(ctx_b));
    ctx_a.sock = sock_a;
    ctx_a.mapped_port = 40000;
    ctx_b.sock = sock_b;
    ctx_b.mapped_port = 40001; /* disagrees with A */
    SDL_Thread* tid_a = SDL_CreateThread(stun_srv_thread, "stun_srv_a", &ctx_a);
    SDL_Thread* tid_b = SDL_CreateThread(stun_srv_thread, "stun_srv_b", &ctx_b);
    if (!tid_a || !tid_b) {
        fail("discover-disagree", "SDL_CreateThread failed");
        ctx_a.stop = ctx_b.stop = true;
        if (tid_a) SDL_WaitThread(tid_a, NULL);
        if (tid_b) SDL_WaitThread(tid_b, NULL);
        close_sock(sock_a);
        close_sock(sock_b);
        return 1;
    }

    static StunServerDesc servers[2];
    servers[0] = (StunServerDesc){ "127.0.0.1", 0 };
    servers[0].port = port_a;
    servers[1] = (StunServerDesc){ "127.0.0.1", 0 };
    servers[1].port = port_b;
    Stun_TestHook_SetServers(servers, 2);

    StunResult res;
    const bool ok = Stun_Discover(&res, 0, 4000);

    Stun_TestHook_SetServers(NULL, 0);
    ctx_a.stop = ctx_b.stop = true;
    SDL_WaitThread(tid_a, NULL);
    SDL_WaitThread(tid_b, NULL);

    int rc = 0;
    if (!ok) {
        fail("discover-disagree", "Stun_Discover failed with two live servers");
        rc = 1;
    } else {
        if (!res.port_disagreement) {
            fail("discover-disagree",
                 "servers mapped 40000 vs 40001 but port_disagreement is false");
            rc = 1;
        }
        if (res.public_port != 40000 && res.public_port != 40001) {
            fprintf(stderr, "[test_stun_mock] FAIL: discover-disagree: mapped port %u not from either server\n",
                    (unsigned)res.public_port);
            fail_count++;
            rc = 1;
        }
        Stun_CloseSocket(&res);
    }

    close_sock(sock_a);
    close_sock(sock_b);
    if (rc == 0) {
        fprintf(stderr, "[test_stun_mock] discover-disagree OK — symmetric-NAT signal recorded\n");
    }
    return rc;
}

#endif /* NETPLAY_TEST_HOOKS */

int Netplay_Test_StunMock(void) {
    fail_count = 0;

    const int wire_rc = run_wire_test();
    const int codec_rc = run_codec_test();
    const int retarget_rc = run_punch_retarget_test();
    int discover_rc = 0;
#ifdef NETPLAY_TEST_HOOKS
    discover_rc |= run_discover_parallel_test();
    discover_rc |= run_discover_alldead_diag_test();
    discover_rc |= run_discover_retransmit_test();
    discover_rc |= run_discover_disagreement_test();
#else
    fprintf(stderr, "[test_stun_mock] discover tests skipped (build lacks NETPLAY_TEST_HOOKS)\n");
#endif

    if (fail_count > 0 || wire_rc != 0 || codec_rc != 0 || retarget_rc != 0 || discover_rc != 0) {
        fprintf(stderr, "[test_stun_mock] %d failure(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "[test_stun_mock] OK — wire + codec + retarget + discover passed\n");
    return 0;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_StunMock(void) {
    fprintf(stderr,
            "[test_stun_mock] not compiled in; rebuild with "
            "-DENABLE_NETPLAY_TESTS to enable.\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
