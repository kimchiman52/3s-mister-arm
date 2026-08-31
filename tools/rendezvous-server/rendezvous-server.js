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
// Task #122: type 5 is NACK — a server->client refusal carrying a reason
// code. Types 5..8 were the S5 relay extension (RELAY_REQ / RELAY_GRANT /
// RELAY_PIN / RELAY_PIN_ACK); the relay was deleted, which freed them, and
// 5 is now claimed. 6..8 remain unallocated and still fall through
// onMessage's `drop: unknown type` branch. The version byte is UNCHANGED
// at 2: REGISTER, POLL, DELIVER and CHALLENGE are byte-for-byte what v2
// always was, and a v2 client that does not know type 5 ignores it exactly
// as it ignored types 5..8 before (an unrecognised type byte reaches no
// branch of the client's race receive switch — see the compatibility note
// on NACK_LEN below).
const TYPE_NACK = 5;

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
// THE FIX, AND WHY IT IS NOT A SLOT HIJACK. Reclaim EITHER slot for a
// REGISTER from the SAME PUBLIC ADDRESS on a new port. This is the exact
// discriminator already used for the HOST slot in the H1 reclaim above --
// same IP, new NAT source port, same session key -- generalised to both
// slots, and it deliberately does NOT relax staleness for anyone else:
//   * A DIFFERENT-IP third party still cannot touch a live slot. That is
//     the property __test_protocol.js testStaleJoinerSlotReplaced asserts
//     ("LIVE slot B not replaced by third party"), and it still holds.
//
// BOTH SLOTS, NOT JUST B -- this is load-bearing, not symmetry for its own
// sake. Slots are FIRST-COME, not role-based: nothing in the REGISTER
// frame identifies a host or a joiner (handleRegister parses only the
// session key, my_public_port and the cookie), so whichever side binds
// first takes slot A. A B-only rule therefore misses every room where the
// joiner won slot A, and the lockout is completely unrepaired there --
// measured 13/13 on the natmatrix rig: all 11 failing reps with the joiner
// in slot A showed 16 ignored REGISTERs and zero reclaims, while both reps
// with the host in slot A reclaimed correctly. The host's ~5 s
// re-REGISTER cadence against the joiner's ~500 ms means joiner-first is
// not a rare ordering; forcing host-first in the rig took an 8 s lead.
//
// If BOTH slots carry the source address (both parties behind one NAT),
// slot B is preferred, arbitrarily but deterministically. That is the
// hairpin room, which the client-side hairpin bypass already refuses
// before either side REGISTERs, so the choice is unreachable in practice
// and exists only so the branch order is total.
//   * The claimant must still pass the S4 return-routability gate at its
//     NEW (address, port). cookieForSlot() is unchanged and still does NOT
//     take the session key, so this grants NO new power to a spoofed
//     source: a spoofer never receives the CHALLENGE and so never obtains
//     a valid cookie for the endpoint it is claiming. What S4 closed --
//     unproven sources binding, evicting or naming a session -- stays
//     closed.
// THE RESIDUAL, CORRECTED (task #130). An earlier revision of this comment
// said the residual was a same-IP party that "can already deny the room
// today by simply REGISTERing into slot B first", and that the cap "bounds
// the churn rather than carrying a security property". Both understated it,
// and the correction is why PORT_RECLAIM_MISSED_REFRESHES exists below.
//
// What a reclaimer actually proves is: same public address, knowledge of the
// 16-byte session key, and return-routability AT ITS OWN NEW ENDPOINT.
// cookieForSlot() does not mix the session key, so the cookie says nothing
// about WHICH party is claiming -- it does NOT prove original-party identity.
// The as-shipped rule therefore did not merely let a co-located party win a
// race for an EMPTY slot; it let one repoint a LIVE, OCCUPIED slot and have
// the server re-notify the paired peer with the attacker's endpoint. Before
// #105 a live occupied slot survived (the third-party drop); after #105 and
// before #130 it was actively hijackable, slot A -- the host -- included.
// That is a strictly new capability, not a restatement of the pre-existing
// first-come property, and the cap bounded only how OFTEN it could be used.
//
// Scope of the correction, stated so it is not over-read: off-path and
// different-IP attackers were never in this window and are not what #130
// closes. They stay blocked by return-routability at the source address
// (testStaleJoinerSlotReplaced / testJoinerPortReclaimSameIp assert exactly
// that, and still do). The downstream punch stays gated by the S4a token and
// the S4b nonce. The room code is a 128-bit session key
// (Rendezvous_DeriveSessionKey, src/netplay/rendezvous.c:135) and is not
// brute-forcible against the 10/s cookied per-IP cap. #130 narrows ONE
// thing: the same-IP trust boundary, which had been widened from "may win an
// unoccupied slot" to "may evict a live one".
//
// THE CAP. The client needs exactly ONE reclaim per join (attempt 2 of 2),
// so a small multiple covers repeated user-initiated joins against the same
// still-live room code within SESSION_TTL_MS without letting a co-located
// peer flap the slot indefinitely. It bounds frequency only: the staleness
// precondition below, not this cap, is what carries the security property.
const MAX_PORT_RECLAIMS = 8;

