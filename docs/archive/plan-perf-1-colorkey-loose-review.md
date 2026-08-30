# Review — plan-perf-1-colorkey-loose.md

> **ARCHIVED 2026-08-30** — true at 3f020a54, not maintained.
> Read for rationale and for what was tried and failed. Do not read for current facts; read the code.


Reviewer pass. All line-number citations below are against the worktree `/Users/sb/Developer/3sx-mister-perf/` at the current branch HEAD (`7ed2c2d3`). Files were opened directly; no file:line claim is forwarded without verification.

The plan is overall well-structured and most factual claims hold up. The findings below are the ones that, if not addressed, would either make a step fail to compile, fail to produce a valid binary, or produce a measurement with no usable A/B comparison.

---

## P-1 — must fix

### P-1.1 — Step 1's grep recipe will miss every RenderTask carry-through site

Plan §Step 1 says (lines 277–279):

> At every call site that already sets `task->software_palette_lut`, also set `task->software_palette_is_binary_alpha = …`. Find these sites by grepping `software_palette_lut\[` in the file (expect ≤4 sites; the main one is in the SetTexture/SetPalette flow around `sdl_game_renderer.c:11676-11695`).

Two distinct problems here:

1. **Wrong grep pattern.** `software_palette_lut\[` matches *array indexing* of the static `software_palette_lut[FL_PALETTE_MAX][256]` (declaration at line 176, indexing at lines 9182, 11686, etc.), not the **`task->software_palette_lut` writes** that need the new bool paired with them. The actual RenderTask-write pattern is `task->software_palette_lut =` and that grep returns three writes plus one NULL-out:
   - `sdl_game_renderer.c:11506` (destroy path: `render_tasks[i].software_palette_lut = NULL;`)
   - `sdl_game_renderer.c:11725` (`begin_quad_task`)
   - `sdl_game_renderer.c:12035` (inline mtrans path)
   - `sdl_game_renderer.c:12196` (input-history glyph path; sets to NULL)

   Plus the **task-array zero-init** loop. This is not a hypothetical — line 11506 sets `software_palette_lut = NULL` and `software_source_is_index8 = false`; the new bool also needs to be zeroed there to avoid carrying a stale `true` from the previous frame's task-slot reuse (`render_tasks` is an array of size `RENDER_TASK_MAX = 1024`, reused; line 40).

2. **The site at `11676-11695` is the wrong site.** That block sets the *globals* `current_software_palette_lut` and `current_software_source_is_index8` (line 11686-11687); it does **not** write to a `RenderTask`. The plan calls it "the main one … in the SetTexture/SetPalette flow," which is true for the global-state binding step, but plumbing the new field requires writing to the *RenderTask* struct in `begin_quad_task`/inline-mtrans/input-glyph paths.

**Fix:** explicitly enumerate the 3+1 RenderTask write sites (11506, 11725, 12035, 12196) and have step 1 add `task->software_palette_is_binary_alpha = …` at each. The cleanest pattern is a single new global `current_software_palette_is_binary_alpha` set adjacent to `current_software_palette_lut` at lines 11686-11695, then mirrored into every `task->software_palette_lut = …` site. This is the same pattern as `current_software_source_is_index8` already follows.

---

### P-1.2 — Step 5's "single ineligible/skipped counter at top of INDEX8 fast-path entry block" doesn't compose with both exact-copy and scaled branches

Plan §Step 5 (lines 547–558) places the `else if (!task->software_palette_is_binary_alpha) ... ineligible++;` increment "**once**, at the top of the INDEX8 fast-path entry block at line ~6116 (just after the `palette_lut` / `i8_surface` / `dst_pixels` locals are bound, before the exact-copy/scaled split)."

Problem: at line 6116 we have not yet entered either the exact-copy or scaled branch, and we have not yet checked `plan->color_mod`. The plan also says (§ Design 2) the loose-form fast path is **only** in the *non-color-mod* branch. So a counter at line 6116 will conflate three cases the plan otherwise carefully separates:
- Color-mod tasks (loose-form intentionally not used) — these should not increment `ineligible` because the kernel was never going to fire anyway.
- Non-binary-α palette in non-color-mod (true ineligible).
- Binary-α palette in non-color-mod with kernel disabled (skipped).

