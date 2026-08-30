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
(`src/netplay/late_punch.h:190`) cannot compose: a rendezvous DELIVER cannot
reach the late-punch layer at all. `src/netplay/sdl_net_adapter.c:368-373`
destroys every `Rendezvous_HasMagic` datagram before the late-punch call at
`:387-389`, and `LatePunch_HandleDatagram` itself returns false for anything
failing `Stun_HasPunchPrefix` (`src/netplay/late_punch.c:217-219`). A relearn
requires an authenticated `3SX_PUNCH` datagram from the established peer IP on
a new port (`late_punch.c:132-134`). Linking them would couple two counters
that share no state and no path.

Coverage: `__test_protocol.js` 39 tests (was 36) — `liveHostSlotNotHijackable`,
`unobservedSlotMaximallyProtected`, `portReclaimBudgetExhausted`;
`joinerPortReclaimSameIp` and `portReclaimSlotA` reworked to assert both
directions.

**That result stands, and answers a narrower question than the review asked.**
See #133 below: it settles composition, not separate exercise.

---

## #133 — both-caps, exercised SEPARATELY — SEVERITY ANSWERED: **TAKEOVER**. Mitigation **SHIPPED** (host-side; rig re-run pending).

**The severity analysis below stands as written and is the record of what was
wrong. The mitigation it recommended is now implemented — see "Mitigation —
IMPLEMENTED" at the end of this section for what changed, what it does and does
not buy, and what is still unproven.**

The question: can a same-public-IP room-code holder exercise `MAX_PORT_RECLAIMS`
(`rendezvous-server.js:194`) and `LATE_PUNCH_MAX_RELEARNS`
(`src/netplay/late_punch.h:190`) **separately** in one session? #130 proved only
that one datagram cannot drive both.

**Answer: yes, and the relearn grants a capability a plain room-code join does
not.** Verified:

- **The room code is sufficient to derive the punch token.** The token is
  `SHA-256("3SXR-PT3" || ip || port || nonce)[0..7]` (`src/netplay/rendezvous.c:146-152`,
  derivation `:86-133`), and all three inputs are carried *inside* the room code
  (`src/netplay/room_code.h:9`, `:129`, `:207`; the nonce is the low 32 bits,
  `room_code.h:231-236`). No wire observation and no server access is needed.
- **The caps sit in different session phases**, so they are independently
  reachable: a reclaim acts on the server during signalling, while late-punch is
  armed only *after* `do_handoff` (`src/netplay/netplay.c:2177-2181`).
- **Baseline — what the room code already authorises.** A holder from *any* IP
  can already become the peer: `classify_host_datagram` returns `PEER_PUNCH` on
  token match with no source-IP test (`src/netplay/direct_p2p.c:1065`), captured
  at `:5108-5121`. That is inside the trust boundary, as expected.
- **What exceeds it: post-commitment displacement.** After capture the host
  leaves `HOST_WAITING` (`direct_p2p.c:5118`) and `host_tick_receive` is
  reachable only from that state (`direct_p2p.c:5748-5761`) — so a racing loser
  normally gets no second attempt. `LatePunch_HandleDatagram` is the only path
  that re-points an already-committed peer, and its sole identity check is
  `strcmp(src_ip, s_peer_ip)` (`src/netplay/late_punch.c:280`) — an IP compare
  that **cannot distinguish "the peer's NAT mapping moved" from "a different
  host behind the same public IP"**. One relearn sets `remote_port` and calls
  `SDLNetAdapter_RetargetPeer` (`netplay.c:1419-1421`), after which every
  outbound session datagram goes to the attacker
  (`src/netplay/sdl_net_adapter.c:288-290`).
- **It survives into the match.** `LatePunch_Disarm` (`late_punch.c:187-206`)
  clears the token and arming but **never touches `s_actual_peer` /
  `s_retarget_active`**; only `SDLNetAdapter_SetCanonicalPeer`
  (`sdl_net_adapter.c:53-64`) and `SDLNetAdapter_Destroy` (`:465-466`) do.
- **No persistence past teardown** — teardown disarms and destroys the adapter,
  and the nonce re-rolls per hosting attempt (`direct_p2p.c:3740`). This half of
  the original expectation held.
- **One relearn is enough; the cap of 8 bounds flapping, not capability.**

Two comments were contradicted by the above. **Both corrected** (this commit):
`late_punch.h` now states that the relearn gate is narrower on source IP but
strictly *wider in time*, and that the relearn cap bounds flapping rather than
capability.

### The severity question — SETTLED: takeover, not DoS

Nothing on the path stops a substituted party that is running the same public
build with the same CPS3 ROM. Each downstream gate, read and (where a harness
exists) exercised:

- **MIST compat gate — passes, by construction.** `classify_peer_payload`
  (`src/netplay/mist_handshake.c:314`) accepts on exactly five fields: arch tag
  `"armv7"`, platform tag `"mister"`, `proto_ver`, `state_ver`, and
  `balance_digest`. `build_hash` is a **warning, never a reject**
  (`mist_handshake.c:378-383`). Every one of those five is a compile-time or
  ROM-derived constant of the shipped build; **none is derived from the room
  code, the punch token, the nonce, or the session**. Exercised:
  `--test-mist-compat-gate` `test1_accept` asserts "a different build_hash
  warns, it does not reject" and accepts a payload assembled byte-by-byte from
  public constants alone (`src/netplay/test_mist_compat_gate.c:225-259`).
  Harness green at this tip: **9 tests, 1294 assertions, 0 failures**.
