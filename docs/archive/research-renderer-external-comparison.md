# Research — Rendering pipeline vs. external 6 ms/frame reference

> **ARCHIVED 2026-08-30** — true at b2c79d7c, not maintained.
> Read for rationale and for what was tried and failed. Do not read for current facts; read the code.


> **Post-Phase-C update (2026-05-03):** `src/port/sdl/fbdev_presenter.{c,h}`
> have been removed. References below to `FBDevPresenter_*` /
> `fbdev_presenter` describe the path as it existed at the time of this
> research and remain useful for understanding the comparison; the
> equivalent live symbols today are in
> `src/port/sdl/fps_overlay_compositor.c` (`FPSOverlay_*` API) and
> `src/port/sdl/native_video_writer.c`.

**Date:** 2026-04-24
**Scope:** Compare our MiSTer (ARM Cortex-A9) software rendering pipeline against a detailed
design writeup from an external engineer whose ARM MiSTer-class port reportedly averages 6 ms
per frame. Identify which of his techniques we already implement, which we do not, and what
cheap instrumentation would resolve the remaining unknowns before committing to any rewrite.

**Method:** Direct source inspection with file:line citations. No assumptions. Claims labelled
"unverified" are claims the source alone cannot settle and that need a runtime probe.

---

## 1. External design — one-page summary

The external writeup (stored at `/Users/sb/Downloads/message.txt` at the time of research)
prescribes the following architecture. All of the items below are *his* recipe, not ours:

- Fixed **384×224** canvas, rendered at native resolution, scaled only once at present.
- Canvas normally **ARGB8888**; **optional RGB565 mode** for lower memory bandwidth targets.
- **Fixed-size quad command list (~512 entries)**. Each quad stores four screen positions,
  four UVs, converted Z, submission index, modulation color, texture index, palette index.
- `DrawSprite` / `DrawSprite2` build axis-aligned quads from top-left/bottom-right input.
  `DrawTexturedQuad` copies all four vertices. `DrawSolidQuad` queues an untextured quad.
- On `RenderFrame`: sort queued quads by converted Z with a deterministic submission-order
  tie-break, clear canvas, rasterize in order, reset queue. Sort an **index array** (not the
  quad structs) using **64-bit sort keys** (float Z + submission index). Quicksort with
  insertion-sort for small ranges.
- Textures: keep 8bpp indexed as borrowed pointers; support 4bpp packed; convert direct 16-bit
  to ARGB8888 once at upload; store power-of-two width/height masks for cheap wrap.
- Palettes: convert PS2 32-bit or 16-bit palette data to ARGB8888; in RGB565 canvas mode, also
  build a preconverted RGB565 palette mirror; detect the "palette index 0 transparent, 1..N
  opaque" color-key pattern and store a `ckey_extent`; scan each 8bpp texture at creation for
  its max index.
- AABB fast path is the main path. Split rows at power-of-two texture wrap boundaries. Detect
  unscaled case (du==dv==1.0) and switch to memcpy-style copies. Dispatch to specialized row
  kernels by (format × scaled × flipped × modulation × color-key).
- Specialized row kernels for: unscaled direct color, unscaled 8bpp, unscaled 4bpp, scaled
  direct, scaled 8bpp, scaled 4bpp, X-flipped variants of each, solid fill.
- Parallelogram path for sheared stage backgrounds before falling back to two textured
  triangles with barycentric + top-left edge rule.
- Scalar optimizations: `if alpha==0 skip`, `if alpha==0xFF overwrite`, only source-over for
  partial alpha, identity-modulation skip, hot-loop unroll, prefetch, 4bpp peel for alignment,
  RGB565 direct-blend math, RGB565 preconverted palette for indexed paths.
- Color-key fast paths: 8bpp+RGB565 reads 8 indices at a time as two `uint32_t`s, uses a
  zero-byte detection trick, packs two RGB565 pixels into one 32-bit store when all-opaque,
  writes only nonzero indices otherwise. 4bpp equivalent uses nibble-zero tests.
- NEON: added after scalar renderer is correct. ARGB8888 solid/direct-blit in 16-pixel blocks
  with `vld4`/`vst4`, modulation as `(c * m + 128) >> 8`, source-over as `s*a + d*(255-a)`,
  preserve destination where α=0, direct store when all-opaque & identity-mod. 4bpp NEON uses
  table lookup. 8bpp NEON discouraged on ARMv7 (no wide gather).
- Present: avoid SDL renderer on ARM. On fbdev (MiSTer-class) mmap the 32bpp framebuffer and
  scale directly into it. Nearest-neighbor default. Fixed-point integer-scale shortcut.