// Task #130 -- the staleness precondition on the two port-reclaim arms.
//
// WHY A PRECONDITION AT ALL. See the correction above: without one, both
// arms fire on same-IP-plus-budget alone and repoint a LIVE slot.
//
// WHY THIS IS NOT SIMPLY "PICK A WINDOW". The bug #105 exists to fix WAS a
// badly-chosen window. SLOT_STALE_MS is 30 s, calibrated (see its comment)
// against the HOST's <= 5 s advertise cadence -- "6+ missed host refreshes".
// Applied to a JOINER slot refreshed every ~500 ms, that same 30 s is 60
// missed refreshes, and it exceeded the joiner's entire 31,800 ms derived
// deadline (src/netplay/direct_p2p.c:6114-6120), so the retry could never
// re-register and the mechanism was inert in exactly the lossy conditions it
// was built for. The defect was never the NUMBER; it was applying a
// host-calibrated constant to a slot whose occupant runs 10x faster.
//
// WHY NO FIXED WINDOW W CAN WORK. Take the two constraints literally:
//   * To protect a LIVE HOST slot, W must exceed the host's advertise
//     cadence (5000 ms default, src/port/config/config.c:111) by enough to
//     survive a lost refresh -- otherwise a single dropped REGISTER over a
//     multi-minute advertise opens a 5 s strike window, and over minutes
//     that is not a residual, it is a certainty. Even ONE missed refresh of
//     margin demands W > 10000.
//   * To preserve #105, the reclaim must land inside attempt 2's signalling
//     leg, which is signal_budget_ms = 8000 (src/netplay/direct_p2p.c:4126)
//     and ends the re-REGISTER stream (direct_p2p.c:1958-1959). So W < 8000.
// 10000 < W < 8000 is empty. A single constant cannot serve both slots
// because the two slots' occupants do not share a cadence.
//
// THE DERIVATION. Keep the standard SLOT_STALE_MS already encodes -- six
// missed refreshes -- and express it in units of the slot's OWN OBSERVED
// cadence instead of the host's assumed one. That is the whole fix: the
// factor is not new, it is SLOT_STALE_MS / the host cadence it was written
// against, 30000 / 5000 = 6, and tools/rendezvous-server/check_reclaim_window.py
// reads both out of their real definitions and fails if they stop agreeing.
//
// What it yields, per slot, with no new magic numbers:
//   * A JOINER-cadence slot (~500 ms observed) becomes reclaimable after
//     6 x 500 = 3000 ms of silence. Attempt 2 re-REGISTERs every 500 ms for
//     8000 ms, and the dead slot's lastSeen is FROZEN (its socket is gone),
//     so the threshold is crossed ~2-2.5 s into attempt 2's race and the
//     reclaim still leaves >= 5 s of race plus the RACE_HARD_CAP tails.
//     #105 keeps working, with margin, which testJoinerPortReclaimSameIp
//     and testPortReclaimSlotA now assert as a TIMED property.
//   * A HOST-cadence slot (~5000 ms observed) needs 6 x 5000 = 30000 ms,
//     which is SLOT_STALE_MS. So over host-cadence slots these arms grant no
//     power the pre-existing staleness rule did not already grant. The live
//     host hijack is closed outright, not narrowed.
//   * A slot whose cadence has NOT been observed yet (one REGISTER, or one
//     that was just repointed) is treated as maximally protected. Unknown
//     must mean SLOT_STALE_MS, never 0 -- defaulting an unmeasured slot to
//     "reclaimable" would reopen the whole finding for the first seconds of
//     every room, which is precisely when a host is advertising.
// The ceiling is SLOT_STALE_MS rather than a new constant for a reason: past
// that point the pre-existing bStale arm reclaims anyway, so a larger
// threshold could not deny anything -- it would only be unreachable code
// pretending to be a control.
//
// RESIDUAL, STATED PLAINLY. The cadence is MEASURED, so it is only as good
// as what the slot's occupant has shown. A slot repointed by a legitimate
// reclaim resets to "unobserved" and is therefore maximally protected, which
// is the safe direction and also ends the flap war the cap used to bound
// alone. But an occupant that legitimately refreshes more slowly than it
// used to (a client tuned via CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS,
// which has a 1000 ms floor and NO ceiling) is protected only to 6x whatever
// it has actually demonstrated. That is a config-dependent residual, not a
// silent one: check_reclaim_window.py gates the SHIPPED default.
const PORT_RECLAIM_MISSED_REFRESHES = 6;
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
// MECHANICALLY CHECKED (task #123). Everything below is derived from
// constants that live in the C CLIENT, and the client and this server
// DEPLOY INDEPENDENTLY — a cadence tune ships in a release ZIP without
// rebuilding, restarting or notifying a long-lived VPS process, and no
// translation unit and no module sees both a C literal and a JS const, so
// neither a _Static_assert nor a JS assertion can reach across. The
// file:line pointers below are navigation aids and WILL drift; the
// arithmetic is held by tools/rendezvous-server/check_key_rate_budget.py,
// which reads every value from its real definition and runs in
// tools/gates/run-gates.sh. If you change a client cadence, that gate is
// what tells you this constant went stale — and a stale constant here is
// not a build break, it is a production room that DoSes itself.
//
// What charges this bucket, from the shipped client:
//   * joiner REGISTER resend inside the punch race — 500 ms
//     (src/netplay/direct_p2p.c:1958, `(now - signal_last_send) >=
//     500u`) => 2/s PER JOINER, for signal_budget_ms (8 s default,
//     direct_p2p.c:4007-4008 / src/port/config/config.c:105).
//   * host re-REGISTER worker — CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_
//     INTERVAL_MS, default 5000 ms (config.c:111), floor 1000 ms
//     (direct_p2p.c:3244) => 1/s worst case. This leg must NEVER be
//     starved: losing it is precisely what reclaims a live room. The host
//     runs NO signalling leg inside the race (direct_p2p.c:3343 sets
//     cfg.signal_leg = false for the host — the DELIVER that started that
//     thread already proves it is paired).
//   * one challenge-triggered immediate resend per side per cookie
//     rotation (COOKIE_ROTATE_MS >= 60 s), since the client answers a
//     CHALLENGE at once (direct_p2p.c:1977).
//
// N, the number of simultaneous dialers on ONE key. The session key is
// derived from the HOST's public endpoint (direct_p2p.c:3984, via
// Rendezvous_DeriveSessionKey), i.e. the key IS the room code — everyone
// who pastes that code lands in the same bucket, and each one starts its
// 2/s leg immediately, without waiting to be accepted (cfg.signal_leg at
// direct_p2p.c:4059 keys only on "have signal URL + have session key", not
// on pairing). The server's two-slot policy silences dialers 2..N at
// DISPATCH, but they have already charged this bucket — the gate runs
// upstream of dispatch. Codes are pasted into a group chat and stay live
// for SESSION_TTL_MS = 10 min, so several people racing for the single
// free slot is the normal case, not an attack. N = 6 is the design point:
// more than any 2-player room can consume, enough to cover a code dropped
// into a small active channel plus the 8 s tails of losing dialers
// overlapping the next wave, and past it the binding constraint stops
// being this limiter and becomes the two-slot policy itself.
//
// It is a NAMED CONSTANT rather than a number in this paragraph because
// three things consume it — this derivation, testKeyBudgetCoversMulti-
// JoinerRoom in __test_protocol.js, and check_key_rate_budget.py — and a
// design point restated in three places is a design point that drifts.
const KEY_RATE_DESIGN_DIALERS = 6;
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

