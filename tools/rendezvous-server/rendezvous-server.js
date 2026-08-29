#!/usr/bin/env node
// 3SX rendezvous server: single-purpose UDP endpoint exchange.
// See ../../docs/plan-bilateral-hole-punch.md Decision 2 for the wire spec.

'use strict';

const dgram = require('dgram');
const crypto = require('crypto');
const { performance } = require('perf_hooks');

// --- Wire constants ----------------------------------------------------------

const MAGIC = 0x33535852; // '3SXR' big-endian
// S4c protocol v2 (docs/plan-netplay-connection.md §6): REGISTER/POLL
// carry an 8-byte return-routability cookie tail; the server answers an
// uncookied (or stale-cookied) request with a CHALLENGE and binds NO
// state until the client echoes the cookie back — proving it actually
// receives at its claimed source address. v1 clients are cleanly
// dropped (logged once per warn cycle); the whole alpha group ships the
// v2 client together, so there is no mixed-version window to support.
const VERSION = 2;
const TYPE_REGISTER = 1;
const TYPE_DELIVER = 2;
const TYPE_POLL = 3;
const TYPE_CHALLENGE = 4;
// Types 5..8 were the S5 relay extension (RELAY_REQ / RELAY_GRANT /
// RELAY_PIN / RELAY_PIN_ACK). The relay was deleted — client and server —
// so nothing here claims them any more and they fall through onMessage's
// `drop: unknown type` branch like any other unallocated type: logged
// under the shared throttle, no reply, no state. The version byte is
// UNCHANGED at 2, because the frames that DO exist (REGISTER, POLL,
// DELIVER, CHALLENGE) are byte-for-byte what v2 always was.

const REGISTER_LEN = 36;
const POLL_LEN = 36;
const DELIVER_LEN = 32;
const CHALLENGE_LEN = 32;
const COOKIE_LEN = 8;

// --- Tunables ----------------------------------------------------------------

// S1 host liveness (docs/plan-netplay-connection.md): 10 minutes, up from
// 60 s. A host now re-REGISTERs every ~5 s for as long as it displays a
// room code, so lastTouch stays fresh regardless — but the TTL must also
// cover a host whose re-REGISTER packets are lost or whose client predates
// S1, and it bounds how long a code shared over chat/voice stays pair-able.
// Memory exposure at this TTL is bounded by MAX_SESSIONS below.
const SESSION_TTL_MS = 10 * 60 * 1000;
const SESSION_SWEEP_INTERVAL_MS = 5 * 1000;

// Hard cap on concurrently-tracked sessions. Each entry is two endpoint
// objects + timestamps + a 32-char hex key; V8 overhead (hidden classes,
// Map buckets, string headers) puts a realistic worst case at ~1.3-2 MB,
// not the naive ~250 B/entry ~1 MB. Before S1 the 60 s TTL implicitly
// bounded slot-squatting; at 10 minutes the table needs an explicit
// bound.
//
// Cap POLICY (review H2): a plain "drop new keys when full" cap is itself
// a lockout vector — filling 4096 fresh keys takes ~410 s inside the
// 10 pkt/s per-IP limit, and source-spoofed REGISTERs bypass the per-IP
// limiter entirely. Two complementary defenses:
//   1. MAX_NEW_KEYS_PER_IP: one (non-spoofing) source IP may have at most
//      this many live keys it CREATED. A legitimate client holds exactly
//      one while hosting (briefly two across a drift re-key), so 4 is
//      generous headroom; a single-IP squatter now parks 4 keys, not 4096.
//   2. At cap, a new key EVICTS the oldest UNPAIRED singleton instead of
//      being dropped: paired sessions are never evicted, and a live host
//      re-REGISTERs every <= 5 s (refreshing lastTouch) so it is never the
//      oldest while a flood is cycling the table (flood would need to
//      turn the whole 4096-slot table over in < 5 s, i.e. > 800 spoofed
//      keys/s, to age a live host to the front). A legitimate new host
//      therefore always gets a slot, even during a spoofed-source flood.
// A stateless address-validation cookie (return-routability) is the
// third defense and SHIPPED in S4c (see COOKIE_ROTATE_MS below): a
// source-spoofed REGISTER can no longer reach handleRegister at all, so
// the "spoofed flood" arm of the reasoning above is now a defense in
// depth rather than the front line. Defenses 1 and 2 still matter — a
// real botnet whose nodes DO receive at their own addresses passes the
// cookie gate and can still flood the table.
//
// Review HIGH-2 correction: an uncookied request is NOT free of every
// server resource, and the comment that used to claim it consumed "no
// rate budget" was wrong in the direction that mattered. It draws on the
// separate PREGATE budget below (which bounds CHALLENGE emission and
// nothing else) and, until that budget is capped, on one bounded Map
// entry. What it still cannot do is create, refresh, evict, or NAME a
// session, consume the per-IP creator quota, or touch the COOKIED per-IP
// budget that a legitimate client depends on.
const MAX_SESSIONS = 4096;
const MAX_NEW_KEYS_PER_IP = 4;

// Per-slot liveness threshold (review H1). A live S1 host re-REGISTERs
// every <= 5 s and a live joiner every 500 ms, so a slot silent for 30 s
// (6+ missed host refreshes) is stale: its endpoint is a leftover from a
// cancelled/abandoned attempt. A stale, UNPAIRED host slot may be
// reclaimed by a REGISTER from the SAME source IP (the cancel-then-re-host
// flow, where the NAT picked a new source port toward us but the UPnP-
// pinned session key is unchanged); without this, the server would file
// the re-hosting client as "the joiner" and DELIVER the client its own
// stale endpoint — poisoning the room for the whole TTL. Pre-S1 clients
// (silent after their 8 s REGISTER budget) keep their slot pair-able by
// DIFFERENT-IP joiners exactly as before; only same-IP re-registration
// (which no legitimate joiner can produce — the client-side hairpin
// bypass fails same-IP joiners before they ever REGISTER) repoints it.
const SLOT_STALE_MS = 30 * 1000;

