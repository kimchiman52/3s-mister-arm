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
#include "netplay/natpmp.h" /* S7, test 18 */
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
/* S6 test 21: "not yet", as opposed to "never" — racing can reach the
 * server before the peer's own REGISTER has landed. */
#define MOCK_RELAY_STATUS_NOT_PAIRED     2

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
    /* S6 test 20C: additionally withhold the peer-bearing DELIVER until
     * this many ms after the mock thread started, so a test can place the
     * DELIVER (and therefore the DELIVER punch candidate) AFTER the relay
     * leg's RACE_RELAY_ARM_MS. Answers the zero-sentinel until then, which
     * every client treats as "keep waiting". 0 = no delay. */
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
    /* S6 test 21: answer the first N RELAY_REQs with NOT_PAIRED before
     * granting, i.e. "the peer has not registered YET". */
    int             relay_notpaired_first;
    volatile int    relay_notpaired_sent;
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
    ctx->started_ms = SDL_GetTicks(); /* S6 test 20C: deliver_delay_ms base */
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
            /* S6 test 21: the first N requests get "not yet". */
            const bool not_paired =
                !refuse && ctx->relay_notpaired_sent < ctx->relay_notpaired_first;
            if (not_paired) ctx->relay_notpaired_sent++;
            uint8_t g[REND_GRANT_LEN];
            const int gl = build_relay_grant(
                g, req_key,
                (refuse || not_paired) ? 0xFFu : ctx->relay_slot,
                refuse ? MOCK_RELAY_STATUS_POOL_EXHAUSTED
                       : not_paired ? MOCK_RELAY_STATUS_NOT_PAIRED
                                    : MOCK_RELAY_STATUS_GRANTED,
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
    DirectP2P_TestHook_SetPunchOracle(mock_punch_oracle);
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
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
        /* S7: same reason, the other backend. Without this a Linux test run
         * would find a real default gateway in /proc/net/route and install a
         * real 1-hour NAT-PMP mapping on the developer's router. (On macOS
         * gateway discovery reports nothing, so this is belt AND braces.) */
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    }
    DirectP2P_TestHook_SetSignalBudgetMs(2500);
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetPunchOracle(mock_punch_oracle);
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
    DirectP2P_TestHook_SetPunchOracle(NULL);
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
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
        /* S7: same reason, the other backend. Without this a Linux test run
         * would find a real default gateway in /proc/net/route and install a
         * real 1-hour NAT-PMP mapping on the developer's router. (On macOS
         * gateway discovery reports nothing, so this is belt AND braces.) */
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
    }
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetPunchOracle(mock_punch_oracle);
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
    DirectP2P_TestHook_SetPunchOracle(NULL);
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

/* --- Test 17: S5 relay codec pinned to literal bytes ------------------- */

/*
 * The relay wire format has TWO independent implementations — rendezvous.c
 * here and rendezvous-server.js there — both written from the same spec by
 * the same hand. Test 16's mock is a THIRD implementation in this file, so
 * a byte-offset mistake shared between the C codec and the C mock would
 * sail through test 16 while failing against the real server; the JS suite
 * would likewise stay green because its own duplicate codec would share
 * the server's (correct) offsets. Neither suite can see that class of bug.
 *
 * This test closes it by pinning the C side to LITERAL BYTES matching the
 * layout documented in docs/plan-netplay-connection.md §7.2 — the same
 * document the server was written from — rather than to any encoder in
 * this repository.
 *
 * It also pins the invariant the whole design rests on: a RELAY_REQ must
 * be byte-identical to a REGISTER apart from the type byte, because that
 * is what lets it ride the server's return-routability gate (which reads
 * the session key at [8..24) and the cookie at [28..36)) instead of
 * needing a second gate. If the cookie ever moved, the gate would read
 * garbage and every relay request would be silently re-challenged
 * forever — a failure that looks exactly like "the server is down".
 */
static int test_relay_codec(void) {
    fprintf(stderr, "[test_bilateral_punch] test 17: S5 relay codec vs literal bytes\n");
    const int fails_before = fail_count;

    uint8_t key[REND_KEY_LEN];
    for (int i = 0; i < REND_KEY_LEN; i++) key[i] = (uint8_t)(0x10u + i);
    uint8_t cookie[REND_COOKIE_LEN];
    for (int i = 0; i < REND_COOKIE_LEN; i++) cookie[i] = (uint8_t)(0xC0u + i);
    uint8_t token[REND_TOKEN_LEN];
    for (int i = 0; i < REND_TOKEN_LEN; i++) token[i] = (uint8_t)(0x70u + i);

    /* (1) THE invariant: RELAY_REQ == REGISTER with byte 5 retyped. */
    {
        uint8_t reg[REND_REGISTER_PKT_LEN];
        uint8_t req[REND_RELAY_REQ_PKT_LEN];
        EXPECT_TRUE("17-build-register", Rendezvous_BuildRegister(0xBEEF, key, cookie, reg));
        EXPECT_TRUE("17-build-relayreq", Rendezvous_BuildRelayReq(0xBEEF, key, cookie, req));
        EXPECT_TRUE("17-req-len-is-register-len",
                    (int)sizeof(req) == REND_REGISTER_PKT_LEN);
        EXPECT_TRUE("17-req-type", req[5] == REND_TYPE_RELAY_REQ);
        EXPECT_TRUE("17-reg-type", reg[5] == REND_TYPE_REGISTER);
        reg[5] = REND_TYPE_RELAY_REQ;
        if (memcmp(reg, req, REND_REGISTER_PKT_LEN) != 0) {
            FAIL("17-req-is-register",
                 "RELAY_REQ differs from REGISTER by more than the type byte — it would "
                 "no longer pass the server's return-routability gate");
        }
        /* And the fields the gate reads are where the gate expects. */
        EXPECT_TRUE("17-req-key-offset", memcmp(&req[8], key, REND_KEY_LEN) == 0);
        EXPECT_TRUE("17-req-cookie-offset",
                    memcmp(&req[28], cookie, REND_COOKIE_LEN) == 0);
    }

    /* (2) RELAY_GRANT: parse a hand-written frame laid out per §7.2 —
     *     magic(4) ver(1) type(1) slot(1) status(1) key(16) port_be(2)
     *     rsv(2) token(8). */
    uint8_t grant[REND_RELAY_GRANT_LEN];
    memset(grant, 0, sizeof(grant));
    grant[0] = 0x33; grant[1] = 0x53; grant[2] = 0x58; grant[3] = 0x52; /* '3SXR' */
    grant[4] = 2;                       /* version — unchanged by S5 */
    grant[5] = REND_TYPE_RELAY_GRANT;
    grant[6] = 1;                       /* slot B */
    grant[7] = 0;                       /* GRANTED */
    memcpy(&grant[8], key, REND_KEY_LEN);
    grant[24] = 0x84; grant[25] = 0xD0; /* port 34000, big-endian */
    memcpy(&grant[28], token, REND_TOKEN_LEN);
    {
        RendezvousRelayGrant g;
        EXPECT_TRUE("17-grant-parse",
                    Rendezvous_ParseRelayGrant(grant, (int)sizeof(grant), key, &g));
        EXPECT_TRUE("17-grant-slot", g.slot == 1);
        EXPECT_TRUE("17-grant-status", g.status == (uint8_t)REND_RELAY_GRANTED);
        if (g.relay_port != 34000) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 17-grant-port: got %u, expected 34000 "
                    "(port is big-endian at [24..26))\n", (unsigned)g.relay_port);
            fail_count++;
        }
        EXPECT_TRUE("17-grant-token", memcmp(g.token, token, REND_TOKEN_LEN) == 0);
    }

    /* (3) Reject table. Every reject must ZERO the output, so a caller
     *     that ignores the return value cannot act on attacker bytes. */
    {
        struct { const char* tag; int off; uint8_t val; } bad[] = {
            { "17-grant-magic",   0, 0x34 },
            { "17-grant-version", 4, 3    },
            { "17-grant-type",    5, 2    }, /* a DELIVER, not a grant */
            { "17-grant-key",     8, 0xFF },
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            uint8_t b[REND_RELAY_GRANT_LEN];
            memcpy(b, grant, sizeof(b));
            b[bad[i].off] = bad[i].val;
            RendezvousRelayGrant g;
            memset(&g, 0xAA, sizeof(g));
            EXPECT_FALSE(bad[i].tag,
                         Rendezvous_ParseRelayGrant(b, (int)sizeof(b), key, &g));
            EXPECT_TRUE("17-grant-reject-zeroes", g.relay_port == 0 && g.status == 0);
        }
        /* Short frame. */
        RendezvousRelayGrant g;
        EXPECT_FALSE("17-grant-short",
                     Rendezvous_ParseRelayGrant(grant, REND_RELAY_GRANT_LEN - 1, key, &g));

        /* FAIL CLOSED: a GRANTED status carrying an unconnectable port,
         * or a slot the pin encoder cannot express, must not parse — the
         * caller would otherwise burn its whole pin budget on nothing. */
        uint8_t p0[REND_RELAY_GRANT_LEN];
        memcpy(p0, grant, sizeof(p0));
        p0[24] = 0; p0[25] = 0;
        EXPECT_FALSE("17-grant-granted-port0",
                     Rendezvous_ParseRelayGrant(p0, (int)sizeof(p0), key, &g));
        uint8_t s2[REND_RELAY_GRANT_LEN];
        memcpy(s2, grant, sizeof(s2));
        s2[6] = 2;
        EXPECT_FALSE("17-grant-granted-slot2",
                     Rendezvous_ParseRelayGrant(s2, (int)sizeof(s2), key, &g));

        /* A REFUSAL is a VALID frame and must parse TRUE with its status —
         * a client that cannot tell "refused" from "silence" is the
         * reporting defect this cause exists to remove. */
        uint8_t refused[REND_RELAY_GRANT_LEN];
        memcpy(refused, grant, sizeof(refused));
        refused[6] = REND_RELAY_SLOT_NONE;
        refused[7] = (uint8_t)REND_RELAY_POOL_EXHAUSTED;
        refused[24] = 0; refused[25] = 0;
        RendezvousRelayGrant gr;
        EXPECT_TRUE("17-grant-refusal-parses",
                    Rendezvous_ParseRelayGrant(refused, (int)sizeof(refused), key, &gr));
        EXPECT_TRUE("17-grant-refusal-status",
                    gr.status == (uint8_t)REND_RELAY_POOL_EXHAUSTED);
        EXPECT_TRUE("17-grant-refusal-port0", gr.relay_port == 0);
    }

    /* (4) RELAY_PIN: magic(4) ver(1) type(1) slot(1) rsv(1) token(8)
     *     rsv2(4) = 20 bytes. */
    {
        uint8_t pin[REND_RELAY_PIN_LEN];
        uint8_t want[REND_RELAY_PIN_LEN];
        memset(want, 0, sizeof(want));
        want[0] = 0x33; want[1] = 0x53; want[2] = 0x58; want[3] = 0x52;
        want[4] = 2;
        want[5] = REND_TYPE_RELAY_PIN;
        want[6] = 1;
        memcpy(&want[8], token, REND_TOKEN_LEN);
        EXPECT_TRUE("17-pin-build", Rendezvous_BuildRelayPin(1, token, pin));
        if (memcmp(pin, want, sizeof(want)) != 0) {
            FAIL("17-pin-bytes", "RELAY_PIN bytes do not match the documented layout");
        }
        /* Slots other than 0/1 do not exist on the wire. */
        EXPECT_FALSE("17-pin-slot2", Rendezvous_BuildRelayPin(2, token, pin));
    }

    /* (5) RELAY_PIN_ACK: magic(4) ver(1) type(1) slot(1) peer_pinned(1)
     *     rsv(4) = 12 bytes. */
    {
        uint8_t ack[REND_RELAY_PIN_ACK_LEN];
        memset(ack, 0, sizeof(ack));
        ack[0] = 0x33; ack[1] = 0x53; ack[2] = 0x58; ack[3] = 0x52;
        ack[4] = 2;
        ack[5] = REND_TYPE_RELAY_PIN_ACK;
        ack[6] = 1;
        ack[7] = 1; /* peer pinned */
        bool pp = false;
        EXPECT_TRUE("17-ack-parse",
                    Rendezvous_ParseRelayPinAck(ack, (int)sizeof(ack), 1, &pp));
        EXPECT_TRUE("17-ack-peer-pinned", pp);
        ack[7] = 0;
        EXPECT_TRUE("17-ack-parse2",
                    Rendezvous_ParseRelayPinAck(ack, (int)sizeof(ack), 1, &pp));
        EXPECT_FALSE("17-ack-peer-not-pinned", pp);
        /* An ACK for the OTHER side is not ours. */
        pp = true;
        EXPECT_FALSE("17-ack-slot-mismatch",
                     Rendezvous_ParseRelayPinAck(ack, (int)sizeof(ack), 0, &pp));
        EXPECT_FALSE("17-ack-reject-clears", pp);
        ack[5] = REND_TYPE_RELAY_GRANT;
        EXPECT_FALSE("17-ack-wrong-type",
                     Rendezvous_ParseRelayPinAck(ack, (int)sizeof(ack), 1, &pp));
    }

    /* (6) Rendezvous_FrameType — the router every receive site now uses
     *     instead of open-coding the magic/version test. */
    {
        EXPECT_TRUE("17-ft-grant",
                    Rendezvous_FrameType(grant, (int)sizeof(grant)) == REND_FRAME_RELAY_GRANT);
        uint8_t v3[REND_RELAY_GRANT_LEN];
        memcpy(v3, grant, sizeof(v3));
        v3[4] = 3;
        EXPECT_TRUE("17-ft-wrong-version", Rendezvous_FrameType(v3, (int)sizeof(v3)) == 0);
        uint8_t nm[REND_RELAY_GRANT_LEN];
        memcpy(nm, grant, sizeof(nm));
        nm[1] = 0x00;
        EXPECT_TRUE("17-ft-wrong-magic", Rendezvous_FrameType(nm, (int)sizeof(nm)) == 0);
        EXPECT_TRUE("17-ft-short", Rendezvous_FrameType(grant, 5) == 0);
        EXPECT_TRUE("17-ft-null", Rendezvous_FrameType(NULL, 36) == 0);
        /* A GekkoNet packet must never look like a '3SXR' frame — that is
         * what makes sdl_net_adapter.c's straggler drop exact rather than
         * heuristic. PacketType is 1..7 (GekkoNet net.h:28-36). */
        for (uint8_t t = 1; t <= 7; t++) {
            uint8_t gek[32];
            memset(gek, 0x5A, sizeof(gek));
            gek[0] = t;
            EXPECT_TRUE("17-ft-gekko-never-matches",
                        Rendezvous_FrameType(gek, (int)sizeof(gek)) == 0);
        }
    }

    /* (7) Rendezvous_HasMagic — the STRAGGLER test, and deliberately
     *     weaker than FrameType (review LOW-1).
     *
     *     sdl_net_adapter.c's drop used to be `FrameType(...) != 0`,
     *     which returns 0 for ANY version != REND_VERSION. A non-v2
     *     '3SXR' frame therefore passed straight through the guard with
     *     data[0] == 0x33, was miscounted as an InputAck (0x33 & 7 == 3),
     *     and reached GekkoNet as a packet of type 51 — the exact bug the
     *     guard exists to close. The version-independent case below is
     *     what fails if that regression ever comes back. */
    {
        EXPECT_TRUE("17-magic-grant",
                    Rendezvous_HasMagic(grant, (int)sizeof(grant)));
        uint8_t v3[REND_RELAY_GRANT_LEN];
        memcpy(v3, grant, sizeof(v3));
        v3[4] = 3;
        EXPECT_TRUE("17-magic-wrong-version-still-dropped",
                    Rendezvous_HasMagic(v3, (int)sizeof(v3)));
        EXPECT_TRUE("17-magic-wrong-version-is-invisible-to-frametype",
                    Rendezvous_FrameType(v3, (int)sizeof(v3)) == 0);
        /* An unknown TYPE on the right version is likewise still ours. */
        uint8_t t99[REND_RELAY_GRANT_LEN];
        memcpy(t99, grant, sizeof(t99));
        t99[5] = 99;
        EXPECT_TRUE("17-magic-unknown-type-still-dropped",
                    Rendezvous_HasMagic(t99, (int)sizeof(t99)));
        uint8_t nm2[REND_RELAY_GRANT_LEN];
        memcpy(nm2, grant, sizeof(nm2));
        nm2[1] = 0x00;
        EXPECT_FALSE("17-magic-wrong-magic", Rendezvous_HasMagic(nm2, (int)sizeof(nm2)));
        EXPECT_FALSE("17-magic-short", Rendezvous_HasMagic(grant, 3));
        EXPECT_FALSE("17-magic-null", Rendezvous_HasMagic(NULL, 36));
        /* Still exact: no GekkoNet packet can carry our magic. */
        for (uint8_t t = 1; t <= 7; t++) {
            uint8_t gek[32];
            memset(gek, 0x5A, sizeof(gek));
            gek[0] = t;
            EXPECT_FALSE("17-magic-gekko-never-matches",
                         Rendezvous_HasMagic(gek, (int)sizeof(gek)));
        }
    }

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 17 OK — relay frames match the documented "
                "byte layout, RELAY_REQ is a retyped REGISTER, and no GekkoNet packet "
                "can be mistaken for a '3SXR' frame\n");
        return 0;
    }
    return 1;
}

