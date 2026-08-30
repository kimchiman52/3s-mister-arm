# 3SX rendezvous server

## Overview

Single-purpose UDP endpoint-exchange service that pairs two peers behind
symmetric NATs so they can run a bilateral STUN hole punch. It does NOT do
matchmaking, presence, lobby, chat, ranking, persistence, or TLS.

It does do a little more than "swap two endpoints", and an operator needs to
know about all of it:

- a **return-routability cookie** handshake (S4c), so a source-spoofing sender
  can never bind a slot;
- **three independent rate budgets** — cookied per-IP, uncookied per-IP, and
  per-session-key;
- **per-slot staleness reclaim**, which is what makes cancel-then-re-host and
  the joiner's second attempt work;
- **caps** on the session table, on live keys per creator IP, and on every
  rate-bucket map.

All state is in memory. There is no database, no admin socket, and no runtime
introspection interface — a restart drops every live session and every
outstanding cookie, and clients recover on their own (one extra challenge
round each).

The relay extension (S5, packet types 5-8) was **deleted**, client and server.
Those type values are unallocated again and fall through the unknown-type drop.

## Wire protocol

Magic `'3SXR'` (`0x33535852`), **version 2**, big-endian fields, `udp4` only.

| type | name | direction | length |
|---|---|---|---|
| 1 | `REGISTER` | client to server | 36 bytes |
| 2 | `DELIVER` | server to client | 32 bytes |
| 3 | `POLL` | client to server | 36 bytes |
| 4 | `CHALLENGE` | server to client | 32 bytes |

Common header is `magic(4) version(1) type(1) reserved(2) session_key(16)`.
`REGISTER` then carries `my_public_port(2)`, reserved, and an 8-byte **cookie
tail at offset 28**; `POLL` has the same 36-byte shape with the same cookie
tail. `DELIVER` carries `peer_ip(4) peer_port(2) reserved(2)`, all zero when
the server has no peer yet. `CHALLENGE` carries the echoed session key plus the
8-byte cookie the client must replay.

The server reads the peer endpoint from the **UDP source address and port**,
never from packet fields. `my_public_port` is a sanity check only: a mismatch
is logged and the packet is still processed.

Version 2 is a deliberate, authorized breaking change. A v1 client's 28-byte
`REGISTER` is dropped on the version byte — logged under a throttle, no reply,
no state — and its budget expiry surfaces as `RENDEZVOUS_DOWN` on the client.