// Task #105 — the JOINER's automatic retry must not be locked out.
//
// THE BUG THIS CLOSES. The joiner runs JOIN_MAX_ATTEMPTS = 2 attempts
// (src/netplay/direct_p2p.c), and each attempt re-runs STUN discovery on a
// FRESHLY BOUND local socket, deliberately: the retry exists to dodge stale
// per-port NAT/conntrack state, so binding the same port again would defeat
// its whole purpose. A fresh local port means a fresh PUBLIC port, so
// attempt 2's REGISTER matches neither slot A (the host) nor slot B (the
// joiner's own attempt-1 endpoint). Slot B is not STALE either — attempt 1
// refreshed lastSeenB only moments earlier at its ~500 ms REGISTER cadence.
// So the whole retry fell through to the third-party drop below and was
// ignored, with no reply, for the entire connect budget. SLOT_STALE_MS is
// 30 s; the joiner's whole derived deadline is 31.8 s. The recovery
// mechanism was therefore inert in exactly the lossy conditions it was
// built for.
//
// THE FIX, AND WHY IT IS NOT A SLOT HIJACK. Reclaim slot B for a REGISTER
// from the SAME PUBLIC ADDRESS on a new port. This is the exact
// discriminator already used for the HOST slot in the H1 reclaim above --
// same IP, new NAT source port, same session key -- applied to the joiner
// slot, and it deliberately does NOT relax staleness for anyone else:
//   * A DIFFERENT-IP third party still cannot touch a live slot B. That is
//     the property __test_protocol.js testStaleJoinerSlotReplaced asserts
//     ("LIVE slot B not replaced by third party"), and it still holds.
//   * The claimant must still pass the S4 return-routability gate at its
//     NEW (address, port). cookieForSlot() is unchanged and still does NOT
//     take the session key, so this grants NO new power to a spoofed
//     source: a spoofer never receives the CHALLENGE and so never obtains
//     a valid cookie for the endpoint it is claiming. What S4 closed --
//     unproven sources binding, evicting or naming a session -- stays
//     closed.
// The residual exposure is a party that (a) knows the room code and (b) is
// behind the SAME public IP as the joiner (CGNAT). Such a party can already
// deny the room today by simply REGISTERing into slot B first: the room
// code is a capability, and this change does not alter that. The cap below
// bounds the churn rather than carrying a security property.
//
// THE CAP. The client needs exactly ONE reclaim per join (attempt 2 of 2),
// so a small multiple covers repeated user-initiated joins against the same
// still-live room code within SESSION_TTL_MS without letting a co-located
// peer flap the slot indefinitely. Once the budget is spent the slot falls
// back to the pre-existing staleness rule.
const MAX_JOINER_PORT_RECLAIMS = 8;
const RATE_SWEEP_INTERVAL_MS = 60 * 1000;

// Per-IP budget for requests that have ALREADY PROVEN return routability
// (valid cookie). Review HIGH-2: this bucket used to be the very first
// statement in onMessage, keyed on rinfo.address alone and applied before
// the length/magic/version checks and before the cookie gate. That made it
// a lockout weapon rather than a defense — 10 spoofed 36-byte REGISTERs
// per second carrying a victim's source address (~2.9 kbit/s; the room
// code still reveals the address) exhausted the victim's own budget, so
// the victim's correctly-cookied REGISTER got zero replies, bound nothing,
// and the user saw RENDEZVOUS_DOWN indefinitely. It now runs AFTER the
// cookie check, on a bucket that only cookie-holding sources can reach, so
// no amount of spoofed traffic can starve a real client.
const RATE_WINDOW_MS = 1000;
const RATE_LIMIT_PER_WINDOW = 10;

// Per-IP budget for UNCOOKIED / stale-cookied first contact. Deliberately
// separate from (and much larger than) the cookied budget above so the two
// classes of traffic cannot starve each other.
//
// What it actually bounds: CHALLENGE emission. The CHALLENGE is 32 bytes
// answering a 36-byte request — amplification factor 0.89, i.e. the server
// is a net ATTENUATOR and is never worth aiming at a victim. So this is
// not an anti-reflection control (there is nothing to reflect); it bounds
// egress toward one address and the work done per source.
//
// Why 100/s:
//   * At the cap, egress toward a spoofed victim is 100 x 32 B = 3.2 kB/s
//     (25.6 kbit/s) and it costs the attacker 100 x 36 B = 3.6 kB/s to
//     elicit — the attacker spends more than the victim receives, every
//     second, which is the whole point of keeping the reply smaller than
//     the request.
//   * It is 10x the cookied budget, so an attacker trying to starve FIRST
//     CONTACT for a shared-NAT site must now spend 10x what it used to,
//     while cookied traffic is structurally immune (separate bucket).
//   * Legitimate headroom is enormous: a client needs one challenge round
//     at startup and one per cookie rotation (>= 60 s), so 100/s per
//     PUBLIC IP covers on the order of 6000 clients behind a single NAT —
//     orders of magnitude beyond this deployment.
//   * Worst-case bucket memory stays small: <= 100 timestamps x 8 B plus
//     array/object overhead, ~900 B for a fully-saturated entry.
// Residual, stated plainly: ANY per-source pre-gate budget is spoof-
// starvable for FIRST CONTACT, because first contact is by definition
// unauthenticated. Choosing the number trades egress against how hard that
// is. What is no longer possible is starving an ESTABLISHED, cookie-
// holding client, which is the lockout the review found.
const PREGATE_WINDOW_MS = 1000;
const PREGATE_LIMIT_PER_WINDOW = 100;

// Review MEDIUM-3: hard cap on every rate-bucket Map. These are keyed on
// attacker-supplied values (source IP, or a session key read out of the
// frame), so without a bound they are an unbounded allocator: the reviewer
// measured 1-byte junk from 5000 spoofed sources producing 5000 rateMap
// entries retained >= 60 s, which at 100k pps is ~6M live entries/minute
// and V8 heap exhaustion. Two mechanisms together (see bucketAllow):
//   1. NOTHING is allocated until the frame passes magic + version +
//     length, so pure junk — the 1-byte probe above — now costs zero
//     entries no matter how many sources send it.
//   2. What survives that filter is capped here with O(1) LRU eviction.
// Sizing: 2x MAX_SESSIONS, i.e. at most twice as many distinct sources as
// the server will ever track sessions for. Worst case is dominated by the
// pre-gate map at ~900 B/saturated entry => ~7 MB, with the two 10/s maps
// at ~200 B/entry => ~1.6 MB each; ~10 MB total, the same order as the
// 1.3-2 MB MAX_SESSIONS budget reasoned about above.
const MAX_RATE_ENTRIES = 2 * MAX_SESSIONS;

