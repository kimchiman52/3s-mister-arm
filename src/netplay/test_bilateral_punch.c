/*
 * test_bilateral_punch.c — Step 6 test harness for
 * docs/plan-bilateral-hole-punch.md.
 *
 * Five tests in one TU:
 *
 *   (1) Mock UDP rendezvous-server REGISTER/DELIVER round-trip — two
 *       mock clients register with the same session key against an
 *       in-process mock server bound on 127.0.0.1; once both are
 *       registered, each receives a DELIVER carrying the OTHER client's
 *       (127.0.0.1, port) tuple, parsed via Rendezvous_ParseDeliver.
 *   (2) Session-key derivation stability — same inputs, same key;
 *       ip_be == 0 returns false.
 *   (3) LAN-bypass truth table — DirectP2P_TestHook_IsLanPeer over the
 *       documented private/loopback/link-local ranges and a sample of
 *       non-LAN public addresses.
 *   (4) Simplified protocol round-trip — REGISTER + POLL + DELIVER end
 *       to end against the same mock server. (See comment block at the
 *       Test 4 driver: a full state-machine drive would require a real
 *       STUN server and is deferred to Step 7 manual smoke tests.)
 *   (5) Kill-switch parser-level test — verifies the
 *       CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL default reads back
 *       through Config_GetBool. The config layer exposes no
 *       Config_SetBool today, so a programmatic flip-and-readback would
 *       require either a Config_SetBool addition or rewriting the on-
 *       disk config.ini and reinitializing — both out of scope for this
 *       harness. The actual gate logic in join_thread_fn is exercised by
 *       Step 7 manual smoke testing.
 *
 * Gated behind ENABLE_NETPLAY_TESTS. Mirrors test_stun_mock.c's pattern
 * (mock server in helper thread; localhost UDP; SDL3 sockets here are
 * raw POSIX/Winsock since the production rendezvous client does not
 * touch SDL_net for the wire codec). Enable with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS='-DENABLE_NETPLAY_TESTS -DNETPLAY_TEST_HOOKS'"
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/connect_fail.h"
#include "netplay/direct_p2p.h"
#include "netplay/net_tuning.h"
#include "netplay/rendezvous.h"
#include "netplay/room_code.h"
#include "port/config/config.h"

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

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((in_addr_t)0x7F000001)
#endif

/* Wire constants must mirror rendezvous.c — duplicated locally so this
 * TU is independent of rendezvous.c's file-statics. */
#define REND_MAGIC_BYTES_0 0x33u  /* '3' */
#define REND_MAGIC_BYTES_1 0x53u  /* 'S' */
#define REND_MAGIC_BYTES_2 0x58u  /* 'X' */
#define REND_MAGIC_BYTES_3 0x52u  /* 'R' */
#define REND_VERSION       1
#define REND_TYPE_REGISTER 1
#define REND_TYPE_DELIVER  2
#define REND_TYPE_POLL     3

#define REND_REGISTER_LEN  28
#define REND_DELIVER_LEN   32
#define REND_KEY_LEN       16

static int fail_count = 0;

static void fail_at(const char* file, int line, const char* tag, const char* why) {
    fprintf(stderr, "[test_bilateral_punch] FAIL: %s:%d: %s: %s\n",
            file, line, tag, why);
    fail_count++;
}

#define FAIL(tag, why) fail_at(__FILE__, __LINE__, (tag), (why))

