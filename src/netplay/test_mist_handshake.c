/*
 * test_mist_handshake.c — Phase 6 Step 8 unit test for the Layer 3 MIST
 * handshake (docs/plan-netplay-phase6.md Step 8).
 *
 * Four cases, all driven on a localhost UDP socket pair so the test has
 * no external dependencies:
 *   (a) hello sent + peer acks         → returns true
 *   (b) hello sent + peer rejects      → returns false, reason captured
 *   (c) hello sent + peer silent       → retransmits 5×, times out at 500ms
 *   (d) hello sent + bogus magic reply → ignored, times out as (c)
 *
 * We spawn a helper thread that plays the peer's role. Cleaner than a
 * single-thread state machine because mist_handshake_send_and_wait() is
 * a blocking loop; driving it with poll-and-tick would leak state from
 * the implementation into the test.
 *
 * Gated behind ENABLE_NETPLAY_TESTS; under #else the entry point prints
 * "not compiled in" and returns 2. Mirrors test_event_queue.c pattern.
 * Enable with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS='-DENABLE_NETPLAY_TESTS -DNETPLAY_TEST_HOOKS'"
 * (NETPLAY_TEST_HOOKS is required because this build links every
 * src/netplay/test_*.c TU, including test_bilateral_punch.c, which needs
 * that macro to declare DirectP2P_TestHook_IsLanPeer.)
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/mist_handshake.h"

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

typedef enum {
    PEER_ACK,             /* reply to the first hello with an ack */
    PEER_REJECT,          /* reply with a reject */
    PEER_SILENT,          /* never reply */
    PEER_BADMAGIC,        /* reply with a malformed frame (wrong magic) */
    /* R-1 compatibility-field cases: */
    PEER_ACK_WRONG_STATE, /* ack with a mismatching state_ver */
    PEER_ACK_WRONG_PROTO, /* ack with a mismatching proto_ver */
    PEER_ACK_LEGACY,      /* pre-R-1 ack: three strings, no version fields */
} PeerMode;

typedef struct {
    int sock;
    PeerMode mode;
    volatile bool stop;
} PeerCtx;

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

/*
 * Peer thread: waits for an inbound hello, then responds per mode.
 * Times out itself after ~1200ms so the test cannot hang forever.
 */
static int SDLCALL peer_thread(void* arg) {
    PeerCtx* ctx = (PeerCtx*)arg;
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

        uint8_t buf[256];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        const int n = (int)recvfrom(ctx->sock, (char*)buf, sizeof(buf), 0,
                                    (struct sockaddr*)&src, &sl);
        if (n <= 0) continue;

        if (ctx->mode == PEER_SILENT) {
            continue; /* never reply */
        }

        uint8_t reply[256];
        int reply_len = 0;
        if (ctx->mode == PEER_ACK) {
            /* Note: build_hash "abcdef0" deliberately differs from the
             * local MIST_BUILD_HASH — a hash difference is warning-only
             * and must NOT reject (only state_ver/proto_ver reject). */
            reply_len = (int)mist_handshake_build_frame(
                MIST_MSG_ACK, "armv7", "mister", "abcdef0",
                0, NULL, reply, sizeof(reply));
        } else if (ctx->mode == PEER_REJECT) {
            reply_len = (int)mist_handshake_build_frame(
                MIST_MSG_REJECT, NULL, NULL, NULL,
                MIST_REJECT_ARCH_MISMATCH, "arch mismatch",
                reply, sizeof(reply));
        } else if (ctx->mode == PEER_ACK_WRONG_STATE) {
            reply_len = (int)mist_handshake_build_frame_ex(
                MIST_MSG_ACK, "armv7", "mister", "abcdef0",
                MIST_PROTO_VER,
                (uint16_t)(mist_handshake_local_state_ver() + 8),
                0, NULL, reply, sizeof(reply));
        } else if (ctx->mode == PEER_ACK_WRONG_PROTO) {
            reply_len = (int)mist_handshake_build_frame_ex(
                MIST_MSG_ACK, "armv7", "mister", "abcdef0",
                (uint8_t)(MIST_PROTO_VER + 1),
                mist_handshake_local_state_ver(),
                0, NULL, reply, sizeof(reply));
        } else if (ctx->mode == PEER_ACK_LEGACY) {
            reply_len = (int)mist_handshake_build_legacy_frame(
                MIST_MSG_ACK, "armv7", "mister", "abcdef0",
                reply, sizeof(reply));
        } else { /* PEER_BADMAGIC */
            reply_len = (int)mist_handshake_build_bad_magic(reply, sizeof(reply));
        }
        if (reply_len > 0) {
            sendto(ctx->sock, (const char*)reply, reply_len, 0,
                   (struct sockaddr*)&src, sl);
        }

        /* For the BADMAGIC test: we want send_and_wait to keep waiting
         * for a valid ack (which will never come). The implementation
         * drops our malformed frame and stays in the loop. Test case (d)
         * will pass if we keep quiet from here on. */
        if (ctx->mode == PEER_BADMAGIC) {
            ctx->mode = PEER_SILENT;
        } else {
            return 0;
        }
    }
}

