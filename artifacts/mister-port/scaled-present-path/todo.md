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
