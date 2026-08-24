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
// S5 relay (docs/plan-netplay-connection.md §7). Types 5/6 ride the SAME
// UDP port, process and systemd unit as the rest of the protocol; types
// 7/8 only ever appear on a per-session relay port (see relayAllocate).
// The version byte is UNCHANGED at 2 — a v2 client that never sends a
// RELAY_REQ is indistinguishable from a pre-S5 one, so this is a pure
// extension, not a breaking change.
const TYPE_RELAY_REQ = 5;      // client -> server, main port
const TYPE_RELAY_GRANT = 6;    // server -> client, main port
const TYPE_RELAY_PIN = 7;      // client -> relay port
const TYPE_RELAY_PIN_ACK = 8;  // relay port -> client

const REGISTER_LEN = 36;
const POLL_LEN = 36;
const DELIVER_LEN = 32;
const CHALLENGE_LEN = 32;
const COOKIE_LEN = 8;

// RELAY_REQ deliberately mirrors REGISTER byte-for-byte in the fields the
// return-routability gate reads (key at [8..24), cookie at [28..36)), so
// it passes through returnRoutabilityGate unmodified — one gate, one
// cookie scheme, no second code path to get wrong.
const RELAY_REQ_LEN = 36;
// RELAY_GRANT is 36 bytes for a 36-byte request: amplification factor
// exactly 1.0, so the relay handshake cannot make this server a
// reflector any more than REGISTER/DELIVER already could. It carries no
// relay IP — the client uses the address it already reached us at, which
// is also the address the GRANT arrives from.
const RELAY_GRANT_LEN = 36;
const RELAY_PIN_LEN = 20;
const RELAY_PIN_ACK_LEN = 12;   // 12 for 20: attenuator, factor 0.6
const RELAY_TOKEN_LEN = 8;

const RELAY_STATUS_GRANTED = 0;
const RELAY_STATUS_POOL_EXHAUSTED = 1;
const RELAY_STATUS_NOT_PAIRED = 2;
const RELAY_SLOT_NONE = 0xff;

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
// single session. A legitimate pair peaks at ~2.5 pkt/s (host 0.2/s +
// joiner 2/s + one challenge round each), so 10/s/key is generous.
// Enforced AFTER cookie validation so spoofed traffic (which never
// binds anyway) cannot consume a victim key's budget.
const KEY_RATE_WINDOW_MS = 1000;
const KEY_RATE_LIMIT_PER_WINDOW = 10;

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

// --- S5 relay tunables -------------------------------------------------------
//
// WHY A CUSTOM RELAY AND NOT COTURN (docs/plan-netplay-connection.md §7):
// do_handoff (src/netplay/direct_p2p.c) transfers a BARE
// NET_DatagramSocket into netplay.c, after which GekkoNet owns plain
// send/recv on it through src/netplay/sdl_net_adapter.c. A TURN
// allocation would interpose Allocate/CreatePermission/ChannelData
// framing on EVERY packet plus refresh timers, on a 60 Hz rollback
// game's hot path — GekkoNet does not speak any of that. This relay
// forwards raw datagrams unmodified, so the relayed socket is
// behaviourally identical to a punched one and neither the MIST
// handshake nor GekkoNet changes at all. Secondary reason: coturn would
// add a scanner-recognisable public service to a VPS that also runs
// unrelated infrastructure.
//
// One UDP port per RELAYED SESSION, drawn from a bounded pool. A port
// per session (rather than demultiplexing many sessions on one port by
// source address) is what keeps the forward path a pure "send the bytes
// to the other pinned endpoint" with no header of our own: the port IS
// the session identifier.
//
// DEPLOYMENT NOTE: the firewall must allow inbound UDP on
// RELAY_PORT_BASE .. RELAY_PORT_BASE + RELAY_POOL_SIZE - 1 in addition
// to the main rendezvous port. start() logs the range at boot.
const RELAY_PORT_BASE = 34000;
const RELAY_POOL_SIZE = 100;

