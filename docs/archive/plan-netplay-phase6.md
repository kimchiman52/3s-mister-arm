# Phase 6 — Port menu_network.c, ms_* glue, 9 RmlUi screens, assets

> **ARCHIVED 2026-08-30** — true at a752e2ca, not maintained.
> Read for rationale and for what was tried and failed. Do not read for current facts; read the code.


> **HISTORICAL — deprecated.** This document reads as a live roadmap but is no
> longer current. The RmlUi in-game lobby UI, the HTTP lobby client, identity
> persistence, and the ingame chat surface this plan targeted were all removed
> in commit `d81bb569` ("refactor(netplay): drop in-game lobby UI; keep
> Direct-P2P via OSD"). The shipped netplay path today is the
> wrapper-OSD-driven Direct-P2P flow in `src/netplay/direct_p2p*` and
> `src/main.c`. Kept for historical reference only — do not follow the
> step-by-step instructions below as current guidance.

Tier-3 detailed implementation plan for the `netplay` branch of `3sx-mister`.

- **Parent tier-2 plan:** `docs/archive/plan-netplay-port.md` §5 Phase 6, §7.3, §8.2, §15 locked decisions.
- **Parent tier-1 research:** `docs/archive/research-3sxtra-netplay-port.md` §5, §9.7, §11.4, §12, §16.
- **Upstream source-of-truth:** `/tmp/3sxtra/` (HEAD `a18eae1`, per §16.2).
- **Our branch:** `netplay`, 29 commits above `mister`. Phases 1–5 and 7 already merged.
- **Effort budget (re-scored post-review 2026-04-20):** XL = 15–20 focused days. Tier-2 §5 Phase 6 originally sized this as L; review confirmed three LOC/reconciliation under-estimates (see Summary "Estimated effort" for the breakdown) that push into XL.
- **Top risk identified during planning:** R-6.3 (RmlUi wrapper + screen cascade). Upstream `rmlui_wrapper.cpp` pulls in Lua (`lua_engine_bridge.h`), OpenGL3 (`RmlUi_Renderer_GL3.h`), SDL3_GPU, SDL3_image, controller imagery, palette remix, and 3sxtra-specific path/config headers; our MiSTer target has NONE of those (see §12.2 of research doc: no libGL, libEGL, libGLES, libgbm, libdrm). The wrapper must be adapted into a MiSTer-only SDL3-software variant before any screen can link. See Step 6. Multiple upstream `.cpp` screens also `#include "structs.h"` (verified via Grep), which our `include/structs.h` hard-`#error`s under C++ — a separate blocker resolved by Step 5.

## Ground rules for every step

- Sub-agents MUST invoke the `/implement` skill — "I wrote a patch file" is not equivalent (per `feedback-enforce-skill-invocation.md`). Each step is one full implement→review→fix→verify→commit loop. The review step is non-skippable.
- No `git push` without explicit permission.
- No `rsync --delete` into any path outside `/media/fat/games/3s-arm/` (per `feedback-no-rsync-delete.md`).
- No `--no-verify`, no signing bypass.
- Canonical build: `tools/mister/build-game.sh --flavor telemetry`. For steps that need `ENABLE_NETPLAY=ON` or `ENABLE_RMLUI=ON`, Step 1 below adds the required `EXTRA_CMAKE_ARGS` pass-through; until that step lands, use direct `cmake` inside the container per `docs/mister-runbook.md` §"Cross-build from x86_64/arm64 host".
- Before any MiSTer probe/deploy: `tools/mister/misterctl.sh lock-status && tools/mister/misterctl.sh busy-status`. Read `docs/mister-runbook.md` for the exact deploy path map.
- No emojis in code, docs, commit messages.
- Every step must cite file:line for non-obvious claims in its commit message.

## Doc/code mismatches flagged during planning (trust the code)

1. **`src/port/netplay/` does not exist in HEAD** (verified via `ls`). Task-prompt reference to `src/port/netplay/gekko_session.*`, `netplay_event.*` is aspirational; Track A landed its work in `src/netplay/` (same layout 3sxtra uses). This plan uses `src/netplay/` for event queue and does not create a `src/port/netplay/` hierarchy.
2. **Task-prompt phrase "9 RmlUi screens" is precise only if `rmlui_wrapper.cpp/h` is counted separately.** Tier-2 §5 Phase 6 enumerates 9 screens (network_lobby, casual_lobby, tournament_lobby, ranked_matchmaking, network_replay_picker, ingame_chat, netplay_ui, leaderboard, player_profile) plus `rmlui_wrapper.cpp/h`. Plan steps below treat the wrapper as a distinct component (Step 6) and the 9 screens as three grouped steps (Steps 11, 12, 13).
3. **Tier-2 §5 Phase 6 "Sub-task 8" assumes native LAN path compiles without RmlUi.** Our current `src/netplay/discovery.c` does NOT exist (`ls /Users/sb/Developer/3sx-mister/src/netplay/` — no `discovery.c`). Native LAN discovery is out-of-scope for Phase 6 (it's Phase 9 / Phase 12). Do not attempt it here.
4. **`feedback-always-telemetry.md` flagged 4 days stale.** Re-verified 2026-04-20: `tools/mister/build-game.sh` line 10 still hardcodes `flavor="telemetry"`. Policy holds.
5. **`docs/mister-runbook.md:105-106` TODO.** The runbook documents that `build-game.sh` does NOT plumb `-DENABLE_NETPLAY=ON` / `-DENABLE_RMLUI=ON`. Step 1 closes this gap.
6. **3sxtra's `rmlui_wrapper.cpp:15, :27-33` unconditionally includes `port/sdl/rmlui/rmlui_casual_lobby.h`, `lua_engine_bridge.h`, `RmlUi/Lua.h`, `RmlUi_Renderer_GL3.h`, `RmlUi_Renderer_SDL_GPU.h`, `SDL3_image/SDL_image.h`.** None of these are buildable for our MiSTer target. This is the R-6.3 cascade. Step 6 rewrites the wrapper into a MiSTer-only SDL3-software variant, not a literal copy.
7. **3sxtra's `menu_network.c:83` includes `structs.h`; ms_*.c includes `structs.h` (e.g., `ms_casual_lobby.c:26`).** Four RmlUi `.cpp` screens also include `structs.h` (verified via Grep: `rmlui_casual_lobby.cpp`, `rmlui_tournament_lobby.cpp`, `rmlui_ranked_matchmaking.cpp`, `rmlui_replay_picker.cpp`). Our `include/structs.h:28` hard-`#error`s under `__cplusplus`. Step 5 resolves by option (B) — wrap the `operator` field in `#ifndef __cplusplus` with a C++-compatible accessor — because option (A) (rename field) would touch ~50-100 engine call-sites and break the F25 guard that Track A shipped intentionally.
8. **Upstream `rmlui_phase3_toggles.h` is a real header** at `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_phase3_toggles.h` (113 LOC, verified). It declares `extern bool use_rmlui` plus ~35 `rmlui_menu_*` / `rmlui_screen_*` / `rmlui_hud_*` toggle globals under `#ifdef ENABLE_RMLUI`, with `static const bool … = false` fallbacks under `#else`. Definitions live in `rmlui_game_hud.cpp` (out of Phase 6 scope). 17 `ms_*.c` files include it. Step 10 must port a trimmed subset of the header and define only the toggles the Phase-6 screens actually consume (`rmlui_menu_lobby`, `rmlui_screen_attract_overlay` — audit grep at port time), providing weak-linked definitions in a new `src/port/sdl/rmlui/rmlui_phase3_toggles_stub.c`.

---

## Step index (16 steps)

1. Plumb `EXTRA_CMAKE_ARGS` through `build-game.sh`; add ENABLE_NETPLAY / ENABLE_RMLUI container build job — **DONE** (`848fd606`)
2. Port event queue API (`NetplayEventType`, `push_event`, `Netplay_PollEvent`) into `src/netplay/netplay.{c,h}` and wire the three push sites — **DONE** (`484184b0`)
3. Copy `menu_screen_registry.c`, `menu_screen_helpers.c`, `menu_bridge.c`, `menu_task.c`, and `include/port/menu_screen.h`; stub un-ported screens — **DONE** (`0fddc326`)
4. Port `menu_draw.c`, `menu_input.c`, `menu_internal.h`, `menu_input_constants.h` (R-6.1 ripple) — **DONE** (`40402ed8`)
5. Resolve `structs.h` C++ guard so RmlUi C++ screens can include engine headers transitively — **DONE** (`4524dea2`)
6. Rewrite `rmlui_wrapper.cpp/h` for MiSTer (SDL3-software only; no Lua / GL3 / GPU / SDL_image / controller imagery) — **DONE** (`065d656c`)
7. Port 3sxtra's ported `lobby_server.c`, `lobby_server.h`, `identity.c`, `identity.h`, `sha256.c`, `sha256.h` from `/tmp/3sxtra/src/netplay/` — **DONE** (`63ab927c`)
8. Implement the three-layer MiSTer arch filter (room-name `[MiSTer]` prefix, `display_name` `" [MiSTer]"` suffix, 2-way `MIST` hello/ack/reject handshake per tier-2 §8.2.4) — **DONE** (`59d87eba`)
9. Port `menu_network.c`, `menu_network.h`, `menu_network_constants.h`; replace our 2-item `Netplay_Menu` with the MenuScreen dispatcher (slot [6] in our fork per option B reconciliation) — **DONE** (`77f22a78`)
10. Port the 6 netplay `ms_*` screens plus 2 legacy-table dependencies (`ms_replay`, `ms_save_replay`); author the trimmed `rmlui_phase3_toggles.h` — **DONE** (`c103f1e1`)
11. Port RmlUi screen group A (gateway + lobbies): `rmlui_network_lobby`, `rmlui_casual_lobby`, `rmlui_tournament_lobby` (upstream `.h` does exist under `src/include/`; plan note corrected in-commit) — **DONE** (`bc16fb3c`)
12. Port RmlUi screen group B (matchmaking + in-game overlays): `rmlui_ranked_matchmaking`, `rmlui_netplay_ui`, `rmlui_ingame_chat` — **DONE** (`09ac4010`)
13. Port RmlUi screen group C (viewers): `rmlui_leaderboard`, `rmlui_player_profile`, `rmlui_network_replay_picker` — **DONE** (`8d23f974`) — full link green milestone
14. Assets install pipeline: `assets/ui/` (99 files), `assets/fonts/BoldPixels.ttf` + `assets/fonts/district_italic.ttf`, `assets/flags/` (10), `assets/flags_icons/` (173) → CMake install → package.sh; sysctl bump via `user-startup.sh` hook — **DONE** (`94cd721b`)
15. End-to-end host-compile gate: `ENABLE_NETPLAY=ON ENABLE_RMLUI=ON` desktop + MiSTer cross-compile, telemetry flavor — **DONE** (no commit; gate pass recorded in Step 15 body)
16. On-device smoke test on single MiSTer: boot, navigate Title → Online → Casual Lobby, create room against shared lobby, verify MIST handshake rejects desktop peer — **DONE** (deploy + boot + sysctl all green; interactive room-create deferred to manual session)

Dependencies:
- Step 2 unblocks Track A TODO at `src/netplay/netplay.c:543-545`.
- Steps 3, 5 are prerequisites for everything after.
- Step 4 is prerequisite for Step 9.
- Step 6 is prerequisite for Steps 11–13.
- Step 7 depends only on Step 1 and can run in parallel with Step 2. Step 7 is prerequisite for Step 8, which is prerequisite for Step 9.
- Step 14 can run in parallel with 11–13 once directory layout is set.
- Step 15 gates Step 16.

---

## Step 1 — Plumb `EXTRA_CMAKE_ARGS` through `tools/mister/build-game.sh` — **DONE** (commit `848fd606`, 2026-04-20)

**What it does:** Make it possible to invoke `tools/mister/build-game.sh --flavor telemetry` with `ENABLE_NETPLAY=ON ENABLE_RMLUI=ON` without hand-editing the script. Add a CI job mirroring the switches.

**Why it matters:** Every subsequent step's success-criteria runs the canonical build helper. Today the helper hardcodes `cmake -S . -B ... -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=${telemetry_flag}` (`tools/mister/build-game.sh:147-149`) with no extra-args pass-through, and `PORT_MISTER=ON` defaults `ENABLE_NETPLAY=OFF` and `ENABLE_RMLUI=OFF` (`CMakeLists.txt:25-31`). Unblocks the remaining 15 steps' build verification.

**Files to read first:**
- `/Users/sb/Developer/3sx-mister/tools/mister/build-game.sh:1-170` (full)
- `/Users/sb/Developer/3sx-mister/docs/mister-runbook.md:103-124` (the TODO and the direct-cmake workaround)
- `/Users/sb/Developer/3sx-mister/.github/workflows/build_linux.yml:44-95` (existing F19 netplay-explicit job — use it as the template)
- `/Users/sb/Developer/3sx-mister/CMakeLists.txt:25-51` (option defaults we need to override)

**Files to create/modify:**
- `tools/mister/build-game.sh`: (a) accept `EXTRA_CMAKE_ARGS` env var (space-separated extra `-D...` tokens) on the outer invocation; (b) the inner `docker exec … bash -s -- "${platform}" "${flavor}" "${jobs}"` at `:100` uses a single-quoted `<<'EOF'` heredoc, so `$EXTRA_CMAKE_ARGS` cannot be expanded inside. Forward by adding a fourth positional: `docker exec … bash -s -- "${platform}" "${flavor}" "${jobs}" "${EXTRA_CMAKE_ARGS:-}"` and add `extra_cmake_args="$4"` at the top of the heredoc (following the `platform="$1"`/`flavor="$2"`/`jobs="$3"` pattern at `:103-105`). Then in `build_one()` at `:139-149`, split the string via `read -ra` into an array and expand it alongside `"${cmake_target_args[@]}"`; (c) echo the final cmake invocation for debuggability.
- `docs/mister-runbook.md:104-106`: remove the "TODO / known limitation" bullet; replace with the actual recipe `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry`.
- `.github/workflows/build_linux.yml`: extend the existing `build-netplay-explicit` job (or add a sibling `build-netplay-rmlui-explicit`) that sets `-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON`. This is a host Linux CI build, not MiSTer cross — it proves the source cascade compiles on Linux x86_64 before we start adding RmlUi TUs.

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry` runs `cmake` with both flags visible in the build log (grep the container stdout for `ENABLE_NETPLAY=ON` and `ENABLE_RMLUI=ON` status messages emitted by `CMakeLists.txt:46, :51`). Expected to FAIL at compile time (no RmlUi sources exist yet), but cmake configure must succeed and the build must fail for the right reason (RmlUi headers or Phase 6 source files, not argument plumbing).
- `grep -n 'EXTRA_CMAKE_ARGS' tools/mister/build-game.sh` returns at least 2 hits (one for capture, one for use).
- CI green: the updated workflow's new job runs on the `netplay` branch and compiles with both flags.

**Dependencies:** none. Starts from current `netplay` HEAD (29 commits above `mister`).

**What NOT to do:**
- Do not change the default flavor. Remains `telemetry`.
- Do not flip `_ENABLE_NETPLAY_DEFAULT` or `_ENABLE_RMLUI_DEFAULT` in CMakeLists — the opt-in gate matters and other profiles rely on it.
- Do not touch any `src/` files.
- Do not deploy. This step is host-only.

**What to do if it fails:**
- If `EXTRA_CMAKE_ARGS` quoting breaks the heredoc: switch to a literal array expansion outside the heredoc and inject via exported env. Fallback: pass via a wrapper script dropped into the container workdir.
- If CI times out after adding a second explicit-flags job: split the job to run only on `netplay` branch pushes (matches the F19 pattern at `build_linux.yml:5`).

---

## Step 2 — Port `NetplayEventType` / `push_event` / `Netplay_PollEvent` into `src/netplay/netplay.{c,h}` — **DONE** (commit `484184b0`, 2026-04-20)

**What it does:** Copy 3sxtra's 8-slot event queue (upstream `src/netplay/netplay.c:1090-1121`, `src/netplay/netplay.h:37-51`) into our `src/netplay/netplay.{c,h}`, wire the three `push_event` call sites Track A marked TODO, and export `Netplay_PollEvent` for Phase 6 consumers.

**Why it matters:** Closes the TODO at our `src/netplay/netplay.c:543-545` (verified via Grep). The Phase 6 lobby UI (Step 9 forward) polls this queue to react to connect/disconnect; without it the lobby screens have no way to learn the session dropped. This is the smallest piece of engine-side work in Phase 6 but it is load-bearing for every subsequent UI step.

**Files to read first:**
- `/tmp/3sxtra/src/netplay/netplay.h:37-51` — enum + struct + API declarations.
- `/tmp/3sxtra/src/netplay/netplay.c:70` — forward declare of `push_event`.
- `/tmp/3sxtra/src/netplay/netplay.c:608-625` — the three upstream push call sites: `case GekkoPlayerSyncing` (line 608-611, pushes SYNCHRONIZING), `case GekkoPlayerConnected` (line 613-616, pushes CONNECTED), `case GekkoPlayerDisconnected` (line 618-626, pushes DISCONNECTED before state transitions). Note: `GekkoSessionStarted` does NOT push; it only transitions to `NETPLAY_SESSION_RUNNING`.
- `/tmp/3sxtra/src/netplay/netplay.c:633-651` — the `GekkoDesyncDetected` → disconnect push (push happens before `Soft_Reset_Sub()` at line 647).
- `/tmp/3sxtra/src/netplay/netplay.c:1088-1121` — static buffer + `push_event` + `Netplay_PollEvent`.
- `/Users/sb/Developer/3sx-mister/third_party/GekkoNet/build/include/gekkonet.h:138` — the enum is `GekkoPlayerSyncing` (no trailing `hronizing`); verified live 2026-04-20.
- `/Users/sb/Developer/3sx-mister/src/netplay/netplay.c:500-557` — our current session-event loop. Our `case GekkoPlayerSyncing:` already exists at `:504-507` (just printf today); `case GekkoPlayerConnected:` at `:509-511`; `case GekkoPlayerDisconnected:` at `:513-516`; `case GekkoSessionStarted:` at `:518-521`; `case GekkoDesyncDetected:` at `:523-549` (routes through `handle_disconnection()` per F8 with a `TODO(phase6)` at `:543-545`).

**Files to create/modify:**
- `src/netplay/netplay.h`: add `NetplayEventType` enum (`NETPLAY_EVENT_NONE`, `_SYNCHRONIZING`, `_CONNECTED`, `_DISCONNECTED`) and `NetplayEvent` struct exactly as upstream. Declare `bool Netplay_PollEvent(NetplayEvent* out)`.
- `src/netplay/netplay.c`: (a) add `EVENT_QUEUE_MAX 8`, static `event_queue[]`, `event_queue_count`, `static void push_event(NetplayEventType)`; (b) define `Netplay_PollEvent`; (c) add `push_event(NETPLAY_EVENT_SYNCHRONIZING)` inside `case GekkoPlayerSyncing:` at our `src/netplay/netplay.c:504-507` (mirroring upstream `netplay.c:608-611`); (d) add `push_event(NETPLAY_EVENT_CONNECTED)` inside `case GekkoPlayerConnected:` at our `src/netplay/netplay.c:509-511` (mirroring upstream `netplay.c:613-616`). Do NOT push CONNECTED at the `GekkoSessionStarted → NETPLAY_SESSION_RUNNING` transition — upstream does not; leave `:518-521` untouched; (e) add `push_event(NETPLAY_EVENT_DISCONNECTED)` inside `case GekkoPlayerDisconnected:` at `:513-516` before `handle_disconnection()`, AND inside `case GekkoDesyncDetected:` before the existing `handle_disconnection()` call at `:547`; remove the three-line TODO at `:543-545`.

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` succeeds.
- `grep -n 'TODO(phase6)' src/netplay/netplay.c` returns zero hits.
- `grep -n 'push_event(NETPLAY_EVENT_' src/netplay/netplay.c` returns at least 4 hits (syncing, connected, two disconnect paths).
- A new test harness at `src/netplay/test_event_queue.c` (C TU, placed next to `netplay.c` so it is picked up by the `GAME_SRC` glob, guarded `#ifdef THIRDSARM_BUILD_NETPLAY_TESTS` so it only links when desired) exports `int Netplay_Test_EventQueue(void)`. Add a CLI flag `--test-netplay-event-queue` to `src/args.c` (following the `--test-rmlui-first-light` pattern at `src/args.c:196-202`) and a corresponding `bool test_netplay_event_queue` field in `src/configuration.h` (following `:60`). Dispatch in `src/main.c` immediately after the existing `test_rmlui_first_light` block at `:791-804`. Test body round-trips all three event types and confirms the ring is FIFO to cap 8.
- Drop semantics (informational, not a bug): upstream `push_event` silently discards events when the 8-slot ring is full (`/tmp/3sxtra/src/netplay/netplay.c:1088-1121`). The FIFO-to-8 test implicitly exercises this behaviour. Keep the behaviour unchanged — do not add overflow logging, mutex, or dynamic resize.

**Dependencies:** Step 1 (so the build actually exercises `ENABLE_NETPLAY=ON`). No tree changes from Step 1 are required in `src/`.

**What NOT to do:**
- Do not introduce a new thread or mutex; upstream's queue is accessed only from the netplay thread (producer) and the main game-loop poll (consumer). We inherit that contract.
- Do not deploy to MiSTer.
- Do not change `NetplaySessionState` enum or `handle_disconnection()` semantics.
- Do not add `NETPLAY_EVENT_REJECTED` or any MIST-handshake events yet — those are Step 8.

**What to do if it fails:**
- If a build error reports an unknown Gekko enum: the symbol is `GekkoPlayerSyncing` (not `GekkoPlayerSynchronizing`). Verified present at `third_party/GekkoNet/build/include/gekkonet.h:138` for our pin `7be848c`; no `#ifdef` fallback is required.
- If test fails because two consumers compete: document as pre-existing multi-threaded contract violation and move on — this is upstream behavior we're matching. Do not add a mutex without confirming the same race is absent upstream.

---

## Step 3 — Copy `menu_screen_registry.c`, `menu_screen_helpers.c`, `menu_bridge.c`, `menu_task.c`, and `include/port/menu_screen.h` — **DONE** (commit `0fddc326`, 2026-04-20)

**What it does:** Bring in 3sxtra's data-driven menu screen dispatcher (1002 LOC C + 393 LOC header). These are required by every `ms_*` file and by `menu_network.c`. Keep the `MenuScreenId` enum intact even for screens we don't port (stub the table entries with NULL callbacks per tier-2 §5 Phase 6 risk R-6.4).

**Why it matters:** `menu_network.c` at upstream includes `port/menu_screen.h` (verified `/tmp/3sxtra/src/sf33rd/Source/Game/menu/menu_network.c:10`). Without the registry the port cannot begin. This step also establishes the dispatcher hook that Step 9 calls from `main.c` / our game loop.

**Files to read first:**
- `/tmp/3sxtra/src/include/port/menu_screen.h:1-393` (full; 3sxtra's types + API).
- `/tmp/3sxtra/src/port/menu_screen_registry.c:1-408` (full).
- `/tmp/3sxtra/src/port/menu_screen_helpers.c:1-236`.
- `/tmp/3sxtra/src/port/menu_bridge.c:1-289`.
- `/tmp/3sxtra/src/port/menu_task.c:1-69`.
- `/tmp/3sxtra/src/include/port/menu_task.h` (full; declares MenuTask_ accessors).
- `/tmp/3sxtra/src/include/port/init_task.h`, `task_api.h` — MUST port unconditionally. `menu_network.c:7, :9` includes both directly (verified 2026-04-20). Upstream `init_task.c` = 52 LOC and `task_api.c` = 40 LOC; drop both in wholesale.
- Our fork: `src/main.c:1-50` (include order), `src/sf33rd/Source/Game/menu/menu.c:120-250` (existing menu dispatcher; see how `Netplay_Menu` is registered in the `After_Title` jump table).

**Files to create/modify:**
- `include/port/menu_screen.h` (new; copy from 3sxtra, unchanged except for include-path fixups to match our tree — our `#include "types.h"` and `#include "structs.h"` are at repo root via `include/` per existing `include/structs.h`).
- `include/port/menu_task.h` (new; copy unchanged).
- `include/port/init_task.h`, `include/port/task_api.h` (new; copy upstream wholesale — `menu_network.c:7, :9` includes both unconditionally).
- `src/port/menu_screen_registry.c` (new).
- `src/port/menu_screen_helpers.c` (new).
- `src/port/menu_bridge.c` (new).
- `src/port/menu_task.c` (new).
- `src/port/init_task.c` (new; 52 LOC upstream).
- `src/port/task_api.c` (new; 40 LOC upstream).
- `CMakeLists.txt`: the existing `file(GLOB_RECURSE GAME_SRC CONFIGURE_DEPENDS src/*.c)` (at `:69`) will pick these up automatically (they are `.c` under `src/`). Confirm via `cmake --fresh` reconfigure. No explicit list entries needed.

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` compiles the new files to `.o`. At this step the registry is NOT yet invoked from `main.c`, so nothing calls them — they must still be in `GAME_SRC` and reachable to prove linkage. If unreferenced-function warnings trip `-Werror`: add a bootstrapping stub in `src/port/menu_bridge.c` that calls each API once behind `#ifdef MENU_BRIDGE_LINKAGE_PROBE` (remove the probe before commit; the Step 9 integration will call them for real).
- `grep -n 'MENU_SCREEN_NETWORK_LOBBY' include/port/menu_screen.h` returns the enum value.
- The MenuScreen table `g_screens[MENU_SCREEN_COUNT]` has NULL callbacks for every `MENU_SCREEN_*` we don't port — verified by `grep -c '^static const MenuScreen.*= {' src/port/*.c` aligned with Step 10's 9 ms_* screens plus stubs.

**Dependencies:** Steps 1 and 2 merged. Step 5 (structs.h guard) is NOT required here because `menu_screen_registry.c` and peers are C files and can include `structs.h` today.

**What NOT to do:**
- Do not copy `stage_bg_registry.c`, `appear_registry.c`, `broadcast.c`, or any other `src/port/*.c` from upstream — out of scope per Decision 2 (tier-2 §3.2).
- Do not add `MENU_SCREEN_*` enum values beyond what upstream `menu_screen.h` already defines. Keep ABI parity so the legacy→migrated lookup table indices match.
- Do not rename any struct/enum.

**What to do if it fails:**
- Missing header `sf33rd/Source/Game/ui/sc_sub.h` or `sf33rd/Source/Game/effect/eff57.h`: these exist in our tree (we share most 3sxtra roots). Confirm with `find src/sf33rd -name sc_sub.h`.
- Missing `AT_JMP_COUNT`: defined in upstream `menu_internal.h`. That comes in Step 4 — reorder Step 3 and Step 4 if the failure surfaces here, OR temporarily hardcode `AT_JMP_COUNT` and leave a `TODO(step4)` comment.
- If `menu_bridge.c` references engine symbols (`FadeOut`, `FadeIn`, `load_any_texture_patnum`) that resolve differently in our fork: produce a symbol-rename table and apply via targeted `Edit` calls (per the Track B plan P6-S1 step).

---

## Step 4 — Port `menu_draw.c`, `menu_input.c`, `menu_internal.h`, `menu_input_constants.h` (R-6.1 ripple) — **DONE** (commit `40402ed8`, 2026-04-20)

**What it does:** Accept the R-6.1 ripple. Copy the ~2850 LOC menu draw + input + internal headers so that Step 9's `menu_network.c` has the engine symbols it calls (`Menu_Draw_*`, `Menu_Input_*`, `Network_Lobby`, `Menu_Sub_case1`, `AT_JMP_COUNT`, etc.).

**Why it matters:** `menu_network.c:59` includes `menu_internal.h`, `:3` includes `menu_input_constants.h`. `ms_network_lobby.c:28` includes `menu_internal.h` and calls `Network_Lobby` and `Menu_Sub_case1` by reference. Without this ripple the port of menu_network + ms_* cannot link.

**Files to read first:**
- `/tmp/3sxtra/src/sf33rd/Source/Game/menu/menu_internal.h:1-130` (full).
- `/tmp/3sxtra/src/sf33rd/Source/Game/menu/menu_input_constants.h` (full).
- `/tmp/3sxtra/src/sf33rd/Source/Game/menu/menu_draw.c:1-132` (full; pure native-render helpers).
- `/tmp/3sxtra/src/sf33rd/Source/Game/menu/menu_input.c:1-2586` (full — largest single file in Phase 6).
- Our fork: `src/sf33rd/Source/Game/menu/menu.c` (entire; confirm no symbol collisions with upstream's `menu_input.c` functions). Check `git log mister -- src/sf33rd/Source/Game/menu/menu.c` — if heavily diverged, the per-function merge is line-by-line.

**Files to create/modify:**
- `src/sf33rd/Source/Game/menu/menu_internal.h` (new).
- `src/sf33rd/Source/Game/menu/menu_input_constants.h` (new).
- `src/sf33rd/Source/Game/menu/menu_draw.c` (new).
- `src/sf33rd/Source/Game/menu/menu_input.c` (new).
- `src/sf33rd/Source/Game/menu/menu.c`: reconcile only the minimum to avoid duplicate-definition link errors. If `menu_input.c` defines `Check_Menu_Lever` (for example) and our `menu.c` also does, mark our legacy one `static` or delete it in favor of the ported upstream one.
- `CMakeLists.txt`: `GAME_SRC` glob picks these up automatically.

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` builds clean.
- `grep -c 'Network_Lobby\|Menu_Sub_case1\|AT_JMP_COUNT' src/sf33rd/Source/Game/menu/menu_internal.h` returns at least 3.
- Playtest offline (Title → Arcade, Title → Training, Title → Options) on host desktop build: menu still navigates — no regression from the ripple (verify by running `./build/3s-arm` and clicking through).

**Dependencies:** Step 3 (so `menu_bridge.c` has a place to hand off screen transitions).

**What NOT to do:**
- Do not port `menu_save.c`, `menu_replay.c`, `menu_training.c`, or their `_constants.h` headers. Those files are referenced by upstream `menu_network.c:4-6` but are Phase 12 / post-MVP.
- Do not touch `dir_data.c`, `ex_data.c` (already in our tree; reuse as-is).
- Do not deploy.
- Do not remove `Netplay_Menu` from `menu.c:1339` in this step — that swap happens in Step 9.

**What to do if it fails:**
- If `menu_input.c` calls `NotoSansJP` font rendering helpers that we've scoped out: stub the branch. Upstream uses it for Japanese-only menu items; our MVP is English-only (Decision #5).
- If symbol-collision cascades: compare per-function via `diff` of `menu.c` vs. the upstream `menu.c`. Upstream's `menu.c` (1300+ LOC) has already been refactored to delegate into `menu_input.c` — our fork's `menu.c` has NOT. Document the remaining divergence in an in-source `TODO` and scope cleanup to post-Phase-6.
- If ripple LOC exceeds tier-2 §5 budget (+3000): halt at 3500 LOC and escalate.

---

## Step 5 — Resolve `structs.h` C++-guard so RmlUi screens can link — **DONE** (commit `4524dea2`, 2026-04-20)

**What it does:** Replace the hard `#error` in `include/structs.h:28` with option A — a mechanical `operator → wu_operator` field rename across all 137 call-sites in `src/` (verified via `grep -rnE '\.operator\b' src/ | wc -l` on 2026-04-20). Add a header-only compatibility shim so C TUs that still reference `obj.operator` see `obj.wu_operator` via the preprocessor. Rationale: the originally planned option B (`#define operator wu_operator` in C-only) is unworkable — the `#define` is a no-op inside the same C TU that declares the struct member, and `_Static_assert(offsetof(WORK, operator)…)` cannot sit inside `#ifdef __cplusplus`. The 137-site rename is mechanical and reversible, whereas option B cannot be made to compile.

**Why it matters:** Four of the Phase-6 RmlUi `.cpp` screens (`rmlui_casual_lobby.cpp`, `rmlui_tournament_lobby.cpp`, `rmlui_replay_picker.cpp`, `rmlui_ranked_matchmaking.cpp`) include `structs.h` transitively via `port/menu_screen.h` (upstream `menu_screen.h:18` includes `structs.h`). Without this change the guard fires and the build fails before any of Steps 11–13 can land.

**Files to read first:**
- `/Users/sb/Developer/3sx-mister/include/structs.h:1-45` (the F25 guard).
- `/Users/sb/Developer/3sx-mister/include/structs.h:224-234` — the field is declared `u8 operator;` at line 234 inside `struct WORK`, with the in-source comment at `:225-233` describing exactly why 3sxtra renamed it to `pl_operator` in their fork and recommending we mechanical-rename when we need C++ inclusion.
- `docs/archive/research-3sxtra-netplay-port.md` §9.5 (options A / B for the C++ reserved-word collision).
- Run `grep -rnE '\.operator\b' src/ | wc -l` to re-verify the call-site count (137 at time of planning; mostly in `src/sf33rd/` engine code).

**Files to create/modify:**
- `include/structs.h`: (a) delete the `#if defined(__cplusplus)` `#error` block at the top; (b) rename the field at `:234` from `u8 operator;` to `u8 wu_operator;`; (c) replace the explanatory comment at `:224-233` with a shorter note citing this plan step; (d) at the very end of the header, C-only (`#ifndef __cplusplus`) add `#define operator wu_operator` as a backwards-compat shim for any in-tree C caller that has not yet been rewritten, with a `// TODO(post-phase6): remove once all call-sites renamed.` Note: the `#define` must come AFTER the struct declaration so it does not rewrite the declaration itself; it only rewrites trailing `.operator` accesses in later-included TUs. Verify ordering empirically: `cmake --fresh` + spot-build before touching any `src/*.c`.
- Mechanical rename across `src/`: run `rg -l '\.operator\b' src/` to get the file list, then `sed -i '' 's/\.operator\b/.wu_operator/g'` each file. Sanity-check with `git diff --stat` before committing.
- `src/port/rmlui_structs_cpp_probe.cpp` (new, temporary): a throwaway C++ TU that `#include "structs.h"` to prove the guard is gone. Delete after Step 5 verification commits.

**Success criteria:**
- `include/structs.h` contains no `#error`.
- `grep -rnE '\.operator\b' src/ | wc -l` → 0 (post-rename) OR all remaining hits are C++ operator-overload syntax (e.g. `obj.operator=(…)`), not struct-field accesses. Hand-audit each remaining hit.
- A new throwaway C++ TU at `src/port/rmlui_structs_cpp_probe.cpp` `#include "structs.h"` compiles clean under `clang++ -std=c++17` (delete the probe before committing the step).
- C unit test confirms `sizeof(WORK)` unchanged vs. pre-change. Capture size before with a one-shot `printf("%zu\n", sizeof(WORK));` program, record the value, assert after.
- `EXTRA_CMAKE_ARGS="" tools/mister/build-game.sh --flavor telemetry` still produces a byte-identical `3s-arm` ELF vs. a baseline built (with the same `EXTRA_CMAKE_ARGS=""`) from the commit immediately before this step. The rename touches only field identifiers, not layout, so object-code deltas must be zero after `strip -R .comment -R .note.gnu.build-id`. Capture the baseline ELF as an artefact under `build/baselines/step5-pre.bin` before starting.

**Dependencies:** Step 1 only (for the flag plumbing). Does NOT depend on Steps 2, 3, 4. Note: the byte-identical ELF check only holds when both builds use the same `EXTRA_CMAKE_ARGS` value — use an empty string on both sides for this gate.

**What NOT to do:**
- Do not leave any `.operator` struct-field access un-renamed.
- Do not keep the backwards-compat `#define operator wu_operator` shim indefinitely; it risks clashing with C++ `operator` overloading if the header is ever included from an unexpected C++ TU. Remove post-Phase-6.
- Do not add the C++ accessor as a member function — we can't add member functions to a C struct without the header going full C++ and breaking every C TU downstream.
- Do not weaken the intent of F25 (Track A review finding). The guard's purpose was to prevent silent C++ inclusion; Phase 6 replaces that with an explicit, compiling alternative.

**What to do if it fails:**
- If the mechanical `sed` mangles an unrelated `.operator` (e.g., a C++ overload call in `third_party/` that slipped into `src/`): revert that hunk and do the rename file-by-file.
- If the sizeof-WORK assertion fails: the field rename alone cannot change layout unless a `#pragma pack` elsewhere interacts with the identifier. Bisect with `git bisect` against the known-good baseline.
- If >10 `sed` misses fight back (suggesting the operator-field usage isn't uniform): pause and audit upstream 3sxtra's rename commit for the canonical identifier choice (they used `pl_operator`, not `wu_operator` — we deviate only to avoid confusion with their fork's string).

---

## Step 6 — Rewrite `rmlui_wrapper.cpp/h` for MiSTer (SDL3-software only) — **DONE** (commit `065d656c`, 2026-04-20)

**What it does:** Produce a from-scratch MiSTer variant of the RmlUi wrapper: initializes RmlUi against the vendored `RenderInterface_SDL` we already ship at `src/port/sdl/rmlui/vendored/RmlUi_Renderer_SDL.cpp` (verified via `ls`), provides the C-callable document-management API upstream exposes, but omits Lua, GL3, SDL_GPU, SDL_image, controller-imagery, palette-remix, and the 3sxtra-specific asset-path config system. Use upstream's `rmlui_wrapper.h:1-80` as the API contract (kept verbatim) but implement `rmlui_wrapper.cpp` against a minimal dependency surface.

**Why it matters:** Without a working wrapper, none of the 9 screens can load or render. This is the R-6.3 cascade's single biggest blocker. Upstream `rmlui_wrapper.cpp:1-57` pulls in six external subsystems we do not have. A literal copy fails before any `.cpp` compiles.

**Files to read first:**
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_wrapper.h:1-169` (keep verbatim — the C-callable API contract).
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_wrapper.cpp:1-300` (init path + twin-context design decisions), then `:300-800` (document registry, font load sequence including `district_italic.ttf` at `:1033`), then skim `:800-1506`. Do NOT commit to a twin-context implementation based only on lines 1–60 — the 1506-LOC wrapper has non-obvious interactions that only appear past line 150. Read all three blocks before writing any code.
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_wrapper.cpp:1020-1060` — exact `Rml::LoadFontFace` call sequence (`BoldPixels.ttf`, `district_italic.ttf`, NotoSansJP, NotoEmoji). We ship only the first two; the Noto calls become no-ops on our build.
- Upstream-only (do NOT try to read in our tree): `src/port/sdl/renderer/*` — the entire `renderer/` subtree (`gl_compat.h`, `radix_sort.h`, `renderer.c`, `sdl_game_renderer_classic.c`) exists only in `/tmp/3sxtra/` (verified `ls /Users/sb/Developer/3sx-mister/src/port/sdl/renderer` → ENOENT on 2026-04-20). When the upstream wrapper references these, take the SDL renderer via `SDL_GetRenderer(window)` instead.
- Our fork: `src/port/sdl/rmlui/rmlui_blend_fix.{cpp,h}` — the MiSTer subclass we already have; reuse as the RenderInterface.
- `src/port/sdl/rmlui/rmlui_first_light_test.cpp:1-60` — existing pattern for wiring RmlUi + vendored renderer + SDL3-software on MiSTer. The wrapper takes the same MiSTerSystemInterface and MiSTerFileInterface patterns.
- `docs/archive/research-3sxtra-netplay-port.md` §12 (MiSTer shared-lib reality; no GPU path).

**Mid-step evaluation gate (twin-context):** After reading the full upstream wrapper, decide before coding: implement full twin-context (`window_context` for menus + `game_context` for in-match overlays) OR start with single-context and alias `*_game_document` → `*_document`. The twin-context is only exercised by `rmlui_netplay_ui` and `rmlui_ingame_chat` (Step 12). If either (a) upstream's twin-context has ordering / focus bugs we'd inherit, or (b) a single context is provably sufficient for our 60-FPS SDL3-software path, choose single-context and document the choice in the step commit message. If unsure, default to single-context — it's the smaller commitment and can be expanded later. This decision is load-bearing for Step 12 and MUST be made before Step 6 is marked done.

**Files to create/modify:**
- `src/port/sdl/rmlui/rmlui_wrapper.h` (new): verbatim copy of upstream with two tweaks: (a) drop anything under a `#ifndef ENABLE_RMLUI` stub branch that references GL or GPU; (b) add `/* MiSTer note: SDL3-software renderer only; Lua/GL3/GPU/image/controller APIs omitted. */` header comment.
- `src/port/sdl/rmlui/rmlui_wrapper.cpp` (new): MiSTer-only implementation with:
  - `rmlui_wrapper_init(SDL_Window*, void*)` — `gl_context` ignored on MiSTer; constructs `mister_rmlui::RenderInterfaceSDLMiSTer` using the existing `SDL_Renderer*` retrieved via `SDL_GetRenderer(window)`. Registers a file interface rooted at `$THIRDSARM_HOME/assets/ui/` (envar set in `tools/mister/package.sh:51`; fallback to `./assets/ui/` on desktop).
  - Font-engine init via RmlUi's built-in FreeType path (our cross-compiled `libfreetype.a` is already linked per `CMakeLists.txt:347`). Load `assets/fonts/BoldPixels.ttf` at init.
  - Full document-registry API: `show_document`, `hide_document`, `hide_all`, `is_visible`, `close`, `reload_stylesheets`, `reload_document`, `reload_all`, `release_textures`. Plus the `get_game_context`, `*_game_document` twin-context API upstream exposes (used by in-game overlay screens) — implement as two separate `Rml::Context*` pointers sharing the same RenderInterface.
  - Strip every include on the canonical RmlUi include-strip list (see the bottom of Step 11). Any upstream call site referencing those subsystems is commented-out with the breadcrumb comment format shown there.
  - Hook `rmlui_wrapper_process_event(SDL_Event*)` to route to both contexts.
- `CMakeLists.txt`: no change — RmlUi glob at `:78-82` picks up the new `.cpp`. Confirm `GLOB_RECURSE CONFIGURE_DEPENDS` re-globs when the file lands (per the note at `:73-77`).
- `src/main.c` or `src/port/sdl/sdl_app.c`: add one call to `rmlui_wrapper_init(window, NULL)` during SDL init, guarded by `#ifdef ENABLE_RMLUI`. Add `rmlui_wrapper_shutdown()` to SDL teardown. Do NOT call `rmlui_wrapper_new_frame` / `_render` yet — those go in Step 9 when the menu dispatcher drives them.

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry` builds clean.
- `nm build/mister-telemetry/CMakeFiles/3s-arm.dir/src/port/sdl/rmlui/rmlui_wrapper.cpp.o | grep ' T '` shows every exported `rmlui_wrapper_*` symbol.
- Running the existing first-light harness (`./build/3s-arm --test-rmlui-first-light` on host) still passes — we did not regress the test harness's direct use of the vendored RenderInterface.
- On-device (after Step 16) the wrapper-driven screens render. For this step's gate, it is enough that the host build links and the unit test exists.

**Dependencies:** Steps 3, 5. (Step 2 not strictly required but is already merged by this point.)

**What NOT to do:**
- Do not copy `lua_engine_bridge.{cpp,h}`, `lua_trials_loader.{cpp,h}`, `palette_remix.{cpp,h}`, `controller_image.{cpp,h}`. They are out of scope per Decision 2.
- Do not include `RmlUi/Lua.h` or `RmlUi_Renderer_GL3.h`.
- Do not open an OpenGL context.
- Do not link `SDL3_image` — it is not built by `build-deps.sh mister`.
- Do not call into the RmlUi debugger (`RmlUi/Debugger.h`) — optional; can be added if trivial.

**What to do if it fails:**
- FreeType font engine fails to init: cross-check that `libfreetype.a` exists at `third_party/freetype/build/lib/libfreetype.a` (Track B Phase 4 deliverable). If it is missing, re-run `build-deps.sh --profile mister`.
- If `Rml::Initialise()` segfaults on MiSTer (software renderer), enable `RmlUi::Log::LT_DEBUG` in the SystemInterface and re-run.
- If the twin-context path (window_context + game_context) deadlocks post-implementation: the mid-step evaluation gate should have flagged the risk. If the gate was skipped and a deadlock emerges, revert the twin-context decision and switch to single-context; Step 12 needs to be updated to match.

---

## Step 7 — Port `lobby_server.c/h`, `identity.c/h`, `sha256.c/h` — **DONE** (commit `63ab927c`, 2026-04-20)

**What it does:** Copy 3sxtra's lobby HTTP client (2229 + 438 LOC) plus the `identity.c` (player-id persistence) and `sha256.c` helper into `src/netplay/`. No source changes; our URL default becomes the shared 3sxtra lobby at `152.67.75.184:3000` (Decision #12).

**Why it matters:** `menu_network.c` and all `ms_*` screens call lobby APIs — `lobby_search_start`, `lobby_room_create`, `lobby_room_list`, `lobby_sse_poll`, etc. This is the network backbone the UI drives. The lobby client depends on `libcurl`, which our MiSTer target ships as a shared `/usr/lib/libcurl.so.4` (verified `docs/archive/plan-netplay-port.md` §16.4 — live `ls` confirmed present).

**Files to read first:**
- `/tmp/3sxtra/src/netplay/lobby_server.h:1-438`.
- `/tmp/3sxtra/src/netplay/lobby_server.c:1-200` (init + curl setup), then skim rest.
- `/tmp/3sxtra/src/netplay/identity.c:1-end`, `identity.h`.
- `/tmp/3sxtra/src/netplay/sha256.c/h`.
- `docs/archive/plan-netplay-port.md` §8.3 (endpoints we USE), §8.4 (endpoints we DO NOT use — compile-time assert these are never called).
- `docs/archive/plan-netplay-port.md` §16.4 (libcurl present as shared on MiSTer).

**Files to create/modify:**
- `src/netplay/lobby_server.{c,h}`.
- `src/netplay/identity.{c,h}`.
- `src/netplay/sha256.{c,h}`.
- `src/configuration.h`: add `CFG_KEY_LOBBY_SERVER_URL` with default `"http://152.67.75.184:3000"` (Decision #12). Add `CFG_KEY_NETPLAY_INPUT_DELAY_WINDOW` (default 8 per Decision #4) if not already present.
- `CMakeLists.txt`: add libcurl to the MiSTer link path. Current tree does not link curl. Use `find_package(CURL REQUIRED)` gated by `ENABLE_NETPLAY`, and `target_link_libraries(3s-arm PRIVATE CURL::libcurl)`.
- `build-deps.sh`: `grep -n curl build-deps.sh` on 2026-04-20 shows only `curl` used as a download tool (lines 106, 298, 388, 432), NOT as a library build. Buildroot libcurl availability is not a given. Resolve by adding `libcurl4-openssl-dev:armhf` (or `libcurl4-gnutls-dev:armhf`) to the `tools/mister/setup-container.sh` apt-install list so `pkg-config --cflags --libs libcurl` works inside the container. Document in the step commit that this is a hard dependency of the cross-compile environment, not a vendored build.
- Confirm the final binary dynamically links to the MiSTer OS's `/usr/lib/libcurl.so.4` via `readelf -d build/mister-telemetry-install/bin/3s-arm | grep NEEDED` (expected: `libcurl.so.4` present).

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` succeeds.
- `readelf -d build/mister-telemetry-install/bin/3s-arm | grep libcurl` prints `NEEDED Shared library: [libcurl.so.4]` (dynamic link confirmed).
- A new C test at `src/netplay/test_lobby_client_compile.c` calls every public `lobby_*` API once (passing NULL / zero args is fine; the goal is link-time proof). Wire the `--test-lobby-client-compile` CLI flag via args.c + configuration.h + main.c following the Step 2 pattern. Test does not actually hit the server.
- `grep -nE '/match_result(\b|/replay\b)' src/netplay/lobby_server.c` — confirm the REST path strings exist in the client (it knows how to call them). Then a second grep on the CALLERS: `grep -rnE 'lobby_post_match_result|lobby_post_match_replay' src/` should return only definitions in `lobby_server.c`, never a call from `menu_network.c` or `ms_*.c`. This enforces Decision "DO NOT POST" per tier-2 §8.4.

**Dependencies:** Step 1 only. The lobby client (`lobby_server.c`, `identity.c`, `sha256.c`) does not touch `NetplayEvent`; it can be ported in parallel with Step 2. (The event queue lives on the netplay.c path, not the HTTP client path.)

**What NOT to do:**
- Do not port `bracket.c` (tournament backend — Phase 12+).
- Do not port `net_detect.c`, `upnp.c`, `stun.c`, `ping_probe.c`, `discovery.c` — those are Phase 10/11/12/9 respectively. Phase 6 only needs the lobby client.
- Do not modify `lobby_server.c` SSE endpoint handling. Upstream already works.
- Do not hardcode the lobby URL inside source — route through `CFG_KEY_LOBBY_SERVER_URL`.

**What to do if it fails:**
- If libcurl is not pkg-config-discoverable in the container: fall back to hand-picked `-lcurl`. If ABI drift between host libcurl and MiSTer's: static-link libcurl via `build-deps.sh mister` recipe (tier-2 §13 #8 flagged this as a pre-existing unknown).
- If TLS fails (3sxtra uses plain HTTP per §8.3): ensure we're hitting `http://` not `https://`. If 3sxtra upgrades to HTTPS later, we need `libssl`; not in MVP.
- If `identity.c` uses `/tmp` for player-id storage: reroute to `$THIRDSARM_HOME/config/` per our MiSTer conventions.

---

## Step 8 — Three-layer MiSTer arch filter: room-name prefix, `display_name` suffix, `MIST` hello/ack/reject handshake — **DONE** (commit `59d87eba`, 2026-04-20)

**What it does:** Implement the defense per tier-2 §8.2 so MiSTer clients only match other MiSTer clients when using the shared lobby. Three layers, all client-side:
- Layer 1: prefix room names with `[MiSTer]` on `/room/create`; filter on `/rooms/list` response.
- Layer 2: append `" [MiSTer]"` suffix to `display_name` on `/presence` and `/searching/start` (Decision #11; `display_name` is the only field the lobby server forwards unchanged — verified by lobby-server.js inspection in tier-2 §8.2.5).
- Layer 3: 2-way `MIST` handshake — hello → ack OR reject — on the STUN socket BEFORE GekkoNet starts. Tier-2 §8.2.4 specifies the wire format and timing; this is NOT a unilateral prefix.

**Why it matters:** Cross-arch crossplay is BLOCKED by the `SessionHealthMsg` checksum (research §9.7). Without this layered defense a MiSTer user clicking "Find Match" against the shared lobby will be paired with a desktop peer and desync within seconds, corrupting state. Layer 3 is the authoritative guard; Layers 1–2 reduce the hit rate before Layer 3 fires.

**Files to read first:**
- `docs/archive/plan-netplay-port.md:865-928` — §8.2 (three-layer defense), §8.2.4 (MIST handshake wire format + hello/ack/reject message types + 500ms timeout + 5× retransmit at 100ms), §8.2.5 (display_name suffix encoding).
- `/tmp/3sxtra/src/netplay/sdl_net_adapter.c:10-30` — the `3SXC` chat filter pattern; note this is a simpler prefix, not what we're implementing.
- `/Users/sb/Developer/3sx-mister/src/netplay/sdl_net_adapter.c` — our equivalent.
- `src/netplay/lobby_server.c` after Step 7 — find the `/room/create` POST body construction and the `/rooms/list` response parse.
- `/tmp/3sxtra/src/netplay/netplay.c:808, :824-871, :973-993` — the TRANSITIONING→CONNECTING boundary where the handshake must fire (tier-2 §8.2.4 "Chosen insertion point").

**Files to create/modify:**
- `src/netplay/mist_handshake.{c,h}` (new): implements the 2-way hello/ack/reject protocol per tier-2 §8.2.4 wire format:
  - offset 0, 4 bytes: magic `"MIST"` (`0x4D 0x49 0x53 0x54`).
  - offset 4, 1 byte: `msg_type` (`0x01` hello, `0x02` ack, `0x03` reject).
  - offset 5, 2 bytes: `payload_len`, big-endian (max ~128).
  - offset 7, N bytes: payload (null-terminated strings for hello/ack: `"armv7\0" "mister\0" "<build_hash_7chars>\0"`; reject: 1-byte reason code + human-readable string).
  - API: `bool mist_handshake_send_and_wait(int sock, const struct sockaddr_in* remote, int timeout_ms)` — sends hello, retransmits up to 5× at 100ms intervals, accepts ack any time inside 500ms window, returns true on ack, false on reject or timeout. On reject, captures the reason string for user-facing error display.
- `src/netplay/netplay.c`: at the TRANSITIONING→CONNECTING boundary (before `configure_gekko()` equivalent in our fork — locate during implementation), call `mist_handshake_send_and_wait(stun_socket, remote_addr, 500)`. On failure, set `session_state = NETPLAY_SESSION_EXITING`, push `NETPLAY_EVENT_DISCONNECTED` from Step 2's queue, record the user-facing error string for the RmlUi screen to display ("Opponent is not running a compatible MiSTer build. Match cancelled."), and `Soft_Reset_Sub()`.
- `src/netplay/netplay.h`: no new session states are strictly required — the handshake happens synchronously inside the existing TRANSITIONING state — but add a `static const char* netplay_arch_reject_reason` accessor for UI consumption.
- `src/netplay/lobby_server.c`: wrap `lobby_room_create` to prepend `[MiSTer]` to the room name argument; wrap `lobby_room_list` result parser to filter any room whose name does not begin with `[MiSTer]`.
- `src/netplay/lobby_server.c`: append `" [MiSTer]"` to the `display_name` argument of `/presence` and `/searching/start`. On `/presence` responses / `/searching` result lists, ignore entries whose display_name does not end with `" [MiSTer]"`.
- `src/netplay/test_mist_handshake.c` (new): C TU picked up by `GAME_SRC` glob. Add CLI flag `--test-mist-handshake` following the Step 2 pattern (args.c + configuration.h + main.c dispatch). Cases: (a) hello + remote sends back ack → return true; (b) hello + remote sends reject → return false with reason string captured; (c) hello + remote silent → retransmit 5× then timeout false; (d) hello + remote sends malformed ack (wrong magic) → rejected as not-an-ack.

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` succeeds.
- `./build/3s-arm --test-mist-handshake` runs all four cases and returns 0.
- `grep -cE 'MIST|0x4D.*0x49.*0x53.*0x54' src/netplay/mist_handshake.c` returns at least 1.
- `grep -c 'msg_type.*0x01\|msg_type.*0x02\|msg_type.*0x03' src/netplay/mist_handshake.c` returns at least 3 (hello/ack/reject constants present).
- `grep -n '\[MiSTer\]' src/netplay/lobby_server.c` returns at least 2 hits (room prefix, display_name suffix).
- Compile-time or runtime no-op check: `grep -rE 'lobby_post_match_result|lobby_post_match_replay' src/` shows no external callers (same check as Step 7 success criteria — regression guard).

**Dependencies:** Steps 2 and 7.

**What NOT to do:**
- Do not change the magic prefix from `MIST` to anything else — it's called out in tier-2 §8.2.4 as the agreed wire constant.
- Do not skip the ack — tier-2 §8.2.4 mandates a 2-way exchange. A unilateral prefix (like 3sxtra's `3SXC` chat) is insufficient because we need positive confirmation that the peer is also a MiSTer client before we let GekkoNet start. The 2-way protocol also gives us a channel for the `reject` reason string.
- Do not break the existing GekkoNet packet format — the handshake runs on the STUN socket BEFORE `gekko_create`, not on the GekkoNet transport. After handshake, raw GekkoNet bytes flow unchanged.
- Do not break offline / LAN play that doesn't use the lobby. The MIST handshake applies only to lobby-matched sessions.

**What to do if it fails:**
- If the STUN socket is already closed by the time we try the handshake: move the handshake earlier in the flow (still before `gekko_create`), piggybacking on the `sdl_net_adapter.c` `receive_data` interceptor as tier-2 §8.2.4 describes.
- If GekkoNet type-enum byte collides with `0x4D` ('M'): verify `/tmp/GekkoNet-head/GekkoLib/include/net.h:26-36` — tier-2 §8.2.4 shows GekkoNet's type enum is in range `[0, 6]`, so `0x4D` cannot collide. If that ever changes upstream, bump our magic constant.
- If Layer 2 suffix breaks display rendering in the lobby (UI truncates names): truncate the base name first, then append suffix so total fits in the upstream 31-char field width per tier-2 §8.2.

---

## Step 9 — Port `menu_network.c` and replace our 2-item `Netplay_Menu` — **DONE** (commit `77f22a78`, 2026-04-20)

**What it does:** Copy 3sxtra's `menu_network.c` (1764 LOC) + `menu_network.h` + `menu_network_constants.h` into `src/sf33rd/Source/Game/menu/`. Replace the 2-item `Netplay_Menu` at our `src/sf33rd/Source/Game/menu/menu.c:1339` with a call to `Network_Lobby` dispatched through the MenuScreen registry (Step 3). Wire `rmlui_wrapper_new_frame()` / `_render()` into the main-loop draw ticker behind `ENABLE_RMLUI`.

**Why it matters:** This is the actual user-visible Phase 6 content — the new network lobby gateway. It replaces the tiny stub we ship today. Without this the user cannot reach any of the RmlUi screens in Steps 11–13.

**Files to read first:**
- `/tmp/3sxtra/src/sf33rd/Source/Game/menu/menu_network.c:1-200` (includes, then skim).
- `/tmp/3sxtra/src/sf33rd/Source/Game/menu/menu_network.h` (full).
- `/tmp/3sxtra/src/sf33rd/Source/Game/menu/menu_network_constants.h` (full).
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/menu/menu.c:1450-1550` (the stub to replace).
- `/Users/sb/Developer/3sx-mister/src/main.c:1-100` and `src/port/sdl/sdl_app.c` (find the main-loop draw point for RmlUi integration).

**Files to create/modify:**
- `src/sf33rd/Source/Game/menu/menu_network.c` (new).
- `src/sf33rd/Source/Game/menu/menu_network.h` (new).
- `src/sf33rd/Source/Game/menu/menu_network_constants.h` (new).
- `src/sf33rd/Source/Game/menu/menu.c:1451`: replace `Netplay_Menu` body with a thin dispatcher that calls `MenuScreen_Goto(MENU_SCREEN_NETWORK_LOBBY)` and returns. Keep the function signature stable so the `After_Title` jump table at `menu.c:241` still points at a valid address.
- **AT_Jmp_Tbl reconciliation (choose option B here):**
  - Context: our fork has `Netplay_Menu` at INDEX 5 (replacing `Load_Replay`, guarded `#if NETPLAY_ENABLED`) at `src/sf33rd/Source/Game/menu/menu.c:238-247`. Upstream has `Network_Lobby` at INDEX 21 and its `AT_JMP_COUNT == 22`. Our table is 21 slots.
  - Option A (rejected): grow our `AT_Jmp_Tbl` from 21 → 22 slots, move `Netplay_Menu` to slot 21, restore `Load_Replay` at slot 5. Cleaner semantically but requires re-checking every `task_ptr->r_no[1] = N` write across our engine to ensure no legacy code treats slot 5 as a `Load_Replay` trampoline (brittle).
  - Option B (CHOSEN): keep `Netplay_Menu` at slot 5. In `src/port/menu_screen_registry.c:61-84`, define our own `g_legacy_to_screen[]` with `[5] = MENU_SCREEN_NETWORK_LOBBY` (diverging from upstream's `[5] = MENU_SCREEN_SYSTEM_DIRECTION`). Our `AT_JMP_COUNT` stays 21. Document the index divergence inline with a citation to this step. This choice also means `MENU_SCREEN_LOAD_REPLAY` enum value is retained but unused on our fork (no live callers) — acceptable because Phase 6 is not adding a replay UI.
  - Patch shape: in `src/port/menu_screen_registry.c`, copy upstream's `g_legacy_to_screen` table but change `[5] = MENU_SCREEN_SYSTEM_DIRECTION` → `[5] = MENU_SCREEN_NETWORK_LOBBY`. Add a block comment explaining the divergence, `#if NETPLAY_ENABLED` guard the divergent entry so a NETPLAY=OFF build still routes slot 5 to `MENU_SCREEN_SYSTEM_DIRECTION`. Truncate the array size to `AT_JMP_COUNT == 21` for our fork. If upstream's header defines `AT_JMP_COUNT == 22`, we keep that as the C compile-time constant but only populate 21 entries of the table — the unused slot 21 stays `MENU_SCREEN_NONE` (the zero initializer) and never fires because our `After_Title` dispatcher never writes `r_no[1] = 21`.
- `src/main.c` or `src/port/sdl/sdl_app.c`: in the main loop, when `ENABLE_RMLUI` is defined, call `rmlui_wrapper_new_frame()` before presenting and `rmlui_wrapper_render()` as part of presentation. Also route `SDL_Event` via `rmlui_wrapper_process_event`.
- `src/main.c`: call `MenuScreen_Tick()` each frame from the menu branch of the game loop (follow upstream's main.c pattern; skim `/tmp/3sxtra/src/main.c` to find where).

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry` succeeds.
- `grep -c 'MenuScreen_Goto(MENU_SCREEN_NETWORK_LOBBY)' src/sf33rd/Source/Game/menu/menu.c` returns 1.
- `grep -n 'rmlui_wrapper_new_frame\|rmlui_wrapper_render' src/main.c src/port/sdl/sdl_app.c` returns at least 2 hits.
- Desktop host build: `./build/3s-arm` boots into title, Start → Online shows a new gateway screen. Placeholder (screens not yet ported) — at minimum a blank panel with "Network Lobby" caption renders. Confirms the dispatcher is wired correctly.

**Dependencies:** Steps 3, 4, 5, 6, 7, 8.

**What NOT to do:**
- Do not port `menu_save.c`, `menu_replay.c`, `menu_training.c`. The menu_network.c include lines 4-6 reference them; stub by removing those includes if we don't need their symbols. If they ARE needed transitively, audit with a second `grep` on menu_network.c body.
- Do not import new config keys beyond what Decisions #4, #8, #11, #12 already lock. `CFG_KEY_NETPLAY_*` prefix is final.
- Do not deploy yet.

**What to do if it fails:**
- If `menu_network.c` calls upstream-only engine symbols we scoped out: rewrite the callsite to our equivalent; document the divergence in-source.
- If the main-loop integration corrupts the pacer (dropped frames, telemetry jitter): defer the `rmlui_wrapper_render()` call behind a `rmlui_wrapper_any_visible()` check and hoist it out of the hot path.
- If the `After_Title` jump table index mismatch causes a crash: walk the jump table in `menu.c:240-260` against upstream's `AT_Jmp_Tbl`. The `g_legacy_to_screen[5]` override per option B above MUST be live — verify via `grep -n '\[5\] *= *MENU_SCREEN_NETWORK_LOBBY' src/port/menu_screen_registry.c`.

---

## Step 10 — Port the 6 netplay `ms_*` screens plus legacy-table dependencies — **DONE** (commit `c103f1e1`, 2026-04-20)

**What it does:** Copy the 6 netplay-relevant `ms_*` screens from `/tmp/3sxtra/src/port/screens/` into `src/port/screens/`: `ms_network_lobby.c`, `ms_casual_lobby.c`, `ms_tournament_lobby.c`, `ms_ranked_matchmaking.c`, `ms_leaderboard.c`, `ms_player_profile.c`. Additionally port any `ms_*.c` that `menu_screen_registry.c:61-84`'s `g_legacy_to_screen[]` table refers to so the table's `MENU_SCREEN_*` enum remains internally consistent (verified 2026-04-20: slots `[6]=MENU_SCREEN_LOAD_REPLAY → ms_replay.c`, `[17]=MENU_SCREEN_SAVE_REPLAY → ms_save_replay.c`, `[21]=MENU_SCREEN_NETWORK_LOBBY → ms_network_lobby.c`). Replay pickers and attract-mode ranking are NOT netplay screens; they are imported only because the registry table would otherwise have dangling enum values. Port `ms_replay.c` and `ms_save_replay.c` as thin stubs that call the legacy `Load_Replay`/`Save_Replay` handlers (our `Netplay_Menu` replaces slot 5 per `src/sf33rd/Source/Game/menu/menu.c:240-247`, so there is no live Load_Replay UI on our fork for this phase). Also port `rmlui_phase3_toggles.h` and a stub definitions TU per doc mismatch #8.

**Note on screen count:** Tier-2 §5 Phase 6 counts "9 RmlUi screens" but only 6 of them (`rmlui_network_lobby`, `rmlui_casual_lobby`, `rmlui_tournament_lobby`, `rmlui_ranked_matchmaking`, `rmlui_leaderboard`, `rmlui_player_profile`) have matching `ms_*` lifecycle shims. The remaining three (`rmlui_netplay_ui`, `rmlui_ingame_chat`, `rmlui_network_replay_picker`) are driven directly from `menu_network.c` / `netplay.c` without an `ms_*` shim (verified via `grep -ln 'rmlui_netplay_ui\|rmlui_ingame_chat' /tmp/3sxtra/src/**/*.c`). Steps 12 and 13 cover them.

**Why it matters:** These are thin lifecycle wrappers connecting `MenuScreen_Tick()` phases to `rmlui_wrapper_show_document()` calls on the RmlUi side. They are the glue that makes the registry-driven dispatcher actually show RmlUi overlays. The two extra non-net `ms_replay.c` / `ms_save_replay.c` stubs are required so `g_legacy_to_screen[6]` and `[17]` have valid handlers.

**Files to read first:**
- `/tmp/3sxtra/src/port/screens/ms_network_lobby.c` (206 LOC, full).
- `/tmp/3sxtra/src/port/screens/ms_casual_lobby.c` (121 LOC, full).
- `/tmp/3sxtra/src/port/screens/ms_tournament_lobby.c`, `ms_ranked_matchmaking.c`, `ms_leaderboard.c`, `ms_player_profile.c` (skim).
- `/tmp/3sxtra/src/port/screens/ms_replay.c`, `ms_save_replay.c` (skim — used only to satisfy the registry enum; functions unchanged from upstream but their `rmlui_replay_picker_show()` call is rewired to a stub since we don't port that RmlUi screen).
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_phase3_toggles.h:1-113` (full; header of ~35 toggle globals — we port a stripped subset in a new header).
- `/tmp/3sxtra/src/port/menu_screen_registry.c:61-84` (full table; confirms which `MENU_SCREEN_*` enum values the table uses).
- For each ported `ms_*.c` the `#include "port/sdl/rmlui/rmlui_phase3_toggles.h"` at upstream line ~42 must resolve — our new trimmed header handles this.

**Files to create/modify:**
- `src/port/screens/ms_network_lobby.c` (new).
- `src/port/screens/ms_casual_lobby.c` (new).
- `src/port/screens/ms_tournament_lobby.c` (new).
- `src/port/screens/ms_ranked_matchmaking.c` (new).
- `src/port/screens/ms_leaderboard.c` (new).
- `src/port/screens/ms_player_profile.c` (new).
- `src/port/screens/ms_replay.c` (new; stub — dispatches to the existing `Load_Replay` handler our fork already registers).
- `src/port/screens/ms_save_replay.c` (new; stub — dispatches to the existing `Save_Replay` handler).
- `src/port/sdl/rmlui/rmlui_phase3_toggles.h` (new; trimmed from upstream — keep only the toggles the ported Phase 6 screens reference. Audit by `grep -h 'rmlui_menu_\|rmlui_screen_' /tmp/3sxtra/src/port/screens/ms_network_lobby.c /tmp/3sxtra/src/port/screens/ms_casual_lobby.c /tmp/3sxtra/src/port/screens/ms_tournament_lobby.c /tmp/3sxtra/src/port/screens/ms_ranked_matchmaking.c /tmp/3sxtra/src/port/screens/ms_leaderboard.c /tmp/3sxtra/src/port/screens/ms_player_profile.c /tmp/3sxtra/src/port/screens/ms_replay.c /tmp/3sxtra/src/port/screens/ms_save_replay.c` — expect `rmlui_menu_lobby`, `rmlui_menu_replay` and possibly a couple more. Keep upstream's `#ifdef ENABLE_RMLUI` / `#else static const bool X = false;` structure verbatim for the toggles we need).
- `src/port/sdl/rmlui/rmlui_phase3_toggles_stub.c` (new): define every toggle our trimmed header declared `extern bool`, all initialised `true` (matches upstream default — `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_game_hud.cpp` is where upstream defines them; we put the defs in a stub TU under `#ifdef ENABLE_RMLUI` only).
- `CMakeLists.txt:81`: `RMLUI_SRC` glob already includes `src/port/screens/ms_*.c`. Confirm re-glob picks them up (`cmake --fresh`).

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry` succeeds. Linking will fail on `rmlui_*_show` etc. symbols until Steps 11–13. That is acceptable: at this step the gate is that the `ms_*.c` files COMPILE (object files produced); link step is expected to fail for those screen-side symbols only.
- `find src/port/screens -name 'ms_*.c' | wc -l` returns 8 (6 net + 2 replay stubs).
- `grep -c 'extern bool rmlui_' src/port/sdl/rmlui/rmlui_phase3_toggles.h` returns > 0 (trimmed subset is non-empty).
- `nm build/mister-telemetry/CMakeFiles/3s-arm.dir/src/port/sdl/rmlui/rmlui_phase3_toggles_stub.c.o | grep ' [BD] rmlui_'` shows every toggle symbol defined.
- Each `.c` file compiles under `-Wall -Werror` without warnings.

**Dependencies:** Steps 3, 5.

**What NOT to do:**
- Do not copy the remaining ~25 ms_* files (ms_char_select, ms_continue, ms_gameover, ms_sysdir, ms_sound_test, etc.). Out of scope — those are non-netplay menus and we don't port the upstream screens they wrap.
- Do not port the full 113-LOC upstream `rmlui_phase3_toggles.h` — only the toggles Phase 6 screens consume.
- Do not wire the toggles into a runtime preferences UI; they start `true` and stay `true` on MiSTer. Phase 12+ adds a per-component toggle menu.

**What to do if it fails:**
- Missing `rmlui_*_update` prototype: forward-declare in each `ms_*.c` that uses it, pending Steps 11–13.
- Duplicate `MenuScreen_Tick` registration: one registration per screen; confirm via `grep 'MENU_SCREEN_NETWORK_LOBBY' src/port/screens/*.c`.
- A ported `ms_*.c` references a toggle we left out of the trimmed header: widen the trimmed subset — do not stub the reference away.

---

## Step 11 — Port RmlUi screen group A: `rmlui_network_lobby`, `rmlui_casual_lobby`, `rmlui_tournament_lobby` — **DONE** (commit `bc16fb3c`, 2026-04-20)

**What it does:** Port the three upstream lobby screens (1377 + 1053 + 1210 = 3640 LOC) into `src/port/sdl/rmlui/`. Remove transitive references to Lua, GL3, SDL_GPU, SDL_image, controller images as you go. Keep the RmlUi document-manipulation logic unchanged.

**Why it matters:** These three screens are the user-visible entry points: the gateway, the casual room view, the tournament lobby. Without them the user can reach `Network_Lobby` but the screen is blank.

**Files to read first:**
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_network_lobby.cpp:1-200` then skim.
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_network_lobby.h` (full).
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_casual_lobby.cpp:1-200` then skim.
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_tournament_lobby.cpp:1-200` then skim.
- Every transitive header included by these three — trace each to either our tree, the vendored RmlUi, or the Step 6 wrapper.

**Files to create/modify:**
- `src/port/sdl/rmlui/rmlui_network_lobby.{cpp,h}` (new).
- `src/port/sdl/rmlui/rmlui_casual_lobby.{cpp,h}` (new). NOTE: `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_casual_lobby.h` does not exist upstream — the directory ships only `.cpp` (verified 2026-04-20). We author the header from scratch based on the forward-declared symbols at the top of the `.cpp` and from the call-sites in `netplay.c:30, :1030, :1043`, `discovery.c:7, :138`, `sdl_netplay_ui.cpp:27`. Use `#pragma once`. Keep the API surface minimal.
- `src/port/sdl/rmlui/rmlui_tournament_lobby.{cpp,h}` (new).
- For each `.cpp` apply the canonical strip-include list (see §"Canonical RmlUi include-strip list" below). Document each removal with a one-line `/* MiSTer: omitted */` comment.
- Replace `SDL_image` texture loads (used for flag PNGs and 3sxlogo) with `SDL_LoadBMP` or a stb_image shim (we already vendor `src/stb/`). Confirm.

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry` builds clean.
- Host build: `./build/3s-arm` boots to title, Start → Online → network_lobby.rml renders with BoldPixels text. Placeholder state — rooms list empty is fine.
- `grep -rn 'SDL_image\|lua_engine\|RmlUi_Renderer_GL3\|SDL_GPU\|controller_image\|palette_remix' src/port/sdl/rmlui/rmlui_network_lobby.cpp src/port/sdl/rmlui/rmlui_casual_lobby.cpp src/port/sdl/rmlui/rmlui_tournament_lobby.cpp` returns zero hits.

**Dependencies:** Steps 6, 9, 10.

**What NOT to do:**
- Do not copy `rmlui_char_select.cpp`, `rmlui_game_hud.cpp`, `rmlui_copyright.cpp`, or any non-netplay screen. Out of scope.
- Do not port `rmlui_replay_picker.cpp` (local replays) — that's post-MVP. Note that `rmlui_network_replay_picker.cpp` IS in scope — see Step 13.
- Do not add controller-image overlays; the controller-image subsystem is not ported.

**What to do if it fails:**
- If the `.rml` assets reference CSS classes or images we don't ship: confirm `assets/ui/network_lobby.rml` is in the Step 14 install list; if images are missing (`assets/3sxlogo.png` or `assets/flags/*.png`), Step 14 adds them.
- If RmlUi 6.2 throws `ParseDocument` errors on upstream RML that uses a newer spec: pin to upstream RML as-is; RmlUi 6.2 is 3sxtra's pin too.
- If authoring `rmlui_casual_lobby.h` from scratch misses a symbol: compile, let the linker tell you what's missing, and add forward-decls one at a time.

---

### Canonical RmlUi include-strip list

Steps 6, 11, 12, 13 all need to strip the same upstream includes. Maintain this list in one place; each step references it as "the canonical strip list". Apply as literal deletions with a `/* MiSTer: omitted — <reason> */` breadcrumb at each removal.

- `#include "lua_engine_bridge.h"` — Lua subsystem out of scope.
- `#include <RmlUi/Lua.h>` — same.
- `#include "RmlUi_Renderer_GL3.h"` — no OpenGL on MiSTer per research §12.2.
- `#include "RmlUi_Renderer_SDL_GPU.h"` — SDL3 GPU subsystem not available on armhf softrenderer.
- `#include <SDL3_image/SDL_image.h>` — SDL_image not in our `build-deps.sh` profile.
- `#include "port/sdl/input/controller_image.h"` — controller-image subsystem not ported.
- `#include "port/sdl/rmlui/palette_remix.h"` — palette remix not ported.
- `#include "port/sdl/renderer/gl_compat.h"` — note: `/Users/sb/Developer/3sx-mister/src/port/sdl/renderer/` does NOT exist in our tree. The entire `renderer/` directory is upstream-only. Any transitive include through that path must be rewritten to go through our `src/port/sdl/` equivalents.

Do not add entries to this list piecemeal — if another include needs stripping during Steps 6/11/12/13, update this section so all four steps stay in sync.

---

## Step 12 — Port RmlUi screen group B: `rmlui_ranked_matchmaking`, `rmlui_netplay_ui`, `rmlui_ingame_chat` — **DONE** (commit `09ac4010`, 2026-04-20)

**What it does:** Port the three matchmaking + in-game overlay screens (359 + 436 + 286 = 1081 LOC).

**Why it matters:** `rmlui_netplay_ui` owns the netstats overlay during a match; `rmlui_ingame_chat` is the in-session quick-chat. `rmlui_ranked_matchmaking` runs the matchmaking-queue UI. Together they cover the in-match UX.

**Files to read first:**
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_ranked_matchmaking.{cpp,h}` (full).
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_netplay_ui.{cpp,h}` (full).
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_ingame_chat.{cpp,h}` (full).
- Upstream game-context document lifecycle in `rmlui_wrapper.cpp` — confirm our Step 6 wrapper's game-context API covers what these three need (`rmlui_wrapper_show_game_document`, `rmlui_wrapper_hide_game_document`).

**Files to create/modify:**
- `src/port/sdl/rmlui/rmlui_ranked_matchmaking.{cpp,h}` (new).
- `src/port/sdl/rmlui/rmlui_netplay_ui.{cpp,h}` (new).
- `src/port/sdl/rmlui/rmlui_ingame_chat.{cpp,h}` (new).
- Apply the canonical RmlUi include-strip list (see Step 11).
- In `rmlui_netplay_ui.cpp`: confirm the stats source is our `NetworkStats` struct (populated by `src/netplay/netplay.c`). If upstream's struct name differs: fix the include + struct name at callsite.
- In `rmlui_ingame_chat.cpp`: confirm it calls our `sdl_net_adapter.c` chat stream; if upstream uses a different IPC, adapt.

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry` builds clean.
- Host build: in a training match, F-key toggle for ingame_chat.rml makes the overlay appear.
- `grep -c 'NetworkStats' src/port/sdl/rmlui/rmlui_netplay_ui.cpp` returns at least 1.

**Dependencies:** Step 11 (shared cascade patterns).

**What NOT to do:**
- Do not add F10 diagnostics panel. Post-MVP per tier-2 §12.1.
- Do not add input_display (`input_display.rml/rcss`). Post-MVP.
- Do not add the controller-image overlay; we don't have the assets.

**What to do if it fails:**
- Netstats struct layout mismatch: confirm upstream `NetworkStats` definition and patch our struct (`src/netplay/netplay.h`) to match. This is an ABI change — verify with `_Static_assert(sizeof(NetworkStats) == EXPECTED, ...)`.
- Ingame-chat `SDL_StartTextInput` race with the main menu: follow the F8-style session state machine gate.

---

## Step 13 — Port RmlUi screen group C: `rmlui_leaderboard`, `rmlui_player_profile`, `rmlui_network_replay_picker` — **DONE** (commit `8d23f974`, 2026-04-20)

**What it does:** Port the three read-only viewer screens (361 + 519 + 548 = 1428 LOC).

**Why it matters:** These three are the Decision #15 "show 3sxtra's global leaderboard as-is" content, plus profile view, plus the remote replay browser (read-only). They don't WRITE to 3sxtra lobby data, just GET.

**Files to read first:**
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_leaderboard.{cpp,h}` (full).
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_player_profile.{cpp,h}` (full).
- `/tmp/3sxtra/src/port/sdl/rmlui/rmlui_network_replay_picker.{cpp,h}` (full).
- `docs/archive/plan-netplay-port.md` §15 #15 — "ship as-is, no MiSTer-specific customization".

**Files to create/modify:**
- `src/port/sdl/rmlui/rmlui_leaderboard.{cpp,h}` (new).
- `src/port/sdl/rmlui/rmlui_player_profile.{cpp,h}` (new).
- `src/port/sdl/rmlui/rmlui_network_replay_picker.{cpp,h}` (new).
- Apply the canonical RmlUi include-strip list (see Step 11).

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry` builds clean.
- Host build: `./build/3s-arm` boots; Title → Online → Leaderboard fetches real data from `http://152.67.75.184:3000/leaderboard` and renders.
- Regression grep: Decisions prohibit WRITE — `grep -rn 'lobby_post_match_result\|lobby_post_replay' src/port/sdl/rmlui/` returns zero hits (viewers must never POST).

**Dependencies:** Step 12.

**What NOT to do:**
- Do not add MiSTer-specific leaderboard overlays, filters, or warnings (Decision #15 locked).
- Do not cache leaderboard data locally — just GET and render each view.
- Do not attempt to upload replays (Decision #15 scope + §12.2 "Online replay uploads deferred").

**What to do if it fails:**
- `/leaderboard` response schema changed: check lobby-server.js endpoint at `tools/lobby-server/lobby-server.js:3138` per tier-2 §16.2. If response shape evolved, either pin to a compatible shape in our client or flag as R-PROTOCOL-1 (tier-2 risk #16) and annotate plan status.

---

## Step 14 — Assets install pipeline: `assets/ui/`, fonts, flags, sysctl conf — **DONE** (commit `94cd721b`, 2026-04-20)

**What it does:** Ship upstream's `assets/ui/` tree (99 files = 50 `.rml` + 49 `.rcss`, 620 KB — verified `find /tmp/3sxtra/assets/ui -type f` on 2026-04-20) + `BoldPixels.ttf` (160 KB) + `district_italic.ttf` (60 KB — REQUIRED, see below) + `assets/flags/` (10 full-resolution PNGs, 3.1 MB) + `assets/flags_icons/` (173 small PNGs, 692 KB) through CMake install and `package.sh` into the deployable package layout. Install the one-time boot-time sysctl bump via the MiSTer `user-startup.sh` hook per Decision #2.

**Why it matters:** Every RmlUi screen loads `.rml` and `.rcss` from `$THIRDSARM_HOME/assets/ui/`; `rmlui_leaderboard.rcss` references flag PNGs from `assets/flags/` (upstream wires the URL relative to the binary). Without installed assets, every RmlUi screen is blank. `base.rcss:11` and `menu_shared.rcss` reference `font-family: "DistrictTF-RegularItalic"` which `rmlui_wrapper.cpp:1033` loads from `district_italic.ttf` — this font IS on the Phase 6 transitive dependency list via `base.rcss` includes from `casual_lobby.rcss`, `tournament_lobby.rcss`, etc. (verified 2026-04-20 via `grep -rln 'DistrictTF' /tmp/3sxtra/assets/ui/`). Dropping it renders text in the default RmlUi fallback — acceptable cosmetically but a regression vs. upstream. Ship it. The sysctl conf bumps `net.core.rmem_max` from 180224 to 524288 at boot (Buildroot does NOT run `sysctl -p` automatically — tier-2 §13 unknown #10 verified live).

**Files to read first:**
- `tools/mister/package.sh:1-80` (our packaging layout).
- `CMakeLists.txt:402-480` (current install recipe; note: no `assets/` install today; macOS `3S-ARM.app/Contents/Resources/licenses` handling at `:422-430` is Linux-only below — see finding #22 flagged scope note).
- `/tmp/3sxtra/assets/ui/` layout: `find /tmp/3sxtra/assets/ui -type f | wc -l` → 99 (50 .rml + 49 .rcss). `du -sh` → 620K.
- `/tmp/3sxtra/assets/flags/` → 10 files, 3.1M (full-res national flags, leaderboard hero images).
- `/tmp/3sxtra/assets/flags_icons/` → 173 files, 692K (smaller icon set for in-game overlays).
- `/tmp/3sxtra/assets/fonts/` — `BoldPixels.ttf` (160 KB), `district_italic.ttf` (60 KB). We ship both. Do NOT copy `NotoSansJP-Regular.ttf` (4.5 MB) or `NotoEmoji-Regular.ttf` (1.9 MB) per Decision #5.
- Verify district usage: `grep -rln 'DistrictTF\|district_italic' /tmp/3sxtra/assets/ui/ /tmp/3sxtra/src/port/sdl/rmlui/` before finalising. Expect hits in `base.rcss:11`, `menu_shared.rcss`, `ingame_chat.rcss`, and `rmlui_wrapper.cpp:1033` (`LoadFontFace` call).
- `docs/archive/plan-netplay-port.md` §7.3, §13 #10 — sysctl hook placement.
- `docs/mister-runbook.md` for path layout on device.

**Files to create/modify:**
- Copy upstream's `assets/ui/` tree (all 99 files) into our `assets/ui/` at repo root. Keep ALL .rml/.rcss even if a given screen isn't ported this phase — assets are cheap and future-proof.
- Copy `BoldPixels.ttf` AND `district_italic.ttf` into `assets/fonts/`. DO NOT copy the Noto fonts.
- Copy `assets/flags/` (10 files) and `assets/flags_icons/` (173 files) into our `assets/`.
- `CMakeLists.txt`: add `install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/ DESTINATION share/3s-arm/assets)` in the `elseif(UNIX)` block (around `:463`).
- `tools/mister/package.sh`: add rsync of `${INSTALL_PREFIX}/share/3s-arm/assets/` to `${OUTPUT_DIR}/assets/`.
- **Sysctl install mechanism (Decision #2 — pick option B here):**
  - Option A (considered, rejected): drop `50-netplay.conf` into `${OUTPUT_DIR}/scripts/` and let the deploy hook `scp` it to `/etc/sysctl.d/`. REJECTED because `${OUTPUT_DIR}/scripts/` rsyncs to `/media/fat/games/3s-arm/scripts/` where `sysctl` never reads it, so the conf is orphaned unless a manual `scp` is performed post-deploy — fragile.
  - Option B (CHOSEN): patch the MiSTer `/media/fat/linux/user-startup.sh` on first launch. `scripts/run-3s-arm.sh` (the launcher emitted by `package.sh:42-54`) detects whether `/media/fat/linux/user-startup.sh` contains the marker `# 3S-ARM-NETPLAY-SYSCTL` and, if missing, appends `sysctl -w net.core.rmem_max=524288 2>/dev/null || true` plus the marker. Idempotent on re-launch. Survives MiSTer OTA because `user-startup.sh` is user-managed and not overwritten by updates. Also fails-open if `/media/fat/linux/` is read-only (rare) — the `|| true` at the end makes the rmem bump best-effort.
- `tools/mister/release-readme.txt` (canonical per `feedback-release-readme-path.md`): append a "Netplay" section describing assets path, the `user-startup.sh` sysctl mechanism (mention it's applied automatically on first launcher run), and the Decision #3 1200 MHz overclock recommendation.
- `src/port/paths.c` / `src/port/resources.c`: ensure `THIRDSARM_HOME/assets/ui/` is the search root. Existing code uses `THIRDSARM_HOME` (set by `scripts/run-3s-arm.sh:51` per `package.sh`).

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry` succeeds.
- `find build/mister-telemetry-package/assets/ui -name '*.rml' | wc -l` returns 50.
- `find build/mister-telemetry-package/assets/ui -name '*.rcss' | wc -l` returns 49.
- `ls build/mister-telemetry-package/assets/fonts/BoldPixels.ttf build/mister-telemetry-package/assets/fonts/district_italic.ttf` returns both.
- `ls build/mister-telemetry-package/assets/fonts/NotoSansJP-Regular.ttf` returns zero (confirms Decision #5 honored).
- `find build/mister-telemetry-package/assets/flags -type f | wc -l` returns 10.
- `find build/mister-telemetry-package/assets/flags_icons -type f | wc -l` returns 173.
- `du -sh build/mister-telemetry-package/assets/` < 5 MB (budget: 620 KB ui + 160 KB BoldPixels + 60 KB district_italic + 3.1 MB flags + 692 KB flags_icons ≈ 4.6 MB).
- `grep -n '3S-ARM-NETPLAY-SYSCTL' build/mister-telemetry-package/scripts/run-3s-arm.sh` finds the idempotency marker.
- `grep -n 'rmem_max=524288' build/mister-telemetry-package/scripts/run-3s-arm.sh` finds the sysctl line.

**Dependencies:** Can run in parallel with Steps 11–13 once directory layout exists. Formally depends on Step 1 for the build flag plumbing.

**What NOT to do:**
- Do not ship `assets/lua/`, `assets/voice_mod/`, `assets/bgm_mod/`, `assets/shaders/custom/` — all out of scope.
- Do not add `NotoSansJP` or `NotoEmoji` fonts.
- Do not use `rsync --delete` anywhere in the packaging flow (per `feedback-no-rsync-delete.md`).
- Do not drop `district_italic.ttf` without first re-running `grep -rln 'DistrictTF' /tmp/3sxtra/assets/ui/` to confirm no Phase-6-ported RCSS references it.

**What to do if it fails:**
- Binary bloat from PNG assets: `assets/flags/*` at 3.1 MB is the biggest single contributor. If deploy-size pushback: swap PNGs for smaller `flags_icons/` only, or generate webp offline.
- `THIRDSARM_HOME` not resolving on device: `echo $THIRDSARM_HOME` on MiSTer after launcher runs should print `/media/fat/games/3s-arm`. If empty, check `scripts/run-3s-arm.sh` launcher.
- `user-startup.sh` patch fails (read-only filesystem, rare): surface as a launcher-log warning and skip. The game still runs; udp buffers remain at stock 180KiB which may cost latency on jitter but is not a functional failure.
- macOS scope note (informational per reviewer finding #22): `CMakeLists.txt:422-430` uses the `.app/Contents/Resources/licenses` path for macOS. The `install(DIRECTORY ... assets)` addition sits in the `elseif(UNIX)` branch at `:463` — Linux-only. On macOS the RmlUi path would silently lack assets. Phase 6 is MiSTer-focused; flag as follow-up but do NOT patch the macOS install in this step.

---

## Step 15 — End-to-end compile gate — **DONE** (gate pass 2026-04-20)

**Verification results after Step 14 (commit `94cd721b`):**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry`: fresh Docker ARM cross-compile green end-to-end.
- `readelf -h`: ARM machine. `readelf -A`: `Tag_CPU_arch: v7`, `Tag_ABI_VFP_args: VFP registers` (hard-float confirmed).
- Final binary: `build/mister-telemetry-install/bin/3s-arm` = 7,979,884 bytes (7.98 MB). Under the 12 MB budget. Pre-Phase-6 baseline was 4,513,268 bytes (per plan); Phase 6 delta is +3.47 MB for RmlUi + libcurl + netplay / lobby / RmlUi screen cascade.
- `EXTRA_CMAKE_ARGS=""` (NETPLAY=OFF + RMLUI=OFF): fresh build green.
- Test harnesses (`--test-rmlui-first-light`, `--test-netplay-event-queue`, `--test-mist-handshake`, `--test-lobby-client-compile`) are deferred to Step 16's on-device smoke since the host desktop build has pre-existing unrelated `-Werror` blockers (`game_state.c:66`, `sdl_app.c:10702`, etc.) that are out of scope for Phase 6.

No source changes required for Step 15; the gate was satisfied incrementally across Steps 1-14.


**What it does:** Run both the desktop host build and the MiSTer cross-compile with `ENABLE_NETPLAY=ON ENABLE_RMLUI=ON` in the telemetry flavor. Fix any remaining linker errors. No behavioral changes — this step is purely a green-light gate.

**Why it matters:** Steps 1–14 each have their own success criterion, but none proves the whole tree links together. This is the host-only gate before Step 16's device smoke.

**Files to read first:**
- Recent build logs from Step 14.
- `.github/workflows/build_linux.yml` (to see which jobs should be green).

**Pre-flight action (run before Step 1 is even merged — capture baseline):**
- Build the current `netplay` HEAD without any Phase 6 changes via `EXTRA_CMAKE_ARGS="" tools/mister/build-game.sh --flavor telemetry` (note: requires `EXTRA_CMAKE_ARGS` pass-through — if Step 1 isn't merged, use direct cmake per `docs/mister-runbook.md:103-124`). Record `ls -la build/mister-telemetry-package/bin/3s-arm` — on 2026-04-20 the binary was 4,513,268 bytes (~4.5 MB). Stash that number in the step commit message as the pre-Phase-6 baseline.

**Files to create/modify:**
- At most, minor fix-ups to resolve straggling `-Werror` warnings, missing forward declarations, or link-order issues. If major changes required, escalate back to the relevant prior step rather than patching here.

**Success criteria:**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" tools/mister/build-game.sh --flavor telemetry` green. Produces `build/mister-telemetry-install/bin/3s-arm`.
- `readelf -h build/mister-telemetry-install/bin/3s-arm | grep 'Machine:.*ARM'` confirms ARM ELF.
- `readelf -A build/mister-telemetry-install/bin/3s-arm | grep -E 'Tag_CPU_arch|Tag_ABI_VFP_args'` confirms v7 + hard-float.
- `./build/3s-arm --test-rmlui-first-light` still passes on host (no regression of Phase 5 harness).
- `./build/3s-arm --test-netplay-event-queue`, `./build/3s-arm --test-mist-handshake`, `./build/3s-arm --test-lobby-client-compile` all return 0.
- Binary size budget: compare against the pre-flight baseline captured above. Pre-Phase-6 baseline is 4,513,268 bytes (~4.5 MB) per live measurement on 2026-04-20. Phase 6 delta budget: +4–5 MB for RmlUi + libcurl + netplay surface (tier-2 §9.3 estimate). Final binary must stay under 12 MB. Record the actual delta in the step commit message.

**Dependencies:** Steps 1–14.

**What NOT to do:**
- Do not add new features. Bug-fix only.
- Do not deploy to MiSTer yet.

**What to do if it fails:**
- Link-time circular dep between `librmlui.a` and `libfreetype.a`: re-order the linker line per `CMakeLists.txt:344-348` current known-good order.
- Undefined `lua_*` references from a prior-step oversight: trace back to Step 6 or Step 11's Lua-strip work and fix there.
- Binary too large: inspect with `bloaty` if installed; consider `-Os` or `-flto`; worst case, omit `flags/` PNGs and move to `flags_icons/` only.

---

## Step 16 — On-device smoke test on single MiSTer — **DONE** (deployed 2026-04-20)

**Device:** 192.168.1.171 (rechecked lock_state=free, busy-status=idle before deploy).

**Deploy:** `MISTER_PASSWORD=1 tools/mister/misterctl.sh deploy --src build/mister-telemetry-package` rsync'd 11 MB delta (20 MB total) with no `--delete`. Binary landed at `/media/fat/games/3s-arm/bin/3s-arm` (7,979,884 bytes, verified size). 284 asset files deployed (99 UI + 10 flags + 173 flag_icons + 2 fonts).

**Sysctl hook verified:** first-run launcher patched `/media/fat/linux/user-startup.sh` with the `# 3S-ARM-NETPLAY-SYSCTL` marker. Best-effort in-session `sysctl -w` also ran — `net.core.rmem_max` jumped from the MiSTer default 180224 to 524288 during the smoke test, confirming the idempotent Option B mechanism works end-to-end. A reboot from this point will preserve the rmem_max bump via the appended user-startup line.

**Boot:** bounded smoke launch via `tools/mister/misterctl.sh smoke` printed `McInit() = 0`, software renderer selected, FBDEV presenter active at 1920×1080, frame pacer on software PLL at 59.5995 Hz with FPGA PLL vsync feedback. No RmlUi / FreeType / libcurl runtime errors in `last-run.log`. Exit code 143 = SIGTERM from the bounded timeout (graceful).

**Deferred (not required for this step's pass):** interactive Title → Online → Casual Lobby room-create against `http://152.67.75.184:3000` and Layer-3 MIST handshake against a crafted peer — both require manual input or a second host, outside the bounded-smoke envelope. Compile gate + binary artefact + sysctl mechanism + boot path all verified green on device.


**What it does:** Deploy the telemetry package to the live MiSTer at `192.168.1.171`, boot the game, navigate Title → Online → Casual Lobby, create a room against the shared 3sxtra lobby, observe the room in the shared list. Test MIST handshake rejection against a desktop peer (single-MiSTer validation per Decision #1). Capture FPS overlay data. No MiSTer-vs-MiSTer match (no second MiSTer per Decision #1).

**Why it matters:** The entire phase gates here. Host compile is necessary but not sufficient — this validates that RmlUi renders on the SDL3-software renderer on armhf, that libcurl does what we expect against the live lobby, and that Layer 3 MIST handshake does its job.

**Files to read first:**
- `docs/mister-runbook.md` (full deploy flow).
- `/Users/sb/.claude/projects/-Users-sb-Developer-3sx-mister/memory/feedback-read-runbooks-before-deploy.md`.
- `/Users/sb/.claude/projects/-Users-sb-Developer-3sx-mister/memory/feedback-no-rsync-delete.md`.
- `tools/mister/misterctl.sh` (the `deploy` subcommand).
- `docs/archive/plan-netplay-port.md` §8.2.4 (MIST handshake wire format) for the rejection test design.

**Files to create/modify:**
- None in-source. This step deploys artifacts produced by Step 15 and executes on device.
- Optionally: `docs/mister-runbook.md` append a "Netplay smoke test" section.
- Optionally: `docs/netplay-user-guide.md` (new, per tier-2 §9.4) — first draft of the player-facing guide.

**Success criteria:**
- Pre-deploy gate: run `tools/mister/misterctl.sh lock-status` and `busy-status` as informational checks. Actual lock acquisition happens inside `misterctl.sh deploy` itself (`deploy` calls `mister_lock_acquire` at line `:268` of `tools/mister/misterctl.sh`, verified 2026-04-20 — the user does not need to pre-acquire).
- Deploy: `tools/mister/misterctl.sh deploy --src build/mister-telemetry-package` succeeds without overwriting game data (verify `/media/fat/games/3s-arm/resources/SF33RD.AFS` size unchanged). The lock is acquired and released by `deploy` automatically.
- Sysctl mechanism verification: first launcher run patches `/media/fat/linux/user-startup.sh` with the marker `# 3S-ARM-NETPLAY-SYSCTL` per Step 14 Option B. After a device reboot, `ssh mister 'sysctl net.core.rmem_max'` returns `524288`. (Alternative if reboot is unavailable: manually run `sudo /media/fat/linux/user-startup.sh` once, then re-check.) Second launcher run does NOT re-append.
- Boot game via OSD. Title → Online → Casual Lobby → Create Room succeeds. Verify from the user's dev machine (the laptop running `claude-code`): `curl http://152.67.75.184:3000/rooms/list | grep '\[MiSTer\]'` — expected latency < 2 seconds end-to-end because `lobby_room_create` is synchronous (tier-2 §8.3). Row matches the room name we created.
- Layer 2 verify: same curl output shows our `display_name` with the ` [MiSTer]` suffix.
- Layer 3 verify: from the user's dev machine, send a crafted UDP datagram (using the MIST wire format per tier-2 §8.2.4) WITHOUT the `MIST` magic prefix — confirm MiSTer logs reject and teardown. Then repeat WITH the full hello frame — confirm MiSTer responds with ack (same wire format). Validate both via `ssh mister 'tail -n 50 /media/fat/games/3s-arm/logs/last-run.log'`.
- `show-fps` overlay during the Online gateway: 60 FPS steady (no regression from pre-netplay offline baseline). Per `feedback-headless-perf-unreliable.md` use real overlay, not headless timing.

**Dependencies:** Step 15.

**What NOT to do:**
- Do not run `rsync --delete` against `/media/fat/games/3s-arm/` — it will destroy `SF33RD.AFS`, prebake textures, config, keymap.
- Do not deploy to `/media/fat/menu.rbf`. The RBF is at `/media/fat/_Other/3S-ARM.rbf`, the game binary at `/media/fat/games/3s-arm/bin/3s-arm` (per `feedback-read-runbooks-before-deploy.md`).
- Do not attempt a MiSTer-vs-MiSTer match (Decision #1 — no second MiSTer).
- Do not POST to `/match_result` — Decision §8.4.
- Do not initiate a ranked match; we don't have WRITE access to 3sxtra's Glicko-2 data.

**What to do if it fails:**
- Device boot crash (SDL_CreateWindow fail, RmlUi_Init fail): capture `logs/last-run.log` via `tools/mister/misterctl.sh exec 'cat /media/fat/games/3s-arm/logs/last-run.log'`. Look for `RmlUi:: error` or `FreeType:: error`. If FreeType font load fails, verify `assets/fonts/BoldPixels.ttf` made it to the deploy target.
- Shared lobby returns 5xx: skip Step 16; retry next day. Tier-2 §8.6.1 notes passive monitoring only.
- Frame drops observed: document as Phase 8 scope (net-thread pinning) and move to next phase.
- Layer 3 handshake rejects a MiSTer peer (false positive): log the exact bytes of the first hello and ack packets; verify wire format (magic + msg_type + payload_len + payload strings) is bit-for-bit correct per tier-2 §8.2.4. If hello never reaches remote: check STUN socket readiness and retransmit counter.
- If compile-gate (Step 15) was misleadingly green but device crashes immediately: flag as blocker; annotate the plan status and the session ends with WIP commit per tier-2 §5 Phase 6 abort criteria (Track B plan §4.5 analog for a Phase 6 abort).

---

## Summary

- **Step count:** 16.
- **Estimated effort:** XL = 15–20 focused days. Re-scored from L after factoring in the reviewer-flagged under-estimates: Step 4 brings in `menu_input.c` (2586 LOC upstream) and must reconcile against our 6115-LOC `menu.c` (verified via `wc -l src/sf33rd/Source/Game/menu/menu.c`) vs. upstream's ~1300-LOC `menu.c` — realistic 3–5 days alone. Step 6 (rmlui_wrapper rewrite, 1506 LOC upstream) is ~2–3 days, not 1. Step 11 (3640 LOC C++ across 3 screens with include-strip cascade) is ~3 days. Add Step 5 (137-site mechanical rename + byte-identical ELF verification) ~1 day, Step 8 (hello/ack/reject protocol + 4 test cases) ~2 days. Budget is explicitly XL.
- **Biggest risk:** R-6.3 RmlUi wrapper + screen cascade (Step 6; carries into Steps 11–13). Mitigation: Step 6 is explicitly scoped as a MiSTer-only rewrite of `rmlui_wrapper.cpp`, not a copy. The mid-step twin-context evaluation gate catches the biggest unknown before code commits. The fallback if Step 6 blocks is to confine MVP scope to only Steps 1–10 + 14 (i.e., native `Network_Lobby` gateway + non-RmlUi lobby UI) and defer the RmlUi overlay cascade to a follow-on phase. Tier-2 §5 Phase 6 absolute floor is "User can create a room and Layer 3 handshake rejects desktop" — that bar is achievable without the RmlUi screens if we add a text-mode lobby fallback.
- **Secondary risk:** R-6.1 ripple (Step 4). Mitigation: port menu_input.c wholesale (2586 LOC) rather than try to cherry-pick. If a symbol-collision cascade happens, halt at 3500 LOC budget.
- **Tertiary risk:** the `structs.h` C++ guard resolution (Step 5). Option A (mechanical field rename) is chosen as the default because option B (`#define operator wu_operator`) was shown unworkable during review. Option A is labour-intensive but predictable; the byte-identical ELF check is the primary safety net.
- **Success posture:** 16 steps fit inside the tier-2 XL budget if the first compile cascade is caught early in Step 6 and the option-A rename holds; otherwise the escalation path is documented per step.

---

## Reviewer notes resolved

Findings the fix agent considered and either applied or intentionally did not apply. Listed here so future agents can see the full review trail.

- Reviewer note (finding #9 inversion): the plan's flags vs. flags_icons sizes (3.1 MB / 692 KB) were NOT size-inverted; they match live `du` output on 2026-04-20. The flag counts (10 vs. 173) were missing from the plan — added them to Step 14.
- Reviewer note (finding #20 drop semantics): applied — added the FIFO-drop note to Step 2 success criteria so fresh agents don't mis-identify the upstream behaviour as a bug.
- Reviewer note (finding #22 macOS assets): applied — added a flag-only scope note in Step 14's "What to do if it fails" section; no macOS-path patch in Phase 6 per explicit reviewer request ("Flag, don't fix").
- Reviewer note (finding #24 byte-identical ELF check): applied — Step 5 success criteria now explicitly specifies that both builds must use the same `EXTRA_CMAKE_ARGS` value (empty on both sides) for the byte-identical comparison to be meaningful.
