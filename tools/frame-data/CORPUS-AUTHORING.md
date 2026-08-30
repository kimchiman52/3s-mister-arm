# Corpus-authoring runbook

Step 10 of `docs/plan-frame-data-completion.md` (Part C, "Corpus-authoring
template"). Distills the empirical lessons burned by `corpus-ryu.yaml` and
`corpus-hugo.yaml` into a repeatable checklist so each of the 17 remaining
characters (Steps 11–13 and beyond) is a bounded task that doesn't re-learn
them from scratch. Every item below cites its precedent — a file, a line
range, or a `docs/frame-data-synthesis.md` section — so you can go verify
the claim yourself rather than trust this document blindly.

**Read before starting:** `corpus-ryu.yaml` and `corpus-hugo.yaml` headers
end-to-end (they ARE the raw material this runbook distills), the schema
docstring + tunables section of `compile_corpus.py`, the verdict-semantics
header of `check_frame_data.py`, `docs/frame-data-synthesis.md` §12.1
(harness limitations register), §13.10 (HIT-R display convention), §13.11
(displayed-A declared-truth convention). If this runbook and one of those
sources ever disagree, the cited source wins — file a correction back into
this runbook (per the parent plan's Step 10 success criteria).

---

## Phase 1 — Oracle fetch

- [ ] **Fetch `<char>.json`** from
      `https://raw.githubusercontent.com/Coccis77/thirdstrikedatabot/master/json/<char>.json`
      (verified 2026-07-08: fetching this URL for `ryu`/`hugo`/`q` and
      hashing the response reproduces the exact md5s, byte-identical to
      the committed files under `docs/arcade-frame-data/`. Ryu and Hugo's
      md5s are pinned in-repo at `corpus-ryu.yaml:4`
      (`cd0f6c79b0735c46e3074f02fb2f5d51`) and `corpus-hugo.yaml:4`
      (`8bd525542b815dd0c228605bffc51523`) respectively. `q.json`'s md5 is
      **not** pinned anywhere in-repo — `corpus-q.yaml`'s header predates
      this md5-pin discipline and `docs/frame-data-synthesis.md:20-21`
      only records the source repo and fetch date, not a hash. Measured
      2026-07-08 against the committed `docs/arcade-frame-data/q.json`:
      `e4251d598ad2b24d755219be1a58b0b2`.) Note: commit
      `4fe2350e`'s message only names the source repo and the md5s, not a
      literal URL — the `json/<char>.json` path was confirmed by listing
      the repo's `json/` directory (`gh api
      repos/Coccis77/thirdstrikedatabot/contents/json`), not by reading
      any in-repo documentation, so re-verify if the upstream repo's
      layout ever changes.
  - [ ] **Character coverage gap:** the source repo's `json/` directory
        (checked 2026-07-08) has entries for akuma, alex, chunli, dudley,
        elena, hugo, ibuki, ken, makoto, necro, oro, q, remy, ryu, sean,
        twelve, urien, yang, yun — **`gill` has no file**. If Gill is ever
        scheduled for a corpus, this fetch step is blocked; escalate to
        the orchestrator rather than fabricating an oracle or reusing
        another character's numbers.
- [ ] **Save** to `docs/arcade-frame-data/<char>.json`, compute its md5
      (`md5 <file>` / `md5sum <file>`), and **record the md5 in the new
      corpus file's header comment** — precedent: `corpus-ryu.yaml:1-4`,
      `corpus-hugo.yaml:1-4`.
- [ ] **Verify the JSON parses.** `compile_corpus.py`'s `load_qjson`
      (`compile_corpus.py:359-368`) will raise on malformed JSON when you
      first compile the corpus, but a manual
      `python3 -c "import json; json.load(open('docs/arcade-frame-data/<char>.json'))"`
      catches it before you've written a single entry.
- [ ] **Verify every targeted move's `Name` exists in the oracle** (and
      disambiguate duplicates). `lookup_qjson` (`compile_corpus.py:409-426`)
      raises a `CompileError` naming the candidate count if a `qjson.name`
      doesn't match or a `qjson.index` is out of range — q.json's two
      "Dashing Head Attack" rows (fast vs. long-charge, `compile_corpus.py:420-425`)
      are the known precedent for needing an explicit `qjson.index`.
  - [ ] **The throw family's `Name` varies per character — don't assume
        `"Throw"`.** ryu.json/q.json use a bare `"Throw"`, but ken.json
        names its three throw rows `"Neutral Throw"` / `"Back Throw"` /
        `"Towards Throw"` (`corpus-ken.yaml:427-431`). Always open the
        fetched oracle and read the exact string for the throw-family rows
        rather than porting the previous character's convention forward —
        a future character may use yet another naming scheme.
- [ ] **Never edit the oracle file.** It is the ground truth every corpus
      is graded against (`compile_corpus.py:352-357`, `docs/frame-data-synthesis.md:22-25`
      "when q.json and the trace disagree, the trace is the suspect").

## Phase 2 — Corpus skeleton

- [ ] **Top-level keys:**
  - `character:` — name (lowercase, matches `CHARACTER_IDS`,
    `compile_corpus.py:36-58`) or int 0-19; resolved by
    `resolve_character` (`compile_corpus.py:371-391`) and threaded through
    to `--test-p1-character` via `meta.json` (`compile_corpus.py:607-610`,
    `run.sh:90-100`).
  - `oracle:` — bare filename under `docs/arcade-frame-data/`, no path
    segments (`resolve_oracle_path`, `compile_corpus.py:394-406`).
  - `inter_entry_wait:` — frames of neutral between entries. Default 120
    (`DEFAULT_INTER_ENTRY_WAIT`, `compile_corpus.py:75`); Ryu uses 90,
    Hugo uses 200 (grabs need a longer settle) — pick per-character, not
    dogmatically the default.
  - `balance:` — **which engine the corpus is measured against** (task
    #108). `"ps2"` (the default, and what all 94 pre-#108 corpora are) or
    `"arcade"`. Resolved by `resolve_balance` in `compile_corpus.py`,
    always written into `meta.json`, and always passed on as
    `--test-balance` by `run.sh` — the engine under test is never implicit.
    An `arcade:` corpus needs a verified CPS3 romset; without one the run
    exits **6** and `run.sh` reports it as a romset problem rather than
    letting the corpus quietly re-measure PS2. Point it at one with
    `FDH_CPS3_ZIP=/path/to/sfiii3nr1.zip` (dev-only; it is exported as
    `$THIRDSARM_CPS3_ZIP`, `src/arcade/arcade_char_data.c:628`).
  - `super_full:` — `true` runs with `--test-p1-super-full`, granting P1
    exactly `store_max` super stock once, at `game_frame <= 2`
    (`tr_spgauge_cont_init2`, `src/test/test_runner.c:993-1009`).
    **An arcade corpus that needs meter must use this, not `sa_gauge:`.**
    The training S.A.GAUGE menu options `sa_gauge:` drives live inside
    `if (!ArcadeBalance_IsEnabled())` in `player_mv_0000`
    (`src/sf33rd/Source/Game/engine/plmain.c:183-204`), so under arcade
    balance the whole `demo_set_sa_full` / `clear_super_arts_point` switch
    is skipped and an `sa_gauge:` corpus gets **no meter at all**.
- [ ] **Standard entry ladder** (plan's own words,
      `docs/plan-frame-data-completion.md:744-747`): **six-button close
      BLOCK → HIT sample → WHIFF sample at 250 → crouching → command
      normals → character specials → negative control.** `corpus-hugo.yaml`
      is the minimal shape template (normals BLOCK/HIT/WHIFF sample +
      crouching + one command normal + negative control, then its grab
      families appended after); `corpus-ryu.yaml` shows the same skeleton
      extended with UOH/Throw/Hadouken sections between crouching-normals
      and the negative control — mirror whichever shape fits the
      character's move set, but keep the negative control LAST.
- [ ] **Label convention:** `<char-prefix>-<move>-<outcome>`, e.g.
      `ryu-jab-block`, `h-strong-hit`. Must match `LABEL_RE`
      (`compile_corpus.py:143`, letters/digits/`.`/`_`/`-` only — no `#`,
      no whitespace) and be ≤ 63 chars (`LABEL_MAX_LEN`,
      `compile_corpus.py:144`, the C-side `input_script.c:20` hard limit).
- [ ] **`setup.dist`:** `"close"` (56px), `"far"` (175px), or an explicit
      int ≤ `DIST_MAX` (300px, `compile_corpus.py:95`) — a larger value is
      a **compile-time error**, not a silent clamp
      (`compile_corpus.py:84-91` CAVEAT). Some moves' real reach doesn't
      match the "close"/"far" convention — binary-search the actual
      connecting distance rather than assuming (Ryu's Far Jab: dist=100
      whiffs, dist=90 is the closest-to-convention value that connects,
      `corpus-ryu.yaml:103-113`).
  - [ ] **For throws specifically, `"close"` (56px) can exceed the
        character's own throw range and silently no-op.** Ken's
        `Throw_range` oracle field is 24px — `dist:close` sits outside it
        and produced a "no FINAL line" read that first looked like a
        label-bleed trap, not a range problem (`corpus-ken.yaml:433-436`,
        `21-28`). Use an explicit `dist:<Throw_range>` (verify the field's
        value in the fetched oracle) for any throw-family entry, same as
        `corpus-ryu.yaml`'s own `ryu-throw-whiff`/`-hit` precedent.
- [ ] **`setup.dummy`:** `none` / `stand` / `crouch` (`GUARD_MODES`,
      `compile_corpus.py:123`).
- [ ] **`outcome`:** `HIT` / `BLOCK` / `WHIFF` / `NONE` (`OUTCOMES`,
      `compile_corpus.py:127`). `NONE` is the negative control — expects
      **zero** FINAL lines in the window, and rejects any `expect.<field>`
      other than `xfail` (`validate_entry_shape`,
      `compile_corpus.py:504-515`). Precedent: `ryu-jump-none` /
      `h-jump-none`, both `press UP` at dist 250 vs a standing dummy.

## Phase 3 — Empirical pinning discipline

- [ ] **Every `expect` is verified against a live trace before commit —
      never guessed from the oracle JSON alone.** Stated discipline in
      both existing files (`corpus-ryu.yaml:11-13`,
      `corpus-hugo.yaml:10-12`).
  - **Mechanical trace→expect loop:** run
    `tools/frame-data/run.sh tools/frame-data/corpus-<char>.yaml`; the
    script echoes `[run.sh] RUNDIR=<path>` (`run.sh:85`) before it builds
    and runs. Open `$RUNDIR/trace.log` (this is `FRAME_TRACE_PATH`,
    `run.sh:98`) and grep for your entry's label to find its window, then
    read the **FINAL** line(s) in that window for the measured S/A/R/adv.
    Write those measured values into `expect.<field>` (or into
    `expect.xfail` per the divergence rules below) in the corpus YAML, then
    re-run `run.sh` and confirm the entry now reports PASS (or XFAIL, if
    that's what you pinned).
- [ ] **Use `from-qjson` wherever the oracle field is numeric.**
      `resolve_from_qjson` (`compile_corpus.py:429-448`) maps `S`→`Startup`,
      `A`→`Hit`, `R`→`Recovery`, `adv`→`Block_advantage`/`Hit_advantage`
      (outcome-gated — `adv: from-qjson` is a `CompileError` on `WHIFF`,
      since q.json has no whiff-advantage field). It raises loudly if the
      field is non-numeric (a range string like `"21 ~ 24"`, or `"-"`) —
      that's the signal to switch to an explicit literal.
- [ ] **Explicit literal + comment for range-string/empty fields.**
      Precedents: Meat Squasher's `Startup` is a range per strength
      (`"21 ~ 24"` etc.) — `S: 24` is authored as an explicit int with the
      range and the distance-dependence documented in the comment
      (`corpus-hugo.yaml:404-421`); DHA's long-charge variant is a similar
      shape (§12.1 register row 3, only the fast variant's `S=13/15/19` is
      reachable and asserted).
- [ ] **`kd` (knockdown flag) is explicit-only, never `from-qjson`.**
      `compile_entry` rejects `expect.kd: from-qjson` outright
      (`compile_corpus.py:539-548`) — q.json's non-numeric
      `"Hit_advantage": "D"` is not auto-mapped to `kd=1`; author the 0/1
      by hand after observing it live.
- [ ] **Divergent entries become `xfail`, not a rewritten `expect`.**
      `expect.<field>` keeps the **true arcade value** (via `from-qjson` or
      an explicit literal); `expect.xfail: "<reason>"` carries the
      measured/wrong value and the mechanism. "The wrong engine number is
      never substituted in as if it were now expected"
      (`corpus-ryu.yaml:63-68`). Classify every divergence into an
      **existing** family (Phase 6 below) before writing a new one.
      **Convention carve-out (2026-07-10):** adopted conventions
      (§13.10 / §13.11 / §13.13, `docs/frame-data-synthesis.md`) are the
      sanctioned exception — for an entry that is an established member of
      one of those conventions, `expect.<field>` instead asserts the
      **measured engine-truth value** as a normative plain `PASS`, citing
      the section; the arcade (whiff-canonical) value moves to the entry
      comment. This is not "substituting in the wrong number" — the
      convention's whole point is that the measured value IS correct for
      the path actually taken. See Phase 6 buckets 1/7/8 below for exactly
      which shapes qualify.
- [ ] **Know the verdict semantics before reading your own results**
      (`check_frame_data.py:1-27`, `FAILING_VERDICTS` at line 197):
      `PASS`/`XFAIL` are green; `FAIL` (unexpected mismatch), `XPASS`
      (an `xfail` entry now matches — the bug is fixed, remove the xfail),
      `SHAPE` (outcome flipped between HIT/BLOCK and WHIFF —
      `is_shape_mismatch`, `check_frame_data.py:109-114`), and `NO-DATA`
      (no FINAL line at all in the window) are all failing and make
      `run.sh` exit nonzero.

## Phase 4 — Mandatory whiff-vs-table verification (user requirement, 2026-07-07)

Every audited move gets its **WHIFF** (or nearest table-comparable path —
e.g. a command grab's WHIFF baseline, since a connected grab has no
comparable BLOCK) measured against the oracle, in addition to its
BLOCK/HIT rows.

- [ ] **This applies to command normals too, even though Phase 2's ladder
      text doesn't call them out explicitly.** Neither `corpus-ryu.yaml`
      nor `corpus-hugo.yaml` authored an explicit WHIFF row for their own
      Towards+button command-normal entries — but Phase 4's rule is
      unconditional ("every audited move"), and skipping it is exactly
      what would have hidden Ken's Towards Roundhouse R-deficit: adding the
      WHIFF row (`ken-twdshk-whiff`, `corpus-ken.yaml:407-412`) is what
      surfaced the divergence, by confirming R=18 on a clean whiff against
      the R=14 measured on BLOCK. Don't skip WHIFF for a command normal
      just because the ladder text's example list didn't spell it out.
- [ ] **Expectation: WHIFF S/A/R matches the oracle exactly for nearly
      every move.** This is not incidental — it's the reason WHIFF is
      the calibration surface: the contact-tick mechanisms behind nearly
      every known divergence family (§13.11 shape (a)/(b), §13.10 classes
      1/3) only fire on contact, never on a clean whiff. Precedent:
      `corpus-ryu.yaml:215-223` ("All six WHIFF entries below match
      arcade exactly, including for the same buttons... whose BLOCK/HIT
      entries above were xfailed"), `corpus-hugo.yaml:243-248` (Roundhouse
      WHIFF exact despite its BLOCK/HIT R-side xfail), §13.10 Class 3's
      whiff table (`docs/frame-data-synthesis.md:3950-3956`, all four
      grabs' WHIFF S/A/R exact).
- [ ] **A whiff-A divergence triggers the UOH-class check** (declared-vs-
      whiff-visible truncation, §13.11): if the gap is explained by
      declared credit ≠ whiff-visible ticks (the same "two
      cancelling-minus-ones" shape as UOH, §13.11 lines 4062-4078), the
      entry **asserts the declared-truth value** (from-qjson if numeric,
      else explicit) **citing §13.11** — this is a plain PASS under the
      convention, **not an xfail**. Precedent: `q-uoh-whiff` in
      `corpus-q.yaml` asserts A=11 (the engine's declared credit), not
      q.json's whiff-visible A=10.
- [ ] **A whiff-R truncation divergence cites RESIDUAL item 18(b)**
      (the §13.11 adjacent-divergence paragraph in
      `docs/frame-data-synthesis.md`, refined by the 2026-07-08 item-18
      split — see the rewritten item 18 and §13.5.1a) — a GROUNDED
      fresh-edge early-cut: the move plays a genuine grounded cleanup
      chart (grounded reset, real post-reset label, grounded flip into
      cghi=1) whose cut point precedes the oracle's actionable point.
      Still open. This one **does stay `xfail`** (no declared-truth
      convention covers R): precedent `q-uoh-whiff-r`, which asserts the
      arcade R=5 as a documented xfail against the measured 3, specifically
      so the divergence stays visible to the harness rather than being
      silently dropped.
      **Re-scoped 2026-07-08 (§13.5.1a):** AIRBORNE/LANDING R-truncations
      (the move leaves the ground and its chart flips to cghi=1 on or
      across the landing tick, or the chart sits at cghi=1 when a mid-anim
      cgix dip fires) were §13.5.1 false cuts, FIXED by the grounded
      fresh-edge gate — those now measure CLEAN (arcade-exact R). Any new
      one that doesn't measure clean is a real finding, not this bucket.
- [ ] **An unexplained whiff divergence is a STOP, not an xfail.** Whiff
      is the calibration surface for everything else in this runbook — if
      whiff itself is wrong, every BLOCK/HIT convention value built on top
      of it (§13.10, §13.11) is suspect. Do not write
      `expect.xfail: "unexplained"` and move on; halt, report the finding,
      and get orchestrator/user input before continuing that character's
      corpus (same posture as `feedback-no-shipping-wrong-data.md` and the
      plan's own house rule against accepting wrong data,
      `docs/plan-frame-data-completion.md:209-211`).

## Phase 4a — Arcade-balance corpora (task #108)

A `balance: arcade` corpus measures the engine that actually ships: arcade
balance auto-selects the moment a CPS3 romset verifies
(`src/arcade/arcade_balance.c`), which is confirmed on hardware. Things that
are true on the PS2 path and NOT on the arcade path, learned the hard way:

- [ ] **`sa_gauge:` is inert.** See the `super_full:` note in Phase 2. Use
      `super_full: true`.
- [ ] **Dummy BLOCK is not trustworthy yet.** `check_illegal_lever_data()`
      lever normalization is PS2-only (`plmain.c:52-55`). A `dummy: stand`
      entry that BLOCKs under PS2 was observed to **HIT** under arcade
      (`corpus-smoke.yaml`'s `close-lp-block-vs-stand`, measured this
      session). That divergence is unexplained, so per Phase 6 it is a
      **STOP, not an xfail** — do not author BLOCK entries into an arcade
      corpus until it is understood. Tracked in `docs/queue.md` under #108.
- [ ] **The CPS3 super-art state machines are only reachable here.**
      `sag_union` dispatches on `sa->gauge_type` under arcade balance
      (`plmain.c:1042-1050`); `sag_union_0/1/3` (`:642/:691/:790`) cannot
      execute at all on the PS2 path. `sag_union_1` (gauge_type 1) is the
      install-super machine that carried the unbounded-stock bug upstream
      `ad411df5` fixed. `corpus-yun-sa3-arcade.yaml` is the worked example,
      including its re-runnable RED proof.

      Measured census (temporary probe inside `sag_union`'s arcade branch,
      2026-08-30): exactly seven (character, super-art) pairs dispatch to
      `sag_union_1`, all with `store_max=1` — **yun SA3, oro SA1, oro SA3,
      yang SA3, makoto SA3, q SA3, twelve SA3**. Everything else measured
      landed on gauge_type 0. Those seven are where an install-stock
      assertion can go.
- [ ] **`store` is not a traced field**, so a corpus cannot assert super
      stock directly. Make stock observable through *what the input
      produces*: with stock, the super; without it, the underlying special.
      Pin the fall-through move against its own oracle row so the entry is a
      real frame-data assertion, not merely "something else happened".

## Phase 5 — Known traps

- [ ] **Isolated single-entry re-verification for anything suspicious.**
      Hugo label-bleed lesson: a 360-shape move that doesn't finalize
      within its own SCRIPT window leaves `g_cur.active` latched TRUE,
      blocking the next entry's MOVE_START — the eventual FINAL gets
      silently misattributed to whatever label is current when it fires.
      Every number in `corpus-hugo.yaml` was re-verified in an isolated
      single-entry corpus (large `inter_entry_wait`, nothing after it)
      before being trusted (`corpus-hugo.yaml:77-86`).
- [ ] **Predecessor-outcome ordering** (§12.1 row 1): teleporting via `P`
      immediately after a HIT command-grab silently no-ops if the
      defender still has residual knockdown-recovery ticks pending.
      Workaround: order a WHIFF predecessor immediately before any
      distance-sensitive entry needing a clean teleport — a WHIFF leaves
      no knockdown residue. Precedent: `corpus-hugo.yaml:439-450`
      (`h-meatsquasher-short-whiff` ordering comment).
- [ ] **Teleport clamps / momentum bleed after connecting dash specials**
      (§12.1 row 2): the entry immediately after an always-connecting dash
      special gets a corrupted P1 teleport that only partially
      re-converges over several following entries. Workaround: order
      distance-sensitive WHIFF entries **before** always-connecting HIT
      entries in the same section (`corpus-q.yaml` ORDERING NOTE, cited in
      §12.1 row 2).
- [ ] **Motion-probe method:** S-value matching against the oracle is the
      ground truth signal for "did I land the right move," same
      methodology as the repo's existing kow=24/26/28 reinvestigation
      (§13.6 item 5). Try existing macros first — `press`, `press1f`,
      `hold`, `wait`, `motion {qcf,qcb,hcb,hcf,2qcf}`, `motion
      charge-back/charge-down` (`compile_corpus.py:193-289`). If none
      land the move, try a `hold`-chain (already-expressible, no compiler
      change) before concluding a new macro is needed — Hugo's two
      360-degree command grabs were landed this way (a cardinal-4 `hold`
      rotation, `corpus-hugo.yaml:48-95`) after `hcb`/`hcf` produced no
      signal or an unidentifiable move. **Extend `_motion_steps`
      (`compile_corpus.py:224-237`) ONLY if no expressible sequence lands
      the move** — neither Ryu nor Hugo needed this; both shipped with
      zero `compile_corpus.py` changes.
- [ ] **Named buffer-entry ordering convention** (EXSUPER-1 closing cleanup,
      2026-07-13; generalizes the "Teleport clamps / momentum bleed" trap
      above once it had 3+ independent sightings). A `kd=1` HIT leg (or any
      other momentum-bleeding, always-connecting leg) run immediately
      before another entry corrupts THAT NEXT entry's own teleport — NOT
      fixable by raising `inter_entry_wait` (verified up to 1200-1500
      frames, 3x the normal value, byte-identical corrupted result both
      times) and NOT fixable by changing the next entry's own requested
      distance (same corruption regardless of distance-delta). This is a
      position/teleport-state corruption, not a settling-time shortfall.
      **Fix:** interleave a neutral buffer entry between the two
      momentum-bleeding legs — either a whole unrelated whiff move's own
      section (`corpus-twelve-ex.yaml`'s N.D.L. (EX) precedent) or a single
      throwaway whiff normal from a DIFFERENT move family (`press LP`,
      `corpus-twelve-sa1.yaml`'s `twelve-sa1-buffer-jab`,
      `corpus-twelve-sa2.yaml`'s `twelve-sa2-buffer-jab-{1,2}`) — verified
      to fully clear the residue, both legs then measuring their TRUE
      isolated values byte-identically. A buffer entry asserts nothing of
      its own (the move it borrows is already covered elsewhere) and is not
      part of its host file's own scope. **3+ sightings, named per this
      convention:** `corpus-twelve-ex.yaml` (N.D.L. (EX)), `corpus-
      twelve-sa1.yaml` (X.N.D.L., same trap X.F.L.A.T. hits in the sibling
      SA2 corpus), `corpus-twelve-sa2.yaml` (X.F.L.A.T.), and a
      **second-order form** on `corpus-alex-sa3.yaml` (Alex SA3, `<sp>/
      sa-alex/run-probe13-reorder/trace.log`): chaining two large-teleport
      HIT/`kd=1` legs back-to-back corrupts the SECOND one regardless of
      distance-delta or wait, and the fix there is to insert the buffer
      (a genuine WHIFF leg of the SAME move, leaving no knockdown residue)
      BETWEEN the two HIT legs, not merely before both of them — same root
      cause (momentum/knockdown residue bleeding into the next teleport),
      one entry-position further downstream than the simple "buffer before
      the connecting leg" shape.
- [ ] **MOTION-NOT-FOUND closures are incomplete until air recipes are
      tried** (Ibuki SA1 lesson, EXSUPER-1 closing cleanup, 2026-07-13).
      `corpus-ibuki-sa1.yaml`'s own header records this directly: Kasumi
      Suzaku's initial 32-candidate GROUND motion sweep found nothing and
      was closed as MOTION NOT FOUND — but the move is an AIR-ONLY
      activation, and the air-input idiom (`press UP; wait 3; motion 2qcf
      <btn>`) landed it on the FIRST air recipe tried. This is the same
      class of documentation-gap trap as Kunai (EX)'s own AIR_DEFERRED
      reclassification. **Do not close a super/EX slot as MOTION NOT FOUND
      on a ground-only sweep** — try at least one air-input recipe before
      recording a negative result as final.
- [ ] **Rest of the §12.1 register**, inline for quick reference (full
      table + evidence citations: `docs/frame-data-synthesis.md:608-615`):
  - DHA long-charge variant recipe unknown — document the gap, don't
    guess the input (wants-follow-up).
  - SDB HIT/BLOCK unreachable — the harness dummy can't be scripted to
    jump; WHIFF is the only assertable outcome for anti-air-only grabs
    (accepted).
  - Fireball WHIFF unreachable (a projectile travels full-screen and
    still connects at dist=300); hadouken block-adv plateau boundary
    121–149 unscanned (optional follow-up, not a blocker since `close`
    sits well inside the exact-match plateau).
  - Merged-move projectile residual — not constructible in current
    corpora; one unscanned precondition pair remains (wants-follow-up).

## Phase 6 — Post-convention divergence-classification decision tree

Classify every divergence into one of these **before** inventing a new
family. This tree reflects the state **after** the 2026-07-07 §13.11
lever-F convention landed — several shapes that used to be "new findings"
should no longer occur at all; their reappearance is a regression signal,
not a fresh audit result.

1. **Contact-A undercount** (measured A < oracle A, S/R/adv exact) →
   check shape (a), `docs/frame-data-synthesis.md:4103-4140`:
   - **(a1) skip-jump** — the contact tick jumps `cg_ix` past one or more
     declared-active cells entirely, so their credit never enters
     `fd_engine_active_count`. **UPDATE (2026-07-10, §13.13):** assert the
     measured engine-truth A citing §13.13, plain `PASS`, with the arcade A
     in the entry comment — this shape's A is unrecoverable by any
     accumulator fix, but §13.13 makes it the normative displayed value.
     Precedent (converted): `ryu-far-strong-block`, `ryu-crmk-block`,
     `h-short-block`/`-hit`, `h-forward-block`/`-hit`.
   - **(a2) char_move-bypass** — `hit_pattern_extdat_check` case `0x41`
     (`hitcheck.c:743-752`) parks `cg_ix` past a declared cell without
     calling `char_move` at all. **UPDATE (2026-07-10, §13.13):** same
     disposition as (a1) — assert the measured A citing §13.13. Precedent
     (converted): `ryu-twdshp-block`.
   - **Shape-(b) STOP rule is unchanged** by §13.13 — shape (b) should
     still NEVER occur (see below); a shape-(b)-looking undercount is
     still a lever-F regression signal, not a §13.13 member. §13.13 covers
     ONLY the unhedged, lever-F-tested-UNCHANGED shape-(a1)/(a2) sightings
     above — a per-entry lever-F toggle test (or an explicit skip/bypass
     trace citation) is still required before citing §13.13, exactly as
     it was required before citing §13.11's unrecoverability statement.
   - **Shape (b) should NO LONGER occur.** Lever F
     (`fd_restore_revoked_declared_credit`, `charset.c`) now restores the
     §13.7.4 revoke's subtraction, so same-jatix same-tick transits
     display their full declared A as a plain **PASS** (11 Ryu entries
     flipped this way 2026-07-07, flip table at
     `docs/frame-data-synthesis.md:4153-4161`). **If a shape-(b)-looking
     undercount appears on a new character, that is a lever-F regression
     signal — STOP and report it, do not xfail it as "a new instance of
     issue #17."**
   - **Practical shape-(a)-vs-(b) discriminator: toggle lever F itself.**
     Rather than reasoning about which shape a new undercount is from the
     trace alone, force `fd_restore_revoked_declared_credit` to `0` on a
     scratch build (`charset.c:507`), rerun the single suspect entry in
     isolation, and compare A. If A is **unchanged** by disabling the
     restore, no revoke fired for that entry — it's shape (a) (skip-jump,
     unrecoverable), not shape (b). If A **changes** (drops further), the
     restore was actively doing something for that entry, which is itself
     the lever-F regression signal above. Always revert the scratch build
     (`git checkout`) and re-verify green on the clean tree before
     committing, per Phase 8's same-binary protocol. Precedent: every
     Ken (a1) entry in `corpus-ken.yaml` (`ken-far-strong-block`,
     `ken-roundhouse-block`/`-hit`, `ken-crforward-block`) was lever-F
     toggle-tested this way and confirmed unchanged.
2. **HIT-R divergence** (S/A/adv exact, R differs from the BLOCK/WHIFF
   figure) → §13.10's three classes
   (`docs/frame-data-synthesis.md:3857-4034`), convention value asserted
   in all three (cite §13.10, keep the whiff/block-canonical arcade R in
   the comment):
   - **Class 1 — hit-branch recovery chains.** The attacker's own
     animation plays a genuinely shorter parallel recovery chain on HIT
     (not a defender-hitstun effect). Precedent: HSB Jab/Strong/Fierce,
     DLA RH, Hugo Strong, Hugo Forward (R clause only — its A clause is
     the unrelated shape-(a1) family above).
   - **Class 2 — §13.7.3 cut-window compression.** UOH-specific; a real
     defender hitstun-vs-blockstun differential shortens the §13.5.1 cut
     window on HIT. Cross-reference §13.7.3, don't reinvent it.
   - **Class 3 — connected grabs.** Arcade R is the **whiff-path**
     figure (verify this against the entry's own WHIFF row per Phase 4);
     HIT R measures the real post-capture throw sequence, bounded by
     whichever end-mechanism fires first (`cut=1`, `endrel=1`, or
     `FD_METER_LEN` saturation). **For saturated grabs** (check `T` in the
     trace — Hugo's grabs hit `T=71`; Q/Ryu Throw do not saturate), the
     formula `R = (FD_METER_LEN-1) - S - A` (`FD_METER_LEN=72`,
     `frame_data_overlay.c:42`) is a **derived** value: re-derive it if
     `FD_METER_LEN` ever changes, don't re-measure from scratch
     (`docs/frame-data-synthesis.md:3982-3995`).
   - **Compliance note (2026-07-10):** a number of Class-1 HIT-R xfails
     authored during the Phase-6 cast rollout were left `xfail` instead of
     asserting the convention value, contrary to this bucket's own "cite
     §13.10, convention value asserted in all three" rule. These were
     brought into compliance 2026-07-10 (tabulated separately as "F2b" in
     the `CONV-C13` tracker row, `docs/plan-frame-data-completion.md`) —
     they cite `§13.10 HIT-R convention` directly (not `§13.13`, since this
     is compliance with an already-adopted convention, not new scope).
     Precedent (converted): `sean-uoh-hit`, `twelve-roundhouse-hit`,
     `twelve-axe-strong/-fierce-hit`, `urien-crfierce-hit`,
     `urien-chariot-mk-hit`, `dudley-jetup-mp-hit`, `dudley-mgb-mp/hp-hit`
     (full converts); `dudley-jetup-lp/hp-hit` (R clause converts, stacked
     with an EXCLUDED §12.2.3-candidate A clause that stays xfail,
     narrowed).
3. **cr.LK-style outcome misses** (a crouching light kick misclassifying
   BLOCK as WHIFF against a guarding dummy) → issue #14, **fixed**
   (`dm_stop != 0` event edge widened). Should no longer occur; reappearance
   on a new character is a regression — STOP, don't file it as a new
   finding.
4. **R-side contact surplus on a landing-clocked move** (A exact, R reads
   high on both BLOCK and HIT, WHIFF exact) → the Hugo-Roundhouse family
   (`cg_extdat=0x80` same-tick contact advance, `hitcheck.c:727-729`, on a
   move whose end is pinned by the outcome-independent
   `jumping_union_process`, `pls01.c:767`, rather than the chain). `xfail`
   citing §13.11's Hugo Roundhouse addendum. Precedent:
   `h-roundhouse-block`/`h-roundhouse-hit`.
   **UPDATE (2026-07-10, CONV-2):** the user decision has now been taken
   - this whole bucket converts to a plain `PASS` citing §13.13's new
   family F5 (SCOPE EXTENSION block, `docs/frame-data-synthesis.md`
   §13.13). Precedent (converted): `h-roundhouse-block`/`-hit` (R=30) and
   the sweep-2 item-4 join `ibuki-twdsroundhouse-block` (R=22) - all
   three FULL, `xfail` removed entirely.
5. **Whiff-R truncation on a GROUNDED cleanup chart** → residual item
   18(b) (Phase 4 above); `xfail` citing the §13.11 adjacent-divergence
   paragraph in `docs/frame-data-synthesis.md` (as refined by the
   2026-07-08 item-18 split). Precedent: `q-uoh-whiff-r`.
   **Re-scoped 2026-07-08 (§13.5.1a):** airborne/landing R-truncations
   are NOT this bucket anymore — they were §13.5.1 false cuts, fixed by
   the grounded fresh-edge gate, and should now measure CLEAN. A new
   airborne/landing R-truncation that does not measure clean is a real
   finding — investigate, don't bucket-5 it.
   **Re-scoped again 2026-07-09 (§13.5.1b guard-rearm gate, lever H):** a
   GROUNDED cleanup cut is not automatically genuine either — check
   `guard_flag` across the post-anchor tail (trace column `gflg`) before
   citing this bucket. If `guard_flag` stays 3 for the whole tail
   (reaching 0 only on the natural `r1: 4→0` edge), that was the ENGINE-2/
   lever-H false-cut shape, now FIXED, and the entry should measure CLEAN
   (or land on a *different*, smaller residual) — see
   `docs/frame-data-synthesis.md` §13.5.1b for the disposition of every
   member this reclassified: `ken-tatsu-lk-whiff`, `akuma-tatsu-lk-whiff`,
   `chunli-hk-whiff`, `chunli-hazan-lk-whiff`, `yun-twdsmk-whiff` +
   `yang-twdsmk-whiff` now PASS; `chunli-farroundhouse-block` and
   `ken-tatsu-lk-block`/`-hit` resolve their truncation but keep their own
   ±1 contact-variance; `chunli-hazan-lk-block`/`-hit` resolve their
   truncation but keep an unrelated shape-(a1) A-undercount. A GENUINE
   item-18(b) member re-arms `guard_flag` at anchor+0/+1 (a TRUE cleanup
   tail) — `q-uoh-whiff-r`, `oro-uoh-whiff`, `akuma-uoh-whiff`,
   `dudley-uoh-block`, and Urien's UOH R clause were checked and confirmed
   genuine; they stay in this bucket, unaffected. A newly-discovered
   whiff-R truncation whose `gflg` stays 3 the whole tail is a lever-H
   regression signal — STOP and report it, don't bucket-5 it.
6. **UOH three-way A/R shape (BLOCK A-surplus, R triplet 3/2/2, all legs
   `cut=1` w/ `anchor_a=engine_a`)** → the UOH landing-clocked active
   tail, `docs/frame-data-synthesis.md` §12.2.1 — mechanism established
   2026-07-09 (credit-ledger traces on necro + alex). UOH's active phase
   ends at LANDING: on BLOCK the guard freeze is shorter than the landing
   delay, so extra real active ticks (sentinel shape) or a loop-back
   re-entered declared cell (alex shape) accrue extra A; HIT's freeze
   equals its landing delay, so HIT = WHIFF. The whiff R leg is residual
   item 18(b); contact legs read one tick lower. `xfail` citing §12.2.1.
   Precedent: `alex-uoh-*`, `elena-uoh-*`, `necro-uoh-*`, `twelve-uoh-*`.
   (Dudley/Chun-Li's outcome-INdependent A=14 stays §13.11; Urien's
   A-undercount stays the self-loop blind spot; Yun/Remy/Yang R legs stay
   item 18(c).)
   **UPDATE (2026-07-10, CONV-2):** the user decision has now been taken
   - contact-leg A clauses convert to measured engine truth citing
   §13.13's new family F4: `alex-uoh-block` (A=15), `alex-uoh-hit`
   (A=12), `elena-uoh-block` (A=12), `necro-uoh-block` (A=12),
   `twelve-uoh-block` (A=12) - all PARTIAL, the R-triplet clause stays
   `xfail` (18(b)-cut, meter-suspect). `alex-uoh-whiff` stays fully
   `xfail` (WHIFF, exclusion 1 outranks proof). `elena-uoh-hit`/
   `necro-uoh-hit`/`twelve-uoh-hit` stay fully `xfail` untouched (A
   already arcade-exact on HIT). §12.2.1's own R-triplet whiff-exact
   variant - `chunli-uoh-block`/`-hit` - also converts (R=4, family F6),
   both entries now plain `PASS`.
7. **Contact-only R-deficit with S/A/adv exact and WHIFF exact (where
   reachable)** → §13.10 Class 1 generalized-to-contact, via the
   contact-branch recovery-shortening mechanism
   (`docs/frame-data-synthesis.md` §12.2.2, trace-proven 2026-07-09): on
   contact the chart branches into (or skips into) a genuinely shorter
   parallel recovery chain; the deficit equals the branch-length
   differential / skipped cells' declared ctr, and arcade's own
   live-measured `adv` embeds the short recovery while its R column is
   whiff-canonical. **UPDATE (2026-07-10, §13.13):** established members
   assert the measured R citing §13.13, plain `PASS`. Precedent
   (converted): `ken-twdshk-block` (branch variant), `akuma-twdsmp-block`
   (skip variant), `akuma-roundhouse-block/-hit`,
   `chunli-forward-block/-hit` (+ `chunli-crfierce-*`, the original
   Class-1-generalized precedent). A new sighting needs the same-tick
   recovery skip/branch visible in its trace before citing this bucket —
   otherwise it is only a §12.2.2 *candidate*, and candidates NEVER
   convert (e.g. `elena-crroundhouse-*`, `ibuki-raida-lp-block`,
   `ibuki-dtwdsforward-block`, `twelve-crstrong-block`'s R clause,
   `dudley-crforward`/`-crroundhouse`'s R clauses, `yun-farfierce`/
   `-roundhouse`'s R clauses — the last four are registry-absent,
   precedent-cite-only sightings that fail the "own recorded skip/branch
   signature" bar and stay xfail, re-worded as unregistered candidates).
   **SWEEP-2 UPDATE (2026-07-10, classification sweep #2):** every
   candidate example named in the previous sentence was subsequently
   promoted — their skip/branch signatures were recorded by direct
   measurement (P2 per-tick streams / rule-transfer recounts under the
   window's corrected counting rule; per-entry citations in each corpus
   comment) — and they are now converted precedents of this bucket:
   `elena-crroundhouse-block/-hit` (R 25), `ibuki-raida-lp-block` (R 17),
   `dudley-crroundhouse-block/-hit` (R 15), `yun-farfierce-block` (R 21),
   `yun-roundhouse-block/-hit` (R 21), plus new R-deficit members
   `sean-farroundhouse-block/-hit` (R 25), `sean-twdshp-block` (R 22),
   `twelve-crfierce-hit`'s R clause (R 13). Additionally the R-SURPLUS
   direction (§12.2.3's corollary, now CLAIMED — synthesis §12.2.2
   SWEEP-2 update) has converted precedents: `alex-fierce-block/-hit`
   (R 17), `chunli-farroundhouse-block` (R 16), `twelve-crstrong-block`
   (R 15), `ibuki-dtwdsforward-block` (R 18), `urien-chariot-mk-block`
   (R 27), `dudley-crforward-block/-hit` (R 25), `ibuki-tsumuji-lk-block`
   (R 21). The "candidates NEVER convert" rule itself is unchanged — a
   new sighting still needs its own recorded signature; what changed is
   that these specific sightings now HAVE one. NOT promoted:
   `yun-crfierce-block`'s R clause — measured NOT branch content (whiff
   gap-tick survival; see §12.2.4's contact-skip R-window
   reclassification class), stays xfail.
   **DATED CORRECTION (2026-07-10, CONV-2):** the line immediately above
   is now FALSE - the user decision has been taken for MEASURED members
   of this class; `yun-crfierce-block`'s R clause converts to R=16
   (§13.13 family F7), and the entry is now a plain `PASS` (its A clause
   already converted under F1 in an earlier cycle). The historical line
   above is preserved verbatim per repo practice (dated append, not
   silent rewrite).
8. **Contact-only A-OVERCOUNT with S/adv exact (the "mirror of #17")** →
   contact-branch declared-credit A-overcount
   (`docs/frame-data-synthesis.md` §12.2.3, trace-proven 2026-07-09 on
   `ken-forward-block/-hit` by credit ledger): the same-tick contact
   advance re-routes the chart onto an outcome-specific branch whose
   EXTRA declared-active cells each bank their declared ctr (§13.11),
   while the interrupted cell keeps its banked credit. Different wrong
   values per outcome = different branch cells. Sign-coherent with #17:
   (a1) branches skip declared-active cells, this bucket's branches
   insert them. **UPDATE (2026-07-10, §13.13):** the established member
   (`ken-forward-block/-hit`) asserts the measured A citing §13.13, plain
   `PASS` — this is the ONLY §13.13 F3 member pair; every other
   signature-matched sighting stays a candidate. A new sighting needs its
   own ledger trace before citing the mechanism as established for that
   move — otherwise it is a §12.2.3 *candidate* (e.g. `sean-ryuubi-*`,
   `dudley-jetup-*`, `twelve-crfierce/-crroundhouse`, `remy-rrf-*` A+1),
   and candidates NEVER convert under §13.13.
   **SWEEP-2 UPDATE (2026-07-10, classification sweep #2):** every
   candidate example named in the previous sentence has since met the
   own-ledger bar (per-entry `[P1ledger]` reconciliation,
   `s3-rundir/run-{sean,remy,twelve,dudley}/run.log`; synthesis §12.2.3
   SWEEP-2 F3 member-set amendment) and converted: `sean-ryuubi-lk/mk-
   block/-hit` + `-hk-hit` (A 10; `-hk-block` PARTIAL, A converts, its
   stacked R/adv residual stays open per §12.1 row 9),
   `dudley-jetup-lp/mp/hp-block` (A 12/20/25), `dudley-jetup-lp-hit`
   (A 16), `dudley-jetup-hp-hit` (A 17), `twelve-crfierce-block/-hit`
   (A 18), `twelve-crroundhouse-hit` (A 10), `remy-rrf-lk-block/-hit`
   (A 4). The F3 member set is now 18 entries (was 2): 16 new (15 FULL +
   the 1 PARTIAL `sean-ryuubi-hk-block`). The bar itself is unchanged —
   new sightings still need their own ledger trace.

If a divergence doesn't fit any of the eight buckets above, check the
one-off class registry in `docs/frame-data-synthesis.md` §12.2.4
(S-divergence, defender-stance-conditional R, sum-preserving A/R
boundary shift, cut-committed whiff R-overshoot, Hayate compound,
block-adv anomalies, two-way contact R=0; SWEEP-2 2026-07-10 added
three rows: no-cut re-entry re-crediting, contact-skip R-window
reclassification, contact-mutated declared cell duration) — a match
there gets cited as
that registered class (still UNCLASSIFIED, but named and linked). Only
if §12.2.4 has no match either is it plausibly a genuinely new finding —
re-read Phase 4's STOP rule first (is it actually a whiff divergence in
disguise?) and re-check the exact mechanism against
`docs/frame-data-synthesis.md` §13 in full before concluding it's new.
New one-offs get ADDED to §12.2.4 (one row), not scattered.

**UPDATE (2026-07-10, CONV-2):** three of these classes' MEASURED members
now convert, citing §13.13's new families (SCOPE EXTENSION block,
`docs/frame-data-synthesis.md` §13.13): the contact-skip R-window
reclassification's MEASURED member `yun-crfierce-block` (family F7,
R=16 - the CANDIDATE `elena-lynxtail-lk-block` stays `xfail`); the
contact-mutated declared cell duration's `ken-tatsu-lk-block`/`-hit`
(family F8, R=15, both FULL); the no-cut re-entry re-crediting's contact
legs `remy-uoh-block`/`-hit` (family F9, A=13/11, both PARTIAL - the R
clause stays item 18(c)). The defender-stance-conditional row
(`necro-flyingviper`) is unchanged - still fully UNCLASSIFIED, no
conversion language applies.

- [ ] **A no-bucket-fits divergence is `xfail` + UNCLASSIFIED, never a new
      invented mechanism claim and never an ad-hoc accept.** Don't
      manufacture a plausible-sounding cause to force the finding into a
      tidy narrative — if the sign doesn't work out (e.g. a trace fact
      like a same-tick `cg_ix` jump would, by its own mechanics, only ever
      *lose* credit, but the measured divergence is an *overcount* — sign-
      incoherent), say so explicitly rather than reporting the trace fact
      as if it were the explanation. Author the entry's `xfail` string
      with: the full trace citation (raw facts only, labeled as
      observations if their causal role isn't established), an explicit
      `UNCLASSIFIED (flagged <date>, needs follow-up classification)`
      tag, any candidate mechanisms labeled as `HYPOTHESES ONLY` (not
      confirmed conclusions), and a note that it's flagged to the
      orchestrator/user for follow-up. Precedent: `ken-forward-block`/
      `-hit` (an A-overcount, the mirror of every previously known #17
      undercount — sign-incoherent with the observed same-tick `cg_ix`
      jump, which by itself can only lose credit) and `ken-twdshk-block`
      (an R-side deficit on a skip-jump trace, candidate mechanisms noted
      as hypotheses only) in `corpus-ken.yaml`. This keeps the divergence
      visible to the harness (still `xfail`, still gates the >50%-section
      STOP rule below) without asserting a mechanism nobody has verified.
      (Vindication note, 2026-07-09: both Ken precedents were later
      mechanism-classified by the sweep — buckets 8 and 7 above — with
      the honest hypothesis labels proving load-bearing: hypothesis (1)
      on ken-forward was confirmed in generalized form, hypothesis (1) on
      ken-twdshk confirmed as the branch variant. The discipline works;
      keep using it.)

9. **Oracle-structural incomparability** (the field itself, not the
   engine, cannot be compared — e.g. a negative Recovery figure against
   an R metric that is always `>=0`, or a zero-active move against an
   S/A/R decomposition that requires an active window to anchor) →
   **FIELD-MASK** (`docs/frame-data-synthesis.md` §13.15, MASK-1,
   2026-07-11 user decision). Allowed ONLY when
   `docs/arcade-frame-data/ERRATA.md` gives the field an explicit
   **STRUCTURAL** verdict — never for a value dispute
   (`UNVERIFIABLE`/`ORACLE-LIKELY-WRONG`) and never for an
   engine/observer-measurement class that happens to look similar (e.g.
   item 18(b)'s whiff-R truncation is a value divergence on a
   *comparable* field, not a structural incomparability — it stays
   `xfail`, never masked). Drop the `expect` assert for the
   incomparable field(s) only (subtractive — never add a new assert to
   an already-unasserted field); keep asserting every other field, which
   must all match golden measured exactly (a comparable-field mismatch
   bars masking entirely — the P-1 guard). Notation: the `FIELD-MASKED
   (ERRATA)` anchor comment (`docs/frame-data-synthesis.md` §13.15's
   "Entry notation" paragraph — §13.15 has no numbered subsections)
   directly above the entry, citing
   the dropped field's oracle value, measured value, and the ERRATA
   section; the `xfail` key is removed (an `xfail` left in place would
   read `XPASS`, a FAILING verdict, `check_frame_data.py:157-159`/`:197`).
   The masked field's measured value stays golden-pinned — future drift
   on that column still trips `--check-golden`. Precedent (converted,
   8 entries): `akuma-ashura-whiff`; `oro-oniyama-lp-whiff/-block/-hit`;
   `sean-roll-lp/mp/hp-whiff`; `ibuki-kubiori-lp-hit`. Excluded, named:
   `sean-dragonsmash-mp-whiff/-block/-hit` (UNVERIFIABLE value dispute,
   not structural); `ibuki-kubiori-lp-whiff/-block` (item 18(b) value
   divergence on the comparable S/A fields' own R clause).

## Phase 7 — Acceptance bar per character

- [ ] **Corpus exits 0.** `tools/frame-data/run.sh tools/frame-data/corpus-<char>.yaml`.
- [ ] **Determinism.** Two runs produce byte-identical results (compare
      `trace.log`/`expected.json` or just re-run and diff the checker
      output) — precedent throughout both existing corpora ("Confirmed
      strength-independent... in two separate runs (byte-identical)",
      `corpus-ryu.yaml:388-389`).
- [ ] **All existing corpora unchanged.** Run Q at minimum as the
      regression canary (`corpus-q.yaml`); running all three (Q/Ryu/Hugo)
      is preferred. Precedent: the §13.11 verification record measured
      all three corpora's before/after totals in the same session
      (`docs/frame-data-synthesis.md:4173-4184`). Current canary targets
      (dated 2026-07-10, re-measured for the §13.13 `CONV-C13` adoption,
      `docs/plan-frame-data-completion.md` Step 5(d)): Q `total=73
      (PASS=72, XFAIL=1)` — unchanged; Ryu `total=37 (PASS=37, XFAIL=0)` —
      all 3 shape-(a) xfails converted under §13.13; Hugo `total=30
      (PASS=28, XFAIL=2)` — 4 shape-(a1) xfails converted under §13.13,
      the Hugo-Roundhouse R-side pair (`h-roundhouse-block/-hit`) stays
      xfail (out of §13.13's named scope) — update these numbers here as
      corpora grow (new characters don't change Q/Ryu/Hugo's own totals,
      but a fix or convention adoption landing later might flip an xfail
      to a pass, which would).
      **UPDATE (2026-07-10, CONV-2):** Hugo's Roundhouse R-side pair now
      converts too (family F5) - Hugo `total=30 (PASS=30, XFAIL=0)`. Q
      and Ryu are unchanged (`total=73 (PASS=72, XFAIL=1)` / `total=37
      (PASS=37, XFAIL=0)`) - neither corpus has a CONV-2 member.
- [ ] **Batch regression gate.** Per-character iteration still uses
      `run.sh <corpus>` for the fast single-corpus loop above. The batch
      gate across every corpus is `tools/frame-data/run-suite.sh
      --check-golden` — build-once + bounded parallel fan-out (one
      isolated RUNDIR per corpus) instead of 19 serial rebuilds, so a
      full-suite check is ~4-12 min instead of ~35 min. It diffs each
      corpus's parsed table against `tools/frame-data/golden/<corpus>.tsv`
      (columns: `label verdict outcome n S A R adv kd`, one row per
      entry) rather than eyeballing totals — the canary totals block
      above is exactly what those golden TSVs encode per entry, now for
      every corpus. Regenerate after an intended change with
      `run-suite.sh --update-golden`.
- [ ] **xfail-rate accounting with the >50%-of-section STOP rule.** If a
      section (e.g. a grab family) crosses 50% xfail, that's the Hugo grab
      precedent (`corpus-hugo.yaml:115-122`: 6/11 = 55% xfail at authoring
      time, required explicit orchestrator/user sign-off before being
      accepted as audit output rather than xfail-spam — later resolved to
      0/11 once §13.10 landed). Get sign-off before accepting a
      high-xfail section on a new character; don't self-approve it.

## Phase 8 — Model/verification discipline notes for orchestrators

- [ ] **One commit per character**, on `frame-data-on-mister`
      (`docs/plan-frame-data-completion.md` house rules, line 206).
- [ ] **Never push** — no remote interaction without the user's explicit
      instruction (house rules, lines 207-208).
- [ ] **Same-binary protocol.** Any scratch/instrumented build used to
      probe a mechanism (e.g. adding a temporary trace `fprintf` to
      understand a divergence) must be reverted (`git checkout`) and the
      corpus's final numbers re-verified green on the **clean** tree
      before committing — precedent: the §13.11 verification record's
      "measured this session on a scratch instrumented build... reverted
      via `git checkout` + rebuild + re-verified green before the
      convention was implemented for real"
      (`docs/frame-data-synthesis.md:4173-4177`). `run.sh` itself always
      rebuilds `build/host` before each run (`run.sh:76-77`), so a stale
      binary is not the risk — a stale *understanding* pinned against an
      instrumented build that never shipped is.
- [ ] **Model policy.** A per-character corpus is `[/implement]`-sized
      (see Step 11's plan tag) — run it through the actual `/implement`
      skill loop (implement → review → fix), sonnet-class for
      implement/fix, fable-/opus-class for review
      (`docs/plan-frame-data-completion.md` house rules, lines 203-204).
      Writing the corpus file directly without the loop does not satisfy
      the plan's own skill-invocation requirement.
