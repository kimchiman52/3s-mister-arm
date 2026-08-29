// Self-contained protocol test for rendezvous-server.js.
// Boots the server in-process on an ephemeral port and drives it with mock
// UDP clients. Wire-format encode logic is duplicated here on purpose so the
// test catches encoding bugs in either side.
//
// Runtime budget: ~1.7 s, dominated by deliberate negative-wait
// timeouts (assertions of the form "no reply arrives"). Keep it under
// 2.5 s; on loopback every POSITIVE wait resolves in well under a
// millisecond, so if this file ever approaches the ceiling the cause is
// a new negative wait, not slow I/O. The map-bound tests (review
// MEDIUM-3) inject ~40k packets through handle._onMessage, but that is
// pure in-process work and costs well under 100 ms.

'use strict';

const dgram = require('dgram');
const crypto = require('crypto');
const { start } = require('./rendezvous-server.js');

// --- Wire constants (duplicate of server) -----------------------------------

const MAGIC = 0x33535852;
// S4c protocol v2: REGISTER/POLL are 36 bytes with an 8-byte return-
// routability cookie tail, and the server answers an uncookied/stale
// request with a 32-byte CHALLENGE instead of binding a slot.
const VERSION = 2;
const V1_VERSION = 1; // pre-S4c, for the mixed-version interlock test
const TYPE_REGISTER = 1;
const TYPE_DELIVER = 2;
const TYPE_POLL = 3;
const TYPE_CHALLENGE = 4;
const REGISTER_LEN = 36;
const POLL_LEN = 36;
const DELIVER_LEN = 32;
const CHALLENGE_LEN = 32;
const COOKIE_LEN = 8;
const V1_REGISTER_LEN = 28;

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
    // Cookie tail: absent -> all zeros, which the server reads as
    // "no cookie yet" and answers with a CHALLENGE.
    if (len >= 36 && opts.cookie) opts.cookie.copy(buf, 28, 0, COOKIE_LEN);
    return buf;
}

