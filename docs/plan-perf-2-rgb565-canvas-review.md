# Review — perf-2 RGB565 canvas plan

**Plan reviewed:** `/Users/sb/Developer/3sx-mister-perf/docs/plan-perf-2-rgb565-canvas.md`
**Branch:** `perf` @ `4d58c909`
**Reviewer mode:** independent-verify against source. Every claim below was checked
against the file/line cited in the plan. Citations of the form `path:line` mean
"reviewer opened that file and read that line."

---

## P-1 — must fix

### P-1.1 — `surfaces_match()` is NOT format-agnostic; 565 parity will read past row end

Plan §12 step 4 (line 290 of plan) asserts:

> `surfaces_match()` already does pixel-by-pixel `SDL_memcmp` per row at `:194-201`,
> format-agnostic. It works on 565 just as well as 8888.

This is **wrong**. Reading `software_frame_parity.c:183-206`:

```
const Uint32* expected_pixels = (const Uint32*)expected->pixels;
const Uint32* actual_pixels = (const Uint32*)actual->pixels;
const int expected_pitch = expected->pitch / (int)sizeof(Uint32);   // /4, not /2
…
for (int y = 0; y < expected->h; y++) {
    for (int x = 0; x < expected->w; x++) {                          // x < 384
        const Uint32 expected_pixel = expected_pixels[y * expected_pitch + x];
        …
    }
}
```

There is no `SDL_memcmp` anywhere in this function. The loop reads `Uint32` per
pixel and divides pitch by `sizeof(Uint32)`. Applied to a 565 384×224 surface:

- Expected: 192 Uint32-words per row, 384 565-pixels per row.
- Loop reads `expected_pixels[y * 192 + x]` for `x < 384` — i.e. it reads 384
  Uint32 words into a row that only has 192 valid words, sliding into the **next
  row** every iteration past x=192.

The fix has to update `surfaces_match` to either (a) take a `bytes_per_pixel`
parameter and drive the loop in pixels of the correct width, or (b) use real
`SDL_memcmp` per row (`expected->pitch` bytes per row, both surfaces). The plan
must amend Step 8 to also extend `surfaces_match`. Failing to do this means
the entire 565 parity matrix passes for nonsense reasons.

### P-1.2 — `software_frame_upload_texture` stays ARGB8888; fbdev-fallback path with 565 canvas silently corrupts

Plan §11 step 5 (line 870-873 of plan) explicitly directs:

> Do NOT change the SDL_Texture format anywhere (`software_frame_upload_texture` at
> `:3396-3404` …). They are independent of canvas format and are not touched on
> the MiSTer present path.

But `upload_software_frame_to_canvas()` at `sdl_game_renderer.c:8753-8761` does:

```
SDL_UpdateTexture(software_frame_upload_texture, NULL,
                  software_frame_surface->pixels,
                  software_frame_surface->pitch);
```

The upload texture is created `SDL_PIXELFORMAT_ARGB8888` at `:3397`. If the
canvas is `SDL_PIXELFORMAT_RGB565`, `SDL_UpdateTexture` will reinterpret 565
bytes as 8888 — silent pixel corruption.

This path is reachable when:
- PORT_MISTER build with `THIRDSARM_NATIVE_VIDEO=0` env var (writer disabled at
  `sdl_app.c:9858-9860`), OR
- `NativeVideoWriter_Init()` fails (no FPGA mapping), OR
- Any future configuration where `native_video_writer_enabled` is false but
  `software_frame_mode_enabled` is true and the canvas was already allocated 565.

The plan defaults `rgb565_canvas_enabled = true` under PORT_MISTER unconditionally,
so any of those failure modes silently corrupts the frame. Fix options:

1. Gate `rgb565_canvas_enabled` default on `native_video_writer_enabled` (the
   bandwidth win only matters on that path anyway).
2. Make `ensure_software_frame_upload_texture` format-aware — read the canvas
   format and create a matching texture.
3. Refuse to allocate a 565 canvas if the writer isn't initialized yet (and
   re-allocate on writer init).

Whichever is picked, Step 5 must explicitly cover this case. As written, it
hides the failure mode behind a "do not touch" instruction.

