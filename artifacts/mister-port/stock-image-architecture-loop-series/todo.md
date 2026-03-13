# Stock Image Architecture Loop Series Todo

## Todo Metadata

- [x] Canonical todo path: `artifacts/mister-port/stock-image-architecture-loop-series/todo.md`
- [x] Active feature/phase: stock-image-only MiSTer gameplay-performance architecture loop series
- [x] Branch: `ralphpart2`
- [x] Living record: `artifacts/mister-port/living-findings.md`
- [x] Historical predecessor checklist: `artifacts/mister-port/ralphpart2/todo.md`
- [x] Stale todo files to retire:
  - `artifacts/mister-port/ralphpart2/todo.md` is now historical and must not receive new active-loop items
  - `artifacts/mister-port/gameplay-loop-series/todo.md` is a completed historical series and must not receive new active-loop items

## Goal and Success Criteria

- [x] Goal: drive the next stock-MiSTer gameplay-performance series by widening deterministic gameplay evidence first, then landing only the stock-image-safe architecture changes that survive IVRFC verification on the live `/dev/fb0` target
- [x] Success criteria:
  - every checklist chunk is one fresh-agent IVRFC loop with one scoped objective and one verified closing commit
  - the active stream stays on stock MiSTer Linux with SDL dummy/software + fbdev presenter; no custom image, `/dev/dri`, KMSDRM, or GPU-stack implementation work enters this checklist
  - deterministic scene coverage expands beyond the current idle-versus control to named `stage-heavy`, `effect-heavy`, and `super-heavy` captures
  - hybrid custom software compositor work is only kept if it improves total gameplay frame time on-device while preserving SDL fallback for uncommon cases
  - any expansion toward a 3SX-owned software game frame happens only after the hybrid path proves broad wins and coverage on the stock-image scene matrix
  - every successful loop updates `artifacts/mister-port/living-findings.md` and this checklist before commit

## Scope and Constraints

- [x] In scope:
  - deterministic gameplay capture expansion on stock MiSTer Linux
  - telemetry that quantifies hybrid-compositor eligibility and fallback reasons
  - a hybrid custom software compositor fast path for the dominant textured-rect workload while keeping SDL fallback for uncommon cases
  - a contingent 3SX-specific software compositor pilot that keeps the main `384x224` game frame in software until the final fbdev push
  - documentation updates in `artifacts/mister-port/living-findings.md` and historical checklist headers
- [x] Out of scope:
  - custom Linux image work
  - `/dev/dri`, KMSDRM, SDL GPU, Vulkan, GLES, GBM/DRM plumbing, or any GPU-stack investigation
  - gameplay logic, determinism semantics, input/timing model, resources/content, unrelated cleanup, committed perf/log artifacts
- [x] Constraints (time, tech, architecture, dependencies):
  - the live stock image exposes `/dev/fb0` only; no `/dev/dri` was found on the current target
  - the current proven bottleneck remains native exact-fit present/readback on the SDL dummy/software + fbdev path
  - use the verified Docker flow in `docs/mister-runbook.md` and the current MiSTer deploy target `root@192.168.1.171:/media/fat/games/3sx/`
  - keep one scoped item per loop and close every loop with a real local commit, even when the runtime diff is rolled back and only docs/checklist updates remain
  - runtime-changing loops require a fresh review pass, selective fixes only, then rebuild/redeploy/re-capture before commit
- [x] Assumptions:
  - the accepted baseline control remains `tools/mister/perf-sampler.sh --gameplay-idle --gameplay-warmup 120`
  - measurement-only loops may commit harness/telemetry improvements when they increase decision quality without changing gameplay behavior
  - a narrow hybrid path can be evaluated without handing the entire frame to SDL's software renderer again

## Blueprint Summary

- [x] Phases:
  - Phase A: freeze a broader deterministic gameplay matrix that includes `stage-heavy`, `effect-heavy`, and `super-heavy` gates on stock MiSTer
  - Phase B: prove or reject a hybrid 3SX software-compositor path for the dominant textured-rect workload while preserving SDL fallback
  - Phase C: only if Phase B wins broadly, pilot a 3SX-owned software game frame that stays in software until the final fbdev push