function makePoll(sessionKey, opts) {
    opts = opts || {};
    const buf = Buffer.alloc(POLL_LEN);
    buf.writeUInt32BE(MAGIC, 0);
    buf.writeUInt8(opts.version !== undefined ? opts.version : VERSION, 4);
    buf.writeUInt8(TYPE_POLL, 5);
    buf.writeUInt16BE(0, 6);
    sessionKey.copy(buf, 8, 0, 16);
    buf.writeUInt32BE(0, 24);
    if (opts.cookie) opts.cookie.copy(buf, 28, 0, COOKIE_LEN);
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

// Assertion wrappers over a captured send. These tolerate "nothing was
// sent at all", which is exactly what a broken/neutralized server does —
// indexing stub.sent[0].buf directly would throw and abort the rest of
// main()'s test list instead of reporting a clean failure.
function assertIsChallenge(sent, msg) {
    if (!sent) {
        assert(false, `${msg} (no packet emitted at all)`);
        return;
    }
    assert(isChallenge(sent.buf), msg);
}

function assertNotChallenge(sent, msg) {
    if (!sent) {
        assert(false, `${msg} (no packet emitted at all)`);
        return;
    }
    assert(!isChallenge(sent.buf), msg);
}

function isChallenge(buf) {
    return buf.length === CHALLENGE_LEN &&
        buf.readUInt32BE(0) === MAGIC &&
        buf.readUInt8(4) === VERSION &&
        buf.readUInt8(5) === TYPE_CHALLENGE;
}

function decodeChallenge(buf) {
    if (!isChallenge(buf)) throw new Error(`not a CHALLENGE (len=${buf.length} ver=${buf.length >= 5 ? buf.readUInt8(4) : '?'} type=${buf.length >= 6 ? buf.readUInt8(5) : '?'})`);
    return {
        sessionKey: Buffer.from(buf.subarray(8, 24)),
        cookie: Buffer.from(buf.subarray(24, 24 + COOKIE_LEN)),
    };
}

// --- S4c cookie plumbing -----------------------------------------------------
//
// Most pre-S4c tests only care about pairing/slot policy, not about the
// challenge round-trip, so they mint the cookie the server would have
// issued via the _cookieFor hook and send ONE cookied REGISTER. That
// keeps their packet counts (and therefore their per-IP rate budgets)
// identical to the v1 versions of these tests. The genuine two-packet
// challenge->echo exchange over a real socket is covered separately by
// testCookieChallengeRequired.
//
// Methodology note (inherited): both peers in the socket-based tests
// register from 127.0.0.1, so same-IP pairing is this file's normal
// case; source-address-dependent policy is driven through
// handle._onMessage injection instead.
let H = null; // server handle, set in main()

function cookieFor(address, port, slotOffset) {
    return H._cookieFor(address, port, slotOffset);
}

// Cookied REGISTER for an injected source address.
function regFrom(sessionKey, myPublicPort, address, port, opts) {
    return makeRegister(sessionKey, myPublicPort,
        Object.assign({ cookie: cookieFor(address, port) }, opts || {}));
}

// Cookied REGISTER for a real loopback client socket.
function regLocal(sessionKey, client, opts) {
    return regFrom(sessionKey, client.port, '127.0.0.1', client.port, opts);
}

function pollLocal(sessionKey, client, opts) {
    return makePoll(sessionKey,
        Object.assign({ cookie: cookieFor('127.0.0.1', client.port) }, opts || {}));
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
        await a.send(regLocal(sessionKey, a), serverPort);
        const aReply = await a.recv(500);
        const aDec = decodeDeliver(aReply.buf);
        assert(aDec.sessionKey.equals(sessionKey), 'A DELIVER session_key matches');
        // B has not registered yet — peer should be zeroed.
        assertEq(aDec.peerIp, '0.0.0.0', 'A first DELIVER peerIp zero');
        assertEq(aDec.peerPort, 0, 'A first DELIVER peerPort zero');

        await b.send(regLocal(sessionKey, b), serverPort);
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
        const bad = regLocal(sessionKey, c, { magic: 0xdeadbeef });
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
        // A FUTURE version (v3) must be dropped, not guessed at.
        const bad = regLocal(sessionKey, c, { version: 3 });
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
        const bad = regLocal(sessionKey, c, { length: 16 });
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
        await c.send(regLocal(sessionKey, c), serverPort);
        const r1 = await c.recv(500);
        const d1 = decodeDeliver(r1.buf);
        assert(d1.sessionKey.equals(sessionKey), 'POLL test: REGISTER reply key matches');

        await c.send(pollLocal(sessionKey, c), serverPort);
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
            await c.send(regLocal(sessionKey, c), serverPort);
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
        await a.send(regLocal(sessionKey, a), serverPort);
        await a.recv(500); // pending DELIVER
        const entry = handle._sessionMap.get(hexKey);
        assert(entry !== undefined, 'TTL: session exists after REGISTER');

        // (b) Age 61 s — dead under the old 60 s TTL, alive under 10 min.
        entry.lastTouch -= 61 * 1000;
        handle._sweepNow();
        assert(handle._sessionMap.has(hexKey), 'TTL: session survives sweep at age 61s');

        // ...and still pair-able: B registers the same key and must get
        // A's endpoint back, and A must get the unsolicited DELIVER push.
        await b.send(regLocal(sessionKey, b), serverPort);
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
    // Review H2 cap policy: at MAX_SESSIONS, a REGISTER for a brand-new key
    // EVICTS the oldest UNPAIRED singleton and is admitted (a flood of
    // squatted singletons can no longer lock out a legitimate new host).
    // Only a table full of PAIRED sessions drops the new key. Existing
    // sessions are always still serviced at cap.
    handle._resetSessions(); // isolate from keys created by earlier tests
    const existingKey = crypto.randomBytes(16);
    const c = await makeClient();
    try {
        await c.send(regLocal(existingKey, c), serverPort);
        await c.recv(500);

        // Fill with synthetic UNPAIRED singletons. lastTouch counts up from
        // 1 (performance.now() epoch) so the fakes are strictly OLDER than
        // the real entry and fake0 is the oldest of all.
        for (let i = handle._sessionMap.size; i < handle._maxSessions; i++) {
            handle._sessionMap.set(`fake${i}`, {
                endpointA: { address: '203.0.113.1', port: 1000 + (i % 60000) },
                endpointB: null,
                lastTouch: 1 + i,
                lastSeenA: 1 + i,
                lastSeenB: 0,
            });
        }
        assertEq(handle._sessionMap.size, handle._maxSessions, 'cap: table filled to MAX_SESSIONS');
        assert(handle._sessionMap.has('fake1'), 'cap: oldest singleton present before new-key REGISTER');

        handle._resetRate();
        const newKey = crypto.randomBytes(16);
        await c.send(regLocal(newKey, c), serverPort);
        const admitted = await c.tryRecv(500);
        assert(admitted !== null, 'cap: new key at cap ADMITTED via singleton eviction (got a reply)');
        assertEq(handle._sessionMap.size, handle._maxSessions, 'cap: table did not grow past MAX_SESSIONS');
        assert(!handle._sessionMap.has('fake1'), 'cap: oldest unpaired singleton was the one evicted');
        assert(handle._sessionMap.has(newKey.toString('hex')), 'cap: new key present after eviction');

        // Existing session still serviced at cap.
        await c.send(regLocal(existingKey, c), serverPort);
        const ok = await c.tryRecv(500);
        assert(ok !== null, 'cap: REGISTER for existing key at cap still replies');

        // All-PAIRED variant: with no unpaired singleton to evict, a new
        // key is genuinely dropped — paired sessions are never evicted.
        for (const [k, e] of handle._sessionMap) {
            if (e.endpointB === null) e.endpointB = { address: '203.0.113.2', port: 2000 };
            void k;
        }
        handle._resetRate();
        const blockedKey = crypto.randomBytes(16);
        // Use a fresh source IP via injection so the per-IP quota (127.0.0.1
        // already owns keys here) is not the reason for the drop.
        const stub = makeStubSocket();
        handle._onMessage(regFrom(blockedKey, 3333, '198.51.100.99', 3333), { address: '198.51.100.99', port: 3333 }, stub);
        assertEq(stub.sent.length, 0, 'cap: new key dropped when table is all paired (no reply)');
        assert(!handle._sessionMap.has(blockedKey.toString('hex')), 'cap: paired sessions were not evicted');

        handle._resetSessions(); // drop synthetic state for later tests
    } finally {
        await c.close();
    }
}

async function testPerIpQuota(handle, serverPort) {
    // Review H2 defense 1: one source IP may hold at most MAX_NEW_KEYS_PER_IP
    // live keys it created. The quota frees up when a key is released (TTL
    // sweep here), so a legitimate client is never permanently locked out.
    handle._resetSessions();
    const quota = handle._maxNewKeysPerIp;
    const c = await makeClient();
    try {
        const keys = [];
        for (let i = 0; i < quota; i++) {
            const k = crypto.randomBytes(16);
            keys.push(k);
            await c.send(regLocal(k, c), serverPort);
            const r = await c.tryRecv(500);
            assert(r !== null, `quota: key ${i + 1}/${quota} admitted`);
        }
        assertEq(handle._sessionMap.size, quota, 'quota: table holds exactly the quota');

        const overKey = crypto.randomBytes(16);
        await c.send(regLocal(overKey, c), serverPort);
        const over = await c.tryRecv(200);
        assert(over === null, 'quota: key over quota dropped (no reply)');
        assertEq(handle._sessionMap.size, quota, 'quota: table did not grow');

        // Existing keys still serviced while at quota.
        await c.send(regLocal(keys[0], c), serverPort);
        assert((await c.tryRecv(500)) !== null, 'quota: existing key still serviced at quota');

        // Release one key via the real TTL sweep; the quota must free up.
        const e0 = handle._sessionMap.get(keys[0].toString('hex'));
        e0.lastTouch -= handle._sessionTtlMs + 1000;
        handle._sweepNow();
        handle._resetRate();
        await c.send(regLocal(overKey, c), serverPort);
        assert((await c.tryRecv(500)) !== null, 'quota: freed by sweep — new key admitted again');
        handle._resetSessions(); // don't let this test's keys count against later tests
    } finally {
        await c.close();
    }
}

async function testSpoofedFloodEviction(handle) {
    // Review H2 end-to-end. NOTE the S4c change of premise: pre-S4c this
    // flood was source-SPOOFED, which bypassed the per-IP limiter and the
    // per-IP quota for free. Post-S4c a spoofed source cannot bind at all
    // (testSpoofedSourceCannotBind proves it), so the surviving threat is
    // a real BOTNET whose nodes genuinely receive at their own addresses
    // and therefore pass the cookie gate — modelled here by minting each
    // node's own valid cookie. The H2 eviction policy must still hold
    // against that: a legitimate new host is admitted (evicting the
    // oldest flood singleton), and a LIVE host that keeps re-REGISTERing
    // is never the eviction victim while the flood continues.
    handle._resetSessions();
    const stub = makeStubSocket();
    const max = handle._maxSessions;
    let firstFloodHex = null;
    for (let i = 0; i < max; i++) {
        const k = crypto.randomBytes(16);
        if (i === 0) firstFloodHex = k.toString('hex');
        const addr = `10.${(i >> 16) & 255}.${(i >> 8) & 255}.${i & 255}`;
        const port = 1024 + (i % 60000);
        handle._onMessage(regFrom(k, port, addr, port), { address: addr, port }, stub);
    }
    assertEq(handle._sessionMap.size, max, 'flood: cookie-capable botnet flood filled the table');

    // Legitimate new host (fresh IP) registers: admitted, oldest flood key evicted.
    stub.sent.length = 0;
    const hostKey = crypto.randomBytes(16);
    handle._onMessage(regFrom(hostKey, 7777, '192.0.2.10', 7777), { address: '192.0.2.10', port: 7777 }, stub);
    assertEq(stub.sent.length, 1, 'flood: legit host got a reply at cap');
    assert(handle._sessionMap.has(hostKey.toString('hex')), 'flood: legit host key admitted');
    assert(!handle._sessionMap.has(firstFloodHex), 'flood: oldest flood singleton evicted');
    assertEq(handle._sessionMap.size, max, 'flood: size still at cap');

    // Flood continues; the live host re-REGISTERs (S1 cadence) between
    // waves and must survive every eviction round.
    for (let wave = 0; wave < 5; wave++) {
        for (let i = 0; i < 50; i++) {
            const k = crypto.randomBytes(16);
            handle._onMessage(regFrom(k, 5000 + i, `172.16.${wave}.${i + 1}`, 5000 + i), { address: `172.16.${wave}.${i + 1}`, port: 5000 + i }, stub);
        }
        handle._onMessage(regFrom(hostKey, 7777, '192.0.2.10', 7777), { address: '192.0.2.10', port: 7777 }, stub);
    }
    assert(handle._sessionMap.has(hostKey.toString('hex')), 'flood: live re-REGISTERing host never evicted');
    handle._resetSessions();
    handle._resetRate();
}

async function testRehostWithinStaleWindow(handle, serverPort) {
    // Review H1 gap: a re-host WITHIN the 30 s staleness window (new NAT
    // port) gets filed as B while old-A is still fresh. Its own periodic
    // re-REGISTERs then only refresh B — so once old-A crosses the
    // threshold, the next re-REGISTER must PROMOTE the client from B to A
    // (clearing B) or a real joiner would be dropped as a third party
    // forever.
    const key = crypto.randomBytes(16);
    const hexKey = key.toString('hex');
    const oldHost = await makeClient();
    const newHost = await makeClient();
    const joiner = await makeClient();
    try {
        await oldHost.send(regLocal(key, oldHost), serverPort);
        await oldHost.recv(500);
        // Immediate re-host (old slot NOT yet stale) -> filed as joiner (B).
        await newHost.send(regLocal(key, newHost), serverPort);
        await newHost.recv(500); // DELIVER carrying old-A (self IP; client ignores it)
        const entry = handle._sessionMap.get(hexKey);
        assertEq(entry.endpointB.port, newHost.port, 'rehost-window: new host filed as B while A fresh');

        // Old slot crosses the staleness threshold; the S1 resender's next
        // periodic re-REGISTER (matching B exactly) must promote B->A.
        entry.lastSeenA -= handle._slotStaleMs + 1000;
        await newHost.send(regLocal(key, newHost), serverPort);
        await newHost.recv(500);
        assertEq(entry.endpointA.port, newHost.port, 'rehost-window: re-REGISTER promoted B to host slot');
        assert(entry.endpointB === null, 'rehost-window: joiner slot freed by promotion');

        // A real joiner can now pair with the re-hosted client.
        await joiner.send(regLocal(key, joiner), serverPort);
        const jr = decodeDeliver((await joiner.recv(500)).buf);
        assertEq(jr.peerPort, newHost.port, 'rehost-window: joiner paired with promoted host');
        const push = decodeDeliver((await newHost.recv(500)).buf);
        assertEq(push.peerPort, joiner.port, 'rehost-window: promoted host got DELIVER push');
    } finally {
        await oldHost.close();
        await newHost.close();
        await joiner.close();
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
        await oldHost.send(regLocal(key, oldHost), serverPort);
        await oldHost.recv(500);
        const entry = handle._sessionMap.get(hexKey);
        assert(entry !== undefined, 'reclaim: session exists after REGISTER');
        assertEq(entry.endpointA.port, oldHost.port, 'reclaim: slot A is old host');

        // Age slot A past SLOT_STALE_MS (real reclaim code runs on the
        // next REGISTER — aging via the _sessionMap hook, same style as
        // testSessionTtl), then re-host from a new port.
        entry.lastSeenA -= handle._slotStaleMs + 1000;
        await newHost.send(regLocal(key, newHost), serverPort);
        const r = decodeDeliver((await newHost.recv(500)).buf);
        assertEq(r.peerIp, '0.0.0.0', 'reclaim: re-host reply has NO peer (not own stale endpoint)');
        assertEq(r.peerPort, 0, 'reclaim: re-host reply peer port zero');
        assertEq(entry.endpointA.port, newHost.port, 'reclaim: slot A repointed to new host port');
        assert(entry.endpointB === null, 'reclaim: slot B still empty');

        // A joiner now pairs with the NEW endpoint, and the new host gets
        // the unsolicited DELIVER push.
        await joiner.send(regLocal(key, joiner), serverPort);
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
        await a.send(regLocal(key, a), serverPort);
        await a.recv(500);
        await b.send(regLocal(key, b), serverPort);
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
    handle._onMessage(regFrom(key, 1111, '198.51.100.7', 1111), { address: '198.51.100.7', port: 1111 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));
    entry.lastSeenA -= handle._slotStaleMs + 1000;
    handle._onMessage(regFrom(key, 2222, '198.51.100.8', 2222), { address: '198.51.100.8', port: 2222 }, stub);
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
    handle._onMessage(regFrom(key, 1111, '198.51.100.17', 1111), { address: '198.51.100.17', port: 1111 }, stub);
    handle._onMessage(regFrom(key, 2222, '198.51.100.19', 2222), { address: '198.51.100.19', port: 2222 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));
    entry.lastSeenA -= handle._slotStaleMs + 1000;
    entry.lastSeenB -= handle._slotStaleMs + 1000;
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 3333, '198.51.100.17', 3333), { address: '198.51.100.17', port: 3333 }, stub);
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
    handle._onMessage(regFrom(key, 1111, '198.51.100.27', 1111), { address: '198.51.100.27', port: 1111 }, stub);
    handle._onMessage(regFrom(key, 2222, '198.51.100.29', 2222), { address: '198.51.100.29', port: 2222 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));
    entry.lastSeenB -= handle._slotStaleMs + 1000;
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 4444, '198.51.100.30', 4444), { address: '198.51.100.30', port: 4444 }, stub);
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
    handle._onMessage(regFrom(key, 5555, '198.51.100.31', 5555), { address: '198.51.100.31', port: 5555 }, stub);
    assertEq(entry.endpointB.address, '198.51.100.30', 'stale-joiner: LIVE slot B not replaced by third party');
    assertEq(stub.sent.length, 0, 'stale-joiner: third party got no reply');
}

