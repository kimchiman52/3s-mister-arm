# Track B (UI) — Phases 4, 5, 6 — Combined Execution Plan

**Status:** Draft. Branch `feat/netplay-phase-4-6-track-b` off `mister` at `17ab61e7`.
**Date:** 2026-04-20.
**Scope:** UI-side netplay port for 3s-mister-arm. Three phases, one worktree.
**Paired docs:** `docs/plan-netplay-port.md` (§5 Phase 4-6), `docs/research-3sxtra-netplay-port.md` (§11, §12, §14, §18).
**Fallback doc** per Track B orchestration brief. `/plan` skill was not used — this doc was produced after reading the authoritative plan and research docs (§5.4-§5.6 of the plan, §11/§12/§14/§18 of the research).

---

## 0. Reality check (2026-04-20 worktree snapshot)

The plan-and-loop skill's `/plan` step was bypassed because:

1. The source-of-truth plan (`docs/plan-netplay-port.md`, 1421 lines) already contains a detailed
   §5.4 / §5.5 / §5.6 per-phase breakdown with deliverables, validation, sub-tasks, risks. A `/plan`
   rerun would duplicate that work and risk contradicting the locked decisions in §3 / §15.
2. The authoritative plan verified file:line citations inline. Re-planning without re-verifying
   would weaken that discipline.
3. Trade-off: we lose the three-agent review. Mitigation: per-step `/implement` is still used, which
   runs its own implement→review→fix loop.

This doc is the **sequenced, phase-to-phase execution plan**. It cites the authoritative plan for
rationale; it does not relitigate decisions.

## 1. Prerequisites verified

- Worktree root: `/Users/sb/Developer/3sx-mister/.claude/worktrees/agent-a5e24d84`
- Branch: `feat/netplay-phase-4-6-track-b` based on `mister` HEAD `17ab61e7`.
- Plan docs copied into worktree: `docs/plan-netplay-port.md` (119266 B), `docs/research-3sxtra-netplay-port.md` (82003 B).
- 3sxtra reference tree: `/tmp/3sxtra/` (present). Pi4 recipe at `tools/batocera/rpi4/download-deps_rpi4.sh:194-227` confirmed — `aarch64-linux-gnu-gcc`, FreeType 2.13.3, RmlUi branch `6.2`.
- RmlUi source: `/tmp/rmlui-6.2/` (present). SDL backend at `Backends/RmlUi_Renderer_SDL.cpp:33-47` confirmed — premultiplied blend via `SDL_ComposeCustomBlendMode`, override required.
- MiSTer SSH: password auth via `MISTER_PASSWORD=1`. Lock status `free` at start. `misterctl.sh` present.
- `build-deps.sh`: 375 LOC, `--profile desktop|mister` pattern established. Our cross toolchain is
  `arm-linux-gnueabihf` clang with `--target=arm-linux-gnueabihf` (NOT aarch64 like Pi4).
- `CMakeLists.txt`: 438 LOC. `PORT_MISTER`, `ENABLE_NETPLAY`, `ENABLE_MISTER_ARM_HARDENING`
  options already established. Third-party roots at `third_party/<lib>/build/` convention.

## 2. Phase 4 — RmlUi 6.2 + FreeType 2.13.3 ARM cross-compile

**Source of truth:** `docs/plan-netplay-port.md` §5.4 (lines 320-358); `docs/research-3sxtra-netplay-port.md` §14 (lines 1040-1078).

### 2.1 Deliverables

