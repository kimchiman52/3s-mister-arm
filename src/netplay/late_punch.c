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

/* Relearn event, produced by the candidate promotion below, consumed
 * one-shot by TakeRelearn on the same (main) thread. */
static bool s_relearn_pending = false;
static int  s_relearn_count = 0;

/* Task #133 liveness challenge. A token-valid punch from a new port no
 * longer moves anything by itself; it only nominates a CANDIDATE, which
 * has to answer an unpredictable nonce before it becomes the send
 * target. One candidate at a time, held for LATE_PUNCH_CHALLENGE_MS.
 *
 * All the time arithmetic lives in Tick(now_ms), never in
 * HandleDatagram: the receive path has no clock argument and the module
 * is deliberately free of SDL_GetTicks so a test can drive the whole
 * decision without one. */
static bool     s_cand_active = false;
static uint16_t s_cand_port = 0;
static uint8_t  s_cand_nonce[STUN_PUNCH_PROBE_NONCE_LEN];
static uint8_t  s_cand_frame[STUN_PUNCH_PROBE_PAYLOAD_LEN];
static uint32_t s_cand_first_ms = 0;  /* valid only when s_cand_timed */
static uint32_t s_cand_last_tx_ms = 0; /* valid only when s_cand_sent */
static bool     s_cand_timed = false;
static bool     s_cand_sent = false;
static bool     s_cand_tx_due = false;

/* Answering someone ELSE's challenge. Parked by HandleDatagram, emitted
 * by Tick. One slot: a challenge flood costs one 26-byte reply per Tick. */
static bool     s_echo_pending = false;
static uint16_t s_echo_port = 0;
static uint8_t  s_echo_frame[STUN_PUNCH_PROBE_PAYLOAD_LEN];

/* Rate-limited advisory counters (same 1st-then-every-50th shape as the
 * host punch gate's advisories in direct_p2p.c). */
static int s_drop_bad_token = 0;
static int s_drop_wrong_ip = 0;
static int s_drop_unverified = 0; /* candidates that never answered */
static int s_drop_busy = 0;       /* nominated while another was pending */
static int s_drop_stale_resp = 0; /* RESPONSE matching no live challenge */

static void late_punch_release_addr(void) {
    if (s_peer_addr != NULL) {
        NET_UnrefAddress(s_peer_addr);
        s_peer_addr = NULL;
    }
}

static void late_punch_clear_candidate(void) {
    s_cand_active = false;
    s_cand_port = 0;
    s_cand_timed = false;
    s_cand_sent = false;
    s_cand_tx_due = false;
    SDL_memset(s_cand_nonce, 0, sizeof(s_cand_nonce));
    SDL_memset(s_cand_frame, 0, sizeof(s_cand_frame));
}

/* A token-valid punch arrived from the established peer IP at a port we
 * are not sending to. Under task #119 this WAS the relearn; it is now
 * only a nomination. Nothing about the send path changes here — not the
 * target, not the relearn budget, not even the prompt-answer flag — so
 * an endpoint that never answers costs the legitimate peer nothing but
 * the candidate slot, and only until it expires. */
static void late_punch_nominate(uint16_t port) {
    if (s_relearn_count >= LATE_PUNCH_MAX_RELEARNS) {
        SDL_Log("[late_punch] relearn budget (%d) spent — keeping %s:%u, "
                "ignoring move to port %u",
                LATE_PUNCH_MAX_RELEARNS, s_peer_ip, (unsigned)s_peer_port,
                (unsigned)port);
        return;
    }
    if (s_cand_active) {
        if (port != s_cand_port) {
            s_drop_busy++;
            if (s_drop_busy == 1 || (s_drop_busy % 50) == 0) {
                SDL_Log("[late_punch] port %u nominated while %u is still "
                        "answering its challenge (#%d) — first come, and it "
                        "expires in %u ms",
                        (unsigned)port, (unsigned)s_cand_port, s_drop_busy,
                        (unsigned)LATE_PUNCH_CHALLENGE_MS);
            }
        }
        return;
    }
    if (!Stun_MakeProbeNonce(s_cand_nonce)) {
        /* Fail CLOSED. A challenge the nominee can predict proves nothing,
         * so with no CSPRNG there is no relearn — the pre-#119 behaviour,
         * which is a delayed failure, not a captured session. */
        SDL_Log("[late_punch] CSPRNG unavailable — refusing to challenge "
                "port %u, and refusing to retarget without one",
                (unsigned)port);
        return;
    }
    Stun_BuildProbePayload(s_token, STUN_PUNCH_PROBE_CHALLENGE, s_cand_nonce,
                           s_cand_frame);
    s_cand_port = port;
    s_cand_active = true;
    s_cand_timed = false;
    s_cand_sent = false;
    s_cand_tx_due = true;
    SDL_Log("[late_punch] peer %s nominated port %u (current target %u) — "
            "challenging it before any retarget",
            s_peer_ip, (unsigned)port, (unsigned)s_peer_port);
}

