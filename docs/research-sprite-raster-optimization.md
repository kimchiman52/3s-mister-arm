# Sprite Raster Optimization Research

**Branch:** `sprite-raster-optimization`
**Date:** 2026-04-03
**Status:** WIP — deployed for testing, awaiting measurement

---

## 1. Problem Statement

Street Fighter III: 3rd Strike on MiSTer FPGA drops frames on sprite-heavy stages. The bottleneck is the software frame renderer compositing 200-280 small MTS sprites per frame.

| Stage | Typical FPS | render_ms |
|-------|-------------|-----------|
| Remy | ~55 | 6.7-8.3ms |
| Chun-Li | ~57 | similar |
| Ryu | ~57 | similar |
| Genei-Jin (super) | ~41 | 20+ ms |

The hot function is `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888()` in `software_frame_non_integer.c`.

## 2. Root Cause Analysis

### 2.1 The Non-Integer Gather Loop

The inner loop performs a dependent-load gather pattern:

```c
for (int col = 0; col < visible_w; col++) {
    const Uint32 src_pixel = src_row[src_x_lookup[col]];  // 2 chained loads
    // alpha test + blend/copy
}
```

Each pixel requires two chained memory accesses (~8+ cycles on Cortex-A9):
1. Load `src_x_lookup[col]` → produces an offset
2. Use offset to load `src_row[offset]` → produces the pixel

ARM NEON lacks scatter/gather instructions, so this cannot be vectorized.

### 2.2 Why Sprites Hit the Non-Integer Path

`build_software_frame_fast_copy_plan()` (`sdl_game_renderer.c:5437`) checks whether `dst_rect.x/y/w/h` are all integers (within epsilon 0.001). If ANY coordinate is fractional, the sprite goes to the non-integer rasterizer instead of the fast exact-copy path (essentially memcpy).

**Source of fractional coordinates:** The BgMATRIX transformation pipeline:

```
njScale(scr_sc, scr_sc, 1.0)       // zoom factor
njTranslate(0, 224, 0)              // flip Y origin
njScale(1, -1, 1)                   // flip Y axis
njTranslate(-h_shift, -v_shift, 0)  // camera scroll offset
```

- `scr_sc` = 1.0 during normal gameplay (no zoom), fractional during zoom events
- `h_shift`/`v_shift` include camera scroll which can have sub-pixel precision
- `scrn_adgjust_x/y` (zoom centering) is always integer (s16)

Even with `scr_sc = 1.0`, fractional camera offsets make 60-80% of sprite positions non-integer during normal gameplay.

### 2.3 CPS3 Hardware Context

The original CPS3 hardware has no sub-pixel sprite addressing. All sprite positions are integer pixel coordinates. The fractional positions in the port are purely artifacts of the floating-point matrix transformation pipeline — they don't represent real sub-pixel rendering.

## 3. What Has Been Tried (and Failed)

| Attempt | Outcome | Why It Failed |
|---------|---------|---------------|
| NEON blend loop acceleration | Reverted | Dependent-load pattern dominates; NEON lacks gather |
| 150 inner-loop optimization iterations | Marginal/regression | Pairs, run batching, alpha shortcuts, threshold tuning — all limited by gather latency floor |
| Tile generation counter dirty tracking | No benefit | `texture_unlocks=0` during steady-state gameplay |
| Prebaked tile decompression | Minimal benefit | Tile cache is warm during gameplay |
| Reciprocal/linear-step lookup rewrite (Loop 99) | Failed parity | Clamping at sprite boundaries breaks linearity |
| Scalar 4x row-walk unroll (Loop 135) | No gain | Doesn't help with chained load latency |
| Pair-density gate (Loop 142) | No gain | Fragmentation too high in practice |

## 4. Optimizations Implemented (This Branch)

### 4.1 Integer-Snap Sprite Positions

**File:** `src/sf33rd/Source/Game/rendering/mtrans.c:1651`

```c
// After njCalcPoints() transforms world → screen coords:
chip->v[0].x = SDL_roundf(chip->v[0].x);
chip->v[0].y = SDL_roundf(chip->v[0].y);
chip->v[1].x = SDL_roundf(chip->v[1].x);
chip->v[1].y = SDL_roundf(chip->v[1].y);
```

