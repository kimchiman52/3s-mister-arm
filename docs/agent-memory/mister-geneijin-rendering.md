# Genei-Jin (Yun SA3) Rendering Reference

## Game Identity

- Yun = character index 3 (`My_char[0] == 3`)
- SA3 = super art index 2 (`Super_Arts[0] == 2`)
- Internal name: "metamorphose"

## Game Code

### Effect System
- **Effect handler:** `src/sf33rd/Source/Game/effect/effk7.c` — K7 effect (effect ID 207 / 0xCF)
  - `effect_K7_init()` — spawned on SA3 activation by `Att_METAMORPHOSE()`
  - `effect_K7_move()` — per-frame movement/control
  - `K7_move_type_0()` — manages metamorphose state transitions (pure state machine, does NOT render anything)
- **Activation:** `src/sf33rd/Source/Game/engine/plpat19.c` — `Att_METAMORPHOSE()` calls `effect_K7_init()`
- **Effect jump table:** `src/sf33rd/Source/Game/effect/effxx.c` line 257 — K7 at position 182

### Color / Palette Transformation
- **File:** `src/sf33rd/Source/Game/rendering/meta_col.c`
  - `metamor_color_trans(wkid)` — modifies 2 ColorRAM rows: `ColorRAM[wkid*16]` and `ColorRAM[wkid*16 + 8]`
  - `metamor_color_copy(wkid)` — copies opponent's 6 palette rows to player slots (12 rows + 1 mcs_sel_tbl entry)
  - `metamor_color_store()` / `metamor_color_restore()` — save/restore original colors
  - **Called ONCE** during activation (effk7.c case 1), NOT per frame

### Sprite Rendering Pipeline
- **File:** `src/sf33rd/Source/Game/rendering/mtrans.c`
  - `seqsStoreChip()` (line ~1609) — core sprite/texture rendering function
  - `seqsAfterProcess()` (line ~1570) — flushes sprite queue to render tasks via `SDLGameRenderer_DrawSprite2()`
  - `mlt_obj_disp()` (line ~155) — multi-texture object display
  - All sprite rendering produces textured rectangles via this pipeline
  - `chip->vertex_color = curr_bright | ((0xFF - alpha) << 24)` — sprite color from brightness table

### Background Rendering Pipeline
- **File:** `src/sf33rd/Source/Game/stage/bg.c`
  - `scr_trans()` (line ~548) — iterates scroll layers, calls per-screen rendering
  - `bgDrawOneChip()` (line ~1094) — renders individual background tiles via `ppgWriteQuadUseTrans()`
  - Background uses `bgPalCodeOffset[bgnm]` = 0x12C (300) for palette indices
  - Background textures loaded once during stage setup, never modified via ppgRenew during gameplay
  - Rendered through PPG path (`SDL_GAME_RENDERER_TASK_SOURCE_PPG`), distinct from sprite path (MTRANS)

### Key Structs
- `PLW.sa` — points to `SA_WORK` structure (super art state)
- `PLW.metamorphose` — flag: Genei-Jin mode active
- `PLW.metamor_index` — tracks metamorphose effect work

## Burst Detection (Port Layer)

### State Tracking (`src/port/sdl/sdl_app.c`)
- `trusted_yun_sa3_burst_active_now()` — checks `(plw[0].sa->ok == -1) || plw[0].metamorphose != 0`
- `update_trusted_yun_sa3_burst_state_for_frame()` — fires each frame in `SDLApp_EndFrame()`
- On activation (false→true transition): sets `trusted_yun_sa3_burst_frames_remaining = 82`
- Decremented each frame while burst remains active
- Logs `SA3-DIAG burst-trigger` once per activation

### Quality Modes
- Config key: `super-effect-quality` (default: `full`)
- `FULL=0` — no reduction, `CACHED_BG=1` — background caching
- Perf sampler passes `--super-effect-quality cached-bg`
- OSD toggle cycles through modes

## Render Task Types
```c
RENDER_TASK_TYPE_GEOMETRY = 0      // Lines, triangles, etc.
RENDER_TASK_TYPE_TEXTURED_RECT = 1 // Sprite/texture rendering
```

## Rendering Paths (All Go Through render_tasks[])

