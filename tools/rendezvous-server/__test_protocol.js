// Self-contained protocol test for rendezvous-server.js.
// Boots the server in-process on an ephemeral port and drives it with mock
// UDP clients. Wire-format encode logic is duplicated here on purpose so the
// test catches encoding bugs in either side.
//
// Runtime budget: under 2 seconds.

'use strict';

const dgram = require('dgram');
const crypto = require('crypto');
const { start } = require('./rendezvous-server.js');

// --- Wire constants (duplicate of server) -----------------------------------

const MAGIC = 0x33535852;
const VERSION = 1;
const TYPE_REGISTER = 1;
const TYPE_DELIVER = 2;
const TYPE_POLL = 3;
const REGISTER_LEN = 28;
const POLL_LEN = 28;
const DELIVER_LEN = 32;

// --- Local encoders ---------------------------------------------------------

function makeRegister(sessionKey, myPublicPort, opts) {
    opts = opts || {};
    const len = opts.length !== undefined ? opts.length : REGISTER_LEN;
    const buf = Buffer.alloc(len);
    buf.writeUInt32BE(opts.magic !== undefined ? opts.magic : MAGIC, 0);
    buf.writeUInt8(opts.version !== undefined ? opts.version : VERSION, 4);
    buf.writeUInt8(opts.type !== undefined ? opts.type : TYPE_REGISTER, 5);
    buf.writeUInt16BE(0, 6);
    if (len >= 24) sessionKey.copy(buf, 8, 0, 16);
    if (len >= 26) buf.writeUInt16BE(myPublicPort & 0xffff, 24);
    if (len >= 28) buf.writeUInt16BE(0, 26);
    return buf;
}

function makePoll(sessionKey) {
    const buf = Buffer.alloc(POLL_LEN);
    buf.writeUInt32BE(MAGIC, 0);
    buf.writeUInt8(VERSION, 4);
    buf.writeUInt8(TYPE_POLL, 5);
    buf.writeUInt16BE(0, 6);
    sessionKey.copy(buf, 8, 0, 16);
    buf.writeUInt32BE(0, 24);
    return buf;
}

function decodeDeliver(buf) {
    if (buf.length !== DELIVER_LEN) throw new Error(`bad DELIVER len ${buf.length}`);
    if (buf.readUInt32BE(0) !== MAGIC) throw new Error(`bad DELIVER magic`);
    if (buf.readUInt8(4) !== VERSION) throw new Error(`bad DELIVER version`);
    if (buf.readUInt8(5) !== TYPE_DELIVER) throw new Error(`bad DELIVER type`);
    return {
        sessionKey: Buffer.from(buf.subarray(8, 24)),
        peerIp: `${buf[24]}.${buf[25]}.${buf[26]}.${buf[27]}`,
        peerPort: buf.readUInt16BE(28),
    };
}

// --- Test plumbing ----------------------------------------------------------

let failed = 0;
function assert(cond, msg) {
    if (!cond) {
        console.error(`ASSERT FAIL: ${msg}`);
        failed += 1;
    }
}
function assertEq(actual, expected, msg) {
    if (actual !== expected) {
        console.error(`ASSERT FAIL: ${msg} (actual=${actual} expected=${expected})`);
        failed += 1;
    }
}