// S4c: per-SESSION-KEY rate cap, alongside the per-IP one. The per-IP
// bucket is bypassable by an attacker with many (real, cookie-capable)
// source IPs all hammering ONE key; this bounds the damage to any
// single session. Enforced AFTER cookie validation so spoofed traffic
// (which never binds anyway) cannot consume a victim key's budget.
//
// ---- HISTORY (read this before "cleaning up" the number) ----------------
// 10/s originally (S4c). Review MEDIUM-4 raised it to 40/s for ONE reason:
// the S5 relay's RELAY_REQ rode this same gate (byte-identical to REGISTER
// on purpose) at RELAY_REQ_RESEND_MS = 300 per side, i.e. 6.67/s on one key
// before anything else happened. The relay is now DELETED, client and
// server, so that 6.67/s no longer exists and nothing justifies 40. The
// relay-removal commit then swung it back to 10 — which review HIGH-3
// showed is a REGRESSION in both directions at once:
//   * it made the per-key cap EQUAL to RATE_LIMIT_PER_WINDOW, so a single
//     cookied IP spending its own per-IP budget consumes 100% of any
//     room's budget (25% back when this was 40). cookieForSlot() below
//     does NOT mix the session key in, so a cookie earned on your own key
//     validates against every key — the attacker needs no cooperation from
//     the room it is silencing, and binds no state doing it.
//   * it is below the traffic a legitimate MULTI-DIALER room generates
//     (derivation below), so the room DoSes itself with no attacker
//     present: the host's liveness REGISTERs get dropped, lastSeenA stops
//     refreshing, and SLOT_STALE_MS / SESSION_TTL_MS reclaim a room whose
//     code is still on the host's screen.
//
// ---- DERIVATION (all figures per KEY_RATE_WINDOW_MS = 1000 ms, so
//      "per window" and "per second" are the same number here) ------------
// What charges this bucket, from the shipped client:
//   * joiner REGISTER resend inside the punch race — 500 ms
//     (src/netplay/direct_p2p.c:1356-1357, `(now - signal_last_send) >=
//     500u`) => 2/s PER JOINER, for signal_budget_ms (8 s default,
//     direct_p2p.c:2832-2833 / src/port/config/config.c:105).
//   * host re-REGISTER worker — CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_
//     INTERVAL_MS, default 5000 ms (config.c:111), floor 1000 ms
//     (direct_p2p.c:2272-2274) => 1/s worst case. This leg must NEVER be
//     starved: losing it is precisely what reclaims a live room. The host
//     runs NO signalling leg inside the race (direct_p2p.c:2374 sets
//     cfg.signal_leg = false for the host — the DELIVER that started that
//     thread already proves it is paired).
//   * one challenge-triggered immediate resend per side per cookie
//     rotation (COOKIE_ROTATE_MS >= 60 s), since the client answers a
//     CHALLENGE at once (direct_p2p.c:1391-1394).
//
// N, the number of simultaneous dialers on ONE key. The session key is
// derived from the HOST's public endpoint (direct_p2p.c:2809-2811), i.e.
// the key IS the room code — everyone who pastes that code lands in the
// same bucket, and each one starts its 2/s leg immediately, without
// waiting to be accepted (cfg.signal_leg at direct_p2p.c:2851 keys only on
// "have signal URL + have session key", not on pairing). The server's
// two-slot policy silences dialers 2..N at DISPATCH, but they have already
// charged this bucket — the gate runs upstream of dispatch. Codes are
// pasted into a group chat and stay live for SESSION_TTL_MS = 10 min, so
// several people racing for the single free slot is the normal case, not
// an attack. N = 6 is the design point: more than any 2-player room can
// consume, enough to cover a code dropped into a small active channel plus
// the 8 s tails of losing dialers overlapping the next wave, and past it
// the binding constraint stops being this limiter and becomes the two-slot
// policy itself.
//
//   legit peak  = host 1 + 2 x N = 1 + 2 x 6            = 13/s
//   per-IP cap  = RATE_LIMIT_PER_WINDOW                 = 10/s
//
// Constraint 1 (the ratio question, HIGH-3). The per-key cap MUST be
// strictly greater than the per-IP cap. At equality the per-key limiter
// adds ZERO attacker cost over the per-IP limiter — that IP is already
// admitted at 10/s — while handing that one IP the power to drop every
// other frame on the key, host liveness included. It stops being a defense
// and becomes a lockout weapon, which is the same shape as the HIGH-2 bug
// fixed above. So require per-key >= k x per-IP for an integer k >= 2.
// Constraint 2 (absorption). One saturating cookied IP must not be able to
// break a legitimate full room:
//   per-key - per-IP >= legit peak  =>  per-key >= 13 + 10 = 23
// The smallest integer k satisfying that is k = 3 (k = 2 gives 20, leaving
// only 10/s for a room that needs 13):
const KEY_RATE_WINDOW_MS = 1000;
const KEY_RATE_LIMIT_PER_WINDOW = 30;
// Resulting margins, stated honestly:
//   * ratio to the per-IP cap: 3:1 — no single cookied IP can take more
//     than 1/3 of a room's budget.
//   * headroom over the legit peak with no attacker: 30/13 = 2.3x.
//   * headroom with one cookied IP saturating against the room: 20/13 =
//     1.5x, so a full N = 6 room still pairs while under attack.
//   * NOT covered with room to spare: the compound worst case where all
//     seven sides rotate their cookie inside the SAME second (+7 => 20/s)
//     AND a hostile cookied IP is saturating (+10). That lands at exactly
//     30 of 30. Without the attacker it is 20 of 30.
// Egress cost of the raise is nil: at the ceiling one key emits
// 30 x 32 B = 960 B/s, and the real anti-squatting bounds remain the slot
// policy and MAX_NEW_KEYS_PER_IP, not this bucket.
// Guarded by testKeyBudgetCoversMultiJoinerRoom in __test_protocol.js,
// which drives N joiners on one key and asserts the host's liveness
// REGISTER still lands — it goes red at 10 and at 40.

// S4c: return-routability cookie. Stateless server side:
//   cookie = SHA-256(secret || addr:port:slot)[0..7]
// with slot = floor(now_wallclock / COOKIE_ROTATE_MS). The current and
// previous slots validate, so a cookie is usable for 60..120 s; a
// client whose cookie expires simply gets re-CHALLENGEd on its next
// REGISTER (the C client answers within one RTT). The secret is drawn
// fresh at process start — a restart invalidates outstanding cookies,
// which costs each live client exactly one extra challenge round.
//
// The cookie is a RETURN-ROUTABILITY proof, not a nonce: it is
// deliberately replayable by whoever holds it, for as long as it lives.
// What that buys and what it does not:
//   * It is bound to (source address, source port), so a cookie is
//     useless from any other endpoint — the only way to obtain one for
//     endpoint E is to RECEIVE a datagram at E. A source-spoofing
//     attacker gets the CHALLENGE delivered to the victim it is
//     impersonating and learns nothing, so it can never bind a slot,
//     occupy a key, or steer a victim's punch traffic.
//   * An OFF-PATH attacker cannot forge one: the secret is 32 random
//     bytes that never leave the process, and the cookie is a SHA-256
//     truncation over it.
//   * An ON-PATH attacker who observes a cookie can replay it — but an
//     on-path attacker at endpoint E already satisfies return
//     routability for E by definition, so there is nothing left to
//     prove. Cookies are NOT an authentication mechanism; peer
//     authentication is S4a's punch token and the S4b nonce-derived
//     session key.
//   * Rotation bounds a leaked cookie's usefulness to <= 120 s.
// The CHALLENGE reply is 32 bytes for a 36-byte request: amplification
// factor 0.89, i.e. the server is a net ATTENUATOR, never a reflector
// worth aiming at a victim. It is emitted under the dedicated PRE-GATE
// bucket (PREGATE_LIMIT_PER_WINDOW, 100/s/IP), so a spoofed flood at one
// victim is capped there — and NOT under the cookied per-IP bucket, which
// spoofed traffic must not be able to reach at all (review HIGH-2).
const COOKIE_ROTATE_MS = 60 * 1000;
const cookieSecret = crypto.randomBytes(32);

