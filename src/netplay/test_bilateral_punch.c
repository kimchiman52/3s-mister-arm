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
/* H-C test 30 wraps SDL's allocator to weigh a NET_Address ref leak. */
#include <stdlib.h>

#ifdef ENABLE_NETPLAY_TESTS

/* MEDIUM-5: ENABLE_NETPLAY_TESTS alone is not enough. Every end-to-end
 * test below drives direct_p2p.c through the DirectP2P_TestHook_* seams,
 * and those symbols only exist under -DNETPLAY_TEST_HOOKS (see the
 * NETPLAY_TEST_HOOKS block in direct_p2p.c). Without it this TU used to
 * fail with ~20 "implicit declaration of function
 * 'DirectP2P_TestHook_...'" errors that named the symptom, never the
 * missing flag. */
#ifndef NETPLAY_TEST_HOOKS
#error "test_bilateral_punch.c needs BOTH -DENABLE_NETPLAY_TESTS and -DNETPLAY_TEST_HOOKS. Configure with: -DENABLE_NETPLAY=ON -DNETPLAY_TEST_HOOKS=ON -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS"
#endif

#include "netplay/connect_fail.h"
#include "netplay/direct_p2p.h"
#include "netplay/natpmp.h" /* S7, test 18 */
#include "netplay/netplay.h" /* task #144 review Item A: Netplay_TestHook_SessionFailCodeForEvent, test 43 */
#include "netplay/netplay_nav.h" /* task #76, test 40 */
#include "netplay/net_tuning.h"
#include "netplay/rendezvous.h"
#include "netplay/room_code.h"
#include "netplay/stun.h" /* #150, tests 46/47: punch payload construction */
#include "netplay/upnp.h" /* M-1: Upnp_TestHook_DiscoverAttempts, test 23e */
#include "port/config/config.h"

#include "gekkonet.h" /* GekkoPlayerDisconnected/GekkoDesyncDetected, test 43 */

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
#define REND_VERSION       2  /* S4c: protocol v2 */
#define REND_TYPE_REGISTER 1
#define REND_TYPE_DELIVER  2
#define REND_TYPE_POLL     3
#define REND_TYPE_CHALLENGE 4  /* S4c: server -> client cookie challenge */

#define REND_REGISTER_LEN  36  /* S4c: 28 + 8-byte cookie tail */
#define REND_DELIVER_LEN   32
#define REND_CHALLENGE_LEN 32
#define REND_KEY_LEN       16

/* NOTE on the mock servers below: they are v2 by version byte and
 * length, but they deliberately do NOT implement the S4c challenge —
 * they answer every REGISTER with a DELIVER directly. That is the
 * "cookie already accepted" steady state, which is what tests 1/4/8/9
 * are actually about, and it doubles as coverage that the client stays
 * correct against a server that never challenges it (§6.5's "v2 client,
 * v2 server that never challenges" row). Tests 13-15 flip
 * MockServerCtx.challenge_enabled on and drive the full S4c handshake
 * end to end on BOTH roles; the codec itself is pinned by test 11 and
 * the server half by tools/rendezvous-server/__test_protocol.js. */

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

    /* --- S4c return-routability gate (tests 13-15) --------------------
     * Off by default so tests 1/4/8/9 keep talking to a server that
     * never challenges — §6.5's "v2 client, v2 server that never
     * challenges" row, a supported configuration whose coverage must not
     * regress. When on, the mock behaves like the real v2 server:
     * REGISTER/POLL whose 8-byte cookie tail (bytes [28..35]) is
     * absent/zero/wrong gets exactly ONE 32-byte CHALLENGE and binds
     * NOTHING — no endpoint recorded, no DELIVER. Only the matching
     * echo binds. */
    volatile bool   challenge_enabled;
    /* §6.5 last row: "always challenges but never accepts" — every
     * request is challenged, no cookie is ever honored, zero DELIVERs. */
    volatile bool   never_accept;
    /* Withhold the peer-bearing DELIVER until this many COOKIED requests
     * have arrived (1 = pair on the echo itself). Lets a test keep the
     * client in its waiting state long enough for the rendezvous WORKER
     * thread's next periodic resend to be observed. */
    int             min_cookied_before_peer;
    /* Additionally withhold the peer-bearing DELIVER until this many ms
     * after the mock thread started, so a test can place the DELIVER
     * (and therefore the DELIVER punch candidate) at a chosen instant.
     * Answers the zero-sentinel until then, which every client treats as
     * "keep waiting". 0 = no delay. */
    int             deliver_delay_ms;
    uint32_t        started_ms;
    /* Fabricated peer endpoint the DELIVER carries. The real localhost
     * source would be 127.0.0.1, which every client rejects via the LAN
     * bypass, so a PUBLIC address is synthesized instead (same trick as
     * self_deliver_server_thread). */
    bool            use_synth_peer;
    uint32_t        synth_peer_ip_be;
    uint16_t        synth_peer_port;
    /* Thread lifetime in seconds; 0 selects the historic 5 s. */
    int             life_secs;

    /* Observability (written by the mock thread, read after a wait). */
    volatile int    challenges_sent;
    volatile int    cookied_requests;
    volatile int    uncookied_requests;
    /* Requests with a missing/zero/wrong cookie arriving from an endpoint
     * that has ALREADY echoed a valid one. §6.4 says the client "carries
     * it on every later resend", so on a healthy client this stays 0.
     * It is the only sound observable for the host's cross-thread cookie
     * publish: a re-challenged worker resend provokes ANOTHER main-thread
     * echo, so "a later cookied packet exists" stays true even with the
     * seqlock dead — the uncookied resend that preceded it does not. */
    volatile int    uncookied_after_bind;
    bool            bound_ep_valid;
    struct sockaddr_in bound_ep;
    uint8_t         last_cookie[REND_COOKIE_LEN]; /* cookie last issued */

} MockServerCtx;

/* Cookie construction for the mock. The real server uses
 * SHA-256(secret ‖ "addr:port:slot")[0..7] (cookieForSlot, rendezvous-server.js:775);
 * matching that byte-for-byte is NOT the property under test. What IS
 * under test is that the cookie is (a) bound to the source endpoint and
 * (b) unforgeable by the client — a client that does not RECEIVE the
 * challenge can never produce it. A keyed FNV-1a over (address, port)
 * with a process-lifetime secret gives both. */
static uint64_t s_mock_cookie_secret = 0;

static void mock_cookie_for(const struct sockaddr_in* src,
                            uint8_t out[REND_COOKIE_LEN]) {
    if (s_mock_cookie_secret == 0) {
        /* Seeded once per process; never leaves this TU, so the only way
         * for a client to hold a valid cookie is to have received it. */
        s_mock_cookie_secret =
            0x9E3779B97F4A7C15ull ^ ((uint64_t)SDL_GetTicks() << 21) ^
            ((uint64_t)(uintptr_t)&s_mock_cookie_secret);
        if (s_mock_cookie_secret == 0) s_mock_cookie_secret = 0xD1B54A32D192ED03ull;
    }
    uint8_t material[14];
    memcpy(&material[0], &src->sin_addr.s_addr, 4);
    memcpy(&material[4], &src->sin_port, 2);
    for (int i = 0; i < 8; i++) {
        material[6 + i] = (uint8_t)((s_mock_cookie_secret >> (8 * i)) & 0xFFu);
    }
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < sizeof(material); i++) {
        h ^= material[i];
        h *= 0x100000001B3ull;
    }
    for (int i = 0; i < REND_COOKIE_LEN; i++) {
        out[i] = (uint8_t)((h >> (8 * i)) & 0xFFu);
    }
    /* An all-zero cookie is the wire's "no cookie" sentinel; never emit
     * one or the gate would accept an uncookied request. */
    if (out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0 &&
        out[4] == 0 && out[5] == 0 && out[6] == 0 && out[7] == 0) {
        out[0] = 0x5A;
    }
}

/* Build the 32-byte server->client CHALLENGE exactly as
 * docs/plan-netplay-connection.md §6.4 specifies:
 *   magic(4) ver(1) type(1)=4 reserved(2) key(16) cookie(8).
 * The key echoed back is the one the REQUEST carried — that is what
 * Rendezvous_ParseChallenge's session-key gate matches against. */
