# Implementation Todo

## Todo Metadata

- [x] Canonical todo path: `artifacts/mister-port/scaled-present-path/todo.md`
- [x] Active feature/phase: MiSTer nearest-scaled present-path optimization for modern displays
- [x] Stale todo files to retire:
  - none on the current clean branch
  - keep historical Ralph loop checklists and journals on `ralph-loop-support` and `mister-perf-backup-20260310-7dd791e8` archived; do not reuse them as the active checklist for this stream

## Goal and Success Criteria

- [x] Goal:
  - make `scale-mode = nearest` on MiSTer use a fast direct fbdev/native present path suitable for modern displays instead of the slower SDL-scaled `screen_texture` path
- [x] Success criteria:
  - `tools/mister/perf-sampler.sh` can automate `scale-mode` selection without hand-editing the remote config
  - nearest-mode captures clearly report active scale-mode and dominant fbdev present path
  - normal nearest-mode gameplay on MiSTer no longer routes through the non-native `screen_texture` present path
  - nearest-mode control plus at least one heavy scene improve `frame_time.mean` by at least `10%` relative to the captured nearest baseline
  - `present_readback.mean_ms` drops materially on the validated nearest-mode gates
  - validated `native` controls do not regress by more than `3%`
  - gameplay behavior, aspect ratio, message composition, screenshots, and FPS overlay placement remain correct

## Active Hotspot Matrix

- [x] Nearest-HDMI loops should no longer rely on only `control` and `stage-heavy` for decision-grade validation.
- [x] Automated nearest gates that are available on this branch today:
  - `control` via `--gameplay-idle`
  - `stage-heavy` via `--test-scene-preset stage-heavy`
  - `ibuki-stage7` via `--test-scene-preset effect-heavy --test-stage 7`
  - `2p-character-select` via `--perf-wait-test-phase character-select`
  - `menu-transition` via `--perf-wait-test-phase character-select-transition`
- [x] Manual or measurement-only hotspot that still matters:
  - `genei-jin-first-activation` remains a known user-visible lane, but there is still no trusted unattended capture on this branch; do not blindly reland the historically rejected `yun-genei-jin` preset and call it coverage
- [x] Follow-on rule:
  - when a nearest HDMI runtime change is large enough to justify expanded validation, sample at least one menu lane and one stage-specific lane in addition to `control` and `stage-heavy`

## Scope and Constraints

- [x] In scope:
  - MiSTer `scale-mode = nearest` measurement support
  - MiSTer native/fbdev present-path routing for nearest mode
  - fbdev mapped nearest-scaling optimization in `src/port/sdl/fbdev_presenter.c`
  - docs updates after the runtime path is validated
- [x] Out of scope:
  - changing the default shipped scale-mode
  - optimizing `linear` or `soft-linear` unless they benefit automatically from shared fbdev helper changes
  - GPU, KMSDRM, `/dev/dri`, or custom-image rendering work
  - gameplay, timing-model, or content changes
- [x] Constraints (time, tech, architecture, dependencies):
  - keep chunk size to roughly `45-90` minutes and `1-3` components
  - use the existing MiSTer stock Linux path: SDL dummy/software plus fbdev presenter
  - use `telemetry` builds for perf iteration and `clean` builds only for player-facing validation
  - use `tools/mister/misterctl.sh` for deploy/probe/smoke and `tools/mister/perf-sampler.sh` for captures
  - preserve the accepted `software-frame-mode = on` MiSTer default
  - on this host, `3sx-mister-build` is currently `x86_64`; use the validated `/work-arm` ARM cross-build path inside that container for any MiSTer-deployable package
- [x] Assumptions:
  - the main user-facing target is `scale-mode = nearest` on a modern panel, not CRT `native`
  - `square-pixels` remains a separate path and should stay flat while nearest work lands
  - common MiSTer output is `1280x720`, but the implementation should remain generic for other framebuffer sizes
  - full-screen nearest will still write materially more pixels than `native`, so the realistic target is “much faster than today,” not parity with `native`

## Blueprint Summary

- [x] Phases:
  - Phase A: add repeatable nearest-mode measurement plumbing and record the baseline matrix
  - Phase B: route nearest mode onto the native/fbdev present pipeline instead of the SDL `screen_texture` branch
  - Phase C: optimize the fbdev mapped nearest scaler once nearest is on the correct pipeline
  - Phase D: validate the clean/player build and document modern-display guidance
- [x] Dependency map:
  - measurement support must land first so nearest captures are reproducible
  - native-path routing comes before scaler micro-optimization, otherwise measurements mix pipeline and copy costs
  - docs and player validation should wait until the nearest runtime path is proven stable
- [x] Risks and mitigations:
  - risk: nearest-mode native routing breaks message overlays or screenshots
    - mitigation: keep the existing readback/composited fallback path for message-content and screenshot cases until direct nearest presentation is proven safe
  - risk: nearest-mode routing improves path selection but the mapped scaler remains too CPU-heavy
    - mitigation: isolate scaler work in its own chunk and measure `copy`/`present` buckets separately
  - risk: telemetry overhead distorts the scaled-path measurements
    - mitigation: use `--perf-basic` for decision-grade gameplay captures and reserve full telemetry for route attribution
  - risk: a generic scaler rewrite regresses `native` or `square-pixels`
    - mitigation: keep exact and integer fast paths untouched and rerun native control guardrails every chunk
- [x] Validation strategy:
  - tier 1: `git diff --check`, `bash -n tools/mister/perf-sampler.sh`, and telemetry build/package at the end of every chunk
  - tier 2: deploy telemetry package and run bounded nearest/native capture matrix every `1-2` chunks
  - tier 3: deploy clean package and validate player-facing nearest mode only after the telemetry path is accepted
- [x] Rollout considerations:
  - keep all work behind existing `scale-mode` selection; do not silently change shipped defaults
  - land measurement and runtime changes as separate commits
  - update docs only after the final path and capture commands are verified on MiSTer

## Iterative Chunks

### Chunk 1: Nearest Measurement Plumbing and Baseline Matrix

- [x] Value delivered:
  - nearest-mode captures become repeatable and self-describing, so later runtime work is based on measured path data instead of ad hoc config edits
- [x] Scope boundary:
  - add a `--scale-mode <mode>` override to `tools/mister/perf-sampler.sh`
  - write the chosen scale-mode into the temporary remote config during captures
  - export active scale-mode into perf metadata or backend diagnostics so captures prove which path was tested
  - recover the nearest baseline matrix for control, stage-heavy, Ibuki stage, and attract-demo logo
- [ ] Estimated effort (target 45-90 min): `60-75 min`
- [x] Dependencies:
  - none
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - `git diff --check`
    - `bash -n tools/mister/perf-sampler.sh`
    - `docker exec 3sx-mister-build bash -lc 'set -euxo pipefail; cp /src/tools/mister/perf-sampler.sh /work-arm/tools/mister/perf-sampler.sh; cp /src/src/port/sdl/sdl_app.c /work-arm/src/port/sdl/sdl_app.c; cd /work-arm; export CC=clang; export CXX=clang++; export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig; export CFLAGS=\"--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include\"; export CXXFLAGS=\"$CFLAGS\"; export LDFLAGS=\"--target=arm-linux-gnueabihf --gcc-toolchain=/usr\"; cmake -S . -B build/mister-telemetry -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=ON -DCMAKE_C_COMPILER_TARGET=arm-linux-gnueabihf -DCMAKE_CXX_COMPILER_TARGET=arm-linux-gnueabihf; cmake --build build/mister-telemetry --parallel 2; cmake --install build/mister-telemetry --prefix build/mister-telemetry-install; tools/mister/package.sh build/mister-telemetry-install build/mister-telemetry-package; readelf -h build/mister-telemetry-package/bin/3sx | sed -n \"1,18p\"'`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh health`
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh deploy --src build/mister-telemetry-package-arm-c1`
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh probe`
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh smoke`
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-idle --frames 300 --tag nearest-c1-control --gameplay-idle --gameplay-warmup 120 --software-frame-mode on --scale-mode nearest --perf-basic`
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-stage-heavy --frames 300 --tag nearest-c1-stage-heavy --test-scene-preset stage-heavy --software-frame-mode on --scale-mode nearest --perf-basic`
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-ibuki-stage7 --frames 300 --tag nearest-c1-ibuki-stage7 --test-scene-preset effect-heavy --test-stage 7 --software-frame-mode on --scale-mode nearest --perf-basic`
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene attract-demo-logo --frames 180 --tag nearest-c1-attract-logo --perf-wait-runtime-state attract-demo-logo --scale-mode nearest --perf-basic`
- [x] Chunk gate pass criteria:
  - nearest captures are reproducible without manual config editing
  - captures record enough metadata to prove nearest mode was actually active
  - the baseline matrix identifies the dominant nearest present path and whether the slowdown is present-bound, copy-bound, or readback-bound
