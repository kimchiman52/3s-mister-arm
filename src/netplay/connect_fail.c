/*
 * connect_fail.c — S3 failure taxonomy (see connect_fail.h for the
 * cause -> evidence table). Pure code, no SDL/SDL_net: everything here
 * is exercised by test_bilateral_punch.c / test_stun_mock.c.
 */
#include "netplay/connect_fail.h"

#include <stddef.h>

const char* ConnectFail_Code(ConnectFailCode code) {
    switch (code) {
    case CONNECT_FAIL_NONE:                 return "P2P_OK";
    case CONNECT_FAIL_DNS_ALLDOWN:          return "P2P_FAIL_DNS_ALLDOWN";
    case CONNECT_FAIL_STUN_ALLDOWN:         return "P2P_FAIL_STUN_ALLDOWN";
    case CONNECT_FAIL_RENDEZVOUS_DOWN:      return "P2P_FAIL_RENDEZVOUS_DOWN";
    case CONNECT_FAIL_COOKIE_REJECTED:      return "P2P_FAIL_COOKIE_REJECTED";
    case CONNECT_FAIL_HOST_OFFLINE:         return "P2P_FAIL_HOST_OFFLINE";
    case CONNECT_FAIL_NAT_BLOCKED:          return "P2P_FAIL_NAT_BLOCKED";
    case CONNECT_FAIL_SYMMETRIC_BOTH:       return "P2P_FAIL_SYMMETRIC_BOTH";
    case CONNECT_FAIL_HAIRPIN:              return "P2P_FAIL_HAIRPIN";
    case CONNECT_FAIL_PUNCH_AUTH:           return "P2P_FAIL_PUNCH_AUTH";
    case CONNECT_FAIL_HOST_UNMAPPABLE:      return "P2P_FAIL_HOST_UNMAPPABLE";
    case CONNECT_FAIL_PEER_REJECTED:        return "P2P_FAIL_PEER_REJECTED";
    case CONNECT_FAIL_TIMEOUT_CONNECTING:   return "P2P_FAIL_TIMEOUT_CONNECTING";
    case CONNECT_FAIL_TIMEOUT_ORCHESTRATOR: return "P2P_FAIL_TIMEOUT_ORCHESTRATOR";
    case CONNECT_FAIL_USER_ABORT:           return "P2P_ABORT_USER";
    case CONNECT_FAIL_INVALID_CODE:         return "P2P_FAIL_INVALID_CODE";
    case CONNECT_FAIL_CODE_VERSION:         return "P2P_FAIL_CODE_VERSION";
    case CONNECT_FAIL_INTERNAL:             return "P2P_FAIL_INTERNAL";
    case CONNECT_FAIL_BALANCE_UNAVAILABLE:  return "P2P_FAIL_BALANCE_UNAVAILABLE";
    }
    return "P2P_FAIL_UNKNOWN";
}

const char* ConnectFail_UserText(ConnectFailCode code) {
    switch (code) {
    case CONNECT_FAIL_NONE:
        return "";
    case CONNECT_FAIL_DNS_ALLDOWN:
        return "No internet connection (DNS failed).";
    case CONNECT_FAIL_STUN_ALLDOWN:
        /* Review L-4: hedged — the same evidence (sends left the box,
         * zero replies) also covers "ISP/uplink down while the LAN is
         * up", which is not the router's fault. */
        return "No STUN reply (UDP blocked or net down).";
    case CONNECT_FAIL_RENDEZVOUS_DOWN:
        return "Matchmaking server unreachable.";
    case CONNECT_FAIL_COOKIE_REJECTED:
        return "Matchmaking auth failed. Update the game.";
    case CONNECT_FAIL_HOST_OFFLINE:
        return "Host not found. Code stale or host offline.";
    case CONNECT_FAIL_NAT_BLOCKED:
        return "Host found, but NAT blocked the link.";
    case CONNECT_FAIL_SYMMETRIC_BOTH:
        return "Both networks too strict (needs relay).";
    case CONNECT_FAIL_HAIRPIN:
        return "Same network as host. Router lacks loopback.";
    case CONNECT_FAIL_PUNCH_AUTH:
        /* The peer was REACHED (its datagrams arrived) but its punch
         * failed the token check — almost always a build too old to
         * send the S4a token, or a code/nonce mismatch. */
        return "Opponent failed auth. Update both builds.";
    case CONNECT_FAIL_HOST_UNMAPPABLE:
        return "Router may be blocking hosting.";
    case CONNECT_FAIL_PEER_REJECTED:
        return "Opponent rejected the connection.";
    case CONNECT_FAIL_TIMEOUT_CONNECTING:
        return "Opponent never synced. Gave up.";
    case CONNECT_FAIL_TIMEOUT_ORCHESTRATOR:
        return "Connection setup timed out.";
    case CONNECT_FAIL_USER_ABORT:
        return "Cancelled.";
    case CONNECT_FAIL_INVALID_CODE:
        return "Invalid room code.";
    case CONNECT_FAIL_CODE_VERSION:
        /* Callers pass a more specific older/newer string via
         * set_fail_msg; this is the generic fallback. */
        return "Code is from a different game version.";
    case CONNECT_FAIL_INTERNAL:
        return "Internal error. See log.";
    case CONNECT_FAIL_BALANCE_UNAVAILABLE:
        /* Callers (Netplay_RefuseArm) pass the specific
         * ArcadeBalance_GetReason() text through set_status instead;
         * this is the generic fallback. */
        return "Netplay needs the arcade ROM.";
    }
    return "Connection failed.";
}

