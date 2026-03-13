# Ralph Part 2 Todo

Historical checklist. Active follow-up work moved to `artifacts/mister-port/stock-image-architecture-loop-series/todo.md`.

## Todo Metadata

- [x] Canonical todo path: `artifacts/mister-port/ralphpart2/todo.md`
- [x] Status: historical predecessor for the stock-image architecture loop series
- [x] Branch: `ralphpart2`
- [x] Living record: `artifacts/mister-port/living-findings.md`
- [x] Historical completed loop series:
  - `artifacts/mister-port/gameplay-loop-series/todo.md`
  - `artifacts/mister-port/render-loop-series/todo.md`
  - `artifacts/mister-port/telemetry-loop/todo.md`

## Goal And Contract

- [x] Goal: continue MiSTer gameplay-performance work with unattended, research-first IVRFC loops that preserve gameplay behavior
- [x] Every cycle must:
  - begin with DEEP research grounded in current gameplay evidence
  - select one research-backed optimization target at a time
  - update this checklist with the loop plan before code changes
  - build, deploy, and verify on MiSTer before any runtime-changing commit
  - run a fresh review pass, apply only valid fixes, then re-verify any runtime-changing diff
  - if research rules out every safe runtime change for the cycle, or a runtime experiment is reverted after failing verification, reconcile the cycle into a real local commit instead of leaving it at `no-commit`
  - update `living-findings.md` and this checklist before the cycle-closing commit
  - leave perf/log artifacts uncommitted

## Starting State

- [x] Current representative gameplay gate: `tools/mister/perf-sampler.sh --gameplay-idle --gameplay-warmup 120`
- [x] Current live MiSTer preflight on branch cut (`2026-03-06`):
  - `run-3sx.sh --probe-renderer-only` confirmed dummy/software + fbdev + native path
  - bounded `launch-osd.sh` smoke run reached the normal startup path and timed out as expected (`runtime_rc=124`)
- [x] Known open safety follow-up carried in from the prior series:
  - live low-resolution verification of the Loop 3 clipped-native crop branch
- [x] Current process note:
  - the prior gameplay-loop series is complete; new cycles must do fresh DEEP research instead of assuming the previous candidate list was exhaustive

## First Automation Loop

- [x] Loop 0 kickoff objective:
  - perform a fresh DEEP research pass against current gameplay performance after the completed series
  - rank the next 1-3 gameplay-safe optimization candidates using current evidence, not startup-only assumptions
  - if one candidate is clearly justified and safely scoped, carry that same cycle through IVRFC to a verified commit
  - otherwise, record the research/update state cleanly and proceed to the next candidate on the following cycle

## Loop 0 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - container baseline build succeeded with `CC=clang CXX=clang++ ... -DPORT_MISTER=ON` in `3sx-mister-build`
  - live native gameplay capture `ralphpart2-research-native-pre`: `60.56 / 1.24 / 55.48 ms` frame/render/present mean, `copy_bytes.mean=344064`, `dirty_tiles=336/336`, `sort=qsort 300/300`
  - live square-pixels gameplay capture `ralphpart2-research-square-pre`: `96.86 / 1.37 / 91.64 ms` frame/render/present mean, `copy_bytes.mean=3096576`, `dirty_tiles=336/336`, `sort=qsort 300/300`
  - SDL3 primary-source check via `context7` / SDL wiki: `SDL_TEXTUREACCESS_TARGET` textures are not lockable; only `SDL_TEXTUREACCESS_STREAMING` supports `SDL_LockTexture*`, so there is no supported direct pixel-access escape hatch for `cps3_canvas`
- [x] Research-backed hypothesis that was tested this cycle:
  - optimize only the scaled current-target fbdev presenter path in `src/port/sdl/fbdev_presenter.c`
  - cache the exact nearest-neighbor source-index mapping for scaled direct-present rectangles so square-pixels/clipped scaled output stops paying per-pixel integer division every frame
  - leave the native exact-fit `384x224` direct-present path unchanged; current evidence still points to SDL readback cost, and prior broader bypass attempts already regressed gameplay
- [x] Success metric and verification plan for the attempted loop:
  - keep the native gate flat within noise versus `ralphpart2-research-native-pre`
  - improve square-pixels `present_ms` measurably versus `ralphpart2-research-square-pre` without changing `copy_bytes`, visible output mapping, or scale-mode behavior
  - verify with Docker build/package, MiSTer probe, bounded runtime launch, native perf capture, square-pixels perf capture, and a fresh review pass before commit
- [x] Rejected before implementation this cycle:
  - do not reopen target-texture locking/readback-bypass ideas for native exact-fit; the SDL API does not support locking the render target texture, and the prior surface-renderer swap already lost on device

## Loop 0 Outcome

- [x] Implementation summary:
  - temporarily added a mapped-scale LUT cache in `src/port/sdl/fbdev_presenter.c` for the scaled current-target direct-present path only
  - reverted the runtime change after MiSTer control verification failed
- [x] Verification evidence:
  - probe + bounded launch passed after deploy on MiSTer with the expected dummy/software + fbdev + native path
  - native pre-control `ralphpart2-research-native-pre`: `60.56 / 1.24 / 55.48 ms`
  - native post with the candidate `ralphpart2-native-post`: `120.16 / 2.33 / 110.33 ms`
  - native post rerun `ralphpart2-native-post-rerun`: `119.81 / 2.44 / 109.60 ms`
  - reverted native check `ralphpart2-native-revert-check`: `105.91 / 2.29 / 97.00 ms` overall, improving only to `73.59 / 1.55 / 67.42 ms` over frames `241-300`
  - square-pixels pre-control `ralphpart2-research-square-pre`: `96.86 / 1.37 / 91.64 ms`; no post-change square-pixels result was accepted once the native control gate failed
- [x] Keep / rollback decision:
  - rollback the LUT-cache idea; it cannot be kept because the native control gate regressed badly after deploy and did not return to the original baseline cleanly even after reverting
  - commit hash: `no-commit`
- [x] Review gate:
  - independent review pass on the remaining diff flagged one wording overstatement: the earlier todo phrasing implied the full verification sequence had completed cleanly
  - accepted fix: reworded that section to describe the attempted loop plan without implying a trusted square-pixels post result or a clean keep-path verification
  - no additional high-confidence in-scope findings were accepted
- [x] Next best candidate:
  - stabilize the post-deploy native control gate before retrying presenter micro-optimizations, or add narrower readback-versus-copy telemetry so future presenter experiments can be isolated more safely
  - do not retry the mapped-scale LUT cache until the native control gate is stable across redeploys

## Candidate Queue

- [x] Re-profile representative gameplay scenes beyond the existing idle-versus gate if the current evidence is too narrow to rank the next bottleneck confidently.
- [ ] Revisit low-resolution/clipped native presentation only after live low-resolution verification confirms the current kept path is correct.
- [x] Stabilize post-deploy native gameplay controls before retrying another presenter micro-optimization.
- [x] Add render-submission telemetry before another runtime change so the next candidate is backed by texture-run and cache-invalidation evidence instead of speculation.
- [x] Add conservative same-texture rect-run mergeability telemetry before any runtime batching attempt; current evidence says run shape matters more than unlock-driven invalidation churn.
- [x] Measure why same-texture native runs are still slow when exact strip mergeability is zero, focusing on mtrans-heavy frames and color/flip/geometry-recovery diversity before another submission-path change.
- [ ] Search for the next gameplay-safe presenter or render-submission win that survives DEEP research and MiSTer verification, while respecting the existing "Do Not Retry Now" list in `living-findings.md`.
- [ ] Add an explicit stage-selection control for deterministic MiSTer scene sweeps before using stage diversity as optimization evidence.
- [x] Re-run stable native and square-pixels gameplay controls from the fixed launcher sequence before choosing the next presenter-specific code change.

## Loop 11 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - current tree still carries the uncommitted Loop 9 matchup-override support plus the Loop 10 no-commit documentation only; no new runtime optimization is active beyond the kept Loop 8 renderer telemetry baseline
  - fresh review of the kept native MiSTer captures keeps the remaining bottleneck shape unchanged: `SDL_RenderReadPixels()` still dominates frame time on the exact-fit native path, but its cost scales with queued software-renderer copy work rather than fbdev copy (`ralphpart2-loop9-control-native`: `60.17 / 1.36 / 55.04 ms`, `present_readback=54.42 ms`, `set_texture_calls=228.35`, `rect_texture_run_links=170.25`, `render_task_count=291.47`)
  - broadened Loop 9 scene evidence shows the same-texture-run signal is not isolated to one matchup: across `Chun-Li/Remy`, `Ken/Twelve`, `Ryu/Makoto`, and `Sean/Elena`, `rect_texture_run_links` stays materially non-zero while `rect_texture_color_breaks` remains exactly `0.0`, which means contiguous same-texture runs already preserve modulation color and are still paying repeated copy-prep work
  - SDL primary sources now point at a library-internal safe optimization target rather than another app-side task rewrite: `context7` confirms `SDL_RenderReadPixels()` returns a newly allocated surface and is explicitly slow, while `SDL_SetSurfaceColorMod()` / `SDL_SetSurfaceAlphaMod()` / `SDL_SetSurfaceBlendMode()` set durable surface blit state; official SDL 3.4.0 source (`src/render/software/SDL_render_sw.c`) still calls all three setters inside `PrepTextureForCopy()` for every `SDL_RENDERCMD_COPY` / `COPY_EX` and leaves a `FIXME` noting that some of those calls can probably be avoided
  - local renderer inspection closes the app-side side-conditions for that SDL patch: 3SX already caches texture color/alpha submission on its side, creates game textures with `SDL_BLENDMODE_BLEND` once, and the representative native gate records zero same-texture color breaks, so contiguous same-texture runs should be able to reuse already-prepared software surfaces without touching draw order, task generation, determinism, or visible gameplay output
