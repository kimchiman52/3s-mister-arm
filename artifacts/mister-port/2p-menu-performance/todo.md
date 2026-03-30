# Implementation Todo

## Todo Metadata

- [x] Canonical todo path: `artifacts/mister-port/2p-menu-performance/todo.md`
- [x] Active feature/phase: MiSTer 2P character-select and super-art chooser optimization
- [x] Prior canonical checklist:
  - `artifacts/mister-port/scaled-present-path/todo.md` is closed through nearest-mode Chunk 4 and remains the historical record for that stream

## Goal and Success Criteria

- [x] Goal:
  - remove the largest user-visible MiSTer menu slowdown in the 2P character-select lane without changing menu behavior, timing, logic, or determinism
- [x] Success criteria:
  - exact `character-select-super-art` captures stay reproducible through `tools/mister/perf-sampler.sh`
  - the chooser lane is attributed with decision-grade telemetry before any runtime optimization is kept
  - accepted runtime changes improve the measured hotspot on-device without moving the slowdown to a new obvious regression inside the same lane
  - once the 2P menu lane no longer has an obvious next measured win, follow-up work shifts to the next broad user-visible MiSTer hotspot

## Scope and Constraints

- [x] In scope:
  - 2P character select overall
  - the immediate post-select super-art chooser slowdown
  - measurement support needed to attribute the chooser lane
  - runtime improvements only after the measured hotspot is isolated
- [x] Out of scope:
  - gameplay, menu semantics, timing, determinism, rules, or content changes
  - speculative runtime edits that are not backed by the chooser telemetry
  - reopening the closed nearest-mode stream unless a later regression proves it necessary
- [x] Constraints:
  - use Docker container `3sx-mister-build`
  - use `telemetry` builds for iteration and `clean` only for player/runtime handoff validation
  - use `tools/mister/misterctl.sh` for deploy/probe/smoke and `tools/mister/perf-sampler.sh` for captures
  - every completed cycle ends with docs synced, a clean worktree, and a real local commit

## Current Measured Baseline

- [x] Exact chooser capture is available and trusted:
  - `menu-c111e-super-art-selection-exact-basic` = `37.3927 FPS` with `26.7432 / 16.8619 / 9.3454 / 0.5358 ms` for `frame/update/render/present`
  - chooser start state: `Sel_PL_Complete=1/1`, `Sel_Arts_Complete=0/0`, `Select_Arts=3/3`, `Moving_Plate=0/0`, `Moving_Plate_Counter=0/0`, `Disp_Command_Name=1/1`
- [x] Full menu attribution is still incomplete:
  - `menu-m2-char-select-overall-full` = `58.8112 FPS` with `17.0036 / 7.2484 / 9.0560 / 0.6991 ms`
  - `menu-m2-super-art-selection-exact-full` = `35.0328 FPS` with `28.5447 / 18.2807 / 9.7936 / 0.4704 ms`
  - the remaining chooser update cost is now assigned to a small set of outer scopes: `task-game 17.8048 ms/frame`, `game-task-seqs-after-process 16.5495 ms/frame`, and only `0.8308 ms/frame` inside `game01-total`
- [x] `seqsAfterProcess` split is now trusted:
  - `menu-m3r-char-select-overall-full` = `58.7372 FPS` with `17.0250 / 7.3297 / 8.9932 / 0.7021 ms`, where `game-task-seqs-after-process` = `4.9636 ms/frame` splits into `game-task-seqs-after-submit` = `4.5567 ms/frame` and `game-task-seqs-after-renew` = `0.3993 ms/frame`
  - `menu-m3r-super-art-selection-exact-full` = `35.1536 FPS` with `28.4466 / 18.2558 / 9.7102 / 0.4806 ms`, where `game-task-seqs-after-process` = `16.3403 ms/frame` splits into `game-task-seqs-after-submit` = `15.5704 ms/frame` and `game-task-seqs-after-renew` = `0.7624 ms/frame`
  - treat the submit lane as the next runtime target inside the chooser hotspot; renew is a smaller residue, not the first measured win
