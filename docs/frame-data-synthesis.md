# Frame Data Synthesis

How the training-mode frame-data overlay derives Startup / Active /
Recovery / Total / Advantage and renders the visual frame meter from
the running PS2-decomp engine state. Captures the design, the engine
quirks we work around, the verification status, and the open issues.

This is a *synthesis* doc — it explains how the system fits together
and why each piece exists. It is not an investigation report; the
known unresolved discrepancies are listed with what we know, what we
hypothesize, and what would need to be done to pin them down.

**This doc is the canonical reference for frame-data work.** Future
agents should read it cover-to-cover before any frame-data
investigation. When findings change, update this doc and prune what's
stale; do not create sibling investigation docs.

## Arcade ground truth

`docs/arcade-frame-data/q.json` (50 entries, fetched from
[Coccis77/thirdstrikedatabot](https://github.com/Coccis77/thirdstrikedatabot)
on 2026-05-04). Every overlay claim about correctness is graded
against this file. When `q.json` and the trace disagree, the trace is
the suspect; when `q.json` and the user's gameplay observation
disagree, escalate the question rather than dismissing one side.

## Status snapshot (2026-05-04, end-of-day truth-up)

This snapshot reflects the **current code state** after a day of
shipping + reverting fixes. Verified by reading the actual
`frame_data_overlay.c` / `charset.c` / `frame_trace.c` files; not from
prior doc claims.

| Class | Status |
|---|---|
| Q standing/crouching normals (block + whiff) | All match arcade S/A/R exactly. (§13.4 close HP / Back+HP A+1 was a stale-binary artifact — resolved.) |
| Q specials in trace (Dashing Head/Leg ×3 each, Back+Roundhouse) | Match arcade exactly. |
| Universal Overhead — **single isolated press** | **FIXED, confirmed by Phase 3 harness (2026-07-07).** Clean: S=15 A=10 R=5 BLOCK / R=3 HIT — exact arcade match. The residual A+1 (cgix=16 transit cell double-dispatch, §13.7.4) was root-caused, fixed in `a386e057`, and is now verified working (harness + mutation test). **UPDATE (2026-07-07, §13.11):** this row's "A=10 exact arcade match" is now pre-convention history. Under the §13.11 declared-truth displayed-A convention, displayed A is 11 (the declared credit, path-independent across WHIFF/BLOCK/HIT) — see §13.11 for the "two cancelling minus-ones" story explaining why 10 was the right number for the wrong reason. S/R are unaffected. |
| Universal Overhead — **chained / spammed** (same-r1=4 retrigger) | **FIXED, confirmed by Phase 3 harness (2026-07-07, §13.9.4).** `q-uoh-chain-retrigger` (`press MP+MK; wait 38; press MP+MK`) now emits a single FINAL with A=10, matching arcade — a gated anchor-time `engine_a` snapshot (taken at the same cghi=1-dwell anchor §13.5.1 already uses) is displayed in place of the live accumulator whenever the cut committed AND that dwell was interrupted by the retrigger. The two 2026-05-04 force-finalize fix attempts (§13.9.1/§13.9.2) remain reverted / NOT in the code — this fix is a third, distinct, finalize-read-only approach. Mutation tests confirm both the jatix-revoke lever and the new gate's own lever. **UPDATE (2026-07-07, §13.11):** this row's "A=10" is now pre-convention history — the anchor snapshot automatically inherits the §13.11 restore with zero changes to this mechanism, so displayed A is now 11 (tap-1's declared total). The anchor fix itself is unaffected and stays fully load-bearing (§13.9.4's own update note). |
| Multi-move-merge (defender blockstun outlasts attacker recovery, e.g. F=4940 parry-counter chain) | **Unverified — Phase 3 harness (2026-07-07) could not exercise this case.** §13.7.8's original force-finalize fix was reverted; a different mitigation (`engine_a` snapshot at `attacker_idle`, §13.7.7 rec #2) shipped in `a386e057` instead. The only move pair tried in the harness (cr.MK + second button) can't open the merge window at all (cr.MK's -3 block advantage means attacker recovery outlasts blockstun) — needs a positive-advantage move pair (e.g. Jab, +2) in the corpus to actually test the shipped mitigation. |
| High Speed Barrage ×3 | **§13.5.2 shipped.** WHIFF/BLOCK R=26/28/28 exact arcade match (was R+18). Multi-hit visualization preserved (§14). HIT R=21 for all three strengths diverges from the WHIFF/BLOCK-matching figure — first clean HIT capture, part of the HIT-R divergence family (§12/§13.8 item 15). **Formula-test excluded 2026-07-07** (Phase 4 items 4+5): `raw_len - S - engine_a` = 40 for all three, vs arcade 26/28/28 — fails the formula test by 12-14 frames, confirming a different, not-yet-root-caused mechanism (still xfail). |
| Q Capture and Deadly Blow (HCB+K command grab) | **§13.6 + §13.6.1 partner-release shipped.** S=12/13/14 A=2 exact across LK/MK/HK. **R+1 FIXED 2026-07-07 (Phase 4 items 4+5)**: R now reads 42/44/46 — exact arcade match, no residual. Root cause was a mis-attributed A/R boundary (declared-tick collapse), not a mis-sized window; fixed by re-deriving R from the engine credit on the partner-release path only (§13.6.1). KD advantage display shipped (renders "KD"). Corpus xfail removed — now plain PASS with `kd: 1` run-failing. |
| Cr.* low-attack BLOCK adv +2 | **RESOLVED BY REBASELINE (2026-07-07) — did NOT reproduce.** cr.MK/cr.HK/cr.HP BLOCK advantage all match arcade exactly under the Phase 3 harness (pinned RNG, controlled spacing). Hypothesis: the +2 was an artifact of the old manual-capture conditions. Former exception: **cr.LK never classified as BLOCK/HIT at all — SHIPPED 2026-07-07** (issue #14, item 14 of the open-work list, §13.2): dm_stop-sign event-edge fix, now plain PASS. |
| HIT outcomes for normals | **Extensively captured by the Phase 3 harness (2026-07-07)** — 56 PASS / 13 xfail across 69 corpus entries. cr.* HIT adv matches arcade exactly (cr.MP HIT variance resolved: gone under pinned RNG, confirms RNG-driven-dummy hypothesis, `com_sub.c:1875`). New HIT-R divergence family found for HSB/Dashing Leg RH/Throw/UOH (§12, item 15 of the open-work list). |
| Live meter coloring | **§14 shipped.** Live mirror of finalize §8.3 painting; scattered-active multi-hit moves preserve per-frame hits. User-confirmed visually. |
| Taunt + non-attack-move classification | **§13.3 shipped.** No more `S=0` garbage on whiff command grabs and taunts. |
| Multi-character coverage | Q-only. Other characters not yet audited. |
| Git state | **Updated 2026-07-07: everything below is committed on `frame-data-on-mister`** (baseline `mister`, not `main`). Overlay + engine hooks: `a386e057`. Training-mode toggle: `118f7350`. Phase 1 self-validating harness (see `docs/plan-frame-data-harness.md`): `09e4b1d3` (H1+H4 input-script player, trace-path override, auto-exit), `98d429ec` (H2+H3 `training-frame-data` preset, `--test-pin-rng`, dummy guard control), `0b5a1eda` (H5 corpus compiler + checker + `run.sh`), `6861b5ed` (H6 — 69-entry `corpus-q.yaml`, Phase 1 acceptance green). No pushes without explicit consent. |

**Truth-up notes for fresh agents.** Two prior agent claims that were
**falsified** today (don't repeat them):

1. "cghi=102–143 r2=3 moves are parry counters — and we know they
   aren't because `paring_counter[0]` stays 0." The identity claim
   is *unverified*, but the falsification reasoning is wrong:
   `paring_counter[]` is a **bonus-mode score multiplier**, not a
   normal-play parry flag — it's only written under
   `Bonus_Game_Flag == 0 && spmv_ng_flag & 0x80` at
   [`hitcheck.c:710`](../src/sf33rd/Source/Game/engine/hitcheck.c).
   Its value tells you nothing about whether parries happened in
   normal training. Likewise the earlier "`pat_status` 20/22/24/26
   are parry stances per `pls01.c:251`" claim is wrong — that line
   is `check_sankaku_tobi` (wall-jump check), and `pat_status` is
   character-specific cgd cell-data loaded at
   [`charset.c:557`](../src/sf33rd/Source/Game/engine/charset.c), not
   a universal "parry" tag. The cghi=102/105/113/122/129/137 r2=3
   moves are LP/MP/HP/LK/MK/HK normals **launched from the
   forward-walk state** (`r2=3` is set by `check_F_R_walk`
   lever_dir=1 at [`pls01.c:476`](../src/sf33rd/Source/Game/engine/pls01.c));
   whether any given instance was a *post-parry* counter requires
   a parry-event signal in the trace which is not currently
   captured. Distinction not testable from existing data.
2. "User pressed single buttons during UOH spam." Verified false:
   trace shows 288 frames with `sw_new=0x0220` (MP+MK chord) and
   appropriate `cghi=229` resolution. Per
   `feedback-trust-user-actions.md`: every UOH the user pressed is
   real. When numbers look wrong, the trace is the authority and the
   bug is in the engine/overlay reading; do not frame as input error.
3. The §2.8 cghi=1 predicate "would regress Dashing Leg Attack." That
   trace was a stale capture; fresh trace shows the predicate gates
   on dur≥3 and Dashing Leg's transient cghi=1 (1–2 frames) does not
   trigger. Concern obsolete.

---

## 1. What the overlay shows

Two horizontally-stacked meter rows (attacker on top, defender below)
of `FD_METER_LEN = 72` cells × `FD_CELL_W = 4` px = 288 px wide,
centered on the 384-wide canvas at `y=190` and `y=198` respectively.
Cells are alternate-shaded (every other cell darkened to 75%
brightness) so individual frames are visible without spending pixels
on borders. A numeric line above the meter at `y=178` shows
`S{startup} A{active} R{recovery} T{total} {±advantage}`. Advantage
is colored green for `+`, red for `-`, white for `0`.

Layout constants and colors:
[`frame_data_overlay.c:38-68`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).

Cell kinds (with attacker / defender / shared meanings):
[`frame_data_overlay.c:70-91`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).

Render entry points: `frame_data_overlay_tick()` runs once per game
frame in `game_step_0()`
([`main.c:616`](../src/main.c)); `frame_data_overlay_draw()` runs in
the present path.

---

## 2. Engine context the synthesis depends on

The 3rd Strike engine drives each character move as a sequence of
animation cells. Every game frame, `char_move()`
([`charset.c:417`](../src/sf33rd/Source/Game/engine/charset.c))
decrements a per-cell counter `cg_ctr` and, when it reaches zero,
advances `cg_ix` to the next cell and reloads `cg_ctr` from the
cell's data via `set_jugde_area()`
([`charset.c:2778-2795`](../src/sf33rd/Source/Game/engine/charset.c)).

Key per-WORK fields the overlay reads (via the `PLW`/`WORK` struct,
captured by `fd_snap_player()`,
[`frame_data_overlay.c:183-213`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)):

| Field | Meaning |
| --- | --- |
| `routine_no[1]` (`r1`) | Top-level state. `0` idle, `4` attacking, `1` reacting (hitstun/blockstun), `2`/`3` thrown |
| `routine_no[2]` (`r2`) | Sub-state. Used at parry recovery to discriminate parry kind (high front=31, high back=32, low=33, air=34) |
| `cg_ix` | Current cell index in attack-animation table |
| `cg_ctr` | Frames remaining on current cell (loaded fresh per cell) |
| `cg_ja.atix` | Engine's active-hitbox index for this cell. Non-zero ⇒ "this cell has a live attack hitbox" |
| `cg_att_ix` | Older active-hitbox signal — used by the live classifier as a secondary "is this an active frame" hint |
| `cg_hit_ix` | Hit-data table index. `!= 1` is the active signal for moves whose engine bypasses `h_att` (Q's close LK/MK) |
| `cg_cancel` | Cancel-window flag (non-zero ⇒ move can be canceled) |
| `h_att` | Pointer into attack-box array. `att_box[4][4]` dimensions tell the post-tick "is the box still alive" question |
| `hit_stop` | Engine pause counter — frozen-frame counter set on contact for both players (independently) |
| `dm_stop` | Defender hit-confirm. `0 → < 0` transition is the contact edge |
| `dm_count_up` | Defender hit count. Increments only on hit (not on block / parry) |
| `pat_status` | Pose status. Used to discriminate parry kinds in some branches |

The PS2 engine code is ours — these field semantics come from reading
the actual engine, not from external docs.

---

## 3. Engine hooks we own

The overlay cannot derive everything from a per-frame snapshot
because the engine resets some fields *during* its own tick, before
our overlay observes them. We hook three engine sites to capture the
signals at the moment they're true.

### 3.1 `fd_engine_hitbox_active[2]`

Set inside the engine when `cg_ja.atix != 0` for a player WORK; reset
by the overlay at the top of `game_step_0()` so the flag is freshly
sampled each game frame.

- Set in `set_jugde_area()`:
  [`charset.c:2778-2795`](../src/sf33rd/Source/Game/engine/charset.c).
- Set in `char_move()`:
  [`charset.c:421-425`](../src/sf33rd/Source/Game/engine/charset.c).
- Reset:
  [`main.c:547-548`](../src/main.c).
- Read by the overlay's `h_att_set` derivation in `fd_snap_player()`:
  [`frame_data_overlay.c:197-205`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).

The reason for the redundant capture in two engine sites is that for
some moves (Q's close LK/MK in particular) `cg_ja.atix` becomes
non-zero only inside `char_move()` for one tick before being cleared,
and `set_jugde_area()` does not run on those frames. Capturing in
both places means we never miss a frame where the engine itself
considered a hitbox live.

### 3.2 `fd_engine_active_count[2]`

A per-player accumulator of `cg_ctr` values across all cells with
non-zero `cg_ja.atix` during the current move.

- Accumulator code:
  [`charset.c:414-457`](../src/sf33rd/Source/Game/engine/charset.c).
- Cleared by the overlay on the `r1: 0 → 4` move-start transition:
  [`frame_data_overlay.c:559-591`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).

Critically, the accumulator dedups by `cg_ix` via
`fd_prev_active_cgix[]` so the same cell isn't summed across the
multiple `char_move()` calls within its display window. Each active
cell contributes its freshly-loaded `cg_ctr` exactly once, on the
tick when `cg_ix` changes.

The sum (1 + 10 + 5 = 16 for Q's close HP, 2 + 2 = 4 for Q's far LP,
etc.) matches the arcade-published Active count across Q's normals.
This is the canonical active value the overlay uses.

#### 3.2.1 Sentinel cgctr handling (per-frame count)

Some active cells carry `cg_ctr >= 200` as an engine sentinel meaning
*"display indefinitely until a cancel or external trigger fires"* —
e.g. HP `cg_ix = 40` carries `cg_ctr = 250`. The cell does not advance
when cgctr decrements normally; it is held until something else (often
the parry/super state machine) advances `cg_next_ix`. Actual frames
spent on the sentinel cell vary by move: close HP holds it for **11**
char_move calls (1 entry + 10 same-cell), back+HP for **9** (1 entry +
8 same-cell). Verified 2026-05-04 via `[CM]` log direct count.

The accumulator counts sentinel cells **per char_move call** rather
than capping at a fixed value
([`charset.c:421-457`](../src/sf33rd/Source/Game/engine/charset.c)):

- **Entry call** (cgix changes into a sentinel cell): adds **0**.
  The entry char_move call is the same one that advances out of the
  *previous* cell; counting it would double-count that exit frame.
- **Subsequent calls** while still on the sentinel cell (cgix
  unchanged, `cg_ja.atix != 0`, `cg_ctr >= 200`): adds **1** each.

`char_move()` doesn't run during attacker hitstop, so hitstop frames
naturally don't contribute. The result is the *actual* number of
frames the engine spent with the sentinel cell's hitbox live —
matches arcade across both close HP (1 + 10 + 5 = 16) and back+HP
(1 + 8 + 2 = 11), without empirical caps.

**Why a flat cap=10 was wrong:** the previous implementation hand-
tuned cap=10 to match close HP. That broke back+HP (gave A=13 vs
arcade 11) because back+HP's sentinel cell only stays for 8 frames.
The cell-data sentinel value (250) is a "wait" marker, not a duration
— duration is determined externally by whatever sets `cg_next_ix`.

Non-sentinel cells (`cg_ctr` in `[1, 30]`) still contribute their
freshly-loaded cgctr at entry, exactly once. Cells in `[31, 199]`
clamp to 30 as a defensive upper bound (no observed move uses this
range). Cells with `cg_ctr < 1` floor to 1.

### 3.3 Self-describing trace

`frame_trace_tick()` writes `/tmp/3sx-frame-trace.log` by default —
overridable via the `FRAME_TRACE_PATH` env var
([`frame_trace.c:14-23`](../src/sf33rd/Source/Game/ui/frame_trace.c),
added in Phase 1 / H4 so the harness's per-run temp dir can't
cross-contaminate with manual captures) — with one row
per game frame plus interleaved `# ...` annotation lines. Per-row
columns include `cgctr` (so cell durations and decrement timing are
visible) and `swnew` (rising-edge input bits, hex), letting analysis
identify which move was pressed without external notes. Annotation
lines are emitted by the overlay:

- `# F=N MOVE_START GT=... atk=... char=... cgix=... cgctr=... cghi=... pat=... kow=... sw_new=0x... atk_x=... def_x=... dist=...`
  on every `r1: 0 → !=0` transition.
- `# F=N FINAL atk=... outcome=HIT/BLOCK/PARRY/WHIFF S=... A=... R=... T=... adv=... engine_a=... use_hatt=... raw_len=... first_active_raw=... event_raw=... move_start_F=... atk_idle_F=... def_idle_F=... event_F=... parry_r2=...`
  on every `fd_finalize()`.

Hookpoints:
[`frame_trace.c`](../src/sf33rd/Source/Game/ui/frame_trace.c) —
`frame_trace_annotate()` plus `ensure_open()` shared between tick and
annotation; [`frame_data_overlay.c:559-591`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
emits MOVE_START, [`frame_data_overlay.c`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
emits FINAL at the end of `fd_finalize()`.

---

## 4. The synthesis state machine

The overlay maintains two pieces of state:

- `g_cur` — `FdMove`, the in-progress move being tracked (cleared on
  reset, populated as frames advance)
  ([`frame_data_overlay.c:105-155`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).
- `g_latched` — `FdLatched`, the last finalized move's numeric and
  meter results, shown until the next move starts
  ([`frame_data_overlay.c:157-175`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).

Each tick (`frame_data_overlay_tick()`,
[`frame_data_overlay.c:533`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)):

1. **Bail conditions.** Outside training mode → reset and clear
   latch. During super-freeze (`sa_stop_check() != 0`) → freeze
   accounting (don't tick `g_local_frame`, don't append).
2. **Snap.** Capture both players' state into `now[2]`.
3. **Move-start detection.** If no move is active, scan both players
   for an `r1: 0 → !=0` transition. The first one becomes the
   attacker; clear `fd_engine_active_count[atk]` and
   `fd_prev_active_cgix[atk]` so the engine accumulator starts fresh.
4. **Per-frame accounting** (sections 5–8 below).
5. **Finalize check.** When `attacker_idle >= 0` and (for hit/block)
   `defender_idle >= 0`, call `fd_finalize()` and reset.

### 4.1 Move start

`r1: 0 → !=0` transition on either player
([`frame_data_overlay.c:559-591`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).

The non-zero sub-states distinguish:

- `r1=4` → normal attack (the metered case)
- `r1=2` or `r1=3` → throw motions (`is_throw = true`, meter is
  suppressed but throw-state defender cells are colored)
- Other `r1` values can transition from idle in edge cases (parry
  recovery, etc.) — those still set `active = true` but do not gather
  a meaningful meter.

### 4.2 Move end

`r1: 4 → 0` on the attacker is the authoritative move-end signal
([`frame_data_overlay.c:670-675`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).

We previously tried `cg_cancel: !=0 → 0` ("non-cancellable tail
begins") as a proxy for arcade move-end, but that fires several
frames before the arcade convention for moves like Q's far LP and
gives short recovery counts. Pure `r1` is closer to arcade.

### 4.3 The `attacker_already_idle` gate

Critical for not over-counting recovery on moves where the defender
stays in stun past the attacker's animation end (Q's far LP, MP, HP,
HK against a standing defender). Once `attacker_idle` is set, the
loop continues so `defender_idle` can still be observed (needed for
advantage), but `raw[]` stops appending — otherwise R would balloon
by `(defender_idle - attacker_idle)` frames
([`frame_data_overlay.c:740-742`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).

`attacker_idle` is set earlier in the same tick by the `r1: 4 → 0`
detector, so the gate already excludes the (now-idle) move-end
transition frame from the meter — which is what we want.

---

## 5. Per-frame raw[] capture

For each frame where the attacker is in a non-throw move, not in
hitstop, and not yet idle, we append one slot to `g_cur.raw[]`
recording the per-frame state needed for finalize-time
classification. The capture happens at
[`frame_data_overlay.c:742-752`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).

Slot fields:

| Field | Source | Purpose |
| --- | --- | --- |
| `h_att_set` | `fd_engine_hitbox_active[atk]` OR (`h_att->att_box[j][1] != 0` for any j) | Active signal. Combines per-frame engine flag with post-tick box-dimension check |
| `cg_hit_ix` | `wu.cg_hit_ix` | Fallback active signal: `!= 1` ⇒ active (covers Q close LK/MK, command normals) |
| `def_r1` | defender `wu.routine_no[1]` | Defender state for color classification |
| `def_dm_stop` | defender `wu.dm_stop` | Used for event-edge detection at finalize |
| `def_hit_stop` | defender `wu.hit_stop` | Defender freeze frames |
| `def_dm_count_up` | defender `wu.dm_count_up` | Discriminates HIT vs BLOCK |
| `event_this_frame` | bool computed this tick | Marks the contact edge frame |

Hitstop frames are skipped from `raw[]` entirely
([`frame_data_overlay.c:725-743`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c))
— SF6-style behavior: the meter freezes for the side that's frozen.
The defender's `hit_stop` is independent; we only gate on the
attacker's. For some moves (Q's MP) the defender is frozen longer
than the attacker, and those post-attacker-freeze frames should
contribute to the attacker's active phase, which they do because we
gate on attacker hit_stop only.

---

## 6. Active signal selection (`use_hatt`)

At finalize, we pick *one* active signal for the whole move
([`frame_data_overlay.c:318-323`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)):

- If `h_att_set` ever fired during the move → use `h_att_set != 0`
  per frame.
- Otherwise → use `cg_hit_ix != 1`.

The fallback exists for moves whose engine bypasses the `h_att` flag:
Q's close LK/MK, certain specials, command normals. On those moves,
`h_att_set` never fires but `cg_hit_ix != 1` cleanly marks active
frames.

The whole-move pick (rather than per-frame) avoids mid-move signal
drift where a move oscillates between the two signals.

---

## 7. Event detection (hit / block / parry)

Edge: defender's `dm_stop` going `0 → < 0`
([`frame_data_overlay.c:635-654`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).
At that edge:

- Defender `r1 == 0` ⇒ **PARRY** (defender remained in idle through
  the parry frame). Capture `parry_r2` for advantage calc.
- Defender `r1 == 1` and `dm_count_up` incremented vs previous frame
  ⇒ **HIT**.
- Defender `r1 == 1` and `dm_count_up` unchanged ⇒ **BLOCK**.

The `dm_count_up` delta is what cleanly discriminates hit from block,
since both put the defender into `r1 = 1`.

### 7.1 Active-window proxy via event

Some moves (Q's command normals, certain specials) connect without
ever setting `cg_att_ix > 0` — the engine bypasses the cell's
attack-data flag. If we never saw a real active frame and the event
fires, we treat the contact frame itself as the active window
([`frame_data_overlay.c:650-653`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).
Without this, those moves would have `first_active < 0` at finalize
and the meter would have no active phase.

### 7.2 Defender idle return

For HIT/BLOCK, advantage requires knowing when the defender returns
to neutral. Captured on defender's `r1: !=0 → 0` transition
([`frame_data_overlay.c:657-661`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).
Parry uses a fixed lookup keyed on `r2`
([`frame_data_overlay.c:215-223`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c))
because parries leave the defender in `r1 = 0` throughout.

---

## 8. Live classification vs finalize override

The overlay paints the meter *during* the move (cells appearing in
real time) and re-paints at finalize once the full move history is
known. These can disagree, and that disagreement causes the only
remaining visual jump.

### 8.1 Live classification

Per-frame, as cells are appended to `raw[]`
([`frame_data_overlay.c:754-771`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)):

```
event_this_frame              → CONTACT
h_att_set != 0 OR cg_att_ix>0 → ACTIVE
first_active < 0              → STARTUP
otherwise                     → RECOVERY
```

This is a best-guess because we don't yet know which active signal
the move will end up using.

### 8.2 Finalize re-classification

At move end
([`frame_data_overlay.c:339-359`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)),
each cell is re-classified using the chosen `use_hatt` decision via
`fd_classify_attacker_finalize()`
([`frame_data_overlay.c:252-266`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).

### 8.3 Finalize engine-active-count override (meter visualization only)

After the per-cell pass, if the engine accumulator captured an active
count, we extend the active band on the meter so visualization agrees
with the canonical A
([`frame_data_overlay.c`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)):

1. Find `first_active_idx` — first cell classified ACTIVE or CONTACT.
2. Find `event_idx` — first cell with `event_this_frame == true`.
3. From `first_active_idx` forward, set:
   - `i == event_idx` ⇒ `CONTACT`
   - `i - first_active_idx < engine_a` ⇒ `ACTIVE`
   - else ⇒ `RECOVERY`

`event_idx` is tracked separately so the contact tick sits at the
actual contact frame, not at the start of the active phase. For
moves that connect mid-active (e.g. Q's far MK at distance), the
contact is several frames into the active window; without this
distinction the tick would always pin to the first active frame.

**The override is purely cosmetic.** The numeric S/A/R reads from a
separate per-frame tally captured *before* this override runs (see
§9), so the override no longer pollutes the recovery count when
engine_a disagrees with raw[]'s active-cell count. Previously this
override drove the numeric line directly and caused systematic
±1 errors on hit/block — see §13 for the diagnosis and fix.

---

## 9. Numeric counts

The numeric line decouples from the meter override. We compute three
counts from the per-frame classification *before* the §8.3 override
runs ([`frame_data_overlay.c`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)):

- `startup_pf` — raw[] cells classified STARTUP by the per-frame
  classifier (whatever frames came before the first ACTIVE/CONTACT).
- `active_pf` — raw[] cells classified ACTIVE or CONTACT by the
  per-frame classifier (frames where `h_att_set` fired, or the
  `cg_hit_ix != 1` fallback fired).
- `recovery_pf` — raw[] cells classified RECOVERY (frames after the
  active phase ended).

The displayed numbers are:

```
S = startup_pf
A = engine_a   (canonical accumulator; falls back to active_pf if 0)
R = recovery_pf
T = S + A + R
advantage = defender_idle - attacker_idle  (HIT / BLOCK)
advantage = (event_frame + parry_recovery_for(r2)) - attacker_idle  (PARRY)
```

`A` reads from the engine accumulator (the "canonical arcade A"),
while `R` reads from raw[]'s recovery tally directly. They no longer
share denominator: in cases where raw[]'s active cell count differs
from engine_a (because of hit-detection sub-frames or hitstop on a
1-frame active cell), the two sources reflect their respective ground
truths. This matches arcade across every Q normal and crouching
normal verified to date — see §12 and §13.

`T` can therefore differ from `raw_len` by ±N when the engine
sub-frames active cells on contact (close LK / close LP / close MK
/ Far MK on block) or stretches a 1-frame active cell with hitstop
(close HP). That delta is correct: it reflects the difference between
"frames the engine animated" and "arcade-canonical frames credited".

The `advantage` framing follows standard fighting-game convention:
positive means the attacker recovers first.

Parry recovery values for `r2`:

| `r2` | Value | Status |
| --- | --- | --- |
| 31 (high front) | 16f | Verified at 16f via 17f autoparry intervals |
| 32 (high back) | 16f | Verified |
| 33 (low) | 19f | Arcade value — engine produces 25f against standing defender; needs crouching-defender retest |
| 34 (air) | 26f | Arcade value — no observed events |
| other | 16f | Default |

[`frame_data_overlay.c:23-36`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).
Parry r2=33 / r2=34 verification is incomplete (TODO in source).

---

## 10. Defender row classification

Independent of attacker classification. Per cell at finalize
([`frame_data_overlay.c:268-282`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)):

```
def.r1 == 2 or 3              → THROW
event_this_frame, outcome=HIT → HIT (single-frame edge)
event_this_frame, outcome=BLK → BLOCK
event_this_frame, outcome=PRY → PARRY
def.r1 == 1, outcome=HIT      → HITSTUN
def.r1 == 1, outcome=BLK      → BLOCKSTUN
otherwise                     → IDLE (drawn as background)
```

Cell colors: see [`frame_data_overlay.c:54-68`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).

---

## 11. Visual rendering

`fd_draw_meter_row()`
([`frame_data_overlay.c:892-901`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c))
draws every cell-slot — even idle slots get the bg color so the bar
has a continuous visible width, important on CRTs where a
discontinuous bar looks broken.

Alternate-shading rule: every odd-indexed cell (`i & 1`) gets its
color piped through `fd_darken_color()`
([`frame_data_overlay.c:848-854`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)),
which scales each RGB channel to 75% (factor 192/256). This produces
visible per-frame separation without sacrificing pixels for borders.

`frame_data_overlay_draw()`
([`frame_data_overlay.c:903-974`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c))
chooses between `g_cur` (in-progress, real-time) and `g_latched`
(last finalized) for the source data. Real-time rendering during the
move is critical for the "fill in front of you" feel.

---

## 12. Verification status

Cross-checked against `docs/arcade-frame-data/q.json` using the
captured trace at `/tmp/3sx-frame-trace.log` with full
MOVE_START/FINAL annotations. Trace move identification is by
`sw_new` rising-edge bits + `kow` stance + `pat` per §13.8
"reference move identification."

### 12.0 Standard verification method (Phase 1 harness, since 2026-07-07)

**`tools/frame-data/run.sh` is now the standard way to verify any
frame-data change.** One command builds the host binary, boots
training mode headless, runs the 69-entry Q move corpus
(`tools/frame-data/corpus-q.yaml`), and diffs the trace's FINAL
annotations against `q.json`-derived expectations — deterministic
(byte-identical FINAL lines across repeat runs, pinned RNG),
~2.5 minutes wall clock, exit 0 when every non-`xfail` entry is green
(`xfail` entries report as known-open, not failures; an `xfail` entry
that unexpectedly passes is an XPASS and fails the run). See
`docs/plan-frame-data-harness.md` §1 for the architecture and its
sequencing table for current status.

The manual fresh-capture protocol documented at §13.4 below is
**demoted** to MiSTer-hardware-side checks only — the harness runs
the same host-build C code the manual protocol used to gate on, so
any correctness question that used to require a human replaying
moves is now the harness's job. Reserve the manual protocol for
things the headless host sim can't exercise: MiSTer hardware timing,
S-Video/RGB output, actual controller input.

### 12.1 Harness limitations register

Six harness/engine limitations have been documented, over the course of
authoring the Q/Hugo/Ryu corpora, as comments living next to the entries
that hit them. They are consolidated here so a fresh agent authoring a
new corpus (per-character rollout, Steps 10–13) can check this table
instead of rediscovering each by tripping over it. None of these are
fixed by this consolidation — it is a pointer table only; the cited
comment blocks remain the source of record and are unchanged.

| # | Mechanism | Evidence | Workaround | Status |
| --- | --- | --- | --- | --- |
| 1 | **P-teleport no-op during knockdown tails.** Teleporting P1 via the `P` directive immediately after a HIT command-grab silently no-ops if the defender still has residual knockdown-recovery ticks pending — the compiled `P` line executes but MOVE_START still reads the prior entry's position. | `corpus-hugo.yaml`, `h-meatsquasher-short-whiff` ordering comment (~lines 435–446) | Order a WHIFF predecessor immediately before any distance-sensitive entry needing a clean teleport — a WHIFF leaves no knockdown residue, so the teleport lands cleanly. Same fix *shape* as row 2 below, but here it's predecessor-**outcome** ordering, not a longer wait. | accepted |
| 2 | **P1 teleport corruption after connecting dash specials.** The entry immediately after a dash special that always connects (Dashing Head Fierce / Dashing Leg RH) gets a corrupted P1 teleport — `atk_x` lands wherever P1's dash carried it, not at the requested position. Re-converges only partially over several following entries, not fully in just the one immediately after (2026-07-06 correction to the original note). Plausible cause: the dash's forward momentum isn't reset by the following `P` teleport poke; not chased into the engine. | `corpus-q.yaml` ORDERING NOTE (~lines 350–359) + 2026-07-06 CORRECTION (~lines 452–464) | Order every distance-sensitive WHIFF entry in a section *before* the always-connecting HIT entries (Fierce Head, RH Leg/DLA RH). | accepted |
| 3 | **DHA long-charge variant recipe unknown.** q.json lists two Dashing Head Attack variants per strength (fast vs. long-charge/full-charge); only the fast variant is reachable from this harness — charge-back durations 45 up to 400 frames (45/60/90/120/150/200/250/300/400 tried) all produced the identical fast-variant numbers (S=13/15/19); a double-tap-forward variant was also tried and produced no special at all. | `corpus-q.yaml` Stage 3 header (~lines 340–348) | None — documenting the gap rather than guessing the input recipe is explicitly authorized by plan section 1.8/1.10. Only the fast variant is covered in the corpus. | wants-follow-up |
| 4 | **SDB HIT/BLOCK unreachable (dummy can't jump).** Shootdown Backbreaker (Hugo's anti-air command grab) whiffs against any grounded dummy configuration (verified dist 20/55/250, `dummy: stand` and `dummy: none`) — never connects on the ground. hugo.json's own `Block_advantage`/`Hit_advantage` = `"-"` for all three SDB rows independently corroborates the air-only role. The harness dummy cannot be scripted to jump (G-directive AI only, not frame-scriptable). | `corpus-hugo.yaml` SDB section comment (~lines 301–320) | None possible within the harness. WHIFF (with its exact S/A/R match) is the only reachable, honestly-assertable outcome; also independently named in the plan's Step 6 Do-Not list ("airborne-grab"). | accepted |
| 5 | **Fireball WHIFF unreachable; hadouken block-adv plateau boundary 121–149 unscanned.** A hadouken travels full-screen and still connects with a standing dummy even at dist=250–300 — no reachable WHIFF window (same dummy-can't-jump-or-duck constraint as row 4). Separately, `adv` was scanned at dist 56/70/80/90/96/100/110/120/150/171/200/250: holds exactly at ryu.json's `Block_advantage=-9` for 56–120, then drifts positive from 150 up (-3@150, +1@171, +8@200, +20@250 as later contact eats the window). The exact plateau boundary between 120 and 150 was never located. | `corpus-ryu.yaml` Hadouken section, WHIFF paragraph (~lines 369–375) + distance-calibration comment (~lines 343–357) | WHIFF: none (structural, no dummy-jump/duck). Plateau boundary: none needed currently — `dist: close` (56) sits well inside the exact-match plateau, so existing BLOCK entries are unaffected; locating 121–149 is optional future refinement, not a blocker. | WHIFF unreachable: accepted. Plateau boundary: wants-follow-up (optional) |
| 6 | **Merged-move projectile contamination residual.** `raw_len`/`proj_seen` are read live off `g_cur` at finalize, not from a snapshot. If a corpus ever hits the merged-move case (attacker idle `r1=4→0` but finalize deferred waiting on `defender_idle`, and a fresh attacker move retriggers before that deferred finalize runs), a spawn belonging to the NEW move could latch onto the OLD move's still-live `proj_seen`/`proj_spawn_slot` before `fd_finalize()` reads them. Not constructible in current corpora (no retriggerable projectile move deferred on `defender_idle` exists in `corpus-q.yaml`). Step 7 (2026-07-07) established the F=4940 *precondition* is reachable with an ordinary normal (Q Jab, N=17), but genuine contamination itself remains unreached for every button-normal second move tried (Jab N=6–20, Crouching Jab N=13–22, second button LP/MP only) — a button normal's minimum startup-to-active delay exceeds the deferred window any positive-advantage Q normal opens. One pair was never scanned: a +4-advantage-first move (Crouching Jab) + THROW-second (a throw's `Startup=2` and its hitbox gate the accumulator via `caix`, structurally faster than a button normal's `atix` path) — Step 7's scans only pressed a second LP/MP, never a throw. | `src/sf33rd/Source/Game/ui/frame_data_overlay.c` "Residual (documented, not fixed)" comment (~lines 1196–1209) AND `docs/frame-data-synthesis.md` §13.9.3 Step 7 update subsection (~lines 3497–3635) | If it ever shows up, the fix is a §13.7.7-style anchor snapshot: capture `proj_seen`/`proj_spawn_slot` at the same "first tick `attacker_idle` is set" point `engine_a_at_atk_idle` already uses, instead of reading the live fields at finalize. | wants-follow-up (one unscanned pair: +4-advantage-first + throw-second) |
| 7 | **Chariot rush (RH) no reachable WHIFF window.** No WHIFF baseline exists for Urien's Chariot rush (RH/HK) across 8 distances spanning 56–300 — the dash connects at every tested distance; `DIST_MAX` is a hard ceiling on this harness. Same SPECIALS-REACHABILITY shape as rows 4/5 above (a dash/projectile that outruns the harness's teleport range). | s13-findings (2026-07-10 classification sweep #2); `corpus-urien.yaml` `urien-chariot-hk-block/-hit` xfail re-cites | None possible within the harness (DIST_MAX=300 hard ceiling). BLOCK/HIT stay the only reachable outcomes; R-side stays an open, unclassified observation on both. | accepted (SPECIALS-REACHABILITY) |
| 8 | **Post-knockdown contact-residue window (Urien Headbutt, Fierce/HP).** Position reads exactly correct — `dist=70` byte-exact at MOVE_START — yet contact fails (outcome reads WHIFF, not HIT) in full-corpus order, even though the identical placement verified HIT in isolated probes. The failure threshold is bracketed to 53..~206 frames of residual state past the PREDECESSOR entry's `def_idle`. Two empirically verified candidate fixes exist: a WHIFF spacer entry inserted before this one, OR raising `inter_entry_wait` to 400. **RESOLVED 2026-07-11 (HEADBUTT-DEFER fix, own re-measure gate):** the WHIFF-spacer candidate was chosen and applied — `corpus-urien.yaml` gained a new `urien-headbutt-hp-hit-spacer-whiff` entry (the same guaranteed-WHIFF "press LP" at dist=250 recipe already used by `urien-lp-whiff`) immediately before `urien-headbutt-hp-hit`. Chosen over the `inter_entry_wait=400` alternative because it touches only this one couplet (the 5 entries ordered after it shift absolute game-time by one spacer — a rows-1-2 ordering-sensitivity context change — but their measured values are byte-identical: the re-measure gate proved zero spillover on every other row, which is exactly why the gate demanded exact-drift) and matches this table's own row-1 idiom (a WHIFF predecessor ordered immediately before the affected entry). Re-measured in full-corpus order: outcome=HIT, S=12/A=6/R=19/kd=1 — byte-identical to the isolated baseline (s13-findings RUN 2) and to the WHIFF-spacer probe (s13-findings RUN 4), and arcade-exact against `docs/arcade-frame-data/urien.json`'s Headbutt (Fierce) row (Startup=12/Hit=6/Recovery=19). `urien-headbutt-hp-hit` is now plain PASS, xfail removed. | s13-findings (2026-07-10 classification sweep #2); `corpus-urien.yaml` `urien-headbutt-hp-hit`/`urien-headbutt-hp-hit-spacer-whiff` entries + comments (2026-07-11 fix); `docs/plan-frame-data-completion.md` HEADBUTT-DEFER row (closed 2026-07-11) | Applied: a WHIFF spacer entry ordered immediately before the affected entry (`inter_entry_wait=400` was the verified-but-unchosen alternative — see reasoning in the Mechanism column). | resolved 2026-07-11 |
| 9 | **Bundle-context 1-tick A↔R sensitivity.** A deterministic, sum-conserving 1-tick A↔R swap appears between an ISOLATED single-entry probe bundle and the canonical FULL-CORPUS run, on at least two members: `urien-vkd-lk-block` (isolated A=5/R=31/adv=−16 vs full-corpus/golden A=4/R=32/adv=−17, reproduced twice, s12-findings) and `sean-ryuubi-hk-block` (isolated R=15/adv=−3, arcade-exact, vs golden R=14/adv=−2 — classification sweep #2, from `s3-rundir/run-sean/trace.log` FINAL vs `golden/sean.tsv:65`). Position is NOT implicated in either case. **This does NOT undermine any golden/shipped value** — goldens derive exclusively from canonical full-corpus runs (`run-suite.sh`), and s12 independently re-verified the full `corpus-urien.yaml` reproduces golden exactly (64/64). **UPDATE (2026-07-11, sweep #3):** the `urien-vkd-lk-*` R clause is CLOSED this sweep (F2 R-corollary, §12.2.4/§12.2.2 R-SURPLUS sub-list — 25-tick parallel recovery branch, contact R − arcade 7 = +25 exactly); `urien-vkd-lk-hit` is now plain PASS. `urien-vkd-lk-block`'s adv residual (measured −17 vs arcade Block_advantage −16) is now this row's sole open clause for that entry — the bundle-context 1-tick A↔R swap this row documents is unaffected (still a harness-isolated-probe-mode observation, not a defect). One-line observation (lane 1, not separately chased this sweep): the WHIFF leg's own FINAL line reads `active_pf`=13 against 12 raw jatix rows in the underlying trace — a whiff-only active_pf/R boundary quirk, noted for a possible future lane, not investigated further here. | s12-findings; classification sweep #2 fable-grade audit (this session); classification sweep #3 fable-grade audit (`sweep3-final-rulings.md` A.2); `corpus-urien.yaml` `urien-vkd-lk-block/-hit`, `corpus-sean.yaml` `sean-ryuubi-hk-block` xfail re-cites | None needed — golden/shipped values are unaffected; this is a bundle-context observation about the harness's isolated-probe mode, not a defect in the shipped corpus. | accepted (context-sensitivity, not a defect); `urien-vkd-lk-hit` R clause closed sweep #3 |

**Dated addendum to row 1 (2026-07-10, classification sweep #2 fable-grade
audit):** a second trigger instance of the P-teleport no-op was found —
this time on an ORDINARY special's knockdown HIT, not just command grabs
(evidence: s3-findings disclosure on a `dudley-jetup-*-hit` leg; the
label-attribution drift there hit only the two WHIFF comparanda, not a
shipped value). Cross-pointer to row 8 above (kinship not decomposed —
the s3 instance did not record whether contact-vs-position failed; row 1
remains its own anchor, unchanged).

For context on the harness's own distance/timing tunables referenced
above (`DIST_MAX=300` teleport ceiling, motion/press hold-frame
constants), see `tools/frame-data/compile_corpus.py`'s tunables section
near the top of the file.

### 12.2 Divergence-class registry (2026-07-09 classification sweep)

The Phase-6 cast rollout (19 corpora) accumulated dozens of per-character
`UNCLASSIFIED` xfail findings; several grew multiple cross-character
members. A dedicated classification sweep (2026-07-09) root-caused the
three recurring families below with trace evidence and registered the
coherent one-offs so future sightings have exactly ONE place to look.
`tools/frame-data/CORPUS-AUTHORING.md` Phase 6 points here.

**Method (same-binary protocol, everything reverted before this text was
committed):** per-tick `FRAME_TRACE_PATH` traces of scratch single-move
corpora (necro/alex UOH; ken Towards-RH + Forward; akuma Towards-Strong +
close RH), plus a temporary accumulator *credit ledger* — an
`fprintf(stderr)` inside `char_move()`'s `fd_engine_active_count` update
block (`src/sf33rd/Source/Game/engine/charset.c`, after the `add > 0`
sum), env-gated, logging `GT/cgix/cgctr/jatix/add/engine_a` per
crediting call. The ledger turns "which cells credited how much" from
snapshot-inference into direct observation. Instrumentation was reverted
(`git checkout`) and the clean tree rebuilt before any shipped edit.

#### 12.2.1 UOH landing-clocked active tail (the recurring "Alex/Elena/Necro/Twelve bucket") — mechanism established

UOH's active phase does not end by declared counters — it ends at the
LANDING (wall-clock). Two data shapes, both trace-proven 2026-07-09:

- **Sentinel shape (Necro; Elena/Twelve byte-identical by corpus-cited
  signature):** declared 2f active cell (necro cgix=24, jatix=48) then an
  open-ended sentinel cell (cgix=28, cgctr=250) crediting **1 per real
  char_move tick** until landing. Ledger, whiff: `add=2` then 8×`add=1`
  (GT=56..63) → A=10 = arcade. Block: contact freezes the attacker chart
  10 ticks (8 hitstop + 2 extra guard-hold at cgctr=1, GT=316–317) but
  delays the landing by 12 ticks vs whiff — the 2-tick mismatch is 2
  extra real sentinel ticks (ledger: 10×`add=1`, GT=319..328) → A=12.
  Hit: chart freeze (8) exactly equals the landing delay (+8) → A=10 =
  whiff. So BLOCK diverges alone, by exactly the guard-freeze/landing
  mismatch.
- **Looping declared shape (Alex):** no sentinel; declared active cells
  16(2)/20(2)/24(2)/28(3)/32(3) and the chart **loops back** while
  airborne. Whiff: landing truncates cell 32 after 1 real tick but its
  declared 3 is already banked (§13.11 declared-truth) → A=12 vs arcade
  10. Block: the chain completes and re-enters cell 28 (ledger: GT=328
  `cgix=28 add=3`) before landing → A=15. Hit: same as whiff → A=12.
  This is the first *trace-proven* instance of re-entry re-crediting in
  this engine (relevant to §12.2.3's candidates).
- **R triplet 3/2/2 (all four members):** every leg is the §13.5.1
  grounded-cleanup cut (`cut=1`, `anchor_a=engine_a`). The whiff leg is
  residual item 18(b) (R=3 vs 5, q-uoh-whiff-r precedent). On the
  contact legs the post-landing cleanup cell plays one fewer tick (whiff
  ctr sequence 3,3,2,1 = 4 ticks; contact 3,2,1 = 3 ticks — landing-tick
  vs char_move alignment differs) → R=2. Observational; the underlying
  truncation is the same 18(b) cut.

Members: `alex-uoh-*` (all three), `elena-uoh-block/-hit`,
`necro-uoh-block/-hit`, `twelve-uoh-block/-hit` (elena/necro/twelve
whiff legs stay item 18(b), now with this mechanism behind the R
triplet). NOT members — verified distinct signatures: dudley-uoh
(outcome-independent A=14, §13.11 whiff-persistent), urien-uoh
(A **under**count, same-cell self-loop accumulator blind spot),
yun/remy-uoh + yang-uoh R clauses (item 18(c) no-cut), yang-uoh A clause
(Makoto partial-restore compound), ibuki-uoh (A=11 §13.11; its own
three-way R), q-uoh (block R exact). Disposition: xfail, no expect
changes — whether displayed A should canonicalize to arcade's 10 on
block is a §13.10/§13.11-style convention decision deliberately NOT
taken by this sweep (needs a user decision).

**UPDATE (2026-07-10, CONV-2):** the user decision has been taken.
Contact-leg A clauses assert measured engine truth (family F4, §13.13
SCOPE EXTENSION block); R-triplet and whiff legs are unchanged. See
§13.13's CONV-2 exclusion-item-3 note for the full member list.

**DATED NOTE (2026-07-10, ENGINE-5 closure — verification, not a
correction):** this paragraph's urien-uoh exclusion above states no
numeric arcade value, so it carried no wrong figure to fix; searched
for completeness. urien-uoh's arcade A is confirmed **10**
(`docs/arcade-frame-data/urien.json:808-812`) — the "same-cell
self-loop accumulator blind spot" named here is now fully closed as
ARCADE-INTERMEDIATE / NO-PRINCIPLED-TARGET; see the new §12.2.4 "Same-
cgix non-sentinel self-loop" row for the four-quantity table and full
citations, and the completion-plan ENGINE-5 row for the tracker close.

**MEMBERSHIP NOTE (2026-07-10, classification sweep #2 fable-grade
audit):** `chunli-uoh-block/-hit`'s R-triplet is a **whiff-exact variant**
of this family — whiff R=5 is arcade-exact (unlike the four members
above, whose whiff legs stay item 18(b)); the contact-side cleanup cell
plays exactly one fewer tick than the whiff chain (first audit §B.1,
unchanged by this sweep's own window). Re-cited here from the corpus
xfail strings; disposition unchanged (xfail, no expect changes, §13.13
exclusion 3).

**UPDATE (2026-07-10, CONV-2):** R clauses now assert measured R=4
(family F6, §13.13 SCOPE EXTENSION block) on both `chunli-uoh-block` and
`chunli-uoh-hit`; A was already asserted at 14 above. Both entries are
now plain `PASS`.

#### 12.2.2 Contact-branch recovery shortening (contact-only R-deficit) = §13.10 Class 1 generalized to contact — mechanism established

On contact, the chart takes a **parallel recovery branch** that is
genuinely shorter than the whiff chain; the arcade table's R column is
whiff-canonical while its (live-measured) advantage column embeds the
shorter contact recovery — which is why every member reads S/A/adv
exact with only R low. Trace evidence (2026-07-09):

- `ken-twdshk-block`: whiff recovery chain cgix 84(3)/88(4)/92(5)/96(5)
  + 1 terminal-cell tick → R=18 exact; on block the chart branches after
  the last active cell (76) into cgix 104(2)/108(3)/112(4)/116(4) + the
  same terminal tick → R=14. Deficit −4 = the branch-length differential
  exactly (17 vs 13 declared ticks). Measured adv=−1 = arcade exact.
- `akuma-twdsmp-block`: contact jump 16→28 lands past recovery cell 24
  (declared ctr=2); deficit −2 = the skipped cell's declared duration
  exactly; adv exact.
- `akuma-roundhouse-block/-hit` (close): identical recovery jump 24→32
  on BOTH outcomes, R=19 both, S/A/adv exact — member by identical
  mechanism signature (whiff-unreachable for the close animation, so the
  −6 rests on the arcade figure rather than a whiff row).
- `chunli-forward-block/-hit` (−1, same-cghi recovery skip 84→96):
  signature member; `chunli-crfierce-*` was already the whiff-confirmed
  §13.10-Class-1-generalized precedent in its own file.

Candidates (sign-coherent but NO skip/branch signature recorded — stay
UNCLASSIFIED): ~~`elena-crroundhouse-block/-hit` (−1 both, whiff clean,
ordinary r1 end), `ibuki-raida-lp-block` (−11, block-only)~~ — **MOVED TO
MEMBERS 2026-07-10, see SWEEP-2 UPDATE below (dated, not deleted: this
strikethrough records the pre-sweep candidate status for the audit
trail).**
Disposition: member xfails re-cite §13.10 Class 1 generalized-to-contact
via this entry; arcade (whiff-canonical) R stays in `expect`.

**UPDATE (2026-07-10, §13.13):** members now assert the measured
contact-path value, plain `PASS`, citing §13.13 (or, for the
already-adopted §13.10-proper HIT-R subset authored during the
Phase-6 rollout, citing §13.10 directly as compliance); candidates
(registry-absent, no recorded skip/branch signature of their own, or
explicitly "stays UNCLASSIFIED") are unchanged — they never convert.

**SWEEP-2 UPDATE (2026-07-10, classification sweep #2 fable-grade
audit).** Method note: the window established a corrected capture
counting rule — a hitstop freeze excludes exactly N−1 ticks (the event
tick forced through), and critically, **a frozen cell RESUMES
contributing real counted ticks once the freeze thaws.** The first
audit's by-hand tallies missed these post-thaw resumption ticks, which
is why several members below were previously left as candidates or
"sign-incoherent" residuals; every one of them closes to ZERO residual
under the corrected rule (per-entry citations in each corpus xfail
comment / removed-xfail comment).

Trace-proven members added (R-deficit direction, F2 proper), each with
its own recorded skip/branch cgix signature (own corpus comment carries
the full per-cell citation): `dudley-crroundhouse-block/-hit` (−6,
branch cells 156–174), `elena-crroundhouse-block/-hit` (−1, whiff-only
cell 36 dropped — promoted from the candidate line above),
`ibuki-raida-lp-block` (−11, skip cells 24/30 — promoted from the
candidate line above), `sean-farroundhouse-block/-hit` (−1),
`sean-twdshp-block` (−3), `yun-farfierce-block` (−1),
`yun-roundhouse-block/-hit` (−1), `twelve-crfierce-hit` R clause (−5,
the Step-1 cross-ref clause).

**SWEEP-3 UPDATE (2026-07-11, classification sweep #3 fable-grade
audit).** `makoto-hayate-lp/mp/hp-hit` (−4, R-deficit direction) joins
this list: own per-tick recount (s5-rundir/run-makoto preserved P2tick
stream) closes the HIT-side R window to post-thaw GT310-329 =
24(4)+28(2)+0(2)+dwell12(11)+0(1)=20 vs whiff's 24; −4 = whiff's own
cgix4+cgix8 cells (2+2 ticks) skipped outright, dwell unchanged (11=11)
— the F2 bar met as written. The two-term "+2 uniform hitstop-conversion
term" framing floated by an earlier lane is MOOT: the audit's own recount
closes both signs of this move as a single branch-content differential
(see the R-SURPLUS sub-list below for the BLOCK-side +8 companion
member). All six `makoto-hayate-*` entries now assert `R` as a plain
literal (§13.13), xfail removed entirely — see corpus-makoto.yaml's
per-entry comments for the full per-cell citation.

**R-SURPLUS member sub-list** (the R-side corollary below, now CLAIMED —
see the §12.2.3 corollary-line update): `alex-fierce-block/-hit` (+1),
`chunli-farroundhouse-block` (+1), `twelve-crstrong-block` (+5),
`ibuki-dtwdsforward-block` (+9), `urien-chariot-mk-block` (+1),
`dudley-crforward-block/-hit` (+3), `ibuki-tsumuji-lk-block` (+2). Each
closes exactly under the corrected counting rule; see the corpus xfail
→ plain-PASS comment for the per-cell citation.

**SWEEP-3 UPDATE (2026-07-11, classification sweep #3 fable-grade
audit).** Two further members join this sub-list. `makoto-hayate-lp/mp/
hp-block` (+8): own per-tick recount closes the BLOCK-side R window to
post-thaw GT183-214 = 24(4)+28(2)+40(2)+44(2)+48(2)+dwell60(19)+0(1)=32
vs whiff's 24; +8 = dwell differential exactly (19 vs 11 ticks), the
40/44/48 detour (6 ticks) exactly replaces whiff's own 0/4/8 (6 ticks) —
paired with the R-deficit member above, both signs of this move now
close to zero residual as a single branch-content differential (see
§12.2.4's Hayate compound row for the closure statement). `urien-vkd-lk-
hit` (+25, F2 R-corollary): a 25-tick parallel recovery branch verified
raw (cgix 68(3)/72(3)/76(2)/80(3)/84(3)/88(11), all jatix=0),
byte-identical to the BLOCK leg, absent from WHIFF, entered only via the
cgix60 hitstop exit (s12-rundir/run-urien-full F5033-5057/F5105-5129); R
= branch 25 + cleanup 7 = 32; contact R − arcade Recovery 7 = +25 =
branch exactly. `urien-vkd-lk-block` closes the identical R clause under
the same branch proof (PARTIAL — the entry's stacked adv residual, −17
vs arcade −16, stays open per §12.1 row 9; see that row and
`corpus-urien.yaml`'s per-entry comments).

#### 12.2.3 Contact-branch declared-credit A-overcount (the "mirror of issue #17") — mechanism established for ken-forward

Credit ledger, `ken-forward` (st.MK): whiff active chain cgix 20(add=4)
→ 44(add=3) = A=7 = arcade. On BLOCK the contact tick banks cell 20's
declared 4, then the same-tick contact advance enters a parallel branch
containing **extra declared-active cells**: 32(add=2) → 36(add=1) →
44(add=3) = A=10. HIT branches differently — 20(4) → 36(1) → 44(3) =
A=8. Both previously "unprecedented" outcome-dependent wrong values
reproduce exactly and are now explained: guard and hit branch to
different cells.

This restores sign-coherence with issue #17: both directions are ONE
engine behavior — a same-tick contact advance re-routes the chart onto
an outcome-specific branch, and A follows that branch's declared-active
content. Shape (a1) branches PAST declared-active cells (undercount);
this family's branches INSERT them, while the interrupted cell keeps its
banked declared credit (§13.11) — overcount.

Members: `ken-forward-block/-hit` (trace-proven). Candidates
~~(signature-matched contact-only A-overcounts with S/adv exact — NOT
individually re-traced, stay UNCLASSIFIED citing this entry as the
candidate mechanism): `sean-ryuubi-*` A clauses (+2),
`dudley-jetup-*-block/-hit` base overcount (+4), `twelve-crfierce-block`
(+11), `twelve-crroundhouse-hit` (+4, HIT-only), `remy-rrf-lk-block/-hit`
stacked A+1.~~ — **MOVED TO MEMBERS 2026-07-10, see the SWEEP-2 F3
member-set amendment below (dated, not deleted).** R-side corollary
~~(candidate shape only, NOT claimed)~~ — **NOW CLAIMED AND ESTABLISHED,
2026-07-10 (see the SWEEP-2 update below and §12.2.2's R-SURPLUS
sub-list)**: a contact branch can equally run LONGER through recovery —
first proofs (own-ledger + P2-direct closures) for the contact
R-surpluses (`twelve-crstrong-block` +5, `dudley-crforward` clause 2 +3,
`alex-fierce/backfierce` +1, `ibuki-dtwdsforward-block` 2×,
`chunli-farroundhouse-block` +1, `urien-chariot-mk-block` +1,
`ibuki-tsumuji-lk-block` +2 — see §12.2.2's R-SURPLUS sub-list for the
per-member citations). **Exception:** `yun-crfierce-block` clause 2 (+1)
was investigated this sweep and is NOT an F2/F3-corollary member — the
own P2 recount shows the +1 is whiff's own inter-active gap tick
surviving into the R window, not branch content; it is routed instead
to the new §12.2.4 "contact-skip R-window reclassification" class (see
below) and stays UNCLASSIFIED.

**UPDATE (2026-07-10, §13.13):** the established member
(`ken-forward-block/-hit`) now asserts the measured contact-path A,
plain `PASS`, citing §13.13. Candidates are unchanged — "NOT
individually re-traced" still means they never convert; the F3 member
set §13.13 recognizes is exactly `{ken-forward-block, ken-forward-hit}`.

**SWEEP-2 F3 member-set amendment (2026-07-10, classification sweep #2
fable-grade audit).** The own-ledger F3 bar (§13.13:5179–5184) is now MET
for 16 further members, own-ledger reconciled from the preserved
`[P1ledger]` streams (`s3-rundir/run-{sean,remy,twelve,dudley}/run.log`):
`sean-ryuubi-lk/mk-block/-hit` + `sean-ryuubi-hk-hit` (A→10, inserted
cell 68(add=2), +2 exact; `sean-ryuubi-hk-block` PARTIAL — its A-clause
converts too, but the stacked R/adv residual stays open, see §12.1 row
9), `dudley-jetup-lp/mp/hp-block` + `dudley-jetup-lp-hit` +
`dudley-jetup-hp-hit` (A→12/20/25/16/17, inserted cell 36(add=4) ±
re-route, F1-direction re-route on the hp-hit leg), `twelve-crfierce-
block/-hit` (A→18, 12-cell insert; -hit ALSO carries a stacked F2
hit-branch R clause →13), `twelve-crroundhouse-hit` (A→10, four add=2
branch cells replace four add=1 cells), `remy-rrf-lk-block/-hit` (A→4,
branch cells 56/60/64 replace displaced cell 20(2)). The F3 member set
§13.13 recognizes is THEREFORE now `{ken-forward-block, ken-forward-hit,
sean-ryuubi-lk-block, sean-ryuubi-lk-hit, sean-ryuubi-mk-block,
sean-ryuubi-mk-hit, sean-ryuubi-hk-block (PARTIAL — A-clause conversion
only), sean-ryuubi-hk-hit, dudley-jetup-lp-block, dudley-jetup-mp-block,
dudley-jetup-hp-block, dudley-jetup-lp-hit, dudley-jetup-hp-hit,
twelve-crfierce-block, twelve-crfierce-hit, twelve-crroundhouse-hit,
remy-rrf-lk-block, remy-rrf-lk-hit}` (18 members total: 2 original + 16
new — this line's history (the prior "exactly {ken-forward-block,
ken-forward-hit}" text above) is preserved per repo practice, not
silently rewritten).

#### 12.2.4 Registered one-off classes (no mechanism claimed)

| Class | Members | Signature | Status |
| --- | --- | --- | --- |
| **S-divergence** (first ever) | `yang-forward-block/-hit` | measured S from `first_active_raw`=5 vs oracle 7 = `event_raw` (first contact-capable tick); A/R/adv exact | UNCLASSIFIED; two labeled hypotheses in the corpus (real "meaty" non-connecting active window vs S-measurement artifact). **UPDATE 2026-07-10 (sweep #2):** CONFIRMED as **ENGINE-6 candidate** — `h_att_set` fires on cell load (`charset.c:2989-2997`, unconditional on atix≠0) two ticks before `att_hit_ok` arms (`charset.c:2938` via `set_new_attnum()`); the engine's real collision loop gates on BOTH (`hitcheck.c:1618-1621`) and never box-tests at F=5-6; ENGINE-4/lever-J already fixed the identical pair on the projectile path. See completion-plan ENGINE-6 row. **UPDATE 2026-07-10 (ENGINE-6 CENSUS-FALSIFIED, 1,039-window pre-diff census across all 19 corpora — `e6-census.tsv` + `e6-census-report.md`, `docs/plan-frame-data-completion.md` ENGINE-6 row):** the candidate above is REJECTED as a general mechanism — no lever L. Every window in the suite where the sticky arm tick (`gen_athok_slot`, first `att_hit_ok`-armed AND `h_att_set` tick) differs from the cell-load tick (`first_active_raw`) was measured: **8 divergent windows across 3 distinct move families**, all `use_hatt=1`/`proj=0`/`endrel=0` (the general-classifier path lever L targets). `yang-forward-block/-hit` (S 5→7) key arcade Startup on the ARM tick — CONFIRMS, arcade-exact under the candidate. But two additional, currently-PASS, arcade-exact families key arcade Startup on the CELL-LOAD tick instead and would REGRESS off arcade-exact under the same fix: `remy-crfierce-block/-whiff/-hit` (S 8→9 vs arcade 8) and `twelve-backforward-block/-whiff/-crouch-probe` (S 5→7 vs arcade 5). All three families are mechanically byte-identical — cell-load fires 1-2 ticks before the arm tick in every case, and the arm coincides with contact on BLOCK/HIT legs but ALSO on WHIFF legs with no contact at all (proving the arm's timing is a property of the attacker's own chart, not a causal contact linkage); every available safety/diagnostic column (hitstop, super-freeze, dirty-start, trim-cap, case-B event-ordering) reads identically clean across all 8 windows. No discriminator exists in any observable engine state — arcade's own published table simply keys Startup per-move (cell-load for remy/twelve, arm for yang) for a bit-for-bit identical engine mechanism; the only available exclusion is enumerating labels, rung 3 on the census's own ladder (rung 1 tighten and rung 2 mechanistic-scope-down both fail — see the report). **Future bar** (same standard as ENGINE-7's closure): a mechanism claim must (i) name the discriminator, (ii) hold across every divergent window in a full re-census, not just the confirming pair, and (iii) show ≥2 independent positives on the SAME code path as the divergent windows — ENGINE-6 itself had a plausible ≥2 argument (ENGINE-4/lever-J's projectile-path fix, cited above) and still died on the general path, so a cross-path positive no longer counts toward the bar; only a same-path positive does. Status: no lever ships; `yang-forward-block/-hit` stay xfail with this census as their citation; `remy-crfierce-*` and `twelve-backforward-*` stay PASS, unaffected. |
| **Defender-stance-conditional attacker R** | `necro-flyingviper` crouch-BLOCK | crouch R=15/adv=+1 vs stand R=19/adv=−3, identical dist | UNCLASSIFIED. **UPDATE 2026-07-10 (sweep #2):** enriched with the pinned-meter identity — `R = raw_len − event_raw − 1` reproduces BOTH 19 (stand/hit, event_raw=26) and 15 (crouch, event_raw=30) from one mechanism; raw_len/T pinned at 46 and atk_idle at start+55 on all three contact legs (outcome- AND stance-independent). HONESTY CAVEAT: active_pf splits (1 stand/hit vs 5 crouch) — two different routes to the same pinned length; `cg_extdat=0x80` not statically confirmed. Stays UNCLASSIFIED. **UPDATE (2026-07-11, sweep #3):** F5 (item-4 Hugo-Roundhouse landing-clocked family) membership tested and REFUSED on `necro-flyingviper-lp-block/-hit` — F5's bar requires a trace-proven same-tick contact advance whose freed declared-active ticks tally as RECOVERY, and neither prong survives the raw trace: no cgix advance exists (cells 32/36 play out inside a freeze already running on cell32's first row, athok never sets — a freeze-consumption variant, not the F5 same-tick advance), and the freed-tick arithmetic does not close (5 declared − 1 active_pf = 4 freed vs an R surplus of only +2; whiff raw_len 48 vs contact 46 unreconciled). Variant-shaped, not-per-tick-closed — CONV-2 (ii) bars conversion. Genuinely NEW enrichment: the terminal tail (cell44 full 3 → cell48 full 4 → cell52 sentinel → idle → FINAL) is now verified cell-for-cell identical across ALL THREE contact legs (first per-tick confirmation of the landing-pinned end); crouch's event_raw 26→30 (+4) shift confirmed in-trace (clean athok=1 window, freeze at cell36). NEW open residual, recorded honestly: whiff's own chart cuts mid-cell44 (1 of 3 declared) and ends at raw 48 vs the contact legs' pinned 46 — the "shortening" direction is whiff-side and unreconciled. `necro-flyingviper-lp-crouch-probe` rides this same enrichment (its adv clause has no family of its own). All three stay xfail. |
| **Sum-preserving A/R boundary shift** | `remy-cbk-lk-whiff` (A 11/R 9 vs 10/10) + `yun-zesshou-hp-block/-hit` (A 16/R 11 vs 15/12) | ±1 A/R swap, S+A+R conserved, adv exact, cut=0 | two characters, same signature — LINKED as a family-of-two, UNCLASSIFIED. **DATED CORRECTION 2026-07-10 (sweep #2, s7):** the "same signature" framing is wrong — `remy-cbk-lk-whiff` IS sum-preserving/self-consistent (its single boundary tick now PROVEN: F=1207, the last-hatt tick at the anim-chain reset, no hitstop, active_pf==engine_a==11); but remy's CONTACT legs (`remy-cbk-lk-block/-hit`) and BOTH yun legs are NOT (T≠S+A+R on all four, and both moves are hitstop-entangled / active_pf≠engine_a in opposite directions). The whiff-leg row stands; the four contact legs are (e) hand-offs — no ledger was run for either move; a future ledger session is the named next probe. **UPDATE (2026-07-11, sweep #3):** the s3w ledger session RAN, closing the sweep-2 hand-off. `remy-cbk-lk-block/-hit`: same-tick interior-transition credit banking — within one Game_timer tick the chart passes 52→44→4, re-entering cell 44 and banking its declared 3 a second time while the per-frame snapshot observes only the tick's final cgix (ledger GT3393/3415 block, GT3700/3713/3716/3719/3722 hit); A=14 = whiff's own 11 + 3 phantom, exact both legs. Moved to the NEW "Same-tick interior-transition credit banking" row below (CONFIRMED-PENDING-GRANT) — see that row; this row's live UNCLASSIFIED member is now the whiff-leg ±1 shift only. `yun-zesshou-hp-block/-hit`: engine-credit-ledger decomposition (60:4+66:5+72:5+78:2 = A=16 exact; cell78 banks declared 2, plays 1; cell60 plays 5 real vs declared 4 across a freeze-extension) — arcade-mapping is two-way ambiguous (cell78 bank vs cell60 freeze-extension tick), no conversion offered; enriched-(b), (c)-cited to the NEW ENGINE-10 candidate row (`docs/plan-frame-data-completion.md`) for the diagnostic `h_att_set`/`jatix` one-tick lag. Both stay xfail. |
| **Cut-committed whiff R-overshoot** | `yang-senkyuutai-lk-whiff/-block/-hit` | R=38 vs 34 on WHIFF itself, `cut=1`, anchor arms | UNCLASSIFIED — explicitly NOT item 18(c) (that bucket requires cut=0/no anchor). **UPDATE 2026-07-10 (sweep #2):** CONFIRMED as **ENGINE-7 candidate** — the §13.5.1 cut anchor fires at the cghi-edge, 4 ticks AFTER the attacker's guard_flag re-arm (rearm at reset+6, anchor at reset+10, gap = overshoot = 4 exactly; a gflg-based R reconstructs arcade 34 exactly; s8-findings, both legs). See the §13.5.1b dated correction (:1426-1432 census figure) and the completion-plan ENGINE-7 row. **UPDATE 2026-07-10 (ENGINE-7 CENSUS-FALSIFIED, 1,039-window pre-diff census across all 19 corpora — `e7-census.tsv` + `e7-census-report.md`, re-plan `engine7-replan.md`, `docs/plan-frame-data-completion.md` ENGINE-7 row):** the candidate above is REJECTED — no lever K. Full census scope: 1,039 windows / 41 cut=1 / 54 proj backstop / q-cndb+q-throw watch items, all clean except the seven-move table below. Every window with a grounded, persistent, post-reset guard-rearm edge preceding its current R end was measured: **seven moves total.** Arcade end = rearm+0 for four (`yang-senkyuutai-lk`, `ibuki-kazekiri-lk`, `yun-uoh-whiff`, `yang-uoh-whiff`), rearm+1 for two (`chunli-uoh` = the anchor itself, one tick after rearm; `remy-uoh` = NO traced engine event at that tick, only a mid-cell `cgctr` decrement), rearm+3 (= the natural r1 end) for one (`urien-headbutt` lp/mp/hp, 8 currently-PASS legs). Two identical-signature pairs land on opposite sides: `ibuki-kazekiri-lk` (+0) and `urien-headbutt` (+3) re-arm under the SAME post-landing `cghi=1` label with the same gap magnitude (3); `yun-uoh`/`yang-uoh` whiff (+0) and `remy-uoh` (+1) share that same `cghi=1` label too. No candidate discriminator (gap magnitude, gap-cell label, anchor type, cut status, cell-ordinal position — re-plan §2.1-2.5) separates the two populations; the only untested lead, per-cell `cg_type`, has no trace column today and is already half-falsified by the mid-cell remy-uoh end (re-plan §2.7). Status: no lever ships; `yang-senkyuutai-lk`, `ibuki-kazekiri-lk`, `yun-uoh`, `yang-uoh`, `remy-uoh` all stay xfail with this census as their citation; `chunli-uoh` and `urien-headbutt` stay PASS, unaffected. **DATED NOTE (2026-07-11, LAYER-1):** under the arcade counting rule (a convention-twin's from-scratch gflg-edge derivation, not an overlay-endpoint retime), the engine's own gflg edge reproduces arcade R exactly on `yang-senkyuutai-lk` (34), `ibuki-kazekiri-lk` (26), `yun-uoh` (6), `remy-uoh` (5) AND `chunli-uoh` (5, no regression) -- see §13.16's chunli reconciliation. Lever-K's REJECTION above stands unchanged: it was a retime of the overlay's existing per-move R endpoint onto the rearm event, and whether that lands on arcade depends on where each move's overlay endpoint sits relative to rearm (+0/+1/+3, this census's own irreducible heterogeneity) -- the twin's uniform strictly-between rule sidesteps that heterogeneity entirely by deriving R from scratch, it does not resurrect lever-K. `urien-headbutt`'s rearm+3 was never arcade-captured, so whether arcade's own busy-R there reads 16 (twin) or 19 (golden/oracle) stays UNKNOWN -- the twin's own P4 bound (§13.16). **DATED NOTE (2026-07-11, RE-ANCHOR-1 SHIPPED, §13.17):** the P4 unknown above is now resolved -- Session 4/5 hardware capture confirms `urien-headbutt`'s busy-R=16/15/16 IS the arcade actionable value (OUTCOME A), and `yang-senkyuutai-lk-whiff` flips XFAIL->PASS at R=34 via lever N (whiff busy-edge R). This is NOT lever K resurrected: lever N derives R from scratch under one uniform strictly-between rule (no overlay endpoint, no retime of `attacker_idle`), sidestepping this census's own rearm+0/+1/+3 heterogeneity entirely rather than resolving it -- see §13.17's "why not ENGINE-7 redux" paragraph. `ibuki-kazekiri-lk`/`yun-uoh`/`yang-uoh` also flip via the same lever; `remy-uoh`'s R-clause flips too (its A-clause is the separate "no-cut re-entry re-crediting" row below). `chunli-uoh` (already PASS) is unaffected -- lever N reproduces its existing PASS value byte-for-byte. |
| **Hayate compound** | `makoto-hayate-*` (6 entries) | A-undercount (lever-F-independent) + outcome-sign-flipping R (block +8, hit −4) | UNCLASSIFIED, top of the user-review list. **UPDATE 2026-07-10 (sweep #2):** the A-clause is RESOLVED — (a)-PARTIAL under F1, own ledger (s5-rundir/run-makoto): whiff credits 16(add=3)+20(add=3)=6=arcade; BOTH contact outcomes credit only 16(3)=3 — cgix=20 never credited; deficit = the skipped cell's declared duration exactly; lever-F already tested UNCHANGED. All six entries now assert `A: 3` (§13.13), xfails narrowed to the R clauses. The R sign-flip (block +8 / hit −4) stays OPEN — disposition (e). **UPDATE (2026-07-11, sweep #3): R clause CLOSED, F2, all six entries plain PASS.** The two-term "+2 uniform hitstop-conversion term" framing is MOOT — the audit's own per-tick recount (s5-rundir/run-makoto preserved P2tick stream) closes both signs as a SINGLE branch-content differential: BLOCK +8 = dwell cell 19 ticks (cgix60) vs whiff's 11 (cgix20), the 3-cell detour (40/44/48, 6 ticks) exactly replacing whiff's 0/4/8 (6 ticks); HIT −4 = whiff's cgix4+cgix8 cells (2+2) skipped outright, dwell unchanged (cgix12, 11 ticks = whiff's 11). F2's bar met as written (own recorded skip/branch cgix signature for this move). Row fully resolved; the sweep-2 "(e) OPEN" disposition is retired. |
| **Block-adv anomaly, S/A/R exact** | `remy-crroundhouse-block` (adv −41 vs −11, T−sum gap 16); `remy-lov-{lp,lk}-block` (adv +23 vs +5 / +17 vs +1, projectile) | numbers exact except adv | UNCLASSIFIED. **UPDATE 2026-07-10 (sweep #2): the row SPLITS.** (i) `remy-lov` ×3 = mechanism-ESTABLISHED, disposition (b): measured adv = arcade adv + projectile travel ticks, closed exactly on all three members (travel 18/16/14 → +23/+17/+15 vs +5/+1/+1); blockstun anatomy identical (27 ticks post-contact), attacker anchor exact; oracle Block_advantage is travel-0 canonical (s11 members 2-4). Stays xfail, arcade adv stays in expect. (ii) `remy-crroundhouse-block` = **ENGINE-9 candidate**: two-contact move; the overlay's def_idle anchor latches blockstun exit #1 (F=33) instead of the FINAL exit (F=63); using the final exit reproduces arcade −11 EXACTLY (`frame_data_overlay.c:1208-1213` first-edge gate, :1187 event latch; s11 member 1). The defect is on the DEFENDER anchor; S/A/R are green. See completion-plan ENGINE-9 row. **UPDATE (2026-07-10,
CONV-2):** the remy-lov side (i) now converts (family F10) - adv=23/17/15
on the three members, all FULL. The ENGINE-9 side (ii,
`remy-crroundhouse-block`) remains excluded - defective value pending the
fix cycle. **UPDATE (2026-07-11, ENGINE-9 RESOLVED/SHIPPED — lever M,
§13.14):** side (ii) is FIXED. Census (`e9-census.tsv` +
`e9-census-report.md`, 1,039 windows, RUNG 0): only 2 windows suite-wide
have a disjoint exit-then-re-stun topology; the shipped M1 re-arm fires on
exactly `remy-crroundhouse-block` (the other, `q-uoh-chain-retrigger`, is
correctly refused by the `cgix_reset_frame<0` same-chart gate); adv now
−11 == remy.json exactly, entry plain PASS, xfail removed. The §3.3.5
accounting-defect exemption's conditions (a) population-exactness /
(b) zero no-op-branch drift / (c) oracle-exactness all PASSED — see
§13.14 for the normative mechanism, census table, mutation contract, and
residuals. This row's remaining live member is the closed remy-lov side
only. |
| **Two-way contact R=0** | `oro-jinchu-lk-block/-hit` | BLOCK=HIT R=0 vs 19, whiff exact post-§13.5.1a | UNCLASSIFIED (GRANTED). **UPDATE 2026-07-10 (sweep #2):** CONFIRMED as **ENGINE-8 candidate** — r1-clear/already_idle end-detection fires mid-animation on contact legs: r1 4→0 concurrent with a discontinuous cgix 80→120 jump into a non-idle segment (cghi=14) that keeps advancing 16 (BLOCK) / 31 (HIT) real frames before chart idle; whiff is the opposite (r1 conservative, arcade-exact) (s9-findings, independently re-traced sweep #2). Fix cycle must also decide whether r1=0 means genuinely actionable (in which case the ORACLE row is the question) — both forks are the cycle's charter. See completion-plan ENGINE-8 row. **UPDATE (2026-07-11, ENGINE-8 CLOSED — ARCADE-WHIFF-CANONICAL / NO-CODE-FIX-EXISTS, engine8-plan.md Stage 1, orchestrator Ruling 2's branch (c), autonomous):** the observer-defect hypothesis this row's classification implied is RETIRED. The move genuinely ends the attack routine at the bounce and hands the attacker back to normal, airborne control (r1 4→0 = `Player_normal` dispatch, `plmain.c:332`/:1134; cghi=14 is the same generic airborne-jump-family animation a plain jump occupies, confirmed by the `oro-jump-none` negative control emitting zero trace rows the same way) — this is engine behavior, not a meter defect. The "16 (BLOCK) / 31 (HIT) real frames before chart idle" figures in the row above are themselves corrected: BLOCK's chart never reaches idle inside the observable window at all (the prior reading was the DEFENDER's own exit-from-blockstun columns, misread as the chart's); HIT's true figures are 32 ticks to the first landing tick and 52 ticks to stable idle (r2=1), with r2 held at 6 through the landing tick and only flipping to 2 the tick after (a further correction to an intermediate draft's "r2 still 6 through landing" claim). Every candidate end anchor was measured and NONE reaches arcade's 19 (r1-clear R=0; first chart-idle label BLOCK truncated ≥16 visible / HIT 32; touchdown ~33-41; stable idle HIT 52 / BLOCK ≥53) — re-verified independently this pass by a fresh 1,058-window census (`e8-census.tsv`/`e8-census-report.md`), which reproduces the HIT-leg 52-tick figure exactly. A dedicated actionability probe (`e8-airact.yaml`) found the post-bounce tail **NOT-ACTIONABLE**: of 4 staggered air-normal presses at the BLOCK leg's own tail, 3 (immediate/early/mid, incl. the decisive +1-tick) are directly observed negatives — each registers at the input level (`sw_new` edge) but produces zero gameplay effect (r1/athok/hatt/jatix/cghi unchanged) — while the 4th (late, +24) truncates before its own press tick, so its negative is inferred, not observed; a plain-jump positive control shows the full signature cleanly — the probe is valid and the negative is genuine, foreclosing the "r1=0 secretly means actionable" reading that would have justified a different display. The census additionally named 3 same-shape CANDIDATE windows (`oro-throw-hit`, `alex-powerbomb-lp-hit`/`-unblockable-probe`, `ibuki-kubiori-lp-hit`) — not converted (candidates never convert): `oro-throw-hit` is mechanistically distinct (§13.10 Class 3 connected-grab R-derivation via raw_len saturation, no cgix/cghi discontinuity at its release tick); the other two are tooling-inconclusive (command-grab multi-phase r1 shape outside this census's single-pass scope, honestly flagged rather than asserted). **Zero code shipped, zero expect/golden changes** — `oro-jinchu-lk-block/-hit` stay XFAIL with arcade R=19; suite counts unchanged, 980 PASS / 79 XFAIL. See `docs/plan-frame-data-completion.md`'s ENGINE-8 row for the full tracker closure. **RESOLVED for HIT, unmeasurable for BLOCK (2026-07-14, ENGINE-JINCHU):** TRACK-A Session 6 hardware capture falsifies this row's own oracle-derived R=19 closure — arcade truth is base-LK 33/33 (symmetric HIT/BLOCK) and EX 34-HIT/52-BLOCK (asymmetric). Lever U (`fd_jinchu_bounce_recovery_r`) admits the two HIT legs' own recovery_pf==0 gate-firer past lever R's G7: `oro-jinchu-lk-hit` converts to PASS (busy-edge 33 == arcade 33 exact); `oro-exjinchu-hit` displays 0->33 but stays XFAIL (arcade 34, a genuine 1-frame divergence). Both BLOCK legs stay exactly as this row describes — unmeasurable, busy edge never latches, R=0, XFAIL now asserting the corrected arcade literal (33/52) instead of the falsified oracle (19/16). See §13.21 for the full record and `docs/arcade-frame-data/ERRATA.md`'s new entry for the oracle-falsification verdict. |
| **No-cut re-entry re-crediting** (NEW 2026-07-10, sweep #2) | `remy-uoh-whiff/-block/-hit` A-clauses | own ledger (s4): whiff/hit visit cgix 16→24→28→32→24 (one genuine re-entry, ea=11); BLOCK adds one more loop (→28, ea=13); +1 (whiff/hit) and +3 (block) vs arcade 10 both explained by re-entry counts exactly; cut=0 excludes §12.2.1 membership | the third UOH A-variant in this repo (distinct from §12.2.1's sentinel and looping-declared shapes); ledger-proven mechanism, disposition (b); stays xfail (whiff never converts per §13.13 excl 1; R-clauses remain item 18(c), untouched). **UPDATE (2026-07-10, CONV-2):** contact-leg A clauses (`remy-uoh-block`/`-hit`) now convert to measured engine truth (family F9) - both PARTIAL, A=13/11, R clause stays item 18(c) untouched. |
| **Contact-skip R-window reclassification** (NEW 2026-07-10, sweep #2) | `yun-crfierce-block` (MEASURED member); `elena-lynxtail-lk-block` R clause (CANDIDATE) | contact skips the remaining active cells, so whiff-side inter-active gap content gets counted in the R window instead — yun: gap cell 40 (jatix=0)'s single unfrozen tick survives (GT=787, P2-cited; the rest of the gap, cells 20-36, is excluded by the negative-hstop guard freeze, hstop=−6…−1, GT=781-786; Block Rwin=40(1)+52(5)+56(5)+60(4)+64(1)=16 exact vs whiff Rwin=15); elena: the BLOCK-only +8 == whiff's own 8-tick inter-window gap declared length, cgctr-identical tail (candidate only — not per-tick closed) | NOT branch content (this MEASURED mechanism refutes the prior §12.2.3-R-corollary candidate framing for yun-crfierce-block); disposition (b); both stay xfail. **UPDATE (2026-07-10, CONV-2):** the MEASURED member (`yun-crfierce-block`) now converts (family F7) - R=16, entry now plain PASS (A already converted under F1). The CANDIDATE (`elena-lynxtail-lk-block`) stays xfail - candidates never convert. **DATED CORRECTION (2026-07-11, sweep #3): the CANDIDATE is REFUTED, not merely un-promoted.** A per-tick decomposition (`s12-rundir/run-elena/trace.log`, F138-158) shows the +8 surplus is 6 gap-heritage ticks (cells 24/28, whiff-identical) + 2 second-freeze ticks at cell40 (hstop 2→0 at a non-gap, non-contact cell with no whiff-side analog) — the old "+8 == whiff's own 8-tick gap" framing was a coincidence of totals, not the per-tick match the candidate implied. `elena-lynxtail-lk-block`'s R clause is now (e)-PERMANENT for this candidacy: candidates never convert (unchanged), but the candidacy itself is honestly terminal — what-was-tried is this per-tick decomposition, and no further probe exists (the remaining question is a hitstop-at-unrelated-cell accounting question with no family). |
| **Contact-mutated declared cell duration** (NEW 2026-07-10, sweep #2; first audit §E.3, upgraded (e)→(b)) | `ken-tatsu-lk-block/-hit` | no branch/skip cells anywhere; cell 28's declared cgctr mutates 3→1 on contact (same jatix=56), raw T conserved at 38 on all legs; the +1 R materializes as cell 48 counting 2 ticks on contact vs whiff's 1 (rule-transfer recount, s2 ken trace, raw_len=38 reproduced on all 3 legs) | no F2 signature exists — never (a); disposition (b); stays xfail; adv never asserted (F-2 constraint). **UPDATE (2026-07-10, CONV-2):** both members (`ken-tatsu-lk-block`/`-hit`) now convert (family F8) - R=15, entries now plain PASS; adv remains unasserted (F-2 constraint unchanged). |
| **Same-tick interior-transition credit banking** (NEW 2026-07-11, sweep #3, CONFIRMED-PENDING-GRANT) | `remy-cbk-lk-block/-hit` A-clauses (loop-back re-entry, +3); `elena-lynxtail-lk-block/-hit` A-clauses (skip-path pass-through, +1/+1 and +1) | `char_move()`'s interior same-tick `cg_ix` transitions bank each entered cell's declared credit while the per-frame snapshot observes only the tick-final cell. Ledger-verified raw both manifestations: remy — within one Game_timer tick the chart passes 52→44→4, re-entering cell 44 and banking its declared 3 a second time while the snapshot only ever observes cgix=4 (block GT3393/3415, hit GT3700/3713/3716/3719/3722); A=14 = whiff's own 11 + 3 phantom, exact both legs. elena — skip-path pass-through credit of a skipped window's first, shorter cell (phantom 12:1@GT364/GT673 + 32:1@GT374, snapshots show cgix=20/40; the second, longer cell 16/36 is genuinely never credited); A=2 (block)/4 (hit) exact. The F1-rider test is REFUTED for both — banking is not clean skip arithmetic (naive skip predicts elena block A=0/hit A=3; actual 2/4, off by exactly the phantom credits) | one registry row, two manifestation topologies (loop-back re-entry vs skip-path pass-through) — Lane 3's cross-lane kinship observation, adopted: cbk and lynxtail-A are the SAME new mechanism class. PROVEN, not hedged, but per the ENGINE-8 governance route (a newly-proven mechanism with no at-grant-time family does not self-license conversion) lands **(b) CONFIRMED-PENDING-GRANT** — registry row + user grant list, no conversion this run. `remy-cbk-lk-whiff` stays excluded (§13.13 excl 1, WHIFF); its own separately-PROVEN boundary-tick finding (F=1207) is unaffected and unrelated. Grant, if given: `elena-lynxtail-lk-hit` (A sole clause) would FLIP with A:4; `elena-lynxtail-lk-block` would NARROW only (A:2; its R clause stays open, see the F7 row above); `remy-cbk-lk-block/-hit` would convert only if the user ALSO accepts the whiff-level ±1 boundary shift (A11/R9 vs 10/10, above) as displayed engine truth (A:14 = 10 + 1 boundary + 3 phantom; R:9) — otherwise narrow only. See `docs/plan-frame-data-completion.md`'s SWEEP-3 row and USER GRANT LIST for the consolidated grant terms. **UPDATE (2026-07-11, G2 GRANTED — user grant 2026-07-11, following the orchestrator's recommendation review):** this class is GRANTED, family **F13** "Same-tick interior-transition credit banking" (§13.13 SCOPE EXTENSION). `elena-lynxtail-lk-hit` FULL CONVERTS (A:4, its sole blocking clause — plain PASS). `elena-lynxtail-lk-block` NARROWS (A:2 converts; its R clause stays XFAIL, (e)-PERMANENT per the F7 row's candidacy refutation above — unaffected by this grant). `remy-cbk-lk-block/-hit` do **NOT** convert: the grant text ("the same-tick credit-banking class ... becomes a granted family") grants the banking mechanism itself but does not additionally state acceptance of `remy-cbk-lk-whiff`'s own separate, still-UNCLASSIFIED ±1 boundary-shift finding — a precondition the grant list's own text makes explicit for converting these two legs. Dependency unmet as stated; both entries stay fully XFAIL, unconverted (see corpus-remy.yaml's xfail strings for the full disposition). Net this grant: **+1 flip** (`elena-lynxtail-lk-hit`). **DATED NOTE (2026-07-11, LAYER-1, G2-precondition VOID):** the `remy-cbk-lk-block/-hit` dependency above ("convert only if the user ALSO accepts the whiff-level ±1 boundary shift ... as displayed engine truth") is now RESOLVED BY MEASUREMENT, not merely still-unmet: arcade 990512 whiff = **17/10/10 == oracle** (2 reps byte-identical, `docs/arcade-frame-data/CAPTURE.md`'s new Session 3), and the convention-twin (a)-classifies it too -- engine-raw whiff = 17/10/10. The overlay's 11/9 (the "proven F=1207 boundary tick") is a METER ARTIFACT, not engine truth: the overlay counts a last-hatt tick whose raw box dims are already zero. The precondition is therefore VOID, not merely unsatisfiable-as-stated: accepting the boundary shift as displayed engine truth would enshrine a MEASURED-WRONG value (house rule: wrong values never ship), so the pending +2 CBK flips are CANCELLED as a grant question entirely, not left pending. `remy-cbk-lk-whiff/-block/-hit` stay xfail at oracle values in `expect` until the METER fix lands (post-fix, whiff should measure 17/10/10 and flip naturally; contact legs' A=14 = 10 + 1 boundary-artifact + 3 banked-credit re-partitions under the fixed meter, re-measured then, not predicted now). No F13 family change: the banking mechanism grant itself is untouched. See `docs/plan-frame-data-completion.md`'s SWEEP-3 user grant list G2 entry for the corresponding dated note. |
| **Contact-outcome hold/skip differential** (NEW 2026-07-11, sweep #3, CONFIRMED-PENDING-GRANT) | `ibuki-uoh-block/-hit` | per-tick closure from the preserved s12 trace (F136-163/F185-208): +2 REAL ticks at the hitstop-exit cell cgix28 (block holds cgctr=2 for 3 rows post-thaw, F147-149, vs hit's 1 row, F195-197; active_pf 12 vs 10 — inside the jatix=101 active window, masked in displayed A by the §13.11 anchor_a=11 convention), plus +2 in the R window (block plays cgix40's cgctr=1 tick + the 1-tick cgix44 sentinel, F158-159; hit skips both, F204→F205 straight to cgix0); cleanup cgix0 identical 3v3. T differential +4 and R differential +2 close exactly, zero residual on both legs against whiff-anchored arithmetic (S+active_pf+R=T on all three legs) | NOT F2 (divergence partly inside the real active window — a mixed A/R-redistribution shape), NOT F6 (enumerated to chunli only, requires whiff-exact R; ibuki whiff R=3 vs arcade 5), NOT §12.2.1/F4 (synthesis's own NOT-members line predates this sweep). Per the ENGINE-8 governance route, a newly-proven mechanism with no at-grant-time family lands **(b) CONFIRMED-PENDING-GRANT** — registry row + user grant list, no conversion this run. Grant, if given, licenses R:6 (block)/R:4 (hit), arcade 5 preserved (2 potential future flips). See `docs/plan-frame-data-completion.md`'s SWEEP-3 row and USER GRANT LIST for the consolidated grant terms. **UPDATE (2026-07-11, G1 GRANTED — user grant 2026-07-11, following the orchestrator's recommendation review):** this class is GRANTED, family **F12** "Contact-outcome hold/skip differential" (§13.13 SCOPE EXTENSION). `ibuki-uoh-block` FULL CONVERTS (R:6, arcade 5 preserved — plain PASS). `ibuki-uoh-hit` FULL CONVERTS (R:4, arcade 5 preserved — plain PASS). `ibuki-uoh-whiff` is unaffected (item 18(b), stays XFAIL). Net this grant: **+2 flips**. |
| **Same-cgix non-sentinel self-loop** (NEW 2026-07-10, ENGINE-5 closure) | `urien-uoh-whiff/-block/-hit` A-clauses | own ledger (`s4-findings.md` P1, byte-identical all 3 legs): exactly 3 credit events (cgix 20/24/28, add=3 each), `engine_a` plateaus at **9** while the FINAL line's independent classifier reads `active_pf`=**11**; cell 28 (`cgctr=3`, non-sentinel) re-dispatches via `comm_ixbw` (opcode 50, `charset.c:1272-1278`) back into itself with no `cgix` change, so neither the new-cell branch nor the sentinel-dwell branch (`charset.c` `char_move()` :455/:530) fires for the 2 extra real active ticks played | **ENGINE-5 CLOSED (2026-07-10, ARCADE-INTERMEDIATE / NO-PRINCIPLED-TARGET, orchestrator ruling — `engine5-plan.md` + `s4-findings.md`; see `docs/plan-frame-data-completion.md` ENGINE-5 row for the tracker close).** The mechanism above is CONFIRMED and ledger-proven, not merely proposed. **The task brief's arcade premise was WRONG, corrected here:** arcade A for Universal Overhead = **10** (`docs/arcade-frame-data/urien.json:808-812`, `"Hit": "10"`, Startup 15/Recovery 5), re-verified directly — not 11; the brief's "undercounts by 2" framing conflated arcade with engine-played ticks (11), the true undercount vs arcade is 1, not 2. Four reachable quantities, all independently verified (`engine5-plan.md` §1.4/§2.4): arcade (oracle) = **10**; measured (today, no self-loop credit) = **9** (this ledger); played (self-loop credited per-tick, sentinel analogy) = **11** (= `active_pf`); declared (self-loop credited at re-dispatch entry, §13.11-literal cell-entry text) = **12** (clamped fresh `cgctr`=3 credited on re-dispatch). Arcade sits strictly BETWEEN every reachable engine-truth quantity (9 < 10 < 11 < 12). The dispatch-vs-dwell distinction is exactly resolvable in-engine, zero inference: dwell = a `char_move` call where `--wk->cg_ctr != 0` (no reload); re-dispatch = `--wk->cg_ctr == 0` → `check_cm_extended_code` (`charset.c:675-712`) → `check_cgd_patdat` block-copies fresh cell data (`charset.c:2678`), a full cell (re-)entry by the engine's own dispatch code (`charset.c:439-444`). This MECHANISM finding is SOUND and stands. Only the credit AMOUNT is unreachable by a principled rule: the sole rule landing on arcade's 10 requires crediting the re-dispatch tick but excluding the same-tick transit-out tick that follows it — a claim with exactly one supporting sample (this window) contradicted by a live counter-reading (`active_pf`=11). This is bit-for-bit the ownership-fitted-rule shape already REJECTED at n=1 by both the ENGINE-6 and ENGINE-7 census closures above — **pre-registered REJECTED here regardless of any future census outcome** (`engine5-plan.md` §2.4). **Verdict: no lever ships**, closed without running a 19-corpus census (the census machinery from ENGINE-6/7 was judged unnecessary — the n=1 population and the four-quantity impasse are already fully determined from this move's own ledger). `urien-uoh-whiff/-block/-hit` A-clauses stay xfail at measured 9; R-clauses are unaffected and stay OUT of scope (clause-level OUT-1). **Flagged follow-up, NOT decided here** (Phase-4-adjacent, queued on the user-review list): a future USER convention grant could adopt N-played (display 11) or N-declared (display 12) for self-loop charts — the same CONV-2/§13.13 grant route `q-uoh` A=11 shipped through. **G3 CLOSED DECLINED (2026-07-11, CAPTURE-1, orchestrator ruling).** The flagged grant question above is now answered by live arcade hardware, not merely the oracle JSON this row already cited: `docs/arcade-frame-data/CAPTURE.md` captured Urien UOH twice on real hardware and measured active=10/recovery=5 directly from box-count frames — confirming, not merely repeating, `urien.json`'s declared 10. Since arcade genuinely plays 10 and this engine has no reachable quantity that produces 10 (only 9/11/12, all independently verified above), **no grant is offered**: displaying 10 would fabricate a value this engine never actually computes, and displaying 11 (the previously-tempting "played" grant option) would now knowingly enshrine a value confirmed to disagree with BOTH the oracle and live arcade. `urien-uoh-whiff/-block/-hit` A-clauses stay XFAIL at measured 9, re-cited to the capture evidence; the R-clauses (measured 3 vs arcade-confirmed 5, same capture) stay XFAIL too, re-cited the same way — see PORT-DIVERGENCE-1 (§13.13 exclusion item 2's DATED REFRAME) for the consolidated finding this closure now feeds. No future grant route remains open on this move; the question is closed, not merely re-deferred. **DATED CORRECTION (2026-07-11, LAYER-1):** the closure's own premise gets a correction, not a reopening -- a convention-twin engine-raw probe (§13.16) measures this move's RAW frames (the rig's any-of-four-s16 `att_box` predicate) at **box-A=10**, arcade-identical, a fifth engine-native quantity distinct from all four already-tabulated ones (9 measured / 11 played / 12 declared / arcade 10). `active_pf`=11 differs because `h_att_set` ORs in the `fd_engine_hitbox_active` cell-transition flag (`charset.c:453`, `frame_trace.c:121-128`), which outlives raw dims -- ENGINE-10's exact shape, not a new mechanism. The closure REMAINS CLOSED (no credit-rule lever ships; the n=1 ownership-rule rejection stands) -- but a principled target DOES exist, it is just a different meter (raw-box frame count), not a credit-accounting tweak. G3's "displaying 10 would fabricate a value this engine never computes" rationale is superseded the same way: the engine genuinely plays 10 raw box frames; G3 stays CLOSED (no convention grant needed) and the 11-vs-12 display question is expected to DISSOLVE if the meter fix lands (overlay would then read 10 and match arcade/oracle outright). Fix path: the OVERLAY RE-ANCHOR program, census-first, user-gated (`docs/plan-frame-data-completion.md`'s new LAYER-1 tracker row). **DATED NOTE (2026-07-11, RE-ANCHOR-1 SHIPPED, §13.17):** the dissolution flagged above has LANDED, for the WHIFF leg only. Lever O (raw-box A, §13.17) now measures `urien-uoh-whiff`'s A at the fifth engine-native quantity already identified above — box-A=10, arcade/oracle-exact — flipping XFAIL->PASS (item 18 CLOSED FULL for this row, `tools/frame-data/corpus-urien.yaml`'s own RE-ANCHOR-1 note). This does not resurrect the rejected credit-accounting rule (the n=1-overfitted-ownership-rule REJECTION above stands unchanged for the accumulator's own four quantities, 9/10/11/12); it is measured on a different meter entirely (raw box-frame count, not a credit tweak). `urien-uoh-block`/`-hit` are OUT of RE-ANCHOR-1's scope (contact legs, §13.17's explicit "Contact-leg scope" exclusion) and stay XFAIL at measured A=9 — G3 CLOSED DECLINED is unaffected for those two legs, unchanged. |
| **Proj-split late-despawn contact R/adv collapse** (NEW 2026-07-12, EX-CONTACT-R disposition, M3) | `oro-exsundisklow-block/-hit` | attacker's own busy total T=42 identical on all three legs; WHIFF R=27=oracle exactly (the oracle's Recovery is the attacker's post-throw recovery, path-independent); on contact the slow disk connects at raw 35 (travel=20) and despawns NATURALLY at raw 41 (`natend=1`) — the §13.12 proj-split A window runs to despawn, leaving R = the post-despawn attacker-busy remainder = 2; adv measured 21/22 = arcade Block_advantage 1 + travel 20 (+1 hitstun on HIT) EXACTLY, the same arithmetic as the already-converted `remy-lov` F10 family (§12.2.4 Block-adv row above) | new registry row, MEASURED/CONFIRMED — purely display semantics of lever J on a projectile whose lifetime outlives the attacker's recovery (the base Sun Disk Jab, fast/instant contact, measures R=27 exact on all legs, confirming the despawn-time dependence). Disposition: adv converts (F10-signature member, measured literal 21/22, `expect.adv`); R stays structurally unassertable on contact for this move class — asserted instead via the WHIFF leg, which is exact. See `<sp>/exsuper/excontact-r-findings.md` §1.10/§3 item 6 for the full derivation. Wave-5 guidance: Remy LOV EX High/Low ×4 is the hotspot most likely to reproduce this class — check `natend`/`first_active_raw` on contact before quarantining. |
| **Contact-leg box-run loss, multi-hit** (NEW 2026-07-12, EX-CONTACT-R disposition, M2 active-side mirror) | `ryu-extatsu-block/-hit` | the number of real attacker box runs drops with contact (`box_runs` 6 WHIFF → 4 BLOCK → 2 HIT) and displayed R grows by exactly the missing runs' span (R 11 → 14 → 16); R equals `busyr` exactly on every leg — real active-content loss on contact, not a window-accounting artifact. Arcade R=11 is whiff-canonical (its own whiff leg proves it exactly) | new registry row, MEASURED/CONFIRMED via an isolated no-build re-run (2026-07-12, current binary, real `box_a`/`busyr`/`box_runs` fields — rundir preserved). This is the active-side mirror of this section's recovery-content-differential rows above (no existing row stated the active-side/box-run-count shape before this disposition). `ryu-extatsu-block` converts R:14, `ryu-extatsu-hit` converts R:16 (both `expect.R`), arcade 11 preserved in the corpus comment. Ken SA3 (Shipuujinrai Kyaku, legacy path, no shipped corpus entry) is the same family umbrella — see `docs/plan-frame-data-completion.md`'s EXSUPER-1 tracker row for its separate M1 root-cause writeup. See `<sp>/exsuper/excontact-r-findings.md` §1.9/§3 item 9 for the full derivation. |
| **Sum-preserving A/R boundary shift, magnitude-5 (EX)** (NEW 2026-07-12, wave-5) | `necro-exflyingviper-block/-hit/-crouch-probe` | measured S=24/A=10/R=9 vs oracle S=24/A=5/R=14 (adv=+2 both legs), T=43 both ways (S+A+R conserved); reproduced byte-identical across a connecting-distance window (130-230px); crouch-probe byte-identical to the stand-guard BLOCK entry (no defender-stance-conditionality; tested, not inherited, against the base Jab's own crouch-vs-stand swing in the "Defender-stance-conditional attacker R" row above) | NEW, UNCLASSIFIED. Checked against this table's own "Sum-preserving A/R boundary shift" row (`remy-cbk-lk-whiff`): that row's live, ledger-proven member trades magnitude-1 (a single boundary tick, active_pf==engine_a); this EX sighting trades magnitude-5, a different magnitude class, with no own-trace/credit-ledger citation this session (NEVER-build wave-5 scope, no lever-F test run). Per the same-shape grouping rule the EX-CONTACT-R disposition itself established (leg + field + direction + magnitude class + box/cut context), a magnitude-5 shift does not join a magnitude-1 row on name resemblance alone - logged as its OWN new one-off, NOT merged into the row above, same posture that row's own dated corrections used when yun-zesshou/remy's contact legs turned out not to share the whiff leg's exact shape despite an initial "family-of-two" framing. Cites the row above for context only; does not convert. |
| **Contact-only A-overcount, box-backed (real active tick)** (NEW 2026-07-12, wave-5) | `sean-exdragonsmash-block/-hit` | measured A=13 vs arcade 12 on BOTH contact legs, WHIFF leg clean at A=12 (contact-only); box_a=13 == engine_a=13 (real box-tracked active agrees with the displayed credit) - the OPPOSITE of §12.2.3 F3's bar (box_a < engine_a, declared-credit without box backing); HIT-leg A(13) == BLOCK-leg A(13), same shape both legs; R/adv stay arcade-exact on both legs | NEW, UNCLASSIFIED. Explicitly tested against §12.2.3 F3 and REFUSED membership - F3's bar requires box_a < engine_a; here box and engine agree, ruling F3 out by definition (not merely "not yet re-traced"). Two HYPOTHESES stated, neither confirmed: a genuine extra real active tick fires only on the contact branch (distinct from F3's declared-only insertion), or an unisolated contact-tick artifact - no lever-F toggle test run (NEVER-build wave-5 scope). No existing §12.2.4 row shares this exact box_a==engine_a-on-contact signature. Flagged to the orchestrator/user for follow-up classification, per Phase 6's "candidates never convert without their own recorded signature" discipline. |
| **Non-sum-preserving joint A/R shift, adv-exact (EX)** (NEW 2026-07-12, wave-6) | `remy-exrrf-block/-hit` | measured S=6/A=3/R=38 vs oracle S=6/A=5/R=41 (adv=-21 both legs, exact); T (S+A+R) = 46 on WHIFF's own clean reading vs T=41 on BLOCK/HIT - **NOT conserved** (Δ=-5), disqualifying this from the "Sum-preserving A/R boundary shift" row above (that row's bar is T constant; here it moves) | NEW, UNCLASSIFIED. Explicitly checked against S+A+R conservation (the sum-preserving row's own defining test) and FAILED it - both A and R drop together (non-compensating) rather than trading ticks, a distinct shape from both the magnitude-1 (`remy-cbk-lk-whiff`) and magnitude-5 (`necro-exflyingviper`) sum-preserving members. Also checked against Phase-6 bucket 1 (R-exact) and bucket 7 (A-exact) - fails both since both fields move. Does not match the base RRF's own §12.2.3 F3 A-overcount precedent (this member UNDERcounts, opposite sign). No lever-F toggle test run (NEVER-build wave-6 scope). Juggle check passes (BLOCK A=3 == HIT A=3). Flagged for follow-up classification. **Related but distinct sighting, same corpus, NOT added as its own row:** `remy-excbk-block/-hit` (measured S=18/A=11/R=9 vs oracle S=18/A=9/R=10, adv=-1 both legs exact; T=19 WHIFF vs T=20 BLOCK/HIT, also not conserved, Δ=+1, opposite sign from RRF's own Δ=-5) was kept at the SAME conservative fully-xfail disposition as the base (non-EX) Cold Blue Kick entry `remy-cbk-lk-block/-hit` (this table's own "Same-tick interior-transition credit banking" row, F13) purely as a precedent-following caution, NOT as a claim that it shares that row's mechanism or the sum-preserving shape - its own T is not conserved either, so it is neither a sum-preserving member nor a same-tick-interior-transition member on the numbers; it remains an unlinked, undirected xfail pending its own investigation. |
| **BUFFER-1 capture-depth-raise regression** (NEW 2026-07-13, BUFFER-1 fix) | `elena-exspinscythe-block` | previously a plain PASS at R=31, oracle-exact by coincidence of the old `FD_CAPTURE_LEN=72` cap; post-raise (256) measured R=41 (busyr=43) vs oracle 31 (`elena.json` Spinning Scythe (EX) Recovery=31) — a genuine engine/oracle divergence only exposed once the capture window is long enough to observe this leg's true recovery span, not a new engine behavior | NEW, UNCLASSIFIED. Not part of the M1 register (that register's members were all previously R-omitted/xfailed pending the raise; this row was previously a clean PASS and is the opposite direction — the raise EXPOSES a divergence rather than resolving one). Per house rule (wrong values never ship), converted from a stale PASS to an explicit `xfail` asserting the ORACLE value (from-qjson) — same idiom as `corpus-akuma-sa2.yaml`'s A-clause (assert oracle, xfail the measured/busyr divergence, cite the exposing mechanism) rather than a silent de-assert. No per-tick declared-credit ledger trace taken this session; flagged for orchestrator/user follow-up classification. See `tools/frame-data/corpus-elena-ex.yaml`'s `elena-exspinscythe-block` entry for the dated note. **Housekeeping, same sighting:** the pre-existing `:947` M1 note above cites its own Elena control as `elena-lynxtail-lk-block` (Spinning Scythe (EX)) — those two labels name two DIFFERENT Elena EX moves (`elena-lynxtail-lk-block` is Lynx Tail (Short), a non-EX move, per `corpus-elena.yaml:770`; Spinning Scythe (EX) is `elena-exspinscythe-block`, per `corpus-elena-ex.yaml:265`, this row's own subject). That label/description conflict predates this fix and is left as-is here (not silently resolved by picking a reading) rather than assumed to mean either move without verification. |

**Trigger re-check (wave-5, 2026-07-12), authoring-policy.md's "≥3 same-shaped quarantines ⇒ STOP":** both new sightings above are first-of-their-shape - verified against every existing §12.2.4 row and the EX-CONTACT-R program ledger: the necro sighting's magnitude class (5) is distinct from the existing sum-preserving row's (1), and the sean sighting's box_a==engine_a contact shape has no precedent anywhere in this document (direct grep, zero prior hits). Neither joins an existing bucket to 3+, and neither forms its own 3-member bucket alone (1 sighting each). Trigger NOT tripped this wave.

**Trigger re-check (wave-6, 2026-07-12), same discipline:** three new sightings this wave - `remy-exrrf-block/-hit` (new one-off, above), `remy-excbk-block/-hit` (not new; precedent-following continuation of an existing base-move xfail, not itself sum-preserving despite surface resemblance), and `hugo-exml-hit` (A undercounts 2 vs true 6, HYPOTHESIS ONLY against the existing Phase-6 "contact-A undercount, shape (a1)/(a2)" family already CONFIRMED elsewhere for `ibuki-extsumuji-block/-hit`, but not itself converted - magnitude differs, 4-tick loss here vs Ibuki's 1-tick, no lever-F toggle test run to confirm family membership). Precisely: RRF-EX and CBK-EX both show "A and R move together with adv held exact" but NEITHER conserves T (RRF Δ=-5, CBK Δ=+1, opposite signs from each other too) - they are not the same shape as each other, and neither is the sum-preserving shape necro/yun/remy's own whiff leg already registered above; they are correctly kept as two separate, unlinked xfail sightings, not merged. Hugo's ML-EX sighting is directionally consistent with the established (a1)/(a2) undercount family (loses credit, never gains) but is not confirmed as a member - flagged for the named lever-F follow-up, not converted, not force-merged. No group reaches the ≥3 threshold this wave (RRF-EX: 1 new one-off; CBK-EX: 0 new, precedent-following; Hugo ML: 1 hypothesis-only sighting against an already-established different family). Trigger NOT tripped.

**Trigger re-check (supers pilot, 2026-07-13), same discipline:** one new sighting this wave — `chunli-sa2-block` A (+1 overcount: measured legacy-displayed A=20 vs oracle 19; S=2/adv=-23 both oracle-exact on the same leg; the HIT leg's displayed A reads the oracle-exact 19, left unasserted only because the mechanical juggle check fails 19≠20; `box_a`=20 == displayed A; `box_runs`=1; no WHIFF control exists — WHIFF-UNREACHABLE, walk_in=198px). Shape-matched against Phase-6 bucket 8 (§12.2.3, the mirror-of-#17 family) by its outcome-differential signature ("different wrong values per outcome": BLOCK 20 vs HIT 19) and xfailed as an unconfirmed bucket-8 CANDIDATE in `tools/frame-data/corpus-chunli-sa2.yaml` — NOT converted: no per-tick declared-credit ledger trace was taken (the bucket's own candidate bar), and its box_a==displayed-A agreement is the OPPOSITE of F3's box_a<engine_a declared-credit-without-box-backing bar — the same by-definition tension that REFUSED `sean-exdragonsmash-block/-hit` F3 membership above. Honest grouping vs that sean row ("Contact-only A-overcount, box-backed"): shares the box-backed contact-A-overcount core, but the per-outcome pattern differs (sean: identical A=13 on BOTH contact legs, WHIFF clean at 12; chun: BLOCK-only +1 with the HIT leg reading oracle-exact) — grouped at the coarsest honest grain, "box-backed contact A-overcount" counts 2 sightings (`sean-exdragonsmash`, `chunli-sa2-block`), not force-merged into one row and not merged into §12.2.3's converted member set. No group reaches the ≥3 threshold (box-backed A-overcount: 2; unconverted §12.2.3-signature candidates: 1). Trigger NOT tripped.

**M1 note — FD_METER_LEN=72 contact-truncation (2026-07-12, EX-CONTACT-R, answering Elena Lynx Tail (EX) T=72 follow-up (b)):** two contact legs are now PROVEN raw[]-capture casualties of the `FD_METER_LEN=72` cap (`frame_data_overlay.c:42`) — `dudley-exmgb-block` (busyr=28=oracle exactly, displayed R=25 clipped by 3 — the ticks lost past the cap) and Ken SA3 BLOCK (non-frozen post-last-active span=27=oracle exactly on both the legacy `hatt`-proxy trace and, as of the 2026-07-12 isolated re-run, the current binary's real `box_a`/`busyr`/`box_runs` fields — raw_len still saturates at 72 either way). **General rule, resolving the general question this section's Sum-preserving/Cut-committed rows left open and Elena's own T=72 signature (§13.10's Class-3 FD_METER_LEN coupling note neighborhood):** T=72 alone is NOT truncation; truncation occurs iff the R window is still open at raw index 71 (S + last-active raw index + R reach index 71). The Elena control, `elena-lynxtail-lk-block` (Spinning Scythe (EX)), had S+A+R ≤ 71 inside its own T=72 capture and measures exact — consistent, now explained rather than merely observed. Wave-5 checklist: if a FINAL shows `raw_len=72` AND S+last-active+R reach index 71, check `busyr` against oracle before quarantining (never assert the clipped R; never quarantine it as "unexplained" either — cite this note). Named wave-5 watch list: Necro Spinning Punch (EX) (long multi-hit flurry, MGB-shaped), Q HSB (EX) (base HSB is Class-1 multi-hit), Hugo Monster Lariat (EX) (long travel). See `<sp>/exsuper/excontact-r-findings.md` §1.1/§1.2/§5 item 1.

**M1 register — class growth (2026-07-13, supers pilot):** the saturation class grows from 2 proven legs to **5 legs across 3 moves, spanning both EX and SUPER classes**. The two originals (`dudley-exmgb-block`, busyr=28=oracle with displayed R clipped by 3; Ken SA3 BLOCK, true span=27=oracle vs displayed R=37) are joined by three legs measured this session, each with `T=72=FD_METER_LEN` exactly: **Ken SA3 HIT** (displayed R=1 vs oracle 27 — a previously-unmeasured leg of the already-named quarantine, not a new seed), **Chun-Li SA2 BLOCK** (displayed R=0, busyr=38) and **Chun-Li SA2 HIT** (displayed R=0, busyr=35 = oracle EXACTLY, reproducing step2b's preserved box_a=19/busyr=35 reading byte-for-byte). Same single mechanism (raw[] capture saturates at `FD_METER_LEN=72`, `frame_data_overlay.c:42`), cited per-leg with the established "omit R with the M1 citation" idiom in `tools/frame-data/corpus-ken-sa3.yaml` and `corpus-chunli-sa2.yaml` — R is never asserted from busyr on a contact leg (Ruling 2 E-1/E-4). With 5+ members and every forthcoming long-duration super a structural candidate (any move whose R window is still open at raw index 71), the class is now a **NAMED CANDIDATE for a dedicated FD_METER_LEN-raise fix cycle** (raise the cap or re-window the capture; engine-observer-only, lever-gated like every prior overlay fix) — recorded **NOT STARTED**, no design or build attempted this session.

**M1 register — class growth (2026-07-13, supers wave S1):** the saturation class grows from 5 legs/3 moves to **14 legs across 7 moves**. Four new moves join this session, each with `T=72=FD_METER_LEN` exactly and `busyr` corroborating the oracle: **Ken SA1 Shoryureppa** (all 3 legs — BLOCK/HIT/WHIFF — `busyr`=45=oracle exactly on every leg, the first M1 sighting to hold on a WHIFF leg for this move), **Ken SA2 Shinryuken** (BLOCK+HIT only, `busyr`=61=oracle exactly on both; its own WHIFF leg is the wave's control-negative — R=61 asserts clean, NOT truncated, despite the identical T=72 flag, refining the general rule already on record: T=72 alone never implied truncation, and this is the first WHIFF-leg confirmation of that non-implication), **Chun-Li SA1 Kikoshou** (all 3 legs, `busyr`=48=oracle exactly on every leg — "the cleanest M1 sighting in the whole program yet", per-leg agreement where every prior member matched on at most 2 of its legs), and **Chun-Li SA3 Tensei Ranka** (WHIFF leg only — `busyr`=46=oracle exactly; this move's own BLOCK/HIT legs are explicitly NOT M1, see the new UNCLASSIFIED R-surplus note below). Ryu SA2/SA3 contribute zero M1 legs this wave (Ryu SA2 HIT-R diverges but `T=63` rules M1 out explicitly; Ryu SA2 BLOCK/WHIFF R and Ryu SA3 HIT R all assert clean, oracle-exact). Same single mechanism throughout (`frame_data_overlay.c:42`), cited per-leg with the established "omit R with the M1 citation" idiom in `corpus-ken-sa1.yaml`/`corpus-ken-sa2.yaml`/`corpus-chunli-sa1.yaml`/`corpus-chunli-sa3.yaml`. The FD_METER_LEN-raise fix-cycle candidate (named, NOT STARTED) now has 14 members spanning EX and SUPER classes across 7 characters' moves — no design or build attempted this session.

**New UNCLASSIFIED sighting — Chun-Li SA3 Tensei Ranka BLOCK+HIT R (2026-07-13, supers wave S1):** a uniform +2 legacy surplus (measured R=48 vs oracle 46) on both contact legs, with `busyr`=62 on both — `busyr` disagreeing with oracle by a wide margin explicitly RULES OUT M1 for these two legs specifically (the same move's own WHIFF leg, above, cleanly fits M1 instead — the two shapes coexist on one move). Checked against Phase 6 bucket 7's R-surplus direction and refused (no per-tick ledger trace taken). Only 2 members of this exact shape (the move's own BLOCK+HIT legs) — below the ≥3 threshold; not merged with any other registry row. Omitted (not xfailed), flagged for follow-up.

**New UNCLASSIFIED sighting — Ryu SA2 Shin Shoryuken HIT-R (2026-07-13, supers wave S1):** measured R=40 vs the oracle-exact 35 both BLOCK and WHIFF legs of the same move read. Does not fit any Phase-6 HIT-R bucket (Class 1 requires a SHORTER hit-branch recovery, not longer; Class 2 is UOH-specific; Class 3 is connected grabs); `T=63` rules out M1. HYPOTHESIS ONLY: possibly a genuine juggle/knockdown-continuation extension specific to this multi-hit super's HIT leg. One member, standalone — no existing bucket matches (leg-scope and magnitude both differ from the Chun-Li SA3 sighting above: HIT-only vs BLOCK+HIT, +5 vs +2). Omitted, flagged for follow-up.

**New shape, checked and NOT merged — whiff-inclusive uniform A undercount (2026-07-13, supers wave S1):** two sightings this wave carry a superficially matching signature — **Ken SA1 Shoryureppa** (measured A=33 vs oracle 34, -1, reproduced identically on ALL THREE legs including a clean WHIFF) and **Chun-Li SA1 Kikoshou** (measured A=19 vs oracle 20, -1, likewise reproduced on all three legs including WHIFF) — same magnitude (-1), same whiff-inclusive reproduction, ruling out every established contact-only bucket (1/7/8) on both. Explicitly tested against each other before grouping, per this document's own "surface resemblance is not membership" discipline (the Remy RRF-EX/CBK-EX precedent, §12.2.4 wave-6 note): the two sightings' *coupling to Block_advantage* differs. Ken SA1's BLOCK adv (-38) is oracle-exact despite the A undercount — the divergence is A-isolated. Chun-Li SA1's BLOCK adv (-38 vs oracle -39, a +1 shift) moves in lockstep with its own A undercount — the divergence is A+adv-coupled. Different internal mechanism signature; NOT merged into one shape. Ken SA1's sighting is **ESCALATED** per CORPUS-AUTHORING.md Phase 4's own rule ("an unexplained whiff divergence is a STOP, not an xfail") — ships OMITTED (not xfailed) on all 3 legs, flagged for orchestrator/user follow-up needing a lever-F toggle build (no build was authorized this session). Chun-Li SA1's sighting ships `xfail`'d UNCLASSIFIED, HYPOTHESIS ONLY that it may be M1-adjacent (T=72 saturated everywhere) but nothing corroborates that the way `busyr` corroborates its own R. Both bundled together as a single follow-up item needing an actual build-enabled session (the lever-F check Ken SA1 needs and the per-tick ledger trace Chun-Li SA1 needs are both no-build-session blockers) — grouped as a 2-member watch list, not a bucket, since the adv-coupling test above keeps them distinct shapes. Neither alone nor together reaches the ≥3 threshold. Trigger NOT tripped.

**Trigger re-check (supers wave S1, 2026-07-13), same discipline, full program ledger:** re-grouped honestly across all six new corpora against every existing §12.2.4 row and the EX-CONTACT-R/M1 registries above. Findings: the whiff-inclusive A-undercount pair (Ken SA1, Chun-Li SA1) tested and kept as 2 distinct one-off shapes (above); the Chun-Li SA3 BLOCK+HIT R-surplus is a 2-member internal-to-one-move shape, below threshold; the Ryu SA2 HIT-R surplus is a 1-member standalone shape, below threshold; the Ryu SA3 BLOCK-leg-unreachable finding is a quarantine (not a value divergence) and was independently domain-checked against oracle `ryu.json`'s own Denjin Hadouken row (`docs/arcade-frame-data/ryu.json:1906`) before integration — `Block_advantage="0"` is published as a NUMERIC value (not the non-numeric `"-"` this same row uses for `Hit`/`Hit_advantage`), which rules out re-filing the gap as "structurally nonexistent for an unblockable move": the oracle's own data model treats this move as blockable, so the numeric 0 (not the harness's always-HIT reading) is the anomaly needing the already-scheduled Ruling 4 2c arcade session. No group reaches the ≥3 threshold this wave. Trigger NOT tripped.

**M1 register — class growth (2026-07-13, supers wave S2):** the saturation class grows from 14 legs/7 moves to **24 legs across 11 moves**. Four new moves join this session, each with `T=72=FD_METER_LEN` exactly and `busyr` corroborating the oracle: **Akuma SA2 Messatsu Gou Shoryuu** (all 3 legs — WHIFF/BLOCK/HIT — `busyr`=37=oracle exactly on every leg), **Akuma SA3 Messatsu Gou Rasen (Ground)** (all 3 legs, `busyr`=51=oracle exactly on every leg), **Dudley SA1 Rocket Uppercut** (all 3 legs, `busyr`=31=oracle exactly on every leg — the second move in this program to show M1 holding on a WHIFF leg, after Ken SA1), and **Yun SA1 You Hou** (WHIFF leg only, `busyr`=36=oracle exactly; this move's own BLOCK/HIT legs are explicitly NOT M1 — see the new UNCLASSIFIED R-surplus note below, the same non-M1/M1 split already on record for Chun-Li SA3). Same single mechanism throughout (`frame_data_overlay.c:42`), cited per-leg with the established "omit R with the M1 citation" idiom in `corpus-akuma-sa2.yaml`/`corpus-akuma-sa3.yaml`/`corpus-dudley-sa1.yaml`/`corpus-yun-sa1.yaml`. The FD_METER_LEN-raise fix-cycle candidate (named, NOT STARTED) now has 24 members spanning EX and SUPER classes across 11 characters' moves — no design or build attempted this session.

**New UNCLASSIFIED sighting — Yun SA1 You Hou BLOCK+HIT R (2026-07-13, supers wave S2):** a legacy R-surplus (measured R=63 vs oracle 36) on both contact legs, with `busyr` (109/105) NOT recounting to oracle either — explicitly the SAME shape already on record for Chun-Li SA3's own BLOCK+HIT R (above, wave S1): a clean M1-fitting WHIFF leg coexisting with a busyr-disagreeing R-surplus on the two contact legs of the SAME move. Checked and matched, not assumed: both moves show WHIFF-clean-M1 + BLOCK/HIT-surplus-non-M1 on the identical field. This shape now has **2 members (moves)** — Chun-Li SA3, Yun SA1 — still below the ≥3 threshold (counting by distinct move, consistent with this document's own wave-6/wave-S1 counting convention). Omitted, flagged for follow-up alongside Chun-Li SA3's existing citation.

**legacy-R-surplus register — GRADUATED TO SYSTEMIC, 5-member census (register catch-up, 2026-07-13, EXSUPER-1 closing cleanup):** this register was never updated in this document as later waves crossed its own ≥3 threshold — corrected here. **Elena SA2 Brave Dance** (supers wave S4, `corpus-elena-sa2.yaml`'s own header) reproduces the identical shape a THIRD time (BLOCK R=29/HIT R=17 vs oracle 14, `busyr` agreeing with the diverging legacy value on each leg, not oracle) — CROSSING the ≥3-member systemic threshold, flagged for orchestrator attention in that corpus's own header at authorship time. **Q SA2 Deadly Double Combination** (`corpus-q-sa2.yaml`'s own header) is a FOURTH reproduction, with its own variant signature: WHIFF R=65=oracle exactly, but BOTH contact legs (BLOCK R=77/HIT R=73) carry `busyr=-1` (box_runs=0 — no att_box tensor at all once the capture fires), a STRONGER form of "non-recounting" than the prior members' own busyr-disagrees-but-exists shape. **Alex SA2 Boomerang Raid HIT-leg** (Part-A P-1 corrective, this same cleanup, `corpus-alex-sa2.yaml`'s own R DISPOSITION) is a FIFTH reproduction: WHIFF/BLOCK R=41=oracle exactly, HIT R=78 (busyr=87, also non-recounting). **Census, final: 5 members — Chun-Li SA3 Tensei Ranka, Yun SA1 You Hou, Elena SA2 Brave Dance, Q SA2 Deadly Double Combination (incl. its own `busyr=-1`-sentinel variant), Alex SA2 Boomerang Raid (HIT-leg only — its own WHIFF/BLOCK stay clean/oracle-exact).** Same per-member disposition throughout (R OMITTED, not xfailed, on every affected contact leg); no new mechanism invented or fix attempted (no build performed this cleanup either) — recorded here as a register catch-up so the systemic-threshold crossing is visible in this document, not only in the individual corpora's own headers.

**New UNCLASSIFIED sighting — Yun SA1 You Hou A, non-uniform across legs (2026-07-13, supers wave S2):** measured A=15 (WHIFF) / 9 (BLOCK) / 9 (HIT) vs oracle 16 — three different values on three legs, not a single reproduced magnitude. Checked against the whiff-inclusive uniform-(-1) A-undercount shape below (SA-WHIFF-A) and explicitly EXCLUDED: that shape's own defining test is a single constant magnitude reproduced identically on every leg including WHIFF, which this sighting fails (-1 on WHIFF, -7 on BLOCK/HIT). Also rules out the §13.11 declared-credit convention (needs the SAME value on every leg) and Phase-6 buckets 1/7/8 (need a clean WHIFF baseline, which this move's own WHIFF already diverges on). One member, standalone, a new shape distinct from every registered A-divergence class in this document. OMITTED all three legs, UNCLASSIFIED, flagged for follow-up.

**New one-off — Akuma SA3 Messatsu Gou Rasen (Ground) BLOCK adv off-by-1 (2026-07-13, supers wave S2):** measured adv=-75 vs oracle -76 on the SAME leg where S/A are both oracle-exact (S=3/A=22). The authoring report frames this as "the FIRST adv divergence seen anywhere in the program" — checked against the existing registry above, that framing does not hold at full-program scope: the "Block-adv anomaly, S/A/R exact" row (`remy-crroundhouse-block`, `remy-lov` ×3, wave 4/5) already registered adv-only divergences with S/A/R numerically exact — the identical shape description, just EX-class rather than SUPER-class. Correctly scoped, this is the first adv divergence among the **supers (SA)** corpora specifically — every prior multi-run super's BLOCK adv (Ken SA1/SA2/SA3, Chun-Li SA1/SA2/SA3, Ryu SA1/SA2/SA3) has been oracle-exact wherever numeric, until now. One member, standalone; does not match `remy-crroundhouse-block`'s own resolved ENGINE-9 two-contact defender-anchor mechanism (Akuma SA3 is single-contact) or `remy-lov`'s F10 travel-tick family (no projectile here). UNCLASSIFIED, xfail'd, flagged for follow-up; does not join SA-WHIFF-A (different field, no A involvement) or any other existing row.

**New one-off — Yun SA2 Sourai Rengeki BLOCK box_a/engine_a internal disagreement (2026-07-13, supers wave S2):** the BLOCK leg's own `box_a=14` disagrees with its own `engine_a=17` (the measured/candidate A value) on the SAME leg — checked against the "Contact-only A-overcount, box-backed" row above (`sean-exdragonsmash`, `chunli-sa2-block`) and explicitly REFUSED membership: that row's bar requires `box_a == engine_a` (box-backed agreement); here they disagree, the opposite signature. Also checked against Phase-6 bucket 8 (established members have `box_a` AGREE with the diverging legacy value) and refused on the same ground. R (25→5, `busyr` 32/56 not recounting to oracle) and adv (-27 vs oracle -18) diverge on the same two legs too, tracked as one combined OMITTED sighting per the authoring report, not split into separate rows. One member, standalone, a genuinely new internal-disagreement shape with no existing §12.2.4 precedent. UNCLASSIFIED, flagged for follow-up.

**SA-WHIFF-A ledger rebuild and trigger fire (2026-07-13, supers wave S2), rebuilt honestly rather than accepted from the per-character authoring reports' own informal groupings:** wave S1 (above) established the whiff-inclusive uniform-(-1) A-undercount signature and split it into two shapes by an adv-coupling test — shape A ("adv-exact": Ken SA1 Shoryureppa, BLOCK adv -38 oracle-exact despite the A undercount) and shape B ("adv co-shifts": Chun-Li SA1 Kikoshou, BLOCK adv +1 shift moving in lockstep with the A undercount). This wave's three whiff-inclusive sightings were each independently re-tested against that same discriminator, not taken from the authoring reports' own prose: **Dudley SA1 Rocket Uppercut** (A=39 vs oracle 40, -1, reproduced on WHIFF+BLOCK) has NO numeric adv field at all (dudley.json publishes non-numeric `Block_advantage`/`Hit_advantage` for all three SA1 legs) — shape B's own test requires an OBSERVED co-shift, impossible with no adv value to shift, so this places in shape A by the absence of any observed coupling, not by assumption. **Akuma SA2 Messatsu Gou Shoryuu** (A=23 vs oracle 24, -1, reproduced on WHIFF+BLOCK+HIT-juggle-check) — its own authoring report claims this "matches the Chun-Li SA1 Kikoshou precedent's own shape exactly" (i.e., shape B), but never actually runs wave S1's own adv-coupling test to justify that; running it here: BLOCK adv measures -25 against oracle -25, EXACT, not a co-shift — shape A's signature, not shape B's. **Corrected placement: Akuma SA2 joins shape A, contradicting its own authoring report's informal grouping.** **Yun SA1 You Hou** A fails the shared prerequisite for EITHER shape (a single constant magnitude on every leg including WHIFF — see the standalone one-off above) and joins neither. **Shape A (adv-exact) now has 3 members — Ken SA1 Shoryureppa, Dudley SA1 Rocket Uppercut, Akuma SA2 Messatsu Gou Shoryuu — crossing authoring-policy.md's own ≥3 same-shaped-quarantine threshold. TRIGGER FIRES.** Named **SA-WHIFF-A** ("whiff-inclusive uniform A-undercount, adv-isolated"). Recorded **NOT STARTED** — no lever-F toggle test run under this no-build wave (the same blocker as Ken SA1's own wave S1 escalation), bundled with the existing wave S1 2-member watch-list follow-up into one build-gated investigation covering all three (now effectively four, counting Chun-Li SA1's own shape-B item) moves once a build-enabled session is authorized. Shape B (adv co-shifts) stays at its wave S1 count of 1 member (Chun-Li SA1 only) — below threshold, does not fire.

**Trigger re-check (supers wave S2, 2026-07-13), same discipline, full program ledger:** re-grouped honestly across all nine new corpora against every existing §12.2.4 row, the EX-CONTACT-R/M1 registries, and wave S1's own two-shape split. Findings: shape A (adv-exact whiff-inclusive A-undercount) grows from 1 to 3 members (Ken SA1 + Dudley SA1 + Akuma SA2, the latter a corrected reclassification) — **THRESHOLD REACHED, TRIGGER FIRES, named SA-WHIFF-A** (see above); shape B (adv co-shifts) stays at 1 member (Chun-Li SA1), below threshold; Yun SA1's own A sighting joins neither shape and stays a standalone one-off; the Akuma SA3 BLOCK-adv-off-by-1 and Yun SA2 box_a/engine_a-disagreement sightings are each new, unrelated one-offs, 1 member apiece, below threshold; the Yun SA1 R BLOCK/HIT surplus joins the existing Chun-Li SA3 BLOCK+HIT R-surplus shape at 2 members (moves) total, still below threshold. SA-WHIFF-A is the only threshold-crossing this wave.

**M1 register — RESOLVED-BY-FIX (2026-07-13, BUFFER-1 capture-depth raise):** the FD_METER_LEN-raise fix-cycle candidate named above (24 legs / 11 moves, NOT STARTED) is now shipped. `frame_data_overlay.c:42`'s capture bound is split from the 72-cell display width: a new `FD_CAPTURE_LEN=256` constant now bounds `raw[]`/`meter_len`/`atk_cells`/`def_cells` (measurement), while `FD_METER_LEN=72` keeps its name, value, and every display use (meter geometry, `FdLatched` cell arrays, the draw-row clamp) — the on-screen meter is byte-identical at every capture depth (proven by the `FD_CAPTURE_LEN=72` mutation-contract identity run: 55/55 GREEN, zero drift). At 256, 21 of the 24 registered legs are clean oracle-equal conversions and ship `expect.R: from-qjson` measuring their pre-registered `busyr`/oracle value exactly: Akuma SA2 (37, ×3), Akuma SA3 (51, ×3), Chun-Li SA1 (48, ×3), Chun-Li SA2 HIT only (35), Chun-Li SA3 (46, WHIFF only — BLOCK/HIT are the separate non-M1 R-surplus shape, unconverted), Dudley MGB (EX) BLOCK (28), Dudley SA1 (31, ×3), Ken SA1 (45, ×3), Ken SA2 BLOCK/HIT (61, ×2 — WHIFF was already the wave-S1 control-negative). **Chun-Li SA2 BLOCK is the register's 22nd leg and does NOT convert**: measured post-fix R=38 matches this leg's own pre-registered `busyr` exactly but NOT the arcade oracle (`docs/arcade-frame-data/chunli.json` Houyoku Sen `Recovery`=35) — the busyr==oracle equality that holds on every other converted leg does not hold here, so R stays unasserted pending divergence classification, same disposition family as `yun-sa1` block/hit (`corpus-chunli-sa2.yaml`'s own dated note records this; the leg's pre-existing A-overcount `xfail` would otherwise silently absorb the mismatch, which the house rule against masked-by-an-unrelated-xfail wrong values forbids). **Ken SA3 (both legs) does NOT resolve** — measured post-fix BLOCK stays byte-identical at R=37 (zero drift at all) and HIT measures 11, neither matching the `excontact-r-findings.md` offline recount of 27 this row's own R DISPOSITION cited; the capture raise proves BLOCK's own append genuinely stops at the same point regardless of depth, meaning it was never actually FD_METER_LEN-capped the way the other 23 legs were — a genuine, still-open discrepancy between the prior offline recount and this fix's own direct measurement, reported rather than forced (`corpus-ken-sa3.yaml`'s own comments now record this). **A previously-untracked sibling member surfaced during this fix's own census/build-time diff and is folded into this closure**: `q-hsb-ex-whiff/-block/-hit` (Q's High Speed Barrage EX) shows the identical clean M1 signature (`busyr`=36=oracle on all three legs) and now also ships `R: from-qjson`, even though it was never carried in this register's leg count. **Side effect, reported prominently, not silently absorbed:** raising the capture depth also un-caps every `ended_by_partner_release` (command-throw/command-grab) move's raw[] capture, which previously saturated at the SAME 72-cell bound independently of the M1 mechanism above. This affects Hugo's six grab HIT rows (the `(FD_METER_LEN-1)-S-A` derived-convention formula §13.10 documents and — in that same subsection — explicitly REJECTS resizing FD_METER_LEN for: "it would only trade one convention's R for another with no arcade oracle either way"), plus `ken-throw-hit`, `makoto-karakusa-lk-hit`, `oro-throw-hit`, `twelve-throw-hit`, `yang-throw-hit`, `yun-throw-hit`, and `alex-powerbomb-lp-hit`/`-unblockable-probe` — all previously asserting a stale capture-bound convention R, all now DE-ASSERTED (not re-derived to a new formula, since no arcade oracle exists for the post-contact span either way) and re-measured to the true, larger post-release span. **This reopens §13.10's "No FD_METER_LEN resize" decision** — it was rejected FOR Hugo specifically at the time, and this fix (built for an unrelated reason, M1) undoes it as an unavoidable side effect. Also affected, with no oracle assertion at risk: `dudley-throw-hit`, `urien-throw-hit`, `akuma-ashura-whiff`/`dudley-crosscounter-lp-whiff` (no-active-signal WHIFF rows whose S now reads the true, longer `raw_len` instead of the old 72-cell cap), and `elena-exspinscythe-block` (a previously oracle-exact, unrelated row whose R was coincidentally correct at the old cap and now measures a genuine divergence — R stays asserted at the oracle value, converted from a plain PASS to an `xfail` per the house idiom, not de-asserted; see this section's own "BUFFER-1 capture-depth-raise regression" row above). `elena-exlynxtail-*` (the M1 register's own long-standing WHIFF/T=72 control-negative, `:947`) also drifts (0/7/7 → 25/25/25 across WHIFF/BLOCK/HIT) — its own capture window was ALSO extended by the raise, refining the "T=72 alone is not truncation" general rule further: that rule predicts whether the OLD, capped R was already correct, not whether raising the cap changes it. Full accounting: `<sp>/bufferfix/census.tsv` (pre-fix census), `<sp>/bufferfix/pre-drift-256.log` (post-fix full diff, the authoritative before/after list). Gate record: `FD_CAPTURE_LEN=72` identity run 55/55 GREEN zero drift; `FD_CAPTURE_LEN=256` pre-drift run matched the predicted table exactly on the 21 clean oracle-equal legs, the chunli-sa2-block busyr-only (non-oracle-equal, unasserted) leg, the Ken SA3 exception, and every side-effect row above; scoped `--update-golden` on the 25 affected corpora; final full `--check-golden` 55/55 GREEN zero drift; determinism ×2 (`ken-sa1`, byte-identical); eight house levers (F/G/H/I/J/M/N/O) re-grep-asserted `=1`; `git diff --stat src/` shows exactly `frame_data_overlay.c`.

**SA-WHIFF-A — D2/D3 diagnosis outcome (2026-07-13, BUFFER-1 build window), Q2-D2 evidence-only per the bundled plan's own sequencing — NO conversion ships this cycle.** The offline declared-credit ledger walk (`<sp>/bufferfix/q2-ledger-report.md`, `<sp>/bufferfix/ledger_walk.py`) pre-registered D1 (ledger==displayed, dissolves the trigger with zero code change) as the expected outcome; the actual result is **MIXED, NOT D1**: Ken SA1 and Akuma SA2 both give **D2** (ledger total == oracle exactly — 34 and 24 respectively — NOT the displayed 33/23), and Dudley SA1 is **D3/inconclusive** (window-dependent: the naive `[move_start,atk_idle_F]` window gives 32, matching neither number; extending through the point `jatix` permanently stops gives 40 = oracle, but reproducing the EXACT snapshot tick requires modeling the §13.5.1 `cghi=1`-dwell commit state machine, not just the accumulator rules, which is out of the ledger walk's scope). Per the plan's own STOP rule for mixed verdicts across an adv-coupling-grouped trio, **no §2.4 conversion edits were applied this cycle** — `corpus-ken-sa1.yaml`/`corpus-dudley-sa1.yaml`/`corpus-akuma-sa2.yaml` A-fields are untouched. **This build window's own Q2-D2 diagnosis (env-gated `fprintf` probe in the `fd_engine_active_count` accumulator, `charset.c:454-561`, scratch-reverted, `git diff` on `charset.c` confirmed zero after revert) goes further and answers WHY the ledger walk's own hand-derivation (34/24) doesn't match live execution (33/23) — a finding the ledger walk itself couldn't produce, since it read only the static per-tick trace columns, not the live accumulator:** directly instrumenting every ADD/REVOKE/RESTORE site and the `engine_a_at_atk_idle` snapshot site, then running all three members' WHIFF legs, shows the LIVE accumulator's own running total already lands on the DISPLAYED value (33/39/23), not the ledger walk's hand-summed total (34/32/24) — the display is faithful to the accumulator; the accumulator itself, not the display, is what falls short of the oracle. The mechanism, reproduced identically on all three members: `fd_reset_move()`'s `fd_engine_active_count[i] = 0` MOVE_START reset (`frame_data_overlay.c:1214`) fires one game-tick AFTER `char_move()`/`charset.c`'s own engine-tick ADD has already credited the move's first active cell (the overlay's own tick runs after the engine tick within the same real frame, `main.c:603`/`:615`, but MOVE_START detection can lag the engine's first active-cgix add by a further whole tick when the r1 edge and the first `jatix!=0` cell don't coincide) — the first cell's credit is silently zeroed, and (on Akuma/Dudley, whose first cell persists ≥2 ticks) the SAME cell then gets mis-re-entered as if newly seen (since `fd_prev_active_cgix` was also reset to -1), contributing only its own decremented per-frame remainder instead of its true full clamped value. Net effect on all three members: exactly -1 relative to a reset-race-free implementation, matching the -1 vs oracle exactly. **Lever-F=0 toggle runs (env-gated, same probe build) on all three members show ZERO effect** (byte-identical A/R to lever-F=1) — confirming the ledger walk's own "0 same-tick revoke events" finding and definitively RULING OUT the revoke+restore mechanism as the cause; this is a different, previously-unnamed mechanism, not a "restore partially works" story. **Recorded as an ENGINE-candidate finding, strengthened-bar note (per §13.17's own precedent for RE-ANCHOR-1's ownership-fitted-rule census discipline): own recorded signature required (three independent members, identical mechanism) before it graduates past candidate status.** The env-gated `fprintf` probe's own per-tick ADD/REVOKE/RESTORE instrumentation output was NOT preserved anywhere (scratch-only, reverted per house rules, `git diff` on `charset.c` confirmed empty) and is not itself citable evidence. What IS preserved and artifact-backed: the lever-F=0 toggle runs (`<sp>/bufferfix/d2probe/{ken-sa1,dudley-sa1,akuma-sa2}-{normal,off}/trace.log`) show byte-identical WHIFF-leg A across lever-F on vs off on all three members (lever-F null, corroborating the ledger walk's own 0-revoke-events finding); those same preserved FINALs show `engine_a`=33 vs `box_a`=34 on Ken SA1's own WHIFF leg (F=150) — a real, artifact-backed 1-tick gap between the displayed accumulator and the raw box-frame tally; and the code-order facts that `njUserMain()` (`main.c:604`) runs before `frame_data_overlay_tick()` (`main.c:616`) each frame, with `fd_reset_move()`'s `fd_engine_active_count[i] = 0` reset sitting at `frame_data_overlay.c:1213-1216`. Framed honestly, the `fd_reset_move()`-vs-`char_move()` tick-ordering race described above is the LEADING HYPOTHESIS consistent with those three artifacts — not a logged signature of the mechanism's own operation, since no per-tick capture of the actual reset-vs-add ordering was preserved. Scope risk flagged explicitly: the reset-timing race is structural to `fd_reset_move()`/`char_move()`'s interaction on EVERY move using this code path, not just these three — it is only VISIBLE here because these three moves' whiff-path A happens to disagree with an independently-known oracle by exactly 1; whether it silently affects other currently-PASS or already-xfailed A readings elsewhere in the suite is unknown and NOT investigated this cycle (out of this window's Q2-D2/evidence-only scope). No fix ships. Ken SA1's own wave-S1 ESCALATED flag (`:951`, "flagged for orchestrator/user follow-up needing a lever-F toggle build") resolves to THIS finding: the lever-F toggle build ran, and it found the answer is NOT lever F.

**SA-WHIFF-A register — member count update to 5, plus Part-A P-1 corrective resolutions (2026-07-13, EXSUPER-1 closing cleanup).** This document's own SA-WHIFF-A count was never updated past the wave-S2 trigger-fire's 3 members (Ken SA1 Shoryureppa, Dudley SA1 Rocket Uppercut, Akuma SA2 Messatsu Gou Shoryuu) even as later waves added members — corrected here. **Elena SA1** (supers wave S4, `corpus-elena-sa1.yaml`'s own header) joined as a 4th member (A=16 vs oracle 17, uniform -1 on all three legs, BLOCK adv=-30 oracle-exact — shape A, adv-exact). **Alex SA2 Boomerang Raid** (this same cleanup's Part-A P-1 corrective, `corpus-alex-sa2.yaml`) is now a confirmed 5th member: A=13 vs oracle 14, uniform -1 on all three legs, no numeric adv field to test coupling (places in shape A by absence of observed coupling, same posture as Dudley SA1). **Census, final: 5 members — Ken SA1 Shoryureppa, Dudley SA1 Rocket Uppercut, Akuma SA2 Messatsu Gou Shoryuu, Elena SA1, Alex SA2 Boomerang Raid** (all shape A, adv-exact/no-coupling-observable; shape B stays at its 1-member count, Chun-Li SA1 Kikoshou, below threshold). **Part-A ledger-walk resolutions (this cleanup):** an isolated single-entry offline declared-credit ledger walk (mirroring the Q2/D2 method above) was run on Alex SA2 (all 3 legs) and Ibuki SA2 (WHIFF leg): both give ledger total == oracle exactly (14 and 15 respectively, NOT the displayed 13/14) — the SAME D2 signature already confirmed on Ken SA1/Akuma SA2, now with 2 more members' worth of corroborating evidence for the `fd_reset_move()` tick-ordering race. Both are now shipped `xfail-at-oracle` (the `corpus-akuma-sa2.yaml` idiom: oracle value asserted via `from-qjson`, measured value recorded in the xfail note) rather than the plain-PASS assert `corpus-alex-sa2.yaml` previously shipped — house rule (wrong values never ship) required the correction regardless of this being the SAME mechanism already flagged as an ENGINE-candidate. **Ibuki SA2 is NOT a SA-WHIFF-A member** despite sharing the identical D2 mechanism: SA-WHIFF-A's own defining test requires a uniform magnitude reproduced on EVERY leg including WHIFF (`:974`'s own test), and Ibuki SA2's divergence is WHIFF-only (BLOCK/HIT both measure A=15==oracle exactly) — it fails the uniformity prerequisite the same way Yun SA1's own non-uniform A sighting did (`:968`), so it is cited to SA-WHIFF-A/D2 as its MECHANISM but stays outside the named shape's own member count. **Remy SA2 Supreme Rising Rage Flash is a DIFFERENT disposition entirely, NOT SA-WHIFF-A and not a census addition**: its own ledger walk (both contact legs) gives 10, matching NEITHER the displayed 15 NOR the oracle 16 — a hitstop-entangled shape the pure charset.c accumulator model cannot reproduce (see `corpus-remy-sa2.yaml`'s own corrected A DISPOSITION), OMITTED per the ledger-outcome disposition's third bucket, not xfailed. **Flagged for re-audit, NOT corrected this cycle (out of Part-A's named 3-corpus scope): `corpus-ryu-sa2.yaml`'s own A=12 plain-PASS assert** (both BLOCK and WHIFF legs, vs arcade/oracle 13) is asserted under the identical §13.11 "path-independence confirms declared truth" reasoning that Remy SA2's own retracted A DISPOSITION used — and that reasoning is now known to be insufficient (the D2 race is itself path-independent, so path-independence alone cannot discriminate a genuine declared-credit truth from a race artifact). Ryu SA2 was NOT re-walked this cycle (out of the 3 named P-1 corpora); this is a scope-risk flag only, not a finding — a future ledger walk on Ryu SA2 would be needed to settle whether it is a 6th SA-WHIFF-A/D2 member, a Remy-SA2-style ledger-neither case, or a genuine §13.11 declared-truth assert.

**SA-WHIFF-A/D2 — RESOLVED (ENGINE-D2, lever S, 2026-07-13).** The `fd_reset_move()`-vs-`char_move()` same-real-frame tick-ordering race diagnosed at `:980` (LEADING HYPOTHESIS at the time, not yet a confirmed fix) is now shipped as a fix: `fd_movestart_same_tick_credit_hold` (lever S, `frame_data_overlay.c`, declared next to levers N/O), gated on `fd_prev_active_cgix_tick[i] == Game_timer` at the MOVE_START reset site. When the engine already entered the new move's first active/catch cell on the SAME real frame the overlay's MOVE_START scan fires (freeze-deferred MOVE_START — `sa_stop_check()` parks the overlay tick while `char_move()` keeps running, so on charts whose first post-flash cell is active, the add lands before the reset), the reset now preserves exactly that cell's contribution (`fd_engine_active_count[i] = fd_prev_active_cgix_add[i]`) instead of zeroing it, while keeping the `fd_prev_active_cgix*` bookkeeping intact so the same cell cannot be double-credited on the next tick. Lever S at 0 restores today's code byte-identically (identity-gate verified).

**Discriminating condition, confirmed by a per-tick reset-site probe this session (not merely re-derived from the trace scan):** a window loses first-cell credit iff the attacker's `jatix`/`jcaix` is already nonzero on the MOVE_START tick's own data row — i.e. the engine enters an active/catch cell the SAME real frame the overlay's MOVE_START reset fires. The probe also surfaced a variant the offline trace scan is structurally blind to: a **same-tick MID-TICK cgix transit** (the engine can advance `cg_ix` through an active cell and past it within one `Game_timer` tick — `check_cm_extended_code` — leaving no end-of-tick trace-row evidence) affects `ryu-sa2-block`/`-hit` and (probe-only, zero real effect) `elena-sa2-hit`/`makoto-sa1-block`. Full-suite reset-site probe census: **44 same-tick windows total, ALL super-freeze SA windows, across 16 member corpora** — zero non-super fire, zero normals/EX fire, the complete affected population.

**Member census, final (16 corpora, 44 legs):** ken-sa1 (×3, A OMITTED→`from-qjson`), dudley-sa1 (whiff/block A OMITTED→`from-qjson`; hit stays OMITTED, a SEPARATE still-open -1 residual not folded into this closure), dudley-sa3 (block/hit A xfail→PASS; whiff golden-unchanged, lever-O box path), akuma-sa2 (×3, A xfail→PASS), elena-sa1 (×3, A OMITTED→`from-qjson`), elena-sa2 (negative-confirm, delta=0 on all three legs — sentinel-cell mid-tick entry never re-entered, golden unchanged), alex-sa2 (×3, A xfail→PASS, R co-moves unasserted on hit), ibuki-sa2 (whiff A xfail→PASS; **chiblast-block/hit PASS→XFAIL**, joining the registered "contact-only A-overcount, box-backed" class — post-fix `engine_a==box_a==16`, the pre-fix oracle-exact reading was two canceling bugs, not a genuine agreement; ORCHESTRATOR SIGN-OFF GRANTED), ryu-sa2 (**all three legs are D2 members** — whiff A xfail-retracted §13.11→`from-qjson`; block is the NEW mid-tick-transit amendment, its own long-standing §13.11 "declared-truth" plain-PASS `A: 12` was ITSELF a race artifact, retracted and converted to `from-qjson`; hit value-only refresh, stays unasserted, still fails its own juggle check post-fix), chunli-sa1 (whiff/hit A xfail→PASS; block's combined A+adv xfail splits — A resolved, adv clause remains open, reworded), yun-sa1 (whiff A OMITTED→`from-qjson`; block/hit stay OMITTED, a SEPARATE, still much larger residual -6 gap, unaffected by this closure), remy-sa2 (block/hit A OMITTED→`from-qjson`; the prior "ledger-neither (10)" record is corrected below — it was the ledger WALK that was wrong, not a genuine third value), sean-sa2 (×3, value-only refresh, stays OMITTED — 30 still ≠ oracle 36), urien-sa1 (block: A resolves to oracle-exact but R does NOT co-move, stays xfailed on the R clause alone, its sum-conservation narrative reworded below; **hit: XFAIL→PASS, the 12th conversion**, one more than this fix's own provisional "up to 11"), yang-sa2 (×2, value-only refresh, stays OMITTED — still nowhere near oracle 59), makoto-sa1 (newly-discovered member this closure — hit value-only refresh stays PASS/unasserted; whiff golden-unchanged, lever-O box path; block probe-only mid-tick, delta=0, golden unchanged). **Suite-wide: 12 XFAIL→PASS, 2 PASS→XFAIL, net +9/-9** (akuma-sa2 ×3, alex-sa2 ×3, ibuki-sa2-whiff, chunli-sa1-whiff/hit, dudley-sa3-block/hit, urien-sa1-hit XFAIL→PASS; ibuki-sa2-chiblast-block/hit PASS→XFAIL) — shipped baseline 1,256 PASS / 91 XFAIL → **1,266 PASS / 81 XFAIL**.

**remy-sa2's own "ledger-neither (10)" walk — CORRECTED.** The `:982` note above records an offline declared-credit ledger walk giving total=10 on both remy-sa2 contact legs, matching neither the then-displayed 15 nor the oracle 16, disposed as OMITTED pending a hitstop-aware ledger model. That walk's own output was itself WRONG, not a genuine third value: `fd_movestart_same_tick_credit_hold`'s mechanism can only ZERO or PRESERVE a cell's credit, never inflate it, so the true engine ledger total can never be strictly BELOW the pre-fix displayed value — a ledger total (10) less than the displayed value (15) is arithmetically impossible for a credit-DROPPING race, proving the walk's own naive charset.c cg_ctr-snapshot model diverged (it under-credits hitstop-frozen cells, this move being heavily hitstop-entangled — 40/50 jatix-live ticks carry nonzero hitstop on both legs), not the accumulator math it was trying to measure. The correct comparison is the mechanical ENGINE-D2 delta (+1) applied to the pre-fix displayed value: 15+1=16 == oracle exactly — and this is exactly what the actual fix measures. `corpus-remy-sa2.yaml`'s own A DISPOSITION now records this correction; A ships `from-qjson` on both legs.

**urien-sa1-block's own sum-conservation narrative — BROKEN post-fix, reworded.** The `:69` A/R DISPOSITION registered urien-sa1-block as a "sum-preserving A/R boundary shift" (S+A+R conserved at 78 on both measured and oracle sides). Post-ENGINE-D2, this leg's own A resolves to oracle-exact (55) via the same-tick race fix, while R (unaffected by lever S) stays at its pre-fix measured value (24, oracle 23) — so measured S+A+R is now 0+55+24=79, NOT 78, breaking the conservation identity. This means the pre-fix "conservation" was a coincidence of two independent, unrelated divergences (the D2 race's own -1 on A, and this leg's own +1 on R from an unrelated cause) summing to a conserved total — not evidence of one single sum-preserving boundary-shift mechanism as originally framed. `corpus-urien-sa1.yaml`'s own xfail note is reworded to the evidenced post-fix state (R-only, A resolved).

**NEW ROW — travel-distance-dependent-A class, GRADUATED (register catch-up, 2026-07-13, EXSUPER-1 closing cleanup).** A recurring shape across the projectile-super/EX census was never given its own named row despite crossing the ≥3 threshold multiple waves ago: a projectile's own contact-leg `proj_a` (displayed A on the proj-split path) scales with how far the projectile has traveled before intercepting the target, so no single "contact A" value exists — it grows/shrinks continuously with distance rather than sitting at one fixed contact-vs-whiff differential. **Census, 5 members: Ryu SA3 Denjin Hadouken** (the anchor precedent, 2b/wave-S1: `proj_a` 3 at close, 16 at far, `corpus-ryu-sa3.yaml:63-64`); **Ibuki SA1 Kasumi Suzaku** (`corpus-ibuki-sa1.yaml:58-65`, contact-leg A/adv OMITTED, "established class, not a fresh anomaly... proj_a scales with travel: 3 close, 16 far" citing the Ryu SA3 precedent directly); **Oro SA2 Yagyou Dama, both variants** (`corpus-oro-sa2.yaml:29-48` base: A=11 close / A=28 at dist:150, vs clean WHIFF A=21 — three distinct values at three distances, OMITTED; `:79-84` its own 2-stock "EX Yagyou Dama" variant: WHIFF A=54 vs oracle 52, contact A=111 at dist:250 — "same travel-dependent-inflation shape"); **Urien SA2 Metallic Sphere** (`corpus-urien-sa2.yaml:78-125`: A=11 close vs oracle 75, A=85/83 at far/max-ceiling — "same class of finding as Ryu SA3 Denjin Hadouken's own proj_a-distance-scaling observation"); **Remy SA1 barrage** (`corpus-remy-sa1.yaml:89-185`: proj_a 20/20/74/127 across four measured configurations, task's own header pre-registering "travel-distance A/R likely"). All 5 members OMITTED (not xfailed) — no single value is a stable move property to assert against, and none has been confirmed as a single unified mechanism across characters (each corpus's own UNCLASSIFIED framing is honest: "same CLASS of finding," not "same confirmed mechanism"). **Sean SA1 Hadou Burst — VERIFIED and EXCLUDED from this census, with a note.** `corpus-sean-sa1.yaml:63-99` independently observes the identical MECHANISM (a distance sweep shows `proj_a` reading 3/9/16 across dist:close/far/300, shrinking/growing in lockstep with R and adv as travel distance changes) — but this move's own shipped `xfail` is R-SHAPED, not A-shaped (the "Proj-split late-despawn contact R/adv collapse" class, `~:935`/`oro-exsundisklow` — measured legacy R=59 vs oracle 61, R is the asserted-then-xfailed field). Critically, **A is never asserted at all for this row**: `sean.json`'s own Hadou Burst `Hit` field (the oracle A value) is the non-numeric literal `"-"` (verified directly in the oracle file this cleanup), so there is no oracle A value for a `proj_a`-vs-oracle divergence to exist against in the first place — Sean SA1 cannot be an A-DIVERGENCE class member because it has no A comparison to diverge on. Excluded from the 5-member census on that basis; noted here as a related MECHANISM sighting (the underlying "proj_a scales with travel distance" behavior is present and consistent with the other 5 members) rather than a 6th census member.

**MULTIHIT basis-mismatch A — recount to 3, WATCH ENTRY fires (register catch-up, 2026-07-13, EXSUPER-1 closing cleanup).** This class's own member count was never carried forward as a running tally in this document. **Census, 3 members: Akuma SA1** Ashura Senku / Messatsu Gou Hadou-family multihit basis mismatch (first sighting), **Sean SA2** (second sighting), **Twelve SA1** X.N.D.L. (third sighting — `corpus-twelve-sa1.yaml`'s own R DISPOSITION cites a basis-mismatch shape for this move's declared-credit accounting). At exactly 3, authoring-policy.md's own "≥3 same-shaped anomalies ⇒ stop and flag systemic" counter FIRES. Per that policy's own handling for a class whose members are NOT uniform in mechanism detail (unlike the single-shape legacy-R-surplus/SA-WHIFF-A rows above, this class's 3 members share only the high-level "multihit basis mismatch" framing, not a single proven arithmetic identity across all three) — this is recorded as a **named WATCH ENTRY**, not escalated to a full systemic investigation this cycle: no per-tick ledger session was run to confirm a single unifying mechanism across the three (out of this cleanup's own scope — no `src/`/tool changes), and no corpus values are altered by this recount. Flagged for a future ledger-walk session to determine whether these 3 sightings share one mechanism (promoting to a systemic register row) or are 3 independent one-offs that merely resemble each other at the framing level.

**Flash-overlap watch entry — count=2, plus the makoto-sa2-whiff R triple-agreement exception (register catch-up, 2026-07-13, EXSUPER-1 closing cleanup).** The sastop/flash-overlap gate (authoring-policy.md Ruling 2 item 2) has recorded exactly 2 watch-list occurrences across the supers program: **Makoto SA2 Abare Tosanami, ALL LEGS** (nonzero sastop ticks inside the entry's own [MOVE_START F, FINAL F] window on WHIFF/BLOCK/HIT — the affected A/R/adv fields inadmissible per the gate's own rule, S remains assertable) and **Ibuki SA3 HIT** (same gate, HIT leg only). Both stay below the ≥3 systemic-investigation threshold — recorded as a named watch entry per the gate's own "logged toward the systemic-pattern counter" instruction, not escalated. **Exception, same move, different field: `makoto-sa2-whiff`'s own R clause is NOT gated** despite the whiff leg's own flash-overlap flag — a triple-agreement (measured R, `busyr`, and oracle R all agree exactly on the WHIFF leg specifically) means the flash-overlap disqualification is moot for that one field on that one leg: the gate exists to catch cases where the overlay's flash-skip behavior might have silently corrupted a field, and a three-way agreement is direct, independent evidence that no corruption occurred for R on this leg, overriding the gate's own default-inadmissible presumption for that field only (A/adv on the WHIFF leg, and all fields on BLOCK/HIT, remain inadmissible per the general rule).

**Late-despawn row — 2nd member, Sean SA1 (register catch-up, 2026-07-13, EXSUPER-1 closing cleanup).** The "Proj-split late-despawn contact R/adv collapse" row (`~:935`, `oro-exsundisklow-block/-hit`, EX-CONTACT-R disposition M3) gains its 2nd member and first super-class sighting: **Sean SA1 Hadou Burst** (`corpus-sean-sa1.yaml:63-99`, both contact legs) reproduces the identical mechanism — a projectile that despawns naturally on contact (`natend=1`) rather than being cut short by the hit, with R measuring the post-despawn attacker-busy remainder rather than the oracle's own Recovery value (measured legacy R=59 vs oracle 61, rep-stable across 3 sampled distances at the near-range plateau, where adv IS oracle-exact — confirming the calibration distance, isolating the divergence to R alone). Unlike `oro-exsundisklow`, Sean SA1 has no WHIFF leg to fall back on for a clean R read (Hadouken-family projectile WHIFF-unreachable-by-design), so R is asserted at the oracle value via `from-qjson` with the measured 59 recorded in the xfail note (the `corpus-akuma-sa2.yaml` idiom, generalized from A to R). Still below the ≥3 threshold at 2 members; flagged for follow-up.

Oracle-side (not engine classes): `ibuki-kubiori` two-phase
incomparability, Sean Dragon Smash Strong suspected oracle duplicate
row, Oro Oniyama negative-oracle-R incomparability.

**Errata (2026-07-10):** external-source verification of this bucket is
recorded in `docs/arcade-frame-data/ERRATA.md` — it covers the closed
7-entry bucket (Akuma Ashura Senkuu, Oro Oniyama, Ibuki Kubiori: 3
STRUCTURAL) plus the open Sean Dragon Smash Strong question (1
UNVERIFIABLE, still open, no side picked).

**MASK-1 (2026-07-11):** the 7 STRUCTURAL-verdict entries above (minus
`ibuki-kubiori-lp-whiff`/`-block`, an item-18(b) value divergence, never
masked) plus the new Sean Roll ×3 (ERRATA §5, same zero-active class as
Akuma) now FIELD-MASK their incomparable field(s) and assert
comparable fields only, plain `PASS` — see §13.15 for the normative
mechanism, member table, and exclusions. Sean Dragon Smash Strong
(UNVERIFIABLE, a value dispute) is explicitly excluded from masking and
stays XFAIL, unchanged.

**DATED RIDER (2026-07-11, CAPTURE-1, append-only):** both paragraphs
above predate the arcade ground-truth capture and are now stale on this
point. The "open Sean Dragon Smash Strong question... still open, no
side picked" framing (Errata paragraph) and the "explicitly excluded
from masking and stays XFAIL, unchanged" framing (MASK-1 paragraph) no
longer describe the current state: the question is CLOSED. Arcade
plays S=7/A=8/R=39 on the dp+MP whiff, matching this engine, confirming
`sean.json`'s declared Strong row is a data-entry duplicate of Jab's
row (`docs/arcade-frame-data/CAPTURE.md`; `docs/arcade-frame-data/
ERRATA.md` §4 verdict upgraded UNVERIFIABLE → ORACLE-WRONG-CONFIRMED).
The three `sean-dragonsmash-mp-*` entries converted to explicit-literal
PASS and are no longer XFAIL. This remains outside MASK-1's own scope
(the dispute was a value question, not a structural incomparability, so
it was never a masking candidate) — but "stays XFAIL, unchanged" is no
longer accurate. Both paragraphs above are preserved verbatim as the
historical record of this bucket's status as of 2026-07-10.

### Q standing normals (whiff and block)

| Move | q.json line | Trace S/A/R | Arcade S/A/R | Status |
| --- | ---: | --- | --- | --- |
| Far Jab | 37–71 | 6/4/4 | 6/4/4 | ✓ |
| Jab (close LP) | 2–36 | 4/4/6 | 4/4/6 | ✓ |
| Strong (MP) | 72–106 | 12/5/19 | 12/5/19 | ✓ |
| Fierce (close HP) | 107–141 | 18/16/23 | 18/16/23 | ✓ (was A+1 on stale trace; resolved 2026-05-04, see §13.4). Knockdown-on-hit ("D" in q.json) renders KD, §13.6.2 SHIPPED. |
| Back+Fierce | 773–805 | 13/11/15 | 13/11/15 | ✓ (was A+1 on stale trace; resolved 2026-05-04, see §13.4) |
| Short (close LK) | 387–421 | 4/4/8 | 4/4/8 | ✓ |
| Forward (close MK) | 422–456 | 7/2/18 | 7/2/18 | ✓ |
| Far Forward (far MK) | 457–491 | 8/5/14 | 8/5/14 | ✓ |
| Roundhouse (close HK) | 492–526 | 20/5/28 | 20/5/28 | ✓ |
| Back+Roundhouse | 807–841 | 11/7/35 | 11/7/35 | ✓. Knockdown-on-hit ("D" in q.json) renders KD, §13.6.2 SHIPPED. |

### Q crouching normals

| Move | q.json line | Trace S/A/R | Arcade S/A/R | Status |
| --- | ---: | --- | --- | --- |
| Crouching Short (cr.LK) | 527–561 | 6/5/8 | 6/5/8 | ✓ S/A/R |
| Crouching Forward (cr.MK) | 562–596 | 7/3/14 | 7/3/14 | ✓ S/A/R |
| Crouching Roundhouse (cr.HK) | 597–631 | 12/8/33 | 12/8/33 | ✓ S/A/R. Knockdown-on-hit ("D" in q.json) renders KD, §13.6.2 SHIPPED (was green "+N" — the primary Phase 4 item 3 reproducer). |

Block adv on cr.MK / cr.HK / cr.HP matches arcade exactly under the
Phase 3 harness (pinned RNG, controlled spacing) — the previously
documented **+2 high** discrepancy did NOT reproduce; see §13.2 for the
resolved-by-rebaseline finding. cr.LK BLOCK does not classify at all
under the harness (finalizes WHIFF instead) — a separate, new finding,
also in §13.2.

### Q specials sampled

| Move | q.json line | Trace S/A/R | Arcade S/A/R | Status |
| --- | ---: | --- | --- | --- |
| Dashing Leg Attack (Short) | 1192–1226 | 25/2/29 | 25/2/29 | ✓ |
| Dashing Leg Attack (Forward) | 1227–1261 | 28/2/30 | 28/2/30 | ✓ |
| Dashing Leg Attack (RH) | 1262–1296 | 31/2/31 | 31/2/31 | ✓ |
| Dashing Head Attack (Jab, long) | 1087–1121 | 24/3/23 | 24/3/23 | ✓ |
| Dashing Head Attack (Strong, long) | 1122–1156 | 28/3/24 | 28/3/24 | ✓ |
| Dashing Head Attack (Fierce, long) | 1157–1191 | 32/3/25 | 32/3/25 | ✓ |
| Universal Overhead (single isolated) | 912–946 | 15/10/5 | 15/10/5 | **Phase 3 harness confirms FIXED (2026-07-07)** — `a386e057`'s jatix-gated same-tick cgix-transit revoke (`charset.c:432-451`) verified working: A=10 exact match, both S=15 (same-frame press) and S=16 (1-frame-apart press, `q-uoh-1f-block`) variants. Mutation test confirms (disabling the revoke flips exactly these entries to FAIL A=11). §13.7.4 residual is resolved. **UPDATE (2026-07-07, §13.11):** the "A=10 exact match" columns above are pre-convention history — displayed A is now 11 by the declared-truth convention (§13.11); S/R unchanged. |
| Universal Overhead (chain / spam) | 912–946 | S=15 A=10 R=5 (at wait=38f retrigger) | 15/10/5 | §13.7.1 / §13.9.4 — **FIXED (2026-07-07), plain PASS.** The gated anchor-time `engine_a` snapshot (§13.9.4) selects tap-1's total (A=10) when the cut committed AND the cghi=1 dwell was interrupted by the retrigger. §13.7.5's original raw[]-derived-A recommendation was tested and **falsified** during this fix's diagnosis (active_pf=13, not 10 — see §13.7.5). Mutation tests confirm both the jatix-revoke lever (now 3 entries, all A=11) and the new gate's own lever (forcing it off reproduces A=20). Accepted residual: a retrigger landing before any cghi=1 dwell forms stays inflated (gate never arms) — out of scope, no worse than before. **UPDATE (2026-07-07, §13.11):** the "A=10" figures above are pre-convention history — the anchor snapshot now inherits the §13.11 restore, so displayed A is 11; mutation lever A's flag set is now ∅ (superseded), and lever B's FAIL value is now A=22 (was 20) — see §13.9.4's own update note. |
| **High Speed Barrage (Jab)** | 1332–1366 | 9/3/26 (WHIFF/BLOCK), R=21 (HIT) | 9/3/26 | §13.5.2 ship — WHIFF/BLOCK exact ✓. HIT-R convention per §13.10, plain PASS asserting measured value (R=21, `q-hsb-jab-hit`, Class 1 hit-branch recovery chain). |
| **High Speed Barrage (Strong)** | 1367–1401 | 8/4/28 (WHIFF/BLOCK), R=21 (HIT) | 8/4/28 | §13.5.2 ship — WHIFF/BLOCK exact ✓. HIT-R convention per §13.10, plain PASS asserting measured value (R=21, `q-hsb-strong-hit`, Class 1). |
| **High Speed Barrage (Fierce)** | 1402–1436 | 9/3/28 (WHIFF/BLOCK), R=21 (HIT) | 9/3/28 | §13.5.2 ship — WHIFF/BLOCK exact ✓. HIT-R convention per §13.10, plain PASS asserting measured value (R=21, `q-hsb-fierce-hit`, Class 1). |
| **Capture and Deadly Blow LK** | (q.json CnDB rows) | 12/2/42 | 12/2/42 | §13.6 / §13.6.1 ship. **R+1 FIXED 2026-07-07** (Phase 4 items 4+5) — R re-derived from engine credit on the partner-release path only; exact arcade match, `xfail` removed, plain PASS. |
| **Capture and Deadly Blow MK** | | 13/2/44 | 13/2/44 | R+1 FIXED (Phase 4), exact match, plain PASS |
| **Capture and Deadly Blow HK** | | 14/2/46 | 14/2/46 | R+1 FIXED (Phase 4), exact match, plain PASS |
| Dashing Leg Attack (RH), BLOCK | 1262–1296 | 31/2/31, adv −15 | 31/2/31, adv −15 | **New Step 5 diagnosis observation (2026-07-07):** first-ever DLA-RH BLOCK capture, `q-dla-rh-block` (new anchor entry) — exact arcade match on all four fields, zero residual. Anchors §13.10 Class 1's claim that the block/whiff chain is arcade-exact for this move. |
| Dashing Leg Attack (RH), connecting | 1262–1296 | R=24 (HIT) | 31 (whiff-range figure) | HIT-R convention per §13.10, plain PASS asserting measured value (R=24, `q-dla-rh-hit`, Class 1 hit-branch recovery chain — defender is knocked down on hit, kd=1). |
| Throw (LP+LK) | (q.json Throw row) | R=34 (HIT) | 21 | HIT-R convention per §13.10, plain PASS asserting measured value (R=34, `q-throw-hit`, Class 3 connected-grab whiff-canonical R — bounded by the §13.5.1 cut, `cut=1`). WHIFF R=21 matches arcade exactly (`q-throw-whiff`, new Step 5 anchor entry). adv unchecked on HIT — arcade convention is "D" (KD), observed +101 matches the CnDB/command-throw KD convention. |

**Open-issue family RESOLVED → §13.10 (2026-07-07).** Before the Phase
3 harness, no clean HIT captures existed for HSB/DLA-RH/Throw (per the
"Hit-outcome traces" row below) — the harness's Stage 2/3 corpus
entries were the first, and their HIT-outcome R diverged from
`q.json`'s WHIFF/BLOCK-matching figure with no mechanism yet
identified. Step 5's per-subfamily diagnosis (§13.10) closed this: HSB
(all three strengths), DLA-RH, and Hugo Strong/Forward are Class 1
(hit-branch recovery chains); UOH was already explained by §13.7.3
(Class 2); Q/Ryu Throw and the Hugo grab family are Class 3
(connected-grab whiff-canonical R). All 16 member entries now cite
§13.10 (or, for UOH, §13.7.3 as generalized by §13.10) — see §13.8's
Resolved table for the per-entry disposition.

Note: `q.json` lists Dashing Head Attack twice — fast (`q.json:947–1086`,
S=13/15/19) and long-range / charged (`q.json:1087–1191`, S=24/28/32).
The trace contains the long-range variant; both arcade tables are
authoritative for their respective input timings.

### Detection & other

| Quantity | Status |
| --- | --- |
| Hit/block detection | Working — `dm_count_up` delta discriminates HIT vs BLOCK |
| Parry detection | Working for high front/back; r2=33 / r2=34 not fully tested |
| Taunt + non-attack-move classification | Fixed (§13.3) — guard at `frame_data_overlay.c:325-348` shipped. |
| Q Capture and Deadly Blow (HCB+K) | **Re-opened** — fresh trace shows a real 72-frame animation firing on all four HCB+K attempts. See §13.6. |
| Hit-outcome traces | **Resolved 2026-07-07** — the Phase 1 harness captures clean HIT outcomes for every Q normal, crouching normal, and several specials (`corpus-q.yaml` Stage 2/3). Ungates §13.2's hit-vs-block comparison (see resolved finding below) and surfaces a new HIT-R divergence family for HSB/DLA-RH/Throw/UOH. |
| Non-Q characters | Not audited. |

---

## 13. Issue history and current open issues

### 13.1 Resolved: Recovery ±1 from override / sentinel cap

**Symptom (pre-fix):**

| Move | Δ vs arcade R |
| --- | --- |
| close LP / far LP block | -1 |
| close MK block | -1 |
| Far MK block | -1 |
| close LK block | -3 |
| close HP block | +1 |
| back+HP block | +2 (A inflated) |

**Root cause #1 — meter override polluting numeric R.**
The §8.3 override forced `ACTIVE = engine_a contiguous cells starting
at first_active_idx` and then derived `R = raw_len − S − A`. That
equality holds on whiff but breaks on block in two opposite ways:

- *Hitstop on a 1-frame active cell* (close HP cgix=36): raw[] has
  *more* active rows than the accumulator credits → the override
  spilled the surplus into recovery (R+1).
- *Sub-frame fast-forward through an active cell* (Far MK cgix=12 on
  contact, close LK cgix=8): raw[] has *fewer* active rows than the
  accumulator credits → the override stole from recovery to fill A
  (R−1, or R−N when N cells got sub-frame'd).

**Fix #1:** decouple numeric S/A/R from the meter override. Tally
`startup_pf`, `active_pf`, `recovery_pf` from the per-frame classifier
output *before* the override modifies `atk_cells`. Display
`S = startup_pf`, `A = engine_a`, `R = recovery_pf`. Override still
runs to anchor the meter visualization but no longer drives numerics.
See §9.

**Root cause #2 — flat sentinel cap=10 in the engine accumulator.**
The original accumulator added a fixed `cap=10` once per sentinel
cell entry, hand-tuned for close HP. Back+HP's sentinel cell only
holds for 8 actual frames, giving us A=13 vs arcade 11.

**Fix #2:** count sentinel cells per char_move call. Entry adds 0
(it overlaps the previous cell's exit), each subsequent same-cell
call with `cg_ctr >= 200` adds 1. Hitstop frames don't run char_move
so they naturally don't contribute. See §3.2.1.

**Verification (fresh re-trace 2026-05-04):** every Q normal, crouching
normal, and sampled special matches arcade S/A/R/T exactly. Close HP
and Back+HP — which the prior trace flagged as A+1 — show the exact
arcade values in fresh capture (18/16/23 and 13/11/15). The previous
A+1 was a stale-binary artifact; see §13.4 for the build-mismatch
finding. UOH retains a residual A+1 (engine_a=11 vs arcade A=10) which
is separate from the sentinel-cell investigation here — UOH cells
aren't sentinel-bearing (cgctr 1–4, never ≥200); see §13.7.

### 13.2 Resolved by Phase 3 rebaseline (2026-07-07): crouching attack BLOCK adv +2 did not reproduce

**Phase 3 harness result (2026-07-07).** Under the Phase 1 harness
(pinned RNG, controlled spacing/teleport), cr.LK/MK/HK/HP BLOCK
advantage all match `q.json` exactly — the +2 documented below did
**not** reproduce (`q-crmk-block`, `q-crhk-block`, `q-crhp-block`, all
plain PASS). Working hypothesis: the +2 was an artifact of the old
manual-capture conditions (imprecise inter-move spacing, unpinned
RNG) rather than an engine bug — stated as a hypothesis, not proven,
since the harness didn't reproduce the original manual-capture setup
byte-for-byte. Treat this issue as **resolved-by-rebaseline**; the
corpus PASS entries above are now the ongoing regression guard. The
investigation below (blockstun state-machine walk, hypotheses,
open-work list) is preserved as historical context in case the +2
resurfaces under different conditions.

**Exception: cr.LK never classified at all — SHIPPED (2026-07-07,
`frame-data-on-mister`, issue #14).** cr.LK vs. a guarding (or even
non-guarding) dummy never set `event_this_frame` (the event edge at
`frame_data_overlay.c` ~line 929, pre-change, required `dn->dm_stop <
0`, but cr.LK's contact makes `P2.dm_stop` go `0 → +7`, positive, on
every dummy/distance/timing variant tried) — the move finalized as
WHIFF instead of BLOCK/HIT (`q-crlk-block`, `q-crlk-hit`, formerly
xfail). cr.MK/cr.HK (same defender guard class) go negative (`0 →
-9`) and classified fine, so this was cr.LK-specific, not a class-wide
gate issue. Root cause and fix: see item 14 of the open-work list —
`dm_stop`'s sign selects hitstop style, not event kind; the edge is
now `dn->dm_stop != 0`. See the resolved entry in §13.8's table.

**cr.MP HIT variance: resolved — gone under pinned RNG.** Two
independent captures of the identical setup (`q-crmp-hit-capture-a`,
`q-crmp-hit-capture-b`) produced byte-identical numbers matching
`q.json` exactly. This confirms the working hypothesis noted below:
the variance was RNG-driven dummy behavior
(`Guard_Data[zz][Lv][random_16_ex_com()]` at `com_sub.c:1875` consumes
RNG in guard logic), not an engine race or overlay bug. Kept as two
plain PASS entries specifically to demonstrate determinism.

---

**Original investigation (2026-05-04, now historical — preserved for
context; the +2 symptom it describes did not reproduce under the
Phase 3 harness, see above).**

**Symptom (updated 2026-05-04 — found cr.HP also affected, so this
is not strictly a low-attack issue):**

| Move | Our adv | Arcade adv | Δ |
| --- | --- | --- | --- |
| cr.LK block | 0 | -2 | +2 |
| cr.MK block | -1 | -3 | +2 |
| cr.HK block | -23 | -25 | +2 |
| cr.HP block | (varies) | — | +2 |

S/A/R all match arcade for these moves. Standing block adv matches
arcade. Only crouching block adv is offset by exactly +2 across
multiple cr.* moves regardless of attack height. The original
"low attacks" framing was based on the first three observations; the
fourth (cr.HP) is a high-attack-from-crouching move and also
exhibits +2.

**HIT trace state.** Some sessions now produce HIT outcomes for
normals. Most cr.* HIT adv match arcade exactly; cr.MP HIT shows
variance: +1 in some captures, -1 in others. Needs a clean
controlled-input retest where cr.MP is mashed against a non-blocking
dummy with the §13.7.7 atk=1-noise filter ON, so the relevant HIT
FINAL is unambiguous. **Resolved 2026-07-07 — see the Phase 3 result
at the top of this section: gone under pinned RNG.**

The trace columns `rno3`, `cmwk14`, `wcaix` added 2026-05-04
expose the `Damage_04000` sub-state machine (`routine_no[3]` ∈
{0,1,2,3} stage), the blockstun pause counter, and the guard-release
animation entry point. With those visible per-frame, a future agent
can diff a cr.MK BLOCK trace against a cr.MK HIT trace cell-for-cell
to identify the divergent stage.

**Investigation (cr.MK block trace, GT=2398–2430):**

| | close MK (high) | cr.MK (low) |
| --- | --- | --- |
| Att hitstop | 11 | 9 |
| Def in r1=1 (wall-clock GT span) | 30 | 24 |
| Effective blockstun (span − att hitstop) | 19 | 15 |
| Arcade-back-derived B (R−A+1−adv) | 17 | 13 |
| **Δ (ours − arcade)** | **+2** | **+2** |

Both standing and crouching defenders' blockstun is +2 longer than
arcade. For close MK the attacker animation is *also* +2 inflated
(presumably from hitstop handling), so adv balances and matches
arcade. For cr.MK only the defender side gets the +2, so it leaks
through to adv.

**Other findings:**

- `P2.hstop = 0` throughout block on both close MK and cr.MK — the
  defender doesn't enter hitstop on block in this engine. The +2 is
  not coming from defender hitstop.
- `P2.r2` stays at 6 during blockstun, transitions to 7 on the
  `r1: 1→0` transition. Same pattern for both high and low blocks —
  no obvious extra sub-stage on low.
- `P2.kow = 0` throughout cr.MK block (defender visually crouching
  but kow=0 — defender-side workid quirk; meaning of kow on the
  defender role unclear).

**Engine state machine for ground block (verified by code grep):**

The defender's `r1=1` blockstun is handled by `Damage_04000()` in
[`plpdm.c:309-352`](../src/sf33rd/Source/Game/engine/plpdm.c). It
progresses through four sub-states tracked in `routine_no[3]`:

- **Case 0** (1 tick): setup, increment to case 1.
- **Case 1** (1 tick): set `cmwk[14] = _guard_pause_table[0][dm_attlv]`,
  increment to case 2. The table is at
  [`bin2obj/etc.c:3`](../src/bin2obj/etc.c) —
  `_guard_pause_table[2][4] = {{8, 11, 14, 16}, {12, 15, 18, 20}}`.
  Row 0 is ground guard, row 1 air. Index by `dm_attlv` (0=light,
  1=medium, 2=heavy, 3=super). For medium attacks (cr.MK / close MK),
  `cmwk[14] = 11`.
- **Case 2** (`cmwk[14]` ticks): `--cmwk[14]` each tick. While > 0,
  fall through to default (`char_move`). When `<= 0`, advance to
  case 3 and call `char_move_wca()`
  ([`charset.c:237-244`](../src/sf33rd/Source/Game/engine/charset.c))
  which resets `cg_ix` to `cg_wca_ix - 1` and `cg_ctr = 1` — i.e.
  jumps the defender to the *guard-release* animation entry point.
- **Case 3** (until r1 transitions): plays the guard-release anim
  via `char_move()` per tick. The animation's last extended-code
  cell triggers `routine_no[1] = 0`.

So total blockstun = `2 (setup) + cmwk[14] (pause) + N (release anim)`.

**Observed totals:**

| Move | Setup | `cmwk[14]` | Release anim N | Total in r1=1 (GT span) |
| --- | --- | --- | --- | --- |
| cr.MK BLOCK | 2 | 11 | 11 | 24 |
| close MK BLOCK | 2 | 11 | 17 | 30 |

The post-pause "release anim N" differs between high and low blocks
even though `dm_attlv` (and therefore `cmwk[14]`) is identical. The
release animation cells are character-and-stance-specific — Q's
crouching-low-block release plays for 11 frames; standing-high-block
plays for 17. Both sides match arcade's *blockstun* number once you
back out hitstop, but for low attacks the timing of "actionable"
diverges from the visible r1: 1→0 by exactly 2 frames.

**Where r2 = 7 comes from on the transition tick:**

After r1 hits 0 (driven by an extended-code at the end of the guard-
release anim, *not* by a static write in `plpdm.c`), the r1=0
dispatcher runs the same tick. It calls `nm_NNNNN()` handlers in
[`pls00.c`](../src/sf33rd/Source/Game/engine/pls00.c), which
ultimately call `check_stand_up()`
([`pls01.c:610-619`](../src/sf33rd/Source/Game/engine/pls01.c)):

```c
s32 check_stand_up(PLW* wk) {
    if (wk->cp->sw_new & 2) { return 0; }   // bit 2 = down direction
    wk->wu.routine_no[1] = 0;                // already 0; no-op here
    wk->wu.routine_no[2] = 7;
    wk->wu.routine_no[3] = 0;
    return 1;
}
```

`sw_new & 2` checks the *rising edge* of down (down pressed THIS
frame). In our trace `P2.swnew = 0x0000` throughout the move — the
dummy doesn't re-press down each frame, so this gate always passes.
This is what sets `r2 = 7` on the transition tick.

**Hypotheses (not yet falsified):**

1. **Low-block release animation has a 2-frame "stand-back-up"
   tail** that arcade convention counts as actionable but our
   overlay reads as still-blockstun. Distinguishable via comparing
   the cgix at the moment cmwk[14] hits 0 against the cgix at the
   r1: 1→0 transition — if there's a known cell range for "release"
   vs "standup tail", we can fire `defender_idle` at the boundary
   instead of at r1=0.
2. **Hit (not block) might also be off by +2 on lows.** Our trace
   only captured BLOCK and WHIFF cases — never HIT. If HIT adv is
   correct, only block has the issue → likely the release-anim
   tail. If HIT adv is also +2, the issue is in `check_stand_up`'s
   timing in the r1=0 dispatcher.
3. **Dummy training-mode stance (P2.kow=0 throughout cr.MK block)
   might be giving the wrong release anim.** P2.kow=0 reads as
   "standing" but the defender is visually crouch-blocking. Worth
   checking whether kow on the defender role normally tracks
   standing vs crouch — if the dummy's auto-block doesn't update
   kow, the engine might be picking a stand-up release anim while
   the defender visually stays crouched, adding the 2-frame
   discrepancy.

**Open work for a fresh agent:**

- Grep the cell-data extended-code that triggers `routine_no[1] = 0`
  at the end of the guard-release anim. Likely in the data tables
  for damage patterns (search for cgd-extended-code constants near
  Damage_04000 cell-init paths).
- Capture a HIT (not block) trace on cr.MK against an unblocking
  dummy and compare adv against arcade. If matches arcade, the
  +2 is purely block-specific.
- Test against a non-Q character's cr.MK to see if the +2 is
  Q-specific or universal across the cast.
- Inspect `cg_wca_ix` for cr.MK vs close MK at the moment of
  case 2 → case 3 transition — different wca entry points would
  prove the release anims differ structurally.

### 13.3 Fixed (2026-05-04): Taunt + non-attack-move misclassification

**Status:** **Shipped and verified.** Guard implemented at
[`frame_data_overlay.c:325-348`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
(`§13.3 non-attack-move guard`). Verified via 4× HCB+K WHIFF FINALs
in `/tmp/3sx-frame-trace.log` (F=5098/5296/5498/5706), all reporting
clean `S=72 A=0 R=0 T=72` instead of the prior `S=0` garbage.

**Original symptom (pre-fix).** Q's taunt finalized as `S=0 A=50
R=12 T=62` vs arcade `S=21 A=1 R=62`. Whiff command-grab attempts
(jumping kicks etc.) showed similar `S=0 A=N R=M` rather than honest
"no active phase."

**Root cause.** When `h_att` never fires *and* `engine_a == 0`, the
finalize classifier falls back to `cg_hit_ix != 1`. For taunt and
non-attack moves this misclassifies every non-1 cghi frame as ACTIVE.

**Fix shipped.** When `engine_a == 0` AND `h_att` never fired AND no
defender event fired, the finalize classifier marks the entire move
as STARTUP (`S=raw_len A=0 R=0`). This matches the "honest no active
phase" display.

**Residual:** arcade taunt is published as A=1 (the personal-action-
with-hit single frame). Even perfect cghi-fallback suppression won't
recover that 1 frame because our overlay doesn't see the
personal-action hit-flag; that frame is invisible to us via current
capture. The fix is correct given what the overlay can observe.

### 13.4 Resolved (2026-05-04): close HP / Back+HP A+1 was a stale-binary artifact

**Status:** **Resolved via fresh re-trace.** The prior trace's A=17/12
vs arcade 16/11 turned out to be from a build that predated the
sentinel-entry=0 accumulator fix. After rebuild + re-trace, close HP
finalizes `S=18 A=16 R=23 T=57` (q.json:107-141, exact match) and
Back+HP finalizes `S=13 A=11 R=15 T=39` (q.json:773-805, exact match).

Verify via:

```sh
grep "FINAL" /tmp/3sx-frame-trace.log | grep -E "A=16 R=23|A=11 R=15"
# F=209  ... S=18 A=16 R=23 T=57 adv=-23   (close HP block)
# F=735  ... S=18 A=16 R=23 T=57 adv=-23   (close HP block)
# F=1876 ... S=13 A=11 R=15 T=39 adv=+0    (Back+HP whiff)
# F=1977 ... S=13 A=11 R=15 T=39 adv=-1    (Back+HP block)
```

#### Resolution and cautionary note for similar cases

The trace and `[CM]` log used to diagnose the prior A+1 came from
two different binary builds; the doc-side investigation incorrectly
treated them as a single dataset. The lesson: **trace and stderr
captures must come from the same binary in the same run.** As of
Phase 1 (2026-07-07), `tools/frame-data/run.sh` (§12.0) is the
standard way to get a same-binary trace on the host build — it
enforces this lesson structurally (build → run → check in one
script). The manual protocol below remains the right tool for
MiSTer-hardware-side checks; keep it for that, not as the default
host-build retest:

```sh
rm -f /tmp/3sx-frame-trace.log /tmp/cm-trace.log
build/host/3S-ARM.app/Contents/MacOS/3S-ARM 2>/tmp/cm-trace.log
# ... gameplay ...
```

(Sentinel-hold count fact, useful for future investigations: close HP
spans 11 char_move calls on cgix=40, Back+HP spans 9. With the
current accumulator code — `entry adds 0`, `same-cell sentinel adds 1`
— that yields 1+0+10+5=16 and 1+0+8+2=11 respectively. The §3.2.1
prose said "close HP holds it for 10" — actual is 11; the math works
out because entry contributes 0 either way.)

### 13.5 Multi-segment recovery inflation (UOH + High Speed Barrage)

**Symptom.** Moves where the engine concatenates a "return-to-neutral"
animation segment under the same r1=4 block report inflated R because
the overlay's move-end signal (`r1: 4 → 0`) waits for the whole
concatenated animation to finish.

| Move | Trace S/A/R | Arcade S/A/R | R inflation | Status |
|---|---|---|---:|---|
| Universal Overhead (clean) | 15/11/5 | 15/10/5 | 0 (R fixed) | §13.5.1 SHIPPED; residual A+1 → §13.7 |
| Universal Overhead (chain) | varies | 15/10/5 | varies | OPEN — §13.9 reverts |
| HSB Jab | 9/3/26 | 9/3/26 | 0 (fixed) | §13.5.2 SHIPPED |
| HSB Strong | 8/4/28 | 8/4/28 | 0 (fixed) | §13.5.2 SHIPPED |
| HSB Fierce | 9/3/28 | 9/3/28 | 0 (fixed) | §13.5.2 SHIPPED |

**UOH structure (F=916–963 in `/tmp/3sx-frame-trace.log`):**

- Real startup F=916–930 (15 frames, cgix 4→8→12, cghi 229→230→231).
- Active F=931–950 (cgix 20→24→28→32, jatix=54).
- Real recovery F=951–952 (cgix 36→40, cghi 126→127).
- **Segment transition F=953:** cgix resets to 0, cghi swaps to 234
  then to 1, pat→0. Arcade's "actionable" frame is roughly here.
- Second-segment cleanup F=953–962 (cgix 0→4→…→16, cghi=1).
- r1: 4→0 at F=963.

**HSB structure (F=964–1037 for Jab, similar for Strong/Fierce).** Three
sequential active windows (cgix 30/66/108 with jatix=70/71/70), each
followed by inter-hit gaps of 9 frames where `h_att=0`. After the
third hit at F=1010, 26 frames of post-active recovery run before
r1→0 — these 26 frames match arcade R=26 exactly. The inflation
to R=44 comes from the two 9-frame inter-hit gaps being classified
as RECOVERY by the per-frame classifier (because `seen_active`
becomes true after hit 1 and `h_att=0` during the gaps yields
RECOVERY).

**Two distinct fix paths required.**

#### 13.5.1 UOH-style (single contiguous active + cleanup tail) — SHIPPED 2026-05-04

Predicate: **"cghi → 1 after a cgix-reset has fired in this move,
where cghi=1 then persists for ≥ 3 frames."** The duration gate
distinguishes UOH/HCB-family (cghi=1 holds 6–12 frames) from Dashing
Leg / Dashing Head (cghi=1 holds 1–2 frames as a transient end-anim
artifact, not a real "cancel window opens" signal).

**Implementation:** state added to `FdMove` at
[`frame_data_overlay.c:128-141`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
(`prev_cgix`, `cgix_reset_frame`, `cghi1_first_frame`,
`cghi1_first_raw_slot`, `cghi1_count`); predicate body at
[`frame_data_overlay.c:825-896`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).
On predicate fire, sets `attacker_idle = cghi1_first_frame` and trims
`raw_len` / `meter_len` to `cghi1_first_raw_slot` so finalize ignores
the post-cut cleanup-anim cells. The same block also houses §13.6.1's
partner-release predicate (`dp.r1==3 && dn.r1==1` short-circuit cut
for command grabs) at
[`frame_data_overlay.c:842-856`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).

**Verification (fresh trace 2026-05-04).** All five UOH BLOCK
finalizes show R=5 exact arcade match (was R=12 pre-fix):

```sh
grep "FINAL" /tmp/3sx-frame-trace.log | grep "A=11 R=5"
# F=3184 ... S=15 A=11 R=5 T=31 adv=-4   (UOH point-blank BLOCK)
# F=3338 ... S=16 A=11 R=5 T=32 adv=-4
# F=3932 ... S=16 A=11 R=5 T=32 adv=-4
# F=3980 ... S=15 A=11 R=5 T=31 adv=-4
# (5th at F=2640/HSB elsewhere; verify with grep)
```

UOH whiffs in the trace show R=3 (predicate fires earlier when
no contact), e.g. F=3055/3221/3279/3375/3665/3731/3804/3865.
Whiff R=3 is below arcade R=5 by 2; the gap is driven by the
shorter active-window duration on whiff, which makes the
cghi=1 reset arrive earlier. Not currently flagged as a
defect — arcade tables canonicalize against contact, not whiff.

A residual A=11 vs arcade A=10 remains on UOH; tracked as §13.7.

##### 13.5.1a Cut-gate: grounded fresh-edge anchor (SHIPPED 2026-07-08, lever G)

The original §13.5.1 anchor condition was a LEVEL test (`cghi == 1`
after a cgix-reset), not an EDGE test. The Phase 6 cast rollout
exposed two trace-proven false-positive shapes (all GT values from the
2026-07-08 investigation traces; MOVE_START-relative):

**Shape A — chart already sitting at cghi=1 when the dip occurs.**
Oro Niou Riki (Human Pillar Driver Jab) whiff, MOVE_START GT=46:
`cghi==1` for the entire animation except the active cells (cghi=135
at GT=55–56). cgix climbs 20→…→60, dips 60→56 at GT=68 (r1 still 4,
y=0 throughout — grounded), climbs again 56→…→88, true `r1: 4→0` at
GT=86. GT=86−46 = 40 = oracle S+A+R = 9+2+29 exactly. The dip set
`cgix_reset_frame=GT68`; the level test saw cghi already 1 → anchored
GT=68, committed at GT=70 → FINAL `S=9 A=2 R=11 T=22 cut=1` (R short
by 18). The oracle was right; the overlay was wrong.

**Shape B — landing-tick label flip on airborne moves.**
Oro Jinchu Nobori (Short) whiff, MOVE_START GT=175: airborne y up to
90; last pre-landing cell cgix=92 (sentinel cgctr=250→245, GT=211–216,
y descending 58→4); at GT=217 the LANDING resets the chart (cgix
92→0, cghi 151→1, y=0). r1 stays 4 until GT=230; arcade R=19 =
GT211..229 (6 sentinel + 13 landing-recovery ticks). Level test:
anchor at GT=217 (reset tick) → `R=6 T=42 cut=1`. Ken Shoryuken Jab
whiff (MOVE_START GT=44) is identical in kind: airborne to y=43,
landing at GT=73 flips cgix 44→4 / cghi 87→1 on the landing tick;
r1→0 at GT=80; arcade R=26 = GT54..79; level test gave R=19. Yun
Nishou Kyaku Short whiff (MOVE_START GT=44) adds the nuance that
falsified two candidate fixes: its cgix decrease happens MID-AIR
(anim loop 144↔138, GT=71–76, y=40..7) and the landing at GT=77 goes
straight to cghi=1 (280→1 across the landing) — r1→0 at GT=86, arcade
R=25 = GT61..85; level test gave R=16.

**What a TRUE cleanup tail looks like (the protected cases).**
- q-uoh-samef-block (R=5, hard wall): UOH hops (y to 25) but its
  reset (cgix 40→0, GT=75) happens GROUNDED (y=0 at GT=75), the
  post-reset chart plays a real label (cghi=234, GT=75–77) and only
  then flips 234→1 at GT=78 — a grounded, post-reset fresh edge.
  R=5 = GT73..77. (q-uoh-whiff identical shape at GT=186/190 →
  measured R=3, item 18's original member, UNCHANGED by this fix.)
- q-throw-hit (R=34, hard wall, §13.10 Class 3): reset 12→6 at
  GT=1130 (grounded), post-reset chart is cghi=0 for 34 ticks, fresh
  edge 0→1 at GT=1164 → anchor GT=1164, R=34. NOTE: the reset is NOT
  to zero.
- q-cndb-*-hit (R=42/44/46, hard walls): end via the §13.6.1
  partner-release branch (`endrel=1`, `cut=0`) — that branch consumes
  `cgix_reset_frame` but never touches the cghi anchor. Reset
  detection is byte-identical pre/post this fix, so CnDB is untouched.
- Oro's own UOH (PASS R=5) adds the killer nuance: it has a MID-AIR
  anim dip (36→32 at GT=6768, y=20 — same shape as Yun Nishou) but
  then lands and plays a genuine grounded cleanup chart (cghi=170,
  GT=6774–6776, then 170→1 at GT=6777). Any "sticky airborne since
  reset" rule mis-kills this cut (measured: regressed R=5→17 FAIL in
  the v1 scratch run). The discriminator must test the EDGE, not the
  reset.

**Unifying statement.** A true cleanup tail is a distinct chart
segment entered on the ground after the reset: the chart shows at
least one post-reset tick with `cghi != 1`, and then flips to
`cghi==1` while grounded on both sides of the flip. Both
false-positive shapes violate this: shape A has no post-reset
transition into 1 at all; shape B's transition happens on/across the
landing tick.

**Falsification table** (candidates evaluated arithmetically against
the measured traces before the fix was chosen):

| Candidate | Verdict |
|---|---|
| (i) require cgix→0 on the reset | FALSIFIED — q-throw-hit's reset is 12→6, never 0 (R=34 wall breaks); Jinchu's landing reset IS 92→0 (misfire stays) |
| (ii) defender-state (partner released / KD) | FALSIFIED — q-uoh-block's defender is in plain blockstun (no grab signature on the UOH path); on whiffs P2.r1=0 for both Niou Riki AND the protected q-uoh-whiff — indistinguishable |
| (iii) r1 4→0 within N ticks (retroactive un-cut, deferred finalize) | FALSIFIED — Ken SRK's false tail (7 ticks, GT73→80) EQUALS q-uoh's true tail (7) — no N separates them; also re-enters §13.9.1/§13.9.2 deferred-finalize territory (two reverted attempts) |
| (iv) arm only on connected grabs (kow/catch at contact) | FALSIFIED — UOH block is a strike and its cut is load-bearing (R would regress to 12, the pre-§13.5.1 defect) |
| (v) hybrid v1: fresh-edge + sticky airborne-since-reset | FALSIFIED — Oro UOH's mid-air dip (GT=6768) sets the sticky flag; its genuine grounded cleanup edge at GT=6777 is rejected; measured FAIL R=17 |
| **(v) hybrid v2 (SHIPPED): grounded fresh-edge anchor** | Zero FAILs across all 12 corpora; 31 XPASS (all flips arcade-exact) |

**The shipped rule (three conditions).** The cghi=1 dwell may only
ANCHOR (set `cghi1_first_frame`) on a tick where (1) `cg_hit_ix`
transitions into 1 — the previous sampled tick in this block had
`cghi != 1` — AND (2) that previous sampled tick is at-or-after
`cgix_reset_frame` (a post-reset non-1 tick was actually observed),
AND (3) both the previous tick and the current tick are grounded
(`wu.xyz[1].disp.pos == 0`). Once anchored, dwell counting, the
3-tick commit, the §13.9.4 snapshot, and the dwell-broken logic are
byte-identical to the pre-fix code. Reset detection and the §13.6.1
partner-release branch are byte-identical. Implementation: the
`fd_cut_requires_grounded_fresh_edge` const (**lever G**) plus
`prev_cghi`/`prev_cghi_frame`/`prev_y` sampling fields in `FdMove`,
in `frame_data_overlay.c`'s §13.5.1 tick-side block. With the lever
at 0 the anchor condition reduces exactly to the legacy level test.

Per-case check of the three sub-conditions (from the traces):
- q-uoh-block: edge 234→1 at GT=78; prev GT=77 ≥ reset GT=75; y=0/0 → anchor (R=5) ✓
- q-uoh-whiff: edge 234→1 at GT=190; prev GT=189 ≥ reset GT=186; y=0/0 → anchor (R stays 3) ✓
- q-throw-hit: edge 0→1 at GT=1164; prev GT=1163 ≥ reset GT=1130; grounded → anchor (R=34) ✓
- Oro UOH: edge 170→1 at GT=6777; prev GT=6776 ≥ reset GT=6768; y=0/0 → anchor (R=5) ✓
- Niou Riki: no transition into 1 exists after reset GT=68 (cghi=1 level) → never anchors → natural r1-edge end, R=29 ✓
- Jinchu: only transition into 1 is AT the reset tick GT=217 (prev GT=216 is pre-reset) → condition (2) fails → no anchor → R=19 ✓
- Ken SRK: same as Jinchu (transition on reset tick GT=73, prev GT=72 pre-reset AND airborne y=4) → no anchor → R=26 ✓
- Yun Nishou: transition 280→1 at landing GT=77; prev GT=76 IS post-reset (reset GT=71) but airborne (y=7) → condition (3) fails → no anchor → R=25 ✓

**Two theoretical residuals of the grounded-fresh-edge rule** (neither
observed in the 12-corpus suite; both hold by construction, not by
measurement):
(a) a future move whose chart happens to land at `disp.pos==0` one
tick before its own cghi-transition would anchor under this rule —
this is a DEFINITIONAL property of what condition (3) means by "a
distinct grounded segment," not a bug, and must not be re-litigated
as a new false-positive shape without a trace showing the anchor is
actually wrong; (b) `prev_cghi`/`prev_y` are sampled at the same
cadence as `prev_cgix` (only ticks that enter the r1=4/2 block), so
sampling granularity can only ever UNDER-cut relative to a
hypothetical continuous-time version of the rule — a missed anchor
reads as a longer R, i.e. a VISIBLE divergence a corpus author would
catch — it can never silently re-admit either of the two false-cut
shapes (A/B) this fix kills.

**Blast radius (all measured, 2026-07-08).** 31 corpus entries flip
arcade-exact: Ken SRK Jab (3), Akuma SRK Jab (3), Yun Nishou Kyaku
all strengths (9), Urien Chariot whiffs + Headbutt all outcomes +
VKD-adjacent (10), Dudley Jet Uppercut whiffs (3), Oro Niou Riki
whiff/block + Jinchu whiff (3). 18 further entries change value but
stay xfail (re-cited); one diagnostic-only change
(makoto-hayate-lp-block `anchor_a` 3→−1 — the never-committed anchor
no longer arms; S/A/R/adv byte-identical). Chun-Li, Ryu, Hugo, Q,
smoke: zero behavioral change. See item 18 (§13.8 open-issue table)
for the residual early-cut family this fix does NOT explain.

##### 13.5.1b Guard-rearm commit gate (SHIPPED 2026-07-09, lever H)

§13.5.1a's grounded-fresh-edge rule (lever G) still let one more
false-cut shape through: a genuinely GROUNDED, post-reset, fresh
cghi→1 edge that satisfies all three lever-G conditions geometrically,
but whose "cleanup" segment is not actually the move's real recovery —
the rest of the animation keeps playing under that same `cghi=1` label
all the way to the natural `r1: 4→0` edge. ENGINE-2 (found 2026-07-08 on
Alex's Air Stampede review trace; a full 19-corpus census then found 26
further members hiding inside item 18(b)) is exactly this shape.

**The discriminator: the engine's own guard-type data.** `Player_attack()`
(`plpat.c:57`) sets `guard_flag = 3` unconditionally on every attacking
(r1=4) tick, then calls `jumping_guard_type_check(wk)` (`plpat.c:89`);
that function (`pls00.c:1160-1170`) clears `guard_flag` to 0 iff the
CURRENT chart cell's `cg_type` is guard-capable ({0xFF, 64, 2, 3, 7}) —
i.e. the move's own animation data says "the opponent can guard from
here." `hitcheck.c:250/315/458` confirm the semantic (`guard_flag == 3`
blocks the guard path). So while r1==4, `guard_flag == 0` is Capcom's
own data-driven "effectively neutral" marker — exactly what the arcade
table's "recovery ends" encodes.

**Measured** (full 19-corpus census, all 84 cut=1 FINAL windows): every
TRUE cleanup tail (the UOH family across 12 characters, Oro Oniyama,
Yang Senkyuutai) re-arms `guard_flag` (3→0) at the anchor tick or the
next (anchor+0/+1). Every FALSE cut — all three ENGINE-2 members, plus
a further 26-window slice hiding inside item 18(b) — keeps
`guard_flag == 3` for the ENTIRE post-anchor tail, reaching 0 only on
the natural `r1: 4→0` edge. The two populations do not overlap.

**DATED CORRECTION (2026-07-10, classification sweep #2 fable-grade
audit):** the Yang Senkyuutai figure above is WRONG. Direct re-measurement
(both `yang-senkyuutai-lk` legs) shows `guard_flag` re-arms at
**anchor−4**, not anchor+0/+1 — the §13.5.1 cut anchor actually fires at
the cghi-edge, 4 ticks AFTER the attacker's own guard_flag re-arm (gap =
overshoot = 4 exactly; a gflg-based R reconstructs arcade 34 exactly).
This is the **ENGINE-7 candidate** (`docs/plan-frame-data-completion.md`
tracker row; corpus-yang.yaml's `yang-senkyuutai-lk-*` xfail re-cites):
"§13.5.1 cut anchor fires at the cghi-edge, 4 ticks after the attacker's
guard_flag re-arm." The other members in the census above (the UOH
family, Oro Oniyama) are unaffected by this correction — only the Yang
Senkyuutai figure was wrong.

**SECOND DATED CORRECTION (2026-07-10, ENGINE-7 pre-diff census, 1,039
windows across all 19 corpora — `docs/plan-frame-data-completion.md`
ENGINE-7 row; raw data `e7-census.tsv`, hand-verification
`e7-census-report.md` §(b)NEW):** the preceding paragraph's "other
members... are unaffected" claim is ALSO WRONG, and so are the still-
standing "Measured" paragraph above (:1533-1537 — "every TRUE cleanup
tail (the UOH family across 12 characters...) re-arms guard_flag... at
the anchor tick or the next (anchor+0/+1)") and the "Blast radius"
paragraph below (:1635-1636 — "49 pass the gate unchanged (re-arm at
anchor+0/+1 — the whole UOH/Oniyama/Senkyuutai family...)"), for the
same reason as Yang Senkyuutai: a second, independently-measured
UOH-family member, `chunli-uoh` (`cut=1`), re-arms `guard_flag` at
**anchor−1**, not anchor+0/+1 — the opposite-sign, smaller-magnitude
cousin of Senkyuutai's own anchor−4. Both legs hand-verified against
`trace.log` (rundir `tmp.ol9ybVmKpI`): WHIFF shows `guard_flag` 3→0 at
F=2084 (rearm, `cghi` still 255) one tick before the `cghi` 255→1 anchor
edge at F=2085 (reset at F=2079); BLOCK shows the identical one-tick
shape at F=2248 (rearm) / F=2249 (anchor). `e7-census.tsv` confirms
delta=1 (`cur_trim − rearm_slot`) on all three legs
(`chunli-uoh-whiff/-block/-hit`). Both cited sentences are corrected to
read "anchor+0/+1, with chunli-uoh the sole exception at anchor−1"
wherever they claim a uniform anchor+0/+1 family. **Lever H itself is
unaffected** — it is a level test (`guard_flag == 0`) evaluated AT the
commit tick, trivially satisfied whether the rearm landed one tick
earlier or later; chunli-uoh's shipped values (whiff R=5 arcade-exact,
block/hit R=4 §13.13-convention-exact) are unchanged and remain correct.

This chunli-uoh finding triggered a full re-measurement of every window
in the 19-corpus suite with a grounded, persistent, post-reset
guard-rearm edge preceding its current R end: **seven distinct moves
total** (the two above plus five more: `ibuki-kazekiri-lk`, `yun-uoh`
whiff, `yang-uoh` whiff, `remy-uoh`, `urien-headbutt`). Arcade end sits
at rearm+0 for four of them, rearm+1 for two, rearm+3 (the natural end)
for one — and no candidate discriminator over any engine state the
overlay can observe separates the two populations. **Verdict: NO LEVER
K.** See §12.2.4's **ENGINE-7 CENSUS-FALSIFIED** entry for the full
seven-move table and the negative findings; `yang-senkyuutai-lk` ×3
stays xfail with this census as its permanent citation.

**Falsified candidates** (all measured before this rule was chosen):

| Candidate | Verdict |
|---|---|
| (i) retroactive un-cut once the r1 4→0 edge arrives >N ticks after the commit | FALSIFIED — elena-scratchwheel's false tail is 6 ticks (anchor→r1-edge); q-uoh-block's TRUE tail is 7 ticks. No N separates them. Also: un-cutting a WHIFF leg requires deferring its finalize decision, re-entering §13.9.1/§13.9.2 reverted territory. |
| (ii) airborne-history + reset-at/near-landing gate | FALSIFIED — q-uoh-block's own reset IS its landing tick, exactly like every ENGINE-2 member; "went airborne, reset at landing" is true for both the protected cut and the false ones. |
| (iii) post-anchor cgix continuation >M ticks | FALSIFIED — q-uoh's TRUE tail advances cgix 8→12→16 (3 cells / 7 ticks); elena's FALSE tail is byte-identical (3 cells / 6 ticks) — no M separates them. |
| **(iv) guard-rearm gate (SHIPPED)** | Survives every wall in the 19-corpus census: zero currently-PASS entries change; every changed entry moves toward (almost always exactly onto) arcade. |

**The shipped rule.** A committed §13.5.1 cut (the `cghi1_count >= 3`
dwell) is now only trusted if `an->r1 != 4` (the r1==2 grapple-tail
carve-out — `Player_catch` never re-arms guarding; this exempts
q-throw-hit's R=34 wall) OR `plw[atk].guard_flag == 0` at the commit
tick. On a refused commit the move simply finalizes at the natural r1
edge — the same path every `cut=0` move already used before this fix;
nothing is forced or deferred. Implementation:
`fd_cut_requires_guard_rearm` (**lever H**) in
`frame_data_overlay.c`'s §13.5.1 tick-side block, plus an explicit
`cut_committed` flag on `FdMove` (set only on the tick the gated commit
actually fires) that `fd_finalize()` now reads instead of re-deriving
"committed" from `cghi1_count >= 3` — on a refused commit the dwell
counter still reaches and stays >= 3, which would otherwise mis-feed
the §13.7.1/§13.9.4 retrigger override. With the lever at 0 the
condition reduces exactly to the pre-fix (lever-G-only) code.

**Residuals** (documented, not shipped as fixes):
(a) 1-tick `guard_flag` sampling lag — `jumping_guard_type_check()`
runs before `char_move` advances the chart, so `guard_flag` reflects
the PREVIOUS tick's `cg_type`. Harmless: the gate only ever
delays/refuses a commit, it never re-times the anchor itself.
(b) Late-rearm moves (`ibuki-kubiori-lp-whiff/-block`: `guard_flag`
re-arms at anchor+7, well before the r1 edge at anchor+16) keep
today's anchor-tick cut — `attacker_idle` and the trim slot are
unchanged, so their FINAL values are byte-identical. The residual
undershoot is now precisely characterized (this particular commit was
always legitimate under the new gate too) but deliberately left as-is.
(c) The `guard_flag` edge also coincides with arcade-actionable on
`cut=0` OVERSHOOT moves (`ibuki-kazekiri-*`: gflg re-arms at
r1end−3, exactly arcade-actionable) — a lead for item 18(c), explicitly
NOT shipped by this fix (that family's cut is correctly REFUSED by
lever G already; nothing about it changes here).

**Blast radius (measured, full 19-corpus census).** 84 cut=1 FINAL
windows total: 49 pass the gate unchanged (re-arm at anchor+0/+1 — the
whole UOH/Oniyama/Senkyuutai family, plus `q-uoh-chain-retrigger` and
the r1==2-exempt `q-throw-hit`); 2 commit LATE but byte-identically
(`ibuki-kubiori-lp-whiff/-block`, residual (b) above); 33 windows
change value, and every one was ALREADY an xfail, and every one moves
toward arcade. See the ENGINE-2 tracker row
(`docs/plan-frame-data-completion.md`) and item 18(b) below for the
per-entry disposition.

#### 13.5.2 HSB-style (multi-hit with inter-hit gaps) — SHIPPED 2026-05-04

**Diagnosis (via `[CMX]` opcode log).** Cross-referencing
`/tmp/cm-trace.log` opcode dispatches against HSB FINAL ranges
identified `comm_hjmp` (cell-data opcode `code=47`,
[`charset.c:1030-1046`](../src/sf33rd/Source/Game/engine/charset.c))
as the post-active discriminator: it fires exactly once per HSB
block at the frame immediately after the last `cg_ja.atix > 0`
frame. The post-`comm_hjmp`-to-`r1=0` window equals arcade R
exactly across all three strengths:

| Move | last jatix>0 GT | r1=0 GT | post-last-active gap | Arcade R |
|---|---:|---:|---:|---:|
| HSB Jab | 4853 | 4880 | 26 | 26 |
| HSB Strong | 4964 | 4993 | 28 | 28 |
| HSB Fierce | 5235 | 5264 | 28 | 28 |

`cg_cancel` confirmed dormant (always 0) for all HSB blocks — the
`cgcan` column would not have helped without `[CMX]`.

**Fix shipped.** No engine instrumentation needed for the predicate
itself — the overlay already snapshots `cg_ja.atix > 0` on every
raw[] cell (the per-frame classifier marks them ACTIVE / CONTACT).
At finalize, track `last_active_pf_idx` = last cell classified
ACTIVE / CONTACT, then override `recovery_pf` to count only cells
AFTER `last_active_pf_idx` (skipping inter-hit gaps that would
otherwise classify as RECOVERY). Implementation at
[`frame_data_overlay.c:386-402`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).
Verified non-regressing on single-active moves: for them
`last_active_pf_idx` is the only ACTIVE cell and post-it count
equals today's recovery_pf.

`[CMX]` instrumentation stays at
[`charset.c:485-496`](../src/sf33rd/Source/Game/engine/charset.c)
for future cell-data investigations even though HSB no longer needs
it.

**Caveats.** The `comm_hjmp` opcode is reused with different
semantics in non-HSB moves. The shipped fix uses the per-frame
ACTIVE classification (not opcode), so it generalizes to any
multi-hit move where some cells fire h_att / cg_att_ix and others
don't. Verify against a non-HSB multi-hit move (e.g., a multi-hit
super) when one shows up in a trace.

**Folded in 2026-07-07 (from the former `docs/hsb-recovery-investigation.md`,
now deleted per this doc's own "no sibling investigation docs" rule
— the analysis above already carries its findings).** One residual
detail from that report not otherwise captured above: its GT-based
frame tables use the engine's `Game_timer` counter, which ticks 1:1
in gameplay, while this doc's FINAL annotations (`move_start_F`,
`atk_idle_F`) use the overlay's own `g_local_frame`, which only ticks
while a move is being tracked. The two counters are not
interchangeable across pause/transition boundaries — cross-referencing
a GT-based recipe against a FINAL's frame fields requires converting
through the relevant MOVE_START/FINAL annotation, not a fixed offset.

### 13.6 Resolved (2026-05-04): Capture and Deadly Blow IS implemented; classifier was blind to catch hitboxes

**Prior conclusion was wrong.** The earlier "not implemented" finding
(based on a cmd-table walk) is refuted by both the trace evidence
and a deeper engine-source pass. Capture and Deadly Blow IS in the
engine, the cmd-table HAS the slot, and the throw machinery DOES
fire successfully. The overlay was just blind to catch hitboxes.

**Folded in 2026-07-07 (from the former `docs/q-hcb-k-reinvestigation.md`,
now deleted per this doc's own "no sibling investigation docs" rule
— the section below already carries that report's findings).** Two
residuals from that report not otherwise captured below: (1) the EX
(4-button) version of Capture and Deadly Blow is structurally wired
through the same `waza_r[i][j]` table (slot 31's `exdt[3]`,
[`pls03.c:140-167`](../src/sf33rd/Source/Game/engine/pls03.c)
handles the EX-attack path with identical indexing) but was never
exercised in a trace, so it's unverified; (2) the human-readable
names for `kow=24/26/28` rest on the S-frame match against `q.json`,
not a static source enumeration — Q's `hit_ix_table` cell data lives
in the AFS binaries, not in `src/`.

**Engine reality** (per re-investigation report and trace at
GT=8854/9049/9248/9456):

1. **Cmd-table slot exists.** `p12_cmd_31` at
   [`cmd_data.c:696-697`](../src/sf33rd/Source/Game/engine/cmd_data.c)
   has `waza_r=19` with the HCB+K motion. The prior agent missed it
   due to non-obvious lever-bit encoding (cross-verified against
   Ryu's QCF hadouken `p1_cmd_30` and Hugo's HCB+K `p6_cmd_33`).

2. **r2=19 routes to `Att_HADOUKEN2`** at
   [`plpatuni.c:123-136`](../src/sf33rd/Source/Game/engine/plpatuni.c)
   — a generic "init-and-run-cell-data" handler, not specifically a
   fireball. Move identity comes from `wk->as->char_ix` set by the
   cmd-table dispatch.

3. **Catch hitbox fires.** Trace at GT=8866 shows `P1.jcaix=2,
   P1.tsuk=1, P2.r1=3, P2.tsmd=1` — the throw machinery engages,
   partner enters thrown state. All four MOVE_STARTs land the catch.

4. **Startup matches arcade exactly.** Trace S=12/13/14 for
   LK/MK/HK = arcade `q.json` Capture and Deadly Blow S=12/13/14.
   This IS CnDB.

5. **kow=24/26/28** are stride-2 strength variants per `acatkoa_table`
   at [`charset.c:2950-2953`](../src/sf33rd/Source/Game/engine/charset.c).
   Set inside cell-data bytecode (loaded from AFS at runtime).

**Why pre-fix overlay reported S=72/A=0/R=0 WHIFF.**
[`charset.c:421-457`](../src/sf33rd/Source/Game/engine/charset.c)
accumulator keyed only on `cg_ja.atix` (attack box), ignored
`cg_ja.caix` (catch box). And `fd_snap_player`'s `h_att_set`
derivation didn't check caix either. Result: command throw fires
correctly in the engine but presents to the overlay as "no active
signal" → §13.3 no-active-signal guard kicks in → all cells
classified STARTUP → S=72.

**Fix shipped (2026-05-04).** Two surgical changes:

1. **Engine accumulator** at
   [`charset.c:421-456`](../src/sf33rd/Source/Game/engine/charset.c)
   and `set_jugde_area` at
   [`charset.c:2790-2794`](../src/sf33rd/Source/Game/engine/charset.c)
   now fire on `(cg_ja.atix != 0) || (cg_ja.caix != 0)`. The same
   `fd_prev_active_cgix` dedup applies, so caix-only moves get the
   same per-cell cgctr accumulation as strikes.
2. **Overlay snap** at
   [`frame_data_overlay.c:206-212`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
   sets `h_att_set = 1` when `cg_ja.caix != 0` (in addition to the
   existing atix/h_att checks). Downstream classification treats
   throw-active frames identically to strike-active frames — no
   parallel signal path needed.

**Verified post-fix (2026-05-04 final pass).** Three additional
fixes shipped on top of the catch-hitbox accumulator + h_att_set
extension:

1. **Throw outcome detection** at
   [`frame_data_overlay.c:621-631`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
   — partner `r1: 0 → 2/3` transition fires `event_this_frame =
   true, event = FD_OUTCOME_HIT`. Throws bypass `dm_stop`, so the
   pre-existing dm_stop-edge detector missed them.
2. **§13.5.1 predicate gate extended to `r1==4 || r1==2`** at
   [`frame_data_overlay.c:603`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).
   For CnDB, attacker r1 transitions 4→2 when the catch lands and
   stays at 2 through the throw animation; the cgix-reset+cghi=1
   predicate now fires during r1=2 to cut the cleanup tail.
3. **Live §8.3 mirror** at
   [`frame_data_overlay.c:793-827`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
   replaces per-tick atk_cells[] with the engine_a-contiguous-band
   painting that finalize uses, so meter colors don't visibly flip
   at move-end. Skipped when ACTIVE cells are scattered (multi-hit
   moves like HSB).

Trace verification (post-partner-release predicate ship):

| Move | Trace | Arcade | Status |
|---|---|---|---|
| HCB+LK HIT | S=12 A=2 R=43 | 12/2/42 | S/A exact ✓, R+1 |
| HCB+MK HIT | S=13 A=2 R=45 | 13/2/44 | S/A exact ✓, R+1 |
| HCB+HK HIT | S=14 A=2 R=47 | 14/2/46 | S/A exact ✓, R+1 |

`outcome=HIT` (was WHIFF). S and A exact arcade match across all
three strengths. R consistently +1 over arcade — improvement from
R+6 pre §13.6.1 partner-release predicate, which is now shipped at
[`frame_data_overlay.c:842-856`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
(see §13.6.1 for the analysis that recommended this signal; the
predicate now fires when `g_cur.cgix_reset_frame >= 0 && dp.r1==3 &&
dn.r1==1`).

**RESOLVED 2026-07-07 — see §13.6.1's "R+1 FIXED" note.** The +1 was
*not* an off-by-one in `attacker_idle`/the window size (that idea is
retracted there, falsified by measurement — the window was already
arcade-sized); it was a mis-attributed A/R boundary inside a
correctly-sized window, fixed by re-deriving R from the engine credit
on this path (gated on `ended_by_partner_release`). Trace now reads
S/A/R = 12/2/42, 13/2/44, 14/2/46 — exact arcade match, no residual.

**Shipped 2026-05-04: KD advantage display.** Pre-fix, CnDB hits
showed `adv=+121-+125` because `defender_idle - attacker_idle` is
the time until the thrown partner gets back up — technically correct
but useless for players. Arcade convention is "KD" (knockdown).

Implementation:

1. New `bool kd` field added to `FdLatched`
   ([`frame_data_overlay.c:160`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).
2. New helper `fd_is_knockdown_at_atk_idle()`
   ([`frame_data_overlay.c:296-300`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c))
   reads the LAST raw[] cell's `def_r1`. The `attacker_already_idle`
   gate stops raw[] from growing past attacker_idle, so the last
   cell's def_r1 reflects the defender's routine_no[1] on the last
   frame counted before attacker became idle. For CnDB, that frame
   has partner in r1=3 (thrown) — exactly the KD signal.
3. `fd_finalize` sets `g_latched.kd = true` in the HIT branch when
   the helper fires
   ([`frame_data_overlay.c:482-484`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).
   Block can't put a partner into r1=2/3 so the gate is HIT-only.
4. `frame_data_overlay_draw` renders "KD" in the FD_COL_THROW color
   slot when `g_latched.kd` is true
   ([`frame_data_overlay.c:920-948`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).
   Numeric advantage is preserved in `g_latched.advantage` so the
   FINAL trace annotation can still show the underlying number.
5. FINAL trace annotation now emits `kd=N`
   ([`frame_data_overlay.c:489-501`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c))
   so post-hoc traces can grep for KD events.

Verification: build clean
(`cmake --build /Users/sb/Developer/3sx-mister/build/host -j8`).
In-game test: trigger CnDB (HCB+LK/MK/HK in training mode); the
numeric line should show `KD` (purple, throw-color) instead of
`+121..+125` (green).

**Hard reminder:** per `feedback-trust-user-actions.md`, the user
performed the HCB+K motion. The trace shows the engine responding
correctly. The overlay was at fault, not the user or the engine.

#### 13.6.1 R+1 RESOLVED 2026-07-07 — CnDB partner-release predicate (was: R+6 investigation)

**Status: shipped at
[`frame_data_overlay.c:842-856`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
inside the §13.5.1 block.** Cuts CnDB cleanup tail when partner
exits grappled state (`dp.r1==3 && dn.r1==1`). Reduced R+6 → R+1
across all three strengths. The investigation below was the basis
for the chosen signal; it's preserved verbatim for future
reference.

**Internal inconsistency — RESOLVED by Phase 3 harness (2026-07-07).**
This subsection's heading reported an R+1 residual, and the §12
verification table reported R=43/45/47 vs arcade 42/44/46. But the
prediction math in this subsection (the table at "Identification of
the cut signal" below, three lines computing R = 42/44/46) described
the predicate yielding *exact* arcade match — an internal
inconsistency that could not be resolved from the 2026-05-04 trace
because that trace predated later commits (see §13.7 "Trace state"
notes).

**The Phase 1 harness resolves it:** a fresh capture on a same-run
binary (`docs/plan-frame-data-harness.md`'s H6 acceptance run,
2026-07-07) confirms **R+1 is real**, not a stale-binary artifact —
`q-cndb-lk-hit`/`q-cndb-mk-hit`/`q-cndb-hk-hit` in
`tools/frame-data/corpus-q.yaml` all reproduce R=43/45/47 vs
`q.json`'s 42/44/46 (marked xfail, citing this section).

**R+1 FIXED 2026-07-07 (Phase 4 items 4+5) — the window was always the
right size; the boundary inside it was wrong.** The "tighten
`attacker_idle` by one frame (`g_local_frame - 1` → `g_local_frame -
2`)" idea floated immediately above is **retracted, falsified by
measurement**: a fresh same-run trace confirms `raw_len == arcade
S+A+R` exactly on all three CnDB HIT entries (56/59/62 vs arcade
12+2+42=56, 13+2+44=59, 14+2+46=62) and on their WHIFF siblings — the
partner-release window is already the right size. Shrinking it by a
frame would undercount T/adv/kd sampling by one frame across the
board, not fix R.

The real mechanism: the catch cell (declared `cg_ctr=2`) collapses
into the throw segment after only 1 real tick instead of its declared
2 — trace evidence at F=3613-3614 (`p44/lk_window.txt`) shows the
active frame collapsing directly into the next segment. The lost
declared tick's duration flows into the recovery tail
(`recovery_pf` counts it as recovery) while displayed A
(`effective_a`, the engine credit accumulator) already counted it as
active — the frame is double-attributed across the A/R boundary, not
missing from the window.

**Fix:** re-derive R from the engine credit on this path only —
`recovery = meter_len - startup_pf - effective_a` (clamped ≥ 0) —
gated by an explicit `bool ended_by_partner_release` on `FdMove`, set
only inside this branch (never inferred from `kd`/`def_r1` at
finalize time, since ordinary Throw also has `def_r1==3` at
`attacker_idle` and would be ambiguous). This re-derivation formula is
**not a universal S/A/R identity** — it was tested against every
currently-PASS entry with `engine_a != active_pf` (sub-framed contact
cells / hitstop-stretched sentinels shift `raw_len` without disturbing
`recovery_pf`) and regresses ~27 of them (e.g. `q-far-jab-block`
4→3, `q-fierce-block` 23→24, `q-uoh-chain-retrigger` 5→-2). It is
exact only when the window itself is externally wall-clocked, which
is unique to the partner-release path — hence the gate.

Shipped result: `q-cndb-lk-hit`/`-mk-hit`/`-hk-hit` now show R=42/44/46
(plain PASS, `kd: 1` run-failing); T also becomes honest on this path
(`T := meter_len`, landing at 56/59/62 == the corrected S+A+R — see
the new S/A/R/T convention subsection below). Proven load-bearing by a
dedicated mutation test: forcing the release-R branch off in
`fd_finalize()` flags exactly the three CnDB HIT entries at their old
R=43/45/47 values; restoring the branch returns the run to green.

---

##### S/A/R/T display convention (Phase 4 items 4+5)

- **S** = frames before the first per-frame ACTIVE/CONTACT cell
  (`startup_pf`), unchanged.
- **A** = arcade-canonical engine credit (`effective_a`), unchanged —
  the anchor-gated engine accumulator that survives sub-framed and
  hitstop-stretched cells (§13.7.1/§13.9.4).
- **R** = arcade-canonical recovery (`recovery_pf`, post-last-active
  per §13.5.2), **except** on the §13.6.1 partner-release path, where
  it is re-derived from the engine credit as described above.
- **T** = real measured move duration (`meter_len`, the trimmed
  `raw_len`), displayed honestly even when `S+A+R != T`. Multi-hit
  moves leave inter-hit gap frames in neither S, A, nor R (the §13.5.2
  override counts recovery only after the last active cell), so T can
  read well above the S+A+R sum (e.g. `q-hsb-jab-block` T=38→57).
  Sub-framed or hitstop-stretched contact cells can also shift T by
  1-2 frames from the sum on ordinary normals — that delta is real
  engine behavior, not a display bug (existing comment at
  `frame_data_overlay.c:552-558`, now generalized to T). T is
  display/FINAL-line only: the checker's `NUMERIC_FIELDS`
  (`tools/frame-data/check_frame_data.py:38`) never include T and no
  corpus entry asserts `expect.T`, so this is a zero-checker-regression
  change. T saturates at `FD_METER_LEN` for moves whose raw[] capture
  window is the limiting factor. Throw outcome keeps its own
  pre-existing total (`attacker_idle - move_start`), unaffected.

**Original investigation (now historical, motivates the shipped fix):**

**Trace session under analysis.** `/tmp/3sx-frame-trace.log`, 1188
rows, three CnDB HIT FINALs landed cleanly:

```sh
grep "^# F=" /tmp/3sx-frame-trace.log | grep -E "MOVE_START|FINAL"
# F=150 MOVE_START GT=343 atk=0 char=17 r1=4 r2=19 cgix=12 cgctr=3 cghi=220 pat=0 kow=24 ...
# F=332 FINAL atk=0 outcome=HIT S=12 A=2 R=48 T=62 ... move_start_F=752 atk_idle_F=813
# F=336 MOVE_START GT=529 atk=0 char=17 r1=4 r2=19 cgix=12 cgctr=4 cghi=220 pat=0 kow=26 ...
# F=523 FINAL atk=0 outcome=HIT S=13 A=2 R=50 T=65 ... move_start_F=938 atk_idle_F=1002
# F=602 MOVE_START GT=821 atk=0 char=17 r1=4 r2=19 cgix=12 cgctr=4 cghi=220 pat=0 kow=28 ...
# F=794 FINAL atk=0 outcome=HIT S=14 A=2 R=52 T=68 ... move_start_F=1230 atk_idle_F=1297
```

`move_start_F` / `atk_idle_F` are overlay-frame counters
([`frame_data_overlay.c:179`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)),
not GT. Mapping to GT via the per-move MOVE_START annotation gives:

| Move | move_start_GT | arcade S+A+R | arcade-actionable GT | trace cut GT |
|---|---:|---:|---:|---:|
| HCB+LK | 343 | 12+2+42=56 | 399 | 404 |
| HCB+MK | 529 | 13+2+44=59 | 588 | 593 |
| HCB+HK | 821 | 14+2+46=62 | 883 | 888 |

Cut consistently fires +5 GT past arcade-actionable. R is +6 because
the trace counts the move_start frame in T while arcade
canonicalizes S+A+R as the inclusive frame budget — verified by
`atk_idle_F - move_start_F = 61/64/67 = T-1` in the FINAL output.

**Per-strength frame-by-frame table.** Window
`[arcade-actionable - 5, arcade-actionable + 5]`. Awk recipe:

```sh
# HCB+LK (window GT 394..405)
awk 'NR>15 && /^[0-9]/ && $2>=394 && $2<=405 \
    {printf "GT=%s P1.r1=%s P1.cgix=%s P1.cghi=%s P1.jatix=%s P1.jcaix=%s P1.cgcan=%s P1.cgctr=%s | P2.r1=%s P2.cgix=%s P2.tsmd=%s\n", \
     $2,$3,$5,$7,$8,$9,$10,$30,$34,$36,$55}' /tmp/3sx-frame-trace.log
```

HCB+LK (arcade-actionable GT=399, current cut GT=404):

| GT | P1.r1 | P1.cgix | P1.cghi | jatix | jcaix | cgcan | P2.r1 | P2.cgix | P2.tsmd |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 394 | 2 | 60 | 0 | 0 | 0 | 0 | 3 | 18 | 1 |
| 395 | 2 | 60 | 0 | 0 | 0 | 0 | 3 | 18 | 1 |
| 396 | 2 | 60 | 0 | 0 | 0 | 0 | 3 | 18 | 1 |
| 397 | 2 | 66 | 0 | 0 | 0 | 0 | 3 | 20 | 1 |
| 398 | 2 | 66 | 0 | 0 | 0 | 0 | 3 | 20 | 1 |
| **399** | **2** | **66** | **0** | 0 | 0 | 0 | **3** | **20** | **1** |
| 400 | 2 | **72** | 0 | 0 | 0 | 0 | **1** | **8** | 1 |
| 401 | 2 | 72 | 0 | 0 | 0 | 0 | 1 | 8 | 0 |
| 402 | 2 | 72 | 0 | 0 | 0 | 0 | 1 | 12 | 0 |
| 403 | 2 | 72 | 0 | 0 | 0 | 0 | 1 | 12 | 0 |
| **404** | 2 | **78** | **1** | 0 | 0 | 0 | 1 | 12 | 0 |
| 405 | 2 | 78 | 1 | 0 | 0 | 0 | 1 | 16 | 0 |

HCB+MK (arcade-actionable GT=588, current cut GT=593):

| GT | P1.r1 | P1.cgix | P1.cghi | P2.r1 | P2.cgix | P2.tsmd |
|---:|---:|---:|---:|---:|---:|---:|
| 583 | 2 | 60 | 0 | 3 | 18 | 1 |
| 584 | 2 | 60 | 0 | 3 | 18 | 1 |
| 585 | 2 | 60 | 0 | 3 | 18 | 1 |
| 586 | 2 | 66 | 0 | 3 | 20 | 1 |
| 587 | 2 | 66 | 0 | 3 | 20 | 1 |
| **588** | **2** | **66** | **0** | **3** | **20** | **1** |
| 589 | 2 | **72** | 0 | **1** | **8** | 1 |
| 590 | 2 | 72 | 0 | 1 | 8 | 0 |
| 591 | 2 | 72 | 0 | 1 | 12 | 0 |
| 592 | 2 | 72 | 0 | 1 | 12 | 0 |
| **593** | 2 | **78** | **1** | 1 | 12 | 0 |

HCB+HK (arcade-actionable GT=883, current cut GT=888):

| GT | P1.r1 | P1.cgix | P1.cghi | P2.r1 | P2.cgix | P2.tsmd |
|---:|---:|---:|---:|---:|---:|---:|
| 878 | 2 | 60 | 0 | 3 | 18 | 1 |
| 879 | 2 | 60 | 0 | 3 | 18 | 1 |
| 880 | 2 | 60 | 0 | 3 | 18 | 1 |
| 881 | 2 | 66 | 0 | 3 | 20 | 1 |
| 882 | 2 | 66 | 0 | 3 | 20 | 1 |
| **883** | **2** | **66** | **0** | **3** | **20** | **1** |
| 884 | 2 | **72** | 0 | **1** | **8** | 1 |
| 885 | 2 | 72 | 0 | 1 | 8 | 0 |
| 886 | 2 | 72 | 0 | 1 | 12 | 0 |
| 887 | 2 | 72 | 0 | 1 | 12 | 0 |
| **888** | 2 | **78** | **1** | 1 | 12 | 0 |

**Candidate signals analyzed.**

| Signal | Fires at GT (LK / MK / HK) | Δ vs arcade-actionable | Verdict |
|---|---|---:|---|
| `cghi: 0 → 1` (current §13.5.1) | 404 / 593 / 888 | **+5** | Late |
| `cg_cancel != 0` (cgcan column $10) | never | n/a | Dormant — see below |
| `[CMX]` opcode at attacker r1=2 cgix=72 | 400 / 589 / 884 | **+1** | Frame after target cut |
| `P1.cgix: 66 → 72` (attacker advance) | 400 / 589 / 884 | **+1** | Frame after target cut |
| **`P2.r1: 3 → 1` (defender released)** | **400 / 589 / 884** | **+1** | **Cleanest signal** |
| `P2.cgix: 20 → 8` (defender new anim) | 400 / 589 / 884 | +1 | Equivalent to P2.r1 transition |
| `P2.tsmd: 1 → 0` (tsukamare cleared) | 401 / 590 / 885 | +2 | One frame late |

`cg_cancel` dormancy verified across all three windows (matches the
§13.5.2 HSB observation that `cgcan` was dormant there too):

```sh
awk 'NR>15 && /^[0-9]/ && (($2>=343 && $2<=410) || ($2>=529 && $2<=600) \
    || ($2>=821 && $2<=895)) && $10!=0 {print $2, "P1.cgcan="$10}' \
    /tmp/3sx-frame-trace.log
# (no output — column $10 never goes nonzero in any of the three windows)
```

`[CMX]` opcode dispatch (player 1 only,
[`charset.c:485-496`](../src/sf33rd/Source/Game/engine/charset.c))
filtered to attacker r1=2 shows uniform `code=1024` (`comm_pat`,
pattern set) at the cgix=72 entry frame for all three strengths —
no distinctive opcode that matches the §13.5.2 `comm_hjmp` (code=47)
HSB precedent. The attacker-side cell-data dispatch during the
throw-anim is essentially "advance pattern, hold N frames" with no
cancel-window opcode.

```sh
grep "\[CMX\]" /tmp/cm-trace.log | awk -F'[= ]' \
    '{for(i=1;i<=NF;i++) if($i=="GT"){gt=$(i+1); break}} \
     gt>=394 && gt<=410 {print}' | grep "r1=2"
# [CMX] GT=394 r1=2 cgix=60 code=771 koc=0 ix=0 pat=23889
# [CMX] GT=397 r1=2 cgix=66 code=777 koc=0 ix=0 pat=23890
# [CMX] GT=400 r1=2 cgix=72 code=1024 koc=0 ix=0 pat=23891
# [CMX] GT=404 r1=2 cgix=78 code=1024 koc=0 ix=0 pat=23892
# [CMX] GT=408 r1=2 cgix=84 code=1024 koc=0 ix=0 pat=23893
```

(LK shown; MK/HK have identical `code=771/777/1024/1024/1024`
pattern at cgix 60/66/72/78/84.)

**Attacker cgix progression (first frame of each cell during r1=2).**

```sh
awk 'NR>15 && /^[0-9]/ && $2>=343 && $2<=410 && $3==2 \
    {print $2, "cgix="$5, "cgctr="$30, "P2.r1="$34}' /tmp/3sx-frame-trace.log \
    | awk 'BEGIN{prev=-1} prev_cgix!=$2 {print; prev_cgix=$2}'
```

| cgix | LK first-GT | MK first-GT | HK first-GT | cgctr at entry |
|---:|---:|---:|---:|---:|
| 48 (post-r1=4→2 catch) | 355 | 542 | 835 | 2 |
| 6  | 356 | 543 | 836 | 3 |
| 12 | 359 | 546 | 839 | 2 |
| 18 | 361 | 548 | 841 | 2 |
| 24 | 363 | 550 | 843 | 3 |
| 30 | 366 | 553 | 846 | 4 |
| 36 | 370 | 557 | 850 | 5 |
| 42 | 375 | 562 | 855 | 6 |
| 48 (hold cell) | 381 | 568 | 861 | **10 / 12 / 14** ← strength variance lives here |
| 54 | 391 | 580 | 875 | 3 |
| 60 | 394 | 583 | 878 | 3 |
| 66 | 397 | 586 | 881 | 3 |
| **72 (= release)** | **400** | **589** | **884** | 4 |
| 78 (cghi=1 begins) | 404 | 593 | 888 | 4 |

Strength variability lives entirely in cgix=48's cgctr (LK=10,
MK=12, HK=14 — matches the strength-spread in the arcade
S=12/13/14 + later cells). Cells 54/60/66 are 3F each in all three;
cgix=72 entry sits at exactly arcade-actionable+1 across all three
strengths without exception.

**Identification of the cut signal.**

The cleanest, simultaneously-firing signal across all three
strengths is **`P2.r1` transitioning from 3 to 1** (defender lifted
out of grappled state). Fires at GT 400 / 589 / 884 — exactly
arcade-actionable+1. Setting `attacker_idle = g_local_frame - 1` on
this transition yields:

- HCB+LK: attacker_idle at GT 399 → R = 399-343-12-2 = 42 ✓
- HCB+MK: attacker_idle at GT 588 → R = 588-529-13-2 = 44 ✓
- HCB+HK: attacker_idle at GT 883 → R = 883-821-14-2 = 46 ✓

Equivalent signal: **attacker `P1.cgix` transitions from 66 to 72**
(i.e., first frame at cgix=72). These two transitions happen on the
exact same game-tick because the partner-release routine and
attacker-cell-advance are coupled in the throw machinery for this
move. Either is implementable; the partner-side `P2.r1: 3→1` edge
is more semantically meaningful (it mirrors the arcade convention
"actionable = partner is no longer in grappled state").

**Recommended predicate adjustment.**

Add a CnDB-specific predicate alongside the existing §13.5.1 block
at
[`frame_data_overlay.c:677-709`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).
The outer gate (`an->r1 == 2 || an->r1 == 4`,
`g_cur.attacker_idle < 0`, `!g_cur.is_throw`) already isolates
CnDB-style command throws. Inside that gate, before the
`now_cghi == 1` test at
[`frame_data_overlay.c:694`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c),
check the partner-side state edge using the existing `dp` / `dn`
locals declared at
[`frame_data_overlay.c:570-571`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c):

```c
/* §13.6.1 CnDB partner-release cut: P2.r1: 3 → 1 marks the
 * exact arcade-actionable frame for command throws. Gate by
 * cgix_reset_frame so we only fire after the attacker has
 * entered the throw-anim (avoids any earlier r1=3→1 outside
 * the move). */
if (g_cur.cgix_reset_frame >= 0
    && dp->r1 == 3 && dn->r1 == 1) {
    g_cur.attacker_idle = g_local_frame - 1;
    if (g_cur.raw_len > 0) {
        g_cur.raw_len--;
        g_cur.meter_len = g_cur.raw_len;
    }
}
```

Place this conditional immediately before the `now_cghi == 1` block
at line 694 — the cghi=1 fallback then only fires for cases where
the partner-release edge doesn't apply (UOH / non-throw multi-
segment moves where dp.r1 was never 3).

**Cross-check — would this signal misfire?**

- Within all three CnDB windows the only `dp.r1==3 && dn.r1==1`
  transition is at GT 400/589/884 (verified by inspecting the table
  above — P2.r1 is 0 throughout startup, 3 during the throw-anim,
  then 1 from arcade-actionable+1 onward).
- The §13.5.1 outer gate `!g_cur.is_throw` already excludes regular
  throws (Q's normal LK/HK throws have `is_throw=true` at MOVE_START
  per
  [`frame_data_overlay.c:565`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) — `now[i].r1 == 2 || 3` is_throw test).
- For UOH / HSB / normals, the partner is never in r1=3 during
  attacker r1=4/2, so `dp.r1 == 3` fails and the cghi=1 fallback
  runs as today.
- The `cgix_reset_frame >= 0` guard adds defense in depth: the
  cgix-reset fires at GT=356/543/836 (concurrent with the 4→2 r1
  transition + cgix wrap from 48 to 6); the partner-release edge
  always lands well after the reset, so the guard adds no false
  negatives but rules out pathological sequences.

**Constraints / unverified.**

- Single-trace investigation. Three samples (one per strength). Not
  re-tested across multiple CnDB attempts at varied distances or
  partner states. A second trace session would strengthen the
  result.
- P2 (defender) is Ken throughout this trace. Whether the P2.r1: 3→1
  edge timing differs by defender character (e.g., bigger characters
  with different thrown-recovery anims) is **not verified**. Arcade
  R=42/44/46 is independent of defender, so any per-defender drift
  on the trace signal would be a defender-specific correction, not a
  strength-variant one.
- No data captured for CnDB whiff (no catch lands). Whiffs likely
  remain on the §13.5.1 cghi=1 fallback path because P2.r1 stays at
  0 throughout — verify on next trace.
- KD advantage display (now shipped per §13.6 KD subsection) is
  unaffected by this fix.

#### 13.6.2 Strike-knockdown diagnosis, 2026-07-07 (Phase 4 item 3)

**Bug.** `fd_is_knockdown_at_atk_idle()` only recognizes the throw family
(`last_def_r1 ∈ {2, 3}`). A swept defender (Q cr.HK on hit and similar)
stays in `Player_damage` (r1=1) for the entire knockdown, so the helper
returns false and the overlay renders a large green `+N`
(time-until-wakeup) instead of "KD".

**Method.** Scratch corpus
(`$SCRATCHPAD/corpus-kd-diag.yaml`, not committed) cloned verbatim
setup/input/waits from `tools/frame-data/corpus-q.yaml` for: `q-crhk-hit`,
`q-backhk-hit`, `q-fierce-hit` (candidates), `q-jab-hit`, `q-crmk-hit`
(ordinary-hitstun negative controls — one with small positive advantage,
one with negative advantage), `q-cndb-lk-hit` (throw-KD positive control),
`q-throw-hit` (disposition unknown pre-capture). Built `build/host` once
(same-binary protocol), ran the harness's manual-capture recipe (bypassing
`run.sh`'s RUNDIR cleanup) against `training-frame-data` with
`--test-pin-rng`, twice consecutively against per-run `mktemp -d`
RUNDIRs. **FINAL lines were byte-identical across both runs**
(determinism confirmed by direct diff of the two `grep FINAL trace.log`
outputs). RUNDIRs are ephemeral temp directories, not preserved past the
implementing session, per the plan's "no committed scratch" rule.

For each entry, the defender row at the last appended `raw[]` cell
(`atk_idle_F − 1` in the overlay's local-frame counter, converted to
`frame_trace`'s own counter via the per-move `MOVE_START` offset — see
the Known-pitfall note two paragraphs below) was read directly off the
per-row trace (`P2.r1`, `P2.r2`, `P2.rno3`, `P2.pat`, `P2.kow`, `P2.y`):

| Entry | today's `kd=` | `adv=` | last raw[] cell: `P2.r1` / `P2.r2` / `rno3` / `pat` / `kow` / `y` |
|---|---|---|---|
| `q-crhk-hit` (cr.HK, candidate) | 0 | +18 | r1=1 / **r2=16** / rno3=3 / pat=38 / kow=0 / y=0 |
| `q-backhk-hit` (Back+HK, candidate) | 0 | +17 | r1=1 / **r2=16** / rno3=3 / pat=38 / kow=0 / y=0 |
| `q-fierce-hit` (Fierce, candidate) | 0 | +51 | r1=1 / **r2=19** / rno3=3 / pat=38 / kow=0 / y=0 |
| `q-jab-hit` (ordinary hit, small +adv, negative control) | 0 | +2 | r1=1 / **r2=12** / rno3=2 / pat=0 / kow=0 / y=0 |
| `q-crmk-hit` (ordinary hit, −adv, negative control) | 0 | −2 | r1=0 / r2=36 / rno3=0 / pat=0 / kow=0 / y=0 |
| `q-cndb-lk-hit` (CnDB, throw-KD positive control) | **1** | +124 | r1=3 / r2=1 / rno3=1 / pat=0 / kow=0 / y=81 |
| `q-throw-hit` (normal throw) | **1** | +101 | r1=3 / r2=1 / rno3=1 / pat=0 / kow=0 / y=156 |

Trace excerpts (local-frame column is `frame_trace`'s own `F=`, converted
from the overlay's `atk_idle_F` via the `MOVE_START` trace-`F` vs.
`move_start_F` offset for that window):

```
q-crhk-hit    F=63 (=atk_idle-1)  P1.r1=4 P2.r1=1 P2.r2=16 rno3=3 pat=38 kow=0 y=0
              F=82 FINAL outcome=HIT S=12 A=8 R=33 T=53 adv=+18 kd=0 ...
q-backhk-hit  F=146 (=atk_idle-1) P1.r1=4 P2.r1=1 P2.r2=16 rno3=3 pat=38 kow=0 y=0
              F=164 FINAL outcome=HIT S=11 A=7 R=35 T=53 adv=+17 kd=0 ...
q-fierce-hit  F=230 (=atk_idle-1) P1.r1=4 P2.r1=1 P2.r2=19 rno3=3 pat=38 kow=0 y=0
              F=282 FINAL outcome=HIT S=18 A=16 R=23 T=57 adv=+51 kd=0 ...
q-jab-hit     F=334 (=atk_idle-1) P1.r1=4 P2.r1=1 P2.r2=12 rno3=2 pat=0 kow=0 y=0
              F=337 FINAL outcome=HIT S=4 A=4 R=6 T=14 adv=+2 kd=0 ...
q-crmk-hit    F=314 (=atk_idle-1) P1.r1=4 P2.r1=0 P2.r2=36 rno3=0 pat=0 kow=0 y=0
              F=315 FINAL outcome=HIT S=7 A=3 R=14 T=24 adv=-2 kd=0 ...
q-cndb-lk-hit F=371 (=atk_idle-1) P1.r1=2 P2.r1=3 P2.r2=1 rno3=1 pat=0 kow=0 y=81
              F=496 FINAL outcome=HIT S=12 A=2 R=43 T=57 adv=+124 kd=1 ...
q-throw-hit   F=595 (=atk_idle-1) P1.r1=2 P2.r1=3 P2.r2=1 rno3=1 pat=0 kow=0 y=156
              F=697 FINAL outcome=HIT S=2 A=1 R=34 T=37 adv=+101 kd=1 ...
```

**Deliverable question, answered.** At the last raw[] cell:

- `r1==1` alone is **ambiguous**: both the strike-KD candidates (positive
  advantage, defender still busy when attacker recovers) and `q-jab-hit`
  (an ordinary ~2-frame-advantage jab, no knockdown at all) sample as
  `r1=1` at this point — any move with positive-or-near-zero advantage
  has the defender still mid-hitstun when the attacker goes idle. `r1==1`
  cannot be the sole predicate.
- **`r2` cleanly separates them.** The three strike-KD candidates land on
  `r2 ∈ {16, 19}` — both inside the `Damage_16000`…`Damage_23000`
  buttobi/blown-away dispatch range identified by static reading of
  `plpdm_lv_00` (`plpdm.c:157-161`) — while both ordinary-hitstun controls
  land on `r2=12` (`Damage_12000`, the standing hit-reaction dispatch)
  or `r2=36` (post-recovery idle-reset value, only reached once `r1` is
  already back to 0). No overlap between the measured KD set `{16, 19}`
  and the measured non-KD set `{12, 36}` at this sampling point.
- `rno3`/`pat`/`kow`/`y` were inspected as candidate auxiliary fields but
  are not needed since `r2` alone is unambiguous: `rno3` is 2 or 3 for
  both KD and non-KD-but-still-busy entries (a generic "settled
  sub-state" value reused across multiple `r2` dispatch targets — the
  same cross-family reuse the plan warned `oki_select_table2` produces
  for `r2` itself), so `rno3` alone would NOT separate the sets; `kow`
  and `y` are 0 for all five strike-family entries regardless of KD
  status (the buttobi vertical launch hasn't visibly started yet at
  `atk_idle−1`; `y` only departs from 0 several frames later, past the
  point the overlay samples) — also not usable. `pat_status` happens to
  also separate this exact comparison set (38 for the three KD
  candidates, 0 for both non-KD controls) but is not used as part of the
  predicate: it is redundant with `r2` here, and unlike `r2` its meaning
  in the down-family case was not independently traced to a dispatch
  table, so `r2` remains the sole, engine-grounded discriminator.
- **`q-cndb-lk-hit` confirms the throw-KD path is unaffected**: `kd=1`
  today, `r1=3` (thrown) at the last raw cell, matching §13.6's existing
  documented behavior exactly.
- **`q-throw-hit` already emits `kd=1` today** — its last raw[] cell has
  `r1=3` (the same catch/grapple family the throw test already checks),
  so the existing `r1 ∈ {2,3}` test already fires for normal Throw. This
  resolves risk item 4 in the plan's "Risks / open questions": `q-throw-hit`'s
  disposition is "already true pre-fix," not something the fix changes.
- **Fierce does enter a down-family reaction in-engine** (`r2=19`,
  inside the buttobi range) — the q.json "D" is not a stagger-only
  divergence for this move; include it in Step 3's corpus additions.

**Chosen predicate (measured, not inferred):** in addition to the
existing throw test, treat the defender as knocked down when the last
raw[] cell has `def_r1 == 1` **and** `def_r2 ∈ {16, 19}` — exactly the
measured down-family set for this diagnosis's three candidate moves.
This is deliberately narrower than the full static `Damage_16000`–
`Damage_23000` range (`plpdm.c:157-161`); r2 values 14, 15, 17, 18, and
20–23 were never captured in a trace and are out of scope for this fix
per the plan's "no static-only conclusions" rule — a future strike-KD
move landing on one of those values would need its own capture before
being added to the set.

**STOP rule check:** did not fire. The downed defender is distinguishable
from ordinary hitstun at the sampling point via `def_r2` membership; no
advantage-threshold heuristic was needed or used.

**SHIPPED 2026-07-07 on `frame-data-on-mister` (Phase 4 item 3 fix).**
`FdMove.raw[]` gained `s16 def_r2` (populated from the same `fd_snap_player`
snapshot `def_r1` already reads); `fd_is_knockdown_at_atk_idle()` now
returns true for the pre-existing throw test (`def_r1 ∈ {2,3}`, unchanged)
OR `def_r1 == 1 && def_r2 ∈ {16, 19}` (this subsection's measured set). No
other logic changed — `fd_finalize`'s HIT-only gate, the FINAL `kd=%d`
emission, and the entire draw path are untouched; strike-KD inherits the
existing purple "KD" rendering. Corpus: `q-crhk-hit`, `q-backhk-hit`,
`q-fierce-hit` gained `expect: { kd: 1 }` (all three were already PASS on
S/A/R; kd is a new, previously-unexpressible assertion, so the harness
verdicts are unchanged — 69 entries, 57 PASS/12 XFAIL, exit 0 both before
and after this fix). Mutation test (temporarily reverting the helper to
the throw-only body, rebuild, run): exit 1, `total=69
(FAIL=3, PASS=54, XFAIL=12)` — exactly the three non-xfail strike-KD
entries (`q-crhk-hit`, `q-backhk-hit`, `q-fierce-hit`) report `kd 0 != 1`
and verdict FAIL; the xfail'd `q-cndb-*-hit`/`q-throw-hit` entries are
**unaffected** (still `kd=1`, no kd mismatch note, only their pre-existing
R mismatch) since their last raw[] cell has `def_r1 ∈ {2,3}` — the
untouched throw-only branch the mutation reverts to — confirming the
mutation is scoped exactly to the new strike-only branch and does not
touch the throw family at all. Restored, rebuilt, green again (identical
69/57/12 table, exit 0). See `docs/plan-frame-data-harness.md` Phase 4
item 3.

**Superseded 2026-07-09 — see §13.6.2b below.** The `{16, 19}` set and
the last-cell-only sampling point this subsection shipped were both
found to be too narrow (cast-rollout census: `remy-crfierce-hit`,
`sean-twdshk-hit`). §13.6.2b grows the set to `{14..23} ∪ {27, 28}` and
adds a per-tick latch over the finalize-deferred window; this
subsection's own diagnosis, mutation test, and STOP-rule reasoning stay
historically accurate for what was known at the time.

#### 13.6.2b Strike-KD latch + measured down-family set, 2026-07-09 (ENGINE-3)

**Bug (found via the cast rollout).** §13.6.2's predicate — `def_r1==1
&& def_r2 ∈ {16,19}` sampled at the LAST raw[] cell only — is both too
narrow (the down-family dispatch range is wider than {16,19}) and
structurally blind to two distinct timing failures. Corpus census
(`remy-crfierce-hit`, `sean-twdshk-hit`) found genuine, arcade-confirmed
knockdowns (`Hit_advantage="D"` in the oracle) measuring `kd=0`.

**Mechanism (plpdm.c, engine-grounded).** A landed hit sets the
defender's `routine_no[1]=1` and `routine_no[2]=<raw "kind">` in the same
tick the attacker's contact lands (`dm_reaction_init_set`,
`hitcheck.c:574-589` — `routine_no[2] = as->wu.att.reaction`, the
attacker's own move-data reaction kind). On the defender's own next
`Player_damage` dispatch (`routine_no[3]==0`), `get_damage_reaction_data`
(`plpdm.c:1581-1619`) looks that raw kind up in `dm_reaction_table`
(`plpdm.c:103-121`): `dm_reaction_table[kind].r_no` (`plpdm.c:1618-1619`)
is what's actually stored back into `routine_no[2]` and what
`plpdm_lv_00[32]` (`plpdm.c:157-162`) dispatches on. That single lookup
is the ONLY table the ordinary path passes through — every strike-KD
row in the corpus takes it. A second table, `dd_convert[kind][dm_attlv]`
(`plpdm.c:123-153`, an intermediate value) pre-converts kind BEFORE that
same `dm_reaction_table` lookup runs, but ONLY when `wk->dead_flag` is
set (`plpdm.c:1602-1607`) — i.e. only on the defender's KO'ing blow,
never on the ordinary hits this predicate measures.

`plpdm_lv_00[14]`/`[15]` are `Damage_14000` (slam-down); `[16]`..`[23]`
are `Damage_16000`..`Damage_23000` (buttobi/blown-away launch family, all
running `setup_butt_own_data` + buttobi landing checks) — the measured
down family. `[24]`/`[25]`/`[26]` are excluded: 24 is a "zuru" shake
reaction (`Damage_24000`, `plpdm.c:837-882`, resets `zuru_timer` — a
juggle-scale wobble, not a knockdown); 25 is `kizetsu` stun/faint
(`Damage_25000`, `plpdm.c:884-926`, `grade_add_em_stun`) — both excluded
on SEMANTIC grounds (known not to be knockdowns, independent of census).
26 (`Damage_26000`, `plpdm.c:928-975`) DOES run the same
`setup_butt_own_data` buttobi shape as 16-23/27/28, but is excluded on
CENSUS grounds only: no corpus member (KD or non-KD) has ever traversed
it, so unlike 24/25 its exclusion is an unproven gap, not a confirmed
semantic fact — a future capture landing on 26 needs its own
investigation before either including or ruling it out. `[27]`/`[28]`
(`Damage_27000`/`_28000`, down-state handlers measured only inside
already-KD contexts: airstampede, powerbomb, moonsault) are included.
`[12]`/`[13]` (`Damage_12000` standing hit-reaction, plus
`oki_select_table2`'s quick-stand reuse of 12/13, `plpdm.c:84,1281`) and
`[1]`..`[11]` (stage-1/settling transients) are excluded.

**Measured set: `{14..23} ∪ {27, 28}`** (`fd_r2_is_down_family()`,
`frame_data_overlay.c`). The full block is REQUIRED, not a narrower
"strike-measured-only" subset (e.g. `{16,17,18,19,20,21,23,27}`):
dropping even one member — 14 specifically — would empty lever I's own
signature, since `urien-vkd-lk-hit`'s entire proof that a tick-side latch
is needed rests on 14 being in the set (see below). The narrower fallback
changes NO current flip and only loses future-gap closure on 14/15/22
(14 is measured, but only in one strike + throw contexts) — kept on
record as the rejected alternative, not shipped.

**Two distinct last-cell-sampling failures, one fix.**

1. *Two-stage damage charts.* `get_damage_reaction_data` runs once, but
   some charts later RE-DISPATCH `routine_no[2] = wk->as->data_ix`
   (`plpdm.c:1033`, `:1061`) into a different down-family value than the
   one first assigned. A short-recovery attacker can go idle before that
   re-dispatch happens, so the last raw[] cell still reads the original
   (non-down-family, ordinary-looking) value.
2. *The reverse timing failure* — measured on `urien-vkd-lk-hit`.
   Violence Knee Drop's defender reaction is a DIRECT `kind→r_no=14`
   dispatch (no two-stage detour): the down phase (`Damage_14000`) runs
   and COMPLETES — the defender wakes up and `routine_no[2]` returns to
   the ordinary-looking `1` (a wakeup-tail value, not a fresh hit) —
   BEFORE Urien's own long-recovery attacker animation finally goes idle.
   The last raw[] cell therefore reads `def_r1==1 / def_r2==1`, even
   though the defender spent real mid-window time at `def_r2==14`. This
   corrects an earlier informal framing of this case (attributed to a
   generic "short-recovery attacker interrupts a two-stage chart"
   narrative): `urien-vkd-lk-hit` is NOT a two-stage-chart instance at
   all — it is a direct dispatch whose down phase ends before a
   *long*-recovery attacker idles, the mirror image of failure 1 (there
   the window ends too early to ever reach the down family; here the
   window runs long enough to leave it again), but it breaks the same
   last-cell test.

**Fix (Design E3-L, shipped 2026-07-09).** A sticky per-tick latch
(`FdMove.strike_kd_seen`, latched by `if (fd_strike_kd_latch &&
g_cur.event == FD_OUTCOME_HIT && dn->r1 == 1 &&
fd_r2_is_down_family(dn->r2))` in the tick-side block, right after the
existing defender-idle-return check) samples EVERY tick of the
already-existing finalize-deferred window — the overlay already runs its
tick loop past `attacker_idle` waiting on `defender_idle` on HIT/BLOCK
(the raw-append gate stops growing `raw[]` at `attacker_idle`, but the
surrounding tick-side block, including this latch site, keeps running
every tick until `fd_finalize()` fires) — not just the last counted cell,
at zero new plumbing cost. `fd_is_knockdown_at_atk_idle()` now returns
true if `g_cur.strike_kd_seen` (the latch) OR the last raw[] cell itself
has `def_r1==1` and `def_r2` in the measured set (lever E's anchor,
kept as a genuine, independently load-bearing fallback — see below).

**Residual hazard (documented, not observed in any of the 19 corpora).**
The raw pre-conversion "kind" value written by `dm_reaction_init_set`
(`hitcheck.c:575`) is overlay-visible in `routine_no[2]` for at least one
hitstop tick while `routine_no[1]` is already 1, before
`get_damage_reaction_data`'s conversion overwrites it. Some of
`dd_convert`'s own intermediate entries (e.g. 95/96/39 at
`plpdm.c:132-133`) coincidentally look like they could fall inside the
final r_no range even though they are not it — if a move's raw
pre-conversion kind value ever fell in 14..23 during that window, this
latch would falsely fire. Census-checked 2026-07-09: no such traversal
occurs anywhere in the 19-corpus suite (every observed pre-conversion
sample is outside the down-family range) — a documented hazard for a
future capture, not a present bug.

**Census evidence (complete, all 19 corpora, `kdmask` = set of def r2
values seen with def r1==1 on any tick from the HIT event until
finalize).** Every ordinary HIT window in the suite (~170 rows) traverses
exactly `{12}` — no non-KD row anywhere intersects the down-family set
(a handful of KD-truth multi-hit rows show `12` transiently INSIDE a
genuine KD sequence — harmless, 12 stays excluded). All previously-KD=1
rows keep kd=1 (additive-only: every throw-family row via the untouched
`r1∈{2,3}` test; every already-passing strike-family row samples/
traverses inside the set at the last cell already).

The fix produces exactly the predicted flip set (Appendix A of the
implementation plan; zero regressions, zero unpredicted drift, confirmed
by `tools/frame-data/run-suite.sh --check-golden`):

- **2 XFAIL→PASS** (the shipped predicate's blindness was the entries'
  ONLY divergence): `remy-crfierce-hit` (last raw[] cell traces
  `def_r2==18` — corrects an earlier hand-decode of this trace that had
  misread the cell as `def_r2==1`; the code-emitted `dlr2` diagnostic
  added for this census shows 18), `sean-twdshk-hit` (`def_r2==17`, the
  first captured instance of a value §13.6.2 had explicitly anticipated
  but never captured). Both flip via the widened last-cell set alone
  (lever E) — neither needs the latch, since their own last raw[] cell is
  already inside `{14..23}∪{27,28}`.
- **7 expect kd:0→kd:1** (3 keep an unrelated, pre-existing xfail clause):
  `alex-crfierce-hit`, `yun-forward-hit`, `yun-twdshp-hit`,
  `oro-strong-hit` (plain PASS); `dudley-crroundhouse-hit` (keeps its
  A/R COMPOUND xfail), `urien-crfierce-hit` (keeps its §13.10 Class 1 R
  xfail), `yang-forward-hit` (keeps its S-divergence xfail). This
  dissolves the entire "D-notation-vs-live-kd inconsistency" family
  previously documented across the alex/dudley/urien/yang/yun/oro corpus
  headers: arcade's "D" annotation was right in every case; the shipped
  predicate was simply blind to r2=18 (all seven of these rows'
  down-family value).
- **26 unasserted golden-kd-column updates** (no `expect` change — kd
  stays explicit-only): `alex-airstampede-lk-hit`, `ken-srk-lp-hit`,
  `elena-scratchwheel-lk-hit`, `ibuki-roundhouse-hit`,
  `ibuki-kazekiri-lk-hit`, `dudley-jetup-{lp,mp,hp}-hit`,
  `necro-ragingcobra-lk-hit`, `necro-flyingviper-lp-hit`,
  `oro-oniyama-lp-hit`, `oro-jinchu-lk-hit`, `remy-rrf-lk-hit`,
  `sean-dragonsmash-{lp,mp,hp}-hit`, `urien-vkd-lk-hit`,
  `akuma-srk-lp-hit`, `akuma-tatsu-lk-hit`, `yang-byakko-lp-hit`,
  `yang-senkyuutai-lk-hit`, `yun-tetsu-{mp,hp}-hit`,
  `yun-nishou-{short,forward,rh}-hit`. All are knockdown moves (SRKs,
  tatsus, flash kicks, knee drop, jinchu, etc.); rows already `xfail` on
  an unrelated clause stay `xfail`.
- **0 regressions.**

**Mutation verification.**

- **Lever E** (`fd_r2_is_down_family` forced to return `false` — kills
  the latch and the last-cell clause together, since both now consume
  this one helper): Q corpus FAILs exactly `q-fierce-hit`, `q-crhk-hit`,
  `q-backhk-hit` (kd 0≠1), nothing else; CnDB/Throw kd unaffected (they
  use the untouched `r1∈{2,3}` test). Restored, rebuilt, green.
- **Lever I** (`fd_strike_kd_latch` forced to `0`, the helper's set left
  intact): the ONLY entry affected anywhere in the suite is
  `urien-vkd-lk-hit` (golden-only kd drift 1→0 — not `expect`-asserted,
  so the checker itself flags nothing). This is deliberately a
  single-member signature: every other flip's own last raw[] cell already
  carries a down-family value (17/18/20/21/23/27), so lever E's widened set
  alone covers them; only `urien-vkd-lk-hit`'s down-family traversal
  (`def_r2==14`) is invisible at the last cell, making it the sole
  witness the per-tick latch is load-bearing. Restored, rebuilt, green.

**Set-choice falsification ledger (stop-rule accounting).**

1. *Last-cell set-widening only, no latch* — falsified: misses
   `urien-vkd-lk-hit` (trace-proven mid-window-only traversal). Kept only
   as lever E's anchor clause.
2. *raw[]-history scan* (store every def_r2 seen and check membership at
   finalize) — strictly weaker than the sticky latch (blind to a
   traversal after `attacker_idle` that the deferred-window latch still
   observes live); same set-membership burden either way.
3. *adv-threshold heuristic* — banned by §13.6.2's own STOP-rule
   precedent (an advantage-magnitude cutoff is not an engine-grounded
   signal).

Not yet at the 3-strikes stop limit; the shipped per-tick latch was
chosen directly (options 1-3 above are documented alternatives that were
considered and rejected, not exhausted attempts that forced a fallback).

**SHIPPED 2026-07-09 on `frame-data-on-mister`.** Observer-only: no
gameplay behavior change; the only engine-file additions are write-only
overlay-feed latches of the existing `fd_engine_proj_spawned` class (none
needed here — this fix is entirely inside `frame_data_overlay.c`). See
`docs/plan-frame-data-completion.md`'s ENGINE-3 tracker row and lever
E/I rows, and `docs/plan-frame-data-harness.md` §1.9 item 3's lever I
paragraph.

### 13.7 Open: UOH variance + residual A+1

**Investigation 2026-05-04 (post fresh re-trace, ~5500 FINALs).**
Section retitled from "UOH residual A+1" because the user-reported
"sometimes right sometimes very wrong" UOH variance turns out to
have **four distinct root causes**, only one of which is the +1 the
section originally tracked. Findings cited to file:line, awk recipe,
or specific FINAL annotation per `~/.claude/CLAUDE.md`.

**Trace inventory.** 7 UOH MOVE_STARTs and 7 UOH FINALs in the
current `/tmp/3sx-frame-trace.log` (5800 rows). Recipe:

```sh
grep -nE "MOVE_START.*cghi=229" /tmp/3sx-frame-trace.log
# 7 hits at F=727, 1392, 5202, 5246, 5425, 5469, 5513.
```

There are an additional 7 BLOCK FINALs whose **MOVE_START line shows
`cghi=1 sw_new=0x0020 r2=4`** (single-button MK rising-edge), which
the trace then resolves to UOH on the NEXT tick (F+1 row shows
`cghi=229 r2=10 sw_new=0x0220`). Those 7 + the 7 cghi=229 starts =
14 distinct UOH instances. They cluster into the variance patterns
below.

**Per-FINAL table (all 14 UOH executions, plus the F=1339 anomaly):**

| FINAL F | MOVE_START F | MS cghi | MS sw_new | outcome | S | A | R | T | adv |
| ---: | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 675   | 632   | 1   | 0x0020 | BLOCK | 16 | 11 | 5 | 32 | -4 |
| 769   | 727   | 229 | 0x0220 | BLOCK | 15 | 11 | 5 | 31 | -4 |
| 866   | 823   | 1   | 0x0020 | BLOCK | 16 | 11 | 5 | 32 | -4 |
| 1103  | 1060  | 1   | 0x0020 | BLOCK | 16 | 11 | 5 | 32 | -4 |
| 1152  | 1109  | 1   | 0x0020 | BLOCK | 16 | 11 | 5 | 32 | -4 |
| 1201  | 1158  | 1   | 0x0020 | BLOCK | 16 | 11 | 5 | 32 | -4 |
| 1250  | 1207  | 1   | 0x0020 | BLOCK | 16 | 11 | 5 | 32 | -4 |
| 1339  | 1256  | 1   | 0x0020 | BLOCK | 16 | **22** | 5 | 43 | -4 |
| 1434  | 1392  | 229 | 0x0220 | BLOCK | 15 | 11 | 5 | 31 | -4 |
| 5109  | 5070  | 1   | 0x0020 | HIT   | 16 | 11 | 3 | 30 | +1 |
| 5154  | 5115  | 1   | 0x0020 | HIT   | 16 | 11 | 3 | 30 | +1 |
| 5240  | 5202  | 229 | 0x0220 | HIT   | 15 | 11 | 3 | 29 | +1 |
| 5284  | 5246  | 229 | 0x0220 | HIT   | 15 | 11 | 3 | 29 | +1 |
| 5329  | 5290  | 1   | 0x0020 | HIT   | 16 | 11 | 3 | 30 | +1 |
| 5374  | 5335  | 1   | 0x0020 | HIT   | 16 | 11 | 3 | 30 | +1 |
| 5419  | 5380  | 1   | 0x0020 | HIT   | 16 | 11 | 3 | 30 | +1 |
| 5463  | 5425  | 229 | 0x0220 | HIT   | 15 | 11 | 3 | 29 | +1 |
| 5507  | 5469  | 229 | 0x0220 | HIT   | 15 | 11 | 3 | 29 | +1 |
| 5551  | 5513  | 229 | 0x0220 | HIT   | 15 | 11 | 3 | 29 | +1 |

(Recipe: `awk '/MOVE_START/{ms=$0;mF=$2}/FINAL/{print mF,$0}'` over
the file, filtered by hand to UOH-shape S/A/R numbers.)

**Note on user's prompt examples.** The prompt cites F=1895 with
"S=6 R=6 raw_len=72" as a UOH wild case. Verified at
`/tmp/3sx-frame-trace.log:1895` annotation: outcome=BLOCK S=6 A=11
R=6 T=23 adv=-23 raw_len=72. The MOVE_START preceding that FINAL is
F=1743 with `cghi=172 r2=18 kow=14 sw_new=0x00e8` — that is **HSB
(High Speed Barrage)**, not UOH. cghi 172/173/174/175/177/178/179
/180/182/183 with three sequential `jatix` values 73/74/75 at
F=1749/1763/1779 is HSB Strong's three-active-window signature
(§13.5.2). raw_len=72 is the `FD_METER_LEN` cap
([`frame_data_overlay.c:38-68`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)),
hit because the move's atk_idle_F (3595) is 152 ticks after
move_start_F (3443) which exceeds the 72-cell raw[] capacity. This
is a §13.5.2 / FD_METER_LEN concern, not a UOH variance. No further
analysis here.

---

#### 13.7.1 Pattern 1: A=22 — same-MOVE_START double UOH (F=1339 case)

**Trace evidence (lines 1255-1395).** F=1255 attacker r1=0 (idle).
F=1256 r1: 0→4 fires MOVE_START (cgix=0 cghi=1 sw_new=0x0020). The
overlay opens move tracking; `fd_engine_active_count[atk]` cleared
to 0 at
[`frame_data_overlay.c:569`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).

**First UOH** runs F=1257-1295 (cgix 4→8→12 startup, 20→24→28→32
active with hitstop F=1272-1280, recovery F=1292-1295). cghi
sequence 229→230→231→232→126→127. Engine accumulator at
[`charset.c:414-457`](../src/sf33rd/Source/Game/engine/charset.c)
sums to 11 (walked below in §13.7.4). Cgix-reset at F=1294
(40→0 cghi=234) sets `g_cur.cgix_reset_frame = 1294`
([`frame_data_overlay.c:692`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).
F=1297-1298 cgix=8 cghi=1: §13.5.1 cghi1_first_frame=1297,
cghi1_count=1 then 2. **Predicate's `≥3` gate hasn't fired yet.**

**F=1299: second UOH retriggered mid-r1=4.** Trace row F=1299
shows `cgix=4 cgctr=4 cghi=229 sw_new=0x0220` — player pressed
MP+MK again. Critically, attacker r1 NEVER returned to 0 (no
intervening idle frame between F=1295 and F=1299; r1 stays 4
throughout). The overlay's MOVE_START detector at
[`frame_data_overlay.c:559-561`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
keys on `g_prev[i].r1 == 0 && now[i].r1 != 0` — that edge does
not fire. No second MOVE_START is annotated; no
`fd_engine_active_count[atk]` reset.

Second UOH runs F=1299-1335 with the same cghi 229→230→231→232 active
sequence. Accumulator adds another 11 to engine_a (UOH cell-data is
identical). Cumulative engine_a = 11 + 11 = 22.

Cgix-reset at F=1336 (40→0). `cgix_reset_frame` was already set to
1294 from first UOH; the predicate at line 691 only sets it when
`g_cur.cgix_reset_frame < 0`, so it stays at 1294. Predicate continues
to look for cghi=1 dwell. F=1339-1342: cgix=8 cghi=1 (cghi1_count
increments 3, 4, 5...). On F=1339 (cghi1_count→3) predicate fires
([`frame_data_overlay.c:714-720`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)):
sets `attacker_idle = cghi1_first_frame = 1297` (first UOH's first
cghi=1 frame), trims `raw_len` to `cghi1_first_raw_slot`.

`fd_finalize` runs. `engine_a` is read from
`fd_engine_active_count[atk_idx]`
([`frame_data_overlay.c:411`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c))
which is **22** (uncapped — u8 still has headroom). Numeric line:
S=16 A=22 R=5 T=43.

**Root cause.** `fd_engine_active_count[atk]` is reset only on the
r1: 0→!=0 MOVE_START edge ([`frame_data_overlay.c:569`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c))
and incremented unconditionally per char_move call when `cg_ja.atix
!= 0 || cg_ja.caix != 0` ([`charset.c:421-457`](../src/sf33rd/Source/Game/engine/charset.c)).
Same-r1=4-window retriggered moves bypass the reset.

**Recommended fix.** Add a per-move-segment reset hook: when the
overlay detects a fresh active phase begin (new cgix-reset followed
by `cg_ja.atix != 0` re-entry without intervening r1=0), reset
`fd_engine_active_count[atk_idx] = 0` before the new phase counts.
Or: at finalize, override engine_a by reading the per-raw[] count of
ACTIVE/CONTACT cells over the kept-portion (raw_len after §13.5.1
cut), since the trimmed raw[] only includes the first UOH's active
cells. This is consistent with §13.5.2's HSB approach
([`frame_data_overlay.c:386-402`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
which tracks `last_active_pf_idx`). Note: simply swapping the
engine_a override path for raw[]-derived A would also fix §13.7.4's
+1 below as a side effect, since it sidesteps the cell-data
double-dispatch double-count.

**Phase 3 update (2026-07-07): confirmed ALIVE, then FIXED (§13.9.4).**
`tools/frame-data/corpus-q.yaml`'s `q-uoh-chain-retrigger` entry (`press
MP+MK; wait 38; press MP+MK`) reproduced this pattern deterministically:
a single FINAL, S and R matching arcade, A doubling to 20 (vs the
F=1339 case's A=22 — the exact inflated value is timing-dependent on
the retrigger offset; wait=37/38/39 all land the retrigger while
attacker r1 is still 4, wait≤36/≥40 miss the window). Reran twice,
byte-identical. The `a386e057` `engine_a`-snapshot fix (§13.7.7 rec #2,
§13.9.3) does not cover this case — confirmed by the harness, not just
by code inspection — because there is no intervening `r1=0` edge to
snapshot at. **Fixed by a different mechanism (§13.9.4):** a gated
anchor-time `engine_a` snapshot taken at the cghi=1-dwell anchor itself
(not at `attacker_idle`), displayed only when the cut committed and the
dwell was interrupted. The raw[]-derived-A alternative proposed just
above and in §13.7.5 was tried first and **falsified** — see §13.7.5's
updated status for the falsification detail — before landing on the
anchor-snapshot approach.

---

#### 13.7.2 Pattern 2: S=16 vs S=15 — input-timing variance, not a bug

Two distinct MOVE_START shapes preface UOH:
- `cgix=0 cgctr=3 cghi=1 r2=4 kow=2 sw_new=0x0020` → next-tick row
  resolves to `cgix=4 cghi=229 sw_new=0x0220 r2=10 kow=0`. FINAL
  reports S=16.
- `cgix=4 cgctr=4 cghi=229 r2=10 kow=0 sw_new=0x0220` directly.
  FINAL reports S=15.

The discriminator is `sw_new` on the MOVE_START frame: 0x0020
(MK rising-edge alone) vs 0x0220 (MP+MK rising-edge same tick).
sw_new is the rising-edge mask `cp->sw_new`
([`frame_data_overlay.c:576`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).

When the trace shows `sw_new=0x0020` on the MOVE_START tick, the
engine started transitioning to a state via MK alone (r2=4 cghi=1),
then on the next tick the rising MP edge triggered an in-r1=4
re-resolution to UOH (cghi=229, r2=10). The first frame thus appears
in raw[] as a STARTUP cell that isn't part of the canonical UOH
animation. S becomes 16 instead of 15.

When `sw_new=0x0220` on the MOVE_START tick, MP and MK rising edges
arrived in the same input snapshot, the engine resolved directly to
UOH on the move-start frame. S=15 matches arcade.

**Trace shows X different across captures:** input-timing differs
across captures. Not a defect in the overlay computation — the
overlay is correctly counting visible-state ticks. Arcade's S=15
canonical figure presumes simultaneous MP+MK; one-frame-apart inputs
genuinely produce 16 visible startup frames in this engine.

**Optional fix.** A "MOVE_START re-anchor" could detect when the
engine swaps move identity within the first ~2 ticks (cghi changes
from `1` → a real attack header `≥ 100` while r1 stays 4) and
re-emit MOVE_START / clear raw[]. Trade-off: would also re-anchor
legitimate cancel chains the first 2 ticks. Not recommended unless
arcade-S parity is critical.

---

#### 13.7.3 Pattern 3: HIT R=3 vs BLOCK R=5 — §13.5.1 cut window

**Trace evidence (HIT F=5070-5114, BLOCK F=632-695).** Both follow
the same cgix sequence post-active: 36→40 (cghi=126,127), then
cgix-reset to 0 cghi=234 (cleanup-anim segment), then cgix=8 cghi=1
where §13.5.1's predicate enters its dwell count.

| Phase | BLOCK F=632 | HIT F=5070 |
| --- | ---: | ---: |
| cgix-reset (cghi=234) | F=670 | F=5104 |
| first cghi=1 (post-reset) | F=673 | F=5107 |
| third cghi=1 → predicate fires | F=675 | F=5109 |
| FINAL annotated | F=675 | F=5109 |
| `attacker_idle` | F=673 | F=5107 |
| `move_start_F` (overlay frame) | 1772 | 9253 |
| `atk_idle_F` (overlay frame) | 1813 | 9290 |
| atk_idle - move_start | 41 | 37 |
| `raw_len` after §13.5.1 trim | 33 | 30 |

The atk_idle - move_start difference is **4 ticks shorter on HIT**
(37 vs 41). That accounts for the 2-frame R difference (raw_len=30
vs 33, engine_a fixed at 11, S fixed at 15-16, so R=raw_len -
first_active_raw - active_pf — see
[`frame_data_overlay.c:372-401`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)).

**Why is HIT shorter than BLOCK?** Defender state. On BLOCK the
defender's r1=1 blockstun runs longer than r1=1 hitstun for HIT (per
the engine's hitstun tables). The attacker stays in r1=4 either way,
but the §13.5.1 cleanup-anim segment runs for fewer frames before
the cghi=1 dwell stabilizes when the engine sees the defender exit
hitstun earlier. Verified: trace column 37 (P2.r1) returns to 0 at
F=5107 (HIT) vs F=695 (BLOCK), and the cghi-segment-transition rows
(cghi 126→127→234→1) compress from 6 ticks (BLOCK F=668-673) to
4 ticks (HIT F=5104-5107).

**Arcade R=5 expectation.** Arcade frame-data tables canonicalize R
against block hit-stop / block-stun, not whiff/hit. BLOCK R=5
matches arcade. HIT R=3 is arcade-conventionally-correct in that the
opponent recovers from hitstun 2 frames before from blockstun, so
the attacker's actionable-window appears 2 frames earlier relative
to defender returning to neutral. Not a defect; just a different
canonical figure.

**Recommended action.** None. Document the HIT R=3 / BLOCK R=5 gap
as expected per defender-state difference. Update §13.5.1 to note
that R varies between HIT and BLOCK due to the cleanup-anim window
shortening on HIT; both are correct in their respective contexts.

**Phase 3 confirmation (2026-07-07).** The harness reproduces this
exactly: `q-uoh-samef-hit`/`q-uoh-1f-hit` show R=3 vs `q.json`'s 5
(xfail, citing this section). **This section's mechanism is now also
cited as the explanation for one member of a broader new HIT-R
divergence family** (§12 "Q specials sampled" table, new open item):
HSB (all 3 strengths), Dashing Leg RH, and Throw also show HIT-outcome
R diverging from their WHIFF/BLOCK-matching `q.json` figure, in their
first-ever clean HIT captures. Whether those moves share this exact
cleanup-anim-window mechanism or need separate per-move analysis is
not yet determined — flagged in §13.8's open-issue table.

**Phase 4 formula-test exclusion (2026-07-07).** When Phase 4 items
4+5 investigated the CnDB R+1 bug, the fix candidate (re-deriving R as
`raw_len - S - engine_a`) was tested against this HIT-R divergence
family as a scope check. UOH: formula gives `29 - 15 - 10 = 4` (S=15
variant) vs arcade R=5 — **fails the formula test by 1 frame**, unlike
CnDB where the same formula lands exactly on arcade. This confirms
UOH's R=3 (measured) is a *different* mechanism from CnDB's boundary
error (this section's hitstun-vs-blockstun cut-window explanation
above remains the correct one for UOH) — the two families only look
similar (HIT R below WHIFF/BLOCK R) on the surface. UOH is explicitly
**out of scope** for the Phase 4 items 4+5 fix; it stays xfail with
this section as the cited mechanism.

**Generalized and adopted as display convention in §13.10
(2026-07-07)**; UOH corpus HIT entries now assert R=3 per that
convention.

---

#### 13.7.4 Pattern 4: residual +1 on clean UOH (engine_a=11 vs arcade A=10)

**Original §13.7 question, now answered with `[CM]` log evidence.**

Walking the F=727 BLOCK UOH (`/tmp/cm-trace.log` GT=1574-1611) using
the accumulator semantics at
[`charset.c:414-457`](../src/sf33rd/Source/Game/engine/charset.c):

```
GT=1589 (call#1): cgix=16 cgctr=1 jatix=54 cgatt=14 athok=1
                  prev_cgix=12 (last seen). cgix_change → add=ctr=1.
                  prev_cgix := 16. running=1.
GT=1589 (call#2): cgix=20 cgctr=2 jatix=54 cgatt=0 athok=0
                  cgix_change 16→20 → add=ctr=2.
                  prev_cgix := 20. running=3.
GT=1598 (post-hitstop): cgix=20 cgctr=1. same cgix → add=0.
GT=1601: cgix=24 cgctr=2 → add=2. running=5.
GT=1602: cgix=24 cgctr=1. same cgix → add=0.
GT=1603: cgix=28 cgctr=3 → add=3. running=8.
GT=1604: cgix=28 cgctr=2. add=0.
GT=1605: cgix=28 cgctr=1. add=0.
GT=1606: cgix=32 cgctr=3 → add=3. running=11.
GT=1607: cgix=32 cgctr=2. add=0.
GT=1608: cgix=32 cgctr=1. add=0.
                                      Final engine_a = 11.
```

Five cells contribute (cgix 16, 20, 24, 28, 32), summing 1+2+2+3+3 =
**11**. The `[CM]` log is conclusive — `awk '/^\[CM\] / && $2 ==
"GT=1589"' /tmp/cm-trace.log` shows two char_move calls in the same
game tick at GT=1589, with cgix=16 and cgix=20 respectively.

**Root cause: cell-data double-dispatch.** At cgix=16 cgctr=1, the
cell-data extended-code path
([`charset.c:473-509`](../src/sf33rd/Source/Game/engine/charset.c))
loaded a 1-tick cell with jatix=54 already set. The first char_move
call this tick (from one of the call sites at
[`charset.c:101, 190, 234, 242, 262`](../src/sf33rd/Source/Game/engine/charset.c))
ran the accumulator on cgix=16 (add=1, prev_cgix=16). The second
char_move call this tick (from another site) pre-decremented cgctr
1→0, dispatched cell-data again, advanced cgix=16→20 cgctr=2, and
ran the accumulator (add=2, prev_cgix=20). cgix=16 had jatix=54 in
its cell data but never persists for any visible frame — by the end
of the tick, the engine state shows cgix=20.

The trace data row at F=1272 (the first frame the player sees the
active phase) reflects only cgix=20 — cgix=16 is invisible to the
trace's per-tick row emission since the row is captured AFTER all
char_move calls have run. The accumulator, by contrast, runs INSIDE
each char_move and counts cgix=16 once.

Visible active phase: cgix=20 (cgctr=2, lasts 2 ticks post-hitstop)
+ cgix=24 (2) + cgix=28 (3) + cgix=32 (3) = **10 ticks visible**.
Arcade A=10 matches. Engine accumulator counts the invisible cgix=16
extra → engine_a=11.

**Hypothesis #5 from §13.7.1 (third cell contributes 3 not 4).
Disproven.** No cell contributes "wrong"; all five cells (including
the invisible cgix=16) contribute their loaded cgctr values
correctly. The bug is that cgix=16 should not have contributed at
all — its 1 tick of "duration" was consumed by the same-tick cell
advance.

**Recommended fix — SHIPPED in commit `a386e057`, CONFIRMED WORKING by
Phase 3 harness (2026-07-07).** Verified at
[`charset.c:432-451`](../src/sf33rd/Source/Game/engine/charset.c):
detects same-tick cgix advance via `fd_prev_active_cgix_tick[2]` /
`fd_prev_active_cgix_add[2]`, exactly as proposed below, plus one
refinement not in the original proposal — the revoke is additionally
gated on matching `jatix` (`fd_prev_active_cgix_jatix[2]`) so that
cells with genuinely distinct hitboxes (cr.MK cgix=12→16 jatix=24→25,
Far Forward jatix=11→12) are never revoked, only same-hitbox
same-tick transits (Q's UOH cgix=16→20). **Confirmed 2026-07-07:** the
Phase 1 harness's `q-uoh-samef-block`/`q-uoh-1f-block` entries show
clean isolated UOH at the arcade-exact A=10 (plain PASS, not xfail).
The mutation test (`docs/plan-frame-data-harness.md` §1.9.3) disables
this revoke and confirms it flips exactly those two entries to FAIL
(A=11), then restores to green — this is the strongest possible
confirmation that the shipped fix is load-bearing and working.
Original proposal preserved below for the historical mechanism
description (the shipped code matches it, plus the jatix gate):

```c
if (wk->cg_ix != fd_prev_active_cgix[wk->id]) {
    if (fd_prev_active_cgix_tick[wk->id] == Game_timer
        && fd_prev_active_cgix_add[wk->id] > 0) {
        // previous cgix was a same-tick transit; revoke its contribution
        fd_engine_active_count[wk->id] -= fd_prev_active_cgix_add[wk->id];
    }
    s16 ctr = wk->cg_ctr;
    /* ...existing clamp... */
    add = ctr;
    fd_prev_active_cgix[wk->id] = wk->cg_ix;
    fd_prev_active_cgix_tick[wk->id] = Game_timer;
    fd_prev_active_cgix_add[wk->id] = ctr;
}
```

Alternative (cleaner): switch the finalize A source from
`fd_engine_active_count[atk]` to a count of ACTIVE/CONTACT cells in
raw[] over the trimmed range, as proposed in §13.7.1's fix for the
A=22 case. raw[] is captured per-trace-tick (post all char_move
calls), so cgix=16's invisible existence is naturally excluded.
Single-source fix for both Pattern 1 (A=22) and Pattern 4 (A+1).
Trade-off: loses the engine-canonical-arcade-A property for moves
where raw[] under-counts due to hitstop / sub-frame attacks
(§9-§13.4 motivation for engine_a). Mitigation: keep engine_a as the
default and use raw[]-derived A only when the §13.5.1 cut has
trimmed raw_len (i.e., multi-segment moves). UOH and HSB both fall
into that branch; standard normals don't.

**UPDATE (2026-07-07, §13.11 declared-truth displayed-A convention
adopted).** The revoke's arithmetic effect on displayed A is now
cancelled: immediately after the subtract above, a gated restore
(`fd_restore_revoked_declared_credit`, `charset.c`) adds the same
`prior_add` back into `fd_engine_active_count`, so the net effect on
the displayed value is zero. The block above is retained byte-
identical — its subtract, its tick/add/jatix bookkeeping, all of it —
so the revoke's history and its mutation lever remain inspectable;
only the restore is new. The revoke's load-bearing mutation-test role
moves from lever A (this section's "mutation test... disables this
revoke" paragraph above) to the new lever F (docs/plan-frame-data-
harness.md §1.9 item 3): forcing the *restore* off now reproduces the
pre-restore undercount, while forcing the *revoke* off (lever A) is
expected to change nothing (subtract-then-restore is arithmetically
identical to never subtracting, for every corpus window measured —
see §13.11). Clean UOH now displays the convention's declared 11 (not
arcade's 10) by design — see §13.11 for the full rationale and the
industry-precedent argument for treating declared credit, not the
arcade table, as ground truth for A.

---

#### 13.7.5 Status

All four patterns remain **OPEN** as of 2026-05-04 EOD. Two fix
attempts were shipped during the day and reverted; see §13.9 for the
attempt history.

| Pattern | Cause | Severity | State | Recommended fix |
| --- | --- | --- | --- | --- |
| A=22 (F=1339) | Same-r1=4 double UOH; engine_a not reset between back-to-back UOH because no `r1: 0→4` edge fires (r1 stays 4) | High | **SHIPPED (§13.9.4) — CONFIRMED FIXED by Phase 3 harness, 2026-07-07.** Two earlier fix attempts remain reverted (§13.9.1/§13.9.2); this is a third, distinct approach. | Gated anchor-time `engine_a` snapshot (§13.9.4), NOT the raw[]-derived-count alternative in this row's own "Recommended fix" column — that was tried and **falsified**: raw[]-derived `active_pf` = 13 for this retrigger and for both clean-UOH BLOCK entries, never 10. See §13.9.4 for the mechanism and falsification detail. |
| S=16 (cghi=1 sw_new=0x0020 starts) | Input timing one-frame-apart MK→MP | Low | OPEN — documented as expected per timing | None recommended; optional MOVE_START re-anchor would have side effects. |
| HIT R=3 vs BLOCK R=5 | Defender hitstun < blockstun shortens cleanup-anim window | None | Documented as expected behavior | No fix; both numbers are arcade-conventionally correct in their respective contexts. |
| Clean UOH A=11 vs arcade 10 | cgix=16 cell-data double-dispatch counts an invisible 1-tick cell | Medium — engine-vs-arcade convention drift | **SHIPPED (`a386e057`) — CONFIRMED FIXED by Phase 3 harness, 2026-07-07** | Jatix-gated same-tick cgix-transit revoke shipped at `charset.c:432-451` (see updated §13.7.4 note above). The cgix=16 cell is **invisible to per-tick trace rows** (which capture state AFTER all char_move calls have run) but visible to the in-tick accumulator — a structural engine-vs-arcade convention drift; the fix subtracts the phantom contribution instead of switching numeric sources. Harness confirms A=10 exact + mutation test flags exactly this fix when disabled. |

**Single-fix path that addresses Pattern 1 + Pattern 4 simultaneously —
PROPOSED THEN FALSIFIED (2026-07-07), do not re-ship.** The original
idea: in `fd_finalize` at
[`frame_data_overlay.c:411`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c),
override `engine_a` with the per-frame ACTIVE/CONTACT count of raw[]
when `g_cur.cghi1_first_frame >= 0` (i.e., §13.5.1 cut fired). Tested
during the §13.9.4 fix's diagnosis and **falsified**: the trimmed-raw[]
ACTIVE/CONTACT count (`active_pf`) is **13** for the F=1339-style
retrigger, and **also 13** for both already-PASSing clean-UOH BLOCK
entries (`q-uoh-samef-block`, `q-uoh-1f-block`) — never 10. Root cause:
post-hitstop, cgix=20 stays on screen 4 ticks past its 2-tick cgctr
budget (a cg_ctr-sticking artifact distinct from the §13.7.4 phantom
cgix=16 cell), so 12 post-hitstop visible ticks + 1 contact = 13.
`active_pf` (visible on-screen ticks) and `engine_a` (cg_ctr-accumulator
lump sum) are **structurally different countings even in the passing
case** — the shipped §13.9.4 fix instead snapshots `engine_a` itself
at the cghi=1-dwell anchor, never switching to the raw[]-derived
quantity. The bullets below describing the (never-shipped) raw[]
override's hypothetical effect are preserved for historical context
only — they do NOT match the harness's measured `active_pf` values:
- F=1339 (A=22): the *hypothesized* effect was raw[] trimmed to first
  UOH only → A=10. Not what was measured (see above).
- F=727/769 etc. (clean A=11): *hypothesized* raw[] never saw cgix=16
  → A=10. Measured `active_pf`=13, not 10, for the equivalent harness
  entries.
- HSB / CnDB (already use a different code path): unaffected either way.

Per `feedback-trust-user-actions.md`: the user reliably reported
"sometimes right sometimes very wrong" — that maps directly to
Patterns 1+2 (variance in S and A across captures), with Patterns
3+4 being the persistent residuals. Trace confirms the variance is
real and engine-deterministic given identical inputs; it is not
reporter error.

#### 13.7.6 Superseded prior investigation (2026-05-04 earlier pass)

Earlier today an investigation attempt concluded that "the data
needed to walk the +1 source is not present in the captured trace"
because at that time `/tmp/3sx-frame-trace.log` ended at F=1159 with
no `cghi=445`-shaped UOH execution. That conclusion was correct for
the trace as it existed then but is now superseded by the post
re-trace analysis above (5800-row trace with 17 UOH FINALs). The
earlier hypothesis "third cell contributes only 3 of 4 frames due
to jatix dropping mid-cell" is **disproven** — the `[CM]` walk in
§13.7.4 shows all five active cells (cgix 16, 20, 24, 28, 32)
contribute their loaded cgctr values exactly; the +1 comes from the
otherwise-invisible cgix=16 transit cell.

A separate detail from the earlier pass: the original §13.7 cited
`cghi=445` as the UOH animation header. Post re-trace verifies the
header is `cghi=229` (entry) → `230` → `231` → `232` (active) →
`126` → `127` (recovery) → `234` → `1` (cleanup-anim). `cghi=445`
does not appear in the current trace; the earlier number was
likely from a stale binary or a different character map.

#### 13.7.7 Comprehensive UOH variance audit (2026-05-04, late pass)

**Scope.** Every UOH FINAL in the current `/tmp/3sx-frame-trace.log`
(102 FINAL annotations, 5800+ data rows) re-walked from raw evidence.
Goal: account for the user's report that "sometimes s=16, sometimes
s=15; sometimes s and r were 0; sometimes it was slightly different
in other ways." Prior §13.7.1–13.7.6 found four root causes for
**clean** UOH variance; this pass adds a fifth (multi-move merge) and
disambiguates the user's "S=0 R=0" cases (they're parry-attacks, not
UOH).

**Identification recipe.** A FINAL is UOH if its preceding MOVE_START
shows either:
- `cghi=229` directly (player input MP+MK arrived in same input
  snapshot — sw_new=0x0220), OR
- `cghi=1 r2=4 sw_new=0x0020` AND the next-tick data row resolves
  to `cghi=229 r2=10 sw_new=0x0220` (MK rising-edge first, MP
  rising-edge one tick later).

The cghi=1+sw_new=0x0020 prefix is ambiguous in isolation — F=46,
F=3864, F=3909 in the trace start with the same input shape but
resolve to close MK (`cghi` stays 1, never reaches 229) and produce
S=12 A=5 R=19 FINALs (close MK arcade S=5 A=3 R=14 — separate move).
Verified per next-tick check (awk over the cghi=1 sw_new=0x0020
MOVE_START list).

**Master inventory: 19 UOH FINALs, 19 UOH MOVE_STARTs (paired).**

| MOVE_START F | MS cghi | MS sw_new | FINAL F | outcome | S | A | R | T | adv | engine_a | raw_len | first_active_raw | event_raw |
| ---: | ---: | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 632   | 1   | 0x0020 | 675   | BLOCK | 16 | 11 | 5 | 32 | -4 | 11 | 34 | 16 | 16 |
| 727   | 229 | 0x0220 | 769   | BLOCK | 15 | 11 | 5 | 31 | -4 | 11 | 33 | 15 | 15 |
| 823   | 1   | 0x0020 | 866   | BLOCK | 16 | 11 | 5 | 32 | -4 | 11 | 34 | 16 | 16 |
| 1060  | 1   | 0x0020 | 1103  | BLOCK | 16 | 11 | 5 | 32 | -4 | 11 | 34 | 16 | 16 |
| 1109  | 1   | 0x0020 | 1152  | BLOCK | 16 | 11 | 5 | 32 | -4 | 11 | 34 | 16 | 16 |
| 1158  | 1   | 0x0020 | 1201  | BLOCK | 16 | 11 | 5 | 32 | -4 | 11 | 34 | 16 | 16 |
| 1207  | 1   | 0x0020 | 1250  | BLOCK | 16 | 11 | 5 | 32 | -4 | 11 | 34 | 16 | 16 |
| 1256  | 1   | 0x0020 | 1339  | BLOCK | 16 | **22** | 5 | 43 | -4 | **22** | 34 | 16 | 16 |
| 1392  | 229 | 0x0220 | 1434  | BLOCK | 15 | 11 | 5 | 31 | -4 | 11 | 33 | 15 | 15 |
| 5070  | 1   | 0x0020 | 5109  | HIT   | 16 | 11 | 3 | 30 | +1 | 11 | 30 | 16 | 16 |
| 5115  | 1   | 0x0020 | 5154  | HIT   | 16 | 11 | 3 | 30 | +1 | 11 | 30 | 16 | 16 |
| 5202  | 229 | 0x0220 | 5240  | HIT   | 15 | 11 | 3 | 29 | +1 | 11 | 29 | 15 | 15 |
| 5246  | 229 | 0x0220 | 5284  | HIT   | 15 | 11 | 3 | 29 | +1 | 11 | 29 | 15 | 15 |
| 5290  | 1   | 0x0020 | 5329  | HIT   | 16 | 11 | 3 | 30 | +1 | 11 | 30 | 16 | 16 |
| 5335  | 1   | 0x0020 | 5374  | HIT   | 16 | 11 | 3 | 30 | +1 | 11 | 30 | 16 | 16 |
| 5380  | 1   | 0x0020 | 5419  | HIT   | 16 | 11 | 3 | 30 | +1 | 11 | 30 | 16 | 16 |
| 5425  | 229 | 0x0220 | 5463  | HIT   | 15 | 11 | 3 | 29 | +1 | 11 | 29 | 15 | 15 |
| 5469  | 229 | 0x0220 | 5507  | HIT   | 15 | 11 | 3 | 29 | +1 | 11 | 29 | 15 | 15 |
| 5513  | 229 | 0x0220 | 5551  | HIT   | 15 | 11 | 3 | 29 | +1 | 11 | 29 | 15 | 15 |

(Recipes:
- UOH MOVE_STARTs with cghi=229: `grep -E '^# F=' /tmp/3sx-frame-trace.log | grep -E 'MOVE_START.*cghi=229'` — 7 hits.
- Ambiguous cghi=1 starts: `grep -E '^# F=' /tmp/3sx-frame-trace.log | grep -E 'MOVE_START.*cghi=1 .*sw_new=0x0020'` — 15 hits.
- Of the 15, three (F=46, F=3864, F=3909) stay close-MK on next tick;
  the other 12 resolve to UOH cghi=229 next tick (verified per-MOVE_START
  by an awk lookup of GT+1 row's cghi field).
- Total UOH MOVE_STARTs = 7 + 12 = 19; UOH FINALs by shape match exactly.)

**Cluster summary.**

| Cluster | Count | Arcade S/A/R | Trace S/A/R | Variance source |
| --- | ---: | --- | --- | --- |
| Clean UOH BLOCK, sw_new=0x0220 | 2 | 15/10/5 | 15/11/5 | A+1 only (§13.7.4) |
| Clean UOH BLOCK, sw_new=0x0020 → 0x0220 | 6 | 15/10/5 | 16/11/5 | S+1 from input timing (§13.7.2) + A+1 (§13.7.4) |
| Clean UOH HIT, sw_new=0x0220 | 5 | 15/10/5 | 15/11/3 | A+1 + R−2 from defender hitstun < blockstun (§13.7.3) |
| Clean UOH HIT, sw_new=0x0020 → 0x0220 | 5 | 15/10/5 | 16/11/3 | S+1 + A+1 + R−2 |
| Anomaly: A=22 retrigger | 1 | — | 16/22/5 | §13.7.1 same-r1=4 double UOH |
| **Total UOH** | **19** | | | |

**No "S=0 R=0" UOH FINAL exists in the trace.** The R=0 atk=0 FINALs
the user observed are all **parry-counter attacks**, not UOH:

| FINAL F | S | A | R | T | MS F | MS cghi | MS r2 | MS pat | MS sw_new | MS kow | move identity |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: | --- |
| 4779 | 2 | 11 | 0 | 13 | 4754 | 102 | 3 | 20 | 0x0010 | 0 | parry-counter LP (high parry) |
| 4815 | 7 | 6  | 0 | 13 | 4785 | 105 | 3 | 22 | 0x0020 | 2 | parry-counter MP (high parry) |
| 4940 | 10 | **18** | 0 | 28 | 4817 | 113 | 3 | 20 | 0x0040 | 4 | parry-counter HP — merged with second move (see below) |
| 4962 | 4 | 10 | 0 | 14 | 4941 | 122 | 3 | 22 | 0x0100 | 1 | parry-counter LK (low parry) |
| 4996 | 9 | 8  | 0 | 17 | 4964 | 129 | 3 | 22 | 0x0200 | 3 | parry-counter MK (low parry) |
| 5024 | 27 | 0 | 0 | 27 | 4997 | 137 | 3 | 22 | 0x0400 | 5 | parry-counter HK whiff (no jatix/h_att signal) |
| 5068 | 17 | 3 | 7 | 27 | 5025 | 137 | 3 | 22 | 0x0400 | 5 | parry-counter HK (post-cut, found 3 cells with jatix/cg_att) |

**Move identity (corrected 2026-05-05).** These are LP/MP/HP/LK/MK/HK
normals launched from the **forward-walk state** (`r2=3` is set by
`check_F_R_walk` lever_dir=1 at
[`pls01.c:476`](../src/sf33rd/Source/Game/engine/pls01.c)). They MAY
be parry-counters by gameplay convention (a forward-walk-buffered
attack can come out of parry-recovery's cancel-timer window per
[`pls00.c:1939-1994`](../src/sf33rd/Source/Game/engine/pls00.c)), but
distinguishing "post-parry counter" from "regular forward-walking
attack" requires a parry-event signal in the trace that is not
currently captured. The earlier prose's `pls01.c:251` citation is
wrong — that line is `check_sankaku_tobi` (wall-jump). `pat_status`
20/22/24/26 are *character-specific cgd cell-data* values loaded at
[`charset.c:557`](../src/sf33rd/Source/Game/engine/charset.c), not
universal parry-stance flags.

Trace-side identifiers:
- `r2 == 3` on MOVE_START → forward-walk carry-over.
- `cghi` in 102–143 range → Q's character-specific normal-attack
  animation headers initiated from forward-walk.
- `kow` 0/2/4/1/3/5 → strength variant (LP/MP/HP/LK/MK/HK) per the
  reference table at the end of §13.7.

The user's "sometimes s and r were 0" report describes parry-counter
FINALs intermixed with UOH in their session, not UOH itself. The
overlay does not currently distinguish parry-counter from a normal
attack — it shows S/A/R for whatever r1=4 phase finalizes — so the
parry-counter numbers (S=2..27 A=0..18 R=0) display alongside UOH
numbers (S=15-16 A=11 R=3-5) in the same training session.

Per `feedback-trust-user-actions.md`: the user did perform UOH; they
also performed parry-counters in the same session, and the overlay's
output for the parry-counter moves looks "very different" from UOH.
The trace doesn't show wrong UOH numbers; it shows parry-counter
numbers that happen to be wildly different from UOH numbers and that
the overlay didn't visually distinguish.

**Per-anomaly walks.**

##### F=1339 A=22 (UOH) — same-r1=4 double UOH retrigger
Already root-caused in §13.7.1. Mechanism: player pressed UOH twice
without P1 returning to r1=0 between attempts; overlay's MOVE_START
edge (`g_prev[i].r1 == 0 && now[i].r1 != 0` —
[`frame_data_overlay.c:559-561`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c))
never fires for the second UOH; engine accumulator
(`charset.c:421-457`) keeps adding active-cell cgctrs for the second
UOH on top of the first. Confirmed by walking trace F=1255-1395 —
r1 stays 4 throughout, two distinct cgix=4→8→12→20→24→28→32 active
sequences run back-to-back, engine_a goes 11+11=22.

##### F=4940 A=18 (parry-counter HP) — multi-move merge, NEW failure mode
**Trace evidence.** awk on data rows F=4810–4945:
```
F=4810 P1.r1=4 P2.r1=1
F=4815 P1.r1=4 P2.r1=0   (P2 exits hitstun — close MK BLOCK ended)
F=4816 P1.r1=0 P2.r1=0   (P1 enters idle — overlay sets attacker_idle for cghi=105)
                          (close MK FINAL emitted at F=4815)
F=4817 P1.r1=4 P2.r1=0   (NEW MOVE_START annotated, cghi=113 parry-counter HP #1)
F=4827 P1.r1=4 P2.r1=1   (parry-counter HP #1 connects, P2 enters blockstun)
F=4844 P1.r1=0 P2.r1=1   (P1 returns to idle — attacker_idle sets, but
                          defender_idle still pending because P2.r1=1)
F=4889-4904 P1.r1=0 with cghi=277/455/456 (Q in dash/walk/etc.)
F=4919 P1.r1=4 P2.r1=1   (NEW parry-counter HP #2, cghi=122 — but
                          MOVE_START annotation MISSING because g_cur.active
                          is still true from #1; line 559 `if (!g_cur.active)`
                          gate suppresses the new MOVE_START)
F=4936 cgix=36 cghi=125 jatix=37 (parry-counter #2's active phase)
F=4937 cgix=0 cghi=1 (cleanup-anim segment begins)
F=4940 P1.r1=0 P2.r1=0   (defender_idle finally fires; FINAL emitted)
```

**Why g_cur.active stayed true.** [`frame_data_overlay.c:830-838`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
finalizes only when `attacker_idle >= 0` AND `defender_idle >= 0` for
HIT/BLOCK outcomes. attacker_idle was set at F=4844 but P2 stayed in
r1=1 (blockstun from parry-counter #1) until F=4940, so finalize
deferred. During that interval (F=4844-4939) the engine accumulator
kept incrementing (`charset.c:421-457` runs unconditionally on
`cg_ja.atix != 0 || cg_ja.caix != 0` regardless of overlay state),
adding parry-counter #2's active cells (cgix=32 add=5, cgix=36 add=4)
on top of #1's (cgix=24 add=3, cgix=28 add=3) plus likely-invisible
cgix=20 transit cell add≈3 (§13.7.4 mechanism). Total engine_a = 18,
matching the FINAL annotation.

**Why raw_len stayed at 16 despite 18 engine-active frames.**
[`frame_data_overlay.c:740-742`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
gates raw[] appends on `!attacker_already_idle`. After F=4844
attacker_idle ≥ 0; raw[] froze at 16 cells. raw[] does not see
parry-counter #2 at all. Hence raw_len < engine_a — a structural
inconsistency between the two sources of truth.

**Why R=0.** §13.5.1 cut fired on the FIRST cghi=1-after-cgix-reset
trio. cgix-reset happened at F=4805 (during cghi=105 cleanup —
inherited state because no MOVE_START fired for cghi=113). cghi=1
at F=4805/4806/4807/4809/4812/4815 satisfied the ≥3 dwell —
attacker_idle gets clobbered to F=4805's local frame, raw_len
trimmed. Then PASS through that, the engine kept running and the
data trace continued.

Actually, on closer inspection: `attacker_idle < 0` is checked at
[`frame_data_overlay.c:685`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
before the §13.5.1 cut block. So the cut only fires while
attacker_idle < 0. The R=0 here came from a different path: at
F=4844 the r1=4→0 edge ran and set attacker_idle = g_local_frame,
then raw_len froze. T=28 = atk_idle - move_start = 8754-8730 = 24
overlay frames; trace 21 trace-frames. Recovery_pf in finalize is
counted as cells in raw[] AFTER the last ACTIVE/CONTACT cell index.
Last ACTIVE cell index = 13 (from first parry-counter), raw_len = 16,
so recovery_pf = 16 - 14 = 2... but R=0 reported. The discrepancy is
because finalize uses engine_a as A which subsumes more cells than
exist in raw[], pushing R past the available range — finalize math
clamps the result to ≥ 0.

Cleaner derivation: in [`frame_data_overlay.c:380-401`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
finalize computes `recovery = raw_len - first_active_raw - active`
where active = engine_a. With raw_len=16, first_active_raw=10,
engine_a=18: 16 - 10 - 18 = -12 → clamps to 0. Hence R=0.

**Why S=10.** first_active_raw=10. The first 10 cells of raw[] (which
captured F=4817-4826 in trace) showed cghi=113-116 jatix=0 — pre-active
cells. raw[10] was the first cell with active signal (F=4827 cgix=24
jatix=34). Finalize sets S = first_active_raw = 10.

##### F=5024 S=27 A=0 R=0 (parry-counter HK whiff) — no engine active signal
**Trace evidence.** awk F=4997-5024 shows cgix=4→8→12→16→20→24→28
with cghi=137-143 jatix=0, cgatt=0, athok=0, hatt=0 throughout. The
parry-counter HK animation has no active hitbox set in the engine
state captured by `fd_snap_player`. Result: no ACTIVE cell, no
event_this_frame; finalize classifies all 27 raw[] cells as STARTUP.
A=0 R=0 by construction.

This may be a real engine quirk (parry-counter HK uses a different
hitbox path that bypasses cg_ja.atix / cg_att_ix / h_att — possibly
projectile-style), or it may be a snapshot gap. Out of scope for the
UOH variance audit; flagged here as a finding to revisit if parry-
counter coverage becomes a goal.

##### F=5068 S=17 A=3 R=7 (parry-counter HK contact) — second attempt connected
Same MS shape as F=5024 (cghi=137 r2=3 pat=22 sw_new=0x0400 kow=5),
but this attempt's data rows show jatix=37/38 firing on cgix=24/28
(non-zero contact path). Finalize counts 3 active cells via raw[],
gives S=17 A=3 R=7. Confirms the F=5024 path is move-state-dependent,
not a permanent classifier blind spot.

##### atk=1 FINAL entries — P2 dummy idle/jumping, NOT UOH
The trace contains 18 atk=1 FINAL entries with shape `S=14-15 A=0 R=0
T=14-15 outcome=WHIFF use_hatt=0 raw_len=14-15 first_active_raw=-1
event_raw=-1`, with MOVE_START fields `r2=5` (or `r2=37`) and
`cghi=145/146`. Examples at trace lines 749/771, 848/870, 947/969,
997/1019, 1046/1068, 1095/1117, 1359/1380, 5173/5195, 5569/5591,
5609/5631, 5652/5674, 5693/5715, 5732/5754, 5773/5795.

These are P2 (the training dummy) entering r1=1 (defender / reaction
state) for blockstun/hitstun/jump-recovery animations. The overlay's
MOVE_START detector at
[`frame_data_overlay.c:559-561`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
fires on r1: 0 → !=0 *for either player*; defender's r1=1 entry
satisfies the condition. Subsequently the overlay tracks "P2's move"
which is just blockstun, finds no active hitbox (hence A=0), and
finalizes as WHIFF.

Confirmation that these are NOT UOH:
- atk=1 means P2 is the "attacker" — but P2 is the dummy in this
  training session.
- char=2 (Ken's id; the trace's P2 is configured to a different
  character than P1's char=17 = Q).
- r2=5/37 with cghi=145/146 are not Q's UOH cghi=229.

These atk=1 entries spam the FINAL stream but contribute no signal
to UOH analysis. They could be filtered out by a future improvement
that gates MOVE_START on attacker-initiated transitions only (e.g.,
ignore r1: 0→1 since r1=1 is a defender state, not an attack), but
that's outside §13.7's scope.

**Summary of UOH variance — all causes accounted for.**

| Pattern | Count in trace | Cause | Fix path |
| --- | ---: | --- | --- |
| Clean UOH (S=15/16 A=11 R=3/5) | 18 | A+1 from §13.7.4 cgix=16 transit; S+1 from §13.7.2 input timing; R=3 vs R=5 from §13.7.3 hitstun<blockstun | §13.7.5's raw[]-derived A path resolves the +1; S+1/R-2 are documented as expected per timing/state |
| A=22 (F=1339) | 1 | §13.7.1 same-r1=4 retrigger; engine_a not reset between back-to-back UOH | Same raw[]-derived A path resolves it (raw[] capped to first UOH only) |
| A=18 (F=4940, parry-counter HP) | 1 (parry, not UOH) | NEW: g_cur.active gate stays true during long defender_idle wait; new MOVE_START suppressed; engine accumulator captures merged moves | Reset `fd_engine_active_count[atk]` whenever the overlay observes a fresh attacker r1: 0→4 edge, even if g_cur.active is still true. OR finalize when attacker_idle is set (don't wait for defender_idle for the engine_a snapshot — read it earlier and freeze it). OR same raw[]-derived A path applies. |
| Parry-counter R=0 / S small (F=4779, 4815, 4962, 4996, 5068) | 5 (parry, not UOH) | Parry-counter animations are short (cghi 102-143, total 13-17 frames); §13.5.1 cut fires aggressively because cghi=1 dwell post-cleanup is short. Numbers are not "wrong" — they correctly describe the parry-counter move; user-perception "wrong" is from confusing parry-counter S/A/R for UOH S/A/R. | No fix needed for arcade-correctness. Display improvement: detect parry-counter (r2=3 on MOVE_START) and render a "PARRY CTR" badge on the meter so user knows this isn't a normal-move FINAL. |
| Parry-counter HK whiff S=27 A=0 (F=5024) | 1 (parry whiff) | Engine bypasses cg_ja.atix/cg_att_ix/h_att for one of HK parry-counter's active windows; classifier sees nothing. | Out of UOH scope. Investigate if parry-counter coverage becomes a goal. |
| atk=1 P2-dummy FINALs S=14-15 A=0 R=0 | 18 (not user moves) | Defender r1: 0→1 (blockstun entry) misclassified as MOVE_START | Filter MOVE_START to attacker-initiated transitions (e.g. r1=4 specifically, or exclude r1=1). |

**Recommendations.**

1. **Highest value:** the §13.7.5-proposed raw[]-derived A path
   (override engine_a with raw[] ACTIVE/CONTACT count when the
   §13.5.1 cut fires) cleans up Patterns §13.7.1 (A=22) and §13.7.4
   (A+1) simultaneously. It does NOT clean up the parry-counter
   F=4940 A=18 case directly, because raw_len froze at 16 there and
   raw[] missed parry-counter #2 entirely; the merged-move case needs
   its own fix below.

2. **New fix for multi-move merge (F=4940 mechanism) — SHIPPED in
   commit `a386e057`, the "second option" shape below.** Verified at
   [`frame_data_overlay.c:114-122`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
   (new `engine_a_at_atk_idle` field on `FdMove`),
   [`frame_data_overlay.c:858-868`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
   (snapshot taken on the first tick `attacker_idle >= 0`, before any
   deferred-finalize ticks let a fresh move's cells inflate the live
   counter), and
   [`frame_data_overlay.c:421-431`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
   (finalize prefers the snapshot over the live counter). The
   original two options considered were:
   - Force-finalize the previous move (treat it as completed at
     attacker_idle, accept that defender_idle never fires, emit FINAL
     with whatever defender state we have) and re-arm for the new
     MOVE_START. Trade-off: advantage calc is wrong for the prior
     move when defender hasn't returned to neutral. **This is the
     approach §13.9.1 tried and reverted — NOT what shipped.**
   - Or reset `fd_engine_active_count[atk] = 0` and bump
     `fd_prev_active_cgix[atk] = -1` so the new move's cells
     accumulate fresh, leaving the prior move's raw[] frozen but its
     A snapshot intact at the moment attacker_idle fired.

   The second option is what shipped — it preserves the prior move's
   numbers while letting the new move re-accumulate independently, by
   capturing `engine_a` at attacker_idle time (not at finalize time)
   so the prior FINAL gets the right A. Whether this fully resolves
   the F=4940 reproduction is unverified since the 2026-05-04 trace
   predates the fix — Phase 3 re-baselines with the harness.

   **Phase 3 result (2026-07-07): NOT reproducible with the move pair
   tried.** The harness's `q-crmk-multimove-merge` entry (cr.MK blocked,
   second button — LP — timed to land at wait=5/7/10/14/20/24, all
   inside the dummy's F=7-28 blockstun window per its own trace) produces
   a timeline byte-identical to plain cr.MK with no second press at all,
   for every wait value tried — no merge, no effect whatsoever. Root
   cause: cr.MK's own recovery (S=7+A=3+R=14=24, attacker idle at local
   F=32) outlasts the dummy's blockstun (exits at F=29, consistent with
   cr.MK's -3 block advantage) by 3 frames, so the "attacker already
   idle, dummy still blockstunned" merge window this fix targets never
   opens for cr.MK — and since Crouching Forward isn't chain/special/
   self-cancelable (`q.json` Cancel: all false), a press arriving mid-move
   is simply dropped, not merged. This does **not** disprove the F=4940
   mechanism (a positive- or near-zero-block-advantage move is needed to
   open the merge window — cr.MK's -3 adv makes the precondition
   structurally unreachable for this move); it means the corpus needs a
   different move pair (e.g. Jab, +2 adv) to actually exercise it. Kept
   as a plain PASS regression guard, not xfail, since nothing currently
   fails for cr.MK.

3. **Forward-walk-attack badge (optional, was: "parry-counter
   badge"):** the r2=3 + cghi 102–143 cluster identifies attacks
   *initiated from forward-walk*, which may or may not be post-parry
   counters (see corrected note above). A useful display would
   distinguish these from canonical-stance normals so users don't
   conflate the S/A/R numbers — but the badge can't honestly say
   "PARRY CTR" without a parry-event signal in the trace. To enable a
   real parry-counter badge, the trace would need to capture
   `set_paring_status` fires (e.g., add a column or annotation around
   [`hitcheck.c:570-571`](../src/sf33rd/Source/Game/engine/hitcheck.c)).

4. **atk=1 noise — SHIPPED in commit `a386e057`.** Verified at
   [`frame_data_overlay.c:583-586`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c):
   MOVE_START now fires only on `r1: 0→4` (attack states) or `0→2/3`
   (throw states), not on `0→1` (defender enters reaction), exactly
   as proposed. This eliminates the class of 18 spurious atk=1
   P2-dummy FINALs seen in the 2026-05-04 trace.

**Single-fix path (revised from §13.7.5).** The §13.7.5 raw[]-derived
A override is still the right move for §13.7.1 + §13.7.4. The newly-
identified F=4940 multi-move merge is a separate failure mode and
needs the §13.7.7 recommendation #2 fix on top. Together these
resolve all engine_a anomalies in the trace.

#### 13.7.8 REVERTED 2026-05-04 — multi-move-merge force-finalize fix (§13.7.7 rec #2)

**Status: NOT in the code.** Was shipped earlier today, then reverted
by the user. Verify by reading
[`frame_data_overlay.c:558-593`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
— the only logic between the `g_local_frame++` increment (line 556)
and the move-start scan (line 559) is a comment block; no
force-finalize guard exists. See §13.9 for the full
investigation-history record of what was tried and why it was
reverted.

The original §13.7.8 ship narrative is preserved below for future
agents to understand the intended fix shape, but the **code below
this point in the section is NOT in the codebase.** Do not assume
multi-move-merge protection is active.

---

**(Original 2026-05-04 ship narrative, now historical.)**

**Hypothesis verification.** The F=4940 A=18 walk in §13.7.7 is the
canonical reproduction of the multi-move-merge bug. Re-walked from
raw `/tmp/3sx-frame-trace.log` with awk:

```sh
awk -F'\t' -v lo=4810 -v hi=4945 'NR>1 && $1+0>=lo && $1+0<=hi {
    printf "F=%d P1.r1=%d cgix=%d cghi=%d jatix=%d P2.r1=%d P2.dstop=%d swnew=%s\n",
           $1, $3, $5, $7, $8, $37, $54, $31
}' /tmp/3sx-frame-trace.log
```

Confirmed sequence (every claim cited to a specific F):

| F | P1.r1 | cghi | P2.r1 | Note |
| --- | --- | --- | --- | --- |
| 4815 | 4 | 1 | 0 | close MK BLOCK ends; FINAL emits |
| 4816 | 0 | 1 | 0 | P1 idle |
| **4817** | **0→4** | 113 | 0 | **MOVE_START fires (parry-counter HP #1, swnew=0x0040)** |
| 4827 | 4 | 118 | 1 | Parry-counter #1 contact, P2 enters blockstun |
| 4844 | 4→0 | 464 | 1 | Attacker_idle for #1 sets; P2 still in blockstun → FINAL deferred |
| 4844-4918 | 0 | 464/277/455 | 1 | P1 idle 75 frames, defender stuck in blockstun |
| **4919** | **0→4** | 122 | 1 | **Fresh r1: 0→4 edge — but `g_cur.active==true` (line 559 gate). SUPPRESSED MOVE_START. P2.r1==1 confirms blockstun was the gate trigger.** |
| 4923 | 4 | 125 | 1 | Parry-counter #2 jatix=37 fires → engine accumulator (`charset.c:421-457`) adds cells to merged total |
| 4940 | 0 | 464 | 0 | P2 finally exits blockstun; FINAL emits with engine_a=18 (= #1's 11 + #2's ~7 cells, consistent with FINAL annotation S=10 A=18 R=0) |

**Confirmation criteria (all met).**
1. Frame between MOVE_START and FINAL with attacker.r1: 0→4: **F=4919** ✓
2. Engine accumulator past UOH-natural A=11 during the wait: **engine_a=18 at FINAL** ✓
3. P2.r1 at the time of the missed MOVE_START: **P2.r1=1 (blockstun)** ✓
4. Gate location: **`frame_data_overlay.c:559` `if (!g_cur.active)`** ✓

**No UOH-specific instance in the current trace.** The only A=22 UOH
case (F=1339) is the §13.7.1 `same-r1=4` retrigger — P1.r1 stays 4
throughout the back-to-back UOHs (verified F=1256-1339 walk: r1=4
unbroken from F=1256 to F=1339, with cghi rolling 229→230→231→232→
234→1→229→…); no fresh r1: 0→4 edge fires for the second UOH because
r1 never returned to 0. That case needs the §13.7.5 raw[]-derived A
override, not this fix. Per `feedback-trust-user-actions.md`: the
user's report of UOH+UOH+UOH against a blocker describes this exact
mechanism (and the engine treats parry-counter and UOH identically
through `char_move`/cgix accumulation), so the fix applies whenever
the chained moves' r1 returns to 0 between attempts. The trace
captured both modes; this fix targets the with-edge mode.

**Fix shipped at [`frame_data_overlay.c:558-583`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).**

```c
/* §13.7.7 multi-move merge guard ... */
if (g_cur.active && !g_cur.is_throw) {
    const int atk = g_cur.atk_idx;
    if (atk >= 0 && g_prev[atk].r1 == 0 && now[atk].r1 != 0) {
        if (g_cur.attacker_idle < 0) {
            g_cur.attacker_idle = g_local_frame - 1;
        }
        fd_finalize();
        fd_reset_move();
    }
}
```

The block runs **before** the existing `if (!g_cur.active)`
MOVE_START scan. On detect:
1. Sets attacker_idle if missing (best-effort end at prior tick).
2. Calls `fd_finalize()` — emits FINAL with the prior move's
   raw[] / engine_a as captured up to that point.
3. Calls `fd_reset_move()` — clears `g_cur.active` and all per-move
   state.
4. The next block (`if (!g_cur.active)`) now sees `g_cur.active ==
   false` and the `g_prev[i].r1 == 0 && now[i].r1 != 0` edge still
   true, so MOVE_START fires for the new move on the same tick.
   Critically, the existing MOVE_START path at lines 569-570 then
   resets `fd_engine_active_count[i] = 0` and
   `fd_prev_active_cgix[i] = -1`, so the new move's active cells
   accumulate fresh.

**Single-move regression check.** Verified clean UOH F=632 single-
move flow remains untouched: at F=727 (next UOH MOVE_START in the
trace) `g_cur.active == false` because the F=632 UOH FINAL emitted
at F=675 (verified per F=632 → F=727 walk, P1.r1: 4→0 at F=726).
The new guard only fires when `g_cur.active` is still true at a
fresh r1 edge — does not trigger on any of the 19 UOH FINALs in the
existing trace, all of which finalize before the next MOVE_START.
The throw-flow (`g_cur.is_throw`) is also excluded so CnDB stays on
its existing path.



**Code state** (list drafted 2026-05-04 while uncommitted; **committed
in `a386e057` as of 2026-07-07** — see the status-snapshot table's
"Git state" row):

- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:128-141` —
  multi-segment recovery cut state in `FdMove` (§13.5.1).
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:206-212` —
  catch-hitbox detection in `fd_snap_player` (§13.6).
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:325-348` —
  §13.3 no-active-signal guard.
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:386-402` —
  §13.5.2 multi-hit recovery override (last_active_pf_idx based).
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:621-631` —
  §13.6 throw-event detection (partner r1: 0→2/3 = HIT).
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:825-896` —
  §13.5.1 predicate (cghi→1-after-cgix-reset with dur≥3, gated
  on r1=4 OR r1=2 for command-throw cleanup).
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:793-827` —
  §14 live §8.3 mirror (with scattered-active skip for multi-hit).
- `src/sf33rd/Source/Game/engine/charset.c:414-457` —
  sentinel-aware `cg_ctr` accumulator (entry=0, per-frame=1, fires
  on `atix != 0 || caix != 0`).
- `src/sf33rd/Source/Game/engine/charset.c:485-496` —
  `[CMX]` cell-data extended-code dispatch log (player 1 only).
- `src/sf33rd/Source/Game/engine/charset.c:2762-2776` —
  `set_jugde_area` extends `fd_engine_hitbox_active` capture to caix.
- `src/sf33rd/Source/Game/ui/frame_trace.c:28-29` — `jcaix`
  (cg_ja.caix) and `cgcan` (cg_cancel) columns added.
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:160` — `bool kd`
  added to `FdLatched` for §13.6 KD advantage display.
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:296-300` —
  `fd_is_knockdown_at_atk_idle()` helper (§13.6).
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:482-484` —
  KD set in fd_finalize HIT branch (§13.6).
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c:920-948` —
  draw path renders "KD" in FD_COL_THROW when `g_latched.kd` true (§13.6).
- `src/sf33rd/Source/Game/ui/frame_trace.c` (TraceSnap struct +
  snap_player + emit_header + emit_row) — new defender-side
  diagnosis columns `rno3` (routine_no[3]), `cmwk14` (cmwk[14]),
  `wcaix` (cg_wca_ix) added 2026-05-04 for §13.2.
- `src/main.c:547-548` resets `fd_engine_hitbox_active` at the top
  of `game_step_0()`; overlay tick called at
  [`main.c:616`](../src/main.c).
- `src/sf33rd/Source/Game/engine/workuser.{c,h}` — globals
  `fd_engine_hitbox_active`, `fd_engine_active_count`,
  `fd_prev_active_cgix`.

**Trace state (fresh 2026-05-04 final pass).**

- `/tmp/3sx-frame-trace.log` and `/tmp/cm-trace.log` regenerated
  multiple times across the day. Most recent confirms HSB exact /
  CnDB S+A exact / UOH R=5 / all §12 normals matching arcade.
- Multiple session captures contain Q normals + crouching normals
  + Dashing Head/Leg ×3 + HSB ×3 + UOH ×N + CnDB ×N (LK/MK/HK)
  + assorted whiffs/jumps + `[CMX]` opcode dispatch log.
- Does NOT contain HIT (non-block) outcomes for normals — gates
  §13.2 cr.* low-block adv +2 verification.

**Open work — prioritized:**

1. **DONE — §13.3** (taunt + non-attack-move guard) shipped, verified.
2. **DONE — §13.4** (close HP / Back+HP A+1) resolved as
   stale-binary artifact.
3. **DONE — §13.5.1** UOH multi-segment recovery cut shipped
   (cghi→1-after-cgix-reset, dur≥3, gate r1=4 OR r1=2). UOH R=5
   exact arcade match. CnDB R reduced to R+1 (after §13.6.1
   partner-release predicate also shipped), then fully FIXED by Phase 4
   items 4+5 (see item 7 below and §13.6.1).
4. **DONE — §13.5.2** HSB multi-hit recovery shipped via
   last_active_pf_idx classifier change. HSB R=26/28/28 exact.
5. **DONE — §13.6** catch-hitbox classifier blindness fixed
   (accumulator + h_att_set extension + throw-event detection).
   CnDB now displays as proper hitting move with HIT outcome.
6. **DONE — §14** live meter coloring matches finalize via §8.3
   mirror; scattered-active multi-hit moves correctly show
   individual hits. User-confirmed visually.
7. **DONE — FIXED by Phase 4 items 4+5 (2026-07-07) — CnDB R+1
   residual.** Was R+6, then R+1 after §13.6.1 partner-release
   predicate shipped, confirmed real (not stale-binary) by the Phase 3
   harness. Root cause: the window was already the right size
   (`raw_len == arcade S+A+R` exactly); the catch cell's collapsed
   declared tick was double-attributed across the A/R boundary, not
   missing from the window — the "tighten `attacker_idle` by one
   frame" idea was tested and **falsified**. Fix: re-derive R from the
   engine credit (`meter_len - startup_pf - effective_a`) gated on a
   new explicit `ended_by_partner_release` flag, applying only to this
   path (falsified as a universal formula against ~27 other PASS
   entries — see §13.6.1). `q-cndb-lk-hit`/`-mk-hit`/`-hk-hit` now
   read R=42/44/46 exact, xfail removed, `kd: 1` run-failing.
8. **DONE — KD advantage display** (2026-05-04). Detects partner in
   r1=2/3 at attacker_idle via the last raw[] cell's def_r1 and
   displays "KD" (in throw-color FD_COL_THROW) instead of `+121..+125`.
   See §13.6 "Shipped 2026-05-04: KD advantage display."
9. **DONE — CONFIRMED FIXED by Phase 3 harness (2026-07-07). §13.7
   UOH residual A+1** (engine_a=11 vs arcade 10). UOH cells aren't
   sentinel-bearing — separate accumulator-side overcount. Fix:
   jatix-gated same-tick cgix-transit revoke at `charset.c:432-451`.
   `[CMX]` log captured for analysis; this was the analysis that
   motivated the shipped fix. Harness confirms A=10 exact
   (`q-uoh-samef-block`/`q-uoh-1f-block`, plain PASS) plus a mutation
   test that flags exactly these entries when the revoke is disabled.
9a. **REVERTED — §13.7.8 multi-move-merge force-finalize fix.**
    Was shipped, then reverted. Force-finalized the prior move when a
    fresh attacker `r1: 0→non-zero` edge fired while `g_cur.active`
    was still true. **NOT in code.** See §13.9 for the failed-attempt
    history. **A different fix for the same F=4940 mechanism (an
    `engine_a` snapshot at `attacker_idle`, not a force-finalize)
    shipped instead in `a386e057` — see §13.7.7 rec #2 and §13.9.3.**
    **Phase 3 update (2026-07-07): the shipped mitigation's coverage of
    the F=4940 case itself remains unverified** — the only move pair
    tried in the harness (cr.MK) can't open the merge window at all
    (cr.MK's -3 block advantage means attacker recovery outlasts
    blockstun); needs a positive-advantage move pair (e.g. Jab, +2) in
    the corpus to actually exercise it.
9b. **REVERTED — Force-finalize-on-retrigger for same-r1=4 case
    (F=1339 A=22).** Tried emitting MOVE_START annotation + re-init
    `g_cur` mid-r1=4 when the engine accumulator advanced past the
    natural per-move ceiling. Produced **S=0 garbage** with FINAL
    emitted mid-active-phase (cgix=20). Reverted. **NOT in code**, and
    the `a386e057` engine_a-snapshot fix does not cover this case
    either (no intervening `r1=0` edge to snapshot at — see §13.9.3).
    **Still OPEN — CONFIRMED ALIVE by Phase 3 harness (2026-07-07)**
    with a scripted reproducer (`q-uoh-chain-retrigger`, xfail, single
    FINAL A=20 at wait=38f, timing-dependent same as the doc's F=1339
    A=22 case).
10. **RESOLVED BY REBASELINE — Phase 3 harness (2026-07-07), did NOT
    reproduce. §13.2 cr.* low-attack BLOCK adv +2.** Previously
    confirmed to affect cr.LK / cr.MK / cr.HK AND cr.HP (broader than
    originally framed as "low attacks"). Under the harness (pinned
    RNG, controlled spacing), cr.MK/cr.HK/cr.HP BLOCK advantage all
    match `q.json` exactly (`q-crmk-block`, `q-crhk-block`,
    `q-crhp-block`, plain PASS) — hypothesis: the +2 was an artifact
    of the old manual-capture conditions. cr.MP HIT variance is also
    resolved: gone under pinned RNG (`q-crmp-hit-capture-a`/`-b`,
    byte-identical) — confirms the RNG-driven-dummy-guard hypothesis
    (`com_sub.c:1875`). Exception: cr.LK doesn't classify as BLOCK/HIT
    at all — see new item 14.
11. **OPEN — Identity of cghi=102/105/113/122/129/137 r2=3 moves on
    Q.** Corrected 2026-05-05: these are LP/MP/HP/LK/MK/HK normals
    launched from the forward-walk state per
    [`pls01.c:476`](../src/sf33rd/Source/Game/engine/pls01.c). The
    earlier "falsified by `paring_counter[0]==0`" reasoning was
    wrong — `paring_counter[]` is a bonus-mode score multiplier
    (`hitcheck.c:710`), not a normal-play parry flag. Whether any
    given instance is genuinely a *post-parry* counter (vs a normal
    forward-walking attack) is **not testable from existing trace
    data**; would need to instrument `set_paring_status` capture in
    `frame_trace.c` to add a parry-event signal.
12. **OPEN — non-Q character audit.** Are residual issues Q-specific
    or universal? Pick one other character (Hugo for command grabs,
    Ken/Ryu for canonical normals) and run the same audit.
13. **DONE — git commit.** Committed in `a386e057` (overlay + engine
    hooks) and `118f7350` (training toggle) on `frame-data-on-mister`.
    See the status-snapshot table's "Git state" row for the full
    commit chain including the Phase 1 harness.
14. **SHIPPED (2026-07-07, `frame-data-on-mister`) — cr.LK never sets
    `event_this_frame` vs. a guarding or hit dummy (issue #14).** Was:
    finalized WHIFF (S=6 A=5 R=8) instead of BLOCK/HIT on every dummy/
    distance/timing variant tried in the harness (`q-crlk-block`,
    `q-crlk-hit`, both formerly xfail). Root cause: `dm_stop =
    att.hs_you` (hitcheck.c:1343 via `dm_status_copy`, called on hit
    :544 and guard :603); `hs_you` is a **signed s8** per-move data
    field (`include/structs.h:115`) loaded verbatim from the attack
    table (`charset.c:2835`) — its sign selects the hitstop *style*,
    not the event kind: positive freezes the defender, negative lets
    `char_move()` keep advancing during the stop
    (`plmain.c:373–392` `check_hit_stop`). Q's and Ryu's cr.LK carry
    `hs_you = +7`; their cr.MK/cr.HK carry `-9`/`-11` (probe-verified);
    Hugo's cr.LK carries negative `hs_you` and classified fine
    untouched (`h-crshort-block`), confirming this is per-move data,
    not a light-kick class property. The old event edge required
    `dn->dm_stop < 0`, silently dropping the positive-sign contact.
    Fix: widened the predicate to `dn->dm_stop != 0`
    (`frame_data_overlay.c` ~line 929). Safety: an exhaustive grep
    inventory of every `dm_stop` writer under
    `src/sf33rd/Source/Game/` confirms no non-contact code writes it
    (all writers are gated on strike/guard/parry/throw contact, plus
    one scripted self-damage site — Q SA3 NINGENBAKUDAN, `plpat18.c:51`
    — which writes the attacker's own WORK and can never form a
    defender edge), so the widened edge cannot create a false
    HIT/BLOCK/PARRY event; parries always write `-15`
    (`set_paring_status`) and already passed the old `< 0` test, so
    they are unaffected. A positive `dm_stop` edge also can never
    coincide with `dn->r1 == 0` (which would misread as PARRY) because
    both positive-edge writers force `routine_no[1] = 1` on the same
    engine tick before the overlay samples (`hitcheck.c:539`/`:596`,
    `plpat18.c:36`). One pre-existing blind spot, unchanged by this fix:
    a player-vs-player
    **trade** abs-merges both combatants' `dm_stop` into `hit_stop` and
    zeroes them within the same hitcheck pass
    (`hitcheck.c:131–153`), so trades produce no `dm_stop` edge under
    either the old or the new predicate. `q-crlk-block`/`q-crlk-hit`
    now plain PASS with from-qjson BLOCK/HIT values; `ryu-crlk-block`
    now classifies BLOCK correctly (S/R/adv exact) but keeps an xfail
    for the unrelated, already-BLOCKED issue #17 A-undercount (A=2 vs
    arcade 3). **UPDATE (2026-07-07):** superseded — the §13.11
    declared-truth convention shipped with the lever-F restore;
    `ryu-crlk-block`'s xfail was removed and it now passes at arcade
    A=3.
15. **RESOLVED (2026-07-07, Step 5, `frame-data-on-mister`) —
    HIT-outcome R diverges from `q.json` for several moves: adopted as
    a documented display convention, §13.10.** Was: HSB R=21 on HIT vs
    26/28 (matches WHIFF/BLOCK); Dashing Leg RH R=24 vs 31; UOH R=3 vs
    5 (already explained by §13.7.3's cut-window analysis); Throw R=34
    vs 21 — all 7 Q entries plus the Ryu/Hugo grab-family members
    stayed xfail pending per-move classification. **CnDB HIT was
    originally suspected to be the same family and confirmed NOT to
    be** (Phase 4 items 4+5, unchanged by this item) — CnDB's R+1 was a
    mis-attributed A/R boundary inside an already-correctly-sized
    window (§13.6.1), while every remaining member of this family
    *fails* the same formula test (`raw_len - S - engine_a` vs arcade
    R) by 1-14 frames, a measured, different mechanism. Step 5's
    per-subfamily diagnosis classified all 16 member entries into
    three mechanism classes (§13.10): Class 1 hit-branch recovery
    chains (HSB×3, DLA-RH, Hugo Strong/Forward — a genuine, data-driven
    outcome branch in the attacker's own animation, not a defender-stun
    differential); Class 2, UOH's pre-existing §13.7.3 mechanism,
    generalized; Class 3 connected grabs (Q/Ryu Throw, Hugo Moonsault
    ×3/Meat Squasher ×3 — arcade R is the whiff-path figure, HIT-R
    measures the real capture sequence bounded by cut/release/
    FD_METER_LEN-saturation). Verdict for both subfamilies is (b),
    oracle-convention adopted — no overlay code change; the 15 xfail
    corpus entries flip to plain PASS asserting the measured convention
    value (citing §13.10), `h-forward-hit` keeps a narrowed xfail for
    its unrelated issue #17 A-clause only, and 3 new anchor entries
    (`q-dla-rh-block`, `q-throw-whiff`, `ryu-throw-whiff`) prove the
    convention's arcade-side block/whiff claims executable.
16. **CLOSED (2026-07-07, Phase 6 Step 8) — close-range "spurious
    FINAL" was two different phenomena, both now explained; not a
    reproducible phantom.** Original report (Phase 4/6 pickup):
    close-range (≤~56px) bare `press UP` or `LP+LK` sometimes produced
    `HIT S=2 A=1 R=34 adv=+101` in one harness session; did not
    reproduce in isolation on retry. A bounded systematic scan
    (`docs/plan-frame-data-completion.md` Step 8: matrix 1 — 2 inputs ×
    3 dists {24,40,56} × 3 preceding-entry-outcomes {none, HIT-with-KD,
    BLOCK} = 18 test entries, generous 150f settle; matrix 2 (refinement)
    — the shipped corpus's 90f inter_entry_wait against an unmodified
    `q-throw-hit`-shaped precede at the exact historically-cited
    distances {20,24,28,56}) resolved both halves and did not reproduce
    a phantom in either:
    - `press LP+LK` at dist ≤56px reproduced the exact numbers **every
      time** (21/21 trials across both matrices), regardless of
      distance — including 40/56px, both beyond Q's nominal 24px
      `Throw_range` — or what preceded it (including the "none" bucket,
      zero preceding attack). This is a **real** Q Throw connecting:
      S=2 A=1 R=34 kd=1 are exactly `q-throw-hit`'s own measured values
      (§13.10 Class 3 convention). Q's throw hitbox reaching well past
      its json-nominal range matches the already-documented CnDB finding
      (dist=70/100 still connect, `corpus-q.yaml:526-528`) — not a bug,
      not spurious.
    - `press UP` never reproduced: 0/17 trials across both matrices
      (dist ∈ {20,24,28,40,56}, all three preceding-entry-outcomes —
      the HIT-with-KD precede was Q's own Throw specifically, chosen
      over a strike-KD move because its S=2/A=1/R=34/kd=1 exactly
      matches the originally-reported numbers — under both a generous
      150f and the shipped 90f `inter_entry_wait`). Per the Step 8 STOP
      rule: not reproducible under systematic scan, keep out of corpus.
    `q-jump-none`'s dist=250 remains the right choice (well clear of
    Q's real, longer-than-nominal throw range). Its comment's
    characterization of the `press LP+LK` close-range connect as a
    "proximity-triggered false positive" is superseded by the
    explanation above (real throw, not a phantom) but is left as-is —
    Step 8's file scope excludes `corpus-q.yaml` absent a reproduction,
    and no phantom was reproduced.
17. **RESOLVED (2026-07-07, Step 2b) — user adopted the declared-truth
    displayed-A convention, §13.11.** **UPDATE (2026-07-10):** the
    shape-(a1)/(a2) half of this finding, described below as
    "permanently xfail-documented" (unrecoverable — the credit never
    enters the runtime), is now covered by §13.13's contact-path
    display convention: 7 of the 9 affected entries convert to a plain
    `PASS` asserting the measured value; see §13.13 for the full
    membership rule and the 2 exceptions (Hugo Roundhouse) that stay
    xfail. Originally opened (2026-07-07,
    Phase 6 Ryu audit) as: same-tick contact advance under-credits `A`
    on roughly half of Ryu's basic normals, engine-wide,
    animation-data-dependent. Resolution: the shape-(b) half of this
    finding (same-jatix same-tick revoke) has its credit restored by a
    gated accumulator-side fix (§13.11) — 11 Ryu entries flip to plain
    PASS at arcade values (incl. `ryu-crlk-block`, census-resolved this
    session as shape (b) — Step 1 had left it untraced) and Q's 5 UOH
    entries re-baseline to the convention's declared A=11. The
    shape-(a1: skip-jump)/(a2: char_move-bypass) half and the Hugo
    R-side manifestation (addendum below) remain permanently
    xfail-documented as unrecoverable — their declared credit never
    enters the runtime at all, so there is nothing for the §13.11
    restore to act on. The Step 1 findings appendix's BLOCKED diagnosis
    of those shapes is unchanged and is now absorbed as the normative
    unrecoverability statement in §13.11. Original finding narrative
    preserved below. On the CONTACT tick, the
    engine advances the attacker's cell chain a second time within the
    same `Game_timer` tick, in two shapes, both confirmed via `[CM]`/GT
    trace: (a) **skip-to-recovery** — Far Strong GT=136 contacts on
    `cgix=8`, same tick jumps to recovery `cgix=16`; the second
    declared active cell (`cgix=12`, `jatix=6`, 3 frames — which plays
    in full on WHIFF, GT=230-232) is never entered, so its credit never
    accrues (engine A=1 vs arcade 4); (b) **same-jatix transit** —
    Strong GT=43 credits cell 12 (`jatix=4`, `cgctr=2`, +2 frames), then
    same-tick advances to pat-consecutive cell 16 (same `jatix=4`),
    tripping the §13.7.4 same-tick jatix-gated revoke (`charset.c:432-
    451`) and discarding those 2 frames (engine A=2 vs arcade 4;
    without the revoke this move reads 4 exact) — `jatix` identity
    alone can't distinguish UOH's phantom transit from a
    contact-collapsed cell arcade still counts. Contrast: cr.HK's
    settle cell shares `cghi`/`jatix` with its active cel but is
    entered on a *later* tick (GT=324→336, spanning hitstop) — no
    same-tick advance, no revoke, A=5 exact — so tick timing, not
    `cghi`/`jatix` sameness, is what discriminates correct from
    undercounted. WHIFF is always exact (no contact ⇒ no same-tick
    branch). **Not Ryu-specific:** the identical same-tick
    contact-advance fires for Q too (Q Strong BLOCK GT=50: `cgix`
    30→36, same tick) — Q's corpus is green only because Q's animation
    data assigns each transited cell a distinct `jatix` (5→6→7), so the
    §13.7.4 revoke never fires there and the continuation chain's
    remaining actives (1+2+2=5) all get credited. This is an
    animation-data-dependent escape, not evidence the mechanism is
    absent on Q — the underlying engine behavior is shared. **SUPERSEDED
    (2026-07-07, §13.11):** this paragraph originally read "any fix
    must not simply disable the §13.7.4 revoke — that regresses
    q-uoh-samef-block/q-uoh-1f-block back to the documented mutation
    FAIL (A=11)." That is now backwards: A=11 IS the §13.11 expect for
    those two entries, not a mutation-FAIL value — the shipped fix
    restores the revoke's subtraction via a gated accumulator-side
    add-back (`fd_restore_revoked_declared_credit`, lever F) rather
    than disabling the revoke itself; disabling the revoke (lever A) is
    now expected to change nothing (path-independence, §13.11). Shape-
    (b) entries are no longer `xfail`-marked (11 Ryu entries flipped to
    plain PASS, §13.11); the remaining xfails in
    `tools/frame-data/corpus-ryu.yaml` are exclusively shape-(a)
    (`ryu-far-strong-block`, `ryu-crmk-block`, `ryu-twdshp-block`).
    Non-Ryu, non-Q character audit (item 12) should re-check this
    finding's reach.

    **Hugo Roundhouse addendum (2026-07-07, plan Step 4) — the same
    mechanism also manifests as R+2 (not A) on a landing-clocked
    move.** `h-roundhouse-block`/`-hit` measure `R=30` vs `hugo.json`
    28 with S=21, A=8, adv exact on both, and WHIFF exact
    (`h-roundhouse-whiff` R=28). Classified from same-binary
    `[CM]`/`[CMX]` traces plus two temporary, since-reverted
    fprintf-only probes (`[HPX]` at `hit_pattern_extdat_check` entry,
    `hitcheck.c:723`; `[JUP]` in `jumping_union_process`'s landing
    branch, `pls01.c:767`), scratch 3-entry corpus, Debug host build at
    `3da7fc25`:

    | Event (move tick = hitstop-free char_move ticks since MOVE_START) | WHIFF GT | BLOCK GT | HIT GT |
    | --- | ---: | ---: | ---: |
    | contact / cell 28 entry (jatix=14, declared ctr=2) | 465 (tick 22; plays 465-466) | 59 (tick 22) — `[HPX] extdat=0x80`, same-tick 28→32 | 262 (tick 22) — same, `extdat=0x80` |
    | cell 32 entry (jatix=15, ctr=3) | 467 (tick 24) | 59 (tick 22 — co-resident with contact) | 262 |
    | last active tick (cell 36, jatix=15) | 472 (tick 29) | 78 (tick 27) | 281 (tick 27) |
    | cell 44 entry (pre-landing tail, jatix=0, ctr=4) | 476 (tick 33) | 82 (tick 31) | 285 (tick 31) |
    | landing: `[JUP] y=-46 jphos=40`, rno3 2→3, jump 44→64 | 477 (tick 34) | 85 (tick 34) | 288 (tick 34) |
    | recovery rows after last active (= displayed R) | 28 (GT 473-500) | 30 (GT 79-108) | 30 |

    Chain of causation, each link measured: (1) the contact tick fires
    this item's driver byte `cg_extdat=0x80` → case `0x80`
    `char_move_z` (`hitcheck.c:727-729`) — cell 28 (declared 2 ticks)
    plays 1 visible tick and cell 32 enters that same tick, so the
    chain runs 2 anim ticks AHEAD of the whiff schedule from contact
    on. (2) **A is untouched** (8 exact, all outcomes): the
    accumulator credits declared cgctr at cell ENTRY
    (`charset.c:483-497`, adds 2+3+3), no cell entry is skipped, and
    the transit jatix differs (14→15) so the §13.7.4 revoke is gated
    off (`charset.c:471-474`) — unlike the A-undercount shapes above.
    (3) The move end is NOT chain-clocked: Hugo st.RH goes airborne
    and its landing segment (cells 64…92, pat_status 22→32) is entered
    by `jumping_union_process`'s ground check (`pls01.c:767-773`:
    `xyz[1].disp.pos + cg_jphos <= 0` → `routine_no[3] = 3` →
    `char_move_cmja`, measured `cmja pat=14` ⇒ dispatch walk enters at
    cgix=52). It fired at move tick 34 with byte-identical
    `y=-46 jphos=40` on all three outcomes (trajectory integration
    freezes in lockstep with the anim during hitstop), interrupting
    cell 44 mid-cell on WHIFF (ctr still 3); the landing-segment tail
    is 24 rows on every path. (4) Net: the 2 collapsed declared-active
    ticks cannot shorten a time-pinned move — the chain instead idles
    2 extra ticks in the no-hitbox cell 44 (3 pre-landing recovery
    rows GT 82-84 on BLOCK vs 1 row GT 476 on WHIFF), and
    `fd_finalize()`'s per-frame recovery tally
    (`frame_data_overlay.c:499-528`) counts them as RECOVERY →
    R=30 vs arcade 28 (= the whiff playout, consistent with Step 1's
    F3 oracle-convention finding).

    **Why this is NOT item 15 / §13.7.3:** no §13.5.1 cut is involved
    (FINAL `cut=0` on all six roundhouse captures; `attacker_idle` is
    the natural r1 4→0 edge at GT=109/501), the divergence hits BLOCK
    and HIT identically, and the sign is opposite (R too HIGH, vs
    R-too-low for #15's HIT-only strike members; #15's grab members
    diverge high but via a different, capture-convention question).
    §13.7.3's mechanism is
    defender-stun-scoped (hitstun < blockstun shortens the cleanup
    window) and by construction cannot produce a BLOCK==HIT surplus
    relative to WHIFF. Not an oracle-convention divergence either:
    arcade R equals the measured WHIFF playout exactly.

    **Family accounting:** on chain-clocked normals (every other
    item-17 entry) the same `0x80` contact advance shortens the whole
    move, so the loss shows in A with S/R/adv exact; on a
    landing-clocked move it shows in R with S/A/adv exact. Same
    driver, same Step-1 unrecoverability (the whiff-path playout is
    destroyed by the contact path; Step 1's STOP rules and BLOCKED
    verdict apply unchanged) — `h-roundhouse-block`/`-hit` stay xfail
    in `tools/frame-data/corpus-hugo.yaml` citing this addendum.

    **UPDATE (2026-07-10, CONV-2):** the line immediately above is now
    FALSE for both labels - the user decision has been taken (§13.13
    family F5, SCOPE EXTENSION block); `h-roundhouse-block`/`-hit`
    convert to a plain PASS asserting R=30. The historical line above is
    preserved verbatim per repo practice (dated append, not silent
    rewrite).

    **§13.11 cross-ref (2026-07-07):** this addendum's mechanism is
    R-side and jatix-differing, so the §13.11 declared-truth A
    convention's gated restore does not touch it — A stays exact (8)
    on this move both before and after §13.11, and this pair remains
    xfail on R exactly as documented above, unrelated to the 11
    A-side entries §13.11 flips.

18. **REWRITTEN/SPLIT 2026-07-08 (§13.5.1a cut-gate fix). Originally:
    NEW, OPEN (2026-07-07, Step 2b, spun off during the §13.11
    declared-truth convention rollout) — UOH WHIFF R measures 3 vs
    q.json's 5, an anim-reset/cut truncation.** The Phase 6 cast
    rollout grew this item into a large "R truncation on whiff/cut"
    bucket; the 2026-07-08 investigation split it three ways:

    **(a) RESOLVED subset — §13.5.1 false cuts (shapes A/B), fixed by
    §13.5.1a.** 31 corpus entries whose R-truncation was the level-test
    anchor misfiring (chart already at cghi=1 when a mid-anim cgix dip
    fires, or a landing-tick label flip on airborne moves): Ken SRK Jab
    whiff/block/hit (R 19→26), Akuma SRK Jab whiff/block/hit (18→26),
    Yun Nishou Kyaku all strengths ×3 outcomes (16/17/19→25/26/28),
    Urien Chariot lk/mk whiffs + Headbutt all outcomes + VKD-adjacent
    (10 entries), Dudley Jet Uppercut whiffs (19/18/22→29/28/32), Oro
    Niou Riki whiff/block (11→29) + Jinchu whiff (6→19). All 31 now
    measure arcade-exact and are plain PASS. See §13.5.1a for the
    mechanism, falsification table, and the grounded fresh-edge rule.

    **(b) RESIDUAL item 18 — stays OPEN (as of 2026-07-08).** Moves that
    cut on a GENUINE grounded fresh-edge cleanup chart (same shape as the
    protected q-uoh: grounded reset, real post-reset label, grounded flip
    into cghi=1) whose cut point sits earlier than the oracle's actionable
    point — the ORIGINAL item-18 mechanism, now correctly scoped.
    Members (measured vs arcade R): `q-uoh-whiff-r` (3 vs 5, the
    original member and precedent), `oro-uoh-whiff` (4 vs 5),
    `akuma-uoh-whiff` (6 vs 7), `dudley-uoh-block` (+ Dudley UOH
    comments), Urien UOH R clause (3 vs 5), `ken-tatsu-lk-whiff`
    (9 vs 14), `akuma-tatsu-lk-whiff` (12 vs 17),
    `chunli-farroundhouse-block` (11 vs 15), `chunli-hk-whiff`
    (10 vs 15), Chun-Li Hazan Shu whiff (15 vs 18), `yun-twdsmk-whiff`
    + companion (3 vs 8). Open: no mechanism-level fix proposed yet;
    distinct from issue #17 (that item is A-side and
    contact-shape-specific, this one is R-side).

    **DATED RE-CITE (2026-07-11, CAPTURE-1).** Two members of this list —
    `q-uoh-whiff-r` (measured R=3 vs oracle 5) and Urien's UOH R clause
    (measured R=3 vs oracle 5, same recipe) — were directly re-measured
    against live arcade hardware (`docs/arcade-frame-data/CAPTURE.md`):
    **arcade plays R=5 on both, byte-identical to the oracle, twice, on
    both characters.** The engine's own measured R=3 is confirmed
    engine-side, not a table error. No conversion follows (this is the
    same evidence as the exclusion-item-2 DATED REFRAME above, cited here
    at the member's own list entry) — both entries stay XFAIL with
    arcade R=5 in `expect`; see PORT-DIVERGENCE-1 (exclusion item 2
    above) for the layer-unknown framing this finding now carries.

    **DATED RE-CITE (2026-07-11, LAYER-1).** Layer RESOLVED: a
    convention-twin engine-raw probe measures both members at R=5,
    matching arcade (990512 REFERENCE + 990608) and the oracle exactly
    -- the engine's own raw busy timeline plays exactly what arcade
    plays; layer = (a) OVERLAY/METER, not layer-unknown.
    PORT-DIVERGENCE-1 is formally RETRACTED (see exclusion item 2's
    retraction note above); see §13.16 for the full triangulation. No
    conversion follows -- both entries stay XFAIL with arcade values in
    `expect`, pending the meter fix (§3.3's OVERLAY RE-ANCHOR program,
    user-gated).

    **ITEM-18 CLOSURE (2026-07-11, RE-ANCHOR-1 SHIPPED, §13.17).** The
    meter fix landed. Every WHIFF-outcome member of this register —
    18(b) residual (`q-uoh-whiff-r`, `oro-uoh-whiff`, `akuma-uoh-whiff`,
    Urien UOH R clause, `sean-uoh-whiff`, `urien-vkd-lk-whiff`, and the
    dudley/elena/necro/twelve/ibuki/alex-uoh-whiff family below) and
    18(c) (`yun-uoh-whiff`, `remy-uoh-whiff`, `yang-uoh-whiff`,
    `ibuki-kazekiri-lk-whiff`) and the cut-committed overshoot
    (`yang-senkyuutai-lk-whiff`, §12.2.4) — flips XFAIL->PASS via lever N
    (whiff busy-edge R), all onto their own arcade/oracle figure.
    **What remains of item-18: two rows, both explicitly excluded, not
    merely deferred.** `ibuki-kubiori-lp-whiff`/`-block` (18(b) shape,
    R=8 vs the first-triplet oracle 15) has ZERO raw box-active frames
    anywhere in its window (the twin construction confirms this
    directly) — Kubiori's real active-frame accounting runs through a
    mechanism the four-slot `att_box` array never observes, so no
    lever-N/O construction applies; excluded by the pre-existing
    `box_count>0` gate, stays XFAIL at its measured value
    (`<sp>/reanchor/census-report.md`'s Kubiori disposition). Item-18's
    CONTACT-leg members (the `-block`/`-hit` R clauses this register
    never listed as WHIFF, e.g. Dudley/Urien/Yun/Yang/Remy/Ibuki-Kazekiri
    contact legs) were never in scope of this program (§13.17's
    contact-leg scope-out) and are unaffected. Item-18 is CLOSED for
    every whiff member it names except the one census-adjudicated
    exclusion above.

    **(b) update 2026-07-09 (§13.5.1b guard-rearm gate, lever H).** A
    full-suite census found that most of the members listed just above
    (plus the whole `alex-crfierce` and `sean-ryuubi` families, absent from
    that original list) were actually the ENGINE-2/lever-H false-cut shape
    wearing item-18(b)'s clothing, not a genuine grounded cleanup shortfall
    — their tail kept `guard_flag=3` for the whole post-anchor window
    instead of re-arming. 26 windows total, split two ways:

    - **15 full flips (now plain PASS, arcade-exact R):**
      `akuma-tatsu-lk-whiff/-block/-hit` (12→17), `ken-tatsu-lk-whiff`
      (9→14), `chunli-hk-whiff` (10→15), Chun-Li Hazan Shu whiff (15→18),
      `sean-ryuubi-lk-whiff`/`-mk-whiff` (9→15 each), `yang-twdsmk-block`/
      `-whiff` (3→8 each), `yun-twdsmk-block`/`-whiff` (3→8 each),
      `alex-crfierce-block`/`-hit`/`-whiff` (19→22).
    - **11 narrowed (stay xfail on a residual, different, unrelated-to-
      lever-H clause — R itself resolves or nearly resolves):**
      `ken-tatsu-lk-block`/`-hit` (10→15, arcade 14, +1 residual R-variance
      — mechanism not investigated), `chunli-farroundhouse-block` (11→16,
      arcade 15, +1 residual R-variance, same shape as Ken's), Chun-Li
      Hazan Shu block/hit (R fully resolves 15→18, arcade-exact, but keeps
      an UNRELATED lever-F-tested shape-(a1) contact A-undercount, A=1 vs
      arcade 3), `sean-ryuubi-lk/-mk-block/-hit` + `-hk-hit` (R fully
      resolves 9→15, arcade-exact, but keeps an UNRELATED §12.2.3-candidate
      contact A-overcount, A=10 vs arcade 8), `sean-ryuubi-hk-block` (R
      only partially resolves 8→14, still 1 tick short of arcade 15 — this
      leg's own pre-fix R was already a tick shorter than its four
      siblings — PLUS the same A-overcount).

    The remaining five original members — `q-uoh-whiff-r`, `oro-uoh-whiff`,
    `akuma-uoh-whiff`, `dudley-uoh-block`, Urien's UOH R clause — were
    checked against the guard-flag test and confirmed GENUINE (re-arm at
    anchor+0/+1): they stay open, completely unaffected by this fix. The
    `sean-ryuubi` family in particular had been misdiagnosed as "not
    ENGINE-2" pre-fix via a since-falsified naive test (does the true r1
    edge land exactly on arcade's own S+A+R sum?) — see
    `corpus-sean.yaml`'s RYUUBI KYAKU CLASSIFICATION header for the
    correction. See §13.5.1b for the mechanism and the ENGINE-2 tracker row
    (`docs/plan-frame-data-completion.md`) for that family's own,
    separately-tracked disposition.

    **(c) NEW sub-finding, UNCLASSIFIED-pending — `yun-uoh-*` no-cut
    value OVERSHOOTS the oracle.** Post-§13.5.1a, Yun UOH's chart flips
    to cghi=1 across its landing, so the gate correctly refuses to
    anchor; the natural r1-edge end then reads ABOVE the arcade figure:
    R 1/4/2 → 10/12/10 vs arcade 6 (block adv −1→−9, hit adv +4→−4).
    This is the opposite sign from (b) and must NOT be folded into it —
    former item-18 member whose no-cut value overshoots. No mechanism
    identified yet.

    **(c) membership reconciled 2026-07-09 (classification sweep).**
    All (c) members share the no-cut signature (`cut=0`, no anchor,
    natural r1-edge end overshooting arcade): `yun-uoh-*` ×3 (the
    precedent above), `remy-uoh-whiff/-block/-hit` R clauses (10/9/9 vs 5,
    cut=0/anchor_a=−1), `yang-uoh-block/-hit` R clauses (14/12 vs 5),
    `ibuki-kazekiri-lk-*` ×3 (29 vs 26, cut=0, uniform across all
    outcomes incl. whiff). Explicitly NOT (c):
    `yang-senkyuutai-lk-*` ×3 — its whiff R-overshoot (38 vs 34) occurs
    WITH a committed cut (`cut=1`, anchor_a=8 armed), a different
    signature; it is registered as its own one-off class in §12.2.4
    ("cut-committed whiff R-overshoot") rather than given a new item-18
    letter (one move, no mechanism established).

    **DATED RE-CITE (2026-07-11, CAPTURE-1).** Two members here were
    directly re-measured against live arcade hardware
    (`docs/arcade-frame-data/CAPTURE.md`): `ibuki-kazekiri-lk` whiff R
    (measured 29 vs oracle 26) and `yang-senkyuutai-lk` whiff R (measured
    38 vs oracle 34, the §12.2.4 one-off cited just above, not a
    membership change). **Arcade plays R=26 (Ibuki) and R=34 (Yang),
    exactly matching each oracle figure.** Both engine overshoots are
    confirmed engine-side, not table errors. No conversion follows; both
    entries stay XFAIL with their arcade values in `expect` — see
    exclusion item 2's PORT-DIVERGENCE-1 note above for the framing this
    finding now carries across the whole item-18 family.

    **DATED RE-CITE (2026-07-11, LAYER-1).** Both overshoots (Ibuki +3,
    Yang +4) are now triangulated (a) OVERLAY/METER: the convention-
    twin's gflg-edge busy-R reads 26 (Ibuki) and 34 (Yang), matching
    arcade on both revisions (990512 REFERENCE + 990608) and the oracle
    exactly -- see §13.16. Layer resolved, PORT-DIVERGENCE-1 retracted
    (exclusion item 2 above); no conversion follows, both stay XFAIL at
    arcade values pending the meter fix.

    **(c) ENGINE-7 census update, 2026-07-10 (member notes enriched;
    see §12.2.4's ENGINE-7 CENSUS-FALSIFIED row and the §13.5.1b second
    dated correction for the full mechanism):** `yun-uoh-whiff`,
    `yang-uoh-whiff/-block/-hit`, and `ibuki-kazekiri-lk-*` all measure
    arcade end == guard-rearm tick exactly (census-measured, rearm+0) —
    but this is NOT convertible into a general trim rule: `urien-headbutt`
    lp/mp/hp (8 currently-PASS legs) re-arm under the identical
    post-landing `cghi=1` label with the same gap magnitude, yet its
    arcade end sits at rearm+3 (the natural r1 end), not rearm+0. This
    contradiction pair blocks any natural-end/guard-rearm trim from
    shipping for this family. `remy-uoh-whiff/-block/-hit`'s R clause:
    census delta −6 would produce R=4/3/3, which UNDERSHOOTS arcade 5 by
    1/2/2 across whiff/block/hit — the old ENGINE-7 plan's §3(ii) prediction that
    this move converts arcade-exact under a census-contingent trim is
    CONTRADICTED; remy-uoh's arcade end additionally matches NO traced
    engine event at all (only a mid-cell `cgctr` decrement, re-plan §2.6)
    — the strongest evidence that this family's arcade R is not
    uniformly machine-derivable from observable chart state. All five
    (c) members stay xfail, arcade values retained in `expect`, no lever.

    **DATED RE-CITE (2026-07-11, LAYER-1).** The convention-twin
    (§13.16) closes this census's own open question for the tested
    members with direct arcade measurement: `yun-uoh-whiff` twin
    busy-R=6 == arcade 990512 6 == oracle; `remy-uoh-whiff` twin busy-R=5
    == arcade 990512 5 == oracle (the census's own "matches no traced
    engine event" end reads plain oracle 5 on hardware -- the twin's
    uniform strictly-between counting rule, not this census's per-move
    overlay-endpoint retime, is what reconstructs it; see §13.16's
    chunli reconciliation for why the two constructions differ).
    `yang-uoh-*` was not itself re-captured this run (`yun-uoh`'s
    sibling, same signature) and stays as this census left it. Lever-K's
    REJECTION above is UNCHANGED by this finding -- it was a retime of
    the overlay's own endpoint, a different construction from the
    twin's from-scratch derivation; the twin does not resurrect lever-K,
    it replaces the question with a full-suite re-anchor candidate (see
    `docs/plan-frame-data-completion.md`'s new LAYER-1 tracker row,
    user-gated).

    **Flagged, unclassified, queued for classification sweep #3
    (2026-07-10):** `yun-uoh` and `yang-uoh`'s reset asymmetry — the
    WHIFF leg of each move fires `cgix_reset` (census `reset_idx` set),
    but the BLOCK and HIT legs of the SAME move never do (`reset_idx`
    None; `e7-census.tsv` rows `yun-uoh-block/-hit`,
    `yang-uoh-block/-hit`). Mechanism unknown; not investigated further
    by the ENGINE-7 census (out of scope — no rearm-gap edge exists on
    the block/hit legs to measure). Recorded here for a future sweep.

    See also §13.11's adjacent-divergence note (the "Known adjacent
    divergence, NOT covered by this convention" paragraph), refined by
    this split — that paragraph is the xfail-precedent home
    `tools/frame-data/CORPUS-AUTHORING.md` cites for whiff-R
    truncation, and it now points at residual item 18(b).

**Trace column index reference (for awk on `/tmp/3sx-frame-trace.log`):**

Header columns (verified against `frame_trace.c` `emit_header()` /
`emit_row()`): per side, 34 fields in order — `r1 r2 cgix cgatt
cghi jatix jcaix cgcan cgst athok hatt hhan hcat hdum heat hcau
hstop dstop dcnt dgrd tsuk tsmd gflg gchu prc pat kow cgctr swnew
rno3 cmwk14 wcaix x y`. Three columns (`rno3`, `cmwk14`, `wcaix`)
were added 2026-05-04 for §13.2 cr.* low-block diagnosis (insertion
point: between `swnew` and `x`), shifting only the position columns
(`x`, `y`) and `sastop`. Two earlier columns (`jcaix`, `cgcan`)
were added 2026-05-04 between `jatix`/`cgst` and between
`cgcan`/`cgst` respectively.

| Column | Field | Column | Field |
| --- | --- | --- | --- |
| `$1` | F (trace local frame) | `$37` | P2.r1 |
| `$2` | GT (Game_timer) | `$38` | P2.r2 |
| `$3` | P1.r1 | `$39` | P2.cgix |
| `$4` | P1.r2 | `$40` | P2.cgatt |
| `$5` | P1.cgix | `$41` | P2.cghi |
| `$6` | P1.cgatt | `$42` | P2.jatix |
| `$7` | P1.cghi | `$43` | P2.jcaix (cg_ja.caix) |
| `$8` | P1.jatix | `$44` | P2.cgcan (cg_cancel) |
| `$9` | P1.jcaix (cg_ja.caix) | `$45` | P2.cgst |
| `$10` | P1.cgcan (cg_cancel) | `$46` | P2.athok |
| `$11` | P1.cgst | `$47` | P2.hatt |
| `$12` | P1.athok | `$48-52` | P2.hhan…hcau |
| `$13` | P1.hatt | `$53` | P2.hstop |
| `$14-18` | P1.hhan…hcau | `$54` | P2.dstop |
| `$19` | P1.hstop | `$55` | P2.dcnt |
| `$20` | P1.dstop | `$56` | P2.dgrd |
| `$21` | P1.dcnt | `$57-58` | P2.tsuk/tsmd |
| `$22` | P1.dgrd | `$59` | P2.gflg |
| `$23-24` | P1.tsuk/tsmd | `$60` | P2.gchu |
| `$25` | P1.gflg | `$61` | P2.prc |
| `$26` | P1.gchu | `$62` | P2.pat |
| `$27` | P1.prc | `$63` | P2.kow |
| `$28` | P1.pat | `$64` | P2.cgctr |
| `$29` | P1.kow | `$65` | P2.swnew |
| `$30` | P1.cgctr | `$66` | P2.rno3 (routine_no[3]) |
| `$31` | P1.swnew | `$67` | P2.cmwk14 (cmwk[14]) |
| `$32` | P1.rno3 (routine_no[3]) | `$68` | P2.wcaix (cg_wca_ix) |
| `$33` | P1.cmwk14 (cmwk[14]) | `$69` | P2.x |
| `$34` | P1.wcaix (cg_wca_ix) | `$70` | P2.y |
| `$35` | P1.x | `$71` | sastop |
| `$36` | P1.y | | |

Annotation lines start with `# F=N` and follow `MOVE_START` /
`FINAL` text — grep with `grep "^# F=" /tmp/3sx-frame-trace.log`
to extract them.

**Common analysis recipes:**

```sh
# All MOVE_START + FINAL annotations
grep "^# F=" /tmp/3sx-frame-trace.log | grep -E "MOVE_START|FINAL"

# Cell-by-cell timeline for a move (replace GT range)
awk 'NR>17 && /^[0-9]/ && $2>=GT_START && $2<=GT_END \
     {printf "GT=%s r1=%s cgix=%s cgctr=%s jatix=%s hstop=%s P2.r1=%s P2.dstop=%s P2.rno3=%s P2.cmwk14=%s\n", \
      $2, $3, $5, $30, $8, $19, $37, $54, $66, $67}' /tmp/3sx-frame-trace.log

# Find all sentinel-active char_move calls (cgctr >= 200, jatix non-zero)
awk 'NR>15 && /^[0-9]/ && $30>=200 && $8!=0 \
     {print $2, $5, $30}' /tmp/3sx-frame-trace.log
```

**Build / launch:**

```sh
cmake --build /Users/sb/Developer/3sx-mister/build/host -j8
rm -f /tmp/3sx-frame-trace.log /tmp/cm-trace.log
/Users/sb/Developer/3sx-mister/build/host/3S-ARM.app/Contents/MacOS/3S-ARM 2>/tmp/cm-trace.log
```

Trace file caps at 200 000 data rows; annotations don't count toward
the cap. Stderr also gets per-call `[CM]` log lines for player 1
(`charset.c:454-465`) plus per-cell `[CMX]` opcode-dispatch lines
(`charset.c:485-496`) — capture stderr to file for sub-frame
analysis of multiple char_move calls per game frame and for
identifying cell-data extended-code transitions, since a single
trace row can hide multiple cell advances.

**Reference move identification (Q, char=17):**

Inputs (`sw_new` rising edge, hex; bit OR with crouch=`0x02` or
direction bits):

| Button | Bit |
| --- | --- |
| LP | 0x10 |
| MP | 0x20 |
| HP | 0x40 |
| LK | 0x100 |
| MK | 0x200 |
| HK | 0x400 |
| START | 0x80 |

Direction bits live in the low nibble (`0x0F`); back is `0x08` for a
right-facing player, `0x0004` is forward, etc. (verify with the
trace's `swnew` values for known moves before relying on direction
encoding for a new test).

`pat=32` distinguishes crouching variants from standing. `kow` value
encodes button+stance:

| `kow` | meaning |
| --- | --- |
| 0 | LP-stance |
| 1 | LK-stance |
| 2 | MP-stance |
| 3 | MK-stance |
| 4 | HP-stance |
| 5 | HK-stance |
| 8/10/12/14 | various special motions (varies) |

---

### 13.8 Pickup checklist (start-here for fresh agents)

If you've been asked to work on the frame-data overlay, read this
section first. Then read §13.9 for the failed-attempt history before
trying any UOH-chained / multi-move fix.

**Currently shipped (verify by reading the file at the cited
line range; do not trust the doc citation alone):**

| Feature | File:line | Notes |
|---|---|---|
| Multi-segment recovery cut state in `FdMove` | [`frame_data_overlay.c:128-141`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | Fields `prev_cgix`, `cgix_reset_frame`, `cghi1_first_frame`, `cghi1_first_raw_slot`, `cghi1_count` |
| §13.5.1 multi-segment recovery predicate | [`frame_data_overlay.c:825-896`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | Includes §13.6.1 partner-release short-circuit (lines 842-856) |
| §13.5.2 multi-hit recovery override | [`frame_data_overlay.c:386-402`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | `last_active_pf_idx`-based recovery_pf computation |
| §13.3 no-active-signal guard | [`frame_data_overlay.c:325-348`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | Whole-move STARTUP when no active signal + no event |
| §13.6 catch-hitbox snap | [`frame_data_overlay.c:206-212`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | `cg_ja.caix > 0` extends `h_att_set` |
| §13.6 throw-event detection | [`frame_data_overlay.c:621-631`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | Partner `r1: 0 → 2/3` fires HIT outcome |
| §13.6 KD field | [`frame_data_overlay.c:160`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | `bool kd` in `FdLatched` |
| §13.6 KD helper | [`frame_data_overlay.c:296-300`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | `fd_is_knockdown_at_atk_idle()` |
| §13.6 KD set in finalize | [`frame_data_overlay.c:482-484`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | HIT branch only |
| §13.6 KD draw | [`frame_data_overlay.c:920-948`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | Renders "KD" in `FD_COL_THROW` |
| §14 live §8.3 mirror | [`frame_data_overlay.c:793-827`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | Skips for scattered-active multi-hit moves |
| Engine accumulator (sentinel-aware, atix \|\| caix) | [`charset.c:414-457`](../src/sf33rd/Source/Game/engine/charset.c) | Entry adds 0 for sentinel; per-frame adds 1 while still on cell |
| `set_jugde_area` capture (atix \|\| caix) | [`charset.c:2778-2795`](../src/sf33rd/Source/Game/engine/charset.c) | Secondary capture site for moves whose atix gets reset before overlay observes |
| `[CMX]` opcode-dispatch log | [`charset.c:485-496`](../src/sf33rd/Source/Game/engine/charset.c) | Player 1 only; for sub-frame analysis |
| `[CM]` per-call log | [`charset.c:459-470`](../src/sf33rd/Source/Game/engine/charset.c) | Player 1 only |
| Trace columns `jcaix` `cgcan` | [`frame_trace.c:27-29`](../src/sf33rd/Source/Game/ui/frame_trace.c) | Added 2026-05-04 mid-day |
| Trace columns `rno3` `cmwk14` `wcaix` | [`frame_trace.c:70-72, 122-124, 184, 196`](../src/sf33rd/Source/Game/ui/frame_trace.c) | Added 2026-05-04 late; for §13.2 cr.* low-block diagnosis |
| Jatix-gated same-tick cgix-transit revoke | [`charset.c:432-451`](../src/sf33rd/Source/Game/engine/charset.c) | Shipped `a386e057` 2026-07 — targets UOH residual A+1 (§13.7.4). **Confirmed working by Phase 3 harness 2026-07-07** (A=10 exact + mutation test). **UPDATE (2026-07-07, §13.11):** the revoke's subtraction is now immediately restored by a new gated flag (`fd_restore_revoked_declared_credit`, same `if` block — see §13.7.4's own update note); displayed A is now 11 by the declared-truth convention. This row's mutation-test role transfers to the new lever F; disabling this revoke (lever A) is now expected to change nothing. |
| `engine_a` snapshot at `attacker_idle` | [`frame_data_overlay.c:114-122, 421-431, 858-868`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | Shipped `a386e057` 2026-07 — targets multi-move-merge (F=4940-style, §13.7.7 rec #2); does NOT cover same-r1=4 retrigger without an intervening idle edge (F=1339-style, see §13.9.3). **Step 7 (2026-07-07, §13.9.3): a positive-advantage pair (Jab, N=17) reproduced the F=4940 precondition for the first time (cr.MK's -3 adv couldn't) — the mitigation's code path fires correctly, but the resulting window is structurally too narrow for the second move's cells to reach the live counter before finalize, so the mutation check cannot discriminate the fix. Coverage remains unverified for lack of a contaminating scenario; see §13.9.3 for the full trace and the unscanned throw-second-move residual.** |
| atk=1 MOVE_START filter | [`frame_data_overlay.c:583-586`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c) | Shipped `a386e057` 2026-07 — MOVE_START only on `r1: 0→4/2/3`, not `0→1`; removes the atk=1 P2-dummy FINAL noise |
| Gated anchor-time `engine_a` snapshot (`cghi1_dwell_broken`, `engine_a_at_cut_anchor`) | `frame_data_overlay.c` (FdMove fields near the §13.5.1 group; snapshot in the tick-side else-if chain; gate + `effective_a` in `fd_finalize`) | Shipped this change (2026-07-07, §13.9.4) — fixes same-r1=4 retrigger (F=1339-style), the one case `a386e057`'s `attacker_idle` snapshot does NOT cover. **Confirmed working by Phase 3 harness** (A=10 exact + two mutation-test levers, see §13.9.4) |
| Gated partner-release R re-attribution (`ended_by_partner_release`) | `frame_data_overlay.c` (`FdMove` field after `engine_a_at_cut_anchor`; set in the §13.6.1 tick-side branch; read in `fd_finalize`'s recovery re-derivation) | Shipped 2026-07-07, Phase 4 items 4+5 — fixes CnDB R+1 (§13.6.1). **Confirmed working by Phase 3/4 harness** (R=42/44/46 exact + mutation-C lever, see §13.6.1) |
| §13.5.1a grounded fresh-edge cut gate (`fd_cut_requires_grounded_fresh_edge`, `prev_cghi`/`prev_cghi_frame`/`prev_y`) | `frame_data_overlay.c` (`FdMove` fields after `cghi1_count`; gate + edge predicate in the §13.5.1 tick-side block) | Shipped 2026-07-08 — the §13.5.1 anchor is now an EDGE test (grounded fresh cghi→1 edge, three conditions), killing the two false-cut shapes (§13.5.1a). 31 corpus entries flip arcade-exact (Ken/Akuma SRK, Yun Nishou, Urien Chariot/Headbutt, Dudley Jet, Oro Niou Riki/Jinchu). Mutation lever G (set const to 0 = legacy level test) flags exactly those 31 across the full suite; Q flags nothing. |
| §13.5.1b guard-rearm commit gate (`fd_cut_requires_guard_rearm`, `cut_committed`) | `frame_data_overlay.c` (`FdMove` field `cut_committed` added near `cghi1_count`; gate in the §13.5.1 tick-side block before the dwell if/else-if chain; `fd_finalize()` reads `cut_committed` instead of re-deriving from `cghi1_count >= 3`) | Shipped 2026-07-09 — a committed §13.5.1 cut is now only trusted if `plw[atk].guard_flag == 0` (or `an->r1 != 4`, the r1==2 grapple exemption) at the commit tick — see §13.5.1b for the mechanism. Fixes ENGINE-2 (9 entries / 3 moves: 5 full flips, 2 narrowed to the stacked remy A-clause, 2 unaffected/re-cited to §12.2.2 — see the ENGINE-2 tracker row) plus a further 26-window slice hiding inside item 18(b) (15 full flips, 11 narrowed to a residual clause — akuma-tatsu/chunli-hk/chunli-hazan-whiff/yang/yun/sean-ryuubi-whiffs fully resolve; ken-tatsu/chunli-farroundhouse/chunli-hazan-contact/sean-ryuubi-contact retain a narrower clause). Mutation lever H (set const to 0) flags exactly the §5/§6 flip set across the full 19-corpus suite (measured, see `docs/plan-frame-data-completion.md` E3); Q flags nothing. |
| §13.6.2b strike-KD latch + measured down-family set (`fd_r2_is_down_family()`, `FdMove.strike_kd_seen`/`strike_kd_r2`, `fd_strike_kd_latch`) | `frame_data_overlay.c` (helper + `fd_is_knockdown_at_atk_idle()` near the §13.6 KD helper; `FdMove` fields after `proj_seen`; tick-side latch site right after the defender-idle-return check) | Shipped 2026-07-09 (ENGINE-3) — the down-family set grows from `{16,19}` to `{14..23}∪{27,28}`, and is now sampled every tick of the finalize-deferred window (not just the last raw[] cell) via a sticky latch. See §13.6.2b for the full mechanism, census, and mutation verification (levers E and I). Fixes 2 XFAIL→PASS + 7 kd re-asserts (3 keep an unrelated xfail clause) + 26 unasserted golden-kd flips, 0 regressions. |
| §13.14 multi-contact adv re-arm (`fd_adv_last_stun_exit`, lever M) | `frame_data_overlay.c` (tick-side non-throw defender block, immediately BEFORE the defender-idle-return latch) | Shipped 2026-07-11 (ENGINE-9) — `defender_idle` re-arms to −1 on a same-chart re-contact (5-conjunct signal: HIT/BLOCK latched, `defender_idle>=0`, `cgix_reset_frame<0`, fresh `dm_stop` 0→nonzero edge, `dn->r1==1`) so the FINAL stun exit anchors adv on multi-contact moves. Census (`e9-census`, 1,039 windows, RUNG 0): re-arm population is exactly `remy-crroundhouse-block` (adv −41→−11, arcade-exact, XFAIL→PASS); `q-uoh-chain-retrigger` correctly refused by the chart gate; zero drift elsewhere; monotonicity `adv_pred>=adv_today` 1039/1039. Mutation lever M (const to 0) drifts exactly `remy-crroundhouse-block` back to −41, nothing else. See §13.14. |

**Resolved by the Phase 3 harness (2026-07-07) — no longer OPEN:**

| Case | Result | Section |
|---|---|---|
| UOH residual A+1 (engine_a=11 vs arcade A=10) | **FIXED, confirmed.** `q-uoh-samef-block`/`q-uoh-1f-block`: A=10 exact (plain PASS). Mutation test flags exactly these entries when the revoke is disabled. **UPDATE (2026-07-07, §13.11):** superseded — under the declared-truth displayed-A convention, both entries now assert `A: 11` (the declared credit), and the mutation lever that flags them is F (restore-gate off), not the revoke-disable lever (A), whose flag set is now ∅. | §13.7.4, §13.11 |
| Same-r1=4 retrigger (UOH chain without intervening idle) | **FIXED, confirmed.** `q-uoh-chain-retrigger`: A=10 exact (plain PASS, xfail removed). Gated anchor-time `engine_a` snapshot — a third, distinct fix from `a386e057` and the two reverted force-finalize attempts. Two mutation-test levers both confirm (jatix-revoke lever now flags 3 entries; the new gate's own lever flags exactly this entry when forced off). **UPDATE (2026-07-07, §13.11):** superseded — the anchor snapshot inherits the §13.11 restore, so `q-uoh-chain-retrigger` now asserts `A: 11`; lever A's 3-entry flag set is superseded (now ∅), and lever B's FAIL value is now A=22 (was 20). | §13.7.1, §13.9.4, §13.11 |
| cr.* BLOCK adv +2 | **Did not reproduce.** cr.LK/MK/HK/HP block advantage matches `q.json` exactly under pinned RNG + controlled spacing (`q-crmk-block`, `q-crhk-block`, `q-crhp-block`). Hypothesis: artifact of old manual-capture conditions. | §13.2 |
| cr.MP HIT adv variance | **Gone under pinned RNG.** Two independent captures byte-identical (`q-crmp-hit-capture-a`/`-b`), confirms RNG-driven-dummy-guard hypothesis (`com_sub.c:1875`). | §13.2 |
| CnDB R+1 residual | **FIXED, confirmed 2026-07-07 (Phase 4 items 4+5).** `q-cndb-lk-hit`/`-mk-hit`/`-hk-hit`: R=42/44/46 exact (plain PASS, xfail removed, `kd: 1` run-failing). Root cause was a mis-attributed A/R boundary in an already-correctly-sized window (declared-tick collapse), not a mis-sized window — R re-derived from the engine credit gated on a new `ended_by_partner_release` flag. Mutation test (release-R branch forced off) flags exactly these 3 entries at the old R=43/45/47. | §13.6.1 |
| cr.LK never classifies vs. a guarding/hit dummy | **SHIPPED, confirmed 2026-07-07 (issue #14).** `q-crlk-block`/`q-crlk-hit`: plain PASS, from-qjson BLOCK/HIT with exact S/A/R/adv (xfail removed). Root cause: cr.LK's contact writes `dm_stop = +7` (positive `hs_you` move-data byte), which the old `dn->dm_stop < 0` event edge silently dropped; cr.MK/cr.HK write negative `hs_you` and classified fine. Fix: widened the edge to `dn->dm_stop != 0` — writer-inventory + parry-safety argument confirms no false HIT/BLOCK/PARRY can result. `ryu-crlk-block` now classifies BLOCK correctly too (S/R/adv exact) but keeps its xfail, narrowed to only the unrelated, already-BLOCKED issue #17 A-undercount. **UPDATE (2026-07-07, §13.11):** superseded — `ryu-crlk-block`'s xfail was removed entirely under the declared-truth displayed-A convention (census-resolved as shape (b), item 17); it now plain-PASSes at arcade A=3. | §13.2, item 14 |
| HIT-outcome R diverges from `q.json` for several moves | **RESOLVED, oracle-convention adopted 2026-07-07 (issue #15, Step 5).** All 16 member entries now cite §13.10's HIT-R display convention (or, for UOH, §13.7.3 as generalized by §13.10): Class 1 hit-branch recovery chains (HSB×3 R=21, DLA-RH R=24, Hugo Strong R=8, Hugo Forward R=20 — the R clause of its stacked xfail only), Class 2 UOH (R=3, §13.7.3, unchanged), Class 3 connected grabs whose arcade R is the whiff-path figure (Q Throw R=34, Ryu Throw R=38, Hugo Moonsault×3 R=67, Hugo Meat Squasher Short/Forward/RH R=48/38/25). No overlay code change — verdict (b) for both subfamilies. `q-hsb-jab-hit`/`-strong-hit`/`-fierce-hit`, `q-dla-rh-hit`, `q-throw-hit`, `q-uoh-samef-hit`/`-1f-hit`, `ryu-throw-hit`, `h-strong-hit`, `h-moonsault-{lp,mp,hp}-hit`, `h-meatsquasher-{short,forward,rh}-hit` all flip xfail→plain PASS asserting the measured convention value; `h-forward-hit` keeps a narrowed xfail citing only its unrelated issue #17 A-clause. Three new anchor entries (`q-dla-rh-block`, `q-throw-whiff`, `ryu-throw-whiff`) prove the convention's arcade-side block/whiff claims executable. **CnDB was checked against this family by Phase 4 items 4+5 and confirmed NOT a member** (its formula test lands exactly on arcade, while every member of this family fails that same test by 1-14 frames — see §13.7.3's Phase 4 note). | §13.10, item 15 |

**Currently OPEN and known-broken (Phase 3 status as of 2026-07-07):**

| Case | Symptom | Section |
|---|---|---|
| Multi-move merge across deferred finalize | A=18 on F=4940-style chains where defender blockstun outlasts attacker recovery | §13.7.7 F=4940, §13.9.1 — mitigation shipped (`a386e057`, `frame_data_overlay.c:114-122, 421-431, 858-868`); **Step 7 (2026-07-07, §13.9.3) tried Jab (+2 block adv) — the precondition (fresh `r1:0→4` edge while `g_cur.active` still deferred) is genuinely reachable, unlike cr.MK, but the resulting window is structurally too narrow for any second move to reach its own active frames before `defender_idle` fires, so the mutation check cannot discriminate the fix (see §13.9.3 for the full trace walk and negative-result mutation transcript). Contamination itself remains not-yet-observed with any Q normal.** |
| Identity of cghi=102/105/113/122/129/137 r2=3 moves | Display shows S/A/R for these but identity unknown; *not* parry counters | §13.7.7 — investigation deferred |
| Residual item 18(b): grounded fresh-edge early-cut | Whiff/block R truncated below arcade on moves whose GENUINE grounded cleanup cut fires before the oracle actionable point — NARROWED 2026-07-09 (§13.5.1b): the members whose tail actually kept `guard_flag=3` (ken/akuma Tatsu LK, Chun far-RH/HK/Hazan Shu, Yun/Yang Towards+MK) were lever-H false cuts, now FIXED (arcade-exact or narrowed to a residual ±1-variance/unrelated-clause, see the corpus files). Remaining GENUINE members (guard_flag re-arms early, confirmed unaffected): `q-uoh-whiff-r` 3 vs 5, `oro-uoh-whiff` 4 vs 5, `akuma-uoh-whiff` 6 vs 7, `dudley-uoh-block`, Urien UOH R clause 3 vs 5 | item 18(b) — re-scoped 2026-07-08 by the §13.5.1a split, re-scoped again 2026-07-09 by §13.5.1b; NOT the false-cut shapes (those are FIXED) |
| Item 18(c): Yun UOH no-cut R overshoot | `yun-uoh-*` R 10/12/10 vs arcade 6 (gate correctly refuses to anchor across Yun UOH's landing flip; natural r1-edge end reads ABOVE oracle) | item 18(c) — UNCLASSIFIED-pending, 2026-07-08 |
| Non-Q character coverage | Unaudited | open |

**Aside, CLOSED (Phase 4/6 pickup 2026-07-07; closed Phase 6 Step 8,
2026-07-07):** close-range (≤~56px) bare `press UP` or `LP+LK`
sometimes produced a spurious FINAL `HIT S=2 A=1 R=34 adv=+101` in one
harness session; did not reproduce in isolation on retry. Step 8's
bounded systematic scan (two matrices, 38 entries total, see item 16
above for the full breakdown) explained both halves and reproduced no
phantom: `press LP+LK` at ≤56px is a **real** Q Throw connecting (Q's
throw hitbox reaches well past its json-nominal 24px range, same as
the already-documented CnDB finding); `press UP` never reproduced
(0/17 trials across dist ∈ {20,24,28,40,56} × all three
preceding-entry-outcomes × both a generous and the shipped
`inter_entry_wait`). Not the same mechanism as the "atk=1 P2-dummy
FINALs" spurious-MOVE_START item above (that item is unrelated and
remains open) — this was a red herring, not a shared root cause.

**Reproducer recipes for OPEN cases.** As of Phase 3 (2026-07-07),
`tools/frame-data/run.sh` is the standard reproducer — the corpus below
**is** the executable form of this table; each row cites its
`tools/frame-data/corpus-q.yaml` label(s) instead of a manual recipe.
The manual protocol (rebuild, fresh trace, play in training mode) is
preserved for MiSTer-hardware-side checks per §12.0:

```sh
rm -f /tmp/3sx-frame-trace.log /tmp/cm-trace.log
build/host/3S-ARM.app/Contents/MacOS/3S-ARM 2>/tmp/cm-trace.log
# In-game: training mode, P1=Q, P2=any
```

| Case | Corpus label(s) | Manual reproducer (fallback) |
|---|---|---|
| Clean UOH A+1 | `q-uoh-samef-block`, `q-uoh-1f-block` (now plain PASS — FIXED) | Single MP+MK press; expect S=15 A=10 R=5 BLOCK / R=3 HIT |
| Same-r1=4 retrigger | `q-uoh-chain-retrigger` (now plain PASS — FIXED, §13.9.4) | `press MP+MK; wait 38; press MP+MK`; expect single FINAL S=15 A=10 R=5 |
| Multi-move merge | `q-crmk-multimove-merge` (plain PASS — did not reproduce with cr.MK; needs a positive-adv move pair) | Press cr.MK on dummy (gets blocked), then immediately press another button while dummy still in r1=1 |
| CnDB R+1 | `q-cndb-lk-hit`, `q-cndb-mk-hit`, `q-cndb-hk-hit` (now plain PASS — FIXED, §13.6.1) | HCB+LK / HCB+MK / HCB+HK in throw range; expect S=12/13/14 A=2 R=42/44/46 with `KD` adv display |
| cr.* BLOCK +2 | `q-crmk-block`, `q-crhk-block`, `q-crhp-block` (plain PASS — did not reproduce) | cr.MK/HK/HP into auto-blocking dummy |
| cr.LK never classifies | `q-crlk-block`, `q-crlk-hit` (now plain PASS — SHIPPED, issue #14) | cr.LK into any dummy; now classifies BLOCK/HIT correctly |
| cr.MP HIT variance | `q-crmp-hit-capture-a`, `q-crmp-hit-capture-b` (plain PASS — gone under pinned RNG) | cr.MP into NOT-blocking dummy; outcome=HIT |
| HIT-R divergence family | `q-hsb-jab-hit`/`-strong-hit`/`-fierce-hit`, `q-dla-rh-hit`, `q-throw-hit`, `q-uoh-samef-hit`/`-1f-hit` (all plain PASS — RESOLVED, §13.10 convention adopted, issue #15) | Any of these moves into a non-blocking dummy; expect the §13.10-cited convention value, not the arcade WHIFF/BLOCK figure |

---

### 13.9 Same-r1=4 retrigger and multi-move-merge — failed-attempt history

**Why this section exists.** Two distinct fixes for chained-UOH-style
problems were shipped earlier today and then reverted by the user
because they produced bad output in different ways. Future agents
need to know what's been tried and why each approach failed, so the
same code does not get re-shipped on a fresh attempt at the same
problem.

Per `feedback-trust-user-actions.md`: every UOH press the user made
is real; the engine ran exactly what the trace shows. The two
failed fixes were attempts to bridge structural gaps between
*what the engine animates* and *what arcade tables canonicalize*,
and both fixes broke output in cases the bridge didn't cover.

#### 13.9.1 Reverted attempt #1 — multi-move-merge force-finalize (§13.7.8 originally)

**Target case.** F=4940 reproduction:
- F=4815: close MK BLOCK FINAL emits.
- F=4817: parry-counter HP #1 MOVE_START — `r1: 0→4` (cghi=113).
- F=4844: P1.r1 returns to 0; `attacker_idle` set; **but P2 still in
  blockstun**, so finalize defers waiting on `defender_idle`.
- F=4919: parry-counter HP #2 MOVE_START — fresh `r1: 0→4` edge,
  but `g_cur.active==true` from #1, so the existing
  [`frame_data_overlay.c:559-561`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
  guard suppresses MOVE_START. The engine accumulator
  ([`charset.c:421-457`](../src/sf33rd/Source/Game/engine/charset.c))
  keeps adding cgctrs across move #2 onto move #1's total.
- F=4940: P2 finally exits blockstun, finalize fires with merged
  engine_a=18.

**Approach tried.** Add a guard *before* the `if (!g_cur.active)`
move-start scan that detects `g_cur.active && g_prev[atk].r1 == 0
&& now[atk].r1 != 0` (fresh r1 edge while a move is still tracked),
sets `attacker_idle = g_local_frame - 1` if not already set, calls
`fd_finalize()`, and `fd_reset_move()`. The next iteration then
sees `g_cur.active == false` and the existing MOVE_START path
fires for the new move, resetting `fd_engine_active_count[i] = 0`
and `fd_prev_active_cgix[i] = -1` so #2 accumulates fresh.

**Why it was reverted.** User reverted this fix during the day's
session. Specific failure mode not captured in trace at revert time
— the change was deemed not worth keeping vs. accepting the
multi-move-merge case as a known gap. The user kept this case
documented as OPEN rather than shipping the fix.

**What was at the code site after revert** (verify by reading
[`frame_data_overlay.c:556-558`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)):
just `g_local_frame++` followed by the unmodified
`if (!g_cur.active)` move-start scan. No force-finalize block.

**Open mitigation paths for a future agent.**
- Same approach but capture `engine_a` at attacker_idle time
  (snapshot the value into `g_cur` when r1 returns to 0) so the
  emitted FINAL gets the per-move A even if defender_idle is still
  pending. Today's reverted shape took the engine_a value at
  finalize time, which on F=4940 had already absorbed move #2's
  contribution.
- raw[]-derived A override (§13.7.5) sidesteps the merge for the
  prior-move FINAL since `raw[]` froze at attacker_idle and never
  saw move #2's cells. Doesn't fix the per-move FINAL count for
  move #2 (which has no MOVE_START fired and no raw[] capture), but
  does correct the prior-move A.

**Phase 3 update (2026-07-07).** The shipped `a386e057` mitigation
(§13.7.7 rec #2 — `engine_a` snapshot at `attacker_idle`) targets this
exact case, but the Phase 1 harness could not reproduce the F=4940
mechanism with the only move pair tried: cr.MK's own -3 block
advantage means the attacker's recovery outlasts the dummy's blockstun
by 3 frames, so the "second move starts before defender_idle fires"
window never opens for cr.MK (see §13.7.7 rec #2's Phase 3 note for
the full walk, and `q-crmk-multimove-merge` in
`tools/frame-data/corpus-q.yaml`). This is a corpus-coverage gap, not
evidence the fix is broken — reproducing F=4940 needs a move with
non-negative block advantage (e.g. Jab, +2) so the attacker recovers
before the defender. Until such a corpus entry exists, the shipped
mitigation's effectiveness on this specific case remains unverified.

#### 13.9.2 Reverted attempt #2 — force-finalize on same-r1=4 retrigger (F=1339 case)

**Target case.** F=1339 A=22 reproduction (§13.7.1):
- F=1256: UOH #1 MOVE_START (`r1: 0→4`, sw_new=0x0020 → cghi=229
  next tick).
- F=1257-1295: UOH #1 active and recovery cells. Engine
  accumulator reaches A=11 cleanly.
- F=1294: cgix-reset (40→0). §13.5.1 state engages
  (`cgix_reset_frame=1294`).
- F=1297-1298: cgix=8, cghi=1. §13.5.1 dwell counter at 2 (need
  ≥3 to fire predicate).
- **F=1299: second UOH retriggered.** Trace shows
  `cgix=4 cgctr=4 cghi=229 sw_new=0x0220` while r1 stays 4
  throughout (verified F=1255-1339 walk). The MOVE_START detector
  at [`frame_data_overlay.c:559-561`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
  keys on `g_prev.r1 == 0 && now.r1 != 0` — that edge does not
  fire. **No `r1: 0→4` edge for the second UOH**, so the multi-move-
  merge fix from §13.9.1 wouldn't have caught it either.
- F=1299-1335: UOH #2 active sequence runs. Engine accumulator
  adds another 11 → engine_a=22.
- F=1336: cgix-reset again (40→0). `cgix_reset_frame` stays 1294
  (only set when `< 0`).
- F=1339: §13.5.1 dwell threshold (≥3 cghi=1 frames) finally
  fires; FINAL emits with engine_a=22.

**Approach tried.** Detect "same-r1=4 retrigger" — when a cgix-reset
fires and `cg_ja.atix` becomes non-zero again on a subsequent
fresh active cell *without* an intervening `r1=0` frame — and
treat it as a forced FINAL: emit a MOVE_START annotation for the
new move, clear `fd_engine_active_count[atk] = 0` and
`fd_prev_active_cgix[atk] = -1`, and re-init `g_cur` for the new
move. Goal: split the chained UOHs at their cgix-reset boundary so
each UOH gets its own A=11 and its own raw[].

**Why it was reverted — output corruption.** The fix produced
**S=0 garbage**: FINAL emitted mid-active-phase with cgix=20,
because the cgix-reset+atix-reactivates predicate fires while the
attacker is still in the *active* portion of UOH #1's
post-recovery transit (cgix=20 has jatix=54). Triggering FINAL at
that moment captures `raw_len` mid-active and the per-frame tally
classifies most of it as STARTUP because `seen_active` was reset.
Result: FINAL annotations like `S=0 A=14 raw_len=long` with active
cells split incorrectly across two FINALs. User reverted because
the broken split produced more garbage than the merged A=22 it was
trying to fix.

**What was at the code site after revert.** No retrigger-detection
logic anywhere in
[`frame_data_overlay.c:825-896`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c)
(the §13.5.1 block) or surrounding regions. The block is exactly
the predicate from §13.5.1 + §13.6.1 partner-release; no
engine_a clearing on cgix-reset.

**Open mitigation paths for a future agent.**
- raw[]-derived A override (§13.7.5) — when §13.5.1 cut fires,
  override `engine_a` with the raw[] ACTIVE/CONTACT count. raw[]
  is trimmed by the §13.5.1 predicate to UOH #1's cells only, so
  a raw[]-derived A would yield A=10 for the F=1339 case.
  Trade-off: doesn't preserve the engine-canonical-arcade-A
  property for moves where raw[] under-counts due to hitstop
  sub-frame attacks. Mitigation: use raw[]-derived only when
  `g_cur.cghi1_first_frame >= 0` (§13.5.1 cut fired); fall back
  to engine_a otherwise.
- More-targeted retrigger detection that ONLY fires when the
  predicate is genuinely between two distinct moves (e.g., gate
  on `cghi=1` having held for ≥1 frame between the cgix-reset
  and the new active cell), not during the active-phase transit.
  The reverted attempt didn't have this gate, which is why it
  fired mid-active.

#### 13.9.3 What is currently shipped for retrigger / multi-move cases

**Updated 2026-07-07 — no longer "nothing".** Neither reverted
force-finalize attempt (§13.9.1, §13.9.2) is back in the code. But a
different fix — an `engine_a` snapshot captured at the moment
`attacker_idle` is set (§13.7.7 rec #2 shape) — **shipped in commit
`a386e057`** (`frame_data_overlay.c:114-122, 421-431, 858-868`; see
§13.7.7 rec #2 for the detail). Its scope, unchanged by this update:
- F=4940 A=18 (parry-counter HP chain with **deferred finalize**,
  i.e. a fresh attacker `r1: 0→4` edge fires while the *previous*
  move is still waiting on `defender_idle`): the snapshot is taken
  when the *first* move's `attacker_idle` fires, before the second
  move's cells can inflate the live counter. This is exactly the case
  the shipped fix targets.
- F=1339 A=22 (same-r1=4 retrigger, **no intervening `r1=0`** between
  the two UOHs): `attacker_idle` itself isn't set until the §13.5.1
  cghi=1-dwell predicate fires *after* both UOHs have already
  accumulated into the live counter — there is no earlier "attacker
  idle" moment to snapshot at. **The engine_a snapshot does NOT fix
  this case.** It remains open exactly as described below.
- General case: any time an attacker initiates a second move before
  `defender_idle` from the first fires *and* there was an
  intervening `r1: 0→4` edge, the snapshot now protects the first
  move's A. Same-r1=4 retrigger without an intervening idle edge
  (F=1339's mechanism) is untouched by this fix.

Whether the F=4940 mitigation actually holds is unverified since the
2026-05-04 trace predates the fix — Phase 3 re-baselines with the
harness.

**Phase 3 result (2026-07-07).** F=1339's mechanism (same-r1=4
retrigger, no intervening idle) was **confirmed still OPEN** at first —
the harness reproduced it deterministically with a scripted input
(`q-uoh-chain-retrigger`: `press MP+MK; wait 38; press MP+MK`), single
FINAL, A doubling to 20 (F=1339's own case showed A=22 at a different
retrigger offset — timing-dependent, both are the same mechanism). As
predicted above, the `a386e057` engine_a-snapshot fix did not touch
this case. **It is now FIXED — see §13.9.4** for the shipped
gated-anchor-snapshot mechanism, which is a third, distinct approach
from both `a386e057` and the two reverted force-finalize attempts.
F=4940's mechanism (deferred finalize across a fresh `r1: 0→4` edge)
still could not be exercised by the harness — the only move pair tried
(cr.MK) structurally can't open the merge window (§13.9.1's Phase 3
update, §13.7.7 rec #2's Phase 3 note) — so that fix's effectiveness
remains unverified pending a better corpus entry. This half of §13.9
(the merge case) remains open.

The merge case is **known-broken** until a fix lands (retrigger, the
other half of this section's original scope, is fixed — see §13.9.4).
Per `feedback-no-shipping-wrong-data.md`: don't paper over with
documentation alone, but also don't ship a fix that produces
worse output in service cases. The reverted §13.9.1 broke nothing
new but was deemed not yet worth keeping in its tested shape;
§13.9.2 actively broke isolated UOH output (S=0 garbage) and so
must not be re-shipped without the active-phase guard.

**Step 7 update (2026-07-07, plan `docs/plan-frame-data-completion.md`
Step 7) — Jab tried, precondition reached but not contamination.**
cr.MK could never open the merge precondition at all (its -3 block
advantage means attacker recovery outlasts dummy blockstun). Step 7
retried with Q Jab (+2 block advantage, `S=4 A=4 R=6` per §12/`q.json`)
via a scratch scan `press LP; wait N; press <btn>` (`dist: close,
dummy: stand`), N scanned 6-20, second button LP and MP. Full scan
table (labels/values from the scratch harness run, `move_start_F` /
`atk_idle_F` / `def_idle_F` are the overlay's own `FdMove` fields
embedded in each FINAL line, `frame_data_overlay.c:758-773`):

| N | Result shape | Mechanism |
| --- | --- | --- |
| 6-12 | S=4 A=4 R=6 adv=+2 (baseline-identical), `cut=0 anchor_a=-1` | Press lands mid-active/mid-recovery, before Jab's own cancel window opens; absorbed with zero effect |
| 13-15 | S=4 A=4 R=3..5 adv=+21..+3 (contaminated), `cut=1 anchor_a=4` | Same-r1=4 retrigger landing **during** the cghi=1 dwell count (before the 3-frame commit) — this is the §13.7.1/§13.9.4 retrigger family, not the F=4940 merge family (r1 never returns to 0) |
| 16 | S=4 A=4 R=6 adv=+2 (clean), `cut=1 anchor_a=4` | Same-r1=4 retrigger, but landing squarely on the dwell commit — §13.9.4's gated-anchor fix already protects this cleanly |
| **17** | **S=4 A=4 R=6 adv=+2 (clean), `cut=0 anchor_a=-1`** | **Genuine F=4940 precondition — see raw walk below** |
| 18 | S=4 A=4 R=6 adv=+2 (clean), `cut=0 anchor_a=-1` | Natural r1:4→0 edge; second press coincides with the exact tick `defender_idle` unlocks (near-miss, see raw walk) |
| 19-20 | Two separate FINALs (extra-FINAL FAIL against a `finals:1` placeholder) | Press lands after full finalize+reset; genuinely independent second move |

**N=17 raw per-frame walk** (raw trace, `frame_trace.c`'s own
per-tick F counter, which only advances while a player is active —
see `frame_trace.c:269-283`; column order `F,P1.r1,cgix,cghi,jatix,
swnew,cgcan,P2.r1`):

```
F=18 P1.r1=4 cgix=28 cghi=1 jatix=0 swnew=0x0000 cgcan=0  P2.r1=1
F=19 P1.r1=0 cgix=28 cghi=1 jatix=0 swnew=0x0000 cgcan=0  P2.r1=1   <- attacker genuinely idle (r1:4->0), P2 still blockstunned
F=20 P1.r1=4 cgix=0  cghi=1 jatix=0 swnew=0x0010 cgcan=0  P2.r1=1   <- FRESH LP press (swnew=0x0010, cgix reset to 0): a genuine second Jab starts, P2 STILL blockstunned
F=21 P1.r1=4 cgix=6  cghi=1 jatix=0 swnew=0x0000 cgcan=0  P2.r1=0   <- P2 exits blockstun one tick later; second Jab is still in its own startup cell (jatix=0), no active hitbox yet
```

This is the F=4940 shape reproduced for the first time with a
non-parry-counter, reachable move pair: attacker idle (F=19) strictly
precedes the fresh `r1:0→4` edge (F=20), which strictly precedes
`defender_idle` (F=21) — exactly the "attacker already idle, second
move starts, defender still blockstunned" precondition
`frame_data_overlay.c:114-122`'s `engine_a_at_atk_idle` snapshot
targets. The overlay's own FINAL for this entry (`move_start_F=1
atk_idle_F=20 def_idle_F=22` — deltas 19/2, identical to a solo-Jab
baseline's own `move_start_F=1 atk_idle_F=20 def_idle_F=22`) confirms
the snapshot fires and finalize reads it — engine_a stays a clean 4,
matching arcade exactly. The second Jab's own MOVE_START is silently
never annotated (`g_cur.active` is still true at the top of the F=20
tick, per the `!g_cur.active` gate at `frame_data_overlay.c:827` — a
distinct, minor swallowed-MOVE_START side effect, not scope-relevant
here) — cosmetic only; it doesn't feed back into the first move's
numbers.

**Why contamination itself is still not reachable with this pair.**
`fd_engine_active_count[atk]` (the engine accumulator that would
inflate A if the snapshot weren't preferred, gated at
`charset.c:447` — `cg_ja.atix != 0 || cg_ja.caix != 0`, adds through
`charset.c:509`) only increments while `cg_ja.atix != 0` for a
button-normal second move — i.e. once the second move reaches an
actual active cell. Jab's own startup takes 4 engine ticks from
its `cgix=0` first tick to its first active cell (`jatix` goes nonzero
at local relative offset +4, confirmed both in the baseline's own
walk and independently in the N=17 second-Jab's own would-be
progression). But `defender_idle` fires just **one** tick after the
second Jab's `r1:0→4` edge (F=20→F=21 above) — the deferred window
Jab's own +2 block advantage opens is narrower than any move's minimum
startup-to-active delay, so the second move's cells can never reach
the live counter before finalize resolves. This holds regardless of N:
no landing tick can satisfy "genuine fresh edge" AND "several ticks of
runway before defender exits" simultaneously for a +2-advantage first
move.

**Mutation check (required by the Step 7 spec) — confirms the above,
does not discriminate.** Forced the snapshot preference off at
`frame_data_overlay.c:545` (temporarily rewritten
`(false && g_cur.engine_a_at_atk_idle >= 0)`, so finalize always reads
the live `fd_engine_active_count[atk]` instead of the snapshot),
rebuilt (`cmake --build build/host -j8`), reran the same N=17 scratch
entries: **byte-identical output** (`S=4 A=4 R=6 adv=+2`,
`move_start_F=1 atk_idle_F=20 def_idle_F=22` unchanged from the
unmutated run). This is expected given the walk above — the live
counter and the snapshot hold the same value (4) at finalize time
either way, because the second move never got a chance to add
anything. Mutation reverted (`git checkout --
src/sf33rd/Source/Game/ui/frame_data_overlay.c`), rebuilt, tree
confirmed clean before resuming.

**Bonus check — Crouching Jab (+4 block advantage, double the
window).** Same scan shape (N=13-22) against `press DOWN+LP; wait N;
press LP`: N=15 hit the same-r1=4 retrigger family (`cut=1
anchor_a=4`, clean), all other N in range were either fully absorbed
with zero effect or produced a fully independent second move (N≥20) —
no N reproduced the F=4940 fresh-edge-during-deferred-wait shape at
all for this move (its own natural-idle-to-defender-idle window did
not line up the same way Jab's did in the N range tried). Not pursued
further past this spot-check — Q has no move with meaningfully larger
positive block advantage than Crouching Jab's +4 (see the sorted
`Block_advantage` list: Far Jab/Crouching Jab +4, Short/Jab +2, then
0 or negative for everything else), so scaling this approach up within
Q's own moveset has no further headroom; the original F=4940 bug's
deferred window (F=4844→F=4940, ~96 frames — an order of magnitude
wider than any normal's block-advantage window) came from the
parry-counter's own unusual long-tail animation, not from an ordinary
normal's block advantage, and no in-scope corpus tool reproduces a
parry-counter setup.

**Verdict.** The corpus is unchanged — no `q-jab-multimove-merge` entry
was committed, because the reproducible N=17 case cannot pass the
spec's own bar ("mutation check... entry flags"): the mutation is a
no-op for this reproduction, so committing it as "verifying the
mitigation" would overclaim. What *is* now established, beyond the
pre-Step-7 state: the F=4940 *precondition* (fresh attacker `r1:0→4`
edge while `g_cur.active` is still deferred on `defender_idle`) is
reachable with an ordinary Q normal (Jab, N=17) — cr.MK's "window
never opens" finding was specific to cr.MK's negative advantage, not a
general property of ordinary normals. But genuine **contamination**
(the thing `a386e057`'s snapshot actually protects against) remains
unreached by either button-normal second move tried, in the N ranges
actually scanned (Jab N=6-20, Crouching Jab N=13-22, second button
LP/MP only): no positive-advantage normal among those two opens a
window wider than the minimum startup-to-active delay a button-normal
second move needs. This scoping is deliberate — it is **not** a claim
about Q's full moveset. Q's THROW has `Startup=2` and its own hitbox
(`caix`) gates the same accumulator through `cg_ja.caix != 0`
(`charset.c:447`, §13.6) rather than `atix`, so a throw's
startup-to-active delay is structurally much shorter than a
button-normal's. Crouching Jab's +4 block advantage opens roughly
double the runway Jab's +2 did (Jab's own window was ~1 tick,
F20→F21 above) — on the order of a few ticks, not independently
re-measured here — which may be enough for a throw's short startup to
land an active hitbox before `defender_idle` fires, even though no
button normal's startup is fast enough to do so in the pairs actually
tried. A +4-advantage-first (Crouching Jab) + throw-second pair was
never scanned — Step 7's scans only pressed a second LP/MP, never a
throw input — and remains the one theoretical residual pair this
verdict does not cover. The mitigation's protective code path is
confirmed to *execute* correctly (snapshot capture and finalize-time
preference both fire, verified via the FINAL's own fields) but its
*protective effect* remains unverified for lack of a contaminating
scenario — unchanged from "unverified," now for a precisely
characterized, structural reason instead of "no move tried opens the
window at all," with the unscanned throw-second pair above as the one
gap in that characterization.

#### 13.9.4 Same-r1=4 retrigger — SHIPPED FIX: gated anchor-time engine_a snapshot

**Shipped 2026-07-07 on `frame-data-on-mister` (this change).** `frame_data_overlay.c` ONLY.
Fixes exactly the `q-uoh-chain-retrigger` case (F=1339-style same-r1=4
retrigger, no intervening `r1=0` edge) that neither `a386e057`
(§13.9.3) nor either reverted force-finalize attempt (§13.9.1,
§13.9.2) covered.

**Mechanism.** Two new inert `FdMove` fields:
`s32 engine_a_at_cut_anchor` (init `-1` in `fd_reset_move()`) and
`bool cghi1_dwell_broken` (init `false`). Both are written only in the
existing §13.5.1 tick-side else-if chain and read only at finalize —
pure observation, never mutating live accumulator state (§13.9's G2
requirement).
- **Anchor snapshot:** the first time a cghi=1 frame is seen after the
  cgix-reset (the same anchor §13.5.1 already cuts recovery at,
  `frame_data_overlay.c`'s `cghi1_first_frame < 0` branch), snapshot
  `fd_engine_active_count[atk_idx]` into `engine_a_at_cut_anchor`. This
  is sound only because `frame_data_overlay_tick()` (`main.c:615`) runs
  strictly after the engine tick / `njUserMain()` (`main.c:603`) — every
  `char_move()` add for that frame has already landed by the time the
  overlay reads it. Verified this holds (single call site, no
  intervening `char_move` calls) and that no accumulator add fires
  between tap-1's last active cell and a retrigger's first active cell
  (confirmed both by arithmetic — FINAL total 20 minus per-tap 10 — and
  by a direct replay of the `charset.c` accumulator logic against a
  captured `[CM]` stderr log: net sum of adds through the anchor's
  `Game_timer` = 10, first add after the anchor lands exactly at the
  retrigger's first active cell).
- **Dwell-broken flag:** a new final `else if (cghi1_first_frame >= 0)`
  arm of the same chain sets `cghi1_dwell_broken = true` whenever cghi
  leaves 1 again (while r1 stays 4/2 and `attacker_idle` is still
  unset) before the cut has committed (`cghi1_count` reaching 3). This
  is the retrigger's specific signature: a second press interrupts the
  cghi=1 dwell that a clean UOH would hold for 3+ consecutive frames.
- **Finalize gate:** `use_anchor_a = (cghi1_count >= 3) &&
  cghi1_dwell_broken && (engine_a_at_cut_anchor > 0)`. All three
  conditions are required (§13.9's G3/G4): `cghi1_first_frame >= 0`
  alone is not "cut committed" (Dashing Head/Leg's 1-2-frame cghi=1
  transients set `cghi1_first_frame` but never reach the ≥3 commit),
  which is why the gate requires `cghi1_count >= 3`; "committed" alone
  is too broad (every clean UOH commits, and betting the snapshot
  equals the final `engine_a` for
  every committing move — CnDB whiff, HCB family — was never
  individually verified); the snapshot-positive check is the G4
  fallback (if the anchor was somehow never taken, fall back to
  today's `engine_a`/`active_pf` selection unchanged). When the gate
  fires, `effective_a` (the snapshot) replaces `engine_a` both in the
  numeric `A=` line and in the finalize meter-band repaint; `engine_a=`
  in the FINAL annotation still reports the raw, un-overridden
  accumulator value so the override is visible in the trace.
- **Never fires on §13.6.1's partner-release branch** (CnDB) — that
  branch does not touch `cghi1_count`/`cghi1_first_frame` at all, so
  CnDB's A keeps reading `engine_a` exactly as before.

**Why the raw[]-derived-A alternative (§13.7.1/§13.7.5's original
recommendation) was NOT used.** It was tried first and **falsified**:
the trimmed-raw[] ACTIVE/CONTACT count (`active_pf`) is 13 for this
retrigger and for both clean-UOH BLOCK entries — never 10. See §13.7.5
for the falsification detail (a cg_ctr-sticking artifact inflates
visible on-screen ticks past the accumulator's cgctr-budget lump sum,
even in the already-passing case). `active_pf` and `engine_a` are
structurally different quantities; the anchor-snapshot fix keeps
`engine_a` as the value of record and only changes which point in time
it's read from.

**New FINAL diagnostics.** ` active_pf=%d cut=%d dwellbrk=%d
anchor_a=%d` appended to the FINAL annotation (harness-safe — the
checker's `got` string is built from parsed S/A/R/adv/outcome only,
never the raw FINAL text, so this addition does not affect XFAIL
byte-identity). Example, `q-uoh-chain-retrigger` post-fix: `S=15 A=10
R=5 T=30 ... engine_a=20 ... active_pf=13 cut=1 dwellbrk=1 anchor_a=10`
— `engine_a=20` proves the live accumulator is untouched (still
merges both taps); `A=10` proves the override is what's displayed.
Clean UOH FINALs show `cut=1 dwellbrk=0` (gate never arms, `A=` reads
`engine_a` exactly as before). CnDB/DHA FINALs show `cut=0 dwellbrk=0
anchor_a=-1` (anchor never taken on those paths).

**Verification (Phase 3, 2026-07-07).** `bash tools/frame-data/run.sh`:
57 PASS / 12 XFAIL, exit 0. All 68 non-retrigger entries' checker `got`
strings byte-identical to the pre-fix baseline (verified programmatically
via `check_frame_data.py`'s own `parse_trace`/`evaluate_entry`, not by
comparing raw FINAL lines). Determinism: two consecutive runs of the
full corpus produce byte-identical trace logs. Mutation test A (§1.9.3,
jatix revoke neutralized): now flags exactly **three** entries —
`q-uoh-samef-block`, `q-uoh-1f-block` (both A=11, unchanged from
before), plus `q-uoh-chain-retrigger` (also A=11 — the anchor snapshot
correctly inherits the un-revoked tap-1 total, since it reads the same
live accumulator the revoke used to protect). Mutation test B (this
fix's own lever, `use_anchor_a` forced off): flags exactly
`q-uoh-chain-retrigger` (A=20, the pre-fix value) — proof the new gate
is load-bearing, not a no-op. Both flavors (`telemetry`, `clean`) of
the MiSTer ARM cross-build still compile with the change (no
preprocessor branches in the touched file).

**Accepted residual (unchanged from the original plan's scope).** A
retrigger landing *before* any cghi=1 dwell forms (the anchor would
then be set only after both taps have already accumulated, so the
snapshot would hold both taps' total) leaves `cghi1_dwell_broken`
false — the gate stays off and A is exactly as inflated as before the
fix, never worse. The corpus reproducer's wait=38 timing has the
anchor-first shape (anchor precedes the second press), so it exercises
the fixed path, not this residual.

**UPDATE (2026-07-07, §13.11 declared-truth displayed-A convention
adopted).** The anchor snapshot reads the live `fd_engine_active_count`
accumulator (see "Anchor snapshot" above), and that accumulator now
automatically inherits the §13.11 gated restore with zero changes to
this file — tap-1's declared total is 11 (not 10), so the anchor now
snapshots 11 and `q-uoh-chain-retrigger`'s expect is `A: 11`. This
section's fix stays fully load-bearing exactly as designed: without
it, the display would be the merged live accumulator — now 22 (11 per
tap), not the pre-convention 20 — see mutation lever B below, whose
expected FAIL value is therefore now A=22, not 20. Mutation lever A's
documented 3-entry flag set two paragraphs above
(`q-uoh-samef-block`/`q-uoh-1f-block`/`q-uoh-chain-retrigger`, all
A=11) is also superseded: under §13.11, disabling the revoke (lever A)
is expected to flag **nothing** on any corpus (subtract-then-restore is
arithmetically identical to never subtracting — see §13.11's
path-independence argument), and lever F (the new restore-gate
mutation) inherits the load-bearing proof this paragraph's lever-A
result used to carry.

---

### 13.10 HIT-R display convention (normative; adopted Step 5, 2026-07-07)

**Statement.** The overlay reports the attacker's *actually-played*
post-active recovery for the observed outcome. Arcade tables publish a
single outcome-independent R canonicalized against the block/whiff
path. On HIT these genuinely differ for three documented mechanism
classes; the overlay value is normative for the display and for corpus
`expect.R` on HIT rows, with the arcade figure preserved in the entry
comment. This closes open-work item 15 (§12's "New open-issue family",
§13.8's open-issue table) for all 16 member entries: each is either
explained by one of the three classes below, or (UOH) already explained
by §13.7.3, which this section generalizes.

#### Class 1 — hit-branch recovery chains

**Members:** HSB Jab/Strong/Fierce, Dashing Leg Attack (RH), Hugo
Strong, Hugo Forward (the R clause of `h-forward-hit`'s stacked xfail
only — its issue #17 A clause is a separate, unrelated family).

**Mechanism.** After the last active cell, the engine plays a
*parallel recovery cell chain* selected by hit-vs-block outcome — the
same `cghi` label sequence, but shorter per-cell `cgctr` durations on
HIT, converging on the same terminal cell. This is **not** the
defender hitstun-vs-blockstun differential (that is UOH's §13.7.3
mechanism specifically, Class 2 below) — it is a genuine, data-driven
outcome branch in the *attacker's own animation*. BLOCK matches arcade
exactly for every member in the table below; the engine genuinely
returns the attacker to r1=0 earlier on HIT because it plays a shorter
chain, not because a cut window closed early.

**Full class-1 summary table** (measured 2026-07-07, all FINAL lines
reproduced independently twice — corpus-authoring session + Step 5
diagnosis session):

| Move | BLOCK S/A/R (=arcade?) | HIT S/A/R | R delta | defender-stun differential (HIT−BLOCK) | mechanism |
|---|---|---|---|---|---|
| HSB Jab | 9/3/26 exact, adv −6 exact | 9/3/**21**, adv +1 | −5 | +2 (58→60) ≠ −5 | hit-branch chain |
| HSB Strong | 8/4/28 exact, adv −9 exact | 8/4/**21**, adv +0 | −7 | ≠ | hit-branch chain |
| HSB Fierce | 9/3/28 exact, adv −8 exact | 9/3/**21**, adv +1 | −7 | ≠ | hit-branch chain |
| DLA RH | 31/2/31 exact, adv −15 exact (`q-dla-rh-block`, new Step 5 anchor entry) | 31/2/**24**, adv +35, kd=1 | −7 | n/a — defender is KNOCKED DOWN on hit (P2 r2=16 buttobi); a hitstun differential cannot even be formed | hit-branch chain |
| Hugo Strong | 10/7/12 exact, adv −5 exact | 10/7/**8**, adv +0 (= arcade `Hit_advantage` 0, exact) | −4 | +1 (27→28) ≠ −4 | hit-branch chain |
| Hugo Forward | 9/5/23 exact (R), adv −4 exact | 9/5/**20**, adv +1 (= arcade 1, exact) | −3 | +2 (53→55) ≠ −3 | hit-branch chain |

**Tick-table proof (HSB Jab).** After last active cgix=108 (jatix=70):
BLOCK `120(3) 126(3) 132(3) 138(3) 144(3) 150(3) 156(3) 162(2) 168(2)
234(1) = 26` → r1=0; HIT `180(3) 186(3) 192(2) 198(2) 204(2) 210(2)
216(2) 222(2) 228(2) 234(1) = 21` → r1=0. Same cghi label sequence both
paths (346,347,184,185,186,187,188,200,201,202); different cgix chain,
shorter durations. Skipped-tick counts (wall − raw: attacker hitstop)
identical: 16 vs 16 — not an overlay skip artifact.

**Tick-table proof (Hugo Strong).** After active cgix=24: BLOCK
`32(2) 36(3) 40(3) 44(3) 68(1) = 12`; HIT `52(1) 56(2) 60(2) 64(2)
68(1) = 8`. Converge at cgix=68. cghi labels equal (14,1,1,1).

**Consistency proof that this is engine truth, not accounting.** Hugo
Strong/Forward HIT `adv` comes out **exactly** at hugo.json's
`Hit_advantage` (0 and 1) — the arcade hit-advantage figures are only
reproducible *because* the engine's real attacker recovery is the
shorter hit-branch chain. An overlay that "corrected" HIT R to the
arcade block-canonical figure would make R inconsistent with the same
table's own `adv` column.

**Why not an overlay fix.** Displaying arcade R on HIT would require
the durations of the *not-played* block-chain cells — a counterfactual
playout of ROM cell data, exactly the class Step 1's findings F3/C1/C3
proved unknowable at runtime (mid-chain `code=49` jumps, outcome-
specific branch cells). No CnDB-style engine-visible boundary exists:
the played chain simply IS shorter. Any attempt to resize the window
also runs into both hard STOP rules: no defender-state-conditional
§13.5.1 cut, and no WHIFF/BLOCK R perturbation (every BLOCK row above
is a currently-green arcade-exact entry).

#### Class 2 — §13.7.3 cut-window compression (UOH)

**Member:** UOH (`q-uoh-samef-hit`, `q-uoh-1f-hit`).

Cross-reference only, unchanged: §13.7.3 already fully explains this
member (HIT R=3 vs BLOCK/WHIFF R=5, a genuine defender hitstun-vs-
blockstun differential that shortens the §13.5.1 cleanup-anim cut
window on HIT). §13.10 generalizes the *convention* framing that
§13.7.3 pioneered to the rest of this family; it does not change
§13.7.3's mechanism or its measured numbers.

#### Class 3 — connected grabs (arcade R is the whiff-path figure)

**Members:** Q Throw, Ryu Throw, Hugo Moonsault Press ×3, Hugo Meat
Squasher ×3.

**Mechanism.** What arcade "Recovery" means for a grab is measured,
not conjectured: **it is the whiff-path figure.** Every whiff baseline
captured matches arcade exactly:

| Grab | WHIFF measured S/A/R | arcade S/Hit/R | match |
|---|---|---|---|
| Q Throw (LP+LK) | 2/1/**21** (`q-throw-whiff`, new Step 5 anchor entry) | 2/1/21 | exact, all three |
| Ryu Throw (LP+LK) | 2/1/**21** (`ryu-throw-whiff`, new Step 5 anchor entry) | 2/1/21 | exact |
| Hugo Moonsault (Jab) | 2/2/**36** (`h-moonsault-lp-whiff`) | 2/2/36 | exact |
| Hugo Meat Squasher (Short) | 24/2/**30** (`h-meatsquasher-short-whiff`) | 21~24/2/30 | exact |

On HIT the grab *connects*, and the overlay measures the real
post-capture throw sequence instead — there is no arcade figure for
that span (`Hit_advantage` is `D`/`-`/45-KD in the respective oracle
tables). The measured HIT-R is bounded by whichever overlay move-end
mechanism fires first:

| Entry | HIT S/A/R (measured) | end mechanism | span identity |
|---|---|---|---|
| q-throw-hit | 2/1/**34**, kd=1, T=37 | `cut=1` (§13.5.1 cghi=1 dwell) | capture → first cghi=1 dwell frame: the slam animation's cleanup boundary. `raw_len=37=S+A+R` exactly. (2026-07-08: this cut boundary is now edge-gated by §13.5.1a — its fresh edge is 0→1 at the anchor tick, grounded — and measured unchanged, R=34.) |
| ryu-throw-hit | 2/1/**38**, kd=1, T=41 | `endrel=1` (§13.6.1 partner release), `R = meter_len−S−A = 41−2−1` | capture → defender exits r1=3 grapple |
| h-moonsault-{lp,mp,hp}-hit | 2/2/**67**, kd=1, T=71 | `endrel=1` **+ raw[] saturation**: `raw_len` hits the FD_METER_LEN=72 cap, `endrel` trims 1 → `meter_len=71`, `R = 71−S−A = 67` | defender is grappled (r1=3) for 102 wall ticks; the real capture-to-release span exceeds the 72-cell buffer, so R **saturates** |
| h-meatsquasher-short-hit | 21/2/**48**, kd=1, T=71 | same: `R = 71−21−2 = 48` | saturated |
| h-meatsquasher-forward-hit | 31/2/**38**, kd=1, T=71 | same: `R = 71−31−2 = 38` | saturated |
| h-meatsquasher-rh-hit | 44/2/**25**, kd=1, T=71 | same: `R = 71−44−2 = 25` | saturated — this single mechanism also explains why RH's measured R (25) sits BELOW arcade's 36 while every other family member sits above: S=44 eats the fixed 71-cell budget |

So the identifiable-span test is answered: `measured-R` is
(capture → cut) for Q Throw, (capture → release) for Ryu Throw, and
`(FD_METER_LEN−1) − S − A` (saturated release path) for all Hugo grab
HITs. The delta vs arcade is not a defect in either number — the two
figures measure different paths (hit-path throw sequence vs
whiff-path recovery). T=71's cap behavior is already documented
display policy (`frame_data_overlay.c`, T comment: "Saturates at
FD_METER_LEN for moves whose raw[] capture window is the limiting
factor").

**Explicit FD_METER_LEN coupling.** All six Hugo grab HIT entries
(moonsault-lp/mp/hp sharing R=67, meatsquasher-short R=48, -forward
R=38, -rh R=25 — four distinct saturated value-rows) are **derived**
values, not independent constants: they equal `(FD_METER_LEN−1) − S −
A` with `FD_METER_LEN=72` (`frame_data_overlay.c:42`). If
`FD_METER_LEN` ever changes, all six saturated corpus entries' R
expects move in lockstep and must be re-derived from this formula, not
re-measured from scratch. The identity fits all four saturated
value-rows with zero residual (67=71−2−2, 48=71−21−2, 38=71−31−2,
25=71−44−2) — this also resolves corpus-hugo.yaml's former "not
uniformly larger or smaller" open note: RH's HIT-R sits below arcade
specifically because its large S(=44) eats most of the fixed
71-cell budget, while the other three grabs' smaller S leaves more of
the budget for R.

**Why not an overlay fix.** The arcade figure is the whiff-path
animation, which the engine never plays on a connected grab — the same
counterfactual-playout wall as the strike subfamily (Class 1). No
overlay-visible state contains the whiff chain's durations. (Raising
FD_METER_LEN to "un-saturate" the Hugo grabs was considered and
rejected: it is a display-geometry change to the 72×4px meter, it
would only trade one convention's R for another with no arcade oracle
either way, and it touches nothing this item's success criteria need.)

#### Corpus policy

HIT-R expects on class-1/2/3 rows are convention values citing this
subsection; they are regression anchors for the overlay's measured
behavior, not arcade claims. F3 precedent (plan Step 1 findings): the
arcade oracle already canonicalizes other columns against one specific
path (arcade A = whiff-visible ticks), so a path-canonical R is the
same oracle property, not a new anomaly.

#### Explicit non-actions and why

- **No overlay change.** The counterfactual-chain-playout wall (F3/
  C1/C3) rules out re-deriving arcade-canonical R on HIT for Class 1;
  the same wall rules it out for Class 3. §13.5.1 is protected (revert
  history, §13.9) and no defender-state-conditional cut was introduced.
  E2 byte-identity on every currently-green WHIFF/BLOCK entry holds
  trivially because nothing in `frame_data_overlay.c` changed.
- **No FD_METER_LEN resize.** Rejected as a display-geometry change
  with no arcade oracle for the un-saturated span either way (see
  Class 3 above). **REOPENED (2026-07-13, BUFFER-1, normative note):**
  a capture-depth raise shipped for the unrelated M1 register (§12.2.4)
  — `FD_CAPTURE_LEN=256` now bounds raw[]/measurement, split from
  `FD_METER_LEN=72` which keeps the display geometry this bullet
  protects untouched. This is NOT the rejected "FD_METER_LEN resize":
  the display width was never touched. But it has the SAME side effect
  on the Hugo grab HIT rows this bullet was written to avoid — their
  `(FD_METER_LEN-1)-S-A` derived-convention R is now stale (the true,
  larger post-release span is captured instead) precisely because the
  capture bound (not the display bound) moved. The six Hugo grab HIT
  rows' R is DE-ASSERTED (not re-derived to a new formula — no arcade
  oracle exists for the post-contact span either way, same reasoning
  this bullet already gave) — see §12.2.4's M1 RESOLVED-BY-FIX entry
  for the full accounting and the wider affected-row list (this same
  side effect hits several other characters' basic Throw HIT legs
  too).
- **No blanket cross-family formula.** The convention asserts
  *per-entry measured constants* (Class 1/3) or an already-documented
  mechanism (Class 2, §13.7.3), each independently reproduced twice.
  The Phase 4 formula test (`raw_len - S - engine_a` vs arcade R)
  already falsified the only obvious blanket candidate for Class 1/2
  (fails by 1–14 frames per entry, see §13.7.3's Phase 4 note); Class
  3's `FD_METER_LEN−1−S−A` formula is scoped explicitly to the four
  saturated Hugo grab rows only, not applied elsewhere.

---

### 13.11 Displayed-A convention: declared engine truth (normative; adopted 2026-07-07, user decision)

**Statement.** The overlay displays the declared active credit the
engine's animation data accrues — the existing per-cell `cgctr`
accumulator semantics (`fd_engine_active_count`, §3/§9) — summed at
cell entry, **path-independently**: WHIFF, BLOCK and HIT display the
same A for the same move. Contact must never destroy already-elapsed
declared credit. Concretely, the §13.7.4 same-tick jatix-gated revoke's
subtraction is immediately restored, gated
(`fd_restore_revoked_declared_credit`, `charset.c`, inside the §13.7.4
revoke block — see that section's 2026-07-07 update) — this is
mutation lever F (`docs/plan-frame-data-harness.md` §1.9 item 3). This
resolves open-work item 17 (the "A-undercount-on-contact" finding first
recorded in §13.7's audit and the `corpus-ryu.yaml` MAJOR FINDING
header) by adopting a convention for what "correct" A means, rather
than by finding a further engine bug.

**Industry precedent.** Modern in-game frame meters (SF6's
training-mode frame meter et al.) display measured engine truth —
what the engine's own data says is active — rather than a
reconstructed arcade-table value. This convention follows that
precedent: the engine's declared credit, not the arcade table's
whiff-visible tick count, is definitionally "the A" this overlay
shows.

**The UOH two-cancelling-minus-ones story (from Step 1 finding F3).**
Arcade's published A=10 for Universal Overhead equals the
*whiff-visible tick count* — the number of on-screen active ticks a
clean whiff plays. The engine's *declared* credit for the same move is
1+2+2+3+3 = **11** (§13.7.4's `[CM]` trace walk). The old BLOCK-path
reading of exactly 10 was the sum of **two independent errors that
cancelled**: the §13.7.4 revoke subtracted the arcade-counted contact
cell (removing 1 from the declared 11), while the block path plays the
tail cell in full where whiff truncates it (adding roughly 1 back) —
right number, wrong reason, and the two errors don't actually cancel
for every member of this family (see the Ryu shape-(b) table below,
where the pre-convention measured A undercounts the arcade figure by
1-3, not always exactly 1). Under this convention every path displays
the same declared value, including WHIFF — which the shipped build has
*always* displayed as 11 for Q's UOH (a latent, previously-untested
divergence vs q.json's 10, now pinned correct by the `q-uoh-whiff`
anchor entry in `corpus-q.yaml`).

**DATED NOTE (2026-07-11, §13.17 RE-ANCHOR-1 whiff-A amendment, user
rule):** the previous paragraph's WHIFF claim no longer describes the
shipped tree. The user adopted a rule directly — "whiff active-frames
come from the raw hitbox count" — that makes whiff-leg A measure
§13.17's raw-box count (lever O), not the declared accumulator this
section describes; path-independence across WHIFF/BLOCK/HIT is
correspondingly retired as a claim, WHIFF-scoped. The `q-uoh-whiff`
anchor entry (`corpus-q.yaml`) now asserts **A=10**, not 11 — matching
arcade/q.json, not the declared-truth value this paragraph walks through.
Contact-leg declared-credit A (the BLOCK/HIT entries this paragraph
describes, and §13.11/§13.13's convention generally) is **UNCHANGED** by
this amendment — the rule is whiff-scoped by construction (§13.17's
lever gate, `outcome == WHIFF`). See §13.17 for the full rule, scope,
and census.

**Whiff-visible vs. declared distinction.** These are two different
countings of the same animation data. Whiff-visible = the actually-
rendered on-screen tick count when nothing interrupts playback (what
arcade cabinet frame-counters historically measured, by counting
visible frames). Declared = the sum of each cell's `cgctr` at the
moment the engine's cell-dispatch code enters it, independent of
whether that cell's frames are ever rendered uninterrupted. The two
coincide on every move where contact never triggers a same-tick
cell-chain re-advance (the vast majority of the roster); they diverge
exactly on the shape-(b) contact-collapse windows this section
restores.

**Path-independence property.** Mutating the revoke off (lever A —
forcing the `if (fd_prev_active_cgix_tick[wk->id] == Game_timer ...)`
condition false, which skips both the subtract *and* the restore)
changes nothing on any of the three corpora: subtract-then-restore is
arithmetically identical to never subtracting, for every corpus window
measured (census: at most one same-tick same-jatix transit per attack
window, so there is never a second revoke to interact with the
restore). This is the convention's own self-check — if disabling the
revoke changed a currently-green entry, the restore's arithmetic
identity claim would be false.

**What stays unrecoverable (shape (a)).** Two distinct shapes exist
where contact advances the cell chain *past* a declared cell without
ever entering it at all — there is no accumulator subtraction to
restore in these cases, because the cell's credit never entered
`fd_engine_active_count` in the first place:
- **(a1) skip-jump:** the contact tick jumps `cg_ix` directly from the
  contact cell to a later (often recovery) cell, skipping one or more
  intermediate declared-active cells entirely. Members: `ryu-far-
  strong-block` (skips cgix=12, ctr=3, jatix=6; measured A=1 vs arcade
  4), `ryu-crmk-block` (skips cgix=16(2)+20(2); measured A=1 vs arcade
  5, the largest observed gap), `h-short-block`/`h-short-hit` (measured
  A=1 vs arcade 4), `h-forward-block`/`h-forward-hit` (measured A=5 vs
  arcade 6; `h-forward-hit` additionally carries the unrelated §13.10
  Class-1 R=20 convention clause).
- **(a2) char_move-bypass:** `hit_pattern_extdat_check` case 0x41
  (`hitcheck.c:743-752`) parks `cg_ix` past a declared cell without
  ever calling `char_move` for the skipped span at all — not even a
  same-tick double-dispatch, no accumulator interaction whatsoever.
  Member: `ryu-twdshp-block` (skips cell 40, ctr=2; measured A=4 vs
  arcade 6).
- **Hugo's R-side manifestation:** `h-roundhouse-block`/`h-roundhouse-
  hit` are a *different* member of the issue #17 family (a same-tick
  `cg_extdat=0x80` contact advance, `hitcheck.c:727-729`) that happens
  to leave A exact (jatix 14→15 differs, so the §13.7.4 revoke is
  gated off — this move was never in the A-undercount set) but shows
  up as an R-side divergence instead (measured R=30 vs arcade 28); see
  the Hugo Roundhouse addendum on open-work item 17 for the full tick
  table. Unaffected by this section — no accumulator restore applies.

These nine entries (`ryu-far-strong-block`, `ryu-crmk-block`, `ryu-
twdshp-block`, `h-short-block`, `h-short-hit`, `h-forward-block`,
`h-forward-hit`, `h-roundhouse-block`, `h-roundhouse-hit`) remain
permanently xfail-documented: their declared credit either never
enters the runtime (a1/a2) or is a distinct R-side mechanism
(Hugo's roundhouse pair), so there is nothing for this convention's
gated restore to act on. The Step 1 findings appendix's BLOCKED
diagnosis of these shapes is unchanged and is now absorbed as the
normative unrecoverability statement for this family.

**UPDATE (2026-07-10, §13.13):** 7 of these 9 entries (the A-side
ones — `ryu-far-strong-block`, `ryu-crmk-block`, `ryu-twdshp-block`,
`h-short-block`, `h-short-hit`, `h-forward-block`, `h-forward-hit`)
now convert to a plain `PASS` under §13.13's contact-path display
convention: "permanently xfail-documented" described the state before
a convention existed for what to do with an unrecoverable-but-proven
contact-path value, not a claim that the value could never become
normative. The remaining 2 (`h-roundhouse-block`, `h-roundhouse-hit`,
the Hugo Roundhouse R-side pair) are unaffected and stay exactly as
described above — they are excluded from §13.13's named scope (see
§13.13's exclusion list item 4).

**FD coupling / lever documentation.** Full re-derived lever matrix
(all three corpora; see `docs/plan-frame-data-completion.md`'s E3
table and `docs/plan-frame-data-harness.md` §1.9 item 3 for the
authoritative anchors and mutation commands):

| Lever | Mutation | Expected flags after this convention |
|---|---|---|
| A — jatix revoke | force condition false | **None** on any corpus — re-purposed as the path-independence invariant check (see above) |
| B — anchor-A gate (§13.9.4) | force `use_anchor_a` false | `q-uoh-chain-retrigger` FAIL **A=22** (was A=20 pre-convention — both taps now accrue 11 each) |
| F — declared-truth restore gate (new) | `fd_restore_revoked_declared_credit = 0` | Q: the 5 UOH entries + `q-uoh-chain-retrigger` FAIL at A=10 (old measured value). Ryu: the 11 flipped shape-(b) entries (table below) FAIL at their old measured A. Hugo: none (zero revoke sites, census 2026-07-07) |

**Flip table (Ryu shape-(b), 11 entries, old measured A → new
displayed A = arcade):** `ryu-far-jab-block` 2→3, `ryu-strong-block`
2→4, `ryu-fierce-block` 3→4, `ryu-far-fierce-block` 1→3, `ryu-forward-
block` 3→5, `ryu-roundhouse-block` 3→5, `ryu-strong-hit` 2→4,
`ryu-fierce-hit` 3→4, `ryu-forward-hit` 3→5, `ryu-roundhouse-hit` 3→5,
`ryu-crlk-block` 2→3 (census-resolved this session as shape (b) —
Step 1's diagnosis had left it untraced; it is not shape (a) despite
the earlier "stays with shape (a)" grouping suggestion). All 11 now
read their exact arcade A and are plain PASS, not xfail.

**Known adjacent divergence, NOT covered by this convention (open-work
item 18).** UOH's WHIFF R measures 3 vs q.json's 5 — an anim-reset/cut
truncation mechanism, structurally similar to but distinct from
§13.7.3's HIT-R differential and not resolved by the A-side convention
above. `q-uoh-whiff` deliberately does not assert R (leaving the
divergence unmeasured would be silent); the companion `q-uoh-whiff-r`
entry asserts the arcade R=5 as a documented xfail against the
measured 3, so the divergence stays visible to the harness. This is a
distinct, still-open item, tracked separately from issue #17.
(Update 2026-07-08: item 18 was split by the §13.5.1a cut-gate fix;
this paragraph now maps to RESIDUAL item 18(b) — grounded fresh-edge
early-cut — see the rewritten item 18 in §13.8.)

**Dated note (2026-07-10, ENGINE-5 closure — a further known adjacent
divergence, also NOT covered by this convention):** `urien-uoh`'s own
same-cgix, non-sentinel self-loop re-dispatch (§12.2.4's "Same-cgix
non-sentinel self-loop" row) is a THIRD declared-entry shape this
convention does not currently credit — a same-cell re-dispatch
(`charset.c:439-444`/:675-712 reload path) is, by this section's own
"Declared = the sum of each cell's `cgctr` at the moment the engine's
cell-dispatch code enters it" text, a genuine cell entry, but the
current crediting gates (new-cell branch / sentinel-dwell branch) both
miss it, plateauing this move's declared sum at 9 instead of a coherent
11 (played) or 12 (declared-literal). Unlike the Ryu/Hugo shape-(a)
family above, this is not a "no accumulator interaction" case — the
mechanism is proven and the fix design is banked — but the credit
AMOUNT is unreachable by any principled rule not fitted to n=1 (see
§12.2.4's closure entry for the four-quantity table). No expect or
display change follows from this note; a possible future USER
convention grant is flagged there, not decided here.

**Verification record (2026-07-07, same-binary protocol).** Measured
this session on a scratch instrumented build (the restore + a `[RVK]`
revoke census `fprintf` in `charset.c`; built, run on all three
corpora, then reverted via `git checkout` + rebuild + re-verified
green before the convention was implemented for real). Baseline (clean
tree): Q `total=71 (PASS=71)`, Ryu `total=37 (PASS=23, XFAIL=14)`,
Hugo `total=30 (PASS=24, XFAIL=6)`. Revoke census: Q fires in exactly
the 5 UOH windows (`prior_add=1` each); Ryu fires in exactly 11
windows (the flip table above); Hugo fires in zero windows
corpus-wide. Post-implementation (this commit): Q `total=73
(PASS=72, XFAIL=1)`, Ryu `total=37 (PASS=34, XFAIL=3)`, Hugo `total=30
(PASS=24, XFAIL=6)`, all exit 0.

### 13.12 Hit-checkable projectile split (ENGINE-4, design E4-A′, lever J) — SHIPPED 2026-07-09

**Bug (found via the cast rollout, twelve-N.D.L.).** The Phase 6A
"arcade-split" projectile design (§ file reference index, `use_proj_split`
branch of `fd_finalize()`) anchors `S = proj_spawn_slot` and
`R = meter_len − proj_spawn_slot`, validated only against Hadouken-style
fire-and-forget fireballs (ryu/ken/akuma/chunli/oro/remy/urien — all
`kind_of_tama=0`, `kotp_00000`). Twelve's N.D.L. (`kind_of_tama=13`,
`kotp_13000`) measured S undershooting arcade by a uniform 3
(S=12/12/13 vs arcade 15/15/16) and R overshooting by 12-14 (R=26/30/34
vs arcade 14/16/20) on all three strengths, WHIFF included — while
`proj_a` (the projectile accumulator) and BLOCK advantage both already
matched the oracle exactly, proving correct move identity and correct
wall-clock timing; only the engine's own S/R decomposition disagreed.

**Mechanism (eff13.c/hitcheck.c, engine-grounded).** The projectile
hit-check requires BOTH `cg_ja.atix != 0` AND `att_hit_ok != 0`
(`hitcheck.c:1618-1623`, `attack_hit_check()` — the same pair that feeds
`fd_engine_hitbox_active`). Measured tama timelines: fireballs arm
`att_hit_ok` the SAME tick they spawn (athok offset = spawn slot − 1),
so `S = spawn_slot` is already exact for them. N.D.L. arms `att_hit_ok`
TWO CHART CELLS AFTER its own `atix` first goes nonzero — a genuine
"hit-checkable projectile split" the old anchor never modeled — so its
arcade S (15/15/16) sits 3 slots later than its spawn slot (12/12/13).
Separately, N.D.L.'s recovery window is bounded by a CHART-NATURAL end
(the mid-chart tick its own `atix` returns to 0 while the chart keeps
running, uncut), not a fire-and-forget meter-length budget: EVERY
`kotp_00000` (fireball) ending is a chart CUT to an erase chart — chart
end (`eff13.c` case 0, `cg_type==0xFF` → `ernm`), ground touch (→
`erex`), life_time/off-screen timeout (→ `ernm`), or on-hit/on-deflect
consumption once vital depletes (case 1, → `erht`/`erdf`/`erex`) — but
`kotp_13000`'s consumption handler (case 1) only clears `att_hit_ok` and
keeps calling `char_move()` on the SAME chart (case 2) until the
chart's own scripted expiry; it never cuts.

**Fix (Design E4-A′, gated by lever J).**
`S' = max(proj_spawn_slot, proj_athok_slot)`, where `proj_athok_slot`
latches the tama's own hit-check-gated arming tick
(`fd_engine_proj_hitok`, set in `charset.c`'s tama branch on
`atix != 0 && att_hit_ok != 0`). `max()` is a byte-identical no-op for
every fireball (athok offset ≤ spawn slot always) and picks up the
later anchor only for N.D.L. On a CHART-NATURAL end
(`fd_engine_proj_natend`, set when `atix` returns to 0 after having been
active with no cut ever recorded for that tama),
`R' = meter_len − (proj_firstact_slot + proj_a)` — the declared
active-window form (both terms freeze-immune, outcome-independent);
otherwise the legacy `R' = meter_len − S'` form is unchanged. The
chart-cut kill-reason itself (`fd_engine_proj_cut`) is written by a tiny
helper, `fd_tama_chart_cut()` (`eff13.c`, next to `kotp_00000`), called
at all four `kotp_00000` erase transitions plus `kotp_13000`'s own
(unreached-within-corpus) chart-end transition — deliberately NOT called
in `kotp_13000`'s consumption handler, since that is precisely the
no-cut path the gate keys on. All three new engine arrays follow the
existing `fd_engine_proj_spawned` sim-write-only class (`workuser.{c,h}`
— write-only overlay feed, no gameplay reads, excluded from
serialization, see that file's netplay-scope comment).

**Two implementation-time races, found only by direct trace instrumentation
(neither anticipated by the original design, both fixed before shipping).**

1. **`fd_engine_proj_hitok` must be edge-triggered, not level-triggered.**
   A WHIFF tama's `att_hit_ok` stays 1 for the rest of its flight
   (`hitcheck.c` only clears it on an actual confirmed hit) — a naive
   "set to 1 whenever the pair holds" write re-arms the flag every
   subsequent tick, including well after the overlay has already
   consumed it once. Because the very next move's `MOVE_START` block
   pre-latches a still-set `fd_engine_proj_hitok` as "armed at slot 0"
   (mirroring `fd_engine_proj_spawned`'s own legitimate same-tick
   caveat), a stuck-at-1 flag from the PRIOR move's tama silently
   corrupted the NEW move's `proj_athok_slot` to 0 — measured: 8 of 9
   N.D.L. rows (every one but the very first, `twelve-ndl-jab-whiff`)
   read `athok=0` at finalize, pinning `S` at the legacy spawn-slot
   value. Fix: `fd_engine_proj_hitok_armed[2]` (charset.c-internal,
   never read by the overlay) tracks whether the tama was hit-checkable
   on the PREVIOUS tick; `fd_engine_proj_hitok` is written only on the
   0→1 transition and `_armed` is cleared whenever `atix` reads 0 (chart
   inactive) or at `MOVE_START` — so the flag can never be re-asserted
   after its own one-time consumption, for either the same tama or a
   fresh one.
2. **`fd_engine_proj_natend` must exclude the in-flight-to-consumption
   window and be re-checked against `fd_engine_proj_cut` at overlay
   read time, independent of charset.c's own write-time gate.** The
   false-write opportunity on a confirmed hit is a same-invocation
   ordering hazard, not a cross-tick one: inside `kotp_00000` case 1,
   `set_char_move_init(erase)` (`eff13.c:365/367/370`) runs its own
   internal `char_move()` call (`charset.c:126`) — the very check this
   comment documents — on the fresh erase chart (`atix` reset to 0)
   while `wu.hf.hit_flag` is still set and before `fd_tama_chart_cut()`
   (`eff13.c:377`) has run, a few statements earlier in the SAME
   `kotp_00000` invocation (measured directly on `ryu-had-lp-block`'s
   contact tick: `atix=0, hitflag=16, cut=0`, moments before `cut`
   becomes 1 later in that same call). charset.c's write-time check
   therefore raced `fd_tama_chart_cut()` and lost, incorrectly latching
   `natend=1`. Guard ablation (review) pins down the in-corpus blast
   radius precisely: dropping either the write-time `hf.hit_flag==0`
   gate or the overlay's read-time `fd_engine_proj_cut` recheck alone
   leaves the full 19-corpus golden check green (each guard alone masks
   the whole race); dropping BOTH reproduces it on exactly 12 rows and
   nothing else — `ryu-had` (R 36→34 across all 6 legs) and
   `chunli-kik` (R off by 2, i.e. 45/38/32→43/36/30 across all 6 legs).
   This same 12-row set is what the bug originally regressed when it
   was first caught, pre-fix, by re-running the full 19-corpus golden
   check after the first implementation pass. Separately, kind-0's own
   case-0-internal cuts (chart-end/ground/timeout) race the SAME write
   the OTHER way: `fd_tama_chart_cut()` is called strictly AFTER the
   `char_move()` call (where this check lives) within the SAME
   `kotp_00000` invocation, and those sites carry `hit_flag==0`
   throughout (case 0 only runs when `hit_flag` was false at this
   tick's `kotp_00000` entry) — so a write-time read can never see a
   cut that hasn't been recorded yet even when one is about to be, on
   the same tick, and the `hf.hit_flag==0` clause is a no-op for this
   direction. Fix: charset.c's write gate adds `wu.hf.hit_flag == 0`
   (excludes the measured consumption-window race — inert for N.D.L.,
   whose own `hit_flag` is long cleared by the time its true mid-chart
   edge arrives many ticks later); the overlay's own consume site
   ADDITIONALLY re-checks `fd_engine_proj_cut[atk] == 0` at read time
   (`frame_data_overlay.c`, after `njUserMain()` has fully completed
   that engine tick — including any later-in-the-same-call cut —
   independent of whatever charset.c's write-time read saw), covering
   the case-0-internal race as a second, deliberate defense-in-depth
   layer — not redundancy, since each guard covers a different
   direction of the same-invocation race.

**Full-population verification (`tools/frame-data/run-suite.sh
--check-golden`, all 19 corpora).** After both fixes: exactly the 9
`twelve-ndl-{jab,strong,fierce}-{whiff,block,hit}` rows drift
(XFAIL→XPASS, S=15/15/16 and R=14/16/20 on all three strengths — arcade
exact) and NOTHING else — all 45 other `proj=1` rows across 7 fireball
characters (ryu/ken/akuma/chunli/oro/remy/urien) are byte-identical to
their pre-fix golden values, confirming both new anchors are true
no-ops for the fire-and-forget population.

**Mutation verification (lever J).** `fd_proj_hitcheck_split` forced to
`0` (the legacy spawn/meter-end anchors only; the engine-side latches
stay unconditional write-only feeds — no rebuild of the engine paths
needed to restore goldens after restoring the lever, only the overlay):
flags EXACTLY the 9 twelve-ndl entries, regressing to their pre-fix
measured values (S=12/12/13, R=26/30/34); all 45 fireball rows remain
byte-identical under both lever settings — that identity is itself
load-bearing (a lever-G-style negative: the new anchors are inert unless
a tama's own hit-check arms later than its spawn AND its ending is a
chart-natural end, neither of which any fireball ever exhibits).

**R-gate falsification ledger (complete history, prior candidates
falsified before the shipped chart-cut kill-reason gate).**

1. *Guard-flag re-arm (lever-H analogue)* — falsified: N.D.L.'s
   `guard_flag` stays 3 until the natural r1 edge; no actionable signal
   at arcade's own 29/31/36.
2. *cg_cancel window edges* — falsified: `cgcan` open/close edges match
   no strength's arcade actionable point.
3. *Wall-clock atix→0 natural end (R = T − edge, no declared-form
   arithmetic)* — falsified: exact on WHIFF legs but +1 on CONNECT legs
   (the contact freeze skips one tama tick) — resolved by switching to
   the declared form (`firstact_slot + proj_a`), which gives the correct
   14/16/20 from the same underlying data on all 9 legs, WHIFF and
   CONNECT alike. (This is the census_post predR artifact: the
   post-census wall-clock-derived R, `T − natend_off`, skews by exactly
   the contact-freeze tick on CONNECT legs; it resolves to the same
   declared form above, not a separate fix.)
4. *athok-at-edge gate (the original design draft)* — falsified:
   `att_hit_ok` is a consumable hit permission
   (`hitcheck.c:1564/1567`), cleared at contact and held at 0 by
   `kotp_13000`'s own consumption handler for the tama's remaining
   life — never set at the atix→0 edge on any CONNECT leg, so this gate
   would never fire on 6 of the 9 rows.
5. *Erase-STATE read at the edge* (`routine_no[1] != 2` or
   `routine_no[2] == 0`) — falsified by code: `kotp_13000`'s consumption
   sets the SAME state codes a genuinely-cut fireball uses, while its
   chart keeps running — indistinguishable by state alone.
6. *atix-persistence tick-count heuristic* ("survived ≥N ticks past
   contact") — rejected unmeasured: arbitrary N, the heuristic class
   §13.6.2's own STOP-rule precedent bans; superseded by the
   event-based cut latch (the engine states its own kill reason — no
   need to infer it from timing).
7. **Chart-cut kill-reason gate (shipped)** — verified on the full
   54-window `proj=1` population (45 fireball + 9 N.D.L.), zero
   mispredictions, after the two implementation-time race fixes above.

**Residuals (documented, not fixed, not exercised by any of the 19
corpora).**
- *Split active window* (an `atix` gap mid-chart, then a re-arm) would
  latch its FIRST gap as the natural end — no such move exists in the
  corpus (every window is contiguous-active up to its terminal edge);
  a first future instance needs its own capture before either fixing or
  ruling this out.
- *Future tama kinds*: the cut latch is instrumented for kinds 0 and 13
  only (the only kinds the `proj=1` corpus population exercises). A
  future corpus row whose projectile uses another `kotp_NN000` needs its
  own erase-transition audit and its own `fd_tama_chart_cut()` call
  sites, or a post-contact erase chart could masquerade as a natural
  end — this fails toward the legacy DECLARED-branch form, visible as
  an R mismatch vs the oracle at corpus-add time (the suite catches it).
- *Reflected projectiles* (`master_id` flip) and multi-projectile
  overlap stay out of scope — same saturating once-per-move convention
  as the existing spawn latch.

**SHIPPED 2026-07-09 on `frame-data-on-mister`.** Observer-only: no
gameplay behavior change; all new engine state is write-only overlay
feed of the existing `fd_engine_proj_spawned` class. See
`docs/plan-frame-data-completion.md`'s ENGINE-4 tracker row and lever J
row, and `docs/plan-frame-data-harness.md` §1.9 item 3's lever J
paragraph.

**Dated note (2026-07-10, ENGINE-6 census — scope clarification, not a
correction):** this section's own claim is scoped strictly to the
projectile path (`proj=1`) and makes no general claim beyond it. The
§12.2.4 S-divergence row's "ENGINE-4/lever-J already fixed the
identical pair on the projectile path" phrasing is accurate as written
but should not be read as evidence the same fix generalizes: the
ENGINE-6 census (`e6-census-report.md`) tested the analogous
`max(first_active, athok_armed)` idea on the general-classifier path
(`proj=0`) and falsified it as a universal rule — two currently-PASS
families (`remy-crfierce`, `twelve-backforward`) key arcade Startup on
the cell-load tick, not the arm tick, for an engine shape byte-identical
to `yang-forward`'s (which DOES key on the arm tick). Nothing in this
section's own scope is affected — no fireball or N.D.L. row changes.

---

### 13.13 Contact-path display convention: the meter shows the played chain (normative; adopted 2026-07-10, user decision)

**Statement.** The overlay reports the cell chain the engine *actually
played* on the observed path. Arcade tables are whiff-canonical — a
single, outcome-independent set of numbers measured (or computed) off
the clean whiff animation. For three specific, mechanism-proven
divergence families, contact provably re-routes the engine's own chart
(by skipping declared-active cells entirely, by branching into a
genuinely shorter or longer parallel recovery chain, or by branching
into a chart with extra declared-active cells), and the resulting
measured value is not a bug to chase — it is what the engine's frame
meter, by definition, should show on that path. Where one of these
three families is established for a given entry (per the Membership
rule below), the corpus's `expect` for the affected field becomes the
measured engine-truth value, a plain normative `PASS`, with the arcade
(whiff-canonical) figure preserved verbatim in the entry's comment
block and cited as `arcade`. This is not a new mechanism-finding
activity — every conversion below cites a mechanism this doc already
established (§13.11's shape (a1)/(a2), §12.2.2, or §12.2.3); §13.13
only supplies the missing normative disposition for entries that
mechanism already covers but which item 17/§13.11 or the Phase-6
registry sweep left `xfail`-documented pending this convention
decision.

**Industry precedent.** As with §13.10 and §13.11, modern in-game
frame meters (SF6's training-mode frame meter et al.) display measured
engine truth — what the engine's own animation data says is active or
recovering on the path actually taken — rather than reconstructing a
static, whiff-canonical table value. A meter that silently displayed
the un-taken path's numbers on contact would be lying about what the
player just watched happen on screen; §13.13 completes the same
principle §13.10 applied to HIT-R and §13.11 applied to displayed-A,
extended now to the specific contact-path re-routing shapes proven
below.

**Families covered.**

- **F1 — shape-(a) contact A-undercount** (issue #17, `docs/frame-data-synthesis.md:3577-3636`;
  §13.11 "what stays unrecoverable", `:4750-4787`). On the contact
  tick, the engine advances `cg_ix` a second time within the same
  `Game_timer` tick, either (a1) jumping directly past one or more
  declared-active cells without ever entering them, or (a2) via
  `hit_pattern_extdat_check` case `0x41` (`hitcheck.c:743-752`) parking
  `cg_ix` past a declared cell without calling `char_move` at all. In
  both shapes the skipped cell's declared credit never enters
  `fd_engine_active_count` — there is no accumulator subtraction to
  restore (unlike §13.11's shape (b)), so the credit is not merely
  hidden, it never existed at runtime. §13.11 itself makes this
  argument for why the value is unrecoverable by any accumulator fix;
  §13.13 takes the next step and asserts that the resulting lower
  value **is** the correct displayed value, precisely because it is
  the only value the engine ever computed on this path. Per-entry
  membership requires an unhedged shape-(a1)/(a2) classification plus
  either a recorded skip/bypass trace citation or a lever-F
  toggle-test showing the value UNCHANGED with the §13.11 restore
  disabled (proving no accumulator revoke is in play — this is purely
  a skip, not a shape-(b) restore candidate).
- **F2 — contact-branch recovery shortening** (§12.2.2,
  `docs/frame-data-synthesis.md:687-715`; generalizes §13.10 Class 1 to
  contact outcomes generally, not just HIT). On contact the chart
  branches into a parallel recovery chain that is genuinely shorter (or,
  per the registry's explicit R-side corollary, occasionally longer)
  than the whiff chain. Every member reads S/A/adv exact with only R
  diverging — and the arcade table's own live-measured `adv` column is
  the proof this is engine truth, not a defect: `adv` is derived from
  the same real attacker-recovery event R is, so an entry whose R
  "should" read the whiff-canonical figure would make that same row's
  own `adv` inconsistent with itself. The measured R is not competing
  with the arcade figure for correctness — the two numbers describe two
  different chains (the one actually played vs. the one whiff would
  have played), and the played one is what the meter must show.
  Per-entry membership requires either explicit §12.2.2 registry
  membership, or (registry-absence alone is not disqualifying) an
  unhedged classification carrying its own recorded skip/branch `cgix`
  signature for *this* move — a bare precedent citation to a sibling
  move's signature ("same shape as X") does not meet this bar and
  stays a candidate.
- **F3 — contact-branch declared-credit A-overcount** (§12.2.3,
  `docs/frame-data-synthesis.md:717-745`; "the mirror of issue #17").
  The same-tick contact advance re-routes the chart onto an
  outcome-specific branch containing *extra* declared-active cells,
  each of which banks its declared `cgctr` at entry (§13.11's
  declared-truth accounting), while the interrupted cell keeps its own
  already-banked credit. Sign-coherent with F1: F1's branches skip
  declared-active cells (undercount), F3's branches insert them
  (overcount) — same underlying same-tick re-route mechanism, opposite
  direction. Established via credit-ledger trace (`char_move()`'s
  `fd_engine_active_count` update, env-gated `fprintf` census,
  reverted before shipping) for exactly one move, `ken-forward`
  (st.MK): whiff banks cgix 20(add=4)→44(add=3)=7=arcade; BLOCK
  contact banks cell 20's declared 4 then branches into
  32(add=2)→36(add=1)→44(add=3)=10; HIT branches differently —
  20(4)→36(1)→44(3)=8. Both previously "unprecedented" wrong values
  reproduce exactly and are now explained as one engine behavior.
  Per-entry membership requires the entry's own CLASSIFICATION (not a
  passing numeric aside or cross-reference) to cite §12.2.3
  specifically, backed by its own credit-ledger trace — the registry is
  explicit that every other signature-matched A-overcount sighting is a
  **candidate only, NOT individually re-traced**, and candidates never
  convert under this section.

**Membership rule.** An entry (or, for a stacked/compound xfail
string, an individual clause within it) is a §13.13 member if and only
if its own classification — not a passing mention, not a sibling's
finding merely referenced by name — cites one of the three mechanisms
above unhedged, meeting that family's bar as stated. Established
members (F1: shape (a1)/(a2) + lever-F test or skip/bypass citation;
F2: registry membership or an unhedged classification with its own
recorded skip/branch signature; F3: registry-established, i.e.
`ken-forward-block`/`-hit` only) convert. Signature-matched but
"candidate"/"NOT individually re-traced"/"HYPOTHESIS ONLY" sightings do
**not** convert, regardless of how sign-coherent they look — the
registry itself draws this line (§12.2.2's and §12.2.3's own
"candidates ... stay UNCLASSIFIED" language) and this section does not
loosen it. A stacked/compound entry converts per-clause: if every
blocking clause is a member, the entry becomes a plain `PASS` (`xfail`
removed entirely); if only some clause(s) are members, those
field(s) convert and `xfail` is narrowed to cite only the residual,
still-open clause(s); if no clause meets the bar, the entry is
untouched. A sibling-reference clause ("same COMPOUND finding as
`<X>`", with no independent mechanism citation of its own) inherits
the disposition of the entry it names — the audit resolves the
reference rather than requiring the evidence to be restated verbatim
in every sibling entry (the corpus's established relocate-don't-
duplicate authoring style).

**Entry notation.** Zero tooling changes — `compile_corpus.py` already
accepts an explicit int literal or the string `from-qjson` for every
`expect.*` field (`compile_corpus.py:553-560`), and golden files store
only the measured value and verdict, never the free-text `xfail`
reason (`golden.py:10-28`), so re-wording or relocating that text
cannot drift a golden. For a converting field: `expect.<field>`
becomes the explicit int literal equal to the golden-measured
engine-truth value (replacing `from-qjson` for that field only); the
`xfail` key is removed (full convert) or narrowed to the residual
clause (partial convert); and a grep-able comment header is added
directly above the entry, in this order: (1) the literal string
`§13.13 contact-path convention` (F2b/§13.10-compliance conversions —
see below — use `§13.10 HIT-R convention` instead, since they are not
new §13.13 scope), (2) the engine-truth value and which path it is
truth for, (3) the family/mechanism citation, carried over verbatim
from the pre-conversion `xfail` string (no evidence is deleted, only
relocated), and (4) the arcade (whiff-canonical) value, labeled with
the word `arcade`. The same F2b/§13.10-compliance variant applies to a
partial convert's own narrowed `xfail` string prefix: F2b partials use
`NARROWED 2026-07-10 (§13.10 compliance, F2b):` instead of the plain
`NARROWED 2026-07-10 (§13.13):` prefix every other partial convert
uses (see `dudley-jetup-lp-hit`/`-hp-hit`) — so a grep for narrowed
entries should match `NARROWED 2026-07-10 (` (the date, not the
section literal) to catch both variants. Example (mirrors the shipped
§13.10/§13.11 comment style):

```yaml
# §13.13 contact-path convention (adopted 2026-07-10, user decision): A=1 is the
# engine-truth credit actually accrued on the BLOCK path - issue #17 shape (a1)
# skip-jump, declared active cell(s) skipped same tick, never dispatched
# (lever-F-tested UNCHANGED; evidence relocated verbatim from this entry's
# pre-convention xfail string). Arcade (whiff-canonical, from-qjson) A=5 -
# preserved here per the convention. S/R/adv remain arcade-exact (from-qjson).
- label: chunli-short-block
  ...
  expect: { S: from-qjson, A: 1, R: from-qjson, adv: from-qjson }
```

**§13.10-compliance conversions (F2b, adjunct, not new §13.13 scope).**
A separate bucket of xfails, authored during the Phase-6 cast rollout,
already unhedged-cite §13.10 Class 1 (hit-branch recovery chains,
generalized to non-HSB/Hugo movesets) but were left `xfail` rather than
asserted, contrary to §13.10's own corpus policy ("HIT-R expects on
class-1/2/3 rows are convention values citing this subsection",
`:4653-4656`) and the authoring runbook's decision tree (bucket 2,
`CORPUS-AUTHORING.md:340-362`, which commands assertion, not `xfail`).
Converting these is **§13.10 compliance**, not new §13.13 scope — they
cite `§13.10 HIT-R convention` in their comment header, not §13.13, and
are tabulated separately (see the tracker row in
`docs/plan-frame-data-completion.md`). The same per-clause / sibling-
reference rules above apply; two members in this bucket stack an
unhedged F2b R-clause with a hedged §12.2.3-candidate A-clause and
convert PARTIAL (R only).

**F3 member-set amendment (2026-07-10, classification sweep #2
fable-grade audit).** The membership rule's parenthetical above ("F3:
registry-established, i.e. `ken-forward-block`/`-hit` only") is
AMENDED, not rewritten: SWEEP-2 performed the per-entry own-ledger
reconciliation the rule requires (preserved `[P1ledger]` streams,
`s3-rundir/run-{sean,remy,twelve,dudley}/run.log`) and established 16
further F3 members, each with its own credit-ledger citation:
`sean-ryuubi-lk-block/-hit`, `sean-ryuubi-mk-block/-hit`,
`sean-ryuubi-hk-block` (PARTIAL — A-clause only) and
`sean-ryuubi-hk-hit` (inserted cell 68(add=2), +2 exact, all six legs);
`dudley-jetup-lp/mp/hp-block` (inserted cell 36(add=4), +4 uniform);
`dudley-jetup-lp-hit` (+8: insert 36(4) + re-route 48/60/66→120/144/150
loop); `dudley-jetup-hp-hit` (−4: insert 36(4) + re-route into the
SHORTER 168/192/198 chain — same one-mechanism re-route, F1 direction;
lever-F already tested UNCHANGED per its own string);
`twelve-crfierce-block/-hit` (+11 = 12 inserted − 1 displaced cell 44);
`twelve-crroundhouse-hit` (+4 = four add=2 branch cells replace four
add=1 cells); `remy-rrf-lk-block/-hit` (+1 = branch cells 56/60/64
replace displaced cell 20(2)). See §12.2.3's SWEEP-2 F3 member-set
amendment for the full member list and the sean-ryuubi-hk-block partial
residual (§12.1 row 9 context-sensitivity note).

**F2 member-set amendment (2026-07-11, classification sweep #3
fable-grade audit).** SWEEP-3 performed the per-tick recount F2's own
bar requires (preserved `[P2tick]` streams, `s5-rundir/run-makoto` +
`s12-rundir/run-urien-full`) and established 7 further F2 members, each
with its own recorded skip/branch cgix signature: `makoto-hayate-lp/mp/
hp-block` (+8, dwell differential 19 vs 11 ticks, detour 40/44/48 (6)
replacing whiff's 0/4/8 (6) — R-surplus direction) and `makoto-hayate-
lp/mp/hp-hit` (−4, whiff's cgix4+cgix8 cells skipped outright, dwell
unchanged — R-deficit direction; both signs of this move close to zero
residual as a SINGLE branch-content differential, not the two-term
framing an earlier lane floated); `urien-vkd-lk-hit` (+25, F2
R-corollary — a 25-tick parallel recovery branch, cgix 68-88, all
jatix=0, entered only via the hitstop exit, byte-identical to the BLOCK
leg and absent from WHIFF). One further member converts PARTIAL:
`urien-vkd-lk-block` closes the identical R clause under the same
25-tick branch proof, but its stacked adv residual (−17 vs arcade −16)
is a pre-existing 1-tick residual §12.1 row 9 already routes (bundle-
context sensitivity) — not touched by this proof, so the entry stays
XFAIL on that clause alone. The F2 member set §13.13 recognizes now
includes these 8 (7 FULL + 1 PARTIAL) in addition to the SWEEP-2 set
above; see `docs/frame-data-synthesis.md` §12.2.2's SWEEP-3 UPDATE
paragraphs (R-deficit list and R-SURPLUS sub-list) for the full
per-cell citations, and `corpus-makoto.yaml`/`corpus-urien.yaml`'s
per-entry comments for the shipped conversion text.

**SWEEP-3 scope-out record (2026-07-11, classification sweep #3
fable-grade audit).** Four items were considered and explicitly NOT run
as lanes this sweep, recorded here rather than silently dropped: (1)
**the item-18 re-attack premise is DEAD** — a pre-registered contingency,
fulfilled by ENGINE-7's 1,039-window census, which FALSIFIED guard-rearm
retime as a general rule (arcade end = rearm+0 on only 4 of 7 rearm-gap
moves; two identical-signature pairs land on opposite sides with no
observable engine discriminator; `docs/plan-frame-data-completion.md`'s
ENGINE-7 row, "lever K REJECTED, not deferred"; this document's §12.2.4
cut-committed-whiff-overshoot row; §13.5.1b's second dated correction).
(2) **Yun/Yang whiff-only reset asymmetry** was NOT run as a lane — all
six member entries are OUT-A item-18(c) passive with E7-permanent
citations, and understanding the asymmetry could not change any
disposition (no granted family covers it; a new mechanism on these
settled entries would be merely decorative (b)); recorded as a curiosity
(`e7-census-report.md:179-180`). (3) **E8 grab-shaped census extras need
nothing**: `alex-powerbomb-lp-hit`/`-unblockable-probe` are already PASS
(`golden/alex.tsv:46-48`), `corpus-alex.yaml`'s own xfail census is 3,
all UOH (`:519/539/557`) — the ENGINE-8 census's own tooling limitation
is self-flagged with no census-contradiction (`e8-census-report.md:43-
51`); `ibuki-kubiori-lp-hit` is the pre-existing OUT-B oracle-
incomparability member, untouched. (4) **ENGINE-5's self-loop display-
convention option (11 played vs 12 declared) remains queued** on the
user-review list, unchanged from sweep #2 (§12.2.4's "Same-cgix
non-sentinel self-loop" row, `:899` in that row's own numbering). None
of these four is a lane finding; none changes any disposition above.

**SCOPE EXTENSION (CONV-2, 2026-07-10, second user decision).**

**Statement.** The user decision (referred to as CONV-2 in every dated
note below) extends the contact-path display convention to every corpus
xfail entry whose divergence mechanism is (i) **registered** - a §12.2
registry row (§12.2.1/§12.2.2/§12.2.3/§12.2.4) or a named family (item-4
Hugo-Roundhouse, §13.13 exclusion-item-7 partial-restore bucket), (ii)
**proven** - trace / credit-ledger / lever-quantified evidence closed for
that entry's own clause, not a hedged "candidate"/"HYPOTHESIS ONLY"/"not
per-tick closed" sighting, and (iii) **engine truth** - the measured
value is the chain/credit/advantage the engine actually realized on the
observed path, not a suspected mis-measurement (item 18(b)/(c)
meter-suspect readings never qualify). Qualifying entries convert to a
normative plain `PASS` asserting the measured engine-truth value, with
the arcade (whiff-canonical / travel-0-canonical) value preserved
verbatim in the comment labeled `arcade`. Per-clause conversion,
sibling-reference resolution, and the Entry notation above are reused
verbatim - CONV-2 extends WHICH mechanisms qualify, not HOW a qualifying
entry is written.

**Proof-mode restatement** (applies to every family below, unchanged from
the Membership rule above): trace / credit-ledger / lever-quantified
evidence, unhedged, per-clause. A signature-matched but
"candidate"/"HYPOTHESIS ONLY"/"not per-tick closed"/"UNCLASSIFIED"
sighting does not convert regardless of family membership.

**Families F4-F11** (designators continue the F1-F3 series above):

- **F4 - §12.2.1 UOH landing-clocked A (contact legs).** Ledger-proven
  extra sentinel ticks (necro/elena/twelve sentinel shape) or loop-back
  re-entry credit (alex looping-declared shape) on the delayed-landing
  BLOCK path, plus alex's own banked-declared HIT leg. §12.2.1's own
  "needs a user decision" disposition (`:694-697`) is hereby decided:
  contact-leg A clauses assert measured engine truth. The R-triplet
  clause of every F4 member does NOT convert (residual item 18(b) cut,
  meter-suspect); `alex-uoh-whiff` does NOT convert (WHIFF, exclusion 1
  outranks mechanism proof, named explicitly below); `elena-uoh-hit`,
  `necro-uoh-hit`, `twelve-uoh-hit` do NOT convert (A already
  arcade-exact on HIT, no convertible clause). Members: `alex-uoh-block`
  (A=15), `alex-uoh-hit` (A=12), `elena-uoh-block` (A=12),
  `necro-uoh-block` (A=12), `twelve-uoh-block` (A=12) - all PARTIAL (R
  clause stays xfail).
- **F5 - item-4 Hugo-Roundhouse landing-clocked R family.** Trace-proven
  same-tick contact advance on a move whose end is pinned by the
  outcome-independent landing check (`jumping_union_process`,
  `pls01.c:767`); the declared-active ticks the contact advance frees up
  tally as RECOVERY on the played chain, not a shortened move. Members:
  `h-roundhouse-block`/`-hit` (R=30, item-17 Hugo Roundhouse addendum,
  `:3790-3837`) and `ibuki-twdsroundhouse-block` (R=22, the sweep-2
  item-4 join, CORPUS-AUTHORING.md bucket 4). All three FULL.
- **F6 - §12.2.1 R-triplet whiff-exact variant.** `chunli-uoh-block`/
  `-hit` only: whiff R=5 is arcade-exact (unlike the F4 members' whiff
  legs, which stay item 18(b)); the contact-side cleanup cell provably
  plays one fewer tick than the whiff chain (`s12-audit-rulings.md` §B.1
  per-tick GT citations, relocated into the entries). Explicitly distinct
  from the 18(b)-truncated R-triplet of the F4 members, which stays
  excluded. Both members FULL (R=4).
- **F7 - §12.2.4 contact-skip R-window reclassification, MEASURED
  members only.** `yun-crfierce-block`'s R clause (P2 per-tick closure,
  run-yun GT=776-813): contact skips the remaining active cells, so
  whiff's own inter-active gap tick survives into the R window instead of
  branch content. FULL (R=16; this entry's A clause already converted
  under F1 in an earlier cycle, so the full entry is now a plain PASS).
  CANDIDATES (`elena-lynxtail-lk-block`'s R clause, "not per-tick closed")
  never convert under this family.
- **F8 - §12.2.4 contact-mutated declared cell duration.**
  `ken-tatsu-lk-block`/`-hit` (per-tick decomposition, s2 ken trace,
  raw_len=38 reproduced on all 3 legs): cell 28's declared cgctr mutates
  3->1 on contact (same jatix=56), raw T conserved at 38, and the +1 R
  materializes as cell 48 counting 2 ticks vs whiff's 1. Both FULL
  (R=15); adv stays unasserted on both (F-2 constraint, matching the
  section's existing omission).
- **F9 - §12.2.4 no-cut re-entry re-crediting A.** `remy-uoh-block`/
  `-hit` contact legs (own ledger, s4): whiff/hit visit cgix
  16->24->28->32->24 (one genuine re-entry, ea=11); BLOCK adds one
  further loop (->28, ea=13). Both PARTIAL (A converts to 13/11; the
  item-18(c) R clause stays xfail). The whiff leg (`remy-uoh-whiff`)
  stays excluded (WHIFF).
- **F10 - projectile travel-time displayed adv.** `remy-lov-lp-block`
  (adv=23), `remy-lov-lk-block` (adv=17), `remy-lov-lk-crouch-probe`
  (adv=15) - all FULL. NORMATIVE STATEMENT (verbatim requirement): *the
  displayed adv on a projectile contact leg embeds the projectile's real
  travel time on the observed path - the realized advantage at the
  corpus-pinned distance; the arcade oracle's `Block_advantage` is
  travel-0 canonical (adjacent contact) and is preserved in the comment
  labeled `arcade`. The asserted value is a per-entry measured constant
  valid only at the entry's pinned distance; changing the entry's `dist`
  invalidates it and requires re-measurement.* These are this repo's
  first in-corpus `adv` int literals (`grep -nE 'adv: *[+-]?[0-9]'
  tools/frame-data/corpus-*.yaml` returned zero hits before this cycle) -
  `compile_corpus.py:553-560` accepts the literal via its generic
  `expect.*` branch, no adv-specific syntax exists or is needed.
- **F11 - partial-restore compound displayed A** (§13.13 exclusion item
  7's bucket). NORMATIVE STATEMENT (verbatim requirement): *displayed A
  embeds the quantified partial lever-F restore stacked on unrecoverable
  shape-(a1) loss; the comment must carry the full decomposition - arcade
  A, measured A, lever-F ON/OFF values, restore delta, unrecoverable
  remainder.* Members: `makoto-crstrong-block` (A=4, ON=4/OFF=3),
  `makoto-crshort-block` (A=4, ON=4/OFF=3), `twelve-axe-jab-block`/
  `-hit`/`-crouch-probe` (A=8, ON=8/OFF=7) - all FULL;
  `yang-uoh-block`/`-hit` (A=8, ON=8/OFF=6) - PARTIAL, the item-18(c) R
  clause stays xfail; bucket-level (a1) attribution (this item's own
  registered text plus the §12.2.1 NOT-members line, `:692-693`) stands
  in for a per-entry ledger for yang, same as it already does for
  makoto/twelve.
- **F12 - §12.2.4 contact-outcome hold/skip differential (GRANTED
  2026-07-11 — user grant 2026-07-11, following the orchestrator's
  recommendation review, G1).** Per-tick closure from the preserved s12
  trace (F136-163/F185-208): +2 REAL ticks at the hitstop-exit cell cgix28
  (block holds cgctr=2 for 3 rows post-thaw vs hit's 1 row - inside the
  jatix=101 active window, masked in displayed A by the §13.11
  anchor_a=11 convention), plus +2 in the R window (block plays cgix40's
  cgctr=1 tick + the cgix44 sentinel; hit skips both); cleanup identical
  3v3; T differential +4 and R differential +2 close exactly, zero
  residual against whiff-anchored arithmetic (S+active_pf+R=T). Distinct
  from F2 (divergence partly inside the real active window here, not pure
  branch/skip) and from F6 (enumerated to chunli only, requires
  whiff-exact R; this move's whiff R=3 vs arcade 5). Members:
  `ibuki-uoh-block` (R=6), `ibuki-uoh-hit` (R=4) - both FULL, arcade 5
  preserved in each comment. `ibuki-uoh-whiff` is explicitly NOT a member
  (WHIFF, exclusion 1; its own item-18(b) residual is unrelated).
- **F13 - §12.2.4 same-tick interior-transition credit banking (GRANTED
  2026-07-11 — user grant 2026-07-11, following the orchestrator's
  recommendation review, G2).** `char_move()`'s interior same-tick
  `cg_ix` transitions bank each entered cell's declared credit while the
  per-frame snapshot observes only the tick-final cell; two manifestation
  topologies of the one mechanism - loop-back re-entry (remy) and
  skip-path pass-through credit of a skipped window's first, shorter cell
  (elena). The F1-rider test is REFUTED for both (banking is not clean
  skip arithmetic). Members: `elena-lynxtail-lk-hit` (A=4, its sole
  blocking clause) - FULL; `elena-lynxtail-lk-block` (A=2) - PARTIAL, its
  R clause stays xfail ((e)-PERMANENT, F7 candidacy refuted, unaffected
  by this grant - see the F7 entry above). **NOT members despite sharing
  the mechanism:** `remy-cbk-lk-block`/`-hit` - the grant text ("the
  same-tick credit-banking class ... becomes a granted family") grants
  the banking mechanism itself but the grant list's own stated dependency
  additionally requires the user to accept `remy-cbk-lk-whiff`'s own
  separate, still-UNCLASSIFIED ±1 boundary-shift finding (A11/R9 vs
  arcade 10/10, boundary tick F=1207 proven) as displayed engine truth
  before these two legs convert (A:14 = 10 + 1 boundary + 3 phantom;
  R:9) - the 2026-07-11 grant does not state that acceptance, so the
  dependency is unmet as written and both entries stay fully XFAIL,
  unconverted (no partial/narrow assertion exists either - A and R on
  these two legs are numerically entangled with the un-granted boundary
  piece, per corpus-remy.yaml's own xfail strings). `remy-cbk-lk-whiff`
  stays excluded (§13.13 exclusion 1, WHIFF) regardless.

Per-entry evidence (GT numbers, ledger traces, lever ON/OFF pairs) lives
in each corpus's own comment header, per the Entry notation above - this
block states which families exist and their membership, not the per-move
trace.

**Exclusion list.** The following are explicitly OUT of scope, and
every excluded entry keeps its arcade value in `expect` and its
`xfail` string (narrowed/re-worded only where a sibling clause did
convert), per `feedback-no-shipping-wrong-data.md` and the plan house
rule (`docs/plan-frame-data-completion.md:217-224`):

1. **Whiff legs — anything with `outcome: WHIFF`.** Whiff must match
   tables; whiff parity is the calibration surface
   (`CORPUS-AUTHORING.md:198-244`, Phase 4). Zero WHIFF-outcome entries
   are members of any of the three families.
2. **Item 18(b) residuals** (grounded fresh-edge early-cut whiff-R
   truncation) and **18(c)** (no-cut R overshoot) —
   `:3733-3809`. No proven contact-path mechanism.
   Includes `q-uoh-whiff-r`, `oro`/`akuma-uoh-whiff`, `dudley-uoh-block`,
   Urien's UOH R clause, the Yun/Remy/Yang UOH R clauses,
   `ibuki-kazekiri-*`.
   **DATED REFRAME (2026-07-11, CAPTURE-1):** the "METER itself may be
   wrong here (suspected measurement imprecision)" framing this bullet
   carried through 2026-07-10 is RETIRED — arcade ground-truth capture
   (`docs/arcade-frame-data/CAPTURE.md`) directly measured four members
   of this exact bucket (`urien-uoh` A+R, `yang-senkyuutai-lk` R,
   `ibuki-kazekiri-lk` R, `q-uoh-whiff-r` R) and found **arcade == oracle
   on every single sampled row**. The tables are not the suspect; the
   divergence is confirmed engine-side. What remains genuinely open is
   *which layer* of this engine produces it — this decompiled port's own
   arcade-vs-port behavioral difference, vs. an overlay/meter accounting
   gap in this repo's own instrumentation — not whether a divergence
   exists. See "PORT-DIVERGENCE-1" below for the load-bearing finding
   (the Urien UOH arithmetic: none of this engine's three reachable
   quantities, 9/11/12, equals arcade's clean 10+5) and
   `docs/arcade-frame-data/CAPTURE.md` for the full per-row capture
   evidence. This reframe does not convert any entry — every member
   listed in this bullet stays XFAIL with its arcade value in `expect`,
   now re-cited to the capture evidence.

   **PORT-DIVERGENCE-1 (2026-07-11, CAPTURE-1 finding).** Urien
   Universal Overhead is the decisive member: arcade plays 10 active +
   5 recovery frames (from first box to the busy 771->768 transition),
   captured twice, byte-identical. This engine's three independently
   reachable quantities for the same window are 9 (measured, today's
   displayed value — the self-loop credit gap ENGINE-5 already
   ledger-proved), 11 (played — the engine's own per-tick classifier,
   `active_pf`), and 12 (declared — crediting a fresh `cgctr` at
   re-dispatch entry, the §13.11-literal reading). Arcade's 10 sits
   strictly between 9 and 11, and strictly below 12 — it is not equal to
   ANY of them. This forecloses ENGINE-5's own closure question (display
   9, 11, or 12?) from a different direction: no display choice this
   engine can make reaches arcade, because arcade's true value is not a
   quantity this engine's own accounting ever produces on this move. The
   whiff-R corollary is the same shape at smaller scale: arcade R=5
   (Urien UOH, Q UOH, byte-identical whiff/oracle recipe) vs this
   engine's measured R=3 on both characters — a uniform 2-frame
   shortfall, same direction as the A gap, on a completely different
   part of the same move's timeline. The layer at which this divergence
   actually originates — this repo's decompiled-port engine differing
   from the original arcade PCB's own code, vs. a gap in this repo's own
   overlay/meter observation of an otherwise-correct engine, **vs. a
   ROM-revision mismatch** (the capture ran only against `sfiii3`, Euro
   990608; other arcade revisions in the same set — `sfiii3r1`/990512,
   `sfiii3nr1` — were not captured and a revision-level behavioral
   difference has not been ruled out, see
   `docs/arcade-frame-data/CAPTURE.md`'s Caveats section) — is
   explicitly **not determined** by this capture and is not chased
   further here; see `docs/arcade-frame-data/CAPTURE.md`'s "Reading the
   Urien UOH result" section for the full writeup and the flagged (not
   pursued) future bounded probe.

   **RETRACTED (2026-07-11, LAYER-1).** The load-bearing claim above --
   "no display choice this engine can make reaches arcade, because
   arcade's true value is not a quantity this engine's own accounting
   ever produces on this move" -- is refuted by direct measurement. A
   convention-twin engine-raw probe (env-gated scratch instrument, never
   committed, validated 6/6 + 1 repeat control, both windows md5-proven
   applied/reverted) measures this engine's RAW frames on Urien UOH
   whiff as **S15/A10/R5 -- identical to arcade on both revisions**
   (990608, this doc's original capture; and the newly-captured
   REFERENCE 990512, `docs/arcade-frame-data/CAPTURE.md`'s new Session
   3). The engine plays 10 raw box-active frames (the rig's
   any-of-four-s16 `att_box` predicate) and re-arms guard exactly where
   arcade drops busy; "9/11/12 are the only reachable quantities" was
   true only of the OVERLAY's credit-based accounting -- the plain
   raw-box frame count is a fifth, engine-native quantity, and it
   equals arcade's 10. The +0x28/+0x10/+0x1C struct-layout divergences
   (plan §1.1) are LAYOUT differences between the arcade binary and the
   decomp's struct literals -- never behavioral, and pre-registered as
   such. PORT-DIVERGENCE-1 is retracted as a port-divergence claim; the
   underlying measurements it recorded (arcade==oracle everywhere
   sampled) all stand and are re-attributed to layer (a) OVERLAY/METER.
   The flagged "11th box frame hit-check-eligible" probe above is now
   obsolete -- there is no 11th raw box frame. See new §13.16 for the
   full triangulation (12 clauses, 10 moves, all (a)) and
   `docs/arcade-frame-data/CAPTURE.md`'s dated update to "Reading the
   Urien UOH result".
3. **§12.2.1 UOH landing-clocked active tail**
   (`alex`/`elena`/`necro`/`twelve-uoh-*`) — mechanism established, but
   not one of the three families the user's decision named; §12.2.1's
   own disposition explicitly defers this to "needs a user decision"
   (`:682-685`). Flagged as an adjacent-proven-family follow-up
   candidate.
   **DATED NOTE (2026-07-10, CONV-2):** the user decision has now been
   taken. Contact-leg A clauses convert to measured engine truth under
   new family F4 (SCOPE EXTENSION block above): `alex-uoh-block`
   (A=15), `alex-uoh-hit` (A=12), `elena-uoh-block` (A=12),
   `necro-uoh-block` (A=12), `twelve-uoh-block` (A=12) - all PARTIAL,
   R-triplet clause stays excluded (18(b)-cut, meter-suspect).
   `alex-uoh-whiff` stays excluded (WHIFF, exclusion 1 outranks proof -
   the 9th member of this nominal family, the reason it converts only
   5). `elena-uoh-hit`, `necro-uoh-hit`, `twelve-uoh-hit` stay excluded
   untouched (A already arcade-exact on HIT, no convertible clause).
   **CORRECTED (2026-07-10, CONV-2 fix-cycle):** this item's own internal
   citation above, `:682-685`, lands on the R-triplet paragraph, not the
   "needs a user decision" disposition - the quoted phrase itself is at
   `:694-697`. The original citation is preserved verbatim above; this is
   an append-only correction, not a rewrite.
4. **Hugo-Roundhouse item-17 R-side family** (`h-roundhouse-block`/
   `-hit`, decision-tree bucket 4, `CORPUS-AUTHORING.md:368-374`) —
   trace-proven, but R-side on a landing-clocked move, not the
   shape-(a) A-undercount this section covers. Flagged the same as
   §12.2.1.
   **DATED NOTE (2026-07-10, CONV-2):** the user decision has now been
   taken. `h-roundhouse-block`/`-hit` and `ibuki-twdsroundhouse-block`
   (the sweep-2 item-4 join) convert to measured engine-truth
   R=30/30/22 under new family F5 (SCOPE EXTENSION block above) - all
   three FULL, xfail removed entirely.
   **CORRECTED (2026-07-10, CONV-2 fix-cycle):** this item's own internal
   citation above, `CORPUS-AUTHORING.md:368-374`, lands on bucket 2
   (HIT-R divergence classes), not bucket 4 (R-side contact surplus /
   Hugo-Roundhouse), which is actually at `CORPUS-AUTHORING.md:403-409`.
   The original citation is preserved verbatim above; this is an
   append-only correction, not a rewrite.
5. **§12.2.4 registered one-offs** (Hayate compound, S-divergence,
   sum-preserving A/R shift, cut-committed whiff overshoot, block-adv
   anomalies, two-way R=0, stance-conditional R) — `:747-761`. No
   mechanism claimed for any of these.
   **DATED AMENDMENT (2026-07-10, sweep #2):** "No mechanism claimed for
   any of these" no longer holds across the board — see §12.2.4's own
   dated row updates: the Hayate A-clause mechanism is now ESTABLISHED
   (F1, own-ledger; the six entries convert (a)-PARTIAL, their R clauses
   stay excluded here); the stance-conditional R row gained the
   pinned-meter identity (+ its active_pf honesty caveat); the
   sum-preserving row's whiff leg is boundary-tick PROVEN (+ the
   family-of-two framing correction); cut-committed → ENGINE-7
   candidate; block-adv split → ENGINE-9 candidate + established
   travel-time mechanism (remy-lov); two-way R=0 → ENGINE-8 candidate.
   Everything still-xfail in this item remains excluded from conversion.
   **DATED NOTE (2026-07-10, CONV-2):** the user decision has now been
   taken for the block-adv anomaly row's ESTABLISHED half. `remy-lov-lp-
   block` (adv=23), `remy-lov-lk-block` (adv=17), `remy-lov-lk-crouch-
   probe` (adv=15) convert under new family F10 (SCOPE EXTENSION block
   above, NORMATIVE STATEMENT on projectile travel-time adv) - all three
   FULL. `remy-crroundhouse-block` (ENGINE-9 candidate) remains excluded
   - its value is defective pending the fix cycle, not a CONV-2
   candidate. Everything else named in this item (Hayate R sign-flip,
   sum-preserving A/R shift, cut-committed whiff overshoot, two-way R=0,
   the stance-conditional row, `necro-flyingviper` judgment) remains
   excluded exactly as the prior dated amendment recorded.
   **DATED AMENDMENT (2026-07-11, sweep #3):** the Hayate R sign-flip is
   no longer in the "remains excluded" set above — F2 now covers both
   signs (§12.2.2's SWEEP-3 UPDATE, §13.13's F2 member-set amendment);
   all six `makoto-hayate-*` entries convert FULL, xfail removed
   entirely (see §12.2.4's Hayate compound row). Everything else named
   in this item (sum-preserving A/R shift, cut-committed whiff
   overshoot, two-way R=0, the stance-conditional row, `necro-
   flyingviper` judgment) remains excluded exactly as the prior dated
   amendments recorded, with two enrichments and no conversions: the
   sum-preserving row's contact legs (`remy-cbk-lk-block/-hit`,
   `yun-zesshou-hp-*`) are now ledger-decomposed (routed to the NEW
   "Same-tick interior-transition credit banking" row / the ENGINE-10
   candidate cite, respectively) and the stance-conditional row's F5
   membership is tested and REFUSED (`necro-flyingviper-lp-block/-hit`
   enriched, not converted) — see §12.2.4's own row updates for the full
   citations.
   **CORRECTED (2026-07-10, CONV-2 fix-cycle):** this item's own internal
   citation above, `:747-761`, lands on §12.2.2's SWEEP-2 update
   paragraph, not §12.2.4's registered one-off table, which is actually
   at `:850-873`. The original citation is preserved verbatim above; this
   is an append-only correction, not a rewrite.
6. **§12.2.2 / §12.2.3 CANDIDATES** — every string carrying a
   "candidate"/"stays UNCLASSIFIED"/"NOT individually re-traced" hedge:
   `sean-ryuubi-*` A clauses, `dudley-jetup-*` base-overcount A
   clauses, `twelve-crfierce`/`-crroundhouse`, `remy-rrf-*` A+1,
   `elena-crroundhouse-*`, `ibuki-raida-lp-block`, `ibuki-dtwdsforward-block`,
   the R-surplus corollary candidates (`twelve-crstrong-block`,
   `dudley-crforward` R clause, `yun-crfierce-block` R
   clause, `alex-fierce`/`-backfierce`). (`dudley-crroundhouse`'s R clause
   is a DIFFERENT shape — an R-side deficit that fails the §12.2.2
   membership bar, not an R-surplus — see the backlog paragraph below.)
   **DATED REMOVAL (2026-07-10, sweep #2):** every name in this item
   except `yun-crfierce-block`'s R clause has been PROMOTED to
   established membership (F2/F3, per-entry citations in §12.2.2's and
   §12.2.3's SWEEP-2 updates) and converted: `sean-ryuubi-*` A (×6, one
   partial), `dudley-jetup-*` A (×5), `twelve-crfierce-block/-hit`,
   `twelve-crroundhouse-hit`, `remy-rrf-lk-block/-hit`,
   `elena-crroundhouse-block/-hit`, `ibuki-raida-lp-block`,
   `ibuki-dtwdsforward-block`, `twelve-crstrong-block`,
   `dudley-crforward-block/-hit` R, `alex-fierce-block/-hit` R, plus
   `dudley-crroundhouse-block/-hit` R and `yun-farfierce`/
   `yun-roundhouse` R from the backlog paragraph below.
   `yun-crfierce-block`'s R clause stays excluded — measured NOT branch
   content (new §12.2.4 contact-skip R-window reclassification class).
   (`alex-backfierce`: no entry by that label exists in
   `corpus-alex.yaml` or `golden/alex.tsv` (grep-verified 2026-07-10) —
   the pre-sweep list's "alex-fierce/backfierce" aside resolves to the
   two `alex-fierce` legs only.)
   **DATED NOTE (2026-07-10, CONV-2), SUPERSEDING CORRECTION:** the line
   immediately above ("`yun-crfierce-block`'s R clause stays excluded -
   measured NOT branch content (new §12.2.4 contact-skip R-window
   reclassification class)") is now FALSE. The user decision has been
   taken for MEASURED members of that class (new family F7, SCOPE
   EXTENSION block above): `yun-crfierce-block`'s R clause converts to
   R=16 - the entry's A clause already converted under F1 in an earlier
   cycle, so the full entry is now a plain PASS (xfail removed entirely).
   `elena-lynxtail-lk-block`'s R clause remains excluded - it is a
   CANDIDATE of the same §12.2.4 class ("not per-tick closed"), and
   candidates never convert under F7 or any other family. The historical
   line above is preserved verbatim per repo practice (dated append, not
   silent rewrite).
7. **Partial-restore compounds** (the Makoto BENIGN NOVEL COMPOUND
   bucket: `makoto-crstrong-block`, `makoto-crshort-block`,
   `twelve-axe-jab-*`, `yang-uoh-*` A clause) — the measured A mixes
   unrecoverable shape-(a1) loss with a partial lever-F restore; a
   compound, not a clean member of any one family.
   **DATED NOTE (2026-07-10, CONV-2):** the user decision has now been
   taken; this bucket is now a named family, F11 (SCOPE EXTENSION block
   above). `makoto-crstrong-block` (A=4), `makoto-crshort-block` (A=4),
   `twelve-axe-jab-block`/`-hit`/`-crouch-probe` (A=8) convert FULL;
   `yang-uoh-block`/`-hit` (A=8) convert PARTIAL (the item-18(c) R clause
   stays excluded). Every member's comment carries the full lever
   decomposition (arcade A / measured A / lever-F ON+OFF / restore delta
   / unrecoverable remainder) per F11's normative statement.
8. **UNCLASSIFIED anything**, oracle-field incomparability
   (`oro-oniyama-*`, `ibuki-kubiori`), measurement-semantics gaps
   (`akuma-ashura-whiff`, `sean-roll-*`), the Dragon Smash oracle
   question, negative controls, and every other `xfail` not proven a
   member.
9. **Three-way / outcome-inconsistent variants the mechanism does not
   cleanly explain** — `elena-lynxtail-lk-block`/`-hit` (genuine
   three-way A-variance, COMPOUND UNCLASSIFIED), `remy-uoh-block` A
   clause (diverges alone), `yun-zesshou-hp-block`/`-hit` and
   `remy-cbk-lk-whiff` (sum-preserving A/R boundary shift, explicitly
   sign-incoherent with shape (a1)).
   **DATED NOTE (2026-07-10, sweep #2):** `elena-lynxtail-lk-block`'s
   R-clause is now routed OUT of this item's "does not cleanly explain"
   framing and INTO the new §12.2.4 "contact-skip R-window
   reclassification" class as a CANDIDATE (the BLOCK-only +8 == whiff's
   own 8-tick inter-window gap declared length, cgctr-identical tail);
   its A-clause stays here (graduated two-window skip, mechanism note
   only, banked-credit arithmetic not ledger-decomposed).
   `remy-uoh-block`'s A clause routes to §12.2.4's no-cut re-entry
   re-crediting row (ledger-proven). All named entries stay xfail.
   **DATED NOTE (2026-07-10, CONV-2):** the user decision has now been
   taken for `remy-uoh-block`'s A clause (routed above to §12.2.4's
   no-cut re-entry re-crediting row) - new family F9 (SCOPE EXTENSION
   block above): A converts to 13, PARTIAL (the item-18(c) R clause
   stays excluded, unchanged). `remy-uoh-hit`'s A clause converts
   identically (A=11, same F9 family, same ledger). `elena-lynxtail-lk-
   block`/`-hit` remain fully excluded - the A-clause "banked-credit
   arithmetic not ledger-decomposed" and the R-clause CANDIDATE status
   are both unchanged by CONV-2; neither meets the proven bar.

**Explicit non-actions.** No overlay or engine behavior changes at
all — this section is corpus-and-doc-only, every documented lever flag
set from §13.10/§13.11/§12.2 is untouched, and no lever was
re-derived. No blanket cross-family formula — every converted value is
a per-entry measured constant pinned from the golden TSVs, not a
computed rule. No new corpus schema or tooling — the notation above
was verified sufficient before any entry was touched
(`compile_corpus.py`, `check_frame_data.py`, `golden.py`, `run-suite.sh`
all remain byte-identical).

**Verification record (2026-07-10).** Baseline (this session,
verified `git rev-parse HEAD` = `ab640c06`, tree clean): 19 golden TSVs
under `tools/frame-data/golden/`, **836 PASS / 222 XFAIL** (+19 header
rows). Derivation (`c13-membership.tsv`, scratchpad-only, not
committed): of the 222 `xfail` entries across 19 corpora
(`corpus-smoke.yaml` excluded, not a character corpus), **90 FULL**
converts (`xfail` removed entirely), **20 PARTIAL** converts (one or
more clause's field converts; `xfail` narrowed to the residual
clause(s)), **112 EXCLUDED** (untouched, arcade values retained). Of
the 90 FULL: F1 (shape-(a) A-undercount) accounts for 68 pure entries
plus 3 more stacked with an equally-established F2b R-clause (both
clauses converting in the same entry); F2 (§12.2.2 contact-branch
recovery shortening) accounts for 10; F3 (§12.2.3, established for
`ken-forward-block`/`-hit` only) accounts for 2; §13.10-compliance
(F2b) accounts for 10 total (7 single-clause + the 3 F1-stacked
entries just counted — `dudley-mgb-hp-hit`, `dudley-mgb-mp-hit`,
`twelve-forward-hit`). Of the 20 PARTIAL: 13 are F1 A-clause-only
converts (their stacked R/other clause stays `xfail`, EXCLUDED per
items 2/5/6/9 above), 5 are F1 A-clause converts whose stacked
R-clause fails the F2 membership bar (registry-absent, precedent-cite
only — `dudley-crroundhouse-block`/`-hit`, `yun-farfierce-block`,
`yun-roundhouse-block`/`-hit` — re-worded as unregistered §12.2.2
candidates, backlog), and 2 are F2b R-clause-only converts whose
stacked A-clause is an EXCLUDED §12.2.3 candidate. New
totals: XFAIL 222 − 90 = **132**; PASS 836 + 90 = **926**; total rows
unchanged at 1,058. Full per-corpus before/after table, the two
`run-suite.sh --check-golden` transcripts (pre-update drift check and
final zero-drift gate), and the arithmetic reconciliation live in the
`CONV-C13` tracker row, `docs/plan-frame-data-completion.md`
(commit message carries the same numbers). Backlog (flagged, not
converted, candidates for a possible follow-up user decision):
§12.2.1 UOH landing-clocked family (item 3 above), Hugo-Roundhouse
R-side pair (item 4), and the re-worded unregistered §12.2.2
candidates surfaced by this derivation (`dudley-crroundhouse` R
clause, `yun-farfierce`/`-roundhouse` R clauses,
`twelve-crfierce-hit`'s stacked R-clause, ambiguous on its own hedge
and excluded rather than force-classified).
**DATED CROSS-REFERENCE (2026-07-10, sweep #2):** the unregistered
§12.2.2 candidates in this backlog are now DISPOSITIONED by
classification sweep #2 (SWEEP-2 tracker row,
`docs/plan-frame-data-completion.md`): `dudley-crroundhouse` R (→15,
promoted+converted), `yun-farfierce`/`-roundhouse` R (→21, promoted+
converted), `twelve-crfierce-hit` stacked R (→13, promoted+converted).
The §12.2.1 UOH family and Hugo-Roundhouse R-side pair remain
un-dispositioned (still awaiting a user convention decision). This
paragraph's original text is preserved above as the historical record.

**UPDATE (2026-07-10, CONV-2):** the line immediately above is now FALSE
- both are now dispositioned. The §12.2.1 UOH family converts PARTIAL
under new family F4 (`alex-uoh-block`/`-hit`, `elena-uoh-block`,
`necro-uoh-block`, `twelve-uoh-block`; A-clause only, R-triplet stays
xfail); the Hugo-Roundhouse R-side pair converts FULL under new family
F5 (`h-roundhouse-block`/`-hit`, R=30). See §13.13's CONV-2 verification
record below for the full derivation and gate results.

**Verification record (2026-07-10, CONV-2).** Baseline (this session,
tree clean at the C13/SWEEP-2 gate): 19 golden TSVs under
`tools/frame-data/golden/`, **961 PASS / 97 XFAIL** (+19 header rows).
Derivation (mechanically reconstructed from `golden/*.tsv`'s 97 XFAIL
rows + the 12 corpus YAML `xfail` strings + `sweep2-final-rulings.md`/
`s12-audit-rulings.md`, cross-checked line-for-line against the plan's
own authoring session; no separate membership TSV committed): **16
FULL** converts (`xfail` removed entirely: F5 ×3 —
`h-roundhouse-block`/`-hit`, `ibuki-twdsroundhouse-block`; F6 ×2 —
`chunli-uoh-block`/`-hit`; F7 ×1 — `yun-crfierce-block`; F8 ×2 —
`ken-tatsu-lk-block`/`-hit`; F10 ×3 — `remy-lov-lp-block`,
`remy-lov-lk-block`, `remy-lov-lk-crouch-probe`; F11 ×5 —
`makoto-crstrong-block`, `makoto-crshort-block`,
`twelve-axe-jab-block`/`-hit`/`-crouch-probe`), **9 PARTIAL** converts
(member clause converts, `xfail` narrowed to the residual, zero golden
drift: F4 ×5 — `alex-uoh-block`/`-hit`, `elena-uoh-block`,
`necro-uoh-block`, `twelve-uoh-block`; F9 ×2 — `remy-uoh-block`/
`-hit`; F11 ×2 — `yang-uoh-block`/`-hit`), **72 EXCLUDED** (untouched,
arcade values retained) — sums to 97. New totals: XFAIL 97 − 16 =
**81**; PASS 961 + 16 = **977**; total rows unchanged at 1,058
(per-corpus reconciliation verified against `golden/*.tsv`
post-update). Verification: Gate 0 (tooling byte-identity + the 5
lever constants all `=1`) asserted before any run; Pass 1 pre-update
`run-suite.sh --check-golden` (19/19 GREEN, transcript
`conv2-verify-pre.log`, session scratchpad) — drift set == exactly the
16 FULL labels, every line `verdict XFAIL->PASS`, zero drift on any
PARTIAL member or any other row; Pass 2 `--update-golden` scoped to
exactly the 8 FULL-bearing corpora (`chunli`, `hugo`, `ibuki`, `ken`,
`makoto`, `remy`, `twelve`, `yun`, 8/8 GREEN) — diff is verdict-only
`XFAIL->PASS` on exactly the 16 labels, measured value columns
byte-identical; Pass 3 final `run-suite.sh --check-golden` (transcript
`conv2-verify-post.log`, session scratchpad) → exit 0, zero drift,
19/19 GREEN. Full per-corpus before/after reconciliation and the
arithmetic live in the `CONV-2` tracker row,
`docs/plan-frame-data-completion.md` (commit message carries the same
numbers). Backlog (flagged, not converted, candidates for a possible
future user decision): all 11 ENGINE-5..9 candidate holdouts
(`urien-uoh-*` ×3, `yang-forward-*` ×2, `yang-senkyuutai-lk` ×3,
`oro-jinchu-lk-*` ×2, `remy-crroundhouse-block` ×1 — defective pending
their own fix cycles, untouched); `necro-flyingviper-lp-block`/
`-crouch-probe`/`-hit` and `elena-lynxtail-lk-block`/`-hit` (item 9,
per-tick closure unmet); `dudley-uoh-block`/`-hit` R clause (item 2, no
validated whiff-exact anchor); the still-excluded item-18(b)/(c)
meter-suspect R residual sitting under every F4/F9/F11 PARTIAL member
(9 entries: alex ×2, elena ×1, necro ×1, twelve ×1, remy ×2, yang ×2);
SWEEP-2's own un-dispositioned lanes (d) (3 entries, §12.1 rows 7/8)
and (e) (8 what-was-tried re-cites: ibuki-uoh, urien-vkd R + §12.1 row
9, remy-cbk contact legs, yun-zesshou); and the oracle-incomparability
/ judgment-call rows already excluded by C13/SWEEP-2 (Dragon Smash
Strong question, §12.2.4 one-off register). Zero `src/` changes, zero
tooling changes (`compile_corpus.py`, `check_frame_data.py`,
`golden.py`, `run-suite.sh` byte-identical throughout).

### 13.14 Multi-contact adv anchors the FINAL stun exit (normative; ENGINE-9, lever M, SHIPPED 2026-07-11)

**The defect (accounting, not inference).** The overlay's
`defender_idle` was a write-once latch on the defender's FIRST stun
exit (the `g_cur.defender_idle < 0` gate in the tick-side
defender-idle-return check): on a multi-contact move whose defender
exits stun BETWEEN contacts, `adv = defender_idle − attacker_idle`
used the stale first-exit anchor. Proven member:
`remy-crroundhouse-block` (two-contact cr.RH; census trace-basis:
contact 1 F=1628, first exit F=1652, contact 2 F=1658, final exit
F=1682, attacker idle F=1693) — displayed adv was 1652−1693 = **−41**
where arcade Block_advantage is **−11** = 1682−1693, the final-exit
figure, EXACTLY. This is a code-inspection fact about a
re-triggerable event feeding a write-once latch, not an inference
about which engine tick arcade tables encode (the E6/E7 death shape).

**The fix (lever M, `fd_adv_last_stun_exit`, overlay-only).** A
re-arm block placed immediately BEFORE the defender-idle-return
latch (same-tick order provably irrelevant: a re-contact tick has
`dn->r1 == 1`, the latch needs `dn->r1 == 0` — they can never both
fire on one tick) resets `g_cur.defender_idle` to −1 when ALL FIVE
conjuncts hold, so the FINAL stun exit re-latches and anchors adv:

1. `event ∈ {HIT, BLOCK}` — a first contact was latched; PARRY
   windows untouched (their adv never reads `defender_idle`).
2. `defender_idle >= 0` — an intermediate stun EXIT already latched.
   Makes continuous-stun multi-hits (HSB: defender re-stunned before
   ever exiting, `defender_idle` still −1) no-ops by construction,
   and proves re-arm only fires pre-`attacker_idle` (once
   `attacker_idle >= 0`, the tick `defender_idle` latches, finalize
   consumes the move later that same tick).
3. `cgix_reset_frame < 0` — the §13.5.1 machinery's own signal that
   the attacker's chart never reset: a contact arriving after a cgix
   reset under r1=4 belongs to a return-to-neutral / retriggered
   chart (`q-uoh-chain-retrigger`: tap-1's landing reset at F=5585
   precedes tap-2's contact at F=5604, so this gate refuses it), not
   a later cell of the original attack chart (remy cr.RH's cgix
   advances 12→16→…→48 monotonically, no reset). This is the M1
   variant; the census also measured M0 (no chart gate) and
   FALSIFIED it on exactly `q-uoh-chain-retrigger` (re-arm →
   nonsense adv_pred +37 on a currently-PASS window).
4. `dp->dm_stop == 0 && dn->dm_stop != 0` — a fresh contact edge,
   the same contact-only signal the event latch trusts (writer
   inventory, item 14).
5. `dn->r1 == 1` — the defender genuinely re-entered hit/block-stun.
   Excludes parried later contacts (r1 stays 0; re-arming there
   would deadlock finalize) and throw-grapple re-entries (r1 2/3).

**Monotonicity (structural).** The re-arm can only move
`defender_idle` LATER; `attacker_idle`'s setters are untouched — so
`adv_pred >= adv_today` on every window, always. Empirically
confirmed 1039/1039 by the census. Any candidate whose oracle needs
a DECREASE (e.g. `sean-ryuubi-hk-block`, −2 measured vs −3 arcade)
is excluded a priori — not a lever-M candidate under any topology;
its residual is the unrelated §13.5.1b R-truncation clause.

**Census evidence (`e9-census.tsv` + `e9-census-report.md`, session
scratchpad; 1,039 FINAL windows across all 19 corpora, RUNG 0).**

| Question | Answer |
| --- | --- |
| Windows with ≥2 disjoint stun episodes (`n_stun_episodes == 2`) | exactly 2: `remy-crroundhouse-block`, `q-uoh-chain-retrigger` |
| M1 re-arm population (predicate actually fires) | exactly 1: `remy-crroundhouse-block` (singleton; `remy-crroundhouse-hit` resolved NOOP — single contact, defender rides the down family with no intermediate exit) |
| M0 re-arm population | 2 (adds `q-uoh-chain-retrigger` → adv_pred +37, nonsense) — M0 falsified, M1 shipped |
| No-op-branch drift | 0 of 1,038 (`adv_pred_M1 == adv_today` everywhere but the member) |
| Sole member vs oracle | `adv_pred_M1 = −11` == remy.json Block_advantage exactly |
| `adv_pred >= adv_today` (monotonicity) | 1039/1039, both variants |
| Finalize deferral (`finalize_delta_M1`) | 0 on all 1,039 windows (member's final exit 1682 < atk idle 1693) — zero kd-flip risk, zero window-merge risk, lever I's sampling window unchanged |
| q-hsb ×6 (3 contacts each) | continuous stun (`n_stun_episodes=1`), NOOP by construction |
| twelve-ndl ×6 | `n_contacts=1` each — not multi-spike-with-gap at all; trivially NOOP |
| elena-lynxtail-lk-block | 2 contacts, continuous stun, NOOP; arcade-exact adv −17 unchanged |

**The accounting-defect exemption (a NAMED replacement of the E6
support bar, not a silent weakening).** The E6 bar (≥2 same-code-path
positives) was built for arcade-keying INFERENCES generalized from
n=1. Lever M corrects a PROVEN write-once code defect, and the E6
bar is unmeetable in this corpus (the census found zero additional
re-arm-branch windows; continuous-stun corroborators like the q-hsb
block legs validate only the covering rule's IDENTITY branch — no-op
when `defender_idle` is still −1 at re-contact — never the re-arm
branch, and are deliberately NOT counted as independent positives).
The bar was therefore explicitly REPLACED by three census-measured
conditions, ALL required to ship at rung 0, and all three PASSED:
(a) **population-exactness** — the re-arm population in-suite is
EXACTLY `{remy-crroundhouse-block}`; (b) **zero no-op-branch
drift** — every non-firing window byte-identical; (c)
**oracle-exactness of the sole member** — `adv_pred == −11` exactly,
not merely closer. A second re-arm-branch member with an arcade-exact
CURRENT adv would have broken (a) and forced rung 2/3.

**Mutation contract (lever M).** `fd_adv_last_stun_exit = 0` restores
today's pre-fix behavior bit-for-bit: full-suite golden drift is
EXACTLY `remy-crroundhouse-block` (verdict PASS→FAIL, adv −11→−41),
nothing else anywhere (measured 2026-07-11, 18 GREEN / 1 RED). The
`defender_idle` writer set is exactly {reset (fd_reset_move), the
re-arm, the idle-return latch}; the event latch, `attacker_idle`
setters, raw[]/meter, `engine_a` snapshot, kd predicates, and
projectile anchors (lever J) are all untouched.

**Residuals (documented, out of harness population).**
1. *Live-play red parry of a later contact*: conjunct 5 excludes it
   (parry keeps r1==0), so behavior is today's — but no corpus window
   exercises a mid-chain defender parry (harness dummies never act);
   untested by the harness, by construction of its population.
2. *Q SA3 NINGENBAKUDAN* (`plpat18.c:36-38,51`) writes its OWN
   `dm_stop = 1` under its own `routine_no[1]=1/routine_no[2]=91` — a
   live-play false-re-arm hazard IN PRINCIPLE if a defender were
   mid-move while Q resolves SA3 concurrently. Harness population
   unaffected (the dummy never acts, so this never appears as a
   defender-side edge in-corpus).
3. *Trade immunity (stronger than out-of-population)*: a trade
   structurally CANNOT trip conjunct 4 — both combatants' `dm_stop`
   is abs-merged into `hit_stop` and zeroed within the same hitcheck
   pass (`hitcheck.c:131-153`, item 14), so a trade never produces a
   fresh defender-side contact edge at all.

Future multi-contact corpus entries inherit final-exit adv semantics
by design: the covering rule is "arcade adv = defender's
actionability after the LAST contact's stun, minus attacker's" — the
same selection every continuous-stun multi-hit PASS row already
computes (the exit that follows the last contact), extended by lever
M to disjoint-episode topologies.

---

### 13.15 Oracle-structural field masking (MASK-1; normative; adopted 2026-07-11, user decision)

**Statement.** For a corpus entry whose divergence is an
`docs/arcade-frame-data/ERRATA.md` **STRUCTURAL** verdict — the
harness's own S/A/R decomposition cannot represent what the oracle
field encodes, independent of whether the published figure is correct
— the entry drops the `expect` assert for that field only (masking),
keeps asserting every other, comparable field, and reads a plain
`PASS` on those. This is a documented non-assert, not a value swap and
not a new assert: nothing is invented, a structurally-incomparable
field is simply removed from the comparison, with the dropped field's
oracle and measured values, and the ERRATA citation, preserved verbatim
in a comment directly above the entry (§13.15's "Entry notation"
paragraph below — §13.15 has no numbered subsections; this cites the
paragraph anchor, not a subsection number). Masking is
**subtractive only** — an already-PASSing field's own assert (e.g. an
oniyama-block leg's `adv`, already unasserted per its own
pre-MASK-1 comment) is never newly added by this convention.

**Industry framing.** This is the same principle §13.10/§13.11/§13.13
already apply to *measurement* divergences, extended here to
*representational* ones: a meter cannot display a number in a unit it
has no concept of (a negative recovery-frame count, a zero-active
move's non-existent active window, a two-phase move's precursor-phase
figure on a leg that measures the other phase) — omitting the field is
more honest than either asserting a wrong value or silently keeping a
literal that is documented, on the harness's own terms, as
uncomparable.

**Membership rule.** Masking is allowed **only** for a field whose
incomparability carries an ERRATA **STRUCTURAL** verdict — never for a
value dispute (`UNVERIFIABLE`, `ORACLE-LIKELY-WRONG`) or an
engine/observer-measurement class (e.g. item 18(b)'s whiff-R
truncation). Every field the entry keeps asserting must match golden
**measured** exactly — a comparable-field mismatch bars masking
entirely for that entry (the P-1 guard: masking must never be used to
paper over a genuine wrong value). This rule was applied and re-proven
against the tree at commit `256f73c2` before any edit in this cycle
(oracle JSON reads + golden TSV reads, both cited per entry below).

**Member table (8 entries, MASK-1, 2026-07-11).**

| # | Entry | ERRATA verdict | Masked (dropped) | Kept & PASSing |
| --- | --- | --- | --- | --- |
| 1 | `akuma-ashura-whiff` | §1 STRUCTURAL | R (oracle 9; measured saturated S=T=72/A=0/R=0, zero-active) | outcome=WHIFF + finals=1 (`expect: {}`) |
| 2 | `oro-oniyama-lp-whiff` | §2 STRUCTURAL | R (oracle −29; measured 28, S+A+R=44=T, cut=1) | S=6, A=10 (`from-qjson`, both exact) |
| 3 | `oro-oniyama-lp-block` | §2 STRUCTURAL | R (same); adv stays unasserted, unchanged (oracle −34 shares the incomparability; measured −21, recorded not asserted) | S=6, A=10 |
| 4 | `oro-oniyama-lp-hit` | §2 STRUCTURAL | R (same); adv/kd stay unasserted, unchanged (measured +75/kd=1, no oracle KD marker) | S=6, A=10 |
| 5 | `sean-roll-lp-whiff` | §5 STRUCTURAL (new, this cycle) | R (oracle 7; measured saturated S=T=28/A=0/R=0, zero-active; T=28=oracle Startup+Hit+Recovery 2+19+7 exactly) | outcome=WHIFF + finals=1 (`expect: {}`) |
| 6 | `sean-roll-mp-whiff` | §5 STRUCTURAL | R (measured S=T=38; T=38=2+29+7) | outcome=WHIFF + finals=1 |
| 7 | `sean-roll-hp-whiff` | §5 STRUCTURAL | R (measured S=T=52; T=52=2+43+7) | outcome=WHIFF + finals=1 |
| 8 | `ibuki-kubiori-lp-hit` | §3 STRUCTURAL | A (oracle first-triplet 11 = precursor strike phase; this leg measures the grab catch-and-throw phase, A=1); R (oracle 15 = strike phase; measured 39 = post-capture throw recovery, T=54=S+A+R, endrel=1); adv stays unasserted, unchanged | S=14 (explicit literal, oracle first-triplet 14 == measured, shared by both phases), kd=1 (explicit literal) |

Every kept field was re-verified against the oracle JSON and the
golden-measured TSV at the commit this cycle edited (§1.3/§1.4 of the
implement session's plan; zero mismatches found — the requirement
above is met for all 8).

**DATED PRIMARY-SOURCE UPDATE (2026-07-11, LAYER-1) -- oro-oniyama-lp-whiff/-block/-hit (rows 2-4).** Arcade 990512 busy-R = **29** (2 reps byte-identical; f1=277334, box 277340-277349, busy768=277379, T=45, pre-move charge frames correctly excluded -- `docs/arcade-frame-data/CAPTURE.md`'s new Session 3); a convention-twin engine-raw probe independently measures **29** too (§13.16). The oracle's magnitude (29; oracle notation -29) and EventHubs' positive 29 both match the hardware count; the engine plays the arcade timeline and this repo's meter drops one tick -- 28 is now a MEASURED-WRONG value, not merely a clean self-consistent reading. **MASK-1 masking STAYS**: oracle R remains structurally negative (-29) in oracle notation, still incomparable with the harness's >=0 R metric -- nothing about the mask's rationale changed by this finding. The "assert R:28 as an arcade-confirmed literal and drop the mask" option (`docs/arcade-frame-data/CAPTURE.md`'s prior follow-up framing) is DEAD: 28 is measured-wrong, and 29 is unassertable until the meter itself is fixed (post-fix measured 29 could be discussed then). `docs/arcade-frame-data/ERRATA.md` §2's sign-conflict caveat upgrades to primary-source-resolved (see ERRATA.md's own dated update); the masked-note provenance changes from "measured R=28 is a clean, self-consistent reading" to "harness measures 28; hardware+engine-raw play 29 (meter artifact, LAYER-1)". Golden-pinned masked drift (28->29) is EXPECTED whenever a future meter fix lands -- no drift is predicted or permitted THIS commit (corpus comments only, no value change).

**DATED NOTE (2026-07-11, RE-ANCHOR-1 SHIPPED, §13.17):** the meter fix
this paragraph anticipated has now landed, and the predicted golden drift
landed with it — `oro-oniyama-lp-whiff`'s measured R moved **28->29**
(`tools/frame-data/golden/oro.tsv`), matching both the arcade hardware
count and the engine-raw twin cited above exactly. **MASK-1 masking
STAYS unchanged**: the mask's rationale (oracle R structurally negative,
incomparable with the harness's >=0 metric) is untouched by this fix —
this row's kept fields (S=6, A=10) are unaffected, only the masked-away R
moved, and masked fields drift silently by design (§13.15's own Audit
semantics paragraph). This is the RE-ANCHOR-1 program's WHIFF-leg meter
fix (§13.17, levers N/O); it is not a MASK-1 amendment.

**Exclusions, named.**
- **`sean-dragonsmash-mp-whiff/-block/-hit`** (ERRATA §4,
  `UNVERIFIABLE`) — explicitly **NOT masked**. This is a live three-way
  source value dispute (`sean.json`/FAT-3S agree Strong≡Jab; EventHubs
  shows a distinct Strong row matching this engine's own measurement),
  not a structural incomparability — masking here would bury the
  question the pending arcade capture is meant to settle. Stays XFAIL
  unchanged.
  **DATED RIDER (2026-07-11, CAPTURE-1, append-only):** the arcade
  capture referenced as "pending" above has since landed
  (`docs/arcade-frame-data/CAPTURE.md`) and settled the dispute: arcade
  plays S=7/A=8/R=39 on the dp+MP whiff, matching this engine, not the
  Jab-duplicate row. These three entries are no longer XFAIL — they
  converted to explicit-literal PASS
  (`docs/plan-frame-data-completion.md`'s CAPTURE-1 tracker row;
  `docs/arcade-frame-data/ERRATA.md` §4, verdict upgraded UNVERIFIABLE →
  ORACLE-WRONG-CONFIRMED). This masking-exclusion discussion is
  preserved verbatim above as the historical record of why masking
  specifically was never applicable here (the dispute was a value
  question, not a structural incomparability, so it was never a MASK-1
  candidate) — "Stays XFAIL unchanged" no longer describes their
  current state.
- **`ibuki-kubiori-lp-whiff`/`-block`** — explicitly **NOT masked**.
  Their divergent field is R, and R **is** comparable there (S=14/A=11
  match the oracle's first-triplet Startup/Hit exactly, identifying the
  strike phase both legs measure): the R mismatch (measured 8 vs oracle
  15) is residual item 18(b) whiff-R truncation, an
  engine/observer-measurement class, i.e. a value divergence on a
  comparable field — the membership rule above bars masking it. Stay
  XFAIL unchanged.

**Entry notation.** Every masked entry carries a `FIELD-MASKED
(ERRATA)` comment block directly above it (grep anchor: the literal
string `FIELD-MASKED (ERRATA)` appears exactly once per masked entry,
8 total — `grep -c "FIELD-MASKED (ERRATA)" tools/frame-data/corpus-*.yaml`
reports akuma=1, oro=3, sean=3, ibuki=1). The comment carries, in
order: the dropped field(s)' oracle value(s) verbatim, the measured
value(s), the ERRATA section + STRUCTURAL verdict, and this section's
citation. Nothing from the pre-mask `xfail` string is silently
discarded — its factual content (measured values, trace facts, dated
findings, RESOLVED notes) relocates into this comment. The `xfail` key
itself is **removed** on every masked entry (never merely kept
alongside a narrowed `expect`) — see Audit semantics below for why.

**Audit semantics.** Dropping an `expect` key is a no-assert, not a
disabled check (`compile_corpus.py:534-537` / `check_frame_data.py:135-137`);
`outcome` is always asserted and `finals` defaults to 1
(`check_frame_data.py:128-129,133-134`), so `expect: {}` still asserts
outcome+finals. Crucially, the `xfail` key **must** be removed on a
masked entry: with `xfail` present, an otherwise-passing entry reads
`XPASS`, which is a **failing** verdict (`check_frame_data.py:157-159`,
`FAILING_VERDICTS`, `:197`) and would RED the suite. This retires the
old xfail-string tripwire ("would XPASS-alert if the overlay ever
gains zero-active R semantics") — its replacement is golden
measured-column pinning: `golden/*.tsv` stores the measured S/A/R/adv/kd
from the first FINAL regardless of what `expect` asserts
(`golden.py:21-23,46-47,59-65`), so if the masked field's own measured
value ever moves (e.g. a future zero-active-R convention, or an
engine change to the throw-phase measurement), `--check-golden` trips
on that column exactly as before — future masked-field drift is
golden drift. Golden drift predicted by this cycle is verdict-only
(`XFAIL->PASS`) on exactly these 8 rows, measured columns
byte-identical, since zero engine/tooling code changed.

**Arithmetic (2026-07-11, post-G1/G2 baseline).** Pre-MASK-1: 990 PASS
/ 69 XFAIL / 1,059 rows (tracker row `G12-GRANTS`). MASK-1 flips
exactly the 8 member rows, verdict-only: **990 + 8 = 998 PASS / 69 − 8
= 61 XFAIL**, rows unchanged at 1,059. Per-corpus: akuma 52/2 → 53/1;
oro 46/6 → 49/3; sean 61/8 → 64/5; ibuki 51/7 → 52/6 (post-G1/G2
baseline; G1's own +2 ibuki-uoh flips already folded in). All other 15
corpora byte-identical.


---

### 13.16 LAYER-1: divergent layer identified -- the overlay meter (dated 2026-07-11)

**Status: LAYER-1 (2026-07-11).** This section closes the "which layer"
question PORT-DIVERGENCE-1 (§13.13 exclusion item 2) and the item-18
register left open: arcade rig capture (990512 REFERENCE + 990608
historical) x a convention-twin engine-raw probe x the overlay, all
measured under `docs/arcade-frame-data/CAPTURE.md`'s counting rule,
triangulate the same answer on every tested row.

**Method.** Three independent instruments measured the same windows:
(1) arcade hardware -- `sfiii3nr1`/Japan 990512 NO CD (REFERENCE
revision, orchestrator ruling) plus the historical Euro 990608 session;
(2) a convention-twin -- an env-gated scratch probe (never committed)
that derives S/A/R straight from the engine's own raw signals (the
any-of-four-s16 `att_box` predicate for A, the `gflg` 3->0 edge under a
strictly-between counting rule for R), validated 6/6 plus one repeat
control, both instrumented windows md5-proven applied-then-reverted
with a clean rebuild and byte-identical post-revert control; (3) the
overlay/golden meter already in this repo.

**Verdict table (12 clauses, 10 moves).**

| Move / clause | Oracle | Overlay (golden) | Engine-raw (twin) | Arcade 990512 (REF) | Arcade 990608 | Verdict |
|---|---|---|---|---|---|---|
| urien-uoh whiff **A** | 10 | 9 meas / 11 played / 12 declared | **10** | 10 | 10 | **(a)** |
| urien-uoh whiff **R** | 5 | 3 | **5** | 5 | 5 | **(a)** |
| q-uoh whiff **R** (A display settled §13.11, not reopened) | 5 | 3 | **5** (box-A=10) | 5 (box-A=10) | 5 (box-A=10) | **(a)** |
| yang-senkyuutai-lk whiff **R** | 34 | 38 | **34** | 34 | 34 | **(a)** |
| ibuki-kazekiri-lk whiff **R** | 26 | 29 | **26** | 26 | 26 | **(a)** |
| remy-cbk-lk whiff **A/R boundary** | 10/10 | 11/9 | **10/10** | 10/10 | -- | **(a)** |
| oro-oniyama-lp whiff **R** | -29 (magnitude 29) | 28 (masked, golden-pinned) | **29** | 29 | -- | **(a)** |
| akuma-uoh whiff **R** | 7 | 6 | **7** | 7 | -- | **(a)** |
| oro-uoh whiff **R** | 5 | 4 | **5** | 5 | -- | **(a)** |
| yun-uoh whiff **R** | 6 | 10 | **6** | 6 | -- | **(a)** |
| remy-uoh whiff **A** | 10 | 11 | **10** | 10 | -- | **(a)** |
| remy-uoh whiff **R** | 5 | 10 | **5** | 5 | -- | **(a)** |

**12 clauses across 10 moves: 12x (a). Zero (b). Zero (c). Zero UNKNOWN
among tested rows.** Both divergence signs (overlay undershoot and
overshoot) land (a). Citations: twin column = an independent per-move
derivation, byte-matching the lane's own tables (`layer/a2-findings.md`,
`layer/a3-counterexample-findings.md`, plus this lane's own §0
re-derivations, `layer-verdicts.md`); arcade-990512 =
`CAPTURE.md`'s Session 3 record, raw fc citations therein
(`capture/session3-results.md`); arcade-990608 = `CAPTURE.md`'s Session
1-2 record.

Non-verdict rows recorded alongside (same-mechanism NOTEs, not new
verdicts -- arcade-captured proof is per-move/per-leg only):
- **chunli-uoh whiff** (control for the ENGINE-7 counterexample): twin
  15/10/5 == golden R (5, arcade-exact per corpus) == chunli.json
  15/10/5. Not arcade-captured this run; stays PASS.
- **chunli-uoh-block/-hit:** twin R=4 both legs == golden's engine-truth
  §13.13/F6 value, 1 short of the whiff-canonical 5 -- the disposition
  golden already records, not a new divergence.
- **urien-headbutt-lp whiff** (PASS row, diagnostic): twin R=16 = golden
  19 - 3 exactly. Bounds the instrument (see Limitations below);
  arcade-uncaptured.
- **h-forward-hit** (§13.10 row): twin S=18/A=1 vs golden 9/5 vs arcade
  9/6 -- a documented TWIN limitation (contact-leg hitstop), not an
  engine finding.
- **Revision re-validation:** all seven 990608 rows re-measured on
  990512 byte-reproduce (sean DS trio 5/6/36, 7/8/39, 9/9/42; urien-uoh
  15/10/5; yang-senk 34; ibuki-kaze 26; q-uoh 5) -- `CAPTURE.md`'s
  Session 3 B2 table. Revision explanations are ruled out for every
  re-validated row/clause.

**Prediction register (P1-P4), scored.**
- **P1 HELD (2/2):** twin gflg-R = 34 (yang-senk) / 26 (ibuki-kaze) ==
  arcade, controls green ⇒ (a) as pre-registered.
- **P2 MISSED:** twin R = **5**, not ≈3, for urien-uoh AND q-uoh -- the
  §13.8(b) "re-arm at anchor+0/+1" reading predicted the overlay's early
  cut was engine-real; it is not. The miss is itself the central
  finding: the engine re-arms where arcade does; the overlay's R
  construction mis-locates the end.
- **P3 MISSED:** urien-uoh twin raw-box A = **10**, not the predicted
  11. This scores as new measurement, not instrument failure:
  `active_pf`=11 is the overlay's OR'd `h_att_set` classifier (the
  cell-transition term can outlive raw dims, ENGINE-10 precedent) -- the
  raw four-s16 rig predicate reads 10.
- **P4 HELD exactly:** urien-headbutt twin R=16 = golden 19 - 3. The
  H2 busy==guard equivalence is per-move, not universal -- see the
  chunli reconciliation below.

**The chunli-uoh reconciliation (why twin busy-R matches arcade where
lever-K regressed).** Raw data: last box-active frame fc363; `gflg`
3->0 (the busy 771->768 edge) at fc369; `old_gdflag` catches up (768->0)
at fc370. Twin R = frames strictly between 363 and 369 = fc364-368 =
**5** -- matching golden (PASS) and chunli.json. The ENGINE-7 census
(§12.2.4 above, `docs/plan-frame-data-completion.md`'s ENGINE-7 row)
measured, in overlay-trace coordinates, "arcade end = rearm+1 for
chunli-uoh (the anchor itself, one tick after rearm)" vs rearm+0 for
yang/ibuki/yun/yang-uoh -- and lever K (trim the overlay's R end to the
rearm event) was REJECTED because that trim reconstructs arcade for the
+0 population but would regress chunli-uoh's arcade-exact 5 to 4. The
two constructions are honestly different: lever-K was a retime of the
overlay's existing per-move R endpoint (an anchor/cut event in the
overlay's own machinery) onto the rearm event -- whether that lands on
arcade depends on where each move's overlay endpoint happened to sit
relative to rearm (+0/+1/+3, the census's irreducible heterogeneity).
The twin does not retime anything: it derives R from scratch as the
count of frames strictly between the last raw-box-active frame and the
gflg 3->0 tick, excluding the actionable tick itself -- `CAPTURE.md`'s
arcade rule verbatim. Under that uniform rule chunli counts 5 and yang
counts 34, both arcade-matching, because the rule fixes which boundary
ticks are in-population instead of inheriting each move's overlay
endpoint. The raw re-arm edge is identical in both constructions (the
H2 equivalence, `layer/a1-mapping.md` (ii)); what differed was the
counting rule around it. This does NOT mean the census heterogeneity
was pure artifact: P4's urien-headbutt (rearm+3) reads twin R=16 vs
golden/oracle 19 on a PASS row, and headbutt was never arcade-captured
-- so whether arcade's own busy-R reads 16 or 19 there is UNKNOWN. The
twin's busy-R matches arcade on every row where arcade was measured
(12/12 clauses + chunli control), and disagrees with the oracle on at
least one uncaptured PASS row. Hence any future re-anchor program must
be census-first with selective arcade verification (see
`docs/plan-frame-data-completion.md`'s new LAYER-1 tracker row), never
a blind meter swap.

**Twin instrument record + limitations.**
- Validation: 6/6 controls exact (window #1) + a ryu-lp control repeat
  exact (window #2); both windows md5-proven applied/reverted, clean
  rebuild, post-revert control byte-identical, probe inert with env
  unset. Zero hypothesis revisions used.
- **Contact-leg hitstop limitation (h-forward-hit):** the twin excludes
  `hstop>0` rows from the S-window scan and the A tally on every leg; on
  h-forward-hit a 12-frame hitstop excursion overlaps the active window,
  distorting twin S/A to 18/1 vs golden 9/5 / arcade 9/6. Twin S/A on
  contact legs where hitstop overlaps actives is NOT trustworthy as
  speced this run; twin busy-R was unaffected (R=20 == golden's §13.10
  engine-truth value). All 12 verdict clauses above are whiff legs
  (frz=0 throughout) -- untouched by this limitation.
- Known measurement artifact: the one-time `player_number` H6 runtime
  confirmation printed `0 0` (fires before population); the mapping
  stands on the structural proof, and no S/A/R derivation reads it.

**Layer question -- answered.** (a) OVERLAY/METER everywhere tested;
(b) PORT retracted (no tested row shows engine-raw != arcade) -- see
PORT-DIVERGENCE-1's dated retraction (§13.13 exclusion item 2, above);
(c) REVISION ruled out everywhere re-validated (7/7 dual-revision
identical) and rendered moot by dual-revision agreement; data-ancestry
stays honestly UNKNOWN but no longer gates anything (the 990512<->990608
delta lives in one program chip, simm1; simm2 + all 40 graphics SIMMs
CRC-identical, `layer/ancestry-findings.md` §1.4 -- the PS2 port's own
data ancestry is separately UNKNOWN and moot for every tested row, since
both revisions agree with each other).

See `docs/arcade-frame-data/CAPTURE.md`'s new Session 3 section for the
full per-row arcade citations this section's table draws on, and
`docs/plan-frame-data-completion.md`'s new LAYER-1 tracker row for the
follow-up framing (OVERLAY RE-ANCHOR, user-gated).

**§13.16 addendum (CONTACT-A, dated 2026-07-14, opus-substitute / WANTS
FABLE RE-REVIEW).** The OVERLAY RE-ANCHOR follow-up flagged above was
executed for the contact-leg A overcount and returns a HARDWARE-CONFIRMED
positive + a proven-non-surgical negative, strengthening this section's
own (a) OVERLAY/METER verdict with a THIRD independent confirmation
layer (arcade hardware, this time on contact legs specifically, not just
whiff): Session 12 (`<sp>/zero/arcade-s12/session-report.md`) measured
arcade active-frames == ORACLE on all 6 grant-route contact rows
(`sean-exdragonsmash-block/-hit`, `remy-cbk-lk-block/-hit`,
`remy-cbk-ex-block/-hit`) — the oracle IS the arcade-true active-frame
value on these rows; the overlay's own displayed contact-A
(`effective_a`/`engine_a`, the declared-credit accumulator,
`frame_data_overlay.c:885-933`) over-counts it via a stacked +1
hitstop-boundary tick (the contact frame that starts hitstop is itself
box-active and gets counted) and, on the Remy legs, an additional F13
same-tick declared-credit-banking phantom re-credit. A new frz-excluded
strict box counter (`box_a_frz` = ticks where `box_active && hit_stop==0`,
a one-line sibling of the shipped `hstop_in_box` counter,
`frame_data_overlay.c:1918-1920`) reproduces oracle exactly on all 6 rows
— but a full-universe native census (`<sp>/zero/contactA/design.md` §3)
proves it is engine-signal-INDISTINGUISHABLE from **113** other
currently-PASS contact-A rows spanning all 19 characters that it would
regress: the structural-twin exhibit (`sean-exdragonsmash-block` needing
`strict`=12 vs `ken-srk-lp-block`/`sean-dragonsmash-mp-block` needing
`displayed`=8, byte-identical on every gate signal — `koc`, `box_runs`,
`hsib`, `engine_a==box_a`, event position) is the exact CONTACT-2 §4.1
UOH-18-unreachability shape reproduced on a THIRD independent basis. The
arcade-faithful target is the move's own whiff-sibling active count, not
any local contact-tape signal — `sean-dragonsmash-mp`'s own whiff was
separately hardware-measured at A=8 (`docs/arcade-frame-data/
CAPTURE.md:161-167`), while its contact `box_a_frz` reads 7: the SAME
strict construction that is exactly right for the EX-variant twin is
exactly wrong for the normal-strength twin. **No lever ships (letter V
stays unclaimed); zero golden edits; zero verdict flips.** Contact-A
therefore joins UOH-18 (this section, above) as an ARCADE-CONFIRMED /
PER-MOVE boundary — a genuine meter artifact whose true value is known,
not a counting-rule gap with an undiscovered fix. The 6 rows' xfail notes
are reclassified (`tools/frame-data/corpus-remy.yaml`, `corpus-remy-
ex.yaml`, `corpus-sean-ex.yaml`) from UNCLASSIFIED to ARCADE-CONFIRMED
METER-OVERCOUNT / TERMINAL-KNOWN-VALUE; oracle stands, overlay limitation
documented. Decisive follow-up (not blocking): a contact-leg arcade
capture of a sample of the 113 (starting with the `sean-dragonsmash-mp`
twin and `ken-srk-lp-block`) is the only thing that could resurrect a
blind lever — if arcade on the 113-class matches `box_a_frz` rather than
`box_a`, the 113 are mis-oracled and a *universal* re-anchor becomes
shippable (see `<sp>/zero/contactA/design.md` §8.3).

**Companion negative results, same session, same non-surgical shape (not
contact-A specific, recorded here as sibling LAYER-1-adjacent proofs).**
- **ADV-REANCHOR (`<sp>/zero/adv/plan.md`).** `adv = defender_idle -
  attacker_idle` (`frame_data_overlay.c:1243`) is arcade-faithful on
  600/607 currently-asserting rows; the 6 remaining divergent rows split
  into two structural classes (D-e: `yang-senkyuutai-lk-block`/
  `yang-exsenkyuutai-block`, a +6 idle-divergence where the busy-edge
  re-anchor points the WRONG direction; D-f: `akuma-sa3-block`/
  `chunli-sa1-block`, an undiagnosed +1 on truncated supers) plus D-g
  (harness bundle-context, below). Every candidate rule broad enough to
  catch the anomalies (`-(R+1)`, a flat `-6`, a flat `-1`) regresses
  86-600 currently-correct rows — the identical CONTACT-A shape, proven
  independently on the adv axis. No lever ships; all 4 D-e/D-f rows stay
  XFAIL, reclassified TERMINAL-NON-SURGICAL (`corpus-yang.yaml`,
  `corpus-yang-ex.yaml`, `corpus-akuma-sa3.yaml`,
  `corpus-chunli-sa1.yaml`).
- **HARNESS-BLEED (`<sp>/zero/harness-bleed/footprint.md`).** A
  reconstructed global dummy-idle-reset (normalizing the standing dummy's
  breathing-cycle phase to its isolated-run value at every corpus-entry
  label) was full-suite-tested: it is net-NEGATIVE — it fixes exactly 1
  row (`sean-ryuubi-hk-block`, D-g) but regresses 4 arcade-correct rows
  off arcade, including making 3 legitimately-connecting moves wrongly
  WHIFF (`urien-crroundhouse-block`, `alex-crroundhouse-hit`,
  `yun-nishou-forward-hit`) and shifting an arcade-exact fireball
  advantage off by one (`urien-msphere-hp-block`). REJECTED; not shipped.
  `urien-vkd-lk-block` and `sean-ryuubi-hk-block`'s xfail notes are
  reclassified DOCUMENTED HARNESS-BUNDLE-CONTEXT ARTIFACT
  (`corpus-urien.yaml`, `corpus-sean.yaml`) — the isolated-run value is
  arcade-exact for both (urien-vkd adv=-16; sean-ryuubi R=15/adv=-3), the
  bundled suite reads a bled value because the dummy's idle-breathing
  cell phase (`charset.c:2982` hurtbox pulse) drifts with run position;
  golden stays pinned to the canonical full-corpus (bled) value. One
  companion row, `urien-vkd-lk-hit`'s A/R clause, is CAPTURE-GATED (no
  arcade anchor resolves pinned-literal A=4/R=32 vs isolated A=5/R=31)
  and is tracked separately in `docs/plan-frame-data-completion.md`.

All three negatives (CONTACT-A, ADV-REANCHOR, HARNESS-BLEED) were
produced opus-substitute (Fable exhausted) and are flagged WANTS FABLE
RE-REVIEW — see each source doc's own §Fable-re-review-scope for the
specific claims to second (load-bearing: the non-gateability/
non-isolability proofs, since each overturns its own task's premise with
a negative result).

**§13.16 addendum (ORACLE-TABLE-INTERNAL-INCONSISTENCY, dated 2026-07-14,
opus-substitute / WANTS FABLE RE-REVIEW).** LAYER-1 above establishes that
where the arcade hardware, the engine-raw timeline, and the oracle table
agree, the OVERLAY meter is the divergent layer. The CAP-3 projectile
capture program (`<sp>/zero/cap3/instrument-report.md` + `phase2-report.md`;
`docs/arcade-frame-data/CAPTURE.md`'s CAP-3 session) and its offline fit
(`<sp>/zero/proj-split/fit.md`) surface a **second, structurally different
divergence layer for a real share of the remaining XFAILs: the published
arcade frame-data TABLES are not internally self-consistent across
characters.** This is not a port error and not a meter artifact — it is the
oracle set itself.

- **The identical-signal collision (the definitive finding).** On the
  projectile-split path (`use_proj_split = proj_seen && !use_hatt`,
  `frame_data_overlay.c:786`) the engine reports a single self-consistent
  convention, `S = proj_s` (the effect-init spawn flag). The engine value
  for a **PASS** row and an **XFAIL** row is **byte-identical**: `ryu-sa1`
  engine `S=3` (oracle 3 → PASS) vs `twelve-sa1` engine `S=3` (oracle 4 →
  XFAIL) — same `proj_spawn_slot=3`, same `proj=1`, same `box_runs=0`, same
  flash-free window, same code path (`proj-split/fit.md §1`). Likewise
  `ryu-sa1 proj_a=11` (S exact) vs `urien-sa2 proj_a=11` (S −1). Both
  projectiles become pool-valid on the same relative post-flash frame; the
  **only** difference is that `ryu.json`/`akuma.json`/`necro.json` publish
  projectile Startup on the *strictly-before / flag-frame* convention (which
  the engine's flash path reports) while `twelve.json` publishes it on the
  *spawn-slot / pool-valid-frame* convention (one higher). For no-flash
  Hadouken the engine reports spawn-slot (10) and the table also uses
  spawn-slot (10) → exact. **The −1 is an oracle-table convention
  inconsistency across characters, with no engine-visible signal revealing
  which convention a given move's table author used.** A broad `S += 1`
  flips 6 rows but regresses 8 currently-PASS rows (ryu-sa1 ×2, ryu-sa3,
  akuma-sa1 ×2, necro-sa3-hit, sean-sa1 ×2); every narrowing (flash-only,
  spawn-slot value, proj_a value, multi-projectile shape) collides on an
  identical signal (`proj-split/fit.md §2`).

- **The internally-over-budget oracle row (`urien-sa2`).** `urien-sa2`
  asserts oracle **S=1** and oracle **R=91 (PASS)** with the engine's whole
  measured window **T=91** (`corpus-urien-sa2.yaml`). Oracle S+R = 1+91 =
  **92 > 91**. `proj_s=0` yields R=91 (right) but S=0 (wrong); `proj_s=1`
  yields S=1 (right) but R=90 (wrong). **No single `proj_spawn_slot`
  satisfies both oracle fields — the oracle table is one frame internally
  over-budget on this row.** This is structurally unfixable in the engine,
  not merely unfit: the target itself is self-contradictory. CAP-3 hardware
  (`phase2-report.md` Target 4) independently confirms the engine's own
  layer is not the beam-A source either — arcade attacker T is FLAT 144 over
  6× spacing while the engine reports a false 11→83 distance-scaling, and the
  projectile's true box-active window is travel-dependent 34/85/105 with no
  flat canonical value for oracle A=75 to be. The row stays XFAIL asserting
  the (self-inconsistent) oracle; the divergence is now hardware-backed.

- **The recovery-side echo (`ibuki-sa3` vs `sean-sa1`).** The same
  cross-character basis inconsistency appears on projectile **R**: the
  fire-and-forget form the engine uses matches oracle exactly on ryu-sa1
  (49), ryu-sa3 (51), urien-sa2 (91), reads **+2** on ibuki-sa3 (32 vs 30),
  and **−2** on sean-sa1 (59 vs 61) — opposite signs, so no uniform offset
  reconciles them, because the tables measure projectile-super recovery on
  inconsistent bases (spawn-anchored, last-projbox→busy-edge, and
  late-despawn respectively) while the engine uses one (`proj-split/fit.md §3`).

**Consequence for the honest floor.** A genuine, characterized share of the
59 remaining XFAILs is **published-table inconsistency, not port error and
not a fixable meter bug**: the engine is arcade-faithful and self-consistent
(LAYER-1), and the divergence lives in the oracle set's own
cross-character/internal convention choices. Correcting these rows in the
port would require *fabricating* a value the engine never computes, to match
a table that disagrees with itself. They stay XFAIL, asserting the oracle,
with the inconsistency documented — the honest floor.

**§13.16 addendum (HONEST FLOOR, dated 2026-07-14, user decision —
opus-substitute / WANTS FABLE RE-REVIEW).** As of the drive-to-zero closing
pass the overlay is **frame-exact on 1,290 of 1,349 legs** and within ~1
frame on the remaining 59, with **every one of the 59 deviations diagnosed
and terminally classified** (per-class census below; full per-row notes in
`tools/frame-data/corpus-*.yaml` and `docs/plan-frame-data-completion.md`).
The user has chosen the **honest floor: NO override.** No display-convention
grant, no blind lever, and no golden re-pin to a fabricated value ships for
any of the 59 — each stays XFAIL asserting the arcade/oracle-true value with
its measured-vs-oracle delta, its hardware/sibling-proven true value (where
one exists), the exact reason it is not surgically fixable, and its terminal
class recorded in its own note. The 59 partition into seven terminal
classes, none of which admits a suite-safe fix:

| Terminal class | Rows | Why terminal (the honest reason) |
|---|---|---|
| **TERMINAL-NON-SURGICAL** | 17 | no census signal isolates the row; every candidate rule regresses 86–600 currently-PASS rows (adv no-signal + contact-A 113-regression + harness-bundle reset net-negative) |
| **ENGINE-DIVERGENCE** | 12 | a genuine engine behavioral difference (two-phase install, block-vs-hit recovery, stance-conditional R, sum-preserving A/R) — an override would paper over the divergence |
| **OVERLAY-DISPLAY-DIVERGENCE** | 10 | overlay self-consistent, arcade/whiff-sibling-true value asserted, small characterized offset, no suite-safe blind re-anchor exists |
| **CAPABILITY-GATED-remaining** | 8 | projectile rows, now hardware-measured (CAP-3) — the divergence is documented, gated behind the oracle-table convention inconsistency above |
| **CONVENTION/RULED** | 6 | genuinely ambiguous mapping between two hardware-real quantities (yang startup 5-vs-7; yun-zesshou A 15-vs-16), user-ruled, both readings defensible |
| **REACHABILITY-GAP** | 4 | the clean-whiff baseline is harness-unproducible (urien-chariot connects even at DIST_MAX; sean-sa1 Hadou-Burst projectile has no reachable whiff) |
| **ORACLE-TABLE-INCONSISTENCY** | 2 | the published table is internally over-budget / cross-character convention-split (urien-sa2 S+R=92>T=91) — correcting the port would fabricate a value to match a self-contradicting table |
| **Total** | **59** | all characterized known-limitations; zero UNCLASSIFIED |

No row remains UNCLASSIFIED. The engine is LAYER-1 arcade-faithful; the
residual 59 are the diagnosed floor, held honestly rather than papered over.

### 13.17 RE-ANCHOR-1: whiff-leg raw-signal A/R (normative; SHIPPED 2026-07-11, levers N/O)

**Status: SHIPPED.** LAYER-1 (§13.16) proved with a triangulated
instrument — arcade hardware (990512 REFERENCE + 990608) x a validated
convention-twin engine-raw probe x this overlay — that on every tested
row the engine's raw timeline is arcade-identical and the OVERLAY METER
mis-measures it. RE-ANCHOR-1 (`docs/plan-frame-data-completion.md`
LAYER-1 tracker row's follow-up, `docs/plan-frame-data-harness.md` §1.9
item 3) makes the overlay's WHIFF-leg numeric A and R measure the same
two raw quantities, gated by a full-suite census (predicted the exact
drift set before any edit) and two arcade capture sessions (the
"actionability" ladder step below).

**The two constructions.**
- **Lever O — raw-box A (`fd_whiff_raw_box_a`).** Displayed A = the count
  of consecutive frames where any of the four `att_box[j][0..3]` s16s of
  any of the 4 slots is nonzero (the twin's `attack_box_count()`
  construction, verbatim) — deliberately NOT `h_att_set`, which ORs a
  cell-transition flag with a width-only dim check and is proven to
  diverge from raw dims (ENGINE-10 shape).
- **Lever N — busy-edge R (`fd_whiff_busy_edge_r`).** Displayed R = the
  count of frames strictly between the last raw-box-active frame and the
  busy `771->768` edge (`guard_flag` 3->0 with `old_gdflag` still 3),
  first-actionable tick excluded — `CAPTURE.md`'s arcade counting rule
  verbatim, derived from scratch (no overlay endpoint anywhere in the
  construction).

Both levers are overlay-only (`frame_data_overlay.c`, zero engine-file
edits, zero new `fd_*` engine globals), gated identically: `outcome ==
WHIFF` (`g_cur.event == FD_OUTCOME_NONE`); `!use_proj_split`;
`!no_active_signal`; `box_count > 0`; **`box_runs == 1`** — the
contiguity guard (the §14 scattered-active precedent applied to the
numeric path, a uniform signal-shape gate, not a label list). Multi-run
whiff windows (none observed in this suite) keep today's paths,
byte-identical. Contact legs never take either branch by construction
(`outcome == WHIFF` excludes them).

**Why this is not ENGINE-7/ENGINE-6 redux.** E7 (lever K,
CENSUS-FALSIFIED, §12.2.4) was a *retime of the overlay's existing R
endpoint onto the rearm event* — whether that lands on arcade depends on
where each move's overlay endpoint happens to sit relative to rearm
(+0/+1/+3, E7's own irreducible heterogeneity); it was REJECTED because
chunli-uoh's arcade-exact 5 would regress to 4. Lever N derives R from
scratch under one uniform counting rule (strictly-between, both boundary
ticks defined by raw signals, no overlay endpoint anywhere) — under that
rule chunli counts 5 and yang counts 34, both arcade-matching, because
the rule fixes which boundary ticks are in-population instead of
inheriting per-move overlay endpoints (§13.16's chunli reconciliation).
E6 (lever L) died because two same-mechanism populations keyed arcade S
on different ticks with no observable discriminator; here the instrument
matched golden PASS on 6/6 controls + 12/12 LAYER-1 clauses, and the
positives outnumber the one open heterogeneity risk (below) by an order
of magnitude on the same code path.

**Finalize deferral (the one control-flow change).** 18(b)-family cut
anchors (levers G/H) set `attacker_idle` 2-4 ticks BEFORE the busy edge,
and legacy whiff finalize fires the same tick `attacker_idle` is set.
Lever N therefore defers the whiff finalize until `busy_edge_frame`
latches, bounded at 20 ticks past `attacker_idle` (max observed gap in
evidence is 4). The deferral predicate is `g_cur.event ==
FD_OUTCOME_NONE`, not `!needs_def_idle` (the latter is also true for
PARRY, which must never defer). A same-attacker-slot r1 edge
(`r1: 0 -> {2,3,4}`) — a same-r1 retrigger or a genuinely new move —
ends the wait early via new, narrowly-scoped detection logic (the
ordinary MOVE_START scan can't see this case, gated on `!g_cur.active`).
On timeout or that edge, finalize immediately: `fd_finalize()`'s own
lever-N gate falls back to legacy `recovery_pf` automatically
(`busy_edge_frame` is still unlatched), flagged on the FINAL line
(`busyr_fb=1`) as a diagnostic only. Every corpus's `inter_entry_wait`
(90-200 frames, mechanically asserted `> 20`) comfortably covers the
bound, so a deferred FINAL line is never dropped by the harness's own
wait/query sequencing.

**The §13.11 whiff-A amendment (PENDING-USER-1, ADOPTED 2026-07-11).**
§13.11's normative basis is "measured engine truth". At adoption
(2026-07-07) the declared-credit accumulator was the only engine-truth
quantity surviving contact sub-framing; its whiff-side side effect
(q-uoh displaying 11 over arcade's 10) was accepted as declared-truth.
LAYER-1 established a fifth, engine-native quantity — the raw box-frame
count — and proved it equals arcade on every tested whiff row. The user
adopted the rule directly:

> "whiff active-frames come from the raw hitbox count" — user, 2026-07-11.

This is a RULE grant, not an enumerated literal-by-literal grant:
membership derives mechanically from applying the rule to every whiff
window, each individually cleared by hardware/dual-source backing before
shipping (per the reviewer's standing requirement). Four members shipped
this commit, each onto its own arcade/oracle figure of 10: `q-uoh-whiff`
(11->10, backed by both arcade revisions + twin, sessions 2-3),
`chunli-uoh-whiff` (14->10), `dudley-uoh-whiff` (14->10),
`ibuki-uoh-whiff` (11->10) — the latter three backed by Session-4
hardware capture (`CAPTURE.md` Session 4 Target 2). Contact-leg A
(§13.11/§13.13 conversions) is unaffected — the rule is whiff-scoped by
the same gate as lever O. `ibuki-kubiori-lp-whiff`'s A:11 literal was
audited, not adopted as a fifth member (see the census disposition
below) — command grabs track active frames through a mechanism the
four-slot `att_box` array never observes.

**Census (Step 1, `<sp>/reanchor/census-report.md`).** Full-suite
(1059-row, 19-corpus) census before any edit: lever O survived clean on
every criterion (18/18 predicted A-cell rows exact, zero unpredicted
A-side movers). Lever N's Criterion 1 (no unpredicted PASS-mover)
**FAILED**: two currently-PASS, arcade-exact whiff windows —
`ibuki-twdsforward-whiff` (golden R=4, twin busy-R=1) and
`ibuki-twdsroundhouse-whiff` (golden R=21, twin busy-R=18) — moved away
from their oracle value, the E7/E6 death shape. No raw-signal
discriminator separated them from the 18 flip rows (`r2==1` shared by 3
agreeing flips; twds-family membership shared by 30 agreeing rows;
tail-existence and tail-phase both falsified by arcade-captured
positives, `<sp>/reanchor/leverN-rediagnosis.md` §1). The ONLY property
distinguishing the two counterexamples (plus the urien-headbutt trio) is
that oracle == overlay-natural-end == busy-edge+3 there, with an
identical 3-tick post-edge `rno3==3` residue signature, while on all 18
flip rows and all 12 LAYER-1-validated clauses oracle == busy-edge R
exactly.

**Session 4 (busy-edge) + Session 5 (actionability) — OUTCOME A.**
Session 4 (`<sp>/capture/session4-results.md`) captured the
urien-headbutt trio's busy-edge R per strength against its OWN oracle
(lp 19, mp 18, hp 19 — never a uniform 19): arcade plays 16/15/16,
confirming engine-arcade timeline fidelity but NOT adjudicating
oracle-vs-busy-edge (CAPTURE.md's counting rule IS the busy edge by
construction; re-applying it begs the question). Session 5
(`<sp>/capture/session5-results.md`) settled it with an actionability
probe (neutral stick, alternating LP/MP mash from 2 ticks before the
expected edge, `first_new_act_fc` = first frame a NEW act id with
`rflag` != 0 appears): both `ibuki-twdsforward-whiff` and
`ibuki-twdsroundhouse-whiff`, plus all three urien-headbutt strengths,
accept a new input exactly at the busy edge (`edge+0`), not at
`edge+3` — a validity control (`ibuki-kazekiri-lk-whiff`, an
already-arcade-exact busy-edge PASS row) confirms the probe measures
actionability, not routine end (it also accepts at `edge+0`, mid-tail).
**OUTCOME A: the busy edge IS the arcade's own first-actionable frame on
all five rows; the oracle's larger figure counts a 3-frame
non-actionable-claimed residue that is in fact actionable.** Lever N
survives ungated, exactly as census-scoped; the two counterexamples
reclassify as oracle-error rows under the flip rule's clause (ii)
(CAPTURE-1/sean-dsmp precedent).

**Corpus re-baselines from OUTCOME A (five oracle-error instances,
3rd-7th after Dragon Smash-mp):** `ibuki-twdsforward-whiff` R 4->1,
`ibuki-twdsroundhouse-whiff` R 21->18 (both whiff legs only;
`ibuki-twdsforward-block` R stays 4, `ibuki-twdsroundhouse-block` R
stays 22 — contact-leg values untouched, citation-only conversion);
`urien-headbutt-{lp,mp,hp}-whiff` R 19/18/19 -> 16/15/16 (each strength
against its own oracle, per Session 4/5 evidence for that strength); the
six `urien-headbutt-*-block/-hit` contact legs keep their measured
values (block 19/18/19, hit 19/18/19) but convert citation from `from-qjson` to an
explicit literal, since the underlying oracle field is now known wrong
for whiff-actionability and `from-qjson` is no longer a citable source
for it — whether arcade's own contact-leg R is also 3 lower is a
SEPARATE, unmeasured question, explicitly left open, not assumed either
way.

**SUPERSEDED (2026-07-13, see §13.19).** The claim above that the six
`urien-headbutt-*-block/-hit` contact legs "keep their measured values
(block 19/18/19, hit 19/18/19)", and the framing of contact-leg R as a
separate, unmeasured, open question, are both now FALSE — CONTACT-2
(§13.19) answers the question: those six goldens convert to 16/15/16
(Session 6/TRACK-A arcade-confirmed), matching the busy-edge
prediction. The historical text above is preserved for the record, not
deleted; §13.19 is the current source of truth for these six rows.

**Kubiori disposition (census-adjudicated).**
`ibuki-kubiori-lp-whiff`/`-block`: the twin construction finds ZERO
box-active frames anywhere in the window (busy goes `0->3->771` and
stays at 771 for 160+ frames, all four `att_box` slots zero throughout)
— consistent with Kubiori being a two-phase strike-then-catch move whose
active-frame accounting the engine tracks through a mechanism the
four-slot array never observes (the corpus's own comment already notes
`engine_a=11` for this window). Excluded by the pre-existing
`box_count > 0` gate — not a new rule, not the ladder, not a flip. Stays
XFAIL at its measured values (R=8 vs the first-triplet oracle
Recovery=15); item-18 does NOT close for this row.

**Contact-leg scope (explicit future scope-out).** RE-ANCHOR-1 applies
to WHIFF-outcome windows only. The twin instrument is not validated on
contact S/A (the h-forward-hit hitstop-overlap distortion, §13.16, is a
documented limitation of the very convention being adopted); contact R
is governed by GRANTED conventions (§13.10, F2/F5/F6/F8/F12) asserting
played-chain engine truth, by design != the whiff-canonical oracle —
busy-R would not fix the remaining contact xfails anyway (the twin
measured chunli-uoh-block/-hit busy-R=4, the granted engine-truth value,
not the whiff-canonical 5). Zero contact-leg drift, mechanically
verified across all 726 contact-outcome rows in the census and the
shipped pre-drift check. Contact-leg re-anchor is out of RE-ANCHOR-1
scope, recorded here as explicit future scope, not silently dropped.

**DATED NOTE (2026-07-12, CONTACT-1 closed at the Step-0 offline gate,
recorded here beside the scope-out above.)** The follow-on program
this scope-out named (contact-side counting-rule design + validation,
`<sp>/contact-plan.md`) ran its own pre-registered Step 0 — an
**offline rule-fit against the preserved RE-ANCHOR-1 twin logs**, zero
builds, zero arcade captures, zero repo writes (`<sp>/contact/
step0-report.md`, full per-row data in `<sp>/contact/step0-fit.tsv`,
726 rows). This is a first for the project: a candidate lever's
Sec6-shaped survival criteria (zero arcade-exact / C1 movers, zero
granted-literal / C2 movers) were checked and **falsified before a
single build ran**, at zero build cost — the ENGINE-6/7 census-closure
discipline (`:6548` §13.16, this file) applied one step earlier than
any prior candidate.

**Verdict: Rule P (contact-R, "count non-frozen ticks between the last
box-active frame and the busy 771->768 edge") does NOT survive.** 27
currently-PASS rows move (8 C1 + 19 C2, `<sp>/contact/step0-report.md`
§1), against a zero-mover bar. The 27 movers resolve into two clean,
traced, reproducible failure shapes plus outliers, none of which match
either of the plan's own pre-authorized Rung-1 fallback shapes:

- **The single-run "+1 freeze-inside-active" UOH family (15 rows):**
  `akuma-uoh-block`, `ken-uoh-block`, `oro-uoh-block`,
  `q-uoh-samef-block`, `q-uoh-1f-block`, `q-uoh-chain-retrigger`,
  `ryu-uoh-block`, `sean-uoh-block` (8 C1, arcade-exact) plus
  `akuma-uoh-hit`, `ibuki-uoh-block/-hit`, `oro-uoh-hit`,
  `q-uoh-samef-hit`, `q-uoh-1f-hit`, `sean-uoh-hit` (7 C2, granted).
  The plan's own pre-registered hypothesis for this family (hstop
  exclusion extended into the R window reconciles it, plan.md §3.2
  item 1) was frame-traced on two exemplars (`akuma-uoh-block`,
  `ken-uoh-block`) and **falsified**: the attacker's hitstop freeze
  occurs *inside the active run itself*, before the box drops, so the
  R window is already clean of hstop frames — the proposed filter has
  nothing to exclude.
- **The uniform "-3" urien-headbutt family (6 rows):**
  `urien-headbutt-{lp,mp,hp}-block/-hit`. Traced on
  `urien-headbutt-lp-block`: same shape, the entire 16-frame R window
  is hstop-clean. Rule P would silently move this granted family by 3
  with no new supporting evidence — the "movement toward oracle is
  still movement" ladder trigger (plan.md §3.6) — reopening a question
  §13.17 explicitly left separate and unmeasured ("whether arcade's
  own contact-leg R is also 3 lower", `:6851` above) without arcade
  backing.
- **6 non-conforming outliers**, no shared shape:
  `h-meatsquasher-short/-forward/-rh-hit` (three command-grab HIT legs,
  deltas -2/+8/+21), `ibuki-twdsforward-block` (-3), `remy-throw-hit`
  (-3), `yun-crfierce-block` (+5).

**Rule Q (contact-A, raw box-frame count) is REJECTED outright**,
exactly per its own LOW-confidence pre-registration: 490/492
currently-PASS computed rows move, zero C2 rows survive
(`step0-report.md` §1). No uniform gate search was worth running
against a >99% mover rate.

**A positive finding survives, preserved for a future design #2:**
Rule P's *other* mechanical fix — multi-hit "last box-active frame
across ALL runs" instead of the AS-IS first-run end — is independently
confirmed exact, 0 movers, on every multi-hit chain exemplar checked:
`sean-tornado-hk-block`, `dudley-mgb-lp-block`, `q-hsb-jab-block`
(`step0-report.md` §1, "item-2 fix"). The two mechanical fixes bundled
into Rule P solve two different, independently-verifiable problems; the
multi-hit reordering is real and reusable, the hstop-window extension
is not (it was aimed at a shape the traced evidence shows does not
exist). T1's 4 rows (`ibuki-kazekiri-lk-block/-hit`,
`yang-senkyuutai-lk-hit`, `yun-uoh-hit`) land exactly on their oracle
value under Rule P (`step0-report.md` §2) but are unshippable — a
lever cannot ship a rule that fails its own survival gate to reach 4
rows while moving 27 others.

**Disposition: CLOSED-RECORDED-FOR-LATER, per plan.md §7's own Step-0
exit gate** ("no P-variant reaches 0 C1/C2 movers ⇒ recommend
recorded-for-later"). Zero repo edits, zero builds, zero arcade time
spent to reach this verdict — the offline-fit method paid for itself
by killing the design before Step 1's census or Step 2's capture
budget was touched. **The bar for any design #2:** it must name an
actual mechanism for BOTH failure shapes above (not merely re-tune a
threshold against them) and reach 0/0 C1/C2 movers in a fresh offline
fit before any window-integrity census or arcade capture spend is
authorized. See `docs/plan-frame-data-completion.md`'s CONTACT-1
tracker row for the program-level disposition and target-list pointer.

**Census summary / totals.** 18 flip-table rows (predicted in
`<sp>/reanchor-plan.md` §5, confirmed by `<sp>/reanchor/census-report.md`)
flip XFAIL->PASS in full (no PARTIAL): `q-uoh-whiff-r`,
`urien-uoh-whiff`, `oro-uoh-whiff`, `akuma-uoh-whiff`, `sean-uoh-whiff`,
`urien-vkd-lk-whiff`, `yun-uoh-whiff`, `remy-uoh-whiff`, `yang-uoh-whiff`,
`yang-senkyuutai-lk-whiff`, `ibuki-kazekiri-lk-whiff`,
`remy-cbk-lk-whiff`, `dudley-uoh-whiff`, `elena-uoh-whiff`,
`necro-uoh-whiff`, `twelve-uoh-whiff`, `ibuki-uoh-whiff`,
`alex-uoh-whiff`. Plus PASS-cell-only golden drift (verdict unchanged):
`q-uoh-whiff` A 11->10, `chunli-uoh-whiff` A 14->10,
`oro-oniyama-lp-whiff` masked R 28->29 (MASK-1 stays — see the dated
§13.15 note above), `urien-headbutt-{lp,mp,hp}-whiff` R 19/18/19 ->
16/15/16, and the two OUTCOME-A `ibuki-twds*-whiff` re-baselines above —
26 golden rows total, zero outside this list (the full-suite pre-drift
check matched this exact set, cell-for-cell, before any golden write).
Arithmetic: **1001 -> 1019 PASS, 58 -> 40 XFAIL, rows unchanged at 1059**
(18 flips; the 2 `ibuki-kubiori-lp-*` rows stay XFAIL, unaffected).

**Files changed.** `src/sf33rd/Source/Game/ui/frame_data_overlay.c`
only in `src/` (new `FdSnap.box_active`/`.busy` fields,
`fd_snap_player()` raw reads; new `FdMove` tick-side tracking fields;
file-scope levers `fd_whiff_busy_edge_r`/`fd_whiff_raw_box_a`; the
finalize A/R branches; the deferral control-flow change; additive FINAL
diagnostics `box_a=`/`busyr=`/`box_runs=`/`busyr_fb=`). Corpus YAMLs per
the drift list above (relocate-don't-delete xfail evidence; dated
amendment/ERRATA comments carrying old literal, new literal, arcade
cite, capture cite). `tools/frame-data/golden/*.tsv` via scoped
`--update-golden` only. Zero engine-file edits; zero `charset.c` changes;
zero contact-leg behavior change; zero rollback/netplay surface (all new
state is overlay statics, training-only, read-only vs engine state).

See `<sp>/reanchor-plan.md` (the program design), `<sp>/reanchor/
census-report.md` + `.tsv` (Step 1), `<sp>/reanchor/leverN-rediagnosis.md`
(the ladder step), `<sp>/capture/session4-results.md` +
`session5-results.md` (the deciding hardware evidence), and
`docs/arcade-frame-data/CAPTURE.md`/`ERRATA.md` for the full per-row
citations.

### 13.19 CONTACT-2: contact-leg busy-edge R extension (normative; SHIPPED 2026-07-13, lever R)

**Status: SHIPPED.** RE-ANCHOR-1 (§13.17) re-anchored WHIFF-leg R to the
busy-edge construction (lever N) but explicitly left the CONTACT (HIT/
BLOCK) legs untouched, naming "whether arcade's contact-leg R is also 3
lower" a SEPARATE, unmeasured question. CONTACT-2 answers it: the SAME
busy-edge arithmetic lever N already computes on WHIFF (`busy_edge_frame
- box_last - 1`) is extended to CONTACT windows that pass a gate proving
the extension is safe — G1 (contact-only, HIT/BLOCK), the shared box/
proj/signal conjuncts N/O also require, G4 (UOH exclusion via the
engine's own dispatch tag, `fd_engine_move_is_uoh`), G5 (freeze-free
window, `hstop_after_box == 0`), G6 (specials-class latch, `koc == 5`),
and G7 (AMENDMENT 1 — `recovery_pf > 0`, excludes the ENGINE-8/jinchu
family whose legacy recovery tally is zero because the engine never
enters a recovery state at all). Lever constant `fd_contact_busy_edge_r`
declared next to N/O/S; lever at 0 restores today's `recovery_pf`
display byte-identically (identity-gate verified against the ac89cc8b
baseline, all 94 corpora).

**Design and process.** `<sp>/zero/contact2/design.md` (offline fit
against 726 preserved contact rows, 19 corpora) proposed an 18-row
payload; the Step-1 on-device census (full 94-corpus universe, 1,347
FINAL windows, `<sp>/zero/contact2/step1/census-full.tsv`) tripped Gate 1
on four deviations (D1-D4, AMENDMENT 1) that the offline 726-row/19-corpus
fit could not see: D1 (`oro-jinchu-lk-block`'s busy edge never latches
natively — twin-tape artifact, safe-direction), D2 (14 additional
genuine gate-passers in the SA/EX corpora outside the original fit's
scope), D3 (the G4 UOH-exclusion fallback compare, `cmoa.ix ==
waza_r[14][0]`, falsified on all 49 UOH windows — the engine's own
dispatch-tag latch is the sole mechanism), D4 (`yang-sa2-hit` overlaps
cell-disjointly with ENGINE-D2's own A fix, verified isolated at the
native-FINAL level). AMENDMENT 1 adjudicated every deviation (design.md
§8), adding gate condition G7 and growing the payload to 30
pre-registered rows plus 2 conditional `urien-exheadbutt` rows held on an
arcade capture (§8.3.7) — Arcade Session 10
(`<sp>/zero/arcade-s10/session-report.md`) resolved the blocker Branch A
(both contact legs measure R=12 on real hardware, 2 reps byte-identical
each, actionability-probe-confirmed, matching the busy-edge prediction
and diverging from the stale oracle-exact 15 by the same -3 magnitude
already established for the base family), growing the shipped payload to
**32 rows**.

**The 32-row payload, by disposition:**
- **4 verdict flips (XFAIL->PASS):** `ibuki-kazekiri-lk-block/-hit` (29->26,
  oracle-exact), `yang-senkyuutai-lk-hit` (38->34, oracle-exact),
  `yang-exsenkyuutai-hit` (29->25, oracle-exact, covered on its own
  contact tape despite no WHIFF sibling).
- **8 rows narrowed (R clause closed, still XFAIL on a separate clause):**
  `yang-senkyuutai-lk-block`/`yang-exsenkyuutai-block` (R closed at
  oracle, adv clause remains open, attacker_idle re-anchoring out of
  scope), `remy-cbk-lk-block/-hit` and `remy-cbk-ex-block/-hit` (R closed
  at oracle, F13 same-tick credit-banking A clause stands),
  `yun-exzesshou-block/-hit` (R closed at oracle, A clause stands, same
  shape as the base zesshou family).
- **8 rows already-PASS, golden-only churn (verdict-inert):** the 6
  `urien-headbutt-{lp,mp,hp}-{block,hit}` conversions (19/18/19 ->
  16/15/16, Session 6/TRACK-A arcade-confirmed) and the 2
  `urien-exheadbutt-{block,hit}` conversions (15 -> 12, Session 10
  arcade-confirmed, the design's own pre-registered blocker resolved
  Branch A).
- **12 rows R-only golden churn, verdict-inert (mixed disposition — 5
  XFAIL-stays-narrowed rows still failing on a non-R clause, plus 7
  already-PASS rows whose R was unasserted/omitted and so never gated
  the verdict):** `yun-zesshou-hp-block/-hit` (11->12, oracle-exact; A
  clause 16-vs-15 stands, TRACK-P's no-conversion stance preserved,
  XFAIL-stays), `necro-exflyingviper-block/-hit/-crouch-probe` (9->10
  x3, trace-verified freeze-free window; A still != oracle regardless,
  XFAIL-stays) — 5 rows XFAIL-stays-narrowed; `oro-oniyama-lp-
  block/-hit` (28->29, MASK-1 pre-authorized, PASS-stays),
  `oro-exoniyama-block/-hit` (42->43, MASK-1, corroborated by the
  move's own WHIFF leg already at 43, PASS-stays), `twelve-sa2-block`
  (48->49, oracle-exact, R stays unasserted per the Chun-Li
  SA2/Ruling-2 discipline, PASS-stays), `twelve-sa2-hit` (77->63, R
  stays omitted, no oracle cross-check on this leg, PASS-stays),
  `yang-sa2-hit` (42->40, R stays omitted/quarantined; cell-isolated
  from ENGINE-D2's own A=21 fix on the same row, PASS-stays) — 7 rows
  PASS-stays.
- **4 jinchu rows explicitly OUT of the payload, goldens untouched at 0,
  dated re-cite only:** `oro-jinchu-lk-block` (busy edge never latches
  natively, G7 moot), `oro-jinchu-lk-hit` (fires 0->33 natively but
  EXCLUDED via G7 — the engine's own legacy recovery tally is zero, the
  ENGINE-8 bounce-recovery signature), `oro-exjinchu-block` (busy edge
  never latches, same as base), `oro-exjinchu-hit` (fires 0->33 natively,
  EXCLUDED via G7; the native 33 does not match arcade's own EX-HIT
  truth of 34 either, since this move's active-window structure diverges
  from hardware — box_runs=1 native vs 3 active runs/25 raw box frames
  on arcade). All four re-route to the jinchu re-route track (TRACK-E,
  Session 6 hardware targets: base LK symmetric 33/33, EX asymmetric
  34/52).

**DATED POINTER (2026-07-14, ENGINE-JINCHU):** the TRACK-E re-route above
has landed. Lever U (`fd_jinchu_bounce_recovery_r`) relaxes this section's
own G7 conjunct (`recovery_pf > 0`) with a `|| (fd_jinchu_bounce_recovery_r
&& recovery_pf == 0)` disjunct, re-admitting the two recovery_pf==0
gate-firers this section already named above (`oro-jinchu-lk-hit`,
`oro-exjinchu-hit`) — `oro-jinchu-lk-hit` converts to PASS (busy-edge 33 ==
arcade 33 exact); `oro-exjinchu-hit`'s display moves 0->33 but stays XFAIL
(arcade 34, a genuine 1-frame divergence). The two BLOCK legs remain
unreachable exactly as this section predicted (busy edge never latches,
G7 never evaluated). See §13.21 for the full lever-U record.

**UOH-18 re-cite, dropped from this payload.** `design.md` §6.2
originally called for a dated CONTACT-2 unreachability re-cite on the
18 UOH contact-leg xfail notes (the same UOH family gate-excluded via
G4 above). That edit is deliberately NOT included in this commit: §8.5
(context postdating the original design) transferred UOH ownership to
the UOH-closure program (`<sp>/zero/uoh-fit/`) end-to-end — goldens,
oracles, and any future G4 relaxation — and Arcade Sessions 7-9
falsified the unreachability premise those re-cites would have
asserted (the busy-edge construction is hardware-accurate on every UOH
row tested; it is the published oracle, not the contact tape, that is
stale). The 18 UOH corpus files are untouched by this commit; any
citation update on those xfail notes rides the UOH program's own
payload commit.

**Suite delta:** 1,266 PASS / 81 XFAIL -> **1,270 PASS / 77 XFAIL** (+4
PASS / -4 XFAIL, zero PASS->non-PASS). Identity check (lever 0, full
94-corpus rebuild) byte-identical to the ac89cc8b baseline on every
measured column. Lever census: ten shipped lever constants asserted
(`fd_cut_requires_guard_rearm`, the pre-N/O six from earlier phases,
`fd_whiff_busy_edge_r`/`fd_whiff_raw_box_a` (N/O), `fd_movestart_same_tick_credit_hold`
(S, ENGINE-D2), `fd_contact_busy_edge_r` (R, this design)).

**Files changed.** `src/sf33rd/Source/Game/ui/frame_data_overlay.c` (new
`FdMove.hstop_after_box`/`.move_is_uoh`/`.koc` tick-side fields; the G4/
G5/G6/G7 gate expression; the new `fd_lever_r_applies` else-if branch,
placed between lever N's whiff branch and the legacy `recovery_pf`
fallback; additive FINAL diagnostics `hsab=`/`uoh=`/`koc=`/
`leverR_pred=`), `src/sf33rd/Source/Game/engine/pls03.c` (one-line
`fd_engine_move_is_uoh[wk->wu.id] = 1` set inside `check_leap_attack()`,
the unique UOH dispatch point), `src/sf33rd/Source/Game/engine/
workuser.c`/`.h` (the `fd_engine_move_is_uoh[2]` array declaration/
export), `src/main.c` (per-frame reset of that array alongside the
existing `fd_engine_hitbox_active` reset, before `njUserMain()` runs).
14 corpus YAMLs (`corpus-urien.yaml`, `corpus-urien-ex.yaml`,
`corpus-ibuki.yaml`, `corpus-yang.yaml`, `corpus-yang-ex.yaml`,
`corpus-remy.yaml`, `corpus-yun.yaml`, `corpus-oro.yaml`,
`corpus-necro-ex.yaml`, `corpus-oro-ex.yaml`, `corpus-remy-ex.yaml`,
`corpus-twelve-sa2.yaml`, `corpus-yang-sa2.yaml`, `corpus-yun-ex.yaml`)
per the drift list above (relocate-don't-delete xfail evidence; dated
amendment comments carrying old literal, new literal, and the arcade/
oracle/native citation). `tools/frame-data/golden/*.tsv` via scoped
`--update-golden` only. Zero `charset.c` changes; zero rollback/netplay
surface (all new state is overlay/engine-diagnostic statics, training-
only, read-only vs engine state, same class as the existing
`fd_engine_hitbox_active`/`fd_engine_active_count` arrays).

See `<sp>/zero/contact2/design.md` (the program design, AMENDMENT 1),
`<sp>/zero/contact2/step1/` (the on-device census that tripped and then
satisfied Gate 1'), `<sp>/zero/arcade-track-a/session-report.md` (Session
6, base headbutt + jinchu hardware targets), `<sp>/zero/arcade-s10/
session-report.md` (Session 10, the exheadbutt blocker resolution), and
`<sp>/zero/contact2/step2/` (this step's gauntlet transcripts) for the
full per-row citations.

### 13.20 UOH-CLOSURE: lever T — UOH contact-leg busy-edge R (normative; SHIPPED 2026-07-13, lever T)

**Status: SHIPPED.** CONTACT-2 (§13.19) extended the busy-edge/lever-R
construction from WHIFF to HIT/BLOCK contact windows, but its own G4 term
deliberately EXCLUDED the whole UOH (`press MP+MK` "Universal Overhead")
family — `!g_cur.move_is_uoh` — because the UOH class's own contact-leg R
was, at that time, an open question with contradictory-looking hardware
(session 8 read some UOH BLOCK legs as siding with the LEGACY display,
not the busy-edge construction — the sole "hard counterexample" CONTACT-2's
own design doc flagged and declined to resolve). UOH-CLOSURE answers it:
lever T (`fd_uoh_contact_busy_edge_t`) amends CONTACT-2's G4 conjunct from
a flat exclusion to `!move_is_uoh || (fd_uoh_contact_busy_edge_t &&
hstop_in_box > 0)` — routing UOH contact windows into the SAME busy-edge
arithmetic (`busy_edge_frame - box_last - 1`) lever R already computes for
every other contact-class move, gated on a new passive counter
(`hstop_in_box`: raw box-active ticks during which the attacker's own
`hit_stop` is nonzero, no event-tick exemption — `frame_data_overlay.c`
§3.3 in the design doc). Lever at 0 restores CONTACT-2's shipped gate
token-for-token (the new disjunct's second branch never fires, and the
non-UOH short-circuit is unchanged). Design: `<sp>/zero/uoh-fit/
uoh-design.md`.

**§2 finding — the discriminator story (engine degeneracy + the
whiff-contamination forensics, and its retraction).** The relayed rule
draft expected `hstop_in_box > 0` to be FALSE on a specific set of
"legacy-siding" BLOCK legs (ryu/ken/oro/sean/q), keeping them off the new
route. Native measurement across the kept CONTACT-2 census tapes found
the opposite: the attacker's 8-tick freeze lies INSIDE the raw box window
on **all 33** UOH contact windows in the engine, with zero exceptions —
including every leg the draft expected to exclude. Engine-side, UOH
contact ⇒ attacker freeze while the box is active, always; no engine
observable (act-id, `koc`, `box_runs`, character identity) separates a
"legacy" population from a "busy-edge" one, because at runtime there
isn't one — the drafted gate term is a blanket "UOH contact → busy-edge"
rule on the current verified universe. This left session 8's five
BLOCK-leg captures (ryu/ken/oro/sean/q, each reading its own LEGACY R) as
the design's one live counterexample to a design that predicted a
uniform 4/6/7/8-style busy-edge value on every contact leg.

Forensic resolution, BEFORE any arcade re-capture: re-deriving the
freeze/box overlap directly from the session-8 raw tapes (not the
session's own summary) showed those five BLOCK captures (plus Chun-Li's)
carried a complete **whiff signature**, not a block signature — (1) their
raw box window length equalled each character's own whiff-canonical
active count exactly, where every verified-contact leg's box window is
freeze-extended well beyond it; (2) zero attacker OR defender freeze
anywhere in the window, where a real block shows the standard 8-tick
guard freeze; (3) the wall-clock span from first-active to the busy edge
(`busy768 - f1`) equalled the whiff-canonical S+A+R sum on all six,
vs a strictly larger span on every real block tape; (4) the decompiled
arcade code itself (LAYER-1-confirmed faithful) freezes the attacker 8
ticks on a blocked UOH for every character, including these six — a real
block with zero attacker freeze would contradict the engine's own shared
hit-check path, but a capture where the move never connected contradicts
nothing. Plausible cause (not load-bearing): those six captures held `P1
Left` from a 50-56px anchor for the whole window, which walks the guard-
stance dummy backward through the 15-tick startup; these six characters'
short-reach UOHs then missed the retreated hurtbox, while their own HIT
legs (idle P1, no walkback) connected at the same anchor. **This RETRACTS
the earlier "hardware sides with legacy on ryu/ken/oro/sean/q BLOCK"
reading as unproven** (the captures measured whiffs whose R happened to
coincide with the legacy display for five of the six, and with each
character's own whiff-canonical oracle for all six) and dissolves the
sixth (Chun-Li) "exception" the same way — 5 is exactly Chun-Li's whiff R
measured on a whiff-shaped tape, not a genuine hardware divergence.
Every VALIDLY-measured UOH contact leg across sessions 7-9 (28 of them)
sides with the busy-edge construction with zero exceptions; the six
suspect BLOCK legs became this program's SESSION 11 capture blocker
instead of a shipped counterexample.

**SESSION 11 — the blocker, resolved.** `<sp>/zero/arcade-s11/
session-report.md` re-captured the six suspect legs (ryu/ken/oro/sean/
q-samef/chunli BLOCK) plus a direct `q-uoh-chain-retrigger` measurement,
with the dummy CORNERED (walkback structurally impossible) and a
mandatory in-tape contact witness required before citing any value
(attacker `frz` 8->0 overlapping a freeze-extended box window, vs the
whiff-shaped signature above). **Result: BRANCH A on all 7 legs, zero
counterexamples** — ryu-block 7, ken-block 8, oro-block 6, sean-block 8,
q-samef-block 6, q-chain-retrigger 6 (directly measured, not
by-extension), each exactly +1 over its own legacy display and exactly
matching the busy-edge/lever-T prediction; chunli-block 4,
display-invariant (legacy and busy-edge already agree), confirming
session 8's "5" reading there was the same whiff artifact as the other
five, NOT a genuine divergence — the pre-registered PASS->XFAIL exception
for Chun-Li does **not** apply; the row stays plain PASS. The blanket
UOH-contact-busy-edge route is confirmed real hardware behavior on every
one of the 35 contact legs measured across sessions 7-9-11 (28 + 7), zero
exceptions anywhere.

**Family-closure matrix (all UOH contact legs, arcade-measured):**

| Family | Legs | Busy-edge value | Sessions |
|---|---|---|---|
| akuma / yun / ibuki | block+hit, 6 rows | akuma 8/6, yun 8/6, ibuki 7/5 | 7 |
| ryu / ken / oro / sean / q-samef / chunli | BLOCK (whiff-contaminated in s8, re-measured), 7 rows | ryu 7, ken 8, oro 6, sean 8, q 6, chain-retrigger 6, chunli 4 (invariant) | 8 (voided) -> 11 (confirmed) |
| ryu / ken / oro / sean / q-samef / chunli | HIT, 6 rows | ryu 5 (new row), ken 6 (new row), oro 4, sean 6, q 4, chunli 4 (invariant, already valid in s8) | 8 |
| alex / dudley / elena / necro / twelve / urien / remy | block+hit, 14 rows (uniform 4/4, XFAIL-18 bucket minus yun/yang) | 4 (all 14) | 9 |
| yang | block+hit, 2 rows (structural outlier, XFAIL-18 bucket) | 10/8 | 9 |

**Engine-degeneracy finding (doc-integrity tripwire).** `hstop_in_box > 0`
is logically equivalent to "UOH contact happened" on the fully-verified
universe today — every engine-reachable UOH contact window freezes the
attacker inside the box, and every UOH whiff window does not. The counter
ships anyway as (a) the drafted rule's named mechanism in executable
form, matching the hardware discriminator's own stated definition
verbatim, and (b) a tripwire: Gate U1 (census, `<sp>/zero/uoh-fit/step1/`)
asserts the counter is nonzero on exactly the 33 known UOH contact
windows and `leverT_pred == -1` everywhere else; any FUTURE UOH contact
window this program has not yet seen with `hstop_in_box == 0` would fall
back to legacy display (the safe direction) and be flagged by that same
census, not silently mis-routed. A future reader must not mistake the
counter's current-universe equivalence to `move_is_uoh` for redundancy —
it is what makes the route self-auditing against the next new UOH-class
move this suite ever exercises.

**A does NOT convert anywhere in this payload** (design §5). Arcade A on
every measured UOH contact leg disagrees with this engine's own displayed
A in BOTH directions across different characters (akuma-block: arcade 10
vs engine 8; urien-block: arcade 14 vs engine 9 — opposite-sign errors,
no uniform construction), and CONTACT-2's own §4.2 already independently
falsified every uniform contact-A candidate (raw box, filtered box,
native `box_count`). The arcade A values are recorded as dated data lines
in each touched corpus's comment for a future A program; nothing asserts
them.

**Mover table (31 golden R cells + 2 new rows; baseline 1,270 PASS / 77
XFAIL / 1,347 rows):**

- **+16 PASS / -16 XFAIL** (the XFAIL-18 bucket minus `urien-uoh-block/
  -hit`, which narrow to an A-side-only residual and stay XFAIL — their R
  clause still re-literals to the arcade value, 4): `alex, dudley, elena,
  necro, twelve, remy` (block+hit, 4 each, s9) + `yang` (block 10/hit 8,
  s9, structural outlier) + `yun` (block 8/hit 6, s7).
- **13 currently-PASS value corrections** (highest-risk class, each with
  a per-row session citation carrying fc numbers): `akuma-uoh-block/-hit`
  (7->8/5->6, s7), `ibuki-uoh-block/-hit` (6->7/4->5, s7),
  `oro-uoh-hit`/`sean-uoh-hit`/`q-uoh-samef-hit` (+1 each, s8 valid-contact
  HIT legs) + the 6 SESSION-11 BLOCK legs (`ryu/ken/oro/sean/
  q-uoh-samef-block` +1 each, `q-uoh-chain-retrigger` 5->6).
- **+2 new rows** (`ryu-uoh-hit` R=5, `ken-uoh-hit` R=6), both PASS,
  arcade-cited (s8) + native-trace-confirmed (KEN-UOH-HIT confirmatory
  step, design §6).
- `chunli-uoh-block/-hit`: display-invariant (busy-edge == legacy == 4),
  stay PASS, no exception row.
- `urien-uoh-block/-hit`: R clause closes at 4 (arcade-cited, s9), stay
  XFAIL narrowed to the A-side-only residual (ENGINE-5 CLOSED, grant
  DECLINED).
- Untouched: all 16 UOH whiff windows; `q-uoh-1f-block/-hit` (native
  `uoh=0 koc=4`, outside the engine's UOH class, dispatch through a
  different path than `check_leap_attack()`); all 1,298 non-UOH windows,
  including CONTACT-2's entire 30-row payload at its shipped values.

**Suite delta: 1,270/77/1,347 -> 1,288 PASS / 61 XFAIL / 1,349 rows**
(+16 PASS / -16 XFAIL from the conversions, +2 rows both PASS) — verified
by full-suite `--check-golden`/`--update-golden` gauntlet, zero drift
outside the pre-registered 31 cells + 2 rows, zero deviation from this
exact arithmetic.

**Gauntlet.** Pre-drift census (full-suite `--check-golden`, tree with
lever T wired but goldens not yet updated) matched the pre-registered
31-cell + 2-new-row disposition EXACTLY — 0 deviations. Full-suite
`--update-golden` then `--check-golden` again: zero drift, totals exactly
1,288/61/1,349. G-identity (lever T=0, rebuild, full suite against the
ORIGINAL pre-payload goldens): zero MEASURED-column drift (S/A/R/adv/kd/
outcome byte-identical) on all 94 pre-existing corpora — the only
observed changes were VERDICT flips on the payload's own already-edited
rows (PASS->FAIL / XFAIL stays XFAIL), an artifact of the corpus's own
literal `expect.R` now demanding the arcade value that only lever T
produces, not a lever-identity violation; `ryu-uoh-hit`/`ken-uoh-hit`
(new rows, absent from the pre-payload baseline entirely) are documented
as T-dependent by construction and excluded from the identity comparison.
Determinism x2 on `ryu`/`ken`/`q` (payload) + `hugo` (non-member): all
four byte-identical across 2 reps. Lever census: **eleven** consts `=1`
(`F,G,H,I,J,M,N,O,S,R` + `T`), zero at `=0`. `git diff --stat src/`:
exactly `frame_data_overlay.c` (Step 1's diagnostics-only diff plus Step
2's G4-term amendment, one file — unlike CONTACT-2, lever T needed no
`pls03.c`/`workuser.c`/`main.c` changes, since it reuses CONTACT-2's
already-shipped `move_is_uoh` dispatch tag), zero `tools/frame-data/*.py`
changes. Every one of the 35 changed/narrowed/new UOH rows carries its
own session citation (s7/s8/s9/s11) in its corpus comment.

See `<sp>/zero/uoh-fit/uoh-design.md` (the program design), `<sp>/zero/
uoh-fit/step1/` (the diagnostics-only census that pre-registered the
33-window contact universe), `<sp>/zero/arcade-s7/`, `<sp>/zero/
arcade-s8/`, `<sp>/zero/arcade-s9/`, `<sp>/zero/arcade-s11/`
(session reports, the full per-row citations and raw fc numbers), and
`<sp>/zero/uoh-fit/step2/` (this step's gauntlet transcripts) for the
complete record.

### 13.21 ENGINE-JINCHU: lever U — jinchu-class bounce-recovery contact-leg busy-edge R (normative; SHIPPED 2026-07-14, lever U)

**Status: SHIPPED.** ENGINE-8 (§12.2.4's "Two-way contact R=0" row,
`docs/plan-frame-data-completion.md`'s ENGINE-8 row) closed
`oro-jinchu-lk-block/-hit` XFAIL at the oracle's whiff-canonical
Recovery=19, ruling NO-CODE-FIX-EXISTS: the move ends by `Player_normal`
dispatch handoff while the attacker is still airborne (r1 4->0 concurrent
with a discontinuous `cgix` jump into the generic airborne-neutral-jump
family, `cghi=14`), so it never enters an in-engine recovery state and the
legacy recovery tally (`recovery_pf`) is 0 — no candidate end-anchor in
the engine's own state (r1-clear, chart-idle, touchdown, stable-idle-loop)
reached arcade's 19. TRACK-A Session 6 (`<sp>/zero/arcade-track-a/
session-report.md`, sfiii3nr1 Japan 990512 NO CD, `derive2.py`, 2 reps/leg
byte-identical) falsifies the VALUE that closure preserved, not its
mechanism finding: real arcade hardware measures base-LK HIT/BLOCK R=33/33
(symmetric) and EX HIT/BLOCK R=34/52 (asymmetric) — none of which is 19 or
16 either. CONTACT-2's own preserved FINAL traces
(`<sp>/zero/contact2/step1/rundirs/{oro,oro-ex}/trace.log`) independently
show a real busy `771->768` edge exists and latches at `busyr=33` on
**both HIT legs** (base and EX) and **never latches** (`busyr=-1`) on
**both BLOCK legs** — the engine has a real, measurable busy-edge signal
on the HIT side of this move family that ENGINE-8's chart-idle/r1-clear
analysis never considered, because that analysis pre-dated CONTACT-2's
own busy-edge instrumentation (§13.19).

**The fix.** Lever R's own gate (`fd_lever_r_applies`, §13.19) already
computes this busy-edge arithmetic for every other contact-class move; it
excludes the jinchu family via G7 (`recovery_pf > 0`, the
engine-entered-recovery requirement) precisely because jinchu's legacy
recovery tally is 0. Lever U (`fd_jinchu_bounce_recovery_r`) relaxes G7
with an explicit, self-documenting disjunct:

```c
&& (recovery_pf > 0
    || (fd_jinchu_bounce_recovery_r && recovery_pf == 0));
```

Lever U = 0 restores `recovery_pf > 0` exactly (byte-identical to every
prior lever-R build); lever U = 1 additionally admits any
`recovery_pf == 0` gate-firer. No new instrumentation, no tick-side
change, no `pls03.c`/`workuser.c`/`main.c` edits — the same shape as
UOH-CLOSURE's own G4 amendment (§13.20), reusing only trackers CONTACT-2
already shipped (`busy_edge_frame`, `box_last`, `recovery_pf`, `koc`,
`box_runs`, `move_is_uoh`, `hstop_after_box`).

**The recovery_pf==0 discriminator (why this only ever reaches jinchu).**
Among ALL lever-R gate-firers suite-wide (the full 94-corpus pre-drift
census, `<sp>/zero/contact2/step1/census-analysis.txt`, 34-row would-move
list), a `grep ' 0 -> '` returns **exactly 2** rows — `oro-jinchu-lk-hit`
and `oro-exjinchu-hit`. Every other one of the 34 gate-firers already has
a nonzero legacy `recovery_pf`, so G7 passes for them regardless of lever
U's setting; relaxing G7 re-admits only the `recovery_pf == 0` subset,
which today is exactly these two rows. The three populations this
discriminator must NOT conflate, each independently verified (not
assumed):
1. **jinchu HIT vs jinchu WHIFF.** The whiff legs
   (`oro-jinchu-lk-whiff`, `oro-exairjinchu-whiff`) never reach
   `fd_lever_r_applies` at all: `event == NONE` fails the contact-only
   conjunct (G1), and `box_runs == 2` fails the single-contiguous-box
   conjunct — independent of G7/lever U. Base-whiff's own FINAL reads
   `busyr=19 box_runs=2` (a different busy timeline than the contact
   legs' 33 — whiff must not be conflated with contact); EX-air-whiff
   reads `busyr=44 box_runs=2`.
2. **jinchu HIT vs jinchu BLOCK.** The BLOCK legs read `busyr=-1`
   (`busy_edge_frame` never latches), so `fd_busy_edge_valid` is false
   and they fall out of the gate before G7 is even evaluated — lever U
   cannot reach them under any setting. Base-block's own FINAL reads
   `busyr=-1 box_runs=1`; EX-block reads `busyr=-1 box_runs=1`.
3. **jinchu HIT vs everything else G7 rightly excludes.** Every other
   lever-R gate-firer already has `recovery_pf > 0` (a nonzero legacy
   tally), so G7 already admits them regardless of lever U — the
   `recovery_pf == 0` disjunct is a no-op everywhere except these two rows.

**Per-leg measurability verdict (the deliverable):**

| Leg (corpus) | Arcade R | Engine `busyr` | Busy edge exists? | Disposition | Verdict after |
|---|---|---|---|---|---|
| `oro-jinchu-lk-hit` (corpus-oro) | 33 | 33 | yes, latches | **CONVERT** (busy-edge EXACT) | XFAIL -> PASS |
| `oro-exjinchu-hit` (corpus-oro-ex) | 34 | 33 | yes, latches | display moves, 1 frame short | XFAIL -> XFAIL |
| `oro-jinchu-lk-block` (corpus-oro) | 33 | -1 | NO (never latches) | UNMEASURABLE, honest XFAIL | XFAIL -> XFAIL |
| `oro-exjinchu-block` (corpus-oro-ex) | 52 | -1 | NO (never latches) | UNMEASURABLE, honest XFAIL | XFAIL -> XFAIL |

**The EX-hit 1-frame divergence (not a code-fixable miss).** Lever U
fires on BOTH hit legs or NEITHER — the two HIT legs are engine-identical
at the gate (`busyr=33`, `koc=5`, `box_runs=1`, `recovery_pf==0`, HIT,
non-UOH); no engine observable separates base-LK from EX. Firing on both
converts the one clean win (base-LK HIT, busy-edge 33 == arcade 33 exact)
at the cost of a display move that stays honestly divergent on EX-hit
(busy-edge 33, arcade 34 — the engine recovers 1 frame faster than
hardware; corroborated by the move's own active-window structure already
differing from hardware: native `box_runs=1` vs arcade's 3 active runs/25
raw box frames). The golden is engine-truth (33); the corpus `expect.R` is
arcade-truth (34) — independent checks, legitimately differing, the same
established precedent as UOH-CLOSURE's own "`expect.R`-literal artifact."
Firing on neither would forfeit the one clean, exact conversion for no
gain (the BLOCK legs are unreachable either way). This EX-hit
display-moves-but-stays-XFAIL disposition, and the block-legs'
unmeasurable-XFAIL disposition, were both flagged for Fable re-review at
plan time (`<sp>/zero/jinchu/plan.md` §9/§11 — Fable plan-access quota was
exhausted 2026-07-13, this design ran on an opus-substitute).

**The per-leg measurability split, restated as a single sentence:** of
the jinchu family's four contact legs, only the two HIT legs have a
measurable busy edge at all (`busyr=33` on both), and of those two only
the base-LK HIT leg's busy edge lands exactly on its own arcade value —
the BLOCK legs are structurally unmeasurable via any busy-edge instrument
(the edge never latches), not merely unconverted.

**Gauntlet.** G-identity (lever U=0, rebuild, full 94-corpus `--check-golden`
against the `f9522a26` baseline goldens, corpus files also reverted to that
baseline): **zero drift on all 94 corpora** (`94 GREEN / 0 RED`) — the G7
edit is byte-identical to HEAD when the lever is off. Gate 1 (census, TOP
RISK; lever U=1, corpus still at baseline, `--check-golden` against the
UNCHANGED goldens): drift on **exactly two** rows, both R `0->33`
(`oro-jinchu-lk-hit`, `oro-exjinchu-hit`) — zero surplus movers anywhere
in the 94-corpus universe; both BLOCK legs stayed `busyr=-1`/
`leverR_pred=-1`, unreachable as predicted. `--update-golden` then touched
**exactly 2 of 94** golden files (`oro.tsv`, `oro-ex.tsv`), each a single
cell (`R 0->33`); `oro.tsv`'s `oro-jinchu-lk-hit` row additionally flips
verdict XFAIL->PASS. Final `--check-golden`: zero drift, 94/94 GREEN.
**Suite delta: 1,288 PASS / 61 XFAIL / 1,349 rows -> 1,289 PASS / 60 XFAIL
/ 1,349 rows** (+1 PASS / -1 XFAIL, zero XPASS, zero FAIL, zero SHAPE,
zero NO-DATA, both before and after — re-measured live off the full-suite
run, matching the pre-registered arithmetic exactly). Determinism x2 on
`oro`/`oro-ex` (payload) + `hugo` (non-member): all three byte-identical
trace.log across 2 reps each. Lever census: **twelve** consts `=1`
(`F` in `charset.c`; `G,H,I,J,M,N,O,R,S,T,U` in `frame_data_overlay.c`),
zero at `=0`. `git diff --stat src/`: exactly `frame_data_overlay.c`
(23 insertions/2 deletions — the lever-U constant plus the G7 disjunct and
its two comment updates), zero `tools/frame-data/*.py` changes; the full
working-tree diff is exactly the eight pre-registered files (`corpus-
oro.yaml`, `corpus-oro-ex.yaml`, `golden/oro.tsv`, `golden/oro-ex.tsv`,
`ERRATA.md`, this doc, `plan-frame-data-completion.md`,
`frame_data_overlay.c`) — zero surplus files, oracle JSON and CAPTURE.md
both untouched as designed.

See `<sp>/zero/jinchu/plan.md` (the design), `<sp>/zero/arcade-track-a/
session-report.md` (Session 6, the hardware capture this design cites),
`<sp>/zero/contact2/step1/` (the preserved FINAL traces and pre-drift
census this design re-reads, not re-runs), and `<sp>/zero/jinchu/step/`
(this step's gauntlet transcripts) for the complete record.

## 14. Live meter coloring: §8.3 mirror + scattered-active handling

**Resolved 2026-05-04.** Earlier the live meter and the post-finalize
meter could disagree on the active-band painting for moves where
`engine_a` differed from raw[]'s per-frame ACTIVE count. Most
visibly on Q's Capture and Deadly Blow — cell 13 showed RECOVERY
during fill, then jumped to ACTIVE at move-end.

**Live mirror.** Each tick, after appending a raw[] cell, the live
classifier now applies the same engine_a-contiguous-band override
that finalize uses (§8.3). Implementation:
[`frame_data_overlay.c:793-827`](../src/sf33rd/Source/Game/ui/frame_data_overlay.c).
Result: cells 12-13 of CnDB show CONTACT/ACTIVE during fill and
stay that way through finalize — no visible repaint at move-end.

**Scattered-active skip.** For multi-hit moves (HSB Jab/Strong/
Fierce, Q's three-slap rapid special), the per-frame classifier
correctly marks each hit as a separate ACTIVE cell with RECOVERY
between them. The contiguous-band override would squash those
scattered hits into a single block, hiding hits 2 and 3 from view.
Both the live mirror and the §8.3 finalize override now check
whether ACTIVE cells are contiguous before applying — if scattered,
the per-frame painting is preserved. The user reports HSB now
visibly shows individual slaps in the meter.

Numeric S/A/R is unaffected by this section — it reads from the
per-frame tally before the override (§9), and `A` reads from
`engine_a` directly.

---

## 15. Sticky design decisions

These are the choices that worked and shouldn't be casually undone.

- **Engine accumulator over per-frame snapshot.** Active counting
  cannot be done from the overlay alone — `cg_ja.atix` is reset
  mid-tick by the engine for some moves. Hooking into `char_move()`
  is the only way to capture the true active-frame budget.
- **Whole-move active-signal pick (`use_hatt`).** Per-frame signal
  oscillation across `h_att` and `cg_hit_ix` would produce broken
  active phases. Picking once per move using "did `h_att` ever fire"
  produces stable classification.
- **Skip hitstop frames.** SF6 convention. Without this the meter
  has artifact "frozen" cells that confuse the user.
- **`attacker_already_idle` gate.** Without it, `raw[]` keeps
  appending while the defender is still in stun, inflating `R` by
  `(defender_idle - attacker_idle)` for short-recovery moves with
  long hitstun.
- **`event_idx` separate from `first_active_idx`.** The contact tick
  must sit where contact actually happened, not at the start of the
  active window — moves that connect mid-active (Q's far MK at
  distance) need the distinction.
- **Live `g_cur` rendering during the move.** Real-time fill is the
  feel users expect; without it the meter only appears at move-end
  and looks dead during the move.
- **Alternate-cell shading instead of borders.** 4-px cells don't
  have room for visible borders on CRT. Darkening every other cell
  to 75% is the cheapest way to make individual frames visible.
- **`r1: 4 → 0` for move-end, not `cg_cancel`.** Cancel-window-end
  fires several frames before arcade move-end on some moves.
- **Sentinel cells count per char_move call, not a flat cap.** The
  cell-data sentinel value (250) is a "wait" marker, not a duration.
  Actual duration varies per move (close HP = 11 calls, back+HP = 9) and
  is determined externally. Counting per-frame matches arcade A
  across both moves where a flat cap=10 only matched one. See §3.2.1.
- **Numeric S/A/R decoupled from meter override.** The §8.3 override
  anchors the visual meter to engine_a-cells, but the numeric tally
  reads from the per-frame classifier output *before* the override.
  Coupling them caused systematic ±1 errors when raw[] active count
  diverged from engine_a (hitstop on 1-frame active cells, sub-frame
  fast-forward on contact). See §13.1.

---

## 16. Hard rules

The user has stated these explicitly and they are non-negotiable for
this overlay's work:

- **The MVP is correct data.** Numeric values must match the arcade
  frame-data tables, not "be close." See
  `~/.claude/projects/-Users-sb-Developer-3sx-mister/memory/feedback-no-shipping-wrong-data.md`.
- **No "accept the small discrepancy and ship."** If something is
  off by 1, the only acceptable next step is to keep investigating.
- **Investigation work must be fact-based.** Every claim cited to a
  specific file:line, command output, or primary source. No
  "likely / probably / typically." Unknown is a valid finding;
  fabricated certainty is not.

---

## 17. File reference index

### Overlay + trace plumbing

| File | Role |
| --- | --- |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.{c,h}` | Overlay state machine, classification, render. Emits MOVE_START / FINAL trace annotations. |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:128-141` | Multi-segment recovery cut state in `FdMove` (`prev_cgix`, `cgix_reset_frame`, `cghi1_first_frame`, `cghi1_first_raw_slot`, `cghi1_count`) — §13.5.1. |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:160` | `bool kd` field in `FdLatched` for §13.6 KD advantage display. |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:206-212` | `fd_snap_player` extends `h_att_set` with `cg_ja.caix > 0` for catch hitboxes (§13.6). |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:296-300` | `fd_is_knockdown_at_atk_idle()` — last raw[] cell `def_r1 ∈ {2,3}` test (§13.6 KD). |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:325-348` | §13.3 no-active-signal guard — when `engine_a==0` AND no event AND `h_att` never fires, mark whole move STARTUP. |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:386-402` | §13.5.2 multi-hit recovery override — `last_active_pf_idx`-based recovery_pf. |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:482-484` | KD set in `fd_finalize` HIT branch (§13.6). |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:559-591` | MOVE_START scan + clearing of `fd_engine_active_count` / `fd_prev_active_cgix` on `r1: 0 → !=0` edge. |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:621-631` | §13.6 throw-event detection — partner `r1: 0 → 2/3` fires HIT outcome. |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:825-896` | §13.5.1 multi-segment recovery cut predicate (gate `r1==4 || r1==2`). Houses §13.6.1 partner-release short-circuit at lines 842-856. |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:793-827` | §14 live §8.3 mirror with scattered-active skip for multi-hit moves. |
| `src/sf33rd/Source/Game/ui/frame_data_overlay.c:920-948` | Draw path renders "KD" in `FD_COL_THROW` when `g_latched.kd` true (§13.6). |
| `src/sf33rd/Source/Game/ui/frame_trace.{c,h}` | Per-frame trace writer at `/tmp/3sx-frame-trace.log`. Captures cgix/cgctr/jatix/jcaix/cgcan/hstop/dstop/dcnt/sw_new/rno3/cmwk14/wcaix etc. for both players (34 columns/side as of 2026-05-04). Exposes `frame_trace_annotate(fmt, ...)` for overlay annotations. |
| `src/sf33rd/Source/Game/ui/frame_trace.c:28-29` | Trace columns `jcaix` (cg_ja.caix, catch hitbox index) and `cgcan` (cg_cancel) added 2026-05-04. |
| `src/sf33rd/Source/Game/ui/frame_trace.c` (TraceSnap + snap_player + emit_header/row) | Trace columns `rno3` (routine_no[3]), `cmwk14` (cmwk[14]), `wcaix` (cg_wca_ix) added 2026-05-04 — defender-side fields for §13.2 cr.* low-block hit-vs-block diagnosis. |
| `src/sf33rd/Source/Game/engine/charset.c:414-466` | `char_move()` — sentinel-aware `cg_ctr` accumulator (entry adds 0 for sentinel, per-frame adds 1 while still on cell) + per-frame `[CM]` stderr log. |
| `src/sf33rd/Source/Game/engine/charset.c:485-496` | `[CMX]` cell-data extended-code dispatch log — emits `code/koc/ix/pat` for player 1 every `check_cm_extended_code` fire. Needed for §13.5.2 and §13.7. |
| `src/sf33rd/Source/Game/engine/charset.c:2778-2795` | `set_jugde_area()` — secondary `fd_engine_hitbox_active` capture site. |
| `src/sf33rd/Source/Game/engine/charset.c:468-589` | `check_cm_extended_code` — cell-data opcode dispatcher (`comm_wca` / `comm_jmp` / `comm_ret` / `comm_end` etc.); now emitted via `[CMX]` log. |
| `src/sf33rd/Source/Game/engine/workuser.{c,h}` | Globals `fd_engine_hitbox_active`, `fd_engine_active_count`, `fd_prev_active_cgix`. |
| `src/main.c:547-548, 616-617` | `game_step_0()` — per-frame reset of `fd_engine_hitbox_active`, ordered `frame_data_overlay_tick()` then `frame_trace_tick()`. |

### Engine throw machinery (referenced by §13.6)

| File | Role |
| --- | --- |
| `src/sf33rd/Source/Game/engine/hitcheck.c:81-91` | `set_judge_result()` — routes `0x100`-flagged hits to throw path. |
| `src/sf33rd/Source/Game/engine/hitcheck.c:162-352` | `set_caught_status()` — sets `tsukami_f`, `tsukamare_f`, defender `routine_no[1] = 3`. |
| `src/sf33rd/Source/Game/engine/hitcheck.c:1491-1574` | `catch_hit_check()` — walks attackers, pairs with throw-eligible defenders. |
| `src/sf33rd/Source/Game/engine/cmd_data.c:1107-1108` | `pl_cmd[17] = p12_cmd` — Q's command-input table; verified to have no HCB+K slot. |
| `src/arcade/arcade_cmd_data.c:486-509, 731-739` | Arcade-balance fallback for Q — byte-identical to `p12_cmd`, also missing HCB+K. |
| `src/sf33rd/Source/Game/engine/cmd_data.c:297-299` | `p6_cmd_33` — Hugo's HCB+K, the working command-grab cmd-table reference. |

### Engine block / damage state machine (referenced by §13.2)

| File | Role |
| --- | --- |
| `src/sf33rd/Source/Game/engine/plpdm.c:309-352` | `Damage_04000()` — defender ground-block state machine. |
| `src/bin2obj/etc.c:3` | `_guard_pause_table[2][4]` — blockstun pause durations by attack level. |
| `src/sf33rd/Source/Game/engine/charset.c:237-244` | `char_move_wca()` — defender's guard-release entry point. |
| `src/sf33rd/Source/Game/engine/pls01.c:610-619` | `check_stand_up()` — sets `r2 = 7` on transition out of blockstun. |

### Reference data + traces

| Path | Role |
| --- | --- |
| `docs/arcade-frame-data/q.json` | Arcade ground truth, fetched 2026-05-04 from [Coccis77/thirdstrikedatabot](https://github.com/Coccis77/thirdstrikedatabot). 50 entries; canonical truth for verification. |
| `/tmp/3sx-frame-trace.log` | Runtime trace. 200 000 data-row cap; annotations don't count. Fresh capture 2026-05-04 has 7574 lines and contains Q normals/crouching/Dashing/HSB/UOH/HCB+K. |
| `/tmp/cm-trace.log` (stderr capture) | `[CM]` per-call accumulator log + `[CMX]` per-cell opcode-dispatch log for player 1 — sub-frame analysis. Fresh capture 2026-05-04 has 4655 `[CMX]` entries. |
