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
// Task #122: type 5 is NACK, a typed server refusal.
const TYPE_NACK = 5;
const REGISTER_LEN = 36;
const POLL_LEN = 36;
const DELIVER_LEN = 32;
const CHALLENGE_LEN = 32;
const NACK_LEN = 28;
const COOKIE_LEN = 8;
const V1_REGISTER_LEN = 28;

// Task #122 reason codes, restated as literals for the same reason every
// other wire constant in this file is: a duplicate catches an encoding bug
// on either side. testNackPerReason ALSO asserts this table equals the
// server's own _nackReasons hook, which is what catches a RENUMBER — the
// failure mode that matters, since the server and the C client deploy
// independently and a renumber is a silent misattribution.
const NACK = {
    BAD_VERSION: 1,
    BAD_LENGTH: 2,
    BAD_TYPE: 3,
    RATE_IP: 4,
    RATE_KEY: 5,
    RATE_PREGATE: 6,
    KEY_QUOTA: 7,
    TABLE_FULL: 8,
    SESSION_FULL: 9,
};

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

function isNack(buf) {
    return buf.length === NACK_LEN &&
        buf.readUInt32BE(0) === MAGIC &&
        buf.readUInt8(4) === VERSION &&
        buf.readUInt8(5) === TYPE_NACK;
}

function decodeNack(buf) {
    if (!isNack(buf)) {
        throw new Error(`not a NACK (len=${buf.length} ver=${buf.length >= 5 ? buf.readUInt8(4) : '?'} type=${buf.length >= 6 ? buf.readUInt8(5) : '?'})`);
    }
    return {
        reason: buf.readUInt8(6),
        sessionKey: Buffer.from(buf.subarray(8, 24)),
    };
}

// The per-reason proof depends on reading the reason off a captured send,
// so "nothing was sent" has to fail cleanly and name the reason we were
// inducing rather than throwing out of the test body.
function assertNackReason(sent, wantReason, msg) {
    if (!sent) {
        assert(false, `${msg} (no packet emitted at all)`);
        return null;
    }
    if (!isNack(sent.buf)) {
        assert(false, `${msg} (emitted a non-NACK: len=${sent.buf.length} type=${sent.buf.length >= 6 ? sent.buf.readUInt8(5) : '?'})`);
        return null;
    }
    const dec = decodeNack(sent.buf);
    assertEq(dec.reason, wantReason, msg);
    return dec;
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
        // A FUTURE version (v3) must never be GUESSED AT — no slot, no
        // state, no DELIVER. Task #122 changed what the sender is told,
        // not what the server does: it is still refused, but now it is
        // refused OUT LOUD. Before #122 this was total silence, which is
        // byte-identical to an unreachable server (#87: prod ran a v1
        // server that dropped every v2 REGISTER exactly this way and
        // nobody could tell).
        const bad = regLocal(sessionKey, c, { version: 3 });
        await c.send(bad, serverPort);
        const reply = await c.tryRecv(300);
        assert(reply !== null, 'bad version draws a reply (a NACK, not silence)');
        if (reply) {
            assertNackReason({ buf: reply.buf }, NACK.BAD_VERSION, 'bad version -> NACK BAD_VERSION');
            // The reply is stamped with OUR version, not the sender's.
            // That is what makes it readable as protocol skew by a client
            // newer than us (direct_p2p.c badver_n).
            assertEq(reply.buf.readUInt8(4), VERSION, 'BAD_VERSION NACK carries the SERVER version byte');
        }
        // Still binds nothing.
        assert(!H._sessionMap.has(sessionKey.toString('hex')), 'bad version binds no session');
    } finally {
        await c.close();
    }
}

async function testLengthReject(serverPort) {
    const sessionKey = crypto.randomBytes(16);
    const c = await makeClient();
    try {
        // A wrong-length REGISTER that is still big enough to answer
        // without amplifying (30 >= NACK_LEN) IS told what is wrong.
        const bad = regLocal(sessionKey, c, { length: 30 });
        await c.send(bad, serverPort);
        const reply = await c.tryRecv(300);
        assert(reply !== null, 'wrong-length REGISTER draws a reply (a NACK, not silence)');
        if (reply) {
            assertNackReason({ buf: reply.buf }, NACK.BAD_LENGTH, 'wrong-length REGISTER -> NACK BAD_LENGTH');
            const dec = decodeNack(reply.buf);
            assert(dec.sessionKey.equals(sessionKey),
                'the BAD_LENGTH NACK echoes the sender\'s own session key');
        }
        assert(!H._sessionMap.has(sessionKey.toString('hex')), 'wrong-length REGISTER binds no session');

        // A SHORT one gets nothing, and that is the amplification rule
        // rather than an oversight: a 16-byte request must never draw a
        // 28-byte reply. This is the same class of frame the old test used,
        // kept here so the silent-drop half of the rule is covered too.
        H._resetRate(); // fresh cooldown, so silence means the LENGTH rule
        const tiny = regLocal(sessionKey, c, { length: 16 });
        await c.send(tiny, serverPort);
        const tinyReply = await c.tryRecv(250);
        assert(tinyReply === null,
            'a 16-byte REGISTER draws NOTHING — a reply must never be larger than the request');
        assert(!H._sessionMap.has(sessionKey.toString('hex')), 'truncated REGISTER binds no session');
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
        // #122: count the frames this limiter is ABOUT — the DELIVERs.
        // Over-budget requests now also draw a NACK, and folding those into
        // the same total made this assertion pass on arithmetic rather than
        // on intent (10 DELIVERs + 1 NACK == the old "<= 11" ceiling
        // exactly), so it would have gone green with the cap raised to 11.
        // NACK volume has its own, tighter bound and its own test.
        const delivers = replies.filter((r) => !isNack(r.buf));
        const nacks = replies.filter((r) => isNack(r.buf));
        // With a sliding window and a fresh state, the first RATE_LIMIT_PER_WINDOW
        // (=10) packets pass and the rest are dropped. Allow +1 slack for clock
        // jitter at the boundary.
        assert(delivers.length >= 1, `rate-limit: at least 1 DELIVER (got ${delivers.length})`);
        assert(delivers.length <= 11, `rate-limit: <=11 DELIVERs (got ${delivers.length})`);
        // The refusals are bounded far more tightly than the drops that
        // caused them: 10 dropped REGISTERs, at most a couple of NACKs.
        assert(nacks.length <= 2,
            `rate-limit: the ${N - delivers.length} dropped REGISTERs drew at most 2 NACKs (got ${nacks.length}) — the cooldown, not one reply per drop`);
        assert(nacks.every((r) => decodeNack(r.buf).reason === NACK.RATE_IP),
            'rate-limit: refusals name the per-IP cookied cap');
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
        // #122: still DROPPED — no session, no DELIVER — but the sender is
        // now told WHY. It passed the cookie gate to get here, so this is a
        // return-routable source being told one bit of global operational
        // state, not a spoofable reflection.
        assertEq(stub.sent.length, 1, 'cap: all-paired drop answers with exactly one frame');
        assertNackReason(stub.sent[0], NACK.TABLE_FULL, 'cap: all-paired drop -> NACK TABLE_FULL');
        assert(!handle._sessionMap.has(blockedKey.toString('hex')), 'cap: paired sessions were not evicted');
        assert(!isChallenge(stub.sent[0].buf), 'cap: the all-paired reply is a NACK, never a DELIVER or CHALLENGE');

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
        const over = await c.tryRecv(300);
        // #122: still DROPPED (asserted below by the table size), but the
        // sender is now told which of the server's nine refusals it hit.
        assert(over !== null, 'quota: key over quota draws a NACK rather than silence');
        if (over) {
            assertNackReason({ buf: over.buf }, NACK.KEY_QUOTA, 'quota: over-quota key -> NACK KEY_QUOTA');
        }
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
    // #122: the third party is REFUSED exactly as before -- slot B did not
    // move, asserted above -- and is now told why instead of being left to
    // report "matchmaking server down" for a room that is simply taken.
    assertEq(stub.sent.length, 1, 'stale-joiner: third party gets exactly one frame');
    assertNackReason(stub.sent[0], NACK.SESSION_FULL, 'stale-joiner: third party -> NACK SESSION_FULL');
}

// --- #130 helpers -----------------------------------------------------------
//
// Give a slot an OBSERVED cadence by making its occupant actually re-REGISTER
// after `gapMs` of simulated silence. This drives the real touchSlot() path
// rather than poking entry.refreshX, so a regression that stops MEASURING the
// cadence (as opposed to stops CHECKING it) still fails these tests.
function observeCadence(handle, entry, key, slot, ip, port, gapMs, stub) {
    entry[slot === 'A' ? 'lastSeenA' : 'lastSeenB'] -= gapMs;
    handle._onMessage(regFrom(key, port, ip, port), { address: ip, port }, stub);
    return entry[slot === 'A' ? 'refreshA' : 'refreshB'];
}

// Age a slot by exactly enough to cross (or not cross) its own reclaim
// threshold. Returns the threshold so a test can assert against it.
function ageSlotPast(handle, entry, slot, extraMs) {
    const refresh = entry[slot === 'A' ? 'refreshA' : 'refreshB'];
    const threshold = handle._slotReclaimIdleMs(refresh);
    entry[slot === 'A' ? 'lastSeenA' : 'lastSeenB'] -= threshold + extraMs;
    return threshold;
}

async function testJoinerPortReclaimSameIp(handle) {
    // Task #105. The joiner's automatic second attempt binds a FRESH local
    // socket on purpose (it exists to dodge stale per-port NAT state), so
    // its REGISTER arrives from a new PUBLIC port and matches neither slot.
    // Slot B is NOT stale — attempt 1 refreshed it moments ago at its
    // ~500 ms cadence — so before this fix the retry fell through to the
    // third-party drop and was ignored for the whole connect budget, and
    // the joiner reported "Matchmaking auth failed. Update the game."
    const key = crypto.randomBytes(16);
    const stub = makeStubSocket();
    handle._onMessage(regFrom(key, 1111, '198.51.100.40', 1111), { address: '198.51.100.40', port: 1111 }, stub);
    handle._onMessage(regFrom(key, 2222, '198.51.100.41', 2222), { address: '198.51.100.41', port: 2222 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));
    assertEq(entry.endpointB.port, 2222, 'joiner-port: attempt 1 holds slot B');

    // #130 DIRECTION 1 -- a LIVE slot is NOT reclaimable.
    //
    // Attempt 1 is racing at its ~500 ms cadence, so let the server MEASURE
    // that, then have a same-IP party on a fresh port try to take the slot
    // while it is still being refreshed. Pre-#130 this succeeded and the
    // paired peer was re-notified with the claimant's endpoint -- the whole
    // finding. It must now be refused exactly like any other third party.
    const refreshB = observeCadence(handle, entry, key, 'B', '198.51.100.41', 2222, 500, stub);
    assert(refreshB >= 500, `joiner-port: slot B cadence observed (got ${refreshB})`);
    assertEq(handle._slotReclaimIdleMs(refreshB),
             refreshB * handle._portReclaimMissedRefreshes,
             'joiner-port: a joiner-cadence slot scales, it does not hit the SLOT_STALE_MS ceiling');
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 3333, '198.51.100.41', 3333), { address: '198.51.100.41', port: 3333 }, stub);
    assertEq(entry.endpointB.port, 2222, 'joiner-port: LIVE slot B NOT repointed by a same-IP new port');
    assertEq(entry.portReclaims, 0, 'joiner-port: no reclaim budget spent on a live slot');
    assertEq(stub.sent.length, 1, 'joiner-port: same-IP claimant on a live slot gets exactly one frame');
    assertNackReason(stub.sent[0], NACK.SESSION_FULL, 'joiner-port: live-slot claim -> NACK SESSION_FULL');

    // #130 DIRECTION 2 -- a GENUINELY stale slot still is. This is the #105
    // case and it must keep working, or the retry lockout returns. Attempt 1's
    // socket is gone, so its lastSeenB is frozen and crosses the threshold
    // while attempt 2 is still re-REGISTERing every 500 ms into an 8000 ms
    // signalling leg (src/netplay/direct_p2p.c:4126).
    const thresholdB = ageSlotPast(handle, entry, 'B', 1);
    assert(thresholdB < 8000,
           `joiner-port: threshold ${thresholdB}ms must fit inside the 8000ms attempt-2 signal leg, or #105 is inert again`);
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 3333, '198.51.100.41', 3333), { address: '198.51.100.41', port: 3333 }, stub);
    assertEq(entry.endpointB.port, 3333, 'joiner-port: stale slot B repointed to the retry port');
    assertEq(entry.portReclaims, 1, 'joiner-port: the PORT-reclaim arm ran (not the bStale arm)');
    assertEq(entry.refreshB, 0, 'joiner-port: a repointed slot restarts its cadence measurement');
    assertEq(entry.endpointA.port, 1111, 'joiner-port: host slot untouched');
    assertEq(stub.sent.length, 2, 'joiner-port: retry got a reply AND the host got a re-notify push');
    const toJoiner = stub.sent.find((s) => s.port === 3333);
    const toHost = stub.sent.find((s) => s.port === 1111);
    assert(toJoiner && decodeDeliver(toJoiner.buf).peerPort === 1111, 'joiner-port: retry told the host endpoint');
    assert(toHost && decodeDeliver(toHost.buf).peerPort === 3333, 'joiner-port: host told the NEW joiner port');

    // THE BOUNDARY THIS MUST NOT CROSS: a DIFFERENT IP still cannot touch
    // a live slot B. Same assertion as testStaleJoinerSlotReplaced's third-
    // party control, restated here against a slot that was just reclaimed.
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 4444, '198.51.100.42', 4444), { address: '198.51.100.42', port: 4444 }, stub);
    assertEq(entry.endpointB.port, 3333, 'joiner-port: different-IP third party did NOT take live slot B');
    assertEq(stub.sent.length, 1, 'joiner-port: different-IP third party gets exactly one frame');
    assertNackReason(stub.sent[0], NACK.SESSION_FULL, 'joiner-port: different-IP third party -> NACK SESSION_FULL');

}