- [x] First submit-lane runtime reland was rejected:
  - `menu-m4-char-select-overall-full` improved slightly to `59.2159 FPS` with `16.8873 / 7.1585 / 9.0596 / 0.6693 ms`, and `game-task-seqs-after-submit` moved down to `4.4100 ms/frame`
  - `menu-m4-super-art-selection-exact-full` regressed to `34.8510 FPS` with `28.6936 / 18.6046 / 9.6051 / 0.4840 ms`, and `game-task-seqs-after-submit` worsened to `15.7101 ms/frame`
  - the attempted normalized-UV `SDLGameRenderer_DrawSprite2` fast path was rolled back fully; generic `Sprite2` rect setup is not the first exact chooser win
- [x] `seqsAfterProcess -> submit` is now split one level deeper:
  - `menu-m5-char-select-overall-full` = `55.7454 FPS` with `17.9387 / 8.2650 / 8.9851 / 0.6886 ms`, where `game-task-seqs-after-submit` = `5.5394 ms/frame` splits into `game-task-seqs-after-submit-state-change` = `4.2823 ms/frame` over `77.3167` calls/frame and `game-task-seqs-after-submit-enqueue` = `0.7152 ms/frame` over `176.8900` calls/frame
  - `menu-m5-super-art-selection-exact-full` = `33.0544 FPS` with `30.2532 / 20.2577 / 9.5474 / 0.4480 ms`, where `game-task-seqs-after-submit` = `17.0030 ms/frame` splits into `game-task-seqs-after-submit-state-change` = `14.8377 ms/frame` over `105.9750` calls/frame and `game-task-seqs-after-submit-enqueue` = `1.2710 ms/frame` over `269.8500` calls/frame
  - treat chooser texture/state churn as the next runtime target inside submit; sprite enqueue is a much smaller residue than the failed `DrawSprite2` fast-path guess suggested
- [x] First chooser submit runtime reland is now kept:
  - `menu-m6rr-char-select-overall-full` = `65.6915 FPS` with `15.2227 / 5.5326 / 8.9545 / 0.7356 ms`, where `game-task-seqs-after-submit-state-change` dropped to `1.5743 ms/frame`, `game-task-seqs-after-submit-enqueue` to `0.6767 ms/frame`, and `software_surface_cache_refresh` to `0.4412 ms/frame`
  - `menu-m6rr-super-art-selection-exact-full` = `59.1152 FPS` with `16.9161 / 6.9430 / 9.4159 / 0.5573 ms`, where `game-task-seqs-after-submit-state-change` dropped to `2.1851 ms/frame`, `game-task-seqs-after-submit-enqueue` to `0.9915 ms/frame`, and `software_surface_cache_refresh` to `1.3913 ms/frame`
  - the kept reland uses stable logical `ppg-seqs` identities `1030/1031/1032/1034` from the `ix_num_first = 1030`, `texture_total = 7` chooser group, not transient runtime handles; it flipped the exact chooser refresh lane from `17.2250` full no-rect attempts/frame to `17.0750` partial attempts/frame, so the next runtime loop should widen back out to the rest of full 2P character select before leaving the menu stream
- [x] Current lane priority:
  - first: full 2P character-select lane
  - second: immediate slowdown when the super-art chooser appears
  - chooser state-change churn is no longer the first measured win after the kept `m6rr` reland; re-rank the broader character-select lane next before moving on to other MiSTer hotspots

## Iterative Chunks

### Chunk M1: Broaden Chooser Update Attribution

- [x] Value delivered:
  - broader chooser telemetry now proves the currently measured higher-level cost is concentrated in `Basic_Sub` effect list `4`, while also showing most chooser update time still sits outside the current scope boundaries
- [x] Scope boundary:
  - keep the exact chooser capture unchanged
  - extend update-breakdown telemetry one level up in the character-select update path
  - rerun `character-select` and exact chooser captures on MiSTer
- [x] Success metric:
  - the new capture must either explain most of the chooser update time or prove that the remaining cost still sits above the newly added boundaries while identifying the strongest measured higher-level family inside those boundaries
- [x] Verification commands:
  - `git diff --check`
  - `bash -n tools/mister/perf-sampler.sh`
  - telemetry ARM build/package in `3sx-mister-build`
  - `tools/mister/misterctl.sh deploy --src <telemetry-package>`
  - `tools/mister/misterctl.sh probe`
  - `tools/mister/misterctl.sh smoke`
  - `tools/mister/perf-sampler.sh --scene character-select --frames 300 --tag <tag> --perf-wait-test-phase character-select`
  - `tools/mister/perf-sampler.sh --scene character-select-super-art --frames 40 --tag <tag> --perf-wait-runtime-state character-select-super-art`