// --- Logging -----------------------------------------------------------------

function ts() {
    return new Date().toISOString();
}

function logInfo(msg) {
    console.log(`[${ts()}] INFO ${msg}`);
}

function logWarn(msg) {
    console.warn(`[${ts()}] WARN ${msg}`);
}

// --- Helpers -----------------------------------------------------------------

function nowMs() {
    return performance.now();
}

function endpointEq(a, b) {
    if (!a || !b) return false;
    return a.address === b.address && a.port === b.port;
}

// Spec says "truncate session_key to first 4 hex chars". 4 hex chars = 2 bytes.
function shortKey4(hexKey) {
    return hexKey.slice(0, 4);
}

function ipv4ToBytes(addr) {
    // addr is dotted-quad because socket is bound 'udp4'. inet_pton equivalent.
    const parts = addr.split('.');
    if (parts.length !== 4) {
        throw new Error(`invalid ipv4 address: ${addr}`);
    }
    const out = Buffer.alloc(4);
    for (let i = 0; i < 4; i++) {
        const n = Number(parts[i]);
        if (!Number.isInteger(n) || n < 0 || n > 255) {
            throw new Error(`invalid ipv4 octet in ${addr}`);
        }
        out[i] = n;
    }
    return out;
}

// --- Packet encode/decode ----------------------------------------------------

// S4c cookie derivation + validation ------------------------------------------

function cookieForSlot(address, port, slot) {
    return crypto
        .createHash('sha256')
        .update(cookieSecret)
        .update(`${address}:${port}:${slot}`)
        .digest()
        .subarray(0, COOKIE_LEN);
}

function currentCookieSlot() {
    return Math.floor(Date.now() / COOKIE_ROTATE_MS);
}

// Constant-time check against the current and previous rotation slots.
function cookieValid(cookieBuf, rinfo) {
    if (!cookieBuf || cookieBuf.length !== COOKIE_LEN) return false;
    const slot = currentCookieSlot();
    for (const s of [slot, slot - 1]) {
        const expect = cookieForSlot(rinfo.address, rinfo.port, s);
        if (crypto.timingSafeEqual(cookieBuf, expect)) return true;
    }
    return false;
}

function encodeChallenge(sessionKeyBuf, rinfo) {
    const buf = Buffer.alloc(CHALLENGE_LEN);
    buf.writeUInt32BE(MAGIC, 0);
    buf.writeUInt8(VERSION, 4);
    buf.writeUInt8(TYPE_CHALLENGE, 5);
    buf.writeUInt16BE(0, 6); // reserved
    sessionKeyBuf.copy(buf, 8, 0, 16);
    cookieForSlot(rinfo.address, rinfo.port, currentCookieSlot()).copy(buf, 24);
    return buf;
}

function encodeDeliver(sessionKeyBuf, peerEndpoint) {
    const buf = Buffer.alloc(DELIVER_LEN);
    buf.writeUInt32BE(MAGIC, 0);
    buf.writeUInt8(VERSION, 4);
    buf.writeUInt8(TYPE_DELIVER, 5);
    buf.writeUInt16BE(0, 6); // reserved
    sessionKeyBuf.copy(buf, 8, 0, 16);
    if (peerEndpoint) {
        ipv4ToBytes(peerEndpoint.address).copy(buf, 24);
        buf.writeUInt16BE(peerEndpoint.port & 0xffff, 28);
    } else {
        // peer absent: 4 zero bytes + 0 port already from Buffer.alloc.
    }
    buf.writeUInt16BE(0, 30); // reserved2
    return buf;
}

// --- State -------------------------------------------------------------------

const sessionMap = new Map();
// key: hex-encoded 16-byte session_key
// value: { endpointA, endpointB, lastTouch, lastSeenA, lastSeenB, creatorIp }
// lastSeenA/B track per-slot liveness (last REGISTER/POLL from that exact
// endpoint) for the SLOT_STALE_MS reclaim logic; lastTouch remains the
// whole-entry TTL clock. creatorIp is the source IP that created the key
// (slot A's original registrant) for the MAX_NEW_KEYS_PER_IP quota.

const creatorCounts = new Map();
// key: source IP string; value: number of LIVE sessionMap keys created by
// that IP. Incremented on new-key admit, decremented via releaseSession.

// Single deletion path so the creator quota stays consistent with the
// session table (sweep, cap eviction, and test resets all route here).
function releaseSession(hexKey, entry) {
    sessionMap.delete(hexKey);
    if (entry && entry.creatorIp) {
        const n = creatorCounts.get(entry.creatorIp) || 0;
        if (n <= 1) creatorCounts.delete(entry.creatorIp);
        else creatorCounts.set(entry.creatorIp, n - 1);
    }
}

const rateMap = new Map();
// key: source IP string
// value: { timestamps: number[], lastSeen: number }
// `timestamps` holds send times within the current sliding window
// (anything older than RATE_WINDOW_MS is filtered out on each access).
// Reached ONLY by requests that already passed the cookie gate (HIGH-2).

const warnedIps = new Set();
// IPs we've already warned about. Cleared when the rateMap entry is evicted
// (i.e. the IP has been quiet long enough to be swept, or was LRU-evicted).

const preGateMap = new Map();
// Review HIGH-2: same shape, but for UNCOOKIED first contact. Kept
// strictly separate from rateMap so spoofed/uncookied traffic claiming a
// victim's source address cannot consume the budget the victim's own
// cookied traffic depends on.

const warnedPreGate = new Set();

const keyRateMap = new Map();
// S4c: key = hex session key; value = { timestamps: number[], lastSeen }.
// Same sliding-window shape as rateMap, enforced post-cookie.

const warnedKeys = new Set();

// --- Rate limiter ------------------------------------------------------------