#define EXPECT_TRUE(tag, cond) do { \
    if (!(cond)) { fail_at(__FILE__, __LINE__, (tag), "expected true: " #cond); } \
} while (0)

#define EXPECT_FALSE(tag, cond) do { \
    if ((cond)) { fail_at(__FILE__, __LINE__, (tag), "expected false: " #cond); } \
} while (0)

/* --- localhost UDP socket helpers ------------------------------------- */

static int open_udp_on_localhost(unsigned short* out_port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int one = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
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

/* --- Mock rendezvous server ------------------------------------------- */

/* Tracks two registered endpoints keyed by session_key. When a second
 * REGISTER for the same key arrives we send each side the OTHER side's
 * endpoint via a DELIVER packet. POLL packets receive a DELIVER if both
 * are registered, else a "pending" DELIVER (peer 0.0.0.0:0).
 *
 * Single key in the mock — the test only registers one session at a
 * time. */
typedef struct {
    bool                used;
    uint8_t             key[REND_KEY_LEN];
    struct sockaddr_in  ep[2];          /* up to two endpoints */
    int                 ep_count;
    uint16_t            ep_public_port[2];
} MockSession;

typedef struct {
    int             sock;
    volatile bool   stop;
    MockSession     session;
} MockServerCtx;

/* Build a DELIVER packet for `peer` (or zeros if peer is NULL meaning
 * "not yet registered"). Returns the 32-byte packet length. */
static int build_deliver(uint8_t out[REND_DELIVER_LEN],
                         const uint8_t key[REND_KEY_LEN],
                         const struct sockaddr_in* peer,
                         uint16_t peer_public_port) {
    memset(out, 0, REND_DELIVER_LEN);
    out[0] = REND_MAGIC_BYTES_0;
    out[1] = REND_MAGIC_BYTES_1;
    out[2] = REND_MAGIC_BYTES_2;
    out[3] = REND_MAGIC_BYTES_3;
    out[4] = (uint8_t)REND_VERSION;
    out[5] = (uint8_t)REND_TYPE_DELIVER;
    /* reserved [6..7] = 0 */
    memcpy(&out[8], key, REND_KEY_LEN);
    if (peer) {
        /* peer_ip raw 4 bytes at out[24..27] (network byte order). */
        memcpy(&out[24], &peer->sin_addr.s_addr, 4);
        /* peer_port BE at out[28..29]. */
        out[28] = (uint8_t)((peer_public_port >> 8) & 0xFFu);
        out[29] = (uint8_t)(peer_public_port & 0xFFu);
    }
    /* reserved2 [30..31] = 0 */
    return REND_DELIVER_LEN;
}

static int SDLCALL mock_server_thread(void* arg) {
    MockServerCtx* ctx = (MockServerCtx*)arg;
    const long long start = (long long)time(NULL);

    for (;;) {
        if (ctx->stop) return 0;
        if ((long long)time(NULL) - start > 5) return 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->sock, &rfds);
        struct timeval tv = { 0, 50 * 1000 };
        const int rc = select(ctx->sock + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) continue;

        uint8_t buf[128];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        const int n = (int)recvfrom(ctx->sock, (char*)buf, sizeof(buf), 0,
                                    (struct sockaddr*)&src, &sl);
        if (n < REND_REGISTER_LEN) continue;

        /* Validate magic + version. */
        if (buf[0] != REND_MAGIC_BYTES_0 || buf[1] != REND_MAGIC_BYTES_1 ||
            buf[2] != REND_MAGIC_BYTES_2 || buf[3] != REND_MAGIC_BYTES_3 ||
            buf[4] != REND_VERSION) {
            continue;
        }
        const uint8_t type = buf[5];
        if (type != REND_TYPE_REGISTER && type != REND_TYPE_POLL) continue;

        const uint8_t* req_key = &buf[8];

        /* Bind / find session by key. */
        MockSession* s = &ctx->session;
        if (!s->used) {
            memcpy(s->key, req_key, REND_KEY_LEN);
            s->used = true;
            s->ep_count = 0;
        } else if (memcmp(s->key, req_key, REND_KEY_LEN) != 0) {
            /* Different session — ignore in this minimal mock. */
            continue;
        }

        /* On REGISTER, record this endpoint if new. The "public_port"
         * carried in the REGISTER packet is at offset 24..25 (BE). The
         * mock stores it but, for the localhost test, the actual sender
         * port (src.sin_port) is what the peer needs to send back to. We
         * use src.sin_port for the DELIVER's peer_port so the mock
         * clients can send-and-recv against each other. */
        uint16_t my_pub_port = 0;
        if (type == REND_TYPE_REGISTER) {
            my_pub_port = (uint16_t)(((uint16_t)buf[24] << 8) | buf[25]);
            /* Have we seen this src endpoint? */
            int idx = -1;
            for (int i = 0; i < s->ep_count; ++i) {
                if (s->ep[i].sin_addr.s_addr == src.sin_addr.s_addr &&
                    s->ep[i].sin_port == src.sin_port) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0 && s->ep_count < 2) {
                s->ep[s->ep_count] = src;
                s->ep_public_port[s->ep_count] = my_pub_port;
                s->ep_count++;
            }
        }
        (void)my_pub_port;

        /* Send a DELIVER back to `src`. If 2 endpoints are registered,
         * the DELIVER carries the OTHER endpoint. Otherwise carries
         * 0.0.0.0:0 ("not yet registered"). */
        const struct sockaddr_in* peer = NULL;
        uint16_t peer_pub_port = 0;
        if (s->ep_count == 2) {
            for (int i = 0; i < 2; ++i) {
                if (s->ep[i].sin_addr.s_addr == src.sin_addr.s_addr &&
                    s->ep[i].sin_port == src.sin_port) {
                    const int other = 1 - i;
                    peer = &s->ep[other];
                    /* Use the actual bound port the peer is reading on
                     * (src.sin_port for the OTHER endpoint), not the
                     * REGISTER-claimed public port — for the localhost
                     * test these are the same when register-port-claim
                     * matches the bound port. The test passes the bound
                     * port as the REGISTER claim, so either works. */
                    peer_pub_port = ntohs(s->ep[other].sin_port);
                    break;
                }
            }
        }

        uint8_t reply[REND_DELIVER_LEN];
        int rl = build_deliver(reply, s->key, peer, peer_pub_port);
        sendto(ctx->sock, (const char*)reply, rl, 0,
               (struct sockaddr*)&src, sl);
    }
}

/* --- Test 1: REGISTER/DELIVER round-trip ------------------------------ */

static int test_register_deliver(void) {
    fprintf(stderr, "[test_bilateral_punch] test 1: REGISTER/DELIVER round-trip\n");

    unsigned short server_port = 0, client_a_port = 0, client_b_port = 0;
    int server_sock  = open_udp_on_localhost(&server_port);
    int client_a     = open_udp_on_localhost(&client_a_port);
    int client_b     = open_udp_on_localhost(&client_b_port);
    if (server_sock < 0 || client_a < 0 || client_b < 0) {
        FAIL("test1", "failed to bind localhost UDP sockets");
        if (server_sock >= 0) close_sock(server_sock);
        if (client_a >= 0)    close_sock(client_a);
        if (client_b >= 0)    close_sock(client_b);
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.stop = false;

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rendezvous_mock", &ctx);
    if (!tid) {
        FAIL("test1", "SDL_CreateThread failed");
        close_sock(server_sock);
        close_sock(client_a);
        close_sock(client_b);
        return 1;
    }

    /* Derive a session key that both clients share. */
    uint8_t key[REND_KEY_LEN];
    if (!Rendezvous_DeriveSessionKey(0x01020304u, 1234, key)) {
        FAIL("test1", "Rendezvous_DeriveSessionKey returned false");
        goto cleanup_fail;
    }

    /* Build REGISTER packets. Each client claims its own bound port as
     * its public port — the mock will echo that back inside the DELIVER. */
    uint8_t reg_a[REND_REGISTER_LEN];
    uint8_t reg_b[REND_REGISTER_LEN];
    if (!Rendezvous_BuildRegister(client_a_port, key, reg_a) ||
        !Rendezvous_BuildRegister(client_b_port, key, reg_b)) {
        FAIL("test1", "Rendezvous_BuildRegister returned false");
        goto cleanup_fail;
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    srv.sin_port = htons(server_port);

    /* A registers first; B registers ~50ms later so the DELIVER A
     * receives arrives only after B has been seen. */
    if (sendto(client_a, (const char*)reg_a, REND_REGISTER_LEN, 0,
               (struct sockaddr*)&srv, sizeof(srv)) != REND_REGISTER_LEN) {
        FAIL("test1", "sendto(client_a, REGISTER) failed");
        goto cleanup_fail;
    }
    SDL_Delay(50);

    /* Drain the "pending" DELIVER A receives (if any) before B registers. */
    {
        uint8_t drop[REND_DELIVER_LEN];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_a, &rfds);
        struct timeval tv = { 0, 50 * 1000 };
        if (select(client_a + 1, &rfds, NULL, NULL, &tv) > 0) {
            (void)recvfrom(client_a, (char*)drop, sizeof(drop), 0,
                           (struct sockaddr*)&from, &fl);
        }
    }

    if (sendto(client_b, (const char*)reg_b, REND_REGISTER_LEN, 0,
               (struct sockaddr*)&srv, sizeof(srv)) != REND_REGISTER_LEN) {
        FAIL("test1", "sendto(client_b, REGISTER) failed");
        goto cleanup_fail;
    }
    SDL_Delay(50);

    /* Both peers should now receive a DELIVER carrying the OTHER's
     * (127.0.0.1, port). B's DELIVER arrives first (its REGISTER was
     * the second one, so it pairs immediately). A may need a second
     * REGISTER to receive the pairing — re-send to coax a fresh
     * DELIVER. */
    if (sendto(client_a, (const char*)reg_a, REND_REGISTER_LEN, 0,
               (struct sockaddr*)&srv, sizeof(srv)) != REND_REGISTER_LEN) {
        FAIL("test1", "sendto(client_a, REGISTER 2nd) failed");
        goto cleanup_fail;
    }

    /* Wait up to 500ms for each side's DELIVER. */
    char a_peer_ip[64] = { 0 };
    uint16_t a_peer_port = 0;
    char b_peer_ip[64] = { 0 };
    uint16_t b_peer_port = 0;

    for (int side = 0; side < 2; ++side) {
        const int sk = (side == 0) ? client_a : client_b;
        char* out_ip = (side == 0) ? a_peer_ip : b_peer_ip;
        uint16_t* out_port = (side == 0) ? &a_peer_port : &b_peer_port;

        const long long deadline = (long long)time(NULL) * 1000LL + 500LL;
        bool got = false;
        while (!got) {
            const long long now = (long long)time(NULL) * 1000LL;
            if (now >= deadline) break;
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sk, &rfds);
            struct timeval tv = { 0, 50 * 1000 };
            const int rc = select(sk + 1, &rfds, NULL, NULL, &tv);
            if (rc <= 0) continue;

            uint8_t buf[REND_DELIVER_LEN];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            const int n = (int)recvfrom(sk, (char*)buf, sizeof(buf), 0,
                                        (struct sockaddr*)&from, &fl);
            if (n < REND_DELIVER_LEN) continue;
            if (Rendezvous_ParseDeliver(buf, n, key, out_ip, out_port)) {
                got = true;
            }
        }
        if (!got) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: %s:%d: test1: side %d timeout waiting for DELIVER\n",
                    __FILE__, __LINE__, side);
            fail_count++;
            goto cleanup_fail;
        }
    }

    /* A's DELIVER should carry B's bound port; B's should carry A's. */
    if (strcmp(a_peer_ip, "127.0.0.1") != 0 || a_peer_port != client_b_port) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test1: A got %s:%u, expected 127.0.0.1:%u\n",
                a_peer_ip, (unsigned)a_peer_port, (unsigned)client_b_port);
        fail_count++;
        goto cleanup_fail;
    }
    if (strcmp(b_peer_ip, "127.0.0.1") != 0 || b_peer_port != client_a_port) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test1: B got %s:%u, expected 127.0.0.1:%u\n",
                b_peer_ip, (unsigned)b_peer_port, (unsigned)client_a_port);
        fail_count++;
        goto cleanup_fail;
    }

    fprintf(stderr,
            "[test_bilateral_punch] test 1 OK: A<->B paired at 127.0.0.1:%u and :%u\n",
            (unsigned)client_a_port, (unsigned)client_b_port);

    /* Clean shutdown: stop flag, send a wake-up packet so the server
     * thread breaks out of select promptly, join. */
    ctx.stop = true;
    {
        uint8_t wake = 0;
        sendto(client_a, (const char*)&wake, 1, 0,
               (struct sockaddr*)&srv, sizeof(srv));
    }
    SDL_WaitThread(tid, NULL);
    close_sock(server_sock);
    close_sock(client_a);
    close_sock(client_b);
    return 0;

cleanup_fail:
    ctx.stop = true;
    {
        uint8_t wake = 0;
        sendto(client_a, (const char*)&wake, 1, 0,
               (struct sockaddr*)&srv, sizeof(srv));
    }
    SDL_WaitThread(tid, NULL);
    close_sock(server_sock);
    close_sock(client_a);
    close_sock(client_b);
    return 1;
}