### Chunk M2: Attribute The Remaining Chooser Update Above Current Game01 Boundaries

- [x] Value delivered:
  - the still-missing chooser update time is now attributed to the outer `Game_Task` lane, with `seqsAfterProcess` dominating the exact chooser slowdown and `Game01` itself measuring as a small residue
- [x] Dependency:
  - Chunk M1 must finish first so the next measurement step starts above `Game01` and `Basic_Sub` instead of remeasuring the same effect slices

### Chunk M3: First Runtime Reland In The Measured Chooser Hotspot

- [x] Value delivered:
  - the first runtime optimization targets the measured dominant chooser/update hotspot rather than a guessed effect routine
- [x] Dependency:
  - Chunk M2 must isolate a clear runtime target first
- [x] Current status:
  - the first runtime attempt targeted normalized-UV `Sprite2` submission in `SDLGameRenderer_DrawSprite2`, but it was rejected after the exact chooser gate regressed
  - the kept second runtime reland expands retained PPG renew dirty-rect tracking to the measured chooser hot logical `ppg-seqs` identities `1030/1031/1032/1034` in the `1030` group, letting the existing software-surface refresh path use partial renew bboxes instead of repeated full `256x256` refreshes across palette variants

## Verification Gates

- [x] Tier 1:
  - `git diff --check`
  - `bash -n tools/mister/perf-sampler.sh` when that file changes
  - telemetry build/package for measurement or runtime menu chunks
- [x] Tier 2:
  - MiSTer `deploy`, `probe`, and bounded `smoke` on runtime or telemetry changes used by captures
  - rerun both overall and exact chooser captures for any accepted menu-lane telemetry/runtime change
- [x] Tier 3:
  - independent review before closeout
  - rerun build/deploy/capture after valid review fixes when runtime code changes

## Cycle Log

- 2026-03-12T19:46:00-0400
  - Research target:
    - turn the measured chooser submit state-change hotspot into a real runtime win by retaining exact renew dirty rects for the hot chooser `ppg-seqs` textures that were still forcing full software-surface refreshes
  - Change summary:
    - added `SDLGameRenderer_QueryTextureLogicalIdentity(...)` in `src/port/sdl/sdl_game_renderer.c` and kept current logical identities registered for active textures so runtime code can target stable chooser texture identities outside perf-capture mode
    - expanded `ppgShouldKeepRenewDirtyRect(...)` in `src/sf33rd/Source/Common/PPGFile.c` to retain renew bboxes for chooser hot logical `ppg-seqs` identities `1030/1031/1032/1034` in the `ix_num_first = 1030`, `texture_total = 7` group, without widening to unrelated menu textures
    - cleared retained renew dirty state on `ppgReleaseTextureHandle(...)` so recycled texture slots cannot inherit chooser dirty-rect history from a prior lifetime
  - Verification evidence:
    - `git diff --check` passed before the build
    - the `/work-arm` telemetry rebuild/install/package completed successfully in `3sx-mister-build`, `readelf -h build/mister-telemetry-package/bin/3sx` still reported `ELF32 ARM` with hard-float ABI, and `docker cp 3sx-mister-build:/work-arm/build/mister-telemetry-package ./build/mister-telemetry-package-arm-menu-m6rr-export2` exported the deployable package
    - MiSTer `health`, `deploy`, `probe`, and bounded `smoke` passed on `build/mister-telemetry-package-arm-menu-m6rr-export2`
    - `menu-m6rr-char-select-overall-full` landed at `65.6915 FPS` with `15.2227 / 5.5326 / 8.9545 / 0.7356 ms`; `game-task-seqs-after-submit-state-change` fell from `4.2823` to `1.5743 ms/frame`, `game-task-seqs-after-submit-enqueue` from `0.7152` to `0.6767 ms/frame`, and `software_surface_cache_refresh` from `3.0894` to `0.4412 ms/frame`
    - `menu-m6rr-super-art-selection-exact-full` landed at `59.1152 FPS` with `16.9161 / 6.9430 / 9.4159 / 0.5573 ms`; `game-task-seqs-after-submit-state-change` fell from `14.8377` to `2.1851 ms/frame`, `game-task-seqs-after-submit-enqueue` from `1.2710` to `0.9915 ms/frame`, and `software_surface_cache_refresh` from `13.8838` to `1.3913 ms/frame`
    - exact chooser refresh work flipped from `17.2250` full no-usable-dirty-rect attempts/frame and `1128857.60` refreshed pixels/frame on `menu-m5-super-art-selection-exact-full` to `17.0750` partial attempts/frame and `39328.00` partial pixels/frame on `menu-m6rr-super-art-selection-exact-full`, with only `0.1500` oversized full-refresh attempts/frame left
  - Keep/rollback decision:
    - keep; the reland directly addresses the measured chooser state-change bottleneck, preserves direct-presented software-frame behavior, and turns the hot exact chooser lane from a catastrophic update stall into a much smaller residue without introducing a new obvious regression in the broader menu lane
  - Final commit hash:
    - `606cddd1` (`perf: keep chooser renew rects on stable seq ids`)
  - Next best candidate:
    - stay on the active 2P menu stream, but widen back out to the rest of full character select before leaving menus; the exact chooser dirty-rect lane no longer has the clearest measured next win

