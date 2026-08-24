#!/usr/bin/env node
// 3SX rendezvous server: single-purpose UDP endpoint exchange.
// See ../../docs/plan-bilateral-hole-punch.md Decision 2 for the wire spec.

'use strict';

const dgram = require('dgram');
const { performance } = require('perf_hooks');

// --- Wire constants ----------------------------------------------------------

const MAGIC = 0x33535852; // '3SXR' big-endian
const VERSION = 1;
const TYPE_REGISTER = 1;
const TYPE_DELIVER = 2;
const TYPE_POLL = 3;

const REGISTER_LEN = 28;
const POLL_LEN = 28;
const DELIVER_LEN = 32;

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
// objects + a hex key (~250 bytes) => worst case ~1 MB. Before S1 the 60 s
// TTL implicitly bounded slot-squatting; at 10 minutes an abuser inside the
// per-IP rate limit (10 pkt/s) could otherwise park ~6000 keys per IP.
// When full, REGISTERs for brand-new keys are dropped (logged); existing
// sessions keep working, so a squatter cannot evict live hosts.
const MAX_SESSIONS = 4096;

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
// value: { endpointA, endpointB, lastTouch, lastSeenA, lastSeenB }
// lastSeenA/B track per-slot liveness (last REGISTER/POLL from that exact
// endpoint) for the SLOT_STALE_MS reclaim logic; lastTouch remains the
// whole-entry TTL clock.

const rateMap = new Map();
// key: source IP string
// value: { timestamps: number[], lastSeen: number }
// `timestamps` holds send times within the current sliding window
// (anything older than RATE_WINDOW_MS is filtered out on each access).

const warnedIps = new Set();
// IPs we've already warned about. Cleared when the rateMap entry is evicted
// (i.e. the IP has been quiet long enough to be swept).

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

// --- Session sweep -----------------------------------------------------------

function sweepSessions() {
    const now = nowMs();
    let evicted = 0;
    for (const [key, entry] of sessionMap) {
        if (now - entry.lastTouch > SESSION_TTL_MS) {
            sessionMap.delete(key);
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
    if (evicted > 0) {
        logInfo(`rate sweep: evicted ${evicted}, live=${rateMap.size}`);
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
        if (sessionMap.size >= MAX_SESSIONS) {
            logWarn(`REGISTER from ${source.address}:${source.port} dropped — session table full (${sessionMap.size}/${MAX_SESSIONS})`);
            return;
        }
        entry = { endpointA: source, endpointB: null, lastTouch: now, lastSeenA: now, lastSeenB: 0 };
        sessionMap.set(hexKey, entry);
    } else if (entry.endpointA && endpointEq(entry.endpointA, source)) {
        entry.lastSeenA = now; // idempotent re-REGISTER from A
    } else if (entry.endpointB && endpointEq(entry.endpointB, source)) {
        entry.lastSeenB = now; // idempotent re-REGISTER from B
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
        logWarn(`drop: unsupported version=${version} from ${rinfo.address}:${rinfo.port}`);
        return;
    }
    const type = buf.readUInt8(5);
    if (type === TYPE_REGISTER) {
        if (buf.length !== REGISTER_LEN) {
            logWarn(`drop: bad REGISTER len=${buf.length} from ${rinfo.address}:${rinfo.port}`);
            return;
        }
        handleRegister(socket, buf, rinfo);
    } else if (type === TYPE_POLL) {
        if (buf.length !== POLL_LEN) {
            logWarn(`drop: bad POLL len=${buf.length} from ${rinfo.address}:${rinfo.port}`);
            return;
        }
        handlePoll(socket, buf, rinfo);
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
        _resetRate() {
            rateMap.clear();
            warnedIps.clear();
        },
        _resetSessions() {
            sessionMap.clear();
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
