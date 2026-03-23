# MiSTer Ralph Working Brief

## Purpose

- Use this file as the default working set for the active Ralph queue on `mister-dev`.
- Load this before `artifacts/mister-port/living-findings.md` unless you need exact historical closeout detail.

## Metadata

- Last updated: `2026-03-23`
- Active branch: `super-fidelity-ralph-loop`
- Active queue: close Loop 179 as a docs-only reject/pivot, then stay on the MiSTer-only super-activation burst-fidelity path with a stronger whole-window Yun SA3 workload cut plus a small harness-repair follow-up for Ken/Chun matrix lanes
- Default loop type: `workload-fidelity`
- Deciding lane: `yun-sa3-repeat-pressure` judged on first `8`, first `60`, and the trusted `82`-frame slowdown proxy
- Primary guard lane: `gameplay-idle`

## Current Goal

- Stabilize super-activation performance on MiSTer by reducing burst-only visual workload across the real slowdown window before spending more loop budget on helper-local alpha branch or codegen relands.
- Loop 179 closed the dark-render audit and the first `full|simplified|minimal` validation pass. The deciding Yun/Q captures stayed direct/native but improved only modestly, so the next successful loop must deliver a larger whole-window Yun SA3 gain than the current first-pass thinning.
- Current working assumption from manual observation still stands: Yun SA3 slowdown likely lasts roughly `80+` frames, not just the first `8`, and any next runtime loop should continue to judge candidates on that broader window.
- Immediate queue gate now shifts from memo audit to execution quality: repair or replace the invalid Ken/Chun super harness lanes before treating them as meaningful matrix gates, while keeping trusted Yun SA3 as the deciding lane.

## Trusted Baselines

- `loop174-yun-onset-cluster-alpha-r3`
- Why it is trusted: low-distortion onset capture that stayed direct/native and proved the hot onset cohort is binary-alpha-heavy.
- `loop146-remy-rerank-r2`
- Why it is trusted: proves Remy-left is a separate exact/direct compare-dirty queue, not the same native Yun problem.
- `gameplay-idle`
- Why it is trusted: simplest non-super gameplay guard for catching broad regressions quickly.

## Current Queue

- Stronger super-effect-quality follow-up
- Why it is still live: Loop 179 proved the first-pass `simplified` / `minimal` cuts are too weak on trusted Yun whole-window slowdown and only modestly move the Q guard lane, so the next runtime step should stay on stronger burst-only thinning rather than reopening unrelated helper/codegen ideas.
- Ken/Chun super-harness repair
- Why it is still live: both `ken-sa3-repeat*` and `chunli-sa2-repeat*` stayed super-ready with zero super entries/active frames, which means the broader matrix is partially blocked by harness invalidity rather than by meaningful runtime evidence.
- Yun-only `frame-skip` follow-up
- Why it is still live: it remains an already-implemented higher-impact burst-fidelity lever if stronger thinning either stalls or review says the next simplest bounded experiment is to validate the cadence-sensitive mode already in tree.
- Native Yun deep measurement
- Why it is still live: PMU or other lower-distortion external evidence could still clarify whether the remaining full-window slowdown is memory-latency, setup, or row-walk dominated, but it is now secondary to the stronger burst-fidelity rerank.

## Banned Or Demoted Families

- Alpha-branch-layout relands in `software_frame_non_integer.c`
- Why it is banned or demoted: Loops `177` and `178` failed in opposite ways and burned the immediate branch-layout/codegen family.
- First-pass `super-effect-quality = simplified|minimal` as a terminal answer
- Why it is banned or demoted: Loop 179 validated those first-pass modes on-device and showed only modest whole-window wins on Yun/Q, far short of a keep-worthy result.
- In-band repeat-pressure family-plus-render-subphase collectors
- Why it is banned or demoted: Loop `176` more than doubled render cost on the deciding comparator and is not decision-grade.
- Narrower `ix 80` or palette-split native Yun retries
- Why it is banned or demoted: the March rerank already closed that queue without new separation evidence.

## Top Candidates