function makeClient() {
    return new Promise((resolve, reject) => {
        const sock = dgram.createSocket('udp4');
        const queue = [];
        const waiters = [];
        sock.on('message', (buf, rinfo) => {
            if (waiters.length > 0) {
                const w = waiters.shift();
                clearTimeout(w.timer);
                w.resolve({ buf, rinfo });
            } else {
                queue.push({ buf, rinfo });
            }
        });
        sock.on('error', reject);
        sock.bind(0, '127.0.0.1', () => {
            resolve({
                sock,
                port: sock.address().port,
                send(buf, dstPort) {
                    return new Promise((res, rej) => {
                        sock.send(buf, 0, buf.length, dstPort, '127.0.0.1', (err) => {
                            if (err) rej(err); else res();
                        });
                    });
                },
                recv(timeoutMs) {
                    return new Promise((res, rej) => {
                        if (queue.length > 0) {
                            res(queue.shift());
                            return;
                        }
                        const timer = setTimeout(() => {
                            const idx = waiters.findIndex((w) => w.timer === timer);
                            if (idx >= 0) waiters.splice(idx, 1);
                            rej(new Error('recv timeout'));
                        }, timeoutMs);
                        waiters.push({ resolve: res, timer });
                    });
                },
                tryRecv(timeoutMs) {
                    return this.recv(timeoutMs).then(
                        (m) => m,
                        () => null,
                    );
                },
                drain(durationMs) {
                    // Collect everything that arrives during durationMs.
                    return new Promise((res) => {
                        const collected = [];
                        const onMsg = (buf, rinfo) => collected.push({ buf, rinfo });
                        sock.on('message', onMsg);
                        // Also flush queue.
                        while (queue.length > 0) collected.push(queue.shift());
                        setTimeout(() => {
                            sock.removeListener('message', onMsg);
                            res(collected);
                        }, durationMs);
                    });
                },
                close() {
                    return new Promise((res) => sock.close(res));
                },
            });
        });
    });
}

function getBoundPort(handle) {
    return new Promise((res) => {
        const tryGet = () => {
            try {
                const a = handle.socket.address();
                if (a && a.port) {
                    res(a.port);
                    return;
                }
            } catch (_) {
                // not listening yet
            }
            setTimeout(tryGet, 5);
        };
        tryGet();
    });
}

// --- Tests ------------------------------------------------------------------

async function testRoundTripRegister(serverPort) {
    const sessionKey = crypto.randomBytes(16);
    const a = await makeClient();
    const b = await makeClient();
    try {
        await a.send(makeRegister(sessionKey, a.port), serverPort);
        const aReply = await a.recv(500);
        const aDec = decodeDeliver(aReply.buf);
        assert(aDec.sessionKey.equals(sessionKey), 'A DELIVER session_key matches');
        // B has not registered yet — peer should be zeroed.
        assertEq(aDec.peerIp, '0.0.0.0', 'A first DELIVER peerIp zero');
        assertEq(aDec.peerPort, 0, 'A first DELIVER peerPort zero');

        await b.send(makeRegister(sessionKey, b.port), serverPort);
        // Both A (unsolicited push) and B (direct reply) should now receive a DELIVER pointing at the other.
        const bReply = await b.recv(500);
        const bDec = decodeDeliver(bReply.buf);
        assert(bDec.sessionKey.equals(sessionKey), 'B DELIVER session_key matches');
        assertEq(bDec.peerIp, '127.0.0.1', 'B DELIVER peerIp');
        assertEq(bDec.peerPort, a.port, 'B DELIVER peerPort matches A bound port');

        const aPush = await a.recv(500);
        const aPushDec = decodeDeliver(aPush.buf);
        assertEq(aPushDec.peerIp, '127.0.0.1', 'A unsolicited DELIVER peerIp');
        assertEq(aPushDec.peerPort, b.port, 'A unsolicited DELIVER peerPort matches B bound port');
    } finally {
        await a.close();
        await b.close();
    }
}

async function testMagicReject(serverPort) {
    const sessionKey = crypto.randomBytes(16);
    const c = await makeClient();
    try {
        const bad = makeRegister(sessionKey, c.port, { magic: 0xdeadbeef });
        await c.send(bad, serverPort);
        const reply = await c.tryRecv(200);
        assert(reply === null, 'bad magic produces no reply');
    } finally {
        await c.close();
    }
}

async function testVersionReject(serverPort) {
    const sessionKey = crypto.randomBytes(16);
    const c = await makeClient();
    try {
        const bad = makeRegister(sessionKey, c.port, { version: 2 });
        await c.send(bad, serverPort);
        const reply = await c.tryRecv(200);
        assert(reply === null, 'bad version produces no reply');
    } finally {
        await c.close();
    }
}

async function testLengthReject(serverPort) {
    const sessionKey = crypto.randomBytes(16);
    const c = await makeClient();
    try {
        const bad = makeRegister(sessionKey, c.port, { length: 16 });
        await c.send(bad, serverPort);
        const reply = await c.tryRecv(200);
        assert(reply === null, 'truncated REGISTER produces no reply');
    } finally {
        await c.close();
    }
}

