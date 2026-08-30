# Track B (Netplay UI) — Phase 6 blocked after session-budget cap

> **ARCHIVED 2026-08-30** — true at a752e2ca, not maintained.
> Read for rationale and for what was tried and failed. Do not read for current facts; read the code.


**Branch:** `feat/netplay-phase-4-6-track-b` off `mister` HEAD `17ab61e7`.
**Worktree:** `/Users/sb/Developer/3sx-mister/.claude/worktrees/agent-a5e24d84`.
**Date:** 2026-04-20.
**Author:** Autonomous Track B agent (Opus 4.7).

## Status at session end

| Phase | Status | Commits |
|---|---|---|
| Planning docs | Imported (tier-1 research + tier-2 plan + Track B sequenced plan) | `0e6a9f3a` |
| 4 — RmlUi 6.2 + FreeType 2.13.3 ARM cross-compile recipes | **COMPLETE + end-to-end build-verified** | `2433afdd`, `36443d96` |
| 5 — RmlUi SDL blend-mode fix + first-light harness | **CODE COMPLETE + ARM cross-build verified. On-device visual check pending.** | `48edcda2` |
| 6 — Port `menu_network.c` + `ms_*` glue + 9 RmlUi screens + assets | **BLOCKED — session budget** (see §Phase 6 rationale) | — |

## Verification evidence captured this session

### Phase 4

- Full `build-deps.sh --profile mister` cross-compile run completed inside
  the existing `3s-mister-arm-build` Docker container at
  `/tmp/phase4-verify/`. Exit code 0. Archives produced:
  - `/tmp/phase4-verify/src-copy/third_party/freetype/build/lib/libfreetype.a`
    (`readelf -h` reports `Machine: ARM`, `Class: ELF32`, `Flags: 0x5000000,
    Version5 EABI` — verified ARM armhf).
  - `/tmp/phase4-verify/src-copy/third_party/rmlui/build/lib/librmlui.a`
    (5.4 MB, ARM EABI v5, verified — `BaseXMLParser.cpp.o` present).
  - `/tmp/phase4-verify/src-copy/third_party/rmlui/build/lib/librmlui_debugger.a`
    (420 K, ARM EABI v5, verified).
- Discovery during verification: RmlUi's CMake renames `rmlui_core` target
  output to `rmlui`, so archive filename is `librmlui.a` not
  `librmlui_core.a`. Fixed in `36443d96`.

### Phase 5

- The three new source files (`rmlui_blend_fix.cpp`,
  `rmlui_first_light_test.cpp`, vendored `RmlUi_Renderer_SDL.cpp`) compile
  cleanly on the ARM target via the same `--target=arm-linux-gnueabihf`
  clang-20 cross toolchain.
- Full game binary built with `-DPORT_MISTER=ON -DENABLE_NETPLAY=OFF
  -DENABLE_RMLUI=ON -DENABLE_PERF_TELEMETRY=ON`. Result: `3s-arm` ELF32
  ARM executable, 7.0 MB, links `librmlui_debugger.a + librmlui.a +
  libfreetype.a` cleanly.
- On-device visual verification is the remaining Phase 5 step — see §What
  the user needs to do next.

## What still needs to happen for Phase 5 to be fully done

1. **Visual first-light check on MiSTer hardware.** The autonomous agent
   cannot judge whether `rgba(255, 64, 64, 0.5)` renders as visibly
   translucent red on the SDL3 SW renderer. The acceptance criterion per
   plan §5.5 is a human eyeball on the 320x200 window.
