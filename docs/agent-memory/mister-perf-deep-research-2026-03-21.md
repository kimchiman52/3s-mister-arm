# MiSTer Super-Art Performance Deep Research Agent Memory

## Purpose

- Use this file to re-rank Ralph loops for native super-art activation, especially Yun/Genei first-visible performance on the MiSTer software-frame path.
- Treat it as the durable synthesis layer above the raw perf captures, living findings, and older `mister-perf-opportunities.md` memo.

## When To Load This

- When reranking native MiSTer perf work after loops `145`-`150`.
- When evaluating new renderer ideas for Yun/Genei first-visible activation or super-art activation.
- When deciding whether a candidate is genuinely new or just a renamed retry of a rejected row-walk/pair/threshold idea.
- Skip for nearest-HDMI-only presenter work, wrapper-core work, or Quartus setup.

## Fast Path

- Entry points: `artifacts/mister-port/perf/loop145-yun-shared-shapes-repro-r1.json`, `artifacts/mister-port/perf/loop146-remy-rerank-r2.json`, `artifacts/mister-port/perf/loop148-yun-lookup-signatures-r2.json`.
- Core code: `src/port/sdl/software_frame_non_integer.c`, `src/port/sdl/sdl_game_renderer.c`, `src/port/sdl/fbdev_presenter.c`.
- Existing memory: `docs/agent-memory/mister-perf-opportunities.md`, `artifacts/mister-port/living-findings.md`.
- Minimal verification: confirm the lane is still native/direct with `present.mean_ms ~0.5`, then check whether the hot work is still `fast_non_integer` and whether the `32x32 -> 34/35/36/37` cluster still dominates.

## Research Starting Points

