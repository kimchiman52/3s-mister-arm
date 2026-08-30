# 3sxtra Netplay Port — Technical Research Document

**Status:** Research only; no code written. Informs planning and implementation decisions for porting 3sxtra's rollback netplay to 3sx-mister (MiSTer ARM target).

**Date:** 2026-04-20

**Target fork:** `kimchiman52/3s-mister-arm` (local: `/Users/sb/Developer/3sx-mister`), branch `mister`, current HEAD `17ab61e7`.

**Reference fork:** `3sxtra/3sxtra`, HEAD `a18eae1` (cloned at `/tmp/3sxtra` during investigation; commit `a18eae19762601098acaf360095c757fde624308` = "Add build_arcade_android workflow to main to enable workflow_dispatch").

**Upstream common ancestor:** `crowded-street/3sx`, main branch HEAD `b84c9980` ("Adjust README (#222)").

**Primary goal:** Port 3sxtra's full internet-capable rollback netplay (GekkoNet + lobby + STUN + UPnP + UI) to MiSTer. LAN-only is explicitly NOT sufficient — the user requires real internet play.

**Every fact in this document is cited with `file:line` or an external URL. Unverified claims are explicitly flagged "REQUIRES VERIFICATION".**

---

## 0. Table of contents

1. TL;DR — go/no-go and headline numbers
2. Fork lineage and current state
3. 3sxtra file inventory
4. Rollback architecture (GekkoNet)
5. Engine coupling — the GameState port surface
6. `setup_vs_mode` — frame-0 canonicalization
7. EffectState and rendering-side state
8. Checksum sanitization
9. Engine divergence — 3sxtra vs upstream
   - 9.7. Cross-architecture netplay compatibility (added 2026-04-20)
10. Audio determinism
11. UI architecture — native vs RmlUi
12. RmlUi feasibility on MiSTer
13. Dependency matrix
14. ARM cross-compile recipes
15. MiSTer platform baseline
16. MiSTer network stack
17. Performance analysis
18. Build-system changes required
19. Top-5 desync risks (ranked)
20. Phased implementation plan
21. Unknowns requiring prototyping (not paper research)
22. On-device verification commands
23. Scope deferrals — what we're NOT doing
24. Sources and references

---

## 1. TL;DR — go/no-go and headline numbers

**Verdict: FEASIBLE. No fundamental blockers.**

| Dimension | Verdict | Evidence |
|---|---|---|
| Engine/rollback port | **~680 LOC + 1 new module** (close sibling, not rewrite) | §5, §6 |
| UI via RmlUi on SDL3 SW renderer | **Works with ~10-LOC blend-mode patch** | §12 |
| Dependency cross-compile | Additive to `build-deps.sh mister` profile | §13, §14 |
| Network stack on MiSTer | Onboard GbE `eth0` via STMMAC; full POSIX sockets | §16 |
| Performance @ 800 MHz stock | Tight in worst-case rollback; 1200 MHz overclock recommended | §17 |

**Total effort estimate:** 30-50 focused developer-days / 8-12 calendar weeks part-time for MVP (LAN + internet via STUN, native-UI + RmlUi lobby). Tournaments, ranked, leaderboards, online replays are additional scope.

**Novelty:** No MiSTer core ships rollback netplay today. This would be the first. No prior art to copy; no prior art saying it's blocked.

---

## 2. Fork lineage and current state

### 2.1 Upstream already has netplay

The premise "port netplay from 3sxtra" was partially wrong — the core netplay is already upstream at `crowded-street/3sx`, merged via these PRs (verified by `git log upstream/main --grep="netplay\|gekko" -i`):

- `0dfb8e9f Add GekkoNet (#90)`
- `3b4e15d7 Netplay basics (#91)`
- `83b6c0cb Netplay improvements (#93)`
- `2f9da8e4 Netplay cleanup (#95)`
- `056658cb Allow connecting to custom IPs in netplay (#114)`
- `c3fdd6a5 Update Gekko (#115)`
- `9e84b3d7 Desync fixes (#120)`
- `13614be9 Finish netplay session (#121)`
- `14e4eac7 Fix consecutive netplay desync (#123)`
- `7cd79304 Update GekkoNet (#128)`
- `19b8c2d6 Client matchmaking poc (#146)`

### 2.2 Our fork already has netplay source (disabled for MiSTer)

`/Users/sb/Developer/3sx-mister/src/netplay/` contains:

| File | LoC |
|---|---|
| `game_state.c` | 1270 |
| `game_state.h` | 645 |
| `matchmaking.c` | 208 |
| `matchmaking.h` | 30 |
| `matchmaking_stub.c` | 27 |
| `netplay.c` | 758 |
| `netplay.h` | 33 |
| `netplay_stub.c` | 48 |
| `sdl_net_adapter.c` | 97 |
| `sdl_net_adapter.h` | 11 |
| **Total** | **3127** |

Plus `src/port/sdl/netplay_screen.c/.h` (76 LoC combined) for SDL integration.

### 2.3 MiSTer build explicitly disables netplay today

- `CMakeLists.txt:22-28` — `PORT_MISTER=ON` flips `_ENABLE_NETPLAY_DEFAULT=OFF` along with ISO import, FFmpeg/ADX, SDL dialogs.
- `CMakeLists.txt:101-107` — when disabled, real files excluded from `GAME_SRC` glob; `netplay_stub.c` + `matchmaking_stub.c` substituted.
- `tools/mister/build-game.sh:147` — always passes `-DPORT_MISTER=ON`.
- `build-deps.sh:182-212` (GekkoNet) and `:218-246` (SDL3_net) — gated to `if [ "$PROFILE" = "desktop" ]`, print "Skipping" for `mister`.

### 2.4 3sxtra relative to our fork

3sxtra is **not** a rewrite of netplay. Both inherit from the same upstream ancestor; 3sxtra evolved additively, our fork evolved toward MiSTer. The `GameState` enumeration preserves identical ancestral ordering and per-module comment structure across both sides (per engine-coupling agent analysis).

---

## 3. 3sxtra file inventory

HEAD `a18eae1` at `/tmp/3sxtra`. GitHub Issues are disabled on the repo (`has_issues: false` per GitHub API), so no issue-tracker signal.

### 3.1 Core netplay C/C++ files

| File | LoC | Role |
|---|---|---|
| `src/netplay/netplay.c` | 1128 | Session lifecycle, GekkoNet driver, state machine |
| `src/netplay/netplay.h` | 70 | Public API |
| `src/netplay/game_state.c` | 1839 | Rollback save/load, checksum, desync dump |
| `src/include/game_state.h` | (large) | `GameState`/`EffectState` typedefs |
| `src/netplay/sdl_net_adapter.c` | 388 | SDL3_net → GekkoNet adapter + OOB chat |
| `src/netplay/sdl_net_adapter.h` | 27 | |
| `src/netplay/discovery.c` | 462 | LAN UDP beacon (port 7999) |
| `src/netplay/discovery.h` | 81 | |
| `src/netplay/stun.c` | 458 | RFC 5389 STUN client |
| `src/netplay/stun.h` | 48 | |
| `src/netplay/upnp.c` | 195 | miniupnpc wrapper (`HAVE_UPNP` gated) |
| `src/netplay/upnp.h` | 39 | |
| `src/netplay/lobby_server.c` | 2229 | HTTP lobby client, HMAC-SHA256, SSE |
| `src/netplay/lobby_server.h` | 438 | |
| `src/netplay/bracket.c` | 650 | Tournament engine (SE, DE, Swiss, RR) |
| `src/netplay/bracket.h` | 117 | |
| `src/netplay/identity.c` | 166 | Stable player fingerprint |
| `src/netplay/identity.h` | 45 | |
| `src/netplay/ping_probe.c` | 381 | Pre-match ping sampling |
| `src/netplay/ping_probe.h` | 75 | |
| `src/netplay/net_detect.c` | 144 | Connection-type detection (wired/WiFi/unknown) |
| `src/netplay/net_detect.h` | 30 | |
| `src/netplay/sha256.c` | 164 | Portable SHA-256/HMAC (no libcrypto dep) |
| `src/netplay/sha256.h` | 44 | |
| `src/netplay/net_tuning.h` | 77 | Socket recv-buffer tuning |

**Total core netplay C: ~9295 LoC across 24 files.**

### 3.2 Native UI layer — SDL

| File | LoC | Role |
|---|---|---|
| `src/port/sdl/netplay/sdl_netplay_ui.cpp` | 1820 | State-machine bridge (NOT a drawable UI — see §11) |
| `src/port/sdl/netplay/sdl_netplay_ui.h` | 241 | |
| `src/port/sdl/netstats_renderer.c` | ~180 | Netstats overlay drawn natively via `SSPutStrPro` |

### 3.3 RmlUi screens (UI layer 2)

C++ controllers plus `.rml`/`.rcss` in `assets/ui/`:

- `rmlui_network_lobby.*` — top-level network menu
- `rmlui_casual_lobby.*` — casual room browser
- `rmlui_tournament_lobby.*`
- `rmlui_ranked_matchmaking.*`
- `rmlui_network_replay_picker.*`
- `rmlui_ingame_chat.*`
- `rmlui_netplay_ui.*` — F10 diagnostics panel
- `rmlui_leaderboard.*`
- `rmlui_player_profile.*`

Approximate C++ LoC (from §11 research):
- `rmlui_wrapper.cpp/h`: 1506 + 169
- `rmlui_network_lobby.cpp`: 1377
- `rmlui_casual_lobby.cpp`: 1053
- `rmlui_tournament_lobby.cpp`: 1210
- `rmlui_ranked_matchmaking.cpp`: 359
- `rmlui_network_replay_picker.cpp`: 548
- `rmlui_ingame_chat.cpp`: 286
- `rmlui_netplay_ui.cpp`: 436
- `rmlui_leaderboard.cpp`: 361
- `rmlui_player_profile.cpp`: 519

**Core netplay RmlUi set: ~7,824 LoC C++.**

Assets: 620 KB `.rml`/`.rcss` + fonts (160 KB BoldPixels sufficient for English; NotoSansJP 4.3 MB + NotoEmoji 1.9 MB optional for i18n).

### 3.4 Legacy native UI — `src/sf33rd/Source/Game/menu/`

- `menu_network.c` (1764 LoC) — the real native network menu, drawn with `SSPutStr2`/`effect_57/61/66_init` (arcade-style pixel-art UI).
- `menu_draw.c`, `menu_input.c`, `menu_internal.c`, `menu_replay.c`, `menu_save.c`, `menu_task_phases.c`, `menu_training.c` (+ constants headers) — complete menu system refactor
- **Our fork does NOT have any of these.** Our fork's menu tree is `src/sf33rd/Source/Game/menu/{menu.c, dir_data.*, ex_data.*}` only.

### 3.5 Node.js lobby server (external, not in game binary)

- `tools/lobby-server/lobby-server.js` (3234 LoC)
- `tools/lobby-server/package.json` — deps: `bad-words`, `better-sqlite3`, `geoip-lite`
- `deploy.sh`, `lobby-server.service` systemd unit
- 6 integration test files + `seed_leaderboard.js`

### 3.6 Unit tests

11+ netplay/GekkoNet test targets per `tests/unit/CMakeLists.txt`: `test_game_state`, `test_netplay_metrics`, `test_netplay_events`, `test_netplay_refactor`, `test_state_differ`, `test_effect_state_persistence`, `test_netplay_oob`, `test_netplay_init`, `test_netplay_catchup`, `test_stun`, `test_netplay_run`, `test_game_state_roundtrip`.

### 3.7 Recent netplay-relevant commits in 3sxtra

(within last 100 commits observed during investigation)

- `6141368 Netplay_RegisterSpectator`
- `76594a8 spectator connect to player`
- `33efd3b spectator take 3`
- `7a1f293 netplay hud and spectator fixes`
- `5b5f84b gekkonet and launcher`
- `cf4c5eb judgement desync fix test`
- `110916c spectator mode and judgement fix for arm64` — **explicit ARM64 desync fix**
- `e0ee408 1 step forward, 2 steps backwards` — author honesty about rough patches

Active, ongoing work. Not brand-new.

---

## 4. Rollback architecture — GekkoNet

### 4.1 Model