async function testPortReclaimBudgetExhausted(handle) {
    // The cap still bounds flapping, now as the SECOND of two independent
    // conditions rather than the only one. Split out of
    // testJoinerPortReclaimSameIp because #130 added two more REGISTERs from
    // that test's joiner IP and the combined test crossed the 10/s cookied
    // per-IP budget -- which silently dropped reclaims and made the cap look
    // like it had been hit early. A fresh key and a fresh pair of IPs keeps
    // every source under RATE_LIMIT_PER_WINDOW.
    const key = crypto.randomBytes(16);
    const stub = makeStubSocket();
    handle._onMessage(regFrom(key, 1111, '198.51.100.60', 1111), { address: '198.51.100.60', port: 1111 }, stub);
    handle._onMessage(regFrom(key, 2222, '198.51.100.61', 2222), { address: '198.51.100.61', port: 2222 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));

    const cap = handle._maxPortReclaims;
    assert(cap > 0, 'budget: cap is exposed and positive');
    // Each flap must ALSO wait the slot out. Drive them through the real
    // arm, one per source port, aging the slot past its own threshold first.
    for (let i = 0; i < cap; i++) {
        const port = 5000 + i;
        entry.lastSeenB -= handle._slotReclaimIdleMs(entry.refreshB) + 1;
        handle._onMessage(regFrom(key, port, '198.51.100.61', port), { address: '198.51.100.61', port }, stub);
    }
    assertEq(entry.portReclaims, cap, 'budget: fully spent through the real reclaim arm');

    const heldPort = entry.endpointB.port;
    stub.sent.length = 0;
    // Stale enough to pass #130's precondition, but out of budget. Keep it
    // UNDER SLOT_STALE_MS so the pre-existing bStale arm cannot mask the
    // refusal -- otherwise this would assert nothing about the cap.
    entry.refreshB = 500;
    entry.lastSeenB -= handle._slotReclaimIdleMs(500) + 1;
    assert(handle._slotReclaimIdleMs(500) + 1 < handle._slotStaleMs,
           'budget: the aged slot is below SLOT_STALE_MS, so bStale cannot mask the cap');
    handle._onMessage(regFrom(key, 6000, '198.51.100.61', 6000), { address: '198.51.100.61', port: 6000 }, stub);
    assertEq(entry.endpointB.port, heldPort, 'budget: reclaim refused once the budget is spent');
    assertEq(stub.sent.length, 1, 'budget: over-budget reclaim gets exactly one frame');
    assertNackReason(stub.sent[0], NACK.SESSION_FULL, 'budget: over-budget reclaim -> NACK SESSION_FULL');
}

async function testLiveHostSlotNotHijackable(handle) {
    // TASK #130, THE FINDING ITSELF, at the slot that matters most.
    //
    // Pre-#130 both reclaim arms fired on same-IP + budget alone. A party
    // sharing a public IP with the HOST (CGNAT) and holding the room code
    // could therefore evict the live host from slot A and have the paired
    // joiner re-notified with ITS endpoint -- not a denial, a redirection.
    //
    // What closes it is that the threshold is a multiple of the slot's OWN
    // observed cadence. A host advertises every 5000 ms
    // (src/port/config/config.c:111), so 6 x 5000 = 30000 ms = SLOT_STALE_MS:
    // over host-cadence slots the port-reclaim arms now grant exactly nothing
    // that the pre-existing staleness rule did not already grant. This test
    // asserts that equivalence numerically, so a change to either constant
    // that breaks it fails here rather than silently reopening the window.
    const key = crypto.randomBytes(16);
    const stub = makeStubSocket();
    handle._onMessage(regFrom(key, 1111, '198.51.100.70', 1111), { address: '198.51.100.70', port: 1111 }, stub);
    handle._onMessage(regFrom(key, 2222, '198.51.100.71', 2222), { address: '198.51.100.71', port: 2222 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));

    // Let the server measure the HOST's 5 s advertise cadence.
    const refreshA = observeCadence(handle, entry, key, 'A', '198.51.100.70', 1111, 5000, stub);
    assert(refreshA >= 5000, `live-host: host cadence observed (got ${refreshA})`);
    assertEq(handle._slotReclaimIdleMs(refreshA), handle._slotStaleMs,
             'live-host: a host-cadence slot resolves to exactly SLOT_STALE_MS');

    // A joiner-scale idle (well past 6 x 500 ms) must NOT be enough here.
    // This is the assertion a fixed window could never make: the same 3 s of
    // silence reclaims a joiner slot and must not touch a host slot.
    const joinerScaleIdle = handle._slotReclaimIdleMs(500) + 100;
    assert(joinerScaleIdle < handle._slotStaleMs, 'live-host: joiner-scale idle is below the host threshold');
    entry.lastSeenA -= joinerScaleIdle;
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 9999, '198.51.100.70', 9999), { address: '198.51.100.70', port: 9999 }, stub);
    assertEq(entry.endpointA.port, 1111, 'live-host: LIVE host slot NOT hijacked by a same-IP claimant');
    assertEq(entry.endpointB.port, 2222, 'live-host: joiner slot untouched');
    assertEq(entry.portReclaims, 0, 'live-host: no reclaim budget spent');
    assertEq(stub.sent.length, 1, 'live-host: same-IP claimant gets exactly one frame');
    assertNackReason(stub.sent[0], NACK.SESSION_FULL, 'live-host: same-IP claimant -> NACK SESSION_FULL');

    // Past SLOT_STALE_MS the host slot is reclaimable exactly as it was
    // BEFORE #105 existed -- via the pre-existing stale-host arm, which is
    // why portReclaims stays at zero. The capability is not removed, it is
    // put back behind the bar it was always meant to sit behind.
    entry.lastSeenA -= handle._slotStaleMs;
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 9999, '198.51.100.70', 9999), { address: '198.51.100.70', port: 9999 }, stub);
    assertEq(entry.endpointA.port, 9999, 'live-host: genuinely stale host slot still reclaimable');
    assertEq(entry.portReclaims, 0, 'live-host: reclaimed by the pre-existing stale arm, not the port arm');
}