- [x] Dependency map:
  - Chunk 1 must land before any new scene-ranking claims; Chunk 2 depends on Chunk 1's stage control
  - Chunk 3 depends on the frozen scene matrix from Chunks 1-2 and is the stop/go gate for hybrid runtime work
  - Chunk 4 depends on Chunk 3 showing strong hybrid coverage; Chunk 5 depends on Chunk 4 scaffold and parity instrumentation
  - Chunk 6 depends on accepted Chunk 5 results and targets exactly one top fallback reason
  - Chunk 7 depends on accepted hybrid loops and decides whether Phase C is justified
  - Chunk 8 only starts if Chunk 7 authorizes a software-frame pilot
- [x] Risks and mitigations:
  - measurement work can accidentally perturb the default control; preserve the no-override `gameplay-idle` path and require a fresh control recapture on every harness loop
  - hybrid work can shift time from `present` into `render`; gate on total `frame_time` first, then inspect bucket shifts second
  - fallback/interleave bugs can break ordering or visuals; add per-frame parity/fallback counters, enable one workload subset at a time, and require fresh review before keep
  - the rejected SDL surface-renderer experiment can be repeated accidentally; do not give SDL ownership of the mapped framebuffer surface, and keep the final push on the fbdev presenter path throughout this stream
  - custom-image/GPU drift would stall the active stream; keep those ideas in a separate future RFC only and never as active items here
- [x] Validation strategy:
  - Tier 1 every chunk: Docker build/install/package, package, deploy, `run-3sx.sh --probe-renderer-only`, bounded `launch-osd.sh`, backend-log inspection, and `last-run.log` inspection
  - Tier 2 every 1-2 chunks: on-device gameplay perf captures for the current control plus the relevant expanded scene matrix, with `jq` checks on any new metrics
  - Tier 3 phase gates: fresh review, post-fix rebuild/redeploy/recapture, `living-findings.md` update, checklist update, and closing commit
- [x] Rollout considerations:
  - each chunk must stay independently revertable
  - never mix measurement-expansion work and runtime-path architecture work in the same loop
  - if a stop/go gate fails, close the loop with docs/findings updates and move to the next ranked item instead of carrying dead code
  - the custom-image/KMSDRM/GPU RFC is future-track only and must stay off this checklist

## Iterative Chunks

### Chunk 1: Stage-Aware Gameplay Matrix

