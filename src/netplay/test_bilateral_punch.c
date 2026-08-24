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
#define REND_VERSION       2  /* S4c: protocol v2 */
#define REND_TYPE_REGISTER 1
#define REND_TYPE_DELIVER  2
#define REND_TYPE_POLL     3
#define REND_TYPE_CHALLENGE 4  /* S4c: server -> client cookie challenge */
/* S5 relay types (docs/plan-netplay-connection.md §7). */
#define REND_TYPE_RELAY_REQ     5
#define REND_TYPE_RELAY_GRANT   6
#define REND_TYPE_RELAY_PIN     7
#define REND_TYPE_RELAY_PIN_ACK 8

#define REND_REGISTER_LEN  36  /* S4c: 28 + 8-byte cookie tail */
#define REND_DELIVER_LEN   32
#define REND_CHALLENGE_LEN 32
#define REND_KEY_LEN       16
#define REND_GRANT_LEN     36
#define REND_PIN_LEN       20
#define REND_PIN_ACK_LEN   12
#define REND_TOKEN_LEN     8

/* S5 relay behaviour of the mock server. OFF is 0 so a memset-zeroed
 * MockServerCtx (every pre-S5 test) keeps its old behaviour exactly. */
typedef enum {
    MOCK_RELAY_OFF = 0,
    MOCK_RELAY_OK,      /* grant a port, ack the pin                       */
    MOCK_RELAY_SILENT,  /* ignore RELAY_REQ entirely -> RELAY_UNAVAILABLE  */
    MOCK_RELAY_REFUSE,  /* grant frame carrying POOL_EXHAUSTED -> REFUSED  */
    MOCK_RELAY_NO_ACK,  /* grant a port that never answers -> PIN_TIMEOUT  */
} MockRelayMode;

#define MOCK_RELAY_STATUS_GRANTED        0
#define MOCK_RELAY_STATUS_POOL_EXHAUSTED 1

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

    /* --- S5 relay (test 16) -------------------------------------------
     * All gated on relay_mode != MOCK_RELAY_OFF, which is the zero value,
     * so every pre-S5 test's memset-zeroed ctx behaves exactly as before
     * and `relay_sock == 0` is never mistaken for a real descriptor. */
    volatile int    relay_mode;     /* MockRelayMode                      */
    int             relay_sock;     /* second UDP socket = the relay port */
    uint16_t        relay_port;
    uint8_t         relay_token[REND_TOKEN_LEN]; /* what the grant carries */
    uint8_t         relay_slot;     /* slot the grant assigns (1 = joiner) */
    volatile int    relay_reqs;
    volatile int    relay_grants;
    volatile int    relay_pins_ok;   /* pins carrying the granted token    */
    volatile int    relay_pins_bad;  /* pins carrying anything else        */
    volatile int    relay_acks;
    volatile int    relay_pin_slot;  /* slot byte of the last accepted pin */
} MockServerCtx;

/* magic(4) ver(1) type(1)=6 slot(1) status(1) key(16) port_be(2)
 * reserved(2) token(8) — see rendezvous.h. */
static int build_relay_grant(uint8_t out[REND_GRANT_LEN],
                             const uint8_t key[REND_KEY_LEN],
                             uint8_t slot, uint8_t status,
                             uint16_t relay_port,
                             const uint8_t token[REND_TOKEN_LEN]) {
    memset(out, 0, REND_GRANT_LEN);
    out[0] = REND_MAGIC_BYTES_0;
    out[1] = REND_MAGIC_BYTES_1;
    out[2] = REND_MAGIC_BYTES_2;
    out[3] = REND_MAGIC_BYTES_3;
    out[4] = (uint8_t)REND_VERSION;
    out[5] = (uint8_t)REND_TYPE_RELAY_GRANT;
    out[6] = slot;
    out[7] = status;
    memcpy(&out[8], key, REND_KEY_LEN);
    if (status == MOCK_RELAY_STATUS_GRANTED) {
        out[24] = (uint8_t)((relay_port >> 8) & 0xFFu);
        out[25] = (uint8_t)(relay_port & 0xFFu);
        memcpy(&out[28], token, REND_TOKEN_LEN);
    }
    return REND_GRANT_LEN;
}

static int build_relay_pin_ack(uint8_t out[REND_PIN_ACK_LEN],
                               uint8_t slot, bool peer_pinned) {
    memset(out, 0, REND_PIN_ACK_LEN);
    out[0] = REND_MAGIC_BYTES_0;
    out[1] = REND_MAGIC_BYTES_1;
    out[2] = REND_MAGIC_BYTES_2;
    out[3] = REND_MAGIC_BYTES_3;
    out[4] = (uint8_t)REND_VERSION;
    out[5] = (uint8_t)REND_TYPE_RELAY_PIN_ACK;
    out[6] = slot;
    out[7] = peer_pinned ? 1u : 0u;
    return REND_PIN_ACK_LEN;
}

/* Service one datagram on the mock's relay port: validate the pin's
 * token and answer (or, in MOCK_RELAY_NO_ACK, deliberately do not). */
static void mock_relay_tick(MockServerCtx* ctx) {
    uint8_t rb[128];
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    const int n = (int)recvfrom(ctx->relay_sock, (char*)rb, sizeof(rb), 0,
                                (struct sockaddr*)&src, &sl);
    if (n < REND_PIN_LEN) return;
    if (rb[0] != REND_MAGIC_BYTES_0 || rb[1] != REND_MAGIC_BYTES_1 ||
        rb[2] != REND_MAGIC_BYTES_2 || rb[3] != REND_MAGIC_BYTES_3 ||
        rb[4] != REND_VERSION || rb[5] != REND_TYPE_RELAY_PIN) {
        return;
    }
    if (memcmp(&rb[8], ctx->relay_token, REND_TOKEN_LEN) != 0) {
        ctx->relay_pins_bad++;
        return;
    }
    ctx->relay_pins_ok++;
    ctx->relay_pin_slot = rb[6];
    if (ctx->relay_mode == MOCK_RELAY_NO_ACK) return;
    uint8_t ack[REND_PIN_ACK_LEN];
    const int al = build_relay_pin_ack(ack, rb[6], /*peer_pinned*/ true);
    ctx->relay_acks++;
    sendto(ctx->relay_sock, (const char*)ack, al, 0, (struct sockaddr*)&src, sl);
}

