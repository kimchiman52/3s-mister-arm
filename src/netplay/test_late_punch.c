/*
 * test_late_punch.c — task #119 unit harness for the late-punch rescue
 * layer's ACCEPT/REJECT/RELEARN decision (src/netplay/late_punch.c).
 *
 * WHY A UNIT LAYER, when this project's netplay testing is nearly all
 * integration harnesses: the netns proof (rescue_scenario.sh) shows the
 * rescue CONNECTS; it cannot show that the layer refuses what it must
 * refuse. A subtle mistake in HandleDatagram is an AUTHENTICATION bug —
 * a redirect oracle — and every such case looks identical to success in
 * a connectivity test. HandleDatagram is deliberately near-pure (its
 * only effects are the return value, the one-shot relearn event, and a
 * prompt-answer flag; it sends nothing), so the decision is tested
 * directly, in-process. The send side (LatePunch_Tick) takes its clock
 * as a parameter, so cadence is tested without sleeps; only datagram
 * DELIVERY uses real loopback sockets.
 *
 * Decisions pinned here, so changing one is a deliberate act:
 *   - valid token, expected endpoint      -> consumed, no relearn
 *   - valid token, same IP, new port      -> consumed, RELEARN (bounded)
 *   - valid token, FOREIGN IP             -> consumed, NO relearn, and
 *     the keepalive keeps targeting the established peer (no redirect)
 *   - wrong/truncated/oversized token     -> consumed (punch-shaped),
 *     NO relearn — including from a same-IP new port
 *   - token from a PREVIOUS hosting attempt (nonce regenerates per
 *     attempt, direct_p2p.c s_work.nonce) -> rejected like any wrong
 *     token, because the token derivation binds the nonce
 *   - replay of a valid payload           -> idempotent (punches are
 *     stateless 17-byte constants by design; from the peer's endpoint a
 *     replay changes nothing, from elsewhere it cannot redirect — the
 *     accepted residual is a same-IP spoofer bouncing the port, which
 *     the relearn cap bounds)
 *   - disarmed (pre-handoff / post-SessionStarted / non-P2P sessions)
 *     -> NOTHING is consumed, no state accrues
 *
 * Exit codes, per this repo's hard-won doctrine: 0 all passed, 1 any
 * failure (each printed loudly with its case tag), 2 not compiled in —
 * which the gate runner treats as a MISBUILD, never a pass.
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/late_punch.h"
#include "netplay/netplay.h"
#include "netplay/rendezvous.h"
#include "netplay/stun.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static int fail_count = 0;

#define CHECK(tag, cond)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "[test_late_punch] FAIL: %s:%d: %s: %s\n",       \
                    __FILE__, __LINE__, (tag), #cond);                       \
            fail_count++;                                                    \
        }                                                                    \
    } while (0)

/* Drain one datagram from `sock`, waiting up to ~500 ms for delivery.
 * Returns true and copies payload/source when one arrived. */
static bool recv_one(NET_DatagramSocket* sock, uint8_t* buf, int cap,
                     int* len_out, uint16_t* src_port_out) {
    for (int spin = 0; spin < 100; spin++) {
        NET_Datagram* d = NULL;
        if (NET_ReceiveDatagram(sock, &d) && d != NULL) {
            int n = d->buflen < cap ? d->buflen : cap;
            memcpy(buf, d->buf, (size_t)n);
            if (len_out) *len_out = d->buflen;
            if (src_port_out) *src_port_out = d->port;
            NET_DestroyDatagram(d);
            return true;
        }
        SDL_Delay(5);
    }
    return false;
}

static bool recv_none(NET_DatagramSocket* sock) {
    SDL_Delay(50);
    NET_Datagram* d = NULL;
    if (NET_ReceiveDatagram(sock, &d) && d != NULL) {
        NET_DestroyDatagram(d);
        return false;
    }
    return true;
}

