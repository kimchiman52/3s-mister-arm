# MiSTer Performance: Dark Render Deep Research (2026-03-21)

Sources: Full code review (software_frame_non_integer.c, fbdev_presenter.c, sdl_game_renderer.c),
loop150-yun-onset-r1.json telemetry analysis, complete living-findings.md + mister-perf-opportunities.md
review, 149-loop git history, ARM Cortex-A9 architecture research, graphics rendering literature survey.

---

## Audit Status (2026-03-23)

- This memo is now a partial historical hypothesis, not a clean current queue contract.
- Important correction: Loop `164+` lower-distortion basic-mode captures did **not** prove that MiSTer moved off the software-frame path. Current code and capture metadata show those runs still stayed on `software-frame on`, `owned`, `direct-present`, with zero fallback/readback.
- The real process bug is narrower but still serious: several software-frame workload counters, including `software_frame_candidate_tasks` and `software_frame_fast_non_integer_tasks`, are gated behind extended stats and therefore read as zero in basic-mode lower-distortion captures even while the software-frame path is still active.
- Therefore any argument that treats zeroed basic-mode software-frame workload counters as evidence of an SDL/GPU render path, or as proof that software-frame work disappeared, is invalid.
- The remaining useful part of this memo is the queue-shaping question: whether there is still a meaningful render-time cost outside the sampled hot helper buckets on the real software-frame path. That question must now be re-audited without mixing full-export accounting and lower-distortion basic-mode accounting as if they were the same measurement regime.

## The Most Important Discovery: The "Dark Render"

During the Yun SA3 Genei-Jin cold burst (first 8 frames, 19.23 FPS):

| Phase | Per-frame | % of frame |
|---|---|---|
| **frame_time** | **52.0 ms** | 100% |
| update | 10.9 ms | 21% |
| **render** | **40.6 ms** | **78%** |
| present | 0.5 ms | 1% |

But within render, the **sampled** non-integer work breaks down as:

| Component | Per-frame (8-frame total / 8) |
|---|---|
| Row raster (actual pixel work) | **7.1 ms** |
| Lookup generation (float math) | **1.1 ms** |
| Reuse telemetry overhead (measurement) | 10.3 ms |
| Generic textured rasterization | 1.2 ms |
| **Total accounted** | **~19.7 ms** |
| **UNACCOUNTED render time** | **~20.9 ms** |

Historical conclusion, now narrowed by audit:

- On the older full-export Loop `150` capture, about `~21 ms` of per-frame render time was not attributed to the sampled pixel buckets that memo was examining.
- That does **not** mean later lower-distortion basic-mode captures found a different renderer path.
- It also does **not** mean the whole unaccounted render gap survived unchanged once measurement overhead was reduced.
- The safe surviving statement is narrower: full-export telemetry exposed a large accounting gap relative to the sampled hot-helper buckets, but later lower-distortion captures changed the measurement regime enough that the memo must be re-audited before using that gap as the basis for another runtime queue.

This "dark render" likely includes:
- Per-task dispatch overhead (717 tasks across 8 frames = ~90 tasks/frame)
- Stack allocation of ~2.8KB lookup tables per task call (90 x 2.8KB = 252KB/frame of stack churn)
- Texture cache refresh when new sprites appear during Genei onset
- SDL surface locking/unlocking
- Task sorting (insertion sort or qsort)
- Software frame surface management
- L1/L2 cache thrashing between tasks

---

## Re-Audit Guidance

- Use full-export captures only for claims that truly depend on extended workload accounting.
- Use lower-distortion basic-mode captures for route validation and relative same-regime comparisons, not for absolute software-frame workload totals unless the required counters are explicitly backfilled.
- If a future loop wants to reopen this memo as runtime guidance, it must first show one bounded candidate whose premises survive that accounting correction.
- If no such candidate survives, return to the current whole-window super-effect-quality queue instead of forcing another software-frame micro-opt from this memo.

## What's Actually Been Tried vs What Hasn't