// --- Task #122: NACK (typed refusal) -----------------------------------------
//
// THE PROBLEM. Before this, a client that was refused saw BARE SILENCE, and
// silence was produced by nine distinguishable server conditions: an
// unsupported version byte, a bad REGISTER/POLL length, an unallocated type
// byte, each of the THREE rate limiters (pre-gate, per-IP cookied,
// per-key), the MAX_NEW_KEYS_PER_IP creator quota, an all-paired full
// session table, and the third-party drop on a room whose two slots are
// both live. The client collapses every one of them into
// P2P_FAIL_RENDEZVOUS_DOWN — see the HONESTY NOTE in
// src/netplay/connect_fail.h, which names four of them and states outright
// that "a distinct NACK needs a wire change — S4/S5 territory". The relay
// removal orphaned that work; this re-homes it.
//
// CHALLENGE was the only server condition a client could positively
// observe. Now every refusal is a typed frame.
//
// --- WHY THIS IS NOT AN AMPLIFICATION / REFLECTION VECTOR ------------------
//
// A NACK is an UNAUTHENTICATED response to an UNAUTHENTICATED request, so
// it is a reflector surface by construction and has to be bounded by
// construction. Two INDEPENDENT bounds, each separately provable and each
// sufficient on its own:
//
//   BOUND 1 — PER FRAME, the response is never LARGER than the request.
//   NACK_LEN = 28 answering a 36-byte REGISTER/POLL: factor 28/36 = 0.778.
//   (CHALLENGE, already shipped, is 32/36 = 0.889; the NACK is tighter.)
//   This is ENFORCED in sendNack, not merely arranged: a NACK is suppressed
//   outright when the request is shorter than NACK_LEN, because the
//   BAD_TYPE branch is otherwise reachable by an 8-byte frame and was
//   measured amplifying 3.5x. The worst case that can now occur is a
//   28-byte v1 REGISTER answered by a 28-byte NACK — factor exactly 1.0,
//   which yields an attacker nothing over sending the packet to the victim
//   directly. So gain <= 1 on every frame, and no spray across any number
//   of spoofed victims can amplify anything.
//
//   BOUND 2 — PER VICTIM, aggregate egress toward any ONE address is capped
//   at 1 NACK per NACK_MIN_INTERVAL_MS regardless of inbound rate. This is
//   the bound that actually matters, because concentrating traffic on a
//   chosen victim is the only thing a reflector is USEFUL for. At 250 ms
//   that is <= 4 x 28 B = 112 B/s toward any address, forever, no matter
//   whether the attacker sends 10 pps or 10 Mpps. Bound 1 degrades
//   gracefully; bound 2 does not degrade at all.
//
// The cooldown is DELIBERATELY TIGHTER THAN EVERY REQUEST-PATH BUDGET —
// 4/s versus the pre-gate's 100/s, the cookied per-IP 10/s and the per-key
// 30/s. So "rate-limited on the same budget as the request path" holds in
// the only form that is not self-contradictory: a rate-limit NACK cannot
// draw the very bucket whose exhaustion caused it, so instead the NACK
// budget is DOMINATED BY all three request budgets. Whatever the request
// path admits, the NACK path admits strictly less.
//
// --- WHY IT LEAKS NO SESSION STATE -----------------------------------------
//
// Invariant, and it is mechanical rather than a judgement call: THE ONLY
// SERVER-ORIGINATED BYTES IN A NACK ARE magic, version, type AND THE REASON
// CODE. Everything else is either zero or an echo of bytes the sender
// itself just sent. There is no endpoint, no peer address, no port, no
// slot, no cookie, no count and no timestamp anywhere in the frame.
//
// The reason code is then split by whether the sender has proven return
// routability, because cookieForSlot() deliberately does not take the
// session key and so a cookie proves an ENDPOINT, never a right to a room:
//   * PRE-COOKIE reasons (BAD_VERSION, BAD_LENGTH, BAD_TYPE, RATE_PREGATE)
//     describe THE SENDER'S OWN FRAME or THE SENDER'S OWN IP BUDGET. They
//     say nothing whatsoever about any session, so an unproven sender —
//     including a source-spoofing one — learns nothing it did not already
//     know about itself.
//   * POST-COOKIE reasons (RATE_IP, RATE_KEY, KEY_QUOTA, TABLE_FULL,
//     SESSION_FULL) are emitted ONLY from inside or after
//     returnRoutabilityGate, i.e. only to a sender that provably RECEIVES
//     at its claimed endpoint. SESSION_FULL is the only one that is even
//     arguably session-derived, and it discloses strictly less than the
//     server already discloses today: a third party on a NOT-full key gets
//     a DELIVER, so occupancy is ALREADY distinguishable from
//     DELIVER-versus-silence. The NACK makes an existing signal explicit,
//     it does not create one — and only for a return-routable holder of the
//     room code, which is a capability that party already had.
//
// --- WHAT DOES *NOT* GET A NACK, AND WHY -----------------------------------
//
//   * MAGIC MISMATCH. Not our protocol at all. Replying would make this
//     server a reflector for ARBITRARY UDP — any 4 bytes that are not
//     '3SXR' — which is a categorically larger surface than replying to
//     frames that at least claim to be ours. Still a silent drop; it now
//     gets the log line it never had (see onMessage).
//   * SHORT PACKET (< 8 bytes). Cannot even be shown to claim our magic
//     and offsets; same reasoning.
// Both remain allocation-free: nothing below is touched until a datagram
// has proven it carries our magic.
const NACK_LEN = 28;
const NACK_MIN_INTERVAL_MS = 250;

// Reason codes. Wire values: APPEND-ONLY, never renumber — they are a
// client-facing enum (mirrored in src/netplay/rendezvous.h) and a log-grep
// anchor. 0 is reserved so an all-zero byte is never a valid reason.
const NACK_REASON_BAD_VERSION  = 1; // version byte is not VERSION
const NACK_REASON_BAD_LENGTH   = 2; // v2 REGISTER/POLL of the wrong length
const NACK_REASON_BAD_TYPE     = 3; // unallocated type, or a server->client type
const NACK_REASON_RATE_IP      = 4; // per-IP COOKIED budget exhausted
const NACK_REASON_RATE_KEY     = 5; // per-session-key budget exhausted
const NACK_REASON_RATE_PREGATE = 6; // uncookied first-contact budget exhausted
const NACK_REASON_KEY_QUOTA    = 7; // this IP already holds MAX_NEW_KEYS_PER_IP
const NACK_REASON_TABLE_FULL   = 8; // session table full of PAIRED sessions
const NACK_REASON_SESSION_FULL = 9; // both slots live and neither is yours

const NACK_REASON_NAME = {
    [NACK_REASON_BAD_VERSION]:  'BAD_VERSION',
    [NACK_REASON_BAD_LENGTH]:   'BAD_LENGTH',
    [NACK_REASON_BAD_TYPE]:     'BAD_TYPE',
    [NACK_REASON_RATE_IP]:      'RATE_IP',
    [NACK_REASON_RATE_KEY]:     'RATE_KEY',
    [NACK_REASON_RATE_PREGATE]: 'RATE_PREGATE',
    [NACK_REASON_KEY_QUOTA]:    'KEY_QUOTA',
    [NACK_REASON_TABLE_FULL]:   'TABLE_FULL',
    [NACK_REASON_SESSION_FULL]: 'SESSION_FULL',
};

// --- Task #122: lost-pairing-push + pairing-to-punch measurement -------------
//
// These are the two field quantities the netplay arc's residual risk rests
// on, and neither had ANY server-side instrumentation. The client already
// measures its half (confirm_ms, deliver_gap_max_ms). This is the other
// half, and it is a DIRECT observation rather than an inference — which is
// only possible because of one measured property of the shipped client:
//
//   A CLIENT STOPS RE-REGISTERING THE INSTANT A REAL-ENDPOINT DELIVER
//   LANDS. Host: src/netplay/direct_p2p.c:4767-4768 sets
//   s_rendezvous_cancel, breaking host_rendezvous_thread_fn's loop at
//   :3250-3253. Joiner: direct_p2p.c:2070-2071 clears signal_active, and
//   the race loop then exits at :2160-2162.
//
// So a REGISTER arriving from a peer we ALREADY pushed a real endpoint to
// means that peer is still behaving as if UNPAIRED — it never got the
// push. There is no ACK in this protocol (the client sends exactly one
// frame type, REGISTER — Rendezvous_BuildPoll has zero production call
// sites), so the CESSATION of REGISTERs is the only "I got it" signal the
// wire carries, and its absence is the loss signal.
//
// The same client fact makes the second quantity fall out for free. On the
// host, the DELIVER that cancels the resender is the SAME DELIVER that
// spawns the punch: direct_p2p.c:4788 sets DIRECT_P2P_FALLBACK_BILATERAL_PUNCH
// and :4796 spawns host_bilateral_punch_thread_fn, in the same function, off
// the same frame. Therefore
//
//   time(pairing -> the pushed-to peer's LAST REGISTER) == time(pairing ->
//   that peer starts punching)
//
// exactly, not approximately. A landed push makes it 0 ms. A LOST push
// makes it one full re-REGISTER interval — and the host's interval
// defaults to 5000 ms (src/port/config/config.c:111, floor 1000 ms at
// direct_p2p.c:3244). One lost datagram therefore costs up to FIVE SECONDS
// of connect latency, invisibly. That is the number this measures.
//
// TWO HONEST LIMITS, both of which bias the estimate DOWNWARD (it is a
// lower bound on loss, never an over-count):
//   1. CROSSING. A REGISTER already in flight when we paired arrives just
//      after the push and is indistinguishable from a lost push. It is
//      bounded by one RTT, while a genuine loss shows at the peer's own
//      cadence — 500 ms for a joiner, defaulting to 5000 ms for a host —
//      so the two populations barely overlap for the case that matters,
//      and the HISTOGRAM (not a single number) is what is reported
//      precisely so a crossing spike near 0 stays visible as itself.
//   2. HAIRPIN. If the pushed endpoint carries the RECIPIENT'S OWN
//      address, the client's self-DELIVER gate (direct_p2p.c:4760-4765
//      host, :2048-2052 joiner) deliberately does NOT cancel the resender,
//      so a later REGISTER is not evidence of anything. Those pushes are
//      excluded from the metric outright.
const PUSH_HIST_BUCKETS_MS = [250, 500, 1000, 2000, 5000, 10000];
// Report cadence for the aggregate. Piggybacks the existing rate sweep
// rather than adding a timer, so it costs no new wakeups.
const PUSH_REPORT_INTERVAL_MS = 60 * 1000;

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