- [x] Value delivered: deterministic stage selection on the current gameplay harness plus named stage-heavy captures that broaden evidence beyond the default idle-versus control
- [x] Scope boundary: `src/main.c`, `src/main.h`, `src/test/test_runner.c`, `tools/mister/perf-sampler.sh`, and minimal perf metadata/docs needed to label stage-aware captures
- [x] Estimated effort (target 45-90 min): ~75 min
- [x] Dependencies: current `gameplay-idle` harness and existing character/super-art override plumbing
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] run the common stock-image smoke gate from `Verification Gates`
    - [x] `bash -n tools/mister/perf-sampler.sh`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-idle --frames 300 --tag stock-arch-c1-control --gameplay-idle --gameplay-warmup 120`
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 STAGE_HEAVY_ID=<stage-id> tools/mister/perf-sampler.sh --scene gameplay-stage-heavy --frames 300 --tag stock-arch-c1-stage-heavy --gameplay-idle --gameplay-warmup 120 --test-stage "$STAGE_HEAVY_ID"`
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 STAGE_ALT_ID=<stage-id> tools/mister/perf-sampler.sh --scene gameplay-stage-alt --frames 300 --tag stock-arch-c1-stage-alt --gameplay-idle --gameplay-warmup 120 --test-stage "$STAGE_ALT_ID"`
    - [x] `jq '.metadata | {scene, stage_id, test_stage_override}' artifacts/mister-port/perf/stock-arch-c1-stage-heavy.json`
- [x] Chunk gate pass criteria:
  - [x] the no-override control remains within noise of the accepted baseline shape
  - [x] at least two explicit stage overrides complete on-device and produce materially different telemetry or total frame time
  - [x] one named `stage-heavy` gate is chosen, recorded in `living-findings.md`, and closed in a verified commit
- [x] Evidence to capture in progress log:
  - [x] chosen stage ids, capture tags, control-vs-stage deltas, and backend-log proof that the override path reached gameplay
  - Chunk 1 closeout: post-fix control `stock-arch-c1-control-postfix` stayed on `stage_id=11` at `60.82 / 1.37 / 55.58 ms`; named `stage-heavy` is `stage_id=19` via `stock-arch-c1-stage-heavy` at `73.52 / 1.76 / 63.77 ms`; explicit alternate override `stage_id=2` via `stock-arch-c1-stage-alt` landed at `73.08 / 1.48 / 67.62 ms`; and post-fix override revalidation `stock-arch-c1-stage-19-postfix` still resolved to `stage_id=19` at `75.81 / 1.85 / 65.99 ms` over `120` frames. The accepted review fixes narrowed `force_stage_transition_override()` so it only requeues background loads when the handoff state is wrong and added an init-time `Debug_w[DEBUG_STAGE_SELECT]` reset to prevent stale overrides from leaking into later no-override runs.

### Chunk 2: Deterministic Effect/Super-Heavy Gates

- [x] Value delivered: deterministic non-idle captures that exercise effect-heavy and super-heavy gameplay without replacing the default idle-versus control
- [x] Scope boundary: test-runner/runtime harness only, plus perf metadata and `tools/mister/perf-sampler.sh` preset plumbing
- [x] Estimated effort (target 45-90 min): ~90 min
- [x] Dependencies: Chunk 1 stage control and existing super-art override plumbing
- [x] Scoped loop plan (`2026-03-06`):
  - choose implementation shape `B`: add a narrow fixed input script on top of the current test runner instead of reusing the deeper replay/demo machinery, because repo-first inspection shows the replay path is coupled to `Replay_w`, `Demo_Flag`, `Play_Mode`, and replay-mode/menu transitions while the current harness already owns deterministic versus entry plus direct pad injection
  - add a `--test-scene-preset` path that stays dormant unless explicitly requested, maps named presets to deterministic stage/character/super-art defaults, and keeps the no-override `gameplay-idle` control unchanged when no preset is supplied
  - seed both heavy presets from the kept `stage-heavy(stage 19)` branch, then drive them with fixed scripted inputs only during the in-game phase so Chunk 2 remains harness/runtime plumbing plus perf metadata rather than gameplay logic work
  - success metric for this loop: one effect-heavy and one super-heavy preset complete end-to-end on MiSTer without manual input, produce stable metadata for `jq` checks, and leave the default idle control within the accepted baseline shape
- [ ] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] run the common stock-image smoke gate from `Verification Gates`
    - [x] `bash -n tools/mister/perf-sampler.sh`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-effect-heavy --frames 300 --tag stock-arch-c2-effect-heavy --test-scene-preset effect-heavy`
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-super-heavy --frames 300 --tag stock-arch-c2-super-heavy --test-scene-preset super-heavy`
    - [x] `jq '.metadata | {scene, test_scene_preset, stage_id, p1_character, p2_character, p1_super_art, p2_super_art}' artifacts/mister-port/perf/stock-arch-c2-super-heavy.json`
- [x] Chunk gate pass criteria:
  - [x] the default control still reproduces the accepted idle baseline when no new preset is supplied
  - [x] at least one deterministic effect-heavy and one deterministic super-heavy capture finish on-device without manual intervention
  - [x] the active stock-image matrix is frozen as `idle-control`, `stage-heavy`, `effect-heavy`, and `super-heavy`, and recorded in `living-findings.md`
- [x] Evidence to capture in progress log:
  - [x] preset names, stage/character/super-art selections, capture tags, and the ranking of the slowest scenes
  - Chunk 2 closeout: post-fix idle control `stock-arch-c2-control-postfix` stayed on `stage_id=11` at `60.27 / 1.41 / 54.97 ms`; accepted `effect-heavy` is preset `effect-heavy` (`Ryu/Ken`, `SA0/0`, `stage 19`) via `stock-arch-c2-effect-heavy` at `75.79 / 1.81 / 65.74 ms`; accepted `super-heavy` is preset `super-heavy` (`Ryu/Ryu`, `SA0/0`, `stage 19`) via `stock-arch-c2-super-heavy-postfix` at `75.95 / 1.89 / 65.27 ms`; the frozen slowest-scene ranking is `super-heavy` > `effect-heavy` > Chunk 1 `stage-heavy` > idle control. The first `Ryu/Ken` super-heavy attempt was reworked before closeout because it landed too close to the effect-heavy result to trust as the final distinct gate.

### Chunk 3: Hybrid-Eligibility Telemetry And Stop/Go Gate

- [x] Value delivered: on-device evidence that says whether a hybrid compositor is worth implementing, and exactly which workload subset to target first
- [x] Scope boundary: telemetry only in the renderer/perf JSON path; no runtime fast path yet
- [x] Estimated effort (target 45-90 min): ~75 min
- [x] Dependencies: Chunks 1-2 matrix freeze
- [x] Scoped loop plan (`2026-03-06`):
  - choose implementation shape `A`: extend the existing renderer frame-stats and perf JSON capture path instead of adding an external post-processor, because repo-first inspection shows `SDLGameRenderer_FrameStats` already feeds both per-frame samples and summary metrics while the frozen Chunk 1-2 captures already isolate the four scenes we need for the stop/go gate
  - define the first-cut hybrid candidate as submitted textured-rect work that stays fully inside the native `384x224` game frame, uses default opaque modulation (`ARGB=0xFFFFFFFF`), and does not request flip; classify every non-candidate task under exactly one primary fallback reason in priority order `solid -> geometry -> clip -> alpha -> color_mod -> flip` so task and pixel totals stay additive
  - add candidate/fallback task counts, submitted pixel-area totals, and per-reason fallback counts to the renderer/perf JSON path only, while keeping the live render/present fast path unchanged when no perf capture is running
  - success metric for this loop: all four frozen scenes emit the new telemetry cleanly on MiSTer, the resulting coverage table yields an explicit Chunk 3 go/no-go decision, and the loop closes without touching Chunk 4 runtime behavior
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] run the common stock-image smoke gate from `Verification Gates`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - [x] run the four frozen scene captures with the new telemetry fields enabled
    - [x] `jq '.metrics | {hybrid_candidate_tasks, hybrid_candidate_pixels, hybrid_fallback_tasks, hybrid_fallback_pixels, hybrid_reason_clip, hybrid_reason_alpha, hybrid_reason_color_mod, hybrid_reason_flip, hybrid_reason_geometry, hybrid_reason_solid}' artifacts/mister-port/perf/stock-arch-c3-super-heavy.json`
    - [x] `for f in artifacts/mister-port/perf/stock-arch-c3-control-postfix.json artifacts/mister-port/perf/stock-arch-c3-stage-heavy-postfix.json artifacts/mister-port/perf/stock-arch-c3-effect-heavy-postfix.json artifacts/mister-port/perf/stock-arch-c3-super-heavy-postfix.json; do jq -e 'all(.samples[]; (.hybrid_candidate_tasks + .hybrid_fallback_tasks) == .render_task_count and (.hybrid_reason_clip + .hybrid_reason_alpha + .hybrid_reason_color_mod + .hybrid_reason_flip + .hybrid_reason_geometry + .hybrid_reason_solid) == .hybrid_fallback_tasks)' "$f" >/dev/null; done`
- [x] Chunk gate pass criteria:
  - [x] the telemetry lands cleanly across all four named scenes
  - [x] the proposed first-cut hybrid subset covers at least 70% of submitted tasks and 80% of submitted pixel area on the two slowest scenes, or the fallback data shows one clearly dominant extra reason worth targeting next
  - [x] if the coverage gate fails, Phase B is closed as unjustified in `living-findings.md` and the loop still ends in a verified docs commit
- [x] Evidence to capture in progress log:
  - [x] candidate-coverage table, fallback-reason ranking, and the explicit go/no-go decision for hybrid runtime work
  - Chunk 3 closeout: the accepted telemetry landed as perf schema `6` with review-fixed submit-path accounting and additive invariant checks passing on all four post-fix captures. The post-fix coverage table was `idle-control 61.43% tasks / 52.17% pixels`, `stage-heavy 66.60% / 58.08%`, `effect-heavy 65.64% / 56.86%`, and `super-heavy 66.85% / 58.19%`; on the two slowest scenes the narrow first-cut subset stayed well below the `70% / 80%` threshold, but `flip` was the clearly dominant extra fallback reason (`68.42` and `67.72` tasks/frame versus `38.30` and `37.96` clip, `~10` alpha, `0` geometry, and `8` solid). Geometry recovery and rect-submit fallback both remained unexercised on the frozen matrix (`textured_geometry_* = 0` and `hybrid_reason_geometry = 0` across the final captures). Decision: stop/no-go for immediate Chunk 4 runtime work under the current stock-image-only plan; if Phase B is ever reopened, `flip` is the next ranked unsupported category to fold into the first supported subset rather than forcing the current narrow base path forward.

### Chunk 4: Hybrid Compositor Scaffold And Parity Counters

- [ ] Value delivered: a 3SX-owned software compositor scaffold for the chosen first subset, behind an explicit mode switch, with SDL fallback and parity logging
- [ ] Scope boundary: new compositor module(s), `src/port/sdl/sdl_game_renderer.c`, `src/port/sdl/fbdev_presenter.c`, config/plumbing, and parity counters; no default path change while the mode is off
- [ ] Estimated effort (target 45-90 min): ~90 min
- [ ] Dependencies: Chunk 3 stop/go approval
- [ ] Chunk-end verification commands:
  - [ ] Tier 1 smoke:
    - [ ] run the common stock-image smoke gate from `Verification Gates`
    - [ ] capture one control run with the new mode disabled
    - [ ] capture one short control run with the new mode enabled
  - [ ] Tier 2 targeted (if checkpoint chunk):
    - [ ] `jq '.metrics | {hybrid_fast_path_tasks, hybrid_fallback_tasks, hybrid_frame_eligible, hybrid_reason_geometry, hybrid_reason_flip}' artifacts/mister-port/perf/stock-arch-c4-control-hybrid.json`
- [ ] Chunk gate pass criteria:
  - [ ] the default-off path reproduces the accepted baseline shape
  - [ ] the enabled path reaches gameplay on-device, reports parity/fallback counters, and shows no blank-screen or log-path regression
  - [ ] the loop closes with a reviewed, verified commit even if the mode remains default-off
- [ ] Evidence to capture in progress log:
  - [ ] off/on capture tags, mode switch used, parity/fallback counters, and review findings

### Chunk 5: First Hybrid Fast Path Keep-Or-Reject

- [ ] Value delivered: actual runtime proof for the first hybrid fast path on the slowest stock-image scenes
- [ ] Scope boundary: enable the first hybrid subset for real on representative scenes; do not add a second fallback category yet
- [ ] Estimated effort (target 45-90 min): ~90 min
- [ ] Dependencies: Chunk 4 scaffold and parity instrumentation
- [ ] Chunk-end verification commands:
  - [ ] Tier 1 smoke:
    - [ ] run the common stock-image smoke gate from `Verification Gates`
  - [ ] Tier 2 targeted (if checkpoint chunk):
    - [ ] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-idle --frames 300 --tag stock-arch-c5-control --gameplay-idle --gameplay-warmup 120`
    - [ ] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-stage-heavy --frames 300 --tag stock-arch-c5-stage-heavy --test-scene-preset stage-heavy`
    - [ ] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-super-heavy --frames 300 --tag stock-arch-c5-super-heavy --test-scene-preset super-heavy`
    - [ ] `jq '.metrics | {frame_time, render, present, present_readback, hybrid_fast_path_tasks, hybrid_fallback_tasks}' artifacts/mister-port/perf/stock-arch-c5-super-heavy.json`