/* --- Tests 18-21: S6 candidate racing ---------------------------------- */

/*
 * docs/plan-netplay-connection.md §8. Four properties, each chosen so that
 * a serial cascade CANNOT satisfy it — i.e. each one goes red if p2p_race
 * stops racing:
 *
 *   18  worst-case wall clock to terminal failure is bounded by the race
 *       budget, not by the SUM of the leg budgets
 *   19  the DELIVER endpoint is punched WHILE the room-code endpoint is
 *       still being punched (serially it could not even start until the
 *       direct window had closed)
 *   20  a confirmed punch beats the relay, and a pair that punches inside
 *       RACE_RELAY_ARM_MS never asks the server for a relay port at all
 *       (the §7.4 "costs a connectable pair nothing" promise, which naive
 *       racing would have broken)
 *   21  a NOT_PAIRED relay refusal is transient, not terminal — racing can
 *       now ask while the peer's REGISTER is still in flight
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
 * signal 8000 + bilateral 5000 + relay 4000 = 19500 ms per attempt, x2
 * attempts for the S2 auto-retry = 39000 ms. Measured on the pre-S6 tree
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


/* Scenario B: every leg runs. The mock DELIVERs a synthetic peer at once
 * (so the bilateral / DELIVER candidate exists) and its relay is SILENT
 * (so the relay leg runs and times out on the GRANT phase). */
static uint32_t probe_run_cascade(const char* label) {
    unsigned short server_port = 0, relay_port = 0;
    const int server_sock = open_udp_on_localhost(&server_port);
    const int relay_sock = open_udp_on_localhost(&relay_port);
    if (server_sock < 0 || relay_sock < 0) {
        fprintf(stderr, "[timing-probe] %s: could not bind the mock sockets\n", label);
        if (server_sock >= 0) close_sock(server_sock);
        if (relay_sock >= 0) close_sock(relay_sock);
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
    ctx.relay_mode = MOCK_RELAY_SILENT;
    ctx.relay_sock = relay_sock;
    ctx.relay_port = relay_port;
    ctx.relay_slot = 1;
    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_probe", &ctx);
    if (!tid) {
        fprintf(stderr, "[timing-probe] %s: SDL_CreateThread failed\n", label);
        close_sock(server_sock);
        close_sock(relay_sock);
        return 0;
    }
    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, false);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_RELAY, false);
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
                "(state=%d, 2 attempts, ~%u ms per attempt, relay_reqs=%d)\n",
                label, (unsigned)elapsed, (int)DirectP2P_GetState(),
                (unsigned)(elapsed / 2u), ctx.relay_reqs);
    } else {
        fprintf(stderr, "[timing-probe] %s: could not build a room code\n", label);
    }
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    mock_server_stop(&ctx, tid, server_sock, server_port);
    close_sock(relay_sock);
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
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, false);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_RELAY, false);
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
    /* Scenario B — EVERY leg runs (the DELIVER arrives, so there is a
     * second punch candidate, and the relay is silent so its leg runs and
     * times out). This is the scenario that distinguishes racing from
     * seriality, and its bound is the one that actually bites:
     *
     *   measured pre-S6 (worktree 3sx-mister-s6base @ 9eadde33, same probe)
     *       19354 ms total  ->  9677 ms/attempt
     *   measured post-S6
     *       10235 ms total  ->  5117 ms/attempt
     *   measured post-S6-review (this tree, same probe)
     *       10229 ms total  ->  5114 ms/attempt
     *   measured under N10 (the legs serialised again)
     *       14216 ms total  ->  7108 ms/attempt
     *
     * S6-review M-4: the old 14 000 ms bound sat 216 ms — 1.5% — below the
     * N10 number, so a faster machine could have made this assertion
     * unable to fail, and a test that cannot fail is worse than no test.
     * 12 000 ms sits between the two MEASURED numbers with 1 771 ms
     * (17.3%) of headroom over the passing run and 2 216 ms (18.5%) of
     * margin under the neutralised one. Serialise the legs again and this
     * goes red on the clock. */
    if (cascade_ms == 0 || cascade_ms >= S6_CASCADE_BOUND_MS) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test18: full-cascade join took %u ms "
                "(~%u ms/attempt), expected < %u ms — the punch, signalling and relay "
                "legs must OVERLAP (measured pre-S6 serial: 19354 ms)\n",
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
} MockGwMode;

