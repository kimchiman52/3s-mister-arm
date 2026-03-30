# Implementation Todo

## Todo Metadata

- [x] Canonical todo path: `/Users/sb/Developer/3sx-mister/artifacts/mister-port/clean-build-loop/todo.md`
- [x] Active feature/phase: `dual-build-flavor split for telemetry vs clean MiSTer runtime`
- [x] Active upstream branch: `ralph-overnight-20260306`
- [x] Existing active todo stream left untouched: `/Users/sb/Developer/3sx-mister/artifacts/mister-port/stock-image-software-frame-loop-series/todo.md`
- [x] Stale todo files to retire/ignore:
  - `artifacts/mister-port/stock-image-architecture-loop-series/todo.md`
  - `artifacts/mister-port/ralphpart2/todo.md`
  - `artifacts/mister-port/gameplay-loop-series/todo.md`
  - `artifacts/mister-port/telemetry-loop/todo.md`
  - `artifacts/mister-port/render-loop-series/todo.md`

## Goal and Success Criteria

- [x] Goal: introduce two build flavors for MiSTer from the same tree:
  - `telemetry` build keeps current perf capture, logging, and optimization workflow intact
  - `clean` build compiles out telemetry-related hot-path work so normal gameplay runs with zero perf-instrumentation overhead
- [x] Success criteria:
  - both flavors build and package without modifying `tools/mister/package.sh`
  - telemetry flavor preserves current CLI/capture workflow and does not regress existing perf tooling
  - clean flavor compiles out perf capture plumbing plus always-updated renderer/presenter frame-stat bookkeeping
  - clean flavor launches and probes on MiSTer successfully
  - docs clearly describe when to use each flavor and how to build/package them

## Scope and Constraints

- [x] In scope:
  - CMake/build-flavor plumbing
  - app-level perf CLI/capture compile-out for clean builds
  - renderer hot-path counter/frame-stat compile-out for clean builds
  - presenter timing/frame-breakdown compile-out for clean builds
  - docs and operator workflow updates
- [x] Out of scope:
  - gameplay or rendering behavior changes
  - new optimization loops unrelated to telemetry removal
  - GPU/KMSDRM/custom Linux image work
  - changes to `tools/mister/package.sh`
- [x] Constraints:
  - chunk size target is `45-90 min` and `1-3 components`
  - each chunk needs explicit Docker/package verification
  - MiSTer validation is required before treating the clean build as usable
  - do not overwrite the current `build/mister*` flow for telemetry work; use separate flavor-specific build/install/package directories
- [x] Assumptions:
  - essential non-perf startup logging may remain in clean builds
  - the software-frame parity harness may stay telemetry-only if that materially simplifies the clean flavor
  - a compile-time feature flag is preferred over runtime branching for the clean build

## Blueprint Summary

- [x] Phases:
  - Phase A: add build-flavor plumbing and app-level compile guards so clean builds stop exposing perf capture entry points
  - Phase B: remove renderer frame-stat and task telemetry from clean-build hot paths
  - Phase C: remove presenter timing/frame-breakdown bookkeeping from clean builds and stabilize dual-artifact build flow
  - Phase D: document operator workflow and final verification
- [x] Dependency map:
  - build flag and generated config/header gating must land first
  - renderer/presenter compile-out depends on a stable app-facing interface for when telemetry is absent
  - docs should only land after the two flavors and their commands are verified
- [x] Risks and mitigations:
  - risk: clean build accidentally breaks telemetry consumers at compile time
    - mitigation: introduce explicit stubs/macros/interfaces instead of ad hoc `#ifdef` sprawl
  - risk: clean build still pays hidden hot-path cost through zeroed structs or noop counters
    - mitigation: remove callsites and per-frame resets, not just JSON emission
  - risk: dual-build commands drift or overwrite artifacts
    - mitigation: pin distinct build/install/package directories and document exact commands
- [x] Validation strategy:
  - per chunk: Docker build/install/package for both flavors
  - every `1-2` chunks: targeted local probe/sanity for both flavors
  - phase gate: MiSTer deploy/probe/smoke with clean build, plus telemetry sanity if interfaces changed
- [x] Rollout considerations:
  - keep telemetry flavor as the developer default for optimization loops
  - treat clean flavor as the player-facing runtime/package
  - include a visible build-flavor identifier in logs/help/version output if feasible

## Iterative Chunks

### Chunk 1: Build Flavor Plumbing and App-Side Compile-Out

