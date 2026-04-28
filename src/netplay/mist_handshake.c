/*
 * mist_handshake.c — Phase 6 Step 8: Layer-3 MiSTer-arch handshake.
 *
 * Implements the 2-way "MIST" magic-prefix handshake described in tier-2
 * §8.2.4 of docs/plan-netplay-port.md. Intended to run on the STUN /
 * matchmaking UDP socket BETWEEN hole-punch completion and gekko_create()
 * in src/netplay/netplay.c. See mist_handshake.h for wire format.
 *
 * Design notes:
 *   - POSIX sockets only. SDL3_net wraps POSIX under the hood on Linux;
 *     taking the fd lets us reuse the plan's exact API and avoid coupling
 *     the handshake to SDL_Net's NET_ReceiveDatagram non-blocking model
 *     (which already spins a thread internally on some platforms).
 *   - We drive the socket via select() with a computed remaining budget.
 *     Caller is responsible for providing a socket fd; we do NOT toggle
 *     O_NONBLOCK because select() gives us deterministic timing either
 *     way. A short recv is a noop (select said "ready").
 *   - Thread-safe for the single sender that the netplay state machine
 *     uses. The cached reject-reason buffer is not multi-thread safe;
 *     netplay.c only ever calls this from the game thread during
 *     TRANSITIONING→CONNECTING.
 *   - Receiver side: implemented. On an inbound hello with matching arch
 *     we reply with an ack; on mismatch we reply with a reject. This
 *     satisfies the plan's "both sides are MiSTer" case where each peer
 *     calls send_and_wait() concurrently — whichever peer's hello arrives
 *     first at the other side gets its ack during the same 500 ms window.
 */

#include "netplay/mist_handshake.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define MIST_SOCK_ERRNO WSAGetLastError()
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define MIST_SOCK_ERRNO errno
#endif

/* Canonical magic bytes. Any frame not starting with these is not ours. */
const uint8_t MIST_MAGIC[MIST_MAGIC_LEN] = {
    MIST_MAGIC_B0, MIST_MAGIC_B1, MIST_MAGIC_B2, MIST_MAGIC_B3,
};

/* Build hash advertised in hello/ack payloads. The plan allows pragmatic
 * sourcing — compile-time macro, env var, or file read. We pick the
 * compile-time define if present (the build system can pass
 * -DMIST_BUILD_HASH=\"abcdef0\") and otherwise fall back to a constant.
 * The reviewer can flip this to read build/provenance.json later. */
#ifndef MIST_BUILD_HASH
#define MIST_BUILD_HASH "0000000"
#endif

static char s_last_reject[128] = { 0 };

static void set_reject_reason(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_reject, sizeof(s_last_reject), fmt, ap);
    va_end(ap);
}

const char* mist_handshake_last_reject_reason(void) {
    if (s_last_reject[0] == '\0') {
        return "";
    }
    return s_last_reject;
}

/* ------------------------------------------------------------------- */
/* Frame builders                                                      */
/* ------------------------------------------------------------------- */

static size_t append_cstr(uint8_t* out, size_t cap, size_t off, const char* s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1; /* include trailing NUL */
    if (off + n > cap) return 0;
    memcpy(out + off, s, n);
    return off + n;
}