static int build_challenge(uint8_t out[REND_CHALLENGE_LEN],
                           const uint8_t key[REND_KEY_LEN],
                           const uint8_t cookie[REND_COOKIE_LEN]) {
    memset(out, 0, REND_CHALLENGE_LEN);
    out[0] = REND_MAGIC_BYTES_0;
    out[1] = REND_MAGIC_BYTES_1;
    out[2] = REND_MAGIC_BYTES_2;
    out[3] = REND_MAGIC_BYTES_3;
    out[4] = (uint8_t)REND_VERSION;
    out[5] = (uint8_t)REND_TYPE_CHALLENGE;
    /* reserved [6..7] = 0 */
    memcpy(&out[8], key, REND_KEY_LEN);
    memcpy(&out[24], cookie, REND_COOKIE_LEN);
    return REND_CHALLENGE_LEN;
}

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
    ctx->started_ms = SDL_GetTicks(); /* S6 test 20C: deliver_delay_ms base */
    const long long start = (long long)time(NULL);
    const long long life = (ctx->life_secs > 0) ? (long long)ctx->life_secs : 5;

    for (;;) {
        if (ctx->stop) return 0;
        if ((long long)time(NULL) - start > life) return 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->sock, &rfds);
        struct timeval tv = { 0, 50 * 1000 };
        const int rc = select(ctx->sock + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) continue;
        if (!FD_ISSET(ctx->sock, &rfds)) continue;

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
        const uint8_t* req_key = &buf[8];

        if (type != REND_TYPE_REGISTER && type != REND_TYPE_POLL) continue;

        /* S4c return-routability gate. Runs between "the frame is
         * well-formed" and "we touch any state" — exactly where
         * returnRoutabilityGate sits in rendezvous-server.js:1529 — so an
         * uncookied/invalid-cookied request binds NOTHING: no session
         * slot, no endpoint recorded, no DELIVER. Only the echo binds. */
        if (ctx->challenge_enabled) {
            uint8_t want[REND_COOKIE_LEN];
            mock_cookie_for(&src, want);
            const bool cookie_ok =
                memcmp(&buf[28], want, REND_COOKIE_LEN) == 0;
            if (!cookie_ok) {
                ctx->uncookied_requests++;
                if (ctx->bound_ep_valid &&
                    ctx->bound_ep.sin_addr.s_addr == src.sin_addr.s_addr &&
                    ctx->bound_ep.sin_port == src.sin_port) {
                    ctx->uncookied_after_bind++;
                }
            }
            if (!cookie_ok || ctx->never_accept) {
                uint8_t chal[REND_CHALLENGE_LEN];
                const int cl = build_challenge(chal, req_key, want);
                memcpy(ctx->last_cookie, want, REND_COOKIE_LEN);
                ctx->challenges_sent++;
                sendto(ctx->sock, (const char*)chal, cl, 0,
                       (struct sockaddr*)&src, sl);
                continue; /* bind nothing */
            }
            ctx->cookied_requests++;
            ctx->bound_ep = src;
            ctx->bound_ep_valid = true;
        }

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
        struct sockaddr_in synth;
        if (ctx->use_synth_peer) {
            /* Single-client tests: the "other side" is fabricated. Until
             * min_cookied_before_peer cookied requests have landed we
             * answer with the zero-sentinel DELIVER ("peer not yet
             * registered"), which every client treats as "keep waiting"
             * — that is the window in which the rendezvous WORKER
             * thread's next periodic resend can be observed. */
            const bool delay_done =
                ctx->deliver_delay_ms <= 0 ||
                (int)(SDL_GetTicks() - ctx->started_ms) >= ctx->deliver_delay_ms;
            if (delay_done && ctx->cookied_requests >= ctx->min_cookied_before_peer) {
                memset(&synth, 0, sizeof(synth));
                synth.sin_family = AF_INET;
                synth.sin_addr.s_addr = ctx->synth_peer_ip_be;
                peer = &synth;
                peer_pub_port = ctx->synth_peer_port;
            }
        } else if (s->ep_count == 2) {
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
    if (!Rendezvous_DeriveSessionKey(0x01020304u, 1234, 0x111, key)) {
        FAIL("test1", "Rendezvous_DeriveSessionKey returned false");
        goto cleanup_fail;
    }

    /* Build REGISTER packets. Each client claims its own bound port as
     * its public port — the mock will echo that back inside the DELIVER. */
    uint8_t reg_a[REND_REGISTER_LEN];
    uint8_t reg_b[REND_REGISTER_LEN];
    if (!Rendezvous_BuildRegister(client_a_port, key, NULL, reg_a) ||
        !Rendezvous_BuildRegister(client_b_port, key, NULL, reg_b)) {
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

/* test_session_key_stability MOVED to src/netplay/test_netplay_units.c
 * (task #132 P3) — pure, no socket, no thread. */

/* --- Test 3: LAN-bypass truth table ----------------------------------- */

/* test_lan_bypass MOVED to src/netplay/test_netplay_units.c (#132 P3). */

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
    if (!Rendezvous_DeriveSessionKey(0x01020304u, 4321, 0x333, key)) {
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
        if (!Rendezvous_BuildRegister(peer_port, key, NULL, reg_p)) {
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
        if (!Rendezvous_BuildRegister(client_port, key, NULL, reg_c)) {
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
        if (!Rendezvous_BuildPoll(key, NULL, poll_c)) {
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
 * (returns false, matching the CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL
 * default at config.c:103 — "off until user opts out"), and (b) the API
 * does not crash on a bogus key. The actual DISABLE_BILATERAL gate path
 * in join_thread_fn is exercised by Step 7 manual smoke testing.
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

/* S6: the seam moved from "replace the blocking Stun_HolePunch" to "does
 * THIS candidate ever confirm?" — the decision these tests were really
 * making. p2p_race consults the oracle ONCE per candidate, when the leg is
 * armed, and a leg under an override puts NOTHING on the wire, so the
 * harness stays offline. `s_mock_punch_calls` therefore counts armed punch
 * CANDIDATES, which is the same quantity it counted before (each pre-S6
 * phase armed exactly one). */
static DirectP2PPunchOracleResult mock_punch_oracle(const char* peer_ip,
                                                    uint16_t peer_port) {
    (void)peer_ip;
    (void)peer_port;
    s_mock_punch_calls++;
    return (s_mock_punch_succeed_from > 0 && s_mock_punch_calls >= s_mock_punch_succeed_from)
               ? DP2P_PUNCH_CONFIRM
               : DP2P_PUNCH_NEVER;
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
    DirectP2P_TestHook_SetPunchOracle(mock_punch_oracle);
    /* S6: the punch leg now lives for BILATERAL_PUNCH_MS (5000 ms
     * default) rather than the old 2500 ms direct-punch window, and this
     * test runs TWO full attempts. Bound the race so the two attempts fit
     * inside the existing 10 s wait-for-state budget — the properties
     * under test (attempt count, fresh local port per attempt) are
     * independent of how long a losing leg is allowed to run. */
    DirectP2P_TestHook_SetRaceBudgetMs(600);

    /* Room code for a LAN peer: after the (mocked) punch failure the
     * deterministic LAN bypass fails the attempt as FAILED_SYMMETRIC
     * before any rendezvous traffic — keeping this test offline. */
    struct in_addr lan_peer;
    lan_peer.s_addr = htonl(0x0A000001u); /* 10.0.0.1 */
    char code[ROOM_CODE_BUF_LEN] = { 0 };
    if (!RoomCode_Encode((uint32_t)lan_peer.s_addr, 5555, 0x0AA, code)) {
        FAIL("test6", "RoomCode_Encode failed");
        DirectP2P_TestHook_SetStunDiscover(NULL);
        DirectP2P_TestHook_SetPunchOracle(NULL);
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
    DirectP2P_TestHook_SetPunchOracle(NULL);
    DirectP2P_TestHook_SetRaceBudgetMs(0);

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
/* test_failure_taxonomy MOVED to src/netplay/test_netplay_units.c
 * (#132 P3) — a pure truth table that used to sit ~13 s into this run. */

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
    if (!RoomCode_Encode((uint32_t)host_ip.s_addr, 6000, 0x0BB, code)) {
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
    DirectP2P_TestHook_SetPunchOracle(mock_punch_oracle);
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
    DirectP2P_TestHook_SetPunchOracle(NULL);
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
/* The OK line's TEXT matters to some cases, not only its count. */
static char s_log_last_ok[512] = { 0 };
static SDL_LogOutputFunction s_prev_log_fn = NULL;
static void* s_prev_log_ud = NULL;

static void SDLCALL capture_log_fn(void* userdata, int category,
                                   SDL_LogPriority priority, const char* message) {
    if (message != NULL) {
        if (strncmp(message, "[netplay-connect] OK", 20) == 0) {
            s_log_ok_lines++;
            SDL_strlcpy(s_log_last_ok, message, sizeof(s_log_last_ok));
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
    DirectP2P_TestHook_SetPunchOracle(mock_punch_oracle);
    s_mock_discover_calls = 0;
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 1; /* direct punch succeeds on attempt 1 */

    struct in_addr host_ip;
    host_ip.s_addr = htonl(0xC6336407u); /* 198.51.100.7 (TEST-NET-2) */
    char code[ROOM_CODE_BUF_LEN] = { 0 };
    int rc = 0;
    if (!RoomCode_Encode((uint32_t)host_ip.s_addr, 6000, 0x0BB, code)) {
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
    DirectP2P_TestHook_SetPunchOracle(NULL);
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
/* test_host_datagram_gate MOVED to src/netplay/test_netplay_units.c (#132 P3). */

/* --- Test 11: S4c rendezvous v2 cookie wire codec --------------------- */

/*
 * Client half of return-routability. The server (tools/rendezvous-
 * server/rendezvous-server.js, exercised by __test_protocol.js) answers
 * an uncookied REGISTER with a CHALLENGE and binds nothing until the
 * cookie is echoed. This test pins the CLIENT's side of that contract:
 *
 *  - REGISTER/POLL are 36 bytes with an 8-byte cookie tail; NULL cookie
 *    encodes as all-zeros, which is precisely the "no cookie yet" form
 *    the server answers with a CHALLENGE. If the tail ever stopped being
 *    zeroed, an uncookied REGISTER would carry stack garbage and the
 *    client would be re-challenged forever.
 *  - Rendezvous_ParseChallenge accepts only a well-formed v2 CHALLENGE
 *    carrying OUR session key, and zeroes its output on every reject —
 *    a client that echoed a cookie lifted from a forged/cross-talk
 *    CHALLENGE would hand an attacker control of which cookie it uses.
 */
/* test_rendezvous_cookie_codec MOVED to src/netplay/test_netplay_units.c (#132 P3). */

/* --- Test 32: the inbound frame router, magic vs magic+version -------- */

/*
 * Rendezvous_HasMagic and Rendezvous_FrameType are two DIFFERENT tests
 * and the difference is load-bearing. Both still ship:
 *
 *   rendezvous.c:331  Rendezvous_HasMagic   — magic ONLY
 *   rendezvous.c:346  Rendezvous_FrameType  — magic AND version AND type
 *   direct_p2p.c:1973-1975 — the race's one shared receive path routes
 *                            with HasMagic ? FrameType : -1
 *   sdl_net_adapter.c:362,369 — the GekkoNet straggler drop
 *
 * These assertions used to live in the S5 relay codec test, purely
 * because a RELAY_GRANT happened to be the frame lying around to build
 * them on. The relay is gone; the property is not. The fixtures here are
 * a DELIVER and a CHALLENGE — REND_FRAME_DELIVER and REND_FRAME_CHALLENGE
 * (rendezvous.h:212-213); #122's NACK is a third, needing no fixture here.
 *
 * THE REGRESSION THIS EXISTS TO CATCH. sdl_net_adapter.c's straggler
 * drop used to test `Rendezvous_FrameType(...) != 0`, which returns 0
 * for ANY version != REND_VERSION. A non-v2 '3SXR' frame therefore
 * passed straight THROUGH the guard with data[0] == 0x33, was miscounted
 * as an InputAck (0x33 & 7 == 3), and reached GekkoNet as a packet of
 * type 51. The version-INDEPENDENT cases below (32-magic-v3-*) are what
 * fail if that ever comes back: they are the only assertions in this
 * tree that distinguish the two functions, and swapping HasMagic for
 * `FrameType(...) != 0` leaves every other test in the suite green.
 */
/* test_rendezvous_frame_router MOVED to src/netplay/test_netplay_units.c (#132 P3). */

/* --- Test 12: S4-review HIGH-1b host punch-gate throttle -------------- */

/*
 * Before this fix the host's punch gate had NO cap, NO per-source
 * throttle and NO backoff: s_host_unauth_drops was a log counter only.
 * The host answers every guess (correct token -> accepted + handed off,
 * wrong token -> silent drop, and the host waits FOREVER by design), so
 * it is a perfect brute-force oracle, drained at ~60 datagrams/second.
 *
 * The gate accounting takes an injected clock, so this test is fully
 * deterministic — no sleeping, no sockets. It asserts against the
 * SHIPPED thresholds (fetched via the limits hook) rather than a
 * hardcoded copy that could silently drift from the header.
 *
 * Every assertion here goes RED against the pre-fix behavior, because
 * pre-fix there is no mute to observe and no re-roll to be owed: with
 * host_punch_gate_note_bad neutralized to `return false` and
 * host_punch_gate_is_muted to `return false`, sub-tests 12a, 12b, 12d,
 * 12e and 12f all fail.
 */
/* test_punch_gate_throttle MOVED to src/netplay/test_netplay_units.c (#132 P3).
 * It drives an INJECTED clock, so it never needed this file. */

/* --- Tests 13-15: S4c client cookie handshake, end to end -------------- */

/*
 * Test 11 pins the CODEC. Nothing pinned the RUNTIME: no test ever
 * delivered a CHALLENGE to a running client, so the cross-thread cookie
 * seqlock (signal_cookie_publish / signal_cookie_snapshot), the host's
 * cookied REGISTER echo in host_handle_challenge, s_host_challenge_seen
 * ever becoming true, CONNECT_FAIL_COOKIE_REJECTED on the HOST path, and
 * the joiner's inline re-BuildRegister+resend were all unexercised.
 *
 * The class of bug that hid in there: host_handle_challenge builds its
 * echo with s_work.stun.public_port. Had it used s_work.advertised_port
 * — a plausible copy/paste from the session-key derivation two lines
 * above, which DOES use advertised_port — every UPnP host whose external
 * port differs from its STUN-observed port would send the server a
 * my_public_port that disagrees with its own UDP source port
 * (rendezvous-server.js:1243-1250 compares exactly those two). The two agree
 * today only because a reviewer read the code, not because a test says
 * so. Test 13 makes them DIFFER at runtime and asserts the echo carries
 * the STUN one.
 */

/* --- observing the wire through the RENDEZVOUS_SEND seam --------------- */

#define REND_SEND_LOG_MAX 64

typedef struct {
    uint32_t t_ms;
    uint16_t target_port;
    int      len;
    uint8_t  pkt[REND_REGISTER_PKT_LEN];
} RendSendRec;

static RendSendRec  s_send_log[REND_SEND_LOG_MAX];
static SDL_AtomicInt s_send_log_n = { 0 };

static void send_log_reset(void) {
    memset(s_send_log, 0, sizeof(s_send_log));
    SDL_SetAtomicInt(&s_send_log_n, 0);
}

static int send_log_count(void) {
    int n = SDL_GetAtomicInt(&s_send_log_n);
    return (n > REND_SEND_LOG_MAX) ? REND_SEND_LOG_MAX : n;
}

/* Records every packet direct_p2p.c pushes through RENDEZVOUS_SEND and
 * then performs the real send, so the machine under test keeps running
 * against the mock server. Both roles route here: the host's rend_q
 * drain AND its main-thread CHALLENGE echo (direct_p2p.c:4844), and the
 * joiner's inline signaling loop sends. */
static bool recording_rendezvous_send(NET_DatagramSocket* sock, NET_Address* target,
                                      uint16_t target_port, const uint8_t* pkt,
                                      size_t pkt_len) {
    const int slot = SDL_AddAtomicInt(&s_send_log_n, 1);
    if (slot >= 0 && slot < REND_SEND_LOG_MAX && pkt != NULL) {
        RendSendRec* r = &s_send_log[slot];
        r->t_ms = SDL_GetTicks();
        r->target_port = target_port;
        r->len = (int)((pkt_len > sizeof(r->pkt)) ? sizeof(r->pkt) : pkt_len);
        memcpy(r->pkt, pkt, (size_t)r->len);
    }
    if (sock == NULL || target == NULL || pkt == NULL || pkt_len == 0) {
        return false;
    }
    return NET_SendDatagram(sock, target, target_port, pkt, (int)pkt_len);
}

static bool rec_is_register(const RendSendRec* r) {
    return r->len >= REND_REGISTER_LEN &&
           r->pkt[0] == REND_MAGIC_BYTES_0 && r->pkt[1] == REND_MAGIC_BYTES_1 &&
           r->pkt[2] == REND_MAGIC_BYTES_2 && r->pkt[3] == REND_MAGIC_BYTES_3 &&
           r->pkt[4] == REND_VERSION && r->pkt[5] == REND_TYPE_REGISTER;
}

static bool rec_has_cookie(const RendSendRec* r) {
    if (r->len < REND_REGISTER_LEN) return false;
    for (int i = 28; i < REND_REGISTER_LEN; i++) {
        if (r->pkt[i] != 0) return true;
    }
    return false;
}

static uint16_t rec_my_public_port(const RendSendRec* r) {
    return (uint16_t)(((uint16_t)r->pkt[24] << 8) | r->pkt[25]);
}

/* Index of the first cookied REGISTER, or -1. */
static int send_log_first_cookied(void) {
    const int n = send_log_count();
    for (int i = 0; i < n; i++) {
        if (rec_is_register(&s_send_log[i]) && rec_has_cookie(&s_send_log[i])) return i;
    }
    return -1;
}

/* --- mock-server lifecycle helper -------------------------------------- */

/* Unblock the mock's select() and join it. Mirrors test 8's teardown. */
static void mock_server_stop(MockServerCtx* ctx, SDL_Thread* tid,
                             int server_sock, unsigned short server_port) {
    ctx->stop = true;
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    srv.sin_port = htons(server_port);
    uint8_t wake = 0;
    sendto(server_sock, (const char*)&wake, 1, 0,
           (struct sockaddr*)&srv, sizeof(srv));
    if (tid != NULL) SDL_WaitThread(tid, NULL);
    close_sock(server_sock);
}

/* --- host-side STUN seam that hands the test a live handle ------------- */

/* direct_p2p.c:3545 calls STUN_DISCOVER(&s_work.stun, ...) — the mock is
 * therefore handed a pointer to the orchestrator's own StunResult. Test
 * 13 keeps it so it can move stun.public_port AFTER the room code (and
 * with it advertised_port) has been latched, which is the only way to
 * make the two ports diverge without a live UPnP mapping. */
static StunResult* s_captured_stun = NULL;

static bool capturing_stun_discover(StunResult* result, uint16_t local_port,
                                    int timeout_ms) {
    if (!mock_stun_discover(result, local_port, timeout_ms)) return false;
    s_captured_stun = result;
    return true;
}

/* Pump DirectP2P_Tick (main-thread work: rend_q drain, host_tick_receive,
 * host_waiting_tick) until `want` is reached or the budget expires. */
static bool tick_until_state(DirectP2PState want, int budget_ms) {
    const uint32_t start = SDL_GetTicks();
    while ((int)(SDL_GetTicks() - start) < budget_ms) {
        if (DirectP2P_GetState() == want) return true;
        DirectP2P_Tick();
        SDL_Delay(5);
    }
    return DirectP2P_GetState() == want;
}

/* Pump Tick until `pred` holds or the budget expires. */
static bool tick_until(bool (*pred)(void), int budget_ms) {
    const uint32_t start = SDL_GetTicks();
    while ((int)(SDL_GetTicks() - start) < budget_ms) {
        if (pred()) return true;
        DirectP2P_Tick();
        SDL_Delay(5);
    }
    return pred();
}

static bool pred_first_cookied_send(void) {
    return send_log_first_cookied() >= 0;
}

/* Set for the duration of a test so the zero-arg tick predicates can see
 * the mock server's counters. */
static MockServerCtx* s_pred_ctx = NULL;

static bool pred_two_cookied_requests(void) {
    return s_pred_ctx != NULL && s_pred_ctx->cookied_requests >= 2;
}

/*
 * "A handoff has happened", observed the only way that cannot be faked
 * by internal state: do_handoff's own call counter.
 *
 * DIRECT_P2P_HANDOFF is NOT the same observable. On the host's bilateral
 * rung the worker raises s_bilateral_handoff_pending and Tick then does
 * set_state(HANDOFF) IMMEDIATELY BEFORE calling do_handoff
 * (direct_p2p.c:5752-5760), so a state-only wait can return with the
 * handoff arguments not yet written — and, if the pending flag were ever
 * dropped, would still be satisfied by any other path that publishes the
 * state. Counting do_handoff calls is what pins the host worker ->
 * s_bilateral_handoff_pending -> main-thread do_handoff chain.
 */
static bool pred_any_handoff(void) {
    int n = 0;
    DirectP2P_TestHook_LastHandoff(NULL, 0, NULL, NULL, &n);
    return n > 0;
}

/* --- Test 13: HOST cookie handshake, end to end ------------------------ */

static int test_host_cookie_handshake(void) {
    fprintf(stderr, "[test_bilateral_punch] test 13: HOST S4c cookie handshake "
                    "(CHALLENGE -> cookied echo -> seqlock -> DELIVER)\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown(); /* whatever ran before -> IDLE */

    unsigned short server_port = 0;
    int server_sock = open_udp_on_localhost(&server_port);
    if (server_sock < 0) {
        FAIL("test13", "failed to bind localhost UDP socket for mock server");
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.challenge_enabled = true;   /* the S4c gate is ON for this one */
    ctx.use_synth_peer = true;
    ctx.synth_peer_ip_be = htonl(0xC6336407u); /* 198.51.100.7 (TEST-NET-2) */
    ctx.synth_peer_port = 6000;
    /* Pair only on the SECOND cookied request, so the host stays in
     * HOST_WAITING long enough for the rendezvous worker's next periodic
     * resend — the packet that proves the seqlock published. */
    ctx.min_cookied_before_peer = 2;
    ctx.life_secs = 30;
    s_pred_ctx = &ctx;

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_s4c", &ctx);
    if (!tid) {
        FAIL("test13", "SDL_CreateThread failed");
        close_sock(server_sock);
        return 1;
    }

    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
        /* 1000 ms is the code's own floor (direct_p2p.c:3033); the
         * seqlock assertion needs one worker cadence to elapse. */
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS, "1000");
    }

    send_log_reset();
    s_captured_stun = NULL;
    s_mock_discover_calls = 0;
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 1; /* the bilateral punch succeeds */
    DirectP2P_TestHook_SetStunDiscover(capturing_stun_discover);
    DirectP2P_TestHook_SetPunchOracle(mock_punch_oracle);
    DirectP2P_TestHook_SetRendezvousSend(recording_rendezvous_send);
    DirectP2P_TestHook_ResetHandoff();

    /* Kill the UPnP probe. Without this the harness performs a REAL IGD
     * discovery and installs a REAL 1-hour UDP mapping on whatever
     * router the developer happens to be behind — a unit test must not
     * mutate the LAN. It also made these tests environment-dependent:
     * they took a different path depending on whether UPnP won. (This
     * needed Config_SetBool; the key is CFG_BOOL and has no
     * default_entries[] row, so Config_SetString would have installed a
     * CFG_STRING entry that Config_GetBool silently ignores.)
     * The port divergence test 13 asserts on is forced through the
     * captured StunResult below, not through UPnP, so nothing is lost. */
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    /* S7: same reason, the other backend. Without this a Linux test run
     * would find a real default gateway in /proc/net/route and install a
     * real 1-hour NAT-PMP mapping on the developer's router. (On macOS
     * gateway discovery reports nothing, so this is belt AND braces.) */
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    DirectP2P_BeginHost(0);
    if (!wait_for_state(DIRECT_P2P_HOST_WAITING, 25000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test13: state %d after budget, expected "
                "HOST_WAITING\n", (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
        goto done;
    }

    /* The room code IS the advertised tuple: decoding it gives exactly
     * the (ip, advertised_port, nonce) the host derives its session key
     * from — no peeking at direct_p2p.c statics required. */
    uint32_t adv_ip_be = 0, nonce = 0;
    uint16_t advertised_port = 0;
    if (RoomCode_Decode(DirectP2P_GetHostCode(), &adv_ip_be, &advertised_port,
                        &nonce) != ROOM_CODE_OK) {
        FAIL("test13", "could not decode the published host room code");
        rc = 1;
        goto done;
    }
    uint8_t expect_key[REND_KEY_LEN];
    if (!Rendezvous_DeriveSessionKey(adv_ip_be, advertised_port, nonce, expect_key)) {
        FAIL("test13", "Rendezvous_DeriveSessionKey failed for the advertised tuple");
        rc = 1;
        goto done;
    }

    /* Force the divergence the reviewer's latent bug needs to be visible.
     * advertised_port was latched from the UPnP-or-STUN choice at
     * direct_p2p.c:1665/1622 and is now frozen in the room code; moving
     * the STUN-observed port through the pointer the discover seam handed
     * us leaves the two permanently different. Done BEFORE the first Tick
     * so nothing has been drained yet. */
    if (s_captured_stun == NULL) {
        FAIL("test13", "STUN seam never handed us the orchestrator's StunResult");
        rc = 1;
        goto done;
    }
    const uint16_t stun_public_port = (uint16_t)(advertised_port + 777u);
    s_captured_stun->public_port = stun_public_port;
    if (stun_public_port == advertised_port) {
        FAIL("test13", "test setup failed to make the two ports differ");
        rc = 1;
        goto done;
    }

    /* 1) A CHALLENGE must reach the main thread and be answered. */
    if (!tick_until(pred_first_cookied_send, 12000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test13: no cookied REGISTER ever left the "
                "host (challenges_sent=%d sends=%d) — the CHALLENGE was not answered\n",
                ctx.challenges_sent, send_log_count());
        fail_count++;
        rc = 1;
        goto done;
    }
    EXPECT_TRUE("13-challenged", ctx.challenges_sent >= 1);

    const int echo_i = send_log_first_cookied();
    const RendSendRec* echo = &s_send_log[echo_i];

    /* 2) The echo is a v2 REGISTER carrying the ADVERTISED-tuple session
     *    key (a key derived from the STUN port would land in a different
     *    server slot and never pair). */
    EXPECT_TRUE("13-echo-is-register", rec_is_register(echo));
    EXPECT_TRUE("13-echo-session-key",
                memcmp(&echo->pkt[8], expect_key, REND_KEY_LEN) == 0);

    /* 3) The echo's cookie is the one the mock issued for the host's
     *    source endpoint — proof it came from RECEIVING the challenge. */
    EXPECT_TRUE("13-echo-cookie",
                memcmp(&echo->pkt[28], ctx.last_cookie, REND_COOKIE_LEN) == 0);

    /* 4) THE PIN. my_public_port must be the STUN-observed port, which
     *    the server compares against the datagram's own source port
     *    (rendezvous-server.js:1243-1250). advertised_port is the UPnP
     *    external port and is NOT what arrives at the server. */
    if (rec_my_public_port(echo) != stun_public_port) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test13: echoed REGISTER my_public_port=%u, "
                "expected stun.public_port=%u (advertised_port=%u). The CHALLENGE echo "
                "must carry the STUN-observed port — the one the server matches against "
                "the UDP source port.\n",
                (unsigned)rec_my_public_port(echo), (unsigned)stun_public_port,
                (unsigned)advertised_port);
        fail_count++;
        rc = 1;
    }

    /* 5) THE SEQLOCK. The cookie arrived on the MAIN thread
     *    (host_handle_challenge); the periodic REGISTERs are rebuilt on
     *    the rendezvous WORKER thread, so it can only reach them through
     *    signal_cookie_publish -> signal_cookie_snapshot.
     *
     *    "A later cookied packet exists" is NOT proof: with the publish
     *    broken, the worker's uncookied resend gets re-challenged and the
     *    main thread echoes AGAIN, so cookied traffic keeps flowing. The
     *    sound observable is the inverse — the mock counts any UNCOOKIED
     *    request from an endpoint that has already echoed. A working
     *    seqlock means the worker never sends the zero tail again. */
    if (!tick_until(pred_two_cookied_requests, 8000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test13: the server saw only %d cookied "
                "request(s) — the host stopped carrying the cookie\n",
                ctx.cookied_requests);
        fail_count++;
        rc = 1;
    }
    if (ctx.uncookied_after_bind != 0) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test13: %d UNCOOKIED request(s) arrived "
                "after the host had already echoed a valid cookie — the rendezvous "
                "worker's periodic REGISTERs are not seeing the published cookie "
                "(signal_cookie_publish / signal_cookie_snapshot)\n",
                ctx.uncookied_after_bind);
        fail_count++;
        rc = 1;
    }
    {   /* The later, worker-produced resend carries the same cookie and
         * the same advertised-tuple session key. */
        int worker_i = -1;
        const int n = send_log_count();
        for (int i = echo_i + 1; i < n; i++) {
            if (rec_is_register(&s_send_log[i]) && rec_has_cookie(&s_send_log[i]) &&
                (uint32_t)(s_send_log[i].t_ms - echo->t_ms) >= 400u) {
                worker_i = i;
                break;
            }
        }
        if (worker_i < 0) {
            FAIL("test13", "no cookied resend >=400ms after the echo");
            rc = 1;
        } else {
            EXPECT_TRUE("13-worker-cookie",
                        memcmp(&s_send_log[worker_i].pkt[28], &echo->pkt[28],
                               REND_COOKIE_LEN) == 0);
            EXPECT_TRUE("13-worker-session-key",
                        memcmp(&s_send_log[worker_i].pkt[8], expect_key,
                               REND_KEY_LEN) == 0);
        }
    }

    /* 6) The mock binds only on the cookie echo, and the pairing then
     *    completes all the way to HANDOFF (DELIVER -> bilateral punch ->
     *    main-thread handoff).
     *
     *    The wait is on do_handoff HAVING BEEN CALLED, not on the state
     *    alone. This is the suite's only wait on a handoff from a HOST
     *    session, and therefore its only coverage of the
     *    s_bilateral_handoff_pending chain: the punch worker raises the
     *    flag (direct_p2p.c:3383), Tick observes it, joins the worker,
     *    publishes HANDOFF and calls do_handoff
     *    (direct_p2p.c:5752-5760). A state-only wait is satisfied one
     *    statement earlier, before any handoff argument is written, and
     *    would also be satisfied by any other path that publishes the
     *    state — so both are waited on, state first. */
    if (!tick_until_state(DIRECT_P2P_HANDOFF, 15000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test13: state %d after the cookie bound, "
                "expected HANDOFF (cookied=%d challenges=%d)\n",
                (int)DirectP2P_GetState(), ctx.cookied_requests, ctx.challenges_sent);
        fail_count++;
        rc = 1;
    } else if (!tick_until(pred_any_handoff, 5000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test13: HOST published HANDOFF but "
                "do_handoff was never called (cookied=%d challenges=%d) — the "
                "s_bilateral_handoff_pending chain did not complete\n",
                ctx.cookied_requests, ctx.challenges_sent);
        fail_count++;
        rc = 1;
    } else {
        char hip[64] = { 0 };
        uint16_t hport = 0;
        int hplayer = 0, hcount = 0;
        DirectP2P_TestHook_LastHandoff(hip, (int)sizeof(hip), &hport, &hplayer,
                                       &hcount);
        /* The peer the mock synthesised is who we were handed. */
        if (hport != ctx.synth_peer_port || strcmp(hip, "198.51.100.7") != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test13: do_handoff got %s:%u, expected "
                    "the synthesised peer 198.51.100.7:%u\n",
                    hip, (unsigned)hport, (unsigned)ctx.synth_peer_port);
            fail_count++;
            rc = 1;
        }
        /* THE PLAYER NUMBER — the host half of the pair asserted on the
         * join side by 19-handoff-player2. The host is player 1
         * (direct_p2p.c:5228, and :4125 on the direct-receive rung).
         * Nothing else in this suite reads it: with both host sites
         * changed to 2 the endpoint, the timing, the state and the
         * status text are all still exactly right, GekkoNet is handed
         * identical local and remote roles on both peers, and no match
         * can start. */
        EXPECT_TRUE("13-handoff-player1", hplayer == 1);
        EXPECT_TRUE("13-one-handoff", hcount == 1);
    }
    EXPECT_TRUE("13-uncookied-never-bound", ctx.uncookied_requests >= 1);

done:
    DirectP2P_TestHook_SetRendezvousSend(NULL);
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetPunchOracle(NULL);
    DirectP2P_TestHook_RunTeardown(); /* releases any UPnP mapping; -> IDLE */
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    mock_server_stop(&ctx, tid, server_sock, server_port);
    s_captured_stun = NULL;
    s_pred_ctx = NULL;

    if (rc == 0 && fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 13 OK — uncookied REGISTER bound nothing, "
                "CHALLENGE answered on the main thread with my_public_port=%u "
                "(advertised_port=%u), cookie crossed the seqlock to the worker, "
                "pairing completed\n",
                (unsigned)stun_public_port, (unsigned)advertised_port);
        return 0;
    }
    return 1;
}

/* --- Test 14: HOST cookie never accepted -> COOKIE_REJECTED ------------ */

/*
 * §6.5's last row on the HOST side: "v2 client, v2 server that always
 * challenges but never accepts" must advise P2P_FAIL_COOKIE_REJECTED,
 * NOT the P2P_FAIL_RENDEZVOUS_DOWN / HOST_UNMAPPABLE the pre-S4c code
 * would have produced (a CHALLENGE is proof the server is alive).
 *
 * Cost note: CONNECT_HOST_ADVISORY_MS is a compile-time 30 s
 * (connect_fail.h:255), so this used to spend ~31 s of real wall clock.
 * DirectP2P_TestHook_SetHostAdvisoryScale scales the elapsed CLOCK
 * rather than the threshold, so the classifier still runs against the
 * shipped 30 s constant — the test is fast without testing a
 * test-only number.
 */

static int s_adv_lines = 0;
static char s_adv_last[512] = { 0 };

static void SDLCALL capture_advisory_log_fn(void* userdata, int category,
                                            SDL_LogPriority priority,
                                            const char* message) {
    if (message != NULL &&
        strncmp(message, "[netplay-connect] ADVISORY", 26) == 0) {
        s_adv_lines++;
        SDL_strlcpy(s_adv_last, message, sizeof(s_adv_last));
    }
    if (s_prev_log_fn != NULL) {
        s_prev_log_fn(s_prev_log_ud, category, priority, message);
    }
}

static bool pred_advisory_seen(void) {
    return s_adv_lines > 0;
}

static int test_host_cookie_rejected(void) {
    fprintf(stderr, "[test_bilateral_punch] test 14: HOST challenged-but-never-accepted "
                    "-> COOKIE_REJECTED\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();

    unsigned short server_port = 0;
    int server_sock = open_udp_on_localhost(&server_port);
    if (server_sock < 0) {
        FAIL("test14", "failed to bind localhost UDP socket for mock server");
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.challenge_enabled = true;
    ctx.never_accept = true;  /* challenges forever; zero DELIVERs */
    ctx.life_secs = 15; /* advisory now fires in ~1 s (advisory-scale seam) */

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_deaf", &ctx);
    if (!tid) {
        FAIL("test14", "SDL_CreateThread failed");
        close_sock(server_sock);
        return 1;
    }

    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS, "1000");
    }

    send_log_reset();
    s_captured_stun = NULL;
    s_mock_discover_calls = 0;
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 0;
    DirectP2P_TestHook_SetStunDiscover(capturing_stun_discover);
    DirectP2P_TestHook_SetPunchOracle(mock_punch_oracle);
    DirectP2P_TestHook_SetRendezvousSend(recording_rendezvous_send);
    /* No real router mutation (see test 13), and reach the shipped 30 s
     * CONNECT_HOST_ADVISORY_MS boundary in ~1 s of wall clock by scaling
     * the elapsed clock rather than the threshold. */
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    /* S7: same reason, the other backend. Without this a Linux test run
     * would find a real default gateway in /proc/net/route and install a
     * real 1-hour NAT-PMP mapping on the developer's router. (On macOS
     * gateway discovery reports nothing, so this is belt AND braces.) */
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    DirectP2P_TestHook_SetHostAdvisoryScale(60);

    DirectP2P_BeginHost(0);
    if (!wait_for_state(DIRECT_P2P_HOST_WAITING, 25000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test14: state %d after budget, expected "
                "HOST_WAITING\n", (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
        goto done;
    }

    s_adv_lines = 0;
    s_adv_last[0] = '\0';
    SDL_GetLogOutputFunction(&s_prev_log_fn, &s_prev_log_ud);
    SDL_SetLogOutputFunction(capture_advisory_log_fn, NULL);

    /* CONNECT_HOST_ADVISORY_MS is 30 s from the first HOST_WAITING tick. */
    const bool got_adv = tick_until(pred_advisory_seen, 40000);
    SDL_SetLogOutputFunction(s_prev_log_fn, s_prev_log_ud);

    if (!got_adv) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test14: no host-waiting advisory after 40s "
                "(challenges_sent=%d)\n", ctx.challenges_sent);
        fail_count++;
        rc = 1;
        goto done;
    }
    if (strstr(s_adv_last, ConnectFail_Code(CONNECT_FAIL_COOKIE_REJECTED)) == NULL) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test14: advisory was \"%s\", expected code=%s "
                "— a server that CHALLENGES is alive, so this is an auth failure, not a "
                "dead rendezvous\n",
                s_adv_last, ConnectFail_Code(CONNECT_FAIL_COOKIE_REJECTED));
        fail_count++;
        rc = 1;
    }
    /* The advisory also owns the overlay status line for this cause. */
    EXPECT_TRUE("14-status-text",
                strcmp(DirectP2P_GetStatusText(),
                       ConnectFail_UserText(CONNECT_FAIL_COOKIE_REJECTED)) == 0);
    /* challenge_seen=1 in the line is s_host_challenge_seen observed at
     * runtime — the flag test 11 could never reach. */
    EXPECT_TRUE("14-challenge-seen-flag", strstr(s_adv_last, "challenge_seen=1") != NULL);
    EXPECT_TRUE("14-zero-cookied-binds", ctx.cookied_requests == 0);

done:
    DirectP2P_TestHook_SetHostAdvisoryScale(1);
    DirectP2P_TestHook_SetRendezvousSend(NULL);
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetPunchOracle(NULL);
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    mock_server_stop(&ctx, tid, server_sock, server_port);
    s_captured_stun = NULL;

    if (rc == 0 && fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 14 OK — %d challenges, 0 binds, host advised "
                "%s\n", ctx.challenges_sent,
                ConnectFail_Code(CONNECT_FAIL_COOKIE_REJECTED));
        return 0;
    }
    return 1;
}

/* --- Test 15: JOINER cookie handshake ---------------------------------- */

/*
 * Part A — the joiner answers a CHALLENGE INLINE in its signaling loop:
 * one RTT to bind instead of waiting out the 500 ms resend cadence.
 * Pinned by timing the first cookied REGISTER.
 * Part B — the same mock, never accepting: budget expiry must classify
 * CONNECT_FAIL_COOKIE_REJECTED (connect_fail.c:166), only reachable
 * when the race's challenge_any evidence was set (direct_p2p.c:4166).
 * (The inline answer itself lives in the race's CHALLENGE arm.)
 */

static int test_joiner_cookie_handshake(void) {
    fprintf(stderr, "[test_bilateral_punch] test 15: JOINER S4c CHALLENGE answered "
                    "inline + COOKIE_REJECTED classification\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();

    unsigned short server_port = 0;
    int server_sock = open_udp_on_localhost(&server_port);
    if (server_sock < 0) {
        FAIL("test15", "failed to bind localhost UDP socket for mock server");
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.challenge_enabled = true;
    ctx.use_synth_peer = true;
    /* The DELIVER must carry a peer that is neither LAN nor our own
     * public IP (203.0.113.9 from mock_stun_discover). */
    ctx.synth_peer_ip_be = htonl(0xC0000209u); /* 192.0.0.9 — public, TEST */
    ctx.synth_peer_port = 6100;
    ctx.min_cookied_before_peer = 1; /* pair on the echo itself */
    ctx.life_secs = 40;

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_join", &ctx);
    if (!tid) {
        FAIL("test15", "SDL_CreateThread failed");
        close_sock(server_sock);
        return 1;
    }

    /* Host tuple in the room code: public, not ours, not LAN, so the
     * joiner reaches FALLBACK_SIGNALING (same shape as test 8). */
    struct in_addr host_ip;
    host_ip.s_addr = htonl(0xC6336407u); /* 198.51.100.7 */
    char code[ROOM_CODE_BUF_LEN] = { 0 };
    const uint32_t join_nonce = 0x0CC;
    if (!RoomCode_Encode((uint32_t)host_ip.s_addr, 6000, join_nonce, code)) {
        FAIL("test15", "RoomCode_Encode failed");
        rc = 1;
        goto done;
    }
    uint8_t expect_key[REND_KEY_LEN];
    if (!Rendezvous_DeriveSessionKey((uint32_t)host_ip.s_addr, 6000, join_nonce,
                                     expect_key)) {
        FAIL("test15", "Rendezvous_DeriveSessionKey failed");
        rc = 1;
        goto done;
    }

    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
    }
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetPunchOracle(mock_punch_oracle);
    DirectP2P_TestHook_SetRendezvousSend(recording_rendezvous_send);

    /* --- Part A: cooperative challenging server ------------------------ */
    DirectP2P_TestHook_SetSignalBudgetMs(4000);
    send_log_reset();
    s_mock_discover_calls = 0;
    s_mock_punch_calls = 0;
    /* call 1 = the DIRECT punch (must fail so we reach signaling);
     * call 2 = the bilateral punch after the DELIVER (must succeed). */
    s_mock_punch_succeed_from = 2;

    DirectP2P_BeginJoin(code);
    if (!wait_for_state(DIRECT_P2P_HANDOFF, 20000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test15A: state %d after budget, expected "
                "HANDOFF (challenges=%d cookied=%d status=\"%s\")\n",
                (int)DirectP2P_GetState(), ctx.challenges_sent, ctx.cookied_requests,
                DirectP2P_GetStatusText());
        fail_count++;
        rc = 1;
    } else {
        EXPECT_TRUE("15A-challenged", ctx.challenges_sent >= 1);
        const int first = send_log_first_cookied();
        if (first < 0) {
            FAIL("test15A", "joiner never sent a cookied REGISTER");
            rc = 1;
        } else if (first == 0) {
            FAIL("test15A", "first REGISTER was already cookied — the mock never "
                            "challenged, so nothing about the answer was tested");
            rc = 1;
        } else {
            const uint32_t dt = s_send_log[first].t_ms - s_send_log[0].t_ms;
            /* The joiner's periodic cadence is 500 ms. An INLINE answer
             * lands within one 50 ms loop tick of the challenge; waiting
             * out the cadence would land at >= 500 ms. */
            if (dt >= 400u) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: test15A: first cookied REGISTER went "
                        "out %u ms after the first REGISTER — the CHALLENGE was NOT "
                        "answered inline, it waited out the 500 ms resend cadence\n",
                        (unsigned)dt);
                fail_count++;
                rc = 1;
            }
            EXPECT_TRUE("15A-echo-session-key",
                        memcmp(&s_send_log[first].pkt[8], expect_key, REND_KEY_LEN) == 0);
            EXPECT_TRUE("15A-echo-cookie",
                        memcmp(&s_send_log[first].pkt[28], ctx.last_cookie,
                               REND_COOKIE_LEN) == 0);
            /* The joiner claims its own STUN-observed port, same field
             * and same rule as the host's echo. */
            EXPECT_TRUE("15A-echo-port",
                        rec_my_public_port(&s_send_log[first]) == 40000);
        }
        EXPECT_TRUE("15A-bound-on-echo", ctx.cookied_requests >= 1);
    }
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();

    /* --- Part B: challenges forever, never accepts --------------------- */
    ctx.never_accept = true;
    ctx.use_synth_peer = false;
    ctx.challenges_sent = 0;
    ctx.cookied_requests = 0;
    send_log_reset();
    s_mock_discover_calls = 0;
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 0; /* every punch fails */
    DirectP2P_TestHook_SetSignalBudgetMs(1000);

    DirectP2P_BeginJoin(code);
    if (!wait_for_state(DIRECT_P2P_FAILED_BILATERAL, 20000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test15B: state %d after budget, expected "
                "FAILED_BILATERAL\n", (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
    } else {
        const char* status = DirectP2P_GetStatusText();
        const char* want = ConnectFail_UserText(CONNECT_FAIL_COOKIE_REJECTED);
        if (strcmp(status, want) != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test15B: status \"%s\", expected \"%s\" "
                    "(challenges=%d). A server that CHALLENGES is alive: without "
                    "ev_challenge_any this misreports as \"%s\".\n",
                    status, want, ctx.challenges_sent,
                    ConnectFail_UserText(CONNECT_FAIL_RENDEZVOUS_DOWN));
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("15B-challenged", ctx.challenges_sent >= 1);
        EXPECT_TRUE("15B-never-bound", ctx.cookied_requests == 0);
    }
    DirectP2P_Cancel();

done:
    DirectP2P_TestHook_SetSignalBudgetMs(0);
    DirectP2P_TestHook_SetRendezvousSend(NULL);
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetPunchOracle(NULL);
    mock_server_stop(&ctx, tid, server_sock, server_port);

    if (rc == 0 && fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 15 OK — CHALLENGE answered inline (well "
                "inside the 500 ms cadence), pairing completed; never-accepting server "
                "classified %s\n", ConnectFail_Code(CONNECT_FAIL_COOKIE_REJECTED));
        return 0;
    }
    return 1;
}

/* --- Tests 18-19: S6 candidate racing ---------------------------------- */

/*
 * docs/plan-netplay-connection.md §8. Two properties, each chosen so that
 * a serial cascade CANNOT satisfy it — i.e. each one goes red if p2p_race
 * stops racing:
 *
 *   18  worst-case wall clock to terminal failure is bounded by the race
 *       budget, not by the SUM of the leg budgets
 *   19  the DELIVER endpoint is punched WHILE the room-code endpoint is
 *       still being punched (serially it could not even start until the
 *       direct window had closed)
 *
 * (Tests 20, 20C and 21 lived here too. All three were about ordering the
 * relay leg against the punches, so they went with the relay rung.)
 */

static int s_probe_sock = -1;

static DirectP2PPunchOracleResult probe_never_punch(const char* ip, uint16_t port) {
    (void)ip;
    (void)port;
    return DP2P_PUNCH_NEVER;
}

/* --- Test 18: worst-case join timing ---------------------------------- */

/*
 * Drives the REAL BeginJoin to terminal failure against a rendezvous
 * BLACK HOLE (a localhost UDP socket that is bound and never answers)
 * with a PUBLIC room code, so none of the LAN/hairpin bypasses fire and
 * every leg runs its full budget. Offline by construction: the punch legs
 * are oracle-driven and put nothing on the wire, and the "server" only
 * ever receives.
 *
 * The pre-S6 joiner spent its budgets SERIALLY — direct punch 2500 +
 * signal 8000 + bilateral 5000 = 15500 ms per attempt, x2
 * attempts for the S2 auto-retry = 31000 ms. Measured on the pre-S6 tree
 * with the equivalent probe: see docs/plan-netplay-connection.md §8.
 */
#define S6_WORST_CASE_BOUND_MS 22000u
#define S6_RACE_BOUND_MS 9000u
/* Scenario B bound — see the comment at the assertion. Tightened from
 * 14 000 ms by the S6 review (M-4): the neutralised (serialised) run
 * measured 14 216 ms against that bound, i.e. 216 ms — 1.5% — of margin,
 * so a slightly faster machine could have left this assertion unable to
 * fail. See the assertion comment for the re-measured margins. */
#define S6_CASCADE_BOUND_MS 12000u


/* Scenario B: every leg runs. The mock DELIVERs a synthetic peer at once,
 * so the seed punch candidate, the DELIVER punch candidate and the
 * rendezvous signalling leg are all alive at the same time. */
static uint32_t probe_run_cascade(const char* label) {
    unsigned short server_port = 0;
    const int server_sock = open_udp_on_localhost(&server_port);
    if (server_sock < 0) {
        fprintf(stderr, "[timing-probe] %s: could not bind the mock socket\n", label);
        return 0;
    }
    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.use_synth_peer = true;
    ctx.synth_peer_ip_be = htonl(0xCB0071F5u); /* 203.0.113.245 (TEST-NET-3) */
    ctx.synth_peer_port = 7100;
    ctx.min_cookied_before_peer = 0;
    ctx.life_secs = 60;
    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_probe", &ctx);
    if (!tid) {
        fprintf(stderr, "[timing-probe] %s: SDL_CreateThread failed\n", label);
        close_sock(server_sock);
        return 0;
    }
    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL, false);
    }

    uint32_t nonce = 0;
    char code[ROOM_CODE_BUF_LEN];
    struct in_addr peer_in;
    uint32_t elapsed = 0;
    if (RoomCode_GenerateNonce(&nonce) &&
        inet_pton(AF_INET, "198.51.100.7", &peer_in) == 1 &&
        RoomCode_Encode((uint32_t)peer_in.s_addr, 6000, nonce, code)) {
        const uint32_t t0 = SDL_GetTicks();
        DirectP2P_BeginJoin(code);
        bool left_idle = false;
        while ((int)(SDL_GetTicks() - t0) < 120000) {
            const DirectP2PState st = DirectP2P_GetState();
            if (st != DIRECT_P2P_IDLE) left_idle = true;
            if (st == DIRECT_P2P_FAILED_STUN || st == DIRECT_P2P_FAILED_PUNCH ||
                st == DIRECT_P2P_FAILED_SYMMETRIC || st == DIRECT_P2P_FAILED_BILATERAL ||
                st == DIRECT_P2P_HANDOFF || (left_idle && st == DIRECT_P2P_IDLE)) {
                elapsed = SDL_GetTicks() - t0;
                break;
            }
            SDL_Delay(5);
        }
        if (elapsed == 0) elapsed = SDL_GetTicks() - t0;
        fprintf(stderr,
                "[timing-probe] %s FULL CASCADE: BeginJoin -> terminal in %u ms "
                "(state=%d, 2 attempts, ~%u ms per attempt)\n",
                label, (unsigned)elapsed, (int)DirectP2P_GetState(),
                (unsigned)(elapsed / 2u));
    } else {
        fprintf(stderr, "[timing-probe] %s: could not build a room code\n", label);
    }
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    mock_server_stop(&ctx, tid, server_sock, server_port);
    return elapsed;
}

static uint32_t probe_run_once(const char* label) {
    unsigned short bh_port = 0;
    s_probe_sock = open_udp_on_localhost(&bh_port);
    if (s_probe_sock < 0) {
        FAIL("test18", "could not bind the black-hole socket");
        return 0;
    }
    char url[64];
    SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)bh_port);
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL, false);

    uint32_t nonce = 0;
    char code[ROOM_CODE_BUF_LEN];
    struct in_addr peer_in;
    if (!RoomCode_GenerateNonce(&nonce) ||
        inet_pton(AF_INET, "198.51.100.7", &peer_in) != 1 ||
        !RoomCode_Encode((uint32_t)peer_in.s_addr, 6000, nonce, code)) {
        FAIL("test18", "could not build a room code");
        close_sock(s_probe_sock);
        s_probe_sock = -1;
        return 0;
    }

    const uint32_t t0 = SDL_GetTicks();
    DirectP2P_BeginJoin(code);
    uint32_t elapsed = 0;
    /* BeginJoin spawns a worker that publishes its first state a moment
     * later, so IDLE only counts as terminal once we have SEEN the machine
     * leave it. Without this the probe reads the pre-spawn IDLE and
     * reports 0 ms — a measurement that cannot fail is worse than none. */
    bool left_idle = false;
    while ((int)(SDL_GetTicks() - t0) < 120000) {
        const DirectP2PState st = DirectP2P_GetState();
        if (st != DIRECT_P2P_IDLE) left_idle = true;
        if (st == DIRECT_P2P_FAILED_STUN || st == DIRECT_P2P_FAILED_PUNCH ||
            st == DIRECT_P2P_FAILED_SYMMETRIC || st == DIRECT_P2P_FAILED_BILATERAL ||
            st == DIRECT_P2P_HANDOFF || (left_idle && st == DIRECT_P2P_IDLE)) {
            elapsed = SDL_GetTicks() - t0;
            break;
        }
        SDL_Delay(5);
    }
    if (elapsed == 0) elapsed = SDL_GetTicks() - t0;
    fprintf(stderr,
            "[timing-probe] %s: BeginJoin -> terminal in %u ms (state=%d, 2 attempts, "
            "~%u ms per attempt)\n",
            label, (unsigned)elapsed, (int)DirectP2P_GetState(), (unsigned)(elapsed / 2u));
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    close_sock(s_probe_sock);
    s_probe_sock = -1;
    return elapsed;
}

static int test_race_worst_case_timing(void) {
    fprintf(stderr, "[test_bilateral_punch] test 18: S6 worst-case join timing\n");
    const int fails_before = fail_count;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetPunchOracle(probe_never_punch);
    const uint32_t elapsed = probe_run_once("S6");
    const uint32_t cascade_ms = probe_run_cascade("S6");
    const uint32_t race_ms = DirectP2P_TestHook_LastRaceMs();
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetPunchOracle(NULL);

    if (elapsed == 0 || elapsed >= S6_WORST_CASE_BOUND_MS) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test18: BeginJoin -> terminal took %u ms, "
                "expected < %u ms (S6 races the legs; the pre-S6 serial cascade needed "
                "~39000 ms)\n",
                (unsigned)elapsed, (unsigned)S6_WORST_CASE_BOUND_MS);
        fail_count++;
    }
    /* Each attempt's race must respect its OWN budget:
     * netplay-direct-p2p-race-budget-ms defaults to 8000; allow scheduler
     * slack on a loaded machine. */
    if (race_ms == 0 || race_ms > S6_RACE_BOUND_MS) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test18: last race ran %u ms, expected "
                "0 < race <= %u ms\n",
                (unsigned)race_ms, (unsigned)S6_RACE_BOUND_MS);
        fail_count++;
    }
    /* Scenario B — EVERY leg runs: the DELIVER arrives, so there is a
     * second punch candidate alongside the seed candidate and the
     * rendezvous signalling leg. This is the scenario that distinguishes
     * racing from seriality, and its bound is the one that actually
     * bites:
     *
     *   measured pre-S6 (worktree 3sx-mister-s6base @ 9eadde33, same probe)
     *       19354 ms total  ->  9677 ms/attempt
     *   measured post-S6
     *       10235 ms total  ->  5117 ms/attempt
     *   measured post-S6-review (same probe, relay leg still present)
     *       10229 ms total  ->  5114 ms/attempt
     *   measured after the relay rung was REMOVED (this tree, same probe)
     *       10231 ms total  ->  5115 ms/attempt
     *   measured under N6 (the DELIVER candidate deferred until the seed
     *   candidate has finished — i.e. the legs serialised again)
     *       16217 ms total  ->  8108 ms/attempt
     *
     * S6-review M-4: the old 14 000 ms bound sat 216 ms — 1.5% — below the
     * then-neutralised number, so a faster machine could have made this
     * assertion unable to fail, and a test that cannot fail is worse than
     * no test. 12 000 ms sits between the two MEASURED numbers with
     * 1 769 ms (17.3%) of headroom over the passing run and 4 217 ms
     * (26.0%) of margin under the neutralised one. Serialise the legs
     * again and this goes red on the clock — re-verified by the N6 run
     * above, which reported exactly this assertion.
     *
     * NOTE the relay's removal moved this number by 2 ms (10229 ->
     * 10231). That is the empirical half of the finding that the relay
     * leg was CONTAINED inside race_budget_ms rather than additive to
     * it. */
    if (cascade_ms == 0 || cascade_ms >= S6_CASCADE_BOUND_MS) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test18: full-cascade join took %u ms "
                "(~%u ms/attempt), expected < %u ms — the punch and signalling "
                "legs must OVERLAP\n",
                (unsigned)cascade_ms, (unsigned)(cascade_ms / 2u),
                (unsigned)S6_CASCADE_BOUND_MS);
        fail_count++;
    }
    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 18 OK — black-hole join %u ms total "
                "(~%u ms/attempt), full-cascade join %u ms total (~%u ms/attempt), "
                "last race %u ms\n",
                (unsigned)elapsed, (unsigned)(elapsed / 2u),
                (unsigned)cascade_ms, (unsigned)(cascade_ms / 2u), (unsigned)race_ms);
        return 0;
    }
    return 1;
}

/* --- Test 18: S7 NAT-PMP / PCP port-mapping backend -------------------- */

/*
 * S7 (docs/plan-netplay-connection.md §9). Three things are under test:
 *
 *   (A) the CODEC, pinned to LITERAL BYTES read off the RFC diagrams —
 *       RFC 6887 §7.1/§7.2/§11.1/§11.2 for PCP and RFC 6886 §3.2/§3.3/
 *       §3.4 for NAT-PMP — in the spirit of test 17. natpmp.c's encoder
 *       is NOT used to produce the expectations; if it and this test
 *       agreed on a wrong offset the test would be worthless.
 *
 *   (B) the CLIENT against a localhost mock gateway: PCP success, the
 *       RFC 6887 §9 downgrade to NAT-PMP, a refusal, and silence.
 *
 *   (C) that a NAT-PMP mapping lands in the SAME S1 §3.6 CGNAT gate the
 *       UPnP mapping already goes through — with a public/CGN pair of
 *       runs, because "the mapping was dropped" is only meaningful next
 *       to a run where it was kept.
 *
 * NOTHING here may touch the developer's real router. Two independent
 * guards: every client call goes through Natpmp_TestHook_SetGateway at a
 * 127.0.0.1 mock, and gateway discovery is Linux-only so a macOS run
 * cannot find a real gateway even if the hook were forgotten.
 */

/* ---- mock gateway ---- */

typedef enum {
    MOCK_GW_SILENT = 0,    /* answers nothing at all                        */
    MOCK_GW_PCP,           /* a PCP server: MAP -> SUCCESS                   */
    MOCK_GW_NATPMP,        /* PCP -> UNSUPP_VERSION v0; speaks NAT-PMP       */
    MOCK_GW_NATPMP_REFUSE, /* as above, but the MAP request is refused       */
    /* S7 review M-5.3: a PCP server that refuses with a HEADER-ONLY
     * 24-octet frame. RFC 6887 §8.3's floor is 24 octets, not the MAP
     * response's 60, and a refusing gateway that only ever sends the
     * common header used to look identical to a dead one. */
    MOCK_GW_PCP_SHORT_ERROR,
    /* S7 review M-5.4: grants a mapping but reports NO external address
     * (all-zeros). The S1 §3.6 CGNAT gate compares that address against
     * STUN's, so a mapping without one is unjudgeable. */
    MOCK_GW_PCP_NO_EXT_IP,
    MOCK_GW_NATPMP_NO_EXT_IP,
    /* S7 review H-6, the per-phase-budget half. Speaks NAT-PMP perfectly
     * but answers every PCP request with a well-formed MAP response
     * carrying the WRONG Mapping Nonce — i.e. NOISE on port 5351 that
     * the §11.4 matcher rejects and that therefore never ends the PCP
     * phase's wait. A gateway that is merely confused, a second PCP
     * client on the LAN, or an attacker all produce this. The point of
     * the test is that such noise must not be able to STARVE the
     * NAT-PMP fallback of its own retransmit budget. */
    MOCK_GW_PCP_NOISE_THEN_NATPMP,
} MockGwMode;

