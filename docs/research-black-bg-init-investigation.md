# Deep Investigation: Black Background from Round Start (when 32-bit peer in session)
Date: 2026-04-24
Scope: BG-from-frame-0 rendering failure only. Desync is a separate investigation.

## Evidence (verbatim, three-scenario correlation)

Per user:
1. Gameplay stage BG layer renders all-black for the entire match, from frame 0. HUD (health bars, SA meter), character sprites, and hit effects all render normally. Only the stage BG is dark.
2. Correlation matrix:
   - Mac ↔ Mac (both 64-bit, localhost or heavy link-conditioned): BG renders correctly.
   - MiSTer ↔ Mac (cross-arch): BOTH peers show black BG.
   - MiSTer ↔ MiSTer (both 32-bit): BOTH peers show black BG.
3. The symptom is present from the start of the round, not introduced by gameplay events.
4. All other visuals render — so it's not a general render-pipeline failure, it's BG-layer-specific.

Because MiSTer↔MiSTer (both 32-bit, identical layout ⇒ no cross-arch checksum drift) also shows black, the mechanism cannot be a layout/pointer-size mismatch alone. MiSTer-only/32-bit code paths OR MiSTer-environment properties (slower CPU, different timing, different texture-backing) are candidates. The cross-arch MiSTer↔Mac case shares the symptom — which suggests the MiSTer peer's state is mutating in a way that propagates to the Mac peer via GekkoNet state sync (or that both peers diverge into the same black-BG state independently via a shared trigger condition).

## Section 1 — BG render pipeline inputs

### 1.1 Entry point

`BG_Draw_System()` at `src/sf33rd/Source/Game/system/sys_sub.c:899-930` is the root BG emission call. Called from every `Game*` routine that renders BG:
- `Game00()` — `src/sf33rd/Source/Game/game.c:194`
- `Game12()` — `src/sf33rd/Source/Game/game.c:289` (transition into character select)
- `Game01()` — `src/sf33rd/Source/Game/game.c:328` (character select)
- `Game2_0()` — `src/sf33rd/Source/Game/game.c:445` (match-start init)
- `Game2_1()` — `src/sf33rd/Source/Game/game.c:562` (main gameplay)
- `Game2_2()`, `Game2_4()`, `Game2_5()`, `Game2_6()`, `Game2_7()` — `src/sf33rd/Source/Game/game.c:589,657,662,704,714`
- `Game03()..Game11()` — various `BG_Draw_System()` calls across game.c.

### 1.2 The `BG_Draw_System` gate

```c
void BG_Draw_System() {
    ...
    if (bg_disp_off == 0) {
        for (i = 0; i < 4; i++, s2 = mask *= 2) {
            if (Screen_Switch_Buffer & mask) {
                scr_trans(i);        // emits polygons
            }
        }
    } else {
        for (i = 0; i < 4; i++, s3 = mask *= 2) {
            if (Screen_Switch_Buffer & mask) {
                scr_calc(i);         // only updates matrix, no polygons
            }
        }
    }
    ...
}
```
Source: `src/sf33rd/Source/Game/system/sys_sub.c:899-917`.

BG polygons fire only when both:
- `bg_disp_off == 0`
- `Screen_Switch_Buffer & (1<<i) != 0` for layer `i`

### 1.3 Polygon emission (`scr_trans` → `bgDrawOneScreen`/`bgDrawOneChip`)

Defined at `src/sf33rd/Source/Game/stage/bg.c:553-1099` (`scr_trans`) → branches on `tokusyu_stage`. All branches call `bgDrawOneScreen` or `bgDrawOneChip`.

`bgDrawOneChip` (`src/sf33rd/Source/Game/stage/bg.c:1145-1156`):
```c
void bgDrawOneChip(s32 x, s32 y, s32 xs, s32 ys, s32 gbix, u32 vtxCol, s32 ofsPal) {
    if ((No_Trans == 0) && ppgCheckTextureNumber(0, gbix)) {
        ppgCalScrPosition(x, y, xs, ys);
        ...
        ppgWriteQuadUseTrans(scrDrawPos, vtxCol, 0, gbix, 0, 0, ofsPal);
    }
}
```

**Critical gate**: `ppgCheckTextureNumber(0, gbix)` — if returns 0, the quad is NOT written. BG tile skipped → BG layer ends up with zero polygons → background clear color (black) visible.

`ppgCheckTextureNumber` (`src/sf33rd/Source/Common/PPGFile.c:1840-1866`):
```c
if (tex == NULL) tex = ppg_w.cur->tex;
if (tex == NULL) return 0;
if (tex->be == 0) return 0;
ix = num - tex->ixNum1st;
if (ix >= tex->total) return 0;
if (tex->handle[ix].b16[0]) return 1;
return 0;
```

Returns 0 (BG tile not drawn) if any of:
- current data list is unset,
- texture chunk not marked ready (`tex->be == 0` — set at end of `ppgSetupTexChunk_1st`),
- tile index out of range for the chunk,
- tile's `handle[ix].b16[0]` is 0 (handle not created by `ppgSetupTexChunk_3rd`).

### 1.4 Inputs to BG rendering

The inputs are the global sim fields saved/loaded for rollback:
1. `bg_disp_off` (u8) — set to 1 by `Bg_Disp_Switch(1)` in `effect_K8_move` (`src/sf33rd/Source/Game/effect/effk8.c:27`); set to 0 in `Bg_Close`, `bg_initialize`, `bg_etc_write`, `ending/end_main.c:250`, `Bg_Disp_Switch(0)` on Gill form-exit.
2. `Screen_Switch_Buffer` (u16, bits 0-3 for 4 BG layers) — written by `Bg_On_R/W`, `Bg_Off_R/W`, `Scrn_Renew` (`src/sf33rd/Source/Game/stage/bg.c:1423-1447`). Initially zero after `Bg_Close`.
3. `bg_w.stage`, `bg_w.scrno`, `bg_w.scno`, `bg_w.bg_index` — written by `bg_initialize` (`src/sf33rd/Source/Game/stage/bg_sub.c:1100-1180`) or by stage selection logic in `sel_pl.c`/`next_cpu.c`.
4. `bg_priority[4]` (u8) — written by `Bg_Texture_Load_EX` (`src/sf33rd/Source/Game/stage/bg.c:297-307`) from `stage_priority[]` table.
5. `bg_prm[i].bg_h_shift`, `bg_prm[i].bg_v_shift` — written by `Irl_Scrn` (`src/sf33rd/Source/Game/stage/bg.c:1460-1469`).
6. `scr_sc` (float zoom) — written by `Zoomf_Init`/`Zoom_Value_Set`.
7. PPG texture cache state (`ppgBgList[0..2].tex`, `ppgBgList[0..2].pal`, internal handles). NOT part of saved `GameState`.
8. Palette contents (`ColorRAM[][64]`). NOT part of saved `GameState`.

Note items 7 and 8 — these are NOT rolled back. This is a well-known constraint (renderer-side + texture-cache state is out of the save/load contract).

### 1.5 Palette path for main BG

`Bg_TexInit` (`src/sf33rd/Source/Game/stage/bg.c:64-78`) sets each `ppgBgList[i].pal = palGetChunkGhostCP3()` (`src/sf33rd/Source/Game/rendering/color3rd.c:508-510`) — this returns the single shared "CP3 ghost" palette pool `col3rd_w.palCP3`.

Main BG does NOT load a per-stage palette in `Bg_Texture_Load_EX`. It ONLY loads:
- stage 7 (Akane): `ppgSetupPalChunk(... akaneList)` at `src/sf33rd/Source/Game/stage/bg.c:340`.
- stages != 20, 21 (Akebono): `ppgSetupPalChunk(... akeList)` at `src/sf33rd/Source/Game/stage/bg.c:357`.

So the main BG tiles are drawn with palette index `bgPalCodeOffset[bgnm]` (default `0x12C`, written in `Bg_Texture_Load_EX` at `src/sf33rd/Source/Game/stage/bg.c:269`) into `col3rd_w.palCP3` — which must be populated with the stage's color data by `palUpdateGhostCP3` from `ColorRAM[]`.

`ColorRAM[]` gets written by `init_trans_color_ram` (`src/sf33rd/Source/Game/rendering/color3rd.c:194-400`) which is only called from the LDREQ queue processor `q_ldreq_color_data` (`src/sf33rd/Source/Game/rendering/color3rd.c:67`), triggered via `Push_LDREQ_Queue_BG(bg_w.stage + 0)` / `Push_LDREQ_Queue_Union(ix+20)` — see `Push_LDREQ_Queue_BG` at `src/sf33rd/Source/Game/io/gd3rd.c:267-270` and `Push_LDREQ_Queue_Union` at `src/sf33rd/Source/Game/io/gd3rd.c:272-292`.

**If `ColorRAM[0x12C..]` is never populated for the selected stage, the palette is whatever garbage/zero was there. All-zero palette → all-black tiles, even if textures render.**

### 1.6 Answer to "does zero palette / zero tilemap render black?"

- **Zero tilemap** — handled by `bgDrawOneChip`: zero/unloaded texture ⇒ `ppgCheckTextureNumber` returns 0 ⇒ no polygon ⇒ back-color clear visible (default black after `njSetBackColor(0,0,0)` in `Game00` at `src/sf33rd/Source/Game/game.c:193` or persisted from prior init).
- **Zero palette** — rendered polygon with all-zero palette entries produces all-black pixels.
- **`bg_disp_off == 1`** — layer skipped via `scr_calc` (matrix-only) ⇒ back color visible.
- **`Screen_Switch_Buffer == 0`** — `scr_trans` not called ⇒ no polygons ⇒ back color visible.

All four mechanisms are distinguishable by post-hoc DEBUG dump but produce the same visible output.

## Section 2 — Init-path code trace (nav → setup_vs_mode → stage load → frame 0)

### 2.1 Nav state machine (`src/netplay/netplay_nav.c`)

Operates entirely in pre-netplay (before Gekko session exists), driving menus with injected `SWK_START` presses.

