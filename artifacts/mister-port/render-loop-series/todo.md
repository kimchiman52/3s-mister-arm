# Implementation Todo

Completed historical checklist.
Active follow-up work moved to `artifacts/mister-port/gameplay-loop-series/todo.md`.

## Todo Metadata

- [x] Canonical todo path: `artifacts/mister-port/render-loop-series/todo.md`
- [x] Active feature/phase: MiSTer gameplay-safe render optimization loop series
- [x] Stale todo files to retire: completed telemetry loop remains in `artifacts/mister-port/telemetry-loop/todo.md`; treat this file as the only active checklist for the remaining render-path loops

## Goal and Success Criteria

- [x] Goal: complete the remaining suggested MiSTer render-path optimizations as isolated IVRFC loops, each ending in a verified commit with no gameplay regressions
- [x] Success criteria:
  - rect-like render work avoids unnecessary geometry packing when safe
  - render ordering/sort overhead is reduced without reviving the reverted offscreen/queue behavior
  - present-path CPU cost is reduced without changing final output semantics
  - each loop passes build/package/deploy/perf verification on MiSTer and ends in a focused commit

## Scope and Constraints

- [x] In scope: `src/port/sdl/sdl_game_renderer.c`, `include/port/sdl/sdl_game_renderer.h`, `src/port/sdl/sdl_app.c`, `src/port/sdl/fbdev_presenter.c`, loop-local notes/docs if needed
- [x] Out of scope: gameplay logic, timing semantics, input handling, collision, RNG, stage logic, resource content, unrelated roadmap cleanup, perf JSON artifact commits
- [x] Constraints (time, tech, architecture, dependencies):
  - execute one optimization objective per loop
  - MiSTer target remains SDL dummy/software + fbdev presenter
  - use the verified Docker path in `docs/mister-runbook.md`
  - use 300-frame `training` captures as the primary steady-state gate
  - leave unrelated untracked perf JSON files untouched
- [x] Assumptions:
  - Docker container `3sx-mister-build` remains available or can be recreated from `docs/mister-runbook.md`
  - MiSTer target remains reachable at `192.168.1.171` with `root` / password `1`
  - telemetry from `eebfacad` remains available for comparing render/present buckets and task counters

## Blueprint Summary

- [x] Phases:
  1. Rect-path specialization for rect-like sprite draws in the renderer.
  2. Deterministic layer-pass batching to reduce sort and submit overhead while preserving draw order.
  3. Present-path reduction between `SDLApp_EndFrame()` and `FBDevPresenter_Present()`.
- [x] Dependency map:
  - Loop 1 can start immediately using current telemetry.
  - Loop 2 depends on Loop 1 preserving render-task ordering and may reuse any rect-path task tags if they help pass partitioning.
  - Loop 3 depends on the render-side loops being stable so present deltas are easier to isolate.
- [x] Risks and mitigations:
  - ordering regression risk: keep exact stable order inside each partition; verify with MiSTer smoke and review against equal-Z behavior
  - hidden gameplay-side coupling risk: restrict edits to renderer/presenter internals only
  - measurement noise risk: compare against the current `telemetry-loop-post` baseline and require non-regression on the same `training` gate
  - presenter regression risk: validate present-path changes on both the profiled high-resolution output mode and the live device output mode; clipped `320x240` outputs invalidated the direct-present assumption until `02c56268`
  - scope creep risk: reject queue/offscreen admission changes in this series because that previous experiment was reverted
- [x] Validation strategy:
  - Tier 1: Docker build/install/package and MiSTer deploy/probe per loop
  - Tier 2: 300-frame `training` perf sample per loop with JSON checks for expected counters
  - Tier 3: fresh-agent review, post-fix rebuild/redeploy, and final tagged sample before commit
- [x] Rollout considerations:
  - each loop must be independently revertable
  - do not batch multiple optimization themes into one commit
  - update `artifacts/mister-port/living-findings.md` only if a loop completes successfully

## Iterative Chunks

### Chunk 1: Rect-Path Specialization

- [x] Value delivered: avoid unnecessary per-task vertex packing and geometry submission for rect-like draws that can be emitted through a lighter-weight path
- [x] Scope boundary: `src/port/sdl/sdl_game_renderer.c`, `include/port/sdl/sdl_game_renderer.h`, and minimal telemetry plumbing in `src/port/sdl/sdl_app.c`
- [x] Estimated effort (target 45-90 min): ~90 min
- [x] Dependencies: existing telemetry from `eebfacad`
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] `docker exec 3sx-mister-build bash -lc 'set -euxo pipefail; cd /src; JOBS=2 bash build-deps.sh --profile mister; CC=clang CXX=clang++ cmake -S . -B build/mister -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON; cmake --build build/mister --parallel 2; cmake --install build/mister --prefix build/mister-install'`
    - [x] `tools/mister/package.sh build/mister-install build/mister-package`
    - [x] validated password-auth `rsync` wrapper from `docs/mister-runbook.md`
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene training --frames 300 --tag rect-loop-post`
    - [x] `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - [x] `jq '.metrics | {frame_time, render, render_task_count, rect_copy_tasks, batch_runs, batched_task_count, sort_strategy}' artifacts/mister-port/perf/rect-loop-post.json`