typedef struct {
    int sock;
    MockGwMode mode;
    volatile bool stop;
    uint32_t ext_ip_be;  /* external IP the gateway claims                  */
    uint16_t ext_port;   /* external port it grants                         */
    uint32_t lifetime_s; /* lifetime it grants (may differ from requested)  */
    /* Observation counters + last frames, for byte-level assertions. */
    int pcp_reqs;
    int pmp_addr_reqs;
    int pmp_map_reqs;
    uint8_t last_pcp[128];
    int last_pcp_len;
    uint8_t last_pmp_map[64];
    int last_pmp_map_len;
} MockGwCtx;

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
            ctx->pcp_reqs++;
            if (n <= (int)sizeof(ctx->last_pcp)) {
                memcpy(ctx->last_pcp, buf, (size_t)n);
                ctx->last_pcp_len = n;
            }
            if (ctx->mode == MOCK_GW_SILENT) continue;
            if (ctx->mode == MOCK_GW_PCP) {
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
                r[11] = 0x2A;           /* Epoch Time, arbitrary           */
                memcpy(&r[24], &buf[24], 12); /* Mapping Nonce, copied     */
                r[36] = buf[36];              /* Protocol, copied          */
                r[40] = buf[40];              /* Internal Port, copied     */
                r[41] = buf[41];
                r[42] = (uint8_t)(ctx->ext_port >> 8);
                r[43] = (uint8_t)(ctx->ext_port & 0xFFu);
                /* Assigned External IP as an IPv4-mapped IPv6 (§5). */
                r[54] = 0xFF; r[55] = 0xFF;
                memcpy(&r[56], &ctx->ext_ip_be, 4);
                sendto(ctx->sock, (const char*)r, sizeof(r), 0,
                       (struct sockaddr*)&from, fl);
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
            r[7] = 0x2A;
            sendto(ctx->sock, (const char*)r, sizeof(r), 0,
                   (struct sockaddr*)&from, fl);
            continue;
        }

        if (buf[0] != 0 || ctx->mode == MOCK_GW_SILENT) continue;
        if (ctx->mode == MOCK_GW_PCP) continue; /* a PCP-only box ignores v0 */

        if (buf[1] == 0) {
            /* Public Address Response, RFC 6886 §3.2 (12 bytes). */
            ctx->pmp_addr_reqs++;
            uint8_t r[12];
            memset(r, 0, sizeof(r));
            r[0] = 0;
            r[1] = 128 + 0;
            r[2] = 0; r[3] = 0;   /* Result Code = 0 */
            r[7] = 0x2A;          /* Seconds Since Start of Epoch */
            memcpy(&r[8], &ctx->ext_ip_be, 4);
            sendto(ctx->sock, (const char*)r, sizeof(r), 0,
                   (struct sockaddr*)&from, fl);
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
            r[7] = 0x2A;
            r[8] = buf[4]; r[9] = buf[5]; /* Internal Port, echoed */
            if (!refuse) {
                r[10] = (uint8_t)(ctx->ext_port >> 8);
                r[11] = (uint8_t)(ctx->ext_port & 0xFFu);
                r[12] = (uint8_t)(ctx->lifetime_s >> 24);
                r[13] = (uint8_t)(ctx->lifetime_s >> 16);
                r[14] = (uint8_t)(ctx->lifetime_s >> 8);
                r[15] = (uint8_t)(ctx->lifetime_s);
            }
            sendto(ctx->sock, (const char*)r, sizeof(r), 0,
                   (struct sockaddr*)&from, fl);
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

    /* ================= (A) codec vs literal RFC bytes ================= */

    uint8_t nonce[NATPMP_PCP_NONCE_LEN];
    for (int i = 0; i < NATPMP_PCP_NONCE_LEN; i++) nonce[i] = (uint8_t)(0xA0u + i);

    {
        /* PCP MAP request, RFC 6887 §7.1 (24-byte common header) +
         * §11.1 (36-byte MAP block) = 60 bytes. Expectation hand-built
         * from the figures, byte by byte. */
        uint8_t want[NATPMP_PCP_MAP_LEN];
        memset(want, 0, sizeof(want));
        want[0] = 2;    /* Version = 2                                  */
        want[1] = 1;    /* R = 0 (request) | Opcode = MAP (1)           */
        /* want[2..4) Reserved (16 bits) = 0                            */
        want[4] = 0x00; want[5] = 0x00; want[6] = 0x0E; want[7] = 0x10; /* 3600 */
        /* PCP Client's IP Address, 128 bits, IPv4-mapped (§5):
         * ::ffff:192.168.1.77 */
        want[18] = 0xFF; want[19] = 0xFF;
        want[20] = 192; want[21] = 168; want[22] = 1; want[23] = 77;
        memcpy(&want[24], nonce, NATPMP_PCP_NONCE_LEN); /* Mapping Nonce */
        want[36] = 17;  /* Protocol = UDP                               */
        /* want[37..40) Reserved (24 bits) = 0                          */
        want[40] = 0xD4; want[41] = 0x31; /* Internal Port 54321        */
        want[42] = 0xD4; want[43] = 0x31; /* Suggested External 54321   */
        /* Suggested External IP = the all-zeros address for "no
         * preference" (§11.1: "it MUST use the address-family-specific
         * all-zeros address (see Section 5)"). For IPv4 that is NOT 16
         * zero bytes — §5 is explicit: "The all-zeros IPv4 address MUST
         * be expressed by 80 bits of zeros, 16 bits of ones, and 32 bits
         * of zeros (::ffff:0:0)." Sending bare :: would be the IPv6
         * unspecified address in an otherwise IPv4 request. */
        want[54] = 0xFF; want[55] = 0xFF;

        struct in_addr client;
        memset(&client, 0, sizeof(client));
        EXPECT_TRUE("22-pton", inet_pton(AF_INET, "192.168.1.77", &client) == 1);

        uint8_t got[NATPMP_PCP_MAP_LEN];
        EXPECT_TRUE("22-pcp-build",
                    Natpmp_BuildPcpMapRequest(got, nonce, NATPMP_PROTO_UDP, 54321,
                                              54321, 0, (uint32_t)client.s_addr, 3600));
        if (memcmp(got, want, sizeof(want)) != 0) {
            int off = -1;
            for (int i = 0; i < (int)sizeof(want); i++) {
                if (got[i] != want[i]) { off = i; break; }
            }
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 18-pcp-req-bytes: first difference at "
                    "offset %d (got 0x%02X, RFC 6887 §7.1/§11.1 layout wants 0x%02X)\n",
                    off, off >= 0 ? got[off] : 0, off >= 0 ? want[off] : 0);
            fail_count++;
        }
        /* And a delete is the same frame with Requested Lifetime 0
         * (§11.1: "The value 0 indicates 'delete'."). */
        uint8_t del[NATPMP_PCP_MAP_LEN];
        EXPECT_TRUE("22-pcp-del-build",
                    Natpmp_BuildPcpMapRequest(del, nonce, NATPMP_PROTO_UDP, 54321, 0, 0,
                                              (uint32_t)client.s_addr, 0));
        EXPECT_TRUE("22-pcp-del-lifetime0",
                    del[4] == 0 && del[5] == 0 && del[6] == 0 && del[7] == 0);
        EXPECT_TRUE("22-pcp-del-still-map", del[1] == 1);
    }

    {
        /* NAT-PMP public-address request, RFC 6886 §3.2: two bytes,
         * "Vers = 0 | OP = 0". */
        uint8_t a[NATPMP_PMP_ADDR_REQ_LEN];
        EXPECT_TRUE("22-pmp-addr-build", Natpmp_BuildPmpAddrRequest(a));
        EXPECT_TRUE("22-pmp-addr-bytes", a[0] == 0 && a[1] == 0);
        EXPECT_TRUE("22-pmp-addr-len", (int)sizeof(a) == 2);
    }

    {
        /* NAT-PMP mapping request, RFC 6886 §3.3, 12 bytes:
         * Vers=0 | OP=x | Reserved(2) | Internal Port(2) |
         * Suggested External Port(2) | Lifetime(4), network byte order. */
        uint8_t want[NATPMP_PMP_MAP_REQ_LEN];
        memset(want, 0, sizeof(want));
        want[0] = 0;    /* Vers = 0                       */
        want[1] = 1;    /* OP = 1 (Map UDP)               */
        want[4] = 0xD4; want[5] = 0x31; /* Internal 54321 */
        want[6] = 0x9C; want[7] = 0x41; /* Suggested ext 40001 */
        want[8] = 0x00; want[9] = 0x00; want[10] = 0x0E; want[11] = 0x10; /* 3600 */

        uint8_t got[NATPMP_PMP_MAP_REQ_LEN];
        EXPECT_TRUE("22-pmp-map-build",
                    Natpmp_BuildPmpMapRequest(got, NATPMP_PMP_OP_MAP_UDP, 54321, 40001,
                                              3600));
        if (memcmp(got, want, sizeof(want)) != 0) {
            int off = -1;
            for (int i = 0; i < (int)sizeof(want); i++) {
                if (got[i] != want[i]) { off = i; break; }
            }
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 18-pmp-req-bytes: first difference at "
                    "offset %d (got 0x%02X, RFC 6886 §3.3 layout wants 0x%02X)\n",
                    off, off >= 0 ? got[off] : 0, off >= 0 ? want[off] : 0);
            fail_count++;
        }

        /* RFC 6886 §3.4 deletion: lifetime 0 AND "The Suggested External
         * Port MUST be set to zero by the client on sending". The builder
         * must force it even when the caller passes one. */
        uint8_t del[NATPMP_PMP_MAP_REQ_LEN];
        EXPECT_TRUE("22-pmp-del-build",
                    Natpmp_BuildPmpMapRequest(del, NATPMP_PMP_OP_MAP_UDP, 54321, 40001, 0));
        if (del[6] != 0 || del[7] != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 18-pmp-del-extport0: deletion request "
                    "carries Suggested External Port %u; RFC 6886 §3.4 says it MUST be "
                    "zero on sending\n",
                    (unsigned)((del[6] << 8) | del[7]));
            fail_count++;
        }
        EXPECT_TRUE("22-pmp-del-lifetime0",
                    del[8] == 0 && del[9] == 0 && del[10] == 0 && del[11] == 0);
        EXPECT_TRUE("22-pmp-del-internal-kept", del[4] == 0xD4 && del[5] == 0x31);
        /* Opcodes other than 1/2 do not exist on this wire (§3.3). */
        uint8_t junk[NATPMP_PMP_MAP_REQ_LEN];
        EXPECT_FALSE("22-pmp-op7", Natpmp_BuildPmpMapRequest(junk, 7, 1, 1, 60));
    }

    /* ---- PCP response parsing: success, then the reject table ---- */

    uint8_t resp[NATPMP_PCP_MAP_LEN];
    memset(resp, 0, sizeof(resp));
    resp[0] = 2;
    resp[1] = 0x80u | 1u;          /* R = 1, Opcode = MAP        */
    resp[3] = 0;                   /* SUCCESS                    */
    resp[4] = 0; resp[5] = 0; resp[6] = 0x02; resp[7] = 0x58; /* lifetime 600 */
    resp[8] = 0; resp[9] = 0; resp[10] = 0; resp[11] = 0x2A;  /* epoch 42     */
    memcpy(&resp[24], nonce, NATPMP_PCP_NONCE_LEN);
    resp[36] = 17;
    resp[40] = 0xD4; resp[41] = 0x31;  /* internal 54321          */
    resp[42] = 0x9C; resp[43] = 0x41;  /* assigned external 40001 */
    resp[54] = 0xFF; resp[55] = 0xFF;  /* ::ffff:198.51.100.7     */
    resp[56] = 198; resp[57] = 51; resp[58] = 100; resp[59] = 7;

    {
        NatpmpPcpMap m;
        memset(&m, 0xAA, sizeof(m));
        EXPECT_TRUE("22-pcp-parse-ok",
                    Natpmp_ParsePcpMapResponse(resp, (int)sizeof(resp), nonce,
                                               NATPMP_PROTO_UDP, 54321, &m) ==
                        NATPMP_PARSE_OK);
        EXPECT_TRUE("22-pcp-parse-lifetime", m.lifetime_s == 600u);
        EXPECT_TRUE("22-pcp-parse-epoch", m.epoch_s == 42u);
        EXPECT_TRUE("22-pcp-parse-internal", m.internal_port == 54321);
        if (m.external_port != 40001) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 18-pcp-parse-extport: got %u, expected "
                    "40001 (Assigned External Port is big-endian at [42..44), RFC 6887 "
                    "§11.2)\n", (unsigned)m.external_port);
            fail_count++;
        }
        struct in_addr got_ip;
        memcpy(&got_ip.s_addr, &m.external_ip_be, 4);
        char ipbuf[64] = { 0 };
        inet_ntop(AF_INET, &got_ip, ipbuf, sizeof(ipbuf));
        if (strcmp(ipbuf, "198.51.100.7") != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 18-pcp-parse-extip: got %s, expected "
                    "198.51.100.7 (IPv4-mapped IPv6 at [44..60), RFC 6887 §5)\n", ipbuf);
            fail_count++;
        }
    }

    {
        /* A forged or misdirected frame must never become a mapping.
         * Every row below is NOT_OURS — "keep listening", not "refused"
         * — and must leave the output zeroed. */
        struct { const char* tag; int off; uint8_t val; } bad[] = {
            { "22-pcp-rej-version",  0,  3    },  /* not version 2            */
            { "22-pcp-rej-rbit",     1,  0x01 },  /* R clear: it is a request */
            { "22-pcp-rej-opcode",   1,  0x82 },  /* R set, but PEER not MAP  */
            { "22-pcp-rej-nonce",    24, 0x00 },  /* someone else's mapping   */
            { "22-pcp-rej-nonce2",   35, 0x00 },  /* last nonce byte counts   */
            { "22-pcp-rej-protocol", 36, 6    },  /* TCP answer to a UDP ask  */
            { "22-pcp-rej-intport",  40, 0x00 },  /* not the port we asked    */
            { "22-pcp-rej-v4mapped", 50, 0x01 },  /* §5: all 96 bits checked  */
            { "22-pcp-rej-v4mapped2",54, 0xFE },  /* 0xFFFF marker corrupted  */
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            uint8_t b[NATPMP_PCP_MAP_LEN];
            memcpy(b, resp, sizeof(b));
            b[bad[i].off] = bad[i].val;
            NatpmpPcpMap m;
            memset(&m, 0xAA, sizeof(m));
            const NatpmpParse v = Natpmp_ParsePcpMapResponse(b, (int)sizeof(b), nonce,
                                                             NATPMP_PROTO_UDP, 54321, &m);
            if (v != NATPMP_PARSE_NOT_OURS) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: %s: verdict %d, expected "
                        "NATPMP_PARSE_NOT_OURS (%d) — a frame that is not provably an "
                        "answer to our request must never be accepted\n",
                        bad[i].tag, (int)v, (int)NATPMP_PARSE_NOT_OURS);
                fail_count++;
            }
            EXPECT_TRUE("22-pcp-rej-zeroes",
                        m.external_port == 0 && m.external_ip_be == 0 &&
                            m.lifetime_s == 0);
        }
        /* Short frames, including one truncated one byte below the
         * documented 60. */
        NatpmpPcpMap m;
        for (int len = 0; len < NATPMP_PCP_MAP_LEN; len++) {
            uint8_t b[NATPMP_PCP_MAP_LEN];
            memcpy(b, resp, sizeof(b));
            memset(&m, 0xAA, sizeof(m));
            if (Natpmp_ParsePcpMapResponse(b, len, nonce, NATPMP_PROTO_UDP, 54321, &m) ==
                NATPMP_PARSE_OK) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 18-pcp-short: a %d-byte frame parsed "
                        "OK; the PCP MAP response is 60 bytes (RFC 6887 §7.2 + §11.2)\n",
                        len);
                fail_count++;
                break;
            }
        }

        /* An error result for OUR request is REFUSED, not NOT_OURS, and
         * must not carry a port out. */
        uint8_t e[NATPMP_PCP_MAP_LEN];
        memcpy(e, resp, sizeof(e));
        e[3] = (uint8_t)NATPMP_PCP_NO_RESOURCES; /* 8, RFC 6887 §7.4 */
        memset(&m, 0xAA, sizeof(m));
        EXPECT_TRUE("22-pcp-refused",
                    Natpmp_ParsePcpMapResponse(e, (int)sizeof(e), nonce, NATPMP_PROTO_UDP,
                                               54321, &m) == NATPMP_PARSE_REFUSED);
        EXPECT_TRUE("22-pcp-refused-code", m.result_code == 8);
        EXPECT_TRUE("22-pcp-refused-noport", m.external_port == 0);

        /* THE DOWNGRADE SIGNAL. RFC 6886 §3.5's Unsupported Version frame
         * is Vers=0 | OP=0 | Result Code=1 | Epoch — eight bytes, and its
         * OP byte has the PCP R bit CLEAR. RFC 6887 §9 step 4 says a
         * version-zero UNSUPP_VERSION means "this is a NAT-PMP server". */
        uint8_t uv[8];
        memset(uv, 0, sizeof(uv));
        uv[0] = 0; uv[1] = 0; uv[2] = 0; uv[3] = 1; uv[7] = 0x2A;
        memset(&m, 0xAA, sizeof(m));
        const NatpmpParse dv = Natpmp_ParsePcpMapResponse(uv, (int)sizeof(uv), nonce,
                                                          NATPMP_PROTO_UDP, 54321, &m);
        if (dv != NATPMP_PARSE_PCP_IS_NATPMP) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: 18-pcp-downgrade: verdict %d, expected "
                    "NATPMP_PARSE_PCP_IS_NATPMP (%d). RFC 6887 §9 step 4: a version-zero "
                    "UNSUPP_VERSION response IS the NAT-PMP server signal; discarding it "
                    "(e.g. by testing the R bit first) makes the gateway look silent.\n",
                    (int)dv, (int)NATPMP_PARSE_PCP_IS_NATPMP);
            fail_count++;
        }
    }

    /* ---- NAT-PMP response parsing ---- */
    {
        uint8_t mr[NATPMP_PMP_MAP_RESP_LEN];
        memset(mr, 0, sizeof(mr));
        mr[0] = 0;
        mr[1] = 128 + 1;
        mr[7] = 0x2A;                      /* epoch 42                */
        mr[8] = 0xD4; mr[9] = 0x31;        /* internal 54321          */
        mr[10] = 0x9C; mr[11] = 0x41;      /* mapped external 40001   */
        mr[14] = 0x0E; mr[15] = 0x10;      /* lifetime 3600           */

        NatpmpPmpMap m;
        memset(&m, 0xAA, sizeof(m));
        EXPECT_TRUE("22-pmp-parse-ok",
                    Natpmp_ParsePmpMapResponse(mr, (int)sizeof(mr), NATPMP_PMP_OP_MAP_UDP,
                                               54321, &m) == NATPMP_PARSE_OK);
        EXPECT_TRUE("22-pmp-parse-extport", m.external_port == 40001);
        EXPECT_TRUE("22-pmp-parse-lifetime", m.lifetime_s == 3600u);
        EXPECT_TRUE("22-pmp-parse-epoch", m.epoch_s == 42u);

        /* Reject table. */
        struct { const char* tag; int off; uint8_t val; } bad[] = {
            { "22-pmp-rej-version", 0, 1        }, /* Vers must be 0            */
            { "22-pmp-rej-op-req",  1, 1        }, /* a request, not a response */
            { "22-pmp-rej-op-tcp",  1, 128 + 2  }, /* §3.3: 'x' MUST match      */
            { "22-pmp-rej-intport", 8, 0x00     }, /* §3.5 correlator mismatch  */
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            uint8_t b[NATPMP_PMP_MAP_RESP_LEN];
            memcpy(b, mr, sizeof(b));
            b[bad[i].off] = bad[i].val;
            memset(&m, 0xAA, sizeof(m));
            const NatpmpParse v = Natpmp_ParsePmpMapResponse(
                b, (int)sizeof(b), NATPMP_PMP_OP_MAP_UDP, 54321, &m);
            if (v != NATPMP_PARSE_NOT_OURS) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: %s: verdict %d, expected "
                        "NATPMP_PARSE_NOT_OURS (%d)\n",
                        bad[i].tag, (int)v, (int)NATPMP_PARSE_NOT_OURS);
                fail_count++;
            }
            EXPECT_TRUE("22-pmp-rej-zeroes", m.external_port == 0 && m.lifetime_s == 0);
        }
        for (int len = 0; len < NATPMP_PMP_MAP_RESP_LEN; len++) {
            memset(&m, 0xAA, sizeof(m));
            if (Natpmp_ParsePmpMapResponse(mr, len, NATPMP_PMP_OP_MAP_UDP, 54321, &m) ==
                NATPMP_PARSE_OK) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 18-pmp-short: a %d-byte frame parsed "
                        "OK; the NAT-PMP mapping response is 16 bytes (RFC 6886 §3.3)\n",
                        len);
                fail_count++;
                break;
            }
        }

        /* A non-zero result code (RFC 6886 §3.5) is a refusal, and the
         * external port / lifetime must NOT come out with it. */
        for (uint8_t rc = 1; rc <= 5; rc++) {
            uint8_t b[NATPMP_PMP_MAP_RESP_LEN];
            memcpy(b, mr, sizeof(b));
            b[3] = rc;
            memset(&m, 0xAA, sizeof(m));
            const NatpmpParse v = Natpmp_ParsePmpMapResponse(
                b, (int)sizeof(b), NATPMP_PMP_OP_MAP_UDP, 54321, &m);
            if (v != NATPMP_PARSE_REFUSED) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: 18-pmp-refuse-%u: verdict %d, "
                        "expected NATPMP_PARSE_REFUSED (%d)\n",
                        (unsigned)rc, (int)v, (int)NATPMP_PARSE_REFUSED);
                fail_count++;
            }
            EXPECT_TRUE("22-pmp-refuse-code", m.result_code == rc);
            EXPECT_TRUE("22-pmp-refuse-noport",
                        m.external_port == 0 && m.lifetime_s == 0);
        }

        /* Public-address response, RFC 6886 §3.2. */
        uint8_t ar[NATPMP_PMP_ADDR_RESP_LEN];
        memset(ar, 0, sizeof(ar));
        ar[0] = 0; ar[1] = 128 + 0; ar[7] = 0x2A;
        ar[8] = 198; ar[9] = 51; ar[10] = 100; ar[11] = 7;
        NatpmpPmpAddr a;
        memset(&a, 0xAA, sizeof(a));
        EXPECT_TRUE("22-pmp-addr-parse",
                    Natpmp_ParsePmpAddrResponse(ar, (int)sizeof(ar), &a) ==
                        NATPMP_PARSE_OK);
        {
            struct in_addr got_ip;
            memcpy(&got_ip.s_addr, &a.external_ip_be, 4);
            char ipbuf[64] = { 0 };
            inet_ntop(AF_INET, &got_ip, ipbuf, sizeof(ipbuf));
            EXPECT_TRUE("22-pmp-addr-ip", strcmp(ipbuf, "198.51.100.7") == 0);
        }
        ar[1] = 128 + 1; /* a mapping response, not an address response */
        memset(&a, 0xAA, sizeof(a));
        EXPECT_TRUE("22-pmp-addr-rej-op",
                    Natpmp_ParsePmpAddrResponse(ar, (int)sizeof(ar), &a) ==
                        NATPMP_PARSE_NOT_OURS);
        /* §3.2: on a non-zero result "the value of the External IPv4
         * Address field is undefined ... MUST be ignored on reception." */
        ar[1] = 128 + 0;
        ar[3] = 3; /* Network Failure */
        memset(&a, 0xAA, sizeof(a));
        EXPECT_TRUE("22-pmp-addr-refused",
                    Natpmp_ParsePmpAddrResponse(ar, (int)sizeof(ar), &a) ==
                        NATPMP_PARSE_REFUSED);
        EXPECT_TRUE("22-pmp-addr-refused-noip", a.external_ip_be == 0);
    }

    /* ================= (B) the client vs a mock gateway ================ */

    /* On this build's host platform the gateway lookup must report
     * NOTHING when the hook is clear — that is the second guard keeping
     * a test run off the developer's router. On Linux it may legitimately
     * find one, so only the no-hook-set invariant is asserted here. */
    Natpmp_TestHook_SetGateway(NULL, 0);
