/*
 * late_punch.c — task #119. See late_punch.h for the capability this
 * adds and the security argument; this file is only the mechanism.
 *
 * Compiled into BOTH the game (the top-level GLOB_RECURSE over src,
 * netplay builds only) and the natmatrix p2p_probe, deliberately: the
 * netns proof of the late-connect rescue runs THIS code, not a
 * reimplementation.
 */

#include "netplay/late_punch.h"

#include "netplay/netplay.h"
#include "netplay/stun.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <string.h>

static bool     s_armed = false;
static uint8_t  s_payload[STUN_PUNCH_PAYLOAD_LEN];
static uint8_t  s_token[STUN_PUNCH_TOKEN_LEN];
static char     s_peer_ip[64];
static uint16_t s_peer_port;
static struct NET_DatagramSocket* s_sock; /* borrowed — never closed here */

/* Send-side state. The peer address is resolved lazily (NET_Resolve is
 * async even for a dotted quad) and re-resolved after every retarget. */
static NET_Address* s_peer_addr = NULL;
static uint32_t s_last_tx_ms = 0;
static bool     s_tx_prompt = false; /* a valid punch arrived — answer now */

/* Relearn event, produced by HandleDatagram, consumed one-shot by
 * TakeRelearn on the same (main) thread. */
static bool s_relearn_pending = false;
static int  s_relearn_count = 0;

/* Rate-limited advisory counters (same 1st-then-every-50th shape as the
 * host punch gate's advisories in direct_p2p.c). */
static int s_drop_bad_token = 0;
static int s_drop_wrong_ip = 0;

static void late_punch_release_addr(void) {
    if (s_peer_addr != NULL) {
        NET_UnrefAddress(s_peer_addr);
        s_peer_addr = NULL;
    }
}

void LatePunch_Arm(struct NET_DatagramSocket* sock,
                   const uint8_t token[STUN_PUNCH_TOKEN_LEN],
                   const char* peer_ip, uint16_t peer_port) {
    LatePunch_Disarm();
    if (sock == NULL || token == NULL || peer_ip == NULL ||
        peer_ip[0] == '\0' || peer_port == 0) {
        return; /* stay disarmed — nothing to speak to */
    }
    memcpy(s_token, token, sizeof(s_token));
    Stun_BuildPunchPayload(s_token, s_payload);
    SDL_strlcpy(s_peer_ip, peer_ip, sizeof(s_peer_ip));
    s_peer_port = peer_port;
    s_sock = sock;
    s_last_tx_ms = 0;
    s_tx_prompt = false;
    s_relearn_pending = false;
    s_relearn_count = 0;
    s_drop_bad_token = 0;
    s_drop_wrong_ip = 0;
    s_armed = true;
    SDL_Log("[late_punch] armed for peer %s:%u (answering authenticated "
            "punches until the session starts)",
            s_peer_ip, (unsigned)s_peer_port);
}

void LatePunch_Disarm(void) {
    if (s_armed) {
        SDL_Log("[late_punch] disarmed (relearns=%d bad_token_drops=%d "
                "wrong_ip_drops=%d)",
                s_relearn_count, s_drop_bad_token, s_drop_wrong_ip);
    }
    s_armed = false;
    s_sock = NULL;
    s_relearn_pending = false;
    late_punch_release_addr();
    /* The token is a secret derived from the room code; don't leave it
     * in a dead static longer than the session that owned it. */
    SDL_memset(s_token, 0, sizeof(s_token));
    SDL_memset(s_payload, 0, sizeof(s_payload));
}

bool LatePunch_IsArmed(void) {
    return s_armed;
}