/* --- Test 2: session-key derivation stability ------------------------- */

static int test_session_key_stability(void) {
    fprintf(stderr, "[test_bilateral_punch] test 2: session-key stability\n");

    uint8_t k1[REND_KEY_LEN];
    uint8_t k2[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, k1));
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, k2));
    if (memcmp(k1, k2, REND_KEY_LEN) != 0) {
        FAIL("test2", "deterministic derivation produced different keys");
        return 1;
    }

    /* Different ip yields different key. */
    uint8_t k3[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Eu, 12345, k3));
    if (memcmp(k1, k3, REND_KEY_LEN) == 0) {
        FAIL("test2", "different ip produced identical key (collision)");
        return 1;
    }

    /* ip_be == 0 must return false. */
    uint8_t k4[REND_KEY_LEN];
    if (Rendezvous_DeriveSessionKey(0u, 12345, k4)) {
        FAIL("test2", "ip_be=0 should return false");
        return 1;
    }

    /* --- S4a: punch-token derivation. Deterministic; sensitive to both
     * inputs; DOMAIN-SEPARATED from the session key (a token must never
     * equal a session-key prefix for the same payload); ip_be==0 fails
     * and zeroes the output. */
    uint8_t t1[REND_PUNCH_TOKEN_LEN];
    uint8_t t2[REND_PUNCH_TOKEN_LEN];
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, t1));
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, t2));
    if (memcmp(t1, t2, REND_PUNCH_TOKEN_LEN) != 0) {
        FAIL("test2-token", "deterministic token derivation produced different tokens");
        return 1;
    }
    uint8_t t3[REND_PUNCH_TOKEN_LEN];
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12346, t3));
    if (memcmp(t1, t3, REND_PUNCH_TOKEN_LEN) == 0) {
        FAIL("test2-token", "different port produced identical token");
        return 1;
    }
    /* Domain separation vs the session key over the SAME payload. */
    if (memcmp(t1, k1, REND_PUNCH_TOKEN_LEN) == 0) {
        FAIL("test2-token", "token equals session-key prefix — no domain separation");
        return 1;
    }
    uint8_t t4[REND_PUNCH_TOKEN_LEN];
    memset(t4, 0xEE, sizeof(t4));
    if (Rendezvous_DerivePunchToken(0u, 12345, t4)) {
        FAIL("test2-token", "ip_be=0 should return false");
        return 1;
    }
    for (int i = 0; i < REND_PUNCH_TOKEN_LEN; i++) {
        if (t4[i] != 0) {
            FAIL("test2-token", "failed derivation must zero the output");
            return 1;
        }
    }

    fprintf(stderr, "[test_bilateral_punch] test 2 OK\n");
    return 0;
}

/* --- Test 3: LAN-bypass truth table ----------------------------------- */

static int test_lan_bypass(void) {
    fprintf(stderr, "[test_bilateral_punch] test 3: LAN bypass table\n");

    /* Truth table per direct_p2p.c:direct_p2p_is_lan_peer ranges. */
    struct {
        const char* ip;
        bool expect;
    } cases[] = {
        { "127.0.0.1",       true  },
        { "10.0.0.1",        true  },
        { "172.16.0.1",      true  },
        { "172.31.255.255",  true  },
        { "192.168.1.1",     true  },
        { "169.254.0.1",     true  },
        { "8.8.8.8",         false },
        { "1.2.3.4",         false },
        { "172.32.0.1",      false },
        { "192.169.0.1",     false },
    };
    const int N = (int)(sizeof(cases) / sizeof(cases[0]));
    int local_fails = 0;
    for (int i = 0; i < N; ++i) {
        const bool got = DirectP2P_TestHook_IsLanPeer(cases[i].ip);
        if (got != cases[i].expect) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test3: %s -> %s, expected %s\n",
                    cases[i].ip,
                    got ? "true" : "false",
                    cases[i].expect ? "true" : "false");
            local_fails++;
        }
    }
    if (local_fails > 0) {
        fail_count += local_fails;
        return 1;
    }

    fprintf(stderr, "[test_bilateral_punch] test 3 OK (%d cases)\n", N);
    return 0;
}

/* --- Test 4: simplified protocol round-trip --------------------------- */

/*
 * Per the Step 6 spec: a full state-machine drive (BeginJoin + observed
 * terminal state) requires reading the joiner's STUN-discovered public
 * IP and a real STUN server, neither of which we want in CI. This test
 * therefore exercises the rendezvous-client API end-to-end against the
 * mock server: REGISTER, then POLL, with each step receiving a DELIVER
 * parsed by Rendezvous_ParseDeliver. Test 1 already covers the
 * REGISTER/DELIVER pairing semantics; this test adds the POLL step.
 */
