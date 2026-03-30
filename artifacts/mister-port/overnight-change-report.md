# Overnight Change Report: `f35bb07e..HEAD`

Date: 2026-03-05
Branch: `mister-offline-bringup`
Range analyzed: `f35bb07e..HEAD`

## Scope And Inputs

Primary scope (committed history):
- 30 commits from `b14c7d70` through `b89364a2`.

Secondary scope (local, uncommitted):
- Working tree modifications and untracked local artifacts at analysis time.

Inputs used:
- `git log`, `git diff`, per-commit patch review, and `git blame` on touched hot paths.
- Runtime/deploy/review artifacts under `artifacts/mister-port/logs/`.

## Aggregate Change Summary (Committed)

- Files changed: 9
- Net diff: +1019 / -151
- Main code concentration:
  - `src/port/sdl/fbdev_presenter.c` (+381 / -8)
  - `src/port/sdl/sdl_game_renderer.c` (+228 / -111)
  - `src/port/sdl/sdl_app.c` (+159 / -24)
  - `src/port/sdl/sdl_message_renderer.c` (+50 / -7)
- Supporting process/docs:
  - `artifacts/mister-port/living-findings.md`
  - `artifacts/mister-port/overnight-perf-prompt.txt`
  - `tools/mister/ralph-loop.sh`

Overall intent was consistent: reduce render/present CPU work on MiSTer SDL software + fbdev paths without touching gameplay/state logic.

## Commit-By-Commit Analysis (Full History)

### A) Automation/Process Baseline

1. `b14c7d70` `ralph loop stuff`
- Added loop prompt and automation runner:
  - `artifacts/mister-port/overnight-perf-prompt.txt`
  - `tools/mister/ralph-loop.sh`
- Rationale: establish unattended optimization/review/deploy loop with strict perf-only guardrails.
- Gameplay regression risk: none (tooling only).

### B) Early FBDev And Render Queue CPU Trims

2. `5e294962` `mister: reduce fbdev present path CPU overhead`
- Added scaling LUT cache and contiguous memcpy fast path in presenter:
  - `src/port/sdl/fbdev_presenter.c:19-22`, `:99-133`, `:462-485`
- Rationale: remove per-pixel divide cost and avoid repeated LUT recomputation.
- Gameplay regression reasoning: only post-render framebuffer copy path changed, not game simulation.
- Primary regression watchpoint: wrong scaling map if LUT invalidation misses (guarded by source size keys).

3. `e0777c13` `mister: trim render queue housekeeping overhead`
- Removed per-frame full-array zeroing and skipped no-op sorts:
  - `src/port/sdl/sdl_game_renderer.c:169-175`, `:330-341`
- Rationale: avoid unnecessary memory clears/sort overhead for bounded queues.
- Gameplay regression reasoning: draw traversal still bounded by `render_task_count`.
- Watchpoint: stale data use if future code ignores `render_task_count` bounds.

4. `8d64eb2c` `mister: reduce fbdev scaler work on repeated source rows`
- Reused prior destination row when nearest-neighbor maps repeated source rows:
  - `src/port/sdl/fbdev_presenter.c:466-484`, `:489-508`
- Rationale: common upscale ratios duplicate rows; memcpy previous row is cheaper.
- Gameplay regression reasoning: identical nearest-neighbor source indices.

5. `abf06e64` `mister: skip redundant palette apply on texture cache hits`
- Palette application moved into texture cache-miss path:
  - `src/port/sdl/sdl_game_renderer.c:549-552`
- Rationale: avoid repeated CPU palette application when SDL texture already cached.
- Gameplay regression reasoning: effective rendered texture remains same cache key (`texture_handle`,`palette_handle`).

6. `5dff2ba6` `mister: skip render qsort on ordered task queues`
- Introduced sortedness tracking (later evolved in subsequent commits).
- Current resulting logic appears in:
  - `src/port/sdl/sdl_game_renderer.c:151-163`, `:330-340`
