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

const SESSION_TTL_MS = 60 * 1000;
const SESSION_SWEEP_INTERVAL_MS = 5 * 1000;
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
// value: { endpointA, endpointB, lastTouch }

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

    if (!entry) {
        entry = { endpointA: source, endpointB: null, lastTouch: nowMs() };
        sessionMap.set(hexKey, entry);
    } else if (entry.endpointA && endpointEq(entry.endpointA, source)) {
        // idempotent re-REGISTER from A
    } else if (entry.endpointB && endpointEq(entry.endpointB, source)) {
        // idempotent re-REGISTER from B
    } else if (!entry.endpointA) {
        entry.endpointA = source;
    } else if (!entry.endpointB) {
        entry.endpointB = source;
        pairedPeer = entry.endpointA; // notify A that B has now joined
    } else {
        // both slots filled with different endpoints; treat as a third party — drop it.
        // (Don't overwrite either slot. Don't reply.)
        logWarn(`REGISTER from ${source.address}:${source.port} for full session key=${shortKey4(hexKey)} — ignored`);
        return;
    }
    entry.lastTouch = nowMs();

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
            peer = entry.endpointB;
        } else if (endpointEq(entry.endpointB, source)) {
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
        _sweepNow() {
            sweepSessions();
            sweepRates();
        },
        _resetRate() {
            rateMap.clear();
            warnedIps.clear();
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