- [x] Value delivered: both flavors build from distinct directories, and clean builds stop compiling perf CLI/capture entry points in `main`/`sdl_app`
- [x] Scope boundary:
  - add a compile-time build option such as `ENABLE_PERF_TELEMETRY`
  - thread the flag through generated config/header usage
  - gate app-level perf capture structs, CLI flags, JSON emission hooks, and basic/full perf mode plumbing
  - keep public interfaces coherent via minimal stubs or feature predicates
- [x] Estimated effort (target 45-90 min): `60-90 min`
- [x] Dependencies: none
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - telemetry build/package:
      - `docker exec 3sx-mister-build bash -lc 'set -euxo pipefail; cd /src; CC=clang CXX=clang++ cmake -S . -B build/mister-telemetry -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=ON; cmake --build build/mister-telemetry --parallel 2; cmake --install build/mister-telemetry --prefix build/mister-telemetry-install; tools/mister/package.sh build/mister-telemetry-install build/mister-telemetry-package'`
    - clean build/package:
      - `docker exec 3sx-mister-build bash -lc 'set -euxo pipefail; cd /src; CC=clang CXX=clang++ cmake -S . -B build/mister-clean -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=OFF; cmake --build build/mister-clean --parallel 2; cmake --install build/mister-clean --prefix build/mister-clean-install; tools/mister/package.sh build/mister-clean-install build/mister-clean-package'`
  - [x] Tier 2 targeted:
    - verify telemetry flavor still exposes perf options and clean flavor omits or rejects them
    - run local `--probe-renderer-only` sanity for both flavors if binaries are easily invocable outside the package
- [x] Chunk gate pass criteria:
  - both flavors compile/package
  - telemetry build preserves perf CLI behavior
  - clean build compiles with perf capture code removed rather than merely disabled at runtime
- [x] Evidence to capture in progress log:
  - chosen build flag name and default
  - exact CLI behavior difference between telemetry and clean binaries
  - package directory naming used for both artifacts

### Chunk 2: Renderer Hot-Path Telemetry Removal in Clean Build

- [x] Value delivered: clean builds stop paying renderer frame-stat/task-source/update telemetry costs in hot paths
- [x] Scope boundary:
  - gate or remove per-task/per-frame `frame_stats` updates in `sdl_game_renderer`
  - gate renderer telemetry-only structs/fields from headers where practical
  - keep telemetry flavor behavior unchanged
  - adjust any app-side readers so clean builds do not require renderer stats to exist
- [x] Estimated effort (target 45-90 min): `60-90 min`
- [x] Dependencies: Chunk 1
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - rerun both Docker build/install/package commands from Chunk 1
  - [x] Tier 2 targeted:
    - telemetry flavor local sanity: perf capture still records renderer stats
    - clean flavor local sanity: renderer path compiles and runs without stat fields/counters active
- [x] Chunk gate pass criteria:
  - clean build removes hot-path renderer bookkeeping called out in the plan context
  - no clean-build runtime or public-interface dependency remains on renderer perf stats
  - telemetry flavor still emits expected renderer metrics
- [x] Evidence to capture in progress log:
  - which renderer counters/callsites were removed from clean builds
  - whether `SDLGameRenderer_GetFrameStats()` became gated, stubbed, or split by flavor

### Chunk 3: Presenter Breakdown Removal and Dual-Artifact Runtime Validation

- [x] Value delivered: clean builds also remove presenter timing/frame-breakdown bookkeeping, and the two packaged flavors are validated as runnable artifacts
- [x] Scope boundary:
  - gate `fbdev_presenter` frame-stat resets, timing accumulation, and perf-only reporting for clean builds
  - ensure clean and telemetry packages remain distinguishable and repeatable via separate build/install/package dirs
  - validate packaged runtime behavior rather than just object-level compilation
- [x] Estimated effort (target 45-90 min): `60-90 min`
- [x] Dependencies: Chunk 2
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - rerun both Docker build/install/package commands from Chunk 1
  - [x] Tier 2 targeted:
    - packaged/local probe sanity for both flavors
  - [x] Tier 3 phase gate:
    - deploy clean package to MiSTer
    - `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only`
    - bounded launch smoke through `/media/fat/games/3sx/launch-osd.sh` or equivalent validated wrapper
    - optional telemetry-package MiSTer sanity if interface packaging changed materially
- [x] Chunk gate pass criteria:
  - clean flavor is proven runnable on MiSTer
  - presenter per-frame perf bookkeeping is compiled out in clean builds
  - dual-artifact build flow is stable and does not clobber telemetry outputs
- [x] Evidence to capture in progress log:
  - MiSTer probe/smoke result for clean flavor
  - exact presenter bookkeeping removed from clean builds
  - final build/package directory convention