/* Cookie construction for the mock. The real server uses
 * SHA-256(secret ‖ "addr:port:slot")[0..7] (rendezvous-server.js:191);
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
    const long long start = (long long)time(NULL);
    const long long life = (ctx->life_secs > 0) ? (long long)ctx->life_secs : 5;

    for (;;) {
        if (ctx->stop) return 0;
        if ((long long)time(NULL) - start > life) return 0;

        /* S5: watch the relay port too when one is armed. Gated on
         * relay_mode (zero for every pre-S5 test) so a zeroed
         * `relay_sock` is never added to the fd set. */
        const bool relay_active =
            ctx->relay_mode != MOCK_RELAY_OFF && ctx->relay_sock > 0;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->sock, &rfds);
        int maxfd = ctx->sock;
        if (relay_active) {
            FD_SET(ctx->relay_sock, &rfds);
            if (ctx->relay_sock > maxfd) maxfd = ctx->relay_sock;
        }
        struct timeval tv = { 0, 50 * 1000 };
        const int rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) continue;

        if (relay_active && FD_ISSET(ctx->relay_sock, &rfds)) {
            mock_relay_tick(ctx);
        }
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

        /* S5: RELAY_REQ (type 5) is a 36-byte frame shaped exactly like a
         * REGISTER; answer it per the configured relay_mode. Handled here
         * — before the REGISTER/POLL filter — because it is neither. */
        if (type == REND_TYPE_RELAY_REQ) {
            if (ctx->relay_mode == MOCK_RELAY_OFF) continue;
            ctx->relay_reqs++;
            if (ctx->relay_mode == MOCK_RELAY_SILENT) continue;
            const bool refuse = (ctx->relay_mode == MOCK_RELAY_REFUSE);
            uint8_t g[REND_GRANT_LEN];
            const int gl = build_relay_grant(
                g, req_key,
                refuse ? 0xFFu : ctx->relay_slot,
                refuse ? MOCK_RELAY_STATUS_POOL_EXHAUSTED : MOCK_RELAY_STATUS_GRANTED,
                ctx->relay_port, ctx->relay_token);
            ctx->relay_grants++;
            sendto(ctx->sock, (const char*)g, gl, 0, (struct sockaddr*)&src, sl);
            continue;
        }

        if (type != REND_TYPE_REGISTER && type != REND_TYPE_POLL) continue;

        /* S4c return-routability gate. Runs between "the frame is
         * well-formed" and "we touch any state" — exactly where
         * returnRoutabilityGate sits in rendezvous-server.js:553 — so an
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
            if (ctx->cookied_requests >= ctx->min_cookied_before_peer) {
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

static int test_session_key_stability(void) {
    fprintf(stderr, "[test_bilateral_punch] test 2: session-key stability\n");

    uint8_t k1[REND_KEY_LEN];
    uint8_t k2[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0x222, k1));
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0x222, k2));
    if (memcmp(k1, k2, REND_KEY_LEN) != 0) {
        FAIL("test2", "deterministic derivation produced different keys");
        return 1;
    }

    /* Different ip yields different key. */
    uint8_t k3[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Eu, 12345, 0x222, k3));
    if (memcmp(k1, k3, REND_KEY_LEN) == 0) {
        FAIL("test2", "different ip produced identical key (collision)");
        return 1;
    }

    /* S4b: different NONCE (same ip:port) yields a different key —
     * this is the whole point of the nonce: (ip, port) alone no longer
     * determines the rendezvous slot. */
    uint8_t k5[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0x223, k5));
    if (memcmp(k1, k5, REND_KEY_LEN) == 0) {
        FAIL("test2", "different nonce produced identical session key");
        return 1;
    }
    /* v3: the nonce is a full 32 bits — 0x1000 is now a PERFECTLY
     * VALID nonce (it was out of range under v2's 12-bit mask), and a
     * high-bit nonce must reach the hash rather than being masked off.
     * Regression net for the widening: if any layer silently truncated
     * the nonce back to 12 or 16 bits these two would collide. */
    uint8_t k6[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0x1000u, k6));
    if (memcmp(k1, k6, REND_KEY_LEN) == 0) {
        FAIL("test2", "nonce 0x1000 produced the same key as 0x222");
        return 1;
    }
    uint8_t k7[REND_KEY_LEN];
    uint8_t k8[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0x00000222u, k7));
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0xDEAD0222u, k8));
    if (memcmp(k7, k8, REND_KEY_LEN) == 0) {
        FAIL("test2", "nonce high 16 bits ignored — 32-bit nonce truncated");
        return 1;
    }

    /* ip_be == 0 must return false. */
    uint8_t k4[REND_KEY_LEN];
    if (Rendezvous_DeriveSessionKey(0u, 12345, 0x222, k4)) {
        FAIL("test2", "ip_be=0 should return false");
        return 1;
    }

    /* --- S4a: punch-token derivation. Deterministic; sensitive to both
     * inputs; DOMAIN-SEPARATED from the session key (a token must never
     * equal a session-key prefix for the same payload); ip_be==0 fails
     * and zeroes the output. */
    uint8_t t1[REND_PUNCH_TOKEN_LEN];
    uint8_t t2[REND_PUNCH_TOKEN_LEN];
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, 0x222, t1));
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, 0x222, t2));
    if (memcmp(t1, t2, REND_PUNCH_TOKEN_LEN) != 0) {
        FAIL("test2-token", "deterministic token derivation produced different tokens");
        return 1;
    }
    uint8_t t3[REND_PUNCH_TOKEN_LEN];
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12346, 0x222, t3));
    if (memcmp(t1, t3, REND_PUNCH_TOKEN_LEN) == 0) {
        FAIL("test2-token", "different port produced identical token");
        return 1;
    }
    /* S4b: different nonce -> different token. */
    uint8_t t5[REND_PUNCH_TOKEN_LEN];
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, 0x223, t5));
    if (memcmp(t1, t5, REND_PUNCH_TOKEN_LEN) == 0) {
        FAIL("test2-token", "different nonce produced identical token");
        return 1;
    }
    /* v3: the token must see all 32 nonce bits too — the punch gate is
     * the thing the widening exists for (room_code.h). */
    uint8_t t6[REND_PUNCH_TOKEN_LEN];
    uint8_t t7[REND_PUNCH_TOKEN_LEN];
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, 0x00000222u, t6));
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, 0xDEAD0222u, t7));
    if (memcmp(t6, t7, REND_PUNCH_TOKEN_LEN) == 0) {
        FAIL("test2-token", "nonce high 16 bits ignored in punch token");
        return 1;
    }
    /* Domain separation vs the session key over the SAME payload. */
    if (memcmp(t1, k1, REND_PUNCH_TOKEN_LEN) == 0) {
        FAIL("test2-token", "token equals session-key prefix — no domain separation");
        return 1;
    }
    uint8_t t4[REND_PUNCH_TOKEN_LEN];
    memset(t4, 0xEE, sizeof(t4));
    if (Rendezvous_DerivePunchToken(0u, 12345, 0x222, t4)) {
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
    if (!RoomCode_Encode((uint32_t)lan_peer.s_addr, 5555, 0x0AA, code)) {
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

    /* --- 7c2 (S4c): CHALLENGE evidence splits "server down" from
     * "our cookie echo never bound". Pre-S4c both looked identical
     * (zero DELIVERs) and every auth/version problem was misreported as
     * RENDEZVOUS_DOWN, sending users to check their internet. */
    memset(&ev, 0, sizeof(ev));
    EXPECT_TRUE("7c2-silence-is-down",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_RENDEZVOUS_DOWN);
    ev.challenge_any = true;
    EXPECT_TRUE("7c2-challenged-but-never-bound",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_COOKIE_REJECTED);
    /* A CHALLENGE plus at least one DELIVER means the cookie DID bind —
     * the failure is downstream, so cookie blame must not stick. */
    ev.deliver_any = true;
    EXPECT_TRUE("7c2-deliver-outranks-challenge",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_HOST_OFFLINE);
    /* Hairpin still outranks the cookie diagnosis. */
    memset(&ev, 0, sizeof(ev));
    ev.challenge_any = true;
    ev.hairpin = true;
    EXPECT_TRUE("7c2-hairpin-outranks",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_HAIRPIN);
    /* COOKIE_REJECTED is its own cause, not an alias of an older one. */
    EXPECT_TRUE("7c2-distinct-code",
                strcmp(ConnectFail_Code(CONNECT_FAIL_COOKIE_REJECTED),
                       ConnectFail_Code(CONNECT_FAIL_RENDEZVOUS_DOWN)) != 0);
    EXPECT_TRUE("7c2-code-string",
                strcmp(ConnectFail_Code(CONNECT_FAIL_COOKIE_REJECTED),
                       "P2P_FAIL_COOKIE_REJECTED") == 0);

    /* --- 7d: host-waiting advisory (cause 8). */
    EXPECT_TRUE("7d-too-early",
                ConnectFail_ClassifyHostWaiting(false, false, false, CONNECT_HOST_ADVISORY_MS - 1) ==
                    CONNECT_FAIL_NONE);
    EXPECT_TRUE("7d-unmappable",
                ConnectFail_ClassifyHostWaiting(false, false, false, CONNECT_HOST_ADVISORY_MS) ==
                    CONNECT_FAIL_HOST_UNMAPPABLE);
    EXPECT_TRUE("7d-rendezvous-down-with-upnp",
                ConnectFail_ClassifyHostWaiting(true, false, false, CONNECT_HOST_ADVISORY_MS) ==
                    CONNECT_FAIL_RENDEZVOUS_DOWN);
    EXPECT_TRUE("7d-deliver-seen",
                ConnectFail_ClassifyHostWaiting(false, true, false, CONNECT_HOST_ADVISORY_MS * 2) ==
                    CONNECT_FAIL_NONE);

    /* --- 7d2 (S4c): same CHALLENGE split on the HOST advisory path.
     * A host that is being challenged but never DELIVERed must be told
     * "auth/version trouble", not "no UPnP mapping" or "server down". */
    EXPECT_TRUE("7d2-challenged-no-upnp",
                ConnectFail_ClassifyHostWaiting(false, false, true, CONNECT_HOST_ADVISORY_MS) ==
                    CONNECT_FAIL_COOKIE_REJECTED);
    EXPECT_TRUE("7d2-challenged-with-upnp",
                ConnectFail_ClassifyHostWaiting(true, false, true, CONNECT_HOST_ADVISORY_MS) ==
                    CONNECT_FAIL_COOKIE_REJECTED);
    /* A DELIVER means the cookie bound — no advisory at all, challenges
     * notwithstanding (every v2 session starts with one). */
    EXPECT_TRUE("7d2-deliver-outranks-challenge",
                ConnectFail_ClassifyHostWaiting(false, true, true, CONNECT_HOST_ADVISORY_MS * 2) ==
                    CONNECT_FAIL_NONE);
    /* Still inside the advisory window: say nothing yet. */
    EXPECT_TRUE("7d2-too-early",
                ConnectFail_ClassifyHostWaiting(false, false, true, CONNECT_HOST_ADVISORY_MS - 1) ==
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
    /* S4-review L-4: "older" and "newer" must be DISTINCT machine codes.
     * Collapsed into one, log triage could not tell which side of a
     * failed pairing was stale — the entire question a support thread
     * has to answer. The generic sweep below proves every code has a
     * distinct string; this proves these two specifically exist, differ,
     * and carry user text that points at the right person. */
    EXPECT_TRUE("test7f-l4",
                strcmp(ConnectFail_Code(CONNECT_FAIL_CODE_VERSION_OLDER),
                       ConnectFail_Code(CONNECT_FAIL_CODE_VERSION_NEWER)) != 0);
    EXPECT_TRUE("test7f-l4",
                strcmp(ConnectFail_UserText(CONNECT_FAIL_CODE_VERSION_OLDER),
                       ConnectFail_UserText(CONNECT_FAIL_CODE_VERSION_NEWER)) != 0);
    EXPECT_TRUE("test7f-l4",
                strstr(ConnectFail_UserText(CONNECT_FAIL_CODE_VERSION_OLDER),
                       "older") != NULL);
    EXPECT_TRUE("test7f-l4",
                strstr(ConnectFail_UserText(CONNECT_FAIL_CODE_VERSION_NEWER),
                       "newer") != NULL);

    for (int c = CONNECT_FAIL_NONE; c <= CONNECT_FAIL_LAST_; c++) {
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
/* S5 (test 16): the OK line now carries relay=0|1, so its TEXT matters
 * and not only its count. */
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
    DirectP2P_TestHook_SetStunHolePunch(mock_stun_punch);
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
static int test_rendezvous_cookie_codec(void) {
    fprintf(stderr, "[test_bilateral_punch] test 11: S4c rendezvous v2 cookie codec\n");
    const int fails_before = fail_count;

    uint8_t key[REND_KEY_LEN];
    uint8_t other_key[REND_KEY_LEN];
    for (int i = 0; i < REND_KEY_LEN; i++) {
        key[i] = (uint8_t)(0xA0 + i);
        other_key[i] = (uint8_t)(0x10 + i);
    }
    uint8_t cookie[REND_COOKIE_LEN] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };

    /* (a) Sizes agree with the wire spec. */
    EXPECT_TRUE("11-sizes", REND_REGISTER_PKT_LEN == 36 && REND_COOKIE_LEN == 8);
    EXPECT_TRUE("11-local-len-agrees", REND_REGISTER_LEN == REND_REGISTER_PKT_LEN);

    /* (b) NULL cookie -> zero tail, version byte 2. */
    uint8_t reg_nc[REND_REGISTER_PKT_LEN];
    memset(reg_nc, 0xFF, sizeof(reg_nc)); /* poison: catch a missing memset */
    EXPECT_TRUE("11-build-null", Rendezvous_BuildRegister(4321, key, NULL, reg_nc));
    EXPECT_TRUE("11-version-2", reg_nc[4] == REND_VERSION);
    EXPECT_TRUE("11-type-register", reg_nc[5] == REND_TYPE_REGISTER);
    EXPECT_TRUE("11-key-echoed", memcmp(&reg_nc[8], key, REND_KEY_LEN) == 0);
    EXPECT_TRUE("11-port-be", reg_nc[24] == 0x10 && reg_nc[25] == 0xE1); /* 4321 */
    {
        bool tail_zero = true;
        for (int i = 28; i < REND_REGISTER_PKT_LEN; i++) {
            if (reg_nc[i] != 0) tail_zero = false;
        }
        EXPECT_TRUE("11-null-cookie-tail-zero", tail_zero);
    }

    /* (c) Cookie present -> exact tail, head byte-identical to (b). */
    uint8_t reg_c[REND_REGISTER_PKT_LEN];
    memset(reg_c, 0xFF, sizeof(reg_c));
    EXPECT_TRUE("11-build-cookie", Rendezvous_BuildRegister(4321, key, cookie, reg_c));
    EXPECT_TRUE("11-cookie-tail", memcmp(&reg_c[28], cookie, REND_COOKIE_LEN) == 0);
    EXPECT_TRUE("11-head-unchanged", memcmp(reg_c, reg_nc, 28) == 0);

    /* (d) POLL carries the cookie the same way, with its own type. */
    uint8_t poll_c[REND_REGISTER_PKT_LEN];
    memset(poll_c, 0xFF, sizeof(poll_c));
    EXPECT_TRUE("11-build-poll", Rendezvous_BuildPoll(key, cookie, poll_c));
    EXPECT_TRUE("11-poll-type", poll_c[5] == REND_TYPE_POLL);
    EXPECT_TRUE("11-poll-version", poll_c[4] == REND_VERSION);
    EXPECT_TRUE("11-poll-cookie-tail", memcmp(&poll_c[28], cookie, REND_COOKIE_LEN) == 0);
    uint8_t poll_nc[REND_REGISTER_PKT_LEN];
    memset(poll_nc, 0xFF, sizeof(poll_nc));
    EXPECT_TRUE("11-build-poll-null", Rendezvous_BuildPoll(key, NULL, poll_nc));
    {
        bool tail_zero = true;
        for (int i = 28; i < REND_REGISTER_PKT_LEN; i++) {
            if (poll_nc[i] != 0) tail_zero = false;
        }
        EXPECT_TRUE("11-poll-null-cookie-tail-zero", tail_zero);
    }

    /* (e) Build a well-formed CHALLENGE the way the server does:
     *     magic(4) ver(1) type(1) reserved(2) key(16) cookie(8). */
    uint8_t chal[REND_CHALLENGE_LEN];
    memset(chal, 0, sizeof(chal));
    chal[0] = REND_MAGIC_BYTES_0;
    chal[1] = REND_MAGIC_BYTES_1;
    chal[2] = REND_MAGIC_BYTES_2;
    chal[3] = REND_MAGIC_BYTES_3;
    chal[4] = (uint8_t)REND_VERSION;
    chal[5] = (uint8_t)REND_TYPE_CHALLENGE;
    memcpy(&chal[8], key, REND_KEY_LEN);
    memcpy(&chal[24], cookie, REND_COOKIE_LEN);

    uint8_t out[REND_COOKIE_LEN];
    memset(out, 0x77, sizeof(out));
    EXPECT_TRUE("11-parse-ok",
                Rendezvous_ParseChallenge(chal, sizeof(chal), key, out));
    EXPECT_TRUE("11-parse-cookie", memcmp(out, cookie, REND_COOKIE_LEN) == 0);

    /* (f) Reject table. Every reject must ALSO zero the output so a
     * caller that ignores the return value cannot echo attacker bytes. */
#define EXPECT_CHAL_REJECT(tag, pkt, len, k) do {                            \
        uint8_t _o[REND_COOKIE_LEN];                                         \
        memset(_o, 0x77, sizeof(_o));                                        \
        EXPECT_FALSE(tag, Rendezvous_ParseChallenge((pkt), (len), (k), _o)); \
        bool _z = true;                                                      \
        for (int _i = 0; _i < REND_COOKIE_LEN; _i++) {                       \
            if (_o[_i] != 0) _z = false;                                     \
        }                                                                    \
        EXPECT_TRUE(tag "-zeroed", _z);                                      \
    } while (0)

    {   /* wrong magic */
        uint8_t bad[REND_CHALLENGE_LEN];
        memcpy(bad, chal, sizeof(bad));
        bad[0] ^= 0xFF;
        EXPECT_CHAL_REJECT("11-bad-magic", bad, sizeof(bad), key);
    }
    {   /* v1 version byte — a v1 server's frame must never be consumed */
        uint8_t bad[REND_CHALLENGE_LEN];
        memcpy(bad, chal, sizeof(bad));
        bad[4] = 1;
        EXPECT_CHAL_REJECT("11-v1-version", bad, sizeof(bad), key);
    }
    {   /* future version */
        uint8_t bad[REND_CHALLENGE_LEN];
        memcpy(bad, chal, sizeof(bad));
        bad[4] = 3;
        EXPECT_CHAL_REJECT("11-future-version", bad, sizeof(bad), key);
    }
    {   /* wrong type: a DELIVER must not be mistaken for a CHALLENGE */
        uint8_t bad[REND_CHALLENGE_LEN];
        memcpy(bad, chal, sizeof(bad));
        bad[5] = (uint8_t)REND_TYPE_DELIVER;
        EXPECT_CHAL_REJECT("11-deliver-not-challenge", bad, sizeof(bad), key);
    }
    /* cross-talk / forgery: right shape, someone else's session key */
    EXPECT_CHAL_REJECT("11-wrong-key", chal, sizeof(chal), other_key);
    /* truncated by one byte */
    EXPECT_CHAL_REJECT("11-short", chal, REND_CHALLENGE_LEN - 1, key);
    /* zero length / negative length */
    EXPECT_CHAL_REJECT("11-zero-len", chal, 0, key);
    EXPECT_CHAL_REJECT("11-neg-len", chal, -1, key);
#undef EXPECT_CHAL_REJECT

    /* (g) NULL-argument safety. */
    EXPECT_FALSE("11-null-pkt", Rendezvous_ParseChallenge(NULL, REND_CHALLENGE_LEN, key, out));
    EXPECT_FALSE("11-null-key", Rendezvous_ParseChallenge(chal, REND_CHALLENGE_LEN, NULL, out));
    EXPECT_FALSE("11-null-out", Rendezvous_ParseChallenge(chal, REND_CHALLENGE_LEN, key, NULL));
    EXPECT_FALSE("11-null-buf-register", Rendezvous_BuildRegister(1, key, cookie, NULL));
    EXPECT_FALSE("11-null-key-register", Rendezvous_BuildRegister(1, NULL, cookie, reg_c));

    if (fail_count == fails_before) {
        fprintf(stderr, "[test_bilateral_punch] test 11 OK — v2 cookie tail + "
                        "CHALLENGE parse gate\n");
        return 0;
    }
    return 1;
}

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
static int test_punch_gate_throttle(void) {
    fprintf(stderr, "[test_bilateral_punch] test 12: S4-review HIGH-1b punch-gate throttle\n");
    const int fails_before = fail_count;

    int src_max_bad = 0, total_reroll = 0, reroll_max = 0, src_table = 0;
    uint32_t mute_ms = 0;
    DirectP2P_TestHook_PunchGateLimits(&src_max_bad, &mute_ms, &total_reroll,
                                       &reroll_max, &src_table);

    /* Sanity on the shipped numbers themselves: a joiner's Stun_HolePunch
     * emits ~10 datagrams in its first 500 ms then 5/s, so the per-source
     * budget must clear that opening burst or a peer that merely raced a
     * drift re-encode would be muted mid-handshake. */
    if (src_max_bad <= 10) {
        FAIL("test12", "HOST_PUNCH_SRC_MAX_BAD <= the joiner's 10-datagram "
                       "opening punch burst — a legitimate peer could be muted");
    }
    if (total_reroll < src_max_bad) {
        FAIL("test12", "re-roll threshold below the per-source threshold — a "
                       "single source would re-roll the code before muting");
    }
    if (mute_ms == 0) {
        FAIL("test12", "mute lifetime is 0 — mutes would never take effect");
    }

    /* --- 12a: one source is muted at exactly the threshold, not before. */
    DirectP2P_TestHook_PunchGateReset();
    const uint32_t t0 = 1000u;
    for (int i = 1; i < src_max_bad; i++) {
        (void)DirectP2P_TestHook_PunchGateNoteBad("198.51.100.9", t0);
        if (DirectP2P_TestHook_PunchGateIsMuted("198.51.100.9", t0)) {
            fprintf(stderr, "[test_bilateral_punch] FAIL: test12a: muted early "
                            "after %d bad punches (threshold %d)\n", i, src_max_bad);
            fail_count++;
            break;
        }
    }
    (void)DirectP2P_TestHook_PunchGateNoteBad("198.51.100.9", t0);
    EXPECT_TRUE("test12a", DirectP2P_TestHook_PunchGateIsMuted("198.51.100.9", t0));

    /* --- 12b: muting is PER SOURCE — an unrelated address is unaffected.
     * This is the assertion that stops the throttle from becoming a
     * global denial of hosting. */
    EXPECT_FALSE("test12b", DirectP2P_TestHook_PunchGateIsMuted("203.0.113.5", t0));

    /* --- 12c: the mute EXPIRES. A permanent mute would turn this
     * defence into a self-inflicted lockout: friend typos the code,
     * burns the budget, then types it correctly and is dropped forever
     * with no diagnosis. */
    EXPECT_TRUE("test12c", DirectP2P_TestHook_PunchGateIsMuted("198.51.100.9",
                                                               t0 + mute_ms - 1u));
    EXPECT_FALSE("test12c", DirectP2P_TestHook_PunchGateIsMuted("198.51.100.9",
                                                                t0 + mute_ms));

    /* --- 12d: the session total crosses the re-roll threshold, and the
     * charge that crosses it is the one that reports "re-roll owed". */
    DirectP2P_TestHook_PunchGateReset();
    bool owed = false;
    int charged = 0;
    /* Spread across enough distinct sources that no single one is muted
     * before the total is reached — a muted source stops accruing
     * per-source strikes but STILL feeds the session total. */
    for (int i = 0; i < total_reroll && !owed; i++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "192.0.2.%d", (i % 200) + 1);
        owed = DirectP2P_TestHook_PunchGateNoteBad(ip, t0);
        charged++;
    }
    EXPECT_TRUE("test12d", owed);
    if (charged != total_reroll) {
        fprintf(stderr, "[test_bilateral_punch] FAIL: test12d: re-roll owed after "
                        "%d bad punches, expected exactly %d\n", charged, total_reroll);
        fail_count++;
    }
    {
        int bad_total = 0, rerolls = 0;
        DirectP2P_TestHook_PunchGateCounters(&bad_total, &rerolls);
        if (bad_total != total_reroll) {
            fprintf(stderr, "[test_bilateral_punch] FAIL: test12d: session total %d "
                            "!= %d\n", bad_total, total_reroll);
            fail_count++;
        }
    }

    /* --- 12e: a re-roll clears every mute. THIS is what keeps the
     * escape hatch open: the code those sources were failing against no
     * longer exists, so holding their strikes against them would be the
     * lockout again. */
    DirectP2P_TestHook_PunchGateReset();
    for (int i = 0; i < src_max_bad; i++) {
        (void)DirectP2P_TestHook_PunchGateNoteBad("198.51.100.9", t0);
    }
    EXPECT_TRUE("test12e", DirectP2P_TestHook_PunchGateIsMuted("198.51.100.9", t0));
    DirectP2P_TestHook_PunchGateClearMutes();
    EXPECT_FALSE("test12e", DirectP2P_TestHook_PunchGateIsMuted("198.51.100.9", t0));

    /* --- 12f: a muted source cannot evict a quieter one from the table
     * by rotating addresses. Fill the table with muted sources, then push
     * (table + 4) fresh addresses through: the original mutes must all
     * survive, otherwise an attacker clears its own mute for free. */
    DirectP2P_TestHook_PunchGateReset();
    for (int slot = 0; slot < src_table; slot++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "198.51.100.%d", slot + 1);
        for (int i = 0; i < src_max_bad; i++) {
            (void)DirectP2P_TestHook_PunchGateNoteBad(ip, t0);
        }
    }
    for (int i = 0; i < src_table + 4; i++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "203.0.113.%d", i + 1);
        (void)DirectP2P_TestHook_PunchGateNoteBad(ip, t0);
    }
    for (int slot = 0; slot < src_table; slot++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "198.51.100.%d", slot + 1);
        if (!DirectP2P_TestHook_PunchGateIsMuted(ip, t0)) {
            fprintf(stderr, "[test_bilateral_punch] FAIL: test12f: muted source %s "
                            "was evicted by address rotation — an attacker can "
                            "clear its own mute for free\n", ip);
            fail_count++;
            break;
        }
    }

    DirectP2P_TestHook_PunchGateReset();

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 12 OK — punch gate is capped "
                "(mute at %d/source, %u ms, re-roll at %d/session, max %d "
                "re-rolls) and cannot lock out a legitimate peer\n",
                src_max_bad, (unsigned)mute_ms, total_reroll, reroll_max);
        return 0;
    }
    return 1;
}

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
 * (rendezvous-server.js:517 compares exactly those two). The two agree
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
 * drain AND its main-thread CHALLENGE echo (direct_p2p.c:2558), and the
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

