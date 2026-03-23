# MiSTer Ralph Working Brief

## Purpose

- Use this file as the default working set for the active Ralph queue on `mister-dev`.
- Load this before `artifacts/mister-port/living-findings.md` unless you need exact historical closeout detail.

## Metadata

- Last updated: `2026-03-23`
- Active branch: `super-fidelity-ralph-loop`
- Active queue: close Loop 182 as a docs-only rejection, then validate the already-implemented Yun-only `frame-skip` mode as the next bounded whole-window burst-fidelity experiment, while leaving Ken/Chun matrix harness repair as a separate setup queue
- Default loop type: `workload-fidelity`
- Deciding lane: `yun-sa3-repeat-pressure` judged on first `8`, first `60`, and the trusted `82`-frame slowdown proxy
- Primary guard lane: `gameplay-idle`

## Current Goal

- Stabilize super-activation performance on MiSTer by reducing burst-only visual workload across the real slowdown window before spending more loop budget on helper-local alpha branch or codegen relands.
- Loop 181 kept the next stronger `minimal` survivor cut. On the current same-build direct/native comparator, Yun improved materially again (`18.4086 -> 21.4300 FPS` overall, `17.8616 -> 21.1999` first `8`, `16.0917 -> 19.0680` first `60`, `16.6563 -> 19.7434` first `82`) while `software_frame_fast_non_integer_pixels` fell `187541.84 -> 150210.59`; Q SA1 stayed slightly positive (`20.3827 -> 20.6169 FPS`) and `gameplay-idle` stayed effectively flat (`63.0761 -> 62.9730 FPS`)
- Loop 182 rejected the next cadence-only survivor cut. Lowering `minimal` again from one-in-four to one-in-five improved same-build Yun versus `full` (`18.2874 -> 21.5878 FPS` overall, `17.4398 -> 21.2644` first `8`, `15.9636 -> 19.2180` first `60`, `16.5149 -> 19.9153` first `82`), but that only beat kept Loop 181 `minimal` by noise-level deltas (`+0.1578 / +0.0645 / +0.1500 / +0.1719 FPS` overall / first `8` / first `60` / first `82`) and slightly regressed Q SA1 (`20.5462 -> 20.3877 FPS` overall). Treat simple cadence-only thinning as saturated for now.
- The absolute full-mode baseline on the live device is lower than the earlier Loop 180 artifact family, so Loop 181 is judged on same-build `full` versus `minimal` deltas rather than on stale cross-loop absolute FPS. The route truth still matches the trusted direct/native software-frame path.
- Current working assumption from manual observation still stands: Yun SA3 slowdown likely lasts roughly `80+` frames, not just the first `8`, and any next runtime loop should continue to judge candidates on that broader window.
- Immediate queue gate now shifts away from cadence-only thinning and onto the already-implemented Yun-only `frame-skip` validation; repair or replace the invalid Ken/Chun super harness lanes before treating them as meaningful matrix gates.

## Trusted Baselines

- `loop174-yun-onset-cluster-alpha-r3`
- Why it is trusted: low-distortion onset capture that stayed direct/native and proved the hot onset cohort is binary-alpha-heavy.
- `loop146-remy-rerank-r2`
- Why it is trusted: proves Remy-left is a separate exact/direct compare-dirty queue, not the same native Yun problem.
- `gameplay-idle`
- Why it is trusted: simplest non-super gameplay guard for catching broad regressions quickly.

## Current Queue

- Yun-only `frame-skip` follow-up
- Why it is still live: Loop 182 says simple cadence-only thinning has probably exhausted its safe headroom, but the already-implemented Yun-only `frame-skip` mode is still the next stronger bounded burst-fidelity lever on the same trusted whole-window lane.
- Ken/Chun super-harness repair
- Why it is still live: both `ken-sa3-repeat*` and `chunli-sa2-repeat*` stayed super-ready with zero super entries/active frames, which means the broader matrix is partially blocked by harness invalidity rather than by meaningful runtime evidence.
- Broader burst-only cohort or super-specific thinning follow-up
- Why it is still live: if `frame-skip` also stalls, the remaining runtime path is still the same user-approved burst-fidelity family, but it should broaden by cohort/rule shape rather than by blindly tightening the same cadence dial again.
- Native Yun deep measurement
- Why it is still live: PMU or other lower-distortion external evidence could still clarify whether the remaining full-window slowdown is memory-latency, setup, or row-walk dominated, but it is now secondary to the stronger burst-fidelity rerank.

## Banned Or Demoted Families

- Alpha-branch-layout relands in `software_frame_non_integer.c`
- Why it is banned or demoted: Loops `177` and `178` failed in opposite ways and burned the immediate branch-layout/codegen family.
- First-pass `super-effect-quality = simplified|minimal` as a terminal answer
- Why it is banned or demoted: Loop 179 validated those first-pass modes on-device and showed only modest whole-window wins on Yun/Q, far short of a keep-worthy result.
- Loop 180 stronger `minimal` cadence change as a terminal answer
- Why it is banned or demoted: it is a real keep, but it still leaves trusted Yun far from stable speed and Q essentially flat, so it should be treated as the next baseline rather than as the end of the queue.
- Loop 181 stronger `minimal` one-in-four cadence as a terminal answer
- Why it is banned or demoted: it is another real keep, but trusted Yun still remains well below stable speed on the current same-build comparator and the queue has not yet proved that simple thinning has exhausted its safe headroom.
- Loop 182 stronger `minimal` one-in-five cadence retry
- Why it is banned or demoted: it only improved kept Loop 181 `minimal` by noise-level Yun deltas and slightly regressed Q SA1, so do not spend another loop on the same cadence-only dial without genuinely new evidence.
- In-band repeat-pressure family-plus-render-subphase collectors
- Why it is banned or demoted: Loop `176` more than doubled render cost on the deciding comparator and is not decision-grade.
- Narrower `ix 80` or palette-split native Yun retries
- Why it is banned or demoted: the March rerank already closed that queue without new separation evidence.