// A relayed session is reclaimed after this long with no pin and no
// forwarded datagram. A live session sends ~120 datagrams/s, so 30 s of
// total silence means both ends are gone (or the match ended); the pool
// is small enough that holding dead entries for the 10-minute session
// TTL would exhaust it. Reclaim closes the socket and frees the port.
const RELAY_IDLE_MS = 30 * 1000;

// Per-session forwarding cap, enforced by DROPPING over-budget datagrams
// (never by closing the session — a rollback netcode absorbs loss, but a
// mid-match teardown is unrecoverable). GekkoNet at 60 Hz costs roughly
// 5 kB/s per direction, so 64 KiB/s is ~12x headroom for a real match
// while bounding what a single relayed session can cost the box: 100
// sessions x 64 KiB/s x 2 directions is the worst case this pool can
// produce. Implemented as a token bucket refilled at the cap rate with a
// one-second burst.
const RELAY_BYTES_PER_SEC = 64 * 1024;

// Relay tokens rotate on the same 60 s cadence as the S4c cookie and,
// like it, the CURRENT and PREVIOUS slots both validate — that pair of
// slots IS the token's expiry (60..120 s), long enough to cover the
// GRANT -> PIN round trip many times over and short enough that a
// captured token dies with the match.
const RELAY_TOKEN_ROTATE_MS = 60 * 1000;

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

// --- S5 relay token ----------------------------------------------------------

// token = HMAC-SHA256(server_secret, "relay:<hexKey>:<side>:<slot>")[0..7]
//
// This REUSES the S4c secret machinery (one 32-byte `cookieSecret` drawn
// from crypto.randomBytes at process start, never leaving the process)
// rather than inventing a second scheme, and domain-separates with the
// "relay:" prefix so a cookie can never be replayed as a token or vice
// versa. `slot` is the rotation slot; accepting the current and previous
// slot gives the token a 60..120 s expiry.
//
// What it proves: the holder was told this token by THIS server, for
// THIS session key and THIS side. The server only ever emits one to an
// endpoint that already (a) passed the return-routability cookie gate
// and (b) occupies one of the two slots of a PAIRED session. So a token
// on the wire is a capability for exactly one side of one relayed
// session, expiring with the rotation window.
function relayTokenForSlot(hexKey, side, slot) {
    return crypto
        .createHmac('sha256', cookieSecret)
        .update(`relay:${hexKey}:${side}:${slot}`)
        .digest()
        .subarray(0, RELAY_TOKEN_LEN);
}

function currentRelaySlot() {
    return Math.floor(Date.now() / RELAY_TOKEN_ROTATE_MS);
}

// Constant-time check against the current and previous rotation slots
// (same shape and same reasoning as cookieValid).
function relayTokenValid(hexKey, side, tokenBuf) {
    if (!tokenBuf || tokenBuf.length !== RELAY_TOKEN_LEN) return false;
    const slot = currentRelaySlot();
    for (const s of [slot, slot - 1]) {
        if (crypto.timingSafeEqual(tokenBuf, relayTokenForSlot(hexKey, side, s))) {
            return true;
        }
    }
    return false;
}

// magic(4) ver(1) type(1)=6 slot(1) status(1) key(16) port(2) reserved(2)
// token(8) = 36 bytes. `port`/`token` are zero on every refusal, and the
// slot byte is RELAY_SLOT_NONE, so a client that ignores `status` still
// cannot mistake a refusal for a grant (port 0 is not connectable).
function encodeRelayGrant(sessionKeyBuf, side, status, port, tokenBuf) {
    const buf = Buffer.alloc(RELAY_GRANT_LEN);
    buf.writeUInt32BE(MAGIC, 0);
    buf.writeUInt8(VERSION, 4);
    buf.writeUInt8(TYPE_RELAY_GRANT, 5);
    buf.writeUInt8(side & 0xff, 6);
    buf.writeUInt8(status & 0xff, 7);
    sessionKeyBuf.copy(buf, 8, 0, 16);
    buf.writeUInt16BE(port & 0xffff, 24);
    buf.writeUInt16BE(0, 26); // reserved
    if (tokenBuf) tokenBuf.copy(buf, 28, 0, RELAY_TOKEN_LEN);
    return buf;
}