- [x] Research-backed hypothesis to test this cycle:
  - keep the runtime change scoped to SDL 3.4.0 software-renderer internals plus the repo plumbing needed to rebuild that patched dependency for MiSTer
  - add a contiguous-run cache in `PrepTextureForCopy()` so the software renderer skips redundant `SDL_SetSurfaceColorMod()` / `SDL_SetSurfaceAlphaMod()` / `SDL_SetSurfaceBlendMode()` calls when the active copy command reuses the same texture with the same modulation/blend state as the immediately previous prepared texture
  - preserve all existing copy ordering, texture contents, modulation semantics, blend semantics, and the fbdev/native presenter path; if the SDL patch fails to improve the native gate or shows any gameplay/output regression, revert it and close the cycle with a docs-backed rollback commit
- [x] Success metric and verification plan for the attempted loop:
  - rebuild the patched SDL dependency and `PORT_MISTER=ON` game binary in Docker container `3sx-mister-build`, package, deploy, and confirm `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only` plus a bounded `launch-osd.sh` smoke run still pass with dummy/software + fbdev + native on the device
  - improve the default native `gameplay-idle` control measurably versus `ralphpart2-loop9-control-native` without increasing render time materially, then re-check at least the broadened `Sean/Elena` matchup so the win is not isolated to one run shape
  - run the required independent review pass on the diff, apply only high-confidence in-scope fixes, then rebuild/redeploy/re-capture if any accepted finding touches the runtime path
- [x] Rejected before implementation this cycle:
  - do not retry cached preflipped textures, strip merging, geometry batching, or app-side texture-mod setter elision; the living findings plus Loop 10 and prior SDL-source review already closed those shapes for the current bottleneck
  - do not spend this cycle on palette/cache-eviction micro-optimizations or build-flag tuning; current gameplay telemetry still points first at repeated software-renderer copy prep inside present, not at cache churn or app-only codegen
  - do not touch gameplay-facing sort keys, task ordering, or simulation-side culling as part of this runtime experiment

## Loop 11 Outcome

- [x] Implementation summary:
  - added a scoped SDL 3.4.0 software-renderer experiment by plumbing `build-deps.sh` to apply a MiSTer-only patch and caching contiguous prepared-texture state inside `PrepTextureForCopy()`
  - forced a fresh SDL rebuild in `3sx-mister-build`, rebuilt/package-installed the MiSTer target, redeployed to `192.168.1.171`, and verified the patched runtime through the normal probe + bounded-launch path before taking gameplay captures
  - rolled the SDL patch and build plumbing back after the on-device gameplay captures stayed flat-to-worse and the fresh review surfaced shared-build hazards that would have made the patch unsafe to keep even if the perf signal were better
- [x] Verification evidence:
  - the patched runtime passed the required device preflight: `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only` still reported dummy/software + fbdev + native at `1280x720`, and the bounded `launch-osd.sh` smoke run still ended with `runtime_rc=124` and `last-run.log` `exit=143`
  - patched default control `ralphpart2-loop11-control-native` measured `61.05 / 1.39 / 55.82 ms` frame/render/present versus the kept Loop 9 control `60.17 / 1.36 / 55.04 ms`, so the representative gameplay gate did not improve
  - broadened patched `Sean/Elena` capture `ralphpart2-loop11-sean-elena` measured `55.76 / 1.11 / 51.01 ms` versus the kept Loop 9 `Sean/Elena` reference `51.77 / 1.08 / 47.10 ms`, which is a clear regression in both total frame time and `present_readback`
  - a fresh independent `codex` review of the working-tree diff found three substantive issues with keeping the SDL plumbing as written: the shared `third_party/sdl3/build` output can silently reuse an unpatched desktop build for MiSTer, the patch would also bleed into default desktop/CI/release builds because it was not profile-scoped, and future patch edits would miss local/CI cache invalidation
  - after rollback, the closure path restored the stock SDL dependency/deploy and the revert control `ralphpart2-loop11-revert-control-native` returned to the accepted baseline shape at `61.27 / 1.39 / 55.94 ms`, confirming the rejected SDL cache path was no longer active on the MiSTer
- [x] Keep / rollback decision:
  - rollback the `PrepTextureForCopy()` cache experiment; it did not improve the representative native gameplay gate, it regressed the broadened `Sean/Elena` capture materially, and the reviewed SDL plumbing would have been unsafe to keep in a shared build graph
  - commit hash: recorded in the loop closure commit
- [x] Review gate:
  - independent review ran through the `debate-code-review` second-opinion runner with a separate `codex` reviewer on the live Loop 11 diff
  - accepted findings:
    - `shared-sdl-build-reuse-can-skip-the-mister-patch`: accepted as rollback evidence; keeping a MiSTer-only SDL runtime change without a profile-specific dependency root would make the verification path non-deterministic
    - `unscoped-sdl-patch-bleeds-into-desktop-builds`: accepted as rollback evidence; the patch was supposed to be MiSTer-only and should not widen into default desktop/CI/release outputs
    - `patch-file-not-in-cache-invalidation`: accepted as rollback evidence; stale dependency caches would undermine future validation of any retained SDL patch variant
  - rejected findings: none
  - no narrower follow-up fix was retained because the runtime patch already failed the on-device gameplay gate
- [x] Next best candidate:
  - do not retry this SDL copy-prep cache shape casually on the current native gate; any future revisit would need profile-isolated SDL dependency plumbing plus new evidence that repeated `PrepTextureForCopy()` surface-state setters are materially visible in real gameplay on-device
  - return to a fresh DEEP research pass for the next loop instead of spending another iteration on this rejected SDL-internal path

## Loop 10 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - current dirty-tree baseline still includes the kept Loop 9 measurement-support changes only; no new runtime edits have been applied yet for this cycle
  - fresh local inspection of the broadened Loop 9 captures keeps the same cross-scene bottleneck shape: native `present_readback` remains dominant, and within each tested scene it tracks `set_texture_calls`, `render_task_count`, `rect_texture_multi_run_tasks`, and `rect_texture_run_links` materially more than cache churn or raw `mtrans` volume (`artifacts/mister-port/perf/ralphpart2-loop9-*.json`)
  - the default slow control still carries a meaningful flipped-copy population on the exact-fit native path (`rect_texture_flipped_tasks.mean = 70.01`, `rect_texture_flip_breaks.mean = 12.29`), while alternate scenes show that flip diversity is workload-dependent rather than fixed noise (`Sean/Elena` reaches `25.51 / 55.18`)
  - SDL primary sources now identify a concrete safe runtime candidate: official SDL3 docs still show `SDL_RenderTextureRotated()` is the copy-with-flip entry point, and current `libsdl-org/SDL` source takes flipped software-renderer copies through `SDL_RENDERCMD_COPY_EX -> SW_RenderCopyEx()`, which rotates/flips temporary surfaces, while plain `SDL_RenderTexture()` stays on the much cheaper `SDL_RENDERCMD_COPY -> SDL_BlitSurface{,Scaled}` path
  - a same-SDL container microbenchmark inside `3sx-mister-build` reproduced the software-renderer + target-texture + `SDL_RenderReadPixels()` shape and measured a mixed-flip workload at `rotated_ms=3.272` versus `preflipped_ms=2.698` (`delta_ms=0.574`) when flipped copies were replaced by cached preflipped textures plus adjusted source rects
  - local renderer inspection shows the port currently routes every flipped textured-rect task through `SDL_RenderTextureRotated(..., angle=0, flip=task->flip)` in `src/port/sdl/sdl_game_renderer.c`, so a cache-backed preflipped-texture path can stay within the existing draw-order / modulation semantics without touching gameplay logic, timing, RNG, or task generation
- [x] Research-backed hypothesis to test this cycle:
  - keep the scope to `src/port/sdl/sdl_game_renderer.c` texture-cache internals and the textured-rect submit path only
  - add cached preflipped SDL texture variants for the existing texture/palette cache keys, remap flipped source rects onto those variants, and submit them through plain `SDL_RenderTexture()` instead of `SDL_RenderTextureRotated()` when `angle` would otherwise remain zero
  - preserve draw order, texture/palette invalidation semantics, color/alpha modulation behavior, and visible output; if the cache variant path cannot preserve those guarantees cleanly, revert it and close the cycle without keeping the runtime change
- [x] Success metric and verification plan for the attempted loop:
  - Docker build/install/package succeeds in `3sx-mister-build`, MiSTer deploy + probe + bounded launch still pass, and backend logs remain on dummy/software + fbdev + native
  - the default native `gameplay-idle` control improves measurably versus `ralphpart2-loop9-control-native` without any startup/resource regressions, and at least one alternate matchup with elevated flip diversity is rechecked to confirm the win is not isolated to one scene
  - run the required independent review pass on the diff, apply only high-confidence in-scope fixes, then rebuild/redeploy/re-capture if any accepted fix touches the runtime path
- [x] Rejected before implementation this cycle:
  - do not reopen direct readback-bypass ideas, geometry batching, strip merging, or app-side texture-mod caching; current living findings plus the fresh SDL/source evidence still leave those paths unsupported or explicitly disproved
  - do not attempt any task reordering, sort-key changes, or gameplay-path instrumentation as the runtime optimization itself; those carry higher behavior risk than the cache-local flip-path substitution and are not required by this cycle's evidence

