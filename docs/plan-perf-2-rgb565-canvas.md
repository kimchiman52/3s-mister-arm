# Plan — perf-2 — RGB565 software_frame_surface on MiSTer

**Worktree:** `/Users/sb/Developer/3sx-mister-perf` (branch `perf`)
**Source-of-truth:** all paths in this plan are relative to that worktree.
**Background:** `/Users/sb/Developer/3sx-mister/docs/research-renderer-external-comparison.md`
(§4.1 = ARGB8888 inventory, §2.7 = palettes, §2.11 = present path) and the
item-1 plan at `docs/plan-perf-1-colorkey-loose.md` (committed `5c855952`,
implementation `7b0d9dba`, fix `4d58c909`). Telemetry from item-1 confirmed
854/854 live palettes are binary-α — the loose-form INDEX8 fast path already
ships and is what we extend here.

## Goal

Switch `software_frame_surface` from `SDL_PIXELFORMAT_ARGB8888` to
`SDL_PIXELFORMAT_RGB565` on MiSTer. Eliminate the per-frame
`convert_argb8888_to_rgb565` pass at `src/port/sdl/sdl_app.c:186-226, 10536-10539`.
Halve destination-side memory bandwidth in every row kernel that writes the
canvas. Keep the desktop / non-MiSTer path on ARGB8888 (it uses the
`cps3_canvas` `SDL_Texture`, not the software surface).

The 565 source-over blend approximation is bit-exact for indexed sources whose
palette LUT is binary-α (§4.2 of research). Telemetry has proven that all
palettes in this workload are binary-α; blend math is only retained for the
few ABGR1555 textures and color-mod paths that synthesize partial α. The
canvas-format swap is therefore a mechanical reshape of the kernel set and
the present-path glue, not a precision-tradeoff bet.

## Scope decisions (with rationale)

The orchestrator brief lists 12 concrete inputs. Each is decided here:

### 1. Canvas format swap

`software_frame_surface` (declared `sdl_game_renderer.c:149`, created
`sdl_game_renderer.c:3387`) becomes `SDL_PIXELFORMAT_RGB565` when the runtime
gate `rgb565_canvas_enabled` is on **and** the native video writer is both
enabled and successfully initialized (see §10 "Gating" for the writer-availability
guard added in response to P-1.2). `cps3_canvas` (`SDL_Texture`, created at
`sdl_game_renderer.c:9439-9440` as `SDL_PIXELFORMAT_RGBA8888`, used by the
desktop / fbdev fallback path) stays RGBA8888 — that path does not write
into `software_frame_surface`.

Two ARGB8888 format asserts at `sdl_game_renderer.c:2899` and `:3233` must
also be widened to allow either canvas format (they assert against
`software_source_surface`, but read the format from the canvas in branches —
verify each before edit; see Step 1).

### 2. Palette LUT mirror

Add `static Uint16 software_palette_lut_565[FL_PALETTE_MAX][256]` next to
`software_palette_lut[][]` at `sdl_game_renderer.c:181`. Build alongside the
ARGB LUT in `build_software_palette_lut` at `sdl_game_renderer.c:9294-9323`
(the function we already touched in item 1 to add the binary-α scan). Memory
cost: `1088 × 256 × 2 B = 557,056 B ≈ 0.56 MB`, on top of the 1.125 MB ARGB
LUT.

### 3. 565 row kernels

Item-1 added the loose-form INDEX8 kernels at `sdl_game_renderer.c:6260-6311`
(NEON + scalar exact-copy) and `sdl_game_renderer.c:6389-6405` (scalar
scaled). Those write `Uint32` into the canvas. We add 565 variants of each
loose-form kernel — same loop structure, same `(α==0?skip:store)` decision,
but reading from `software_palette_lut_565[][]` and writing `Uint16`. The
existing ARGB8888 versions stay as-is and are gated by canvas format (see
§Format-aware dispatch below). Other rasterizer paths needing 565 variants:

- Color-mod INDEX8 at `sdl_game_renderer.c:6160-6251` (NEON + scalar both
  branches).
- ARGB8888 fast-copy branches at `sdl_game_renderer.c:6439-6622` (color-mod
  exact, non-color-mod exact) and `:6625-6657` (scaled). These exist for the
  rare INDEX4LSB-feeding-via-ARGB-cache and ABGR1555 cases.
- Parallelogram path at `sdl_game_renderer.c:7850-7962` (both INDEX8 and
  ARGB8888 branches; INDEX8 is at `7875-7907`, ARGB at `7909-7951`).
- Float-parallelogram path at `sdl_game_renderer.c:7964-8055` (handles
  INDEX8 via `fp_palette_lut`).
- Triangle barycentric fallbacks: `raster_textured_float_triangle_to_software_frame`
  at `:8057-8131`, `raster_textured_triangle_to_software_frame` at
  `:8133-8209`, `raster_textured_translated_triangle_to_software_frame` at
  `:8211-8299`.
- Solid-triangle path at `:8418-8504` (uses `fill_argb8888_span` and the
  inline `blend_solid_argb8888`).
- `raster_solid_task_to_software_frame` solid rect path at `:8540-8567`.
- Non-integer lookup raster at
  `src/port/sdl/software_frame_non_integer.c:452-716`.
- The sa-bg-cache nearest-neighbor scaled blit at
  `sdl_game_renderer.c:3630-3663`.

The volume is meaningful but the duplications are mechanical — each one is
the same control flow with the channel arithmetic swapped from 8888 to 565.

### 4. Blend math

Retain three primitives:

- `blend_argb8888_opaque_dst` (def `sdl_game_renderer.c:6757-6776`) — the
  hot-path source-over. ~50 sites grep-able from §4.1 of research; all of
  them are partial-α blends that fire only when (a) color-mod synthesizes
  partial-α (`(src_a * mod_a + 128) >> 8` ∈ (0,255)), or (b) source is ABGR1555
  with the rare partial-α encoding. **This is the one we must port carefully.**
- `blend_argb8888` (def `sdl_game_renderer.c:6714-6749`, marked
  `__attribute__((unused))`) and `blend_argb8888` in
  `software_frame_non_integer.c:68-103` — same shape but generic dst-α. The
  one in non-integer is the one that actually fires.
- `blend_solid_argb8888` (def `sdl_game_renderer.c:6795+`) — partial-α solid
  fill.

Add `blend_rgb565_opaque_dst`, `blend_rgb565` (in non-integer), and
`blend_solid_rgb565`. Standard expand-blend-repack: extract 5/6/5 → expand
to 8 bits via `(c << 3) | (c >> 2)` (R/B) and `(c << 2) | (c >> 4)` (G), do
the same `(s*a + d*(255-a) + 128) >> 8` arithmetic on each channel, repack
to 565. Hot path is `blend_rgb565_opaque_dst` — it is what gets hit on every
color-mod render of a Yun ghost sprite and every modulated burst sprite.

Channel-precision note: the 565 round-trip drops 3 LSB of R/B and 2 LSB of
G per channel, with at most ±1 LSB error in the 565 result vs the 8888 result
truncated to 565. The ghost-sprite half-resolution mode
(`modulate_argb8888_blue_tint` at `sdl_game_renderer.c:6687-6701`) is already
an approximation; additional 565 quantization is bounded.

### 5. Solid fill

Add `fill_rgb565_span(Uint16* dst_pixels, int pixel_count, Uint16 color)` next
to `fill_argb8888_span` (def `sdl_game_renderer.c:6778-6794`). Grep verifies
**3 callers** in 3 distinct kernel functions:

- `sdl_game_renderer.c:6859` (inside `raster_full_height_diagonal_strip_to_software_frame`).
- `sdl_game_renderer.c:8494` (inside `raster_solid_triangle_to_software_frame`).
- `sdl_game_renderer.c:8553` (inside `raster_solid_task_to_software_frame`,
  axis-aligned rect fast path).

Separately, `blend_solid_argb8888` (def `:6795-6828`) has 3 callers at
`:6881`, `:8497`, `:8563` — these are the partial-α fall-through paths inside
the same three functions; each needs a parallel `blend_solid_rgb565` call.

Each call site converts to `fill_rgb565_span` / `blend_solid_rgb565` when
`dst_format == 565`, else keeps the existing call.

### 6. Modulation: choose strategy (b) — modulate from cached LUT 8888 channels

Two options:

(a) Expand the 565 dst pixel and the 565 src pixel to 8888, modulate as 8888,
    repack to 565.
(b) Modulate from the LUT-cached 8888 channels directly before storing 565
    (i.e. the `palette_lut[i]` lookup feeds the modulator, the modulator
    output is then packed-to-565 once).

**Pick (b).** Rationale: the modulator already operates on the LUT's ARGB
output. The hot-path INDEX8 color-mod kernel at `sdl_game_renderer.c:6160-6251`
is a sequence `palette_lut[i8_row[x]] → modulate → α-decide → store`. With (a)
we'd add an 565-expand on the dst read **and** an 565-pack on store — two
extra channel shuffles per pixel. With (b) we keep the existing modulate
math producing an ARGB8888 intermediate, then pack to 565 once at store time
(the `α==0?skip:store` decision uses the ARGB α, then the store is a
packed-565 word). On Cortex-A9 each saved 8888-expand of dst is ~3 ALU ops;
across the inner loop that's 6 ops/pixel saved. The LUT was already 1.125 MB
and stays — no extra storage. The 565 LUT mirror is only used by the
non-color-mod loose-form fast paths (steps 5–7 of this plan).

The `modulate_argb8888_blue_tint` blue-tint shortcut at
`sdl_game_renderer.c:6687-6701` keeps its 8888 form — the path remains
`modulate_argb8888_blue_tint(palette_lut[i], rg_factor, mod_a) → pack-565`.
No new `modulate_rgb565_blue_tint` is needed.

**Channel-loss caveat (P-2.6).** 565 cannot store partial dst α — the canvas
is α-less by construction. In the production renderer the canvas is cleared
opaque and `blend_argb8888_opaque_dst` always returns `0xFF000000u | …`, so
chained blends through the **renderer** primitives keep dst_α at 255 by
design. The `software_frame_non_integer.c:68-103` `blend_argb8888` is generic-α
and could in principle leave dst_α < 255 in the 8888 canvas, but its only
caller is the non-integer lookup raster, whose source α is the same LUT8888
α as everywhere else (item-1 confirms 854/854 live palettes are binary-α —
α ∈ {0, 255} only — so the generic-α path collapses to opaque-dst in
practice). The 565 sibling deliberately drops the α store; this is a
behavioral tightening, not a regression. The parity test (Step 8) covers
both palette flavors (strict/loose) on both dst formats, so any silent
divergence on a non-binary-α palette would be caught there. If a future
content set ever introduces a true partial-α palette, the 565 canvas would
quantize α to 0 vs 255 by the LUT8888 α threshold and the loose-form fast
path would be ineligible (item-1 guard); the binary-α-aware kernel selection
remains the correctness gate.

### 7. FPS overlay: choose strategy (a) — add a 565 variant

