# Implementation Todo

## Todo Metadata

- [x] Canonical todo path: `artifacts/mister-port/gameplay-loop-series/todo.md`
- [x] Active feature/phase: MiSTer gameplay-focused render/presenter optimization loop series
- [x] Historical checklists:
  - `artifacts/mister-port/render-loop-series/todo.md` is the completed earlier render-loop series
  - `artifacts/mister-port/telemetry-loop/todo.md` is the completed telemetry loop

## Goal and Success Criteria

- [x] Goal: complete the remaining gameplay-performance optimizations as isolated MiSTer IVRFC loops without gameplay regressions
- [x] Success criteria:
  - every optimization item runs as its own fresh-agent loop
  - every loop begins with a scoped plan
  - every loop ends in a verified commit
  - every completed loop updates `artifacts/mister-port/living-findings.md`
  - verification emphasizes representative gameplay behavior rather than only startup/intro behavior

## Scope and Constraints

- [x] In scope:
  - gameplay-representative telemetry improvements if needed
  - broad layer-pass research and implementation
  - direct-native presenter dirty-diff improvements
  - clipped-native presenter work for low-resolution and square-pixels outputs
  - broader presenter/readback architecture changes if still justified after earlier loops
- [x] Out of scope: gameplay logic, timing semantics, input handling, rules, collision, RNG, content/resources, unrelated cleanup, committing perf/log artifacts
- [x] Constraints:
  - use a fresh agent for each optimization item
  - keep one optimization objective per verified loop
  - use the verified Docker path in `docs/mister-runbook.md`
  - verify on MiSTer before committing
  - update the living findings doc and this checklist after each successful loop
- [x] Measurement caveat:
  - current `tools/mister/perf-sampler.sh --scene training` captures start immediately from normal boot and `--scene` is metadata only
  - do not use the current startup-window captures alone to reject gameplay-focused optimization ideas

## Execution Order

### Phase 0: Measurement Fixup

- [x] Loop 0 goal: make gameplay-representative verification possible
- [x] Success gate:
  - there is a documented capture path that reaches a representative gameplay state, or there is explicit evidence that a safer existing harness can be used for that purpose
  - resulting telemetry is sufficient to compare gameplay-heavy scenes against startup/intro captures
- [x] Implementation summary:
  - exposed the scripted test runner flags in release builds
  - made the test runner work without external state dumps by defaulting to a deterministic idle-versus match
  - added `--perf-wait-in-game` and `--perf-warmup` so perf capture can start after gameplay is live instead of at boot
  - added `tools/mister/perf-sampler.sh --gameplay-idle` as the standard representative gameplay gate
- [x] Evidence:
  - startup control `loop0-startup-control`: `render_task_count.mean=1.81`, `sort_strategy.none=100%`, `fps.mean=198.90`
  - gameplay gate `loop0-gameplay-idle-pre`: `render_task_count.mean=291.47`, `sort_strategy.qsort=100%`, `fps.mean=16.50`
  - gameplay gate `loop0-gameplay-idle-post`: `render_task_count.mean=291.47`, `sort_strategy.qsort=100%`, `fps.mean=16.03`
  - backend log confirmed `PERF capture start: in_game=1 warmup_frames=120`
- [x] Review note:
  - separate `codex review --uncommitted` was started for the diff
  - no actionable finding was returned before the review process stalled, so no review-driven code change was applied

### Phase 1: Broad Layer-Pass Work

- [x] Loop 1 goal: research the broad layer-pass idea against gameplay-relevant scenes before implementation
- [x] Research implementation summary:
  - added per-task source tagging/counters to the SDL renderer telemetry path
  - tagged every in-tree `SDLGameRenderer_Draw*` entry point (`PPG`, `mtrans`, direct UI, solid)
  - extended perf JSON/backend logs with source counters so gameplay captures can prove queue composition instead of inferring it from z values
- [x] Research requirements:
  - identify where task layering/order semantics actually come from in the current renderer
  - determine whether gameplay-heavy scenes produce enough task volume or sort pressure to justify broader pass partitioning
  - define the safest implementation shape before planning