// Sliding-window token bucket over a SIZE-CAPPED Map (review MEDIUM-3).
//
// Insertion order in a JS Map IS recency order as long as every hit
// re-inserts its key, so `map.keys().next()` is the least-recently-used
// entry and eviction is O(1). That matters: an O(n) "scan for the oldest
// lastSeen" would turn a flood of distinct sources into quadratic work,
// i.e. a second DoS bolted onto the fix for the first.
//
// Evicting a rate bucket FAILS OPEN — the evicted source's budget resets.
// That is the correct direction here: these buckets are a courtesy
// throttle, while the return-routability cookie is the actual
// authorisation gate. A flood can therefore make the throttle less
// effective, but it can never use eviction to DENY a legitimate client.
function bucketAllow(map, warned, key, windowMs, limit) {
    const now = nowMs();
    let entry = map.get(key);
    if (entry !== undefined) {
        map.delete(key); // re-inserted below => moves to the recent end
    } else {
        while (map.size >= MAX_RATE_ENTRIES) {
            const victim = map.keys().next();
            if (victim.done) break;
            map.delete(victim.value);
            if (warned) warned.delete(victim.value);
        }
        entry = { timestamps: [], lastSeen: now };
    }
    map.set(key, entry);
    // Drop timestamps outside the current sliding window.
    const cutoff = now - windowMs;
    if (entry.timestamps.length > 0 && entry.timestamps[0] <= cutoff) {
        entry.timestamps = entry.timestamps.filter((t) => t > cutoff);
    }
    entry.lastSeen = now;
    if (entry.timestamps.length >= limit) return false;
    entry.timestamps.push(now);
    return true;
}

// Post-cookie per-IP budget.
function rateLimitAllow(ip) {
    if (bucketAllow(rateMap, warnedIps, ip, RATE_WINDOW_MS, RATE_LIMIT_PER_WINDOW)) {
        return true;
    }
    if (!warnedIps.has(ip)) {
        warnedIps.add(ip);
        logWarn(`rate-limit: dropping COOKIED packets from ${ip} (further drops will be silent until this IP is quiet long enough to evict)`);
    }
    return false;
}

// Pre-cookie per-IP budget: bounds CHALLENGE emission only (review HIGH-2).
function preGateAllow(ip) {
    if (bucketAllow(preGateMap, warnedPreGate, ip, PREGATE_WINDOW_MS, PREGATE_LIMIT_PER_WINDOW)) {
        return true;
    }
    if (!warnedPreGate.has(ip)) {
        warnedPreGate.add(ip);
        logWarn(`pre-gate: dropping UNCOOKIED packets from ${ip} (challenge budget exhausted; cookied traffic from this IP is unaffected)`);
    }
    return false;
}

// S4c per-key limiter (post-cookie; see KEY_RATE_* rationale) --------------
function keyRateAllow(hexKey) {
    if (bucketAllow(keyRateMap, warnedKeys, hexKey, KEY_RATE_WINDOW_MS, KEY_RATE_LIMIT_PER_WINDOW)) {
        return true;
    }
    if (!warnedKeys.has(hexKey)) {
        warnedKeys.add(hexKey);
        logWarn(`key-rate-limit: dropping packets for key=${shortKey4(hexKey)}... (further drops silent until the key is quiet long enough to evict)`);
    }
    return false;
}

// Aggregated logging for PRE-VALIDATION events (review HIGH-2 side effect).
//
// Before the reordering, the per-IP bucket ran first and implicitly capped
// these log lines at 10/s/IP. Moving the bucket behind the frame checks
// would otherwise turn a junk flood into an unbounded console write, and a
// blocking stdout is its own denial of service. So they are aggregated:
// keyed on a FIXED set of reason strings chosen by this file, never on
// anything an attacker supplies, so `noteMap` cannot grow.
const NOTE_INTERVAL_MS = 10 * 1000;
const noteMap = new Map(); // reason -> { count, lastLog }

function noteThrottled(logFn, reason, detail) {
    let st = noteMap.get(reason);
    if (st === undefined) {
        st = { count: 0, lastLog: -Infinity };
        noteMap.set(reason, st);
    }
    st.count += 1;
    const now = nowMs();
    if (now - st.lastLog < NOTE_INTERVAL_MS) return;
    logFn(`${reason}: ${st.count} since last report (latest: ${detail})`);
    st.count = 0;
    st.lastLog = now;
}

// --- Session sweep -----------------------------------------------------------

function sweepSessions() {
    const now = nowMs();
    let evicted = 0;
    for (const [key, entry] of sessionMap) {
        if (now - entry.lastTouch > SESSION_TTL_MS) {
            releaseSession(key, entry);
            evicted += 1;
        }
    }
    if (evicted > 0) {
        logInfo(`session sweep: evicted ${evicted}, live=${sessionMap.size}`);
    }
}

// Idle eviction: eligible when there is no in-window activity AND the
// bucket has been quiet for 60 windows. This is the SLOW path that keeps
// the maps small in normal operation; MAX_RATE_ENTRIES (review MEDIUM-3)
// is the hard bound that holds when a flood outruns this sweep.
function sweepBucketMap(map, warned, windowMs, now) {
    let evicted = 0;
    const cutoff = now - windowMs;
    for (const [k, entry] of map) {
        const inWindow = entry.timestamps.length > 0 && entry.timestamps[entry.timestamps.length - 1] > cutoff;
        if (!inWindow && now - entry.lastSeen > windowMs * 60) {
            map.delete(k);
            warned.delete(k);
            evicted += 1;
        }
    }
    return evicted;
}

function sweepRates() {
    const now = nowMs();
    const evicted = sweepBucketMap(rateMap, warnedIps, RATE_WINDOW_MS, now);
    const preEvicted = sweepBucketMap(preGateMap, warnedPreGate, PREGATE_WINDOW_MS, now);
    // S4c: same policy for the per-key buckets.
    const keyEvicted = sweepBucketMap(keyRateMap, warnedKeys, KEY_RATE_WINDOW_MS, now);
    if (evicted > 0 || preEvicted > 0 || keyEvicted > 0) {
        logInfo(`rate sweep: evicted ${evicted} ip(s) + ${preEvicted} pre-gate ip(s) + ${keyEvicted} key(s), live=${rateMap.size}/${preGateMap.size}/${keyRateMap.size}`);
    }
}

// --- Packet handlers ---------------------------------------------------------