/* Pump Tick until one send actually leaves (the first Tick after Arm can
 * legitimately send nothing while the peer address resolves), advancing
 * the fake clock past the cadence each pump so the rate limit never
 * blocks resolution retries. Each attempt WAITS for delivery before
 * advancing the clock, so at most one datagram is ever outstanding —
 * without the wait, a slow loopback delivery makes the loop send twice
 * and the leftover datagram fails the next quiet-check. Returns the
 * clock value at the send that was received. */
static uint32_t tick_until_sent(NET_DatagramSocket* rx, uint32_t now,
                                uint8_t* buf, int cap, int* len_out) {
    for (int spin = 0; spin < 40; spin++) {
        LatePunch_Tick(now);
        for (int wait = 0; wait < 20; wait++) {
            NET_Datagram* d = NULL;
            if (NET_ReceiveDatagram(rx, &d) && d != NULL) {
                int n = d->buflen < cap ? d->buflen : cap;
                memcpy(buf, d->buf, (size_t)n);
                if (len_out) *len_out = d->buflen;
                NET_DestroyDatagram(d);
                return now;
            }
            SDL_Delay(5);
        }
        now += LATE_PUNCH_TX_INTERVAL_MS + 1;
    }
    if (len_out) *len_out = -1;
    return now;
}