/* direct_p2p.c:1407 calls STUN_DISCOVER(&s_work.stun, ...) — the mock is
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
        /* 1000 ms is the code's own floor (direct_p2p.c:1225); the
         * seqlock assertion needs one worker cadence to elapse. */
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS, "1000");
    }

    send_log_reset();
    s_captured_stun = NULL;
    s_mock_discover_calls = 0;
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 1; /* the bilateral punch succeeds */
    DirectP2P_TestHook_SetStunDiscover(capturing_stun_discover);
    DirectP2P_TestHook_SetStunHolePunch(mock_stun_punch);
    DirectP2P_TestHook_SetRendezvousSend(recording_rendezvous_send);

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
     * direct_p2p.c:1476/1501 and is now frozen in the room code; moving
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
     *    (rendezvous-server.js:516-518). advertised_port is the UPnP
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
     *    main-thread handoff). */
    if (!tick_until_state(DIRECT_P2P_HANDOFF, 15000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test13: state %d after the cookie bound, "
                "expected HANDOFF (cookied=%d challenges=%d)\n",
                (int)DirectP2P_GetState(), ctx.cookied_requests, ctx.challenges_sent);
        fail_count++;
        rc = 1;
    }
    EXPECT_TRUE("13-uncookied-never-bound", ctx.uncookied_requests >= 1);

