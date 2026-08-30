# Work queue — TEMPORARY, dated 2026-08-30

> **DELETE THIS FILE WHEN THE QUEUE IS EMPTY.** It exists only because the
> session's task tooling was withdrawn mid-flight and the queue had nowhere
> else to live. A stale tasklist checked into a repo is worse than none: it
> reads as current long after it stops being true.
>
> **Staleness test, runnable:** every line number below was verified against
> `dcf7631a` on `upstream-engine-fixes`. Run
> `git log --oneline dcf7631a..HEAD -- src/netplay tools/rendezvous-server`
> — if that prints anything, re-verify before trusting a citation here.
> If every entry is closed, `git rm docs/queue.md`.

Findings and citations only. No narrative, no status prose.

---

## #130 — slot-reclaim staleness — CLOSED

Both port-reclaim arms now carry a staleness precondition:
`tools/rendezvous-server/rendezvous-server.js:1348` (slot B) and `:1365` (slot A).

The threshold is a multiple of the slot's **own observed cadence**, not a fixed
window — `slotReclaimIdleMs` (`rendezvous-server.js:739`), measured by
`touchSlot` (`:692`), reset on repoint by `seatSlot` (`:713`).

**No fixed window exists.** Protecting a live host slot with one missed-refresh
of margin needs `W > 10000` (host advertise default 5000 ms,
`src/port/config/config.c:111`); preserving #105 needs `W < 8000` (the
attempt-2 signalling leg, `src/port/config/config.c:105`, ended at
`src/netplay/direct_p2p.c:1958-1959`). The interval is empty.

`PORT_RECLAIM_MISSED_REFRESHES = 6` (`rendezvous-server.js:262`) is not a new
constant: it is `SLOT_STALE_MS / host cadence` = 30000/5000, i.e. the "six
missed refreshes" standard `SLOT_STALE_MS` (`:109`) already encodes, re-expressed
in units of the slot that is actually being reclaimed. Yields:

- joiner-cadence slot (~500 ms) → 3000 ms; fits the 8000 ms retry leg
- host-cadence slot (~5000 ms) → 30000 ms = `SLOT_STALE_MS`; the arms grant
  nothing over host-cadence slots that the pre-existing stale arm did not
- unobserved cadence → `SLOT_STALE_MS` (never 0)

Enforced, not asserted in prose: `tools/rendezvous-server/check_reclaim_window.py`,
gate `reclaim-window` in `tools/gates/run-gates.sh`. Client and server deploy
independently, so no `_Static_assert` or JS assertion can see both sides — same
coupling and same remedy as #123's `check_key_rate_budget.py`.

The understated comment at the old `:160-164` is replaced by
`rendezvous-server.js:160-188` ("THE RESIDUAL, CORRECTED").

**The two 8-caps were NOT linked, with evidence.** `MAX_PORT_RECLAIMS`
(`rendezvous-server.js:194`) and `LATE_PUNCH_MAX_RELEARNS`
(`src/netplay/late_punch.h:98`) cannot compose: a rendezvous DELIVER cannot
reach the late-punch layer at all. `src/netplay/sdl_net_adapter.c:368-373`
destroys every `Rendezvous_HasMagic` datagram before the late-punch call at
`:387-389`, and `LatePunch_HandleDatagram` itself returns false for anything
failing `Stun_HasPunchPrefix` (`src/netplay/late_punch.c:101-102`). A relearn
requires an authenticated `3SX_PUNCH` datagram from the established peer IP on
a new port (`late_punch.c:132-134`). Linking them would couple two counters
that share no state and no path.

Coverage: `__test_protocol.js` 39 tests (was 36) — `liveHostSlotNotHijackable`,
`unobservedSlotMaximallyProtected`, `portReclaimBudgetExhausted`;
`joinerPortReclaimSameIp` and `portReclaimSlotA` reworked to assert both
directions.

---

## #131 — review batch (five items) — CLOSED

All five landed as separate commits; full gate set GREEN including ARM.
Frame-data was **skipped deliberately**: nothing here reaches the simulation
(orchestrator timing arithmetic, a socket-lifetime guard, a Python gate, JS
logging, and comments).

