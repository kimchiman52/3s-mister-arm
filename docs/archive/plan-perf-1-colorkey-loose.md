# Plan — perf-1 — Loose-form binary-α color-key fast path for INDEX8 raster kernels

> **ARCHIVED 2026-08-30** — true at a752e2ca, not maintained.
> Read for rationale and for what was tried and failed. Do not read for current facts; read the code.


**Worktree:** `/Users/sb/Developer/3sx-mister-perf` (branch `perf`)
**Source-of-truth:** all paths in this plan are relative to that worktree.
**Background:** see `/Users/sb/Developer/3sx-mister/docs/research-renderer-external-comparison.md`,
especially §2.6, §2.7, §2.9, §2.10, §4.2, §6 item #1. Telemetry from a representative gameplay
session showed 854/854 live palettes had only α∈{0,255} and 854/854 had `palette[0].α==0`.

## Goal

Replace the per-pixel `if (a==0) skip; else if (a==0xFF) overwrite; else blend;` triplet in the
INDEX8 fast-path kernels at `src/port/sdl/sdl_game_renderer.c:6121-6313` with the **loose** form:

```
if ((palette_lut[i] >> 24) == 0) skip; else *dst = palette_lut[i];
```

This is correct for any palette where every entry has α∈{0,255}, regardless of how many
zero-α entries there are or where they sit in the LUT. The strict form (`if (i == 0) skip`,
which assumes only `palette[0]` is transparent) is **out of scope** of this plan; this plan is
the safer, more general first integration.

## Out of scope (do not touch in this plan)

- RGB565 canvas mode (separate plan).
- Strict-ckey kernel and packed-RGB565 store.
- NEON scaled-INDEX8 kernel.
- INDEX4LSB rasterizer / INDEX4 fast paths.
- Removing the existing telemetry instrumentation patches (palette histogram, format census).
- Changing or removing the existing α=0/α=0xFF/blend kernels in the ARGB8888 branches at
  `sdl_game_renderer.c:6317-6420+`, the parallelogram raster, or the triangle fallback.

## Files to change (master list)

- `src/port/sdl/sdl_game_renderer.c` — palette-binary-α detection state, new kernels,
  gating bool, telemetry counter increments, runtime setter.
- `include/port/sdl/sdl_game_renderer.h` — public setter + telemetry-struct field additions.
- `src/port/config/config.h` — new `CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED` define (step 2).
- `src/port/sdl/sdl_app.c` — config-driven setter call at startup; perf-capture JSON
  emission of new counters; reset call.
- `src/port/sdl/software_frame_parity.c` — INDEX8 fast-path parity coverage.
- `docs/plan-perf-1-colorkey-loose.md` — this plan (already exists once committed).

The implementation is **scalar-first**, then NEON. No header rename. No new translation units.

## Design decisions

### 1. Eligibility detection: scan-once at LUT-build time (Option A)

Compute a per-palette `software_palette_lut_is_binary_alpha[FL_PALETTE_MAX]` bool while the LUT
is being built in `build_software_palette_lut()` at `sdl_game_renderer.c:9177-9193`. The scan
adds 256 byte reads after the existing 256-entry build loop and runs only when a palette is
unlocked, not per-pixel. We do **not** rely on the universal-binary-α observation as a runtime
assumption; if a future palette format change lands, the bool flips off and the fast path is
disabled for that palette. The RenderTask carries a `software_palette_is_binary_alpha` pointer
or bool alongside the existing `software_palette_lut` pointer (defined at
`sdl_game_renderer.c:97-100`).

### 2. Identity-modulation interaction