done:
    DirectP2P_TestHook_SetRendezvousSend(NULL);
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetStunHolePunch(NULL);
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
 * (connect_fail.h:198), so this used to spend ~31 s of real wall clock.
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
    DirectP2P_TestHook_SetStunHolePunch(mock_stun_punch);
    DirectP2P_TestHook_SetRendezvousSend(recording_rendezvous_send);
    /* No real router mutation (see test 13), and reach the shipped 30 s
     * CONNECT_HOST_ADVISORY_MS boundary in ~1 s of wall clock by scaling
     * the elapsed clock rather than the threshold. */
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
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
    DirectP2P_TestHook_SetStunHolePunch(NULL);
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
 * Part A — the joiner answers a CHALLENGE INLINE in its signaling loop
 * (direct_p2p.c:1836-1846): one RTT to bind instead of waiting out the
 * 500 ms resend cadence. Pinned by timing the first cookied REGISTER.
 * Part B — the same mock, never accepting: budget expiry must classify
 * COOKIE_REJECTED, which is only reachable when ev_challenge_any was set
 * at runtime.
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
    DirectP2P_TestHook_SetStunHolePunch(mock_stun_punch);
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
    DirectP2P_TestHook_SetStunHolePunch(NULL);
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

/* --- Test 16: S5 relay rung, end to end on the REAL joiner ------------- */