If the increments fire at 6116 unconditionally on `!is_binary_alpha`, color-mod tasks will be double-counted as "ineligible" and the on-device check `ineligible == 0` will trip even though every palette is binary-α (matching research). Conversely, if the increment is left at 6116 only when `is_binary_alpha == false`, color-mod tasks with a non-binary-α palette inflate the same counter for a path the optimization doesn't cover.

**Fix:** move the increments into the same `if`-ladder that picks the kernel — increment `_hits` at the top of each new fast-path branch (steps 2/3/4, exactly as the plan already says at lines 545-546), and increment `_skipped` / `_ineligible` only inside the existing **non-color-mod** branches (line 6224 for exact-copy; line 6296 for scaled), gated by the same `colorkey_loose_kernel_enabled` and `task->software_palette_is_binary_alpha` checks. Don't increment in the color-mod branch.

---

### P-1.3 — Step 3 — NEON `vbslq_u32` operand order is correct in the plan, BUT the plan doesn't note the file already has working `vbslq_u8` precedents to mirror

Plan §Step 3 sketch (line 398): `vst1q_u32(dst, vbslq_u32(keep_dst_mask, dst_v, src_v));` with mask = `vceqq_u32(alpha_v, vdupq_n_u32(0))`.

ARM ACLE: `vbslq_u32(mask, a, b)` returns lane-wise `(mask & a) | (~mask & b)` — i.e. lanes where mask bits are set take from `a`. The plan's order is correct: `keep_dst_mask` is set in α==0 lanes, so those lanes take `dst_v`, others take `src_v`.

This is **already independently verified** by the existing usage in this same file at lines 6004 and 6072:

```
const uint8x16_t mask_zero = vceqq_u8(alpha_full, zero_vec);   // line 5999
result = vbslq_u8(mask_zero, dst_bytes, result);                // line 6004 — "replace with dst where alpha==0"
```

The plan's open question 3 asks the reviewer to verify operand order. **Verified — order is correct, and mirrors the existing `vbslq_u8` usage.** This is a confirm, not a P-1.

However: the §"What to do if it fails" item at line 462 says

> If `vbslq_u32` argument order looks wrong: the ARM convention is `vbslq_u32(mask, a, b)` → for each bit `mask?a:b`. Test with a 4-pixel toy: `src=[0xFFAA0000, 0x00000000, 0xFFBB0000, 0x00CC0000]` should yield `[src[0], dst[1], src[2], dst[3]]` (assuming index 1 has α=0 and index 3 has α=0).

The toy example is internally inconsistent: `0x00CC0000` has α=0, so lane 3 should yield `dst[3]` (correct), but `0xFFBB0000` has α=0xFF → keep `src[2]` (correct). However the example confusingly also says `0xFFAA0000` for lane 0 (α=0xFF, keep src) and `0x00000000` for lane 1 (α=0, keep dst). Reads correct on close inspection but the tutorial framing makes it look like a sketch that hasn't been re-checked. **Recommend** the plan's "what to do if it fails" item link directly to the existing `vbslq_u8` site at line 5999-6006 instead — that's the unambiguous, in-tree precedent.

This one is borderline P-1 / P-2. Calling it P-1 because if a fix-agent reads the (slightly off-feeling) toy example and second-guesses operand order, they could swap the args and ship a binary that paints transparent pixels into dst.

---

### P-1.4 — Step 7's "edit default off and rebuild" baseline strategy is needlessly destructive AND the recommended baseline binary is not pristine

Plan §Step 7 procedure item 1 (lines 670-674):

> Baseline capture (kernel disabled): apply a one-line local edit setting `colorkey_loose_kernel_enabled = false` (default on, but baseline records OFF). Build, deploy, capture 60 s of three scenes…

This is the right idea but bumps into two existing items:

1. The reviewer was asked to consider `~/Developer/3sx-mister-baselines/2026-04-25-telemetry-instrumented/3s-arm`. **Verified:** that path exists, with `source-commit.txt` = `7ed2c2d3743d73f97b6f1efeef360763ebdf40c2`, which **matches** the current branch HEAD (`git log -1` on the perf worktree returns the same hash). Provenance file says it has additional **uncommitted instrumentation patches** (palette alpha histogram + format census) at lines ~9195–9228 and ~11676–11694.
2. The instrumentation patches are explicitly noted as "one-shot loggers, not in the hot path. Should not measurably affect steady-state frame time." Accepting that claim, the saved binary IS suitable as a baseline — and it is the *exact* commit, on the *exact* flavor (telemetry), with no kernel changes. That is a strictly better baseline than "edit-default-off-and-rebuild" because:
   - The runtime toggle (per the plan's own §Design 7) means once the new code is shipped, you can A/B in the *same* binary by toggling at runtime — no rebuild needed.
   - The saved binary captures the state *before* the perf-1 changes started landing, which is the more interesting reference point.

**Fix:** rewrite Step 7 procedure item 1 to deploy the saved baseline binary (`~/Developer/3sx-mister-baselines/2026-04-25-telemetry-instrumented/3s-arm`) for the "before" capture, and use the new build (with kernel default ON) for the "after" capture. Do **not** include the "one-line local edit" approach unless the runtime toggle ends up not landing for some reason. Note that this means the runtime toggle from §Design 7 must be present (which the plan already commits to).

(See also P-1.5 below — adding a runtime config key compounds with this.)

---

### P-1.5 — Default-ON with no config key forecloses A/B comparison without a rebuild, contradicting `feedback-debug-build-for-live-tests.md`

Plan §Design 7 (lines 145-160) and Step 7 (lines 670-680):

> Default ON so perf testing in normal release/telemetry builds just runs the new kernel; flipping it OFF requires a developer call (initially via a small debug hook from `sdl_app.c` if needed, but the plan does not require a config-key plumb-through in this iteration).

This contradicts the explicit user feedback memo `feedback-debug-build-for-live-tests.md` (cited in the plan itself at line 162): "ship a DEBUG-flavor binary with diagnostics on" — which translates to "make A/B reachable without a rebuild." A "small debug hook" from `sdl_app.c` is not user-reachable; without a config key, every A/B requires a rebuild + redeploy.

**Verified:** the existing config plumbing (`src/port/config/config.h:25-100`) makes a runtime toggle trivially cheap. Pattern is:
1. Add `#define CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED "colorkey-loose-kernel-enabled"` next to existing keys.
2. Call `SDLGameRenderer_SetColorkeyLooseKernelEnabled(Config_GetBool(CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED))` in `sdl_app.c` startup, gated on `Config_HasExplicitKey(...)` so the default-ON behavior persists when the key is absent.

This is the same pattern used for `CFG_KEY_FULLSCREEN` (line 9807) and `SDLGameRenderer_SetSuperEffectQualityMode` (line 9979) at startup. (`SDLGameRenderer_SetGhostResolutionMode` was removed in `d9dfe736` when the ghost-resolution knob was retired.)

**Fix:** promote the runtime config key from "out of scope, maybe later" to a step (either as part of step 2 alongside the runtime bool, or as its own brief step before step 7). The cost is roughly ~10 lines spread across two files and removes the need to edit-and-rebuild for baseline capture.

---

## P-2 — should fix

### P-2.1 — Open question 1 (shim vs. move parity helper into `sdl_game_renderer.c`) — verified, plan choice is correct, but reasoning should be cleaner

Reviewer instruction asked: "Read `software_frame_parity.c` end-to-end before answering — does it already reach into static helpers via shims, or does it operate at a different abstraction level?"

**Verified by reading the file end-to-end.** `software_frame_parity.c` calls into `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888` (line 390) — a public, non-static function exported from `software_frame_non_integer.h`. It does NOT reach into any `static` helper in `sdl_game_renderer.c`; the abstraction it uses is the *next layer down* (the standalone non-integer rasterizer module). The parity test currently does **not** exercise `try_fast_copy_fast_textured_task_to_software_frame()` at all (the new INDEX8 path lives there).

So the plan's choice "(a) small shim" (Design §9, line 217) is consistent with the existing abstraction: parity test stays at the public-API layer, shim provides a public surface for a static helper. Moving the parity helper into `sdl_game_renderer.c` would be a regression in terms of code locality (parity tests should live in a dedicated TU).

**Plan's choice is correct.** P-2 only because the rationale in Design §9 is thin ("keeps parity test isolated"); a one-line note that parity already operates at the public-API layer (via `SDLSoftwareFrame_*`) and the shim continues that pattern would help the fix agent justify the choice if challenged later.

---

### P-2.2 — Step 6 should explicitly mention setting up `current_software_palette_lut` for the parity shim

Plan §Step 6 (lines 597-614) describes a shim `SDLGameRenderer_RunIndex8FastPathParityCase` that "constructs a `RenderTask` with the supplied params (color = `0xFFFFFFFFu`, identity modulation), invokes `try_setup_software_frame_fast_copy_plan()` + `try_fast_copy_fast_textured_task_to_software_frame()` directly."

But `try_fast_copy_fast_textured_task_to_software_frame()` (line 6098) reads `task->software_source_is_index8`, `task->software_palette_lut`, **and** (after step 1) `task->software_palette_is_binary_alpha` directly off the `RenderTask`. The shim must populate all three fields, not just `software_palette_lut`. The plan implies this (says "supplied params") but doesn't enumerate. Missing one will produce silent-pass: a `software_palette_is_binary_alpha = false` will skip the new kernel and the test will exercise only the *old* INDEX8 path, defeating the purpose of step 6.

**Fix:** in Step 6 §Files to modify, explicitly list `task->software_source_is_index8 = true; task->software_palette_lut = palette_lut; task->software_palette_is_binary_alpha = palette_is_binary_alpha;` as required init for the shim's `RenderTask`. Also `task->color = 0xFFFFFFFFu;`. The shim signature in the plan already takes `palette_is_binary_alpha` as an explicit param — good — but the body description should make the assignment explicit.

---

### P-2.3 — `RenderTask` field placement inside `#if INDEX8_RASTERIZATION_ENABLED` is correct, but the `false`-default at task-array zero-init needs an explicit memset

Plan §Step 1 (line 274-275): "Add `bool software_palette_is_binary_alpha;` to `RenderTask` (line 89-108) inside the existing `#if INDEX8_RASTERIZATION_ENABLED` block."

`RenderTask` (lines 89-108) — verified — has fields `software_palette_lut` (line 98) and `software_source_is_index8` (line 99) inside `#if INDEX8_RASTERIZATION_ENABLED`. The new bool fits there.

`RenderTask` instances live in `render_tasks[RENDER_TASK_MAX]` (declared as a static array somewhere in the file); they get reused across frames. Lines 11505-11506 explicitly clear `software_source_is_index8 = false; software_palette_lut = NULL;` in the destroy path. The new bool must follow the same lifecycle: any code that clears either of those two fields must also clear the new bool. Step 1 should call this out (and 11506 is the one such site, currently unmentioned).

**Fix:** add to step 1 §Files to modify a bullet for `sdl_game_renderer.c:11506` (destroy/invalidate path): also set `task->software_palette_is_binary_alpha = false;`.

---

### P-2.4 — Step 5 "JSON sub-object" placement — correct line range, but the "trailing comma" guidance is the brittle part

Plan §Step 5 says "in the existing `software_surface_cache_refresh_*` JSON block starting at line 5929, append a new sub-object after the existing `software_surface_cache_refresh_blit_sampling` block."

**Verified:** there are multiple `io_printf(io, "    \"software_surface_cache_refresh_*\": {…},\n", ...)` blocks starting at sdl_app.c line 5933 (`refresh_source_formats`), then `refresh_actual_work` (line 5955+), and several more. Each emits an object closed with `},\n` and each block has a trailing comma after the closing brace. New sub-object insertion is mechanically correct.

The plan's §What to do if it fails item ("check trailing comma — match the existing pattern (each object ends with `},\n` except the last)") is correct but the failure mode is silent in JSON because most parsers tolerate trailing commas. Suggest the §Success criteria explicitly include "feed `perf_capture.json` to `python -c 'import json; json.load(open(...))'` to confirm it parses" — the existing emission has multi-MiB payloads where a typo is hard to spot otherwise.

P-2 because the JSON likely still parses regardless; just adding a one-line verification step would catch the rare formatting issue.

---

### P-2.5 — `populate_scaled_lookup_table` calls — verified, plan is consistent

Plan §Step 4 says the new kernel "reuses the same lookup tables." **Verified:** lines 6291-6294 populate `src_x_lookup` / `src_y_lookup` once before either branch, and the new kernel can sit immediately above line 6296 to share them. No additional calls needed.

OK / verified.

---

## OK / verified

The following plan claims were spot-checked against the source and confirmed:

- **`build_software_palette_lut()` at `sdl_game_renderer.c:9177-9193`** — function definition, 256-entry build loop, signature `(int palette_index, const SDL_Palette* palette)`. Plan §Step 1 cite is exact. Two callers exist: line 11380 (palette refresh) and line 11564 (CreatePalette).
- **`software_palette_lut[FL_PALETTE_MAX][256]` and `software_palette_lut_valid[FL_PALETTE_MAX]`** at lines 176-177 — types and sizes match the plan's proposed parallel `software_palette_lut_is_binary_alpha[FL_PALETTE_MAX]`.
- **`RenderTask` struct fields** at lines 89-108. `software_palette_lut` at 98, `software_source_is_index8` at 99 inside `#if INDEX8_RASTERIZATION_ENABLED`.
- **`color_mod = task->color != 0xFFFFFFFFu`** at line 5576 (plan §Design 2 cites 5575-5576 — close enough; the actual line is 5576).
- **Other `task->color != 0xFFFFFFFFu` sites:** line 7372 and 7545 (consistent with plan §"identity-mod handling at 5576, 7372, 7545").
- **`try_fast_copy_fast_textured_task_to_software_frame()` at line 6098** — function signature exact; INDEX8 fast-path branch at line 6110-6315 verified; `color_mod` branch at 6130-6222 and non-color-mod exact-copy at 6224-6285 confirmed.
- **NEON 4-px helpers `neon_blend_4pixels` (line 5940), `neon_blend_modulate_4pixels` (line 6014)** — confirmed.
- **`__builtin_prefetch` at lines 6152-6153, 6231-6233, 6266-6268, 6300-6302** — confirmed pattern: `(0, 0)` for src reads, `(1, 0)` for dst writes.
- **Scaled INDEX8 path at 6288-6313** — confirmed; lines 6307-6310 are the α=0/α=0xFF/blend triplet.
- **Existing `vbslq_u8` use at 6004, 6072 with mask = `vceqq_u8(alpha, zero)` and arg order `(mask, dst_when_zero, src_when_nonzero)`** — confirmed precedent for plan §Step 3's NEON sketch.
- **`RENDERER_TELEMETRY` macro at lines 44-53** — confirmed; gated on `ENABLE_PERF_TELEMETRY`.
- **`SDLGameRenderer_PerfCaptureRefreshTelemetry` struct at `sdl_game_renderer.h:210-242`** — confirmed; ends with `sampled_full_oversized_*` pair at 240-241; insertion before closing brace at 242 is correct.
- **`perf_capture_refresh_telemetry` storage at `sdl_game_renderer.c:248`** — confirmed.
- **Reset via `SDL_zero(perf_capture_refresh_telemetry)` at line 9581** — confirmed; new fields auto-zeroed.
- **Setter pattern at 9382-9410** — confirmed; new `SDLGameRenderer_SetColorkeyLooseKernelEnabled` would slot in cleanly after line 9412.
- **Header setter declarations at `sdl_game_renderer.h:725-732`** — confirmed; new declaration fits in this block.
- **Existing parity-check entry `SDLApp_RunSoftwareFrameParityCheck()` at `sdl_app.c:1252-1254`** — confirmed; calls `SDLGameRenderer_RunSoftwareFrameParityCheck()`.
- **`software_frame_parity.c:344` (`SDLGameRenderer_RunSoftwareFrameParityCheck`)** — confirmed exists; only exercises `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888` (line 390); does NOT exercise `try_fast_copy_fast_textured_task_to_software_frame`. Plan §Step 6 §Why is correct.
- **Parity helpers `fill_index8_test_source` (line 208), `fill_index8_test_palette` (line 219), `surfaces_match` (line 183), `run_software_source_refresh_parity_check` (line 245)** — all present at the cited lines; current `fill_index8_test_palette` does fill α=255 for every entry (line 225) so the plan's "extend to also produce a binary-α variant" is needed.
- **Baseline binary at `~/Developer/3sx-mister-baselines/2026-04-25-telemetry-instrumented/`** — exists; `source-commit.txt` = `7ed2c2d3` matches current branch HEAD; provenance file documents two uncommitted instrumentation patches that are non-hot-path; usable as a baseline.
- **Build/deploy:** `tools/mister/build-game.sh --flavor telemetry` is correct per `AGENTS.md` line 19-22 (canonical telemetry build).
- **Config plumbing** at `src/port/config/config.h` — `Config_GetBool`, `Config_HasExplicitKey`, `Config_SetString` all exist; existing keys (CFG_KEY_FULLSCREEN, CFG_KEY_GAME_MODE, etc.) follow a uniform pattern; runtime config key for the kernel toggle is one #define + one call site in sdl_app.c startup. (Relevant for P-1.5.)