async function testUnobservedSlotMaximallyProtected(handle) {
    // #130 default-safety. A slot the server has seen exactly ONCE has no
    // measured cadence, and "no measurement" must resolve to maximally
    // protected, never to zero. Getting this backwards would reopen the
    // whole finding for the first seconds of every room -- precisely the
    // window in which a host has just begun advertising and has not yet sent
    // its second REGISTER.
    assertEq(handle._slotReclaimIdleMs(0), handle._slotStaleMs,
             'unobserved: an unmeasured cadence resolves to SLOT_STALE_MS');
    const key = crypto.randomBytes(16);
    const stub = makeStubSocket();
    handle._onMessage(regFrom(key, 1111, '198.51.100.80', 1111), { address: '198.51.100.80', port: 1111 }, stub);
    handle._onMessage(regFrom(key, 2222, '198.51.100.81', 2222), { address: '198.51.100.81', port: 2222 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));
    assertEq(entry.refreshB, 0, 'unobserved: one REGISTER yields no cadence');

    // Far past a joiner-cadence threshold, still short of SLOT_STALE_MS.
    entry.lastSeenB -= handle._slotReclaimIdleMs(500) * 2;
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 3333, '198.51.100.81', 3333), { address: '198.51.100.81', port: 3333 }, stub);
    assertEq(entry.endpointB.port, 2222, 'unobserved: unmeasured slot is not reclaimable on a joiner-scale idle');
    assertEq(entry.portReclaims, 0, 'unobserved: no reclaim budget spent');
    assertNackReason(stub.sent[0], NACK.SESSION_FULL, 'unobserved: claimant -> NACK SESSION_FULL');

    // A repointed slot must ALSO reset to unmeasured, so a reclaimer cannot
    // inherit the liveness the previous occupant demonstrated.
    entry.lastSeenB -= handle._slotStaleMs;
    handle._onMessage(regFrom(key, 3333, '198.51.100.81', 3333), { address: '198.51.100.81', port: 3333 }, stub);
    assertEq(entry.endpointB.port, 3333, 'unobserved: stale slot reclaimed');
    assertEq(entry.refreshB, 0, 'unobserved: the new occupant starts unmeasured, inheriting nothing');
}

async function testPortReclaimSlotA(handle) {
    // Task #105 follow-up, and the regression this exists to prevent.
    // Slots are FIRST-COME, not role-based — no role bit exists anywhere in
    // the REGISTER frame — so the retrying party routinely holds slot A
    // rather than slot B. A reclaim keyed only on endpointB silently misses
    // that entire population: measured 13/13 on the natmatrix rig, every
    // failing rep with the joiner in slot A logged 16 ignored REGISTERs and
    // zero reclaims. This test fails against a B-only rule.
    const key = crypto.randomBytes(16);
    const stub = makeStubSocket();
    // The RETRYING party binds first and therefore takes slot A.
    handle._onMessage(regFrom(key, 1111, '198.51.100.50', 1111), { address: '198.51.100.50', port: 1111 }, stub);
    handle._onMessage(regFrom(key, 2222, '198.51.100.51', 2222), { address: '198.51.100.51', port: 2222 }, stub);
    const entry = handle._sessionMap.get(key.toString('hex'));
    assertEq(entry.endpointA.address, '198.51.100.50', 'slotA-reclaim: retrying party holds slot A');
    assertEq(entry.endpointB.address, '198.51.100.51', 'slotA-reclaim: peer holds slot B');

    // #130 DIRECTION 1 -- live slot A is NOT reclaimable either. This is the
    // worse half of the finding: slot A is where a HOST sits, so pre-#130 a
    // co-located party could evict the host and have the joiner DELIVERed the
    // attacker's endpoint.
    const refreshA = observeCadence(handle, entry, key, 'A', '198.51.100.50', 1111, 500, stub);
    assert(refreshA >= 500, `slotA-reclaim: slot A cadence observed (got ${refreshA})`);
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 3333, '198.51.100.50', 3333), { address: '198.51.100.50', port: 3333 }, stub);
    assertEq(entry.endpointA.port, 1111, 'slotA-reclaim: LIVE slot A NOT repointed by a same-IP new port');
    assertEq(entry.portReclaims, 0, 'slotA-reclaim: no reclaim budget spent on a live slot');
    assertNackReason(stub.sent[0], NACK.SESSION_FULL, 'slotA-reclaim: live-slot claim -> NACK SESSION_FULL');

    // #130 DIRECTION 2 -- the #105 slot-A retry still works once the old
    // endpoint has actually gone silent for longer than its own cadence.
    const thresholdA = ageSlotPast(handle, entry, 'A', 1);
    assert(thresholdA < 8000,
           `slotA-reclaim: threshold ${thresholdA}ms must fit inside the 8000ms attempt-2 signal leg`);
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 3333, '198.51.100.50', 3333), { address: '198.51.100.50', port: 3333 }, stub);
    assertEq(entry.endpointA.port, 3333, 'slotA-reclaim: stale slot A repointed to the retry port');
    assertEq(entry.portReclaims, 1, 'slotA-reclaim: the PORT-reclaim arm ran');
    assertEq(entry.endpointB.port, 2222, 'slotA-reclaim: peer slot untouched');
    assertEq(stub.sent.length, 2, 'slotA-reclaim: retry got a reply AND the peer got a re-notify push');
    const toRetry = stub.sent.find((s) => s.port === 3333);
    const toPeer = stub.sent.find((s) => s.port === 2222);
    assert(toRetry && decodeDeliver(toRetry.buf).peerPort === 2222, 'slotA-reclaim: retry told the peer endpoint');
    assert(toPeer && decodeDeliver(toPeer.buf).peerPort === 3333, 'slotA-reclaim: peer told the NEW port');

    // Same boundary as the slot-B arm: a different IP cannot take a live slot.
    stub.sent.length = 0;
    handle._onMessage(regFrom(key, 4444, '198.51.100.52', 4444), { address: '198.51.100.52', port: 4444 }, stub);
    assertEq(entry.endpointA.port, 3333, 'slotA-reclaim: different-IP third party did NOT take live slot A');
    assertEq(entry.endpointB.port, 2222, 'slotA-reclaim: different-IP third party did NOT take live slot B');
    assertEq(stub.sent.length, 1, 'slotA-reclaim: different-IP third party gets exactly one frame');
    assertNackReason(stub.sent[0], NACK.SESSION_FULL, 'slotA-reclaim: different-IP third party -> NACK SESSION_FULL');
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
    // #122: count only the frames the cap is ABOUT — the DELIVERs the
    // server would otherwise have to produce. An over-budget request now
    // also draws a NACK, but a NACK is 28 B and is separately capped at one
    // per source per 250 ms (testNackAmplificationBound), so it is not the
    // work this limiter exists to bound. Counting them here would make the
    // test measure the wrong thing and go green against a server with the
    // per-key cap removed.
    const delivers = stub.sent.filter((s) => !isNack(s.buf));
    // +1 slack for a sliding-window boundary landing mid-loop.
    assert(delivers.length <= limit + 1,
        `per-key cap: <= ${limit + 1} DELIVER replies for ONE key across ${shots} distinct source IPs (got ${delivers.length})`);
    // The refusals that replaced them must all be the per-KEY reason.
    const refusals = stub.sent.filter((s) => isNack(s.buf));
    assert(refusals.length > 0, 'per-key cap: the over-budget sources were told they were refused');
    assert(refusals.every((s) => decodeNack(s.buf).reason === NACK.RATE_KEY),
        'per-key cap: every refusal names the per-KEY cap, not some other reason');
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
    // with version=1. Required behavior is REJECT CLEANLY — no state, no
    // CHALLENGE, and above all no hang: the server must still serve the
    // very next valid v2 exchange normally.
    //
    // #122 changes ONE thing here, and it is the whole point of the task.
    // The reply used to be nothing at all, which is byte-identical to an
    // unreachable server — and that is not hypothetical: #87 is a
    // production incident where the deployed April v1 server dropped every
    // v2 REGISTER in exactly this silence, so every client reported
    // "matchmaking server down" and the real cause went unfound. A v1
    // client now gets a NACK stamped with the version this server DOES
    // speak. It still binds nothing and it is still not a CHALLENGE.
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
        const v1Reply = await c.tryRecv(300);
        assert(v1Reply !== null, 'interlock: v1 REGISTER draws a NACK rather than #87-style silence');
        if (v1Reply) {
            assert(!isChallenge(v1Reply.buf), 'interlock: v1 REGISTER gets NO CHALLENGE');
            assertNackReason({ buf: v1Reply.buf }, NACK.BAD_VERSION, 'interlock: v1 REGISTER -> NACK BAD_VERSION');
            assertEq(v1Reply.buf.readUInt8(4), VERSION,
                'interlock: the NACK carries the version this server speaks, so a skewed client can read it');
        }
        assert(!handle._sessionMap.has(v1Key.toString('hex')), 'interlock: v1 REGISTER bound no session');
        assertEq(handle._creatorCounts.size, 0, 'interlock: v1 REGISTER consumed no key quota');

        // Also: a v2-length packet still carrying version=1 is dropped
        // by the same check, before the cookie gate.
        // Judge this probe on its own: without clearing the NACK cooldown
        // the first v1 REGISTER above has already spent this source's
        // budget, and the silence that followed would be the COOLDOWN
        // talking, not the version check. That would have made this
        // assertion pass even against a server whose version gate was
        // removed entirely.
        handle._resetRate();
        const v1Long = makeRegister(v1Key, c.port, { version: V1_VERSION });
        await c.send(v1Long, serverPort);
        const longReply = await c.tryRecv(300);
        assert(longReply !== null, 'interlock: v1 version byte at v2 length also answered');
        if (longReply) {
            assertNackReason({ buf: longReply.buf }, NACK.BAD_VERSION,
                'interlock: v1 version at v2 length -> NACK BAD_VERSION (the VERSION check, not the length check)');
        }
        assert(!handle._sessionMap.has(v1Key.toString('hex')),
            'interlock: v1 version byte at v2 length still bound no session');
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
    // #122: count CHALLENGEs, not "everything the server sent". This
    // budget bounds CHALLENGE emission and nothing else; an over-budget
    // request now also draws a NACK, and folding those in made the old
    // total (100 CHALLENGEs + 1 NACK) sit exactly on the "<= limit + 1"
    // ceiling — green by coincidence rather than by measurement.
    const challenges = stub.sent.filter((s) => isChallenge(s.buf));
    const nacks = stub.sent.filter((s) => isNack(s.buf));
    // +1 slack for a sliding-window boundary landing mid-loop.
    assert(challenges.length <= limit + 1,
        `pre-gate: <= ${limit + 1} CHALLENGEs emitted for ${shots} uncookied requests from one IP (got ${challenges.length})`);
    assertEq(challenges.length + nacks.length, stub.sent.length,
        'pre-gate: the server emitted only CHALLENGEs and NACKs');
    assert(nacks.length <= 2,
        `pre-gate: the ${shots - challenges.length} over-budget requests drew at most 2 NACKs (got ${nacks.length}) — the NACK cooldown is tighter than the budget it reports on`);
    assert(nacks.every((s) => decodeNack(s.buf).reason === NACK.RATE_PREGATE),
        'pre-gate: refusals name the pre-gate budget');
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

    // (a) TRUE junk -- a frame that cannot even be shown to claim our
    // protocol -- still allocates nothing and draws nothing, at any rate.
    // #122 keeps this class exactly as it was: a NACK is never sent for a
    // short packet or a bad magic, because answering those would make the
    // server a reflector for ARBITRARY UDP rather than merely for frames
    // that claim to be ours.
    const JUNK_SOURCES = 5000;
    for (let i = 0; i < JUNK_SOURCES; i++) {
        const addr = `10.${(i >> 16) & 255}.${(i >> 8) & 255}.${i & 255}`;
        handle._onMessage(Buffer.alloc(1), { address: addr, port: 1 }, stub);           // too short
        handle._onMessage(makeRegister(crypto.randomBytes(16), 1, { magic: 0xdeadbeef }),
            { address: addr, port: 2 }, stub);                                          // bad magic
    }
    assertEq(stub.sent.length, 0, 'bounded: short/bad-magic junk produced no replies');
    assertEq(handle._nackMap.size, 0, `bounded: ${JUNK_SOURCES} short/bad-magic sources allocated NO nack-cooldown entries`);
    assertEq(handle._rateMap.size, 0, `bounded: ${JUNK_SOURCES} junk sources allocated NO cookied rate entries`);
    assertEq(handle._preGateMap.size, 0, `bounded: ${JUNK_SOURCES} junk sources allocated NO pre-gate entries`);
    assertEq(handle._keyRateMap.size, 0, `bounded: ${JUNK_SOURCES} junk sources allocated NO per-key entries`);

    // (a2) #122: magic-bearing frames that fail version/length DO now draw
    // a NACK, so they allocate in the cooldown map -- and that map must be
    // bounded by the same cap and the same LRU as every other one. An
    // attacker must spend four correct magic bytes per entry, and gets
    // nowhere: the map stops growing.
    handle._resetRate();
    stub.sent.length = 0;
    const nackCap = handle._maxRateEntries;
    const nackSources = nackCap + 500;
    let nackFirstAddr = null;
    let nackLastAddr = null;
    for (let i = 0; i < nackSources; i++) {
        const addr = `192.0.${(i >> 8) & 255}.${i & 255}`;
        if (i === 0) nackFirstAddr = addr;
        nackLastAddr = addr;
        handle._onMessage(makeRegister(crypto.randomBytes(16), 1, { version: 3 }),
            { address: addr, port: 3 }, stub);
    }
    assert(handle._nackMap.size <= nackCap,
        `bounded: nack-cooldown map stayed <= ${nackCap} across ${nackSources} distinct spoofed sources (got ${handle._nackMap.size})`);
    assert(!handle._nackMap.has(nackFirstAddr), 'bounded: nack cooldown evicts least-recently-used');
    assert(handle._nackMap.has(nackLastAddr), 'bounded: nack cooldown retains the most recent source');
    // Still allocates nothing in the REQUEST-PATH buckets: a bad-version
    // frame never reaches the cookie gate, so it cannot touch the budgets
    // a real client depends on (review HIGH-2 / MEDIUM-3 both intact).
    assertEq(handle._rateMap.size, 0, 'bounded: bad-version frames allocate NO cookied rate entries');
    assertEq(handle._preGateMap.size, 0, 'bounded: bad-version frames allocate NO pre-gate entries');
    assertEq(handle._keyRateMap.size, 0, 'bounded: bad-version frames allocate NO per-key entries');
    handle._resetRate();
    stub.sent.length = 0;

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
    //     cfg.signal_leg = false when acting as host: the DELIVER that
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

    // The TWO-PEER cadence, stated so a change to either leg trips here
    // rather than in the field.
    assert(handle._keyRateLimit >= 3 * steadyPerSec,
        `key-budget: KEY_RATE_LIMIT_PER_WINDOW (${handle._keyRateLimit}) keeps >= 3x margin over the two-peer per-key cadence (${steadyPerSec}/s)`);

    // This test deliberately states NO upper bound on the constant, and
    // that omission is review HIGH-3. It used to assert
    // `_keyRateLimit <= 4 * steadyPerSec` (<= 12), which looks like a
    // tight sizing guard but is really a hardcoded TWO-PEER model: the
    // session key IS the room code (derived from the host's public
    // endpoint, src/netplay/direct_p2p.c:3191-3193), so a room's real
    // traffic is 1 + 2N for N simultaneous dialers, and every dialer
    // charges this bucket whether or not the two-slot policy lets it in.
    // A guard that can only ever see N = 1 cannot distinguish a correctly
    // sized cap from one that self-DoSes a five-dialer room -- it happily
    // ratified 10/s, the value that does. Sizing (and the per-IP-vs-per-key
    // ratio) is asserted by testKeyBudgetCoversMultiJoinerRoom below.
    handle._resetSessions();
    handle._resetRate();
}