Sequence (armed at cold-launch when user selects Netplay):
1. `NAV_WAIT_INIT` (line 174-186): waits for `task[TASK_INIT].condition == 0 && G_No[0] == 1`.
2. `NAV_PRESS_COIN` (line 188-214): at attract mode, injects Start to advance `Loop_Demo → Next_Title_Sub → G_No[0]=2`.
3. `NAV_PRESS_TITLE` (line 216-237): at title screen, injects Start to go Entry_01 → Game0_2 case 5 → `G_No[1]=12 + cpReadyTask(TASK_MENU, Menu_Task)`.
4. `NAV_WAIT_MENU` (line 239-258): waits for `task[TASK_MENU].condition == 1`.
5. `NAV_DRIVE_VS` (line 260-296):
   - Forces `Interface_Type[0] = 2; Interface_Type[1] = 2; Menu_Cursor_Y[0] = 1`.
   - Injects Start. Mode_Select case 3 (`src/sf33rd/Source/Game/menu/menu.c:402-432`) fires `Setup_VS_Mode(task_ptr); G_No[1] = 12; G_No[2] = 1; Mode_Type = MODE_VERSUS; cpExitTask(TASK_MENU)`.
   - Next tick: sees `task[TASK_MENU].condition == 0`, calls `apply_network_mode_override()` which overwrites `Mode_Type = MODE_NETWORK, Present_Mode = MODE_NETWORK, save_w[MODE_NETWORK].Handicap = 0, save_w[MODE_NETWORK].Time_Limit = 99`, etc.
6. `NAV_WAIT_ORCHESTRATOR` (line 298-309): waits for `Netplay_IsRemoteIpSet()`.
7. `NAV_START_NETPLAY` (line 311-314): calls `Netplay_BeginDirectP2P()` → sets `direct_p2p_pending = true`.
8. `NAV_DONE` (line 316-319).

### 2.2 `Netplay_TickDirectP2P` → `setup_vs_mode`

`Netplay_TickDirectP2P` at `src/netplay/netplay.c:815-844` (called every frame when `direct_p2p_pending == true`):
1. Gate: `if (task[TASK_INIT].condition != 0) return;`.
2. Clears `direct_p2p_pending`.
3. Calls `setup_vs_mode()`.
4. `session_state = NETPLAY_SESSION_TRANSITIONING`.

`setup_vs_mode()` at `src/netplay/netplay.c:147-378`. Relevant BG/state writes:
- `SDL_zeroa(plw); SDL_zeroa(zanzou_table); SDL_zeroa(super_arts);` (line 158-160).
- `task[i].timer = 0; SDL_zeroa(task[i].free);` for all tasks (line 165-168).
- `task[TASK_MENU].r_no[0] = 5; cpExitTask(TASK_SAVER); cpExitTask(TASK_PAUSE);` (line 171-173).
- `Pause = 0; Game_pause = 0;` (line 177-178).
- `System_all_clear_Level_B();` (line 197) — see 2.3.
- `G_No[0] = 2; E_No[0] = 1; Demo_Flag = 1; G_No[1] = 12; G_No[2] = 1;` (line 199-204).
- `Mode_Type = MODE_NETWORK; Present_Mode = MODE_NETWORK; Play_Mode = 0;` (line 205-207).
- `cpExitTask(TASK_MENU);` (line 210).
- Overrides `save_w[MODE_NETWORK]` (Time_Limit=99, Damage_Level=0, Handicap=0, etc. — line 216-222).
- Zeros a very long list of per-frame scalars (see line 223-376).
- BG-relevant zeros: `SDL_zeroa(bg_pos); SDL_zeroa(fm_pos); SDL_zeroa(bg_prm); Screen_Switch = 0; Screen_Switch_Buffer = 0; system_timer = 0; Interrupt_Timer = 0;` (line 249-255).
- `VS_Stage = 0;` (line 366).

**What `setup_vs_mode` does NOT touch**:
- `bg_w` struct — not zeroed or reset.
- `bg_w.stage` — retains prior value (zero at cold boot, else whatever prior match/demo left there).
- `bg_w.bg_routine` — retains prior value.
- `bg_w.bg_index` — retains prior value.
- `bg_disp_off` — not explicitly written (but `System_all_clear_Level_B → Bg_Close` sets it to 0 at `src/sf33rd/Source/Game/stage/bg.c:240`).
- `New_Challenger` — not written. If prior game set it, persists.
- `Champion` is written (line 305: `Champion = 0;`).
- `bg_priority[4]`, `bgPalCodeOffset[8]` — not cleared.
- `rw_dat[20]`, `rw_num`, `rw_bg_flag[4]`, `tokusyu_stage` — not cleared.
- PPG texture cache (`ppgBgList[]`, `ppgBgTex[]`, their handles) — survives `System_all_clear_Level_B` incompletely; `Bg_Close` releases texture HANDLES (`ppgReleaseTextureHandle(&ppgBgTex[i], -1)` at `src/sf33rd/Source/Game/stage/bg.c:229-237`) but does NOT tear down the `ppg_w` data list.

### 2.3 `System_all_clear_Level_B` (`src/sf33rd/Source/Game/system/sys_sub.c:951-954`)

```c
void System_all_clear_Level_B() {
    Bg_Close();
    effect_work_init();
}
```

`Bg_Close` (`src/sf33rd/Source/Game/stage/bg.c:223-241`):
```c
tokusyu_stage = 0;
rw_num = 0;
for (i = 0; i < 3; i++) ppgReleaseTextureHandle(&ppgBgTex[i], -1);
ppgReleaseTextureHandle(&ppgRwBgTex, -1);
ppgReleaseTextureHandle(&ppgAkeTex, -1);
ppgReleasePaletteHandle(&ppgAkePal, -1);
ppgReleaseTextureHandle(&ppgAkaneTex, -1);
ppgReleasePaletteHandle(&ppgAkanePal, -1);
Screen_Switch = 0;
Screen_Switch_Buffer = 0;
bg_disp_off = 0;
```

`effect_work_init` (`src/sf33rd/Source/Game/effect/effect.c:88-108`):
```c
SDL_zeroa(frw);
for (i = 0; i < EFFECT_MAX; i++) {
    frwctr = (EFFECT_MAX - 1) - i;
    c_addr = (WORK*)frw[frwctr];
    frwque[i] = c_addr->myself = frwctr;
    c_addr->before = c_addr->behind = -1;
}
frwctr = EFFECT_MAX;
frwctr_min = frwctr;
for (i = 0; i < 8; i++) head_ix[i] = tail_ix[i] = -1; exec_tm[i] = 0;
```

### 2.4 Transition to character select

After `setup_vs_mode`, state is: `G_No[0]=2, G_No[1]=12, G_No[2]=1, G_No[3]=0, Mode_Type=MODE_NETWORK`.

`session_state = NETPLAY_SESSION_TRANSITIONING`. Now `Netplay_Run()` runs per-frame (from `src/main.c:584-585`). In `NETPLAY_SESSION_TRANSITIONING` state, `Netplay_Run` calls `step_game(true)` per-frame UNTIL `game_ready_to_run_character_select()` returns true (`src/netplay/netplay.c:903-910, 574-576`):
```c
static bool game_ready_to_run_character_select() {
    return G_No[1] == 1;
}
```

Per-frame `step_game` invokes `njUserMain → Game_Task → Main_Jmp_Tbl[G_No[0]](task_ptr)` = `Main_Jmp_Tbl[2] = Game`. `Game()` at `src/sf33rd/Source/Game/game.c:181-189` dispatches `Game_Jmp_Tbl[G_No[1]]` = `Game_Jmp_Tbl[12] = Game12`.

`Game12()` (`src/sf33rd/Source/Game/game.c:287-298`) calls `Game12_Jmp_Tbl[G_No[2]]()`.
- `G_No[2]==1` → `Game12_1` (line 304-308): `G_No[2]+=1; Switch_Screen_Init(1); SsBgmFadeOut(0x1000);`.
- `G_No[2]==2` → `Game12_2` (line 310-324): waits for `Switch_Screen(1)`, then `G_No[1] = 1; G_No[2] = 0; G_No[3] = 0; Control_Time = 481; Cover_Timer = 23; effect_work_init(); cpExitTask(TASK_MENU);`.

Once `G_No[1]==1`, `transition_ready_frames` starts incrementing in `Netplay_Run`. After 2 frames, `configure_gekko()` fires → `session_state = NETPLAY_SESSION_CONNECTING`.

### 2.5 Character select runs UNDER Gekko

After `configure_gekko`, every frame goes through `run_netplay → step_logic → process_events`, which runs `advance_game` for `GekkoAdvanceEvent`, `save_state` for `GekkoSaveEvent`, `load_state_from_event` for `GekkoLoadEvent`.

`advance_game` (`src/netplay/netplay.c:591-604`) injects synthesized inputs into `p1sw_0/p2sw_0` and calls `step_game`. So Select_Player runs under GekkoNet control.

`Game01()` (`src/sf33rd/Source/Game/game.c:327-434`) runs. Case 0 includes `System_all_clear_Level_B();` on line 360 — **second** clear of `Screen_Switch`, `bg_disp_off`, PPG BG tex handles.

Default case (after select completes): `if (Switch_Screen(0) != 0) { Game01_Sub(); ...; if (Demo_Flag) { G_No[1] = 2; ... } else { ... }`. `Demo_Flag` was set to 1 by `setup_vs_mode` (line 201). `Sel_PL_Cont_3rd` (`src/sf33rd/Source/Game/screen/sel_pl.c:316-328`) would clear `Demo_Flag` only if `G_No[1] != 1` — during char select `G_No[1]==1`, so `Demo_Flag` is NOT cleared there. Unless another code path clears it, we enter the `Demo_Flag` branch that jumps directly to `G_No[1] = 2` WITHOUT running the `else` branch that deactivates players (line 408-412).

Note this `Demo_Flag=1` post-setup_vs_mode mirrors the comment intent in the function (line 201 `Demo_Flag = 1`) — this makes the engine think the session is in "attract mode", which drives certain routing decisions (Switch_Screen Cut_Cut_Cut, specific Face_MV paths, etc.).

### 2.6 Exit path (sel_pl.c) and stage selection