`FBDevPresenter_ApplyFPSOverlayToBuffer` at `fbdev_presenter.c:2538-2589`
calls `apply_rasterized_fps_overlay_to_argb_buffer` at
`fbdev_presenter.c:1169-1186`. The latter is a `SDL_memcpy` of a row of
ARGB8888 pixels per glyph row.

(a) Add a 565 variant `apply_rasterized_fps_overlay_to_rgb565_buffer` plus a
    public `FBDevPresenter_ApplyFPSOverlayToRGB565Buffer`. The
    pre-rasterized overlay glyph atlas (`fps_overlay_pixels`) is built once
    in ARGB8888; we either build a 565 mirror or convert per-blit. Per-blit
    convert at FPS-overlay rate (~32×7 pixels per row, top-left corner once
    per frame) is trivial CPU and avoids a 565 atlas-cache field.
(b) Inline ARGB→565 patch at the call site in `sdl_app.c:10533-10535`.

**Pick (a).** Rationale: keeps the present-path call site clean
(`if (canvas_565) FBDevPresenter_ApplyFPSOverlayToRGB565Buffer(...)`), keeps
the overlay layout/anchor code in one place, and matches the precedent of
keeping pixel-format choices inside the presenter rather than the
glue. Per-blit convert is fine — fps-overlay rasterization already runs once
per frame and the overlay is ≤100 pixels tall; 8888→565 of that is
microseconds.

### 8. sa_bg_cache_surface

Created at `sdl_game_renderer.c:3413` as ARGB8888. Used as a memcpy backup of
`software_frame_surface`. **Must follow** the canvas format. When canvas is
565, `sa_bg_cache_surface` is also 565; the existing `SDL_memcpy` at
`:3732-3733` and `:8736-8738` carries arbitrary bytes and is format-agnostic.
The scaled-restore kernel at `sdl_game_renderer.c:3630-3663` dereferences
both surfaces as `Uint32*` (`(const Uint32*)sa_bg_cache_surface->pixels` at
`:3643`, `(Uint32*)software_frame_surface->pixels` at `:3644`) — that
function needs a 565 variant or an internal format dispatch.

### 9. Present path: choose strategy (b) — point writer directly at canvas, drop scratch

(a) Keep `native_video_rgb565_scratch` and turn `convert_argb8888_to_rgb565`
    into a `memcpy`.
(b) Point `NativeVideoWriter_WriteFrame` at `software_frame_surface->pixels`
    directly and remove both the scratch buffer and the convert function
    when `rgb565_canvas_enabled` is on.

**Pick (b).** Rationale: the scratch was only needed because the canvas was
ARGB8888. With a 565 canvas, the scratch is *the same data* as
`frame->pixels`. A `memcpy` accomplishes nothing — the writer's internal
`memcpy` to DDR3 (`native_video_writer.c:76-107`) is the actual bus
transfer. Keeping the scratch buys us a redundant 172 KB read+write per
frame, defeating the bandwidth saving the entire plan exists for.

Safety: the writer is documented to `memcpy` once when `pitch == 384*2` and
falls back to row-by-row otherwise (`native_video_writer.c:76-93`). The
canvas surface's pitch is set by SDL on creation; for a 384×224 565 surface
SDL pads to 4-byte alignment which gives `pitch == 384*2 = 768` (already
4-aligned). The plan does **not** add an `SDL_assert(frame->pitch == 384 * 2)`
— the writer already handles the unexpected case correctly, and a hard
assert would convert a recoverable runtime difference into a debug-build
crash. If observability into the unexpected case is wanted later, add a
one-shot `backend_logf` warning, not an assert.

We do retain `convert_argb8888_to_rgb565` as a dead-code function for the
fbdev path which is still ARGB8888 (the orchestrator notes fbdev is dead on
shipped MiSTer builds, but the code remains). Mark it `__attribute__((unused))`
under the new gate so the compiler doesn't warn; final cleanup is a
follow-up.

### 10. Gating

Runtime config key `CFG_KEY_RGB565_CANVAS_ENABLED` mirroring item 1's
`CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED`. Default ON for MiSTer
(`PORT_MISTER`); ON-by-default produces the bandwidth savings the user wants
without asking. Default OFF for desktop because the desktop path uses
`cps3_canvas`, not `software_frame_surface`, and a 565 software surface there
would still go through SDL's renderer which would re-convert.

Wire through `Config_GetBool` / `Config_HasExplicitKey` at the same call site
where item 1 wired its setter — `src/port/sdl/sdl_app.c:9988-9990`. Setter
declared in `include/port/sdl/sdl_game_renderer.h` next to
`SDLGameRenderer_SetColorkeyLooseKernelEnabled` at `sdl_game_renderer.h:736`.

**Native-video-writer availability guard (P-1.2 mitigation — chosen
strategy: gate canvas allocation on writer availability).** The 565 canvas
saves bandwidth only on the native-video-writer path. The fbdev / desktop
fallback path goes through `upload_software_frame_to_canvas()` at
`sdl_game_renderer.c:8753-8777`, which calls `SDL_UpdateTexture` into
`software_frame_upload_texture` — created `SDL_PIXELFORMAT_ARGB8888` at
`:3396-3397`. If the canvas were 565 but the writer was unavailable
(`THIRDSARM_NATIVE_VIDEO=0` env var, `NativeVideoWriter_Init` failed, or any
non-MiSTer build), `SDL_UpdateTexture` would reinterpret 565 bytes as 8888
and silently corrupt every frame.

We therefore gate the **canvas-format choice** on
`native_video_writer_enabled` at allocation time. Concretely,
`SDLGameRenderer_SetRGB565CanvasEnabled(true)` is only called from
`sdl_app.c` **after** `NativeVideoWriter_Init()` has returned true. If the
writer is disabled (env var) or fails to init, the setter is never called
and `rgb565_canvas_enabled` stays at its default — under PORT_MISTER the
default is therefore changed from "true" to "false" and the explicit setter
call is what flips it on.

This makes the 565 canvas opt-in via successful writer init, not just a
PORT_MISTER compile flag. Three failure modes that previously would have
silently corrupted are now safe:

- `THIRDSARM_NATIVE_VIDEO=0` → `native_video_writer_enabled = false` →
  setter not called → canvas stays 8888, fbdev fallback works as before.
- `NativeVideoWriter_Init()` returns false (no FPGA mapping) → same path,
  canvas stays 8888.
- Future ports that build PORT_MISTER but lack the FPGA writer would also
  fall back cleanly.

The `software_frame_upload_texture` itself is **not** made format-aware in
this plan — by construction, when it runs, the canvas is guaranteed 8888,
so its current `SDL_PIXELFORMAT_ARGB8888` is correct. As a defense-in-depth
guard, `upload_software_frame_to_canvas()` at `:8753` adds a one-shot
runtime check: `if (software_frame_surface->format != SDL_PIXELFORMAT_ARGB8888)
{ backend_logf once + return false; }`. This is a belt-and-suspenders
catch — if a future change ever lets a 565 canvas reach this path, it logs
and bails rather than corrupting.

The runtime gate must be checked **once at canvas-creation time** in
`ensure_software_frame_surface()` at `sdl_game_renderer.c:3382-3389`. The
chosen format is then sticky for the run — flipping the toggle mid-run is
not supported and is not needed (kernel-level A/B is still possible via the
item-1 toggle). Format dispatch in inner loops reads
`software_frame_surface->format` directly, not the runtime bool — see the
next subsection.

Other mitigations considered and rejected:

- **(A) Make `software_frame_upload_texture` format-aware.** Possible —
  read canvas format and create a matching texture. Rejected because the
  upload path is the *fallback* path and its only consumers are the
  desktop SDL renderer (which expects ARGB8888 textures for `cps3_canvas`)
  and the dead fbdev presenter. The added complexity would only ever be
  exercised under failure modes; the writer-availability guard removes
  those modes by construction.
- **(B) Insert an explicit 565→8888 expand at upload time.** Slow,
  redundant, and would defeat the bandwidth saving on a path that should
  not be reached anyway. Rejected.
- **(D) Allow 565 canvas without NVW, plus runtime format check.** This
  was considered but adds an awkward branch to every `SDL_UpdateTexture`
  call. Rejected in favor of (C) which keeps the format invariant simple
  ("if NVW is up, canvas may be 565; otherwise canvas is 8888").

**Verification.** Plan Step 5 success criteria are extended to include the
NVW-disabled smoke: deploy a binary with `THIRDSARM_NATIVE_VIDEO=0` set in
the environment, confirm the canvas allocates as ARGB8888 (log line from
the new defense-in-depth check at upload time stays silent — meaning the
guard never tripped because the canvas was 8888 as expected).

### 11. Telemetry

Add **two** counters to `SDLGameRenderer_PerfCaptureRefreshTelemetry` at
`include/port/sdl/sdl_game_renderer.h:210-245`, immediately after the item-1
fields at `:242-244`:

- `Uint64 rgb565_canvas_kernel_hits;`         — 565 kernel ran on a task
- `Uint64 argb8888_canvas_kernel_hits;`       — fallback ARGB kernel ran

Increments use the existing `RENDERER_TELEMETRY(...)` macro at
`sdl_game_renderer.c:44-53` (compile-out under non-telemetry release).