typedef struct {
    int sock;
    MockGwMode mode;
    volatile bool stop;
    uint32_t ext_ip_be;  /* external IP the gateway claims                  */
    uint16_t ext_port;   /* external port it grants                         */
    uint32_t lifetime_s; /* lifetime it grants (may differ from requested)  */
    /*
     * S7 review H-6: how long this gateway takes to answer, applied
     * INLINE in the receive loop so the mock is single-threaded and
     * falls behind exactly the way a real low-cost NAT box does. RFC
     * 6886 §3.1 names the shape: "NAT gateways are often low-cost
     * devices, with limited memory and CPU speed... In the case of a
     * slow NAT gateway that takes perhaps half a second to respond to a
     * NAT-PMP request, the client SHOULD respect this and allow the NAT
     * gateway to operate at the pace it can manage, and not overload it
     * by issuing requests faster than the rate it's answering them."
     *
     * A mock that replied late WITHOUT serialising would be a strictly
     * easier target and would not reproduce the measured failure.
     */
    uint32_t reply_delay_ms;
    /* Seconds Since Start of Epoch to report (RFC 6886 §3.6 / RFC 6887
     * §7.2). Writable between calls so a test can make the gateway's
     * clock jump backwards, i.e. reboot. */
    uint32_t epoch_s;
    /* Observation counters + last frames, for byte-level assertions. */
    int pcp_reqs;
    int pmp_addr_reqs;
    int pmp_map_reqs;
    uint8_t last_pcp[128];
    int last_pcp_len;
    uint8_t last_pmp_map[64];
    int last_pmp_map_len;
    /* Arrival time of each PCP request, relative to the first, so a test
     * can pin the retransmit ladder's SHAPE and not merely its total. */
    uint64_t pcp_arrival_ms[16];
    uint64_t first_arrival_ms;
} MockGwCtx;

/* Every reply goes through here so the injected latency cannot be
 * forgotten on one branch. */
static void mock_gw_reply(MockGwCtx* ctx, const void* buf, size_t len,
                          const struct sockaddr_in* to, socklen_t tolen) {
    if (ctx->reply_delay_ms != 0) {
        SDL_Delay(ctx->reply_delay_ms);
    }
    sendto(ctx->sock, (const char*)buf, len, 0, (const struct sockaddr*)to, tolen);
}

static int SDLCALL mock_gateway_thread(void* arg) {
    MockGwCtx* ctx = (MockGwCtx*)arg;
    for (;;) {
        uint8_t buf[256];
        struct sockaddr_in from;
        memset(&from, 0, sizeof(from));
        socklen_t fl = sizeof(from);
        const int n = (int)recvfrom(ctx->sock, (char*)buf, sizeof(buf), 0,
                                    (struct sockaddr*)&from, &fl);
        if (ctx->stop) return 0;
        if (n <= 0) continue;

        /* RFC 6887 Appendix A: "The first octet of the packet indicates
         * if it is NAT-PMP (first octet zero) or PCP (first octet
         * non-zero)." The mock dispatches exactly that way. */
        if (buf[0] == 2) {
            {
                const uint64_t now = SDL_GetTicks();
                if (ctx->pcp_reqs == 0) ctx->first_arrival_ms = now;
                if (ctx->pcp_reqs <
                    (int)(sizeof(ctx->pcp_arrival_ms) / sizeof(ctx->pcp_arrival_ms[0]))) {
                    ctx->pcp_arrival_ms[ctx->pcp_reqs] = now - ctx->first_arrival_ms;
                }
            }
            ctx->pcp_reqs++;
            if (n <= (int)sizeof(ctx->last_pcp)) {
                memcpy(ctx->last_pcp, buf, (size_t)n);
                ctx->last_pcp_len = n;
            }
            if (ctx->mode == MOCK_GW_SILENT) continue;
            if (ctx->mode == MOCK_GW_PCP_SHORT_ERROR) {
                /* RFC 6887 §7.2 common response header ONLY — 24 octets,
                 * a legal length under §8.3's "Responses shorter than 24
                 * octets ... are invalid and ignored" floor. Result code
                 * 2 = NOT_AUTHORIZED (§7.4). */
                uint8_t r[NATPMP_PCP_HDR_LEN];
                memset(r, 0, sizeof(r));
                r[0] = 2;
                r[1] = 0x80u | 1u;
                r[3] = (uint8_t)NATPMP_PCP_NOT_AUTHORIZED;
                r[8] = (uint8_t)(ctx->epoch_s >> 24);
                r[9] = (uint8_t)(ctx->epoch_s >> 16);
                r[10] = (uint8_t)(ctx->epoch_s >> 8);
                r[11] = (uint8_t)(ctx->epoch_s);
                mock_gw_reply(ctx, r, sizeof(r), &from, fl);
                continue;
            }
            if (ctx->mode == MOCK_GW_PCP_NOISE_THEN_NATPMP) {
                /* A syntactically perfect 60-octet MAP response whose
                 * Mapping Nonce is not the one we sent. RFC 6887 §11.4
                 * matches "the protocol, the internal port, and the
                 * mapping nonce", so the client must treat it as not
                 * ours and keep waiting — which is exactly the state in
                 * which a shared deadline lets one phase eat the rest. */
                uint8_t r[60];
                memset(r, 0, sizeof(r));
                r[0] = 2;
                r[1] = 0x80u | 1u;
                r[3] = 0;
                r[7] = 60;
                memset(&r[24], 0x5A, 12); /* deliberately NOT our nonce */
                r[36] = buf[36];
                r[40] = buf[40]; r[41] = buf[41];
                r[42] = (uint8_t)(ctx->ext_port >> 8);
                r[43] = (uint8_t)(ctx->ext_port & 0xFFu);
                r[54] = 0xFF; r[55] = 0xFF;
                memcpy(&r[56], &ctx->ext_ip_be, 4);
                mock_gw_reply(ctx, r, sizeof(r), &from, fl);
                continue;
            }
            if (ctx->mode == MOCK_GW_PCP || ctx->mode == MOCK_GW_PCP_NO_EXT_IP) {
                /* Common Response Header (RFC 6887 §7.2, 24 bytes) +
                 * MAP response (§11.2, 36 bytes). Hand-built. */
                uint8_t r[60];
                memset(r, 0, sizeof(r));
                r[0] = 2;               /* Version = 2                     */
                r[1] = 0x80u | 1u;      /* R = 1 (response), Opcode = MAP  */
                r[2] = 0;               /* Reserved (8 bits)               */
                r[3] = 0;               /* Result Code = SUCCESS           */
                r[4] = (uint8_t)(ctx->lifetime_s >> 24);
                r[5] = (uint8_t)(ctx->lifetime_s >> 16);
                r[6] = (uint8_t)(ctx->lifetime_s >> 8);
                r[7] = (uint8_t)(ctx->lifetime_s);
                r[8] = (uint8_t)(ctx->epoch_s >> 24);   /* Epoch Time (§7.2) */
                r[9] = (uint8_t)(ctx->epoch_s >> 16);
                r[10] = (uint8_t)(ctx->epoch_s >> 8);
                r[11] = (uint8_t)(ctx->epoch_s);
                memcpy(&r[24], &buf[24], 12); /* Mapping Nonce, copied     */
                r[36] = buf[36];              /* Protocol, copied          */
                r[40] = buf[40];              /* Internal Port, copied     */
                r[41] = buf[41];
                r[42] = (uint8_t)(ctx->ext_port >> 8);
                r[43] = (uint8_t)(ctx->ext_port & 0xFFu);
                /* Assigned External IP as an IPv4-mapped IPv6 (§5). The
                 * NO_EXT_IP mode still sends a well-formed IPv4-mapped
                 * field — it just maps 0.0.0.0, which is what a gateway
                 * that has no WAN address yet reports. */
                r[54] = 0xFF; r[55] = 0xFF;
                if (ctx->mode != MOCK_GW_PCP_NO_EXT_IP) {
                    memcpy(&r[56], &ctx->ext_ip_be, 4);
                }
                mock_gw_reply(ctx, r, sizeof(r), &from, fl);
                continue;
            }
            /* NAT-PMP-only gateway. RFC 6886 §3.5: "If the version in the
             * request is not zero, then the NAT-PMP server MUST return
             * the following 'Unsupported Version' error response":
             * Vers=0 | OP=0 | Result Code = 1 | Epoch (32 bits). */
            uint8_t r[8];
            memset(r, 0, sizeof(r));
            r[0] = 0;
            r[1] = 0;
            r[2] = 0; r[3] = 1;
            r[4] = (uint8_t)(ctx->epoch_s >> 24);
            r[5] = (uint8_t)(ctx->epoch_s >> 16);
            r[6] = (uint8_t)(ctx->epoch_s >> 8);
            r[7] = (uint8_t)(ctx->epoch_s);
            mock_gw_reply(ctx, r, sizeof(r), &from, fl);
            continue;
        }

        if (buf[0] != 0 || ctx->mode == MOCK_GW_SILENT) continue;
        /* A PCP-only box ignores version 0. */
        if (ctx->mode == MOCK_GW_PCP || ctx->mode == MOCK_GW_PCP_NO_EXT_IP ||
            ctx->mode == MOCK_GW_PCP_SHORT_ERROR) {
            continue;
        }

        if (buf[1] == 0) {
            /* Public Address Response, RFC 6886 §3.2 (12 bytes). */
            ctx->pmp_addr_reqs++;
            uint8_t r[12];
            memset(r, 0, sizeof(r));
            r[0] = 0;
            r[1] = 128 + 0;
            r[2] = 0; r[3] = 0;   /* Result Code = 0 */
            r[4] = (uint8_t)(ctx->epoch_s >> 24); /* Seconds Since Start of Epoch */
            r[5] = (uint8_t)(ctx->epoch_s >> 16);
            r[6] = (uint8_t)(ctx->epoch_s >> 8);
            r[7] = (uint8_t)(ctx->epoch_s);
            if (ctx->mode != MOCK_GW_NATPMP_NO_EXT_IP) {
                memcpy(&r[8], &ctx->ext_ip_be, 4);
            }
            mock_gw_reply(ctx, r, sizeof(r), &from, fl);
            continue;
        }
        if (buf[1] == 1) {
            /* Mapping Response, RFC 6886 §3.3 (16 bytes). */
            ctx->pmp_map_reqs++;
            if (n <= (int)sizeof(ctx->last_pmp_map)) {
                memcpy(ctx->last_pmp_map, buf, (size_t)n);
                ctx->last_pmp_map_len = n;
            }
            const bool refuse = (ctx->mode == MOCK_GW_NATPMP_REFUSE);
            uint8_t r[16];
            memset(r, 0, sizeof(r));
            r[0] = 0;
            r[1] = 128 + 1;
            r[2] = 0;
            /* §3.5 result code 2 = Not Authorized/Refused. */
            r[3] = refuse ? 2 : 0;
            r[4] = (uint8_t)(ctx->epoch_s >> 24);
            r[5] = (uint8_t)(ctx->epoch_s >> 16);
            r[6] = (uint8_t)(ctx->epoch_s >> 8);
            r[7] = (uint8_t)(ctx->epoch_s);
            r[8] = buf[4]; r[9] = buf[5]; /* Internal Port, echoed */
            if (!refuse) {
                r[10] = (uint8_t)(ctx->ext_port >> 8);
                r[11] = (uint8_t)(ctx->ext_port & 0xFFu);
                r[12] = (uint8_t)(ctx->lifetime_s >> 24);
                r[13] = (uint8_t)(ctx->lifetime_s >> 16);
                r[14] = (uint8_t)(ctx->lifetime_s >> 8);
                r[15] = (uint8_t)(ctx->lifetime_s);
            }
            mock_gw_reply(ctx, r, sizeof(r), &from, fl);
            continue;
        }
    }
}

static void mock_gateway_stop(MockGwCtx* ctx, SDL_Thread* tid,
                              unsigned short port) {
    ctx->stop = true;
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    srv.sin_port = htons(port);
    uint8_t wake = 0xEE;
    sendto(ctx->sock, (const char*)&wake, 1, 0, (struct sockaddr*)&srv, sizeof(srv));
    if (tid != NULL) SDL_WaitThread(tid, NULL);
    close_sock(ctx->sock);
}

/* Bring up a mock gateway and aim the client at it. Returns the thread,
 * or NULL on failure. */
static SDL_Thread* mock_gateway_start(MockGwCtx* ctx, MockGwMode mode,
                                      uint32_t ext_ip_be, uint16_t ext_port,
                                      uint32_t lifetime_s) {
    unsigned short port = 0;
    memset(ctx, 0, sizeof(*ctx));
    ctx->sock = open_udp_on_localhost(&port);
    if (ctx->sock < 0) return NULL;
    ctx->mode = mode;
    ctx->ext_ip_be = ext_ip_be;
    ctx->ext_port = ext_port;
    ctx->lifetime_s = lifetime_s;
    /* Default Seconds Since Start of Epoch. Tests that care about RFC
     * 6886 §3.6 reboot detection overwrite ctx->epoch_s between calls;
     * everything else just needs a stable non-zero value. */
    ctx->epoch_s = 42u;
    SDL_Thread* tid = SDL_CreateThread(mock_gateway_thread, "natpmp_gw_mock", ctx);
    if (tid == NULL) {
        close_sock(ctx->sock);
        return NULL;
    }
    Natpmp_TestHook_SetGateway("127.0.0.1", (uint16_t)port);
    return tid;
}

/* The mock's own bound port, needed by mock_gateway_stop's wake-up. */
static unsigned short mock_gateway_port(const MockGwCtx* ctx) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    socklen_t sl = sizeof(a);
    if (getsockname(ctx->sock, (struct sockaddr*)&a, &sl) != 0) return 0;
    return ntohs(a.sin_port);
}

static int test_natpmp_pcp(void) {
    fprintf(stderr, "[test_bilateral_punch] test 22: S7 NAT-PMP / PCP backend\n");
    const int fails_before = fail_count;

    /* ================= (A) codec vs literal RFC bytes =================
     *
     * MOVED to src/netplay/test_netplay_units.c (task #132 P3). Every
     * assertion in it was pure — hand-built RFC 6886/6887 frames through
     * the Natpmp_Build and Natpmp_Parse entry points — and it sat
     * roughly 88 s into this
     * harness, behind the scenario tests above. What stays here is (B)
     * and (C): the client against a real mock gateway on a real socket,
     * which is what this file is for. */

    /* ================= (B) the client vs a mock gateway ================ */

    /* On this build's host platform the gateway lookup must report
     * NOTHING when the hook is clear — that is the second guard keeping
     * a test run off the developer's router. On Linux it may legitimately
     * find one, so only the no-hook-set invariant is asserted here. */
    Natpmp_TestHook_SetGateway(NULL, 0);
    Natpmp_TestHook_ResetState();
#if !defined(__linux__)
    {
        char gwip[64] = { 0 };
        EXPECT_FALSE("22-no-gateway-off-linux",
                     Natpmp_TestHook_DiscoverGateway(gwip, (int)sizeof(gwip)));
    }
#endif
    {
        /* S7 review, method rule 3: with NO mock gateway configured, a
         * HARNESS build must refuse to consult the real default route —
         * on EVERY platform, not just the macOS one that happens to have
         * no discovery implementation. This assertion is what makes the
         * "no test can touch a real router" claim hold when the suite
         * runs on Linux, which is the shipping platform. */
        UpnpMapping m;
        memset(&m, 0, sizeof(m));
        EXPECT_FALSE("22-addmapping-without-gateway",
                     Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NONE, 300));
        EXPECT_FALSE("22-addmapping-inactive", m.active);
        UpnpMapping rm;
        memset(&rm, 0, sizeof(rm));
        rm.active = true;
        rm.backend = PORTMAP_BACKEND_PCP;
        rm.internal_port = 54321;
        Natpmp_RemoveMapping(&rm); /* must not reach a real gateway either */
        EXPECT_FALSE("22-removemapping-without-gateway-inactive", rm.active);
    }

    /* --- B1: a PCP gateway --- */
    {
        struct in_addr ext;
        memset(&ext, 0, sizeof(ext));
        inet_pton(AF_INET, "198.51.100.20", &ext);
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_PCP, (uint32_t)ext.s_addr,
                                             40011, 1800);
        if (tid == NULL) {
            FAIL("22-b1", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            EXPECT_TRUE("22-b1-add",
                        Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NONE, 2000));
            EXPECT_TRUE("22-b1-active", m.active);
            EXPECT_TRUE("22-b1-backend", m.backend == PORTMAP_BACKEND_PCP);
            EXPECT_TRUE("22-b1-extport", m.external_port == 40011);
            EXPECT_TRUE("22-b1-intport", m.internal_port == 54321);
            EXPECT_TRUE("22-b1-lifetime", m.lifetime_s == 1800u);
            EXPECT_TRUE("22-b1-extip", strcmp(m.external_ip, "198.51.100.20") == 0);
            /* A PCP gateway must never have been asked in NAT-PMP. */
            EXPECT_TRUE("22-b1-no-natpmp", gw.pmp_addr_reqs == 0 && gw.pmp_map_reqs == 0);
            /* The PCP Client IP field must be the socket's real source
             * address (RFC 6887 §7.1) — 127.0.0.1 here — as an
             * IPv4-mapped IPv6 (§5). A hard-coded 0.0.0.0 would be an
             * ADDRESS_MISMATCH (§7.4) on a real server. */
            if (gw.last_pcp_len >= 24) {
                EXPECT_TRUE("22-b1-client-ip-mapped",
                            gw.last_pcp[18] == 0xFF && gw.last_pcp[19] == 0xFF);
                EXPECT_TRUE("22-b1-client-ip-loopback",
                            gw.last_pcp[20] == 127 && gw.last_pcp[23] == 1);
            } else {
                FAIL("22-b1-client-ip", "mock gateway never captured a PCP request");
            }
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* --- B2: a NAT-PMP-only gateway: RFC 6887 §9 downgrade --- */
    {
        struct in_addr ext;
        memset(&ext, 0, sizeof(ext));
        inet_pton(AF_INET, "198.51.100.21", &ext);
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_NATPMP, (uint32_t)ext.s_addr,
                                             40022, 3600);
        if (tid == NULL) {
            FAIL("22-b2", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            const uint64_t t0 = SDL_GetTicks();
            const bool ok = Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NONE, 2000);
            const uint64_t dt = SDL_GetTicks() - t0;
            if (!ok) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 18-b2-add: the client did not get a "
                        "mapping from a NAT-PMP-only gateway (pcp_reqs=%d addr_reqs=%d "
                        "map_reqs=%d). RFC 6887 §9 step 4 requires downgrading to NAT-PMP "
                        "on a version-zero UNSUPP_VERSION.\n",
                        gw.pcp_reqs, gw.pmp_addr_reqs, gw.pmp_map_reqs);
                fail_count++;
            }
            EXPECT_TRUE("22-b2-backend", m.backend == PORTMAP_BACKEND_NATPMP);
            EXPECT_TRUE("22-b2-extport", m.external_port == 40022);
            EXPECT_TRUE("22-b2-extip", strcmp(m.external_ip, "198.51.100.21") == 0);
            EXPECT_TRUE("22-b2-lifetime", m.lifetime_s == 3600u);
            EXPECT_TRUE("22-b2-tried-pcp-first", gw.pcp_reqs >= 1);
            /* The external IP can only have come from a §3.2 public
             * address request — a NAT-PMP mapping response carries none. */
            EXPECT_TRUE("22-b2-asked-address", gw.pmp_addr_reqs >= 1);
            EXPECT_TRUE("22-b2-asked-map", gw.pmp_map_reqs >= 1);
            /* The downgrade is immediate, not a timeout: the whole
             * exchange has to be far inside the budget. */
            EXPECT_TRUE("22-b2-fast", dt < 1000u);
            /* The mapping request the gateway actually received must be
             * the RFC 6886 §3.3 frame, not something reshaped in flight. */
            if (gw.last_pmp_map_len == 12) {
                EXPECT_TRUE("22-b2-wire-vers", gw.last_pmp_map[0] == 0);
                EXPECT_TRUE("22-b2-wire-op", gw.last_pmp_map[1] == 1);
                EXPECT_TRUE("22-b2-wire-intport",
                            ((gw.last_pmp_map[4] << 8) | gw.last_pmp_map[5]) == 54321);
                EXPECT_TRUE("22-b2-wire-lifetime",
                            gw.last_pmp_map[8] == 0 && gw.last_pmp_map[9] == 0 &&
                                gw.last_pmp_map[10] == 0x0E && gw.last_pmp_map[11] == 0x10);
            } else {
                FAIL("22-b2-wire", "mock gateway never captured a 12-byte NAT-PMP map request");
            }
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* --- B3: a NAT-PMP gateway that refuses --- */
    {
        struct in_addr ext;
        memset(&ext, 0, sizeof(ext));
        inet_pton(AF_INET, "198.51.100.22", &ext);
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_NATPMP_REFUSE,
                                             (uint32_t)ext.s_addr, 40033, 3600);
        if (tid == NULL) {
            FAIL("22-b3", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0xAA, sizeof(m));
            const bool ok = Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NONE, 2000);
            if (ok) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 18-b3-refused: a NAT-PMP result code "
                        "2 (Not Authorized/Refused, RFC 6886 §3.5) became a MAPPING. A "
                        "refusal must fail closed.\n");
                fail_count++;
            }
            EXPECT_FALSE("22-b3-inactive", m.active);
            EXPECT_TRUE("22-b3-zeroed",
                        m.external_port == 0 && m.external_ip[0] == '\0' &&
                            m.backend == PORTMAP_BACKEND_NONE);
            EXPECT_TRUE("22-b3-did-ask", gw.pmp_map_reqs >= 1);
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* --- B4: silence -> bounded timeout, no mapping, no hang --- */
    {
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_SILENT, 0, 0, 0);
        if (tid == NULL) {
            FAIL("22-b4", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0xAA, sizeof(m));
            const int budget = 1200;
            const uint64_t t0 = SDL_GetTicks();
            const bool ok = Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NONE,
                                              budget);
            const uint64_t dt = SDL_GetTicks() - t0;
            EXPECT_FALSE("22-b4-no-mapping", ok);
            EXPECT_FALSE("22-b4-inactive", m.active);
            /* The whole point of truncating RFC 6886 §3.1's ~127-second
             * ladder: the call must come back inside its budget. Slack is
             * generous because this machine runs the suite under load. */
            if (dt > (uint64_t)budget + 1500u) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 18-b4-bounded: a silent gateway held "
                        "Natpmp_AddMapping for %u ms against a %d ms budget — the RFC 6886 "
                        "§3.1 retransmit ladder is not being truncated by the deadline\n",
                        (unsigned)dt, budget);
                fail_count++;
            }
            /* ...and it really did retransmit rather than give up after
             * one datagram (§3.1's 250 ms / 500 ms rungs). */
            if (gw.pcp_reqs < 2) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 18-b4-retransmit: the gateway saw %d "
                        "request(s); RFC 6886 §3.1 wants a retransmit after 250 ms\n",
                        gw.pcp_reqs);
                fail_count++;
            }
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* --- B5: renewal stays on ONE backend --- */
    {
        struct in_addr ext;
        memset(&ext, 0, sizeof(ext));
        inet_pton(AF_INET, "198.51.100.23", &ext);
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_NATPMP, (uint32_t)ext.s_addr,
                                             40044, 120);
        if (tid == NULL) {
            FAIL("22-b5", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            /* A renewal hint of NATPMP must not emit a PCP probe at all. */
            EXPECT_TRUE("22-b5-renew",
                        Natpmp_AddMapping(&m, 54321, 40044, PORTMAP_BACKEND_NATPMP, 2000));
            EXPECT_TRUE("22-b5-backend", m.backend == PORTMAP_BACKEND_NATPMP);
            if (gw.pcp_reqs != 0) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 18-b5-no-pcp: a NAT-PMP renewal sent "
                        "%d PCP request(s); a renewal must run only the backend that holds "
                        "the mapping\n", gw.pcp_reqs);
                fail_count++;
            }
            /* RFC 6886 §3.3: a renewal SHOULD carry the ASSIGNED external
             * port so a rebooted gateway can recreate the same mapping. */
            if (gw.last_pmp_map_len == 12) {
                EXPECT_TRUE("22-b5-suggests-assigned",
                            ((gw.last_pmp_map[6] << 8) | gw.last_pmp_map[7]) == 40044);
            } else {
                FAIL("22-b5-wire", "no NAT-PMP map request captured");
            }
            /* And the SHORT lease the gateway granted is carried out, so
             * the caller can renew at ITS half-life rather than a fixed
             * half hour (RFC 6886 §3.3 "The NAT gateway MAY reduce the
             * lifetime from what the client requested"). */
            EXPECT_TRUE("22-b5-short-lease", m.lifetime_s == 120u);
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    Natpmp_TestHook_SetGateway(NULL, 0);

    /* ============ (C) the S1 §3.6 CGNAT gate, end to end ============== */

    /*
     * The gate lives at direct_p2p.c's host_thread_fn and is NOT
     * duplicated per backend — so the thing worth proving is that a
     * NAT-PMP mapping REACHES it. Observable: the port the published
     * room code carries.
     *
     *   mapping kept    -> the room code carries the MAPPED external port
     *   mapping dropped -> it carries the STUN-observed port
     *
     * The mock STUN seam reports 203.0.113.9:40000 (mock_stun_discover),
     * and the mock gateway grants a deliberately different external port,
     * so the two outcomes are distinguishable by a single integer.
     *
     * Run TWICE. A drop-only assertion would also pass if the NAT-PMP
     * mapping had never been created in the first place; the public-IP
     * run is the control that rules that out.
     */
    {
        struct {
            const char* tag;
            const char* ext_ip;
            bool expect_kept;
        } cases[] = {
            /* RFC 6598 shared address space, the CGN signature the gate
             * exists for (plan §3.6). */
            { "22-c-cgnat", "100.64.5.9", false },
            /* TEST-NET-2, public as far as the gate is concerned: 1:1
             * NAT / DMZ territory, mapping must be kept. */
            { "22-c-public", "198.51.100.30", true },
        };

        for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
            NET_Init();
            DirectP2P_Init();
            DirectP2P_TestHook_RunTeardown();

            struct in_addr ext;
            memset(&ext, 0, sizeof(ext));
            EXPECT_TRUE("22-c-pton", inet_pton(AF_INET, cases[ci].ext_ip, &ext) == 1);

            const uint16_t mapped_port = 41000;
            MockGwCtx gw;
            SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_NATPMP,
                                                 (uint32_t)ext.s_addr, mapped_port, 3600);
            if (tid == NULL) {
                FAIL(cases[ci].tag, "could not start the mock gateway");
                continue;
            }

            /* Keep this entirely offline: no real IGD (test 13's rule),
             * no rendezvous traffic, and STUN is the mock seam. */
            Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
            Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, false);
            Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL, true);
            /* NOTE: no punch seam is installed. This case only ever
             * drives host_thread_fn as far as HOST_WAITING — the CGNAT
             * gate fires between STUN discovery and the room-code
             * encode — and with the bilateral fallback off there is no
             * DELIVER and therefore no punch. Staying off the punch seam
             * keeps this test independent of how punching is mocked. */
            DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);

            DirectP2P_BeginHost(0);
            const bool reached = wait_for_state(DIRECT_P2P_HOST_WAITING, 25000);
            uint16_t adv_port = 0;
            if (!reached) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: %s: state %d after budget, expected "
                        "HOST_WAITING\n", cases[ci].tag, (int)DirectP2P_GetState());
                fail_count++;
            } else {
                uint32_t ip_be = 0, nnc = 0;
                if (RoomCode_Decode(DirectP2P_GetHostCode(), &ip_be, &adv_port, &nnc) !=
                    ROOM_CODE_OK) {
                    FAIL(cases[ci].tag, "could not decode the published host room code");
                } else {
                    const uint16_t want = cases[ci].expect_kept ? mapped_port : 40000;
                    if (adv_port != want) {
                        fprintf(stderr,
                                "[test_bilateral_punch] FAIL: %s: advertised port %u, "
                                "expected %u. The gateway reported external IP %s while "
                                "STUN reported 203.0.113.9; the S1 §3.6 CGNAT gate should "
                                "have %s the NAT-PMP mapping (mapped port %u, STUN port "
                                "40000).\n",
                                cases[ci].tag, (unsigned)adv_port, (unsigned)want,
                                cases[ci].ext_ip,
                                cases[ci].expect_kept ? "KEPT" : "DROPPED",
                                (unsigned)mapped_port);
                        fail_count++;
                    }
                }
                /* Proof the mapping was really attempted through the
                 * NAT-PMP path (and not, say, skipped entirely). */
                EXPECT_TRUE("22-c-mapped", gw.pmp_map_reqs >= 1);
            }

            DirectP2P_TestHook_SetStunDiscover(NULL);
            DirectP2P_TestHook_RunTeardown();
            if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
            Natpmp_TestHook_SetGateway(NULL, 0);
        }

        /* Leave the config as the rest of the suite expects. */
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL, false);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    }

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 22 OK — PCP/NAT-PMP frames match the RFC "
                "byte layouts, forged responses are rejected, the RFC 6887 §9 downgrade "
                "works, a silent gateway times out inside budget, and a NAT-PMP mapping "
                "with a CGN external IP is dropped by the S1 §3.6 gate while a public one "
                "is kept\n");
        return 0;
    }
    return 1;
}

/* ======================================================================= */
/* Test 23: the S7 adversarial-review fixes                                */
/* ======================================================================= */

/*
 * Everything here exists because an adversarial review found the S7
 * guard it covers either WRONG or UNTESTABLE. Five of the original S7
 * neutralisations were vacuous — the reviewer reverted the guarded
 * behaviour and the suite stayed green — so each section below names the
 * exact reversion it is built to catch.
 *
 * Nothing here may reach a real router. Two guards, one of them new:
 * every client call goes through Natpmp_TestHook_SetGateway at a
 * 127.0.0.1 mock, and a harness build (NETPLAY_TEST_HOOKS +
 * ENABLE_NETPLAY_TESTS) now REFUSES to consult the real default route
 * when no mock is set — see 22-addmapping-without-gateway.
 */

/* --- 23a: the DISABLE_UPNP / DISABLE_NATPMP pairing, asserted --------- *
 *
 * Method rule: no test may install a mapping on the developer's router.
 * Eight sites in this file disable UPnP before driving the host state
 * machine, and every one of them is hand-paired with a matching
 * disable-natpmp. Hand-maintained and, until now, unasserted: adding a
 * ninth site and forgetting the pair would silently aim the NAT-PMP
 * backend at whatever gateway the test machine actually has.
 *
 * A source-level discipline is asserted at the source. __FILE__ is
 * absolute here (CMake compiles with absolute paths), and a failure to
 * open it is a FAILURE, not a skip — a scan that silently matched
 * nothing would be exactly the vacuous test this whole exercise is
 * about.
 *
 * FOUR rules are checked, all by scanning this file's own source:
 *
 *   (1) PAIRING. Every line that sets DISABLE_UPNP to true must be
 *       followed, within the next two non-blank lines, by a line setting
 *       DISABLE_NATPMP to true. (Two, not one, because a case may then
 *       deliberately re-enable NAT-PMP against its own mock on the line
 *       after — which is what the CGNAT block does.)
 *
 *   (2) FLOOR. The scan must find at least as many disable-UPnP sites as
 *       exist today, so a scanner that silently stopped matching — or a
 *       deletion of a site — is caught. The floors are the MEASURED
 *       counts (8 disable-UPnP sites, 5 DirectP2P_BeginHost calls, both
 *       as reported by this test's own scan after the relay rung was
 *       removed), not a round number below them: an earlier floor of 11
 *       against 13 real sites meant two could be deleted with the suite
 *       still green.
 *       (Do not re-derive these with a plain grep and paste the number
 *       here — the pattern would match this comment, and the printed
 *       tally line below is the honest source.)
 *
 *   (3) COVERAGE — the rule the reviewer found missing. Rules (1) and
 *       (2) never said anything about the thing that actually matters:
 *       that every drive of the host state machine happens with UPnP
 *       off. DirectP2P_BeginHost is the entry into
 *       try_portmap/upnp_worker_fn, and upnp_worker_fn's ONLY brake is
 *       Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP)
 *       (src/netplay/direct_p2p.c:2479). So: every DirectP2P_BeginHost
 *       call site must have a disable-UPnP site within the preceding
 *       UPNP_SETUP_WINDOW_LINES lines. (Widest real gap today is 15
 *       lines; the window is 25, loose enough to survive a comment being
 *       added, tight enough that a setup block from an unrelated earlier
 *       test cannot vouch for a later BeginHost.)
 *
 *   (4) NO RE-ENABLE. Setting DISABLE_UPNP back to FALSE anywhere in
 *       this file is forbidden outright. Rule (3) checks that a site
 *       exists before each BeginHost; it cannot see a later line undoing
 *       it. Today there are zero such lines, and that is asserted rather
 *       than assumed.
 *
 * MATCHING IS WHITESPACE-INSENSITIVE. Each source line is squeezed to
 * its non-whitespace characters before matching, and the keys are built
 * squeezed to suit. A plain strstr() for one exact spelling — with the
 * one space after the comma that clang-format happens to emit today —
 * would miss a reflow or a hand-added space and quietly under-count,
 * which rule (2) would report as a scanner failure at best and which
 * rules (1), (3) and (4) would silently skip at worst. The keys are
 * still whole call expressions, so this is not a loose match: it
 * discriminates on the full function name, the full config key, and the
 * literal value.
 *
 * NOTE TO EDITORS: do not spell any of the four keys out in full,
 * contiguously, anywhere in this file — including in a comment like this
 * one. The scanner reads this file's own source, and a prose mention
 * would be counted as a site. That is why the keys below are assembled
 * at runtime from two halves on two separate source lines.
 */