- Rationale: avoid full `qsort` when queue already comparator-ordered.
- Watchpoint: ordering/tie semantics correctness for equal Z (addressed by later commits).

7. `967fb100` `mister: avoid render-task copy in draw hot path`
- Converted draw path to write directly into `render_tasks[render_task_count]`:
  - `src/port/sdl/sdl_game_renderer.c:568-575`, `:618-648`
- Rationale: remove stack task + memcpy per draw.
- Gameplay regression reasoning: task content still populated before `push_render_task` increments count.

8. `d886f462` `mister: reduce quad color/vertex setup overhead`
- Added `rgba8_to_float` lookup and streamlined color conversion:
  - `src/port/sdl/sdl_game_renderer.c:46`, `:245-258`, `:295-299`
- Rationale: reduce repeated byte->float division cost.
- Gameplay regression reasoning: pure arithmetic substitution (same mapping 0..255 -> 0..1).

9. `a0126ec2` `mister: streamline SDL quad setup in draw hot path`
- Refactored quad setup into `begin_quad_task` + `draw_sprite_rect`:
  - `src/port/sdl/sdl_game_renderer.c:568-616`, `:650-674`
- Rationale: remove transient structs and repeated coordinate reshaping.
- Gameplay regression reasoning: preserved sprite corner and UV mapping paths.

10. `94777c51` `mister: avoid full qsort for equal-z render runs`
- Added equal-Z run reversal and inversion flags:
  - `src/port/sdl/sdl_game_renderer.c:42-44`, `:194-215`, `:330-340`
- Rationale: avoid `qsort` when only tie-order correction is needed.
- Gameplay regression reasoning: tie rule remains aligned to `compare_render_tasks` index ordering.
- Watchpoint: float equality (`==`) on Z grouping.

### C) Present-Path Housekeeping And Message Composition

11. `40daf63f` `mister: trim native present housekeeping overhead`
- Introduced native output rect caching and gated FPS bookkeeping to debug builds:
  - `src/port/sdl/sdl_app.c:46-50`, `:568-591`
- Rationale: avoid per-frame output-size/rect recompute in stable geometry.
- Gameplay regression reasoning: affects presentation geometry calculations only.

12. `ef76b8be` `mister: remove redundant begin-frame window clear`
- Removed unconditional window clear in `SDLApp_BeginFrame`:
  - `src/port/sdl/sdl_app.c:437-440`
- Rationale: clear already handled in end-frame composition when needed.
- Watchpoint: stale backbuffer in branches that skip end-frame clear (addressed by conditional clear logic later).

13. `bc120b21` `mister: skip empty message-layer compositing`
- Added subtitle content tracking and conditional message compositing:
  - `include/port/sdl/sdl_message_renderer.h:11`
  - `src/port/sdl/sdl_message_renderer.c:56-58`, `:153`
  - `src/port/sdl/sdl_app.c:636-689`
- Rationale: skip blending empty message canvas.
- Gameplay regression reasoning: only bypasses compositing when `has_content=false`.

14. `53cb572a` `mister: cache repeated quad color conversion`
- Added single-pixel cache for repeated `read_rgba32_fcolor` inputs:
  - `src/port/sdl/sdl_game_renderer.c:47-49`, `:245-258`
- Rationale: exploit repeated vertex color values in hot path.

15. `849ec8c0` `mister: optimize near-sorted render-task ordering`
- Added inversion count heuristic + insertion sort for near-sorted queues:
  - `src/port/sdl/sdl_game_renderer.c:44`, `:50-51`, `:217-232`, `:331-337`
- Rationale: `insertion_sort` cheaper than `qsort` for few inversions/small queues.

16. `2f253ba8` `mister: defer message-canvas clears on empty frames`
- Deferred clear until first subtitle draw after content frame:
  - `src/port/sdl/sdl_message_renderer.c:20`, `:46-54`, `:146-150`
- Rationale: remove per-frame target clear when overlays absent.
- Watchpoint: stale subtitle canvas if `has_content` bookkeeping breaks.