### Path A: PPG (Background / Stage)
`ppgWriteQuadUseTrans()` → `ppgWriteQuadOnly2()` → `SDLGameRenderer_DrawSprite()` → `push_render_task()`
- Source: `SDL_GAME_RENDERER_TASK_SOURCE_PPG`
- Used by: background tiles, stage elements

### Path B: MTRANS / Seqs (Characters / Effects)
`seqsStoreChip()` → `seqsAfterProcess()` → `SDLGameRenderer_DrawSprite2()` → `push_render_task()`
- Source: `SDL_GAME_RENDERER_TASK_SOURCE_MTRANS`
- Used by: character sprites, effect sprites, particle effects

### Path C: 2D Polygon Buffer (Solid Geometry)
`njdp2d_sort()` → `njdp2d_draw()` → `SDLGameRenderer_DrawSolidQuad()` → `push_render_task()`
- Source: `SDL_GAME_RENDERER_TASK_SOURCE_SOLID`
- Used by: shadows, solid-color quads

### Per-Frame Execution Order (main.c:976-980)
```
njUserMain()           ← game logic populates seqs_w.chip[] and njdp2d_w
seqsBeforeProcess()    ← (unclear — may clear sprite queue)
njdp2d_draw()          ← flush 2D polygons → push_render_task()
seqsAfterProcess()     ← flush sprites → push_render_task()
```

## Palette System Architecture

### Palette Handle Separation (CRITICAL)
- **Character palettes:** CP3 indices 0-31 (player 0: 0-15, player 1: 16-31)
  - Set via `(attr & 0x1FF) + wk->colcd` in `mlt_obj_trans_cp3()` (mtrans.c line ~1095)
- **Background palettes:** CP3 indices 300+ (`bgPalCodeOffset = 0x12C`)
  - Set via `*tran + pal` where `pal = bgPalCodeOffset[bgnm]` in `ppgWriteQuadUseTrans()` (PPGFile.c line ~522)
- **These never overlap.** Palette invalidation during Genei-Jin only touches character palette handles.

### Texture Cache
```c
static SDL_Texture* texture_cache[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1]; // [256][1089]
static SDL_Surface* software_surface_cache[FL_TEXTURE_MAX][FL_PALETTE_MAX + 1];
```
- Indexed by `[texture_handle][palette_handle]`
- Palette unlock scans 256 texture indices for ONE palette column
- Texture unlock scans 1089 palette columns for ONE texture row
- Software frame mode has unchanged-palette skip optimization (line ~10037)

### Genei-Jin Palette Changes Do NOT Invalidate Background Textures
- `metamor_color_trans(0)` unlocks `palCP3.handle[0]` and `palCP3.handle[8]` (character palettes)
- Background textures are cached under `palCP3.handle[300+]` — completely separate column
- The cache invalidation loop is effectively a no-op for backgrounds (iterates NULLs)
- Palette changes are one-time (activation only), not per-frame

---

## Background Caching Fix (SHIPPED — CACHED_BG quality mode)

### The Problem

On MiSTer's ARM Cortex-A9 (software frame rendering), Genei-Jin drops from ~60fps to ~45fps. The 82-frame SA3 burst window adds extra sprites (ghost clones, character overlays, particle effects) that push total pixel throughput over the 16.6ms frame budget. The bottleneck is **total render work**, not any specific effect type — the background tiles spanning multiple parallax scroll layers consume the bulk of CPU time, and the extra SA3 sprites consume the remaining headroom.

### The Solution: One-Shot Background Caching with Scaled Restore

Cache the rendered background surface once on the first burst frame. For all subsequent burst frames, restore the cached background via blit (fast memcpy or scaled nearest-neighbor), then render characters/effects/HUD fresh on top at 60fps. Background tasks are dropped from `render_tasks[]` so the rasterizer never touches them.

**Result:** Full 60fps during Genei-Jin with all visual effects intact. The zoom animation during SA3 activation is handled by a scaled blit of the cached background (~0.5ms) instead of re-rendering 100+ background tiles (~10ms).

### Why Background Caching Works

