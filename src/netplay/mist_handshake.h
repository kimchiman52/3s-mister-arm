/*
 * mist_handshake.h — Phase 6 Step 8: Layer-3 MiSTer-arch handshake.
 *
 * Ports tier-2 §8.2.4 of docs/plan-netplay-port.md. After STUN pairing and
 * BEFORE GekkoNet starts on the paired socket, peers exchange a 2-way
 * "MIST" magic-prefix frame to confirm both sides are 32-bit armv7 MiSTer
 * builds. If the opponent fails to ack within 500 ms — or sends a reject —
 * the session aborts before the first GekkoNet SyncRequest, preventing
 * silent desyncs caused by the SessionHealthMsg checksum divergence across
 * architectures (research §9.7).
 *
 * Wire format (big-endian where noted):
 *   offset  size   field
 *   ------  ----   -----
 *   0       4      magic "MIST" (0x4D 0x49 0x53 0x54)
 *   4       1      msg_type (0x01 hello | 0x02 ack | 0x03 reject)
 *   5       2      payload_len (big-endian, max MIST_PAYLOAD_MAX)
 *   7       N      payload
 *
 * Hello/ack payload (R-1 layout, proto_ver 1):
 *   three null-terminated strings
 *     "armv7\0" "mister\0" "<build_hash_7chars>\0"
 *   followed by two fixed-width compatibility fields:
 *     +0  u8   proto_ver   (MIST_PROTO_VER, currently 1)
 *     +1  u16  state_ver   (big-endian; sizeof(GameState) — equals the
 *                           EXPECTED_GAME_STATE_SIZE pin on 32-bit builds
 *                           via the _Static_assert in game_state.c)
 *   Peers reject on state_ver or proto_ver mismatch (different GameState
 *   layouts desync mid-match); build_hash difference is a warning only.
 *   Pre-R-1 peers end the payload after the three strings; the missing
 *   version fields classify them as legacy-incompatible (their GameState
 *   layout predates the current pin) and they are rejected cleanly.
 *
 * Reject payload: one-byte reason code (mist_reject_reason_t) followed by
 * an optional null-terminated human-readable string.
 *
 * Retransmit: sender fires hello every 100 ms up to 5 times; accepts an
 * ack/reject from the peer at any point inside the 500 ms window.
 *
 * Collision safety: GekkoNet's wire type enum at byte 0 is in [0, 6] per
 * /tmp/GekkoNet-head/GekkoLib/include/net.h:26-36, so our 0x4D 'M' magic
 * cannot collide with a live GekkoNet packet.
 */
#ifndef NETPLAY_MIST_HANDSHAKE_H
#define NETPLAY_MIST_HANDSHAKE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keep the magic / message-type constants visible to the test harness so it
 * can craft raw frames without duplicating literals. */
#define MIST_MAGIC_B0 0x4D /* 'M' */
#define MIST_MAGIC_B1 0x49 /* 'I' */
#define MIST_MAGIC_B2 0x53 /* 'S' */
#define MIST_MAGIC_B3 0x54 /* 'T' */
#define MIST_MAGIC_LEN 4
#define MIST_HEADER_LEN 7 /* magic(4) + msg_type(1) + payload_len(2) */

#define MIST_MSG_HELLO 0x01
#define MIST_MSG_ACK 0x02
#define MIST_MSG_REJECT 0x03

#define MIST_PAYLOAD_MAX 128
#define MIST_FRAME_MAX (MIST_HEADER_LEN + MIST_PAYLOAD_MAX)

/* Retransmit: 5× hello at 100 ms; total budget 500 ms. */
#define MIST_RETRANSMIT_COUNT 5
#define MIST_RETRANSMIT_INTERVAL_MS 100
#define MIST_DEFAULT_TIMEOUT_MS 500

/* Arch / platform strings we advertise. */
#define MIST_ARCH_TAG "armv7"
#define MIST_PLATFORM_TAG "mister"

/* R-1: handshake protocol version, first byte after the three payload
 * strings. Bump when the hello/ack payload layout changes again. */
#define MIST_PROTO_VER 1

/* Reject reason code at payload[0]. Sent as a single unsigned byte.
 * Values are wire-stable — append only, never renumber. */