- [ ] Chunk gate pass criteria:
  - [ ] total `frame_time.mean` improves by at least 5% on `idle-control` and at least one heavy scene, with no validated scene regressing by more than 3%
  - [ ] SDL-side present/readback work drops materially where the hybrid path applies
  - [ ] fallback share stays within the modeled bound from Chunk 3 and no correctness regression survives review
  - [ ] otherwise the runtime diff is reverted and the loop closes with a docs-backed commit
- [ ] Evidence to capture in progress log:
  - [ ] before/after table for the kept baseline, fallback rate per scene, and the keep/reject decision

### Chunk 6: Single-Category Hybrid Coverage Expansion

- [ ] Value delivered: lower fallback share by teaching the hybrid path exactly one next-highest-volume unsupported case
- [ ] Scope boundary: one fallback reason only, chosen from Chunk 5 telemetry; examples include alpha blend, color modulation, flip, or clipped rects
- [ ] Estimated effort (target 45-90 min): ~75 min
- [ ] Dependencies: accepted Chunk 5 result
- [ ] Chunk-end verification commands:
  - [ ] Tier 1 smoke:
    - [ ] run the common stock-image smoke gate from `Verification Gates`
  - [ ] Tier 2 targeted (if checkpoint chunk):
    - [ ] rerun the two scenes most blocked by the chosen fallback reason plus the default control
    - [ ] `jq '.metrics | {frame_time, render, present, hybrid_fallback_tasks, hybrid_fallback_pixels, hybrid_reason_alpha, hybrid_reason_color_mod, hybrid_reason_flip, hybrid_reason_clip}' artifacts/mister-port/perf/stock-arch-c6-targeted.json`
