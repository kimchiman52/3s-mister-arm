# Review: perf-1 color-key loose-form INDEX8 fast path (commit 7b0d9dba)

Reviewed by reading the actual committed source (not just git diff). Worktree:
`/Users/sb/Developer/3sx-mister-perf`. Build was reported passing; this review
is correctness-only.

## P-1 — must fix before verify

### P-1.1 — `scaled-up` parity test case violates the kernel's own integer-source-rect precondition

`src/port/sdl/software_frame_parity.c:501` defines:

```
{ "scaled-up", { 2.0f, 2.0f, 36.0f, 28.0f }, { 0.10f, 0.10f, 0.40f, 0.40f }, SDL_FLIP_NONE },
```

Source surface is 48 × 32 (line 516–517). The shim's first call is
`build_software_frame_fast_copy_plan` (`src/port/sdl/sdl_game_renderer.c:12404`),
which at lines 5570–5582 computes:

- `src_x_start_f = 0.10 * 48 = 4.8`, `SDL_roundf -> 5`, `nearly_equal(4.8, 5.0)` is
  **false** (epsilon = 0.001 at line 591).

The plan returns `SOFTWARE_FRAME_FAST_COPY_RESULT_NON_INTEGER`. The shim at
`src/port/sdl/sdl_game_renderer.c:12405-12407` returns `false`. The parity test at
line 588 logs `"Index8 fast-path parity shim rejected case index8/strict/scaled-up"`
and returns `false` from `run_index8_fast_path_parity_check()`.

Net result: **the parity check `SDLGameRenderer_RunSoftwareFrameParityCheck()` will
fail unconditionally** every time it runs, including in the verify stage. The
implement agent's claim that "build passes" only covers compilation — not the
parity-check CLI invocation `SDLApp_RunSoftwareFrameParityCheck()` referenced by
the plan §6 success criteria.

**Fix:** change `scaled-up` UV coords to ones that produce integer src coords for
a 48×32 source — e.g. `{ 0.0f, 0.0f, 0.5f, 0.5f }` (yields src 24×16, dst 36×28
non-exact = scaled). Or change the source dims so 0.10 lands on an integer.

### P-1.2 — Parity reference for scaled path uses wrong scaling formula; will mismatch the kernel even when the kernel is correct

`src/port/sdl/software_frame_parity.c:466-489` (the `else` branch of
`raster_reference_index8_loose`) computes:

```
src_x = src_x_int + ((dst_col_idx * src_w_int) / dst_w);
src_y = src_y_int + ((dst_row_idx * src_h_int) / dst_h);
```

That is naive **floor** scaling (truncates towards 0).

The production kernel uses `populate_scaled_lookup_table` at
`src/port/sdl/sdl_game_renderer.c:5810-5823`:

```
const int src_offset = (((dst_offset * 2) + 1) * src_span) / (dst_span * 2);
```

That is **center-aware** rounding (samples the center of each destination
pixel, equivalent to floor-of-midpoint).

These produce different src indices for almost every dst column. For the
`scaled-down` case (src 30 → dst 12, src_origin=0):

- Production lookup[0] = `(0*2+1)*30 / (12*2) = 30/24 = 1`
- Reference     [0]    = `0 * 30 / 12 = 0`

That's a one-column source offset, which means every pixel in the comparison
will mismatch. `surfaces_match` will fail. The flip-axis branch in the
reference (lines 470-471, 480-481) inherits the same wrong formula and so will
also mismatch.

The 4 exact-copy parity cases pass because the `exact_copy` reference path
(lines 450-464) is byte-identical to the production exact-copy kernel. Only
scaled cases fail.

**Fix:** make `raster_reference_index8_loose`'s scaled branch call
`populate_scaled_lookup_table` (or inline the same `((dst_offset * 2) + 1) * src_span / (dst_span * 2)`
formula). This is the only honest "ground truth" — using a different formula
isn't testing the kernel, it's testing a different algorithm.

### P-1.3 — Parity test uses raw `SDL_ConvertSurface` rather than the production palette LUT; introduces a second mismatch source

`src/port/sdl/software_frame_parity.c:428` builds the reference ARGB array via
`SDL_ConvertSurface(index8_src, SDL_PIXELFORMAT_ARGB8888)`, but the kernel
under test reads through the test's `lut[]` (built at line 280's
`build_test_palette_lut`).

If SDL3's `SDL_ConvertSurface` byte-order or alpha-handling for INDEX8 → ARGB8888
differs from the test's `((a<<24) | (r<<16) | (g<<8) | b)` packing in
`build_test_palette_lut` (line 277), the reference and actual will mismatch
even though the kernel is correct.