**Tried extensively (149 loops):** Inner pixel loop optimizations — pairs, run batching, alpha
shortcuts, opaque paths, threshold tuning, lookup caching, shape-specific admission, compare-dirty
caps, strip merging, presenter template rows, PPG renew-dirty tracking.

**Never tried:** Everything below.

---

## NEW AVENUE 1: Profile the Render Sub-Phases

**Why:** We literally don't know where 21ms/frame is going. Every optimization since loop ~110 has
been guessing at the inner pixel loop, which is only 7.1ms.

**What:** Add lightweight timing around the render sub-phases in `SDLGameRenderer_RenderFrame()`:
- Task sort time
- Per-task dispatch/setup overhead (cumulative)
- Texture cache refresh time (SDL_CreateTextureFromSurface etc.)
- Software frame surface clear/setup
- Exact-copy path time vs non-integer path time

This is a measurement-only change with zero risk. It would rerank the entire optimization queue.

**Files:** `src/port/sdl/sdl_game_renderer.c` (lines ~7301-7358, RenderFrame)

---

## NEW AVENUE 2: Fixed-Point Lookup Generation

**Why:** `populate_non_integer_lookup()` does **per-coordinate floating-point division** on Cortex-A9
(15-25 cycles each). The mapping is a linear function of pixel index — it's doing:
`coord = ((i + offset) - origin) / span` which is just `base + i * step`.

**Current code** (runs 608 times per task x 90 tasks = ~55K float ops/frame):
```c
float coord = ((dst_coordinate + 0.5f) - dst_origin) / dst_span;
out_lookup[i] = (int)SDL_floorf(src_origin + (coord * src_span));
```

**Proposed:** Pre-compute the constant step outside the loop, use fixed-point integer arithmetic
inside:
```c
// One float division per axis per task, not per pixel
const float step = 1.0f / dst_span;
const float base = ((visible_start + 0.5f) - dst_origin) * step;
int32_t coord_fixed = (int32_t)(base * 65536.0f);
const int32_t step_fixed = (int32_t)(step * 65536.0f);
for (int i = 0; i < visible_count; i++) {
    int normalized = clamp(coord_fixed, 0, 65535);
    out_lookup[i] = clamp(
        (int)((src_origin_fixed + (int64_t)normalized * src_span_fixed) >> 16),
        0, src_limit);
    coord_fixed += step_fixed;
}
```
Eliminates ~55K float operations per frame. Lookup generation is currently 1.1 ms/frame — this
could cut it to ~0.2-0.3 ms.

**Risk:** Low. Must verify parity (integer rounding matches float floor).

**Files:** `src/port/sdl/software_frame_non_integer.c` (lines 187-206, `populate_non_integer_lookup`)

---

## NEW AVENUE 3: Static/Persistent Lookup Buffers

**Why:** Each of the ~90 task calls allocates 2.8KB on the stack:
```c
int src_x_lookup[384];              // 1536 bytes
int src_y_lookup[224];              // 896 bytes
Uint8 same_source_pair_lookup[384]; // 384 bytes
```
These are written, read once, then abandoned. With 90 calls, that's 252KB of stack memory being
allocated, written, and evicted from L1 cache every frame. The constant cache pollution reduces
effective L1 capacity for the actual pixel work.

**Proposed:** Use static buffers (file-scope) instead of stack-allocated arrays. The memory persists
across calls, staying warm in cache. Since rendering is single-threaded, there's no race condition.

**Risk:** Very low. Same semantics. Single-threaded guarantee.

**Files:** `src/port/sdl/software_frame_non_integer.c` (lines 254-256)

---

## NEW AVENUE 4: Task-Level Y Lookup Sharing

**Why:** Many of the 64 families share the same vertical scaling parameters (same source height 32,
similar destination heights 34-37). The Y lookup table is regenerated from scratch for each task
even when consecutive tasks have identical Y mapping parameters.

**Proposed:** Cache the last Y lookup and its parameters. If a new task has the same Y mapping, skip
the Y lookup generation entirely. Given the family clustering (ix81 tex57 appears with 4 different
palettes but same geometry), this could skip Y lookup for 50-70% of tasks.