/* The candidate answered our nonce. THIS is the relearn, and it is the
 * only thing that spends the budget. */
static void late_punch_promote(void) {
    s_relearn_count++;
    char line[224];
    SDL_snprintf(line, sizeof(line),
                 "[late_punch] RELEARN #%d/%d: authenticated peer moved "
                 "%s:%u -> %s:%u — liveness challenge answered, retargeting "
                 "session sends",
                 s_relearn_count, LATE_PUNCH_MAX_RELEARNS, s_peer_ip,
                 (unsigned)s_peer_port, s_peer_ip, (unsigned)s_cand_port);
    Netplay_LogConnectEvent(line);
    s_peer_port = s_cand_port;
    late_punch_release_addr(); /* re-resolve toward the new endpoint */
    s_relearn_pending = true;
    s_tx_prompt = true; /* the new endpoint gets our answer immediately */
    late_punch_clear_candidate();
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
    s_drop_unverified = 0;
    s_drop_busy = 0;
    s_drop_stale_resp = 0;
    s_echo_pending = false;
    s_echo_port = 0;
    late_punch_clear_candidate();
    s_armed = true;
    SDL_Log("[late_punch] armed for peer %s:%u (answering authenticated "
            "punches until the session starts)",
            s_peer_ip, (unsigned)s_peer_port);
}

void LatePunch_Disarm(void) {
    if (s_armed) {
        SDL_Log("[late_punch] disarmed (relearns=%d/%d bad_token_drops=%d "
                "wrong_ip_drops=%d unanswered_challenges=%d busy_drops=%d "
                "stale_responses=%d)",
                s_relearn_count, LATE_PUNCH_MAX_RELEARNS, s_drop_bad_token,
                s_drop_wrong_ip, s_drop_unverified, s_drop_busy,
                s_drop_stale_resp);
    }
    s_armed = false;
    s_sock = NULL;
    s_relearn_pending = false;
    s_echo_pending = false;
    late_punch_clear_candidate();
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

    /* Task #133 liveness probe frames, checked before the 17-byte payload
     * because they are a different length and cannot be confused with it. */
    {
        uint8_t kind = 0;
        uint8_t nonce[STUN_PUNCH_PROBE_NONCE_LEN];
        if (Stun_ParseProbePayload(buf, len, s_token, &kind, nonce)) {
            if (strcmp(src_ip, s_peer_ip) != 0) {
                /* Same rule as a foreign-IP punch, and for the same
                 * reason: no answer, so a replayer gets no oracle. */
                s_drop_wrong_ip++;
                if (s_drop_wrong_ip == 1 || (s_drop_wrong_ip % 50) == 0) {
                    SDL_Log("[late_punch] ignored valid-token probe from "
                            "foreign IP %s:%u (#%d, peer is %s)",
                            src_ip, (unsigned)src_port, s_drop_wrong_ip,
                            s_peer_ip);
                }
                return true;
            }
            if (kind == STUN_PUNCH_PROBE_CHALLENGE) {
                /* The peer is verifying US — it has handed off too and is
                 * about to retarget at this endpoint. Answering costs one
                 * 26-byte datagram and is the mirror of what we demand. */
                Stun_BuildProbePayload(s_token, STUN_PUNCH_PROBE_RESPONSE,
                                       nonce, s_echo_frame);
                s_echo_port = src_port;
                s_echo_pending = true;
                return true;
            }
            /* RESPONSE. The ONLY input that can move the send target. It
             * must come from the port we challenged and carry the nonce we
             * drew for it — neither of which a party that merely holds the
             * room code can supply without having actually received our
             * challenge at that endpoint. */
            if (s_cand_active && src_port == s_cand_port &&
                Stun_ProbeNonceEqual(nonce, s_cand_nonce)) {
                late_punch_promote();
            } else {
                s_drop_stale_resp++;
                if (s_drop_stale_resp == 1 || (s_drop_stale_resp % 50) == 0) {
                    SDL_Log("[late_punch] liveness response from %s:%u matches "
                            "no live challenge (#%d) — ignored",
                            src_ip, (unsigned)src_port, s_drop_stale_resp);
                }
            }
            return true;
        }
    }

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
         * NAT rebind — OR someone else on the peer's public IP is holding
         * the room code. Task #133: those two are indistinguishable here,
         * so this only NOMINATES. No answer is scheduled either: the
         * prompt below would hand an unverified endpoint the confirmation
         * it wants. The challenge is the only thing it gets. */
        late_punch_nominate(src_port);
        return true;
    }
    /* Answer promptly: our next payload is what confirms the peer's leg
     * (Stun_PunchOffer accepts the byte-identical payload from our IP).
     * The cadence in Tick rate-limits this — prompt only skips the wait,
     * it cannot exceed one datagram per Tick call. */
    s_tx_prompt = true;
    return true;
}

