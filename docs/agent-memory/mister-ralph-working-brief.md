# MiSTer Ralph Working Brief

## Purpose

- Use this file as the default working set for the active Ralph queue on `mister-dev`.
- Load this before `artifacts/mister-port/living-findings.md` unless you need exact historical closeout detail.

## Metadata

- Last updated: `2026-03-22`
- Active branch: `mister-dev`
- Active queue: closeable first-pass validation of the existing MiSTer-only super-activation burst-fidelity diff
- Default loop type: `workload-fidelity`
- Deciding lane: `yun-sa3-repeat-pressure`
- Primary guard lane: `gameplay-idle`

## Current Goal

- Stabilize super-activation performance on MiSTer by reducing burst-only visual workload before spending more loop budget on helper-local alpha branch or codegen relands.
- The next successful loop should show a material first-visible or early-burst speedup on the expanded super matrix without changing gameplay timing, logic, determinism, or input semantics.

## Trusted Baselines

- `loop174-yun-onset-cluster-alpha-r3`
- Why it is trusted: low-distortion onset capture that stayed direct/native and proved the hot onset cohort is binary-alpha-heavy.
- `loop146-remy-rerank-r2`
- Why it is trusted: proves Remy-left is a separate exact/direct compare-dirty queue, not the same native Yun problem.
- `gameplay-idle`
- Why it is trusted: simplest non-super gameplay guard for catching broad regressions quickly.

## Current Queue

- Super-effect-quality matrix
- Why it is still live: user-approved fidelity reduction is now the highest expected-value lever, but the first closeable pass must stay honest about current scope: real non-`full` behavior is Yun SA3-only today, while Q/Ken/Chun-Li are secondary route/regression checks until the runtime gate broadens.
- Native Yun deep measurement
- Why it is still live: PMU or other lower-distortion external evidence could still clarify whether the remaining onset ceiling is memory-latency, setup, or row-walk dominated.
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

- Validate `super-effect-quality = full|simplified|minimal` on `yun-sa3-repeat-pressure`, then use Q/Ken/Chun-Li pressure lanes as secondary route/regression checks
- Lever class: `workload-fidelity`
- Expected upside: largest remaining user-visible win with the lowest risk of another tiny helper-local false summit, while still checking that the new public sweep surface does not destabilize other scripted super lanes
- Main risk: current non-`full` behavior is still gated to trusted P1 Yun SA3, so non-Yun captures are validation guards, not proof of broader effect coverage
- `frame-skip` follow-up on trusted Yun SA3 only, after the first automated matrix closes
- Lever class: `workload-fidelity`
- Expected upside: larger burst relief remains possible if the current raster-thinning modes are still too weak
- Main risk: it reuses the previous rendered frame on alternating trusted Yun SA3 burst frames, so it is cadence-sensitive and intentionally excluded from Loop 179's first automated sweep
- Stronger post-sort burst-task shedding if `minimal` is still too slow
- Lever class: `workload-fidelity`
- Expected upside: follows the same approved lane with larger potential gain than another branch micro-opt
- Main risk: may need careful guardrails to avoid changing gameplay-adjacent visuals outside the intended burst window
- External or off-path PMU-backed measurement on the native Yun onset lane
- Lever class: `measurement`
- Expected upside: could finally distinguish setup cost from row-walk cost before another runtime specialization
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
- Existing diff under test: the already-implemented `super-effect-quality` surface across `docs/config.md`, `include/port/sdl/sdl_game_renderer.h`, `src/main.c`, `src/port/sdl/sdl_app.c`, `src/port/sdl/sdl_game_renderer.c`, `src/test/test_runner.c`, and `tools/mister/perf-sampler.sh`
- One scoped change: validate that existing diff as-is on device and close the loop from that evidence; do not add stronger thinning or `frame-skip` inside Loop 179
- Stop immediately if: gameplay timing/logic changes, the direct/native route drifts unexpectedly, `gameplay-idle` regresses materially, or the documented sweep plumbing is missing in the tree under test
- Capture plan: `full/simplified/minimal` on `yun-sa3-repeat-pressure`; use `q-sa1-repeat-pressure`, `ken-sa3-repeat-pressure`, and `chunli-sa2-repeat-pressure` as secondary route/regression sweeps that should remain effectively `full` under the current Yun-only gate, with simpler repeat lanes only when a pressure lane proves ambiguous; keep `frame-skip` out of this first automated matrix because it is a Yun-only cadence-risk follow-up
- Keep if: at least one Yun fidelity mode produces a clear super-lane win while `gameplay-idle` and the secondary non-Yun sweeps stay inside guardrails
- Reject if: all Yun first-pass fidelity modes stay too weak, the documented sweep plumbing is absent or broken, or broad regressions appear; queue stronger thinning or an explicit `frame-skip` follow-up as the next loop instead of extending Loop 179 in place

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