/* Copy src into dst with every space, tab, CR and LF removed. */
static const char* squeeze_ws(char* dst, size_t dst_size, const char* src) {
    size_t j = 0;
    for (const char* p = src; *p != '\0' && j + 1 < dst_size; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            continue;
        dst[j++] = *p;
    }
    dst[j] = '\0';
    return dst;
}

/* Rule (3)'s window: how many source lines before a DirectP2P_BeginHost
 * call a disable-UPnP site may sit and still count as covering it. */
#define UPNP_SETUP_WINDOW_LINES 25

static int test_s7_disable_pairing(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 23a: DISABLE_UPNP sites pair with "
            "DISABLE_NATPMP\n");
    const int fails_before = fail_count;

    FILE* f = fopen(__FILE__, "r");
    if (f == NULL) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 23a-open: cannot read %s to verify the "
                "disable-UPnP/disable-NAT-PMP pairing discipline\n", __FILE__);
        fail_count++;
        return 1;
    }

    /* Built at runtime, and SPLIT ACROSS TWO PHYSICAL LINES, so this
     * scanner does not match ITSELF. Squeezing removes whitespace but
     * not newlines-between-lines: each half lands on its own source
     * line, and neither half is a whole key. KEEP THEM SPLIT. */
    char key_upnp[128];
    char key_natpmp[128];
    char key_upnp_off[128];
    char key_beginhost[128];
    SDL_snprintf(key_upnp, sizeof(key_upnp), "%s%s", "Config_SetBool(CFG_KEY_NETPLAY_",
                 "DIRECT_P2P_DISABLE_UPNP,true)");
    SDL_snprintf(key_natpmp, sizeof(key_natpmp), "%s%s", "Config_SetBool(CFG_KEY_NETPLAY_",
                 "DIRECT_P2P_DISABLE_NATPMP,true)");
    SDL_snprintf(key_upnp_off, sizeof(key_upnp_off), "%s%s", "Config_SetBool(CFG_KEY_NETPLAY_",
                 "DIRECT_P2P_DISABLE_UPNP,false)");
    SDL_snprintf(key_beginhost, sizeof(key_beginhost), "%s%s", "DirectP2P_",
                 "BeginHost(");

    int pending = 0;   /* lines still allowed to satisfy an open UPNP site */
    int upnp_line = 0; /* line number of that site                          */
    int lineno = 0;
    int sites = 0;
    int unpaired = 0;
    int last_disable_line = 0; /* rule (3): most recent disable-UPnP site  */
    int hosts = 0;             /* rule (3): DirectP2P_BeginHost call sites */
    int uncovered = 0;         /* rule (3): ... not covered by one         */
    int reenables = 0;         /* rule (4)                                 */
    char buf[512];
    char sq[512];
    while (fgets(buf, sizeof(buf), f) != NULL) {
        lineno++;
        const char* line = squeeze_ws(sq, sizeof(sq), buf);
        if (pending > 0) {
            /* Blank lines do not consume the window. */
            if (line[0] != '\0') {
                if (strstr(line, key_natpmp) != NULL) {
                    pending = 0;
                } else if (--pending == 0) {
                    fprintf(stderr,
                            "[test_bilateral_punch] FAIL: 23a-unpaired: %s:%d disables "
                            "UPnP but no disable-NAT-PMP follows within two lines. That "
                            "test would aim the NAT-PMP/PCP backend at the machine's "
                            "REAL default gateway.\n",
                            __FILE__, upnp_line);
                    fail_count++;
                    unpaired++;
                }
            }
        }
        /* Rule (4): a re-enable anywhere silently voids rule (3). */
        if (strstr(line, key_upnp_off) != NULL) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 23a-reenable: %s:%d sets "
                    "DISABLE_UPNP back to false. upnp_worker_fn's only brake is that "
                    "flag (src/netplay/direct_p2p.c:2301); with it clear, the next "
                    "DirectP2P_BeginHost in this harness aims miniupnpc at the "
                    "developer's REAL router and Upnp_AddMapping installs a 3600 s "
                    "lease on it.\n",
                    __FILE__, lineno);
            fail_count++;
            reenables++;
        }
        /* Rule (3): each BeginHost must be covered by a recent disable. */
        if (strstr(line, key_beginhost) != NULL) {
            hosts++;
            if (last_disable_line == 0 ||
                lineno - last_disable_line > UPNP_SETUP_WINDOW_LINES) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23a-uncovered-host: %s:%d calls "
                        "DirectP2P_BeginHost with no disable-UPnP site in the preceding "
                        "%d lines (nearest is line %d). BeginHost runs try_portmap, "
                        "whose UPnP leg is gated ONLY by that flag "
                        "(src/netplay/direct_p2p.c:2301) — this test would run real "
                        "SSDP discovery and a real UPNP_AddPortMapping against the "
                        "developer's router.\n",
                        __FILE__, lineno, UPNP_SETUP_WINDOW_LINES, last_disable_line);
                fail_count++;
                uncovered++;
            }
        }
        if (strstr(line, key_upnp) != NULL) {
            sites++;
            pending = 2;
            upnp_line = lineno;
            last_disable_line = lineno;
        }
    }
    if (pending > 0) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 23a-unpaired-eof: %s:%d disables UPnP at "
                "the end of the file with no disable-NAT-PMP after it\n",
                __FILE__, upnp_line);
        fail_count++;
        unpaired++;
    }
    fclose(f);

    /* Rule (2). The scan must have found the sites that exist. A scanner
     * that matched zero lines would "pass" forever. Both floors are the
     * measured counts, not round numbers below them: a floor two under
     * the truth lets two sites be deleted unnoticed.
     *
     * The floors were 13 / 6 until the relay rung was removed. Deleting
     * tests 16, 20, 20C, 21, 24, 25, 26 and 29 took five disable-UPnP
     * sites and one DirectP2P_BeginHost site with them (the host half of
     * the relay rung, test 16E), so the measured counts are now 8 / 5.
     * They are re-pinned to the new truth, NOT relaxed. */
    if (sites < 8) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 23a-scan: found only %d disable-UPnP "
                "site(s); 8 exist. Either the scanner is not matching — in which "
                "case its verdict means nothing — or a site was deleted and some "
                "test now drives the host state machine with UPnP live.\n", sites);
        fail_count++;
    }
    if (hosts < 5) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 23a-scan-hosts: found only %d "
                "DirectP2P_BeginHost call site(s); 5 exist. The coverage rule is not "
                "matching, so its verdict means nothing.\n", hosts);
        fail_count++;
    }
    fprintf(stderr,
            "[test_bilateral_punch] test 23a: scanned %d disable-UPnP site(s), %d "
            "unpaired; %d BeginHost site(s), %d uncovered; %d re-enable(s)\n",
            sites, unpaired, hosts, uncovered, reenables);

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 23a OK — every disable-UPnP site pairs "
                "with disable-NAT-PMP, every host drive is covered by one, and "
                "nothing re-enables UPnP\n");
        return 0;
    }
    return 1;
}

/* --- Test 33: every do_handoff call site's player number, at the source */

/*
 * do_handoff's first argument is the ONLY thing that tells GekkoNet
 * which side we are (direct_p2p.c:4376). It is a LITERAL at every call
 * site — nothing downstream can correct a wrong one — and two peers that
 * both hand off as the same number get identical local and remote roles,
 * so the session never starts.
 *
 * Three call sites ship (direct_p2p.c, as of this test):
 *
 *   :3953  host_tick_receive   — the DIRECT rung        -> 1
 *   :4599  Tick, on s_bilateral_handoff_pending          -> 1
 *   :3961  join_tick_handoff                             -> 2
 *
 * Only two of them have runtime coverage: :4599 via 13-handoff-player1
 * and :3961 via 19-handoff-player2. NOTHING in this suite reaches
 * host_tick_receive — a full run logs zero "Host received first inbound"
 * lines — so :3953 cannot be pinned by an end-to-end assertion without a
 * rig that does not exist. It is pinned HERE instead, at the source, the
 * same way test 23a pins the disable-UPnP discipline.
 *
 * The shape asserted is the INVARIANT, not a transcript: across all
 * call sites the multiset of literal player numbers must be exactly
 * {1, 1, 2}. Changing either host site to 2 (or the join site to 1)
 * makes it {2,2,2} / {1,1,1} and this goes red. Adding a fourth call
 * site also goes red, which is correct: a new handoff path is a decision
 * about player numbering and must be made deliberately.
 *
 * MATCHING is whitespace-squeezed, like 23a's, so a reflow cannot make
 * the scan silently under-count. The definition line
 * (`static void do_handoff(int player, ...)`) matches neither key.
 */
#define HANDOFF_P1_SITES 2
#define HANDOFF_P2_SITES 1

static int test_handoff_player_number_sites(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 33: every do_handoff call site's literal "
            "player number\n");
    const int fails_before = fail_count;

    /* __FILE__ is absolute (CMake compiles with absolute paths), so the
     * sibling source is found by swapping the basename. */
    char path[1024];
    SDL_strlcpy(path, __FILE__, sizeof(path));
    char* slash = strrchr(path, '/');
    if (slash == NULL) {
        FAIL("33-path", "__FILE__ is not an absolute path; cannot locate direct_p2p.c");
        return 1;
    }
    SDL_strlcpy(slash + 1, "direct_p2p.c", sizeof(path) - (size_t)(slash + 1 - path));

    FILE* f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 33-open: cannot read %s to verify the "
                "do_handoff player numbers\n", path);
        fail_count++;
        return 1;
    }

    /* Built at runtime from two halves on two separate source lines so
     * this scanner cannot match itself if it is ever pointed at this
     * file. KEEP THEM SPLIT. */
    char key_p1[64];
    char key_p2[64];
    SDL_strlcpy(key_p1, "do_hand", sizeof(key_p1));
    SDL_strlcat(key_p1, "off(1,", sizeof(key_p1));
    SDL_strlcpy(key_p2, "do_hand", sizeof(key_p2));
    SDL_strlcat(key_p2, "off(2,", sizeof(key_p2));

    int p1 = 0, p2 = 0, first_p1_line = 0, first_p2_line = 0, lineno = 0;
    char line[4096];
    char sq[4096];
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        lineno++;
        squeeze_ws(sq, sizeof(sq), line);
        if (strstr(sq, key_p1) != NULL) {
            p1++;
            if (first_p1_line == 0) first_p1_line = lineno;
        }
        if (strstr(sq, key_p2) != NULL) {
            p2++;
            if (first_p2_line == 0) first_p2_line = lineno;
        }
    }
    fclose(f);

    fprintf(stderr,
            "[test_bilateral_punch] test 33: %s: %d site(s) hand off as player 1 "
            "(first at :%d), %d as player 2 (first at :%d)\n",
            path, p1, first_p1_line, p2, first_p2_line);

    if (p1 != HANDOFF_P1_SITES || p2 != HANDOFF_P2_SITES) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 33-player-numbers: %s has %d do_handoff "
                "site(s) passing player 1 and %d passing player 2; expected %d and %d. "
                "The HOST rungs (host_tick_receive and the "
                "s_bilateral_handoff_pending rung in Tick) must both pass 1 and the "
                "JOIN rung must pass 2 — if a host site passes 2, both peers come up "
                "as player 2, GekkoNet gets identical local and remote roles and no "
                "match can start. A count of 0 means the scan is not matching, in "
                "which case its verdict means nothing.\n",
                path, p1, p2, HANDOFF_P1_SITES, HANDOFF_P2_SITES);
        fail_count++;
    }

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 33 OK — %d host site(s) hand off as "
                "player 1, %d join site as player 2\n", p1, p2);
        return 0;
    }
    return 1;
}

/* --- 23e: the harness may not run SSDP discovery ---------------------- *
 *
 * The companion to 22-addmapping-without-gateway, for the OTHER
 * port-mapping backend. natpmp.c's discover_gateway() has refused to
 * consult the real default route from a harness build for a while;
 * upnp.c had no equivalent, so a single forgotten
 * Config_SetBool(...DISABLE_UPNP, true) was all that stood between the
 * suite and upnpDiscover() multicasting M-SEARCH at the developer's
 * router — with UPNP_AddPortMapping and its one-hour lease two calls
 * further on. That is not hypothetical; it happened. upnp.c now refuses
 * (see upnp_ensure_cached()), and this test is what keeps the refusal
 * honest.
 *
 * SAFETY CONSTRAINT — DO NOT "IMPROVE" THIS TEST BY DRIVING
 * Upnp_AddMapping(). Read this before editing:
 *
 *   The value of this test is that it can be proven red, by deleting
 *   the refusal and re-running. That neutralised run REALLY DOES reach
 *   the network. Upnp_GetExternalIP() is read-only — at worst it does
 *   SSDP discovery plus UPNP_GetExternalIPAddress, and installs
 *   nothing. Upnp_AddMapping() is not: neutralised, it reaches
 *   UPNP_AddPortMapping() and leaves a real 3600 s mapping on whatever
 *   router answered. So this test drives ONLY the read-only entry
 *   point. Every entry point funnels through the same
 *   upnp_ensure_cached(), so the read-only one proves the refusal for
 *   all three anyway; there is nothing to gain and a live router
 *   mapping to lose.
 *
 * The assertion is the ATTEMPT COUNTER, not the return value. A test
 * that only checked the return would pass on a desk with no UPnP router
 * even with the refusal deleted — vacuous on some machines, meaningful
 * on others. The counter is incremented immediately before the one
 * upnpDiscover() call in the tree and so moves the instant the refusal
 * is gone, on every machine.
 */
static int test_upnp_harness_no_discovery(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 23e: a harness build refuses SSDP "
            "discovery\n");
    const int fails_before = fail_count;

    /* Cache can never be valid in a guarded build, but reset both so the
     * reading below is this test's and not some earlier test's. */
    Upnp_InvalidateCache();
    Upnp_TestHook_ResetDiscoverAttempts();

    char ext_ip[64];
    memset(ext_ip, 0, sizeof(ext_ip));
    const bool got = Upnp_GetExternalIP(ext_ip, (int)sizeof(ext_ip));

    const int attempts = Upnp_TestHook_DiscoverAttempts();
    if (attempts != 0) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 23e-discover-attempted: the harness "
                "entered upnpDiscover() %d time(s). A test build just multicast "
                "M-SEARCH on the developer's LAN; the refusal in "
                "upnp_ensure_cached() is gone, and Upnp_AddMapping() is now two "
                "calls from installing a real one-hour port mapping on their "
                "router.\n", attempts);
        fail_count++;
    }
    if (got) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 23e-external-ip: Upnp_GetExternalIP "
                "returned true (%s) in a harness build. With discovery refused "
                "there is no IGD to ask, so the only way to a real answer is a "
                "real router.\n", ext_ip);
        fail_count++;
    }

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 23e OK — 0 discovery attempts, "
                "Upnp_GetExternalIP declined\n");
        return 0;
    }
    return 1;
}

/* --- 23b: the S7 review fixes ---------------------------------------- */

static int test_s7_review_fixes(void) {
    fprintf(stderr, "[test_bilateral_punch] test 23b: S7 adversarial-review fixes\n");
    const int fails_before = fail_count;

    Natpmp_TestHook_SetGateway(NULL, 0);
    Natpmp_TestHook_ResetState();

    /* ============ H-7.2: the ladder's SHAPE ===========================
     *
     * MOVED to src/netplay/test_netplay_units.c (task #132 P3). The
     * behavioural companion below — the datagram COUNT against a silent
     * gateway — stays, because it needs the gateway. */

    /* A behavioural companion to the pin above: against a SILENT gateway
     * the PCP phase must send exactly as many datagrams as it has rungs,
     * at the rung intervals. Restoring the nine-rung ladder grows the
     * phase budget past the caller's overall ceiling and the count moves. */
    {
        Natpmp_TestHook_ResetState();
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_SILENT, 0, 0, 0);
        if (tid == NULL) {
            FAIL("23b-ladder-obs", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            (void)Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_PCP,
                                    NATPMP_PROBE_BUDGET_MS);
            if (gw.pcp_reqs != 3) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23b-ladder-count: a silent gateway "
                        "saw %d PCP request(s) in one phase; the truncated §3.1 ladder "
                        "has exactly 3 rungs\n", gw.pcp_reqs);
                fail_count++;
            } else {
                /* Arrival offsets, relative to the first: 0, 250, 750.
                 * Loose bounds — this box runs the suite under load — but
                 * tight enough that a 3 s IRT (RFC 6887 §8.1.1's own
                 * default) or a missing rung cannot pass. */
                const uint64_t lo[3] = { 0u, 150u, 550u };
                const uint64_t hi[3] = { 120u, 600u, 1400u };
                for (int i = 0; i < 3; i++) {
                    if (gw.pcp_arrival_ms[i] < lo[i] || gw.pcp_arrival_ms[i] > hi[i]) {
                        fprintf(stderr,
                                "[test_bilateral_punch] FAIL: 23b-ladder-timing%d: "
                                "request %d arrived at +%u ms, expected %u..%u ms "
                                "(§3.1 rungs 250/500/1000)\n",
                                i, i, (unsigned)gw.pcp_arrival_ms[i], (unsigned)lo[i],
                                (unsigned)hi[i]);
                        fail_count++;
                    }
                }
            }
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* ============ H-6: a slow-but-normal gateway gets a mapping ======= *
     *
     * RFC 6886 §3.1 calls out "a slow NAT gateway that takes perhaps half
     * a second to respond". Before the fix, three phases shared ONE
     * absolute deadline and a 700 ms gateway produced NO mapping: the
     * measured run spent 3896 ms, answered three public-address requests
     * and never got a mapping request as far as the gateway at all.
     *
     * 300 ms is the control: it worked before and must keep working.
     */
    {
        const uint32_t delays[] = { 300u, 700u };
        for (size_t di = 0; di < sizeof(delays) / sizeof(delays[0]); di++) {
            Natpmp_TestHook_ResetState();
            struct in_addr ext;
            memset(&ext, 0, sizeof(ext));
            inet_pton(AF_INET, "198.51.100.40", &ext);
            MockGwCtx gw;
            SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_NATPMP,
                                                 (uint32_t)ext.s_addr, 40055, 3600);
            if (tid == NULL) {
                FAIL("23b-slow-gw", "could not start the mock gateway");
                continue;
            }
            gw.reply_delay_ms = delays[di];
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            const uint64_t t0 = SDL_GetTicks();
            const bool ok = Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NONE,
                                              NATPMP_PROBE_BUDGET_MS);
            const uint64_t dt = SDL_GetTicks() - t0;
            fprintf(stderr,
                    "[test_bilateral_punch] 23b-slow-gw %u ms delay: mapping=%s "
                    "elapsed=%u ms pcp_reqs=%d addr_reqs=%d map_reqs=%d ext_port=%u\n",
                    (unsigned)delays[di], ok ? "YES" : "NO", (unsigned)dt, gw.pcp_reqs,
                    gw.pmp_addr_reqs, gw.pmp_map_reqs, (unsigned)m.external_port);
            if (!ok) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23b-slow-gw-%u: a gateway that "
                        "answers in %u ms produced NO mapping (elapsed %u ms, "
                        "map requests seen by the gateway: %d). RFC 6886 §3.1 names "
                        "\"perhaps half a second\" as slow-but-normal; each protocol "
                        "phase must get its own retransmit budget.\n",
                        (unsigned)delays[di], (unsigned)delays[di], (unsigned)dt,
                        gw.pmp_map_reqs);
                fail_count++;
            } else {
                EXPECT_TRUE("23b-slow-gw-extport", m.external_port == 40055);
                EXPECT_TRUE("23b-slow-gw-backend", m.backend == PORTMAP_BACKEND_NATPMP);
            }
            /* And the whole call still respects the advertised ceiling. */
            if (dt > (uint64_t)NATPMP_PROBE_BUDGET_MS + 1500u) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23b-slow-gw-bounded-%u: the call "
                        "took %u ms against a %d ms ceiling\n",
                        (unsigned)delays[di], (unsigned)dt, NATPMP_PROBE_BUDGET_MS);
                fail_count++;
            }
            /* §3.1: "not overload it by issuing requests faster than the
             * rate it's answering them." Once the gateway has answered
             * anything, the ladder must stop retransmitting — three full
             * rungs per phase into a 700 ms box is what starved the
             * mapping request in the first place. */
            if (gw.pmp_addr_reqs > 2) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23b-slow-gw-flood-%u: %d "
                        "public-address requests were issued at a gateway that had "
                        "already answered; RFC 6886 §3.1 says to let it work at its own "
                        "pace\n", (unsigned)delays[di], gw.pmp_addr_reqs);
                fail_count++;
            }
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* ===== H-6, the other half: one phase may not starve the next ===== *
     *
     * The 700 ms case above is fixed by the stop-retransmitting rule
     * alone. THIS case is what the per-phase budget is for.
     *
     * A gateway that answers PCP with unmatchable noise (wrong Mapping
     * Nonce — §11.4 says ignore it) but speaks NAT-PMP perfectly. The
     * client cannot end the PCP phase on any of those frames, and with
     * the retransmit suppression in force it waits out the phase. If
     * that wait runs to a SHARED deadline, the PCP phase consumes the
     * whole allowance and the NAT-PMP fallback — the working protocol,
     * on the same box — never gets a single datagram.
     */
    {
        Natpmp_TestHook_ResetState();
        struct in_addr ext;
        memset(&ext, 0, sizeof(ext));
        inet_pton(AF_INET, "198.51.100.44", &ext);
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_PCP_NOISE_THEN_NATPMP,
                                             (uint32_t)ext.s_addr, 40111, 3600);
        if (tid == NULL) {
            FAIL("23b-noise", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            const uint64_t t0 = SDL_GetTicks();
            const bool ok = Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NONE,
                                              NATPMP_PROBE_BUDGET_MS);
            const uint64_t dt = SDL_GetTicks() - t0;
            fprintf(stderr,
                    "[test_bilateral_punch] 23b-noise: mapping=%s elapsed=%u ms "
                    "pcp_reqs=%d addr_reqs=%d map_reqs=%d\n",
                    ok ? "YES" : "NO", (unsigned)dt, gw.pcp_reqs, gw.pmp_addr_reqs,
                    gw.pmp_map_reqs);
            if (!ok) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23b-noise-starved: unmatchable "
                        "PCP noise on port 5351 starved the NAT-PMP fallback (the "
                        "gateway saw %d address and %d mapping request(s) in %u ms). "
                        "Each protocol phase must get its OWN retransmit budget; a "
                        "shared deadline lets the first phase spend the whole "
                        "allowance.\n",
                        gw.pmp_addr_reqs, gw.pmp_map_reqs, (unsigned)dt);
                fail_count++;
            } else {
                EXPECT_TRUE("23b-noise-extport", m.external_port == 40111);
                EXPECT_TRUE("23b-noise-backend", m.backend == PORTMAP_BACKEND_NATPMP);
            }
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* ============ H-5: the PCP Mapping Nonce is PERSISTED ============= *
     *
     * RFC 6887 §11.3: "If operating in the Simple Threat Model (Section
     * 18.1), and the internal port, protocol, and internal address match
     * an existing explicit dynamic mapping, but the mapping nonce does
     * not match, the request MUST be rejected with a NOT_AUTHORIZED
     * error". A renewal with a fresh nonce is therefore refused by every
     * conforming gateway, and so is the teardown delete.
     *
     * Observed on the wire, at octets 24..36 of the frame the gateway
     * actually received (§11.1 Mapping Nonce, 96 bits).
     */
    {
        Natpmp_TestHook_ResetState();
        struct in_addr ext;
        memset(&ext, 0, sizeof(ext));
        inet_pton(AF_INET, "198.51.100.41", &ext);
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_PCP, (uint32_t)ext.s_addr,
                                             40066, 3600);
        if (tid == NULL) {
            FAIL("23b-nonce", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            EXPECT_TRUE("23b-nonce-add",
                        Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_PCP, 2000));
            uint8_t nonce_create[NATPMP_PCP_NONCE_LEN];
            memset(nonce_create, 0, sizeof(nonce_create));
            bool have_create = gw.last_pcp_len >= 36;
            if (have_create) {
                memcpy(nonce_create, &gw.last_pcp[24], NATPMP_PCP_NONCE_LEN);
            } else {
                FAIL("23b-nonce-create-frame", "no PCP request captured for the create");
            }
            /* A fresh nonce must not be all zeros — that would be the
             * CSPRNG failing open rather than the §18.1 fail-closed. */
            {
                bool all_zero = true;
                for (int i = 0; i < NATPMP_PCP_NONCE_LEN; i++) {
                    if (nonce_create[i] != 0) { all_zero = false; break; }
                }
                EXPECT_FALSE("23b-nonce-not-zero", all_zero);
            }

            /* THE RENEWAL. Same internal port, PCP hint — exactly what
             * upnp_renew_tick issues at half-lease. */
            UpnpMapping m2;
            memset(&m2, 0, sizeof(m2));
            EXPECT_TRUE("23b-nonce-renew",
                        Natpmp_AddMapping(&m2, 54321, 40066, PORTMAP_BACKEND_PCP, 2000));
            if (have_create && gw.last_pcp_len >= 36) {
                if (memcmp(&gw.last_pcp[24], nonce_create, NATPMP_PCP_NONCE_LEN) != 0) {
                    fprintf(stderr,
                            "[test_bilateral_punch] FAIL: 23b-nonce-renew-differs: the "
                            "renewal carried a DIFFERENT Mapping Nonce than the "
                            "creation. RFC 6887 §11.3 makes a conforming gateway reject "
                            "that with NOT_AUTHORIZED, so the mapping can never be "
                            "renewed and dies at the end of its lease.\n");
                    fail_count++;
                }
            }

            /* THE DELETE. §11.1 makes a delete a MAP with lifetime 0, so
             * §11.3's nonce rule applies to it too. */
            UpnpMapping del = m2;
            del.active = true;
            Natpmp_RemoveMapping(&del);
            EXPECT_FALSE("23b-nonce-del-inactive", del.active);
            if (have_create && gw.last_pcp_len >= 36) {
                /* lifetime 0 at octets 4..8 identifies it as the delete */
                const bool is_delete = gw.last_pcp[4] == 0 && gw.last_pcp[5] == 0 &&
                                       gw.last_pcp[6] == 0 && gw.last_pcp[7] == 0;
                EXPECT_TRUE("23b-nonce-del-is-delete", is_delete);
                if (memcmp(&gw.last_pcp[24], nonce_create, NATPMP_PCP_NONCE_LEN) != 0) {
                    fprintf(stderr,
                            "[test_bilateral_punch] FAIL: 23b-nonce-del-differs: the "
                            "delete carried a DIFFERENT Mapping Nonce than the mapping "
                            "it is deleting; RFC 6887 §11.3 rejects it with "
                            "NOT_AUTHORIZED and the mapping stays installed\n");
                    fail_count++;
                }
            }

            /* After a delete the mapping is gone, so the NEXT create for
             * that port is a new mapping and must draw a new nonce. */
            UpnpMapping m3;
            memset(&m3, 0, sizeof(m3));
            EXPECT_TRUE("23b-nonce-recreate",
                        Natpmp_AddMapping(&m3, 54321, 54321, PORTMAP_BACKEND_PCP, 2000));
            if (have_create && gw.last_pcp_len >= 36) {
                if (memcmp(&gw.last_pcp[24], nonce_create, NATPMP_PCP_NONCE_LEN) == 0) {
                    fprintf(stderr,
                            "[test_bilateral_punch] FAIL: 23b-nonce-stale-after-delete: "
                            "a create issued AFTER the delete reused the deleted "
                            "mapping's nonce\n");
                    fail_count++;
                }
            }
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* ====== M-5.3: a short PCP error response is processed, fast ====== *
     *
     * RFC 6887 §8.3: "Responses shorter than 24 octets, longer than 1100
     * octets, or not a multiple of 4 octets are invalid and ignored."
     * The old floor was the MAP response's 60 octets, so a gateway
     * refusing with a header-only frame looked SILENT and cost the whole
     * ladder.
     */
    {
        Natpmp_TestHook_ResetState();
        /* The codec half MOVED to src/netplay/test_netplay_units.c (#132 P3). */

        /* Now end to end: the client must give up FAST, not burn the
         * ladder waiting for a gateway that already said no. */
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_PCP_SHORT_ERROR, 0, 0, 0);
        if (tid == NULL) {
            FAIL("23b-short-e2e", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            const uint64_t t0 = SDL_GetTicks();
            const bool ok = Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NONE,
                                              NATPMP_PROBE_BUDGET_MS);
            const uint64_t dt = SDL_GetTicks() - t0;
            EXPECT_FALSE("23b-short-e2e-no-mapping", ok);
            if (dt > 1200u) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23b-short-e2e-fast: a gateway that "
                        "REFUSED with a 24-octet PCP error held the call for %u ms. "
                        "RFC 6887 §8.3 requires processing that response; a refusal is "
                        "a verdict, not silence.\n", (unsigned)dt);
                fail_count++;
            }
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* ====== M-5.1: RFC 6886 §3.6 gateway-reboot detection ============= */
    {
        Natpmp_TestHook_ResetState();
        struct in_addr ext;
        memset(&ext, 0, sizeof(ext));
        inet_pton(AF_INET, "198.51.100.42", &ext);
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_NATPMP, (uint32_t)ext.s_addr,
                                             40077, 3600);
        if (tid == NULL) {
            FAIL("23b-epoch", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            gw.epoch_s = 100000u; /* gateway has been up a while */
            EXPECT_TRUE("23b-epoch-add",
                        Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NATPMP,
                                          2000));
            /* A forward-moving clock is NOT a reboot. */
            EXPECT_FALSE("23b-epoch-no-false-positive", Natpmp_TakeEpochReset(NULL));
            gw.epoch_s = 100003u;
            UpnpMapping m2;
            memset(&m2, 0, sizeof(m2));
            EXPECT_TRUE("23b-epoch-renew",
                        Natpmp_AddMapping(&m2, 54321, 40077, PORTMAP_BACKEND_NATPMP,
                                          2000));
            EXPECT_FALSE("23b-epoch-still-no-reset", Natpmp_TakeEpochReset(NULL));

            /* THE REBOOT. §3.6: "If the NAT gateway resets or loses the
             * state of its port mapping table, due to reboot, power
             * failure, or any other reason, it MUST reset its epoch time
             * and begin counting SSSoE from zero again." */
            gw.epoch_s = 3u;
            UpnpMapping m3;
            memset(&m3, 0, sizeof(m3));
            EXPECT_TRUE("23b-epoch-after-reboot",
                        Natpmp_AddMapping(&m3, 54321, 40077, PORTMAP_BACKEND_NATPMP,
                                          2000));
            uint32_t jitter = 0xFFFFFFFFu;
            if (!Natpmp_TakeEpochReset(&jitter)) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23b-epoch-reset: the gateway's "
                        "Seconds Since Start of Epoch fell from 100003 to 3 and the "
                        "client did not notice. RFC 6886 §3.6 makes detecting that a "
                        "client MUST — it is the only signal that the router lost every "
                        "mapping it had.\n");
                fail_count++;
            }
            /* §3.7: "the client MUST first delay by a random amount of
             * time selected with uniform random distribution in the range
             * 0 to 5 seconds". */
            if (jitter > 5000u) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23b-epoch-jitter: %u ms, outside "
                        "RFC 6886 §3.7's 0..5000 ms window\n", (unsigned)jitter);
                fail_count++;
            }
            /* Consume-once: one reboot must not renew forever. */
            EXPECT_FALSE("23b-epoch-consumed", Natpmp_TakeEpochReset(NULL));
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* ====== M-5.4: no external address => no mapping (fail closed) ==== */
    {
        const MockGwMode modes[] = { MOCK_GW_PCP_NO_EXT_IP, MOCK_GW_NATPMP_NO_EXT_IP };
        const char* tags[] = { "23b-noextip-pcp", "23b-noextip-natpmp" };
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            Natpmp_TestHook_ResetState();
            MockGwCtx gw;
            SDL_Thread* tid = mock_gateway_start(&gw, modes[i], 0u, 40088, 3600);
            if (tid == NULL) {
                FAIL(tags[i], "could not start the mock gateway");
                continue;
            }
            UpnpMapping m;
            memset(&m, 0xAA, sizeof(m));
            const bool ok = Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NONE,
                                              NATPMP_PROBE_BUDGET_MS);
            if (ok) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: %s: a gateway that reported NO "
                        "external address still produced a mapping. The S1 §3.6 CGNAT "
                        "gate judges a mapping by comparing that address against STUN's; "
                        "with none there is nothing to compare, and the host would "
                        "advertise a port on an address it never learned.\n", tags[i]);
                fail_count++;
            }
            EXPECT_FALSE("23b-noextip-inactive", m.active);
            EXPECT_TRUE("23b-noextip-zeroed",
                        m.external_port == 0 && m.external_ip[0] == '\0');
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    /* The gate predicate's truth table MOVED to
     * src/netplay/test_netplay_units.c (#132 P3). */

    /* ====== H-7.3 / M-5.2: renewal cadence follows the GRANTED lease ==
     *
     * MOVED to src/netplay/test_netplay_units.c (#132 P3) — a pure
     * 5-row table over two TestHook trampolines. */

    /* ====== H-7.5: Natpmp_RemoveMapping refuses foreign backends ====== *
     *
     * The reviewer deleted the ownership check and the suite stayed
     * green. On a router that speaks both protocols, sending a NAT-PMP
     * lifetime-0 delete for a mapping that miniupnpc installed removes
     * whatever unrelated IGD entry happens to sit on that external port.
     */
    {
        Natpmp_TestHook_ResetState();
        struct in_addr ext;
        memset(&ext, 0, sizeof(ext));
        inet_pton(AF_INET, "198.51.100.43", &ext);
        const PortMapBackend foreign[] = { PORTMAP_BACKEND_UPNP, PORTMAP_BACKEND_NONE };
        for (size_t i = 0; i < sizeof(foreign) / sizeof(foreign[0]); i++) {
            MockGwCtx gw;
            SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_NATPMP,
                                                 (uint32_t)ext.s_addr, 40099, 3600);
            if (tid == NULL) {
                FAIL("23b-own", "could not start the mock gateway");
                continue;
            }
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            m.active = true;
            m.backend = foreign[i];
            m.internal_port = 54321;
            m.external_port = 40099;
            Natpmp_RemoveMapping(&m);
            EXPECT_FALSE("23b-own-cleared", m.active);
            if (gw.pcp_reqs != 0 || gw.pmp_map_reqs != 0 || gw.pmp_addr_reqs != 0) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23b-own-backend%d: "
                        "Natpmp_RemoveMapping sent %d PCP / %d NAT-PMP datagram(s) for a "
                        "mapping owned by backend %d. Deleting another backend's mapping "
                        "over the wrong protocol removes whatever unrelated entry sits on "
                        "that external port.\n",
                        (int)foreign[i], gw.pcp_reqs, gw.pmp_map_reqs, (int)foreign[i]);
                fail_count++;
            }
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
        /* The control: a mapping this backend DOES own really does emit a
         * delete through the same code path — without it, "sent nothing"
         * above would also be satisfied by a RemoveMapping that never
         * sends anything at all. */
        Natpmp_TestHook_ResetState();
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_NATPMP, (uint32_t)ext.s_addr,
                                             40099, 3600);
        if (tid == NULL) {
            FAIL("23b-own-control", "could not start the mock gateway");
        } else {
            UpnpMapping m;
            memset(&m, 0, sizeof(m));
            m.active = true;
            m.backend = PORTMAP_BACKEND_NATPMP;
            m.internal_port = 54321;
            m.external_port = 40099;
            Natpmp_RemoveMapping(&m);
            if (gw.pmp_map_reqs < 1) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23b-own-control: removing a "
                        "NAT-PMP-owned mapping sent NO delete; the refusal assertions "
                        "above would then pass vacuously\n");
                fail_count++;
            }
            /* RFC 6886 §3.4: lifetime 0 and Suggested External Port 0. */
            if (gw.last_pmp_map_len == 12) {
                EXPECT_TRUE("23b-own-control-lifetime0",
                            gw.last_pmp_map[8] == 0 && gw.last_pmp_map[9] == 0 &&
                                gw.last_pmp_map[10] == 0 && gw.last_pmp_map[11] == 0);
                EXPECT_TRUE("23b-own-control-extport0",
                            gw.last_pmp_map[6] == 0 && gw.last_pmp_map[7] == 0);
            }
            mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        }
    }

    Natpmp_TestHook_SetGateway(NULL, 0);
    Natpmp_TestHook_ResetState();

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 23b OK — ladder shape pinned, a 700 ms "
                "gateway gets a mapping, the PCP Mapping Nonce survives renewal and "
                "delete, short PCP errors fail fast, an epoch rollback is detected, a "
                "gateway with no external address is refused, the renewal cadence "
                "follows the granted lease, and a foreign backend's mapping is never "
                "deleted over this wire\n");
        return 0;
    }
    return 1;
}