If `Demo_Flag` is somehow cleared before Game01 default case fires (e.g., a rollback/resim sequence after a `Sel_PL_Cont_3rd` that the engine only reaches when `G_No[1] != 1` — which doesn't fit the actual flow), then the `else` branch at `src/sf33rd/Source/Game/game.c:408-412` runs, deactivating both players.

In the normal netplay flow: `Demo_Flag==1` ⇒ jump to `G_No[1]=2` without Exit_1st/Exit_2nd. This means:
- `Battle_Country = Setup_Battle_Country()` is NEVER called.
- `bg_w.stage = Battle_Country` is NEVER written.
- `Push_LDREQ_Queue_BG(bg_w.stage + 0)` is NEVER called.
- `bg_w.stage` retains the pre-select value (probably 0 from cold boot; but see 2.7 below).

### 2.7 BUT — the Game01 default case also has a `case 2:` path

Looking at `Game01` case 2 and default branch (`src/sf33rd/Source/Game/game.c:370-428`):
- case 2 calls `Select_Player()` returning true → advance to default.
- default: `Select_Player()`; then if `Switch_Screen(0)` completes:
  - runs `Game01_Sub()`
  - applies `Debug_w[0x1D] / [0x1E]` overrides to `My_char[0] / My_char[1]`.
  - `Purge_texcash_of_list(3); Make_texcash_of_list(3);` — tears down and reloads texture cache list 3 (which is character-select-specific textures).
  - branches on `Demo_Flag`.

In the `Demo_Flag == 1` branch: `G_No[1] = 2; G_No[2] = 0; G_No[3] = 0; E_No[0..3] = 4,0,0,0;`. This jumps straight into Game02. But wait — `bg_w.stage` was NOT updated.

`Select_Player()` is the UI loop at `src/sf33rd/Source/Game/screen/sel_pl.c:174+`. It presumably calls Exit_1st..Exit_7th internally. Let me verify:

Actually looking at `Select_Player` at `src/sf33rd/Source/Game/screen/sel_pl.c` — search for its use of `Exit_*`:

The file has `Exit_1st`, `Exit_2nd`, ..., `Exit_7th` defined but I need to see if they're called from a state-machine jump table inside `Select_Player`.

Line 1536 shows the jmp table: `{ ..., Exit_1st, Exit_6th, Exit_7th, Handicap_1st, Handicap_2nd, Handicap_3rd }`. So yes, `Select_Player`'s internal state machine runs Exit_*.

Exit_2nd (`src/sf33rd/Source/Game/screen/sel_pl.c:1572-1606`):
```c
Battle_Country = Setup_Battle_Country();
bg_w.stage = Battle_Country;
bg_w.area = 0;
...
Push_LDREQ_Queue_BG(bg_w.stage + 0);
```

So Exit_2nd DOES fire in the normal select flow. `bg_w.stage` is updated.

But only if `Select_Status[0] == 3` (line 1577). Otherwise Exit_2nd falls through to the "introduce new char" path that doesn't update `bg_w.stage`.

`Select_Status[0] == 3` means P1 has selected and gone through something. In demo/attract mode, this is NOT set to 3 — the demo select auto-picks characters without setting select_status to 3. In a real user-driven character select, P1 pressing start sets it to 3.

In our netplay case, `Demo_Flag == 1` post-setup_vs_mode. Inside character select, the user's real inputs drive both sides (via Gekko). Each peer presses buttons; their inputs are delivered as `inputs[0]` and `inputs[1]` to both peers identically by Gekko. So `Select_Status[0..1]` should get set to 3 for each player when they confirm their character.

### 2.8 `Setup_Battle_Country` when Mode_Type == MODE_NETWORK

`Setup_Battle_Country` (`src/sf33rd/Source/Game/screen/sel_pl.c:2013-2035`):
```c
u8 Setup_Battle_Country() {
    if (Mode_Type == MODE_VERSUS) {
        if (VS_Stage == 20) {
            Rnd32 = random_32();
            return Random_Stage_Data[1][Rnd32];
        }
        return VS_Stage;
    }
    if (My_char[0] == 17 && My_char[1] == 17) {
        Rnd32 = random_32();
        return Random_Stage_Data[0][Rnd32];
    }
    if (My_char[New_Challenger] == 17) return My_char[Champion];
    return My_char[New_Challenger];
}
```

Because `Mode_Type == MODE_NETWORK` (override applied by nav machine and by `setup_vs_mode`), we skip the `MODE_VERSUS` branch. Fall through to arcade-logic lookups.

`My_char[0]==17 && My_char[1]==17` — only if both pick Q (character 17). Normally false.
`My_char[New_Challenger] == 17` — only if the "challenger" picked Q.
Otherwise: returns `My_char[New_Challenger]`.

**`New_Challenger` is NOT initialized by `setup_vs_mode`** (`src/netplay/netplay.c:147-378` has no `New_Challenger = ...`). Its value is whatever was left by prior menus/boot — initially 0 (BSS), or whatever last set it. `Champion = 0` IS set (`src/netplay/netplay.c:305`).

If `New_Challenger == 0`: `Battle_Country = My_char[0]` = P1's pick = valid stage.
If `New_Challenger == 1`: `Battle_Country = My_char[1]` = P2's pick = valid stage.

Both are valid. **This by itself would not cause black BG on MiSTer specifically.**

### 2.9 Game2_0 — the "first match frame" init

`Game2_0` (`src/sf33rd/Source/Game/game.c:442-529`):
```c
BG_Draw_System();                         // Draws with prior state
Switch_Screen(0);
if (Check_LDREQ_Clear() == 0) fatal_error("Load queue failed to drain in time");
System_all_clear_Level_B();               // Bg_Close clears Screen_Switch, releases BG tex, effect_work_init
switch (Mode_Type) {
    case MODE_ARCADE: ...
    case MODE_VERSUS: ... /* fallthrough */
    case MODE_NETWORK:
        Play_Mode = 1;
        All_Clear_Random_ix();
        All_Clear_Timer();
        All_Clear_ETC();
        break;
    case MODE_REPLAY: ...
}
...
G_No[2] = 3;  // next frame goes Game2_3 → Game2_1
G_Timer = 10;
Round_num = 0;
...
bg_work_clear();                          // bg_routine = 0
win_lose_work_clear();
player_face_init();
TATE00();                                 // bg_initialize (loads stage texture)
```

`bg_work_clear` (`src/sf33rd/Source/Game/stage/bg_sub.c:1047-1066`) sets `bg_w.bg_routine = 0`.

`TATE00` (`src/sf33rd/Source/Game/stage/tate00.c:41-52`) then dispatches `jump_tbl[bg_w.bg_routine]()`:
- `bg_routine==0` → `ta0_init00` (line 54-61): `bg_w.bg_routine++; random_16(); bg_initialize();`.
- Subsequent frames: `bg_routine=1 → ta0_init01 → akebono_initialize + ta_move_tbl[bg_w.bg_index]()`; `bg_routine=2 → ta0_init02`; `bg_routine=3 → ta0_move` (steady state).

`bg_initialize` (`src/sf33rd/Source/Game/stage/bg_sub.c:1100-1180`):
```c
Bg_Off_R(7);
Family_Init();
Scrn_Pos_Init();
Zoomf_Init();
bg_w.bg_opaque = stage_opaque[bg_w.stage];
Screen_Switch = 0;
Screen_Switch_Buffer = 0;
bg_disp_off = 0;
bg_w.bg_index = bg_index_tbl[bg_w.stage][bg_w.area];
bg_w.scno = use_scr[bg_w.bg_index];
bg_w.scrno = use_real_scr[bg_w.bg_index];
...
if (G_No[0] != 2 || G_No[1] != 2 || G_No[2] != 2) {
    Bg_Texture_Load_EX();
}
Bg_Kakikae_Set();
...
```

At Game2_0 entry: `G_No[0]==2, G_No[1]==2, G_No[2]==0`. Condition `(G_No[0] != 2 || G_No[1] != 2 || G_No[2] != 2)` = (false || false || true) = **TRUE** → `Bg_Texture_Load_EX()` runs.

`Bg_Texture_Load_EX` (`src/sf33rd/Source/Game/stage/bg.c:243-367`):
```c
Bg_TexInit();                                          // line 266: wires ppgBgList[i].pal = palGetChunkGhostCP3()
for (i = 0; i < 8; i++) bgPalCodeOffset[i] = 0x12C;   // line 268-270
ending_flag = 0;                                       // line 272
for (stg = 0; stg < 3; stg++) if (stage_bgw_number[bg_w.stage][stg] != 0) break;  // line 274-278
for (i = 0; i < use_real_scr[bg_w.stage]; i++) scr_bcm[stg + i] = bg_map_tbl[bg_w.stage][i];  // line 280-282
for (i = 0; i < 3; i++) if (stage_bgw_number[bg_w.stage][i] > 0) Bg_On_R(1 << i);  // line 284-288 ← ENABLES LAYERS
if (bg_w.stage == 7) Bg_On_R(4);
key1 = Search_ramcnt_type(0x12);                       // line 294
loadAdrs = (void*)Get_ramcnt_address(key1);
loadSize = Get_size_data_ramcnt_key(key1);
...
for (j = 0; j < 3; j++, ...) {
    prio = stage_priority[bg_w.stage];
    bg_priority[j] = (prio & pmask) >> shift;
}                                                      // writes bg_priority[0..2]
bg_priority[3] = 70;
...                                                    // ppgSetupTexChunk_* calls
```

So at return from `Bg_Texture_Load_EX`:
- `Screen_Switch` has bits 0-2 set for each `stage_bgw_number[bg_w.stage][i] > 0`. Also bit 2 if stage==7.
- `bg_priority[0..3]` populated.
- `bgPalCodeOffset[]` = 0x12C uniformly.
- PPG tex chunk handles set.

After return, `Bg_Kakikae_Set` (`src/sf33rd/Source/Game/stage/bg.c:80-172`) sets up per-stage RW (rewrite) data — `rw_dat[]`, `tokusyu_stage`.

**Game2_0 then sets `G_No[2] = 3`.** Next frame: `Game2_3` → `Game2_1` (line 647-654):
```c
void Game2_3() {
    Game2_1();
    if (--G_Timer == 0) { G_No[2] = 1; Clear_Flash_No(); }
}
```

`Game2_1` (line 532-584) is the main gameplay routine that calls `BG_Draw_System()`.

### 2.10 Why LDREQ drain gate matters

`Game2_0` has the sync point (`src/sf33rd/Source/Game/game.c:448-450`):
```c
if (Check_LDREQ_Clear() == 0) {
    fatal_error("Load queue failed to drain in time");
}
```

This asserts the LDREQ queue is empty before we clear and reload. `Push_LDREQ_Queue_BG` is called earlier (Exit_2nd). The `Check_LDREQ_Queue_BG(bg_w.stage + 0)` in `Exit_6th` (`src/sf33rd/Source/Game/screen/sel_pl.c:1690-1692`) waits for that specific request to complete. The LDREQ `color_file` entries include palette files — so when `Exit_6th` waits for `Check_LDREQ_Queue_BG` to return true, it's ALSO waiting for the stage palette color file to have been fed to `init_trans_color_ram` and written into `ColorRAM[]`.

If the LDREQ processing is not deterministic across rollbacks — which it is NOT, because `q_ldreq[16]`, `ldreq_result[294]`, `ldreq_break`, and `fsCheckCommandExecuting()` state are NOT in the `GameState` rollback snapshot — then a rollback-and-replay can result in palette-load being deemed "complete" when in fact it has not run (if the rollback discarded the real completion).

## Section 3 — PORT_MISTER gates affecting BG state

### 3.1 Full PORT_MISTER grep in rendering/BG-relevant source

`grep -rn "PORT_MISTER" src/sf33rd/Source/Game/stage/ src/sf33rd/Source/Game/system/ src/sf33rd/Source/Game/rendering/ src/sf33rd/Source/Game/ui/` — **zero hits**.

All `PORT_MISTER` gates live in `src/port/` and `src/main.c`:
- `src/port/sdl/sdl_game_renderer.c`
- `src/port/sdl/native_video_writer.c`
- `src/port/sdl/fbdev_presenter.c` (dead on shipped MiSTer per memory `feedback-rmlui-render-target`)
- `src/port/sdl/sdl_app.c`
- `src/port/sdl/sdl_pad.c`
- `src/port/paths.c`
- `src/port/config/config.c`
- `src/port/linux/console_mode.c`
- `src/port/sound/spu.c`
- `src/main.c`

**None of these gate sim-side BG state**. They gate presentation (native_video_writer, sa_bg_cache), input device (sdl_pad), paths/config. The sim `Screen_Switch`, `bg_disp_off`, `bg_w.stage`, `bg_priority`, etc. are arch-agnostic at the `#if` level.

### 3.2 `sa_bg_cache_*` (presentation-side, not relevant to frame-0)

`src/port/sdl/sdl_game_renderer.c:150-167, 3394-3400, 3595-3725, 8614-8625, 9359`. Gated by `sa_bg_cache_frames_remaining > 0`; this counter is set by `SDLGameRenderer_StartSaBgCache` (callers elsewhere). At frame 0 of round `sa_bg_cache_frames_remaining == 0` — so the cache restore/snapshot paths are inert. Confirmed NOT a frame-0 mechanism.

### 3.3 `native_video_writer` (presentation-side)

Pure DDR3 blit path for 384×224 frames — presents whatever the SDL renderer wrote. Does NOT touch sim state. If the SDL renderer wrote a black frame, native_video_writer shows a black frame. Moves the symptom up a level but doesn't explain it.

### 3.4 Conclusion

**No `PORT_MISTER` gate exists in the sim-side BG pipeline**. Whatever mechanism is making BG black on MiSTer must be either:
- an unintended emergent behavior of architecture-agnostic code that behaves differently under the slower MiSTer CPU / different timing;
- an effect of the 32-bit ABI on structures that are serialized/deserialized as a memcpy through an architecture-dependent layout;
- an effect of non-deterministic side effects (LDREQ queue, texture cache) that the rollback contract does not cover and that behave differently on each peer.

## Section 4 — `setup_vs_mode` audit (BG-specific fields)

Audit of every field `setup_vs_mode` touches or fails to touch, for BG relevance:

### 4.1 Touched fields affecting BG, directly
- `Screen_Switch = 0` — via `System_all_clear_Level_B → Bg_Close` (`src/sf33rd/Source/Game/stage/bg.c:238`).
- `Screen_Switch_Buffer = 0` — same (`src/sf33rd/Source/Game/stage/bg.c:239`).
- `bg_disp_off = 0` — same (`src/sf33rd/Source/Game/stage/bg.c:240`).
- `bg_pos[]`, `fm_pos[]`, `bg_prm[]` zeroed at `src/netplay/netplay.c:249-251`.
- `system_timer = 0`, `Interrupt_Timer = 0` at `src/netplay/netplay.c:254-255`.
- `VS_Stage = 0` at `src/netplay/netplay.c:366`.

### 4.2 Touched fields affecting BG, indirectly
- `G_No[0]=2, G_No[1]=12, G_No[2]=1` at `src/netplay/netplay.c:199-204` — ensures game flows Game12 → Game01 → Game02.
- `Mode_Type = MODE_NETWORK, Present_Mode = MODE_NETWORK` at `src/netplay/netplay.c:205-206` — this is what triggers `Setup_Battle_Country`'s arcade-logic branch (rather than MODE_VERSUS which would use `VS_Stage`).
- `Demo_Flag = 1` at `src/netplay/netplay.c:201` — affects Game01 default case routing.
- `save_w[MODE_NETWORK].Handicap = 0` at `src/netplay/netplay.c:221` — skips handicap/stage-select UI.
- `Champion = 0` at `src/netplay/netplay.c:305`.
- `SDL_zeroa(My_char)` at `src/netplay/netplay.c:351`.

### 4.3 NOT touched / NOT reset, affecting BG

- `bg_w` struct — ALL fields retain pre-call values. Specifically:
  - `bg_w.stage` — retains prior (cold boot: 0).
  - `bg_w.area` — retains prior (0).
  - `bg_w.bg_index` — retains prior.
  - `bg_w.bg_routine` — retains prior (post-attract-mode: probably 3 or 0).
  - `bg_w.scno`, `bg_w.scrno` — retain prior.
  - `bg_w.bg_opaque` — retains prior.
- `rw_dat[20]` — not zeroed. Contains pointers (`const s16* rwd_ptr`, `const s16* brw_ptr` — see `include/structs.h:1566-1573`).
- `rw_num`, `rw_bg_flag[4]`, `tokusyu_stage` — not zeroed (but `Bg_Close` zeros `tokusyu_stage` and `rw_num`).
- `rw_gbix[13]`, `stage_flash`, `stage_ftimer`, `yang_ix_plus`, `yang_ix`, `yang_timer` — not zeroed.
- `ending_flag`, `end_prm[8]`, `gouki_end_gbix[16]` — not zeroed.
- `rw3col_ptr` — not zeroed.
- `bgPalCodeOffset[8]` — not zeroed (`Bg_Texture_Load_EX` resets to 0x12C later).
- `bg_priority[4]` — not zeroed (`Bg_Texture_Load_EX` overwrites later).
- `New_Challenger` — not zeroed (Champion IS zeroed).
- `Unsubstantial_BG[4]` — not zeroed.
- PPG texture cache infrastructure (`ppg_w.cur`, per-list `tex`, `pal`) — `Bg_Close` only releases tex handles; the DataList pointers survive.

None of these should individually cause black BG on frame 0 once Game2_0 runs (because `Bg_Texture_Load_EX` and `Bg_Kakikae_Set` rewrite them). But pre-match overlay ticks (Game12 and Game01 frames before Game2_0) may read unreset state.

### 4.4 Does `setup_vs_mode` call `Bg_Disp_Switch(1)`?

No. It calls `System_all_clear_Level_B` → `Bg_Close` → `bg_disp_off = 0`. It never sets `bg_disp_off` to 1 or calls `Bg_Disp_Switch(1)`. The BOTH-PEERS-SYMMETRICALLY-BLACK-FROM-INIT symptom does NOT match a `Bg_Disp_Switch(1)` path fired during init.

## Section 5 — Nav machine + character/stage selection

### 5.1 Does nav pick characters?

No. `netplay_nav.c` only drives the menus up to Mode_Select's Versus choice. After `NAV_START_NETPLAY` fires `Netplay_BeginDirectP2P`, nav is `NAV_DONE`. Character select is driven by the LIVE player inputs delivered through GekkoNet (`advance_game` at `src/netplay/netplay.c:591-604` injects `inputs[0]/inputs[1]` as `p1sw_0/p2sw_0`).

### 5.2 Does nav select a stage?

No. Nav does NOT touch `VS_Stage`. `setup_vs_mode` sets `VS_Stage = 0` (line 366). In MODE_NETWORK, `Setup_Battle_Country` (`src/sf33rd/Source/Game/screen/sel_pl.c:2013-2035`) does NOT consult `VS_Stage` (that branch is `Mode_Type == MODE_VERSUS`-only). The stage comes from `My_char[New_Challenger]` — the challenger's selected character number.

Therefore: whatever character the "challenger" (P1 if `New_Challenger==0`, P2 if `New_Challenger==1`) picked determines the stage. Both peers run the same `setup_vs_mode`, same `New_Challenger` initial value (zero from BSS or prior demo), same `My_char[New_Challenger]` after character select (because Gekko delivers the same inputs to both peers). Stage selection is deterministic and identical.

### 5.3 Is there a stage-select UI in the net flow?

No. `Exit_1st` (`src/sf33rd/Source/Game/screen/sel_pl.c:1540-1570`):
```c
if (Mode_Type == MODE_VERSUS && save_w[Present_Mode].Handicap != 0) {
    Exit_No = 7;                 // jumps to Handicap flow (stage select)
} else {
    Exit_No++;                   // goes to Exit_2nd (no stage select)
}
```
Because `Mode_Type == MODE_NETWORK` (not MODE_VERSUS), this evaluates to FALSE and jumps directly to Exit_2nd regardless of Handicap. So there is no user-visible stage selection step; `bg_w.stage` is determined entirely by `My_char[New_Challenger]`.

### 5.4 Timing difference between peers?

Nav-machine button presses are local — each peer arms nav independently via `NetplayNav_Arm()` (`src/main.c` cold-launch path). But `advance_game` delivers the SAME `inputs[0]/inputs[1]` to BOTH peers. So the nav injections only matter pre-session (before `configure_gekko`). Once CONNECTING starts, every frame's logical advance is driven by Gekko-delivered inputs; local nav state at this point is `NAV_DONE` and inert.

So nav timing differences between peers cannot cause post-`configure_gekko` divergence.

## Section 6 — Stage-load "invalid input" behavior

### 6.1 Stage 0 exists

`stage_bgw_number[22][3]` at `src/sf33rd/Source/Game/stage/bg_data.c:49` — index 0 defines Alex's stage. `stage_opaque[22]` at `src/sf33rd/Source/Game/stage/bg_data.c:410`. `bg_index_tbl[22][3]` at `src/sf33rd/Source/Game/stage/bg_data.c:537`. All define index 0.

If `bg_w.stage == 0`, `bg_initialize` sets `bg_w.bg_index = bg_index_tbl[0][0]`, `bg_w.scno = use_scr[0]`, etc. — all valid. `Bg_Texture_Load_EX` runs normally; stage 0 palette/tilemap loads.

### 6.2 Out-of-range stage

`bg_w.stage` is `s8` (signed 8-bit). Range `-128..127`. `stage_bgw_number[bg_w.stage][stg]` with `bg_w.stage >= 22` or `bg_w.stage < 0` is OUT OF BOUNDS — reads garbage. Would produce garbage tile-map / palette offsets.

Is there a code path that writes `bg_w.stage > 21 || bg_w.stage < 0` during netplay init? 

`Setup_Battle_Country` returns u8 (0..255). `My_char[New_Challenger]` is u8 (0..17 for regular roster, possibly higher for bosses). Let me audit the characters:

Per `src/sf33rd/Source/Game/screen/sel_pl.c:2025`: `My_char[0] == 17 && My_char[1] == 17` — Q is 17. Per line 2030 `My_char[New_Challenger] == 17` → fallback. So `My_char[]` includes values 0..19 (various characters including shadow variants). If `My_char[new_challenger]` is 18 (Twelve) or 19 (Urien), these ARE valid stage indices too. If 20 or 21, those are bonus stages. If > 21, invalid.

Character `0..17` roster maps to stages `0..17` for `Battle_Country`. Stage indices 18, 19 are bosses (Gill, Urien). 20 is bonus. 21 is bonus 2. Stage 22+ does not exist — `stage_bgw_number[22]` has only 22 entries. Going past end is undefined behavior.

**But this would apply to Mac↔Mac too**, so it's not MiSTer-specific.

### 6.3 Cold-boot `bg_w.stage`

`bg_w` is BSS. `bg_w.stage == 0` at cold boot. First match: `bg_w.stage` gets set either by `My_char[New_Challenger]` in netplay or by other paths. Valid stage in all paths.

### 6.4 Hypothesis verdict

**No code-visible path produces invalid `bg_w.stage` on MiSTer specifically**. The "stage-load invalid input" hypothesis has no supporting evidence as the dominant mechanism.

## Section 7 — `Bg_Disp_Switch` from init-time angle

### 7.1 All writes to `bg_disp_off`

(Confirmed via `grep -rn 'Bg_Disp_Switch\|bg_disp_off\s*=' src/sf33rd/Source/Game/`):

| Location | Write | Context |
|----------|-------|---------|
| `src/sf33rd/Source/Game/effect/effk8.c:27` | `Bg_Disp_Switch(1)` | Gill seraph enter. routine_no[0]==0 |
| `src/sf33rd/Source/Game/effect/effk8.c:38` | `Bg_Disp_Switch(0)` | Gill seraph exit on dead_f |
| `src/sf33rd/Source/Game/effect/effk8.c:52` | `Bg_Disp_Switch(0)` | Gill seraph exit on dir change |
| `src/sf33rd/Source/Game/stage/bg.c:240` | `bg_disp_off = 0` | `Bg_Close` |
| `src/sf33rd/Source/Game/stage/bg_sub.c:1111` | `bg_disp_off = 0` | `bg_initialize` |
| `src/sf33rd/Source/Game/stage/bg_sub.c:1202` | `bg_disp_off = 0` | `bg_etc_write` |
| `src/sf33rd/Source/Game/ending/end_main.c:250` | `bg_disp_off = 0` | ending path |
| `src/sf33rd/Source/Game/stage/bg.c:1508` | `bg_disp_off = on_off` | `Bg_Disp_Switch` body |

**There is no init-time path that sets `bg_disp_off = 1`**. The only setter is `effect_K8_move` (Gill seraph mid-round). Therefore the frame-0 black BG is NOT caused by an init-time `bg_disp_off == 1`.

### 7.2 Rollback restoration of `bg_disp_off`

`GS_SAVE(bg_disp_off)` at `src/netplay/game_state.c:596` and `GS_LOAD(bg_disp_off)` at `src/netplay/game_state.c:1278`. The field IS saved and restored on rollback — so even if a mid-match `Bg_Disp_Switch(1)` fired, the loaded state would restore it correctly on rollback.

The field is NOT in the checksum whitelist (`src/netplay/game_state.c:1668-1727`). So a divergence in `bg_disp_off` would be silent.

### 7.3 Could a PORT_MISTER-specific code path toggle it once at game load?

Grep `bg_disp_off` across `src/port/`: zero hits. No port-side write to `bg_disp_off`. The field is written ONLY by the locations in 7.1.

**Conclusion**: `bg_disp_off == 1 at frame 0` is ruled out as a mechanism for BG-black-from-start. The init paths all set it to 0.

## Ranked candidate mechanisms

Each candidate must explain:
- (i) BG black from frame 0 of match (not mid-match).
- (ii) Both peers affected (Mac and MiSTer in mixed; both MiSTer in same-arch).
- (iii) Only when 32-bit peer is present. Mac↔Mac does not reproduce.

### Candidate A — `Screen_Switch_Buffer == 0` at frame 0 because LDREQ color/tex didn't drain

**Mechanism**: `Exit_6th` (`src/sf33rd/Source/Game/screen/sel_pl.c:1685-1707`) uses `Check_LDREQ_Queue_BG(bg_w.stage + 0)` to gate progression. `q_ldreq[16]`, `ldreq_result[294]`, `ldreq_break` are NOT in the GameState (grep `q_ldreq|ldreq_break|ldreq_result src/netplay/game_state.c` returns zero hits). On a rollback, the queue advances are NOT restored. If rollback occurs after Exit_6th passed through on the live-play side, the resim sees `Check_LDREQ_Queue_BG` returning "loaded" (because queue is still drained from the real-play pass) and proceeds through Exit_6th without actually doing a fresh load. 

However: the LDREQ queue drives `init_trans_color_ram` which populates `ColorRAM[]`. If during resim the engine "skips past" the LDREQ wait because the queue is already drained, but the ROLLBACK did NOT re-write `ColorRAM[]`, then `ColorRAM[]` state is the one from the moment the live pass completed the LDREQ. This state persists. So actually the palette WOULD be valid on resim.

Unless the live pass never actually completed the load (e.g., if game2_0 fired without a real LDREQ completion because the queue was ALREADY drained on entry from a prior game session). Very hard to construct.

**Fit**: (i) plausible if LDREQ queue was empty when Game2_0 entered, so `Check_LDREQ_Clear() == 0` would panic (`fatal_error`). Not silent-black.

**Fit against MiSTer↔MiSTer both-black + 32-bit correlation**: Not clear. The slower MiSTer CPU produces more Gekko rollbacks, but a rollback can't bypass `Check_LDREQ_Clear()` because that's a hard `fatal_error`.

**Rank: Low**.

### Candidate B — PPG texture cache / handle invalidation across rollback

**Mechanism**: `Bg_Close` at Game2_0 release PPG BG texture handles. `Bg_Texture_Load_EX` allocates new handles and calls `flCreateTextureHandle` (`src/sf33rd/AcrSDK/ps2/flps2vram.c:25-44`) synchronously to upload + create. `tex->handle[i].b16[0]` holds the resulting handle number.

`tex` and `handle[]` are pointed at by `ppgBgList[i].tex` (a `Texture*`). `ppgBgList[]` itself lives in `ppg_w` (not in GameState). So across a rollback, the `ppgBgList[i].tex` pointer is the SAME pointer. But the texture it points to was re-allocated via `ppgMallocF` / `ppgSetupTexChunk_1st`. If live-play ran `Bg_Close` + `Bg_Texture_Load_EX` at time T, then GekkoNet's rollback resnaps to T-N (before Game2_0's texture tear-down/setup), then advance replays T-N → T, the replay re-calls `Bg_Close` + `Bg_Texture_Load_EX`. Each call allocates new handles. The old ones LEAK.