- Source: `artifacts/mister-port/perf/loop145-yun-shared-shapes-repro-r1.json` | Use for: trusted current-tree Yun first-8 shape/family mix and render-vs-present split.
- Source: `artifacts/mister-port/perf/loop146-remy-rerank-r2.json` | Use for: trusted Remy exact/direct rerank and proof that Remy is a separate queue.
- Source: `artifacts/mister-port/perf/loop148-yun-lookup-signatures-r2.json` | Use for: lookup-signature diffusion, shared-shape clustering, and phase-timing splits.
- Source: `docs/agent-memory/mister-perf-opportunities.md` | Use for: older AI research items plus the current March 21 no-retry guidance.
- Source: `artifacts/mister-port/living-findings.md` | Use for: exact loop closeouts and rejected-runtime guardrails.
- Source: [Arm NEON vector rearranging guidance](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/coding-for-neon---part-5-rearranging-vectors) | Use for: why changing data shape earlier is usually stronger than trying to vectorize scattered loads directly.
- Source: [Arm ACLE NEON intrinsics reference](https://arm-software.github.io/acle/neon_intrinsics/advsimd.html) | Use for: confirming `vtbl1`..`vtbl4` are tiny byte-table lookups, not a real gather replacement.
- Source: [GCC builtins docs](https://gcc.gnu.org/onlinedocs/gcc-14.2.0/gcc/Other-Builtins.html) | Use for: `__builtin_prefetch` and alignment-hint semantics.
- Source: [Arm DS-5 Cortex-A9 PMU workshop](https://developer.arm.com/-/media/developer/products/software-tools/ds-5-development-studio/resources/DS-5_Workshop-v5-13-d1622-6-12-03-SB-DSTREAM.pdf) | Use for: PMU counters such as data-dependent stalls and L2 read-hit/request ratio.
- Source: [Arm Cortex-A family overview](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/high-efficiency-midrange-or-high-performance-cortex-a---what-is-the-difference) | Use for: Cortex-A9 partial OoO context when reasoning about dependent-load latency.

## Current Verified State

- Native present is not the first-line bottleneck on the trusted March `2026-03-21` lanes. `loop145` first-8 frames are `20.9510 FPS / 47.7304 ms frame / 35.9909 ms render / 0.5093 ms present`, and `loop146-remy-rerank-r2` stays at `0.5255 ms` present with zero non-integer or generic residue.
- Yun/Genei first-visible activation remains the dominant native failure. The main cost is still the non-integer helper in `src/port/sdl/software_frame_non_integer.c`, routed through the `384`-pixel threshold in `src/port/sdl/sdl_game_renderer.c`.
- Remy-left is a separate exact/direct compare-dirty residue track. Do not expect Yun raster work to move it materially.
- The stale March queue is closed on this tree. Loop `134` rejected the bounded `ix 80 / texture 56` admission, Loop `135` rejected the scalar `4x` row-walk unroll, Loop `142` rejected the pair-density gate, and Loop `149` closed the remaining `ix 80` audit.
- Lookup and pair-setup are not the main remaining runtime cost. Summing `loop148` sampled fast-non-integer families gives about `10.63 ms` lookup-plus-pair work versus about `57.94 ms` row raster; after removing capture-only reuse bookkeeping, row raster is about `84.5%` of the accounted no-telemetry time and lookup-plus-pair is about `15.5%`.
- The hot onset work is clustered by shape, not by one tiny signature. In `loop148`, the top four shared shapes are `32x32 -> 34x34`, `35x35`, `36x36`, and `37x37`, totaling about `30.0 ms` of `78.3 ms` shared-shape sampled time.
- The narrower shapes are still important because some are more expensive per pixel. `16x32 -> 18x35`, `19x37`, and `32x16 -> 34x17` are slower in `ns/pixel` than the big `32x32` cohort even when they contribute less total time.
- The hottest families are overwhelmingly binary-alpha, not blend-heavy. In `loop145`, large families such as `ix 82 / tex 58 / pal 393`, `ix 81 / tex 57 / pal 393`, `ix 81 / tex 57 / pal 394`, and several non-PPG families are `100%` opaque; others such as `ix 81 / tex 57 / pal 391`, `ix 81 / tex 57 / pal 329`, `ix 1102 / tex 18 / pal 37`, and `ix 43 / tex 41 / pal 1` are opaque-plus-transparent with effectively zero blended pixels.
- Same-source horizontal reuse is real but limited. The strong families usually sit around `0.21`-`0.26` reused-pixel ratio with `same_source_max_run_length = 2`, and the pair topology stayed too fragmented for a simple endpoint-aware reland.
- Existing whole-row opaque work was too coarse for the current hotspot. The cached row mask in `src/port/sdl/sdl_game_renderer.c` only proves whether an entire `256`-pixel source row is fully opaque, while the hot onset families mostly sample `16x16`-`32x32` or `32x16` subrects inside `256x256` ARGB atlases. A whole-row miss does not prove the sampled sprite span is mixed-alpha.
- MiSTer-target build flags are already sane. `CMakeLists.txt` uses `-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard`; this queue should not spend time on another generic compiler-flag guess before measurement-backed runtime work.

## Ranked Claims To Verify

### 1. Subrect alpha-structure telemetry is the strongest new measurement lane

- Claim: the rejected whole-row opaque relands were testing the wrong granularity, and the right next measurement is whether the sampled sprite spans inside the hot `256x256` ARGB surfaces are fully opaque, fully transparent, or binary-alpha-only often enough to justify new skip/copy paths.
- Why it looks new: the repo already rejected whole-row opaque shortcuts, transparent-row skip on the stage-7 path, and simple endpoint-aware duplicate relands, but it does not show a trusted non-integer capture that classifies sampled subrect rows/spans inside the hot Yun families.
- Evidence: the dominant onset families mostly use `source_rect_w/h = 16..32 / 16..32` inside `256x256` surfaces, and many of those families are `100%` opaque or opaque-plus-transparent at the pixel level.
- Verification questions:
- How often is the sampled subrect row fully opaque even when the full `256`-pixel source row is not?
- How often are sampled subrect rows or spans fully transparent?
- How often are sampled spans binary-alpha-only with zero blended pixels?

### 2. A subrect alpha sidecar is the strongest genuinely new runtime candidate

- Claim: if claim `1` is validated, cache a compact per-surface alpha sidecar for `256x256` ARGB software surfaces and use it in the non-integer helper to skip transparent source loads entirely and to route fully opaque spans through alpha-free copy loops.
- Why it differs from rejected work: this is not another full-row opaque shortcut and not another pair-topology reland. It changes the metadata granularity from whole `256`-pixel rows to the hot subrect/span level.
- Candidate forms:
- A `1`-bit-per-pixel alpha mask for `256x256` ARGB surfaces, which costs about `8 KiB` per surface and supports fast span scans.
- Per-row span metadata if the hot sampled shapes prove sparse enough to justify a smaller structure.
- Verification questions:
- Can the existing software-surface cache lifetime and invalidation path own the sidecar safely?
- Is the mask/span build cost low enough relative to reuse?
- Does the helper save enough source-pixel loads to matter on the trusted Yun first-8 window?

### 3. Any future specialization must be clustered-shape-first, not signature-first

- Claim: if another runtime reland happens in the native Yun lane, it needs to target the broader clustered cohort around `32x32 -> 34/35/36/37` with adjacent `16x32` and `32x16` shapes, not another tiny exact-signature or lookup-hash idea.
- Evidence: `loop148` top four shared shapes contribute about `30.0 ms` of `78.3 ms`, while the top `32` lookup profiles cover only about `14%` of total fast-non-integer family sampled time.
- Verification questions:
- Is there a low-risk specialization boundary that covers the clustered shapes without exploding branch cost or code size?
- Can a measurement-only row-plan export isolate a safe clustered kernel shape before runtime edits begin?

### 4. PMU-backed measurement should precede more gather-latency folklore

- Claim: the next deep measurement step should try to collect Cortex-A9 PMU evidence for data-dependent stalls and L2 read-hit/request behavior during the first-visible Yun window so future loops can distinguish a true memory-latency wall from other row-walk costs.
- Why it matters: current reasoning about gather-latency dominance is strong, but still largely inferential.
- Verification questions:
- Can the current MiSTer tooling safely run a bounded `perf stat` or equivalent PMU capture on the device?
- Which counters are available on this kernel/image, and can they be correlated with the trusted first-8 scene windows?

### 5. Prefetch is only worth a tiny prototype after PMU evidence

- Claim: `__builtin_prefetch` remains lower priority than claims `1`-`4`, but it is still worth a very small measured experiment if PMU data confirms data-dependent or L2-latency pressure in the hot row-walk.
- Why it stays small: GCC documents prefetch as a hint, and the dependent lookup chain may not give enough lead time to matter.
- Verification questions:
- Does the hot sampled onset work miss L1/L2 enough for prefetch to have plausible headroom?
- Is there a lead distance that helps without adding more overhead than it hides?

## Known Pitfalls

- Do not reopen the rejected `ix 80 / texture 56` threshold/admission lane unchanged.
- Do not reopen the rejected scalar `4x` row-walk unroll unchanged.
- Do not reopen simple endpoint-aware duplicate relands or pair-density gating unchanged.
- Do not treat whole-row opaque-row rejection as proof that subrect/span alpha metadata is useless; the granularity is different.
- Do not prioritize blend-focused NEON work for the current native Yun queue unless fresh telemetry proves blended pixels are materially larger than the current binary-alpha-heavy mix.
- Do not chase presenter-side native work for this problem; native present is already near the floor on the trusted current lanes.

## Update Rules

- Last verified: `2026-03-21`.
- Keep new items tied to trusted loop IDs or primary sources.
- When a claim is disproven, replace it with the narrower surviving statement instead of appending a contradiction.
- If a future agent proves or rejects any ranked claim above, update both this file and `artifacts/mister-port/living-findings.md`.