1. **Background is static during Genei-Jin.** Palette changes (`metamor_color_trans`) only touch character palette indices (0-31). Background palettes (300+) are completely separate and never invalidated. Background textures are loaded once during stage setup.
2. **Background is the costliest layer.** v14 (drop 50% uniform) and v16 (drop bottom 50%) diagnostics proved that removing background restores full speed while thinning effects has negligible impact.
3. **Background is identifiable.** All background render tasks use palette handles >= 256 (`bgPalCodeOffset = 0x12C = 300`). They sit contiguously at the bottom of the Z-sorted `render_tasks[]` array.
4. **Zoom is rare and short.** The SA3 activation zoom animation lasts ~12-24 frames out of 82. A scaled blit during zoom frames produces slightly soft background but is imperceptible under the flashy SA3 visual effects.

### Architecture Overview

```
Frame N (first burst frame — full render + snapshot):
  ┌─────────────────────────────────────────────┐
  │ render_tasks[] (Z-sorted, low Z at index 0) │
  │ ┌─────────────────┐                         │
  │ │ BG tiles (pal≥256) │ ← rendered normally  │
  │ └─────────────────┘                         │
  │         ↓ mid-render snapshot here           │
  │         ↓ memcpy surface → sa3_burst_saved   │
  │ ┌─────────────────┐                         │
  │ │ Chars/FX/HUD    │ ← rendered on top       │
  │ └─────────────────┘                         │
  └─────────────────────────────────────────────┘

Frames N+1..N+81 (cached restore + fresh upper layers):
  ┌─────────────────────────────────────────────┐
  │ 1. Restore sa3_burst_saved → surface        │
  │    (memcpy if no zoom/scroll change,        │
  │     scaled blit if zoom or scroll changed)  │
  │ 2. Drop BG tasks from render_tasks[]        │
  │ 3. Render chars/effects/HUD fresh at 60fps  │
  └─────────────────────────────────────────────┘

Burst end (frames_remaining hits 0):
  ┌─────────────────────────────────────────────┐
  │ sa3_burst_saved_surface_valid = false        │
  │ Normal full rendering resumes               │
  └─────────────────────────────────────────────┘
```

### Implementation Details (`src/port/sdl/sdl_game_renderer.c`)

#### Static State Variables (line ~131)

```c
static SDL_Surface* sa3_burst_saved_surface = NULL;      /* Cached background pixels (384x224 ARGB8888) */
static bool sa3_burst_saved_surface_valid = false;        /* True once snapshot has been taken */
static int sa3_burst_save_snapshot_at_index = -1;         /* Render task index to trigger mid-render save */
static float sa3_burst_cached_scr_sc = 1.0f;             /* scr_sc at time of snapshot */
static short sa3_burst_cached_adgjust_x = 0;             /* scrn_adgjust_x at time of snapshot */
static short sa3_burst_cached_adgjust_y = 0;             /* scrn_adgjust_y at time of snapshot */
```

#### External Declarations (line ~10)

```c
#include "sf33rd/Source/Game/system/work_sys.h"   /* provides extern f32 scr_sc (line 67) */

extern short scrn_adgjust_x;  /* from bg_data.h — zoom scroll X compensation */
extern short scrn_adgjust_y;  /* from bg_data.h — zoom scroll Y compensation */
```

`scrn_adgjust_x/y` are declared directly via `extern` rather than `#include "bg_data.h"` to avoid pulling in bg_data.h's deep dependency chain (bg.h → types.h → ...).

#### Surface Allocator: `ensure_sa3_burst_saved_surface()` (line ~3373)

Lazy-allocates a 384x224 ARGB8888 SDL_Surface matching `software_frame_surface`. Called once; the surface persists for the lifetime of the process.

```c
static bool ensure_sa3_burst_saved_surface(void) {
    if (sa3_burst_saved_surface != NULL) return true;
    sa3_burst_saved_surface = SDL_CreateSurface(cps3_width, cps3_height, SDL_PIXELFORMAT_ARGB8888);
    return sa3_burst_saved_surface != NULL;
}
```

#### Pre-Render Decision: `apply_super_effect_burst_reduction_after_sort()` (line ~3925)

Called after `compare_render_tasks` sort, before `render_frame_to_software_surface()` rasterization. This is the main control function.