---

## 2. Our current pipeline — verified factual map

### 2.1 Canvas

- **Physical canvas**: `software_frame_surface`, `SDL_PIXELFORMAT_ARGB8888`, 384×224 — created
  at `src/port/sdl/sdl_game_renderer.c:3373`. Declared at `sdl_game_renderer.c:148`.
- **Dimensions**: `cps3_width = 384`, `cps3_height = 224` — `sdl_game_renderer.c:121-122`.
- **Secondary canvas**: `cps3_canvas` (an `SDL_Texture`, ARGB8888, `SDL_TEXTUREACCESS_TARGET`)
  created at `sdl_game_renderer.c:9309-9311`. Only used when `software_frame_mode_active=false`
  (i.e. desktop / non-MiSTer). Bypassed on MiSTer.
- **Gate**: `software_frame_mode_active` bool — declared `sdl_game_renderer.c:158`, assigned at
  `9316`. MiSTer builds enable it; desktop builds default off.

### 2.2 Draw entry points — all deferred

- `SDLGameRenderer_DrawSprite` — `sdl_game_renderer.c:11935` → `draw_sprite_rect()` → enqueues.
- `SDLGameRenderer_DrawSprite2` — `sdl_game_renderer.c:11948` → same path.
- `SDLGameRenderer_DrawTexturedQuad` — `sdl_game_renderer.c:11880` → attempts
  `try_setup_textured_rect_task()` for AABB input; otherwise stores 4-vertex geometry task.
- `SDLGameRenderer_DrawSolidQuad` — `sdl_game_renderer.c:11919` → always a geometry task with
  zero UVs.
- None of these rasterize inline. All go through `push_render_task()` at
  `sdl_game_renderer.c:1279`.

### 2.3 Command queue

- `RenderTask` struct — `sdl_game_renderer.c:89-108`. Includes `SDL_Vertex vertices[4]`,
  `SDL_FRect dst_rect`, `SDL_FRect src_uv_rect`, `SDL_FlipMode flip`, `Uint32 color`, `float z`,
  `int index`.
- Storage: `static RenderTask render_tasks[RENDER_TASK_MAX]` with `RENDER_TASK_MAX = 1024` —
  `sdl_game_renderer.c:40, 545`.
- Task types: `RENDER_TASK_TYPE_GEOMETRY = 0` and `RENDER_TASK_TYPE_TEXTURED_RECT = 1` —
  `sdl_game_renderer.c:55-58`. Axis-aligned sprites collapse to the rect form in
  `try_setup_textured_rect_task()`; non-axis-aligned quads remain as 4-vertex geometry.
- Overflow: hard cap with silent drop — `if (render_task_count >= RENDER_TASK_MAX) break;` at
  `sdl_game_renderer.c:12020-12022`.

### 2.4 Sort

- Key: float `z` primary, `index` secondary (descending tie-break). Comparator at
  `sdl_game_renderer.c:6770-6785`. Ties are broken by reversed `index` comparison (comment:
  "This eliminates z-fighting").
- Implementation: `insertion_sort_render_tasks()` at `sdl_game_renderer.c:6787-6801` **moves
  full `RenderTask` structs during swaps** (not an index array). `qsort()` at
  `sdl_game_renderer.c:9513` likewise on full structs.
- Sort gate at `sdl_game_renderer.c:9506-9515`: only run when `count > 1` and inversions are
  detected. Insertion sort chosen when `count <= 512` and inversions `<= 8`; otherwise
  `qsort`.

### 2.5 Frame flow — `SDLGameRenderer_RenderFrame`

- Entry: `sdl_game_renderer.c:9497-9563`.
- Sort: lines `9505-9517`.
- Dispatch:
  - `software_frame_mode_active=true` (MiSTer) → `render_frame_to_software_surface()` at
    `sdl_game_renderer.c:8473`.
  - Else → `submit_render_tasks()` at `sdl_game_renderer.c:8908` (SDL_RenderGeometry /
    SDL_RenderTexture path).
- Canvas clear: not performed explicitly in `RenderFrame`; handled per-texture via
  `destroy_textures()` at `EndFrame` (`sdl_game_renderer.c:9576`).

### 2.6 Textures

- Accepted PS2 formats — `SDLGameRenderer_CreateTexture` at `sdl_game_renderer.c:11440+`:
  - `SCE_GS_PSMT8` → `SDL_PIXELFORMAT_INDEX8` (line 11452-11454).
  - `SCE_GS_PSMT4` → `SDL_PIXELFORMAT_INDEX4LSB`, pitch = `width/2` (line 11457-11459).
  - `SCE_GS_PSMCT16` → `SDL_PIXELFORMAT_ABGR1555` (line 11462-11464).
