# Plan — Frame-Data Completion (post-harness open items)

**Status:** PARTS A+B COMPLETE 2026-07-07 (steps 1–9; see tracker) — master
completion plan for everything left open after `docs/plan-frame-data-harness.md`
(which is COMPLETE, all phases 1–6 committed).
Part A steps 1–2 (Issue #17): step 1 diagnosis's CONFIRM-BLOCKED verdict is
**RESOLVED 2026-07-07 by user decision** — escalation option 2 (declared-truth
displayed-A convention) adopted; step 2 executed as **Step 2b**
(`docs/frame-data-synthesis.md` §13.11). Every other open issue (#14, #15,
#16, Hugo RH R+2, argparse, merge reproducer, gap register) is
resolved/classified/closed. Part C's gate text: escalation decided
2026-07-07; Part C may resume (Part C's own step statuses are unchanged by
this note — resuming is the user's call). Part D (step 14) pending Part C.
Suite state after Step 2b: Q 73 entries (72 PASS / 1 XFAIL), Ryu 37 (34/3),
Hugo 30 (24/6); 10 remaining xfails: 1 Q (`q-uoh-whiff-r`, item 18 — UOH
whiff R-truncation, distinct from issue #17) + 9 issue-#17 shape-(a)/R-side
(3 Ryu + 6 Hugo), permanently documented via §13.11.
**Branch:** `frame-data-on-mister`. Baseline is `mister` (not `main` — ignore the
gitStatus "Main branch" hint; see `project-baseline-branch.md` memory).
**Pairs with:**
- [`docs/frame-data-synthesis.md`](frame-data-synthesis.md) — canonical frame-data
  reference. Its numbered open-work list (items 14–17, §13 near the end of §13.8's
  lead-in, at the "Open work — prioritized" list) and §13.8's open-issue table are
  the source inventory for this plan.
- [`docs/plan-frame-data-harness.md`](plan-frame-data-harness.md) — the completed
  predecessor plan; its §1.9 acceptance criteria and sequencing table define the
  harness this plan's every step is verified with.

**Written 2026-07-07** against tree state `6ba03537` (+ uncommitted status-line edit
to `plan-frame-data-harness.md`). Every number in the preamble was re-measured on
this tree by running the harness (all three corpora), not copied from prior docs.

---

## Preamble

### Why this plan exists

The harness plan closed with a working, deterministic, self-validating regression
harness and three green-with-cited-xfails corpora. What remains is (a) two real
engine/overlay defects with scripted reproducers (issues #17 and #14), (b) one
divergence family needing per-move classification (issue #15 + a Hugo R-variant),
(c) a systemic CLI-parsing memory-safety bug, (d) a handful of documented harness
gaps and unverified asides, (e) the character-coverage rollout, and (f) a one-time
MiSTer-hardware verification pass. This plan sequences all of it so opus- and
sonnet-class agents can execute to completion without further architectural
oversight.

### Current verified state (2026-07-07, measured for this plan)

`tools/frame-data/run.sh <corpus>` on the current tree, three consecutive corpus
runs, all exit 0:

Note (2026-07-07): this table describes the pre-Step-3/pre-Step-5 XFAIL state;
see the tracker for current membership.

| Corpus | Entries | PASS | XFAIL | Exit |
|---|---:|---:|---:|---:|
| `tools/frame-data/corpus-q.yaml` | 69 | 60 | 9 | 0 |
| `tools/frame-data/corpus-ryu.yaml` | 36 | 21 | 15 | 0 |
| `tools/frame-data/corpus-hugo.yaml` | 30 | 17 | 13 | 0 |
| **Total** | **135** | **98** | **37** | |

The 37 XFAILs by family (every one carries a cited `xfail:` reason in its corpus
file — read those before touching anything):

Note (2026-07-07): this table describes the pre-Step-3/pre-Step-5 XFAIL state;
see the tracker for current membership.

| Family | Corpus entries | Count |
|---|---|---:|
| **Issue #17 — A-undercount on contact** (synthesis open-work item 17) | `ryu-far-jab-block`, `ryu-strong-block`, `ryu-far-strong-block`, `ryu-fierce-block`, `ryu-far-fierce-block`, `ryu-forward-block`, `ryu-roundhouse-block`, `ryu-strong-hit`, `ryu-fierce-hit`, `ryu-forward-hit`, `ryu-roundhouse-hit`, `ryu-crmk-block`, `ryu-twdshp-block`, `h-short-block`, `h-forward-block`, `h-short-hit` (pure); `ryu-crlk-block`, `h-forward-hit` (stacked with another family) | 16 pure + 2 stacked |
| **Issue #15 — HIT-R divergence** (item 15) | `q-dla-rh-hit`, `q-hsb-jab-hit`, `q-hsb-strong-hit`, `q-hsb-fierce-hit`, `q-uoh-samef-hit`, `q-uoh-1f-hit`, `q-throw-hit`, `ryu-throw-hit`, `h-strong-hit`, `h-moonsault-lp-hit`, `h-moonsault-mp-hit`, `h-moonsault-hp-hit`, `h-meatsquasher-short-hit`, `h-meatsquasher-forward-hit`, `h-meatsquasher-rh-hit` (pure); `h-forward-hit` (stacked with #17) | 15 pure + 1 stacked |
| **Issue #14 — cr.LK never classifies vs guard** (item 14) | `q-crlk-block`, `q-crlk-hit` (pure); `ryu-crlk-block` (stacked with #17) | 2 pure + 1 stacked |
| **Hugo Roundhouse contact-R +2** (new, Hugo audit — unclassified) | `h-roundhouse-block`, `h-roundhouse-hit` | 2 |

(16 + 15 + 2 + 2 pure = 35, plus `ryu-crlk-block` and `h-forward-hit` counted once
each as stacked = 37. ✓)

Issue #17 is the highest-value target: a correct fix flips up to 18 entries.

### Open-item inventory (verified on this tree; each maps to a step)

1. **Issue #17 — engine-wide A-undercount on contact.** Synthesis open-work item
   17 (full write-up) + `tools/frame-data/corpus-ryu.yaml` header (trace-proven
   two-shape root cause). → Steps 1–2.
2. **Issue #14 — cr.LK never sets `event_this_frame` vs guard/hit.** Synthesis
   item 14; `P2.dm_stop` goes `0 → +7` instead of negative; the overlay's event
   edge requires `dn->dm_stop < 0`. Affects Q AND Ryu. → Step 3.
3. **Hugo Roundhouse contact-R +2** — A clean, WHIFF exact, R+2 on both BLOCK and
   HIT (`corpus-hugo.yaml` header + `h-roundhouse-*` comments). Same
   "contact-tick" family shape as #17 but manifests on R. → Step 4.
4. **Issue #15 — HIT-R divergence family.** Synthesis item 15 + §13.7.3 (UOH's
   member is fully explained: defender hitstun ends before blockstun would,
   shortening the §13.5.1 cleanup-anim cut window). All members failed the Phase 4
   formula test (`raw_len − S − engine_a` vs arcade R) by 1–14 frames, so this is
   NOT CnDB's mechanism. Needs per-move classification and a per-subfamily
   overlay-vs-oracle-convention decision. → Step 5.
5. **argparse `ARGPARSE_OPT_BOOLEAN` 4-byte RMW through `bool*`.**
   `src/argparse/argparse.c:57-65` does `*(int *)opt->value = *(int *)opt->value +
   1` while `src/args.c` passes `bool` (1-byte) `Configuration` fields (23
   `OPT_BOOLEAN` sites, e.g. `args.c:217` `&configuration->headless`, `args.c:371`
   `&configuration->test.enabled`). Recorded in commit `c6a1a3da`'s message: "flags
   in the last 3 bytes of the struct can be silently reset by OOB garbage (affects
   test_sparse_effect_save dispatch nondeterministically)". Systemic — affects the
   reliability of every CLI flag, including the harness's own `--test-*` flags.
   → Step 6.
6. **Multi-move merge (F=4940 class) reproducer gap.** The `a386e057` mitigation
   (`engine_a` snapshot at `attacker_idle`) is shipped but UNVERIFIED: the only
   move pair tried (`q-crmk-multimove-merge`) cannot open the merge window (cr.MK's
   −3 block advantage means attacker recovery outlasts dummy blockstun by 3f).
   Needs a positive-advantage move pair (Q Jab is +2 on block). Synthesis §13.8
   open table row 1, §13.9.3. → Step 7.
7. **Issue #16 — unconfirmed close-range spurious FINAL.** Synthesis item 16: bare
   `press UP` or `LP+LK` at ≤~56px once produced `HIT S=2 A=1 R=34 adv=+101`; did
   not reproduce in isolation. → Step 8.
8. **Harness gaps** (all documented in corpus comments, consolidated nowhere):
   P-teleport no-op during knockdown tails (`corpus-hugo.yaml`,
   `h-meatsquasher-short-whiff` ordering comment), P1 teleport corruption after
   connecting dash specials (`corpus-q.yaml` ORDERING NOTE + HSB CORRECTION
   comment), DHA long-charge variant recipe unknown (charge 45–400f all give the
   fast variant, `corpus-q.yaml` Stage 3 header), SDB HIT/BLOCK unreachable (dummy
   can't jump, `corpus-hugo.yaml` SDB section), fireball WHIFF unreachable
   (`corpus-ryu.yaml` Hadouken section), hadouken block-adv plateau boundary
   121–149 unscanned (`corpus-ryu.yaml` distance-calibration comment), merged-move
   projectile contamination residual (`frame_data_overlay.c` consume-site comment,
   the "Residual (documented, not fixed)" block near line 1186). → Step 9
   (register) + Step 7 (the projectile-residual scope check).
9. **UOH retrigger residual — accepted, no step.** A retrigger landing before any
   cghi=1 dwell forms keeps inflated A (gate never arms). Documented as accepted
   scope in synthesis §13.9.4 "Accepted residual". Do NOT reopen; it is listed here
   so nobody mistakes it for an unknown.
10. **Coverage roadmap.** 17 characters un-audited; per-character JSON fetchable
    from Coccis77/thirdstrikedatabot (md5-pin discipline per commit `4fe2350e`).
    → Steps 10–13.
11. **MiSTer-side manual verification pass.** Overlay rendering on real hardware
    (KD purple, projectile numbers, honest T) has never been re-checked since the
    Phase 4–6 fixes landed. → Step 14.
12. **Netplay/rollback scope statement — DONE (Phase 5), do NOT reopen.** The
    `fd_*` globals' netplay-scope comment is in `workuser.c` above
    `fd_engine_hitbox_active`. No step.

### Regression envelope (MANDATORY for every engine-touching step)

Every step that modifies `frame_data_overlay.c`, `charset.c`, `frame_trace.c`, or
any engine file MUST run this full envelope before commit. "The harness" always
means `tools/frame-data/run.sh` (it rebuilds — same-binary protocol is structural).

**E1 — Three-corpora green.** All three runs exit 0 with exactly the expected
split — Q 62/7 (corrected 2026-07-07; the `q-crlk-block`/`q-crlk-hit` flip from
Step 3 postdates this text's original draft), Ryu 21/15, Hugo 17/13 — *adjusted
only by the entries your step
explicitly claims to flip* (an xfail your fix targets must flip to XPASS→you then
remove its `xfail:` key in the same commit so it becomes plain PASS; any OTHER
change in any corpus is a regression, stop and investigate). Corrected again
2026-07-07 (Step 2b, §13.11 declared-truth displayed-A convention): current
split is Q 73 (72 PASS / 1 XFAIL), Ryu 34/3, Hugo 24/6 — see the tracker for
the authoritative current membership.

```sh
tools/frame-data/run.sh tools/frame-data/corpus-q.yaml
tools/frame-data/run.sh tools/frame-data/corpus-ryu.yaml
tools/frame-data/run.sh tools/frame-data/corpus-hugo.yaml
```

**E2 — Byte-identity for untouched entries.** Two consecutive runs of each corpus
must produce byte-identical FINAL annotation sequences (determinism), and every
entry NOT targeted by your fix must have a byte-identical checker `got` string
vs. a pre-change baseline run (capture the pre-change RUNDIR table output first).
(Known benign exception, 2026-07-08 §13.5.1a: `makoto-hayate-lp-block`'s FINAL
diagnostic `anchor_a` changed 3→−1 — the never-committed anchor no longer arms
under the edge gate. S/A/R/adv byte-identical; `anchor_a` is not a checked field,
so the checker `got` string is unchanged.)

**E3 — The nine mutation levers (A–I).** These prove the shipped fixes stay load-bearing.
For each lever: apply the mutation, run the Q corpus (plus Ryu for lever D),
confirm EXACTLY the listed entries flag and nothing else, then `git checkout` the
mutated file and confirm green before proceeding. Anchors are function + snippet —
line numbers WILL drift; search for the snippet.

| Lever | File / anchor | Mutation | Expected flags (exactly these) |
|---|---|---|---|
| A — jatix revoke | `src/sf33rd/Source/Game/engine/charset.c`, in `char_move()`, the `if` beginning `if (fd_prev_active_cgix_tick[wk->id] == Game_timer` | force the condition false (e.g. prefix `0 &&`) | **SUPERSEDED 2026-07-07 (§13.11):** now expected to flag **NONE** on any corpus — the restore lives inside this same revoke block, so forcing the condition false skips subtract AND restore together, which is arithmetically identical to subtract-then-restore (path-independence invariant; see `docs/frame-data-synthesis.md` §13.11). Its former load-bearing role (flagging `q-uoh-samef-block`/`q-uoh-1f-block`/`q-uoh-chain-retrigger` at A=11) transfers to lever F below. |
| B — anchor-A gate | `src/sf33rd/Source/Game/ui/frame_data_overlay.c`, in `fd_finalize()`, `const bool use_anchor_a = cut_committed && g_cur.cghi1_dwell_broken && g_cur.engine_a_at_cut_anchor > 0;` | force false | **UPDATED 2026-07-07 (§13.11):** `q-uoh-chain-retrigger` FAIL **A=22** (was A=20 pre-convention — both taps now accrue 11 each under the declared-truth restore; trial FINAL `engine_a=22` proves the merged live value). |
| C — release-R | same file, in `fd_finalize()`, `} else if (g_cur.ended_by_partner_release) {` | force branch off (`else if (false && …)`) | `q-cndb-lk-hit`/`-mk-hit`/`-hk-hit` FAIL R=43/45/47 |
| D — projectile arcade-split | same file, in `fd_finalize()`, `const bool use_proj_split = g_cur.proj_seen && !use_hatt;` | force false | the 6 `ryu-had-*` entries FAIL with the legacy signature (S=0-style garbage, S/R wrong; pre-Phase-6A hadouken read `S=0 A=42 R=4`) |
| **E — strike-KD predicate — UPDATED ANCHOR + GROWN SET 2026-07-09 (§13.6.2b, ENGINE-3)** | same file, `fd_r2_is_down_family(s16 r2)` (new helper, near `fd_is_knockdown_at_atk_idle()`): `return (r2 >= 14 && r2 <= 23) \|\| r2 == 27 \|\| r2 == 28;` — used by BOTH the last-cell clause in `fd_is_knockdown_at_atk_idle()` and the tick-side latch (lever I, below) | force the helper to return `false` unconditionally (kills the latch and the last-cell clause together, since both consume this one helper) | `q-fierce-hit`, `q-crhk-hit`, `q-backhk-hit` FAIL kd 0≠1 (CnDB/throw kd unaffected — they use the untouched r1∈{2,3} test). Suite-wide (goldens): checker-FAILs on every kd:1-asserted strike row incl. the 9 flips of §13.6.2b/A.3(c); golden-only drift on the 26 unasserted flips + q-dla-rh-hit + elena-lynxtail + elena-rhinohorn ×2 + yun-zesshou ×3. Smoke-verified on Q 2026-07-09 (exactly the 3 entries, nothing else); full-suite golden gauntlet not re-run for this row (redundant with lever I's full-suite run, since both mutate the same underlying set). |
| **I — strike-KD deferred-window latch (new, 2026-07-09, §13.6.2b, ENGINE-3)** | same file, tick-side block right after the defender-idle-return check (non-throw branch): `const int fd_strike_kd_latch = 1;` | set to `0` (the helper's set stays intact — only the last-cell clause remains) | Checker flags NOTHING on any of the 13 affected corpora (akuma/alex/dudley/elena/ibuki/ken/necro/oro/remy/sean/urien/yang/yun — full-suite run 2026-07-09); golden drift is EXACTLY `urien-vkd-lk-hit` (kd 1→0) — the census-proven only member whose down-family traversal (`def_r2==14`) is invisible at the last raw[] cell (every other flip's own last cell already carries a down-family value: 17/18/20/21/23/27). This single-member signature is the latch's load-bearing proof. |
| **J — hit-checkable projectile split (new, 2026-07-09, §13.12, ENGINE-4)** | same file, in `fd_finalize()`'s `use_proj_split` branch: `const int fd_proj_hitcheck_split = 1;` | set to `0` (only the legacy `S = proj_spawn_slot` / `R = meter_len − S` anchors remain; the three engine-side latches `fd_engine_proj_hitok`/`_cut`/`_natend` stay unconditional write-only feeds — no rebuild needed to restore goldens after restoring the lever) | Flags EXACTLY the 9 `twelve-ndl-{jab,strong,fierce}-{whiff,block,hit}` entries, regressing to their pre-fix measured values (S=12/12/13, R=26/30/34); all 45 other `proj=1` rows (ryu/ken/akuma/chunli/oro/remy/urien) are byte-identical under both lever settings — this identity is itself load-bearing (a lever-G-style negative, proving the new anchors are a no-op for every fire-and-forget fireball). |
| **F — declared-truth restore gate (new, 2026-07-07, §13.11)** | `src/sf33rd/Source/Game/engine/charset.c`, in `char_move()`, `const int fd_restore_revoked_declared_credit = 1;` (inside the same revoke block as lever A, immediately after `fd_prev_active_cgix_add[wk->id] = 0;`) | set to `0` | **Q:** `q-uoh-samef-block`, `q-uoh-1f-block`, `q-uoh-samef-hit`, `q-uoh-1f-hit` FAIL A=10, and `q-uoh-chain-retrigger` FAIL A=10 (anchor regresses) — all ≠ new expect 11. `q-uoh-whiff`/`q-uoh-whiff-r` do NOT flag (no revoke fires on whiff). **Ryu:** the 11 flipped shape-(b) entries (far-jab, strong-b/h, fierce-b/h, far-fierce, forward-b/h, roundhouse-b/h, crlk) FAIL at their old measured A. **Hugo:** none (zero revoke sites, census 2026-07-07). |
| **G — §13.5.1a cut gate (new, 2026-07-08) — UPDATED TEST PROCEDURE 2026-07-09** | `src/sf33rd/Source/Game/ui/frame_data_overlay.c`, in the §13.5.1 tick-side block, `const int fd_cut_requires_grounded_fresh_edge = 1;` | set to `0` (restores the legacy `cghi==1` level-test anchor) | **Q:** flags NOTHING (the protected walls — q-uoh R=5, q-throw-hit R=34, CnDB R=42/44/46 — never depended on the gate's rejects; this negative is itself load-bearing). **The legacy-level-test configuration is now G=0 AND H=0 together**, not G=0 alone: testing G=0 with H=1 is NOT the legacy configuration — lever H would still refuse many legacy level-anchors (e.g. Ken SRK's landing tail keeps `guard_flag=3`), masking part of the G mutation's own effect. Run as G=0 **and** H=0: flags EXACTLY the documented 31 legacy entries (ken `ken-srk-lp-{whiff,block,hit}` R=19, akuma `akuma-srk-lp-{whiff,block,hit}` R=18, yun the 9 `yun-nishou-*` entries R=16/17/19, urien the 10 chariot/headbutt/VKD-adjacent entries at their pre-fix R/adv, dudley `dudley-jetup-{lp,mp,hp}-whiff` R=19/18/22, oro `oro-niouriki-lp-{whiff,block}` R=11 + `oro-jinchu-lk-whiff` R=6) **PLUS** the lever-H §5 flip set at ITS OWN legacy values (the grown set is mechanically explained: any entry whose PASS depends on the cut machinery flags when the whole machinery reverts to both levers' pre-fix behavior) — see `docs/frame-data-synthesis.md` §13.5.1a/§13.5.1b. |
| **H — §13.5.1b guard-rearm commit gate (new, 2026-07-09)** | `src/sf33rd/Source/Game/ui/frame_data_overlay.c`, in the §13.5.1 tick-side block (before the dwell if/else-if chain), `const int fd_cut_requires_guard_rearm = 1;` | set to `0` (restores the pre-fix behavior: a committed cut is trusted regardless of `guard_flag`) | **Q:** flags NOTHING (q-throw-hit is r1==2, exempt from the gate by construction; no other Q entry depends on it — this negative is itself load-bearing). **Mandatory full-19-corpus run (G stays at its shipped value, 1):** CHANGES exactly the §5/§6 flip set (33 windows) to their pre-fix measured values and nothing else — but "changes" is not uniformly "flags": the checker only newly FAILS the 20 of those 33 that are plain-PASS entries (no `xfail` key) — `alex-airstampede-lk-whiff` R=2, `elena-scratchwheel-lk-{whiff,block,hit}` R=25, `remy-rrf-lk-whiff` R=29, `akuma-tatsu-lk-{whiff,block,hit}` R=12, `ken-tatsu-lk-whiff` R=9, `chunli-hk-whiff` R=10, `chunli-hazan-lk-whiff` R=15, `sean-ryuubi-{lk,mk}-whiff` R=8/9, `yang-twdsmk-{block,whiff}` R=3, `yun-twdsmk-{block,whiff}` R=3, `alex-crfierce-{block,hit,whiff}` R=19. The remaining 13 (`remy-rrf-lk-{block,hit}`, `ken-tatsu-lk-{block,hit}`, `chunli-farroundhouse-block`, `chunli-hazan-lk-{block,hit}`, `sean-ryuubi-{lk,mk}-{block,hit}`+`hk-{block,hit}`) are already `xfail` on their own narrowed/adjacent clause (e.g. an A-overcount or a ±1 contact-variance) — their regression to the pre-fix R value is SILENT under XPASS/FAIL semantics: the checker still reports XFAIL (not a new flag), and the drift is visible only in the measured value / golden diff, never the exit code. Every protected wall (q-uoh 5/3, q-throw-hit 34, q-cndb 42/44/46, senkyuutai 38, oro-uoh/oniyama, ibuki-kubiori/-kazekiri, elena-lynxtail) stays unmoved. Smoke-verified (alex + q) 2026-07-09; the full 19-corpus mutation run is the orchestrator's authoritative A–I gauntlet. |

A fix that changes any lever's expected flag set is presumptively wrong; the ONLY
acceptable outcome is a *documented, understood* change (like §1.9.3's two→three
entry growth for lever A, which was root-caused and written up). If you can't
explain the change mechanically, revert.

**E4 — MiSTer cross-build.** `tools/mister/build-game.sh --flavor telemetry` must
still compile; the ARM binary must contain no harness symbols
(`strings <binary> | grep -c InputScript` == 0) when your change touches
`src/test/`. (Per `feedback-always-telemetry`, telemetry is the only flavor we
ship.)

### House rules for implementing agents (non-negotiable)

- **Skill loop is mandatory.** Every step below marked `/implement` must actually
  invoke the `/implement` skill (three-agent implement→review→fix loop); steps
  marked `/plan then /implement` need the plan skill first. Writing files without
  the loop does not count (`feedback-enforce-skill-invocation.md`).
- **Model policy:** implement/fix agents run sonnet-class; review agents run
  fable- or opus-class. Diagnosis agents inside a step may be sonnet with the
  step's STOP rules pasted into their prompt verbatim.
- **One commit per step**, descriptive message, on `frame-data-on-mister`.
  **Never push** — no remote interaction of any kind without the user's explicit
  instruction.
- **No wrong-data acceptance.** Never "accept the small discrepancy and ship"
  (`feedback-no-shipping-wrong-data.md`). If a value is wrong and the step's STOP
  rules end the investigation, the entry stays/becomes `xfail` with a cited
  mechanism — the arcade value stays in `expect`, the measured wrong value goes in
  the `xfail:` string. Never substitute a measured wrong value into `expect` as if
  it were correct (the only exception is a *documented oracle-convention*
  divergence explicitly adopted by the user — currently **§13.10 / §13.11 /
  §13.13**, each adopted by explicit user decision — which changes what "correct"
  means for the affected entries and must cite the synthesis section that adopts
  the convention).
- **Same-binary protocol.** All traces/stderr must come from the binary built in
  the same run. `run.sh` enforces this; if you hand-run the game for diagnosis,
  `cmake --build build/host -j8` first, in the same shell block.
- **LSP note:** the decompiled `src/sf33rd/` sources use nonstandard
  types/macros that produce spurious language-server diagnostics. The compiler is
  the authority — a change is clean iff `cmake --build build/host` and the E4
  cross-build are clean. Do not "fix" LSP-only complaints in decompiled code.
- **Fact-based diagnosis.** Every claim in a finding cites file:line, a command +
  output, or a trace excerpt. Unknown is a valid finding; a guess is not.
- **Read before touching:** `docs/frame-data-synthesis.md` §13.8 (pickup
  checklist), §13.9 (failed-attempt history — two force-finalize fixes were
  shipped and REVERTED; do not re-ship them), and the corpus file headers.

---

## Part A — Engine fixes that flip existing xfails

### Step 1 — Issue #17 diagnosis: choose the A-undercount fix mechanism `[/plan]` (size M)

**Why.** Issue #17 is the largest open defect (16 pure + 2 stacked xfails across
Ryu/Hugo, engine-wide, latently affecting Q by animation-data luck). Three fix
families are plausible; picking wrong regresses UOH (lever A/B territory). This
step produces a written, falsification-tested fix selection; Step 2 implements it.
Splitting them keeps each agent's blast radius small.

**Read first (exact files):**
- `docs/frame-data-synthesis.md` — open-work item 17 (search "same-tick contact
  advance under-credits"), §13.7.4 (the revoke this interacts with), §13.9
  (failed-attempt history).
- `tools/frame-data/corpus-ryu.yaml` header ("MAJOR FINDING" block — the
  trace-proven two-shape mechanism with GT references).
- `src/sf33rd/Source/Game/engine/charset.c` — `char_move()`, the accumulator block
  starting at `if (wk->cg_ja.atix != 0 || wk->cg_ja.caix != 0)` (~line 447):
  revoke, per-cell `add = ctr` crediting, sentinel handling.
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c` — `fd_finalize()` (`effective_a`
  selection, `use_anchor_a`), the tick-side §13.5.1 else-if chain.
- `docs/plan-frame-data-harness.md` §1.9.3 (mutation-test history).

**Files to modify:** none in the shipped tree except scratch instrumentation that
MUST be reverted before the step's commit; the committed artifact is a new
`## Step 1 findings` appendix in THIS file (or a synthesis-doc subsection under
item 17) recording the evidence and the chosen mechanism.

**The two confirmed shapes (from the corpus-ryu header; re-verify on your own
fresh trace before designing anything):**
- (a) **skip-to-recovery**: contact tick advances the chain past a declared active
  cell that is never entered (Ryu Far Strong GT=136: contacts on cgix=8, same tick
  jumps to recovery cgix=16; the skipped cgix=12/jatix=6/3-frame cell plays in
  full on WHIFF). Its declared `cg_ctr` credit never accrues → A=1 vs arcade 4.
- (b) **same-jatix transit**: contact tick advances to a pat-consecutive cell with
  the SAME jatix, tripping the §13.7.4 revoke, which discards already-elapsed real
  frames (Ryu Strong GT=43: cell 12 credited +2, then revoked; A=2 vs arcade 4;
  without the revoke this one move reads exactly 4).

**Candidate mechanisms to evaluate (design + falsify on paper/trace before any
implementation; at least one discriminating measurement per candidate):**
1. **Credit-skipped-cells at contact (charset.c side):** when the same-tick
   advance fires on a contact tick, credit the skipped/collapsed cells' declared
   `cg_ctr` before the branch (shape (a)) and suppress/compensate the revoke
   (shape (b)). Needs a reliable in-`char_move()` contact-tick signal — candidate
   discriminators to measure: attacker hitstop (`hit_stop`/`hstop` trace column),
   defender `dm_stop` transition on the same GT, `fd_engine_hitbox_active` state.
   Check: does crediting the skipped cell produce arcade A on BOTH shapes for all
   ~10 affected moves, and 0 delta on WHIFF (no contact ⇒ no branch) and on Q/UOH?
2. **Contact-aware revoke:** narrow lever A's condition so it never fires on a
   contact tick (fixes shape (b) only — shape (a) needs candidate 1 or 3 anyway,
   measure how many entries each shape accounts for; if (b)-only covers few
   entries this candidate is insufficient alone).
3. **Finalize-side correction (overlay only, like Phase 4.4):** latch a
   "same-tick contact advance happened, N declared frames lost" quantity at the
   tick/accumulator site into a new `FdMove` field, and re-derive displayed A at
   `fd_finalize()` gated on an explicit flag (the CnDB `ended_by_partner_release`
   pattern). Keeps `charset.c` behavior-neutral for UOH.
4. Any hybrid — but the decision doc must say which shape each half handles.

**Success criteria (all required):**
1. A written findings section: per affected move (≥ the 9 Ryu + 2 Hugo pure-A
   entries), which shape it is, measured lost-credit quantity, and the arcade A it
   must reach — each row backed by a GT-cited trace excerpt.
2. A chosen mechanism with an explicit paper falsification pass: applied mentally/
   arithmetically to every affected entry AND to the lever A/B protected cases
   (UOH samef/1f/chain-retrigger) AND to 3+ currently-green Q contact entries
   (e.g. `q-strong-block`, `q-crhk-hit`, `q-fierce-hit`), showing expected A for
   each. Anything the arithmetic can't predict = not chosen.
3. An implementation sketch for Step 2: exact functions, new fields/flags, gate
   conditions, and which mutation lever(s) the new code itself gets.
4. No tree changes committed other than the findings text.

**STOP rules (paste into the diagnosing agent's prompt):**
- STOP if the fix would require changing engine *gameplay* behavior (hit timing,
  cell advancement itself, `hitcheck.c`, `dm_stop` writing). The accumulator and
  overlay observe; they never steer. Write up and escalate to the user.
- STOP if every candidate requires weakening/disabling the §13.7.4 revoke such
  that lever A's expected flag set changes in a way you cannot mechanically
  explain and defend for UOH. (The corpus-ryu header and commit `4fe2350e` both
  state: a fix must NOT simply disable the revoke.)
- STOP if the discriminating signal for "contact tick" cannot be established from
  data already visible inside `char_move()`/the overlay tick (no new engine
  plumbing into gameplay routines beyond read-only observation).
- STOP after 3 falsified candidates: write the falsifications up (§13.7.5-style),
  leave the xfails standing, report to the orchestrator/user.

**Dependencies:** none. **What NOT to do:** don't implement; don't re-try the two
reverted force-finalize approaches (§13.9.1/§13.9.2); don't touch corpora.
**Failure fallback:** the STOP write-up itself is the step's deliverable; commit
it (docs-only) and mark the step BLOCKED in the tracker with the falsification
citations.

### Step 2 — Issue #17 fix implementation `[/implement]` (size L)

**Why.** Flips up to 18 xfails (16 pure + the #17 halves of `ryu-crlk-block`,
`h-forward-hit`) and removes the biggest known engine-wide overlay inaccuracy.

**Read first:** Step 1's findings section (mandatory gate — if Step 1 is BLOCKED,
this step does not run); the same file list as Step 1.

**Files to modify:**
- `src/sf33rd/Source/Game/engine/charset.c` and/or
  `src/sf33rd/Source/Game/ui/frame_data_overlay.c` per Step 1's chosen mechanism.
- `tools/frame-data/corpus-ryu.yaml`, `tools/frame-data/corpus-hugo.yaml` —
  remove `xfail:` from every entry the fix flips (their `expect` already holds the
  true arcade values, per the audit discipline — you change ONLY the xfail key).
  For the two stacked entries, EDIT the `xfail:` string to remove the #17 clause
  and keep the remaining family's clause (`ryu-crlk-block` keeps the cr.LK
  dm_stop clause; `h-forward-hit` keeps the HIT-R clause).
- `docs/frame-data-synthesis.md` — item 17 gets a SHIPPED marker with commit ref,
  mechanism summary, and the new mutation lever's documentation.
- `docs/plan-frame-data-harness.md` §1.9.3 — append the new lever if the fix adds
  one (follow the lever-C precedent text).

**Success criteria:**
1. Full regression envelope E1–E4 passes. Expected new splits: Ryu ≥ 34 PASS
   (21 + 13 pure #17 entries) / ≤ 2 XFAIL; Hugo ≥ 20 PASS / ≤ 10 XFAIL; Q exactly
   60/9 unchanged (Q has no #17 xfails — any Q delta is a regression).
   `ryu-crlk-block` stays xfail (its outcome-shape failure is #14's), with its
   A-clause removed only if the trace shows A now correct beneath the WHIFF
   misclassification.
2. Every lever A–E mutation still flags exactly its documented set (or the
   documented-and-explained superset, written up like §1.9.3's lever-A growth).
3. A NEW mutation lever for this fix (force its gate off → exactly the flipped
   entries regress to their old measured values), verified and documented.
4. WHIFF entries all still exact (the fix must be contact-gated; WHIFF paths
   never enter it).
5. E4 cross-build clean.

**Dependencies:** Step 1. **What NOT to do:** don't touch the §13.6.1
partner-release path, the anchor-A snapshot, or `use_proj_split`; don't remove
xfails the fix doesn't actually flip; don't chase the Hugo Roundhouse R+2 here
(Step 4 owns it — if your fix happens to change `h-roundhouse-*`, STOP and
re-diagnose, because a pure-A fix has no business moving R).
**Failure fallback:** if implementation falsifies the chosen mechanism (measured
A wrong on some subset), revert fully (`git checkout` the touched engine files),
append the falsification to Step 1's findings, and re-enter Step 1 at the
next-candidate position. Two full falsification round-trips = BLOCKED, escalate.

### Step 3 — Issue #14: cr.LK never classifies vs guard `[/plan then /implement]` (size M)

**Why.** Q and Ryu cr.LK finalize WHIFF against a guarding/hit dummy — the overlay
misses the entire outcome, which is worse than a wrong number. 3 xfail entries.

**Read first:**
- `docs/frame-data-synthesis.md` item 14 + §13.2 (the dm_stop-sign finding).
- `src/sf33rd/Source/Game/ui/frame_data_overlay.c` — `frame_data_overlay_tick()`,
  the event-edge block: `if (g_cur.event == FD_OUTCOME_NONE && dp->dm_stop == 0 &&
  dn->dm_stop < 0)` (search the snippet; the doc's old `:663` citation has
  drifted — it's ~line 929 on this tree).
- Engine writers of `dm_stop`: grep `dm_stop` under `src/sf33rd/Source/Game/`
  (expect `plpdm.c` / damage-application sites) to establish what a POSITIVE
  dm_stop means before widening the predicate.
- `tools/frame-data/corpus-q.yaml` `q-crlk-block`/`q-crlk-hit`,
  `corpus-ryu.yaml` `ryu-crlk-block`.

**Diagnosis first, with STOP rules:**
- Establish from engine code + trace WHY cr.LK's contact writes `dm_stop = +7`
  while cr.MK/cr.HK write −9 (which move-data field / code path flips the sign,
  and what the sign semantically means — hitstop direction? counter? chip?).
- STOP if positive-dm_stop turns out to be a broad engine state shared with
  non-contact situations (e.g. also fires on proximity/pushback) — then a naive
  `!= 0` edge would create false HIT/BLOCK events; the fix must find a
  discriminator, and if none exists in overlay-visible state, write up + leave
  xfail.
- STOP if the fix requires touching the engine's damage code. Overlay-side only.

**Files to modify:** `frame_data_overlay.c` (the event-edge predicate; likely
accept `dn->dm_stop != 0` transition from 0, or add a parallel positive-edge arm
— per what diagnosis shows), plus the three corpus entries (remove xfail),
synthesis item 14 (SHIPPED marker + mechanism), and §13.2.

**Success criteria:** `q-crlk-block` flips to plain PASS asserting BLOCK with
from-qjson S/A/R/adv; `q-crlk-hit` PASS with HIT; `ryu-crlk-block` either PASS
(if its stacked #17 A-clause was already fixed by Step 2) or stays xfail citing
ONLY the remaining un-fixed clause. Full envelope E1–E4; levers unchanged. A new
mutation note is optional (small predicate), but the commit message must state the
before/after predicate exactly.

**Dependencies:** none strictly; run after Step 2 to avoid corpus churn on the
stacked entry. **What NOT to do:** don't touch the throw-grapple event edge
(`dp->r1 == 0 && (dn->r1 == 2 || dn->r1 == 3)`) or the parry arm; don't let the
widened predicate re-classify parries (parry keeps `dn->r1 == 0` + its own dm_stop
shape — verify parry entries in the trace still classify PARRY; the corpus has no
parry entries, so check manually via a one-off scratch corpus if the predicate
change could plausibly reach it).
**Failure fallback:** xfail stands with the diagnosis appended to synthesis item
14; commit docs-only.

### Step 4 — Hugo Roundhouse contact-R +2: classify (and fix only if it falls out) `[/plan]` (size S)

**Why.** `h-roundhouse-block`/`-hit` measure R=30 vs arcade 28 with A and adv
exact and WHIFF exact — a contact-tick R divergence that is neither #17 (A) nor
obviously #15 (which is HIT-only; this one hits BLOCK too). It must be classified
so it lands in exactly one family's bucket.

**Read first:** `corpus-hugo.yaml` header + the two entries' comments; Step 1's
findings (the same-tick contact-advance mechanism may explain an R-side loss:
a skipped/collapsed cell's frames landing in recovery instead of active);
synthesis §13.7.3 (the HIT-R cut-window mechanism — does NOT fit BLOCK, note why
in the write-up); `frame_data_overlay.c` recovery derivation in `fd_finalize()`
(the `recovery_pf` tally and §13.5.2 override).

**Procedure:** after Step 2 lands, re-run the Hugo corpus. If the entries now
PASS, they were #17-family — remove xfails, done. If not: capture a fresh trace
(`run.sh` leaves RUNDIR on failure; or run a single-entry scratch corpus), do the
§13.7.3-style tick table (cgix/cghi/GT around contact and move end) for BLOCK,
HIT, and WHIFF, and identify where the 2 extra R frames accrue.

**STOP rules:** this is a classification step — STOP once the mechanism is
identified and written up; implementing a fix is IN scope only if it's a
parameter-level reuse of Step 2's shipped mechanism (same flag, same gate,
demonstrably the same root cause). Anything novel becomes its own follow-up entry
in the tracker. STOP after one focused session without a mechanism: leave xfail,
write up what was measured.

**Files to modify:** `corpus-hugo.yaml` (xfail strings updated to cite the
classified family), synthesis (item-17 or item-15 text gains the Hugo R case, or
a new numbered item if it's genuinely a third mechanism).

**Success criteria:** the two entries cite a *specific* mechanism (not "same
general family"), verified by a tick-table excerpt in the write-up; envelope E1–E3
untouched if no code changed.

**Dependencies:** Step 2. **What NOT to do:** don't fold it into #15's convention
decision without BLOCK-side evidence (BLOCK R divergence contradicts §13.7.3's
defender-hitstun mechanism by construction). **Failure fallback:** documented
non-classification (measurements + dead ends) in synthesis; xfails stand.

### Step 5 — Issue #15: HIT-R divergence family — per-move classification + convention decision `[/plan then /implement]` (size L)

**Why.** 15–16 xfails. UOH's member is *fully explained and arguably correct*
(§13.7.3: arcade tables canonicalize R against blockstun; on HIT the defender
recovers earlier, so the §13.5.1 cut window is genuinely shorter — "not a defect;
just a different canonical figure"). If that explanation generalizes, most of this
family is an *oracle-convention* mismatch, not a bug — and the right ship is a
documented convention + corpus convention-expectations, not code.

**Read first:**
- Synthesis §13.7.3 (UOH mechanism + Phase 4 formula-test exclusion), item 15,
  §12 "Q specials sampled" table rows for HSB/DLA-RH/Throw.
- The 16 entries' xfail strings (Q 7, Ryu 1, Hugo 8 incl. the stacked one).
- `frame_data_overlay.c`: §13.5.1 cut predicate (tick side), §13.5.2 multi-hit
  recovery override (`last_active_pf_idx`), throw-path R accounting.

**Procedure (diagnosis-first, per-subfamily):**
1. **Strike subfamily** (HSB×3, DLA-RH, `h-strong-hit`, `h-forward-hit`'s R
   clause): for each, produce the §13.7.3-style tick table on HIT vs BLOCK/WHIFF.
   Question to answer per move: is the R delta exactly the defender
   hitstun-vs-blockstun differential (⇒ UOH's mechanism, convention issue), or
   something else (HSB's −5..−7 and DLA-RH's −7 are large; §13.7.3's UOH case was
   only −2)?
2. **Grab subfamily** (Q/Ryu Throw, Moonsault×3, Meat Squasher×3): these END by
   knockdown; "R" in the arcade table for a connected grab is a different
   convention question entirely (q.json's Throw 21 is called a "capture
   convention" question in `q-throw-hit`'s xfail). Determine what the oracle's R
   even measures for a landed grab (compare against WHIFF R where exact:
   Moonsault WHIFF R=36 = arcade; measured HIT R=67) — likely the overlay is
   measuring through the throw animation while arcade counts a different span.
3. **Convention decision, per subfamily:** for each subfamily either
   (a) **overlay change** — R should be re-derived to match arcade convention on
   HIT (only if a principled boundary exists, CnDB-style, gated by an explicit
   flag with its own mutation lever), or (b) **oracle convention adopted** — the
   synthesis doc gains a normative subsection ("HIT-R display convention") stating
   the overlay intentionally reports actionable-relative-to-defender-neutral (or
   whatever the measured truth is), and the corpus entries change from `xfail` to
   asserting the *convention-correct* expected values with a comment citing that
   subsection. Option (b) is the ONLY sanctioned way in this whole plan to change
   an `expect` away from the raw oracle number, and it requires the synthesis
   subsection to exist in the same commit.
4. Implement whichever (a) fixes fall out; ship (b) documentation for the rest.

**STOP rules:**
- STOP on any temptation to make the §13.5.1 cut defender-state-conditional
  without per-move tick evidence — that predicate protects UOH R=5/CnDB and has a
  revert history (§13.9).
- STOP per-move after the tick table is built and the delta still has no
  mechanism; that move's xfail stands with the table pasted into the write-up.
- STOP the whole step if a proposed overlay change would alter WHIFF/BLOCK R on
  ANY currently-green entry (E2 byte-identity outside HIT rows is a hard wall).

**Files to modify:** `frame_data_overlay.c` (only for sanctioned (a) fixes),
all three corpus files (xfail→PASS for (a); xfail→convention-expect for (b)),
`docs/frame-data-synthesis.md` (the new convention subsection + item 15
resolution), `docs/plan-frame-data-harness.md` lever table if a new lever ships.

**Success criteria:** zero remaining `xfail` entries citing "HIT-R divergence
family" as an *unexplained* mechanism — each is either PASS (fixed), PASS
(convention-asserted, citing the new synthesis subsection), or xfail citing a
move-specific documented dead-end table. Envelope E1–E4 + all levers.

**Dependencies:** Steps 2, 4 (so the family's membership is final before the
convention is decided). **What NOT to do:** no blanket formula applied across the
family (the Phase 4 formula test already falsified the only obvious one — see
item 15: fails by 1–14 frames per move); no un-cited expect changes.
**Failure fallback:** (b)-only outcome is acceptable and still closes the item;
full non-classification on a subfamily leaves those xfails standing with tables.

---

## Part B — Toolchain correctness and reproducer gaps

### Step 6 — argparse `OPT_BOOLEAN` 4-byte write through `bool*` `[/implement]` (size S)

**Why.** `argparse_getvalue()` (`src/argparse/argparse.c`, `case
ARGPARSE_OPT_BOOLEAN:`) executes `*(int *)opt->value = *(int *)opt->value + 1`
(and the OPT_UNSET/-1 and clamp variants) — a 4-byte read-modify-write through
pointers that `src/args.c` supplies as 1-byte `bool` fields of `Configuration`
(23 sites; e.g. `&configuration->headless` at `args.c:217`,
`&configuration->test.enabled` at `args.c:371`,
`&configuration->test.initial_super_full` at `args.c:424`). Reading 3 bytes of
adjacent struct memory and writing them back races struct layout: the tail bools
of `Configuration` (`test_netplay_event_queue` … `test_bilateral_punch`,
`src/configuration.h:89-125`) get corrupted nondeterministically. Recorded in
commit `c6a1a3da`'s message. This is the CLI the harness itself stands on.

**Read first:** `src/argparse/argparse.{c,h}` (the whole small vendor lib),
`src/args.c` (every `OPT_BOOLEAN(` site and each target field's declared type in
`src/configuration.h`), `src/argparse/README` if present (vendor provenance —
`git log --follow src/argparse/argparse.c`).

**Decide between two shapes (implementer's choice, justify in commit):**
1. **Fix the lib:** make `ARGPARSE_OPT_BOOLEAN` write a single byte (treat
   `value` as `bool*`/`unsigned char*`; counting semantics degrade to set-1/clear-
   0, which is what every call site in this repo uses — verify no site relies on
   the increment-counting behavior: grep for reads of any OPT_BOOLEAN-bound field
   expecting values > 1).
2. **Fix the call sites:** change every OPT_BOOLEAN-bound `Configuration` field to
   `int` (touches `configuration.h` consumers — grep each field's readers).

Option 1 is smaller and fixes future call sites; take it unless the verification
grep finds a counting consumer.

**Files to modify:** `src/argparse/argparse.c` (option 1) or `src/configuration.h`
+ readers (option 2). NOT both.

**Success criteria:**
1. A before/after demonstration: a minimal reproduction showing the corruption
   (e.g. compile a tiny harness or add a temporary assert dumping the tail-bool
   bytes after parsing `--test-enable`-style flags with a poisoned struct
   pattern), captured in the commit message or a test — then gone after the fix.
   If a live corruption repro proves elusive (it was nondeterministic), a
   UBSan/ASan run of `3S-ARM --test-enable --headless --probe-renderer-only`-class
   flag combinations before (flagging the OOB) and after (clean) is the
   acceptable substitute — `clang -fsanitize=address` on the host build.
2. Full harness envelope E1–E2 (the harness passes `--test-enable`,
   `--test-pin-rng` etc. — proves the parse still works).
3. `--test-sparse-effect-save` dispatch behaves deterministically across 10
   repeated invocations (the symptom named in `c6a1a3da`).
4. E4 cross-build.

**Dependencies:** none — may run any time, including in parallel with Part A
(different files). **What NOT to do:** don't restyle/reformat the vendor lib;
don't change `OPT_BIT`/`OPT_INTEGER` semantics; don't switch args parsing
libraries. **Failure fallback:** none expected (mechanical); if option 1 breaks a
counting consumer, fall back to option 2 for that field only.

### Step 7 — Multi-move-merge reproducer (positive-advantage pair) `[/implement]` (size S–M)

**Why.** The `a386e057` merge mitigation (engine_a snapshot at `attacker_idle`)
has never been exercised: cr.MK can't open the window (recovery outlasts
blockstun by 3f — proven, synthesis §13.8 open table row 1). The F=4940 class
needs: attacker idle while defender still blockstunned, then a second button
before deferred finalize runs. Q Jab is +2 on block — the window exists.

**Read first:** synthesis §13.7.7 "F=4940" subsection + §13.9.1/§13.9.3;
`frame_data_overlay.c` deferred-finalize logic (the `attacker_already_idle` gate
and where finalize waits on `defender_idle`) and the `engine_a_at_atk_idle`
snapshot fields; `corpus-q.yaml` `q-crmk-multimove-merge` entry (the template —
including its comment about why it can't reproduce).

**Procedure:** author `q-jab-multimove-merge` (or several timing variants):
`dist: close, dummy: stand`, `press LP; wait N; press <second button>` with N
scanned so the second press lands inside the 2-frame idle-while-blockstunned
window (close Jab is S=4 A=4 R=6 per synthesis §12, +2 on block, so defender
blockstun outlasts attacker recovery by ~2f; scan N over the
plausible 8–14f range in a scratch corpus, one entry per N, and read the trace
for which N produces the deferred-finalize overlap — the FINAL count and
`move_start_F`/`atk_idle_F` fields discriminate). Then assert: TWO clean FINALs
(the mitigation working — first move finalizes with its own A, uncontaminated) or
document what actually emerges.

**Also in scope (cheap, same trace):** confirm the merged-move *projectile*
contamination residual (`frame_data_overlay.c`, "Residual (documented, not
fixed)" comment in the Phase 6A consume-site block) stays unreachable — the
merge pair here is non-projectile; note in the entry comment that a
projectile-merge pair would need a Ryu-corpus follow-up if one is ever
constructible.

**STOP rules:** if no N in the scanned range opens the window (engine may
re-trigger the same move id differently than the F=4940 chain did), STOP after
the scan and document the negative result with the trace excerpts — the
mitigation stays "unverified, no reachable reproducer", which is itself the
answer; do NOT start modifying overlay code to force the window.

**Files to modify:** `tools/frame-data/corpus-q.yaml` (new entry/entries),
synthesis §13.8 open table row 1 + §13.9.3 (verified / not-constructible update).

**Success criteria:** either (a) a committed corpus entry that exercises the
merge window (verified from trace: second MOVE_START before first FINAL, or the
documented deferred-finalize shape) and passes with the mitigation's expected
values — plus a mutation check (force the `engine_a` snapshot selection off →
entry flags), or (b) a committed negative-result write-up + the scratch-scan
methodology, with the corpus unchanged. Envelope E1–E2 (corpus grew: Q becomes
70+ entries — update the expected counts in the tracker and in E1 wherever this
plan states them).

**Dependencies:** best after Step 2 (a #17 fix may perturb Jab's A accounting on
contact — do not scan against a moving target). **What NOT to do:** don't
re-ship §13.9.1/§13.9.2 force-finalize code under any circumstances; don't relax
the checker's `finals:` semantics. **Failure fallback:** outcome (b) is a
complete, acceptable deliverable.

### Step 8 — Issue #16: bounded reproduction of the close-range spurious FINAL `[/implement]` (size S)

**Why.** Synthesis item 16: one harness session produced `HIT S=2 A=1 R=34
adv=+101` from a bare `press UP` / `LP+LK` at ≤~56px; never reproduced in
isolation. It's the only "we saw garbage once" item left, and it borders the
throw-event edge (`dp->r1==0 && dn->r1 ∈ {2,3}`), so it may be a real
mis-detection with a narrow context precondition.

**Read first:** synthesis item 16 + the §13.8 "Aside, unconfirmed" paragraph;
`frame_data_overlay.c` MOVE_START classifier (the `r1: 0→4/2/3` filter) and
throw-event edge; `corpus-q.yaml` `q-jump-none` and `q-throw-hit` (S=2 A=1 —
note the spurious FINAL's S=2 A=1 matches the throw shape; adv=+101 matches the
KD convention — hypothesis: a phantom/real throw-detection against some prior
residual state).

**Procedure (timeboxed):** author a scratch corpus (NOT committed) that
brute-forces context: for each of {press UP, press LP+LK} × dist {24, 40, 56} ×
preceding-entry-outcome {none, HIT-with-KD, BLOCK}, one entry, generous waits.
Run twice (determinism means one reproduction is enough to keep). If reproduced:
identify from the trace what MOVE_START fired (r1 edge, kow, cgix) and file it
either into the atk=1-noise family or as a new numbered synthesis item with the
reproducer promoted into `corpus-q.yaml` as an xfail (or a fixed entry if the fix
is a one-line classifier tightening whose blast radius is provably nil —
mutation-check it).

**STOP rules:** hard timebox — one scratch-corpus matrix (≤ ~20 entries) and one
follow-up refinement matrix. No reproduction ⇒ STOP, downgrade the synthesis item
to "not reproducible under systematic scan (matrix cited), keep out of corpus",
and close it. Do not touch overlay code without a deterministic reproducer.

**Files to modify:** `docs/frame-data-synthesis.md` item 16 (outcome either way);
`tools/frame-data/corpus-q.yaml` only on reproduction.

**Success criteria:** item 16 is no longer "unconfirmed" — it is either
reproduced-and-filed (with corpus reproducer) or systematically-not-reproduced
(with the scan matrix recorded). Envelope E1–E2 if the corpus changed.

**Dependencies:** none. Run after Step 3 if convenient (the dm_stop predicate
change could theoretically interact; re-run the matrix after Step 3 if Step 3
shipped a predicate widening). **What NOT to do:** no speculative classifier
changes. **Failure fallback:** the negative-scan write-up.

### Step 9 — Harness-gap register (docs-only consolidation) `[direct edit]` (size S)

**Why.** Six real harness limitations are documented only as corpus comments
(inventory item 8 above lists them with their exact locations). Fresh agents
authoring new corpora (Steps 10–13) must find them in one place instead of
rediscovering each by tripping over it.

**Read first:** the six cited comment blocks (listed in inventory item 8), plus
`compile_corpus.py`'s tunables section (DIST_MAX≈300 clamp, timing constants).

**Files to modify:** `docs/frame-data-synthesis.md` — add a compact
"Harness limitations register" subsection under §12.0 (or as §12.1): one row per
gap — mechanism, evidence citation (corpus file + entry/comment), workaround
(e.g. "order distance-sensitive WHIFF entries before connecting dash specials";
"WHIFF predecessor before teleport-sensitive entries"), and status (accepted /
wants-follow-up). Include: P-teleport no-op during knockdown tails; post-dash
teleport corruption + momentum bleed; DHA long-charge recipe unknown; SDB
HIT/BLOCK unreachable (no airborne dummy); fireball WHIFF unreachable; hadouken
adv plateau boundary 121–149 unscanned; merged-move projectile residual (cite the
`frame_data_overlay.c` comment and Step 7's outcome).

**Success criteria:** every row cites its primary source; no new claims beyond
what the cited comments state; corpus files gain a one-line pointer to the
register in their headers (optional but preferred). No code changes — envelope
not required beyond E1 on the corpora if headers were touched (comments don't
affect compilation, but run Q once as a sanity check).

**Dependencies:** after Step 7 (so its outcome is recordable). **What NOT to
do:** don't attempt to fix any gap here; don't delete the original corpus
comments (they stay as local context). **Failure fallback:** n/a (mechanical).

---

## Part C — Coverage rollout

### Step 10 — Corpus-authoring template (the repeatable recipe) `[/plan]` (size M)

**Why.** 17 characters remain. Ryu and Hugo each burned a session's worth of
empirical discovery (motion probing, distance calibration, ordering traps). The
rollout must not re-learn those lessons 17 times. This step distills the process
into a checklist-style runbook so each subsequent character is a bounded,
sonnet-executable task.

**Read first:** `corpus-ryu.yaml` + `corpus-hugo.yaml` headers end-to-end (they
ARE the raw material), `compile_corpus.py` (schema docstring, CHARACTER_IDS map,
motion macros, tunables), `check_frame_data.py` header (verdict semantics),
commit message of `4fe2350e` (oracle fetch + md5-pin discipline), Step 9's
register.

**Files to modify:** new `tools/frame-data/CORPUS-AUTHORING.md` (tool-adjacent
runbook; an exception to the no-new-docs default because Steps 11–13 and 15+
future characters consume it — get orchestrator sign-off implicitly via this
plan). Content (each item cites precedent):
1. **Oracle fetch:** download `<char>.json` from Coccis77/thirdstrikedatabot
   (same source as q/ryu/hugo), record md5 in the corpus header, verify the JSON
   parses and every targeted move Name exists; never edit the oracle file.
2. **Corpus skeleton:** `character:`/`oracle:`/`inter_entry_wait:` keys; the
   standard entry ladder (six-button close BLOCK → HIT sample → WHIFF sample at
   250 → crouching → command normals → character-specials → negative control),
   copied from corpus-hugo's shape.
3. **Empirical pinning discipline:** every `expect` verified against a live
   trace before commit; `from-qjson` wherever the oracle field is numeric;
   explicit literal + comment for range-string/empty fields (Meat Squasher / DHA
   precedents); divergent entries = xfail with the arcade value kept in expect
   and the measured value in the xfail string; cite the issue family.
4. **Known traps:** isolated single-entry re-verification for anything
   suspicious (Hugo label-bleed lesson), predecessor-outcome ordering, teleport
   clamps, motion-probe method (S-value matching against the oracle; try
   existing macros first, then `hold`-chains; extend `_motion_steps` ONLY if no
   expressible sequence lands the move).
5. **Acceptance bar per character:** corpus exits 0; determinism (two
   byte-identical runs); Q/Ryu/Hugo corpora unchanged; xfail-rate accounting
   with the >50%-of-section STOP rule (Hugo grab precedent — orchestrator/user
   sign-off required to accept a high-xfail section as audit output).
6. **Issue-family re-check list:** every new character re-checks #17's reach
   (per synthesis item 17's closing instruction), cr.LK/#14, HIT-R/#15 members
   — classify new divergences into existing families before inventing new ones.

**Success criteria:** the runbook exists, cites everything, and Step 11 executes
against it without needing corpus-ryu/hugo archaeology (that's the test — Step
11's agent reports any gap in the runbook back into it).

**Dependencies:** Steps 2–5 ideally done first (so the family taxonomy the
runbook references is stable), Step 9. **What NOT to do:** don't inline the
whole synthesis doc; don't promise CI (still out of scope, no assets on
runners). **Failure fallback:** n/a.

### Step 11 — Ken corpus `[/implement]` (size M)

**Why Ken next:** (1) canonical shoto whose motion set is already fully covered
by the existing macros — `press`, `motion qcf` (Hadouken: exercises lever D's
arcade-split machinery on a second character with zero compiler work), same
close/far normal structure as Ryu, so it isolates "new animation data" as the
only variable — the cleanest possible #17-reach probe; (2) Ken is the game's
most-played tournament character, and this is a training-mode tool for a MiSTer
3rd Strike port — audit value tracks what users actually practice; (3) P2=Ken is
already the harness preset's default dummy (`training-frame-data`,
`test_runner.c`), so Ken's character data is battle-tested in-harness.

**Read first:** `tools/frame-data/CORPUS-AUTHORING.md` (Step 10), `corpus-ryu.yaml`
(the structural template — Ken mirrors it nearly 1:1).

**Files to modify:** new `docs/arcade-frame-data/ken.json` (fetched, md5-pinned in
the corpus header), new `tools/frame-data/corpus-ken.yaml`. Nothing else.

**Scope:** normals ladder per the runbook + Hadouken BLOCK/HIT ×3 strengths +
throw + negative control. Shoryuken/Tatsu: include ONLY if a probe lands them
with existing primitives (DP-shape `hold`-chain per the Hugo SDB precedent —
budget one probe session); otherwise document as out-of-scope in the header, no
new macros. NO supers/EX (Step 13 owns scoping those).

**Success criteria:** `run.sh tools/frame-data/corpus-ken.yaml` exit 0,
deterministic; every divergence xfail-classified into the existing family
taxonomy (post-Step-2/3/5 this should mostly mean *no* #17 xfails — if Ken shows
fresh A-undercounts, that's a Step 2 regression signal: STOP and report rather
than xfail-spamming); Q/Ryu/Hugo corpora untouched and still green; runbook
amended with anything Ken taught.

**Dependencies:** Step 10 (hard), Steps 2/3/5 (soft but strongly preferred —
otherwise Ken inherits ~10 pre-classified xfails and the plan's counts churn).
**What NOT to do:** no compile_corpus.py changes unless the probe proves a
required move is inexpressible AND the runbook's macro-extension criterion is
met; no oracle edits. **Failure fallback:** if >50% of a section xfails, stop
that section and escalate per the runbook rule.

### Step 12 — Chun-Li corpus `[/implement]` (size M)

**Why Chun-Li third:** (1) the other perennial top-tier pick and THE most common
training-drill target in 3S (hit-confirm practice into her SA2 is the canonical
drill; her cr.MK/normals are what training-mode users measure) — maximum user
value per entry for this MiSTer training tool; (2) engine breadth: a different
rig/animation-data family from the shoto twins (re-checks #17's
animation-data-dependence from a new angle) and Kikouken gives the projectile
arcade-split a non-QCF motion variant (probe with existing primitives — the
runbook's motion-probe method — before touching any macro); (3) her SA2 is the
natural first target for Step 13's super scoping. Yun was considered and
deferred: his value is concentrated in Genei-Jin (super) + dive-kick (airborne)
— both currently unscriptable/unscoped, so the assertable surface is thin until
Step 13 lands.

**Read/modify/criteria/fallback:** identical shape to Step 11 with
`chunli.json` / `corpus-chunli.yaml` (`character: chunli`, id 15 per
`compile_corpus.py` CHARACTER_IDS). Kikouken entries only if the motion probe
lands them (document either way).

**Dependencies:** Steps 10–11 (Ken first — validate the template on the easy
case before the novel one). **What NOT to do:** same as Step 11.

### Step 13 — EX moves + supers: scoping spike `[/plan]` (size M)

**Why.** The oracle JSONs contain EX and super rows the harness has never
attempted. Meter is the blocker; the plumbing partially exists and must be
verified before any corpus promises are made.

**Verified starting points (this plan's investigation):**
- `--test-p1-super-full` CLI flag exists (`src/args.c`, `OPT_BOOLEAN` bound to
  `configuration->test.initial_super_full`) and is applied by
  `apply_initial_super_full_overrides()` (`src/test/test_runner.c`, search the
  function name) — but only within the first 2 gameplay frames (`game_frame > 2`
  early-return) and only fills P1's gauge once.
- Scene-preset machinery has a repeat-super refill concept
  (`apply_scene_preset_super_refill_overrides()`, same file) used by the
  `*-repeat` perf presets — precedent for mid-run refills.
- Training mode itself may auto-refill meter (the real game's training regen) —
  UNVERIFIED; the spike's first measurement.

**Procedure:** (1) measure whether `--test-p1-super-full` + `training-frame-data`
preset yields usable meter at script time, and whether meter regenerates between
entries in harness training mode; (2) probe ONE EX move (e.g. Ken EX Hadouken,
QCF+PP — two-button chord already expressible) and ONE super (Chun SA2 or Ken
SA3, 2×QCF+button — expressible as chained qcf macros or hold-chains) in a
scratch corpus; (3) check what the oracle rows for EX/supers actually contain
(many may be "-" strings → nothing assertable, same as hadouken A); (4) write
the scoping verdict: what's assertable, what plumbing (if any) is missing (e.g.
a `--test-p1-super-refill-every-entry` flag or a `G`-directive-style refill
script primitive), and the cost of adding it (`input_script.c` `#if DEBUG`
plumbing per the harness plan's hard requirement 5).

**STOP rules:** this is a spike — no shipping corpus entries, no engine/CLI
changes; if meter can't be arranged with existing plumbing, the deliverable is
the plumbing proposal, not the plumbing.

**Files to modify:** this plan (findings appended under the step) or a synthesis
§12 note. **Success criteria:** a go/no-go per (EX, super) with measurements,
and — on "go" — concrete follow-up step definitions appended to the tracker.
**Dependencies:** Steps 10–12 (character corpora exist to hang entries off).
**What NOT to do:** don't build meter plumbing speculatively.
**Failure fallback:** documented no-go.

#### Step 13 findings (2026-07-08, scratch spike, no shipped corpus/engine/CLI changes)

**Method.** All measurements below used the same-binary protocol: `cmake
--build build/host --parallel 8` (clean tree, commit `f4851c0c`), then the
compiled `3S-ARM` binary invoked directly (not via `run.sh`, which has no CLI
surface for extra flags) with `FRAME_TRACE_PATH=... SDL_VIDEODRIVER=dummy
SDL_AUDIODRIVER=dummy ./3S-ARM --test-enable --test-scene-preset
training-frame-data --test-input-script <script.fdi> --test-pin-rng
--test-p1-character 11 [--test-p1-super-art 2] [--test-p1-super-full]`, with
`script.fdi`/`expected.json` produced by `compile_corpus.py` from scratch
corpora kept only in the session scratchpad, never committed —
`tools/frame-data/` and `docs/arcade-frame-data/` are untouched by this
spike. Character: Ken (id 11, `ken.json`).

**1. `--test-p1-super-full` + `training-frame-data` DOES yield usable,
oracle-exact meter at script time.** Measured a Ken EX Hadouken (motion
`hold DOWN 2; hold DOWN+RIGHT 2; hold RIGHT+LP+HP 2; wait 1` — see item 2
below for why this hand-rolled chain was needed instead of the `motion`
macro) at `setup.dist:close` (56px) vs a standing dummy, BLOCK outcome:
- **With** `--test-p1-super-full`: trace FINAL `S=11 A=5 R=38 adv=-6` —
  exact match to `ken.json`'s `"Hadouken EX"` row (`Startup=11, Recovery=38,
  Block_advantage=-6`).
- **Without** the flag (identical script, identical LP+HP chord, no stock
  available): FINAL `S=11 A=3 R=38 adv=-11` — exact match to the regular
  (non-EX) `"Hadouken Jab/Strong/Fierce"` rows (`Block_advantage=-11`,
  shared across all three strengths per `ken.json`), i.e. the simultaneous
  LP+HP press silently fell back to a single-strength Hadouken when no
  stock was available, rather than failing to come out at all.

  This confirms both that the flag delivers real, spendable meter at the
  exact moment the script needs it (frames 0-2 of gameplay, verified against
  `test_runner.c:1339-1342`'s `game_frame = 0` reset coinciding with
  `InputScript_Tick` starting — `apply_initial_super_full_overrides()`'s
  `game_frame > 2` window, `test_runner.c:990`, lines up with the script's
  own first frames) and that the harness can *distinguish* EX from non-EX
  by the resulting numbers, which is the whole point of scripting an EX
  probe.

**2. `motion qcf LP+HP` is NOT expressible — confirmed as a compile error**
(`error: entry 'bad-chord', command 'motion qcf LP+HP': unknown
button/direction token 'LP+HP'`), because `cmd_motion`
(`compile_corpus.py:203-222`) calls `bits_for_token()` (single-token lookup)
on the button argument, not `parse_chord()` (which supports `+`-joined
chords and is what `cmd_hold` uses). **Workaround, zero compiler changes:**
a manual `hold`-chain reproducing `_motion_steps("qcf")`'s step sequence
(`DOWN`, `DOWN+RIGHT`, then the final step merged into `RIGHT+LP+HP` as one
`hold` chord, `+ wait 1` release) lands the EX exactly, per the runbook's
own "hold-chain before compiler change" method (`CORPUS-AUTHORING.md`
Phase 5, "Motion-probe method", Hugo 360-grab precedent). So: two-button EX
chords are assertable today with existing primitives, just not via the
`motion` verb directly — a corpus author needs to know this workaround
(worth folding into `CORPUS-AUTHORING.md` if/when a real EX corpus section
is written, per the runbook's own self-correction clause).

**3. Ken SA3 (Shipuujinrai Kyaku, oracle name) IS directly expressible via
the existing `motion 2qcf <btn>` macro** — no chord problem here since it's
a single button (HK). Measured (`setup.dist:far`, BLOCK vs standing dummy,
`--test-p1-super-art 2 --test-p1-super-full`): FINAL `S=1 A=9 R=37 adv=-11`.
Oracle `"Shipuujinrai Kyaku"`: `Startup=1, Hit=9, Recovery=27,
Block_advantage=-11`. **S, A(engine_a), and adv match the oracle exactly.
R does not** (measured 37 vs oracle 27, a +10 divergence) — an unexplained,
UNCLASSIFIED R divergence (S/A/adv exact, R differs from the oracle's
single R figure; note it does NOT fit `CORPUS-AUTHORING.md` Phase 6
bucket 2, whose precondition is a HIT-vs-BLOCK/WHIFF relationship — this
was measured on BLOCK); not root-caused here (out of the
spike's scope: this is a genuine open question for whichever step first
ships a real SA3 corpus entry, not a plumbing question).

**4. Meter does NOT regenerate between entries — but a single fill has
more headroom than the plan's own working assumption implied, and the
exhaustion point is exactly measurable.** An 8-entry scratch script (all
identical `motion 2qcf HK` attempts, `--test-p1-super-art 2
--test-p1-super-full`, no other refill mechanism) showed a hard boundary:
- Entries 1-3: FINAL `S=1 A=9 R=37 adv=-11` (the super, exact per item 3).
- Entries 4-8: FINAL `S=30 A=5 R=14 adv=-1`, identical across all five —
  a non-super fallback move (a bare HK press once the QCFx2 gauge/stock
  requirement fails), with **no reversion back to super numbers** even
  four entries later. This proves there is no passive/idle meter
  regeneration in this harness configuration (default zero-filled
  `Training[].contents` — the training-mode "S.A. GAUGE" menu option
  defaults to `0`/EMPTY, `effe3.c:117-121`) — meter, once spent, stays
  spent for the rest of the run.
- The same exhaustion pattern was separately measured for Ken EX Hadouken
  (4 consecutive attempts, `setup.dist:close`, `--test-p1-super-full`,
  default `--test-p1-super-art` (SA1)): all 4 read the exact EX numbers
  (`S=11 A=5 R=38 adv=-6`), i.e. EX headroom is **at least 4** off one
  fill and wasn't exhausted within this session's probe size (consistent
  with `plmain.c:614-626`'s EX-activation code path spending from a
  fractional `gauge.s.h` counter and only decrementing the whole-stock
  `store` when that fraction is insufficient — a materially cheaper cost
  model than a super's flat `store -= 1` (`plmain.c:677`; `plmain.c:726` is
the opposite-sign `store -= -1` increment in `sag_union_1`) in the
  `sag_union_0`/`sag_union_1` handlers reached via
  `sag_union_jump_table`, `plmain.c:1137`). The exact mechanical reason
  SA3 (nominally a single-segment/one-shot super in real 3rd Strike)
  granted **3** activations rather than 1 off `--test-p1-super-full`
  was not traced further (`sag_union_jump_table` dispatch by SA
  kind/index was not fully read) — flagged as an open question, not
  asserted as a mechanism.

**Verdict — Ken EX Hadouken: GO, no plumbing needed.** Directly scriptable
today (via the `hold`-chain workaround, item 2) and oracle-exact on S/R/adv
with the existing `--test-p1-super-full` flag; a real corpus section can
budget multiple EX entries (measured headroom ≥4 uses) off one flag, no new
CLI/engine/script primitive required. A's oracle field is `"-"` (non-numeric,
same as every other Hadouken strength) so it's not asserted, consistent
with existing Hadouken-family convention — not a new gap.

**Verdict — Ken SA3: GO for a single super entry (or up to 3) per script
run, no plumbing needed for that scope; a genuine open R-divergence exists
and is a corpus-authoring/root-cause question, not a plumbing gap.**
S/A/adv are oracle-exact and directly scriptable via `motion 2qcf <btn>`
today. R diverges by +10 and needs its own investigation before a real SA3
corpus entry ships (per the runbook's "unexplained divergence is a STOP,
not an xfail" rule, Phase 4) — that investigation is out of this spike's
scope.

**Plumbing proposal (not built — STOP rule), for if/when a corpus needs
*more* super/EX entries than one fill's headroom covers** (e.g. a section
wanting BLOCK+HIT+WHIFF for both an EX move and a super, potentially
exceeding the measured 3-super/≥4-EX budget once interleaved with other
moves that might also draw down `store`): add a new one-letter `.fdi`
directive (e.g. `S`, following the exact precedent of the existing `G`
directive) that calls `tr_spgauge_cont_init2(0)` — the same function
`apply_initial_super_full_overrides()` already calls
(`test_runner.c:1003`/`1017`) — on demand, mid-script, rather than only in
the `game_frame <= 2` window. Shape of the change, all `#if DEBUG`-scoped
per the harness's hard requirement 5:
- `src/test/input_script.c`: one new `InputScriptDirectiveType` enum value,
  one parse branch in `parse_directive_line()`, one ~10-line
  `input_script_apply_super_fill()` function mirroring
  `input_script_apply_guard_mode()`'s shape (same file, `input_script.c:130-157`).
- `tools/frame-data/compile_corpus.py`: one new per-entry corpus key (e.g.
  `refill_meter: true`) emitting the new directive line before the entry's
  `W` lines, analogous to how `setup.dummy` emits the `G` directive today.
- No `args.c` or engine (`sf33rd/`) changes needed at all — `tr_spgauge_cont_init2`
  is already a plain (non-`#if DEBUG`) exported function
  (`spgauge.h:61`), so this is purely a harness-side (`src/test/`) addition.
- **Cost estimate:** small — same order of magnitude as the existing `G`
  directive (~30 lines including the doc comment), reviewable in a single
  `/implement` loop pass. Not built here per the spike's STOP rule; only
  needed if a future character's real corpus section proves the
  measured per-fill headroom (item 4 above) insufficient.

---

## Part D — Hardware

### Step 14 — MiSTer-side manual verification pass (user-assisted) `[checklist, user-driven]` (size S)

**Why.** Every Phase 4–6 change is host-verified only. The overlay's actual
rendering on real hardware — KD text, projectile numbers, honest T — needs one
human pass on the MiSTer. The harness cannot do this (headless host sim; §12.0
demotes the manual protocol to exactly this purpose).

**Read first:** `docs/mister-runbook.md` (MANDATORY before any deploy —
`feedback-read-runbooks-before-deploy.md`), `AGENTS.md` Safety section,
`docs/frame-data-synthesis.md` §12.0.

**Protocol (agent prepares, user executes on hardware):**
1. Build: `tools/mister/build-game.sh --flavor telemetry` (telemetry always —
   `feedback-always-telemetry`). Do NOT deploy anything without the user's
   explicit go-ahead in-session; when given, follow the runbook's deploy-wrapper
   path (`tools/mister/misterctl.sh deploy-wrapper --src build/mister-release/stage
   --artifacts-only`; RBF only ever to `/media/fat/_Other/`, never `menu.rbf`;
   never `rsync --delete` — `feedback-no-rsync-delete.md`).
2. Hand the user this checklist (training mode, FRAME DATA option ON in the
   training menu):
   - [ ] Overlay toggles on/off from the training options FRAME DATA row (added
         in `118f7350`) and OFF leaves no `/tmp/3sx-frame-trace.log` growth
         (Phase 5 gating).
   - [ ] Q cr.HK on hit: advantage renders **KD** (purple/throw-color), not a
         green "+N" (Phase 4.3).
   - [ ] Q CnDB on hit: KD renders; R reads 42/44/46 for LK/MK/HK (Phase 4.4).
   - [ ] Ryu (P1) Hadouken vs blocking dummy: S=10 R=36 on the numeric line,
         projectile flight visible in the meter, adv −9 at point-blank (Phase 6A).
   - [ ] Q HSB blocked: T shows the full measured duration (> S+A+R sum) — the
         honest-T convention (Phase 4.5), numeric line visually sane.
   - [ ] UOH pressed twice ~38f apart: single FINAL, A=11 (§13.9.4 fix +
         §13.11 declared-truth convention, updated 2026-07-07 — was A=10
         pre-convention) — spot check the on-screen A.
   - [ ] General: overlay text legible on the user's CRT/output path; no frame
         pacing regressions with overlay ON (show-fps overlay if in doubt —
         headless numbers don't count, `feedback-headless-perf-unreliable.md`).
3. Record results in synthesis §12.0 as a dated hardware-pass note. Any FAIL
   becomes a new tracker row, not an on-the-spot fix.

**Success criteria:** user-confirmed checklist, recorded. **Dependencies:** after
Parts A–B land (so hardware verifies the final behavior; don't burn a user
session per step — `feedback-friend-build-distribution.md` spirit).
**What NOT to do:** no deploys/pushes/releases without explicit user consent
(`feedback-no-premature-release.md`); no CRT service-menu suggestions if
geometry looks off (`feedback-no-crt-service-menu.md`). **Failure fallback:**
failed items filed as tracker rows with the user's observations quoted.

---

## Sequencing and value order

Recommended execution order: **1 → 2 → 3 → 6 → 4 → 5 → 7 → 8 → 9 → 10 → 11 → 12
→ 13 → 14.** (Step 6 is independent and may interleave anywhere; Steps 4–5 need
Step 2's dust settled; coverage waits for the family taxonomy to stabilize so new
corpora don't xfail-churn.)

## Progress tracker

| Order | Step | Skill | Size | Status |
|---|---|---|---|---|
| 1 | Issue #17 diagnosis — fix-mechanism selection with falsification gates | `/plan` | M | RESOLVED 2026-07-07 — user adopted escalation option 2 (declared-truth convention, §13.11) |
| 2 | Issue #17 fix — implement + flip ~16–18 xfails + new mutation lever | `/implement` | L | DONE 2026-07-07 (Step 2b) — restore lever shipped; 11 Ryu xfails flipped (incl. crlk, census-resolved shape (b)); Q UOH re-baselined to A=11 + q-uoh-whiff anchor + q-uoh-whiff-r xfail probe (item 18); Hugo untouched; levers re-derived (A→∅, B→22, new F); Q 73 (72 PASS / 1 XFAIL), Ryu 34/3, Hugo 24/6 |
| 3 | Issue #14 — cr.LK dm_stop event classification | `/plan` + `/implement` | M | DONE 2026-07-07 — dm_stop edge widened to `!= 0`; q-crlk-block/hit PASS, ryu-crlk-block narrowed to #17-only clause; Q 62/7, envelope+levers verified |
| 4 | Hugo Roundhouse contact-R +2 — classify | `/plan` | S | DONE 2026-07-07 — classified: issue-#17 family (cg_extdat=0x80), R-side manifestation on landing-clocked move; xfails stand citing synthesis item-17 Hugo addendum |
| 5 | Issue #15 — HIT-R family classification + convention decision | `/plan` + `/implement` | L | DONE 2026-07-07 — option (b) both subfamilies; normative synthesis §13.10; 15 xfails -> PASS + 3 new anchors; Q 71/0, Ryu 23/14, Hugo 24/6; lever A grew 3->5 (documented) |
| 6 | argparse OPT_BOOLEAN 4-byte RMW fix | `/implement` | S | DONE 2026-07-07 — lib fixed (single-byte write); ASan before/after + 10x sparse-effect-save determinism; harness green |
| 7 | Multi-move-merge reproducer (Q Jab +2 pair) | `/implement` | S–M | DONE 2026-07-07 — outcome (b): F=4940 precondition reproduced (Jab N=17) but contamination window structurally unreachable for button-normal pairs; no corpus entry (can't pass its own mutation bar); write-up in synthesis §13.9.3; one residual pair (+4-first + throw-second) noted unscanned |
| 8 | Issue #16 — bounded spurious-FINAL reproduction scan | `/implement` | S | DONE 2026-07-07 — closed: LP+LK half = real Q Throw connecting (21/21, hitbox past nominal range); UP half not reproducible under 38-entry systematic scan; corpus unchanged |
| 9 | Harness-gap register (docs consolidation) | direct edit | S | DONE 2026-07-07 — synthesis §12.1, six rows, all source-cited; corpus headers point at it |
| 10 | Corpus-authoring template runbook | `/plan` | M | DONE 2026-07-08 — tools/frame-data/CORPUS-AUTHORING.md (8 phases; whiff-vs-table rule + post-convention decision tree; oracle URL pattern verified; Gill has NO oracle json) |
| 11 | Ken corpus | `/implement` | M | DONE 2026-07-08 — corpus-ken.yaml 48 entries (35/13) exit 0; SRK+Tatsu item-18 sections accepted w/ orchestrator sign-off (user review pending); 2 UNCLASSIFIED new #17-family variants flagged (ken-forward A-overcount, ken-twdshk R-deficit); no lever-F regression; runbook amended w/ 5 lessons |
| 12 | Chun-Li corpus | `/implement` | M | DONE 2026-07-08 — corpus-chunli.yaml 49 entries (36/13) exit 0; Kikkoken+SBK clean (lever-D 3rd character); UOH resolved to §13.11 declared A=14 (whiff-persistent class); Hazan Shu section GRANTED (item-18 + shape-a1); chunli-forward R-deficit UNCLASSIFIED (2nd character w/ ken-twdshk-like shape); Hyakuretsu not landable |
| 13 | EX/supers scoping spike | `/plan` | M | DONE 2026-07-08 — spike run, verdicts GO (EX) / GO (supers, ≤3 per script run): --test-p1-super-full delivers spendable meter; Ken EX Hadouken oracle-exact; SA3 S/A/adv exact with an UNCLASSIFIED R+10 open question; NO passive meter regen (8-entry exhaustion probe); qcf-chord compiler gap documented w/ hold-chain workaround; ~30-line refill-directive proposal recorded (not built). See "Step 13 findings". **DATED CLOSURE (2026-07-12, EX-CONTACT-R):** the R+10 open question is ROOT-CAUSED, not merely open — see the EXSUPER-1 row's EX-CONTACT-R disposition below (M1 — FD_METER_LEN=72 window misplacement; true non-frozen recovery span = 27 = oracle exactly, confirmed on both the legacy trace and a 2026-07-12 isolated re-run on the current binary's real box_a/busyr/box_runs fields). Displayed R=37 stays a wrong-window value, not an engine truth — no corpus entry exists for Ken SA3 to flip. |
| 13a | Follow-up (defined by Step 13, GO): EX sections for existing corpora — Ken EX Hadouken first (assertable today via --test-p1-super-full; needs run.sh flag passthrough or hand-run protocol; hold-chain chord workaround until/unless cmd_motion learns chords) | `/implement` | S–M | NOT STARTED |
| 13b | Follow-up (defined by Step 13, GO): supers — root-cause the SA3 R+10 UNCLASSIFIED divergence first, then ship ≤3-super sections per corpus (no refill plumbing needed at that scope; the ~30-line .fdi refill directive proposal applies beyond it) | `/plan` + `/implement` | M | **ROOT-CAUSE HALF DONE 2026-07-12 (EX-CONTACT-R)** — the SA3 R+10 divergence this row exists to unblock is now root-caused (M1, see row 13's dated closure and the EXSUPER-1 row below); the ≤3-super-sections-per-corpus authoring half of this row remains NOT STARTED. |
| C-Yun | Cast rollout: Yun corpus (user-directed 2026-07-08) | `/implement` | M | DONE 2026-07-08 — corpus-yun.yaml 68 entries (45/23) exit 0; 4 ground specials landed (Tetsu Zankou fully clean); UOH reclassified per-path (whiff=item 18, block/hit=cut-window, 1 hedged residual); Nishou 9/9 item-18 section GRANTED; 2 UNCLASSIFIED variants (crfierce R-surplus, Zesshou Fierce boundary shift); dive kick/Genei-Jin excluded (airborne/13b) |
| C-Makoto | Cast rollout: Makoto corpus | `/implement` | M | DONE 2026-07-08 — corpus-makoto.yaml 61 entries (39/22) exit 0; first shape-(b) members on a new character (crjab/twdshk, lever-F restore working); BENIGN NOVEL COMPOUND ruled (crstrong/crshort a1+partial-b); Hayate UNCLASSIFIED sign-flipping R (TOP of user-review list); Oroshi recipe corrected (hcb+P); Fukiage BLOCK/HIT unreachable (SDB-class); 3 >50% sections GRANTED |
| C-Dudley | Cast rollout: Dudley corpus | `/implement` | M | DONE 2026-07-08 — corpus-dudley.yaml 66 entries (44/22) exit 0; Jet Uppercut lever-F irregularity ruled BENIGN w/ structural proof (restore ≡ prior_add inside revoke block); UOH reconciled to §13.11 A=14 (chunli precedent, dwellbrk exclusion retracted); base Jet A-overcount UNCLASSIFIED (HIGH user-review); Cross Counter WHIFF-only, Ducking movement-only; 5 >50% sections GRANTED |
| C-Urien | Cast rollout: Urien corpus | `/implement` | M | DONE 2026-07-08 — corpus-urien.yaml 64 entries (41/23) exit 0; Metallic Sphere all-PASS (lever-D 4th character); NEW mechanism found: same-cell self-loop accumulator blind spot (UOH: 9 entry-credits + 2 uncredited self-loop actives = active_pf 11; below-oracle A floored to UNCLASSIFIED-pending, NOT asserted as §13.11); review caught 5 stale-pinned entries incl. a masked SHAPE failure (headbutt-hp-hit re-authored at verified dist=70); Chariot RH distance-dependent a1 (A=6 close / 15 far); VKD three-way R UNCLASSIFIED (HIGH review); 5 >50% sections GRANTED post-fix |
| C-Akuma | Cast rollout: Akuma corpus | `/implement` | M | DONE 2026-07-08 — corpus-akuma.yaml 54 entries (41/13) exit 0; cleanest audit yet (zero P-1s); Hadouken lever-D 5th character; 9 shape-(b) members restored; Hyakki Shuu fully assertable; Ashura Senku recipe found (DP hold-chain + 3P chord), R asserted w/ measurement-semantics xfail per review ruling; SRK/Tatsu item-18 sections GRANTED (Ken precedent); 2 UNCLASSIFIED (close-RH, twdsmp — ken-twdshk analogues) |
| ENGINE-1 | §13.5.1a recovery-cut misfire fix (lever G) — found by Oro's review trace-gate | `/plan` + `/implement` | L | DONE 2026-07-08 — cut anchor was a cghi level-test; now requires a grounded fresh edge into cghi=1 at-or-after the reset. 31 xfails flip arcade-exact (28 in-commit + 3 Oro deferred), 17 re-cites, item 18 split (resolved/residual/yun-overshoot); all protected walls + levers A-G verified by orchestrator; E4 clean |
| C-Oro | Cast rollout: Oro corpus (whose review trace-gate found ENGINE-1) | `/implement` | M | DONE 2026-07-08 — corpus-oro.yaml 52 entries (46/6) exit 0 post-cut-fix; Niou Riki + Jinchu whiff arcade-exact (Class-3 axiom vindicated); Nichirin lever-D 6th character; source-grounded name mapping (plpat09.c); Jinchu BLOCK/HIT R=0 residual UNCLASSIFIED (GRANTED); Oniyama negative-oracle-R incomparability; D-claim corrected (Gate 2) |
| C-Yang | Cast rollout: Yang corpus | `/implement` | M | DONE 2026-07-08 — corpus-yang.yaml 47 entries (29/18) exit 0; first character authored fully post-cut-fix; FIRST-EVER S-divergence found (yang-forward S=5 vs 7, trace-verified first_active_raw=5 vs event_raw=7, UNCLASSIFIED); UOH A re-cited to Makoto compound bucket; Senkyuutai cut=1-overshoot UNCLASSIFIED; 4 specials landed first-probe; UOH section GRANTED |
| C-Ibuki | Cast rollout: Ibuki corpus | `/implement` | M | DONE 2026-07-08 — corpus-ibuki.yaml 58 entries (42/16) exit 0; widest command-normal coverage (6 rows); UOH A=11 asserted per §13.11; Raida is a BLOCKABLE command grab (novel); Kubiori motion corrected empirically (hcf+P) w/ two-phase HIT incomparability; twdsroundhouse joins the Hugo-RH item-4 family; Kazekiri item-18(c) shape; 3 >50% sections GRANTED |
| C-Alex | Cast rollout: Alex corpus | `/implement` | M | DONE 2026-07-08 — corpus-alex.yaml 59 entries (47/12) exit 0; Power Bomb/Spiral DDT unblockable Class-3 grabs clean; Air Knee Smash motion corrected (DP not charge); UOH three-way A-variance UNCLASSIFIED; Air Stampede trace-ruled ENGINE-2 (new §13.5.1a-class false-cut shape, deferred) |
| ENGINE-2 | §13.5.1a-class false-cut shape #2: full-recovery-under-cghi=1 landing moves (found by Alex review trace gate) | `/plan` + `/implement` | M | DONE 2026-07-09 — fixed by §13.5.1b guard-rearm commit gate (lever H): the true discriminator is the engine's own `guard_flag` (re-arms 3→0 on a TRUE cleanup tail, stays 3 on a false one), not the naive "does the true r1 edge land exactly on arcade's own sum" test that missed sean-ryuubi and originally motivated the ENGINE-2 deferral. **9-entry disposition:** `alex-airstampede-lk-whiff` FLIPS (R 2→24, arcade-exact); `elena-scratchwheel-lk-whiff/-block/-hit` FLIP (R 25→31, arcade-exact, uniform); `remy-rrf-lk-whiff` FLIPS (R 29→32, arcade-exact); `remy-rrf-lk-block/-hit` R clause resolves (29→32) but stay xfail on the separate, untouched stacked A+1 clause (§12.2.3 candidate); `alex-airstampede-lk-block/-hit` are UNAFFECTED (cut=0 the whole move — the contact branch jumps the chart forward at landing instead of resetting it, so the cut machinery never arms at all) and are re-cited from ENGINE-2 to §12.2.2 contact-branch recovery shortening (values unchanged, R=19 vs arcade 24, adv already exact). Net: 7 resolved via the fix (5 full flips + 2 R-clause-resolved), 2 re-cited to §12.2.2. **Sweep bonus:** the same fix resolved 26 further windows hiding inside item 18(b) (15 full flips + 11 narrowed to a residual clause — includes the `sean-ryuubi` family, previously mis-cleared of ENGINE-2 pre-fix by the same since-falsified naive sum test; see that item's 2026-07-09 update and `corpus-sean.yaml`'s CORRECTED note). Full 19-corpus verification: all corpora exit 0 with the predicted splits; determinism byte-identical; lever H mutation flags exactly the predicted set and nothing else, Q unaffected. |
| C-Elena | Cast rollout: Elena corpus | `/implement` | M | DONE 2026-07-08 — corpus-elena.yaml 57 entries (42/15) exit 0; Scratch Wheel trace-confirmed as ENGINE-2's SECOND member (re-cited from 18(b) by review); Lynx Tail landing-whiff CLEAN (fix working as designed); UOH recurs in the Alex bucket (2nd member); Lynx Tail compound + crRH item-4 sign-mirror UNCLASSIFIED; Mallet Smash overhead-label contradiction recorded; oracle has NO Far rows + NO Common_name; 3 sections GRANTED |
| C-Remy | Cast rollout: Remy corpus | `/implement` | M | DONE 2026-07-08 — corpus-remy.yaml 56 entries (34/22) exit 0; Light of Virtue = lever-D 7th character + FIRST charge projectile, high/low empirically proven (crouch probes); Rising Rage Flash = ENGINE-2 3rd member; crfierce-hit exposes ENGINE-3 (strike-KD predicate timing race); crRH adv=-41 UNCLASSIFIED; 3 sections GRANTED |
| ENGINE-3 | Strike-KD predicate timing-race gap: fd_is_knockdown_at_atk_idle misses KDs whose defender last raw cell sits outside r2∈{16,19} at atk-idle (found via remy-crfierce-hit; §13.6.2-anticipated class) | `/plan` + `/implement` | S–M | **DONE 2026-07-09 — Design E3-L: tick-side strike-KD latch (`FdMove.strike_kd_seen`) sampling every tick of the finalize-deferred window, over a measured down-family set GROWN to `{14..23}∪{27,28}` (`fd_r2_is_down_family()`, new lever anchor for lever E; new lever I gates the latch itself) — see `docs/frame-data-synthesis.md` §13.6.2b for the full mechanism, census, and mutation verification. CORRECTION to the original diagnosis text above: remy-crfierce-hit's last raw[] cell was hand-decoded as `P2.r2=1` (a genuine timing-race read); the code-emitted `dlr2` diagnostic added for this fix's census instead traces `def_r2==18` at that exact point — so remy's flip is via the widened last-cell set alone (lever E), same mechanism class as sean's, not a distinct timing-race sub-shape; the two "sub-shapes" language above is superseded. **Result:** 2 XFAIL→PASS (remy-crfierce-hit, sean-twdshk-hit) + 7 expect kd:0→kd:1 (alex-crfierce-hit, dudley-crroundhouse-hit, urien-crfierce-hit, yang-forward-hit, yun-forward-hit, yun-twdshp-hit, oro-strong-hit — 3 keep an unrelated pre-existing xfail clause) + 26 unasserted golden-kd-column flips, 0 regressions — dissolves the entire "D-notation-vs-live-kd inconsistency" family previously documented across the alex/dudley/urien/yang/yun/oro corpus headers (arcade's "D" was right; the predicate was blind to r2=18). Verified: `run-suite.sh --check-golden` drifted EXACTLY the predicted 35 rows pre-update, zero drift post-update; lever E mutation (helper forced false) FAILs exactly q-fierce/crhk/backhk on Q; lever I mutation (latch off) flags nothing on the checker across all 13 affected corpora, golden drift exactly `urien-vkd-lk-hit` (the latch's single-member load-bearing proof). |
| C-Necro | Cast rollout: Necro corpus | `/implement` | M | DONE 2026-07-08 — corpus-necro.yaml 51 entries (45/6) exit 0, cleanest split of the rollout; NEW class: defender-stance-conditional R (Flying Viper crouch 15 vs stand 19, UNCLASSIFIED); Necro-local throw collision floor (Throw HIT unreachable, documented w/ control test + cross-corpus scope check); 3 of 5 motion guesses corrected empirically; Denji Blast inexpressible (7 attempts); 2 sections GRANTED |
| C-Twelve | Cast rollout: Twelve corpus | `/implement` | M | DONE 2026-07-09 — corpus-twelve.yaml 59 entries (34/25) exit 0; N.D.L. = first traveling multi-spike projectile through lever-D → exposes ENGINE-4 (proj-split S/R decomposition limitation, 9 xfails); A.X.E.-Jab re-cited to Makoto compound (review dissolved a spurious STOP); UOH = 4th recurring-bucket member (byte-identical to Necro); D.R.A. motion unfound (17 attempts); 6 sections GRANTED |
| MODEL-NOTE | Model-policy fallback (2026-07-09): Fable 5 quota exhausted during the ENGINE-2 plan review. Items from ENGINE-2 onward (ENGINE-2/4/3 diagnosis+review) run on Opus 4.8 xhigh as the strongest substitute — WANT A FABLE RE-REVIEW PASS when quota returns. Steps 1–13, reopened Step 2, ENGINE-1, SWEEP-1 were genuinely Fable-reviewed (no re-review needed). | note | — | 2026-07-09 |
| RUN-COMPLETE | Autonomous run finale 2026-07-10: original plan steps 1-13 + user-directed cast rollout (19/19 fetchable characters) + 5 engine-observer fixes (levers F-J, zero gameplay changes) + harness tooling (run-suite/goldens, 52m->9.5m) + classification sweep + Fable re-review (CLEAN). Final gate: 19/19 GREEN, zero golden drift, 836 PASS / 222 XFAIL / 1,058 entries — every xfail classified+cited. Part D (MiSTer hardware pass) remains user-assisted. USER-REVIEW LIST: section grants, Dragon Smash oracle question, §12.2.4 one-offs, LATENT-1. | — | — | DONE |
| TOOL-1 | Harness tooling: run-suite.sh (build-once parallel, 52m->9m28s measured) + golden.py/golden/*.tsv (per-entry drift gate, strictly stronger than the checker) | `/plan` + `/implement` | M | DONE 2026-07-09 — opus plan+diff reviews SHIP; orchestrator dogfood confirmation zero-drift; host-side only, zero src/ changes |
| MODEL-NOTE-2 | Fable quota RESTORED 2026-07-09 — all subsequent agents back on Fable; the single Opus-substitute engine item (ENGINE-2) queued for Fable re-review (task #41, running in a worktree). Re-review COMPLETED CLEAN (see FABLE-RE-REVIEW-1 below) — the Opus-substitute debt is retired, no engine item remains unreviewed by Fable. | note | — | 2026-07-09 |
| FABLE-RE-REVIEW-1 | ENGINE-2 (76e0e38d) Fable re-review: CLEAN, zero defects; Opus-substitute debt retired | check | — | DONE 2026-07-09. Note for the record: 76e0e38d's own commit message says "15 more entries" narrowed to a residual clause; the docs' census (ENGINE-2 tracker row, `docs/frame-data-synthesis.md` §13.8) correctly says 13 — the commit message is immutable, don't chase the discrepancy as a fresh finding. |
| LATENT-1 | Stale-anchor commit on dwell-break-re-entry: on a REFUSED commit, `cghi1_first_frame`/`count>=3` stay armed; if the dwell breaks and cghi re-enters 1 later in the same move, the first gate-passing re-entry tick commits `attacker_idle` at the STALE original anchor (`frame_data_overlay.c`'s §13.5.1 tick-side block — re-entry needs no fresh edge). | check | — | NOT STARTED (no reachable member across all 19 corpora — census populations disjoint; pre-existing shape, window widened by lever H — documentation-only until a member appears). Candidate tightening: reset the dwell counter on `cghi1_dwell_broken`, or require the commit tick to be cghi-contiguous with the anchor. |
| ENGINE-4 | Proj-split S/R decomposition limitation: S=proj_spawn_slot + R=meter_len−spawn anchors valid only for fire-and-forget fireballs (found via twelve-N.D.L.) | `/plan` + `/implement` | M | **DONE 2026-07-09 — Design E4-A′: S' = max(proj_spawn_slot, proj_athok_slot), anchored on the tama's own hit-check-gated arming tick (`fd_engine_proj_hitok`, hitcheck.c's own atix!=0 && att_hit_ok!=0 gate); R', on a chart-natural end (`fd_engine_proj_natend`, gated on the engine's own chart-cut kill-reason latch `fd_engine_proj_cut` never having fired — a tiny helper, `fd_tama_chart_cut()`, called at all 5 kotp_00000/kotp_13000 erase transitions), is meter_len − (proj_firstact_slot + proj_a) instead of meter_len − S'. Both anchors are a byte-identical no-op for all 45 fire-and-forget fireball rows (ryu/ken/akuma/chunli/oro/remy/urien) and reproduce all 9 N.D.L. arcade values exactly (S=15/15/16, R=14/16/20). See `docs/frame-data-synthesis.md` §13.12 for the full mechanism, the R-gate falsification ledger, and TWO implementation-time races found only by direct trace instrumentation (neither anticipated by the original design): (1) `fd_engine_proj_hitok` had to be made edge-triggered (`fd_engine_proj_hitok_armed`), not level-triggered — a WHIFF tama's held-armed flag was leaking into the NEXT move via the MOVE_START slot-0 rescue, corrupting S on 8 of 9 N.D.L. rows; (2) `fd_engine_proj_natend`'s write-time gate raced the engine's own cut recording two different ways, both same-invocation (not cross-tick) hazards: kind-0's own case-0-internal cuts (chart-end/ground/timeout — `fd_tama_chart_cut()` always called strictly after the `char_move()` call that first shows `atix==0`, within the SAME `kotp_00000` invocation), and — on confirmed hits — `set_char_move_init(erase)`'s own internal `char_move()` call (`charset.c:126`, from `eff13.c:365/367/370`) running on the fresh erase chart with `hf.hit_flag` still set, a few statements before `fd_tama_chart_cut()` (`eff13.c:377`) records the cut later in that SAME invocation — fixed by adding a `hf.hit_flag==0` write-time exclusion plus an independent `fd_engine_proj_cut==0` re-check at the overlay's own read time (deliberate defense-in-depth, confirmed by review guard ablation: dropping either guard alone leaves the golden suite green, only dropping both reproduces the race); this second bug was silently regressing exactly 12 fireball BLOCK/HIT rows' R (`ryu-had` 36→34 and `chunli-kik` off by 2, all 6 legs each, nothing else) until caught by re-running the full 19-corpus golden check after the first implementation pass. **Result:** 9 XFAIL→PASS (twelve-ndl-{jab,strong,fierce}-{whiff,block,hit}), 0 regressions across the complete 54-window proj=1 population. Verified: after both race fixes were applied, `run-suite.sh --check-golden` drifted EXACTLY the predicted 9 rows (S/R/verdict) and nothing else; before the fixes, the two races corrupted 8/9 N.D.L. S values (race 1) and 12 fireball R values on `ryu-had`/`chunli-kik` (race 2), caught by direct trace instrumentation plus the full 19-corpus golden check. Lever J mutation (forced 0) flags exactly the 9 twelve-ndl entries at their legacy pre-fix values, all 45 fireball rows byte-identical under both lever settings (load-bearing negative). **Members (sweep census 2026-07-09, exactly the 9 twelve-ndl entries, single character/move):** twelve-ndl-{jab,strong,fierce}-{whiff,block,hit} |
| C-Sean | Cast rollout: Sean corpus (LAST full corpus) | `/implement` | M | DONE 2026-07-09 — corpus-sean.yaml 69 entries (47/22) exit 0; Dragon Smash Strong = likely ORACLE data-entry duplicate (Strong≡Jab row; measured values are exact Jab→Fierce midpoints — USER REVIEW: external source check); ENGINE-3's first captured anticipated-range member (r2=17); Ryuubi ENGINE-2 hypothesis properly trace-refuted; Sean Tackle motion unfound (14 attempts, motion-collision hazard documented); review caught a masked HK-whiff outcome flip + 2 transposed quotes — fixed |
| C-Gill | Cast rollout: Gill — feasibility close-out | check | S | CLOSED 2026-07-09 (fetch-blocked): re-verified live — raw.githubusercontent.com/.../json/gill.json returns 404 and the repo's json/ listing contains exactly the 19 characters now covered; CHARACTER_IDS has gill (id 0) so the harness side is ready if an oracle ever appears; per the runbook rule this is escalate-don't-fabricate — no corpus authored. Cast coverage: 19/19 fetchable characters DONE |
| SWEEP-1 | Cross-cast UNCLASSIFIED classification sweep (post-rollout): re-cite multi-member families, register divergence classes | direct edit + scratch traces | M | DONE 2026-07-09 — three families mechanism-ESTABLISHED via per-tick + credit-ledger scratch traces (charset.c instrumentation, reverted; same-binary protocol): (1) UOH landing-clocked active tail (synthesis §12.2.1; alex/elena/necro/twelve, sentinel + looping-declared shapes, ledger-traced on necro+alex), (2) contact-branch recovery shortening = §13.10 Class 1 generalized-to-contact (§12.2.2; ken-twdshk/akuma-twdsmp trace-proven, akuma-close-RH + chunli-forward signature members), (3) contact-branch declared-credit A-overcount, the "#17 mirror" (§12.2.3; ken-forward ledger-proven — restores sign-coherence with #17; sean-ryuubi/dudley-jetup/twelve-trio/remy-rrf stay candidates). One-off class registry created (§12.2.4: S-divergence, stance-conditional R, sum-preserving A/R shift linked remy-cbk+yun-zesshou, cut-committed whiff R-overshoot, Hayate compound, adv anomalies); item 18(c) membership reconciled (yun/remy/yang UOH R + ibuki-kazekiri; yang-senkyuutai explicitly excluded, cut=1); runbook Phase 6 grew buckets 6-8 + §12.2.4 pointer; ENGINE-2/3/4 member lists sharpened. Strings/comments only — all 19 corpus splits byte-unchanged, verified by full-suite run |
| CONV-C13 | §13.13 contact-path display convention adoption (user decision 2026-07-10): shape-(a) A-undercount (F1), contact-branch recovery shortening (F2), contact-branch declared-credit A-overcount (F3) all convert to plain PASS asserting the measured engine-truth value, citing §13.13; a fourth, adjunct bucket (F2b, §13.10-compliance — rollout-era Class-1 HIT-R xfails that should already have asserted the convention value) converts citing §13.10 directly | `/plan` + `/implement` | M | DONE 2026-07-10 — derivation (`c13-membership.tsv`, scratchpad, script-proposed + per-entry audited against all 222 xfail strings across 19 corpora): **90 FULL** (xfail removed: F1 68 pure + 3 stacked with an equally-established F2b R-clause; F2 10; F3 2 — exactly `ken-forward-block/-hit`; F2b 10 total — 7 single-clause + the 3 F1-stacked entries just counted, `dudley-mgb-hp-hit`/`-mp-hit`/`twelve-forward-hit`), **20 PARTIAL** (member clause converts, xfail narrowed to the residual: 13 F1 A-only with an EXCLUDED stacked clause; 5 F1 A-only whose stacked §12.2.2-shaped R-clause fails the registry-membership bar — `dudley-crroundhouse-block/-hit`, `yun-farfierce-block`, `yun-roundhouse-block/-hit` — re-worded as unregistered candidates, backlogged; 2 F2b R-only whose stacked A-clause is an EXCLUDED §12.2.3 candidate — `dudley-jetup-lp-hit/-hp-hit`), **112 EXCLUDED** (untouched, arcade values retained) — sums to 222. Verification: pre-update `run-suite.sh --check-golden` drift set == exactly the 90 FULL labels, every line `verdict XFAIL->PASS`, zero drift elsewhere (mechanically diffed); `--update-golden` touched only the 16 member-bearing golden TSVs, each changed line a FULL-member verdict-only flip; final `--check-golden` → exit 0, zero drift, 19/19 GREEN. New totals: XFAIL 222−90=**132**, PASS 836+90=**926**, total rows unchanged at 1,058 (reconciled per-corpus against the membership table, every corpus's `baseline_xfail − |FULL| == new_xfail`). Canary re-measured: Ryu 37/0 (was 34/3), Hugo 28/2 (was 24/6), Q unchanged 72/1. Backlog (flagged, not converted — candidates for a possible follow-up user decision): §12.2.1 UOH landing-clocked family (`alex/elena/necro/twelve-uoh-*`), the Hugo-Roundhouse R-side pair (`h-roundhouse-block/-hit`), and the re-worded unregistered §12.2.2 candidates surfaced by this derivation (`dudley-crroundhouse` R clause, `yun-farfierce`/`-roundhouse` R clauses); `twelve-crfierce-hit`'s stacked R-clause was judged ambiguous on its own hedge and EXCLUDED rather than force-classified into F2/F2b. **CROSS-REF 2026-07-10 (append-only): this row's backlog is now dispositioned by SWEEP-2 (see that row below)** — `dudley-crroundhouse` R, `yun-farfierce`/`-roundhouse` R, and `twelve-crfierce-hit`'s stacked R all promoted + converted; the flagged `twelve-axe-jab-hit` R-clause (F-5) converted as a §13.10-compliance/F2b narrow; the §12.2.1 UOH family and Hugo-Roundhouse pair remain un-dispositioned. **FURTHER CROSS-REF (2026-07-10, CONV-2, append-only):** both are now dispositioned — see the CONV-2 row below (§12.2.1 UOH family converts PARTIAL under new family F4; Hugo-Roundhouse converts FULL under new family F5). |
| SWEEP-2 | Classification sweep #2 (post-C13): measurement-window lanes over the 132-xfail roster (82 IN / 50 OUT), fable-grade consolidation audit, §13.13 conversions for every (a) ruling, registry/limitations updates, engine-observer candidate rows | direct edit + orchestrated lanes + fable-grade audit | L | DONE 2026-07-10 — evidence record: `sweep2-final-rulings.md` (session scratchpad; per-member rulings with P2/P1ledger/trace citations). **Method finding (load-bearing):** the window established the capture counting rule directly (`[P2tick]` streams): a tick is appended iff `r1!=0` AND (`hstop==0` OR first-contact event tick); an N-valued hitstop freeze excludes exactly N−1 ticks; **a frozen cell RESUMES contributing counted ticks post-thaw**; the first `r1==0` tick is excluded on every leg. The first audit's §A residual table and its ENGINE-5-shaped "+1 capture boundary" candidate were tally artifacts of missing the post-thaw resumption ticks — DISSOLVED (no tracker row); the contamination flag on shipped C13 values is WITHDRAWN (capture accounting measured symmetric/exact on every questioned axis; no re-measure needed). **Dispositions (82 IN entries): (a) 42** = 35 FULL converts (verdict flips XFAIL→PASS: 20 F2 R-promotions incl. the R-surplus corollary now CLAIMED with first proofs, 15 F3 A-promotions via own-ledger reconciliation) + 7 PARTIAL narrows (hayate ×6 `A:3` under F1, sean-ryuubi-hk-block `A:10` — zero drift, all stay XFAIL); **(b) 18** (mechanism-established re-cites: remy-uoh no-cut re-entry ledger, remy-lov travel-time closure, necro-flyingviper pinned-meter identity, ken-tatsu duration-mutation, yun-crfierce contact-skip reclassification, chunli-uoh §12.2.1 whiff-exact variant, ibuki-twdsroundhouse item-4 join, elena-lynxtail window-skip, remy-cbk-whiff boundary tick); **(c) 11** across 5 new candidate rows (ENGINE-5..9 below); **(d) 3** (§12.1 rows 7/8); **(e) 8** (what-was-tried re-cites: ibuki-uoh, urien-vkd R + §12.1 row 9, remy-cbk contact legs, yun-zesshou). Plus OUT-4 F-5: twelve-axe-jab-hit `R:12` F2b narrow (no flip). Synthesis: §12.2.2 members grown (+12 entries + 9-member R-SURPLUS sub-list), §12.2.3 F3 set 2→18 members (dated amendments at both member-set statements), §12.2.4 3 NEW rows + 7 enriched, §12.1 rows 7/8/9 + row-1 addendum, §13.5.1b Yang-census dated correction, §13.13 exclusion items 5/6/9 dated amendments. Verification: gate sequence per plan §1.5 — tooling byte-identity + 5 lever constants asserted; pre-update drift == exactly the 35 FULL labels (all `verdict XFAIL->PASS`, zero elsewhere); `--update-golden` on exactly 10 corpora, verdict-only diffs; final `--check-golden` exit 0, 19/19 GREEN. New totals: XFAIL 132−35=**97**, PASS 926+35=**961**, rows unchanged at 1,058 (per-corpus reconciliation in the commit message). **CROSS-REF (2026-07-10, CONV-2, append-only):** its 18 (b)-disposition entries are now dispositioned by CONV-2 (see that row below) — 11 converted (remy-uoh no-cut re-entry ledger ×2 entries, remy-lov travel-time closure ×3, ken-tatsu duration-mutation ×2, yun-crfierce contact-skip reclassification ×1, chunli-uoh §12.2.1 whiff-exact variant ×2, ibuki-twdsroundhouse item-4 join ×1), 7 remain excluded (necro-flyingviper pinned-meter identity ×3, judgment call unmet proof bar; elena-lynxtail window-skip ×2 entries — `-block`/`-hit` — candidate/compound, unmet proof bar; remy-cbk-whiff boundary tick ×1 and remy-uoh-whiff ×1, both WHIFF-excluded). |
| CONV-2 | §13.13 scope extension (user decision 2026-07-10, second §13.13-family decision of the day): extend the live-meter convention to every registered + proven + engine-truth divergence mechanism not already covered by CONV-C13/SWEEP-2 — families F4 (§12.2.1 UOH landing-clocked A), F5 (Hugo-Roundhouse landing-clocked R), F6 (§12.2.1 R-triplet whiff-exact variant), F7 (§12.2.4 contact-skip R-window, MEASURED members only), F8 (§12.2.4 contact-mutated declared cell duration), F9 (§12.2.4 no-cut re-entry re-crediting A), F10 (projectile travel-time displayed adv), F11 (partial-restore compound displayed A) | direct edit + mechanical derivation + fable-grade audit | M | DONE 2026-07-10 — derivation (`conv2-membership.tsv` equivalent, mechanically reconstructed from `golden/*.tsv`'s 97 XFAIL rows + the 12 corpus YAML xfail strings + `sweep2-final-rulings.md`/`s12-audit-rulings.md`, cross-checked line-for-line against this plan's own authoring session): **16 FULL** (xfail removed: F5 ×3 — `h-roundhouse-block/-hit` R=30, `ibuki-twdsroundhouse-block` R=22; F6 ×2 — `chunli-uoh-block/-hit` R=4; F7 ×1 — `yun-crfierce-block` R=16 (A already converted under F1 in an earlier cycle); F8 ×2 — `ken-tatsu-lk-block/-hit` R=15; F10 ×3 — `remy-lov-lp-block` adv=23, `remy-lov-lk-block` adv=17, `remy-lov-lk-crouch-probe` adv=15 (this repo's first in-corpus `adv` int literals); F11 ×5 — `makoto-crstrong-block` A=4, `makoto-crshort-block` A=4, `twelve-axe-jab-block/-hit/-crouch-probe` A=8), **9 PARTIAL** (member clause converts, xfail narrowed to residual, zero golden drift: F4 ×5 — `alex-uoh-block` A=15/`alex-uoh-hit` A=12/`elena-uoh-block` A=12/`necro-uoh-block` A=12/`twelve-uoh-block` A=12, R-triplet clause stays meter-suspect; F9 ×2 — `remy-uoh-block` A=13/`remy-uoh-hit` A=11, item-18(c) R clause stays; F11 ×2 — `yang-uoh-block/-hit` A=8, item-18(c) R clause stays), **72 EXCLUDED** (untouched, arcade values retained) — sums to 97. Named judgment calls, all confirmed EXCLUDE by review (OQ-1 through OQ-4): `necro-flyingviper-lp-*` ×3 (stays UNCLASSIFIED, HONESTY CAVEAT, two active_pf routes never per-tick closed); `elena-lynxtail-lk-*` ×2 (A-clause banked-credit arithmetic not ledger-decomposed; R-clause CANDIDATE of the F7 class, "not per-tick closed"); `dudley-uoh-block/-hit` ×2 (hedged UNCLASSIFIED-R, no mechanism claimed); `alex-uoh-whiff` (the named 9th member of the §12.2.1 nominal family — WHIFF, exclusion 1 outranks mechanism proof, the reason that family converts only 5 of 9); `elena-uoh-hit`/`necro-uoh-hit`/`twelve-uoh-hit` (A already arcade-exact on HIT, no convertible clause); all 11 ENGINE-5..9 candidates (defective pending their fix cycles, untouched). Verification: Gate 0 (tooling byte-identity + 5 lever constants `=1`) asserted before any run; Pass 1 pre-update `run-suite.sh --check-golden` (19/19 GREEN, `conv2-verify-pre.log`) — drift set == exactly the 16 FULL labels (`chunli-uoh-block/-hit`, `h-roundhouse-block/-hit`, `ibuki-twdsroundhouse-block`, `ken-tatsu-lk-block/-hit`, `makoto-crstrong-block`, `makoto-crshort-block`, `remy-lov-lp-block`, `remy-lov-lk-block`, `remy-lov-lk-crouch-probe`, `twelve-axe-jab-block/-hit/-crouch-probe`, `yun-crfierce-block`), every line `verdict XFAIL->PASS`, zero drift on any PARTIAL member or any other row; Pass 2 `--update-golden` scoped to exactly the 8 FULL-bearing corpora (chunli hugo ibuki ken makoto remy twelve yun, 8/8 GREEN) — diff is verdict-only XFAIL→PASS on exactly the 16 labels, measured value columns byte-identical (git-diff verified); Pass 3 final `run-suite.sh --check-golden` (`conv2-verify-post.log`) → exit 0, zero drift, 19/19 GREEN. New totals: XFAIL 97−16=**81**, PASS 961+16=**977**, rows unchanged at 1,058 (per-corpus reconciliation: akuma 2→2, alex 3→3, chunli 2→0, dudley 3→3, elena 5→5, hugo 2→0, ibuki 10→9, ken 2→0, makoto 8→6, necro 6→6, oro 6→6, q 1→1, remy 10→7, ryu 0→0, sean 8→8, twelve 6→3, urien 9→9, yang 8→8, yun 6→5 — all verified against `golden/*.tsv` post-update). Canary re-measured: Hugo `total=30 (PASS=30, XFAIL=0)` (was 28/2, both Roundhouse entries now convert under F5); Chun-Li 0 xfail (was 2); Ken 0 xfail (was 2); Q unchanged `total=73 (PASS=72, XFAIL=1)`; Ryu unchanged `total=37 (PASS=37, XFAIL=0)`. Backlog (flagged, not converted — candidates for a possible future user decision): `necro-flyingviper-lp-block/-crouch-probe/-hit` (nearest promotion candidate — a per-tick closure of the two active_pf routes would convert it); `elena-lynxtail-lk-block/-hit` (a per-tick closure of its A-clause or R-clause would convert it under F4-style or F7); `dudley-uoh-block/-hit` R clause (no validated whiff-exact anchor the way chunli-uoh has one); the still-excluded item-18(b)/(c) meter-suspect R inventory across the F4/F9/F11 PARTIAL members' residual clauses (9 entries: alex ×2, elena ×1, necro ×1, twelve ×1, remy ×2, yang ×2) — all meter-suspect, no proven mechanism, not candidates for any named family as currently understood. Zero `src/` changes, zero tooling changes (`compile_corpus.py`, `check_frame_data.py`, `golden.py`, `run-suite.sh` byte-identical throughout). |
| ENGINE-5 | CANDIDATE (sweep #2, 2026-07-10): accumulator blind to same-cgix non-sentinel self-loop (urien-uoh). CONFIRMED by own ledger: exactly 3 credit events (cgix 20/24/28, add=3 each), engine_a plateaus at 9 on all three legs while active_pf=11; cell 28 (cgctr=3, non-sentinel) self-loops without a cgix change so neither crediting gate fires (s4-findings; gate pair in `charset.c` `char_move()`). Affected: `urien-uoh-whiff/-block/-hit` A-clauses (uniform −2). **DATED CORRECTION (2026-07-10, ENGINE-5 closure — see Status column, this row):** the "−2" figure above is vs engine-played ticks (`active_pf`=11), not arcade; arcade A (`docs/arcade-frame-data/urien.json:808-812`) = 10, so the true undercount vs arcade is 1, not 2. | future `/plan` + `/implement` | S–M | **CLOSED — ARCADE-INTERMEDIATE / NO-PRINCIPLED-TARGET (2026-07-10, orchestrator ruling, without a census cycle)** — arcade A=10 (re-verified directly, `urien.json:808-812`) sits strictly BETWEEN every reachable engine-truth quantity: measured/declared-today 9 (`s4-findings.md` P1 ledger — self-loop re-dispatch invisible to the crediting edge), played-ticks 11 (`active_pf`, independent per-frame classifier), §13.11-declared-with-selfloop 12 (clamped fresh `cgctr` credited at re-dispatch entry). Only an n=1-overfitted rule (credit the re-dispatch tick, exclude the same-tick transit-out tick) reaches 10 — the ownership-fitted-rule shape already REJECTED by both the ENGINE-6 and ENGINE-7 census closures above; REJECTED here too, regardless of census outcome (pre-registered, `engine5-plan.md` §2.4). The dispatch-vs-dwell MECHANISM is sound and exactly distinguishable in-engine (`charset.c:439-444`/:675-712, dwell = `--cg_ctr != 0`, re-dispatch = the reload path) — that finding is preserved; only the FIX TARGET is unreachable by a principled rule. See synthesis §12.2.4's new "Same-cgix non-sentinel self-loop" row for the full four-quantity table and citations (`engine5-plan.md`, `s4-findings.md`). Flagged, not decided here: a future USER convention grant (display 11 played-truth or 12 declared-truth for self-loop charts) is queued on the user-review list, Phase-4-adjacent. Zero code shipped, zero goldens touched; `urien-uoh-whiff/-block/-hit` A-clauses stay xfail at measured 9. R-clauses stay NOT in scope (clause-level OUT-1). **UPDATE 2026-07-11 (CAPTURE-1): the flagged grant is CLOSED DECLINED**, not merely still-flagged — arcade hardware capture confirms A=10/R=5 directly; see the CAPTURE-1 tracker row below and the SWEEP-3 grant list's G3 entry. **DATED CORRECTION (2026-07-11, LAYER-1):** a convention-twin engine-raw probe measures this move's RAW frames (any-of-four-s16 `att_box` predicate) at box-A=**10**, arcade-identical -- a fifth, engine-native quantity distinct from the four already tabulated (9/11/12/arcade-10). `active_pf`=11 differs because `h_att_set` ORs in the cell-transition flag (`charset.c:453`), which outlives raw dims (ENGINE-10's shape). Closure REMAINS CLOSED (no credit-rule lever ships); a principled target DOES exist now, it is just a different meter (raw-box count), not a credit tweak -- fix path is the LAYER-1 tracker row's OVERLAY RE-ANCHOR program, user-gated. See `docs/frame-data-synthesis.md` §13.16. |
| ENGINE-6 | CANDIDATE (sweep #2, 2026-07-10): general-classifier S anchor ignores `att_hit_ok` (yang-forward). CONFIRMED by trace+source: `h_att_set` fires on cell load (`charset.c:2989-2997`, unconditional on atix≠0) two ticks before `att_hit_ok` arms (`charset.c:2938` via `set_new_attnum()`); the engine's real collision loop gates on BOTH (`hitcheck.c:1618-1621`) and never box-tests at F=5-6; ENGINE-4/lever-J already fixed the identical pair on the projectile path (s6-findings, H2 proven). Affected: `yang-forward-block/-hit` (S=5 vs 7). | future `/plan` + `/implement` | S–M | **CENSUS-FALSIFIED (2026-07-10)** — a 1,039-window pre-diff census across all 19 corpora (`e6-census.tsv` + `e6-census-report.md`) measured every window where the sticky `att_hit_ok`-arm tick differs from the cell-load `first_active_raw` tick: 8 divergent windows across 3 move families. `yang-forward-block/-hit` (×2) key arcade Startup on the arm tick — CONFIRMS. `remy-crfierce-block/-whiff/-hit` (×3, arcade-exact PASS at S=8) and `twelve-backforward-block/-whiff/-crouch-probe` (×3, arcade-exact PASS at S=5) key arcade Startup on the cell-load tick instead — the candidate's `max(first_active, athok_armed)` fix would REGRESS both off arcade-exact (8→9, 5→7). All three families are mechanically byte-identical (cell-load fires 1-2 ticks before the arm; the arm/contact coincidence holds on WHIFF legs too, with no contact at all, proving it is not causal); every safety/diagnostic column reads identically clean across all 8 windows — no discriminator exists in any observable engine state. Lever L is REJECTED, not deferred: the "full lever gauntlet" expectation above is VOID — zero levers touched, zero code shipped this cycle. Arcade keys Startup per-move (cell-load on remy/twelve, arm on yang); rung 3 per the census's own ladder (tighten and mechanistic-scope-down both fail first). `yang-forward-block/-hit` stay xfail with this census as their citation; `remy-crfierce-*` and `twelve-backforward-*` stay PASS, unaffected. See synthesis §12.2.4's S-divergence row for the full ENGINE-6 CENSUS-FALSIFIED record and the strengthened future bar (positives must be on the SAME code path as the divergent windows — ENGINE-6's own cross-path lever-J positive was not enough). |
| ENGINE-7 | CANDIDATE (sweep #2, 2026-07-10): §13.5.1 cut anchor fires at the cghi-edge, 4 ticks after the attacker's guard_flag re-arm (yang-senkyuutai). CONFIRMED shape on both legs (rearm at reset+6, anchor at reset+10, gap = overshoot = 4 exactly; gflg-based R reconstructs arcade 34 exactly; s8-findings). Includes the shipped §13.5.1b census correction (synthesis :1426-1432 claimed rearm at anchor+0/+1 for this move — measured anchor−4; dated correction landed with SWEEP-2). Affected: `yang-senkyuutai-lk` ×3 (R=38 vs 34, uniform). | future `/plan` + `/implement` | M | **CENSUS-FALSIFIED (2026-07-10)** — a 1,039-window pre-diff census across all 19 corpora (`e7-census.tsv` + `e7-census-report.md`; re-plan `engine7-replan.md`) measured every grounded, post-reset guard-rearm edge preceding a current R end in the full suite: 7 moves total. Arcade end = rearm+0 for 4 (`yang-senkyuutai-lk`, `ibuki-kazekiri-lk`, `yun-uoh-whiff`, `yang-uoh-whiff`), rearm+1 for 2 (`chunli-uoh` = the anchor itself; `remy-uoh` = no traced engine event, only a mid-cell `cgctr` decrement), rearm+3 (= the natural r1 end) for 1 (`urien-headbutt` lp/mp/hp, 8 PASS legs). Two identical-signature pairs land on opposite sides — `ibuki-kazekiri-lk` (+0) vs `urien-headbutt` (+3), and `yun-uoh`/`yang-uoh` whiff (+0) vs `remy-uoh` (+1) — with no observable engine state discriminating them. The retime/trim ("lever K") is REJECTED, not deferred: the original "fix cycle decides whether the anchor should retime to guard_flag; full lever gauntlet" expectation above is VOID — zero levers touched, zero code shipped this cycle. This census also surfaced a second falsification of the shipped §13.5.1b census claim: `chunli-uoh` re-arms at anchor−1, not anchor+0/+1 — see the synthesis's §13.5.1b SECOND dated correction and its §12.2.4 ENGINE-7 CENSUS-FALSIFIED row for the full seven-move table. `yang-senkyuutai-lk` ×3 stays xfail with this census as its permanent citation. **DATED NOTE (2026-07-11, LAYER-1):** under the twin's from-scratch gflg-edge derivation (not an overlay-endpoint retime), the engine's own gflg edge reproduces arcade R exactly on `yang-senkyuutai-lk` (34), `ibuki-kazekiri-lk` (26), `yun-uoh` (6), `remy-uoh` (5) AND `chunli-uoh` (5, no regression) -- see `docs/frame-data-synthesis.md` §13.16's chunli reconciliation. Lever K's REJECTION above stands unchanged; the twin sidesteps this census's own irreducible heterogeneity by deriving R from scratch instead of retiming the overlay's existing endpoint. `urien-headbutt` (rearm+3) was never arcade-captured; whether arcade's own busy-R there reads 16 (twin) or 19 (golden/oracle) stays UNKNOWN. |
| ENGINE-8 | CANDIDATE (sweep #2, 2026-07-10): r1-clear/already_idle end-detection fires mid-animation on contact legs (oro-jinchu). CONFIRMED shape by independent re-trace of both legs: r1 4→0 concurrent with a discontinuous cgix 80→120 jump into a non-idle segment (cghi=14) that keeps advancing 16 (BLOCK) / 31 (HIT) real frames before chart idle; whiff is the opposite (r1 conservative, arcade-exact) (s9-findings). Affected: `oro-jinchu-lk-block/-hit` (R=0 vs 19). **CORRECTED (2026-07-11, engine8-plan.md §1.4 premise verification):** the "16/31 real frames before chart idle" figures above were themselves wrong — a defender-column misread (BLOCK: the window's last row's cgix=0/cghi=237 are the DEFENDER's own exit-from-blockstun columns, not the chart's; the attacker's own row at that tick is still airborne, cgix=132/cghi=14/y=140 mid-tail — the chart never reaches idle inside the observable window at all) and a landing-label-vs-stable-idle conflation with an r2 mis-statement (HIT: r1 clears F=318, r2=6 held through the fall; F=350 first landing tick (cgix=0/cghi=1), 32 ticks after r1-clear, r2 STILL 6 at that exact tick; r2 flips 6→2 at the very next tick F=353, landing anim F=353-366 plays at r2=2 not "r2 still 6"; stable idle (r2=1) only at F=370, 52 ticks after r1-clear). Neither correction revives R=19 — both are further from it. | future `/plan` + `/implement` | M | **CLOSED — ARCADE-WHIFF-CANONICAL / NO-CODE-FIX-EXISTS (2026-07-11, engine8-plan.md Stage 1, autonomous per pre-registered branch (c)).** Branch (a) (end-anchor fix) is DEAD BY ARITHMETIC at design time (engine8-plan.md §3, independently re-verified by a fresh 1,058-window census this pass, `e8-census.tsv`/`e8-census-report.md`): every candidate end anchor was measured — r1-clear (today, R=0), first chart-idle label (BLOCK: unobservable/truncated, ≥16 visible; HIT: 32, census-exact match to the plan's hand-derived figure), touchdown (~33-41 by rise/fall symmetry), stable-idle loop (HIT: 52, census-exact; BLOCK: ≥53 conservative) — none lands on arcade's 19, and a suite-wide pure-chart-idle anchor would REGRESS the currently arcade-exact WHIFF leg (own chart reaches cghi=1 at F=42, 13 ticks before r1's correct, arcade-exact clear at F=55 — re-verified directly against raw trace this pass). Measured BLOCK adv=+16 is EXACT to arcade's own Block_advantage=16, anchored at the bounce — the coherence witness that the engine treats the attacker as done there. Branch (b) (§13.13/F12 engine-truth registration) did NOT ship: the Step-1 actionability probe (engine8-plan.md §6, `e8-airact.yaml`, `e8-probe-rundir/trace.log`) found the post-bounce tail **NOT-ACTIONABLE** — 4 staggered air-normal (MK) presses at the BLOCK leg's own post-bounce tail (≈bounce+1/+4/+12/+24 ticks, each independently located via its own `sw_new` input-rising-edge). 3 of the 4 (immediate/early/mid, incl. the decisive +1-tick) are directly observed negatives, each confirmed to land exactly at its intended offset and registering at the input-system level but producing zero gameplay effect (r1/athok/hatt/jatix/cghi all unchanged); the 4th (late, +24) truncates at bounce+15/16, before its own press tick, so its negative is inferred, not observed. The `e8-jumpMK-control` positive control (same MK tap mid neutral-jump) shows the full actionability signature cleanly (athok 0→1, hatt 0→1, jatix nonzero, own tracked WHIFF S=4/A=8/R=33) — proving the probe methodology sound and the negative read genuine. Per the plan's pre-registered readout: control-positive + 3 directly observed negatives (immediate/early/mid, incl. the decisive +1-tick) + late truncated-before-press → inferred negative → NOT-ACTIONABLE → branch (c). Census also found 3 same-shape CANDIDATE windows beyond the two jinchu legs (`oro-throw-hit`, `alex-powerbomb-lp-hit`/`-unblockable-probe`, `ibuki-kubiori-lp-hit`) — named, not converted (candidates never convert): `oro-throw-hit` reads clean (census sanity OK) but is mechanistically DIFFERENT (cghi=6, no cgix/cghi discontinuity at its release tick — the already-documented §13.10 Class 3 connected-grab R-derivation via raw_len saturation, unrelated to jinchu's mechanism); the other two are tooling-inconclusive (command-grab multi-phase r1 shape defeats this census's single-pass heuristic, flagged by its own FINAL-cross-check sanity gate rather than hand-waved). None is PASS-arcade-exact at R>0 while airborne-released (both PASS members assert an explicit derived R literal, never `from-qjson`; the XFAIL member isn't PASS) — no census-contradiction of the model, no STOP triggered. **Zero code shipped, zero expect/golden changes** — `oro-jinchu-lk-block/-hit` stay XFAIL with arcade R=19. Suite counts unchanged, 980 PASS / 79 XFAIL. See `docs/frame-data-synthesis.md`'s §12.2.4 "Two-way contact R=0" row and `corpus-oro.yaml`'s section comment for the full closure record. **VERDICT FALSIFIED/SUPERSEDED 2026-07-14 by ENGINE-JINCHU (Session 6 hardware capture, `<sp>/zero/arcade-track-a/session-report.md`; see the new ENGINE-JINCHU row below):** the NO-CODE-FIX-EXISTS verdict above is WRONG for the two HIT legs — a real busy `771->768` edge exists and latches at `busyr=33` on both `oro-jinchu-lk-hit` and `oro-exjinchu-hit` (CONTACT-2's own preserved FINAL traces, `<sp>/zero/contact2/step1/rundirs/`), which this row's own chart-idle/r1-clear analysis never considered (it pre-dates CONTACT-2's busy-edge instrumentation). Arcade truth is base-LK 33/33 (symmetric HIT/BLOCK, not the oracle's 19) and EX 34-HIT/52-BLOCK (asymmetric, not the oracle's 16) — `oro-jinchu-lk-hit` converts to PASS via the new lever U (busy-edge 33 == arcade 33 exact). This row's own MECHANISM finding (no in-engine recovery anchor; airborne `Player_normal` handoff; every end-anchor candidate measured, none matching) stands UNCHANGED and remains exactly why the BLOCK legs (busy edge never latches, `busyr=-1`) and the EX-hit's own 1-frame gap (busy-edge 33 vs arcade 34) cannot be closed by any busy-edge lever — only the VALUE this row preserved (oracle R=19/16) was wrong, not the underlying analysis. |
| ENGINE-9 | CANDIDATE (sweep #2, 2026-07-10): adv defender-idle anchor latches the FIRST blockstun exit on multi-contact block chains (remy-crroundhouse). CONFIRMED: two-contact move; overlay def_idle latches exit #1 (F=33); using the FINAL exit (F=63) reproduces arcade −11 EXACTLY (`frame_data_overlay.c:1208-1213` first-edge gate, :1187 event latch; s11-findings member 1). Defect is on the DEFENDER anchor; S/A/R are green. Affected: `remy-crroundhouse-block` (adv −41 vs −11). | future `/plan` + `/implement` | S–M | **DONE 2026-07-11 — FIXED (lever M `fd_adv_last_stun_exit`, synthesis §13.14; the run's first census-approved engine fix after the E5/E6/E7 falsifications).** Design: `defender_idle` re-arms to −1 on a same-chart re-contact — 5-conjunct signal (event HIT/BLOCK latched ∧ `defender_idle>=0` ∧ `cgix_reset_frame<0` ∧ fresh `dm_stop` 0→nonzero defender edge ∧ `dn->r1==1`) — placed immediately before the idle-return latch, so the FINAL stun exit anchors adv; overlay-only, zero engine-file changes. **Census scope (Step 1, `e9-census.tsv`+`e9-census-report.md`, RUNG 0 SHIP-ELIGIBLE):** 1,039 FINAL windows / 19 corpora; exactly 2 windows suite-wide have a disjoint exit-then-re-stun topology; shipped-variant (M1) re-arm population is the SINGLETON `remy-crroundhouse-block` (`remy-crroundhouse-hit` resolved NOOP — single contact); M0 (no chart gate) falsified by `q-uoh-chain-retrigger` (re-arm → adv_pred +37 on a PASS row; its tap-1 cgix reset F=5585 precedes tap-2 contact F=5604, so M1's gate provably refuses it); monotonicity `adv_pred>=adv_today` confirmed 1039/1039; `finalize_delta=0` on all windows (zero kd-flip / window-merge / lever-I interaction risk). The E6 ≥2-same-path-positives bar was explicitly REPLACED (not silently weakened) by the §13.14 accounting-defect exemption — conditions (a) population-exactness, (b) zero no-op-branch drift, (c) oracle-exactness of the sole member: ALL THREE PASSED. **Drift set:** exactly `remy-crroundhouse-block` verdict XFAIL→PASS, adv −41→−11 (== remy.json Block_advantage); xfail removed from corpus-remy.yaml; golden/remy.tsv updated via scoped `--update-golden`; final `--check-golden` 19/19 GREEN zero drift. **Totals: 979 PASS / 80 XFAIL → 980 / 79** (rows unchanged at 1,059). Lever gauntlet: M=0 drifts exactly the member back to −41 (PASS→FAIL, 18 GREEN/1 RED, nothing else); I=0 golden drift exactly `urien-vkd-lk-hit` kd 1→0 (E3 contract unchanged — census bound the lever-I interaction to zero); J=0 flags exactly the 9 twelve-ndl rows at legacy S=12/12/13, R=26/30/34 (ENGINE-4 contract unchanged); E (helper false, Q-scoped per its E3 precedent contract) FAILs exactly q-fierce/crhk/backhk (+ the E3-documented `q-dla-rh-hit` golden-only kd drift); A/B/F/G/H dispositioned by grep (`defender_idle`/`fd_adv_last_stun_exit` read nowhere in their attacker-side credit/cut paths; charset.c untouched) + the zero-drift full-suite runs as smoke. Determinism ×2 (consecutive remy runs byte-identical). NOTE: **gate 0 now asserts SIX lever constants** (F `charset.c` + G/H/I/J/M overlay). Residuals documented in §13.14: live-play red-parry (untested by harness population), Q SA3 NINGENBAKUDAN own-`dm_stop` hazard (`plpat18.c:36-38,51`, harness-unreachable), trade immunity by construction (`hitcheck.c:131-153` abs-merge). |
| ENGINE-10 | CANDIDATE (sweep #3, 2026-07-11): overlay's hatt-based `active_pf` accumulator disagrees with `engine_a` (jatix-grounded) by exactly one tick at the active-window tail on Yun's Zesshou Hohou (Fierce) (`yun-zesshou-hp-block/-hit`). CONFIRMED by audit re-trace on both legs (`s3w-rundir/zesshou/trace.log`): `h_att_set` outlives `jatix` by exactly one tick (block GT76 jatix=0/hatt=1, clears GT77; hit GT383/GT384 identical shape), no credit event fires at the lag tick, `active_pf`=17 vs `engine_a`=16. DIAGNOSTIC-ONLY: displayed A=16 is `engine_a`-grounded and R=11 is the T−S−A complement, so aligning `active_pf` to the jatix boundary would change ZERO displayed values, zero goldens, zero gameplay. Affected: `yun-zesshou-hp-block/-hit` (diagnostic only — both entries stay xfail on their own, unrelated sum-preserving-row clause). | future `/plan` (if ever) | S | CONFIRMED as a candidate tracker row (2026-07-11, classification sweep #3 fable-grade audit, `sweep3-final-rulings.md` B.9) — evidence real and audit-verified on both legs. Any future fix must meet the strengthened E6 bar (named discriminator; holds across a full re-census; ≥2 independent positives on the SAME code path — cross-path positives do not count, per the ENGINE-6/ENGINE-7 census precedent established above). Expected outcome per the E5/E6/E7 precedent is a closure note, not a lever — the divergence is diagnostic-only and does not affect any displayed value, so there is no drift to fix. Zero code shipped, zero census run this cycle (not required to register a diagnostic-only candidate); `yun-zesshou-hp-block/-hit` remain xfail on their pre-existing (b)-enriched sum-preserving-row clause, unaffected by this row. See `docs/frame-data-synthesis.md` §12.2.4's "Sum-preserving A/R boundary shift" row for the (c)-cite. |
| ENGINE-D2 | SA-WHIFF-A/D2's own MOVE_START-site-reset-vs-`char_move()` same-real-frame tick-ordering race (BUFFER-1 diagnosis, `docs/frame-data-synthesis.md` §12.2.4 `:978`/`:980`; the racing zeroing is the inline block at the MOVE_START site, NOT `fd_reset_move()`, which resets only `g_cur` fields): the MOVE_START reset zeroes `fd_engine_active_count[i]` on the same tick `char_move()` already credited the new move's first active/catch cell, on charts whose first post-super-flash cell is active (freeze-deferred MOVE_START). LEADING HYPOTHESIS at diagnosis time, not yet a shipped fix. | `/plan` + `/implement` | M | **DONE 2026-07-13 — FIXED (lever S `fd_movestart_same_tick_credit_hold`, `frame_data_overlay.c`, declared next to N/O).** Gated on `fd_prev_active_cgix_tick[i] == Game_timer` at the reset site: preserves the same-tick cell's credit (`fd_engine_active_count[i] = fd_prev_active_cgix_add[i]`) instead of zeroing it, keeping the `fd_prev_active_cgix*` bookkeeping intact so the same cell cannot double-credit next tick. Lever at 0 restores today's code byte-identically (identity-gate verified). **Step 1 (census/pre-registration, no engine edits):** full-suite offline mover scan (94 corpora, 1,326 FINAL windows) plus a per-tick reset-site probe (`FDO_D2_PROBE`) found the complete affected population — 44 same-tick windows, ALL super-freeze SA windows, across 16 member corpora, zero non-super/normals/EX fire. The probe also surfaced a mid-tick-transit variant the offline trace scan is structurally blind to (`ryu-sa2-block/-hit`, `elena-sa2-hit` probe-only, `makoto-sa1-block` probe-only) — 4 additional windows beyond the 40 trace-visible ones. **Step 2 (fix + conversions, one commit):** pre-drift census matched the amended 40-cell predicted table EXACTLY (zero unpredicted movers); identity run (lever S=0) byte-identical to baseline on MEASURED columns (outcome/S/A/R/adv/kd) across all 94 corpora, with verdict-column-only diffs confined to exactly the 12 expect-edited conversion corpora; lever-F(=0) × lever-S(=1) spot check confirms full additive independence — whiff legs and all ken-sa1 legs are byte-identical under the F toggle, while akuma-sa2/dudley-sa1 contact legs diverge under F for F's own revoke-restore reasons, with S itself contributing a uniform +1 in BOTH F states on every member leg (akuma block/hit F0 21→22 / F1 23→24; dudley block F0 33→34 / F1 39→40, hit F0 35→36 / F1 38→39). **Member census (16 corpora, 44 legs):** ken-sa1 (×3, A→`from-qjson`), dudley-sa1 (whiff/block A→`from-qjson`; hit's own separate -1 residual stays OMITTED, untouched), dudley-sa3 (block/hit XFAIL→PASS), akuma-sa2 (×3, XFAIL→PASS), elena-sa1 (×3, A→`from-qjson`), elena-sa2 (negative-confirm, delta=0, golden unchanged), alex-sa2 (×3, XFAIL→PASS), ibuki-sa2 (whiff XFAIL→PASS; **chiblast-block/hit PASS→XFAIL**, joining the "contact-only A-overcount, box-backed" class — post-fix `engine_a==box_a==16`, the pre-fix oracle-exact reading was two canceling bugs, not agreement; ORCHESTRATOR SIGN-OFF GRANTED), ryu-sa2 (all 3 legs are D2 members — the long-standing §13.11 "declared-truth" plain-PASS `A: 12` on BLOCK/WHIFF is RETRACTED as a race artifact and converted to `from-qjson`; HIT value-only refresh, stays unasserted), chunli-sa1 (whiff/hit XFAIL→PASS; block's A clause resolves, adv clause stays open, note reworded), yun-sa1 (whiff A→`from-qjson`; block/hit's own separate, larger -6 residual stays OMITTED), remy-sa2 (block/hit A→`from-qjson`; the prior "ledger-neither (10)" record corrected — the ledger WALK was wrong, not a third value, per the arithmetic-impossibility proof: a credit-dropping race can never make displayed < true ledger), sean-sa2/yang-sa2 (value-only refresh, stay OMITTED — still far from oracle), urien-sa1 (block: A resolves but R does NOT co-move, stays xfail on R alone, sum-conservation narrative reworded; **hit: XFAIL→PASS, the 12th conversion**, one more than this fix's own provisional "up to 11"), makoto-sa1 (newly-discovered member — hit value-only, stays PASS; whiff/block golden-unchanged). **Gauntlet:** full-suite `--check-golden` zero drift with goldens updated, totals EXACTLY 1,266 PASS / 81 XFAIL (from 1,256/91: net 12 XFAIL→PASS, 2 PASS→XFAIL); lever census NINE consts `=1` (the 8 existing + lever S), zero at `=0`; `git diff --stat src/` exactly `frame_data_overlay.c`, zero `tools/frame-data/*.py` changes. See `docs/frame-data-synthesis.md` §12.2.4's SA-WHIFF-A/D2 RESOLVED entry for the full mechanism/member/correction writeup. |
| SWEEP-3 | Classification sweep #3 (post-SWEEP-2/CONV-2/ENGINE-9): measurement-window lanes over the 79-xfail roster (19 IN / 60 OUT by roster rule), fable-grade consolidation audit, §13.13 F2 conversions, registry/limitations updates, one new engine-observer candidate row, consolidated user grant list | direct edit + orchestrated lanes + fable-grade audit | M | DONE 2026-07-11 — evidence record: `sweep3-final-rulings.md` (session scratchpad; per-member rulings with `[P2tick]`/`[P1ledger]`/trace citations, cross-checked against oracle JSONs and golden TSVs). **Roster: 79 = 35 OUT-A + 11 OUT-B + 10 OUT-C + 3 OUT-D + 1 OUT-E + 19 IN** (lanes s3l1/s3l2/s3w + this audit). **Dispositions (19 IN entries): 7 FLIPS** — `makoto-hayate-lp/mp/hp-block/-hit` ×6 (F2, both signs closed to a SINGLE branch-content differential by the audit's own per-tick recount: BLOCK +8 = dwell differential 19 vs 11 ticks, detour 40/44/48 replacing whiff's 0/4/8; HIT −4 = whiff's cgix4/cgix8 skipped outright, dwell unchanged) and `urien-vkd-lk-hit` (F2 R-corollary, 25-tick parallel recovery branch, all jatix=0, contact R − arcade 7 = +25 exactly); **1 NARROW** — `urien-vkd-lk-block` (R clause closed under the identical branch proof; adv −17-vs-−16 residual stays open, §12.1 row 9); **2 REFUSED FLIPS** — `necro-flyingviper-lp-block/-hit` (F5 membership tested and refused: no cgix advance exists, freed-tick arithmetic doesn't close, whiff raw_len 48 vs contact 46 unreconciled — a variant-shaped, not-per-tick-closed sighting CONV-2 (ii) bars; genuinely enriched with a new cell-identical terminal-tail proof across all 3 legs); **2 (b)-CONFIRMED-PENDING-GRANT classes registered** (new mechanisms, no at-grant-time family, per the ENGINE-8 governance route — registry row + user grant list, no conversion this run): "Same-tick interior-transition credit banking" (`remy-cbk-lk-block/-hit` loop-back re-entry +3; `elena-lynxtail-lk-block/-hit` A-clauses skip-path pass-through — ONE registry row, two manifestation topologies, Lane 3's cross-lane kinship adopted) and "Contact-outcome hold/skip differential" (`ibuki-uoh-block/-hit`, +2 real ticks at the hitstop-exit hold + +2 in the R window, zero residual both legs); **1 (e)-PERMANENT** — `elena-lynxtail-lk-block`'s R clause (F7 candidacy REFUTED, not merely un-promoted: the +8 surplus is 6 gap-heritage + 2 second-freeze ticks, not a per-tick match to whiff's gap; candidacy honestly terminal, no further probe exists); **1 (b)-enriched-(c)-cited, NOT on the grant list** — `yun-zesshou-hp-block/-hit` (ledger-closed exactly, but arcade-mapping is two-way-ambiguous so no conversion is offered; ENGINE-10 candidate row registered for the diagnostic `h_att_set`/`jatix` one-tick lag). **Scope-out record** (considered, not run as lanes): item-18 re-attack premise DEAD (ENGINE-7 census citation); Yun/Yang whiff-only reset asymmetry not run (no disposition would change); E8 grab-shaped census extras need nothing; ENGINE-5's 11-vs-12 display-convention option remains queued unchanged. Synthesis edits: §12.2.2 R-deficit list + R-SURPLUS sub-list grown (+2 members: `makoto-hayate-*-hit` R-deficit, `makoto-hayate-*-block`/`urien-vkd-lk-hit` R-surplus), §12.2.4 4 rows updated (Hayate compound CLOSED, stance-conditional enriched, F7 dated-correction/REFUTED, sum-preserving row split) + 2 NEW CONFIRMED-PENDING-GRANT rows, §12.1 row 9 vkd rider + whiff active_pf/R boundary one-liner, §13.13 F2 member-set amendment (+7 FULL/+1 PARTIAL) + exclusion-item-5 dated amendment, SWEEP-3 scope-out dated note. **Verification: gate sequence per plan §1.5** — SIX lever constants asserted `=1` (`charset.c:507`; `frame_data_overlay.c:802/:1222/:1247/:1356/:1378`); tooling byte-identity (`compile_corpus.py`/`check_frame_data.py`/`golden.py`/`run-suite.sh`/`run.sh` vs HEAD; zero `src/` changes; instrumented-window integrity — the s3w window applied a +57-line probe patch to `charset.c` + `frame_data_overlay.c`, built and ran instrumented, then restored via `git checkout -- src/` with pre-patch/post-restore md5s of both files verified == HEAD, followed by a clean rebuild whose measured control values came back byte-identical (see `s3w-findings.md`'s window-integrity block)); pre-update `run-suite.sh --check-golden` drift == EXACTLY the 7 predicted labels (makoto ×6, `urien-vkd-lk-hit`), every line `verdict XFAIL->PASS`, measured columns byte-identical, zero drift elsewhere; `--update-golden makoto urien` scoped, diff verdict-only on exactly those 7 rows; final `--check-golden` → exit 0, zero drift, 19/19 GREEN. **New totals: 980 PASS / 79 XFAIL → 987 / 72** (rows unchanged at 1,059; per-corpus reconciliation: makoto 6→0 (6 flips), urien 8→7 (1 flip), all other 17 corpora unchanged). Consolidated **USER GRANT LIST** (see the section below the tracker table): G1 ibuki hold/skip (+2 potential flips), G2 interior-transition credit banking (+1 to +3 potential flips depending on scope accepted), G3 ENGINE-5 display convention (already queued, restated unchanged); no zesshou conversion option exists (two-way-ambiguous mapping). |
| G12-GRANTS | User grant execution (2026-07-11): G1 (ibuki-uoh hold/skip, §13.13 F12) and G2 (same-tick credit-banking class, §13.13 F13) converted per the SWEEP-3 USER GRANT LIST below; G3 and Dragon Smash NOT granted, untouched | direct edit (corpus + docs) | S | **DONE 2026-07-11** — user grant 2026-07-11, following the orchestrator's recommendation review: "G1 = the ibuki-uoh guard-hold/cut-skip mechanism becomes a granted §13.13 family; G2 = the same-tick credit-banking class (remy-cbk + elena-lynxtail-A) becomes a granted family." **G1 executed in full:** `ibuki-uoh-block` (R:6) and `ibuki-uoh-hit` (R:4) FULL CONVERT under new family **F12** "Contact-outcome hold/skip differential" (§13.13), arcade 5 preserved in both comments — 2 flips. **G2 executed per its own stated scope, one entry blocked by an unmet dependency:** `elena-lynxtail-lk-hit` (A:4, its sole clause) FULL CONVERTS under new family **F13** "Same-tick interior-transition credit banking" — 1 flip; `elena-lynxtail-lk-block` NARROWS (A:2 converts; R clause stays XFAIL, (e)-PERMANENT per the F7 candidacy refutation, unaffected by this grant) — 0 flips; `remy-cbk-lk-block`/`-hit` do **NOT** convert — the grant list's own text (`docs/plan-frame-data-completion.md`, USER GRANT LIST section below) makes CBK conversion conditional on the user ALSO accepting `remy-cbk-lk-whiff`'s separate, still-UNCLASSIFIED ±1 boundary-shift finding as displayed engine truth; the 2026-07-11 grant text grants only "the same-tick credit-banking class... becomes a granted family" and does not state that additional acceptance — dependency unmet as written, so both entries stay fully XFAIL, unconverted, disposition recorded in each entry's own xfail string and in the F13 registry row. **G3 and Dragon Smash are explicitly NOT granted this round — untouched, still queued.** **Net this run: +3 flips** (`ibuki-uoh-block`, `ibuki-uoh-hit`, `elena-lynxtail-lk-hit`). **Totals: 987 PASS / 72 XFAIL → 990 / 69** (rows unchanged at 1,059; per-corpus reconciliation: ibuki 2 (2 flips), elena 1 (1 flip), remy 0 (re-cited, no flip), all other 16 corpora unchanged). Synthesis edits: §12.2.4 "Contact-outcome hold/skip differential" and "Same-tick interior-transition credit banking" rows both dated-updated to GRANTED with per-member conversion/narrow/blocked dispositions; §13.13 gains families F12/F13 (Families F4-F11 block, continuing the sequence). Gate: SIX lever constants asserted `=1`; tooling byte-identity vs HEAD, zero `src/` changes; pre-update `run-suite.sh --check-golden` drift == exactly the 3 predicted labels (`ibuki-uoh-block`, `ibuki-uoh-hit`, `elena-lynxtail-lk-hit`, all XFAIL→PASS); `--update-golden ibuki elena` scoped, verdict-only diff on exactly those 3 rows; final `--check-golden` zero drift, 19/19 GREEN. |
| HEADBUTT-DEFER | Deferred harness fix (sweep #2, 2026-07-10, P-1 deferral absolute): urien-headbutt-hp-hit post-knockdown contact-residue window (§12.1 row 8) — position reads exactly correct (dist=70 byte-exact at MOVE_START) yet contact fails in full-corpus order; threshold bracketed 53..~206 frames past predecessor def_idle; two empirically verified candidate fixes on record: WHIFF spacer entry OR `inter_entry_wait=400`. | `direct edit` (corpus) | S | **DONE 2026-07-11** — chose the WHIFF-spacer candidate (new `urien-headbutt-hp-hit-spacer-whiff` entry, same "press LP" @ dist=250 recipe as `urien-lp-whiff`, inserted immediately before `urien-headbutt-hp-hit`) over the global `inter_entry_wait=400` alternative: least perturbs neighboring entries (only this couplet changes vs. re-pacing all 62 other entries), matches the §12.1 row-1 WHIFF-predecessor idiom. Re-measured: `urien-headbutt-hp-hit` now HIT, S=12/A=6/R=19/kd=1, arcade-exact, plain PASS (xfail removed); byte-identical to the isolated baseline and the RUN 4 spacer probe. Full-suite `--check-golden` gate: pre-update drift exactly the hp-hit row + the new spacer row addition, nothing else; `--update-golden urien` then final `--check-golden` zero drift 19/19 GREEN. §12.1 row 8 marked resolved. |
| ERRATA-1 | Oracle errata: external-source verification of the oracle-field-incomparability bucket (doc-only, no oracle/corpus/golden/engine changes) | direct edit | S | DONE 2026-07-10 — `docs/arcade-frame-data/ERRATA.md`: 4 rows checked against EventHubs + FAT-3S (retrieved 2026-07-10) — Akuma Ashura Senkuu, Oro Oniyama, Ibuki Kubiori all **STRUCTURAL** (external sources corroborate the oracle figure; the harness's own S/A/R model can't represent it); Sean Dragon Smash Strong (open, `corpus-sean.yaml:842`, NOT in the closed 7-entry bucket) **UNVERIFIABLE** — EventHubs' independent Strong row matches this engine's live measurement after a convention offset verified on that source's own Jab/Fierce rows, but `sean.json`+FAT-3S agree Strong≡Jab; no side picked. Tally: 3 STRUCTURAL + 1 UNVERIFIABLE. Side findings recorded: FAT-3S's Kubiori `active` field is a spreadsheet date-autocorrect artifact; FAT-3S/GameFAQs both say Kubiori is qcf+P, conflicting with the corpus's own hcf+P live-probe finding (unresolved, needs repo-side re-probe). `docs/frame-data-synthesis.md` §12.2.4 register gets a one-line pointer; oracle JSONs/corpus YAMLs/golden TSVs untouched. |
| MASK-1 | Per-entry field masking for oracle-structural rows (§13.15, user decision 2026-07-11): 8 ERRATA-STRUCTURAL entries drop the assert on their structurally-incomparable field(s) only, keep every comparable field asserted, plain PASS | direct edit (corpus + docs) | S | **DONE 2026-07-11** — user decision 2026-07-11: adopt per-entry FIELD MASKING for the oracle-structural corpus rows. **Membership: exactly 8** — the closed 7-entry ERRATA bucket (`docs/arcade-frame-data/ERRATA.md`) MINUS `ibuki-kubiori-lp-whiff/-block` (item 18(b), a value divergence on the comparable S/A fields, never masked) PLUS the new ERRATA §5 Sean Roll ×3 (STRUCTURAL, same zero-active class as §1 Akuma Ashura Senkuu; cached FAT-3S snapshot independently corroborates sean.json's Recovery=7 on all three strengths). **Per-entry splits:** `akuma-ashura-whiff` masks R (oracle 9; measured saturated S=T=72/A=0/R=0), keeps outcome=WHIFF only (`expect: {}`) — same precedent as `dudley-ducking-whiff`. `oro-oniyama-lp-whiff/-block/-hit` mask R (oracle −29; measured 28, S+A+R=44=T, cut=1), keep S=6/A=10 asserted (`from-qjson`, both exact); block's `adv` and hit's `adv`/`kd` stay unasserted, unchanged (not newly masked — subtractive-only rule). `sean-roll-lp/mp/hp-whiff` mask R (oracle 7; measured saturated S=T=28/38/52, T=oracle Startup+Hit+Recovery sum exactly), keep outcome=WHIFF only. `ibuki-kubiori-lp-hit` masks A (oracle first-triplet 11 = precursor strike phase; this leg's A=1 = grab catch-and-throw phase) and R (oracle 15 = strike phase; measured 39 = post-capture throw recovery, T=54=S+A+R, endrel=1), keeps S=14 and kd=1 as explicit literals (both exact); adv stays unasserted, unchanged. **Excluded, named:** `sean-dragonsmash-mp-whiff/-block/-hit` (ERRATA §4 UNVERIFIABLE, a live value dispute — masking would bury the question the pending arcade capture is meant to settle) and `ibuki-kubiori-lp-whiff/-block` (item 18(b) whiff-R truncation, a value divergence on the comparable S/A fields — masking would paper over a wrong value, barred by the P-1 guard). Both stay XFAIL, unchanged. **Docs:** new synthesis §13.15 "Oracle-structural field masking (MASK-1)" — statement, membership rule (ERRATA STRUCTURAL only, comparable-field-match required, P-1 guard), 8-entry member table, exclusions, entry notation (`FIELD-MASKED (ERRATA)` anchor contract), audit semantics (xfail removed — XPASS is FAILING, `check_frame_data.py:157-159`/`:197` — golden measured-column pinning replaces the old XPASS-alert tripwire), arithmetic; one-line §12.2.4 pointer. `ERRATA.md` gains §5 (Sean Roll, STRUCTURAL) + dated header/scope/per-section cross-refs (§1-§4) + summary-table row 5 + tally update (4 STRUCTURAL + 1 UNVERIFIABLE). `CORPUS-AUTHORING.md` gains Phase-6 bucket 9 "Oracle-structural incomparability → FIELD-MASK", allowed ONLY on ERRATA-verdict STRUCTURAL, never value disputes. **Gate:** SIX lever constants asserted `=1` (`charset.c:507`; `frame_data_overlay.c:802/:1222/:1247/:1356/:1378`); tooling byte-identity vs HEAD (`compile_corpus.py`/`check_frame_data.py`/`golden.py`/`run.sh`/`run-suite.sh`), zero `src/` changes; `compile_corpus.py` exits 0 on all four touched corpora, `expected.json` diffs show exactly the 8 predicted key drops (masked fields + `xfail` → `null`) and nothing else; `grep -c "FIELD-MASKED (ERRATA)"` = akuma 1 / oro 3 / sean 3 / ibuki 1, zero elsewhere; pre-update `run-suite.sh --check-golden` drift == EXACTLY the 8 member labels, every line `verdict XFAIL->PASS`, measured columns byte-identical, zero drift elsewhere (`mask-verify-pre.log`/pass-2 log); `--update-golden akuma oro ibuki sean` scoped, diff verdict-only on exactly those 8 rows; final `--check-golden` → exit 0, zero drift, 19/19 GREEN (`mask-verify-post.log`). **Arithmetic:** post-G1/G2 baseline 990 PASS / 69 XFAIL → **990 + 8 = 998 PASS / 69 − 8 = 61 XFAIL**, rows unchanged at 1,059 (independently re-summed off the final 19-corpus golden run: 998 PASS + 61 XFAIL = 1,059 exact). Per-corpus: akuma 52/2→53/1; oro 46/6→49/3; sean 61/8→64/5; ibuki 51/7→52/6 (post-G1/G2 baseline, G1's own +2 ibuki-uoh flips already folded in); all other 15 corpora byte-identical. Zero engine/gameplay changes. |
| CAPTURE-1 | Arcade ground-truth capture lands (2026-07-11): live Fightcade FBNeo + Lua bridge captures of sfiii3 (Euro 990608), savestate-anchored, instrument validated 8/8 controls across two sessions, 2 byte-identical reps per disputed row — the first primary-hardware source this repo has ever compared against | direct edit (corpus + docs) | M | **DONE 2026-07-11.** Rig + methodology permanently documented: `docs/arcade-frame-data/CAPTURE.md` (new). **Sean Dragon Smash Strong (R1, value correction):** arcade plays S=7/A=8/R=39 on dp+MP, exactly matching this repo's own engine measurement, NOT `sean.json`'s declared Strong row (a confirmed Jab-duplicate data-entry error — Jab/Fierce both replayed their own declared rows exactly the same session, ruling out a rig-miscalibration explanation). This is the ONE sanctioned way an `expect` value moves onto a measured engine value without a display-convention adoption: the reference itself is proven wrong by a primary source. `tools/frame-data/corpus-sean.yaml`'s three `sean-dragonsmash-mp-*` entries now assert `S: 7, A: 8, R: 39` (`adv: -30` on BLOCK) as explicit literals, `xfail` removed — plain PASS on all three. `docs/arcade-frame-data/ERRATA.md` §4 upgraded UNVERIFIABLE → **ORACLE-WRONG-CONFIRMED**, closed. **G3 (R2, CLOSED DECLINED):** arcade confirms Urien UOH A=10/R=5 directly — no engine-reachable quantity (9/11/12) equals 10, so the ENGINE-5 11-vs-12 display-convention grant is declined, not deferred; `urien-uoh-*` entries stay xfail, re-cited with the capture evidence + the new **PORT-DIVERGENCE-1** finding (this engine's own accounting has no path to arcade's true value; the layer — port vs overlay — is explicitly left open, flagged not chased). See the SWEEP-3 grant list's G3 entry above and `docs/frame-data-synthesis.md`'s ENGINE-5 closure note. **item-18 register reframe (R3):** the sampled item-18/18(c) members this capture reached — `q-uoh-whiff-r` (R=5), Urien UOH R clause (R=5), `yang-senkyuutai-lk-*` (R=34), `ibuki-kazekiri-lk-*` (R=26) — all measured **arcade == oracle**, confirming the tables were right on every single sampled row; `docs/frame-data-synthesis.md` §13.13 exclusion item 2's framing changes from "the METER itself may be wrong" to "engine-side divergence confirmed vs arcade (layer unknown)" (dated append, history preserved). No conversions — every re-cited entry stays XFAIL with its arcade value in `expect`. **yang-forward (R5):** hardware confirmation added to the existing ENGINE-6 census re-cite — box appears tick 6, first connects tick 8, both values directly observed as real on real hardware; direct proof of the standing ENGINE-6 census conclusion, no new finding, no xfail change. **Gate:** SIX lever constants asserted `=1`; tooling byte-identity vs HEAD, zero `src/` changes; pre-update `run-suite.sh --check-golden` drift == EXACTLY the 3 `sean-dragonsmash-mp-*` labels (verdict XFAIL->PASS only — measured values were already the golden values), zero drift elsewhere; `--update-golden sean` scoped; final `--check-golden` → exit 0, zero drift, 19/19 GREEN. **Arithmetic:** 998 PASS + 3 = **1001 PASS**; 61 XFAIL − 3 = **58 XFAIL**; rows unchanged at **1,059** (1001+58=1059 exact). Per-corpus: sean 64/5 → 67/2 (post-MASK-1 baseline), all other 18 corpora byte-identical. Zero engine/gameplay changes; docs-and-corpus-only. **Follow-up target, queued (not captured this session):** `oro-oniyama-lp-whiff`'s masked R rests on a 2-of-3 secondary-source majority with a live EventHubs-vs-{oracle,FAT-3S} sign conflict (`docs/arcade-frame-data/ERRATA.md` §2) — first target for any future capture session, same rig (see `docs/arcade-frame-data/CAPTURE.md`'s "Follow-up targets for a future capture session" section). |
| LAYER-1 | Which layer produces the divergence (port vs overlay/meter vs revision) -- arcade rig (990512 REFERENCE session 3 + 990608 historical) x a convention-twin engine-raw probe (2 windows, env-gated, never committed) x the overlay, all under CAPTURE.md's counting rule; plus the ancestry lane | direct edit + orchestrated lanes | M | **DONE 2026-07-11.** Verdict: **12/12 clauses (a) OVERLAY/METER, zero (b) PORT, zero (c) REVISION, zero UNKNOWN** among tested rows (`docs/frame-data-synthesis.md` §13.16). PORT-DIVERGENCE-1 RETRACTED -- engine-raw urien-uoh whiff = 15/10/5, identical to arcade on both revisions; the overlay's 9/11/12 was never the only reachable quantity, raw box-A=10 is a fifth, engine-native one. Revision ruled out 7/7 re-validated rows (990512==990608 everywhere tested); data ancestry (Lane C) stays UNKNOWN but no longer gates anything. Prediction register: P1 HELD (2/2), P2 MISSED (engine re-arms where arcade does -- the overlay's early cut is not engine-real), P3 MISSED (raw box-A=10, not the predicted 11 -- new measurement, not instrument failure), P4 HELD (urien-headbutt twin R=16 bounds the instrument on an uncaptured PASS row). Dispositions this run: item-18 register METER-confirmed (dated re-cites, `docs/frame-data-synthesis.md` §13.13 exclusion item 2 + item-18(b)/(c) blocks); ENGINE-5 dated correction (fifth quantity, closure stands, `:934`); ENGINE-7 dated note (chunli reconciliation, `:910`); G2 CBK precondition VOID by measurement (arcade+twin 17/10/10, SWEEP-3 grant list G2 entry below); oro-oniyama MASK-1 dated note (hardware+twin R=29, mask stays, `docs/frame-data-synthesis.md` §13.15; `docs/arcade-frame-data/ERRATA.md` §2). Evidence pointers: this row + `docs/frame-data-synthesis.md` §13.16 + `docs/arcade-frame-data/CAPTURE.md`'s new Session 3 section + scratchpad lane files (`layer-plan.md`, `layer/a1-mapping.md`/`a2-findings.md`/`a3-counterexample-findings.md`/`ancestry-findings.md`, `capture/session3-results.md`). **Follow-up program framing -- "OVERLAY RE-ANCHOR": a census-first engine-observer program that re-derives the overlay's S/A/R from the raw-signal constructions the twin validated (raw any-of-four-s16 box predicate for A; gflg-edge strictly-between rule for R), using the twin as the full-suite ground-truth census instrument; potential to genuinely fix ~30 meter xfails (exact count is the census's first deliverable); prerequisites: contact-leg hitstop rule alignment (h-forward-hit limitation) + a PASS-row regression census (urien-headbutt shows busy-R != golden on at least one PASS row) + selective arcade verification of any PASS row the census would move. Status: NOT STARTED, user-gated as new scope.** No overlay code in this commit. **Gate:** SIX lever constants asserted `=1` (`charset.c:507`; `frame_data_overlay.c:802/:1222/:1247/:1356/:1378`); zero `src/` changes (docs + corpus YAML xfail-string/comment re-cites only); zero value/expect/golden changes predicted -- `run-suite.sh --check-golden` required zero drift, 19/19 GREEN, totals unchanged at 1,001 PASS / 58 XFAIL. **DATED NOTE (2026-07-11): the OVERLAY RE-ANCHOR follow-up this row named EXECUTED and SHIPPED — see the new RE-ANCHOR-1 tracker row below.** |
| 14 | MiSTer hardware verification pass (user-assisted) | checklist | S | |
| RE-ANCHOR-1 | LAYER-1's OVERLAY RE-ANCHOR follow-up: make the overlay's WHIFF-leg numeric A/R measure the two raw-signal constructions the twin validated -- lever O (`fd_whiff_raw_box_a`, raw box-frame A) + lever N (`fd_whiff_busy_edge_r`, busy-edge R), gated on `outcome==WHIFF && !use_proj_split && !no_active_signal && box_count>0 && box_runs==1`; full-suite census-first (Step 1), two arcade capture sessions (Session 4 busy-edge + Session 5 actionability, the ladder step for two census counterexamples) | `/plan` + `/implement` | L | **DONE 2026-07-11.** Both levers SHIPPED. Census (`<sp>/reanchor/census-report.md`) predicted the exact drift set before any edit; lever O survived clean; lever N's Criterion 1 failed on two currently-PASS windows (`ibuki-twdsforward-whiff`, `ibuki-twdsroundhouse-whiff`) sharing the Urien Headbutt trio's exact signature (oracle == overlay-natural-end == busy-edge+3). Session 4 (busy-edge, `<sp>/capture/session4-results.md`) + Session 5 (actionability probe, `<sp>/capture/session5-results.md`) resolved OUTCOME A: the busy edge IS the arcade first-actionable frame on all five rows; lever N ships ungated, the two counterexamples plus the Headbutt trio reclassify as oracle-error rows (ERRATA instances 3-7 after Dragon Smash-mp, `docs/arcade-frame-data/ERRATA.md` §§6-7). §13.11 whiff-A amendment (PENDING-USER-1) ADOPTED same day, user grant "whiff active-frames come from the raw hitbox count" (2026-07-11) -- 4 members (q/chunli/dudley/ibuki-uoh-whiff A) re-baseline onto their oracle figure of 10, each individually hardware-backed (Session-4 for chunli/dudley/ibuki, Sessions 2-3 already for q). `ibuki-kubiori-lp-whiff/-block` census-adjudicated EXCLUDED (zero raw box-active frames anywhere in the window -- the pre-existing `box_count>0` gate, not a new rule); item-18 does NOT close for this pair. **Totals: 1001->1019 PASS, 58->40 XFAIL, rows unchanged at 1059** (18 full flips, zero PARTIAL). **Gate:** EIGHT lever constants asserted `=1` (the prior SIX + `fd_whiff_busy_edge_r`/`fd_whiff_raw_box_a`, `frame_data_overlay.c:339-340`); `git diff --stat src/` == `frame_data_overlay.c` only; full-suite pre-drift == the census-predicted 26-row/cell-exact drift list, verified before any golden write; zero contact-leg drift (726 contact rows checked); scoped `--update-golden` on the 15 affected corpora only; final `run-suite.sh --check-golden` 19/19 GREEN, zero drift; lever gauntlet (N=0 -> exactly the 26-row R-side set reverts; O=0 -> exactly the 9-row A-side set reverts, incl. the 4 amendment literals; G=0/H=0 -> zero RE-ANCHOR-1 whiff rows affected, since lever N derives R from raw signals independent of the cut/rearm machinery entirely -- a stronger, cleaner result than the census's own hedged prediction, itself written before OUTCOME A was known; M=0 -> unchanged single-row E9 contract) all confirmed; determinism x2 byte-identical; twin loop-closure (patch applied on top of the fix, all 26 flipped/drifted rows' overlay FINAL S/A/R matched a FRESH independent twin derivation exactly, zero mismatches; patch reverted, md5-verified, clean rebuild, zero drift). See `docs/frame-data-synthesis.md` §13.17, `<sp>/reanchor-plan.md`, `<sp>/reanchor/census-report.md`+`.tsv`, `<sp>/reanchor/leverN-rediagnosis.md`, `<sp>/capture/session4-results.md`+`session5-results.md`. |
| CONTACT-1 | §13.17's recorded contact scope-out, executed as a program: census-first contact-side counting-rule design (lever P contact-R + conditional lever Q contact-A) intended to reach full-suite census + arcade capture + ONE payload commit, per `<sp>/contact-plan.md` | Step 0 offline rule-fit (zero builds/captures) -> Step 1 fresh census -> Step 2 capture -> Step 3 user checkpoint -> Step 4 implement | L (attempted at Step 0 only) | **ATTEMPTED, CLOSED-RECORDED-FOR-LATER at the Step-0 exit gate (2026-07-12).** Per-tier numbers (`<sp>/contact/step0-report.md`, full data `<sp>/contact/step0-fit.tsv`, 726 rows, zero builds/captures/repo writes): Rule P FAILED survival — **8 C1 + 19 C2 = 27 movers** among currently-correct rows, resolving into the single-run "+1 freeze-inside-active" UOH family (15 rows: 8 C1 + 7 C2, e.g. `akuma-uoh-block`, `q-uoh-samef-hit`) and the uniform "-3" `urien-headbutt-*-block/-hit` family (6 rows), plus 6 outliers (`h-meatsquasher-*-hit` x3, `ibuki-twdsforward-block`, `remy-throw-hit`, `yun-crfierce-block`); Rule Q REJECTED outright (490/492 movers, matching its own LOW pre-registration). T1's 4 rows (`ibuki-kazekiri-lk-block/-hit`, `yang-senkyuutai-lk-hit`, `yun-uoh-hit`) fit their oracle value exactly but are unshippable under a rule that fails its own zero-mover gate. Positive finding preserved: the multi-hit chain "last-active across ALL runs" reordering is independently exact (0 movers) on `sean-tornado-hk-block`/`dudley-mgb-lp-block`/`q-hsb-jab-block` — reusable by a future design. Zero repo edits, zero builds, zero census, zero captures spent (Steps 1-4 never started) — the offline-fit method (a first for this project: kill a candidate at its own survival gate before any build) paid for itself. **Design-#2 door:** open, gated on naming a mechanism for BOTH failure shapes above and reaching 0/0 C1/C2 movers in a fresh offline fit before any window/capture spend; not attempted this cycle. See `docs/frame-data-synthesis.md`'s new §13.17 dated note (CONTACT-1 closure record) for the full evidence writeup. |
| CONTACT-2 | The design-#2 door CONTACT-1 left open: a mechanism-named contact-side R counting rule (lever R, `fd_contact_busy_edge_r`) — the SAME busy-edge construction lever N already ships on WHIFF (§13.17), extended to HIT/BLOCK behind a gate (G1/G4/G5/G6/G7) proving each conjunct against a named census counterexample, per `<sp>/zero/contact2/design.md` | Step 0 offline fit (726 rows, 19 corpora, zero builds) -> Step 1 on-device census + diagnostics-only instrumentation (one commit) -> Step 2 payload (lever wire + corpus/golden/docs, one commit) | L | **SHIPPED 2026-07-13.** Step 1 (`ac89cc8b`) landed the diagnostics-only instrumentation (`hstop_after_box`/`move_is_uoh`/`koc` tick-side fields, `fd_engine_move_is_uoh[2]` engine dispatch tag in `check_leap_attack()`, additive FINAL keys) and ran the full 94-corpus on-device census (1,347 windows), which tripped Gate 1 on four deviations (D1-D4) outside the original 726-row/19-corpus offline fit's scope — honored as a STOP, not forced. AMENDMENT 1 (`design.md` §8) adjudicated all four (D1/D4 safe-direction instrument artifacts; D2 fourteen additional genuine gate-passers, twelve ADMITted with cited values + two HELD on an arcade capture; D3 the G4 UOH-exclusion fallback falsified and withdrawn, dispatch-tag form is sole/sufficient), added gate condition **G7** (`recovery_pf > 0`, excludes the ENGINE-8/jinchu family), and grew the payload to 30 pre-registered rows. Arcade Session 10 (`<sp>/zero/arcade-s10/session-report.md`) resolved the one pre-registered blocker (`urien-exheadbutt-block/-hit`, held per §8.3.7) Branch A — both legs measure R=12 on real hardware, 2 reps byte-identical each, actionability-probe-confirmed — growing the shipped payload to **32 rows**. Step 2 (this commit) wired the amended gate into `recovery` (the else-if branch between lever N's whiff branch and the legacy `recovery_pf` fallback) and shipped the full 32-row corpus/golden payload across 14 corpus YAMLs. Gauntlet: Gate 1' re-census (post-edit, race-free re-run) matched the 32-row list exactly, zero extras, all four jinchu rows `pred=-1`; full-suite `--check-golden` zero drift post-`--update-golden`; identity (lever 0) byte-identical to the `ac89cc8b` baseline on all 94 corpora; determinism x2 on 3 payload corpora + 1 non-member; ten lever constants (`F,G,H,I,J,M,N,O,S,R`) asserted `=1`; `git diff --stat src/` exactly the five Step-1 files (`frame_data_overlay.c`, `pls03.c`, `workuser.c`/`.h`, `main.c`), zero `tools/frame-data/*.py` changes. Suite delta **1,266/81 -> 1,270/77** (+4 PASS / -4 XFAIL), matching the design's own predicted arithmetic exactly. See `docs/frame-data-synthesis.md`'s new §13.19 (lever R record) for the full rule/gate/counterexample writeup. **Coordination note:** `docs/arcade-frame-data/CAPTURE.md` does NOT gain Session 6/Session 10 entries in this commit — those ride the separate UOH-program payload commit (`<sp>/zero/uoh-fit/`), which owns the CAPTURE.md session-log update end-to-end; this commit's own arcade citations point at the scratchpad session reports directly (`<sp>/zero/arcade-track-a/session-report.md`, `<sp>/zero/arcade-s10/session-report.md`). **G4 boundary AMENDED 2026-07-13 (UOH-CLOSURE, see next row):** the flat `!move_is_uoh` exclusion this row shipped is relaxed by lever T into `!move_is_uoh || (fd_uoh_contact_busy_edge_t && hstop_in_box > 0)` — this row's own 32-row payload and gate conjuncts are untouched (lever T's disjunct only reaches the branch this row's G4 used to exclude outright); this row's own citation of "does NOT gain Session 6/Session 10 entries" is itself corrected by UOH-CLOSURE's own CAPTURE.md work: Session 10 turned out to BE the exheadbutt capture this row already cites (`<sp>/zero/arcade-s10/session-report.md`), not the "freeze-overlap discriminator analysis" placeholder UOH-CLOSURE's own design doc guessed at design time (its `raw/` was empty when that guess was made) — CAPTURE.md's new Session 10 entry documents the real exheadbutt content; **CORRECTION (this commit):** Session 6 is now documented in CAPTURE.md — the claim above that its raw artifacts were "not present in this scratchpad session" was false: `<sp>/zero/arcade-track-a/` (30 raw capture files + `session-report.md` + `derive2.py`) was present the whole time and is this row's own cited source (see the "Session 6" citation earlier in this same row). CAPTURE.md's new Session 6 entry is authored directly from that session report, shipped alongside UOH-CLOSURE's own Sessions 7-11 entries in this same payload. |
| UOH-CLOSURE | The one hard counterexample CONTACT-2 declined to resolve: whether the busy-edge/lever-R construction (§13.19) also covers the UOH ("Universal Overhead") contact class CONTACT-2's own G4 term flatly excluded — lever T (`fd_uoh_contact_busy_edge_t`), amending G4 to a disjunct gated on a new passive counter (`hstop_in_box`), per `<sp>/zero/uoh-fit/uoh-design.md` | Step 1 (diagnostics-only instrumentation + full-universe census, one commit) -> SESSION 11 (arcade re-capture of the one suspect BLOCK-leg population, off-machine) -> Step 2 (gate wire + 31-cell/2-new-row corpus/golden/docs payload, one commit) | L | **SHIPPED 2026-07-13.** Step 1 landed the diagnostics-only `hstop_in_box` counter + `leverT_pred` FINAL key (`frame_data_overlay.c`, byte-identical goldens) and a full 94-corpus on-device census that found the drafted discriminator (`hstop_in_box > 0`) is a blanket "UOH contact → busy-edge" rule on the current verified universe — the attacker's freeze lies inside the raw box on all 33 engine UOH contact windows, with no engine observable separating a "legacy" population from a "busy-edge" one. Forensic re-derivation of session 8's own raw tapes (before any re-capture) showed its five "legacy-siding" BLOCK captures (ryu/ken/oro/sean/q) plus Chun-Li's were WHIFF-shaped, not block-shaped (raw box length == whiff-canonical active count, zero freeze anywhere, wall-clock span == whiff S+A+R) — RETRACTING the "hardware sides with legacy" reading as unproven and turning those six legs into a SESSION 11 capture blocker instead of a shipped counterexample. SESSION 11 (`<sp>/zero/arcade-s11/session-report.md`) re-captured all seven suspect rows (6 legs + `q-uoh-chain-retrigger`) with the dummy cornered (walkback structurally impossible) and a mandatory in-tape contact witness: **BRANCH A on all 7, zero counterexamples** — ryu/ken/oro/sean/q-samef-block each +1 over legacy exactly matching the busy-edge prediction, chunli-block display-invariant at 4 (confirming session 8's "5" reading there was the same whiff artifact, NOT a genuine divergence — the pre-registered PASS→XFAIL exception does not apply). Step 2 wired the amended G4 disjunct into `fd_lever_r_applies` (one term, same file lever R already lives in — no `pls03.c`/`workuser.c`/`main.c` changes needed, unlike CONTACT-2, since lever T reuses CONTACT-2's already-shipped `move_is_uoh` dispatch tag) and shipped the full 31-cell + 2-new-row corpus/golden payload across 17 corpus YAMLs (16 XFAIL→PASS conversions from the XFAIL-18 bucket minus `urien-uoh-block/-hit`, which narrow to an A-side-only residual and stay XFAIL; 13 currently-PASS value corrections carrying per-row session citations; 2 new rows `ryu-uoh-hit`/`ken-uoh-hit`). Gauntlet: pre-drift census matched the pre-registered 31-cell + 2-row disposition EXACTLY, zero deviation; full-suite `--check-golden`/`--update-golden` zero drift post-update, totals exactly 1,288/61/1,349; G-identity (lever T=0, rebuild) zero MEASURED-column drift on all 94 pre-existing corpora against the `86c5ad8b` baseline (the only changes were verdict flips on the payload's own edited corpus rows, an `expect.R`-literal artifact, not a lever-identity violation; the 2 new rows are documented T-dependent and excluded from the identity comparison); determinism x2 on `ryu`/`ken`/`q` (payload) + `hugo` (non-member), all byte-identical; **eleven** lever constants (`F,G,H,I,J,M,N,O,S,R,T`) asserted `=1`; `git diff --stat src/` exactly `frame_data_overlay.c`, zero `tools/frame-data/*.py` changes. Suite delta **1,270/77 -> 1,288/61** (+16 PASS / -16 XFAIL from conversions, +2 new PASS rows, +2 total rows), matching the design's own predicted arithmetic exactly. A does NOT convert anywhere in this payload (arcade A disagrees with engine A in both directions across characters, no uniform construction — same finding class as CONTACT-2 §4.2). See `docs/frame-data-synthesis.md`'s new §13.20 (lever T record) for the full rule/discriminator-story/family-closure writeup, and `docs/arcade-frame-data/CAPTURE.md`'s new Sessions 7–11 entries + the `derive2.py` adoption note (this payload carries the capture-doc updates per CONTACT-2's own coordination note, extended to the sessions this program's own design doc scoped: 7-11; Session 6 also gains its own CAPTURE.md entry in this same payload, authored from `<sp>/zero/arcade-track-a/session-report.md` — the CONTACT-2 row's own adjacent correction note applies here too; "stays undocumented" no longer holds). **WANTS FABLE RE-REVIEW:** this payload's diff review ran opus-substitute (Fable plan-access quota exhausted 2026-07-13 mid-review, `<sp>/zero/FABLE-REREVIEW-QUEUE.md`) — re-review scope is the lever-T G4 disjunct plus this row's own "unconditional for UOH contacts" honesty framing; the design + Step-1 census + CONTACT-2/ENGINE-D2 were Fable-reviewed earlier, only this payload's diff review fell to opus. |
| EXSUPER-1 | EX/Supers corpus expansion program: pre-registered implementation plan (`<sp>/exsuper-plan.md`) plus a 5-ruling disposition (`<sp>/exsuper/authoring-policy.md`) governing whiff-coverage geometry, contact-leg licensing, multi-run supers, arcade scope, and batch structure for the ~130-134 new EX/super moves (71 EX + 75 super oracle rows) | `/plan` (ruling) + `/implement` per commit | XL (multi-commit program) | **COMPLETE 2026-07-13** (opened 2026-07-12; authoring waves S1-S6 + the EXSUPER-1 closing cleanup all landed — see the dated wrap-up paragraph at the end of this cell for the full closure). Steps 0/1/2a/2b DONE (one-line outcomes): **Step 0** — 146-row assertable-universe TSV derived from all 19 oracle JSONs (71 EX + 75 super rows), exact match to the plan's G9 sizing, zero ambiguous super-block boundaries (`<sp>/exsuper/step0-universe.tsv`+`step0-report.md`). **Step 1** — `--test-training-sa-gauge <0-3>` harness route implemented; the first every-frame `init_E3_flag` re-arm caused a destructive 2-frame `spmv_ng_flag2` oscillation (caught by a temporary diagnostic print, STOP'd), root-caused and fixed same session with an edge-triggered re-arm (`training_sa_gauge_expected_flag2()` gates the re-arm on an observed mismatch only), re-verified stable: 7/8 (INFINITY) and 8/8 (MAXIMUM) Ken SA3 activations, hard no-op when unset (`<sp>/exsuper/step1-report.md`). **Step 2a** — offline sastop-gate fit over preserved Ken SA3 BLOCK traces: sastop (super-flash) ticks are pre-`MOVE_START` idle-settling artifacts, ZERO ever land inside a move's own S/A/R window (10/10 segments checked); the R+10 sastop-exclusion hypothesis is REFUTED on this data (`<sp>/exsuper/step2a-report.md`). **Step 2b** — pilot-shape scratch probes (Chun-Li SA2 single-run, Ken SA3 multi-run `box_runs=12`, Ryu SA1 proj-split): every oracle-checkable value matched exactly; verdict **O-C (mixed, per-move), leaning O-A-supportive** — no lever or signal misbehaved anywhere this session (`<sp>/exsuper/step2b-report.md`). **Policy pointer:** `<sp>/exsuper/authoring-policy.md`'s 5 rulings — Ruling 1 (whiff-coverage geometry, opt-in `dist_mode: centered`), Ruling 2 (contact-leg licensing via the legacy path + per-entry admissibility census, not box/busy agreement), Ruling 3 (multi-run supers field-by-field), Ruling 4 (arcade scope, deferred-but-scheduled after the pilot trio), Ruling 5 (batch structure: Commit 0 below, then a tooling micro-batch, then EX-first per-character, then the supers pilot trio). **Quarantine seed (named, not shipped):** Ken SA3 BLOCK R=37 vs oracle 27 (+10, UNEXPLAINED) — sastop-exclusion mechanism REFUTED (Step 2a); awaiting Ruling 4's arcade adjudication session. **Commit 0 (this commit) — harness/tooling landed, zero corpus authoring yet:** the `[gauge-probe]` diagnostic print stripped from `test_runner.c` (`git grep gauge-probe` → zero hits); 20-corpus byte-identity recompile (`script.fdi`/`expected.json`/`meta.json`) clean for all 20 `tools/frame-data/corpus-*.yaml` (none use the new `super_art`/`sa_gauge` keys); full `run-suite.sh --check-golden` **19/19 GREEN, zero drift, 1,019 PASS / 40 XFAIL / 1,059 rows** (baseline exactly reproduced — the new flag/keys are no-ops for every existing corpus, including the hugo zero-behavior spot: `hugo 30/30 PASS`, golden-exact); the standing EIGHT lever constants (`frame_data_overlay.c:339-340,897,1360,1385,1494,1516`; `charset.c:507`) all still `== 1`, unaffected (this commit touches no engine/overlay file); E4 cross-build (`tools/mister/build-game.sh --flavor telemetry`) clean, zero `InputScript`/`training_sa_gauge`/`gauge-probe` symbols in the ARM binary (`#if DEBUG`-scoped, doesn't reach the shipped build). See `docs/plan-frame-data-harness.md` §1.9's dated 2026-07-12 update for the harness-surface writeup. **Tooling micro-batch + Ryu EX pilot (this commit, 2026-07-12, ~42min wall-time — the program's first wall-time datapoint, plan Step 5's pre-registered checkpoint metric).** **Tooling:** `setup.dist_mode: centered` landed in `compile_corpus.py` (opt-in, absent ⇒ today's anchored placement, byte-identical) — places the pair symmetrically about a new `CENTER_X=500` instead of anchoring P1 at `NUMERIC_BASE_X=424`; one probe run (`P 300 700`, read `P1.x`/`P2.x` off the MOVE_START annotation) measured an achieved separation of **328px** for a requested 400px symmetric gap, matching Ruling 1's recorded "~320-332" observation almost exactly; `CENTERED_DIST_MAX=310` set 18px inside that ceiling (enforced as a compile-time error, same discipline as the existing `DIST_MAX=300`). The `DIST_MAX` comment's falsified "safe below 300" claim (E-7) is corrected in place: 9 shipped `dist:300` entries actually run at ~252px (frozen, no golden movement). **Gate:** 20-corpus byte-identity recompile (`script.fdi`/`expected.json`/`meta.json`, before/after diff, zero bytes differ) + hugo zero-behavior spot run (30/30 PASS, golden-exact, matching the CORPUS-AUTHORING.md Phase-7 canary). **Ryu EX pilot** (`tools/frame-data/corpus-ryu-ex.yaml`, new satellite, `golden/ryu-ex.tsv` born): Ryu's 4 ground EX rows (step0-universe rows 104-107; row 108 Air Tatsumaki EX stays AIR_DEFERRED). Motion recipes: EX Hadouken/Shoryuken via qcf/dp hold-chains with an LP+HP chord on the final step (Ken's own Step-13-spike precedent); EX Tatsumaki via a qcb hold-chain + LK+HK; EX Joudan Sokutou Geri's base motion had NO in-repo precedent (`corpus-ryu.yaml` never authored it) — identified this session by a 5-candidate motion probe as `hcf` (half-circle-forward) + LK+HK, confirmed via S/R exact match (S=13/R=27) against no other candidate. **Meter finding:** the first authoring attempt (no `sa_gauge` key) silently fell back to each move's plain (non-EX) equivalent on every entry except Joudan — training mode's default gauge is not reliably full; `sa_gauge: 3` (MAXIMUM, per policy) at the corpus top level fixed all four moves deterministically. **Outcomes:** EX Hadouken (proj-split, §13.12) BLOCK+HIT both oracle-exact (S=9/R=36/adv=0), no WHIFF leg (documented gap, same fireball-can't-whiff precedent as `corpus-ryu.yaml`'s own Hadouken section). EX Shoryuken BLOCK+HIT+WHIFF all 4 fields oracle-exact (S=2/A=19/R=38/adv=-37) — the cleanest of the four. EX Tatsumaki Senpuu Kyaku: S/A/adv oracle-exact on BLOCK+HIT, WHIFF fully oracle-exact (S=10/A=12/R=11); genuinely multi-run (`box_runs`=4/2/6 on BLOCK/HIT/WHIFF, the Ruling-3 shape) — its BLOCK/HIT R diverges from its own oracle-exact WHIFF figure (measured 14/16 vs oracle 11) with no established Phase-6 bucket confirmed this session (a §12.2.2 R-surplus *candidate*, own-ledger trace not done — budget-bounded); **QUARANTINE #2 this program** (R omitted on both contact legs, not xfailed-unexplained, per the plan's PER-MOVE QUARANTINE rule; Ken SA3 BLOCK R+10 is quarantine seed #1/#0-supers). EX Joudan Sokutou Geri: BLOCK all 4 fields oracle-exact; HIT's A fails the Ruling-2 contact-A juggle check (measured 1 vs BLOCK's 4) so A is documented-unasserted (not quarantined — the check exists for exactly this shape); WHIFF is **WHIFF-UNREACHABLE(dist=300, walk_in≈83px)** — even at `CENTERED_DIST_MAX`'s practical ceiling the move's own pre-`MOVE_START` walk-in (back-hold before the forward lunge) eats enough of the requested separation that it still connects; documented per Ruling 1, no entry shipped for it. **Census:** single-FINAL integrity held after raising `inter_entry_wait` to 250 (one label-bleed found and fixed at 150 — Joudan's own HIT leg, the Hugo lesson); sastop=0 across every entry (EX moves are flash-free, confirmed). **Gates:** determinism x2 byte-identical; full-suite `run-suite.sh --check-golden` pre-birth showed the ONLY drift as `ryu-ex`'s missing golden (19/19 existing corpora untouched, canary totals exact: q 73/73, ryu 37/37, hugo 30/30); scoped `--update-golden ryu-ex`; final full-suite check **20/20 GREEN, zero drift**. **Arithmetic: 1,059 + 10 = 1,069 rows (1,019 + 10 = 1,029 PASS / 40 XFAIL unchanged)**, independently re-summed off the 20-corpus golden run. See `<sp>/exsuper/ryu-pilot-report.md` for the full per-move table and `<sp>/exsuper-pilot-commit-msg.txt` for the commit message. **Wave 1 (this commit, 2026-07-12): ken-ex + chunli-ex satellite corpora, file-disjoint sibling batches, akuma scoped out.** **Ken** (`tools/frame-data/corpus-ken-ex.yaml`, 7 entries, ALL ORACLE-EXACT, zero quarantines): EX Hadouken (proj-split, BLOCK+HIT exact, no WHIFF leg — the same fireball-can't-whiff gap as every other Hadouken variant in this program); EX Shoryuken (BLOCK+HIT+WHIFF all 4 fields exact); EX Tatsumaki Senpuu Kyaku (BLOCK+HIT exact on all 4 fields, genuinely multi-run `box_runs=6` per Ruling 3 but with ZERO divergence — unlike Ryu's own EX Tatsumaki quarantine; WHIFF-UNREACHABLE at the hard `CENTERED_DIST_MAX=310` cap, root-caused to the move's own multi-hit spin covering the whole centered ceiling, not a walk-in artifact). **Chun-Li** (`tools/frame-data/corpus-chunli-ex.yaml`, 8 entries, ALL ORACLE-EXACT, zero quarantines): Kikkoken EX (proj-split, BLOCK+HIT exact, same Hadouken-class gap, no WHIFF); Hazan Shu EX (BLOCK+HIT+WHIFF all 4 fields exact, cleanest possible shape); Spinning Bird Kick EX (BLOCK+HIT+WHIFF exact on both assertable fields, oracle publishes no numeric R/adv for this move family). **Hyakuretsu Kyaku (EX) primitive-gap note:** NOT landed — two motion attempts (single 2-kick chord; 3-tap chord mash) both measured back as a plain standing Roundhouse (oracle-exact match to chunli.json's Roundhouse row, not any Hyakuretsu variant); same root cause as `corpus-chunli.yaml`'s own documented non-EX Hyakuretsu gap, a same-button/chord re-input buffer timing window this harness's primitives don't express — out of scope, no entry shipped, no guess made (`<sp>/exsuper/chunli-ex-report.md`). **Akuma null-EX finding:** zero `class=EX` rows for Akuma anywhere in `step0-universe.tsv` and zero move names containing "ex" (case-insensitive) in `docs/arcade-frame-data/akuma.json` — Akuma has no EX-class content to author under this program's own scoping rule (his meter only buys Super Arts, including two secret-input supers); no corpus file created, read-only session, zero edits/builds/runs (`<sp>/exsuper/akuma-ex-report.md`). **Gates:** pre-birth full-suite `run-suite.sh --check-golden` showed the ONLY drift as `ken-ex`/`chunli-ex`'s missing goldens (20/20 existing corpora untouched, zero drift); scoped `--update-golden ken-ex chunli-ex`; final full-suite check **22/22 GREEN, zero drift, exit 0**. **Arithmetic: 1,069 + 15 = 1,084 rows (1,029 + 15 = 1,044 PASS / 40 XFAIL unchanged)**, independently re-summed off the 22-corpus golden run (per-verdict tally across all 22 `golden/*.tsv` files: 1,044 PASS / 40 XFAIL / 1,084 total). See `<sp>/exsuper/ken-ex-report.md` and `<sp>/exsuper/chunli-ex-report.md` for the full per-move tables. **Wave 2 (this commit, 2026-07-12): dudley-ex + yun-ex + ibuki-ex satellite corpora, file-disjoint sibling batches, 7 entries each.** **Dudley** (`tools/frame-data/corpus-dudley-ex.yaml`, 7/7 PASS, zero XFAIL): Jet Uppercut (EX) WHIFF fully oracle-exact, HIT A documented-unasserted (juggle-check shape already flagged in `corpus-dudley.yaml`'s own base move), BLOCK R **QUARANTINE #3** (measured 35 vs oracle 34, +1, breaks the base move's own R-exact BLOCK pattern). Machine Gun Blow (EX) A/adv oracle-exact/established-convention, BLOCK+HIT R **QUARANTINE #4** (measured 25/13 vs oracle 28/28, breaks the base move's own two-tier R shape), WHIFF-UNREACHABLE (move-reach). Short Swing Blow (EX) BLOCK+HIT fully oracle-exact on S/A, WHIFF-UNREACHABLE (walk-in). Cross Counter (EX) NOT AUTHORED (`NONNUMERIC_UNACCOUNTED`, dudley.json publishes zero frame data at any strength, same gap as the base move's own Jab/Strong/Fierce). **Yun** (`tools/frame-data/corpus-yun-ex.yaml`, 5 PASS / 2 XFAIL): Tetsu Zankou (EX) BLOCK+HIT fully oracle-exact, WHIFF-UNREACHABLE (genuine move-reach). Zesshou Hohou (EX) BLOCK+HIT ship as **2 XFAIL** (A=16/R=11 vs oracle 15/12, sum-preserving boundary shift — the identical signature to `corpus-yun.yaml`'s own base-move UNCLASSIFIED finding, §12.2.4's sum-preserving A/R family, cited not silently assumed), WHIFF-UNREACHABLE (same harness gap as base Fierce). Nishou Kyaku (EX) BLOCK+HIT+WHIFF all fully oracle-exact, genuinely multi-run (`box_runs=2`) with zero divergence — same clean shape as Ken's own EX Tatsumaki. Zero program quarantines this lane. **Ibuki** (`tools/frame-data/corpus-ibuki-ex.yaml`, 7/7 PASS, zero XFAIL, zero quarantines): Kubiori (EX) BLOCK S/A oracle-exact (R/adv omitted, item 18(b), same mechanism as mains), HIT field-masked (two-phase move, same structural finding as mains), WHIFF-UNREACHABLE (EX dash-in reaches farther than base). Kazekiri (EX) WHIFF+BLOCK fully oracle-exact, HIT R diverges (measured 29 vs its own oracle-exact 33, UNCLASSIFIED, documented-unasserted not quarantined). Tsumuji (EX) BLOCK+HIT S/R/adv oracle-exact, A undercounts by 1 on both legs (established §13.13 shape (a1), explicit literal per precedent), WHIFF-UNREACHABLE (genuinely multi-hit, `box_runs=4` on the unshipped attempt). Kunai (EX) and Hien (EX) MOTION NOT FOUND (21 combined candidates exhausted, no entries shipped, no oracle edit, no compiler change). **CLASSIFICATION CROSS-CHECK (wave-2 integration gate):** Kunai (EX)'s and Hien (EX)'s MOTION NOT FOUND findings were re-examined against a "step0 classification miss, not a real gap" hypothesis. **Kunai (EX) — RECLASSIFIED.** `corpus-ibuki.yaml`'s own mains header (predating this program, 2026-07-08) already documents Kunai as an "airborne-capable projectile - the harness attacker cannot be scripted to jump for the air variant"; this session's 10 exhausted ground-motion candidates reproduce that SAME pre-existing harness gap on the EX side, not a fresh motion-primitive gap. Kunai's Name has no "air" substring, so `derive_universe.py`'s name-pattern `is_air` regex never caught it (the exact blind spot already named-and-fixed once, manually, for Akuma's Tenma Gou Zankuu via `AIR_SUPER_EXPLICIT`) — a genuine Step 0 classification miss. `<sp>/exsuper/step0-universe.tsv` row 46 (ibuki index 39) `exclusion_flag` corrected `ASSERTABLE` → `AIR_DEFERRED`, dated citation added to its own `move_shape_notes` column. **Hien (EX) — NOT reclassified**, stays `ASSERTABLE`: mains' own header calls Hien a "dash-in kick special" with no air-context claim anywhere (unlike Kunai); the alternative "Kasumi-Gake-follow-up-context" hypothesis was directly tested this session (candidates 6-9, a 0/6/12/20-frame post-dash gap) and DISCONFIRMED — all four gaps re-buffered Kasumi Gake's own S=25 (its Fierce-strength value), never Hien's S=26. Verdict: genuine motion-primitive gap, not a Step 0 error; `<sp>/exsuper/step0-universe.tsv` row 47 gets a dated `REVIEWED` note recording the disconfirmed hypothesis, `exclusion_flag` unchanged. Both verdicts also recorded in `corpus-ibuki-ex.yaml`'s own header. **Quarantine tally (systemic-trigger check, authoring-policy.md's "≥3 same-shaped quarantines in a batch ⇒ STOP"):** program total is now 4 named quarantines — #1 Ken SA3 BLOCK R+10 (SUPER, surplus, BLOCK-only), #2 Ryu EX Tatsumaki BLOCK+HIT R (surplus, both legs), #3 Dudley Jet Uppercut (EX) BLOCK R+1 (surplus, BLOCK-only), #4 Dudley Machine Gun Blow (EX) BLOCK+HIT R (deficit, both legs). No shape repeats 3+ times: #1/#3 share "BLOCK-only R surplus" but differ in class (SUPER vs EX) and magnitude (+10 vs +1); #2/#4 share "BLOCK+HIT R divergence, no established Phase-6 bucket" but are opposite-signed (surplus vs deficit) — dudley-ex's own batch-level check already confirmed its 2 quarantines are "different signs/moves", and no cross-batch shape reaches the ≥3 threshold either. No STOP triggered. **Gates:** pre-birth full-suite `run-suite.sh --check-golden` showed the ONLY drift as `dudley-ex`/`yun-ex`/`ibuki-ex`'s missing goldens (22/22 existing corpora untouched, zero drift — canary totals exact, 1,084 rows reproduced); scoped `--update-golden dudley-ex yun-ex ibuki-ex`; final full-suite check **25/25 GREEN, zero drift, exit 0**. **Arithmetic: 1,084 + 21 = 1,105 rows (1,044 + 19 = 1,063 PASS / 40 + 2 = 42 XFAIL)**, independently re-summed off the 25-corpus golden run. See `<sp>/exsuper/dudley-ex-report.md`, `<sp>/exsuper/yun-ex-report.md`, and `<sp>/exsuper/ibuki-ex-report.md` for the full per-move tables. **Wave 3 (this commit, 2026-07-12): yang-ex + makoto-ex + elena-ex satellite corpora, file-disjoint sibling batches.** **Yang** (`tools/frame-data/corpus-yang-ex.yaml`, 3 PASS / 2 XFAIL, zero quarantines): Tourou Zan (EX) fully oracle-exact on every field (WHIFF+BLOCK+HIT). Senkyuutai (EX) ships 2 XFAIL on R (+downstream BLOCK adv) — the identical +4-magnitude "cut-committed overshoot" shape already registered for this same character's own Senkyuutai (Short) contact legs in `corpus-yang.yaml`, explicitly named out of RE-ANCHOR-1's fix scope (contact legs excluded there); not a new UNCLASSIFIED finding. Two traps found/resolved: a genuine distance-dependent Startup on Senkyuutai (EX) (a forward-traveling multi-part hitbox whose first-contact tick scales with achieved separation — root-caused as a real move-reach fact, not an ENGINE-6 divergence; ships at `dist:310/centered` to reproduce oracle S=29 exactly, per Ryu's Far Jab precedent for binary-searching the actual connecting distance), and an ordering/momentum-bleed corruption when placing two adjacent connecting Senkyuutai (EX) legs back-to-back (fixed by reordering the shipped corpus: BLOCK first, HIT last, buffered by Tourou Zan (EX)'s own three legs). **Makoto** (`tools/frame-data/corpus-makoto-ex.yaml`, 9/9 PASS): Hayate EX, Oroshi EX, Fukiage EX (WHIFF-only, BLOCK/HIT confirmed live UNREACHABLE) all land oracle-exact or exact-where-assertable. Karakusa has NO EX row (verified directly against makoto.json's SUPER-block boundary) — the expected grab-family EX handling simply doesn't apply here. Hayate EX ships **QUARANTINE #5** (A+R, both BLOCK and HIT, OMIT+log — A undercounts on both legs matching the base-Hayate family's own established skip-jump shape but without a lever-F toggle test to confirm family membership; R=20 both legs vs oracle 24, a hit-branch-adjacent shape that doesn't cleanly meet §13.10 Class 1 since BLOCK also diverges — UNCLASSIFIED, flagged for follow-up). Tsurugi EX ships **QUARANTINE #6** (BLOCK adv, measured +7 vs oracle +3 — BLOCK-leg R+1 over its own WHIFF-leg R, coincident with a defender-contact event WHIFF never reaches — UNCLASSIFIED, flagged for follow-up). **NAMED FOLLOW-UP (a) — AIR-INPUT CAPABILITY DISCOVERED:** Tsurugi EX is reachable, contrary to `corpus-makoto.yaml`'s own header, which excludes it citing "the harness attacker cannot be scripted to jump" against a `docs/frame-data-synthesis.md` §12.1 precedent that doesn't actually apply on inspection (that precedent is about the *defender* jumping into an anti-air grab, not an airborne attacker special, and §12.1 has zero mentions of Tsurugi). Tested directly this session: `press UP; wait 3; motion qcb LK` lands the real move (S=14/A=4, exact match), while the grounded motion reads as a plain standing Short. The harness CAN script an airborne attacker input — `corpus-makoto.yaml`'s "cannot jump" exclusion was an untested assumption, not a verified harness gap. Recorded as a named follow-up only (NOT reclassifying the mains TSV wholesale this session, file-disjoint scope): a future pass could gain 4+ Tsurugi mains rows (makoto.json's only "a."-prefixed Common_name family, the same jump-prefix convention as "n.j."/"j.") — **named follow-up (c)** below tracks this mains-corpus gap specifically. **Elena** (`tools/frame-data/corpus-elena-ex.yaml`, 13/13 PASS, zero quarantines): Mallet Smash (EX), Scratch Wheel (EX), Spinning Scythe (EX), Rhino Horn (EX) all fully oracle-exact or exact-where-assertable (Spinning Scythe (EX) HIT's 1-frame R undershoot converts to a plain PASS per the established 2026-07-10 F2b Class-1 compliance note). Mallet Smash (EX) has a confirmed MINIMUM engagement distance (~65-94px dead band, the inverse of the program's usual reach-ceiling findings) — ships at `dist:100`. **NAMED FOLLOW-UP (b) — FD_METER_LEN=72 SATURATION SIGNATURE:** Lynx Tail (EX) is fully oracle-exact on S/A but its R/adv are OMITTED (not quarantined) on all three legs — every affected FINAL reads T=72 exactly, matching the compile-time `FD_METER_LEN` constant (`frame_data_overlay.c:42`), with the per-tick trace showing the recovery chart still climbing at the last recorded tick; reproduced in two independent runs. A same-session control (Spinning Scythe (EX) BLOCK) shows the identical T=72 signature yet its own R IS oracle-exact, so the signature alone doesn't prove truncation in general — root cause not established this session, worth a register note against `FD_METER_LEN` as an instrument-limit candidate, flagged for follow-up. **Quarantine tally (systemic-trigger check, authoring-policy.md's "≥3 same-shaped quarantines in a batch ⇒ STOP"):** program total is now 6 named quarantines — #1 Ken SA3 BLOCK R+10 (SUPER, surplus, BLOCK-only), #2 Ryu EX Tatsumaki BLOCK+HIT R (surplus, both legs), #3 Dudley Jet Uppercut (EX) BLOCK R+1 (EX, surplus, BLOCK-only), #4 Dudley Machine Gun Blow (EX) BLOCK+HIT R (EX, deficit, both legs), #5 Makoto Hayate EX BLOCK+HIT A+R (EX, deficit, both legs), #6 Makoto Tsurugi EX BLOCK adv (EX, surplus, BLOCK-only). Re-checked honestly across the full program ledger: the closest shape overlaps are "BLOCK-only R surplus, EX class, +1 magnitude" (#3 and #6's underlying R clause) at 2 members, and "BLOCK+HIT R divergence, no established bucket, deficit sign" (#4 and #5's R clause) also at 2 members — no group reaches the ≥3 threshold. No STOP triggered. **Gates:** pre-integration full-suite `run-suite.sh --check-golden` showed the ONLY drift as `yang-ex`/`makoto-ex`/`elena-ex`'s missing goldens (25/25 existing corpora untouched, zero drift, 1,105 rows reproduced); scoped `--update-golden yang-ex makoto-ex elena-ex`; final full-suite check **28/28 GREEN, zero drift, exit 0**. **Arithmetic: 1,105 + 27 = 1,132 rows (1,063 + 25 = 1,088 PASS / 42 + 2 = 44 XFAIL)**, independently re-summed off the 28-corpus golden run (verified against the suite's own per-corpus PASS/XFAIL totals). **NAMED FOLLOW-UP (c) — Makoto Tsurugi mains-corpus gap:** see (a) above; tracked here for visibility in the tracker's own follow-up list. See `<sp>/exsuper/yang-ex-report.md`, `<sp>/exsuper/makoto-ex-report.md`, and `<sp>/exsuper/elena-ex-report.md` for the full per-move tables. **Wave 4 (this commit, 2026-07-12): oro-ex + urien-ex + alex-ex satellite corpora, file-disjoint sibling batches — the program's first air-move milestone.** **Oro** (`tools/frame-data/corpus-oro-ex.yaml`, 9 entries: 7 PASS / 2 XFAIL): Oniyama (EX) S/A oracle-exact (R/adv masked, oracle Recovery negative, same ERRATA mask family as the base corpus). Sun Disk Palm (EX Low) WHIFF fully oracle-exact; BLOCK/HIT R+adv UNCLASSIFIED-omitted (measured R=2/adv=+21-22 vs oracle 27/+1, no Phase-6 bucket fits since adv diverges too). **Sun Disk Palm (EX High) NOT AUTHORED** — 9 empirical motion candidates plus a `cmd_data.c` source dive could not identify its input recipe; documented as an open gap (same class as the existing Q Dashing-Head-Attack precedent), not guessed. Jinchu Nobori (EX) BLOCK+HIT ship **XFAIL citing ENGINE-8 CLOSED** (same dedicated-dispatch-function sibling mechanism as the base move, independently re-traced not assumed). **Air Jinchu Nobori (EX) — FIRST AIR-MOVE ENTRY AUTHORED IN THIS PROGRAM**, via the `press UP; wait 3; motion ...` air-input idiom (Makoto Tsurugi EX precedent, re-verified not inherited on faith: S=8/A=7 exact, distinct from both the grounded EX's S=31 and the non-EX air move's S=8/A=6); BLOCK/HIT confirmed structurally UNREACHABLE (reactive dummy evades at all 8 distance/guard combinations tried, same SDB-class disposition); WHIFF's R (44 vs oracle 30) left unasserted, no cross-check leg available, flagged for follow-up. **Urien** (`tools/frame-data/corpus-urien-ex.yaml`, 9/9 PASS): Metallic Sphere (EX) and Headbutt (EX) fully oracle-exact (Headbutt's WHIFF R=12 vs oracle 15 ships as a plain PASS citing §13.17 RE-ANCHOR-1 by analogy, identical -3 magnitude to the base three strengths); Violence Knee Drop (EX) and Chariot rush (EX) both convert A by §13.13 (a1)-by-analogy, both **quarantine R** (program **QUARANTINE #7** VKD: BLOCK+HIT R=29 vs oracle 7, +22 surplus both legs identical; **QUARANTINE #8** Chariot: BLOCK R=28/HIT R=19 vs oracle 39, -11/-20 deficit, non-identical between legs, A already off too). Ordering-artifact caught and fixed (Chariot's dash-momentum residue corrupting the next entry's teleport, same §12.1 row-2 shape as prior waves) — Headbutt's WHIFF leg moved to run FIRST in the shipped corpus. **Alex** (`tools/frame-data/corpus-alex-ex.yaml`, 4/4 PASS): Flash Chop EX fully oracle-exact on every leg. Slash Elbow EX ships WHIFF-only — BLOCK/HIT confirmed structurally UNREACHABLE (a genuine button-family switch, EX is punch-based vs the base kick variants, cross-checked at an identical 168px gap against the connecting base move); its own A/R (measured 4/17 vs oracle non-numeric/23) are UNCLASSIFIED-omitted, no Phase-6 bucket fits a WHIFF-leg divergence with self-consistent S+A+R=T and no cut event. **Gates:** pre-integration full-suite `run-suite.sh --check-golden` showed the ONLY drift as `oro-ex`/`urien-ex`/`alex-ex`'s missing goldens (28/28 existing corpora untouched, zero drift, 1,132 rows reproduced, tee'd to `<sp>/wave4-pre.log`); scoped `--update-golden oro-ex urien-ex alex-ex`; final full-suite check **31/31 GREEN, zero drift, exit 0** (tee'd to `<sp>/wave4-post.log`). **Arithmetic: 1,132 + 22 = 1,154 rows (1,088 + 20 = 1,108 PASS / 44 + 2 = 46 XFAIL)**, independently re-summed off the 31-corpus golden run's own per-corpus totals (1,154 total / 1,108 PASS / 46 XFAIL, matches exactly). **PROGRAM QUARANTINE LEDGER REBUILT (systemic-trigger check, authoring-policy.md's "≥3 same-shaped quarantines ⇒ STOP"), this integration pass, across every wave report (ryu-pilot, ken/chunli/akuma, dudley/yun/ibuki, yang/makoto/elena, oro/urien/alex) plus the Ken SA3 seed:** re-grouping the full ledger HONESTLY by shape (leg + field + direction + magnitude class + box/cut context) — not just per-batch as prior waves checked it — surfaces a shape that DOES cross the ≥3 threshold once wave 4's members are added: **unexplained Recovery(R)-field divergence on a contact leg (BLOCK and/or HIT), no established Phase-6 bucket, spanning both surplus and deficit sign, both single-run and multi-run, both EX and SUPER class.** Members: #1 Ken SA3 BLOCK R+10 (SUPER, surplus, multi-run box_runs=12, seed); #2 Ryu EX Tatsumaki BLOCK R+3/HIT R+5 (surplus, multi-run box_runs=4/2/6); #3 Dudley Jet Uppercut (EX) BLOCK R+1 (surplus, single-run); #4 Dudley Machine Gun Blow (EX) BLOCK R-3/HIT R-15 (deficit, single-run); #5 Makoto Hayate EX BLOCK+HIT R-4 (deficit, single-run, A also off); #6 Makoto Tsurugi EX BLOCK adv+4 / underlying R+1 vs its own WHIFF leg (weaker match, field is adv not R directly); **NEW** Urien Violence Knee Drop (EX) BLOCK+HIT R+22 (surplus, single-run, identical both legs); **NEW** Urien Chariot rush (EX) BLOCK R-11/HIT R-20 (deficit, single-run, A already off too); **NEW** Oro Sun Disk Palm (EX Low) BLOCK+HIT R-25 (deficit, single-run, proj=1, adv also diverges) — **this is the member that tips the deficit sub-shape (#4/#5 + this) to 3, and combined with Ibuki Kazekiri (EX) HIT R-4 (deficit, single-leg-only, single-run, already flagged UNCLASSIFIED in wave 2 but never program-numbered) the deficit sub-shape alone reaches 4-5 members; the surplus sub-shape (#1/#2 + Urien VKD) independently reaches 3.** Named as a systemic investigation candidate per this task's directive (NOT started this session): **EX-CONTACT-R** — the recurring "displayed Recovery diverges from oracle on a contact leg with no citable mechanism" shape that keeps reappearing across unrelated characters/move-families/engine dispatch paths, now the program's dominant unresolved-divergence class. Flagged to the tracker and commit message for a future dedicated investigation pass; authoring for this wave proceeds unblocked (per-move quarantine, not a STOP, since the directive is to name it, not chase it now). See `<sp>/exsuper/oro-ex-report.md`, `<sp>/exsuper/urien-ex-report.md`, and `<sp>/exsuper/alex-ex-report.md` for the full per-move tables. **EX-CONTACT-R TRIGGER RESOLVED (this commit, 2026-07-12) — `<sp>/exsuper/excontact-r-findings.md`.** Read-only decomposition (zero edits/builds/runs during the investigation itself) resolved the ≥3-member "unexplained contact-leg Recovery divergence" umbrella into 4 real mechanisms, not one systemic defect — **no novel engine mechanism spans multiple members; the arcade oracle is vindicated in every member that decomposes.** **M1 — instrument (FD_METER_LEN=72 saturation / legacy window misplacement), oracle exact both members:** Ken SA3 BLOCK (+10 legacy; true non-frozen recovery span = 27 = oracle exactly, now CONFIRMED on the current binary's real box_a/busyr/box_runs fields via an isolated re-run — 3-rep-plus-live-re-run vindication; SA3 has no shipped corpus entry, display-convention question deferred to the supers program, do NOT xfail-assert 37) and Dudley MGB (EX) BLOCK (−3; busyr=28=oracle exactly, displayed R clipped by the 72-tick cap). **M2 — engine truth, outcome-dependent chart/box content, arcade R whiff-canonical (§12.2.2/§13.10 Class 1, both signs), 7 members converted:** Dudley Jet Uppercut (EX) BLOCK R:35, Dudley MGB (EX) HIT R:13, Ibuki Kazekiri (EX) HIT R:29, Makoto Hayate EX BLOCK/HIT A:3+R:20, Urien VKD (EX) BLOCK/HIT R:29 (own-trace-confirmed via isolated re-run, byte-identical branch both legs), Urien Chariot (EX) BLOCK R:28/HIT R:19 (own-trace-confirmed via isolated re-run, outcome-split branch entry), Ryu EX Tatsumaki BLOCK R:14/HIT R:16 (own-trace-confirmed via isolated re-run, box_runs 6→4→2, R=busyr exactly — NEW §12.2.4 "contact-leg box-run loss, multi-hit" row, the active-side mirror of the recovery-content-differential family). **M3 — new registry row, proj-split late-despawn collapse:** Oro Sun Disk Palm (EX Low) adv:21/22 converts (F10 travel arithmetic, arcade 1 + travel 20 exactly); R stays omitted (structurally lives on the exact WHIFF leg) — NEW §12.2.4 "proj-split late-despawn contact R/adv collapse" row. **M4 — honest residue, stays quarantined:** Makoto Tsurugi EX BLOCK adv — REMOVED from the EX-CONTACT-R member list (oracle publishes no R for the Tsurugi family; this member never belonged to the shape). **Isolated re-runs (3, all FDH_SKIP_BUILD=1, current binary, rundirs preserved under `<sp>/excr/`):** Urien VKD+Chariot combined (1 corpus, 4 entries) reproduced the shipped values byte-for-byte and recorded each move's own branch-chain signature; Ryu EX Tatsumaki (3 entries) reproduced box_runs=6/4/2 and R=busyr exactly on all three legs; Ken SA3 (1 entry) reproduced R=37/adv=−11 and, via the real box_a/busyr/box_runs fields (not the legacy `hatt` proxy), confirmed the 27-tick non-frozen post-active recovery span (35 raw ticks post-last-active, 8 frozen blockstun ticks, 27 non-frozen) = oracle Recovery exactly. Every pre-registered hypothesis in the findings file matched the live re-run evidence — no member was force-converted against a mismatch. **Registry:** two new `docs/frame-data-synthesis.md` §12.2.4 rows (M3 proj-split late-despawn; M2 contact-leg box-run loss) plus an M1 FD_METER_LEN contact-truncation note (answering Elena Lynx Tail (EX)'s wave-3 named follow-up (b): T=72 alone is not truncation, truncation occurs iff the R window is still open at raw index 71). **Quarantine ledger after this disposition:** program quarantines #3/#4 (Dudley), #7/#8 (Urien VKD/Chariot), and the Kazekiri/Hayate UNCLASSIFIED items are DISCHARGED (converted); #2 (Ryu EX Tatsumaki) DISCHARGED; #1 (Ken SA3) EXPLAINED-INSTRUMENT-SIDE, not discharged (no corpus entry to flip; display convention deferred to the supers program); #6 (Makoto Tsurugi EX adv) REMAINS quarantined, M4 honest residue, removed from the EX-CONTACT-R list specifically since no oracle R exists for its family. **Wave-5 guidance (Necro/Sean/Q/Remy/Hugo/Twelve EX), recorded here for the wave-5 agents:** (1) saturation check first — if FINAL shows `raw_len=72` and S+last-active+R reach index 71, check `busyr` against oracle before quarantining (watch: Necro Spinning Punch (EX), Q HSB (EX), Hugo Monster Lariat (EX)); (2) recognize-inheritance check — read the base move's own §12.2.2/§12.2.3/§12.2.4/§13.10 membership before classifying the EX leg, but test don't inherit blindly (Kazekiri EX did NOT inherit its base's item-18(c) shape; known wave-5 base families: Sean Ryuubi (EX)/base is a converted F3 A-overcount member, Sean Tornado Kick (EX)/base tornado-hk-block is the multi-hit exemplar, Q DLA (EX)/base DLA RH is the §13.10 Class-1 KD member, Remy RRF (EX)/base rrf A+1 F3 member, Remy CBK (EX)/base cbk same-tick interior-transition credit-banking row); (3) KD/hit-branch check — HIT-only R deficit with BLOCK oracle-exact and kd=1 (or shorter raw_len): dump the recovery chain from the rundir's own trace.log the same session and convert under §13.10/§12.2.2 with the recorded signature instead of quarantining (no build, no extra run, ~10 minutes offline, per this disposition's own 1.3/1.5/1.5-style method); (4) projectile legs — Remy LOV EX High/Low ×4 is the wave-5 hotspot most likely to reproduce M3 (check `natend`/`first_active_raw` on contact before quarantining; assert R on WHIFF; check measured adv = arcade + travel before anything else); (5) preserve the rundir for any leg you intend to quarantine — copy trace.log out before exit (the Urien lesson, now resolved twice); (6) quarantine default stands for anything left: omit+log with the raw FINAL line quoted (box_a, busyr, box_runs, raw_len — the four diagnostics that carried this entire investigation). **Gates (derived-prediction corrected by measurement, per this codebase's own house rule of verifying over assuming):** the pre-registered expectation was that adding these 15 new field-assertions across 13 rows would show as golden drift. **Measurement corrected this:** `golden.py`'s stored verdict is the categorical PASS/XFAIL/FAIL/etc. enum (this corpus format's `xfail:` key drives XFAIL, not a literal-vs-oracle mismatch by itself), and every row touched by this disposition was ALREADY verdict=PASS before this session (fields simply omitted, not failing) and remains verdict=PASS after (the newly-asserted literals all match the measured trace exactly — confirmed independently by the `dudley-ex`/`ibuki-ex`/`makoto-ex`/`oro-ex`/`ryu-ex`/`urien-ex` corpora all reporting 0 RED in both gate runs). Golden.py's own stored measured-value columns (S/A/R/adv/kd) are also unaffected, since `expect:` assertions do not change what the engine measures. Net effect: **zero golden drift in both the pre gate and the post gate** — this is the CORRECT result, not a miss, and independently confirms every literal added in this disposition (R:35, R:13, R:29 ×2, R:28, R:19, R:14, R:16, A:3+R:20 ×2, adv:21, adv:22) exactly matches this binary's measured output. Pre-disposition full `run-suite.sh --check-golden`: **31/31 GREEN, zero drift** (tee'd `<sp>/excr-pre.log`). Scoped `--update-golden` on the 6 affected corpora: **0 new/changed of 6** (golden files byte-identical, confirming the above). Final full `run-suite.sh --check-golden`: **31/31 GREEN, zero drift, exit 0** (tee'd `<sp>/excr-post.log`). **Arithmetic: 1,154 rows / 1,108 PASS / 46 XFAIL — UNCHANGED from wave 4's closing tally** (row-level verdict counts don't move; this disposition converts field-level coverage — 15 previously-omitted fields across 13 rows now explicitly asserted and green — inside rows that were already counted PASS). No STOP triggered; no arcade capture was needed for any member. **Wave 5 (this commit, 2026-07-12): necro-ex + sean-ex + q-ex satellite corpora, file-disjoint sibling batches.** **Necro** (`tools/frame-data/corpus-necro-ex.yaml`, 8 entries: 5 PASS / 3 XFAIL): Spinning Punch (EX) BLOCK oracle-exact, HIT R converts under §13.10 Class 1 (own-trace recovery-chain recount, R:21, arcade 23 preserved) — wave-5 guidance item 1's FD_METER_LEN=72 saturation watch explicitly ruled OUT via a far-connect probe at raw_len=72 matching busyr=21, confirming genuine engine truth not instrument clipping. Raging Cobra (EX) fully oracle-exact, no divergence. Flying Viper (EX) WHIFF oracle-exact; BLOCK/HIT/crouch-probe ship 3 XFAIL, a NEW sum-preserving A/R boundary shift (S=24/A=10/R=9 vs oracle S=24/A=5/R=14, adv=+2 both, T=43 both) — checked per guidance item 2 against both the base Jab's own item-4 R-surplus and FINDING-3 stance-conditional shapes (inherits neither) and against this table's existing "Sum-preserving A/R boundary shift" row (`remy-cbk-lk-whiff`, magnitude-1): this sighting's magnitude-5 shift is a different magnitude class with no own-trace citation, so it does NOT convert or merge into that row — logged as its own new one-off (`docs/frame-data-synthesis.md` §12.2.4). **Sean** (`tools/frame-data/corpus-sean-ex.yaml`, 7 entries: 5 PASS / 2 XFAIL): Ryuubi Kyaku (EX) BLOCK+HIT S/R/adv oracle-exact, A converts under the established §12.2.3/§13.13 F3 family (independently re-verified on this EX leg's own trace, not inherited on faith). Tornado Kick (EX) BLOCK+HIT fully oracle-exact. Tackle (EX) NOT AUTHORED — base motion remains unidentified after a further bounded 5-candidate P-button probe this session (combined with the mains session's 14 K-button + Zenten P-button attempts, every expressible motion primitive on both button families now tried); no entries shipped, no guess made. Dragon Smash (EX) WHIFF clean; BLOCK+HIT ship 2 XFAIL, a NEW contact-only A-overcount (measured A=13 vs arcade 12, `box_a=13 == engine_a=13`) explicitly tested against and REFUSED §12.2.3 F3 membership (F3's bar requires `box_a < engine_a`; here box and engine agree, a different shape) — logged as its own new one-off (`docs/frame-data-synthesis.md` §12.2.4), flagged for follow-up classification. **Q** (`tools/frame-data/corpus-q-ex.yaml`, 8 entries, ALL PASS, zero XFAIL): Dashing Head Attack (EX) WHIFF+BLOCK oracle-exact, HIT R converts under §13.10 Class 1 (R:18, arcade 36 preserved) — the base non-EX DHA Fierce does NOT show this split, the expected "test don't inherit" result per guidance item 2. Dashing Leg Attack (EX) WHIFF-UNREACHABLE (own move-reach exceeds the harness's centered ceiling); BLOCK R converts citing the §12.2.2 R-surplus corollary by direct analogy to the already-solved Dudley Jet Uppercut (EX) precedent; HIT R converts under §13.10 Class 1, exactly the wave-5 guidance's own pre-registered prediction for this move. High Speed Barrage (EX) hits the M1 FD_METER_LEN=72 saturation the wave-5 guidance explicitly flagged for this move on all three legs; `busyr=36` = oracle Recovery exactly on every leg (the MGB-EX precedent shape) — R omitted per the established M1 disposition on all three, S/A/adv oracle-exact throughout. Guidance items 1-6 all applied and confirmed working this wave (item 1's saturation rule-out on Necro Spinning Punch and Q HSB both landed exactly as predicted; item 2's recognize-inheritance test correctly separated true inheritance (Sean Ryuubi F3, Q DLA §13.10 Class 1) from non-inheritance (Sean Tornado Kick, Necro Flying Viper's stance-conditionality) in every case tested). **Gates:** pre-integration full-suite `run-suite.sh --check-golden` showed the ONLY drift as `necro-ex`/`sean-ex`/`q-ex`'s missing goldens (31/31 existing corpora untouched, zero drift, 1,154 rows reproduced, entry counts matching all three per-move reports exactly, tee'd to `<sp>/wave5-pre.log`); scoped `--update-golden necro-ex sean-ex q-ex`; final full-suite check **34/34 GREEN, zero drift, exit 0** (tee'd to `<sp>/wave5-post.log`, independently re-summed off the 34-corpus golden run's own per-corpus totals: 1,177 total / 1,126 PASS / 51 XFAIL, matches exactly). **Arithmetic: 1,154 + 23 = 1,177 rows (1,108 + 18 = 1,126 PASS / 46 + 5 = 51 XFAIL)**. **Ledger update (this commit):** two new §12.2.4 one-off rows added to `docs/frame-data-synthesis.md` — necro's Flying Viper (EX) sum-preserving A/R shift (magnitude-5, explicitly grouped as a SEPARATE sighting from the registered magnitude-1 `remy-cbk-lk-whiff` row, not a member) and sean's Dragon Smash (EX) contact-only A-overcount (`box_a==engine_a`, explicitly refused §12.2.3 F3 membership since F3 requires `box_a < engine_a`). **≥3 same-shaped-quarantine trigger re-checked honestly against the full program ledger:** both new sightings are first-of-their-shape — neither's magnitude/mechanism signature matches any existing bucket, so neither joins an existing shape to reach 3+, and neither alone forms a 3-member bucket. Trigger NOT tripped. See `<sp>/exsuper/necro-ex-report.md`, `<sp>/exsuper/sean-ex-report.md`, and `<sp>/exsuper/q-ex-report.md` for the full per-move tables. **Wave 6 (this commit, 2026-07-12): remy-ex + hugo-ex + twelve-ex satellite corpora, file-disjoint sibling batches — THE FINAL EX WAVE; EX UNIVERSE COMPLETE.** **Remy** (`tools/frame-data/corpus-remy-ex.yaml`, 13 shipped entries + 1 negative control = 14 rows: 10 PASS / 4 XFAIL): Light of Virtue (EX High/Low) — the oracle's own duplicate-Name-row hotspot resolved empirically (High→index 0/R=24, Low→index 1/R=28, the only reachable recipe per strength); BLOCK+HIT+crouch-probe oracle-exact both strengths (BLOCK adv = F10 travel literal, established convention), WHIFF-UNREACHABLE on Low (genuine reach extension, no entry shipped). **NEW HARNESS LIMITATION found and fixed:** any LOV-family attempt (either strength, any outcome) corrupts the immediately-following LOV-family cast's MOVE_START (byte-identical degenerate S=0 reading, reproduced 3x, immune to larger inter_entry_wait) — fixed by alternating every LOV entry with an RRF-EX/CBK-EX spacer, confirmed clean 14/14 across probes 4-8; documented as a load-bearing ordering constraint in the shipped file's own header. Rising Rage Flash (EX) and Cold Blue Kick (EX) both ship 2 XFAIL each (BLOCK+HIT) — see the new/updated `docs/frame-data-synthesis.md` §12.2.4 rows below; S/adv assert, A/R xfail on both. **Hugo** (`tools/frame-data/corpus-hugo-ex.yaml`, 6 entries: 5 PASS / 1 XFAIL) — verified BEFORE authoring that Hugo's EX universe is non-empty (2 rows, unlike Akuma's null case) and has no command-grab EX variant. Neither Giant Palm Bomber nor Monster Lariat is a charge special in this engine's implementation despite real-arcade documentation calling both CHARGE moves (batch-probed motion macros found `qcb+P`/`qcf+K` respectively, confirmed via clean S-match, not memory of game notation) — a genuine engine-vs-documentation divergence, caught by methodology. Giant Palm Bomber (EX) fully oracle-exact on WHIFF, BLOCK/HIT R+1 surplus asserted as measured engine truth citing the established §12.2.2 R-surplus corollary (same +1 magnitude as Dudley Jet Uppercut EX / Q DLA EX). Monster Lariat (EX) WHIFF/BLOCK oracle-exact (R is a range field, floor/ceiling both inside published bounds); HIT ships **1 XFAIL** — A measures 2 vs true arcade/BLOCK value 6, juggle check FAILS, per-tick trace cited (`<sp>/ex-hugo/run6/trace.log` lines 208-222 vs 272-279) showing a genuine same-tick cgix/jatix divergence coincident with contact — consistent with but NOT confirmed as the registered Phase-6 "contact-A undercount, shape (a1)/(a2)" family (no lever-F toggle test, no build, out of this no-build session's scope); kept the true arcade A=6 as `expect`, flagged for follow-up classification (**named follow-up: hugo lever-F check**). **Twelve** (`tools/frame-data/corpus-twelve-ex.yaml`, 8 entries, ALL PASS, zero XFAIL, zero quarantines): N.D.L. (EX) proj-split, BLOCK+HIT oracle-exact at the anchored dist:250 point (distance-dependent connect, same `S = max(spawn, athok)` formula lever-J already established), WHIFF-UNREACHABLE at every distance tried; a predecessor-residue ordering trap (any adjacent second N.D.L.-EX cast corrupts the following cast's athok-arm, independent of leg/outcome) fixed by shipping HIT first and moving BLOCK past the entire A.X.E.-EX section as a buffer. A.X.E. (EX) WHIFF/BLOCK fully oracle-exact; HIT R converts to a plain `R:12` assert citing the SAME uniform R=12 pattern `corpus-twelve.yaml`'s own base A.X.E. already established for all three ground strengths (§13.10 Class 1, genuine inheritance confirmed via this entry's own trace, not assumed). **Air A.X.E. (EX) — FIRST FULLY-REACHABLE AIR-EX MOVE IN THE PROGRAM:** the air idiom (`press UP; wait 3; motion ...`, the Makoto Tsurugi EX / Oro Air Jinchu Nobori precedent) landed cleanly on the first attempt (S=6/A=10, distinct from grounded A.X.E. EX's own S=8/A=11, confirming a genuine air variant not a misread); ALL THREE legs (WHIFF/BLOCK/HIT) fully reachable and oracle-exact on both numeric oracle fields — unlike Oro's Air Jinchu Nobori (EX) (the program's first air-move entry, wave 4), whose BLOCK/HIT were structurally UNREACHABLE, this move's BLOCK+HIT are BOTH reachable, zero divergence. D.R.A. (EX) NOT AUTHORED — the base (non-EX) family's own 17-attempt MOTION NOT FOUND result reconfirmed with EX chords (three untested motion shapes, all resolved to plain Roundhouse via strongest-button dispatch, same failure mode as the mains session); no entry shipped, no guess made. **Ledger update (this commit):** one new `docs/frame-data-synthesis.md` §12.2.4 row added — Remy RRF-EX's non-sum-preserving joint A/R shift (adv-exact, T not conserved, distinct from both the magnitude-1 and magnitude-5 sum-preserving members already registered) — plus a same-row note that Remy CBK-EX, despite surface resemblance ("A and R move together, adv exact"), is NOT sum-preserving either (T:19→20, Δ+1, opposite sign from RRF's Δ-5) and was kept fully xfail purely as a precedent-following continuation of the base CBK entry, not a claim of shared mechanism. **≥3 same-shaped-quarantine trigger re-checked honestly, precisely distinguishing shape classes:** Remy's two joint-shift sightings (RRF-EX, CBK-EX) are NOT the sum-preserving shape necro's Flying Viper (EX) and yun's Zesshou Hohou (EX) already occupy — both remy entries fail the T-conservation test that defines that row (RRF: T 46→41; CBK: T 19→20; neither constant), and the two remy sightings aren't even the same shape as each other (opposite-signed Δ). Hugo's Monster Lariat (EX) A-undercount is a separate, third shape (contact-A-only, no R/adv involvement) directionally consistent with but not confirmed as the pre-existing Ibuki-Tsumuji-anchored (a1)/(a2) family. No group reaches the ≥3 threshold this wave. Trigger NOT tripped; see `docs/frame-data-synthesis.md` §12.2.4 for the full reasoning. **Gates:** pre-integration full-suite `run-suite.sh --check-golden` showed the ONLY drift as `remy-ex`/`hugo-ex`/`twelve-ex`'s missing goldens (34/34 existing corpora untouched, zero drift, 1,177 rows reproduced, tee'd to `<sp>/wave6-pre.log`); scoped `--update-golden remy-ex hugo-ex twelve-ex`; final full-suite check **37/37 GREEN, zero drift, exit 0** (tee'd to `<sp>/wave6-post.log`, independently re-summed off the 37-corpus golden run's own per-corpus totals: 1,205 total / 1,149 PASS / 56 XFAIL, matches exactly). **Arithmetic: 1,177 + 28 = 1,205 rows (1,126 + 23 = 1,149 PASS / 51 + 5 = 56 XFAIL)**. **Supers wave S2 (this commit, 2026-07-13): akuma-sa{1,2,3} + dudley-sa{1,2,3} + yun-sa{1,2,3} satellite corpora, file-disjoint sibling batches, 24 new rows across 9 files — the program's FIRST INSTALL-type super authored.** (Tracker note: this row's own narration last closed at wave 6/1,205 rows; the intervening supers-pilot [ryu-sa1/2/3] and supers wave S1 [ken-sa1/2/3, chunli-sa1/2/3] sessions landed and are fully documented in `docs/frame-data-synthesis.md`'s own dated M1/trigger notes, but were never appended to this specific tracker cell — a pre-existing narration gap this wave does not attempt to backfill, out of this session's own file-disjoint scope; this wave's own baseline is measured directly off the pre-integration golden run, 1,227 rows / 46 corpora, not off wave 6's stale 1,205 figure.) **Akuma slot-correction:** the task brief's premise that Akuma's SA3 is "Shun Goku Satsu (a GRAB-type super)" does not match `step0-universe.tsv`/`akuma.json` — Shun Goku Satsu's own oracle row is `sa_slot=EXTRA` (a secret always-available input, not tied to a select-screen slot); the TSV's actual `sa_slot=3` row is Messatsu Gou Rasen (Ground), which `corpus-akuma-sa3.yaml` correctly authors instead. Kongou Kokuretsu Zan (also EXTRA) got one real, bounded, inconclusive motion attempt (3 candidate chords, all land the SAME distinct-but-unidentified S=15/A=6/R=50/adv=-26 result — reproducibly real, matching neither oracle `Startup=15` row exactly) and was NOT shipped (wrong values never ship as if expected); no `corpus-akuma-extra.yaml` created. **Install-super first:** Yun SA3 Genei Jin is the program's first INSTALL-class super corpus entry — empirically confirmed context-independent zero-active activation (S=9/A=0/R=0 identical across close/far/no-dummy contexts, all three probed), shipped as a single WHIFF-outcome entry asserting only S per this batch's own INSTALL DISPOSITION convention (`sa_gauge:3` MAXIMUM, never INFINITY). **Akuma** (2/3/3 entries, SA1 2 PASS, SA2 3 XFAIL, SA3 2 PASS+1 XFAIL): SA1 Messatsu Gou Hadou ships S/adv only (A omitted, a 29-unit gap categorically unlike any Phase-6 A-bucket, pre-registered `MULTIHIT_CITED` in step0-universe.tsv, HYPOTHESIS ONLY that the oracle Hit=42 is a cumulative multi-orb total this harness's single-window `proj_a` can't capture). SA2 Messatsu Gou Shoryuu: uniform -1 A-undercount reproduced whiff-inclusively (23 vs oracle 24), BLOCK adv oracle-exact (-25==-25) — see the SA-WHIFF-A ledger rebuild below, correcting the authoring report's own informal "matches Chun-Li SA1" framing. SA3 Messatsu Gou Rasen (Ground): a real motion-ID correction (the naive `2qcb HK` lands the plain special, not the super; the actual recipe is `2qcf HK`, double-QCF **forward**), S/A oracle-exact both WHIFF/HIT, BLOCK adv off by 1 (-75 vs oracle -76) — the FIRST adv divergence among the **supers** corpora specifically (the program at large already has EX-class adv anomalies, `remy-crroundhouse-block`/`remy-lov`, §12.2.4 Block-adv anomaly row — the authoring report's own "first ever" framing is corrected to supers-scope here). **Dudley** (3/3/3, SA1 3 PASS, SA2 1 PASS+2 XFAIL, SA3 1 PASS+2 XFAIL): SA1 Rocket Uppercut ships S-only, ESCALATED whiff-inclusive A undercount (39 vs oracle 40, no lever-F test possible, non-numeric adv field) — joins SA-WHIFF-A below. SA2 Rolling Thunder/SA3 Corkscrew Blow both ship clean WHIFF (S/A/R all oracle-exact) with a contact-only -1 A undercount on BLOCK/HIT (bucket-1 CANDIDATE, directionally consistent with this character's own established Machine Gun Blow -2 pattern, not yet lever-F-confirmed) — NOT part of SA-WHIFF-A (contact-only, not whiff-inclusive). **Yun** (3/3/1, SA1 3 PASS, SA2 3 PASS, SA3 1 PASS): SA1 You Hou ships S(+BLOCK adv where numeric) only — A diverges to THREE different values across legs (15/9/9 vs oracle 16, WHIFF≠BLOCK≠HIT), explicitly ruled out of SA-WHIFF-A (non-uniform magnitude, fails both shapes' own defining test) and logged as its own standalone one-off; R also carries a two-shape split (WHIFF clean M1, BLOCK/HIT an unrelated busyr-disagreeing surplus matching the existing Chun-Li SA3 BLOCK/HIT precedent — 2 sightings total for that shape, below threshold). SA2 Sourai Rengeki ships a genuinely clean super WHIFF (S/A/R all oracle-exact, the wave's strongest identity confirmation, found via a 5-button sweep after the kick-ending sub-motion got intercepted by Nishou Kyaku) with a NEW BLOCK/HIT contact-only shape where the leg's own `box_a` disagrees with its own `engine_a` (14 vs 17) — refused §12.2.4's box-backed-overcount row membership (that row requires box_a==engine_a; here they disagree) and refused Phase-6 bucket 8 (established members agree on the diverging value); logged as its own new one-off. SA3 Genei Jin: the install-super first, above. **SA-WHIFF-A ledger rebuild (trigger priority, this session), correcting rather than accepting the per-character reports' own informal groupings:** wave S1 split the whiff-inclusive uniform-(-1) A-undercount sightings into shape-A (adv-exact: Ken SA1) and shape-B (adv co-shifts: Chun-Li SA1) by an adv-coupling test. Re-run against this wave's three whiff-inclusive sightings, independently, not on the authoring reports' own prose: Dudley SA1 has no numeric adv field at all (non-numeric oracle), so shape-B's coupling test (which requires an OBSERVED co-shift) cannot apply — it places in shape-A by the absence of any observed coupling. Akuma SA2's own report claims a shape-B match ("the Chun-Li SA1 Kikoshou precedent's own shape exactly") but its own table shows BLOCK adv=-25 measured against oracle -25 — EXACT, not a co-shift — so it is CORRECTED to shape-A here, contradicting the authoring report's own framing. **Shape-A (adv-exact) now has 3 members — Ken SA1 Shoryureppa, Dudley SA1 Rocket Uppercut, Akuma SA2 Messatsu Gou Shoryuu — crossing the ≥3 same-shaped-quarantine threshold. TRIGGER FIRES, named SA-WHIFF-A** ("whiff-inclusive uniform A-undercount, adv-isolated"), recorded **NOT STARTED** (no lever-F toggle test run this no-build wave), bundled with the existing wave S1 follow-up item into one build-gated investigation covering all three moves. Shape-B (adv co-shifts) stays at 1 member (Chun-Li SA1 only), below threshold. Yun SA1's own A sighting joins NEITHER shape (non-uniform per-leg magnitude) and stays its own standalone UNCLASSIFIED one-off. Full reasoning and the two new one-off registry entries (Akuma SA3 adv, Yun SA2 box_a/engine_a) in `docs/frame-data-synthesis.md` §12.2.4. **M1 register:** grows from 14 legs/7 moves (through wave S1) to **24 legs/11 moves** — this wave's own 10 new legs: Akuma SA2 (WHIFF/BLOCK/HIT, `busyr`=37=oracle), Akuma SA3 (WHIFF/BLOCK/HIT, `busyr`=51=oracle), Dudley SA1 (WHIFF/BLOCK/HIT, `busyr`=31=oracle), Yun SA1 WHIFF-only (`busyr`=36=oracle; its own BLOCK/HIT legs are the separate non-M1 R-surplus shape above). **Gates:** pre-integration full-suite `run-suite.sh --check-golden` showed the ONLY drift as the 9 new corpora's missing goldens (46/46 existing corpora untouched, zero drift, 1,227 rows reproduced, tee'd to `<sp>/waveS2-pre.log`); scoped `--update-golden` on the 9 new corpora (9 new golden files born, tee'd to `<sp>/waveS2-update.log`); final full-suite check **55/55 GREEN, zero drift, exit 0** (tee'd to `<sp>/waveS2-post.log`, independently re-summed off the 55-corpus golden run's own per-corpus totals: 1,251 total / 1,183 PASS / 68 XFAIL, matches exactly). **Arithmetic: 1,227 + 24 = 1,251 rows (1,167 + 16 = 1,183 PASS / 60 + 8 = 68 XFAIL)**. See `<sp>/exsuper/akuma-sa-report.md`, `<sp>/exsuper/dudley-sa-report.md`, and `<sp>/exsuper/yun-sa-report.md` for the full per-move tables.
| ENGINE-JINCHU | Oro Jinchu Nobori bounce-recovery contact-leg re-anchor: TRACK-A Session 6 hardware capture (`<sp>/zero/arcade-track-a/session-report.md`) falsifies ENGINE-8's "NO-CODE-FIX-EXISTS / stays xfail at oracle R=19" closure — CONTACT-2's own preserved FINAL traces show a real busy `771->768` edge exists and latches at `busyr=33` on both jinchu HIT legs (base-LK and EX), and never latches (`busyr=-1`) on both BLOCK legs. Arcade truth: base-LK 33/33 (symmetric HIT/BLOCK); EX 34-HIT/52-BLOCK (asymmetric). New lever U (`fd_jinchu_bounce_recovery_r`) relaxes lever R's G7 (`recovery_pf > 0`) to admit the two `recovery_pf==0` gate-firers — census-verified to be exactly the two jinchu HIT legs among all lever-R gate-firers, zero surplus. | `/plan` (opus-substitute, Fable plan-access quota exhausted 2026-07-13) + `/implement` | S | **DONE 2026-07-14 — SHIPPED (lever U `fd_jinchu_bounce_recovery_r`, `frame_data_overlay.c`, declared next to lever T; single-term G7 disjunct: `recovery_pf > 0 \|\| (fd_jinchu_bounce_recovery_r && recovery_pf == 0)`).** Gauntlet: G-identity (lever U=0, rebuild, full 94-corpus `--check-golden` against the `f9522a26` baseline goldens/corpus) zero drift, 94/94 GREEN. Gate 1 census (lever U=1, corpus still at baseline): drift on exactly 2 of 94 corpora's goldens, both R `0->33` (`oro-jinchu-lk-hit`, `oro-exjinchu-hit`), zero surplus movers, both BLOCK legs stayed `busyr=-1`/unreachable. `--update-golden` touched exactly `oro.tsv`/`oro-ex.tsv` (2 of 94), `oro-jinchu-lk-hit` verdict XFAIL->PASS; final `--check-golden` zero drift, 94/94 GREEN. **Suite delta: 1,288/61/1,349 -> 1,289 PASS / 60 XFAIL / 1,349 rows** (+1/-1 exactly, zero XPASS/FAIL/SHAPE/NO-DATA before or after). Determinism x2 on `oro`/`oro-ex` + `hugo` (non-member): byte-identical. Lever census: **twelve** consts `=1` (`F` in `charset.c`; `G,H,I,J,M,N,O,R,S,T,U` in `frame_data_overlay.c`), zero `=0`. `git diff --stat src/` exactly `frame_data_overlay.c`, zero `tools/frame-data/*.py` changes; full diff exactly the 8 pre-registered files (`frame_data_overlay.c`, `corpus-oro.yaml`, `corpus-oro-ex.yaml`, `golden/oro.tsv`, `golden/oro-ex.tsv`, `ERRATA.md`, `frame-data-synthesis.md`, this file) — oracle JSON/CAPTURE.md untouched. See `docs/frame-data-synthesis.md`'s §13.21 for the full gauntlet transcript citations (`<sp>/zero/jinchu/step/`). **WANTS FABLE RE-REVIEW (opus-substitute):** this design + its Step-1 diff both ran on an opus-substitute (Fable plan-access quota exhausted 2026-07-13, `<sp>/zero/FABLE-REREVIEW-QUEUE.md`) — re-review scope per the plan's own §11 checklist: (1) the lever-U G7-disjunct shape vs a positive jinchu identifier; (2) the EX-hit meaning-decision (display moves 0->33 while golden/corpus assert arcade 34, stays XFAIL); (3) the block-leg "unmeasurable, honest XFAIL" disposition; (4) the ERRATA verdict choice (`ORACLE-LIKELY-WRONG` vs a new class — see `docs/arcade-frame-data/ERRATA.md` item 9's own inline flag). See `docs/frame-data-synthesis.md`'s new §13.21 (lever U record) for the full rule/discriminator/per-leg-measurability writeup. |
| INSTALL-ADJ | Install-super oracle-comparison adjudication for the two remaining NEEDS-ADJUDICATION rows (`twelve-sa3-activation` S 17→12, `urien-sa3-aegis-activation` R 9→12): are these engine bugs, wrong-oracle-field errata, or genuine terminal divergences, given all 5 install/activation supers share one identical finalize path (`<sp>/zero/install-adj/adjudication.md`)? | direct edit (opus-substitute, Fable exhausted) | S | **SHIPPED 2026-07-14 (DISPOSITION-COMMIT batch).** `urien-sa3-aegis-activation`: verdict (c) ERRATA/WRONG-COMPARISON — engine R=9 is the identical 9-frame post-flash r1=4 settle Yun's own install files as an exact-match S=9; it equals urien.json's own Startup "(9/9/9)" parenthetical, NOT Recovery=12 (a categorically different out-of-scope quantity). Per the Oro SA3 Tengu Stones proj-split precedent (`corpus-oro-sa3.yaml`'s own `expect: {}` OMIT), `corpus-urien-sa3.yaml`'s R assertion is removed entirely (`expect: {}`) — **flips XFAIL→PASS, +1 row, zero risk** (removing an assertion cannot regress any other row). `twelve-sa3-activation`: verdict (b) TERMINAL-DIVERGENCE — X.C.O.P.Y. is a genuine two-sub-phase activation (pat=22 phase F=86-95, then a cgix 64→0 reset + pat→0 at F=96, second phase F=96-102); the engine's r1-idle heuristic cannot subdivide the two phases (r1 stays=4 across the boundary) and faithfully reports the full 17-frame span, while oracle row-43 Startup=12 counts sub-phase 1 only — the identical finalize path matches oracle EXACTLY for 3 single-phase siblings (Yun 9=9, Q 18=18, Yang 15=15), proving the measurement is arcade-faithful; no suite-safe lever reproduces 12 (internal boundary is at offset 10, not 12; cgix-reset-under-r1 is not a general idle signal). Note reworded to DOCUMENTED TERMINAL DIVERGENCE, stays XFAIL, golden unchanged. Neither row's the real arcade actionable-return (~95/~65, session-6/7) adopted — a THIRD quantity, would redefine the install-family oracle-Startup convention and break the 3 exact siblings. **WANTS FABLE RE-REVIEW** (opus-substitute) — see the source doc's own §8 checklist. |
| HARNESS-BLEED | Full-suite footprint sizing for the harness inter-entry state-bleed (D-g class, adv/plan.md §4.3): reconstruct and full-corpus-test the prior agent's dummy-idle-reset patch to determine whether it should ship as a general test-infra fix (`<sp>/zero/harness-bleed/footprint.md`). | direct edit (opus-substitute, Fable exhausted) | S | **DONE 2026-07-14 (DISPOSITION-COMMIT batch) — REJECTED, not shipped.** Reconstructed the global dummy-idle-reset (snapshot/restore the dummy's `cg_ix`/`cg_ctr`/`cg_next_ix` at every corpus-entry label) and ran the full 94-corpus `--check-golden` under it: **net-NEGATIVE** — fixes exactly 1 row (`sean-ryuubi-hk-block`, XFAIL→XPASS at arcade R=15/adv=-3) but regresses 4 arcade-correct rows off arcade (`urien-msphere-hp-block` adv+3→+4, `urien-crroundhouse-block` and `alex-crroundhouse-hit` and `yun-nishou-forward-hit` all connecting moves turned WHIFF), plus 2 cosmetic/no-verdict-impact rows (`alex-twdsmp-block`, `remy-sa1-hit-far`) and 1 genuinely UNKNOWN-needs-capture row (`urien-vkd-lk-hit`'s A/R clause — pinned bled literals A=4/R=32 vs isolated A=5/R=31, no arcade anchor either way). **Footprint: 8 rows / 5 corpora total, confirmed complete** (`--check-golden` reported drift on exactly these 8, no added/removed rows). Global reset NOT adopted (would need 4 regressions to gain 1 fix). `urien-vkd-lk-block` (`corpus-urien.yaml`) and `sean-ryuubi-hk-block` (`corpus-sean.yaml`) xfail notes reclassified to DOCUMENTED HARNESS-BUNDLE-CONTEXT ARTIFACT (golden unchanged, still asserting the arcade value); `urien-vkd-lk-hit`'s A/R clause remains the ONE genuinely open, capture-gated question in this program — untouched this batch, needs an arcade capture of Violence Knee Drop (LK) HIT active/recovery before any re-pin. **WANTS FABLE RE-REVIEW** (opus-substitute). |
| CONTACT-A | Contact-leg A-overcount re-anchor design (grant-route's "OVERLAY RE-ANCHOR" follow-up): can a frz-excluded strict box-count lever ship for the 6 arcade-confirmed contact rows (`<sp>/zero/contactA/design.md`, cross-validated against Session 12 hardware capture)? | direct edit (opus-substitute, Fable exhausted) | S | **DONE 2026-07-14 (DISPOSITION-COMMIT batch) — NEGATIVE, no lever ships (letter V stays unclaimed).** Session 12 hardware capture (`docs/arcade-frame-data/CAPTURE.md`, new Session 12) proved arcade active-frames == ORACLE on 8/9 grant-route contact legs (the overlay overcounts via a +1 hitstop-boundary tick and/or F13 declared-credit banking). A new frz-excluded strict box counter (`box_a_frz`) reproduces oracle exactly on the 6 single-hit rows but a full-universe native census proves it engine-signal-INDISTINGUISHABLE from **113** other currently-PASS contact-A rows across all 19 characters it would regress (structural-twin proof: `sean-exdragonsmash-block` needs `strict`=12, `ken-srk-lp-block`/`sean-dragonsmash-mp-block` need `displayed`=8, byte-identical gate signals) — the CONTACT-2 §4.1 UOH-18-unreachability shape, confirmed on a third independent basis. **Zero golden edits, zero verdict flips.** The 6 rows (`sean-exdragonsmash-block/-hit`, `remy-cbk-lk-block/-hit`, `remy-cbk-ex-block/-hit`) reclassify from UNCLASSIFIED to ARCADE-CONFIRMED METER-OVERCOUNT / TERMINAL-KNOWN-VALUE (`corpus-remy.yaml`, `corpus-remy-ex.yaml`, `corpus-sean-ex.yaml`); `docs/frame-data-synthesis.md` §13.16 gets the CONTACT-A addendum. Decisive follow-up flagged, not blocking: a contact-leg arcade capture of the 113-class (starting with the `sean-dragonsmash-mp` twin) is the only thing that could resurrect a blind lever. **WANTS FABLE RE-REVIEW** (opus-substitute) — the non-gateability claim is load-bearing. **NATIVE-CENSUS RIDER 2026-07-17:** the "113 regression" count was offline-derived at drive time (strict = box_a − E predictor, 2026-07-10 `uoh-fit/step1` baseline). With `box_a_frz` now SHIPPED (B2/B3), a native census over the 94 kept `b3-suite-retry` rundirs gives buckets 265/95/170/6/125 (firers/identical/movers/converts-at-oracle/PASS-FAIL-regress) vs the stale offline 263/93/170/6/113 — the drive-to-zero advanced UOH/oro-jinchu rows AFTER the offline baseline (+2 firers, +12 regressions), so offline 113 is a STRICT SUBSET of native 125 (none dropped). The 6 converts reproduce EXACTLY at oracle natively; design.md §8.1 Gate-1 (6 converts at oracle + >=~100 engine-indistinguishable PASS regressions) is natively satisfied — the negative is CONFIRMED AND STRENGTHENED, not weakened. Still NEGATIVE, no lever ships, zero golden edits; `docs/frame-data-synthesis.md` §13.16 gets the CONTACT-A native-census rider (with the twelve-sa2-hit E-predictor-miss and 6 native==oracle-still-XFAIL observations). WANTS FABLE RE-REVIEW. |
| ADV-REANCHOR | `adv` re-anchor diagnosis for the 6 TRACK-D rows where Recovery is closed but `adv` is off by a fixed amount (D-e/D-f/D-g, `<sp>/zero/adv/plan.md`): engine lever, harness fix, or terminal? | direct edit (opus-substitute, Fable exhausted) | S | **DONE 2026-07-14 (DISPOSITION-COMMIT batch) — NEGATIVE for D-e/D-f, no lever ships (V stays unclaimed); D-g routed to HARNESS-BLEED (above).** `adv = defender_idle - attacker_idle` is arcade-faithful on 600/607 currently-asserting rows. D-e (`yang-senkyuutai-lk-block`/`yang-exsenkyuutai-block`, +6) and D-f (`akuma-sa3-block`/`chunli-sa1-block`, +1) are 4 of the 7 exceptions; every candidate rule broad enough to catch them (`-(R+1)`, flat `-6`, flat `-1`) regresses 86-600 currently-correct rows — the identical CONTACT-A failure shape, proven independently on the adv axis. The busy edge (D-e's own lever R) points the WRONG direction for a fix (R went 38→34, i.e. earlier than `attacker_idle`, while the oracle needs adv MORE negative, i.e. a LATER anchor) — refuted by direction, not just regression count. No census signal isolates any of the 4 rows from the correct population; per-move hardcoding is policy-forbidden. All 4 reclassified to TERMINAL-NON-SURGICAL (`corpus-yang.yaml`, `corpus-yang-ex.yaml`, `corpus-akuma-sa3.yaml`, `corpus-chunli-sa1.yaml`), golden unchanged, deferred pending a new instrumented diagnostic (build task). D-g (`urien-vkd-lk-block`, `sean-ryuubi-hk-block`) confirmed HARNESS-FIX (engine correct, isolated run is arcade-exact) — see the HARNESS-BLEED row above for the full footprint/rejection. **WANTS FABLE RE-REVIEW** (opus-substitute). **SUPERSEDED in part 2026-07-17: lever W (LANDING-CUT, synthesis §13.22) ships the cut-discriminator adv re-anchor for the yang/oro landing family — the "no census signal isolates" claim was falsified by the Fable re-review; the D-f akuma/chunli residual stands. D-f UPDATE 2026-07-18: ENGINE-EXONERATED by the FD_IDLE_PROBE per-tick idle ledger (H-A/H-B refuted with tick-cited evidence; blockstun table-exact, anchors faithful); the +1 is external (harness-vs-oracle measurement-context or convention, H-C vs H-D undecided - no sampled context reproduces the oracle values); no lever possible; see the corpus xfail notes for the mechanism map and reopen condition.** |

**DISPOSITION-COMMIT batch (2026-07-14) — summary.** This batch also carries `corpus-yang.yaml`'s `yang-forward-block`/`-hit` YANG-RULING note (a user ruling, 2026-07-14: the S 5-vs-7 dispute is a genuine STARTUP actionability convention choice between two hardware-real quantities — box-appear tick 6 vs arm/contact tick 8 — decided by the tape's frz-onset signal cross-validated against the close-LP control + `charset.c:2938/2989`; asserts oracle S=7, no code override, stays XFAIL) — a note-only ruling, no golden change (already asserted `from-qjson`). **Batch totals: 1,289 PASS / 60 XFAIL / 1,349 rows → 1,290 PASS / 59 XFAIL / 1,349 rows** (the single `urien-sa3-aegis-activation` flip above is the ONLY verdict change in the batch; every other edit is an xfail-note reword/reclassification or docs addition, zero measured-value drift). All five investigations in this batch (INSTALL-ADJ, HARNESS-BLEED, CONTACT-A, ADV-REANCHOR, YANG-RULING) ran opus-substitute (Fable plan/review-access quota exhausted) — **WANTS FABLE RE-REVIEW on the whole batch**, per each source doc's own re-review-scope section (`<sp>/zero/install-adj/adjudication.md` §8, `<sp>/zero/contactA/design.md` §11, `<sp>/zero/adv/plan.md` §8; harness-bleed and the yang ruling are lower-stakes, doc/adjudication-only changes).

**DRIVE-TO-ZERO — CLOSING SUMMARY (2026-07-14, honest-floor lock; opus-substitute / WANTS FABLE RE-REVIEW).** The drive-to-zero program is closed at the **honest floor: 1,290 PASS / 59 XFAIL / 1,349 rows, NO override** (user decision 2026-07-14). **UPDATE 2026-07-17 (Fable re-review, lever V):** the floor moved to **1,294 PASS / 55 XFAIL** — the Fable re-review found remy-sa1's S −1 was mis-classified as the oracle-table convention split when it is an engine slot-0-latch artifact (`proj_spawn_raw=0`); lever V (display-only slot-0→post-append harmonization) resolves remy-sa1 ×4 to the oracle-true value (Startup=1). This is not an "override" — it re-pins to the oracle-verified value by fixing an engine artifact, wrong values still never ship. **UPDATE 2026-07-17 (Fable re-review, lever W):** the floor moved again to **1,296 PASS / 53 XFAIL** — the re-review falsified the ADV-REANCHOR negative's "no census signal isolates" premise (the `cut` census column separates the 8-leg yang/oro airborne-landing family from all 86 correct lever-R adv rows), and lever W (LANDING-CUT) re-anchors their advantage off the attacker's natural r1 edge, flipping yang-senkyuutai-lk-block + yang-exsenkyuutai-block XFAIL→PASS at published oracle (TERMINAL-NON-SURGICAL 17→15). Each of the remaining 53 XFAIL rows carries a complete corpus note stating its measured-vs-oracle delta, its hardware-proven/sibling-proven true value (where one exists) with the arcade session that measured it, the exact reason it is not surgically fixable, and its terminal class. No row is UNCLASSIFIED. What this program shipped and ruled out:

- **5 shipped levers / dispositions (the whole flippable set).** Lever **S** (`fd_movestart_same_tick_credit_hold`, ENGINE-D2 — startup same-tick credit), lever **R** (`fd_contact_busy_edge_r`, CONTACT-2 — contact-leg busy-edge recovery), lever **T** (`fd_uoh_...`, UOH-CLOSURE — UOH contact-leg busy-edge R), lever **U** (`fd_jinchu_bounce_recovery_r`, ENGINE-JINCHU — oro-jinchu HIT busy-edge R), and the **INSTALL-ADJ OMIT** (`urien-sa3-aegis-activation` R-assertion removed per the oro-sa3 proj-split precedent, XFAIL→PASS). These absorbed the last of the surgically-closable rows.
- **3 non-surgical negatives, each honest (no lever ships; the true value is known but no suite-safe rule reaches it)** — ADV-REANCHOR, the original 4th, was SUPERSEDED 2026-07-17 by lever W (see next bullet). **CONTACT-A** (`<sp>/zero/contactA/design.md`): the frz-excluded strict box-count reproduces oracle on all 6 arcade-confirmed contact rows but is engine-signal-INDISTINGUISHABLE from **113** currently-PASS contact rows it would regress (structural-twin ken-srk-lp / sean-dragonsmash-mp). **HARNESS-BLEED** (`<sp>/zero/harness-bleed/footprint.md`): the reconstructed global dummy-idle-reset is net-NEGATIVE (fixes 1 row, regresses 4 arcade-correct rows, 3 of them wrongly whiffing connecting moves). **PROJ-SPLIT** (`<sp>/zero/proj-split/fit.md`): the projectile-Startup −1 is an oracle-table convention inconsistency across characters (ryu-sa1 engine S=3 PASS ≡ twelve-sa1 engine S=3 XFAIL, byte-identical signal); a broad S+=1 flips 6 but regresses ≥14 S-matching rows (8 in fit.md §2 + 6 oro-sa2 legs the enumeration omitted). **CORRECTION 2026-07-17 (digest finding #1):** urien-sa2 is NOT "internally over-budget" — the 91 in "S+R=92>T=91" is the engine meter window, not an oracle quantity; the arcade 94-frame post-flash busy window makes oracle S=1+R=91 jointly satisfiable, so the row is an ENGINE-MODEL limitation (single-slot proj-split model + open 91-vs-94 meter-window question), not a self-contradictory table. Each negative is honest because the engine is LAYER-1 arcade-faithful and the residual is either a self-inconsistent oracle, a signal-less off-by-1, or a per-move meter artifact with no local discriminant.
- **ADV-REANCHOR → lever W (LANDING-CUT), SHIPPED 2026-07-17 (Fable re-review).** The original ADV-REANCHOR negative's load-bearing premise — "no census signal isolates the divergent rows from the 86 correct lever-R rows" — was FALSIFIED by the Fable re-review (`<sp>/zero-b/`): the `cut` census column cleanly separates the yang/oro airborne-landing family from all 86 correct lever-R adv rows. Of the 90 lever-R-firing adv rows, 86 (all correct) have `cut==0`; `leverR_pred>=0 ∧ cut==1` selects EXACTLY 8 legs (yang senkyuutai-lk / EX senkyuutai, oro oniyama-lp / EX oniyama, each ×{block,hit}). Mechanism (`<sp>/zero-b/b4-mechanism-reanchor.txt`): the §13.5.1b landing cut backdates `attacker_idle` to `cghi1_first_frame`, EARLIER than the attacker's natural r1(nonzero→0) edge (`atk_r1_end`) that lands 6 ticks (yang) / 13 ticks (oro) later; re-anchoring `adv = def_idle − atk_r1_end` reproduces published hardware EXACTLY on the four block legs (yang −35/−26, oro −34/−60, `<sp>/zero-b/b4-oracle-truth.txt`). Lever W (`fd_landing_cut_adv_reanchor`, `frame_data_overlay.c` §13.22) ships this display-only re-anchor gated on the pure contact-leg lever-R shape (`fd_lever_r_applies ∧ !move_is_uoh`) ∧ `cut_committed` ∧ `atk_r1_end>=0`. Net: **+2 PASS** (yang-senkyuutai-lk-block, yang-exsenkyuutai-block XFAIL→PASS at oracle; the 2 oro block legs stay PASS but gain from-qjson adv assertions; the 4 hit legs re-pin display only — Hit_adv is unpublished "-" in both JSONs, so the hit values are mechanically derived from the same corrected recovery edge, NOT oracle-claimed). **Residual:** akuma-sa3-block / chunli-sa1-block (+1 divergence, `cut==0`, NOT landing-cut) stay TERMINAL-NON-SURGICAL — a distinct class the cut discriminator does not cover. **Cross-check risk (empirical, not by-construction):** the live finalize-deferral gate and the finalize-time `fd_lever_r_applies` gate could disagree (lever-T staleness precedent); the ship carries a `leverW_live` FINAL-line cross-check diagnostic, and any timeout/new-move escape leaves `atk_r1_end` −1 → legacy display, never fabricated.
- **CAP-3 (projectile-visibility instrument, built + validated).** `<sp>/zero/cap3/instrument-report.md` built and validated (Ryu Hadouken S=10/R=36/T=46 exact) a projectile-object rig that reads the spawned "tama" object's own attack box (base `0x02028990`, list index 3, box `+0x2C8`). Phase-2 (`phase2-report.md`) captured all 5 disputed projectile supers on hardware — **zero capability-gaps remain**; the projectile rows are now hardware-measured DOCUMENTED DIVERGENCE (twelve-sa1 S = oracle-table convention split, `proj_spawn_raw=3`; ibuki-sa3 R +2 vs sean-sa1 R −2 opposite-sign; urien-sa2 beam-A distance-scaling refuted by flat arcade T=144), not blocked capability. They stay XFAIL asserting the oracle. **CORRECTION 2026-07-17 (lever V):** remy-sa1 S was NOT the table-convention split — it is an engine slot-0-latch artifact (`proj_spawn_raw=0`, spawn on the MOVE_START frame), surgically resolved display-only by lever V; remy-sa1 ×4 flip XFAIL→PASS at oracle Startup=1 (see synthesis §13.16 DATED CORRECTION + `<sp>/zero-b/`).
- **CAP-4 (chariot whiff-reachability, terminal).** `<sp>/zero/cap4/chariot-impl-report.md`: a centered-mode forced whiff at the harness DIST_MAX ceiling (dist=322 at dash-fire) STILL CONNECTS (HIT, not WHIFF) — the urien-chariot specials-reachability gap is empirically confirmed across the entire achievable placement envelope. Path A is falsified; the only remaining route is a new no-collision dummy-harness driver mode (out of scope). `urien-chariot-hk-block/-hit` stay XFAIL, terminal REACHABILITY-GAP.

**Per-class XFAIL census (the honest floor; sums to 53 — reconciled 2026-07-17 (lever W), superseding the earlier 55 / 59).**

| Terminal class | Rows | Members |
|---|---|---|
| **TERMINAL-NON-SURGICAL** | 15 | akuma-sa3-block, chunli-sa1-block, urien-sa1-block, alex-sa3-whiff, sean-exdragonsmash-block/-hit, remy-cbk-lk-block/-hit, remy-cbk-ex-block/-hit, chunli-sa2-block, ibuki-sa2-hit, ibuki-sa2-chiblast-block, urien-vkd-lk-block, sean-ryuubi-hk-block |
| **ENGINE-DIVERGENCE** | 12 | twelve-sa3-activation, elena-exspinscythe-block, elena-lynxtail-lk-block, oro-jinchu-lk-block, oro-exjinchu-block, oro-exjinchu-hit, necro-flyingviper-lp-block/-hit/-crouch-probe, necro-exflyingviper-block/-hit/-crouch-probe |
| **OVERLAY-DISPLAY-DIVERGENCE** | 10 | urien-uoh-block/-hit, dudley-sa2-block/-hit, h-exml-hit, makoto-sa1-block, remy-rrf-ex-block/-hit, ibuki-kubiori-lp-block/-whiff |
| **CAPABILITY-GATED-remaining** | 4 | twelve-sa1-block/-hit, ibuki-sa3-block/-whiff (was 8; remy-sa1 ×4 removed 2026-07-17 — lever V resolved the slot-0-latch artifact, XFAIL→PASS at oracle Startup=1) |
| **CONVENTION/RULED** | 6 | yang-forward-block/-hit, yun-zesshou-hp-block/-hit, yun-exzesshou-block/-hit |
| **REACHABILITY-GAP** | 4 | urien-chariot-hk-block/-hit, sean-sa1-block/-hit |
| **ENGINE-MODEL-LIMITATION** | 2 | urien-sa2-block/-hit (re-adjudicated 2026-07-17, digest finding #1: the "ORACLE-TABLE-INCONSISTENCY / over-budget" label is refuted — engine single-slot proj-split model + open 91-vs-94 meter-window question. Genuine oracle-table convention split is twelve-sa1, under CAPABILITY-GATED-remaining) |
| **Total** | **53** | |

**(Census reconciliation, dated 2026-07-17.)** The rows above now sum to 53 (15+12+10+4+6+4+2), matching the **Total** row and the post-lever-W suite count (1,296 PASS / 53 XFAIL); the earlier figure of 55 is superseded by the lever-W yang-senkyuutai-lk-block + yang-exsenkyuutai-block XFAIL→PASS flip (TERMINAL-NON-SURGICAL 17→15), which itself superseded 59 via the lever-V remy-sa1 ×4 flip (CAPABILITY-GATED-remaining 8→4). Mirrors the twin-table reconciliation in `docs/frame-data-synthesis.md` §13.16.

**WANTS FABLE RE-REVIEW (whole drive-to-zero close).** The 5 levers/dispositions, the 3 remaining negatives (ADV-REANCHOR now superseded by lever W), CAP-3, CAP-4, and the honest-floor census all ran opus-substitute (Fable plan/review-access quota exhausted) — queued in `<sp>/zero/FABLE-REREVIEW-QUEUE.md`. Load-bearing claims to second: the remaining non-gateability proof (CONTACT-A 113-regression), the lever-W cut-discriminator + landing-cut re-anchor (the empirical live-vs-finalize gate cross-check especially), the oracle-table-internal-inconsistency finding (PROJ-SPLIT identical-signal collision on twelve-sa1; the urien-sa2 "S+R>T over-budget" sub-claim was REFUTED 2026-07-17, digest finding #1 — engine-model limitation, not table self-contradiction), the CAP-4 reachability terminal, and the TERMINAL-NON-SURGICAL vs OVERLAY-DISPLAY-DIVERGENCE vs ENGINE-DIVERGENCE boundary calls (elena-exspinscythe R+10 and the necro crouch-probe are the closest).

**Waves S3-S6 (2026-07-13, commits `3592f10b`/`a385799e`/`d80a8eca`/`570d8d11`) — this tracker row was never updated past wave S2 until this closing-cleanup pass; caught up here.** **Wave S3** (`3592f10b`, Yang/Ibuki/Makoto, 9 corpora/22 rows, 19 PASS/3 XFAIL): Ibuki SA1 Kasumi Suzaku's MOTION NOT FOUND ground closure was overturned same-session by the air idiom (first try) — the CORPUS-AUTHORING.md lesson this closing cleanup now names; Ibuki SA2's own JUDGMENT row ("Missed grab (Chi Blast)") is CONFIRMED as Yoroi Doushi's own guard-denied follow-up, not a separate slot; Makoto SA2 Abare Tosanami is this program's FIRST flash-overlap (sastop) sighting (all 3 legs, A/R/adv inadmissible); suite 1,251 → 1,273. **Wave S4** (`a385799e`, Elena/Oro/Necro, 9 corpora/24 rows, 24 PASS/0 XFAIL): Elena SA2 Brave Dance's R crosses the legacy-R-surplus systemic threshold (3rd member); Elena SA1 is SA-WHIFF-A member #4; Oro SA2 surfaces the (then-unnamed) travel-distance-dependent-A shape; Oro's Ground/Air Grab oracle rows stay MOTION NOT FOUND after 12 candidates + 2 decomp passes (waza 46/47 the only lead); suite 1,273 → 1,297. **Wave S5** (`d80a8eca`, Urien/Alex/Sean, 9 corpora/22 rows, 14 PASS/8 XFAIL): Alex SA2 Boomerang Raid becomes SA-WHIFF-A member #5 (shipped, WRONGLY, as a plain-PASS literal `13` — the exact defect this closing cleanup's Part-A corrects) and legacy-R-surplus member #4 (HIT-leg R, correctly omitted); Urien SA2 Temporal Thunder adds a further travel-distance-dependent-A sighting; Alex SA3's back-to-back-`kd=1`-legs teleport corruption is this convention's second-order form (named in CORPUS-AUTHORING.md this cleanup); Urien Aegis Reflector (EX) NOT shipped (candidate recipe contradicts the oracle's own "Hit: -", an unresolved-identity gap); suite 1,297 → 1,319. **Wave S6** (`570d8d11`, Q/Remy/Hugo/Twelve, 12 corpora/28 rows, 21 PASS/7 XFAIL, "EXSUPER-1 authoring COMPLETE" — all 19 characters' SA slots now authored): Q SA2 Deadly Double Combination is legacy-R-surplus member #4+ (subsequently corrected to the definitive 5-member census by this cleanup, see synthesis.md §12.2.4); Q's own Far/Close Grab oracle rows stay an unresolved provenance question (arcade-session target); Remy SA2 Supreme Rising Rage Flash shipped its A via a **wrongly-argued** §13.11 path-independence claim (this closing cleanup's Part-A finds the ledger matches NEITHER displayed NOR oracle — OMITTED, not the §13.11 assert originally shipped); Twelve's own ordering-trap sighting (3rd instance, after twelve-ex/twelve-sa1) promotes the buffer-entry fix to the named CORPUS-AUTHORING.md convention this cleanup ships; Hugo Gigas Breaker HIT is unreachable at a deeper level than the known grab-span issue (the throw path never emits a FINAL — an overlay instrumentation gap, not a corpus gap); suite 1,319 → **1,347 rows, all 94 corpora (`git ls-files corpus-*.yaml` minus smoke)**.

**EXSUPER-1 CLOSING CLEANUP (2026-07-13, this pass, on top of `570d8d11`) — PROGRAM COMPLETE.** Fable's closing review found a wrong-data class the authoring waves shipped: three corpora (`corpus-alex-sa2.yaml`, `corpus-ibuki-sa2.yaml`, `corpus-remy-sa2.yaml`) asserted a measured A value one below the oracle as a **plain PASS**, using reasoning (§13.11/§13.13 path-independence, "matching how corpus-ryu-sa2.yaml shipped it") this cleanup shows is insufficient — the same `fd_reset_move()` tick-ordering race the D2 register (`16737236`, synthesis.md §12.2.4 `:978`) already found for Ken SA1/Akuma SA2 is ALSO path-independent, so path-independence alone cannot license a plain-PASS assert. **Part A** re-ran the D2 ledger walk (isolated single-entry re-probes, `<sp>/exsuper/review/`) on all three: Alex SA2 (all 3 legs) and Ibuki SA2 (WHIFF leg) both give ledger==oracle exactly (the D2 signature — displayed value is the race artifact) and converted to `xfail-at-oracle`; Remy SA2 (BLOCK/HIT) gives ledger==NEITHER displayed nor oracle (a hitstop-entangled shape outside the pure ledger walk's scope) and was OMITTED per the ledger-outcome disposition's third bucket. Scoped `--update-golden`/`--check-golden` on the 3 corpora: only the expected PASS→XFAIL flips (4 rows: 3 Alex SA2 legs + Ibuki SA2 WHIFF), zero unexpected drift. **Part B** fixed 4 corpus comment inaccuracies found in the same review (elena-sa1's false "every existing member is OMITTED" claim, urien-sa1's stale sum-preserving member count, makoto-sa1's missing xfail date, ibuki-sa2's grab-HIT R assert needing a verified-endrel note) — zero measurement changes, scoped `--check-golden` confirmed zero drift. **Part C** closed the documentation gaps the review found: CORPUS-AUTHORING.md gained the named buffer-entry ordering convention (3+ sightings: twelve-ex/twelve-sa1/twelve-sa2, second-order form on alex-sa3) and the MOTION-NOT-FOUND-needs-air-recipes lesson (ibuki-sa1); synthesis.md §12.2.4 caught up 6 registers that had drifted out of sync with the corpus files' own headers (legacy-R-surplus to its true 5-member systemic census; SA-WHIFF-A to its true 5-member count plus the Part-A resolutions and a ryu-sa2 re-audit flag; a new travel-distance-dependent-A row, 5-member census, with Sean SA1 explicitly verified-and-excluded; MULTIHIT basis-mismatch A recount to 3, a new named watch entry; flash-overlap watch entry to 2 members plus the makoto-sa2-whiff R exception; the late-despawn row's 2nd member, Sean SA1); this row itself, never updated past wave S2, caught up through S3-S6. **Final tallies (post-cleanup, full-universe `--check-golden`, 94 corpora): 1,347 rows total, unchanged by Part A (only 4 rows flip PASS→XFAIL, no rows added/removed) — see this pass's own final message for the exact PASS/XFAIL split.** `git diff --stat` for this cleanup touches 3 corpus YAMLs (Part A) + 4 corpus YAMLs (Part B) + `tools/frame-data/CORPUS-AUTHORING.md` + `docs/frame-data-synthesis.md` + this file — zero `src/` or `tools/frame-data/*.py` changes. **FOLLOW-UP REGISTER (consolidated from all six waves + this cleanup, for whoever picks up post-EXSUPER-1 work):**
- Hien (EX) air retest — wave-2's own MOTION-NOT-FOUND ground closure never tried the air idiom (same inference gap Kasumi Suzaku exposed); one-run retest.
- Q Far/Close Grab provenance — unresolved after a broad motion-probe ledger; arcade-session target, not Q's own C&DB.
- Urien Aegis (EX) identity — candidate recipe contradicts the oracle's own "Hit: -"; needs a source-level motion-table lookup.
- Oro Ground/Air Grab, waza 46/47 — MOTION NOT FOUND after 12 candidates + 2 decomp passes (plpat09.c/cmd_data.c/plmain.c); waza numbers are the only lead.
- Oro Tengu Stones S/R field-swap hypothesis — flagged UNCONFIRMED, needs an instrumented build.
- Necro Electric Snake R — measures 51 close / 48 at 150 vs oracle 39 on both legs; arcade-session target.
- Urien Temporal Thunder distance-A — large distance-scaling A divergence (11 close vs 83-85 far vs oracle 75), same class as the new travel-distance-dependent-A row.
- Hugo Gigas HIT no-FINAL instrumentation gap — the throw path (r1=2) never emits a FINAL at all; deeper than the known grab-span issue.
- Remy Blue Nocturne dummy-attack capability gap — no dummy-attack primitive exists to trigger the counter path; oracle S=0/A=21/R=27 unreachable, not fabricated.
- Ken SA3 open discrepancy — BLOCK R+10 vs the offline recount of 27, still unresolved after the BUFFER-1 capture-depth raise proved the append genuinely stops regardless of depth.
- D2 tick-ordering ENGINE-candidate — now with the Part-A ledger outcomes (Alex SA2/Ibuki SA2 D2-confirmed, Remy SA2 a distinct ledger-neither case) as added evidence; still no fix ships, own recorded signature required before it graduates past candidate status.
- yang-sa1 §13.13 lever-F toggle — pair with q-sa1's own unconverted candidate for a future build-enabled session.
- remy-sa1 100%-xfail escalation disposition — RESOLVED 2026-07-17 (lever V): the escalation dispositioned to an engine slot-0-latch artifact, fixed display-only, all 4 legs now PASS at oracle Startup=1.
- One-offs still open: Alex SA3 whiff-R -1; Twelve SA1 S-1 / SA2 BLOCK A-gap / SA3 S+5; ~~Remy SA1 S=0 proj_spawn_raw~~ (RESOLVED 2026-07-17, lever V); Hugo SA1 whiff-A 1-vs-2; Ibuki SA3 proj-split R+2; Sean SA3 compound (BLOCK A collapse + HIT A overshoot + HIT R); Yang SA1/SA2 HIT-R; Urien SA1 HIT-R.

**THE EX UNIVERSE IS COMPLETE (2026-07-12).** All 71 `class=EX` rows in `<sp>/exsuper/step0-universe.tsv` (spanning all 19 characters with oracle JSONs) have now been examined and dispositioned across 7 authoring sessions (the Ryu pilot + waves 1-6) — every row is either authored (across up to 3 legs: WHIFF/BLOCK/HIT), or explicitly gapped with a cited, verified reason (never guessed). **Verified corpus count (correcting this row's own earlier "~15" estimate against the actual repo state): 18 `corpus-*-ex.yaml` files exist** (`ls tools/frame-data/corpus-*-ex.yaml`, confirmed 2026-07-12) — Akuma is the one character with a verified-null EX universe (zero `class=EX` rows, zero "ex"-substring move names in `akuma.json`; his meter only buys Super Arts) and correctly has no corpus file. **Per-wave totals** (rows / PASS / XFAIL, independently re-summed off each wave's own closing golden run): Pilot (ryu-ex) 10/10/0; Wave 1 (ken-ex, chunli-ex; akuma null) 15/15/0; Wave 2 (dudley-ex, yun-ex, ibuki-ex) 21/19/2; Wave 3 (yang-ex, makoto-ex, elena-ex) 27/25/2; Wave 4 (oro-ex, urien-ex, alex-ex) 22/20/2; Wave 5 (necro-ex, sean-ex, q-ex) 23/18/5; Wave 6 (remy-ex, hugo-ex, twelve-ex) 28/23/5. **Program EX-only total: 146 rows / 130 PASS / 16 XFAIL across 18 corpora** — independently re-summed directly off `<sp>/wave6-post.log`'s per-corpus columns for every `*-ex` line, matching the per-wave sum exactly. **Quarantine ledger, final status:** 8 named program quarantines (#1 Ken SA3 BLOCK R+10; #2 Ryu EX Tatsumaki BLOCK+HIT R; #3 Dudley Jet Uppercut EX BLOCK R+1; #4 Dudley MGB EX BLOCK+HIT R; #5 Makoto Hayate EX BLOCK+HIT A+R; #6 Makoto Tsurugi EX BLOCK adv; #7 Urien VKD EX BLOCK+HIT R; #8 Urien Chariot EX BLOCK+HIT R) — 6 DISCHARGED (converted to measured engine truth) by the EX-CONTACT-R investigation (#2,#3,#4,#5,#7,#8); 2 remain: #1 EXPLAINED-INSTRUMENT-SIDE (M1, FD_METER_LEN=72 window misplacement, true value = oracle exactly, but no shipped corpus entry exists to flip — a supers-program item, not EX); #6 M4 honest residue (Tsurugi's family publishes no oracle R, removed from the EX-CONTACT-R list specifically). **Unclassified one-off xfail sightings (own `docs/frame-data-synthesis.md` §12.2.4 rows, not force-merged into any bucket):** Yun Zesshou Hohou EX (sum-preserving, magnitude-1, wave 2), Necro Flying Viper EX (sum-preserving, magnitude-5, wave 5), Sean Dragon Smash EX (contact-only A-overcount, box-backed, wave 5), Remy RRF-EX (non-sum-preserving joint A/R shift, wave 6, new this wave) — 4 distinct one-off shapes, none reaching the ≥3-member STOP trigger; plus 2 hypothesis-only sightings pending their own named follow-up (Makoto Hayate EX / Hugo Monster Lariat EX, both candidate members of the (a1)/(a2) contact-A-undercount family, neither confirmed) and Remy CBK-EX (precedent-following continuation, not a new shape). **WHIFF-UNREACHABLE / structurally-unreachable census:** at least 11 legs across the program (Remy LOV EX Low, Twelve N.D.L. EX, Ryu/Ken/Sean/Yun/Ibuki-family multi-hit or dash-reach moves, Oro Air Jinchu Nobori EX BLOCK+HIT, Alex Slash Elbow EX BLOCK+HIT, among others cited in their own wave reports) — every instance is a cited move-reach or structural-evasion fact, never a guessed omission. **Motion-gap census (NOT AUTHORED, no oracle edit, no guess):** Dudley Cross Counter EX (NONNUMERIC_UNACCOUNTED, oracle publishes zero frame data), Sean Tackle EX, Oro Sun Disk Palm EX High, Twelve D.R.A. EX (all three MOTION NOT FOUND after exhaustive candidate probing), Ibuki Hien EX (genuine motion-primitive gap, disconfirmed Kasumi-Gake-follow-up hypothesis) — 5 rows. **AIR_DEFERRED census:** 11 rows programwide (both EX and SUPER class) per `step0-universe.tsv`; of the EX-class subset, 2 are now AUTHORED (Oro Air Jinchu Nobori EX, wave 4, WHIFF-only reachable; Twelve Air A.X.E. EX, wave 6, ALL THREE legs reachable, the program's air-milestone capstone) and 5 remain un-authored (Alex Air Knee Smash EX, Alex Air Stampede EX, Ken Air Tatsumaki Senpuu Kyaku EX, Ryu Air Tatsumaki Senpuu Kyaku EX, Ibuki Kunai EX) — **the air-idiom's 3-for-3 track record this program (Makoto Tsurugi EX mains discovery, Oro Air Jinchu Nobori EX, Twelve Air A.X.E. EX, the last with zero divergence on every leg) makes these 5 remaining rows provably reachable candidates for a future dedicated air pass, not a re-opening of the motion-gap census** — named follow-up, not started this session. **Follow-up register (full list, for the next session/orchestrator):** (i) hugo-lever-F — confirm or refute Hugo Monster Lariat EX HIT's contact-A undercount against the (a1)/(a2) family via a lever-F toggle build; (ii) Makoto Hayate EX's own A+R quarantine (#5-adjacent UNCLASSIFIED remnant) same lever-F need; (iii) the 5 input-recipe gaps above (Cross Counter, Tackle, Sun Disk Palm High, D.R.A. EX, Hien EX) — no new candidates identified this program, would need a fresh motion-search strategy or an out-of-band source; (iv) the LOV-family harness limitation (Remy) — a genuine harness/adjacency bug (not an engine-truth question) worth a dedicated root-cause pass, currently only worked around via corpus ordering; (v) a possible **AIR PASS** authoring the 5 remaining AIR_DEFERRED EX rows now that the air idiom has a 3-for-3 track record; (vi) Ken SA3's quarantine #1 display-convention question, explicitly deferred to the supers program (Step 13b, NOT STARTED). **Supers scope note:** this closure covers the EX half only (71/71 rows); the 75 SUPER-class rows in `step0-universe.tsv` and Step 13b's ≤3-super-sections-per-corpus authoring remain a separate, not-yet-started program phase. See `<sp>/exsuper/remy-ex-report.md`, `<sp>/exsuper/hugo-ex-report.md`, and `<sp>/exsuper/twelve-ex-report.md` for the full per-move tables. **SUPERS PILOT TRIO SHIPPED (2026-07-13)** — Ruling 5's pilot trio lands as three new satellite corpora, one per 2b-probed shape: `tools/frame-data/corpus-ryu-sa1.yaml` (Shinkuu Hadouken, proj-split; 2 rows BLOCK+HIT, every asserted field oracle-exact, plain PASS, zero new findings — no WHIFF leg, fireball-can't-whiff precedent), `corpus-ken-sa3.yaml` (Shipuujinrai Kyaku, multi-run, box_runs=10 on HIT; 2 rows PASS asserting S/A/adv on BLOCK + S-only on HIT, whose A fails the Ruling-2 juggle check 27≠9; R OMITTED both legs under the pre-registered M1 quarantine — BLOCK re-measured byte-identical S=1/A=9/R=37/adv=-11, HIT is a newly-measured leg of the SAME named quarantine, not a new seed), and `corpus-chunli-sa2.yaml` (Houyoku Sen, clean single-run; 2 rows: BLOCK asserts S/adv oracle-exact and xfails A as a NEW unconfirmed Phase-6 bucket-8 CANDIDATE — measured 20 vs oracle 19, a +1 box-backed overcount, see `docs/frame-data-synthesis.md` §12.2.4's supers-pilot trigger re-check (grouped honestly, no bucket reaches 3, trigger NOT tripped); HIT asserts S only per the failing juggle check 19≠20; R OMITTED both legs, T=72 saturated, HIT busyr=35=oracle exactly). **M1 register grows 2 → 5 proven legs across 3 moves (Ken SA3 BLOCK+HIT, Dudley MGB EX BLOCK, Chun SA2 BLOCK+HIT) and is now a NAMED, NOT-STARTED candidate for an FD_METER_LEN-raise fix cycle** — see the new M1 register note in `docs/frame-data-synthesis.md` §12.2.4. **Whiff-reachability:** both forward-motion supers stay WHIFF-UNREACHABLE even under `dist_mode:centered` at dist=300 (Ken walk-in ~42px, Chun ~198px — each reproduces 2b's anchored-mode figure under a different placement geometry, corroborating walk-in as a real per-move property); O-A (lever-N busy-0 anchor) remains unconfirmed on any real super WHIFF, still open for 2c/arcade. **Gates:** pre-update full `run-suite.sh --check-golden` = **40 corpora, 40 GREEN / 0 RED**, 37 goldens zero-drift + exactly the 3 expected MISSING satellites (tee'd `<sp>/sapilot-pre.log`); scoped `--update-golden ryu-sa1 ken-sa3 chunli-sa2` (3 NEW golden TSVs, 2 entries each, 3 GREEN); final full `--check-golden` exit 0, **40/40 GREEN, zero drift, 40 goldens OK** (tee'd `<sp>/sapilot-post.log`). **Arithmetic: 1,205 + 6 = 1,211 rows (1,149 + 5 = 1,154 PASS / 56 + 1 = 57 XFAIL)**, independently re-summed off the post-run suite table's per-corpus columns (1,154 + 57 = 1,211 exact). Zero engine/tooling/gameplay changes — corpus YAML + golden TSVs + the two doc ledgers only; zero builds beyond run-suite.sh's own build-once protocol. **Wall-time calibration (the supers-wave planning datapoint, `<sp>/exsuper/sa-pilot-report.md`):** per-move cost tracks SHAPE, not move count — a clean proj-split/single-run super with no fresh divergence ≈ 2-5 min (Ryu SA1: ~2); one genuinely new divergence to classify roughly triples it (Chun SA2: ~8); a multi-run super with an already-known-quarantined R costs the most even with the mechanism pre-solved, since both legs' juggle/quarantine status needs independent per-leg verification (Ken SA3: ~13). The shared one-time ramp (policy + prior reports + oracle facts) is now paid once for the whole ~56-slot supers population. **Wave guidance for the ~50 remaining SA slots:** budget fan-out by predicted shape — any super whose whiff box_runs>1 budgets near Ken's figure; expect every long-duration super whose R window is still open at raw index 71 to JOIN the M1 class (omit R with the M1 citation) rather than seed a new quarantine; flat per-character costing will mis-budget. **SUPERS WAVE S1 SHIPPED (2026-07-13) — first genuine super whiffs.** Three sibling agents authored the remaining Ryu/Ken/Chun-Li SA slots, file-disjoint, zero builds: `corpus-ryu-sa2.yaml` (Shin Shoryuken, 3 rows BLOCK/HIT/WHIFF — **the program's first pilot-idiom super whose WHIFF leg is genuinely reachable**, a short-reach stationary DP-style anti-air per Ruling 1's own pre-registered prediction; §13.11 declared-credit convention applies cleanly to the new box_runs=2 multi-run-whiff population, A=12 path-independent on BLOCK+WHIFF, plain PASS; HIT-R UNCLASSIFIED, see below), `corpus-ryu-sa3.yaml` (Denjin Hadouken, proj-split, 1 row HIT only — BLOCK genuinely unreachable across 3 independent attempts against an actively-guarding dummy; **DOMAIN-CHECKED, not re-dispositioned**: the naive "unblockable-by-design" hypothesis was tested against `ryu.json`'s own row and falsified — `Block_advantage="0"` is a published NUMERIC value, not the non-numeric `"-"` this row uses for `Hit`/`Hit_advantage`, so the oracle's data model treats the move as blockable; the numeric 0 is now the anomaly, quarantine unchanged, deferred to Ruling 4's 2c arcade session), `corpus-ken-sa1.yaml` (Shoryureppa, 3 rows, WHIFF-reachable) and `corpus-ken-sa2.yaml` (Shinryuken, 3 rows, WHIFF-reachable, fully oracle-exact on S/A/BLOCK-adv), `corpus-chunli-sa1.yaml` (Kikoshou, 3 rows, **genuine WHIFF**) and `corpus-chunli-sa3.yaml` (Tensei Ranka, 3 rows, **genuine WHIFF**, fully clean on S/A/adv). **Both Chun-Li supers and both Ken supers reached genuine WHIFF this session** — combined with Ryu SA2, this wave alone contributes 5 of the program's now-6 confirmed-reachable super WHIFFs (the pilot trio's own report found zero), closing authoring-policy.md Ruling 4 item 2 for every move in this wave; O-A (lever-N busy-0 anchor) still awaits its first real confirmation, unchanged, open for 2c/arcade. **M1 register grows 5→14 legs across 3→7 moves** (Ken SA1 all 3 legs, Ken SA2 BLOCK+HIT with a first-of-its-kind clean WHIFF-leg control disproving universal T=72⇒truncation, Chun SA1 all 3 legs — the cleanest M1 sighting yet, Chun SA3 WHIFF-leg only) — see the new `docs/frame-data-synthesis.md` §12.2.4 M1 register-growth paragraph. **New findings, honestly grouped (not force-merged) — see the same doc's new §12.2.4 notes:** (1) a whiff-inclusive uniform -1 A undercount appears on both Ken SA1 (all 3 legs, adv unaffected — ESCALATED per Phase 4's whiff-divergence-is-a-STOP rule, needs a lever-F toggle build) and Chun SA1 (all 3 legs, adv co-shifts +1 — UNCLASSIFIED, HYPOTHESIS-ONLY M1-adjacent); tested against each other and kept as 2 distinct shapes (adv-coupling differs) rather than force-merged, bundled together as one build-gated follow-up item alongside the FD_METER_LEN fix-cycle candidate since both need an actual build session to resolve; (2) Chun SA3's own BLOCK+HIT legs carry a separate +2 R-surplus UNCLASSIFIED shape (busyr disagrees, explicitly non-M1), 2 members, below threshold; (3) Ryu SA2's HIT-R UNCLASSIFIED (+5 surplus, T=63 rules out M1, no Phase-6 HIT-R bucket fits), 1 member, standalone. **≥3 same-shaped-quarantine trigger re-checked honestly across the full program ledger this wave:** no group reaches the threshold (the A-undercount pair stays at 2, explicitly distinct shapes; the two new R-surplus sightings are each below 3 on their own terms). Trigger NOT tripped. **Gates:** pre-integration full `run-suite.sh --check-golden` = **46 corpora, 46 GREEN / 0 RED**, 40 goldens zero-drift + exactly the 6 expected MISSING satellites (tee'd `<sp>/waveS1-pre.log`); scoped `--update-golden ryu-sa2 ryu-sa3 ken-sa1 ken-sa2 chunli-sa1 chunli-sa3` (6 NEW golden TSVs, 6 GREEN); final full `--check-golden` exit 0, **46/46 GREEN, zero drift** (tee'd `<sp>/waveS1-post.log`). **Arithmetic: 1,211 + 16 = 1,227 rows (1,154 + 13 = 1,167 PASS / 57 + 3 = 60 XFAIL)**, independently re-summed from the pre-check log's own per-corpus total/PASS/XFAIL columns (40 pre-existing corpora sum to exactly 1,211/1,154/57, matching this row's own prior closing tally; the 6 new corpora sum to 16/13/3). Zero engine/tooling/gameplay changes — corpus YAML + golden TSVs + the two doc ledgers only; zero builds beyond run-suite.sh's own build-once protocol. See `<sp>/exsuper/ryu-sa-report.md`, `<sp>/exsuper/ken-sa-report.md`, and `<sp>/exsuper/chunli-sa-report.md` for the full per-move tables. |

**BUFFER-1 tracker row (2026-07-13), DONE — capture-depth raise, RE-ANCHOR-1's gate template applied.** `frame_data_overlay.c:42`'s `FD_METER_LEN=72` raw[]-capture cap (the mechanism behind this file's own M1 register, 24 legs / 11 moves, "NAMED CANDIDATE... NOT STARTED" as of supers wave S2) is now split from the 72-cell DISPLAY width via a new `FD_CAPTURE_LEN=256` constant (capture sites: `raw[]`/`meter_len`/`atk_cells`/`def_cells`/the append gate; display sites — `FD_METER_LEN`, `FdLatched` cell arrays, the draw-row clamp, meter geometry — untouched). Load-bearing remedy: the finalize + live engine-active-count paint-override blocks (cosmetic-only, never touch S/A/R) are bounded to `min(meter_len, FD_METER_LEN)`/`slot<FD_METER_LEN` so the first-72 DISPLAY cells stay byte-identical at every capture depth — proven by the `FD_CAPTURE_LEN=72` mutation-contract identity run (55/55 GREEN, zero drift; capacity constant, not a semantic lever per §13.4's own argued exemption — no ninth house lever added). **Gate:** identity run (72) 55/55 GREEN zero drift; raise run (256) full-suite pre-drift diff matched the predicted 21-of-24 clean oracle-equal M1 legs at their pre-registered `busyr`/oracle values exactly, correctly flagged the chunli-sa2-block leg's own busyr-only (non-oracle-equal) value as unasserted rather than silently shipped, PLUS correctly flagged the Ken SA3 exception (both legs did not resolve as predicted — a genuine discrepancy vs the prior offline recount, reported not forced) and a wider `ended_by_partner_release` side-effect family (Hugo's six grab HIT rows + 7 other characters' basic Throw HIT legs, reopening `docs/frame-data-synthesis.md` §13.10's own "No FD_METER_LEN resize" rejection as an unavoidable consequence of this unrelated fix) — see that doc's own M1 RESOLVED-BY-FIX entry for the full accounting. Scoped `--update-golden` on the 25 affected corpora; final full `--check-golden` 55/55 GREEN zero drift; determinism ×2 (byte-identical); eight house levers re-grep-asserted `=1`; `git diff --stat src/` exactly `frame_data_overlay.c`; zero `tools/*.py` changes. **Bundled in the same session: SA-WHIFF-A's own Q2-D2 evidence-gathering** (env-gated `fprintf` probe in `charset.c`'s `fd_engine_active_count` accumulator, scratch-reverted, `git diff` on `charset.c` confirmed empty after revert) — found the ledger walk's own hand-derivation (34/24 for Ken SA1/Akuma SA2) does NOT match live execution (33/23, matching displayed exactly); root cause is a `fd_reset_move()`-vs-`char_move()` tick-ordering race that silently drops the first active cell's credit on all three members (Ken SA1, Dudley SA1, Akuma SA2), independent of lever F (toggle runs showed zero effect on any of the three, ruling out revoke/restore). Recorded as an ENGINE-candidate finding (docs/frame-data-synthesis.md's own SA-WHIFF-A register), no fix shipped this cycle — Ken SA1's wave-S1 ESCALATED flag resolves to this finding. See `<sp>/bufferfix-plan.md`, `<sp>/bufferfix/census.tsv`, `<sp>/bufferfix/pre-drift-256.log`, `<sp>/bufferfix/q2-ledger-report.md`.

Each step lands as its own commit on `frame-data-on-mister`. No pushes without
explicit consent. No release artifacts until the user says so.

### SWEEP-3 user grant list (consolidated, 2026-07-11)

Every pending-grant item this run's fable-grade audit surfaced, in one place.
Conversion happens only if/when the user grants — none of these ship this
commit.

- **G1** `ibuki-uoh-block/-hit` R (new class row, §12.2.4 "Contact-outcome
  hold/skip differential"): grant ⇒ `R:6`/`R:4`, arcade 5 preserved in
  comment ⇒ **+2 flips**. **GRANTED + EXECUTED 2026-07-11** — user grant
  2026-07-11, following the orchestrator's recommendation review: "the
  ibuki-uoh guard-hold/cut-skip mechanism becomes a granted §13.13 family."
  Family **F12**. Both entries FULL CONVERT, both flips landed; see the
  `G12-GRANTS` tracker row above.
- **G2** same-tick interior-transition credit banking (§12.2.4 new row):
  `elena-lynxtail-lk-hit` grant ⇒ `A:4` ⇒ **+1 flip**; `elena-lynxtail-lk-
  block` ⇒ NARROW only (its R clause stays open, (e)-permanent per the F7
  candidacy refutation); `remy-cbk-lk-block/-hit` ⇒ convert only if the user
  ALSO accepts the whiff-level ±1 boundary shift (`remy-cbk-lk-whiff`'s own
  proven F=1207 boundary tick) as displayed engine truth (`A:14` = 10 + 1
  boundary + 3 phantom; `R:9`) ⇒ up to **+2 flips**, else narrow only.
  **GRANTED + EXECUTED 2026-07-11** — user grant 2026-07-11, following the
  orchestrator's recommendation review: "the same-tick credit-banking class
  (remy-cbk + elena-lynxtail-A) becomes a granted family." Family **F13**.
  The grant text covers the credit-banking mechanism itself; it does not
  separately state acceptance of the CBK whiff-boundary-shift dependency
  above. Executed strictly per this row's own text: `elena-lynxtail-lk-hit`
  FULL CONVERTS (+1 flip landed); `elena-lynxtail-lk-block` NARROWS (A
  converts, R stays open, per this row's own "else"); `remy-cbk-lk-block/
  -hit`'s stated dependency ("convert only if...") is UNMET as written — the
  2026-07-11 grant does not include acceptance of the whiff boundary shift
  — so both stay fully XFAIL, unconverted (the "else narrow only" branch of
  this row, which for CBK resolves to no independently-assertable literal —
  see corpus-remy.yaml's xfail strings). Net this grant: **+1 flip**
  landed of the up-to-3 possible; the remaining +2 (CBK) stay pending on the
  whiff-boundary-shift acceptance, not yet granted. **DATED NOTE (2026-07-11, LAYER-1): the CBK precondition is VOID, not merely still-pending.** Arcade 990512 whiff = 17/10/10 == oracle (2 reps, `docs/arcade-frame-data/CAPTURE.md`'s new Session 3), and a convention-twin engine-raw probe (a)-classifies it identically; the overlay's 11/9 is a meter artifact (a last-hatt tick whose raw box dims are already zero), not engine truth. Accepting the boundary shift as displayed engine truth would enshrine a measured-wrong value (house rule) -- the pending +2 CBK flips are CANCELLED as a grant question, re-routed to the OVERLAY RE-ANCHOR program (LAYER-1 tracker row), not merely left pending on user acceptance. `remy-cbk-lk-whiff/-block/-hit` stay xfail at oracle values until the meter fix lands. **DATED NOTE (2026-07-11, RE-ANCHOR-1 SHIPPED):** the meter fix landed. `remy-cbk-lk-whiff` FULL CONVERTS (A=10, R=10, both arcade/oracle-exact via levers O/N) -- the G2-VOID route materialized exactly as this note predicted. `remy-cbk-lk-block`/`-hit` (contact legs) are out of RE-ANCHOR-1 scope and stay as they were.
- **G3** ENGINE-5 self-loop display convention, 11 (played) vs 12 (declared)
  — already queued (sweep #2), restated unchanged; no new flips proposed
  this sweep. **NOT granted 2026-07-11 — still pending, untouched.**
  **CLOSED DECLINED 2026-07-11 (CAPTURE-1, orchestrator ruling; see the
  CAPTURE-1 tracker row below).** Arcade ground-truth capture
  (`docs/arcade-frame-data/CAPTURE.md`) measured Urien UOH active/recovery
  directly on real hardware: A=10, R=5 — confirming, not merely repeating,
  `urien.json`'s own declared row. No engine-reachable quantity (9
  measured / 11 played / 12 declared) equals 10, so this grant is
  **DECLINED**, not deferred further: displaying 11 would now knowingly
  enshrine a value confirmed to disagree with both the oracle and live
  arcade. `urien-uoh-whiff/-block/-hit` A-clauses stay xfail at measured
  9 (R-clauses stay xfail at measured 3 vs arcade-confirmed 5), re-cited
  to the capture evidence — see `docs/frame-data-synthesis.md`'s ENGINE-5
  closure note and "PORT-DIVERGENCE-1" (§13.13 exclusion item 2). No
  future grant route remains open on this move. **DATED NOTE (2026-07-11, LAYER-1):** G3's rationale gets a dated update, not a reopening -- the engine genuinely plays 10 raw box-active frames (convention-twin measurement, `docs/frame-data-synthesis.md` §13.16); "no engine-reachable quantity equals 10" was true only of the overlay's credit-based accounting. G3 stays CLOSED DECLINED (no convention grant needed) -- the 11-vs-12 display question is expected to DISSOLVE, not be answered by a grant, if the OVERLAY RE-ANCHOR program (this file's new LAYER-1 tracker row) lands: a correct meter would display 10 and match arcade/oracle outright.
- **No zesshou conversion option** — `yun-zesshou-hp-block/-hit`'s
  arcade-mapping is two-way-ambiguous (cell78 declared-over-real bank vs
  cell60 freeze-extension tick, and the discriminating whiff baseline is
  harness-unreachable); offering a conversion would risk shipping a wrong
  story, so none is offered (see the SWEEP-3 tracker row and
  `docs/frame-data-synthesis.md` §12.2.4's "Sum-preserving A/R boundary
  shift" row).

Maximum additional flips if every grant above is given in full: **+5**
(G1 2 + G2 up to 3), on top of this commit's 987/72. **UPDATE (2026-07-11,
G1+G2 grant execution):** +3 of those 5 are landed this run (G1's full +2,
G2's `elena-lynxtail-lk-hit` +1); G2's remaining up-to-+2 (`remy-cbk-lk-
block/-hit`) and G3's flips (undetermined count, display-convention choice
not yet made) stay pending, not granted. Running total after this run:
**990 PASS / 69 XFAIL** (was 987/72). **DATED NOTE (2026-07-11, LAYER-1):** G2's remaining +2 (CBK) is no longer pending on a grant -- it is re-routed to the OVERLAY RE-ANCHOR program (this file's new LAYER-1 tracker row), since the CBK precondition is VOID by measurement (arcade+twin 17/10/10, see this section's own G2 dated note above). G3's flips stay undetermined and unchanged by this note (display-convention choice not made; expected to DISSOLVE if the meter fix lands, not to be decided by grant).

---

## Step 1 findings

**Date:** 2026-07-07. **Verdict: BLOCKED** — all three candidate mechanisms
(plus their hybrids) falsified with trace-cited counterexamples; STOP rules 3
and 4 both trigger. Per the step's failure fallback, this write-up is the
deliverable; the issue-#17 xfails stay standing. Every claim below cites a
file:line or a trace excerpt with GT values.

### Method / same-binary protocol

All traces in this section come from one Debug host build (`cmake --build
build/host -j8`) at commit `d7d3d706` plus **temporary, since-reverted**
diagnosis instrumentation in `charset.c` (a `[FDA]` stderr line per
accumulator decision for P1, logging `GT cgix ctr jatix caix add rev cnt
hstop athok tdmstop thstop` — `rev` = the §13.7.4 revoke amount taken this
call, `hstop` = attacker `wk->hit_stop`, `tdmstop`/`thstop` = the target
WORK's `dm_stop`/`hit_stop` at that exact moment). The instrumentation was
removed with `git checkout src/sf33rd/Source/Game/engine/charset.c` and the
binary rebuilt clean before this doc was written; the shipped tree is
untouched. Scratch corpora (never committed) ran via `compile_corpus.py` +
direct game invocation with `FRAME_CM_LOG=1`, replicating `run.sh`'s flags:

- `scratch-ryu.yaml`: the 9 affected Ryu moves (BLOCK) + 4 HIT variants +
  green controls (Jab/Short/Far Forward/cr.HK) + WHIFF probes incl. two new
  ones (cr.MK, Towards+Fierce at dist 250).
- `scratch-q.yaml`: q-strong-block, q-fierce-hit, q-crhk-hit (green contact
  controls) + q-uoh-samef-block, q-uoh-1f-block, q-uoh-chain-retrigger
  (lever-A/B protected cases).
- `scratch-hugo.yaml`: h-jab-block (green) + h-short/h-forward BLOCK + new
  WHIFF probes for both.
- one-entry `q-uoh-whiff` probe (dist 250, dummy none).

All corpus-documented measured values reproduced exactly (e.g.
ryu-strong-block A=2, ryu-far-strong-block A=1, ryu-crmk-block A=1,
h-short-block A=1; UOH samef/1f A=10, retrigger displayed A=10 with
engine_a=20 anchor_a=10). The two new whiff probes confirmed the declared
chains: ryu-crmk-whiff A=5, ryu-twdshp-whiff A=6 — both arcade-exact.

### Re-verified mechanism, refined to THREE sub-shapes

The corpus-ryu header's two shapes re-verified on fresh traces, with one
refinement: shape (a) splits into (a1) and (a2).

- **(b) same-jatix transit revoke.** On the contact tick there are two
  `char_move()` calls. Call #1 is the tick's normal advance (enters the
  contact cell; `hstop=0 athok=1`); hit processing runs *between* the calls
  (sets attacker `hit_stop`, defender `dm_stop`, clears `att_hit_ok`);
  call #2 is the contact reaction advancing the chain one cell
  (`hstop>0 athok=0 tdmstop<0`). When the two cells share `jatix`, the
  `charset.c:455-482` revoke discards call #1's credit.
  Example — ryu-strong-block:
  `[FDA] GT=229 cgix=12 ctr=2 jatix=4 add=2 rev=0 cnt=2 hstop=0 athok=1`
  then `[FDA] GT=229 cgix=16 ctr=2 jatix=4 add=2 rev=2 cnt=2 hstop=9
  athok=0 tdmstop=-9` → A=2 vs arcade 4; the revoked 2 is exactly the
  missing credit.
- **(a1) skip-to-recovery jump.** Call #2 does not walk the chain — it
  enters `check_cm_extended_code` directly at a jump target
  (`cg_next_ix`-style), and the skipped declared-active cells are **never
  dispatched by any code**. Example — ryu-far-strong-block: `[CMX] GT=322
  cgix=8 code=256` → `[FDA] GT=322 cgix=8 ctr=1 jatix=5 add=1` → `[CMX]
  GT=322 cgix=16 code=1536` (recovery, jatix=0; no [FDA]). Skipped cell 12
  (ctr=3, jatix=6) exists only in the whiff trace (`[FDA] GT=1625 cgix=12
  ctr=3 jatix=6 add=3`). A=1 vs arcade 4; lost = 3.
- **(a2) non-char_move advance (NEW).** Towards+Fierce's second hit: the
  trace row at GT=1555 shows `cgix=40 jatix=65 hstop=11 P2.dstop=-11`, but
  the last `[CM]` line at GT=1555 is `cgix=36` and the next `[CM]` at all is
  GT=1567 already dispatching `cgix=44` — the advance 36→40 happens through
  `hit_pattern_extdat_check()` case 0x41 (`src/sf33rd/Source/Game/hitcheck.c:743-752`),
  measured `extdat=0x4C` on this Towards+Fierce hit-2 tick: it sets
  `cg_ctr=1` and parks `cg_ix` at target-minus-one, without ever calling
  `char_move()`. The *event* is observable — a read-only hook at that call
  site sees it fire — but what is unrecoverable is the *amount*: the
  skipped cell's declared ctr, the same class of unrecoverability as shape
  (a1). Cell 40 (declared ctr=2 per whiff `[FDA] GT=2199 cgix=40 ctr=2
  jatix=66 add=2`) contributes nothing. A=4 vs arcade 6; lost = 2.

**Unifying mechanism (review pass, NEW).** All three shapes are driven by
one dispatch point: the contact cell's per-cell ROM byte `cg_extdat`, read
by `hit_pattern_extdat_check()` (`hitcheck.c:723-753`). `extdat=0x80` is
shape (b) (same-jatix transit — case 0x80); `extdat=0x8N` is shape (a1)
(skip-jump); `extdat=0x4N` is shape (a2) (char_move-bypassing park, case
0x41 above). The crux measurement: `cg_extdat` is byte-identical
(`0x80`/case `0x80`) for UOH's arcade-**correct** revoke (GT=53, jatix=54,
hstop=8) and for Ryu Far Jab's (GT=42, jatix=3, hstop=7) / Ryu Strong's
(GT=136, 12→16, rev=2, jatix=4, hstop=9) arcade-**wrong** revokes — the
causal trigger itself carries no discriminator between the correct and
incorrect cases.

**WHIFF never loses credit** (re-confirmed on all 9 whiff probes: every
whiff A arcade-exact; no same-tick advances, no revokes — the revoke's
`Game_timer`-equality gate can only be satisfied by the contact reaction's
second call).

### Per-affected-move table (all rows trace-cited above or in the run logs)

| Entry (BLOCK unless noted) | Arcade A | Measured A | Shape | Lost credit | Evidence (contact tick) |
|---|---|---|---|---|---|
| ryu-far-jab | 3 | 2 | b | 1 = rev | GT=135: cell 16(1) revoked at 16→20, jatix=3 both |
| ryu-strong (+HIT) | 4 | 2 | b | 2 = rev | GT=229 (HIT GT=973): cell 12(2) revoked at 12→16, jatix=4 |
| ryu-far-strong | 4 | 1 | a1 | 3 | GT=322: jump 8→16 skips cell 12 (ctr=3, jatix=6; whiff GT=1625) |
| ryu-fierce (+HIT) | 4 | 3 | b | 1 = rev | GT=414 (HIT GT=1065): cell 12(1) revoked at 12→16, jatix=9; cell 20 (+2) accrues post-hitstop GT=426 |
| ryu-far-fierce | 3 | 1 | b | 2 = rev | GT=514: cell 20(2) revoked at 20→24, jatix=11 |
| ryu-forward (+HIT) | 5 | 3 | b | 2 = rev | GT=693 (HIT GT=1158): cell 12(2) revoked at 12→16(3), jatix=13 |
| ryu-roundhouse (+HIT) | 5 | 3 | b | 2 = rev | GT=883 (HIT GT=1255): cell 24(2) revoked at 24→28(3), jatix=16 |
| ryu-crmk | 5 | 1 | a1 | 4 | GT=1347: jump 12→24 skips cells 16(2)+20(2) (whiff GT=2092/2094) |
| ryu-twdshp | 6 | 4 | a2 | 2 | GT=1555: 36→40 advance outside char_move; cell 40(2) never accumulated (whiff GT=2199) |
| h-short (+HIT per corpus) | 4 | 1 | a1 | 3 | GT=246: jump 8→16 skips cell 12 (ctr=3, jatix=10; whiff GT=653) |
| h-forward (+HIT, stacked) | 6 | 5 | a1 | 1 | GT=453: jump 12→30 skips cell 18 (ctr=1, jatix=12; whiff GT=860) |

Shape accounting over the pure-A xfail entries: shape (b) = 10 entries (6
Ryu BLOCK + 4 Ryu HIT); shape (a1/a2) = the rest (~8 entries incl. Hugo).
`ryu-crlk-block` (stacked with the dm_stop-sign issue) is the same family
per its corpus note and was not separately traced.

### Finding F1 — the same-tick advance is contact-caused, universally
(corrects §13.7.4's structural "double-dispatch" reading)

A fresh `q-uoh-whiff` probe (dist 250): **no same-tick transit exists on
whiff.** Cell 16 plays a full visible tick (`[FDA] GT=53 cgix=16 ctr=1
jatix=54 add=1`, row GT=53 shows cgix=16) and cell 20 enters on the NEXT
tick (GT=54). The 16→20 same-tick transit seen on UOH BLOCK (GT=362) is the
contact reaction — same mechanism as every Ryu shape-(b) move — not a
structural cell-data artifact. §13.7.4's claim that cgix=16 "never persists
for any visible frame" is true only on the contact path.

### Finding F2 — every observed revoke fires on a contact tick, with
identical signatures for arcade-correct and arcade-wrong cases

Across all runs, **14 of 14** revoke events carry the contact signature
(`hstop∈{7,8,9,11} athok=0 tdmstop<0`): 4 UOH events (rev=1 — the ONLY
case where the revoke is arcade-correct, A=10) and 10 Ryu-normal events
(rev=1..2 — all arcade-wrong). Side-by-side, the arcade-correct and
arcade-wrong cases are indistinguishable in every accumulator-visible
field:

```
q-uoh-samef-block  [FDA] GT=362 cgix=20 ctr=2 jatix=54 add=2 rev=1 hstop=8 athok=0 tdmstop=-8
ryu-far-jab-block  [FDA] GT=135 cgix=20 ctr=2 jatix=3  add=2 rev=1 hstop=7 athok=0 tdmstop=-7
```

Both: contact cell declared ctr=1, entered on the contact tick by call #1,
same-jatix transit to a ctr=2 cell by call #2. Arcade counts far jab's
contact cell (A=3=1+2) but not UOH's (A=10 excludes the 1). Checked and
dead as discriminators: jatix identity, contact-cell ctr, entry-tick
position, target-cell ctr, `athok`, `hstop`, `tdmstop`,
`fd_engine_hitbox_active`, `kow`. Four fields DO differ — `cg_hit_ix` (232
vs 22/24), `cg_att_ix` (14 vs 4/5), `cg_cancel` (0 vs 96-120), `pat_status`
(20 vs 0) — but all four are move-identity animation data, not a contact
semantic: `pat_status` is loaded from cell data at `charset.c:661-664`
(`comm_sps`), also at `charset.c:202/2601/2773` and `effect.c:420`, never
assigned as a hit-check flag. This is exactly the data-luck class the
findings reject as a gate; it is rejected here too.

### Finding F3 — oracle convention, and UOH's latent whiff divergence (NEW)

Arcade-table A equals the **whiff-path visible active ticks** for every
measured move: Far Jab 3, Far Strong 4, cr.MK 5, Towards+Fierce 6, Hugo
Short 4, Hugo Forward 6 — and UOH **10**: the whiff rows show cell 32
visible only GT=61..62 (2 rows) though declared ctr=3 (`[FDA]` shows the
third `char_move` tick at GT=63 but the row at GT=63 already reads cgix=0 —
the anim reset lands the same tick), so whiff-visible = 1+2+2+3+2 = 10.
The accumulator's declared sum is 11. Consequently:

- **`q-uoh-whiff` measures `S=15 A=11 R=3` vs q.json 15/10/5 — a latent,
  previously untested divergence in the shipped build** (no corpus has a
  UOH WHIFF entry; run this probe to reproduce).
- UOH BLOCK's arcade-exact A=10 is the sum of two independent, mutually
  cancelling +1s: the contact path hides cell 16 (whose declared 1 the
  §13.7.4 revoke subtracts) while playing cell 32 in full (rows GT=379-381),
  whereas the whiff path shows cell 16 but truncates cell 32. The revoke
  subtracts the *arcade-counted* cell and compensates for the
  *tail-over-declare* — right number, wrong reason.

### Candidate falsifications (§13.7.5-style)

**C1 — credit-skipped-cells at contact (charset.c). FALSIFIED, 3 ways.**
1. *UOH regression:* suppressing/compensating the revoke on contact ticks
   must not touch UOH, but per F2 the discriminating signals proposed by
   the step spec (hit_stop / dm_stop / fd_engine_hitbox_active) are
   identical between UOH and the shape-(b) normals. Un-revoked UOH sums
   1+2+2+3+3 = **11** — the documented lever-A mutation-FAIL value for
   `q-uoh-samef-block`/`q-uoh-1f-block`, and the anchor snapshot
   (`frame_data_overlay.c:1022`) inherits it for the retrigger entry.
2. *Shape-(a1) amounts unknowable in char_move:* the skipped cells are
   never dispatched (F/[CMX] evidence, e.g. GT=322); their declared ctrs
   exist only in ROM cell data. A static, non-executing walk of the cell
   table between prev and new cgix is unsafe: chains contain jump commands
   mid-stream (`[CMX] GT=135 cgix=4 code=49`; `[CMX] GT=2197 cgix=28
   code=49`) and interleave outcome-specific branch cells (Towards+Fierce
   cell 32 is entered ONLY on the block path yet sits between whiff-path
   cells 28→36), so positional adjacency does not equal whiff-path
   membership.
3. *Shape-(a2) invisible:* twdshp's 36→40 advance never calls `char_move()`
   at all (GT=1555 row vs [CM] evidence above) — no charset.c accumulator
   code can observe it without new hooks in additional gameplay routines.

**C2 — contact-aware revoke (narrow lever A to never fire on contact
ticks). FALSIFIED, 2 ways.** (1) The revoke *only ever* fires on contact
ticks (F2: 14/14) — "never on contact" is "never", i.e. disabling the
revoke, which the step spec forbids and which regresses UOH to A=11
exactly as the §1.9.3 mutation test documents. (2) Coverage: shape (b) is
10 of ~18 affected entries; the 8 shape-(a) entries are untouched by any
revoke change.

**C3 — finalize-side correction from a latched lost-credit quantity.
FALSIFIED, 2 ways.** (1) The (b)-half quantity is latchable (charset.c
knows `prior_add` at the revoke), but restoring it at `fd_finalize()` needs
the same UOH-vs-normal discriminator that F2 proves absent — no
finalize-visible quantity separates them either (far jab needs +1
restored, UOH needs +0; engine_a/active_pf/anchor patterns differ per move,
not per class). (2) The ROM cell table is mapped and addressable, but safe
whiff-path membership of the skipped cells is not derivable — `code=49`
`comm_ixfw` relative jumps fire mid-chain (measured `GT=42 cgix=4 code=49`
on far jab) and outcome-specific branch cells break positional adjacency —
so the (a)-half amounts are unrecoverable without a side-effect-free
reimplementation of the 125-opcode `decode_chcmd` interpreter evaluating a
counterfactual path (C1 falsifiers 2-3); there is nothing to latch. The
CnDB `ended_by_partner_release` analogy fails because there the correction
was computable from `meter_len - startup_pf - effective_a`
(`frame_data_overlay.c:669`), all engine-visible.

**Hybrids:** any (b)-half still trips the UOH discriminator; any (a)-half
still needs the unknowable amounts. No composition escapes both.

### STOP-rule mapping

- **STOP rule 3** (discriminating signal not establishable from data
  visible inside `char_move()`/the overlay tick): triggered twice — the
  UOH-vs-normal revoke decision (F2) and the shape-(a) lost amounts
  (C1-2/3). The information required is the *whiff-path* play-out, which
  the contact path provably destroys.
- **STOP rule 4** (3 falsified candidates): C1, C2, C3 above.
- STOP rules 1-2 were respected throughout: no gameplay behavior was
  changed (instrumentation was fprintf-only and reverted); no candidate
  proposing to simply disable the revoke was entertained past its
  falsification.

### Escalation options for the user (Step 2 as specced is NOT implementable)

1. **Leave as-is** (this write-up + standing xfails). Zero risk; the ~18
   entries keep their cited-mechanism xfail status.
2. **Adopt an oracle-convention divergence for UOH** (Step-5-style user
   decision, per the house rules' only-exception clause): ship the honest
   engine-side fix for shape (b) — "contact must never destroy
   already-elapsed declared credit"; suppress the revoke's effect at
   finalize using the latched (b)-half quantity — which flips the 10
   shape-(b) xfails to PASS, and re-baseline the three UOH entries to the
   documented engine truth A=11 (defensible via F3: the arcade UOH row
   disagrees with the engine animation on every path, including the
   currently-shipped whiff behavior, which already displays 11). Costs:
   changes lever-A's protected expectations (plan explicitly gates this on
   user sign-off), leaves the 8 shape-(a) entries open forever (amounts
   unknowable), and reverses Phase 3's flagship "UOH exact" result.
3. **Accept a move-identity gate** (`pat_status==20`-style UOH carve-out)
   to keep UOH at 10 while restoring shape-(b) credit. Rejected by this
   diagnosis as unprincipled (F2), recorded only for completeness.

None of these is chosen here; option 1 is the default state of the tree.
Steps 3+ of this plan are independent of Step 2 and can proceed.

### Review pass (2026-07-07) — CONFIRM-BLOCKED

An independent fable review agent re-measured this diagnosis via fresh
scratch `[HPX]`/`[FDR]` probes (temporary, since-reverted, same
fprintf-only discipline as the `[FDA]` instrumentation above) and
**confirmed the BLOCKED verdict**. Corrections folded into the sections
above:

- Shape (a2)'s code path is `hit_pattern_extdat_check()` case 0x41
  (`hitcheck.c:743-752`, `extdat=0x4C` measured), not
  exset/check_cgd_patdat2 — corrected in the mechanism section.
- All three shapes share one driver: the contact cell's `cg_extdat` byte,
  read by `hit_pattern_extdat_check()` (`hitcheck.c:723-753`) — `0x80` =
  shape (b), `0x8N` = shape (a1), `0x4N` = shape (a2) — and `cg_extdat` is
  byte-identical between UOH's correct revoke and Ryu's incorrect ones (see
  the unifying-mechanism paragraph above).
- F2's "indistinguishable in every accumulator-visible field" overstated
  the case: four fields differ (`cg_hit_ix`, `cg_att_ix`, `cg_cancel`,
  `pat_status`), all move-identity animation data rather than contact
  semantics — corrected above, verdict unaffected.
- C3(2)'s "never exists in any runtime-visible state" overstated the case:
  the ROM cell table is addressable — it's the *derivation* of safe
  whiff-path membership that's unrecoverable (mid-chain jumps,
  outcome-specific branch cells) — corrected above, verdict unaffected.

**Additional hybrid falsified by the review:** restore-all-revokes plus an
end-of-move tail reconciliation (subtract un-elapsed declared credit when
the anim reset kills the last active cell mid-tick). This fixes UOH WHIFF
(11→10) but **not** UOH BLOCK: on block, cell 32 plays its full declared 3
ticks (GT=70-72 rows), so reconciliation subtracts 0 credit and block still
reads 11 ≠ 10. Dead on arrival.

**Reproduction:** the UOH WHIFF latent divergence (F3; measured `S=15 A=11
R=3` vs `q.json` 15/10/5) was independently reproduced on the shipped
build.

**Conclusion:** the verdict stands at BLOCKED. The only remaining routes
are the escalation options above; none is exercised here — that remains a
user decision.

### Resolution (2026-07-07, user decision)

The user adopted **escalation option 2** (declared-truth displayed-A
convention). Step 2b (`docs/plan-frame-data-completion.md` Step 2, executed
under that name) implemented it; see `docs/frame-data-synthesis.md` §13.11
for the normative write-up and `charset.c`'s `fd_restore_revoked_declared_credit`
gate (mutation lever F) for the shipped mechanism.

The census taken during Step 2b's measurement session additionally resolved
`ryu-crlk-block` as shape (b) — Step 1's diagnosis above had left it
untraced (grouped loosely with "stays with shape (a)"); the census found a
`rev=1` revoke event at GT=2089 and the trial read the exact arcade A=3,
same mechanism as the other 10 flipped Ryu entries. The shape-(a)
unrecoverability analysis in this appendix (skip-jump (a1), char_move-bypass
(a2)) stands unchanged and is now normative via §13.11's "what stays
unrecoverable" section. The F3 latent whiff divergence (UOH WHIFF measuring
A=11 against q.json's 10 on the shipped build, reproduced above) is pinned
by the new `q-uoh-whiff` corpus anchor as the convention-correct value. The
UOH whiff-R divergence (measured R=3 vs q.json 5, noted in F3's own
reproduction) was spun off as a distinct open item (synthesis item 18) and
is pinned as a documented xfail by the companion `q-uoh-whiff-r` entry,
rather than left silently unasserted.

**Reconciling option 2's own wording with what shipped.** Option 2 above,
written before this session's revoke census, described the fix as
"suppress the revoke's effect at finalize using the latched (b)-half
quantity — which flips the 10 shape-(b) xfails to PASS, and re-baseline the
three UOH entries [to A=11]." That text was a pre-measurement estimate, not
the implementation. Measurement resolved three details differently:
- **Mechanism:** an **accumulator-side restore** (immediately re-adding the
  revoked amount inside `char_move()`, gated), not a finalize-side latched
  quantity. A finalize-side latch was considered and rejected during Step
  2b's design pass: it double-counts on the `q-uoh-chain-retrigger` case —
  the live latch would hold BOTH taps' revoked amounts (+2) while the
  §13.9.4 anchor already excludes tap-2, and fixing that would need a
  second, anchor-coupled snapshot of the latch (more state, more gates).
  The accumulator-side restore has no such problem: the anchor
  automatically inherits it for free, zero overlay changes.
- **Eleven shape-(b) flips, not ten:** `ryu-crlk-block` is census-resolved
  shape (b) this session (rev=1 at GT=2089) — Step 1 had left it untraced,
  not classified as shape (a) as option 2's phrasing implied.
- **Five re-baselined UOH entries, not three:** the two HIT variants,
  `q-uoh-samef-hit`/`q-uoh-1f-hit`, also assert A and are re-baselined
  alongside the two BLOCK variants and `q-uoh-chain-retrigger`.