**Step 1 — Gate checks:**
```c
if ((super_effect_quality_mode != SDL_GAME_RENDERER_SUPER_EFFECT_QUALITY_CACHED_BG) ||
    (trusted_yun_sa3_burst_frames_remaining <= 0) || (render_task_count <= 1))
    return;
```

**Step 2 — Identify contiguous background tasks from bottom of Z-order:**

Scan `render_tasks[]` from index 0 upward. Background tiles have `HI_16_BITS(texture_binding) >= 256` (palette handle). Stop scanning at the first textured task with palette < 256 (character/effect). Non-textured tasks (solid geometry like shadows, which have `texture == NULL`) are skipped over — they're cheap to re-render.

```c
int bg_end = 0;
for (int i = 0; i < render_task_count; i++) {
    const int pal = HI_16_BITS(render_tasks[i].texture_binding);
    if (pal >= 256) {
        bg_end = i + 1;
    } else if (render_tasks[i].texture != NULL) {
        break;  /* first character/effect task */
    }
}
```

**Why contiguous-from-bottom?** Some stages have foreground decorative elements that also use palette >= 256, but these appear at HIGH Z values (top of the array, interleaved with HUD). Stopping at the first non-background textured task ensures we only cache the actual background slab, not foreground stage elements.

**Step 3 — First burst frame (no cache yet):**

Let the full render proceed normally. Set `sa3_burst_save_snapshot_at_index = bg_end` to trigger a mid-render surface copy. Record the current zoom/scroll state for later delta computation.

```c
if (!sa3_burst_saved_surface_valid || sa3_burst_saved_surface == NULL) {
    sa3_burst_save_snapshot_at_index = bg_end;
    sa3_burst_cached_scr_sc = scr_sc;
    sa3_burst_cached_adgjust_x = scrn_adgjust_x;
    sa3_burst_cached_adgjust_y = scrn_adgjust_y;
    return;
}
```

**Step 4 — Subsequent burst frames (cache exists):**

Compute scroll/zoom deltas. If nothing changed, fast memcpy. If zoom or scroll changed, use the scaled blit. Then drop all background tasks so the rasterizer only processes characters/effects/HUD.

```c
const int scroll_dx = (int)scrn_adgjust_x - (int)sa3_burst_cached_adgjust_x;
const int scroll_dy = (int)scrn_adgjust_y - (int)sa3_burst_cached_adgjust_y;
const bool needs_transform = (sa3_burst_cached_scr_sc != scr_sc) ||
                             (scroll_dx != 0) || (scroll_dy != 0);
/* ... lock, blit (scaled or memcpy), unlock ... */

/* Drop background tasks — shift upper tasks down */
const int upper_count = render_task_count - bg_end;
SDL_memmove(&render_tasks[0], &render_tasks[bg_end],
            (size_t)upper_count * sizeof(RenderTask));
render_task_count = upper_count;
```

#### Mid-Render Snapshot in `render_frame_to_software_surface()` (line ~7542)

Inside the main software rasterization loop, after rendering task index `i >= sa3_burst_save_snapshot_at_index`, copy the current surface state to `sa3_burst_saved_surface`. This captures background-only pixels — no character data has been rendered yet.

```c
if ((sa3_burst_save_snapshot_at_index >= 0) &&
    (i >= sa3_burst_save_snapshot_at_index) &&
    !sa3_burst_saved_surface_valid &&
    ensure_sa3_burst_saved_surface()) {
    if (SDL_LockSurface(sa3_burst_saved_surface)) {
        SDL_memcpy(sa3_burst_saved_surface->pixels,
                   software_frame_surface->pixels,
                   (size_t)software_frame_surface->pitch * software_frame_surface->h);
        SDL_UnlockSurface(sa3_burst_saved_surface);
        sa3_burst_saved_surface_valid = true;
    }
    sa3_burst_save_snapshot_at_index = -1;
}
```

#### Burst End Invalidation in `SDLGameRenderer_SetTrustedYunSA3BurstFramesRemaining()` (line ~8167)

When burst countdown hits zero, invalidate the cache so the next SA3 activation gets a fresh snapshot.

```c
if (trusted_yun_sa3_burst_frames_remaining <= 0) {
    sa3_burst_saved_surface_valid = false;
}
```

### Scaled Blit Math Derivation