// --- S4c: return-routability (challenge cookie) ------------------------------

async function testCookieChallengeRequired(handle, serverPort) {
    // THE core S4c property, over a real socket, with the real two-packet
    // exchange (no _cookieFor shortcut).
    //
    // Pre-S4c the server answered the very first REGISTER with a DELIVER
    // and bound a slot on the spot — no proof the sender could receive
    // anything. Post-S4c the first REGISTER gets a CHALLENGE and binds
    // NOTHING; only the cookie echo binds.
    handle._resetSessions();
    handle._resetRate();
    const sessionKey = crypto.randomBytes(16);
    const hexKey = sessionKey.toString('hex');
    const c = await makeClient();
    try {
        // (1) Uncookied REGISTER -> CHALLENGE, and NO state.
        await c.send(makeRegister(sessionKey, c.port), serverPort);
        const first = await c.recv(500);
        assert(isChallenge(first.buf), 'cookie: first (uncookied) REGISTER answered with a CHALLENGE, not a DELIVER');
        assert(!handle._sessionMap.has(hexKey), 'cookie: uncookied REGISTER bound NO session slot');
        assertEq(handle._creatorCounts.size, 0, 'cookie: uncookied REGISTER consumed no per-IP key quota');

        if (!isChallenge(first.buf)) {
            // Bail out cleanly rather than letting decodeChallenge throw:
            // an exception here would abort main()'s whole test list and
            // hide every LATER regression behind one stack trace.
            return;
        }
        const ch = decodeChallenge(first.buf);
        assert(ch.sessionKey.equals(sessionKey), 'cookie: CHALLENGE echoes our session key');
        assertEq(ch.cookie.length, COOKIE_LEN, 'cookie: CHALLENGE carries an 8-byte cookie');
        assert(!ch.cookie.every((b) => b === 0), 'cookie: challenge cookie is not all-zero');
        // Attenuator, not reflector: reply must be no larger than the request.
        assert(CHALLENGE_LEN <= REGISTER_LEN, `cookie: CHALLENGE (${CHALLENGE_LEN}B) <= REGISTER (${REGISTER_LEN}B) — amplification factor <= 1`);

        // (2) Repeating the uncookied REGISTER still binds nothing — an
        // attacker cannot wear the gate down by retrying.
        await c.send(makeRegister(sessionKey, c.port), serverPort);
        assert(isChallenge((await c.recv(500)).buf), 'cookie: repeat uncookied REGISTER re-CHALLENGEd');
        assert(!handle._sessionMap.has(hexKey), 'cookie: repeat uncookied REGISTER still bound no slot');

        // (3) Echo the cookie -> DELIVER, and the slot binds.
        await c.send(makeRegister(sessionKey, c.port, { cookie: ch.cookie }), serverPort);
        const second = await c.recv(500);
        assert(!isChallenge(second.buf), 'cookie: cookie echo answered with a DELIVER, not another CHALLENGE');
        const d = decodeDeliver(second.buf);
        assert(d.sessionKey.equals(sessionKey), 'cookie: DELIVER after echo carries our key');
        assert(handle._sessionMap.has(hexKey), 'cookie: cookie echo BOUND the session slot');
        assertEq(handle._sessionMap.get(hexKey).endpointA.port, c.port, 'cookie: bound slot A is our endpoint');

        // (4) A POLL is gated identically.
        handle._resetRate();
        await c.send(makePoll(sessionKey), serverPort);
        assert(isChallenge((await c.recv(500)).buf), 'cookie: uncookied POLL answered with a CHALLENGE');
        await c.send(makePoll(sessionKey, { cookie: ch.cookie }), serverPort);
        assert(!isChallenge((await c.recv(500)).buf), 'cookie: cookied POLL answered with a DELIVER');
    } finally {
        await c.close();
        handle._resetSessions();
        handle._resetRate();
    }
}