Specifically, SDL3's INDEX8→ARGB8888 conversion respects the palette's
`SDL_Color.a` field, which `fill_index8_test_palette_strict_binary_alpha` and
`fill_index8_test_palette_loose_binary_alpha` set to 0/255 — that part is fine.
But pixel-byte order in the converted surface depends on
`SDL_PIXELFORMAT_ARGB8888` semantics on the host, which on little-endian hosts
stores `B,G,R,A` in memory while `lut[]` stores `((a<<24)|(r<<16)|(g<<8)|b)`
also `B,G,R,A` in memory — these match in practice but the lack of an explicit
sanity check means a future SDL upgrade could silently break the test.

A more conservative reference: build the ARGB pixel directly from
`build_test_palette_lut`'s LUT, exactly as the kernel does. That removes one
potential source of false negatives. (This is a smaller follow-on to P-1.2; if
P-1.2 is fixed by inlining the kernel's lookup formula, then it's also natural
to use the same LUT lookup the kernel uses, and this concern dissolves.)

## P-2 — should fix

### P-2.1 — `neon_index8_loose_4pixels` operand order: correct, but the comment incorrectly cites line 6014

`src/port/sdl/sdl_game_renderer.c:5958` says
`"vbslq_u8(mask, dst_bytes, result) precedent at sdl_game_renderer.c:6014"`.

Line 6014 in the current file is the `alpha_mask_tbl` literal, not a `vbslq_u8`
call. The actual `vbslq_u8(mask_zero, dst_bytes, result)` precedent is at
**line 6034** (`result = vbslq_u8(mask_zero, dst_bytes, result);`). The
operand order in the new kernel is correct (lanes with mask=1 take from `dst_v`,
which is what we want for α==0 lanes), but a future maintainer chasing the
cited line will land on a no-op literal and be confused.

**Fix:** change `5958` comment to cite line 6034.

### P-2.2 — `_skipped` and `_ineligible` counters double-count when both exact-copy and scaled paths are reached on the same frame

The exact-copy fall-through (line 6315–6320) increments `_skipped` /
`_ineligible` exactly once per task that lands in non-color-mod exact-copy and
falls through. The scaled fall-through (line 6406–6411) does the same in its
branch. Each task hits exactly one of the two branches (the entire
`try_fast_copy_fast_textured_task_to_software_frame` is single-pass), so this
is **not** double-counting. (Initial concern was wrong; verified by reading
the function structure at lines 6128-6431.) **Marking OK** — no fix needed.

### P-2.3 — `clip-top-left` parity case parameter sign produces correct integer rounding but is brittle

`src/port/sdl/software_frame_parity.c:503` uses `dst_rect = { -6.0f, -4.0f, ... }`.
`SDL_roundf(-6.0f) = -6` and `nearly_equal(-6.0f, -6.0f)` is true, so the plan
build accepts it. However, `SDL_roundf` rounds halves toward +∞ on most C
runtimes (C99 `roundf` is "round half away from zero"). If anyone changes the
test value to e.g. `-6.5f`, the plan-build's nearly_equal check will fail
silently. Add a comment near the test cases stating "must be integer-valued
floats". Not blocking.

### P-2.4 — Test-only shim has a `SDL_zero(plan)` that is later overwritten; harmless but noisy

`src/port/sdl/sdl_game_renderer.c:12401-12402` does `SDL_zero(plan); ... build_software_frame_fast_copy_plan(&task, dst_surface, index8_src, &plan);`.
The plan-build fully overwrites the struct on the success paths. The zero
initialization is dead-store on paths where plan-build returns OK. Not a bug;
just style. Skip.

### P-2.5 — JSON emission position is between the cache-refresh-blit-sampling block and the raster-bucket-sampling array

`src/port/sdl/sdl_app.c:6068-6073` emits `"colorkey_loose"` between the
preceding `"software_surface_cache_refresh_blit_sampling"` block (ending
`},\n` at line 6020) and the following `"software_frame_raster_bucket_sampling"`
array opener at line 6074. The trailing `},\n` on line 6070 is correct (a
following key follows). Verified clean. **OK** — no fix.

### P-2.6 — `Index8FastPathCase.flip` uses `SDL_FlipMode` on lines 459-462 of the test harness, but the typedef at line 459 lists case names that don't match `SDLGameRenderer_RunIndex8FastPathParityCase` signature

False alarm — the function is declared `SDL_FlipMode flip` in
`include/port/sdl/sdl_game_renderer.h:749`. Verified consistent. **OK.**

## OK — verified clean