**Rationale:** Since CPS3 hardware has no sub-pixel addressing, rounding screen-space coordinates to integers is more authentic, not less. This routes sprites through the exact-copy fast path (memcpy-like) instead of the expensive non-integer gather rasterizer.

**Expected impact:** 2-4ms/frame savings by moving 50-80% of sprites from the gather loop to the fast copy path.

**Risk:** < 1 pixel visual shift. May affect sprite alignment during zoom sequences (`scr_sc != 1.0`) where the zoom factor genuinely produces non-integer scaled dimensions. Need to verify Genei-Jin and other zoom-heavy sequences visually.

### 4.2 Static Lookup Buffers

**File:** `src/port/sdl/software_frame_non_integer.c` (lines 395, 484)

Changed stack-allocated lookup arrays to `static`:
- `src_x_lookup[384]` — 1,536 bytes
- `src_y_lookup[224]` — 896 bytes
- `same_source_pair_lookup[384]` — 384 bytes
- **Total per call: 2,816 bytes**

Applied in both:
- `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888()` (main raster path)
- `SDLSoftwareFrame_AnalyzeNonIntegerSourceAlphaARGB8888()` (telemetry analysis)

**Rationale:** With ~200 calls/frame, the stack allocation/deallocation cycle writes ~563KB/frame (33.8 MB/s at 60fps) that immediately evicts hot data from the 32KB L1 D-cache. Static buffers eliminate this churn entirely.

**Safety:** The renderer is confirmed single-threaded (no pthread, SDL_Thread, or atomics). No re-entrancy risk. BSS cost is only 5.6KB.

## 5. Optimizations Investigated but Deferred

### 5.1 Fixed-Point Lookup Generation

`populate_non_integer_lookup()` computes `coord = ((i + offset) - origin) / span` per coordinate — a linear mapping that could use fixed-point stepping instead of per-element float division.

**Why deferred:**
- A previous attempt (Loop 99, commit b95b0f26) already tried a reciprocal/linear-step rewrite and **failed parity on clipped cases**. The clamp logic at sprite boundaries breaks the linear assumption.
- With typical sprite sizes of 8-40px, each call iterates only 8-40 times — modest absolute cost per call.
- A recent NEON acceleration (commit c0940898, 2026-04-02) already addresses pixel-processing throughput.

**If revisited:** Would need separate handling for clamped vs. non-clamped regions. The linear interior is easy; the boundary clamping requires per-element computation regardless.

### 5.2 Extend Background Cache to Normal Gameplay

The `sa_bg_cache` system (`sdl_game_renderer.c:3589-3666`) caches composited backgrounds and drops background render tasks — but only activates during super art freeze (`super_effect_quality_mode == CACHED_BG`). Extending it to normal gameplay could eliminate 100-150 background tasks/frame.

**Why deferred:**
- During normal gameplay, the camera scrolls continuously, invalidating the cache frequently
- Would need incremental update logic (shift cached surface, render only newly-revealed edges)
- Higher architectural complexity; should measure simpler wins first

### 5.3 Per-Surface Alpha Sidecar Bitmask

Add 1-bit-per-pixel alpha mask (8KB per 256x256 surface) to the texture cache. Before rasterizing a subrect, scan alpha bits to skip fully-transparent regions or route fully-opaque regions to a memcpy fast path.

**Why deferred:** Medium complexity. Research confirms most hot sprite families are binary-alpha (fully opaque or fully transparent), so the payoff is real. Worth pursuing if integer-snap doesn't close the gap.

### 5.4 Morton-Order Texture Swizzling

Row-major storage forces 32x32 sprites to scatter across 32 cache lines. Morton-order interleaving groups spatially-adjacent pixels into the same cache line.

**Why deferred:** Requires PMU measurement to confirm L1 miss rate is actually the bottleneck (vs. pipeline interlock). Medium implementation complexity. Should pursue after establishing PMU baseline.

### 5.5 Clustered-Shape Specialization

The top 4 sprite shape families (32x32→34-37px) contribute 30ms of 78ms during Genei-Jin. A hand-optimized loop unrolled for these exact dimensions could outperform the generic path.