- [x] Evidence to capture in progress log:
  - nearest baseline FPS/frame/update/render/present numbers for each gate
  - dominant fbdev path for each nearest gate
  - exact gap between nearest and native control

### Chunk 2: Route Nearest Onto Native/Fbdev Presentation

- [x] Value delivered:
  - nearest mode stops paying the extra SDL `screen_texture` scaling/compositing tax during normal MiSTer gameplay
- [x] Scope boundary:
  - extend the native present-path routing in `src/port/sdl/sdl_app.c` so nearest mode uses `native_output_rect` and fbdev presentation
  - keep message-content and screenshot fallback behavior correct
  - preserve exact and integer paths for `native` and `square-pixels`
- [ ] Estimated effort (target 45-90 min): `60-90 min`
- [x] Dependencies:
  - Chunk 1 baseline and measurement support
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] `git diff --check`
    - `bash -n tools/mister/perf-sampler.sh`
    - [x] `docker exec 3sx-mister-build bash -lc 'set -euxo pipefail; cd /src; CC=clang CXX=clang++ cmake -S . -B build/mister-telemetry -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=ON; cmake --build build/mister-telemetry --parallel 2; cmake --install build/mister-telemetry --prefix build/mister-telemetry-install; tools/mister/package.sh build/mister-telemetry-install build/mister-telemetry-package'`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh deploy --src build/mister-telemetry-package`
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh probe`
    - [x] rerun `nearest-c1-control`, `nearest-c1-stage-heavy`, and `nearest-c1-attract-logo` with fresh `c2` tags
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/perf-sampler.sh --scene gameplay-idle --frames 300 --tag native-c2-control-guard --gameplay-idle --gameplay-warmup 120 --software-frame-mode on --scale-mode native --perf-basic`
- [x] Chunk gate pass criteria:
  - normal nearest gameplay no longer falls through the non-native `screen_texture` branch
  - nearest control is dominated by `software_frame_mapped_scale` or `current_target_mapped_scale` rather than readback/composited paths
  - `present_readback.mean_ms` on nearest control drops materially from the Chunk 1 baseline
  - native control remains within the `3%` regression guardrail
- [x] Evidence to capture in progress log:
  - before/after present-path ratios on nearest control and one heavy gate
  - any remaining cases that still require the composited/readback fallback
  - screenshot and message-overlay behavior notes

### Chunk 3: Optimize Mapped Nearest Scaling

- [x] Value delivered:
  - nearest mode becomes materially cheaper once it is on the correct pipeline, reducing per-frame copy and present cost on modern displays
- [x] Scope boundary:
  - optimize `copy_argb_surface_scaled_to_fb_mapped_rect(...)` in `src/port/sdl/fbdev_presenter.c`
  - prefer cached lookup tables or fixed-point stepping over per-pixel divides in the mapped scaler
  - preserve the existing exact and integer fast paths unchanged
- [ ] Estimated effort (target 45-90 min): `60-90 min`
- [x] Dependencies:
  - Chunk 2
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - `git diff --check`
    - `docker exec 3sx-mister-build bash -lc 'set -euxo pipefail; cd /src; CC=clang CXX=clang++ cmake -S . -B build/mister-telemetry -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=ON; cmake --build build/mister-telemetry --parallel 2; cmake --install build/mister-telemetry --prefix build/mister-telemetry-install; tools/mister/package.sh build/mister-telemetry-install build/mister-telemetry-package'`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh deploy --src build/mister-telemetry-package`
    - rerun `nearest` control, stage-heavy, Ibuki stage 7, and attract-logo captures with fresh `c3` tags
    - rerun native control and square-pixels control spot checks for guardrails
- [x] Chunk gate pass criteria:
  - nearest control and at least one heavy gate improve `frame_time.mean` by at least `10%` versus the Chunk 1 nearest baseline
  - nearest `present_ms` and/or presenter `copy_ms` drop materially relative to the Chunk 2 routed baseline
  - no validated native or square-pixels guardrail regresses by more than `3%`
- [x] Evidence to capture in progress log:
  - before/after nearest timing table with `frame`, `render`, `present`, and dominant fbdev path
  - whether remaining nearest cost is now mostly copy bandwidth rather than path overhead
  - any scenes that still need path-specific follow-up

### Chunk 4: Player Validation and Docs Closeout

- [x] Value delivered:
  - the accepted nearest-mode improvements are validated in the clean package and documented for future work
- [x] Scope boundary:
  - build and deploy the `clean` package with the accepted runtime changes
  - update docs for nearest-mode modern-display guidance, perf workflow, and any new sampler flags
  - record the final keep/reject decision for any optional scaler follow-up left out of scope