- 2026-03-12T19:30:00-0400
  - Research target:
    - split the measured chooser `seqsAfterProcess -> submit` hotspot into texture/state-change work versus sprite enqueue work before attempting another runtime reland
  - Change summary:
    - added telemetry-only nested scopes for `game-task-seqs-after-submit-state-change` and `game-task-seqs-after-submit-enqueue` in `src/main.c`, `src/main.h`, and `src/sf33rd/Source/Game/rendering/mtrans.c`
    - rebuilt the ARM telemetry package through `/work-arm` in `3sx-mister-build`, exported it back to the host, redeployed it with `tools/mister/misterctl.sh`, reran bounded `probe`/`smoke`, and reran both the broad character-select and exact chooser captures on MiSTer
    - completed an independent read-only `codex review --uncommitted` pass on the scoped diff; the review found no issues, so the verified tree stayed unchanged
  - Verification evidence:
    - `git diff --check` passed before the build
    - the `/work-arm` telemetry rebuild/install/package completed successfully in `3sx-mister-build`, `readelf -h build/mister-telemetry-package/bin/3sx` still reported `ELF32 ARM` with hard-float ABI, and `docker cp 3sx-mister-build:/work-arm/build/mister-telemetry-package ./build/mister-telemetry-package-arm-menu-m5-export` exported the deployable package
    - MiSTer `health`, `deploy`, `probe`, and bounded `smoke` passed on `build/mister-telemetry-package-arm-menu-m5-export`
    - `menu-m5-char-select-overall-full` landed at `55.7454 FPS` with `17.9387 / 8.2650 / 8.9851 / 0.6886 ms`; `task-game` averaged `7.8542 ms/frame`, `game-task-seqs-after-process` `5.9363 ms/frame`, `game-task-seqs-after-submit` `5.5394 ms/frame`, `game-task-seqs-after-submit-state-change` `4.2823 ms/frame`, and `game-task-seqs-after-submit-enqueue` `0.7152 ms/frame`
    - `menu-m5-super-art-selection-exact-full` landed at `33.0544 FPS` with `30.2532 / 20.2577 / 9.5474 / 0.4480 ms`; `task-game` averaged `19.7939 ms/frame`, `game-task-seqs-after-process` `17.8355 ms/frame`, `game-task-seqs-after-submit` `17.0030 ms/frame`, `game-task-seqs-after-submit-state-change` `14.8377 ms/frame`, and `game-task-seqs-after-submit-enqueue` `1.2710 ms/frame`
  - Keep/rollback decision:
    - keep measurement-support only; the chooser submit hotspot now measures as overwhelmingly texture/state-change cost rather than sprite enqueue, so the next runtime reland should move to `flSetRenderState` / `SDLGameRenderer_SetTexture` behavior instead of retrying `DrawSprite2`
  - Final commit hash:
    - recorded in the loop closure commit
  - Next best candidate:
    - keep the next runtime loop on the chooser submit lane, but target state churn first by measuring and then reducing the repeated `FLRENDER_TEXSTAGE0` / `SDLGameRenderer_SetTexture` cost in the exact chooser path before widening back out to other menu work