**Why deferred:** Narrow benefit (Genei-Jin specific). Integer-snap may eliminate the need by routing these shapes to the exact-copy path.

### 5.6 Y-Lookup Memoization

Many sprites share identical vertical scaling parameters. Caching the last Y lookup table and skipping regeneration when parameters match could save ~50-70% of Y lookup calls.

**Why deferred:** Small absolute savings. Worth combining with other lookup optimizations if pursued.

## 6. Architecture Reference

### Sprite Dispatch Chain

```
mtrans.c: seqsStoreChip()
  → Set world coords on Sprite2 vertices
  → njCalcPoints(BgMATRIX) transforms to screen space
  → ★ INTEGER SNAP (new) ★
  → Offscreen culling (screen bounds check)
  → seqs_w.chip[] array (up to 1024 entries)

mtrans.c: mlt_obj_trans_ext() loop
  → SDLGameRenderer_DrawSprite2() for each valid sprite
    → draw_sprite_rect() → try_setup_textured_rect_task()
      → RenderTask array (render_tasks[], max 1024)

sdl_game_renderer.c: render_frame_to_software_surface()
  → Z-sort render tasks
  → Merge adjacent same-texture rect tasks
  → For each task:
    → build_software_frame_fast_copy_plan() classifies path:
      → EXACT: integer position, same size → memcpy-like (FAST)
      → SCALED: integer position, different size → scaled copy
      → NON_INTEGER: fractional position → gather rasterizer (SLOW)
      → COLOR_MOD: has color modulation
    → Route to appropriate rasterizer
```

### Render Path Performance (Cortex-A9 @ 1200 MHz)

| Path | Cost | When Used |
|------|------|-----------|
| Exact copy | ~0.5 cycles/pixel | Integer position, src_w == dst_w |
| Scaled copy | ~1.0 cycles/pixel | Integer position, src_w != dst_w |
| Non-integer lookup | ~2.0 cycles/pixel | Fractional position (THE BOTTLENECK) |
| Generic textured | ~20 cycles/pixel | Float division per pixel (rare) |

### Key Data Structures

- **Sprite2** (`primitives.h`): Two Vec3 vertices, two TexCoords, vertex_color, tex_code, id
- **RenderTask** (`sdl_game_renderer.c:82`): dst_rect, src_uv_rect, flip, color, z, texture, software_source_surface
- **Surface cache**: `software_surface_cache[256][257]` — pre-converted ARGB8888 surfaces, lookup via `get_or_create_software_source_surface()`

### Telemetry Fields

- `sampled_lookup_x_ns` / `sampled_lookup_y_ns` — lookup table generation time per axis
- `sampled_pair_lookup_ns` — same-source pair detection time
- `sampled_row_raster_ns` — actual pixel rasterization time (minus telemetry overhead)
- `same_source_reused_pixels` — pixels saved by pair optimization
- `source_alpha_opaque/transparent/blended_pixels` — alpha class breakdown

## 7. Test Plan

### Visual Verification

- [ ] Remy stage: sprites align correctly, no visible jitter during camera scroll
- [ ] Chun-Li stage: background layers composite correctly
- [ ] Genei-Jin activation: zoom effects look correct (scr_sc != 1.0 during super)
- [ ] Character sprites: no misalignment at sprite boundaries
- [ ] Stage transitions: no visual artifacts

### Performance Measurement

- [ ] Remy stage `render_ms` before/after (target: 6.7ms → <4ms)
- [ ] Chun-Li stage `render_ms`
- [ ] Genei-Jin `render_ms` (may not improve if zoom keeps positions fractional)
- [ ] FPS on Remy stage (target: 55 → 58-60)
- [ ] Check telemetry: ratio of EXACT vs NON_INTEGER fast copy results

### Regression Checks

- [ ] All stages playable without crashes
- [ ] No visual glitches on any character select screen backgrounds
- [ ] Training mode overlay renders correctly
- [ ] OSD menu renders correctly

## 8. Hardware

- ARM Cortex-A9 @ 1200 MHz (overclocked), 32KB L1 D-cache, 512KB L2
- NEON VFPv3-D32 (no scatter/gather)
- Build: Clang 20, `-O3 -mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard`
- MiSTer SSH: `root@192.168.1.171`, password=1