static int test_protocol_round_trip(void) {
    fprintf(stderr, "[test_bilateral_punch] test 4: REGISTER + POLL round-trip\n");

    unsigned short server_port = 0, client_port = 0, peer_port = 0;
    int server_sock = open_udp_on_localhost(&server_port);
    int client_sock = open_udp_on_localhost(&client_port);
    int peer_sock   = open_udp_on_localhost(&peer_port);
    if (server_sock < 0 || client_sock < 0 || peer_sock < 0) {
        FAIL("test4", "failed to bind localhost UDP sockets");
        if (server_sock >= 0) close_sock(server_sock);
        if (client_sock >= 0) close_sock(client_sock);
        if (peer_sock   >= 0) close_sock(peer_sock);
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.stop = false;

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rendezvous_mock4", &ctx);
    if (!tid) {
        FAIL("test4", "SDL_CreateThread failed");
        close_sock(server_sock);
        close_sock(client_sock);
        close_sock(peer_sock);
        return 1;
    }

    uint8_t key[REND_KEY_LEN];
    if (!Rendezvous_DeriveSessionKey(0x01020304u, 4321, key)) {
        FAIL("test4", "DeriveSessionKey returned false");
        goto cleanup_fail;
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    srv.sin_port = htons(server_port);

    /* (a) Register peer first so the client's REGISTER pairs immediately. */
    {
        uint8_t reg_p[REND_REGISTER_LEN];
        if (!Rendezvous_BuildRegister(peer_port, key, reg_p)) {
            FAIL("test4", "BuildRegister(peer) failed");
            goto cleanup_fail;
        }
        if (sendto(peer_sock, (const char*)reg_p, REND_REGISTER_LEN, 0,
                   (struct sockaddr*)&srv, sizeof(srv)) != REND_REGISTER_LEN) {
            FAIL("test4", "sendto(peer REGISTER) failed");
            goto cleanup_fail;
        }
        /* Drain peer's pending DELIVER. */
        SDL_Delay(50);
        uint8_t drop[REND_DELIVER_LEN];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(peer_sock, &rfds);
        struct timeval tv = { 0, 50 * 1000 };
        if (select(peer_sock + 1, &rfds, NULL, NULL, &tv) > 0) {
            (void)recvfrom(peer_sock, (char*)drop, sizeof(drop), 0,
                           (struct sockaddr*)&from, &fl);
        }
    }

    /* (b) Client REGISTER -> expect DELIVER with peer info. */
    {
        uint8_t reg_c[REND_REGISTER_LEN];
        if (!Rendezvous_BuildRegister(client_port, key, reg_c)) {
            FAIL("test4", "BuildRegister(client) failed");
            goto cleanup_fail;
        }
        if (sendto(client_sock, (const char*)reg_c, REND_REGISTER_LEN, 0,
                   (struct sockaddr*)&srv, sizeof(srv)) != REND_REGISTER_LEN) {
            FAIL("test4", "sendto(client REGISTER) failed");
            goto cleanup_fail;
        }

        char peer_ip_str[64] = { 0 };
        uint16_t parsed_peer_port = 0;
        const long long deadline = (long long)time(NULL) * 1000LL + 500LL;
        bool got = false;
        while (!got) {
            const long long now = (long long)time(NULL) * 1000LL;
            if (now >= deadline) break;
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(client_sock, &rfds);
            struct timeval tv = { 0, 50 * 1000 };
            if (select(client_sock + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
            uint8_t buf[REND_DELIVER_LEN];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            const int n = (int)recvfrom(client_sock, (char*)buf, sizeof(buf), 0,
                                        (struct sockaddr*)&from, &fl);
            if (n < REND_DELIVER_LEN) continue;
            if (Rendezvous_ParseDeliver(buf, n, key, peer_ip_str, &parsed_peer_port)) {
                got = true;
            }
        }
        if (!got) {
            FAIL("test4", "REGISTER step: timeout waiting for paired DELIVER");
            goto cleanup_fail;
        }
        if (strcmp(peer_ip_str, "127.0.0.1") != 0 || parsed_peer_port != peer_port) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test4 REGISTER step: got %s:%u, expected 127.0.0.1:%u\n",
                    peer_ip_str, (unsigned)parsed_peer_port, (unsigned)peer_port);
            fail_count++;
            goto cleanup_fail;
        }
    }

    /* (c) Client POLL -> expect DELIVER with peer info (already paired). */
    {
        uint8_t poll_c[REND_REGISTER_LEN];
        if (!Rendezvous_BuildPoll(key, poll_c)) {
            FAIL("test4", "BuildPoll failed");
            goto cleanup_fail;
        }
        if (sendto(client_sock, (const char*)poll_c, REND_REGISTER_LEN, 0,
                   (struct sockaddr*)&srv, sizeof(srv)) != REND_REGISTER_LEN) {
            FAIL("test4", "sendto(client POLL) failed");
            goto cleanup_fail;
        }

        char peer_ip_str[64] = { 0 };
        uint16_t parsed_peer_port = 0;
        const long long deadline = (long long)time(NULL) * 1000LL + 500LL;
        bool got = false;
        while (!got) {
            const long long now = (long long)time(NULL) * 1000LL;
            if (now >= deadline) break;
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(client_sock, &rfds);
            struct timeval tv = { 0, 50 * 1000 };
            if (select(client_sock + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
            uint8_t buf[REND_DELIVER_LEN];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            const int n = (int)recvfrom(client_sock, (char*)buf, sizeof(buf), 0,
                                        (struct sockaddr*)&from, &fl);
            if (n < REND_DELIVER_LEN) continue;
            if (Rendezvous_ParseDeliver(buf, n, key, peer_ip_str, &parsed_peer_port)) {
                got = true;
            }
        }
        if (!got) {
            FAIL("test4", "POLL step: timeout waiting for DELIVER");
            goto cleanup_fail;
        }
        if (strcmp(peer_ip_str, "127.0.0.1") != 0 || parsed_peer_port != peer_port) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test4 POLL step: got %s:%u, expected 127.0.0.1:%u\n",
                    peer_ip_str, (unsigned)parsed_peer_port, (unsigned)peer_port);
            fail_count++;
            goto cleanup_fail;
        }
    }

    fprintf(stderr, "[test_bilateral_punch] test 4 OK\n");

    ctx.stop = true;
    {
        uint8_t wake = 0;
        sendto(client_sock, (const char*)&wake, 1, 0,
               (struct sockaddr*)&srv, sizeof(srv));
    }
    SDL_WaitThread(tid, NULL);
    close_sock(server_sock);
    close_sock(client_sock);
    close_sock(peer_sock);
    return 0;

cleanup_fail:
    ctx.stop = true;
    {
        uint8_t wake = 0;
        sendto(client_sock, (const char*)&wake, 1, 0,
               (struct sockaddr*)&srv, sizeof(srv));
    }
    SDL_WaitThread(tid, NULL);
    close_sock(server_sock);
    close_sock(client_sock);
    close_sock(peer_sock);
    return 1;
}

/* --- Test 5: kill-switch parser-level test ---------------------------- */

/*
 * Simplification: there is no Config_SetBool API today (config.c only
 * exposes Config_SetString; bools are loaded from disk by dict_iterator
 * detecting the literal strings "true"/"false"). A full programmatic
 * round-trip would require either adding Config_SetBool or rewriting
 * config.ini and reinitializing — both beyond Step 6's scope. Instead,
 * we verify (a) the default value is reachable through Config_GetBool
 * (returns false, matching the documented kill-switch default at
 * config.c:81 — "off until user opts out"), and (b) the API does not
 * crash on a bogus key. The actual DISABLE_BILATERAL gate path in
 * join_thread_fn is exercised by Step 7 manual smoke testing.
 */
static int test_kill_switch_round_trip(void) {
    fprintf(stderr, "[test_bilateral_punch] test 5: kill-switch config gate\n");

    /* Default must be false (bilateral fallback ENABLED out of the box). */
    const bool default_disabled =
        Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL);
    if (default_disabled) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test5: default DISABLE_BILATERAL "
                "should be false (bilateral fallback enabled by default), got true\n");
        fail_count++;
        return 1;
    }

    /* Bogus key must return false (the documented Config_GetBool fallback). */
    const bool bogus = Config_GetBool("netplay-bogus-key-no-such-thing");
    if (bogus) {
        FAIL("test5", "Config_GetBool on unknown key returned true");
        return 1;
    }

    fprintf(stderr,
            "[test_bilateral_punch] test 5 OK (default=false; programmatic flip "
            "deferred — no Config_SetBool API)\n");
    return 0;
}

/* --- Test 6: joiner fresh-socket auto-retry (S2) ----------------------- */

/*
 * Drives the REAL BeginJoin state machine end-to-end using the
 * Stun_Discover + Stun_HolePunch seams (no real network):
 *
 *   Part A — punch fails on both attempts against a LAN peer
 *   (10.0.0.1, so the deterministic LAN bypass fails the attempt as
 *   FAILED_SYMMETRIC without ever touching the rendezvous server).
 *   Expectation: the joiner runs exactly TWO full attempts before
 *   surfacing the error, and each attempt binds a FRESH local socket
 *   (different OS-assigned port — a new local port dodges stuck
 *   conntrack/NAT state).
 *
 *   Part B — punch fails on attempt 1 and succeeds on attempt 2.
 *   Expectation: the retry rescues the join; terminal state HANDOFF.
 *
 * The discover mock creates a REAL SDL3_net socket (so the failure
 * paths' Stun_CloseSocket and the handoff bookkeeping operate on real
 * resources) and records each socket's bound port via the net_tuning
 * layout mirror.
 */

static int s_mock_discover_calls = 0;
static uint16_t s_mock_discover_ports[8];
static int s_mock_punch_calls = 0;
static int s_mock_punch_succeed_from = 0; /* 1-based call # from which punch succeeds; 0 = never */

