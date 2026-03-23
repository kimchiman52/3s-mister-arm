# MiSTer Ralph Working Brief

## Purpose

- Use this file as the default working set for the active Ralph queue on `mister-dev`.
- Load this before `artifacts/mister-port/living-findings.md` unless you need exact historical closeout detail.

## Metadata

- Last updated: `2026-03-23`
- Active branch: `mister-dev`
- Active queue: re-audit the dark-render memo and lower-distortion onset accounting first, then either open one bounded software-frame-real runtime follow-up or return to the existing MiSTer-only super-activation burst-fidelity diff
- Default loop type: `measurement`
- Deciding lane: `first-visible Yun SA3 onset`
- Primary guard lane: `gameplay-idle`

## Current Goal

- Stabilize super-activation performance on MiSTer by reducing burst-only visual workload before spending more loop budget on helper-local alpha branch or codegen relands.
- The next successful loop should show a material speedup across the full super slowdown window on the expanded super matrix without changing gameplay timing, logic, determinism, or input semantics.
- Current working assumption from manual observation: Yun SA3 slowdown likely lasts roughly `80+` frames, not just the first `8`, but the exact length is still unverified and the next loops should measure it instead of assuming it.
- Immediate queue gate: re-audit the March dark-render memo before another runtime reland. Loop `164`-style basic-mode captures stayed on software-frame direct/native present, but they zeroed several software-frame workload counters because extended stats were off, so any memo that treats those zeroed counters as a path change or disappearing software-frame work needs correction before it drives queue ranking.

## Trusted Baselines

- `loop174-yun-onset-cluster-alpha-r3`
- Why it is trusted: low-distortion onset capture that stayed direct/native and proved the hot onset cohort is binary-alpha-heavy.
- `loop146-remy-rerank-r2`
- Why it is trusted: proves Remy-left is a separate exact/direct compare-dirty queue, not the same native Yun problem.
- `gameplay-idle`
- Why it is trusted: simplest non-super gameplay guard for catching broad regressions quickly.

## Current Queue

- Dark-render memo re-audit
- Why it is still live: the repo now has enough evidence to correct one important process error. Lower-distortion basic-mode onset captures preserved software-frame direct/native routing, but their zeroed software-frame workload counters were not valid proof that the underlying software-frame work disappeared; the memo should be re-audited before another runtime bet.
- Super-effect-quality matrix
- Why it is still live: user-approved fidelity reduction is still the best fallback queue if the dark-render re-audit does not produce a bounded, software-frame-real runtime candidate. Real non-`full` behavior is Yun SA3-only today, while Q/Ken/Chun-Li are still required route/regression checks until the runtime gate broadens; all evaluation should now target the whole slowdown span instead of just the onset frames.
- Native Yun deep measurement
- Why it is still live: PMU or other lower-distortion external evidence could still clarify whether the remaining full-window slowdown is memory-latency, setup, or row-walk dominated, and could also pin down the real slowdown length instead of assuming an 8-frame burst; but the memo/accounting re-audit comes first.
- Nearest-HDMI presenter follow-up
- Why it is still live: nearest still trails native on `1920x1080`, but it is not the first-line queue while the active super-activation problem remains open.

## Banned Or Demoted Families

- Alpha-branch-layout relands in `software_frame_non_integer.c`
- Why it is banned or demoted: Loops `177` and `178` failed in opposite ways and burned the immediate branch-layout/codegen family.
- In-band repeat-pressure family-plus-render-subphase collectors
- Why it is banned or demoted: Loop `176` more than doubled render cost on the deciding comparator and is not decision-grade.
- Narrower `ix 80` or palette-split native Yun retries
- Why it is banned or demoted: the March rerank already closed that queue without new separation evidence.

## Top Candidates