A third present-path counter, `convert_pass_skipped_frames`, lives in
`sdl_app.c` directly (P-2.5 — see §Step 7's "Counter ownership" note). The
present-path skipping decision is owned by `sdl_app.c`; routing it through
the renderer struct would require a new one-off public setter and header
churn for no architectural benefit.

JSON emission added in `sdl_app.c` next to the existing
`"colorkey_loose": { "hits": ..., "skipped": ..., "ineligible": ... }` block
at `sdl_app.c:6068-6073`. Same `(unsigned long long)` cast / `%llu` style.
The renderer-side counters come from
`SDLGameRenderer_GetPerfCaptureRefreshTelemetry()`; the
`convert_pass_skipped_frames` value is the local `sdl_app.c` static directly.

### 12. Correctness verification: extend item-1 parity test

Existing parity at `src/port/sdl/software_frame_parity.c`:

- `run_index8_fast_path_parity_check()` at `:495-619` — currently builds an
  ARGB8888 `expected` surface via `raster_reference_index8_loose()` at
  `:412-493`, runs the kernel via `SDLGameRenderer_RunIndex8FastPathParityCase`
  (shim in `sdl_game_renderer.c:12378-12411`), and compares ARGB8888 against
  ARGB8888 via `surfaces_match()` at `:183-206`.

**Decision: parameterize the harness, not duplicate.** The parity reference
is already an "expand INDEX8 + LUT directly" model. To validate 565, we add
a parameter to the case loop: `dst_format ∈ { ARGB8888, RGB565 }`. When
dst_format is 565:

1. Build LUT565 from the test palette directly (the same way item-1's
   `build_test_palette_lut` builds LUT8888). Add `build_test_palette_lut_565`.
2. Build the `expected` surface as 565 by writing
   `lut_565[index]` directly per pixel, with the loose-form `α==0?skip:store`
   shortcut. The `α` here is the original 8888 α — checked from the
   parallel LUT8888.
3. Build the `actual` surface as 565 by allocating an `SDL_PIXELFORMAT_RGB565`
   destination and feeding it through the kernel via the existing shim. The
   shim at `:12378-12411` accepts an `SDL_Surface* dst_surface`; it does not
   itself assert format. Verify the kernel respects `dst_surface->format`
   (this is the dispatch point in step 1 below).
4. **`surfaces_match()` is NOT format-agnostic in its current form** (P-1.1).
   At `software_frame_parity.c:183-206` it casts both surfaces to `Uint32*`
   and divides pitch by `sizeof(Uint32)`, so on a 565 384×224 surface it
   would read past row end every row. The fix is to rewrite `surfaces_match`
   to use a real per-row `SDL_memcmp(expected_row, actual_row,
   expected->w * SDL_BYTESPERPIXEL(expected->format))` with the byte width
   computed from the surface's `format` field, and per-row pointer advance
   driven by `expected->pitch` / `actual->pitch` directly. This is genuinely
   format-agnostic and works for any pixel size SDL produces.
5. **`raster_reference_index8_loose()` at `:412-493` has the same Uint32-cast
   bug** (P-2.3): `dst_pitch = dst_surface->pitch / (int)sizeof(Uint32)` at
   `:446`. The 565 branch must use `Uint16* dst_pixels` and
   `dst_pitch = dst_surface->pitch / (int)sizeof(Uint16)`. Plan Step 8
   explicitly extends this function with a format dispatch at the top:
   pick `Uint32*` + `/4` pitch for ARGB8888, `Uint16*` + `/2` pitch for
   565. Both branches use the same outer src walk; only the destination
   pointer type, pitch divisor, and store form differ. The store for 565
   uses `pack_rgb565_from_argb(lut[idx])` (with the same α-decide from
   LUT8888 driving the skip).

Add a `flavor`-style outer loop or an extra inner loop that runs each existing
case once per dst_format. This doubles the parity test count from 16 to 32;
runtime is still well under a second on host.

The runtime config key gate (`rgb565_canvas_enabled`) does NOT need to be
toggled for parity — the parity uses an explicit `dst_surface` argument, not
the production canvas, so format follows the test allocation. Parameterizing
on `dst_format` is the cleanest way to exercise both kernels in the same run.

## Format-aware dispatch

Each kernel call site picks 565 vs 8888. **Use a runtime check on
`software_frame_surface->format`.** Justification:

- A `dst_format` field on `RenderTask` (carried like `software_palette_lut`)
  is wrong: every task in a frame writes to the same canvas, so the field
  is per-frame, not per-task. Adding it to `RenderTask` would denormalize a
  global into every task slot for no read-side benefit (1024 redundant
  copies).
- An enum at the dispatch point inside `try_fast_copy_fast_textured_task_to_software_frame`
  is OK but the enum is recomputed on every task. Reading
  `software_frame_surface->format` once per task is no more expensive (same
  cache line, the surface is hot), and the format is the source of truth.
- A runtime check on `dst_surface->format` (the kernel already receives
  `dst_surface` as a parameter — see signature at
  `sdl_game_renderer.c:6128-6131`) is **the simplest and most correct
  option.** It also makes the parity shim work without changes — the shim
  passes the parity test's allocated `dst_surface`, and the kernel reads
  format off it.

Inside the kernel, the dispatch is one branch at the entry of each "INDEX8
non-color-mod" / "INDEX8 color-mod" / "ARGB exact" / "ARGB scaled" / "INDEX8
scaled" sub-block. The branch picks 565 inner loop vs 8888 inner loop. The
existing item-1 binary-α gate (`task->software_palette_is_binary_alpha`)
remains; it now controls whether the 565 path uses the loose-form shortcut
or has to do `blend_rgb565_opaque_dst`.

## Files to change (master list)

- `src/port/sdl/sdl_game_renderer.c` — canvas format, palette LUT565
  storage + build, format-aware dispatch in every kernel listed in §3,
  primitives (`blend_rgb565_opaque_dst`, `fill_rgb565_span`,
  `blend_solid_rgb565`, optional `pack_rgb565` / `expand_rgb565`
  helpers), sa_bg_cache surface format, scaled restore kernel 565
  variant, format assert relaxations at `:2899` and `:3233`, runtime
  gate static + setter, telemetry counter increments (2 counters), parity
  shim format passthrough, defense-in-depth format check at
  `upload_software_frame_to_canvas` entry. **No per-task LUT565 pointer
  added** (strategy (b) — derives from `task->texture_binding`).
- `src/port/sdl/software_frame_non_integer.c` — `blend_rgb565` and a 565
  rasterizer variant of
  `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888`. Either rename to
  `_RasterNonIntegerLookup` and dispatch on dst format inside, or add a sibling
  `_RasterNonIntegerLookupRGB565`. **Pick sibling** (avoids touching every
  call site; existing callers stay on the 8888 entry point and the renderer
  picks the new one when canvas is 565).
- `src/port/sdl/software_frame_non_integer.h` — declare new sibling.
- `src/port/sdl/sdl_app.c` — config key wiring (with NVW-availability
  gate), writer-availability call to `SDLGameRenderer_SetRGB565CanvasEnabled`,
  present-path bypass, FPS overlay 565 call,
  `convert_pass_skipped_frames_counter` static (P-2.5 — counter lives
  here, not in renderer struct).
- `src/port/sdl/fbdev_presenter.c` — 565 overlay variant.
- `src/port/sdl/fbdev_presenter.h` — declare new public.
- `include/port/sdl/sdl_game_renderer.h` — telemetry struct fields (2 new),
  public setter `SDLGameRenderer_SetRGB565CanvasEnabled`.
- `src/port/config/config.h` — `CFG_KEY_RGB565_CANVAS_ENABLED` define.
- `src/port/sdl/software_frame_parity.c` — rewrite `surfaces_match` to
  per-row `SDL_memcmp` (P-1.1 — replaces the broken Uint32-cast walk),
  `dst_format`-parameterized harness, LUT565 build, 565 reference raster
  with explicit `Uint16*` / `pitch / sizeof(Uint16)` math (P-2.3).

## Rollback plan

1. **Fastest (no rebuild):** add `rgb565-canvas-enabled = false` to the
   device's `config.ini`. Restart the game. The canvas reverts to ARGB8888
   and the existing convert pass at `sdl_app.c:10536-10539` runs. The 565
   kernels remain in the binary but are gated off by the dispatch.
2. **Slower but cleanest:** revert the commits from steps 5–8 (the format
   swap and dispatches). Steps 1–4 (LUT565, primitives, telemetry) are
   independently safe to keep — the LUT mirror is built once at palette
   unlock and is 0.56 MB cold, primitives stay unused, counters stay zero.
3. **Compile-time fallback:** flip the default of `rgb565_canvas_enabled`
   from `true` to `false` at `sdl_game_renderer.c` (one-line change).
   Rebuild + redeploy.

The runtime toggle is the primary rollback; default-flip is the safety net.

## Steps

8 steps total. Each step is one commit, executable independently by
`/implement` without follow-up clarification.

---

### Step 1 — Add palette LUT565 + binary-α-aware build pass + telemetry struct fields

**Title:** Build `software_palette_lut_565[][]` alongside `software_palette_lut[][]`,
add three `PerfCaptureRefreshTelemetry` fields.

**Why:** All 565 kernels that consume INDEX8 read the LUT565. Build it once
per palette unlock, in the same scan pass that already produces
`software_palette_lut_is_binary_alpha[]` (item-1 precedent at
`sdl_game_renderer.c:9311-9322`). Telemetry fields land here too so the
counters are addressable from later steps without a header churn.

**Files to read first:**
- `src/port/sdl/sdl_game_renderer.c:181-186` — existing
  `software_palette_lut` (decl `:181`), `software_palette_lut_valid`
  (decl `:182`), `software_palette_lut_is_binary_alpha` (decl `:185`)
  storage.
- `src/port/sdl/sdl_game_renderer.c:9290-9323` — `build_software_palette_lut`
  function that item 1 extended.
- `include/port/sdl/sdl_game_renderer.h:210-245` — `PerfCaptureRefreshTelemetry`
  struct (item-1 fields at `:242-244`).
- `src/port/sdl/sdl_app.c:6068-6073` — JSON emission template for new fields.

**Files to modify:**
- `src/port/sdl/sdl_game_renderer.c`:
  - Add storage at `:185` (right after `software_palette_lut_is_binary_alpha`):
    ```c
    /* Per-palette RGB565 LUT mirror for the 565 canvas mode (perf-2). 256
     * entries × 2 B/entry × FL_PALETTE_MAX = 0.56 MB. Built alongside the
     * ARGB LUT in build_software_palette_lut(); read by 565 kernels. */
    static Uint16 software_palette_lut_565[FL_PALETTE_MAX][256];
    ```
  - In `build_software_palette_lut()` at `:9294-9323`, between the existing
    ARGB build loop (`:9301-9304`) and the binary-α scan (`:9311-9322`),
    insert a 565 mirror build:
    ```c
    /* Mirror to 565 for the 565 canvas mode (perf-2). Reads the same
     * SDL_Color entries; α not encoded in 565 — α==0 is signalled via
     * software_palette_lut_is_binary_alpha[] + LUT8888[i] α byte. */
    Uint16* lut565 = software_palette_lut_565[palette_index];
    for (int i = 0; i < ncolors && i < 256; i++) {
        const SDL_Color* c = &palette->colors[i];
        lut565[i] = (Uint16)(((Uint32)(c->r >> 3) << 11) |
                             ((Uint32)(c->g >> 2) << 5)  |
                             ((Uint32)(c->b >> 3)));
    }
    for (int i = ncolors; i < 256; i++) { lut565[i] = 0; }
    ```
- `include/port/sdl/sdl_game_renderer.h`:
  - At `:244` (after `colorkey_loose_fast_path_ineligible;`), add the **two**
    new `Uint64` fields exactly as enumerated in §11 above
    (`rgb565_canvas_kernel_hits` and `argb8888_canvas_kernel_hits`). The
    third counter (`convert_pass_skipped_frames`) lives in `sdl_app.c` as
    a static — see §11 / Step 7 / P-2.5.

**Success criteria:**
- `tools/mister/build-game.sh --flavor telemetry --container 3s-mister-arm-build-perf`
  passes.
- `grep -n software_palette_lut_565 src/port/sdl/sdl_game_renderer.c` shows
  the storage decl and the build loop.
- `grep -n rgb565_canvas_kernel_hits include/port/sdl/sdl_game_renderer.h`
  finds the field.
- Render output unchanged (no consumer of LUT565 yet).

**Dependencies:** none.

**What NOT to do:**
- Do NOT add the runtime gate yet (Step 2 owns it).
- Do NOT touch `ensure_software_frame_surface` yet (Step 5 does).
- Do NOT scan the LUT in any hot path other than `build_software_palette_lut`.

**What to do if it fails:**
- If the build fails on the static-array size, double-check `FL_PALETTE_MAX`
  is `1088` (`include/sf33rd/AcrSDK/ps2/foundaps2.h:10`). The new table is
  556 KB, well within BSS — no failure expected.
- If the JSON emit later fails to compile, the telemetry struct field must be
  added before Step 7 (which references it).

---

### Step 2 — Add runtime gate + config key + setter

**Title:** `CFG_KEY_RGB565_CANVAS_ENABLED` config key, `rgb565_canvas_enabled`
static bool, public `SDLGameRenderer_SetRGB565CanvasEnabled` setter, startup
wiring.

**Why:** Mirrors item 1's `CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED` flow exactly
so A/B comparisons can flip the canvas format without rebuild. Default ON
under `PORT_MISTER`, OFF on desktop.

**Files to read first:**
- `src/port/config/config.h:23` — item-1 `CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED`
  define.
- `src/port/sdl/sdl_game_renderer.c:172-175` — item-1
  `colorkey_loose_kernel_enabled` static bool with comment block.
- `src/port/sdl/sdl_game_renderer.c:9382-9410` (or grep
  `SDLGameRenderer_SetColorkeyLooseKernelEnabled` for the def) — setter
  pattern.
- `src/port/sdl/sdl_app.c:9987-9990` — startup wiring for item-1 setter.
- `include/port/sdl/sdl_game_renderer.h:736` — setter declaration.

**Files to modify:**
- `src/port/config/config.h`: add `#define CFG_KEY_RGB565_CANVAS_ENABLED
  "rgb565-canvas-enabled"` immediately after the item-1 line at `:23`.
- `src/port/sdl/sdl_game_renderer.c`:
  - Add `static bool rgb565_canvas_enabled = false;` at `:175` (just after
    the item-1 colorkey toggle), with a paragraph comment. The default is
    **false everywhere**, including PORT_MISTER — the writer-availability
    guard in `sdl_app.c` (see below) is what flips it to true after
    `NativeVideoWriter_Init` succeeds. This is the P-1.2 mitigation: a
    PORT_MISTER build with `THIRDSARM_NATIVE_VIDEO=0` or a failed init
    therefore stays on ARGB8888 and the fbdev fallback is safe.
    ```c
    /* Runtime gate for the RGB565 software-frame canvas (perf-2 plan).
     * Default OFF unconditionally — flipped to true at startup by sdl_app.c
     * only after NativeVideoWriter_Init() returns true. The 565 canvas is
     * unsafe on the fbdev fallback path (software_frame_upload_texture
     * stays ARGB8888) so we only enable it when the native video writer
     * path is the only consumer. See plan §10. */
    static bool rgb565_canvas_enabled = false;
    ```
  - Define the public setter `SDLGameRenderer_SetRGB565CanvasEnabled(bool
    enabled)` immediately adjacent to
    `SDLGameRenderer_SetColorkeyLooseKernelEnabled` (find via grep). Two
    lines of body: `rgb565_canvas_enabled = enabled;`. **Do not** call any
    canvas re-creation from the setter — the gate is sticky once the canvas
    is allocated; flipping mid-run is undefined and not required.
  - Add a defense-in-depth format check at the top of
    `upload_software_frame_to_canvas()` at `:8753-8777`:
    ```c
    if (software_frame_surface->format != SDL_PIXELFORMAT_ARGB8888) {
        static bool warned = false;
        if (!warned) {
            backend_logf("upload_software_frame_to_canvas: canvas format 0x%x is "
                         "not ARGB8888; refusing to SDL_UpdateTexture into "
                         "ARGB8888 staging texture (would corrupt). "
                         "rgb565_canvas should not be enabled when the upload "
                         "path runs.", (unsigned)software_frame_surface->format);
            warned = true;
        }
        return false;
    }
    ```
    This catches any future regression where a 565 canvas reaches the
    upload path (which would silently corrupt) — not a normal-path branch,
    a guard that should never fire if §10's writer-availability guard
    holds.
- `src/port/sdl/sdl_app.c`:
  - At `:9858-9860` (where `native_video_writer_enabled = NativeVideoWriter_Init()`
    is called), **after** the boolean is set, add an explicit setter call
    when the writer came up successfully:
    ```c
    /* P-1.2 guard: only enable the 565 canvas if the native video writer
     * is actually available. The fbdev fallback path's
     * software_frame_upload_texture is ARGB8888-only and would silently
     * corrupt 565 frames. */
    if (native_video_writer_enabled) {
        SDLGameRenderer_SetRGB565CanvasEnabled(true);
    }
    ```
  - At `:9988-9990` (the item-1 setter call site for the `Config_HasExplicitKey`
    family), append a parallel block that lets the user **disable** but
    not enable when NVW is down:
    ```c
    if (Config_HasExplicitKey(CFG_KEY_RGB565_CANVAS_ENABLED)) {
        const bool requested = Config_GetBool(CFG_KEY_RGB565_CANVAS_ENABLED);
        if (!requested) {
            /* User can always disable. */
            SDLGameRenderer_SetRGB565CanvasEnabled(false);
        } else if (native_video_writer_enabled) {
            /* User can only enable if the writer is up. */
            SDLGameRenderer_SetRGB565CanvasEnabled(true);
        } else {
            backend_logf("rgb565-canvas-enabled=true ignored: native video "
                         "writer is not available; canvas stays ARGB8888.");
        }
    }
    ```
- `include/port/sdl/sdl_game_renderer.h`: add `void
  SDLGameRenderer_SetRGB565CanvasEnabled(bool enabled);` at `:736` (right
  after the item-1 setter declaration).

**Success criteria:**
- Telemetry build passes.
- `grep -n CFG_KEY_RGB565_CANVAS_ENABLED src/port/` returns hits in `config.h`
  and `sdl_app.c`.
- Setting `rgb565-canvas-enabled = false` in `config.ini` runs the setter
  with `false` (verified by adding a temporary `backend_logf` in the setter
  and checking the log line — remove the log before commit).
- On a PORT_MISTER telemetry build with default config and writer up,
  `rgb565_canvas_enabled` is true at canvas-creation time (visible via the
  Step-5 success criterion).
- On a PORT_MISTER telemetry build started with `THIRDSARM_NATIVE_VIDEO=0`,
  `rgb565_canvas_enabled` stays false; canvas allocates ARGB8888; fbdev
  fallback works as before (verifies P-1.2 mitigation).
- No canvas-format change yet *in this step* (Step 5 does the actual swap);
  render output identical.

**Dependencies:** Step 1 (uses no Step-1 symbols directly, but the telemetry
struct fields land in Step 1 to keep the header churn together).

**What NOT to do:**
- Do NOT change `ensure_software_frame_surface` yet — Step 5.
- Do NOT add the canvas-format dispatch in any kernel yet — Step 4 onward.
- Do NOT default `rgb565_canvas_enabled = true` under PORT_MISTER. The
  default must be false; only the writer-init success path flips it on.
  This is the P-1.2 mitigation invariant.

**What to do if it fails:**
- If `Config_HasExplicitKey` returns true unexpectedly under default
  `config.ini`, recheck the key name string matches the define exactly
  (no typos). The item-1 wiring works; the same path here should too.
- If the defense-in-depth log fires in `upload_software_frame_to_canvas`,
  the writer-availability guard upstream is broken — check that the
  `if (native_video_writer_enabled) SDLGameRenderer_SetRGB565CanvasEnabled(true)`
  block actually fires only when the writer is up.

---

### Step 3 — Add 565 primitives: pack/expand helpers, blend, fill, solid blend

**Title:** Define `pack_rgb565_from_argb`, `expand_rgb565_to_argb_channels`,
`blend_rgb565_opaque_dst`, `fill_rgb565_span`, `blend_solid_rgb565`, and
the non-integer-side `blend_rgb565`. No call sites yet; this step is pure
helper-library addition.

**Why:** Decouple primitive correctness from kernel structure. Step 4 onward
uses these helpers; if a primitive is wrong the parity test (Step 8) catches
it without churning every kernel site.

**Files to read first:**
- `src/port/sdl/sdl_game_renderer.c:6659-6673` — `modulate_argb8888` for
  channel-arithmetic style.
- `src/port/sdl/sdl_game_renderer.c:6757-6776` — `blend_argb8888_opaque_dst`
  to mirror for 565.
- `src/port/sdl/sdl_game_renderer.c:6778-6794` — `fill_argb8888_span`.
- `src/port/sdl/sdl_game_renderer.c:6795-6829` — `blend_solid_argb8888`.
- `src/port/sdl/software_frame_non_integer.c:68-103` — `blend_argb8888`
  generic.
- `src/port/sdl/sdl_app.c:186-226` — existing ARGB→565 pack used as the
  reference packing formula (`((R>>3)<<11) | ((G>>2)<<5) | (B>>3)`).

**Files to modify:**
- `src/port/sdl/sdl_game_renderer.c`: insert the new helpers immediately
  after the existing ARGB primitives (after `:6794` for `fill_argb8888_span`,
  before the next function). Helper signatures:
  ```c
  static inline Uint16 pack_rgb565_from_argb(Uint32 argb) {
      const Uint32 r = (argb >> 16) & 0xFFu;
      const Uint32 g = (argb >>  8) & 0xFFu;
      const Uint32 b =  argb        & 0xFFu;
      return (Uint16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  }

  static inline void expand_rgb565_to_argb_channels(Uint16 px,
                                                    Uint32* out_r,
                                                    Uint32* out_g,
                                                    Uint32* out_b) {
      const Uint32 r5 = (px >> 11) & 0x1Fu;
      const Uint32 g6 = (px >>  5) & 0x3Fu;
      const Uint32 b5 =  px        & 0x1Fu;
      *out_r = (r5 << 3) | (r5 >> 2);   /* 5→8 bits, replicate top */
      *out_g = (g6 << 2) | (g6 >> 4);   /* 6→8 */
      *out_b = (b5 << 3) | (b5 >> 2);   /* 5→8 */
  }

  static inline Uint16 blend_rgb565_opaque_dst(Uint16 dst_pixel,
                                               Uint32 src_argb) {
      const Uint32 src_a = (src_argb >> 24) & 0xFFu;
      if (src_a == 0u) { return dst_pixel; }
      if (src_a == 255u) { return pack_rgb565_from_argb(src_argb); }
      Uint32 dst_r, dst_g, dst_b;
      expand_rgb565_to_argb_channels(dst_pixel, &dst_r, &dst_g, &dst_b);
      const Uint32 src_r = (src_argb >> 16) & 0xFFu;
      const Uint32 src_g = (src_argb >>  8) & 0xFFu;
      const Uint32 src_b =  src_argb        & 0xFFu;
      const Uint32 inv = 255u - src_a;
      const Uint32 out_r = ((src_r * src_a) + (dst_r * inv) + 128u) >> 8;
      const Uint32 out_g = ((src_g * src_a) + (dst_g * inv) + 128u) >> 8;
      const Uint32 out_b = ((src_b * src_a) + (dst_b * inv) + 128u) >> 8;
      return (Uint16)(((out_r >> 3) << 11) | ((out_g >> 2) << 5) | (out_b >> 3));
  }

  static void fill_rgb565_span(Uint16* dst_pixels, int pixel_count, Uint16 color) {
      if ((dst_pixels == NULL) || (pixel_count <= 0)) { return; }
      int x = 0;
      for (; (x + 4) <= pixel_count; x += 4) {
          dst_pixels[x] = color;
          dst_pixels[x + 1] = color;
          dst_pixels[x + 2] = color;
          dst_pixels[x + 3] = color;
      }
      for (; x < pixel_count; x++) { dst_pixels[x] = color; }
  }

  static Uint16 blend_solid_rgb565(Uint16 dst_pixel,
                                   Uint32 src_a,
                                   Uint32 inv_src_a,
                                   Uint32 src_r_premul,
                                   Uint32 src_g_premul,
                                   Uint32 src_b_premul) {
      Uint32 dst_r, dst_g, dst_b;
      expand_rgb565_to_argb_channels(dst_pixel, &dst_r, &dst_g, &dst_b);
      const Uint32 out_r = (src_r_premul + (dst_r * inv_src_a) + 128u) >> 8;
      const Uint32 out_g = (src_g_premul + (dst_g * inv_src_a) + 128u) >> 8;
      const Uint32 out_b = (src_b_premul + (dst_b * inv_src_a) + 128u) >> 8;
      (void)src_a; /* solid path tracks dst α as opaque always — see note */
      return (Uint16)(((out_r >> 3) << 11) | ((out_g >> 2) << 5) | (out_b >> 3));
  }
  ```

  Note: 565 has no α channel; `blend_solid_rgb565` mirrors the
  `blend_solid_argb8888` `dst_a==255` fast path (the only branch fired in
  practice on the canvas, which is always cleared opaque).

- `src/port/sdl/software_frame_non_integer.c`: add a sibling
  `static Uint16 blend_rgb565(Uint16 dst_pixel, Uint32 src_argb)` at the
  bottom of the existing blend block (after `:103`), with the same shape
  as the renderer's `blend_rgb565_opaque_dst` plus a generic-α handling
  branch (mirror the existing `blend_argb8888` shape exactly).

  This non-integer path's `blend_argb8888` at `:68-103` is a *generic-α*
  function (handles dst_a < 255), unlike the renderer's
  `blend_argb8888_opaque_dst`. The 565 canvas is always opaque-dst (no α
  channel exists in the destination), so the 565 sibling collapses to the
  `dst_a == 255` branch. Document this in the function comment.

**Success criteria:**
- Telemetry build passes.
- `grep -n blend_rgb565 src/port/sdl/` returns hits in both files.
- Each helper compiles with `-Wunused-function` even before any caller; mark
  with `__attribute__((unused))` if needed. Remove the unused attr in
  Step 4 when the first call site lands.

**Dependencies:** none (additive only).

**What NOT to do:**
- Do NOT add NEON kernels here; they're a separate optimization. (Item 1's
  precedent: scalar first, NEON in a follow-up step.)