- [x] Chunk gate pass criteria:
  - [x] render median does not regress beyond the current ~`17.28 ms` baseline and improves materially (`16.94 ms` mean render baseline to `2.45 ms` mean render on `rect-loop-post`)
  - [x] no startup/resource/input regressions on MiSTer
  - [x] rect-specialized path preserves visual ordering and blending semantics for sprite/UI draws within the probed `training` workload
- [x] Evidence to capture in progress log:
  - [x] before/after render and frame means: `telemetry-loop-post` `26.10/16.94/8.23 ms` frame/render/present vs `rect-loop-post` `11.80/2.45/8.44 ms`
  - [x] render-task count stayed flat (`1.81` mean) while rect-copy work replaced prior geometry batching (`rect_copy_tasks` mean `1.67`, `batch_runs` mean dropped from `0.83` to `0.00`)
  - [x] review disposition and final commit hash: fresh-agent review attempts stalled, so the loop closed on local audit plus MiSTer verification and landed as `ee069c84`

### Chunk 2: Deterministic Ordering Fast Path

- [x] Value delivered: remove the hot equal-Z reversal path by queueing equal-Z work in final draw order, reducing render ordering overhead without a broader pass partition refactor
- [x] Scope boundary: `src/port/sdl/sdl_game_renderer.c` only; broad background/fighter/HUD pass splitting deferred because current training telemetry now averages only `1.81` render tasks/frame after Chunk 1
- [x] Estimated effort (target 45-90 min): ~45 min
- [x] Dependencies: Chunk 1 complete and verified
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] build/install/package/deploy commands from Chunk 1
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene training --frames 300 --tag layer-loop-post`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - [x] `jq '.metrics | {frame_time, render, render_task_count, rect_copy_tasks, batch_runs, batched_task_count, sort_strategy}' artifacts/mister-port/perf/layer-loop-post.json`
- [x] Chunk gate pass criteria:
  - [x] deterministic ordering is preserved for equal-Z work in the verified `training` workload
  - [x] sort work improves versus Chunk 1 (`equal_z_reverse` ratio `0.8333` to `0.0000`, `sort=none` on all 300 sampled frames) while render mean improves slightly (`2.4461 ms` to `2.4031 ms`)
  - [x] no queue/offscreen reject logic is reintroduced
- [x] Evidence to capture in progress log:
  - [x] chosen narrowed model and why it is behavior-safe: keep original comparator/index semantics, but insert equal-Z tasks at the head of their run during enqueue so no post-pass reversal is needed
  - [x] telemetry change in sort-strategy usage: `none` rose from `16.7%` to `100%` and `equal_z_reverse` dropped from `83.3%` to `0%`
  - [x] review disposition and final commit hash: fresh-agent review attempts again stalled, local audit found no accepted regression issue, and the loop landed as `6487167c`

### Chunk 3: Present-Path Reduction

- [x] Value delivered: reduce readback/copy overhead in the fbdev present handoff without changing output semantics or letterbox behavior
- [x] Scope boundary: `src/port/sdl/fbdev_presenter.c`, `src/port/sdl/fbdev_presenter.h`, and `src/port/sdl/sdl_app.c` only
- [x] Estimated effort (target 45-90 min): ~90 min
- [x] Dependencies: Chunks 1-2 complete and verified
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] build/install/package/deploy commands from Chunk 1
    - [x] `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only` (validated via perf-sampler startup probe on `present-loop-direct-final`)
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene training --frames 300 --tag present-loop-direct-final`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - [x] `jq '.metrics | {frame_time, render, present, copy_bytes, dirty_tiles, full_copy_fallback}' artifacts/mister-port/perf/present-loop-direct-final.json`
- [x] Chunk gate pass criteria:
  - [x] present median does not regress beyond the current ~`8.14 ms` baseline and improves materially (`8.4639 ms` mean present on `layer-loop-post` to `6.1158 ms` mean present on `present-loop-direct-final`)
  - [x] `copy_bytes` and fallback ratios remain internally consistent with the chosen path (`355205.12` mean bytes, `full_copy_fallback_ratio=0.0000`)
  - [x] no letterbox/bar-clear/output corruption on MiSTer probe or bounded run