The existing exact-copy fast path at `sdl_game_renderer.c:6121-6285` already separates two
sub-branches:
- `if (plan->color_mod) { ... }` — color-mod (must blend; modulation can produce α∈(0,255)).
- The non-color-mod branch starting at `sdl_game_renderer.c:6224` ("INDEX8 non-color-mod
  exact copy").

`color_mod` is set in `try_setup_software_frame_fast_copy_plan()` at line 5576 (`task->color
!= 0xFFFFFFFFu`). The loose-form fast path lives **inside the existing non-color-mod branch
only** — it does not affect the color-mod branch. This keeps the patch scope bounded and
avoids wrestling with the modulation-α ∈ (0,255) edge case.

For the scaled path at `sdl_game_renderer.c:6288-6313`, the `try_setup_software_frame_fast_copy_plan()`
return at line 5577-5579 already excludes color-mod scaled (`COLOR_MOD` result). So the scaled
path is non-color-mod by construction; the fast-path swap is unconditional within that branch.

### 3. Flip handling

The existing exact-copy uses `src_x_step` and `src_y_step` (see lines 6125-6128 verbatim:
`const int src_x_step = plan->flip_h ? -1 : 1;` etc.). The fast path must continue to handle
both flips. Per the existing pattern (lines 6225-6258), the **NEON branch only fires when
`src_x_step == 1`** (forward H scan). We mirror that: NEON loose-form fast path is also gated on
`src_x_step == 1`; flipped scan falls through to the scalar loose-form kernel. Scalar loose-form
kernel handles both flips by reusing the existing `src_x_step` variable.

### 4. Prefetch

The existing kernels use `__builtin_prefetch(...)` for src and dst rows (e.g. lines 6152-6153,
6231-6233, 6266-6268, 6300-6302). The new loose-form kernels MUST replicate this — same
distance (one row ahead), same args (`(0, 0)` for read, `(1, 0)` for write).

### 5. NEON variant for unscaled non-color-mod

Existing NEON inner loop pattern (lines 6237-6248):

```
for (; (col + 3) < plan->visible_w; col += 4) {
    const Uint32 gathered[4] = {
        palette_lut[i8_row[src_row0_x + col]],
        palette_lut[i8_row[src_row0_x + col + 1]],
        palette_lut[i8_row[src_row0_x + col + 2]],
        palette_lut[i8_row[src_row0_x + col + 3]]
    };
    if (((gathered[0] | gathered[1] | gathered[2] | gathered[3]) >> 24) == 0u) {
        continue;
    }
    neon_blend_4pixels(gathered, dst_row + col);
}
```

The loose-form NEON kernel keeps the 4-px gather (no widening — A9 has no `vqtbl1q`, see
research §7), keeps the all-4-α-zero short-circuit (still cheap and now even more often
useful), and **replaces `neon_blend_4pixels` with a per-lane select**: for each lane, if α==0
keep dst, else write src. We can express this as a `vld1q_u32` of dst, `vld1q_u32` of the
gathered src, an alpha mask `vshrq_n_u32` then `vtstq_u32` against zero (or `vceqq_u32` against
0), and a `vbslq_u32(mask, dst, src)`. Final `vst1q_u32(dst, result)`. No multiply, no
`(c * m + 128) >> 8`, no source-over math. This is strictly cheaper than `neon_blend_4pixels`.

We do **not** introduce a wider 8-px or 16-px NEON kernel. A9's load/store pipe and 32 KB L1 D
make 4-px/iter the established sweet spot here; widening must be justified separately by
measurement, not asserted up front.

Helper signature: `static inline void neon_index8_loose_4pixels(const Uint32* src, Uint32* dst);`
Defined adjacent to `neon_blend_4pixels` near `sdl_game_renderer.c:5940`.

### 6. Scaled fast path

The existing scaled scalar kernel at lines 6296-6312 reads palette via `i8_row[src_x_lookup[col]]`.
Loose-form swap: the inner triplet at lines 6307-6310

```
const Uint32 src_a = (src_pixel >> 24) & 0xFFu;
if (src_a == 0u) { continue; }
if (src_a == 0xFFu) { dst_row[col] = src_pixel; continue; }
dst_row[col] = blend_argb8888_opaque_dst(dst_row[col], src_pixel);
```

becomes

```
if ((src_pixel >> 24) == 0u) { continue; }
dst_row[col] = src_pixel;
```

No NEON variant for the scaled path — out of scope (research §6 item 4 is a separate
deprioritised follow-up).

### 7. Gating mechanism: runtime bool + `config.ini` key, default ON

We add a single runtime toggle:

```c
static bool colorkey_loose_kernel_enabled = true;
void SDLGameRenderer_SetColorkeyLooseKernelEnabled(bool enabled);
```

declared in `include/port/sdl/sdl_game_renderer.h` next to the existing
`SDLGameRenderer_SetPerfCaptureLogicalIdentityEnabled` family
(around `sdl_game_renderer.h:725-733`), and defined adjacent to the existing setters at
`sdl_game_renderer.c:9382-9410`. Default ON so perf testing in normal release/telemetry builds
just runs the new kernel.

The setter is wired through `config.ini` via a new `CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED`
key (`config.h:6-22` follows a uniform `#define` pattern). On startup, `sdl_app.c` calls
`SDLGameRenderer_SetColorkeyLooseKernelEnabled(Config_GetBool(CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED))`
gated on `Config_HasExplicitKey(...)` so the default-ON behavior persists when the key is
absent. Same pattern as `Config_GetBool(CFG_KEY_FULLSCREEN)` at `sdl_app.c:9807`. This is a
~10-line addition spread across `config.h` + `sdl_app.c`; cost is one branch outside the
inner loop.

**Rationale for runtime over compile-time**: the user feedback memo `feedback-debug-build-for-live-tests.md`
says "ship a DEBUG-flavor binary with diagnostics on" — i.e. A/B must be reachable without a
rebuild. Telemetry counters are also more useful when both A and B paths are reachable in
the same binary. Step 7's baseline strategy uses the saved baseline binary at
`~/Developer/3sx-mister-baselines/2026-04-25-telemetry-instrumented/3s-arm` for the "before"
capture and the runtime toggle for any same-binary sanity-check captures.

### 8. Telemetry counters

Three counters added to `SDLGameRenderer_PerfCaptureRefreshTelemetry` at
`include/port/sdl/sdl_game_renderer.h:210-242`:

- `Uint64 colorkey_loose_fast_path_hits;`     — fast path actually ran
- `Uint64 colorkey_loose_fast_path_skipped;`  — palette was binary-α but kernel disabled
- `Uint64 colorkey_loose_fast_path_ineligible;` — palette was NOT binary-α (full-α blend stayed)

Increments use the existing `RENDERER_TELEMETRY(...)` macro (defined at
`sdl_game_renderer.c:44-53`) wrapped around the new kernel entry/decision points so they
compile out of release-non-telemetry builds. JSON emission added in `sdl_app.c` next to the
existing `software_surface_cache_refresh_source_formats` block at `sdl_app.c:5929-5954` — same
formatting style. Storage already lives in the static
`perf_capture_refresh_telemetry` instance at `sdl_game_renderer.c:248`. The existing reset at
`sdl_game_renderer.c:9581-9582` (zeroing via `SDL_zero`) covers the new fields automatically.

### 9. Correctness verification

**Automated** (perf-worktree host build):

Add INDEX8 cases to `src/port/sdl/software_frame_parity.c`. The file already has:

- A `fill_index8_test_source()` helper at line 208.
- A `fill_index8_test_palette()` helper at line 219 (currently fills α=255 for every entry —
  we extend it to also produce a **binary-α** variant with `α=0` for index 0 only and a
  **mixed-binary-α** variant with several scattered α=0 indices).
- A `run_software_source_refresh_parity_check()` at line 245 — refresh-only, not raster.
- Pixel-comparison `surfaces_match()` at line 183.

The current top-level `SDLGameRenderer_RunSoftwareFrameParityCheck()` at line 344 only exercises
the **non-integer** ARGB8888 path (`SDLSoftwareFrame_RasterNonIntegerLookupARGB8888`). It does
**not** today exercise the INDEX8 exact-copy or scaled-copy fast paths. So step 6 must add a
new helper `run_index8_fast_path_parity_check()` that:

1. Builds an INDEX8 surface + binary-α palette via the existing helpers (extended).
2. Builds a reference ARGB8888 surface by `SDL_BlitSurface(index8 -> argb8888)` (SDL converts
   via the palette; this is the ground truth).
3. Runs the new kernel via `try_fast_copy_fast_textured_task_to_software_frame()` against an
   ARGB8888 destination of the same shape.
4. Compares pixel-by-pixel via `surfaces_match()`.

Cases must include: unscaled forward, unscaled flip-h, unscaled flip-v, unscaled flip-both,
scaled-up, scaled-down, clipped-top-left, clipped-bottom-right. Mixed-binary-α variant must be
covered for at least one case so the scanner is exercised.

The `try_fast_copy_fast_textured_task_to_software_frame()` function at `sdl_game_renderer.c:6098`
is `static`; the parity helper either (a) reaches it via a small new
non-`static` shim (`SDLGameRenderer_RunIndex8FastPathParityCheckInternal`) declared in
`sdl_game_renderer.h` and called from `software_frame_parity.c`, or (b) the parity helper is
moved into `sdl_game_renderer.c` near the function. **Pick option (a) (small shim).**
`software_frame_parity.c` already operates at the public-API layer — its existing call to
`SDLSoftwareFrame_RasterNonIntegerLookupARGB8888` (line 390) is a public function exported
from `software_frame_non_integer.h`. The shim continues that pattern: parity test stays in
its own TU, the kernel under test gets a thin public surface for testability. Moving the
parity test into `sdl_game_renderer.c` would regress code locality.

The shim must populate **all three** task fields the kernel reads — `software_source_is_index8`,
`software_palette_lut`, **and** `software_palette_is_binary_alpha` (added in step 1). See
step 6 for the explicit init block.

**Manual smoke** (on device):

1. Deploy via `tools/mister/build-game.sh --flavor telemetry` and existing deploy helper.
2. Launch a short play session covering: char-select, super-art burst (Yun SA3), a stage with
   scaled BG (Genei-Jin), a stage with scrolling background.
3. Confirm no visible color-banding, no missing transparent pixels, no halos around sprites.
4. Pull `perf_capture.json`. Verify `colorkey_loose_fast_path_hits > 0` and
   `colorkey_loose_fast_path_ineligible == 0` (all palettes were binary-α), matching the
   research observation.
5. (Optional A/B) call the runtime toggle to disable the kernel; confirm visuals remain
   identical and `colorkey_loose_fast_path_skipped` increments.

## Rollback plan

If the kernel ships and breaks something:

1. **Fastest (no rebuild):** add `colorkey-loose-kernel-enabled = false` to the device's
   `config.ini` (step 2 plumbs the runtime config key). Restart the game. The kernel is
   now disabled; old behavior restored.
2. **Slower but cleanest:** revert the commits from steps 2–4 (kernel + NEON + scaled). Steps 1
   (eligibility scanner), 5 (telemetry), and 6 (parity test) are independently safe to keep —
   the scanner is dormant without callers, the counters stay zero, and the parity test
   continues to validate the existing code paths through the new shim.
3. **Compile-time fallback:** flip the default of `colorkey_loose_kernel_enabled` from `true`
   to `false` at `sdl_game_renderer.c` (one-line change). Rebuild + redeploy.

## Steps

Each step below is one commit, executable independently by `/implement` without requiring
follow-up clarification.

---

### Step 1 — Add palette-binary-α scanner + RenderTask plumbing

**Title:** Add `software_palette_lut_is_binary_alpha[]` + per-task carry-through.

**Why:** Eligibility detection must run once when a palette is built, not per-pixel. Carrying
the bool on the `RenderTask` keeps the inner loop's branch on a register-cached value, not a
table lookup.

**Files to read first:**
- `src/port/sdl/sdl_game_renderer.c:170-178` — existing `software_palette_lut_valid[]`.
- `src/port/sdl/sdl_game_renderer.c:9177-9193` — `build_software_palette_lut()`.
- `src/port/sdl/sdl_game_renderer.c:89-108` — `RenderTask` struct (fields
  `software_palette_lut` at 98, `software_source_is_index8` at 99 inside
  `#if INDEX8_RASTERIZATION_ENABLED`).
- `src/port/sdl/sdl_game_renderer.c:530-540` — `current_software_palette_lut` global at
  line 536 (the per-binding "live" pointer that the four RenderTask sites copy from).
- `src/port/sdl/sdl_game_renderer.c:11680-11695` — the SetTexture/SetPalette block that
  binds the **globals** `current_software_palette_lut` / `current_software_source_is_index8`.
  This is **not** a RenderTask write site; this is the per-binding global update.
- `src/port/sdl/sdl_game_renderer.c:11500-11510, 11720-11730, 12030-12040, 12190-12200` —
  the **four** RenderTask write sites for `software_palette_lut`.

**Files to modify:**
- `src/port/sdl/sdl_game_renderer.c`:
  - Add `static bool software_palette_lut_is_binary_alpha[FL_PALETTE_MAX] = { false };` next to
    `software_palette_lut_valid[]` at line 177.
  - In `build_software_palette_lut()`, after the existing build loop at lines 9184-9191, add a
    second pass that scans the 256 entries: for each `lut[i]`, extract `(lut[i] >> 24) & 0xFFu`,
    if it is anything other than `0` or `0xFFu`, mark `is_binary_alpha = false` and break.
    Default `is_binary_alpha = true` before scan.
  - Add a per-binding global mirror next to the existing `current_software_palette_lut`
    at line 536: `static bool current_software_palette_is_binary_alpha = false;`.
  - In the SetTexture/SetPalette binding block at line 11686, after
    `current_software_palette_lut = software_palette_lut[palette_handle - 1];`, add
    `current_software_palette_is_binary_alpha = software_palette_lut_is_binary_alpha[palette_handle - 1];`.
    In the matching `else` branch at line 11693, add
    `current_software_palette_is_binary_alpha = false;`. The other `current_software_palette_lut = NULL;`
    sites at lines 1084, 1128, 1172, 1217, 1255, 9338 must each get a paired
    `current_software_palette_is_binary_alpha = false;`.
  - Add `bool software_palette_is_binary_alpha;` to `RenderTask` (line 89-108) inside the
    existing `#if INDEX8_RASTERIZATION_ENABLED` block, immediately after
    `software_source_is_index8` at line 99.
  - At **all four** RenderTask `software_palette_lut` write sites, also set the new bool
    using the same condition as the existing `task->software_source_is_index8` line
    immediately below it (so the bool follows the same lifecycle):
    1. **`sdl_game_renderer.c:11506`** (texture-destroy invalidation path): add
       `render_tasks[i].software_palette_is_binary_alpha = false;` next to the
       `render_tasks[i].software_palette_lut = NULL;` clear. Required to avoid carrying
       a stale `true` from a previous frame's task-slot reuse (`render_tasks` is a
       1024-entry static array reused across frames, line 40 of file).
    2. **`sdl_game_renderer.c:11725`** (`begin_quad_task`): add
       `task->software_palette_is_binary_alpha = textured && current_software_palette_is_binary_alpha;`
       paired with the existing `task->software_palette_lut = textured ? current_software_palette_lut : NULL;`.
    3. **`sdl_game_renderer.c:12035`** (inline mtrans path): add
       `task->software_palette_is_binary_alpha = current_texture_binding_valid && current_software_palette_is_binary_alpha;`
       paired with the existing `task->software_palette_lut = current_texture_binding_valid ? current_software_palette_lut : NULL;`.
    4. **`sdl_game_renderer.c:12196`** (input-history glyph path, sets to NULL): add
       `task->software_palette_is_binary_alpha = false;` paired with the existing
       `task->software_palette_lut = NULL;`.