static size_t build_frame(uint8_t msg_type,
                          const char* arch,
                          const char* platform,
                          const char* build_hash,
                          uint8_t reject_reason,
                          const char* reject_text,
                          uint8_t* out,
                          size_t cap) {
    if (!out || cap < MIST_HEADER_LEN) return 0;

    /* Reserve header; fill after we know the payload length. */
    size_t off = MIST_HEADER_LEN;

    if (msg_type == MIST_MSG_HELLO || msg_type == MIST_MSG_ACK) {
        off = append_cstr(out, cap, off, arch ? arch : MIST_ARCH_TAG);
        if (!off) return 0;
        off = append_cstr(out, cap, off, platform ? platform : MIST_PLATFORM_TAG);
        if (!off) return 0;
        off = append_cstr(out, cap, off, build_hash ? build_hash : MIST_BUILD_HASH);
        if (!off) return 0;
    } else if (msg_type == MIST_MSG_REJECT) {
        if (off + 1 > cap) return 0;
        out[off++] = reject_reason;
        off = append_cstr(out, cap, off, reject_text ? reject_text : "");
        if (!off) return 0;
    } else {
        return 0;
    }

    const size_t payload_len = off - MIST_HEADER_LEN;
    if (payload_len > MIST_PAYLOAD_MAX) return 0;

    out[0] = MIST_MAGIC_B0;
    out[1] = MIST_MAGIC_B1;
    out[2] = MIST_MAGIC_B2;
    out[3] = MIST_MAGIC_B3;
    out[4] = msg_type;
    out[5] = (uint8_t)((payload_len >> 8) & 0xFF); /* big-endian high */
    out[6] = (uint8_t)(payload_len & 0xFF);        /* big-endian low  */

    return off;
}

/* ------------------------------------------------------------------- */
/* Frame parsers                                                       */
/* ------------------------------------------------------------------- */

/*
 * parse_header — validate magic + msg_type + bounded length. Returns
 * true if the frame is a structurally well-formed MIST packet; false
 * otherwise (caller should drop and keep waiting).
 */
static bool parse_header(const uint8_t* buf, size_t len,
                         uint8_t* out_msg_type, size_t* out_payload_len) {
    if (len < MIST_HEADER_LEN) return false;
    if (buf[0] != MIST_MAGIC_B0 || buf[1] != MIST_MAGIC_B1 ||
        buf[2] != MIST_MAGIC_B2 || buf[3] != MIST_MAGIC_B3) return false;
    const uint8_t msg_type = buf[4];
    /* Accept the three tier-2 §8.2.4 message types:
     *   hello  = 0x01  (MIST_MSG_HELLO)
     *   ack    = 0x02  (MIST_MSG_ACK)
     *   reject = 0x03  (MIST_MSG_REJECT)
     * Any other value is dropped as not-a-MIST-frame. */
    if (msg_type != MIST_MSG_HELLO && msg_type != MIST_MSG_ACK && msg_type != MIST_MSG_REJECT) {
        return false;
    }
    const size_t payload_len = ((size_t)buf[5] << 8) | (size_t)buf[6];
    if (payload_len > MIST_PAYLOAD_MAX) return false;
    if (len < MIST_HEADER_LEN + payload_len) return false;
    *out_msg_type = msg_type;
    *out_payload_len = payload_len;
    return true;
}

/*
 * read_cstr — consume a null-terminated string from payload. Advances *off.
 * Returns false if no terminator inside the declared payload length.
 */
static bool read_cstr(const uint8_t* payload, size_t payload_len, size_t* off,
                      char* out, size_t out_cap) {
    size_t i = *off;
    while (i < payload_len && payload[i] != 0) i++;
    if (i >= payload_len) return false; /* no NUL */
    const size_t n = i - *off;
    if (out_cap == 0) return false;
    const size_t copy = n < out_cap - 1 ? n : out_cap - 1;
    memcpy(out, payload + *off, copy);
    out[copy] = '\0';
    *off = i + 1;
    return true;
}

/* ------------------------------------------------------------------- */
/* Time helpers                                                        */
/* ------------------------------------------------------------------- */

static long long now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
#endif
}

/* ------------------------------------------------------------------- */
/* Receive side — handle incoming hello and respond                    */
/* ------------------------------------------------------------------- */