1. **Host ladder undercounted the worker startup delay — FIXED** (`dfae3c13`).
   `ORCH_HOST_LADDER_MS` (`src/netplay/direct_p2p.c:6158`) now multiplies
   `WORKER_STARTUP_DELAY_MS` by `1 + HOST_STUN_MAX_RETRIES`: the S2 ladder
   respawns `host_thread_fn` (`:5856`) and the delay is that worker's first
   statement (`:3518`), so it is paid per rung. Assert `[D]` re-derived
   86450 → **87050** (`:6289`); shipped defaults 42,450 → 43,050; the `[C]`
   inversion corner 30,450 → 31,050 (still under postwait's 31,300, so the
   max stays load-bearing); the #96 upper guard 120,200 → 120,800.
   The joiner term was already correct and is untouched — its delay is taken
   once (`:4334`), OUTSIDE the `JOIN_MAX_ATTEMPTS` loop.
   **Perturbed to confirm:** reverting the macro fails the build on `[D]`
   with `200 + 11250 + 4*15000 + 3*5000 = 86450 >= 87050`.

2. **Late-punch socket invalidation — GUARD ADDED, no live bug**
   (`6b17dc73`). `Netplay_SetStunSocket` (`src/netplay/netplay.c:2727`) now
   calls `LatePunch_Disarm()` on the destroy branch only.
   **The failing case could NOT be constructed, and the attempt failed on a
   real structural guard rather than on luck:** `do_handoff` (`direct_p2p.c:4522`)
   is the only production caller, it nulls `s_work.stun.socket` immediately
   after (`:4532`), and Tick's HANDOFF case re-enters `join_tick_handoff`
   only while that field is non-NULL (`:5783`) — so a second in-session
   handoff is unreachable. Both terminal paths disarm before the destroy by
   explicit ordering (`netplay.c:2543` vs `:2562`; SessionStarted `:1798`).
   What remains is that the invariant lives in a *different* TU, so it is now
   pinned by `test_late_punch` A12 (`src/netplay/test_late_punch.c:334`),
   which fails with the disarm removed.

3. **Rate-cap gate vs its prose — GATE TIGHTENED, prose is authoritative**
   (`15b7d40e`). `check_key_rate_budget.py:250` now solves for the smallest
   INTEGER `k`: `k = max(MIN_KEY_TO_IP_RATIO, ceil(absorption/ip_cap))`,
   giving **30** instead of 23.
   Chosen over relaxing the prose because (a) the gate's own docstring
   already quoted "the smallest integer k … is k = 3", so the file
   contradicted itself and the prose half was right; (b) at 23 the
   under-attack headroom is 13 of 13 — exactly zero — against the 1.5x the
   derivation claims, so documenting 23 would have licensed a capless cap;
   (c) `__test_protocol.js` already enforced the UPPER half (`cap <= 3 x`
   per-IP) but also passed at 24, so nothing closed the band.
   `docs/plan-netplay-connection.md` already said 30 and needed no edit.
   **Falsified by sweep:** cap 22/23/24/29 → exit 1, cap 30 → exit 0. The
   old gate passed 24.

4. **Unthrottled WARNs — FIXED, and it was three sites not one** (`a1928795`).
   `notePushLost` (`rendezvous-server.js:1212`) now routes through
   `noteThrottled`. Auditing every `logWarn` found two *worse* post-cookie
   per-packet sites, both of which fire with **no attacker present**:
   "REGISTER NAT mismatch" (`:1249`, once per REGISTER for any
   port-rewriting NAT) and "REGISTER … for full session" (`:1420`, once per
   REGISTER from each of dialers 3..N). `notePushLost` was in fact the
   quietest of the three — bounded by the cookied per-IP gate *and* by
   single-observation clearing. All three converted; each keeps a
   file-chosen CONSTANT reason (noteMap keys on it) with the variable parts
   in `detail`. `pushStats` still increments unconditionally and
   `reportPushStats` already emits the aggregate, so no data is lost.

5. **Doc drift, three sites — FIXED** (`2ae601d4`).
   - `netplay_nav.c:133` 120.2 s → **87.05 s**, with a note that it is prose
     about a derived, `_Static_assert`-pinned number and has now rotted twice.
   - `direct_p2p.c:2643` "10 s ceiling" → **11.25 s** (6000 + 3 x 1750 =
     `PORTMAP_PROBE_BUDGET_MS`, which the loop below enforces).
   - `gen_plw_canon_fields.py:21` stated its descend/emit rule exactly
     BACKWARDS; `parse_plw` descends into non-unions and emits unions whole.
     Generated output unaffected — `--check` passes either way (399 fields,
     38 pointers, sizeof=1092/1304).
   - 11 citations in `docs/plan-netplay-connection.md` follow the +2/+6 line
     shift. Verified as a shift (the doc had 0 errors immediately prior), not
     taken from the linter's suggestions — four of those point at a different
     occurrence of the anchor. `--fix` was not run.