## Loop 10 Outcome

- [x] Implementation summary:
  - temporarily added cached preflipped texture variants in `src/port/sdl/sdl_game_renderer.c` for flipped textured-rect tasks so the software renderer could stay on `SDL_RenderTexture()` instead of `SDL_RenderTextureRotated(... flip=...)`
  - the first cache-build path used `SDL_DuplicateSurface()` and failed on-device for indexed surfaces (`Fatal error: Failed to duplicate SDL surface for flip cache: Blit combination not supported`); the runtime experiment was narrowed to `SDL_ConvertSurface(... ARGB8888)` plus `SDL_FlipSurface()` before retesting
  - reverted the runtime change after broadened MiSTer verification showed the candidate improved the default control's present bucket but regressed total frame time badly on an alternate scene
- [x] Verification evidence:
  - Docker build/install/package succeeded in `3sx-mister-build` for both the attempted runtime diff and the final reverted tree; MiSTer deploy, `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only`, and bounded `launch-osd.sh` smoke runs all passed, with logs still showing dummy/software + fbdev + native and `runtime_rc=124`
  - the first on-device run of the duplicate-surface variant aborted immediately with the indexed-surface duplication error above, which ruled that cache-build shape out before gameplay capture
  - the narrowed convert+flip path improved the default control `ralphpart2-loop10-control-native` to `58.62 / 10.04 / 44.60 ms`, but the broadened `Sean/Elena` check `ralphpart2-loop10-demo-sean-elena` landed at `60.80 / 12.48 / 44.60 ms`; compared with the earlier Loop 9 Sean/Elena reference capture (`51.77 / 1.08 / 47.10 ms`, on a different scene label and frame window), that was strong enough to reject this runtime change for the current cycle
  - after rollback, the rebuilt/redeployed control `ralphpart2-loop10-revert-control-native` returned to the prior behavior shape at `61.16 / 1.38 / 55.92 ms`, confirming the rejected experiment was no longer active on device
- [x] Keep / rollback decision:
  - rollback the preflipped-texture cache experiment; it is not safe to keep because the broadened gameplay check showed a major render-time regression even though the default control's present bucket improved
  - commit hash: `no-commit`
- [x] Review gate:
  - independent review ran through the `debate-code-review` workflow with a separate `codex` reviewer plus one debate round
  - accepted findings:
    - narrowed the Loop 10 guardrail wording so the Sean/Elena regression is recorded as strong rollback evidence for this cycle without claiming a matched permanent cross-scene disproof
    - corrected the Loop 9 research note to reference `Demo_PL_Play_Data` rather than `Demo_Stage_Play_Data` for demo matchup pairs
  - rejected findings: none
  - no rebuild/redeploy was required after the accepted fixes because they were documentation-only
- [x] Next best candidate:
  - do not retry cached preflipped texture variants on the native exact-fit path casually; the current cycle's broadened check was strong enough to reject this experiment, and any future revisit should start with a matched alternate-scene baseline plus a narrower cache-cost hypothesis
  - rank the next loop against the existing multi-scene evidence again, with preference for a different gameplay-safe bottleneck than flip-path substitution on the software-renderer native path

## Loop 9 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - fresh current-`HEAD` Docker build/install/package succeeded in `3sx-mister-build` via `build/mister-loop9baseline`, then redeployed cleanly to MiSTer
  - live MiSTer probe + bounded launch still match the kept baseline: dummy/software + fbdev + native at `1280x720`, `runtime_rc=124`, and `last-run.log` ending in `exit=143`
  - fresh native gameplay capture `ralphpart2-loop9-baseline-native` reproduces the same bottleneck shape as Loop 8: `61.27 / 1.43 / 55.86 ms` frame/render/present with `present_readback=55.22 ms`, `rect_run_links=170.25`, `rect_color_breaks=0`, `rect_flip_breaks=12.29`, `set_texture_calls=228.35`, and `mtrans_tasks=86.47`
  - SDL primary sources rule out the most tempting low-effort runtime tweak: official SDL docs still define `SDL_SetTextureColorMod()` / `SDL_SetTextureAlphaMod()` as durable texture properties, `SDL_render.c` simply stores that state on the texture, and `SDL_render_sw.c` still reruns `PrepTextureForCopy()` (`SDL_SetSurfaceColorMod`, `SDL_SetSurfaceAlphaMod`, `SDL_SetSurfaceBlendMode`) for every queued copy; caching redundant texture-mod setter calls on the app side would not remove the dominant software-renderer per-copy work
  - local code inspection shows the current representative gate is still scene-limited: every kept gameplay perf JSON on this branch is `scene = gameplay-idle`, while `src/test/test_runner.c` already provides a deterministic menu-to-match harness and `src/sf33rd/Source/Game/demo/demo02.c` includes curated demo matchup pairs (`{Chun-Li, Remy}`, `{Ken, Twelve}`, `{Ryu, Makoto}`, `{Sean, Elena}`) that can broaden character/workload coverage safely if the harness is parameterized
  - local game-path inspection limits the current harness scope: `Setup_Battle_Country()` in `src/sf33rd/Source/Game/screen/sel_pl.c` is character-driven only in non-versus flows, while the current `gameplay-idle` path is an idle-versus harness, so this loop should treat matchup overrides as fighter/workload diversification only and leave explicit stage diversity as future work
- [x] Research-backed hypothesis to test this cycle:
  - do not force a runtime optimization on the current single-scene evidence; instead, keep the cycle scoped to test-runner/perf-capture infrastructure that broadens the deterministic gameplay scene set safely
  - add optional test-runner character and super-art overrides, preserve the existing Ryu/Ken defaults when unspecified, and plumb those overrides through `tools/mister/perf-sampler.sh` so MiSTer captures can sweep alternate idle-versus matchups without affecting normal gameplay/runtime behavior
  - use the repo-local `Demo_PL_Play_Data` matchup pairs as the initial broad-scene sweep after the code change, then compare their `present_readback`, `mtrans_tasks`, and submission-shape metrics against the default control to identify whether the remaining bottleneck is scene-specific or general across fighter/workload mixes
- [x] Success metric and verification plan for the attempted loop:
  - Docker build/package succeeds in `3sx-mister-build`, MiSTer deploy + probe + bounded launch still pass, and the default `gameplay-idle` control remains consistent when no new test-runner overrides are supplied
  - at least one alternate deterministic matchup capture completes on-device through the new perf-sampler path, and the resulting data is sufficient to rank whether the next safe optimization should stay on the native submission path or move to a different gameplay bottleneck
  - run the required independent review pass on the diff, apply only high-confidence in-scope fixes, then rebuild/redeploy/re-capture if any accepted fix changes the measurement path
- [x] Rejected before implementation this cycle:
  - do not spend a runtime loop on app-side texture color/alpha-mod caching; current SDL primary sources show it would only skip durable texture-property writes on submission, not the dominant per-copy software-renderer surface-mod work inside `PrepTextureForCopy()`
  - do not reopen same-texture strip batching, geometry batching, or native-present micro-optimizations on the current `gameplay-idle` gate; Loop 7/8 evidence plus the fresh baseline still leave those ideas unsupported on this scene

## Loop 9 Outcome

- [x] Implementation summary:
  - added optional test-runner character and super-art overrides in `src/main.c`, `src/main.h`, and `src/test/test_runner.c`, while preserving the existing default Ryu/Ken idle-versus path when overrides are absent
  - extended `tools/mister/perf-sampler.sh` to accept character names or ids for those overrides, then accepted the review fixes to require `--gameplay-idle` for override-driven runs and to canonicalize numeric ids to base-10 before forwarding them to the app
  - used the new path to sweep the repo-local demo matchup pairs on MiSTer: `Chun-Li/Remy`, `Ken/Twelve`, `Ryu/Makoto`, and `Sean/Elena`
- [x] Verification evidence:
  - Docker build/install/package of the measurement-support diff succeeded in `3sx-mister-build` via `build/mister-loop9scene`; MiSTer deploy, `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only`, and bounded `launch-osd.sh` smoke run all passed with the expected dummy/software + fbdev + native path and `last-run.log` ending in `exit=143`
  - post-change default control `ralphpart2-loop9-control-native` stayed consistent with the fresh baseline and prior kept telemetry: `60.17 / 1.36 / 55.04 ms` frame/render/present with `present_readback=54.42 ms`, `set_texture_calls=228.35`, and `mtrans_tasks=86.47`
  - alternate matchup captures all stayed on `current_target_exact` + `SDL_PIXELFORMAT_ARGB8888` and remained readback-dominated, but the workload shape varied materially: `Chun-Li/Remy` = `49.06 / 1.30 / 40.67 ms` with `mtrans=186.06`, `set_texture=167.24`; `Ken/Twelve` = `49.54 / 1.04 / 45.05 ms` with `mtrans=127.05`, `set_texture=150.21`; `Ryu/Makoto` = `34.76 / 0.92 / 29.90 ms` with `mtrans=104.68`, `set_texture=109.20`; `Sean/Elena` = `51.77 / 1.08 / 47.10 ms` with `mtrans=134.19`, `set_texture=122.28`
  - the default control remained the slowest tested scene even though some alternates carried much higher `mtrans` counts, which means the remaining native bottleneck is not explained by `mtrans` volume alone; across the broadened sweep, `present_readback` still dominates while task count / texture-switch shape moves the severity more than stage-neutral transparency volume
  - after the review fixes, `bash -n tools/mister/perf-sampler.sh` passed, an override-only invocation now fails fast locally, and the decimal-normalization check succeeded on-device with zero-padded inputs forwarded as `--test-p1-character '10' --test-p2-character '9'`