**Success criteria:**
- Build passes (`tools/mister/build-game.sh --flavor telemetry`).
- `grep -n software_palette_lut_is_binary_alpha src/port/sdl/sdl_game_renderer.c` shows
  the array decl, the scanner update, and the SetTexture/SetPalette binding lookup.
- `grep -n 'task->software_palette_lut\s*=\|render_tasks\[.*\]\.software_palette_lut\s*=' src/port/sdl/sdl_game_renderer.c`
  returns the four sites at lines 11506, 11725, 12035, 12196 — and each line should be
  immediately adjacent (within ±2 lines) to a corresponding
  `software_palette_is_binary_alpha` write. Verify by re-running
  `grep -n 'software_palette_is_binary_alpha\s*=' src/port/sdl/sdl_game_renderer.c` and
  confirming a mirror write exists for each of the four sites.
- Render output is unchanged (kernel hasn't been touched yet).

**Dependencies:** none.

**What NOT to do:**
- Do **not** change any rasterizer kernel in this step.
- Do **not** add a setter or telemetry counter yet (steps 5 and 7 do that).
- Do **not** scan in any hot path other than `build_software_palette_lut()`. The scan is
  amortized over palette lifetime; a per-frame or per-bind scan would defeat the purpose.

**What to do if it fails:**
- If the build breaks because a call site for `task->software_palette_lut` was missed, run
  `grep -n 'task->software_palette_lut\s*=\|render_tasks\[.*\]\.software_palette_lut\s*='
  src/port/sdl/sdl_game_renderer.c` — every line in the output must have a paired
  `software_palette_is_binary_alpha` assignment immediately adjacent.
- If the runtime later observes `colorkey_loose_fast_path_hits` going up but visual
  artifacts appear only after a frame where a texture was destroyed, the destroy-path
  clear at line 11506 is missing — task-slot reuse will carry stale `true`.

---

### Step 2 — Add scalar loose-form INDEX8 exact-copy kernel + runtime gate + config key

**Title:** Loose-form fast path for INDEX8 unscaled non-color-mod (scalar) under
`colorkey_loose_kernel_enabled`, with runtime setter and `config.ini` plumb-through.

**Why:** This is the highest-traffic INDEX8 raster path (research §2.8, §2.9). Replacing the
α=0/α=0xFF/blend triplet with `α==0?skip:store` removes one compare and the (never-taken)
blend branch entirely on every pixel. The runtime setter is wired through `config.ini` so
that A/B comparisons (kernel ON vs OFF) do not require a rebuild — see
`feedback-debug-build-for-live-tests.md`. Default is ON when the key is absent.

**Files to read first:**
- `src/port/sdl/sdl_game_renderer.c:6224-6285` — current INDEX8 non-color-mod exact-copy
  scalar kernel (with flip support).
- `src/port/sdl/sdl_game_renderer.c:5575-5576` — `color_mod = task->color != 0xFFFFFFFFu`.
- `src/port/sdl/sdl_game_renderer.c:9382-9410` — existing setter pattern.
- `src/port/config/config.h:6-22` — existing `CFG_KEY_*` defines.
- `src/port/config/config.h:51-62` — `Config_GetBool` and `Config_HasExplicitKey` signatures.
- `src/port/sdl/sdl_app.c:9970-9985` — startup block where `SDLGameRenderer_*` setters are
  called immediately after `SDLGameRenderer_Init(renderer)` (lines 9977-9981).
- `src/port/sdl/sdl_app.c:9807` — `Config_GetBool(CFG_KEY_FULLSCREEN)` precedent for a
  default-aware boolean read.

**Files to modify:**
- `src/port/config/config.h`:
  - Add `#define CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED "colorkey-loose-kernel-enabled"`
    next to the other `CFG_KEY_*` block at line 22.
- `src/port/sdl/sdl_game_renderer.c`:
  - Add `static bool colorkey_loose_kernel_enabled = true;` near the other static bool toggles
    at line 168-170.
  - Add the public setter `SDLGameRenderer_SetColorkeyLooseKernelEnabled(bool enabled)` after
    the last existing `Set...Enabled` setter near line 9410.
- `src/port/sdl/sdl_app.c`:
  - In the startup block at line 9981 (after `SDLGameRenderer_SetSABgCacheFramesRemaining(0);`),
    add: `if (Config_HasExplicitKey(CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED)) {
    SDLGameRenderer_SetColorkeyLooseKernelEnabled(Config_GetBool(CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED));
    }`. The `HasExplicitKey` guard preserves the default-ON behavior when the key is absent
    from `config.ini`.
- `src/port/sdl/sdl_game_renderer.c` (kernel insertion, continued):
  - Inside `try_fast_copy_fast_textured_task_to_software_frame()` at line 6098, locate the
    non-color-mod exact-copy branch starting at line 6224 (`/* INDEX8 non-color-mod exact copy */`).
    **Above** the existing NEON `#if RENDERER_HAVE_NEON` block at line 6225, insert:
    ```c
    if (colorkey_loose_kernel_enabled && task->software_palette_is_binary_alpha) {
        /* Scalar loose-form fast path. Handles both flip directions because
           src_x_step is reused. Mirrors prefetch pattern of the existing kernel. */
        for (int row = 0; row < plan->visible_h; row++) {
            const Uint8* i8_row = index8_src_row(i8_surface, src_row0_y + (row * src_y_step));
            Uint32* dst_row = dst_pixels + ((plan->dst_y0 + row) * dst_pitch) + plan->dst_x0;
            if ((row + 1) < plan->visible_h) {
                __builtin_prefetch(index8_src_row(i8_surface, src_row0_y + ((row + 1) * src_y_step)), 0, 0);
                __builtin_prefetch(dst_pixels + ((plan->dst_y0 + row + 1) * dst_pitch) + plan->dst_x0, 1, 0);
            }
            int src_x = src_row0_x;
            for (int col = 0; col < plan->visible_w; col++) {
                const Uint32 src_pixel = palette_lut[i8_row[src_x]];
                if ((src_pixel >> 24) != 0u) { dst_row[col] = src_pixel; }
                src_x += src_x_step;
            }
        }
        return true;
    }
    ```
- `include/port/sdl/sdl_game_renderer.h`:
  - Declare `void SDLGameRenderer_SetColorkeyLooseKernelEnabled(bool enabled);` near line 725.

**Success criteria:**
- Build passes telemetry flavor.
- With kernel enabled (default — no config key set), gameplay is visually identical to the
  previous build (verified later in step 7 / on-device smoke).
- Setting `colorkey-loose-kernel-enabled = false` in `config.ini` restores the old behavior
  on next launch (no rebuild needed).
- `grep -n CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED src/port/` returns hits in both
  `config.h` and `sdl_app.c`.

**Dependencies:** Step 1 (uses `task->software_palette_is_binary_alpha`).

**What NOT to do:**
- Do **not** touch the color-mod branch at `sdl_game_renderer.c:6130-6222`. Color-mod must keep
  the full blend.
- Do **not** touch the NEON branch yet — step 3 owns that.
- Do **not** drop the existing fallback kernel; the new kernel **early-returns** above it,
  the existing kernel stays as the fallback for `!enabled` and `!is_binary_alpha`.

**What to do if it fails:**
- If pixels mismatch on flip cases, double-check `src_x_step` is being added inside the inner
  loop *after* the read (the original kernel reads first then steps; mirror that).
- If pixels mismatch only when palette is **mixed-binary-α** (some α=0 indices not at index 0),
  this confirms the loose vs strict distinction matters; the kernel as written here is correct
  — bug must be elsewhere.

---

### Step 3 — Add NEON loose-form INDEX8 exact-copy kernel

**Title:** NEON 4-px/iter loose-form fast path for INDEX8 unscaled non-color-mod, `src_x_step==1`.

**Why:** The NEON branch covers the highest-frequency `flip == FORWARD` case. Replacing
`neon_blend_4pixels` with a per-lane α-zero select removes the multiplies and source-over
math from every block.

**Files to read first:**
- `src/port/sdl/sdl_game_renderer.c:5940-6012` — `neon_blend_4pixels` definition for style.
- `src/port/sdl/sdl_game_renderer.c:6225-6258` — current NEON INDEX8 non-color-mod exact copy.

**Files to modify:**
- `src/port/sdl/sdl_game_renderer.c`:
  - Add a new helper `static inline void neon_index8_loose_4pixels(const Uint32* src, Uint32* dst)`
    next to `neon_blend_4pixels` at line 5940. Body sketch (final code MUST be verified to
    compile under `-march=armv7-a -mfpu=neon -mfloat-abi=hard`):
    ```c
    const uint32x4_t src_v = vld1q_u32(src);
    const uint32x4_t dst_v = vld1q_u32(dst);
    /* lane mask = 0xFFFFFFFF where (src_v >> 24) == 0, else 0 */
    const uint32x4_t alpha_v = vshrq_n_u32(src_v, 24);
    const uint32x4_t keep_dst_mask = vceqq_u32(alpha_v, vdupq_n_u32(0));
    /* keep_dst_mask=1 -> dst_v, else src_v */
    vst1q_u32(dst, vbslq_u32(keep_dst_mask, dst_v, src_v));
    ```
  - Above the existing NEON block at `sdl_game_renderer.c:6225`, in the **same**
    "above the existing NEON branch" position used for the scalar in step 2, add the NEON
    loose-form branch:
    ```c
#if RENDERER_HAVE_NEON
    if (colorkey_loose_kernel_enabled && task->software_palette_is_binary_alpha &&
        src_x_step == 1) {
        for (int row = 0; row < plan->visible_h; row++) {
            const Uint8* i8_row = index8_src_row(i8_surface, src_row0_y + (row * src_y_step));
            Uint32* dst_row = dst_pixels + ((plan->dst_y0 + row) * dst_pitch) + plan->dst_x0;
            if ((row + 1) < plan->visible_h) {
                __builtin_prefetch(index8_src_row(i8_surface, src_row0_y + ((row + 1) * src_y_step)), 0, 0);
                __builtin_prefetch(dst_pixels + ((plan->dst_y0 + row + 1) * dst_pitch) + plan->dst_x0, 1, 0);
            }
            int col = 0;
            for (; (col + 3) < plan->visible_w; col += 4) {
                const Uint32 gathered[4] = {
                    palette_lut[i8_row[src_row0_x + col]],
                    palette_lut[i8_row[src_row0_x + col + 1]],
                    palette_lut[i8_row[src_row0_x + col + 2]],
                    palette_lut[i8_row[src_row0_x + col + 3]]
                };
                /* All-α-zero short-circuit (matches existing pattern) */
                if (((gathered[0] | gathered[1] | gathered[2] | gathered[3]) >> 24) == 0u) {
                    continue;
                }
                neon_index8_loose_4pixels(gathered, dst_row + col);
            }
            /* Scalar tail */
            for (; col < plan->visible_w; col++) {
                const Uint32 src_pixel = palette_lut[i8_row[src_row0_x + col]];
                if ((src_pixel >> 24) != 0u) { dst_row[col] = src_pixel; }
            }
        }
        return true;
    }
#endif /* RENDERER_HAVE_NEON */
    ```

  - The order of branches inside the non-color-mod exact-copy block becomes:
    1. NEON loose-form (this step) — `enabled && is_binary_alpha && src_x_step == 1`
    2. Scalar loose-form (step 2) — `enabled && is_binary_alpha`
    3. Existing NEON `neon_blend_4pixels` kernel — unchanged
    4. Existing scalar fallback — unchanged

**Success criteria:**
- Telemetry build links. NEON intrinsics compile without warnings.
- Pixel parity test (added in step 6) passes.
- On x86 host (where `RENDERER_HAVE_NEON==0`), the NEON branch is `#if`'d out and the scalar
  branch covers the case.

**Dependencies:** Step 2.

**What NOT to do:**
- Do **not** widen to an 8-px or 16-px iter — A9 is not the place. (Research §7 explicitly.)
- Do **not** introduce a `vld4`/`vst4` deinterleave — irrelevant for this kernel; we have
  packed `Uint32` already.
- Do **not** drop the existing all-α-zero short-circuit `if` before the helper. It's a free
  win for empty regions of sprites.

**What to do if it fails:**
- If `vbslq_u32` argument order looks wrong, do **not** reason about it from the toy
  example — re-read the in-tree precedent at `sdl_game_renderer.c:5999-6006`
  (`mask_zero = vceqq_u8(alpha_full, zero_vec)` followed by
  `result = vbslq_u8(mask_zero, dst_bytes, result)`). The ARM convention is
  `vbslq_uXX(mask, a, b)` returning lane-wise `(mask & a) | (~mask & b)` — lanes where the
  mask is set take from `a`. In our kernel, `keep_dst_mask` is set in the α==0 lanes, so
  argument `a` must be `dst_v` and argument `b` must be `src_v`. This mirrors the existing
  `vbslq_u8(mask_zero, dst_bytes, result)` pattern at line 6004 exactly.
- If a build target lacks `arm_neon.h`: the existing `#if RENDERER_HAVE_NEON` guard should
  already cover it.

---

### Step 4 — Add scalar loose-form INDEX8 scaled fast path

**Title:** Loose-form fast path for INDEX8 scaled (non-exact, non-color-mod) — scalar only.

**Why:** Scaled is a smaller hit-rate than exact-copy but non-trivial for stage backgrounds.
The scalar kernel is straightforward to swap.

**Files to read first:**
- `src/port/sdl/sdl_game_renderer.c:6288-6313` — current INDEX8 scaled scalar kernel.
- `src/port/sdl/sdl_game_renderer.c:5575-5579` — proof scaled implies !color_mod.

**Files to modify:**
- `src/port/sdl/sdl_game_renderer.c`: above line 6296 (`for (int row = ...`), insert:
  ```c
  if (colorkey_loose_kernel_enabled && task->software_palette_is_binary_alpha) {
      for (int row = 0; row < plan->visible_h; row++) {
          const Uint8* i8_row = index8_src_row(i8_surface, src_y_lookup[row]);
          Uint32* dst_row = dst_pixels + ((plan->dst_y0 + row) * dst_pitch) + plan->dst_x0;
          if ((row + 1) < plan->visible_h) {
              __builtin_prefetch(index8_src_row(i8_surface, src_y_lookup[row + 1]), 0, 0);
              __builtin_prefetch(dst_pixels + ((plan->dst_y0 + row + 1) * dst_pitch) + plan->dst_x0, 1, 0);
          }
          for (int col = 0; col < plan->visible_w; col++) {
              const Uint32 src_pixel = palette_lut[i8_row[src_x_lookup[col]]];
              if ((src_pixel >> 24) != 0u) { dst_row[col] = src_pixel; }
          }
      }
      return true;
  }
  ```

**Success criteria:**
- Telemetry build passes.
- Visual smoke confirms scaled INDEX8 sprites (e.g. PPG glyphs, scaled BG layers) render
  identically.

**Dependencies:** Step 1.

**What NOT to do:**
- Do **not** add a NEON variant here — out of scope.
- Do **not** alter the existing `populate_scaled_lookup_table` calls; the new path reuses the
  same lookup tables.

**What to do if it fails:**
- If scaled output has pixel artifacts but exact-copy is fine, the bug is in the
  `src_x_lookup`/`src_y_lookup` indexing, not the loose-form swap. Compare against the existing
  kernel inline to spot the deviation.

---

### Step 5 — Add telemetry counters and JSON emission

**Title:** Wire `colorkey_loose_fast_path_{hits,skipped,ineligible}` counters through to perf
JSON.

**Why:** Without counters we can't tell whether the fast path is firing, whether the
eligibility scanner correctly says everything is binary-α, or whether disabled vs ineligible
diverge in production.

**Files to read first:**
- `include/port/sdl/sdl_game_renderer.h:210-242` — telemetry struct.
- `src/port/sdl/sdl_game_renderer.c:248` — telemetry storage instance.
- `src/port/sdl/sdl_game_renderer.c:9581-9582` — telemetry reset (uses `SDL_zero`, no per-field
  changes needed).
- `src/port/sdl/sdl_app.c:5929-5954` — JSON emission of existing refresh telemetry block.

**Files to modify:**
- `include/port/sdl/sdl_game_renderer.h`: add three `Uint64` fields to
  `SDLGameRenderer_PerfCaptureRefreshTelemetry` after the existing `sampled_full_oversized_*`
  pair (around line 240, before the closing brace at line 242):
  ```c
  Uint64 colorkey_loose_fast_path_hits;
  Uint64 colorkey_loose_fast_path_skipped;
  Uint64 colorkey_loose_fast_path_ineligible;
  ```
- `src/port/sdl/sdl_game_renderer.c`: increments must be placed **inside the same branch
  ladder that picks the kernel** so that color-mod tasks (which never use the loose-form
  fast path by design — see §Design 2) are not counted as "ineligible". Specifically:
  - `_hits++`: increment via `RENDERER_TELEMETRY(...)` at the top of each new fast-path
    branch from steps 2, 3, 4 — i.e. immediately inside each
    `if (colorkey_loose_kernel_enabled && task->software_palette_is_binary_alpha [&& src_x_step == 1])`
    block, before the row loop.
  - `_skipped++` and `_ineligible++`: place these **inside the existing non-color-mod
    branches** only, not at the line 6116 entry. Two sites:
    1. Non-color-mod exact-copy branch (line 6224, just before the existing
       `#if RENDERER_HAVE_NEON` at line 6225 but **after** the new fast-path branches
       from steps 2 and 3 have been considered — i.e. the path that falls through to the
       existing kernel because either the runtime toggle is off or the palette is not
       binary-α):
       ```c
       /* This is the fall-through path: either kernel disabled or palette not binary-α. */
       if (task->software_palette_is_binary_alpha) {
           RENDERER_TELEMETRY(perf_capture_refresh_telemetry.colorkey_loose_fast_path_skipped++);
       } else {
           RENDERER_TELEMETRY(perf_capture_refresh_telemetry.colorkey_loose_fast_path_ineligible++);
       }
       ```
    2. Scaled non-color-mod branch (line 6296, just before the existing scaled scalar
       row loop — same pattern as above, after the step-4 fast-path branch has been
       considered).
  - The color-mod branch at lines 6130-6222 receives **no** counter increments — color-mod
    is intentionally out of scope for the loose-form optimization, and counting it would
    inflate `_ineligible` beyond what the on-device check (`ineligible == 0` matches research
    §4.2) is meant to verify.
  - Increments are per-task, never per-pixel.
- `src/port/sdl/sdl_app.c`: in the existing `software_surface_cache_refresh_*` JSON block
  starting at line 5929, append a new sub-object after the existing
  `software_surface_cache_refresh_blit_sampling` block. Match the existing
  `(unsigned long long)` cast / `%llu` format pattern. New JSON key example:
  `"colorkey_loose": { "hits": %llu, "skipped": %llu, "ineligible": %llu }`.

**Success criteria:**
- Telemetry build emits the three new fields in `perf_capture.json`.
- `python3 -c 'import json,sys; json.load(open(sys.argv[1]))' perf_capture.json` exits 0
  (catches stray-comma / missing-quote regressions silently tolerated by some JSON
  parsers but caught by Python's strict parser).
- On a test gameplay, `hits > 0` and (per research expectation) `ineligible == 0`.
- Disabling the kernel via `colorkey-loose-kernel-enabled = false` in `config.ini`
  (added in step 2) makes `hits == 0` and `skipped > 0`.

**Dependencies:** Steps 2, 3, 4.

**What NOT to do:**
- Do **not** allocate per-texture or per-palette breakdowns — out of scope for this plan.
- Do **not** emit at every frame — emission is once per perf-capture window, same as existing.
- Do **not** skip the `RENDERER_TELEMETRY(...)` wrapper. Without it the increments compile
  into release-non-telemetry builds, regressing.

**What to do if it fails:**
- If JSON parse fails downstream, check trailing comma — match the existing pattern (each
  object ends with `},\n` except the last).

---

### Step 6 — Add INDEX8 fast-path parity coverage

**Title:** Extend `software_frame_parity.c` with INDEX8 exact + scaled cases.

**Why:** Today the parity check only exercises the ARGB8888 non-integer path. The new INDEX8
loose-form kernels need a deterministic pixel-equality test that runs on host before deploy.

**Files to read first:**
- `src/port/sdl/software_frame_parity.c` — entire file (it is short).
- `src/port/sdl/sdl_app.c:1252-1254` — entry point that already invokes the parity check.

**Files to modify:**
- `include/port/sdl/sdl_game_renderer.h`: declare a new test-only helper:
  ```c
  bool SDLGameRenderer_RunIndex8FastPathParityCase(const SDL_Surface* index8_src,
                                                   const Uint32* palette_lut,
                                                   bool palette_is_binary_alpha,
                                                   const SDL_FRect* dst_rect,
                                                   const SDL_FRect* src_uv_rect,
                                                   SDL_FlipMode flip,
                                                   SDL_Surface* dst_surface);
  ```
  This is a thin shim that constructs a `RenderTask` with the supplied params (color =
  `0xFFFFFFFFu`, identity modulation), invokes `try_setup_software_frame_fast_copy_plan()`
  + `try_fast_copy_fast_textured_task_to_software_frame()` directly, and returns success.
  Defined in `sdl_game_renderer.c` next to existing `static`-internal entry helpers.
- `src/port/sdl/sdl_game_renderer.c`: define `SDLGameRenderer_RunIndex8FastPathParityCase`.
  Plumb a temporary `RenderTask` and call `try_fast_copy_fast_textured_task_to_software_frame()`
  with appropriate plan + dst surface. Mark the helper as **only used by the parity test**
  (comment). The shim's `RenderTask` body **must** initialize all fields the new kernel
  reads off the task (silent skipping the new kernel makes the test exercise the *old*
  path and defeats the purpose of step 6):
  ```c
  RenderTask task = {0};
  task.color = 0xFFFFFFFFu;                              /* identity modulation */
  task.software_source_surface = (SDL_Surface*)index8_src;
  task.software_source_is_index8 = true;                 /* required by line 6114 entry guard */
  task.software_palette_lut = palette_lut;               /* required by line 6114 entry guard */
  task.software_palette_is_binary_alpha = palette_is_binary_alpha;  /* required by step-2/3/4 fast-path branches */
  /* … remaining fields (vertices, dst rect, flip flags) per the supplied params. */
  ```
- `src/port/sdl/software_frame_parity.c`:
  - Extend `fill_index8_test_palette()` with two flavors:
    - `fill_index8_test_palette_strict_binary_alpha()` (palette[0].α=0, all others α=255)
    - `fill_index8_test_palette_loose_binary_alpha()` (multiple scattered α=0 indices)
  - Add `run_index8_fast_path_parity_check()` — same skeleton as
    `run_software_source_refresh_parity_check()` at line 245, but:
    - Source: 32×32 INDEX8 surface filled via `fill_index8_test_source()`.
    - Reference: `SDL_BlitSurface` from INDEX8 source to a fresh ARGB8888 expected surface
      (this is the ground truth — SDL converts via the palette).
    - Actual: invoke `SDLGameRenderer_RunIndex8FastPathParityCase` for each flip / scale /
      clip permutation listed in §design-decisions-§9. Run each case once with strict and
      once with loose binary-α palette flavors.
    - Compare via existing `surfaces_match()`.
  - Call `run_index8_fast_path_parity_check()` from
    `SDLGameRenderer_RunSoftwareFrameParityCheck()` at line 426 (next to the existing
    `run_software_source_refresh_parity_check()` call).

**Success criteria:**
- `SDLApp_RunSoftwareFrameParityCheck()` returns `true` (existing CLI invocation) on host.
- Output line confirms the new INDEX8 cases ran (`SDL_Log` from
  `run_index8_fast_path_parity_check`).

**Dependencies:** Steps 1, 2, 4. (Step 3 NEON is host-disabled on Mac so the NEON path is not
exercised here; on-device smoke covers it.)

**What NOT to do:**
- Do **not** parity-check NEON output on host. NEON is disabled on x86 host; coverage there
  is not meaningful. Device smoke (step 7) covers it.
- Do **not** require the parity test to run during normal startup — keep it CLI-flag-gated as
  the existing parity check already is.

**What to do if it fails:**
- If parity fails on a flip case, the ground-truth `SDL_BlitSurface(index8 -> argb8888)`
  should be inspected for whether SDL itself respects α=0 entries. Older SDL versions on some
  platforms drop α; verify with a `printf` on a single-pixel test before assuming the kernel
  is wrong.
- If parity fails only on the loose (scattered-α=0) palette but passes on strict, the fast
  path is doing strict-form somewhere. Re-check that the test compares via
  `(palette_lut[i] >> 24) != 0`, not `i != 0`.

---

### Step 7 — On-device smoke + measurement

**Title:** Deploy telemetry build, run gameplay session, record before/after.

**Why:** Final correctness and perf check.

**Files to read first:**
- `docs/mister-runbook.md` — canonical deploy + perf-capture flow.
- `tools/mister/build-game.sh` — build entry point; `--flavor telemetry`.
- `~/Developer/3sx-mister-baselines/2026-04-25-telemetry-instrumented/PROVENANCE.md` —
  documents the saved baseline binary (instrumentation patches present but non-hot-path).
- `~/Developer/3sx-mister-baselines/2026-04-25-telemetry-instrumented/source-commit.txt` —
  must be verified to match the parent commit of the perf branch before relying on it
  (currently `7ed2c2d3743d73f97b6f1efeef360763ebdf40c2`, which matches `perf` HEAD).

**Files to modify:** none (this step is operational).

**Procedure:**
1. **Baseline capture (saved binary, kernel does not exist):** verify
   `~/Developer/3sx-mister-baselines/2026-04-25-telemetry-instrumented/source-commit.txt`
   matches the parent commit of the current perf branch (i.e. the commit before the
   perf-1 step-1 commit landed). If mismatched, **stop** and either rebuild a fresh
   baseline at the matching commit or pause to rebase. If matched, deploy the
   `3s-arm` binary from that directory, capture 60 s of three scenes: char-select /
   hadou loop / Genei-Jin SA3. This baseline reflects pre-perf-1 behavior with no
   loose-form kernel present at all.
2. **Treatment capture (kernel enabled, default ON):** build the perf branch with
   `tools/mister/build-game.sh --flavor telemetry`, deploy, capture the same three
   scenes. Default config has the new kernel ON.
3. **Optional A/B in same binary:** add `colorkey-loose-kernel-enabled = false` to
   `config.ini`, restart, capture the same three scenes. This validates the runtime
   toggle and produces a same-binary OFF capture for sanity-checking the saved baseline.
4. Compare median frame time, p99 frame time, and FPS overlay readout across (1)
   saved-baseline and (2) treatment. Record both numbers in the working brief. The
   optional capture in (3) is a sanity check, not the primary A/B.
5. Visual side-by-side: pause-frame screenshots from baseline and treatment captures of
   (a) Yun standing-idle sprite, (b) Genei-Jin activation flash, (c) a stage with
   scaled BG.
6. Confirm `colorkey_loose_fast_path_hits >> colorkey_loose_fast_path_skipped` in
   treatment JSON, and `ineligible == 0`. (The saved-baseline binary lacks these
   counters and will not emit them — that is expected.)

**Success criteria:**
- The baseline binary's `source-commit.txt` matches the parent commit of the perf
  branch's first perf-1 commit. (If not, the comparison is invalid.)
- No visible regression in any of the three scenes (baseline vs treatment).
- Treatment median frame time ≤ saved-baseline median (any value; this plan is not
  gating on a specific delta — research suggests 1–2 ms/frame in INDEX8-heavy scenes
  but does not promise it).
- `ineligible == 0` in the treatment JSON confirms research §4.2 holds in practice.
- Optional A/B (3) shows visual identity between treatment-OFF and saved-baseline
  on the same scenes; this validates the runtime toggle and confirms the saved
  baseline's instrumentation patches are not affecting steady-state frame time.

**Dependencies:** Steps 1–6.

**What NOT to do:**
- Do **not** ship a release ZIP yet. Per `feedback-no-premature-release.md`, perf land
  requires user OK after first-light + gameplay test pass.
- Do **not** modify deploy targets outside `/media/fat/MiSTer_3S-ARM`,
  `/media/fat/_Other/3S-ARM.rbf`, `/media/fat/games/3s-arm/`.
- Do **not** use `rsync --delete` against `/media/fat`.

**What to do if it fails:**
- If on-device output diverges from host (host parity passed, device shows artifacts), the
  most likely cause is the NEON kernel (step 3). Disable NEON loose-form by inverting the
  `if (... && src_x_step == 1)` condition (or temporarily setting the runtime toggle off in
  a debug build). If artifacts remain, the scalar kernel from step 2 is also wrong — re-read
  the existing kernel at lines 6261-6284 verbatim and diff.

---

## Review issues addressed

### P-1 (must fix)

- **P-1.1 — Step 1 RenderTask grep recipe wrong, wrong "main site":** addressed by Step 1
  §Files to read first (now lists the four sites at 11506, 11725, 12035, 12196 + the global
  binding at 11680-11695 separately) and §Files to modify (now enumerates all four sites
  with the exact paired-write pattern, plus a new `current_software_palette_is_binary_alpha`
  global mirror to match the existing `current_software_palette_lut` lifecycle including
  the six other `current_software_palette_lut = NULL;` clears at lines 1084, 1128, 1172,
  1217, 1255, 9338). Step 1 §Success criteria now uses
  `grep -n 'task->software_palette_lut\s*=\|render_tasks\[.*\]\.software_palette_lut\s*='`
  to find the actual write sites.
- **P-1.2 — Step 5 counter at 6116 conflates color-mod / non-color-mod / ineligible:**
  addressed by Step 5 §Files to modify (counters now placed inside the same branch
  ladder that picks the kernel — `_hits++` inside each step-2/3/4 fast-path branch;
  `_skipped++` / `_ineligible++` inside the existing non-color-mod branches at line 6224
  exact-copy and line 6296 scaled, post-fall-through; explicitly **no** counters in the
  color-mod branch).
- **P-1.3 — NEON `vbslq_u32` operand-order toy example confusing:** addressed by Step 3
  §What to do if it fails (now points the fix agent directly at the in-tree precedent
  `sdl_game_renderer.c:5999-6006` `vbslq_u8(mask_zero, dst_bytes, result)` instead of
  reasoning from the toy example).
- **P-1.4 — Step 7 "edit default + rebuild" baseline strategy:** addressed by Step 7
  §Procedure rewritten to use the saved baseline binary at
  `~/Developer/3sx-mister-baselines/2026-04-25-telemetry-instrumented/3s-arm` (verified
  `source-commit.txt` = `7ed2c2d3` matches `perf` HEAD as of fix-agent run); no edit-and-
  rebuild needed; runtime toggle (P-1.5) provides same-binary sanity-check capture.
- **P-1.5 — No config key forecloses A/B without rebuild:** addressed by Step 2 (now
  includes `CFG_KEY_COLORKEY_LOOSE_KERNEL_ENABLED` define in `config.h:22` + a
  `Config_HasExplicitKey`-gated setter call at `sdl_app.c:9981`, ~10 lines total). Design
  §7 updated to document the config-key wiring and removes the "small debug hook only"
  note that contradicted `feedback-debug-build-for-live-tests.md`.

### P-2 (should fix)

- **P-2.1 — Design §9 shim-vs-move rationale thin:** addressed by Design §9 (now notes
  parity test already operates at the public-API layer via `SDLSoftwareFrame_*`, shim
  continues that pattern).
- **P-2.2 — Step 6 shim must init all three RenderTask fields:** addressed by Step 6
  §Files to modify (now contains an explicit `RenderTask task = {0};` init block listing
  all four required field assignments — `color`, `software_source_surface`,
  `software_source_is_index8`, `software_palette_lut`, `software_palette_is_binary_alpha`).
- **P-2.3 — Destroy-path clear at 11506 must zero the new bool:** addressed in Step 1
  §Files to modify item 1 (`render_tasks[i].software_palette_is_binary_alpha = false;`
  at line 11506) and §What to do if it fails (calls out the failure mode of stale-true
  carrying across task-slot reuse).
- **P-2.4 — JSON parse verification missing:** addressed by Step 5 §Success criteria
  (now includes `python3 -c 'import json,sys; json.load(open(sys.argv[1]))' perf_capture.json`
  exit-0 check).
- **P-2.5 — `populate_scaled_lookup_table` reuse:** verified, no plan change required.

### Self-consistency propagations

- Master "Files to change" list updated to include `src/port/config/config.h` (P-1.5).
- Rollback plan reordered: config.ini key flip (no rebuild) is now the fastest rollback,
  with compile-time default flip demoted to fallback (P-1.5 consequence).
- Design §7 rewritten to document the config-key path explicitly and reference
  `Config_GetBool(CFG_KEY_FULLSCREEN)` precedent at `sdl_app.c:9807`.
- Step 7 success criteria explicitly require source-commit.txt match before relying on
  the baseline binary.