**Gates:** `tools/gates/run-gates.sh --arm` → all 9 GREEN, exit 0. 11
harnesses, every one exit 0, zero "not compiled in" in any log.
`__test_protocol.js` 39/39. Doc-citation baselines 11 scopes, 0 breached,
0 slack. ARM cross-build GREEN in the aarch64 container.

## #132 — coverage plan — PRIORITY 1 CLOSED

Three commits. Priority 2 and 3 remain open, unchanged, below.

**What was covered, and what each test catches.** Every test below was shown
RED against a specific mutation of the code it pins; a test that has never
been red proves nothing. Each red was confirmed after deleting BOTH the object
file AND the linked binary — Unix Makefiles compares mtimes at one-second
granularity, so rewriting a source inside the same second leaves the harness
silently re-running the previous binary (observed twice while building the
proof, once as a one-step lag and once as an every-other-step lag).

### `--test-mist-compat-gate` (`3faacc32`) — 9 tests, 1294 assertions

`classify_peer_payload` (`src/netplay/mist_handshake.c:314`), `parse_header`
`:210`, and the four bounds-checked readers `:236/:255/:263/:271`, reached
through six thin trampolines in the existing `ENABLE_NETPLAY_TESTS` block
(`mist_handshake.c:1003-1057`). NOT the `mist_handshake_gate_next` `:894`
production-export shape: promoting the classifier to a shipped symbol would
widen the compat gate's API for no shipped caller. The `s_balance_digest`
global needed no extraction at all — `mist_handshake_set_balance_digest`
(`:110`) is already a production setter.

| test | mutation it was proved against |
| --- | --- |
| `accept` | build_hash difference becomes a reject |
| `truncate` | `read_u64be` bound off by one |
| `reasons` | arch reject reports the platform code |
| `order` | `state_ver` checked before `proto_ver` |
| `strings` | `read_cstr` advances by the TRUNCATED copy length |
| `header` | declared-vs-received length check removed |
| `readers` | `read_u16be` becomes little-endian |
| `digest` | digest compared on its 32-bit DISPLAY fold |
| `text` | the arch reason text ignores `text_cap` |

The truncation sweep asserts a reason per prefix byte: `[0,20]` MALFORMED,
`[21,23]` LEGACY, `[24,31]` MALFORMED, `32` accept. The LEGACY window is three
bytes wide between two MALFORMED regions, which is what makes an off-by-one in
any reader visible.

### `--test-punch-predicates` (`80327519`) — 7 tests, 744 assertions

`Stun_IsBindingResponse` (`src/netplay/stun.c:1028`) had zero tests despite
gating three receive paths (`direct_p2p.c:1060`, `:2230`,
`sdl_net_adapter.c:343`). `Stun_IsPunchPayload` `:147` had a four-row truth
table at `test_stun_mock.c:861-894` that said nothing about WHICH bytes it
compares. Now: all 64 token bits, all 72 prefix bits, the length swept -2..64,
plus a FOLD trap and an ORDER trap. Those two earn their place — replacing
`diff |=` with `diff ^=` at `stun.c:160` passes all 136 single-bit flips and
fails only those two assertions.

Three late-punch decisions the socket harness cannot observe, because seeing a
datagram at all means advancing past `LATE_PUNCH_TX_INTERVAL_MS`, by which
point the cadenced keepalive has fired anyway: a foreign-IP valid token must
schedule no answer (`late_punch.c:116-123` says so; nothing enforced it),
the capped move must change nothing and keep targeting the LEARNED endpoint,
and an accepted relearn must move the target and hand netplay.c the same port.
`LatePunch_TestPeek` (read-only, `ENABLE_NETPLAY_TESTS` only) is what makes
the send target observable without a socket.

Mutations proved: N1 binding-response floor 20→16, N2 prefix memcmp 9→1 byte,
N3 length becomes a minimum, N3b loop drops the last token byte, N3c
accumulator becomes an xor fold, N4 foreign-IP guard removed, N5 cap removed,
N6 accepted relearn does not move the target, N6b the capped move moves it,
N7 a wrong-token punch schedules an answer.

### `constant-time-compare` gate (`c62f2792`) — gate count 9 → 10