function encodeRelayPinAck(side, peerPinned) {
    const buf = Buffer.alloc(RELAY_PIN_ACK_LEN);
    buf.writeUInt32BE(MAGIC, 0);
    buf.writeUInt8(VERSION, 4);
    buf.writeUInt8(TYPE_RELAY_PIN_ACK, 5);
    buf.writeUInt8(side & 0xff, 6);
    buf.writeUInt8(peerPinned ? 1 : 0, 7);
    buf.writeUInt32BE(0, 8); // reserved
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
    // A relay only exists to carry a session's traffic; when the session
    // is gone the port SHOULD go back to the pool rather than waiting out
    // RELAY_IDLE_MS — but only if the relay is not carrying a live match.
    //
    // Review CRITICAL-1. This used to be unconditional, and that killed
    // every relayed match at exactly SESSION_TTL_MS. Nothing refreshes a
    // session's `lastTouch` during a relayed match: only handleRegister /
    // handlePoll / handleRelayReq touch it, and after the handoff neither
    // client ever speaks to the MAIN rendezvous port again (the joiner
    // returns DIRECT_P2P_HANDOFF and its worker ends; the host raises
    // s_bilateral_handoff_pending and its punch worker returns, the
    // rendezvous worker having already exited). So `lastTouch` freezes at
    // the last RELAY_REQ — i.e. at setup — and ten minutes into gameplay
    // sweepSessions evicted the entry, closed the relay socket and
    // returned the port to the pool. Both clients hold NAT mappings only
    // toward that relay endpoint, so both went instantly silent, mid-match,
    // unrecoverably. Reproduced against this module: at the boundary the
    // relay's own lastActivity was 2 ms old, so the idle reclaim would
    // never have fired; the session TTL alone did it.
    //
    // The fix is to let sweepRelays own relay lifetime EXCLUSIVELY, because
    // the relay already has the correct liveness clock (lastActivity, which
    // forwarded traffic refreshes ~120 times a second). A relay that really
    // is finished still goes back to the pool RELAY_IDLE_MS later, via the
    // same 5 s sweep — so the pool-pressure argument for immediate release
    // costs at most 30 s of one port.
    //
    // (§7.3's rationale for the 30 s idle reclaim reasoned explicitly about
    // SESSION_TTL_MS and still missed this reverse coupling. Noted so the
    // next reader checks BOTH directions of a lifetime dependency.)
    const relay = relayMap.get(hexKey);
    if (relay !== undefined && nowMs() - relay.lastActivity < RELAY_IDLE_MS) {
        logInfo(`[RELAY] key=${shortKey4(hexKey)}... port=${relay.port} OUTLIVES its session ` +
            `(active ${Math.round(nowMs() - relay.lastActivity)} ms ago; sweepRelays owns it from here)`);
        return;
    }
    relayRelease(hexKey, 'session released');
}

// --- S5 relay state ----------------------------------------------------------

const relayMap = new Map();
// key: hex session key; value: {
//   hexKey, port, socket,
//   side: [ {ep|null}, {ep|null} ],   // 0 = slot A (host), 1 = slot B (joiner)
//   lastActivity, allowance, lastRefill,
//   forwarded, forwardedBytes, dropUnpinned, dropCap, pinRejects, createdAt
// }
// Bounded by RELAY_POOL_SIZE — the pool IS the cap.

const relayPortInUse = new Map(); // port -> hexKey
const relayPortBlocked = new Set();
// Ports whose bind() failed (already in use by something else on the
// box). Without this the linear scan below would hand out the same dead
// port on every retry; with it the pool self-heals down to whatever is
// actually bindable.