static uint16_t mock_sdlnet_local_port(NET_DatagramSocket* sock) {
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

static bool mock_stun_discover(StunResult* result, uint16_t local_port, int timeout_ms) {
    (void)local_port;
    (void)timeout_ms;
    memset(result, 0, sizeof(*result));

    NET_Address* bind_addr = NET_ResolveHostname("0.0.0.0");
    if (bind_addr) {
        int wait = 0;
        while (NET_GetAddressStatus(bind_addr) == NET_WAITING && wait < 100) {
            SDL_Delay(1);
            wait++;
        }
    }
    result->socket = NET_CreateDatagramSocket(bind_addr, 0); /* port 0: fresh OS bind */
    if (bind_addr) NET_UnrefAddress(bind_addr);
    if (result->socket == NULL) {
        return false;
    }
    result->local_port = mock_sdlnet_local_port(result->socket);
    if (s_mock_discover_calls < (int)(sizeof(s_mock_discover_ports) / sizeof(s_mock_discover_ports[0]))) {
        s_mock_discover_ports[s_mock_discover_calls] = result->local_port;
    }
    s_mock_discover_calls++;
    SDL_strlcpy(result->public_ip, "203.0.113.9", sizeof(result->public_ip)); /* TEST-NET-3 */
    result->public_port = 40000;
    return true;
}

static bool mock_stun_punch(StunResult* local, char* peer_ip, uint16_t* peer_port,
                            const uint8_t punch_token[STUN_PUNCH_TOKEN_LEN],
                            int punch_duration_ms, SDL_AtomicInt* cancel_flag) {
    (void)local;
    (void)peer_ip;
    (void)peer_port;
    (void)punch_token; /* S4a: token now flows through the seam */
    (void)punch_duration_ms;
    (void)cancel_flag;
    s_mock_punch_calls++;
    return s_mock_punch_succeed_from > 0 && s_mock_punch_calls >= s_mock_punch_succeed_from;
}

/* Poll DirectP2P_GetState until it reaches `want` or `budget_ms` runs
 * out. Returns true on reach. */
static bool wait_for_state(DirectP2PState want, int budget_ms) {
    const uint32_t start = SDL_GetTicks();
    while ((int)(SDL_GetTicks() - start) < budget_ms) {
        if (DirectP2P_GetState() == want) return true;
        SDL_Delay(20);
    }
    return DirectP2P_GetState() == want;
}

static int test_joiner_fresh_socket_retry(void) {
    fprintf(stderr, "[test_bilateral_punch] test 6: joiner fresh-socket auto-retry\n");

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetStunHolePunch(mock_stun_punch);

    /* Room code for a LAN peer: after the (mocked) punch failure the
     * deterministic LAN bypass fails the attempt as FAILED_SYMMETRIC
     * before any rendezvous traffic — keeping this test offline. */
    struct in_addr lan_peer;
    lan_peer.s_addr = htonl(0x0A000001u); /* 10.0.0.1 */
    char code[ROOM_CODE_BUF_LEN] = { 0 };
    if (!RoomCode_Encode((uint32_t)lan_peer.s_addr, 5555, code)) {
        FAIL("test6", "RoomCode_Encode failed");
        DirectP2P_TestHook_SetStunDiscover(NULL);
        DirectP2P_TestHook_SetStunHolePunch(NULL);
        return 1;
    }

    int rc = 0;

    /* --- Part A: both attempts fail -> exactly 2 attempts, fresh ports. */
    s_mock_discover_calls = 0;
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 0;
    memset(s_mock_discover_ports, 0, sizeof(s_mock_discover_ports));

    DirectP2P_BeginJoin(code);
    if (!wait_for_state(DIRECT_P2P_FAILED_SYMMETRIC, 10000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test6A: state %d after budget, expected FAILED_SYMMETRIC\n",
                (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
    } else {
        if (s_mock_discover_calls != 2) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test6A: %d discover call(s), expected 2 "
                    "(one automatic retry)\n", s_mock_discover_calls);
            fail_count++;
            rc = 1;
        }
        if (s_mock_punch_calls != 2) {
            fprintf(stderr, "[test_bilateral_punch] FAIL: test6A: %d punch call(s), expected 2\n",
                    s_mock_punch_calls);
            fail_count++;
            rc = 1;
        }
        if (s_mock_discover_calls >= 2 &&
            (s_mock_discover_ports[0] == 0 ||
             s_mock_discover_ports[0] == s_mock_discover_ports[1])) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test6A: retry reused local port %u — the retry "
                    "must bind a FRESH socket\n", (unsigned)s_mock_discover_ports[0]);
            fail_count++;
            rc = 1;
        }
    }
    DirectP2P_Cancel(); /* back to IDLE for Part B */

    /* --- Part B: retry rescues the join -> HANDOFF. */
    s_mock_discover_calls = 0;
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 2;

    DirectP2P_BeginJoin(code);
    if (!wait_for_state(DIRECT_P2P_HANDOFF, 10000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test6B: state %d after budget, expected HANDOFF "
                "(retry should have rescued the join)\n",
                (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
    } else if (s_mock_discover_calls != 2 || s_mock_punch_calls != 2) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test6B: discover=%d punch=%d, expected 2/2\n",
                s_mock_discover_calls, s_mock_punch_calls);
        fail_count++;
        rc = 1;
    }

    /* Restore production hooks. The HANDOFF-state socket is deliberately
     * left to the process teardown — Cancel is a no-op in HANDOFF and
     * the harness exits right after this test. */
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetStunHolePunch(NULL);

    if (rc == 0) {
        fprintf(stderr,
                "[test_bilateral_punch] test 6 OK — 2 attempts, fresh ports (%u -> %u), "
                "retry rescued Part B to HANDOFF\n",
                (unsigned)s_mock_discover_ports[0], (unsigned)s_mock_discover_ports[1]);
    }
    return rc;
}

/* --- Test 7: S3 failure taxonomy -------------------------------------- */

/*
 * S3 (docs/plan-netplay-connection.md §5): the tri-state DELIVER parse
 * plus the pure classifiers in connect_fail.c. These are the functions
 * direct_p2p.c uses to turn thrown-away evidence into distinct machine
 * codes + user strings; a taxonomy with no tests is a taxonomy that
 * rots.
 */
