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

/* R-1: the state_ver compatibility field is sizeof(GameState), read
 * symbolically so it auto-tracks future re-pins of the rollback state
 * layout. On 32-bit builds this equals EXPECTED_GAME_STATE_SIZE via the
 * _Static_assert in game_state.c (17676 as of the #296 port). */
#include "netplay/game_state.h"

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

/* Build hash advertised in hello/ack payloads. R-1: wired for real —
 * CMakeLists.txt derives the git short SHA at configure time and passes
 * it as a per-source COMPILE_DEFINITIONS on this file. The constant
 * fallback covers builds where git is unavailable (e.g. tarball or a
 * container without repo access). Difference is a WARNING only, never a
 * reject: same build_hash is not required for compatibility, only same
 * state_ver/proto_ver are. */
#ifndef MIST_BUILD_HASH
#define MIST_BUILD_HASH "0000000"
#endif

/* R-1: wire value for the state_ver field (big-endian u16 on the wire). */
#define MIST_STATE_VER ((uint16_t)sizeof(GameState))

uint16_t mist_handshake_local_state_ver(void) {
    return MIST_STATE_VER;
}

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
                          uint8_t proto_ver,
                          uint16_t state_ver,
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
        /* R-1 compatibility fields: proto_ver (u8) then state_ver (u16,
         * big-endian — matches the payload_len field's byte order). */
        if (off + 3 > cap) return 0;
        out[off++] = proto_ver;
        out[off++] = (uint8_t)((state_ver >> 8) & 0xFF);
        out[off++] = (uint8_t)(state_ver & 0xFF);
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

/* R-1: bounds-checked fixed-width readers for the version fields. This
 * parser receives attacker-controlled bytes; every read must stay inside
 * the declared payload length (which parse_header already bounded by
 * MIST_PAYLOAD_MAX and by the actual received datagram length). A short
 * read returns false — never reads out of bounds. */
static bool read_u8(const uint8_t* payload, size_t payload_len, size_t* off,
                    uint8_t* out) {
    if (*off + 1 > payload_len) return false;
    *out = payload[*off];
    *off += 1;
    return true;
}

static bool read_u16be(const uint8_t* payload, size_t payload_len, size_t* off,
                       uint16_t* out) {
    if (*off + 2 > payload_len) return false;
    *out = (uint16_t)(((uint16_t)payload[*off] << 8) | (uint16_t)payload[*off + 1]);
    *off += 2;
    return true;
}

/*
 * classify_peer_payload — parse + validate a hello/ack payload against
 * our own profile. Returns 0 when the peer is compatible; otherwise a
 * mist_reject_reason_t value, with a short human-readable explanation
 * written to `text` (sized for the direct-P2P overlay's status line).
 *
 * Reject-worthy (hard incompatibility):
 *   - malformed strings                → MIST_REJECT_MALFORMED
 *   - arch / platform tag mismatch     → MIST_REJECT_{ARCH,PLATFORM}_MISMATCH
 *   - payload ends after the strings   → MIST_REJECT_LEGACY (pre-R-1 build)
 *   - proto_ver differs                → MIST_REJECT_PROTO_MISMATCH
 *   - state_ver differs                → MIST_REJECT_STATE_MISMATCH — THE
 *     desync-preventing check: different sizeof(GameState) means the two
 *     builds save/load different rollback layouts and will diverge.
 * Warning only (never rejects):
 *   - build_hash differs — logged; identical hashes are not required for
 *     compatibility, identical state layout is.
 */
