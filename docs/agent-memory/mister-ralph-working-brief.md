# MiSTer Ralph Working Brief

## Purpose

- Use this file as the default working set for the active Ralph queue on `mister-dev`.
- Load this before `artifacts/mister-port/living-findings.md` unless you need exact historical closeout detail.

## Metadata

- Last updated: `2026-03-23`
- Active branch: `super-fidelity-ralph-loop`
- Active queue: bounded MiSTer rechecks on `2026-03-23` still have not produced any trustworthy remote command output, so preserved branch `preserve-loop187-flipped-41-1-frame-skip` remains the first verification target once the device gate recovers; keep Loop 184's full-window Yun-only `frame-skip` extension as the burst-fidelity baseline, keep Loop 186's Ken/Chun preset repair, and do not invent a different runtime queue until the preserved flipped `41/1` follow-up is either verified or explicitly retired
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
- Loop 186 rejected the broader non-flipped `256x256` rendered-tick reland but kept the Ken/Chun preset repair. Widening the trusted selector to families `57/317`, `57/328`, `58/327`, and `58/344` improved the same-build candidate versus `full`, but only to `33.6987 / 28.2044 / 29.2213 / 40.4766 FPS` on the deciding first-`8` / first-`60` / first-`82` / full-active windows, still far below kept Loop 184 `frame-skip` (`50.9294 / 49.8172 / 51.4887 / 52.2134`). The repeat-preset button repair is still a keep because Ken SA3 and Chun-Li SA2 now both produce `p1_super_art_active_starts_total = 1` at frame `179` with `44` active frames on-device.
- Loop 187 verification is currently device-blocked, not reranked away. The preserved flipped `41/1` rendered-tick reland still matches the top surviving burst-fidelity hypothesis, but a fresh `2026-03-23` recheck hit the stop condition when bounded `misterctl.sh health` and `probe` both timed out before any trustworthy remote command completed. Keep that preserved branch as the next verification target instead of opening a different runtime experiment from local-only evidence.
- Loop 188 kept the queue blocked for the same reason, but with a cleaner serial recheck. The preserved flipped `41/1` rendered-tick reland is still the oldest unresolved runtime candidate, yet serial bounded `health` and `probe` attempts under an outer watchdog again timed out on `2026-03-23` before any trustworthy remote output appeared. Treat this as device-gate recovery work, not as evidence against the preserved runtime diff.
- Loop 189 kept the queue blocked again after another preserved-branch-first audit. `preserve-native-analog-yc-crt-filter` and `preserve-yc-packet-logging` still resolve as already-integrated ancestors of `HEAD`, the dark-render memo still does not leave a bounded current-tree runtime candidate after the accounting correction, and fresh bounded `health` / `probe` rechecks under an outer watchdog again died on `2026-03-23` before any trustworthy remote output appeared. Treat this as another device-gate recovery closeout, not as evidence against the preserved flipped `41/1` diff.
- The absolute full-mode baseline on the live device is lower than the earlier Loop 180 artifact family, so Loop 181 is judged on same-build `full` versus `minimal` deltas rather than on stale cross-loop absolute FPS. The route truth still matches the trusted direct/native software-frame path.
- Loop 184 closes the window-length question on the trusted repeat-pressure lane: the longer `124`-frame cap materially improved the previously untouched active tail without broad guard regression, so another cap-length tweak is no longer the best next runtime bet.
- Trusted Yun is still not close enough to stable `60 FPS` to stop at this keep. After Loop 186's rejection, the next runtime pass should stay on the same burst-fidelity family but move away from both the exact opaque-family-only cadence split and the broader non-flipped `256x256` selector reland, using the now-live Ken/Chun matrix coverage to judge a materially different burst-only follow-up.

## Trusted Baselines

- `loop174-yun-onset-cluster-alpha-r3`
- Why it is trusted: low-distortion onset capture that stayed direct/native and proved the hot onset cohort is binary-alpha-heavy.
- `loop146-remy-rerank-r2`
- Why it is trusted: proves Remy-left is a separate exact/direct compare-dirty queue, not the same native Yun problem.
- `gameplay-idle`
- Why it is trusted: simplest non-super gameplay guard for catching broad regressions quickly.

## Current Queue

- Preserve-loop187 verification recheck
- Why it is still live: the runtime diff already exists on `preserve-loop187-flipped-41-1-frame-skip`, and the latest two cycles were blocked only by MiSTer `health` / `probe` timeouts. This stays ahead of any new runtime hypothesis until the device gate produces a trustworthy command again.
- Materially different burst-only follow-up on top of the kept full-window `frame-skip` baseline
- Why it is still live: Loop 186 closed the “broaden the trusted selector to the next safe-shape non-flipped `256x256` families” idea. The deciding Yun lane improved over same-build `full`, but it still stayed far below kept Loop 184, so the next runtime lever must be meaningfully different rather than another small selector broadening on the same premise. This remains secondary until `preserve-loop187-flipped-41-1-frame-skip` is verified or explicitly retired.
- Keep decision-grade Ken SA3 / Chun-Li SA2 matrix coverage live in every future super-fidelity sweep
- Why it is still live: Loop 186 repaired the repeat-preset button mismatch, and both lanes now produce real super activations on-device. Future burst-fidelity relands should use those lanes as actual guards instead of treating them as deferred setup work.
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
- Loop 186 broader non-flipped `256x256` rendered-tick selector reland (`57/317`, `57/328`, `58/327`, `58/344`)
- Why it is banned or demoted: it improved the same-build candidate versus `full`, but it still landed far below kept Loop 184 on the deciding Yun windows, so do not retry this exact safe-shape selector broadening unchanged now.
- Raw `game` wait / zero-warmup repeat-super capture starts for frame-skip validation
- Why it is banned or demoted: Loop 183's first probe showed that starting at plain `game` with zero warmup was too early and never reached the first scripted super within the sample. Use `game-input-active` with zero warmup for trigger-inclusive repeat-super frame-skip validation instead.
- In-band repeat-pressure family-plus-render-subphase collectors
- Why it is banned or demoted: Loop `176` more than doubled render cost on the deciding comparator and is not decision-grade.
- Narrower `ix 80` or palette-split native Yun retries
- Why it is banned or demoted: the March rerank already closed that queue without new separation evidence.