async function testCookieBoundToSource(handle) {
    // The cookie is bound to (address, port): a cookie minted for one
    // endpoint must not validate from any other. This is what makes the
    // echo a proof of RECEIPT rather than a shared password.
    handle._resetSessions();
    handle._resetRate();
    const key = crypto.randomBytes(16);
    const hexKey = key.toString('hex');
    const stub = makeStubSocket();

    // Right address, WRONG port.
    handle._onMessage(makeRegister(key, 5000, { cookie: cookieFor('198.51.100.5', 5000) }),
        { address: '198.51.100.5', port: 5001 }, stub);
    assertEq(stub.sent.length, 1, 'cookie-bind: wrong-port cookie got exactly one reply');
    assertIsChallenge(stub.sent[0], 'cookie-bind: wrong-port cookie re-CHALLENGEd');
    assert(!handle._sessionMap.has(hexKey), 'cookie-bind: wrong-port cookie bound nothing');

    // Right port, WRONG address.
    stub.sent.length = 0;
    handle._onMessage(makeRegister(key, 5000, { cookie: cookieFor('198.51.100.5', 5000) }),
        { address: '198.51.100.6', port: 5000 }, stub);
    assertIsChallenge(stub.sent[0], 'cookie-bind: wrong-address cookie re-CHALLENGEd');
    assert(!handle._sessionMap.has(hexKey), 'cookie-bind: wrong-address cookie bound nothing');

    // Garbage cookie.
    stub.sent.length = 0;
    handle._onMessage(makeRegister(key, 5000, { cookie: crypto.randomBytes(COOKIE_LEN) }),
        { address: '198.51.100.5', port: 5000 }, stub);
    assertIsChallenge(stub.sent[0], 'cookie-bind: forged cookie re-CHALLENGEd');
    assert(!handle._sessionMap.has(hexKey), 'cookie-bind: forged cookie bound nothing');

    // Matching endpoint -> binds.
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 5000, '198.51.100.5', 5000), { address: '198.51.100.5', port: 5000 }, stub);
    assertNotChallenge(stub.sent[0], 'cookie-bind: matching cookie got a DELIVER');
    assert(handle._sessionMap.has(hexKey), 'cookie-bind: matching cookie bound the slot');
    handle._resetSessions();
    handle._resetRate();
}

async function testSpoofedSourceCannotBind(handle) {
    // The attack S4c exists to stop. An attacker at ATT can obtain a
    // perfectly valid cookie for ITS OWN address (it does receive there).
    // It then source-spoofs VICTIM to (a) squat the victim's session key,
    // (b) steer the victim's punch traffic, or (c) use us as a reflector.
    // All three must fail: the CHALLENGE goes to the VICTIM (so the
    // attacker never learns the victim's cookie), nothing binds, and the
    // reflected packet is smaller than the packet that caused it.
    handle._resetSessions();
    handle._resetRate();
    const ATT = '203.0.113.66';
    const VICTIM = '192.0.2.77';
    const key = crypto.randomBytes(16);
    const hexKey = key.toString('hex');
    const stub = makeStubSocket();

    // Attacker legitimately completes the gate for its OWN address, to
    // prove it really does hold a valid cookie.
    handle._onMessage(regFrom(key, 4444, ATT, 4444), { address: ATT, port: 4444 }, stub);
    assert(handle._sessionMap.has(hexKey), 'spoof: attacker can bind under its OWN address (control)');
    handle._resetSessions();
    handle._resetRate();

    // Now spoof. The attacker replays the only cookie it has.
    stub.sent.length = 0;
    const spoofed = makeRegister(key, 4444, { cookie: cookieFor(ATT, 4444) });
    handle._onMessage(spoofed, { address: VICTIM, port: 4444 }, stub);
    assert(!handle._sessionMap.has(hexKey), 'spoof: source-spoofed REGISTER bound NO slot');
    assertEq(handle._creatorCounts.size, 0, 'spoof: source-spoofed REGISTER consumed no key quota');
    assertEq(stub.sent.length, 1, 'spoof: exactly one packet emitted');
    assertEq(stub.sent[0].address, VICTIM, 'spoof: the CHALLENGE went to the SPOOFED address, not the attacker');
    assertIsChallenge(stub.sent[0], 'spoof: reply was a CHALLENGE (no endpoint disclosed)');
    assert(stub.sent[0].buf.length <= spoofed.length, 'spoof: reflected bytes <= received bytes (attenuator)');

    // And spoofing repeatedly never converges on a bind.
    for (let i = 0; i < 5; i++) {
        handle._onMessage(spoofed, { address: VICTIM, port: 4444 }, stub);
    }
    assert(!handle._sessionMap.has(hexKey), 'spoof: repeated spoofing still bound nothing');

    // A spoofed POLL discloses nothing either: bind the victim's key from
    // a real endpoint first, then try to read the peer out of it by
    // spoofing that endpoint.
    handle._resetRate();
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 6000, VICTIM, 6000), { address: VICTIM, port: 6000 }, stub);
    assert(handle._sessionMap.has(hexKey), 'spoof: victim bound its own key (setup)');
    stub.sent.length = 0;
    handle._onMessage(makePoll(key, { cookie: cookieFor(ATT, 4444) }), { address: VICTIM, port: 6000 }, stub);
    assertIsChallenge(stub.sent[0], 'spoof: POLL with a foreign cookie discloses no endpoint');
    handle._resetSessions();
    handle._resetRate();
}