// --- #130: per-slot observed cadence -----------------------------------------
//
// touchSlot() is the ONLY place a slot's liveness clock advances, so the
// observed cadence cannot drift away from lastSeen by someone refreshing one
// and forgetting the other. Both REGISTER (the idempotent re-register arms)
// and POLL route through it: the question the cadence answers is "how long
// may this occupant legitimately be silent", and POLL is liveness for that
// purpose exactly as REGISTER is.
//
// The estimator is the MAXIMUM gap yet seen between consecutive liveness
// signals from the CURRENT occupant, not the last gap. Under-estimating is
// the security failure -- it shortens the window in which the occupant is
// still considered live -- while over-estimating only delays a reclaim, and
// is bounded anyway by the SLOT_STALE_MS ceiling in slotReclaimIdleMs().
// A lost REGISTER therefore widens protection rather than opening it.
function touchSlot(entry, slot, now) {
    const seenKey = slot === 'A' ? 'lastSeenA' : 'lastSeenB';
    const refreshKey = slot === 'A' ? 'refreshA' : 'refreshB';
    const prev = entry[seenKey];
    // `!== 0`, not `> 0`. Zero is this file's "slot empty" sentinel and is the
    // only value that must be skipped. A NEGATIVE prev is a legitimately very
    // old timestamp -- it is also how every test in __test_protocol.js ages
    // state (`entry.lastTouch -= 61 * 1000` and friends), because the clock is
    // performance.now() and starts near zero. Writing `> 0` here silently
    // refused to measure a cadence in exactly those tests, which is a gate
    // that cannot see the thing it gates.
    if (prev !== 0 && now > prev) {
        const gap = now - prev;
        if (gap > (entry[refreshKey] || 0)) entry[refreshKey] = gap;
    }
    entry[seenKey] = now;
}

// Called wherever a slot changes OCCUPANT. The new occupant has shown no
// cadence yet, so its measurement must start over -- inheriting the previous
// occupant's cadence would let a reclaimer borrow a stranger's liveness.
function seatSlot(entry, slot, source, now) {
    if (slot === 'A') {
        entry.endpointA = source;
        entry.lastSeenA = now;
        entry.refreshA = 0;
    } else {
        entry.endpointB = source;
        entry.lastSeenB = now;
        entry.refreshB = 0;
    }
}

function clearSlotB(entry) {
    entry.endpointB = null;
    entry.lastSeenB = 0;
    entry.refreshB = 0;
}

// How long this slot must have been silent before a same-IP port reclaim may
// repoint it. See the PORT_RECLAIM_MISSED_REFRESHES block for the derivation;
// the two clauses here are the two halves of it:
//   * an UNOBSERVED cadence (0) means we have no evidence about this
//     occupant, and unknown must resolve to maximally protected;
//   * the ceiling is SLOT_STALE_MS because beyond it the pre-existing bStale
//     arm reclaims regardless, so a higher threshold would be a control that
//     cannot deny anything.
function slotReclaimIdleMs(refreshMs) {
    if (!(refreshMs > 0)) return SLOT_STALE_MS;
    return Math.min(refreshMs * PORT_RECLAIM_MISSED_REFRESHES, SLOT_STALE_MS);
}