- [x] Keep / rollback decision:
  - keep the measurement-support changes in the working tree and keep the broadened scene findings; close the cycle as `no-commit` because this loop intentionally stopped at scene-broadening infrastructure and evidence gathering, and no runtime optimization was justified strongly enough to checkpoint
  - commit hash: `no-commit`
- [x] Review gate:
  - independent review ran through the `debate-code-review` workflow with a separate `codex` reviewer plus one debate round
  - accepted findings:
    - `override-flags-bypass-gameplay-gate`: fixed by requiring `--gameplay-idle` whenever `--test-p*-*` overrides are used
    - `numeric-id-roundtrip-is-not-base10-safe`: fixed by canonicalizing digit-only character ids to base-10 in `tools/mister/perf-sampler.sh`
    - `stage-coverage-goal-not-implemented`: narrowed the loop documentation so Loop 9 now claims fighter/workload diversification only on the current idle-versus harness
  - rejected findings: none
- [x] Next best candidate:
  - add an explicit versus-stage override or a deterministic non-versus/demo capture path before spending another optimization loop on “scene diversity”; the current matchup override path is useful, but it does not broaden stage on the idle-versus harness
  - when ranking the next safe runtime optimization, use the new multi-scene evidence instead of the Ryu/Ken control alone; the common bottleneck across tested scenes is still native `present_readback`, but the slowest scene tracks render-task and texture-switch pressure more than raw `mtrans` count

## Loop 8 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - current kept native gameplay telemetry still shows the exact-fit MiSTer gate dominated by queued software-renderer work flushed during present: `ralphpart2-loop7-native-final` = `59.44 / 1.38 / 54.20 ms` frame/render/present mean with `present_readback=53.59 ms`, `present_copy=0.56 ms`, and fixed `copy_bytes.mean=344064`
  - fresh correlation analysis on the same `300`-frame capture keeps the submission-shape signal intact after Loop 7 closed exact strip merging: `present_readback` still tracks `rect_texture_multi_run_tasks ~= 0.613`, `render_task_count ~= 0.606`, `mtrans_tasks ~= 0.606`, and `set_texture_calls ~= 0.587`, while cache churn stays weak (`texture_cache_misses ~= 0.151`, `texture_unlock_calls ~= -0.003`)
  - current local telemetry still stops at texture-only run length plus exact strip adjacency; it does not say whether the slow same-texture runs are actually broken by color modulation changes, flip / `SDL_RenderTextureRotated()` usage, or textured geometry tasks that recover to rect copies versus stay on the geometry path
  - SDL primary sources remain the governing constraint: official docs still warn `SDL_RenderReadPixels()` is slow and returns a newly allocated surface, current `SDL_render.c` still flushes queued render commands before the readback, and the current software renderer still reruns `PrepTextureForCopy()` (`SDL_SetSurfaceColorMod`, `SDL_SetSurfaceAlphaMod`, `SDL_SetSurfaceBlendMode`) for every `SDL_RENDERCMD_COPY`, `SDL_RENDERCMD_COPY_EX`, and textured `SDL_RENDERCMD_GEOMETRY` command; that supports measuring command-shape diversity before any new batching or presenter experiment
- [x] Research-backed hypothesis to test this cycle:
  - keep the cycle scoped to perf-capture telemetry only: add capture-gated renderer stats for same-texture run links, color-break links, flip-break links, flipped rect-copy tasks, and textured geometry tasks that recover to rect copies versus fall back to textured geometry
  - use the new telemetry to decide whether the remaining `mtrans`-heavy bottleneck is still a viable same-texture submission candidate or whether color / flip / geometry diversity is already too high for a safe batching win
  - do not change draw order, renderer selection, texture cache semantics, present behavior, or visible output this cycle; if the new telemetry shows high diversity and low recovery headroom, record that and move to a different bottleneck instead of forcing a runtime optimization
- [x] Success metric and verification plan for the attempted loop:
  - Docker build/package succeeds in `3sx-mister-build`, MiSTer deploy + probe + bounded launch still pass, and a fresh native gameplay capture records the new break-reason fields cleanly
  - the new telemetry should answer whether same-texture runs are mostly interrupted by color / flip transitions or by textured geometry fallback, so the next runtime loop can reject or prioritize submission-path ideas with device evidence
  - run the required independent review pass on the diff, apply only high-confidence in-scope fixes, then rebuild/redeploy/re-capture if any accepted fix changes the telemetry path
- [x] Rejected before implementation this cycle:
  - do not force a runtime batching or presenter change yet; Loop 7 already ruled out exact strip coalescing, and the current capture still does not separate state-diverse same-texture runs from truly batchable ones
  - do not reopen geometry batching as a runtime experiment; current SDL software-renderer source still makes textured geometry a higher-cost path than copy commands unless the new telemetry proves a materially different real-game command shape

## Loop 8 Outcome

- [x] Implementation summary:
  - added perf-capture-only native renderer telemetry for same-texture run links, color-break links, flip-break links, flipped rect-copy tasks, and textured geometry task counts, then threaded those fields into MiSTer perf JSON/log output as schema `5`
  - ran the required independent `codex` review/debate pass, accepted the real fix by switching `backend_logf()` to dynamic formatting so the expanded perf lines no longer truncate in `backend.log`, and narrowed the geometry finding to documentation only because the representative scene recorded zero textured geometry tasks
  - kept the normal gameplay/runtime path unchanged outside perf capture; the representative native gate now shows color diversity is not the remaining same-texture bottleneck on this scene
- [x] Verification evidence:
  - Docker build/install/package succeeded in `3sx-mister-build` via `build/mister-loop8telemetry`; the post-review rebuild was incremental and clean with `cmake --build ... --parallel 4`
  - MiSTer deploy, `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only`, and bounded `launch-osd.sh` smoke runs passed before and after the accepted review fix; backend logs continued to show dummy/software + fbdev presenter + native path at `1280x720`, and `last-run.log` still ended with `exit=143`
  - pre-review native capture `ralphpart2-loop8-native-telemetry` measured `59.41 / 1.36 / 54.24 ms` frame/render/present with `rect_run_links=170.25`, `rect_color_breaks=0.00`, `rect_flip_breaks=12.29`, `rect_flipped_tasks=70.01`, and `textured_geometry_tasks = textured_geometry_rect_recovered_tasks = textured_geometry_fallback_tasks = 0.00`
  - final post-review native capture `ralphpart2-loop8-native-final` measured `60.48 / 1.39 / 55.27 ms` with the same telemetry shape, and the remote `backend.log` now preserves the full summary line through `output=/media/fat/games/3sx/logs/perf-ralphpart2-loop8-native-final.json`
- [x] Keep / rollback decision:
  - keep the telemetry-only change and the backend-log formatting fix; the device evidence closes color / geometry diversity as the next native same-texture bottleneck on the representative gameplay-idle gate without changing gameplay behavior
  - checkpoint commit hash: `1f891f00`
- [x] Review gate:
  - independent review ran through the `debate-code-review` workflow using a separate `codex` reviewer against the loop diff
  - accepted findings:
    - `F2`: the expanded perf logs overran the fixed `backend_logf()` buffer; fixed by replacing the `1024`-byte stack buffer with dynamic `SDL_vasprintf()` formatting, then rebuilding, redeploying, and re-capturing on MiSTer
  - rejected / narrowed findings:
    - `F1`: no extra geometry-break counter was added this cycle because the representative capture already proved the current scene has zero textured geometry tasks across all `300` frames, so geometry is not splitting the native same-texture runs on this gate; the documentation now records that narrower conclusion explicitly
- [x] Next best candidate:
  - stop spending the next loop on color-mod or geometry-diversity telemetry for the current native gameplay-idle gate; Loop 8 closes both with device evidence (`rect_color_breaks=0`, textured geometry counts `=0`)
  - broaden the representative gameplay scene set before another submission-path optimization, or move to a different safe bottleneck than exact-fit native same-texture micro-batching on this scene

## Loop 7 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - current kept native gameplay telemetry still shows the exact-fit MiSTer gate dominated by queued software-renderer work flushed during present: `ralphpart2-loop6-native-final` = `59.79 / 1.24 / 54.70 ms` frame/render/present mean with `present_readback=54.08 ms` and fixed `copy_bytes.mean=344064`
  - fresh quartile analysis of the same 300-frame capture keeps the submission-shape signal intact: the slowest quartile averages `313.64` render tasks, `248.96` multi-run rect tasks, `232.73` `set_texture` calls, and `108.64` `mtrans` tasks versus `274.12` / `212.67` / `224.87` / `69.12` in the fastest quartile
  - local code inspection confirms almost all gameplay draws already land on the `RENDER_TASK_TYPE_TEXTURED_RECT` path via `draw_sprite_rect()` / `SDLGameRenderer_DrawTexturedQuad()`, which means any safe submission win must come from reducing `SDL_RenderTexture()` command count without changing draw order
  - SDL primary sources now sharpen the next constraint: official docs still note `SDL_RenderReadPixels()` flushes the queued renderer before returning a new surface, and current `libsdl-org/SDL` software-renderer source shows every queued `SDL_RENDERCMD_COPY` re-runs `PrepTextureForCopy()` (`SDL_SetSurfaceColorMod`, `SDL_SetSurfaceAlphaMod`, `SDL_SetSurfaceBlendMode`) before the blit, while `SDL_RENDERCMD_GEOMETRY` still rasterizes triangles; this supports a copy-count reduction idea but not another geometry rewrite