- `SDLGameRenderer_UnlockTexture` at `sdl_game_renderer.c:11408-11438` invalidates caches; no
  format conversion here.
- No power-of-two wrap-mask storage. Verified by grep — the code uses **clamp-to-edge** on UV
  (examples at `sdl_game_renderer.c:7243-7244, 7395, 7403, 7574, 7589, 7906, 7912` and
  `software_frame_non_integer.c:340-345`).
- No per-texture max-index scan. Verified by grep (no `max_index` symbols).

### 2.7 Palettes

- Creation: `SDLGameRenderer_CreatePalette` at `sdl_game_renderer.c:11547-11566`.
- Conversion: `fill_palette_colors_from_fl_texture` at `sdl_game_renderer.c:9128-9171`:
  - 32-bit (`SCE_GS_PSMCT32`, 4 bytes/entry) via `read_rgba32_color` at `9070-9075`: raw read
    of B,G,R,A bytes — alpha is a **full 8-bit byte**, any value 0..255 passes through
    untransformed.
  - 16-bit (`SCE_GS_PSMCT16`, 2 bytes/entry) via `read_rgba16_color` at `9093-9098`: 5-5-5
    color plus MSB alpha — `(pixel & 0x8000) ? 255 : 0`. **By hardware this is binary alpha.**
- Palette size is 16 (for 4bpp) or 256 (for 8bpp) — decided by `fl_palette->width *
  fl_palette->height` at `sdl_game_renderer.c:9129+`.
- Per-palette ARGB LUT: `software_palette_lut[FL_PALETTE_MAX][256]` at
  `sdl_game_renderer.c:176-178`, built by `build_software_palette_lut` at
  `sdl_game_renderer.c:9177-9193`.
- `FL_PALETTE_MAX = 1088` (foundaps2.h:10). LUT total: 1088 × 256 × 4 B = **1.125 MB**.
- No RGB565 LUT mirror exists. Verified by grep.
- No `ckey_extent` / "palette[0] transparent, 1..N opaque" detection. Verified by grep.
- Palette data flows unchanged from PS2 source — `flPS2ConvertAlpha`
  (`src/sf33rd/AcrSDK/ps2/flps2etc.c:520-544`) divides texture alpha by 2 for `bitdepth==4`
  (32-bit RGBA textures), but is **not called on palette data**. Palettes in APX/TIM2 are
  copied raw at `flps2etc.c:404, 508`.

### 2.8 Rasterizer paths — what exists today

All MiSTer rasterization goes through `render_frame_to_software_surface()` at
`sdl_game_renderer.c:8473` and eventually into:

- `raster_textured_task_to_software_frame()` — `sdl_game_renderer.c:7074+`. Locks surfaces,
  tries fast copy, falls back to non-integer lookup, then generic float-UV.
- `try_fast_copy_fast_textured_task_to_software_frame()` — `sdl_game_renderer.c:6098-6315`
  (INDEX8 branch) and `6317+` (ARGB8888 branch).
  - **INDEX8 exact copy** (unscaled, H/V flip, with and without color-mod, NEON 4 px/iter
    tail-scalar) — `sdl_game_renderer.c:6121-6285`.
  - **INDEX8 scaled copy** (scalar only, uses `populate_scaled_lookup_table` to prebuild
    `src_x_lookup[]`/`src_y_lookup[]`) — `sdl_game_renderer.c:6288-6313`.
  - **ARGB8888 exact / scaled** paths — `sdl_game_renderer.c:6317-6420+`.
- `raster_textured_parallelogram_to_software_frame()` — `sdl_game_renderer.c:7733-7845` (int
  parallelograms) and `raster_textured_float_parallelogram_to_software_frame()` at `7847-7938`
  (affine). Handles sheared rectangular quads via row-walk.
- `raster_textured_triangle_to_software_frame()` — `sdl_game_renderer.c:8016-8092`. Per-pixel
  barycentric with `w0, w1, w2` edge weights, signed-area winding check at `8060-8063`. **No
  top-left edge rule.**
- `raster_solid_triangle_to_software_frame()` — `sdl_game_renderer.c:8301-8387`. Solid fill
  via `fill_argb8888_span()` at `8377`.
- `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888()` — `src/port/sdl/software_frame_non_integer.c:452-716`
  (lookup-table row blitter for non-integer UV).