Because `ppgCheckTextureNumber` (`src/sf33rd/Source/Common/PPGFile.c:1840-1866`) checks `tex->be == 0` first (returns 0 if not ready), and `Bg_Texture_Load_EX` re-sets `tex->be = 1` after completion, the gate should pass — BUT only if `ppgSetupTexChunk_1st` completes.

`ppgSetupTexChunk_1st` calls `ppgMallocF` for the handle array and offset array. `ppgFree` is called inside `Bg_Close` via `ppgReleaseTextureHandle`. If the handle LIST has not been cleaned properly between resims, a re-call could hit a corrupted state.

**On 32-bit vs 64-bit**: `sizeof(TextureHandle*)` is 4 bytes on 32-bit, 8 bytes on 64-bit. `ppgMallocF(ixNums * sizeof(TextureHandle))` allocates correctly for each arch — but if a rollback-induced re-allocation exhausts `ppg_w`'s pool, the second allocation returns NULL → `goto error_handler` at `src/sf33rd/Source/Common/PPGFile.c:1320-1331` which ends in `while (1) {}` — infinite loop. That would hang, not render black.

**Fit**: (i) possible — if handles drift, `tex->handle[ix].b16[0] == 0` on resim → `ppgCheckTextureNumber` returns 0 → tile skipped. (ii) symmetric because both peers roll back identically. (iii) MiSTer being slower causes more rollbacks, triggering more re-enters.