/*
 * docs/plan-netplay-connection.md §7. Drives BeginJoin through the whole
 * cascade against a mock rendezvous server that ALSO speaks the relay
 * extension, and asserts the four outcomes the rung can produce.
 *
 * The property that matters most is the first one: the relay endpoint
 * must reach the EXISTING do_handoff. The assertion is on do_handoff's
 * own arguments (DirectP2P_TestHook_LastHandoff), not on internal state
 * — a rung that populated s_work correctly but never reached the handoff
 * would otherwise pass.
 *
 * Both hole-punch phases are driven to failure through the force-relay
 * override rather than through mock_stun_punch, which is deliberate: it
 * exercises the override itself (a shipped, documented knob), it proves
 * the override really does no-op the punches (s_mock_punch_calls stays
 * 0), and it skips the LAN/hairpin bypasses that would otherwise
 * short-circuit this loopback rig before the rung is ever reached.
 */

static MockServerCtx* s_relay_ctx = NULL;
static int s_relay_handoffs_before = 0;
static int s_relay_fails_before = 0;

static bool pred_relay_handoff_done(void) {
    int n = 0;
    DirectP2P_TestHook_LastHandoff(NULL, 0, NULL, NULL, NULL, &n);
    return n > s_relay_handoffs_before;
}

static bool pred_relay_fail_reported(void) {
    return s_log_fail_lines > s_relay_fails_before;
}