static int test_failure_taxonomy(void) {
    /* --- 7a: Rendezvous_ParseDeliverEx tri-state split. Pre-S3 the
     * bool API conflated zero-sentinel with malformed. */
    uint8_t key[REND_KEY_LEN];
    memset(key, 0xAB, sizeof(key));

    uint8_t deliver[REND_DELIVER_LEN];
    memset(deliver, 0, sizeof(deliver));
    deliver[0] = REND_MAGIC_BYTES_0;
    deliver[1] = REND_MAGIC_BYTES_1;
    deliver[2] = REND_MAGIC_BYTES_2;
    deliver[3] = REND_MAGIC_BYTES_3;
    deliver[4] = REND_VERSION;
    deliver[5] = REND_TYPE_DELIVER;
    memcpy(&deliver[8], key, REND_KEY_LEN);
    /* peer = 0.0.0.0:0 (already zero) — the "not yet registered" sentinel. */

    char ip[64] = { 0 };
    uint16_t port = 0;
    EXPECT_TRUE("7a-empty",
                Rendezvous_ParseDeliverEx(deliver, sizeof(deliver), key, ip, &port) ==
                    REND_DELIVER_EMPTY);
    EXPECT_TRUE("7a-empty-outputs", ip[0] == '\0' && port == 0);
    /* Legacy bool wrapper still reports "no peer" for the sentinel. */
    EXPECT_FALSE("7a-empty-legacy",
                 Rendezvous_ParseDeliver(deliver, sizeof(deliver), key, ip, &port));

    /* Real endpoint 9.8.7.6:4321. */
    deliver[24] = 9; deliver[25] = 8; deliver[26] = 7; deliver[27] = 6;
    deliver[28] = (uint8_t)(4321 >> 8);
    deliver[29] = (uint8_t)(4321 & 0xFF);
    EXPECT_TRUE("7a-peer",
                Rendezvous_ParseDeliverEx(deliver, sizeof(deliver), key, ip, &port) ==
                    REND_DELIVER_PEER);
    EXPECT_TRUE("7a-peer-outputs", strcmp(ip, "9.8.7.6") == 0 && port == 4321);
    EXPECT_TRUE("7a-peer-legacy",
                Rendezvous_ParseDeliver(deliver, sizeof(deliver), key, ip, &port));

    /* Wrong session key -> MALFORMED (not EMPTY). */
    uint8_t wrong_key[REND_KEY_LEN];
    memset(wrong_key, 0xCD, sizeof(wrong_key));
    EXPECT_TRUE("7a-wrongkey",
                Rendezvous_ParseDeliverEx(deliver, sizeof(deliver), wrong_key, ip, &port) ==
                    REND_DELIVER_MALFORMED);
    /* Truncated frame -> MALFORMED. */
    EXPECT_TRUE("7a-short",
                Rendezvous_ParseDeliverEx(deliver, 16, key, ip, &port) ==
                    REND_DELIVER_MALFORMED);
    /* Wrong type (REGISTER) -> MALFORMED. */
    deliver[5] = REND_TYPE_REGISTER;
    EXPECT_TRUE("7a-wrongtype",
                Rendezvous_ParseDeliverEx(deliver, sizeof(deliver), key, ip, &port) ==
                    REND_DELIVER_MALFORMED);
    deliver[5] = REND_TYPE_DELIVER;

    /* --- 7b: STUN discovery classification (cause 1 vs cause 2). */
    /* All DNS failed, nothing answered -> no network / DNS dead. */
    EXPECT_TRUE("7b-dns-alldown",
                ConnectFail_ClassifyStunDiscover(0, 0, 0, true) == CONNECT_FAIL_DNS_ALLDOWN);
    /* DNS dead but numeric fallbacks probed + sent, still silent ->
     * DNS blackout stays the primary diagnosis. */
    EXPECT_TRUE("7b-dns-alldown-fallbacks",
                ConnectFail_ClassifyStunDiscover(3, 0, 9, true) == CONNECT_FAIL_DNS_ALLDOWN);
    /* DNS fine, sends went out, zero responses -> UDP filtered. */
    EXPECT_TRUE("7b-stun-alldown",
                ConnectFail_ClassifyStunDiscover(4, 0, 12, false) == CONNECT_FAIL_STUN_ALLDOWN);
    /* Sends all failed at the socket layer -> no-network bucket. */
    EXPECT_TRUE("7b-sends-failed",
                ConnectFail_ClassifyStunDiscover(4, 0, 0, false) == CONNECT_FAIL_DNS_ALLDOWN);
    /* Discovery succeeded -> not a discovery failure. */
    EXPECT_TRUE("7b-ok",
                ConnectFail_ClassifyStunDiscover(4, 2, 8, false) == CONNECT_FAIL_NONE);

    /* --- 7c: joiner fallback classification (causes 3-7). */
    ConnectJoinEvidence ev;
    memset(&ev, 0, sizeof(ev));
    /* Zero DELIVERs of any kind -> server down (it answers EVERY
     * REGISTER with a DELIVER, so silence is the server, not the host). */
    EXPECT_TRUE("7c-rendezvous-down",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_RENDEZVOUS_DOWN);
    /* Only zero-sentinel DELIVERs -> host offline / code stale. */
    ev.deliver_any = true;
    EXPECT_TRUE("7c-host-offline",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_HOST_OFFLINE);
    /* Real endpoint arrived, punch failed, cone-family NAT -> blocked. */
    ev.deliver_real = true;
    EXPECT_TRUE("7c-nat-blocked",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_NAT_BLOCKED);
    /* Same + our S2 symmetric signal -> relay-needed class. */
    ev.port_disagreement = true;
    EXPECT_TRUE("7c-symmetric-both",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_SYMMETRIC_BOTH);
    /* S4a: bad-token evidence outranks the NAT diagnoses — the peer's
     * datagrams arrived, they just failed auth. */
    ev.punch_bad_token = true;
    EXPECT_TRUE("7c-punch-auth",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_PUNCH_AUTH);
    ev.punch_bad_token = false;
    /* Hairpin outranks everything. */
    ev.hairpin = true;
    EXPECT_TRUE("7c-hairpin",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_HAIRPIN);
    /* Punch succeeded -> no failure (a stray bad-token sighting must
     * not fail a join that actually connected). */
    memset(&ev, 0, sizeof(ev));
    ev.deliver_any = ev.deliver_real = ev.bilateral_punched = true;
    ev.punch_bad_token = true;
    EXPECT_TRUE("7c-ok", ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_NONE);

    /* --- 7d: host-waiting advisory (cause 8). */
    EXPECT_TRUE("7d-too-early",
                ConnectFail_ClassifyHostWaiting(false, false, CONNECT_HOST_ADVISORY_MS - 1) ==
                    CONNECT_FAIL_NONE);
    EXPECT_TRUE("7d-unmappable",
                ConnectFail_ClassifyHostWaiting(false, false, CONNECT_HOST_ADVISORY_MS) ==
                    CONNECT_FAIL_HOST_UNMAPPABLE);
    EXPECT_TRUE("7d-rendezvous-down-with-upnp",
                ConnectFail_ClassifyHostWaiting(true, false, CONNECT_HOST_ADVISORY_MS) ==
                    CONNECT_FAIL_RENDEZVOUS_DOWN);
    EXPECT_TRUE("7d-deliver-seen",
                ConnectFail_ClassifyHostWaiting(false, true, CONNECT_HOST_ADVISORY_MS * 2) ==
                    CONNECT_FAIL_NONE);

    /* --- 7e: deadline + abort-hold policy helpers (Part A). */
    EXPECT_FALSE("7e-unarmed", ConnectFail_DeadlineExpired(123456, 0, 15000));
    EXPECT_FALSE("7e-young", ConnectFail_DeadlineExpired(10000, 1, 15000));
    EXPECT_TRUE("7e-expired", ConnectFail_DeadlineExpired(15001, 1, 15000));
    /* Wrap-safe: now < since via u64 wraparound still measures elapsed. */
    EXPECT_TRUE("7e-wrap",
                ConnectFail_DeadlineExpired(5000, UINT64_MAX - 20000ULL, 15000));

    int held = 0;
    for (int i = 0; i < CONNECT_ABORT_HOLD_FRAMES - 1; i++) {
        held = ConnectFail_AbortHoldTick(held, true);
    }
    EXPECT_FALSE("7e-hold-not-yet", ConnectFail_AbortHoldFired(held));
    held = ConnectFail_AbortHoldTick(held, true);
    EXPECT_TRUE("7e-hold-fired", ConnectFail_AbortHoldFired(held));
    held = ConnectFail_AbortHoldTick(held, false);
    EXPECT_FALSE("7e-hold-release-resets", ConnectFail_AbortHoldFired(held));
    EXPECT_TRUE("7e-hold-zero", held == 0);

    /* --- 7f: every code has a distinct machine string and a user string. */
    for (int c = CONNECT_FAIL_NONE; c <= CONNECT_FAIL_INTERNAL; c++) {
        const char* mc = ConnectFail_Code((ConnectFailCode)c);
        EXPECT_TRUE("7f-code-nonnull", mc != NULL && mc[0] != '\0');
        EXPECT_TRUE("7f-user-nonnull", ConnectFail_UserText((ConnectFailCode)c) != NULL);
        for (int d = CONNECT_FAIL_NONE; d < c; d++) {
            if (strcmp(mc, ConnectFail_Code((ConnectFailCode)d)) == 0) {
                FAIL("7f-code-distinct", "duplicate machine code string");
            }
        }
    }

    if (fail_count == 0) {
        fprintf(stderr, "[test_bilateral_punch] test 7 OK — DELIVER tri-state + "
                        "taxonomy classifiers + deadline/abort policy\n");
    }
    return fail_count > 0 ? 1 : 0;
}

/* --- Test 8: joiner self-DELIVER misdiagnosis (S3 review HIGH-1) ------- */

/*
 * Regression for the empirically-reproduced stale-room-code flow: a
 * joiner REGISTERing a session key the host never occupied CREATES the
 * server entry with itself as endpointA; the S2 retry re-REGISTERs from
 * a fresh source port, gets filed as endpointB, and the server DELIVERs
 * the joiner ITS OWN attempt-1 endpoint as "the peer". Pre-fix the
 * joiner consumed that self-endpoint as a live host (ev_deliver_real),
 * punched its own dead mapping, and classified the failure as
 * NAT_BLOCKED — the single most common real failure ("stale code")
 * never reported "Host not found".
 *
 * This test drives the REAL BeginJoin through the discover/punch seams
 * against a localhost mock rendezvous that answers EVERY REGISTER with
 * a DELIVER carrying the joiner's own (mock) public endpoint. Required
 * outcome: terminal FAILED_BILATERAL classified as HOST_OFFLINE ("Host
 * not found. Code stale or host offline."), NOT NAT_BLOCKED.
 */

typedef struct {
    int           sock;
    volatile bool stop;
    uint32_t      self_ip_be;   /* endpoint the DELIVER carries (network order) */
    uint16_t      self_port;    /* .. and its port (host order) */
} SelfDeliverCtx;