typedef enum {
    MIST_REJECT_UNKNOWN         = 0,
    MIST_REJECT_ARCH_MISMATCH   = 1,
    MIST_REJECT_PLATFORM_MISMATCH = 2,
    MIST_REJECT_BUILD_MISMATCH  = 3,
    MIST_REJECT_MALFORMED       = 4,
    /* R-1 additions: */
    MIST_REJECT_LEGACY          = 5, /* payload ends after the strings — pre-R-1 build */
    MIST_REJECT_STATE_MISMATCH  = 6, /* sizeof(GameState) differs — would desync */
    MIST_REJECT_PROTO_MISMATCH  = 7, /* MIST_PROTO_VER differs */
} mist_reject_reason_t;

extern const uint8_t MIST_MAGIC[MIST_MAGIC_LEN];

/*
 * mist_handshake_send_and_wait — perform a 2-way MIST handshake.
 *
 * @param sock           Connected-or-bindable SOCK_DGRAM file descriptor.
 *                       Caller owns the socket; this function neither
 *                       creates nor closes it. Socket MUST be left in
 *                       blocking mode; we internally drive it via select().
 * @param remote_addr    Peer address (ipv4). Not NULL.
 * @param remote_addrlen Size of *remote_addr (sizeof(struct sockaddr_in)).
 * @param timeout_ms     Total budget in ms for the whole exchange. 0 or
 *                       negative falls back to MIST_DEFAULT_TIMEOUT_MS.
 * @return true on ack, false on reject or timeout. Use
 *         mist_handshake_last_reject_reason() for a human-readable reason.
 *
 * The function also services one inbound hello (opposite direction of
 * Layer-3) and replies with ack/reject, so a symmetric MiSTer peer does
 * not need to call this twice.
 */
bool mist_handshake_send_and_wait(int sock,
                                  const void* remote_addr,
                                  size_t remote_addrlen,
                                  int timeout_ms);

/*
 * mist_handshake_build_hello — build our standard hello frame. Used by
 * the SDL_net wrapper in netplay.c. Returns bytes written, 0 on failure.
 */
size_t mist_handshake_build_hello(uint8_t* out, size_t cap);

/*
 * mist_handshake_parse_response — classify a received frame.
 * Returns:
 *   +1  frame is a valid ack from a compatible peer.
 *    0  frame is an inbound hello — caller should respond (ack or reject).
 *   -1  frame is a reject or incompatible ack (reject reason cached in
 *       mist_handshake_last_reject_reason()).
 *   -2  frame is not a MIST frame — caller drops and keeps waiting.
 */
int mist_handshake_parse_response(const uint8_t* buf, size_t len);

/*
 * mist_handshake_build_reply — given an inbound hello payload, produce
 * the appropriate ack or reject frame. `in_payload` points at the
 * payload bytes after the 7-byte header; `in_payload_len` is its length.
 */
size_t mist_handshake_build_reply(const uint8_t* in_payload,
                                  size_t in_payload_len,
                                  uint8_t* out,
                                  size_t cap);

/*
 * mist_handshake_last_reject_reason — returns the most recent failure
 * reason as a human-readable string. Valid after a false return from
 * mist_handshake_send_and_wait(). Always non-NULL; returns "ok" if the
 * last exchange succeeded and "" if none has run yet.
 */
const char* mist_handshake_last_reject_reason(void);

/*
 * mist_handshake_local_state_ver — the state_ver this build advertises
 * on the wire: (uint16_t)sizeof(GameState). On 32-bit builds this equals
 * the EXPECTED_GAME_STATE_SIZE pin (enforced by the _Static_assert in
 * game_state.c). Exposed for logging and for the unit test to craft
 * mismatching frames without hardcoding the number.
 */
uint16_t mist_handshake_local_state_ver(void);

/* ------------------------------------------------------------------- */
/* R-1 adv-review M-5: testable live-runner core.                      */
/*                                                                     */
/* The production handshake loop used to live inline in netplay.c's    */
/* run_mist_handshake_on_net_sock() where no test could reach it. The  */
/* loop now lives here as mist_handshake_run_attempt(), driven through */
/* a small IO vtable: netplay.c supplies an SDL3_net-backed adapter    */
/* for the live session socket, and the unit test supplies an          */
/* in-memory adapter with a fake clock, so every branch (tri-state     */
/* result, retransmit budget, gratuitous ack, reject latching) is      */
/* testable deterministically without sockets or real time.            */
/* ------------------------------------------------------------------- */