/* One BeginJoin -> terminal-outcome pass against `mode`. Returns 0 on
 * success. `expect` is CONNECT_FAIL_NONE for the success case. */
static int relay_subrun(const char* tag, int mode, ConnectFailCode expect) {
    const int fails_before = fail_count;

    unsigned short server_port = 0, relay_port = 0;
    int server_sock = open_udp_on_localhost(&server_port);
    int relay_sock = open_udp_on_localhost(&relay_port);
    if (server_sock < 0 || relay_sock < 0) {
        FAIL(tag, "failed to bind localhost UDP sockets for the mock relay server");
        if (server_sock >= 0) close_sock(server_sock);
        if (relay_sock >= 0) close_sock(relay_sock);
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.use_synth_peer = true;
    ctx.synth_peer_ip_be = htonl(0xC6336407u); /* 198.51.100.7 (TEST-NET-2) */
    ctx.synth_peer_port = 6000;
    ctx.min_cookied_before_peer = 0; /* pair on the very first REGISTER */
    ctx.life_secs = 30;
    ctx.relay_mode = mode;
    ctx.relay_sock = relay_sock;
    ctx.relay_port = relay_port;
    ctx.relay_slot = 1; /* the joiner is slot B */
    for (int i = 0; i < REND_TOKEN_LEN; i++) {
        ctx.relay_token[i] = (uint8_t)(0xA0u + i);
    }
    s_relay_ctx = &ctx;

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_relay", &ctx);
    if (!tid) {
        FAIL(tag, "SDL_CreateThread failed");
        close_sock(server_sock);
        close_sock(relay_sock);
        return 1;
    }

    int rc = 0;
    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RELAY_BUDGET_MS, "800");
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
    }
    DirectP2P_TestHook_SetSignalBudgetMs(2500);
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetStunHolePunch(mock_stun_punch);
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 0; /* belt and braces: no punch may succeed */
    DirectP2P_TestHook_ResetHandoff();
    s_relay_handoffs_before = 0;
    s_relay_fails_before = s_log_fail_lines;

    /* Room code for a PUBLIC peer, matching the synthetic DELIVER: a LAN
     * or same-IP code would be bypassed before the fallback ever runs. */
    uint32_t nonce = 0;
    char code[ROOM_CODE_BUF_LEN];
    struct in_addr peer_in;
    if (!RoomCode_GenerateNonce(&nonce) ||
        inet_pton(AF_INET, "198.51.100.7", &peer_in) != 1 ||
        !RoomCode_Encode((uint32_t)peer_in.s_addr, 6000, nonce, code)) {
        FAIL(tag, "could not build a room code for the mock peer");
        rc = 1;
        goto done;
    }

    DirectP2P_BeginJoin(code);

    if (expect == CONNECT_FAIL_NONE) {
        if (!tick_until(pred_relay_handoff_done, 15000)) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: %s: no handoff (state=%d "
                    "relay_reqs=%d grants=%d pins_ok=%d acks=%d)\n",
                    tag, (int)DirectP2P_GetState(), ctx.relay_reqs, ctx.relay_grants,
                    ctx.relay_pins_ok, ctx.relay_acks);
            fail_count++;
            rc = 1;
            goto done;
        }
        char hip[64] = { 0 };
        uint16_t hport = 0;
        int hplayer = 0, hcount = 0;
        bool hrelay = false;
        DirectP2P_TestHook_LastHandoff(hip, (int)sizeof(hip), &hport, &hplayer,
                                       &hrelay, &hcount);

        /* THE assertion: do_handoff was handed the RELAY endpoint. */
        if (hport != relay_port || strcmp(hip, "127.0.0.1") != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: %s: do_handoff got %s:%u, expected the "
                    "relay endpoint 127.0.0.1:%u\n",
                    tag, hip, (unsigned)hport, (unsigned)relay_port);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("16A-handoff-flagged-relay", hrelay);
        EXPECT_TRUE("16A-handoff-player2", hplayer == 2);
        EXPECT_TRUE("16A-one-handoff", hcount == 1);

        /* The rung actually happened on the wire, on both legs. */
        EXPECT_TRUE("16A-relay-req", ctx.relay_reqs >= 1);
        EXPECT_TRUE("16A-relay-grant", ctx.relay_grants >= 1);
        EXPECT_TRUE("16A-pin-authenticated", ctx.relay_pins_ok >= 1);
        EXPECT_TRUE("16A-no-bad-pins", ctx.relay_pins_bad == 0);
        EXPECT_TRUE("16A-ack", ctx.relay_acks >= 1);
        /* The pin carried the slot the GRANT assigned — the client did
         * not invent one (the server's token HMAC binds the slot). */
        EXPECT_TRUE("16A-pin-slot-echoes-grant", ctx.relay_pin_slot == 1);
        /* The force-relay override really did no-op both punches. */
        EXPECT_TRUE("16A-punches-skipped", s_mock_punch_calls == 0);
        /* The user is told, and the report records which rung won. */
        EXPECT_TRUE("16A-status-names-relay",
                    strstr(DirectP2P_GetStatusText(), "relay") != NULL);
        if (strstr(s_log_last_ok, "via_relay=1") == NULL) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: %s: OK report line does not record the "
                    "relay rung: \"%s\"\n", tag, s_log_last_ok);
            fail_count++;
            rc = 1;
        }
    } else {
        if (!wait_for_state(DIRECT_P2P_FAILED_BILATERAL, 20000)) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: %s: state %d, expected FAILED_BILATERAL "
                    "(relay_reqs=%d grants=%d pins_ok=%d acks=%d)\n",
                    tag, (int)DirectP2P_GetState(), ctx.relay_reqs, ctx.relay_grants,
                    ctx.relay_pins_ok, ctx.relay_acks);
            fail_count++;
            rc = 1;
            goto done;
        }
        /* The user string is the taxonomy's, i.e. set_fail ran with the
         * relay cause and not with a NAT verdict. */
        if (strcmp(DirectP2P_GetStatusText(), ConnectFail_UserText(expect)) != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: %s: status \"%s\", expected \"%s\" (%s)\n",
                    tag, DirectP2P_GetStatusText(), ConnectFail_UserText(expect),
                    ConnectFail_Code(expect));
            fail_count++;
            rc = 1;
        }
        /* ...and the machine-coded report line agrees. */
        if (!tick_until(pred_relay_fail_reported, 5000)) {
            FAIL(tag, "no FAIL report line was emitted");
            rc = 1;
        } else {
            char want[64];
            SDL_snprintf(want, sizeof(want), "code=%s", ConnectFail_Code(expect));
            if (strstr(s_log_last_fail, want) == NULL) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: %s: report line lacks %s: \"%s\"\n",
                        tag, want, s_log_last_fail);
                fail_count++;
                rc = 1;
            }
            SDL_snprintf(want, sizeof(want), "relay_fail=%s", ConnectFail_Code(expect));
            if (strstr(s_log_last_fail, want) == NULL) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: %s: report line lacks %s: \"%s\"\n",
                        tag, want, s_log_last_fail);
                fail_count++;
                rc = 1;
            }
            EXPECT_TRUE("16-fail-not-flagged-relay",
                        strstr(s_log_last_fail, "via_relay=0") != NULL);
        }
        if (mode == MOCK_RELAY_NO_ACK) {
            /* The pin-timeout case must have PINNED (so the timeout is
             * genuinely about the ack) and received nothing back. */
            EXPECT_TRUE("16D-did-pin", ctx.relay_pins_ok >= 1);
            EXPECT_TRUE("16D-no-ack", ctx.relay_acks == 0);
        }
        if (mode == MOCK_RELAY_SILENT) {
            EXPECT_TRUE("16B-req-was-sent", ctx.relay_reqs >= 1);
            EXPECT_TRUE("16B-never-granted", ctx.relay_grants == 0);
        }
    }