async function testPerKeyRateCap(handle) {
    // The per-IP bucket is bypassable by anyone with many real source
    // addresses (a botnet, or simply a /64 of cookie-capable hosts) all
    // hammering ONE session key. The per-key cap bounds the damage to a
    // single session. Every source here is distinct, so the per-IP
    // limiter never fires and only the per-key cap can be responsible.
    // The probe is a POLL, not a REGISTER, on purpose: handlePoll answers
    // EVERY gated request (with zeroes when the source is not a slot),
    // whereas REGISTER traffic for one key is mostly swallowed by the
    // two-slot "third party — ignored" rule after the second sender. With
    // REGISTERs the reply count would sit at 3 whether or not the cap
    // exists, and the test could not distinguish the two — it would pass
    // against a server with the cap ripped out.
    handle._resetSessions();
    handle._resetRate();
    const limit = handle._keyRateLimit;
    const victimKey = crypto.randomBytes(16);
    const stub = makeStubSocket();
    const shots = limit + 8;

    for (let i = 0; i < shots; i++) {
        const addr = `198.18.${(i >> 8) & 255}.${i & 255}`;
        const port = 20000 + i;
        handle._onMessage(makePoll(victimKey, { cookie: cookieFor(addr, port) }),
            { address: addr, port }, stub);
    }
    // +1 slack for a sliding-window boundary landing mid-loop.
    assert(stub.sent.length <= limit + 1,
        `per-key cap: <= ${limit + 1} replies for ONE key across ${shots} distinct source IPs (got ${stub.sent.length})`);
    assert(stub.sent.length >= 1, 'per-key cap: at least one reply got through');
    assert(handle._keyRateMap.has(victimKey.toString('hex')), 'per-key cap: the key has a rate bucket');

    // Control: the SAME traffic volume spread over distinct keys is not
    // capped at all — the limiter is per-key, not a global throttle, and
    // this is what proves the assertion above is not just measuring the
    // per-IP bucket or the slot policy.
    handle._resetSessions();
    handle._resetRate();
    stub.sent.length = 0;
    for (let i = 0; i < shots; i++) {
        const addr = `198.19.${(i >> 8) & 255}.${i & 255}`;
        const port = 30000 + i;
        handle._onMessage(makePoll(crypto.randomBytes(16), { cookie: cookieFor(addr, port) }),
            { address: addr, port }, stub);
    }
    assertEq(stub.sent.length, shots, 'per-key cap: distinct keys are NOT throttled (control)');
    handle._resetSessions();
    handle._resetRate();
}

async function testCookieRotationWindow(handle) {
    // Cookies rotate every COOKIE_ROTATE_MS; current AND previous slots
    // validate (so a cookie lives 60..120 s and a client straddling a
    // rotation is not spuriously re-challenged), older slots do not.
    assertEq(handle._cookieRotateMs, 60 * 1000, 'rotation: COOKIE_ROTATE_MS is 60 s');
    handle._resetSessions();
    handle._resetRate();
    const ADDR = '198.51.100.44';
    const PORT = 7000;
    const stub = makeStubSocket();

    // Previous slot: accepted.
    const kPrev = crypto.randomBytes(16);
    handle._onMessage(makeRegister(kPrev, PORT, { cookie: cookieFor(ADDR, PORT, -1) }),
        { address: ADDR, port: PORT }, stub);
    assert(handle._sessionMap.has(kPrev.toString('hex')), 'rotation: previous-slot cookie still accepted');

    // Two slots back: rejected.
    stub.sent.length = 0;
    const kOld = crypto.randomBytes(16);
    handle._onMessage(makeRegister(kOld, PORT, { cookie: cookieFor(ADDR, PORT, -2) }),
        { address: ADDR, port: PORT }, stub);
    assertIsChallenge(stub.sent[0], 'rotation: two-slots-old cookie re-CHALLENGEd');
    assert(!handle._sessionMap.has(kOld.toString('hex')), 'rotation: expired cookie bound nothing');

    // A FUTURE slot must not validate either (no clock-skew freebie).
    stub.sent.length = 0;
    const kFuture = crypto.randomBytes(16);
    handle._onMessage(makeRegister(kFuture, PORT, { cookie: cookieFor(ADDR, PORT, 1) }),
        { address: ADDR, port: PORT }, stub);
    assertIsChallenge(stub.sent[0], 'rotation: next-slot cookie re-CHALLENGEd');
    assert(!handle._sessionMap.has(kFuture.toString('hex')), 'rotation: future cookie bound nothing');
    handle._resetSessions();
    handle._resetRate();
}

async function testV1ClientInterlock(handle, serverPort) {
    // Version interlock: a pre-S4c (v1) client sends a 28-byte REGISTER
    // with version=1. Required behavior is REJECT CLEANLY — no reply, no
    // state, and above all no hang: the server must still serve the very
    // next valid v2 exchange normally.
    handle._resetSessions();
    handle._resetRate();
    assertEq(handle._version, 2, 'interlock: server advertises protocol v2');
    assertEq(handle._registerLen, 36, 'interlock: v2 REGISTER is 36 bytes');
    const v1Key = crypto.randomBytes(16);
    const c = await makeClient();
    try {
        const v1 = makeRegister(v1Key, c.port, { version: V1_VERSION, length: V1_REGISTER_LEN });
        assertEq(v1.length, 28, 'interlock: v1 REGISTER is 28 bytes');
        await c.send(v1, serverPort);
        assert((await c.tryRecv(150)) === null, 'interlock: v1 REGISTER gets NO reply (not even a CHALLENGE)');
        assert(!handle._sessionMap.has(v1Key.toString('hex')), 'interlock: v1 REGISTER bound no session');
        assertEq(handle._creatorCounts.size, 0, 'interlock: v1 REGISTER consumed no key quota');

        // Also: a v2-length packet still carrying version=1 is dropped
        // by the same check, before the cookie gate.
        const v1Long = makeRegister(v1Key, c.port, { version: V1_VERSION });
        await c.send(v1Long, serverPort);
        assert((await c.tryRecv(150)) === null, 'interlock: v1 version byte at v2 length also dropped');
        assert(!handle._sessionMap.has(v1Key.toString('hex')), 'interlock: still no session');

        // Liveness: the server did not wedge on any of that.
        const okKey = crypto.randomBytes(16);
        await c.send(regLocal(okKey, c), serverPort);
        const r = await c.tryRecv(500);
        assert(r !== null && !isChallenge(r.buf), 'interlock: server still serves v2 clients after v1 traffic');
        assert(handle._sessionMap.has(okKey.toString('hex')), 'interlock: v2 client bound normally afterwards');
    } finally {
        await c.close();
        handle._resetSessions();
        handle._resetRate();
    }
}

// --- Review HIGH-2 / MEDIUM-3: pre-validation resource policy ---------------

async function testCookiedNotStarvedByUncookied(handle) {
    // Review HIGH-2, the attack this exists to stop. rateLimitAllow used to
    // be the FIRST statement in onMessage, keyed on rinfo.address alone and
    // run before the length/magic/version checks and before the cookie
    // gate. So an attacker who knows a host's IP — which the room code
    // still reveals — could send 10 spoofed 36-byte REGISTERs/s carrying
    // that address (~2.9 kbit/s), exhaust the VICTIM's per-IP budget, and
    // the victim's OWN correctly-cookied REGISTER would then get zero
    // replies and bind nothing. A permanent matchmaking lockout of a named
    // host for the price of a trickle, surfaced to the user as
    // RENDEZVOUS_DOWN.
    //
    // Required property: uncookied/spoofed traffic claiming source X must
    // not be able to consume anything a correctly-cookied request from X
    // needs. The two budgets are now structurally separate buckets, so
    // this is not a question of tuning a limit.
    handle._resetSessions();
    handle._resetRate();
    const VICTIM = '192.0.2.77';
    const VICTIM_PORT = 5555;
    const stub = makeStubSocket();

    // The flood: 4x the COOKIED budget, spoofing the victim's address.
    // (The attacker picks the source port; it cannot know or match the
    // victim's, and the limiter never looked at the port anyway.)
    const floodShots = handle._rateLimit * 4;
    for (let i = 0; i < floodShots; i++) {
        handle._onMessage(makeRegister(crypto.randomBytes(16), 9999),
            { address: VICTIM, port: 9999 }, stub);
    }
    assertEq(handle._sessionMap.size, 0, 'starvation: spoofed uncookied flood bound no sessions');
    assertEq(handle._creatorCounts.size, 0, 'starvation: spoofed uncookied flood consumed no key quota');
    // Structural core of the fix: uncookied traffic never reaches the
    // cookied bucket at all.
    assert(!handle._rateMap.has(VICTIM),
        'starvation: uncookied traffic did NOT touch the victim\'s COOKIED per-IP budget');
    assert(handle._preGateMap.has(VICTIM),
        'starvation: uncookied traffic was accounted to the separate pre-gate budget');

    // Now the victim itself: one correctly-cookied REGISTER from its real
    // endpoint. It must be served.
    stub.sent.length = 0;
    const key = crypto.randomBytes(16);
    handle._onMessage(regFrom(key, VICTIM_PORT, VICTIM, VICTIM_PORT),
        { address: VICTIM, port: VICTIM_PORT }, stub);
    assertEq(stub.sent.length, 1, 'starvation: cookied REGISTER from the flooded IP got exactly one reply');
    assertNotChallenge(stub.sent[0], 'starvation: cookied REGISTER got a DELIVER, not a CHALLENGE');
    assert(handle._sessionMap.has(key.toString('hex')),
        'starvation: cookied REGISTER from the flooded IP BOUND its session slot');

    // And it keeps working: the victim's whole cookied budget is intact,
    // not merely its first packet.
    stub.sent.length = 0;
    for (let i = 1; i < handle._rateLimit; i++) {
        handle._onMessage(regFrom(key, VICTIM_PORT, VICTIM, VICTIM_PORT),
            { address: VICTIM, port: VICTIM_PORT }, stub);
    }
    assertEq(stub.sent.length, handle._rateLimit - 1,
        'starvation: the rest of the victim\'s cookied budget survived the flood too');

    handle._resetSessions();
    handle._resetRate();
}