### Chunk 4: Docs and Operator Workflow

- [x] Value delivered: docs tell developers which flavor to use, how to build/package them, and what guarantees clean builds provide
- [x] Scope boundary:
  - update relevant build/perf/operator docs
  - record the clean-build loop outcome in the new canonical todo and any living findings doc if warranted
  - document any intentionally telemetry-only tools or flags
- [x] Estimated effort (target 45-90 min): `45-60 min`
- [x] Dependencies: Chunk 3
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - `git diff --check`
    - rerun the final successful Docker package command(s) if docs reference exact paths/commands that were adjusted during implementation
  - [x] Tier 2 targeted:
    - spot-check docs against the built artifact names and verified MiSTer commands
- [x] Chunk gate pass criteria:
  - docs match actual commands, directory names, and flavor behavior
  - the canonical todo is fully checked off with recorded evidence
  - repo is ready for future clean-vs-telemetry packaging use without tribal knowledge
- [x] Evidence to capture in progress log:
  - final doc files updated
  - any remaining intentional limitation of the clean build

## Verification Gates

- [x] Tier 1 (per chunk smoke):
  - `git diff --check`
  - Docker configure/build/install/package for both flavors using separate build trees
- [x] Tier 2 (phase targeted every 1-2 chunks):
  - local CLI/probe sanity for both flavors
  - telemetry flavor verifies perf path still works
  - clean flavor verifies perf flags/paths are absent or compiled out cleanly
- [x] Tier 3 (full-suite final gate only):
  - MiSTer deploy/probe/smoke for clean build after presenter/runtime split is complete
  - final repo cleanliness check before closeout

## Checklist Sync Rules

- [x] Step checkbox is marked `[x]` immediately after its verification command passes.
- [x] Chunk checkboxes are marked `[x]` only after required chunk gate commands pass.
- [x] Goal/success and other summary checkboxes are updated when evidence is recorded.

## Right-Sized Steps

- [x] Step 1: add CMake build flag and a generated compile-time definition for perf telemetry
  - Chunk: `Chunk 1`
  - Affected area or component: `CMakeLists.txt`, generated config/header plumbing
  - Verification method and command: both Docker configure/build/package commands from Chunk 1
  - Dependencies: none

- [x] Step 2: gate app-level perf CLI parsing, capture structs, and JSON/log emission behind the new build flag
  - Chunk: `Chunk 1`
  - Affected area or component: `src/main.h`, `src/main.c`, `include/port/sdl/sdl_app.h`, `src/port/sdl/sdl_app.c`
  - Verification method and command: telemetry CLI retains perf flags; clean build omits/rejects them after both builds succeed
  - Dependencies: Step 1

- [x] Step 3: make the clean build interface coherent by replacing telemetry consumers with stubs/feature predicates instead of runtime no-ops
  - Chunk: `Chunk 1`
  - Affected area or component: `main`/`sdl_app` cross-calls and any perf helper entry points
  - Verification method and command: local probe sanity for both flavors
  - Dependencies: Step 2

- [x] Step 4: remove renderer per-task source counters and per-frame stat seeding from clean builds
  - Chunk: `Chunk 2`
  - Affected area or component: `include/port/sdl/sdl_game_renderer.h`, `src/port/sdl/sdl_game_renderer.c`
  - Verification method and command: both Docker builds plus telemetry perf capture sanity
  - Dependencies: Step 3

- [x] Step 5: resolve renderer stat accessors/readers so clean builds do not depend on renderer frame-stat storage or updates
  - Chunk: `Chunk 2`
  - Affected area or component: renderer/app interfaces that currently expose or consume `frame_stats`
  - Verification method and command: clean flavor local runtime/probe sanity
  - Dependencies: Step 4

- [x] Step 6: remove presenter per-frame stat reset and timing accumulation from clean builds
  - Chunk: `Chunk 3`
  - Affected area or component: `src/port/sdl/fbdev_presenter.h`, `src/port/sdl/fbdev_presenter.c`
  - Verification method and command: both Docker builds and packaged probe sanity
  - Dependencies: Step 5

- [x] Step 7: stabilize dual-artifact build outputs and validate the clean package on MiSTer
  - Chunk: `Chunk 3`
  - Affected area or component: build directory conventions, deploy workflow, validated launch path
  - Verification method and command: MiSTer probe/smoke gate from Chunk 3
  - Dependencies: Step 6

