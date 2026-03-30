# Implementation Todo

## Todo Metadata

- [x] Canonical todo path: `artifacts/mister-port/hps-fb-yc-fix/todo.md`
- [x] Active feature/phase: `mister-port / hps-fb-yc-fix`
- [x] Stale todo files to retire: none identified; existing `artifacts/mister-port/*/todo.md` files appear to be separate workstreams, not duplicates

## Goal and Success Criteria

- [ ] Goal: restore color on native analog S-Video/CVBS launches when 3SX is using the HPS framebuffer path by adding YC encoding on the framebuffer/scaler pipeline and matching the YC phase calculation to the output pixel clock.
- [ ] Success criteria:
  - [x] `vendor/Menu_MiSTer/sys/sys_top.v` routes framebuffer-native analog output through a dedicated `yc_out` path guarded by `MISTER_DISABLE_YC`.
  - [x] `vendor/Main_MiSTer/video.cpp` auto-selects output-clock `PHASE_INC` for native-analog `vga_fb` launches, preserves the existing `yc_modes` phase override path, and logs `clock_source=output-fb-auto`.
  - [x] Local HPS and wrapper-core builds complete and produce fresh `build/mister-wrapper-hps/MiSTer_3SX` and `build/mister-wrapper-core/3SX.rbf`.
  - [ ] Safe wrapper validation on device passes: `probe-wrapper`, `smoke-wrapper`, Menu color regression check, HDMI regression check, and native analog S-Video color check.
  - [x] Waviness in the `384x224 -> 640x240` expansion is unchanged or deferred explicitly; this plan only closes the grayscale bug.

## Scope and Constraints

- [ ] In scope:
  - [x] `vendor/Main_MiSTer/video.cpp` native-analog `PHASE_INC` auto-selection for `vga_fb`.
  - [x] `vendor/Menu_MiSTer/sys/sys_top.v` framebuffer-path YC encoder, DAC mux selection, and subcarrier gating update.
  - [x] Local wrapper-core + HPS artifact builds, wrapper package assembly/check, and safe wrapper deploy/probe/smoke.
  - [ ] Real hardware validation of the restored color path.
- [ ] Out of scope:
  - [ ] Reworking the `384 -> 640` CRT resampling path or revisiting the rejected filtered-presenter experiment.
  - [ ] Broad cleanup of temporary YC diagnostics unless they directly block this implementation.
  - [ ] Non-wrapper runtime packaging changes unrelated to the HPS framebuffer YC fix.
- [ ] Constraints (time, tech, architecture, dependencies):
  - [ ] Real fix requires both sides: HPS `PHASE_INC` auto-selection and wrapper-core framebuffer YC encoding.
  - [ ] Wrapper-core build must use the `menu` seed and the validated Quartus flow from `docs/agent-memory/mister-wrapper-quartus.md`.
  - [ ] Remote MiSTer work must go through `tools/mister/misterctl.sh`, with `lock-status` and `busy-status` checked before deploy/probe/smoke work.
  - [ ] Prefer `deploy-wrapper --artifacts-only` first because this change only affects wrapper-owned artifacts unless a runtime refresh is proven necessary.
  - [ ] Current worktree already has local/untracked documentation changes; do not disturb them while implementing this stream.
- [ ] Assumptions:
  - [ ] The existing plan in `docs/agent-memory/plan-hps-fb-yc-fix.md` is still the intended direction and no newer architecture doc supersedes it.
  - [ ] `build/mister-clean-package` already exists or can be produced separately if wrapper package assembly needs a current runtime tree.
  - [ ] Existing YC tracing remains available long enough to confirm `clock_source=output-fb-auto` during hardware validation.
  - [ ] A real S-Video test target is available for final validation, and CVBS validation is optional if no composite profile is ready.

## Blueprint Summary

- [ ] Phases:
  - [x] Phase 1: preflight current code and land the HPS-side `PHASE_INC` auto-selection.
  - [x] Phase 2: add framebuffer-path YC encoding and muxing in wrapper RTL.
  - [x] Phase 3: build fresh wrapper artifacts and assemble/check a deployable wrapper package.
  - [ ] Phase 4: perform safe on-device wrapper validation and real hardware color/regression checks.