/* Tri-state result of one handshake attempt (one MIST_DEFAULT_TIMEOUT_MS
 * budget): OK / silent timeout (retryable — the peer may not have
 * reached its own gate yet) / hard failure (explicit reject, classified
 * incompatibility, or broken local transport). */
typedef enum {
    MIST_HS_OK = 0,
    MIST_HS_TIMEOUT,
    MIST_HS_FAIL,
} MistHandshakeResult;

/* Peer-skew tolerance: the two peers reach the TRANSITIONING gate at
 * slightly different times (cold-launch re-exec on the OSD host path can
 * leave one peer in Init_Task seconds longer than the other). A silent
 * 500 ms timeout therefore must NOT hard-fail immediately — the peer may
 * simply not be listening yet. Each attempt keeps the plan's 500 ms
 * budget; the caller retries across TRANSITIONING ticks up to this cap
 * before declaring the peer incompatible/unreachable (40 x 500 ms ≈ 20 s
 * of active waiting). An explicit reject still fails immediately. */
#define MIST_HANDSHAKE_MAX_ATTEMPTS 40

/* What the session gate should do after one attempt — the retry-policy
 * mapping that used to live inline in Netplay_Run()'s TRANSITIONING
 * case. Extracted so the attempt-cap/exhaustion-message behavior is unit
 * testable (adv-review M-5). */
typedef enum {
    MIST_GATE_PROCEED = 0, /* handshake done — start GekkoNet */
    MIST_GATE_RETRY,       /* silent timeout, budget remains — retry next tick */
    MIST_GATE_FAIL,        /* hard fail — tear the session down */
} MistGateAction;

/* R-1 adv-review H-1: valid first-byte range of a live GekkoNet
 * datagram. GekkoNet serializes MsgHeader first and its PacketType enum
 * (u8, byte 0 on the wire) spans Inputs=1 .. NetworkHealth=7 — pinned
 * third_party/GekkoNet/build/include/net.h:28-36. Used by the runner's
 * implicit-completion guard: once THIS session has classified the
 * session peer's hello as compatible, Gekko-shaped traffic from that
 * peer proves the peer already completed its own gate (during the
 * handshake phase a peer only ever emits MIST frames, first byte 0x4D
 * 'M', or hole-punch keepalives "3SX_PUNCH", first byte 0x33 — nothing
 * in [1, 7]) and is accepted as handshake success. */
#define MIST_GEKKO_PACKET_TYPE_MIN 1
#define MIST_GEKKO_PACKET_TYPE_MAX 7

/*
 * IO vtable the runner core drives. All callbacks are non-NULL.
 *
 *   send_to_peer       — send a frame to the session peer (hellos and the
 *                        completion-race gratuitous ack).
 *   recv               — non-blocking: copy the next queued datagram into
 *                        `buf` (truncating to `cap` — only MIST frames,
 *                        which fit MIST_FRAME_MAX, are ever parsed beyond
 *                        byte 0). Returns the copied byte count, or 0 when
 *                        nothing is pending (a zero-length datagram reads
 *                        as nothing pending; the next call resumes the
 *                        drain). Sets *from_session_peer to true when the
 *                        datagram's source address AND port match the
 *                        session peer.
 *   send_reply_to_last — send a frame to the source of the most recently
 *                        recv'd datagram (ack/reject answers to a hello).
 *   now_ms             — monotonic milliseconds.
 *   delay_ms           — sleep (keeps the live loop from burning CPU; a
 *                        test clock advances virtual time here instead).
 */
typedef struct MistRunnerIo {
    void* ctx;
    void (*send_to_peer)(void* ctx, const uint8_t* buf, size_t len);
    int (*recv)(void* ctx, uint8_t* buf, size_t cap, bool* from_session_peer);
    void (*send_reply_to_last)(void* ctx, const uint8_t* buf, size_t len);
    uint64_t (*now_ms)(void* ctx);
    void (*delay_ms)(void* ctx, uint32_t ms);
} MistRunnerIo;