/* Candidate bookkeeping: start the clock on a fresh nomination, expire
 * one that never answered, and say whether a challenge datagram is due.
 * Deliberately runs BEFORE the socket check in Tick so the expiry is
 * observable in a test that has no socket. */
static bool late_punch_challenge_due(uint32_t now_ms) {
    if (!s_cand_active) {
        return false;
    }
    if (!s_cand_timed) {
        s_cand_first_ms = now_ms;
        s_cand_timed = true;
    } else if ((now_ms - s_cand_first_ms) >= LATE_PUNCH_CHALLENGE_MS) {
        s_drop_unverified++;
        SDL_Log("[late_punch] port %u did not answer the liveness challenge "
                "in %u ms (#%d) — NOT retargeting, and the relearn budget "
                "is untouched (%d/%d)",
                (unsigned)s_cand_port, (unsigned)LATE_PUNCH_CHALLENGE_MS,
                s_drop_unverified, s_relearn_count, LATE_PUNCH_MAX_RELEARNS);
        late_punch_clear_candidate();
        return false;
    }
    if (s_cand_tx_due || !s_cand_sent) {
        return true;
    }
    return (now_ms - s_cand_last_tx_ms) >= LATE_PUNCH_CHALLENGE_RETX_MS;
}

void LatePunch_Tick(uint32_t now_ms) {
    if (!s_armed) {
        return;
    }
    /* Time-based candidate state first — it must advance even on a tick
     * that sends nothing. */
    const bool want_challenge = late_punch_challenge_due(now_ms);
    if (s_sock == NULL) {
        return;
    }
    const bool want_echo = s_echo_pending;
    const bool want_keepalive =
        s_tx_prompt || s_last_tx_ms == 0 ||
        (now_ms - s_last_tx_ms) >= LATE_PUNCH_TX_INTERVAL_MS;
    if (!want_echo && !want_challenge && !want_keepalive) {
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
        break;
    case NET_FAILURE:
        /* A dotted quad cannot fail to resolve; treat as transient. */
        late_punch_release_addr();
        return;
    default: /* NET_WAITING — try again next tick */
        return;
    }
    /* All three sends go to the one established peer IP; only the port
     * differs, so they share the resolved address. */
    if (want_echo) {
        (void)NET_SendDatagram(s_sock, s_peer_addr, s_echo_port, s_echo_frame,
                               (int)sizeof(s_echo_frame));
        s_echo_pending = false;
    }
    if (want_challenge) {
        (void)NET_SendDatagram(s_sock, s_peer_addr, s_cand_port, s_cand_frame,
                               (int)sizeof(s_cand_frame));
        s_cand_last_tx_ms = now_ms;
        s_cand_sent = true;
        s_cand_tx_due = false;
    }
    if (want_keepalive) {
        (void)NET_SendDatagram(s_sock, s_peer_addr, s_peer_port,
                               s_payload, (int)sizeof(s_payload));
        s_last_tx_ms = (now_ms != 0) ? now_ms : 1u;
        s_tx_prompt = false;
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

/* Task #133. The liveness challenge is the whole mitigation, and every
 * property worth pinning about it is invisible from outside: whether a
 * nomination opened a challenge at all, which port it went to, and what
 * nonce it carries. The nonce especially — a test that cannot read it
 * cannot forge a CORRECT response, and a test that only ever forges
 * wrong ones proves the module rejects everything, which is not the
 * property under test.
 *
 * Test-only, read-only, and absent from the shipped build: this hands
 * out the challenge nonce, which is exactly the value production must
 * never disclose to anyone who did not receive the challenge datagram. */
void LatePunch_TestPeekChallenge(bool* active_out, uint16_t* port_out,
                                 uint8_t nonce_out[STUN_PUNCH_PROBE_NONCE_LEN],
                                 bool* echo_pending_out,
                                 uint16_t* echo_port_out) {
    if (active_out != NULL) {
        *active_out = s_cand_active;
    }
    if (port_out != NULL) {
        *port_out = s_cand_port;
    }
    if (nonce_out != NULL) {
        memcpy(nonce_out, s_cand_nonce, STUN_PUNCH_PROBE_NONCE_LEN);
    }
    if (echo_pending_out != NULL) {
        *echo_pending_out = s_echo_pending;
    }
    if (echo_port_out != NULL) {
        *echo_port_out = s_echo_port;
    }
}
#endif /* ENABLE_NETPLAY_TESTS */