- Do NOT alter the existing 8888 primitives.

**What to do if it fails:**
- If `pack_rgb565_from_argb` produces wrong output, sanity-check the formula
  against the existing `convert_argb8888_to_rgb565` scalar tail at
  `sdl_app.c:210-216`. The plan formula should match exactly.

---

### Step 4 — Format dispatch: INDEX8 fast-copy 565 kernels (loose-form + color-mod)

**Title:** Add 565-canvas branches to `try_fast_copy_fast_textured_task_to_software_frame`
covering INDEX8 exact-copy and INDEX8 scaled-copy paths. Dispatch via
`dst_surface->format == SDL_PIXELFORMAT_RGB565`.

**Why:** This is the highest-traffic kernel — most PS2 sprites land here.
Item 1 already added loose-form 8888 kernels; the 565 versions read from
LUT565 (built in Step 1) and write `Uint16`. Color-mod path uses the
modulate-from-LUT-8888-then-pack-565 strategy from §6.

**Files to read first:**
- `src/port/sdl/sdl_game_renderer.c:6128-6431` — entire INDEX8 block of
  `try_fast_copy_fast_textured_task_to_software_frame`. Read every branch:
  - 6160-6251: INDEX8 color-mod (NEON + scalar).
  - 6260-6311: INDEX8 non-color-mod loose-form (NEON + scalar) — item-1.
  - 6312-6378: INDEX8 non-color-mod fall-through (existing
    α==0/0xFF/blend triplet, NEON + scalar).
  - 6381-6430: INDEX8 scaled (loose-form + fall-through).
