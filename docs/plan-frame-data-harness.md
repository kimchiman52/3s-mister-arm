# Plan — Frame-Data Self-Validating Harness + Remaining Frame-Data Work

**Status:** COMPLETE, 2026-07-07 — all phases (1-6) executed and committed on `frame-data-on-mister`. Remaining/new findings are tracked in docs/plan-frame-data-completion.md.
**Branch:** `frame-data-on-mister`. Baseline is `mister` (not `main`).
**Pairs with:** [`docs/frame-data-synthesis.md`](frame-data-synthesis.md) — the canonical
frame-data reference. Read it cover-to-cover before touching frame-data code.
**Caveat for future agents:** the synthesis doc's status tables are dated
2026-05-04/05 and predate commits `a386e057`/`118f7350`. Several items it
lists as OPEN are shipped (see Phase 2). Trust the code over the doc until
Phase 2 lands.

---

## Preamble

### Why this plan exists

The frame-data overlay work went through a long whack-a-mole cycle: fixes
shipped and reverted twice (synthesis §13.9), a day lost to a stale-binary
artifact (§13.4), and three open issues blocked on "need a clean
controlled-input retest." The root cause of all of these is that
**verification is manual gameplay**: a human plays moves in training mode,
then someone greps `/tmp/3sx-frame-trace.log` by hand. Every fix carries
unbounded regression risk because re-verifying the full move set costs a
session.

Phase 1 removes that root cause: a one-command harness that builds the
current binary, boots into training mode headless, executes a scripted move
corpus, and diffs the overlay's FINAL output against arcade ground truth
(`docs/arcade-frame-data/q.json`). After Phase 1, every subsequent phase's
verification step is "run the harness."

### Goal

`tools/frame-data/run.sh` → exit 0 with a green table, or exit 1 with a
per-move diff, in under ~2 minutes, fully unattended, on the macOS host
build.

### Non-goals

- Running the harness on MiSTer hardware. Host build only — the sim is the
  same C code; MiSTer-specific verification stays manual.
- CI integration. The game needs its AFS asset files, which are not in the
  repo; CI runners don't have them. Local-first. (A later phase may add a
  self-hosted/asset-cached CI job; explicitly out of scope here.)
- Netplay/rollback frame-data correctness. The overlay is a
  local-training-only instrument (accepted scope; see Phase 5 for making
  that explicit in code).
- Fixing the open frame-data issues themselves — that's Phase 4, *after*
  the harness exists to verify the fixes.

### Hard requirements (carry over from the synthesis doc §16 and memory)

1. **The MVP is correct data.** Match `q.json` exactly, not "close."
   Never propose "accept the small discrepancy and ship"
   (`feedback-no-shipping-wrong-data.md`).
2. **Same-binary protocol.** Trace and stderr captures must come from the
   binary built in the same run (§13.4 lesson). The harness enforces this
   structurally: build → run → check in one script.
3. **Investigations are fact-based.** Every claim cited to file:line,
   command output, or primary source.
4. **Sub-agents implementing steps of this plan must invoke `/implement`**
   (or `/plan` where a step says to plan first) — writing files without the
   skill loop doesn't count (`feedback-enforce-skill-invocation.md`).
5. **Don't degrade the MiSTer hot path.** Harness code is `#if DEBUG` /
   host-only; anything touching engine files must compile away in release
   flavors.

---

## Phase 1 — Self-validating regression harness (detailed)

### 1.1 Verified building blocks (inventory — all confirmed 2026-07-06)

Most of the harness already exists in-tree. Do not rebuild these:

| Piece | Where | Status |
|---|---|---|
| Scripted-input test runner, `#if DEBUG` | `src/test/test_runner.c` — `TestRunner_Prologue()` at `test_runner.c:1173-1347` phase machine (`PHASE_INIT→TITLE→MENU→CHARACTER_SELECT_*→GAME`) | Working; used by existing `--test-*` flows |
| Boots straight into **training mode** with forced characters/SA | `test_runner.c:1208-1222` (menu nav into training), `test_runner.c:724-767` (char/SA forcing), preset `training-yun-ryu-ryu-stage` at `test_runner.c:845-851` | Working precedent — needs a Q-focused preset |
| Deterministic char-select cursor table | `character_to_cursor[20][2]` at `test_runner.c:70-72`; Q = char id 17 | Working |
| Input injection choke point | `p1sw_buff`/`p2sw_buff` written by `keyConvert()` at `ioconv.c:123-124`, overwritten by `TestRunner_Prologue()` at `main.c:562-564`, latched at `main.c:581-582` | This is where scripted input goes |
| Existing RLE input record/replay (reference for format) | `Setup_Replay_Buff()` `sys_sub.c:1267-1293`, `Replay()` `sys_sub.c:1295-1341` — 16-bit words, low 12 bits state, high 4 bits repeat count | Format precedent only; harness uses its own file |
| Headless run | SDL `dummy` video driver is already the **default** (`config.c:34-35, 73`; `sdl_app.c:1817-1818`); `SDL_VIDEODRIVER` env honored (`sdl_app.c:1596`). NOTE: `--headless` CLI flag is a parsed no-op (`args.c:262`, zero consumers) — don't use it, don't trust it | Working via env/default |
| Frame trace with machine-parseable annotations | `frame_trace.c` — rows + `# F=N MOVE_START ...` / `# F=N FINAL ... S=. A=. R=. T=. adv=.` annotations from `frame_data_overlay.c:540,608`; training-mode gated (`frame_trace.c:217`) | This is the harness's output channel |
| Arcade ground truth | `docs/arcade-frame-data/q.json` (50 Q entries, from Coccis77/thirdstrikedatabot) | The oracle |
| CLI plumbing | `read_args()` `args.c:174-464`; test flags at `args.c:368-439`; dispatch `main.c:945-1003` | Extend, don't invent |

Gaps the harness must close (each is a step below):

- No per-frame input **script file** player (only preset button-mash logic
  and `--test-states` RAM-dump replay, which needs captured dumps).
- Training mode seeds RNG from wall-clock frame count:
  `game.c:350-352` sets `Random_ix32 = Interrupt_Timer` when
  `Mode_Type != MODE_NETWORK` → **non-reproducible**, and the training
  dummy's guard logic consumes RNG (`Guard_Data[zz][Lv][random_16_ex_com()]`
  at `com_sub.c:1875`).
- No control of player spacing (close vs. far normals resolve by distance).
- No control of dummy guard mode from the CLI.
- Trace path is hardcoded `/tmp/3sx-frame-trace.log` (`frame_trace.c:16`).
- No auto-exit when the script finishes.
- No checker that pairs FINAL lines with expected `q.json` values.

### 1.2 Architecture

```
tools/frame-data/corpus-q.yaml          (human-edited move corpus, single source of truth)
        │
        ▼  tools/frame-data/compile_corpus.py
   ┌────┴─────────┐
   ▼              ▼
 script.fdi   expected.json             (generated; never hand-edited)
   │              │
   ▼              │
 3S-ARM --test-enable --test-scene-preset training-frame-data \
        --test-input-script script.fdi          (headless, dummy video)
   │              │
   ▼              │
 trace log (FINAL + # SCRIPT annotations)
   │              │
   └──────┬───────┘
          ▼  tools/frame-data/check_frame_data.py
     pass/fail table, exit code
```