/* --- 23c: the disable-natpmp kill switch, end to end ------------------ *
 *
 * The reviewer DELETED the kill switch and the suite stayed green,
 * because try_portmap held a second copy of the check that
 * short-circuited before the worker ran. That duplicate is gone; the
 * switch is enforced once, in upnp_worker_fn, and this drives the real
 * host state machine to prove it.
 *
 * Both halves matter:
 *   OFF  -> the mock gateway must see NOTHING, and the room code must
 *           carry the STUN-observed port.
 *   ON   -> the same setup must produce a mapping (the control; without
 *           it "saw nothing" would also pass if the plumbing were dead).
 */
static int test_s7_natpmp_kill_switch(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 23c: the disable-natpmp kill switch\n");
    const int fails_before = fail_count;

    struct { const char* tag; bool disabled; } cases[] = {
        { "23c-disabled", true },
        { "23c-enabled", false },
    };

    for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        NET_Init();
        DirectP2P_Init();
        DirectP2P_TestHook_RunTeardown();
        Natpmp_TestHook_ResetState();

        struct in_addr ext;
        memset(&ext, 0, sizeof(ext));
        EXPECT_TRUE("23c-pton", inet_pton(AF_INET, "198.51.100.50", &ext) == 1);
        const uint16_t mapped_port = 41100;
        MockGwCtx gw;
        SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_NATPMP, (uint32_t)ext.s_addr,
                                             mapped_port, 3600);
        if (tid == NULL) {
            FAIL(cases[ci].tag, "could not start the mock gateway");
            continue;
        }

        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, cases[ci].disabled);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL, true);
        DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);

        DirectP2P_BeginHost(0);
        const bool reached = wait_for_state(DIRECT_P2P_HOST_WAITING, 25000);
        if (!reached) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: %s: state %d after budget, expected "
                    "HOST_WAITING\n", cases[ci].tag, (int)DirectP2P_GetState());
            fail_count++;
        } else {
            uint32_t ip_be = 0, nnc = 0;
            uint16_t adv_port = 0;
            if (RoomCode_Decode(DirectP2P_GetHostCode(), &ip_be, &adv_port, &nnc) !=
                ROOM_CODE_OK) {
                FAIL(cases[ci].tag, "could not decode the published host room code");
            } else {
                const uint16_t want = cases[ci].disabled ? 40000 : mapped_port;
                if (adv_port != want) {
                    fprintf(stderr,
                            "[test_bilateral_punch] FAIL: %s: advertised port %u, "
                            "expected %u (netplay-direct-p2p-disable-natpmp=%d)\n",
                            cases[ci].tag, (unsigned)adv_port, (unsigned)want,
                            (int)cases[ci].disabled);
                    fail_count++;
                }
            }
            const int seen = gw.pcp_reqs + gw.pmp_addr_reqs + gw.pmp_map_reqs;
            if (cases[ci].disabled && seen != 0) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23c-disabled-silent: "
                        "netplay-direct-p2p-disable-natpmp was SET and the gateway still "
                        "received %d datagram(s) (%d PCP, %d addr, %d map). The kill "
                        "switch is the only thing standing between a user who set it and "
                        "a backend they asked not to run.\n",
                        seen, gw.pcp_reqs, gw.pmp_addr_reqs, gw.pmp_map_reqs);
                fail_count++;
            }
            if (!cases[ci].disabled && gw.pmp_map_reqs < 1) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 23c-enabled-control: with the kill "
                        "switch CLEAR the gateway saw no mapping request, so the "
                        "\"saw nothing\" assertion above proves nothing\n");
                fail_count++;
            }
        }

        DirectP2P_TestHook_SetStunDiscover(NULL);
        DirectP2P_TestHook_RunTeardown();
        if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
        mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
        Natpmp_TestHook_SetGateway(NULL, 0);
    }

    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL, false);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 23c OK — the disable-natpmp switch keeps "
                "the gateway silent, and clearing it produces a mapping\n");
        return 0;
    }
    return 1;
}

/* --- 23d: a LOST mapping stops being advertised ---------------------- *
 *
 * The other half of review H-5, and all of M-5.5. A gateway grants a
 * short lease and then goes silent. Before the fix the chain was:
 * renewal refused -> s_upnp_mapping.active stays true -> the lease
 * expires -> the room code and the drift re-encode keep pinning an
 * external port the router has already forgotten. The user reads out a
 * code that worked five minutes ago and nobody can connect to it.
 *
 * Driven end to end, because the interesting behaviour lives in the
 * interaction between the renewal tick, the lease clock and the room
 * code — not in any one of them.
 *
 * Wall clock: an 8 s lease renews at 4 s (half), the renewal fails after
 * one silent NAT-PMP phase, retries at +2 s, and the second failure
 * lands past expiry and drops the mapping. Budget generously.
 */
static int test_s7_lost_mapping(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 23d: a lost port mapping stops being "
            "advertised\n");
    const int fails_before = fail_count;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    Natpmp_TestHook_ResetState();

    struct in_addr ext;
    memset(&ext, 0, sizeof(ext));
    EXPECT_TRUE("23d-pton", inet_pton(AF_INET, "198.51.100.60", &ext) == 1);
    const uint16_t mapped_port = 41200;
    MockGwCtx gw;
    /* Lifetime 8 s: short enough to watch expire, long enough that the
     * half-lease renewal is above RFC 6887 §11.2.1's four-second floor. */
    SDL_Thread* tid = mock_gateway_start(&gw, MOCK_GW_NATPMP, (uint32_t)ext.s_addr,
                                         mapped_port, 8);
    if (tid == NULL) {
        FAIL("23d", "could not start the mock gateway");
        return 1;
    }

    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, false);
    /* Bilateral OFF keeps the rendezvous worker from spawning, here and
     * on the re-publish path (host_rendezvous_restart honours the same
     * switch), so nothing leaves this machine. */
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL, true);
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);

    DirectP2P_BeginHost(0);
    if (!wait_for_state(DIRECT_P2P_HOST_WAITING, 25000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 23d-host: state %d after budget, expected "
                "HOST_WAITING\n", (int)DirectP2P_GetState());
        fail_count++;
    } else {
        uint32_t ip_be = 0, nnc = 0;
        uint16_t adv0 = 0;
        EXPECT_TRUE("23d-decode",
                    RoomCode_Decode(DirectP2P_GetHostCode(), &ip_be, &adv0, &nnc) ==
                        ROOM_CODE_OK);
        /* The control: the mapping really was created and really is what
         * the code advertises. Without this, "the port reverted" below
         * would also pass if no mapping had ever existed. */
        if (adv0 != mapped_port) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 23d-control: the published code "
                    "advertises port %u, expected the MAPPED port %u — there is no live "
                    "mapping to lose, so this test would prove nothing\n",
                    (unsigned)adv0, (unsigned)mapped_port);
            fail_count++;
        }
        EXPECT_TRUE("23d-lifetime-honoured", gw.pmp_map_reqs >= 1);

        /* THE ROUTER GOES AWAY. Every renewal from here on fails. */
        gw.mode = MOCK_GW_SILENT;

        /* Pump the main-thread tick — the only place the renewal and the
         * lease clock live — until the advertised port reverts. */
        const uint64_t t0 = SDL_GetTicks();
        uint16_t adv = adv0;
        bool reverted = false;
        while (SDL_GetTicks() - t0 < 25000u) {
            DirectP2P_Tick();
            SDL_Delay(10);
            uint32_t ib = 0, nn = 0;
            uint16_t p = 0;
            if (RoomCode_Decode(DirectP2P_GetHostCode(), &ib, &p, &nn) == ROOM_CODE_OK) {
                adv = p;
                if (p == 40000) { reverted = true; break; }
            }
        }
        const uint64_t dt = SDL_GetTicks() - t0;
        fprintf(stderr,
                "[test_bilateral_punch] 23d: advertised port %u -> %u after %u ms\n",
                (unsigned)adv0, (unsigned)adv, (unsigned)dt);
        if (!reverted) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 23d-still-advertised: %u ms after the "
                    "gateway went silent the room code still advertises port %u, whose "
                    "8 s lease expired long ago. A mapping that cannot be renewed must "
                    "be DROPPED and the STUN-observed endpoint (port 40000) advertised "
                    "instead — otherwise the code the user already shared is dead and "
                    "nothing says so.\n",
                    (unsigned)dt, (unsigned)adv);
            fail_count++;
        }
    }

    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    mock_gateway_stop(&gw, tid, mock_gateway_port(&gw));
    Natpmp_TestHook_SetGateway(NULL, 0);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL, false);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 23d OK — a mapping whose lease expired "
                "without a successful renewal is dropped and the room code falls back "
                "to the STUN endpoint\n");
        return 0;
    }
    return 1;
}

/* --- Test 19: the DELIVER leg starts while the seed leg is still live -- */

/*
 * THE test for S6's core claim. The mock server DELIVERs a synthetic peer
 * endpoint on the first REGISTER. The punch oracle confirms ONLY that
 * DELIVER endpoint and never the room-code endpoint.
 *
 * Pre-S6 the DELIVER endpoint could not be punched until the direct punch
 * had burned its whole 2500 ms window AND the signalling loop had broken
 * out — so the earliest possible handoff was > 2500 ms. Here the handoff
 * must land well inside that, which is only possible if the two legs
 * overlapped.
 *
 * The assertion is on do_handoff's OWN arguments, not on internal state:
 * a race that populated s_work correctly but never reached the handoff
 * would pass an s_work assertion and cannot pass this one.
 */
#define S6_OVERLAP_BOUND_MS 2000u

static char s_r19_deliver_ip[64] = { 0 };
static uint16_t s_r19_deliver_port = 0;
static int s_r19_seed_arms = 0;
static int s_r19_deliver_arms = 0;

static DirectP2PPunchOracleResult r19_oracle(const char* ip, uint16_t port) {
    s_mock_punch_calls++;
    if (s_r19_deliver_ip[0] != '\0' && port == s_r19_deliver_port &&
        strcmp(ip, s_r19_deliver_ip) == 0) {
        s_r19_deliver_arms++;
        return DP2P_PUNCH_CONFIRM;
    }
    s_r19_seed_arms++;
    return DP2P_PUNCH_NEVER;
}

static int s_r19_handoffs_before = 0;
static bool pred_r19_handoff(void) {
    int n = 0;
    DirectP2P_TestHook_LastHandoff(NULL, 0, NULL, NULL, &n);
    return n > s_r19_handoffs_before;
}

static int test_race_deliver_overlaps_seed(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 19: the DELIVER candidate is punched while "
            "the room-code candidate is still live\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();

    unsigned short server_port = 0;
    const int server_sock = open_udp_on_localhost(&server_port);
    if (server_sock < 0) {
        FAIL("test19", "could not bind the mock server socket");
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.use_synth_peer = true;
    ctx.synth_peer_ip_be = htonl(0xCB0071F5u); /* 203.0.113.245 (TEST-NET-3) */
    ctx.synth_peer_port = 7100;
    ctx.min_cookied_before_peer = 0; /* pair on the very first REGISTER */
    ctx.life_secs = 30;

    SDL_strlcpy(s_r19_deliver_ip, "203.0.113.245", sizeof(s_r19_deliver_ip));
    s_r19_deliver_port = 7100;
    s_r19_seed_arms = 0;
    s_r19_deliver_arms = 0;
    s_mock_punch_calls = 0;

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_r19", &ctx);
    if (!tid) {
        FAIL("test19", "SDL_CreateThread failed");
        close_sock(server_sock);
        return 1;
    }

    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    }
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetPunchOracle(r19_oracle);
    DirectP2P_TestHook_ResetHandoff();
    s_r19_handoffs_before = 0;

    uint32_t nonce = 0;
    char code[ROOM_CODE_BUF_LEN];
    struct in_addr peer_in;
    if (!RoomCode_GenerateNonce(&nonce) ||
        inet_pton(AF_INET, "198.51.100.7", &peer_in) != 1 ||
        !RoomCode_Encode((uint32_t)peer_in.s_addr, 6000, nonce, code)) {
        FAIL("test19", "could not build a room code");
        rc = 1;
        goto done;
    }

    {
        const uint32_t t0 = SDL_GetTicks();
        DirectP2P_BeginJoin(code);
        if (!tick_until(pred_r19_handoff, 20000)) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test19: no handoff (state=%d seed_arms=%d "
                    "deliver_arms=%d)\n",
                    (int)DirectP2P_GetState(), s_r19_seed_arms, s_r19_deliver_arms);
            fail_count++;
            rc = 1;
            goto done;
        }
        const uint32_t elapsed = SDL_GetTicks() - t0;

        char hip[64] = { 0 };
        uint16_t hport = 0;
        int hplayer = 0, hcount = 0;
        DirectP2P_TestHook_LastHandoff(hip, (int)sizeof(hip), &hport, &hplayer,
                                       &hcount);

        /* THE assertion: do_handoff was handed the DELIVER endpoint... */
        if (hport != s_r19_deliver_port || strcmp(hip, s_r19_deliver_ip) != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test19: do_handoff got %s:%u, expected the "
                    "DELIVER endpoint %s:%u\n",
                    hip, (unsigned)hport, s_r19_deliver_ip, (unsigned)s_r19_deliver_port);
            fail_count++;
            rc = 1;
        }
        /* ...and it got there before the pre-S6 code could even have
         * STARTED punching it. This is the number a serial cascade cannot
         * produce: its direct-punch window alone was 2500 ms. */
        if (elapsed >= S6_OVERLAP_BOUND_MS) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test19: handoff took %u ms; the DELIVER "
                    "candidate must be punched CONCURRENTLY with the room-code candidate "
                    "(expected < %u ms — pre-S6 the direct-punch window alone was 2500 ms)\n",
                    (unsigned)elapsed, (unsigned)S6_OVERLAP_BOUND_MS);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("19-seed-was-armed", s_r19_seed_arms >= 1);
        EXPECT_TRUE("19-deliver-was-armed", s_r19_deliver_arms >= 1);
        EXPECT_TRUE("19-two-candidates", s_mock_punch_calls == 2);
        /* THE PLAYER NUMBER. do_handoff's first argument is the only
         * thing that decides which side GekkoNet is told it is: 1 ->
         * local player 0 / remote 1, 2 -> local 1 / remote 0
         * (direct_p2p.c:3516 do_handoff). It is passed as a LITERAL at
         * every call site, so nothing downstream can correct a wrong
         * one — two peers that both hand off as the same number give
         * GekkoNet identical local and remote roles and no match can
         * ever start. It is also invisible to every other assertion in
         * this suite: endpoint, timing, state and status text are all
         * exactly right when the number is wrong.
         *
         * This is the JOIN side, and join is player 2
         * (direct_p2p.c:4529). The host half is pinned at runtime in
         * test 13 (13-handoff-player1), and all three call sites are
         * pinned at the source in test 33. */
        EXPECT_TRUE("19-handoff-player2", hplayer == 2);
        /* Exactly one handoff: a second one would re-enter GekkoNet
         * setup on a live session. `hcount` is what separates "no
         * handoff at all" from "a handoff to 0.0.0.0:0", so asserting
         * it also makes the endpoint and player assertions above
         * non-vacuous. */
        EXPECT_TRUE("19-one-handoff", hcount == 1);
        if (rc == 0 && fail_count == fails_before) {
            fprintf(stderr,
                    "[test_bilateral_punch] test 19 OK — DELIVER candidate punched and "
                    "handed off in %u ms with the room-code candidate still live\n",
                    (unsigned)elapsed);
        }
    }

done:
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetPunchOracle(NULL);
    mock_server_stop(&ctx, tid, server_sock, server_port);
    s_r19_deliver_ip[0] = '\0';
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Tests 23, 27, 28: the S6 adversarial-review lane ----------------- */
/* ====================================================================== */

/*
 * WHY THESE EXIST AT ALL (review finding H-2).
 *
 * Across a full pristine run of this suite the line "S6 race: punching
 * candidate" appeared 27 times and "Hole punch SUCCESS" appeared ZERO
 * times. Every candidate in tests 18-19 is oracle-driven, and
 * race_punch_settled() in direct_p2p.c short-circuits to `true` for any
 * candidate whose oracle is not DP2P_PUNCH_REAL — so the Stun_PunchSettled
 * branch that the SHIPPING path always takes was executed by no test at
 * all. That is the structural reason H-1 (a punch confirming in the last
 * 600 ms of the race budget was silently discarded), H-4 and M-2 could
 * survive a green suite.
 *
 * Nothing below uses the punch oracle. Every leg here is DP2P_PUNCH_REAL
 * and punches a REAL loopback UDP socket through the real
 * Stun_PunchBegin / Stun_PunchPump / Stun_PunchOffer / Stun_PunchSettled
 * state machine, so the S4a token check, the source-IP gate and the
 * 600 ms confirmation tail are genuinely executed.
 *
 * They also use a door the rest of the suite does not have:
 * DirectP2P_TestHook_RunRace. Every other seam drives the race through
 * BeginJoin or the host worker, which own process-wide state, so only ONE
 * can be live at a time — and the split-brain defect (H-3) is by
 * construction a TWO-peer property. p2p_race takes everything by
 * argument, so two of them run concurrently on two threads here.
 */

/* --- shared plumbing --------------------------------------------------- */

/* An SDL_net datagram socket on a port we know. SDL_net has no
 * "what port did I get?" accessor, so a POSIX socket is bound to port 0
 * to have the OS name a free one, closed, and that number handed to
 * SDL_net. The retry loop covers the (tiny) window in between. */
static NET_DatagramSocket* sb6_net_socket(uint16_t* out_port) {
    for (int attempt = 0; attempt < 16; attempt++) {
        unsigned short p = 0;
        const int probe = open_udp_on_localhost(&p);
        if (probe < 0) continue;
        close_sock(probe);
        NET_DatagramSocket* s = NET_CreateDatagramSocket(NULL, (Uint16)p);
        if (s != NULL) {
            *out_port = (uint16_t)p;
            return s;
        }
    }
    return NULL;
}

static const uint8_t k_sb6_token[STUN_PUNCH_TOKEN_LEN] = {
    0x5A, 0xA5, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB
};
static const uint8_t k_sb6_key[REND_KEY_LEN] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
};

/* --- log observation, with timestamps ---------------------------------- */

static SDL_LogOutputFunction s_sb6_prev_fn = NULL;
static void* s_sb6_prev_ud = NULL;
static int s_sb6_punch_success = 0;
static uint32_t s_sb6_punch_success_ms = 0;
static int s_sb6_arm_lines = 0;
static int s_sb6_tail_holds = 0;

static void SDLCALL sb6_log_fn(void* ud, int cat, SDL_LogPriority pri,
                               const char* msg) {
    (void)ud;
    if (msg != NULL) {
        if (strstr(msg, "Hole punch SUCCESS") != NULL) {
            if (s_sb6_punch_success++ == 0) s_sb6_punch_success_ms = SDL_GetTicks();
        }
        if (strstr(msg, "S6 race: punching candidate") != NULL) s_sb6_arm_lines++;
        if (strstr(msg, "holding the race open") != NULL) s_sb6_tail_holds++;
    }
    if (s_sb6_prev_fn != NULL) s_sb6_prev_fn(s_sb6_prev_ud, cat, pri, msg);
}

static void sb6_log_begin(void) {
    s_sb6_punch_success = 0;
    s_sb6_punch_success_ms = 0;
    s_sb6_arm_lines = 0;
    s_sb6_tail_holds = 0;
    SDL_GetLogOutputFunction(&s_sb6_prev_fn, &s_sb6_prev_ud);
    SDL_SetLogOutputFunction(sb6_log_fn, NULL);
}

static void sb6_log_end(void) {
    SDL_SetLogOutputFunction(s_sb6_prev_fn, s_sb6_prev_ud);
}

/* --- the punch echo peer ----------------------------------------------- */

/*
 * A loopback UDP socket that answers an authenticated punch with THE SAME
 * 17 bytes it received. Echoing verbatim is what makes it pass
 * Stun_PunchOffer by construction: the payload is "3SX_PUNCH" plus the
 * peer's own 8-byte token, so Stun_HasPunchPrefix and the constant-time
 * Stun_IsPunchPayload token compare both succeed, and the source IP is
 * 127.0.0.1 — the very address the leg was armed on. The S4a fail-closed
 * checks are therefore exercised, not bypassed.
 *
 * `delay_ms` is measured from the FIRST punch this peer sees rather than
 * from thread start, so a test controls the confirmation instant relative
 * to the race's own t0 without having to guess how long the plumbing in
 * front of the race takes.
 */
typedef struct {
    int  sock;
    volatile bool stop;
    int  delay_ms;
    volatile int punches_seen;
    volatile int echoes_sent;
    volatile int nonpunch_seen;
    uint32_t first_punch_ms;
    volatile uint32_t first_echo_ms;
} PunchEchoCtx;

static int SDLCALL punch_echo_thread(void* arg) {
    PunchEchoCtx* c = (PunchEchoCtx*)arg;
    for (;;) {
        if (c->stop) return 0;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(c->sock, &rfds);
        struct timeval tv = { 0, 5 * 1000 };
        if (select(c->sock + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        uint8_t b[128];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        const int n = (int)recvfrom(c->sock, (char*)b, sizeof(b), 0,
                                    (struct sockaddr*)&src, &sl);
        if (n <= 0) continue;
        if (n != STUN_PUNCH_PAYLOAD_LEN || !Stun_HasPunchPrefix(b, n)) {
            c->nonpunch_seen++;
            continue;
        }
        c->punches_seen++;
        const uint32_t now = SDL_GetTicks();
        if (c->first_punch_ms == 0) c->first_punch_ms = now;
        if ((int)(now - c->first_punch_ms) < c->delay_ms) continue;
        sendto(c->sock, (const char*)b, n, 0, (struct sockaddr*)&src, sl);
        if (c->first_echo_ms == 0) c->first_echo_ms = now;
        c->echoes_sent++;
    }
}

/* --- a minimal rendezvous mock for the adversarial lane ---------------- */

/*
 * Deliberately NOT MockServerCtx: these cases need per-frame receive
 * counts and a DELIVER burst of their own, and a bespoke thread keeps
 * that out of a struct the earlier stages share.
 *
 * Answers REGISTER with a DELIVER for a fixed endpoint, repeated
 * `deliver_burst` times so the duplicate-endpoint guard has duplicates to
 * guard against.
 */
typedef struct {
    int  sock;          /* signal port  */
    volatile bool stop;
    int  life_secs;

    /* DELIVER behaviour (0 = answer no REGISTERs at all). */
    int      deliver_burst;
    int      deliver_after_ms;
    uint32_t deliver_ip_be;
    uint16_t deliver_port;

    uint32_t started_ms;
    volatile int registers;
    volatile int delivers;
} Sb6ServerCtx;

static int SDLCALL sb6_server_thread(void* arg) {
    Sb6ServerCtx* c = (Sb6ServerCtx*)arg;
    c->started_ms = SDL_GetTicks();
    const long long start = (long long)time(NULL);
    const long long life = (c->life_secs > 0) ? (long long)c->life_secs : 30;

    for (;;) {
        if (c->stop) return 0;
        if ((long long)time(NULL) - start > life) return 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(c->sock, &rfds);
        struct timeval tv = { 0, 20 * 1000 };
        if (select(c->sock + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        if (!FD_ISSET(c->sock, &rfds)) continue;

        uint8_t buf[128];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        const int n = (int)recvfrom(c->sock, (char*)buf, sizeof(buf), 0,
                                    (struct sockaddr*)&src, &sl);
        if (n < REND_REGISTER_LEN) continue;
        if (buf[0] != REND_MAGIC_BYTES_0 || buf[1] != REND_MAGIC_BYTES_1 ||
            buf[2] != REND_MAGIC_BYTES_2 || buf[3] != REND_MAGIC_BYTES_3 ||
            buf[4] != REND_VERSION) {
            continue;
        }
        const uint8_t type = buf[5];
        const uint8_t* key = &buf[8];

        if (type != REND_TYPE_REGISTER && type != REND_TYPE_POLL) continue;
        c->registers++;
        if (c->deliver_burst <= 0) continue;

        const bool due = (int)(SDL_GetTicks() - c->started_ms) >= c->deliver_after_ms;
        struct sockaddr_in peer;
        memset(&peer, 0, sizeof(peer));
        peer.sin_family = AF_INET;
        peer.sin_addr.s_addr = c->deliver_ip_be;
        for (int i = 0; i < c->deliver_burst; i++) {
            uint8_t reply[REND_DELIVER_LEN];
            const int rl = build_deliver(reply, key, due ? &peer : NULL,
                                         due ? c->deliver_port : 0);
            sendto(c->sock, (const char*)reply, rl, 0, (struct sockaddr*)&src, sl);
            c->delivers++;
        }
    }
}

/* --- Test 23: a REAL punch, and H-1's last-600 ms rescue --------------- */

/*
 * H-1 (alpha blocker). direct_p2p.c's overall-budget break had no
 * exemption for a leg that had CONFIRMED and was still inside its
 * STUN_PUNCH_CONFIRM_MS tail. RACE_PUNCHED is only ever set behind
 * race_punch_settled(), so a punch confirming later than
 * `race_budget_ms - STUN_PUNCH_CONFIRM_MS` was thrown away and the
 * classifier told the user NAT had blocked them. With the shipped
 * defaults that is t+7400..8000 ms of an 8000 ms budget — 7.5% of every
 * race, on both of the joiner's two attempts.
 *
 * The rig: one race, one real punch leg pointed at the echo peer, race
 * budget 4000 ms and the echo peer withholding its first echo until
 * 3600 ms after the first punch it sees. Stun_PunchPump's cadence is
 * 50 ms for the first 500 ms and 200 ms after that (so the sends land on
 * 505 + 200k ms), which puts the confirming datagram at ~3705 ms —
 * squarely inside the last 600 ms — and the tail at ~4305 ms, past the
 * budget edge with ~300 ms of margin on both sides.
 *
 * The race duration is asserted to be GREATER than the budget on purpose:
 * that is what proves the confirmation actually landed in the exempted
 * window rather than comfortably before it, so the test cannot pass for
 * the wrong reason.
 */
#define SB6_H1_BUDGET_MS 4000
#define SB6_H1_ECHO_DELAY_MS 3600

static int test_race_confirm_at_budget_edge(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 23: a REAL punch confirming in the last "
            "%d ms of the race budget still connects (H-1)\n",
            STUN_PUNCH_CONFIRM_MS);
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetPunchOracle(NULL); /* REAL legs only */

    unsigned short echo_port = 0, server_port = 0;
    const int echo_sock = open_udp_on_localhost(&echo_port);
    const int server_sock = open_udp_on_localhost(&server_port);
    uint16_t my_port = 0;
    NET_DatagramSocket* sock = sb6_net_socket(&my_port);
    if (echo_sock < 0 || server_sock < 0 || sock == NULL) {
        FAIL("test23", "could not bind the rig sockets");
        if (echo_sock >= 0) close_sock(echo_sock);
        if (server_sock >= 0) close_sock(server_sock);
        if (sock != NULL) NET_DestroyDatagramSocket(sock);
        return 1;
    }

    PunchEchoCtx echo;
    memset(&echo, 0, sizeof(echo));
    echo.sock = echo_sock;
    echo.delay_ms = SB6_H1_ECHO_DELAY_MS;
    SDL_Thread* echo_tid = SDL_CreateThread(punch_echo_thread, "sb6_echo", &echo);

    Sb6ServerCtx srv;
    memset(&srv, 0, sizeof(srv));
    srv.sock = server_sock;
    srv.life_secs = 30;
    srv.deliver_burst = 1;
    srv.deliver_ip_be = htonl(0x7F000001u); /* 127.0.0.1 — the echo peer */
    srv.deliver_port = echo_port;
    SDL_Thread* srv_tid = SDL_CreateThread(sb6_server_thread, "sb6_srv23", &srv);

    if (echo_tid == NULL || srv_tid == NULL) {
        FAIL("test23", "SDL_CreateThread failed");
        rc = 1;
        goto done;
    }

    sb6_log_begin();
    {
        DirectP2PRaceProbeCfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.host_role = false;
        cfg.sock = sock;
        cfg.punch_token = k_sb6_token;
        cfg.seed_ip = "127.0.0.1";
        cfg.seed_port = 0; /* no seed leg: the DELIVER candidate is the subject */
        cfg.signal_ip = "127.0.0.1";
        cfg.signal_port = server_port;
        cfg.session_key = k_sb6_key;
        cfg.my_public_port = my_port;
        cfg.signal_leg = true;
        cfg.signal_budget_ms = SB6_H1_BUDGET_MS;
        cfg.punch_leg_ms = 5000;
        cfg.race_budget_ms = SB6_H1_BUDGET_MS;

        DirectP2PRaceProbeOut out;
        DirectP2P_TestHook_RunRace(&cfg, &out);
        sb6_log_end();

        /* H-2: a punch was really on the wire and really confirmed. */
        if (s_sb6_punch_success < 1) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test23: no \"Hole punch SUCCESS\" — the "
                    "leg never confirmed on the wire (echo peer saw %d punches, sent %d "
                    "echoes)\n",
                    echo.punches_seen, echo.echoes_sent);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("23-echo-peer-was-punched", echo.punches_seen > 1);

        if (out.outcome != DP2P_RACE_PROBE_PUNCHED) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test23: outcome=%d after %u ms, expected "
                    "PUNCHED(%d) — a punch that confirms inside the last %d ms of the "
                    "%d ms budget must NOT be discarded (H-1)\n",
                    (int)out.outcome, (unsigned)out.t_race_ms,
                    (int)DP2P_RACE_PROBE_PUNCHED, STUN_PUNCH_CONFIRM_MS,
                    SB6_H1_BUDGET_MS);
            fail_count++;
            rc = 1;
        }
        if (out.peer_port != echo_port || strcmp(out.peer_ip, "127.0.0.1") != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test23: race produced %s:%u, expected the "
                    "echo peer 127.0.0.1:%u\n",
                    out.peer_ip, (unsigned)out.peer_port, (unsigned)echo_port);
            fail_count++;
            rc = 1;
        }
        /* The confirmation MUST have landed in the exempted window, or
         * this test would be green for a reason that has nothing to do
         * with H-1. */
        if (out.t_race_ms <= (uint32_t)SB6_H1_BUDGET_MS ||
            out.t_race_ms > (uint32_t)(SB6_H1_BUDGET_MS + STUN_PUNCH_CONFIRM_MS + 400)) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test23: race ran %u ms; the confirmation "
                    "must land inside the last %d ms of the %d ms budget (so the race must "
                    "outlast the budget) and the loop must stay hard-bounded at %d ms\n",
                    (unsigned)out.t_race_ms, STUN_PUNCH_CONFIRM_MS, SB6_H1_BUDGET_MS,
                    SB6_H1_BUDGET_MS + STUN_PUNCH_CONFIRM_MS);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("23-tail-hold-was-logged", s_sb6_tail_holds >= 1);

        if (rc == 0 && fail_count == fails_before) {
            fprintf(stderr,
                    "[test_bilateral_punch] test 23 OK — real punch confirmed on the wire "
                    "(%d echoes), race ran %u ms against a %d ms budget, handed back "
                    "%s:%u\n",
                    echo.echoes_sent, (unsigned)out.t_race_ms, SB6_H1_BUDGET_MS,
                    out.peer_ip, (unsigned)out.peer_port);
        }
    }

