# Implementation Todo

## Todo Metadata

- [x] Canonical todo path: `artifacts/mister-port/telemetry-loop/todo.md`
- [x] Active feature/phase: MiSTer render telemetry repair and counter pass
- [x] Stale todo files to retire: none; this completed loop is retained only as historical context

## Goal and Success Criteria

- [x] Goal: make MiSTer render/present telemetry accurate enough to drive the next performance loops without changing gameplay behavior
- [x] Success criteria:
  - `copy_bytes` reflects fbdev copy work on direct `SDL_ConvertPixels` success paths
  - perf JSON exposes a small set of render counters that identify queue, batching, cache, and sort behavior
  - build/package/deploy and on-device perf capture pass with no startup/input/resource regressions
  - the loop ends in a focused verified commit that excludes unrelated dirty files and perf artifacts

## Scope and Constraints

- [x] In scope: `src/port/sdl/fbdev_presenter.c`, `src/port/sdl/sdl_game_renderer.c`, `src/port/sdl/sdl_app.c`, related SDL port headers if needed, perf JSON schema/output plumbing
- [x] Out of scope: gameplay logic, timing semantics, sprite ordering behavior, render-path optimization beyond measurement accuracy, docs unrelated to this loop, perf JSON artifact commits
- [x] Constraints (time, tech, architecture, dependencies): one IVRFC-sized change set; MiSTer target remains SDL dummy/software plus fbdev; clang MiSTer build flow only; leave unrelated working-tree changes in `docs/mister-runbook.md` and untracked perf JSON files untouched
- [x] Assumptions:
  - MiSTer target remains reachable at `192.168.1.171`
  - `/media/fat/games/3sx/resources/SF33RD.AFS` is already staged on target
  - new metrics can be added without breaking local analysis scripts that only read existing fields

## Blueprint Summary

- [x] Phases:
  1. Repair inaccurate presenter byte accounting in all fbdev copy paths.
  2. Add minimal renderer counters and plumb them into per-frame samples plus JSON summary output.
  3. Run IVRFC verification, independent review, selective fixes, re-verification, and commit.
- [x] Dependency map: phase 2 depends on stable per-frame reset/latch points in the current frame loop; phase 3 depends on schema/output being finalized
- [x] Risks and mitigations:
  - schema churn risk: keep existing fields stable and add new fields additively
  - counter overhead risk: use integer counters reset once per frame; avoid heap allocation
  - accidental scope creep: reject any optimization that changes render behavior rather than measurement
- [x] Validation strategy: local clang build/package, MiSTer deploy, 120-frame smoke capture for fast iteration, 300-frame target capture for verification, independent fresh-agent review before commit
- [x] Rollout considerations: treat this as the measurement prerequisite for later rect-path and layer-pass loops; do not edit the broader MiSTer todo during this pass

## Iterative Chunks

### Chunk 1: Presenter Accounting Repair

- [x] Value delivered: `copy_bytes` becomes trustworthy on the fbdev direct-convert paths that currently under-report work
- [x] Scope boundary: `src/port/sdl/fbdev_presenter.c` and any narrow plumbing required to keep sample capture consistent
- [x] Estimated effort (target 45-90 min): ~45 min
- [x] Dependencies: none
- [ ] Chunk-end verification commands:
  - [x] `CC=clang CXX=clang++ cmake -S . -B build/mister -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON`
  - [x] `cmake --build build/mister --parallel`
  - [x] `cmake --install build/mister --prefix build/mister-install`
  - [x] `tools/mister/package.sh build/mister-install build/mister-package`
  - [x] `rsync -av --delete --omit-dir-times --no-perms --no-owner --no-group --exclude 'resources/SF33RD.AFS' --filter 'P resources/SF33RD.AFS' build/mister-package/ root@192.168.1.171:/media/fat/games/3sx/`
  - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene training --frames 120 --tag telemetry-loop-c1`
- [ ] Chunk gate pass criteria:
  - [x] perf capture succeeds on target
  - [x] at least one post-startup sample (`frame > 1`) reports `copy_bytes > 0` with `full_copy_fallback=false`
  - [x] the verification evidence shows one of the newly-fixed `SDL_ConvertPixels` presenter paths actually executed; if the training scene does not hit that branch, add a narrow presenter counter/log or a targeted capture before closing Chunk 1
  - [x] no startup/resource/input regressions observed in remote logs

### Chunk 2: Renderer Counter Plumbing

- [x] Value delivered: perf JSON shows render queue size, batching/cache activity, and sort-path behavior for each frame and in summary metrics
- [x] Scope boundary: `src/port/sdl/sdl_game_renderer.c`, `src/port/sdl/sdl_app.c`, related SDL port headers if needed
- [x] Estimated effort (target 45-90 min): ~75 min
- [x] Dependencies: Chunk 1
- [ ] Chunk-end verification commands:
  - [x] local build/package commands from Chunk 1
  - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene training --frames 300 --tag telemetry-loop-c2`
  - [x] `jq '.metrics | {copy_bytes, render_task_count, batch_runs, batched_task_count, texture_cache_hits, texture_cache_misses, texture_creates, sort_strategy}' artifacts/mister-port/perf/telemetry-loop-c2.json`