**Rank: Medium — but doesn't cleanly explain MiSTer↔MiSTer both-black from frame 0 since Mac↔Mac with heavy link conditioning also rolls back and doesn't show black.** Unless texture cache state differs between arches (e.g., 32-bit hits handle exhaustion faster due to smaller malloc pool or different alignment waste).

### Candidate C — `frw[EFFECT_MAX][448]` storage size vs actual `sizeof(WORK)` mismatch on 32-bit

**Mechanism**: `frw[EFFECT_MAX][448]` is declared as `uintptr_t frw[128][448]`. Each slot is `448 * sizeof(uintptr_t)` bytes:
- 32-bit: 448 × 4 = **1792 bytes per effect slot**.
- 64-bit: 448 × 8 = **3584 bytes per effect slot**.

`sizeof(WORK)` — the struct stored in each slot — has many pointer fields (`u32* char_table[12]` alone is 48 bytes on 32-bit or 96 bytes on 64-bit). Let's roughly compute the struct on 32-bit: at a minimum, WORK has ~30+ pointer-typed fields (see `include/structs.h:280-448` for the full list including `char_table[12]`, `se_random_table`, `step_xy_table`, `move_xy_table`, `overlap_char_tbl`, `olc_ix_table`, `rival_catch_tbl`, `curr_rca`, `set_char_ad`, `hit_ix_table`, `body_adrs`, `h_bod`, `hand_adrs`, `h_han`, `dumm_adrs`, `h_dumm`, `catch_adrs`, `h_cat`, `caught_adrs`, `h_cau`, `attack_adrs`, `h_att`, `h_eat`, `hosei_adrs`, `h_hos`, `att_ix_table`, `my_effadrs` — 27 pointers) plus many scalars.