done:
    echo.stop = true;
    srv.stop = true;
    if (echo_tid != NULL) SDL_WaitThread(echo_tid, NULL);
    if (srv_tid != NULL) SDL_WaitThread(srv_tid, NULL);
    close_sock(echo_sock);
    close_sock(server_sock);
    NET_DestroyDatagramSocket(sock);
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Test 27: the duplicate-endpoint guard ----------------------------- */

/*
 * M-2. race_arm_punch's duplicate-endpoint guard was itself uncovered —
 * deleting it left the suite green. It is not cosmetic: without it every
 * repeated DELIVER re-arms slot 1, which doubles the punch traffic and
 * (pre-fix) leaked the previous leg's NET_Address ref, because the slot
 * was memset BEFORE Stun_PunchBegin could fail.
 *
 * The mock answers each REGISTER with FIVE identical peer-bearing
 * DELIVERs, so the guard has real duplicates to reject. Exactly one
 * candidate may ever be armed.
 */
#define SB6_DUP_BURST 5

static int test_race_duplicate_candidate_guard(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 27: repeated DELIVERs of the same endpoint arm "
            "exactly ONE punch candidate (M-2)\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetPunchOracle(NULL);

    unsigned short sink_port = 0, server_port = 0;
    const int sink_sock = open_udp_on_localhost(&sink_port);   /* bound, silent */
    const int server_sock = open_udp_on_localhost(&server_port);
    uint16_t my_port = 0;
    NET_DatagramSocket* sock = sb6_net_socket(&my_port);
    SDL_Thread* srv_tid = NULL;
    Sb6ServerCtx srv;
    memset(&srv, 0, sizeof(srv));
    if (sink_sock < 0 || server_sock < 0 || sock == NULL) {
        FAIL("test27", "could not bind the rig sockets");
        rc = 1;
        goto done;
    }

    memset(&srv, 0, sizeof(srv));
    srv.sock = server_sock;
    srv.life_secs = 30;
    srv.deliver_burst = SB6_DUP_BURST;
    srv.deliver_ip_be = htonl(0x7F000001u);
    srv.deliver_port = sink_port;
    srv_tid = SDL_CreateThread(sb6_server_thread, "sb6_srv27", &srv);
    if (srv_tid == NULL) {
        FAIL("test27", "SDL_CreateThread failed");
        rc = 1;
        goto done;
    }

    sb6_log_begin();
    {
        DirectP2PRaceProbeCfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.host_role = false;
        cfg.sock = sock;
        cfg.punch_token = k_sb6_token;
        cfg.seed_ip = "127.0.0.1";
        cfg.seed_port = 0; /* no seed leg, so the count below is unambiguous */
        cfg.signal_ip = "127.0.0.1";
        cfg.signal_port = (uint16_t)server_port;
        cfg.session_key = k_sb6_key;
        cfg.my_public_port = my_port;
        cfg.signal_leg = true;
        cfg.signal_budget_ms = 2000;
        cfg.punch_leg_ms = 5000;
        cfg.race_budget_ms = 2000;

        DirectP2PRaceProbeOut out;
        DirectP2P_TestHook_RunRace(&cfg, &out);
        sb6_log_end();

        if (srv.delivers < SB6_DUP_BURST) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test27: the mock only sent %d DELIVER(s); "
                    "the guard had nothing to guard against\n", srv.delivers);
            fail_count++;
            rc = 1;
        }
        if (s_sb6_arm_lines != 1) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test27: %d candidates were armed against "
                    "%d identical DELIVERs, expected exactly 1 — race_arm_punch's "
                    "duplicate-endpoint guard must reject an endpoint already being "
                    "punched\n",
                    s_sb6_arm_lines, srv.delivers);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("27-no-punch-from-a-silent-sink",
                    out.outcome == DP2P_RACE_PROBE_EXHAUSTED);
        if (rc == 0 && fail_count == fails_before) {
            fprintf(stderr,
                    "[test_bilateral_punch] test 27 OK — %d identical DELIVERs armed "
                    "exactly 1 candidate\n", srv.delivers);
        }
    }

done:
    srv.stop = true;
    if (srv_tid != NULL) SDL_WaitThread(srv_tid, NULL);
    if (sink_sock >= 0) close_sock(sink_sock);
    if (server_sock >= 0) close_sock(server_sock);
    if (sock != NULL) NET_DestroyDatagramSocket(sock);
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Test 28: wrap safety, and the 600 ms tail pinned by literal ------- */

/*
 * M-1. The recorded reasoning for the wrap-safety fix was BACKWARDS: it
 * claimed the pre-fix form let the race "run until every leg finished
 * with no overall bound". The pre-fix condition was an OR of a wrap-safe
 * term and a wrap-unsafe one, so the wrap-safe term always bounded the
 * loop. The real symptom is the opposite — `t0 + budget` computed in
 * uint32_t overflows to a SMALL number whenever t0 is within `budget` of
 * the wrap, so `now >= t0 + budget` is true on the FIRST iteration and
 * every join attempted in the ~8 s before a 49.7-day wrap fails INSTANTLY.
 * The wrong reasoning is what justified shipping it untested.
 *
 * SDL_GetTicks cannot be moved to the wrap (it is monotonic from SDL init
 * and this build has no clock injection seam), so the deadline predicate
 * is a pure function and this drives it directly at synthetic wrap
 * values. That is the whole defect: the arithmetic, not the plumbing.
 *
 * M-3 rides along: STUN_PUNCH_CONFIRM_MS was unpinned — setting it to 0
 * left run_punch_leg_offer_test green because its assertions were written
 * in terms of the constant. It is safety-relevant now that H-1's
 * exemption is sized by it, so it is pinned to its literal here.
 */
/* test_race_budget_wrap_safety MOVED to src/netplay/test_netplay_units.c
 * (#132 P3) — pure predicate over injected timestamps. */


/* --- Tests 30-31: what M-2's fix ACTUALLY is (review finding H-C) ------ */
/* ====================================================================== */

/*
 * H-C. Test 27 above is a real test of a real invariant, but it is
 * credited to the wrong code. The duplicate-endpoint guard it neutralises
 * (direct_p2p.c:1574-1579) is BYTE-IDENTICAL to the pre-fix tree:
 *
 *   $ git show 26deb2fc:src/netplay/direct_p2p.c | sed -n '1210,1215p' | md5
 *   d1f6c2bdeb1ee0d52b98b3fffc9fc17c
 *   $ sed -n '1391,1396p' src/netplay/direct_p2p.c | md5
 *   d1f6c2bdeb1ee0d52b98b3fffc9fc17c
 *
 * Task #105 repair: the second span had drifted and the recipe no longer
 * reproduced the digest — at ed37cb42 the block was already at 1385-1390
 * while this comment still said 1313-1318, so the check had been silently
 * false for 72 lines' worth of edits before this task moved it a further
 * six. The DIGEST was never wrong and the claim it supports still holds;
 * only the span was stale. Re-derived by scanning the file for the
 * six-line window that hashes d1f6c2bd..., not by trusting the old
 * number. If you edit direct_p2p.c above this point, re-run BOTH md5
 * lines rather than assuming this one still resolves — a byte-identity
 * proof that silently stops pointing at the bytes is worse than no proof.
 *
 * The guard was never the defect. M-2's fix is the code just below it —
 * direct_p2p.c:1653-1666 — and it is TWO changes, not one:
 *
 *   (i)  VALIDATE, THEN memset. The new StunPunchLeg is built on the
 *        STACK and Stun_PunchBegin is allowed to fail BEFORE the
 *        candidate slot is touched. The pre-fix order wiped the slot
 *        first, so a re-arm that failed to resolve destroyed a live,
 *        punching candidate and the race went quiet for the rest of its
 *        budget.
 *   (ii) race_finish_punch(c, now) before the memset. That is what
 *        releases the NET_Address ref the OUTGOING leg still holds in
 *        c->leg.target (Stun_PunchEnd -> NET_UnrefAddress,
 *        stun.c:922-926). memset over a live leg.target loses the only
 *        pointer to a ref'd address: a permanent leak, one per re-arm.
 *
 * Test 27 counts occurrences of the log line "S6 race: punching
 * candidate" into s_sb6_arm_lines (test_bilateral_punch.c:6258). Neither
 * (i) nor (ii) changes that count, which is why restoring the pre-fix
 * ordering AND deleting the race_finish_punch call leaves test 27 — and
 * the whole suite — GREEN. Test 30 covers (i), test 31 covers (ii).
 */

/* --- a punch sink: counts punches, never echoes, timestamps both ends -- */

/*
 * Deliberately NOT PunchEchoCtx. That peer exists to CONFIRM a leg; these
 * cases need a candidate that is punched and never confirms, so the race
 * runs its full budget and the punch cadence stays observable to the end.
 * `last_ms` is the whole point: "for how long was this endpoint still
 * being punched" is exactly the question (i) asks.
 */
typedef struct {
    int  sock;
    volatile bool stop;
    volatile int  punches;
    volatile uint32_t first_ms;
    volatile uint32_t last_ms;
} HcSinkCtx;

static int SDLCALL hc_sink_thread(void* arg) {
    HcSinkCtx* c = (HcSinkCtx*)arg;
    for (;;) {
        if (c->stop) return 0;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(c->sock, &rfds);
        struct timeval tv = { 0, 5 * 1000 };
        if (select(c->sock + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        uint8_t b[128];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        const int n = (int)recvfrom(c->sock, (char*)b, sizeof(b), 0,
                                    (struct sockaddr*)&src, &sl);
        if (n <= 0) continue;
        if (n != STUN_PUNCH_PAYLOAD_LEN || !Stun_HasPunchPrefix(b, n)) continue;
        const uint32_t now = SDL_GetTicks();
        if (c->punches++ == 0) c->first_ms = now;
        c->last_ms = now;
    }
}

/* --- a rendezvous mock that DELIVERs a SEQUENCE of distinct endpoints -- */

/*
 * Sb6ServerCtx delivers ONE fixed endpoint N times, which is what test 27
 * needs (duplicates for the guard to reject) and exactly what these cases
 * cannot use: the duplicate-endpoint guard rejects a repeat before the
 * validate/memset decision is ever reached. Slot 1 is only ever RE-armed
 * by a DELIVER carrying a DIFFERENT endpoint, so this mock walks a list.
 *
 * The first endpoint answers the first REGISTER; the rest are pushed
 * UNSOLICITED to the address that REGISTER came from, one every
 * `gap_ms`. Unsolicited is not a cheat: direct_p2p.c sets
 * signal_active = false the moment the first DELIVER_PEER lands
 * (direct_p2p.c:2071), so no further REGISTER is ever sent — but the
 * REND_FRAME_DELIVER branch of the receive path (direct_p2p.c:2012) is
 * NOT gated on signal_active, so every later DELIVER is still parsed and
 * still re-arms slot 1. That asymmetry is the production behaviour under
 * test.
 */
#define HC_MAX_EP 40

typedef struct {
    int  sock;
    volatile bool stop;
    int  life_secs;
    uint16_t ports[HC_MAX_EP];
    int  n_ports;
    int  gap_ms;
    uint8_t  key[REND_KEY_LEN];
    volatile int registers;
    volatile int delivers;
} HcServerCtx;

static int SDLCALL hc_server_thread(void* arg) {
    HcServerCtx* c = (HcServerCtx*)arg;
    const long long start = (long long)time(NULL);
    const long long life = (c->life_secs > 0) ? (long long)c->life_secs : 30;

    struct sockaddr_in cli;
    socklen_t cli_len = 0;
    bool have_cli = false;
    int sent = 0;
    uint32_t next_push_ms = 0;

    for (;;) {
        if (c->stop) return 0;
        if ((long long)time(NULL) - start > life) return 0;

        /* Push the next endpoint in the sequence when it comes due. */
        if (have_cli && sent < c->n_ports && SDL_GetTicks() >= next_push_ms) {
            struct sockaddr_in peer;
            memset(&peer, 0, sizeof(peer));
            peer.sin_family = AF_INET;
            peer.sin_addr.s_addr = htonl(0x7F000001u);
            uint8_t reply[REND_DELIVER_LEN];
            const int rl = build_deliver(reply, c->key, &peer, c->ports[sent]);
            sendto(c->sock, (const char*)reply, rl, 0,
                   (struct sockaddr*)&cli, cli_len);
            c->delivers++;
            sent++;
            next_push_ms = SDL_GetTicks() + (uint32_t)c->gap_ms;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(c->sock, &rfds);
        struct timeval tv = { 0, 5 * 1000 };
        if (select(c->sock + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        uint8_t buf[128];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        const int n = (int)recvfrom(c->sock, (char*)buf, sizeof(buf), 0,
                                    (struct sockaddr*)&src, &sl);
        if (n < REND_REGISTER_LEN) continue;
        if (buf[0] != REND_MAGIC_BYTES_0 || buf[1] != REND_MAGIC_BYTES_1 ||
            buf[2] != REND_MAGIC_BYTES_2 || buf[3] != REND_MAGIC_BYTES_3 ||
            buf[4] != REND_VERSION) {
            continue;
        }
        if (buf[5] != REND_TYPE_REGISTER && buf[5] != REND_TYPE_POLL) continue;
        c->registers++;
        if (!have_cli) {
            memcpy(c->key, &buf[8], REND_KEY_LEN);
            cli = src;
            cli_len = sl;
            have_cli = true;
            next_push_ms = SDL_GetTicks(); /* first endpoint goes out at once */
        }
    }
}

/* --- Test 30: half (i), VALIDATE THEN memset --------------------------- */

/*
 * A re-arm that FAILS must leave the candidate that was already there
 * armed and still punching. Pre-fix it was wiped, and because the pump
 * loop skips any candidate with armed == false (direct_p2p.c:1920) the
 * race then punched NOTHING for the rest of its budget while still
 * reporting a full-length race.
 *
 * The rig delivers three endpoints on one race: A at t+0, B at t+~600
 * (a normal, successful re-arm of slot 1 — A is replaced), then Z at
 * t+~1200, whose arm is made to FAIL. With the fix, B is untouched by the
 * failed arm and keeps being punched until the 4 000 ms budget expires.
 * Pre-fix, B's slot is wiped at ~1200 ms and the punches stop dead.
 *
 * The assertion is on the WIRE: how long B was still receiving punch
 * datagrams, measured by B's own socket. It observes no internal state and
 * no log line, so it cannot be satisfied by a candidate that is "armed"
 * but silent.
 *
 * WHY THE ARM-FAIL SEAM. race_arm_punch's only failure mode past its
 * up-front ip/port guard is Stun_PunchBegin, which fails on an
 * unresolvable host (stun.c:781-798) — and slot 1's IP always comes from
 * inet_ntop in Rendezvous_ParseDeliverEx (rendezvous.c:300-302), so the
 * wire can only ever deliver a resolvable dotted quad. The seam swaps the
 * hostname STRING handed to the real Stun_PunchBegin for a 144-character
 * DNS label; the failure is produced by the real NET_ResolveHostname path.
 * See direct_p2p.c's DirectP2P_TestHook_SetArmFailEndpoint.
 */
#define HC30_BUDGET_MS   4000
#define HC30_GAP_MS       600
/* B is armed at ~600 ms and, WITH the fix, is punched until the race ends
 * at ~4000 ms: a span of ~3400 ms. WITHOUT it, B is wiped by the failed
 * arm at ~1200 ms: a span of ~600 ms, i.e. exactly HC30_GAP_MS. 2000 ms
 * sits between the two with >1300 ms of margin on either side. */
#define HC30_MIN_SPAN_MS 2000

static int test_race_failed_rearm_keeps_live_candidate(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 30: a FAILED re-arm leaves the live candidate "
            "armed and still punching (M-2 half i: validate, then memset)\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetPunchOracle(NULL);
    DirectP2P_TestHook_SetArmFailEndpoint(NULL, 0);

    unsigned short pa = 0, pb = 0, pz = 0, server_port = 0;
    HcSinkCtx sink_a, sink_b;
    memset(&sink_a, 0, sizeof(sink_a));
    memset(&sink_b, 0, sizeof(sink_b));
    sink_a.sock = open_udp_on_localhost(&pa);
    sink_b.sock = open_udp_on_localhost(&pb);
    const int zsock = open_udp_on_localhost(&pz);       /* reserves the number */
    const int server_sock = open_udp_on_localhost(&server_port);
    uint16_t my_port = 0;
    NET_DatagramSocket* sock = sb6_net_socket(&my_port);

    SDL_Thread* a_tid = NULL;
    SDL_Thread* b_tid = NULL;
    SDL_Thread* srv_tid = NULL;
    HcServerCtx srv;
    memset(&srv, 0, sizeof(srv));

    if (sink_a.sock < 0 || sink_b.sock < 0 || zsock < 0 || server_sock < 0 ||
        sock == NULL) {
        FAIL("test30", "could not bind the rig sockets");
        rc = 1;
        goto done;
    }

    srv.sock = server_sock;
    srv.life_secs = 30;
    srv.gap_ms = HC30_GAP_MS;
    srv.n_ports = 3;
    srv.ports[0] = (uint16_t)pa;
    srv.ports[1] = (uint16_t)pb;
    srv.ports[2] = (uint16_t)pz;   /* this one's arm is made to fail */

    DirectP2P_TestHook_SetArmFailEndpoint("127.0.0.1", (uint16_t)pz);

    a_tid = SDL_CreateThread(hc_sink_thread, "hc_sink_a", &sink_a);
    b_tid = SDL_CreateThread(hc_sink_thread, "hc_sink_b", &sink_b);
    srv_tid = SDL_CreateThread(hc_server_thread, "hc_srv29", &srv);
    if (a_tid == NULL || b_tid == NULL || srv_tid == NULL) {
        FAIL("test30", "SDL_CreateThread failed");
        rc = 1;
        goto done;
    }

    sb6_log_begin();
    {
        DirectP2PRaceProbeCfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.host_role = false;
        cfg.sock = sock;
        cfg.punch_token = k_sb6_token;
        cfg.seed_ip = "127.0.0.1";
        cfg.seed_port = 0;          /* no seed leg: slot 1 is the only candidate */
        cfg.signal_ip = "127.0.0.1";
        cfg.signal_port = (uint16_t)server_port;
        cfg.session_key = k_sb6_key;
        cfg.my_public_port = my_port;
        cfg.signal_leg = true;
        cfg.signal_budget_ms = HC30_BUDGET_MS;
        /* Longer than the whole race, so B can only stop being punched
         * because the slot was wiped — never because the leg timed out. */
        cfg.punch_leg_ms = 60000;
        cfg.race_budget_ms = HC30_BUDGET_MS;

        DirectP2PRaceProbeOut out;
        DirectP2P_TestHook_RunRace(&cfg, &out);
        sb6_log_end();

        const int span_b = (sink_b.punches > 0)
                               ? (int)(sink_b.last_ms - sink_b.first_ms) : -1;

        fprintf(stderr,
                "[test_bilateral_punch] test 30: delivers=%d arm-lines=%d "
                "A(punches=%d) B(punches=%d span=%d ms) outcome=%d\n",
                srv.delivers, s_sb6_arm_lines, sink_a.punches, sink_b.punches,
                span_b, (int)out.outcome);

        /* --- rig sanity: all three endpoints really were offered ------ */
        if (srv.delivers < 3) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test30: the mock only sent %d "
                    "DELIVER(s), needed 3 (A, B, then the one that fails to arm)\n",
                    srv.delivers);
            fail_count++;
            rc = 1;
        }
        /* A and B armed; Z did not. If Z HAD armed, the seam is broken and
         * the rest of this test would be measuring nothing. */
        if (s_sb6_arm_lines != 2) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test30: %d candidates armed, expected "
                    "exactly 2 (A and B) — the third endpoint's Stun_PunchBegin was "
                    "supposed to FAIL, so the arm-fail seam is not working and this "
                    "test is measuring nothing\n", s_sb6_arm_lines);
            fail_count++;
            rc = 1;
        }
        if (sink_a.punches <= 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test30: endpoint A was never punched; "
                    "the rig never got as far as the re-arm under test\n");
            fail_count++;
            rc = 1;
        }

        /* --- the actual invariant ------------------------------------ */
        if (span_b < HC30_MIN_SPAN_MS) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test30: endpoint B stopped being "
                    "punched %d ms after it started (%d datagrams), expected at least "
                    "%d ms — a re-arm that FAILS must leave the live candidate armed. "
                    "race_arm_punch memset the slot BEFORE letting Stun_PunchBegin "
                    "fail, so the failed arm destroyed a punching candidate and the "
                    "race went silent for the rest of its %d ms budget\n",
                    span_b, sink_b.punches, HC30_MIN_SPAN_MS, HC30_BUDGET_MS);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("29-silent-sinks-never-confirm",
                    out.outcome == DP2P_RACE_PROBE_EXHAUSTED);
        if (rc == 0 && fail_count == fails_before) {
            fprintf(stderr,
                    "[test_bilateral_punch] test 30 OK — the failed re-arm left B armed; "
                    "B was still being punched %d ms after its first datagram\n", span_b);
        }
    }

done:
    DirectP2P_TestHook_SetArmFailEndpoint(NULL, 0);
    srv.stop = true;
    sink_a.stop = true;
    sink_b.stop = true;
    if (srv_tid != NULL) SDL_WaitThread(srv_tid, NULL);
    if (a_tid != NULL) SDL_WaitThread(a_tid, NULL);
    if (b_tid != NULL) SDL_WaitThread(b_tid, NULL);
    if (sink_a.sock >= 0) close_sock(sink_a.sock);
    if (sink_b.sock >= 0) close_sock(sink_b.sock);
    if (zsock >= 0) close_sock(zsock);
    if (server_sock >= 0) close_sock(server_sock);
    if (sock != NULL) NET_DestroyDatagramSocket(sock);
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Test 31: half (ii), the NET_Address ref release ------------------- */

/*
 * Every successful re-arm of slot 1 hands the slot a fresh NET_Address ref
 * (Stun_PunchBegin -> NET_ResolveHostname, stun.c:781) and must give the
 * outgoing one back (race_finish_punch -> Stun_PunchEnd ->
 * NET_UnrefAddress, stun.c:922-926). memset over a live leg.target loses
 * the only pointer to it: an unbounded leak on the one path a real joiner
 * takes whenever the rendezvous revises the host's endpoint.
 *
 * HOW THIS IS OBSERVED, and why it is the memory and not a proxy.
 * SDL_GetNumAllocations() is documented to return "-1 if allocation counts
 * are disabled" (SDL_stdinc.h:1620-1634) and MEASURED on this build it
 * returns exactly -1, so it cannot see anything. SDL_SetMemoryFunctions
 * can: SDL3_net routes its allocations through SDL_malloc, so counting
 * wrappers see NET_ResolveHostname/NET_UnrefAddress directly. Measured
 * with a standalone probe against this same SDL3 + SDL3_net:
 *
 *   32 x (NET_ResolveHostname + NET_UnrefAddress)  ->  net  +0 live allocs
 *   32 x  NET_ResolveHostname, no unref            ->  net +96 live allocs
 *
 * repeatably, over three rounds each: exactly THREE live allocations per
 * un-released NET_Address, and exactly zero when the ref is given back.
 * So the assertion below is on real heap residency, not on a call count.
 *
 * The race is run TWICE and only the second is measured, so first-touch
 * allocations (resolver thread, socket state, log buffers) are already
 * paid for before the window opens.
 */
#define HC31_REARMS      32
#define HC31_GAP_MS      40
#define HC31_BUDGET_MS   (HC31_REARMS * HC31_GAP_MS + 900)
/* Rig sanity: the leak is (arms - 1) x 3 live allocations, so 24 arms is
 * still a 69-allocation signal. Below that the rig, not the fix, is what
 * the number would be describing. */
#define HC31_MIN_ARMS    24
/* Sized from measurement, not from taste. On the fixed tree this delta was
 * EXACTLY +0 on five consecutive runs — the SDL_net allocations the race
 * makes are all matched by frees, so the noise floor here is literally
 * zero. The signal it has to separate from is 31 re-arms x 3 live
 * allocations = +93. 12 is four leaked addresses' worth of headroom above
 * a measured-zero floor and still less than an eighth of the signal, so it
 * cannot be satisfied by a build that skips the release. */
#define HC31_MAX_DELTA   12

static SDL_AtomicInt g_hc_live_allocs;

static void* SDLCALL hc_malloc(size_t s) {
    SDL_AddAtomicInt(&g_hc_live_allocs, 1);
    return malloc(s);
}
static void* SDLCALL hc_calloc(size_t n, size_t s) {
    SDL_AddAtomicInt(&g_hc_live_allocs, 1);
    return calloc(n, s);
}
static void* SDLCALL hc_realloc(void* p, size_t s) {
    if (p == NULL) SDL_AddAtomicInt(&g_hc_live_allocs, 1);
    return realloc(p, s);
}
static void SDLCALL hc_free(void* p) {
    if (p != NULL) SDL_AddAtomicInt(&g_hc_live_allocs, -1);
    free(p);
}

/* One race over `n` distinct endpoints. Returns the number of candidates
 * armed, or -1 if the rig failed to come up. */
static int hc31_run_one_race(int n, int* out_delivers) {
    unsigned short server_port = 0;
    int sinks[HC_MAX_EP];
    for (int i = 0; i < HC_MAX_EP; i++) sinks[i] = -1;
    const int server_sock = open_udp_on_localhost(&server_port);
    uint16_t my_port = 0;
    NET_DatagramSocket* sock = sb6_net_socket(&my_port);
    SDL_Thread* srv_tid = NULL;
    HcServerCtx srv;
    memset(&srv, 0, sizeof(srv));
    int armed = -1;

    if (server_sock < 0 || sock == NULL) goto done;

    srv.sock = server_sock;
    srv.life_secs = 30;
    srv.gap_ms = HC31_GAP_MS;
    srv.n_ports = n;
    /* Real bound sockets, kept open for the whole race: the port numbers
     * are then guaranteed distinct AND guaranteed not to be re-issued to
     * anything else mid-race, so every DELIVER really is a NEW endpoint
     * and really does re-arm slot 1. They are never read; the punches
     * they collect are irrelevant. */
    for (int i = 0; i < n; i++) {
        unsigned short p = 0;
        sinks[i] = open_udp_on_localhost(&p);
        if (sinks[i] < 0) goto done;
        srv.ports[i] = (uint16_t)p;
    }

    srv_tid = SDL_CreateThread(hc_server_thread, "hc_srv30", &srv);
    if (srv_tid == NULL) goto done;

    sb6_log_begin();
    {
        DirectP2PRaceProbeCfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.host_role = false;
        cfg.sock = sock;
        cfg.punch_token = k_sb6_token;
        cfg.seed_ip = "127.0.0.1";
        cfg.seed_port = 0;
        cfg.signal_ip = "127.0.0.1";
        cfg.signal_port = (uint16_t)server_port;
        cfg.session_key = k_sb6_key;
        cfg.my_public_port = my_port;
        cfg.signal_leg = true;
        cfg.signal_budget_ms = HC31_BUDGET_MS;
        cfg.punch_leg_ms = 60000;
        cfg.race_budget_ms = HC31_BUDGET_MS;

        DirectP2PRaceProbeOut out;
        DirectP2P_TestHook_RunRace(&cfg, &out);
        sb6_log_end();
        armed = s_sb6_arm_lines;
    }

done:
    srv.stop = true;
    if (srv_tid != NULL) SDL_WaitThread(srv_tid, NULL);
    if (out_delivers != NULL) *out_delivers = srv.delivers;
    for (int i = 0; i < HC_MAX_EP; i++) if (sinks[i] >= 0) close_sock(sinks[i]);
    if (server_sock >= 0) close_sock(server_sock);
    if (sock != NULL) NET_DestroyDatagramSocket(sock);
    return armed;
}

static int test_race_rearm_releases_address_ref(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 31: re-arming a candidate RELEASES the outgoing "
            "leg's NET_Address ref (M-2 half ii)\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetPunchOracle(NULL);
    DirectP2P_TestHook_SetArmFailEndpoint(NULL, 0);

    SDL_malloc_func  o_malloc = NULL;
    SDL_calloc_func  o_calloc = NULL;
    SDL_realloc_func o_realloc = NULL;
    SDL_free_func    o_free = NULL;
    SDL_GetMemoryFunctions(&o_malloc, &o_calloc, &o_realloc, &o_free);

    /* Warm-up race: pays every first-touch allocation before the window. */
    int warm_delivers = 0;
    const int warm_armed = hc31_run_one_race(HC31_REARMS, &warm_delivers);
    if (warm_armed < 0) {
        FAIL("test31", "the warm-up race could not bring the rig up");
        rc = 1;
        goto done;
    }
    SDL_Delay(500);

    SDL_SetAtomicInt(&g_hc_live_allocs, 0);
    if (!SDL_SetMemoryFunctions(hc_malloc, hc_calloc, hc_realloc, hc_free)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test31: SDL_SetMemoryFunctions was "
                "refused (%s) — the leak cannot be observed\n", SDL_GetError());
        fail_count++;
        rc = 1;
        goto done;
    }

    int delivers = 0;
    const int armed = hc31_run_one_race(HC31_REARMS, &delivers);
    /* Let the SDL_net resolver thread finish retiring anything the race
     * released on its way out before the window is closed. */
    SDL_Delay(500);
    const int delta = SDL_GetAtomicInt(&g_hc_live_allocs);
    SDL_SetMemoryFunctions(o_malloc, o_calloc, o_realloc, o_free);

    fprintf(stderr,
            "[test_bilateral_punch] test 31: warm-up armed %d, measured race armed %d "
            "of %d DELIVERs; live-allocation delta across the measured race = %+d "
            "(a leaked NET_Address is 3)\n",
            warm_armed, armed, delivers, delta);

    if (armed < 0) {
        FAIL("test31", "the measured race could not bring the rig up");
        rc = 1;
        goto done;
    }
    if (armed < HC31_MIN_ARMS) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test31: only %d candidates were armed "
                "(needed >= %d); with too few re-arms the allocation delta is not "
                "measuring the release path\n", armed, HC31_MIN_ARMS);
        fail_count++;
        rc = 1;
    }
    if (delta > HC31_MAX_DELTA) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test31: %+d live allocations survived a "
                "race that re-armed slot 1 %d time(s), bound is %d. Each un-released "
                "NET_Address is 3 live allocations, so this is ~%d leaked address(es). "
                "race_arm_punch must call race_finish_punch(c, now) BEFORE it memsets "
                "the slot — the memset loses the only pointer to the ref'd address the "
                "outgoing leg still holds in leg.target\n",
                delta, armed > 0 ? armed - 1 : 0, HC31_MAX_DELTA, delta / 3);
        fail_count++;
        rc = 1;
    }
    if (rc == 0 && fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 31 OK — %d re-arms leaked %+d live "
                "allocations (bound %d)\n", armed - 1, delta, HC31_MAX_DELTA);
    }