- [ ] Dependency map:
  - [ ] Chunk 1 can begin immediately after preflight review.
  - [ ] Chunk 2 depends on the same bug context but touches a separate file; it can be developed in parallel with Chunk 1 after shared preflight, but both must finish before Chunk 3.
  - [ ] Chunk 3 depends on Chunks 1 and 2 completing cleanly.
  - [ ] Chunk 4 depends on fresh local artifacts from Chunk 3 plus an idle MiSTer target.
- [ ] Risks and mitigations:
  - [ ] Risk: incorrect framebuffer YC muxing regresses Menu or HDMI paths. Mitigation: gate on `vga_fb & ~vga_scaler & yc_en`, keep `MISTER_DISABLE_YC` guards, and run Menu/HDMI regression checks in final validation.
  - [ ] Risk: subcarrier gating remains disabled on the framebuffer path. Mitigation: update the `subcarrier_out` condition together with the DAC mux change and verify color on hardware.
  - [ ] Risk: Quartus environment on this Mac is unavailable or stale. Mitigation: use `build-core.sh --check-env` first and fall back to the validated `quartus2` VM flow before changing design scope.
  - [ ] Risk: remote validation collides with another workflow. Mitigation: check `lock-status` and `busy-status`, avoid raw `misterctl.sh exec`, and prefer `deploy-wrapper --artifacts-only`.
  - [ ] Risk: local diagnostic edits in the worktree drift from implementation needs. Mitigation: review dirty files before editing and preserve useful YC trace instrumentation until the first successful color validation.
- [ ] Validation strategy:
  - [ ] Tier 1: per-chunk smoke checks using targeted `rg`, env checks, source-prepare/package checks, and artifact existence tests.
  - [ ] Tier 2: end-of-phase targeted builds and bounded wrapper probe/smoke commands.
  - [ ] Tier 3: final real-hardware gate on S-Video color plus Menu/HDMI/CVBS regression coverage.
- [ ] Rollout considerations:
  - [ ] First on-device pass should deploy wrapper-owned artifacts only.
  - [ ] If runtime package freshness is uncertain, confirm whether the current device runtime already matches local wrapper expectations before escalating to a full wrapper package deploy.
  - [ ] Keep final evidence in wrapper logs and YC trace logs so future agents can compare packet values and launch path behavior quickly.

## Iterative Chunks

### Chunk 1: HPS output-clock auto-selection

- [x] Value delivered: the HPS side computes `PHASE_INC` against the framebuffer output pixel clock during native analog `vga_fb` launches, aligning the control path with the planned framebuffer YC encoder.
- [x] Scope boundary: `vendor/Main_MiSTer/video.cpp` only; no RTL edits in this chunk.
- [x] Estimated effort (target 45-90 min): 45-60 min
- [x] Dependencies: confirm current `set_yc_mode()` branch structure and keep the existing `yc_modes` phase override behavior intact.
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] `rg -n "fb_native_analog_auto|output-fb-auto|get_vga_fb|should_use_native_analog_tv_mode" vendor/Main_MiSTer/video.cpp`
    - [x] `bash tools/mister-wrapper/build-hps.sh --check-env`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - [x] `bash tools/mister-wrapper/build-hps.sh`
- [x] Chunk gate pass criteria:
  - [x] The new auto path only fires when `output_clock_available`, `get_vga_fb()`, and `should_use_native_analog_tv_mode()` are all true.
  - [x] Existing `yc_modes` key-based `PHASE_INC` overrides still apply after the automatic clock selection.
  - [x] `build/mister-wrapper-hps/MiSTer_3SX` is produced successfully if the Tier 2 build is run.
- [x] Evidence to capture in progress log:
  - [x] diff excerpt or commit note for the new auto-selection block
  - [x] HPS build output path and timestamp

### Chunk 2: Framebuffer YC encoder in wrapper RTL

- [x] Value delivered: native analog framebuffer launches gain a dedicated YC-encoded output path instead of bypassing the only existing `yc_out`.
- [x] Scope boundary: `vendor/Menu_MiSTer/sys/sys_top.v` only; no HPS logic changes in this chunk.
- [x] Estimated effort (target 45-90 min): 60-90 min
- [x] Dependencies: preserve existing Menu/core-video YC path and use the same parameter registers already fed by `UIO_SET_YC_PAR`.
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] `rg -n "yc_out_fb|yc_fb_o|vga_fb_yc_en|subcarrier_out" vendor/Menu_MiSTer/sys/sys_top.v`
    - [x] `colima --profile quartus2 ssh -- bash -lc 'export PATH=/home/sb.linux/intelFPGA_lite/17.0/quartus/bin:$PATH LC_ALL=C LANG=C && quartus_sh --version'`
    - [x] `bash tools/mister-wrapper/build-core.sh --seed menu --prepare-source`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - [x] `rg -n "yc_out_fb|vga_fb_yc_en" build/mister-wrapper-core/src/sys/sys_top.v`