static void respond_to_hello(int sock, const struct sockaddr* peer, socklen_t peer_len,
                             const uint8_t* payload, size_t payload_len) {
    char peer_arch[32] = { 0 };
    char peer_platform[32] = { 0 };
    char peer_build[32] = { 0 };

    size_t off = 0;
    (void)read_cstr(payload, payload_len, &off, peer_arch, sizeof(peer_arch));
    (void)read_cstr(payload, payload_len, &off, peer_platform, sizeof(peer_platform));
    (void)read_cstr(payload, payload_len, &off, peer_build, sizeof(peer_build));

    uint8_t frame[MIST_FRAME_MAX];
    size_t frame_len = 0;

    if (strcmp(peer_arch, MIST_ARCH_TAG) != 0) {
        frame_len = build_frame(MIST_MSG_REJECT, NULL, NULL, NULL,
                                MIST_REJECT_ARCH_MISMATCH,
                                "arch mismatch",
                                frame, sizeof(frame));
    } else if (strcmp(peer_platform, MIST_PLATFORM_TAG) != 0) {
        frame_len = build_frame(MIST_MSG_REJECT, NULL, NULL, NULL,
                                MIST_REJECT_PLATFORM_MISMATCH,
                                "platform mismatch",
                                frame, sizeof(frame));
    } else {
        frame_len = build_frame(MIST_MSG_ACK, MIST_ARCH_TAG, MIST_PLATFORM_TAG,
                                MIST_BUILD_HASH, 0, NULL,
                                frame, sizeof(frame));
    }

    if (frame_len > 0) {
        (void)sendto(sock, (const char*)frame, (int)frame_len, 0, peer, peer_len);
    }
}

/* ------------------------------------------------------------------- */
/* Sender side — public API                                            */
/* ------------------------------------------------------------------- */