#if !defined(__linux__)
    {
        UpnpMapping m;
        memset(&m, 0, sizeof(m));
        char gwip[64] = { 0 };
        EXPECT_FALSE("22-no-gateway-off-linux",
                     Natpmp_TestHook_DiscoverGateway(gwip, (int)sizeof(gwip)));
        EXPECT_FALSE("22-addmapping-without-gateway",
                     Natpmp_AddMapping(&m, 54321, 54321, PORTMAP_BACKEND_NONE, 300));
        EXPECT_FALSE("22-addmapping-inactive", m.active);
    }
#endif

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
    DirectP2P_TestHook_LastHandoff(NULL, 0, NULL, NULL, NULL, &n);
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
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, false);
        /* The relay must not be able to win this race — the property under
         * test is punch-leg overlap. It could not anyway (it arms at
         * RACE_RELAY_ARM_MS = 2500 ms and this must finish inside 2000 ms),
         * but saying so explicitly keeps the test's meaning single. */
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_RELAY, true);
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
        bool hrelay = false;
        DirectP2P_TestHook_LastHandoff(hip, (int)sizeof(hip), &hport, &hplayer,
                                       &hrelay, &hcount);

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
        EXPECT_TRUE("19-not-relayed", !hrelay);
        EXPECT_TRUE("19-seed-was-armed", s_r19_seed_arms >= 1);
        EXPECT_TRUE("19-deliver-was-armed", s_r19_deliver_arms >= 1);
        EXPECT_TRUE("19-two-candidates", s_mock_punch_calls == 2);
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
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_RELAY, false);
    mock_server_stop(&ctx, tid, server_sock, server_port);
    s_r19_deliver_ip[0] = '\0';
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Test 20: a punch beats the relay, and never asks for one ---------- */

/*
 * §7.4 promised that "a pair that can punch never reaches this code, so an
 * enabled relay costs a connectable pair nothing". Racing the relay leg
 * against the punch legs would have broken that promise outright: every
 * pair would request a pool port, and a slow-but-working direct link could
 * lose to a fast relay and be silently downgraded to European-VPS ping.
 *
 * RACE_RELAY_ARM_MS restores it. The mock server here is a fully working
 * relay (MOCK_RELAY_OK) that would grant instantly, so the ONLY reason it
 * is never asked is the arm delay; and the ONLY reason the punch endpoint
 * wins is the punch-first ordering rule.
 */
static int s_r20_handoffs_before = 0;
static bool pred_r20_handoff(void) {
    int n = 0;
    DirectP2P_TestHook_LastHandoff(NULL, 0, NULL, NULL, NULL, &n);
    return n > s_r20_handoffs_before;
}

static DirectP2PPunchOracleResult r20_oracle(const char* ip, uint16_t port) {
    (void)ip;
    (void)port;
    s_mock_punch_calls++;
    return DP2P_PUNCH_CONFIRM;
}

static int test_race_punch_beats_relay(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 20: a confirmed punch beats the relay, and a "
            "connectable pair never requests a relay port\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();

    unsigned short server_port = 0, relay_port = 0;
    const int server_sock = open_udp_on_localhost(&server_port);
    const int relay_sock = open_udp_on_localhost(&relay_port);
    if (server_sock < 0 || relay_sock < 0) {
        FAIL("test20", "could not bind the mock server sockets");
        if (server_sock >= 0) close_sock(server_sock);
        if (relay_sock >= 0) close_sock(relay_sock);
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.use_synth_peer = true;
    ctx.synth_peer_ip_be = htonl(0xCB0071F5u); /* 203.0.113.245 */
    ctx.synth_peer_port = 7100;
    ctx.min_cookied_before_peer = 0;
    ctx.life_secs = 30;
    ctx.relay_mode = MOCK_RELAY_OK; /* a relay that WOULD answer instantly */
    ctx.relay_sock = relay_sock;
    ctx.relay_port = relay_port;
    ctx.relay_slot = 1;
    for (int i = 0; i < REND_TOKEN_LEN; i++) {
        ctx.relay_token[i] = (uint8_t)(0xB0u + i);
    }

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_r20", &ctx);
    if (!tid) {
        FAIL("test20", "SDL_CreateThread failed");
        close_sock(server_sock);
        close_sock(relay_sock);
        return 1;
    }

    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, false);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_RELAY, false);
    }
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetPunchOracle(r20_oracle);
    DirectP2P_TestHook_ResetHandoff();
    s_r20_handoffs_before = 0;
    s_mock_punch_calls = 0;

    uint32_t nonce = 0;
    char code[ROOM_CODE_BUF_LEN];
    struct in_addr peer_in;
    if (!RoomCode_GenerateNonce(&nonce) ||
        inet_pton(AF_INET, "198.51.100.7", &peer_in) != 1 ||
        !RoomCode_Encode((uint32_t)peer_in.s_addr, 6000, nonce, code)) {
        FAIL("test20", "could not build a room code");
        rc = 1;
        goto done;
    }

    DirectP2P_BeginJoin(code);
    if (!tick_until(pred_r20_handoff, 20000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test20: no handoff (state=%d relay_reqs=%d)\n",
                (int)DirectP2P_GetState(), ctx.relay_reqs);
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
        /* The punch endpoint, NOT the relay port. */
        if (hport == relay_port) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test20: do_handoff got the RELAY endpoint "
                    "%s:%u — a confirmed punch must outrank the relay\n",
                    hip, (unsigned)hport);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("20-handoff-not-flagged-relay", !hrelay);
        /* And the server was never even ASKED for a relay port: the
         * connectable pair costs the pool nothing (§7.4). */
        if (ctx.relay_reqs != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test20: the server saw %d RELAY_REQ(s); a "
                    "pair that punches inside RACE_RELAY_ARM_MS must never request a relay "
                    "port\n",
                    ctx.relay_reqs);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("20-relay-never-granted", ctx.relay_grants == 0);
    }

    goto after_20a;
done:
    /* Early-exit teardown for 20A. */
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetPunchOracle(NULL);
    mock_server_stop(&ctx, tid, server_sock, server_port);
    close_sock(relay_sock);
    return 1;

after_20a:
    /* --- 20B: the ARM DELAY specifically -----------------------------
     * 20A alone does not isolate RACE_RELAY_ARM_MS: its punch confirms on
     * the first pump, so the "a confirmed punch drops the relay leg" rule
     * would keep relay_reqs at 0 even with the delay removed. Here the
     * punch NEVER confirms and the whole race is bounded to 1500 ms,
     * which is BELOW RACE_RELAY_ARM_MS (2500). The only thing that can
     * keep the request count at zero is the delay itself. */
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    DirectP2P_TestHook_SetPunchOracle(probe_never_punch);
    DirectP2P_TestHook_SetRaceBudgetMs(1500);
    DirectP2P_TestHook_ResetHandoff();
    {
        const int reqs_before = ctx.relay_reqs;
        DirectP2P_BeginJoin(code);
        const uint32_t t0 = SDL_GetTicks();
        bool left_idle = false;
        while ((int)(SDL_GetTicks() - t0) < 20000) {
            const DirectP2PState st = DirectP2P_GetState();
            if (st != DIRECT_P2P_IDLE) left_idle = true;
            if (st == DIRECT_P2P_FAILED_SYMMETRIC || st == DIRECT_P2P_FAILED_BILATERAL ||
                st == DIRECT_P2P_FAILED_STUN || st == DIRECT_P2P_HANDOFF ||
                (left_idle && st == DIRECT_P2P_IDLE)) {
                break;
            }
            SDL_Delay(5);
        }
        if (ctx.relay_reqs != reqs_before) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test20B: %d RELAY_REQ(s) inside a %u ms "
                    "race; the relay leg must not arm before RACE_RELAY_ARM_MS (2500 ms)\n",
                    ctx.relay_reqs - reqs_before, 1500u);
            fail_count++;
            rc = 1;
        }
        /* Sanity: the run really happened (a race that never started would
         * trivially satisfy the assertion above). */
        if (!left_idle) {
            FAIL("test20B", "the orchestrator never left IDLE — nothing was measured");
            rc = 1;
        }
    }
    DirectP2P_TestHook_SetRaceBudgetMs(0);

    if (rc == 0 && fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 20 OK — a confirmed punch won the handoff and "
                "zero RELAY_REQs were sent, against a relay that would have answered "
                "instantly (20A punch-first, 20B arm-delay)\n");
    }
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetPunchOracle(NULL);
    mock_server_stop(&ctx, tid, server_sock, server_port);
    close_sock(relay_sock);
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Test 20C: a punch that lands while the relay leg is IN FLIGHT ----- */