static int SDLCALL self_deliver_server_thread(void* arg) {
    SelfDeliverCtx* ctx = (SelfDeliverCtx*)arg;
    const long long start = (long long)time(NULL);
    for (;;) {
        if (ctx->stop) return 0;
        if ((long long)time(NULL) - start > 20) return 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->sock, &rfds);
        struct timeval tv = { 0, 50 * 1000 };
        if (select(ctx->sock + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        uint8_t buf[128];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        const int n = (int)recvfrom(ctx->sock, (char*)buf, sizeof(buf), 0,
                                    (struct sockaddr*)&src, &sl);
        if (n < REND_REGISTER_LEN) continue;
        if (buf[0] != REND_MAGIC_BYTES_0 || buf[1] != REND_MAGIC_BYTES_1 ||
            buf[2] != REND_MAGIC_BYTES_2 || buf[3] != REND_MAGIC_BYTES_3 ||
            buf[4] != REND_VERSION || buf[5] != REND_TYPE_REGISTER) {
            continue;
        }
        /* Echo the REGISTER's own session key; carry the SELF endpoint. */
        struct sockaddr_in self_ep;
        memset(&self_ep, 0, sizeof(self_ep));
        self_ep.sin_family = AF_INET;
        self_ep.sin_addr.s_addr = ctx->self_ip_be;
        uint8_t reply[REND_DELIVER_LEN];
        const int rl = build_deliver(reply, &buf[8], &self_ep, ctx->self_port);
        sendto(ctx->sock, (const char*)reply, rl, 0, (struct sockaddr*)&src, sl);
    }
}

static int test_joiner_self_deliver(void) {
    fprintf(stderr, "[test_bilateral_punch] test 8: joiner self-DELIVER -> HOST_OFFLINE\n");

    NET_Init();
    DirectP2P_Init();

    unsigned short server_port = 0;
    int server_sock = open_udp_on_localhost(&server_port);
    if (server_sock < 0) {
        FAIL("test8", "failed to bind localhost UDP socket for mock server");
        return 1;
    }

    SelfDeliverCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.stop = false;
    /* The mock discover (below) reports the joiner's public endpoint as
     * 203.0.113.9:40000; the DELIVER carries the same IP with a DIFFERENT
     * port — exactly what the real server serves back after a retry
     * re-REGISTER (the attempt-1 NAT mapping's port). */
    ctx.self_ip_be = htonl(0xCB007109u); /* 203.0.113.9 (TEST-NET-3) */
    ctx.self_port = 40123;

    SDL_Thread* tid = SDL_CreateThread(self_deliver_server_thread,
                                       "self_deliver_mock", &ctx);
    if (!tid) {
        FAIL("test8", "SDL_CreateThread failed");
        close_sock(server_sock);
        return 1;
    }

    /* Room code for a PUBLIC, non-LAN, non-self host so neither the LAN
     * bypass nor the hairpin bypass fires and the joiner reaches the
     * fallback-signaling loop. 198.51.100.7 (TEST-NET-2). */
    struct in_addr host_ip;
    host_ip.s_addr = htonl(0xC6336407u); /* 198.51.100.7 */
    char code[ROOM_CODE_BUF_LEN] = { 0 };
    int rc = 0;
    if (!RoomCode_Encode((uint32_t)host_ip.s_addr, 6000, code)) {
        FAIL("test8", "RoomCode_Encode failed");
        rc = 1;
        goto done;
    }

    /* Point the signaling at the localhost mock; shrink the budget so
     * waiting out the (now exit-less) loop twice stays fast. */
    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
    }
    DirectP2P_TestHook_SetSignalBudgetMs(1000);
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetStunHolePunch(mock_stun_punch);
    s_mock_discover_calls = 0;
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 0; /* every punch fails */

    DirectP2P_BeginJoin(code);
    if (!wait_for_state(DIRECT_P2P_FAILED_BILATERAL, 15000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test8: state %d after budget, expected "
                "FAILED_BILATERAL\n", (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
    } else {
        const char* status = DirectP2P_GetStatusText();
        const char* want = ConnectFail_UserText(CONNECT_FAIL_HOST_OFFLINE);
        const char* nat = ConnectFail_UserText(CONNECT_FAIL_NAT_BLOCKED);
        if (strcmp(status, nat) == 0 ||
            strcmp(status, ConnectFail_UserText(CONNECT_FAIL_SYMMETRIC_BOTH)) == 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test8: self-DELIVER was consumed as a "
                    "live host — status \"%s\" (the HIGH-1 misdiagnosis)\n", status);
            fail_count++;
            rc = 1;
        } else if (strcmp(status, want) != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test8: status \"%s\", expected \"%s\"\n",
                    status, want);
            fail_count++;
            rc = 1;
        }
    }
    DirectP2P_Cancel(); /* back to IDLE for the tests that follow */

done:
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetStunHolePunch(NULL);
    DirectP2P_TestHook_SetSignalBudgetMs(0);
    ctx.stop = true;
    {
        struct sockaddr_in srv;
        memset(&srv, 0, sizeof(srv));
        srv.sin_family = AF_INET;
        srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        srv.sin_port = htons(server_port);
        uint8_t wake = 0;
        sendto(server_sock, (const char*)&wake, 1, 0,
               (struct sockaddr*)&srv, sizeof(srv));
    }
    SDL_WaitThread(tid, NULL);
    close_sock(server_sock);
    if (rc == 0) {
        fprintf(stderr,
                "[test_bilateral_punch] test 8 OK — self-DELIVER treated as 'peer not "
                "registered'; classified HOST_OFFLINE\n");
    }
    return rc;
}

/* --- Test 9: post-handoff failure report integrity (S3 review HIGH-2) -- */

/*
 * Regression for the spurious-OK / suppressed-FAIL defect: with the
 * orchestrator parked in HANDOFF, DirectP2P_NotifySessionFailed
 * re-arms the outcome reporter, and (pre-fix) the very next Tick's
 * HANDOFF case unconditionally re-fired the SUCCESS report — writing a
 * second "[netplay-connect] OK" and setting the report guard so the
 * real FAIL from FAILED_HANDSHAKE was suppressed. Required behavior:
 * after the failure is latched, ZERO further OK lines, and exactly ONE
 * "[netplay-connect] FAIL code=P2P_FAIL_PEER_REJECTED" line once
 * teardown parks FAILED_HANDSHAKE.
 *
 * Drives the REAL machinery: BeginJoin -> HANDOFF via the seams,
 * DirectP2P_Tick for the report path, NotifySessionFailed for the
 * latch, and the registered teardown callback (via the RunTeardown
 * hook — the same function netplay.c's EXITING pass fires). Log lines
 * are counted by intercepting SDL's log output, which both
 * Netplay_LogConnectEvent and netplay_log_line tee into.
 */

static int s_log_ok_lines = 0;
static int s_log_fail_lines = 0;
static char s_log_last_fail[512] = { 0 };
static SDL_LogOutputFunction s_prev_log_fn = NULL;
static void* s_prev_log_ud = NULL;

static void SDLCALL capture_log_fn(void* userdata, int category,
                                   SDL_LogPriority priority, const char* message) {
    if (message != NULL) {
        if (strncmp(message, "[netplay-connect] OK", 20) == 0) {
            s_log_ok_lines++;
        } else if (strncmp(message, "[netplay-connect] FAIL", 22) == 0) {
            s_log_fail_lines++;
            SDL_strlcpy(s_log_last_fail, message, sizeof(s_log_last_fail));
        }
    }
    if (s_prev_log_fn != NULL) {
        s_prev_log_fn(s_prev_log_ud, category, priority, message);
    }
}

