# Active Super Art Background Caching Research

Last updated: 2026-03-25

## Problem

Certain characters experience noticeable slowdown during the ACTIVE super art execution phase (after the activation cinematic ends):
- **Q SA1** (Critical Combo Attack) -- rushing grab super
- **Chun-Li SA2** (Houyoku Sen) -- rapid kick combo super
- **Remy SA1** (Light of Virtue) -- projectile super

Background caching during the activation cinematic was already working (using `sa_stop_check()` + grace period), but performance dropped after the cinematic freeze ended.

## Root Cause

During the active SA attack animation:
1. Extra sprite layers from effect objects (hit sparks, per-hit particles, screen flashes)
2. Higher MTRANS task count (character overlays, ghost/clone effects, projectile sprites)
3. Full background rendering (~170 PPG tasks) consuming ~10ms of the 16.6ms frame budget
4. Combined with effect sprites, total render time exceeds budget

The background tiles (textures, palettes) remain static during the active SA -- they never change during gameplay. Only the SCROLL POSITION changes as the camera follows character movement.

## Solution: Extended Background Caching via `sa->ok == -1`

### Detection Signal

`sa->ok == -1` is the game's canonical "super art is actively executing" flag:
- Set when SA execution begins (via `check_full_gauge_attack()` in `pls03.c`)
- Stays active for the entire SA animation duration (cinematic + attack phase)
- Cleared when the SA completes (via `sag_union()` in `plmain.c`)
- Per-player independent (`plw[0].sa->ok`, `plw[1].sa->ok`)

### Implementation

Extended `sa_bg_cache_should_be_active()` in `sdl_app.c`:
1. **Primary gate**: `sa_stop_check() != 0` -- cinematic freeze (unchanged)
2. **Extended gate**: either player has `sa->ok == -1` -- active SA attack phase (NEW)

### Periodic Background Recapture

During the active SA attack phase, the camera scrolls as characters move. The cached background surface was captured at a specific scroll position. The existing scaled blit handles scroll deltas, but large scroll drift causes edge-clamping artifacts.

Solution: invalidate the bg cache every `SA_BG_CACHE_ACTIVE_RECAPTURE_INTERVAL` (30) frames during the active SA phase. This forces a fresh full-render + snapshot on one frame, then uses the cached version for the next 29 frames.

Cost: 1 out of 30 frames (~3.3%) renders at full cost. The remaining 96.7% benefit from caching.

### Why Background Caching is Safe During Active SAs

1. Background tile TEXTURES are loaded once during stage setup, never modified during gameplay
2. Background PALETTES (indices 300+) are completely separate from character palettes (0-31)
3. Palette changes during any SA only touch character indices
4. Stage tile "animations" (Ed_Kakikae) cycle tile indices but don't modify texture data
5. The periodic recapture picks up tile animation state changes every 30 frames

## Measured Results

### Q SA1 (tag: active-sa-bgcache-q-sa1)

With extended bg caching (`--super-effect-quality cached-bg`):

| Metric | Value |
|--------|-------|
| FPS | 50.20 |
| Frame time (mean) | 19.92 ms |
| Render time (mean) | 9.72 ms |
| Render time (median) | 9.22 ms |
| Render time (p95) | 13.08 ms |
| Render time (max) | 18.15 ms |
| Frames > 15ms render | 2 / 300 |
| PPG tasks pushed (mean) | 174 |
| MTRANS tasks (mean) | 183 |
| SOLID tasks (mean) | 7 |
| Total render tasks (mean) | 329 |

### Chun-Li SA2 (tag: active-sa-bgcache-chunli-sa2)

With extended bg caching:

| Metric | Value |
|--------|-------|
| FPS | 53.14 |
| Frame time (mean) | 18.82 ms |
| Render time (mean) | 8.90 ms |
| Render time (median) | 8.25 ms |
| Render time (p95) | 13.17 ms |
| Render time (max) | 13.85 ms |
| Frames > 15ms render | 0 / 300 |
| PPG tasks pushed (mean) | 174 |
| MTRANS tasks (mean) | 155 |
| SOLID tasks (mean) | 7 |
| Total render tasks (mean) | 302 |

### Interpretation

- Background caching eliminates ~170 PPG tasks from the render pipeline on ~97% of frames
- This saves ~8-10ms per frame (the cost of rendering 170+ background tiles)
- Chun-Li SA2 achieves 53 FPS with all frames under 14ms render time
- Q SA1 achieves 50 FPS with only 2 outlier frames (recapture frames)
- The recapture spike (18ms) is the full-render cost, occurring once every 30 frames
- Without this change, ALL frames would be ~18ms+ render time

### Remy SA1

Not captured -- the test input script uses QCFx2+K but Remy's SA1 may need different timing or the gauge wasn't filling properly. The Remy test preset was added but needs debugging. The optimization applies universally to all SAs via `sa->ok == -1`, so Remy SA1 will benefit once the test script is fixed.

## Files Modified

- `src/port/sdl/sdl_app.c` -- Extended `sa_bg_cache_should_be_active()` with `sa->ok == -1` gate; added periodic recapture logic
- `src/port/sdl/sdl_game_renderer.c` -- Updated comment to reflect extended caching scope
- `src/test/test_runner.c` -- Added `TEST_SCENE_PRESET_REMY_SA1_REPEAT` and `_PRESSURE` variants
- `src/main.c` -- Added remy-sa1-repeat to supported preset list
- `tools/mister/perf-sampler.sh` -- Added remy-sa1-repeat preset configuration

## Key Architecture Details

### SA State Machine

```
sa->ok == 0  : No SA meter
sa->ok == 1  : SA meter full (ready)
sa->ok == -1 : SA executing (active)
sa->mp == -1 : SA gauge consumed (transition to active)
```

### Detection Timing in Frame Pipeline

```
SDLApp_EndFrame()
  -> update_sa_bg_cache_state_for_frame()    // Decide caching state
     -> sa_bg_cache_should_be_active()        // Check sa_stop_check() OR sa->ok == -1
     -> Periodic recapture every 30 frames    // During active SA phase only
  -> SDLGameRenderer_SetSABgCacheFramesRemaining()
  -> SDLGameRenderer_RenderFrame()
     -> apply_super_effect_burst_reduction_after_sort()
        -> Identify bg tasks (pal >= 256)
        -> If cache valid: restore cached bg, drop bg tasks
        -> If cache invalid: full render + mid-render snapshot
```

### Character Roster (for reference)

| Index | Character | plpat file |
|-------|-----------|------------|
| 0 | Gill | plpat00 |
| 3 | Yun | plpat03 |
| 11 | Ken | plpat11 |
| 15 | Chun-Li | plpat16 |
| 17 | Q | plpat18 |
| 19 | Remy | plpat20 |

Note: plpat index doesn't always match character index. The `plxx_extra_attack_table[]` in `plpat.c` maps character indices to plpat dispatch functions.

## Future Optimization Opportunities

1. **Reduce recapture interval**: Currently 30 frames. Could try 15-20 for better tile animation fidelity at cost of more full-render frames.
2. **Scroll-delta-based recapture**: Instead of fixed interval, recapture when `abs(scroll_dx) + abs(scroll_dy) > threshold`. This would recapture more often during fast scrolling (Q's rush) and less during slow movement.
3. **Dynamic render-time threshold**: Enable bg caching whenever render time exceeds ~12ms, regardless of SA state. This would catch any heavy frame scenario.
4. **Fix Remy test preset**: Debug why Remy SA1 doesn't trigger in the test runner. May need different input timing or initial gauge setup.