/*
 * 20A and 20B both keep the relay leg from ever arming. 20C is the one
 * whose relay leg DOES arm, arranged by delaying the mock's peer-bearing
 * DELIVER past RACE_RELAY_ARM_MS (2500 ms): the relay's `deliver_real`
 * gate and the DELIVER punch candidate are both satisfied in the same
 * instant, and the punch confirms into an already-armed relay leg.
 *
 * WHAT IT ACTUALLY COVERS (honestly re-scoped by the S6 review, which
 * found the old wording claiming more than the run does):
 *   - the arm delay, measured from the race's own log line — the leg must
 *     report arming at t+>=2500 ms;
 *   - the punch still winning the handoff with a relay leg in flight;
 *   - the reporting rule: abandoning a relay leg because we won is NOT a
 *     relay failure, so the OK line must carry relay_fail=P2P_OK.
 *
 * WHAT IT DOES NOT COVER. On a passing run its relay leg lives ~6 ms,
 * never leaves RELAY_LEG_REQ, never receives a GRANT, and the mock is
 * MOCK_RELAY_NO_ACK — whose own comment says it "cannot win". So it does
 * NOT exercise the TEAR-DOWN in ordering rule 1: deleting
 * `relay_state = RELAY_LEG_DONE;` leaves this test green. Test 26 covers
 * that, with a relay that grants immediately and a real punch leg.
 */
static int s_r20c_handoffs_before = 0;
static bool pred_r20c_handoff(void) {
    int n = 0;
    DirectP2P_TestHook_LastHandoff(NULL, 0, NULL, NULL, NULL, &n);
    return n > s_r20c_handoffs_before;
}

static char s_r20c_deliver_ip[64] = { 0 };
static uint16_t s_r20c_deliver_port = 0;

static DirectP2PPunchOracleResult r20c_oracle(const char* ip, uint16_t port) {
    s_mock_punch_calls++;
    if (s_r20c_deliver_ip[0] != '\0' && port == s_r20c_deliver_port &&
        strcmp(ip, s_r20c_deliver_ip) == 0) {
        return DP2P_PUNCH_CONFIRM;
    }
    return DP2P_PUNCH_NEVER;
}

static int test_race_punch_beats_inflight_relay(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 20C: a punch that lands while the relay leg is "
            "already in flight still wins\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();

    unsigned short server_port = 0, relay_port = 0;
    const int server_sock = open_udp_on_localhost(&server_port);
    const int relay_sock = open_udp_on_localhost(&relay_port);
    if (server_sock < 0 || relay_sock < 0) {
        FAIL("test20C", "could not bind the mock server sockets");
        if (server_sock >= 0) close_sock(server_sock);
        if (relay_sock >= 0) close_sock(relay_sock);
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.use_synth_peer = true;
    ctx.synth_peer_ip_be = htonl(0xCB0071F5u); /* 203.0.113.245 */
    ctx.synth_peer_port = 7100;
    ctx.min_cookied_before_peer = 0;
    ctx.deliver_delay_ms = 3000; /* > RACE_RELAY_ARM_MS (2500) */
    ctx.life_secs = 60;
    ctx.relay_mode = MOCK_RELAY_NO_ACK; /* grants, never acks: cannot win */
    ctx.relay_sock = relay_sock;
    ctx.relay_port = relay_port;
    ctx.relay_slot = 1;
    for (int i = 0; i < REND_TOKEN_LEN; i++) {
        ctx.relay_token[i] = (uint8_t)(0xD0u + i);
    }

    SDL_strlcpy(s_r20c_deliver_ip, "203.0.113.245", sizeof(s_r20c_deliver_ip));
    s_r20c_deliver_port = 7100;
    s_mock_punch_calls = 0;

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_r20c", &ctx);
    if (!tid) {
        FAIL("test20C", "SDL_CreateThread failed");
        close_sock(server_sock);
        close_sock(relay_sock);
        return 1;
    }

    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, false);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_RELAY, false);
        /* Set EXPLICITLY, not inherited: an earlier case in this TU leaves
         * the key at 800 ms, and the H-4 arm cap below is computed from
         * (race_budget - relay_budget), so an inherited value silently
         * changes what this test exercises. */
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RELAY_BUDGET_MS, "4000");
    }
    SDL_GetLogOutputFunction(&s_prev_log_fn, &s_prev_log_ud);
    SDL_SetLogOutputFunction(capture_log_fn, NULL);
    s_log_last_ok[0] = '\0';
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetPunchOracle(r20c_oracle);
    /* S6-review H-4 changed what it takes to get a relay leg armed here.
     * The arm delay is now the LATER of RACE_RELAY_ARM_MS and each live
     * candidate's own RACE_PUNCH_MIN_WINDOW_MS, capped at
     * (race_budget - relay_budget). On the default 8 000 ms budget that
     * cap is 4 000 ms, and this test's DELIVER candidate arms at 3 000 ms
     * and confirms instantly — so the relay would never arm and 20C would
     * degenerate into 20A. A 6 000 ms race budget against the 4 000 ms
     * relay budget set above puts the cap back at RACE_RELAY_ARM_MS
     * (2 500 ms), so the leg arms the moment the delayed DELIVER makes us
     * paired, which is the situation 20C exists to test. */
    DirectP2P_TestHook_SetRaceBudgetMs(6000);
    DirectP2P_TestHook_ResetHandoff();
    s_r20c_handoffs_before = 0;

    uint32_t nonce = 0;
    char code[ROOM_CODE_BUF_LEN];
    struct in_addr peer_in;
    if (!RoomCode_GenerateNonce(&nonce) ||
        inet_pton(AF_INET, "198.51.100.7", &peer_in) != 1 ||
        !RoomCode_Encode((uint32_t)peer_in.s_addr, 6000, nonce, code)) {
        FAIL("test20C", "could not build a room code");
        rc = 1;
        goto done;
    }

    DirectP2P_BeginJoin(code);
    if (!tick_until(pred_r20c_handoff, 25000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test20C: no handoff (state=%d relay_reqs=%d "
                "grants=%d pins_ok=%d)\n",
                (int)DirectP2P_GetState(), ctx.relay_reqs, ctx.relay_grants,
                ctx.relay_pins_ok);
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
        /* The relay leg REALLY armed — otherwise this test degenerates
         * into 20A and proves nothing new. */
        if (ctx.relay_reqs < 1) {
            FAIL("test20C",
                 "the relay leg never armed, so 'a punch beats an IN-FLIGHT relay' was "
                 "never exercised");
            rc = 1;
        }
        /* The S6 review found an empty "NEUTRALISATION CHECK for the
         * arm-delay" comment here with NO CODE UNDER IT. Resolved by
         * DELETING it rather than by writing the check, because the check
         * it described cannot fail in this scenario: 20C's DELIVER is
         * delayed to 3 000 ms BY CONSTRUCTION and the relay's `paired`
         * gate is that DELIVER, so the leg cannot arm before 2 500 ms no
         * matter what the arm rule says. An assertion that cannot fail is
         * worse than none. The arm delay is covered where it CAN fail:
         * 20B (a 1 500 ms race that must see zero RELAY_REQs) and test 29
         * (no punch candidate at all, so RACE_RELAY_ARM_MS is the only
         * thing holding the leg back).
         * ...and the punch still won. */
        if (hport != s_r20c_deliver_port || strcmp(hip, s_r20c_deliver_ip) != 0) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test20C: do_handoff got %s:%u, expected "
                    "the punched DELIVER endpoint %s:%u\n",
                    hip, (unsigned)hport, s_r20c_deliver_ip,
                    (unsigned)s_r20c_deliver_port);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("20C-not-flagged-relay", !hrelay);
        /* Abandoning the relay leg because we won is NOT a relay failure:
         * §7.5 defines relay_fail= as "it ran and failed like this". */
        if (strstr(s_log_last_ok, "relay_fail=P2P_OK") == NULL) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test20C: OK report line does not carry "
                    "relay_fail=P2P_OK — abandoning the relay leg for a won punch is not "
                    "a relay failure: \"%s\"\n",
                    s_log_last_ok);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("20C-ok-line-not-via-relay",
                    strstr(s_log_last_ok, "via_relay=0") != NULL);
        if (rc == 0 && fail_count == fails_before) {
            fprintf(stderr,
                    "[test_bilateral_punch] test 20C OK — %d RELAY_REQ(s) were in flight "
                    "when the punch confirmed, the punch still won, and the OK line "
                    "reports relay_fail=P2P_OK\n",
                    ctx.relay_reqs);
        }
    }

done:
    SDL_SetLogOutputFunction(s_prev_log_fn, s_prev_log_ud);
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetPunchOracle(NULL);
    DirectP2P_TestHook_SetRaceBudgetMs(0);
    mock_server_stop(&ctx, tid, server_sock, server_port);
    close_sock(relay_sock);
    s_r20c_deliver_ip[0] = '\0';
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Test 21: NOT_PAIRED is transient, POOL_EXHAUSTED is terminal ------ */

/*
 * Serially the relay rung ran only after the signalling phase had
 * definitely completed, so any refusal was durable and terminating on it
 * was correct. Racing can now ask while the peer's own REGISTER is still
 * in flight, and a server that answers NOT_PAIRED at that instant is not
 * saying "never" — it is saying "not yet". Treating the first answer as
 * final would INVENT a failure for exactly the symmetric pairs the relay
 * exists to carry.
 *
 * The mock refuses the first `relay_notpaired_first` requests with
 * NOT_PAIRED and grants after that.
 */
static int s_r21_handoffs_before = 0;
static bool pred_r21_handoff(void) {
    int n = 0;
    DirectP2P_TestHook_LastHandoff(NULL, 0, NULL, NULL, NULL, &n);
    return n > s_r21_handoffs_before;
}

static int test_race_not_paired_is_transient(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 21: a NOT_PAIRED relay refusal is transient\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();

    unsigned short server_port = 0, relay_port = 0;
    const int server_sock = open_udp_on_localhost(&server_port);
    const int relay_sock = open_udp_on_localhost(&relay_port);
    if (server_sock < 0 || relay_sock < 0) {
        FAIL("test21", "could not bind the mock server sockets");
        if (server_sock >= 0) close_sock(server_sock);
        if (relay_sock >= 0) close_sock(relay_sock);
        return 1;
    }

    MockServerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sock = server_sock;
    ctx.use_synth_peer = true;
    ctx.synth_peer_ip_be = htonl(0xC6336407u); /* 198.51.100.7 */
    ctx.synth_peer_port = 6000;
    ctx.min_cookied_before_peer = 0;
    ctx.life_secs = 30;
    ctx.relay_mode = MOCK_RELAY_OK;
    ctx.relay_notpaired_first = 2; /* two "not yet" answers, then a grant */
    ctx.relay_sock = relay_sock;
    ctx.relay_port = relay_port;
    ctx.relay_slot = 1;
    for (int i = 0; i < REND_TOKEN_LEN; i++) {
        ctx.relay_token[i] = (uint8_t)(0xC0u + i);
    }

    SDL_Thread* tid = SDL_CreateThread(mock_server_thread, "rend_mock_r21", &ctx);
    if (!tid) {
        FAIL("test21", "SDL_CreateThread failed");
        close_sock(server_sock);
        close_sock(relay_sock);
        return 1;
    }

    {
        char url[64];
        SDL_snprintf(url, sizeof(url), "udp://127.0.0.1:%u", (unsigned)server_port);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL, url);
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RELAY_BUDGET_MS, "3000");
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_NATPMP, true);
        /* Force-relay arms no punch legs and skips the bypasses, so the
         * relay leg is the only runner and its refusal handling is the
         * only thing under test. */
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, true);
        Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_RELAY, false);
    }
    DirectP2P_TestHook_SetSignalBudgetMs(2500);
    DirectP2P_TestHook_SetStunDiscover(mock_stun_discover);
    DirectP2P_TestHook_SetPunchOracle(mock_punch_oracle);
    s_mock_punch_calls = 0;
    s_mock_punch_succeed_from = 0;
    DirectP2P_TestHook_ResetHandoff();
    s_r21_handoffs_before = 0;

    uint32_t nonce = 0;
    char code[ROOM_CODE_BUF_LEN];
    struct in_addr peer_in;
    if (!RoomCode_GenerateNonce(&nonce) ||
        inet_pton(AF_INET, "198.51.100.7", &peer_in) != 1 ||
        !RoomCode_Encode((uint32_t)peer_in.s_addr, 6000, nonce, code)) {
        FAIL("test21", "could not build a room code");
        rc = 1;
        goto done;
    }

    DirectP2P_BeginJoin(code);
    if (!tick_until(pred_r21_handoff, 25000)) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test21: no handoff after two NOT_PAIRED "
                "refusals (state=%d status=\"%s\" relay_reqs=%d grants=%d notpaired=%d "
                "pins_ok=%d acks=%d) — a 'not yet' refusal must not end the leg\n",
                (int)DirectP2P_GetState(), DirectP2P_GetStatusText(), ctx.relay_reqs,
                ctx.relay_grants, ctx.relay_notpaired_sent, ctx.relay_pins_ok,
                ctx.relay_acks);
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
                    "[test_bilateral_punch] FAIL: test21: do_handoff got %s:%u, expected the "
                    "relay endpoint 127.0.0.1:%u\n",
                    hip, (unsigned)hport, (unsigned)relay_port);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("21-handoff-flagged-relay", hrelay);
        /* The refusals really happened — otherwise this test proves
         * nothing about how they are handled. */
        EXPECT_TRUE("21-refusals-were-sent", ctx.relay_notpaired_sent >= 2);
        EXPECT_TRUE("21-kept-asking", ctx.relay_reqs >= 3);
        EXPECT_TRUE("21-eventually-granted", ctx.relay_grants >= 1);
        if (rc == 0 && fail_count == fails_before) {
            fprintf(stderr,
                    "[test_bilateral_punch] test 21 OK — %d NOT_PAIRED refusal(s) absorbed, "
                    "%d RELAY_REQ(s) sent, relay endpoint reached do_handoff\n",
                    ctx.relay_notpaired_sent, ctx.relay_reqs);
        }
    }