- [x] Research-backed hypothesis to test this cycle:
  - keep the cycle scoped to perf-capture telemetry only: measure whether contiguous same-texture rect runs also contain conservative strip-merge candidates that match color, flip, scale, and exact source/destination adjacency
  - split the telemetry by horizontal versus vertical strip candidates and count both strip groups and tasks covered, so the next runtime loop can tell whether a safe `SDL_RenderTexture()` strip coalescer is worth attempting on MiSTer
  - do not change draw order, task generation, renderer selection, present behavior, texture cache semantics, or visible output this cycle; if the capture shows low mergeability, record that and move to a different bottleneck instead of forcing batching code
- [x] Success metric and verification plan for the attempted loop:
  - Docker build/package succeeds in `3sx-mister-build`, MiSTer deploy + probe + bounded launch still pass, and a fresh native gameplay capture records the new mergeability fields cleanly
  - the new telemetry should answer whether the slow frames are dominated by strip-mergeable rect workloads or by same-texture runs that still cannot be coalesced safely
  - run the required independent review pass on the diff, apply only high-confidence in-scope fixes, then rebuild/redeploy/re-capture if any accepted fix changes the telemetry path
- [x] Rejected before implementation this cycle:
  - do not force a runtime strip-batching implementation yet; the current telemetry proves same-texture runs exist, but it still does not prove that adjacent rects match color/flip/scale and exact source/destination contiguity often enough to justify a safe code path
  - do not reopen geometry-based batching or texture/palette unlock churn as the next runtime target; current SDL source plus the kept gameplay telemetry still make both paths lower-confidence than direct mergeability measurement

## Loop 7 Outcome

- [x] Implementation summary:
  - added perf-capture-only native renderer telemetry for conservative horizontal and vertical strip-merge candidates inside successful same-texture rect runs, then threaded the new fields into MiSTer perf JSON/log output as schema `4`
  - ran the required independent `codex` review/debate pass and accepted both telemetry fixes: geometry tasks recovered by the rect-copy path now feed their normalized submitted rect into the strip telemetry, and source-edge adjacency now uses a strip-specific sub-half-texel threshold instead of the generic UV epsilon
  - kept the runtime path behavior unchanged outside perf capture; the final device result is evidence-only and does not introduce a gameplay/runtime optimization
- [x] Verification evidence:
  - Docker build/install/package succeeded in `3sx-mister-build` via `build/mister-loop7telemetry`; the post-review rebuild was incremental and clean with `cmake --build ... --parallel 4`
  - MiSTer deploy, `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only`, and bounded `launch-osd.sh` smoke runs passed before and after the review fixes; backend logs continued to show dummy/software + fbdev presenter + native path at `1280x720`, `runtime_rc=124`, and `last-run.log` still ended with `exit=143`
  - pre-review native capture `ralphpart2-loop7-native-telemetry` measured `60.37 / 1.29 / 55.28 ms` frame/render/present with `rect_hstrip_runs=0.00`, `rect_hstrip_tasks=0.00`, `rect_vstrip_runs=0.00`, and `rect_vstrip_tasks=0.00`
  - final post-review native capture `ralphpart2-loop7-native-final` measured `59.44 / 1.38 / 54.20 ms` frame/render/present with `present_readback=53.59 ms`; all new strip metrics stayed zero across all `300` frames (`nonzero_hstrip_frames=0`, `nonzero_vstrip_frames=0`)
- [x] Keep / rollback decision:
  - keep the telemetry-only change and reject exact strip coalescing as the next runtime loop on the current gameplay-idle native gate; even after the accepted review fixes, the representative capture shows no exact horizontal or vertical strip candidates to coalesce safely
  - checkpoint commit hash: `9acdf0bf`
- [x] Review gate:
  - independent review ran through the `debate-code-review` workflow using a separate `codex` reviewer against the loop diff
  - accepted findings:
    - `F1`: geometry tasks recovered by `try_submit_geometry_task_as_rect_copy()` were extending run telemetry without contributing to strip telemetry; fixed by returning the normalized submitted rect and recording that rect shape in the strip counters
    - `F2`: the initial source-edge adjacency check reused the generic normalized-UV epsilon, which was too loose for exact strip telemetry on `1024px` textures; fixed by switching strip adjacency to a texture-size-derived sub-half-texel threshold
  - rejected findings: none
- [x] Next best candidate:
  - do not spend the next runtime loop on exact source/destination strip batching for the current native gameplay gate; Loop 7 closes that specific idea with zero candidates
  - next research target should profile what still breaks same-texture runs in the mtrans-heavy slow bucket, especially color/flip diversity and geometry-recovery frequency, or broaden the gameplay scene set before another submission-path optimization

## Loop 6 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - current kept native gameplay telemetry still shows the exact-fit MiSTer gate dominated by the software-renderer present bucket: `ralphpart2-loop4-native-final` = `60.19 / 1.26 / 55.10 ms` frame/render/present mean with `present_readback=54.47 ms`, `present_copy=0.57 ms`, `render_task_count.mean=291.47`, `rect_copy_tasks.mean=283.47`, and `texture_creates.mean=4.41`
  - the existing capture only reports aggregate task/cache totals, so it still cannot answer whether the queued workload is dominated by redundant textured-rect submission shape or by unlock-driven cache invalidation churn
  - local code inspection shows `SDLGameRenderer_UnlockTexture()` and `SDLGameRenderer_UnlockPalette()` invalidate cached SDL textures unconditionally after every game-side unlock, while the current perf output does not report unlock counts, invalidation fan-out, or queued destroys
  - local game-path inspection narrowed the live unlock sources to `ppgRenewTexChunkSeqs()` plus palette updates in `palUpdateGhostDC()` / `palUpdateGhostCP3()`, which means the current native gate likely depends on a small set of frequently renewed assets rather than broad texture creation every frame
  - official SDL primary sources changed the interpretation of the present breakdown: the SDL wiki still states `SDL_RenderReadPixels()` is slow and returns a newly allocated `SDL_Surface`, and the current `libsdl-org/SDL` source flushes queued render commands before the readback, so the software-renderer rasterization cost can be hidden inside the `present_readback` bucket instead of the `render_ms` bucket
- [x] Research-backed hypothesis to test this cycle:
  - keep this loop scoped to perf-capture telemetry only: add capture-gated renderer stats for contiguous textured-rect runs, redundant texture-binding hits, unlock call counts, cache invalidation counts, and queued texture destroys
  - use the new telemetry to decide whether the next safe runtime loop should target submission/run-shape churn or unlock-driven texture/palette cache churn; do not guess at another runtime optimization until this evidence exists on device
  - do not change gameplay logic, render ordering, presenter math, texture formats, cache semantics, or visible output behavior this cycle
- [x] Success metric and verification plan for the attempted loop:
  - Docker build/package succeeds in `3sx-mister-build`, MiSTer deploy + `run-3sx.sh --probe-renderer-only` + bounded launch still pass, and a fresh native gameplay capture records the new renderer telemetry fields cleanly
  - the fresh capture should make the next optimization choice data-backed by showing whether rect-copy work is concentrated in longer same-texture runs and how much cache churn is actually triggered by unlocks
  - run the required independent review pass on the diff, apply only high-confidence in-scope fixes, then rebuild/redeploy/re-capture if any accepted fix changes the telemetry path
- [x] Rejected before implementation this cycle:
  - do not retry another presenter micro-optimization just because `present_readback` is large; SDL source now shows that bucket includes queued software-renderer work before the copy itself
  - do not reopen direct target-locking, direct framebuffer software-renderer, or synthetic geometry-batching runtime experiments without new live telemetry proving a safe win path first

## Loop 6 Outcome

- [x] Implementation summary:
  - added perf-capture-only renderer telemetry in `SDLGameRenderer` / `SDLApp` for same-texture textured-rect runs, redundant texture-binding reuse, texture/palette unlock counts, cache-invalidation fan-out, and queued texture destroys
  - bumped the MiSTer perf capture schema to `3`, threaded the new counters into backend logs plus JSON summaries/samples, and capture-gated the extra accounting so normal runtime behavior stays unchanged outside perf collection
  - ran an independent `codex` review pass and accepted the one runtime-adjacent telemetry fix: only count rect-texture runs after the rect path actually submits, not when a geometry fallback keeps the task on the slower path
- [x] Verification evidence:
  - Docker `PORT_MISTER=ON` build/install/package succeeded in `3sx-mister-build` via `build/mister-loop6telemetry`; the first default-parallel build was OOM-killed, so the accepted verification path uses `cmake --build ... --parallel 4`
  - MiSTer deploy, `/media/fat/games/3sx/run-3sx.sh --probe-renderer-only`, and a bounded startup launch all passed after deploy; remote logs continued to show dummy/software + fbdev presenter + native path, and `last-run.log` still ended with `exit=143`
  - fresh native telemetry before the review fix (`ralphpart2-loop6-native-telemetry`) and after the accepted fix (`ralphpart2-loop6-native-final`) kept the same overall shape; final means were `59.79 / 1.24 / 54.70 ms` frame/render/present with `present_readback=54.08 ms`
  - the 300-frame telemetry analysis now points at submission/run shape, not unlock churn: `rect_texture_multi_run_tasks -> present_readback ~= 0.609`, `render_task_count ~= 0.606`, `set_texture_calls ~= 0.597`, `rect_texture_multi_runs ~= 0.591`, versus `texture_unlock_calls ~= 0.031`, `texture_cache_evictions ~= 0.043`, and `palette_cache_evictions ~= -0.015`
