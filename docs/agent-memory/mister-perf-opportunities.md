# MiSTer Performance: Remaining Opportunities

Last updated: 2026-03-21
Sources: Full code review (software_frame_non_integer.c, fbdev_presenter.c, sdl_game_renderer.c),
complete living-findings.md search, 130+ loop git history, two research sessions.

---

## March 21 Native Current Truth

- Native presentation is still not the first-line bottleneck on the trusted March `2026-03-21`
  lanes: `loop132-yun-family-time-r2` recorded `present.mean_ms = 0.5300`, and
  fresh aligned Remy rerun `loop146-remy-rerank-r2` recorded `0.5255`.
- Genei / first-visible Yun SA3 remains the dominant current native failure, but the immediate
  runtime queue is no longer the older micro-admission/unroll list. The latest trusted schema-`62`
  onset repro `loop145-yun-shared-shapes-repro-r1` stayed at `25.0873 FPS / 39.8609 / 9.5879 /
  29.7127 / 0.5603 ms` overall and `20.9510 FPS / 47.7304 / 11.2303 / 35.9909 / 0.5093 ms` for
  `capture_windows.first_8_frames`, still with direct present and zero fallback/readback.
- Fresh current-tree replay `loop150-yun-onset-r1` made the native no-retry guidance stricter, not
  looser: it landed at `23.0476 FPS / 43.3885 / 9.6383 / 33.2248 / 0.5253 ms` overall and
  `19.2290 FPS / 52.0047 / 10.8829 / 40.5975 / 0.5243 ms` for the first `8` frames, still direct
  present with zero fallback/readback. The first-`8` generic lane remained only `9.6129 ms` on the
  same `ppg-seqs ix 80 / texture 56` family, the top four shared-shape buckets were still just the
  adjacent `32x32 -> 34/35/36/37` cluster at `28.4546 ms` out of `74.8340 ms`, and the top eight
  lookup profiles covered only `5.3071 ms`. That is still not a safe helper-local reland target on
  the current tree.
- Remy-left remains a different problem from Genei. The fresh deciding rerun
  `loop146-remy-rerank-r2` stayed at `58.0531 FPS / 17.2256 / 8.4721 / 8.2280 / 0.5255 ms`, with
  `fast_non_integer = 0`, `generic_textured = 0`, and the same tiny compare-dirty full-refresh
  tail already kept in Loop `137` (`ix 80 = 13/1992`, `ix 81 = 10/700`, `ix 82 = 6/614`).
- The older March queue is now stale on the current tree. Loop `134` rejected the bounded
  `ix 80 / texture 56` generic admission, Loop `135` rejected the scalar `4x` row-walk unroll,
  Loop `142` rejected the low-density pair gate, Loop `149` closed the narrower palette-specific
  `ix 80` retry angle, and Loop `150` reran the deciding current-tree Yun burst without finding a
  new safe helper-local reland.
- Current native order after Loops `149` / `150`:
  `(1)` do not reopen helper-local Yun admission/unroll/pair or one-cluster shared-shape ideas
  unchanged, `(2)` do not broaden compare-dirty caps again on Remy without new evidence that the
  tiny tail has grown or that the lane stopped staying exact/direct, `(3)` only start another
  native runtime edit after either a broader row-walk redesign is proven with lower-overhead
  measurement or a newly measured gameplay bottleneck clearly outranks both current queues.
- Do not spend the next native loop on presenter-side work, broader same-source run batching beyond
  pairs, dual-core raster threading, or runtime row dedup before measurement.

---

## March 21 Research Synthesis / Debate Amendments

This section preserves the later March `2026-03-21` debate-style review of the new research ideas,
so future loops inherit not just the final ranking but also the reasons some proposals were
accepted, narrowed, or demoted.

### Accepted From The New Research

- **Scalar row-walk unroll is a real next-step candidate.** The most durable new idea from the later
  research/review pass is a careful scalar `4x` unroll in the hot non-integer gather loop. This is
  now preferred over more lookup/pair setup work because Loop 133 showed the meaningful helper cost
  is in row-walk gather time, not in lookup generation or pair-bitmap setup.

- **The small `ix 80 / texture 56` generic residue is worth a bounded audit.** Loop 132 family-time
  telemetry showed the remaining generic lane is secondary but real, and the hot families sit just
  below the current shared `384`-pixel non-integer threshold. That supports a narrow micro-lookup
  admission audit, not another broad threshold-style reland.

- **Remy-left must stay on its own track.** The newer debate clarified that Remy-left and Genei are
  no longer the same bottleneck family. Remy-left is still exact/refresh-bound compare-dirty
  residue around `ppg-seqs 81/82`, so native Genei raster experiments should not be expected to
  move it materially.

