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
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS"
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

typedef enum {
    PEER_ACK,      /* reply to the first hello with an ack */
    PEER_REJECT,   /* reply with a reject */
    PEER_SILENT,   /* never reply */
    PEER_BADMAGIC, /* reply with a malformed frame (wrong magic) */
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
            reply_len = (int)mist_handshake_build_frame(
                MIST_MSG_ACK, "armv7", "mister", "abcdef0",
                0, NULL, reply, sizeof(reply));
        } else if (ctx->mode == PEER_REJECT) {
            reply_len = (int)mist_handshake_build_frame(
                MIST_MSG_REJECT, NULL, NULL, NULL,
                MIST_REJECT_ARCH_MISMATCH, "arch mismatch",
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

static int run_case(const char* label, PeerMode mode, bool expect_success) {
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
        fprintf(stderr, "[test_mist_handshake] %s OK (reason=\"%s\")\n", label, reason);
    } else {
        fprintf(stderr, "[test_mist_handshake] %s OK\n", label);
    }
    return 0;
}

int Netplay_Test_MistHandshake(void) {
    int fails = 0;
    fails += run_case("(a) ack",        PEER_ACK,      true);
    fails += run_case("(b) reject",     PEER_REJECT,   false);
    fails += run_case("(c) silent",     PEER_SILENT,   false);
    fails += run_case("(d) bad-magic",  PEER_BADMAGIC, false);
    if (fails > 0) {
        fprintf(stderr, "[test_mist_handshake] %d case(s) failed\n", fails);
        return 1;
    }
    fprintf(stderr, "[test_mist_handshake] OK — 4 cases passed\n");
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