- [x] Step 8: update docs and close the checklist with exact commands and flavor guarantees
  - Chunk: `Chunk 4`
  - Affected area or component: relevant docs plus this canonical todo
  - Verification method and command: `git diff --check` and spot-check docs against successful commands
  - Dependencies: Step 7

## Parallelizable Work

- [x] Workstream: draft doc wording for flavor usage while runtime chunks are in progress
  - Parallel with: `Chunk 2` and `Chunk 3`
  - Preconditions: final artifact names and flag names are settled

## Open Questions

- [ ] Should the clean binary print an explicit startup line such as `Perf telemetry: off (clean build)` to make runtime identification obvious?
- [x] Is it preferable for clean builds to omit `--perf-*` options entirely, or to keep parser stubs that fail fast with a clear message?
  - Implemented in Chunk 1: clean builds omit the perf options from `--help` and reject `--perf-capture` as an unknown option.
- [x] Does any non-MiSTer local workflow depend on perf structures existing even when capture is not used, requiring a small compatibility shim?
  - Current answer after Chunk 1: no additional compatibility shim was needed beyond header stubs and compile-time guards.

## Execution Log

- [x] 2026-03-07T17:05:13Z to 2026-03-07T17:16:54Z | Chunk 1 / Step 1
  - Added `ENABLE_PERF_TELEMETRY` with default `ON` and generated `port/build_config.h` from `cmake/port_build_config.h.in`.
  - Verified the exact Chunk 1 Docker build/install/package commands for both `-DENABLE_PERF_TELEMETRY=ON` and `-DENABLE_PERF_TELEMETRY=OFF`.
  - Confirmed distinct artifact directories: `build/mister-telemetry`, `build/mister-telemetry-install`, `build/mister-telemetry-package`, `build/mister-clean`, `build/mister-clean-install`, and `build/mister-clean-package`.
  - Next: Step 2 app-side perf CLI/config/capture compile-out in `main` and `sdl_app`.
- [x] 2026-03-07T17:16:54Z to 2026-03-07T17:22:54Z | Chunk 1 / Steps 2-3 / Chunk gate
  - Re-ran the exact Chunk 1 telemetry and clean Docker build/install/package commands; both flavors packaged successfully without clobbering one another.
  - Telemetry `--help` still exposes `--perf-capture`, `--perf-basic`, `--perf-output`, `--perf-wait-in-game`, and `--software-frame-parity-check`.
  - Clean `--help` omits the perf flags, and `build/mister-clean/3sx --perf-capture 1` fails with `error: unknown option`.
  - Local `--probe-renderer-only` sanity passed for both `build/mister-telemetry-install/bin/3sx` and `build/mister-clean-install/bin/3sx` with `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy XDG_RUNTIME_DIR=/tmp`.
  - App-side compile-out now lives in `src/main.h`, `src/main.c`, `include/port/sdl/sdl_app.h`, and `src/port/sdl/sdl_app.c` via conditional config fields, header stubs, and telemetry-only capture machinery.
  - Next: Chunk 2 renderer hot-path telemetry removal in clean builds.
- [x] 2026-03-07T17:22:54Z to 2026-03-07T17:44:25Z | Chunks 2-4 / Renderer + presenter compile-out / docs + device gate
  - Clean builds now drop renderer task-source assignment, software-frame state now uses dedicated booleans instead of telemetry counters, and hot `SetTexture()` cache hit/miss/create accounting is compiled out with the rest of clean-flavor renderer telemetry.
  - Presenter clean builds now skip per-frame `frame_stats` reset/breakdown setup and do not write presenter path telemetry; telemetry flavor behavior stayed intact.
  - Re-ran both final Docker build/install/package commands. Telemetry still exposes `--perf-capture`, `--perf-basic`, `--perf-output`, `--perf-wait-in-game`, and `--software-frame-parity-check`; clean still hides `--perf-*`, rejects `--perf-capture`, and both flavors still pass Docker `--probe-renderer-only`. Telemetry also still passes `--headless --software-frame-parity-check`.
  - MiSTer validation passed on the clean package after redeploy through the password-auth `expect`/`rsync` wrapper with `-o PubkeyAuthentication=no -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1`: `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only` still reported dummy/software + fbdev + native + `Software frame mode: on`, and the bounded `launch-osd.sh` smoke again ended at `runtime_rc=124` with `last-run.log` ending in `exit=143`.
  - Docs updated: `docs/mister-runbook.md` now explains telemetry vs clean flavors, exact dual-build/package commands, and the validated password-auth deploy flags; `artifacts/mister-port/living-findings.md` now records the clean-build split and the recovered MiSTer deploy path.