On 32-bit, WORK should fit comfortably in 1792 bytes. On 64-bit, WORK is bigger due to pointer bloat (27 × 4 extra bytes = +108 bytes from pointers alone; plus alignment padding). 1792 bytes is likely TOO SMALL for WORK on 64-bit.

Wait — the 64-bit array is 3584 bytes per slot, which is plenty. The 32-bit array is 1792 bytes per slot, which is also plenty for 32-bit WORK.

So in each arch's own slot size, WORK fits. There's no overflow. Both architectures allocate `EFFECT_MAX * sizeof(uintptr_t) * 448` — but with different `sizeof(uintptr_t)`, the slot SIZE is different. The WORK that fits in it is also sized differently (smaller on 32-bit because of smaller pointers). They're self-consistent within each arch.

**Fit**: This only matters for cross-arch state size mismatches, which is the documented 32↔64-bit incompat. For MiSTer↔MiSTer (both 32-bit) same-arch it doesn't apply. **Does not fit** the MiSTer↔MiSTer all-black symptom.

**Rank: Low for THIS investigation scope**.

### Candidate D — Unrestored `ppg_w.cur` / renderer-side texture state between load_state calls

**Mechanism**: `ppgSetupCurrentDataList(&ppgBgList[stg])` writes to `ppg_w.cur` — a module-level pointer in `src/sf33rd/Source/Common/PPGWork.c`. This is NOT in `GameState`. If `ppg_w.cur` points to the wrong list when `bgDrawOneChip` runs, `ppgCheckTextureNumber(0, gbix)` resolves to the wrong texture. If the current list happens to not have the tile index, returns 0 → tile skipped.

**Fit**: (i) possible — a rollback + advance pattern could leave `ppg_w.cur` pointing at `ppgRwBgList` (the rewrite list) from a prior sim, and the current sim tile lookup falls into that list and doesn't find the tile. (ii) symmetric because both peers follow identical advance patterns. (iii) MiSTer-specific correlation unclear; likely the same on Mac with enough rollbacks.

**Rank: Low-Medium** — doesn't cleanly produce "black from frame 0, not later".

### Candidate E — `ColorRAM[]` not populated when Game2_1 runs on MiSTer (timing/LDREQ race)

**Mechanism**: Main BG palette comes from `palUpdateGhostCP3` reading `ColorRAM[]`, which is populated only by `init_trans_color_ram` (via LDREQ). If on MiSTer the LDREQ for the stage's color file has NOT completed by the time `Game2_1`'s `BG_Draw_System` runs, the palette is pre-match state (zero or stale). Stale might match; zero → all-black pixels even if texture tiles render.

But `Exit_6th` gates progression on `Check_LDREQ_Queue_BG(bg_w.stage + 0)`. This waits for the request queued in Exit_2nd. So LDREQ should be drained before Game2_0 runs.

HOWEVER, if rollback happens during the sel_pl.c Exit flow, the LDREQ state is not restored by `load_state`. If live-play completed the load at frame F, and rollback pushes back to F-10 (before load completed), resim of F-10..F will NOT re-issue the load (because Exit_2nd's `Push_LDREQ_Queue_BG` only runs when `Select_Status[0] == 3` etc., which is re-computed from restored state — OK, that's deterministic). The load WILL re-push into the queue. The queue will drain on subsequent frames just like live-play.

But there's a subtlety: `Check_LDREQ_Queue_BG(bg_w.stage + 0)` returns true if the request for that specific stage has completed. After rollback, the previous completion flag in `ldreq_result[]` might still be set (since `ldreq_result` is not rolled back). So on resim, Exit_6th passes immediately even though the load has not been redone. Then Game2_0 runs — `Check_LDREQ_Clear()` might return true (empty queue) even though ColorRAM hasn't been updated for the current context.

Actually — if no new push was issued (because the resim saw ldreq_result already has the "done" flag), then `ColorRAM[]` is STILL correctly populated from the live-play pass. The palette is valid. So NOT a frame-0 mechanism.

Unless the resim's push-and-drain pattern leaves `ColorRAM[]` corrupted. Or unless NEW pushes overwrite the just-completed pushes' results and leave `ColorRAM[]` partially written.

**Fit**: (i) possibly. (ii) symmetric. (iii) MiSTer-specific because more rollbacks occur on MiSTer due to slower CPU, increasing the chance of hitting a rollback-resim timing window where color data doesn't land correctly.

**Rank: Medium**.

### Candidate F — `Demo_Flag == 1` path in Game01 default case skips Exit_1st/Exit_2nd entirely

**Mechanism**: `setup_vs_mode` sets `Demo_Flag = 1` (`src/netplay/netplay.c:201`). After character select completes via `Select_Player()`, Game01 default case (`src/sf33rd/Source/Game/game.c:399-413`) sees `Demo_Flag == 1` and jumps `G_No[1] = 2; G_No[2] = 0;` WITHOUT traversing the else branch (which would have been for the demo deactivation path). But this jump happens AFTER `Switch_Screen(0) != 0`, which itself requires `Select_Player()` to advance its internal Exit_* state machine.

`Select_Player` returning means its internal Exit_No has advanced to completion. Let me verify the actual control flow.

`Select_Player` at `src/sf33rd/Source/Game/screen/sel_pl.c:146` — this is an `s16 Select_Player()` function that drives the state machine internally. Each tick advances `Exit_No` etc. until it returns a terminal value. Inside Select_Player, Exit_2nd IS reached if Select_Status indicates completion. So `bg_w.stage` IS updated.

Actually Demo_Flag's impact in Game01 default case is ONLY to route the G_No advancement (`G_No[1]=2` vs deactivating players). It doesn't bypass the `Select_Player()` call itself. So the sel_pl.c Exit_* path executes during `Select_Player()` regardless of `Demo_Flag`.

So Exit_2nd DOES run. `bg_w.stage` IS set. `Push_LDREQ_Queue_BG(bg_w.stage + 0)` IS called. Exit_6th DOES wait for the queue drain.

**Fit**: Demo_Flag doesn't on its own skip BG setup. Ruled out.

**Rank: Low — wrongly hypothesized**.

### Candidate G — `Bg_Texture_Load_EX` at Game2_0 fails to run because `bg_initialize`'s early-exit gate misreads G_No

**Mechanism**: Reading `bg_initialize` at `src/sf33rd/Source/Game/stage/bg_sub.c:1118-1120`:
```c
if (G_No[0] != 2 || G_No[1] != 2 || G_No[2] != 2) {
    Bg_Texture_Load_EX();
}
```

At Game2_0 entry: `G_No[0]=2, G_No[1]=2, G_No[2]=0`. So `(false || false || true) == true` → Bg_Texture_Load_EX runs. GOOD.

But: look at when TATE00 is called. Game2_0 calls TATE00 → ta0_init00 → bg_initialize. But Game2_0 is running in the current frame; `G_No[2]` was `0` on entry and gets set to `3` at line 505 BEFORE TATE00 is called at line 528.

Wait, rereading (`src/sf33rd/Source/Game/game.c:442-529`):
```c
void Game2_0() {
    s16 ix;
    BG_Draw_System();
    Switch_Screen(0);
    if (Check_LDREQ_Clear() == 0) fatal_error(...);
    System_all_clear_Level_B();
    switch (Mode_Type) { ... }
    Check_Replay();
    if (Demo_Flag == 0) { Play_Mode = 0; ... }
    Game_difficulty = 15;
    Game_timer = 0;
    Game_pause = 0;
    Demo_Time_Stop = 0;
    C_No[0] = 0; ...
    G_No[2] = 3;                 // ← G_No[2] set to 3 HERE
    G_Timer = 10;
    ...
    bg_work_clear();
    win_lose_work_clear();
    player_face_init();
    TATE00();                    // ← bg_initialize called here
}
```

So by the time `bg_initialize` runs, `G_No[2] == 3`. The gate condition `G_No[0] != 2 || G_No[1] != 2 || G_No[2] != 2` = `(0 || 0 || 1) = true` → `Bg_Texture_Load_EX` runs. GOOD.

But wait — this is the FIRST time `bg_initialize` runs. Let me also check: what if `bg_initialize` runs again before Game2_0's texture load completes? `bg_routine = 0` at Game2_0 time. After `bg_initialize`, `bg_routine` becomes `1`. Subsequent frames run `ta0_init01` (not `bg_initialize`).

**Fit**: This path fires correctly. Ruled out.

### Candidate H — State snapshot taken at `configure_gekko` captures a cleared-but-not-reloaded BG state; rollbacks resnap to this snapshot

**Mechanism**: `configure_gekko` runs at `G_No[1]==1` (character select, but early — `transition_ready_frames == 2`). At that moment, state is:
- `Screen_Switch = 0` (cleared by earlier `Bg_Close` via `setup_vs_mode`)
- `Screen_Switch_Buffer = 0`
- `bg_disp_off = 0`
- `bg_w.stage = 0` (or whatever from prior state; not reset by `setup_vs_mode`)