### Demoted Or Corrected After Review

- **Row dedup was over-claimed.** The earlier research treated duplicate-`src_y` rows as a likely
  high-confidence win, but the later review corrected that assumption: the trusted Yun families do
  not show a simple uniform vertical-scale pattern, and the safe-copy condition is not guaranteed on
  enough rows to justify calling it a likely runtime win. Keep this as measurement-only until a real
  duplicate-row cohort is proven on a trusted capture.

- **Broader same-source run batching beyond pairs is stale on this tree.** Loop 128 already kept the
  pair-only reuse path, the hot families top out at `same_source_max_run_length = 2`, and the later
  debate agreed that the more ambitious "multiplicative" run-batching framing was no longer
  evidence-backed here.

- **Dual-core raster splitting is not a near-term candidate.** The new research correctly noted the
  second A9 core is idle, but the review also pointed out that the current hot tasks are too small
  and too memory-latency-heavy to justify threading complexity yet. Keep threading ideas off the
  immediate native queue unless a later measurement pass re-ranks CPU work outside this raster loop.

- **Presenter-side native work is not the current lever.** The research review closed this cleanly:
  native present is already near the floor on the deciding March `2026-03-21` captures, so
  presenter-side NEON/unroll work belongs to nearest-HDMI follow-up, not the current native queue.

- **Cold-cache and first-activation one-off stories are secondary, not the root cause.** The later
  debate did not find enough evidence to treat cold creation or one-time setup as the dominant
  sustained explanation for the Genei slowdown. Keep those as possible contributors, not as the main
  ranking signal.

### Quantitative Corrections Worth Remembering

- The later review corrected one ROI estimate for the generic lane: `120.9596 ms` sampled generic
  time over `300` frames is about `0.40 ms/frame`, not `0.10 ms/frame`. That is still secondary to
  the non-integer hotspot, but it is large enough to justify a bounded audit before the heavier
  unroll work.

- The debate also clarified that a global threshold drop is not "free" just because the hot residue
  sits under `384` pixels. Earlier threshold-style expansions have already regressed gameplay on this
  tree, so future work should prefer targeted admission over policy-wide threshold movement.

### Durable Native Queue After Synthesis

Use this order unless newer trusted captures contradict it:

1. Audit the `ppg-seqs ix 80 / texture 56` generic residue and, if justified, try a narrow
   micro-lookup admission instead of another global threshold change.
2. If the generic audit does not clear the deciding Genei lane, test one careful scalar `4x`
   unroll in the non-integer row-walk gather loop while preserving the kept pair-only reuse path.
3. Keep Remy-left on separate compare-dirty `ppg-seqs 81/82` residue work.
4. Keep row-dedup ideas measurement-only, and leave presenter-native, dual-core, and broader
   run-batching ideas off the immediate queue.

## Post-Loop-146 Validation Addendum

This addendum overrides the older idea-ranking sections below for the current `mister-dev` native
queue. Those sections remain useful as historical research context, but they should not be treated
as the active next-step list without fresh validation.

- The `ix 80 / texture 56` generic-residue idea is still the same already-rejected family, not a
  new narrower admission target. On the recovered trusted onset repro
  `loop145-yun-shared-shapes-repro-r1`, the first-`8` generic families stay on
  `texture_handle = 56 / logical_ix_num = 80` with the same `8x8..16x16` or `16x16` source-rect
  shapes expanding to only `9..20` or `17..20` visible spans. That is the same family shape Loop
  `134` already rejected, so do not reland it unchanged.

- The current `384`-pixel gate is still exactly where the surviving generic residue sits. Current
  code keeps `software_frame_non_integer_lookup_threshold_pixels = 384`, and the trusted onset
  capture still shows the same small under-threshold `ix 80` cohort rather than a new isolated miss
  that would justify another policy change or helper-local admission reland.

- Loop `149` closes the narrower “palette-specific `ix 80` retry” angle too. On the trusted
  schema-`63` `loop148-yun-lookup-signatures-r2` capture, first-`8` generic residue totals only
  `9.9682 ms` across eight `ppg-seqs ix 80 / texture 56` palette families, and the largest slice
  is only `palette 393 = 3.0456 ms` (`30.55%` of the generic lane). Loop `134` already made the
  deciding Yun lane slower while admitting a broader superset of that same under-`384` family, so
  do not reopen palette-specific or otherwise narrower sub-`384` admissions without genuinely new
  measured separation.