During SA3 activation, the camera zooms in briefly (`zoom_add` drops below 64, `scr_sc = 64.0 / zoom_add > 1.0`). The game compensates by adjusting scroll offsets (`scrn_adgjust_x/y`) so the camera centers on the player. We need to map pixels from the cached background surface (rendered at one zoom/scroll state) to the current frame's zoom/scroll state.

#### The BgMATRIX Transform Chain (`bg.c` line 599-604)

For each background layer `bgnm`, the game builds a 4x4 matrix:

```c
njUnitMatrix(0);
njScale(0, scr_sc, scr_sc, 1.0);           // (1) scale by zoom factor
njTranslate(0, 0, 224.0, 0);                // (2) flip origin to bottom
njScale(0, 1.0, -1.0, 1.0);                 // (3) Y-axis flip
njTranslate(0, -h_shift, -v_shift, 0);      // (4) scroll offset
```

Where:
```c
h_shift = scrn_adgjust_x + base_scroll_x    // bg.c line 1395: Irl_Scrn()
v_shift = base_scroll_y - scrn_adgjust_y     // bg.c line 1397: Irl_Scrn()
```

#### The `scrn_adgjust` Computation (`bg.c` line 1265: `Frame_Adgjust()`)

`scrn_adgjust_x/y` are computed from `zoom_add` and a focal position (`pos_x`, `pos_y`):

```c
// When zooming in (zoom_add < 64):
scrn_adgjust_x = +((64 - zoom_add) * pos_x) >> 6
scrn_adgjust_y = +((64 - zoom_add) * (pos_y + 0x15)) >> 6

// When zooming out (zoom_add >= 64):
scrn_adgjust_x = -((zoom_add - 64) * pos_x) >> 6
scrn_adgjust_y = -((zoom_add - 64) * (pos_y + 0x15)) >> 6
```

These offsets shift the scroll so that the zoom centers on the player. **This is key**: `scrn_adgjust` already encodes zoom centering. Any blit transform that uses `scrn_adgjust` deltas does NOT need an additional center-screen correction.

#### Deriving the Blit Formula

For a world point W, the screen position is (simplified to X axis, Y is analogous):

```
screen_x = (W_x - h_shift) * scr_sc
         = (W_x - scrn_adgjust_x - base_x) * scr_sc
```

At cache time (zoom = `cached_sc`, scroll = `cached_adj_x`):
```
cached_pixel = (W_x - cached_adj_x - base_x) * cached_sc
```

At current frame (zoom = `current_sc`, scroll = `current_adj_x`):
```
current_pixel = (W_x - current_adj_x - base_x) * current_sc
```

We want: given a destination pixel position `dst` in the current frame, what source pixel `src` in the cached surface shows the same world content?

Solve for W_x from the current frame equation:
```
W_x = dst / current_sc + current_adj_x + base_x
```

Substitute into the cached surface equation:
```
src = (dst / current_sc + current_adj_x + base_x - cached_adj_x - base_x) * cached_sc
    = (dst / current_sc + (current_adj_x - cached_adj_x)) * cached_sc
    = dst * (cached_sc / current_sc) + (current_adj_x - cached_adj_x) * cached_sc
```

**Final formula:**
```
src = dst * (cached_sc / current_sc) + scroll_delta * cached_sc
```

Where:
- `cached_sc / current_sc` = scale ratio (maps destination pixels to source pixels)
- `scroll_delta = current_adj - cached_adj` = change in `scrn_adgjust` since caching
- `scroll_delta * cached_sc` = scroll offset in cached-surface pixel coordinates

Note: `base_x` cancels out completely. The formula only depends on the `scrn_adgjust` delta and the two zoom factors.

#### Why No Center-Screen Correction Is Needed

An earlier iteration added a center-screen correction term `(cx - cx * inv_rel)` which caused the background to zoom too much. This was wrong because `scrn_adgjust` already provides zoom centering — `Frame_Adgjust()` computes an offset proportional to `(64 - zoom_add)` which is exactly the scroll compensation for the zoom. Including both `scrn_adgjust` deltas AND a center-screen correction double-counts the centering.

#### Implementation: Fixed-Point 16.16 Integer Math

The inner loop uses fixed-point arithmetic for ARM Cortex-A9 performance (no FPU in the hot path):