- [ ] Chunk gate pass criteria:
  - [x] new JSON fields are present and non-null
  - [x] at least one counter changes across frames in the sample set
  - [x] frame/render medians do not regress by more than 5% versus the current ~26.24 ms / ~17.08 ms baseline

### Chunk 3: IVRFC Review, Re-Verify, Commit

- [x] Value delivered: reviewed, validated telemetry pass lands as a focused commit usable by later performance loops
- [x] Scope boundary: diff from Chunks 1-2 only; no new feature expansion
- [x] Estimated effort (target 45-90 min): ~45 min
- [x] Dependencies: Chunks 1-2
- [ ] Chunk-end verification commands:
  - [x] `codex review --help >/dev/null`
  - [x] local build/package commands from Chunk 1
  - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene training --frames 300 --tag telemetry-loop-post`
  - [ ] `git status --short`
- [ ] Chunk gate pass criteria:
  - [x] review findings are evaluated and only valid fixes are applied
  - [x] final target perf JSON contains the intended counters and accurate copy accounting
  - [ ] commit includes only source and loop-local plan artifacts, not perf JSON captures or unrelated user changes

## Verification Gates

- [x] Tier 1 (per chunk smoke): local clang build/package flow from Chunk 1 plus a 120-frame MiSTer perf sample
- [x] Tier 2 (targeted verification): 300-frame MiSTer perf sample with `jq` checks for new metrics and non-regression
- [x] Tier 3 (final gate): fresh-agent review, repeat local build/package, repeat 300-frame MiSTer sample, then commit only scoped files

## Checklist Sync Rules

- [x] Mark a step or chunk complete only after its listed command succeeds and evidence is available locally
- [x] Do not check off commit completion until review and post-fix verification have both passed
- [x] Do not stage or commit files under `artifacts/mister-port/perf/` for this loop

## Right-Sized Steps

- [x] Step 1: Patch fbdev direct-convert copy paths so `frame_copy_bytes` increments on successful `SDL_ConvertPixels` copies.
  - Chunk: Chunk 1
  - Affected area or component: `src/port/sdl/fbdev_presenter.c`
  - Verification method and command: `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene training --frames 120 --tag telemetry-loop-c1`
  - Dependencies: none

- [x] Step 2: Define a compact per-frame render stats struct and reset/latch hooks in the renderer frame lifecycle.
  - Chunk: Chunk 2
  - Affected area or component: `src/port/sdl/sdl_game_renderer.c`, related header if needed
  - Verification method and command: `cmake --build build/mister --parallel`
  - Dependencies: Step 1

- [x] Step 3: Count render task volume, texture-run batching, texture cache misses/creates, and sort strategy usage.
  - Chunk: Chunk 2
  - Affected area or component: `src/port/sdl/sdl_game_renderer.c`
  - Verification method and command: `jq '.samples[0] | {render_task_count, batch_runs, batched_task_count, texture_cache_hits, texture_cache_misses, texture_creates, sort_strategy}' artifacts/mister-port/perf/telemetry-loop-c2.json`
  - Dependencies: Step 2

- [x] Step 4: Extend perf sample capture and JSON summary serialization to include the new render stats without removing existing fields.
  - Chunk: Chunk 2
  - Affected area or component: `src/port/sdl/sdl_app.c`
  - Verification method and command: `jq '.metrics | {render_task_count, batch_runs, batched_task_count, texture_cache_hits, texture_cache_misses, texture_creates, sort_strategy}' artifacts/mister-port/perf/telemetry-loop-c2.json`
  - Dependencies: Steps 1-3

- [x] Step 5: Run a fresh-agent code review on the loop diff, apply only valid fixes, re-run build/package and the 300-frame target sample, then commit scoped files.
  - Chunk: Chunk 3
  - Affected area or component: working tree diff for this loop only
  - Verification method and command: `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene training --frames 300 --tag telemetry-loop-post`
  - Dependencies: Step 4

## Parallelizable Work

- [ ] Workstream: none; keep this loop serialized to preserve a tight IVRFC scope and make review simpler

## Open Questions

- [x] Question: should the added counters live under a new `schema_version`, or stay additive under version 1 to avoid breaking existing quick `jq` usage?
- [x] Question: is `sort_strategy` better encoded as numeric counters only, or as a summary string plus per-frame flags for `none` / `equal-z-reverse` / `insertion` / `qsort`?