function slotReclaimable(entry, slot, now) {
    const idle = now - (slot === 'A' ? entry.lastSeenA : entry.lastSeenB);
    return idle > slotReclaimIdleMs(slot === 'A' ? entry.refreshA : entry.refreshB);
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

// Task #122. 28 bytes:
//   [0..3]   magic '3SXR'
//   [4]      version — ALWAYS this server's VERSION, including in the
//            BAD_VERSION reply. That is the point: a client newer than us
//            reads a version byte it cannot parse from the very endpoint it
//            is REGISTERing to, which is exactly the source-gated evidence
//            src/netplay/direct_p2p.c:2074-2132 counts as badver_n, turning
//            RENDEZVOUS_DOWN into the DEFINITE CONNECT_ATTRIB_VERSION_SKEW
//            verdict instead of the hedged AMBIG_VERSION one. Today that
//            counter can never fire against this server, because the server
//            answers a version mismatch with nothing at all.
//   [5]      type = TYPE_NACK
//   [6]      reason
//   [7]      reserved (0)
//   [8..23]  the sender's own session-key bytes, echoed
//   [24..27] reserved (0)
//
// `srcBuf` is the REQUEST. Bytes [8..24] are echoed from it when it is long
// enough, and zeroed otherwise. Echoing is what lets the client gate the
// frame on its own session key (the key embeds the S4b nonce, so an
// off-path forger cannot produce a matching one) — and it is safe by
// construction because it returns the sender's bytes to the sender.
//
// For BAD_VERSION we are echoing from a frame whose version we do not
// speak, so the [8..24] offset is a GUESS about a foreign layout. It is a
// deliberate and harmless one: it has held across v1 and v2, and if a
// future version moves the field the echo is simply wrong bytes, which
// fails the client's key check and degrades to today's behaviour. It can
// never leak, because every byte of it came from the sender.
function encodeNack(reason, srcBuf) {
    const buf = Buffer.alloc(NACK_LEN);
    buf.writeUInt32BE(MAGIC, 0);
    buf.writeUInt8(VERSION, 4);
    buf.writeUInt8(TYPE_NACK, 5);
    buf.writeUInt8(reason, 6);
    buf.writeUInt8(0, 7); // reserved
    if (srcBuf && srcBuf.length >= 24) {
        srcBuf.copy(buf, 8, 8, 24);
    }
    // [24..27] reserved, already zero from Buffer.alloc.
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

// Task #122: NACK emission cooldown. Same sliding-window shape as the rate
// buckets, with limit = 1 over NACK_MIN_INTERVAL_MS, which is what makes
// "at most one NACK per source IP per 250 ms" a property of the SAME
// audited, MAX_RATE_ENTRIES-capped, O(1)-LRU machinery rather than a
// second hand-rolled one.
//
// Bounded-allocation note (review MEDIUM-3 lineage): this map IS keyed on
// an attacker-supplied value and IS allocated before the cookie gate, so a
// magic-bearing flood from N distinct sources creates entries. It is capped
// at MAX_RATE_ENTRIES with the same LRU eviction as the others (~1 stamp
// per entry => ~200 B, so ~1.6 MB fully saturated, the same order as
// rateMap). What MEDIUM-3 actually required is BOUNDEDNESS, and it holds.
// The stronger "junk allocates literally nothing" property also still
// holds for the two true junk classes: a short packet and a bad-magic
// packet never reach this map, because nothing here is touched until a
// datagram has proven it carries '3SXR'. An attacker must therefore spend
// four correct magic bytes to allocate one capped entry.
const nackMap = new Map();

// Task #122 metrics. Plain counters; no unbounded keys anywhere.
const pushStats = {
    pushed: 0,        // pairing pushes emitted (excluding hairpin-excluded)
    excluded: 0,      // hairpin pushes deliberately not measured
    lost: 0,          // pushes followed by a REGISTER from the pushed-to peer
    hist: new Array(PUSH_HIST_BUCKETS_MS.length + 1).fill(0),
    sumMs: 0,
    maxMs: 0,
    lastReport: -Infinity,
};

const nackStats = {
    sent: 0,
    suppressed: 0,    // refused by the cooldown — the amplification bound working
    byReason: new Map(), // reason(int) -> count. Fixed key set, cannot grow.
};

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
//
// Task #122 (operator gap). The one-shot `warned` set means the FIRST drop
// from an IP got a WARN and every later one was invisible until the bucket
// was evicted — so an operator reading the log could see that throttling
// started but never that it was still happening, still less how much of it
// there was. The one-shot WARN is kept (it names the IP the instant it
// starts) and a THROTTLED RUNNING COUNT is added underneath it. The count
// is keyed on a FIXED reason string, never on the IP, so noteMap cannot be
// grown by an attacker who rotates source addresses.
function rateLimitAllow(ip) {
    if (bucketAllow(rateMap, warnedIps, ip, RATE_WINDOW_MS, RATE_LIMIT_PER_WINDOW)) {
        return true;
    }
    if (!warnedIps.has(ip)) {
        warnedIps.add(ip);
        logWarn(`rate-limit: dropping COOKIED packets from ${ip} (running total follows under a ${NOTE_INTERVAL_MS / 1000}s throttle)`);
    }
    noteThrottled(logWarn, 'drop: per-IP cookied rate limit', `latest ${ip}, cap ${RATE_LIMIT_PER_WINDOW}/${RATE_WINDOW_MS}ms`);
    return false;
}

// Pre-cookie per-IP budget: bounds CHALLENGE emission only (review HIGH-2).
function preGateAllow(ip) {
    if (bucketAllow(preGateMap, warnedPreGate, ip, PREGATE_WINDOW_MS, PREGATE_LIMIT_PER_WINDOW)) {
        return true;
    }
    if (!warnedPreGate.has(ip)) {
        warnedPreGate.add(ip);
        logWarn(`pre-gate: dropping UNCOOKIED packets from ${ip} (challenge budget exhausted; cookied traffic from this IP is unaffected; running total follows under a ${NOTE_INTERVAL_MS / 1000}s throttle)`);
    }
    noteThrottled(logWarn, 'drop: pre-gate uncookied rate limit', `latest ${ip}, cap ${PREGATE_LIMIT_PER_WINDOW}/${PREGATE_WINDOW_MS}ms`);
    return false;
}

// S4c per-key limiter (post-cookie; see KEY_RATE_* rationale) --------------
function keyRateAllow(hexKey) {
    if (bucketAllow(keyRateMap, warnedKeys, hexKey, KEY_RATE_WINDOW_MS, KEY_RATE_LIMIT_PER_WINDOW)) {
        return true;
    }
    if (!warnedKeys.has(hexKey)) {
        warnedKeys.add(hexKey);
        logWarn(`key-rate-limit: dropping packets for key=${shortKey4(hexKey)}... (running total follows under a ${NOTE_INTERVAL_MS / 1000}s throttle)`);
    }
    noteThrottled(logWarn, 'drop: per-key rate limit', `latest key=${shortKey4(hexKey)}..., cap ${KEY_RATE_LIMIT_PER_WINDOW}/${KEY_RATE_WINDOW_MS}ms`);
    return false;
}

// Task #122: the single emission point for every NACK.
//
// Routing every reason through one function is what makes the two
// amplification bounds auditable in one place instead of nine: BOUND 1 is
// encodeNack's fixed NACK_LEN, BOUND 2 is the cooldown immediately below,
// and there is no other path that can put a NACK on the wire.
//
// Returns true if a NACK was actually sent. Callers ignore it: a
// suppressed NACK degrades to exactly the silence that shipped before this
// change, which is the correct failure mode.
function sendNack(socket, rinfo, reason, srcBuf) {
    // BOUND 1, ENFORCED RATHER THAN ASSUMED. Never answer with more bytes
    // than arrived.
    //
    // This guard is not theoretical. Without it the BAD_TYPE branch is a
    // 3.5x AMPLIFIER: the smallest frame that reaches it is 8 bytes
    // (onMessage's minimum, plus our magic and a v2 version byte, plus an
    // unallocated type), and it drew a 28-byte reply. Measured, not
    // reasoned about. BOUND 2 kept that from being a usable reflector, but
    // "the reply is always smaller than the request" was simply FALSE as
    // written, and a stated invariant that does not hold is worse than no
    // invariant.
    //
    // >= rather than > is deliberate. A v1 REGISTER is exactly 28 bytes,
    // the same as NACK_LEN, and that is THE case this whole feature exists
    // for (#87). Equal size is not amplification — an attacker gains
    // nothing over sending the packet to the victim itself — so the rule
    // is "never LARGER than the request", which admits v1 while making
    // gain impossible. Legitimate REGISTER/POLL are 36 bytes and clear it
    // with room to spare; nothing shorter than 28 bytes is a frame this
    // protocol has ever defined, so the silent drop is the right answer
    // for it.
    if (!srcBuf || srcBuf.length < NACK_LEN) {
        nackStats.suppressed += 1;
        noteThrottled(logInfo, 'nack: suppressed (request smaller than the reply)',
            `latest ${rinfo.address}:${rinfo.port} len=${srcBuf ? srcBuf.length : 0} < ${NACK_LEN}`);
        return false;
    }
    // BOUND 2. limit = 1 per NACK_MIN_INTERVAL_MS per source address.
    // `null` for the warn-set: there is nothing to warn about, suppression
    // is the design working, and it is counted in nackStats instead.
    if (!bucketAllow(nackMap, null, rinfo.address, NACK_MIN_INTERVAL_MS, 1)) {
        nackStats.suppressed += 1;
        noteThrottled(logInfo, 'nack: suppressed by cooldown',
            `latest ${rinfo.address} reason=${NACK_REASON_NAME[reason] || reason} (cap 1 per ${NACK_MIN_INTERVAL_MS}ms per source)`);
        return false;
    }
    const frame = encodeNack(reason, srcBuf);
    socket.send(frame, 0, NACK_LEN, rinfo.port, rinfo.address);
    nackStats.sent += 1;
    nackStats.byReason.set(reason, (nackStats.byReason.get(reason) || 0) + 1);
    // noteThrottled's contract is that its key comes from a FIXED set
    // chosen by this file, never from anything an attacker supplies —
    // otherwise noteMap is an unbounded allocator. `reason` is always one
    // of the nine NACK_REASON_* constants (it is chosen at the call site
    // and never read out of a packet), so the lookup below is total; the
    // fallback is a CONSTANT rather than an interpolation of `reason` so
    // that the key set stays finite even if that ever stops being true.
    noteThrottled(logInfo, `nack: ${NACK_REASON_NAME[reason] || 'unnamed'}`,
        `${rinfo.address}:${rinfo.port} (${NACK_LEN}B answering ${srcBuf ? srcBuf.length : 0}B)`);
    return true;
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
            if (warned) warned.delete(k); // #122: nackMap has no warn-set
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
    // #122: the NACK cooldown map ages out on the same policy.
    const nackEvicted = sweepBucketMap(nackMap, null, NACK_MIN_INTERVAL_MS, now);
    if (evicted > 0 || preEvicted > 0 || keyEvicted > 0 || nackEvicted > 0) {
        logInfo(`rate sweep: evicted ${evicted} ip(s) + ${preEvicted} pre-gate ip(s) + ${keyEvicted} key(s) + ${nackEvicted} nack-cooldown ip(s), live=${rateMap.size}/${preGateMap.size}/${keyRateMap.size}/${nackMap.size}`);
    }
    reportPushStats(now);
}

// --- Task #122: the two field quantities -------------------------------------

function pushHistBucket(ms) {
    for (let i = 0; i < PUSH_HIST_BUCKETS_MS.length; i++) {
        if (ms < PUSH_HIST_BUCKETS_MS[i]) return i;
    }
    return PUSH_HIST_BUCKETS_MS.length;
}

function pushHistLabel(i) {
    if (i < PUSH_HIST_BUCKETS_MS.length) {
        const lo = i === 0 ? 0 : PUSH_HIST_BUCKETS_MS[i - 1];
        return `${lo}-${PUSH_HIST_BUCKETS_MS[i]}ms`;
    }
    return `>=${PUSH_HIST_BUCKETS_MS[PUSH_HIST_BUCKETS_MS.length - 1]}ms`;
}

// Record one directly-observed lost push. `delayMs` is pairing -> the
// pushed-to peer's re-REGISTER, which per the derivation above IS
// pairing -> that peer starts punching.
function notePushLost(hexKey, peer, delayMs) {
    pushStats.lost += 1;
    pushStats.hist[pushHistBucket(delayMs)] += 1;
    pushStats.sumMs += delayMs;
    if (delayMs > pushStats.maxMs) pushStats.maxMs = delayMs;
    // #131: aggregated like every other per-packet warn in this file. The
    // COUNTERS above are the record; this line was only ever the sample,
    // and reportPushStats already emits count/mean/max/histogram on its
    // own interval, so nothing observable is lost. The reason string is a
    // file-chosen constant (noteThrottled keys noteMap on it), and the
    // per-event key/endpoint/delay move to the detail.
    noteThrottled(logWarn,
        '[PUSH-LOST] a paired peer re-REGISTERed after its pairing DELIVER — it never received the push (a paired client stops re-REGISTERing: direct_p2p.c:4749 host / :2068 joiner)',
        `key=${shortKey4(hexKey)}... ${peer.address}:${peer.port} +${Math.round(delayMs)} ms`);
}

function reportPushStats(now) {
    if (pushStats.pushed === 0 && pushStats.excluded === 0) return;
    if (now - pushStats.lastReport < PUSH_REPORT_INTERVAL_MS) return;
    pushStats.lastReport = now;
    const landed = pushStats.pushed - pushStats.lost;
    const pct = pushStats.pushed > 0
        ? ((pushStats.lost / pushStats.pushed) * 100).toFixed(1) : '0.0';
    const mean = pushStats.lost > 0 ? Math.round(pushStats.sumMs / pushStats.lost) : 0;
    const hist = pushStats.hist
        .map((n, i) => (n > 0 ? `${pushHistLabel(i)}=${n}` : null))
        .filter((s) => s !== null)
        .join(' ');
    logInfo(`[PUSH-STATS] pairing pushes=${pushStats.pushed} landed=${landed} lost=${pushStats.lost} (${pct}%) hairpin-excluded=${pushStats.excluded}`);
    logInfo(`[PUSH-STATS] pairing->first-punch for LOST pushes: mean=${mean}ms max=${Math.round(pushStats.maxMs)}ms hist[${hist || 'empty'}] (landed pushes are 0 ms by construction)`);
    if (nackStats.sent > 0 || nackStats.suppressed > 0) {
        const byReason = [...nackStats.byReason.entries()]
            .map(([r, n]) => `${NACK_REASON_NAME[r] || r}=${n}`)
            .join(' ');
        logInfo(`[NACK-STATS] sent=${nackStats.sent} suppressed-by-cooldown=${nackStats.suppressed} [${byReason}]`);
    }
}

// --- Packet handlers ---------------------------------------------------------

function handleRegister(socket, buf, rinfo) {
    const sessionKeyBuf = Buffer.from(buf.subarray(8, 24)); // copy out of receive buf
    const myPublicPort = buf.readUInt16BE(24);
    if (myPublicPort !== rinfo.port) {
        // #131: this is a PER-PACKET condition, not an event. A client
        // behind a port-rewriting NAT trips it on every REGISTER, i.e. at
        // its full resend cadence for the whole signalling leg, with no
        // attacker involved. Aggregate it.
        noteThrottled(logWarn,
            'REGISTER NAT mismatch: claimed my_public_port != source port',
            `claimed=${myPublicPort} source=${rinfo.port} key=${shortKey4(sessionKeyBuf.toString('hex'))}...`);
    }
    const hexKey = sessionKeyBuf.toString('hex');
    const source = { address: rinfo.address, port: rinfo.port };
    let entry = sessionMap.get(hexKey);
    let pairedPeer = null; // endpoint to receive an unsolicited DELIVER if we just paired
    const now = nowMs();

    // #122 METRIC. A REGISTER from the exact endpoint we last pushed a real
    // peer endpoint to. The shipped client cancels its resender the moment
    // such a DELIVER lands (direct_p2p.c:4767-4768 host, :2070-2071
    // joiner), so this frame is that peer still behaving as UNPAIRED: the
    // push was lost. Checked BEFORE any branch mutates state so it holds
    // for every path below (fresh pairing, reclaim, third-party drop).
    // pushTo is cleared on observation so one push yields one sample.
    if (entry && entry.pushTo && endpointEq(entry.pushTo, source)) {
        notePushLost(hexKey, source, now - entry.pushAtMs);
        entry.pushTo = null;
    }

    if (!entry) {
        // Per-IP live-key quota (review H2 defense 1) — checked before the
        // cap so a quota-violating IP can never trigger evictions either.
        const created = creatorCounts.get(source.address) || 0;
        if (created >= MAX_NEW_KEYS_PER_IP) {
            logWarn(`REGISTER from ${source.address}:${source.port} dropped — IP already holds ${created}/${MAX_NEW_KEYS_PER_IP} live keys`);
            // #122: post-cookie, so the sender is return-routable, and the
            // fact disclosed is about the SENDER'S OWN IP. connect_fail.h
            // names this as reachable in the field by one joiner retrying
            // >= 5 distinct stale codes inside the TTL.
            sendNack(socket, rinfo, NACK_REASON_KEY_QUOTA, buf);
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
                // #122: post-cookie. Discloses one bit of GLOBAL
                // operational state ("the server is saturated"), no
                // session state — and it is the difference between a user
                // told "try again in a minute" and a user told the
                // matchmaking server is down.
                sendNack(socket, rinfo, NACK_REASON_TABLE_FULL, buf);
                return;
            }
            releaseSession(oldestKey, oldestEntry);
            logWarn(`session table full — evicted oldest unpaired singleton key=${shortKey4(oldestKey)}... to admit ${source.address}:${source.port}`);
        }
        // #122: pushTo/pushAtMs track the LAST pairing DELIVER we pushed
        // (see the lost-push check at the top of this function). null =
        // nothing outstanding to observe.
        // #130: refreshA/refreshB are the per-slot OBSERVED cadence (max gap
        // between consecutive liveness signals from the current occupant).
        // They start at 0 = "not yet observed", which slotReclaimIdleMs()
        // resolves to SLOT_STALE_MS -- a brand-new slot is maximally
        // protected, not freely reclaimable.
        entry = { endpointA: source, endpointB: null, lastTouch: now, lastSeenA: now, lastSeenB: 0, refreshA: 0, refreshB: 0, creatorIp: source.address, portReclaims: 0, pushTo: null, pushAtMs: 0 };
        sessionMap.set(hexKey, entry);
        creatorCounts.set(source.address, created + 1);
    } else if (entry.endpointA && endpointEq(entry.endpointA, source)) {
        touchSlot(entry, 'A', now); // idempotent re-REGISTER from A (#130: also measures A's cadence)
    } else if (entry.endpointB && endpointEq(entry.endpointB, source)) {
        touchSlot(entry, 'B', now); // idempotent re-REGISTER from B (#130: also measures B's cadence)
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
            seatSlot(entry, 'A', source, now);
            clearSlotB(entry);
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
            seatSlot(entry, 'A', source, now);
            if (bStale) {
                clearSlotB(entry);
            }
        } else if (!entry.endpointA) {
            seatSlot(entry, 'A', source, now);
        } else if (!entry.endpointB) {
            seatSlot(entry, 'B', source, now);
            pairedPeer = entry.endpointA; // notify A that B has now joined
        } else if (entry.endpointB &&
                   entry.endpointB.address === source.address &&
                   slotReclaimable(entry, 'B', now) &&
                   (entry.portReclaims || 0) < MAX_PORT_RECLAIMS) {
            // Task #105: a party of THIS room retrying on a fresh source
            // port, and it happens to hold slot B. Same public address, port
            // changed, and — task #130 — slot B silent for longer than ITS
            // OWN observed cadence allows. Without this arm the retry is
            // ignored for the whole connect budget; without the staleness
            // term it repointed a LIVE slot. See the PORT_RECLAIM_MISSED_
            // REFRESHES block above for why the term is a cadence multiple
            // and not a fixed window (no fixed window exists), and the
            // MAX_PORT_RECLAIMS block for what this does and does not prove.
            entry.portReclaims = (entry.portReclaims || 0) + 1;
            logInfo(`[RECLAIM] port key=${shortKey4(hexKey)}... slotB ${entry.endpointB.address}:${entry.endpointB.port} -> ${source.address}:${source.port} (same-IP retry ${entry.portReclaims}/${MAX_PORT_RECLAIMS}, idle ${now - entry.lastSeenB}ms > ${slotReclaimIdleMs(entry.refreshB)}ms)`);
            seatSlot(entry, 'B', source, now);
            pairedPeer = entry.endpointA; // re-notify the peer with the new endpoint
        } else if (entry.endpointA &&
                   entry.endpointA.address === source.address &&
                   slotReclaimable(entry, 'A', now) &&
                   (entry.portReclaims || 0) < MAX_PORT_RECLAIMS) {
            // Task #105 follow-up. The SAME retry, for the party that holds
            // slot A instead. This arm is NOT redundant: slots are
            // first-come, not role-based (there is no role bit anywhere in
            // the REGISTER frame — handleRegister parses only key,
            // my_public_port and cookie), so the JOINER routinely wins slot
            // A. Measured 13/13: every failing rep with the joiner in slot A
            // showed 16 ignored REGISTERs and zero reclaims, because a
            // B-only rule cannot match a source whose address sits in A.
            //
            // Note this also repairs the HOST retry that the H1 reclaim
            // above misses. #130 narrows HOW MUCH it repairs, deliberately:
            // a host-cadence slot (~5000 ms observed) resolves to a 30000 ms
            // threshold, i.e. SLOT_STALE_MS, so over host-cadence slots this
            // arm now grants nothing the H1 arm did not already grant. That
            // is the point — the live-host hijack was the worst half of the
            // finding, and closing it is worth losing a repair that only
            // ever applied to a party whose HOST_WAITING state is unbounded
            // by design (src/netplay/direct_p2p.c:3331-3345) and can afford
            // to wait. The joiner, which cannot, keeps its fast path.
            entry.portReclaims = (entry.portReclaims || 0) + 1;
            logInfo(`[RECLAIM] port key=${shortKey4(hexKey)}... slotA ${entry.endpointA.address}:${entry.endpointA.port} -> ${source.address}:${source.port} (same-IP retry ${entry.portReclaims}/${MAX_PORT_RECLAIMS}, idle ${now - entry.lastSeenA}ms > ${slotReclaimIdleMs(entry.refreshA)}ms)`);
            seatSlot(entry, 'A', source, now);
            pairedPeer = entry.endpointB; // re-notify the peer with the new endpoint
        } else if (bStale) {
            // Both slots filled but the joiner slot is stale (abandoned
            // attempt). Replace it — same-IP retry from a new port, or a
            // fresh joiner arriving after an abandoned pairing — and
            // re-notify A of the new joiner endpoint.
            logInfo(`[RECLAIM] joiner slot key=${shortKey4(hexKey)}... ${entry.endpointB.address}:${entry.endpointB.port} -> ${source.address}:${source.port} (stale ${Math.round((now - entry.lastSeenB) / 1000)}s)`);
            seatSlot(entry, 'B', source, now);
            pairedPeer = entry.endpointA;
        } else {
            // Both slots live with different endpoints; treat as a third party.
            // (Don't overwrite either slot. Don't DELIVER.)
            // #131: also per-packet. Dialers 3..N of a room each resend
            // at the in-race cadence the per-key budget is sized for, so a
            // full KEY_RATE_DESIGN_DIALERS room emits this several times a
            // second for the whole signalling tail -- again with no
            // attacker present. Aggregate it.
            noteThrottled(logWarn,
                'REGISTER for a full session — ignored',
                `${source.address}:${source.port} key=${shortKey4(hexKey)}...`);
            // #122: post-cookie. This is the "the room you typed is
            // already taken" case, and it is the most USER-MEANINGFUL of
            // all nine reasons — today it is reported as "matchmaking
            // server down". It discloses no more than the server already
            // does: a third party on a NOT-full key gets a DELIVER, so
            // occupancy was always readable as DELIVER-versus-silence.
            sendNack(socket, rinfo, NACK_REASON_SESSION_FULL, buf);
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
        // #122 METRIC. Arm the lost-push observation.
        //
        // HAIRPIN EXCLUSION, and it is required for correctness rather
        // than tidiness: when the pushed endpoint carries the RECIPIENT'S
        // OWN address, the client's self-DELIVER gate
        // (direct_p2p.c:4760-4765 host, :2048-2052 joiner) deliberately
        // does NOT cancel the resender. Such a peer keeps REGISTERing
        // whether or not the push arrived, so counting it would
        // manufacture loss that did not happen — the H-1 misattribution
        // shape. Counted separately so the exclusion is visible rather
        // than silent.
        if (pairedPeer.address === source.address) {
            entry.pushTo = null;
            pushStats.excluded += 1;
        } else {
            entry.pushTo = { address: pairedPeer.address, port: pairedPeer.port };
            entry.pushAtMs = now;
            pushStats.pushed += 1;
        }
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
        // #151: the TTL refresh belongs to the SEATED arms only. It used
        // to sit up here, before the slot match, so the else arm below —
        // "source isn't a registered endpoint for this key" — refreshed
        // the entry too, letting any cookied holder of the key keep it
        // alive past SESSION_TTL_MS while seated in neither slot
        // (pinning one of the creator IP's key slots and one of
        // MAX_SESSIONS). handleRegister's SESSION_FULL branch returns
        // before ITS lastTouch write; now the two verbs agree.
        if (endpointEq(entry.endpointA, source)) {
            // slot liveness (review H1); #130: POLL is liveness for the
            // cadence estimate too -- see touchSlot().
            entry.lastTouch = nowMs();
            touchSlot(entry, 'A', entry.lastTouch);
            peer = entry.endpointB;
        } else if (endpointEq(entry.endpointB, source)) {
            entry.lastTouch = nowMs();
            touchSlot(entry, 'B', entry.lastTouch);
            peer = entry.endpointA;
        } else {
            // Source isn't a registered endpoint for this key.
            // Reply with zeroes — caller is asking but isn't pinned —
            // and refresh nothing (#151).
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
            // #122: PRE-cookie, so this says nothing about any session —
            // only "your IP's uncookied first-contact budget is spent",
            // which is a fact about the sender's own address. Under the
            // NACK cooldown, so a saturating source gets 4 NACKs/s, not
            // one per dropped packet: the limiter keeps limiting.
            sendNack(socket, rinfo, NACK_REASON_RATE_PREGATE, buf);
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
        // #122: post-cookie. The sender is proven, and the fact is about
        // its own budget.
        sendNack(socket, rinfo, NACK_REASON_RATE_IP, buf);
        return null;
    }
    if (!keyRateAllow(hexKey)) {
        // #122: post-cookie, and the key is the sender's own — it just
        // sent it. This is the one that silently breaks a busy room.
        sendNack(socket, rinfo, NACK_REASON_RATE_KEY, buf);
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
        // Magic mismatch — silent drop per spec ("if not 0x33535852,
        // drop"), and NO NACK: this is not our protocol, so replying would
        // make the server a reflector for arbitrary UDP.
        //
        // #122 (operator gap). It used to drop with NO LOG LINE AT ALL, so
        // the single most common thing that can arrive on a public UDP
        // port — scanners, stray STUN, wrong-port traffic, a client
        // pointed at the wrong host — was completely invisible to an
        // operator. Aggregated under the shared throttle on a FIXED reason
        // string, so a flood cannot turn this into an unbounded console
        // write and noteMap cannot grow.
        noteThrottled(logWarn, 'drop: magic mismatch',
            `magic=0x${magic.toString(16).padStart(8, '0')} len=${buf.length} from ${rinfo.address}:${rinfo.port}`);
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
        // #122. The most valuable NACK of the nine, and the one #87 is
        // about: prod ran an April v1 server that dropped v2 REGISTERs in
        // total silence, so every client saw "matchmaking server down" and
        // nobody could tell the difference from an unreachable box. The
        // reply is stamped with THIS server's version, which is precisely
        // the source-gated evidence direct_p2p.c:2074-2132 counts as
        // badver_n — promoting the client's verdict from the hedged
        // CONNECT_ATTRIB_AMBIG_VERSION to the definite
        // CONNECT_ATTRIB_VERSION_SKEW. Pre-cookie, but it carries no
        // session state at all: only a version byte and the sender's own
        // echoed bytes.
        sendNack(socket, rinfo, NACK_REASON_BAD_VERSION, buf);
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
            // #122. Pre-cookie; describes the sender's own frame only.
            // A truncating middlebox and a broken build are both real
            // field causes and neither is visible today.
            sendNack(socket, rinfo, NACK_REASON_BAD_LENGTH, buf);
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
    } else if (type === TYPE_CHALLENGE || type === TYPE_DELIVER || type === TYPE_NACK) {
        // The three SERVER -> CLIENT types. A client sending one is
        // confused or hostile; never let it reach state.
        //
        // #122 — DELIBERATELY NO NACK HERE, and this is the branch that
        // keeps the whole feature loop-free. These are exactly the frames
        // this server EMITS, so answering them is the one shape that could
        // put two servers (or a server and a reflected copy of its own
        // output) into a ping-pong. Refusing structurally means no
        // '3SXR' frame this server sends can ever elicit a reply from
        // another instance: a NACK is type 5 and lands right here, and it
        // never reaches the length check because it is not REGISTER/POLL.
        // The cooldown would have bounded such a loop anyway; not having
        // one at all is better than a bounded one.
        const name = type === TYPE_CHALLENGE ? 'CHALLENGE' : (type === TYPE_DELIVER ? 'DELIVER' : 'NACK');
        noteThrottled(logWarn, `drop: unexpected ${name}`, `from ${rinfo.address}:${rinfo.port}`);
        return;
    } else {
        // Unallocated type byte (6..8 are the freed relay types, plus
        // everything else). No legitimate client emits one, but a STALE
        // client does — relay-era builds emitted types 5..8 — and telling
        // it so is the difference between "update your build" and
        // "matchmaking is down".
        noteThrottled(logWarn, 'drop: unknown type', `type=${type} from ${rinfo.address}:${rinfo.port}`);
        sendNack(socket, rinfo, NACK_REASON_BAD_TYPE, buf);
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
        _maxPortReclaims: MAX_PORT_RECLAIMS, // #105
        _portReclaimMissedRefreshes: PORT_RECLAIM_MISSED_REFRESHES, // #130
        _slotReclaimIdleMs: slotReclaimIdleMs, // #130
        _sweepNow() {
            sweepSessions();
            sweepRates();
        },
        // Clears ALL THREE rate buckets — most callers just want a clean
        // budget. _resetKeyRate() isolates the per-key one.
        // Clears the NACK cooldown too (#122) — otherwise the 250 ms
        // cooldown leaks across tests and a later test's NACK vanishes
        // for reasons that have nothing to do with what it is asserting.
        _resetRate() {
            rateMap.clear();
            warnedIps.clear();
            preGateMap.clear();
            warnedPreGate.clear();
            keyRateMap.clear();
            warnedKeys.clear();
            nackMap.clear();
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
        // #123: the multi-joiner budget test sizes its dialer wave from the
        // DESIGN POINT rather than restating it, so the test and the
        // derivation cannot disagree about what N is.
        _keyDesignDialers: KEY_RATE_DESIGN_DIALERS,
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
        // --- Task #122 hooks ---------------------------------------------
        _typeNack: TYPE_NACK,
        _nackLen: NACK_LEN,
        _nackMinIntervalMs: NACK_MIN_INTERVAL_MS,
        _nackMap: nackMap,
        _nackStats: nackStats,
        _nackReasons: {
            BAD_VERSION: NACK_REASON_BAD_VERSION,
            BAD_LENGTH: NACK_REASON_BAD_LENGTH,
            BAD_TYPE: NACK_REASON_BAD_TYPE,
            RATE_IP: NACK_REASON_RATE_IP,
            RATE_KEY: NACK_REASON_RATE_KEY,
            RATE_PREGATE: NACK_REASON_RATE_PREGATE,
            KEY_QUOTA: NACK_REASON_KEY_QUOTA,
            TABLE_FULL: NACK_REASON_TABLE_FULL,
            SESSION_FULL: NACK_REASON_SESSION_FULL,
        },
        _pushStats: pushStats,
        _pushHistBucketsMs: PUSH_HIST_BUCKETS_MS,
        _resetMetrics() {
            pushStats.pushed = 0;
            pushStats.excluded = 0;
            pushStats.lost = 0;
            pushStats.hist.fill(0);
            pushStats.sumMs = 0;
            pushStats.maxMs = 0;
            pushStats.lastReport = -Infinity;
            nackStats.sent = 0;
            nackStats.suppressed = 0;
            nackStats.byReason.clear();
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