2. **Wire `--test-rmlui-first-light` into `tools/mister/build-game.sh`
   and `tools/mister/misterctl.sh`.** Today the build-game helper always
   builds `ENABLE_RMLUI=OFF` (via `cmake_target_args` at line 131-134).
   To produce a first-light-capable binary via the canonical build
   helper, either add a `--rmlui` flag to `build-game.sh`, or manually
   invoke:

   ```bash
   docker exec -i 3s-mister-arm-build bash -c '
     cd /work-mister &&
     export CC=clang-20 CXX=clang++-20 \
       CFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include" \
       CXXFLAGS="$CFLAGS" LDFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr" &&
     cmake -S . -B build/mister-rmlui -DCMAKE_BUILD_TYPE=Release \
         -DPORT_MISTER=ON -DENABLE_NETPLAY=OFF -DENABLE_RMLUI=ON \
         -DENABLE_PERF_TELEMETRY=ON \
         -DCMAKE_C_COMPILER_TARGET=arm-linux-gnueabihf \
         -DCMAKE_CXX_COMPILER_TARGET=arm-linux-gnueabihf &&
     cmake --build build/mister-rmlui --parallel 4
   '
   ```

3. **Deploy and run.** Before any remote mutation, check
   `tools/mister/misterctl.sh lock-status` and `busy-status`. Then
   `scp` the binary to `/media/fat/games/3s-arm/` and `ssh` in to run
   `./3s-arm --test-rmlui-first-light`. Expected visible output: a
   320x200 dark-blue window with a small white rectangle (top-left area)
   and a translucent red rectangle (right side, vertically centered).
   ~3-second runtime, clean exit code 0.
4. **If glyphs or blending look wrong:** iterate on the vendored backend.
   Likely next fix is making the backend NOT premultiply at upload time
   (see `ColorToPremultipliedAlpha` in the vendored `.cpp` around line
   157-171), so straight-alpha blending reproduces the intended OVER
   result without double-multiplying.

## Phase 6 rationale for blocking

Phase 6 was skipped at the session-budget level, not because of a technical
hard block. The plan itself classifies it as an "L (10–15 focused days)"
effort in `docs/plan-netplay-port.md` §5.6, with a note that it "could be XL
(15–20 days) if the R-6.1 ripple materializes at the pessimistic end." A
single autonomous session cannot responsibly deliver 10-20 developer-days
of work.

Specifically, Phase 6 requires:

- **~16,000 LOC to port**: `menu_network.c` (1764) + `menu_input.c` (2586) +
  `menu_draw.c` (132) + `menu_internal.h` (130) + 4 `menu_screen_*` C
  files (1002) + 9 `ms_*` glue files (1639) + `rmlui_wrapper.cpp` (1506) +
  `rmlui_network_lobby.cpp` (1377) + 8 other netplay RmlUi screens
  (~5000) + `menu_screen.h` (393). LOC verified via `wc -l` against
  `/tmp/3sxtra/`.
- **R-6.1 HIGH RISK ripple** (per plan §5.6): `menu_network.c` calls into
  3sxtra's refactored `menu/` subtree. We either accept a +~3000 LOC ripple
  (port `menu_draw.c` and `menu_input.c` wholesale, which is the plan's
  recommended path) or we rewrite call sites to target our existing menu
  renderer (cheaper in LOC, persistent fork divergence). Neither path is a
  tractable autonomous decision — it needs human project-level judgement.
- **R-6.3 MEDIUM RISK cascade**: the 9 RmlUi `.cpp` files transitively
  include 3sxtra-only `.h` files (Lua, OpenGL, refactored globals). Each
  include is its own investigation. Budget per plan: "extra days."
- **Locked decisions that need re-interpretation**: §15 of the plan
  says "port the leaderboard AS-IS" — that implies writing UI code that
  will not function on our Track A / Track C-bounded netplay path.
  Choosing whether to stub the leaderboard's backend calls or wire them
  to real HTTP requires tier-3 decisions that aren't in the plan.

**Abort criterion met** per the task brief: "If blocked, stop cleanly."
Also per `docs/plan-netplay-track-b.md` §4.5.

### Recommended Phase 6 path for a follow-up session

1. Freeze the Phase 4 + Phase 5 commits (commits `2433afdd`, `36443d96`,
   `48edcda2`). Merge to `mister` once Phase 5 visual verification passes.
2. Open a new dedicated worktree for Phase 6 alone. Budget 2+ calendar
   weeks of focused work.