- `build-deps.sh`: add FreeType 2.13.3 section (mister profile only). Mirror Pi4 recipe structure, swap `aarch64-linux-gnu-gcc` -> our clang-20 cross toolchain (uses env from build-game.sh).
- `build-deps.sh`: add RmlUi 6.2 section (mister profile only). `RMLUI_LUA_BINDINGS=OFF`, `RMLUI_SAMPLES=OFF`, `RMLUI_TESTS=OFF`, `RMLUI_FONT_ENGINE=freetype`, FreeType via `CMAKE_PREFIX_PATH`.
- Disable HarfBuzz/brotli/bz2/PNG/zlib in FreeType build.
- `CMakeLists.txt`: new `ENABLE_RMLUI` option. Default OFF when `PORT_MISTER`; default OFF elsewhere (no desktop RmlUi yet — follow plan's conservative default).
- `CMakeLists.txt`: when `ENABLE_RMLUI` is ON, include `third_party/rmlui/build/include` and link `librmlui.a librmlui_debugger.a libfreetype.a` (link order: core before debugger before freetype, freetype resolves TT_* symbols).

### 2.2 Toolchain detail

The MiSTer build runs `tools/mister/build-game.sh` inside a Docker container with:
- `CFLAGS/CXXFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include"`
- `-DCMAKE_C_COMPILER_TARGET=arm-linux-gnueabihf`

For `build-deps.sh --profile mister` the CMake invocations MUST inherit the same `--target` via explicit `CMAKE_C_COMPILER_TARGET`. This is NOT in the existing mister recipes (which rely on the container environment). **Decision:** the new FreeType/RmlUi recipes will be invoked from inside the same Docker container when targeting mister, so they inherit `CFLAGS/CXXFLAGS`. The recipes just need `-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm -DBUILD_SHARED_LIBS=OFF` and will pick up the toolchain from environment. Match SDL3 recipe behavior.

### 2.3 Sub-tasks (sequenced)

P4-S1. Read Pi4 recipe at `/tmp/3sxtra/tools/batocera/rpi4/download-deps_rpi4.sh:180-280` to understand full FreeType + RmlUi + dependency flow (including libraries like tinyxml2 that RmlUi may want). Note: plan §5.4 lists no transitive deps beyond FreeType, so keep minimal.
P4-S2. Append FreeType recipe to `build-deps.sh` (gated to `$PROFILE = mister`). Use FreeType 2.13.3. Output: `third_party/freetype/build/lib/libfreetype.a`.
P4-S3. Append RmlUi recipe to `build-deps.sh`. Use tag `6.2`. Gate: `$PROFILE = mister`. Pass `CMAKE_PREFIX_PATH=$FREETYPE_BUILD`. Output: `third_party/rmlui/build/lib/librmlui.a`, `librmlui_debugger.a`.
P4-S4. `CMakeLists.txt`: add `ENABLE_RMLUI` option (default OFF). When ON: define `ENABLE_RMLUI`, include RmlUi headers, link libraries.
P4-S5. Verify via Docker cross-build container run: `bash build-deps.sh --profile mister` inside `3s-mister-arm-build`. Expect `.a` archives. Run `readelf -h third_party/freetype/build/lib/libfreetype.a | head` to confirm ARM target (machine = `ARM`).
P4-S6. Commit: `feat(netplay): Phase 4 — RmlUi 6.2 + FreeType 2.13.3 ARM cross-compile recipes`.

### 2.4 Risks & mitigations

- R-4.1 (med): FreeType pulls `-lz` from system. Mitigation: `-DFT_DISABLE_ZLIB=ON` per Pi4 recipe.
- R-4.2 (low): RmlUi auto-detects Lua. Mitigation: `-DRMLUI_LUA_BINDINGS=OFF`.
- R-4.3 (low): RmlUi samples/tests pull GLFW. Mitigation: `-DRMLUI_SAMPLES=OFF -DRMLUI_TESTS=OFF`. (Flag names re-verified in §2.5.)
- R-4.4 (NEW): Docker container doesn't exist yet on this host (first-time mister build). Mitigation: `tools/mister/setup-build-container.sh` is called by `build-game.sh`. For `build-deps.sh` to inherit the toolchain, the user may need to run `docker exec 3s-mister-arm-build bash build-deps.sh --profile mister` or `build-deps.sh` may need Docker wrapper. Check current behavior — our `SDL3` recipe already works for mister profile, so the mechanism exists.

### 2.5 Validation (exit criteria)

- `third_party/freetype/build/lib/libfreetype.a` exists and `readelf -h` reports `Machine: ARM`.
- `third_party/rmlui/build/lib/librmlui.a` exists and `readelf -h` reports `Machine: ARM`.
- `cmake --build` with `ENABLE_RMLUI=ON` completes without errors (no actual RmlUi sources yet, so just linkage test).
- Commit is clean and atomic.

### 2.6 Merge-conflict risk (Track C overlap)

Track C will edit `build-deps.sh` to un-gate GekkoNet+SDL3_net from desktop-only. That edit is at lines 182-246. Our edits append new sections at the end (line 375+). **Expected conflict: none** if we append only. If we modify the profile argparse or the early sections, conflicts are likely.

`CMakeLists.txt`: Track C may also edit the `ENABLE_NETPLAY` block and the third-party root section. We add an `ENABLE_RMLUI` block; interleaving should be clean.

## 3. Phase 5 — SDL blend-mode vendor+patch (first light)

**Source of truth:** `docs/plan-netplay-port.md` §5.5 (lines 360-392); `docs/research-3sxtra-netplay-port.md` §12 (lines 885-987).

### 3.1 Deliverables

- `src/port/sdl/rmlui/vendored/RmlUi_Renderer_SDL.{cpp,h}`: vendored copy of RmlUi 6.2's SDL backend, with a ~2-LOC constructor patch that replaces the `SDL_ComposeCustomBlendMode(...)` call with `blend_mode = SDL_BLENDMODE_BLEND;`. Vendor+patch is required because `BeginFrame()` is non-virtual and `blend_mode` is private in the upstream header — subclassing cannot reach either.
- `src/port/sdl/rmlui/rmlui_blend_fix.cpp` + `.h`: thin `RenderInterfaceSDLMiSTer` subclass over the vendored `RenderInterface_SDL`. Exists to make the intent discoverable at call sites and to centralize construction; adds no state.
- `src/port/sdl/rmlui/rmlui_first_light_test.cpp`: minimal test harness. Sets up SDL window, RmlUi context, loads a tiny inline RML document (text + semi-transparent quad), renders for 3 seconds, exits.
- `src/port/config/cli_parser.c` + `.h`: minimal CLI flag parser. Adds `--test-rmlui-first-light` flag. (NEW file — this path does not exist on mister; the plan's reference is prospective. Implement minimally.)
- `src/main.c`: gate the first-light path on the flag; otherwise normal boot.

### 3.2 Sub-tasks (sequenced)

P5-S1. Read `/tmp/rmlui-6.2/Backends/RmlUi_Renderer_SDL.h` and `.cpp` fully to confirm the class surface. Note: `BeginFrame()` is non-virtual and `blend_mode` is a private field, so override-based approaches do not work — vendor+patch is the only viable path.
P5-S2. Vendor `Backends/RmlUi_Renderer_SDL.{cpp,h}` from RmlUi 6.2 into `src/port/sdl/rmlui/vendored/` and apply the ~2-LOC constructor patch (`blend_mode = SDL_BLENDMODE_BLEND;`). Create `src/port/sdl/rmlui/rmlui_blend_fix.{cpp,h}` as a thin subclass wrapper (expected ~40 LOC).
P5-S3. Create `src/port/sdl/rmlui/rmlui_first_light_test.cpp` (expected ~150 LOC).
P5-S4. Create `src/port/config/cli_parser.{c,h}` with one flag and `cli_parser_is_test_rmlui_first_light()` accessor (expected ~80 LOC). Integrate cleanly with existing `main.c` argv path.
P5-S5. Update `CMakeLists.txt`: when `ENABLE_RMLUI=ON`, include the RmlUi SDL backend source (`/tmp/rmlui-6.2/Backends/RmlUi_Renderer_SDL.cpp` + header) — vendor it by copying to `src/port/sdl/rmlui/vendored/`.
P5-S6. Build via `tools/mister/build-game.sh --flavor telemetry` with `ENABLE_RMLUI=ON`.
P5-S7. Deploy to MiSTer via `tools/mister/misterctl.sh`. Check `lock-status` and `busy-status` first. Follow `docs/mister-runbook.md`.
P5-S8. SSH to MiSTer, run `3s-arm --test-rmlui-first-light`. Visual verification (screenshot capture via `misterctl.sh capture-wrapper` if supported; otherwise describe observed output).
P5-S9. If visual is wrong (missing text, opaque quad, garbled blending): iterate on the blend fix — try `SDL_BLENDMODE_BLEND_PREMULTIPLIED` if SDL3 supports it, or patch the vertex color path in RmlUi backend.
P5-S10. Commit: `feat(netplay): Phase 5 — RmlUi SDL blend-mode subclass + first-light on MiSTer`.

### 3.3 Risks

- R-5.1 (med): SDL3 SW renderer on armhf has quirks beyond blend mode. Only hardware verification catches this. **Contingency:** if first-light is wrong in a way we can't diagnose remotely, commit the code + a note describing the observed behavior, and flag for manual triage.
- R-5.2 (low): ImGui already renders fine on the same SDL_Renderer (research §12.3). Strong prior that RmlUi will work with the blend fix.
- R-5.3 (NEW): MiSTer may be busy with Track A or Track C. Mitigation: `misterctl.sh lock-status` before deploy; abort if occupied.

### 3.4 Validation (exit criteria)

- Binary builds with `ENABLE_RMLUI=ON` on telemetry flavor.
- Deploys to MiSTer without error.
- `--test-rmlui-first-light` runs without crash.
- Visual output shows readable text AND correctly-blended semi-transparent quad (not pitch-black, not opaque).

## 4. Phase 6 — Port menu_network + ms_* + 9 RmlUi screens + assets

**Source of truth:** `docs/plan-netplay-port.md` §5.6 (lines 394-451). High risk (R-6.1 at HIGH).

### 4.1 Scope in — what we port

- `src/sf33rd/Source/Game/menu/menu_network.c` (1764 LOC) + `menu_network.h` + `menu_network_constants.h`.
- Ripple per R-6.1: `menu_draw.c` (132 LOC), `menu_input.c` (2586 LOC), `menu_internal.h`.
- `src/port/screens/ms_casual_lobby.c` (121), `ms_tournament_lobby.c` (102), `ms_ranked_matchmaking.c` (298), `ms_network_lobby.c` (206), `ms_leaderboard.c` (218), `ms_player_profile.c` (137), `ms_replay.c` (267), `ms_save_replay.c` (221), `ms_ranking.c` (69). Total 1639 LOC.
- `src/port/menu_screen_registry.c` (408), `menu_screen_helpers.c` (236), `menu_bridge.c` (289), `menu_task.c` (69). Total 1002 LOC.
- `src/include/port/menu_screen.h` (393 LOC).
- `src/port/sdl/rmlui/rmlui_wrapper.{cpp,h}` + 9 netplay-specific screens (~8000 LOC C++).
- `assets/ui/*.{rml,rcss}` + `BoldPixels.ttf` (~620 KB; exclude NotoSansJP/NotoEmoji).
- Strip/stub 4 non-self-guarded RmlUi headers.

### 4.2 Scope out — what we DO NOT port

- `bracket.c` (Phase 12+).
- `lobby_server.c`, `discovery.c`, `stun.c` (Track C).
- 3sxtra engine refactors (`globals/`, `training/`, `stage/bg_*`).
- NotoSansJP + NotoEmoji fonts.
- Lua plugin.

### 4.3 Sub-tasks (high-level, NOT all-sequential)

P6-S1. Read `menu_network.c` to identify all external engine symbols; match to our fork's engine symbol names. Produce a symbol-mapping table.
P6-S2. Read `menu_draw.c`, `menu_input.c`, `menu_internal.h` — confirm the +3000 LOC ripple claim and check for transitive 3sxtra-only deps.
P6-S3. Copy & adapt `menu_network.c` / `.h` / `_constants.h` to `src/sf33rd/Source/Game/menu/`.
P6-S4. Copy `menu_draw.c`, `menu_input.c`, `menu_internal.h` to same dir; patch symbol references.
P6-S5. Copy `menu_screen_registry.c` + `_helpers.c` + `menu_screen.h` + `menu_bridge.c` + `menu_task.c`.
P6-S6. Copy 9 `ms_*` screen files.
P6-S7. Copy `src/port/sdl/rmlui/` subtree minus non-netplay screens.
P6-S8. Copy `assets/ui/` excluding NotoSansJP/NotoEmoji.
P6-S9. Wire `rmlui_wrapper` lifecycle into `main.c` / `sdl_app.c`.
P6-S10. Replace our `menu.c:1451` 2-item `Netplay_Menu` with call to `Network_Lobby`.
P6-S11. Add `ENABLE_RMLUI` source-list gating in `CMakeLists.txt`: `list(FILTER GAME_SRC EXCLUDE REGEX "src/port/sdl/rmlui/")` when OFF.
P6-S12. Stub-guard the 4 non-self-guarded headers.
P6-S13. Build. Fix missing-symbol cascades one by one.
P6-S14. Deploy to MiSTer. Navigate Title → Online → Casual Lobby to verify UI renders.
P6-S15. Commit: `feat(netplay): Phase 6 — port menu_network.c + ms_* glue + 9 RmlUi screens + assets`.

### 4.4 Risks (HIGH severity)

- R-6.1 HIGH (plan §5.6): ripple from `menu_network.c` into `menu_draw.c`/`menu_input.c`. Budget +3000 LOC.
- R-6.2 MEDIUM: asset paths hardcoded; packaging under `/media/fat/games/3s-arm/assets/ui/` needs fixup.
- R-6.3 MEDIUM: 9 RmlUi `.cpp` transitively include headers we don't have (Lua, OpenGL, 3sxtra-specific). Each is a cascade.
- R-6.4 LOW: `MenuScreenId` enum lists screens we don't port. Keep enum, stub screens.
- R-6.5 NEW HIGH: **Effort exceeds a single session**. Plan estimate is 10-15 focused days (L), potentially 15-20 (XL). If cascade hits, implementation blocks.

### 4.5 Abort criteria (when to stop Phase 6)

- If after 4 compile iterations, undefined-symbol count is still growing (cascade unbounded).
- If `menu_network.c` touches engine state we've deliberately skipped per Decision 2.
- If `rmlui_wrapper` requires RmlUi APIs only available in >6.2 (we pin to 6.2).
- **On abort:** commit work-in-progress under `WIP Track B Phase 6` message, drop `TRACK_B_BLOCKED.md` with blocker summary.

### 4.6 Validation (exit criteria)

- Build with `ENABLE_RMLUI=ON` succeeds via `tools/mister/build-game.sh --flavor telemetry`.
- Binary boots on MiSTer.
- `Netplay_Menu` entry in main menu now dispatches into `Network_Lobby`.
- Gateway screen renders correctly (text readable, layout intact). No requirement to actually connect (that's Track A + Phase 10).

## 5. Execution order

1. Phase 4 — attempt in-session. Commit on success.
2. Phase 5 — attempt in-session. MiSTer device verification required. Commit on success.
3. Phase 6 — attempt in-session. **Most likely abort point.** If abort: commit WIP, write `TRACK_B_BLOCKED.md`.

## 6. Session-level safety

- **No `rsync --delete`** anywhere.
- **No `git push`**.
- Check `misterctl.sh lock-status` + `busy-status` before ANY remote MiSTer command.
- Use `tools/mister/build-game.sh --flavor telemetry` only (AGENTS.md rule).
- Reference `docs/mister-runbook.md` before first deploy.

## 7. Session log (this plan's source of decisions)

- 2026-04-20: Switched worktree `agent-a5e24d84` from upstream base (`83b5fa7b`) to `feat/netplay-phase-4-6-track-b` off `mister` (`17ab61e7`). Reason: original base didn't contain any mister port infrastructure referenced in the plan.
- 2026-04-20: Copied untracked `plan-netplay-port.md` + `research-3sxtra-netplay-port.md` from main working tree into worktree `docs/`.
- 2026-04-20: Decided NOT to use `/plan` skill; authoritative plan §5.4-§5.6 already has the breakdown.
- 2026-04-20: Verified Pi4 recipe uses `aarch64`; our MiSTer toolchain is `arm-linux-gnueabihf` (armv7 hard-float). Recipe adaptation is not line-for-line — `CMAKE_SYSTEM_PROCESSOR` changes to `arm`, compiler inherited from Docker container environment.
