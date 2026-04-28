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
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS"
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/stun.h"

#include <SDL3/SDL.h>
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
static int build_response(uint8_t out[36], const uint8_t txid[12]) {
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
    const uint16_t xport = (uint16_t)MOCK_MAPPED_PORT
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

int Netplay_Test_StunMock(void) {
    fail_count = 0;

    const int wire_rc = run_wire_test();
    const int codec_rc = run_codec_test();

    if (fail_count > 0 || wire_rc != 0 || codec_rc != 0) {
        fprintf(stderr, "[test_stun_mock] %d failure(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "[test_stun_mock] OK — wire + codec passed\n");
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