- The full-telemetry phase split does not revive the old lookup-generation or pair-bitmap queue.
  On `loop145-yun-shared-shapes-repro-r1`, `software_frame_fast_non_integer_phase_sampling`
  reports only `82.7123 ms` total for `lookup_x + lookup_y + pair_lookup` across
  `1319.9161 ms` family-sampled fast-non-integer time. `row_raster` contributes `468.5720 ms`,
  and the largest measured slice (`737.9976 ms`) is the extended-stats reuse-telemetry
  instrumentation itself, not player-runtime work. Do not use that capture-only phase split to
  justify same-source batching, lookup-generation, or pair-setup runtime edits without a lower-
  overhead proof.

- Remy-left root-cause measurement is closed on the current tree. The aligned deciding rerun
  `loop146-remy-rerank-r2` stayed exact/direct at `58.0531 FPS / 17.2256 / 8.4721 / 8.2280 /
  0.5255 ms`, with zero `fast_non_integer`, zero `generic_textured`, and only the tiny stable
  `13 / 10 / 6` full-refresh tail on `ix 80 / 81 / 82`. Do not reopen compare-dirty caps without
  new evidence that this tail grew or the lane stopped staying exact/direct.

- The current Yun onset cluster still points at a broader row-walk specialization problem, not a
  safe bounded reland. The top shared shape on `loop145` still covers only `6.33%` of first-`8`
  sampled fast-non-integer time, the top four adjacent `32x32 -> 34/35/36/37` buckets only
  `17.96%`, and the top ten only `28.27%`. Another helper-local reland would still be guessing at
  a broader clustered redesign rather than acting on a new measured narrow lever.

---

## Current State

**Build:** CMake Release → `-O3`, clang 20, `-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard`,
`-fno-strict-aliasing`. No `-ffast-math`. No LTO.

**NEON usage:** Only `memset32` in `fbdev_presenter.c:373`. Every other hot path — the non-integer
gather loop, the mapped nearest scaler, the integer scale expansion — is scalar.

**Baselines (native, scale-mode = native, March `2026-03-21` trusted anchors):**
- The accepted native software-frame baseline still keeps the ordinary control / heavy frozen matrix
  around or above target; the remaining native outliers are Yun first-visible Genei and Remy-left.
- Remy-left: **54.57 FPS** ✗ — improved materially from the older `~42.5 FPS` state; the remaining
  work is compare-dirty residue in `ppg-seqs 81/82`, not an unknown root cause
- Genei-Jin first visible activation: **41.37 FPS** ✗ — dominant current player-visible native
  failure on the trusted lane
- Native present overhead on both deciding lanes: **~0.53-0.54 ms** — no longer the first-line
  native target

**Present overhead:** Negligible on native/crt-4x3 (direct memcpy, <1ms). Moderate on nearest
(mapped scaler, present.mean_ms ~3-7ms depending on scene).

---

## The Gather Problem — Why Everything Is Hard

The innermost loop of both the non-integer raster path (`software_frame_non_integer.c:158-169`)
and the nearest scaler (`fbdev_presenter.c:710-712`) share the same fundamental shape:

```c
dst[col] = src[lookup[col]];  // chained indirect: lookup → address → pixel
```

On Cortex-A9 (in-order pipeline, L1 latency ~4 cycles): each pixel requires two chained loads —
load `lookup[col]` (4 cycles), then use the result to load `src[...]` (4 more cycles). That's ≥8
cycles/pixel minimum before any alpha check or blend. ARM A9 NEON has **no gather instruction**
(`vtbl` only works with 8-bit indices into 32-byte tables — useless here).

This is why every NEON attempt in the blend path has had limited impact: the gather dominates
per-pixel time, and blending consumes only 10-30% of it. Eliminating blend cost entirely through
opaque-source specialization (tried, living-findings ~line 3386) didn't survive because the gather
latency was already the floor.

---

## Confirmed Dead Ends

### Toolchain Upgrade
Upgraded to Clang 20 — **no measurable performance improvement.** Confirmed: the hot loops have
`continue` branches that block auto-vectorization in any compiler, and the chained indirect load
latency is a hardware constraint that better code generation cannot change. Do not revisit compiler
version as a lever.

### From Living-Findings History