- 2026-03-12T15:03:52-0400
  - Research target:
    - test whether a normalized-UV `Sprite2` submit fast path in `SDLGameRenderer_DrawSprite2` is the first real runtime win inside the measured chooser `seqsAfterProcess -> submit` hotspot
  - Change summary:
    - attempted a runtime reland in `src/port/sdl/sdl_game_renderer.c` that bypassed the heavier generic rect setup for `Sprite2` submission when the UVs were already normalized
    - completed an independent read-only `codex review --uncommitted` pass on the runtime diff; the review found no correctness regression, but the measured hotspot still failed the keep gate
    - rolled the runtime diff back fully, rebuilt the restored ARM telemetry package in `3sx-mister-build`, redeployed it through `tools/mister/misterctl.sh`, and reran bounded `probe`/`smoke` so the device returned to the prior baseline
  - Verification evidence:
    - `git diff --check` passed before the candidate build and again after the rollback
    - the candidate `/work-arm` telemetry rebuild/install/package and the rollback rebuild/install/package both completed successfully in `3sx-mister-build`, and `readelf -h build/mister-telemetry-package/bin/3sx` reported `ELF32 ARM` with hard-float ABI on both builds
    - MiSTer `health`, candidate `deploy`, `probe`, and bounded `smoke` passed before the keep gate; `menu-m4-char-select-overall-full` landed at `59.2159 FPS` with `16.8873 / 7.1585 / 9.0596 / 0.6693 ms`, and `game-task-seqs-after-submit` improved slightly to `4.4100 ms/frame`
    - the exact hotspot keep gate failed: `menu-m4-super-art-selection-exact-full` landed at `34.8510 FPS` with `28.6936 / 18.6046 / 9.6051 / 0.4840 ms`, while `game-task-seqs-after-submit` worsened to `15.7101 ms/frame` from the trusted `15.5704 ms/frame`
    - after rollback, MiSTer `deploy`, `probe`, and bounded `smoke` passed again on `build/mister-telemetry-package-arm-menu-m4-rollback-r1`
  - Keep/rollback decision:
    - rollback; the candidate slightly helped broad character select but made the user-priority exact chooser gate worse, so this `Sprite2` rect fast-path shape is not the right first runtime win inside the chooser submit lane
  - Final commit hash:
    - `643bc991`
  - Next best candidate:
    - stay on Chunk M3, but split `seqsAfterProcess` submit work more narrowly before another runtime reland, starting with texture-bind/state-change cost versus sprite enqueue cost inside `mtrans`

- 2026-03-12T14:44:09-0400
  - Research target:
    - split the measured `seqsAfterProcess` chooser hotspot into renew-versus-submit work before attempting the first runtime reland in the 2P menu lane
  - Change summary:
    - added telemetry-only nested scopes for `game-task-seqs-after-renew` and `game-task-seqs-after-submit` in `src/main.c`, `src/main.h`, `src/sf33rd/Source/Game/game.c`, and `src/sf33rd/Source/Game/rendering/mtrans.c`
    - completed an independent read-only `codex exec` review, accepted the valid attribution finding, and gated the new scopes so they only record while `Game_Task` owns the parent `game-task-seqs-after-process` scope
    - rebuilt the ARM telemetry package in `3sx-mister-build`, redeployed it through `tools/mister/misterctl.sh`, reran bounded `probe`/`smoke`, and reran both the overall character-select and exact chooser captures on MiSTer
  - Verification evidence:
    - `git diff --check` passed before both builds
    - the reviewed `/work-arm` telemetry rebuild/install/package completed successfully in `3sx-mister-build`, and `readelf -h build/mister-telemetry-package/bin/3sx` still reported `ELF32 ARM` with hard-float ABI
    - MiSTer `deploy`, `probe`, and bounded `smoke` passed on `build/mister-telemetry-package-arm-menu-m3-export`
    - `menu-m3r-char-select-overall-full` landed at `58.7372 FPS` with `17.0250 / 7.3297 / 8.9932 / 0.7021 ms`; `task-game` averaged `6.9062 ms/frame`, `game-task-seqs-after-process` `4.9636 ms/frame`, `game-task-seqs-after-submit` `4.5567 ms/frame`, and `game-task-seqs-after-renew` `0.3993 ms/frame`
    - `menu-m3r-super-art-selection-exact-full` landed at `35.1536 FPS` with `28.4466 / 18.2558 / 9.7102 / 0.4806 ms`; `task-game` averaged `17.8224 ms/frame`, `game-task-seqs-after-process` `16.3403 ms/frame`, `game-task-seqs-after-submit` `15.5704 ms/frame`, and `game-task-seqs-after-renew` `0.7624 ms/frame`
  - Keep/rollback decision:
    - keep measurement-support only; the chooser hotspot is now decisively the submit lane inside `seqsAfterProcess`, and the review-fixed scope gating keeps that attribution clean
  - Final commit hash:
    - `a56ee484`
  - Next best candidate:
    - keep the next runtime loop on the chooser lane and target `seqsAfterProcess` submit work first, starting with the mtrans/render-task submission path rather than texture-renew handling