- [x] Keep / rollback decision:
  - keep the telemetry-only change; it does not alter gameplay/render behavior, it passed the required review-and-reverify gate on device, and it closes the next bottleneck decision with higher-confidence evidence
  - checkpoint commit hash: `72b1dcc1`
- [x] Review gate:
  - independent review ran through the `debate-code-review` workflow using a separate `codex` reviewer against the loop diff
  - accepted findings:
    - `F1`: rect-run telemetry could overcount when `submit_rect_task()` fell back to geometry; fixed by counting only confirmed rect-path submissions and flushing pending run state on fallback
    - `F2`: the loop ledger still had placeholder `pending` entries; fixed by recording the final loop outcome in this checklist instead of leaving the cycle partially closed
  - rejected findings: none
- [x] Next best candidate:
  - add conservative mergeability telemetry inside the confirmed same-texture rect runs, specifically whether adjacent tasks also match color/flip and contiguous source/destination layout, before attempting any rect-strip batching or submission rewrite
  - deprioritize unlock-driven texture/palette invalidation as the next runtime target; the new capture shows that churn is weakly correlated with the real `present_readback` bottleneck compared with submission/run-shape metrics

## Loop 5 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - current native telemetry from kept commit `ce9c7110` still shows the representative gameplay gate dominated by the exact-fit present step: `ralphpart2-loop4-native-final` = `60.19 / 1.26 / 55.10 ms` frame/render/present mean with `present_readback=54.47 ms`, `present_copy=0.57 ms`, `render_task_count.mean=291.47`, `rect_copy_tasks.mean=283.47`, and `texture_creates.mean=4.41`
  - correlation on the same 300-frame sample shows task count tracks the slow bucket more than texture churn: `render_task_count -> present_readback ~= 0.615`, `texture_creates -> present_readback ~= 0.197`
  - local code inspection narrowed the safest remaining exact-path candidates to two small ideas only: skip the explicit `SDL_SetRenderTarget(renderer, cps3_canvas)` before direct present, or batch contiguous textured-rect runs to reduce `SDL_RenderTexture()` submission count
  - SDL primary-source review through the official wiki plus `libsdl-org/SDL` source ruled out the retarget idea immediately: `SDL_SetRenderTarget()` already returns early when the requested target matches the current target, and `SDL_RenderGeometry()` uses per-vertex color while the software renderer walks queued triangles sequentially, which makes geometry batching semantically plausible but still performance-sensitive
  - a container microbenchmark inside `3sx-mister-build`, using dummy/software renderer + `384x224` target texture + per-frame `SDL_RenderReadPixels()`, showed the synthetic single-texture `RUN_LEN=3` proxy that includes per-frame vertex generation was dramatically slower than the current rect-copy submission shape (`copy_ms=2.699`, `geometry_run_ms=26.547`); reproduction lives in `tools/mister/bench-rect-batching.sh`
- [x] Research-backed hypothesis to test this cycle:
  - keep the cycle research-only unless the current evidence supports a safe, high-confidence runtime win
  - reject the same-target retarget skip if SDL source confirms it is already a no-op
  - reject contiguous textured-rect geometry batching if the container benchmark loses materially under the same software-renderer + readback shape used by the MiSTer path
- [x] Success metric and verification plan for the attempted loop:
  - rebuild/package current `HEAD` in `3sx-mister-build` so the research result is anchored to the live container state
  - if a candidate survives research, implement one small runtime change and then run the full device gate
  - otherwise record a no-keep result, update durable notes, and move the next cycle to a different bottleneck instead of forcing a risky change
- [x] Rejected before implementation this cycle:
  - do not spend a runtime loop on the explicit `SDL_SetRenderTarget(renderer, cps3_canvas)` call in `SDLApp_EndFrame()`; official SDL source shows the unchanged-target case already returns immediately
  - do not open a runtime loop on the synthetic single-texture `RUN_LEN=3` `SDL_RenderGeometry()` benchmark shape; it lost badly in-container, and any real batching revisit now needs live run-length telemetry first

## Loop 5 Outcome

- [x] Implementation summary:
  - kept the runtime tree unchanged after research disproved the retarget idea and showed the synthetic single-texture `RUN_LEN=3` batching proxy loses badly
  - corrected the prior Loop 4 checkpoint reference to the actual keep commit `ce9c7110`
  - promoted the one-off rect-submission microbenchmark into repo-local script `tools/mister/bench-rect-batching.sh` so the rejection remains auditable without depending on ignored local logs
  - recorded the blocked result and the new do-not-retry note so the next loop can move to a different candidate instead of rediscovering the same dead end
- [x] Verification evidence:
  - fresh `PORT_MISTER=ON` configure/build/install/package of current `HEAD` succeeded in `3sx-mister-build` via `build/mister-loop5research`
  - official SDL docs/source confirmed the same-target `SDL_SetRenderTarget()` call is already a no-op in SDL itself
  - the synthetic single-texture `RUN_LEN=3` container microbenchmark reproducing the software-renderer + target-texture + readback shape showed the "per-frame vertex generation + `SDL_RenderGeometry()`" proxy loses badly versus `SDL_RenderTexture()` submissions (`2.699 ms` vs `26.547 ms`); reproduction lives in `tools/mister/bench-rect-batching.sh`
  - after the review-driven harness hardening, rerunning `tools/mister/bench-rect-batching.sh` kept the same result shape (`2.703 ms` vs `26.467 ms`)
  - no device deploy/run was taken because research eliminated both candidate runtime changes before implementation
- [x] Keep / rollback decision:
  - keep no runtime change this cycle; the research did not support a safe/high-confidence optimization, so forcing one would risk behavior for no evidence-backed upside
  - commit hash: `no-commit`
- [x] Review gate:
  - independent review ran via the `debate-code-review` workflow using a separate `codex` reviewer against the record-only diff
  - accepted findings:
    - `F1`: the durable workflow text and unattended prompt still described every cycle as if it must end in a device-verified commit, so the contract language was tightened to explicitly allow research-only no-runtime-change closes and reverted-runtime no-commit closes
    - `F2`: the geometry-batching rejection now points at repo-local reproduction script `tools/mister/bench-rect-batching.sh` instead of ignored local log artifacts
    - `F3`: the stale Loop 0 kickoff wording that still implied a research-only cycle should commit was narrowed to "record the research/update state cleanly"
    - `F4`: the durable rejection was narrowed to the specific synthetic single-texture `RUN_LEN=3` proxy that was actually disproved, with live run-length telemetry kept as a prerequisite for any real revisit
    - `F5`: `tools/mister/bench-rect-batching.sh` was hardened to resolve Docker from `PATH` first, isolate container temp paths, respect surface pitch, and fail fast on SDL errors before printing timings
  - final rerun on the updated diff returned no findings
  - rejected findings: none
- [x] Next best candidate:
  - add targeted render-submission telemetry before another runtime change, specifically contiguous texture-run lengths and unlock-driven texture/palette invalidation so the next safe candidate is data-backed instead of speculative
  - keep low-resolution clipped-native verification open as a separate safety follow-up, not the next performance loop

## Loop 4 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - current representative native gameplay evidence still shows the remaining bottleneck is present-path work, not render submission: `ralphpart2-loop3-native-final-rerun` = `60.27 / 1.23 / 55.18 ms` frame/render/present mean with fixed `copy_bytes.mean=344064`
  - square-pixels is no longer the lead blocker after the kept integer-expand fast path: `ralphpart2-loop3-square-final` = `74.53 / 1.30 / 69.39 ms`, materially better than the earlier `97.38 / 1.42 / 92.10 ms` control from `ralphpart2-loop2-square-pre`
  - local code inspection shows the current perf capture records only total `present_ms`, `copy_bytes`, and tile-diff counters; the kept native exact-fit path in `src/port/sdl/fbdev_presenter.c` still collapses `SDL_RenderReadPixels(renderer, NULL)`, any format conversion, the fbdev blit, and bar clears into one present bucket
  - SDL3 primary-source docs via `context7` reaffirm the constraint that `SDL_RenderReadPixels()` returns a newly allocated `SDL_Surface`, reads from the current render target or viewport rect, and is explicitly a slow operation; current docs still do not expose a supported direct-lock path for the `SDL_TEXTUREACCESS_TARGET` native render target
  - analysis of the existing native capture shows present-time variation tracks scene/task count only moderately while copy volume stays completely flat, which is consistent with readback/allocation dominating over framebuffer bandwidth but is still not precise enough to justify another runtime experiment safely
- [x] Research-backed hypothesis to test this cycle:
  - keep this loop scoped to telemetry only: add perf-capture-only fbdev present breakdown stats so native and square-pixels captures report how much time was spent in readback, conversion, fbdev copy/scale, and bar clear work, plus which presenter path ran and what readback pixel format SDL returned
  - do not change presenter math, render ordering, scaling behavior, target formats, copy volume, or fallback selection this cycle; the objective is to close the measurement gap that blocked the last native optimization decision
  - if the new telemetry confirms native exact-fit is overwhelmingly `SDL_RenderReadPixels()`-bound, treat that as evidence to skip more fbdev micro-optimizations and move to a different safe candidate on the next loop
- [x] Success metric and verification plan for the attempted loop:
  - Docker build/package succeeds in `3sx-mister-build`, MiSTer deploy + probe + bounded launch still pass, and gameplay startup behavior remains unchanged
  - a fresh native perf capture gains stable breakdown fields that show the relative cost of readback versus conversion/copy/clear on the kept current-target path
  - run the required independent review pass on the diff, apply only high-confidence in-scope fixes, then rebuild/redeploy/reverify before committing