| Attempt | Outcome | Why |
|---|---|---|
| Opaque-source fast path in `non_integer` (full parity reland) | **Rejected** (~line 3386) | Gather is still the floor; alpha check already branch-predicted correctly |
| Opaque row mask on exact-copy path | **Tried** (~line 198) | Branch overhead vs. savings |
| Non-integer threshold (512 kept, 256 rejected, **384 current**) | Done | Diminishing returns |
| Identity-color branch split in `non_integer` | **Rejected** (loop 101) | Extra branch hurt overall |
| Alpha early-out additions | **Rejected** (loop 107) | super-heavy regressed |
| Memcpy alternative in present paths | **Rejected** (loop 117) | SDL_memcpy already NEON-tuned |
| Equal-Z queue, dirty-tile, opaque-row rejection in renderer | **All rejected** (loops 115-119) | No net gain or regressions |
| All template replay body shapes for nearest (6+ attempts) | **Rejected** | Reordering swaps costs without net gain |
| Dense-band template seeding widening | **Rejected** | First-row overhead cancels repeat savings |
| PPG renew-dirty seq-specific reland | **Rejected** (loop 118) | Caused corruption |
| INDEX8 direct surface converter | **Rejected** (loop 22) | — |
| Sparse palette-guard shortcut | **Rejected** (loop 37) | — |

---

## What Has Not Been Tried

Confirmed absent from living-findings after exhaustive keyword search (`NEON`, `gather`, `prefetch`,
`unroll`, `lto`, `LTO`, `ffast`, `two-pass`, `run batch`, `BGM`, `pthread`, `second core`).
This section is historical to the memo's first draft; use the March `2026-03-21` current-truth
section above plus the validation addendum below when it disagrees with later loop evidence.

---

## Opportunities — Ordered by Confidence

---

### 1. Remy Stage Root-Cause (Measurement — No Code Change)

**Why first:** Remy at 42.5 FPS is the largest gap. Two attempted runtime fixes (Loop 118, 119)
failed because neither targeted the actual bottleneck. Until the bottleneck is isolated, any runtime
attempt is a guess.

**What to do:** Run full-telemetry `gameplay-remy-stage` capture. Determine:
- Is it update-heavy (game logic), render-heavy (raster task count/type), or present-heavy?
- Which task families dominate: `fast_exact`, `fast_non_integer`, `generic_textured`, other?
- Compare task counts vs. control stage to find Remy-specific excess.

**Files:** `tools/mister/perf-sampler.sh`, `src/test/test_runner.c`, `src/main.c`

---

### 2. Horizontal Same-Source-Pixel Run Batching in Non-Integer Raster

**Why untried and promising:** During horizontal non-integer upscaling, many consecutive dst pixels
map to the same src pixel (at 1.5× scale, some src pixels span 2 adjacent dst cols). The current
inner loop does a full lookup + alpha check + blend per dst pixel even when `src_x_lookup[col] ==
src_x_lookup[col+1]`. This run structure is never exploited. **Never attempted.**

**Proposed change** (`src/port/sdl/software_frame_non_integer.c`):
```c
for (int col = 0; col < visible_w; ) {
    const int src_col = src_x_lookup[col];
    const Uint32 src_pixel = src_row[src_col];
    const Uint32 src_a = (src_pixel >> 24) & 0xFFu;

    // find run length where the same src column repeats
    int run_end = col + 1;
    while (run_end < visible_w && src_x_lookup[run_end] == src_col) run_end++;

    if (src_a == 0u) {
        col = run_end;  // skip entire run, no writes
        continue;
    }
    if (src_a == 0xFFu) {
        for (int r = col; r < run_end; r++) dst_row[r] = src_pixel;
        col = run_end;
        continue;
    }
    // semi-transparent: dst pixels may differ, must blend per-pixel,
    // but src lookup and alpha decode are done only once per run
    for (int r = col; r < run_end; r++) {
        dst_row[r] = blend_argb8888(dst_row[r], src_pixel);
    }
    col = run_end;
}
```
Opaque and transparent runs get a true batch skip. Semi-transparent still pays per-pixel blend
but eliminates the repeated lookup + alpha decode for run pixels.

**Expected win:** Highest during Genei-Jin fog textures (horizontal tiling). Add a run-length
counter before implementing to confirm run frequency justifies the inner `while`.

**Risk:** Low. Must verify parity. Does not change blend correctness for distinct source pixels.

**Files:** `src/port/sdl/software_frame_non_integer.c`

---

### 3. LTO (ThinLTO)

**Why:** LTO is not enabled. Adding `-flto=thin` lets Clang inline across the
`sdl_game_renderer.c` → `software_frame_non_integer.c` boundary and improves constant-propagation
through the blend path. Zero changes to hot files.

**What to add** in `CMakeLists.txt`, inside the existing ARM hardening `if` block:
```cmake
target_compile_options(3sx PRIVATE -flto=thin)
target_link_options(3sx PRIVATE -flto=thin)
```
Use ThinLTO (not full LTO) to keep Docker build link time reasonable.