async function testPreGateBudgetBoundsChallenges(handle) {
    // The HIGH-2 fix must not be "delete the limiter". Uncookied first
    // contact keeps a budget of its own — PREGATE_LIMIT_PER_WINDOW — whose
    // job is to bound CHALLENGE egress toward a single address. The
    // CHALLENGE is 32 bytes for a 36-byte request (factor 0.89), so the
    // server is a net attenuator and this budget is about egress volume
    // and per-source work, not about amplification.
    handle._resetSessions();
    handle._resetRate();
    const limit = handle._preGateLimit;
    assert(limit > handle._rateLimit,
        `pre-gate: uncookied budget (${limit}) is larger than the cookied one (${handle._rateLimit}) — the two must not be the same bucket`);
    const SRC = '203.0.113.90';
    const stub = makeStubSocket();
    const shots = limit + 25;
    for (let i = 0; i < shots; i++) {
        handle._onMessage(makeRegister(crypto.randomBytes(16), 4000), { address: SRC, port: 4000 }, stub);
    }
    // +1 slack for a sliding-window boundary landing mid-loop.
    assert(stub.sent.length <= limit + 1,
        `pre-gate: <= ${limit + 1} CHALLENGEs emitted for ${shots} uncookied requests from one IP (got ${stub.sent.length})`);
    assert(stub.sent.length >= 1, 'pre-gate: at least one CHALLENGE got through');
    for (const s of stub.sent) {
        if (s.address !== SRC) {
            assert(false, `pre-gate: every CHALLENGE goes to the claimed source (saw ${s.address})`);
            break;
        }
    }
    assertIsChallenge(stub.sent[0], 'pre-gate: emitted packets are CHALLENGEs');

    // Control: the same volume spread over distinct sources is not
    // throttled — this is a per-source budget, not a global one, and this
    // is what proves the assertion above measured the pre-gate bucket.
    handle._resetRate();
    stub.sent.length = 0;
    for (let i = 0; i < shots; i++) {
        const addr = `198.18.${(i >> 8) & 255}.${i & 255}`;
        handle._onMessage(makeRegister(crypto.randomBytes(16), 4000), { address: addr, port: 4000 }, stub);
    }
    assertEq(stub.sent.length, shots, 'pre-gate: distinct sources are NOT throttled (control)');
    handle._resetSessions();
    handle._resetRate();
}

async function testRateMapBounded(handle) {
    // Review MEDIUM-3. rateLimitAllow used to run before ANY validation, so
    // every datagram allocated a Map entry keyed on its CLAIMED source
    // address: 1-byte junk from 5000 spoofed IPs produced 5000 rateMap
    // entries retained >= 60 s, and at 100k pps that is ~6M live
    // entries/minute — V8 heap exhaustion from traffic that never carried
    // a valid byte. Two properties are required now:
    //   (a) nothing is allocated until the frame passes magic + version +
    //       length, so pure junk costs zero entries at any rate; and
    //   (b) what does survive that filter is hard-capped with LRU
    //       eviction, so even well-formed spoofed traffic from unbounded
    //       distinct sources cannot grow the maps without limit.
    handle._resetSessions();
    handle._resetRate();
    const stub = makeStubSocket();

    // (a) Junk that never passes validation allocates nothing at all.
    const JUNK_SOURCES = 5000;
    for (let i = 0; i < JUNK_SOURCES; i++) {
        const addr = `10.${(i >> 16) & 255}.${(i >> 8) & 255}.${i & 255}`;
        handle._onMessage(Buffer.alloc(1), { address: addr, port: 1 }, stub);           // too short
        handle._onMessage(makeRegister(crypto.randomBytes(16), 1, { magic: 0xdeadbeef }),
            { address: addr, port: 2 }, stub);                                          // bad magic
        handle._onMessage(makeRegister(crypto.randomBytes(16), 1, { version: 3 }),
            { address: addr, port: 3 }, stub);                                          // bad version
        handle._onMessage(makeRegister(crypto.randomBytes(16), 1, { length: 16 }),
            { address: addr, port: 4 }, stub);                                          // bad length
    }
    assertEq(stub.sent.length, 0, 'bounded: pre-validation junk produced no replies');
    assertEq(handle._rateMap.size, 0, `bounded: ${JUNK_SOURCES} junk sources allocated NO cookied rate entries`);
    assertEq(handle._preGateMap.size, 0, `bounded: ${JUNK_SOURCES} junk sources allocated NO pre-gate entries`);
    assertEq(handle._keyRateMap.size, 0, `bounded: ${JUNK_SOURCES} junk sources allocated NO per-key entries`);

    // (b) Well-formed uncookied REGISTERs DO allocate — from more distinct
    // spoofed sources than the cap allows. The map must stop growing.
    const cap = handle._maxRateEntries;
    const sources = cap + 1500;
    let firstAddr = null;
    let lastAddr = null;
    for (let i = 0; i < sources; i++) {
        const addr = `172.${(i >> 16) & 255}.${(i >> 8) & 255}.${i & 255}`;
        if (i === 0) firstAddr = addr;
        lastAddr = addr;
        handle._onMessage(makeRegister(crypto.randomBytes(16), 4000), { address: addr, port: 4000 }, stub);
    }
    assert(handle._preGateMap.size <= cap,
        `bounded: pre-gate map stayed <= ${cap} across ${sources} distinct spoofed sources (got ${handle._preGateMap.size})`);
    assertEq(handle._rateMap.size, 0, 'bounded: spoofed uncookied traffic never allocated a COOKIED rate entry');
    // LRU, not FIFO-of-first-insert-only: the least recently seen source is
    // the one that goes, the most recent survives.
    assert(!handle._preGateMap.has(firstAddr), 'bounded: the least-recently-used source was evicted');
    assert(handle._preGateMap.has(lastAddr), 'bounded: the most recent source is retained');

    // Same treatment for the post-cookie maps: a cookie-capable botnet
    // hitting distinct keys from distinct addresses cannot grow them past
    // the cap either.
    handle._resetRate();
    stub.sent.length = 0;
    for (let i = 0; i < cap + 500; i++) {
        const addr = `100.${(i >> 16) & 255}.${(i >> 8) & 255}.${i & 255}`;
        const port = 6000;
        handle._onMessage(makePoll(crypto.randomBytes(16), { cookie: cookieFor(addr, port) }),
            { address: addr, port }, stub);
    }
    assert(handle._rateMap.size <= cap,
        `bounded: cookied per-IP map stayed <= ${cap} (got ${handle._rateMap.size})`);
    assert(handle._keyRateMap.size <= cap,
        `bounded: per-key map stayed <= ${cap} (got ${handle._keyRateMap.size})`);

    handle._resetSessions();
    handle._resetRate();
}