int Netplay_Test_LatePunch(void) {
    if (!SDL_Init(0) || !NET_Init()) {
        fprintf(stderr, "[test_late_punch] FAIL: SDL/NET init: %s\n", SDL_GetError());
        return 1;
    }

    /* Three loopback sockets: A is "ours" (armed), B is the peer's
     * established endpoint, C is the peer's post-retry endpoint. */
    NET_Address* lo = NET_ResolveHostname("127.0.0.1");
    for (int spin = 0; spin < 100 && NET_GetAddressStatus(lo) == NET_WAITING; spin++) {
        SDL_Delay(5);
    }
    if (lo == NULL || NET_GetAddressStatus(lo) != NET_SUCCESS) {
        fprintf(stderr, "[test_late_punch] FAIL: cannot resolve 127.0.0.1\n");
        return 1;
    }
    NET_DatagramSocket* sockA = NULL;
    NET_DatagramSocket* sockB = NULL;
    NET_DatagramSocket* sockC = NULL;
    uint16_t portB = 0, portC = 0;
    /* Fixed candidate ports, scanned so a busy machine cannot fail the
     * harness; the pair (B, C) must differ, which the scan guarantees. */
    for (uint16_t p = 42911; p < 42991 && (sockB == NULL || sockC == NULL || sockA == NULL); p++) {
        if (sockA == NULL) { sockA = NET_CreateDatagramSocket(lo, p); continue; }
        if (sockB == NULL) { sockB = NET_CreateDatagramSocket(lo, p); portB = p; continue; }
        if (sockC == NULL) { sockC = NET_CreateDatagramSocket(lo, p); portC = p; continue; }
    }
    if (sockA == NULL || sockB == NULL || sockC == NULL) {
        fprintf(stderr, "[test_late_punch] FAIL: could not bind three loopback sockets\n");
        return 1;
    }

    /* Two attempts' tokens: same advertised tuple, different nonce — the
     * situation after a host re-hosts (s_work.nonce regenerates). */
    uint8_t token[STUN_PUNCH_TOKEN_LEN];
    uint8_t stale_token[STUN_PUNCH_TOKEN_LEN];
    if (!Rendezvous_DerivePunchToken(0x7f000001u, portB, 0xA1B2C3D4u, token) ||
        !Rendezvous_DerivePunchToken(0x7f000001u, portB, 0x5E6F7081u, stale_token)) {
        fprintf(stderr, "[test_late_punch] FAIL: token derivation failed\n");
        return 1;
    }
    CHECK("nonce-binds-token", memcmp(token, stale_token, sizeof(token)) != 0);

    uint8_t payload[STUN_PUNCH_PAYLOAD_LEN];
    uint8_t stale_payload[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(token, payload);
    Stun_BuildPunchPayload(stale_token, stale_payload);

    char rip[64];
    uint16_t rport = 0;
    uint8_t buf[64];
    int blen = 0;

    /* ---- A1: disarmed = inert. The phase boundary: before do_handoff
     * arms it (and after Disarm), the layer consumes NOTHING — punches
     * belong to the orchestrator's own gates then. */
    CHECK("A1", !LatePunch_HandleDatagram(payload, (int)sizeof(payload), "127.0.0.1", portB));
    CHECK("A1", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));

    /* ---- A12: Arm refuses degenerate arguments and stays disarmed. */
    LatePunch_Arm(NULL, token, "127.0.0.1", portB);
    CHECK("A12", !LatePunch_IsArmed());
    LatePunch_Arm(sockA, token, "", portB);
    CHECK("A12", !LatePunch_IsArmed());
    LatePunch_Arm(sockA, token, "127.0.0.1", 0);
    CHECK("A12", !LatePunch_IsArmed());

    /* ---- A2: valid token from the established endpoint — consumed, no
     * relearn. */
    LatePunch_Arm(sockA, token, "127.0.0.1", portB);
    CHECK("A2", LatePunch_IsArmed());
    CHECK("A2", LatePunch_HandleDatagram(payload, (int)sizeof(payload), "127.0.0.1", portB));
    CHECK("A2", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));

    /* ---- A10: replay of the valid payload — idempotent, still no
     * relearn. (Deliberate: the payload is a stateless constant; from
     * the peer's own endpoint a replay is indistinguishable from a
     * retransmit and must change nothing.) */
    for (int i = 0; i < 5; i++) {
        CHECK("A10", LatePunch_HandleDatagram(payload, (int)sizeof(payload), "127.0.0.1", portB));
    }
    CHECK("A10", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));

    /* ---- B1: keepalive cadence, clock-controlled. */
    uint32_t now = tick_until_sent(sockB, 1000, buf, (int)sizeof(buf), &blen);
    CHECK("B1", blen == STUN_PUNCH_PAYLOAD_LEN);
    CHECK("B1", blen == STUN_PUNCH_PAYLOAD_LEN && memcmp(buf, payload, sizeof(payload)) == 0);
    LatePunch_Tick(now + 10); /* inside the interval — must not send */
    CHECK("B1", recv_none(sockB));
    LatePunch_Tick(now + LATE_PUNCH_TX_INTERVAL_MS + 1);
    CHECK("B1", recv_one(sockB, buf, (int)sizeof(buf), &blen, NULL));
    now += LATE_PUNCH_TX_INTERVAL_MS + 1;

    /* ---- B2: a valid inbound punch flags a prompt answer — the next
     * Tick sends even though the cadence has not elapsed. */
    CHECK("B2", LatePunch_HandleDatagram(payload, (int)sizeof(payload), "127.0.0.1", portB));
    LatePunch_Tick(now + 10);
    CHECK("B2", recv_one(sockB, buf, (int)sizeof(buf), &blen, NULL));
    now += 10;

    /* ---- A6: wrong token — consumed (punch-shaped, never GekkoNet's),
     * NO relearn even from a same-IP new port. This is the redirect
     * oracle the layer must not have. */
    {
        uint8_t bad[STUN_PUNCH_PAYLOAD_LEN];
        memcpy(bad, payload, sizeof(bad));
        bad[STUN_PUNCH_PAYLOAD_LEN - 1] ^= 0x01;
        CHECK("A6", LatePunch_HandleDatagram(bad, (int)sizeof(bad), "127.0.0.1", portC));
        CHECK("A6", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));
    }

    /* ---- A8: stale token from the PREVIOUS hosting attempt — same
     * verdict as any wrong token. The nonce is in the derivation, so a
     * peer holding last attempt's code cannot move this session. */
    CHECK("A8", LatePunch_HandleDatagram(stale_payload, (int)sizeof(stale_payload), "127.0.0.1", portC));
    CHECK("A8", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));

    /* ---- A7: truncated / undersized / oversized shapes. */
    CHECK("A7", LatePunch_HandleDatagram(payload, STUN_PUNCH_PAYLOAD_LEN - 1, "127.0.0.1", portC));
    CHECK("A7", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));
    CHECK("A7-short", !LatePunch_HandleDatagram(payload, 4, "127.0.0.1", portC));
    {
        uint8_t big[STUN_PUNCH_PAYLOAD_LEN + 1];
        memcpy(big, payload, sizeof(payload));
        big[STUN_PUNCH_PAYLOAD_LEN] = 0x00;
        CHECK("A7-long", LatePunch_HandleDatagram(big, (int)sizeof(big), "127.0.0.1", portC));
        CHECK("A7-long", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));
    }

    /* ---- A5: valid token from a FOREIGN IP — consumed, counted, but
     * never a relearn, and the keepalive keeps targeting the established
     * peer. A recorded-payload replay from off-path cannot redirect the
     * session. */
    CHECK("A5", LatePunch_HandleDatagram(payload, (int)sizeof(payload), "203.0.113.7", 4444));
    CHECK("A5", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));
    LatePunch_Tick(now + 2 * LATE_PUNCH_TX_INTERVAL_MS);
    now += 2 * LATE_PUNCH_TX_INTERVAL_MS;
    CHECK("A5", recv_one(sockB, buf, (int)sizeof(buf), &blen, NULL)); /* still to B */
    CHECK("A5", recv_none(sockC));

    /* ---- A3/B3 (rewritten, task #133): valid token, same IP, NEW port
     * — the S2-retry shape. This used to BE the relearn: one datagram and
     * every session send followed it. It now only NOMINATES, and the move
     * costs a round trip the nominee cannot fake from the room code.
     *
     * Driven over real sockets on purpose: the challenge is observed
     * ARRIVING at the nominee (sockC), and the response is built from the
     * nonce that actually came off the wire, not from a peek. A test that
     * read the nonce out of the module could pass against a module that
     * never sent it. */
    uint8_t chal[64];
    int chal_len = -1;
    {
        CHECK("A3", LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                             "127.0.0.1", portC));
        /* Nothing has moved yet. */
        CHECK("A3-nominate-only",
              !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));

        /* The keepalive still goes to the ESTABLISHED port... */
        now = tick_until_sent(sockB, now + LATE_PUNCH_TX_INTERVAL_MS + 1, buf,
                              (int)sizeof(buf), &blen);
        CHECK("A3-keepalive-unmoved", blen == STUN_PUNCH_PAYLOAD_LEN);

        /* ...and the nominee gets a CHALLENGE instead of the payload it
         * would need to confirm its own leg. */
        for (int spin = 0; spin < 40 && chal_len < 0; spin++) {
            LatePunch_Tick(now);
            for (int wait = 0; wait < 20; wait++) {
                NET_Datagram* d = NULL;
                if (NET_ReceiveDatagram(sockC, &d) && d != NULL) {
                    int n = d->buflen < (int)sizeof(chal) ? d->buflen
                                                          : (int)sizeof(chal);
                    memcpy(chal, d->buf, (size_t)n);
                    chal_len = d->buflen;
                    NET_DestroyDatagram(d);
                    break;
                }
                SDL_Delay(5);
            }
            now += LATE_PUNCH_TX_INTERVAL_MS + 1;
        }
        CHECK("B3-challenge-sent", chal_len == STUN_PUNCH_PROBE_PAYLOAD_LEN);
    }
    if (chal_len == STUN_PUNCH_PROBE_PAYLOAD_LEN) {
        uint8_t kind = 0;
        uint8_t nonce[STUN_PUNCH_PROBE_NONCE_LEN];
        CHECK("B3-challenge-shape",
              Stun_ParseProbePayload(chal, chal_len, token, &kind, nonce));
        CHECK("B3-challenge-kind", kind == STUN_PUNCH_PROBE_CHALLENGE);
        /* It is punch-PREFIXED, so every receive path in the tree already
         * drops it before GekkoNet. */
        CHECK("B3-challenge-prefixed", Stun_HasPunchPrefix(chal, chal_len));
        CHECK("B3-challenge-not-a-punch",
              !Stun_IsPunchPayload(chal, chal_len, token));

        /* A WRONG answer buys nothing. */
        {
            uint8_t wrong[STUN_PUNCH_PROBE_PAYLOAD_LEN];
            uint8_t flipped[STUN_PUNCH_PROBE_NONCE_LEN];
            memcpy(flipped, nonce, sizeof(flipped));
            flipped[0] ^= 0x01u;
            Stun_BuildProbePayload(token, STUN_PUNCH_PROBE_RESPONSE, flipped,
                                   wrong);
            CHECK("A3-wrong-nonce",
                  LatePunch_HandleDatagram(wrong, (int)sizeof(wrong),
                                           "127.0.0.1", portC));
            CHECK("A3-wrong-nonce",
                  !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));
        }

        /* The RIGHT answer, from the port that was challenged, IS the
         * relearn — the rescue still works, which is the whole point. */
        {
            uint8_t resp[STUN_PUNCH_PROBE_PAYLOAD_LEN];
            Stun_BuildProbePayload(token, STUN_PUNCH_PROBE_RESPONSE, nonce,
                                   resp);
            CHECK("A3", LatePunch_HandleDatagram(resp, (int)sizeof(resp),
                                                 "127.0.0.1", portC));
            CHECK("A3", LatePunch_TakeRelearn(rip, sizeof(rip), &rport));
            CHECK("A3", strcmp(rip, "127.0.0.1") == 0 && rport == portC);
            CHECK("A3", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport)); /* one-shot */
        }
        now = tick_until_sent(sockC, now + LATE_PUNCH_TX_INTERVAL_MS + 1, buf,
                              (int)sizeof(buf), &blen);
        CHECK("B3", blen == STUN_PUNCH_PAYLOAD_LEN);
        CHECK("B3", recv_none(sockB)); /* nothing still leaking to the old port */
    }

    /* ---- A9 (task #133): the budget is ONE VERIFIED move, and a second
     * distinct port is refused before a challenge is even opened. Under
     * the old cap of 8 this loop is exactly the capture: eight token-valid
     * punches from eight ports left the target on the last one. */
    {
        CHECK("A9-budget-is-one", LATE_PUNCH_MAX_RELEARNS == 1);
        int refused = 0;
        for (int i = 0; i < 8; i++) {
            const uint16_t rogue = (uint16_t)(portB + 300 + i);
            CHECK("A9", rogue != portC);
            CHECK("A9", LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                                 "127.0.0.1", rogue));
            CHECK("A9-cap", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));
            refused++;
        }
        CHECK("A9", refused == 8);
        /* No challenge went out to any of them, and the send target is
         * still the endpoint that answered. */
        LatePunch_Tick(now + LATE_PUNCH_TX_INTERVAL_MS + 1);
        now += LATE_PUNCH_TX_INTERVAL_MS + 1;
        CHECK("A9", recv_one(sockC, buf, (int)sizeof(buf), &blen, NULL));
        CHECK("A9", blen == STUN_PUNCH_PAYLOAD_LEN);
        CHECK("A9", recv_none(sockB));
    }

    /* ---- A13 (task #133): a nominee that never answers EXPIRES, costs
     * nothing, and the real peer is still relearnable afterwards.
     *
     * This is the property that keeps "budget of 1" from becoming a
     * one-datagram freeze — the failure mode the old cap of 8 had, only
     * cheaper. If a nomination could spend the budget, a single spoofed
     * punch would pin the target forever. */
    {
        LatePunch_Arm(sockA, token, "127.0.0.1", portB);
        CHECK("A13-armed", LatePunch_IsArmed());
        uint32_t t = 1000u;
        /* Drain whatever the fresh arm sends. */
        (void)tick_until_sent(sockB, t, buf, (int)sizeof(buf), &blen);

        /* A rogue port nominates, is challenged, and never answers. */
        const uint16_t rogue = (uint16_t)(portB + 777);
        CHECK("A13", LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                              "127.0.0.1", rogue));
        bool active = false;
        uint16_t cport = 0;
        LatePunch_TestPeekChallenge(&active, &cport, NULL, NULL, NULL);
        CHECK("A13-challenged", active && cport == rogue);

        /* Walk the clock past the challenge window. */
        for (int i = 0; i < 4; i++) {
            t += LATE_PUNCH_CHALLENGE_MS;
            LatePunch_Tick(t);
        }
        LatePunch_TestPeekChallenge(&active, NULL, NULL, NULL, NULL);
        CHECK("A13-expired", !active);
        CHECK("A13-no-move", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));

        /* The budget survived, so the REAL peer can still be relearned. */
        CHECK("A13", LatePunch_HandleDatagram(payload, (int)sizeof(payload),
                                              "127.0.0.1", portC));
        uint8_t nonce2[STUN_PUNCH_PROBE_NONCE_LEN];
        LatePunch_TestPeekChallenge(&active, &cport, nonce2, NULL, NULL);
        CHECK("A13-renominated", active && cport == portC);
        uint8_t resp2[STUN_PUNCH_PROBE_PAYLOAD_LEN];
        Stun_BuildProbePayload(token, STUN_PUNCH_PROBE_RESPONSE, nonce2, resp2);
        CHECK("A13", LatePunch_HandleDatagram(resp2, (int)sizeof(resp2),
                                              "127.0.0.1", portC));
        CHECK("A13-relearned", LatePunch_TakeRelearn(rip, sizeof(rip), &rport));
        CHECK("A13-relearned", rport == portC);
        LatePunch_Disarm();
    }

    /* ---- A14 (task #133): we ANSWER a challenge aimed at us, over the
     * wire, to the port it came from. A peer that has also handed off runs
     * this same check against us, so a module that only CHALLENGES and
     * never ANSWERS would deadlock two rescuing peers. */
    {
        LatePunch_Arm(sockA, token, "127.0.0.1", portB);
        uint32_t t = 5000u;
        (void)tick_until_sent(sockB, t, buf, (int)sizeof(buf), &blen);
        static const uint8_t k_nonce[STUN_PUNCH_PROBE_NONCE_LEN] = {
            1, 2, 3, 4, 5, 6, 7, 8
        };
        uint8_t inbound[STUN_PUNCH_PROBE_PAYLOAD_LEN];
        Stun_BuildProbePayload(token, STUN_PUNCH_PROBE_CHALLENGE, k_nonce,
                               inbound);
        CHECK("A14", LatePunch_HandleDatagram(inbound, (int)sizeof(inbound),
                                              "127.0.0.1", portC));
        /* The answer goes to portC (where the challenge came from), NOT to
         * portB (the established target). */
        int alen = -1;
        uint8_t abuf[64];
        for (int spin = 0; spin < 40 && alen < 0; spin++) {
            t += LATE_PUNCH_TX_INTERVAL_MS + 1;
            LatePunch_Tick(t);
            for (int wait = 0; wait < 20; wait++) {
                NET_Datagram* d = NULL;
                if (NET_ReceiveDatagram(sockC, &d) && d != NULL) {
                    int n = d->buflen < (int)sizeof(abuf) ? d->buflen
                                                          : (int)sizeof(abuf);
                    memcpy(abuf, d->buf, (size_t)n);
                    alen = d->buflen;
                    NET_DestroyDatagram(d);
                    break;
                }
                SDL_Delay(5);
            }
        }
        CHECK("A14-answered", alen == STUN_PUNCH_PROBE_PAYLOAD_LEN);
        if (alen == STUN_PUNCH_PROBE_PAYLOAD_LEN) {
            uint8_t kind = 0;
            uint8_t got[STUN_PUNCH_PROBE_NONCE_LEN];
            CHECK("A14", Stun_ParseProbePayload(abuf, alen, token, &kind, got));
            CHECK("A14-kind", kind == STUN_PUNCH_PROBE_RESPONSE);
            CHECK("A14-nonce", memcmp(got, k_nonce, sizeof(got)) == 0);
        }
        /* Answering a challenge is not a relearn. */
        CHECK("A14-no-move", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));
        LatePunch_Disarm();
    }

    /* ---- A15 (task #133): the OTHER side of the round trip. In the
     * rescue the endpoint being challenged is a peer still RACING its
     * punch, i.e. a StunPunchLeg, not this module — its handoff has not
     * happened yet. If the leg does not answer, the challenge is never
     * satisfied and the rescue this whole file exists to protect stops
     * working. Proven over the wire: Offer parks the answer, Pump emits
     * it to the port the challenge came from. */
    {
        StunPunchLeg leg;
        CHECK("A15-setup",
              Stun_PunchBegin(&leg, "127.0.0.1", portB, token, 0));
        static const uint8_t k_nonce[STUN_PUNCH_PROBE_NONCE_LEN] = {
            0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18
        };
        uint8_t inbound[STUN_PUNCH_PROBE_PAYLOAD_LEN];
        Stun_BuildProbePayload(token, STUN_PUNCH_PROBE_CHALLENGE, k_nonce,
                               inbound);
        CHECK("A15-consumed",
              Stun_PunchOffer(&leg, NULL, inbound, (int)sizeof(inbound),
                              "127.0.0.1", portC, 0));
        /* A probe frame is as good as a punch for confirming the leg —
         * it carries the same prefix and the same token — so the rescue
         * does not get SLOWER because the other side now verifies. */
        CHECK("A15-confirmed", Stun_PunchConfirmed(&leg));

        /* Pump it out on sockA and read it at portC. */
        Stun_PunchPump(&leg, sockA, 0);
        int alen = -1;
        uint8_t abuf[64];
        for (int wait = 0; wait < 40 && alen < 0; wait++) {
            NET_Datagram* d = NULL;
            if (NET_ReceiveDatagram(sockC, &d) && d != NULL) {
                int n = d->buflen < (int)sizeof(abuf) ? d->buflen
                                                      : (int)sizeof(abuf);
                memcpy(abuf, d->buf, (size_t)n);
                alen = d->buflen;
                NET_DestroyDatagram(d);
                break;
            }
            SDL_Delay(5);
        }
        CHECK("A15-answered", alen == STUN_PUNCH_PROBE_PAYLOAD_LEN);
        if (alen == STUN_PUNCH_PROBE_PAYLOAD_LEN) {
            uint8_t kind = 0;
            uint8_t got[STUN_PUNCH_PROBE_NONCE_LEN];
            CHECK("A15", Stun_ParseProbePayload(abuf, alen, token, &kind, got));
            CHECK("A15-kind", kind == STUN_PUNCH_PROBE_RESPONSE);
            CHECK("A15-nonce", memcmp(got, k_nonce, sizeof(got)) == 0);
        }

        /* A RESPONSE is consumed but never echoed — no ping-pong. */
        uint8_t resp[STUN_PUNCH_PROBE_PAYLOAD_LEN];
        Stun_BuildProbePayload(token, STUN_PUNCH_PROBE_RESPONSE, k_nonce, resp);
        CHECK("A15-resp-consumed",
              Stun_PunchOffer(&leg, NULL, resp, (int)sizeof(resp), "127.0.0.1",
                              portC, 0));
        Stun_PunchPump(&leg, sockA, 0);
        /* Whatever Pump sent is the ordinary punch payload, never a probe. */
        {
            int n2 = -1;
            uint8_t b2[64];
            for (int wait = 0; wait < 20 && n2 < 0; wait++) {
                NET_Datagram* d = NULL;
                if (NET_ReceiveDatagram(sockC, &d) && d != NULL) {
                    int n = d->buflen < (int)sizeof(b2) ? d->buflen
                                                        : (int)sizeof(b2);
                    memcpy(b2, d->buf, (size_t)n);
                    n2 = d->buflen;
                    NET_DestroyDatagram(d);
                    break;
                }
                SDL_Delay(5);
            }
            CHECK("A15-no-pingpong", n2 != STUN_PUNCH_PROBE_PAYLOAD_LEN);
        }

        /* A probe frame from a DIFFERENT room's token is not answered. */
        {
            uint8_t other_token[STUN_PUNCH_TOKEN_LEN];
            memcpy(other_token, token, sizeof(other_token));
            other_token[0] ^= 0xFFu;
            uint8_t foreign[STUN_PUNCH_PROBE_PAYLOAD_LEN];
            Stun_BuildProbePayload(other_token, STUN_PUNCH_PROBE_CHALLENGE,
                                   k_nonce, foreign);
            StunResult diag;
            memset(&diag, 0, sizeof(diag));
            CHECK("A15-foreign-consumed",
                  Stun_PunchOffer(&leg, &diag, foreign, (int)sizeof(foreign),
                                  "127.0.0.1", portC, 0));
            CHECK("A15-foreign-flagged", diag.diag_punch_bad_token);
            Stun_PunchPump(&leg, sockA, 0);
            int n3 = -1;
            uint8_t b3[64];
            for (int wait = 0; wait < 20 && n3 < 0; wait++) {
                NET_Datagram* d = NULL;
                if (NET_ReceiveDatagram(sockC, &d) && d != NULL) {
                    n3 = d->buflen;
                    (void)b3;
                    NET_DestroyDatagram(d);
                    break;
                }
                SDL_Delay(5);
            }
            CHECK("A15-foreign-unanswered",
                  n3 != STUN_PUNCH_PROBE_PAYLOAD_LEN);
        }
        Stun_PunchEnd(&leg);
        (void)recv_none(sockB);
    }

    /* ---- A11: Disarm ends everything; re-Arm starts clean. */
    LatePunch_Disarm();
    CHECK("A11", !LatePunch_IsArmed());
    CHECK("A11", !LatePunch_HandleDatagram(payload, (int)sizeof(payload), "127.0.0.1", portB));
    CHECK("A11", !LatePunch_TakeRelearn(rip, sizeof(rip), &rport));
    LatePunch_Arm(sockA, token, "127.0.0.1", portB);
    CHECK("A11", LatePunch_IsArmed());
    CHECK("A11", LatePunch_HandleDatagram(payload, (int)sizeof(payload), "127.0.0.1", portB));
    LatePunch_Disarm();

    /* ---- A12 (task #131): installing a DIFFERENT STUN socket disarms.
     *
     * late_punch BORROWS its socket and never closes it (late_punch.c:26),
     * while Netplay_SetStunSocket DESTROYS the socket it replaces. Armed
     * across that call, LatePunch_Tick's next NET_SendDatagram would run
     * on freed memory.
     *
     * No production ordering reaches this today — do_handoff is the only
     * caller and it cannot run twice per session (see the comment on
     * Netplay_SetStunSocket for the guard that stops it). That is exactly
     * why this case is worth pinning: the property is currently held by
     * call-ordering discipline in ANOTHER translation unit, so nothing
     * else would notice a second call site appearing, and the symptom
     * would be a use-after-free in a thread that never named either
     * function. */
    {
        NET_DatagramSocket* held = NET_CreateDatagramSocket(lo, 0);
        NET_DatagramSocket* nextsock = NET_CreateDatagramSocket(lo, 0);
        CHECK("A12-setup", held != NULL && nextsock != NULL);
        if (held != NULL && nextsock != NULL) {
            /* Ownership of both moves into netplay.c, which frees them. */
            Netplay_SetStunSocket(held);
            LatePunch_Arm(held, token, "127.0.0.1", portB);
            CHECK("A12-armed", LatePunch_IsArmed());
            /* Destroys `held` — the borrowed pointer must not outlive it. */
            Netplay_SetStunSocket(nextsock);
            CHECK("A12-disarmed", !LatePunch_IsArmed());
            /* A tick while armed here would have been the UAF. */
            LatePunch_Tick(1000);
            Netplay_SetStunSocket(NULL); /* frees `nextsock`, clears the slot */
        }
    }

    NET_DestroyDatagramSocket(sockA);
    NET_DestroyDatagramSocket(sockB);
    NET_DestroyDatagramSocket(sockC);
    NET_UnrefAddress(lo);

    if (fail_count > 0) {
        fprintf(stderr, "[test_late_punch] %d failure(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "[test_late_punch] OK — all cases passed\n");
    return 0;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_LatePunch(void) {
    fprintf(stderr,
            "[test_late_punch] not compiled in; rebuild with "
            "-DENABLE_NETPLAY_TESTS to enable.\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