function relayRelease(hexKey, why) {
    const r = relayMap.get(hexKey);
    if (r === undefined) return;
    relayMap.delete(hexKey);
    relayPortInUse.delete(r.port);
    try {
        r.socket.close();
    } catch (_) {
        // already closed / never bound
    }
    logInfo(`[RELAY] released key=${shortKey4(hexKey)}... port=${r.port} (${why}) ` +
        `fwd=${r.forwarded}/${r.forwardedBytes}B dropUnpinned=${r.dropUnpinned} ` +
        `dropCap=${r.dropCap} pinRejects=${r.pinRejects} pinSourceRejects=${r.pinSourceRejects}`);
}

// Review HIGH-1: which source IP may pin `side`.
//
// The defect this closes: every frame on the MAIN port is source-bound by
// the S4c cookie gate, but RELAY_PIN on the relay port was bound to
// NOTHING but the token — and the token is a capability for
// (hexKey, side, slot) only. relayOnMessage recorded rinfo.address/port
// verbatim, never asking that address to prove it can receive. So a party
// legitimately holding a side-0 token (trivially arranged by creating your
// own session with two of your own sockets) could present it from a
// SPOOFED source; the relay pinned side 0 to an arbitrary victim and then
// forwarded everything the attacker sent as side 1 to that victim at up to
// RELAY_BYTES_PER_SEC. Reproduced: 200x1200 B offered, 54 datagrams /
// 64800 B forwarded to the victim, plus an unsolicited PIN_ACK. It is 1:1,
// so source-laundering and uplink burn rather than classic amplification —
// still an off-path-drivable reflector, and still unshippable.
//
// The fix binds the pin to the IP that the session's return-routability
// gate already proved receives at its own address: a slot's registered IP.
// An off-path attacker cannot get a victim's IP into a session slot,
// because doing so requires answering a CHALLENGE delivered to the victim.
//
// MATCHING IS IP-ONLY, DELIBERATELY, AND MUST STAY THAT WAY. A symmetric
// NAT hands out a DIFFERENT mapping toward the relay port than toward the
// rendezvous port — which is precisely the case S5 exists for. Requiring
// the port to match would break the entire stage for exactly the users it
// was built for. testRelayPinSourceBoundToSlotIp asserts both halves: a
// wrong IP is refused, and the right IP from a DIFFERENT PORT is accepted.
//
// Source of truth is the LIVE session entry (it tracks slot reclaim), with
// the snapshot taken at grant time as the fallback for a relay that has
// outlived its session — which review CRITICAL-1's fix makes reachable.
function relaySlotIp(relay, side) {
    const entry = sessionMap.get(relay.hexKey);
    if (entry !== undefined) {
        const ep = side === 0 ? entry.endpointA : entry.endpointB;
        if (ep) return ep.address;
    }
    return relay.slotIp[side];
}

// True when `rinfo` is allowed to pin (or re-pin) `side`.
//
// Runs BEFORE the HMAC (review MEDIUM-3): an unknown source is now
// rejected on a Map lookup and a string compare instead of costing up to
// two HMAC-SHA256 plus a timingSafeEqual.
function relayPinSourceAllowed(relay, side, rinfo) {
    const s = relay.side[side];
    if (s.ep !== null) {
        // Already pinned: only the pinned endpoint may re-pin (idempotent
        // re-ACK). Holding a token cannot hijack a live side — this is the
        // pre-existing no-hijack rule, just moved ahead of the HMAC.
        return s.ep.address === rinfo.address && s.ep.port === rinfo.port;
    }
    const want = relaySlotIp(relay, side);
    return want !== null && want !== undefined && want === rinfo.address;
}