async function testPollAfterRegister(serverPort) {
    const sessionKey = crypto.randomBytes(16);
    const c = await makeClient();
    try {
        await c.send(makeRegister(sessionKey, c.port), serverPort);
        const r1 = await c.recv(500);
        const d1 = decodeDeliver(r1.buf);
        assert(d1.sessionKey.equals(sessionKey), 'POLL test: REGISTER reply key matches');

        await c.send(makePoll(sessionKey), serverPort);
        const r2 = await c.recv(500);
        const d2 = decodeDeliver(r2.buf);
        assert(d2.sessionKey.equals(sessionKey), 'POLL reply session_key matches');
        // Peer not registered — should be zeroes.
        assertEq(d2.peerIp, '0.0.0.0', 'POLL reply peerIp zero (no peer yet)');
        assertEq(d2.peerPort, 0, 'POLL reply peerPort zero');
    } finally {
        await c.close();
    }
}

async function testRateLimit(serverPort) {
    const sessionKey = crypto.randomBytes(16);
    const c = await makeClient();
    try {
        const N = 20;
        for (let i = 0; i < N; i++) {
            await c.send(makeRegister(sessionKey, c.port), serverPort);
        }
        // Drain incoming for 300ms.
        const replies = await c.drain(300);
        // With a sliding window and a fresh state, the first RATE_LIMIT_PER_WINDOW
        // (=10) packets pass and the rest are dropped. Allow +1 slack for clock
        // jitter at the boundary.
        assert(replies.length >= 1, `rate-limit: at least 1 reply (got ${replies.length})`);
        assert(replies.length <= 11, `rate-limit: <=11 replies (got ${replies.length})`);
    } finally {
        await c.close();
    }
}

async function testSessionTtl(handle, serverPort) {
    // S1 host liveness: TTL is 10 minutes (was 60 s). Verify (a) the
    // constant, (b) a session aged past the OLD 60 s TTL survives a sweep
    // and is still pair-able, (c) a session aged past the new TTL is
    // evicted. Aging is simulated by rewinding lastTouch through the
    // _sessionMap hook — the sweep itself runs the real eviction code.
    assertEq(handle._sessionTtlMs, 10 * 60 * 1000, 'SESSION_TTL_MS is 10 minutes');

    const sessionKey = crypto.randomBytes(16);
    const hexKey = sessionKey.toString('hex');
    const a = await makeClient();
    const b = await makeClient();
    try {
        await a.send(makeRegister(sessionKey, a.port), serverPort);
        await a.recv(500); // pending DELIVER
        const entry = handle._sessionMap.get(hexKey);
        assert(entry !== undefined, 'TTL: session exists after REGISTER');

        // (b) Age 61 s — dead under the old 60 s TTL, alive under 10 min.
        entry.lastTouch -= 61 * 1000;
        handle._sweepNow();
        assert(handle._sessionMap.has(hexKey), 'TTL: session survives sweep at age 61s');

        // ...and still pair-able: B registers the same key and must get
        // A's endpoint back, and A must get the unsolicited DELIVER push.
        await b.send(makeRegister(sessionKey, b.port), serverPort);
        const bReply = decodeDeliver((await b.recv(500)).buf);
        assertEq(bReply.peerIp, '127.0.0.1', 'TTL: B paired with aged-61s host (ip)');
        assertEq(bReply.peerPort, a.port, 'TTL: B paired with aged-61s host (port)');
        const aPush = decodeDeliver((await a.recv(500)).buf);
        assertEq(aPush.peerPort, b.port, 'TTL: aged host still receives DELIVER push');

        // (c) Age past the full TTL — must be evicted by the real sweep.
        entry.lastTouch -= 10 * 60 * 1000 + 1000;
        handle._sweepNow();
        assert(!handle._sessionMap.has(hexKey), 'TTL: session evicted past 10 minutes');
    } finally {
        await a.close();
        await b.close();
    }
}