- **Balance digest — derivable, not observed.** `ArcadeBalance_GetDigest()`
  (`src/arcade/arcade_balance.c:179`) returns `ArcadeCharData_ComputeDigest()`
  computed once at boot from the adapted CPS3 ROM data (`:152`). Same ROM
  revision → same digest for every player. It is not a secret and not
  session-bound.
- **Nothing source-gates the ack that completes the handshake.** In
  `mist_handshake_pump`, the `cls == 1` branch returns `MIST_PUMP_OK` with **no
  `from_peer` test** (`src/netplay/mist_handshake.c:743-771`); `from_peer` gates
  only the H-1 hello latch (`:835`) and implicit completion (`:854`). And after
  a relearn the substitute *is* `from_peer` anyway — `late_punch_service` sets
  `remote_port` (`netplay.c:1419`), which `mist_pump_start` copies into
  `io->peer_port` (`netplay.c:1339`) after the `s_hs_peer_refetch` re-resolve
  (`:1309-1314`).
- **GekkoNet authenticates nothing about the remote.** The remote is registered
  once by `"ip:port"` string (`netplay.c:1559-1561`) and matched by
  `GetRemoteHandlesForAddress` (`third_party/GekkoNet/build/include/backend.h:157`).
  The `u16 session magic` (`net.h:40`, `backend.h:76`, `:191`) is negotiated
  **in band** via SyncRequest/SyncResponse (`backend.h:151-153`) toward whatever
  endpoint we send to — which after a relearn is the substitute.
- **The desync checksum does not save us.** `GekkoDesyncDetected` does terminate
  the session (`netplay.c:1818-1851`) — but a substitute that is a real 3SX
  instance simulates the same game and produces matching checksums. The
  checksum only bites a substitute that *cannot* simulate; that variant is the
  DoS floor, not the ceiling.

**Two capture windows, both reachable.** `late_punch_service` runs in
TRANSITIONING (`netplay.c:2319`) and CONNECTING (`:2432`):

1. **Relearn in TRANSITIONING** — `remote_port` moves *before*
   `configure_gekko`, so `SDLNetAdapter_SetCanonicalPeer` (`netplay.c:1599`)
   registers **the substitute as the canonical peer outright**. The legitimate
   peer's datagrams then arrive under a non-canonical address string and
   GekkoNet ignores them. Clean substitution; MIST runs against the substitute.
2. **Relearn in CONNECTING** — after `configure_gekko`, so
   `SDLNetAdapter_RetargetPeer` sets `s_retarget_active`
   (`sdl_net_adapter.c:67-85`): all outbound goes to the substitute
   (`:288-290`) and its inbound is relabelled canonical (`:405`). The
   legitimate peer stops receiving our inputs and times out, leaving the
   substitute as the only live remote.

**The relearn cap is anti-mitigation.** Once `s_relearn_count` reaches
`LATE_PUNCH_MAX_RELEARNS` the endpoint is **frozen** and every later move is
refused (the pre-mitigation relearn-cap block in `late_punch.c`, deleted by
the fix; the refusal now lives in `late_punch_nominate`,
`src/netplay/late_punch.c:95-102`, and refuses at ONE). A same-IP party that
fires 8
token-valid punches from 8 different source ports therefore locks the send
target onto its own 8th port, and the legitimate peer — still punching from its
original mapping — can never be relearned back. The cap converts a flapping
contest into a **deterministic** capture.

**Window size:** up to `40 x 500 ms` of MIST retries
(`src/netplay/mist_handshake.h:250`) plus `CONNECT_TIMEOUT_CONNECTING_MS =
15000` (`src/netplay/connect_fail.h:359`) — roughly 35 s per session.

**What the attacker gains.** With the room code alone, no wire observation, no
server access, and no participation in the original race: they play the match
as the peer. The host believes it is playing its friend; the attacker's inputs
drive the host's rollback engine and the host's inputs are delivered to the
attacker. If the attacker cannot simulate the game, the same primitive is a
clean DoS — the room dies and the code is burned (the nonce re-rolls per
hosting attempt, `direct_p2p.c:3740`). The attacker picks which.

**Precondition, honestly stated.** The substitute's datagrams must reach the
host from a *new* source port on the shared public IP. That requires the host's
NAT to be full-cone or address-restricted; address-and-port-dependent filtering
(`tools/netplay/natmatrix/natns.sh:86`) admits only the exact punched
`ip:port`. **This is exactly the same precondition the feature itself needs** —
the S2 retry's fresh socket also arrives from a new port — so the attack
surface is coextensive with the relearn's working set. It cannot be narrowed by
NAT assumptions without also disabling the rescue.

### Could not verify