// --- Relay removal -----------------------------------------------------------

async function testKeyBudgetCoversLegitimateSignalling(handle) {
    // KEY_RATE_LIMIT_PER_WINDOW was raised 10 -> 40 for exactly one
    // reason: the S5 relay's RELAY_REQ rode the same S4c gate at 300 ms
    // per side (6.67/s on ONE key) and had eaten the margin down to ~1x.
    // The relay is deleted, so that traffic no longer exists and the
    // constant is back at 10. This test is what keeps the restored number
    // honest: it models the worst SECOND the SURVIVING client cadences can
    // put on one key and asserts every frame is answered.
    //
    // The cadences, from the shipped client:
    //   * joiner REGISTER resend inside the punch race: 500 ms
    //     (src/netplay/direct_p2p.c, `(now - signal_last_send) >= 500u`)
    //     -> 2/s;
    //   * host re-REGISTER worker: CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_
    //     INTERVAL_MS, default 5000 ms (src/port/config/config.c), floor
    //     1000 ms -> 1/s at the floor, 0.2/s at the default. The host does
    //     NOT also run a signalling leg inside the race (direct_p2p.c sets
    //     cfg.signal_leg = false for RACE_ROLE_HOST: the DELIVER that
    //     started that thread already proves it is paired).
    //   * one challenge-triggered immediate resend per side, at most once
    //     per cookie rotation (>= 60 s).
    // Steady worst case is 3/s; the modelled second below is that plus
    // BOTH challenge resends landing inside it.
    //
    // The two sides are injected from DISTINCT source IPs, so the per-IP
    // bucket (RATE_LIMIT_PER_WINDOW = 10) cannot be what is being
    // measured -- neither side sends more than 3 frames.
    handle._resetSessions();
    handle._resetRate();
    const key = crypto.randomBytes(16);
    const HOST = { address: '198.51.100.10', port: 5000 };
    const JOINER = { address: '203.0.113.20', port: 6000 };
    const stub = makeStubSocket();

    const hostPerSec = 1000 / 1000;   // REGISTER_INTERVAL_MS floor
    const joinerPerSec = 1000 / 500;  // race signal resend
    const steadyPerSec = hostPerSec + joinerPerSec;

    // The modelled second, in arrival order: steady traffic interleaved,
    // then one challenge-triggered resend from each side.
    const second = [HOST, JOINER, JOINER, HOST, JOINER];
    assertEq(second.length, steadyPerSec + 2,
        'key-budget: the modelled second is steady traffic plus one challenge resend per side');

    for (const who of second) {
        handle._onMessage(regFrom(key, who.port, who.address, who.port), who, stub);
    }

    // Every frame is answered with a DELIVER to its own sender, and the
    // pair produces ONE extra unsolicited push (to the host, when the
    // joiner's first REGISTER completes the pair) -- so the expected send
    // count is frames + 1.
    assertEq(stub.sent.length, second.length + 1,
        `key-budget: all ${second.length} frames of a worst legitimate second were answered (+1 pairing push); nothing silently rate-dropped`);
    for (const who of [HOST, JOINER]) {
        const mine = second.filter((w) => w === who).length;
        const got = stub.sent.filter((s) => s.address === who.address && s.port === who.port).length;
        assert(got >= mine,
            `key-budget: ${who.address} got at least its own ${mine} repl(ies) (got ${got})`);
    }
    for (const s of stub.sent) {
        assert(!isChallenge(s.buf),
            'key-budget: every answer is a DELIVER, not a re-CHALLENGE');
    }

    // The constant itself, stated so a cadence change trips here rather
    // than in the field.
    assert(handle._keyRateLimit >= 3 * steadyPerSec,
        `key-budget: KEY_RATE_LIMIT_PER_WINDOW (${handle._keyRateLimit}) keeps >= 3x margin over the surviving per-key cadence (${steadyPerSec}/s)`);
    // ...and that it was actually brought back DOWN from the relay-era
    // 40/s. An oversized per-key cap is a WEAKER bound on a key that an
    // attacker with several cookie-capable source IPs is hammering, and
    // there is no longer any legitimate traffic that needs the headroom.
    assert(handle._keyRateLimit <= 4 * steadyPerSec,
        `key-budget: ...and is not oversized for it (${handle._keyRateLimit}/s vs a ${steadyPerSec}/s peak) -- the relay rung that justified 40/s is gone`);

    handle._resetSessions();
    handle._resetRate();
}