### 2.9 Scalar optimizations already present

- **Alpha-0 skip / alpha-0xFF overwrite / else blend** — everywhere. Examples:
  `sdl_game_renderer.c:6176-6178, 6205-6211, 6274-6281, 7778-7779, 8002-8005, 8080-8083`;
  `software_frame_non_integer.c:568-571, 623-626, 694-697`.
- **Identity-modulation skip** (`color == 0xFFFFFFFFu`) —
  `sdl_game_renderer.c:5576, 7372, 7545`; `software_frame_non_integer.c:419, 534`.
- **16.16 fixed-point UV step** — `sdl_game_renderer.c:3624-3646`
  (`sa_bg_cache_restore_background_scaled`).
- **Full-opaque row memcpy** — parallelogram path uses `full_opaque_row_mask` + `SDL_memcpy`
  at `sdl_game_renderer.c:7816-7820`.
- **Prefetch** — `__builtin_prefetch()` on src/dst rows in INDEX8 hot loops
  (`sdl_game_renderer.c:6152-6153, 6196-6197, 6231-6233, 6266-6268, 6300-6302`).

### 2.10 NEON

- Header guard: `#if defined(PORT_MISTER) && (defined(__ARM_NEON) || defined(__ARM_NEON__))`
  at `sdl_game_renderer.c:26-31`. Macro `RENDERER_HAVE_NEON`.
- No runtime CPU detection. Compile-gated only.
- NEON kernels:
  - `neon_blend_4pixels` / `neon_blend_modulate_4pixels` — used inside INDEX8 exact-copy hot
    loops at `sdl_game_renderer.c:6168, 6247`.
  - NEON modulation/blend primitive blocks — `sdl_game_renderer.c:5877-5928, 5930-5976,
    6142-6188`.
  - ARGB8888 → RGB565 convert — `sdl_app.c:186-226` (`convert_argb8888_to_rgb565`), 8 px/iter
    with `vld4_u8` deinterleave, scalar tail.
- Scalar fallback is always present as tail loops and full alternative implementations. NEON
  is opt-in via compile flag; scalar is correctness baseline.
- No scaled-INDEX8 NEON kernel. The scaled-copy branch at `sdl_game_renderer.c:6288-6313` is
  pure scalar.
- No NEON path for INDEX4LSB (see §2.13).

### 2.11 Present path on MiSTer

- Producer: `software_frame_surface` (ARGB8888, 384×224). Accessor:
  `SDLGameRenderer_GetSoftwareFrameSurface()` at `sdl_game_renderer.c:9418`.
- **Per-frame conversion pass**: `sdl_app.c:10527-10530` calls
  `convert_argb8888_to_rgb565(frame->pixels, native_video_rgb565_scratch, 384*224)` into the
  cached RGB565 scratch buffer declared at `sdl_app.c:184`.
- Writer: `NativeVideoWriter_WriteFrame(scratch, 384, 224, 384*2)` at `sdl_app.c:10531-10533`.
- Writer implementation — `src/port/sdl/native_video_writer.c:76-107`:
  - mmap'd DDR3 region at physical `0x3A000000`, size `0x60000`.
  - Dual-buffered RGB565 frames at offsets `NV_BUF0_OFFSET=0x100` and `NV_BUF1_OFFSET=0x2A200`,
    each `384*224*2 = 172,032` B.
  - Single `memcpy` when `pitch == 384*2`; row-by-row otherwise.
  - Control word at offset 0 written last with `(frame_counter << 2) | (active_buf & 1)`.
  - `active_buf ^= 1` for next frame.
- FPS overlay drawn into the software frame *before* the convert pass, at
  `sdl_app.c:10523-10526`, via `FBDevPresenter_ApplyFPSOverlayToBuffer((Uint32*)frame->pixels,
  frame->w, frame->h)`.
- `fbdev_presenter` path is mutually exclusive with `native_video_writer` —
  `sdl_app.c:10544` gates it with `fbdev_presenter_enabled && !native_video_writer_enabled`.
  fbdev is dead on shipped MiSTer builds (see user memory `feedback-fbdev-not-used.md`).
- No DRM/KMS dumb-buffer code. (`kmsdrm` at `sdl_app.c:788` is only an SDL video driver hint,
  not a pixel path.)

### 2.12 RmlUi — fully removed

Only historical comments remain in the tree; there is no live RmlUi code.
- `src/netplay/netplay.c:87` — comment "Dormant since the RmlUi lobby UI was removed".
- `src/netplay/direct_p2p_overlay.c:7` — comment "no RmlUi dependency, no separate
  SDL_Renderer target".
