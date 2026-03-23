# MiSTer Ralph Working Brief

## Purpose

- Use this file as the default working set for the active Ralph queue on `mister-dev`.
- Load this before `artifacts/mister-port/living-findings.md` unless you need exact historical closeout detail.

## Metadata

- Last updated: `2026-03-23`
- Active branch: `super-fidelity-ralph-loop`
- Active queue: keep Loop 184's full-window Yun-only `frame-skip` extension as the burst-fidelity baseline, close Loop 185's opaque-family-only frame-skip thinning as rejected evidence, then stay on the same user-approved render-only path with a broader trusted burst-only cohort follow-up while restoring decision-grade Q/Ken/Chun matrix validation instead of leaving Ken/Chun off to the side
- Default loop type: `workload-fidelity`
- Deciding lane: `yun-sa3-repeat-pressure` judged on first `8`, first `60`, first `82`, the post-`82` active tail, and the full trusted active window
- Primary guard lane: `gameplay-idle`

## Current Goal

- Stabilize super-activation performance on MiSTer by reducing burst-only visual workload across the real slowdown window before spending more loop budget on helper-local alpha branch or codegen relands.
- Loop 181 kept the next stronger `minimal` survivor cut. On the current same-build direct/native comparator, Yun improved materially again (`18.4086 -> 21.4300 FPS` overall, `17.8616 -> 21.1999` first `8`, `16.0917 -> 19.0680` first `60`, `16.6563 -> 19.7434` first `82`) while `software_frame_fast_non_integer_pixels` fell `187541.84 -> 150210.59`; Q SA1 stayed slightly positive (`20.3827 -> 20.6169 FPS`) and `gameplay-idle` stayed effectively flat (`63.0761 -> 62.9730 FPS`)
- Loop 182 rejected the next cadence-only survivor cut. Lowering `minimal` again from one-in-four to one-in-five improved same-build Yun versus `full` (`18.2874 -> 21.5878 FPS` overall, `17.4398 -> 21.2644` first `8`, `15.9636 -> 19.2180` first `60`, `16.5149 -> 19.9153` first `82`), but that only beat kept Loop 181 `minimal` by noise-level deltas (`+0.1578 / +0.0645 / +0.1500 / +0.1719 FPS` overall / first `8` / first `60` / first `82`) and slightly regressed Q SA1 (`20.5462 -> 20.3877 FPS` overall). Treat simple cadence-only thinning as saturated for now.
- Loop 183 kept the already-implemented Yun-only `frame-skip` mode once the capture regime was corrected to include the inactive-to-active trigger. On the same-build trigger-inclusive comparator (`game-input-active`, zero warmup, trigger at frame `296`), Yun moved from `34.7786 FPS` on `full` and `35.5341` on `minimal` to `41.0140` on `frame-skip`; trigger-relative windows improved from `19.2751 / 18.1013 / 18.3406 FPS` (`first8 / first60 / first82`) on `full` and `21.9657 / 18.8698 / 19.5025` on `minimal` to `34.9757 / 30.1135 / 31.0109` on `frame-skip`, with zero readback and `41/41` scheduled skips actually applied
- Loop 184 kept the longer full-window Yun-only `frame-skip` cap. Extending the trusted reuse window from `82` to the recovered `124` active frames, while stopping it immediately once the trusted Yun active state clears, improved same-build Yun from `46.9374 -> 50.4138 FPS` overall, `39.9817 -> 50.9294` first `8`, `38.2801 -> 49.8172` first `60`, `39.5247 -> 51.4887` first `82`, `44.2897 -> 53.6886` on the previously uncovered post-`82` tail, and `41.0195 -> 52.2134` across the full active window; scheduled/applied skips rose from `41/41` to `62/62`
- Loop 185 rejected the next family-specific cadence split on top of that keep. Thinning only the three proven opaque families harder on rendered `frame-skip` ticks still improved same-build Yun versus `full`, but it did not beat kept Loop 184 `frame-skip` on the deciding full active window (`52.3884 -> 52.1061 FPS`) or the key mid-window spans (`49.9367 -> 49.2362` first `60`, `51.5937 -> 50.9965` first `82`), so that exact six-family opaque-only cadence reland is now closed evidence rather than the next baseline
- The absolute full-mode baseline on the live device is lower than the earlier Loop 180 artifact family, so Loop 181 is judged on same-build `full` versus `minimal` deltas rather than on stale cross-loop absolute FPS. The route truth still matches the trusted direct/native software-frame path.
- Loop 184 closes the window-length question on the trusted repeat-pressure lane: the longer `124`-frame cap materially improved the previously untouched active tail without broad guard regression, so another cap-length tweak is no longer the best next runtime bet.
- Trusted Yun is still not close enough to stable `60 FPS` to stop at this keep. After Loop 185's rejection, the next runtime pass should stay on the same burst-fidelity family but move away from the exact opaque-family-only cadence split and instead test a broader trusted burst-only cohort on rendered `frame-skip` ticks, while restoring decision-grade Ken/Chun matrix coverage instead of deferring it indefinitely.

## Trusted Baselines

- `loop174-yun-onset-cluster-alpha-r3`
- Why it is trusted: low-distortion onset capture that stayed direct/native and proved the hot onset cohort is binary-alpha-heavy.
- `loop146-remy-rerank-r2`
- Why it is trusted: proves Remy-left is a separate exact/direct compare-dirty queue, not the same native Yun problem.
- `gameplay-idle`
- Why it is trusted: simplest non-super gameplay guard for catching broad regressions quickly.

## Current Queue