// Token bucket over forwarded bytes. Refills at RELAY_BYTES_PER_SEC with
// a one-second burst ceiling; an over-budget datagram is DROPPED, which
// is the only safe over-budget action mid-match (see RELAY_BYTES_PER_SEC).
function relayBandwidthAllow(relay, bytes, now) {
    const dt = now - relay.lastRefill;
    if (dt > 0) {
        relay.allowance = Math.min(
            RELAY_BYTES_PER_SEC,
            relay.allowance + (dt / 1000) * RELAY_BYTES_PER_SEC);
        relay.lastRefill = now;
    }
    if (relay.allowance < bytes) return false;
    relay.allowance -= bytes;
    return true;
}

// The relay socket's whole job. Two frame classes and nothing else:
//
//   1. RELAY_PIN — the ONLY frame this socket interprets. A valid token
//      for a side that is not yet pinned records the source endpoint;
//      the same endpoint re-pinning is idempotent (and re-ACKed, which
//      is how a client learns its peer has arrived). A valid token from
//      a DIFFERENT endpoint for an already-pinned side is refused, so
//      holding the token cannot hijack a live side.
//   2. Everything else — forwarded VERBATIM to the other pinned
//      endpoint, or dropped. No framing, no rewriting, no inspection:
//      that is what makes the relayed socket behaviourally identical to
//      a punched one for GekkoNet and for the MIST handshake.
//
// Nothing is ever answered without a valid token, so this socket is not
// a reflector: an unpinned source with no token gets silence.
function relayOnMessage(relay, buf, rinfo) {
    const now = nowMs();

    if (buf.length === RELAY_PIN_LEN &&
        buf.readUInt32BE(0) === MAGIC &&
        buf.readUInt8(4) === VERSION &&
        buf.readUInt8(5) === TYPE_RELAY_PIN) {
        const side = buf.readUInt8(6);
        if (side !== 0 && side !== 1) {
            relay.pinRejects += 1;
            return;
        }
        // Review HIGH-1 + MEDIUM-3: source admission BEFORE the HMAC. The
        // token alone is a capability for (key, side, slot) and proves
        // nothing about who is holding it, so the source IP must be one the
        // S4c cookie gate already proved receives at its own address — a
        // registered slot IP. IP only, never port: see relayPinSourceAllowed.
        if (!relayPinSourceAllowed(relay, side, rinfo)) {
            relay.pinSourceRejects += 1;
            return; // silent — a wrong guess must look like a dead port
        }
        const token = Buffer.from(buf.subarray(8, 8 + RELAY_TOKEN_LEN));
        if (!relayTokenValid(relay.hexKey, side, token)) {
            relay.pinRejects += 1;
            return; // silent — a wrong guess must look like a dead port
        }
        const s = relay.side[side];
        if (s.ep === null) {
            s.ep = { address: rinfo.address, port: rinfo.port };
            logInfo(`[RELAY] pin key=${shortKey4(relay.hexKey)}... port=${relay.port} ` +
                `side=${side} <- ${rinfo.address}:${rinfo.port}`);
        }
        relay.lastActivity = now;
        const ack = encodeRelayPinAck(side, relay.side[1 - side].ep !== null);
        relay.socket.send(ack, 0, RELAY_PIN_ACK_LEN, rinfo.port, rinfo.address);
        return;
    }

    let from = -1;
    for (let i = 0; i < 2; i++) {
        const e = relay.side[i].ep;
        if (e !== null && e.address === rinfo.address && e.port === rinfo.port) {
            from = i;
            break;
        }
    }
    if (from < 0) {
        relay.dropUnpinned += 1; // unknown source: never forwarded, never answered
        return;
    }
    const dst = relay.side[1 - from].ep;
    if (dst === null) {
        relay.dropUnpinned += 1; // peer has not pinned yet — nowhere to send
        return;
    }
    if (!relayBandwidthAllow(relay, buf.length, now)) {
        relay.dropCap += 1;
        return;
    }
    relay.lastActivity = now;
    relay.forwarded += 1;
    relay.forwardedBytes += buf.length;
    relay.socket.send(buf, 0, buf.length, dst.port, dst.address);
}