- **`_session_magic`'s derivation.** `third_party/GekkoNet` ships headers plus
  `build/lib/libGekkoNet.a` only (`find third_party/GekkoNet -type f` → 10
  files, no `.cpp`), so the seeding of `MessageSystem::_session_magic`
  (`backend.h:191`) could not be read. It cannot be a pre-shared secret with the
  legitimate peer — that peer learns it over the same wire — but the exact
  derivation is unread.
- **No live two-host exercise.** The MiSTer device lock is held by another
  session; nothing was run on hardware. The verdict is a code + unit-harness
  reading, not a demonstrated capture.
- The CGNAT / shared-NAT premise itself is a network property, taken as given.

### Mitigation — the options as weighed (the RECOMMENDED one is now built)

Weighed against the demonstrated availability win (late-punch converted a real
`FAILED_HANDSHAKE` into a connect at +11.4 s):

- **REJECTED — clear `s_actual_peer` / `s_retarget_active` in
  `LatePunch_Disarm`.** This does not work and would break the feature. Disarm
  fires at `GekkoSessionStarted` (`netplay.c:1827`); a window-2 relearn is
  applied to the adapter but *not* to GekkoNet's registration, so clearing the
  retarget at session start would send every packet back to the dead original
  port and kill exactly the session the rescue just saved. It also does not
  touch window 1, where the substitute is already canonical. Cross-session
  hygiene is a non-issue: `SDLNetAdapter_SetCanonicalPeer` (`netplay.c:1599`)
  re-seeds both fields on every `configure_gekko`.
- **REJECTED — default the kill switch on.** `CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_LATE_PUNCH`
  defaults to `false` (`src/port/config/config.c:122`). Flipping it trades a
  measured, every-session availability win against a same-IP-only risk, and
  restores the "15 s hang + dead room" failure the module exists to remove.
  Keep it as the field-attributable escape hatch it is.
- **RECOMMENDED — bind the relearn to something stronger than source IP, and
  make it once-and-done.** Two changes, both inside `late_punch.c`:
  1. **Lower `LATE_PUNCH_MAX_RELEARNS` to 1** (or gate the second and later
     moves behind evidence the first endpoint went quiet). One relearn covers
     the S2 retry, which is the only sequence the header claims as legitimate;
     it removes the 8-port cap-burn that makes capture deterministic, and it
     makes the *first* mover win rather than the last — the legitimate peer,
     which is already punching, rather than a party that has to arrive later.
  2. **Require the relearned endpoint to prove liveness before it becomes the
     send target** — a challenge/response over the existing token
     (nonce-in-punch, echoed) rather than accepting the first token-valid
     datagram from the IP. The token is derivable from the room code, so this
     does not authenticate identity; it does make capture require a live
     round-trip at the exact moment of the move rather than a one-shot spray.
  Neither weakens the rescue: a genuinely retrying joiner answers a challenge
  and moves once.

**The trade.** Keep late-punch on by default and keep its availability win; pay
for it by shrinking the relearn from "8 moves, last one wins, frozen forever" to
"one move, first one wins, liveness-checked". That preserves the +11.4 s rescue
in full — it needs exactly one move — while removing the property that makes the
same-IP capture deterministic rather than a coin flip. Full identity binding is
not available at this layer: the punch token is room-code-derived by design
(`room_code.h:221-236` calls the code key material), so a same-IP holder of the
code is inside the token's trust boundary no matter what this module does.

**No test distinguished the two same-IP cases** at the time this was written.
`src/netplay/test_punch_predicates.c` and `src/netplay/test_late_punch.c`
covered the cap and the foreign-IP refusal; none separated "the peer moved"
from "a second host on the same NAT", and an IP-string compare cannot. That is
now the property the liveness challenge tests, below.

### Mitigation — IMPLEMENTED

Both recommended changes, plus the responder they need on the other side.

**1. `LATE_PUNCH_MAX_RELEARNS` 8 -> 1** (`src/netplay/late_punch.h:163-200`).
The proven rescue needs exactly one move. Critically, the budget is now spent
by `late_punch_promote` (`late_punch.c:139-155`) — i.e. only by an endpoint
that ANSWERED — so an endpoint that merely sends cannot consume it. Without
that, a cap of 1 would be a one-datagram freeze, which is the old failure made
cheaper.

**2. A liveness round trip gates the retarget.** A token-valid punch from a new
port on the peer IP no longer moves anything; it calls `late_punch_nominate`
(`src/netplay/late_punch.c:304`, defined at `src/netplay/late_punch.c:95`). The
module draws 8 CSPRNG bytes with `Stun_MakeProbeNonce`
(`src/netplay/stun.c:167-175`), sends them to the nominee as a 26-byte probe
frame, and retargets only on a RESPONSE that comes back from that exact port
carrying that exact nonce — `Stun_ProbeNonceEqual`
(`src/netplay/late_punch.c:256-258`). No CSPRNG, no challenge, no relearn:
`Stun_MakeProbeNonce` failing is fail-closed
(`src/netplay/late_punch.c:116-124`). An unanswered nomination expires after
`LATE_PUNCH_CHALLENGE_MS` (1500 ms) and costs nothing —
`late_punch_challenge_due` (`src/netplay/late_punch.c:319-339`); one candidate
is held at a time, first come, so a flood cannot make the module challenge
whoever spoke last.