- The sub-path returns `true` after each kernel runs; we add a 565 branch
  in front of each existing 8888 branch.
- `src/port/sdl/sdl_game_renderer.c:6148-6149` — establishes
  `dst_pixels = (Uint32*)dst_surface->pixels` and `dst_pitch =
  dst_surface->pitch / sizeof(Uint32)`. The 565 branches must compute
  `Uint16* dst_pixels_16 = (Uint16*)dst_surface->pixels` and
  `dst_pitch_16 = dst_surface->pitch / sizeof(Uint16)`.

**Files to modify:**
- `src/port/sdl/sdl_game_renderer.c`:
  - Inside the existing
    `if (task->software_source_is_index8 && (task->software_palette_lut != NULL) ...)`
    block at `:6144`, *before* establishing `dst_pixels = (Uint32*)`,
    capture the format:
    ```c
    const bool dst_is_565 = (dst_surface->format == SDL_PIXELFORMAT_RGB565);
    ```
  - Replace the existing `Uint32* dst_pixels = ...` with a conditional:
    branches that write `Uint32` keep that cast; new 565 branches use
    `Uint16*`. The cleanest factoring is: refactor each existing kernel
    branch to wrap with an `if (dst_is_565) { /* 565 kernel */ }
    else { /* existing 8888 kernel unchanged */ }`. Keeps existing 8888
    code byte-identical (smaller diff, simpler review).
  - For each of the four INDEX8 sub-blocks (color-mod NEON, color-mod
    scalar, non-color-mod loose-form NEON, non-color-mod loose-form scalar,
    non-color-mod fall-through NEON, non-color-mod fall-through scalar,
    scaled loose-form, scaled fall-through), add a 565 sibling. The 565
    siblings mirror the existing inner-loop structure but:
    - `Uint16* dst_row = dst_pixels_16 + ((dst_y0 + row) * dst_pitch_16) + dst_x0;`
    - LUT lookup: keep `palette_lut[i8_row[x]]` (LUT8888) — do not switch
      the source-of-truth for color or modulation.
    - For modulation (color-mod sub-blocks): apply
      `modulate_argb8888(...)` on the LUT8888 result, then
      `dst_row[col] = pack_rgb565_from_argb(src_pixel)` if α==0xFF, else
      `dst_row[col] = blend_rgb565_opaque_dst(dst_row[col], src_pixel)`,
      else skip.
    - For non-color-mod loose-form (item-1 fast path): use LUT565 directly
      via in-kernel handle-based lookup. **Pick strategy (b) — derive
      LUT565 from `task->texture_binding`.** Inside the kernel:
      ```c
      const unsigned int palette_handle = HI_16_BITS(task->texture_binding);
      const Uint16* lut565 = (palette_handle > 0u && palette_handle <= FL_PALETTE_MAX)
                             ? software_palette_lut_565[palette_handle - 1u]
                             : NULL;
      ```
      Justification for the policy split with item-1 (which threads
      `task->software_palette_is_binary_alpha` as a per-task bool): the
      binary-α flag is *not* cheaply derivable in-kernel — it would
      require a second array probe (`software_palette_lut_is_binary_alpha[handle-1]`)
      whose lifecycle (set/cleared on palette load/unlock) is independent
      from `software_palette_lut[]`'s validity. Caching it per-task
      consolidates a write-side decision that's already being made when
      the task is built. The LUT565 pointer in contrast is **derivable
      from data the task already carries** (`texture_binding` is the
      packed `(texture_handle, palette_handle)` word — see `:1812-1813`,
      `:11815-11816`). Re-derivation is one shift+mask+sub+array-index;
      no second lifecycle to track. Strategy (b) avoids the 8 KB of
      redundant denormalized state across 1024 task slots and the 11
      write-site updates that strategy (a) would require (which is also
      where the reviewer caught the missing-NULL-out at `:11641` —
      strategy (b) makes that whole class of bug impossible).
    - The kernel must still gate on the existing LUT8888 pointer
      (`task->software_palette_lut != NULL`) and the new LUT8888 validity
      check applies symmetrically to LUT565: both LUTs are populated
      in the same `build_software_palette_lut` pass (Step 1) under the
      same `software_palette_lut_valid[]` flag, so a non-NULL
      `task->software_palette_lut` implies a valid LUT565 at the same
      index. No new validity flag is needed.
  - Telemetry: add `RENDERER_TELEMETRY(perf_capture_refresh_telemetry.rgb565_canvas_kernel_hits++);`
    at the top of each new 565 branch; mirror
    `argb8888_canvas_kernel_hits++` at the top of each preserved 8888
    branch. Place the increments **inside** the path-picking branch, not at
    the function entry, so disabled-canvas frames don't double-count.
  - **No per-task LUT565 pointer added** (strategy (b) — see above). No
    `RenderTask` field, no global `current_software_palette_lut_565`, no
    write-site fan-out. This avoids the 11 sites that would have needed
    parallel updates (the eight the original plan listed plus the three
    the reviewer caught: texture-unlock NULL-out at `:11641`, the actual
    input-history glyph NULL-out at `:12336-12338`, and the parity-shim
    setter at `:12395`).

**Success criteria:**
- Telemetry build passes.
- `grep -n software_palette_lut_565 src/port/sdl/sdl_game_renderer.c` shows
  the storage decl (Step 1), the build loop (Step 1), and the in-kernel
  reads added here. **No** per-task field, no global mirror, no per-site
  bind. (If grep finds a `task->software_palette_lut_565` anywhere,
  strategy (a) leaked in — back it out.)
- Render output **on the desktop build** (where canvas is still 8888) is
  unchanged byte-for-byte.
- Render output **on a MiSTer telemetry build** with the gate enabled (after
  Step 5) shows the 565 canvas; this step alone leaves the gate off so
  output is still 8888-matching even with the new branches present.

**Dependencies:** Steps 1, 3.

**What NOT to do:**
- Do NOT swap the canvas format yet — Step 5.
- Do NOT touch the parallelogram / triangle / non-integer / sa_bg paths —
  Steps 6–7.
- Do NOT add NEON 565 kernels yet. Scalar-first; NEON 565 is a follow-up
  step, justified by measurement.
- Do NOT add a per-task `software_palette_lut_565` field — strategy (b)
  derives it in-kernel from `task->texture_binding`. See rationale above.

**What to do if it fails:**
- If parity fails on a color-mod case, the bug is most likely in the
  pack-after-modulate step, not the modulate. Diff the 565 inner loop
  against the existing 8888 inner loop line-by-line. They should differ
  only in the store form.
- If the kernel reads NULL from `software_palette_lut_565[handle-1]`,
  the LUT565 build pass in Step 1 didn't run for that palette index — check
  `software_palette_lut_valid[handle-1]` and the build-loop ordering.

---

### Step 5 — Swap canvas format + sa_bg cache format + scaled-restore 565 variant