- Broader trusted burst-only cohort on top of the kept full-window `frame-skip` baseline
- Why it is still live: Loop 184 already covers the full trusted `124`-frame Yun active window and materially improves the formerly uncovered tail, but the deciding lane still sits around `52 FPS` across the full active span instead of stable `60`. Loop 185 also showed that a narrower opaque-family-only cadence split on the same six-family gate does not beat the kept baseline, so the next runtime lever should broaden the burst-only cohort rather than reland that exact family split.
- Restore decision-grade Ken SA3 / Chun-Li SA2 matrix coverage
- Why it is still live: the broader super-fidelity loop contract still requires Q/Ken/Chun validation rather than Yun-only keeps. Both `ken-sa3-repeat*` and `chunli-sa2-repeat*` stayed super-ready with zero super entries/active frames, so the next follow-up must repair or replace those lanes before a later burst-fidelity reland can count as fully validated.
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
- Loop 184 longer `124`-frame cap as a terminal answer
- Why it is banned or demoted: it is a real keep, but trusted Yun still remains around `52.2134 FPS` across the full active window, so the next runtime pass should broaden the same burst-fidelity family rather than spending another loop on cap length alone.
- Loop 185 opaque-family-only frame-skip cadence split on the current six-family gate
- Why it is banned or demoted: it improved Yun versus same-build `full`, but it did not beat kept Loop 184 `frame-skip` on the deciding full active window or the main mid-window spans, so do not retry that exact opaque-only cadence reland unchanged now.
- Raw `game` wait / zero-warmup repeat-super capture starts for frame-skip validation
- Why it is banned or demoted: Loop 183's first probe showed that starting at plain `game` with zero warmup was too early and never reached the first scripted super within the sample. Use `game-input-active` with zero warmup for trigger-inclusive repeat-super frame-skip validation instead.
- In-band repeat-pressure family-plus-render-subphase collectors
- Why it is banned or demoted: Loop `176` more than doubled render cost on the deciding comparator and is not decision-grade.
- Narrower `ix 80` or palette-split native Yun retries
- Why it is banned or demoted: the March rerank already closed that queue without new separation evidence.

## Top Candidates

- Broader trusted burst-only cohort on top of the kept full-window `frame-skip` baseline
- Lever class: `workload-fidelity`
- Expected upside: Loop 184 already converted the full trusted active window into a stronger Yun keep, and Loop 185 closed the narrower opaque-only cadence split. The next remaining headroom is still on the same user-approved burst-fidelity path, but it now points at a broader rendered-frame cohort rather than another retry of the current six-family split
- Main risk: broadening the cohort can leak beyond the intended burst-visual scope, so the next loop still needs the same direct-route checks plus the broader Q/Ken/Chun matrix guards
- Repair or replace `ken-sa3-repeat*` / `chunli-sa2-repeat*` as decision-grade super-matrix lanes
- Lever class: `measurement`
- Expected upside: restores the broader matrix so future fidelity wins are not judged on Yun/Q alone or on zero-activation false guards
- Main risk: the harness fix could take a loop without producing a player-facing runtime win, but it now has to be treated as part of the active super-fidelity validation contract rather than as indefinitely deferrable side work
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
- Existing diff under test: the kept Loop 184 full-window `frame-skip` baseline plus the existing Yun-only trigger/window plumbing in `src/port/sdl/sdl_app.c`
- One scoped change: stay on the same render-only super-fidelity path and test one bounded broader-cohort follow-up on `yun-sa3-repeat-pressure` beyond the now-closed window-length and opaque-only-cadence levers, while restoring decision-grade Q/Ken/Chun matrix validation as part of the same follow-up contract
- Stop immediately if: the candidate widens beyond burst-scoped MiSTer-only render degradation, changes gameplay timing/logic/determinism, or tries to relitigate alpha-branch/layout or dark-render memo ideas without genuinely new evidence
- Capture plan: keep `yun-sa3-repeat-pressure` as the deciding lane, start the repeat-super captures at `game-input-active` with zero warmup so the trigger lands inside the sample, and continue judging first `8`, first `60`, first `82`, the post-`82` active tail, and the full active window; keep Q SA1 live and repair or replace the Ken/Chun lanes until they produce decision-grade `p1_super_active_starts > 0` coverage instead of another zero-activation false guard
- Keep if: the broader-cohort follow-up materially improves the full trusted active window beyond kept Loop 184 while preserving the same direct/native route and without broad Q/idle regression
- Reject if: the stronger follow-up fails to move trusted Yun materially, widens degradation beyond the intended burst scope, or creates route/accounting drift that cannot be explained cleanly as intentional previous-frame reuse
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
- Keep: Loop 184 full-window Yun frame-skip extension
- Why it mattered: it closed the window-length question on the trusted repeat-pressure lane by extending the cap to the recovered `124` active frames, lifting the previously uncovered post-`82` tail from `44.2897` to `53.6886 FPS` and the full active window from `41.0195` to `52.2134`, while keeping Q SA1 and `gameplay-idle` flat
- Reject: Loop 185 opaque-family-only frame-skip thinning
- Why it mattered: it proved that the narrower “thin only the three opaque hot families harder on the same six-family frame-skip gate” reland does not beat the kept Loop 184 baseline on the deciding full active window, so the next runtime pass should rerank toward a broader rendered-frame cohort instead of retrying that exact cadence split

## Archive Pointers

- Full history: `artifacts/mister-port/living-findings.md`
- Active checklist: `artifacts/mister-port/stock-image-software-frame-loop-series/todo.md`
- Deep research memo: `docs/agent-memory/mister-perf-deep-research-2026-03-21.md`
- Process doc: `docs/agent-memory/mister-ralph-loop-v2.md`