async function testSessionCap(handle, serverPort) {
    // Fill the table to MAX_SESSIONS with synthetic entries, then verify a
    // REGISTER for a brand-new key is dropped (no reply, no growth) while
    // a REGISTER touching an EXISTING session still works.
    const existingKey = crypto.randomBytes(16);
    const c = await makeClient();
    try {
        await c.send(makeRegister(existingKey, c.port), serverPort);
        await c.recv(500);

        const before = handle._sessionMap.size;
        for (let i = handle._sessionMap.size; i < handle._maxSessions; i++) {
            handle._sessionMap.set(`fake${i}`, {
                endpointA: { address: '203.0.113.1', port: 1000 + (i % 60000) },
                endpointB: null,
                lastTouch: Date.now(),
            });
        }
        assertEq(handle._sessionMap.size, handle._maxSessions, 'cap: table filled to MAX_SESSIONS');

        handle._resetRate();
        const newKey = crypto.randomBytes(16);
        await c.send(makeRegister(newKey, c.port), serverPort);
        const dropped = await c.tryRecv(200);
        assert(dropped === null, 'cap: REGISTER for new key at cap produces no reply');
        assertEq(handle._sessionMap.size, handle._maxSessions, 'cap: table did not grow past MAX_SESSIONS');

        // Existing session still serviced at cap.
        await c.send(makeRegister(existingKey, c.port), serverPort);
        const ok = await c.tryRecv(500);
        assert(ok !== null, 'cap: REGISTER for existing key at cap still replies');

        // Cleanup synthetic entries so later logic isn't affected.
        for (const k of [...handle._sessionMap.keys()]) {
            if (k.startsWith('fake')) handle._sessionMap.delete(k);
        }
        void before;
    } finally {
        await c.close();
    }
}

// Stub socket for handle._onMessage injection: captures outbound sends so
// tests can exercise source-IP-dependent policy without real routing.
function makeStubSocket() {
    const sent = [];
    return {
        sent,
        send(buf, off, len, port, address) {
            sent.push({ buf: Buffer.from(buf.subarray(off, off + len)), port, address });
        },
    };
}

async function testStaleSlotReclaim(handle, serverPort) {
    // Review H1 (server side): a REGISTER from the same IP as a STALE,
    // unpaired host slot reclaims the slot (cancel-then-re-host with a new
    // NAT source port, session key pinned by UPnP) instead of being filed
    // as "the joiner" and DELIVERed its own stale endpoint.
    const key = crypto.randomBytes(16);
    const hexKey = key.toString('hex');
    const oldHost = await makeClient();
    const newHost = await makeClient();
    const joiner = await makeClient();
    try {
        // Old host registers, then goes silent (user pressed Cancel).
        await oldHost.send(makeRegister(key, oldHost.port), serverPort);
        await oldHost.recv(500);
        const entry = handle._sessionMap.get(hexKey);
        assert(entry !== undefined, 'reclaim: session exists after REGISTER');
        assertEq(entry.endpointA.port, oldHost.port, 'reclaim: slot A is old host');

        // Age slot A past SLOT_STALE_MS (real reclaim code runs on the
        // next REGISTER — aging via the _sessionMap hook, same style as
        // testSessionTtl), then re-host from a new port.
        entry.lastSeenA -= handle._slotStaleMs + 1000;
        await newHost.send(makeRegister(key, newHost.port), serverPort);
        const r = decodeDeliver((await newHost.recv(500)).buf);
        assertEq(r.peerIp, '0.0.0.0', 'reclaim: re-host reply has NO peer (not own stale endpoint)');
        assertEq(r.peerPort, 0, 'reclaim: re-host reply peer port zero');
        assertEq(entry.endpointA.port, newHost.port, 'reclaim: slot A repointed to new host port');
        assert(entry.endpointB === null, 'reclaim: slot B still empty');

        // A joiner now pairs with the NEW endpoint, and the new host gets
        // the unsolicited DELIVER push.
        await joiner.send(makeRegister(key, joiner.port), serverPort);
        const jr = decodeDeliver((await joiner.recv(500)).buf);
        assertEq(jr.peerPort, newHost.port, 'reclaim: joiner paired with reclaimed host');
        const push = decodeDeliver((await newHost.recv(500)).buf);
        assertEq(push.peerPort, joiner.port, 'reclaim: reclaimed host received DELIVER push');
    } finally {
        await oldHost.close();
        await newHost.close();
        await joiner.close();
    }
}