- Re-audit `docs/agent-memory/mister-perf-dark-render-research-2026-03-21.md` against Loop `164+` lower-distortion capture semantics and current code, then decide whether one bounded runtime candidate still survives
- Lever class: `measurement`
- Expected upside: removes a process-level false signal before another runtime loop and may still recover one high-value software-frame-real candidate if the memo survives correction
- Main risk: the audit may end as a docs-only pivot with no fresh runtime reland, in which case the queue should return to workload-fidelity rather than forcing another helper micro-opt
- Validate `super-effect-quality = full|simplified|minimal` on `yun-sa3-repeat-pressure`, then run Q SA1, Ken SA3, and Chun-Li SA2 pressure lanes as part of the same deciding super matrix
- Lever class: `workload-fidelity`
- Expected upside: still the best next queue if the dark-render re-audit does not leave behind one bounded runtime reland, while also checking whether improvements hold beyond the old first-window shortcut and across multiple real super lanes
- Main risk: current non-`full` behavior is still gated to trusted P1 Yun SA3, so non-Yun captures are still partly validation guards today, not proof of broader effect coverage
- Research and plan expansion from Yun-only gating to all supers across the full roster once `minimal` gets trusted Yun SA3 close to stable speed
- Lever class: `measurement`
- Expected upside: turns a single-lane super-fidelity win into a roadmap for broader player-visible benefit instead of stopping at Yun-only success
- Main risk: expanding too early without a stable Yun proof could spread the queue too wide, so this must stay sequenced after the first clear Yun keep
- `frame-skip` follow-up on trusted Yun SA3 only, after the first automated matrix closes
- Lever class: `workload-fidelity`
- Expected upside: larger full-window relief remains possible if the current raster-thinning modes are still too weak
- Main risk: it reuses the previous rendered frame on alternating trusted Yun SA3 burst frames, so it is cadence-sensitive and intentionally excluded from Loop 179's first automated sweep
- Stronger post-sort burst-task shedding if `minimal` is still too slow
- Lever class: `workload-fidelity`
- Expected upside: follows the same approved lane with larger potential gain than another branch micro-opt and can target slowdown beyond the opening frames
- Main risk: may need careful guardrails to avoid changing gameplay-adjacent visuals outside the intended burst window
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

- Loop type: `measurement`
- Existing diff under test: docs/process corrections plus any bounded memo updates required to reclassify Loop `164+` lower-distortion captures correctly
- One scoped change: re-audit the dark-render memo against current code and trusted Loop `164+` evidence before another runtime reland; only if that audit leaves behind one bounded software-frame-real candidate should the next loop open runtime work, otherwise return directly to the super-effect-quality whole-window validation queue
- Stop immediately if: the re-audit depends on inventing an unverified renderer-path swap from zeroed basic-mode counters, ignores the preserved `software_frame_owned/direct_present/fallback` route evidence, or tries to force a runtime candidate after the audit has collapsed its premises
- Capture plan: no new runtime capture is required for the memo-only re-audit by default; use existing trusted `loop164`, `loop169`, `loop174`, `loop176`, `loop177`, and `loop178` evidence plus current code inspection to classify which conclusions were routing-truth, which were workload-accounting truth, and which mixed the two. If a follow-up runtime idea survives the audit, it must be written down explicitly as a separate next loop rather than smuggled into the audit itself.
- Keep if: the audit yields one bounded, software-frame-real candidate whose premises still hold after the accounting correction, or it cleanly proves that the queue should return to workload-fidelity instead
- Reject if: the audit still relies on mixed-regime accounting or zeroed basic-mode software-frame counters as primary evidence; in that case close the audit as a docs/process correction and resume the super-effect-quality queue
- Post-Yun success note: if the `super-effect-quality` queue later proves that trusted Yun SA3 is stable enough on `minimal`, do not stop at the Yun-only keep. Open a bounded research-and-planning loop next to map how the same burst-fidelity surface could expand to all supers for all characters, including gating strategy, likely grouping, measurement matrix, and rollout order before implementation broadens.

## Recent Decisive Evidence

- Keep: `loop174-yun-onset-cluster-alpha-r3`
- Why it mattered: it narrowed the hot onset cohort to a binary-alpha problem with lower measurement drag than the older collectors
- Reject: Loop `176` repeat-pressure render-subphase collector
- Why it mattered: it proved the current in-band collector family is too distortive to guide another runtime decision
- Pivot: Loops `177` and `178` alpha-branch/layout failures
- Why it mattered: they justify moving first-line effort away from helper-local branch-order/codegen relands

## Archive Pointers

- Full history: `artifacts/mister-port/living-findings.md`
- Active checklist: `artifacts/mister-port/stock-image-software-frame-loop-series/todo.md`
- Deep research memo: `docs/agent-memory/mister-perf-deep-research-2026-03-21.md`
- Process doc: `docs/agent-memory/mister-ralph-loop-v2.md`