**Spec pointer, with a caveat.**
[`docs/plan-bilateral-hole-punch.md` Decision 2](../../docs/plan-bilateral-hole-punch.md#2-protocol-for-endpoint-exchange)
is the original design note, but its wire-format block is **stale**: it still
documents `version = 1`, 28-byte `REGISTER`/`POLL`, no cookie tail, no
`CHALLENGE` type, and a 60-second entry expiry. Read it for the *why* of
endpoint exchange, not for the frame layout. The current wire spec is
`docs/plan-netplay-connection.md` §6.4 (S4c) and §6.5 (version interlock) —
noting that even §6.4's two rate-limit numbers have since been superseded by
the source (see Operator notes below). **`rendezvous-server.js` is the
authority.**

## Run locally

```
node rendezvous-server.js 3478
```

Node 18+. Port defaults to 3478 if omitted; pass `0` to bind an ephemeral port
(the bound port is logged). A port argument outside `0..65535`, or a
non-integer, exits with status 2.

## Test

```
node __test_protocol.js
```

The test spins up an in-process server on an ephemeral port and drives it with
mock UDP clients, including injected source addresses that loopback cannot
produce. It asserts an exact test count, so a skipped or aborted test fails the
run rather than silently shrinking coverage. Success prints
`protocol test passed (31/31 tests)` and exits 0.

## Deploy

```
./deploy.sh user@host:/opt/rendezvous-server
```

Then on the remote:

```
sudo chown -R rendezvous:rendezvous /opt/rendezvous-server
sudo cp /opt/rendezvous-server/rendezvous-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl restart rendezvous-server
```

Skip the `systemctl` steps if the unit file itself did not change; the restart
alone is enough to pick up new server code.

**The chown is not optional, and it is easy to get wrong.** `deploy.sh` rsyncs
with `--no-owner --no-group`, which suppresses a genuinely bad failure mode:
plain `-a` maps owner and group by *name*, and the developer's local uid/gid
(501:20 on macOS, where gid 20 is `staff` locally but `dialout` on this host)
does not exist remotely, so `-a` silently rewrites the tree to a bogus
`501:dialout`. But `--no-owner` does not preserve ownership either — rsync
writes a temp file and renames over the target, so every file it actually
**replaces** ends up owned by the transferring ssh user (`root`), while
untouched files keep their previous owner. Without the chown the tree drifts
into a mix of owners. The service still runs (the files are world-readable and
it only needs to read them), so this fails silently rather than loudly.

On a first install the service account has to exist before the chown:

```
sudo useradd -r -s /usr/sbin/nologin rendezvous
```

The unit runs as `User=rendezvous` with `Restart=always`,
`NoNewPrivileges`, `PrivateTmp`, and `ProtectSystem=strict`.

## Operator notes

### Sessions and TTL

- **Session TTL is 10 minutes** (not the 60 s of the original design). It is
  measured from the last *successful* `REGISTER` or `POLL` on that key — the
  entry's `lastTouch`. A live host re-REGISTERs about every 5 s, so the TTL
  really only covers lost refreshes and bounds how long a room code pasted into
  a chat stays pair-able.
- A `REGISTER` that is *refused* (creator quota, table full of paired sessions,
  third-party drop) does **not** refresh the TTL. A `POLL` refreshes it whenever
  the key exists, even from a cookied source occupying neither slot.
- The sweeper runs every **5 seconds** and logs only when it actually evicts.
- **`MAX_SESSIONS` is 4096.** At the cap a new key evicts the **oldest unpaired
  singleton**; paired sessions are never evicted. If everything is paired, the
  new `REGISTER` is refused with a WARN.
- **A single source IP may hold at most 4 keys it created.** The fifth is
  refused with a WARN naming the count. A legitimate client holds one (briefly
  two across a re-key).

### The cookie handshake

- Cookie is 8 bytes: `SHA-256(secret || "addr:port:slot")[0..7]`, compared in
  constant time. The 32-byte secret is drawn at process start and never leaves
  the process.
- `slot = floor(wall_clock_ms / 60000)`. **The current and previous slot both
  validate**, so a cookie lives 60-120 seconds.
- **The round trip:** a client's first `REGISTER`/`POLL` carries an all-zero
  cookie tail. The server replies with exactly one 32-byte `CHALLENGE` bound to
  that source address and port, and **binds no state at all** — no slot, no
  creator quota, no session naming, no cookied rate budget. The client echoes
  the cookie on its next request and is then processed normally.
- A **restart invalidates every outstanding cookie**. Expect a burst of
  `[CHALLENGE]` aggregate lines after any `systemctl restart`; that is normal
  and costs each live client one extra round trip.
- Exactly **one** challenge is the normal opening of a healthy session. Repeated
  challenges to a client that is already echoing a cookie mean its source port
  is being reassigned between datagrams (NAT churn) or the frame is being
  tampered with.
- `CHALLENGE` is 32 bytes answering a 36-byte request: amplification factor
  0.89. The server is a net attenuator and is not usable as a reflector.

### The three rate limiters

All three are sliding windows with a **1-second** window, so "per window" and
"per second" are the same number. Each logs a WARN on the **first** drop for a
given IP or key and then goes silent for that IP/key until its bucket is
evicted — so one WARN line can represent an arbitrary number of drops.

| budget | limit | governs | first-drop log |
|---|---|---|---|
| pre-gate, per source IP | **100/s** | uncookied / stale-cookied first contact; bounds `CHALLENGE` emission and nothing else | `pre-gate: dropping UNCOOKIED packets from <ip>` |
| cookied, per source IP | **10/s** | everything that has already proven return routability | `rate-limit: dropping COOKIED packets from <ip>` |
| per session key | **30/s** | all cookied traffic for one room code, summed across every source IP | `key-rate-limit: dropping packets for key=<4 hex>...` |

Why the split matters operationally: the cookied and uncookied budgets are
**separate buckets**, so no amount of spoofed first-contact traffic can starve
a real client's established budget. The per-key budget is deliberately 3x the
per-IP budget, so one saturating cookied IP can take at most a third of a
room's budget; a legitimate busy room (host liveness plus up to six racing
dialers) peaks around 13/s and still fits underneath a saturating attacker.

- Every rate-bucket map is capped at **8192 entries** (2 x `MAX_SESSIONS`) with
  O(1) least-recently-used eviction. Eviction **fails open** — the evicted
  source's budget resets. These buckets are a throttle; the cookie is the
  actual gate.
- Nothing is allocated for a packet that fails the magic, version, or length
  checks, so pure junk from many spoofed sources costs zero map entries.
- The bucket sweeper runs every **60 seconds** and drops buckets with no
  in-window traffic that have been quiet for 60 windows (60 s). It logs only
  when it evicts, reporting all three map sizes as `live=<ip>/<pre-gate>/<key>`.

### Slot reclaim — why cancel-then-re-host works

Slots are **first-come, not role-based**: nothing in a `REGISTER` frame says
"host" or "joiner", so whichever side registers first takes slot A. Every
reclaim rule below is therefore symmetric across both slots.

- A slot is **stale after 30 seconds** of silence from that exact endpoint
  (a live host refreshes every ~5 s, a live joiner every ~500 ms).
- **Same source IP, new source port** is the discriminator for every reclaim. A
  different-IP third party can never touch a live slot.