done:
    /* DirectP2P_Cancel deliberately no-ops in HANDOFF (that is a live
     * session, not something a menu may abort), so the success sub-run
     * would otherwise leave the orchestrator parked there and every
     * later BeginJoin would early-return on `state != IDLE`. Run the
     * registered session-teardown callback — the same one netplay.c's
     * EXITING pass fires — exactly as test 13 does. */
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    DirectP2P_TestHook_SetSignalBudgetMs(0);
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetStunHolePunch(NULL);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, false);
    mock_server_stop(&ctx, tid, server_sock, server_port);
    close_sock(relay_sock);
    s_relay_ctx = NULL;
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/*
 * 16E — the HOST half. The rung exists on both roles and they take
 * different code paths to reach it (the joiner runs inline in its own
 * worker; the host runs on the bilateral-punch worker and has to rebuild
 * the signal endpoint and the advertised-tuple session key from scratch,
 * because the rendezvous worker has already exited by then). Covering
 * only the joiner would leave the entire host reconstruction unexercised.
 *
 * Drives the REAL BeginHost: HOST_WAITING -> REGISTER -> synthetic
 * DELIVER -> bilateral-punch worker (no-oped by force-relay) -> relay
 * rung -> s_bilateral_handoff_pending -> Tick's main-thread do_handoff.
 */