- [x] Chunk gate pass criteria:
  - [x] New RTL stays under `ifndef MISTER_DISABLE_YC`.
  - [x] `vga_fb_yc_en` gates only `vga_fb & ~vga_scaler & yc_en`.
  - [x] DAC mux and `subcarrier_out` logic preserve existing behavior when `vga_fb_yc_en` is false.
  - [x] Prepared wrapper source contains the new framebuffer YC path.
- [x] Evidence to capture in progress log:
  - [x] diff excerpt for the new wires, `yc_out_fb` instance, DAC mux, and subcarrier gating
  - [x] prepared-source confirmation under `build/mister-wrapper-core/src`

### Chunk 3: Local artifact builds and wrapper package check

- [x] Value delivered: both wrapper-owned artifacts build from the same worktree and a deployable wrapper package/check exists before any device touch.
- [x] Scope boundary: local build/package commands only; no remote deploy/probe/smoke in this chunk.
- [x] Estimated effort (target 45-90 min): 60-90 min, excluding Quartus queue/setup delays
- [x] Dependencies: Chunks 1 and 2 complete; required toolchains or VM/container environment available.
- [x] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] `test -f build/mister-wrapper-hps/MiSTer_3SX`
    - [x] `test -f build/mister-wrapper-core/3SX.rbf`
    - [x] `tools/mister-wrapper/package-wrapper.sh --check --runtime-package build/mister-runtime-package`
  - [x] Tier 2 targeted (if checkpoint chunk):
    - [x] `bash tools/mister-wrapper/build-hps.sh`
    - [x] `colima --profile quartus2 ssh -- bash -lc 'export PATH=/home/sb.linux/intelFPGA_lite/17.0/quartus/bin:$PATH LC_ALL=C LANG=C && cd /Users/sb/Developer/3sx-mister && OUTPUT_DIR=/home/sb.linux/build/mister-wrapper-core bash tools/mister-wrapper/build-core.sh --seed menu --prepare-source && cd /home/sb.linux/build/mister-wrapper-core/src && quartus_sh --flow compile 3SX -c 3SX'`
    - [x] `tools/mister-wrapper/package-wrapper.sh --runtime-package build/mister-runtime-package`
- [x] Chunk gate pass criteria:
  - [x] Fresh `MiSTer_3SX` and `3SX.rbf` artifacts exist locally.
  - [x] `package-wrapper.sh --check` succeeds against the intended runtime package and artifact paths.
  - [x] If full package assembly is run, `build/mister-wrapper-package/MiSTer_3SX` and `build/mister-wrapper-package/_Other/3SX.rbf` exist.
- [x] Evidence to capture in progress log:
  - [x] artifact paths, timestamps, and build mode used (`local`, `docker`, or VM-backed Quartus path)
  - [x] package check or package output root

### Chunk 4: Safe wrapper deploy and real hardware validation