async function testKeyBudgetCoversMultiJoinerRoom(handle) {
    // Review HIGH-3. The failure the two-peer test above structurally
    // cannot see: N people dialling ONE room code.
    //
    // Why they all share a bucket: the session key is derived from the
    // HOST's public endpoint (src/netplay/direct_p2p.c:3191-3193), so the
    // key IS the room code. Everyone who pastes that code hashes to the
    // same key, and each one starts its 500 ms REGISTER resend
    // (direct_p2p.c:1958 -> 2/s) the moment the code is entered,
    // without waiting to be accepted (cfg.signal_leg, direct_p2p.c:4059,
    // keys only on "have signal URL + have session key"). The server's
    // two-slot policy silences dialers 2..N at DISPATCH -- but the per-key
    // gate runs UPSTREAM of dispatch (returnRoutabilityGate), so they have
    // already spent the budget by then.
    //
    // What must survive: the host's re-REGISTER liveness leg (floor
    // 1000 ms, direct_p2p.c:2463-2465 -> 1/s). Its packets are what
    // refresh entry.lastSeenA; if they are rate-dropped, lastSeenA goes
    // stale and SLOT_STALE_MS / SESSION_TTL_MS reclaim a room whose code
    // is still displayed on the host's screen. No attacker required.
    handle._resetSessions();
    handle._resetRate();

    // The arithmetic below is in requests-per-second, which is only the
    // same thing as requests-per-window because the window is 1 s.
    assertEq(handle._keyRateWindowMs, 1000,
        'multi-joiner: KEY_RATE_WINDOW_MS is 1 s, so per-window == per-second');

    // #123: read the design point rather than restating it. The two client
    // cadences below CANNOT be read from here — they are literals in C that
    // no JS module can see — so they stay mirrored, and the coupling is held
    // instead by tools/rendezvous-server/check_key_rate_budget.py, which
    // reads them out of direct_p2p.c and runs in the gate set.
    const N = handle._keyDesignDialers;
    const HOST_PER_SEC = 1000 / 1000;  // REGISTER_INTERVAL_MS floor
    const JOINER_PER_SEC = 1000 / 500; // race signal resend, per joiner
    const legitPeak = HOST_PER_SEC + N * JOINER_PER_SEC;
    assertEq(legitPeak, 13, 'multi-joiner: a 6-dialer room peaks at 1 + 2*6 = 13 req/s on one key');

    const key = crypto.randomBytes(16);
    const hexKey = key.toString('hex');
    const HOST = { address: '198.51.100.10', port: 5000 };
    // Distinct source IP per joiner, so the per-IP bucket
    // (RATE_LIMIT_PER_WINDOW) can never be what this test measures --
    // no source below sends more than 2 frames.
    const joiners = [];
    for (let i = 0; i < N; i++) {
        joiners.push({ address: `203.0.113.${20 + i}`, port: 6000 + i });
    }
    const stub = makeStubSocket();

    // Setup, OUTSIDE the measured second: the host is already registered
    // and holding slot A -- this is the room whose code is on screen. The
    // per-key bucket is cleared afterwards so setup traffic does not
    // charge the second we are about to model.
    handle._onMessage(regFrom(key, HOST.port, HOST.address, HOST.port), HOST, stub);
    assert(handle._sessionMap.has(hexKey), 'multi-joiner: host bound slot A (setup)');
    handle._resetRate();
    stub.sent.length = 0;

    // The modelled second, in the PESSIMAL arrival order: every dialer's
    // traffic lands before the host's liveness tick. This is not a
    // contrived ordering -- the dialers are free-running at 2/s each and
    // the host ticks once, so the host's packet is last in some second
    // roughly N/(N+1) of the time.
    const second = [];
    for (const j of joiners) { second.push(j); second.push(j); }
    assertEq(second.length, N * JOINER_PER_SEC,
        'multi-joiner: the joiner phase is exactly 2 frames per dialer');
    for (const who of second) {
        handle._onMessage(regFrom(key, who.port, who.address, who.port), who, stub);
    }

    // Age the host's slot liveness so the refresh is OBSERVABLE. Without
    // this, a passing and a failing run both leave lastSeenA at a value
    // within the same millisecond and the assertion cannot tell them
    // apart. 5 s is well inside SLOT_STALE_MS (30 s), so this does not
    // trip the stale-slot reclaim paths.
    const entry = handle._sessionMap.get(hexKey);
    assert(entry !== undefined, 'multi-joiner: the room still exists after the dialer wave');
    const agedTo = entry.lastSeenA - 5000;
    entry.lastSeenA = agedTo;

    // The host's liveness REGISTER, last in the second.
    const before = stub.sent.length;
    handle._onMessage(regFrom(key, HOST.port, HOST.address, HOST.port), HOST, stub);
    const toHost = stub.sent.slice(before)
        .filter((s) => s.address === HOST.address && s.port === HOST.port);

    // THE assertion. A rate-dropped REGISTER is answered with nothing at
    // all, so "the host got a packet in response to its own frame" is
    // exactly "the host's liveness leg was not starved". The slice()
    // above matters: the first dialer's REGISTER pairs into slot B and
    // pushes an unsolicited DELIVER to the host, so counting sends over
    // the WHOLE second would score a starved host as healthy.
    assertEq(toHost.length, 1,
        `multi-joiner: the host's liveness REGISTER was answered after ${second.length} dialer frames on the same key`);
    assert(entry.lastSeenA > agedTo,
        'multi-joiner: ...and it refreshed lastSeenA, so SLOT_STALE_MS cannot reclaim a live room');

    // Corroboration: every frame of the modelled second was actually
    // admitted by the PER-KEY bucket. This is what proves the assertion
    // above is measuring this limiter and not the slot policy.
    const bucket = handle._keyRateMap.get(hexKey);
    assert(bucket !== undefined, 'multi-joiner: the room code has a per-key rate bucket');
    assertEq(bucket ? bucket.timestamps.length : -1, second.length + 1,
        `multi-joiner: all ${second.length + 1} frames of the modelled second passed the per-key gate`);
    // ...and that no single source came anywhere near the per-IP cap, so
    // RATE_LIMIT_PER_WINDOW is provably not the limiter under test.
    assert(2 < handle._rateLimit,
        `multi-joiner: no source sent more than 2 frames, well under the per-IP cap (${handle._rateLimit})`);

    // --- Sizing invariants -------------------------------------------------
    // These fail loudly if someone re-equalises the two caps or restores
    // the relay-era 40. See the derivation on KEY_RATE_LIMIT_PER_WINDOW.
    //
    // 1. Strictly greater than the per-IP cap. At equality the per-key
    //    limiter buys ZERO attacker cost over the per-IP one -- that IP is
    //    already admitted at its full rate -- while handing one cookied IP
    //    the power to drop every other frame on the key. cookieForSlot()
    //    does not mix the session key in, so a cookie earned on the
    //    attacker's own key validates against any room's key.
    assert(handle._keyRateLimit > handle._rateLimit,
        `sizing: per-key cap (${handle._keyRateLimit}) is strictly greater than the per-IP cap (${handle._rateLimit}) -- at equality one cookied IP owns 100% of a room's budget`);
    // 2. Absorption: one saturating cookied IP must not break a full room.
    assert(handle._keyRateLimit - handle._rateLimit >= legitPeak,
        `sizing: ${handle._keyRateLimit} - ${handle._rateLimit} = ${handle._keyRateLimit - handle._rateLimit}/s survives one saturating cookied IP and still covers a ${legitPeak}/s room`);
    // 3. Not oversized. k = 3 is the SMALLEST integer factor satisfying
    //    (2), so anything above it is relay-era slack with no surviving
    //    traffic to justify it -- and a looser per-key bound is a weaker
    //    bound on a key an attacker with many cookie-capable IPs is
    //    hammering.
    assert(handle._keyRateLimit <= 3 * handle._rateLimit,
        `sizing: per-key cap (${handle._keyRateLimit}) is at most 3x the per-IP cap (${3 * handle._rateLimit}) -- k=3 is the smallest factor that satisfies absorption; more is relay-era slack`);

    handle._resetSessions();
    handle._resetRate();
}