- Reclaims you will see in the journal, all logged at INFO with a `[RECLAIM]`
  prefix:
  - `promote B->A` — a re-hosting client landed in the joiner slot while its own
    stale endpoint still held the host slot; it is promoted and the joiner slot
    is freed.
  - `host slot` — same-IP claimant replaces a stale host slot (a stale joiner
    slot is cleared at the same time so the reclaimed host is not handed a dead
    endpoint).
  - `joiner slot` — a stale joiner slot is replaced and the host is re-notified.
  - `port ... slotA` / `port ... slotB` — the same-IP-new-port retry against a
    slot that is **not** stale. This is the joiner's automatic second attempt,
    which rebinds a fresh local socket on purpose; without this arm the retry
    was ignored for the entire connect budget. Capped at **8 reclaims per
    session**, after which the slot falls back to the plain staleness rule.
- A claimant must still pass the cookie gate at its *new* address and port, so
  none of this grants anything to a spoofed source.
- If both slots are live with different endpoints, a third `REGISTER` is dropped
  with a WARN (`for full session key=... — ignored`) and no reply.

### Reading the journal

```
journalctl -u rendezvous-server -f
```

INFO goes to stdout, WARN to stderr; both land in the journal. Every line is
`[<ISO timestamp>] INFO|WARN <message>`. Session keys are truncated to the
first 4 hex characters.

**Normal traffic (INFO):** `bound udp4 ...` at startup; `[REGISTER] from
<ip>:<port> key=... a=set b=null` per accepted register, with `a`/`b` showing
slot occupancy; `[POLL] from ...` likewise; `[DELIVER] push to ...` when a pair
completes and the waiting peer is notified; `[RECLAIM] ...` as above;
`session sweep: evicted N, live=M`; `rate sweep: ...`; `shutting down (SIGTERM)`.

**Aggregated (INFO):** `[CHALLENGE] uncookied/stale request: N since last
report (latest: ...)`. Reported at most once per 10 seconds with a running
count — a flood cannot turn this into an unbounded console write, and a
blocking stdout would itself be a denial of service.

**Refusals and malformed input (WARN):** `REGISTER NAT mismatch` (logged,
packet still processed); the three rate-limit first-drop lines above;
`REGISTER ... dropped — IP already holds n/4 live keys`;
`REGISTER ... dropped — session table full of PAIRED sessions`;
`session table full — evicted oldest unpaired singleton`;
`REGISTER ... for full session key=... — ignored`. Malformed frames are
aggregated on the same 10-second throttle under fixed reason strings:
`drop: short packet`, `drop: unsupported version`, `drop: bad REGISTER length`,
`drop: bad POLL length`, `drop: unexpected CHALLENGE`, `drop: unexpected
DELIVER`, `drop: unknown type`.

**Process-level (WARN):** `socket error: ...` and `handler error: <stack>`. A
handler exception is caught per datagram and never takes the process down; if
these repeat, that is a real bug and worth capturing.

### Silent drops — what an operator cannot see, and what a client cannot see

Two different blind spots, both worth knowing at 2am.

**Genuinely unlogged, server side:**

- **Magic mismatch.** A datagram whose first four bytes are not `'3SXR'` is
  dropped with no log line of any kind. This is per spec ("if not
  `0x33535852`, drop") and is the only path in the server that drops without
  ever logging anything.
- **Repeat rate-limit drops.** After the first WARN for an IP or key, all
  further drops from that IP or key are unlogged until its bucket is evicted.
- **Throttled reasons between reports.** The aggregated `drop:` and
  `[CHALLENGE]` reasons emit at most one line per 10 seconds; the count in the
  next line is the only evidence the rest happened.

If you are debugging a specific client and see nothing at all in the journal,
the packet either failed the magic check, or the client is being dropped under
an already-warned bucket, or it never arrived. Check first that traffic is
reaching the host at all before concluding anything about the server.

**Indistinguishable, client side (work item #122).** Every server-side refusal
below produces the same thing on the wire — total silence:

- version mismatch, bad frame length, unknown type;
- pre-gate, per-IP, and per-key rate-limit drops;
- creator-quota refusal and paired-table-full refusal;
- the third-party drop on a full session.

There is no NACK frame. A client cannot tell these apart and reports the
generic `RENDEZVOUS_DOWN` (or `RENDEZVOUS_NOPAIR` once a cookie has bound
without being re-challenged) for all of them. `CHALLENGE` is the single
server-side condition a client can positively observe. **The journal on this
host is the only place the real reason exists** — when a user reports
"matchmaking is down", the answer is here or nowhere.

### Test hooks

`start()` returns an object of underscore-prefixed handles (`_version`,
`_sessionTtlMs`, `_sessionMap`, `_cookieFor`, `_onMessage`, and friends) used
by `__test_protocol.js` to inject packets from arbitrary source addresses and
to assert the tunables. These are reachable only by `require`-ing the module
in-process. They are **not** an operator interface: the deployed unit runs the
file as a CLI entrypoint, and there is no way to reach them on a running
server.