- [ ] Chunk gate pass criteria:
  - [ ] the chosen fallback reason drops materially on the targeted scenes
  - [ ] total frame time improves on at least one previously fallback-limited scene
  - [ ] the control scene stays within noise; otherwise revert and close with docs
- [ ] Evidence to capture in progress log:
  - [ ] selected fallback reason, before/after counters, and the next stop/go recommendation

### Chunk 7: Promotion Decision For A 3SX-Owned Software Game Frame

- [ ] Value delivered: an explicit stop/go decision on whether expanding beyond hybrid is justified by accepted stock-image evidence
- [ ] Scope boundary: analysis/docs only unless a tiny telemetry addition is needed to resolve the gate
- [ ] Estimated effort (target 45-90 min): ~45 min
- [ ] Dependencies: accepted hybrid loops and current matrix captures
- [ ] Chunk-end verification commands:
  - [ ] Tier 1 smoke:
    - [ ] if the loop stays docs-only, verify the referenced perf JSONs and working-tree cleanliness instead of rebuilding
  - [ ] Tier 2 targeted (if checkpoint chunk):
    - [ ] summarize the accepted hybrid captures with `jq`/notes and compare them against the promotion thresholds
- [ ] Chunk gate pass criteria:
  - [ ] proceed to Chunk 8 only if the accepted hybrid path covers at least 85% of submitted tasks and 85% of submitted pixel area on the two slowest scenes, and the remaining SDL fallback still tracks total frame time materially
  - [ ] if the promotion gate fails, Phase C is closed in `living-findings.md` and the stock-image stream stops at the accepted hybrid end state
  - [ ] the loop still ends in a verified commit that records the decision