bool mist_handshake_send_and_wait(int sock,
                                  const void* remote_addr,
                                  size_t remote_addrlen,
                                  int timeout_ms) {
    if (sock < 0 || !remote_addr || remote_addrlen == 0) {
        set_reject_reason("invalid argument");
        return false;
    }
    if (timeout_ms <= 0) timeout_ms = MIST_DEFAULT_TIMEOUT_MS;

    s_last_reject[0] = '\0';

    /* Build our hello once; we re-use it for every retransmit. */
    uint8_t hello[MIST_FRAME_MAX];
    const size_t hello_len = build_frame(MIST_MSG_HELLO,
                                         MIST_ARCH_TAG,
                                         MIST_PLATFORM_TAG,
                                         MIST_BUILD_HASH,
                                         0, NULL,
                                         hello, sizeof(hello));
    if (hello_len == 0) {
        set_reject_reason("hello frame build failed");
        return false;
    }

    const long long start = now_ms();
    const long long deadline = start + (long long)timeout_ms;
    int sends = 0;
    long long next_send_at = start; /* first send immediately */

    for (;;) {
        const long long t = now_ms();

        /* Retransmit hello. Budget: MIST_RETRANSMIT_COUNT sends. */
        if (t >= next_send_at && sends < MIST_RETRANSMIT_COUNT) {
            const ssize_t sent = sendto(sock, (const char*)hello, (int)hello_len, 0,
                                        (const struct sockaddr*)remote_addr,
                                        (socklen_t)remote_addrlen);
            (void)sent;
            sends++;
            next_send_at = t + MIST_RETRANSMIT_INTERVAL_MS;
        }

        /* Compute wait budget until next send or final deadline. */
        long long wait_until = deadline;
        if (sends < MIST_RETRANSMIT_COUNT && next_send_at < wait_until) {
            wait_until = next_send_at;
        }
        long long wait_ms = wait_until - t;
        if (wait_ms < 0) wait_ms = 0;
        if (t >= deadline) {
            set_reject_reason("timeout (peer did not respond)");
            return false;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv;
        tv.tv_sec = (long)(wait_ms / 1000);
        tv.tv_usec = (long)((wait_ms % 1000) * 1000);
        const int rc = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (MIST_SOCK_ERRNO == EINTR) continue;
            set_reject_reason("select() failed errno=%d", (int)MIST_SOCK_ERRNO);
            return false;
        }
        if (rc == 0) {
            /* Tick — loop back to check retransmit / deadline. */
            continue;
        }

        /* Drain available datagrams without blocking — we've been told
         * at least one is ready. */
        for (;;) {
            uint8_t buf[MIST_FRAME_MAX];
            struct sockaddr_storage src;
            socklen_t src_len = sizeof(src);

            /* Make the read non-blocking by re-checking select first. If
             * select says nothing is there, stop draining. */
            fd_set rfds2;
            FD_ZERO(&rfds2);
            FD_SET(sock, &rfds2);
            struct timeval tv0 = { 0, 0 };
            const int rc2 = select(sock + 1, &rfds2, NULL, NULL, &tv0);
            if (rc2 <= 0) break;

            const ssize_t n = recvfrom(sock, (char*)buf, sizeof(buf), 0,
                                       (struct sockaddr*)&src, &src_len);
            if (n <= 0) break;

            uint8_t msg_type = 0;
            size_t payload_len = 0;
            if (!parse_header(buf, (size_t)n, &msg_type, &payload_len)) {
                /* Not our frame (e.g., GekkoNet, wrong magic). Drop and keep
                 * waiting inside the budget. */
                continue;
            }
            const uint8_t* payload = buf + MIST_HEADER_LEN;

            if (msg_type == MIST_MSG_ACK) {
                /* Validate peer's profile. */
                char peer_arch[32] = { 0 };
                char peer_platform[32] = { 0 };
                char peer_build[32] = { 0 };
                size_t off = 0;
                if (!read_cstr(payload, payload_len, &off, peer_arch, sizeof(peer_arch)) ||
                    !read_cstr(payload, payload_len, &off, peer_platform, sizeof(peer_platform)) ||
                    !read_cstr(payload, payload_len, &off, peer_build, sizeof(peer_build))) {
                    continue; /* malformed ack — drop */
                }
                if (strcmp(peer_arch, MIST_ARCH_TAG) != 0 ||
                    strcmp(peer_platform, MIST_PLATFORM_TAG) != 0) {
                    set_reject_reason("peer ack arch/platform mismatch (%s/%s)",
                                      peer_arch, peer_platform);
                    return false;
                }
                s_last_reject[0] = '\0';
                return true;
            } else if (msg_type == MIST_MSG_REJECT) {
                uint8_t reason = MIST_REJECT_UNKNOWN;
                if (payload_len >= 1) reason = payload[0];
                char text[96] = { 0 };
                if (payload_len >= 2) {
                    size_t off = 1;
                    (void)read_cstr(payload, payload_len, &off, text, sizeof(text));
                }
                set_reject_reason("peer rejected: reason=%u %s",
                                  (unsigned)reason, text[0] ? text : "");
                return false;
            } else if (msg_type == MIST_MSG_HELLO) {
                /* Peer also called send_and_wait. Reply to keep the
                 * symmetric case working. */
                respond_to_hello(sock, (struct sockaddr*)&src, src_len,
                                 payload, payload_len);
                /* Keep waiting for their ack to our hello. */
                continue;
            }
        }
    }
}

/* ------------------------------------------------------------------- */
/* SDL_net-compatible helpers (not fd-based)                           */
/* ------------------------------------------------------------------- */

size_t mist_handshake_build_hello(uint8_t* out, size_t cap) {
    return build_frame(MIST_MSG_HELLO, MIST_ARCH_TAG, MIST_PLATFORM_TAG,
                       MIST_BUILD_HASH, 0, NULL, out, cap);
}