static int run_case(const char* label, PeerMode mode, bool expect_success,
                    const char* expect_reason_substr) {
    mist_handshake_test_reset();

    unsigned short peer_port = 0, local_port = 0;
    int peer_sock = open_udp_on_localhost(&peer_port);
    int local_sock = open_udp_on_localhost(&local_port);
    if (peer_sock < 0 || local_sock < 0) {
        fprintf(stderr, "[test_mist_handshake] %s FAIL: socket bind\n", label);
        return 1;
    }

    PeerCtx ctx = { peer_sock, mode, false };
    SDL_Thread* tid = SDL_CreateThread(peer_thread, "mist_peer", &ctx);
    if (!tid) {
        fprintf(stderr, "[test_mist_handshake] %s FAIL: SDL_CreateThread\n", label);
        close_sock(peer_sock);
        close_sock(local_sock);
        return 1;
    }

    struct sockaddr_in peer_addr;
    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer_addr.sin_port = htons(peer_port);

    const bool got = mist_handshake_send_and_wait(local_sock, &peer_addr,
                                                  sizeof(peer_addr), 500);
    ctx.stop = true;
    SDL_WaitThread(tid, NULL);
    close_sock(peer_sock);
    close_sock(local_sock);

    if (got != expect_success) {
        fprintf(stderr, "[test_mist_handshake] %s FAIL: got=%s expected=%s reason=\"%s\"\n",
                label,
                got ? "true" : "false",
                expect_success ? "true" : "false",
                mist_handshake_last_reject_reason());
        return 1;
    }

    if (!expect_success) {
        const char* reason = mist_handshake_last_reject_reason();
        if (!reason || reason[0] == '\0') {
            fprintf(stderr, "[test_mist_handshake] %s FAIL: expected a reject reason, got empty\n", label);
            return 1;
        }
        if (expect_reason_substr != NULL && strstr(reason, expect_reason_substr) == NULL) {
            fprintf(stderr,
                    "[test_mist_handshake] %s FAIL: reason \"%s\" does not contain \"%s\"\n",
                    label, reason, expect_reason_substr);
            return 1;
        }
        fprintf(stderr, "[test_mist_handshake] %s OK (reason=\"%s\")\n", label, reason);
    } else {
        fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    }
    return 0;
}

/*
 * R-1 responder-direction cases: drive mist_handshake_build_reply (the
 * exact code the live receive paths use — respond_to_hello delegates to
 * it) with crafted hello payloads and assert on the reply frame's
 * msg_type + reject reason byte. Pure functions, no sockets.
 */
static int run_reply_case(const char* label,
                          const uint8_t* frame, size_t frame_len,
                          uint8_t expect_msg_type, uint8_t expect_reason) {
    if (frame_len < MIST_HEADER_LEN) {
        fprintf(stderr, "[test_mist_handshake] %s FAIL: bad input frame\n", label);
        return 1;
    }
    const size_t payload_len = frame_len - MIST_HEADER_LEN;

    uint8_t reply[MIST_FRAME_MAX];
    const size_t reply_len = mist_handshake_build_reply(frame + MIST_HEADER_LEN,
                                                        payload_len,
                                                        reply, sizeof(reply));
    if (reply_len < MIST_HEADER_LEN) {
        fprintf(stderr, "[test_mist_handshake] %s FAIL: no reply built\n", label);
        return 1;
    }
    if (reply[4] != expect_msg_type) {
        fprintf(stderr,
                "[test_mist_handshake] %s FAIL: reply msg_type 0x%02x expected 0x%02x\n",
                label, reply[4], expect_msg_type);
        return 1;
    }
    if (expect_msg_type == MIST_MSG_REJECT) {
        if (reply_len < MIST_HEADER_LEN + 1 || reply[MIST_HEADER_LEN] != expect_reason) {
            fprintf(stderr,
                    "[test_mist_handshake] %s FAIL: reject reason %u expected %u\n",
                    label,
                    (reply_len > MIST_HEADER_LEN) ? (unsigned)reply[MIST_HEADER_LEN] : 0u,
                    (unsigned)expect_reason);
            return 1;
        }
    }
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/*
 * ---------------------------------------------------------------------
 * R-1 adv-review M-5: live-runner core tests.
 *
 * mist_handshake_run_attempt is the loop the production gate actually
 * runs (netplay.c binds it to the session socket). Here we bind it to an
 * in-memory IO with a fake clock: recv serves a pre-queued datagram
 * list, delay_ms advances virtual time (so a full 500 ms attempt takes
 * microseconds), and everything the runner sends is recorded for
 * assertions — send_to_peer traffic (hellos, gratuitous ack) separately
 * from send_reply_to_last traffic (answers to inbound hellos).
 * ---------------------------------------------------------------------
 */

#define FAKE_DGRAM_CAP 256
#define FAKE_QUEUE_CAP 8
#define FAKE_LOG_CAP 16

typedef struct {
    uint8_t data[FAKE_DGRAM_CAP];
    size_t len;
    bool from_peer;
} FakeDgram;

typedef struct {
    FakeDgram inq[FAKE_QUEUE_CAP];
    int inq_count;
    int inq_next;
    FakeDgram sent[FAKE_LOG_CAP];    /* send_to_peer log */
    int sent_count;
    FakeDgram replies[FAKE_LOG_CAP]; /* send_reply_to_last log */
    int replies_count;
    uint64_t clock_ms;
} FakeIo;

static void fake_queue(FakeIo* io, const uint8_t* buf, size_t len, bool from_peer) {
    if (io->inq_count >= FAKE_QUEUE_CAP || len > FAKE_DGRAM_CAP) return;
    memcpy(io->inq[io->inq_count].data, buf, len);
    io->inq[io->inq_count].len = len;
    io->inq[io->inq_count].from_peer = from_peer;
    io->inq_count++;
}

static void fake_send_to_peer(void* v, const uint8_t* buf, size_t len) {
    FakeIo* io = (FakeIo*)v;
    if (io->sent_count >= FAKE_LOG_CAP || len > FAKE_DGRAM_CAP) return;
    memcpy(io->sent[io->sent_count].data, buf, len);
    io->sent[io->sent_count].len = len;
    io->sent_count++;
}

static int fake_recv(void* v, uint8_t* buf, size_t cap, bool* from_peer) {
    FakeIo* io = (FakeIo*)v;
    if (io->inq_next >= io->inq_count) return 0;
    FakeDgram* d = &io->inq[io->inq_next++];
    size_t n = d->len < cap ? d->len : cap;
    memcpy(buf, d->data, n);
    *from_peer = d->from_peer;
    return (int)n;
}

static void fake_send_reply(void* v, const uint8_t* buf, size_t len) {
    FakeIo* io = (FakeIo*)v;
    if (io->replies_count >= FAKE_LOG_CAP || len > FAKE_DGRAM_CAP) return;
    memcpy(io->replies[io->replies_count].data, buf, len);
    io->replies[io->replies_count].len = len;
    io->replies_count++;
}

static uint64_t fake_now(void* v) {
    return ((FakeIo*)v)->clock_ms;
}

static void fake_delay(void* v, uint32_t ms) {
    ((FakeIo*)v)->clock_ms += (uint64_t)ms;
}

static void fake_io_bind(FakeIo* fio, MistRunnerIo* io) {
    memset(fio, 0, sizeof(*fio));
    io->ctx = fio;
    io->send_to_peer = fake_send_to_peer;
    io->recv = fake_recv;
    io->send_reply_to_last = fake_send_reply;
    io->now_ms = fake_now;
    io->delay_ms = fake_delay;
}

/* A minimal "live GekkoNet" datagram: first byte a valid PacketType
 * (4 = SyncRequest — what a freshly started session actually emits),
 * rest arbitrary. Only byte 0 matters to the implicit-completion guard. */
static size_t fake_gekko_frame(uint8_t first_byte, uint8_t* out, size_t cap) {
    if (cap < 16) return 0;
    memset(out, 0xA5, 16);
    out[0] = first_byte;
    return 16;
}

#define RCHECK(cond, why)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "[test_mist_handshake] %s FAIL: %s\n", label, why); \
            return 1;                                                           \
        }                                                                       \
    } while (0)