- `src/port/sdl/sdl_app.c:10588-10592` — comment referring to a non-existent `ENABLE_RMLUI`
  block. The block itself is gone. (`grep ENABLE_RMLUI` returns only the comment.)

Implication: there is no UI overlay layer that writes into `software_frame_surface` other than
the FPS overlay.

### 2.13 INDEX4LSB — recognized but not handled by the fast path

- Created correctly as `SDL_PIXELFORMAT_INDEX4LSB` with pitch `width/2` —
  `sdl_game_renderer.c:11457-11459`.
- `current_software_source_is_index8` is gated on `surface->format ==
  SDL_PIXELFORMAT_INDEX8` — `sdl_game_renderer.c:11676-11695`. INDEX4LSB always sets the flag
  to `false`.
- Telemetry counter exists: `perf_capture_refresh_telemetry.index4_attempts` incremented at
  `sdl_game_renderer.c:1528-1530`, emitted in perf JSON at `sdl_app.c:5939-5940` (key
  `"index4"`).
- Asset-side evidence that 4bpp textures *are* created by the engine:
  - `src/sf33rd/AcrSDK/ps2/flps2vram.c:97` — `format = SCE_GS_PSMT4`.
  - `flps2vram.c:1032-1036` — size formula `(dw * dh) >> 1` confirms 4-bit packing.
  - `flps2etc.c:500` — both PSMT4 and PSMT8 require palettes.
  - `src/sf33rd/Source/PS2/mc/knjsub.c:1080` — creates a PSMT4 font texture (debug/UI).
- **Unverified from source alone:** what actually happens at render time when an INDEX4LSB
  surface arrives at the generic non-INDEX8 path at `sdl_game_renderer.c:6317` (which casts
  source pixels to `const Uint32*`). No refresh/convert step was traced end-to-end. Either
  (a) an ARGB refresh cache intercepts it before the cast, (b) INDEX4 textures never reach
  the software path in practice, or (c) it corrupts silently. This requires runtime
  instrumentation to resolve — see §5.

---

## 3. Side-by-side match matrix