static uint8_t classify_peer_payload(const uint8_t* payload, size_t payload_len,
                                     char* text, size_t text_cap) {
    char peer_arch[32] = { 0 };
    char peer_platform[32] = { 0 };
    char peer_build[32] = { 0 };
    size_t off = 0;

    if (!read_cstr(payload, payload_len, &off, peer_arch, sizeof(peer_arch)) ||
        !read_cstr(payload, payload_len, &off, peer_platform, sizeof(peer_platform)) ||
        !read_cstr(payload, payload_len, &off, peer_build, sizeof(peer_build))) {
        snprintf(text, text_cap, "malformed handshake payload");
        return MIST_REJECT_MALFORMED;
    }
    if (strcmp(peer_arch, MIST_ARCH_TAG) != 0) {
        snprintf(text, text_cap, "arch mismatch (%s)", peer_arch);
        return MIST_REJECT_ARCH_MISMATCH;
    }
    if (strcmp(peer_platform, MIST_PLATFORM_TAG) != 0) {
        snprintf(text, text_cap, "platform mismatch (%s)", peer_platform);
        return MIST_REJECT_PLATFORM_MISMATCH;
    }

    uint8_t peer_proto = 0;
    uint16_t peer_state = 0;
    if (!read_u8(payload, payload_len, &off, &peer_proto) ||
        !read_u16be(payload, payload_len, &off, &peer_state)) {
        /* Strings parsed clean but the payload ends there: a pre-R-1
         * build. Its GameState layout predates the current pin, so it is
         * incompatible by construction. */
        snprintf(text, text_cap, "Opponent build too old - both need this update");
        return MIST_REJECT_LEGACY;
    }
    if (peer_proto != MIST_PROTO_VER) {
        snprintf(text, text_cap, "Handshake v%u vs v%u - update one side",
                 (unsigned)peer_proto, (unsigned)MIST_PROTO_VER);
        return MIST_REJECT_PROTO_MISMATCH;
    }
    if (peer_state != MIST_STATE_VER) {
        /* Symmetric phrasing — this exact text is also what the OTHER
         * peer displays when we send it inside our reject frame. */
        snprintf(text, text_cap, "Build state %u vs %u - update one side",
                 (unsigned)peer_state, (unsigned)MIST_STATE_VER);
        return MIST_REJECT_STATE_MISMATCH;
    }

    if (strcmp(peer_build, MIST_BUILD_HASH) != 0) {
        fprintf(stderr,
                "[mist_handshake] WARNING: peer build_hash %s != ours %s "
                "(state_ver %u matches; proceeding)\n",
                peer_build, MIST_BUILD_HASH, (unsigned)MIST_STATE_VER);
    }
    return 0;
}

/* Fallback text for an inbound reject frame that carries a reason code
 * but no (or an empty) human-readable string. */
static const char* reject_reason_fallback_text(uint8_t reason) {
    switch (reason) {
    case MIST_REJECT_ARCH_MISMATCH:     return "arch mismatch";
    case MIST_REJECT_PLATFORM_MISMATCH: return "platform mismatch";
    case MIST_REJECT_BUILD_MISMATCH:    return "build mismatch";
    case MIST_REJECT_MALFORMED:         return "malformed handshake";
    case MIST_REJECT_LEGACY:            return "build too old - update needed";
    case MIST_REJECT_STATE_MISMATCH:    return "game state version mismatch - update one side";
    case MIST_REJECT_PROTO_MISMATCH:    return "handshake version mismatch - update one side";
    default:                            return "unknown reason";
    }
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
    /* R-1: same validation + framing as the SDL_net path — one shared
     * implementation in mist_handshake_build_reply (classify, then ack
     * or reject with reason). */
    uint8_t frame[MIST_FRAME_MAX];
    const size_t frame_len = mist_handshake_build_reply(payload, payload_len,
                                                        frame, sizeof(frame));
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
                                         MIST_PROTO_VER,
                                         MIST_STATE_VER,
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
                /* Validate peer's profile (R-1: includes proto_ver /
                 * state_ver — see classify_peer_payload). */
                char why[96] = { 0 };
                const uint8_t reason = classify_peer_payload(payload, payload_len,
                                                             why, sizeof(why));
                if (reason == MIST_REJECT_MALFORMED) {
                    continue; /* garbled ack — drop, keep waiting */
                }
                if (reason != 0) {
                    set_reject_reason("%s", why);
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
                set_reject_reason("%s", text[0] ? text
                                              : reject_reason_fallback_text(reason));
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
                       MIST_BUILD_HASH, MIST_PROTO_VER, MIST_STATE_VER,
                       0, NULL, out, cap);
}