/* (r1) valid ack -> OK; retransmitted hello then gratuitous ack sent. */
static int runner_case_ack_ok(void) {
    const char* label = "(r1) runner ack -> OK + gratuitous ack";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    uint8_t f[MIST_FRAME_MAX];
    size_t fl = mist_handshake_build_frame(MIST_MSG_ACK, "armv7", "mister",
                                           "abcdef0", 0, NULL, f, sizeof(f));
    fake_queue(&fio, f, fl, true);

    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    const MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));

    RCHECK(hs == MIST_HS_OK, "expected MIST_HS_OK");
    RCHECK(fio.sent_count == 2, "expected exactly hello + gratuitous ack");
    RCHECK(fio.sent[0].data[4] == MIST_MSG_HELLO, "first send not a hello");
    RCHECK(fio.sent[1].data[4] == MIST_MSG_ACK, "no gratuitous ack after completion");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r2) valid ack with a peer hello queued behind it -> OK and the queued
 * hello is answered before the runner returns (H-1 hardening: the
 * completion race window is exactly this queued-hello case). */
static int runner_case_ok_answers_queued_hello(void) {
    const char* label = "(r2) runner OK answers queued hello";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    uint8_t f[MIST_FRAME_MAX];
    size_t fl = mist_handshake_build_frame(MIST_MSG_ACK, "armv7", "mister",
                                           "abcdef0", 0, NULL, f, sizeof(f));
    fake_queue(&fio, f, fl, true);
    fl = mist_handshake_build_frame(MIST_MSG_HELLO, "armv7", "mister",
                                    "abcdef0", 0, NULL, f, sizeof(f));
    fake_queue(&fio, f, fl, true);

    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    const MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));

    RCHECK(hs == MIST_HS_OK, "expected MIST_HS_OK");
    RCHECK(fio.replies_count == 1, "queued hello not answered");
    RCHECK(fio.replies[0].data[4] == MIST_MSG_ACK, "queued hello answer not an ack");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r3) silence -> TIMEOUT after the full budget with exactly
 * MIST_RETRANSMIT_COUNT hellos on the wire. */
static int runner_case_timeout(void) {
    const char* label = "(r3) runner silence -> TIMEOUT, 5 hellos";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    const MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));

    RCHECK(hs == MIST_HS_TIMEOUT, "expected MIST_HS_TIMEOUT");
    RCHECK(strstr(reason, "timeout") != NULL, "reason does not mention timeout");
    RCHECK(fio.sent_count == MIST_RETRANSMIT_COUNT, "retransmit budget not honored");
    RCHECK(fio.clock_ms >= MIST_DEFAULT_TIMEOUT_MS, "returned before the budget elapsed");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r4) explicit reject frame -> FAIL with the frame's reason text. */
static int runner_case_reject_fail(void) {
    const char* label = "(r4) runner reject -> FAIL";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    uint8_t f[MIST_FRAME_MAX];
    size_t fl = mist_handshake_build_frame(MIST_MSG_REJECT, NULL, NULL, NULL,
                                           MIST_REJECT_ARCH_MISMATCH,
                                           "arch mismatch", f, sizeof(f));
    fake_queue(&fio, f, fl, true);

    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    const MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));

    RCHECK(hs == MIST_HS_FAIL, "expected MIST_HS_FAIL");
    RCHECK(strstr(reason, "arch mismatch") != NULL, "reason lost");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r5) H-2a regression: an incompatible (wrong state_ver) hello makes
 * the RESPONDER fail its own session too — reject sent to the peer AND
 * MIST_HS_FAIL returned with the same classify text — instead of
 * burning the remaining retry budget. Latch must stay unarmed. */