int mist_handshake_parse_response(const uint8_t* buf, size_t len) {
    uint8_t msg_type = 0;
    size_t payload_len = 0;
    if (!parse_header(buf, len, &msg_type, &payload_len)) {
        return -2; /* not ours — drop */
    }
    const uint8_t* payload = buf + MIST_HEADER_LEN;
    if (msg_type == MIST_MSG_ACK) {
        char peer_arch[32] = { 0 };
        char peer_platform[32] = { 0 };
        char peer_build[32] = { 0 };
        size_t off = 0;
        if (!read_cstr(payload, payload_len, &off, peer_arch, sizeof(peer_arch)) ||
            !read_cstr(payload, payload_len, &off, peer_platform, sizeof(peer_platform)) ||
            !read_cstr(payload, payload_len, &off, peer_build, sizeof(peer_build))) {
            set_reject_reason("malformed ack");
            return -1;
        }
        if (strcmp(peer_arch, MIST_ARCH_TAG) != 0) {
            set_reject_reason("peer ack arch mismatch (%s)", peer_arch);
            return -1;
        }
        if (strcmp(peer_platform, MIST_PLATFORM_TAG) != 0) {
            set_reject_reason("peer ack platform mismatch (%s)", peer_platform);
            return -1;
        }
        s_last_reject[0] = '\0';
        return 1;
    }
    if (msg_type == MIST_MSG_REJECT) {
        uint8_t reason = MIST_REJECT_UNKNOWN;
        if (payload_len >= 1) reason = payload[0];
        char text[96] = { 0 };
        if (payload_len >= 2) {
            size_t off = 1;
            (void)read_cstr(payload, payload_len, &off, text, sizeof(text));
        }
        set_reject_reason("peer rejected: reason=%u %s", (unsigned)reason,
                          text[0] ? text : "");
        return -1;
    }
    if (msg_type == MIST_MSG_HELLO) {
        return 0;
    }
    return -2;
}

size_t mist_handshake_build_reply(const uint8_t* in_payload,
                                  size_t in_payload_len,
                                  uint8_t* out,
                                  size_t cap) {
    char peer_arch[32] = { 0 };
    char peer_platform[32] = { 0 };
    char peer_build[32] = { 0 };
    size_t off = 0;
    (void)read_cstr(in_payload, in_payload_len, &off, peer_arch, sizeof(peer_arch));
    (void)read_cstr(in_payload, in_payload_len, &off, peer_platform, sizeof(peer_platform));
    (void)read_cstr(in_payload, in_payload_len, &off, peer_build, sizeof(peer_build));

    if (strcmp(peer_arch, MIST_ARCH_TAG) != 0) {
        return build_frame(MIST_MSG_REJECT, NULL, NULL, NULL,
                           MIST_REJECT_ARCH_MISMATCH, "arch mismatch",
                           out, cap);
    }
    if (strcmp(peer_platform, MIST_PLATFORM_TAG) != 0) {
        return build_frame(MIST_MSG_REJECT, NULL, NULL, NULL,
                           MIST_REJECT_PLATFORM_MISMATCH, "platform mismatch",
                           out, cap);
    }
    return build_frame(MIST_MSG_ACK, MIST_ARCH_TAG, MIST_PLATFORM_TAG,
                       MIST_BUILD_HASH, 0, NULL, out, cap);
}

/* ------------------------------------------------------------------- */
/* Test-only hooks                                                     */
/* ------------------------------------------------------------------- */

#ifdef ENABLE_NETPLAY_TESTS

size_t mist_handshake_build_frame(uint8_t msg_type,
                                  const char* arch,
                                  const char* platform,
                                  const char* build_hash,
                                  uint8_t reject_reason,
                                  const char* reject_text,
                                  uint8_t* out,
                                  size_t cap) {
    return build_frame(msg_type, arch, platform, build_hash,
                       reject_reason, reject_text, out, cap);
}

size_t mist_handshake_build_bad_magic(uint8_t* out, size_t cap) {
    if (!out || cap < MIST_HEADER_LEN) return 0;
    /* Same shape as a hello but wrong magic byte. */
    out[0] = 'X';
    out[1] = MIST_MAGIC_B1;
    out[2] = MIST_MAGIC_B2;
    out[3] = MIST_MAGIC_B3;
    out[4] = MIST_MSG_ACK;
    out[5] = 0;
    out[6] = 0;
    return MIST_HEADER_LEN;
}

void mist_handshake_test_reset(void) {
    s_last_reject[0] = '\0';
}

#endif /* ENABLE_NETPLAY_TESTS */