| Capability | External recipe | Our code | Status |
|---|---|---|---|
| 384×224 fixed canvas | ✓ | ✓ — `sdl_game_renderer.c:121-122` | match |
| Deferred queue | 512 entries | 1024 entries — `sdl_game_renderer.c:40` | match (larger) |
| Sort by Z + submission tie-break | index array + 64-bit key | full-struct swaps, float Z, int index — `sdl_game_renderer.c:6770-6785, 6787-6801` | **functionally yes, inefficient** |
| Axis-aligned rect collapse | ✓ | ✓ — `try_setup_textured_rect_task()` at `sdl_game_renderer.c:11845` | match |
| INDEX8 kept through to raster loop | ✓ | ✓ — `INDEX8_RASTERIZATION_ENABLED`, per-palette ARGB LUT at `sdl_game_renderer.c:176-178, 6110-6315` | match |
| INDEX4 packed kept through | ✓ | recognized at upload, no fast path — `sdl_game_renderer.c:11457-11459` | **gap; runtime behaviour unverified** |
| 16-bit → ARGB once at upload | ✓ | partially — 16-bit textures are `SDL_PIXELFORMAT_ABGR1555`, not pre-converted; 16-bit palettes are expanded to ARGB LUT | **partial match** |
| PO2 wrap masks | ✓ | no — clamp only | gap (but correct for CPS3, which doesn't wrap) |
| Per-texture max-index scan | ✓ | no | gap |
| Palette ckey-extent detection | ✓ | no | gap |
| AABB fast path, unscaled + scaled + flip | ✓ | ✓ — `sdl_game_renderer.c:6098-6420+` | match |
| Parallelogram path | ✓ | ✓ — `raster_textured_parallelogram_to_software_frame` at `7733+` | match |
| Triangle fallback (barycentric) | ✓ | ✓ — `raster_textured_triangle_to_software_frame` at `8016-8092` | match |
| Top-left edge rule on triangles | ✓ | no — signed-area winding only | gap (cold path) |
| Alpha-0 skip / alpha-0xFF overwrite | ✓ | ✓ | match |
| Identity-modulation skip | ✓ | ✓ | match |
| Unscaled-case memcpy shortcut | ✓ | ✓ — parallelogram path at `7816-7820` | match |
| 16.16 fixed-point UV step | ✓ | ✓ — `sdl_game_renderer.c:3624-3646` | match |
| Row split at PO2 wrap boundary | ✓ | no (clamp only) | gap |
| RGB565 canvas mode | optional | no — canvas is ARGB8888 throughout | **gap** |
| RGB565 palette LUT mirror | optional | no | gap (depends on canvas mode) |
| RGB565 reduced-alpha blend math | optional | no | gap (depends on canvas mode) |
| Color-key 8bpp fast kernel (two-u32 load, zero-byte trick, packed-565 store) | ✓ | no | **gap** |
| 4bpp color-key kernel (nibble-zero trick) | ✓ | no | gap |
| NEON on hottest INDEX8 blend | ✓ | ✓ — `sdl_game_renderer.c:6142-6188, 6225-6258` (4 px/iter) | match |
| NEON on scaled INDEX8 | ✓ | no | **gap** |
| NEON on 4bpp | ✓ | no | gap |
| Scalar baseline with NEON opt-in | ✓ | ✓ | match |
| Direct FPGA/fbdev present, no SDL renderer | ✓ | ✓ — `native_video_writer.c` + `software_frame_mode_active` | match |
| fbdev scaler / nearest fixed-point scale | ✓ | N/A — FPGA scales | not applicable |

---

## 4. Deep-dive findings — per-area evidence

### 4.1 RGB565 canvas feasibility

**Inventory of places that assume ARGB8888 writes into `software_frame_surface`:**

- 14 sites cast `software_frame_surface->pixels` (or parallel buffers fed by it) to `Uint32*`
  or `uint32_t*` — `sdl_game_renderer.c:3630, 7752, 7866, 7971, 8049, 8113, 8331, 8423`;
  `software_frame_non_integer.c:417, 422, 530, 531, 539, 540`.
- ~20 pitch-math sites divide pitch by `sizeof(Uint32)`.
- Canvas-write blend/modulate/fill primitives and their call counts:
  - `blend_argb8888_opaque_dst` — 23 callers — def at `sdl_game_renderer.c:6640-6659`.
  - `modulate_argb8888` — 18 callers — def at `sdl_game_renderer.c:6542-6556`.
  - `modulate_argb8888_blue_tint` — 9 callers — def at `sdl_game_renderer.c:6570-6584`.
  - `fill_argb8888_span` — 4 callers — def at `sdl_game_renderer.c:6661-6680`.
  - `blend_argb8888` (generic, used by non-integer path) — 15 callers — def at
    `software_frame_non_integer.c:68`.
- Bit-layout constants: `0xFF000000u` (5 sites), `>> 24 & 0xFF` (~40 sites), shift-packs
  (`<< 24`, `<< 16`, `<< 8`) (~30 sites), format asserts `SDL_PIXELFORMAT_ARGB8888` at
  `sdl_game_renderer.c:2885, 3219`.

**Unaffected:**
- `cps3_canvas` (SDL_Texture) — bypassed whenever `software_frame_mode_active=true`.
- `SDL_BlitSurface` / `SDL_ConvertSurface` sites (`sdl_game_renderer.c:3257, 3258, 3263, 3271,
  3272, 3277, 3531`) — none of them touch `software_frame_surface`; they operate on texture
  refresh caches.
- Pre-converted 16→32 texture cache (`SDL_ConvertSurface` at `3531`) — separate buffer, not
  tied to canvas format.

**Also needs conversion if canvas format changes:**
- `sa_bg_cache_surface` — created ARGB8888 at `sdl_game_renderer.c:3399`, used as a backup
  copy of the canvas (readback at `3630-3639`, restore at `3718`).
- `FBDevPresenter_ApplyFPSOverlayToBuffer` — hardcoded `Uint32*` buffer and `row_bytes =
  layout->width * sizeof(Uint32)` (`fbdev_presenter.c:1175, 2538-2589`). Would need a 565
  variant.

**Memory cost of a 565 palette LUT mirror:** `FL_PALETTE_MAX × 256 × 2 B = 1088 × 512 = 557,056
B ≈ 0.56 MB` on top of the existing 1.125 MB ARGB LUT.

**Hot-path elimination:** the per-frame `convert_argb8888_to_rgb565` pass at `sdl_app.c:186-226,
10527-10530` would go away. That pass touches 86,016 pixels — 344 KB of reads and 172 KB of
writes per frame (source ARGB8888 to destination RGB565 scratch), for a total of ~516 KB per
frame of memory traffic. At 60 Hz that is ~31 MB/s of bus traffic eliminated.

**Precision:** 565 blend truncates R/B to 5 bits (max 1 LSB error vs 8 bits) and G to 6 bits
(max 1 LSB error vs 8 bits). Compounding occurs on partial-alpha blends. Not proven visually
acceptable or unacceptable — requires A/B comparison if undertaken.

**Specific precision-sensitive callers to audit:**
- Blue-tint modulation (`modulate_argb8888_blue_tint`) — uses a `rg_factor` derived from the
  modulation color. Already an approximation; additional 565 quantization may or may not be
  visible. (Historically also hit the now-retired ghost half-resolution mode, removed in
  `d9dfe736` along with `ghost_resolution_mode` / `is_ghost_sprite_color`.)

**Verdict:** Mechanically feasible. No format-contradiction blockers once RmlUi's absence is
confirmed (done — §2.12). The work is duplicating ~5 pixel primitives, adding a 565 palette
LUT, swapping the canvas surface format, and writing a 565 FPS-overlay variant.

### 4.2 Color-key palette pattern

**Hardware-level certainty (16-bit palettes):**
`read_rgba16_color` at `sdl_game_renderer.c:9093-9098` maps bit 15 to `α=0` or `α=255`
exclusively. Any palette sourced from `SCE_GS_PSMCT16` **is** binary-alpha by construction.

**Open question (32-bit palettes):**
`read_rgba32_color` at `sdl_game_renderer.c:9070-9075` reads α as a raw byte. There is no code
path that clamps, quantizes, or forces index-0 to α=0. `flPS2ConvertAlpha`
(`flps2etc.c:520-544`) divides α by 2 but is only called on 32-bit texture data
(`bitdepth==4`), **not** on palette data. So 32-bit palette alpha passes through unchanged —
it can be any value 0..255.

**Convention search:**
No grep hits for `ckey`, `colorkey`, `transparent` (in palette sense), `trans_index`, or
`alpha_index` in `src/sf33rd/` or the renderer. `get_my_trans_mode()` in
`src/sf33rd/Source/Game/rendering/texcash.c:438` returns a transparency *render state* (blend
mode), not an index convention.

**No palette-alpha histogram exists** — no existing telemetry counter reports α distribution.

**Verdict:** 16-bit-sourced palettes *are* ckey-pattern by physics. 32-bit-sourced palettes are
an open question that no amount of code reading will resolve. Resolvable by a ~10 LOC
instrumentation patch (see §5).

### 4.3 INDEX4 actual behaviour at runtime

**What is certain from source:**
- Textures are created as INDEX4LSB with packed pitch (§2.13).
- The INDEX8 fast path rejects INDEX4LSB (explicit format check at
  `sdl_game_renderer.c:11676-11695`).
- The generic non-INDEX8 path at `sdl_game_renderer.c:6317+` casts source pixels to
  `const Uint32*`.

**What is not certain from source:**
- Whether a refresh-to-ARGB step interposes before that cast (`SDL_ConvertSurface` at
  `sdl_game_renderer.c:3531` converts texture refresh caches to ARGB8888, but was not traced
  to confirm it handles INDEX4LSB).
- Whether any INDEX4LSB surface actually reaches the raster path during real gameplay (`knjsub`
  font textures may route through a different path; not all INDEX4 creation sites necessarily
  feed the software rasterizer).
- Whether the `"index4"` JSON telemetry key is absent from recent perf captures because the
  counter never fires or because the capture schema predates the counter. Not resolvable from
  the capture files alone.

**Verdict:** Unverified. Requires runtime instrumentation (see §5).

---

## 5. Open questions and proposed instrumentation

None of the following require architectural changes; they are observation patches intended to
run once, on a representative gameplay session, then be removed.

### 5.1 Palette alpha histogram

**Patch location:** `SDLGameRenderer_UnlockPalette` (`sdl_game_renderer.c:11356`) or
`build_software_palette_lut` (`sdl_game_renderer.c:9177`).

**Behaviour:** After the LUT is rebuilt, scan `colors[0..color_count-1]` and emit a single log
line per palette:

```
palette[PH] size=N alpha0=X alpha255=Y alpha_mid=Z source_fmt={32|16}
```

Where `alpha0` is the count of entries with `α=0`, `alpha255` is the count with `α=255`, and
`alpha_mid` is the count with any other value.

**Decision rules after one gameplay session:**
- If `alpha_mid == 0` always: the ckey fast path is universally applicable.
- If `alpha_mid == 0 && alpha0 == 1 && palette[0].α == 0`: the strict "index 0 transparent,
  1..N opaque" pattern holds — maximum benefit.
- If `alpha_mid > 0` on meaningful palette counts: full 8-bit-alpha blend stays required.

### 5.2 Software-path format census

**Patch location:** `SDLGameRenderer_SetTexture` (`sdl_game_renderer.c:11676+`), in the
INDEX8-path gating block.

**Behaviour:** Increment a counter per surface format whenever a texture is bound for software
rasterization. Emit one summary line at end of capture:

```
soft_path_binds INDEX8=A INDEX4LSB=B ABGR1555=C ARGB8888=D other=E
```

**Decision rules:**
- `INDEX4LSB > 0`: we have a correctness concern or a missing kernel. Trace what the generic
  path does for these inputs. Either add an INDEX4 kernel or insert an upload-time unpack
  to INDEX8.
- `INDEX4LSB == 0`: INDEX4 textures never reach the software raster path; skip any 4bpp raster
  work permanently.

### 5.3 Sort swap cost

**Patch location:** around `insertion_sort_render_tasks` (`sdl_game_renderer.c:6787-6801`) and
the `qsort` invocation at `9513`.

**Behaviour:** Count bytes moved during sort per frame (`num_swaps * sizeof(RenderTask)`).
Emit mean and p99 per capture window.

**Decision rule:**
- If per-frame swap bytes are below ~32 KB on any realistic lane, the index-array + packed-key
  conversion is a cosmetic improvement — deprioritise.
- If swap bytes spike above ~128 KB/frame on hot scenes, the conversion is likely measurable.

---

## 6. Prioritised recommendations (research-only)

Ordered by (likely impact × inverse cost), given the verified facts above. **Nothing here has
been implemented. This is a decision aid.**

1. **Ship a one-off telemetry build** covering §5.1 and §5.2 simultaneously. Single gameplay
   session resolves two of the three largest uncertainties. Cost: ~20 LOC, one build, one
   capture. **Do this first — everything else depends on its output.**

2. **RGB565 canvas mode on MiSTer.** Dependent on (1) only for confidence in precision budget
   (if palettes turn out to be binary-alpha, the blend precision argument effectively
   disappears). Core win is the per-frame convert pass plus halved destination bandwidth in
   every row kernel. Cost: moderate, mechanical, no blockers.

3. **Color-key fast path.** Dependent on (1). If `alpha_mid == 0`, this is the largest per-pixel
   speedup available; pairs naturally with (2) for the two-u32 load and packed-565 store.

4. **NEON scaled-INDEX8 kernel.** Targets a known hot workload (SA bursts, scaled backgrounds).
   Independent of (1). Smaller win than (2) or (3).

5. **INDEX4 correctness/perf.** Dependent on (1). If census shows no INDEX4 in software path,
   drop entirely. If present, prioritise correctness (likely upload-time INDEX4→INDEX8
   unpack) over a dedicated kernel until perf justifies it.

6. **Sort by index + 64-bit packed key.** Deprioritise unless §5.3 shows significant swap
   bytes.

---

## 7. Things we deliberately do not adopt

- **fbdev scaler / 32bpp fbdev mmap / nearest-neighbor scaling at present time.** FPGA scales
  on the output side; our present is a fixed 384×224 memcpy to DDR3 plus a control word. The
  entire present-side scaler section of the external recipe is not applicable.
- **DRM/KMS dumb-buffer path.** Not our platform.
- **AArch64 `vqtbl1q` 8bpp NEON kernel.** We target Cortex-A9 ARMv7 only. The external writeup
  explicitly says ARMv7 8bpp should stay scalar.
- **ARGB8888 `vld4`/`vst4` 16-px/iter solid/direct kernels.** Our current 4-px/iter INDEX8
  NEON is already close to memory-bound on A9; widening is not likely to help without also
  reducing per-pixel work (which is what RGB565 + ckey do together).

---

## 8. Sources

- External writeup: `/Users/sb/Downloads/message.txt` (local file, not checked into repo).
- Rendering entry points: `src/rendering/renderer.c`.
- Core renderer: `src/port/sdl/sdl_game_renderer.c` (~12 k lines).
- Non-integer raster: `src/port/sdl/software_frame_non_integer.c`.
- Present path: `src/port/sdl/sdl_app.c`, `src/port/sdl/native_video_writer.c`,
  `src/port/sdl/fbdev_presenter.c`.
- Texture/palette loaders: `src/sf33rd/AcrSDK/ps2/flps2vram.c`,
  `src/sf33rd/AcrSDK/ps2/flps2etc.c`, `src/sf33rd/Source/PS2/mc/knjsub.c`.
- Prior project notes: `docs/agent-memory/mister-perf-opportunities.md`,
  `docs/performance-optimizations.md`, `feedback-fbdev-not-used.md` (user memory).