- 2026-03-12T14:19:08-0400
  - Research target:
    - assign the remaining chooser update time above `Game01` to task-level or outer `Game_Task` scopes before attempting any runtime reland in the 2P menu lane
  - Change summary:
    - added telemetry-only update-breakdown scopes for `TASK_ENTRY`, `TASK_MENU`, `TASK_GAME`, `TASK_DEBUG`, the main `Game_Task` phases, and inclusive `Game01` total timing in `src/main.c`, `src/main.h`, and `src/sf33rd/Source/Game/game.c`
    - kept the runtime behavior-neutral, rebuilt the ARM telemetry package in `3sx-mister-build`, redeployed it through `tools/mister/misterctl.sh`, and reran both the overall character-select and exact chooser captures on MiSTer
    - completed an independent `codex exec -s read-only` review pass on the uncommitted diff with `No findings.`
  - Verification evidence:
    - `git diff --check` passed before the build
    - the `/work-arm` telemetry rebuild/install/package completed successfully in `3sx-mister-build`, and `readelf -h build/mister-telemetry-package/bin/3sx` still reported `ELF32 ARM` with hard-float ABI
    - MiSTer `health`, `deploy`, `probe`, and bounded `smoke` passed on `build/mister-telemetry-package-arm-menu-m2-export`
    - `menu-m2-char-select-overall-full` landed at `58.8112 FPS` with `17.0036 / 7.2484 / 9.0560 / 0.6991 ms`; the dominant outer scopes were `task-game 6.8318 ms`, `game-task-seqs-after-process 4.8561 ms`, `game-task-main-dispatch 1.6148 ms`, and only `game01-total 0.8204 ms`
    - `menu-m2-super-art-selection-exact-full` landed at `35.0328 FPS` with `28.5447 / 18.2807 / 9.7936 / 0.4704 ms`; the dominant outer scopes were `task-game 17.8048 ms`, `game-task-seqs-after-process 16.5495 ms`, `game-task-main-dispatch 0.8364 ms`, `game01-total 0.8308 ms`, and `basic-sub-effect-list-4 0.6826 ms`
  - Keep/rollback decision:
    - keep measurement-support only; the chooser hotspot is now clearly above `Game01` and concentrated in `seqsAfterProcess`, so the next runtime reland should target that lane rather than the earlier effect-list guess
  - Final commit hash:
    - `f8dc1d73`
  - Next best candidate:
    - instrument and then optimize the dominant `seqsAfterProcess` work in the exact chooser lane, with special attention to the mtrans/sprite submission path that explodes after super-art selection appears