**Risk:** LTO occasionally misbehaves with `-fno-strict-aliasing` code from PS2-origin types.
Run parity check (`--software-frame-parity-check`) before measuring FPS; roll back if it fails.

---

### 4. NEON 4-Way Unroll in `rasterize_mapped_row_tile_runs` (Nearest Mode)

**Location:** `fbdev_presenter.c:710-712`

**The loop:**
```c
for (int x = dst_x0; x < dst_x1; x++) {
    dst_row[x - dst_x0] = src_row[scale_x_lut[x]];
}
```
Processes up to 640 dst pixels per changed row. Each iteration ≥8 cycles on A9. Manual 4-way
unroll issues 4 independent gather chains that A9's pipeline can overlap:

```c
int x = dst_x0;
for (; x + 3 < dst_x1; x += 4) {
    const int i0 = scale_x_lut[x],   i1 = scale_x_lut[x+1];
    const int i2 = scale_x_lut[x+2], i3 = scale_x_lut[x+3];
    const Uint32 p0 = src_row[i0], p1 = src_row[i1];
    const Uint32 p2 = src_row[i2], p3 = src_row[i3];
    uint32x4_t out = {p0, p1, p2, p3};
    vst1q_u32(dst_row + (x - dst_x0), out);
}
// scalar tail
```
Collapses 4 chains from `4×8 = 32 cycles` toward `~12-14 cycles` = 3-3.5 cycles/pixel.
`vst1q_u32` is efficient for write-combining framebuffer.

**Expected gain:** 30-50% speedup on nearest changed-row rasterization. ~0.5-1.5 FPS on nearest
heavy scenes (Genei-Jin lanes, mapped_first_row.mean_ms ~2ms).

**Risk:** Low. Same semantics, identical pixel values. Parity unaffected.

---

### 5. Two-Pass Gather + NEON Blend in Non-Integer Raster

**Why:** The gather prevents NEON from touching the current single-pass loop. A two-pass structure
decouples gather from blend, making pass 2 sequential-access on both src and dst — NEON-eligible.

**Pass 1 (gather — same cost as today):**
```c
Uint32 temp[384];  // stack, fits in L1 (1.5KB)
for (int col = 0; col < visible_w; col++) {
    temp[col] = src_row[src_x_lookup[col]];
}
```

**Pass 2 (blend — sequential access, NEON vectorizable):**
```c
// vld1q_u32 loads 4 temp pixels; vld1q_u32 loads 4 dst pixels
// vmull_u8: src×alpha and dst×inv_alpha (8-bit × 8-bit → 16-bit, 8 channels at once)
// divide-by-255: (t + (t >> 8) + 1) >> 8
// vshrn_n_u16 pack back, vst1q_u32 store 4 results
```

**Why this differs from the rejected opaque-source reland:** That attempt bypassed blend for
fully-opaque tasks. This applies NEON to the blend for ALL tasks — including the semi-transparent
heavy scenes that are the actual ceiling. The two-pass doesn't change which pixels are processed;
it restructures memory access to enable vectorization.

**Expected gain:** 1.5-3× speedup on the blend portion. If blend is 30-50% of render time for
effect-heavy/super-heavy: 0.5-1.5 FPS on the hardest scenes. Measure blend-vs-copy split first
(see opportunity 7 below) before implementing.

**Risk:** Medium. Must satisfy parity checker — either prove the NEON division approximation is
bit-exact with `(x + 127) / 255`, or update expected values. Stack 1.5KB per call is safe.

---

### 6. BGM Audio Offload to Second Core

**Why:** DE10-Nano has dual-core Cortex-A9. The second core is completely idle. `BGM_Server()` is
called synchronously on the main thread every frame at `src/main.c:844`, driving
`ADX_ProcessTracks()` in `src/port/sound/adx.c`.

**Step 1 — measure first:** Add `perf_bgm_start_ns` / `perf_bgm_end_ns` around `BGM_Server()` in
`sdl_app.c` frame profiling. If < 0.2ms → not worth threading complexity. If > 0.5ms → attractive.

**Step 2 — if worth it:** Move `BGM_Server()` to a background pthread pinned to core 1. Ring-buffer
handoff with the SDL audio callback thread. Audit whether `BGM_Server()` reads any shared game
state before touching thread boundaries.

**Files:** `src/main.c`, `src/port/sound/adx.c`, `src/port/sdl/sdl_app.c`

**Risk:** High if implemented without measurement first. Threading + shared state is a correctness
risk. Do not implement without telemetry proof that it's a meaningful cost.

---

### 7. Measure Blend-vs-Copy Split Before NEON Blend Work