static int test_posthandoff_failure_report(void) {
    fprintf(stderr, "[test_bilateral_punch] test 9: post-handoff failure -> one FAIL, no OK\n");

    NET_Init();
    DirectP2P_Init();

    /* Test 6 Part B leaves the orchestrator parked in HANDOFF; reset to
     * IDLE the same way a real session exit does — via the registered
     * teardown callback (no failure latched => parks IDLE). */
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) {
        FAIL("test9", "teardown hook did not reset the orchestrator to IDLE");
        return 1;
    }

    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetStunHolePunch(mock_stun_punch);
    s_mock_discover_calls = 0;
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 1; /* direct punch succeeds on attempt 1 */

    struct in_addr host_ip;
    host_ip.s_addr = htonl(0xC6336407u); /* 198.51.100.7 (TEST-NET-2) */
    char code[ROOM_CODE_BUF_LEN] = { 0 };
    int rc = 0;
    if (!RoomCode_Encode((uint32_t)host_ip.s_addr, 6000, code)) {
        FAIL("test9", "RoomCode_Encode failed");
        rc = 1;
        goto done;
    }

    DirectP2P_BeginJoin(code);
    if (!wait_for_state(DIRECT_P2P_HANDOFF, 10000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test9: state %d after budget, expected HANDOFF\n",
                (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
        goto done;
    }

    /* Intercept the log stream, then run the exact frame sequence. */
    s_log_ok_lines = 0;
    s_log_fail_lines = 0;
    s_log_last_fail[0] = '\0';
    SDL_GetLogOutputFunction(&s_prev_log_fn, &s_prev_log_ud);
    SDL_SetLogOutputFunction(capture_log_fn, NULL);

    /* Frame A: first HANDOFF tick — executes the main-thread handoff and
     * emits the one legitimate OK line. */
    DirectP2P_Tick();
    if (s_log_ok_lines != 1 || s_log_fail_lines != 0) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test9: after handoff tick, OK=%d FAIL=%d "
                "(expected 1/0)\n", s_log_ok_lines, s_log_fail_lines);
        fail_count++;
        rc = 1;
    }

    /* Frame B: the MIST gate rejects (Netplay_Run runs before
     * DirectP2P_Tick in the same frame) ... */
    DirectP2P_NotifySessionFailed(CONNECT_FAIL_PEER_REJECTED,
                                  "engine version mismatch (test)");
    /* ... and the SAME frame's Tick — plus a few more while netplay is
     * still winding down — must NOT re-fire the success report. */
    DirectP2P_Tick();
    DirectP2P_Tick();
    DirectP2P_Tick();
    if (s_log_ok_lines != 1) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test9: %d OK line(s) after the failure was "
                "latched — the HANDOFF tick re-fired a spurious success report\n",
                s_log_ok_lines - 1);
        fail_count++;
        rc = 1;
    }

    /* Frame C: netplay's EXITING pass fires the teardown callback, which
     * converts the latch into FAILED_HANDSHAKE. */
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_FAILED_HANDSHAKE) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test9: state %d after teardown, expected "
                "FAILED_HANDSHAKE\n", (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
    }
    DirectP2P_Tick();
    DirectP2P_Tick(); /* second tick must not double-report */
    if (s_log_fail_lines != 1) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test9: %d FAIL line(s) after FAILED_HANDSHAKE "
                "(expected exactly 1)\n", s_log_fail_lines);
        fail_count++;
        rc = 1;
    } else if (strstr(s_log_last_fail, "code=P2P_FAIL_PEER_REJECTED") == NULL) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test9: FAIL line lacks "
                "code=P2P_FAIL_PEER_REJECTED: %s\n", s_log_last_fail);
        fail_count++;
        rc = 1;
    }
    if (s_log_ok_lines != 1) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test9: total OK lines %d (expected exactly "
                "the one pre-failure line)\n", s_log_ok_lines);
        fail_count++;
        rc = 1;
    }

    SDL_SetLogOutputFunction(s_prev_log_fn, s_prev_log_ud);
    DirectP2P_Cancel(); /* FAILED_HANDSHAKE -> IDLE */

done:
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetStunHolePunch(NULL);
    if (rc == 0) {
        fprintf(stderr,
                "[test_bilateral_punch] test 9 OK — one OK before the failure, zero after, "
                "exactly one FAIL (PEER_REJECTED) from FAILED_HANDSHAKE\n");
    }
    return rc;
}

/* --- Test 10: S4a host datagram gate ----------------------------------- */

/*
 * Truth table for the host-waiting datagram classifier
 * (classify_host_datagram via the test hook) — the routing gate that
 * decides, for every inbound datagram on the host's advertising socket,
 * between rendezvous frame / STUN response / authenticated peer punch /
 * IGNORE. The load-bearing rows are the IGNORE ones: pre-S4a the final
 * arm was "anything else IS the peer", so a single stray datagram
 * (port scan, stale packet, legacy unauthenticated punch, spoofed
 * garbage) was captured as the opponent, echoed, and handed the
 * session off — hijacking the room AND permanently consuming the
 * host's one peer slot. To prove this test bites: make
 * classify_host_datagram return DP2P_HOST_DGRAM_PEER_PUNCH in its
 * final arm (the pre-fix behavior) and the IGNORE rows fail.
 */
static int test_host_datagram_gate(void) {
    fprintf(stderr, "[test_bilateral_punch] test 10: S4a host datagram gate\n");
    const int fails_before = fail_count;

    uint8_t token[STUN_PUNCH_TOKEN_LEN] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    uint8_t wrong[STUN_PUNCH_TOKEN_LEN] = { 9, 8, 7, 6, 5, 4, 3, 1 };

    /* (a) Authenticated punch -> PEER_PUNCH. */
    uint8_t punch[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(token, punch);
    EXPECT_TRUE("10-auth-punch",
                DirectP2P_TestHook_ClassifyHostDatagram(punch, sizeof(punch), token, true) ==
                    DP2P_HOST_DGRAM_PEER_PUNCH);

    /* (b) WRONG token -> IGNORE (never the peer). */
    uint8_t bad[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(wrong, bad);
    EXPECT_TRUE("10-wrong-token",
                DirectP2P_TestHook_ClassifyHostDatagram(bad, sizeof(bad), token, true) ==
                    DP2P_HOST_DGRAM_IGNORE);

    /* (c) Legacy pre-S4a 9-byte "3SX_PUNCH" -> IGNORE. */
    EXPECT_TRUE("10-legacy-punch",
                DirectP2P_TestHook_ClassifyHostDatagram((const uint8_t*)"3SX_PUNCH", 9,
                                                        token, true) ==
                    DP2P_HOST_DGRAM_IGNORE);

    /* (d) Arbitrary garbage (the classic slot-consumer: one stray
     * packet from a scanner) -> IGNORE. */
    static const uint8_t garbage[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x41 };
    EXPECT_TRUE("10-garbage",
                DirectP2P_TestHook_ClassifyHostDatagram(garbage, (int)sizeof(garbage),
                                                        token, true) ==
                    DP2P_HOST_DGRAM_IGNORE);

    /* (e) Valid punch but NO valid token on the host (fail closed) ->
     * IGNORE. */
    EXPECT_TRUE("10-no-token-fail-closed",
                DirectP2P_TestHook_ClassifyHostDatagram(punch, sizeof(punch), token, false) ==
                    DP2P_HOST_DGRAM_IGNORE);

    /* (f) Rendezvous frame ('3SXR', 32 bytes) -> RENDEZVOUS. */
    uint8_t key[REND_KEY_LEN];
    memset(key, 0x5A, sizeof(key));
    uint8_t deliver[REND_DELIVER_LEN];
    (void)build_deliver(deliver, key, NULL, 0);
    EXPECT_TRUE("10-rendezvous",
                DirectP2P_TestHook_ClassifyHostDatagram(deliver, sizeof(deliver), token, true) ==
                    DP2P_HOST_DGRAM_RENDEZVOUS);

    /* (g) STUN Binding Response (type 0x0101 + magic cookie) -> STUN. */
    uint8_t stun_resp[20] = { 0 };
    stun_resp[0] = 0x01; stun_resp[1] = 0x01;
    stun_resp[4] = 0x21; stun_resp[5] = 0x12;
    stun_resp[6] = 0xA4; stun_resp[7] = 0x42;
    EXPECT_TRUE("10-stun",
                DirectP2P_TestHook_ClassifyHostDatagram(stun_resp, sizeof(stun_resp),
                                                        token, true) ==
                    DP2P_HOST_DGRAM_STUN);

    /* (h) Empty / NULL -> IGNORE. */
    EXPECT_TRUE("10-empty",
                DirectP2P_TestHook_ClassifyHostDatagram(punch, 0, token, true) ==
                    DP2P_HOST_DGRAM_IGNORE);
    EXPECT_TRUE("10-null",
                DirectP2P_TestHook_ClassifyHostDatagram(NULL, 17, token, true) ==
                    DP2P_HOST_DGRAM_IGNORE);

    if (fail_count == fails_before) {
        fprintf(stderr, "[test_bilateral_punch] test 10 OK — unauthenticated datagrams "
                        "can no longer consume the host's peer slot\n");
        return 0;
    }
    return 1;
}

/* --- Entry point ------------------------------------------------------ */

int Netplay_Test_BilateralPunch(void) {
    fail_count = 0;

    int rc = 0;
    rc |= test_register_deliver();
    rc |= test_session_key_stability();
    rc |= test_lan_bypass();
    rc |= test_protocol_round_trip();
    rc |= test_kill_switch_round_trip();
    rc |= test_joiner_self_deliver();
    rc |= test_joiner_fresh_socket_retry();
    rc |= test_failure_taxonomy();
    rc |= test_posthandoff_failure_report();
    rc |= test_host_datagram_gate();

    if (fail_count > 0 || rc != 0) {
        fprintf(stderr, "[test_bilateral_punch] %d failure(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "[test_bilateral_punch] OK\n");
    return 0;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_BilateralPunch(void) {
    fprintf(stderr,
            "[test_bilateral_punch] not compiled in; rebuild with "
            "-DENABLE_NETPLAY_TESTS to enable.\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