done:
    DirectP2P_TestHook_SetArmFailEndpoint(NULL, 0);
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Test 41: a re-arm must not destroy a CONFIRMED leg (#120) --------- */

/*
 * THE PROMISED LISTENING INTERVAL, site 4 (see the rule of that name in
 * direct_p2p.c). Slot 1 is re-armed whenever a DELIVER carries a
 * DIFFERENT endpoint, and race_arm_punch calls race_finish_punch() on the
 * incumbent to release its leg. Before the guard that call was
 * UNCONDITIONAL, so a second DELIVER landing between a leg's CONFIRM and
 * its SETTLE ran Stun_PunchEnd on a punch that had provably reached us —
 * and stopped the tail owed to a peer our partial tail may already have
 * confirmed. Section 1 of p2p_race only ends the race once a confirmed
 * leg SETTLES, so that band is exactly RACE_PUNCH_SETTLE_MS wide and the
 * leg is live and valuable throughout it.
 *
 * REACHABILITY, stated honestly: this needs the registered endpoint to
 * CHANGE mid-race (a reclaim push, or a NAT rebind), which is narrow. No
 * divergent production trace was ever constructed. The rig below does not
 * claim one — it constructs the mechanism directly, which is what the
 * guard is written against.
 *
 * THE RIG. Two endpoints, delivered in sequence by hc_server_thread:
 *   A = punch_echo_thread with delay 0, so the REAL leg armed on it
 *       confirms on the first pump (~one 5 ms tick after the arm).
 *   B = hc_sink_thread, which never echoes, so a leg armed on it can
 *       never confirm and the race would run to EXHAUSTED on it.
 * The gap between the two DELIVERs is HC41_GAP_MS, which must sit inside
 * A's settle window: after the gap A is CONFIRMED and NOT YET SETTLED,
 * which is the only state in which the defect is reachable.
 *
 * WHY IT CANNOT PASS FOR THE WRONG REASON. `echoes_sent > 0` proves A
 * really confirmed (not merely that B was refused), `delivers >= 2`
 * proves the rig really offered a second, different endpoint, and
 * `arm_lines == 1` is the guard itself. Without the guard the race ends
 * EXHAUSTED on a silent B; with it, PUNCHED on A.
 */
#define HC41_GAP_MS 200
#define HC41_BUDGET_MS 4000

static int test_race_rearm_over_confirmed_leg(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 41: a DELIVER carrying a DIFFERENT endpoint "
            "does NOT re-arm over a CONFIRMED leg (#120)\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetPunchOracle(NULL); /* REAL legs only */
    DirectP2P_TestHook_SetArmFailEndpoint(NULL, 0);

    /* The whole test lives inside A's tail; if the gap ever grew past it,
     * A would have SETTLED and section 1 would have ended the race before
     * the second DELIVER, so the guard would never be exercised and the
     * test would pass vacuously. */
    if (HC41_GAP_MS >= STUN_PUNCH_CONFIRM_MS) {
        FAIL("test41", "HC41_GAP_MS must be inside the confirmation tail");
        return 1;
    }

    unsigned short echo_port = 0, sink_port = 0, server_port = 0;
    PunchEchoCtx echo;
    HcSinkCtx sink_b;
    HcServerCtx srv;
    memset(&echo, 0, sizeof(echo));
    memset(&sink_b, 0, sizeof(sink_b));
    memset(&srv, 0, sizeof(srv));

    echo.sock = open_udp_on_localhost(&echo_port);
    echo.delay_ms = 0;                 /* confirm A as early as possible */
    sink_b.sock = open_udp_on_localhost(&sink_port);
    const int server_sock = open_udp_on_localhost(&server_port);
    uint16_t my_port = 0;
    NET_DatagramSocket* sock = sb6_net_socket(&my_port);

    SDL_Thread* echo_tid = NULL;
    SDL_Thread* b_tid = NULL;
    SDL_Thread* srv_tid = NULL;

    if (echo.sock < 0 || sink_b.sock < 0 || server_sock < 0 || sock == NULL) {
        FAIL("test41", "could not bind the rig sockets");
        rc = 1;
        goto done;
    }

    srv.sock = server_sock;
    srv.life_secs = 30;
    srv.gap_ms = HC41_GAP_MS;
    srv.n_ports = 2;
    srv.ports[0] = (uint16_t)echo_port;   /* confirms */
    srv.ports[1] = (uint16_t)sink_port;   /* silent — must never be armed */

    echo_tid = SDL_CreateThread(punch_echo_thread, "hc_echo41", &echo);
    b_tid = SDL_CreateThread(hc_sink_thread, "hc_sink41", &sink_b);
    srv_tid = SDL_CreateThread(hc_server_thread, "hc_srv41", &srv);
    if (echo_tid == NULL || b_tid == NULL || srv_tid == NULL) {
        FAIL("test41", "SDL_CreateThread failed");
        rc = 1;
        goto done;
    }

    sb6_log_begin();
    {
        DirectP2PRaceProbeCfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.host_role = false;
        cfg.sock = sock;
        cfg.punch_token = k_sb6_token;
        cfg.seed_ip = "127.0.0.1";
        cfg.seed_port = 0;          /* no seed leg: slot 1 is the only candidate */
        cfg.signal_ip = "127.0.0.1";
        cfg.signal_port = (uint16_t)server_port;
        cfg.session_key = k_sb6_key;
        cfg.my_public_port = my_port;
        cfg.signal_leg = true;
        cfg.signal_budget_ms = HC41_BUDGET_MS;
        /* Longer than the race, so a leg can only stop being punched
         * because its slot was taken — never because it timed out. */
        cfg.punch_leg_ms = 60000;
        cfg.race_budget_ms = HC41_BUDGET_MS;

        DirectP2PRaceProbeOut out;
        DirectP2P_TestHook_RunRace(&cfg, &out);
        sb6_log_end();

        fprintf(stderr,
                "[test_bilateral_punch] test 41: delivers=%d arm-lines=%d "
                "echo(punches=%d echoes=%d) B(punches=%d) outcome=%d peer=%s:%u\n",
                srv.delivers, s_sb6_arm_lines, echo.punches_seen, echo.echoes_sent,
                sink_b.punches, (int)out.outcome, out.peer_ip,
                (unsigned)out.peer_port);

        /* --- rig sanity: the second, DIFFERENT endpoint was offered --- */
        if (srv.delivers < 2) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test41: the mock sent only %d "
                    "DELIVER(s); the guard had no re-arm to refuse\n", srv.delivers);
            fail_count++;
            rc = 1;
        }
        /* --- rig sanity: A really CONFIRMED ---------------------------- */
        if (echo.echoes_sent <= 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test41: the echo peer never echoed, "
                    "so no leg ever confirmed and this test proves nothing\n");
            fail_count++;
            rc = 1;
        }

        /* --- the guard ------------------------------------------------- */
        if (s_sb6_arm_lines != 1) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test41: %d candidates were armed "
                    "against %d DELIVERs, expected exactly 1 — a DELIVER carrying a "
                    "different endpoint re-armed slot 1 over a CONFIRMED leg, which "
                    "Stun_PunchEnds it mid-tail (THE PROMISED LISTENING INTERVAL, "
                    "site 4)\n",
                    s_sb6_arm_lines, srv.delivers);
            fail_count++;
            rc = 1;
        }
        /* B is a silent sink: punching it at all means the confirmed leg
         * was thrown away for an endpoint that can never answer. */
        EXPECT_TRUE("41-silent-endpoint-never-punched", sink_b.punches == 0);
        /* And the race must still converge on the endpoint that answered. */
        EXPECT_TRUE("41-punched", out.outcome == DP2P_RACE_PROBE_PUNCHED);
        EXPECT_TRUE("41-peer-is-the-confirmed-leg", out.peer_port == echo_port);

        if (rc == 0 && fail_count == fails_before) {
            fprintf(stderr,
                    "[test_bilateral_punch] test 41 OK — %d DELIVERs armed exactly 1 "
                    "candidate and the confirmed leg survived to PUNCHED\n",
                    srv.delivers);
        }
    }

done:
    echo.stop = true;
    sink_b.stop = true;
    srv.stop = true;
    if (echo_tid != NULL) SDL_WaitThread(echo_tid, NULL);
    if (b_tid != NULL) SDL_WaitThread(b_tid, NULL);
    if (srv_tid != NULL) SDL_WaitThread(srv_tid, NULL);
    if (echo.sock >= 0) close_sock(echo.sock);
    if (sink_b.sock >= 0) close_sock(sink_b.sock);
    if (server_sock >= 0) close_sock(server_sock);
    if (sock != NULL) NET_DestroyDatagramSocket(sock);
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Test 34: TWO p2p_race instances, concurrently, against each other - */

/*
 * WHY THIS RIG EXISTS AT ALL.
 *
 * Every other seam in this file drives the race through BeginJoin or the
 * host worker, and those own process-wide state (s_work, s_state, the
 * worker threads), so exactly ONE race can be live at a time. p2p_race
 * itself takes everything by argument, and DirectP2P_TestHook_RunRace is
 * the door to it — so this is the only place in the tree where two races
 * run CONCURRENTLY, punching each other through a delay line, which is
 * the only way a two-peer property can be observed at all.
 *
 * That rig arrived with the S5 relay (as test 24, the split-brain hunt)
 * and went out with it. Four of its eight probe points were relay
 * arbitration — three derived from RACE_RELAY_ARM_MS + RACE_RELAY_GRACE_MS
 * (the relay-commit instant) and one that asserted a RELAYED outcome —
 * and those are correctly gone, along with RACE_PUNCH_MIN_WINDOW_MS.
 * What is restored here are the points that were never about the relay:
 * the three derived from the PUNCH SEND WINDOW (`punch_leg_ms`), plus the
 * control point that proves the rig delivers punches at all.
 *
 * WHAT IS ASSERTED, AND WHY IT IS NOT THE OLD ASSERTION.
 *
 * The old points expected PUNCHED at punch_end-owd, punch_end and
 * punch_end+owd. That expectation was produced by a rule that the first
 * draft of the relay removal deleted: a candidate stays on the RECEIVE
 * path past its send window, which had been scoped to `relay_in_play`
 * and went out with the relay. It is back, unconditionally, as
 * `RACE_PUNCH_SETTLE_MS` (direct_p2p.c) — see the record below for what
 * this test measured while it was missing — so those instants can once
 * again end PUNCHED. They are deliberately NOT pinned either way here:
 * whether the last punch in flight lands before or after a teardown is
 * a scheduling detail, and pinning it would make this test a
 * thermometer for the machine it runs on.
 *
 * So the property asserted at the punch-send-window instants is the one
 * that survived, and it is the one the whole split-brain hunt was
 * actually about:
 *
 *   CONVERGENCE — at every skew, peer A and peer B reach the SAME
 *   outcome. One peer PUNCHED while the other is EXHAUSTED is the
 *   split brain: GekkoNet registers the remote once by source address
 *   with no relearn path, so that pair hangs for the whole
 *   CONNECT_TIMEOUT_CONNECTING_MS and fails.
 *
 * Convergence alone is trivially satisfiable by a rig that punches
 * nothing (both EXHAUSTED), so it is anchored at both ends:
 *
 *   - the CONTROL skew, comfortably inside the overlap, must converge on
 *     PUNCHED. If it reds the rig is broken, not the code.
 *   - the PAST skew, a full punch window plus several delays beyond the
 *     end, must converge on EXHAUSTED. This is the anchor that keeps the
 *     settle window HONEST: a fix that merely relocated the band, or one
 *     that made every skew punch by listening forever, reds here. It is
 *     also the point that catches a rig not injecting the delay it says
 *     it is. It is meaningful only while it sits past the last instant
 *     either peer can still be confirmed, `punch_leg + settle - owd`;
 *     `past` is `punch_leg + 3*owd + 500`, so that holds for every
 *     owd above (RACE_PUNCH_SETTLE_MS - 500) / 4 = 25 ms.
 *
 * Both punch legs are DP2P_PUNCH_REAL (no oracle), so this runs the real
 * Stun_PunchBegin / Pump / Offer / Settled machine over real loopback
 * UDP on both sides. RaceCfg carries no role field at all — the race is
 * symmetric by derivation (RACE_PUNCH_SETTLE_MS and the G >= 2d
 * convergence condition have no role term), and RunRace never threads
 * the probe's `host_role` flag into p2p_race, so HOST on both sides
 * costs nothing.
 *
 * WHAT THIS TEST FOUND THE MOMENT IT WAS RESTORED — READ BEFORE
 * "FIXING" THE TEST.
 *
 * (FIXED in direct_p2p.c, not here. The record below is what this test
 * measured against the first draft of the relay removal, and it is the
 * red this test must still produce if `RACE_PUNCH_SETTLE_MS` is ever
 * taken back out. Verified by reverting direct_p2p.c to that draft and
 * re-running: the 2350 ms row below reproduces exactly.)
 *
 * It went RED at skew = punch_leg - owd, and the red was the shipping
 * code, not the rig:
 *
 *   skew= 500 ms -> A=PUNCHED   B=PUNCHED     (control)
 *   skew=2350 ms -> A=PUNCHED   B=EXHAUSTED   *** SPLIT BRAIN ***
 *   skew=2500 ms -> A=PUNCHED   B=EXHAUSTED   (boundary; also seen converged)
 *   skew=2650 ms -> A=EXHAUSTED B=EXHAUSTED
 *   skew=3450 ms -> A=EXHAUSTED B=EXHAUSTED   (past every punch)
 *
 * It is BOUNDARY-LOCKED, not a flake. The band moves with whichever
 * constant is moved, measured over three configurations:
 *
 *   owd=150 punch_leg=2500 -> splits at 2350 and 2500, converged at 2650
 *   owd=150 punch_leg=1800 -> splits at 1650 and 1800, converged at 1950
 *   owd=300 punch_leg=2500 -> splits at 2200 and 2500, converged at 2800
 *
 * i.e. the divergence band is skew in [punch_leg - owd, punch_leg], one
 * one-way delay wide, and it closes one owd after the send window ends
 * because by then the early peer has stopped punching before the late
 * peer armed and neither side can confirm.
 *
 * The mechanism: the early peer B stopped SENDING and stopped LISTENING
 * at the same instant. The late peer A arms one owd before that, so A is
 * confirmed by B's last punches while A's own first punch reaches B
 * exactly as B tears the leg down — section 2 runs before the shared
 * receive path in the same loop iteration, so B never sees it. A hands
 * off to GekkoNet; B reports failure.
 *
 * This band did not exist before the relay was removed. The deferral
 * that closed it — hold a candidate on the RECEIVE path past its send
 * window — was scoped to `relay_in_play`, and in production a relay leg
 * was always possible, so it always applied. The first draft of the
 * removal deleted the deferral along with the relay and asserted in
 * prose the thing this test measures to be false: "With no relay there
 * is nothing to disagree about: both peers simply fail."
 *
 * The base suite could not see this: its rig always ran with
 * relay_leg = true, so the only configuration it ever probed was the one
 * where the deferral applied. The base comment nevertheless names the
 * exact failure mode in advance, at
 * 9240aa50:src/netplay/test_bilateral_punch.c:8040-8043 — "PUNCH SEND
 * END ... The early peer stops sending here. If it also stopped
 * LISTENING here, the late peer's punch would confirm one-sidedly and
 * the band would simply have moved to this instant instead."
 *
 * THE FIX BELONGS IN direct_p2p.c, NOT IN THIS FILE. It is section 2 of
 * p2p_race keeping a send-expired candidate on the receive path for
 * `RACE_PUNCH_SETTLE_MS`, unconditionally rather than only while a relay
 * leg was deciding. Do not relax the convergence assertion to make this
 * green.
 */

/* One-way delay injected between the two peers, and the rig's punch SEND
 * window. Both are runtime knobs so the probe points can be pointed
 * anywhere without a rebuild — the literals below are defaults, not
 * chosen constants. */
#define SB6_OWD_DEFAULT_MS      150
#define SB6_RIG_PUNCH_LEG_MS    2500
#define SB6_RIG_BUDGET_MS       6000
/* A control skew comfortably below the end of the punch window: both
 * sides must punch. */
#define SB6_SAFE_SKEW_MS        500

static int sb6_env_int(const char* name, int fallback) {
    const char* v = SDL_getenv(name);
    if (v == NULL || v[0] == '\0') {
        return fallback;
    }
    return SDL_atoi(v);
}

static int sb6_owd_ms(void) {
    const int v = sb6_env_int("S6_SPLIT_OWD_MS", SB6_OWD_DEFAULT_MS);
    return (v < 0) ? 0 : v;
}

static int sb6_punch_leg_ms(void) {
    return sb6_env_int("S6_SPLIT_PUNCH_LEG_MS", SB6_RIG_PUNCH_LEG_MS);
}

static const char* sb6_outcome_name(DirectP2PRaceProbeOutcome o) {
    switch (o) {
    case DP2P_RACE_PROBE_PUNCHED:   return "PUNCHED";
    case DP2P_RACE_PROBE_CANCELLED: return "CANCELLED";
    default:                        return "EXHAUSTED";
    }
}

/* A store-and-forward pipe between the two peers. Each side's punches
 * are held for `delay_ms` and then sent out of the OTHER side's socket,
 * so each peer sees the source endpoint it was told to punch and no
 * symmetric-NAT retarget is involved — the property under test stays
 * single. */
typedef struct {
    int sock_a;   /* peer A punches here; forwarded to B out of sock_b */
    int sock_b;   /* peer B punches here; forwarded to A out of sock_a */
    int delay_ms;
    volatile bool stop;
    struct sockaddr_in addr_a;
    struct sockaddr_in addr_b;
    bool have_a;
    bool have_b;
    volatile int forwarded;
    volatile int dropped_unknown_peer;
    struct {
        uint8_t  buf[64];
        int      len;
        uint32_t due;
        bool     to_b;
        bool     used;
    } q[512];
} DelayLineCtx;

static void sb6_delay_enqueue(DelayLineCtx* c, const uint8_t* b, int n,
                              bool to_b, uint32_t due) {
    for (size_t i = 0; i < sizeof(c->q) / sizeof(c->q[0]); i++) {
        if (c->q[i].used) continue;
        memcpy(c->q[i].buf, b, (size_t)((n > 64) ? 64 : n));
        c->q[i].len = (n > 64) ? 64 : n;
        c->q[i].due = due;
        c->q[i].to_b = to_b;
        c->q[i].used = true;
        return;
    }
}

static int SDLCALL sb6_delay_line_thread(void* arg) {
    DelayLineCtx* c = (DelayLineCtx*)arg;
    for (;;) {
        if (c->stop) return 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(c->sock_a, &rfds);
        FD_SET(c->sock_b, &rfds);
        const int maxfd = (c->sock_a > c->sock_b) ? c->sock_a : c->sock_b;
        struct timeval tv = { 0, 2 * 1000 };
        const int sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        const uint32_t now = SDL_GetTicks();

        if (sel > 0 && FD_ISSET(c->sock_a, &rfds)) {
            uint8_t b[128];
            struct sockaddr_in src;
            socklen_t sl = sizeof(src);
            const int n = (int)recvfrom(c->sock_a, (char*)b, sizeof(b), 0,
                                        (struct sockaddr*)&src, &sl);
            if (n > 0) {
                c->addr_a = src;
                c->have_a = true;
                sb6_delay_enqueue(c, b, n, /*to_b*/ true, now + (uint32_t)c->delay_ms);
            }
        }
        if (sel > 0 && FD_ISSET(c->sock_b, &rfds)) {
            uint8_t b[128];
            struct sockaddr_in src;
            socklen_t sl = sizeof(src);
            const int n = (int)recvfrom(c->sock_b, (char*)b, sizeof(b), 0,
                                        (struct sockaddr*)&src, &sl);
            if (n > 0) {
                c->addr_b = src;
                c->have_b = true;
                sb6_delay_enqueue(c, b, n, /*to_b*/ false, now + (uint32_t)c->delay_ms);
            }
        }

        for (size_t i = 0; i < sizeof(c->q) / sizeof(c->q[0]); i++) {
            if (!c->q[i].used) continue;
            if ((int)(now - c->q[i].due) < 0) continue;
            if (c->q[i].to_b) {
                if (c->have_b) {
                    sendto(c->sock_b, (const char*)c->q[i].buf, c->q[i].len, 0,
                           (struct sockaddr*)&c->addr_b, sizeof(c->addr_b));
                    c->forwarded++;
                } else {
                    c->dropped_unknown_peer++;
                }
            } else {
                if (c->have_a) {
                    sendto(c->sock_a, (const char*)c->q[i].buf, c->q[i].len, 0,
                           (struct sockaddr*)&c->addr_a, sizeof(c->addr_a));
                    c->forwarded++;
                } else {
                    c->dropped_unknown_peer++;
                }
            }
            c->q[i].used = false;
        }
    }
}

typedef struct {
    DirectP2PRaceProbeCfg cfg;
    DirectP2PRaceProbeOut out;
    int start_delay_ms;
    volatile bool done;
} Sb6PeerCtx;

static int SDLCALL sb6_peer_thread(void* arg) {
    Sb6PeerCtx* p = (Sb6PeerCtx*)arg;
    if (p->start_delay_ms > 0) SDL_Delay((Uint32)p->start_delay_ms);
    DirectP2P_TestHook_RunRace(&p->cfg, &p->out);
    p->done = true;
    return 0;
}

/* Run ONE two-peer race at a given start skew. Returns false only if the
 * rig itself could not be stood up. */
static bool sb6_run_two_peer(int skew_ms, int owd_ms,
                             DirectP2PRaceProbeOutcome* out_a,
                             DirectP2PRaceProbeOutcome* out_b) {
    unsigned short pa = 0, pb = 0;
    const int sock_a = open_udp_on_localhost(&pa);
    const int sock_b = open_udp_on_localhost(&pb);
    uint16_t port_a = 0, port_b = 0;
    NET_DatagramSocket* net_a = sb6_net_socket(&port_a);
    NET_DatagramSocket* net_b = sb6_net_socket(&port_b);
    if (sock_a < 0 || sock_b < 0 || net_a == NULL || net_b == NULL) {
        if (sock_a >= 0) close_sock(sock_a);
        if (sock_b >= 0) close_sock(sock_b);
        if (net_a != NULL) NET_DestroyDatagramSocket(net_a);
        if (net_b != NULL) NET_DestroyDatagramSocket(net_b);
        return false;
    }

    DelayLineCtx* line = (DelayLineCtx*)SDL_calloc(1, sizeof(DelayLineCtx));
    if (line == NULL) {
        close_sock(sock_a);
        close_sock(sock_b);
        NET_DestroyDatagramSocket(net_a);
        NET_DestroyDatagramSocket(net_b);
        return false;
    }
    line->sock_a = sock_a;
    line->sock_b = sock_b;
    line->delay_ms = owd_ms;

    SDL_Thread* line_tid = SDL_CreateThread(sb6_delay_line_thread, "sb6_line", line);

    Sb6PeerCtx a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.start_delay_ms = skew_ms;
    b.start_delay_ms = 0;

    /* No signal leg and no signal endpoint: with the relay gone there is
     * nothing in this rig for a rendezvous server to do, and RunRace
     * treats a NULL signal_ip as "no legs" (direct_p2p.c:2339-2356). The
     * seed candidate — the delay line — is the whole race. */
    a.cfg.host_role = true;
    a.cfg.sock = net_a;
    a.cfg.punch_token = k_sb6_token;
    a.cfg.seed_ip = "127.0.0.1";
    a.cfg.seed_port = (uint16_t)pa;   /* A punches the line's A side */
    a.cfg.signal_ip = NULL;
    a.cfg.signal_port = 0;
    a.cfg.session_key = k_sb6_key;
    a.cfg.my_public_port = port_a;
    a.cfg.signal_leg = false;
    a.cfg.punch_leg_ms = sb6_punch_leg_ms();
    a.cfg.race_budget_ms = sb6_env_int("S6_SPLIT_BUDGET_MS", SB6_RIG_BUDGET_MS);

    b.cfg = a.cfg;
    b.cfg.sock = net_b;
    b.cfg.seed_port = (uint16_t)pb;   /* B punches the line's B side */
    b.cfg.my_public_port = port_b;

    SDL_Thread* ta = SDL_CreateThread(sb6_peer_thread, "sb6_peer_a", &a);
    SDL_Thread* tb = SDL_CreateThread(sb6_peer_thread, "sb6_peer_b", &b);
    if (ta != NULL) SDL_WaitThread(ta, NULL);
    if (tb != NULL) SDL_WaitThread(tb, NULL);

    line->stop = true;
    if (line_tid != NULL) SDL_WaitThread(line_tid, NULL);

    *out_a = a.out.outcome;
    *out_b = b.out.outcome;

    SDL_free(line);
    close_sock(sock_a);
    close_sock(sock_b);
    NET_DestroyDatagramSocket(net_a);
    NET_DestroyDatagramSocket(net_b);
    return true;
}

typedef struct {
    int  skew_ms;
    bool expect_punched; /* false => both must EXHAUST */
    bool pinned;         /* false => convergence only (see below) */
    const char* why;
} Sb6ProbePoint;

static int test_race_two_peer_convergence(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 34: two real peers punching each other "
            "through a %d ms one-way delay must never end on different rungs\n",
            sb6_owd_ms());
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetPunchOracle(NULL); /* REAL legs on both peers */

    const int owd = sb6_owd_ms();
    const int punch_end = sb6_punch_leg_ms();
    /* Comfortably past every punch: the late peer starts a full send
     * window plus several delays after the early peer's window closed. */
    const int past = punch_end + 3 * owd + 500;

    const Sb6ProbePoint points[] = {
        /* The anti-triviality anchor at the PUNCHED end. */
        { SB6_SAFE_SKEW_MS, true, true,
          "control: well inside the overlap — both peers must punch" },
        /* The three points the relay never had anything to do with: the
         * PUNCH SEND WINDOW instant, probed at -owd, +0 and +owd, which
         * is where a one-round-trip asymmetry lands. Their outcome is
         * NOT pinned: exactly at the instant a leg is torn down, whether
         * the last punch in flight lands before or after the teardown is
         * a scheduling detail. What must hold either way — and what the
         * whole two-peer rig exists to observe — is that BOTH peers
         * reach the SAME answer. */
        { punch_end - owd, false, false,
          "one owd BEFORE the punch send window ends" },
        { punch_end, false, false,
          "exactly AT the end of the punch send window" },
        { punch_end + owd, false, false,
          "one owd AFTER the punch send window ends" },
        /* The anti-triviality anchor at the EXHAUSTED end. */
        { past, false, true,
          "past every punch and past every settle window: both peers must fail" },
    };

    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        DirectP2PRaceProbeOutcome oa = DP2P_RACE_PROBE_EXHAUSTED;
        DirectP2PRaceProbeOutcome ob = DP2P_RACE_PROBE_EXHAUSTED;
        if (!sb6_run_two_peer(points[i].skew_ms, owd, &oa, &ob)) {
            FAIL("test34", "could not stand up the two-peer rig");
            rc = 1;
            goto done;
        }
        if (oa != ob) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test34: SPLIT BRAIN at skew=%d ms "
                    "(%s) — peer A ended %s and peer B ended %s. The two peers are on "
                    "different rungs; GekkoNet registers the remote once by source "
                    "address with no relearn path, so this pair hangs for the whole "
                    "%u ms CONNECT_TIMEOUT_CONNECTING_MS and fails\n",
                    points[i].skew_ms, points[i].why, sb6_outcome_name(oa),
                    sb6_outcome_name(ob), (unsigned)CONNECT_TIMEOUT_CONNECTING_MS);
            fail_count++;
            rc = 1;
        }
        if (points[i].pinned) {
            const DirectP2PRaceProbeOutcome want = points[i].expect_punched
                                                       ? DP2P_RACE_PROBE_PUNCHED
                                                       : DP2P_RACE_PROBE_EXHAUSTED;
            if (oa != want) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: test34: at skew=%d ms (%s) the "
                        "converged outcome must be %s, not %s%s\n",
                        points[i].skew_ms, points[i].why, sb6_outcome_name(want),
                        sb6_outcome_name(oa),
                        points[i].expect_punched
                            ? " — the rig is not delivering punches at all, so every "
                              "convergence verdict above it is vacuous"
                            : " — this skew is past every punch, so a PUNCHED outcome "
                              "means the rig is not injecting the delay it claims");
                fail_count++;
                rc = 1;
            }
        }
        fprintf(stderr,
                "[test_bilateral_punch] test34: skew=%4d ms -> A=%-9s B=%-9s (%s)\n",
                points[i].skew_ms, sb6_outcome_name(oa), sb6_outcome_name(ob),
                points[i].why);
    }

    if (rc == 0 && fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 34 OK — two concurrent races converged at "
                "every probe point around the punch send window (%d ms), punched at "
                "the control (%d ms) and both failed past it (%d ms), owd=%d ms\n",
                punch_end, SB6_SAFE_SKEW_MS, past, owd);
    }

done:
    DirectP2P_TestHook_RunTeardown();
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* ======================================================================
 * Task #76 — the NAV_WAIT_ORCHESTRATOR backstop is DERIVED, not flat
 * ======================================================================
 *
 * The bug: a wedged orchestrator left the player on a static
 * "Connecting..." overlay for a flat 150 s before any attributed failure
 * appeared. netplay_nav.c now derives its deadline from
 * DirectP2P_OrchWorstCaseMs(), which sums the orchestrator's own live
 * clamped budgets.
 *
 * DIVISION OF LABOUR with the _Static_asserts in direct_p2p.c. Those
 * check the cascade at COMPILE time against fixed configs (shipped
 * defaults, and both clamp extremes) — they are the tripwire that makes
 * a future cascade change break the BUILD. They cannot check that the
 * production path actually CONSUMES the derivation, because a constant
 * would satisfy a fixed-config assertion just as well. That is this
 * test's job: it varies the live config and demands the enforced
 * deadline move with it.
 *
 * NEUTRALIZATION. Each assertion names the mutation it exists to catch:
 *
 *   [N1] Revert netplay_nav.c to `#define NAV_WAIT_ORCH_TIMEOUT_FRAMES
 *        (150 * 60)`. Caught by the shipped-defaults ceiling.
 *   [N2] Replace the derivation with ANY constant (150 s, 60 s, 30 s).
 *        Caught by the slope check, which pins the increase to exactly
 *        the JOIN_MAX_ATTEMPTS passes join_thread_fn actually runs. A
 *        constant has slope 0; a one-attempt bound has half the slope.
 *   [N3] Size the joiner against the race budget ALONE, letting nav cut
 *        inside the S6/S7 H-1 confirmation tail and resurrecting the
 *        misattribution H-1 was written to fix.
 *   [N4] Make the bound role-blind (drop the ForRole switch, or hand a
 *        joiner the host term). Caught by the defaults ceiling: the host
 *        ladder term is ~3x the joiner term and blows it.
 *   [N5] Charge the race ONE confirmation tail instead of TWO — i.e.
 *        revert RACE_HARD_CAP_MS to `budget + 1 * STUN_PUNCH_CONFIRM_MS`.
 *        This is not hypothetical: it is EXACTLY the drift that happened
 *        while this work sat parked. The one-tail sum was correct when
 *        written and went stale the moment p2p_race's section 8 granted a
 *        confirmed leg a second tail. The tail-containment check below is
 *        sized against BOTH tails precisely so a one-tail regression is
 *        red rather than merely 600 ms optimistic.
 *
 * Direction note (S6/S7 review H-B): every bound below is computed from
 * PRODUCTION values — the config figures this test itself writes, and
 * STUN_PUNCH_CONFIRM_MS / CONNECT_TIMEOUT_CONNECTING_MS /
 * NAV_ORCH_TIMEOUT_MARGIN_MS from their production headers. No
 * production margin is sized against a constant that lives in this
 * file. */