static int runner_case_h2a_wrong_state_hello(void) {
    const char* label = "(r5) H-2a wrong-state hello -> reject + own FAIL";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    uint8_t f[MIST_FRAME_MAX];
    size_t fl = mist_handshake_build_frame_ex(
        MIST_MSG_HELLO, "armv7", "mister", "abcdef0", MIST_PROTO_VER,
        (uint16_t)(mist_handshake_local_state_ver() + 8), 0, NULL, f, sizeof(f));
    fake_queue(&fio, f, fl, true);

    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    const MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));

    RCHECK(hs == MIST_HS_FAIL, "expected MIST_HS_FAIL (reject-then-wait bug)");
    RCHECK(strstr(reason, "Build state") != NULL, "reason not the state-mismatch text");
    RCHECK(fio.replies_count == 1, "no reject sent to the peer");
    RCHECK(fio.replies[0].data[4] == MIST_MSG_REJECT, "reply not a reject");
    RCHECK(fio.replies[0].data[MIST_HEADER_LEN] == MIST_REJECT_STATE_MISMATCH,
           "wrong reject reason code");
    RCHECK(!peer_hello_ok, "incompatible hello must not arm the latch");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r6) MALFORMED is the deliberate H-2a exception: a garbled
 * MIST-magic datagram gets a reject reply but must NOT kill the session
 * (the real peer's retransmitted hello will classify properly). */
static int runner_case_malformed_hello_keeps_waiting(void) {
    const char* label = "(r6) malformed hello -> reject reply, keep waiting";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    /* Header + 5 payload bytes with no NUL terminator anywhere. */
    uint8_t f[MIST_HEADER_LEN + 5];
    f[0] = MIST_MAGIC_B0;
    f[1] = MIST_MAGIC_B1;
    f[2] = MIST_MAGIC_B2;
    f[3] = MIST_MAGIC_B3;
    f[4] = MIST_MSG_HELLO;
    f[5] = 0;
    f[6] = 5;
    memset(f + MIST_HEADER_LEN, 0xFF, 5);
    fake_queue(&fio, f, sizeof(f), true);

    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    const MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));

    RCHECK(hs == MIST_HS_TIMEOUT, "malformed hello must not hard-fail the session");
    RCHECK(fio.replies_count == 1, "malformed hello not answered");
    RCHECK(fio.replies[0].data[4] == MIST_MSG_REJECT, "answer not a reject");
    RCHECK(fio.replies[0].data[MIST_HEADER_LEN] == MIST_REJECT_MALFORMED,
           "wrong reject reason code");
    RCHECK(!peer_hello_ok, "malformed hello must not arm the latch");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r7) H-2b regression: a hello queued BEHIND the reject that fails us
 * still gets answered before the runner returns, so the peer hears the
 * real mismatch instead of timing out on the generic no-reply text. */
static int runner_case_h2b_answers_hello_behind_reject(void) {
    const char* label = "(r7) H-2b reject drains + answers queued hello";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    uint8_t f[MIST_FRAME_MAX];
    size_t fl = mist_handshake_build_frame(MIST_MSG_REJECT, NULL, NULL, NULL,
                                           MIST_REJECT_ARCH_MISMATCH,
                                           "arch mismatch", f, sizeof(f));
    fake_queue(&fio, f, fl, true);
    fl = mist_handshake_build_frame_ex(
        MIST_MSG_HELLO, "armv7", "mister", "abcdef0", MIST_PROTO_VER,
        (uint16_t)(mist_handshake_local_state_ver() + 8), 0, NULL, f, sizeof(f));
    fake_queue(&fio, f, fl, true);

    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    const MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));

    RCHECK(hs == MIST_HS_FAIL, "expected MIST_HS_FAIL");
    RCHECK(strstr(reason, "arch mismatch") != NULL, "reason not from the reject frame");
    RCHECK(fio.replies_count == 1, "queued hello behind reject not answered");
    RCHECK(fio.replies[0].data[4] == MIST_MSG_REJECT, "answer not a reject");
    RCHECK(fio.replies[0].data[MIST_HEADER_LEN] == MIST_REJECT_STATE_MISMATCH,
           "answer must carry the classify-derived reason");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r8) H-1 headline regression: two IDENTICAL builds where the peer's
 * single gratuitous ack is LOST must still connect. Attempt 1: the
 * peer's compatible hello arms the latch (and gets our ack), but no ack
 * for OUR hello ever arrives -> TIMEOUT (this is the stranded state).
 * Attempt 2 (latch persisted): the peer — already in GekkoNet — sends
 * Gekko traffic -> implicit completion, MIST_HS_OK, gate PROCEEDs. */
static int runner_case_h1_stranded_completion(void) {
    const char* label = "(r8) H-1 identical builds, gratuitous ack dropped";
    mist_handshake_test_reset();

    bool peer_hello_ok = false;
    int attempts = 0;
    char reason[128] = { 0 };

    /* Attempt 1: peer hello arrives; our hello's ack never does. */
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);
    uint8_t f[MIST_FRAME_MAX];
    size_t fl = mist_handshake_build_frame(MIST_MSG_HELLO, "armv7", "mister",
                                           "abcdef0", 0, NULL, f, sizeof(f));
    fake_queue(&fio, f, fl, true);

    MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));
    RCHECK(hs == MIST_HS_TIMEOUT, "attempt 1 should time out (ack lost)");
    RCHECK(fio.replies_count == 1 && fio.replies[0].data[4] == MIST_MSG_ACK,
           "peer hello not acked");
    RCHECK(peer_hello_ok, "compatible peer hello must arm the latch");
    RCHECK(mist_handshake_gate_next(hs, &attempts, MIST_HANDSHAKE_MAX_ATTEMPTS,
                                    reason, sizeof(reason)) == MIST_GATE_RETRY,
           "gate should retry after one timeout");

    /* Attempt 2: peer's GekkoNet traffic (it completed; we're stranded). */
    fake_io_bind(&fio, &io);
    uint8_t g[32];
    size_t gl = fake_gekko_frame(4 /* SyncRequest */, g, sizeof(g));
    fake_queue(&fio, g, gl, true);

    hs = mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));
    RCHECK(hs == MIST_HS_OK, "Gekko traffic from a classified-clean peer must complete");
    RCHECK(mist_handshake_gate_next(hs, &attempts, MIST_HANDSHAKE_MAX_ATTEMPTS,
                                    reason, sizeof(reason)) == MIST_GATE_PROCEED,
           "gate should proceed");
    RCHECK(attempts == 0, "OK must reset the attempt counter");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r9) H-1 guard, CRITICAL bypass check: Gekko-shaped traffic WITHOUT a
 * classified-clean peer hello must NOT complete the handshake — a
 * legacy/incompatible peer that simply starts GekkoNet (every pre-R-1
 * release does exactly that) stays gated and times out. Also: a
 * compatible hello from a FOREIGN source must not arm the latch. */