- Strengthen burst-only thinning on trusted Yun SA3 after Loop 179's weak first-pass result
- Lever class: `workload-fidelity`
- Expected upside: larger whole-window gains are still most likely to come from the same user-approved burst-fidelity path, with lower risk than reopening another helper-local or codegen family
- Main risk: stronger thinning needs careful guardrails so it stays burst-scoped and does not over-thin non-super or non-target families
- Repair or replace `ken-sa3-repeat*` / `chunli-sa2-repeat*` as decision-grade super-matrix lanes
- Lever class: `measurement`
- Expected upside: restores the broader matrix so future fidelity wins are not judged on Yun/Q alone or on zero-activation false guards
- Main risk: the harness fix could take a loop without producing a player-facing runtime win, so it should stay narrow and should not crowd out the stronger Yun runtime rerank
- Research and plan expansion from Yun-only gating to all supers across the full roster once a stronger mode gets trusted Yun SA3 close to stable speed
- Lever class: `measurement`
- Expected upside: turns a single-lane super-fidelity win into a roadmap for broader player-visible benefit instead of stopping at Yun-only success
- Main risk: expanding too early without a stable Yun proof could spread the queue too wide, so this must stay sequenced after the first clear Yun keep
- `frame-skip` follow-up on trusted Yun SA3 only, after the first automated matrix closes
- Lever class: `workload-fidelity`
- Expected upside: larger full-window relief remains possible if the current raster-thinning modes are still too weak
- Main risk: it reuses the previous rendered frame on alternating trusted Yun SA3 burst frames, so it is cadence-sensitive and intentionally excluded from Loop 179's first automated sweep
- External or off-path PMU-backed measurement on the native Yun super lane
- Lever class: `measurement`
- Expected upside: could finally distinguish setup cost from persistent row-walk cost and establish the true slowdown span before another runtime specialization
- Main risk: tooling availability on the device may block it or make correlation awkward

## Measurement Rules

- Default comparator: `tools/mister/perf-sampler.sh --perf-basic`
- Accepted richer modes: only richer modes that already proved low distortion on the deciding lane should guide runtime decisions
- Distortion budget note: reject any collector whose own bookkeeping becomes a dominant reported phase or that materially perturbs the deciding lane relative to trusted `--perf-basic`
- Required route checks: direct/native route, zero unexpected fallback/readback, and stable scene identity

## Statistics Rules

- Baseline rerun count for close calls: `2`
- Candidate rerun count for close calls: `2`
- Decision metric: median frame time plus worst-frame sanity check on the deciding lane
- Known variance note: do not treat small single-run improvements as real when the queue is already in the low-single-digit regime

## Review Rules

- Preferred review path: bounded independent diff review on the authored change
- Fallback review path: manual diff review if automated repo-wide review stalls
- Extra review focus for this queue: selector scope, burst-window gating, and non-super regression risk

## Next Loop Contract

- Loop type: `workload-fidelity`
- Existing diff under test: the already-committed MiSTer-only super-fidelity branch, now closed as "first-pass cuts too weak" after Loop 179 validation
- One scoped change: either implement one stronger burst-only thinning step for trusted Yun SA3 or, if that work is not ready, spend one bounded setup loop fixing the invalid Ken/Chun repeat harness so the broader matrix becomes trustworthy again
- Stop immediately if: the candidate widens beyond burst-scoped MiSTer-only render degradation, changes gameplay timing/logic/determinism, or tries to relitigate alpha-branch/layout or dark-render memo ideas without genuinely new evidence
- Capture plan: keep `yun-sa3-repeat-pressure` as the deciding lane and continue judging first `8`, first `60`, and trusted `82`; rerun Q SA1 as the secondary live guard; do not trust Ken/Chun matrix outcomes again until the harness proves `p1_super_active_starts > 0`
- Keep if: the next stronger cut moves trusted Yun SA3 materially beyond Loop 179's roughly `+0.6 to +0.8 FPS` whole-window gain without broad idle regression, or the harness loop restores decision-grade non-Yun super starts
- Reject if: the stronger cut remains in the same low-single-digit improvement regime or the harness work still leaves zero-activation super lanes
- Post-Yun success note: if a later super-fidelity loop gets trusted Yun SA3 close enough to stable speed, do not stop at the Yun-only keep. Open a bounded research-and-planning loop next to map how the same burst-fidelity surface could expand to all supers for all characters, including gating strategy, likely grouping, measurement matrix, and rollout order before implementation broadens.

## Recent Decisive Evidence

- Keep: `loop174-yun-onset-cluster-alpha-r3`
- Why it mattered: it narrowed the hot onset cohort to a binary-alpha problem with lower measurement drag than the older collectors
- Reject: Loop `176` repeat-pressure render-subphase collector
- Why it mattered: it proved the current in-band collector family is too distortive to guide another runtime decision
- Pivot: Loop 179 first-pass burst-fidelity validation
- Why it mattered: it closed the dark-render audit as a docs/process correction, proved the current `simplified` / `minimal` modes are too weak on the deciding whole-window Yun/Q lanes, and exposed the remaining Ken/Chun harness invalidity

## Archive Pointers

- Full history: `artifacts/mister-port/living-findings.md`
- Active checklist: `artifacts/mister-port/stock-image-software-frame-loop-series/todo.md`
- Deep research memo: `docs/agent-memory/mister-perf-deep-research-2026-03-21.md`
- Process doc: `docs/agent-memory/mister-ralph-loop-v2.md`