**Title:** `software_frame_surface` becomes `SDL_PIXELFORMAT_RGB565` when
`rgb565_canvas_enabled` is true; `sa_bg_cache_surface` follows; add a 565
variant of `sa_bg_cache_restore_background_scaled`.

**Why:** This is the moment the bandwidth saving kicks in. Before this
step, the new 565 branches in Step 4 sit dormant (canvas is still 8888 so
the dispatch picks the 8888 path).

**Files to read first:**
- `src/port/sdl/sdl_game_renderer.c:3382-3389` — `ensure_software_frame_surface`.
- `src/port/sdl/sdl_game_renderer.c:3408-3415` — `ensure_sa_bg_cache_surface`.
- `src/port/sdl/sdl_game_renderer.c:3630-3663` —
  `sa_bg_cache_restore_background_scaled`.
- `src/port/sdl/sdl_game_renderer.c:2899` — assert
  `surface->format != SDL_PIXELFORMAT_ARGB8888` in
  `SDLGameRenderer_LockSoftwareSourceSurface`. **Verify** this assert is
  about the source surface, not the canvas. (It is — the function takes
  a texture surface.)
- `src/port/sdl/sdl_game_renderer.c:3233` — assert
  `cached_surface->format != SDL_PIXELFORMAT_ARGB8888` in
  `refresh_texture_cache`. Same — the cache surface, not the canvas.
- The two asserts above gate **the texture refresh cache** (which stays
  ARGB8888 because the SDL_Texture upload path has not changed). No
  modification needed; verify in step success criteria.
- `src/port/sdl/sdl_game_renderer.c:8736-8738` — sa_bg_cache snapshot
  `SDL_memcpy` (format-agnostic, byte-copy).
- `src/port/sdl/sdl_game_renderer.c:3729-3735` — sa_bg_cache restore branch
  (memcpy fast path + scaled-blit slow path).

**Files to modify:**
- `src/port/sdl/sdl_game_renderer.c`:
  - In `ensure_software_frame_surface()` at `:3382-3389`:
    ```c
    static bool ensure_software_frame_surface(void) {
        if (software_frame_surface != NULL) { return true; }
        const Uint32 format = rgb565_canvas_enabled ? SDL_PIXELFORMAT_RGB565
                                                    : SDL_PIXELFORMAT_ARGB8888;
        software_frame_surface = SDL_CreateSurface(cps3_width, cps3_height, format);
        return software_frame_surface != NULL;
    }
    ```
  - In `ensure_sa_bg_cache_surface()` at `:3408-3415`: same pattern, same
    format choice — `sa_bg_cache_surface` always matches the canvas:
    ```c
    const Uint32 format = (software_frame_surface != NULL)
                          ? software_frame_surface->format
                          : (rgb565_canvas_enabled ? SDL_PIXELFORMAT_RGB565
                                                    : SDL_PIXELFORMAT_ARGB8888);
    sa_bg_cache_surface = SDL_CreateSurface(cps3_width, cps3_height, format);
    ```
    Reading the format from `software_frame_surface` is preferred because
    if the canvas surface is allocated first (which it always is in the
    real flow) the cache cannot drift.
  - Add a 565 sibling
    `sa_bg_cache_restore_background_scaled_565` immediately after the
    existing `:3630-3663` function. Same fixed-point math, but reads
    `Uint16*` and writes `Uint16*`. Each pixel is one
    `dst_row[x] = src_row[src_x]` (no blend, just copy — 565 is opaque).
  - In the existing scroll-aware block at `:3724-3736`, dispatch on
    `software_frame_surface->format`:
    ```c
    if (needs_transform) {
        if (software_frame_surface->format == SDL_PIXELFORMAT_RGB565) {
            sa_bg_cache_restore_background_scaled_565(...);
        } else {
            sa_bg_cache_restore_background_scaled(...);
        }
    } else {
        SDL_memcpy(software_frame_surface->pixels, sa_bg_cache_surface->pixels,
                   (size_t)software_frame_surface->pitch * software_frame_surface->h);
    }
    ```
    The `SDL_memcpy` branch is byte-count-driven so it's already
    format-agnostic.

**Success criteria:**
- Telemetry build passes.
- On a deployed MiSTer telemetry build with default config (no key set,
  writer up), `rgb565_canvas_enabled = true` after `NativeVideoWriter_Init`,
  the canvas is 565, and the new 565 kernels from Step 4 fire
  (`rgb565_canvas_kernel_hits > 0` in JSON).
- Setting `rgb565-canvas-enabled = false` in `config.ini` reverts to the
  ARGB8888 canvas; gameplay matches the pre-perf-2 build.
- **P-1.2 verification.** Deploy with `THIRDSARM_NATIVE_VIDEO=0` set in
  the launcher environment. Confirm `native_video_writer_enabled = false`
  in the boot log, the writer-availability guard does NOT call
  `SDLGameRenderer_SetRGB565CanvasEnabled(true)`, the canvas allocates
  ARGB8888, the fbdev fallback present path runs cleanly (FBDevPresenter
  log lines visible). The defense-in-depth check at the top of
  `upload_software_frame_to_canvas` stays silent (canvas is 8888 as
  expected; the guard never fires).
- The two ARGB8888 asserts at `:2899` and `:3233` did NOT need changes;
  verify by inspection that they target source / refresh-cache surfaces.

**Dependencies:** Steps 2, 4.

**What NOT to do:**
- Do NOT remove the 8888 path code; it must remain reachable when the gate
  is off.
- Do NOT change the SDL_Texture format anywhere
  (`software_frame_upload_texture` at `:3396-3404`, `cps3_canvas` at
  `:9362-9365` block). They are independent of canvas format and are not
  touched on the MiSTer present path.

**What to do if it fails:**
- If sa_bg_cache restore corrupts the canvas, the bug is in the 565 scaled
  restore — diff against the 8888 version. The pixel-step math is
  identical; only the pixel size changes. The fixed-point setup at
  `:3639-3641` is format-independent.
- If `ensure_sa_bg_cache_surface` is called before
  `ensure_software_frame_surface`, the format read from the canvas is
  invalid. Check call order; the canvas is created via
  `SDLGameRenderer_SetSoftwareFrameMode` which fires before any
  `sa_bg_cache_*` codepath.

---

### Step 6 — 565 variants of parallelogram / float-parallelogram / triangles / solid-fill

**Title:** Add format-aware dispatch + 565 inner loops to all remaining
canvas-write paths in `sdl_game_renderer.c`.

**Why:** The fast-copy path covers the AABB hot case but stage-shear,
affine quads, sa3 effects can fall to parallelogram / float-parallelogram
/ triangle. They must respect canvas format; a missing 565 dispatch
silently corrupts pixels.

**Files to read first:**
- `src/port/sdl/sdl_game_renderer.c:7850-7962` —
  `raster_textured_parallelogram_to_software_frame` (INDEX8 branch at
  `:7875-7906`, ARGB branch at `:7909-7951`).
- `src/port/sdl/sdl_game_renderer.c:7964-8055` —
  `raster_textured_float_parallelogram_to_software_frame` (INDEX8 + ARGB
  in same function).
- `src/port/sdl/sdl_game_renderer.c:8057-8131` —
  `raster_textured_float_triangle_to_software_frame` (handles INDEX8 via
  `palette_lut` arg).
- `src/port/sdl/sdl_game_renderer.c:8133-8209` —
  `raster_textured_triangle_to_software_frame` (same shape).
- `src/port/sdl/sdl_game_renderer.c:8211-8299` —
  `raster_textured_translated_triangle_to_software_frame`.
- `src/port/sdl/sdl_game_renderer.c:8418-8504` —
  `raster_solid_triangle_to_software_frame` (uses `fill_argb8888_span` and
  inline `blend_solid_argb8888`).
- `src/port/sdl/sdl_game_renderer.c:8506-8588` —
  `raster_solid_task_to_software_frame` (axis-aligned rect + diagonal-strip
  fast paths).
- `src/port/sdl/sdl_game_renderer.c:6830-6885` —
  `raster_full_height_diagonal_strip_to_software_frame` (uses
  `fill_argb8888_span` and `blend_solid_argb8888`). Confirm by grep —
  exact lines in current file.

**Files to modify:**
- `src/port/sdl/sdl_game_renderer.c`: in each of the seven functions above,
  at the point each picks `dst_pixels = (Uint32*)software_frame_surface->pixels`,
  replace with a format dispatch:
  ```c
  const bool dst_is_565 = (software_frame_surface->format == SDL_PIXELFORMAT_RGB565);
  if (dst_is_565) {
      Uint16* dst_pixels = (Uint16*)software_frame_surface->pixels;
      const int dst_pitch = software_frame_surface->pitch / (int)sizeof(Uint16);
      /* … 565 variant of the kernel … */
      RENDERER_TELEMETRY(perf_capture_refresh_telemetry.rgb565_canvas_kernel_hits++);
  } else {
      Uint32* dst_pixels = (Uint32*)software_frame_surface->pixels;
      const int dst_pitch = software_frame_surface->pitch / (int)sizeof(Uint32);
      /* … existing 8888 kernel — unchanged … */
      RENDERER_TELEMETRY(perf_capture_refresh_telemetry.argb8888_canvas_kernel_hits++);
  }
  ```
  In the 565 branch, `palette_lut[i]` reads stay 8888 (LUT8888); the inner
  store is `dst_row[col] = pack_rgb565_from_argb(src_pixel)` for the α==0xFF
  fast path and `dst_row[col] = blend_rgb565_opaque_dst(dst_row[col],
  src_pixel)` for partial-α. The `α==0?skip` shortcut stays.
- `fill_argb8888_span` calls in solid paths become `fill_rgb565_span(...)`
  with `pack_rgb565_from_argb(color)` as the second argument — dispatch
  same pattern.

**Success criteria:**
- Telemetry build passes.
- Stage-shear backgrounds (Genei-Jin, Yun) render correctly under 565
  canvas. (Manual visual check on device — not a build gate.)
- `rgb565_canvas_kernel_hits` increments under stage-heavy gameplay.
- 8888 kernel byte-identity preserved for the desktop build.

**Dependencies:** Steps 3, 4, 5.

**What NOT to do:**
- Do NOT inline the 565 channel arithmetic in each function — call
  `pack_rgb565_from_argb` and `blend_rgb565_opaque_dst`. The compiler will
  inline them; the source stays single-source-of-truth.
- Do NOT touch `software_frame_non_integer.c` — that's Step 7.

**What to do if it fails:**
- If a triangle case shows visible artifacts but parallelogram is fine,
  read the barycentric inner loop diff line-by-line against the 8888
  version. Most likely cause: forgetting to expand 565 dst before blend.
- If the diagonal-strip path is corrupt, double-check `dst_pitch / 2` vs
  `dst_pitch / 4` (565 is `Uint16`).

---

### Step 7 — 565 variant of non-integer raster + 565 FPS overlay + present path bypass

**Title:** Add `SDLSoftwareFrame_RasterNonIntegerLookupRGB565`, the 565 FPS
overlay path in `fbdev_presenter.c`, and switch the present path to feed
`software_frame_surface->pixels` directly to `NativeVideoWriter_WriteFrame`.