static int runner_case_h1_guard_unarmed(void) {
    const char* label = "(r9) H-1 guard: unarmed Gekko traffic stays gated";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    /* Foreign-source compatible hello (answered, but must not arm)... */
    uint8_t f[MIST_FRAME_MAX];
    size_t fl = mist_handshake_build_frame(MIST_MSG_HELLO, "armv7", "mister",
                                           "abcdef0", 0, NULL, f, sizeof(f));
    fake_queue(&fio, f, fl, false /* NOT the session peer */);
    /* ...followed by Gekko traffic from the session peer. */
    uint8_t g[32];
    size_t gl = fake_gekko_frame(1 /* Inputs */, g, sizeof(g));
    fake_queue(&fio, g, gl, true);

    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    const MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));

    RCHECK(hs == MIST_HS_TIMEOUT, "unarmed Gekko traffic must NOT complete the gate");
    RCHECK(!peer_hello_ok, "foreign hello must not arm the latch");
    RCHECK(fio.replies_count == 1 && fio.replies[0].data[4] == MIST_MSG_ACK,
           "foreign compatible hello should still be answered");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r10) H-1 guard: even with the latch armed, Gekko-shaped traffic from
 * a source that is NOT the session peer must not complete. */
static int runner_case_h1_guard_foreign_source(void) {
    const char* label = "(r10) H-1 guard: foreign Gekko traffic stays gated";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    uint8_t g[32];
    size_t gl = fake_gekko_frame(4, g, sizeof(g));
    fake_queue(&fio, g, gl, false /* NOT the session peer */);

    bool peer_hello_ok = true; /* armed */
    char reason[128] = { 0 };
    const MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));

    RCHECK(hs == MIST_HS_TIMEOUT, "foreign-source Gekko traffic must NOT complete");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r11) H-1 guard: armed latch + session-peer source, but the payload
 * is not Gekko-shaped (byte 0 outside [1,7]) — punch keepalives, zero
 * bytes, out-of-range types all stay dropped. */