static int test_nav_orch_deadline_is_derived(void) {
    fprintf(stderr, "[test_bilateral_punch] test 40: task #76 nav orchestrator "
                    "deadline is derived from the live budgets\n");
    const int fails_before = fail_count;

    /* The old flat constant, reproduced here ONLY as the thing we must be
     * strictly under. Nothing in production reads it any more. */
    const int old_flat_frames = 150 * 60;

    /* The joiner's attempt count. Mirrors JOIN_MAX_ATTEMPTS, which is
     * private to direct_p2p.c; the slope check below is what proves the
     * mirror is still accurate, so it cannot drift silently. */
    const int join_attempts = 2;

    /* A previous test in this process may have left the race-budget test
     * seam armed; it would mask every config write below. */
    DirectP2P_TestHook_SetRaceBudgetMs(0);

    /* ---- [N1]/[N4] shipped defaults: joiner deadline is human-scale --- */
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS, "4000");
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RACE_BUDGET_MS, "8000");

    const int join_default_frames =
        NetplayNav_OrchTimeoutFrames(DirectP2P_OrchWorstCaseMsForRole(ROLE_JOIN));

    /* The UX ceiling is the SAME product policy direct_p2p.c asserts at
     * compile time, not a number invented here: the longest wait this
     * product already asks a player to accept is
     * CONNECT_TIMEOUT_CONNECTING_MS on the post-handoff overlay, so the
     * pre-handoff cascade gets that once per join attempt plus nav's
     * scheduling margin, and no more.
     *
     * At the shipped defaults the derivation lands at 31 800 ms
     * (200 startup + 2 x (4000 stun + 100 resolve + 8000 race + 2 x 600
     * confirmation tail) + 5000 margin), against a 35 000 ms ceiling. */
    const int ux_ceiling_ms =
        join_attempts * (int)CONNECT_TIMEOUT_CONNECTING_MS + NAV_ORCH_TIMEOUT_MARGIN_MS;
    const int ux_ceiling_frames = (int)(((long long)ux_ceiling_ms * NAV_FPS) / 1000);
    if (join_default_frames > ux_ceiling_frames) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test40: joiner nav deadline at shipped "
                "defaults is %d frames (%d ms), expected <= %d frames (%d ms) — the "
                "player must not sit on a static overlay longer than %u ms per join "
                "attempt plus nav's margin before the attributed failure appears\n",
                join_default_frames, (join_default_frames * 1000) / NAV_FPS,
                ux_ceiling_frames, ux_ceiling_ms,
                (unsigned)CONNECT_TIMEOUT_CONNECTING_MS);
        fail_count++;
    }
    if (join_default_frames >= old_flat_frames) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test40: joiner nav deadline is %d "
                "frames, not under the old flat %d — the deadline is still the "
                "pre-task-#76 constant\n",
                join_default_frames, old_flat_frames);
        fail_count++;
    }

    /* ---- [N2] slope: the deadline tracks the race budget, N attempts -- */
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RACE_BUDGET_MS, "4000");
    const int join_lo_ms = DirectP2P_OrchWorstCaseMsForRole(ROLE_JOIN);
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RACE_BUDGET_MS, "9000");
    const int join_hi_ms = DirectP2P_OrchWorstCaseMsForRole(ROLE_JOIN);

    /* join_thread_fn runs join_attempt() JOIN_MAX_ATTEMPTS times, so a
     * +5000 ms race budget must move the joiner bound by exactly +10000.
     * A constant moves it by 0; a one-attempt bound moves it by 5000. */
    const int expected_delta = join_attempts * (9000 - 4000);
    if (join_hi_ms - join_lo_ms != expected_delta) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test40: raising the race budget by "
                "5000 ms moved the joiner bound by %d ms, expected exactly %d "
                "(%d attempts x the budget delta). A flat constant moves it by 0.\n",
                join_hi_ms - join_lo_ms, expected_delta, join_attempts);
        fail_count++;
    }
    /* Same slope must be visible through the nav conversion the state
     * machine actually enforces, not only through the raw ms. */
    if (NetplayNav_OrchTimeoutFrames(join_hi_ms) <=
        NetplayNav_OrchTimeoutFrames(join_lo_ms)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test40: nav deadline did not increase "
                "with the race budget (%d vs %d frames) — nav is not consuming the "
                "derived bound\n",
                NetplayNav_OrchTimeoutFrames(join_lo_ms),
                NetplayNav_OrchTimeoutFrames(join_hi_ms));
        fail_count++;
    }
    /* And it must track the STUN budget too — a bound that only watched
     * the race budget would pass the slope check above while dropping a
     * whole leg. */
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RACE_BUDGET_MS, "8000");
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS, "2000");
    const int stun_lo_ms = DirectP2P_OrchWorstCaseMsForRole(ROLE_JOIN);
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS, "6000");
    const int stun_hi_ms = DirectP2P_OrchWorstCaseMsForRole(ROLE_JOIN);
    if (stun_hi_ms - stun_lo_ms != join_attempts * (6000 - 2000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test40: raising the STUN budget by "
                "4000 ms moved the joiner bound by %d ms, expected exactly %d — the "
                "STUN leg is not in the sum\n",
                stun_hi_ms - stun_lo_ms, join_attempts * (6000 - 2000));
        fail_count++;
    }

    /* ---- [N3]/[N5] BOTH confirmation tails are inside the bound ------- */
    /* Run at the STUN clamp floor and the race clamp ceiling so the tails
     * are the difference between pass and fail rather than being absorbed
     * by slack.
     *
     * The cap is TWO tails, not one. p2p_race section 8: a leg listening
     * through its RACE_PUNCH_SETTLE_MS window can be CONFIRMED as late as
     * `budget + one tail` (send_end can itself be the budget), and it then
     * owes its peer a FULL tail from there. If the nav bound contains only
     * one tail, the races H-1 rescued get cut off by nav instead and are
     * misattributed again — the regression moved one layer up rather than
     * being fixed. */
    const int stun_floor_ms = 1000;   /* stun_budget_ms() clamp floor      */
    const int race_ceil_ms  = 30000;  /* race_budget_ms() clamp ceiling    */
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", stun_floor_ms);
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS, buf);
    snprintf(buf, sizeof(buf), "%d", race_ceil_ms);
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RACE_BUDGET_MS, buf);

    const int join_tail_ms = DirectP2P_OrchWorstCaseMsForRole(ROLE_JOIN);
    const int tail_floor_ms =
        join_attempts * (stun_floor_ms + race_ceil_ms + 2 * (int)STUN_PUNCH_CONFIRM_MS);
    if (join_tail_ms < tail_floor_ms) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test40: joiner bound is %d ms, below "
                "the %d ms needed for %d attempts x (STUN %d + race %d + TWO H-1 "
                "confirmation tails of %d). The nav deadline can now cut inside a "
                "confirmed punch's tail — this is the S6/S7 H-1 regression.\n",
                join_tail_ms, tail_floor_ms, join_attempts, stun_floor_ms,
                race_ceil_ms, (int)STUN_PUNCH_CONFIRM_MS);
        fail_count++;
    }

    /* ---- [N4] the two role bounds are genuinely different ------------- */
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS, "4000");
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RACE_BUDGET_MS, "8000");
    const int host_default_ms = DirectP2P_OrchWorstCaseMsForRole(ROLE_HOST);
    const int join_default_ms = DirectP2P_OrchWorstCaseMsForRole(ROLE_JOIN);
    if (host_default_ms <= join_default_ms) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test40: host bound (%d ms) is not "
                "above the joiner bound (%d ms) at shipped defaults — the role "
                "switch has been flattened, so the joiner is being charged the "
                "host's port-map + STUN retry ladder\n",
                host_default_ms, join_default_ms);
        fail_count++;
    }
    /* And a role-blind caller must be bounded by the LONGER path, never
     * short: ROLE_NONE is what nav sees if the role has not been
     * published yet. */
    if (DirectP2P_OrchWorstCaseMsForRole(ROLE_NONE) < host_default_ms) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test40: ROLE_NONE bound (%d ms) is "
                "shorter than the host bound (%d ms) — an unpublished role would "
                "get a deadline too short for the path it may take\n",
                DirectP2P_OrchWorstCaseMsForRole(ROLE_NONE), host_default_ms);
        fail_count++;
    }

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 40 OK — joiner deadline at shipped "
                "defaults %d frames (%d ms) vs the old flat %d frames (150000 ms); "
                "tracks race and STUN budgets at %d attempts; contains both %d ms "
                "confirmation tails\n",
                join_default_frames, (join_default_frames * 1000) / NAV_FPS,
                old_flat_frames, join_attempts, (int)STUN_PUNCH_CONFIRM_MS);
    }

    return (fail_count == fails_before) ? 0 : 1;
}

/* --- Test 42: mid-session failure taxonomy reaches teardown (task #144) */
/*
 * Regression for queue #144: GekkoPlayerDisconnected and GekkoDesyncDetected
 * in netplay.c's process_session() used to push_event() and
 * handle_disconnection() without ever calling DirectP2P_NotifySessionFailed,
 * so a mid-match peer drop or a detected desync parked the orchestrator in
 * IDLE at teardown — the reason-bearing overlay only ever fired for
 * connect-phase failures, and a RUNNING-phase failure dropped the player to
 * attract mode with no on-screen explanation at all.
 *
 * This harness cannot drive netplay.c's process_session() to a live
 * GekkoPlayerDisconnected/GekkoDesyncDetected event — that needs two real
 * UDP peers carried past the MIST handshake into a running GekkoNet
 * session, and process_session() itself has no test seam. What IS
 * testable, and what this pins, is the exact mechanism the fix threads the
 * two new codes through — the same DirectP2P_NotifySessionFailed /
 * direct_p2p_on_teardown latch the connect-phase failures (test 9) already
 * use, called with the same arguments netplay.c's two handlers now pass:
 *   (1) each new taxonomy code reaches direct_p2p_on_teardown and parks
 *       DIRECT_P2P_FAILED_HANDSHAKE — a reason-bearing terminal state —
 *       never DIRECT_P2P_IDLE (the reason-less silent drop #144 reports);
 *   (2) the two reasons render DISTINCT overlay status text, so "opponent
 *       left" and "we desynced" are told apart on screen, which is the
 *       whole ask in #144's second paragraph.
 */
static int test_midsession_failure_taxonomy(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 42: mid-session failure taxonomy "
            "(task #144)\n");
    const int fails_before = fail_count;
    char disc_text[128] = { 0 };
    char desync_text[128] = { 0 };

    NET_Init();
    DirectP2P_Init();

    /* Start from a proven-clean IDLE: run the REAL teardown callback with
     * nothing latched first, exactly like test 9's own setup, so the
     * "reason-less teardown reaches IDLE" baseline this test's assertions
     * depend on is demonstrated by the same machinery, not assumed. */
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) {
        FAIL("test42-baseline", "teardown hook did not reset the orchestrator to IDLE");
        goto done;
    }

    /* --- (a) GekkoPlayerDisconnected's exact call ---------------------- */
    DirectP2P_NotifySessionFailed(CONNECT_FAIL_PEER_DISCONNECTED, NULL);
    DirectP2P_TestHook_RunTeardown();
    EXPECT_TRUE("test42-disc-parks-failed-handshake",
                DirectP2P_GetState() == DIRECT_P2P_FAILED_HANDSHAKE);
    SDL_strlcpy(disc_text, DirectP2P_GetStatusText(), sizeof(disc_text));
    EXPECT_TRUE("test42-disc-text-matches-code",
                strcmp(disc_text, ConnectFail_UserText(CONNECT_FAIL_PEER_DISCONNECTED)) == 0);

    /* Reset to IDLE between cases the way the network menu's exit does. */
    DirectP2P_Cancel();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) {
        FAIL("test42-cancel-resets", "DirectP2P_Cancel did not return to IDLE between cases");
        goto done;
    }

    /* --- (b) GekkoDesyncDetected's exact call --------------------------- */
    DirectP2P_NotifySessionFailed(CONNECT_FAIL_DESYNC_DETECTED, NULL);
    DirectP2P_TestHook_RunTeardown();
    EXPECT_TRUE("test42-desync-parks-failed-handshake",
                DirectP2P_GetState() == DIRECT_P2P_FAILED_HANDSHAKE);
    SDL_strlcpy(desync_text, DirectP2P_GetStatusText(), sizeof(desync_text));
    EXPECT_TRUE("test42-desync-text-matches-code",
                strcmp(desync_text, ConnectFail_UserText(CONNECT_FAIL_DESYNC_DETECTED)) == 0);

    /* --- (c) the two reasons are told apart on screen ------------------- */
    EXPECT_TRUE("test42-reasons-distinct-text", strcmp(disc_text, desync_text) != 0);

    DirectP2P_Cancel(); /* back to IDLE for whatever test runs next */

done:
    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 42 OK — GekkoPlayerDisconnected/"
                "GekkoDesyncDetected's new taxonomy codes each park "
                "FAILED_HANDSHAKE (never IDLE) with distinct overlay text: "
                "\"%s\" / \"%s\"\n", disc_text, desync_text);
    }
    return (fail_count == fails_before) ? 0 : 1;
}

/* --- Test 43: RUNNING-phase event->code mapping (task #144 review Item A) */
/*
 * Step-2 review's Item A: test 42 pins the latch/park/text MECHANISM inside
 * direct_p2p.c, but nothing pinned that netplay.c's process_session()
 * actually calls DirectP2P_NotifySessionFailed for GekkoPlayerDisconnected
 * and GekkoDesyncDetected — deleting both call sites would leave test 42
 * green, since test 42 drives the mechanism directly rather than through
 * process_session(). process_session() itself still has no test seam (a
 * live GekkoPlayerDisconnected/GekkoDesyncDetected needs two real UDP
 * peers carried past the MIST handshake into a running GekkoNet session),
 * so the call WIRING remains unpinned — that residual is recorded in the
 * task's closeout, not silently dropped.
 *
 * What this test pins is the narrower, genuinely testable claim: the pure
 * event->code MAPPING netplay.c extracted into session_fail_code_for_event
 * (exposed here as Netplay_TestHook_SessionFailCodeForEvent) maps
 * GekkoPlayerDisconnected to CONNECT_FAIL_PEER_DISCONNECTED and
 * GekkoDesyncDetected to CONNECT_FAIL_DESYNC_DETECTED — so "the mapping is
 * pinned; only the call wiring at the two process_session() sites is not."
 * A future edit that swapped the two codes, or that pointed both events at
 * the same code, fails this test even though it cannot fail test 42.
 */
static int test_session_fail_code_mapping(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 43: RUNNING-phase event->code "
            "mapping (task #144 review Item A)\n");
    const int fails_before = fail_count;

    EXPECT_TRUE("test43-disconnect-maps-to-peer-disconnected",
                Netplay_TestHook_SessionFailCodeForEvent(GekkoPlayerDisconnected)
                    == CONNECT_FAIL_PEER_DISCONNECTED);
    EXPECT_TRUE("test43-desync-maps-to-desync-detected",
                Netplay_TestHook_SessionFailCodeForEvent(GekkoDesyncDetected)
                    == CONNECT_FAIL_DESYNC_DETECTED);
    /* The two RUNNING-phase codes must not collapse onto each other — a
     * regression that makes both events map to the same code would slip
     * past the first two assertions in the (unlikely) case both are wrong
     * in the same direction, but not past this one. */
    EXPECT_TRUE("test43-disconnect-and-desync-codes-distinct",
                Netplay_TestHook_SessionFailCodeForEvent(GekkoPlayerDisconnected)
                    != Netplay_TestHook_SessionFailCodeForEvent(GekkoDesyncDetected));
    /* An event outside the taxonomy (e.g. a clean connect) must not map to
     * either failure code — the helper's default case is CONNECT_FAIL_NONE,
     * not a silent fallthrough onto one of the two new codes. */
    EXPECT_TRUE("test43-unrelated-event-maps-to-none",
                Netplay_TestHook_SessionFailCodeForEvent(GekkoPlayerConnected)
                    == CONNECT_FAIL_NONE);

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 43 OK — "
                "session_fail_code_for_event maps GekkoPlayerDisconnected -> "
                "PEER_DISCONNECTED, GekkoDesyncDetected -> DESYNC_DETECTED, "
                "distinctly, and leaves unrelated events at NONE\n");
    }
    return (fail_count == fails_before) ? 0 : 1;
}

/* --- Test 44: same-batch overwrite is first-wins (task #144 review Item B) */
/*
 * Step-2 review's Item B: a single gekko_session_events() batch can carry
 * both a disconnect and a desync event. handle_disconnection()'s EXITING
 * guard stops the second TEARDOWN, but DirectP2P_NotifySessionFailed used
 * to run unconditionally before that guard on EVERY event, so a
 * disconnect-then-desync batch would silently overwrite the disconnect's
 * status text with the desync's — misattributing the overlay to whichever
 * event happened to sort last. Both reasons are truthful about a failed
 * session, so this was cosmetic misattribution, not a correctness bug, but
 * it is fixed anyway: DirectP2P_NotifySessionFailed is now first-wins for a
 * latched session (direct_p2p.c).
 *
 * This test calls it twice back-to-back with NO intervening teardown or
 * Cancel — reproducing the exact same-batch shape process_session()'s
 * for-loop produces when both events land in one poll — and pins that the
 * FIRST reason's text survives the SECOND call untouched.
 */
static int test_notify_session_failed_first_wins(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 44: same-batch notify is "
            "first-wins (task #144 review Item B)\n");
    const int fails_before = fail_count;
    char text_after_first[128] = { 0 };
    char text_after_second[128] = { 0 };

    NET_Init();
    DirectP2P_Init();

    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) {
        FAIL("test44-baseline", "teardown hook did not reset the orchestrator to IDLE");
        goto done;
    }

    /* First event of the batch: disconnect. */
    DirectP2P_NotifySessionFailed(CONNECT_FAIL_PEER_DISCONNECTED, NULL);
    SDL_strlcpy(text_after_first, DirectP2P_GetStatusText(), sizeof(text_after_first));
    EXPECT_TRUE("test44-first-call-sets-disconnect-text",
                strcmp(text_after_first, ConnectFail_UserText(CONNECT_FAIL_PEER_DISCONNECTED)) == 0);

    /* Second event of the SAME batch: desync. No teardown/Cancel between —
     * this is exactly the shape of two events landing in one
     * gekko_session_events() poll before handle_disconnection()'s EXITING
     * guard can stop anything. */
    DirectP2P_NotifySessionFailed(CONNECT_FAIL_DESYNC_DETECTED, NULL);
    SDL_strlcpy(text_after_second, DirectP2P_GetStatusText(), sizeof(text_after_second));
    EXPECT_TRUE("test44-second-call-does-not-overwrite",
                strcmp(text_after_second, text_after_first) == 0);
    EXPECT_TRUE("test44-first-reason-still-disconnect-not-desync",
                strcmp(text_after_second, ConnectFail_UserText(CONNECT_FAIL_DESYNC_DETECTED)) != 0);

    /* Teardown still parks FAILED_HANDSHAKE with the FIRST reason. */
    DirectP2P_TestHook_RunTeardown();
    EXPECT_TRUE("test44-teardown-parks-failed-handshake",
                DirectP2P_GetState() == DIRECT_P2P_FAILED_HANDSHAKE);
    EXPECT_TRUE("test44-teardown-text-is-first-reason",
                strcmp(DirectP2P_GetStatusText(), ConnectFail_UserText(CONNECT_FAIL_PEER_DISCONNECTED)) == 0);

    DirectP2P_Cancel(); /* back to IDLE for whatever test runs next */

done:
    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 44 OK — a second same-batch "
                "DirectP2P_NotifySessionFailed call is dropped; the first "
                "reason (\"%s\") survives to teardown\n", text_after_first);
    }
    return (fail_count == fails_before) ? 0 : 1;
}

/* --- Test 45: #147 portmap straggler slot ------------------------------ */

/* The slot/park/reap/verdict logic behind the #147 spawn gate:
 * portmap_backends_quiescent() must be false exactly while a parked
 * overrun worker still runs, must refuse a second occupant, and must
 * reap (join + free) a finished straggler on the next query. The REAL
 * overrun — a worker stuck inside miniupnpc past its budget — cannot be
 * produced in-tree (no mock IGD; harness builds refuse SSDP by design),
 * so the simulated straggler occupies the real slot via the same
 * portmap_straggler_park the production detach sites now call. The three
 * call-site gates (try_portmap, join_portmap_spawn, host_thread_fn) are
 * one predicate call each and are covered by inspection, not here. */
static int test_portmap_straggler_slot(void) {
    fprintf(stderr, "[test_bilateral_punch] test 45: #147 portmap straggler "
                    "slot (park -> non-quiescent -> reap)\n");
    const int fails_before = fail_count;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown(); /* whatever ran before -> IDLE */

    /* Idle baseline: no straggler, backends quiescent. */
    EXPECT_TRUE("45-quiescent-at-idle", DirectP2P_TestHook_PortmapQuiescent());

    /* Park a running straggler: the spawn-gate predicate must go false
     * and STAY false while it runs. */
    if (!DirectP2P_TestHook_PortmapStragglerSim()) {
        FAIL("45-sim", "could not occupy the straggler slot");
        return 1;
    }
    for (int i = 0; i < 5; i++) {
        EXPECT_FALSE("45-nonquiescent-while-running",
                     DirectP2P_TestHook_PortmapQuiescent());
        SDL_Delay(10);
    }

    /* The slot holds ONE straggler; a second occupant is refused (the
     * production park site logs BUG and detaches — reachable only if a
     * spawn gate were bypassed). */
    EXPECT_FALSE("45-second-occupant-refused",
                 DirectP2P_TestHook_PortmapStragglerSim());

    /* Release it: the predicate must flip to quiescent (reaping the
     * thread for free) within a bounded wait, and stay there. */
    DirectP2P_TestHook_PortmapStragglerRelease();
    {
        const uint32_t start = SDL_GetTicks();
        bool reaped = false;
        while ((int)(SDL_GetTicks() - start) < 2000) {
            if (DirectP2P_TestHook_PortmapQuiescent()) { reaped = true; break; }
            SDL_Delay(10);
        }
        EXPECT_TRUE("45-reaped-after-release", reaped);
    }
    EXPECT_TRUE("45-quiescent-stays", DirectP2P_TestHook_PortmapQuiescent());
    /* And the slot is free for a future overrun again. */
    EXPECT_TRUE("45-slot-reusable", DirectP2P_TestHook_PortmapStragglerSim());
    DirectP2P_TestHook_PortmapStragglerRelease();
    {
        const uint32_t start = SDL_GetTicks();
        while ((int)(SDL_GetTicks() - start) < 2000 &&
               !DirectP2P_TestHook_PortmapQuiescent()) {
            SDL_Delay(10);
        }
    }
    EXPECT_TRUE("45-clean-exit", DirectP2P_TestHook_PortmapQuiescent());

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 45 OK — straggler slot parks one "
                "worker, refuses a second, reports non-quiescent while it runs "
                "and reaps it on the first query after exit\n");
        return 0;
    }
    return 1;
}

/* --- Tests 46/47: #150 HOST_WAITING receive batch + punch-gate recovery  */

/* Send `len` raw bytes to 127.0.0.1:port from `sock`. */
static bool send_raw_to_host(int sock, uint16_t port, const void* buf, int len) {
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(port);
    return sendto(sock, (const char*)buf, len, 0, (struct sockaddr*)&to,
                  sizeof(to)) == len;
}

/* Derive the authenticated 17-byte punch payload from the PUBLISHED room
 * code — the same inputs a real joiner has (no peeking at statics). */
static bool host_code_punch_payload(const char* code,
                                    uint8_t out[STUN_PUNCH_PAYLOAD_LEN]) {
    uint32_t ip_be = 0, nonce = 0;
    uint16_t adv_port = 0;
    uint8_t token[STUN_PUNCH_TOKEN_LEN];
    if (RoomCode_Decode(code, &ip_be, &adv_port, &nonce) != ROOM_CODE_OK)
        return false;
    if (!Rendezvous_DerivePunchToken(ip_be, adv_port, nonce, token))
        return false;
    Stun_BuildPunchPayload(token, out);
    return true;
}

/* Test 46: the #150 bounded batch. Pre-#150, host_tick_receive drained
 * ONE datagram per frame, so ten garbage datagrams queued ahead of the
 * legitimate joiner's punch cost ten frames before the punch was even
 * looked at — a flood at the advertised endpoint starved the joiner
 * behind the 256 KB receive buffer. Now one Tick (two allowed below, for
 * SDL_net's receive-pump slack) must drain past the garbage, accept the
 * punch, echo it and hand off. */
static int test_host_batch_drain(void) {
    fprintf(stderr, "[test_bilateral_punch] test 46: #150 HOST_WAITING batch "
                    "drain (garbage ahead of the joiner's punch)\n");
    const int fails_before = fail_count;
    int rc = 0;
    int client = -1;
    unsigned short client_port = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    /* No rendezvous mock: an empty signal URL keeps the worker unspawned
     * (it logs its own cause) — this test is about the receive path. */
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, "");
    s_mock_discover_calls = 0;
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_ResetHandoff();
    /* A unit test must not mutate the LAN (see test 13's rationale). */
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    DirectP2P_BeginHost(0);
    if (!wait_for_state(DIRECT_P2P_HOST_WAITING, 25000)) {
        FAIL("46-host-waiting", "host never reached HOST_WAITING");
        rc = 1;
        goto done;
    }

    uint8_t punch[STUN_PUNCH_PAYLOAD_LEN];
    if (!host_code_punch_payload(DirectP2P_GetHostCode(), punch)) {
        FAIL("46-derive", "could not derive the punch payload from the host code");
        rc = 1;
        goto done;
    }
    const uint16_t host_port = s_mock_discover_ports[0];
    if (host_port == 0) {
        FAIL("46-host-port", "mock STUN discover recorded no bound port");
        rc = 1;
        goto done;
    }

    client = open_udp_on_localhost(&client_port);
    if (client < 0) {
        FAIL("46-client", "failed to bind the joiner-side socket");
        rc = 1;
        goto done;
    }

    /* Ten garbage datagrams FIRST (not punch-shaped: uncharged noise),
     * then the authenticated punch behind them — same source, FIFO. */
    {
        uint8_t garbage[12];
        memset(garbage, 0xAB, sizeof(garbage));
        for (int i = 0; i < 10; i++) {
            if (!send_raw_to_host(client, host_port, garbage, (int)sizeof(garbage))) {
                FAIL("46-send-garbage", "sendto failed");
                rc = 1;
                goto done;
            }
        }
        if (!send_raw_to_host(client, host_port, punch, STUN_PUNCH_PAYLOAD_LEN)) {
            FAIL("46-send-punch", "sendto failed");
            rc = 1;
            goto done;
        }
    }
    SDL_Delay(150); /* let the 11 datagrams reach SDL_net's receive queue */

    /* THE PIN: at most two Ticks. Eleven datagrams fit one
     * HOST_TICK_RECV_BATCH (16); the second Tick only covers SDL_net
     * pumping the tail in late. One-per-frame draining needs eleven. */
    DirectP2P_Tick();
    if (DirectP2P_GetState() != DIRECT_P2P_HANDOFF) {
        SDL_Delay(50);
        DirectP2P_Tick();
    }
    if (DirectP2P_GetState() != DIRECT_P2P_HANDOFF) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 46-batch: state %d after two Ticks "
                "with 10 garbage + 1 punch queued — the punch behind the garbage "
                "was not reached (one-datagram-per-frame drain?)\n",
                (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
        goto done;
    }
    {
        char hip[64] = { 0 };
        uint16_t hport = 0;
        int hplayer = 0, hcount = 0;
        DirectP2P_TestHook_LastHandoff(hip, (int)sizeof(hip), &hport, &hplayer,
                                       &hcount);
        EXPECT_TRUE("46-handoff-endpoint",
                    hport == client_port && strcmp(hip, "127.0.0.1") == 0);
        EXPECT_TRUE("46-handoff-player1", hplayer == 1);
        EXPECT_TRUE("46-one-handoff", hcount == 1);
    }
    /* The legitimate joiner RECOVERS: the byte-identical authenticated
     * echo must come back to it (that is what its Stun_HolePunch loop
     * requires to return success). */
    {
        bool echoed = false;
        const uint32_t start = SDL_GetTicks();
        while ((int)(SDL_GetTicks() - start) < 2000 && !echoed) {
            fd_set rfds;
            struct timeval tv = { 0, 100 * 1000 };
            FD_ZERO(&rfds);
            FD_SET(client, &rfds);
            if (select(client + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
            uint8_t rbuf[64];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            const int n = (int)recvfrom(client, (char*)rbuf, sizeof(rbuf), 0,
                                        (struct sockaddr*)&from, &fl);
            if (n == STUN_PUNCH_PAYLOAD_LEN &&
                memcmp(rbuf, punch, STUN_PUNCH_PAYLOAD_LEN) == 0) {
                echoed = true;
            }
        }
        EXPECT_TRUE("46-punch-echoed", echoed);
    }

done:
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    if (client >= 0) close_sock(client);

    if (rc == 0 && fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 46 OK — one frame drained past 10 "
                "garbage datagrams, accepted the authenticated punch behind them "
                "and echoed it to the joiner\n");
        return 0;
    }
    return 1;
}

/* Test 47: the punch-gate mute -> re-roll -> legitimate-joiner-recovers
 * chain, end to end over the real socket. Previously only the gate's
 * accounting was unit-tested (PunchGateNoteBad etc.); the receive-path
 * consequences — a muted source's AUTHENTICATED punch being suppressed,
 * and the re-rolled code accepting a fresh punch — were not. */
static int test_punch_gate_reroll_recovery(void) {
    fprintf(stderr, "[test_bilateral_punch] test 47: #150 punch-gate mute -> "
                    "re-roll -> recovery over the wire\n");
    const int fails_before = fail_count;
    int rc = 0;
    int client = -1;
    unsigned short client_port = 0;
    char code1[ROOM_CODE_BUF_LEN] = { 0 };

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, "");
    s_mock_discover_calls = 0;
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_ResetHandoff();
    /* Same LAN-safety rationale as test 13. */
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    DirectP2P_BeginHost(0);
    if (!wait_for_state(DIRECT_P2P_HOST_WAITING, 25000)) {
        FAIL("47-host-waiting", "host never reached HOST_WAITING");
        rc = 1;
        goto done;
    }
    SDL_strlcpy(code1, DirectP2P_GetHostCode(), sizeof(code1));

    uint8_t punch1[STUN_PUNCH_PAYLOAD_LEN];
    if (!host_code_punch_payload(code1, punch1)) {
        FAIL("47-derive", "could not derive the punch payload from the host code");
        rc = 1;
        goto done;
    }
    uint8_t bad_punch[STUN_PUNCH_PAYLOAD_LEN];
    memcpy(bad_punch, punch1, sizeof(bad_punch));
    bad_punch[STUN_PUNCH_PAYLOAD_LEN - 1] ^= 0xFF; /* wrong token, right shape */

    const uint16_t host_port = s_mock_discover_ports[0];
    client = open_udp_on_localhost(&client_port);
    if (host_port == 0 || client < 0) {
        FAIL("47-setup", "no host port / no client socket");
        rc = 1;
        goto done;
    }

    /* 30 bad-token punches (> HOST_PUNCH_SRC_MAX_BAD = 24) followed by a
     * VALID one — FIFO from one source, so the mute engages before the
     * valid punch is reached: it must be SUPPRESSED, not handed off. */
    for (int i = 0; i < 30; i++) {
        (void)send_raw_to_host(client, host_port, bad_punch, STUN_PUNCH_PAYLOAD_LEN);
    }
    (void)send_raw_to_host(client, host_port, punch1, STUN_PUNCH_PAYLOAD_LEN);
    SDL_Delay(150);
    for (int i = 0; i < 12; i++) {
        DirectP2P_Tick();
        SDL_Delay(10);
    }
    if (DirectP2P_GetState() != DIRECT_P2P_HOST_WAITING) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: 47-mute: state %d — a valid punch "
                "from a source that just ground 30 bad tokens was ANSWERED; the "
                "mute did not suppress it\n",
                (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
        goto done;
    }

    /* 40 more bad punches push the session total past
     * HOST_PUNCH_TOTAL_REROLL (64): the host must re-roll the room code
     * (the displayed code CHANGES — the user-visible recovery). */
    for (int i = 0; i < 40; i++) {
        (void)send_raw_to_host(client, host_port, bad_punch, STUN_PUNCH_PAYLOAD_LEN);
    }
    SDL_Delay(150);
    {
        bool rerolled = false;
        const uint32_t start = SDL_GetTicks();
        while ((int)(SDL_GetTicks() - start) < 5000) {
            DirectP2P_Tick();
            if (DirectP2P_GetState() != DIRECT_P2P_HOST_WAITING) break;
            if (strcmp(DirectP2P_GetHostCode(), code1) != 0) { rerolled = true; break; }
            SDL_Delay(10);
        }
        if (!rerolled) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 47-reroll: the room code never "
                    "changed after 70 bad-token punches (state %d)\n",
                    (int)DirectP2P_GetState());
            fail_count++;
            rc = 1;
            goto done;
        }
    }

    /* The re-roll cleared every mute and re-derived the token: a joiner
     * holding the NEW code — same source IP that was muted — recovers. */
    {
        uint8_t punch2[STUN_PUNCH_PAYLOAD_LEN];
        if (!host_code_punch_payload(DirectP2P_GetHostCode(), punch2)) {
            FAIL("47-derive2", "could not derive a payload from the re-rolled code");
            rc = 1;
            goto done;
        }
        if (!send_raw_to_host(client, host_port, punch2, STUN_PUNCH_PAYLOAD_LEN)) {
            FAIL("47-send2", "sendto failed");
            rc = 1;
            goto done;
        }
        if (!tick_until_state(DIRECT_P2P_HANDOFF, 5000)) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 47-recover: state %d — the punch "
                    "derived from the RE-ROLLED code was not accepted from the "
                    "formerly muted source\n",
                    (int)DirectP2P_GetState());
            fail_count++;
            rc = 1;
            goto done;
        }
        char hip[64] = { 0 };
        uint16_t hport = 0;
        int hplayer = 0, hcount = 0;
        DirectP2P_TestHook_LastHandoff(hip, (int)sizeof(hip), &hport, &hplayer,
                                       &hcount);
        EXPECT_TRUE("47-handoff-endpoint",
                    hport == client_port && strcmp(hip, "127.0.0.1") == 0);
        EXPECT_TRUE("47-one-handoff", hcount == 1);
    }

done:
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    if (client >= 0) close_sock(client);

    if (rc == 0 && fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 47 OK — mute suppressed the ground "
                "source's valid punch, 70 bad tokens re-rolled the code, and the "
                "new code's punch paired from the same source\n");
        return 0;
    }
    return 1;
}

/* --- Entry point ------------------------------------------------------ */

int Netplay_Test_BilateralPunch(void) {
    fail_count = 0;

    int rc = 0;
    rc |= test_register_deliver();
    rc |= test_protocol_round_trip();
    rc |= test_kill_switch_round_trip();
    rc |= test_joiner_self_deliver();
    rc |= test_joiner_fresh_socket_retry();
    rc |= test_posthandoff_failure_report();
    rc |= test_handoff_player_number_sites(); /* 33: do_handoff player numbers */
    rc |= test_host_cookie_handshake();
    rc |= test_joiner_cookie_handshake();
    rc |= test_race_deliver_overlaps_seed();  /* S6 */
    rc |= test_race_worst_case_timing();      /* S6: ~16 s of wall clock */
    /* S6 adversarial review: real-wire punch legs, two-peer split brain. */
    rc |= test_race_duplicate_candidate_guard();   /* M-2: the guard (see H-C) */
    rc |= test_race_failed_rearm_keeps_live_candidate(); /* H-C: M-2 half i  */
    rc |= test_race_rearm_releases_address_ref();        /* H-C: M-2 half ii */
    rc |= test_race_rearm_over_confirmed_leg();    /* #120: test 41 */
    rc |= test_race_confirm_at_budget_edge();      /* H-1 / H-2 */
    rc |= test_race_two_peer_convergence();        /* 34: two peers, concurrently */
    rc |= test_natpmp_pcp();  /* S7: test 22 */
    rc |= test_s7_disable_pairing();    /* S7 review: test 23a */
    rc |= test_upnp_harness_no_discovery(); /* M-1: test 23e */
    rc |= test_s7_review_fixes();       /* S7 review: test 23b */
    rc |= test_s7_natpmp_kill_switch(); /* S7 review: test 23c */
    rc |= test_s7_lost_mapping();       /* S7 review: test 23d */
    rc |= test_nav_orch_deadline_is_derived(); /* task #76: test 40 */
    rc |= test_midsession_failure_taxonomy(); /* task #144: test 42 */
    rc |= test_session_fail_code_mapping(); /* task #144 review Item A: test 43 */
    rc |= test_notify_session_failed_first_wins(); /* task #144 review Item B: test 44 */
    rc |= test_portmap_straggler_slot();      /* task #147: test 45 */
    rc |= test_host_batch_drain();            /* task #150: test 46 */
    rc |= test_punch_gate_reroll_recovery();  /* task #150: test 47 */
    rc |= test_host_cookie_rejected(); /* last: ~31 s of wall clock */

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