GGPO-style peer-to-peer rollback netcode via GekkoNet (https://github.com/HeatXD/GekkoNet). README at `/tmp/3sxtra/README.md:100` states "Built on GekkoNet GGPO rollback netcode." Verified directly in `netplay.c`.

### 4.2 Session configuration

`/tmp/3sxtra/src/netplay/netplay.c:422-438`:

```c
gekko_create(&session, GekkoGameSession);
GekkoConfig config = { ... };
config.num_players            = PLAYER_COUNT;      // = 2
config.input_size             = sizeof(u16);       // 2-byte input per player
config.state_size             = sizeof(State);     // full rollback snapshot
config.input_prediction_window = 8;                // max rollback frames
config.desync_detection        = true;
```

### 4.3 Actor setup

- Local: `gekko_add_actor(session, GekkoLocalPlayer, NULL)` with `gekko_set_local_delay(session, handle, DELAY_FRAMES_DEFAULT)` — `netplay.c:488-489`.
- Remote: `gekko_add_actor(session, GekkoRemotePlayer, &remote_address)` — `netplay.c:491`.

### 4.4 Per-frame loop

`run_netplay()` / `step_logic()` / `process_events()` drive GekkoNet (`netplay.c:663-697`):
- `gekko_update_session()` → returns `GekkoGameEvent**`
- dispatch `GekkoLoadEvent` / `GekkoAdvanceEvent` / `GekkoSaveEvent`

### 4.5 Dynamic input delay

`compute_tuning_from_ping()` at `netplay.c:402-420`: delay 0→5 frames, frame-skip cap 2→5, bucketed by effective RTT (avg + jitter) at 90/150/200/250 ms. Applied once at battle start via `gekko_set_local_delay()` (`netplay.c:754`).

### 4.6 Transport

P2P over UDP. Lobby server is HTTP-only; does NOT relay gameplay packets. Verified at `tools/lobby-server/lobby-server.js:4-8`: "Minimal lobby/matchmaking server for 3SX P2P netplay ... players register presence, mark themselves as 'searching', and exchange STUN room codes to establish P2P connections via hole-punching."

### 4.7 NAT traversal

STUN first, UPnP fallback. `stun.c` implements RFC 5389 Binding Request with XOR-MAPPED-ADDRESS parsing (`stun.c:130-168`). On success the pre-punched socket is passed to GekkoNet via `Netplay_SetStunSocket()` (`netplay.h:58`, `netplay.c:808-814`), then reused at `netplay.c:440-444` (keeping NAT pinhole open). If `stun_socket == NULL` → fallback socket via `NET_CreateDatagramSocket()` (`netplay.c:454-465`).

### 4.8 Main-loop integration

`/tmp/3sxtra/src/main.c:451-480`:

```c
NetplaySessionState current_net_state = Netplay_GetSessionState();

if (current_net_state != NETPLAY_SESSION_IDLE) {
    Netplay_Run();
    NetstatsRenderer_Render();
    Renderer_Flush2DPrimitives();
    current_net_state = Netplay_GetSessionState();
}

if (current_net_state == NETPLAY_SESSION_IDLE || current_net_state == NETPLAY_SESSION_LOBBY) {
    njUserMain();
    seqsBeforeProcess();
    Renderer_Flush2DPrimitives();
    seqsAfterProcess();
}
```

During `TRANSITIONING`/`CONNECTING`/`RUNNING`, `njUserMain()` is not called from main — instead `Netplay_Run()` internally calls `step_game()` (`netplay.c:554-561`) which wraps `njUserMain() + seqsBeforeProcess() + seqsAfterProcess()` and flips `No_Trans` on/off to suppress rendering during rollback replay frames.

### 4.9 Input polling

`get_inputs()` (`netplay.c:496-515`):
- ORs both local controllers (`p1sw_buff | p2sw_buff`)
- Applies local player's P1 button remap via `Remap_Buttons(inputs, &save_w[1].Pad_Infor[0])`
- During rollback replay, simulation uses identity `Pad_Infor` (set in `setup_vs_mode`), so remapping happens once at the network boundary

Inputs fed to Gekko: `gekko_add_local_input(session, player_handle, &local_inputs)` (`netplay.c:599`).

On `GekkoAdvanceEvent`, `advance_game()` (`netplay.c:578-591`) unpacks 2-player `u16` array into game globals `p1sw_0/p2sw_0/PLsw[p][0]` and previous-frame `p1sw_1/p2sw_1/PLsw[p][1]` (ring-buffered `input_history[2][120]`). Then `step_game(render)` runs.

---

## 5. Engine coupling — the GameState port surface

This is the highest-risk area of the port. Any mismatched field = silent desync.

### 5.1 GameState field count

- **3sxtra**: 601 fields in `struct GameState` (`/tmp/3sxtra/src/include/game_state.h:54-780`)
- **Our fork**: 568 fields (`/Users/sb/Developer/3sx-mister/src/netplay/game_state.h:15-640`)
- **Net difference: 36 fields in 3sxtra (33 row-groups when composite rows in §5.3 are not flattened), -3 in ours**

> **Counting note (added during Track A review):** §5.3 below tabulates the
> additions as 31 rows. Two of those rows are composite — "X_Adjust,
> Y_Adjust, X_Adjust_Buff[3], Y_Adjust_Buff[3]" (4 fields) and "yang_ix,
> yang_timer, yang_ix_plus" (3 fields) — so the flattened count is 36, not
> 33. Phase 1 of the netplay port adds 36 fields to `game_state.h`, which
> matches the flattened count. The "33 total" header on §5.3 refers to row
> groups, not distinct fields.

### 5.2 `sizeof(GameState)` tripwires

- 3sxtra (`/tmp/3sxtra/src/netplay/game_state.c:67-85`):
  - 32-bit: 17800 bytes
  - 64-bit: 19376 bytes
  - Tripwire also pins `sizeof(struct _TASK) = 20` (32-bit) / 32 (64-bit)
- **Our fork has NO `_Static_assert`** — confirmed via Grep. Adding one is recommended to catch silent layout drift.

### 5.3 Fields 3sxtra has that we don't (36 fields, 31 row groups)

All but one exist as globals in our engine tree — they're just not currently save/loaded. See §5.1 counting note: the table groups some additions onto a single row.

| Field | Type | Owner module | Exists in our engine? |
|---|---|---|---|
| `select_timer_state` | `SelectTimerState` | `sf33rd/Source/Game/select_timer.c` | **NO — new 3sxtra module, ~150 LOC to port** |
| `bg_disp_off` | `u8` | stage/bg | Yes (`bg.h:100`) |
| `bg_pos[8]` | `BG_POS` | system/work_sys | Yes (`work_sys.c:37`) |
| `bg_prm[8]` | `BackgroundParameters` | system/work_sys | Yes (`work_sys.c:39`) |
| `BgMATRIX[9]` | `MTX` | system/work_sys | Yes (`work_sys.c:42`) |
| `bgPalCodeOffset[8]` | `s32` | stage/bg | Yes (`bg.h:96`) |
| `ck_ex_option` | `_EXTRA_OPTION` | system/work_sys | Yes (`work_sys.c:12`) |
| `cmd_sel[2]` | `char` | engine/plcnt | Yes (`plcnt.h:71`) |
| `end_prm[8]` | `BackgroundParameters` | stage/bg | Yes (`bg.c:41`) |
| `end_w` | `END_W` | ending/end_data | Yes (`end_data.c:9`) |
| `ending_flag` | `u8` | stage/bg | Yes (`bg.c:40`) |
| `fm_pos[8]` | `FM_POS` | system/work_sys | Yes (`work_sys.c:38`) |
| `Gill_Appear_Flag` | `s8` | system/work_sys | Yes (`work_sys.c:35`) |
| `gouki_end_gbix[16]` | `u8` | stage/bg | Yes (`bg.c:42`) |
| `Hnc_Num` | `s16` | ui/sc_sub | Yes (`sc_sub.h:24`) |
| `no_sa[2]` | `char` | engine/plcnt | Yes (`plcnt.h:73`) |
| `rw_bg_flag[4]` | `u8` | stage/bg | Yes (`bg.c:32`) |
| `rw_dat[20]` | `RW_DATA` | stage/bg | Yes (`bg.c:48`) |
| `rw_gbix[13]` | `s32` | stage/bg | Yes (`bg.c:34`) |
| `rw_num` | `u8` | stage/bg | Yes (`bg.c:31`) |
| `rw3col_ptr` | `const u32*` | stage/bg | Yes (`bg.c:43`) |
| `scr_sc` | `f32` | system/work_sys | Yes (`work_sys.c:40`) |
| `Screen_Switch` | `u16` | stage/bg | Yes (`bg.h:98`) |
| `Screen_Switch_Buffer` | `u16` | stage/bg | Yes (`bg.h:97`) |
| `stage_flash` | `s8` | stage/bg | Yes (`bg.c:35`) |
| `stage_ftimer` | `s8` | stage/bg | Yes (`bg.c:36`) |
| `system_timer` | `u32` | system/work_sys | Yes (`work_sys.c:21`) |
| `tokusyu_stage` | `u8` | stage/bg | Yes (`bg.c:33`) |
| `vm_w` | `struct _VM_W` | system/work_sys | Yes (`work_sys.c:10`) |
| `X_Adjust`, `Y_Adjust`, `X_Adjust_Buff[3]`, `Y_Adjust_Buff[3]` | `s32` | system/work_sys | Yes (`work_sys.c:23-26`) |
| `yang_ix`, `yang_timer`, `yang_ix_plus` | `s8`/`s32` | stage/bg | Yes (`bg.c:37-39`) |

### 5.4 Fields we have that 3sxtra doesn't (3)

| Field | Type | Note |
|---|---|---|
| `combo_type[2]` | `ComboType` | 3sxtra moved into PLW as a member (`/tmp/3sxtra/src/include/structs.h:672`). Ours kept as top-level global (`plcnt.c:83`). |
| `remake_power[2]` | `ComboType` | Same — moved into PLW (`structs.h:692`). Ours global (`plcnt.c:84`). |
| `Disp_Input_History` | `u8` | Training-mode flag; 3sxtra has separate `sf33rd/Source/Game/training/` subsystem (8 files, not in our fork). |

### 5.5 GS_SAVE/GS_LOAD macro counts

- 3sxtra: 601 unique `GS_SAVE(...)` calls in `/tmp/3sxtra/src/netplay/game_state.c` (1839 LoC)
- Ours: 568 unique `GS_SAVE(...)` calls in `/Users/sb/Developer/3sx-mister/src/netplay/game_state.c` (1270 LoC)

The 33 missing calls match the 33 missing fields. Backfill: **~70 LoC** (GS_SAVE + GS_LOAD pair per field).

### 5.6 Task struct layout concern

3sxtra enforces `sizeof(_TASK) = 20` on 32-bit (per `_Static_assert` at `game_state.c:67-85`). Our task struct matches this (no `callback_adrs` field). But a future upstream merge could change it silently if tripwire absent. **Action: add our own tripwires.**

---

## 6. `setup_vs_mode` — frame-0 canonicalization

This is "the most important function for initial sync" per its own comment at `/tmp/3sxtra/src/netplay/netplay.c:140-156`.

- **3sxtra**: `netplay.c:158-391` (234 lines)
- **Ours**: `netplay.c:117-147` (30 lines)

### 6.1 What our version does today

Zeros only: `task[TASK_MENU].r_no[0]`, `cpExitTask(TASK_SAVER)`, player operator flags, `Operator_Status`, grade workers, training difficulty, `G_No[1]/[2]`, `Mode_Type`, `E_Timer`, `Deley_Shot_No/Timer`, `Random_ix16/32`, `Clear_Flash_Init`, `clean_input_buffers`.

### 6.2 What 3sxtra additionally resets

All of these globals exist in our engine (verified via Grep into `/Users/sb/Developer/3sx-mister/src/sf33rd/`), except `MenuTask_SetPhase` which is part of 3sxtra's menu refactor.

| Category | Globals force-reset | Required helper | In our engine? |
|---|---|---|---|
| Player/combat zeroing | `SDL_zeroa(plw)`, `SDL_zeroa(zanzou_table)`, `SDL_zeroa(super_arts)` | — | Yes |
| Task timers | `task[i].timer = 0`, `task[i].free` zeroed (i=0..10) | — | Yes |
| Menu task | `MenuTask_SetPhase(MTP_NETPLAY_IDLE)` | **MenuTask_SetPhase** | **NO — replace with `task[TASK_MENU].r_no[0] = 5`** |
| Pause flags | `Pause = 0`, `Game_pause = 0`, `cpExitTask(TASK_PAUSE)` | — | Yes |
| Personal data | `Clear_Personal_Data(0/1)` | exists | Yes |
| Effect reset | `System_all_clear_Level_B()` | exists at `sys_sub.c:951` | Yes |
| Round settings | `save_w[MODE_NETWORK].Time_Limit=99`, `Battle_Number[0/1]=2`, `Damage_Level=0`, `Handicap=0`, `GuardCheck=0` | `save_w` array | Yes |
| Replay flags | `Replay_Status[0/1] = 0`, `cpExitTask(TASK_MENU)` | — | Yes |
| BG scroll | `SDL_zeroa(bg_pos)`, `SDL_zeroa(fm_pos)`, `SDL_zeroa(bg_prm)` | — | Yes |
| Screen state | `Screen_Switch = 0`, `Screen_Switch_Buffer = 0`, `system_timer = 0`, `Interrupt_Timer = 0` | — | Yes |
| Rendering layers | `SDL_zeroa(Order)`, `Weak_PL = 0` | — | Yes |
| Button identity | `save_w[MODE_NETWORK].Pad_Infor[p].Shot` = identity, `Vibration=0` | `Pad_Infor` | Yes |
| First-to-X | `s_negotiated_ft` + `save_w.Battle_Number = 1` | new var | Need to add |
| Input remap | `SDL_zeroa(Check_Buff)`, `SDL_zeroa(Convert_Buff)` | — | Yes |
| Timers | `Game_timer=0`, `Control_Time=0`, `players_timer=0`, `G_Timer=0` | — | Yes |
| Per-player globals | `Champion=0`, `Forbid_Break=0`, `Connect_Status=0`, `Stop_SG=0`, `Exec_Wipe=0`, `Gap_Timer=0`, `SDL_zeroa(E_No)` | — | Yes |
| State-machine | `SDL_zeroa(C_No)`, `SDL_zeroa(SC_No)` | — | Yes |
| Extended RNG | `Random_ix16_ex=0`, `Random_ix32_ex=0`, `Random_ix16_com=0`, `Random_ix32_com=0`, `Random_ix16_ex_com=0`, `Random_ix32_ex_com=0` | — | Yes |
| Round state | `Round_Level=0`, `Round_Result=0`, `SDL_zeroa(PL_Wins)`, `Conclusion_Type=0`, `SDL_zeroa(win_type)` | — | Yes |
| Attract cleanup | `Combo_Demo_Flag=0`, `Select_Demo_Index=0`, `Demo_Stage_Index=0`, `Demo_PL_Index=0`, `SDL_zeroa(My_char)`, `SDL_zeroa(Super_Arts)` | — | Yes |
| Combat flags | `SDL_zeroa(Attack_Flag)`, `Counter_Attack`, `Guard_Flag`, `Flip_Flag`, `Lie_Flag`, `Attack_Counter`, `Bullet_No`, `Bullet_Counter`, `paring_counter` | — | Yes |
| Flow | `VS_Stage=0`, `SLOW_timer=0`, `SLOW_flag=0`, `EXE_flag=0` | — | Yes |
| Gauge/vitality | `SDL_zeroa(piyori_type)`, `Max_vitality=160` | — | Yes |

### 6.3 Effort

~150 new LoC in `setup_vs_mode` + 1 menu-phase line swap (`MenuTask_SetPhase(MTP_NETPLAY_IDLE)` → `task[TASK_MENU].r_no[0] = 5`).

**Nothing our fork has is force-reset in 3sxtra that shouldn't be.** Training-mode state (`Disp_Input_History`, `Training_Index`, `Training_ID`) is in GameState as inert fields.

---

## 7. EffectState and rendering-side state

### 7.1 EffectState is identical, not a 3sxtra invention

3sxtra definition at `/tmp/3sxtra/src/include/game_state.h:44-52`:

```c
typedef struct EffectState {
    s16 frwctr;
    s16 frwctr_min;
    s16 head_ix[8];
    s16 tail_ix[8];
    s16 exec_tm[8];
    uintptr_t frw[EFFECT_MAX][448];
    s16 frwque[EFFECT_MAX];
} EffectState;
```

`EFFECT_MAX = 128`.

**Sizing on 32-bit ARM**: `128 × 448 × 4 = 229,376 bytes + small fields ≈ 224 KB.**
**On 64-bit**: ≈ 458 KB.

### 7.2 Our fork has identical EffectState

Inlined in `/Users/sb/Developer/3sx-mister/src/netplay/netplay.c:42-50` (not in `game_state.h`).

Underlying engine subsystem at `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/effect/effect.h:7-15`:

```c
#define EFFECT_MAX 128
extern uintptr_t frw[EFFECT_MAX][448];
extern s16 head_ix[8];
extern s16 tail_ix[8];
extern s16 frwctr_min;
extern s16 frwctr;
extern s16 frwque[EFFECT_MAX];
```

### 7.3 Serialization equivalence

Both sides do the same `SDL_copya` over the same fields (`3sxtra game_state.c:1463-1470` vs ours `netplay.c:372-380`).

**Zero porting cost for EffectState mechanism itself**; just move the typedef from `netplay.c` to `game_state.h`.

### 7.4 Total State size

`sizeof(State) = sizeof(GameState) + sizeof(EffectState)`:
- 32-bit ARM (MiSTer target): **~17.4 KB + 224 KB = ~241 KB**
- 64-bit: **~18.9 KB + 458 KB = ~477 KB**

Matches the "~247 KB on 32-bit" figure used in earlier performance analysis.

---

## 8. Checksum sanitization

### 8.1 3sxtra's focused whitelist approach

`/tmp/3sxtra/src/netplay/game_state.c:1602-1713` computes a checksum over only deterministic, rendering-independent state:

- `plw_scratch[2]` — sanitized PLW (pointers zeroed, rendering bits masked, `before/behind/myself/listix/timing = 0`)
- All 8 RNG indices: `Random_ix16`, `Random_ix32`, `_ex`, `_com`, `_ex_com` (excludes `_bg` — saved but not checksummed)
- Round state: `Round_num`, `Round_Level`, `Round_Result`, `PL_Wins`, `Conclusion_Type`, `win_type`
- Player identity: `My_char`, `Super_Arts`
- Combat flags: `Attack_Flag`, `Counter_Attack`, `Guard_Flag`, `Flip_Flag`, `Lie_Flag`, `Attack_Counter`, `Bullet_No`, `Bullet_Counter`, `paring_counter`
- Game flow: `Present_Mode`, `VS_Stage`
- Slow motion: `SLOW_timer`, `SLOW_flag`, `EXE_flag`
- Super gauge/stun/vitality: `super_arts`, `piyori_type`, `Max_vitality`

Hash: **djb2** (`djb2_init`, `djb2_update_mem` from `sf33rd/utils/djb2_hash.h`). Not CRC-32 or xxHash — simple, deterministic, portable.

**Active in both DEBUG and RELEASE** (gated only by `battle_start_frame >= 0`).

### 8.2 Our fork's current approach

`/Users/sb/Developer/3sx-mister/src/netplay/netplay.c:241-245`: `calculate_checksum()` hashes the entire sanitized `State` via `djb2_updatep(hash, state)`. That's ~241 KB of bytes — no whitelist.

**Gated behind `#if DEBUG`** at `netplay.c:240,389`. **Our RELEASE ships no desync detection.** Operational gap — see §19 risk #5.

### 8.3 `sanitize_plw_pointers`

3sxtra (`/tmp/3sxtra/src/netplay/game_state.c:1522-1530`):

```c
static void sanitize_plw_pointers(PLW* p) {
    sanitize_work_pointers(&p->wu);
    sanitize_work_rendering(&p->wu);
    p->cp = NULL;
    p->dm_step_tbl = NULL;
    p->as = NULL;
    p->sa = NULL;
    p->py = NULL;
}
```

`sanitize_work_pointers` (lines 1475-1507) zeros 30 pointer fields of WORK: `target_adrs`, `hit_adrs`, `dmg_adrs`, `suzi_offset`, `char_table`, `se_random_table`, `step_xy_table`, `move_xy_table`, `overlap_char_tbl`, `olc_ix_table`, `rival_catch_tbl`, `curr_rca`, `set_char_ad`, `hit_ix_table`, `body_adrs`, `h_bod`, `hand_adrs`, `h_han`, `dumm_adrs`, `h_dumm`, `catch_adrs`, `h_cat`, `caught_adrs`, `h_cau`, `attack_adrs`, `h_att`, `h_eat`, `hosei_adrs`, `h_hos`, `att_ix_table`, `my_effadrs`.

**Our fork's equivalent** at `/Users/sb/Developer/3sx-mister/src/netplay/netplay.c:248-285` is called `clean_work_pointers()` and zeros ALL the same pointer fields. Key divergences:
- Ours also zeros `current_colcd`, `extra_col`, `extra_col_2` **entirely** (not just 0x2000 bit mask).
- Ours does NOT zero `my_col_code`.
- Ours `clean_plw_pointers` additionally zeros `plw->cb` and `plw->rp` which 3sxtra does not (those fields only exist in our PLW).

### 8.4 `sanitize_work_rendering`

`/tmp/3sxtra/src/netplay/game_state.c:1513-1519`:

```c
static void sanitize_work_rendering(WORK* w) {
    w->current_colcd &= ~0x2000;
    w->my_col_code   &= ~0x2000;
    w->colcd         = 0;
    w->extra_col     &= ~0x2000;
    w->extra_col_2   &= ~0x2000;
}
```

3sxtra is surgical (masks palette-side flag 0x2000 only, preserves index). Ours is more aggressive. **If we adopt 3sxtra's focused-checksum verbatim, our more-aggressive sanitizer will mask semantic differences, but the checksum semantics are still correct — neither approach introduces false-positive desyncs.** See risk #2 in §19.

### 8.5 32-bit ARM heap heuristic

Line 1631:

```c
uint64_t* words = (uint64_t*)&plw_scratch[p];
for (i = 0; i < sizeof(PLW)/sizeof(uint64_t); i++) {
    uint64_t v = words[i];
    if (v > 0x100000000ULL && (v >> 47) == 0) {
        words[i] = 0;
    }
}
```

**On 32-bit ARM**: pointers are 32-bit. Reading 64-bit fuses two adjacent 32-bit words. A lone pointer `0x12345678` next to zeros reads as `0x12345678` → condition `v > 0x100000000ULL` is **false** → not zeroed. Two adjacent pointers `[0x40000000, 0x50000000]` little-endian as `0x5000000040000000` → `v > 0x100000000ULL` is true, `(v >> 47) = 0xA0` is NOT zero → not zeroed either.

**On 32-bit ARM the heuristic is effectively a no-op — this is intentional, not a bug.** The design comment at line 1624 confirms: "Use fixed uint64_t stride so both 32-bit and 64-bit platforms scan the same bytes." Explicit pointer fields are already zeroed by `sanitize_plw_pointers`; this sweep catches only stray pointers stashed in `u32*/void*` fields not in the explicit zero-list — a 64-bit-only ASLR concern.

---

## 9. Engine divergence — 3sxtra vs upstream

`diff -rq /tmp/3sxtra/src/sf33rd/ /Users/sb/Developer/3sx-mister/src/sf33rd/`:
- **620 files differ** (content)
- **82 files exist on only one side** (62 only in 3sxtra, 20 only in ours)

### 9.1 3sxtra-only additions

- `Game/select_timer.{c,h}` — new module (~150 LOC), required for `SelectTimerState`
- `Game/globals/` — 5 new files (combo_stage, match, player, score, timer_hud — modular refactor)
- `Game/training/` — 8 new files (training_dummy, training_hud, training_prediction, training_state, trials, trials_data.inc)
- `Game/menu/menu_{draw,input,internal,network,replay,save,task_phases,training}` + 3 related `_constants.h` — complete menu system rewrite
- `Game/io/{file_loader,fs_sys,gd_data}.{c,h}` — file I/O abstraction
- `Game/stage/bg_{load,rewrite,constants}` — BG module refactor
- `Game/opening/opening_bg.c`, `opening_scenes.c` — opening module refactor
- `Game/engine/caldir_data.{c,h}`, `workuser_{combat,score,select,system}.h`, `cmd_constants.h` — new engine sub-modules
- `Game/sound/sound_lookup{,_data}.c`
- `Game/debug/font_test.{c,h}`
- `Game/ending/end_maps.{c,h}`
- `Game/game_globals.c`, `init_task_phases.h`, various `_states.h` files

### 9.2 Our-fork-only additions

- `Game/effect/effa5.{c,h}` (arcade-era effect)
- `Game/rendering/dc_ghost.{c,h}`, `mts_hash.h` — MiSTer MTS optimizations
- `AcrSDK/common/plapx.c`, `plbmp.c`, `plpic.c`, `pltim2.c` — kept audio/SDK files 3sxtra removed
- `Game/game.h`, `init3rd.h` — upstream headers they renamed

### 9.3 RNG determinism

`/tmp/3sxtra/src/sf33rd/Source/Game/engine/pls02.c` has a long netplay doc comment (lines 3-35) describing all 10 indices and their tables. **The RNG functions themselves exist in our fork with identical signatures and identical index names.** No evidence 3sxtra replaced the RNG implementation — they **documented** it, not rewrote it.

Our fork's `pls02.c` (1260 lines) differs only in refactorings and `read_adrs_store_mvxy`/`meri_case_switch` static-ness changes, not in RNG math. Divergence count: 841 lines diff, but no semantic math changes.

### 9.4 Hitcheck "turbo" optimization

`/tmp/3sxtra/src/sf33rd/Source/Game/engine/hitcheck.c:664-674` replaces a 10-way switch with `(0x15 >> p_idx) & 1` bitwise logic. `0x15 = 0b10101` = cases 0/2/4. Semantically equivalent IF the bitmask is correct — risk surface if cases 6,7,8,9 handling differs. **Do not cherry-pick** hitcheck changes — all-or-nothing.

### 9.5 PLW/structs.h divergences

`/tmp/3sxtra/src/include/structs.h` vs `/Users/sb/Developer/3sx-mister/include/structs.h` (345 diff lines):

1. `WORK.pl_operator` (3sxtra) vs `WORK.operator` (ours) — renamed to avoid C++ keyword
2. `PLW.combo_type` and `PLW.remake_power` (3sxtra members) vs top-level globals (ours)
3. `_TASK.func_adrs` signature: `void (*)(struct _TASK*)` (3sxtra) vs `void (*)()` (ours)
4. `_TASK.callback_adrs` (3sxtra only)
5. 3sxtra added `/// @netplay_sync` doc comments on WORK fields
6. 3sxtra extracted `CharState` as a separate typedef (was inline in ours)

### 9.6 Verdict

3sxtra's CPS3 engine is a **documentation-heavy, modularly refactored, but semantically faithful** version of upstream. No RNG determinism replacements. No fixed-point math swaps. No nondeterministic-behavior removals. The `hitcheck.c` branchless trick is the only micro-optimization that could affect simulation if miscoded.

---

## 9.7 Cross-architecture netplay compatibility (ADDED 2026-04-20)

**Definitive verdict: 32-bit vs 64-bit crossplay is NOT feasible in 3sxtra today.** Added after deep investigation triggered by the fork author's ("Daouid") claim that Android 32-bit builds "seemed ok". The claim is factually grounded (32-bit Android builds do exist and run), but does not validate cross-architecture netplay.

### 9.7.1 The kill mechanism

GekkoNet's wire protocol transmits **inputs only**, not state. So peers COULD in principle have different `sizeof(State)` without breaking the input exchange. But `SessionHealthMsg` carries a `u32 checksum` every few frames (`/tmp/GekkoNet/GekkoLib/include/net.h:94-102`, `/tmp/GekkoNet/GekkoLib/src/backend.cpp:678-700`).

That checksum is computed at `/tmp/3sxtra/src/netplay/game_state.c:1642-1643`:

```c
djb2_update_mem(h, (const uint8_t*)&plw_scratch[p], sizeof(PLW))
```

`sizeof(PLW)` differs between 32-bit and 64-bit because WORK embeds ~30 pointer fields (4 vs 8 bytes each) with resulting struct padding differences. The hash inputs are different lengths → outputs cannot match.

`netplay.c:633-651` handles `GekkoDesyncDetected` by calling `Soft_Reset_Sub()` and transitioning to `NETPLAY_SESSION_EXITING`. Cross-arch matches die within seconds.

### 9.7.2 The `_Static_assert` admits it

`/tmp/3sxtra/src/netplay/game_state.c:67-78` literally branches expected size per arch:

```c
#if UINTPTR_MAX == 0xffffffff
#define EXPECTED_GAME_STATE_SIZE 17800     // 32-bit
#else
#define EXPECTED_GAME_STATE_SIZE 19376     // 64-bit
#endif
_Static_assert(sizeof(GameState) == EXPECTED_GAME_STATE_SIZE, ...);
```

The commit that introduced this branch (`cc87f0c` "android port on same codebase initial commit, expect even more bugs than usual") added 32-bit handling precisely so the armeabi-v7a compile wouldn't trip the assert. **The author acknowledges different sizes across archs, just accepts both.** There's no runtime enforcement that peers agree.

### 9.7.3 The 32-bit sweep comment is misleading

`game_state.c:1624-1634`:

```c
// Use fixed uint64_t stride so both 32-bit and 64-bit platforms scan the same bytes
// and produce identical checksums
count = sizeof(PLW) / sizeof(uint64_t);
uint64_t* words = (uint64_t*)&plw_scratch[p];
for (i = 0; i < count; i++) { ... }
```

The comment's claim that "both platforms scan the same bytes" is **false**. `sizeof(PLW)` differs across archs → the loop iterates different numbers of times → scans different buffers. The stride is fixed at 8 bytes but the buffer itself differs.

### 9.7.4 Android CI confirms 32-bit builds exist

`/tmp/3sxtra/android/app/build.gradle:22`: `abiFilters 'arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86'` — four ABIs.
`/tmp/3sxtra/android/androiddeps.sh:30`: `TARGET_ABIS=("arm64-v8a" "armeabi-v7a" "x86_64" "x86")` — third-party deps built for all four.

No `splits.abi { enable true; universalApk false }` → output is a fat APK with all four ABIs. Android 7+ with 32-bit ARM hardware runs the `armeabi-v7a` variant.

**The author's "32-bit Android builds work" claim is about local determinism on 32-bit phones, not cross-arch compatibility with 64-bit desktops.**

### 9.7.5 Zero crossplay mitigations in upstream

- `tools/lobby-server/lobby-server.js`: no arch filtering in pairing (grep for `arch|platform|build_id` → no hits).
- `GekkoNet SyncRequest/Response` (`net.h:85-92`): just a 16-bit magic number. No arch or version info.
- `sdl_netplay_ui.cpp`, `lobby_server.c`, `stun.c`: no `arch|platform|build_hash|build_id` fields.
- `tests/unit/`: no cross-arch tests.
- `README.md`, `GAP_ANALYSIS.md`: no documentation claiming crossplay works.

Commit `110916c` "spectator mode and judgement fix for arm64" fixes arm64-ONLY bugs (sign-compare, RNG selection in effc9.c), not cross-arch compatibility.

### 9.7.6 What crosses the wire vs what stays local

**Per-peer local (can differ across arch):**
- Full `State = { GameState; EffectState }` — saved to `std::unique_ptr<u8[]>(state_size)` buffer
- `config.state_size` — local allocation size
- `save_state`/`load_state` callbacks — same-process memcpy

**Wire (must be identical in semantic meaning):**
- `InputMsg` — `u8` input arrays sized by `_input_size = sizeof(u16) = 2 bytes` (arch-stable)
- `InputAckMsg` — frame + `i8` advantage (arch-stable)
- `SyncMsg` — `u16 rng_data` (arch-stable)
- `NetworkHealthMsg` — RTT (arch-stable)
- **`SessionHealthMsg` — `Frame + u32 checksum`** — wire format stable, checksum VALUE is arch-dependent → **the bug**

### 9.7.7 Niche compatible case

MiSTer (32-bit armhf/clang-20) ↔ 3sxtra 32-bit Android (armeabi-v7a/clang-NDK) would theoretically match because both are armv7 with matching compiler padding rules. Compiler toolchain differences (glibc 2.31 vs Android Bionic) don't affect struct layout since structs are source-determined. Unverified without a live experiment. Niche because MiSTer-vs-Android-32bit is a very small audience.

### 9.7.8 What would enable full crossplay

- Architecture-neutral state serialization — replace `uintptr_t`/pointer fields in GameState/WORK/PLW with fixed-width `u32` identity tokens, strip padding, introduce explicit wire-format snapshot rather than raw memcpy. Significant upstream engineering on 3sxtra's side.
- OR full-state exchange over wire (abandons rollback contract; impractical).
- OR drop `SessionHealthMsg` checksum-based desync detection entirely (masks other bugs).

**None of these are downstream MiSTer fork work.** This would be an upstream negotiation with 3sxtra.

### 9.7.9 Implications for MiSTer port strategy

1. Cross-play with 3sxtra desktop/Pi4/iOS/non-32-bit-Android users: NOT a shipped feature. Defer indefinitely.
2. Shared lobby (3sxtra's `152.67.75.184:3000`) is still usable — just pair MiSTer-only via client-side filters.
3. Matchmaking rules must enforce MiSTer-only at multiple layers (room-name prefix + presence arch tag + GekkoNet socket handshake rejection). See plan doc §8.2.

### 9.7.10 Sources for this section

- `/tmp/3sxtra/src/netplay/game_state.c:1475-1713` — sanitizers and checksum
- `/tmp/3sxtra/src/netplay/game_state.c:67-78` — arch-specific `_Static_assert` (commit `cc87f0c`)
- `/tmp/GekkoNet/GekkoLib/include/net.h:51-112` — wire message bodies
- `/tmp/GekkoNet/GekkoLib/src/backend.cpp:762-875` — input-only send path
- `/tmp/3sxtra/android/app/build.gradle:22` — fat APK ABI filters
- `/tmp/3sxtra/android/androiddeps.sh:30` — four-ABI dep build
- `/tmp/3sxtra/tools/lobby-server/lobby-server.js` — no arch filtering (verified grep)

---

## 10. Audio determinism

### 10.1 Saved audio state

Both forks save only BGM control state:

```
GS_SAVE(BGM_Vol);
GS_SAVE(BGM_No);
GS_SAVE(BGM_Timer);
```

Neither saves `eflSpuMap.c` state, voice channel state, or PCM streams.

### 10.2 Rationale

Audio is a consumer of game state (BGM_No/Timer determine which track) but audio playback pointers/buffers are NOT simulation inputs. Rendering and audio can diverge per peer without desyncing gameplay — they're sinks, not sources. During rollback with `No_Trans=1`, most sound-triggering functions skip via the same guard.

What remains: audio playback that "double-starts" during rollback replay. **3sxtra accepts this as a cosmetic defect.** No audio muting during rollback.

### 10.3 Our fork

Equivalent on audio determinism. No porting work needed.

---

## 11. UI architecture — native vs RmlUi

### 11.1 What "native UI" actually is in 3sxtra

Critical correction: `sdl_netplay_ui.cpp/.h` is misleadingly named. It's NOT a UI — it's a **state-machine bridge**:
- `SDLNetplayUI_DrawNativeHUD(void)` declared at `sdl_netplay_ui.h:26` but **never defined** (grep of `/tmp/3sxtra/src` confirms — only declaration + no-op stub at `sdl_netplay_ui.h:97`).
- Handles STUN/UPnP/hole-punch async state, lobby polling, pending-invite buffers.
- Exposes C getters: `SDLNetplayUI_GetOnlinePlayerName(i)`, `SDLNetplayUI_HasPendingInvite()`, `SDLNetplayUI_GetStatusMsg()`.
- Explicit comments at `:991`, `:1001`, `:1313` confirm ImGui HUD was gutted: `"RenderToasts() removed — replaced by RmlUI netplay overlay"`.

### 11.2 The real native UI: `menu_network.c`

`/tmp/3sxtra/src/sf33rd/Source/Game/menu/menu_network.c` (1764 LOC). Drawn with the game's arcade-style 2D renderer (`Renderer_Queue2DPrimitive`, `SSPutStr2`, `SSPutStr_Bigger`, `effect_57_init`, `effect_61_init`, `effect_66_init`). **Not RmlUi. Not ImGui.**

Entry: `Network_Lobby(struct _TASK* task_ptr)` at `menu_network.c:275`, dispatched from legacy `AT_Jmp_Tbl[21]` via the new `MenuScreen` registry (`ms_network_lobby.c:68`).

Menu surface:

| Path | Lines | Type |
|---|---|---|
| Gateway menu (7 items) | 275-374 | Native |
| Leaderboard view | 379-458 | **RmlUi-only** |
| Network Replays | 463-552 | **RmlUi-only** |
| Lobby (casual/tournament) | 557-1298 | **Hybrid** (branches on `task_ptr->free[2]`: `NET_MODE_RMLUI` vs `NET_MODE_NATIVE`) |
| **LAN-only lobby** | **1305-1687** | **100% native** (3-item menu: AUTO-CONN / CONNECT / EXIT) |
| Profile | 349 | RmlUi-only |
| Popups (incoming/outgoing challenge) | 105-273 | Native |

### 11.3 Our fork lacks `menu_network.c` entirely

Our `src/sf33rd/Source/Game/menu/` has only `menu.c + dir_data.* + ex_data.*`.

**Both UI approaches (native or RmlUi) require porting `menu_network.c` (1764 LOC) regardless.** This was a mistake in earlier analysis that assumed we had a native path already.

Our current `Netplay_Menu(struct _TASK*)` at `menu.c:1451` is a 2-item menu (START MATCHMAKING / EXIT) calling `Netplay_BeginMatchmaking()` + `Netplay_BeginDirectP2P()` from our upstream-era netplay.

### 11.4 RmlUi coupling in netplay core — exhaustive catalog

Only **11 touchpoints** across all netplay files.

**`netplay.c`** (8 sites):

| Line | Call | Stub safety |
|---|---|---|
| 30 | `#include "port/sdl/rmlui/rmlui_casual_lobby.h"` | Header not self-guarded → needs stub |
| 31 | `#include "port/sdl/rmlui/rmlui_wrapper.h"` | Self-guarded at `rmlui_wrapper.h:17,85,135` |
| 32 | `#include "port/menu_screen.h"` | Data-driven screen system (3sxtra-only, but not RmlUi) |
| 33 | `#include "port/sdl/rmlui/rmlui_ingame_chat.h"` | Self-guarded |
| 499 | `if (rmlui_ingame_chat_is_typing()) return 0;` | SAFE stub → returns `false` |
| 825 | `MenuScreenId cur = MenuScreen_GetCurrent();` | Requires MenuScreen API (load-bearing) |
| 836 | `rmlui_wrapper_hide_all_game_documents();` | SAFE stub (self-guarded no-op exists) |
| 1030 | `const char* room = rmlui_casual_lobby_get_room_code();` | SAFE stub → `""` |
| 1035 | `Menu_ReenterNetworkLobby();` | **Load-bearing** — defined at `menu_network.c:1691-1764`, must be ported |
| 1043 | `rmlui_casual_lobby_set_room(room_buf);` | SAFE stub for LAN-only path |
| 1044 | `MenuScreen_Goto(MENU_SCREEN_CASUAL_LOBBY);` | Never hit without RmlUi |
| 1053 | `MenuScreen_Goto(dest);` | Load-bearing |

**`discovery.c`** (1 site):

| Line | Call | Stub safety |
|---|---|---|
| 7 | `#include "port/sdl/rmlui/rmlui_casual_lobby.h"` | Not self-guarded |
| 138 | `const char* room_code = rmlui_casual_lobby_get_room_code();` then `auto_now = false` if set | SAFE stub → `""` |

**`sdl_net_adapter.c`**: zero code coupling (one comment only).

**`sdl_netplay_ui.cpp`** (3 sites):

| Line | Call | Stub safety |
|---|---|---|
| 27 | `#include "port/sdl/rmlui/rmlui_casual_lobby.h"` | Needs stub |
| 1033 | `room_code` for match_source tag | Safe → "ranked" |
| 1293, 1769, 1811 | `active_room` for mid-match lobby teardown check | Safe |

**`menu_network.c`**: ~35 sites, but concentrated in non-LAN paths.

### 11.5 Features lost without RmlUi

Backend works; only UI is RmlUi-only. Lost:
- Casual lobby browsing
- Tournament brackets UI
- Ranked matchmaking UI
- Leaderboards view
- Network replay picker
- In-game chat overlay
- QR code join
- Player profile
- F10 diagnostics panel (ping history, FPS graph)
- Toast notifications

### 11.6 Features retained (fully native)

- LAN peer discovery (`discovery.c`)
- LAN handshake + challenge/accept flow (`netplay.c:879-970`)
- STUN hole-punch + UPnP (`stun.c`, `upnp.c`, `sdl_netplay_ui.cpp:697-750`)
- GekkoNet rollback (`netplay.c:422-802`)
- `setup_vs_mode()` (`netplay.c:158-391`)
- Native incoming/outgoing challenge popups (`menu_network.c:105-273`)
- Native LAN-only lobby (`menu_network.c:1305-1687`)
- Netstats overlay (`netstats_renderer.c`) — drawn with `SSPutStrPro`, palette 6, format `"R:%d P:%d"`

### 11.7 Native-only effort estimate

Rewriting the 9 netplay RmlUi screens natively against our primitive 2D renderer:
- Text wrapping, scrollable lists, dynamic queue status, bracket visualization, leaderboard paging, chat with timestamps — each re-implemented per screen
- Rough estimate: **~8,000-12,000 LOC hand-written C, 6-10 weeks focused work**
- Ongoing cost: linear per new screen
- No prior arcade-style rollback lobby benchmark to cite

### 11.8 CLI shorthand is documented but not implemented

README at `/tmp/3sxtra/README.md:190-191` documents `3sx 1 192.168.1.100` direct-connect shorthand. But `src/port/config/cli_parser.c:59-173` has **no positional-arg parsing**. `grep 'argv\[1\]\|argv\[2\]' /tmp/3sxtra/src` returns zero matches.

Supported flags: `--scale`, `--volume`, `--renderer`, `--plugin`, `--port`, `--window-pos`, `--window-size`, `--enable-broadcast`, `--shm-suffix`, `--font-test`, `--ui <rmlui>`, `--help`, `--test-*`.

**If we want headless auto-join on MiSTer, we need to add it — ~20 LOC, trivial.**

### 11.9 Batocera precedent

3sxtra's Pi4/Batocera build ships RmlUi by default via `3sx.sh` (`--renderer gl`). It also ships `3sx-sdl2d.sh` (`--renderer classic`) as a fallback using `SDL_Renderer`. **MiSTer has no precedent for netplay-without-RmlUi in the 3sxtra community.**

---

## 12. RmlUi feasibility on MiSTer

### 12.1 Verdict: WORKS with specific effort

3sxtra pins RmlUi version **6.2** (`build-deps.sh:582`, `tools/batocera/rpi4/download-deps_rpi4.sh:226`).

- **C++ standard**: C++14 (`/tmp/rmlui-6.2/CMake/Utilities.cmake:101`)
- **CMake**: 3.10–3.27 (`/tmp/rmlui-6.2/CMakeLists.txt:3`)
- **Required dep**: FreeType 2.13.3 only (`build-deps.sh:470`)
- **Optional (all off in 3sxtra)**: LuaJIT, rlottie, lunasvg
- **Lua bindings**: `RMLUI_LUA_BINDINGS=ON` in 3sxtra but **zero `.rml` files under `assets/ui/` use `<script>` tags** (verified via grep). For netplay-only port, set `RMLUI_LUA_BINDINGS=OFF` and drop Lua entirely.

### 12.2 Rendering backend selection

RmlUi 6.2 ships several backends at `/tmp/rmlui-6.2/Backends/`:
- `RmlUi_Renderer_GL3.cpp` — desktop GL 3.3 core
- `RmlUi_Renderer_GL2.cpp` — legacy GL
- `RmlUi_Renderer_VK.cpp` — Vulkan
- `RmlUi_Renderer_SDL_GPU.cpp` — SDL3 GPU (Vulkan/Metal/D3D)
- **`RmlUi_Renderer_SDL.cpp` — plain `SDL_Renderer` (207 LOC, self-contained)**

Per `/Users/sb/Developer/3sx-mister/docs/archive/mister-port-plan.md:81-91,139,143`: stock MiSTer image has `libSDL2-2.0.so.0.14.0` but no `libGL*`, `libGLES*`, `libEGL*`, `libvulkan*`, `libdrm*`, `libgbm*`. **GL and SDL_GPU are unavailable.** Only the SDL_Renderer backend applies.

### 12.3 Why it will work on MiSTer

Our fork already drives `SDL_RenderGeometry` through SDL3's software renderer every frame:
- `/Users/sb/Developer/3sx-mister/vendor/Main_MiSTer/thirdsarm_wrapper.cpp:1818-1820`: sets `SDL_VIDEODRIVER=dummy` and `SDL_RENDER_DRIVER=software`
- `/Users/sb/Developer/3sx-mister/src/port/sdl/sdl_app.c:9815`: `SDL_CreateWindowAndRenderer(...)` returns an `SDL_Renderer*`
- `/Users/sb/Developer/3sx-mister/src/port/sdl/sdl_game_renderer.c:6876, 8868, 8890`: game renderer calls `SDL_RenderGeometry`
- `/Users/sb/Developer/3sx-mister/src/imgui/imgui/imgui_impl_sdlrenderer3.cpp:236`: ImGui renders triangles through SDL_Renderer

**ImGui already runs on MiSTer via this exact pipeline.** That's the same primitive RmlUi's SDL backend uses.

### 12.4 3sxtra integration

`/tmp/3sxtra/CMakeLists.txt:408-414`: all 4 backends linked, selected at runtime based on `SDLApp_GetRenderer()`.

`/tmp/3sxtra/src/port/sdl/rmlui/rmlui_wrapper.cpp:616-644`:
```
RENDERER_OPENGL          → RenderInterface_GL3       (Pi4 default)
RENDERER_SDLGPU          → RenderInterface_SDL_GPU   (Vulkan)
RENDERER_SDL2D / _CLASSIC → RenderInterface_SDL      (SDL_Renderer) ← our target
```

Zero per-frame cost when no overlay is visible: `rmlui_wrapper_new_frame` at `:907-909` checks `s_any_window_visible` and early-returns.

Rendering: direct `s_window_context->Render()` on the shared `SDL_Renderer` (`:972-982`) — no begin/end frame, no clear (preserves game canvas).

Font setup (`:712-715`): BoldPixels.ttf loaded at init; NotoSansJP (4.3 MB) deferred until first frame.

### 12.5 The blend-mode gotcha

`/tmp/rmlui-6.2/Backends/RmlUi_Renderer_SDL.cpp:37-38`:

```cpp
blend_mode = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, ...)
```

Custom premultiplied-alpha blend. **SDL3's software renderer does NOT implement `SDL_ComposeCustomBlendMode`** — only predefined `BLEND`/`ADD`/`MOD`/`MUL` enums. `SDL_SetRenderDrawBlendMode(custom)` returns an error; rendering falls back to previously set mode.

**Fix**: Since `BeginFrame()` is non-virtual and `blend_mode` is private in RmlUi 6.2's `Backends/RmlUi_Renderer_SDL.h`, the practical fix is to vendor `Backends/RmlUi_Renderer_SDL.cpp` under our tree and patch the constructor to set `blend_mode = SDL_BLENDMODE_BLEND`. ~2-LOC patch on the vendored copy.

**REQUIRES VERIFICATION**: first-light prototype to confirm visual correctness. Inferred from SDL3 source comments (`src/render/software/SDL_render_sw.c`), not a running MiSTer.

### 12.6 Binary size (host arm64 macOS build, Release, stripped)

Measured values:
- `librmlui.a` archive: **4.62 MB unstripped**
- Minimal binary with only `Rml::Initialise + CreateContext`: **1.53 MB stripped** (strong DCE evidence)
- Realistic netplay UI use: **~2-3 MB text+rodata**
- FreeType stripped static: **~0.5-0.7 MB**
- UI assets: 620 KB `.rml/.rcss` + 160 KB BoldPixels = **~780 KB** (or +5 MB if NotoSansJP Japanese font kept)

**Total MiSTer binary impact: +3-4 MB text, +780 KB assets** (excluding Japanese font).

**Caveat**: armhf linux text typically ~10-15% larger than arm64 macOS. Approximate.

### 12.7 Pi4 precedent

- Pi4 default: GL3 (`/tmp/3sxtra/tools/batocera/rpi4/3sx.sh:14`)
- Pi4 fallback: SDL2D (`/tmp/3sxtra/tools/batocera/rpi4/3sx-sdl2d.sh:10`) — `exec "$SCRIPT_DIR/3sx" --renderer classic` — this is the `RmlUi_Renderer_SDL` path, shipping code.
- MiSTer's Cyclone V HPS has no GPU driver in stock userspace. Only the SDL2D-style path applies.

### 12.8 Cross-compile

- FreeType cross-build is a direct copy of Pi4's recipe at `/tmp/3sxtra/tools/batocera/rpi4/download-deps_rpi4.sh:201-213`, substituting `armhf-linux-gnueabihf-gcc` and `CMAKE_SYSTEM_PROCESSOR=arm`.
- RmlUi's CMake has no x86 intrinsics, no Windows-only paths.
- `/tmp/3sxtra/CMakeLists.txt:140-158` demonstrates the import-target pattern needed for `CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY` cross-compile toolchains.

No blockers. Pi4 recipe is ~90% reusable.

### 12.9 RmlUi vs native-rewrite comparison

| Approach | Code port | New LOC | Time | Maintenance |
|---|---|---|---|---|
| **RmlUi (recommended)** | 1,764 (`menu_network.c`) + 1,675 (wrapper) + 6,150 (9 screens) + 2,000 (ms_* glue) + 780 KB assets | **~150 (blend fix)** | **2-3 weeks** (after engine plumbing) | Edit `.rcss`, recompile nothing |
| Native rewrite | 1,764 (`menu_network.c`) + 2,000 (ms_* glue) | **~8,000-12,000** | **6-10 weeks** | Linear per new screen |

**RmlUi is 3-4× less work and gives better feature velocity for tournaments/leaderboards/replays later.**

---

## 13. Dependency matrix

### 13.1 Full dep list

| Dep | Source | License | Linkage | Required? | How fetched |
|---|---|---|---|---|---|
| **GekkoNet** | `github.com/HeatXD/GekkoNet` @ `7be848c` | Apache-2.0 | static `.a` | **Core** | `build-deps.sh:193-207`, `-DNO_ASIO_BUILD=ON -DBUILD_SHARED_LIBS=OFF` |
| **SDL3_net** | `github.com/libsdl-org/SDL_net` @ `92022dc` | Zlib | static `.a` | **Core** | `build-deps.sh:229` |
| **RmlUi** | `github.com/mikke89/RmlUi` @ `6.2` | MIT | static `.a`s | **Core (UI)** | 3sxtra `build-deps.sh:582`, Pi4 `download-deps_rpi4.sh:226` |
| **FreeType** | freetype @ `2.13.3` | FTL/GPLv2 | static | **Core (UI)** | 3sxtra `build-deps.sh:470` |
| **cJSON** | `github.com/DaveGamble/cJSON` | MIT | source compile | Optional (lobby) | `build-deps.sh:156-164` |
| **miniupnpc** | system package `libminiupnpc-dev` | BSD-3 | dynamic | Optional (`HAVE_UPNP`) | `find_library(MINIUPNPC_LIB ...)` |
| **qrcodegen** | vendored at `src/third_party/qrcodegen/` | MIT | source | Optional (QR join) | Copy 2 files |
| **sha256 (vendored)** | `src/netplay/sha256.*` | PD | source | Identity/lobby only | Copy 2 files |
| **libcurl** | system `libcurl` | MIT-style | dynamic | Lobby only | `target_link_libraries(3sx PRIVATE … curl)` at CMakeLists.txt:560 |
| **Lua** | — | MIT | — | NOT needed (zero `<script>` tags in netplay `.rml`) | — |
| **librashader** | Rust crate | MPL-2.0 | — | **NOT a netplay dep** | `CMakeLists.txt:524-530` |
| **Tracy** | `github.com/wolfpld/tracy` v0.13.1 | BSD-3 | — | **NOT a netplay dep** | `CMakeLists.txt:100-109` |
| **Spout2** | `github.com/leadedge/Spout2` | BSD-2 | Windows-only | **NOT a netplay dep** | `CMakeLists.txt:255,711-720` |

### 13.2 GekkoNet specifics

- Header-only C API; C++ impl.
- `NO_ASIO_BUILD=ON` removes the heavy dep.
- **REQUIRES VERIFICATION**: whether `asio.hpp` is still transitively pulled at compile time even with `NO_ASIO_BUILD=ON`. Only manifests during the actual ARM cross-build.
- Our fork already builds it for desktop (`CMakeLists.txt:208` defines `GEKKONET_STATIC GEKKONET_NO_ASIO`).

### 13.3 Lobby server (separate, not in game binary)

- `tools/lobby-server/lobby-server.js`: Node.js + `bad-words`, `better-sqlite3`, `geoip-lite`
- Not required for LAN/direct-IP. Required for casual rooms, tournaments, ranked.
- Hosted separately (systemd unit shipped).

### 13.4 STUN default servers

`/tmp/3sxtra/src/netplay/stun.c:191-199` (hardcoded, no config key):

```c
static const struct {
    const char* host;
    uint16_t port;
} stun_servers[] = {
    { "stun.l.google.com", 19302 },
    { "stun1.l.google.com", 19302 },
    { "stun.cloudflare.com", 3478 },
    { "stun.nextcloud.com", 443 },
};
```

Tried in order (`stun.c:221-343`). If all 4 fail, `Stun_Discover` returns `false` (`stun.c:346-348`) → plain UDP bind fallback at `netplay.c:454`. **No hosted STUN server required.**

---

## 14. ARM cross-compile recipes

### 14.1 Current state of `/Users/sb/Developer/3sx-mister/build-deps.sh`

- Lines 54-127: SDL3 + FFmpeg recipes (SDL3 already built for both desktop and mister profiles)
- Lines 152-163: MiSTer SDL3 cross-build works
- **Lines 182-212: GekkoNet** — gated to `desktop` profile, prints "Skipping GekkoNet" on `mister`
- **Lines 218-246: SDL3_net** — same gating
- Lines 252-292: libcdio (desktop only)
- **New recipes needed**: RmlUi + FreeType (not present), cJSON (not present), optional miniupnpc

### 14.2 Toolchain context

- Host: Docker `linux/amd64` image per `tools/mister/setup-build-container.sh:157` (installs `libc6-dev-armhf-cross`)
- Target: armhf glibc (Debian/Ubuntu armhf)
- Toolchain: `clang-20` with `--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include`
- MiSTer runtime is glibc-based (confirmed by `readelf -A` checks in `docs/mister-runbook.md:145-156`)

### 14.3 What needs adding

```sh
# In build-deps.sh for PROFILE=mister:
# 1. Move GekkoNet recipe out of `if [ "$PROFILE" = "desktop" ]` gate
# 2. Move SDL3_net recipe out of gate
# 3. Add FreeType cross-build recipe (follow Pi4 pattern)
# 4. Add RmlUi cross-build recipe (CMake add_subdirectory or static lib)
# 5. Add cJSON source-compile (if lobby kept)
# 6. Optional: miniupnpc cross-build or armhf package
```

### 14.4 Risks

- GekkoNet transitive `asio.hpp` include (verify during cross-build)
- FreeType build flags minimization (Pi4 disables HarfBuzz/brotli/bz2/PNG/zlib)
- Toolchain env inheritance — nested `cmake` calls must inherit `CC`/`CXX` from the Docker container

---

## 15. MiSTer platform baseline

### 15.1 Hardware

- Board: Terasic DE10-Nano (Cyclone V SoC)
- HPS CPU: ARMv7 Cortex-A9 dual-core @ 800 MHz stock (overclockable to 1200 MHz per commit `e510bc42`)
- RAM: 1 GB DDR3

### 15.2 OS / kernel

| Item | Value | Source |
|---|---|---|
| Kernel | Linux 5.15 (`MiSTer-v5.15` branch) | [MiSTer-devel/Linux-Kernel_MiSTer](https://github.com/MiSTer-devel/Linux-Kernel_MiSTer) |
| Preempt model | `CONFIG_PREEMPT_NONE=y` | defconfig |
| Userland libc | glibc (armhf) | `docs/mister-runbook.md:145-156` (readelf checks) |
| Init / network daemons | `dhcpcd` on `eth0`, `wpa_supplicant` for WiFi | [MiSTer docs](https://mister-devel.github.io/MkDocs_MiSTer/advanced/network/) |
| Root FS | Custom Mr. Fusion installer + small Linux image | [mr-fusion](https://github.com/MiSTer-devel/mr-fusion) |
| Firewall | No rules by default; opt-in via `security_fixes.sh` | [script](https://github.com/MiSTer-devel/Scripts_MiSTer/blob/master/security_fixes.sh) |
| Security | No seccomp, no AppArmor, runs as root | kernel config |

### 15.3 Kernel net config (from MiSTer_defconfig)

Enabled:
- `CONFIG_INET=y`, `CONFIG_IP_MULTICAST=y`, `CONFIG_IP_PNP=y`, `CONFIG_IP_PNP_DHCP=y`
- `CONFIG_TCP_CONG_CUBIC=y`, `CONFIG_DEFAULT_TCP_CONG="cubic"`
- `CONFIG_TUN=y`, `CONFIG_PPP=y`, `CONFIG_PPP_MPPE=y`
- `CONFIG_STMMAC_ETH=y`, `CONFIG_DWMAC_SOCFPGA=y` — **onboard GbE**

Disabled:
- `# CONFIG_IPV6 is not set` — **IPv6 completely off at kernel level**
- `# CONFIG_SECCOMP is not set`
- `# CONFIG_NF_NAT is not set`, `# CONFIG_IP_NF_NAT is not set`
- `# CONFIG_NF_TABLES is not set` (no nftables; legacy iptables only)
- `# CONFIG_USB_USBNET is not set`, `# CONFIG_USB_NET_* is not set`, `# CONFIG_USB_RTL8152 is not set`

---

## 16. MiSTer network stack

### 16.1 Correction: onboard GbE DOES exist on DE10-Nano HPS

Previously (incorrectly) believed: DE10-Nano has no native HPS ethernet; must use USB adapters.

**Correct**: DE10-Nano has a Gigabit PHY wired directly to HPS as `eth0` via STMMAC. Typical boot log: `socfpga-dwmac ff702000.ethernet eth0: Link is Up - 1Gbps/Full - flow control rx/tx`.

Source: [DE10-Nano getting started](https://www.intel.com/content/www/us/en/developer/articles/guide/terasic-de10-nano-get-started-guide.html), [MiSTer Advanced Networking](https://mister-devel.github.io/MkDocs_MiSTer/advanced/network/).

### 16.2 Interface ranking for end users

1. **Onboard `eth0` RJ45** — always works, 1 Gbps, no config.
2. **USB WiFi dongle** — works with specific chipsets. Compiled as modules in MiSTer_defconfig:
   - Realtek: RTL8187, RTL8188EU/FU, RTL8812AU, RTL8821AU/CU, RTL8822BU, RTL8XXXU
   - MediaTek: MT7601U, MT76xx
   - Ralink: RT2800USB, RT73USB, RT2500USB
   - Marvell: LIBERTAS, MWIFIEX
   - Community-recommended: ASUS USB-AC53 Nano, D-Link DWA-171 Rev A1 (NOT Rev C1), CanaKit, Comfast CF-812AC, TP-Link small adapters. Source: [Recommended WiFi Dongles](https://misterfpga.org/viewtopic.php?t=407).
3. **USB ethernet dongles — NOT SUPPORTED**. All `CONFIG_USB_NET_*` disabled. Do not recommend. Source: [USB Ethernet forum thread](https://misterfpga.org/viewtopic.php?t=3027).
4. **FPGA-side GbE**: not wired to HPS on stock MiSTer; irrelevant.

### 16.3 POSIX socket support

All BSD socket APIs needed by GekkoNet + SDL3_net + STUN + discovery:
- `socket`, `bind`, `connect`, `sendto`, `recvfrom`, `select`/`poll`/`epoll_*`
- `SO_REUSEADDR`, `SO_RCVBUF`, `SO_SNDBUF`, `SO_BROADCAST`, `SO_TIMESTAMP`
- `getaddrinfo`, `inet_ntop`/`inet_pton`, `fcntl(O_NONBLOCK)`

### 16.4 Caveats

- **IPv6**: any call with `AF_INET6` fails with `EAFNOSUPPORT`. `getaddrinfo(..., AF_UNSPEC)` returns IPv4-only. 3sxtra is IPv4-first per code inspection, so low risk.
- **SDL3_net broadcast**: per source inspection ([SDL_net main](https://github.com/libsdl-org/SDL_net)), it does NOT set `SO_BROADCAST` on you. 3sxtra's `discovery.c` likely uses raw BSD sockets for UDP broadcast on port 7999. **REQUIRES VERIFICATION** during port: may need separate `socket()` + `setsockopt(SO_BROADCAST)` path.

### 16.5 UDP specifics

- Arbitrary port binding above 1024: standard Linux.
- Port 7999 usable for explicit bind (standard `net.ipv4.ip_local_port_range` = `32768-60999` puts it below ephemeral range).
- MTU: 1500 on eth0 and USB WiFi; GekkoNet packets (~200 bytes) never fragment.

### 16.6 STUN / hole-punching

No platform blockers. Behind NAT behavior is entirely determined by user's home router, not MiSTer.

UPnP-IGD via miniupnpc: kernel supports the multicast discovery (`CONFIG_IP_MULTICAST=y`); miniupnpc is pure userland. No MiSTer blocker.

Standard symmetric NAT / CGNAT / double-NAT limitations apply (same as any netplay implementation).

### 16.7 Scheduling concerns

`CONFIG_PREEMPT_NONE=y` = lowest-preemption model. Not RT-safe. Our frame pacer runs `SCHED_FIFO` prio 49 (commit `e65b51a8`).

**Net thread pinning recommendations** (for avoiding priority inversion with pacer):
1. `SCHED_OTHER` pinned to CPU1 — lowest risk, simplest
2. `SCHED_FIFO` prio 20 (below pacer) pinned to CPU1 — guaranteed RX head-of-line

### 16.8 USB 2.0 bandwidth

DE10-Nano OTG is USB 2.0 (480 Mbps). WiFi dongle shares with HID controllers and storage. GekkoNet packets are tiny (~100 kbps each way at 60 Hz) → bandwidth fine. Interrupt contention with USB HID polling could matter under heavy load — **REQUIRES ON-DEVICE VERIFICATION**.

### 16.9 USB WiFi jitter

Desktop 2.4 GHz WiFi typically shows 5-30 ms jitter under load. At 60 Hz (16.67 ms per frame), this is bad for rollback. **Recommend wired ethernet in release notes.** No MiSTer-specific numbers published.

### 16.10 MiSTer netplay precedent

**No MiSTer core ships working rollback netplay as of 2026-04-20.**
- MiSTerNet ([forum t=1929](https://misterfpga.org/viewtopic.php?t=1929)) was design discussion, never shipped.
- RetroArch rollback exists on similar ARM Linux hardware (reference).
- 3sx-mister would be **first**. No prior art to copy; no prior art saying it's blocked.

### 16.11 Update considerations

- MiSTer Update_All script updates kernel periodically — re-verify on new kernels.
- Mr. Fusion reinstall wipes `/etc` — don't depend on user `/etc` customizations.
- Runs as root: no capability barriers, security concern only.

---

## 17. Performance analysis

### 17.1 State size and rollback memcpy

- `sizeof(State) ≈ 241 KB` on 32-bit ARM (GameState ~17 KB + EffectState ~224 KB).
- `input_prediction_window = 8` (`netplay.c:430`).
- Worst case per game frame: 1 save (241 KB) + up to 8 loads+replays (8 × 241 KB = 1.93 MB).
- Total memcpy traffic: **~2.17 MB/frame × 60 Hz = ~130 MB/s**.
- DDR3 practical bandwidth on Cortex-A9 @ 800 MHz: ~500 MB/s–1 GB/s.
- Memcpy is ~13-26% of bandwidth — significant but not prohibitive.

### 17.2 Re-simulation cost (dominant)

From `/Users/sb/Developer/3sx-mister/docs/performance-optimizations.md`:
- Steady-state @ 800 MHz stock: **~60 FPS normal gameplay, 45-55 FPS during super-art bursts** (line 6)
- Game logic `U:` subcomponent: ~2-4 ms steady state (line 349)

If `step_game ≈ 2-3 ms`:
- 1 frame = 16.67 ms budget
- Worst case 8 rollback replays: 8 × 2.5 ms = **20 ms → exceeds frame budget**
- Super-art scenes (45-55 FPS baseline = 3-4 ms headroom) worst-hit

### 17.3 Realistic impact

- Normal gameplay, 1-2 frame rollback typical: ~5-10% frame time
- Heavy rollback (3+ frames) in super-art scenes: **frame drops likely at 800 MHz**

### 17.4 Mitigations

1. **1200 MHz overclock** (commit `e510bc42`) → +50% CPU → restores headroom.
2. **Reduce `input_prediction_window` 8 → 4-5** (one-line change at `netplay.c:430`; trades input delay for CPU budget).
3. **Pin game loop + pacer to CPU0, net thread to CPU1** (MiSTer has 2 A9 cores).
4. **Disable non-essential telemetry during matches** (perf overlay already toggleable).

### 17.5 UI performance (RmlUi)

- Zero cost when no overlay visible (`rmlui_wrapper.cpp:907-909` early return)
- Overlay active: extrapolated 1-3 ms from existing ImGui cost on MiSTer
- **REQUIRES VERIFICATION** with prototype — no direct measurement yet

### 17.6 Binary size

| Component | Added size |
|---|---|
| RmlUi static libs | ~2-3 MB text |
| FreeType static | ~0.5-0.7 MB |
| UI assets | ~780 KB (English only) or ~5 MB (with NotoSansJP) |
| GekkoNet + SDL3_net | ~0.5-1 MB combined |
| **Total impact on 3s-arm binary** | **~4-5 MB** |

Fits comfortably in MiSTer's 1 GB RAM and current RBF/release pipeline.

---

## 18. Build-system changes required

### 18.1 `CMakeLists.txt` changes

Current relevant sections in `/Users/sb/Developer/3sx-mister/CMakeLists.txt`:

- Lines 16-28: `PORT_MISTER=ON` flips `_ENABLE_NETPLAY_DEFAULT=OFF`. Keep OFF default until netplay is green.
- Lines 62-72: `GAME_SRC` glob + stub substitution when disabled.
- Lines 112-114: `ENABLE_NETPLAY` defines (`NETPLAY_ENABLED GEKKONET_STATIC GEKKONET_NO_ASIO ENABLE_NETPLAY`).
- Lines 262-267: include dirs conditional on `ENABLE_NETPLAY`.
- Lines 284-289: link libs conditional.

Changes needed:

1. New flags (suggested defaults OFF for MiSTer until green):
   - `ENABLE_NETPLAY` — flip default to ON for mister once baseline works
   - `ENABLE_RMLUI` — new option
   - `ENABLE_NETPLAY_LOBBY` — gate on libcurl + cJSON
   - `ENABLE_NETPLAY_STUN` — gate on `stun.c`
   - `ENABLE_NETPLAY_UPNP` — gate on miniupnpc + `HAVE_UPNP`
2. Source filter: `list(FILTER GAME_SRC EXCLUDE REGEX "src/port/sdl/rmlui/")` when RmlUi off (guard includes with stubs).
3. Link libs for RmlUi/FreeType when on.

### 18.2 `build-deps.sh` changes

- Lines 182-212: Move GekkoNet recipe out of `if [ "$PROFILE" = "desktop" ]` gate.
- Lines 218-246: Move SDL3_net out of gate.
- New recipes: FreeType, RmlUi (match Pi4 pattern with `CMAKE_SYSTEM_PROCESSOR=arm`).
- Optional: cJSON source clone, miniupnpc.

### 18.3 Conflict check

`PORT_MISTER=ON + ENABLE_MISTER_ARM_HARDENING=ON + ENABLE_NETPLAY=ON`: orthogonal, no conflicts. Verified by inspecting:
- `CMakeLists.txt:108` (PORT_MISTER: define + include dir)
- `:169-203` (ARM hardening: `-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard`)
- `:112-114` (NETPLAY: defines + conditional link)

### 18.4 Config key additions

Our `config.h` has no `CFG_KEY_NETPLAY_*` keys today. Must add:
- `CFG_KEY_NETPLAY_PORT` (for local UDP bind)
- `CFG_KEY_NETPLAY_INPUT_DELAY` (override for `gekko_set_local_delay`)
- `CFG_KEY_NETPLAY_STUN_SERVERS` (optional — override hardcoded list)
- Likely others as the port evolves

---

## 19. Top-5 desync risks (ranked)

### Risk 1: `combo_type`/`remake_power` storage location mismatch

- 3sxtra's PLW contains `combo_type`, `remake_power` as members (`structs.h:672,692`)
- Ours has them as top-level globals (`plcnt.c:81-82`)
- `sanitize_plw_pointers` copies PLW wholesale; if we lift 3sxtra's focused checksum verbatim, our top-level `combo_type/remake_power` **never get checksummed** → silent desync when damage scaling drifts (pls02.c:949, cmb_win.c:54, hitcheck.c:1344)
- **Mitigation**: keep as globals, add explicit `GS_SAVE(combo_type)`/`(remake_power)` (already present in ours) **and** add them to the focused-checksum whitelist manually. Do NOT refactor PLW layout to match 3sxtra.

### Risk 2: `WORK.operator` rename + `p->cb`/`p->rp` PLW-field divergence

- 3sxtra renamed `WORK.operator` → `WORK.pl_operator` (C++ keyword conflict avoidance)
- 3sxtra's `sanitize_plw_pointers` does NOT zero `p->cb`/`p->rp` — those fields only exist in our PLW
- Verbatim copy of 3sxtra sanitizer → our heap pointers in `cb/rp` leak into checksum → checksum divergence without actual desync (false-positive)
- **Mitigation**: keep our `clean_plw_pointers`, port 3sxtra's `sanitize_work_rendering` separately. Keep `WORK.operator` (not rename) or patch `setup_vs_mode` line 195.

### Risk 3: Hitcheck "turbo" micro-optimization

- 3sxtra's `hitcheck.c:664-674`: `(0x15 >> p_idx) & 1` replaces 10-way switch
- Semantically equivalent IF bitmask correct; risk if cases 6,7,8,9 branch differs
- **Mitigation**: don't cherry-pick. Either port whole `hitcheck.c` (big diff, high risk) or keep ours entirely.

### Risk 4: `select_timer_state` missing module

- 3sxtra's `setup_vs_mode` calls `SelectTimer_Init()`-derived logic; `select_timer_state` is checksum-critical for character-select
- Our fork uses raw CPS3 `Select_Timer` global
- If we add `SelectTimerState` to GameState but don't port the module, save_state writes zeros, load_state zeros the field — both peers stay at zero, but engine runs different BCD math in `sc_timer.c` → `Select_Timer` diverges
- **Mitigation**: port `select_timer.c/h` wholesale (~150 LoC) + all call sites in `game.c`, `menu.c`, `sc_timer.c`.

### Risk 5: Release-build checksum coverage gap

- Our fork: `#if DEBUG` gates `calculate_checksum()` (`netplay.c:240,389`)
- 3sxtra: focused checksum active unconditionally (gated only by `battle_start_frame >= 0`)
- **Shipping our MiSTer binary in Release with no desync detection means desyncs manifest as "the match is weird" without diagnostic.**
- **Mitigation**: port `save_current_state` + focused checksum unconditional of DEBUG; keep only `dump_desync_state` behind `#if DEBUG`.

### Honorable mentions

- `_Static_assert` absence — add `sizeof(GameState)` and `sizeof(_TASK)` tripwires to our fork
- Task struct layout: our `_TASK.func_adrs` is `void (*)()` vs 3sxtra's `void (*)(struct _TASK*)`; 3sxtra also has `callback_adrs` field we lack. If 3sxtra's `EXPECTED_TASK_SIZE == 20` matches our layout, we're safe today — but future upstream merge could drift silently.
- `MenuTask_SetPhase` in `setup_vs_mode`: replace with `task[TASK_MENU].r_no[0] = 5` equivalent.

---

## 20. Phased implementation plan

| Phase | Deliverable | Validation criterion | Effort | Status |
|---|---|---|---|---|
| **1. Backfill GameState** | Add 33 fields + `select_timer` module + `_Static_assert` guards. No behavior change yet — data just present. | Unit tests pass; checksum identical before/after if new fields zeroed | S (3-5d) | **DONE** (merge `417609b4`, feat `03093eb7`) |
| **2. `setup_vs_mode` expansion** | Port all 234 lines of 3sxtra's version, replacing `MenuTask_SetPhase(MTP_NETPLAY_IDLE)` with `task[TASK_MENU].r_no[0] = 5` | Desync-free 100-frame desktop LAN match | M (5-8d) | **DONE** (merge `417609b4`, feat `5698e7a3`) |
| **3. Focused checksum + sanitizers** | Port `save_current_state` with focused checksum, `sanitize_plw_pointers` (keep our `cb`/`rp` zeroing), `sanitize_work_rendering`, and `dump_desync_state`. Unconditional of DEBUG. | Desync detection works in Release; `#if DEBUG` keeps the dump pipeline | S (2-3d) | **DONE** (merge `417609b4`, feat `671aa18c`) |
| **4. RmlUi + FreeType cross-compile** | Add recipes to `build-deps.sh` mister profile. Pi4 template with `CMAKE_SYSTEM_PROCESSOR=arm`. | `librmlui.a`, `libfreetype.a` in `build/mister-deps/`; CMake `find_package` succeeds | S (2-3d) | **DONE** (merge `b82802db`, feat `2433afdd` + `36443d96`) |
| **5. Blend-mode SDL subclass** | Subclass `RenderInterface_SDL::BeginFrame()` for `SDL_BLENDMODE_BLEND`; ensure non-premultiplied textures. First-light prototype. | Minimal RmlUi test overlay renders visually correctly on MiSTer | S (1-2d) | **DONE** (merge `b82802db`, feat `48edcda2`) |
| **6. Port `menu_network.c` + `ms_*` glue + 9 RmlUi screens + assets** | Full UI port from 3sxtra. Replace our `Netplay_Menu` in `menu.c:1339` with `Network_Lobby` dispatcher. | Navigate lobby → create/join room → initiate match flow on desktop | L (10-15d) | Pending |
| **7. GekkoNet + SDL3_net ARM cross-compile** | Move recipes out of desktop-profile gate. Verify with `readelf -A`. | Binary links, runs on MiSTer without dynamic-loader errors | S (1-2d) | **DONE** (merge `a46073a3`, feat `36ffbb07`) |
| **8. Net thread pinning** | Pin game loop + pacer to CPU0, net thread to CPU1 with `SCHED_OTHER` (or `SCHED_FIFO` prio 20). | No frame-pacer regression during active net I/O; `show-fps` overlay stable | S (1-2d) | Pending |
| **9. LAN match on two MiSTers** | On-device test using onboard `eth0` on two boxes | 0 desync events in 300-frame match, stable 60 FPS | M (3-5d; desync hunts wildcard) | Pending |
| **10. STUN + internet play** | Enable `ENABLE_NETPLAY_STUN=ON`. Test public internet P2P. | Match between MiSTer + desktop over internet | M (3-5d) | Pending |
| **11. Optional UPnP** | miniupnpc cross-build, `HAVE_UPNP` define. | Auto-forward behind common home routers | S (1-2d) | Pending |
| **12. Optional: lobby/tournaments/ranked** | Host Node.js lobby server; wire up full feature matrix | Tournament bracket end-to-end | XL (deferred) | Pending |

**MVP (phases 1-10): 30-50 focused days / 8-12 calendar weeks part-time.**

---

## 21. Unknowns requiring prototyping

Not paper-research — must be verified with actual code on actual hardware:

1. **RmlUi custom blend mode on SDL3 SW renderer.** 10-LOC wrapper fix, but first-light prototype required to confirm visual correctness. Inferred from SDL3 source comments.
2. **Rollback CPU budget on 800 MHz stock.** Worst-case 8-rollback exceeds 16.67 ms frame budget. Need real `show-fps` measurement during live rollback to quantify. Mitigations (1200 MHz overclock, reduce prediction window) validated after.
3. **USB WiFi jitter in practice on MiSTer.** Desktop norm is 5-30 ms; MiSTer-specific numbers unknown. Measurable with `iperf3 -u -b 1M` if installed.
4. **GekkoNet `asio.hpp` transitive include on ARMv7.** `NO_ASIO_BUILD=ON` may not prevent header pull-in. Only surfaces at cross-build.
5. **SDL3_net internal struct ABI.** `net_tuning.h:45-56` mirrors SDL3_net private struct to reach raw socket for `SO_RCVBUF`. Pinned to SDL3_net `92022dc`. Silently breaks if ref advances.
6. **UDP broadcast on port 7999 in practice.** Kernel + SDL3_net claims work; on-device verification pending.
7. **RmlUi overlay render cost on 800 MHz.** Extrapolated from ImGui (~1-3 ms); need real measurement during active lobby.

---

## 22. On-device verification commands

Run against `192.168.1.171` (per `reference-mister-credentials.md`). All read-only; no mutations.

```sh
# ===== MiSTer Network Baseline Verification =====
# ssh root@192.168.1.171 'bash -s' < this_script.sh

echo "=== [1] Kernel + OS ==="
uname -a
cat /etc/os-release 2>/dev/null || echo "(no /etc/os-release)"
cat /proc/version

echo
echo "=== [2] libc / userland ==="
/lib/ld-linux-armhf.so.3 --version 2>&1 | head -2 || echo "(no armhf ld)"
ldd --version 2>&1 | head -2

echo
echo "=== [3] Interfaces & addresses ==="
ip -4 addr show
ip link show

echo
echo "=== [4] Routing & DNS ==="
ip route
cat /etc/resolv.conf 2>/dev/null || echo "(no resolv.conf)"

echo
echo "=== [5] DHCP client ==="
pgrep -a dhcpcd || echo "(no dhcpcd running)"
ls -la /etc/dhcpcd.conf 2>/dev/null
cat /etc/dhcpcd.conf 2>/dev/null | head -30

echo
echo "=== [6] Firewall state ==="
which iptables && iptables -L -v -n
which nft && nft list ruleset

echo
echo "=== [7] Ephemeral port range + socket tunables ==="
sysctl net.ipv4.ip_local_port_range
sysctl net.ipv4.tcp_congestion_control
sysctl net.core.rmem_default net.core.rmem_max net.core.wmem_default net.core.wmem_max
sysctl -n net.ipv6.conf.all.disable_ipv6 2>/dev/null || echo "(net.ipv6 sysctl absent — IPv6 truly off)"

echo
echo "=== [8] Kernel network config ==="
zcat /proc/config.gz 2>/dev/null | grep -E "^(CONFIG_IPV6|CONFIG_NF_NAT|CONFIG_TUN|CONFIG_USB_USBNET|CONFIG_USB_NET_|CONFIG_STMMAC|CONFIG_SECCOMP|CONFIG_PREEMPT)" | head -40 || echo "(no /proc/config.gz — try /boot/config-$(uname -r))"

echo
echo "=== [9] /dev/net/tun presence ==="
ls -la /dev/net/tun 2>/dev/null || echo "(no /dev/net/tun)"

echo
echo "=== [10] Outbound UDP reachability to STUN servers ==="
for host in stun.l.google.com stun1.l.google.com stun.cloudflare.com stun.nextcloud.com; do
  echo -n "Resolving $host ... "
  getent hosts "$host" || echo "FAILED"
done

echo
echo "=== [11] Ping router + Internet ==="
GATEWAY=$(ip route | awk '/default/ {print $3; exit}')
echo "Default gateway: $GATEWAY"
ping -c 3 -W 2 "$GATEWAY" 2>&1 | tail -3
ping -c 3 -W 2 8.8.8.8 2>&1 | tail -3

echo
echo "=== [12] UDP broadcast capability (LAN discovery prep) ==="
ip -o link show | grep -o "BROADCAST" | head -1 && echo "eth/wlan supports broadcast flag"

echo
echo "=== [13] USB device tree ==="
lsusb 2>/dev/null || echo "(no lsusb)"

echo
echo "=== [14] CPU scheduling capability ==="
grep -E "(Hz|MHz|cpu MHz)" /proc/cpuinfo | head -4
nproc
chrt -m 2>/dev/null | head -5

echo
echo "=== [15] Network driver status ==="
dmesg 2>/dev/null | grep -Ei "(stmmac|dwmac|eth0|wlan|rtl|mt76|cdc_ether|usbnet)" | tail -20

echo
echo "=== [16] iperf3 probe (if installed) ==="
which iperf3 && echo "iperf3 available — consider 'iperf3 -c <peer> -u -b 1M' for jitter data" || echo "(iperf3 not installed)"

echo
echo "=== Done. ==="
```

Expected outputs:
- [3]: `eth0` present at gigabit (GbE link)
- [6]: `Chain INPUT (policy ACCEPT)` with no rules (stock no-firewall state)
- [7]: port range `32768 60999` (safe for port 7999 explicit bind)
- [8]: confirms config assumptions against actual shipping kernel
- [10]/[11]: outbound DNS + UDP reachability to STUN servers
- [15]: driver-level evidence of network path

---

## 23. Scope deferrals — what we're NOT doing

MVP explicitly excludes:
- **Node.js lobby server hosting** — 3234 LOC server + deployment + systemd + SQLite + GeoIP. Defer indefinitely unless someone runs hosted infra.
- **Tournament brackets** (`bracket.c` 650 LOC) — UI + backend exist in 3sxtra, wire later.
- **Ranked matchmaking** (Glicko-2, `lobby_server.c` HTTP/SSE) — requires server.
- **Online replays** — requires server-side storage.
- **Leaderboards** — requires server.
- **In-game chat overlay** — RmlUi-only; chat backend works (`sdl_net_adapter.c` OOB magic prefix `0x33 0x53 0x58 0x43` = "3SXC").
- **Player profiles** — requires identity.c + lobby server.
- **QR code join** — requires casual-lobby server.
- **F10 diagnostics panel** — RmlUi-only; data pipeline works at `sdl_netplay_ui.cpp:884,905,911`.
- **Toast notifications** — RmlUi-only; state machine works.
- **`ping_probe.c`, `net_detect.c`, `identity.c`** — mostly lobby UI decoration. Identity needed only if stable player-ID shown in LAN beacons.
- **Spectator mode** — `Netplay_RegisterSpectator`, event handlers `GekkoSpectatorPaused/Unpaused` (netplay.c:654-658). Later phase.

Deferred to future phases, not removed from code entirely. The `ENABLE_NETPLAY_LOBBY` / `ENABLE_NETPLAY_STUN` / `ENABLE_NETPLAY_UPNP` flags gate these for build-time selection.

---

## 24. Sources and references

### Code (all local or cloned)

- `/Users/sb/Developer/3sx-mister/` — our fork
  - `CMakeLists.txt` — build flag definitions (`:22-44`, `:62-72`, `:112-114`, `:262-267`, `:284-289`)
  - `build-deps.sh:182-246` — GekkoNet + SDL3_net recipes (desktop-gated today)
  - `src/netplay/` — existing upstream netplay (3127 LOC across 10 files)
  - `src/port/sdl/netplay_screen.c/.h` — SDL integration (76 LOC)
  - `src/port/sdl/sdl_game_renderer.c:6876,8868,8890` — SDL_RenderGeometry usage
  - `src/imgui/imgui/imgui_impl_sdlrenderer3.cpp:236` — ImGui SDL_Renderer path
  - `vendor/Main_MiSTer/thirdsarm_wrapper.cpp:1818-1820` — SDL_VIDEODRIVER=dummy
  - `src/sf33rd/Source/Game/effect/effect.h:7-15` — EffectState equivalents
  - `docs/mister-runbook.md:145-156` — readelf ARM tag checks
  - `docs/performance-optimizations.md:6,327,349,439-440` — perf baseline
  - `docs/archive/mister-port-plan.md:81-91,139,143` — platform constraints
- `/tmp/3sxtra/` — 3sxtra fork HEAD `a18eae1`
  - `src/netplay/` — 24 files, ~9295 LOC
  - `src/sf33rd/Source/Game/menu/menu_network.c` — 1764 LOC native UI
  - `src/port/sdl/netplay/sdl_netplay_ui.cpp` — 1820 LOC state machine bridge
  - `src/port/sdl/rmlui/` — ~7824 LOC C++ + `.rml/.rcss` assets
  - `src/include/game_state.h` — GameState typedef
  - `CMakeLists.txt:12-13,278-281,408-414` — ENABLE_RMLUI gates
  - `tools/batocera/rpi4/download-deps_rpi4.sh:201-227` — RmlUi/FreeType Pi4 recipe
  - `tools/batocera/rpi4/3sx-sdl2d.sh:10` — SDL backend launcher (shipping precedent)
  - `tools/lobby-server/lobby-server.js` — Node.js lobby (3234 LOC)
  - `GAP_ANALYSIS.md:11-29` — 3sxtra author's own netplay-gap self-audit
- `/tmp/rmlui-6.2/` — RmlUi 6.2 source
  - `Backends/RmlUi_Renderer_SDL.cpp:37-38,87` — SDL backend + blend-mode gotcha
  - `CMake/Utilities.cmake:101` — C++14 requirement
  - `CMakeLists.txt:3` — CMake 3.10-3.27 range

### External (URLs, all verified during research)

- [GekkoNet](https://github.com/HeatXD/GekkoNet) — Apache-2.0, C++17, cross-portable
- [SDL_net main](https://github.com/libsdl-org/SDL_net) — SDL3_net, Zlib
- [RmlUi](https://github.com/mikke89/RmlUi) — MIT
- [MiSTer Linux-Kernel_MiSTer v5.15](https://github.com/MiSTer-devel/Linux-Kernel_MiSTer/tree/MiSTer-v5.15)
- [MiSTer_defconfig raw](https://raw.githubusercontent.com/MiSTer-devel/Linux-Kernel_MiSTer/MiSTer-v5.15/arch/arm/configs/MiSTer_defconfig)
- [MiSTer Advanced Networking](https://mister-devel.github.io/MkDocs_MiSTer/advanced/network/)
- [MiSTer WiFi](https://mister-devel.github.io/MkDocs_MiSTer/basics/wifi/)
- [MiSTer Bible: WiFi Setup](https://boogermann.github.io/Bible_MiSTer/getting-started/network/wifi-setup/)
- [MiSTer security_fixes.sh](https://github.com/MiSTer-devel/Scripts_MiSTer/blob/master/security_fixes.sh)
- [USB Ethernet thread](https://misterfpga.org/viewtopic.php?t=3027)
- [MiSTer Recommended WiFi Dongles](https://misterfpga.org/viewtopic.php?t=407)
- [DE10-Nano Get Started (Intel)](https://www.intel.com/content/www/us/en/developer/articles/guide/terasic-de10-nano-get-started-guide.html)
- [MISTer Ethernet forum thread](https://www.atari-forum.com/viewtopic.php?t=34503)
- [MiSTerNet forum thread](https://misterfpga.org/viewtopic.php?t=1929)
- [MiSTerFPGA Online Netplay thread](https://misterfpga.org/viewtopic.php?t=2949)
- [emulation.gametechwiki Netplay](https://emulation.gametechwiki.com/index.php/Netplay)
- [Mr. Fusion](https://github.com/MiSTer-devel/mr-fusion)
- [MiSTer Lag Explained](https://mister-devel.github.io/MkDocs_MiSTer/advanced/lag/)

### Memory references

- `reference-mister-credentials.md` — SSH to 192.168.1.171 (password=1)
- `reference-mister-network-stack.md` — kernel/stack baseline
- `feedback-fbdev-not-used.md` — fbdev is dead code, ignore
- `project-fpga-native-video.md` — FPGA native DDR3 path
- `docs/design-fpga-native-video.md` — FPGA native video protocol

---

## 25. Document maintenance

**If any fact in this document is used to make a decision, verify the fact is still current against the referenced code / URL.** Memories age. Fork HEADs advance. Upstream breaks. Re-verify critical citations before committing to implementation choices.

When this document is superseded by implementation progress, update the relevant section rather than starting a new doc. Tag supersedence in the section header with the implementation commit.