static int runner_case_h1_guard_shape(void) {
    const char* label = "(r11) H-1 guard: non-Gekko shapes stay gated";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    uint8_t g[32];
    size_t gl = fake_gekko_frame(0, g, sizeof(g)); /* below range */
    fake_queue(&fio, g, gl, true);
    gl = fake_gekko_frame(8, g, sizeof(g)); /* above range */
    fake_queue(&fio, g, gl, true);
    static const char punch[] = "3SX_PUNCH"; /* keepalive, byte 0 = 0x33 */
    fake_queue(&fio, (const uint8_t*)punch, sizeof(punch) - 1, true);

    bool peer_hello_ok = true; /* armed */
    char reason[128] = { 0 };
    const MistHandshakeResult hs =
        mist_handshake_run_attempt(&io, &peer_hello_ok, reason, sizeof(reason));

    RCHECK(hs == MIST_HS_TIMEOUT, "non-Gekko-shaped traffic must NOT complete");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (r12) gate retry policy: 39 timeouts RETRY, the 40th FAILs with the
 * no-reply message; OK resets the counter; FAIL passes the runner's
 * reason through untouched. */
static int runner_case_gate_policy(void) {
    const char* label = "(r12) gate policy: cap, exhaustion message, resets";
    int attempts = 0;
    char reason[128] = { 0 };

    for (int i = 0; i < MIST_HANDSHAKE_MAX_ATTEMPTS - 1; i++) {
        snprintf(reason, sizeof(reason), "timeout (peer did not respond)");
        RCHECK(mist_handshake_gate_next(MIST_HS_TIMEOUT, &attempts,
                                        MIST_HANDSHAKE_MAX_ATTEMPTS, reason,
                                        sizeof(reason)) == MIST_GATE_RETRY,
               "timeout under the cap must RETRY");
    }
    RCHECK(attempts == MIST_HANDSHAKE_MAX_ATTEMPTS - 1, "attempt counter drifted");
    RCHECK(mist_handshake_gate_next(MIST_HS_TIMEOUT, &attempts,
                                    MIST_HANDSHAKE_MAX_ATTEMPTS, reason,
                                    sizeof(reason)) == MIST_GATE_FAIL,
           "exhausting the cap must FAIL");
    RCHECK(strstr(reason, "No reply") != NULL, "exhaustion message missing");

    attempts = 7;
    RCHECK(mist_handshake_gate_next(MIST_HS_OK, &attempts,
                                    MIST_HANDSHAKE_MAX_ATTEMPTS, reason,
                                    sizeof(reason)) == MIST_GATE_PROCEED,
           "OK must PROCEED");
    RCHECK(attempts == 0, "OK must reset the attempt counter");

    snprintf(reason, sizeof(reason), "Build state 17672 vs 17676 - update one side");
    RCHECK(mist_handshake_gate_next(MIST_HS_FAIL, &attempts,
                                    MIST_HANDSHAKE_MAX_ATTEMPTS, reason,
                                    sizeof(reason)) == MIST_GATE_FAIL,
           "FAIL must FAIL");
    RCHECK(strstr(reason, "Build state 17672") != NULL,
           "FAIL must not overwrite the runner's reason");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* ---------------------------------------------------------------------
 * S3 — incremental pump (mist_handshake_pump_begin / mist_handshake_pump).
 * The production gate now runs ONE bounded slice per frame instead of
 * blocking the whole 500 ms attempt inside Netplay_Run. These cases pin
 * the pump's contract: it never sleeps (delay_ms must never be called —
 * with the FakeIo clock, any delay call would advance clock_ms), each
 * slice is bounded, and its terminal results match the blocking runner.
 * --------------------------------------------------------------------- */

/* (p1) pump completes across ticks without ever calling delay_ms: slice 1
 * sends the hello and returns PENDING; the peer's ack queued before
 * slice 2 completes with OK + the gratuitous ack. Virtual clock is
 * advanced ONLY by the test between slices — an unchanged-by-pump clock
 * proves delay_ms was never invoked. */
static int pump_case_ok_across_ticks(void) {
    const char* label = "(p1) pump: ack across ticks, no delay calls";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    MistPumpState st;
    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    RCHECK(mist_handshake_pump_begin(&st, &io, reason, sizeof(reason)),
           "pump_begin failed");

    /* Slice 1: nothing queued — hello out, attempt pending. */
    fio.clock_ms = 16; /* one frame later */
    MistPumpStatus ps = mist_handshake_pump(&st, &io, &peer_hello_ok,
                                            reason, sizeof(reason));
    RCHECK(ps == MIST_PUMP_PENDING, "slice 1 should be PENDING");
    RCHECK(fio.sent_count == 1 && fio.sent[0].data[4] == MIST_MSG_HELLO,
           "slice 1 must send exactly one hello");
    RCHECK(fio.clock_ms == 16, "pump must NOT call delay_ms (clock moved)");

    /* Slice 2: peer ack arrives between frames. */
    uint8_t f[MIST_FRAME_MAX];
    size_t fl = mist_handshake_build_frame(MIST_MSG_ACK, "armv7", "mister",
                                           "abcdef0", 0, NULL, f, sizeof(f));
    fake_queue(&fio, f, fl, true);
    fio.clock_ms = 32;
    ps = mist_handshake_pump(&st, &io, &peer_hello_ok, reason, sizeof(reason));
    RCHECK(ps == MIST_PUMP_OK, "slice 2 should complete with OK");
    RCHECK(fio.sent_count == 2 && fio.sent[1].data[4] == MIST_MSG_ACK,
           "completion must send the gratuitous ack");
    RCHECK(fio.clock_ms == 32, "pump must NOT call delay_ms (clock moved)");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (p2) pump timeout: driven at a 16 ms virtual frame cadence with a
 * silent peer, the attempt must retransmit the full hello ladder
 * (MIST_RETRANSMIT_COUNT at 100 ms spacing) and report TIMEOUT once the
 * 500 ms budget elapses — in ~budget/16 slices, none of them sleeping. */
static int pump_case_timeout_frame_cadence(void) {
    const char* label = "(p2) pump: silent peer times out at frame cadence";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    MistPumpState st;
    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    RCHECK(mist_handshake_pump_begin(&st, &io, reason, sizeof(reason)),
           "pump_begin failed");

    int slices = 0;
    MistPumpStatus ps = MIST_PUMP_PENDING;
    while (ps == MIST_PUMP_PENDING && slices < 100) {
        fio.clock_ms += 16; /* the test, not the pump, advances time */
        ps = mist_handshake_pump(&st, &io, &peer_hello_ok, reason, sizeof(reason));
        slices++;
    }
    RCHECK(ps == MIST_PUMP_TIMEOUT, "silent peer must TIMEOUT");
    RCHECK(fio.clock_ms >= MIST_DEFAULT_TIMEOUT_MS,
           "TIMEOUT before the 500 ms budget elapsed");
    RCHECK(slices <= (MIST_DEFAULT_TIMEOUT_MS / 16) + 2,
           "too many slices — pump not honoring the deadline");
    RCHECK(fio.sent_count == MIST_RETRANSMIT_COUNT,
           "full hello retransmit ladder expected");
    RCHECK(strstr(reason, "timeout") != NULL, "timeout reason missing");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

/* (p3) pump reject parity: an explicit reject terminates the very slice
 * that drains it, with the runner-identical reason text. */
static int pump_case_reject(void) {
    const char* label = "(p3) pump: explicit reject fails the slice";
    mist_handshake_test_reset();
    FakeIo fio;
    MistRunnerIo io;
    fake_io_bind(&fio, &io);

    uint8_t f[MIST_FRAME_MAX];
    size_t fl = mist_handshake_build_frame(MIST_MSG_REJECT, NULL, NULL, NULL,
                                           1 /* arch mismatch */, "arch mismatch (x86)",
                                           f, sizeof(f));
    fake_queue(&fio, f, fl, true);

    MistPumpState st;
    bool peer_hello_ok = false;
    char reason[128] = { 0 };
    RCHECK(mist_handshake_pump_begin(&st, &io, reason, sizeof(reason)),
           "pump_begin failed");
    fio.clock_ms = 16;
    const MistPumpStatus ps = mist_handshake_pump(&st, &io, &peer_hello_ok,
                                                  reason, sizeof(reason));
    RCHECK(ps == MIST_PUMP_FAIL, "reject must FAIL the attempt");
    RCHECK(strstr(reason, "arch mismatch") != NULL, "reject reason missing");
    fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    return 0;
}

#undef RCHECK

int Netplay_Test_MistHandshake(void) {
    int fails = 0;
    fails += run_case("(a) ack",        PEER_ACK,      true,  NULL);
    fails += run_case("(b) reject",     PEER_REJECT,   false, "arch mismatch");
    fails += run_case("(c) silent",     PEER_SILENT,   false, "timeout");
    fails += run_case("(d) bad-magic",  PEER_BADMAGIC, false, "timeout");
    /* R-1: compatibility-field validation on the ack path. */
    fails += run_case("(e) wrong-state ack", PEER_ACK_WRONG_STATE, false, "Build state");
    fails += run_case("(f) wrong-proto ack", PEER_ACK_WRONG_PROTO, false, "Handshake v");
    fails += run_case("(g) legacy ack",      PEER_ACK_LEGACY,      false, "too old");

    /* R-1: responder direction — what we send back to a peer's hello. */
    uint8_t hello[MIST_FRAME_MAX];
    size_t hello_len;

    hello_len = mist_handshake_build_frame(MIST_MSG_HELLO, "armv7", "mister",
                                           "abcdef0", 0, NULL,
                                           hello, sizeof(hello));
    fails += run_reply_case("(h) reply to matching hello -> ack",
                            hello, hello_len, MIST_MSG_ACK, 0);

    hello_len = mist_handshake_build_frame_ex(
        MIST_MSG_HELLO, "armv7", "mister", "abcdef0",
        MIST_PROTO_VER, (uint16_t)(mist_handshake_local_state_ver() + 8),
        0, NULL, hello, sizeof(hello));
    fails += run_reply_case("(i) reply to wrong-state hello -> reject",
                            hello, hello_len, MIST_MSG_REJECT,
                            MIST_REJECT_STATE_MISMATCH);

    hello_len = mist_handshake_build_frame_ex(
        MIST_MSG_HELLO, "armv7", "mister", "abcdef0",
        (uint8_t)(MIST_PROTO_VER + 1), mist_handshake_local_state_ver(),
        0, NULL, hello, sizeof(hello));
    fails += run_reply_case("(j) reply to wrong-proto hello -> reject",
                            hello, hello_len, MIST_MSG_REJECT,
                            MIST_REJECT_PROTO_MISMATCH);

    /* (j2) THE v2 -> v3 BOUNDARY, PINNED TO A LITERAL.
     *
     * Case (j) above is version-RELATIVE (MIST_PROTO_VER + 1), so it stays
     * green for any value of MIST_PROTO_VER and therefore says nothing about
     * which versions are actually incompatible. This case pins the specific
     * pairing task #115 exists to refuse: a peer still advertising proto_ver
     * 2 -- i.e. any build from before task #111 replaced the desync
     * checksum's PLW input with the canonical member image -- must be
     * REJECTED, not merely warned at.
     *
     * That pairing is otherwise undetectable. build_hash difference is a
     * warning only (mist_handshake.c), and state_ver is sizeof(GameState),
     * which a checksum-semantics change need not move at all. Without this
     * reject the two builds connect and then disagree on every frame's
     * checksum.
     *
     * Deliberately a literal 2, not (MIST_PROTO_VER - 1): if MIST_PROTO_VER
     * is ever reverted to 2, this case must FAIL rather than quietly follow
     * it down. */
    hello_len = mist_handshake_build_frame_ex(
        MIST_MSG_HELLO, "armv7", "mister", "abcdef0",
        (uint8_t)2, mist_handshake_local_state_ver(),
        0, NULL, hello, sizeof(hello));
    fails += run_reply_case("(j2) reply to pre-#111 proto_ver=2 hello -> reject",
                            hello, hello_len, MIST_MSG_REJECT,
                            MIST_REJECT_PROTO_MISMATCH);

    hello_len = mist_handshake_build_legacy_frame(MIST_MSG_HELLO, "armv7",
                                                  "mister", "abcdef0",
                                                  hello, sizeof(hello));
    fails += run_reply_case("(k) reply to legacy hello -> reject",
                            hello, hello_len, MIST_MSG_REJECT,
                            MIST_REJECT_LEGACY);

    hello_len = mist_handshake_build_frame(MIST_MSG_HELLO, "x86_64", "mister",
                                           "abcdef0", 0, NULL,
                                           hello, sizeof(hello));
    fails += run_reply_case("(l) reply to wrong-arch hello -> reject",
                            hello, hello_len, MIST_MSG_REJECT,
                            MIST_REJECT_ARCH_MISMATCH);

    /* v2: balance-digest validation. Build the peer frame under digest A,
     * then classify under local digest B (the setter is the only knob —
     * builders and classifier both read the module-static). */
    mist_handshake_set_balance_digest(0x1122334455667788ULL);
    hello_len = mist_handshake_build_frame(MIST_MSG_HELLO, "armv7", "mister",
                                           "abcdef0", 0, NULL,
                                           hello, sizeof(hello));
    mist_handshake_set_balance_digest(0x8877665544332211ULL);
    fails += run_reply_case("(m) reply to wrong-balance-digest hello -> reject",
                            hello, hello_len, MIST_MSG_REJECT,
                            MIST_REJECT_BALANCE_MISMATCH);

    /* (n) sender side: an ack whose digest mismatches ours is a hard
     * reject with the symmetric ROM-mismatch text... */
    mist_handshake_set_balance_digest(0x1122334455667788ULL);
    uint8_t ack[MIST_FRAME_MAX];
    size_t ack_len = mist_handshake_build_frame(MIST_MSG_ACK, "armv7", "mister",
                                                "abcdef0", 0, NULL,
                                                ack, sizeof(ack));
    mist_handshake_set_balance_digest(0x8877665544332211ULL);
    if (mist_handshake_parse_response(ack, ack_len) != -1 ||
        strstr(mist_handshake_last_reject_reason(), "ROM sets differ") == NULL) {
        fprintf(stderr,
                "[test_mist_handshake] (n) wrong-balance-digest ack FAIL: got '%s'\n",
                mist_handshake_last_reject_reason());
        fails += 1;
    } else {
        fprintf(stderr, "[test_mist_handshake] (n) wrong-balance-digest ack -> reject OK\n");
    }

    /* (o) ...and matching digests still ack. */
    mist_handshake_set_balance_digest(0x1122334455667788ULL);
    ack_len = mist_handshake_build_frame(MIST_MSG_ACK, "armv7", "mister",
                                         "abcdef0", 0, NULL, ack, sizeof(ack));
    if (mist_handshake_parse_response(ack, ack_len) != 1) {
        fprintf(stderr, "[test_mist_handshake] (o) matching-digest ack FAIL\n");
        fails += 1;
    } else {
        fprintf(stderr, "[test_mist_handshake] (o) matching-digest ack -> OK\n");
    }

    /* (q) THE ORDER CASE: a genuine v1 peer on the wire. Cases (f)/(j)
     * cover a wrong-proto frame that still CARRIES a digest, because
     * mist_handshake_build_frame_ex always appends one. A real pre-digest
     * build emits proto_ver=1 with NOTHING after state_ver, so this is
     * the only shape that can prove classify_peer_payload returns
     * PROTO_MISMATCH *before* it tries to read the digest field. If the
     * digest read ever moved above the proto check, this frame would
     * classify MALFORMED ("no balance digest") and an old build would be
     * told its handshake was corrupt instead of "update one side".
     *
     * Build a v2 frame at proto_ver 1, then truncate the trailing 8-byte
     * digest and fix payload_len (offset 5, big-endian) to match. */
    mist_handshake_set_balance_digest(0x1122334455667788ULL);
    uint8_t v1[MIST_FRAME_MAX];
    size_t v1_len = mist_handshake_build_frame_ex(
        MIST_MSG_ACK, "armv7", "mister", "abcdef0",
        1 /* v1 proto */, mist_handshake_local_state_ver(),
        0, NULL, v1, sizeof(v1));
    if (v1_len <= 8) {
        fprintf(stderr, "[test_mist_handshake] (q) v1 frame build FAIL\n");
        fails += 1;
    } else {
        const uint16_t old_payload_len = (uint16_t)((v1[5] << 8) | v1[6]);
        const uint16_t new_payload_len = (uint16_t)(old_payload_len - 8);
        v1[5] = (uint8_t)((new_payload_len >> 8) & 0xFF);
        v1[6] = (uint8_t)(new_payload_len & 0xFF);
        v1_len -= 8; /* drop the digest bytes entirely */

        /* The expected text tracks MIST_PROTO_VER instead of hardcoding the
         * local version. What this case asserts is that the PROTO check fires
         * BEFORE the digest read on a frame with no digest to read -- the
         * local version number is incidental to that. It was pinned to a
         * literal "v2" and so broke on the task #115 bump to v3 while still
         * testing exactly what it always did. Building the string keeps the
         * assertion (v1 vs OURS, proto-mismatch wording) and drops only the
         * false pin. The peer side stays a literal v1: that IS the subject. */
        char want_proto_text[64];
        snprintf(want_proto_text, sizeof(want_proto_text),
                 "Handshake v1 vs v%u", (unsigned)MIST_PROTO_VER);

        if (mist_handshake_parse_response(v1, v1_len) != -1 ||
            strstr(mist_handshake_last_reject_reason(), want_proto_text) == NULL) {
            fprintf(stderr,
                    "[test_mist_handshake] (q) digest-less v1 ack FAIL: expected the "
                    "proto-mismatch text, got '%s'\n",
                    mist_handshake_last_reject_reason());
            fails += 1;
        } else {
            fprintf(stderr,
                    "[test_mist_handshake] (q) digest-less v1 ack -> PROTO_MISMATCH "
                    "(before any digest read) OK\n");
        }

        /* Responder direction: the reject we SEND a v1 peer must carry the
         * same PROTO_MISMATCH code, not a digest complaint. */
        uint8_t v1_hello[MIST_FRAME_MAX];
        size_t v1_hello_len = mist_handshake_build_frame_ex(
            MIST_MSG_HELLO, "armv7", "mister", "abcdef0",
            1, mist_handshake_local_state_ver(), 0, NULL,
            v1_hello, sizeof(v1_hello));
        const uint16_t hl = (uint16_t)(((v1_hello[5] << 8) | v1_hello[6]) - 8);
        v1_hello[5] = (uint8_t)((hl >> 8) & 0xFF);
        v1_hello[6] = (uint8_t)(hl & 0xFF);
        v1_hello_len -= 8;
        fails += run_reply_case("(q2) reply to digest-less v1 hello -> PROTO_MISMATCH",
                                v1_hello, v1_hello_len, MIST_MSG_REJECT,
                                MIST_REJECT_PROTO_MISMATCH);
    }

    /* Back to the digest-agnostic default for the runner cases below. */
    mist_handshake_set_balance_digest(0);

    /* R-1 adv-review M-5: live-runner core (mist_handshake_run_attempt /
     * mist_handshake_gate_next — the loop netplay.c actually runs). */
    fails += runner_case_ack_ok();
    fails += runner_case_ok_answers_queued_hello();
    fails += runner_case_timeout();
    fails += runner_case_reject_fail();
    fails += runner_case_h2a_wrong_state_hello();
    fails += runner_case_malformed_hello_keeps_waiting();
    fails += runner_case_h2b_answers_hello_behind_reject();
    fails += runner_case_h1_stranded_completion();
    fails += runner_case_h1_guard_unarmed();
    fails += runner_case_h1_guard_foreign_source();
    fails += runner_case_h1_guard_shape();
    fails += runner_case_gate_policy();

    /* S3: incremental per-tick pump (the production gate's new driver). */
    fails += pump_case_ok_across_ticks();
    fails += pump_case_timeout_frame_cadence();
    fails += pump_case_reject();

    if (fails > 0) {
        fprintf(stderr, "[test_mist_handshake] %d case(s) failed\n", fails);
        return 1;
    }
    /* Case-count reconciliation (rebase onto S3/S4):
     *   (a)-(g) sender-side           7
     *   (h)-(l) responder/reply       5
     *   (m)-(o) v2 balance digest     3   [this lane]
     *   (q)/(q2) v1-on-the-wire order 2   [this lane]
     *   runner_case_*                12
     *   pump_case_*                   3   [S3]
     *                                --
     *                                32
     * S3 added its three pump cases without bumping this string (it
     * still said 24, the pre-S3 total); this lane's digest and
     * classify-order cases land on top. */
    fprintf(stderr, "[test_mist_handshake] OK — 32 cases passed\n");
    return 0;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_MistHandshake(void) {
    fprintf(stderr,
            "[test_mist_handshake] not compiled in; rebuild with "
            "-DENABLE_NETPLAY_TESTS to enable.\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