**Why:** Closes the remaining canvas-write surface (non-integer lookup) and
the present-path glue. After this step the pre-frame
`convert_argb8888_to_rgb565` pass disappears for 565 canvases.

**Files to read first:**
- `src/port/sdl/software_frame_non_integer.c:452-716` —
  `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888`.
- `src/port/sdl/software_frame_non_integer.h` — public function decls.
- `src/port/sdl/sdl_app.c:184-226` — convert function + scratch.
- `src/port/sdl/sdl_app.c:10525-10551` — present-path call site.
- `src/port/sdl/fbdev_presenter.c:1169-1186` —
  `apply_rasterized_fps_overlay_to_argb_buffer`.
- `src/port/sdl/fbdev_presenter.c:2538-2589` —
  `FBDevPresenter_ApplyFPSOverlayToBuffer`.
- `src/port/sdl/fbdev_presenter.h:104` — public decl.
- `src/port/sdl/native_video_writer.c:76-107` — confirm pitch-aware behavior.

**Files to modify:**
- `src/port/sdl/software_frame_non_integer.c`: copy
  `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888` to a new
  `SDLSoftwareFrame_RasterNonIntegerLookupRGB565` immediately after. Same
  signature. Inside:
  - `Uint16* dst_pixels = (Uint16*)dst_surface->pixels;`
  - `const int dst_pitch = dst_surface->pitch / (int)sizeof(Uint16);`
  - Inner-loop stores use `pack_rgb565_from_argb` (re-exported from
    `sdl_game_renderer.c` via a new tiny inline in a shared header — or
    just duplicated inline; mirror the existing 8888 sibling style. Pick:
    duplicate the inline. The pack helper is 4 lines.)
  - Use `blend_rgb565` (new in Step 3) for partial-α.
- `src/port/sdl/software_frame_non_integer.h`: declare
  `SDLSoftwareFrame_RasterNonIntegerLookupRGB565` with the same signature
  as the 8888 sibling.
- `src/port/sdl/sdl_game_renderer.c`: at the call site for
  `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888` (use grep —
  `raster_textured_task_to_software_frame` calls it; find via grep
  `RasterNonIntegerLookupARGB8888\|RasterNonIntegerLookup`), dispatch on
  `dst_surface->format` to call the 565 sibling when 565.
- `src/port/sdl/fbdev_presenter.c`:
  - Add `apply_rasterized_fps_overlay_to_rgb565_buffer(Uint16* pixels, int
    width, int height, int stride_pixels)` immediately after
    `apply_rasterized_fps_overlay_to_argb_buffer` at `:1186`. Body: same
    as 8888 but `row_bytes = layout->width * sizeof(Uint16)`; for each row
    convert the ARGB8888 atlas row to 565 inline (8 px at a time scalar is
    fine — overlay is small) into the 565 dst. Or use
    `pack_rgb565_from_argb` per pixel (overlay rasterization is not hot).
  - Add public `FBDevPresenter_ApplyFPSOverlayToRGB565Buffer(Uint16*
    pixels, int width, int height)` at `:2589` — clone of the 8888 entry,
    swap the inner call to the 565 helper.
  - Mirror the `#else` stub at `:2665+` for non-MiSTer hosts.
- `src/port/sdl/fbdev_presenter.h`: declare
  `FBDevPresenter_ApplyFPSOverlayToRGB565Buffer` next to the 8888 one at
  `:104`.
- `src/port/sdl/sdl_app.c`:
  - **Counter ownership (P-2.5).** The `convert_pass_skipped_frames`
    counter is a present-path decision made entirely in `sdl_app.c`. Per
    reviewer's analysis (`sdl_app.c:2800-2801` only reads the renderer's
    telemetry struct via getters; there is no existing precedent for
    `sdl_app.c` writing into the renderer's static counters), keep this
    counter in `sdl_app.c` next to the present-path block — no new public
    setter on the renderer, no header churn.
    - Add `static Uint64 convert_pass_skipped_frames_counter = 0;` near
      the top of `sdl_app.c` next to other present-path counters.
    - Increment it inline in the 565 branch.
    - Emit it directly from `sdl_app.c`'s perf-capture JSON block
      (Step 8) instead of going through the renderer's telemetry struct.
    - Step 1's plan added `convert_pass_skipped_frames` to
      `PerfCaptureRefreshTelemetry`; **drop that field** (revise Step 1).
      Only `rgb565_canvas_kernel_hits` and `argb8888_canvas_kernel_hits`
      stay in the renderer struct (those are kernel-level decisions made
      in `sdl_game_renderer.c`).
  - At `:10525-10551`, the present-path block, replace the unconditional
    convert call with a format dispatch:
    ```c
    if (native_video_writer_enabled && SDLGameRenderer_HasSoftwareOwnedFrame()) {
        const SDL_Surface* frame = SDLGameRenderer_GetSoftwareFrameSurface();
        if (frame && frame->pixels && frame->w == 384 && frame->h == 224) {
            if (frame->format == SDL_PIXELFORMAT_RGB565) {
                if (fps_overlay_mode != FPS_OVERLAY_OFF) {
                    FBDevPresenter_ApplyFPSOverlayToRGB565Buffer(
                        (Uint16*)frame->pixels, frame->w, frame->h);
                }
                NativeVideoWriter_WriteFrame(
                    (const uint16_t*)frame->pixels, 384, 224, frame->pitch);
                convert_pass_skipped_frames_counter++;
            } else {
                if (fps_overlay_mode != FPS_OVERLAY_OFF) {
                    FBDevPresenter_ApplyFPSOverlayToBuffer(
                        (Uint32*)frame->pixels, frame->w, frame->h);
                }
                convert_argb8888_to_rgb565((const uint32_t*)frame->pixels,
                                           native_video_rgb565_scratch, 384 * 224);
                NativeVideoWriter_WriteFrame(native_video_rgb565_scratch,
                                             384, 224, 384 * 2);
            }
        } else { /* existing nv_warned block, unchanged */ }
    }
    ```
    No `SDL_assert(frame->pitch == 384 * 2)` (P-2.1 — the writer
    handles the unexpected case correctly).
  - Mark `convert_argb8888_to_rgb565` and `native_video_rgb565_scratch`
    `__attribute__((unused))` if compile warns when 565 is the runtime
    default (these are still reachable in the 8888 fall-through above so
    the attribute may not be needed; verify and add only if warning fires).

**Success criteria:**
- Telemetry build passes.
- With 565 canvas active, perf-capture JSON reports
  `convert_pass_skipped_frames > 0` on every frame the writer fires.
- With 565 disabled at runtime, the convert pass still runs and the binary
  is identical to a pre-perf-2 build modulo the dispatch branches.
- Visual on-device smoke: FPS overlay renders correctly when 565 is on.

**Dependencies:** Steps 3, 5.

**What NOT to do:**
- Do NOT delete the convert function or scratch buffer in this step. They
  remain reachable in the 8888 fallback. Cleanup is out of scope (orchestrator
  brief explicit).
- Do NOT change `NativeVideoWriter_WriteFrame` signature or behavior. Its
  pitch-aware logic at `native_video_writer.c:76-107` already handles
  `pitch == 384*2` correctly.

**What to do if it fails:**
- If the assert `frame->pitch == 384 * 2` ever fires, SDL has padded the
  surface. Fall back to row-by-row inside `NativeVideoWriter_WriteFrame`
  (the writer already supports this path). Remove the assert and let the
  writer pick.
- If FPS overlay shows incorrect colors, the inline ARGB→565 in the
  overlay function is wrong. Cross-check against the existing
  `convert_argb8888_to_rgb565` formula at `sdl_app.c:210-216`.

---

### Step 8 — Extend parity test for 565 dst + telemetry JSON emission

**Title:** Parameterize `run_index8_fast_path_parity_check` on
`dst_format`. Emit the three new perf-2 telemetry fields in
`sdl_app.c`'s perf-capture JSON.

**Why:** Without parity coverage, 565 kernel correctness is purely smoke
work. The harness already runs both strict-α and loose-α palette flavors;
extending to (`dst_format` × `palette_flavor` × `case`) is a 2× expansion
of the existing matrix.

**Files to read first:**
- `src/port/sdl/software_frame_parity.c:412-619` — existing parity for
  INDEX8 (loose-form 8888).
- `src/port/sdl/software_frame_parity.c:264-273` —
  `build_test_palette_lut`.
- `src/port/sdl/software_frame_parity.c:183-206` — `surfaces_match`. **NOT
  format-agnostic in current form** (P-1.1) — casts to `Uint32*` and
  divides pitch by `sizeof(Uint32)`. Must be rewritten; see "Files to
  modify" below.
- `src/port/sdl/sdl_app.c:6068-6073` — JSON template for item-1 telemetry,
  pattern to mirror.
- `src/port/sdl/sdl_game_renderer.c:12378-12411` — parity shim
  `SDLGameRenderer_RunIndex8FastPathParityCase`. The shim's last argument
  is `SDL_Surface* dst_surface`; format follows the test allocation. No
  shim change needed.