/*
 * mist_handshake_run_attempt — one full handshake attempt over `io`:
 * retransmits our hello (MIST_RETRANSMIT_COUNT x MIST_RETRANSMIT_INTERVAL_MS),
 * answers inbound hellos, and classifies replies, within one
 * MIST_DEFAULT_TIMEOUT_MS budget. On MIST_HS_TIMEOUT / MIST_HS_FAIL a
 * human-readable reason is written to `reason` (sized `reason_cap`).
 *
 * `peer_hello_ok` (R-1 adv-review H-1) is the caller-owned,
 * SESSION-scoped "the session peer's hello classified compatible" latch
 * that arms the implicit-completion path. The stranded-completion race:
 * peer A completes the handshake (its hello reached us and ours reached
 * it), starts GekkoNet the same tick, and its single gratuitous ack to
 * us is lost — our further hellos land in A's GekkoNet, which drops
 * them silently (pinned backend.cpp:132-151 catches the deserialize
 * throw), so two IDENTICAL builds would burn the full retry budget and
 * hard-fail with a misleading no-reply message. Fix: Gekko-shaped
 * traffic (first byte in [MIST_GEKKO_PACKET_TYPE_MIN,
 * MIST_GEKKO_PACKET_TYPE_MAX]) arriving FROM THE SESSION PEER is
 * treated as handshake success — but ONLY when *peer_hello_ok is
 * already true. The guard is what keeps the compatibility gate closed:
 * the latch is set exclusively by classify_peer_payload() returning 0
 * for a hello whose source address+port match the session peer, i.e.
 * the full arch/platform/proto_ver/state_ver check passed. A legacy or
 * mismatched peer can never set it, so blasting GekkoNet traffic at the
 * gate cannot bypass validation — every path to MIST_HS_OK still runs
 * the peer's advertised profile through the same classifier (explicit
 * path validates the peer's ack payload; implicit path validated the
 * peer's hello payload).
 *
 * Lifecycle: the caller must clear the latch at session start and at
 * session teardown, alongside its attempt counter. It must persist
 * ACROSS attempts within one session — the race spans attempts (the
 * hello classifies clean in one 500 ms attempt, the peer's Gekko
 * traffic arrives in a later one).
 */
MistHandshakeResult mist_handshake_run_attempt(const MistRunnerIo* io,
                                               bool* peer_hello_ok,
                                               char* reason,
                                               size_t reason_cap);

/*
 * mist_handshake_gate_next — fold one attempt result into the session
 * gate's retry state. `attempts` is the caller-owned consecutive-timeout
 * counter (reset to 0 on MIST_HS_OK). On the timeout that exhausts
 * `max_attempts`, overwrites `reason` with the no-reply explanation
 * (every pre-R-1 build never answers MIST hellos — see the caller's
 * comment in netplay.c).
 */
MistGateAction mist_handshake_gate_next(MistHandshakeResult hs,
                                        int* attempts,
                                        int max_attempts,
                                        char* reason,
                                        size_t reason_cap);

#ifdef ENABLE_NETPLAY_TESTS
/* Test-only helpers. Implementation details surfaced for the unit test
 * to drive a synthetic peer over a loopback socket. */

/* Build the local hello/ack/reject frame into `out` (capacity `cap`).
 * Returns the number of bytes written, or 0 on failure. */
size_t mist_handshake_build_frame(uint8_t msg_type,
                                  const char* arch,
                                  const char* platform,
                                  const char* build_hash,
                                  uint8_t reject_reason,
                                  const char* reject_text,
                                  uint8_t* out,
                                  size_t cap);

/* R-1 tests — like mist_handshake_build_frame but with explicit
 * proto_ver / state_ver so the test can craft mismatching hello/ack
 * frames (wrong state size, wrong protocol version). */
size_t mist_handshake_build_frame_ex(uint8_t msg_type,
                                     const char* arch,
                                     const char* platform,
                                     const char* build_hash,
                                     uint8_t proto_ver,
                                     uint16_t state_ver,
                                     uint8_t reject_reason,
                                     const char* reject_text,
                                     uint8_t* out,
                                     size_t cap);

/* R-1 tests — craft a pre-R-1 hello/ack: three strings, NO version
 * fields. Exercises the legacy-peer reject path. */
size_t mist_handshake_build_legacy_frame(uint8_t msg_type,
                                         const char* arch,
                                         const char* platform,
                                         const char* build_hash,
                                         uint8_t* out,
                                         size_t cap);

/* Hosts test (d) — craft a malformed frame (wrong magic). */
size_t mist_handshake_build_bad_magic(uint8_t* out, size_t cap);

/* Reset the cached reject reason (test isolation). */
void mist_handshake_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_MIST_HANDSHAKE_H */