### P-1.3 — Step 4 misses the texture-unlock NULL-out at `:11641`

Plan Step 4 (line 738-744) enumerates the LUT-pointer write sites that need a
parallel `_565` assign:

> Six clear sites at `:1093-1095, 1138-1140, 1183-1185, 1229-1231, 1268-1270,
> 9468-9470`; binding site at `:11822-11824` and `:11830-11832`; quad-task at
> `:11860-11865`; mtrans path at `:12174-12176`; input-history glyph site at
> `:12196-12200` (set to NULL).

Reviewer grep:

```
$ grep -n 'render_tasks\[.*\]\.software_palette_lut\s*=\|task->software_palette_lut\s*=\|\.software_palette_lut\s*=' \
    src/port/sdl/sdl_game_renderer.c
11641:            render_tasks[i].software_palette_lut = NULL;
11863:            task->software_palette_lut = textured ? current_software_palette_lut : NULL;
12174:            task->software_palette_lut = current_texture_binding_valid ? current_software_palette_lut : NULL;
12336:            task->software_palette_lut = NULL;
12395:            task.software_palette_lut = palette_lut;       // parity shim
```

`:11641` is **not** in the plan's list. Reading `sdl_game_renderer.c:11635-11644`
shows it's the texture-unlock invalidation pass — when a texture is unlocked
mid-frame, every queued render task referencing it has its LUT pointer cleared
so the rasterizer skips the now-stale task. If the new `software_palette_lut_565`
field is added to `RenderTask` but not cleared here, a kernel that later looks
at `task->software_palette_lut_565` (when the gating bool indicates 565 mode)
would dereference a stale pointer to LUT565 memory that's still valid (the LUT
table itself isn't freed) but corresponds to an unlocked palette/texture. That
would render with the wrong palette colors, not crash — even worse from a debug
standpoint.

### P-1.4 — Step 4 cites wrong line for the input-history glyph NULL-out

Plan Step 4 says "input-history glyph site at `:12196-12200` (set to NULL)."

Reviewer read `sdl_game_renderer.c:12190-12200` — that range is inside an
`if (surface == NULL)` early-return that calls `draw_sprite_rect()`. There is no
`software_palette_lut = NULL` there. The actual NULL-out for the glyph path is
at `:12336`:

```
12335: #if INDEX8_RASTERIZATION_ENABLED
12336:     task->software_palette_lut = NULL;
12337:     task->software_source_is_index8 = false;
12338:     task->software_palette_is_binary_alpha = false;
12339: #endif
```

A fix-agent who follows the plan literally will edit a region that doesn't
have the assignment, or won't find it at all. Plan must correct to `:12336-12338`.

### P-1.5 — `cps3_canvas` location and format both wrong in §1

Plan §1 (line 36-38):

> `cps3_canvas` (`SDL_Texture`, created at `sdl_game_renderer.c:9362-9365`
> block, used by the desktop path) stays ARGB8888 — that path does not write
> into `software_frame_surface`.

Reviewer read `sdl_game_renderer.c:9439-9440`:

```
cps3_canvas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET, cps3_width, cps3_height);
```

Two errors: (a) location is `:9439-9440`, off by ~75 lines; (b) format is
`RGBA8888`, not `ARGB8888` (different byte order). The plan's assertion that
"that path does not write into software_frame_surface" is still correct, so
the bug is cosmetic for the swap itself, but anywhere downstream the plan
reasons about cps3_canvas behavior the format claim is wrong (R/B channels
swapped). Probably no behavioral consequence here, but a fix-agent reading the
plan as gospel will get a wrong mental model.

---

## P-2 — should fix

### P-2.1 — `SDL_assert(frame->pitch == 384 * 2)` is redundant and harmful

Plan §9 (line 213-216):

> Confirm at runtime by `SDL_assert(frame->pitch == 384 * 2)` at the call site.
> If on a future SDL version the pitch differs, the writer's row-by-row fallback
> handles it.

Reading `native_video_writer.c:76-93`:

```
if (pitch == NV_FRAME_WIDTH * 2) {
    memcpy((void*)dst, pixels_rgb565, NV_FRAME_BYTES);
} else {
    /* Row-by-row copy */
    for (int y = 0; y < NV_FRAME_HEIGHT; y++) {
        memcpy((void*)(dst + y * NV_FRAME_WIDTH * 2), src + y * pitch, …);
    }
}
```

The writer **already** handles both cases correctly. The plan's `SDL_assert`
converts a recoverable runtime difference into a hard fail in debug builds.
Either omit the assert entirely (the writer makes it redundant) or change to a
`SDL_assert_paranoid` / soft-log if you want telemetry on the unexpected case.
The plan acknowledges the writer can recover ("the writer's row-by-row fallback
handles it") but still proposes the assert anyway. Pick one.

### P-2.2 — Per-task `software_palette_lut_565` denormalizes a global into 1024 task slots

Plan §3 / Step 4 (line 720-726) picks strategy (a):

> The task does not currently carry a LUT565 pointer; either (a) thread it
> through `RenderTask` next to `software_palette_lut`, or (b) derive it from
> the same `palette_handle` the LUT8888 was looked up from. **Pick (a) — thread
> `task->software_palette_lut_565` through.**

The kernel already has the palette_handle in scope via
`task->texture_binding` (`HI_16_BITS(task->texture_binding)`), and the LUT565
table is a static array indexed by handle. Strategy (b) is `lut565[HI_16_BITS(task->texture_binding) - 1]`
— a cheap shift+mask+sub, then array indexing on a hot static array. The hot
path doesn't change. Strategy (a) bloats `RenderTask` by 8 bytes × 1024 tasks
= 8 KB of redundant denormalized state, and forces the fix-agent to wire the
new field through every write site (including the one missed at `:11641` per
P-1.3).

Item-1 made the right call there: it added a per-task `software_palette_is_binary_alpha`
**bool** (a property of the LUT, not redundantly derivable cheaply at the
kernel — the kernel doesn't have FL_PALETTE_MAX-indexed binary-α state cached
similarly). The 565 LUT pointer **is** cheaply derivable. Recommend strategy (b).

### P-2.3 — `surfaces_match` claim aside, 565 reference raster has the same Uint32-cast bug

Same root cause as P-1.1 but in `raster_reference_index8_loose` at
`software_frame_parity.c:412-493`. Plan Step 8 line 1135-1139:

> The existing function writes ARGB; extend it to detect `dst_surface->format`
> and write 565 pixels via `pack_rgb565_from_argb` …

Reviewer read `:446`:

```
const int dst_pitch = dst_surface->pitch / (int)sizeof(Uint32);
```

Plan must explicitly call out the pitch-divisor change for the 565 branch
(`/sizeof(Uint16)`) inside `raster_reference_index8_loose`, not just the store
type. Fix-agent reading "extend it to detect format and write 565 pixels" can
miss the pitch math.

### P-2.4 — `fill_argb8888_span` caller-count narrative contradicts itself

Plan §5 (line 122-130) opens with:

> The existing function has 4 callers verified by grep:

…lists 3 call sites…

> (No fourth caller — the brief said 4; grep shows 3 …)

…concludes:

> Both directly-callable sites need 565 variants if the canvas is 565.

So the same paragraph says 4, then 3, then 2 ("both directly-callable sites").
Reviewer grep confirms 3 callers (`:6859, :8494, :8553`) plus the def at
`:6778`. Plan should reword to "3 call sites in 3 distinct kernel functions"
and drop the contradictory "Both directly-callable sites" wording.

### P-2.5 — Convert-pass-skipped counter ownership

Open orchestrator question 5. Plan Step 7 (line 1064-1068) proposes a new
public setter `SDLGameRenderer_NoteConvertPassSkipped()` so `sdl_app.c` can
bump a counter that lives in `sdl_game_renderer.c`'s static struct.

Reviewer checked existing precedent: `sdl_app.c:2800-2801` only **reads** the
renderer's telemetry struct via `SDLGameRenderer_GetPerfCaptureRefreshTelemetry`;
no existing call from `sdl_app.c` writes into the renderer's counters. The
new setter is a one-off pattern. The cleaner alternative is to put the counter
in `sdl_app.c` next to where the convert decision is made (it's a present-path
decision, not a renderer decision). Emit it in `sdl_app.c`'s JSON block
directly, no renderer involvement. This avoids both the new public setter and
a header churn.

If the planner prefers to keep all perf-2 counters together in the renderer's
struct (which is a reasonable consistency argument), say so explicitly in the
rationale, and at minimum match the existing `RENDERER_TELEMETRY` macro form
rather than the placeholder `RENDERER_TELEMETRY_PRESENT` (line 1046 of plan)
which doesn't exist.

### P-2.6 — `blend_solid_rgb565` lossiness vs `blend_solid_argb8888` not documented

`blend_solid_argb8888` at `sdl_game_renderer.c:6795-6828` has both a
`dst_a == 255` fast path and a `dst_a < 255` generic path with an inverse
divide. Plan §3 step 3 (line 610-628) defines `blend_solid_rgb565` as the
255-only branch and casts away `src_a` ("solid path tracks dst α as opaque
always — see note").

Reviewer verified: in the existing renderer, `blend_argb8888_opaque_dst`
(`:6757-6776`) **always** returns `0xFF000000u | …`, so dst_a stays 255 across
chained blends *iff* every prior write goes through that primitive. But the
non-integer rasterizer's `blend_argb8888` (`software_frame_non_integer.c:68-103`)
is generic-α and **could** leave dst_a < 255 in the canvas. The 565 canvas
literally cannot represent that — α is dropped on every store. This is a
behavioral lossiness vs the 8888 path, not just a precision tradeoff.

The plan should either (a) document this loss explicitly in §6 (the modulation
section currently only discusses 565 quantization, not α-channel-loss), or
(b) confirm via runtime telemetry / parity that no canvas pixel ever ends up
with dst_a ≠ 255 in production (item-1's binary-α scan tells you per-palette,
but doesn't tell you the canvas state). Without one of these, the plan has a
silent-divergence risk on weird palette/blend combos that don't show up in
the strict/loose parity matrix.

### P-2.7 — `software_palette_lut_valid[]` line citation off

Orchestrator brief line 31 (paraphrased into plan §1's "Files to read first"
at Step 1) says `software_palette_lut_valid[]` is at `:177`. Actual location:
`sdl_game_renderer.c:182`. Five-line drift. Plan body at line 392 cites
`:178-186` for "existing `software_palette_lut`, `software_palette_lut_valid`,
`software_palette_lut_is_binary_alpha` storage" which is **correct for the
range** (decls at :181, :182, :185). So plan is internally consistent but
absorbed an off-by-5 hint from the brief. Minor; flag in case anyone leans
on the stale line.

### P-2.8 — `raster_full_height_diagonal_strip_to_software_frame` line range slightly off

Plan Step 6 (line 920-921) says `:6845-6883`. Reviewer read:

```
6830: static bool raster_full_height_diagonal_strip_to_software_frame(...)
6885: }
```

Function spans `:6830-6885`, not `:6845-6883`. Off by 15 lines on the start,
2 on the end. Fix-agent grep should land them on the right function regardless
("Confirm by grep — exact lines in current file" was added by planner) but
the cited range is wrong.

---

## OK / verified

The reviewer opened the source at every line the plan cited and confirmed:

- `software_frame_surface` decl at `:149`, creation at `:3387` — verified.
- `software_palette_lut[FL_PALETTE_MAX][256]` at `:181`,
  `software_palette_lut_is_binary_alpha[]` at `:185`,
  `build_software_palette_lut()` at `:9294-9323` — verified.
- `RENDERER_TELEMETRY` macro at `:44-53` — verified.
- `try_fast_copy_fast_textured_task_to_software_frame` signature at `:6128-6131`
  takes `dst_surface` parameter — verified, dispatch on
  `dst_surface->format` is feasible.
- INDEX8 color-mod at `:6160-6251`, loose-form non-color-mod at `:6260-6311`,
  fall-through at `:6312-6378`, scaled at `:6381-6430` — verified.
- `blend_argb8888_opaque_dst` at `:6757-6776`, `fill_argb8888_span` at
  `:6778-6794`, `blend_solid_argb8888` at `:6795-6828` — verified.
- `modulate_argb8888_blue_tint` at `:6687-6701` — verified.
- `sa_bg_cache_restore_background_scaled` at `:3630-3663` reads/writes
  `Uint32*` at `:3643/:3644` — verified.
- `sa_bg_cache_surface` creation at `:3413` (ARGB8888) — verified.
- Background memcpy paths at `:3732-3733` and `:8736-8738` are byte-count
  driven — verified format-agnostic.
- ARGB8888 fast-copy paths at `:6439-6622` and scaled at `:6625-6657` — verified.
- Triangle/parallelogram functions at `:7850, :7964, :8057, :8133, :8211,
  :8418, :8506` — verified.
- `software_frame_non_integer.c:452` is the start of
  `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888`, file ends at `:716` —
  verified.
- `convert_argb8888_to_rgb565` at `sdl_app.c:186-226`,
  `native_video_rgb565_scratch[384*224]` at `:184` — verified.
- Present-path call site at `sdl_app.c:10525-10551` — verified.
- `apply_rasterized_fps_overlay_to_argb_buffer` at `fbdev_presenter.c:1169-1186`,
  `FBDevPresenter_ApplyFPSOverlayToBuffer` at `:2538-2589`, header decl at
  `fbdev_presenter.h:104` — verified.
- `NativeVideoWriter_WriteFrame` at `native_video_writer.c:76-107` already
  handles both contiguous and row-by-row pitch — verified.
- `CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED` at `config.h:35`,
  setter decl at `sdl_game_renderer.h:736`,
  startup wiring at `sdl_app.c:9988-9990` — verified, plan's mirror pattern
  is faithful.
- `SDLGameRenderer_PerfCaptureRefreshTelemetry` struct at
  `include/port/sdl/sdl_game_renderer.h:210-245`, item-1 fields at `:242-244`
  — verified.
- JSON emission template at `sdl_app.c:6068-6073` — verified.
- `FL_PALETTE_MAX = 1088` at `include/sf33rd/AcrSDK/ps2/foundaps2.h:10` —
  verified; plan's 0.56 MB calculation is correct.
- Asserts at `:2899` and `:3233` target source / texture-cache surfaces, NOT
  the canvas — verified, plan's "no edit needed" call is correct.
- `vst1q_u16` precedent at `sdl_app.c:206` — verified (relevant if NEON 565
  follow-up lands; this plan correctly defers).
- Item-1 parity shim at `:12378-12411` accepts `SDL_Surface* dst_surface`,
  doesn't itself assert format — verified, plan's reuse claim for the shim
  itself is fine (the bug is in `surfaces_match` and the reference raster,
  P-1.1 / P-2.3, not the shim).
- All 3 caller sites of `fill_argb8888_span` (`:6859, :8494, :8553`) and 3 of
  `blend_solid_argb8888` (`:6881, :8497, :8563`) — verified count.
- Plan §3's enumeration of files-to-touch (parallelogram, float-parallelogram,
  triangle trio, solid-triangle, solid-task, diagonal-strip, non-integer,
  sa_bg scaled-restore) — covers every Uint32-write into
  `software_frame_surface->pixels` that reviewer grep found.
- Item-1 commits `7b0d9dba` and `4d58c909` are in tree on the `perf` branch.
  Plan's mirroring of item-1's pattern (config key + bool gate + setter +
  startup wiring + parity shim + telemetry counters + JSON emission) is
  structurally faithful.

---

## Summary numbers

- P-1: 5
- P-2: 8
- OK: ~30 spot-checks

## Headline issue

**P-1.2 (silent corruption on fbdev fallback) is the load-bearing one.** The
plan defaults the 565 canvas ON for PORT_MISTER and explicitly tells the
fix-agent NOT to change `software_frame_upload_texture`'s ARGB8888 format. If
the native_video_writer ever fails to init (env var, missing FPGA, future
core change), the upload path runs anyway and reinterprets 565 bytes as 8888.
Either gate the 565 default on writer-availability or make the upload texture
format-aware. P-1.1 (parity test reads past row end) is a near-second — it
would let buggy 565 kernels pass parity for nonsense reasons.