- [ ] Evidence to capture in progress log:
  - [ ] promotion-threshold table, final go/no-go decision, and the rationale for stopping or continuing

### Chunk 8: 3SX Software Game-Frame Pilot

- [ ] Value delivered: first pilot that keeps the main `384x224` game frame in 3SX-owned software memory until the final fbdev push, with SDL fallback limited to unsupported/uncommon draw classes
- [ ] Scope boundary: main game frame only; no custom image work, no SDL ownership of `/dev/fb0`, and no full renderer rewrite in one loop
- [ ] Estimated effort (target 45-90 min): ~90 min
- [ ] Dependencies: Chunk 7 go decision
- [ ] Chunk-end verification commands:
  - [ ] Tier 1 smoke:
    - [ ] run the common stock-image smoke gate from `Verification Gates`
  - [ ] Tier 2 targeted (if checkpoint chunk):
    - [ ] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-idle --frames 300 --tag stock-arch-c8-control --gameplay-idle --gameplay-warmup 120`
    - [ ] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-stage-heavy --frames 300 --tag stock-arch-c8-stage-heavy --test-scene-preset stage-heavy`
    - [ ] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-super-heavy --frames 300 --tag stock-arch-c8-super-heavy --test-scene-preset super-heavy`
    - [ ] `jq '.metrics | {frame_time, render, present, fbdev_push_ms, software_frame_pixels, sdl_fallback_tasks}' artifacts/mister-port/perf/stock-arch-c8-super-heavy.json`
- [ ] Chunk gate pass criteria:
  - [ ] total frame time improves versus the accepted hybrid baseline on the control and at least one heavy scene
  - [ ] no scene regresses materially and no correctness/runtime instability survives review
  - [ ] otherwise the runtime diff is reverted and the loop closes with a docs-backed commit
- [ ] Evidence to capture in progress log:
  - [ ] final pilot capture table, remaining SDL fallback share, and the keep/reject decision

## Verification Gates

- [x] Tier 1 (per chunk smoke):
  - `docker exec 3sx-mister-build bash -lc 'set -euxo pipefail; cd /src; JOBS=2 bash build-deps.sh --profile mister; CC=clang CXX=clang++ cmake -S . -B build/mister -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON; cmake --build build/mister --parallel 2; cmake --install build/mister --prefix build/mister-install'`
  - `tools/mister/package.sh build/mister-install build/mister-package`
  - deploy with the validated `rsync` command from `docs/mister-runbook.md` or its password-auth wrapper when required
  - `ssh -o StrictHostKeyChecking=no root@192.168.1.171 "/media/fat/games/3sx/run-3sx.sh --probe-renderer-only"`
  - `ssh -o StrictHostKeyChecking=no root@192.168.1.171 "cd /media/fat/games/3sx && rm -f logs/last-run.log && timeout 20 ./launch-osd.sh; rc=$?; echo runtime_rc=$rc; tail -n 40 logs/backend.log; tail -n 40 logs/last-run.log"`
  - pass only when logs still show dummy/software + fbdev + native, bounded launch returns the expected timeout shape, and `last-run.log` ends in the normal terminated exit
- [x] Tier 2 (phase targeted every 1-2 chunks):
  - always recapture the current `idle-control` alongside the scene(s) targeted by the chunk
  - freeze tags in `artifacts/mister-port/perf/` using the `stock-arch-c<chunk>-<scene>` naming pattern so later loops can compare apples-to-apples
  - summarize the relevant metrics with `jq` immediately after each capture pull and record the interpretation in `living-findings.md`
- [x] Tier 3 (full-suite final gate only):
  - every runtime-changing loop gets a fresh review before commit
  - any accepted runtime fix requires rebuild, redeploy, probe, bounded launch, and recapture of the control plus affected heavy scenes
  - update `artifacts/mister-port/living-findings.md`, update this checklist, ensure the tree is clean except intended files/artifacts, then create the closing commit

## Checklist Sync Rules

- [x] Step checkboxes are marked `[x]` immediately after their named verification command passes.
- [x] Chunk checkboxes are marked `[x]` only after the required chunk gate commands pass and the closing commit exists.
- [x] `artifacts/mister-port/living-findings.md` is updated before any chunk is considered complete.
- [x] If a runtime experiment is reverted, the checklist still records the rejected idea, verification evidence, and closure commit instead of leaving the chunk open-ended.

## Right-Sized Steps

- [x] Step 1: Expose a `--test-stage` override from CLI to the gameplay harness and perf metadata while preserving the current no-override control.
  - Chunk: Chunk 1
  - Affected area or component: `src/main.c`, `src/main.h`, `src/test/test_runner.c`, `tools/mister/perf-sampler.sh`
  - Verification method and command: Tier 1 smoke gate plus `bash -n tools/mister/perf-sampler.sh`
  - Dependencies: existing character/super-art override path

- [x] Step 2: Capture the default control plus two explicit stage overrides, choose the named `stage-heavy` gate, and record it in `living-findings.md`.
  - Chunk: Chunk 1
  - Affected area or component: MiSTer perf capture flow and `artifacts/mister-port/living-findings.md`
  - Verification method and command: the three Chunk 1 Tier 2 capture commands and `jq '.metadata | {scene, stage_id, test_stage_override}' ...`
  - Dependencies: Step 1

- [x] Step 3: Add a deterministic preset path for effect-heavy and super-heavy captures without changing normal gameplay behavior.
  - Chunk: Chunk 2
  - Affected area or component: test-runner harness, CLI/config plumbing, `tools/mister/perf-sampler.sh`
  - Verification method and command: Tier 1 smoke gate plus preset-driven MiSTer capture commands
  - Dependencies: Chunk 1

- [x] Step 4: Freeze the four-scene stock-image matrix (`idle-control`, `stage-heavy`, `effect-heavy`, `super-heavy`) and record the chosen presets in `living-findings.md`.
  - Chunk: Chunk 2
  - Affected area or component: perf capture flow and `artifacts/mister-port/living-findings.md`
  - Verification method and command: Chunk 2 Tier 2 captures and `jq '.metadata | {test_scene_preset, stage_id, p1_character, p2_character, p1_super_art, p2_super_art}' ...`
  - Dependencies: Step 3

- [ ] Step 5: Add capture-gated counters for hybrid fast-path eligibility, pixel coverage, and fallback reasons.
  - Chunk: Chunk 3
  - Affected area or component: renderer telemetry path and perf JSON output
  - Verification method and command: Tier 1 smoke gate plus `jq '.metrics | {hybrid_candidate_tasks, hybrid_candidate_pixels, hybrid_fallback_tasks, hybrid_fallback_pixels}' ...`
  - Dependencies: Chunk 2

- [ ] Step 6: Run the full frozen matrix, summarize the hybrid coverage, and close Chunk 3 with an explicit go/no-go decision.
  - Chunk: Chunk 3
  - Affected area or component: MiSTer perf capture flow, `artifacts/mister-port/living-findings.md`, this checklist
  - Verification method and command: four matrix captures plus the Chunk 3 fallback-reason `jq` summary
  - Dependencies: Step 5

- [ ] Step 7: Introduce the software-compositor scaffold behind an explicit mode switch while keeping the default path unchanged.
  - Chunk: Chunk 4
  - Affected area or component: new compositor module(s), `src/port/sdl/sdl_game_renderer.c`, `src/port/sdl/fbdev_presenter.c`, config/plumbing
  - Verification method and command: Tier 1 smoke gate with the mode off
  - Dependencies: Chunk 3 go decision

- [ ] Step 8: Add parity/fallback counters for the scaffold, run off/on-device smoke, and close the default-off scaffold loop with review.
  - Chunk: Chunk 4
  - Affected area or component: perf counters, config/runtime mode plumbing, `artifacts/mister-port/living-findings.md`
  - Verification method and command: Chunk 4 Tier 2 `jq` summary plus fresh review before commit
  - Dependencies: Step 7

- [ ] Step 9: Route the chosen first workload subset through the hybrid fast path while keeping all unsupported work on SDL fallback.
  - Chunk: Chunk 5
  - Affected area or component: software compositor fast path and fallback routing
  - Verification method and command: Tier 1 smoke gate plus the Chunk 5 control/stage-heavy/super-heavy captures
  - Dependencies: Chunk 4

- [ ] Step 10: Run the broadened performance gate, take a fresh review, and either keep or revert the first hybrid fast path in a verified commit.
  - Chunk: Chunk 5
  - Affected area or component: MiSTer perf capture flow, review workflow, docs/checklist closure
  - Verification method and command: Chunk 5 Tier 2 `jq` summary and Tier 3 review/reverify gate
  - Dependencies: Step 9

- [ ] Step 11: Choose exactly one highest-volume fallback reason from accepted Chunk 5 telemetry.
  - Chunk: Chunk 6
  - Affected area or component: perf result analysis and planning notes in `artifacts/mister-port/living-findings.md`
  - Verification method and command: summarize Chunk 5 captures and document the selected reason before editing code
  - Dependencies: accepted Chunk 5

- [ ] Step 12: Implement support for that one fallback reason, rerun the targeted scenes plus the control, and keep/revert the result with review.
  - Chunk: Chunk 6
  - Affected area or component: software compositor fallback handling for one additional category
  - Verification method and command: Tier 1 smoke gate, targeted captures, Chunk 6 `jq` summary, and Tier 3 review/reverify gate
  - Dependencies: Step 11

- [ ] Step 13: Summarize the accepted hybrid end state against the Phase C promotion thresholds.
  - Chunk: Chunk 7
  - Affected area or component: accepted perf JSONs, `artifacts/mister-port/living-findings.md`, this checklist
  - Verification method and command: `jq` summaries on the accepted hybrid capture set
  - Dependencies: accepted Chunk 5 and, if present, Chunk 6

- [ ] Step 14: Record a docs-only go/no-go decision for the software-frame pilot and close the chunk in a commit.
  - Chunk: Chunk 7
  - Affected area or component: `artifacts/mister-port/living-findings.md`, this checklist
  - Verification method and command: verify working-tree cleanliness and the recorded promotion decision before commit
  - Dependencies: Step 13

- [ ] Step 15: Pilot a 3SX-owned `384x224` software game frame while keeping final fbdev push and SDL fallback only for uncommon classes.
  - Chunk: Chunk 8
  - Affected area or component: software compositor core, fbdev presenter integration, fallback routing
  - Verification method and command: Tier 1 smoke gate plus the Chunk 8 control/stage-heavy/super-heavy captures
  - Dependencies: Chunk 7 go decision

- [ ] Step 16: Run full broad-scene verification, fresh review, and keep/reject closure for the software-frame pilot.
  - Chunk: Chunk 8
  - Affected area or component: MiSTer perf capture flow, review workflow, `artifacts/mister-port/living-findings.md`
  - Verification method and command: Chunk 8 Tier 2 `jq` summary and Tier 3 review/reverify gate
  - Dependencies: Step 15

## Parallelizable Work

- [ ] Workstream: review prep and findings-draft assembly after captures complete
  - Parallel with: perf JSON summarization for the same chunk
  - Preconditions: code changes are frozen for that loop and the required device captures have already been pulled locally

## Open Questions

- [x] Which deterministic path yields the cleanest effect/super-heavy stock-image gate without gameplay logic changes: stage override on idle-versus, a scripted demo preset, or a narrow non-idle test-runner script?
  - answer: use shape `B`, a narrow non-idle test-runner script layered on the existing deterministic versus harness; the replay/demo path stayed out because repo-first inspection showed it depends on `Replay_w`, `Demo_Flag`, `Play_Mode`, and replay/menu transitions that were deeper than Chunk 2 needed.
- [ ] Does the first hybrid subset need alpha/color-mod support from day one to clear the Chunk 3 coverage gate, or will exact textured-rect copies alone justify Chunk 4?
- [ ] If Chunk 7 authorizes Phase C, which unsupported classes stay on SDL fallback in the first software-frame pilot: direct UI only, or UI plus geometry/solids?
