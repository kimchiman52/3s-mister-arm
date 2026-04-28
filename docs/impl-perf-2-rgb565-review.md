# Review: perf-2 RGB565 canvas (commit f70d99bd)

Reviewer: independent review agent (read source files, not just diff).
Scope: 1405 lines of changes across renderer + parity + presenter for the
RGB565 software_frame canvas mode.

## Summary

Two P-1 issues found. The single biggest issue is that `raster_textured_task_to_software_frame` has TWO sub-paths that the per-format dispatch missed entirely, both of which silently corrupt a 565 destination by casting `Uint32*` and using `pitch / sizeof(Uint32)`. Build passing means the casts are not type-errors, but they are semantic corruption. Three smaller P-2 polish notes.

The writer-availability gate, defense-in-depth check at the upload path, in-kernel LUT565 lookup (no per-task pointer), parity-test 565 plumbing, surfaces_match rewrite, blend math, sa_bg dispatch, and counter ownership all check out.

## P-1 (must fix before merge)

### P-1.A: COLOR_MOD lookup-table path is not 565-aware

**File / lines:** `src/port/sdl/sdl_game_renderer.c` 7741-7918 (the `if (fast_copy_result == SOFTWARE_FRAME_FAST_COPY_RESULT_COLOR_MOD ...)` block inside `raster_textured_task_to_software_frame`).

**What is wrong:**
- Lines 7791-7794: unconditionally
  ```
  const Uint32* cm_src_pixels = (const Uint32*)src_surface->pixels;
  Uint32* cm_dst_pixels = (Uint32*)dst_surface->pixels;
  const int cm_src_pitch = src_surface->pitch / (int)sizeof(Uint32);
  const int cm_dst_pitch = dst_surface->pitch / (int)sizeof(Uint32);
  ```
- Lines 7847, 7898, 7901 then write `Uint32` values via `cm_dst_row` and call `blend_argb8888_opaque_dst`.
- The block ends with `return true;` at 7916 — there is no fall-through to the format-aware ARGB-source branch at 8102.

**When it fires:** `build_software_frame_fast_copy_plan` returns `COLOR_MOD` whenever `task->color != 0xFFFFFFFFu` AND the task is non-exact (scaled) AND non-INDEX8. The ABGR1555 cache and INDEX4LSB-via-cache tasks mentioned in the comment at 6665-6668 can hit this when they're color-modulated and scaled. The exact production frequency is unknown, but the path is reachable on a 565 canvas, and when it fires it writes 4 bytes per pixel into a 2-byte-per-pixel buffer — silent corruption + write past the row.

**Mirroring expectation:** Section E of the review brief required format-aware dispatch on `dst_surface->format` for every kernel family that touches the canvas. The implementer dispatched the generic ARGB path at 8102 and the INDEX8 fast/scaled paths at 6227+, but missed this COLOR_MOD lookup-table sub-path, which is a separate kernel sandwich between them.

### P-1.B: INDEX8 generic (non-fast-copy) path is not 565-aware

**File / lines:** `src/port/sdl/sdl_game_renderer.c` 7922-8096 (the `#if INDEX8_RASTERIZATION_ENABLED ... if (task->software_source_is_index8 ...)` fall-through block in `raster_textured_task_to_software_frame`).

**What is wrong:**
- Lines 7932-7933:
  ```
  Uint32* i8_dst_pixels = (Uint32*)dst_surface->pixels;
  const int i8_dst_pitch = dst_surface->pitch / (int)sizeof(Uint32);
  ```
- Lines 7988, 8031-8032, 8077-8078 write Uint32 to `dst_row` and call `blend_argb8888_opaque_dst`.
- Block ends with `return true;` at 8095.

**When it fires:** This is the INDEX8 fall-through after `try_fast_copy_*` returned but the plan result was neither EXACT nor SCALED nor COLOR_MOD nor NON_INTEGER (i.e., `SOURCE_BOUNDS`, `UNSUPPORTED_FLIP`, or any non-INDEX8-suppressed COLOR_MOD that fell through after the cm_valid check at 7762 failed). Less common than P-1.A but still reachable on a 565 canvas with INDEX8 source, with the same corruption signature.