One YAML corpus file generates both the input script and the expectations,
so a move can never drift out of sync with its expected numbers. The C-side
script player stays dumb (plays words, emits label annotations); all
intelligence (motion macros, expectations, xfail policy) lives in Python.

### 1.3 Step H1 — input-script player (`src/test/`)

New file `src/test/input_script.c` (+ header), `#if DEBUG`, wired into
`TestRunner_Prologue()`'s `PHASE_GAME` branch so it only runs once
`training_mode_gameplay_started()` (`test_runner.c:719-722`) is true.

- New CLI flag `--test-input-script <path>` (follow the pattern of
  `--test-states` at `args.c:370`).
- **File format (`.fdi`)** — line-oriented, generated by Python, minimal to
  parse in C:
  - `W <p1_word_hex> <p2_word_hex> <frames>` — hold these input words for N
    frames (the only instruction that consumes time). Words are the
    `SWK_*` bit layout already documented in `replay_game.c:12-26`.
  - `L <label>` — emit `# F=n SCRIPT <label>` via `frame_trace_annotate()`
    (`frame_trace.c:248`) so the checker can pair the following FINAL(s)
    with a corpus entry.
  - `P <p1_x> <p2_x>` — teleport players to absolute X positions (writes
    the WORK position fields the same way test_runner already force-writes
    character/SA state). Needed for close-vs-far normal selection and
    throw range. Exact field names to confirm at implementation time from
    `WORK` in `include/structs.h` (the overlay's MOVE_START annotation
    already prints `atk_x`/`def_x`/`dist`, so the fields are known-readable
    — `frame_data_overlay.c` MOVE_START emit).
  - `G <mode>` — set dummy guard mode (see Step H3).
  - `Q` — end of script: request clean shutdown after a grace period
    (~120 frames) so the last FINAL flushes.
- Injection: overwrite `p1sw_buff`/`p2sw_buff` at the same site
  `TestRunner_Prologue` already uses (`main.c:562-564`). P1 = Q (attacker),
  P2 = dummy.
- The player must be a no-op unless both `--test-enable` and
  `--test-input-script` are set.

### 1.4 Step H2 — `training-frame-data` scene preset

Add a preset (enum + `args.c:29-39` list + `test_runner.c` table) that:

- Boots training mode (reuse the `training-yun-ryu-ryu-stage` machinery,
  `test_runner.c:845-851`).
- Honors `--test-p1-character` / `--test-p2-character` (already parsed,
  `args.c:384-439`) instead of hardcoding, defaulting P1=Q (17), P2=Ken.
- Forces a fixed stage (flat ground — reuse `--test-stage`).

### 1.5 Step H3 — determinism pinning + dummy guard control

- New flag `--test-pin-rng`: at battle start, zero `Random_ix16/32/_ex`
  exactly as network mode does (`Setup_Net_Random_ix()`,
  `sys_sub.c:1451-1458`) instead of seeding from `Interrupt_Timer`
  (`game.c:350-352`). Smallest patch: in `game.c`, gate the
  `Interrupt_Timer` seeding on `!configuration.test.pin_rng` (`#if DEBUG`).
- Dummy guard mode: the training menu's dummy/guard settings live in the
  training `contents[][][]` data (persisted by
  `src/port/config/training_config.c`, consumed via `menu.c` /
  `Guard_Type[2]` at `workuser.c:384` and `com_sub.c:573-589,1875,4922`).
  **Discovery sub-task:** identify which contents column maps to the GUARD
  setting and what values mean OFF / ALL / AUTO, by reading
  `menu.c` `Setup_NTr_Data` (`menu.c:4874-4904`) and the consumers above.
  Then implement `G <mode>` either by (a) poking that contents slot + the
  derived globals directly, or (b) pre-seeding the training config file the
  game loads (`training_config.c` reads a versioned struct — v2 as of
  `118f7350`). Prefer (a): no filesystem coupling.
  - The corpus needs three dummy behaviors: **stand/no-guard** (HIT and
    WHIFF outcomes), **guard-all standing**, **guard-all crouching**
    (the §13.2 cr.* investigation explicitly needs crouching-defender
    coverage).
- Two consecutive harness runs must produce byte-identical FINAL
  annotation sequences. That is the acceptance test for this step.

### 1.6 Step H4 — trace path override + auto-exit

- `FRAME_TRACE_PATH`: read `getenv("FRAME_TRACE_PATH")` with the current
  `/tmp/3sx-frame-trace.log` as fallback (`frame_trace.c:13`). The harness
  writes into a per-run temp dir so runs can't cross-contaminate (this also
  enforces requirement 2, same-binary protocol).
- Auto-exit: on `Q` directive + grace frames, exit the main loop cleanly
  (mirror however `--perf-capture N` terminates at `main.c:711-726`).
  Exit code 0 on script completion, non-zero if the script could not start
  (wrong mode, file missing) so the wrapper distinguishes "game never got
  to training" from "checker found diffs."

### 1.7 Step H5 — corpus compiler + checker (Python, `tools/frame-data/`)

**`compile_corpus.py`** reads `corpus-q.yaml` and emits `script.fdi` +
`expected.json`. Corpus entry shape (illustrative):

```yaml
- label: q-far-lp-block
  char: 17
  setup: { dist: far, dummy: guard-stand }
  input: "press LP"            # macro layer: press/hold/motion HCB+LK/UOH etc.
  qjson: { name: "Far Jab" }   # match key into docs/arcade-frame-data/q.json
  outcome: BLOCK
  expect: { S: 6, A: 4, R: 4, adv: from-qjson }
- label: q-uoh-chain-x3
  input: "press MP+MK; wait 20; press MP+MK; wait 20; press MP+MK"
  outcome: BLOCK
  expect: { xfail: "§13.7.1 same-r1=4 retrigger — A inflates" }
```

Macro layer requirements:

- Button chords with exact same-frame press (UOH needs MP+MK rising the
  same frame — the §13.7.2 S=15 vs S=16 distinction is *input timing*, so
  the compiler must support both same-frame and 1-frame-apart variants as
  separate corpus entries).
- Motion macros for specials: HCB+K (CnDB), charge moves (Dashing
  Head/Leg need back-charge — the macro must hold back ≥ the charge time
  before the forward+button frames), 2×QCF for supers (HSB). Validate
  against the cmd-table timing windows empirically — if a motion doesn't
  come out, the trace shows a normal instead, and the checker flags the
  mismatched move shape rather than silently comparing wrong numbers.
- `wait` long enough between entries for the previous move to finalize
  (defender must return to neutral), except entries deliberately testing
  chains/merges.

**`check_frame_data.py`**:

- Parses the trace: `# F=n SCRIPT <label>` opens a window; MOVE_START /
  FINAL lines inside the window belong to that label.