bool LatePunch_HandleDatagram(const uint8_t* buf, int len,
                              const char* src_ip, uint16_t src_port) {
    if (!s_armed || buf == NULL || src_ip == NULL) {
        return false;
    }
    if (!Stun_HasPunchPrefix(buf, len)) {
        return false; /* not punch traffic — MIST / GekkoNet frame etc. */
    }
    /* Punch-shaped from here on: CONSUMED regardless of verdict, so a
     * stray or hostile punch never reaches GekkoNet's deserializer. */
    if (!Stun_IsPunchPayload(buf, len, s_token)) {
        s_drop_bad_token++;
        if (s_drop_bad_token == 1 || (s_drop_bad_token % 50) == 0) {
            SDL_Log("[late_punch] dropped punch-shaped datagram with a bad "
                    "token from %s:%u (#%d) — wrong room code or replay",
                    src_ip, (unsigned)src_port, s_drop_bad_token);
        }
        return true;
    }
    if (strcmp(src_ip, s_peer_ip) != 0) {
        /* Valid token, WRONG source IP. The pre-handoff host gate would
         * have accepted this (any-source capture); post-handoff we are
         * stricter — the peer's IP is already established and a
         * retrying joiner keeps its NAT's public IP, so a cross-IP
         * "peer" is a recorded-payload replay from a third party (or a
         * mid-connect CGNAT IP flip, accepted as a residual: that pair
         * re-hosts). No answer either way: answering would hand a
         * replayer a confirmation oracle. */
        s_drop_wrong_ip++;
        if (s_drop_wrong_ip == 1 || (s_drop_wrong_ip % 50) == 0) {
            SDL_Log("[late_punch] ignored valid-token punch from foreign IP "
                    "%s:%u (#%d, peer is %s) — not retargeting",
                    src_ip, (unsigned)src_port, s_drop_wrong_ip, s_peer_ip);
        }
        return true;
    }
    if (src_port != s_peer_port) {
        /* The peer's mapping moved — the S2 retry's fresh socket, or a
         * NAT rebind. Retarget, bounded. */
        if (s_relearn_count >= LATE_PUNCH_MAX_RELEARNS) {
            SDL_Log("[late_punch] relearn cap (%d) hit — keeping %s:%u, "
                    "ignoring move to port %u",
                    LATE_PUNCH_MAX_RELEARNS, s_peer_ip,
                    (unsigned)s_peer_port, (unsigned)src_port);
            return true;
        }
        s_relearn_count++;
        char line[192];
        SDL_snprintf(line, sizeof(line),
                     "[late_punch] RELEARN #%d: authenticated peer moved "
                     "%s:%u -> %s:%u — retargeting session sends",
                     s_relearn_count, s_peer_ip, (unsigned)s_peer_port,
                     src_ip, (unsigned)src_port);
        Netplay_LogConnectEvent(line);
        s_peer_port = src_port;
        late_punch_release_addr(); /* re-resolve toward the new endpoint */
        s_relearn_pending = true;
    }
    /* Answer promptly: our next payload is what confirms the peer's leg
     * (Stun_PunchOffer accepts the byte-identical payload from our IP).
     * The cadence in Tick rate-limits this — prompt only skips the wait,
     * it cannot exceed one datagram per Tick call. */
    s_tx_prompt = true;
    return true;
}

void LatePunch_Tick(uint32_t now_ms) {
    if (!s_armed || s_sock == NULL) {
        return;
    }
    if (!s_tx_prompt && s_last_tx_ms != 0 &&
        (now_ms - s_last_tx_ms) < LATE_PUNCH_TX_INTERVAL_MS) {
        return;
    }
    if (s_peer_addr == NULL) {
        s_peer_addr = NET_ResolveHostname(s_peer_ip);
        if (s_peer_addr == NULL) {
            return; /* retry next tick */
        }
    }
    switch (NET_GetAddressStatus(s_peer_addr)) {
    case NET_SUCCESS:
        (void)NET_SendDatagram(s_sock, s_peer_addr, s_peer_port,
                               s_payload, (int)sizeof(s_payload));
        s_last_tx_ms = (now_ms != 0) ? now_ms : 1u;
        s_tx_prompt = false;
        break;
    case NET_FAILURE:
        /* A dotted quad cannot fail to resolve; treat as transient. */
        late_punch_release_addr();
        break;
    default: /* NET_WAITING — try again next tick */
        break;
    }
}

bool LatePunch_TakeRelearn(char* ip_out, size_t ip_cap, uint16_t* port_out) {
    if (!s_relearn_pending) {
        return false;
    }
    s_relearn_pending = false;
    if (ip_out != NULL && ip_cap > 0) {
        SDL_strlcpy(ip_out, s_peer_ip, ip_cap);
    }
    if (port_out != NULL) {
        *port_out = s_peer_port;
    }
    return true;
}

#ifdef ENABLE_NETPLAY_TESTS
/* Task #132. Read-only window onto the decision state HandleDatagram
 * produces, so the three verdicts that need no socket can be asserted
 * without one.
 *
 * Without it the send TARGET is only observable by actually sending —
 * which means a loopback socket, a Tick, a delivery wait, and a clock
 * advance past LATE_PUNCH_TX_INTERVAL_MS. That is exactly what makes the
 * two properties below untestable today: "a refused move still targets
 * the LEARNED endpoint" and "a foreign-IP punch does not trigger a
 * prompt answer" both disappear the moment you advance the clock far
 * enough to observe a datagram, because the cadenced keepalive would
 * have sent one anyway.
 *
 * Read-only and test-only: this changes no production behaviour and the
 * symbol does not exist in the shipped build. */
void LatePunch_TestPeek(char* ip_out, size_t ip_cap, uint16_t* port_out,
                        bool* tx_prompt_out, int* relearn_count_out) {
    if (ip_out != NULL && ip_cap > 0) {
        SDL_strlcpy(ip_out, s_peer_ip, ip_cap);
    }
    if (port_out != NULL) {
        *port_out = s_peer_port;
    }
    if (tx_prompt_out != NULL) {
        *tx_prompt_out = s_tx_prompt;
    }
    if (relearn_count_out != NULL) {
        *relearn_count_out = s_relearn_count;
    }
}
#endif /* ENABLE_NETPLAY_TESTS */