**Note:** The comment at line 7923 says this path "handles all fallthrough cases (non-integer too small, COLOR_MOD with non-integer, and generic float UV) for INDEX8 textures." So the breadth is wider than the trigger conditions imply. It is the catch-all INDEX8 generic path.

## P-2 (nice to fix but does not block correctness)

### P-2.A: Parity test entry has no caller — 565 cases are not runtime-verified

**Files:** `src/port/sdl/software_frame_parity.c:702` `SDLGameRenderer_RunSoftwareFrameParityCheck` and `src/port/sdl/sdl_app.c:1258` `SDLApp_RunSoftwareFrameParityCheck`. Both are defined; nothing calls them. The implementer noted this; recording it formally so the fix agent / next runtime-test pass knows the 565 parity matrix has been compiled but not executed.

The shim plumbing (LUT565 setup at `sdl_game_renderer.c:13256-13272`, restore at 13301-13304) and reference raster (`software_frame_parity.c:498-512`, `:531-543`) look correct on inspection.

### P-2.B: 565 sa_bg_cache scaled restore is faithful but skips the SDL_memcpy fast path mismatch protection

**File:** `src/port/sdl/sdl_game_renderer.c:3807-3809` (the `else { SDL_memcpy(...) }` no-transform branch in `apply_super_effect_burst_reduction_after_sort`).

The memcpy uses `software_frame_surface->pitch * software_frame_surface->h` which is correct for both formats since both surfaces have the same format (forced by `ensure_sa_bg_cache_surface` at 3442-3445). But the comment at 3807 says "byte-count-driven so it works for both formats" — accurate but does not assert the same-format invariant. If a future change creates `sa_bg_cache_surface` with a different format than `software_frame_surface`, this memcpy will silently produce nonsense. A `SDL_assert(sa_bg_cache_surface->format == software_frame_surface->format);` here would prevent that regression. Cosmetic; existing code compiles and works.

### P-2.C: `surfaces_match` requires identical formats; cross-format diff is treated as setup error

**File:** `src/port/sdl/software_frame_parity.c:189-196`. The rewrite is correct per the brief (per-row SDL_memcmp keyed on `SDL_BYTESPERPIXEL`, per-surface pitch). One subtle observation: the early `expected->format != actual->format` check returns false. This is correct for the parity harness (both surfaces are created with the same `dst_formats[df].format` at lines 631-632), but the brief mentioned "tolerates differing pitches (one surface may have padding)". That is satisfied. Mismatched formats remain rejected, which is the right behavior — they cannot be byte-compared meaningfully.

## OK (verified, no issue)

- **Section A — writer-availability gate:** `rgb565_canvas_enabled` defaults `false` at `sdl_game_renderer.c:182`. Flipped to true at `sdl_app.c:9883-9885` only if `native_video_writer_enabled` (set by `NativeVideoWriter_Init()` at 9874). Config-driven path at 10018-10028 also guards on `native_video_writer_enabled` before flipping true; an explicit `=false` is always honored.
- **Section B — defense-in-depth at upload:** `upload_software_frame_to_canvas` checks `software_frame_surface->format != SDL_PIXELFORMAT_ARGB8888` at `sdl_game_renderer.c:9586`, logs once, returns false rather than running `SDL_UpdateTexture` against the ARGB8888 staging texture.
- **Section D — no per-task LUT565:** `grep software_palette_lut_565 src/port/sdl/*.c include/port/sdl/*.h` finds only the global storage decl at `sdl_game_renderer.c:197`, the build pass at 10155, the in-kernel handle-based read at 6236, the parity-shim setup at 13256-13272 / 13294-13304. No `task->...` field added.
- **Section E partial — format dispatch coverage that IS in place:**
  - INDEX8 fast-copy exact + scaled at `sdl_game_renderer.c:6227+` (color-mod and non-color-mod, including loose-form fast path)
  - Generic ARGB-source fast-copy at 6669+
  - Parallelogram at 8362+
  - Float-parallelogram at 8554+
  - Float-triangle at 8698+
  - Translated-triangle at 8930+
  - Solid-triangle at 9188+ (line 9234 dispatch, 9273-9293 dispatch in span loop)
  - Solid-rect + diagonal-strip at 7225+ and 9342+
  - Generic ARGB float-UV path at 8102+ (scalar 565 sibling, NEON 8888 deferred)
  - Non-integer at `software_frame_non_integer.c:763` plus dispatch at `sdl_game_renderer.c:7695-7712`
  - sa_bg cache scaled restore at 3799-3804
  - The two MISSING dispatches are the P-1 items above (COLOR_MOD lookup-table path; INDEX8 generic fall-through).