- [x] Rejected before implementation this cycle:
  - do not force another native runtime optimization before this breakdown telemetry exists; the current evidence is strong enough to suspect SDL readback, but not strong enough to justify another safe code-path change
  - do not broaden this loop into low-resolution live validation or another surface/texture format experiment; those are separate follow-ups once the present bucket is better attributed

## Loop 4 Outcome

- [x] Implementation summary:
  - added perf-capture-only fbdev presenter breakdown stats for `readback`, `convert`, `copy`, and `clear` time, plus per-frame presenter path and SDL readback surface format/size metadata
  - threaded the new presenter stats into the MiSTer perf JSON/log output in `src/port/sdl/sdl_app.c`, bumping the capture schema to `2` and recording the new per-sample fields alongside the existing frame/render/present metrics
  - accepted and applied both independent review findings: staging `copy_ns` now times only actual write helpers, and fallback frames reset presenter stats before the retry so one sample no longer merges two presenter attempts
- [x] Verification evidence:
  - Docker build/package succeeded twice in `3sx-mister-build` via `build/mister-loop4b`; the review-fix rebuild was incremental and clean
  - MiSTer deploy, `run-3sx.sh --probe-renderer-only`, and bounded `launch-osd.sh` smoke run passed before and after the review fixes; `last-run.log` still ended with `exit=143`, and backend logs continued to confirm dummy video + software renderer + fbdev presenter + `scale-mode=native` on a `1280x720` framebuffer
  - pre-review native telemetry sample `ralphpart2-loop4-native-post`: `60.94 / 1.25 / 55.78 ms` frame/render/present mean with `present_readback=55.14 ms`, `present_convert=0.00 ms`, `present_copy=0.58 ms`, `present_clear=0.00 ms`, `fbdev_present_path.current_target_exact.ratio=1.0`, and `readback_surface={ARGB8888, 384x224}`
  - post-review native telemetry sample `ralphpart2-loop4-native-final`: `60.19 / 1.26 / 55.10 ms` frame/render/present mean with `present_readback=54.47 ms`, `present_convert=0.00 ms`, `present_copy=0.57 ms`, `present_clear=0.00 ms`, `fbdev_present_path.current_target_exact.ratio=1.0`, and the same stable `ARGB8888 384x224` readback surface
- [x] Keep / rollback decision:
  - keep the telemetry change; it does not alter gameplay/render behavior, it survived the required review-and-reverify gate, and it closes the measurement gap that blocked the next native optimization decision
  - checkpoint commit hash: `ce9c7110`
- [x] Review gate:
  - independent review/debate ran via the `debate-code-review` workflow using a separate `codex` reviewer process
  - accepted findings:
    - `F1`: staging `copy_ns` was timing diff/decision work as well as writes; fixed by moving staging copy timing into `copy_staging_tile_to_fb()` and `copy_full_staging_to_fb()`
    - `F2`: fallback recovery could merge two presenter attempts into one telemetry sample; fixed by resetting presenter stats before the fallback `FBDevPresenter_Present()` call in `SDLApp_EndFrame()`
  - rejected findings: none
- [x] Next best candidate:
  - do not spend another loop on native exact-fit fbdev copy/format/bar-clear micro-optimizations unless the SDL readback floor changes first; the new telemetry shows `SDL_RenderReadPixels()` itself is effectively the whole bottleneck on the kept path
  - next research target should be a safe, primary-source-backed way to avoid or materially reduce native `SDL_RenderReadPixels()` allocation/readback cost, or a different gameplay-safe bottleneck outside the current-target fb copy path

## Loop 3 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - current kept MiSTer gameplay evidence still points to native exact-fit present as the real remaining bottleneck: `ralphpart2-loop2-native-final` stayed at `60.19 / 1.21 / 55.16 ms` frame/render/present mean while square-pixels had already improved to `73.52 / 1.31 / 68.37 ms`
  - current device logs still confirm the intended runtime path on MiSTer: dummy video driver, software renderer, fbdev presenter active at `1280x720`, native render path enabled, and bounded launch exits cleanly with `exit=143`
  - SDL3 primary-source docs via `context7` still rule out the old bypass ideas: `SDL_RenderReadPixels()` returns a newly allocated surface, is explicitly documented as slow, and render-target textures are still not lockable because only `SDL_TEXTUREACCESS_STREAMING` supports `SDL_LockTexture*`
  - an isolated SDL probe built inside `3sx-mister-build` showed the software renderer advertises `SDL_PIXELFORMAT_ARGB8888` as a supported texture format, while `SDL_RenderReadPixels()` returns `SDL_PIXELFORMAT_ARGB8888` even when the target texture was created as `SDL_PIXELFORMAT_RGBA8888`
  - local code inspection confirms `cps3_canvas` is still created as `SDL_PIXELFORMAT_RGBA8888` in `src/port/sdl/sdl_game_renderer.c`, while the fbdev present path treats `SDL_PIXELFORMAT_ARGB8888` as the no-conversion fast case; this makes a target-format mismatch the highest-confidence remaining present-path candidate
- [x] Research-backed hypothesis to test this cycle:
  - keep the loop scoped to the main game render target only: switch `cps3_canvas` from `SDL_PIXELFORMAT_RGBA8888` to `SDL_PIXELFORMAT_ARGB8888` so native/square current-target readback can stay in the software renderer's observed native readback format
  - do not change gameplay logic, render ordering, scaling math, presenter placement, or buffer dimensions; only align the target texture format with the documented/observed readback format
  - leave `screen_texture`, `message_canvas`, and unsupported readback-bypass ideas untouched this cycle; if the focused `cps3_canvas` change does not win safely on MiSTer, roll it back and move to a narrower telemetry loop instead
- [x] Success metric and verification plan for the attempted loop:
  - first refresh a current-tree baseline in `3sx-mister-build`, redeploy it, and record fresh native plus square-pixels gameplay controls before editing
  - after the scoped texture-format change, keep the native gameplay gate flat or better overall while improving native `present_ms` measurably versus the fresh pre-control
  - treat square-pixels as a secondary guardrail because it also uses current-target present; it must stay flat within noise or improve, with unchanged `copy_bytes` and unchanged visible output placement/scale-mode behavior
  - verify with Docker build/package, MiSTer deploy, `run-3sx.sh --probe-renderer-only`, bounded `launch-osd.sh`, fresh native control, fresh square-pixels control, remote log inspection, and a fresh independent review pass before commit
- [x] Rejected before implementation this cycle:
  - do not reopen target-texture locking, caller-provided readback-buffer, or surface-backed renderer swap ideas; current SDL docs still do not support those shapes for this path
  - do not broaden the experiment to `message_canvas` or `screen_texture` until the narrower `cps3_canvas` format-alignment change proves safe and useful on device

## Loop 3 Outcome

- [x] Implementation summary:
  - changed `cps3_canvas` in `src/port/sdl/sdl_game_renderer.c` from `SDL_PIXELFORMAT_RGBA8888` to `SDL_PIXELFORMAT_ARGB8888` so the MiSTer software-renderer target matched the observed `SDL_RenderReadPixels()` format
  - rebuilt, redeployed, and verified the scoped runtime change on MiSTer, then reverted the code change after the native gameplay gate failed to improve
  - rebuilt and redeployed the reverted tree, then re-ran the MiSTer controls to confirm the rollback state before closing the cycle
- [x] Verification evidence:
  - fresh current-tree pre-controls after Docker build/package + redeploy + required probe/bounded launch: native `ralphpart2-loop3-native-pre` = `60.30 / 1.22 / 55.21 ms`; square-pixels `ralphpart2-loop3-square-pre` = `74.25 / 1.27 / 69.17 ms`
  - candidate runtime result after the ARGB8888 change: native `ralphpart2-loop3-native-post` = `60.40 / 1.22 / 55.39 ms`; square-pixels `ralphpart2-loop3-square-post` = `73.33 / 1.30 / 68.24 ms`
  - because the targeted native exact-fit bottleneck moved the wrong way instead of improving, the runtime change was rolled back even though square-pixels moved slightly better
  - reverted-tree verification passed through the same Docker/package + MiSTer probe/bounded-launch path, with `last-run.log` still ending in `exit=143` and the device config restored to `scale-mode = native`
  - rollback confirmation: first reverted native sample `ralphpart2-loop3-native-final` came back high at `61.29 / 1.30 / 56.14 ms`, so an immediate rerun was taken; accepted rollback native control `ralphpart2-loop3-native-final-rerun` = `60.27 / 1.23 / 55.18 ms`, and reverted square-pixels `ralphpart2-loop3-square-final` = `74.53 / 1.30 / 69.39 ms`
  - copy volume stayed unchanged throughout the experiment (`344064` native, `3096576` square-pixels), so the attempted format swap did not change visible output coverage or presenter write volume
- [x] Keep / rollback decision:
  - rollback the `cps3_canvas` ARGB8888 format swap; the researched hypothesis was reasonable, but the live MiSTer native exact-fit gate did not improve on the actual bottleneck, so there is no justification to keep the runtime change
  - commit hash: `no-commit`
- [x] Review gate:
  - independent review pass on the isolated diff reported no actionable gameplay/correctness findings for the one-line target-format change itself
  - accepted fixes: none
  - rejected findings: none; the keep/rollback decision was driven by device performance evidence rather than review concerns
