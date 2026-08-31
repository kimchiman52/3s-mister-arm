/*
 * connect_fail.c — S3 failure taxonomy (see connect_fail.h for the
 * cause -> evidence table). Pure code, no SDL/SDL_net: everything here
 * is exercised by test_bilateral_punch.c / test_stun_mock.c.
 */
#include "netplay/connect_fail.h"

/* Task #122: REND_NACK_* wire values. rendezvous.h is itself
 * dependency-free (stdbool/stdint only), so including it here does not
 * cost this TU the "pure, no SDL" property the header claims. */
#include "netplay/rendezvous.h"

#include <stddef.h>

const char* ConnectFail_Code(ConnectFailCode code) {
    switch (code) {
    case CONNECT_FAIL_NONE:                 return "P2P_OK";
    case CONNECT_FAIL_DNS_ALLDOWN:          return "P2P_FAIL_DNS_ALLDOWN";
    case CONNECT_FAIL_STUN_ALLDOWN:         return "P2P_FAIL_STUN_ALLDOWN";
    case CONNECT_FAIL_RENDEZVOUS_DOWN:      return "P2P_FAIL_RENDEZVOUS_DOWN";
    case CONNECT_FAIL_COOKIE_REJECTED:      return "P2P_FAIL_COOKIE_REJECTED";
    case CONNECT_FAIL_RENDEZVOUS_NOPAIR:    return "P2P_FAIL_RENDEZVOUS_NOPAIR";
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
    case CONNECT_FAIL_CODE_VERSION_OLDER:   return "P2P_FAIL_CODE_VERSION_OLDER";
    case CONNECT_FAIL_CODE_VERSION_NEWER:   return "P2P_FAIL_CODE_VERSION_NEWER";
    case CONNECT_FAIL_INTERNAL:             return "P2P_FAIL_INTERNAL";
    case CONNECT_FAIL_BALANCE_UNAVAILABLE:  return "P2P_FAIL_BALANCE_UNAVAILABLE";
    case CONNECT_FAIL_RENDEZVOUS_ROOM_FULL: return "P2P_FAIL_RENDEZVOUS_ROOM_FULL";
    case CONNECT_FAIL_RENDEZVOUS_TABLE_FULL:return "P2P_FAIL_RENDEZVOUS_TABLE_FULL";
    case CONNECT_FAIL_RENDEZVOUS_BUSY:      return "P2P_FAIL_RENDEZVOUS_BUSY";
    case CONNECT_FAIL_RENDEZVOUS_BADFRAME:  return "P2P_FAIL_RENDEZVOUS_BADFRAME";
    case CONNECT_FAIL_RENDEZVOUS_REFUSED:   return "P2P_FAIL_RENDEZVOUS_REFUSED";
    case CONNECT_FAIL_PEER_DISCONNECTED:    return "P2P_FAIL_PEER_DISCONNECTED";
    case CONNECT_FAIL_DESYNC_DETECTED:      return "P2P_FAIL_DESYNC_DETECTED";
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
        /* Task #105: this string is now only reached when the server
         * RE-challenged a cookie we had already echoed, which is a real
         * refusal. It still does not name "update" as the only remedy:
         * the cookie is bound to (address, port), so a NAT that reassigns
         * our source port between datagrams also makes the echo fail
         * forever, and that user has nothing to update. Both remedies are
         * named; neither is asserted. */
        return "Matchmaking refused us. Update, or host must forward.";
    case CONNECT_FAIL_RENDEZVOUS_NOPAIR:
        /* Deliberately NOT an auth message and NOT "update the game": the
         * cookie demonstrably bound. The server took our registration and
         * we were never paired inside the budget — a lost datagram or a
         * slot the server was still holding. Retrying is the true remedy
         * and it is the one the evidence supports. */
        return "Matchmaking never paired you. Try again.";
    case CONNECT_FAIL_HOST_OFFLINE:
        return "Host not found. Code stale or host offline.";
    case CONNECT_FAIL_NAT_BLOCKED:
        /* TERMINAL — there is no relay rung after this one any more.
         * The remedy names the HOST because the joiner never runs a
         * port-mapping attempt (try_portmap has one call site, inside
         * host_thread_fn), so "enable UPnP" aimed at the reader of this
         * string would be false advice. */
        return "NAT blocked. Host should forward a port.";
    case CONNECT_FAIL_SYMMETRIC_BOTH:
        /* TERMINAL, and the one fact measured about OUR OWN side:
         * StunResult.port_disagreement means our router handed out
         * different external ports to different STUN servers. The
         * actionable remedy is still the host's. */
        return "Your NAT reassigns ports. Host must forward.";
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
    case CONNECT_FAIL_CODE_VERSION_OLDER:
        return "Code is from an older game version.";
    case CONNECT_FAIL_CODE_VERSION_NEWER:
        return "Code is from a newer game version.";
    case CONNECT_FAIL_INTERNAL:
        return "Internal error. See log.";
    case CONNECT_FAIL_BALANCE_UNAVAILABLE:
        /* Callers (Netplay_RefuseArm) pass the specific
         * ArcadeBalance_GetReason() text through set_status instead;
         * this is the generic fallback. */
        return "Netplay needs the arcade ROM.";

    /* --- Task #122: the typed refusals ---------------------------------
     * Every string below is written to survive being WRONG about the
     * cause it cannot see. See ConnectFail_ClassifyNackReason for which
     * reason bytes reach each one. */
    case CONNECT_FAIL_RENDEZVOUS_ROOM_FULL:
        /* Covers BOTH SESSION_FULL populations and says nothing that is
         * false of either: someone else's two endpoints hold the code,
         * or our OWN same-IP retry ran past MAX_PORT_RECLAIMS and the
         * server stopped treating it as a reclaim. A fresh code fixes
         * both (a new session key is a new server entry), so the remedy
         * is asserted while the cause is not. Deliberately NOT "someone
         * else took your room" — that would be a guess. */
        return "Room is full. Ask host for a new code.";
    case CONNECT_FAIL_RENDEZVOUS_TABLE_FULL:
        /* A fact about the SERVER's capacity. A new room code does not
         * help here — the table has no free entry for any key — so the
         * remedy differs from ROOM_FULL and the code has to as well. */
        return "Matchmaking server is full. Try again.";
    case CONNECT_FAIL_RENDEZVOUS_BUSY:
        /* THE AMBIGUITY, STATED. Four reasons land here and not one of
         * them tells us WHO spent the budget: RATE_IP and RATE_PREGATE
         * are per-source-address and a CGNAT egress pools them across
         * strangers; KEY_QUOTA counts keys created by our address, which
         * is the same address those strangers share; RATE_KEY is per-room
         * and a busy room is not the reader's doing either. So the string
         * names the LIMITER (measured) and no party at all (not
         * measured), and the remedy it gives — wait — is the only one
         * that is true whoever spent it. */
        return "Matchmaking is busy. Wait and retry.";
    case CONNECT_FAIL_RENDEZVOUS_BADFRAME:
        /* The server refused the BYTES, not the request. Two live causes
         * with two different remedies and no way to tell them apart from
         * one frame: a build older than the server, or a middlebox that
         * truncated/mangled the datagram in flight. Both are named. */
        return "Server rejected our packet. Update or retry.";
    case CONNECT_FAIL_RENDEZVOUS_REFUSED:
        /* A reason byte this build has no name for. The one honest thing
         * to say is that the server refused us and the number is in the
         * log; inventing a cause would be exactly the H-1 error. */
        return "Matchmaking refused us. See log.";

    /* Task #144: mid-session (RUNNING-phase) failures. Deliberately
     * DISTINCT strings — a player dropped by their opponent should not
     * read the same message as one whose session desynced (queue #144's
     * whole complaint is that today they read NOTHING, not that they read
     * the same thing, but a future reader conflating the two would
     * recreate half of that complaint). */
    case CONNECT_FAIL_PEER_DISCONNECTED:
        return "Opponent disconnected.";
    case CONNECT_FAIL_DESYNC_DETECTED:
        return "Session desynced. Ending match.";
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

/* Task #122 — THE MAPPING TABLE, and the argument for every grouping.
 *
 * Nine server reasons, five verdicts. The count is the point: a verdict
 * is a sentence shown to a person, so two reasons share one only when
 * the person would do the same thing about both, and they get separate
 * ones the moment the remedy diverges.
 *
 *   reason         | verdict                  | why this grouping
 *   ---------------+--------------------------+---------------------------
 *   SESSION_FULL   | RENDEZVOUS_ROOM_FULL     | Distinct because the fix
 *                  |                          | is a NEW CODE and nothing
 *                  |                          | else. Two populations reach
 *                  |                          | it (a third party on a full
 *                  |                          | key, and our OWN retry past
 *                  |                          | MAX_PORT_RECLAIMS -- see
 *                  |                          | rendezvous-server.js
 *                  |                          | handleRegister) and a fresh
 *                  |                          | key fixes both, so one
 *                  |                          | verdict is honest for both.
 *   TABLE_FULL     | RENDEZVOUS_TABLE_FULL    | Distinct from ROOM_FULL
 *                  |                          | because a new code does NOT
 *                  |                          | help: the table has no free
 *                  |                          | entry for any key. Same
 *                  |                          | word ("full"), opposite
 *                  |                          | remedy.
 *   RATE_IP        | RENDEZVOUS_BUSY          | COLLAPSED, deliberately.
 *   RATE_KEY       | RENDEZVOUS_BUSY          | All four say a limiter
 *   RATE_PREGATE   | RENDEZVOUS_BUSY          | fired; NONE says who spent
 *   KEY_QUOTA      | RENDEZVOUS_BUSY          | the budget. RATE_IP,
 *                  |                          | RATE_PREGATE and KEY_QUOTA
 *                  |                          | are all keyed on our
 *                  |                          | SOURCE ADDRESS, which under
 *                  |                          | CGNAT is shared with
 *                  |                          | strangers, and the pre-gate
 *                  |                          | budget is additionally
 *                  |                          | spendable by anyone willing
 *                  |                          | to spoof our address (that
 *                  |                          | is precisely why the server
 *                  |                          | keeps it separate from the
 *                  |                          | cookied one). RATE_KEY is
 *                  |                          | keyed on the ROOM. Minting
 *                  |                          | four verdicts here would be
 *                  |                          | inventing four causes out
 *                  |                          | of one measurement, which
 *                  |                          | is the H-1 error. The reason
 *                  |                          | byte itself is NOT lost --
 *                  |                          | it goes to the log line via
 *                  |                          | Rendezvous_NackReasonText,
 *                  |                          | which is where triage (not
 *                  |                          | the user) reads it.
 *   BAD_VERSION    | RENDEZVOUS_BADFRAME      | COLLAPSED, and this one
 *   BAD_LENGTH     | RENDEZVOUS_BADFRAME      | needs the reachability
 *   BAD_TYPE       | RENDEZVOUS_BADFRAME      | argument. A BAD_VERSION
 *                  |                          | NACK is stamped with the
 *                  |                          | SERVER's version byte
 *                  |                          | (encodeNack in
 *                  |                          | rendezvous-server.js), and
 *                  |                          | Rendezvous_ParseNack refuses
 *                  |                          | any frame whose version is
 *                  |                          | not ours -- so the only
 *                  |                          | server whose BAD_VERSION we
 *                  |                          | can read is one that speaks
 *                  |                          | our version and still found
 *                  |                          | our version byte wrong.
 *                  |                          | That is not version skew;
 *                  |                          | it is our bytes arriving
 *                  |                          | changed, exactly like
 *                  |                          | BAD_LENGTH (truncation) and
 *                  |                          | BAD_TYPE (a mangled or
 *                  |                          | stale type byte). Real
 *                  |                          | version skew never reaches
 *                  |                          | this function at all: it is
 *                  |                          | the badver_n path in
 *                  |                          | p2p_race, which already
 *                  |                          | yields the DEFINITE
 *                  |                          | CONNECT_ATTRIB_VERSION_SKEW
 *                  |                          | and is untouched here.
 *   anything else  | RENDEZVOUS_REFUSED       | A newer server naming a
 *                  |                          | reason we have no word for.
 *                  |                          | Reported as "refused, see
 *                  |                          | log" -- never coerced onto
 *                  |                          | the nearest name we do
 *                  |                          | have.
 *
 * REND_NACK_NONE (0) lands in the default arm on purpose. It is not a
 * reason; a caller that reaches here with 0 has read an out-parameter it
 * should have gated on `nack_any`, and returning a refusal verdict is the
 * conservative answer to that (it cannot manufacture a SUCCESS).
 */
ConnectFailCode ConnectFail_ClassifyNackReason(uint8_t reason) {
    switch (reason) {
    case REND_NACK_SESSION_FULL:
        return CONNECT_FAIL_RENDEZVOUS_ROOM_FULL;
    case REND_NACK_TABLE_FULL:
        return CONNECT_FAIL_RENDEZVOUS_TABLE_FULL;
    case REND_NACK_RATE_IP:
    case REND_NACK_RATE_KEY:
    case REND_NACK_RATE_PREGATE:
    case REND_NACK_KEY_QUOTA:
        return CONNECT_FAIL_RENDEZVOUS_BUSY;
    case REND_NACK_BAD_VERSION:
    case REND_NACK_BAD_LENGTH:
    case REND_NACK_BAD_TYPE:
        return CONNECT_FAIL_RENDEZVOUS_BADFRAME;
    default:
        return CONNECT_FAIL_RENDEZVOUS_REFUSED;
    }
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
         *
         * Task #105 refines what it proves. It used to be read as "our
         * cookie echo never bound", but a single challenge proves the
         * opposite of nothing: the server issues a CHALLENGE only when
         * cookieValid() fails, and the joiner then carries that cookie on
         * EVERY later REGISTER. So one challenge is the NORMAL opening of
         * a healthy v2 session, and the honest discriminator is whether we
         * were challenged AGAIN after echoing:
         *
         *   re-challenged  -> the echo really did not verify. Auth or
         *                     version trouble (or a NAT reassigning our
         *                     source port, which the cookie is bound to).
         *   not re-challenged -> the cookie BOUND. Zero DELIVERs then
         *                     means the pairing never happened: a lost
         *                     DELIVER, or the server dropping our valid
         *                     REGISTER for a non-auth reason. Blaming
         *                     auth here sends the user to "update the
         *                     game" for a dropped datagram. */
        /* Task #122: a typed NACK OUTRANKS both of the inferences below,
         * and it outranks them for the same reason in each case — they
         * are inferences and it is a statement. A CHALLENGE proves only
         * that the server is alive; a NACK proves the server is alive AND
         * names the refusal, which is strictly more evidence about the
         * same question. Total silence proves nothing at all.
         *
         * This is also the arm that closes the reclaim case. When the
         * server rate-caps a reclaiming retry (portReclaims past
         * MAX_PORT_RECLAIMS in handleRegister) the retry falls through to
         * the third-party arm and draws SESSION_FULL — and because the S2
         * retry runs join_attempt() again on freshly cleared evidence,
         * that whole attempt sees ZERO DELIVERs and lands exactly here.
         * Before #122's client half it reported "Matchmaking server
         * unreachable" about a server that had answered every single
         * REGISTER. */
        if (ev->nack_any) {
            return ConnectFail_ClassifyNackReason(ev->nack_reason);
        }
        if (ev->challenge_any) {
            return ev->cookie_rechallenged ? CONNECT_FAIL_COOKIE_REJECTED
                                           : CONNECT_FAIL_RENDEZVOUS_NOPAIR;
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
        /* Task #122, and this arm is DELIBERATELY NARROWER than the one
         * above: only SESSION_FULL overrides HOST_OFFLINE here.
         *
         * HOST_OFFLINE asserts "the server never had this host". A
         * SESSION_FULL NACK asserts "both slots of this room are live and
         * neither is yours", which is a flat CONTRADICTION of that — the
         * server does have endpoints for the key — so the measured claim
         * displaces the inferred one.
         *
         * Nothing else does. The rate family can arrive alongside
         * sentinel DELIVERs and explains a STALL, but it does not
         * contradict "the host never registered" and it is not what the
         * user must act on; TABLE_FULL cannot coexist with a live key at
         * all (it is refused before any entry is created); and a
         * BAD_LENGTH/BAD_TYPE drawn by ONE mangled datagram in a run that
         * was otherwise talking to the server would, if it won here,
         * replace a correct HOST_OFFLINE with "update the game". That is
         * the misattribution this whole feature is under orders not to
         * commit, so the narrow rule is the one that ships. */
        if (ev->nack_any && ev->nack_reason == REND_NACK_SESSION_FULL) {
            return CONNECT_FAIL_RENDEZVOUS_ROOM_FULL;
        }
        /* Server alive, but it only ever reported "peer not
         * registered": the host is gone or the code is stale. */
        return CONNECT_FAIL_HOST_OFFLINE;
    }
    if (!ev->bilateral_punched) {
        /* Task #122: a NACK does NOT reach here, on purpose. Past this
         * point a real-endpoint DELIVER has arrived — the rendezvous
         * conversation SUCCEEDED and handed us the host — and the failure
         * that remains is the punch. A refusal the server issued about
         * some other datagram says nothing about why two NATs would not
         * open, and letting it win would send a NAT-blocked user to the
         * matchmaking server. */
        /* S4a: bad-token evidence outranks the NAT diagnoses — the
         * peer's datagrams REACHED us (connectivity fine), they just
         * failed authentication. Blaming NAT here would send the user
         * chasing router settings for a version mismatch. */
        if (ev->punch_bad_token) {
            return CONNECT_FAIL_PUNCH_AUTH;
        }
        /* We learned the host's live endpoint and still couldn't punch:
         * the NAT pair is the blocker. Our own port_disagreement (S2)
         * upgrades the diagnosis to the symmetric-both class, whose user
         * string names OUR port reassignment explicitly. Both arms are
         * TERMINAL — the relay rung that used to follow them is gone. */
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

/* --- #36: attribution confidence ---------------------------------------- */

/* RULE ORDER IS THE CONTRACT. Read top to bottom:
 *   1. success                                  -> OK
 *   2. a confirm contradicts the code           -> AMBIG_CONFIRM
 *   3. silence WITH a measured skew frame       -> VERSION_SKEW  (definite)
 *   4. silence with nothing else                -> AMBIG_VERSION (honest)
 *   5. otherwise                                -> SUPPORTED
 *
 * 2 outranks 3 because a confirm is evidence about OUR OWN socket
 * (a datagram provably arrived), while a skew frame is evidence about
 * the SERVER. If both are somehow present the socket-level fact is the
 * one that contradicts the classified code, so it wins. */
ConnectAttribution ConnectFail_Attribute(ConnectFailCode code,
                                         bool race_confirm_seen,
                                         unsigned race_badver_n) {
    if (code == CONNECT_FAIL_NONE) {
        return CONNECT_ATTRIB_OK;
    }
    /* A punch leg that reached CONFIRMED proves a datagram from the peer
     * arrived at our socket and passed the token check. That single fact
     * contradicts every code below, each for its own reason:
     *
     *   NAT_BLOCKED     — asserts the peer's datagrams could not reach
     *                     us. One provably did.
     *   SYMMETRIC_BOTH  — same assertion, plus a port-reassignment
     *                     detail that a successful confirm makes moot.
     *   HOST_OFFLINE    — classified from `deliver_any && !deliver_real`,
     *                     i.e. "the server never had this host". But the
     *                     SEED punch leg (the endpoint decoded from the
     *                     room code) can confirm without the rendezvous
     *                     server ever naming the host, and a confirm
     *                     there proves the host IS online and answering.
     *                     The user string — "Host not found. Code stale
     *                     or host offline." — is then a flat
     *                     contradiction of the wire evidence.
     *   HAIRPIN         — asserts the router cannot loop a packet back to
     *                     the LAN. A confirm on that path proves loopback
     *                     worked.
     *   PUNCH_AUTH      — asserts the peer's punch failed the token
     *                     check. A leg that CONFIRMED passed that very
     *                     check, so the bad-token record must have come
     *                     from a DIFFERENT datagram. That is mixed
     *                     evidence, not a clean auth verdict.
     *
     * DELIBERATELY NOT IN THIS SET: CONNECT_FAIL_COOKIE_REJECTED. Do not
     * "fix" that omission. The cookie is the RENDEZVOUS SERVER's
     * return-routability challenge — the server refused to bind our
     * REGISTER because our cookie echo did not verify. That is a
     * client<->server fact. A punch confirm is a client<->PEER fact on a
     * different conversation with a different counterparty, and the two
     * can be simultaneously true with no contradiction whatsoever: the
     * peer can answer us perfectly while the server still rejects our
     * cookie. Marking it ambiguous would be inventing doubt about a
     * verdict the evidence fully supports, which is the same class of
     * error as inventing certainty.
     *
     * ALSO DELIBERATELY NOT IN THIS SET, and for the identical reason:
     * the five task-#122 CONNECT_FAIL_RENDEZVOUS_* refusal codes. Each of
     * them is the SERVER's own statement about our conversation with the
     * SERVER. A punch confirm is a fact about our conversation with the
     * PEER. The peer can answer us perfectly while the rendezvous server
     * refuses us for a full room or a spent budget, so there is no
     * contradiction to mark. They fall through to SUPPORTED below, which
     * is the truth: the server told us the reason. */
    if (race_confirm_seen &&
        (code == CONNECT_FAIL_NAT_BLOCKED ||
         code == CONNECT_FAIL_SYMMETRIC_BOTH ||
         code == CONNECT_FAIL_HOST_OFFLINE ||
         code == CONNECT_FAIL_HAIRPIN ||
         code == CONNECT_FAIL_PUNCH_AUTH)) {
        return CONNECT_ATTRIB_AMBIG_CONFIRM;
    }
    if (code == CONNECT_FAIL_RENDEZVOUS_DOWN) {
        /* SILENCE WITH A WITNESS. RENDEZVOUS_DOWN is inferred from the
         * absence of parseable frames — but p2p_race also counts the
         * '3SXR' frames that arrived FROM THE RENDEZVOUS ENDPOINT and
         * carried a protocol version this build cannot parse. When that
         * counter is non-zero the silence is not unexplained: the server
         * answered, we could not read the answer. That is a DEFINITE
         * verdict with a definite remedy (update the game, or the
         * server), and reporting it as "ambiguous" would bury the one
         * case this counter exists to attribute. */
        if (race_badver_n > 0u) {
            return CONNECT_ATTRIB_VERSION_SKEW;
        }
        /* TRUE silence: zero frames of any kind. Now the ambiguity is
         * real and must be stated. The causes are bit-for-bit identical
         * on the wire: the server is unreachable, or the server speaks a
         * protocol version we do not and DROPS our frame with no reply
         * whatsoever (the deployed v1-vs-v2 case — that direction is
         * undetectable precisely because it produces no frame for
         * badver_n to count). The header documents four more silent-drop
         * causes on a LIVE server, which this same ambiguity covers. */
        return CONNECT_ATTRIB_AMBIG_VERSION;
    }
    return CONNECT_ATTRIB_SUPPORTED;
}

const char* ConnectFail_AttributionText(ConnectAttribution a) {
    switch (a) {
    case CONNECT_ATTRIB_OK:            return "ok";
    case CONNECT_ATTRIB_SUPPORTED:     return "supported";
    case CONNECT_ATTRIB_AMBIG_CONFIRM: return "ambiguous-punch-confirmed";
    case CONNECT_ATTRIB_AMBIG_VERSION: return "ambiguous-rendezvous-silent";
    case CONNECT_ATTRIB_VERSION_SKEW:  return "version-skew";
    }
    return "supported";
}

const char* ConnectFail_AttributionNote(ConnectAttribution a) {
    switch (a) {
    case CONNECT_ATTRIB_AMBIG_CONFIRM:
        return "a punch leg was CONFIRMED during the race but the race still ended "
               "EXHAUSTED — do NOT read this as a NAT failure; the peer's datagram "
               "provably reached us";
    case CONNECT_ATTRIB_AMBIG_VERSION:
        return "zero rendezvous frames: an unreachable server and a server speaking a "
               "different protocol version are indistinguishable on the wire (a "
               "version-mismatched server drops our frame with no reply)";
    case CONNECT_ATTRIB_VERSION_SKEW:
        /* Deliberately contains no hedging word. The frames were counted
         * from the rendezvous endpoint itself; there is nothing to hedge
         * about. */
        return "the rendezvous server answered with a '3SXR' protocol version this "
               "build does not speak — this is a build/server version mismatch, not a "
               "network failure; update the game (or the server)";
    case CONNECT_ATTRIB_OK:
    case CONNECT_ATTRIB_SUPPORTED:
        break;
    }
    return "";
}