- Compares S/A/R/adv (and outcome) against `expected.json`.
- **xfail semantics** (this is what keeps the suite green while issues are
  open, without hiding regressions): an entry marked `xfail` that fails =
  reported as "known-open, still failing" (doesn't fail the run); an xfail
  entry that *passes* = **XPASS, fails the run** — it means a fix landed
  and the corpus must be updated (or a fix had an unexpected side effect).
  Everything else: fail on any mismatch.
- Output: one table row per corpus entry (`label | expected | got | verdict`),
  summary counts, exit code.

**`run.sh`** — the single entry point:

```sh
cmake --build build/host -j8                    # same-binary protocol
FRAME_TRACE_PATH=$RUNDIR/trace.log SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  build/host/.../3S-ARM --test-enable --test-scene-preset training-frame-data \
  --test-input-script $RUNDIR/script.fdi --test-pin-rng
python3 tools/frame-data/check_frame_data.py $RUNDIR/trace.log $RUNDIR/expected.json
```

with a hard `timeout` wrapper (a hung game must fail the run, not hang the
harness).

### 1.8 Step H6 — seed the corpus

Initial corpus = everything the synthesis doc already verified plus every
open-issue reproducer (from §13.8's reproducer table):

1. All Q standing/crouching normals from §12, BLOCK + WHIFF (expect exact).
2. Same normals on HIT (unblocks the §13.2/§13.2-HIT investigations —
   currently *no clean HIT captures exist*).
3. Q specials: Dashing Head ×3 (both fast and long-charge variants —
   `q.json` lists both), Dashing Leg ×3, HSB ×3, CnDB ×3 (HIT + WHIFF),
   UOH (same-frame and 1-frame-apart press variants; HIT + BLOCK).
4. xfail entries: UOH chain ×3 (retrigger A=22), multi-move merge (cr.MK
   blocked → immediate second button), cr.LK/MK/HK/HP block advantage
   (+2), CnDB R (+1), cr.MP HIT (variance).
5. Negative-control entries: taunt (S=raw A=0 R=0 honest display), a
   normal throw, a jump (nothing should finalize as garbage).

### 1.9 Acceptance criteria for Phase 1

1. `tools/frame-data/run.sh` on a clean tree exits 0; all non-xfail
   entries green; xfail entries reported as known-open.
2. Determinism: two consecutive runs produce byte-identical FINAL lines.
3. Mutation test: reverting one shipped fix (e.g. the jatix-gated revoke
   at `charset.c:432-451`) makes the run exit 1, flagging exactly the UOH
   entries. **Updated 2026-07-07 (Phase 4 item 1, §13.9.4 fix):** the
   expected flag set for the jatix-revoke mutation grew from two entries
   to **three** — `q-uoh-samef-block`, `q-uoh-1f-block` (both A=11, as
   before) plus `q-uoh-chain-retrigger` (also A=11) — because the new
   gated anchor-time `engine_a` snapshot reads the same live accumulator
   the revoke protects, so disabling the revoke un-revokes the
   snapshot's own tap-1 total too. A second, independent mutation test
   (force `use_anchor_a` off in `frame_data_overlay.c`) proves that new
   gate is itself load-bearing: it flags exactly `q-uoh-chain-retrigger`
   (A=20, the pre-fix value) and nothing else. Both mutations restore to
   green (`git diff` of the mutated file empty before re-running).
   **Updated 2026-07-07 (Phase 4 items 4+5, CnDB R+1 + honest T):** a
   third, independent mutation (mutation C — force the `ended_by_partner_
   release` release-R branch off in `fd_finalize()`, e.g.
   `if (false && g_cur.ended_by_partner_release)`) proves the R
   re-attribution is load-bearing: it flags exactly the three CnDB HIT
   entries (`q-cndb-lk-hit`/`-mk-hit`/`-hk-hit`, R=43/45/47, the pre-fix
   values) and nothing else, restoring to green after the gate is
   restored. Mutations A and B were re-run after the item 4+5 change
   landed and are unaffected (same flagged-entry sets as above) — see
   `docs/frame-data-synthesis.md` §13.6.1.
   **Updated 2026-07-07 (Step 5, issue #15 HIT-R convention adoption,
   `docs/frame-data-synthesis.md` §13.10):** the jatix-revoke mutation's
   expected flag set grew again, from three entries to **five** —
   `q-uoh-samef-block`, `q-uoh-1f-block`, `q-uoh-chain-retrigger` (all
   A=11, unchanged from the three-entry growth above) plus
   `q-uoh-samef-hit` and `q-uoh-1f-hit` (also A=11). This is not a new
   mechanism: those two HIT entries always ran through the same
   jatix-revoke-protected accumulator as their BLOCK siblings, but were
   previously `xfail` (citing the pre-Step-5 HIT-R divergence, R=3 vs
   arcade 5), which masked the mutation-induced A=11 FAIL under the
   xfail's own R mismatch. Step 5 flipped both to plain PASS (asserting
   R=3 as the §13.10 convention value), which un-masks the same A=11
   regression the mutation always produced on them — the underlying
   accumulator behavior did not change, only the corpus's ability to
   observe it did. Verified: applying the mutation (`0 &&` prefix on the
   `if (fd_prev_active_cgix_tick[wk->id] == Game_timer` condition,
   `charset.c` ~line 471) flags exactly these five entries and nothing
   else (`total=71 (FAIL=5, PASS=66)`); reverting (`git checkout` the
   file) and rebuilding restores `total=71 (PASS=71)`. Mutation B (the
   gate's own lever) is unaffected — still flags exactly
   `q-uoh-chain-retrigger` alone.
   **Updated 2026-07-07 (Step 2b, `docs/frame-data-synthesis.md` §13.11
   declared-truth displayed-A convention, adopted by user decision on the
   issue #17 Step-1 escalation):** the §13.11 convention re-derives the
   lever matrix, **superseding** both this item's five-entry text above
   and `docs/plan-frame-data-completion.md`'s own three-entry E3 row for
   the jatix-revoke mutation — both describe the *pre-§13.11* jatix-revoke
   mutation behavior and are no longer current truth. The jatix-revoke
   mutation (lever A) is now expected to flag **nothing** on any corpus:
   the §13.11 restore lives inside the same revoke block, so forcing the
   condition false skips subtract AND restore together, which is
   arithmetically identical to never subtracting (path-independence
   invariant — census confirms no corpus window contains a second
   same-tick same-jatix transit that could observe the difference). Lever
   A is retained as exactly this invariant check, not removed. The new
   restore-gate mutation (lever F —
   `fd_restore_revoked_declared_credit = 0` in `charset.c`, same `if`
   block as lever A) inherits the load-bearing proof: it flags exactly 5
   Q entries (`q-uoh-samef-block`, `q-uoh-1f-block`, `q-uoh-samef-hit`,
   `q-uoh-1f-hit`, `q-uoh-chain-retrigger`, all regressing to their old
   measured A=10) and 11 Ryu entries (the shape-(b) flip set, regressing
   to their old measured A — see synthesis §13.11's flip table), 0 Hugo
   (zero revoke sites, census 2026-07-07). Mutation B's expected FAIL
   value is now A=22 (was 20) — the anchor snapshot inherits the restore,
   so both taps now accrue 11 each.
   **Updated 2026-07-08 (§13.5.1a cut-gate fix): new lever G** — the
   `fd_cut_requires_grounded_fresh_edge` const in
   `frame_data_overlay.c`'s §13.5.1 tick-side block. At 0 the anchor
   condition reduces exactly to the legacy level test
   (`cghi==1 && reset>=0`). Expected flag set: the Q corpus flags
   NOTHING (the protected walls — q-uoh R=5, q-throw-hit R=34, CnDB
   R=42/44/46 — never depended on the gate's rejects); the mandatory
   full-12-corpus run with the gate forced to the legacy level test
   flags EXACTLY the 31 entries the fix flipped arcade-exact, at their
   pre-fix measured values — ken `ken-srk-lp-whiff/-block/-hit` R→19,
   akuma `akuma-srk-lp-whiff/-block/-hit` R→18, yun the 9 NISHOU
   entries R→16/17/19, urien the 10 chariot/headbutt/VKD-adjacent
   entries back to their pre-fix R/adv, dudley the 3 JET whiffs
   R→19/18/22, oro `oro-niouriki-lp-whiff/-block` R→11 +
   `oro-jinchu-lk-whiff` R→6 — and nothing else anywhere (in
   particular the 18 changed-but-still-xfail entries and every
   protected wall stay at their post-fix values, unmoved by the gate
   reverting). See `docs/frame-data-synthesis.md` §13.5.1a.
   **Updated 2026-07-09 (§13.5.1b guard-rearm commit gate fix): new
   lever H** — the `fd_cut_requires_guard_rearm` const in
   `frame_data_overlay.c`'s §13.5.1 tick-side block, gating on the
   engine's own `guard_flag` (cleared by `jumping_guard_type_check()`,
   `pls00.c:1160`, iff the current chart cell is guard-capable; set to 3
   every r1=4 tick by `Player_attack()`, `plpat.c:57`). At 0 a committed
   cut is trusted unconditionally, exactly like the pre-fix code.
   Expected flag set: the Q corpus flags NOTHING (`q-throw-hit` is
   r1==2, exempt from the gate by construction). The mandatory
   full-19-corpus run with the gate forced off CHANGES exactly the 33
   windows the fix changed, at their pre-fix measured values — but
   "changes" is not uniformly "flags": the checker only newly FAILS the
   20 of those 33 that are plain-PASS entries (no `xfail` key) — the
   ENGINE-2 family (`alex-airstampede-lk-whiff` R→2,
   `elena-scratchwheel-lk-*` R→25, `remy-rrf-lk-whiff` R→29) and the
   item-18(b) bonus family (`akuma-tatsu-lk-*` R→12, `ken-tatsu-lk-whiff`
   R→9, `chunli-hk-whiff` R→10, `chunli-hazan-lk-whiff` R→15,
   `sean-ryuubi-{lk,mk}-whiff` R→8/9, `yang-twdsmk-*` R→3,
   `yun-twdsmk-*` R→3, `alex-crfierce-*` R→19). The remaining 13
   (`remy-rrf-lk-{block,hit}`, `ken-tatsu-lk-{block,hit}`,
   `chunli-farroundhouse-block`, `chunli-hazan-lk-{block,hit}`,
   `sean-ryuubi-{lk,mk}-{block,hit}`+`hk-{block,hit}`) are already
   `xfail` on their own narrowed/adjacent clause — their regression to
   the pre-fix R value is SILENT under XPASS/FAIL semantics (still
   reported XFAIL, no new flag; the drift shows only in the measured
   value / golden diff, not the exit code) — and nothing else anywhere:
   every protected wall (q-uoh 5/3, q-throw-hit 34, CnDB 42/44/46,
   senkyuutai 38, oro-uoh/oniyama, ibuki-kubiori/-kazekiri,
   elena-lynxtail) and every lever-G-only entry stays at its post-fix
   value, unmoved by the H gate reverting. Combining G=0 with H=0
   together is the true legacy configuration (see
   `docs/plan-frame-data-completion.md` E3's lever-G row) — testing G=0
   with H=1 alone under-flags relative to the true pre-§13.5.1a
   behavior, since lever H would still refuse several of lever G's own
   legacy level-test anchors. See `docs/frame-data-synthesis.md`
   §13.5.1b.
   **Updated 2026-07-09 (§13.6.2b strike-KD latch fix, ENGINE-3): new
   lever I** — the `fd_strike_kd_latch` const in `frame_data_overlay.c`'s
   §13.6.2b tick-side block (right after the defender-idle-return check,
   non-throw branch), gating a sticky per-tick latch
   (`FdMove.strike_kd_seen`) that samples the defender's down-family
   membership (`fd_r2_is_down_family()`, also lever E's new anchor —
   forcing that helper false kills both levers' effects together, see
   `docs/plan-frame-data-completion.md` E3's lever-E row) on every tick
   of the finalize-deferred window, not just the last raw[] cell. At 0
   only the last-cell clause remains. Expected flag set: the checker
   flags NOTHING across the 13 affected corpora (akuma, alex, dudley,
   elena, ibuki, ken, necro, oro, remy, sean, urien, yang, yun — full
   run 2026-07-09); golden drift is EXACTLY `urien-vkd-lk-hit` (kd
   1→0) — the census-proven only member whose down-family traversal
   (`def_r2==14`) is invisible at the last raw[] cell (every other
   flip's own last cell already carries a down-family value: 17/18/20/
   21/23/27). This single-member signature is the latch's load-bearing
   proof.
   See `docs/frame-data-synthesis.md` §13.6.2b.
   **Updated 2026-07-09 (§13.12 hit-checkable projectile split fix,
   ENGINE-4): new lever J** — the `fd_proj_hitcheck_split` const in
   `frame_data_overlay.c`'s `fd_finalize()` `use_proj_split` branch,
   gating the `S' = max(proj_spawn_slot, proj_athok_slot)` /
   `R' = meter_len − (proj_firstact_slot + proj_a)` anchors (design
   E4-A′) added on top of the existing spawn/meter-end anchors. At 0,
   only the legacy spawn-slot/meter-end anchors remain; the three new
   engine-side latches (`fd_engine_proj_hitok`, `fd_engine_proj_cut`,
   `fd_engine_proj_natend`) stay unconditional write-only feeds — no
   rebuild is needed to restore goldens after restoring the lever.
   Expected flag set: flags EXACTLY the 9
   `twelve-ndl-{jab,strong,fierce}-{whiff,block,hit}` entries, regressing
   to their pre-fix measured values (S=12/12/13, R=26/30/34); all 45
   other `proj=1` rows (ryu/ken/akuma/chunli/oro/remy/urien-had/kik/
   nichirin/lov/msphere) are byte-identical under both lever settings —
   this identity is itself load-bearing (a lever-G-style negative,
   proving the new anchors are inert for every fire-and-forget
   fireball). See `docs/frame-data-synthesis.md` §13.12.
   **Updated 2026-07-11 (§13.14 multi-contact adv re-arm fix,
   ENGINE-9): new lever M** — the `fd_adv_last_stun_exit` const in
   `frame_data_overlay.c`'s tick-side non-throw defender block
   (immediately BEFORE the defender-idle-return latch), gating the
   `defender_idle` re-arm on a same-chart re-contact (5-conjunct
   signal — see §13.14). At 0 the write-once first-exit latch behavior
   is restored bit-for-bit. Expected flag set (measured 2026-07-11,
   full 19-corpus run): golden drift is EXACTLY
   `remy-crroundhouse-block` (verdict PASS→FAIL, adv −11→−41, the
   legacy stale first-exit anchor) and nothing else anywhere — the
   census-proven singleton re-arm population is the lever's own
   load-bearing proof (`e9-census`, 1,039 windows: only 2 windows
   suite-wide have a disjoint exit-then-re-stun topology, and the
   other, `q-uoh-chain-retrigger`, is refused by the
   `cgix_reset_frame<0` conjunct under both lever settings). See
   `docs/frame-data-synthesis.md` §13.14.
   **Updated 2026-07-11 (RE-ANCHOR-1, §13.17): new levers N/O — both
   file-scope in `frame_data_overlay.c`** (`fd_whiff_busy_edge_r` /
   `fd_whiff_raw_box_a`, declared together near `g_latched`, not
   function-local like G/H/I/J/M — lever N's mutation contract must
   gate both `fd_finalize()`'s R computation AND
   `frame_data_overlay_tick()`'s finalize-deferral control-flow from the
   SAME single toggle). Measured 2026-07-11, full 19-corpus run:
   **N=0** → golden drift is EXACTLY the 26-row R-side flip/drift set
   reverting to its legacy `recovery_pf` value (every row whose R moved
   under RE-ANCHOR-1) and nothing else — `chunli-uoh-whiff` (an
   O-only mover) shows zero drift under N=0, confirming the R/A
   decoupling. **O=0** → golden drift is EXACTLY the 9-row A-side set
   reverting (`alex/chunli/dudley/ibuki/q(x2)/remy(x2)/urien-uoh-whiff`),
   including the 4 PENDING-USER-1 amendment literals failing — the
   load-bearing negative the plan pre-registered. **G=0/H=0 combined**
   (adjacent machinery, full toggle) → zero RE-ANCHOR-1 whiff rows
   affected at all: lever N's busy-edge R is derived entirely from raw
   `box_active`/`busy` signals, never from `attacker_idle` or any cut/
   rearm state, so disabling G/H changes only the (unrelated) contact-leg
   and pre-existing G/H-contract whiff rows (Akuma/Ken Tatsu, Chun-Li
   Hazan/far-RH, Yun Nishou, Oro Niou Riki/Jinchu, Alex air-special
   family, etc. — G/H's own established E2/E3 members, unaffected by
   this commit) — this is a stronger, cleaner independence than the
   census's own pre-OUTCOME-A hedge predicted (it had reserved the
   possibility of a `busyr_fb=1` fallback path; none occurs, since N
   never depended on G/H's timing in the first place). **M=0** →
   unchanged single-row E9 contract (`remy-crroundhouse-block` adv
   only). Determinism, twin loop-closure, and the byte-identical
   restore-to-1 were all separately verified. See
   `docs/frame-data-synthesis.md` §13.17 and
   `docs/plan-frame-data-completion.md`'s RE-ANCHOR-1 tracker row.
4. Wall clock ≤ ~2 minutes.
5. No harness code compiles into non-DEBUG builds (verify the MiSTer
   flavor still builds and its binary contains no `input_script` symbols).
   **Updated 2026-07-12 (EX/Supers program Commit 0): new opt-in test flag
   + corpus keys, same zero-behavior-when-unset contract as every existing
   test flag.** `--test-training-sa-gauge <0-3>` (`args.c`,
   `configuration.h`'s `training_sa_gauge` int, -1 sentinel = unset) pins
   the training-mode S.A.GAUGE menu cell every frame and re-arms
   `init_E3_flag` — but ONLY on an edge (`apply_training_sa_gauge_overrides()`,
   `test_runner.c`): each frame compares the observed `spmv_ng_flag2` against
   the pattern `effe3.c`'s own switch guarantees once latched
   (`training_sa_gauge_expected_flag2()`) and re-arms only on mismatch. The
   first attempt (holding the re-arm every frame unconditionally) produced a
   destructive 2-frame `spmv_ng_flag2` oscillation — caught by a temporary
   same-binary-protocol diagnostic print, root-caused, fixed with the
   edge-triggered comparison above, re-verified stable (no oscillation, 7/8
   INFINITY / 8/8 MAXIMUM SA3 activations), then the diagnostic print was
   stripped before this commit (verification artifact, not shippable;
   `git grep gauge-probe` → zero hits). `compile_corpus.py` gained two
   optional top-level corpus keys, `super_art`/`sa_gauge`, threaded to
   `meta.json`'s `p1_super_art`/`sa_gauge` **only when a corpus sets them**,
   and a `2qcb` motion macro (mirrors `2qcf` with `BACK` in place of
   `FORWARD`, needed for back-motion supers, unused by any existing corpus).
   `run.sh` passes both through as extra CLI args only when present. Gate
   (this commit): all 20 `tools/frame-data/corpus-*.yaml` recompiled via
   `compile_corpus.py` before/after — `script.fdi`/`expected.json`/
   `meta.json` byte-identical for every one (none use the new keys); full
   `run-suite.sh --check-golden` 19/19 GREEN, zero drift; the standing EIGHT
   lever constants (`frame_data_overlay.c:339-340`,`:897`,`:1360`,`:1385`,
   `:1494`,`:1516`; `charset.c:507`) all still `== 1`, unaffected (this
   commit touches no engine/overlay file); E4 cross-build clean
   (`tools/mister/build-game.sh --flavor telemetry`), zero `InputScript` /
   `training_sa_gauge` / `gauge-probe` symbols in the ARM binary — the flag
   is `#if DEBUG`-scoped like every other test flag and does not reach the
   shipped build at all.

### 1.10 Risks / open questions

- **Motion recognition under scripted input** — cmd-table windows
  (`cmd_data.c`) may need specific lever timing; budget iteration time on
  the HCB/charge macros. Mitigation: the trace itself shows what move came
  out (`cghi`/`kow` identification recipe, synthesis §13 end), so failures
  are diagnosable offline.
- **Dummy guard control** — the contents-column mapping is a discovery
  task (Step H3); if poking globals proves fragile, fall back to
  pre-seeding the training config file.
- **Spacing** — the `P` teleport primitive assumes writing WORK X
  positions mid-neutral is safe (training mode's own position-reset does
  something similar). Verify by observing close vs. far normal selection
  in the trace.
- **Auto-timing** — the compiler's default inter-entry `wait` must exceed
  the slowest finalize (defender blockstun on heavy moves); start at 120
  frames and tune down.

---

## Phase 2 — Truth-up the synthesis doc (small)

The doc self-describes as canonical but is behind the code. One pass:

- Mark as SHIPPED (with commit refs): §13.7.4 jatix-gated same-tick revoke
  (`charset.c:432-451`), §13.7.7-rec-2-style engine_a snapshot at
  attacker-idle (`frame_data_overlay.c:114-122, 421-431, 858-868`), atk=1
  MOVE_START filter (`frame_data_overlay.c:583-586`).
- Update the "Git state: uncommitted" note (§ status snapshot, line ~48)
  — everything is committed on `frame-data-on-mister`.
- Add pointers to this plan and (once it exists) the harness as the
  standard verification method, replacing the manual fresh-capture
  protocol as the default (keep the manual protocol documented for
  MiSTer-side checks).
- Fold `docs/hsb-recovery-investigation.md` and
  `docs/q-hcb-k-reinvestigation.md` summaries in (or mark them historical)
  per the doc's own "no sibling investigation docs" rule.

## Phase 3 — Re-baseline = first real harness run (small)

The first full harness run **is** the re-baseline the open issues need.
Expected outcomes to record in the doc:

- CnDB R: resolves the §13.6.1 internal inconsistency (doc predicts exact;
  §12 table says R+1 from a possibly-stale binary). Whichever it is, update
  doc + corpus.
- cr.MP HIT variance: with pinned RNG, either the variance disappears
  (was RNG-driven dummy behavior — note `com_sub.c:1875` consumes RNG in
  guard logic) or it reproduces deterministically and becomes diagnosable.
- Fresh confirmation of which xfail entries still fail on the current
  binary.

**Results (2026-07-07, H6 acceptance run — 69 entries, 56 PASS / 13
xfail, deterministic under `--test-pin-rng`).** See
`docs/frame-data-synthesis.md` §12/§13.2/§13.6.1/§13.7/§13.8/§13.9 for
the full write-up; summary:

- **CnDB R+1: resolved as real, not stale-binary.** R=43/45/47 vs
  `q.json`'s 42/44/46 reproduces on the fresh same-run binary.
- **cr.MP HIT variance: gone under pinned RNG** (two independent
  captures byte-identical) — confirms the RNG-driven-dummy-guard
  hypothesis (`com_sub.c:1875`).
- **cr.* BLOCK advantage +2: did not reproduce** (cr.LK/MK/HK/HP block
  advantage matches `q.json` exactly under pinned RNG + controlled
  spacing) — likely an artifact of the old manual-capture conditions.
- **UOH same-r1=4 retrigger: alive**, now with a scripted reproducer
  (`q-uoh-chain-retrigger`, A=20 at wait=38f).
- **UOH clean A+1: fixed** — `a386e057`'s jatix-gated revoke verified
  working (A=10 exact, mutation-tested). **UPDATE (2026-07-07, Step 2b,
  `docs/frame-data-synthesis.md` §13.11 declared-truth displayed-A
  convention):** the "A=10 exact" here is pre-convention history —
  displayed A is now 11 (the declared credit, restored via a new gated
  flag, `fd_restore_revoked_declared_credit`); see §13.11 for the
  rationale and this doc's own §1.9 item 3 update for the re-derived
  lever matrix.
- **cr.MK multi-move merge (§13.9.1 case): not reproducible** on the
  current binary — cr.MK's own recovery outlasts dummy blockstun by 3
  frames, so the merge window never opens for this move.
- **New finding:** cr.LK never sets `event_this_frame` against a guard
  (finalizes WHIFF instead of BLOCK/HIT on every variant tried).
- **New finding:** a HIT-R divergence family (HSB, Dashing Leg RH, UOH,
  Throw) — first clean HIT captures for these moves; R differs from
  `q.json`'s WHIFF/BLOCK-matching figure. UOH's case is already
  explained by §13.7.3's cut-window analysis; the others need
  per-move follow-up.

## Phase 4 — Fix the remaining known-wrongs (medium; one `/plan` + `/implement` each, verified by harness)

Ordered by value; each fix's done-criterion is "harness entry flips from
xfail to pass, zero other entries change":

1. **Same-r1=4 retrigger + chain merge (A=22).**
   **Phase 3 update (2026-07-07):** retrigger half confirmed ALIVE with a
   scripted reproducer (`q-uoh-chain-retrigger`, wait=38, single FINAL
   A=20 — timing-dependent, doc's own F=1339 case shows A=22 for the
   same mechanism). Merge half (§13.9.1's F=4940-style deferred-finalize
   case) did NOT reproduce with the only move pair tried
   (cr.MK + second button, `q-crmk-multimove-merge`) — cr.MK's -3 block
   advantage means attacker recovery outlasts dummy blockstun by 3
   frames, so the "attacker idle, dummy still blockstunned" merge window
   never opens for this move; a positive-advantage move (e.g. Jab, +2)
   would be needed to actually hit that window.
   **Phase 4 update (2026-07-07): retrigger half DONE, shipped 2026-07-07 on `frame-data-on-mister`
   (this change).** The originally-designed approach — override
   `engine_a` with the raw[]-derived ACTIVE/CONTACT count when the
   §13.5.1 cut fired (synthesis §13.7.5, `g_cur.cghi1_first_frame >= 0`)
   — was tried during diagnosis and **falsified**: that quantity
   (`active_pf`) is 13 for the retrigger and for both already-passing
   clean-UOH BLOCK entries, never 10 (synthesis §13.7.5/§13.9.4 has the
   falsification detail). The shipped fix instead snapshots the engine
   accumulator itself (`fd_engine_active_count[atk]`) at the same
   cghi=1-dwell anchor §13.5.1 already cuts recovery at, and displays
   that snapshot in place of the live counter only when the cut
   committed (`cghi1_count >= 3`) AND the dwell was interrupted by the
   retrigger (`cghi1_dwell_broken`) — synthesis §13.9.4 has the full
   mechanism, verification, and both mutation-test results. Neither
   reverted attempt (§13.9.1/§13.9.2) was re-shipped; this is a third,
   distinct, finalize-read-only approach. Merge half (§13.9.1's
   F=4940-style case) remains open, pending a positive-advantage corpus
   entry — out of scope for this fix.
2. **cr.* BLOCK advantage +2.** Blocked on HIT-trace comparison → Phase 3
   provides it. Diff `Damage_04000` sub-states (`plpdm.c:309-352`) via the
   `rno3`/`cmwk14`/`wcaix` trace columns. Working hypothesis (synthesis
   §13.2): guard-release animation has a ~2-frame tail arcade counts as
   actionable. If confirmed, evaluate fixing the *class* — derive
   defender-idle from actionability rather than the r1 1→0 edge — since
   CnDB R+1 looks like the same convention drift on the attacker side.
   **Phase 3 update (2026-07-07): MOOT — did not reproduce.** Harness
   run under pinned RNG + controlled spacing shows cr.LK/MK/HK/HP BLOCK
   advantage matching `q.json` exactly (`q-crmk-block`, `q-crhk-block`,
   `q-crhp-block`; cr.LK finalizes WHIFF instead of BLOCK for an
   unrelated reason — see the new cr.LK non-classification issue). The
   +2 was likely an artifact of the old manual-capture conditions
   (imprecise spacing/timing, unpinned RNG) rather than an engine bug.
   Downgrade this item's priority; keep the PASS entries as regression
   guards rather than pursuing a fix. cr.MP HIT variance (originally
   grouped with this item) is similarly resolved: gone under pinned RNG
   (`q-crmp-hit-capture-a`/`-b`, byte-identical) — confirms the
   RNG-driven-dummy-guard-logic hypothesis (`com_sub.c:1875`).
3. **Strike-knockdown advantage garbage** (new finding, 2026-07-06 review):
   `fd_is_knockdown_at_atk_idle` (`frame_data_overlay.c:306-310`) only
   detects throw-KD (def r1 ∈ {2,3}); a sweep shows a large green "+N"
   (time-until-wakeup) instead of "KD". Detect the downed state through
   the damage routine and render KD. Add corpus entries (cr.HK hit).
   **Phase 4 update (2026-07-07):** DONE, shipped —
   `fd_is_knockdown_at_atk_idle` extended with the measured strike-KD
   predicate (def r1==1 && r2 ∈ {16,19}, §13.6.2); corpus asserts `kd:1`
   on crhk/backhk/fierce hits (run-failing) + CnDB/throw (xfail-note
   coverage).
4. **CnDB R+1** — **Phase 3 update (2026-07-07): CONFIRMED still exists.**
   Harness (fresh same-run binary, pinned RNG) reproduces R=43/45/47 vs
   `q.json`'s 42/44/46 for LK/MK/HK exactly (`q-cndb-lk-hit`,
   `q-cndb-mk-hit`, `q-cndb-hk-hit`, all xfail). This resolves §13.6.1's
   internal inconsistency in favor of "R+1 is real" — not a stale-binary
   artifact. ~~Tighten the partner-release `attacker_idle` by one frame
   (`frame_data_overlay.c:730-735` area) — throw-specific gate means zero
   UOH regression risk per §13.6.1 — and re-run harness.~~
   **Phase 4 update (2026-07-07): SUPERSEDED — root cause is a mis-sized
   A/R boundary, not a mis-sized window.** Measurement gate (fresh
   same-run binary, `docs/../` scratch tables) confirms `raw_len == arcade
   S+A+R` exactly (56/59/62) on all three CnDB HIT entries and on the
   WHIFF siblings — the partner-release window is already the right size.
   The catch cell collapses mid-tick (declared `cg_ctr=2` elapses only 1
   real frame before the throw segment takes over), so the lost declared
   tick gets double-attributed: `recovery_pf` counts it as recovery while
   displayed A (`effective_a`) already counts it as active. Tightening
   `attacker_idle` by one frame would shrink the window below arcade's
   S+A+R and corrupt T/adv/kd sampling — retracted. Fix instead: re-derive
   R from the engine credit (`meter_len − startup_pf − effective_a`) on
   this path only, gated by an explicit `ended_by_partner_release` flag.
   Formula test against the falsification table (~27 currently-PASS
   entries with `engine_a != active_pf`) confirms the naive/ungated form
   regresses those entries; it is exact only on the partner-release path
   (falsification detail and shipped mechanism duplicated in
   `docs/frame-data-synthesis.md` §13.6.1).
   **DONE — shipped 2026-07-07 on `frame-data-on-mister`** (implementation
   step of the reviewed plan; commit not yet made at time of writing —
   this doc will carry the commit hash once the change lands). `FdMove`
   gained `bool ended_by_partner_release` (set only in the §13.6.1
   tick-side branch, never inferred); `fd_finalize()` re-derives
   `recovery` from `meter_len - startup_pf - effective_a` only when that
   flag is set. Corpus: `q-cndb-lk-hit`/`-mk-hit`/`-hk-hit` xfail removed,
   now plain PASS with `kd: 1` run-failing. Harness: 69 entries, 60
   PASS / 9 XFAIL, exit 0, byte-identical to the pre-fix baseline outside
   the 3 CnDB rows; two consecutive runs byte-identical (determinism);
   mutation C (release-R branch forced off) flags exactly the 3 CnDB
   entries at their old R=43/45/47, restores clean; mutations A/B re-run
   unaffected; MiSTer telemetry cross-build compiles, zero `InputScript`
   symbols in the ARM binary.
5. **Multi-hit T inconsistency** (new finding): with the §13.5.2 override,
   inter-hit gap frames land in neither S, A, nor R, so displayed
   T = S+A+R < real duration. Decide the convention (likely: T is
   move duration, S/A/R are the arcade-canonical figures, so display
   T = raw duration instead of the sum) and make the numeric line honest.
   **Phase 4 update (2026-07-07): convention adopted.** T becomes the
   measured hitstop-free duration (`g_cur.meter_len`, non-throw path);
   S/A/R stay arcade-canonical (R gated per item 4 above). T is
   display/FINAL-only — the checker's `NUMERIC_FIELDS` never include T
   and no corpus entry asserts `expect.T`, so this is a zero-regression
   display change. Confirmed changes: the §3 list in the reviewed plan
   (HSB, UOH, retrigger, and small ±1 hitstop-affected normals) plus
   `q-hsb-jab-whiff` (38→56, multi-hit gaps exist on whiff too, omitted
   from the plan's §3 list but confirmed by the measurement gate).
   **DONE — shipped 2026-07-07 on `frame-data-on-mister`** alongside item
   4 (same `fd_finalize()` edit; `total := g_cur.meter_len` on the
   non-throw path). Display/FINAL-only, zero checker regression (`T` is
   not in `NUMERIC_FIELDS`); the FINAL trace line also gained an
   `endrel=%d` diagnostic token (`(int)g_cur.ended_by_partner_release`).

## Phase 5 — Shipping hygiene before merging toward `mister` (checklist)

None of this is optional for merge; all of it is mechanical:

**Status: DONE (2026-07-07), `/implement` step 8.** Line numbers below are
as of the commits that motivated this checklist; the actual fixes were
re-located by content (several files had moved on since).

- [x] `[CM]` / `[CMX]` stderr logs (`charset.c`, `char_move()` /
      `check_cm_extended_code()`) — gated behind `#if DEBUG` (compile-time;
      CMakeLists.txt only defines `DEBUG` for the Debug config, so Release/
      MiSTer telemetry+clean flavors strip the format strings and calls
      entirely — verified `strings <ARM binary> | grep -c '\[CM\] GT='` == 0)
      **and** a runtime env var `FRAME_CM_LOG` (default off, cached via a
      static in `frame_cm_log_enabled()`) so a plain DEBUG host build
      doesn't spam stderr by default either. Set `FRAME_CM_LOG=1` to
      reproduce the §Phase 4.1 diagnosis stderr-capture workflow in a DEBUG
      build. The three inline `extern u16 Game_timer;` (item below) were
      removed as part of this same edit since `workuser.h` already declares
      it.
- [x] `frame_trace` — `frame_trace_tick()` and `frame_trace_annotate()`
      (`frame_trace.c`) now additionally gate on `Disp_Frame_Data` (in
      addition to `Is_Training_Mode`), so an ordinary training-mode player
      who never toggles the training-menu "FRAME DATA" option pays zero
      trace I/O. `FRAME_TRACE_PATH` env override kept as-is. The harness
      doesn't navigate the real menu to turn the option on; instead
      `TestRunner_Epilogue()` (`test_runner.c`) pins
      `Training[0/2].contents[0][1][6] = 1` every frame for the
      `training-frame-data` preset — the same persisted-config-cell poke
      pattern `input_script_apply_guard_mode()` already uses for
      guard/stance — which `Wait_Pause_in_Tr` (`menu.c`) latches into
      `Disp_Frame_Data` for real at the menu→gameplay transition. Per-row
      `fflush` kept: now gated behind the same opt-in condition, and
      MiSTer's default trace path is under `/tmp`, which is tmpfs
      (RAM-backed, no SD wear — see `docs/plan-stun-direct-p2p.md`'s netplay
      handoff notes for the same fact cited elsewhere in this codebase).
- [x] `frame_data_overlay_tick` (`frame_data_overlay.c`) — added an
      early-out (with the same state reset the `!Is_Training_Mode` branch
      already does) when `Disp_Frame_Data == 0`, mirroring the draw-side
      check (`sc_sub.c:2441`). The engine accumulator hooks in
      `charset.c`'s `char_move()` (`fd_engine_hitbox_active`,
      `fd_engine_active_count`, `fd_prev_active_cgix*`) are deliberately
      **not** gated on `Disp_Frame_Data` — they already run unconditionally
      in every mode (arcade/vs/netplay, not just training) and cost a
      couple of branches plus u8/u16/s16 array writes bounded to at most
      the 2 player WORKs per tick; no loops, no I/O. No measurable hot-path
      cost either way, so left as-is.
- [x] Trailing `NULL` sentinel dropped from `Letter_Data_A3` (`effa3.c`,
      row 6, the training-option submenu) — **documented, not restored**.
      Nothing iterates the array looking for NULL: `effect_A3_move()`
      indexes by an explicit caller-supplied index, and
      `Training_Option()`'s setup loop (`menu.c`, `for (ix = 0; ix < 9;
      ix++)`) plus its cursor bound (`Dummy_Move_Sub(..., 8)`) were bumped
      to match the new 9-entry row in the same commit (`118f7350`) that
      dropped the sentinel — the row is fully dense (9/9 slots) with no
      free slot for one. See the comment added above the array in
      `effa3.c`.
- [x] `SPARSE_CEILING_SLOTS` 82→100 bump (`game_state.h`) — **reverted to
      82**. Confirmed accidental bundle: the bump's own commit
      (`cce9095a`, squashed into `a386e057`) says its motivation was
      "headroom for the in-flight lobby-mvp state additions"; that lobby
      feature was abandoned (`netplay.c:272-273`: "Dormant since the
      RmlUi lobby UI was removed — no code path sets s_lobby_session =
      true anymore"), and no lobby-derived `GameState` fields ever
      landed. The frame-data overlay work the bump got squashed alongside
      only added a 1-byte `Disp_Frame_Data` scalar — doesn't motivate 100
      either. `src/netplay/test_sparse_effect_save.c` references the
      macro symbolically (not the literal 82/100), so nothing hardcodes
      the old ceiling.
- [x] Triple `extern u16 Game_timer;` inline declarations in `charset.c`
      — removed; `workuser.h:461` already declares it and was already
      included by `charset.c`.
- [x] Netplay scope comment — added above the `fd_*` declarations in
      `workuser.c` (right before `fd_engine_hitbox_active`): overlay is
      local-training-only; `fd_*` globals are sim-write-only (nothing
      reads them back into game state or serializes them), so no desync
      risk under rollback, but their values are meaningless mid-rollback
      since re-simulated frames' `char_move()` calls overwrite the same
      slots.

## Phase 6 — Non-Q coverage (after Phases 1–5)

- Fetch per-character JSON from Coccis77/thirdstrikedatabot (same source
  as `q.json`) for the next audit targets: Ryu or Ken (canonical normals +
  a projectile), Hugo (command grabs).
- **Projectile blind spot must be designed for first** (new finding): the
  overlay samples only the player WORK; a fireball is a separate WORK, so
  its active frames are invisible → hadouken would finalize as
  `S=raw A=0 R=0` or contact-frame-only. Decide the display convention
  (arcade tables give fireball S + "total" recovery on the player; active
  belongs to the projectile entity) and extend the accumulator/snap to
  attacker-owned effect WORKs before auditing Ryu.
- Corpus files become per-character (`corpus-ryu.yaml`, …); the harness
  itself shouldn't need changes beyond `--test-p1-character`.

---

## Suggested step sequencing for agents

| Order | Work | Skill | Size | Status |
|---|---|---|---|---|
| 1 | H1 input-script player + H4 trace/exit plumbing | `/implement` | M | **DONE** — `src/test/input_script.c` (.fdi player: W/L/P/G/Q), `--test-input-script`, `FRAME_TRACE_PATH` env override, auto-exit (0 = script completed, 3 = script could not start) |
| 2 | H2 preset + H3 determinism/guard control | `/implement` (H3 discovery first) | M | **DONE** — `training-frame-data` preset (P1=Q/P2=Ken defaults, char/stage overridable), `--test-pin-rng`, `G none\|stand\|crouch` (pokes `Training[0/2].contents[0][0][0..1]` + `control_pl_rno`; guard slot mapping: 1=NO GUARD 2=ALL GUARD, stance 0=STAND 1=CROUCH). Harness runs bypass TrainingConfig load/save. Two consecutive runs verified byte-identical |
| 3 | H5 compiler + checker | `/implement` | M | **DONE** — `tools/frame-data/{compile_corpus.py,check_frame_data.py,run.sh,corpus-smoke.yaml}`. Corpus schema adds `outcome: NONE` (negative controls), `finals: N` (chain entries can XPASS), `qjson.index` (duplicate Names). Motion macros verified live (HCB, charge). `setup.dist` capped at 300 (engine clamps larger teleports to ~320, unrooted). Smoke run: 3/3 PASS in ~17 s |
| 4 | H6 corpus + acceptance run | `/implement` | S–M | **DONE** — `corpus-q.yaml`: 69 entries (56 PASS, 13 xfail, all cited). §1.9 acceptance 2026-07-07: green run exit 0; FINAL lines byte-identical across runs; mutation test (jatix revoke disabled → exit 1, exactly `q-uoh-samef-block`/`q-uoh-1f-block` FAIL A=11≠10, restored → green); wall 149 s (~2.5 min, waits below 90f unsafe); MiSTer telemetry flavor builds, zero `InputScript` symbols in ARM binary. New findings: cr.LK never sets `event_this_frame` (finalizes WHIFF vs guard); throw/HSB/UOH/DLA HIT-R diverges from q.json; UOH retrigger alive (A=20 at 38f gap); cr.MK merge NOT reproducible (recovery outlasts blockstun) |
| 5 | Phase 2 doc truth-up | direct edit | S | **DONE** — synthesis doc truth-up: SHIPPED markers added with commit refs (§13.7.4, §13.7.7 rec #2/#4), git-state note updated, this plan's harness cross-referenced as the standard verification method (§12.0), historical investigation docs folded in (§13.6 CnDB EX-attack/kow note) |
| 6 | Phase 3 re-baseline run + doc/corpus updates | direct | S | **DONE** — 2026-07-07: H6 acceptance run (69 entries, 56 PASS/13 xfail) IS the re-baseline. CnDB R+1 confirmed real (not stale-binary, §13.6.1 resolved); cr.MP HIT variance gone under pinned RNG (was `com_sub.c:1875` RNG-driven); cr.* BLOCK +2 did NOT reproduce (cr.MK/HK/HP exact match, moot pending non-repro); UOH retrigger (A=20) confirmed alive with scripted reproducer; UOH clean A+1 confirmed FIXED (A=10 exact, `a386e057` revoke verified + mutation-tested); cr.MK multi-move merge NOT reproducible (recovery outlasts blockstun by 3f, non-cancelable); new findings: cr.LK never classifies vs guard (WHIFF not BLOCK/HIT), HIT-R divergence family (HSB/DLA-RH/UOH/throw) |
| 7 | Phase 4 fixes, one at a time | `/plan` then `/implement` | M each | **DONE** — 4.1 retrigger (33ba31f1), 4.2 moot (re-baseline), 4.3 strike-KD (ff25162e), 4.4+4.5 combined S/A/R-split + honest T (4702d27c) |
| 8 | Phase 5 hygiene checklist | `/implement` | S | **DONE** — all 7 items (c6a1a3da) |
| 9 | Phase 6 non-Q | `/plan` first (projectiles) | L | **DONE** — projectile arcade-split (468cb84c), tooling+Ryu corpus (4fe2350e), hadouken entries (53dcaf4e), Hugo corpus (6ba03537) |

Each step lands as its own commit on `frame-data-on-mister`. No pushes
without explicit consent. No release artifacts until the user says so.