async function testFreshSlotNotReclaimed(handle, serverPort) {
    // Reclaim must require staleness: a same-IP REGISTER from a new port
    // against a LIVE host slot pairs as the joiner (normal flow preserved).
    const key = crypto.randomBytes(16);
    const a = await makeClient();
    const b = await makeClient();
    try {
        await a.send(makeRegister(key, a.port), serverPort);
        await a.recv(500);
        await b.send(makeRegister(key, b.port), serverPort);
        const br = decodeDeliver((await b.recv(500)).buf);
        assertEq(br.peerPort, a.port, 'fresh-slot: same-IP new-port REGISTER paired as joiner');
        const entry = handle._sessionMap.get(key.toString('hex'));
        assertEq(entry.endpointA.port, a.port, 'fresh-slot: slot A untouched');
        assertEq(entry.endpointB.port, b.port, 'fresh-slot: slot B is the new client');
    } finally {
        await a.close();
        await b.close();
    }
}

async function testReclaimRequiresSameIp(handle) {
    // Injection: a stale host slot is NOT reclaimed by a different-IP
    // source — that source becomes the joiner, keeping pre-S1 silent
    // hosts pair-able exactly as before.
    const key = crypto.randomBytes(16);
    const stub = makeStubSocket();
    handle._onMessage(makeRegister(key, 1111), { address: '198.51.100.7', port: 1111 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));
    entry.lastSeenA -= handle._slotStaleMs + 1000;
    handle._onMessage(makeRegister(key, 2222), { address: '198.51.100.8', port: 2222 }, stub);
    assertEq(entry.endpointA.address, '198.51.100.7', 'same-ip-required: stale A kept for different-IP source');
    assertEq(entry.endpointB.address, '198.51.100.8', 'same-ip-required: different IP paired as joiner');
    // 3 sends total: A's initial reply, B's pairing reply, A's push.
    assertEq(stub.sent.length, 3, 'same-ip-required: reply+reply+push emitted');
}

async function testPoisonedKeyBothSlotsStale(handle) {
    // Review H1 variant: both slots stale (abandoned pairing). A same-IP
    // re-host REGISTER must reclaim slot A AND clear the dead joiner slot
    // instead of being dropped as a third party for the whole TTL.
    const key = crypto.randomBytes(16);
    const stub = makeStubSocket();
    handle._onMessage(makeRegister(key, 1111), { address: '198.51.100.17', port: 1111 }, stub);
    handle._onMessage(makeRegister(key, 2222), { address: '198.51.100.19', port: 2222 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));
    entry.lastSeenA -= handle._slotStaleMs + 1000;
    entry.lastSeenB -= handle._slotStaleMs + 1000;
    stub.sent.length = 0;
    handle._onMessage(makeRegister(key, 3333), { address: '198.51.100.17', port: 3333 }, stub);
    assertEq(entry.endpointA.port, 3333, 'poisoned-key: slot A reclaimed by same-IP re-host');
    assert(entry.endpointB === null, 'poisoned-key: stale joiner slot cleared');
    assertEq(stub.sent.length, 1, 'poisoned-key: re-host got a reply (not third-party drop)');
    const d = decodeDeliver(stub.sent[0].buf);
    assertEq(d.peerIp, '0.0.0.0', 'poisoned-key: reply carries no peer');
}