- [x] Next best candidate:
  - add narrower native exact-fit telemetry around the current-target `SDL_RenderReadPixels()` path so the next loop can separate readback cost from surface-format/copy assumptions before another presenter experiment
  - do not retry the `cps3_canvas` ARGB8888 target-format swap as a standalone optimization unless future telemetry shows the native path has changed materially

## Loop 2 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - fresh Docker rebuild/package of current `HEAD` succeeded in `3sx-mister-build` via `build/mister-cycle`, then redeployed cleanly to MiSTer
  - required device preflight passed on the redeployed package: `run-3sx.sh --probe-renderer-only` confirmed dummy/software + fbdev, and the bounded `launch-osd.sh` smoke run exited with `143` and no measurement-poisoning orphan cleanup was needed
  - fresh representative native gameplay control `ralphpart2-loop2-native-pre`: `60.54 / 1.22 / 55.50 ms` frame/render/present mean, `copy_bytes.mean=344064`
  - fresh representative square-pixels gameplay control `ralphpart2-loop2-square-pre`: `97.38 / 1.42 / 92.10 ms` frame/render/present mean, `copy_bytes.mean=3096576`
  - local code inspection confirms the current square-pixels MiSTer path is a fixed unclipped `384x224 -> 1152x672` integer-3x present on the live `1280x720` framebuffer, while `copy_argb_surface_scaled_to_fb_mapped_rect()` still computes per-pixel divisions on every unique source row
  - SDL3 primary-source docs via `context7` still describe `SDL_RenderReadPixels()` as slow and still limit direct texture locking to `SDL_TEXTUREACCESS_STREAMING`; the native exact-fit target remains `SDL_TEXTUREACCESS_TARGET`, so there is still no supported target-texture lock bypass for the native path
- [x] Research-backed hypothesis to test this cycle:
  - do not reopen native exact-fit readback-bypass work this cycle; the dominant native cost is still the SDL readback itself, and the supported SDL API surface has not changed
  - optimize only the unclipped integer-scale current-target presenter path in `src/port/sdl/fbdev_presenter.c`
  - add a dedicated integer-expand fast path for exact integer-scaled mapped rects so square-pixels no longer pays per-pixel division/remap work for the live `3x` MiSTer case, while preserving the existing generic mapped-scale path for clipped or non-integer cases
- [x] Success metric and verification plan for the attempted loop:
  - keep `ralphpart2-loop2-native-pre` flat within noise on the native control gate
  - improve square-pixels `present_ms` measurably versus `ralphpart2-loop2-square-pre` without changing `copy_bytes`, output placement, or scale-mode behavior
  - verify with Docker rebuild/package, MiSTer deploy, probe, bounded launch, fresh native control, fresh square-pixels control, and a fresh review pass before commit
- [x] Rejected before implementation this cycle:
  - do not retry target-texture locking or other native readback bypass ideas; SDL still documents `SDL_RenderReadPixels()` as slow and only streaming textures as lockable
  - do not broaden this loop into another renderer-architecture swap; the safe scoped opportunity from current evidence is the square-pixels integer-expand presenter path only

## Loop 2 Outcome

- [x] Implementation summary:
  - added an exact integer-expand fast path in `src/port/sdl/fbdev_presenter.c` for unclipped integer-scaled mapped current-target presents
  - kept the existing generic mapped-scale path as the fallback for clipped or non-integer destination rects, so native exact-fit and other presenter cases keep their prior behavior
- [x] Verification evidence:
  - review-adjusted Docker rebuild/package succeeded in `3sx-mister-build`; redeploy, `run-3sx.sh --probe-renderer-only`, and bounded `launch-osd.sh` smoke run all passed on MiSTer, with `last-run.log` ending in `exit=143`
  - native pre `ralphpart2-loop2-native-pre`: `60.54 / 1.22 / 55.50 ms`; final native `ralphpart2-loop2-native-final`: `60.19 / 1.21 / 55.16 ms`
  - square-pixels pre `ralphpart2-loop2-square-pre`: `97.38 / 1.42 / 92.10 ms`; final square-pixels `ralphpart2-loop2-square-final`: `73.52 / 1.31 / 68.37 ms`
  - copy volume stayed unchanged through the keep-path verification (`344064` native, `3096576` square-pixels), confirming the win came from removing per-pixel remap/divide work rather than changing visible output coverage
  - remote config was restored to `scale-mode = native` after the final verification pass
- [x] Keep / rollback decision:
  - keep the integer-expand fast path; it materially improves the live square-pixels MiSTer gate while leaving the native control flat to slightly improved
  - checkpoint commit hash: `56b01376`
- [x] Review gate:
  - independent review/debate ran via the `debate-code-review` workflow with a separate `claude` reviewer process
  - accepted review fix: widen the new row-offset operands before multiplication in `src/port/sdl/fbdev_presenter.c` so the helper matches the file's existing safe pointer-arithmetic pattern
  - rejected findings: the dead `scale_x/scale_y` guard and the extra local `argb == NULL` guard were both treated as low-value churn, not correctness issues
- [x] Next best candidate:
  - native exact-fit present remains SDL-readback-bound; the next high-confidence performance candidate is narrower native readback-versus-copy telemetry or another official-source-backed experiment that can reduce native `SDL_RenderReadPixels()` cost safely
  - low-resolution live verification of the kept clipped-native crop branch remains open, but it is a safety follow-up rather than the next performance loop

## Loop 1 Research Snapshot

- [x] Research evidence gathered on `2026-03-06` before implementation:
  - clean Docker rebuild/package of current HEAD succeeded in `3sx-mister-build` via a fresh `build/mister-cycle` tree
  - fresh deploy + `run-3sx.sh --probe-renderer-only` + bounded `launch-osd.sh` smoke run recreated the expected MiSTer defaults (`scale-mode = native`, `video-driver-order = dummy`, `render-driver-order = software`)
  - the first unmodified native gameplay control after that bounded launch regressed badly again: `ralphpart2-loop1-native-control` = `125.04 / 2.71 / 114.52 ms`
  - immediate device-state inspection showed two orphaned `/media/fat/games/3sx/bin/3sx` processes still running under `PPid=1`, consuming about `42%` and `25%` CPU respectively, with the MiSTer core process also active
  - after `killall 3sx`, the same native gameplay control returned to baseline shape without any code change: `ralphpart2-loop1-native-after-killall` = `60.95 / 1.23 / 55.85 ms`
- [x] Research-backed hypothesis to test this cycle:
  - the bounded-launch verification step is leaking orphaned `3sx` processes on MiSTer because the generated `launch-osd.sh` wrapper does not trap `TERM`/`HUP`/`INT` and does not forward termination to its child
  - fixing only the MiSTer launcher template in `tools/mister/package.sh` should keep the bounded smoke-run from poisoning later controls without touching gameplay, presenter math, simulation timing, or rendering behavior
- [x] Success metric and verification plan for the attempted loop:
  - redeployed package survives the required bounded `launch-osd.sh` smoke run with no lingering `3sx` process afterward
  - a native gameplay control taken after that bounded launch, without a manual `killall`, stays near the recovered baseline from `ralphpart2-loop1-native-after-killall`
  - verify with Docker build/package, MiSTer deploy, probe, bounded launch, remote `ps` check, native perf capture, and a fresh review pass before commit
- [x] Rejected before implementation this cycle:
  - do not retry any presenter micro-optimization until the launcher/process-leak issue is fixed; the stale orphaned `3sx` processes fully explain the apparent native regressions

## Loop 1 Outcome

- [x] Implementation summary:
  - updated the generated MiSTer `launch-osd.sh` template in `tools/mister/package.sh` to launch `3sx` as a child process, trap `INT`/`HUP`/`TERM`, and kill/wait the child before logging the exit code
  - kept the runtime/code change scoped to launcher shutdown behavior only; no presenter, simulation, or gameplay-path code was changed
- [x] Verification evidence:
  - initial failing control after the old bounded-launch sequence: `ralphpart2-loop1-native-control` = `125.04 / 2.71 / 114.52 ms`
  - clean-state confirmation after manual `killall 3sx`: `ralphpart2-loop1-native-after-killall` = `60.95 / 1.23 / 55.85 ms`
  - post-fix bounded launch left no lingering `3sx` process on MiSTer, and `logs/last-run.log` recorded `exit=143`
  - final native control after the required probe + bounded launch sequence, with no manual cleanup in between: `ralphpart2-loop1-final-native` = `60.74 / 1.26 / 55.51 ms`
- [x] Keep / rollback decision:
  - keep the launcher shutdown fix; it removes the bounded-launch orphan process leak that was poisoning later gameplay controls
  - checkpoint commit hash: `e9299324`
- [x] Review gate:
  - independent review flagged a possible console-restore gap when `launch-osd.sh` terminates the child with a signal
  - rejected that finding for this loop after validation: `/dev/tty2` reported the same `kbd_mode` state after a clean one-frame `--perf-capture` exit, so the console-state symptom was pre-existing and not caused by the launcher diff
  - no additional high-confidence in-scope findings were accepted
- [x] Next best candidate:
  - with the bounded-launch leak fixed and native controls stable again, return to presenter-focused research from a clean baseline
  - best next candidate remains narrower readback-versus-copy telemetry or another small presenter-path experiment backed by fresh stable native and square-pixels controls

## Sync Rules

- [x] Treat this file as the canonical active checklist for the `ralphpart2` automation series.
- [x] Keep completed loops summarized, with explicit keep/reject decisions and commit hashes.
- [x] If a cycle is research-only, record why no code change was justified and what the next candidate is.