## Top Candidates

- Preserve-loop187 verification recheck
- Lever class: `workload-fidelity`
- Expected upside: it is still the highest-value bounded runtime diff already in hand, and preserved-branch-first rules keep it ahead of any newly invented reland once the MiSTer gate produces a trustworthy command again
- Main risk: the device gate is still the blocker, so the next loop may close as another recovery recheck rather than a runtime judgment if remote commands keep failing before verification starts
- Materially different burst-only follow-up on the kept full-window `frame-skip` baseline
- Lever class: `workload-fidelity`
- Expected upside: Loop 186 closed the next non-flipped safe-shape selector broadening, which leaves the remaining headroom in a different super-specific rendered-tick lever rather than another small extension of the same family gate. The dominant measured outside family is still flipped `41/1`, and that exact reland is already preserved for verification on-device.
- Main risk: the remaining visible headroom is in a riskier part of the workload, and device-gate instability is now the immediate blocker. This stays secondary until the preserved diff is either verified or explicitly retired.
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
- Existing diff under test: preserved branch `preserve-loop187-flipped-41-1-frame-skip`
- One scoped change: do not author a new runtime reland first. Recheck bounded MiSTer `health` / `probe`, then verify the preserved flipped `41/1` rendered-tick follow-up on `yun-sa3-repeat-pressure` with Q SA1, Ken SA3, Chun-Li SA2, and `gameplay-idle` as guards
- Stop immediately if: bounded serial `misterctl.sh health` and `probe` still fail before any trustworthy remote command completes, or if verifying the preserved diff would require widening beyond burst-scoped MiSTer-only render degradation
- Capture plan: once the device gate recovers, keep `yun-sa3-repeat-pressure` as the deciding lane, start the repeat-super captures at `game-input-active` with zero warmup so the trigger lands inside the sample, and continue judging first `8`, first `60`, first `82`, the post-`82` active tail, and the full active window; keep Q SA1, Ken SA3, and Chun-Li SA2 live as actual guards now that their repeat-pressure presets trigger real activations
- Keep if: the preserved flipped `41/1` follow-up materially improves the full trusted active window beyond kept Loop 184 while preserving the same direct/native route semantics and without broad Q/Ken/Chun/idle regression
- Reject if: the preserved follow-up fails to move trusted Yun materially or widens degradation beyond the intended burst scope once trustworthy device verification completes; if the device gate remains unhealthy again, preserve/defer the branch rather than treating that as runtime rejection
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
- Keep: Loop 186 Ken/Chun repeat-preset button repair
- Why it mattered: it restored decision-grade `ken-sa3-repeat-pressure` and `chunli-sa2-repeat-pressure` matrix lanes by switching those repeat presets to a kick-button finisher, producing real `p1_super_art_active_starts_total = 1` captures on-device instead of zero-activation false guards
- Reject: Loop 186 broader non-flipped `256x256` rendered-tick selector reland
- Why it mattered: it proved that broadening the trusted selector to `57/317`, `57/328`, `58/327`, and `58/344` is still not enough to beat kept Loop 184 on the deciding Yun windows, so future frame-skip follow-ups need a materially different lever rather than another safe-shape selector extension
- Blocked: Loop 187 preserved flipped `41/1` verification recheck
- Why it mattered: it proved the next blocker is the MiSTer gate, not queue ranking. The preserved branch still matches the top surviving burst-fidelity hypothesis, but bounded `health` and `probe` both timed out on `2026-03-23`, so the next loop must start with device recovery/recheck instead of opening a different runtime experiment.
- Blocked: Loop 188 serial device-gate recheck
- Why it mattered: it confirmed the blocker is still remote reachability rather than queue selection. Even with serial bounded checks and an outer watchdog, both `health` and `probe` timed out before any trustworthy remote output, so the preserved flipped `41/1` branch remains deferred rather than rejected.
- Blocked: Loop 189 bounded device-gate recheck
- Why it mattered: it reconfirmed that preserved-branch-first audit does not leave another unresolved perf branch ahead of `preserve-loop187-flipped-41-1-frame-skip`, and it still did not produce a trustworthy remote command. The next loop must stay on device recovery/recheck rather than opening a different runtime or measurement queue.

## Archive Pointers

- Full history: `artifacts/mister-port/living-findings.md`
- Active checklist: `artifacts/mister-port/stock-image-software-frame-loop-series/todo.md`
- Deep research memo: `docs/agent-memory/mister-perf-deep-research-2026-03-21.md`
- Process doc: `docs/agent-memory/mister-ralph-loop-v2.md`