async function testStaleJoinerSlotReplaced(handle) {
    // Full entry, live host, stale joiner: a fresh joiner (any IP) replaces
    // slot B and the host is re-notified with the new endpoint.
    const key = crypto.randomBytes(16);
    const stub = makeStubSocket();
    handle._onMessage(makeRegister(key, 1111), { address: '198.51.100.27', port: 1111 }, stub);
    handle._onMessage(makeRegister(key, 2222), { address: '198.51.100.29', port: 2222 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));
    entry.lastSeenB -= handle._slotStaleMs + 1000;
    stub.sent.length = 0;
    handle._onMessage(makeRegister(key, 4444), { address: '198.51.100.30', port: 4444 }, stub);
    assertEq(entry.endpointB.address, '198.51.100.30', 'stale-joiner: slot B replaced');
    assertEq(entry.endpointA.address, '198.51.100.27', 'stale-joiner: live host slot untouched');
    // Reply to the new joiner (carrying A) + re-notify push to A (carrying new B).
    assertEq(stub.sent.length, 2, 'stale-joiner: reply + host push emitted');
    const toJoiner = stub.sent.find((s) => s.address === '198.51.100.30');
    const toHost = stub.sent.find((s) => s.address === '198.51.100.27');
    assert(toJoiner && decodeDeliver(toJoiner.buf).peerPort === 1111, 'stale-joiner: joiner told host endpoint');
    assert(toHost && decodeDeliver(toHost.buf).peerPort === 4444, 'stale-joiner: host told NEW joiner endpoint');
    // Live joiner control: fresh slot B must NOT be replaceable.
    stub.sent.length = 0;
    handle._onMessage(makeRegister(key, 5555), { address: '198.51.100.31', port: 5555 }, stub);
    assertEq(entry.endpointB.address, '198.51.100.30', 'stale-joiner: LIVE slot B not replaced by third party');
    assertEq(stub.sent.length, 0, 'stale-joiner: third party got no reply');
}

async function testSweepHook(handle) {
    // The sweep hook exists and is callable. We don't try to time-warp; just
    // assert calling it doesn't throw and doesn't crash the server.
    assert(typeof handle._sweepNow === 'function', 'server exposes _sweepNow hook');
    handle._sweepNow();
    assert(handle._sessionMap instanceof Map, 'server exposes _sessionMap');
    assert(handle._rateMap instanceof Map, 'server exposes _rateMap');
}

// --- Main -------------------------------------------------------------------

async function main() {
    // Silence info/warn logs from the server during tests so output is clean.
    const origLog = console.log;
    const origWarn = console.warn;
    console.log = () => {};
    console.warn = () => {};

    const handle = start(0);
    const serverPort = await getBoundPort(handle);

    let exitCode = 0;
    try {
        // Tests that touch the rate-limit budget share the 127.0.0.1 source IP,
        // so we explicitly clear the rate state between tests via the
        // _resetRate() hook rather than waiting on wall-clock windows.
        await testRoundTripRegister(serverPort);   // 4 packets
        handle._resetRate();
        await testMagicReject(serverPort);         // 1 packet
        await testVersionReject(serverPort);       // 1 packet
        await testLengthReject(serverPort);        // 1 packet
        await testPollAfterRegister(serverPort);   // 2 packets
        handle._resetRate();
        await testRateLimit(serverPort);
        handle._resetRate();
        await testSessionTtl(handle, serverPort);   // 3 packets
        handle._resetRate();
        await testSessionCap(handle, serverPort);   // 4 packets
        handle._resetRate();
        await testStaleSlotReclaim(handle, serverPort); // 4 packets
        handle._resetRate();
        await testFreshSlotNotReclaimed(handle, serverPort); // 2 packets
        handle._resetRate();
        await testReclaimRequiresSameIp(handle);
        await testPoisonedKeyBothSlotsStale(handle);
        await testStaleJoinerSlotReplaced(handle);
        handle._resetRate();
        await testSweepHook(handle);
    } catch (err) {
        console.error(`UNCAUGHT: ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    } finally {
        console.log = origLog;
        console.warn = origWarn;
    }

    if (failed > 0) {
        console.error(`${failed} assertion(s) failed`);
        exitCode = 1;
    }

    // Clean up server.
    try { handle._shutdown && handle._shutdown('test-end'); } catch (_) {}

    if (exitCode === 0) {
        console.log('protocol test passed');
    }
    // Force exit so dangling timers don't keep the test alive past the budget.
    setTimeout(() => process.exit(exitCode), 50).unref();
}

main();