async function testRetiredRelayTypesAreUnknown(handle) {
    // Types 5/6/7/8 were RELAY_REQ / RELAY_GRANT / RELAY_PIN /
    // RELAY_PIN_ACK. With the relay deleted they are unallocated, and the
    // requirement is that they die in onMessage's `drop: unknown type`
    // branch: no reply of ANY kind (a reply would make the server a
    // reflector for a frame nothing owns), no state bound, no crash --
    // including for a type-5 frame that is still perfectly cookied for its
    // own source, which is exactly what a stale relay-era client emits.
    handle._resetSessions();
    handle._resetRate();
    const key = crypto.randomBytes(16);
    const hexKey = key.toString('hex');
    const SRC = { address: '198.18.7.7', port: 41000 };
    const stub = makeStubSocket();

    for (const type of [5, 6, 7, 8, 9, 200]) {
        // Reset per iteration so each type is judged on its own: without
        // this, one type that DOES bind state makes every later type's
        // assertion fail too and the failure names the wrong frame.
        handle._resetSessions();
        handle._resetKeyRate();
        stub.sent.length = 0;
        // Correctly shaped AND correctly cookied for this source: the only
        // thing wrong with the frame is its type byte, so anything that
        // answers is answering on the type byte alone.
        const buf = makeRegister(key, SRC.port,
            { type, cookie: cookieFor(SRC.address, SRC.port) });
        handle._onMessage(buf, SRC, stub);
        assertEq(stub.sent.length, 0, `retired-types: type ${type} draws no reply at all`);
        assertEq(handle._sessionMap.size, 0, `retired-types: type ${type} binds no session state`);
        assert(!handle._keyRateMap.has(hexKey),
            `retired-types: type ${type} never even reaches the per-key rate bucket`);
    }

    // Control: the SAME frame with the REGISTER type byte IS answered and
    // DOES bind a session, so the loop above is measuring the type
    // dispatch and not some unrelated malformity in the probe.
    stub.sent.length = 0;
    handle._onMessage(makeRegister(key, SRC.port,
        { cookie: cookieFor(SRC.address, SRC.port) }), SRC, stub);
    assertEq(stub.sent.length, 1, 'retired-types: control -- type 1 (REGISTER) IS answered');
    assertEq(handle._sessionMap.size, 1, 'retired-types: control -- type 1 DOES bind a session');

    handle._resetSessions();
    handle._resetRate();
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

// Review HIGH-3. This file used to wrap the ENTIRE test list in one
// try/catch. The first `recv timeout` thrown by any test unwound the whole
// runner, silently skipped every test after it, and still printed a small,
// plausible-looking failure count. It was proven in review: with the
// server's version check disabled the run reported "1 assertion(s) failed"
// while everything from testLengthReject onwards — the entire S4c block AND
// testV1ClientInterlock, the test that was supposed to catch exactly that
// regression — never executed at all. A suite that cannot report the truth
// about its own coverage is worse than no suite.
//
// Three properties now hold:
//   1. Each test runs inside its OWN try/catch. A throw is recorded against
//      that test by name and the runner continues.
//   2. `testsRun` is counted and compared against a LITERAL expected count,
//      so a silently-skipped (or accidentally-unregistered) test is itself
//      a hard failure rather than an invisible hole.
//   3. Shared server state is reset AFTER every test, pass or throw, so a
//      test that dies halfway through cannot poison the next one (a throw
//      mid-testSessionCap used to leave 4096 synthetic entries behind).
// Combined with the eager `process.exitCode` (see §6.6 of
// docs/plan-netplay-connection.md), the shell now sees the truth.

// EXPECTED_TESTS is deliberately a literal, NOT TESTS.length: the point is
// to catch a test vanishing from the registry, and deriving it from the
// registry would make that undetectable.
const EXPECTED_TESTS = 28;

let testsRun = 0;
let testsFailed = 0;

async function runTest(name, fn) {
    testsRun += 1;
    const before = failed;
    let threw = null;
    try {
        await fn();
    } catch (err) {
        threw = err;
    }
    // Per-test isolation, pass or throw. Every test either builds its own
    // session keys or resets first, so a blanket reset here is safe — and
    // it is what makes a test that dies halfway through harmless to its
    // successors (a throw inside testSessionCap used to leave 4096
    // synthetic entries and a filled per-IP quota behind).
    try {
        H._resetRate();
        H._resetSessions();
    } catch (resetErr) {
        console.error(`TEST ERROR: ${name}: reset hook failed: ${resetErr && resetErr.stack ? resetErr.stack : resetErr}`);
        threw = threw || resetErr;
    }
    const assertionsFailed = failed - before;
    if (assertionsFailed > 0) {
        console.error(`TEST FAIL: ${name} (${assertionsFailed} assertion(s) failed)`);
    }
    if (threw) {
        console.error(`TEST ERROR: ${name}: ${threw && threw.stack ? threw.stack : threw}`);
    }
    if (assertionsFailed > 0 || threw) {
        testsFailed += 1;
    }
}

async function main() {
    // Silence info/warn logs from the server during tests so output is clean.
    const origLog = console.log;
    const origWarn = console.warn;
    console.log = () => {};
    console.warn = () => {};

    const handle = start(0);
    H = handle; // cookie helpers above need the live server's oracle
    const serverPort = await getBoundPort(handle);

    let exitCode = 0;
    try {
        // Tests that touch the rate-limit budget share the 127.0.0.1 source
        // IP; runTest() clears the rate + session state after every entry
        // rather than waiting on wall-clock windows.
        await runTest('roundTripRegister', () => testRoundTripRegister(serverPort));
        await runTest('magicReject', () => testMagicReject(serverPort));
        await runTest('versionReject', () => testVersionReject(serverPort));
        await runTest('lengthReject', () => testLengthReject(serverPort));
        await runTest('pollAfterRegister', () => testPollAfterRegister(serverPort));
        await runTest('rateLimit', () => testRateLimit(serverPort));
        await runTest('sessionTtl', () => testSessionTtl(handle, serverPort));
        await runTest('sessionCap', () => testSessionCap(handle, serverPort));
        await runTest('perIpQuota', () => testPerIpQuota(handle, serverPort));
        await runTest('spoofedFloodEviction', () => testSpoofedFloodEviction(handle));
        await runTest('staleSlotReclaim', () => testStaleSlotReclaim(handle, serverPort));
        await runTest('rehostWithinStaleWindow', () => testRehostWithinStaleWindow(handle, serverPort));
        await runTest('freshSlotNotReclaimed', () => testFreshSlotNotReclaimed(handle, serverPort));
        await runTest('reclaimRequiresSameIp', () => testReclaimRequiresSameIp(handle));
        await runTest('poisonedKeyBothSlotsStale', () => testPoisonedKeyBothSlotsStale(handle));
        await runTest('staleJoinerSlotReplaced', () => testStaleJoinerSlotReplaced(handle));

        // --- S4c: return-routability + per-key cap + version interlock ---
        await runTest('cookieChallengeRequired', () => testCookieChallengeRequired(handle, serverPort));
        await runTest('cookieBoundToSource', () => testCookieBoundToSource(handle));
        await runTest('spoofedSourceCannotBind', () => testSpoofedSourceCannotBind(handle));
        await runTest('perKeyRateCap', () => testPerKeyRateCap(handle));
        await runTest('cookieRotationWindow', () => testCookieRotationWindow(handle));
        await runTest('v1ClientInterlock', () => testV1ClientInterlock(handle, serverPort));

        // --- Review HIGH-2 / MEDIUM-3: pre-validation resource policy ---
        await runTest('cookiedNotStarvedByUncookied', () => testCookiedNotStarvedByUncookied(handle));
        await runTest('preGateBudgetBoundsChallenges', () => testPreGateBudgetBoundsChallenges(handle));
        await runTest('rateMapBounded', () => testRateMapBounded(handle));

        // --- Relay removal: the budget it inflated, and the types it freed ---
        await runTest('keyBudgetCoversLegitimateSignalling', () => testKeyBudgetCoversLegitimateSignalling(handle));
        await runTest('retiredRelayTypesAreUnknown', () => testRetiredRelayTypesAreUnknown(handle));

        await runTest('sweepHook', () => testSweepHook(handle));
    } catch (err) {
        // Only reachable if runTest() itself (or the registry) breaks — a
        // test body throwing is handled inside runTest and never lands here.
        console.error(`UNCAUGHT (runner): ${err && err.stack ? err.stack : err}`);
        exitCode = 1;
    } finally {
        console.log = origLog;
        console.warn = origWarn;
    }

    if (testsRun !== EXPECTED_TESTS) {
        console.error(`COVERAGE FAIL: ran ${testsRun} test(s), expected exactly ${EXPECTED_TESTS} — a test was skipped, aborted the runner, or was removed without updating EXPECTED_TESTS`);
        exitCode = 1;
    }
    if (failed > 0 || testsFailed > 0) {
        console.error(`${failed} assertion(s) failed; ${testsFailed}/${testsRun} test(s) failed or errored`);
        exitCode = 1;
    }

    // Clean up the server socket WITHOUT routing through handle._shutdown:
    // that path ends in socket.close(() => process.exit(0)), whose
    // hardcoded 0 raced ahead of our own exit and stamped "success" over
    // every failing run. Closing the socket directly leaves the server's
    // sweep intervals holding the event loop open just long enough for
    // the forced-exit timer below to fire with the REAL code.
    try { handle.socket.close(); } catch (_) {}

    if (exitCode === 0) {
        console.log(`protocol test passed (${testsRun}/${EXPECTED_TESTS} tests)`);
    }
    // Set the exit code EAGERLY. The forced-exit timer below is .unref()'d
    // so it cannot hold the loop open — which means that once the server
    // socket and every client socket are closed, node drains and exits on
    // its own, BEFORE the timer ever fires. Relying on the timer alone
    // made this whole file exit 0 no matter how many assertions failed
    // (found 2026-08-24 while proving the S4c tests can go red: a
    // deliberately broken server still "passed" at the shell level).
    // process.exitCode survives a natural drain; the timer stays as the
    // belt-and-braces path for a run that leaves a live handle behind.
    process.exitCode = exitCode;
    setTimeout(() => process.exit(exitCode), 50).unref();
}

main();