async function testRetiredRelayTypesAreUnknown(handle) {
    // Types 5/6/7/8 were RELAY_REQ / RELAY_GRANT / RELAY_PIN /
    // RELAY_PIN_ACK. The relay is deleted. Type 5 has since been
    // RE-ALLOCATED as NACK (task #122); 6/7/8 remain unallocated.
    //
    // The invariants that must hold for ALL of them are the state ones,
    // and they are unchanged: no session bound, and the frame never even
    // reaches the per-key rate bucket.
    //
    // What #122 DID change is the reply rule, deliberately. This test used
    // to require "no reply of ANY kind, because a reply would make the
    // server a reflector for a frame nothing owns". That objection was
    // sound when there was no bounded reply mechanism; there is one now
    // (28 B answering >= 36 B, and at most one per source IP per 250 ms
    // regardless of inbound rate -- see testNackAmplificationBound), so a
    // stale relay-era build gets told "BAD_TYPE" instead of being left to
    // report "matchmaking server down". Two classes now:
    //   * UNALLOCATED types -> exactly one NACK, reason BAD_TYPE.
    //   * SERVER -> CLIENT types (DELIVER 2, CHALLENGE 4, NACK 5) -> still
    //     absolutely no reply, which is what keeps the feature loop-free:
    //     no frame this server emits can elicit a reply from another
    //     instance of it. Covered in full by testNackNeverAnswersOwnFrames.
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
        // _resetRate() also clears the NACK cooldown, so each iteration
        // gets a fresh budget and a missing NACK means a missing NACK.
        handle._resetSessions();
        handle._resetRate();
        stub.sent.length = 0;
        // Correctly shaped AND correctly cookied for this source: the only
        // thing wrong with the frame is its type byte, so anything that
        // answers is answering on the type byte alone.
        const buf = makeRegister(key, SRC.port,
            { type, cookie: cookieFor(SRC.address, SRC.port) });
        handle._onMessage(buf, SRC, stub);
        if (type === TYPE_NACK) {
            assertEq(stub.sent.length, 0,
                'retired-types: type 5 is now NACK (server->client) and draws no reply -- no NACK ping-pong');
        } else {
            assertEq(stub.sent.length, 1, `retired-types: unallocated type ${type} draws exactly one frame`);
            assertNackReason(stub.sent[0], NACK.BAD_TYPE, `retired-types: type ${type} -> NACK BAD_TYPE`);
        }
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

// --- Task #122: typed refusals + the two field metrics -----------------------

async function testNackPerReason(handle) {
    // THE CORE PROOF. Nine server conditions used to be byte-identical on
    // the wire (silence) and were all reported to the user as
    // P2P_FAIL_RENDEZVOUS_DOWN. Each one is INDUCED here for real -- by
    // driving the server into the actual condition, never by calling the
    // encoder -- and the reply's reason byte is checked to be the RIGHT
    // one. A NACK that says the wrong thing is worse than silence (H-1),
    // so "a NACK arrived" is not the assertion; "the correct NACK arrived"
    // is.
    const seen = new Set();

    // Wire values are shared with src/netplay/rendezvous.h and the two
    // deploy independently, so a renumber is a silent misattribution.
    // Pin this file's literals against the server's own table.
    for (const [name, value] of Object.entries(NACK)) {
        assertEq(handle._nackReasons[name], value,
            `nack: reason ${name} has the same wire value in the test and the server`);
    }
    assertEq(Object.keys(handle._nackReasons).length, Object.keys(NACK).length,
        'nack: the server defines exactly the reason set this test knows about');

    // ...and against the C CLIENT's copy, which is the one that actually
    // matters. The check above is JS-vs-JS and cannot see a drift between
    // this server and src/netplay/rendezvous.h -- and those two DEPLOY
    // INDEPENDENTLY (the server is a long-lived VPS process, the client
    // ships in a release ZIP), so nothing at build time links them. This
    // is the same hazard check_key_rate_budget.py exists for on the
    // cadence side: a renumber on either side would not break a build, it
    // would silently make the client report the WRONG REASON, which is
    // strictly worse than the silence #122 replaced.
    {
        const headerPath = require('path').join(__dirname, '..', '..', 'src', 'netplay', 'rendezvous.h');
        let header = null;
        try {
            header = require('fs').readFileSync(headerPath, 'utf8');
        } catch (err) {
            assert(false, `nack/C-parity: cannot read ${headerPath}: ${err.message}`);
        }
        if (header !== null) {
            const cValues = new Map();
            const re = /^\s*REND_NACK_([A-Z_]+)\s*=\s*(\d+)\s*,?/gm;
            let m;
            while ((m = re.exec(header)) !== null) {
                if (m[1] !== 'NONE') cValues.set(m[1], Number(m[2]));
            }
            assertEq(cValues.size, Object.keys(NACK).length,
                `nack/C-parity: rendezvous.h defines ${cValues.size} reasons, the server defines ${Object.keys(NACK).length}`);
            for (const [name, value] of Object.entries(NACK)) {
                assertEq(cValues.get(name), value,
                    `nack/C-parity: REND_NACK_${name} is ${cValues.get(name)} in rendezvous.h and ${value} in the server`);
            }
            // The frame length is the amplification bound; a C client that
            // disagrees about it would reject every NACK on the length test.
            const lenM = /#define\s+REND_NACK_LEN\s+(\d+)/.exec(header);
            assert(lenM !== null, 'nack/C-parity: rendezvous.h defines REND_NACK_LEN');
            if (lenM) {
                assertEq(Number(lenM[1]), handle._nackLen,
                    'nack/C-parity: REND_NACK_LEN agrees with the server NACK_LEN');
            }
            const typeM = /#define\s+REND_FRAME_NACK\s+(\d+)/.exec(header);
            assert(typeM !== null, 'nack/C-parity: rendezvous.h defines REND_FRAME_NACK');
            if (typeM) {
                assertEq(Number(typeM[1]), handle._typeNack,
                    'nack/C-parity: REND_FRAME_NACK agrees with the server TYPE_NACK');
            }
        }
    }

    // ...and, task #122 JOB 1, that the client actually DOES something
    // with each one.
    //
    // WHY THIS CHECK EXISTS AT ALL. The parity block above passed for the
    // whole life of the feature while the client half was DEAD: eaf72865
    // shipped nine reasons and Rendezvous_ParseNack to read them, and
    // ParseNack had no caller anywhere -- so every reason still collapsed
    // into P2P_FAIL_RENDEZVOUS_DOWN. Matching NUMBERS was never the
    // property that mattered; a reason reaching a VERDICT is.
    //
    // Nothing at build time links these files (no TU sees both a JS const
    // and a C case label), and they deploy independently, so a reason the
    // server learns to send and connect_fail.c never learns to name would
    // degrade silently to the catch-all rather than failing anything.
    // Same hazard, same remedy, as the value parity above.
    {
        const cfPath = require('path').join(__dirname, '..', '..', 'src', 'netplay', 'connect_fail.c');
        let cf = null;
        try {
            cf = require('fs').readFileSync(cfPath, 'utf8');
        } catch (err) {
            assert(false, `nack/verdict-parity: cannot read ${cfPath}: ${err.message}`);
        }
        if (cf !== null) {
            // The mapper's own body, not the whole file: a REND_NACK_*
            // mentioned in a comment somewhere else must not count as
            // coverage.
            const fnStart = cf.indexOf('ConnectFailCode ConnectFail_ClassifyNackReason(uint8_t reason) {');
            assert(fnStart >= 0,
                'nack/verdict-parity: connect_fail.c defines ConnectFail_ClassifyNackReason');
            if (fnStart >= 0) {
                const fnEnd = cf.indexOf('\n}', fnStart);
                assert(fnEnd > fnStart, 'nack/verdict-parity: the mapper body is delimited');
                const body = cf.slice(fnStart, fnEnd);
                const named = new Set();
                const caseRe = /case\s+REND_NACK_([A-Z_]+)\s*:/g;
                let cm;
                while ((cm = caseRe.exec(body)) !== null) named.add(cm[1]);
                for (const name of Object.keys(NACK)) {
                    assert(named.has(name),
                        `nack/verdict-parity: ConnectFail_ClassifyNackReason has no case for REND_NACK_${name} -- the server can send it and the client would report the catch-all`);
                }
                assertEq(named.size, Object.keys(NACK).length,
                    'nack/verdict-parity: the mapper names exactly the reasons the server sends');
                // A default arm is REQUIRED, not optional: a server newer
                // than us will send a reason this table has no name for,
                // and falling out of the switch would return whatever the
                // compiler left behind.
                assert(/default\s*:/.test(body),
                    'nack/verdict-parity: the mapper has a default arm for an unnamed reason');
            }
        }
    }

    const stub = makeStubSocket();
    const fresh = () => { handle._resetSessions(); handle._resetRate(); stub.sent.length = 0; };
    const record = (want) => { seen.add(want); };

    // --- 1. BAD_VERSION: a version byte we do not speak. ------------------
    fresh();
    handle._onMessage(makeRegister(crypto.randomBytes(16), 5000, { version: 3 }),
        { address: '198.18.1.1', port: 5000 }, stub);
    assertEq(stub.sent.length, 1, 'nack/BAD_VERSION: exactly one reply');
    assertNackReason(stub.sent[0], NACK.BAD_VERSION, 'nack/BAD_VERSION: correct reason');
    record(NACK.BAD_VERSION);

    // --- 2. BAD_LENGTH: right version, wrong REGISTER size. ---------------
    fresh();
    handle._onMessage(makeRegister(crypto.randomBytes(16), 5000, { length: 30 }),
        { address: '198.18.1.2', port: 5000 }, stub);
    assertEq(stub.sent.length, 1, 'nack/BAD_LENGTH: exactly one reply');
    assertNackReason(stub.sent[0], NACK.BAD_LENGTH, 'nack/BAD_LENGTH: correct reason');
    record(NACK.BAD_LENGTH);

    // --- 3. BAD_TYPE: an unallocated type byte (a relay-era build). -------
    fresh();
    handle._onMessage(makeRegister(crypto.randomBytes(16), 5000, { type: 7, cookie: cookieFor('198.18.1.3', 5000) }),
        { address: '198.18.1.3', port: 5000 }, stub);
    assertEq(stub.sent.length, 1, 'nack/BAD_TYPE: exactly one reply');
    assertNackReason(stub.sent[0], NACK.BAD_TYPE, 'nack/BAD_TYPE: correct reason');
    record(NACK.BAD_TYPE);

    // --- 4. RATE_PREGATE: uncookied first-contact budget. -----------------
    // The first PREGATE_LIMIT uncookied requests are answered with
    // CHALLENGEs; the one after that is over budget.
    fresh();
    {
        const SRC = { address: '198.18.2.1', port: 6000 };
        const key = crypto.randomBytes(16);
        for (let i = 0; i < handle._preGateLimit; i++) {
            handle._onMessage(makeRegister(key, SRC.port), SRC, stub); // uncookied
        }
        assertEq(stub.sent.length, handle._preGateLimit,
            'nack/RATE_PREGATE: the in-budget requests were all answered (with CHALLENGEs)');
        assert(stub.sent.every((s) => isChallenge(s.buf)),
            'nack/RATE_PREGATE: in-budget uncookied requests get CHALLENGEs, not NACKs');
        stub.sent.length = 0;
        handle._onMessage(makeRegister(key, SRC.port), SRC, stub); // over budget
        assertEq(stub.sent.length, 1, 'nack/RATE_PREGATE: the over-budget request draws exactly one reply');
        assertNackReason(stub.sent[0], NACK.RATE_PREGATE, 'nack/RATE_PREGATE: correct reason');
        record(NACK.RATE_PREGATE);
    }

    // --- 5. RATE_IP: the per-IP COOKIED budget. ---------------------------
    fresh();
    {
        const SRC = { address: '198.18.2.2', port: 6100 };
        const key = crypto.randomBytes(16); // ONE key: avoids the creator quota
        const reg = () => handle._onMessage(regFrom(key, SRC.port, SRC.address, SRC.port), SRC, stub);
        for (let i = 0; i < handle._rateLimit; i++) reg();
        assertEq(stub.sent.length, handle._rateLimit,
            'nack/RATE_IP: the in-budget cookied requests were all answered (with DELIVERs)');
        stub.sent.length = 0;
        reg(); // over budget
        assertEq(stub.sent.length, 1, 'nack/RATE_IP: the over-budget request draws exactly one reply');
        assertNackReason(stub.sent[0], NACK.RATE_IP, 'nack/RATE_IP: correct reason');
        record(NACK.RATE_IP);
    }

    // --- 6. RATE_KEY: the per-SESSION-KEY budget. -------------------------
    // Must NOT be reachable by tripping the per-IP budget instead, so the
    // key budget is spent across SEVERAL source IPs, each staying inside
    // its own per-IP allowance. That is exactly the attacker model the
    // per-key cap exists for (many real cookie-capable addresses, one
    // room), and it is why the assertion is meaningful.
    fresh();
    {
        const key = crypto.randomBytes(16);
        const perIp = handle._rateLimit;
        const nIps = Math.ceil(handle._keyRateLimit / perIp);
        let spent = 0;
        for (let i = 0; i < nIps && spent < handle._keyRateLimit; i++) {
            const SRC = { address: `198.18.3.${10 + i}`, port: 6200 + i };
            for (let j = 0; j < perIp && spent < handle._keyRateLimit; j++) {
                handle._onMessage(regFrom(key, SRC.port, SRC.address, SRC.port), SRC, stub);
                spent += 1;
            }
        }
        assertEq(spent, handle._keyRateLimit, 'nack/RATE_KEY: the key budget was spent exactly, not overrun');
        stub.sent.length = 0;
        const LATE = { address: '198.18.3.99', port: 6299 };
        handle._onMessage(regFrom(key, LATE.port, LATE.address, LATE.port), LATE, stub);
        assertEq(stub.sent.length, 1, 'nack/RATE_KEY: the over-budget request draws exactly one reply');
        assertNackReason(stub.sent[0], NACK.RATE_KEY, 'nack/RATE_KEY: correct reason');
        // The late sender has spent NONE of its own per-IP budget, which
        // proves the refusal came from the per-KEY limiter and not the
        // per-IP one -- i.e. the reason byte is not merely plausible.
        assert(!handle._rateMap.has(LATE.address) || handle._rateMap.get(LATE.address).timestamps.length <= 1,
            'nack/RATE_KEY: the refusal was the per-KEY cap, not the per-IP cap');
        record(NACK.RATE_KEY);
    }

    // --- 7. KEY_QUOTA: MAX_NEW_KEYS_PER_IP live created keys. -------------
    fresh();
    {
        const SRC = { address: '198.18.4.1', port: 6300 };
        for (let i = 0; i < handle._maxNewKeysPerIp; i++) {
            handle._onMessage(regFrom(crypto.randomBytes(16), SRC.port, SRC.address, SRC.port), SRC, stub);
        }
        assertEq(handle._sessionMap.size, handle._maxNewKeysPerIp, 'nack/KEY_QUOTA: quota filled with real keys');
        stub.sent.length = 0;
        handle._onMessage(regFrom(crypto.randomBytes(16), SRC.port, SRC.address, SRC.port), SRC, stub);
        assertEq(stub.sent.length, 1, 'nack/KEY_QUOTA: the quota-exceeding request draws exactly one reply');
        assertNackReason(stub.sent[0], NACK.KEY_QUOTA, 'nack/KEY_QUOTA: correct reason');
        assertEq(handle._sessionMap.size, handle._maxNewKeysPerIp, 'nack/KEY_QUOTA: no extra key was bound');
        record(NACK.KEY_QUOTA);
    }

    // --- 8. TABLE_FULL: the table is full and every entry is PAIRED. ------
    fresh();
    {
        for (let i = 0; i < handle._maxSessions; i++) {
            handle._sessionMap.set(`full${i}`, {
                endpointA: { address: '203.0.113.1', port: 1000 + (i % 60000) },
                endpointB: { address: '203.0.113.2', port: 2000 + (i % 60000) },
                lastTouch: 1 + i, lastSeenA: 1 + i, lastSeenB: 1 + i,
                pushTo: null, pushAtMs: 0,
            });
        }
        const SRC = { address: '198.18.5.1', port: 6400 };
        stub.sent.length = 0;
        handle._onMessage(regFrom(crypto.randomBytes(16), SRC.port, SRC.address, SRC.port), SRC, stub);
        assertEq(stub.sent.length, 1, 'nack/TABLE_FULL: the refused request draws exactly one reply');
        assertNackReason(stub.sent[0], NACK.TABLE_FULL, 'nack/TABLE_FULL: correct reason');
        record(NACK.TABLE_FULL);
    }

    // --- 9. SESSION_FULL: both slots live, and you are neither. -----------
    fresh();
    {
        const key = crypto.randomBytes(16);
        const A = { address: '198.18.6.1', port: 6500 };
        const B = { address: '198.18.6.2', port: 6501 };
        const C = { address: '198.18.6.3', port: 6502 };
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        handle._onMessage(regFrom(key, B.port, B.address, B.port), B, stub);
        stub.sent.length = 0;
        handle._onMessage(regFrom(key, C.port, C.address, C.port), C, stub);
        assertEq(stub.sent.length, 1, 'nack/SESSION_FULL: the third party draws exactly one reply');
        assertNackReason(stub.sent[0], NACK.SESSION_FULL, 'nack/SESSION_FULL: correct reason');
        // And it is still a REFUSAL: neither slot moved.
        const entry = handle._sessionMap.get(key.toString('hex'));
        assertEq(entry.endpointA.address, A.address, 'nack/SESSION_FULL: slot A untouched');
        assertEq(entry.endpointB.address, B.address, 'nack/SESSION_FULL: slot B untouched');
        record(NACK.SESSION_FULL);
    }

    // Distinguishability is the whole point: nine conditions, nine
    // different bytes on the wire.
    assertEq(seen.size, 9, 'nack: all nine refusal conditions produced DISTINCT reason codes');

    // Every NACK echoes the sender's own session key and nothing else --
    // the anti-leak invariant. Re-proved on a fully-paired room, which is
    // the only reason whose fact is even arguably session-derived.
    {
        fresh();
        const key = crypto.randomBytes(16);
        const A = { address: '198.18.9.1', port: 7000 };
        const B = { address: '198.18.9.2', port: 7001 };
        const C = { address: '198.18.9.3', port: 7002 };
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        handle._onMessage(regFrom(key, B.port, B.address, B.port), B, stub);
        stub.sent.length = 0;
        handle._onMessage(regFrom(key, C.port, C.address, C.port), C, stub);
        const dec = decodeNack(stub.sent[0].buf);
        assert(dec.sessionKey.equals(key), 'nack: the frame echoes the SENDER-SUPPLIED session key');
        // Bytes [24..27] are reserved and must stay zero: this is where a
        // careless implementation would helpfully leak the occupying
        // endpoint back to a party that is not in the room.
        assert(stub.sent[0].buf.subarray(24, 28).equals(Buffer.alloc(4)),
            'nack: the reserved tail is zero -- no endpoint is disclosed to a non-member');
        assertEq(stub.sent[0].buf.readUInt8(7), 0, 'nack: reserved byte 7 is zero');
        // Nothing in the frame matches either occupant's address/port.
        const aPortBe = Buffer.alloc(2); aPortBe.writeUInt16BE(A.port);
        assertEq(stub.sent[0].buf.indexOf(aPortBe), -1,
            'nack: the occupying endpoint\'s port does not appear anywhere in the frame');
    }

    handle._resetSessions();
    handle._resetRate();
}

async function testNackAmplificationBound(handle) {
    // A NACK is an UNAUTHENTICATED reply to an UNAUTHENTICATED request, so
    // it is a reflector surface by construction and has to be bounded by
    // construction. Two INDEPENDENT bounds, proved separately.
    handle._resetSessions();
    handle._resetRate();
    const stub = makeStubSocket();

    // BOUND 1 -- PER FRAME: the response is strictly SMALLER than the
    // request, so the server is a net ATTENUATOR on every single frame and
    // no spray across any number of victims can amplify anything.
    assert(handle._nackLen < REGISTER_LEN,
        `amplification: NACK (${handle._nackLen} B) is smaller than the REGISTER it answers (${REGISTER_LEN} B)`);
    assert(handle._nackLen < POLL_LEN,
        `amplification: NACK (${handle._nackLen} B) is smaller than the POLL it answers (${POLL_LEN} B)`);
    // Tighter than the CHALLENGE, the only other unauthenticated reply.
    assert(handle._nackLen < CHALLENGE_LEN,
        `amplification: NACK (${handle._nackLen} B) attenuates harder than CHALLENGE (${CHALLENGE_LEN} B)`);
    // And measured on the wire, not just asserted about the constant.
    {
        const req = makeRegister(crypto.randomBytes(16), 5000, { version: 3 });
        handle._onMessage(req, { address: '198.18.20.1', port: 5000 }, stub);
        assertEq(stub.sent.length, 1, 'amplification: the probe drew exactly one frame');
        assert(stub.sent[0].buf.length < req.length,
            `amplification: measured reply ${stub.sent[0].buf.length} B < request ${req.length} B`);
    }

    // THE SHORT-REQUEST HOLE, which this test did not catch until it was
    // looked for. The smallest frame that reaches the BAD_TYPE branch is 8
    // bytes: onMessage's minimum length, our magic, a v2 version byte and
    // an unallocated type. Answering it with a 28-byte NACK is a 3.5x
    // AMPLIFIER — measured, not hypothetical. sendNack must refuse.
    //
    // Swept across every length below NACK_LEN so the guard cannot be
    // satisfied by a single special case, and each length is judged with a
    // fresh cooldown so a refusal is the LENGTH rule talking and not
    // BOUND 2 masking it.
    for (let len = 8; len < handle._nackLen; len++) {
        handle._resetRate();
        stub.sent.length = 0;
        const runt = Buffer.alloc(len);
        runt.writeUInt32BE(MAGIC, 0);
        runt.writeUInt8(VERSION, 4);
        runt.writeUInt8(99, 5); // unallocated type -> the BAD_TYPE branch
        handle._onMessage(runt, { address: '198.18.22.1', port: 5002 }, stub);
        const out = stub.sent.reduce((n, s) => n + s.buf.length, 0);
        assert(out <= len,
            `amplification: a ${len} B frame drew ${out} B — a reply must never exceed the request that caused it`);
    }

    // The boundary case is admitted, because it is the one the feature
    // exists for: a v1 REGISTER is exactly NACK_LEN bytes, so it still gets
    // its BAD_VERSION NACK (#87) at a gain of exactly 1.0 — which buys an
    // attacker nothing over sending the packet to the victim itself.
    handle._resetRate();
    stub.sent.length = 0;
    {
        const v1 = makeRegister(crypto.randomBytes(16), 5003,
            { version: V1_VERSION, length: V1_REGISTER_LEN });
        assertEq(v1.length, handle._nackLen, 'amplification: a v1 REGISTER is exactly NACK_LEN bytes');
        handle._onMessage(v1, { address: '198.18.23.1', port: 5003 }, stub);
        assertEq(stub.sent.length, 1, 'amplification: the v1 boundary case is still answered');
        assertNackReason(stub.sent[0], NACK.BAD_VERSION, 'amplification: v1 boundary -> NACK BAD_VERSION');
        assertEq(stub.sent[0].buf.length, v1.length,
            'amplification: the v1 boundary reply is exactly as large as the request, never larger');
    }

    // BOUND 2 -- PER VICTIM: aggregate egress toward ONE address is capped
    // at 1 NACK per NACK_MIN_INTERVAL_MS no matter how hard the attacker
    // pushes. This is the bound that matters, because concentrating
    // traffic on a chosen victim is the only thing a reflector is USEFUL
    // for. Bound 1 degrades gracefully under load; bound 2 does not
    // degrade at all.
    // Fresh cooldown AND fresh counters: BOUND 1's probe above already
    // spent one NACK, and the counts below are exact, not approximate.
    handle._resetRate();
    handle._resetMetrics();
    stub.sent.length = 0;
    const FLOOD = 2000;
    const VICTIM = { address: '198.18.21.7', port: 5001 };
    let reqBytes = 0;
    for (let i = 0; i < FLOOD; i++) {
        const req = makeRegister(crypto.randomBytes(16), VICTIM.port, { version: 3 });
        reqBytes += req.length;
        handle._onMessage(req, VICTIM, stub);
    }
    assertEq(stub.sent.length, 1,
        `amplification: ${FLOOD} spoofed requests produced ONE NACK toward the victim (cooldown holds)`);
    const respBytes = stub.sent.reduce((n, s) => n + s.buf.length, 0);
    assert(respBytes * 100 < reqBytes,
        `amplification: ${respBytes} B out for ${reqBytes} B in -- egress collapses under flood, it does not scale`);
    assertEq(handle._nackStats.sent, 1, 'amplification: the server counted exactly one NACK sent');
    assertEq(handle._nackStats.suppressed, FLOOD - 1,
        'amplification: every other refusal was suppressed by the cooldown and counted as such');

    // The cooldown is DELIBERATELY tighter than every request-path budget,
    // which is the only non-self-contradictory reading of "rate-limit the
    // NACK on the same budget as the request path": a rate-limit NACK
    // cannot draw the very bucket whose exhaustion caused it, so instead
    // the NACK budget is DOMINATED BY all three request budgets.
    const nacksPerSec = 1000 / handle._nackMinIntervalMs;
    assert(nacksPerSec < handle._rateLimit,
        `amplification: NACK rate ${nacksPerSec}/s is under the cookied per-IP budget ${handle._rateLimit}/s`);
    assert(nacksPerSec < handle._preGateLimit,
        `amplification: NACK rate ${nacksPerSec}/s is under the pre-gate budget ${handle._preGateLimit}/s`);
    assert(nacksPerSec < handle._keyRateLimit,
        `amplification: NACK rate ${nacksPerSec}/s is under the per-key budget ${handle._keyRateLimit}/s`);

    // A suppressed NACK must degrade to SILENCE -- i.e. exactly the
    // behaviour that shipped before #122 -- and never to some other frame.
    assert(stub.sent.every((s) => isNack(s.buf)),
        'amplification: nothing but NACKs came out; suppression degrades to silence, not to another frame type');

    handle._resetSessions();
    handle._resetRate();
    handle._resetMetrics();
}

async function testNackNeverAnswersOwnFrames(handle) {
    // Loop-freedom. The three SERVER -> CLIENT types are exactly the
    // frames this server EMITS, so answering one is the single shape that
    // could put two instances (or a server and a reflected copy of its own
    // output) into a ping-pong. They must draw NOTHING -- not a bounded
    // reply, nothing.
    handle._resetSessions();
    handle._resetRate();
    const stub = makeStubSocket();
    const SRC = { address: '198.18.30.1', port: 8000 };

    for (const [type, name] of [[TYPE_DELIVER, 'DELIVER'], [TYPE_CHALLENGE, 'CHALLENGE'], [TYPE_NACK, 'NACK']]) {
        handle._resetRate(); // fresh cooldown, so silence means silence
        stub.sent.length = 0;
        handle._onMessage(makeRegister(crypto.randomBytes(16), SRC.port,
            { type, cookie: cookieFor(SRC.address, SRC.port) }), SRC, stub);
        assertEq(stub.sent.length, 0, `loop-free: an inbound ${name} draws no reply of any kind`);
        assertEq(handle._sessionMap.size, 0, `loop-free: an inbound ${name} binds no state`);
    }

    // The decisive case: feed the server a REAL NACK of its own making.
    // If this ever draws a reply, two servers pointed at each other
    // ping-pong forever.
    handle._resetRate();
    stub.sent.length = 0;
    handle._onMessage(makeRegister(crypto.randomBytes(16), SRC.port, { version: 3 }), SRC, stub);
    assertEq(stub.sent.length, 1, 'loop-free: produced a genuine NACK to feed back');
    const realNack = stub.sent[0].buf;
    assert(isNack(realNack), 'loop-free: the captured frame really is a NACK');
    handle._resetRate();
    stub.sent.length = 0;
    handle._onMessage(realNack, SRC, stub);
    assertEq(stub.sent.length, 0,
        'loop-free: the server\'s own NACK, replayed at it, draws nothing -- no ping-pong is possible');

    handle._resetSessions();
    handle._resetRate();
    handle._resetMetrics();
}

async function testLostPairingPushObserved(handle) {
    // METRIC 1. When the second party fills a room, the server pushes an
    // UNSOLICITED DELIVER to the party already there. That push is
    // unacknowledged -- this protocol has no ACK, and the client only ever
    // emits REGISTER (Rendezvous_BuildPoll has zero production call sites)
    // -- so its loss was completely invisible.
    //
    // It is observable after all, because of one measured property of the
    // shipped client: A CLIENT STOPS RE-REGISTERING THE INSTANT A
    // REAL-ENDPOINT DELIVER LANDS (src/netplay/direct_p2p.c:4767-4768 for
    // the host, :2070-2071 for the joiner). So a REGISTER arriving from a
    // peer we already pushed to is that peer still behaving as UNPAIRED:
    // direct evidence the push was lost, not an inference.
    handle._resetSessions();
    handle._resetRate();
    handle._resetMetrics();
    const stub = makeStubSocket();

    // --- the push LANDS: the pushed-to peer goes quiet. -------------------
    {
        const key = crypto.randomBytes(16);
        const A = { address: '198.18.40.1', port: 9000 };
        const B = { address: '198.18.40.2', port: 9001 };
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        handle._onMessage(regFrom(key, B.port, B.address, B.port), B, stub);
        assertEq(handle._pushStats.pushed, 1, 'push: one pairing push was emitted and armed for measurement');
        assertEq(handle._pushStats.lost, 0, 'push: a peer that goes quiet is NOT counted as a lost push');
    }

    // --- the push is LOST: the pushed-to peer keeps REGISTERing. ---------
    {
        const key = crypto.randomBytes(16);
        const A = { address: '198.18.41.1', port: 9100 };
        const B = { address: '198.18.41.2', port: 9101 };
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        handle._onMessage(regFrom(key, B.port, B.address, B.port), B, stub);
        assertEq(handle._pushStats.pushed, 2, 'push: second pairing push armed');
        // A -- the peer we pushed to -- comes back asking. A client that
        // received the push would have cancelled its resender.
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        assertEq(handle._pushStats.lost, 1, 'push: the pushed-to peer re-REGISTERing IS a lost push');

        // One push yields exactly ONE sample: a peer that keeps hammering
        // must not inflate the loss rate.
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        assertEq(handle._pushStats.lost, 1, 'push: further REGISTERs from the same peer do not double-count');
    }

    // --- a DIFFERENT peer's REGISTER is not evidence about our push. -----
    {
        const key = crypto.randomBytes(16);
        const A = { address: '198.18.42.1', port: 9200 };
        const B = { address: '198.18.42.2', port: 9201 };
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        handle._onMessage(regFrom(key, B.port, B.address, B.port), B, stub);
        const lostBefore = handle._pushStats.lost;
        // B was NOT the pushed-to peer (A was). B re-REGISTERing says
        // nothing about whether A got its push.
        handle._onMessage(regFrom(key, B.port, B.address, B.port), B, stub);
        assertEq(handle._pushStats.lost, lostBefore,
            'push: a REGISTER from the peer we did NOT push to is not counted');
    }

    // --- HAIRPIN EXCLUSION, and it is correctness, not tidiness. ---------
    // When the pushed endpoint carries the recipient's OWN address, the
    // client's self-DELIVER gate (direct_p2p.c:4760-4765 host,
    // :2048-2052 joiner) deliberately does NOT cancel the resender. Such a
    // peer keeps REGISTERing whether or not the push arrived, so counting
    // it would manufacture loss that did not happen -- the H-1 shape.
    {
        const key = crypto.randomBytes(16);
        const A = { address: '198.18.43.1', port: 9300 };
        const B = { address: '198.18.43.1', port: 9301 }; // SAME address
        const pushedBefore = handle._pushStats.pushed;
        const lostBefore = handle._pushStats.lost;
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        handle._onMessage(regFrom(key, B.port, B.address, B.port), B, stub);
        assertEq(handle._pushStats.pushed, pushedBefore, 'push: a hairpin push is not counted as measurable');
        assert(handle._pushStats.excluded > 0, 'push: the hairpin push was counted as EXCLUDED, visibly');
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        assertEq(handle._pushStats.lost, lostBefore,
            'push: a hairpin peer that keeps REGISTERing is NOT reported as a lost push');
    }

    handle._resetSessions();
    handle._resetRate();
    handle._resetMetrics();
}

async function testPairingToPunchHistogram(handle) {
    // METRIC 2. On the host, the DELIVER that cancels the resender is the
    // SAME DELIVER that spawns the punch: src/netplay/direct_p2p.c:4788
    // sets DIRECT_P2P_FALLBACK_BILATERAL_PUNCH and :4796 spawns the punch, in
    // the same function, off the same frame. So
    //
    //   time(pairing -> the pushed-to peer's re-REGISTER)
    //     == time(pairing -> that peer starts punching)
    //
    // exactly. A landed push makes it 0 ms; a LOST push makes it one whole
    // re-REGISTER interval, and the host's interval DEFAULTS TO 5000 ms
    // (src/port/config/config.c:111, floor 1000 ms at direct_p2p.c:3244).
    // One lost datagram therefore costs up to five seconds of connect
    // latency, invisibly. That is the distribution this histogram exists
    // to expose, and why a single mean would not do.
    handle._resetSessions();
    handle._resetRate();
    handle._resetMetrics();
    const stub = makeStubSocket();
    const buckets = handle._pushHistBucketsMs;
    assert(Array.isArray(buckets) && buckets.length > 0, 'hist: the server exposes its bucket edges');

    // Drive one sample into each bucket by ageing the recorded push time,
    // which is the same technique the stale-slot tests use for lastSeen.
    const wanted = [];
    for (let i = 0; i < buckets.length; i++) {
        wanted.push(i === 0 ? Math.floor(buckets[0] / 2)
                            : Math.floor((buckets[i - 1] + buckets[i]) / 2));
    }
    wanted.push(buckets[buckets.length - 1] + 5000); // the overflow bucket

    for (let i = 0; i < wanted.length; i++) {
        const delay = wanted[i];
        const key = crypto.randomBytes(16);
        const hexKey = key.toString('hex');
        const A = { address: `198.18.50.${i + 1}`, port: 9400 + i };
        const B = { address: `198.18.51.${i + 1}`, port: 9500 + i };
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
        handle._onMessage(regFrom(key, B.port, B.address, B.port), B, stub);
        const entry = handle._sessionMap.get(hexKey);
        assert(entry && entry.pushTo, `hist: sample ${i} armed a push`);
        entry.pushAtMs -= delay; // age the push by `delay` ms
        handle._onMessage(regFrom(key, A.port, A.address, A.port), A, stub);
    }

    assertEq(handle._pushStats.lost, wanted.length, 'hist: every sample was recorded');
    // One sample landed in each bucket, in order -- so the histogram
    // actually discriminates rather than dumping everything in one cell.
    for (let i = 0; i < handle._pushStats.hist.length; i++) {
        assertEq(handle._pushStats.hist[i], 1, `hist: bucket ${i} holds exactly its one sample`);
    }
    assertEq(handle._pushStats.hist.length, buckets.length + 1,
        'hist: there is an overflow bucket above the largest edge');
    assert(handle._pushStats.maxMs >= buckets[buckets.length - 1],
        'hist: the max tracks the slowest observed pairing->punch delay');

    handle._resetSessions();
    handle._resetRate();
    handle._resetMetrics();
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
// +5 (task #122): nackPerReason, nackAmplificationBound,
// nackNeverAnswersOwnFrames, lostPairingPushObserved,
// pairingToPunchHistogram.
const EXPECTED_TESTS = 39; // +3 (task #130): portReclaimBudgetExhausted,
// liveHostSlotNotHijackable, unobservedSlotMaximallyProtected. The first is
// split OUT of joinerPortReclaimSameIp rather than new coverage -- #130 added
// two REGISTERs from that test's joiner IP and the combined test crossed the
// 10/s cookied per-IP budget, which drops packets instead of failing loudly.

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
        // #122: the metric counters are shared process state too. Without
        // this, testRateMapBounded's thousands of induced NACKs leak into
        // testNackAmplificationBound's counts and the amplification proof
        // reads a number it did not produce.
        H._resetMetrics();
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
        await runTest('joinerPortReclaimSameIp', () => testJoinerPortReclaimSameIp(handle));
        await runTest('portReclaimSlotA', () => testPortReclaimSlotA(handle));
        await runTest('portReclaimBudgetExhausted', () => testPortReclaimBudgetExhausted(handle));
        await runTest('liveHostSlotNotHijackable', () => testLiveHostSlotNotHijackable(handle));
        await runTest('unobservedSlotMaximallyProtected', () => testUnobservedSlotMaximallyProtected(handle));

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
        await runTest('keyBudgetCoversMultiJoinerRoom', () => testKeyBudgetCoversMultiJoinerRoom(handle));
        await runTest('retiredRelayTypesAreUnknown', () => testRetiredRelayTypesAreUnknown(handle));

        // --- #122: typed refusals, their amplification bound, the metrics ---
        await runTest('nackPerReason', () => testNackPerReason(handle));
        await runTest('nackAmplificationBound', () => testNackAmplificationBound(handle));
        await runTest('nackNeverAnswersOwnFrames', () => testNackNeverAnswersOwnFrames(handle));
        await runTest('lostPairingPushObserved', () => testLostPairingPushObserved(handle));
        await runTest('pairingToPunchHistogram', () => testPairingToPunchHistogram(handle));

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