- [x] Evidence to capture in progress log:
  - [x] before/after present median and copy metrics: `layer-loop-post` `11.8068/2.4031/8.4639 ms` frame/render/present vs `present-loop-direct-final` `7.6938/0.1253/6.1158 ms`, with copy mean unchanged at `355205.12`
  - [x] which readback/copy branch is now dominant: direct current-target readback of the unscaled `cps3_canvas` into fbdev letterbox bounds when the destination rect matches the native `384x224` canvas; scaled/native-square cases continue to use the prior composite/readback path
  - [x] review disposition and final commit hash: fresh-agent review flagged reliance on the current render target; fix accepted by explicitly rebinding `cps3_canvas` before direct fbdev readback, then re-verified on target and landed as `91b69f44`
  - [x] low-resolution follow-up: clipped `320x240` outputs black-screened after `91b69f44`; hotfix `02c56268` now keeps the direct-present path disabled unless the native rect fully fits inside the framebuffer, and the safe clipped readback path was re-verified on target (`copy_bytes` about `286720`)

## Verification Gates

- [x] Tier 1 (per chunk smoke): Docker build/install/package, MiSTer deploy, bounded probe or runtime smoke, and one 300-frame `training` perf sample
- [x] Tier 2 (phase targeted every 1-2 chunks): `jq` inspection of the loop’s expected render/present counters plus comparison to `artifacts/mister-port/perf/telemetry-loop-post.json`
- [x] Tier 3 (full-suite final gate only): fresh-agent review, post-fix rebuild/redeploy, repeated 300-frame `training` sample, then focused commit

## Checklist Sync Rules

- [x] Step checkbox is marked `[x]` immediately after its verification command passes.
- [x] Chunk checkboxes are marked `[x]` only after required chunk gate commands pass.
- [x] Goal/success and other summary checkboxes are updated when evidence is recorded.

## Right-Sized Steps

- [x] Step 1: identify rect-like render tasks that can bypass generic quad geometry assembly without changing blend/order semantics
  - Chunk: Chunk 1
  - Affected area or component: `src/port/sdl/sdl_game_renderer.c`
  - Verification method and command: `rg -n "draw_sprite_rect|SDL_RenderGeometry" src/port/sdl/sdl_game_renderer.c`
  - Dependencies: none

- [x] Step 2: implement the rect fast path, keep the existing geometry path as fallback, and expose any needed telemetry to prove it is exercised
  - Chunk: Chunk 1
  - Affected area or component: `src/port/sdl/sdl_game_renderer.c`, `include/port/sdl/sdl_game_renderer.h`, optional `src/port/sdl/sdl_app.c`
  - Verification method and command: `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene training --frames 300 --tag rect-loop-post`
  - Dependencies: Step 1

- [x] Step 3: define a deterministic ordering fast path that preserves current equal-Z semantics without speculative culling or broad pass partitioning
  - Chunk: Chunk 2
  - Affected area or component: `src/port/sdl/sdl_game_renderer.c`
  - Verification method and command: `jq '.samples | map(.sort_strategy) | unique' artifacts/mister-port/perf/layer-loop-post.json`
  - Dependencies: Chunk 1

- [x] Step 4: implement equal-Z enqueue ordering and use existing telemetry to prove the sort-path change
  - Chunk: Chunk 2
  - Affected area or component: `src/port/sdl/sdl_game_renderer.c`
  - Verification method and command: `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene training --frames 300 --tag layer-loop-post`
  - Dependencies: Step 3

- [x] Step 5: isolate the dominant present-path branch and replace or tighten the most expensive readback/copy stage without changing rendered output
  - Chunk: Chunk 3
  - Affected area or component: `src/port/sdl/fbdev_presenter.c`, `src/port/sdl/sdl_app.c`
  - Verification method and command: `jq '.metrics | {present, copy_bytes, dirty_tiles, full_copy_fallback}' artifacts/mister-port/perf/present-loop-direct-final.json`
  - Dependencies: Chunk 2

- [x] Step 6: run fresh-agent review, apply only valid fixes, re-verify, and commit each loop before moving to the next one
  - Chunk: Chunks 1-3
  - Affected area or component: loop-local diffs only
  - Verification method and command: `git log --oneline -n 1`
  - Dependencies: per-loop implementation complete

## Parallelizable Work

- [ ] Workstream: none; keep the three loops serialized so each perf delta is attributable and independently revertable
  - Parallel with: n/a
  - Preconditions: n/a

## Open Questions

- [ ] Question: which rect-like draw subset can safely use an SDL rect/copy path without losing per-vertex color or UV behavior?
- [ ] Question: can the layer-pass partitioning be expressed purely from existing render-task fields, or does it require a new lightweight task tag?
- [x] Question: is the safest present-loop target a smaller `SDL_RenderReadPixels` footprint, a different renderer target handoff, or a staging-surface reuse path?
  - Answer: the winning safe change was a different renderer target handoff, but only for the strict native-canvas case; general scaled paths still need the prior composite/readback flow
