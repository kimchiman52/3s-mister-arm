# MiSTer Ralph Working Brief

## Purpose

- Use this file as the default working set for the active Ralph queue on `mister-dev`.
- Load this before `artifacts/mister-port/living-findings.md` unless you need exact historical closeout detail.

## Metadata

- Last updated: `2026-03-23`
- Active branch: `super-fidelity-ralph-loop`
- Active queue: keep Loop 183's Yun-only `frame-skip` validation as the new burst-fidelity baseline, then extend or retime the trusted Yun slowdown window beyond the provisional `82` frames on the same render-only path, while leaving Ken/Chun matrix harness repair as a separate setup queue
- Default loop type: `workload-fidelity`
- Deciding lane: `yun-sa3-repeat-pressure` judged on first `8`, first `60`, and the trusted `82`-frame slowdown proxy
- Primary guard lane: `gameplay-idle`

## Current Goal

- Stabilize super-activation performance on MiSTer by reducing burst-only visual workload across the real slowdown window before spending more loop budget on helper-local alpha branch or codegen relands.
- Loop 181 kept the next stronger `minimal` survivor cut. On the current same-build direct/native comparator, Yun improved materially again (`18.4086 -> 21.4300 FPS` overall, `17.8616 -> 21.1999` first `8`, `16.0917 -> 19.0680` first `60`, `16.6563 -> 19.7434` first `82`) while `software_frame_fast_non_integer_pixels` fell `187541.84 -> 150210.59`; Q SA1 stayed slightly positive (`20.3827 -> 20.6169 FPS`) and `gameplay-idle` stayed effectively flat (`63.0761 -> 62.9730 FPS`)
- Loop 182 rejected the next cadence-only survivor cut. Lowering `minimal` again from one-in-four to one-in-five improved same-build Yun versus `full` (`18.2874 -> 21.5878 FPS` overall, `17.4398 -> 21.2644` first `8`, `15.9636 -> 19.2180` first `60`, `16.5149 -> 19.9153` first `82`), but that only beat kept Loop 181 `minimal` by noise-level deltas (`+0.1578 / +0.0645 / +0.1500 / +0.1719 FPS` overall / first `8` / first `60` / first `82`) and slightly regressed Q SA1 (`20.5462 -> 20.3877 FPS` overall). Treat simple cadence-only thinning as saturated for now.
- Loop 183 kept the already-implemented Yun-only `frame-skip` mode once the capture regime was corrected to include the inactive-to-active trigger. On the same-build trigger-inclusive comparator (`game-input-active`, zero warmup, trigger at frame `296`), Yun moved from `34.7786 FPS` on `full` and `35.5341` on `minimal` to `41.0140` on `frame-skip`; trigger-relative windows improved from `19.2751 / 18.1013 / 18.3406 FPS` (`first8 / first60 / first82`) on `full` and `21.9657 / 18.8698 / 19.5025` on `minimal` to `34.9757 / 30.1135 / 31.0109` on `frame-skip`, with zero readback and `41/41` scheduled skips actually applied
- The absolute full-mode baseline on the live device is lower than the earlier Loop 180 artifact family, so Loop 181 is judged on same-build `full` versus `minimal` deltas rather than on stale cross-loop absolute FPS. The route truth still matches the trusted direct/native software-frame path.
- Current working assumption from manual observation still stands: Yun SA3 slowdown lasts well beyond the first `8` frames, and Loop 183 now quantifies the current gap more concretely. The corrected deciding capture shows `p1_super_art_active_frames_total = 124` while the current frame-skip gate only applies through active frame `82`, so the next runtime loop should target that remaining active tail instead of reopening cadence-only thinning.
- Immediate queue gate now shifts away from validating `frame-skip` and onto extending or retiming the trusted Yun slowdown window on the same render-only reuse path; repair or replace the invalid Ken/Chun super harness lanes before treating them as meaningful matrix gates.

## Trusted Baselines

- `loop174-yun-onset-cluster-alpha-r3`
- Why it is trusted: low-distortion onset capture that stayed direct/native and proved the hot onset cohort is binary-alpha-heavy.
- `loop146-remy-rerank-r2`
- Why it is trusted: proves Remy-left is a separate exact/direct compare-dirty queue, not the same native Yun problem.
- `gameplay-idle`
- Why it is trusted: simplest non-super gameplay guard for catching broad regressions quickly.