- [x] Evidence:
  - fresh-agent review found no correctness issues in the source-tagging diff
  - MiSTer gate `loop1-source-telemetry-target-fixed`: `render_task_count.mean=291.47`, `sort_strategy.qsort=100%`
  - MiSTer gate `loop1-source-telemetry-target-fixed`: `ppg_tasks.mean=197.00`, `mtrans_tasks.mean=86.47`, `solid_tasks.mean=8.00`, `ui_direct_tasks.mean=0.00`, `unknown_tasks.mean=0.00`
  - source-bucket invariant check passed on all 300 target frames:
    `ppg_tasks + mtrans_tasks + ui_direct_tasks + solid_tasks + unknown_tasks == render_task_count`
- [x] Research conclusion:
  - broad layer-pass work remains justified under gameplay load
  - the safe implementation should follow the verified source split (`PPG`/`mtrans`/solid) instead of relying on z-only partitioning
  - direct UI work is not present in the current gameplay-idle gate, so it should be preserved as a compatibility edge case rather than treated as a primary batch target
- [x] Implementation gate:
  - research captured in `living-findings.md`
  - next loop under this phase should implement the first source-aware pass split and verify that draw order and gameplay output stay unchanged
- [x] Loop 4 implementation result:
  - tested a conservative source-pass sort fast path that only would have activated when gameplay frames were already grouped as `PPG -> mtrans -> solid` with strictly non-overlapping z ranges between passes
  - representative gameplay capture `loop4-layer-passes-post` stayed `qsort=100%` across all `300` sampled frames and landed at `60.26 / 1.27 / 54.99 ms` frame/render/present mean vs. the prior native baseline `59.91 / 1.24 / 54.78 ms`
  - because the proof never activated on the gameplay gate, the runtime change was reverted and not kept
- [x] Durable conclusion:
  - the reopened layer-pass idea was worth checking, but the first safe source-pass formulation does not trigger on the current gameplay-idle workload
  - do not keep dormant source-pass code in the runtime path; any future revisit needs richer pass semantics or explicit telemetry about why the gameplay queue fails the proof

### Phase 2: Direct Native Presenter Dirty Diff

- [x] Loop 2 goal: evaluate source-sized dirty diffing on the exact-fit native present path and keep it only if it improves the gameplay gate
- [x] Gate result:
  - output semantics stayed correct during the temporary implementation
  - copy volume improved sharply, but representative gameplay present cost regressed, so the runtime change was rejected and reverted
- [x] Implementation summary:
  - temporarily extended `FBDevPresenter_PresentCurrentTarget()` to route the exact-fit `384x224` readback through the existing staging-buffer dirty-diff machinery
  - verified offset handling and previous-buffer invalidation on the current-target path
  - reverted the runtime change after MiSTer gameplay captures showed that lower copy volume did not translate into lower frame or present time
- [x] Evidence:
  - baseline gameplay gate `loop1-source-telemetry-target-fixed`: `60.22 / 1.24 / 55.13 ms` frame/render/present mean, `copy_bytes.mean=344064`
  - temporary path `loop2-native-dirty-diff-post`: `61.73 / 1.25 / 56.69 ms` frame/render/present mean, `copy_bytes.mean=30095.36`, `dirty_hit_rate.mean=0.9125`
  - late-window check still regressed: after frame 120, baseline `58.31 / 53.43 ms` frame/present mean vs. dirty-diff `60.17 / 55.33 ms`
- [x] Durable conclusion:
  - on the existing exact-fit path, `SDL_RenderReadPixels()` remains the dominant cost and the extra staging compare/copy work makes gameplay slower even when framebuffer writes collapse
  - do not pursue this exact optimization again unless the readback cost itself is reduced or removed first

### Phase 3: Clipped-Native Presenter Path

- [x] Loop 3 goal: add a dedicated low-resolution/square-pixels presenter path that avoids unnecessary composite/readback work while staying safe on clipped outputs
- [x] Gate result:
  - high-resolution native behavior remained stable on the representative gameplay gate
  - square-pixels gameplay improved materially by bypassing the dummy-target composite step
  - live low-resolution clipped-output verification is still pending because the current MiSTer framebuffer is `1280x720`, but the clipped-native crop path is now implemented with the same mapped current-target presenter