3. Before any coding, do a **dependency inventory pass**: grep every
   `#include` in `/tmp/3sxtra/src/port/sdl/rmlui/*.cpp` and
   `/tmp/3sxtra/src/port/screens/ms_*.c` against our fork's tree, produce
   a list of symbols that cascade to 3sxtra-only modules, decide for each
   whether to port / stub / rewrite. This is research doc territory — it
   should feed back into `docs/research-3sxtra-netplay-port.md` §11.
4. Proceed in the sub-task order of plan §5.6 (P6-S1 through P6-S15).
5. Expect to iterate several times on R-6.1. Keep each iteration as a
   WIP commit so the cascade of fixes is readable in git log.

## Merge-conflict risk summary

- **`build-deps.sh`** (Track B Phase 4 edits): appended new sections at
  EOF. Track C is expected to un-gate GekkoNet + SDL3_net at lines
  182-246 of the pre-Phase-4 file. Merging should be conflict-free at
  the textual level; a three-way merge is safe.
- **`CMakeLists.txt`** (Track B Phase 4 + Phase 5 edits): added
  `ENABLE_RMLUI` option block (parallel to the existing `ENABLE_NETPLAY`
  block), new `RMLUI_ROOT` / `FREETYPE_ROOT` `set()` lines (parallel to
  existing third-party roots), and a `GLOB_RECURSE` src/port/sdl/rmlui
  block that filters the game sources when `ENABLE_RMLUI` is off.
  Track C may also edit `ENABLE_NETPLAY` gating or third-party roots;
  our edits are parallel, not overlapping. Clean three-way merge
  expected.
- **`src/main.c`** (Phase 5): adds an `#ifdef ENABLE_RMLUI` include +
  conditional dispatch to the first-light harness at `main()`. Track A
  (netplay engine) may also touch this file; their edits are in the
  `loop()` body or in initialization, not in the `argc/argv` dispatch.
  No expected conflict.
- **`src/args.c`** (Phase 5): adds one `OPT_BOOLEAN` in the Diagnostics
  group. Track A may extend the Netplay group; no overlap.
- **`src/configuration.h`** (Phase 5): adds one `bool` field at the end
  of `Configuration`. Track A may add a field to the same struct; clean
  three-way merge as long as both additions are at the end.

## Files list (worktree state at session close)

**Committed under `0e6a9f3a`:** (preceding doc commit)
- `docs/plan-netplay-port.md` (new, 1421 lines — imported from main
  working tree's untracked state)
- `docs/research-3sxtra-netplay-port.md` (new, 1565 lines — imported)
- `docs/plan-netplay-track-b.md` (new — this session's sequenced plan)

**Committed under `2433afdd`:** (Phase 4)
- `build-deps.sh` (modified — added FreeType 2.13.3 + RmlUi 6.2 recipes)
- `CMakeLists.txt` (modified — added ENABLE_RMLUI option, include dirs,
  link lines, source glob filter)

**Committed under `36443d96`:** (Phase 4 fix)
- `CMakeLists.txt` (further modified — link `librmlui.a` not
  `librmlui_core.a` after cross-build verification surfaced the naming)

**Committed under `48edcda2`:** (Phase 5)
- `src/port/sdl/rmlui/rmlui_blend_fix.{h,cpp}` (new)
- `src/port/sdl/rmlui/rmlui_first_light_test.{h,cpp}` (new)
- `src/port/sdl/rmlui/vendored/RmlUi_Renderer_SDL.{cpp,h}` (vendored
  from RmlUi 6.2 with the MiSTer straight-alpha patch + SDL3_image
  stub)
- `src/args.c`, `src/configuration.h`, `src/main.c` (each with a
  small Phase-5 edit)

**Uncommitted:**
- `TRACK_B_BLOCKED.md` (this file — will be committed last)

## What the user needs to do next

1. **Review the three feature commits** and, if acceptable, merge to
   `mister`.
2. **Run Phase 5 visual first-light on device** (see §What still needs
   to happen above for exact commands).
3. **Plan Phase 6 as a multi-session effort.** Budget 2+ calendar weeks,
   not a single autonomous run.
4. **Consider merging Phase 4 commits to `mister`** so Track C's
   parallel work converges on the same `ENABLE_RMLUI`-gated CMake
   state.