function handleRegister(socket, buf, rinfo) {
    const sessionKeyBuf = Buffer.from(buf.subarray(8, 24)); // copy out of receive buf
    const myPublicPort = buf.readUInt16BE(24);
    if (myPublicPort !== rinfo.port) {
        logWarn(`REGISTER NAT mismatch: claimed my_public_port=${myPublicPort} but source port=${rinfo.port} (key=${shortKey4(sessionKeyBuf.toString('hex'))})`);
    }
    const hexKey = sessionKeyBuf.toString('hex');
    const source = { address: rinfo.address, port: rinfo.port };
    let entry = sessionMap.get(hexKey);
    let pairedPeer = null; // endpoint to receive an unsolicited DELIVER if we just paired
    const now = nowMs();

    if (!entry) {
        // Per-IP live-key quota (review H2 defense 1) — checked before the
        // cap so a quota-violating IP can never trigger evictions either.
        const created = creatorCounts.get(source.address) || 0;
        if (created >= MAX_NEW_KEYS_PER_IP) {
            logWarn(`REGISTER from ${source.address}:${source.port} dropped — IP already holds ${created}/${MAX_NEW_KEYS_PER_IP} live keys`);
            return;
        }
        if (sessionMap.size >= MAX_SESSIONS) {
            // Cap policy (review H2 defense 2): evict the oldest UNPAIRED
            // singleton to make room. Paired sessions are never evicted;
            // if everything is paired, only then drop the new key.
            let oldestKey = null;
            let oldestEntry = null;
            for (const [k, e] of sessionMap) {
                if (e.endpointB === null && (oldestEntry === null || e.lastTouch < oldestEntry.lastTouch)) {
                    oldestKey = k;
                    oldestEntry = e;
                }
            }
            if (oldestKey === null) {
                logWarn(`REGISTER from ${source.address}:${source.port} dropped — session table full of PAIRED sessions (${sessionMap.size}/${MAX_SESSIONS})`);
                return;
            }
            releaseSession(oldestKey, oldestEntry);
            logWarn(`session table full — evicted oldest unpaired singleton key=${shortKey4(oldestKey)}... to admit ${source.address}:${source.port}`);
        }
        entry = { endpointA: source, endpointB: null, lastTouch: now, lastSeenA: now, lastSeenB: 0, creatorIp: source.address, joinerPortReclaims: 0 };
        sessionMap.set(hexKey, entry);
        creatorCounts.set(source.address, created + 1);
    } else if (entry.endpointA && endpointEq(entry.endpointA, source)) {
        entry.lastSeenA = now; // idempotent re-REGISTER from A
    } else if (entry.endpointB && endpointEq(entry.endpointB, source)) {
        entry.lastSeenB = now; // idempotent re-REGISTER from B
        // Review H1 (within-stale-window re-host): if this key's HOST slot
        // is a stale endpoint from this same IP, we are a re-hosted client
        // that re-registered before its old slot crossed SLOT_STALE_MS and
        // was therefore filed as "the joiner". Promote to the host slot
        // (and free B) so a real joiner can pair; the client keeps its
        // resender alive across the interim thanks to its self-DELIVER
        // ignore gate.
        if (entry.endpointA && entry.endpointA.address === source.address &&
            now - entry.lastSeenA > SLOT_STALE_MS) {
            logInfo(`[RECLAIM] promote B->A key=${shortKey4(hexKey)}... ${entry.endpointA.address}:${entry.endpointA.port} (stale) replaced by ${source.address}:${source.port}`);
            entry.endpointA = source;
            entry.lastSeenA = now;
            entry.endpointB = null;
            entry.lastSeenB = 0;
        }
    } else {
        // Source matches neither slot exactly. Review H1: before the old
        // fill-or-drop logic, consider stale-slot reclamation so a
        // cancel-then-re-host client (same IP, new NAT source port, same
        // UPnP-pinned session key) reclaims its own slot instead of being
        // filed as "the joiner" (which DELIVERed the client its own stale
        // endpoint) or dropped as a third party (poisoned-key variant).
        const aStale = entry.endpointA !== null && now - entry.lastSeenA > SLOT_STALE_MS;
        const bStale = entry.endpointB !== null && now - entry.lastSeenB > SLOT_STALE_MS;
        if (entry.endpointA && aStale && entry.endpointA.address === source.address) {
            // Same-IP reclaim of the stale host slot. Also drop a stale
            // leftover joiner slot so the reclaimed host is not handed a
            // dead endpoint in the reply DELIVER.
            logInfo(`[RECLAIM] host slot key=${shortKey4(hexKey)}... ${entry.endpointA.address}:${entry.endpointA.port} -> ${source.address}:${source.port} (stale ${Math.round((now - entry.lastSeenA) / 1000)}s)`);
            entry.endpointA = source;
            entry.lastSeenA = now;
            if (bStale) {
                entry.endpointB = null;
                entry.lastSeenB = 0;
            }
        } else if (!entry.endpointA) {
            entry.endpointA = source;
            entry.lastSeenA = now;
        } else if (!entry.endpointB) {
            entry.endpointB = source;
            entry.lastSeenB = now;
            pairedPeer = entry.endpointA; // notify A that B has now joined
        } else if (entry.endpointB &&
                   entry.endpointB.address === source.address &&
                   (entry.joinerPortReclaims || 0) < MAX_JOINER_PORT_RECLAIMS) {
            // Task #105: the room's OWN joiner retrying on a fresh source
            // port. Same public address, port changed, slot B NOT stale
            // (attempt 1 refreshed it moments ago). Without this the retry
            // is ignored for the whole connect budget — see the long note
            // on MAX_JOINER_PORT_RECLAIMS above for why this is not a slot
            // hijack and why S4 stays closed.
            entry.joinerPortReclaims = (entry.joinerPortReclaims || 0) + 1;
            logInfo(`[RECLAIM] joiner port key=${shortKey4(hexKey)}... ${entry.endpointB.address}:${entry.endpointB.port} -> ${source.address}:${source.port} (same-IP retry ${entry.joinerPortReclaims}/${MAX_JOINER_PORT_RECLAIMS})`);
            entry.endpointB = source;
            entry.lastSeenB = now;
            pairedPeer = entry.endpointA; // re-notify A with the new joiner endpoint
        } else if (bStale) {
            // Both slots filled but the joiner slot is stale (abandoned
            // attempt). Replace it — same-IP retry from a new port, or a
            // fresh joiner arriving after an abandoned pairing — and
            // re-notify A of the new joiner endpoint.
            logInfo(`[RECLAIM] joiner slot key=${shortKey4(hexKey)}... ${entry.endpointB.address}:${entry.endpointB.port} -> ${source.address}:${source.port} (stale ${Math.round((now - entry.lastSeenB) / 1000)}s)`);
            entry.endpointB = source;
            entry.lastSeenB = now;
            pairedPeer = entry.endpointA;
        } else {
            // Both slots live with different endpoints; treat as a third party — drop it.
            // (Don't overwrite either slot. Don't reply.)
            logWarn(`REGISTER from ${source.address}:${source.port} for full session key=${shortKey4(hexKey)} — ignored`);
            return;
        }
    }
    entry.lastTouch = now;

    // Reply to source with the OTHER endpoint (or zeroes).
    const otherForSource = endpointEq(entry.endpointA, source) ? entry.endpointB : entry.endpointA;
    const reply = encodeDeliver(sessionKeyBuf, otherForSource);
    socket.send(reply, 0, DELIVER_LEN, source.port, source.address);

    // If we just paired, push unsolicited DELIVER to the previously-registered peer.
    if (pairedPeer) {
        const push = encodeDeliver(sessionKeyBuf, source);
        socket.send(push, 0, DELIVER_LEN, pairedPeer.port, pairedPeer.address);
    }

    const aStr = entry.endpointA ? 'set' : 'null';
    const bStr = entry.endpointB ? 'set' : 'null';
    logInfo(`[REGISTER] from ${source.address}:${source.port} key=${shortKey4(hexKey)}... a=${aStr} b=${bStr}`);
    if (pairedPeer) {
        logInfo(`[DELIVER] push to ${pairedPeer.address}:${pairedPeer.port} key=${shortKey4(hexKey)}... peer=${source.address}:${source.port}`);
    }
}

