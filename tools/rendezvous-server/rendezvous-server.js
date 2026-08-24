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
const RATE_WINDOW_MS = 1000;
const RATE_LIMIT_PER_WINDOW = 10;

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
// worth aiming at a victim. It is also emitted under the per-IP token
// bucket, so a spoofed flood at one victim is capped at
// RATE_LIMIT_PER_WINDOW challenges/second.
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

const warnedIps = new Set();
// IPs we've already warned about. Cleared when the rateMap entry is evicted
// (i.e. the IP has been quiet long enough to be swept).

const keyRateMap = new Map();
// S4c: key = hex session key; value = { timestamps: number[], lastSeen }.
// Same sliding-window shape as rateMap, enforced post-cookie.

const warnedKeys = new Set();

// --- Rate limiter ------------------------------------------------------------

function rateLimitAllow(ip) {
    const now = nowMs();
    let entry = rateMap.get(ip);
    if (!entry) {
        entry = { timestamps: [], lastSeen: now };
        rateMap.set(ip, entry);
    }
    // Drop timestamps outside the current sliding window.
    const cutoff = now - RATE_WINDOW_MS;
    if (entry.timestamps.length > 0 && entry.timestamps[0] <= cutoff) {
        entry.timestamps = entry.timestamps.filter((t) => t > cutoff);
    }
    entry.lastSeen = now;
    if (entry.timestamps.length >= RATE_LIMIT_PER_WINDOW) {
        if (!warnedIps.has(ip)) {
            warnedIps.add(ip);
            logWarn(`rate-limit: dropping packets from ${ip} (further drops will be silent until this IP is quiet long enough to evict)`);
        }
        return false;
    }
    entry.timestamps.push(now);
    return true;
}

// S4c per-key limiter (post-cookie; see KEY_RATE_* rationale) --------------
function keyRateAllow(hexKey) {
    const now = nowMs();
    let entry = keyRateMap.get(hexKey);
    if (!entry) {
        entry = { timestamps: [], lastSeen: now };
        keyRateMap.set(hexKey, entry);
    }
    const cutoff = now - KEY_RATE_WINDOW_MS;
    if (entry.timestamps.length > 0 && entry.timestamps[0] <= cutoff) {
        entry.timestamps = entry.timestamps.filter((t) => t > cutoff);
    }
    entry.lastSeen = now;
    if (entry.timestamps.length >= KEY_RATE_LIMIT_PER_WINDOW) {
        if (!warnedKeys.has(hexKey)) {
            warnedKeys.add(hexKey);
            logWarn(`key-rate-limit: dropping packets for key=${shortKey4(hexKey)}... (further drops silent until the key is quiet long enough to evict)`);
        }
        return false;
    }
    entry.timestamps.push(now);
    return true;
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

function sweepRates() {
    const now = nowMs();
    let evicted = 0;
    for (const [ip, entry] of rateMap) {
        // Eligible when no in-window activity AND quiet for at least 60s.
        const cutoff = now - RATE_WINDOW_MS;
        const inWindow = entry.timestamps.length > 0 && entry.timestamps[entry.timestamps.length - 1] > cutoff;
        if (!inWindow && now - entry.lastSeen > RATE_WINDOW_MS * 60) {
            rateMap.delete(ip);
            warnedIps.delete(ip);
            evicted += 1;
        }
    }
    // S4c: same policy for the per-key buckets.
    let keyEvicted = 0;
    for (const [k, entry] of keyRateMap) {
        const cutoff = now - KEY_RATE_WINDOW_MS;
        const inWindow = entry.timestamps.length > 0 && entry.timestamps[entry.timestamps.length - 1] > cutoff;
        if (!inWindow && now - entry.lastSeen > KEY_RATE_WINDOW_MS * 60) {
            keyRateMap.delete(k);
            warnedKeys.delete(k);
            keyEvicted += 1;
        }
    }
    if (evicted > 0 || keyEvicted > 0) {
        logInfo(`rate sweep: evicted ${evicted} ip(s) + ${keyEvicted} key(s), live=${rateMap.size}/${keyRateMap.size}`);
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

// --- S4c return-routability gate ---------------------------------------------

// Runs between "the frame is well-formed" and "we touch any state".
// Returns the hex session key when the request may proceed, or null when
// it was answered with a CHALLENGE / dropped.
//
// Ordering is load-bearing:
//   1. cookie check FIRST — a request that has not proven return
//      routability must not be able to create, refresh, evict, or even
//      NAME a session, and must not consume the victim key's rate budget.
//   2. per-key cap SECOND — now that the source is proven, bound how
//      much one session key may be hammered by an attacker who controls
//      many real (cookie-capable) source addresses and therefore slips
//      past the per-IP bucket.
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
        const challenge = encodeChallenge(sessionKeyBuf, rinfo);
        socket.send(challenge, 0, CHALLENGE_LEN, rinfo.port, rinfo.address);
        const uncookied = cookieBuf.every((b) => b === 0);
        logInfo(`[CHALLENGE] ${what} from ${rinfo.address}:${rinfo.port} key=${shortKey4(hexKey)}... (${uncookied ? 'uncookied' : 'stale/invalid cookie'}) — no state bound`);
        return null;
    }

    if (!keyRateAllow(hexKey)) {
        return null;
    }
    return hexKey;
}

// --- Top-level dispatch ------------------------------------------------------

function onMessage(socket, buf, rinfo) {
    if (!rateLimitAllow(rinfo.address)) {
        return;
    }
    if (buf.length < 8) {
        logWarn(`drop: short packet len=${buf.length} from ${rinfo.address}:${rinfo.port}`);
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
        logWarn(`drop: unsupported version=${version} from ${rinfo.address}:${rinfo.port} (this server speaks v${VERSION} only)`);
        return;
    }
    const type = buf.readUInt8(5);
    if (type === TYPE_REGISTER) {
        if (buf.length !== REGISTER_LEN) {
            logWarn(`drop: bad REGISTER len=${buf.length} from ${rinfo.address}:${rinfo.port}`);
            return;
        }
        if (returnRoutabilityGate(socket, buf, rinfo, 'REGISTER') === null) {
            return;
        }
        handleRegister(socket, buf, rinfo);
    } else if (type === TYPE_POLL) {
        if (buf.length !== POLL_LEN) {
            logWarn(`drop: bad POLL len=${buf.length} from ${rinfo.address}:${rinfo.port}`);
            return;
        }
        if (returnRoutabilityGate(socket, buf, rinfo, 'POLL') === null) {
            return;
        }
        handlePoll(socket, buf, rinfo);
    } else if (type === TYPE_CHALLENGE) {
        // Server -> client only. A client sending us one is confused or
        // hostile; never let it reach state.
        logWarn(`drop: unexpected CHALLENGE from ${rinfo.address}:${rinfo.port}`);
        return;
    } else if (type === TYPE_DELIVER) {
        // Server doesn't accept DELIVER from clients.
        logWarn(`drop: unexpected DELIVER from ${rinfo.address}:${rinfo.port}`);
        return;
    } else {
        logWarn(`drop: unknown type=${type} from ${rinfo.address}:${rinfo.port}`);
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
        _sweepNow() {
            sweepSessions();
            sweepRates();
        },
        // Clears BOTH rate buckets — most callers just want a clean
        // budget. _resetKeyRate() isolates the per-key one.
        _resetRate() {
            rateMap.clear();
            warnedIps.clear();
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