- [ ] Value delivered: the fix is exercised through the actual wrapper launch path on device, with grayscale resolution and key regressions checked on real hardware.
- [ ] Scope boundary: wrapper-safe remote validation only; no unrelated deploy helper changes.
- [ ] Estimated effort (target 45-90 min): 45-90 min plus time for real CRT checks
- [x] Dependencies: Chunk 3 artifacts/package ready and MiSTer target confirmed idle.
- [ ] Chunk-end verification commands:
  - [x] Tier 1 smoke:
    - [x] `tools/mister/misterctl.sh lock-status`
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh busy-status`
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh deploy-wrapper --src build/mister-wrapper-package --artifacts-only`
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh probe-wrapper`
    - [x] `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh smoke-wrapper`
  - [ ] Tier 2 targeted (if checkpoint chunk):
    - [ ] Inspect `/media/fat/games/3sx/logs/yc-debug.log` or the current wrapper trace log for `clock_source=output-fb-auto`
    - [ ] Real hardware checks: 3SX S-Video color, Menu S-Video color, HDMI sanity, and CVBS color if a composite profile is available
- [ ] Chunk gate pass criteria:
  - [x] Remote preflight confirms no active conflicting workflow.
  - [x] Wrapper probe and smoke both return success without leaving stale processes behind.
  - [ ] 3SX native analog S-Video launch shows color on the CRT.
  - [ ] Menu remains in color, HDMI remains unchanged, and CVBS is either confirmed or explicitly deferred due to missing hardware/profile availability.
- [ ] Evidence to capture in progress log:
  - [x] wrapper probe/smoke return codes and log paths
  - [ ] YC trace line showing `clock_source=output-fb-auto`
  - [ ] brief real-hardware observation notes for S-Video, Menu, HDMI, and optional CVBS

## Verification Gates

- [x] Tier 1 (per chunk smoke):
  - [x] Chunk 1: `rg` for the new auto-clock branch plus `bash tools/mister-wrapper/build-hps.sh --check-env`
  - [x] Chunk 2: `rg` for new framebuffer YC symbols plus VM Quartus availability and `build-core.sh --prepare-source`
  - [x] Chunk 3: local artifact existence checks plus `tools/mister-wrapper/package-wrapper.sh --check --runtime-package build/mister-runtime-package`
  - [x] Chunk 4: `lock-status`, `busy-status`, artifacts-only wrapper deploy, `probe-wrapper`, and `smoke-wrapper`
- [ ] Tier 2 (phase targeted every 1-2 chunks):
  - [x] After Chunk 1: `bash tools/mister-wrapper/build-hps.sh`
  - [x] After Chunk 2 or together with Chunk 3: VM-backed `quartus_sh --flow compile 3SX -c 3SX`
  - [x] After Chunk 3: `tools/mister-wrapper/package-wrapper.sh --runtime-package build/mister-runtime-package`
  - [ ] During Chunk 4: inspect YC trace output and run bounded real-hardware regression checks
- [ ] Tier 3 (full-suite final gate only):
  - [x] Fresh local wrapper artifacts exist and optional wrapper package assembly succeeded.
  - [x] Safe wrapper deploy/probe/smoke passed on an idle target.
  - [ ] Real hardware confirms 3SX S-Video color restoration.
  - [ ] Menu S-Video and HDMI show no regression, and CVBS is verified when hardware is available.

## Checklist Sync Rules

- [x] Step checkbox is marked `[x]` immediately after its verification command passes.
- [x] Chunk checkboxes are marked `[x]` only after required chunk gate commands pass.
- [x] Goal/success and other summary checkboxes are updated when evidence is recorded.

## Right-Sized Steps

- [x] Step 1: Review current `set_yc_mode()` logic, dirty worktree context, and any active YC trace helpers before editing.
  - Chunk: Chunk 1
  - Affected area or component: `vendor/Main_MiSTer/video.cpp`, current worktree state
  - Verification method and command: `git status --short` and `rg -n "set_yc_mode|clock_source_name|output_CLK_VIDEO" vendor/Main_MiSTer/video.cpp`
  - Dependencies: none

- [x] Step 2: Add `fb_native_analog_auto` logic so native-analog `vga_fb` launches select `output_CLK_VIDEO` while preserving the existing `yc_modes` override path.
  - Chunk: Chunk 1
  - Affected area or component: `vendor/Main_MiSTer/video.cpp`
  - Verification method and command: `rg -n "fb_native_analog_auto|output-fb-auto" vendor/Main_MiSTer/video.cpp`
  - Dependencies: Step 1

- [x] Step 3: Run the HPS env/build gate and confirm the wrapper binary still builds with the new auto-clock branch.
  - Chunk: Chunk 1
  - Affected area or component: HPS wrapper build flow
  - Verification method and command: `bash tools/mister-wrapper/build-hps.sh --check-env` and `bash tools/mister-wrapper/build-hps.sh`
  - Dependencies: Step 2

- [x] Step 4: Add framebuffer YC wires and the `yc_out_fb` instance guarded by `MISTER_DISABLE_YC`.
  - Chunk: Chunk 2
  - Affected area or component: `vendor/Menu_MiSTer/sys/sys_top.v`
  - Verification method and command: `rg -n "yc_fb_o|yc_out_fb|vga_fb_yc_en" vendor/Menu_MiSTer/sys/sys_top.v`
  - Dependencies: Step 1

- [x] Step 5: Update the DAC mux and `subcarrier_out` logic to select framebuffer YC output only for native analog framebuffer launches.
  - Chunk: Chunk 2
  - Affected area or component: `vendor/Menu_MiSTer/sys/sys_top.v`
  - Verification method and command: `rg -n "VGA_VS|VGA_HS|VGA_R|subcarrier_out" vendor/Menu_MiSTer/sys/sys_top.v`
  - Dependencies: Step 4

- [x] Step 6: Stage the wrapper-core source and confirm the prepared build tree contains the new framebuffer YC path.
  - Chunk: Chunk 2
  - Affected area or component: wrapper-core source preparation flow
  - Verification method and command: `colima --profile quartus2 ssh -- bash -lc 'export PATH=/home/sb.linux/intelFPGA_lite/17.0/quartus/bin:$PATH LC_ALL=C LANG=C && quartus_sh --version'`, `bash tools/mister-wrapper/build-core.sh --seed menu --prepare-source`, and `rg -n "yc_out_fb|vga_fb_yc_en" build/mister-wrapper-core/src/sys/sys_top.v`
  - Dependencies: Step 5

- [x] Step 7: Build fresh HPS and wrapper-core artifacts from the same worktree.
  - Chunk: Chunk 3
  - Affected area or component: `build/mister-wrapper-hps`, `build/mister-wrapper-core`
  - Verification method and command: `bash tools/mister-wrapper/build-hps.sh`, VM-backed `quartus_sh --flow compile 3SX -c 3SX`, `test -f build/mister-wrapper-hps/MiSTer_3SX`, and `test -f build/mister-wrapper-core/3SX.rbf`
  - Dependencies: Steps 3 and 6

- [x] Step 8: Assemble or at least input-check the wrapper package that will be used for deploy-wrapper.
  - Chunk: Chunk 3
  - Affected area or component: `tools/mister-wrapper/package-wrapper.sh`, `build/mister-wrapper-package`
  - Verification method and command: `tools/mister/build-runtime-package.sh`, `tools/mister-wrapper/package-wrapper.sh --check --runtime-package build/mister-runtime-package`, and `tools/mister-wrapper/package-wrapper.sh --runtime-package build/mister-runtime-package`
  - Dependencies: Step 7 and availability of the intended runtime package

- [x] Step 9: Preflight the remote target and deploy wrapper-owned artifacts with the safest suitable mode.
  - Chunk: Chunk 4
  - Affected area or component: `tools/mister/misterctl.sh`, wrapper deploy flow
  - Verification method and command: `tools/mister/misterctl.sh lock-status`, `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh busy-status`, and `MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 tools/mister/misterctl.sh deploy-wrapper --src build/mister-wrapper-package --artifacts-only`
  - Dependencies: Step 8

- [ ] Step 10: Run wrapper probe/smoke and capture log evidence that the framebuffer path now uses the output-clock YC parameters.
  - Chunk: Chunk 4
  - Affected area or component: on-device wrapper validation, YC trace logs
  - Verification method and command: `MISTER_HOST=<mister-ip> MISTER_USER=root MISTER_PASSWORD=<password> tools/mister/misterctl.sh probe-wrapper`, `MISTER_HOST=<mister-ip> MISTER_USER=root MISTER_PASSWORD=<password> tools/mister/misterctl.sh smoke-wrapper`, and inspect the current YC trace log for `clock_source=output-fb-auto`
  - Dependencies: Step 9

- [ ] Step 11: Validate color restoration and key regressions on real hardware.
  - Chunk: Chunk 4
  - Affected area or component: S-Video/CVBS/HDMI runtime behavior on MiSTer
  - Verification method and command: launch 3SX on S-Video native analog, confirm Menu S-Video color, confirm HDMI sanity, and verify CVBS if available
  - Dependencies: Step 10

## Parallelizable Work

- [x] Workstream: Chunk 1 HPS auto-clock logic and Chunk 2 RTL framebuffer YC edits can be developed in parallel after the shared preflight review.
  - Parallel with: each other
  - Preconditions: preserve current dirty-worktree diagnostics, avoid overlapping edits to the same file, and merge both before any build/package gate

## Open Questions

- [x] Question: `build/mister-clean-package` was not present, so wrapper packaging was validated and assembled with a freshly built `build/mister-runtime-package`.
- [x] Question: Existing YC trace instrumentation stayed in place through the first deploy/probe/smoke pass; cleanup remains out of scope until after real hardware color validation.
- [x] Question: CVBS remains a documented follow-up unless a composite profile and physical test path are available during the final hardware validation pass.
- [ ] Question: Who can perform and report the remaining real-hardware S-Video, Menu, and HDMI observations needed to close Chunk 4?