function handlePoll(socket, buf, rinfo) {
    const sessionKeyBuf = Buffer.from(buf.subarray(8, 24));
    const hexKey = sessionKeyBuf.toString('hex');
    const source = { address: rinfo.address, port: rinfo.port };
    const entry = sessionMap.get(hexKey);

    let peer = null;
    if (entry) {
        // Refresh lastTouch even though we don't update endpoints.
        entry.lastTouch = nowMs();
        if (endpointEq(entry.endpointA, source)) {
            entry.lastSeenA = entry.lastTouch; // slot liveness (review H1)
            peer = entry.endpointB;
        } else if (endpointEq(entry.endpointB, source)) {
            entry.lastSeenB = entry.lastTouch;
            peer = entry.endpointA;
        } else {
            // Source isn't a registered endpoint for this key.
            // Reply with zeroes — caller is asking but isn't pinned.
            peer = null;
        }
    }

    const reply = encodeDeliver(sessionKeyBuf, peer);
    socket.send(reply, 0, DELIVER_LEN, source.port, source.address);

    const aStr = entry && entry.endpointA ? 'set' : 'null';
    const bStr = entry && entry.endpointB ? 'set' : 'null';
    logInfo(`[POLL] from ${source.address}:${source.port} key=${shortKey4(hexKey)}... a=${aStr} b=${bStr}`);
}

// --- S4c return-routability gate ---------------------------------------------

// Runs between "the frame is well-formed" and "we touch any state".
// Returns the hex session key when the request may proceed, or null when
// it was answered with a CHALLENGE / dropped.
//
// Ordering is load-bearing:
//   1. cookie check FIRST — a request that has not proven return
//      routability must not be able to create, refresh, evict, or even
//      NAME a session, and must not consume the victim key's rate budget
//      or the victim IP's COOKIED budget.
//   2. uncookied requests draw on the SEPARATE pre-gate budget (review
//      HIGH-2). That budget exists to bound CHALLENGE emission toward one
//      address; it is deliberately not the same bucket as (2b), because
//      sharing one bucket is exactly what let spoofed traffic starve a
//      real client. Over budget => silent drop, still binding nothing.
//   2b. per-IP cap on COOKIED traffic — the source is proven, so this is
//      now a throttle on a real client rather than a lockout weapon.
//   3. per-key cap LAST — bound how much one session key may be hammered
//      by an attacker who controls many real (cookie-capable) source
//      addresses and therefore slips past the per-IP bucket.
// The gate NEVER binds state and NEVER hangs: every path either sends
// exactly one 32-byte CHALLENGE or returns silently.
function returnRoutabilityGate(socket, buf, rinfo, what) {
    const sessionKeyBuf = Buffer.from(buf.subarray(8, 24));
    const hexKey = sessionKeyBuf.toString('hex');
    const cookieBuf = Buffer.from(buf.subarray(28, 28 + COOKIE_LEN));

    if (!cookieValid(cookieBuf, rinfo)) {
        // Uncookied (first contact) or stale/forged/wrong-endpoint cookie.
        // Answer with a CHALLENGE bound to THIS source and bind nothing.
        // A spoofing sender has the challenge delivered to the address it
        // is impersonating and therefore never learns the cookie.
        if (!preGateAllow(rinfo.address)) {
            return null;
        }
        const challenge = encodeChallenge(sessionKeyBuf, rinfo);
        socket.send(challenge, 0, CHALLENGE_LEN, rinfo.port, rinfo.address);
        const uncookied = cookieBuf.every((b) => b === 0);
        // Aggregated: one uncookied packet per line would be an unbounded
        // console write under a flood (see noteThrottled).
        noteThrottled(logInfo, '[CHALLENGE] uncookied/stale request',
            `${what} from ${rinfo.address}:${rinfo.port} key=${shortKey4(hexKey)}... (${uncookied ? 'uncookied' : 'stale/invalid cookie'}) — no state bound`);
        return null;
    }

    if (!rateLimitAllow(rinfo.address)) {
        return null;
    }
    if (!keyRateAllow(hexKey)) {
        return null;
    }
    return hexKey;
}

// --- Top-level dispatch ------------------------------------------------------