- [x] Implementation summary:
  - broadened `FBDevPresenter_PresentCurrentTarget()` so it can map the current `cps3_canvas` target directly into any native-output rect, using crop-only copies for clipped native and nearest-neighbor scaling for square-pixels
  - moved native render-path presentation to prefer the current-target fbdev path whenever `use_fbdev_only_present` is active and no message overlay is present
  - added a logged fallback back to the old composited-readback path if the current-target presenter ever fails
- [x] Evidence:
  - native gameplay gate `loop3-native-direct-post`: `59.91 / 1.24 / 54.78 ms` frame/render/present mean vs. prior native baseline `60.22 / 1.24 / 55.13 ms`
  - square-pixels baseline `loop3-square-pixels-pre`: `140.17 / 55.05 / 81.29 ms` frame/render/present mean, `fps.mean=7.13`
  - square-pixels direct path `loop3-square-pixels-post`: `98.25 / 1.38 / 93.02 ms` frame/render/present mean, `fps.mean=10.18`
  - square-pixels delta: `-41.93 ms/frame`, with render/composite work collapsing by `-53.66 ms` while present rises by `+11.73 ms`
- [x] Durable conclusion:
  - for native render modes, the biggest waste was not framebuffer copy volume but the extra SDL composite pass before readback
  - the broadened current-target presenter is worth keeping for square-pixels and remains safe on the current high-resolution native gate
  - keep an explicit follow-up to live-check the clipped-native branch on a real low-resolution framebuffer before treating that sub-case as fully closed

### Phase 4: Broader Presenter Architecture

- [x] Loop 5 goal: evaluate and, if justified, implement a broader fbdev-only presenter path that reduces or removes per-frame SDL readback cost
- [x] Gate result:
  - a research-backed prototype was built, deployed, and verified on MiSTer
  - it was independently revertable, but representative gameplay got materially slower, so the runtime change was rejected and reverted
- [x] Implementation summary:
  - temporarily wrapped the mapped framebuffer in an SDL surface and created a software renderer on that surface for fbdev-only mode
  - temporarily switched the MiSTer path to render directly to that surface-backed renderer instead of using the normal SDL target plus fbdev readback presenter split
  - reverted the runtime change after gameplay verification showed that removing readback moved far more work into `render_ms` than it saved in `present_ms`
- [x] Evidence:
  - baseline gameplay gate `loop3-native-direct-post`: `59.91 / 1.24 / 54.78 ms` frame/render/present mean
  - prototype probe logged `FBDEV surface renderer: enabled (software)`
  - prototype gameplay gate `loop5-surface-renderer-post`: `81.86 / 54.87 / 23.20 ms` frame/render/present mean, `copy_bytes.mean=0`
- [x] Durable conclusion:
  - the broadest "remove SDL readback entirely" architecture was worth testing, but the full surface-backed software-renderer swap is not a gameplay win on MiSTer
  - this closes the broader presenter architecture item for the current loop series; any future revisit should be a narrower hybrid path instead of replacing the renderer/presenter split wholesale

## Checklist Sync Rules

- [x] Keep this file as the canonical active checklist for the new gameplay-focused loop series.
- [x] Mark a loop complete only after plan, implementation, MiSTer verification, doc updates, and commit all succeed.
- [x] Record commit hashes and representative perf evidence under the relevant phase after each successful loop.
- [x] If a loop changes the measurement strategy, update the living findings doc before the next loop starts.

## Open Questions

- [x] What is the lowest-risk path to capture representative gameplay on MiSTer for perf work?
  - answer: use `tools/mister/perf-sampler.sh --gameplay-idle --gameplay-warmup 120`, which now drives the built-in idle-versus harness to a real in-game state before capture starts
- [x] Does broad layer-pass splitting need new task metadata, or can it be driven from existing renderer fields?
  - answer: use explicit task-source metadata; startup-window/z-only evidence was not strong enough for a safe gameplay pass split
- [x] For low-resolution outputs, is a direct cropped copy path enough, or is a different render-target architecture needed?
  - answer: for native render modes, a direct mapped current-target path is enough to justify the optimization; the remaining follow-up is live low-resolution verification, not a new architecture for this phase
