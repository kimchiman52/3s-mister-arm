# 3sxtra Netplay Port — Tier-2 Implementation Plan

## 1. Document metadata and purpose

**Tier:** 2 (architectural roadmap / workstream plan).
**Status:** Draft. Planning only — no code, no commits, no builds triggered by this document.
**Date:** 2026-04-20.
**Authors:** Planning agent on branch `mister` HEAD `17ab61e7` (verified via `git status` prelude).
**Supersedes:** None. This is the first comprehensive plan for the netplay port.
**Paired with:** `docs/research-3sxtra-netplay-port.md` (tier-1 research doc, 1565 lines as of 2026-04-20 post-§9.7 addition, authoritative for facts).

**How to use this document:**

- Use §5 (Phase-by-phase roadmap) as the work breakdown. Each phase block has a "Workstream" tag pointing to §4.
- When a phase is ready to start, branch a tier-3 detailed plan that covers exact file-level edits, exact tests, exact commit plan. This document stops one level above that.
- Every factual claim is either cited with `file:line` / URL / on-device command output, or tagged `UNVERIFIED`. Do not promote an UNVERIFIED claim to action without re-verifying.
- §10 (Risk register) and §15 (Open questions) must be re-scanned before starting each phase — some risks are phase-specific, and an open question blocking a later phase can be answered opportunistically during an earlier one.
- Locked strategic decisions (§3) are the frame; do not reopen them inside tier-3 work without surfacing to the user first.

**Verification protocol used while authoring (not claimed, but enforced):**

- Platform claims re-verified live via `ssh root@192.168.1.171` on 2026-04-20; outputs recorded below. Research doc §15 was corrected on one point (Buildroot vs Debian).
- Research doc file:line citations were spot-checked (not exhaustively re-read) for the load-bearing items: `setup_vs_mode` lines, checksum gating, menu directory contents, lobby server table schema, RmlUi SDL renderer blend-mode line, netplay source tree sizing.
- Where verification was impossible without running a cross-build or standing up a VPS, items are tagged `UNVERIFIED` and listed in §13 (Unknowns).

---

## 2. Executive summary