done:
    DirectP2P_TestHook_RunTeardown();
    if (DirectP2P_GetState() != DIRECT_P2P_IDLE) DirectP2P_Cancel();
    DirectP2P_TestHook_SetSignalBudgetMs(0);
    DirectP2P_TestHook_SetStunDiscover(NULL);
    DirectP2P_TestHook_SetPunchOracle(NULL);
    Config_SetBool(CFG_KEY_NETPLAY_DIRECT_P2P_FORCE_RELAY, false);
    Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_RELAY_BUDGET_MS, "4000");
    mock_server_stop(&ctx, tid, server_sock, server_port);
    close_sock(relay_sock);
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}


/* ====================================================================== */
/* --- Tests 23-27: the S6 adversarial-review lane ---------------------- */
/* ====================================================================== */

/*
 * WHY THESE EXIST AT ALL (review finding H-2).
 *
 * Across a full pristine run of this suite the line "S6 race: punching
 * candidate" appeared 27 times and "Hole punch SUCCESS" appeared ZERO
 * times. Every candidate in tests 18-21 is oracle-driven, and
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
static int s_sb6_relay_dropped = 0;
static uint32_t s_sb6_relay_dropped_ms = 0;
static int s_sb6_relay_armed = 0;
static uint32_t s_sb6_relay_armed_ms = 0;
static int s_sb6_arm_lines = 0;
static int s_sb6_tail_holds = 0;

static void SDLCALL sb6_log_fn(void* ud, int cat, SDL_LogPriority pri,
                               const char* msg) {
    (void)ud;
    if (msg != NULL) {
        if (strstr(msg, "Hole punch SUCCESS") != NULL) {
            if (s_sb6_punch_success++ == 0) s_sb6_punch_success_ms = SDL_GetTicks();
        }
        if (strstr(msg, "dropping the relay leg") != NULL) {
            if (s_sb6_relay_dropped++ == 0) s_sb6_relay_dropped_ms = SDL_GetTicks();
        }
        if (strstr(msg, "relay leg armed at t+") != NULL) {
            if (s_sb6_relay_armed++ == 0) s_sb6_relay_armed_ms = SDL_GetTicks();
        }
        if (strstr(msg, "S6 race: punching candidate") != NULL) s_sb6_arm_lines++;
        if (strstr(msg, "holding the race open") != NULL) s_sb6_tail_holds++;
    }
    if (s_sb6_prev_fn != NULL) s_sb6_prev_fn(s_sb6_prev_ud, cat, pri, msg);
}

static void sb6_log_begin(void) {
    s_sb6_punch_success = 0;
    s_sb6_punch_success_ms = 0;
    s_sb6_relay_dropped = 0;
    s_sb6_relay_dropped_ms = 0;
    s_sb6_relay_armed = 0;
    s_sb6_relay_armed_ms = 0;
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

/* --- a relay-only rendezvous mock -------------------------------------- */

/*
 * Deliberately NOT MockServerCtx: these cases need per-frame receive
 * TIMESTAMPS (the ordering-rule-1 teardown is only observable as "the pin
 * resends STOPPED"), and a bespoke thread keeps that out of a struct four
 * other stages share.
 *
 * Answers RELAY_REQ with a GRANT on the signal port and — unless
 * `never_ack` — a PIN_ACK carrying peer_pinned=1 on the relay port.
 * Optionally also answers REGISTER with a DELIVER for a fixed endpoint,
 * repeated `deliver_burst` times so the duplicate-endpoint guard has
 * duplicates to guard against.
 */
typedef struct {
    int  sock;          /* signal port  */
    int  relay_sock;    /* relay port   */
    uint16_t relay_port;
    uint8_t  relay_slot;
    uint8_t  relay_token[REND_TOKEN_LEN];
    bool never_ack;
    /* S6-review L-1: answer every RELAY_REQ with NOT_PAIRED ("the server
     * never saw the other side of this room"), which must NOT be reported
     * as POOL_EXHAUSTED's "the relay is full". */
    bool always_not_paired;
    volatile bool stop;
    int  life_secs;

    /* DELIVER behaviour (0 = answer no REGISTERs at all). */
    int      deliver_burst;
    int      deliver_after_ms;
    uint32_t deliver_ip_be;
    uint16_t deliver_port;

    uint32_t started_ms;
    volatile int reqs;
    volatile int grants;
    volatile int pins;
    volatile int registers;
    volatile int delivers;
    volatile uint32_t last_pin_ms;
    volatile uint32_t first_req_ms;
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
        int maxfd = c->sock;
        if (c->relay_sock > 0) {
            FD_SET(c->relay_sock, &rfds);
            if (c->relay_sock > maxfd) maxfd = c->relay_sock;
        }
        struct timeval tv = { 0, 20 * 1000 };
        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        if (c->relay_sock > 0 && FD_ISSET(c->relay_sock, &rfds)) {
            uint8_t rb[128];
            struct sockaddr_in src;
            socklen_t sl = sizeof(src);
            const int n = (int)recvfrom(c->relay_sock, (char*)rb, sizeof(rb), 0,
                                        (struct sockaddr*)&src, &sl);
            if (n >= REND_PIN_LEN && rb[5] == (uint8_t)REND_TYPE_RELAY_PIN &&
                memcmp(&rb[8], c->relay_token, REND_TOKEN_LEN) == 0) {
                c->pins++;
                c->last_pin_ms = SDL_GetTicks();
                if (!c->never_ack) {
                    uint8_t ack[REND_PIN_ACK_LEN];
                    const int al = build_relay_pin_ack(ack, rb[6], true);
                    sendto(c->relay_sock, (const char*)ack, al, 0,
                           (struct sockaddr*)&src, sl);
                }
            }
        }
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

        if (type == REND_TYPE_RELAY_REQ) {
            if (c->reqs++ == 0) c->first_req_ms = SDL_GetTicks();
            uint8_t g[REND_GRANT_LEN];
            const int gl = build_relay_grant(
                g, key, c->always_not_paired ? 0xFFu : c->relay_slot,
                c->always_not_paired ? MOCK_RELAY_STATUS_NOT_PAIRED
                                     : MOCK_RELAY_STATUS_GRANTED,
                c->relay_port, c->relay_token);
            c->grants++;
            sendto(c->sock, (const char*)g, gl, 0, (struct sockaddr*)&src, sl);
            continue;
        }
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
    srv.relay_sock = -1;
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
        cfg.relay_leg = false;
        cfg.relay_budget_ms = 3000;
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

/* --- Test 24: the split brain, with two real peers --------------------- */

/*
 * H-3 (alpha blocker, NEW in S6). Ordering rule 1 is a purely LOCAL
 * decision and the relay handoff ended the race immediately, with no
 * cross-peer arbitration. Punch confirmation between two peers is
 * inherently about one one-way delay apart, so a skew of a few tens of
 * milliseconds is enough for one side to commit to the relay while the
 * other commits to a direct link. GekkoNet resolves inbound packets by
 * source address and the remote is registered once at configure time with
 * no relearn path, so the pair then hangs for the whole
 * CONNECT_TIMEOUT_CONNECTING_MS (15 s) and gives up.
 *
 * Pre-S6 this could not happen from timing alone: the relay rung only ran
 * after the bilateral punch had spent its entire window on BOTH sides.
 *
 * THE RIG. Two real p2p_race instances on two threads, each with its own
 * SDL_net socket, punching each other through a UDP delay line that adds
 * a fixed one-way delay `SB6_OWD_MS` (loopback is otherwise ~0 ms, which
 * would compress the whole effect into one 5 ms loop iteration). Both are
 * HOST-role so the relay-arm rule is identical on both sides and the ONLY
 * asymmetry is the one under test: peer A starts `skew_ms` after peer B.
 *
 * With the pre-fix code the outcomes diverge for
 *      skew ∈ (t_relay_commit − OWD, t_relay_commit)
 * — B's relay commits at ~RACE_RELAY_ARM_MS + one signal round trip while
 * B's own punch is still one one-way delay away from confirming, and A,
 * which started later, still has plenty of budget and punches. The
 * measured width of that band is reported below and in
 * docs/plan-netplay-connection.md §8.9.
 */
#define SB6_OWD_MS 150      /* one-way delay injected between the two peers */
#define SB6_RIG_BUDGET_MS 7000
#define SB6_RIG_RELAY_BUDGET_MS 3000

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
                /* Sent out of sock_b, so peer B sees the source endpoint
                 * it was told to punch — no symmetric-NAT retarget is
                 * involved and the property under test stays single. */
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

/* Run one two-peer race at a given start skew. Returns false only if the
 * rig itself could not be stood up. */
static bool sb6_run_split_brain(int skew_ms, int owd_ms,
                                DirectP2PRaceProbeOutcome* out_a,
                                DirectP2PRaceProbeOutcome* out_b) {
    unsigned short server_port = 0, relay_port = 0, pa = 0, pb = 0;
    const int server_sock = open_udp_on_localhost(&server_port);
    const int relay_sock = open_udp_on_localhost(&relay_port);
    const int sock_a = open_udp_on_localhost(&pa);
    const int sock_b = open_udp_on_localhost(&pb);
    uint16_t port_a = 0, port_b = 0;
    NET_DatagramSocket* net_a = sb6_net_socket(&port_a);
    NET_DatagramSocket* net_b = sb6_net_socket(&port_b);
    if (server_sock < 0 || relay_sock < 0 || sock_a < 0 || sock_b < 0 ||
        net_a == NULL || net_b == NULL) {
        if (server_sock >= 0) close_sock(server_sock);
        if (relay_sock >= 0) close_sock(relay_sock);
        if (sock_a >= 0) close_sock(sock_a);
        if (sock_b >= 0) close_sock(sock_b);
        if (net_a != NULL) NET_DestroyDatagramSocket(net_a);
        if (net_b != NULL) NET_DestroyDatagramSocket(net_b);
        return false;
    }

    Sb6ServerCtx srv;
    memset(&srv, 0, sizeof(srv));
    srv.sock = server_sock;
    srv.relay_sock = relay_sock;
    srv.relay_port = relay_port;
    srv.relay_slot = 1;
    srv.life_secs = 60;
    for (int i = 0; i < REND_TOKEN_LEN; i++) srv.relay_token[i] = (uint8_t)(0xE0u + i);

    DelayLineCtx* line = (DelayLineCtx*)SDL_calloc(1, sizeof(DelayLineCtx));
    if (line == NULL) {
        close_sock(server_sock);
        close_sock(relay_sock);
        close_sock(sock_a);
        close_sock(sock_b);
        NET_DestroyDatagramSocket(net_a);
        NET_DestroyDatagramSocket(net_b);
        return false;
    }
    line->sock_a = sock_a;
    line->sock_b = sock_b;
    line->delay_ms = owd_ms;

    SDL_Thread* srv_tid = SDL_CreateThread(sb6_server_thread, "sb6_srv24", &srv);
    SDL_Thread* line_tid = SDL_CreateThread(sb6_delay_line_thread, "sb6_line", line);

    Sb6PeerCtx a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.start_delay_ms = skew_ms;
    b.start_delay_ms = 0;

    a.cfg.host_role = true;
    a.cfg.sock = net_a;
    a.cfg.punch_token = k_sb6_token;
    a.cfg.seed_ip = "127.0.0.1";
    a.cfg.seed_port = (uint16_t)pa;   /* A punches the line's A side */
    a.cfg.signal_ip = "127.0.0.1";
    a.cfg.signal_port = (uint16_t)server_port;
    a.cfg.session_key = k_sb6_key;
    a.cfg.my_public_port = port_a;
    a.cfg.signal_leg = false;
    a.cfg.relay_leg = true;
    a.cfg.relay_budget_ms = SB6_RIG_RELAY_BUDGET_MS;
    a.cfg.punch_leg_ms = 5000;
    a.cfg.race_budget_ms = SB6_RIG_BUDGET_MS;

    b.cfg = a.cfg;
    b.cfg.sock = net_b;
    b.cfg.seed_port = (uint16_t)pb;   /* B punches the line's B side */
    b.cfg.my_public_port = port_b;

    SDL_Thread* ta = SDL_CreateThread(sb6_peer_thread, "sb6_peer_a", &a);
    SDL_Thread* tb = SDL_CreateThread(sb6_peer_thread, "sb6_peer_b", &b);
    if (ta != NULL) SDL_WaitThread(ta, NULL);
    if (tb != NULL) SDL_WaitThread(tb, NULL);

    srv.stop = true;
    line->stop = true;
    if (srv_tid != NULL) SDL_WaitThread(srv_tid, NULL);
    if (line_tid != NULL) SDL_WaitThread(line_tid, NULL);

    *out_a = a.out.outcome;
    *out_b = b.out.outcome;

    SDL_free(line);
    close_sock(server_sock);
    close_sock(relay_sock);
    close_sock(sock_a);
    close_sock(sock_b);
    NET_DestroyDatagramSocket(net_a);
    NET_DestroyDatagramSocket(net_b);
    return true;
}

static const char* sb6_outcome_name(DirectP2PRaceProbeOutcome o) {
    switch (o) {
    case DP2P_RACE_PROBE_PUNCHED:   return "PUNCHED";
    case DP2P_RACE_PROBE_RELAYED:   return "RELAYED";
    case DP2P_RACE_PROBE_CANCELLED: return "CANCELLED";
    default:                        return "EXHAUSTED";
    }
}

/* The skew that lands inside the pre-fix divergence band. Derived from
 * the measurement recorded in §8.9: the band is
 * (relay_commit − SB6_OWD_MS, relay_commit) and relay_commit on this rig
 * is RACE_RELAY_ARM_MS + ~10 ms of loopback signalling. */
#define SB6_SPLIT_SKEW_MS 2450
/* A control point comfortably below the band: both sides must punch. */
#define SB6_SAFE_SKEW_MS 1000

static int test_race_split_brain(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 24: two real peers, %d ms one-way delay — "
            "neither may end up on a different rung from the other (H-3)\n",
            SB6_OWD_MS);
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetPunchOracle(NULL); /* REAL legs on both peers */

    /* Optional sweep: used to MEASURE the divergence band. Off by default
     * because it costs ~25 races; the two fixed points below are what the
     * suite runs. */
    const char* sweep = SDL_getenv("S6_SPLIT_SWEEP");
    if (sweep != NULL && sweep[0] == '1') {
        for (int skew = 2150; skew <= 2750; skew += 25) {
            DirectP2PRaceProbeOutcome oa = DP2P_RACE_PROBE_EXHAUSTED;
            DirectP2PRaceProbeOutcome ob = DP2P_RACE_PROBE_EXHAUSTED;
            if (!sb6_run_split_brain(skew, SB6_OWD_MS, &oa, &ob)) break;
            fprintf(stderr, "[split-sweep] skew=%4d ms  A=%-9s B=%-9s  %s\n",
                    skew, sb6_outcome_name(oa), sb6_outcome_name(ob),
                    (oa == ob) ? "converged" : "*** SPLIT BRAIN ***");
        }
    }

    {
        DirectP2PRaceProbeOutcome oa = DP2P_RACE_PROBE_EXHAUSTED;
        DirectP2PRaceProbeOutcome ob = DP2P_RACE_PROBE_EXHAUSTED;
        if (!sb6_run_split_brain(SB6_SPLIT_SKEW_MS, SB6_OWD_MS, &oa, &ob)) {
            FAIL("test24", "could not stand up the two-peer rig");
            rc = 1;
            goto done;
        }
        if (oa != ob) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test24: SPLIT BRAIN at skew=%d ms — "
                    "peer A ended %s and peer B ended %s. The two peers are now on "
                    "different rungs; GekkoNet registers the remote once by source "
                    "address with no relearn path, so this pair hangs for the whole "
                    "%u ms CONNECT_TIMEOUT_CONNECTING_MS and fails\n",
                    SB6_SPLIT_SKEW_MS, sb6_outcome_name(oa), sb6_outcome_name(ob),
                    (unsigned)CONNECT_TIMEOUT_CONNECTING_MS);
            fail_count++;
            rc = 1;
        }
        if (oa != DP2P_RACE_PROBE_PUNCHED) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test24: at skew=%d ms both peers can "
                    "still punch, so the converged outcome must be PUNCHED, not %s — a "
                    "relayed pair pays European-VPS ping for the whole match\n",
                    SB6_SPLIT_SKEW_MS, sb6_outcome_name(oa));
            fail_count++;
            rc = 1;
        }
        fprintf(stderr, "[test_bilateral_punch] test24: skew=%d ms -> A=%s B=%s\n",
                SB6_SPLIT_SKEW_MS, sb6_outcome_name(oa), sb6_outcome_name(ob));
    }

    {
        /* Control: a skew nowhere near the relay-arm boundary must punch
         * on both sides on the pre-fix code too. If THIS one ever goes
         * red the rig is broken, not the fix. */
        DirectP2PRaceProbeOutcome oa = DP2P_RACE_PROBE_EXHAUSTED;
        DirectP2PRaceProbeOutcome ob = DP2P_RACE_PROBE_EXHAUSTED;
        if (!sb6_run_split_brain(SB6_SAFE_SKEW_MS, SB6_OWD_MS, &oa, &ob)) {
            FAIL("test24", "could not stand up the two-peer rig (control)");
            rc = 1;
            goto done;
        }
        if (oa != DP2P_RACE_PROBE_PUNCHED || ob != DP2P_RACE_PROBE_PUNCHED) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test24 control: skew=%d ms gave A=%s "
                    "B=%s, expected both PUNCHED — the rig itself is not delivering "
                    "punches\n",
                    SB6_SAFE_SKEW_MS, sb6_outcome_name(oa), sb6_outcome_name(ob));
            fail_count++;
            rc = 1;
        }
    }

    if (rc == 0 && fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 24 OK — two real peers converged on the "
                "same rung at both the boundary skew (%d ms) and the control (%d ms)\n",
                SB6_SPLIT_SKEW_MS, SB6_SAFE_SKEW_MS);
    }