ConnectFailCode ConnectFail_ClassifyStunDiscover(int servers_probed,
                                                 int servers_answered,
                                                 int sends_ok,
                                                 bool dns_all_failed) {
    if (servers_answered > 0) {
        return CONNECT_FAIL_NONE; /* discovery succeeded — not our failure */
    }
    if (dns_all_failed) {
        /* Every getaddrinfo failed. Even if the numeric fallbacks got
         * probed afterward and stayed silent, the DNS blackout is the
         * primary "no network" signal. */
        return CONNECT_FAIL_DNS_ALLDOWN;
    }
    if (servers_probed > 0 && sends_ok > 0) {
        /* DNS worked, datagrams left the box, nothing came back from
         * ANY server: outbound UDP (or the reply path) is filtered. */
        return CONNECT_FAIL_STUN_ALLDOWN;
    }
    /* Nothing was probed or every send failed at the socket layer —
     * the box has no usable network path at all. */
    return CONNECT_FAIL_DNS_ALLDOWN;
}

ConnectFailCode ConnectFail_ClassifyJoin(const ConnectJoinEvidence* ev) {
    if (ev == NULL) {
        return CONNECT_FAIL_INTERNAL;
    }
    if (ev->hairpin) {
        return CONNECT_FAIL_HAIRPIN;
    }
    if (!ev->deliver_any) {
        /* S4c: a CHALLENGE is proof of life — the server answered us.
         * Challenges without a single DELIVER for the whole budget
         * means our cookie echo never bound: auth/version trouble,
         * not a dead server. */
        if (ev->challenge_any) {
            return CONNECT_FAIL_COOKIE_REJECTED;
        }
        /* The v2 server answers EVERY well-formed REGISTER with a
         * CHALLENGE or a DELIVER — total silence for the whole budget
         * means the server or the path to it is down.
         * Review L-3: silent-drop exceptions exist (rate limiter,
         * per-IP key quota, per-KEY rate cap (S4c), paired-table-full,
         * third-party drop, and a VERSION-MISMATCHED client — a v1
         * client against the v2 server lands here too) and currently
         * classify here as well — see the honesty note in
         * connect_fail.h; a client-distinguishable NACK needs another
         * wire change. */
        return CONNECT_FAIL_RENDEZVOUS_DOWN;
    }
    if (!ev->deliver_real) {
        /* Server alive, but it only ever reported "peer not
         * registered": the host is gone or the code is stale. */
        return CONNECT_FAIL_HOST_OFFLINE;
    }
    if (!ev->bilateral_punched) {
        /* S4a: bad-token evidence outranks the NAT diagnoses — the
         * peer's datagrams REACHED us (connectivity fine), they just
         * failed authentication. Blaming NAT here would send the user
         * chasing router settings for a version mismatch. */
        if (ev->punch_bad_token) {
            return CONNECT_FAIL_PUNCH_AUTH;
        }
        /* We learned the host's live endpoint and still couldn't punch:
         * the NAT pair is the blocker. Our own port_disagreement (S2)
         * upgrades the diagnosis to the symmetric/relay-needed class. */
        return ev->port_disagreement ? CONNECT_FAIL_SYMMETRIC_BOTH
                                     : CONNECT_FAIL_NAT_BLOCKED;
    }
    return CONNECT_FAIL_NONE;
}

ConnectFailCode ConnectFail_ClassifyHostWaiting(bool upnp_active,
                                                bool deliver_any,
                                                bool challenge_any,
                                                uint32_t waited_ms) {
    if (waited_ms < CONNECT_HOST_ADVISORY_MS || deliver_any) {
        return CONNECT_FAIL_NONE;
    }
    /* S4c: challenges prove the server is alive; zero DELIVERs despite
     * them means our cookie echoes never bound. */
    if (challenge_any) {
        return CONNECT_FAIL_COOKIE_REJECTED;
    }
    /* Zero DELIVERs after >= 6 REGISTER cycles (5 s cadence): the
     * rendezvous path is dead. Without UPnP that leaves no reliable way
     * in (a cone-NAT direct punch may still work, hence "advisory"). */
    return upnp_active ? CONNECT_FAIL_RENDEZVOUS_DOWN
                       : CONNECT_FAIL_HOST_UNMAPPABLE;
}

bool ConnectFail_DeadlineExpired(uint64_t now_ms, uint64_t since_ms,
                                 uint32_t budget_ms) {
    if (since_ms == 0) {
        return false; /* not armed */
    }
    return (now_ms - since_ms) >= (uint64_t)budget_ms;
}

int ConnectFail_AbortHoldTick(int held_frames, bool button_down) {
    if (!button_down) {
        return 0;
    }
    if (held_frames >= CONNECT_ABORT_HOLD_FRAMES) {
        return held_frames; /* saturate — caller acts once on Fired() */
    }
    return held_frames + 1;
}

bool ConnectFail_AbortHoldFired(int held_frames) {
    return held_frames >= CONNECT_ABORT_HOLD_FRAMES;
}