static int relay_host_subrun(void) {
    const char* tag = "test16E";
    const int fails_before = fail_count;

    unsigned short server_port = 0, relay_port = 0;
    int server_sock = open_udp_on_localhost(&server_port);
    int relay_sock = open_udp_on_localhost(&relay_port);
    if (server_sock < 0 || relay_sock < 0) {
        FAIL(tag, "failed to bind localhost UDP sockets for the mock relay server");
        if (server_sock >= 0) close_sock(server_sock);
        if (relay_sock >= 0) close_sock(relay_sock);
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.use_synth_peer = true;
    ctx.synth_peer_ip_be = htonl(0xC6336407u); /* 198.51.100.7 (TEST-NET-2) */
    ctx.synth_peer_port = 6000;
    ctx.min_cookied_before_peer = 0;
    ctx.life_secs = 30;
    ctx.relay_mode = MOCK_RELAY_OK;
    ctx.relay_sock = relay_sock;
    ctx.relay_port = relay_port;
    ctx.relay_slot = 0; /* the host is slot A */
    for (int i = 0; i < REND_TOKEN_LEN; i++) {
        ctx.relay_token[i] = (uint8_t)(0x50u + i);
    }

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_relay_h", &ctx);
    if (!tid) {
        FAIL(tag, "SDL_CreateThread failed");
        close_sock(server_sock);
        close_sock(relay_sock);
        return 1;
    }

    int rc = 0;
    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS, "1000");
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RELAY_BUDGET_MS, "800");
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, true);
        /* Never let a unit test install a real 1-hour UPnP mapping on
         * whatever router the developer is behind (see test 13). */
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
    }
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetStunHolePunch(mock_stun_punch);
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 0;
    DirectP2P_TestHook_ResetHandoff();
    s_relay_handoffs_before = 0;

    DirectP2P_BeginHost(0);
    if (!wait_for_state(DIRECT_P2P_HOST_WAITING, 25000)) {
        fprintf(stderr, "[test_bilateral_punch] FAIL: %s: state %d, expected HOST_WAITING\n",
                tag, (int)DirectP2P_GetState());
        fail_count++;
        rc = 1;
        goto done;
    }

    if (!tick_until(pred_relay_handoff_done, 20000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: %s: no handoff (state=%d relay_reqs=%d "
                "grants=%d pins_ok=%d acks=%d)\n",
                tag, (int)DirectP2P_GetState(), ctx.relay_reqs, ctx.relay_grants,
                ctx.relay_pins_ok, ctx.relay_acks);
        fail_count++;
        rc = 1;
        goto done;
    }

    {
        char hip[64] = { 0 };
        uint16_t hport = 0;
        int hplayer = 0, hcount = 0;
        bool hrelay = false;
        DirectP2P_TestHook_LastHandoff(hip, (int)sizeof(hip), &hport, &hplayer,
                                       &hrelay, &hcount);
        if (hport != relay_port || strcmp(hip, "127.0.0.1") != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: %s: do_handoff got %s:%u, expected the "
                    "relay endpoint 127.0.0.1:%u\n",
                    tag, hip, (unsigned)hport, (unsigned)relay_port);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("16E-handoff-flagged-relay", hrelay);
        /* The host is player 1 — the same argument the direct and
         * bilateral host paths pass. */
        EXPECT_TRUE("16E-handoff-player1", hplayer == 1);
        EXPECT_TRUE("16E-one-handoff", hcount == 1);
        /* The host reconstructed the signal endpoint and the
         * advertised-tuple session key on the punch worker after the
         * rendezvous worker had exited — if either were wrong, the mock
         * would have seen no RELAY_REQ at all. */
        EXPECT_TRUE("16E-relay-req", ctx.relay_reqs >= 1);
        EXPECT_TRUE("16E-relay-grant", ctx.relay_grants >= 1);
        EXPECT_TRUE("16E-pin-authenticated", ctx.relay_pins_ok >= 1);
        EXPECT_TRUE("16E-no-bad-pins", ctx.relay_pins_bad == 0);
        EXPECT_TRUE("16E-pin-slot-echoes-grant", ctx.relay_pin_slot == 0);
        EXPECT_TRUE("16E-punches-skipped", s_mock_punch_calls == 0);
        EXPECT_TRUE("16E-status-names-relay",
                    strstr(DirectP2P_GetStatusText(), "relay") != NULL);
    }

done:
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetStunHolePunch(NULL);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, false);
    mock_server_stop(&ctx, tid, server_sock, server_port);
    close_sock(relay_sock);
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

static int test_relay_rung(void) {
    fprintf(stderr, "[test_bilateral_punch] test 16: S5 relay rung "
                    "(RELAY_REQ -> GRANT -> PIN -> ACK -> do_handoff)\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown(); /* whatever ran before -> IDLE */
    SDL_GetLogOutputFunction(&s_prev_log_fn, &s_prev_log_ud);
    SDL_SetLogOutputFunction(capture_log_fn, NULL);

    rc |= relay_subrun("test16A", MOCK_RELAY_OK, CONNECT_FAIL_NONE);
    rc |= relay_subrun("test16B", MOCK_RELAY_SILENT, CONNECT_FAIL_RELAY_UNAVAILABLE);
    rc |= relay_subrun("test16C", MOCK_RELAY_REFUSE, CONNECT_FAIL_RELAY_REFUSED);
    rc |= relay_subrun("test16D", MOCK_RELAY_NO_ACK, CONNECT_FAIL_RELAY_PIN_TIMEOUT);
    rc |= relay_host_subrun(); /* 16E: the HOST half of the rung */

    SDL_SetLogOutputFunction(s_prev_log_fn, s_prev_log_ud);
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();

    if (rc == 0 && fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 16 OK — relay endpoint reached do_handoff on "
                "BOTH roles, and silent / refused / unacked relays classify as %s / %s / %s\n",
                ConnectFail_Code(CONNECT_FAIL_RELAY_UNAVAILABLE),
                ConnectFail_Code(CONNECT_FAIL_RELAY_REFUSED),
                ConnectFail_Code(CONNECT_FAIL_RELAY_PIN_TIMEOUT));
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
    rc |= test_rendezvous_cookie_codec();
    rc |= test_punch_gate_throttle();
    rc |= test_host_cookie_handshake();
    rc |= test_joiner_cookie_handshake();
    rc |= test_relay_rung(); /* S5 */
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