## Current Queue

- Extend or retime the trusted Yun frame-skip window beyond `82` active frames
- Why it is still live: Loop 183 proved the already-implemented Yun-only `frame-skip` mode is a real keep, but the deciding capture still shows `124` active Yun super frames while reuse only arms for `41` alternating frames through active frame `82`. The cleanest next lever is to cover more of that remaining active tail without widening beyond the same MiSTer-only burst-visual scope.
- Ken/Chun super-harness repair
- Why it is still live: both `ken-sa3-repeat*` and `chunli-sa2-repeat*` stayed super-ready with zero super entries/active frames, which means the broader matrix is partially blocked by harness invalidity rather than by meaningful runtime evidence.
- Broader burst-only cohort or super-specific thinning follow-up
- Why it is still live: if a longer Yun frame-skip window still leaves too much slowdown, the remaining runtime path is still the same user-approved burst-fidelity family, but it should broaden by cohort/rule shape rather than by blindly tightening the same cadence dial again.
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
- Raw `game` wait / zero-warmup repeat-super capture starts for frame-skip validation
- Why it is banned or demoted: Loop 183's first probe showed that starting at plain `game` with zero warmup was too early and never reached the first scripted super within the sample. Use `game-input-active` with zero warmup for trigger-inclusive repeat-super frame-skip validation instead.
- In-band repeat-pressure family-plus-render-subphase collectors
- Why it is banned or demoted: Loop `176` more than doubled render cost on the deciding comparator and is not decision-grade.
- Narrower `ix 80` or palette-split native Yun retries
- Why it is banned or demoted: the March rerank already closed that queue without new separation evidence.

## Top Candidates

- Extend or retime the trusted Yun frame-skip window past active frame `82`
- Lever class: `workload-fidelity`
- Expected upside: Loop 183 proved `frame-skip` is the first stronger post-cadence keep on the active queue, and the deciding capture now shows exactly where the remaining uncovered slowdown lives: Yun stays active for `124` frames while the current reuse gate stops at `82`
- Main risk: widening the reuse window too aggressively could overrun the real slowdown tail or create visual artifacts outside the intended burst scope, so the next loop still needs the same direct-route checks plus Q/idle guards
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
- Existing diff under test: the kept Loop 183 frame-skip baseline plus the existing Yun-only trigger/window plumbing in `src/port/sdl/sdl_app.c`
- One scoped change: extend or retime the trusted Yun frame-skip window on `yun-sa3-repeat-pressure` so the same render-only reuse path covers more of the still-slow active tail beyond the current provisional `82` active-frame cap; keep Q SA1 and `gameplay-idle` as guards; do not repair the Ken/Chun harness in the same loop
- Stop immediately if: the candidate widens beyond burst-scoped MiSTer-only render degradation, changes gameplay timing/logic/determinism, or tries to relitigate alpha-branch/layout or dark-render memo ideas without genuinely new evidence
- Capture plan: keep `yun-sa3-repeat-pressure` as the deciding lane, start the repeat-super captures at `game-input-active` with zero warmup so the trigger lands inside the sample, and continue judging first `8`, first `60`, trusted `82`, plus the newly exposed post-`82` active tail; rerun Q SA1 as the secondary live guard; do not trust Ken/Chun matrix outcomes again until the harness proves `p1_super_active_starts > 0`
- Keep if: the longer or retimed reuse window materially improves the remaining Yun post-`82` tail without broad Q/idle regression or route drift
- Reject if: the longer window fails to move the remaining active tail meaningfully, widens reuse beyond the real slowdown, or creates route/accounting drift that cannot be explained cleanly as intentional previous-frame reuse
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
- Keep: Loop 183 Yun-only frame-skip validation
- Why it mattered: it corrected the repeat-super capture timing so the trigger landed inside the sample, proved the already-implemented Yun-only `frame-skip` mode is a real keep on the same-build deciding lane, and exposed the next concrete queue edge: the current reuse gate stops at active frame `82` while the captured Yun super stays active for `124` frames

## Archive Pointers

- Full history: `artifacts/mister-port/living-findings.md`
- Active checklist: `artifacts/mister-port/stock-image-software-frame-loop-series/todo.md`
- Deep research memo: `docs/agent-memory/mister-perf-deep-research-2026-03-21.md`
- Process doc: `docs/agent-memory/mister-ralph-loop-v2.md`