**Risk:** Very low. Pure memoization.

**Files:** `src/port/sdl/software_frame_non_integer.c` (lines 274-282)

---

## NEW AVENUE 5: Investigate Update Phase (10.9 ms)

**Why:** Nobody has looked at `update` during Genei onset. 10.9 ms is 21% of frame time. During
super art activation, the game runs particle systems, gauge updates, animation state machines. If
some of that work is redundant or optimizable, it's a completely untapped 10.9 ms budget.

**What:** Add sub-phase timing inside `step_0()`:
- `spgauge_cont_main()` time
- Animation update time
- Particle/effect update time
- Collision/physics time

**Files:** `src/main.c` (step_0), `src/sf33rd/Source/Game/engine/spgauge.c`,
`src/sf33rd/Source/Game/rendering/mtrans.c`

---

## NEW AVENUE 6: Task Sorting Overhead

**Why:** With ~90 tasks per frame during Genei onset, and Z-inversions likely from layered effects:
- If Z inversions > 8, it falls to `qsort()` on 90 tasks (O(n log n) with function pointer overhead)
- Each `RenderTask` is a large struct (contains `SDL_Vertex[4]`, rects, etc.) — copying during sort
  is expensive
- Sort could be index-based instead of moving full structs

**Proposed:** Sort an array of indices, then dereference. Reduces sort memory movement by ~10x.

**Risk:** Low. Same output ordering.

**Files:** `src/port/sdl/sdl_game_renderer.c` (lines 5466-5497, sort; lines 38-53, RenderTask struct)

---

## NEW AVENUE 7: Texture Cache Cold-Start Penalty

**Why:** Genei-Jin onset is specifically a "cold burst" — new sprites appearing that weren't on
screen before. The texture cache might be creating new SDL textures or refreshing dirty textures
during these frames. Each `SDL_CreateTextureFromSurface()` or surface lock/unlock has kernel and
GPU overhead.

**What to measure:** Count `texture_creates`, `texture_cache_misses`, and dirty-texture refreshes
per frame during the first 8 Genei frames vs steady-state.

**Files:** `src/port/sdl/sdl_game_renderer.c` (lines ~8933-9019, SetTexture / cache management)

---

## NEW AVENUE 8: LTO (ThinLTO) — Still Untried

**Why:** Build uses `-O3` with Clang 20 but NO link-time optimization. The hot path crosses module
boundaries: `sdl_game_renderer.c` calls `SDLSoftwareFrame_RasterNonIntegerLookupARGB8888()` in
`software_frame_non_integer.c`. Without LTO, the compiler can't inline `blend_argb8888()`,
`modulate_argb8888()`, or the entire raster function. With ThinLTO, these become inlining candidates.

**What to add** in `CMakeLists.txt`, inside the existing ARM hardening `if` block:
```cmake
target_compile_options(3sx PRIVATE -flto=thin)
target_link_options(3sx PRIVATE -flto=thin)
```

**Risk:** Low-medium. Need to verify linker compatibility in Docker build. Run parity check before
measuring FPS. ThinLTO occasionally misbehaves with `-fno-strict-aliasing` code from PS2-origin
types.

**Files:** `CMakeLists.txt` (lines ~170-178, ARM hardening block)

---

## NEW AVENUE 9: Compiler Hints (Zero Code Change Risk)

**Why:** The hot path has ZERO compiler hints:
- No `__restrict` on any pointer — compiler assumes aliasing, preventing optimizations
- No `__builtin_expect` (likely/unlikely) on alpha checks — branch prediction can't be guided
- No `__attribute__((hot))` on the raster function
- No `__builtin_prefetch` anywhere in the render path

**Quick wins:**
```c
// In the inner loop, tell compiler src and dst don't alias:
const Uint32* __restrict src_row = ...;
Uint32* __restrict dst_row = ...;

// Tell compiler the common case is opaque:
if (__builtin_expect(src_a == 0xFFu, 1)) { ... }
```

**Risk:** Extremely low. These are hints, not behavioral changes.