**Why it matters:** If 90%+ of heavy-scene pixels are opaque (the branch predictor already handles
them cheaply — consistent with the rejected opaque-source reland), the blend path is cold and
opportunity 5 is low ROI. If 30%+ hit the semi-transparent path, it's high ROI.

**What to measure:** For effect-heavy and super-heavy: breakdown of `non_integer` raster time
between opaque-copy pixels and actual blend pixels. The `perf_capture_raster_sample_ns` buckets
should get close; a dedicated blend-call counter may be needed.

---

### 8. NEON Horizontal Expansion in Integer Scale Path

**Location:** `fbdev_presenter.c:1667-1699`, `copy_argb_surface_integer_scaled_to_fb_rect`

**The loop:**
```c
for (int src_x = 0; src_x < argb->w; src_x++) {
    const Uint32 pixel = src_row[src_x];
    for (int repeat_x = 0; repeat_x < scale_x; repeat_x++) {
        dst_row[dst_x++] = pixel;
    }
}
```
For `scale_x = 2`: `vdupq_n_u32(pixel)` + `vst1q_u32` writes 4 copies at once. Sequential src
read + broadcast write — fully vectorizable. Currently zero NEON here.

**Expected gain:** 1.5-2× on integer-scale horizontal expansion. Impact depends on which modes
hit this path (not native/crt-4x3 exact cases).

**Risk:** Very low. Same semantics.

---

### 9. Smaller / Measurement-First Items

- **`__builtin_prefetch` in `non_integer` inner loop:** For 384-wide textures (1.5KB) the entire
  surface fits in L1 — likely no benefit. For 256×256 textures (256KB), prefetching
  `&src_row[src_x_lookup[col + 16]]` could hide L2 latency. Low effort, zero risk.

- **`texture_handle 77` clipped-helper residue:** A persistent `128×48` residue stays in
  `generic_textured` after threshold changes. Export exact dst rect in full-telemetry Genei capture
  to confirm whether clipping puts it below the 512px threshold.

- **Attract/logo exact gate:** Broad attract captures show ~52.91 FPS but Loop 109 couldn't narrow
  the gate. Export `D_No[*]`, `title_tex_flag`, `op_w` to perf JSON to isolate the exact overlay
  window.

- **CRT-4x3 present baseline:** The 384→640 (1.667×) non-integer present path has never been
  directly profiled against native. Run `perf-sampler.sh --scale-mode crt-4x3` against control,
  stage-heavy, effect-heavy. Compare `present.mean_ms` and `copy_bytes` to native equivalents.

- **Compare-dirty partial refresh (loop 124 current):** The living-findings memo (line 3633) points
  to "remaining oversized/full compare-dirty residue in Remy PPG seqs 81/82." Orthogonal to the
  rasterization directions above — complete the 3/8 cap evaluation before broadening.

---

## Suggested Loop Sequence

| Loop | Target | Type | Primary File | Risk |
|------|--------|------|-------------|------|
| A | Remy root-cause | Measurement | `perf-sampler.sh`, `test_runner.c` | None |
| B | Remy runtime (post-A) | Runtime | TBD from A | TBD |
| C | Horizontal run batching | Runtime | `software_frame_non_integer.c` | Low |
| D | LTO flag | Build | `CMakeLists.txt` | Low–Med |
| E | BGM_Server time audit | Measurement | `sdl_app.c` | None |
| F | CRT-4x3 baseline | Measurement | `perf-sampler.sh` | None |
| G | NEON nearest unroll | Runtime | `fbdev_presenter.c` | Low |
| H | Blend-vs-copy split measurement | Measurement | `sdl_app.c` | None |
| I | Two-pass NEON blend (if H warrants) | Runtime | `software_frame_non_integer.c` | Med |
| J | BGM pthread (if E warrants) | Runtime | `main.c`, `adx.c`, `sdl_app.c` | High |

---

## Key Architectural Takeaway

The gather is the fundamental ceiling on Cortex-A9. Every attempt that reduced per-pixel branch or
blend cost without changing the gather pattern produced marginal gains — those operations live in the
shadow of the 2-level chained load latency (≥8 cycles/pixel). The compiler cannot fix this; Clang 20
confirmed it. The remaining levers are: (a) exploit repeated source pixels to skip redundant gathers
(run batching — item 2), (b) hide gather latency through independent parallel chains (NEON unroll —
items 4, 8), and (c) offload unrelated synchronous work from the main thread (BGM — item 6).

---

## Independent Validation Addendum (2026-03-20)

This section preserves the original memo above and records a separate repo-plus-primary-source review.
Use it as the current truth when the original text and this addendum disagree.