17. `71f91b1c` `mister: avoid redundant subtitle target binds`
- Initially avoided bind via `SDL_GetRenderTarget` check; later superseded by cached bind flag.
- Current equivalent behavior:
  - `src/port/sdl/sdl_message_renderer.c:18`, `:142-144`

18. `3edc3480` `mister: cache subtitle texture modulation state`
- Cached color/alpha modulation state:
  - `src/port/sdl/sdl_message_renderer.c:13-17`, `:130-140`
- Rationale: skip redundant `SDL_SetTextureColorMod/AlphaMod`.

19. `49c3ea3f` `mister: cache subtitle target binds with invalidation`
- Added explicit invalidation API and bind-cache discipline:
  - `include/port/sdl/sdl_message_renderer.h:12`
  - `src/port/sdl/sdl_message_renderer.c:60-62`, `:142-144`
  - `src/port/sdl/sdl_game_renderer.c:324-327`
- Rationale: avoid repeated target queries/binds while preserving correctness across renderer transitions.

20. `9489800e` `mister: cache current texture binding in game renderer`
- Added fast return for identical texture/palette binding and invalidation on destroy paths:
  - `src/port/sdl/sdl_game_renderer.c:33-35`, `:431-437`, `:506-512`, `:539-541`, `:564-565`
- Rationale: eliminate redundant texture rebinding work in draw hot path.

21. `e503a4f7` `mister: skip redundant present clears when no bars`
- Added bar-detection state and conditional clears:
  - `src/port/sdl/sdl_app.c:48-50`, `:509-512`, `:581-590`, `:655-660`, `:679-683`
- Rationale: avoid full-target clear when no letterbox bars are visible.
- Gameplay regression reasoning: only black-bar fill behavior optimized.

### D) FBDev Direct-Copy And Rect-Readback Evolution

22. `677a6bd6` `mister: avoid fbdev temp conversion surface on direct copies`
- Introduced direct full-surface conversion/copy branches and streamlined same-size path:
  - `src/port/sdl/fbdev_presenter.c:397-448`
- Rationale: avoid unnecessary temporary conversion surfaces for compatible cases.

23. `b511ddef` `mister: bypass SDL output-size query in fbdev mode`
- Used fbdev dimensions directly in fbdev-only mode:
  - `src/port/sdl/sdl_app.c:549-566`
- Rationale: remove expensive/pointless SDL output-size query in fixed fbdev presentation mode.

24. `88396e7a` `mister: read back native content rect for fbdev`
- Added `content_rect`-aware `FBDevPresenter_Present` API and rect readback path:
  - `src/port/sdl/fbdev_presenter.h:24`
  - `src/port/sdl/fbdev_presenter.c:278-302`, `:373-380`
  - `src/port/sdl/sdl_app.c:637-652`, `:716`
- Rationale: reduce readback/copy cost by limiting to active content rectangle.
- Watchpoint: bar clearing correctness for changing rect boundaries.

25. `bf804cfc` `mister: skip redundant fbdev bar clears`
- Cached cleared bar-rect bounds and skipped repeat bar clears when unchanged:
  - `src/port/sdl/fbdev_presenter.c:23-27`, `:293-297`
- Rationale: reduce repeated memset bandwidth on stable geometry.

26. `9c40f906` `mister: skip redundant native bar clears`
- Removed redundant SDL-side bar clear in fbdev-native-rect mode; strengthened fallback bar clear safety:
  - `src/port/sdl/sdl_app.c:655-660`
  - `src/port/sdl/fbdev_presenter.c:382-387`, `:413-416`, `:512-515`
- Rationale: bars need not be cleared on SDL target when fbdev reads only content rect.

27. `98f0b63f` `mister: trim redundant frame-loop target work`
- Added `ENABLE_NETPLAY` gating for overlay calls and game-target bind cache:
  - `src/port/sdl/sdl_app.c:626-629`
  - `src/port/sdl/sdl_game_renderer.c:36`, `:324-328`, `:371`
- Rationale: avoid unnecessary target churn and unnecessary overlay work when netplay disabled.