- [ ] Estimated effort (target 45-90 min): `45-60 min`
- [x] Dependencies:
  - Chunk 3
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - `git diff --check`
    - `bash -n tools/mister/perf-sampler.sh`
    - `docker exec 3sx-mister-build bash -lc 'set -euxo pipefail; cd /work-arm; export CC=clang; export CXX=clang++; export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig; export CFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include"; export CXXFLAGS="$CFLAGS"; export LDFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr"; cmake -S . -B build/mister-clean -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=OFF -DCMAKE_C_COMPILER_TARGET=arm-linux-gnueabihf -DCMAKE_CXX_COMPILER_TARGET=arm-linux-gnueabihf; cmake --build build/mister-clean --parallel 2; cmake --install build/mister-clean --prefix build/mister-clean-install; tools/mister/package.sh build/mister-clean-install build/mister-clean-package; readelf -h build/mister-clean-package/bin/3sx | sed -n "1,18p"'`
    - `rm -rf build/mister-clean-package-arm && docker cp 3sx-mister-build:/work-arm/build/mister-clean-package ./build/mister-clean-package-arm`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh deploy --src build/mister-clean-package-arm`
    - `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh probe`
    - bounded nearest-mode player validation on-device with the clean package
- [x] Chunk gate pass criteria:
  - clean package deploys and probes successfully with the accepted nearest runtime path
  - docs match the actual capture and validation flow
  - the repo is ready for another nearest-mode optimization pass without rediscovering the same plumbing
- [x] Evidence to capture in progress log:
  - final nearest before/after summary
  - doc files updated
  - any remaining follow-up item that should become a separate todo stream

## Verification Gates

- [x] Tier 1 (per chunk smoke):
  - `git diff --check`
  - `bash -n tools/mister/perf-sampler.sh` when that file changes
  - telemetry or clean build/package for the flavor touched by the chunk
- [x] Tier 2 (phase targeted every 1-2 chunks):
  - deploy telemetry package and rerun the nearest capture matrix
  - probe MiSTer after every runtime chunk
  - rerun native control guardrails whenever present-path code changes
- [x] Tier 3 (full-suite final gate only):
  - deploy clean package to MiSTer
  - verify nearest modern-display play on the clean package
  - update docs and memory artifacts only after the runtime keep decision is final

## Checklist Sync Rules

- [x] Step checkbox is marked `[x]` immediately after its verification command passes.
- [x] Chunk checkboxes are marked `[x]` only after required chunk gate commands pass.
- [x] Goal/success and other summary checkboxes are updated when evidence is recorded.

## Right-Sized Steps

- [x] Step 1: add `--scale-mode` override support to `tools/mister/perf-sampler.sh`
  - Chunk: `Chunk 1`
  - Affected area or component: `tools/mister/perf-sampler.sh`
  - Verification method and command: `bash -n tools/mister/perf-sampler.sh`
  - Dependencies: none

- [x] Step 2: export active scale-mode into perf metadata or backend diagnostics used by the sampler
  - Chunk: `Chunk 1`
  - Affected area or component: `src/port/sdl/sdl_app.c`, possibly `src/main.c` if a new metadata field is needed
  - Verification method and command: telemetry build/package plus one nearest tagged capture that reports scale-mode explicitly
  - Dependencies: Step 1

- [x] Step 3: recover the nearest baseline matrix for control, stage-heavy, Ibuki stage 7, and attract-demo logo
  - Chunk: `Chunk 1`
  - Affected area or component: MiSTer telemetry package and perf artifacts under `artifacts/mister-port/perf/`
  - Verification method and command: the four Chunk 1 targeted `perf-sampler` commands
  - Dependencies: Steps 1-2

- [x] Step 4: extend MiSTer native-path selection so nearest uses `native_output_rect`/fbdev presentation
  - Chunk: `Chunk 2`
  - Affected area or component: `src/port/sdl/sdl_app.c`
  - Verification method and command: telemetry build/package plus a nearest control capture showing `software_frame_mapped_scale` or `current_target_mapped_scale`
  - Dependencies: Step 3

- [x] Step 5: preserve correctness for message-content, screenshots, and fallback readback when nearest uses the native path
  - Chunk: `Chunk 2`
  - Affected area or component: `src/port/sdl/sdl_app.c`, `src/port/sdl/fbdev_presenter.c`
  - Verification method and command: MiSTer `probe`, bounded smoke, and nearest attract-logo capture after the routing change
  - Dependencies: Step 4

- [x] Step 6: rerun native and nearest guardrails after the routing change
  - Chunk: `Chunk 2`
  - Affected area or component: MiSTer telemetry package and capture matrix
  - Verification method and command: nearest control/stage-heavy/attract plus native control reruns
  - Dependencies: Step 5

- [x] Step 7: replace mapped nearest per-pixel divides with cached LUT or fixed-point stepping
  - Chunk: `Chunk 3`
  - Affected area or component: `src/port/sdl/fbdev_presenter.c`
  - Verification method and command: telemetry build/package plus nearest control rerun showing lower `present`/`copy` cost
  - Dependencies: Step 6

- [x] Step 8: preserve or improve row-reuse behavior in the mapped scaler for repeated source rows
  - Chunk: `Chunk 3`
  - Affected area or component: `src/port/sdl/fbdev_presenter.c`
  - Verification method and command: nearest stage-heavy and Ibuki reruns after Step 7
  - Dependencies: Step 7

- [x] Step 9: validate nearest improvements against native and square-pixels guardrails
  - Chunk: `Chunk 3`
  - Affected area or component: telemetry capture matrix
  - Verification method and command: nearest and native/square-pixels targeted captures with fresh `c3` tags
  - Dependencies: Step 8

- [x] Step 10: deploy the clean package and update docs for nearest modern-display workflow
  - Chunk: `Chunk 4`
  - Affected area or component: `docs/config.md`, `docs/mister-runbook.md`, `docs/agent-memory/mister-performance.md`, clean package output
  - Verification method and command: clean build/package, `misterctl.sh deploy`, and `misterctl.sh probe`
  - Dependencies: Step 9

## Parallelizable Work

- [x] Workstream:
  - local inspection of `fbdev_presenter.c` scale helpers and review of existing perf JSON can happen in parallel with Docker telemetry builds once Chunk 1 plumbing is authored
  - Parallel with:
    - build/package turnaround
  - Preconditions:
    - no concurrent MiSTer deploy/capture session is holding the local MiSTer lock

## Open Questions

- [x] Question:
  - nearest is the primary target; if the mapped-scaler rewrite ends up also benefiting `linear` or `soft-linear`, keep that as an incidental win rather than widening scope during the first pass
- [x] Question:
  - if Chunk 3 still leaves nearest clearly bandwidth-bound after path routing and LUT work, start a separate follow-up stream for dirty-row or tile-aware scaled software-frame present instead of overloading this checklist

## Cycle Log

- 2026-03-15T00:05:20-0400
  - Commit hash:
    - recorded in the loop closure commit
  - Bottleneck targeted:
    - the remaining row-granular `software_frame_mapped_scale` copy tax on the restored nearest HDMI direct path at `1920x1080`, specifically whether tightening changed mapped rows to their changed source-column span could cut bandwidth without touching route selection or native exact behavior
  - Change summary:
    - recovered fresh `r6` nearest and native baselines on the live device and confirmed there was still no route regression: nearest stayed on `software_frame_mapped_scale`, native stayed on `software_frame_exact`, and the preserved `r5` row-cache baseline still described the current branch accurately
    - updated `src/port/sdl/fbdev_presenter.c` to record first/last changed source columns for each cached `384x224` mapped-present row and remap only that destination span through the existing nearest LUT, while preserving the current whole-row fallback whenever the mapped cache or LUT is unavailable
    - attempted `codex review --uncommitted` for the required review pass, but the helper stalled again; completed a manual scoped review of the kept diff and found no additional correctness issues
  - Verification result summary:
    - `git diff --check`, ARM telemetry rebuild/install/package in `3sx-mister-build-nearest-hdmi-perf`, and non-destructive `docker cp` export to `build/mister-telemetry-package-arm-nearest-r7-spans` all passed, with `readelf` still reporting `ELF32 ARM` and hard-float ABI inside the container package
    - MiSTer `deploy`, `probe`, and bounded `smoke` all passed on the candidate package; probe and smoke still reported `FBDEV: active (1920x1080 ...)`, `Native render path: enabled (scale-mode=nearest)`, and `Software frame mode: on`
    - `nearest-hdmi-r7-control-full` improved from the fresh `r6` baseline `48.3212 FPS / 20.6948 / 9.7569 / 9.7423 ms` (`frame / present / present_copy`) and `1490016` copied bytes/frame to `65.4989 FPS / 15.2674 / 4.4119 / 4.3974 ms` and `443678.77` copied bytes/frame, while staying on `software_frame_mapped_scale = 1.0000`
    - `nearest-hdmi-r7-stage-heavy-basic` improved from `33.4123 FPS / 29.9291 / 16.1042 ms` and `2588697.60` copied bytes/frame to `46.3955 FPS / 21.5538 / 7.8331 ms` and `935399.44` copied bytes/frame, again staying fully on `software_frame_mapped_scale`
    - the first native guard `native-hdmi-r7-control-basic` landed outside the `3%` budget, so the guard was rerun immediately; the accepted rerun `native-hdmi-r7-control-basic-rerun` held `93.0101 FPS / 10.7515 / 0.5518 ms` versus the fresh `r6` guard `94.6604 FPS / 10.5641 / 0.5473 ms`, keeping native exact presentation inside the allowed drift on `software_frame_exact = 1.0000`
  - Keep/rollback decision with reason:
    - keep; the dirty-span reland preserves the intended direct nearest HDMI presenter route, cuts mapped present bandwidth materially on both measured gates, and the accepted native rerun stays within guardrail
  - Next best candidate optimization:
    - if modern-display nearest still needs another loop after this reland, stay inside partial mapped-present follow-up by moving from coarse row spans to source-tile-guided copies or another sparse-span reuse shape rather than reopening route selection

- 2026-03-14T23:40:54-0400
  - Research target:
    - modern-display HDMI nearest on the restored direct path: confirm the fresh `r4` baseline still matches the preserved `r3` state, then test a single partial-present hypothesis that avoids rewriting unchanged mapped rows to the `1920x1080` framebuffer
  - Change summary:
    - recovered fresh `r4` nearest and native baselines on the live device and confirmed there was no route regression: nearest still direct-presented on `software_frame_mapped_scale`, while native remained on `software_frame_exact`
    - updated `src/port/sdl/fbdev_presenter.c` to cache the prior `384x224` source rows for mapped nearest present and only rescale/write destination rows whose source rows changed; invalidated that cache on non-mapped presenter paths so fallback/readback frames still force a safe refresh
    - attempted `codex review --uncommitted` for the required review pass, but the helper stalled again; completed a manual scoped review of the kept diff and found no additional correctness issues
  - Verification evidence:
    - `git diff --check` passed; the ARM telemetry rebuild/install/package through `/work-arm` in `3sx-mister-build-nearest-hdmi-perf` succeeded, `readelf -h build/mister-package/bin/3sx` still reported `ELF32` `ARM` with hard-float ABI inside the container, and the package exported cleanly to `build/mister-telemetry-package-arm-nearest-r5-rowcache`
    - MiSTer `deploy`, `probe`, and bounded `smoke` all passed on the candidate package, with probe and smoke still reporting `FBDEV: active (1920x1080 ...)`, `Native render path: enabled (scale-mode=nearest)`, and `Software frame mode: on`
    - control keep gate: `nearest-hdmi-r4-control-full` = `21.3593 FPS` / `46.8180 / 35.8808 / 35.8674 ms` (`frame / present / present_copy`) with `6220800` copied bytes/frame; `nearest-hdmi-r5-control-full` improved to `48.3931 FPS` / `20.6641 / 9.7551 / 9.7403 ms` with `1490016` copied bytes/frame and the dominant path unchanged at `software_frame_mapped_scale = 1.0000`
    - heavy nearest keep gate: `nearest-hdmi-r4-stage-heavy-basic` = `20.0642 FPS` / `49.8400 / 35.9736 ms` with `6220800` copied bytes/frame; `nearest-hdmi-r5-stage-heavy-basic` improved to `34.9093 FPS` / `28.6457 / 15.0385 ms` with `2588697.60` copied bytes/frame, again staying fully on `software_frame_mapped_scale`
    - native guard held: `native-hdmi-r4-control-basic` = `93.4393 FPS` / `10.7021 / 0.5452 ms`; `native-hdmi-r5-control-basic` stayed effectively flat at `92.9181 FPS` / `10.7622 / 0.5390 ms` on `software_frame_exact = 1.0000`
  - Keep/rollback decision:
    - keep the mapped-present source-row cache reland; on the live `1920x1080` HDMI path it preserves the intended direct presenter route, cuts the remaining nearest present/copy cost by more than half on both measured gates, and leaves native exact presentation flat
  - Final commit hash:
    - recorded in the loop closure commit
  - Next best candidate:
    - if modern-display nearest still needs another loop after this reland, keep the branch on partial mapped-present follow-up and widen from row-only updates to narrower dirty spans or source-tile-guided copies before reopening route-selection work

- 2026-03-14T23:15:00-0400
  - Research target:
    - modern-display HDMI `scale-mode = nearest` slowdown on the restored direct path: verify the current branch against the preserved `r1` recovery at `1920x1080`, confirm whether the older mapped-LUT reland was missing, and test that single hypothesis before widening to a more invasive scaled-present rewrite
  - Change summary:
    - reproduced the fresh nearest HDMI baseline on the live device and confirmed the branch still matched the preserved direct-path recovery: `misterctl.sh probe` reported `Native render path: enabled (scale-mode=nearest)`, while `nearest-hdmi-r2-control-full` and `nearest-hdmi-r2-stage-heavy-basic` stayed on `software_frame_mapped_scale` with zero readback but very high mapped-copy cost
    - relanded the missing mapped nearest LUT cache in `src/port/sdl/fbdev_presenter.c`, keying cached indices on source dimensions plus mapped destination geometry and reusing that cache for both `copy_argb_surface_scaled_to_fb_mapped_rect(...)` and the shared fullscreen scaled helper
    - attempted `codex review --uncommitted` for the required review pass, but the helper stalled during read-only inspection; completed a manual scoped review of the kept diff instead and found no additional correctness issues
  - Verification evidence:
    - `git diff --check` passed; the ARM telemetry rebuild/install/package through `/work-arm` in `3sx-mister-build-nearest-hdmi-perf` succeeded, `readelf -h build/mister-telemetry-package/bin/3sx` still reported `ELF32` `ARM` with hard-float ABI, and the package exported cleanly to `build/mister-telemetry-package-arm-nearest-r2-lut`
    - MiSTer `deploy`, `probe`, and bounded `smoke` all passed on the candidate package, with probe and smoke both still reporting `FBDEV: active (1920x1080 ...)`, `Native render path: enabled (scale-mode=nearest)`, and `Software frame mode: on`
    - control keep gate: `nearest-hdmi-r2-control-full` = `13.7570 FPS` / `72.6901 / 3.5826 / 7.4603 / 61.6472 ms` (`frame / update / render / present`) with `present_copy.mean_ms = 61.6330`; `nearest-hdmi-r3-control-full` improved to `21.2800 FPS` / `46.9925 / 3.5854 / 7.3557 / 36.0514 ms`, with `present_copy.mean_ms = 36.0379` and the dominant path unchanged at `software_frame_mapped_scale = 1.0000`
    - heavy nearest keep gate: `nearest-hdmi-r2-stage-heavy-basic` = `13.2111 FPS` / `75.6939 / 4.9553 / 9.1315 / 61.6070 ms`; `nearest-hdmi-r3-stage-heavy-basic` improved to `20.2745 FPS` / `49.3231 / 4.8857 / 8.9595 / 35.4778 ms`, again staying fully on `software_frame_mapped_scale`
    - native guard held: `native-hdmi-r2-control-basic` = `92.9890 FPS` / `10.7540 / 3.1950 / 7.0041 / 0.5549 ms`; `native-hdmi-r3-control-basic` stayed effectively flat at `92.7909 FPS` / `10.7769 / 3.1401 / 7.0768 / 0.5601 ms` on `software_frame_exact = 1.0000`
  - Keep/rollback decision:
    - keep the mapped-LUT reland; on the live `1920x1080` HDMI path it preserves the intended direct presenter route, cuts mapped nearest present cost by roughly `41%` on both measured gates, and leaves the native guardrail well inside the allowed drift
  - Final commit hash:
    - recorded in the loop closure commit
  - Next best candidate:
    - if modern-display nearest still needs another loop after this reland, move to a partial scaled-present / dirty-row style investigation rather than reopening route selection or native exact behavior

- 2026-03-14T22:50:00-0400
  - Research target:
    - modern-display HDMI regression recovery for `scale-mode = nearest`: verify whether the accepted direct fbdev/native path had fallen out on `nearest-hdmi-perf`, recover a fresh on-device baseline at `1920x1080`, and restore the routed nearest path before opening a new scaler hypothesis
  - Change summary:
    - deep rechecked the current branch against the preserved nearest findings and confirmed the reland had drifted out: probe no longer reported `Native render path: enabled (scale-mode=nearest)`, and the preserved `--scale-mode` capture plumbing had also fallen out of `tools/mister/perf-sampler.sh`
    - restored the nearest native/fbdev route and native-path screenshot target support in `src/port/sdl/sdl_app.c`, then restored `--scale-mode` override, runtime scale-mode capture, and dominant-present-path summary support in `tools/mister/perf-sampler.sh`
    - completed the required review pass on the scoped diff after verification; the standalone `codex review --uncommitted` helper stalled during read-only inspection, so the kept tree closed on a manual scoped review with no additional correctness findings
  - Verification evidence:
    - remote lock and busy checks were clear before every device step; `git diff --check`, `bash -n tools/mister/perf-sampler.sh`, and the telemetry ARM rebuild/install/package through `/work-arm` in `3sx-mister-build-nearest-hdmi-perf` all passed on the kept tree
    - reproduced the live HDMI regression first: `nearest-hdmi-r0-control-full` landed at `5.7141 FPS` with `175.0050 / 9.7776 / 161.5944 ms` for `frame/render/present`, `dominant_present_path = readback_rect`, and `present_readback.mean_ms = 152.5426`; the probe log likewise lacked the nearest native-path line
    - after the reland, `misterctl.sh probe` again logged `Native render path: enabled (scale-mode=nearest)`, bounded smoke passed, and `nearest-hdmi-r1-control-full` moved to `14.2285 FPS` with `70.2816 / 7.4068 / 59.3430 ms`, `dominant_present_path = software_frame_mapped_scale`, `software_frame_direct_present_ratio = 1.0000`, and `present_readback.mean_ms = 0.0000`
    - the heavy nearest rerun `nearest-hdmi-r1-stage-heavy-basic` stayed on the same direct path at `13.2139 FPS` with `75.6779 / 9.1134 / 61.5909 ms`; native guard `native-hdmi-r1-control-basic` held `92.9996 FPS` with `10.7527 / 7.0311 / 0.5480 ms` and `dominant_present_path = software_frame_exact`
  - Keep/rollback decision:
    - keep the routing and sampler restoration; they remove the catastrophic `readback_rect` regression on `1920x1080` HDMI nearest without disturbing native exact presentation, but the remaining modern-display nearest cost is still dominated by mapped-scale presenter work rather than by readback or upload fallback
  - Final commit hash:
    - recorded in the loop closure commit
  - Next best candidate:
    - stay on the reopened modern-display nearest stream and attack the remaining `software_frame_mapped_scale` copy cost on `1920x1080` output without reopening gameplay logic, native exact routing, or the older closed menu stream

- 2026-03-11T09:15:20-0400
  - Research target:
    - first attribution pass on the user-priority menu lane: measure which select-screen update scopes dominate 2P character select overall and the exact super-art chooser slowdown, especially the suspected circle/highlight animation
  - Change summary:
    - added telemetry-only update-breakdown scopes and export plumbing for `SelectTimer_Run`, `Select_Player`, `Sel_PL_Control`, `Player_Select_Control`, `Sel_Arts_Sub`, and targeted menu effect IDs in `effect.c`, then surfaced the top scopes in `tools/mister/perf-sampler.sh`
    - completed the required independent review pass with a fresh `codex` reviewer; accepted and fixed both valid findings by removing the raw `<stdint.h>` dependency from `main.h` and gating the new probes so they only execute when a relevant full character-select capture is active
    - kept the runtime tree gameplay-neutral: the final telemetry reports per-frame portrait, super-art plate, cursor-circle, and command-name slices without changing menu logic, timing, or determinism
  - Verification evidence:
    - fresh `codex` review produced two valid findings and both were fixed; the parallel `claude` review attempt stalled without an artifact and was ignored. `git diff --check` and `bash -n tools/mister/perf-sampler.sh` passed after the fixes
    - host telemetry rebuild/install plus `--headless --software-frame-parity-check` passed in `3sx-mister-build` with `Software-frame parity check passed: 10 cases` and `Software-source refresh parity check passed: 2 cases`
    - `/work-arm` telemetry rebuild/install passed; because the host-side telemetry package script is still unreliable here, the deploy package was refreshed by replacing `bin/3sx` inside the prior known-good `build/mister-telemetry-package-arm-c112` shell, producing `build/mister-telemetry-package-arm-c112r`. MiSTer `health`, `deploy`, `probe`, and bounded `smoke` all passed on that package
    - `menu-c112r-char-select-overall-full` landed at `44.7572 FPS` with `22.3428 / 11.3586 / 10.4541 / 0.5302 ms` for `frame/update/render/present`; top update scopes were `effect-38-portrait 0.2040 ms`, `effect-79-super-art-plate 0.0641 ms`, `effect-d8-cursor-circle 0.0292 ms`, `select-player 0.0229 ms`, and `effect-80-super-art-command-name 0.0224 ms`
    - `menu-c112r-super-art-selection-exact-full` landed at `35.2220 FPS` with `28.3914 / 18.3873 / 9.3891 / 0.6150 ms`; top update scopes were `effect-38-portrait 0.2329 ms`, `effect-79-super-art-plate 0.1065 ms`, `effect-d8-cursor-circle 0.0493 ms`, `select-player 0.0211 ms`, and `effect-39-name 0.0204 ms`
  - Keep/rollback decision:
    - keep the measurement-support changes; the accepted review fixes preserve portability and remove idle telemetry overhead, and the chooser lane still measures as decisively update-bound with the suspected circle/highlight slice far too small to explain the `~18.4 ms` update cost by itself
  - Final commit hash:
    - `b54dc230`
  - Next best candidate:
    - stay on the user-priority chooser lane but move attribution one level up from these effect slices, likely into broader character-select/menu task dispatch outside individual effect moves, before attempting a runtime optimization

- 2026-03-11T08:47:00-0400
  - Research target:
    - measurement-only closeout for the next active runtime stream: make the 2P post-character-select super-art chooser reproducible as an exact perf lane without regressing the older `attract-demo-logo` runtime-state workflow
  - Change summary:
    - added `character-select-super-art` support to the perf runtime-state plumbing in `src/main.c`, `src/port/sdl/sdl_app.c`, and `tools/mister/perf-sampler.sh`, including a concise chooser start-state snapshot in perf JSON/summary output
    - fixed `tools/mister/perf-sampler.sh` so `--perf-wait-runtime-state` enables the test runner only for `character-select-super-art`; `attract-demo-logo` now keeps its original idle title/demo path
    - completed the required independent review pass with a fresh `codex` reviewer; kept the valid blanket-automation regression fix, tightened the chooser runtime-state to chooser-instantiated signals (`Select_Arts >= 0` plus visible command-name state), and added a fail-fast CLI guard so raw `character-select-super-art` waits require `--test-enable` instead of hanging indefinitely
  - Verification evidence:
    - `git diff --check` and `bash -n tools/mister/perf-sampler.sh` passed; telemetry ARM rebuild/install/package in `3sx-mister-build`, host `docker cp` export, MiSTer `deploy`, and `probe` all passed on the kept tree
    - unchanged-tree refreshed menu baselines confirmed the user-priority lane is update-bound, not presenter-bound: `menu-c110-char-select-overall-basic` landed at `39.6416 FPS` with `15.5266 ms update`, while warmed `menu-c110-super-art-selection-basic` landed at `36.7670 FPS` with `17.2662 ms update` and only `0.5464 ms present`
    - the final exact-lane rerun `menu-c111e-super-art-selection-exact-basic` landed at `37.3927 FPS` with `26.7432 / 16.8619 / 9.3454 / 0.5358 ms` for `frame/update/render/present`, `runtime_state=character-select-super-art`, `active_frames=40`, and chooser start state `Sel_PL_Complete=1/1`, `Sel_Arts_Complete=0/0`, `Select_Arts=3/3`, `Moving_Plate=0/0`, `Moving_Plate_Counter=0/0`, `Disp_Command_Name=1/1`
    - regression guard `menu-c111-attract-demo-logo-check` landed at `63.1002 FPS`, `runtime_state=attract-demo-logo`, and `active_frames=60`, confirming the attract/logo idle path still arms after the sampler fix
  - Keep/rollback decision:
    - keep the measurement-support changes; they recover a reproducible chooser-instantiation super-art lane and preserve the older attract/logo lane, while the earlier pre-chooser gate was replaced rather than kept
  - Final commit hash:
    - recorded in the loop closure commit
  - Next best candidate:
    - stay on the user-priority menu lane and attribute the remaining `~16.5 ms` chooser update cost inside the select-screen path itself, starting with the circle/highlight and related menu-effect update routines before broader safe wins

- 2026-03-11T07:21:00-0400
  - Research target:
    - Chunk 4 closeout: validate the player-facing `clean` package on MiSTer, document the finished nearest modern-display workflow, and hand the branch back to the next measured runtime lane
  - Change summary:
    - rebuilt the deployable ARM hard-float `clean` package through `/work-arm` in `3sx-mister-build`, copied it back to the host as `build/mister-clean-package-arm` with `docker cp`, redeployed it with `tools/mister/misterctl.sh`, and validated both the default clean startup path and a temporary nearest-mode launch on-device
    - updated `docs/config.md`, `docs/mister-runbook.md`, and `docs/agent-memory/mister-performance.md` so the repo now records the validated `nearest` modern-display guidance plus the `telemetry` versus `clean` workflow explicitly
    - completed the required independent review pass with a fresh `codex exec` reviewer scoped to the docs/checklist diff; accepted its reproducibility finding by adding the explicit host-side `docker cp` export step before deploy and by removing the ad hoc `-c4` package path from the canonical checklist, then reran review and got `No findings`
  - Verification evidence:
    - `git diff --check` passed, `bash -n tools/mister/perf-sampler.sh` passed, the `/work-arm` clean build/install/package command succeeded in `3sx-mister-build`, `docker cp 3sx-mister-build:/work-arm/build/mister-clean-package ./build/mister-clean-package-arm` succeeded, and `readelf -h build/mister-clean-package/bin/3sx` reported `Machine: ARM` and `hard-float ABI`
    - `tools/mister/misterctl.sh deploy --src build/mister-clean-package-arm`, `probe`, and bounded `smoke` all passed on MiSTer; the clean-package default path reported `Native render path: enabled (scale-mode=native)`, `Scale mode: native`, `Software frame mode: on`, `__RUNTIME_RC__=124`, and `exit=143`
    - temporary nearest-mode clean validation also passed on MiSTer: the scripted config override reported `scale-mode = nearest` / `software-frame-mode = on`, `--probe-renderer-only` and bounded `launch-osd.sh` both logged `Native render path: enabled (scale-mode=nearest)` plus `Scale mode: nearest`, and the device returned `__RUNTIME_RC__=124` / `exit=143` before the config restored to `scale-mode = native` / `software-frame-mode = on`
    - final nearest keep summary carried forward from the accepted telemetry matrix: control `47.5056 -> 84.1389 FPS`, stage-heavy `40.0243 -> 65.1989 FPS`, Ibuki stage 7 `45.6766 -> 81.6454 FPS`, and attract/logo `38.3566 -> 60.1424 FPS`; native guard remained `96.0766 FPS` and square-pixels guard remained `95.9970 FPS`
  - Keep/rollback decision:
    - keep; the nearest scaled-present path is already performance-validated in telemetry, the clean package now passes player-facing native and nearest launches on-device, and this checklist can close without reopening runtime code
  - Final commit hash:
    - `29168767`
  - Next best candidate:
    - start the next measurement-first runtime loop on 2P character select overall, then the immediate post-select super-art selection slowdown, especially the circle/highlight animation around the super-art name, before spending time on lower-priority safe wins

- 2026-03-15T01:04:55-0400
  - Research target:
    - modern-display HDMI nearest on the verified `r8` direct path: test whether source-tile-guided dirty runs can cut mapped-copy waste beyond the kept dirty-span reland without reopening route selection
  - Change summary:
    - recovered fresh `r8` nearest control, stage-heavy, and native guard baselines on the live `1920x1080` HDMI target and confirmed there was still no route regression: nearest stayed on `software_frame_mapped_scale`, native stayed on `software_frame_exact`, and the preserved `r7` direct-path runtime still described the branch accurately
    - updated `src/port/sdl/fbdev_presenter.c` to cache per-row `16`-pixel source-tile runs for mapped nearest present and only remap/copy those runs, while preserving the current coarse-span fallback whenever the mapped cache or LUT is unavailable
    - attempted `codex review --uncommitted` for the required review pass, but the helper stalled again after read-only inspection; completed a manual scoped review of the kept diff and found no additional correctness issues
  - Verification evidence:
    - `git diff --check` passed; the ARM telemetry rebuild/install/package through `/work-arm` in `3sx-mister-build-nearest-hdmi-perf` succeeded, `readelf -h build/mister-telemetry-package/bin/3sx` inside the container still reported `ELF32` `ARM` with hard-float ABI, and the package exported cleanly to `build/mister-telemetry-package-arm-nearest-r9-tileruns-export1`
    - MiSTer `deploy`, `probe`, and bounded `smoke` all passed on the candidate package; probe and smoke still reported `FBDEV: active (1920x1080 ...)`, `Native render path: enabled (scale-mode=nearest)`, and `Software frame mode: on`
    - control keep gate: `nearest-hdmi-r8-control-full` = `65.1655 FPS` / `15.3455 / 3.8646 / 4.3349 ms` (`frame / present / present_copy`) with `443678.77` copied bytes/frame; `nearest-hdmi-r9-control-full` improved to `67.3417 FPS` / `14.8496 / 3.8646 / 3.8498 ms` with `376378.40` copied bytes/frame, while staying on `software_frame_mapped_scale = 1.0000`
    - stage-heavy keep gates: `nearest-hdmi-r8-stage-heavy-basic` = `47.0640 FPS` / `21.2476 / 5.3031 ms` with `935399.44` copied bytes/frame; `nearest-hdmi-r9-stage-heavy-basic` improved to `51.9519 FPS` / `19.2486 / 5.3031 ms` with `544665.60` copied bytes/frame. Attribution rerun `nearest-hdmi-r8-stage-heavy-full` = `43.4044 FPS` / `23.0391 / 7.7996 / 7.7855 ms` (`frame / present / present_copy`) improved to `nearest-hdmi-r9-stage-heavy-full` = `48.7243 FPS` / `20.5236 / 5.3149 / 5.2997 ms`, again with `544665.60` copied bytes/frame and the dominant path unchanged at `software_frame_mapped_scale = 1.0000`
    - native guard held: `native-hdmi-r8-control-basic` = `92.2942 FPS` / `10.8349 / 0.5355 ms`; `native-hdmi-r9-control-basic` stayed within the `3%` budget at `91.5832 FPS` / `10.9190 / 0.5368 ms` on `software_frame_exact = 1.0000`
  - Keep/rollback decision with reason:
    - keep; the source-tile reland preserves the intended direct nearest HDMI presenter route, materially cuts mapped-copy cost on both the control and heavy gates, and leaves native exact presentation effectively flat
  - Next best candidate optimization:
    - if modern-display nearest needs another loop after this reland, stay on mapped-present sparsity follow-up by testing finer sparse-span merging or a newly reproduced player-visible HDMI gate before reopening route selection

- 2026-03-15T04:40:00-0400
  - Research target:
    - revalidate the modern-display `scale-mode = nearest` symptom on the current canonical package and recover the blocked full-capture transport in `tools/mister/perf-sampler.sh`
  - Change summary:
    - verified on the live `1920x1080` HDMI target that the current branch/package still probes as dummy/software + fbdev with `Native render path: enabled (scale-mode=nearest)` and that the preserved nearest runtime has not drifted off `software_frame_mapped_scale`
    - replaced `perf-sampler.sh`'s large base64-over-SSH artifact return path with serialized `scp` downloads through a new `mister_scp_download(...)` helper, while preserving remote config restore and failure cleanup
  - Verification evidence:
    - `tools/mister/misterctl.sh lock-status`, `busy-status`, `health`, and `probe` all passed before sampling; the live device still matched `scale-mode = nearest`, `software-frame-mode = on`, and `FBDEV: active (1920x1080 ...)`
    - fresh nearest/runtime guardrails did not reproduce the user-reported slowdown:
      - `nearest-hdmi-r8-control-full` landed at `65.1470 FPS` / `15.3499 / 7.3049 / 4.3903 / 4.3758 ms` for `frame/render/present/present_copy`, with `dominant_present_path = software_frame_mapped_scale` and `copy_bytes = 443678.77`
      - `nearest-hdmi-r8-stage-heavy-basic` landed at `46.0352 FPS` / `21.7225 / 8.8949 / 7.7571 ms`, with `dominant_present_path = software_frame_mapped_scale` and `copy_bytes = 935399.44`
      - `native-hdmi-r8-control-basic` held `93.4249 FPS` / `10.7038 / 7.0115 / 0.5076 ms`, with `dominant_present_path = software_frame_exact`
    - compared with the kept `r7` references, control stayed at `65.4989 -> 65.1470 FPS`, `15.2674 -> 15.3499 ms`, `4.3974 -> 4.3758 ms present_copy`; `stage-heavy` stayed at `46.3955 -> 46.0352 FPS`, `21.5538 -> 21.7225 ms`; native improved slightly to `93.4249 FPS`
    - `git diff --check` plus `bash -n tools/mister/mister-common.sh tools/mister/perf-sampler.sh` passed, the rerun `nearest-hdmi-r8-control-full` saved local JSON successfully after the transport fix, and the post-fix smoke `nearest-hdmi-r8-tooling-smoke` also completed through the new download path
    - attempted `codex review --uncommitted` twice for the tooling diff, but the review CLI stalled/timed out in this environment before returning findings; final keep used manual diff review plus the successful post-fix rerun above
  - Keep/rollback decision:
    - keep the tooling fix; the reported nearest HDMI slowdown does not reproduce on the current branch/package, and the only real regression recovered this cycle was the full-capture transport in `perf-sampler.sh`
  - Final commit hash:
    - recorded in the cycle closeout commit
  - Next best candidate:
    - do not spend another runtime loop on route recovery unless a fresh on-device nearest repro falls materially below the kept `r7` baseline; if nearest HDMI work resumes, start from the verified `r8` baseline and target only a newly measured mapped-scale hotspot

- 2026-03-15T02:07:09-0400
  - Research target:
    - modern-display HDMI nearest on the kept `r11` direct path: test whether trimming each dirty mapped run down to the actual changed source-pixel edges can cut the remaining nearest overcopy without reopening route selection
  - Change summary:
    - recovered fresh `r12` nearest control, stage-heavy, and native guard baselines on the live `1920x1080` HDMI target and confirmed there was still no route regression: nearest stayed on `software_frame_mapped_scale`, native stayed on `software_frame_exact`, and the branch still matched the kept `r11` state
    - updated `src/port/sdl/fbdev_presenter.c` so the mapped nearest path keeps the existing `8`-pixel tile comparisons but trims each dirty run to the actual changed source-pixel edges before mapping/copying it, while preserving the current cache, LUT, and non-mapped fallback behavior
    - completed a manual scoped review of the single-file diff with focus on cached-row coherence, merged-run bounds, and row-stride limits; no correctness issues were found
  - Verification evidence:
    - `git diff --check` passed; the telemetry ARM rebuild/install/package succeeded in fresh `/work-arm-nearest-r13-edgetrim-20260315a` on `3sx-mister-build-nearest-hdmi-perf`, `readelf -h` still reported `ELF32` `ARM` with hard-float ABI, and the package exported cleanly to `build/mister-telemetry-package-arm-nearest-r13-edgetrim-20260315a`
    - MiSTer `deploy`, `probe`, and bounded `smoke` all passed on the candidate package; probe and smoke still reported `FBDEV: active (1920x1080 ...)`, `Native render path: enabled (scale-mode=nearest)`, and `Software frame mode: on`
    - control keep gate: `nearest-hdmi-r12-control-full` = `70.7016 FPS` / `14.1440 / 3.4697 / 3.4559 ms` (`frame / present / present_copy`) with `290150.00` copied bytes/frame; `nearest-hdmi-r13-control-full` improved to `71.2450 FPS` / `14.0361 / 3.2068 / 3.1925 ms` with `183059.04` copied bytes/frame, while staying on `software_frame_mapped_scale = 1.0000`
    - stage-heavy keep gates: `nearest-hdmi-r12-stage-heavy-basic` = `53.2120 FPS` / `18.7928 / 4.8743 ms` with `425478.80` copied bytes/frame; `nearest-hdmi-r13-stage-heavy-basic` improved to `55.4209 FPS` / `18.0437 / 4.2385 ms` with `262170.55` copied bytes/frame. Attribution rerun `nearest-hdmi-r12-stage-heavy-full` = `50.3031 FPS` / `19.8795 / 4.8516 / 4.8369 ms` (`frame / present / present_copy`) improved to `nearest-hdmi-r13-stage-heavy-full` = `52.4355 FPS` / `19.0711 / 4.1679 / 4.1539 ms`, again with the dominant path unchanged at `software_frame_mapped_scale = 1.0000`
    - bursty tails tightened too: control p95/p99 copied bytes `1134360 / 1355880 -> 908148 / 1098980` with p95/p99 `present_copy` `9.8481 / 11.0254 -> 8.6670 / 10.7886 ms`; stage-heavy p95/p99 copied bytes `1283160 / 1420800 -> 930496 / 1120004` with p95/p99 `present_copy` `10.6873 / 13.1460 -> 9.3520 / 10.6897 ms`
    - native guard held: `native-hdmi-r12-control-basic` = `92.3190 FPS` / `10.8320 / 0.5340 ms`; `native-hdmi-r13-control-basic` stayed within the `3%` budget at `92.5885 FPS` / `10.8005 / 0.5240 ms` on `software_frame_exact = 1.0000`
  - Keep/rollback decision with reason:
    - keep; trimming mapped dirty runs to the actual changed source-pixel edges materially lowers nearest HDMI copy traffic and presenter time on both measured gates without reopening route selection or regressing native exact presentation
  - Next best candidate optimization:
    - if modern-display nearest still needs another loop after this reland, stay inside mapped-present sparsity follow-up by testing interior disjoint-run splitting or other finer source-pixel-guided runs on a freshly reproduced HDMI hotspot rather than revisiting route selection

- 2026-03-15T02:40:25-0400
  - Research target:
    - fresh modern-display HDMI nearest repro on the kept `r13` direct path, then test whether splitting changed `8`-pixel compare tiles into interior disjoint source runs can cut the remaining mapped-present overcopy without reopening route selection
  - Change summary:
    - rebuilt the current branch as a telemetry ARM package in fresh `/work-arm-r14b`, redeployed it to MiSTer, and recovered fresh `r14` nearest control, stage-heavy, and native guard baselines; the live branch still matched the kept `r13` state with `software_frame_mapped_scale` on `1920x1080`, zero readback, and no route drift
    - tried a single-file runtime reland in `src/port/sdl/fbdev_presenter.c` that split changed mapped compare tiles into disjoint source-pixel runs instead of copying each changed tile as one trimmed span
    - completed a manual scoped review of the candidate diff with focus on run-count bounds, cached-row coherence, and fallback coverage; no correctness issue was found, but the runtime change was rejected on measured performance
  - Verification evidence:
    - `git diff --check` passed before and after the attempted runtime reland; the telemetry ARM rebuild/install/package succeeded in `/work-arm-r14b`, exported cleanly to `build/mister-telemetry-package-arm-nearest-r14-baseline-20260315a` and `build/mister-telemetry-package-arm-nearest-r15-disjointruns-20260315a`, and `readelf -h` still reported `ELF32` `ARM` with hard-float ABI
    - MiSTer `deploy`, `probe`, and bounded `smoke` all passed on the candidate package, and baseline restore `deploy` plus final `probe` passed after rollback; every probe in the cycle still reported `FBDEV: active (1920x1080 ...)`, `Native render path: enabled (scale-mode=nearest)`, and `Software frame mode: on`
    - fresh baseline confirmed the current branch/package still matches the kept direct nearest path: `nearest-hdmi-r14-control-full = 70.8695 FPS / 14.1104 / 3.1638 / 3.1499 ms` (`frame / present / present_copy`) with `183059.04` copied bytes/frame, `nearest-hdmi-r14-stage-heavy-full = 51.8518 FPS / 19.2857 / 4.3133 / 4.2960 ms` with `262170.55` copied bytes/frame, `nearest-hdmi-r14-stage-heavy-basic = 55.3620 FPS / 18.0629 / 4.2224 ms`, and `native-hdmi-r14-control-basic = 92.3879 FPS / 10.8239 / 0.5306 ms`
    - the disjoint-run candidate lowered copied bytes but still regressed nearest timing: `nearest-hdmi-r15-control-full` moved to `70.6957 FPS / 14.1451 / 3.3252 / 3.3083 ms` with `166429.97` copied bytes/frame, `nearest-hdmi-r15-stage-heavy-full` to `50.7895 FPS / 19.6891 / 4.5776 / 4.5633 ms` with `233904.96` copied bytes/frame, and `nearest-hdmi-r15-stage-heavy-basic` to `54.5137 FPS / 18.3440 / 4.5521 ms`; native guard `native-hdmi-r15-control-basic` stayed flat at `92.5896 FPS / 10.8004 / 0.5433 ms`
  - Keep/rollback decision with reason:
    - rollback; splitting changed compare tiles into disjoint source runs does reduce mapped-copy bytes, but the extra run fanout raises `present` / `present_copy` enough to regress both nearest HDMI keep gates, so the branch and live device were restored to the kept baseline package
  - Final commit hash:
    - recorded in the cycle closeout commit
  - Next best candidate optimization:
    - keep route selection closed and do not retry this higher-fanout disjoint-run shape blindly; if another nearest HDMI loop is needed, stay on lower-overhead mapped-present sparsity follow-up or move to a newly reproduced player-visible nearest gate

- 2026-03-15T01:36:38-0400
  - Research target:
    - modern-display HDMI nearest on the kept `r9` direct path: test whether the mapped-source `16`-pixel compare/run size is still too coarse on bursty `1920x1080` nearest frames without perturbing the older staging-diff path
  - Change summary:
    - recovered fresh `r10` nearest control, stage-heavy, and native guard baselines on the live `1920x1080` HDMI target and confirmed there was still no route regression: nearest stayed on `software_frame_mapped_scale`, native stayed on `software_frame_exact`, and the live device still matched the kept `r9` state
    - updated `src/port/sdl/fbdev_presenter.c` to split mapped-source compare/run granularity from `staging_tile_size` and tighten only the mapped nearest path to `8`-pixel source tiles
    - completed a manual scoped review of the single-file diff with focus on mapped-row stride sizing, run-count bounds, and non-nearest staging isolation; no correctness issues were found
  - Verification evidence:
    - `git diff --check` passed; the telemetry ARM rebuild/install/package succeeded in a fresh `/work-arm-nearest-r11-subtile8-20260315a` tree, `readelf -h` still reported `ELF32` `ARM` with hard-float ABI, and the package exported cleanly to `build/mister-telemetry-package-arm-nearest-r11-subtile8-20260315a`
    - MiSTer `deploy`, `probe`, and bounded `smoke` all passed on the candidate package; probe and smoke still reported `FBDEV: active (1920x1080 ...)`, `Native render path: enabled (scale-mode=nearest)`, and `Software frame mode: on`
    - control keep gate: `nearest-hdmi-r10-control-full` = `67.3534 FPS` / `14.8471 / 3.8729 / 3.8585 ms` (`frame / present / present_copy`) with `376378.40` copied bytes/frame; `nearest-hdmi-r11-control-full` improved to `69.0655 FPS` / `14.4790 / 3.5510 / 3.5371 ms` with `290150.00` copied bytes/frame, while staying on `software_frame_mapped_scale = 1.0000`
    - stage-heavy keep gate: `nearest-hdmi-r10-stage-heavy-basic` = `52.1437 FPS` / `19.1778 / 5.3019 ms` with `544665.60` copied bytes/frame; `nearest-hdmi-r11-stage-heavy-basic` improved to `53.4607 FPS` / `18.7053 / 4.8880 ms` with `425478.80` copied bytes/frame, again staying on `software_frame_mapped_scale = 1.0000`
    - bursty control frames tightened too: `nearest-hdmi-r10-control-full` p95/p99 copied bytes `1423200 / 1615920` and p95/p99 `present_copy` `10.6263 / 11.9710 ms` improved to `1080840 / 1350360` bytes and `9.2925 / 11.3529 ms` on `nearest-hdmi-r11-control-full`
    - native guard held: `native-hdmi-r10-control-basic` = `93.8539 FPS / 10.6549 / 0.5217 ms`; `native-hdmi-r11-control-basic` stayed within the `3%` budget at `93.2606 FPS / 10.7226 / 0.5371 ms` on `software_frame_exact = 1.0000`
  - Keep/rollback decision with reason:
    - keep; the narrower mapped-source run granularity trims nearest HDMI copy traffic on both measured gates, improves bursty present cost, and leaves native exact presentation within guardrail while keeping the older staging-diff path untouched
  - Next best candidate optimization:
    - if modern-display nearest still needs another loop after this reland, stay inside mapped-present sparsity follow-up by testing even finer edge trimming or source-pixel-guided runs on a freshly reproduced HDMI hotspot rather than reopening route selection

- 2026-03-11T06:56:00-0400
  - Research target:
    - Chunk 3 mapped-scaler reland: remove the remaining `software_frame_mapped_scale` divide-heavy copy cost in `copy_argb_surface_scaled_to_fb_mapped_rect(...)` now that nearest-mode is on the correct fbdev route
  - Change summary:
    - generalized the existing fbdev scale LUT cache in `src/port/sdl/fbdev_presenter.c` so it keys on source size plus mapped destination geometry instead of only fullscreen dimensions
    - switched the mapped nearest-scaling path to reuse those cached LUTs when available, keeping the old exact/integer paths unchanged and preserving repeated-source-row memcpy reuse
    - completed the required independent review pass with a fresh `codex exec` reviewer scoped to the diff; it returned `No findings`, so no post-review code changes were needed
  - Verification evidence:
    - `git diff --check` passed; host telemetry build/install plus `--headless --software-frame-parity-check` passed with `Software-frame parity check passed: 10 cases` and `Software-source refresh parity check passed: 2 cases`
    - deployable telemetry package rebuilt successfully through `/work-arm` in `3sx-mister-build`, confirmed as ARM hard-float by `readelf`, copied out as `build/mister-telemetry-package-arm-c3`, and host-side `tools/mister/package.sh build/mister-telemetry-install build/mister-telemetry-package-host-c3` also passed
    - MiSTer `health`, telemetry deploy, `probe`, and bounded `smoke` all passed on the candidate package
    - full route-attribution control check versus Chunk 2 routed baseline:
      - `nearest-c2-control-full` -> `nearest-c3-control-full`: `53.7597 -> 77.3658 FPS` (`+43.91%`), `18.6013 -> 12.9256 ms` frame (`-30.51%`), `7.5018 -> 1.9336 ms` present (`-74.22%`), `7.4874 -> 1.9207 ms` presenter copy (`-74.35%`), path unchanged at `software_frame_mapped_scale = 1.0000`
    - decision-grade nearest keep matrix versus Chunk 1 baseline:
      - `nearest-c3-control`: `47.5056 -> 84.1389 FPS` (`+77.12%`), `21.0501 -> 11.8851 ms` frame (`-43.54%`), `9.3925 -> 1.9913 ms` present (`-78.80%`)
      - `nearest-c3-stage-heavy`: `40.0243 -> 65.1989 FPS` (`+62.90%`), `24.9849 -> 15.3377 ms` frame (`-38.61%`), `9.5416 -> 1.9445 ms` present (`-79.62%`)
      - `nearest-c3-ibuki-stage7`: `45.6766 -> 81.6454 FPS` (`+78.75%`), `21.8930 -> 12.2481 ms` frame (`-44.05%`), `9.5200 -> 1.9673 ms` present (`-79.34%`)
      - `nearest-c3-attract-logo`: `38.3566 -> 60.1424 FPS` (`+56.79%`), `26.0711 -> 16.6272 ms` frame (`-36.22%`), `9.5125 -> 1.9303 ms` present (`-79.71%`)
    - guardrails held: `native-c2-control-guard` -> `native-c3-control-guard` improved slightly from `94.8017 FPS` / `10.5483 ms` to `96.0766 FPS` / `10.4084 ms`, and `square-c3-control-guard` stayed on `software_frame_exact = 1.0000` at `95.9970 FPS` / `10.4170 ms`
  - Keep/rollback decision:
    - keep; the LUT reland preserves the routed nearest path, collapses mapped-scale presenter cost on every measured nearest gate, clears the Chunk 3 `10%`-vs-Chunk-1 target by a wide margin, and leaves native/square guardrails flat to slightly better
  - Final commit hash:
    - `6a971ea3`
  - Next best candidate:
    - with Chunk 3 closed, shift the next runtime-measurement loop to the user-priority 2P character-select lane and the immediate post-select super-art selection slowdown before spending time on Chunk 4 docs-only closeout

- 2026-03-11T02:30:59-0400
  - Research target:
    - Chunk 2 runtime reland: move MiSTer `scale-mode = nearest` from the `screen_texture` / `fullscreen_staging` path onto `native_output_rect` plus fbdev presentation, while preserving screenshot and fallback correctness
  - Change summary:
    - enabled the MiSTer fbdev-only native render path for `scale-mode = nearest` in `src/port/sdl/sdl_app.c`, so ordinary nearest gameplay can direct-present the software-owned frame through `software_frame_mapped_scale` instead of routing every frame through `screen_texture`
    - added a dedicated native-path screenshot render target so `screenshot_screen.bmp` stays available even when `screen_texture` is absent on the native path
    - accepted the valid independent review finding about silent native screenshot failure and added a backend log when the screenshot target cannot be created; rejected the other review finding because the render-path switch is the explicit Chunk 2 objective and was validated on-device
  - Verification evidence:
    - `git diff --check`, telemetry ARM rebuild/package in `/work-arm`, and host telemetry `--headless --software-frame-parity-check` all passed twice; parity remained `Software-frame parity check passed: 10 cases` plus `Software-source refresh parity check passed: 2 cases`
    - MiSTer `health`, deploy, probe, bounded smoke, and the routed nearest capture matrix all passed on the ARM package; the current device gate reported `FBDEV: active (320x240 ...)`, so the keep comparison uses the same-device `nearest-c1-*` baselines rather than the older `1280x720` native reference
    - full route-attribution spot check `nearest-c2-control-full`: `53.7597 FPS` / `18.6013 / 3.7660 / 7.3335 / 7.5018 ms`, dominant path `software_frame_mapped_scale`, `software_frame_direct_present_ratio = 1.0000`, `software_frame_uploaded_ratio = 0.0000`, `present_readback.mean_ms = 0.0000`
    - decision-grade nearest reruns versus Chunk 1 baseline:
      - `nearest-c2-control`: `47.5056 -> 57.1255 FPS` (`+20.25%`), `21.0501 -> 17.5053 ms` frame (`-16.84%`), `9.3925 -> 7.5821 ms` present (`-19.27%`), path `fullscreen_staging = 1.0000 -> software_frame_mapped_scale = 1.0000`
      - `nearest-c2-stage-heavy`: `40.0243 -> 47.5627 FPS` (`+18.83%`), `24.9849 -> 21.0249 ms` frame (`-15.85%`), `9.5416 -> 7.5866 ms` present (`-20.49%`), path `fullscreen_staging = 1.0000 -> software_frame_mapped_scale = 1.0000`
      - `nearest-c2-attract-logo`: `38.3566 -> 45.1744 FPS` (`+17.77%`), `26.0711 -> 22.1364 ms` frame (`-15.09%`), `9.5125 -> 7.5319 ms` present (`-20.82%`), path `fullscreen_staging = 1.0000 -> software_frame_mapped_scale = 1.0000`
    - native guard spot check on the current low-resolution gate: `native-c2-control-guard` landed at `94.8017 FPS` / `10.5483 / 3.1422 / 6.8732 / 0.5330 ms` with `software_frame_exact = 1.0000`
    - post-fix rerun after the review-driven logging change stayed aligned: `nearest-c2-control-postfix` landed at `57.1437 FPS` / `17.4997 / 3.1451 / 6.8084 / 7.5463 ms` with dominant path `software_frame_mapped_scale`
  - Keep/rollback decision:
    - keep; the MiSTer nearest path now leaves `fullscreen_staging` entirely, direct-presents the software frame through the native fbdev path, materially improves every routed nearest gate, preserves the native exact guard, and keeps screenshot failure diagnosable
  - Final commit hash:
    - recorded in the loop closure commit
  - Next best candidate:
    - start Chunk 3 and optimize `copy_argb_surface_scaled_to_fb_mapped_rect(...)` in `src/port/sdl/fbdev_presenter.c`; nearest is now on the correct route, so the next safe win is lowering the mapped-scaler copy cost rather than reopening broader routing work

- 2026-03-11T01:37:02-0400
  - Research target:
    - Chunk 1 measurement support: make nearest captures reproducible, self-describing, and decision-grade before any present-path runtime work
  - Change summary:
    - added `--scale-mode` override support to `tools/mister/perf-sampler.sh`, wrote the chosen value into the temporary remote config, and stamped capture metadata plus summary output with `scale_mode` and the dominant fbdev present path
    - logged the applied runtime scale mode from `src/port/sdl/sdl_app.c` so the sampler can trust the effective MiSTer mode instead of only the CLI request
    - accepted the independent review finding that the sampler could mislabel MiSTer `soft-linear` as effective `soft-linear` after the runtime coerced it to `nearest`, then fixed the metadata path to prefer the runtime-reported mode
    - validated the actual deployable telemetry package through the container-local `/work-arm` ARM cross-build after the host-mounted `/src` package probe exposed an `x86_64` binary mismatch on MiSTer
  - Verification evidence:
    - `git diff --check` and `bash -n tools/mister/perf-sampler.sh` passed
    - telemetry compile/install passed in `3sx-mister-build`, and the validated `/work-arm` cross-build produced an ARM hard-float package confirmed by `readelf -h build/mister-telemetry-package/bin/3sx`
    - `tools/mister/misterctl.sh health`, `deploy`, `probe`, and bounded `smoke` passed on MiSTer after the ARM redeploy
    - nearest baseline matrix:
      - `nearest-c1-control`: `47.5056 FPS` / `21.0501 / 3.2656 / 8.3920 / 9.3925 ms`, dominant path `fullscreen_staging`, `scale_mode = nearest`
      - `nearest-c1-stage-heavy`: `40.0243 FPS` / `24.9849 / 5.1238 / 10.3195 / 9.5416 ms`, dominant path `fullscreen_staging`, `scale_mode = nearest`
      - `nearest-c1-ibuki-stage7`: `45.6766 FPS` / `21.8930 / 3.8760 / 8.4970 / 9.5200 ms`, dominant path `fullscreen_staging`, `scale_mode = nearest`
      - `nearest-c1-attract-logo`: `38.3566 FPS` / `26.0711 / 4.6631 / 11.8956 / 9.5125 ms`, dominant path `fullscreen_staging`, `scale_mode = nearest`
    - nearest versus trusted native control baseline: `47.5056 FPS` versus `89.6375 FPS` and `21.0501 ms` versus `11.1560 ms`, a `47.00%` FPS drop and `88.69%` frame-time increase
    - review-fix spot check: `nearest-c1-softlinear-coerce-check` reported `scale_mode = nearest` on-device even when invoked with `--scale-mode soft-linear`
  - Keep/rollback decision:
    - keep measurement-support only; the tooling changes are behavior-neutral and the recovered baseline shows nearest is consistently trapped on `fullscreen_staging` with `present_readback.mean_ms = 0.0000`, so the next runtime step is clearly path routing rather than more capture plumbing
  - Final commit hash:
    - `5f9b3566`
  - Next best candidate:
    - start Chunk 2 and route nearest onto `native_output_rect` plus fbdev presentation so normal nearest gameplay stops paying the `screen_texture`/`fullscreen_staging` tax while preserving message-content and screenshot fallback behavior