```c
static void sa3_burst_restore_background_scaled(float cached_sc, float current_sc,
                                                 int scroll_dx, int scroll_dy) {
    const int w = software_frame_surface->w;       /* 384 */
    const int h = software_frame_surface->h;       /* 224 */
    const int w_max = w - 1;                        /* 383 */
    const int h_max = h - 1;                        /* 223 */
    const float inv_rel = cached_sc / current_sc;   /* dst→src scale ratio */

    /* Convert to 16.16 fixed-point — float math only happens once here */
    const int inv_rel_fp = (int)(inv_rel * 65536.0f);
    const int offset_x_fp = (int)((float)scroll_dx * cached_sc * 65536.0f);
    const int offset_y_fp = (int)((float)scroll_dy * cached_sc * 65536.0f);

    const Uint32* src_pixels = (const Uint32*)sa3_burst_saved_surface->pixels;
    Uint32* dst_pixels = (Uint32*)software_frame_surface->pixels;
    const int src_pitch4 = sa3_burst_saved_surface->pitch / 4;
    const int dst_pitch4 = software_frame_surface->pitch / 4;

    for (int y = 0; y < h; y++) {
        int src_y = (y * inv_rel_fp + offset_y_fp) >> 16;
        if (src_y < 0) src_y = 0;
        else if (src_y > h_max) src_y = h_max;

        Uint32* dst_row = dst_pixels + y * dst_pitch4;
        const Uint32* src_row = src_pixels + src_y * src_pitch4;
        int sx_fp = offset_x_fp;                    /* X accumulator starts at scroll offset */

        for (int x = 0; x < w; x++) {
            int src_x = sx_fp >> 16;
            if (src_x < 0) src_x = 0;
            else if (src_x > w_max) src_x = w_max;
            dst_row[x] = src_row[src_x];            /* nearest-neighbor sample */
            sx_fp += inv_rel_fp;                     /* step by scale ratio */
        }
    }
}
```

**Performance:** ~0.5ms for 384x224 pixels with integer-only inner loop. Compare to ~10ms for full background tile rendering.

**Edge clamping:** Source coordinates outside [0, w_max] or [0, h_max] are clamped to edge pixels. This avoids black borders when the current zoom is tighter than the cached zoom (some destination pixels map outside the cached surface bounds). The clamped edge pixels are visually acceptable because they're at the screen border and covered by the zoom animation effects.

### Failed Approaches and Why They Failed

These are documented to prevent repeating dead-end investigations.

#### 1. Per-Effect Task Dropping (v1-v13 diagnostic series)

Attempted to identify and drop specific SA3 visual effect render tasks by texture/palette metadata. Failed because **effects have no distinguishing metadata** — they're standard textured rects with alpha=0xFF, color=0xFFFFFFFF, using many different texture handles. 14+ visual diagnostic iterations proved this conclusively.

#### 2. Full-Frame Alternating (Frame Skip without caching)

Reuse entire previous frame every other frame → 30fps visual. Characters appeared to stutter because the restored frame contained old character pixel positions. The "20fps look" was because human perception reads character motion more than background.

#### 3. Full-Frame Save/Restore from `SDLGameRenderer_RenderFrame()`

Saving the complete frame (background + characters + everything) after render and restoring on skip frames had the same stale-character problem as #2. Characters were frozen at their previous frame positions.

#### 4. render_task_count/2 Split

Dropping the bottom half of render_tasks by index count. Characters sit in the bottom half on some frames, causing them to disappear. The task count varies per frame, so a percentage-based split is unreliable.

#### 5. Full-Array Palette Scan for Background Identification

Scanning the ENTIRE render_tasks array for any task with pal >= 256 found stage foreground decoration elements at high Z values. Including these in the "background" slab meant characters and foreground were both dropped → frozen screen.

#### 6. Zoom-Frame Full Render Fallback

Adding `if (scr_sc != 1.0f) return;` to skip caching during zoom frames. The zoom frames are the HEAVIEST frames because the zoom itself adds rendering cost. Falling back to full rendering on exactly the frames that need the most help defeated the purpose.

#### 7. Center-Screen Correction in Scaled Blit