First `GekkoSaveEvent` captures this. Gekko uses this as the initial "known good" state.

Now during character select, frames advance. Exit_2nd runs → `bg_w.stage = My_char[New_Challenger]`, `Push_LDREQ_Queue_BG`. Exit_6th waits, Game2_0 runs, `Bg_Texture_Load_EX` fires, `Screen_Switch` gets bits set.

But at some point, Gekko MIGHT issue a `GekkoLoadEvent` for rollback. `load_state` restores `bg_w` (including `bg_w.stage`), `Screen_Switch`, `Screen_Switch_Buffer`, `bg_disp_off` from the LATER snapshot — but Gekko's save/load is frame-keyed. Rollback to frame F loads the state SAVED at frame F.

If the save was well-formed (captured post-`Bg_Texture_Load_EX` state), load restores `Screen_Switch = 0b111` etc. Renderer's PPG cache is still populated. All good.

UNLESS the save happened at a frame where `Screen_Switch == 0` because `Bg_Close` had just fired but `Bg_Texture_Load_EX` hadn't run yet. This is a 1-frame window at Game2_0's System_all_clear_Level_B+System_all_clear_Level_B→`Bg_Close` (clearing), before TATE00 runs bg_initialize.

Actually looking at Game2_0: it runs in a single frame. Call sequence within single `step_game`:
1. `BG_Draw_System()` — with OLD state (stale)
2. `Switch_Screen(0)`
3. `Check_LDREQ_Clear()`
4. `System_all_clear_Level_B()` — **Screen_Switch → 0**
5. Mode-specific setup
6. Many zeroes
7. `bg_work_clear()`
8. `TATE00()` → `ta0_init00` → `bg_initialize` → **Bg_On_R** → Screen_Switch bits set.

All within one frame. The save happens AT FRAME BOUNDARY (end of frame). So if save captures end-of-frame state, `Screen_Switch` is `0b001..0b111` (set) at save time. Load restores it. Good.

BUT — if the frame in which Game2_0 ran is the SAME frame as the save-after-advance, but via `sysFF > 1` multiple advance loops happen per save (`Game_Task` at `src/sf33rd/Source/Game/game.c:142-174` iterates `for (ix = 0; ix < ff; ix++)`), then MULTIPLE game dispatches happen in one "frame" for Gekko. Normally `sysFF == 1` so one dispatch per frame. If `sysFF` changes due to slow-motion or pause, multiple dispatches occur.

Gekko sees frames as atomic. The save/load is atomic too. So within one frame, the state transitions are all-or-nothing.

**Fit**: (i) possible only if there's a mid-frame save/restore window where state is partially updated. Unlikely given the atomic per-frame contract.

**Rank: Low** — doesn't hold up under scrutiny.

### Candidate I — `Demo_Flag == 1` mid-select: `Game01_Sub` sets both `wu_operator = 0` on line 409-412, but only for NON-Demo_Flag branch

Reading again (`src/sf33rd/Source/Game/game.c:399-413`):
```c
if (Demo_Flag) {
    G_No[1] = 2; G_No[2] = 0; G_No[3] = 0;
    E_No[0] = 4; E_No[1] = 0; E_No[2] = 0; E_No[3] = 0;
} else {
    Demo_Time_Stop = 1;
    plw[0].wu.wu_operator = 0;
    Operator_Status[0] = 0;
    plw[1].wu.wu_operator = 0;
    Operator_Status[1] = 0;
}
```

With Demo_Flag==1, players STAY as operator==1 (active). Without, they deactivate. In netplay (Demo_Flag==1), both players stay active — which is what we want. So this doesn't break BG.

Ruled out.

### Candidate J — Per-arch `sizeof(State)` vs GekkoNet buffer allocations

**Mechanism**: `config.state_size = sizeof(State)` (`src/netplay/netplay.c:483`). On 32-bit:
- `sizeof(GameState) == 17580` (pinned in `src/netplay/game_state.c:52`).
- `sizeof(EffectState)` = `s16 frwctr + s16 frwctr_min + s16 head_ix[8] + s16 tail_ix[8] + s16 exec_tm[8] + uintptr_t frw[128][448] + s16 frwque[128]`:
  - 32-bit: 2 + 2 + 16 + 16 + 16 + (128*448*4) + 256 = 229,684 bytes approximately.
  - 64-bit: 2 + 2 + 16 + 16 + 16 + (128*448*8) + 256 = 459,060 bytes approximately.
- `sizeof(State)` = `sizeof(GameState) + sizeof(EffectState)` plus padding.

This has LAYOUT IMPLICATIONS inside `GameState`. BG-relevant fields live inside `GameState` — their OFFSETS within GameState depend on prior field sizes. Let me check: are all `GameState` fields scalar types (no pointers)?

Quick scan of `src/netplay/game_state.h:29-714`: most fields are `s8/u8/s16/u16/u32` and fixed-size arrays thereof. No pointers. But `BG bg_w;` at line 533 — does BG contain pointers?

From `src/sf33rd/Source/Game/stage/bg.h:7-88`:
```c
typedef struct { ... u16* bg_address; u16* suzi_adrs; ... u16* start_suzi; ... u16* suzi_adrs2; u16* start_suzi2; ... s16* deff_rl; s16* deff_plus; s16* deff_minus; ... } BGW;
typedef struct { ... BGW bgw[7]; ... } BG;
```

**BGW HAS POINTERS**. `bg_w` is 7 × BGW + other fields. On 32-bit, each BGW contains pointer fields at 4-byte size. On 64-bit, they're 8 bytes with 8-byte alignment → larger struct.

Therefore `sizeof(BG)` differs between 32-bit and 64-bit, and so does `sizeof(GameState)`.

**Cross-arch**: when GekkoNet sends a state bit over the wire (for desync detection checksum), the checksum is computed over `sizeof(GameState)` bytes. Different sizes → different checksums → desync. This is the documented incompatibility.

**Same-arch 32-bit vs 64-bit**: within MiSTer↔MiSTer (both 32-bit), same layout, same offsets. Not a problem.

**Fit**: 32-bit specific bugs could hypothetically exist if `GameState` layout on 32-bit triggers a specific value pattern that on 64-bit doesn't trigger. E.g., uninitialized padding bytes read as "black palette" indirectly. No concrete mechanism found.

**Rank: Low-Medium**. The 32-bit correlation invites this hypothesis but no direct mechanism found.

### Candidate K — `rw_dat[20]` rollback corruption on 32-bit specifically

`rw_dat` is `RW_DATA rw_dat[20]`. Each `RW_DATA` (`include/structs.h:1566-1573`):
```c
typedef struct {
    u8 bg_num;
    const s16* rwd_ptr;
    const s16* brw_ptr;
    s16 rw_cnt;
    s16 rwgbix;
    s16 gbix;
} RW_DATA;
```

On 32-bit: `sizeof(RW_DATA) = 1 (bg_num) + 3 (pad) + 4 (rwd_ptr) + 4 (brw_ptr) + 2 (rw_cnt) + 2 (rwgbix) + 2 (gbix) + 2 (pad) = 20 bytes`.
On 64-bit: `sizeof(RW_DATA) = 1 + 7 (pad to 8) + 8 + 8 + 2 + 2 + 2 + 9 (pad to 8) = ~40 bytes`.

`GS_SAVE(rw_dat) = memcpy(&dst->rw_dat, &rw_dat, sizeof(rw_dat))`. On each arch, sizeof matches. Save/load within same arch = consistent.

BUT on restore, `rwd_ptr` and `brw_ptr` are written back as bitwise-copied pointer values. These pointers were originally set by `Bg_Kakikae_Set` to addresses inside RODATA (`rw30`, `rw190`, etc.). The pointer values saved = valid local addresses. On restore (same process), they remain valid. No cross-peer transmission; this is local rollback.

Cross-arch (MiSTer↔Mac) — different story, but that's the desync problem, not BG-black.

**Fit**: Within 32-bit same-peer rollback, `rw_dat` should restore correctly.

**Rank: Low**.

### Candidate L — Texture handle allocation pool exhaustion on MiSTer due to repeated Bg_Close/Bg_Texture_Load_EX cycles under rollback

**Mechanism**: `FL_TEXTURE_MAX` is fixed (defined elsewhere, probably 1024 or similar). `flPS2GetTextureHandle` (`src/sf33rd/AcrSDK/ps2/flps2vram.c:160-174`) scans `flTexture[]` for free slot. `ppgReleaseTextureHandle` frees handles on `Bg_Close`. 

Under frequent rollback, Bg_Close + Bg_Texture_Load_EX runs repeatedly. If the free/alloc contract has subtle bugs — e.g., a handle is not actually freed before reallocation — we exhaust the pool and `flCreateTextureHandle` returns 0. Then `tch->handle[i].b16[0] = 0` in `ppgSetupTexChunk_3rd`. Then `ppgCheckTextureNumber` returns 0. **Tile not drawn. BG black.**

This would be deterministic: both peers roll back similarly (they see identical input streams), both exhaust handle pool similarly, both have black BG.

Why MiSTer-specific? MiSTer CPU is ~2-3x slower than a Mac ARM. More rollbacks per unit time. Hit pool exhaustion before Mac↔Mac does. For MiSTer↔Mac (cross-arch), each peer hits it at their own pace, but because the cross-arch desync fires fast (checksum mismatch), the session tears down quickly — yet the symptom is that during the few frames before teardown, BG is already black.

**Fit**: (i) yes, reasonable — handle pool exhaustion in pre-match rollbacks → `tex->handle[i].b16[0] == 0` → tiles not drawn from frame 0 of match. (ii) symmetric between peers. (iii) MiSTer-specific because slower CPU triggers more rollbacks.

**Rank: High-Medium** — fits all three criteria but requires empirical confirmation that (a) rollbacks actually occur pre-match in high numbers, (b) handle pool actually exhausts.

### Candidate M — BG texture load failure on MiSTer due to memory/ramcnt pressure

**Mechanism**: `Bg_Texture_Load_EX` uses `Search_ramcnt_type(0x12)` to find the BG texture. If it returns 0 (no matching key), `Get_ramcnt_address(key1)` fires `ERR_STOP` — while(1) hang. Not black BG, but hang.