int mist_handshake_parse_response(const uint8_t* buf, size_t len) {
    uint8_t msg_type = 0;
    size_t payload_len = 0;
    if (!parse_header(buf, len, &msg_type, &payload_len)) {
        return -2; /* not ours — drop */
    }
    const uint8_t* payload = buf + MIST_HEADER_LEN;
    if (msg_type == MIST_MSG_ACK) {
        /* R-1: full compatibility validation, including proto_ver and
         * state_ver. A pre-R-1 ack (three strings only) classifies as
         * MIST_REJECT_LEGACY. Malformed stays a hard -1 here (unchanged
         * from the pre-R-1 behavior of this helper). */
        char why[96] = { 0 };
        const uint8_t reason = classify_peer_payload(payload, payload_len,
                                                     why, sizeof(why));
        if (reason != 0) {
            set_reject_reason("%s", why);
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
        set_reject_reason("%s", text[0] ? text
                                        : reject_reason_fallback_text(reason));
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
    /* R-1: classify the inbound hello against our full profile; reply
     * with an ack when compatible, otherwise a reject that carries both
     * the machine reason code and the human-readable explanation (the
     * peer displays that text on its overlay). */
    char why[96] = { 0 };
    const uint8_t reason = classify_peer_payload(in_payload, in_payload_len,
                                                 why, sizeof(why));
    if (reason != 0) {
        return build_frame(MIST_MSG_REJECT, NULL, NULL, NULL, 0, 0,
                           reason, why, out, cap);
    }
    return build_frame(MIST_MSG_ACK, MIST_ARCH_TAG, MIST_PLATFORM_TAG,
                       MIST_BUILD_HASH, MIST_PROTO_VER, MIST_STATE_VER,
                       0, NULL, out, cap);
}

/* ------------------------------------------------------------------- */
/* R-1 adv-review M-5: testable live-runner core                       */
/* ------------------------------------------------------------------- */

/* Receive scratch size for the runner. GekkoNet datagrams can exceed
 * MIST_FRAME_MAX; the runner only ever inspects byte 0 of non-MIST
 * traffic, and every MIST frame fits MIST_FRAME_MAX, so truncation to
 * this cap never affects classification. */
#define MIST_RUNNER_RECV_CAP 2048

/* Idle sleep between drain passes — keeps the live loop from burning
 * 100% CPU (same 5 ms the pre-extraction netplay.c loop used). */
#define MIST_RUNNER_IDLE_DELAY_MS 5

/* R-1 adv-review H-2b: before the runner returns a hard failure, answer
 * any peer hellos already sitting in the receive queue. Without this the
 * peer never hears WHY the pair is incompatible — it burns its whole
 * retry budget and reports the generic no-reply message instead of the
 * real mismatch. One non-blocking pass over the currently queued
 * datagrams; each hello gets the classify-derived ack/reject reply
 * (retransmitted hellos naturally get the reject re-sent, which also
 * raises the reject frame's delivery odds). Non-hello frames drop. */
static void drain_and_answer_hellos(const MistRunnerIo* io) {
    uint8_t buf[MIST_RUNNER_RECV_CAP];
    bool from_peer = false;
    int n;
    while ((n = io->recv(io->ctx, buf, sizeof(buf), &from_peer)) > 0) {
        uint8_t msg_type = 0;
        size_t declared_len = 0;
        if (!parse_header(buf, (size_t)n, &msg_type, &declared_len)) {
            continue;
        }
        if (msg_type != MIST_MSG_HELLO) {
            continue;
        }
        uint8_t reply[MIST_FRAME_MAX];
        const size_t reply_len = mist_handshake_build_reply(
            buf + MIST_HEADER_LEN, declared_len, reply, sizeof(reply));
        if (reply_len > 0) {
            io->send_reply_to_last(io->ctx, reply, reply_len);
        }
    }
}

MistHandshakeResult mist_handshake_run_attempt(const MistRunnerIo* io,
                                               bool* peer_hello_ok,
                                               char* reason,
                                               size_t reason_cap) {
    uint8_t hello[MIST_FRAME_MAX];
    const size_t hello_len = mist_handshake_build_hello(hello, sizeof(hello));
    if (hello_len == 0) {
        snprintf(reason, reason_cap, "hello build failed");
        return MIST_HS_FAIL;
    }

    const uint64_t start_ms = io->now_ms(io->ctx);
    const uint64_t deadline_ms = start_ms + (uint64_t)MIST_DEFAULT_TIMEOUT_MS;
    int sends = 0;
    uint64_t next_send_ms = start_ms;

    for (;;) {
        const uint64_t now = io->now_ms(io->ctx);
        if (now >= deadline_ms) {
            snprintf(reason, reason_cap, "timeout (peer did not respond)");
            return MIST_HS_TIMEOUT;
        }
        if (now >= next_send_ms && sends < MIST_RETRANSMIT_COUNT) {
            io->send_to_peer(io->ctx, hello, hello_len);
            sends++;
            next_send_ms = now + MIST_RETRANSMIT_INTERVAL_MS;
        }

        /* Drain pending datagrams. */
        uint8_t buf[MIST_RUNNER_RECV_CAP];
        bool from_peer = false;
        int n;
        while ((n = io->recv(io->ctx, buf, sizeof(buf), &from_peer)) > 0) {
            const int cls = mist_handshake_parse_response(buf, (size_t)n);
            if (cls == 1) {
                /* Completion race guard: our side is done, but the peer
                 * still needs an ack for ITS hello. If its hello was
                 * reordered behind the ack we just consumed (or lost),
                 * the peer would never complete — we'd start GekkoNet
                 * and it would time out. Send one gratuitous ack so the
                 * peer completes regardless of hello arrival order.
                 * Redundant acks are harmless: a peer that already
                 * completed has GekkoNet on the socket, which drops
                 * non-Gekko frames. Building the reply from our OWN
                 * hello payload always yields an ack (we classify
                 * ourselves as compatible). */
                uint8_t final_ack[MIST_FRAME_MAX];
                const size_t final_ack_len =
                    mist_handshake_build_reply(hello + MIST_HEADER_LEN,
                                               hello_len - MIST_HEADER_LEN,
                                               final_ack, sizeof(final_ack));
                if (final_ack_len > 0) {
                    io->send_to_peer(io->ctx, final_ack, final_ack_len);
                }
                /* H-1 hardening: also answer any peer hellos ALREADY
                 * queued behind this ack right now — the race window is
                 * exactly "peer's hello arrived after the ack we just
                 * consumed"; answering the queued ones deterministically
                 * closes that half (the implicit-completion path below
                 * covers hellos that arrive after we return). */
                drain_and_answer_hellos(io);
                return MIST_HS_OK;
            }
            if (cls == -1) {
                /* Explicit reject frame, or an ack that classified as
                 * incompatible. Reason was cached by parse_response.
                 * H-2b: answer any hellos already queued behind this
                 * frame before failing, so the peer learns the real
                 * reason instead of timing out on the generic one. */
                const char* r = mist_handshake_last_reject_reason();
                snprintf(reason, reason_cap, "%s", r[0] ? r : "peer rejected");
                drain_and_answer_hellos(io);
                return MIST_HS_FAIL;
            }
            if (cls == 0) {
                /* Peer also runs the handshake — reply with ack/reject.
                 * Use the DECLARED payload length from the header
                 * (parse_header re-derives it; cls == 0 guarantees the
                 * frame already passed parse_header, so this cannot
                 * fail) rather than the raw datagram tail, so trailing
                 * garbage can never be misparsed as payload fields. */
                uint8_t msg_type = 0;
                size_t declared_len = 0;
                if (!parse_header(buf, (size_t)n, &msg_type, &declared_len)) {
                    continue; /* unreachable — defensive */
                }
                char why[96] = { 0 };
                const uint8_t hello_reason = classify_peer_payload(
                    buf + MIST_HEADER_LEN, declared_len, why, sizeof(why));
                uint8_t reply[MIST_FRAME_MAX];
                size_t reply_len;
                if (hello_reason != 0) {
                    reply_len = build_frame(MIST_MSG_REJECT, NULL, NULL, NULL,
                                            0, 0, hello_reason, why,
                                            reply, sizeof(reply));
                    if (reply_len > 0) {
                        io->send_reply_to_last(io->ctx, reply, reply_len);
                    }
                    /* H-2a: the peer's hello proves the pair is
                     * incompatible — fail OUR side too, with the same
                     * classify text the reject frame carries, instead
                     * of burning the remaining retry budget waiting
                     * for an ack that can never validly arrive. The
                     * one exception is MALFORMED: a garbled datagram
                     * that happens to carry MIST magic proves nothing
                     * about the peer's build (its retransmitted hello
                     * will classify properly), so it must not kill a
                     * session between identical builds — reject-reply
                     * and keep waiting. */
                    if (hello_reason != MIST_REJECT_MALFORMED) {
                        snprintf(reason, reason_cap, "%s", why);
                        drain_and_answer_hellos(io);
                        return MIST_HS_FAIL;
                    }
                } else {
                    reply_len = build_frame(MIST_MSG_ACK, MIST_ARCH_TAG,
                                            MIST_PLATFORM_TAG, MIST_BUILD_HASH,
                                            MIST_PROTO_VER, MIST_STATE_VER,
                                            0, NULL, reply, sizeof(reply));
                    if (reply_len > 0) {
                        io->send_reply_to_last(io->ctx, reply, reply_len);
                    }
                    /* H-1: latch "the SESSION PEER's hello passed the
                     * full compatibility check this session". Arms the
                     * implicit-completion path below. Source-gated so a
                     * third party's hello can never arm it. */
                    if (from_peer) {
                        *peer_hello_ok = true;
                    }
                }
            }
            if (cls == -2) {
                /* Not a MIST frame. H-1 implicit completion: if the
                 * session peer's hello already classified compatible
                 * (this session) and the peer is now sending
                 * Gekko-shaped traffic, the peer has completed its own
                 * gate and started GekkoNet — its gratuitous ack was
                 * lost and any hello of ours now lands in its GekkoNet,
                 * which drops non-Gekko frames silently. Waiting longer
                 * can never succeed; treat it as handshake success.
                 * Guard rationale + why this cannot re-open the
                 * compatibility bypass: see mist_handshake.h
                 * (mist_handshake_run_attempt docs). Anything else —
                 * punch keepalives ("3SX_PUNCH", 0x33), STUN, garbage,
                 * third-party sources — is dropped as before. */
                if (*peer_hello_ok && from_peer &&
                    buf[0] >= MIST_GEKKO_PACKET_TYPE_MIN &&
                    buf[0] <= MIST_GEKKO_PACKET_TYPE_MAX) {
                    return MIST_HS_OK;
                }
                /* else: drop and keep listening. */
            }
        }

        io->delay_ms(io->ctx, MIST_RUNNER_IDLE_DELAY_MS);
    }
}

MistGateAction mist_handshake_gate_next(MistHandshakeResult hs,
                                        int* attempts,
                                        int max_attempts,
                                        char* reason,
                                        size_t reason_cap) {
    if (hs == MIST_HS_OK) {
        *attempts = 0;
        return MIST_GATE_PROCEED;
    }
    if (hs == MIST_HS_TIMEOUT) {
        *attempts += 1;
        if (*attempts < max_attempts) {
            /* Silent peer — likely still booting toward its own gate
             * (cold-launch skew). Caller stays in TRANSITIONING and
             * retries next tick; each attempt keeps the 500 ms budget. */
            return MIST_GATE_RETRY;
        }
        /* Exhausted every retry with zero MIST traffic: either the
         * connection died, or the opponent runs a build that predates
         * the handshake (every pre-R-1 release) — which also predates
         * the current GameState layout. Say so. */
        snprintf(reason, reason_cap, "No reply - opponent build may be too old");
        return MIST_GATE_FAIL;
    }
    return MIST_GATE_FAIL;
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
                       MIST_PROTO_VER, MIST_STATE_VER,
                       reject_reason, reject_text, out, cap);
}

size_t mist_handshake_build_frame_ex(uint8_t msg_type,
                                     const char* arch,
                                     const char* platform,
                                     const char* build_hash,
                                     uint8_t proto_ver,
                                     uint16_t state_ver,
                                     uint8_t reject_reason,
                                     const char* reject_text,
                                     uint8_t* out,
                                     size_t cap) {
    return build_frame(msg_type, arch, platform, build_hash,
                       proto_ver, state_ver,
                       reject_reason, reject_text, out, cap);
}

size_t mist_handshake_build_legacy_frame(uint8_t msg_type,
                                         const char* arch,
                                         const char* platform,
                                         const char* build_hash,
                                         uint8_t* out,
                                         size_t cap) {
    /* Byte-identical to what a pre-R-1 build_frame emitted for hello/ack:
     * header + three strings, no version fields. */
    if (!out || cap < MIST_HEADER_LEN) return 0;
    if (msg_type != MIST_MSG_HELLO && msg_type != MIST_MSG_ACK) return 0;

    size_t off = MIST_HEADER_LEN;
    off = append_cstr(out, cap, off, arch ? arch : MIST_ARCH_TAG);
    if (!off) return 0;
    off = append_cstr(out, cap, off, platform ? platform : MIST_PLATFORM_TAG);
    if (!off) return 0;
    off = append_cstr(out, cap, off, build_hash ? build_hash : MIST_BUILD_HASH);
    if (!off) return 0;

    const size_t payload_len = off - MIST_HEADER_LEN;
    if (payload_len > MIST_PAYLOAD_MAX) return 0;
    out[0] = MIST_MAGIC_B0;
    out[1] = MIST_MAGIC_B1;
    out[2] = MIST_MAGIC_B2;
    out[3] = MIST_MAGIC_B3;
    out[4] = msg_type;
    out[5] = (uint8_t)((payload_len >> 8) & 0xFF);
    out[6] = (uint8_t)(payload_len & 0xFF);
    return off;
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