1. **Scope: full feature matrix (client-side).** LAN, direct-IP, STUN hole-punching, UPnP fallback, casual rooms, in-game chat, player profiles — all MiSTer-only via Layers 1/2/3. Ranked write-backs, tournaments hosting, online replay uploads are deferred (would pollute 3sxtra's shared lobby; see §12.2). Spectator mode deferred to post-MVP Phase 13 per §15 #6. Lobby infrastructure: we integrate with 3sxtra's existing production lobby at `152.67.75.184:3000` (see §3.4, §8). No self-hosted infra for MVP.
2. **Approach: preserve + additively patch.** Locked Decision 1 (§3.1): keep our fork's existing 3127-LOC netplay tree, patch it toward parity with 3sxtra rather than replace. Verified: `ls /Users/sb/Developer/3sx-mister/src/netplay/` returned 10 files; `src/port/sdl/netplay_screen.{c,h}` extant.
3. **Engine-refactor scope is minimized.** Locked Decision 2 (§3.2): port only `select_timer.{c,h}` (183 LOC total, `wc -l /tmp/3sxtra/src/sf33rd/Source/Game/select_timer.{c,h}`), `menu_network.c` (1764 LOC, `wc -l`), and the `ms_*` netplay-screen glue. No `globals/`, `training/`, `stage/bg_*` refactors.
4. **Directory layout mirrors 3sxtra.** Locked Decision 3 (§3.3): new subtrees at `src/port/sdl/rmlui/`, new files under `src/netplay/`, new screens under `src/port/screens/ms_*`, new header at `src/include/port/menu_screen.h`.
5. **Four parallel workstreams** (§4): Game-side engine port, Game-side UI (RmlUi), Lobby server + infra, Build/test/deploy/docs. Critical path runs through workstreams 1 and 2; workstreams 3 and 4 can start day 1 and finish independently.
6. **Platform reality: Buildroot 2021.02.4 / glibc 2.31** (verified 2026-04-20 via `/etc/os-release` on 192.168.1.171). This corrects research doc §15 which stated "armhf glibc (Debian/Ubuntu armhf)". All toolchain assumptions must target Buildroot sysroot compatibility, not Debian.
7. **Platform shared libs present on target:** `/usr/lib/libcurl.so.4.7.0`, `/usr/lib/libfreetype.so.6.17.4`, `/usr/lib/libssl.so.1.1`, `/usr/lib/libSDL2-2.0.so.0.14.0`, `/lib/libstdc++.so.6.0.28` (verified live 2026-04-20). GPU libs (libGL*, libEGL*, libvulkan*, libdrm*, libgbm*) absent, confirming research doc §12.2.
8. **Platform nets: `net.core.rmem_max = 180224`** (~176 KB) verified live; `net_tuning.h` (see `wc -l /tmp/3sxtra/src/netplay/net_tuning.h` = 77) attempts to raise socket recv buffer. Either raise the kernel cap at boot (sysctl) or accept the default — decision in §7.
9. **Missing utilities on target:** `getent`, `pgrep`, `iperf3`, `nft` — none of them load-bearing for the game binary itself, but this affects on-device verification scripts (§22 of research doc needs tweaks) and diagnostics tooling.
10. **Timeline (§14): MVP game-side: 10–14 calendar weeks part-time.** Lobby infra ready in parallel in 2–3 calendar weeks once phase 6 starts. Full tournament/ranked/leaderboard feature matrix: an additional 4–8 weeks beyond MVP core.

---

## 3. Locked decisions and rationale

These four decisions were locked by the user in the task brief. They constrain all phase work. If re-opening one, stop and raise it; do not silently drift.

### 3.1 Decision 1 — Preserve existing netplay + additively patch

**Locked:** YES. Do not wipe `src/netplay/` and re-import 3sxtra's tree.

**Tradeoff captured in research doc:**
- Research doc §2.4: "3sxtra is **not** a rewrite of netplay. Both inherit from the same upstream ancestor; 3sxtra evolved additively, our fork evolved toward MiSTer. The `GameState` enumeration preserves identical ancestral ordering and per-module comment structure across both sides."
- Research doc §5.1: our `GameState` has 568 fields, 3sxtra has 601. Delta is 33 additive fields, all of which (except `select_timer_state`) already exist as globals in our engine and just need `GS_SAVE`/`GS_LOAD` plumbing added (research doc §5.3).
- Research doc §19 risk 1 and 2: a verbatim 3sxtra import causes divergence in `combo_type`/`remake_power` (moved into PLW there but kept as globals here) and `WORK.operator` rename vs our `WORK.operator` unchanged; these create *silent* desync or false-positive checksum divergence.

**Upshot:** an additive patch path is strictly less risky than wipe-and-replace because the "wipe" part deletes our working MiSTer-aware pointer-sanitization (`clean_plw_pointers` at `netplay.c:248-285` with `cb`/`rp` zeroing research doc §8.3) which 3sxtra does not have.

**Where this bites:** phases 1 (§5.1), 3 (§5.3), 6 (§5.6). Any PR that deletes more from `src/netplay/` than it adds needs explicit justification.

### 3.2 Decision 2 — Port only what netplay needs from 3sxtra's engine refactors

**Locked:** YES. Do NOT port `src/sf33rd/Source/Game/globals/`, `training/`, `stage/bg_*`, `opening/`, `io/file_loader`, `debug/font_test`, or `game_globals.c`.

**Tradeoff captured in research doc:**
- Research doc §9.1 lists 62 3sxtra-only files. Porting all is weeks of diff reconciliation with zero netplay benefit.
- Research doc §5.2: `select_timer_state` is the ONLY field out of the 33 GameState additions that requires a *new module* to be ported. Everything else is zeroing plumbing.
- Research doc §19 risk 4: if we add `select_timer_state` to GameState but don't port the module, the field is inert, peers diverge on the real `Select_Timer` CPS3 global. So the module itself has to come with.

**The minimal "engine-adjacent" surface we DO port:**
- `src/sf33rd/Source/Game/select_timer.c` (152 LOC — verified `wc -l`)
- `src/sf33rd/Source/Game/select_timer.h` (31 LOC — verified `wc -l`)
- `src/sf33rd/Source/Game/menu/menu_network.c` (1764 LOC — verified `wc -l`)
- `src/sf33rd/Source/Game/menu/menu_network.h` (exists; `wc` not run, flagged for phase 6)
- `src/sf33rd/Source/Game/menu/menu_network_constants.h` (exists, verified via `ls`)
- The `ms_*` screen glue: `ms_casual_lobby.c` (121), `ms_tournament_lobby.c` (102), `ms_ranked_matchmaking.c` (298), `ms_network_lobby.c` (206), `ms_leaderboard.c` (218), `ms_player_profile.c` (137), `ms_replay.c` (267), `ms_save_replay.c` (221), `ms_ranking.c` (69) — all verified via `wc -l`. Total: **1639 LOC** of new C in `src/port/screens/`.
- `src/port/menu_screen_registry.c` (408), `src/port/menu_screen_helpers.c` (236), `src/port/menu_bridge.c` (289), `src/port/menu_task.c` (69) — totaling **1002 LOC** of new C in `src/port/`. Verified via `wc -l`.
- `src/include/port/menu_screen.h` (393 LOC — verified `wc -l`).

**Total minimal 3sxtra engine-adjacent surface: ~4800 LOC** of new C plus the `ms_*` glue. Not trivial, but an order of magnitude smaller than importing all 62 files.

**Where this bites:** phase 2 (`setup_vs_mode`) — the port of `MenuTask_SetPhase(MTP_NETPLAY_IDLE)` to our `task[TASK_MENU].r_no[0] = 5` (research doc §6.3) must be done by hand because we are NOT bringing in the menu-task phase system. Our existing one-line equivalent (verified at our `src/netplay/netplay.c:118`: `task[TASK_MENU].r_no[0] = 5`) is already correct; just extend it.

### 3.3 Decision 3 — Mirror 3sxtra directory paths exactly

**Locked:** YES. New files go to the same relative paths 3sxtra uses.

**Rationale:** reduces future-merge friction. When porting a new feature or pulling a bugfix from 3sxtra, `git cherry-pick` stands a chance only if paths match. Mixed layouts (like our historical divergence in `src/sf33rd/Source/Game/menu/` — verified 3 files vs 3sxtra's 23) are the biggest source of merge-conflict noise.

**Specific path commitments:**
- `src/netplay/stun.c`, `src/netplay/stun.h` — new, mirror 3sxtra.
- `src/netplay/discovery.c`, `src/netplay/discovery.h` — new, mirror.
- `src/netplay/upnp.c`, `src/netplay/upnp.h` — new, mirror (optional, gated by `HAVE_UPNP`).
- `src/netplay/lobby_server.c`, `src/netplay/lobby_server.h` — new, mirror (gated by `ENABLE_NETPLAY_LOBBY`).
- `src/netplay/bracket.c`, `src/netplay/bracket.h` — new, mirror (gated by `ENABLE_NETPLAY_LOBBY`).
- `src/netplay/identity.c`, `src/netplay/identity.h` — new, mirror.
- `src/netplay/ping_probe.c`, `src/netplay/ping_probe.h` — new, mirror.
- `src/netplay/net_detect.c`, `src/netplay/net_detect.h` — new, mirror.
- `src/netplay/sha256.c`, `src/netplay/sha256.h` — new, mirror.
- `src/netplay/net_tuning.h` — new, mirror.
- `src/port/sdl/rmlui/` — entire subtree new.
- `src/port/sdl/netplay/sdl_netplay_ui.{cpp,h}` — new, mirror.
- `src/port/sdl/netstats_renderer.{c,h}` — already exists at our side (verified `ls /Users/sb/Developer/3sx-mister/src/port/sdl/` shows `netstats_renderer.{c,h}`). Merge-in rather than replace.
- `src/port/screens/ms_*.c` — new.
- `src/port/menu_screen_registry.c`, `src/port/menu_screen_helpers.c`, `src/port/menu_bridge.c`, `src/port/menu_task.c` — new.
- `src/include/port/menu_screen.h` — new.
- `assets/ui/*.{rml,rcss}` — new, mirror.
- `tools/lobby-server/` — new, mirror 3sxtra.

**Exception:** if a file name collides with an existing MiSTer-specific file (none found during verification), flag in tier-3 plan.

### 3.4 Decision 4 — Full feature matrix via shared 3sxtra lobby + client-side arch filtering (REVISED 2026-04-20)

**Locked:** YES, with strategic revision.

**Original intent (deprecated):** Host our own Node.js lobby on Hetzner CX11 / equivalent. Parallel infrastructure workstream with VPS + domain + TLS + SQLite backups + moderation tooling.

**Revised direction:** Use **3sxtra's existing production lobby server** at `http://152.67.75.184:3000` (Oracle Cloud hosted, live + healthy per 2026-04-20 probe). Port the `lobby_server.c` client into our fork unchanged; point our client at their URL via `CFG_KEY_LOBBY_SERVER_URL`. Restrict matchmaking to MiSTer-only peers via three client-side defenses (§8 rewrite).

**Why the revision:**
- 32-bit vs 64-bit crossplay is still blocked, but **the reason originally given here is superseded and no longer true.** This bullet used to read: "`SessionHealthMsg.checksum` hashes `sizeof(PLW)` raw bytes, which differs between ILP32 and LP64 due to embedded pointer fields → cross-arch matches die within seconds via `GekkoDesyncDetected`." Task #111 (2026-08-29) removed that cause: the checksum no longer hashes raw `PLW` bytes but a canonical member image emitted by `GameState_EmitPlwCanonical` (`src/netplay/plw_canon.c`), whose size is a fixed 885 bytes on every architecture. Measured, not assumed — `tools/netplay/plw_canon_crossarch.c` built against the production emitter reports the SAME `CANON_CHECKSUM=0x113128fd` on macOS arm64 (`pointer_width=8 sizeof(PLW)=1304`, tiling `885 + 392 + 27`) and on the MiSTer's armv7l (`pointer_width=4 sizeof(PLW)=1092`, tiling `885 + 196 + 11`), both runs 2026-08-29.
- What still blocks cross-arch is a DIFFERENT thing, and it is not the PLW checksum: `GameState` itself remains architecture-dependent, because `EffectState.frw` is declared `uintptr_t frw[EFFECT_MAX][448]` (`src/netplay/game_state.h:24`) and so is 4 bytes per element on ILP32 and 8 on LP64. That flows straight into `state_ver` (`sizeof(GameState)`, 17772 on armv7), which the MIST handshake rejects on. Re-argue cross-arch against `frw`'s sizing, not against the PLW hash.
- Cross-play between our MiSTer fork (32-bit armv7) and 3sxtra's shipping builds (all 64-bit: Win x86-64, Linux x86-64/aarch64, macOS universal, Pi4 aarch64) CANNOT work without significant upstream engineering on 3sxtra's side to architecture-neutralize the simulation.
- Hosting our own lobby for a small MiSTer-only cohort is disproportionate infra cost when 3sxtra's server is already running and stable.
- 32-bit Android builds (`armeabi-v7a`) exist in 3sxtra's fat APK but the author hasn't actually validated they crossplay with 64-bit builds (verified research). So there is no live 32-bit vs 64-bit precedent to stand on.

**Implication:**
- Infrastructure workstream shrinks dramatically — client-only work.
- Port `lobby_server.c` (2229 LOC) client unchanged.
- Layer 2 presence tag rides on `display_name` suffix `" [MiSTer]"` (verified 2026-04-20: lobby-server.js drops extra JSON fields at `/presence` and `/searching/start`; see §8.2 / §8.2.5).
- Filter `/rooms/list` client-side by `[MiSTer]` name prefix.
- Implement `MIST` magic-prefix handshake on GekkoNet socket — authoritative rejection before rollback starts.
- **Do NOT POST to `/match_result` or `/match_result/replay`** on 3sxtra's server — avoids polluting their Glicko-2 leaderboard + replay storage.
- Eventually: work with 3sxtra author toward full crossplay (requires arch-neutral serialization upstream, months of engineering on their side). Deferred indefinitely.

**Cost and ops implication:** near-zero. No VPS, no domain, no TLS, no SQLite backups, no moderation burden on our side. Ops risk: 3sxtra's server could go down or author could ask us to leave. Contingency: self-host as Plan B (the Hetzner / Oracle Free Tier recipe we'd have used originally is retained in §8.x archive for that scenario).

**Where this bites:**
- Phase 10 changes from "Deploy lobby server on VPS" to "Integrate with 3sxtra lobby + arch filter + handshake reject".
- Phase 12 (feature matrix) must explicitly disable result-reporting and replay-upload endpoints to avoid polluting 3sxtra's data.
- Out-of-band coordination with 3sxtra author is NOT required per §15 #9 — the author already knows our intent; proceed without explicit OK. If they later object, Plan B (§8.7) activates.

---

## 4. Workstream map

Four tracks, designed to be run in parallel after an initial shared phase 1. Dependencies flow left-to-right; parallel lanes on vertical axis.

### 4.1 Tracks

| Track | Scope | Owner profile | Peak parallelism |
|---|---|---|---|
| **WS-Engine** (Game-side engine port) | `src/netplay/*`, `src/sf33rd/Source/Game/select_timer.*`, `src/sf33rd/Source/Game/menu/menu_network.*`, GameState field backfill, `setup_vs_mode` expansion, focused checksum, `sanitize_plw_pointers` refinement, main-loop integration, net-thread pinning, ARM cross-build gating | C systems/game-engine | Mostly serial (desync risk); 1 person |
| **WS-UI** (Game-side UI via RmlUi) | `src/port/sdl/rmlui/`, `assets/ui/`, `src/port/screens/ms_*`, `src/port/menu_screen_*`, `src/include/port/menu_screen.h`, blend-mode subclass, RmlUi + FreeType cross-compile | C++/UI/SDL | Can parallel WS-Engine once ABI settled |
| **WS-Lobby** (Shared-lobby client integration — REVISED) | Port `src/netplay/lobby_server.c`, `identity.c`, `sha256.c` unchanged from 3sxtra. Implement Layers 1/2/3 MiSTer-only filtering (§8.2). Disable `/match_result` + `/match_result/replay` writes. Coordinate with 3sxtra author. **No VPS/infra work under shared-lobby direction.** Self-host contingency recipe archived in §8.7 if activated. | C networking / protocol | Largely parallel with WS-Engine once ABI settled |
| **WS-Ops** (Build, test, deploy, docs) | `build-deps.sh` recipes for RmlUi/FreeType/miniupnpc/cJSON, `CMakeLists.txt` flag wiring, CI updates, on-device verification scripts, release notes, `docs/mister-runbook.md` updates, memory-file updates | Build/release engineer | Continuous throughout |

### 4.2 Dependency edges

Previous revisions attempted a matrix but its caption ("Row = depends-on, Column = feeds-into") combined two opposite semantics and the Phase 9 row encoded dependencies in the wrong direction. Replaced 2026-04-20 with an unambiguous adjacency list. Each line: "Phase X feeds into: <list>" means phase X must be complete before the listed phases can proceed.

- **Phase 1 (GameState backfill + select_timer)** feeds into: 2, 3, 6, 7 (in that it finalizes the State layout that Phase 7's cross-compile validates), 9, 10, 11, 12.
- **Phase 2 (`setup_vs_mode` expansion)** feeds into: 9, 10, 11, 12.
- **Phase 3 (focused checksum + sanitizers)** feeds into: 9, 10, 11, 12.
- **Phase 4 (RmlUi + FreeType cross-compile)** feeds into: 5, 6.
- **Phase 5 (blend-mode subclass)** feeds into: 6.
- **Phase 6 (menu_network + ms_* + 9 RmlUi screens)** feeds into: 10 (lobby UI), 12 (feature matrix UI). Not required for Phase 9 LAN smoke (native path suffices per §4.3).
- **Phase 7 (GekkoNet/SDL3_net ARM cross-compile)** feeds into: 8, 9, 10, 11, 12.
- **Phase 8 (net thread pinning, scheduling)** feeds into: 9, 10, 11, 12.
- **Phase 9 (LAN on two MiSTers)** feeds into: 12 only, and only as an early desync-free confidence signal. Conditional per §15 #1 (runs only if a second MiSTer is available).
- **Phase 10 (STUN internet + 3sxtra lobby integration)** feeds into: 11, 12.
- **Phase 11 (UPnP)** feeds into: 12 (UPnP is an enhancement, not a gate; omit if deferred).
- **Phase 12 (lobby feature matrix)** is a sink; nothing downstream in MVP.

### 4.3 Parallel starts

- **Day 1 can start in parallel:** Phase 1 (WS-Engine), Phase 4 (WS-UI), Phase 7 (WS-Ops recipe movement). (Former parallel "VPS+TLS provisioning" lane removed under the shared-lobby direction; see §3.4.)
- **Dominant critical path:** Phase 1 → Phase 2 → Phase 3 → Phase 9 → Phase 10.
- **RmlUi path critical inside UI track:** Phase 4 → Phase 5 → Phase 6.
- Phase 6 is the largest single phase (10–15 days per research doc §20); it gates Phase 9 feature-completeness but NOT the basic LAN smoke test (which only needs phases 1–3 + 7 + 8).

Critical path analysis is repeated in §6.

---

## 5. Phase-by-phase roadmap

Each phase block uses this structure:

```
### Phase N — One-line pitch

Goal — why we’re doing this phase.
Deliverables — concrete artefacts.
Prerequisites — which phases/conditions must hold.
Validation — measurable pass/fail criteria.
Sub-tasks — enough to seed a tier-3 plan.
Risks — phase-local, rank → main risk register in §10.
Effort — S/M/L/XL + rough day count.
Workstream — WS-Engine / WS-UI / WS-Lobby / WS-Ops.
```

### Phase 1 — Backfill GameState and port `select_timer`

**STATUS (as of 2026-04-20): MERGED to `netplay` branch.** See merge commits:
- Track A (Phases 1-3): `417609b4`
- Track B (Phases 4-5): `b82802db`
- Track C (Phase 7): `a46073a3`

Individual phase commits: `03093eb7` (Phase 1 — backfill GameState, port select_timer, add tripwires) plus review-pass fix commits (`00d676a4`, `7db3ffa1`, `1ba7df25`, `270cc567`, `f7b1a5ba`).

**Pitch:** Fill in the 33 missing rollback fields and ship the `select_timer` module so rollback has a consistent view of character-select BCD timers. No behavior change yet; the data path simply exists.

**Goal:** Align `src/netplay/game_state.{c,h}` with 3sxtra's GameState surface without touching behavior. Add `_Static_assert` tripwires for layout safety.

**Deliverables:**
- `src/netplay/game_state.h` — +33 field declarations (research doc §5.3 table).
- `src/netplay/game_state.c` — +33 `GS_SAVE(...)` / `GS_LOAD(...)` lines.
- New `src/sf33rd/Source/Game/select_timer.c` (152 LOC — verified), `select_timer.h` (31 LOC — verified) copied from 3sxtra.
- Call-site wiring for `select_timer` in `sc_timer.c`, `game.c`, `menu.c`, `sel_pl.c`, `next_cpu.c` (files named in `Grep select_timer` output over `/tmp/3sxtra/src/sf33rd`).
- `_Static_assert(sizeof(GameState) == <computed>, ...)` and `_Static_assert(sizeof(_TASK) == 20, ...)` at top of `game_state.c`. Value computed after the 33 adds; recompute and commit once.
- Move the inline `EffectState` typedef from `src/netplay/netplay.c:42-50` (verified inline currently) to `src/netplay/game_state.h` so save/load can reference it cleanly.

**Prerequisites:**
- Repository currently builds (`tools/mister/build-game.sh --flavor telemetry` green on HEAD `17ab61e7`, confirmed via project status) — precondition, not a deliverable.

**Validation:**
- Desktop build (`cmake --preset desktop` or equivalent) succeeds with `ENABLE_NETPLAY=ON`.
- `tests/unit/` existing netplay tests (research doc §3.6 lists 11+ targets) still pass.
- A GameState struct-size round-trip unit test (new): serialize→deserialize→memcmp equals original.
- Static-assert does not fire at compile time.
- On-device (desktop LAN match against another desktop peer): 300-frame session with zero desync events, same behavior as HEAD `17ab61e7`.

**Sub-tasks:**
1. Copy `select_timer.{c,h}` into `src/sf33rd/Source/Game/`; merge call-site changes in `sc_timer.c` / `game.c` / `menu.c` / `sel_pl.c` / `next_cpu.c`. Read diff of each source file before patching.
2. Extend `GameState` struct with the 33 fields in the same ancestral order as 3sxtra uses.
3. Add matching `GS_SAVE`/`GS_LOAD`. Order must match declaration order for consistency (not functionally required but reduces review noise).
4. Add `_Static_assert` with computed sizes. Compute on desktop build first; if the sizes match 3sxtra's 17800/19376 (research doc §5.2), use those values verbatim to catch future drift.
5. Move `EffectState` typedef from `netplay.c:42-50` to `game_state.h`.

**Risks (local → §10):**
- R-1.1 (low): a new field's type differs between 3sxtra and our engine at binary level (e.g., `u32` vs `s32` on same-name global). Audit each of the 33 against our engine's actual declaration.
- R-1.2 (medium): `SelectTimerState` struct definition inside 3sxtra's `select_timer.h` references engine types we don't have. Verify during sub-task 1.

**Effort:** S–M (3–5 days) per research doc §20. Upgrade to M if `select_timer.h` pulls unexpected engine includes.

**Workstream:** WS-Engine.

---

### Phase 2 — `setup_vs_mode` expansion

**STATUS (as of 2026-04-20): MERGED to `netplay` branch.** See merge commits:
- Track A (Phases 1-3): `417609b4`
- Track B (Phases 4-5): `b82802db`
- Track C (Phase 7): `a46073a3`

Individual phase commits: `5698e7a3` (Phase 2 — expand setup_vs_mode to full frame-0 canonicalization) plus review-pass fix commits (Track A findings rolled into Phase 1/2/3 review fixes).

**Pitch:** Frame-0 canonicalization — the single most important function for rollback initial sync — is currently 30 lines vs 3sxtra's 234. Close the gap.

**Goal:** Port 3sxtra's `setup_vs_mode` at `/tmp/3sxtra/src/netplay/netplay.c:158-391` into our `src/netplay/netplay.c:117-147`, substituting `MenuTask_SetPhase(MTP_NETPLAY_IDLE)` with our equivalent `task[TASK_MENU].r_no[0] = 5` (which is already there — verified at our `netplay.c:118`).

**Deliverables:**
- `src/netplay/netplay.c::setup_vs_mode` — +~150 LOC (per research doc §6.3).
- New helper globals: `s_negotiated_ft` (first-to-X, research doc §6.2 flags it as a new var we need to add).
- Wire-up of `save_w[MODE_NETWORK]` defaults. Verify `save_w` array shape matches 3sxtra's (both forks inherit from upstream, but the `Pad_Infor[p].Shot` identity-mapping helper may differ — confirm during tier-3).

**Prerequisites:**
- Phase 1 complete (the 33 GameState additions include several fields (`bg_pos`, `fm_pos`, `Screen_Switch`, `system_timer` etc.) that setup_vs_mode zeroes; cross-port cleanly only once they're present).

**Validation:**
- Desktop LAN match (same-host two-process) end-to-end: 300-frame session with zero desync events.
- Match survives "rematch" (end match → press button → second match) without state pollution — this is the specific case `setup_vs_mode` fixes.
- Existing unit tests `test_netplay_run`, `test_netplay_refactor` (research doc §3.6) still pass.

**Sub-tasks:**
1. Copy 3sxtra's setup_vs_mode verbatim into a scratch file; go line-by-line annotating which of our engine globals match 3sxtra's names vs diverge.
2. Patch: `MenuTask_SetPhase(MTP_NETPLAY_IDLE)` → our existing `task[TASK_MENU].r_no[0] = 5` (no-op change since our line is already there).
3. Patch: `plw[].wu.pl_operator = 1` (3sxtra `/tmp/3sxtra/src/netplay/netplay.c:195-196`) → `plw[].wu.operator = 1` (ours `src/netplay/netplay.c:121-122`, verified 2026-04-20). Research doc §9.5 point 1 flags this rename; we keep our name, adjust the line during port.
4. Add `s_negotiated_ft` + `save_w[MODE_NETWORK].Battle_Number` wiring.
5. Remove the isolated `Random_ix16 = 0; Random_ix32 = 0;` at our current line 142-143 and fold into the broader RNG-reset block from 3sxtra (all 10 RNG indices).
6. Keep our existing `clean_input_buffers()` call (verified at `netplay.c:146`) — 3sxtra's equivalent is `SDL_zeroa(Check_Buff), SDL_zeroa(Convert_Buff)` plus the function call; merge not replace.

**Risks:**
- R-2.1 (high, #4 in main register): silent desync if any of the 150 new reset lines references a symbol that doesn't exist in our engine. Caught at compile time if missing globals; caught only at match-time if the symbol is a shadow (e.g., identically-named but different-indexed array).
- R-2.2 (medium): `s_negotiated_ft` is a new variable that must be read by the match-end reset logic. Wire-up mistakes cause round count to drift across rematches.

**Effort:** M (5–8 days) per research doc §20.

**Workstream:** WS-Engine.

---

### Phase 3 — Focused checksum, sanitizers, desync dump

**STATUS (as of 2026-04-20): MERGED to `netplay` branch.** See merge commits:
- Track A (Phases 1-3): `417609b4`
- Track B (Phases 4-5): `b82802db`
- Track C (Phase 7): `a46073a3`

Individual phase commits: `671aa18c` (Phase 3 — port focused checksum, sanitizers, dump_desync_state) plus review-pass fix commits (Track A findings rolled into Phase 1/2/3 review fixes).

**Pitch:** Replace our "hash-the-whole-247KB-State-in-debug-only" checksum with 3sxtra's focused whitelist (research doc §8.1). Ship it in release. Without this, production desyncs are undetectable.

**Goal:** Port `save_current_state` + focused-checksum + `sanitize_plw_pointers` + `sanitize_work_rendering` + `dump_desync_state`. Make focused checksum unconditional of `#if DEBUG` (research doc §19 risk 5, §8.2).

**Deliverables:**
- `src/netplay/game_state.c` updated with `save_current_state` and djb2-based focused hash (research doc §8.1 lists the exact fields; `djb2_updatep`/`djb2_update_mem` live in our `sf33rd/utils/djb2_hash.h`, verified at our `src/netplay/netplay.c:21` import).
- Our existing `clean_work_pointers` / `clean_plw_pointers` (verified at our `src/netplay/netplay.c:248-285`) merged with 3sxtra's `sanitize_work_rendering` (palette-bit 0x2000 masking, research doc §8.4). Keep our `cb`/`rp` zeroing (research doc §19 risk 2) because those fields exist in our PLW and not 3sxtra's.
- `dump_desync_state` behind `#if DEBUG` as a separate function (so release still has detection, just not dump).
- New explicit whitelist-add for `combo_type[2]` and `remake_power[2]` into the focused checksum (research doc §19 risk 1 — our top-level globals, not PLW members).
- `CHECKSUM` preprocessor define: currently our `CMakeLists.txt:95-96` only sets `CHECKSUM` for Release + non-PORT_MISTER. Extend: set for all Release builds including MiSTer once focused-checksum is shipping.

**Prerequisites:**
- Phase 1 complete (needs the 33 additional fields in GameState, even though most aren't in the checksum whitelist).

**Validation:**
- Desktop Release build has active checksum (grep for `calculate_checksum` or `save_current_state` symbol in binary; confirm not DCE'd out).
- On intentional desync injection (flip one bit in one peer's `Random_ix16` before frame 10), both peers detect mismatch within same frame.
- No false-positive desyncs across 10 back-to-back LAN matches (regression against phase 2 baseline).
- Release-build MiSTer binary has symbol present (`arm-linux-gnueabihf-readelf` on the stripped build-root confirms).

**Sub-tasks:**
1. Copy 3sxtra's `save_current_state` (research doc §8.1 field list) into our `game_state.c`; adjust each line for our field names.
2. Merge `sanitize_plw_pointers`: KEEP our existing 30+ pointer zeros + our `cb`/`rp` zeros; ADD 3sxtra's `sanitize_work_rendering` which only masks palette-bit 0x2000 (research doc §8.4).
3. Port `sanitize_work_rendering` separately (it's surgical, ours is more aggressive — see research doc §8.4). Use 3sxtra's verbatim.
4. Add `combo_type`/`remake_power` to the focused-checksum whitelist explicitly, since they're not in PLW on our side (research doc §19 risk 1 — this is the fix for that risk).
5. Remove `#if DEBUG` gate from the new save_current_state / checksum path at our `netplay.c:240,389`.
6. Keep `#if DEBUG` on dump-to-disk path only.

**Risks:**
- R-3.1 (high, #5 in main register): if we forget to add our globals (`combo_type`, `remake_power`) to the whitelist, damage scaling drifts silently and checksum fails to catch.
- R-3.2 (medium): verbatim copy of 3sxtra's `sanitize_plw_pointers` zeros 30 fields that exist in both; but 3sxtra's version doesn't zero our `cb`/`rp`. If we do verbatim without merge, heap pointers leak into the checksum → false-positive divergence.
- R-3.3 (low): performance regression from running checksum in Release. Research doc §8 implies focused hash is orders of magnitude smaller than our whole-state hash; measure during phase 9.

**Effort:** S (2–3 days) per research doc §20.

**Workstream:** WS-Engine.

---

### Phase 4 — RmlUi + FreeType ARM cross-compile

**STATUS (as of 2026-04-20): MERGED to `netplay` branch.** See merge commits:
- Track A (Phases 1-3): `417609b4`
- Track B (Phases 4-5): `b82802db`
- Track C (Phase 7): `a46073a3`

Individual phase commits: `2433afdd` (Phase 4 — RmlUi 6.2 + FreeType 2.13.3 ARM cross-compile recipes) + `36443d96` (Phase 4 link librmlui.a fix) plus review-pass fix commits (`a3f1e2bd`, `5dfce90d`, `c9364ca8`, `5487bafb`).

**Pitch:** Get RmlUi 6.2 and FreeType 2.13.3 building for MiSTer via the existing `build-deps.sh` pattern. First checkpoint before any UI code can compile.

**Goal:** Add `build-deps.sh` recipes, mirroring 3sxtra's Pi4 recipe pattern at `/tmp/3sxtra/tools/batocera/rpi4/download-deps_rpi4.sh:201-227` (research doc §12.8).

**Deliverables:**
- `build-deps.sh` with new sections (ungated or gated to `mister` profile):
  - FreeType cross-build; minimal config (disable HarfBuzz/brotli/bz2/PNG/zlib per research doc §14.4).
  - RmlUi 6.2 cross-build with `RMLUI_LUA_BINDINGS=OFF`, `RMLUI_FONT_ENGINE=freetype`, and `CMAKE_SYSTEM_PROCESSOR=arm`.
  - Output dirs follow existing convention: `third_party/freetype/build/`, `third_party/rmlui/build/` — matches pattern of `GEKKONET_ROOT` / `SDL3_NET_ROOT` at our `CMakeLists.txt:243-244`.
- `CMakeLists.txt` updates: new `ENABLE_RMLUI` option (default OFF on MiSTer until green; default ON on desktop), conditional include + link.
- Platform note on library linkage: target has `/usr/lib/libfreetype.so.6.17.4` (verified live 2026-04-20), but Buildroot 2021.02.4 FreeType is older than RmlUi's pinned 2.13.3. **Decision (rationale below)**: static-link FreeType into our binary rather than dynamic-link; avoids ABI drift across MiSTer updates and keeps our deps hermetic.

**Prerequisites:**
- None; can start day 1.

**Validation:**
- `build-deps.sh mister` produces `third_party/rmlui/build/lib/librmlui.a`, `librmlui_debugger.a`, and `third_party/freetype/build/lib/libfreetype.a`.
- `readelf -A` on `libfreetype.a` shows `Tag_CPU_arch: v7`, `Tag_FP_arch: VFPv3`, `Tag_ABI_VFP_args: VFP registers` (docs/mister-runbook.md:145-156 describes this pattern for ARM-target validation).
- A `hello RmlUi` binary that only calls `Rml::Initialise(); Rml::Shutdown();` cross-compiles and links without errors. Does not need to run — just link.

**Sub-tasks:**
1. Read 3sxtra's `/tmp/3sxtra/tools/batocera/rpi4/download-deps_rpi4.sh:201-227` carefully — all verified present (UNVERIFIED: actual line contents; research doc cites but I have not re-read the script body).
2. Add `FreeType 2.13.3` recipe to `build-deps.sh` under the `mister` branch.
3. Add `RmlUi 6.2` recipe, using the FreeType we just built via `FREETYPE_ROOT` env var.
4. Extend `CMakeLists.txt` with `ENABLE_RMLUI` option and `target_link_libraries` conditional for mister.
5. Verify the Docker cross-build container has `cmake` new enough for RmlUi's 3.10–3.27 range (`/tmp/rmlui-6.2/CMakeLists.txt:3` — UNVERIFIED line content, research doc §12.1).

**Risks:**
- R-4.1 (medium, #6 in main register): FreeType builds but pulls system `-lz` where we want our own. Use `CMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE` or equivalent.
- R-4.2 (low): RmlUi's CMake auto-detects Lua if found on host; explicit `RMLUI_LUA_BINDINGS=OFF` (research doc §12.1 confirms no `.rml` uses `<script>` tags in the netplay set) prevents drag-in.
- R-4.3 (low): RmlUi's tests/samples CMake enables GLFW; must disable with `RMLUI_SAMPLES=OFF, BUILD_TESTING=OFF`. Verified against `/tmp/rmlui-6.2/CMakeLists.txt:45-55`: `BUILD_TESTING` is the user-facing gate; `RMLUI_TESTS` is derived internally and passing it directly is a no-op.

**Effort:** S (2–3 days) per research doc §20.

**Workstream:** WS-UI + WS-Ops.

---

### Phase 5 — RmlUi SDL blend-mode subclass (first light)

**STATUS (as of 2026-04-20): MERGED to `netplay` branch.** See merge commits:
- Track A (Phases 1-3): `417609b4`
- Track B (Phases 4-5): `b82802db`
- Track C (Phase 7): `a46073a3`

Individual phase commits: `48edcda2` (Phase 5 — RmlUi SDL blend-mode fix + first-light harness) plus review-pass fix commits (`a3f1e2bd`, `5dfce90d`, `c9364ca8`, `5487bafb`, `456a2a77`).

**Pitch:** RmlUi's SDL backend assumes premultiplied alpha via `SDL_ComposeCustomBlendMode`, which SDL3's software renderer does not implement. Fix it with a ~2-LOC patch on a vendored copy of the backend.

**Goal:** Ship a vendored `RmlUi_Renderer_SDL.cpp` whose constructor sets `blend_mode = SDL_BLENDMODE_BLEND` (straight alpha) and confirm it renders correctly on MiSTer.

**Deliverables:**
- Vendored `src/port/sdl/rmlui/vendored/RmlUi_Renderer_SDL.{cpp,h}` (copied from RmlUi 6.2) with a ~2-LOC constructor patch replacing the `SDL_ComposeCustomBlendMode(...)` call with `blend_mode = SDL_BLENDMODE_BLEND`. Because `BeginFrame()` is non-virtual and `blend_mode` is private in the upstream header, patching the vendored translation unit is the practical path. A thin wrapper (`src/port/sdl/rmlui/rmlui_blend_fix.{cpp,h}`) exposes a `RenderInterfaceSDLMiSTer` subclass so call sites can discover the intent.
- Verified visual correctness: a minimal `.rml` overlay (single text string on solid background) renders to the shared SDL_Renderer on MiSTer without visible blend artefacts.
- Confirmed the RmlUi-side texture upload path is non-premultiplied (RmlUi's default, per `/tmp/rmlui-6.2/Backends/RmlUi_Renderer_SDL.cpp:35-36` comment: "RmlUi serves vertex colors and textures with premultiplied alpha" — RE-VERIFIED 2026-04-20 by reading lines 33-38 of the file live). So: the *default* is premultiplied, and we must explicitly override.

**Prerequisites:**
- Phase 4 complete (need the lib).

**Validation:**
- On MiSTer: overlay renders without blend glitches against a known-good test scene.
- Overlay can be shown/hidden without per-frame cost in the hidden state (research doc §12.4 cites `rmlui_wrapper.cpp:907-909` early-return; preserve that optimization).

**Sub-tasks:**
1. Vendor `Backends/RmlUi_Renderer_SDL.{cpp,h}` from RmlUi 6.2 into `src/port/sdl/rmlui/vendored/` (constructor line `RenderInterface_SDL::RenderInterface_SDL(SDL_Renderer* renderer)` verified via live read of `/tmp/rmlui-6.2/Backends/RmlUi_Renderer_SDL.cpp:33`). Because `BeginFrame()` is non-virtual and `blend_mode` is a private field in the upstream header, patching the vendored translation unit is the supported path.
2. Patch the vendored constructor: replace the `SDL_ComposeCustomBlendMode(...)` call with `blend_mode = SDL_BLENDMODE_BLEND;` — roughly a 2-LOC change.
3. Add a thin `RenderInterfaceSDLMiSTer` subclass in `src/port/sdl/rmlui/rmlui_blend_fix.{cpp,h}` that re-exports the vendored class under a MiSTer-specific name so call sites in `rmlui_wrapper.cpp` (Phase 6) and the first-light harness (Phase 5) can express intent.
4. Build a standalone test binary that brings up RmlUi, shows a single `.rml` with a text block, exits. Deploy to MiSTer; visually verify.

**Risks:**
- R-5.1 (medium, #7 in main register): SDL3's software renderer on armhf may have its own quirks beyond blend mode (e.g., non-premultiplied blending at sub-pixel precision). Only first-light verification catches this.
- R-5.2 (low): our existing ImGui on same SDL_Renderer (research doc §12.3) already renders fine → strong prior.

**Effort:** S (1–2 days) per research doc §20.

**Workstream:** WS-UI.

---

### Phase 6 — Port `menu_network.c`, `ms_*` glue, 9 RmlUi screens, assets

**Pitch:** The biggest single phase. Bring in the actual UI — native menu_network + RmlUi overlays + all screen glue — so the user can actually navigate into a match.

**Goal:** User-facing parity with 3sxtra's network menu surface on both RmlUi and native paths. Our current `Netplay_Menu` (verified at our `src/sf33rd/Source/Game/menu/menu.c:1339`, 2-item menu) is replaced by `Network_Lobby`-dispatching through the MenuScreen registry.

**Deliverables:**
- `src/sf33rd/Source/Game/menu/menu_network.c` (1764 LOC, verified), `menu_network.h`, `menu_network_constants.h` — copied, patched for our engine symbol names.
- `src/port/screens/ms_casual_lobby.c` (121), `ms_tournament_lobby.c` (102), `ms_ranked_matchmaking.c` (298), `ms_network_lobby.c` (206), `ms_leaderboard.c` (218), `ms_player_profile.c` (137), `ms_replay.c` (267), `ms_save_replay.c` (221), `ms_ranking.c` (69) — total **1639 LOC** (verified via `wc -l`).
- `src/port/menu_screen_registry.c` (408), `src/port/menu_screen_helpers.c` (236), `src/port/menu_bridge.c` (289), `src/port/menu_task.c` (69) — total **1002 LOC** (verified via `wc -l`).
- `src/include/port/menu_screen.h` (393 LOC, verified).
- `src/port/sdl/rmlui/` subtree: `rmlui_wrapper.{cpp,h}` + the 9 netplay-specific screens:
  - `rmlui_network_lobby.cpp` (1377 per research doc §3.3 — UNVERIFIED, not re-counted)
  - `rmlui_casual_lobby.cpp` (1053)
  - `rmlui_tournament_lobby.cpp` (1210)
  - `rmlui_ranked_matchmaking.cpp` (359)
  - `rmlui_network_replay_picker.cpp` (548)
  - `rmlui_ingame_chat.cpp` (286)
  - `rmlui_netplay_ui.cpp` (436)
  - `rmlui_leaderboard.cpp` (361)
  - `rmlui_player_profile.cpp` (519)
  - Plus `rmlui_wrapper.cpp/h` (1506 + 169)
- `assets/ui/*.{rml,rcss}` — mirror 3sxtra (620 KB total, research doc §3.3; total dir size re-verified 2026-04-20 via `du -sh /tmp/3sxtra/assets/ui` = 620K).
- `assets/ui/fonts/BoldPixels.ttf` (160 KB) — required. `NotoSansJP` + `NotoEmoji` excluded from MVP (see §12 deferrals).
- `CMakeLists.txt` — add `list(FILTER GAME_SRC EXCLUDE REGEX "src/port/sdl/rmlui/")` conditional on `NOT ENABLE_RMLUI`.
- Stub-safe include guards for the 4 self-guarded headers (research doc §11.4 lists: `rmlui_wrapper.h`, `rmlui_casual_lobby.h`, `rmlui_ingame_chat.h` — `rmlui_casual_lobby.h` is NOT self-guarded per research doc §11.4, flagged needing stub).

**Prerequisites:**
- Phases 1, 4, 5 complete.

**Validation:**
- Build (telemetry flavor per AGENTS.md) succeeds with `ENABLE_NETPLAY=ON` and `ENABLE_RMLUI=ON`.
- On MiSTer: navigate Title → Online → Casual Lobby → Create Room → wait for second player (can be verified by querying the lobby HTTP endpoint manually).
- On MiSTer: navigate Title → Online → LAN Play → Auto-Connect → successful discovery beacon → placeholder for phase 9 match start.
- No regression on existing offline flow (Arcade / Training / Options) — menu transitions still work.
- Per research doc §11.4, `netplay.c` has 8 RmlUi touchpoints at lines 30-33, 499, 825, 836, 1030, 1035, 1043, 1044, 1053 (CITED, not re-verified — the 3sxtra `netplay.c` is 1128 LOC per `wc -l` confirming line numbers are plausible). All must resolve post-port.

**Sub-tasks:**
1. Copy `menu_network.c` + headers; hand-patch `Pad_Infor`, `save_w`, task indexing for our engine.
2. Copy `src/port/menu_screen_registry.c` + `_helpers.c` + `menu_screen.h`; wire into our `main.c` init sequence (UNVERIFIED: 3sxtra's main.c line range for init — flagged).
3. Copy all 9 `ms_*` screen files.
4. Copy `src/port/sdl/rmlui/` minus the non-netplay screens (`rmlui_char_select`, `rmlui_continue`, `rmlui_copyright`, etc.) unless they're transitively required by the wrapper or the netplay screens.
5. Copy `assets/ui/` — all `.rml` / `.rcss` / `BoldPixels.ttf`.
6. Wire `src/main.c` or `src/port/sdl/sdl_app.c` main-loop to the `MenuScreen_GetCurrent()`/`MenuScreen_Goto()` surface (research doc §11.4 references both).
7. Replace our `menu.c:1339` 2-item `Netplay_Menu` with call into `Network_Lobby` dispatcher.
8. For the native-only LAN path (research doc §11.2 table: "LAN-only lobby ... 1305-1687 ... 100% native"), verify it compiles without RmlUi (stub the `rmlui_casual_lobby_get_room_code()` at discovery.c:138 and menu_network.c call sites to return `""`).

**Risks:**
- R-6.1 (high, #8 in main register): `menu_network.c` calls functions in 3sxtra's refactored `menu/` subtree (e.g., `menu_draw.c`, `menu_input.c`) that we don't port. Either we accept an additional ripple (bringing in `menu_draw.c` = 132 LOC, `menu_input.c` = 2586 LOC, `menu_internal.h`) or we rewrite the callsites to our existing menu renderer. Rewrite path is cheaper in LOC but creates persistent fork divergence. Recommend accepting the ripple and pulling in `menu_draw.c`, `menu_input.c`, `menu_internal.h` — budget +~3000 LOC.
- R-6.2 (medium): `.rml`/`.rcss` asset paths are hardcoded relative to the binary; deployment packaging (under `/media/fat/games/3s-arm/`) needs update.
- R-6.3 (medium): the 9 RmlUi `.cpp` files transitively include other 3sxtra-only `.h` files we don't have. Verified 2026-04-20: `rmlui_wrapper.cpp` includes `port/sdl/rmlui/rmlui_casual_lobby.h` directly (line 15) and others pull in Lua/OpenGL headers we don't link. Each pull-in is a cascade; budget extra days.
- R-6.4 (low): the `MenuScreenId` enum in `menu_screen.h` (393 LOC) lists all screens including ones we don't port. Keep the enum intact to avoid re-synced ABI, stub the non-ported screens.

**Effort:** L (10–15 focused days) per research doc §20. Could be XL (15–20 days) if the R-6.1 ripple materializes at the pessimistic end.

**Workstream:** WS-UI (primary); WS-Engine for `menu_network.c` engine-side integration.

---

### Phase 7 — GekkoNet + SDL3_net ARM cross-compile

**STATUS (as of 2026-04-20): MERGED to `netplay` branch.** See merge commits:
- Track A (Phases 1-3): `417609b4`
- Track B (Phases 4-5): `b82802db`
- Track C (Phase 7): `a46073a3`

Individual phase commits: `36ffbb07` (Phase 7 — cross-compile GekkoNet + SDL3_net for MiSTer armhf) plus review-pass fix commits (`b9c72d29`).

**Pitch:** The two libraries our netplay already depends on are gated to `desktop` profile. Move them to build for `mister` profile too. No source code change.

**Goal:** `build-deps.sh mister` produces `libGekkoNet.a` and `libSDL3_net.a` for armhf.

**Deliverables:**
- `build-deps.sh`: remove the `if [ "$PROFILE" = "desktop" ]` gate around both recipes (verified currently at `build-deps.sh:182-246`).
- `libGekkoNet.a` at `third_party/GekkoNet/build/lib/libGekkoNet.a` for armhf.
- `libSDL3_net.a` at `third_party/SDL_net/build/lib/libSDL3_net.a` for armhf.
- `CMakeLists.txt`: our `ENABLE_NETPLAY` already unconditional; no change.

**Prerequisites:**
- None; can start day 1.

**Validation:**
- `readelf -A libGekkoNet.a`: `Tag_CPU_arch: v7`, hard-float.
- Test-link a minimal binary: `int main(){ gekko_session_create(...); return 0; }` → links without unresolved symbols.
- Research doc §14.4 risk: "GekkoNet transitive `asio.hpp` include on ARMv7"; verify at cross-build that `NO_ASIO_BUILD=ON` at build-deps.sh:198 actually prevents `asio.hpp` pull-in at header level. Symptoms: undefined references to asio symbols, or unexpected `boost`-prefix symbols.
- `net_tuning.h:45-56` mirrors SDL3_net private struct (research doc §21 #5); our SDL3_net cross-build must be pinned to `92022dc` verbatim (verified current at `build-deps.sh:219`).

**Sub-tasks:**
1. Delete the `if [ "$PROFILE" = "desktop" ]` wrapping at `build-deps.sh:182` and `:218`, AND their else-arms at `:210-212` (GekkoNet `else; echo "Skipping GekkoNet for profile '$PROFILE'"; fi`) and `:244-246` (SDL3_net `else; echo "Skipping SDL3_net for profile '$PROFILE'"; fi`). Verified 2026-04-20 via re-read of `/Users/sb/Developer/3sx-mister/build-deps.sh:175-246`. Leaves a clean unconditional build for both dependencies.
2. Run `build-deps.sh mister` in the Docker cross-compile container.
3. Capture compile errors (ASIO header leakage most likely); patch around with `-DGEKKONET_NO_ASIO` at compile time if needed (research doc mentions define at our `CMakeLists.txt:208`).
4. Re-link our existing `3s-arm` binary (ENABLE_NETPLAY=ON) for mister; confirm symbol resolution.

**Risks:**
- R-7.1 (medium, #6 in main register): `asio.hpp` leaks through a transitively-included GekkoNet header even with `NO_ASIO_BUILD=ON`. Patch with `-DGEKKO_NO_ASIO` or stub `asio.hpp` with empty include.
- R-7.2 (low): our Docker cross-compile container uses Debian-based cross-tools (verified: `tools/mister/setup-build-container.sh:157` installs `libc6-dev-armhf-cross`) — but target is Buildroot. Static libs (`.a`) are portable across Linux userlands; this is not a bug, just a note.
- R-7.3 (low, NEW 2026-04-20): GekkoNet pin `7be848c` (2026-02-24, `build-deps.sh:183`) vs. upstream HEAD of `github.com/HeatXD/GekkoNet` (2026-04-15). Verified 2026-04-20 by full clone + diff:
  - 3 commits ahead: `8ca4058` (Add runahead support to GameSession #45), `7f1f19e` (Fix spectators not receiving GekkoPlayerDisconnected events), `2685b0c` (Update README.md).
  - `GekkoLib/include/net.h` diff = **zero bytes**. `MsgHeader`, `MsgType`, `PacketType`, `SyncMsg`, `InputMsg`, `InputAckMsg`, `SessionHealthMsg`, `NetworkHealthMsg` all byte-identical. Wire magic and struct layout unchanged.
  - `GekkoLib/include/gekkonet.h` diff = +2 lines: new field `running_ahead` in `GekkoGameEvent` struct; new function `gekko_set_runahead(session, runahead)`. Both additive; do NOT break ABI for existing callers.
  - `backend.cpp` change is control-flow only (spectator disconnect handling), not wire.
  - **Verdict: our pin is wire-compatible with current upstream through at least commit `8ca4058`.** 3sxtra's unpinned `git clone --depth 1 master` could pick up the runahead fields, but as long as we keep our pin we're compatible. If we ever repin forward, re-run `diff` on `net.h` first.

**Effort:** S (1–2 days) per research doc §20.

**Workstream:** WS-Ops.

---

### Phase 8 — Net thread pinning and scheduling

**Pitch:** With `CONFIG_PREEMPT_NONE` kernel and our `SCHED_FIFO prio 49` pacer, the net thread needs explicit CPU pinning + priority to avoid starving or being starved.

**Goal:** Pin game loop + pacer to CPU0, net thread to CPU1 with `SCHED_FIFO prio 20` (research doc §16.7).

**Deliverables:**
- `src/netplay/netplay.c` or equivalent: `pthread_setaffinity_np(CPU1)` + `pthread_setschedparam(SCHED_FIFO, prio=20)` when net thread is spawned.
- Fallback: if `SCHED_FIFO` fails (e.g., non-root), use `SCHED_OTHER` pinned to CPU1 (research doc §16.7 option 1).
- Optional: `sysctl net.core.rmem_max` bump at boot via our startup hook (see §7 — user decision pending, flagged in §15).

**Prerequisites:**
- Phase 7 (need the actual netplay library built).

**Validation:**
- On MiSTer during active netplay session: `ps -LeF | grep 3s-arm` shows net thread on CPU1, game thread on CPU0.
- `show-fps` overlay stays at 60 FPS with active net I/O (no regression vs offline baseline).
- No pacer-starvation symptoms (frame drop correlated with packet burst).

**Sub-tasks:**
1. Identify where net thread is spawned in our `src/netplay/netplay.c` (or `sdl_net_adapter.c` — research doc §3.2 shows 97 LOC; likely there).
2. Add pthread_attr + setaffinity + setschedparam calls.
3. Measure on-device with `show-fps` against known-bad workload (Genei Jin super art, per MEMORY.md `project-mts-sprite-raster-bottleneck.md`).

**Risks:**
- R-8.1 (low): `SCHED_FIFO` requires `CAP_SYS_NICE`. We run as root on MiSTer (`reference-mister-network-stack.md`), so uncapped; validate anyway.
- R-8.2 (low): hard pinning to CPU1 means thread can't migrate if CPU1 is hot; on A9 dual-core this is a feature (deterministic), not a bug.

**Effort:** S (1–2 days) per research doc §20.

**Workstream:** WS-Engine + WS-Ops.

---

### Phase 9 — LAN match on two MiSTers

**CONDITIONAL per §15 #1:** Only run this phase if a second MiSTer unit is available. Otherwise skip to Phase 10; its validation criterion was revised (see §15 implications at the end of §15) so MiSTer-vs-MiSTer is NOT required for MVP sign-off.

**Pitch:** First end-to-end proof: two MiSTer boxes, same subnet, wired eth0 discovery, GekkoNet rollback match, zero desync over 300 frames.

**Goal:** Validate the integrated system on real hardware pair. No internet, no STUN, no lobby server — just LAN discovery + rollback.

**Deliverables:**
- Second MiSTer unit on user's network (UNVERIFIED — may require hardware purchase; see §15 open question).
- Wired eth0 link between both (user currently has WiFi via RTL8821CU per task brief).
- Packaged build of 3s-arm + 3S-ARM.rbf deployed to both per `docs/mister-runbook.md`.
- LAN discovery beacon seen on both (`tcpdump -i eth0 port 7999` on a peer box; UNVERIFIED — tcpdump presence on MiSTer).
- Match played to completion, 300+ frames, zero desyncs.
- Netstats overlay on (research doc §3.2, 11.2: `src/port/sdl/netstats_renderer.c`, verified extant at our side).

**Prerequisites:**
- Phases 1, 2, 3, 6, 7, 8 complete.

**Validation:**
- 300-frame LAN match, no desync events (checksum from phase 3 must not fire).
- Stable 60 FPS (per `show-fps` overlay).
- Re-run of match 3+ times with rematches; no desync from stale state (this is the phase-2 guarantee).
- Optional (if time permits): stress with intentional packet loss via `tc qdisc` on one side; rollback should cover.

**Sub-tasks:**
1. Deploy build to MiSTer #1 (user's unit, 192.168.1.171).
2. Procure + set up MiSTer #2 (pending user decision).
3. Run LAN discovery on both; verify beacon sent and received.
4. Play match, log desync events.
5. Iterate on desync hunts (research doc §20 acknowledges "desync hunts wildcard").

**Risks:**
- R-9.1 (high, #1 in main register): first on-device desync surfaces. Combinatorial to diagnose — could be any of risks R-1.1, R-2.1, R-3.1, R-6.1 materializing.
- R-9.2 (medium): USB WiFi jitter on WiFi fallback if eth0 unavailable (research doc §16.9, §21 #3). Recommend wired only for this phase.
- R-9.3 (low): second-MiSTer availability (§15).

**Effort:** M (3–5 days plus desync hunt tail, which is unbounded).

**Workstream:** WS-Engine + WS-Ops.

---

### Phase 10 — STUN + internet play via shared 3sxtra lobby (REVISED 2026-04-20)

**Pitch:** Port the lobby client, wire STUN, point at 3sxtra's production lobby, play MiSTer ↔ MiSTer over public internet with client-side arch filtering.

**Goal:** First internet P2P match through casual-room path using 3sxtra's hosted lobby at `http://152.67.75.184:3000`. Exercises: lobby HTTP client, STUN hole-punching, GekkoNet over internet, MiSTer-only handshake rejection.

**Deliverables:**
- Game binary with `ENABLE_NETPLAY_STUN=ON`, `ENABLE_NETPLAY_LOBBY=ON` (gated per research doc §18.1).
- `src/netplay/stun.c` (458 LOC per research doc §3.1), `discovery.c` (462), `lobby_server.c` (2229), `sha256.c` (164), `identity.c` (166) — copied from 3sxtra unchanged.
- **Layer 1 filter:** `[MiSTer]` prefix applied on every `/room/create`; filter applied on every `/rooms/list` response.
- **Layer 2 presence tag:** our client appends `" [MiSTer]"` to `display_name` on every `/presence` update (see §8.2.5 for wire details; extra JSON fields in `/searching/start` and `/presence` are dropped server-side per `lobby-server.js:2683,2720`).
- **Layer 3 handshake:** `MIST` magic-prefix packet exchange on GekkoNet UDP socket before rollback starts; reject mismatched opponents with user-facing error.
- **Disabled endpoints:** `/match_result` and `/match_result/replay` POSTs explicitly skipped (per §8.4).
- Config keys: `CFG_KEY_LOBBY_SERVER_URL` defaults to 3sxtra's URL; `CFG_KEY_NETPLAY_ALLOW_CROSS_PLATFORM=false` default.
- Match completed: MiSTer (192.168.1.171) vs another MiSTer peer (over internet, via 3sxtra's lobby), 300+ frames, no desync. *Note: requires a second MiSTer — see §15 #1; validation adjusted per §15 implications.*

**Prerequisites:**
- Phase 9 (LAN baseline) if a second MiSTer is available; otherwise phase 10 is first cross-machine validation per §15 #1.
- Probe verifies 3sxtra's server is live + reachable from MiSTer (onboard eth0 preferred per research doc §16.2).
- No formal coordination required with 3sxtra author (per §15 #9); see §8.5.

**Validation:**
- `curl -s http://152.67.75.184:3000/ | head -1` returns 2xx from MiSTer.
- STUN discovery from MiSTer reaches one of the 4 configured servers (`stun.l.google.com`, etc. — `/tmp/3sxtra/src/netplay/stun.c:191-199`, research doc §13.4).
- Two MiSTer peers on different networks (one at home, one at a friend's) establish P2P via 3sxtra's lobby, match plays to completion.
- Desktop peer trying to join a `[MiSTer]`-room is rejected at handshake stage with the user-facing error. Validates Layer 3 is authoritative.
- `/match_result` is NOT called (verified via lobby log or manual tcpdump — 3sxtra server Glicko-2 state unchanged).

**Sub-tasks:**
1. Copy `src/netplay/stun.c`, `discovery.c`, `lobby_server.c`, `sha256.c`, `identity.c` from 3sxtra (preserving `[MiSTer]` prefix and gating changes).
2. Retire legacy `src/netplay/matchmaking.{c,h}` + `matchmaking_stub.c` (verified 2026-04-20: 208+30+27 LOC custom TCP line-protocol client, 6-public-function surface: `Matchmaking_Start/Run/GetState/GetResult/GetSocket/Reset`). The call sites are at `src/netplay/netplay.c:178, 648, 657, 659, 662, 682, 686, 687` and `src/port/sdl/netplay_screen.c:37` (verified via Grep). Transition strategy:
   - DELETE: `matchmaking.c`, `matchmaking.h`, `matchmaking_stub.c` once `lobby_server.c` is wired (they have no reuse value — custom protocol incompatible with 3sxtra's HTTP+SSE server).
   - REPLACE: call sites in `netplay.c` now go through `LobbyServer_*` APIs (`LobbyServer_JoinRoom`/`LobbyServer_AcceptMatch`/`LobbyServer_SSEPoll` → `NETPLAY_SESSION_TRANSITIONING`).
   - REPLACE: `src/port/sdl/netplay_screen.c` — its only `Matchmaking_GetState()` poll is redirected to `LobbyServer_SSEPoll()`-driven status; consider rewriting on top of the new `sdl_netplay_ui.{cpp,h}` from 3sxtra instead of retaining `netplay_screen.{c,h}`. Tier-3 decides whether to keep the 2-file legacy module or fold entirely into the ported UI.
   - RETAIN: `netplay_screen.{c,h}` may be kept as a minimal native-path stub during the transition so builds stay green; DO NOT delete until 3sxtra's `sdl_netplay_ui.cpp` is in place.
3. Wire the `Netplay_SetStunSocket()` / `NET_CreateDatagramSocket()` paths (research doc §4.7).
4. Add `CFG_KEY_NETPLAY_PORT`, `CFG_KEY_NETPLAY_INPUT_DELAY`, `CFG_KEY_LOBBY_SERVER_URL`, `CFG_KEY_NETPLAY_ALLOW_CROSS_PLATFORM` to `src/port/config/` (research doc §18.4).
5. Implement Layer 1 room-name prefix + client-side list filter.
6. Implement Layer 2 `display_name` suffix + client-side strstr filter on `/rooms/list`, `/searching`, `/presence` responses (per §8.2.5).
7. Implement Layer 3 `MIST` magic-prefix handshake on GekkoNet socket, mirroring chat `3SXC` pattern at `sdl_net_adapter.c:14` (see §8.2.4 for insertion point + wire format).
8. Explicitly gate `/match_result` + `/match_result/replay` POSTs off.
9. Run internet match.

**Risks:**
- R-10.1 (high, #2 in main register): symmetric NAT on user's home router prevents hole-punching (research doc §16.6). Workaround: relay-TURN, not scoped in MVP. Flag in §15.
- R-10.2 (medium): 3sxtra server unreachable from MiSTer (firewall, route, DNS). Diagnostic logging in lobby client.
- R-10.3 (low): clock skew between peers breaks HMAC signing (60s window per lobby-server.js:11 — verified).
- R-10.4 (medium, NEW): 3sxtra author declines access or asks us to leave after MVP ships. Mitigation: self-host contingency (§8.7). Hard-coded fallback URL + config-switchable.
- R-10.5 (low, NEW): display_name suffix lands inside user-chosen name budget (22 chars user portion + 9 chars suffix = 31 total). Users with longer preferred handles see truncation. Mitigation: document the 22-char effective budget; tier-3 task caps input length at publish time.
- R-10.6 (low, NEW): our `[MiSTer]` rooms clutter 3sxtra's desktop-user browsers. Mitigation: use `visibility=1` as escalation (MiSTer users find rooms by code instead of list).

**Effort:** M (3–5 days) per research doc §20.

**Workstream:** WS-Engine + WS-Lobby (client-only, no infra).

---

### Phase 11 — UPnP automatic port forwarding

**Pitch:** Optional quality-of-life: attempt UPnP-IGD port forwarding so users behind NAT don't need manual router config.

**Goal:** `miniupnpc` integration working; falls back gracefully when router doesn't support UPnP or has it off.

**Deliverables:**
- `src/netplay/upnp.c` (195 LOC), `upnp.h` (39) — mirrored from 3sxtra.
- `build-deps.sh mister` recipe for `miniupnpc` (system package on Debian, source build on Buildroot — UNVERIFIED: Buildroot 2021.02.4 package availability. Likely need source build.)
- `CMakeLists.txt` `ENABLE_NETPLAY_UPNP=OFF` default (opt-in), flipping `HAVE_UPNP` define (research doc §3.1 confirms `HAVE_UPNP` gate).
- User-facing setting in OSD to opt-in (deferred to post-MVP if scope permits).

**Prerequisites:**
- Phase 10 (STUN is the primary path; UPnP is secondary).

**Validation:**
- On a router that supports UPnP (documented test hardware TBD in §15): MiSTer opens port 7999 externally, verified via `curl ifconfig.co` + external tcpdump.
- On a router without UPnP: MiSTer logs the failure and continues to the STUN path; no hang.

**Sub-tasks:**
1. Source-build `miniupnpc` as a static lib; add `MINIUPNPC_ROOT` to `third_party/`.
2. Copy `upnp.c`/`upnp.h` from 3sxtra.
3. Wire `HAVE_UPNP` conditional in `CMakeLists.txt`.
4. Test on two home routers (mine + one other).

**Risks:**
- R-11.1 (low): `miniupnpc` cross-compile for armhf/Buildroot. It's pure userland C, no platform quirks; UNVERIFIED but strong prior from Pi4 port.
- R-11.2 (low): UPnP security posture (some routers disable by default). Covered by graceful fallback.

**Effort:** S (1–2 days) per research doc §20.

**Workstream:** WS-Engine + WS-Ops.

---

### Phase 12 — Lobby feature matrix (chat; read-only leaderboard + replay browsing; NO ranked/replay WRITES)

**Banner (per §15 #6):** Spectator mode is deferred to post-MVP Phase 13. Any text in this phase that once referenced spectator was removed 2026-04-20 to eliminate contradictions with the decision table.

**REVISED 2026-04-20:** Since we use 3sxtra's shared lobby (§8), features that WRITE to their backend are disabled or deferred. MiSTer users see their MATCHES happen MiSTer-only (via arch filter), but they see the global 3sxtra leaderboard/replay data as READ-ONLY if they browse.

**Pitch:** Light up the remaining non-write feature set — in-game chat, read-only leaderboard + replay browsing. Ranked ratings, tournament hosting, replay uploads, and spectator mode are explicitly deferred.

**Goal:** Complete the visible feature surface for MiSTer users, respecting the shared-lobby hygiene commitments from §8.4.

**Deliverables:**
- `src/netplay/ping_probe.c` (381), `ping_probe.h` (75) — pre-match ping sampling (read-only, client-local).
- `src/netplay/net_detect.c` (144), `net_detect.h` (30) — wired/WiFi detection (client-local).
- In-game chat overlay wired to OOB magic prefix `0x33 0x53 0x58 0x43` in `sdl_net_adapter.c`. Chat is P2P, not through lobby — no moderation burden on 3sxtra's server.
- Leaderboard view: port `rmlui_leaderboard.cpp` as-is from 3sxtra. Shows their global leaderboard. Our MiSTer matches never appear in it (we don't POST `/match_result` per §8.4). **No MiSTer-specific overlay / filter / warning** per §15 #15 (least-work decision) — just ship what's there.
- Read-only replay browsing: `GET /replays`, `GET /replays/:id` consumed. Playback only if the replay is MiSTer-recorded (detect via replay metadata — tier-3 investigation whether format includes arch tag). Otherwise show "Not compatible with MiSTer build".
- `src/netplay/bracket.c` — **not ported**. Tournament hosting requires WRITE access to lobby tournament state, which we're declining.
- Spectator mode — moved to post-MVP Phase 13 per §15 #6.

**Prerequisites:**
- Phase 6 (UI) and Phase 10 (shared lobby integration) complete.

**Validation:**
- Chat: message sent on peer A → visible on peer B; profanity filter is served by OOB protocol (check for `bad-words` parity locally — UNVERIFIED if filtering is P2P or lobby-mediated; tier-3 investigation).
- Leaderboard: UI renders 3sxtra's global leaderboard fetched via `GET /leaderboard`. Our results do NOT appear (phase 10 disabled writes).
- Replay browsing: UI lists replays; clicking a non-MiSTer replay shows compatibility notice; clicking a MiSTer replay (if detectable) plays back.
- `/match_result` and `/match_result/replay` are still disabled (regression guard).

**Sub-tasks:**
1. Copy `ping_probe.c`, `net_detect.c` plus RmlUi wiring.
2. Hook chat overlay via existing OOB magic prefix.
3. Implement read-only leaderboard + replay browsing; gate playback on arch-compatibility check.
4. Verify no WRITE calls leak to 3sxtra endpoints (automated test or manual tcpdump audit).

**Risks:**
- R-12.1 (medium): chat moderation expectations. If 3sxtra chat is lobby-mediated with profanity filter, our P2P chat bypasses that entirely. Mitigation: port `bad-words` filter client-side as a separate library OR accept that MiSTer chat is unfiltered by their policy. Document in release notes.
- R-12.2 (low): replay metadata arch tag may not exist → we can't gate playback safely. Fallback: warn user on every non-own-recorded replay.
- R-12.3 (low): ranked/tournaments are deferred entirely — some users may expect them based on "full feature matrix" messaging. Document clearly as "deferred pending arch-neutral serialization".

**Effort:** XL (15–25 days) per research doc §20 (which defers entirely).

**Workstream:** WS-Engine + WS-UI + WS-Lobby.

---

## 6. Parallelization analysis

### 6.1 Critical path

Longest-serial path (based on §4.2 dependency matrix):

```
Phase 1 → Phase 2 → Phase 3 → Phase 9 → Phase 10 → Phase 12
  S-M        M          S         M         M         XL
  (5d)      (8d)       (3d)      (5d)      (5d)     (20d)
  --------------------------------------------------------
  ~46 focused days → ~10 calendar weeks part-time
```

Phase 12 (XL) dominates the tail and is structurally fragmentable; realistic MVP critical path cuts Phase 12 and ends at Phase 10 + Phase 11 in parallel → **~26 focused days → ~6 calendar weeks part-time**.

### 6.2 Side paths (parallelizable)

- **Phase 4 → Phase 5 → Phase 6 (UI path):** 2+2+15 = ~19d, fits in the 26d critical path envelope if started on day 1 alongside phase 1.
- **Phase 7 (cross-compile):** 2d, standalone, any time.
- **Phase 8 (scheduling):** 2d, any time after 7.
- **Phase 11 (UPnP):** 2d, standalone after 10.
- **WS-Lobby shared-lobby integration:** ~1d for baseline probe + config keys, rest rolled into Phase 10 sub-tasks. No parallel infra lane under shared-lobby direction.

### 6.3 Suggested calendar sequencing (part-time pace)

- **Week 1:** Phase 1 sub-tasks 1-3 (GameState fields). Phase 4 day 1 (build-deps recipes). Phase 7 (strip gates; build).
- **Week 2:** Phase 1 finish. Phase 4 finish. Phase 5 (first light).
- **Week 3:** Phase 2 start. Phase 6 start (big copy, minimal wiring).
- **Week 4:** Phase 2 finish. Phase 3 start.
- **Week 5:** Phase 3 finish. Phase 6 continues.
- **Week 6:** Phase 6 continues. Phase 8.
- **Week 7:** Phase 6 finishes. Phase 9 starts.
- **Week 8–9:** Phase 9 (desync hunt). Phase 10 infra-side (lobby deploy).
- **Week 10:** Phase 10 finishes. Phase 11.
- **Week 11+:** Phase 12 sub-features.

### 6.4 Contingency

Each phase assumes the previous phase's *validation* criterion passed. Risks in §10 are the main source of schedule slip. Budget 20% contingency on every phase; don't commit Week 11 milestones to an external audience.

---

## 7. Platform prerequisites (verified 2026-04-20)

### 7.1 Verified facts from live device (192.168.1.171)

All outputs below from `ssh root@192.168.1.171` 2026-04-20:

| Fact | Value | Source |
|---|---|---|
| Kernel | `5.15.1-MiSTer` | `uname -r` |
| Arch | `armv7l` | `uname -a` |
| Userland | `Buildroot 2021.02.4` | `/etc/os-release` |
| libc | `ldd (GNU) 2.31` | `ldd --version` |
| CPU | `ARMv7 Cortex-A9 rev 0, 2 cores` | `/proc/cpuinfo` |
| `net.core.rmem_max` | `180224` (176 KB) | `sysctl net.core.rmem_max` |
| `net.core.wmem_max` | `180224` | `sysctl net.core.wmem_max` |
| `net.ipv4.ip_local_port_range` | `32768 60999` | `sysctl` |
| `libcurl` | `/usr/lib/libcurl.so.4.7.0` | `ls /usr/lib/libcurl*` |
| `libfreetype` | `/usr/lib/libfreetype.so.6.17.4` | `ls` |
| `libssl` | `/usr/lib/libssl.so.1.1` | `ls` |
| `libSDL2` | `/usr/lib/libSDL2-2.0.so.0.14.0` (shared, but irrelevant — we use SDL3 we ship) | `ls` |
| `libstdc++` | `/lib/libstdc++.so.6.0.28` (gcc 10.x era ABI) | `ls` |
| GPU libs | none of libGL*, libEGL*, libGLES*, libvulkan*, libdrm*, libgbm* | `ls ... || echo MISSING` |
| `/dev/net/tun` | absent | `ls /dev/net/tun || echo NO_TUN` |
| Missing utilities | `getent`, `pgrep`, `iperf3`, `nft`, `node` | `which` |
| `curl` CLI | `/usr/bin/curl` present | `which curl` |

### 7.2 Corrections to research doc

**§15 claimed userland "glibc (armhf), Debian/Ubuntu armhf packages."** Correct: **Buildroot 2021.02.4 with glibc 2.31**. Root reads this at `/etc/os-release` (verified live).

Impact:
- Debian package names do not apply. `apt-get` is not present.
- Our cross-compile toolchain (Debian-based per `tools/mister/setup-build-container.sh:157` — `libc6-dev-armhf-cross`) targets glibc — compatible with 2.31 on target. Not broken; just correctly named.
- Any "use the system apt package" shortcut in the research doc (e.g., "miniupnpc system package" at §13.1) must be source-built instead on the cross-compile side.

### 7.3 Implications per phase

| Phase | Platform implication |
|---|---|
| Phase 4 | FreeType static-linked (not shared) — our own hermetic copy. The `/usr/lib/libfreetype.so.6.17.4` is NOT reliable across MiSTer updates and Buildroot minor revs. **Decision: static.** |
| Phase 4 | RmlUi static-linked. |
| Phase 6 | `assets/ui/` deployed under `/media/fat/games/3s-arm/assets/ui/` per `docs/mister-runbook.md`. |
| Phase 7 | GekkoNet + SDL3_net static — already the pattern in `CMakeLists.txt:286-287`. |
| Phase 8 | `net.core.rmem_max = 180224` is below what `net_tuning.h` wants. Two options: (a) ship a boot-time `sysctl` hook in `docs/mister-runbook.md` install procedure ("add `net.core.rmem_max=524288` to `/etc/sysctl.d/50-netplay.conf`"), (b) accept the cap and live with smaller recv buffer. **Recommendation: (a)** — the cap is too small for heavy-rollback scenarios; document the hook in runbook. See §15 #2. **Caveat (verified 2026-04-20 via `ssh root@192.168.1.171`):** MiSTer's Buildroot image has neither `/etc/sysctl.d/` nor `/etc/sysctl.conf` and its init scripts do NOT invoke `sysctl -p` at boot. We must ship our own hook (e.g., a BusyBox init.d snippet or an S99 rc.local equivalent) that creates the directory and reads config at boot. Unknown #10 captures the concrete path choice. |
| Phase 9 | Missing `iperf3` on device — substitute with a one-off `nc` or the netstats renderer. |
| Phase 10 | `libcurl` shared library present but we still static-link our own curl via build-deps.sh (if we need one — confirming: 3sxtra's `lobby_server.c` uses what? **UNVERIFIED** — research doc §3.1 lists `libcurl` at §13.1 as "Lobby only, dynamic linkage." Our options are dynamic against target libcurl or static. Flag for tier-3 phase 10 plan.) |
| Phase 10 | No `/dev/net/tun`: not a blocker (we don't need it). |
| Phase 11 | `miniupnpc` source-build (no Debian/apt). |
| Phase 12 | `node` not present: fine, node runs on 3sxtra's Oracle Cloud instance, not on MiSTer. |
| — | Runs as root: no capability issues for `SCHED_FIFO` etc. `reference-mister-network-stack.md`. |

### 7.4 On-device verification script update

Research doc §22's on-device verification script uses `getent` (line 1321 of research doc) and `pgrep` (line 1293). Both absent on target. Suggested substitutions (tier-3 detail):

- `getent hosts stun.l.google.com` → `host stun.l.google.com` or `nslookup stun.l.google.com` (UNVERIFIED: `host` / `nslookup` present on target — check before tier-3).
- `pgrep -a dhcpcd` → `ps -ef | grep '[d]hcpcd'`.
- `iperf3` subsection: skip or install iperf3 for jitter measurement (MiSTer has a packages path per `reference-quartus-build-env.md`-style workflow — UNVERIFIED for iperf3 specifically).

---

## 8. Lobby strategy — shared 3sxtra lobby + MiSTer-only arch filter (REVISED 2026-04-20)

### 8.1 Strategic direction

**Primary:** Use 3sxtra's existing production lobby. Our MiSTer client POSTs to the same URL 3sxtra desktop clients use. Matchmaking gated on our side to MiSTer-only peers.

**Target lobby:** `http://152.67.75.184:3000` (Oracle Cloud hosted, `OC-195` block, live + healthy per 2026-04-20 probe `HTTP 200 time=0.225s`). Hardcoded as `DEFAULT_LOBBY_URL` in 3sxtra's `src/netplay/lobby_server.c:73`. Overridable via `CFG_KEY_LOBBY_SERVER_URL` (their `src/port/config/config.h:66`).

**Why:** 32-bit vs 64-bit crossplay is structurally broken (research doc §9.7 cross-architecture compatibility section). Hosting our own lobby for a MiSTer-only cohort that can't crossplay with 3sxtra users anyway is disproportionate to the value delivered.

**Out-of-band requirement:** Informal coordination with 3sxtra's maintainer ("Daouid") before MVP ships. Not a technical blocker but a courtesy — and an opportunity to align on arch-tag protocol, later crossplay work, leaderboard partitioning, etc.

### 8.2 Three-layer MiSTer-only defense

Goal: zero cross-arch matches get past matchmaking → no silent desyncs for users.

**Layer 1 — Room name prefix.** When our client creates a room via `/room/create`, prefix the `name` field with `[MiSTer]` (9 of 31 name chars). When our client calls `/rooms/list`, filter returned rooms by this prefix. Desktop clients will SEE our rooms in their browser but the prefix makes it obvious to skip. Cost: ~10 LOC.

**Layer 2 — `display_name` suffix (PRIMARY mechanism).** The lobby server destructures a fixed field set in both `POST /presence` (`tools/lobby-server/lobby-server.js:2679-2710` — re-verified 2026-04-20: only `{ player_id, display_name, region, room_code, connect_to, rtt_ms, connection_type, ft }` make it to the stored record) and `POST /searching/start` (`lobby-server.js:2713-2727` — only `data.player_id` is read; all other keys are silently dropped). **Extra JSON fields ARE stripped.** Therefore the arch tag cannot ride on an arbitrary field; encode it in `display_name` as a `" [MiSTer]"` suffix instead. Display name is capped at 31 chars server-side (see `lobby-server.js:2694` `.slice(0,31)`) so budget the 9-char suffix (user_name up to 22 chars). Clients browsing `/presence` or `/searching` filter by substring match on the returned `display_name`. Cost: ~15 LOC (build suffix on publish; strstr filter on read). See §8.2.5 for the wire format detail.

**Layer 3 — Authoritative handshake rejection.** After STUN pairing, before GekkoNet starts exchanging inputs, exchange a `MIST` magic-prefix packet on the GekkoNet socket containing `{build_hash, arch="armv7", platform="mister"}`. If opponent doesn't respond with a matching profile within a short timeout (say 500 ms), abort session with user-facing error: "Opponent is not running a MiSTer build. Match cancelled." Uses the same pattern as 3sxtra's OOB chat magic (`3SXC` at `sdl_net_adapter.c:14`). **This is the actual desync preventer.** Cost: ~80 LOC.

**Config override:** `CFG_KEY_NETPLAY_ALLOW_CROSS_PLATFORM=false` default. Advanced users can disable the handshake guard only for 32-bit armv7 peers (e.g., experimental Android 32-bit crossplay — see research doc §9.7.7 niche case). Enabling produces a prominent warning. Default stays false.

### 8.2.4 Layer 3 MIST handshake — wire format and atomicity

Research target: ensure the handshake exchange happens AFTER the STUN socket is hole-punched (so both peers can reach each other) and BEFORE GekkoNet starts sending `SyncRequest`/`InputMsg` (the first desync-risk traffic).

**Insertion point in 3sxtra's flow (verified 2026-04-20 against `/tmp/3sxtra/src/netplay/netplay.c`):**

1. `Netplay_SetStunSocket(sock)` at `netplay.c:808` — STUN hole-punch complete; socket is bound and paired.
2. `Netplay_Begin()` at `netplay.c:824-871` — calls `setup_vs_mode()` then flips `session_state = NETPLAY_SESSION_TRANSITIONING`. No GekkoNet calls yet.
3. While `session_state == NETPLAY_SESSION_TRANSITIONING`, `Netplay_Run()` at `netplay.c:973-993` calls `step_game(true)` until `game_ready_to_run_character_select()` returns true for ≥2 frames, then calls `configure_gekko()` at `netplay.c:989` which in turn calls `gekko_create` (line 434), `gekko_start` (line 435), `gekko_net_adapter_set` (line 443/457), and `gekko_add_actor` (lines 488, 491).
4. GekkoNet first emits `SyncRequest` during the NEXT `gekko_update_session()` invocation (in `NETPLAY_SESSION_CONNECTING`).

**Chosen insertion point:** piggyback on the `sdl_net_adapter.c` `receive_data` interceptor the same way `3SXC` chat does (lines 194-222 of `sdl_net_adapter.c`, verified). Before `configure_gekko()` runs, perform a blocking-bounded `MIST` exchange on the STUN socket directly using `NET_SendDatagram`/`NET_ReceiveDatagram` (the socket is idle between phase 2 and phase 3 of the flow above; no race). Either both peers confirm compatible profile within 500 ms or abort to `Soft_Reset_Sub()`.

**Wire format:**

```
offset   size   field
------   ----   -----
0        4      magic = "MIST" (0x4D 0x49 0x53 0x54)
4        1      msg_type (0x01 = hello, 0x02 = ack, 0x03 = reject)
5        2      payload_len (big-endian, max ~128)
7        N      payload (LEB128-ish: null-terminated strings)
```

Hello payload: `"armv7\0" "mister\0" "<build_hash_7chars>\0"`.
Ack payload: same shape; peer confirms its own profile.
Reject payload: one-byte reason code + human-readable string.

**Timeout & retry:** 5× retransmit hellos at 100 ms each; accept ack from opponent any time inside the 500 ms window. If no ack, session teardown with user-facing "Opponent is not running a compatible MiSTer build. Match cancelled."

**Collision with GekkoNet:** GekkoNet's `zpp::serializer` wraps `MsgHeader { PacketType type; u16 magic; }` (`/tmp/GekkoNet-head/GekkoLib/include/net.h:38-46`). The serialized wire bytes start with the type enum (packed) then the magic u16. A 4-byte `"MIST"` prefix cannot collide because GekkoNet's type enum at byte 0 is in range `[0, 6]` (`net.h:26-36`) — never `0x4D` ('M'). Same reasoning the existing `3SXC` (`0x33`) relies on at `sdl_net_adapter.c:14`.

**Pseudocode (do NOT copy verbatim; illustrative only):**

```
// in Netplay_Run() at the TRANSITIONING→CONNECTING boundary,
// BEFORE configure_gekko() at netplay.c:989:
if (stun_socket && !mist_handshake_done) {
    if (!mist_handshake_send_and_wait(stun_socket, remote_addr, 500_ms)) {
        session_state = NETPLAY_SESSION_EXITING;
        show_user_error("Opponent is not running a compatible MiSTer build.");
        Soft_Reset_Sub();
        return;
    }
    mist_handshake_done = true;
}
```

Tier-3 plan owns exact LOC count, allocation, and error-string i18n.

### 8.2.5 Layer 2 implementation via `display_name` suffix

Verified 2026-04-20 against `/tmp/3sxtra/tools/lobby-server/lobby-server.js`:

- `display_name` is persisted server-side in the `players` map via `POST /presence` (line 2694: `.slice(0,31)`). Max 31 chars, UTF-8 safe (JS string slice on code units — ASCII-only recommended for filter robustness).
- `display_name` is returned in `GET /searching` responses (see line 2745+ handler) and via SSE events in `/room/events` + `/rooms/list` room member summaries.
- No per-player `arch` / `build` / `platform` field exists server-side; extra JSON passed to `/presence` is dropped during the destructure at line 2683.

**Format:** `"<user-chosen-name> [MiSTer]"`. Suffix is 9 bytes (space + bracket + 6 letters + bracket). User-chosen portion budgeted to 22 chars to stay under the 31-char cap.

**Publish side (our client):** when calling `LobbyServer_UpdatePresence`, append the suffix to the display name string before send. When calling `LobbyServer_CreateRoom`, apply the same prefix convention to `room.name` (Layer 1 already handles this — layers 1 and 2 together effectively encode MiSTer at both room and player level).

**Consumer side (our client):** on every `/rooms/list` response, reject rooms whose `name` doesn't contain `[MiSTer]`. On every `/searching` / `/presence` response, reject entries whose `display_name` doesn't contain `[MiSTer]`. A tight `SDL_strstr(display_name, "[MiSTer]")` check suffices.

**Downside we accept:** desktop users see a `[MiSTer]` tag in their room browser. Visual clutter only; Layer 3 is the authoritative reject.

**Edge case:** a desktop user could spoof a display_name containing `[MiSTer]` to try to match us. Layer 3 `MIST` handshake rejects them at session setup; no desync risk.

### 8.3 Endpoints we USE

Matchmaking (client-side port of 3sxtra's `lobby_server.c`, unchanged wire format):
- `POST /searching/start` — register presence
- `POST /searching/stop` — deregister
- `GET /rooms/list` — browse public rooms (we filter by `[MiSTer]` prefix)
- `POST /room/create` — create room (we inject prefix)
- `POST /room/join` — join room
- `POST /room/leave`
- `POST /room/chat` — send chat (matches Layer 3 guarantee)
- `GET /room/events` (SSE) — room state updates
- `POST /room/match/accept` — confirm match proposal
- `POST /room/match/decline`
- `POST /room/match/end`

Identity (minimal, still needed):
- Our client uses `identity.c` to compute a stable player_id fingerprint (research doc §3.1). Value passes to server in `/searching/start` as `player_id`. Server treats as opaque. Fine.

### 8.4 Endpoints we DO NOT use

To avoid polluting 3sxtra's global leaderboard / Glicko-2 ratings / replay storage:

- **`POST /match_result`** — result reporting for Glicko-2. Skip. Our wins/losses do not feed their ranked ladder. If we later want an internal MiSTer-only leaderboard, we do it client-local (per-device) or via a future self-hosted shadow lobby.
- **`POST /match_result/replay`** — replay upload. Skip. Replays stay local (`replays/` directory per research doc §3.1 reference).
- **Tournament admin endpoints** (`/tournament/*`) — skip for MVP. Tournaments require lobby-side state; running MiSTer-only tournaments on their server would also pollute.
- **Leaderboard endpoints** (`/leaderboard`, `/player/:id/stats`) — we can READ these to show their global leaderboard as read-only info, but we never WRITE. Defer.

### 8.5 Courtesy and coordination with 3sxtra (REVISED 2026-04-20 per §15 #9, #10)

**User decision:** No formal coordination step. The 3sxtra author already knows the user intends to connect MiSTer clients. Proceed with the port without waiting for explicit OK.

**Behavior if author later objects:** fall back to Plan B self-hosting (§8.7). Hard-coded fallback URL is config-switchable via `CFG_KEY_LOBBY_SERVER_URL` so a client update isn't the only escape path.

**No ongoing coordination channel** set up (Discord DM, issue, etc.) per #10. Monitor passively: if 3sxtra's server becomes unreachable or behavior changes unexpectedly, treat as a Plan B trigger.

### 8.6 Ongoing risks with shared lobby

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| 3sxtra's Oracle Cloud instance goes down (free tier reclaim, account issue, etc.) | **Low-Medium** (evidence-based, see §8.6.1) | High (all MiSTer netplay offline) | Self-host contingency ready (§8.7); the Node.js process servicing HTTP+SSE will keep CPU above Oracle's idle-reclaim threshold under typical load |
| 3sxtra IP changes (no stable domain) | Medium | Medium (clients stuck on old IP) | Publish a well-known DNS record we control (e.g., `lobby.3s-mister.example.com`) that currently CNAMEs to their server, switch it if they move |
| Author revokes access (policy change, traffic concerns) | Low | High | Self-host contingency |
| Protocol drift (3sxtra rev lobby API) | Medium | Medium | Pin to a known version; watch for their changes; coordinate through §8.5 channel |
| Desktop user finds `[MiSTer]` room and mis-matches | Medium | Low (handshake reject catches it) | Layer 3 is authoritative; user-facing error is clear |
| Our traffic pollutes their visible room list | Low | Low (visual UX only) | `visibility=1` option available if it becomes a problem |

### 8.6.1 Oracle Always Free reclaim — evidence-based assessment (NEW 2026-04-20)

Research to replace the prior unqualified "Medium" likelihood in R-LOBBY-1.

**Oracle's documented policy** ([Always Free Resources](https://docs.oracle.com/en-us/iaas/Content/FreeTier/freetier_topic-Always_Free_Resources.htm)): "Idle Always Free compute instances may be reclaimed. Oracle deems an instance idle if during a 7-day period the 95th-percentile CPU utilization is less than 20% AND network utilization is below thresholds."

**Key nuance:** reclamation applies to IDLE instances, not running workloads. A Node.js HTTP+SSE lobby that ticks on the match-timeout reaper (`lobby-server.js:82+ setInterval` cadence — verified via Grep: `setInterval` appears multiple times for cleanup/heartbeat loops) will generate continuous low-intensity CPU + non-zero network. This is likely enough to avoid idle reclamation even with zero active players.

**Community / third-party reports:**
- [LowEndTalk "Oracle OCI Always Free VPS"](https://lowendtalk.com/discussion/182644/oracle-oci-always-free-vps-why-anything-else/p3) — multi-year uptime reports on Ampere A1 Always Free; reclamations mostly reported for truly idle shells, not long-running daemons.
- [ServeTheHome](https://www.servethehome.com/oracle-cloud-giving-away-ampere-arm-a1-instances-always-free/) — original launch coverage; service continues.
- [Ampere community forum](https://community.amperecomputing.com/t/anyone-familiar-with-oracle-cloud-free-tier/565) — 2025-era confirmations that Always Free continues for active workloads.

**Specific to 3sxtra:** their instance at `152.67.75.184` has been live through at least February-April 2026 (verified via external probes in research doc §3.1 and plan §8.1). That's the lobby's production lifetime observably stable. Whether 3sxtra's account is paid vs. Always Free is unknowable externally.

**Revised likelihood rating: Low-Medium.** The combination of "Node daemon generates continuous CPU" + "observed multi-month uptime" + "active community reports of similar workloads surviving on Always Free" puts this below a naive "Medium" but not at "Low" (Oracle's policy can still bite, and account-level issues are outside our visibility).

**Revised risk register entry:** see §10 R-LOBBY-1 — likelihood updated from "Medium" to "Low-Medium" with rationale citing this subsection.

**Residual mitigation:** the self-host contingency recipe in §8.7 stays as written. No action needed unless trigger fires.

Sources:
- [Oracle — Always Free Resources docs](https://docs.oracle.com/en-us/iaas/Content/FreeTier/freetier_topic-Always_Free_Resources.htm)
- [Oracle Cloud Free Tier FAQ](https://www.oracle.com/cloud/free/faq/)
- [ServeTheHome — Ampere A1 Always Free launch](https://www.servethehome.com/oracle-cloud-giving-away-ampere-arm-a1-instances-always-free/)
- [LowEndTalk Oracle OCI Always Free VPS thread](https://lowendtalk.com/discussion/182644/oracle-oci-always-free-vps-why-anything-else/p3)
- [Ampere community forum](https://community.amperecomputing.com/t/anyone-familiar-with-oracle-cloud-free-tier/565)

### 8.7 Archive — self-host recipe (contingency)

Retained for the Plan B scenario if 3sxtra declines or disappears.

**Provider (Plan B default):** Oracle Cloud Infrastructure Always Free tier — same choice 3sxtra made. 4× Arm Ampere cores + 24 GB RAM, permanently free. Matches architecture parity (Ampere is aarch64 — same arch as our desktop-dev build at least). Alternative: Hetzner CX11 ~$5/mo if Oracle free tier is unavailable.

**Stack** (copied verbatim from 3sxtra's `tools/lobby-server/`):
- Node.js 18+ (per `package.json:16`)
- `lobby-server.js` (3234 LOC)
- Three npm deps: `bad-words ^4.0.0`, `better-sqlite3 ^11.0.0`, `geoip-lite ^1.4.10`
- SQLite DB (`./lobby.db`, schemas `players_db`, `matches`, `pending_results`)
- systemd unit: `lobby-server.service` (per `tools/lobby-server/lobby-server.service` — verified)

**TLS + domain:** Let's Encrypt via nginx; dedicated subdomain on a domain we already own.

**Backup:** Daily SQLite `.backup` + zstd + rclone to Backblaze B2 (~$0.01/mo for <1 GB). Retention 14d+8w+6m.

**Ops runbook:** `docs/lobby-server-runbook.md` (deferred — only written if we activate plan B).

**Rough cost (if activated):** $0 (Oracle free) or ~$5/mo (Hetzner). Time cost: ~1 week to provision, deploy, and document.

**Triggers for plan B activation:**
- 3sxtra server unreachable for >24h with no known maintenance
- Author revokes access
- Protocol incompatibility we can't work around
- MiSTer cohort outgrows small shared usage (unlikely but possible)

### 8.8 Legal / privacy posture

Drastically simpler under shared-lobby direction:
- **No user data stored on our infrastructure.** 3sxtra's lobby stores player_id, display_name, chat, match results, IP-derived region.
- **Our disclosure to users:** "Online features use a shared lobby provided by the 3sxtra project. By using online features, you agree to 3sxtra's data practices. We do not operate the lobby server. Your display name and match participation are visible on the shared lobby." Document in release notes + `README-netplay.md`.
- **GDPR:** 3sxtra's operator is the data controller; we link to their contact/deletion process (ask Daouid for deletion request channel).
- **ToS / CoC:** 3sxtra's rules apply. We reference them; don't invent our own.

If we ever activate plan B (self-host), §8.8 expands back to the full GDPR posture documented in this doc's pre-2026-04-20 version (git history preserves it).

### 8.9 Security posture under netplay (NEW 2026-04-20)

Research on MiSTer attack surface with netplay enabled, to fill a gap flagged during audit.

**Listening ports (verified via `/tmp/3sxtra/src/netplay/netplay.c` + `stun.c` + `lobby_server.c`):**
- One UDP listener for GekkoNet + chat on `local_port` (default 7001 per `CFG_KEY_NETPLAY_PORT`). Can be STUN-bound or SDL3_net-bound.
- No TCP listener on our side. Lobby server uses outbound HTTP+SSE (curl-style client initiates).
- LAN discovery sends/receives UDP broadcast on port 7999 (`src/netplay/discovery.c`).

Summary: exactly ONE UDP port open to internet during active netplay; closed otherwise.

**Deserializer attack surface (GekkoNet):**
- `zpp::serializer` ([reference](https://github.com/eyalz800/serializer)) — header-only C++ library handling wire deserialization in `GekkoLib/include/net.h:38-49` and throughout `backend.cpp`.
- No published CVEs against `zpp::serializer` as of 2026-04-20 (WebSearch confirmed no matches). No documented fuzzing campaigns. Library uses standard C++ stream primitives; not known to be memory-unsafe but not fuzz-audited either.
- Attack vector: a malicious peer crafts a malformed `InputMsg` / `SessionHealthMsg` packet on the game port, targeting deserializer logic. Since MiSTer runs as root (see project memory `reference-mister-network-stack.md`), a RCE here is high-severity.
- Mitigation layers:
  1. Layer 3 `MIST` handshake (§8.2.4) rejects unpaired peers before GekkoNet deserialization begins, so a drive-by scanner can't reach the deserializer unless it first passes the handshake. This is NOT a cryptographic authentication — an attacker who observes our lobby traffic can spoof MIST — but it raises the bar from trivial.
  2. `CFG_KEY_NETPLAY_PORT` defaults to an ephemeral value; document that users expose ONLY this one UDP port in their router config (avoid "DMZ" / open-all).
  3. Netplay runs only during active sessions, not continuously. Binary should not listen outside of explicit Online menu navigation.

**Comparison to peer emulator netplay:**
- RetroArch netplay listens on TCP 55435 + UPnP by default ([docs.libretro.com](https://docs.libretro.com/guides/netplay-faq/)); history of protocol-level issues across cores ([GitHub issue #10146](https://github.com/libretro/RetroArch/issues/10146)). RetroArch typically runs as non-root; MiSTer is worse-case.
- Dolphin / CEmu use their own protocols; no public CVEs found specifically against netplay deserialization.
- General OWASP-category concern: [Insecure Deserialization](https://owasp.org/www-community/vulnerabilities/Insecure_Deserialization) applies to any binary protocol accepting untrusted input.

**Hardening posture (recommend, not all adopted for MVP):**
- MVP-in-scope:
  - Document recommended firewall / router config in `docs/netplay-user-guide.md`: forward only one UDP port, don't DMZ.
  - Keep `CFG_KEY_NETPLAY_ALLOW_CROSS_PLATFORM=false` default (§8.2) — limits the set of peers that reach our deserializer.
  - Tighten `NetTuning_SetRecvBuf` sizing to avoid unbounded buffer growth under flood.
- Post-MVP / research deferred:
  - Input-length validation at the `sdl_net_adapter.c:receive_data` layer before handing to GekkoNet. Cap at e.g. 4 KB per datagram.
  - Dropping `CAP_SYS_NICE` after thread setup — not critical since kernel capabilities on MiSTer's stock image are not sandboxed.
  - Running netplay in a non-root child process — architecturally invasive; out of MVP scope.
- Out of scope:
  - Fuzzing campaign against GekkoNet `zpp::serializer`. Valuable but belongs upstream with GekkoNet maintainers.

**New risk register entry (see §10 R-SEC-1 below):** attack surface from malicious peer via GekkoNet deserializer with root privileges. Probability low (requires observed lobby state + crafted packets + handshake bypass). Impact critical if triggered (RCE as root).

Sources:
- [OWASP — Insecure Deserialization](https://owasp.org/www-community/vulnerabilities/Insecure_Deserialization)
- [RetroArch netplay FAQ](https://docs.libretro.com/guides/netplay-faq/)
- [RetroArch netplay protocol improvement thread](https://github.com/libretro/RetroArch/issues/10146)
- [`zpp::serializer` GitHub](https://github.com/eyalz800/serializer) — no CVEs published
- [GekkoNet](https://github.com/HeatXD/GekkoNet) — no CVEs published

---

## 9. Cross-cutting concerns

### 9.1 CI changes needed

- `ci/` or GitHub Actions additions:
  - Desktop build with `ENABLE_NETPLAY=ON`, `ENABLE_RMLUI=ON`.
  - MiSTer cross-build identical flags (after phase 4).
  - Unit tests: the 11+ existing netplay tests (research doc §3.6) plus any new we add.
  - Smoke test: spin up local HTTP lobby server in CI, connect 2 test-client binaries, play 60 frames, assert zero desync events.
- `build-deps.sh mister` must stay idempotent; CI should verify by calling twice.
- Docker cross-compile container must have `cmake >= 3.10` (already has per existing MiSTer builds).

### 9.2 Testing strategy

- **Unit:** existing 11+ tests plus new ones in phase 1 (struct-size round-trip), phase 3 (focused checksum round-trip, sanitize_work_rendering fuzz), phase 12 (Glicko-2 math reference against `__test_tournament.js`).
- **Integration:** two-peer smoke harness in CI.
- **On-device:** phase 9 two-MiSTer pair; phase 10 internet.
- **Stress:** intentional latency/jitter via `tc qdisc` in phase 9; rollback depth stress test to confirm 800 MHz behavior (research doc §17).

### 9.3 Release pipeline impact

- Deploy artefacts grow: `assets/ui/*` adds ~620 KB; binary adds ~4-5 MB (research doc §12.6 — caveat: arm armhf likely 10-15% larger than arm64 macOS).
- `release-readme.txt` (per `feedback-release-readme-path.md`) must mention: requires wired ethernet recommended; WiFi works but jittery; lobby server URL; privacy note.
- Release naming: per `project-release-naming.md`, dated tags only, no "0.3.0 NETPLAY" marketing.

### 9.4 Documentation updates

- `docs/mister-runbook.md`: add networking prerequisites (wired eth recommended; `/etc/sysctl.d/50-netplay.conf` + init.d hook to apply at boot since Buildroot's BusyBox init does NOT run `sysctl -p` automatically — verified 2026-04-20), lobby URL, firewall guidance.
- `docs/building.md`: new `ENABLE_NETPLAY`, `ENABLE_RMLUI`, `ENABLE_NETPLAY_*` flags documented.
- `docs/config.md`: new `CFG_KEY_NETPLAY_*` keys.
- Conditional: `docs/lobby-server-runbook.md` — only written if Plan B self-host activates per §8.7. Matches §11.3 conditionalization.
- New: `docs/netplay-user-guide.md` (player-facing: how to join a room, what a good ping looks like, what to do if peers desync).

### 9.5 Memory files to update

- `reference-mister-network-stack.md`: update Buildroot version string, `rmem_max` cap, missing utilities list, shared libs list. Post-phase-9.
- New `project-netplay-port.md`: tracks progress of this plan; points to the tier-3 sub-plans as they are written.
- New `reference-lobby-server.md` (only if Plan B self-host activates per §11.3): ops URL, hostname, contact email for incidents. Under the shared-lobby direction this file is not written — the 3sxtra URL is the only datum we need, and it lives in `CFG_KEY_LOBBY_SERVER_URL` default + release notes.

---

## 10. Risk register

Ranked by `likelihood × impact`. Each risk references the phase where it first surfaces and the owner workstream. Research-doc risks §19 integrated.

| # | Risk | Likelihood | Impact | Mitigation | Surfaces in | Owner |
|---|---|---|---|---|---|---|
| 1 | **R-9.1 / R-DS-1: On-device first desync (combinatorial root cause)** | High | Critical | Ship phase 3 focused checksum unconditionally; keep debug dump pipeline; methodical bisection — bring the 33 GameState fields online in groups of 5, playtest between each | Phase 9 | WS-Engine |
| 2 | **R-10.1 / R-NAT-1: symmetric NAT on user's router blocks STUN hole-punch** | Medium | High | Fall back to direct-IP + router port-forward instructions; document UPnP workaround; explicitly NOT adding TURN relay in MVP | Phase 10 | WS-Engine + WS-Ops |
| 3 | **R-DS-2: `combo_type`/`remake_power` storage mismatch vs 3sxtra (research doc §19 risk 1)** | High | High | Explicit whitelist entry in focused checksum; unit test for damage-scaling scenario; NO PLW refactor | Phase 3 | WS-Engine |
| 4 | **R-2.1 / R-DS-3: silent symbol mismatch in 234-line setup_vs_mode port** | Medium | High | Line-by-line audit in tier-3 plan; compile-time checks; phase 9 smoke catches many | Phase 2 | WS-Engine |
| 5 | **R-3.1 / R-DS-4: focused-checksum whitelist misses a field that's now drifting (research doc §19 risk 5)** | Medium | High | Ship checksum in release; add unit test that flips known fields and confirms detection | Phase 3 | WS-Engine |
| 6 | **R-7.1 / R-BUILD-1: GekkoNet transitive `asio.hpp` on ARMv7 cross-build** | Medium | Medium | `NO_ASIO_BUILD=ON` already set; grep product headers for `asio`; patch with empty stub if needed | Phase 7 | WS-Ops |
| 7 | **R-5.1 / R-UI-1: RmlUi SDL blend mode visually wrong on MiSTer despite subclass** | Medium | Medium | First-light prototype in phase 5 before committing to phase 6; fall back to solid-color UI (no alpha gradients) if needed | Phase 5 | WS-UI |
| 8 | **R-6.1 / R-UI-2: `menu_network.c` pulls in more engine-refactor surface than scoped** | High | Medium | Accept ripple (add menu_draw/input/internal to port); alternate path is hand-rewrite at our menu renderer — worse per research doc §11.7 | Phase 6 | WS-UI + WS-Engine |
| 9 | **R-DS-5: hitcheck "turbo" optimization discrepancy (research doc §19 risk 3)** | Low | Critical | Do NOT cherry-pick; keep our hitcheck untouched | — (ongoing guard) | WS-Engine |
| 10 | **R-LOBBY-1 (REVISED): 3sxtra shared lobby unavailable (outage / IP change / author revoked access)** | **Low-Medium** (per §8.6.1 evidence-based review) | High | Self-host contingency recipe ready in §8.7; hardcode fallback URL in a way that's config-switchable via `CFG_KEY_LOBBY_SERVER_URL`; monitor passively — §15 #10 defers any ongoing coordination channel | Phase 10+ | WS-Lobby |
| 11 | **R-XARCH-1 (NEW): accidental cross-arch match (MiSTer ↔ desktop) reaches rollback and desyncs** | Medium | High | Three-layer defense per §8.2 (room prefix + arch tag + MIST handshake reject); handshake is authoritative and runs before GekkoNet input exchange | Phase 10 | WS-Lobby + WS-Engine |
| 12 | **R-PERF-1: 8-frame rollback exceeds 16.67ms budget on 800MHz (research doc §17.2)** | Medium | Medium | Reduce `input_prediction_window` 8 → 4-5 (research doc §17.4); document 1200 MHz overclock as recommended | Phase 9 | WS-Engine |
| 13 | **R-POLICY-1 (NEW): 3sxtra data pollution via misdirected POST to `/match_result` or `/match_result/replay`** | Low | High (reputational + author relationship) | Compile-time guard + regression test that matches of these POSTs find zero call sites; tcpdump spot-check during QA | Phase 10+ | WS-Lobby + WS-Ops |
| 14 | **R-DS-6: EffectState typedef move from netplay.c to game_state.h introduces ABI drift** | Low | Medium | Exact move + verify sizeof(State) identical; phase 1 sub-task catches | Phase 1 | WS-Engine |
| 15 | **R-SCOPE-1: Phase 12 scope creep** | High | Medium | Phase 12 now smaller (chat + read-only views; spectator deferred to Phase 13 per §15 #6; ranked/tournament/replay-upload deferred per §12.2). Split remaining into sub-phases if needed | Phase 12 | All |
| 16 | **R-PROTOCOL-1 (NEW): 3sxtra lobby API changes break our client** | Medium | Medium | Pin to a verified commit of their `lobby-server.js`; watch their repo; coordination channel per §8.5 | Phase 10+ | WS-Lobby |
| 17 | **R-HW-1: second MiSTer unit unavailable for phase 9** | Medium | Medium | Option A: user acquires one. Option B: skip phase 9 → phase 10 direct (MiSTer + desktop peer — but only the MiSTer side validates, desktop gets arch-rejected). | Phase 9 | — |
| 18 | **R-SEC-1 (NEW 2026-04-20): malicious peer RCE via GekkoNet `zpp::serializer` deserializer; MiSTer runs as root** | Low (requires MIST bypass + crafted exploit) | Critical (RCE as root) | Layer 3 handshake (§8.2.4) raises bar; firewall guidance; post-MVP: per-datagram size cap at `sdl_net_adapter.c:receive_data`. See §8.9. | Phase 10+ | WS-Engine + WS-Ops |

Research doc §19's Risk 4 (`select_timer_state` module) is treated as R-1.2 and is low-likelihood now that we're explicitly porting the module in phase 1.

Former R-10 (VPS compromise), R-11 (moderation burden), R-13 (plaintext chat TLS gap), R-COST-1 (VPS cost escalation) all REMOVED from main register under the shared-lobby direction. They return if plan B (self-host) activates; archived in §8.7.

---

## 11. Acceptance criteria for "MVP done"

Phases 1–11 define MVP. Phase 12 is post-MVP.

### 11.1 User-facing features (REVISED 2026-04-20 per §15 decisions)

- [ ] Lobby join + room creation from single MiSTer succeeds against 3sxtra's lobby.
- [ ] Layer 3 `MIST` handshake correctly REJECTS a desktop peer with clear user-facing error (authoritative cross-arch guard validated).
- [ ] Two-desktop match over internet via 3sxtra lobby completes (exercises our ported `lobby_server.c` + STUN + GekkoNet rollback on desktop builds — no MiSTer involvement).
- [ ] UPnP automatic port forward (opt-in) on supported routers.
- [ ] Casual room creation + join + start match flow (the RmlUi lobby screens from phase 6), restricted to MiSTer-only peers via Layers 1/2/3.
- [ ] Netstats overlay visible during matches.
- [ ] In-match desync is detected and the match ends cleanly (vs silently corrupting).
- [ ] Rematch flow resets state cleanly (no stale from previous match).
- [ ] MiSTer-vs-MiSTer LAN + internet matches — **DEFERRED** pending second-MiSTer availability (not a MVP blocker per decision #1).

### 11.2 Developer-facing quality bar

- [ ] `tools/mister/build-game.sh --flavor telemetry` succeeds green with `ENABLE_NETPLAY=ON`, `ENABLE_RMLUI=ON`.
- [ ] All existing unit tests + new phase-1/3 tests pass in CI.
- [ ] No P0 desync: 10 consecutive 300-frame LAN matches, 0 detected desyncs.
- [ ] 60 FPS steady in `show-fps` overlay during netplay on MiSTer @ 800 MHz (normal gameplay).
- [ ] No `/match_result` or `/match_result/replay` POSTs leak to 3sxtra server (tcpdump or log audit).
- [ ] Automated regression check confirms Layer 3 `MIST` handshake rejects mock-desktop opponent.
- [ ] Release candidate tag builds, deploys, and passes the on-device smoke in §11.1.

### 11.3 Documentation complete

- [ ] `docs/mister-runbook.md` updated with network prereqs.
- [ ] `docs/building.md` flags documented.
- [ ] `docs/netplay-user-guide.md` written — includes "uses 3sxtra shared lobby; MiSTer-only matchmaking; contact for coordination" disclosures.
- [ ] `reference-mister-network-stack.md` memory file updated with verified 2026-04-20 facts.
- [ ] (Only if self-host contingency activated) `docs/lobby-server-runbook.md` written.

---

## 12. Deferred / out-of-scope

Aligns with research doc §23 and expands on lobby-side deferrals:

### 12.1 Game-side deferrals (post-MVP)

- **NotoSansJP + NotoEmoji fonts** (research doc §12.6: adds ~4.3 MB + 1.9 MB). English-only UI for MVP.
- **Training mode integration** (research doc §9.1: 8 new files in `Game/training/`). Not blocking netplay; defer.
- **Input display during netplay** (RmlUi-only, `input_display.rcss/rml`): ship if trivial post-phase-6; otherwise defer.
- **F10 diagnostics panel** (research doc §11.5): RmlUi screen, low value without ping history aggregation; defer.
- **Toast notifications** (research doc §11.5): RmlUi-only; defer.
- **QR code join** (research doc §23): requires `qrcodegen` vendored lib; defer unless trivial.
- **CLI direct-connect shorthand `3sx 1 192.168.1.100`** (research doc §11.8): +~20 LOC in cli_parser.c; easy win post-MVP.

### 12.2 Lobby-side deferrals (revised for shared-lobby direction)

- **Ranked play (Glicko-2 match results)**: deferred because we can't WRITE to 3sxtra's ranked ladder without polluting their data (§8.4). Contingent on either (a) 3sxtra implementing arch-partitioned leaderboards on their side, or (b) self-hosting activation.
- **Online replay uploads**: same reason. MiSTer users keep local replays only.
- **Tournament hosting** (`bracket.c`): same reason. Requires WRITE access to lobby tournament state.
- **Read-only leaderboard pagination polish**: basic view OK for MVP; rich filtering post-MVP.
- **P2P chat moderation parity with 3sxtra's lobby filter**: MVP accepts unfiltered P2P chat; document limitation.

### 12.3 Infrastructure deferrals (under shared-lobby direction)

- **Self-hosted lobby infrastructure**: not needed for MVP under current direction. Archive recipe retained at §8.7 for contingency activation.
- **Full crossplay with 3sxtra desktop users**: deferred indefinitely. Requires upstream arch-neutral state serialization — months of engineering on 3sxtra's side. Track as "future collaboration opportunity" rather than a commitment.
- **TURN relay for symmetric-NAT users**: scoped out; document workaround (open port forward manually).
- **Multi-region lobby latency optimization**: moot (we don't operate the lobby).
- **Analytics / metrics**: nothing to collect on our side.

### 12.4 Architectural choices NOT made (per decision locks)

- Do NOT port 3sxtra's `globals/`, `training/`, `stage/bg_*`, `opening_*`, `file_loader`, `font_test`, `game_globals.c` (Decision 2).
- Do NOT wipe our `src/netplay/` (Decision 1).
- Do NOT restructure to non-3sxtra paths (Decision 3).

---

## 13. Unknowns that require prototype-not-research

These cannot be answered by reading code. They surface at specific phases.

| # | Unknown | Surfaces in phase | How to resolve |
|---|---|---|---|
| 1 | RmlUi custom blend-mode visual correctness on SDL3 SW renderer on armhf (research doc §21 #1) | Phase 5 | Ship minimal overlay, eyeball on device |
| 2 | Rollback CPU budget at 800 MHz stock (research doc §21 #2) | Phase 9 | `show-fps` during intentional 4/6/8-frame rollback |
| 3 | USB WiFi jitter on MiSTer with real netplay (research doc §21 #3) | Phase 9 / Phase 10 | Not needed if eth0 primary; measure only if WiFi user reports issue |
| 4 | GekkoNet `asio.hpp` transitive include on ARMv7 (research doc §21 #4) | Phase 7 | Cross-build; grep for asio symbols |
| 5 | SDL3_net internal struct ABI drift risk (research doc §21 #5) | Phase 7 / any update | Pin `SDL3_net` to `92022dc`; break CI if pin drifts |
| 6 | UDP broadcast on port 7999 actually works on MiSTer (research doc §21 #6) | Phase 9 LAN test | Two peers, `tcpdump` confirms |
| 7 | RmlUi overlay render cost at 800 MHz (research doc §21 #7) | Phase 9 / Phase 12 | `show-fps` with lobby UI active vs not |
| 8 | Buildroot sysroot shared-lib static-vs-dynamic for libcurl (new, from 2026-04-20 verification) | Phase 10 | Cross-link test; fall back to static if dynamic causes issues |
| 9 | Whether `nslookup`/`host` works as substitute for missing `getent` on target (new) | Phase 9 / verification scripts | Single `ssh` check |
| 10 | MiSTer Buildroot has no `/etc/sysctl.d/` and no init script invoking `sysctl -p` (verified live 2026-04-20). Concrete hook placement (BusyBox `/etc/init.d/S99netplay` vs `/media/fat/linux/user-startup.sh` vs `_Other/3S-ARM.ini` preload) plus survival across MiSTer OTA / firmware update. | Phase 8 | Prototype the hook on 192.168.1.171; reboot; re-verify `sysctl net.core.rmem_max`. |

---

## 14. Timeline estimate

Assumption: part-time pace (user is also maintaining the main fork + shipping other unrelated work). "Part-time" here = ~3 focused days per calendar week.

### 14.1 MVP (phases 1–11, game-side)

Rebalanced 2026-04-20 for two post-planning effects:
- Shared-lobby pivot (§3.4): Phase 10 loses VPS+TLS+systemd+nginx provisioning (~5 focused days off the non-critical infra lane; no critical-path impact).
- Phase 9 made conditional (§15 #1): if a second MiSTer is unavailable, skip Phase 9's ~5-day budget entirely — the critical path becomes 1 → 2 → 3 → 10 → 11 instead of 1 → 2 → 3 → 9 → 10 → 11.

- Total focused effort with Phase 9 included: ~26 days. Without Phase 9: ~21 days.
- Part-time calendar (Phase 9 included): **~9 calendar weeks**, +20% contingency ~11 calendar weeks.
- Part-time calendar (Phase 9 skipped per §15 #1 default): **~7 calendar weeks**, +20% contingency ~9 calendar weeks.

### 14.2 MVP (shared-lobby integration side)

Under the shared-lobby direction (§3.4), infrastructure work collapses to a single task:

- Shared-lobby integration (config default URL, probe script, Layer 1/2/3 wiring): ~1 focused day, absorbed into Phase 10.
- No VPS / TLS / systemd / nginx / backups work. Self-host contingency recipe (§8.7) is only activated on Plan B trigger.

### 14.3 Feature complete (phases 1–12)

- Phase 12 adds 15–25 focused days, but fragmentable.
- Part-time calendar: **+5–8 weeks** after MVP.
- Full total (MVP + feature matrix): **~16–20 calendar weeks**.

### 14.4 Full-time pace (reference only)

- MVP focused effort: ~26 days → **~5 calendar weeks full-time**.
- Full feature matrix: ~46 days → **~9 calendar weeks full-time**.

### 14.5 Milestones

- **End week 2:** Phase 1 + Phase 4 complete. First RmlUi hello visible on MiSTer (phase 5).
- **End week 5:** Phase 3 complete. Focused checksum shipping in desktop Release.
- **End week 7:** Phase 6 content-complete. Navigation works end-to-end in desktop.
- **End week 9:** Phase 9 LAN match works (only if second MiSTer available per §15 #1; otherwise this milestone slides into Phase 10).
- **End week 11 (Phase 9 included) / End week 9 (Phase 9 skipped):** Phase 10 shared-lobby integration verified (Layer 3 handshake rejects desktop; single-MiSTer lobby join + room create works; two-desktop end-to-end via 3sxtra lobby completes). **MVP complete.**
- **End week 16–19 (stretch):** Phase 12 sub-features wrapped per priority.

---

## 15. Decisions locked (resolved 2026-04-20)

All 15 open questions resolved. Recorded for implementers.

| # | Question | Decision | Implication |
|---|---|---|---|
| 1 | Second MiSTer for phase 9? | **NO** | Skip phase 9 as originally scoped. Phase 10 becomes first cross-machine validation; use desktop peer to validate Layer 3 handshake REJECTION (not a successful match). MiSTer-vs-MiSTer validation deferred until a second unit is available. |
| 2 | `rmem_max` sysctl hook? | **Preserve — ship the hook** | MVP installs `/etc/sysctl.d/50-netplay.conf` that bumps `net.core.rmem_max=524288` at boot. Document in `docs/mister-runbook.md`. Phase 8 sub-task. |
| 3 | 1200 MHz overclock default? | **Recommend, don't force** | Release notes + netplay user guide strongly recommend 1200 MHz overclock. Binary runs correctly at 800 MHz stock; no auto-overclock at install time. |
| 4 | `input_prediction_window` default? | **Config key** | Add `CFG_KEY_NETPLAY_INPUT_DELAY_WINDOW` (default 8 per GekkoNet). Document in `docs/config.md` that users on stock 800 MHz may prefer 4-5 for fewer frame drops at cost of slightly more input delay. |
| 5 | Fonts? | **English only for MVP** | Ship BoldPixels (160 KB) only. NotoSansJP / NotoEmoji post-MVP. |
| 6 | Spectator mode? | **Last — post-MVP** | Spectator paths coded but gated off in MVP. Enable in a post-MVP phase 13 (renamed from current phase 12 spectator scope). |
| 7 | TURN relay for symmetric NAT? | **Not in MVP** | Document limitation in release notes: "Users behind symmetric NAT / double-NAT / CGNAT may not be able to connect. This matches 3sxtra's behavior since we share their lobby." |
| 8 | `CFG_KEY_*` naming convention? | **`CFG_KEY_NETPLAY_*` prefix** | Matches research doc §18.4 suggestion. |
| 9 | 3sxtra author coordination? | **Not required; author already knows user's intent** | Skip formal outreach step. Proceed without waiting for explicit OK. If author later objects, fall back to Plan B (§8.7). |
| 10 | Coordination channel? | **Defer** | Not important right now. |
| 11 | Layer 2 arch tag encoding? | **`display_name` suffix `" [MiSTer]"`** | Verified 2026-04-20: lobby-server.js destructures fixed field sets at `/presence` (lines 2679-2710) and `/searching/start` (lines 2713-2727); extra JSON fields are stripped. Encode in `display_name` only. See §8.2.5. |
| 12 | Own domain for stable lobby URL? | **No — hardcode 3sxtra IP** | Hardcode `152.67.75.184:3000` as `DEFAULT_LOBBY_URL`. Accept the risk that if 3sxtra moves IP, users need a client update. |
| 13 | Plan B self-host trigger? | **Defer** | Not important right now. Re-evaluate if and when 3sxtra server issue surfaces. |
| 14 | User-facing lobby disclosure wording? | **Defer** | Not a MVP blocker. Write during release-notes phase. |
| 15 | Show 3sxtra global leaderboard to MiSTer users? | **Yes — ship as-is, no MiSTer-specific customization** | Port `rmlui_leaderboard.cpp` with phase 6 block; no custom overlay / filter / warning. MiSTer matches naturally absent since we don't POST `/match_result`. |

### Implications for phase structure

Based on #1 and #6, phase numbering adjusts:

- **Phase 9 (LAN match on two MiSTers) — DOWNGRADED**: Mark as "conditional / skip unless second MiSTer available". Not blocking.
- **Phase 10 — validation criterion changed**: instead of "MiSTer-vs-MiSTer over internet", validation is:
  - Two desktops over internet (lobby + rollback working end-to-end on non-MiSTer)
  - MiSTer + desktop: Layer 3 `MIST` handshake correctly REJECTS the desktop peer with clear user-facing error (authoritative cross-arch guard working)
  - Single-MiSTer lobby join + room creation works
- **Phase 12 — scope reduced**: Remove spectator from phase 12 (now phase 13+). Leaderboard view ported as-is via phase 6 block (no custom MiSTer overlays per #15). Phase 12 becomes "chat + ping_probe + net_detect + read-only replay browsing + leaderboard view (from phase 6 block, no customization)".
- **New Phase 13 (post-MVP)**: Spectator mode, replay browsing (if compatible), 2nd-MiSTer-enabled MiSTer-vs-MiSTer validation, any deferred items.

### Implications for §11 acceptance criteria

Remove "two MiSTer units" requirement from §11.1. Replace with:
- [ ] Layer 3 handshake correctly rejects a desktop peer (validates cross-arch guard)
- [ ] Lobby join + room creation from single MiSTer succeeds
- [ ] Two-desktop match over internet via 3sxtra lobby completes (exercises our ported lobby_server.c + STUN)
- [ ] MiSTer-vs-MiSTer match — deferred, gate on second-MiSTer availability

---

## 16. Appendix — key file:line anchors for implementers

Quick-reference citations that every tier-3 plan should re-verify before using.

### 16.1 Our fork (3sx-mister, HEAD `17ab61e7`)

| Topic | Location |
|---|---|
| Netplay disabled for MiSTer | `CMakeLists.txt:22-28` |
| Stub substitution | `CMakeLists.txt:66-72` (verified live) |
| `ENABLE_NETPLAY` define | `CMakeLists.txt:148-150` |
| Link libs conditional | `CMakeLists.txt:284-289` (verified live) |
| GekkoNet desktop-gate | `build-deps.sh:182-212` (verified live) |
| SDL3_net desktop-gate | `build-deps.sh:218-246` (verified live) |
| `setup_vs_mode` current | `src/netplay/netplay.c:117-147` (verified live) |
| `MenuTask_SetPhase` equivalent | `src/netplay/netplay.c:118` — `task[TASK_MENU].r_no[0] = 5;` (verified live) |
| `plw[].wu.operator` (not 3sxtra's `pl_operator`) | `src/netplay/netplay.c:121-122` (verified live) |
| `#if DEBUG` checksum gate | `src/netplay/netplay.c:240` (verified live) |
| `clean_work_pointers` | `src/netplay/netplay.c:248-285` (verified live) |
| Inline `EffectState` typedef | `src/netplay/netplay.c:42-50` (verified live) |
| `djb2_hash.h` include | `src/netplay/netplay.c:21` (verified live) |
| Netplay_Menu 2-item stub | `src/sf33rd/Source/Game/menu/menu.c:1451` (research doc) |
| Current menu dir contents | 3 files: `menu.c`, `dir_data.{c,h}`, `ex_data.{c,h}` (verified live via `ls`) |
| SDL Video driver override | `vendor/Main_MiSTer/thirdsarm_wrapper.cpp:1818-1820` (research doc) |
| `SDL_RenderGeometry` calls | `src/port/sdl/sdl_game_renderer.c:6876, 8868, 8890` (research doc) |
| ImGui SDL_Renderer | `src/imgui/imgui/imgui_impl_sdlrenderer3.cpp:236` (research doc) |

### 16.2 3sxtra reference (HEAD `a18eae1`)

| Topic | Location | Verified live |
|---|---|---|
| `setup_vs_mode` full | `src/netplay/netplay.c:158-391` | — (research doc) |
| GekkoNet config | `src/netplay/netplay.c:422-438` | — (research doc) |
| Main loop integration | `src/main.c:451-480` | — (research doc) |
| Focused checksum | `src/netplay/game_state.c:1602-1713` | — (research doc) |
| `sanitize_plw_pointers` | `src/netplay/game_state.c:1522-1530` | — (research doc) |
| `sanitize_work_rendering` | `src/netplay/game_state.c:1513-1519` | — (research doc) |
| GameState sizeof tripwire | `src/netplay/game_state.c:67-85` | — (research doc) |
| `select_timer.{c,h}` | `src/sf33rd/Source/Game/select_timer.{c,h}` (152 + 31 = 183 LOC) | verified via `wc -l` 2026-04-20 |
| `menu_network.c` (1764 LOC) | `src/sf33rd/Source/Game/menu/menu_network.c` | verified via `wc -l` |
| `menu_draw.c` (132 LOC) | `src/sf33rd/Source/Game/menu/menu_draw.c` | verified via `wc -l` |
| `menu_input.c` (2586 LOC) | `src/sf33rd/Source/Game/menu/menu_input.c` | verified via `wc -l` |
| `assets/ui/` (620 KB) | `/tmp/3sxtra/assets/ui/` | verified via `du -sh` |
| Lobby server `player_db` schema | `tools/lobby-server/lobby-server.js:126-138` | verified live |
| Lobby server Glicko-2 | `tools/lobby-server/lobby-server.js:477-596` | verified live |
| Lobby server endpoints | `tools/lobby-server/lobby-server.js:1704 (/match_result/replay legacy), 1742 (/room/events SSE), 1790 (/room/state), 1798 (/rooms/list), 1929 (/room/create), 1980 (/room/join), 2016 (/room/leave), 2074 (/room/chat), 2114 (/room/queue/join), 2137 (/room/queue/leave), 2152 (/room/match/accept), 2209 (/room/match/decline), 2228 (/room/match/end), 2679 (/presence), 2713 (/searching/start), 2730 (/searching/stop), 2745 (/searching), 2802 (/match_result), 2929 (/match_result/replay), 3031 (/replays), 3138 (/leaderboard)` | re-verified 2026-04-20 via Grep |
| Lobby server deploy | `tools/lobby-server/deploy.sh` (58 LOC) | verified live |
| Lobby server systemd | `tools/lobby-server/lobby-server.service` (16 LOC) | verified live |
| Lobby server package | `tools/lobby-server/package.json` — `bad-words`, `better-sqlite3`, `geoip-lite`, node>=18 | verified live |
| `ms_*` netplay glue LOC | 9 files = 1639 LOC total | verified via `wc -l` |
| `menu_screen_*.c` LOC | 4 files = 1002 LOC total | verified via `wc -l` |
| `menu_screen.h` | `src/include/port/menu_screen.h` (393 LOC) | verified via `wc -l` |
| STUN hardcoded servers | `src/netplay/stun.c:191-199` | — (research doc) |

### 16.3 RmlUi 6.2

| Topic | Location | Verified live |
|---|---|---|
| SDL backend renderer | `/tmp/rmlui-6.2/Backends/RmlUi_Renderer_SDL.cpp` (207 LOC) | exists (verified via `ls`) |
| Premultiplied-alpha blend | lines 33-38 | verified live |
| `RenderInterface_SDL::BeginFrame` | lines 41-47 | verified live |
| C++14 requirement | `CMake/Utilities.cmake:101` | — (research doc) |
| CMake range | `CMakeLists.txt:3` (3.10-3.27) | — (research doc) |

### 16.4 Live verification record (2026-04-20)

Commands executed and their outputs captured above:

- `uname -a`: `Linux MiSTer 5.15.1-MiSTer #1 SMP Wed Apr 2 20:01:54 CST 2025 armv7l GNU/Linux`.
- `/etc/os-release`: `PRETTY_NAME="Buildroot 2021.02.4"`, `VERSION_ID=2021.02.4`.
- `ldd --version`: `ldd (GNU) 2.31`.
- `sysctl net.core.{r,w}mem_{default,max}`: all 180224.
- `sysctl net.ipv4.ip_local_port_range`: `32768 60999`.
- `which getent pgrep iperf3 nft node`: absent; `which curl`: `/usr/bin/curl`.
- `ls /usr/lib/lib{curl,freetype,ssl,SDL2}*`: all present as shared.
- `ls /usr/lib/lib{GL,EGL,GLES,vulkan,drm,gbm}*`: all absent (research doc §12.2 confirmed).
- `/proc/cpuinfo`: 2× ARMv7 Cortex-A9.

---

## 17. Appendix — glossary of phase-specific acronyms

- **WS-Engine / WS-UI / WS-Lobby / WS-Ops:** workstreams per §4.
- **MVP:** phases 1–11 complete with shared-lobby integration verified (3sxtra's existing production lobby at `152.67.75.184:3000`). Excludes phase 12 feature matrix.
- **Tier-1:** the research doc (`docs/research-3sxtra-netplay-port.md`).
- **Tier-2:** this document.
- **Tier-3:** per-phase detailed plan, written when that phase starts.
- **Focused checksum:** 3sxtra's whitelist-based rollback state hash (research doc §8.1).
- **`ms_*` glue:** `src/port/screens/ms_<screen>.c` — thin C wrappers tying the MenuScreen registry into the RmlUi controllers.
- **MenuScreen registry:** `src/port/menu_screen_registry.c` — the data-driven screen dispatch 3sxtra built.
- **MenuTask phases:** 3sxtra's menu-task state-machine `MTP_*` enum; we explicitly do NOT port it (Decision 2).
- **Setup VS mode:** frame-0 canonicalization function; highest-risk single function in the port (research doc §6).
- **Desync:** peers compute different GameState on the same frame number; match ends or silently corrupts.
- **Rollback:** GekkoNet re-simulates past frames when new remote input arrives late.
- **Prediction window:** max rollback depth (8 default, 4-5 recommended at 800 MHz).
- **STUN:** RFC 5389 endpoint discovery for NAT traversal.
- **UPnP-IGD:** Universal Plug-and-Play Internet Gateway Device; router port-forward automation.
- **SSE:** Server-Sent Events (lobby server → client push channel).

---

## 18. Changelog of this document

- **2026-04-20 v1:** initial plan. Verified against research doc + live MiSTer. Buildroot correction applied. Open questions §15 left for user.
- **2026-04-20 v1.1:** shared-lobby pivot (§3.4) fully propagated; cross-arch crossplay confirmed blocked (see research doc §9.7); Layer 2 demoted from "extra JSON field" to `display_name` suffix (verified via `lobby-server.js:2679-2727`); decisions #1–#15 resolved and propagated into Phase 9 conditional banner, Phase 10 prereqs, Phase 12 spectator deferral; §4.2 rewritten as adjacency list to fix self-contradictory matrix; added §8.2.4 (MIST handshake atomicity) and §8.2.5 (display_name suffix wire format); audit Findings 1-20 applied or disputed with evidence.
- **v1.1 (2026-04-20):** Shared-lobby pivot per §3.4 revision. Cross-arch crossplay confirmed blocked (see research §9.7). Layer 2 demoted to display_name suffix per §8.2. Decisions #1-#15 resolved in §15. Multiple doc sections retrofitted (§2, §3.4, §4, §5 Phase 10, §5 Phase 12, §8, §10 risks, §11, §12, §15).
- **v1.2 (2026-04-20):** Tier-1 research doc audit pass. Added research §9.7 cross-architecture compatibility section. Applied 5 corrections (Buildroot vs Debian, rmem_max=180224 finding, missing on-target utilities, `/dev/net/tun` absence, `setup_vs_mode` globals verified).
- **v1.3 (2026-04-20):** Tracks A/B/C implementation + review + merge to `netplay` branch. Phase 1-5 + 7 code complete. 23 commits above `mister` (3 merge commits + 20 underlying). Two doc conflicts resolved during merge (plan §338/§352/§362-381, research §5.1/§5.3 + §12.5/§12.6/§1354). See §5 phase-status markers.