But `Search_ramcnt_type` could return a stale key pointing to data NOT containing the current stage's textures (e.g., if rollback restored a state where the ramcnt was pointing at a different stage). Then `ppgSetupTexChunk_1st` parses the wrong PPG file → maybe fails, maybe works with wrong content.

The ramcnt state is NOT in `GameState`. Grep `rckey_work` in `src/netplay/game_state.c`: zero hits. So this key-to-address mapping is NOT rolled back.

If the LIVE run loaded stage 0's BG into ramcnt key K, and a rollback restores `bg_w.stage = 0`, then on resim `Bg_Texture_Load_EX` retrieves key K again — same mapping, same data. Good.

But what if during rollback, `Bg_Texture_Load_EX` gets CALLED MULTIPLE TIMES, and each call allocates NEW ramcnt-managed memory via `ppgMallocF`? These allocations may NOT be freed between calls, leaking memory. Eventually malloc fails → hang or invalid pointer returns.

**Fit**: (i) plausible if rollback triggers cascading allocations. (ii) symmetric. (iii) MiSTer-specific (less available memory; different alloc behavior).

**Rank: Medium**.

### Candidate N — Incomplete field reset in `setup_vs_mode` leaves `bg_w` at a stale state that defeats `bg_initialize`

**Mechanism**: `bg_w.bg_routine` is not zeroed by `setup_vs_mode`. If prior demo left `bg_w.bg_routine == 3` (steady state), and then character select runs under Gekko (which snapshots the NON-zeroed `bg_w.bg_routine`), then when Game01 default case jumps to `G_No[1]=2` and `G_No[2]=0` (Game2_0), Game2_0 calls `bg_work_clear()` which sets `bg_w.bg_routine = 0`. Then TATE00 runs ta0_init00 → `bg_initialize` which includes `Bg_Texture_Load_EX`.

So `bg_routine` IS reset by `bg_work_clear` at Game2_0 entry. Not a problem for frame-0 of match.

**Fit**: Ruled out.

### Summary of ranking

| Rank | Candidate | Explains (i) | Explains (ii) | Explains (iii) |
|------|-----------|--------------|--------------|--------------|
| High-Medium | L. Texture handle pool exhaustion under MiSTer rollback pressure | Yes | Yes | Yes (timing-dependent) |
| Medium | M. BG texture ramcnt/malloc pressure on MiSTer | Yes | Yes | Yes (memory-dependent) |
| Medium | E. ColorRAM not populated when Game2_1 runs on MiSTer | Yes | Yes | Partially |
| Medium | B. PPG texture cache handle drift across rollback | Partial | Yes | Partially |
| Low | A. Screen_Switch_Buffer=0 because LDREQ didn't drain | No | Yes | Unclear |
| Low | D. `ppg_w.cur` unrestored between load_state calls | Partial | Yes | Unclear |
| Low | C. `frw[][448]` size mismatch | No | N/A | Same-arch N/A |
| Low | H. Snapshot at configure_gekko with empty-BG-state | No | N/A | N/A |
| Low | J. `sizeof(State)` arch layout | Unclear | N/A | Weakly |
| Low | K. `rw_dat` rollback corruption | No | N/A | N/A |
| Ruled out | F. Demo_Flag skipping Exit_2nd | Wrongly modeled | — | — |
| Ruled out | G. bg_initialize gate | Correctly fires | — | — |
| Ruled out | I. Demo_Flag deactivating operators | Opposite path | — | — |
| Ruled out | N. bg_w.bg_routine stale | Reset by bg_work_clear | — | — |

**No single candidate dominates** as a provable root cause given only static code inspection. The top candidates (L, M, E, B) all require timing-dependent empirical measurement on MiSTer to confirm.

## Elimination experiments

These are code-inspection and minimal-instrumentation steps to disambiguate the remaining candidates. None are fixes.

### Experiment 1 — Log which gate fires in `bgDrawOneChip` on frame 0 of Game2_1

Add DEBUG log in `bgDrawOneChip` at `src/sf33rd/Source/Game/stage/bg.c:1145-1156` that fires ONCE per stage-start:
```
log: bgDrawOneChip(bgnm=X, gbix=Y) No_Trans=? ppgCheckTextureNumber(cur, Y)=?
     cur->tex->be=? cur->tex->handle[ix].b16[0]=?
```

This disambiguates:
- Texture not ready (`be==0`) → points to L, M, B (setup failed).
- Handle zero (`handle[ix].b16[0]==0`) → points to L (pool exhaustion).
- No_Trans==1 — means rendering is disabled → shouldn't happen at first drawn frame.
- `ppg_w.cur` wrong list → points to D.

### Experiment 2 — Log `Screen_Switch_Buffer` and `bg_disp_off` at BG_Draw_System entry

Add DEBUG log in `BG_Draw_System` at `src/sf33rd/Source/Game/system/sys_sub.c:899`:
```
log: BG_Draw_System Screen_Switch_Buffer=0x%x bg_disp_off=%d Play_Game=%d
```

If `Screen_Switch_Buffer==0` at frame 0 of match → points to A (LDREQ/state race), or setup path failure.
If `bg_disp_off==1` → points to initialization path setting it incorrectly (NOT in current code, so would be a smoking gun for unknown mechanism).
Both should be `0x7` (or similar non-zero) and `0` respectively.

### Experiment 3 — Log `bg_w.stage` at Game2_0 entry

Add DEBUG log at `src/sf33rd/Source/Game/game.c:442` (top of Game2_0):
```
log: Game2_0 entry bg_w.stage=%d bg_w.area=%d My_char[0]=%d My_char[1]=%d New_Challenger=%d Mode_Type=%d
```

Verifies stage selection made sense. If `bg_w.stage > 21` or `< 0`, we have an invalid stage.

### Experiment 4 — Log LDREQ queue state before `Check_LDREQ_Clear` in Game2_0

Add DEBUG log in `Game2_0` between `Switch_Screen` and `Check_LDREQ_Clear`:
```
log: Game2_0 LDREQ state: queue[0..15].be = [%d, %d, ...]
```

Detects whether the queue is actually drained cleanly pre-match.

### Experiment 5 — Count rollback events and texture allocation pool pressure

Add a counter:
- in `load_state_from_event` at `src/netplay/game_state.c:1773` — tracks rollbacks.
- in `flPS2GetTextureHandle` at `src/sf33rd/AcrSDK/ps2/flps2vram.c:160` — tracks alloc count and whether loop actually finds a free slot.

At end of match or at desync, dump counts. If MiSTer sees >>rollback count than Mac AND handle allocation pressure mounts, that's evidence for L.

### Experiment 6 — Snapshot `ColorRAM[0x12C..0x12C+1]` around Game2_0

Add a hex dump of `ColorRAM[0x12C][0..15]` (first 16 palette entries for BG palette offset 0x12C) at:
- Entry to Game2_0 (before `System_all_clear_Level_B`)
- After `Bg_Texture_Load_EX` returns
- At first BG_Draw_System in Game2_1

If `ColorRAM[0x12C][i]` is all zeros at the third snapshot → palette never loaded → BG renders black. Points to E (LDREQ/palette race).

### Experiment 7 — Check `ppg_w.cur` at entry to `scr_trans`

Add log at top of `scr_trans` (`src/sf33rd/Source/Game/stage/bg.c:553`):
```
log: scr_trans bgnm=%d ppg_w.cur=%p expected=%p (ppgBgList[%d])
```

Detects whether D (wrong current data list) is the mechanism.

## Unverified questions

To pick among the remaining top candidates (L, M, E, B), a DEBUG run would need to print:

1. **Rollback frequency and pattern on MiSTer** — does MiSTer see rollbacks pre-match? How many per character select? How many per frame during Game2_0's first few frames?

2. **Texture handle allocation accounting** — does `flPS2GetTextureHandle` return 0 ever? Does the count of active handles grow without bound across rollbacks?

3. **`ppgCheckTextureNumber` return value trace for BG tiles on frame-0** — which of the four early-exits in the function fires?

4. **`ColorRAM[0x12C..0x150]` contents around match start** — is the BG palette actually populated with the stage's colors?

5. **`ppg_w.cur` pointer at BG draw time** — is it pointing at the correct `ppgBgList[i]`?

6. **Per-section checksum drift pattern** — do the non-hashed BG state fields (`bg_w`, `Screen_Switch`, `bg_disp_off`) stay consistent between peers? If they diverge, the black-from-frame-0 is a determinism issue; if they stay identical, it's a local rendering/cache issue that happens to fire symmetrically on both MiSTer peers.

7. **On MiSTer↔MiSTer, is the black BG identical on both screens or different shades/patterns?** If identical → driven by (synchronized) sim-state. If different → local rendering issue triggered by similar timing on both.

## Summary

**Top candidate: Candidate L — PPG texture handle pool exhaustion or inconsistency triggered by repeated `Bg_Close`/`Bg_Texture_Load_EX` cycles across rollback resimulations, where the MiSTer peer's slower CPU forces more rollbacks per frame and the Mac peer in a cross-arch session is pulled into lockstep with the MiSTer's advance pattern via GekkoNet's shared input stream.** This fires from frame 0 of the match because Game2_0 is the point at which BG textures are torn down (`Bg_Close`) and re-setup (`Bg_Texture_Load_EX`), and if the setup fails to create a non-zero handle, `ppgCheckTextureNumber` returns 0 in `bgDrawOneChip` (`src/sf33rd/Source/Game/stage/bg.c:1146`), causing every BG tile to be skipped — producing all-black BG while HUD/sprites/effects (which use different texture paths) render normally.

**However, this is not proven by static code inspection.** No single candidate dominates unambiguously; the top four (L, M, E, B) all require empirical DEBUG measurement on MiSTer to disambiguate. The experiments in the previous section can discriminate among them in one test run.

**Caveat for the caller**: the prior investigation (`docs/research-desync-deep-investigation.md`) focused on mid-match BG-black via `bg_disp_off` or `Screen_Switch_Buffer` divergence during Gill seraph or similar effects. That mechanism is NOT the frame-0 mechanism — as confirmed by grep: the only `Bg_Disp_Switch(1)` is in `effect_K8_move`, which does NOT fire at init. So the new user observation (from-frame-0) correctly moves the investigation out of the "mid-match rollback" framing and into the "init-time or Game2_0 setup" framing. This report is oriented around the new framing.