### Global Corrections

- **Remy status is partly stale.** The memo's `~42.5 FPS` Remy baseline no longer matches the kept
  compare-dirty reland in `living-findings.md`: loop 123 raised native Remy from `38.9423` to
  `52.2588 FPS` and explicitly narrowed the remaining work to oversized/full compare-dirty residue
  in Remy `ppg-seqs 81/82`. Re-rank future Remy work from that kept state, not from a fresh
  "unknown bottleneck" assumption.

- **The gather diagnosis is directionally right, but the Cortex-A9 shorthand is too absolute.**
  Arm's own Cortex-A family guidance describes Cortex-A9 as a mid-range core with *partial*
  out-of-order execution, not a purely in-order machine. The important takeaway still holds:
  chained dependent loads are the real problem, and compiler upgrades alone are unlikely to erase
  that cost.

- **`vtbl` is still not a gather replacement here.** Arm's NEON intrinsics reference confirms the
  A32 `vtbl1`..`vtbl4` family is byte-table lookup over `8`, `16`, `24`, or `32` bytes total. That
  is useful evidence against trying to vectorize `src_row[src_x_lookup[col]]` directly with table
  lookups.

### Opportunity-by-Opportunity Validation

#### 1. Remy Stage Root-Cause

- **Revised verdict:** stale as written; partially superseded.
- **Why:** the repo already has a kept Remy-specific fix path in compare-dirty retained refresh
  work. The best next Remy step is the memo's own smaller item about remaining compare-dirty
  residue, not a restart from zero.
- **Updated framing:** "finish Remy `ppg-seqs 81/82` compare-dirty residue" should outrank
  "measure Remy root cause."

#### 2. Horizontal Same-Source-Pixel Run Batching in Non-Integer Raster

- **Verdict:** strongest new runtime candidate in this memo.
- **Why it still looks good:** the current loop in `software_frame_non_integer.c` really does repeat
  `src_row[src_x_lookup[col]]` per destination pixel, so batching repeated `src_x_lookup` runs is a
  structurally different attack from the already-rejected branch-only and opaque-only work.
- **Validation caveat:** add a run-length histogram first so future loops can prove the repeated
  source-pixel cohort is large enough on the real Genei/effect-heavy lanes.

#### 3. ThinLTO

- **Verdict:** technically valid but lower confidence than the memo implies.
- **Why:** Clang ThinLTO absolutely can enable cross-module importing and inlining, but it also
  requires a supported linker path. This repo does not currently configure `gold`/plugin or `lld`
  explicitly, and an existing MiSTer build cache still points at plain `/usr/bin/ld`.
- **Practical implication:** treat ThinLTO as a build experiment first, not as a near-certain FPS
  win. Expect build-system work before any meaningful measurement.

#### 4. NEON 4-Way Unroll in `rasterize_mapped_row_tile_runs`

- **Verdict:** plausible, but the memo likely over-credits NEON itself.
- **Why:** the hot present loop in `fbdev_presenter.c` is exactly the kind of dependent-load code
  where multiple independent chains may help. The likely win is from manual unrolling and better
  overlap of independent lookups, not specifically from packing values into a NEON register before
  storing.
- **Recommendation:** if this is tested, benchmark a scalar 4x unroll and a NEON-assisted variant.
  Do not assume `vst1q_u32` is the deciding factor without measurement.

#### 5. Two-Pass Gather + NEON Blend

- **Verdict:** medium-to-high risk; not ready to rank above easier work.
- **Why:** the current blend math is stricter than the memo's sketch suggests. The memo proposes the
  common `/255` approximation `(t + (t >> 8) + 1) >> 8`, but that is **not** bit-exact with the
  current scalar `(x + 127) / 255` form used in the raster helper.
- **Verified parity note:** a local brute-force check over `0..65025` found `32,385` mismatches
  between those two formulas. If a future NEON path needs a bit-exact divide-by-255 replacement,
  `((x + 128) * 257) >> 16` matched the current scalar rounding in the same local check.
- **Updated ranking:** do not attempt this until source-alpha mix telemetry proves the blended-pixel
  cohort is large enough to matter.

#### 6. BGM Audio Offload to Second Core

- **Verdict:** the memo is probably targeting the wrong function.
- **Why:** `BGM_Server()` does run on the main-thread update path in `src/main.c`, but the actual
  audio decode/queue work on the SDL side happens in `ADX_ProcessTracks()` during
  `SDLApp_EndFrame()`, and the heavy ADX decode/queue loops live in `src/port/sound/adx.c`.