done:
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Test 25: H-4, each candidate gets its own punch window ------------ */

/*
 * H-4 (alpha blocker). RACE_RELAY_ARM_MS was measured from RACE START,
 * not from when each candidate armed. §8.4 rule 2 justifies 2 500 ms as
 * "exactly the window the pre-S6 direct punch had entirely to itself" —
 * true of punch[0], and false of the DELIVER candidate, which arms at
 * DELIVER time D and pre-S6 got its OWN full BILATERAL_PUNCH_MS window.
 * With D = 1 200 ms the DELIVER candidate got ~1.3 s instead of 5 s, so a
 * US-US pair that needs ~2 s from DELIVER was diverted onto a European
 * relay it did not need.
 *
 * The rig puts the DELIVER at 1 200 ms and has the echo peer confirm
 * 2 000 ms after the first punch — i.e. ~3 200 ms into the race, which is
 * AFTER the old wall-clock arm point (2 500 ms) and before the
 * per-candidate one. The relay would grant and pin instantly, so the only
 * thing that can keep the punch alive is the per-candidate window.
 */
#define SB6_H4_DELIVER_MS 1200
#define SB6_H4_ECHO_DELAY_MS 2000

static int test_race_relay_defers_per_candidate(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 25: a DELIVER candidate keeps its own punch "
            "window and is not stolen by the relay (H-4)\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetPunchOracle(NULL);

    unsigned short echo_port = 0, server_port = 0, relay_port = 0;
    const int echo_sock = open_udp_on_localhost(&echo_port);
    const int server_sock = open_udp_on_localhost(&server_port);
    const int relay_sock = open_udp_on_localhost(&relay_port);
    uint16_t my_port = 0;
    NET_DatagramSocket* sock = sb6_net_socket(&my_port);
    SDL_Thread* echo_tid = NULL;
    SDL_Thread* srv_tid = NULL;
    PunchEchoCtx echo;
    Sb6ServerCtx srv;
    memset(&echo, 0, sizeof(echo));
    memset(&srv, 0, sizeof(srv));
    if (echo_sock < 0 || server_sock < 0 || relay_sock < 0 || sock == NULL) {
        FAIL("test25", "could not bind the rig sockets");
        rc = 1;
        goto done;
    }

    memset(&echo, 0, sizeof(echo));
    echo.sock = echo_sock;
    echo.delay_ms = SB6_H4_ECHO_DELAY_MS;
    echo_tid = SDL_CreateThread(punch_echo_thread, "sb6_echo25", &echo);

    memset(&srv, 0, sizeof(srv));
    srv.sock = server_sock;
    srv.relay_sock = relay_sock;
    srv.relay_port = relay_port;
    srv.relay_slot = 1;
    srv.life_secs = 30;
    srv.deliver_burst = 1;
    srv.deliver_after_ms = SB6_H4_DELIVER_MS;
    srv.deliver_ip_be = htonl(0x7F000001u);
    srv.deliver_port = echo_port;
    for (int i = 0; i < REND_TOKEN_LEN; i++) srv.relay_token[i] = (uint8_t)(0xA0u + i);
    srv_tid = SDL_CreateThread(sb6_server_thread, "sb6_srv25", &srv);

    if (echo_tid == NULL || srv_tid == NULL) {
        FAIL("test25", "SDL_CreateThread failed");
        rc = 1;
        goto done;
    }

    sb6_log_begin();
    {
        DirectP2PRaceProbeCfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.host_role = false; /* joiner: the relay also needs a real DELIVER */
        cfg.sock = sock;
        cfg.punch_token = k_sb6_token;
        cfg.seed_ip = "127.0.0.1";
        cfg.seed_port = 0; /* no seed leg: the DELIVER candidate is the subject */
        cfg.signal_ip = "127.0.0.1";
        cfg.signal_port = (uint16_t)server_port;
        cfg.session_key = k_sb6_key;
        cfg.my_public_port = my_port;
        cfg.signal_leg = true;
        cfg.signal_budget_ms = 6000;
        cfg.relay_leg = true;
        cfg.relay_budget_ms = 3000;
        cfg.punch_leg_ms = 5000;
        cfg.race_budget_ms = 8000;

        DirectP2PRaceProbeOut out;
        DirectP2P_TestHook_RunRace(&cfg, &out);
        sb6_log_end();

        if (out.outcome != DP2P_RACE_PROBE_PUNCHED ||
            out.peer_port != echo_port) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test25: outcome=%s peer=%s:%u after "
                    "%u ms — the DELIVER candidate armed at t+%d ms confirms at ~t+%d ms "
                    "and must win; the relay (which would grant instantly) must not "
                    "steal it. relay_reqs=%d\n",
                    sb6_outcome_name(out.outcome), out.peer_ip, (unsigned)out.peer_port,
                    (unsigned)out.t_race_ms, SB6_H4_DELIVER_MS,
                    SB6_H4_DELIVER_MS + SB6_H4_ECHO_DELAY_MS, srv.reqs);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("25-real-punch-confirmed", s_sb6_punch_success >= 1);
        if (rc == 0 && fail_count == fails_before) {
            fprintf(stderr,
                    "[test_bilateral_punch] test 25 OK — DELIVER candidate armed at "
                    "t+%d ms won at t+%u ms; the relay leg sent %d RELAY_REQ(s) and lost\n",
                    SB6_H4_DELIVER_MS, (unsigned)out.t_race_ms, srv.reqs);
        }
    }

done:
    echo.stop = true;
    srv.stop = true;
    if (echo_tid != NULL) SDL_WaitThread(echo_tid, NULL);
    if (srv_tid != NULL) SDL_WaitThread(srv_tid, NULL);
    if (echo_sock >= 0) close_sock(echo_sock);
    if (server_sock >= 0) close_sock(server_sock);
    if (relay_sock >= 0) close_sock(relay_sock);
    if (sock != NULL) NET_DestroyDatagramSocket(sock);
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
}

/* --- Test 26: ordering rule 1 really TEARS THE RELAY LEG DOWN ---------- */

/*
 * H-7. `grep -c "dropping the relay leg"` over a full pristine run was 0,
 * and deleting the tear-down action itself —
 *     relay_state = RELAY_LEG_DONE;
 * in the ordering-rule-1 block — left the suite fully green. Test 20C
 * does not cover it: its relay leg lived 6 ms, never left RELAY_LEG_REQ,
 * never got a GRANT, and its mock is MOCK_RELAY_NO_ACK, whose own comment
 * says it "cannot win".
 *
 * The mechanism is only observable as an ABSENCE: once a punch confirms,
 * the relay leg must stop costing the pool anything. So the relay here
 * GRANTS immediately (the leg reaches RELAY_LEG_PIN and starts resending
 * RELAY_PIN every RELAY_PIN_RESEND_MS = 150 ms) but never ACKs, and the
 * mock records the arrival time of the last pin it saw. A real punch then
 * confirms mid-flight. With the tear-down the pins stop at the confirm;
 * without it they keep coming for the whole 600 ms confirmation tail.
 */
#define SB6_H7_ECHO_DELAY_MS 2900

static int test_race_punch_drops_relay_leg(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 26: a confirmed punch TEARS DOWN an in-flight, "
            "granted relay leg (H-7)\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetPunchOracle(NULL);

    unsigned short echo_port = 0, server_port = 0, relay_port = 0;
    const int echo_sock = open_udp_on_localhost(&echo_port);
    const int server_sock = open_udp_on_localhost(&server_port);
    const int relay_sock = open_udp_on_localhost(&relay_port);
    uint16_t my_port = 0;
    NET_DatagramSocket* sock = sb6_net_socket(&my_port);
    SDL_Thread* echo_tid = NULL;
    SDL_Thread* srv_tid = NULL;
    PunchEchoCtx echo;
    Sb6ServerCtx srv;
    memset(&echo, 0, sizeof(echo));
    memset(&srv, 0, sizeof(srv));
    if (echo_sock < 0 || server_sock < 0 || relay_sock < 0 || sock == NULL) {
        FAIL("test26", "could not bind the rig sockets");
        rc = 1;
        goto done;
    }

    memset(&echo, 0, sizeof(echo));
    echo.sock = echo_sock;
    echo.delay_ms = SB6_H7_ECHO_DELAY_MS;
    echo_tid = SDL_CreateThread(punch_echo_thread, "sb6_echo26", &echo);

    memset(&srv, 0, sizeof(srv));
    srv.sock = server_sock;
    srv.relay_sock = relay_sock;
    srv.relay_port = relay_port;
    srv.relay_slot = 1;
    srv.never_ack = true; /* grants, then never acks: it cannot win on its own */
    srv.life_secs = 30;
    for (int i = 0; i < REND_TOKEN_LEN; i++) srv.relay_token[i] = (uint8_t)(0xB8u + i);
    srv_tid = SDL_CreateThread(sb6_server_thread, "sb6_srv26", &srv);

    if (echo_tid == NULL || srv_tid == NULL) {
        FAIL("test26", "SDL_CreateThread failed");
        rc = 1;
        goto done;
    }

    sb6_log_begin();
    {
        DirectP2PRaceProbeCfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.host_role = true; /* host: the relay arms without needing a DELIVER */
        cfg.sock = sock;
        cfg.punch_token = k_sb6_token;
        cfg.seed_ip = "127.0.0.1";
        cfg.seed_port = echo_port; /* the seed IS the echo peer */
        cfg.signal_ip = "127.0.0.1";
        cfg.signal_port = (uint16_t)server_port;
        cfg.session_key = k_sb6_key;
        cfg.my_public_port = my_port;
        cfg.signal_leg = false;
        cfg.relay_leg = true;
        cfg.relay_budget_ms = 4000;
        cfg.punch_leg_ms = 6000;
        cfg.race_budget_ms = 8000;

        DirectP2PRaceProbeOut out;
        DirectP2P_TestHook_RunRace(&cfg, &out);
        sb6_log_end();

        /* Preconditions — without these the test would be green for
         * reasons that have nothing to do with the tear-down. */
        if (srv.grants < 1 || srv.pins < 2) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test26: the relay leg never reached a "
                    "resending PIN state (grants=%d pins=%d); 'a punch tears down an "
                    "IN-FLIGHT relay leg' was not exercised\n",
                    srv.grants, srv.pins);
            fail_count++;
            rc = 1;
        }
        if (s_sb6_punch_success < 1 || s_sb6_relay_dropped < 1) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test26: punch_success=%d relay_dropped=%d "
                    "— expected a real confirm AND the ordering-rule-1 tear-down line\n",
                    s_sb6_punch_success, s_sb6_relay_dropped);
            fail_count++;
            rc = 1;
        }
        EXPECT_TRUE("26-punch-won", out.outcome == DP2P_RACE_PROBE_PUNCHED);

        /* THE assertion. RELAY_PIN_RESEND_MS is 150 ms and the tail is
         * 600 ms, so a leg that was NOT torn down sends ~4 more pins after
         * the confirm. One in-flight pin of slack, no more. */
        if (rc == 0) {
            const int since_drop = (int)(srv.last_pin_ms - s_sb6_relay_dropped_ms);
            if (since_drop > 200) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: test26: the relay port was still "
                        "being pinned %d ms after the punch confirmed (tear-down logged at "
                        "t=%u, last pin at t=%u, %d pins total). Ordering rule 1 must set "
                        "relay_state = RELAY_LEG_DONE, not merely log that it did\n",
                        since_drop, (unsigned)s_sb6_relay_dropped_ms,
                        (unsigned)srv.last_pin_ms, srv.pins);
                fail_count++;
                rc = 1;
            } else if (fail_count == fails_before) {
                fprintf(stderr,
                        "[test_bilateral_punch] test 26 OK — relay granted and pinned %d "
                        "times, punch confirmed, last pin %d ms after the tear-down "
                        "(tail is %d ms)\n",
                        srv.pins, since_drop, STUN_PUNCH_CONFIRM_MS);
            }
        }
    }