**Files:** `src/port/sdl/software_frame_non_integer.c` (lines 300-384, inner loops)

---

## NEW AVENUE 10: Reduce Telemetry Measurement Distortion

**Why:** The reuse telemetry overhead (10.3 ms/frame) is larger than the actual raster work
(7.1 ms/frame). This means capture runs are ~30% slower than production. The optimization decisions
made from capture data are based on a frame time that's inflated by measurement overhead. The
relative ranking of bottlenecks may be different in production.

**Proposed:** Add a "lightweight capture" mode that measures timing without running
`note_non_integer_row_reuse_telemetry()`. Use this for keep/reject decisions while keeping full
telemetry as a separate diagnostic tool.

**Files:** `src/port/sdl/software_frame_non_integer.c` (lines 105-185, reuse telemetry;
lines 310-318, per-row telemetry call)

---

## Recommended Priority Order

1. **Profile the dark render** (measurement-only, zero risk) — highest-value investigation because
   it could reveal that the real bottleneck is something nobody has looked at yet

2. **Fixed-point lookup generation** (low risk) — eliminates ~55K float ops/frame, directly reduces
   the 1.1ms lookup budget

3. **Static lookup buffers** (very low risk) — reduces cache pollution from 252KB/frame of stack
   allocation

4. **Compiler hints** (`restrict`, `likely`) — free performance from better compiler optimization,
   zero behavioral change

5. **LTO build experiment** — enables cross-module inlining of the entire hot path

6. **Task-level Y lookup memoization** — skip redundant Y lookup for families with identical
   vertical scaling

7. **Investigate update phase** — 10.9ms of untapped optimization potential

---

## Key Architectural Insight

**Stop optimizing the 7.1ms inner loop and start profiling the 21ms of mystery render time.**

The ralph loops have been drilling deeper and deeper into the non-integer pixel rasterization inner
loop — pairs, run batching, alpha shortcuts, threshold tuning — and every attempt has produced
marginal gains or regressions. The reason is now clear: that inner loop is only ~20% of the render
phase. Even a 2x speedup of the inner loop would only save ~3.5ms per frame (~2 FPS). The 21ms of
uncharted render time is 3x larger and has never been profiled.

The gather problem (chained indirect load, >=8 cycles/pixel on Cortex-A9) is real and sets a floor
on per-pixel cost. But the bigger opportunity is in the per-task overhead, cache behavior, and
non-raster render work that nobody has measured yet.

---

## Supporting Data

### Yun Onset Family Profile (loop150-yun-onset-r1.json, first 8 frames)

- 64 non-integer families, 717 tasks, ~1.05M total pixels
- ~90 tasks/frame, ~131K pixels/frame
- Top 5 families by sampled time:
  1. ix82 tex58 pal393: 15.71ms total, 72882 pixels, 32x32->37x37
  2. ix81 tex57 pal391: 15.64ms total, 75090 pixels, 32x32->36x36
  3. ix81 tex57 pal394: 10.88ms total, 48198 pixels, 32x16->35x18
  4. ix81 tex57 pal393: 7.38ms total, 36242 pixels, 32x32->35x35
  5. ix1102 tex18 pal37: 7.08ms total, 34475 pixels, 32x16->37x19
- All families: same_source_max_run_length = 2 (pairs only, no longer runs)
- Generic textured: 8 families, 9.61ms total, 50021 pixels (all ix80 tex56)

### Hardware Reference (DE10-Nano)

- Dual-core ARM Cortex-A9 @ 800 MHz (second core idle)
- 32 KB L1 D-Cache per core (4-way, 32-byte lines)
- 512 KB shared L2 (PL310, 8-way)
- NEON VFPv3-D32 (no gather instruction)
- Framebuffer: 32bpp ARGB via `/dev/fb0`, mmap'd MAP_SHARED (write-combining)

### Build Configuration

- Clang 20, `-O3`, `-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard`
- `-fno-strict-aliasing`, NO `-ffast-math`, NO LTO
- NEON usage: only `memset32` in `fbdev_presenter.c:373`; all hot paths are scalar