## Top Candidates

- Validate the already-implemented Yun-only `frame-skip` mode on the trusted whole-window SA3 lane
- Lever class: `workload-fidelity`
- Expected upside: it is the next stronger burst-fidelity lever already in tree, so it can answer quickly whether the active MiSTer-only visual-reduction path still has enough headroom to matter after cadence-only thinning stalled.
- Main risk: it is cadence-sensitive because it reuses the previous rendered frame on alternating trusted burst frames, so it needs the same whole-window Yun judgment plus idle/route guards.
- Repair or replace `ken-sa3-repeat*` / `chunli-sa2-repeat*` as decision-grade super-matrix lanes
- Lever class: `measurement`
- Expected upside: restores the broader matrix so future fidelity wins are not judged on Yun/Q alone or on zero-activation false guards
- Main risk: the harness fix could take a loop without producing a player-facing runtime win, so it should stay narrow and should not crowd out the stronger Yun runtime rerank
- Broaden the trusted burst-only cohort or add super-specific thinning after `frame-skip` if the current Yun-only mode is still too weak
- Lever class: `workload-fidelity`
- Expected upside: keeps the queue on the same user-approved render-only super-fidelity track without reopening unrelated helper/codegen families
- Main risk: broadening the cohort or adding super-specific rules raises selector-scope risk and should wait until the simpler in-tree `frame-skip` lever is either kept or rejected
- Research and plan expansion from Yun-only gating to all supers across the full roster once a stronger mode gets trusted Yun SA3 close to stable speed
- Lever class: `measurement`
- Expected upside: turns a single-lane super-fidelity win into a roadmap for broader player-visible benefit instead of stopping at Yun-only success
- Main risk: expanding too early without a stable Yun proof could spread the queue too wide, so this must stay sequenced after the first clear Yun keep
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
- Existing diff under test: the kept Loop 181 stronger-`minimal` one-in-four baseline plus the already-implemented Yun-only `frame-skip` mode that is still dormant behind config/runtime selection
- One scoped change: validate the already-implemented Yun-only `frame-skip` mode on trusted `yun-sa3-repeat-pressure`; keep Q SA1 and `gameplay-idle` as guards; do not broaden the runtime gate or repair the Ken/Chun harness in the same loop
- Stop immediately if: the candidate widens beyond burst-scoped MiSTer-only render degradation, changes gameplay timing/logic/determinism, or tries to relitigate alpha-branch/layout or dark-render memo ideas without genuinely new evidence
- Capture plan: keep `yun-sa3-repeat-pressure` as the deciding lane and continue judging first `8`, first `60`, and trusted `82`; rerun Q SA1 as the secondary live guard; do not trust Ken/Chun matrix outcomes again until the harness proves `p1_super_active_starts > 0`
- Keep if: `frame-skip` moves trusted Yun SA3 materially beyond kept Loop 181 / rejected Loop 182 `minimal` on the whole-window comparator without broad idle regression or route drift
- Reject if: `frame-skip` fails to beat kept Loop 181 meaningfully, introduces visible route/guard drift, or still leaves Yun far enough below stable speed that the cadence-sensitive reuse trade is not worth broadening
- Post-Yun success note: if a later super-fidelity loop gets trusted Yun SA3 close enough to stable speed, do not stop at the Yun-only keep. Open a bounded research-and-planning loop next to map how the same burst-fidelity surface could expand to all supers for all characters, including gating strategy, likely grouping, measurement matrix, and rollout order before implementation broadens.

## Recent Decisive Evidence

- Keep: `loop174-yun-onset-cluster-alpha-r3`
- Why it mattered: it narrowed the hot onset cohort to a binary-alpha problem with lower measurement drag than the older collectors
- Reject: Loop `176` repeat-pressure render-subphase collector
- Why it mattered: it proved the current in-band collector family is too distortive to guide another runtime decision
- Pivot: Loop 179 first-pass burst-fidelity validation
- Why it mattered: it closed the dark-render audit as a docs/process correction, proved the first-pass `simplified` / `minimal` modes were too weak on the deciding whole-window Yun/Q lanes, and exposed the remaining Ken/Chun harness invalidity
- Keep: Loop 180 stronger `minimal` cadence change
- Why it mattered: it delivered the first keep-worthy whole-window Yun SA3 gain on the active super-fidelity queue while preserving the direct/native route and only slightly slipping the idle guard
- Keep: Loop 181 stronger `minimal` one-in-four cadence change
- Why it mattered: it proved that simple post-sort survivor cuts still have meaningful same-build whole-window Yun headroom after Loop 180, with Q still slightly positive and `gameplay-idle` still within noise on the trusted route
- Reject: Loop 182 stronger `minimal` one-in-five cadence retry
- Why it mattered: it showed the same cadence-only dial has now flattened into noise-level Yun gains with a slight Q SA1 regression, so the next runtime rerank should move to `frame-skip` rather than another stronger cadence keep

## Archive Pointers

- Full history: `artifacts/mister-port/living-findings.md`
- Active checklist: `artifacts/mister-port/stock-image-software-frame-loop-series/todo.md`
- Deep research memo: `docs/agent-memory/mister-perf-deep-research-2026-03-21.md`
- Process doc: `docs/agent-memory/mister-ralph-loop-v2.md`