Adding `offset += (cx - cx * inv_rel)` to center the zoom around mid-screen. `scrn_adgjust` already handles zoom centering in the game's coordinate system. The center-screen term double-counted the offset, causing background to zoom more than characters. Removing it was the fix.

#### 8. Scroll Offset * inv_rel Factor

Early scaled blit used `scroll_delta * inv_rel` (= `scroll_delta * cached_sc / current_sc`). The correct factor is `scroll_delta * cached_sc` — the delta needs to be transformed into cached-surface pixel space, not destination pixel space. The `inv_rel` factor was wrong because it maps destination→source for position, not for offset translation.

### tex/pal Families Investigated

| tex | pal | What it actually is |
|-----|-----|-------------------|
| 41 | 1, 5 | Character body overlays — dropping garbles player sprite |
| 14 | 1, 5 | Character body overlays — same issue |
| 61 | 11 | SA3-exclusive (zero baseline count), but dropping 75% yields only +1.3 fps with no visible effect change |
| 57 | 397 | SA3-exclusive, same story |
| 62 | 528 | Ghost clones (color=6FFFFFFF) — identified in differential, not yet tested |
| 63 | 528 | Ghost clones — same |

### Exact burst families (tex 56/57/58 with pal 391-394) — status unknown
These were in the original exact burst classifier. The v1 per-family tint diagnostic assigned them colors (ORANGE/PINK/WHITE) but user reported "effects had no tint" — suggesting these also aren't the visible orbs/bolts. However, they may contribute to other visual elements during burst.

### Differential Diagnostic Limitations
Count-delta between baseline and burst frames does NOT reliably identify visual effects. Genei-Jin changes character rendering state (extra layers, color shifts) which increases counts of character tex/pal combos. The actual effects may use tex/pal combos that also exist in baseline.

## Perf Measurements

| Build | FPS | Render ms | Notes |
|-------|-----|-----------|-------|
| Baseline (quality=full) | 45.38 | 9.930 | No reduction |
| 4 families (41/14/61/57) | 48.33 | 8.835 | Character garbled, effects unchanged |
| 2 safe families (61/57) | 46.66 | 9.383 | No garble, +1.3 fps, effects unchanged |
| Drop 50% uniform (v14) | ~60 | — | Full speed, effects thinned, bg/char garbled |
| Drop bottom 50% (v16) | ~60 | — | Full speed, bg removed, effects+char+HUD intact |
| Thin middle 45% (v15) | ~47 | — | Slight improvement, char slightly garbled |
| **BG caching + scaled blit** | **~60** | **~0.5ms blit** | **Full speed, all effects intact, zoom correct** |

## Legacy Code State

`src/port/sdl/sdl_game_renderer.c` still contains `#if 0`-wrapped remnants of the earlier per-effect classification approach. These are dead code and can be removed once the background caching approach is considered stable.

### `#if 0` wrapped (safe to delete)
- Reduction constants at line ~958 (`super_effect_minimal_non_integer_keep_cadence`, etc.)
- `classify_super_effect_exact_burst_cohort()` at line ~3591
- `note_perf_capture_super_effect_burst_reduction_stat()` at line ~3611
- `super_effect_exact_burst_candidate_matches_task()` at line ~3710
- `super_effect_exact_burst_keep_cadence_for_mode()` at line ~3767
- `super_effect_exact_burst_should_drop_for_mode()` at line ~3802
- `sa3_tint_diagnostic_color()` v1 at line ~3826
- `classify_super_effect_hot_family()` — removed (was live dead code, not `#if 0`-wrapped)

## Build & Test

```bash
# Build
tools/mister/build-game.sh --flavor telemetry

# Deploy
MISTER_PASSWORD=1 tools/mister/misterctl.sh deploy --src build/mister-telemetry-package

# Deploy while game is running
MISTER_PASSWORD=1 MISTER_ALLOW_BUSY_TARGET=1 tools/mister/misterctl.sh deploy --src build/mister-telemetry-package

# Automated SA3 perf test
MISTER_PASSWORD=1 tools/mister/perf-sampler.sh \
  --scene yun-sa3 --frames 300 --tag <tag-name> \
  --test-scene-preset yun-sa3-repeat \
  --perf-wait-test-phase p1-super-art-active \
  --perf-basic \
  --super-effect-quality cached-bg
```