### A. RenderTask plumbing completeness — OK

All 13 `software_palette_lut =` write sites (one declaration + 12 assignments)
have a paired `software_palette_is_binary_alpha =` write within ±2 lines.
Verified at:

- `src/port/sdl/sdl_game_renderer.c:544/546` — global decl pair
- `src/port/sdl/sdl_game_renderer.c:1093/1095` — invalidate-texture clear
- `src/port/sdl/sdl_game_renderer.c:1138/1140` — invalidate-software-cache clear
- `src/port/sdl/sdl_game_renderer.c:1183/1185` — invalidate-by-palette clear
- `src/port/sdl/sdl_game_renderer.c:1229/1231` — invalidate-software-by-palette clear
- `src/port/sdl/sdl_game_renderer.c:1268/1270` — destroy_textures clear
- `src/port/sdl/sdl_game_renderer.c:9468/9470` — SetSoftwareFrameMode clear
- `src/port/sdl/sdl_game_renderer.c:11641/11642` — DestroyTexture render_tasks loop
- `src/port/sdl/sdl_game_renderer.c:11822/11824` — SetTexture index8 branch
- `src/port/sdl/sdl_game_renderer.c:11830/11832` — SetTexture else branch
- `src/port/sdl/sdl_game_renderer.c:11863/11865` — begin_quad_task
- `src/port/sdl/sdl_game_renderer.c:12174/12176` — DrawSprites2Batch
- `src/port/sdl/sdl_game_renderer.c:12336/12338` — DrawInputHistoryGlyph (NULL-clear)

The plan called out four task write sites + six current_software_palette_lut
clears. Implementation has all four task sites + all six clears + the additional
SetTexture/SetSoftwareFrameMode/destroy_textures clears that follow the same
lifecycle pattern. Better than the plan's minimum.

### B. NEON kernel correctness — OK

`src/port/sdl/sdl_game_renderer.c:5959-5968`:

```
const uint32x4_t src_v = vld1q_u32(src);
const uint32x4_t dst_v = vld1q_u32(dst);
const uint32x4_t alpha_v = vshrq_n_u32(src_v, 24);
const uint32x4_t keep_dst_mask = vceqq_u32(alpha_v, vdupq_n_u32(0));
vst1q_u32(dst, vbslq_u32(keep_dst_mask, dst_v, src_v));
```

Operand order matches the existing precedent at line 6034
(`result = vbslq_u8(mask_zero, dst_bytes, result)`): when mask lane = 0xFFFFFFFF
(α==0), output lane takes from `dst_v`. Correct.

The kernel uses scalar gather + vector blend (matches existing pattern at
lines 6188-6198), no `vld4`/`vmovl` widening. Tail is handled at lines 6276-6279
with a scalar fallback. The 4-px iteration matches the existing INDEX8 NEON
kernel.

### C. Color-mod path correctly excluded — OK

The new fast paths are placed inside the non-color-mod branch:

- Exact-copy non-color-mod block begins at `src/port/sdl/sdl_game_renderer.c:6254` after the
  color-mod branch closes at line 6252.