done:
    echo.stop = true;
    srv.stop = true;
    if (echo_tid != NULL) SDL_WaitThread(echo_tid, NULL);
    if (srv_tid != NULL) SDL_WaitThread(srv_tid, NULL);
    if (echo_sock >= 0) close_sock(echo_sock);
    if (server_sock >= 0) close_sock(server_sock);
    if (relay_sock >= 0) close_sock(relay_sock);
    if (sock != NULL) NET_DestroyDatagramSocket(sock);
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
    srv.relay_sock = -1;
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
        cfg.relay_leg = false;
        cfg.relay_budget_ms = 3000;
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
static int test_race_budget_wrap_safety(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 28: the race deadline survives the "
            "SDL_GetTicks 32-bit wrap (M-1), and the confirmation tail is pinned "
            "(M-3)\n");
    const int fails_before = fail_count;

    /* M-3: pinned by LITERAL. H-1's hard cap is budget + this value, and
     * the punch's own confirmation burst is sized by it. */
    if (STUN_PUNCH_CONFIRM_MS != 600) {
        fprintf(stderr,
                "[test_bilateral_punch] FAIL: test28: STUN_PUNCH_CONFIRM_MS is %d, "
                "expected the shipped literal 600 — the post-confirmation burst is what "
                "gets the peer its last datagram, and H-1's budget exemption is sized by "
                "it. Changing it is a deliberate act, not a refactor\n",
                (int)STUN_PUNCH_CONFIRM_MS);
        fail_count++;
    }

    {
        const int budget = 8000;
        /* t0 sits 256 ms before the uint32 wrap, so t0 + budget overflows
         * to 7744 — a number `now` is already far past. */
        const uint32_t t0 = 0xFFFFFF00u;

        EXPECT_FALSE("28-wrap-first-iteration",
                     DirectP2P_TestHook_RaceBudgetExpired(t0, t0, budget, false));
        EXPECT_FALSE("28-wrap-1ms-in",
                     DirectP2P_TestHook_RaceBudgetExpired(t0 + 1u, t0, budget, false));
        EXPECT_FALSE("28-wrap-just-inside",
                     DirectP2P_TestHook_RaceBudgetExpired(t0 + 7999u, t0, budget, false));
        EXPECT_TRUE("28-wrap-at-budget",
                    DirectP2P_TestHook_RaceBudgetExpired(t0 + 8000u, t0, budget, false));

        /* And the same at a t0 nowhere near the wrap, so the fix is not
         * "always false". */
        const uint32_t mid = 1000000u;
        EXPECT_FALSE("28-mid-just-inside",
                     DirectP2P_TestHook_RaceBudgetExpired(mid + 7999u, mid, budget, false));
        EXPECT_TRUE("28-mid-at-budget",
                    DirectP2P_TestHook_RaceBudgetExpired(mid + 8000u, mid, budget, false));

        /* H-1's exemption: hard-bounded at budget + one tail, across the
         * wrap as well. */
        EXPECT_FALSE("28-tail-holds-at-budget",
                     DirectP2P_TestHook_RaceBudgetExpired(t0 + 8000u, t0, budget, true));
        EXPECT_FALSE("28-tail-holds-just-inside",
                     DirectP2P_TestHook_RaceBudgetExpired(
                         t0 + (uint32_t)(8000 + STUN_PUNCH_CONFIRM_MS - 1), t0, budget, true));
        EXPECT_TRUE("28-tail-is-hard-capped",
                    DirectP2P_TestHook_RaceBudgetExpired(
                        t0 + (uint32_t)(8000 + STUN_PUNCH_CONFIRM_MS), t0, budget, true));
    }

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_bilateral_punch] test 28 OK — the deadline is wrap-safe at "
                "t0=0xFFFFFF00, the tail exemption is hard-capped at budget+%d ms, and "
                "STUN_PUNCH_CONFIRM_MS is pinned to 600\n",
                STUN_PUNCH_CONFIRM_MS);
        return 0;
    }
    return 1;
}


/* --- Test 29: NOT_PAIRED is not "the relay is full" -------------------- */

/*
 * L-1. A RELAY_GRANT carrying NOT_PAIRED was remembered as
 * CONNECT_FAIL_RELAY_REFUSED, whose log line says "the relay port pool is
 * exhausted" and whose user string is "Relay is full. Try again shortly."
 * Two entirely different causes with two different actions — "wait for
 * capacity" versus "your opponent never reached the rendezvous server" —
 * collapsed into one misleading message, which is exactly the failure
 * mode S3 exists to remove.
 *
 * The mock refuses EVERY request with NOT_PAIRED, so the relay leg spends
 * its whole GRANT phase holding a transient refusal that never resolves
 * (§8.4 rule 4 keeps asking; it is only reported when the phase runs
 * out). The leg's own verdict must name that cause.
 */
static int test_relay_not_paired_named_separately(void) {
    fprintf(stderr,
            "[test_bilateral_punch] test 29: a persistent NOT_PAIRED reports its own "
            "cause, not \"the relay pool is exhausted\" (L-1)\n");
    const int fails_before = fail_count;
    int rc = 0;

    NET_Init();
    DirectP2P_Init();
    DirectP2P_TestHook_RunTeardown();
    DirectP2P_TestHook_SetPunchOracle(NULL);

    unsigned short server_port = 0, relay_port = 0;
    const int server_sock = open_udp_on_localhost(&server_port);
    const int relay_sock = open_udp_on_localhost(&relay_port);
    uint16_t my_port = 0;
    NET_DatagramSocket* sock = sb6_net_socket(&my_port);
    SDL_Thread* srv_tid = NULL;
    Sb6ServerCtx srv;
    memset(&srv, 0, sizeof(srv));
    if (server_sock < 0 || relay_sock < 0 || sock == NULL) {
        FAIL("test29", "could not bind the rig sockets");
        rc = 1;
        goto done;
    }

    srv.sock = server_sock;
    srv.relay_sock = relay_sock;
    srv.relay_port = relay_port;
    srv.relay_slot = 1;
    srv.always_not_paired = true;
    srv.life_secs = 30;
    for (int i = 0; i < REND_TOKEN_LEN; i++) srv.relay_token[i] = (uint8_t)(0x70u + i);
    srv_tid = SDL_CreateThread(sb6_server_thread, "sb6_srv29", &srv);
    if (srv_tid == NULL) {
        FAIL("test29", "SDL_CreateThread failed");
        rc = 1;
        goto done;
    }

    sb6_log_begin();
    {
        const uint32_t t_begin = SDL_GetTicks();
        DirectP2PRaceProbeCfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.host_role = true;   /* paired by construction: the relay arms */
        cfg.sock = sock;
        cfg.punch_token = k_sb6_token;
        cfg.seed_ip = "127.0.0.1";
        cfg.seed_port = 0;      /* no punch leg at all */
        cfg.signal_ip = "127.0.0.1";
        cfg.signal_port = (uint16_t)server_port;
        cfg.session_key = k_sb6_key;
        cfg.my_public_port = my_port;
        cfg.signal_leg = false;
        cfg.relay_leg = true;
        cfg.relay_budget_ms = 2000;
        cfg.punch_leg_ms = 5000;
        cfg.race_budget_ms = 6000;

        DirectP2PRaceProbeOut out;
        DirectP2P_TestHook_RunRace(&cfg, &out);
        sb6_log_end();

        /* This is also the ONE place RACE_RELAY_ARM_MS can be isolated.
         * Everywhere else a punch candidate is armed at t0, so the H-4
         * per-candidate window (also 2 500 ms) binds first and hides it.
         * Here there is no punch candidate at all — seed_port is 0 and
         * there is no signalling leg — so the arm delay is the only thing
         * holding the relay leg back. */
        if (s_sb6_relay_armed < 1) {
            FAIL("test29", "the relay leg never armed, so nothing was measured");
            rc = 1;
        } else {
            const int armed_at = (int)(s_sb6_relay_armed_ms - t_begin);
            if (armed_at < 2400) {
                fprintf(stderr,
                        "[test_bilateral_punch] FAIL: test29: the relay leg armed at "
                        "t+%d ms with no punch candidate in the race; it must not arm "
                        "before RACE_RELAY_ARM_MS (2500 ms)\n", armed_at);
                fail_count++;
                rc = 1;
            }
        }

        EXPECT_TRUE("29-relay-really-ran", out.relay_ran);
        EXPECT_TRUE("29-server-was-asked", srv.reqs >= 2);
        if (out.relay_fail != (int)CONNECT_FAIL_RELAY_NOT_PAIRED) {
            fprintf(stderr,
                    "[test_bilateral_punch] FAIL: test29: relay_fail=%s after %d "
                    "NOT_PAIRED refusal(s), expected %s. %s reads \"%s\" to the user and "
                    "\"the relay port pool is exhausted\" in the log, which is a "
                    "different cause with a different action\n",
                    ConnectFail_Code((ConnectFailCode)out.relay_fail), srv.grants,
                    ConnectFail_Code(CONNECT_FAIL_RELAY_NOT_PAIRED),
                    ConnectFail_Code(CONNECT_FAIL_RELAY_REFUSED),
                    ConnectFail_UserText(CONNECT_FAIL_RELAY_REFUSED));
            fail_count++;
            rc = 1;
        }
        /* Distinct in BOTH surfaces, or the split buys nothing. */
        EXPECT_TRUE("29-machine-codes-differ",
                    strcmp(ConnectFail_Code(CONNECT_FAIL_RELAY_NOT_PAIRED),
                           ConnectFail_Code(CONNECT_FAIL_RELAY_REFUSED)) != 0);
        EXPECT_TRUE("29-user-strings-differ",
                    strcmp(ConnectFail_UserText(CONNECT_FAIL_RELAY_NOT_PAIRED),
                           ConnectFail_UserText(CONNECT_FAIL_RELAY_REFUSED)) != 0);
        if (rc == 0 && fail_count == fails_before) {
            fprintf(stderr,
                    "[test_bilateral_punch] test 29 OK — %d NOT_PAIRED refusal(s) "
                    "reported as %s (\"%s\")\n",
                    srv.grants, ConnectFail_Code(CONNECT_FAIL_RELAY_NOT_PAIRED),
                    ConnectFail_UserText(CONNECT_FAIL_RELAY_NOT_PAIRED));
        }
    }

done:
    srv.stop = true;
    if (srv_tid != NULL) SDL_WaitThread(srv_tid, NULL);
    if (server_sock >= 0) close_sock(server_sock);
    if (relay_sock >= 0) close_sock(relay_sock);
    if (sock != NULL) NET_DestroyDatagramSocket(sock);
    return (rc == 0 && fail_count == fails_before) ? 0 : 1;
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
    rc |= test_relay_codec(); /* S5 */
    rc |= test_relay_rung();  /* S5 */
    rc |= test_race_deliver_overlaps_seed();  /* S6 */
    rc |= test_race_punch_beats_relay();      /* S6 */
    rc |= test_race_punch_beats_inflight_relay(); /* S6 */
    rc |= test_race_not_paired_is_transient();/* S6 */
    rc |= test_race_worst_case_timing();      /* S6: ~16 s of wall clock */
    /* S6 adversarial review: real-wire punch legs, two-peer split brain. */
    rc |= test_race_budget_wrap_safety();          /* M-1 / M-3 */
    rc |= test_race_duplicate_candidate_guard();   /* M-2 */
    rc |= test_race_confirm_at_budget_edge();      /* H-1 / H-2 */
    rc |= test_race_punch_drops_relay_leg();       /* H-7 */
    rc |= test_race_relay_defers_per_candidate();  /* H-4 */
    rc |= test_race_split_brain();                 /* H-3: two peers */
    rc |= test_relay_not_paired_named_separately();/* L-1 */
    rc |= test_natpmp_pcp();  /* S7: test 22 */
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