28. `0f995821` `mister: trim fbdev letterbox readback bandwidth`
- Extended rect-readback selection to letterboxed non-native path; added debug full-readback override:
  - `src/port/sdl/sdl_app.c:638-643`, `:672-677`, `:679-683`
- Rationale: same optimization principle as native rect path, generalized to letterbox case.
- Gameplay regression reasoning: debug builds force full readback to preserve deterministic inspection behavior.

29. `d3cd63ee` `mister: trim non-native present housekeeping`
- Added non-native output rect cache and removed redundant target-cache invalidations:
  - `src/port/sdl/sdl_app.c:51-55`, `:593-609`, `:668-671`, `:686-689`
  - `src/port/sdl/sdl_game_renderer.c:319` (removed pre-bind invalidation in begin frame)
- Also introduced living-memory process doc:
  - `artifacts/mister-port/living-findings.md:1-74`
- Rationale: reduce repeated letterbox math and superfluous invalidation calls.

30. `b89364a2` `mister: trim fbdev fallback copy bandwidth`
- Added same-size fallback rect-copy helper before full-copy fallbacks:
  - `src/port/sdl/fbdev_presenter.c:135-161`, `:225-276`, `:398-407`
- Rationale: even fallback path should avoid copying pixels outside active content.
- Gameplay regression reasoning: full-copy behavior retained when rect copy cannot be applied.

## Gameplay Regression Reasoning (Cross-Cut)

What stayed untouched:
- No edits in gameplay/simulation/content domains (`sf33rd/...` gameplay logic, move data, collision, rules, RNG, timing model).
- No input mapping semantic changes.

What changed:
- SDL renderer task setup/order path (`sdl_game_renderer.c`).
- Subtitle/message compositing path (`sdl_message_renderer.c`).
- SDL present-frame orchestration (`sdl_app.c`).
- fbdev copy/scale/readback behavior (`fbdev_presenter.c`).

Why gameplay-impact risk is low but non-zero:
- All changes are render/present-side and preserve existing comparison/order logic intent.
- Remaining non-zero risk is visual compositing/order correctness under edge cases (equal-Z ordering, target-cache invalidation discipline, subpixel content-rect boundaries), not simulation behavior.

## Verification Evidence

Positive evidence observed:
- Device probe confirms expected MiSTer backend mode (dummy/software + fbdev active + native path):
  - `artifacts/mister-port/logs/device-20260305-101932/probe.out:7-26`
- Runtime sanity check completed with expected bounded-timeout success:
  - `artifacts/mister-port/logs/device-20260305-101932/runtime.out:4-5`
- Last-run log confirms runtime environment and fbdev activation:
  - `artifacts/mister-port/logs/device-20260305-101932/last-run.log:30-35`
- Structured pass example with explicit RCs all zero:
  - `artifacts/mister-port/logs/device-20260305-083834/results.txt:2-14`
- Living findings document records successful build/deploy/probe/runtime cycles and keep-decisions:
  - `artifacts/mister-port/living-findings.md:37-74`

Review gate evidence:
- Independent review output includes two non-blocking findings (coverage + conversion-path consistency), no high-confidence correctness defect requiring immediate rollback:
  - `artifacts/mister-port/logs/review-20260305-101224/findings.json:1`

Negative/noisy evidence to account for:
- Some earlier device result bundles show non-zero RCs due harness/deploy/transient issues (not clearly code-correctness failures):
  - `artifacts/mister-port/logs/device-20260305-094541/results.txt:2,4,7`
  - `artifacts/mister-port/logs/device-20260305-094639/results.txt:5,10-12`
  - `artifacts/mister-port/logs/device-20260305-095136/results.txt:5`
- Latest two result bundles (`101115`, `101932`) only include `LOG_DIR` and omit explicit RC key/value checks, reducing machine-verifiable certainty:
  - `artifacts/mister-port/logs/device-20260305-101115/results.txt:1`
  - `artifacts/mister-port/logs/device-20260305-101932/results.txt:1`