`tools/gates/check_constant_time_compare.py`. An early-exiting rewrite of the
punch-token compare is BEHAVIOURALLY IDENTICAL — same verdict for every input
— so the 136-bit sweep passes against it unchanged and no unit test can see
it. A timing measurement over a 17-byte compare is noise. So the property is
checked where it is expressed: the loop still runs over
`STUN_PUNCH_TOKEN_LEN`, accumulates with `|=`, and contains no `return`,
`break`, `goto`, `continue`, `if`, `?:` or short-circuit. Red-proved four
ways; a renamed function exits **2** (ERROR), because a check that silently
finds nothing is worse than no check.

**Anti-vacuity, all three devices proved to fire** in both harnesses: literal
`EXPECTED_TESTS` (commenting one call out → "ran 8, expected exactly 9"), the
assertion floor ("only 527 assertion(s) ran, floor is 550"), and per-sweep
case floors. Both stubs say "not compiled **in**", the spelling
`tools/gates/run-gates.sh:181` greps for; both exit **2** against the
shipped-config binary.

**Judged NOT worth testing, with reasons:**
- The `*off + n > payload_len` form in the fixed-width readers wraps for an
  offset near `SIZE_MAX`. Unreachable by construction — `*off` is only ever
  advanced by `read_cstr` and by the readers themselves, all of which keep it
  in `[0, payload_len]`. A test would pin a precondition the code cannot
  reach; the harness sweeps all 153 `(off, len)` pairs inside the real
  invariant instead and names the residual in a comment.
- Whether the relearn-cap path ALSO raises the prompt-answer flag. The flag is
  sticky until `LatePunch_Tick` sends (`late_punch.c:181`), so after the eight
  accepted moves an answer is legitimately already owed. Separating the two
  needs a Tick, which needs a socket, which is `test_late_punch.c`'s job.
  Faking it would be the vacuous version.
- Constant-time TIMING itself — see the gate above.

**Frame-data: SKIPPED, deliberately.** Nothing in this lane reaches the
simulation. Two new test translation units, six test-only trampolines, one
read-only test-only accessor, and a Python source-shape check; the only
non-test source edits are inside `#ifdef ENABLE_NETPLAY_TESTS`.

**Priority 2** (open)
- The `ORCH_*` cascade — `ORCH_HOST_WORST_CASE_MS`
  (`src/netplay/direct_p2p.c:6162`) — table-driven **from independently listed
  constants, never importing the macro under test**.
- natpmp timeout math, refactored to take a caller-supplied clock.

**Priority 3** (open)
- Extract ~13 pure blocks out of `src/netplay/test_bilateral_punch.c`.

**Non-goals, deliberately:** the punch race, split brain, the PROMISED
LISTENING INTERVAL, the reclaim boundary, GekkoNet/rollback determinism, UPnP.
All interaction bugs, where a mocked unit passes vacuously.

---

## Smaller open items

- **#129** — ROM search trim + a legible miss message.
- **#125** — `--test-connect-observability` flakes **only under concurrent gate
  runs**, always at `test6-byte-budget`; shared logs directory.
- **#128** — five reproducible file-load failures (file numbers 9, 10, 1454,
  1456, 1458) plus 379 ms startup outliers. Pre-existing.
- **#108** — `--test-enable` pins PS2, so no corpus run exercises the shipping
  engine; `Present_Mode` 4/5 hides the select screen.

---

## Open structural gaps

- `tools/netplay/natmatrix/mech_matrix.sh:112` falls off the end of the file
  and exits 0 regardless of per-rep `rc` — the last statement is an `echo`,
  with no stage-rc propagation of the kind `tools/netplay/natmatrix/run_all.sh:27-31` grew.

**Closed 2026-08-30 (task #122 test commit), recorded so the list does not
re-open them:**

- `src/netplay/test_sparse_effect_save.c` printed "not compiled **with**" where
  `tools/gates/run-gates.sh:179` greps "not compiled **in**". Fixed. The same
  evasion was found and fixed in `src/test/test_texcash_bounds.c`.

---

## Needs the user at the machine

- TV + pad confirmation of the shortened select countdown — measured **at the
  source of the rendered digits, never at the glass**.

---

*Verified at `dcf7631a` (`upstream-engine-fixes`), i.e. with task #122's
client-half (`bfa20e56`) and test (`dcf7631a`) commits applied. Re-verify every
line number if the tree has moved.*