**The frame** (`stun.h`, `STUN_PUNCH_PROBE_*`): `"3SX_PUNCH" | token[8] |
kind[1] | nonce[8]`, 26 bytes. The punch prefix is retained deliberately —
every receive path already drops punch-prefixed traffic before GekkoNet —
`Stun_HasPunchPrefix` (`src/netplay/sdl_net_adapter.c:386-393`) — the MIST pump
offers it to `LatePunch_HandleDatagram` first (`src/netplay/netplay.c:1243-1249`),
and `classify_host_datagram` still ignores it because it demands the exact
17-byte payload: `Stun_IsPunchPayload` (`src/netplay/direct_p2p.c:1063-1066`). Nothing
downstream had to change to stay safe, including on a build that predates the
frame. The kind byte stops a challenge/response pair ping-ponging.

**3. The racing peer answers.** In the rescue the endpoint being challenged has
NOT handed off — it is a `StunPunchLeg` mid-race — so `Stun_PunchOffer` parks
the answer and `Stun_PunchPump` emits it to the challenge's source port
(`stun.c`, `StunPunchLeg.echo_*`). A probe frame carries the same prefix and
token a punch does, so it also confirms/retargets the leg exactly as a punch
would: the rescue does not get slower because the other side now verifies.

**Why a room-code holder cannot predict the challenge.** The token is
room-code-derived and therefore public to them; anything checked against values
they can compute is a slower version of the same hole. The nonce is the one
value in the exchange the nominee did not choose and cannot derive: producing
it requires having RECEIVED our datagram at the address claimed. Blind and
off-path injection is now dead outright — a forged source that cannot receive
can never answer.

**What it does NOT buy, stated plainly.** A same-public-IP party holding the
room code and running this build still receives the challenge and can still
answer it. It can therefore still win the single relearn if it gets there
first. Full identity binding is not available at this layer for the reason
already argued above. What changed is the cost: from "send one datagram, own
the session, deterministically" to "hold a live socket at the claimed endpoint,
beat the real peer's own retry to the single budget, and complete a round trip
inside 1500 ms".

**Unit pins (all shown RED against a mutation).** `--test-punch-predicates` now
runs 10 tests / 1680 assertions (`EXPECTED_TESTS` 7 -> 10, floor 550 -> 1400);
`--test-late-punch` gained A13/A14/A15 and rewrote A3/B3/A9. New coverage: a
nomination alone moves nothing and spends nothing; every single-bit nonce error
is refused; the right nonce from the wrong port or a foreign IP is refused; a
replayed response buys nothing; sixteen challenges never reuse a nonce and are
never all-zero; eight distinct ports after a verified move are all refused
without a challenge being opened; a nominee in flight cannot be displaced; an
unanswered nominee expires without spending the budget and the real peer is
still relearnable after it; the challenge is observed ARRIVING at the nominee
over a real socket and the response is built from the nonce that came off the
wire; and `Stun_PunchOffer`/`Pump` answer a challenge to the port it came from.

**Mutations proved (all RED; object file AND linked binary deleted before each
run, per the doctrine above).** M1 cap 1->8; M2 nomination retargets
immediately (the pre-fix behaviour); M3 nonce check removed; M4 source-port
check removed; M5 budget spent at nomination instead of verification; M6 nonce
compare `|=` -> `^=`; M7 probe length exact -> minimum; M8 a RESPONSE is echoed
too; M9 foreign-IP guard removed from the probe path; M10 the challenge never
expires; M11 the nonce is a constant (fail-open CSPRNG); M12 the candidate slot
is displaceable; M13 the budget is not enforced at nomination; M14 the racing
leg never answers; M15 the parked answer is never sent. M4, M6, M7, M9, M11,
M12 and M13 are caught by `--test-punch-predicates` only; M10, M14 and M15 by
`--test-late-punch` only.

**THE RESCUE STILL WORKS — re-run on the netns rig against the fixed code.**
`tools/netplay/natmatrix/rescue_scenario.sh` is Linux-netns only (`sudo -n ip
netns exec`, `iptables -m string --algo bm --string "3SX_PUNCH"`), so it was run
in a privileged `debian:trixie` container on the Docker Desktop VM kernel
(`6.12.76-linuxkit aarch64`, `xt_string` present), with SDL3/SDL3_net/GekkoNet
built for the guest and the probe configured `session phase ON`. Same rig,
same `--lift-s 11`, one variable:

| phase | before (`81571c85`) | after (this fix) |
| --- | --- | --- |
| control | both sync, host 633 ms | both sync, host 641 ms |
| baseline | host `FAILED_HANDSHAKE`/`DEADLINE`, joiner `FAILED_BILATERAL` | identical |
| rescue | host relearns **x1**, syncs 11423 ms; joiner 32 ms | host relearns **x1**, syncs **11409 ms**; joiner 32 ms |

`RESULT: GREEN`, exit 0. The added round trip costs nothing measurable — the
challenge and its answer both ride the pre-existing punch cadence, and the
joiner's leg confirms on the challenge itself.

**The rig's integrity guards were shown FIRING, not assumed.** A `p2p_probe`
once went 16 hours unlinked while its drivers exited 0, so none of the above is
worth anything until the instrument can fail:

- *vacuous baseline -> 3.* Re-run with `--lift-s 0` so the injection misses:
  baseline ends `HANDOFF/STARTED`, `RESULT: VACUOUS`, exit **3**.
- *rig contamination -> 5.* A two-row `TOPOLOGY_UP_FAILED` `.jsonl` fed to
  `tools/netplay/natmatrix/summarize.py`: `## REFUSING TO SUMMARIZE`, exit **5**.
- *a pass that syncs without a relearn -> 1.* A scratch copy of the scenario
  with `RH_RELEARN` forced to 0 after the scrape: the run genuinely relearned
  once, and the guard still refused it — `RESULT: RED -- the pair synced but the
  host applied no relearn`, exit **1**.

**Still unproven.** Nothing was exercised on the MiSTer device (its lock is held
by another session). Cross-version behaviour is a known degradation, not a
regression: a peer on a build without the probe frame consumes the challenge as
punch-shaped bad-token traffic and never answers, so no relearn happens and the
pair falls back to the pre-#119 delayed failure. The rig exercises the
LEGITIMATE mover only — it has no adversary namespace, so the same-IP capture
itself is pinned at unit level, not on the wire.

---

## #129 — ROM search trim + a legible miss — CLOSED

Candidate set cut from 19 directories x 2 basenames (38) to 5 x 2 (10):
`src/arcade/arcade_char_data.c`. Kept `/media/fat/games/mame` (update_all's
default, and the only path ever exercised), `/media/fat/mame`,
`/media/fat/_Arcade/mame` (Main's own fallback) and both `/media/usb0`
layouts. Dropped `usb1`–`usb5`, `/media/network`, `/media/fat/cifs` — on the
target device `/media/network` and `/media/fat/cifs` do not exist even as
mount points, while `/media/usb0`..`/media/usb7` do. Nothing was kept against
the brief.

Upstream order re-verified against Main_MiSTer @ `915ca339`, and **three
citations in the pre-existing comment were wrong**: the order list is
`file_io.cpp:1048-1056` (not `:1048-1055`, which cut off
`/media/fat/<prefix>/<dir>` — the one path that actually works), the bare
`/media/fat/mame` check is `file_io.cpp:1118-1122` (not `:1124-1127`, which is
the `games/` branch), and `findPrefixDir` ends at `:1133` (not `:1132`).

A miss now reads differently in each of the three cases — no directory
existed / a directory existed but held neither basename / a zip was found and
**rejected by content verification** (the wrong-revision case, previously
indistinguishable from having no ROM). Bounded: a normal miss is one line.
`rom_load.c` untouched — the classification is done by `SDL_GetPathInfo` in
`arcade_char_data.c` and never gates a `Rom_Load` call, so what gets FOUND is
unchanged.

Induced, not asserted: the no-directory case and the found-and-rejected case
were each provoked against the real binary and print different text (a
valid-zip-with-wrong-contents drives the second). The directory-exists-but-no-
basename case needs a writable `/media`, so it is induced on the device under a
tmpfs overlay rather than on the host.

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
   (`6b17dc73`). `Netplay_SetStunSocket` (`src/netplay/netplay.c:2756`) now
   calls `LatePunch_Disarm()` on the destroy branch only.
   **The failing case could NOT be constructed, and the attempt failed on a
   real structural guard rather than on luck:** `do_handoff` (`direct_p2p.c:4522`)
   is the only production caller, it nulls `s_work.stun.socket` immediately
   after (`:4532`), and Tick's HANDOFF case re-enters `join_tick_handoff`
   only while that field is non-NULL (`:5783`) — so a second in-session
   handoff is unreachable. Both terminal paths disarm before the destroy by
   explicit ordering (`netplay.c:2543` vs `:2562`; SessionStarted `:1798`).
   What remains is that the invariant lives in a *different* TU, so it is now
   pinned by `test_late_punch` A12 (`src/netplay/test_late_punch.c:621`),
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

`Stun_IsBindingResponse` (`src/netplay/stun.c:1146`) had zero tests despite
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
  sticky until `LatePunch_Tick` sends (`late_punch.c:342`), so after the eight
  accepted moves an answer is legitimately already owed. Separating the two
  needs a Tick, which needs a socket, which is `test_late_punch.c`'s job.
  Faking it would be the vacuous version.
- Constant-time TIMING itself — see the gate above.

**Frame-data: SKIPPED, deliberately.** Nothing in this lane reaches the
simulation. Two new test translation units, six test-only trampolines, one
read-only test-only accessor, and a Python source-shape check; the only
non-test source edits are inside `#ifdef ENABLE_NETPLAY_TESTS`.

**Priority 2 — CLOSED.** See `--test-netplay-units` below.

**Priority 3 — CLOSED.** See `--test-netplay-units` below.

### `--test-netplay-units` — priorities 2 and 3, one harness

`src/netplay/test_netplay_units.c`. **15 tests, 1059 assertions, 0.18 s.**
Registered the same way every other harness is: a `Configuration` field, an
`OPT_BOOLEAN` in `src/args.c`, a forward decl and a dispatch `if` in
`src/main.c`. Harness count 13 → 14.

**P3 — what moved, and the number that justifies it.** Per-test wall clock was
measured directly (temporary `SDL_GetTicks` instrumentation around each call in
`Netplay_Test_BilateralPunch`), not estimated:

| test | ms |
| --- | --- |
| `test_race_worst_case_timing` | 26447 |
| `test_race_two_peer_convergence` | 17956 |
| `test_s7_lost_mapping` | 11776 |
| `test_joiner_cookie_handshake` | 10481 |
| `test_joiner_self_deliver` | 10236 |
| `test_s7_review_fixes` | 7549 |
| `test_race_rearm_releases_address_ref` | 6589 |
| ... 26 more | |
| **whole harness** | **110471** |

The harness is one linear dispatch, so what an engineer iterating on a block
actually pays is the CUMULATIVE cost to REACH it. Summing the dispatch order:
the NAT-PMP/PCP wire codec sat **~88 s** in; the ladder-shape and renewal-
cadence tables sat **~90 s** in; the earliest of the moved blocks
(`test_failure_taxonomy`) sat **~13 s** in. **All 13 now run in 0.18 s.**

**Be precise about what did NOT change: the gate's total wall clock.**
`--test-bilateral-punch` was 110.47 s before and 110.24 s after — noise. The
extracted blocks were *pure*, so they were never what cost the time; the
scenario tests that do cost it are all still there, which is the instruction and
also correct (a mocked unit would pass vacuously in their place). The win is the
cost to REACH a decision, not the cost of the suite. The 47 s + 31 s figures in
the original framing did not reproduce: `test_host_cookie_rejected` measures
**745 ms**, not 31 s — it drives a mocked clock and only *reports*
`waited_ms=30060`.

The 13: `session_key_stability`, `lan_bypass`, `failure_taxonomy`,
`host_datagram_gate`, `rendezvous_cookie_codec`, `rendezvous_frame_router`,
`punch_gate_throttle`, `race_budget_wrap_safety`, `natpmp_codec` (the whole
RFC 6886/6887 build+parse section), `natpmp_ladder_shape`, `pcp_short_error`,
`nonpublic_gate`, `portmap_renew_cadence`. Moved, not copied — nothing is
asserted twice, `test_bilateral_punch.c` went 8312 → 6840 lines and kept every
scenario test.

**P2a — the ORCH cascade, derived independently.** `unit_orch_cascade`
(`src/netplay/test_netplay_units.c:1827`). The `ORCH_*` macros are file-local to
`direct_p2p.c` and `DirectP2P_OrchWorstCaseMsForRole` reads its budgets from
live config, so the arithmetic could only be reached by writing config and
reading a role bound back — which pins the SLOPE and never the value. New hook
`DirectP2P_TestHook_OrchCascade` (`src/netplay/direct_p2p.c:6396`) evaluates the
real macros at CALLER-SUPPLIED budgets, no config, no clock. The test then
re-lists the primitives (`HOST_STUN_MAX_RETRIES`, `WORKER_STARTUP_DELAY_MS`,
`PORTMAP_PROBE_BUDGET_MS`, `RESOLVE_POLL_MAX_MS`, `JOIN_MAX_ATTEMPTS`,
`STUN_PUNCH_CONFIRM_MS`, the four clamps) as its OWN literals and recomputes the
ladder itself. 7 (stun, race) rows × 6 derived values, plus both slopes swept,
plus the corner where the `MAX` branch flips.

That duplication is the alarm, and it is deliberate: change
`WORKER_STARTUP_DELAY_MS` in `direct_p2p.c` and this test goes red, so the
change becomes a decision instead of a silent slide. The four numbers #131 got
wrong are ALSO pinned as literals (ceiling ladder 87050, shipped defaults 43050,
the corner, and #96's 120800 upper guard), because a careless "fix the test"
would edit the macro and the re-derivation together and leave the table green.

**P2b — natpmp timeout math takes the clock.** `np_remaining_ms`
(`src/netplay/natpmp.c:920`), the new `np_step_deadline` (`:932`, hoisted out of
`np_transact`'s inline expression) and `np_phase_deadline` (`:1156`) all take
`now_ms` instead of calling `SDL_GetTicks()`. The callers still read the clock —
this is a testability seam, not a virtual clock. Hooks
`Natpmp_TestHook_RemainingMs` / `_StepDeadline` / `_PhaseDeadline`
(`src/netplay/natpmp.c:869`) forward to the production functions rather than
reimplement them. `unit_natpmp_deadline_math` (`:1976`) then sweeps what elapsed
time cannot produce on demand: an already-expired deadline, the exact deadline
edge, the 2000000000 ms clamp, a phase ceiling truncating a rung, a suppressed
rung inheriting the whole phase, out-of-range steps, `now` up near 2^64, and
three back-to-back phases fitting `NATPMP_PROBE_BUDGET_MS` exactly — the H-6
property, in microseconds instead of the 3896 ms it took to measure.

**Mutations (object file AND linked binary deleted before each run).**
V1 a unit dropped from the dispatch → RED (`EXPECTED_TESTS`).
V2 the RFC 6886 ladder grown back to 5 rungs → RED.
V3 the #131 bug restored (startup delay paid once) **with every ORCH
`_Static_assert` neutralised**, so the unit test is the only detector left →
RED.
V4 a primitive changed under the test (`HOST_STUN_RETRY_BACKOFF_MS` 5000→4000),
asserts neutralised → RED ("macro says 40050, the independently derived value is
43050").
V5 the `MAX` dropped from `ORCH_HOST_WORST_CASE_MS`, asserts neutralised → RED
at the corner.
V6b `np_remaining_ms` one ms too generous → RED.
V7 `np_step_deadline` drops the phase ceiling → RED.
V8 `np_remaining_ms` drops the 2000000000 clamp → RED.

**V6 is an EQUIVALENT mutant, not a gap.** `now_ms >= deadline_ms` →
`now_ms > deadline_ms` survived, and it should: at `now == deadline` the
fall-through computes `left = 0`, `0 > 2000000000u` is false, and it returns 0 —
byte-identical behaviour for every input. The early return is an underflow
guard, not a boundary decision. V6b exists because of it.

**Non-goals, deliberately:** the punch race, split brain, the PROMISED
LISTENING INTERVAL, the reclaim boundary, GekkoNet/rollback determinism, UPnP.
All interaction bugs, where a mocked unit passes vacuously.

---

## Smaller open items

- **#125 — CLOSED.** `--test-connect-observability` flaked only under
  concurrent gate runs. **Reproduced first** (4 concurrent runs, 2 rounds: 3 of
  the 4 failed in round 2; a later 12-run batch failed 8 times), then fixed,
  then re-run 16/16 clean.

  **The theory in this line was half right and the diagnosis it implied was
  wrong.** It is not one test and it is not only the directory. Three distinct
  mechanisms, all measured:

  1. **Shared log FILENAME.** The session log is
     `<PrefPath>logs/netplay-<utc_ms>.log` — `netplay_log_open`
     (`src/netplay/netplay.c:529`) — so
     processes that start in the same millisecond open the SAME FILE. Measured:
     three of four concurrent runs all opened
     `netplay-1788111677761.log`. Symptom: `test4-mt-sink: 4 of 800 MT lines
     missing or torn` — interleaved writers, nothing wrong with the sink.
  2. **The harness picked someone else's file.** `obs_find_new_log` took the
     NEWEST `netplay-*.log` in the directory past a timestamp. Measured: those
     same three processes all validated against `netplay-1788111677763.log`, a
     fourth process's file, and reported `800 of 800 MT lines missing or torn`
     about a file they had never written a line to. The end-of-run cleanup
     removed that file too — i.e. it could delete a live log belonging to
     another process.
  3. **Cross-process truncation.** `fopen(path, "w")` on a colliding name, plus
     the #44 prune keeping the 20 newest, produced
     `session file grew from 261995 to 0 bytes AFTER the TRUNCATED marker` and
     `wrote 20000 padded lines and the session file never published a TRUNCATED
     marker`. That second one is the reported `test6-byte-budget` symptom.

  **Why the directory had to be the fix and not just the accessor.** With the
  accessor fix alone (each process reading its own path), 5 of 12 concurrent
  runs still failed — mechanisms 1 and 3 are writer-side. Measured, both ways.

  **Fix, three parts.** `Paths_GetPrefPath` (`src/port/paths.c:46`) now honours
  `THIRDSARM_HOME` on **every** port, not just MiSTer/Miyoo. On the host build
  there was previously no way at all to move this directory: `SDL_GetPrefPath`
  goes through `NSApplicationSupportDirectory` on macOS, which ignores `$HOME`
  — measured, a harness run with `HOME=/tmp/fakehome1` still wrote to
  `~/Library/Application Support/CrowdedStreet/3S-ARM/`. `tools/gates/run-gates.sh:198`
  then gives every harness invocation its own `$$`-keyed home, which makes
  concurrent RUNS hermetic too, not just concurrent harnesses within a run. And
  `Netplay_TestHook_SessionLogPath` (`src/netplay/netplay.c:515`) hands the test
  the path this process actually opened, so the harness is correct even in a
  shared directory.

  **Result:** 16/16 concurrent runs clean, 0 failures.

  **Deliberately NOT fixed, with the reason.** The production filename
  collision itself. It would need `netplay-<ms>-<pid>.log` and a widening of
  `netplay_log_name_stamp` (`src/netplay/netplay.c:376`), which the file calls
  "the entire safety argument for the prune". It cannot bite in practice: two
  3S-ARM instances on one machine is not a supported configuration, and the
  natmatrix probes link `netplay_stub.c` (`netplay_stub.c:42`), so they write no
  session log at all. Widening a safety-critical predicate to paper over missing
  test isolation is the wrong trade.
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

---

## #134 — the five boot-time AFS load failures — CLOSED (root-caused and fixed)

Every device smoke run logged exactly five
`ファイルの読み込みに失敗しました。ファイル番号：N` lines for N = 9, 10, 1454,
1456, 1458, and nothing consumed the error. **Not a missing asset and not a cut
PS2 asset — a lost wakeup in the port's async I/O layer.**

**The numbers are AFS entry indices**, emitted by `load_it_use_this_key`
(`src/sf33rd/Source/Game/io/gd3rd.c:355-380`, the message at `:378`). All five
resolve, and all five are physically present in the shipped archive — parsed
from `SF33RD.AFS` (1535 entries, format per `src/port/io/afs.c:89-160`):

| fnum | name | size | verdict |
| --- | --- | --- | --- |
| 9 | `default.bin` | 61,440 | present, read inside EOF |
| 10 | `scrscrn.ppg` | 50,348 | present, read inside EOF |
| 1454 | `ef02_usa.bin` | 1,128,236 | present, read inside EOF |
| 1456 | `ef06.bin` | 595,800 | present, read inside EOF |
| 1458 | `ef40.bin` | 170,688 | present, read inside EOF |

The sibling "file number is abnormal" message (`gd3rd.c:311-312`, the
`AFS_GetFileCount` bound test and the `flLogOut` it guards) was never logged,
which already ruled out an out-of-range index.

**Root cause.** `AFS_ReadSync` (`src/port/io/afs.c`) drained the async queue and
decided whose completion it had by reading `request->index` **after**
`process_asyncio_outcome` had run. That handler `SDL_zerop()`s a slot whose
deferred close has landed, which resets `index` to 0 — so every such completion
was indistinguishable from slot 0's, and `AFS_ReadSync(0, ...)` returned while
its own read was still in flight. `fsCheckFileReaded` then saw
`AFS_READ_STATE_READING`, `fsFileReadSync` reported failure, and the retry loop
in `load_it_use_this_key` (`gd3rd.c:355-380`) logged and retried — the retry succeeding, because nothing was
ever wrong with the file. Second half of the same defect: `AFS_Open` reuses the
first free slot, so a CLOSE completion left over from the slot's previous user
carried the same index and would end the wait just as wrongly.

**Fixed** by reading the slot identity before the handler can zero it and by
ending the wait only on this slot's own `SDL_ASYNCIO_TASK_READ`.

**Reproduced and proven on the host, no device needed.** The bug is in shared
port code, not MiSTer-specific:

```
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  build/host-release/3S-ARM.app/Contents/MacOS/3S-ARM --headless --perf-capture 600
```

Before: the same five numbers, in call order `9, 1456, 1458, 10, 1454`
(`Init_load_on_memory_data` at `src/sf33rd/Source/Game/rendering/aboutspr.c:102-109`,
then `Scrscreen_Init` at `src/sf33rd/Source/Game/ui/sc_sub.c:425-437`, then
`checkSelObjFileLoaded` at `src/sf33rd/Source/Game/rendering/texgroup.c:558`).
After: zero, with 600 frames still completing.

**The 379 ms startup outliers are a consequence, not a separate cause.** The
outlier test (`src/port/sdl/sdl_app.c:3627`, threshold 50 ms — its comment
still says 25 ms and is stale) measures a window that encloses the blocking
loads (`src/main.c:902-904`). Each failure made the game read the file twice —
for fnum 1454 that is 2 x 551 x 2048 = 2,256,896 bytes of blocking I/O in one
frame. The host reproduces the failures but logs **no** outlier at all, so the
379 ms figure is the device's slower storage crossing the threshold on the
doubled read. **Not verified on hardware** — the device lock was held.

---

## Citation-cost measurement (analysis only, no scheme landed)

Re-measured at `522e574c`, linter `check_doc_citations.py` sha256
`3acc6c44c93fd0d6cf567acf5fff76501f8e2a4eacb2c5ba005102eadc508073`. **The
working tree also carried three other lanes' uncommitted edits at the time**,
so these are counts over that tree, not over the commit alone.

That caveat is not decoration. The same script run twenty minutes earlier at
`81571c85` gave 3,521 line anchors, 1,020 clean and 996 moved, against 3,540 /
1,014 / 1,017 here. A ceiling fitted to either number would have been red at
the other — which is the concrete form of the hazard that sank the previous
`<tree> [unanchored-citation]` ceiling.

- **3,540** line-anchored citations tree-wide; 12,015 positionless references
  (3,042 bare paths + 8,973 backticked symbols). Line anchors are 22.8% of all
  references.
- Only **2,217 of the 3,540 make a checkable claim at all**. Of those: 45.7%
  clean, 45.9% merely moved, and **8.4% (186) name a file that has never
  contained the symbol the prose claims.** The linter is silent on that last
  class by design ("absence of proof is not a finding"), so the class most
  likely to be a genuine defect is the one nothing reports. That 186 was
  identical at both commits.
- Line-anchor invalidation over the last 30 h alone: **69 of 283** citations
  carrying a uniquely-locatable symbol (24.4%) had that symbol move, i.e. would
  have needed a hand repoint. The symbol itself stayed resolvable in every one.
- **22 commits on this branch have "repoint" as their subject**, all inside that
  same 30 h window (1,019 insertions / 486 deletions across 84 file-touches).
- In the 11 enforced scopes: **85.2% of resolvable citations are pure pointers**;
  only 14.8% restate a value the code defines (17.6% under a looser literal
  test). Hand-auditing 12 sampled "stale value" findings, every one I could
  check concretely was a false positive — bare line numbers in prose, markdown
  table row bleed, or a value the code computes rather than spells. **Value
  duplication is not a significant staleness surface in the enforced scopes.**