## Unresolved Risks

1. fbdev presenter rect-copy path lacks dedicated automated tests.
- Evidence: review finding F001.
- Risk: regressions in copied bounds, bar-clear interactions, or conversion fallback may only appear on-device.

2. Conversion behavior differs between full-surface and rect-surface helpers.
- Evidence: review finding F002 (`FOURCC` gate and differing conversion routes).
- Risk: path-dependent behavior and maintenance drift.

3. Target-bind cache correctness depends on strict invalidation discipline.
- Relevant code:
  - `src/port/sdl/sdl_message_renderer.c:60-62`, `:142-144`
  - `src/port/sdl/sdl_game_renderer.c:324-327`
- Risk: future render target changes outside existing invalidation points could cause stale target assumptions.

4. Rect-boundary rounding behavior may be sensitive to fractional coordinates.
- Relevant code:
  - `src/port/sdl/fbdev_presenter.c:67-70`
- Risk: rare 1px edge artifacts at changing letterbox/content boundaries.

5. Quantitative perf evidence is mostly qualitative/process-based.
- There are successful sanity runs, but no committed before/after frame-time histogram in this range.

## Recommended Follow-Up Checks

1. Add a focused fbdev presenter regression harness.
- Cover:
  - Rect-readback success path.
  - Rect-readback failure fallback.
  - Same-size fallback rect-copy path (`b89364a2`) with bar-clear assertions.

2. Unify conversion logic between `copy_surface_to_fb_offset` and `copy_surface_rect_to_fb_offset`.
- Reduce divergence and make format-handling behavior explicit/documented.

3. Add pixel-diff snapshot checks for representative scale modes.
- Native + letterboxed non-native + fbdev-only mode.
- Compare bars/content bounds and subtitle compositing.

4. Add instrumentation counters for chosen fast paths.
- Example counters:
  - `present_readback_rect` hit rate.
  - fallback rect-copy hit rate.
  - number of `qsort` vs insertion-sort vs equal-run reverse events.

5. Standardize device `results.txt` schema.
- Always emit explicit RC keys to keep verification machine-checkable across script variants.

## Uncommitted Local Context

Working tree status at analysis time:
- Modified tracked files:
  - `artifacts/mister-port/overnight-perf-prompt.txt`
  - `tools/mister/ralph-loop.sh`
- Untracked directory:
  - `artifacts/mister-port/logs/`

Details:

1. `artifacts/mister-port/overnight-perf-prompt.txt` (uncommitted edits)
- Added explicit instruction to read `living-findings.md` at cycle start:
  - `artifacts/mister-port/overnight-perf-prompt.txt:7`
- Added required "Living memory update" section and structure:
  - `artifacts/mister-port/overnight-perf-prompt.txt:30-41`
- Impact: process-only; no runtime/gameplay code impact.

2. `tools/mister/ralph-loop.sh` (uncommitted edits)
- Added `LIVING_DOC` variable and auto-bootstrap for missing living findings file:
  - `tools/mister/ralph-loop.sh:6`
  - `tools/mister/ralph-loop.sh:46-56`
- Impact: automation/process-only; no runtime/gameplay code impact.

3. Untracked log corpus under `artifacts/mister-port/logs/`
- Approx size: `11M` total, with `ralph-loop.log` ~`8.1M`.
- Approx structure snapshot:
  - ~80 `device-*` directories
  - ~23 `review-*` directories
  - ~653 files total
- Contains review/debate artifacts (e.g., `findings.json`, `debate-round-*.json`, `steelman.md`) plus deploy/probe/runtime logs.
- Risk/operational note: these artifacts are useful for auditability but high-churn; treat as local run artifacts unless explicitly requested for commit.

## Bottom Line

The committed range is a coherent series of render/present-path optimizations with strong evidence of repeated MiSTer sanity verification and no direct gameplay logic edits. The main remaining concerns are correctness confidence in fbdev rect/fallback edge paths and testability/consistency of conversion and verification harness behavior, not intentional gameplay behavior changes.