**Files to modify:**
- `src/port/sdl/software_frame_parity.c`:
  - **Rewrite `surfaces_match` to be genuinely format-agnostic** (P-1.1).
    Replace the `Uint32`-typed pixel walk with per-row `SDL_memcmp`:
    ```c
    static bool surfaces_match(const char* case_name,
                               const SDL_Surface* expected,
                               const SDL_Surface* actual) {
        if (expected->format != actual->format ||
            expected->w != actual->w ||
            expected->h != actual->h) {
            SDL_Log("Software-frame parity surface mismatch in %s: "
                    "format/size differ (expected %dx%d fmt 0x%x, "
                    "actual %dx%d fmt 0x%x)",
                    case_name, expected->w, expected->h,
                    (unsigned)expected->format, actual->w, actual->h,
                    (unsigned)actual->format);
            return false;
        }
        const int bpp = (int)SDL_BYTESPERPIXEL(expected->format);
        const size_t row_bytes = (size_t)expected->w * (size_t)bpp;
        const Uint8* exp_row = (const Uint8*)expected->pixels;
        const Uint8* act_row = (const Uint8*)actual->pixels;
        for (int y = 0; y < expected->h; y++) {
            if (SDL_memcmp(exp_row, act_row, row_bytes) != 0) {
                /* find the first diverging pixel for the log */
                for (int x = 0; x < expected->w; x++) {
                    if (SDL_memcmp(exp_row + x*bpp, act_row + x*bpp, bpp) != 0) {
                        SDL_Log("Software-frame parity mismatch in %s at (%d,%d)",
                                case_name, x, y);
                        return false;
                    }
                }
            }
            exp_row += expected->pitch;
            act_row += actual->pitch;
        }
        return true;
    }
    ```
    This compares only the visible `w * bpp` bytes per row, advancing by
    `pitch` between rows — correct for any pixel size SDL produces.
  - Add `static void build_test_palette_lut_565(const SDL_Palette* palette,
    Uint16* out_lut)` mirroring the existing `build_test_palette_lut` at
    `:264`.
  - In `raster_reference_index8_loose` at `:412-493`, extend with a format
    dispatch on `dst_surface->format` (P-2.3). The existing 8888 branch
    keeps `Uint32* dst_pixels` and `dst_pitch = pitch / sizeof(Uint32)`.
    Add a parallel 565 branch with `Uint16* dst_pixels = (Uint16*)
    dst_surface->pixels` and **`dst_pitch = dst_surface->pitch /
    (int)sizeof(Uint16)`**. The α-decide stays identical (read α from
    LUT8888 byte); the store form is `pack_rgb565_from_argb(lut8888[idx])`
    instead of `lut8888[idx]`. Duplicate the inline `pack_rgb565_from_argb`
    at the top of the parity TU (same 4 lines as the renderer's helper) so
    parity does not link to renderer internals.
  - In `run_index8_fast_path_parity_check` at `:495-619`, add an outer
    `dst_format` loop:
    ```c
    typedef struct DstFormatFlavor {
        const char* label;
        SDL_PixelFormat format;
    } DstFormatFlavor;
    static const DstFormatFlavor dst_formats[] = {
        { "argb8888", SDL_PIXELFORMAT_ARGB8888 },
        { "rgb565",   SDL_PIXELFORMAT_RGB565   },
    };
    ```
    Wrap the existing `flavor_index` loop. Allocate the `expected` and
    `actual` surfaces using `dst_formats[df].format`. The `case_label`
    becomes `"index8/<dst>/<flavor>/<case>"`.
- `src/port/sdl/sdl_app.c`: at `:6068-6073` (item-1 JSON block), add a
  parallel block for the perf-2 counters:
  ```c
  io_printf(io,
            "    \"rgb565_canvas\": {"
            "\"hits\": %llu, \"argb8888_hits\": %llu, \"convert_pass_skipped\": %llu},\n",
            (unsigned long long)refresh_telemetry.rgb565_canvas_kernel_hits,
            (unsigned long long)refresh_telemetry.argb8888_canvas_kernel_hits,
            (unsigned long long)convert_pass_skipped_frames_counter);
  ```
  Place immediately after the `colorkey_loose` block. Note the third
  value is the local `sdl_app.c` static
  (`convert_pass_skipped_frames_counter`), not a renderer struct field —
  see §11 / Step 7 / P-2.5.

**Success criteria:**
- Telemetry build passes.
- `SDLApp_RunSoftwareFrameParityCheck()` succeeds on host (returns true).
  Output log includes `"index8/argb8888/strict/exact-forward"` and
  `"index8/rgb565/strict/exact-forward"` runs (and the full matrix —
  16 cases × 2 flavors × 2 dst_formats = 64 cases run).
- `python3 -c 'import json,sys; json.load(open(sys.argv[1]))' perf_capture.json`
  exits 0.
- On-device telemetry capture shows
  `rgb565_canvas_kernel_hits >> argb8888_canvas_kernel_hits` when 565 is on
  (we expect mostly-565 since the canvas drives the dispatch); flipping
  the gate flips the ratio.

**Dependencies:** Steps 1, 3, 5, 7.

**What NOT to do:**
- Do NOT add NEON test cases — host disables NEON; on-device smoke covers
  it (next step is operational, not in this plan).
- Do NOT remove the existing 8888 cases — the parameterization adds the
  565 cases on top.

**What to do if it fails:**
- If 565 parity fails on a clipped case, the dst_pitch math (`/sizeof(Uint16)`
  vs `/sizeof(Uint32)`) is wrong somewhere. Diff against the 8888 path.
- If JSON parse fails, the `rgb565_canvas` object is missing a trailing
  comma, or the io_printf format string drifted. Match the existing
  `colorkey_loose` block exactly.

---

## Out of scope (do NOT implement here)

Per the orchestrator brief:

- Removing the existing telemetry instrumentation patches (palette
  histogram + format census). Cleanup later.
- Strict-ckey kernel + packed-565 store (item #3 — future work that would
  build on this).
- NEON scaled-INDEX8 (item #4).
- INDEX4LSB rasterizer (skipped per telemetry: only 4 textures, all going
  through ARGB cache, low priority).
- Sort by index + 64-bit packed key (item #5).

## Review issues addressed

This section logs every P-1 and P-2 finding from the review at
`docs/plan-perf-2-rgb565-canvas-review.md` and records how the plan was
updated (or, if applicable, why a finding was not adopted).

### P-1 (must fix)

- **P-1.1 — `surfaces_match` not format-agnostic.** Addressed by Step 8.
  Plan now mandates a rewrite of `surfaces_match` to use per-row
  `SDL_memcmp` keyed off `SDL_BYTESPERPIXEL(format)` and per-surface
  `pitch`. Previously the plan claimed (incorrectly) that the function
  was already format-agnostic. Reviewer was right — verified at
  `software_frame_parity.c:183-206` (Uint32 cast + pitch/sizeof(Uint32)).
- **P-1.2 — `software_frame_upload_texture` ARGB-only / fbdev fallback
  silent corruption.** Addressed by §10 (mitigation strategy C: gate 565
  canvas on `native_video_writer_enabled` after a successful init) and
  Step 2 (default `rgb565_canvas_enabled = false` everywhere; flipped
  on by `sdl_app.c` only after `NativeVideoWriter_Init() == true`). Step
  2 also adds a defense-in-depth runtime check at the top of
  `upload_software_frame_to_canvas` that bails (rather than corrupts) if
  a 565 canvas ever reaches the upload path. Reviewer was right —
  verified at `sdl_game_renderer.c:8753-8761` (canvas pixels uploaded
  unconditionally to ARGB8888 staging texture) and
  `sdl_app.c:9858-9860` (env-var disable path).
- **P-1.3 — Texture-unlock NULL-out at `:11641` missed.** Addressed by
  Step 4's strategy switch to (b) (in-kernel handle-based LUT565 lookup,
  no per-task pointer). With strategy (b) there is no per-task
  `software_palette_lut_565` field to forget to NULL out at any of the
  ~11 lifecycle sites. The class of bug the reviewer caught (and would
  have hit at `:11641`, `:12336`, `:12395`) is structurally eliminated.
  Reviewer's grep finding is preserved in the plan as the rationale for
  picking strategy (b). Verified `:11641` is real (texture-unlock
  invalidation pass NULLs `software_palette_lut`).
- **P-1.4 — Wrong line for input-history glyph NULL-out.** Addressed
  indirectly by Step 4's strategy switch (no LUT565 NULL-outs needed at
  all). The original plan's `:12196-12200` citation was incorrect; the
  actual NULL-out is at `:12336-12338` — verified. The plan no longer
  needs to cite either site since strategy (b) eliminates the per-site
  fan-out.
- **P-1.5 — `cps3_canvas` location and format wrong.** Addressed by §1.
  Updated to `:9439-9440` (verified) and `RGBA8888` (verified, not
  `ARGB8888`). The plan's behavioral claim ("that path does not write
  into `software_frame_surface`") was correct; only the location/format
  text was wrong.

### P-2 (should fix)

- **P-2.1 — Harmful redundant assert.** Addressed by §9. Removed the
  `SDL_assert(frame->pitch == 384 * 2)` proposal entirely; the writer
  already handles both `pitch == 384*2` and pitched cases at
  `native_video_writer.c:76-93`. Also propagated to Step 7.
- **P-2.2 — Per-task LUT565 pointer denormalizes a global.** Addressed by
  Step 4. Switched from strategy (a) (thread `task->software_palette_lut_565`)
  to strategy (b) (in-kernel handle-based lookup via
  `HI_16_BITS(task->texture_binding)`). This is a deliberate **policy
  split** with item-1 (which keeps `task->software_palette_is_binary_alpha`
  as a per-task bool); rationale is documented in Step 4. The 565 LUT
  pointer **is** cheaply derivable from data the task already carries;
  the binary-α bool is **not** (it is a separate lifecycle, set/cleared
  at palette load/unlock — caching it per-task consolidates a write-side
  decision). 8 KB of denormalized state and the 11-site write-fan-out
  avoided.
- **P-2.3 — 565 reference raster has same Uint32-cast bug.** Addressed
  by Step 8. The pitch-divisor for the 565 branch
  (`/sizeof(Uint16)`, not `/sizeof(Uint32)`) is now called out
  explicitly, plus the dst pointer type change.
- **P-2.4 — Contradictory caller count.** Addressed by §5. Reworded to
  "3 callers in 3 distinct kernel functions" with a separate paragraph
  documenting `blend_solid_argb8888`'s 3 callers. The "4 / 3 / both
  directly-callable sites" wording is gone.
- **P-2.5 — Convert-pass-skipped counter ownership.** Addressed by Step
  7 and §11. Moved the `convert_pass_skipped_frames` counter out of
  `PerfCaptureRefreshTelemetry` and into a `static Uint64` in
  `sdl_app.c` (it is a present-path decision owned by `sdl_app.c`).
  Removed the proposed `SDLGameRenderer_NoteConvertPassSkipped` setter
  and the `RENDERER_TELEMETRY_PRESENT` placeholder macro. JSON emit
  reads the local static directly. Step 1's renderer-struct field count
  reduced from 3 to 2.
- **P-2.6 — 565 α-channel-loss vs 8888 not documented.** Addressed by
  §6. Added a "Channel-loss caveat" paragraph documenting that 565
  cannot represent partial dst α, that the renderer's
  `blend_argb8888_opaque_dst` always returns dst_α = 255 by design, and
  that the only generic-α blend (`software_frame_non_integer.c:68-103`)
  collapses to opaque-dst on binary-α palettes (854/854 confirmed by
  item-1). The parity test (Step 8) covers both flavors on both formats,
  so any silent divergence on a non-binary-α palette would be caught.
- **P-2.7 — `software_palette_lut_valid[]` line citation off.**
  Addressed by Step 1's "Files to read first" — corrected to `:181-186`
  with explicit per-decl line numbers (`software_palette_lut` at `:181`,
  `software_palette_lut_valid` at `:182`,
  `software_palette_lut_is_binary_alpha` at `:185`). The drift came from
  the orchestrator brief.
- **P-2.8 — `raster_full_height_diagonal_strip_to_software_frame` line
  range off.** Addressed by Step 6. Updated `:6845-6883` →
  `:6830-6885`. Verified by reading the function bounds.

### Self-consistency propagation

- The P-1.2 mitigation (canvas default false; setter call after writer
  init) was propagated from §10 (Gating) into Step 2 (Files to modify;
  Success criteria; What NOT to do; What to do if it fails) and into
  Step 5 (Success criteria — added the `THIRDSARM_NATIVE_VIDEO=0` smoke
  test).
- The P-2.5 counter-ownership change was propagated from §11 (Telemetry)
  into Step 1 (header field list reduced from 3 to 2) and Step 8 (JSON
  emit references the `sdl_app.c` static).
- The P-2.2 strategy switch (in-kernel LUT565 lookup) was propagated
  from Step 4 into the Files-to-change master list (no `RenderTask`
  field added, no global `current_software_palette_lut_565`).