// Ordering here is load-bearing (review HIGH-2 + MEDIUM-3).
//
// It used to be: rateLimitAllow(rinfo.address) FIRST, before the length,
// magic and version checks and before the cookie gate. That was wrong
// twice over:
//   * HIGH-2 — the bucket was keyed on the source address only, so 10
//     spoofed 36-byte REGISTERs/s carrying a victim's address (~2.9
//     kbit/s, and the room code still reveals the address) exhausted the
//     VICTIM's budget. The victim's own correctly-cookied REGISTER then
//     got zero replies and bound nothing: a permanent, cheap matchmaking
//     lockout of a named host, reported to the user as RENDEZVOUS_DOWN.
//   * MEDIUM-3 — running first meant a Map entry was allocated for the
//     claimed source of EVERY datagram, including 1-byte junk, before a
//     single byte had been validated.
// Now: allocation-free frame checks first (so junk costs nothing at all),
// then the cookie gate, and only then the per-IP bucket — on a bucket
// that only cookie-holding sources can reach. Uncookied first contact has
// its own, separate, larger pre-gate budget inside the gate.
function onMessage(socket, buf, rinfo) {
    if (buf.length < 8) {
        noteThrottled(logWarn, 'drop: short packet', `len=${buf.length} from ${rinfo.address}:${rinfo.port}`);
        return;
    }
    const magic = buf.readUInt32BE(0);
    if (magic !== MAGIC) {
        // Magic mismatch — silent drop per spec ("if not 0x33535852, drop").
        return;
    }
    const version = buf.readUInt8(4);
    if (version !== VERSION) {
        // Version interlock (S4c): a v1 client's 28-byte REGISTER lands
        // here and is dropped CLEANLY — logged, no reply, no state, no
        // timer, no hang. The client sees total silence and its existing
        // budget expiry classifies P2P_FAIL_RENDEZVOUS_DOWN. This is the
        // authorized breaking change; see docs/plan-netplay-connection.md
        // §6 for the full mixed-version matrix.
        noteThrottled(logWarn, 'drop: unsupported version',
            `version=${version} from ${rinfo.address}:${rinfo.port} (this server speaks v${VERSION} only)`);
        return;
    }
    const type = buf.readUInt8(5);
    if (type === TYPE_REGISTER || type === TYPE_POLL) {
        // ONE shared gate for both cookied request types. (It briefly
        // carried a third — the S5 relay's RELAY_REQ, made byte-identical
        // to REGISTER for exactly this reason. The relay is deleted; the
        // gate is otherwise untouched, and REGISTER/POLL take the same
        // path through it that they always have.)
        const what = type === TYPE_REGISTER ? 'REGISTER' : 'POLL';
        const wantLen = type === TYPE_REGISTER ? REGISTER_LEN : POLL_LEN;
        if (buf.length !== wantLen) {
            noteThrottled(logWarn, `drop: bad ${what} length`, `len=${buf.length} from ${rinfo.address}:${rinfo.port}`);
            return;
        }
        if (returnRoutabilityGate(socket, buf, rinfo, what) === null) {
            return;
        }
        if (type === TYPE_REGISTER) {
            handleRegister(socket, buf, rinfo);
        } else {
            handlePoll(socket, buf, rinfo);
        }
    } else if (type === TYPE_CHALLENGE) {
        // Server -> client only. A client sending us one is confused or
        // hostile; never let it reach state.
        noteThrottled(logWarn, 'drop: unexpected CHALLENGE', `from ${rinfo.address}:${rinfo.port}`);
        return;
    } else if (type === TYPE_DELIVER) {
        // Server doesn't accept DELIVER from clients.
        noteThrottled(logWarn, 'drop: unexpected DELIVER', `from ${rinfo.address}:${rinfo.port}`);
        return;
    } else {
        noteThrottled(logWarn, 'drop: unknown type', `type=${type} from ${rinfo.address}:${rinfo.port}`);
        return;
    }
}

// --- Server lifecycle --------------------------------------------------------

function start(port) {
    const socket = dgram.createSocket('udp4');
    let sessionInterval = null;
    let rateInterval = null;

    socket.on('message', (buf, rinfo) => {
        try {
            onMessage(socket, buf, rinfo);
        } catch (err) {
            logWarn(`handler error: ${err && err.stack ? err.stack : err}`);
        }
    });

    socket.on('error', (err) => {
        logWarn(`socket error: ${err.message}`);
    });

    socket.on('listening', () => {
        const addr = socket.address();
        logInfo(`bound udp4 ${addr.address}:${addr.port}`);
    });

    let shuttingDown = false;
    function shutdown(reason) {
        if (shuttingDown) return;
        shuttingDown = true;
        logInfo(`shutting down (${reason})`);
        if (sessionInterval) clearInterval(sessionInterval);
        if (rateInterval) clearInterval(rateInterval);
        try {
            socket.close(() => process.exit(0));
        } catch (_) {
            process.exit(0);
        }
        // Safety net.
        setTimeout(() => process.exit(0), 1000).unref();
    }

    process.on('SIGTERM', () => shutdown('SIGTERM'));
    process.on('SIGINT', () => shutdown('SIGINT'));

    sessionInterval = setInterval(sweepSessions, SESSION_SWEEP_INTERVAL_MS);
    rateInterval = setInterval(sweepRates, RATE_SWEEP_INTERVAL_MS);

    socket.bind(port);

    // Test hooks: expose minimal poke at internal state.
    return {
        socket,
        _sessionMap: sessionMap,
        _rateMap: rateMap,
        _sessionTtlMs: SESSION_TTL_MS,
        _maxSessions: MAX_SESSIONS,
        _slotStaleMs: SLOT_STALE_MS,
        _maxJoinerPortReclaims: MAX_JOINER_PORT_RECLAIMS, // #105
        _sweepNow() {
            sweepSessions();
            sweepRates();
        },
        // Clears ALL THREE rate buckets — most callers just want a clean
        // budget. _resetKeyRate() isolates the per-key one.
        _resetRate() {
            rateMap.clear();
            warnedIps.clear();
            preGateMap.clear();
            warnedPreGate.clear();
            keyRateMap.clear();
            warnedKeys.clear();
        },
        _resetSessions() {
            sessionMap.clear();
            creatorCounts.clear();
        },
        _creatorCounts: creatorCounts,
        _maxNewKeysPerIp: MAX_NEW_KEYS_PER_IP,
        // --- S4c hooks ---------------------------------------------------
        _version: VERSION,
        _registerLen: REGISTER_LEN,
        _cookieLen: COOKIE_LEN,
        _cookieRotateMs: COOKIE_ROTATE_MS,
        _keyRateMap: keyRateMap,
        _keyRateLimit: KEY_RATE_LIMIT_PER_WINDOW,
        // Exposed so the budget tests can assert the window really is 1 s
        // rather than ASSUMING "per window == per second" while doing
        // their arithmetic in requests/second.
        _keyRateWindowMs: KEY_RATE_WINDOW_MS,
        // --- Review HIGH-2 / MEDIUM-3 hooks ------------------------------
        _preGateMap: preGateMap,
        _preGateLimit: PREGATE_LIMIT_PER_WINDOW,
        _rateLimit: RATE_LIMIT_PER_WINDOW,
        _maxRateEntries: MAX_RATE_ENTRIES,
        _resetKeyRate() {
            keyRateMap.clear();
            warnedKeys.clear();
        },
        // Mint the cookie this server would issue to (address, port).
        // `slotOffset` reaches back to older rotation slots so tests can
        // exercise the accept-previous / reject-older window without
        // sleeping 60 s. Test-only: the real oracle is the CHALLENGE.
        _cookieFor(address, port, slotOffset) {
            return cookieForSlot(address, port, currentCookieSlot() + (slotOffset || 0));
        },
        // Inject a packet as if it arrived from rinfo — lets tests exercise
        // source-IP-dependent policy (slot reclaim identity, spoofed-source
        // floods) that loopback UDP cannot produce. fakeSocket, when given,
        // captures outbound sends instead of hitting the wire.
        _onMessage(buf, rinfo, fakeSocket) {
            onMessage(fakeSocket || socket, buf, rinfo);
        },
        _shutdown: shutdown,
    };
}

// --- CLI entrypoint ----------------------------------------------------------

if (require.main === module) {
    const argPort = process.argv[2];
    let port = 3478;
    if (argPort !== undefined) {
        const n = Number(argPort);
        if (!Number.isInteger(n) || n < 0 || n > 65535) {
            logWarn(`invalid port arg "${argPort}", expected 0..65535`);
            process.exit(2);
        }
        port = n;
    }
    start(port);
}

module.exports = { start };