- **Section F — blend math:** `pack_rgb565_from_argb` (7112-7117), `expand_rgb565_to_argb_channels` (7119-7129), `blend_rgb565_opaque_dst` (7134-7152) all correct. R/B 5-bit, G 6-bit. α=0 fast-skip at 7136, α=255 overwrite at 7139, partial-α expand→blend→repack at 7142-7151.
- **Section G — solid-fill α-loss documented:** `blend_solid_rgb565` (7210-7223) only mirrors the dst_a==255 branch of 8888; comment at 7205-7209 explicitly documents that the canvas is α-less / opaque-by-construction and that's why this is correct.
- **Section H — surfaces_match rewrite:** `software_frame_parity.c:188-216`. Uses `SDL_memcmp`, per-row, with `SDL_BYTESPERPIXEL(format)` and per-surface pitch. Correct for any pixel size SDL produces.
- **Section I partial — parity test plumbing:** Test matrix at `software_frame_parity.c:587-590` declares both ARGB8888 and RGB565 dst formats. Loop at 599-696 produces `dst_formats × flavors × cases` = 2 × 2 × 8 = 32 cases. LUT565[0] populated at `sdl_game_renderer.c:13262-13272`, restored at 13301-13304. Reference uses INDEX8 + LUT8888 + `parity_pack_rgb565_from_argb` directly — no `SDL_ConvertSurface` intermediary. (See P-2.A: not invoked.)
- **Section J — counter ownership:** `convert_pass_skipped_frames_counter` defined `static` at `sdl_app.c:168`. Incremented in present-path bypass at 10583 (the `frame->format == SDL_PIXELFORMAT_RGB565` branch). Emitted in JSON at 6080-6088 ("rgb565_canvas" block) directly from sdl_app.c, not via a renderer setter.
- **Section K — item-1 telemetry untouched:** `git show f70d99bd -- src/port/sdl/sdl_game_renderer.c | grep -E 'palette_alpha_histogram|soft_path_bind'` returned zero lines, confirming the item-1 instrumentation is unchanged.
- **Section L — fbdev_presenter 565 overlay:** `FBDevPresenter_ApplyFPSOverlayToRGB565Buffer` at `fbdev_presenter.c:2618-2666` for MiSTer, stub at 2748-2752 for non-MiSTer. Helper `apply_rasterized_fps_overlay_to_rgb565_buffer` at 1192-1213 reuses `fps_overlay_pixels` glyph data — no separate atlas. Naming matches existing `FBDevPresenter_*` convention; header decl at `fbdev_presenter.h:108`.
- **Section M — no obvious unused variables / sign warnings:** `(void)dst_pixels_16; (void)dst_pitch_16` at 6239 / 8387 / 8579 properly suppress the unused-local warning when dispatch picks one branch but both are computed up front.
- **Canvas allocation timing:** `ensure_software_frame_surface` (3402-3415) reads `rgb565_canvas_enabled` at allocation time. Allocation is lazy (called from `SDLGameRenderer_BeginFrame` at 10471). The setter at 10402-10408 documents stickiness with a comment, and `sdl_app.c` calls the setter before the first frame begins.
- **Canvas clear:** uses `SDL_MapSurfaceRGBA` (10477) which is format-aware.