- Scaled fast path at line 6389 — the scaled branch is non-color-mod by
  construction (line 5591-5593: `color_mod && !exact_copy` returns COLOR_MOD,
  caller doesn't enter scaled).

The fast-path entry condition at lines 6261, 6286, 6390 checks
`task->software_palette_is_binary_alpha`, which is set to `false` whenever the
texture isn't INDEX8 or color-mod is implied — covered by the `textured && ...`
gate in `begin_quad_task` (line 11865). Identity mod check is implicit:
non-color-mod is the **else** of the explicit `if (plan->color_mod)` at line 6160.

`_hits` increments are inside each fast-path branch (lines 6263, 6286, 6391) —
not in color-mod. `_skipped` / `_ineligible` are at lines 6315-6319 and
6407-6411 — only in the non-color-mod fall-through.

### D. Config-key gate — OK

- `CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED` defined at `src/port/config/config.h:23`
- `sdl_app.c:9988-9990` reads it via `Config_GetBool` gated on `Config_HasExplicitKey`
  (precedent: `Config_GetBool(CFG_KEY_FULLSCREEN)` at line 9813)
- `SDLGameRenderer_SetColorkeyLooseKernelEnabled` defined at
  `src/port/sdl/sdl_game_renderer.c:9545-9547`, declared at header line 736
- Default is `true` at line 173; `Config_HasExplicitKey` guard preserves default
- Fast-path entry condition reads the bool; when false, the fast-path branch
  is skipped and the existing kernel runs at lines 6321+ / 6413+

### E. Parity test wiring — OK at the call-graph level

- Shim is non-static, declared at header line 744, defined at game_renderer line 12378.
- Shim sets all required `RenderTask` fields explicitly: `color`,
  `software_source_surface`, `software_source_is_index8`, `software_palette_lut`,
  `software_palette_is_binary_alpha`, `dst_rect`, `src_uv_rect`, `flip`
  (lines 12390-12399).
- Test calls the shim and compares against a reference, byte-comparing via
  `surfaces_match` (line 592).
- Coverage: 8 cases × 2 palette flavors = 16 cases, including unscaled forward,
  flip-h, flip-v, flip-both, scaled-up, scaled-down, clip-top-left,
  clip-bottom-right.

But: **the test will fail at runtime due to P-1.1 and P-1.2.**

### F. NEON kernel `src_x_step == 1` gate — OK

`src/port/sdl/sdl_game_renderer.c:6261` — gate is
`colorkey_loose_kernel_enabled && task->software_palette_is_binary_alpha && src_x_step == 1`.
Matches existing pattern at line 6173. Flipped horizontal scan falls through
to the scalar loose-form kernel at line 6286.

### G. Prefetch — OK

NEON loose-form: lines 6266-6269. Scalar loose-form: 6291-6294. Scaled scalar
loose-form: 6395-6398. All three replicate the existing pattern: prefetch one
src row ahead and one dst row ahead with hint flags `(0, 0)` and `(1, 0)`.
Matches existing kernels at lines 6181-6184, 6225-6228, 6266-6268, 6300-6302.

### H. Header struct ordering / ABI — OK

Three new `Uint64` fields appended at the end of
`SDLGameRenderer_PerfCaptureRefreshTelemetry` at
`include/port/sdl/sdl_game_renderer.h:242-244`, before the closing brace at
line 245. No insertion in middle of struct.

`RenderTask` got one new bool `software_palette_is_binary_alpha` inside the
existing `#if INDEX8_RASTERIZATION_ENABLED` block at line 100, immediately
after `software_source_is_index8`. Same conditional-compile context. OK.

### Pre-existing telemetry strings unchanged — OK

`git show 7b0d9dba -- src/port/sdl/sdl_game_renderer.c | grep -E 'palette_alpha_histogram|soft_path_bind'`
returns no matches. The diff doesn't touch those strings.

### Unused variables, signedness, implicit casts — none observed

- `neon_index8_loose_4pixels` marked `static inline __attribute__((unused))`
  matching the existing `neon_blend_4pixels` style (which is conditionally
  used only on NEON-enabled targets).
- All counter increments cast to `unsigned long long` in the JSON formatter.
- All loop variables are `int` and stay within int range.
- `(Uint8)((i * 37) & 0xFF)` style casts in the test palette fillers are
  explicit, no implicit narrowing.

## Items I tried to verify but couldn't conclude

- **Cache-warmup behavior under task-slot reuse:** the destroy-texture clear at
  line 11642 sets the bool to false. But task slots are also reused via
  `render_task_count = 0` resets between frames (file declares `render_tasks`
  as a 1024-entry static array). Without instrumenting at runtime I can't
  confirm there's no "carry-over" of stale `software_palette_is_binary_alpha=true`
  from a prior frame's task that overlaps a new frame's task. The plan §1.1
  flagged this; the destroy-path clear at 11642 covers texture-destroy reuse,
  but not generic frame-to-frame reuse. The frame reset path likely
  re-initializes all task fields on `begin_quad_task` (line 11860), but I did
  not exhaustively trace every entry into `render_tasks[*]`. Suggest the fix
  agent verify by adding an `SDL_assert` on first-use that the bool is
  consistent with the source's INDEX8/palette state.

- **Parity test pre-existing CLI invocation:** the parity check is reachable
  via `SDLApp_RunSoftwareFrameParityCheck` per plan, but I did not trace the
  argv path that triggers it. P-1.1 and P-1.2 mean it will fail when invoked,
  regardless.

## Confirmation

I opened and read the actual source files in the worktree:

- `/Users/sb/Developer/3sx-mister-perf/src/port/sdl/sdl_game_renderer.c` (multiple offsets)
- `/Users/sb/Developer/3sx-mister-perf/include/port/sdl/sdl_game_renderer.h`
- `/Users/sb/Developer/3sx-mister-perf/src/port/config/config.h`
- `/Users/sb/Developer/3sx-mister-perf/src/port/sdl/sdl_app.c`
- `/Users/sb/Developer/3sx-mister-perf/src/port/sdl/software_frame_parity.c`

This was not a git-diff-only review.