// Allocate (or return the existing) relay for `hexKey`. Returns null when
// the pool is exhausted — the caller answers RELAY_STATUS_POOL_EXHAUSTED
// so the client can say "relay full" instead of guessing at silence.
//
// bind() is asynchronous, so the socket is returned before it is
// listening. That is safe by construction: the GRANT still has to reach
// the client and the client's first PIN still has to come back, which is
// at least one network round trip, while bind completes on the next
// event-loop turn. A bind FAILURE tears the entry down and blocklists the
// port; the client's next RELAY_REQ resend (300 ms cadence) then draws a
// different port.
function relayAllocate(hexKey) {
    const existing = relayMap.get(hexKey);
    if (existing !== undefined) return existing;
    if (relayMap.size >= RELAY_POOL_SIZE) return null;

    for (let i = 0; i < RELAY_POOL_SIZE; i++) {
        const port = RELAY_PORT_BASE + i;
        if (relayPortInUse.has(port) || relayPortBlocked.has(port)) continue;

        const socket = dgram.createSocket('udp4');
        const now = nowMs();
        const relay = {
            hexKey,
            port,
            socket,
            side: [{ ep: null }, { ep: null }],
            // Review HIGH-1: the registered slot IPs, snapshotted by
            // handleRelayReq. Only a source at slot N's IP may pin side N.
            slotIp: [null, null],
            lastActivity: now,
            allowance: RELAY_BYTES_PER_SEC,
            lastRefill: now,
            forwarded: 0,
            forwardedBytes: 0,
            dropUnpinned: 0,
            dropCap: 0,
            pinRejects: 0,
            pinSourceRejects: 0,
            createdAt: now,
        };
        socket.on('message', (b, ri) => {
            try {
                relayOnMessage(relay, b, ri);
            } catch (err) {
                logWarn(`relay handler error: ${err && err.stack ? err.stack : err}`);
            }
        });
        socket.on('error', (err) => {
            logWarn(`relay socket error on port ${port}: ${err.message}`);
            relayPortBlocked.add(port);
            relayRelease(hexKey, 'socket error');
        });
        relayMap.set(hexKey, relay);
        relayPortInUse.set(port, hexKey);
        socket.bind(port);
        logInfo(`[RELAY] allocated key=${shortKey4(hexKey)}... port=${port} ` +
            `(pool ${relayMap.size}/${RELAY_POOL_SIZE})`);
        return relay;
    }
    return null;
}