- **Updated framing:** if audio CPU time becomes a serious candidate, instrument
  `ADX_ProcessTracks()` first. Moving only `BGM_Server()` off-thread is unlikely to move the real
  decode cost.
- **Correctness caution:** SDL3 audio streams are thread-safe to use from any thread, but the game's
  BGM state machinery is not currently structured with explicit synchronization. Treat a threaded BGM
  redesign as high-risk even if the SDL side is cooperative.

#### 7. Measure Blend-vs-Copy Split Before NEON Blend Work

- **Verdict:** still important, but the memo overstates what current telemetry already tells us.
- **Why:** the existing fast-non-integer counters classify task color state such as alpha-only color
  modulation and RGB modulation, not per-pixel source alpha composition. They do not yet answer
  "what share of pixels actually blended?"
- **Recommendation:** add dedicated source-alpha pixel counters for `src_a == 0`, `src_a == 255`,
  and blended pixels if this lane is reopened.

#### 8. NEON Horizontal Expansion in Integer Scale Path

- **Verdict:** technically strong, likely low current ROI.
- **Why:** the integer-scale expansion loop in `fbdev_presenter.c` is genuinely vector-friendly. The
  reason it ranks lower is not feasibility but scene relevance: it is not the main bottleneck for
  native gameplay, nearest-HDMI Genei, or the current Remy residue.

### Smaller-Item Corrections

- **`__builtin_prefetch`:** still measurement-first. GCC documents it as a latency-hiding hint, not a
  guaranteed win. With dependent lookup chains, useful lead time may be too short to matter.

- **`texture_handle 77` residue:** worth measuring, but the memo says "below the 512px threshold"
  even though the current shared threshold is `384`. Keep the residue note, but correct the threshold
  when this is revisited.

- **Attract/logo exact gate:** partly solved already. The requested `D_No[*]`, `title_tex_flag`, and
  `op_w` export was already added in loop 109. The remaining issue is recovering a trustworthy wait
  gate, not re-adding the export.

- **CRT-4x3 present baseline:** worthwhile, but the current wording is inaccurate in two ways:
  first, true analog `crt-4x3` does not imply the same "direct memcpy" shape as native exact;
  second, `tools/mister/perf-sampler.sh` does not currently accept `crt-4x3` as a `--scale-mode`
  override even though `docs/config.md` documents the mode.

### Updated Priority Order

If future work is ranked from the validated state above, the strongest sequence is:

1. Do not start a new native runtime reland on current `mister-dev` without new evidence. The old
   micro-admission, scalar-unroll, pair-density, and broad compare-dirty-cap ideas are all already
   rejected or superseded on the current tree.
2. If another native loop is required, first add or recover only the narrowest measurement that can
   prove a broader clustered row-walk specialization across the adjacent `32x32 -> 34/35/36/37`
   Yun onset cohort while separating extended-stats overhead from player-runtime cost.
3. Keep the small `ix 80 / texture 56` generic residue closed unless fresh trusted captures
   materially diverge from the Loop `132` / `145` family shape or isolate a genuinely narrower
   cohort than the already-rejected Loop `134` admission.
4. Keep Remy-left on compare-dirty residue watch only, and do not broaden caps again unless new
   telemetry shows the remaining full-refresh tail is materially larger or the lane stops staying
   exact/direct.
5. Leave presenter-native work, broader same-source batching beyond pairs, dual-core threading,
   ThinLTO, NEON blend work, and row-dedup relands off the immediate native queue until a new
   measurement pass re-ranks them.

### Primary Sources Consulted For This Addendum

- Clang ThinLTO documentation:
  <https://clang.llvm.org/docs/ThinLTO.html>
- SDL3 migration/audio documentation:
  <https://wiki.libsdl.org/SDL3/README-migration>
- GCC builtins documentation for `__builtin_prefetch`:
  <https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html>
- Arm Cortex-A family overview noting Cortex-A9 partial OoO:
  <https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/high-efficiency-midrange-or-high-performance-cortex-a---what-is-the-difference>
- Arm NEON intrinsics reference for `vtbl1`..`vtbl4` table sizes:
  <https://arm-software.github.io/acle/neon_intrinsics/advsimd.html>
- Intel Cyclone V product brief confirming dual-core Cortex-A9 HPS:
  <https://cdrdv2-public.intel.com/853455/cyclone-v-fpgas-and-socs-product-brief.pdf>
- Arm DS-5 workshop example showing Cortex-A9 PMU counters and "data dependent stall":
  <https://developer.arm.com/-/media/developer/products/software-tools/ds-5-development-studio/resources/DS-5_Workshop-v5-13-d1622-6-12-03-SB-DSTREAM.pdf>