- 2026-03-12T17:56:00-0400
  - Research target:
    - broaden the chooser attribution one level up from targeted select/effect routines and verify whether the missing `~18 ms` chooser update cost sits inside `Game01`/`Basic_Sub` or above them
  - Change summary:
    - created `artifacts/mister-port/2p-menu-performance/todo.md` as the canonical checklist for the new 2P menu stream and updated `artifacts/mister-port/living-findings.md` to point at it
    - added telemetry-only update-breakdown scopes for `Game01` `BG_Draw_System`, `Game01` `Setup_Play_Type`, and each `Basic_Sub` effect-list call in `src/sf33rd/Source/Game/game.c` and `src/sf33rd/Source/Game/system/sys_sub.c`
    - kept the runtime tree behavior-neutral, rebuilt a fresh ARM telemetry package in `3sx-mister-build`, redeployed it, and completed the required independent `codex review --uncommitted` pass with no findings
  - Verification evidence:
    - `git diff --check` and `bash -n tools/mister/perf-sampler.sh` passed
    - recreated the validated `3sx-mister-build` cross-build container, installed the documented cross-toolchain, rebuilt `/work-arm` telemetry, exported `build/mister-telemetry-package-arm-menu-m1-export`, and confirmed the final package binary is `ELF32 ARM` with hard-float ABI from `readelf` inside the container
    - MiSTer `health`, `deploy`, `probe`, and bounded `smoke` all passed on the new telemetry package
    - `menu-m1-char-select-overall-full` landed at `58.6687 FPS` with `17.0449 / 7.2955 / 9.0756 / 0.6738 ms`; the largest exported scopes were `basic-sub-effect-list-4 0.3473 ms` and `game01-bg-draw-system 0.2967 ms`, while total exported scope mean was only `0.8487 ms`
    - `menu-m1-super-art-selection-exact-full` landed at `34.9233 FPS` with `28.6342 / 18.7385 / 9.3934 / 0.5022 ms`; the largest exported scopes were `basic-sub-effect-list-4 0.7380 ms`, `effect-38-portrait 0.1592 ms`, `effect-79-super-art-plate 0.1367 ms`, and `game01-bg-draw-system 0.1321 ms`, while total exported scope mean was only `1.3091 ms`
  - Keep/rollback decision:
    - keep measurement-support only; the new scopes identify `Basic_Sub` effect list `4` as the strongest measured higher-level chooser bucket, but they still explain only a small fraction of total chooser update time, so a runtime reland would still be premature
  - Final commit hash:
    - recorded in the loop closure commit
  - Next best candidate:
    - instrument task-level or outer update-loop scopes above `Game01` so the remaining chooser update cost is attributed before deciding whether effect list `4` or an even higher menu/task path is the real runtime target

- 2026-03-11T05:16:48-0400
  - Research target:
    - first update-path attribution pass for the user-priority 2P character-select lane and the exact super-art chooser slowdown
  - Change summary:
    - added targeted update-breakdown scopes for `SelectTimer_Run`, `Select_Player`, `Sel_PL_Control`, `Player_Select_Control`, `Sel_Arts_Sub`, and selected menu effect IDs
    - kept the tree gameplay-neutral and measurement-only
  - Verification evidence:
    - `menu-c112r-char-select-overall-full` landed at `44.7572 FPS` with `11.3586 ms update`
    - `menu-c112r-super-art-selection-exact-full` landed at `35.2220 FPS` with `18.3873 ms update`
    - the largest exported chooser slices were still only `effect-38-portrait 0.2329 ms`, `effect-79-super-art-plate 0.1065 ms`, and `effect-d8-cursor-circle 0.0493 ms`
  - Keep/rollback decision:
    - keep measurement-support only; the remaining chooser update cost still needs broader attribution before any runtime reland
  - Final commit hash:
    - `b54dc230`
  - Next best candidate:
    - instrument broader character-select/menu task dispatch above the targeted effects so the missing chooser update time is attributable

- 2026-03-11T04:34:44-0400
  - Research target:
    - recover an exact, reproducible super-art chooser capture for the post-character-select slowdown
  - Change summary:
    - added `character-select-super-art` runtime-state capture support plus chooser start-state telemetry
  - Verification evidence:
    - `menu-c111e-super-art-selection-exact-basic` landed at `37.3927 FPS` with `16.8619 ms update`
    - `menu-c111-attract-demo-logo-check` confirmed the older attract/logo runtime-state path still works
  - Keep/rollback decision:
    - keep measurement-support only; exact chooser capture is now stable and ready for deeper attribution
  - Final commit hash:
    - `c975b823`
  - Next best candidate:
    - attribute the remaining chooser update cost inside the character-select update path before attempting a runtime optimization