function sweepRelays() {
    const now = nowMs();
    for (const [hexKey, r] of relayMap) {
        if (now - r.lastActivity > RELAY_IDLE_MS) {
            relayRelease(hexKey, `idle > ${RELAY_IDLE_MS} ms`);
        }
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
    // S5: relays age out far faster than sessions (RELAY_IDLE_MS vs
    // SESSION_TTL_MS) because the port pool is 100, not 4096.
    sweepRelays();
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
        entry = { endpointA: source, endpointB: null, lastTouch: now, lastSeenA: now, lastSeenB: 0, creatorIp: source.address };
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

// S5: RELAY_REQ handler. Runs AFTER returnRoutabilityGate, exactly like
// REGISTER/POLL, so the source has already proven return routability.
//
// Admission is deliberately narrow — this must never become an open
// reflector or a free UDP-port dispenser:
//   * the session key must EXIST (unknown key -> silence);
//   * the source must BE one of that session's two registered slots,
//     which is what identifies which side is asking and is impossible to
//     satisfy without having completed the normal pairing (unknown
//     source -> silence, no disclosure that the key exists);
//   * the session must be PAIRED. An unpaired session has no second side
//     to relay to, and allocating a port for it would let a lone host
//     hold pool capacity. That one gets a NOT_PAIRED refusal rather than
//     silence, because the requester is provably a participant and a
//     client that cannot distinguish "refused" from "server gone" is the
//     exact reporting defect §6.5 documents.
function handleRelayReq(socket, buf, rinfo) {
    const sessionKeyBuf = Buffer.from(buf.subarray(8, 24));
    const hexKey = sessionKeyBuf.toString('hex');
    const source = { address: rinfo.address, port: rinfo.port };
    const entry = sessionMap.get(hexKey);

    if (!entry) {
        noteThrottled(logWarn, 'drop: RELAY_REQ for unknown key',
            `from ${source.address}:${source.port} key=${shortKey4(hexKey)}...`);
        return;
    }
    let side = -1;
    if (endpointEq(entry.endpointA, source)) side = 0;
    else if (endpointEq(entry.endpointB, source)) side = 1;
    if (side < 0) {
        noteThrottled(logWarn, 'drop: RELAY_REQ from a non-slot source',
            `from ${source.address}:${source.port} key=${shortKey4(hexKey)}...`);
        return;
    }

    // A live relay request is proof the pair is still trying; keep the
    // session off the TTL sweep's eviction front.
    entry.lastTouch = nowMs();
    if (side === 0) entry.lastSeenA = entry.lastTouch;
    else entry.lastSeenB = entry.lastTouch;

    if (!entry.endpointA || !entry.endpointB) {
        socket.send(
            encodeRelayGrant(sessionKeyBuf, RELAY_SLOT_NONE, RELAY_STATUS_NOT_PAIRED, 0, null),
            0, RELAY_GRANT_LEN, source.port, source.address);
        logInfo(`[RELAY_REQ] refused NOT_PAIRED key=${shortKey4(hexKey)}... from ${source.address}:${source.port}`);
        return;
    }

    const relay = relayAllocate(hexKey);
    if (relay === null) {
        socket.send(
            encodeRelayGrant(sessionKeyBuf, RELAY_SLOT_NONE, RELAY_STATUS_POOL_EXHAUSTED, 0, null),
            0, RELAY_GRANT_LEN, source.port, source.address);
        logWarn(`[RELAY_REQ] refused POOL_EXHAUSTED key=${shortKey4(hexKey)}... ` +
            `(${relayMap.size}/${RELAY_POOL_SIZE} in use)`);
        return;
    }

    // Review HIGH-1: snapshot the registered slot IPs onto the relay, so
    // the relay port can source-bind a PIN without depending on the session
    // entry still being alive (CRITICAL-1's fix lets a relay outlive it).
    // Refreshed on every grant, so a slot reclaimed between grants is
    // picked up. relaySlotIp() still prefers the live entry when there is
    // one; this is the fallback.
    relay.slotIp[0] = entry.endpointA.address;
    relay.slotIp[1] = entry.endpointB.address;

    // Both sides converge on the same port; each gets a token scoped to
    // its own side, so neither can pin the other's slot.
    const token = relayTokenForSlot(hexKey, side, currentRelaySlot());
    socket.send(
        encodeRelayGrant(sessionKeyBuf, side, RELAY_STATUS_GRANTED, relay.port, token),
        0, RELAY_GRANT_LEN, source.port, source.address);
    logInfo(`[RELAY_REQ] granted key=${shortKey4(hexKey)}... side=${side} ` +
        `port=${relay.port} to ${source.address}:${source.port}`);
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
    if (type === TYPE_REGISTER || type === TYPE_POLL || type === TYPE_RELAY_REQ) {
        // S5: RELAY_REQ shares REGISTER's length and its key/cookie
        // offsets precisely so it can ride the SAME return-routability
        // gate. A second gate would be a second thing to get wrong.
        const what = type === TYPE_REGISTER ? 'REGISTER'
            : type === TYPE_POLL ? 'POLL' : 'RELAY_REQ';
        const wantLen = type === TYPE_REGISTER ? REGISTER_LEN
            : type === TYPE_POLL ? POLL_LEN : RELAY_REQ_LEN;
        if (buf.length !== wantLen) {
            noteThrottled(logWarn, `drop: bad ${what} length`, `len=${buf.length} from ${rinfo.address}:${rinfo.port}`);
            return;
        }
        if (returnRoutabilityGate(socket, buf, rinfo, what) === null) {
            return;
        }
        if (type === TYPE_REGISTER) {
            handleRegister(socket, buf, rinfo);
        } else if (type === TYPE_POLL) {
            handlePoll(socket, buf, rinfo);
        } else {
            handleRelayReq(socket, buf, rinfo);
        }
    } else if (type === TYPE_RELAY_GRANT) {
        // Server -> client only, same as CHALLENGE.
        noteThrottled(logWarn, 'drop: unexpected RELAY_GRANT', `from ${rinfo.address}:${rinfo.port}`);
        return;
    } else if (type === TYPE_RELAY_PIN || type === TYPE_RELAY_PIN_ACK) {
        // These only ever belong on a per-session relay port. Arriving on
        // the main port they are a misconfigured client or a probe.
        noteThrottled(logWarn, 'drop: relay-port frame on the main port',
            `type=${type} from ${rinfo.address}:${rinfo.port}`);
        return;
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
        logInfo(`S5 relay pool: udp ${RELAY_PORT_BASE}-${RELAY_PORT_BASE + RELAY_POOL_SIZE - 1} ` +
            `(${RELAY_POOL_SIZE} sessions, ${RELAY_BYTES_PER_SEC} B/s each, idle reclaim ${RELAY_IDLE_MS} ms) ` +
            `— the firewall must allow this range inbound`);
    });

    let shuttingDown = false;
    function shutdown(reason) {
        if (shuttingDown) return;
        shuttingDown = true;
        logInfo(`shutting down (${reason})`);
        if (sessionInterval) clearInterval(sessionInterval);
        if (rateInterval) clearInterval(rateInterval);
        for (const k of [...relayMap.keys()]) relayRelease(k, 'shutdown');
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
        // --- S5 relay hooks -----------------------------------------------
        _relayMap: relayMap,
        _relayPortInUse: relayPortInUse,
        _relayPortBase: RELAY_PORT_BASE,
        _relayPoolSize: RELAY_POOL_SIZE,
        _relayIdleMs: RELAY_IDLE_MS,
        _relayBytesPerSec: RELAY_BYTES_PER_SEC,
        _relayTokenRotateMs: RELAY_TOKEN_ROTATE_MS,
        _relayReqLen: RELAY_REQ_LEN,
        _relayGrantLen: RELAY_GRANT_LEN,
        _relayPinLen: RELAY_PIN_LEN,
        _relayPinAckLen: RELAY_PIN_ACK_LEN,
        // Mint the token this server would issue for (key, side).
        // `slotOffset` reaches back/forward through rotation slots so a
        // test can exercise the accept-previous / reject-older window
        // without sleeping 60 s. Test-only: the real oracle is the GRANT.
        _relayTokenFor(hexKey, side, slotOffset) {
            return relayTokenForSlot(hexKey, side, currentRelaySlot() + (slotOffset || 0));
        },
        _relaySweepNow() {
            sweepRelays();
        },
        // Inject a datagram into a relay as if it arrived from `rinfo`,
        // optionally capturing the relay's outbound sends. Same shape and
        // same purpose as _onMessage: it lets a test drive source-endpoint-
        // dependent policy (pinning, hijack refusal) and count forwarded
        // bytes deterministically, which loopback UDP scheduling cannot do.
        _relayInject(hexKey, buf, rinfo, fakeSocket) {
            const r = relayMap.get(hexKey);
            if (r === undefined) return false;
            const real = r.socket;
            if (fakeSocket) r.socket = fakeSocket;
            try {
                relayOnMessage(r, buf, rinfo);
            } finally {
                r.socket = real;
            }
            return true;
        },
        _resetRelays() {
            for (const k of [...relayMap.keys()]) relayRelease(k, 'test reset');
            relayPortBlocked.clear();
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
