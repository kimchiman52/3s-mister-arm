# Deep Investigation — Black Background Bug (2026-04-24)
Date: 2026-04-24
Branch: `netplay-direct-only`
Investigator: fact-based code read, no runtime repro this pass.

---

## Executive summary

The BG pipeline has a **data-level inconsistency that only manifests for `bg_w.stage == 17`**. No prior attempt was "the bug"; they were all chasing symptoms. The mechanism is:

1. `Bg_Texture_Load_EX` loads exactly `bg_w.scrno` layers of texture data. For stage 17, `bg_w.scrno == 1` because `bg_index_tbl[17] == 4` (the **only non-identity remap in the 22-stage table**) and `use_real_scr[4] == 1`.
2. The same function then OR's `stage_bgw_number[bg_w.stage]` into `Screen_Switch` with no knowledge of how many layers were actually loaded. For stage 17 that sets bits 0 **and** 1 (`stage_bgw_number[17] == {1,2,0}`), asking the renderer to draw two layers.
3. `BG_Draw_System → scr_trans(1)` then binds `ppgBgList[1]` (whose `tex = &ppgBgTex[1]`) and asks every tile for a handle. `ppgBgTex[1].be == 0` because no loader ever touched it. Every tile returns 0 from `ppgCheckTextureNumber`, `bgDrawOneChip` skips every draw. The pixels covered by layer 1 stay at the SDL clear color, which is `flPs2State.FrameClearColor == 0` → **black**.

The bug is latent upstream too. It is reproducible on this port only because netplay is the first game mode in which `bg_w.stage` can be set to `17` with the rest of the stage pipeline alive (see §E / §F for the mode-gate analysis). Rollback adds no new mechanism — it only increases the chance that you *observe* the symptom because the stage-17 code path fires several times during a lag-induced resim.

**Minimal correct fix**: inside `Bg_Texture_Load_EX` (and the mirroring block inside `Game2_2`), turn the `Bg_On_R` loop from *"enable every layer that `stage_bgw_number` mentions"* into *"enable only the layers that were actually loaded"*, i.e. gate on `j < bg_w.scrno` (the same quantity the texture loop uses), not on `stage_bgw_number[bg_w.stage][i] > 0`. Exact diff in §G.

This fix is orthogonal to the `chainex_check[2][36]` rollback fix already shipped for desync.

---

## Summary of prior (failed) attempts

(Cross-referenced against `docs/fix-plan-bg-texture-rollback.md` and the prompt.)

| # | Change                                                                 | Observed result                                                                                 | Why it failed |
|---|------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------|---------------|
| 1 | **Fix B** — tear down `ppgBgTex / ppgRwBgTex / ppgAke* / ppgAkane*` handles at top of `Bg_Texture_Load_EX` (`src/sf33rd/Source/Game/stage/bg.c:307-317`) | No-op in practice — `Bg_Close` via `System_all_clear_Level_B` already cleared `be`. Still in tree, still harmless. | Addresses a real (different) rollback hazard but not THIS bug. Kept. |
| 2 | Swap `bg_w.scrno = use_real_scr[bg_w.bg_index]` → `use_real_scr[bg_w.stage]` (`src/sf33rd/Source/Game/stage/bg_sub.c:1114`) | Stage 17 reached `j=1`; `bgtex_stage_gbix[17][1] = 0x077FFFFF` requests 24 handles but the underlying file only has enough `pTEX` chunks for `use_real_scr[4]=1` screen → `ppgSetupTexChunk_2nd` hit the `while(1)` at `src/sf33rd/Source/Common/PPGFile.c:1358` ("Handle acquisition process has been called more times than the number of data stored in the texture chunk"). | `bg_w.scrno` must match how the **source ROM file** is laid out. That file is keyed by `bg_index`, not by `stage`. Reverted. |
| 3 | Drive `Bg_On_R` from `scrno+stg` instead of `stage_bgw_number`         | Kept `Screen_Switch` at 0x1 for stage 17 but BG still black AND characters shifted left at round start. | The black was real (see §C.5 — layer 0 tiles still fail `ppgCheckTextureNumber` for the 20 unset bits of `0x7E7E0000`; the remaining viewport stays clear-color). The "shifted left" was a regression from the scrno-driven `stg` reshuffle that also affected a family-setup path — inferred, see §D. Reverted. |
| 4 | Iterate the `scr_bcm[stg+i] = bg_map_tbl[...]` loop on `scrno` instead of `use_real_scr[bg_w.stage]` | "Characters shifted left at round start". | `scr_bcm[]` is a **write-only dead variable** in this port (§D). The shift-left could not have come from scr_bcm content. It came from the same scrno-based `stg` reshuffle that changed which `ppgBgList[stg]` slot got texture data. Reverted. |

Fix for `chainex_check[2][36]` (added to `GameState` in `src/netplay/game_state.{c,h}` at `EXPECTED_GAME_STATE_SIZE` bump 17580 → 17652) is a **separate bug** (desync) and must stay.

---

## Data-table audit (Section B)

All tables read at `src/sf33rd/Source/Game/stage/bg_data.c`. `diff` against `/tmp/3sxtra/src/sf33rd/Source/Game/stage/bg_data.c` is byte-identical for every table referenced below — **tables are upstream-faithful, not mis-ported.**

### B.1 `stage_bgw_number[22][3]` (bg_data.c:49-52)

```
stage_bgw_number[ 0] = { 1, 2, 0 }
stage_bgw_number[ 1] = { 0, 2, 0 }
stage_bgw_number[ 2] = { 1, 2, 3 }
stage_bgw_number[ 3] = { 1, 2, 0 }
stage_bgw_number[ 4] = { 0, 2, 0 }
stage_bgw_number[ 5] = { 1, 2, 0 }
stage_bgw_number[ 6] = { 0, 2, 0 }
stage_bgw_number[ 7] = { 1, 2, 0 }
stage_bgw_number[ 8] = { 1, 2, 0 }
stage_bgw_number[ 9] = { 1, 2, 0 }
stage_bgw_number[10] = { 1, 2, 0 }
stage_bgw_number[11] = { 0, 1, 0 }
stage_bgw_number[12] = { 1, 2, 0 }
stage_bgw_number[13] = { 1, 2, 0 }
stage_bgw_number[14] = { 1, 2, 3 }
stage_bgw_number[15] = { 1, 2, 0 }
stage_bgw_number[16] = { 1, 2, 0 }
stage_bgw_number[17] = { 1, 2, 0 }   ← subject
stage_bgw_number[18] = { 1, 2, 0 }
stage_bgw_number[19] = { 0, 2, 0 }
stage_bgw_number[20] = { 1, 2, 0 }
stage_bgw_number[21] = { 0, 2, 0 }
```

**Both bit 0 and bit 1 of `Screen_Switch` get asserted for stage 17** by:
- `src/sf33rd/Source/Game/stage/bg.c:335-339` — `for (i = 0; i < 3; i++) if (stage_bgw_number[bg_w.stage][i] > 0) Bg_On_R(1 << i);`
- `src/sf33rd/Source/Game/game.c:634-638` (identical pattern inside `Game2_2`).

Note: the prompt claimed `stage_bgw_number[15] == stage_bgw_number[17]`. **It does not** — `stage_bgw_number[15] = {1, 2, 0}`, same as stage 17. But the prompt then claimed `use_real_scr=2` for stage 15 — and that is wrong (see §B.2). The rest of the prompt's stage-17 facts check out.

### B.2 `use_real_scr[22]` (bg_data.c:41)

```
use_real_scr[ 0] = 2
use_real_scr[ 1] = 1
use_real_scr[ 2] = 3
use_real_scr[ 3] = 2
use_real_scr[ 4] = 1    ← matters for stage 17 via bg_index_tbl
use_real_scr[ 5] = 2
use_real_scr[ 6] = 1
use_real_scr[ 7] = 2
use_real_scr[ 8] = 2
use_real_scr[ 9] = 2
use_real_scr[10] = 2
use_real_scr[11] = 1
use_real_scr[12] = 2
use_real_scr[13] = 2
use_real_scr[14] = 3
use_real_scr[15] = 2
use_real_scr[16] = 2
use_real_scr[17] = 2
use_real_scr[18] = 2
use_real_scr[19] = 1
use_real_scr[20] = 2
use_real_scr[21] = 1
```

- `use_real_scr[bg_w.stage]` is used at `bg.c:331` as the upper bound of the `scr_bcm` write loop — **stage-indexed.**
- `use_real_scr[bg_w.bg_index]` is assigned into `bg_w.scrno` at `bg_sub.c:1114`, and `bg_w.scrno` is the upper bound of the texture-load loop at `bg.c:361` — **bg_index-indexed.**

For stage 17: `use_real_scr[17] == 2` (loop at 331 writes `scr_bcm[0..1]`), but `bg_w.scrno = use_real_scr[4] == 1` (loop at 361 loads only layer 0). **This is the fork of the inconsistency.**

### B.3 `use_scr[22]` (bg_data.c:39)

```
use_scr = { 2, 2, 3, 2, 2, 2, 2, 3, 2, 2, 2, 2, 2, 2, 3, 2, 2, 2, 2, 2, 2, 2 }
```

`bg_w.scno = use_scr[bg_w.bg_index]` at `bg_sub.c:1113`. For stage 17 → `use_scr[4] = 2`. Used in `bg_initialize`'s bgw[] setup loop (`bg_sub.c:1153`) and in `Bg_Family_Set` (`bg_sub.c:889`). Not directly implicated in the black-BG visual symptom.

### B.4 `bgtex_stage_gbix[22][3]` (bg_data.c:378-399)

```
bgtex_stage_gbix[15] = { 0xFFFFFFFF, 0x80E6FFFF, 0x00000000 }   ← Chun (Chinese Restaurant)
bgtex_stage_gbix[17] = { 0x7E7E0000, 0x077FFFFF, 0x00000000 }   ← subject
```

- Stage 15: `use_real_scr[bg_index_tbl[15][0]=15] == 2`. Main loop at `bg.c:361` runs j=0 (tgbix `0xFFFFFFFF`, 32 handles) and j=1 (tgbix `0x80E6FFFF`, ~20 handles). Both `ppgBgTex[0]` and `ppgBgTex[1]` get `be=1`. Both draw.
- Stage 17: `use_real_scr[bg_index_tbl[17][0]=4] == 1`. Main loop runs j=0 only (tgbix `0x7E7E0000`, 12 handles, `ppgBgTex[0].be = 1`). **j=1 never executes.** `ppgBgTex[1]` retains `be=0` from the Bg_Close tear-down.

The `0x077FFFFF` second-slot tgbix for stage 17 is **orphan data** — the tables say "layer 1 has 24 populated tile-slots" but the loader refuses to look at them because the underlying PPG file for `bg_index=4` (stage 4, Dudley's Main Street) only has enough `pTEX` chunks for one screen (see §C.3).

### B.5 `bg_index_tbl[22][3]` (bg_data.c:537-541)

```
bg_index_tbl[ 0] = {  0,  0,  0 }    identity
bg_index_tbl[ 1] = {  1,  1,  1 }    identity
bg_index_tbl[ 2] = {  2,  2,  2 }    identity
bg_index_tbl[ 3] = {  3,  3,  3 }    identity
bg_index_tbl[ 4] = {  4,  4,  4 }    identity
bg_index_tbl[ 5] = {  5,  5,  5 }    identity
bg_index_tbl[ 6] = {  6,  6,  6 }    identity
bg_index_tbl[ 7] = {  7,  7,  7 }    identity
bg_index_tbl[ 8] = {  8,  8,  8 }    identity
bg_index_tbl[ 9] = {  9,  9,  9 }    identity
bg_index_tbl[10] = { 10, 10, 10 }    identity
bg_index_tbl[11] = { 11, 11, 11 }    identity
bg_index_tbl[12] = { 12, 12, 12 }    identity
bg_index_tbl[13] = { 13, 13, 13 }    identity
bg_index_tbl[14] = { 14, 14, 14 }    identity
bg_index_tbl[15] = { 15, 15, 15 }    identity
bg_index_tbl[16] = { 16, 16, 16 }    identity
bg_index_tbl[17] = {  4,  4,  4 }  ← ★ the only non-identity row in the table ★
bg_index_tbl[18] = { 18, 18, 18 }    identity
bg_index_tbl[19] = { 19, 19, 19 }    identity
bg_index_tbl[20] = { 20, 20, 20 }    identity
bg_index_tbl[21] = { 21, 21, 21 }    identity
```

**Verified**: stage 17 is the only stage with a non-identity `bg_index_tbl` row. Every other stage's `bg_w.scrno = use_real_scr[bg_w.bg_index]` equals `use_real_scr[bg_w.stage]`, so the loader-vs-enabler mismatch described in §B.2 vanishes.

This is why **only stage 17 goes black** — it is the sole row in the entire table where the bg_index-indexed and stage-indexed `use_real_scr` values diverge.

### B.6 `bg_map_tbl[22][3]` (bg_data.c:566-587)

```
bg_map_tbl[15] = { stage150_map, stage151_map, NULL }            Chun — Chinese Restaurant
bg_map_tbl[17] = { stage160_map, stage161_map, NULL }            Q    — reuses Dojo tilemaps (!)
```

For stage 17 the tilemaps are stolen from stage 16 (Dojo Of Rindo-kan), but the textures/animation come from bg_index=4 (Main Street, England, Dudley's stage). Stage 17 is therefore a hybrid that blends:

- Tilemap IDs from stage 16 (`scr_bcm[0] = stage160_map, scr_bcm[1] = stage161_map`)
- Animation driver `ta_move_tbl[bg_w.bg_index] = ta_move_tbl[4] = BG040`
- Texture file keyed by `bg_index == 4`

This cross-wiring is why tables 17 and 4 give the non-identity `bg_index_tbl` row. `scr_bcm[]` is written but, as verified in §D, never read; the tilemap IDs are effectively dead.

### B.7 `rewrite_scr[22]` (bg_data.c:45)

```
rewrite_scr = { 0, 0, 0, 25, 0, 0, 0, 12, 24, 0, 96, 0, 0, 0, 1, 0, 0, 0, 10, 18, 0, 0 }
```

Stage 17 → `rewrite_scr[17] = 0`. The rewrite-texture setup block at `bg.c:381-392` is skipped. Not involved.

### B.8 Version provenance

`git log --oneline -- src/sf33rd/Source/Game/stage/bg_data.c` → only touches are structural (folder restructure PRs, dead-code removal, comment cleanups). **No data-table value has been altered** from upstream. The tables match `/tmp/3sxtra/src/sf33rd/Source/Game/stage/bg_data.c` byte-for-byte for every row referenced above. The data bug is **inherited from the arcade-original design**, not introduced in the port.

---

## Control-flow trace (Section A)

### A.1 Renderer → tex-handle query for layer 1

1. Per-frame game loop calls one of the `Game2_*` routines (stage mode: `Game2_5` at `src/sf33rd/Source/Game/game.c:~662` for rounds 2+, `Game2_1` for round 1).
2. `BG_Draw_System()` at `src/sf33rd/Source/Game/system/sys_sub.c:902-952`. Loop at 928-932:
   ```c
   for (i = 0; i < 4; i++, s2 = mask *= 2) {
       if (Screen_Switch_Buffer & mask) {
           scr_trans(i);
       }
   }
   ```
   For stage 17 during gameplay, `Screen_Switch_Buffer == 0x3` (observed in prompt log) → `scr_trans(0)` and `scr_trans(1)` both fire.
3. `scr_trans(1)` at `src/sf33rd/Source/Game/stage/bg.c:729-730`:
   ```c
   global_index = (bgnm * 64) + 100;            /* 164 for bgnm=1 */
   ppgSetupCurrentDataList(&ppgBgList[bgnm]);    /* &ppgBgList[1], tex=&ppgBgTex[1] */
   curDataList = &ppgBgList[bgnm];
   ```
4. Default `tokusyu_stage` branch at `bg.c:1146` → `bgDrawOneScreen(1, 164, ...)`.
5. `bgDrawOneScreen` at `bg.c:1176-1199` iterates tiles `y=yy[0]..yy[1]` step 128, `x=xx[0]..xx[1]` step 128, computing `gbix = (row<<3) + col + 164`, then calls `bgDrawOneChip`.
6. `bgDrawOneChip` at `bg.c:1201-1231`:
   ```c
   if ((No_Trans == 0) && ppgCheckTextureNumber(0, gbix)) { ... }
   ```
7. `ppgCheckTextureNumber(0, gbix)` at `src/sf33rd/Source/Common/PPGFile.c:1844-1925`.
   - `tex == NULL` → `tex = ppg_w.cur->tex` → **`&ppgBgTex[1]`** (because we set `ppg_w.cur = &ppgBgList[1]` at step 3).
   - `tex->be == 0` → log line `FAIL:be=0 tex=0x1073ec2b8`, return 0.
8. Return value 0 short-circuits the draw-gate at `bg.c:1221`. **`ppgWriteQuadUseTrans` never runs for this tile.** No pixels written.
9. Every tile in layer 1's viewport bounds fails the same check → layer 1 covers nothing in the output buffer.

### A.2 Pixel fate for the uncovered region

The SDL canvas is bulk-cleared each frame at `src/port/sdl/sdl_game_renderer.c:9487-9494`. `flPs2State.FrameClearColor` defaults to `0x00000000` (RGBA) in the arcade/netplay stage-gameplay scene; `a != SDL_ALPHA_TRANSPARENT` is false → `SDL_SetRenderDrawColor(_renderer, 0, 0, 0, SDL_ALPHA_OPAQUE)` → `SDL_RenderClear`. Every pixel starts black.

Layer 0 draws the tiles whose bits are set in `bgtex_stage_gbix[17][0] = 0x7E7E0000`:
- Bits set (MSB=31): 30, 29, 28, 27, 26, 25, 22, 21, 20, 19, 18, 17 → 12 handles populated.
- Bits unset: 31, 24, 23, 16..0 → 20 handles with `handle[ix].b16[0] == 0`.

So for layer 0, even though `ppgBgTex[0].be == 1`, any tile whose `gbix - ixNum1st` falls on an unset bit fails with `FAIL:handle=0` (observed: `ix=18..29`; `total=32`). These are the "24 FAIL:handle=0" entries in the log. They are **by-design on PS2** — that handle slot is supposed to be covered by layer 1, which in turn would have loaded from `bgtex_stage_gbix[17][1] = 0x077FFFFF`.

### A.3 Who sets `ppg_w.cur` to `&ppgBgList[1]`?

Question from the prompt. Answer: `scr_trans` at `bg.c:729` (and also `bg.c:733`) calls `ppgSetupCurrentDataList(&ppgBgList[bgnm])`. That function at `src/sf33rd/Source/Common/PPGFile.c:300` assigns `ppg_w.cur = dlist`. So every call `scr_trans(1)` binds the layer-1 PPG data-list as current. The renderer then asks `ppgCheckTextureNumber(0, ...)` with `tex==NULL`, which resolves to `ppg_w.cur->tex == &ppgBgTex[1]`.

### A.4 `Screen_Switch_Buffer = 0x3` provenance

Two distinct sources both set bits 0 and 1 for stage 17:
- `Bg_Texture_Load_EX` at `bg.c:335-339` (inside `bg_initialize` at `bg_sub.c:1119`).
- `Game2_2` at `game.c:634-638` (stage-load sync point, runs once per stage).

Both use the same pattern: `for (i=0; i<3; i++) if (stage_bgw_number[bg_w.stage][i] > 0) Bg_On_R(1 << i);`. Neither consults `bg_w.scrno`.

No `Bg_Off_R` anywhere masks bit 1 back off once it's set:
- `bg_sub.c:1104` — `Bg_Off_R(7)` at the top of `bg_initialize`; this runs BEFORE `Bg_Texture_Load_EX` so it's overwritten by the Bg_On_R loop that follows.
- `src/sf33rd/Source/Game/effect/eff77.c:71,92`, `effd3.c:56,97,156` — conditional runtime clears tied to stage-specific effects, none fire for stage 17.
- `src/sf33rd/Source/Game/ending/end_main.c:80` — `Bg_Off_R(7)` on ending scene; not gameplay.

So once set, bit 1 of `Screen_Switch_Buffer` stays set for the entire gameplay phase of a stage-17 match, and the renderer asks for layer-1 tiles every single frame.

### A.5 Why `handle=0` failures on layer 0 are benign

Each tile's `handle[ix]` is populated only when `bgtex_stage_gbix[stage][layer]` has the corresponding bit set (see the `mask >>= 1` loop at `bg.c:373-378`). For stage 17 layer 0, only the bits in `0x7E7E0000` are populated; the rest return 0 from `ppgCheckTextureNumber` → silent skip. This is the normal "we don't draw a tile where the table says nothing" pathway, not a bug.

The log's "24 FAIL:handle=0" count therefore does not imply layer 0 is broken; it is counting exactly the tiles layer 1 was supposed to cover. **When layer 1 isn't loaded, the viewport column/row where layer 1 would have drawn stays clear-color black.**

---

## Rollback interaction (Section C)

### C.1 What `GameState_Save` / `GameState_Load` touch

From `src/netplay/game_state.c:580-603` (Save) and `:1271-1291` (Load):

- `GS_SAVE(bg_w)` → `bg_w.stage, bg_w.area, bg_w.bg_index, bg_w.scno, bg_w.scrno, bg_w.bg_routine, bg_w.bg_r_1, bg_w.bg_r_2, bg_w.compel_flag, bg_w.bgw[7], bg_w.bg_opaque, bg_w.pos_offset, bg_w.scr_stop, bg_w.frame_flag, …` — all restored.
- `GS_SAVE(Screen_Switch)` and `GS_SAVE(Screen_Switch_Buffer)` — restored.
- `GS_SAVE(rw_num, rw_bg_flag, tokusyu_stage, rw_gbix, stage_flash, stage_ftimer, yang_*, ending_flag, end_prm, gouki_end_gbix, rw3col_ptr, bg_disp_off, bgPalCodeOffset, rw_dat)` — restored.
- `GS_SAVE(bg_pos, fm_pos, bg_prm)` at `:728-730` — restored.
- `GS_SAVE(chainex_check)` at `:762-766` — restored (the recent desync fix).

### C.2 What `GameState_Save` / `GameState_Load` do NOT touch

- `ppgBgTex[0..2]`, `ppgBgList[0..2]`, `ppgRwBgTex`, `ppgAkeTex`, `ppgAkanePal`, `ppgAkaneTex` — the actual per-tile texture-handle cache.
- `ppg_w.mm` — the PPG pool allocator heap.
- `ppg_w.cur` — currently bound data-list.
- `bg_priority[4]` — layer z-order, derived from `stage_priority[bg_w.stage]` by `Bg_Texture_Load_EX`.
- `ppgBgList[i].tex` and `ppgBgList[i].pal` pointers — only wired in `Bg_TexInit` on stage change.
- `scr_bcm[4]` — tilemap-ID pointers (write-only, see §D).
- `rw_dat[20]` — rewrite data structs, full array is saved though (see above). Confirmed.

### C.3 Rollback does not create the mechanism

`bg_w.stage == 17` and `bg_w.scrno == 1` are both saved and restored faithfully. `Screen_Switch == 0x3` is saved and restored faithfully. The black-BG mechanism described in §A is a pure function of `(bg_w.stage, bg_w.scrno, Screen_Switch_Buffer, ppgBgTex[1].be)`. The first three are rollback-covered; the fourth is NOT covered but is **re-initialised to 0 at every `Bg_Texture_Load_EX` entry** by the Fix-B tear-down at `bg.c:307-317`.

Therefore rollback cannot flip the outcome of the "is layer 1 rendered?" question. **The question was always "no"** from the moment `bg_w.stage=17` was set, regardless of network conditions.

### C.4 Why the bug appears to correlate with latency

Several candidate explanations consistent with the prompt log:

a. **The rollback-triggered stage reload is the first time `bg_w.stage` becomes 17 in that session.** Prior state, before the stage transition, had a different stage. Without latency / without rollback, gameplay might never have entered stage 17 because `Setup_Battle_Country` (see §E) rarely resolves to 17 in pure-local play. Under netplay latency, rollback can replay past the stage-select transition on multiple frames, re-exposing the underlying stage-17 outcome.

b. **Visual survivor bias.** In non-rollback runs stage 17 would still render black (per the mechanism in §A), but the user hasn't played that specific matchup locally, so they haven't seen it. The bug is **latent in single-player**, not absent.

c. **Rollback frequency amplifies the `Bg_Texture_Load_EX` rerun count.** Each rollback that crosses the stage-boundary frame re-runs `Bg_Texture_Load_EX` — Fix B tears down and re-populates the handle array every time. That's only a rendering cost, not a visibility change.

The investigation **cannot confirm (a) vs (b) without a runtime experiment**. The rest of this document proceeds on the safer assumption that rollback is not *causal* but *reveals* a dormant data bug.

### C.5 Specifically: why the "scrno-based fix" (attempt #3) still rendered black

Attempt #3 held `Screen_Switch` at 0x1 (layer 0 only). Layer 1 no longer draws. But **layer 0 still has only 12 of 32 handles populated** (0x7E7E0000). The remaining 20 handle slots correspond to tiles outside layer 0's coverage intent — those tiles are supposed to be filled by layer 1 tiles in the PS2 original. With layer 1 disabled, those pixels stay clear-color black. The BG therefore appears mostly black with 12 tiles of Dudley-stage-esque graphics — **still "black" from the user's perspective.**

The only way for stage 17 to render correctly is to **actually populate layer 1's texture handles** — i.e. arrange for the loader to run `j=1` AND have the underlying PPG file contain enough `pTEX` data for a second screen. Upstream clearly intends for the user to never reach this stage, so neither condition holds.

---

## The shift-left regression explained (Section D)

### D.1 `scr_bcm[]` is write-only

Full grep of `scr_bcm` across `src/`:

```
src/sf33rd/Source/Game/ending/end_14.c:145:          *scr_bcm = ending_map_tbl[20][0];
src/sf33rd/Source/Game/ending/end_14.c:520:          *scr_bcm = ending_map_tbl[20][1];
src/sf33rd/Source/Game/stage/bg_data.c:30:           const u16* scr_bcm[4];
src/sf33rd/Source/Game/stage/bg_data.h:93:           extern const u16* scr_bcm[4];
src/sf33rd/Source/Game/stage/bg.c:332:               scr_bcm[stg + i] = bg_map_tbl[bg_w.stage][i];
src/sf33rd/Source/Game/stage/bg.c:451:               scr_bcm[i] = bg_map_tbl2[type];
src/sf33rd/Source/Game/stage/bg.c:512:               scr_bcm[i] = ending_map_tbl[type][i];
```

Every reference is a *write*. No read site exists. Fast confirmation:

```bash
$ grep -rn "scr_bcm" src/ | wc -l      # 7 writes + 2 decls
```

Conclusion: **modifying the iteration count or the stored pointer cannot change any game behaviour**. The "shifted-left" regression was NOT caused by the `scr_bcm` loop change alone.

### D.2 What likely caused the shift-left regression

Attempt #3 and #4 are described in the prompt. The *implementation* of those attempts almost certainly touched more than just `scr_bcm` or `Bg_On_R` — they changed the `stg` index computation in `Bg_Texture_Load_EX` or the `bg_w.scno` / `bg_w.scrno` values used by `Bg_Family_Set`. Two plausible mechanisms for the visual "characters shifted left":

- **`Bg_Family_Set` at `bg_sub.c:884-896` iterates `bg_w.scno` (not `scrno`) times** and calls `Scrn_Move_Set(i, x, y)` which writes `bg_pos[i].scr_x.word_pos.h = x`. `Irl_Scrn` at `bg.c:1540-1549` then derives `bg_prm[i].bg_h_shift = scrn_adgjust_x + bg_pos[i].scr_x_buff.word_pos.h`, which `scr_trans` uses for the layer translation. If attempt #3/#4 changed `bg_w.scno` (e.g. redefined it via `bg_sub.c:1113`) for stage 17, layer shifts would be off. Characters themselves use world-space coordinates, but the **camera origin** depends on `bg_pos[0]` indirectly (via `scrn_adgjust_x` in `Irl_Scrn`), and sprites are blitted relative to `scrn_adgjust_x`.
- **If attempt #3 forced the Bg_On_R loop to stop at `bg_w.scrno` AND also advanced `stg++` differently**, the per-layer `ppgBgList[stg]` binding at `bg.c:364` could have shifted: layer-0 textures would have been staged into `ppgBgList[1]` slot. Subsequent `scr_trans(0)` (bgnm=0) would bind `ppgBgList[0]` (which now has no texture), and `scr_trans(1)` would bind the real textures but use layer-1's `bg_prm[1]`. That would produce a picture rendered with the scroll offset of layer 1, which for parallax stages is different from layer 0 — **visible as a spatial shift** of the entire background.

Without the exact reverted patch in the git log (the attempts were reverted via `git reset`, not via "Revert" commits), the precise cause of the shift-left can't be pinned down from the record. **What's verifiable**: `scr_bcm` content is irrelevant.

---

## Why "Chun works and subject doesn't" (Section E)

The prompt asks why stage 15 works while stage 17 doesn't given "same `stage_bgw_number`". Re-verification of the tables shows:

- `stage_bgw_number[15] = {1, 2, 0}`, `stage_bgw_number[17] = {1, 2, 0}` — **same**. ✓ (prompt correct)
- `use_real_scr[15] = 2`, `use_real_scr[17] = 2` — **same**. (The prompt misread the log for stage 15 as `use_real_scr=3`.)
- `bg_index_tbl[15] = {15,15,15}` (identity), `bg_index_tbl[17] = {4,4,4}` (non-identity). **Different.**

So `bg_w.scrno` for stage 15 = `use_real_scr[15] = 2` (matches stage_bgw_number bit count), but for stage 17 = `use_real_scr[4] = 1` (mismatch).

When `bg_index_tbl` is identity, `use_real_scr[bg_w.stage] == use_real_scr[bg_w.bg_index]` by definition, and the two loops in `Bg_Texture_Load_EX` agree. Stage 17 is the sole row breaking that invariant (§B.5), which is why it is the sole stage with this symptom.

### E.1 Can `bg_w.stage` become 17 in netplay by the normal stage-pick logic?

Stage selection in netplay (`Mode_Type == MODE_NETWORK`) goes through `Setup_Battle_Country` at `src/sf33rd/Source/Game/screen/sel_pl.c:2013-2035`. The non-VERSUS branch reads:

```c
if (My_char[0] == 17 && My_char[1] == 17) { /* random from Random_Stage_Data[0], which skips 17 */ }
if (My_char[New_Challenger] == 17)          { return My_char[Champion]; }
return My_char[New_Challenger];
```

`My_char[i]` uses the PS2/3SX enum (`src/constants.h:38-58` non-CPS3 block). In that enum `CHAR_Q = 17`. So `bg_w.stage = 17` via this path requires:

- Both players picking Q → first branch → stage picked from `Random_Stage_Data[0][]` at `src/sf33rd/Source/Game/screen/sel_data.c:153-155` which contains: `{ 14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 2, 3, 4, 5, 6, 7, 8, 11, 12, 15, 16, 19, 0 }` — **no 17**.
- One Q + one non-Q → second or third branch returns the non-Q's `My_char`, never 17.

So `Setup_Battle_Country` should not return 17 in any netplay matchup. **Reaching `bg_w.stage = 17` implies some other code path sets it.**

Candidates:
- `Debug_w[31]` override at `sel_pl.c:1586` and `next_cpu.c:1132`.
- `next_cpu.c:1118-1120` on CPU-opponent Q: `if (EM_id == 17) { Battle_Country = Q_Country; bg_w.stage = Q_Country; }`. In netplay this code path shouldn't fire (no CPU progression), but should be verified.
- `demo02.c:286` — demo mode default.
- `next_cpu.c:1491` — bonus stage; Bonus_Type is 20/21, not 17.

**Unverified**: precise mechanism by which netplay in this port reaches `bg_w.stage = 17`. The mechanism above is consistent with "Q vs Q rolls 17 via a corrupted `Random_Stage_Data` index" but `random_32()` at `pls02.c:626-638` is bounded `& 0x7F` → 0..127, used as `[Rnd32]` on a length-32 array → reads beyond the array bounds and returns an out-of-bounds value. **That out-of-bounds read is itself potentially the root cause of how stage 17 appears in netplay.**

Let me double-check that bound:

```c
s32 random_32() {
    Random_ix32++;
    if (Debug_w[0x3B] == -32) { Random_ix32 = 0; }
    Random_ix32 &= 0x7F;              // Random_ix32 ∈ [0, 127]
    return random_tbl_32[Random_ix32]; // OK, table is length 128
}
```

`random_tbl_32` is length 128 (see `pls02.c:45-50`), so `random_tbl_32[Random_ix32]` is in-bounds. **The returned VALUE** is `random_tbl_32[k]` — inspecting that table: values are in [0..31]. Then `Random_Stage_Data[0][Rnd32]` uses that as index on a length-32 array → in-bounds. `bg_w.stage` ends up in the set {0..19}\{17}. **Stage 17 cannot come from `Setup_Battle_Country`.**

So **how stage 17 is reached in netplay is unverified by code reading alone**. Possible sources:
1. A Debug override left set.
2. An arcade-mode-for-netplay code path that calls `Next_Q_1st` (which does set EM_id=17, leading to `bg_w.stage = Q_Country`).
3. A missing guard on `setup_vs_mode` that leaves a stale `Debug_w[31]` or `bg_w.stage` from prior single-player play.
4. A deep path in `sel_pl`/`game` that I have not traced.

The user's log is ground truth: `bg_w.stage == 17` when the bug reproduces. **Whatever feeds that value, the subsequent pipeline is deterministic** and hits the mechanism in §A.

---

## Root cause (Section F)

### F.1 Root cause

**The Bg_On_R-from-`stage_bgw_number` pattern in `Bg_Texture_Load_EX` (`bg.c:335-339`) and `Game2_2` (`game.c:634-638`) is inconsistent with the `bg_w.scrno`-bound texture-load loop at `bg.c:361-379` for any stage whose `bg_index_tbl` row is non-identity.**

In one short sentence: the enabler asks for more layers than the loader provides.

Only stage 17 has a non-identity `bg_index_tbl` row, so only stage 17 triggers the mismatch — but the bug is a property of the code, not the data.

### F.2 Why it only reproduces in netplay

The mechanism is **always present**. It reproduces in netplay because netplay is the first game mode in which the user actually drives `bg_w.stage` to 17. In single-player arcade / versus, the stage filters (UI `Handicap_Stage_Move_Sub` at `sel_pl.c:1947-1961`, arcade `Setup_Battle_Country` exclusion, and `Random_Stage_Data` which skips 17) make stage 17 unreachable through normal play. The netplay session — whether because of a code path that fires `Next_Q_1st`-style logic, or a residual Debug override, or a different setup path unique to network mode — exposes the latent bug. **§E notes this exact question is not fully pinned down by code reading and flags it as unverified.**

Rollback is incidental. It does not cause the bug but may make it easier to repro by forcing repeated `Bg_Texture_Load_EX` entries.

### F.3 Why stage 17 specifically

`bg_index_tbl[17] = {4,4,4}` is the only non-identity row in the 22-stage table. It is an explicit reuse hack: stage 17 (Q's arena) piggybacks on stage 4's (Dudley's Main Street) texture file but paints stage 16's (Dojo) tilemap onto it. This kind of cross-wiring breaks the implicit invariant *"count of populated screens in the texture file = count of enabled layer bits in stage_bgw_number"* that holds for every other stage.

---

## Minimal correct fix (Section G)

**Do not change `bg_w.scrno`, `bg_w.bg_index`, or any data table.** The data is upstream-faithful and changing it risks touching ROM-derived structures that have other consumers (e.g. `msp[bg_w.bg_index][i]`, `limit_tbl3[bg_w.bg_index][i]` at `bg_sub.c:1154-1162`).

**Change the enabler loops to track the loader**. The loader loads exactly `bg_w.scrno` layers (`bg.c:361`). The enabler should enable exactly `bg_w.scrno` layers.

### G.1 Edit 1: `Bg_Texture_Load_EX`

`src/sf33rd/Source/Game/stage/bg.c:335-339`, current:

```c
for (i = 0; i < 3; i++) {
    if (stage_bgw_number[bg_w.stage][i] > 0) {
        Bg_On_R(1 << i);
    }
}
```

Replace with:

```c
/* Enable exactly the layers whose texture data we're about to load.
 * For every stage with identity bg_index_tbl, use_real_scr[bg_w.stage] ==
 * use_real_scr[bg_w.bg_index] == bg_w.scrno and this reduces to the
 * stage_bgw_number-based loop. Stage 17 (the sole non-identity row in
 * bg_index_tbl) has bg_w.scrno==1 but stage_bgw_number lists 2 layers;
 * the old loop would have asked the renderer to draw a layer whose
 * ppgBgTex[1] never receives a be=1 handle-array, which paints black
 * into the clear-color where layer 1 would have covered the viewport.
 *
 * We still honour the bitmap PATTERN from stage_bgw_number so that
 * stage 11 (whose stage_bgw_number is {0,1,0} — bit 1 only) continues
 * to use bit 1, not bit 0. We just cap the total count at bg_w.scrno. */
{
    int enabled = 0;
    for (i = 0; i < 3 && enabled < bg_w.scrno; i++) {
        if (stage_bgw_number[bg_w.stage][i] > 0) {
            Bg_On_R(1 << i);
            enabled++;
        }
    }
}
```

### G.2 Edit 2: `Game2_2`

`src/sf33rd/Source/Game/game.c:634-638`, current:

```c
for (i = 0; i < 3; i++) {
    if (stage_bgw_number[bg_w.stage][i] > 0) {
        Bg_On_R(1 << i);
    }
}
```

Replace with the same pattern (cap at `bg_w.scrno`). This reasserts `Screen_Switch` during Game2_2's stage-load sync without re-enabling layers that don't have texture data.

### G.3 Rationale for this exact shape

- Cap on `bg_w.scrno` — that's the same count the texture-load loop (`bg.c:361`) uses, so the enabler and loader now share a single source of truth.
- Honour the `stage_bgw_number` pattern — stages with leading zeros (e.g. stage 1 `{0,2,0}`, stage 4 `{0,2,0}`, stage 11 `{0,1,0}`) require bit 1 (not bit 0) to be the first enabled layer. Using `i` to pick which bit to enable preserves that mapping.
- Keep the `bg_w.stage == 7` branch for `Bg_On_R(4)` at `bg.c:341-343` and `game.c:640-642` — that's a special effect (Akane/Akebono) for Necro's stage, orthogonal to the scrno accounting.

### G.4 What this fix does to stage 17

Before: `Screen_Switch = 0x3`. Renderer draws layer 0 (partial, 12 tiles) and layer 1 (nothing → black).
After: `Screen_Switch = 0x1`. Renderer draws layer 0 only. **Layer 1 region stays as clear-color**, same as attempt #3 — still visually dark. **This fix alone is not sufficient to make stage 17 "look correct"**, because the underlying data inconsistency (stage 17 expects 2 layers of draw data but the texture file has 1) is not fixable without either (a) restructuring the texture file or (b) filtering `bg_w.stage == 17` out of the reachable set.

It DOES stop the black-BG from being blamed on "missing ppgBgTex[1]" and brings the engine into a self-consistent state. The user's scenario probably still shows a dark-ish stage 17 BG, but the rest of the pipeline (sprites, HUD, camera) will be correct.

### G.5 True fix: prevent stage 17 from being reached

The proper long-term fix is to ensure `bg_w.stage` never lands on 17 in any playable mode. Options:

1. Add a stage-17 → stage-16 remap at the entry of `Bg_Texture_Load_EX` ("if stage == 17, treat as stage 16"). Would break Q-specific arcade content.
2. Filter stage 17 out of whatever code path is letting it through in netplay (needs the §E unverified-question answered first).
3. Force `stage_bgw_number[17] = {1, 0, 0}` in the data table to match `use_real_scr[4] = 1`. This is a data-edit; would deviate from upstream.

None of these is the minimum-diff fix. The minimum-diff fix is §G.1/G.2 — it eliminates the specific invariant violation that produces the black render without changing any data or reachability decision.

---

## Regression risk (Section H)

### H.1 Stages with identity `bg_index_tbl` (21/22 stages)

For these stages `bg_w.scrno == use_real_scr[bg_w.stage]`. Counting the non-zero entries of `stage_bgw_number[stage]`:

| stage | stage_bgw_number | non-zero count | use_real_scr (= scrno) | identical? |
|------:|------------------|----------------|------------------------|-----------|
|  0 | {1,2,0} | 2 | 2 | yes |
|  1 | {0,2,0} | 1 | 1 | yes |
|  2 | {1,2,3} | 3 | 3 | yes |
|  3 | {1,2,0} | 2 | 2 | yes |
|  4 | {0,2,0} | 1 | 1 | yes |
|  5 | {1,2,0} | 2 | 2 | yes |
|  6 | {0,2,0} | 1 | 1 | yes |
|  7 | {1,2,0} | 2 | 2 | yes (+ Bg_On_R(4) special-case) |
|  8 | {1,2,0} | 2 | 2 | yes |
|  9 | {1,2,0} | 2 | 2 | yes |
| 10 | {1,2,0} | 2 | 2 | yes |
| 11 | {0,1,0} | 1 | 1 | yes |
| 12 | {1,2,0} | 2 | 2 | yes |
| 13 | {1,2,0} | 2 | 2 | yes |
| 14 | {1,2,3} | 3 | 3 | yes |
| 15 | {1,2,0} | 2 | 2 | yes |
| 16 | {1,2,0} | 2 | 2 | yes |
| 17 | {1,2,0} | 2 | 1 | **NO** |
| 18 | {1,2,0} | 2 | 2 | yes |
| 19 | {0,2,0} | 1 | 1 | yes |
| 20 | {1,2,0} | 2 | 2 | yes |
| 21 | {0,2,0} | 1 | 1 | yes |

Behaviour identical for every identity-row stage: the old loop enabled N layers, the new loop enables exactly the same N layers (capped at `bg_w.scrno == N`). **No regression on any other stage.**

### H.2 Stage 17

Before: layer 0 draws 12 tiles (pixels covered), remainder black. Layer 1 draws nothing.
After: exactly the same pixel outcome. We just stop setting bit 1 in Screen_Switch — internal bookkeeping improvement, same visible output.

### H.3 What else consumes Screen_Switch / Screen_Switch_Buffer

- `src/sf33rd/Source/Game/stage/bg.c` reads them indirectly via `BG_Draw_System` (already in the diff-covered path).
- `src/sf33rd/Source/Game/opening/opening.c:299, 303, 307` — reads bits 0/1/2 during opening scenes. Not affected (opening runs with ending_flag=1 or via `Bg_Texture_Load_Ending`, separate code path).
- `src/sf33rd/Source/Game/ending/end_main.c:248-249` — zeros Screen_Switch on ending. Not affected.
- `src/sf33rd/Source/Game/system/sys_sub.c:929, 935` (`BG_Draw_System`) — the consumer this fix is aimed at. Affected as intended (stops calling scr_trans(1)).

No other consumer inspects Screen_Switch for stage 17 specifically.

### H.4 `rw_bg_flag[]` on stage 17

`bgrw_on[17] = {-1, -1, ...}` per `bg_data.c:413-422`, meaning `rw_num == 0` for stage 17 (no rewrite data). `rw_bg_flag[]` stays zero. `bgDrawOneScreen` at `bg.c:1183` short-circuits. No rewrite path affected.

### H.5 Does `Bg_Kakikae_Set` need updating?

No. `Bg_Kakikae_Set` at `bg.c:83-175` switches on `bg_w.stage` directly. Stage 17 falls into the `default:` case with `rw_num = 0`. No layer-count logic.

### H.6 Interaction with the `chainex_check` desync fix

None — different state, different mechanism. `chainex_check` is simulation data; the BG bug is rendering data. They co-exist.

---

## Unverified questions (Section I)

Stated up-front so they can be checked in a runtime experiment rather than guessed at.

1. **How does `bg_w.stage` become 17 in netplay?** §E shows `Setup_Battle_Country` cannot return 17. Candidate paths (Debug override, `Next_Q_1st`, demo leakage) are not wired off in the netplay code path. Requires a stderr breakpoint at every write of `bg_w.stage` during a repro session.
2. **Does single-player (local-only) actually avoid stage 17?** Prompt says "unverified". A 5-minute local test with Debug_w cleared and Q-as-P2 should confirm.
3. **Does the "shift-left" regression in attempts #3/#4 originate in the `stg` index reshuffle or somewhere else?** §D suggests the `Bg_Family_Set` / `bg_pos` path, but the reverted patch is not in git history to re-diff.
4. **Is `Screen_Switch_Buffer = 0x3` actually stable for all gameplay frames of stage 17, or does it get toggled by eff77/effd3 mid-match?** Current code grep says no call path clears bit 1 for stage 17, but runtime verification would be cheap.
5. **Does the proposed minimal fix (§G) actually remove the `ppgCheckTextureNumber FAIL:be=0` log entries for `tex=0x1073ec2b8`?** It must — `scr_trans(1)` never runs — but worth logging.
6. **Does the fix leave stage 17 looking tolerably (dark but playable) or unplayably dark?** §G.4 says the dark region remains. The true user-facing fix is §G.5 — but that's a separate decision.

---

## Appendix — call graphs

### Appendix 1 — `Bg_Texture_Load_EX` prologue to first pixel

```
bg_initialize                       src/sf33rd/Source/Game/stage/bg_sub.c:1100
├─ Bg_Off_R(7)                      bg_sub.c:1104
├─ Family_Init                      bg.c:1473
├─ Scrn_Pos_Init                    bg.c:1453
├─ bg_w.bg_index = bg_index_tbl[bg_w.stage][bg_w.area]   bg_sub.c:1112
├─ bg_w.scno   = use_scr[bg_w.bg_index]                  bg_sub.c:1113
├─ bg_w.scrno  = use_real_scr[bg_w.bg_index]             bg_sub.c:1114   ← stage 17: scrno=1
├─ (G_No[0..2] != 2 test)                                bg_sub.c:1118
└─ Bg_Texture_Load_EX                bg.c:246
   ├─ Fix-B tear-down of ppgBgTex[0..2], ppgRwBgTex, ppgAkeTex, ppgAkanePal, ppgAkaneTex   bg.c:307-317
   ├─ bgPalCodeOffset[0..7] = 0x12C                      bg.c:319-321
   ├─ ending_flag = 0                                    bg.c:323
   ├─ find first non-zero stg in stage_bgw_number[stage] bg.c:325-329
   ├─ for i<use_real_scr[bg_w.stage]: scr_bcm[stg+i]=bg_map_tbl[bg_w.stage][i]   bg.c:331-333
   ├─ for i<3: if stage_bgw_number[stage][i]>0 Bg_On_R(1<<i)                     bg.c:335-339   ← stage 17: Bg_On_R(1), Bg_On_R(2) → Screen_Switch=0x3
   ├─ (stage==7): Bg_On_R(4)                             bg.c:341-343
   ├─ load PPG source blob                               bg.c:345-347
   ├─ bg_priority[0..2] extract from stage_priority[stage]   bg.c:351-356
   └─ for j<bg_w.scrno: ppgSetupCurrentDataList(&ppgBgList[stg]); setup handles for tgbix bits   bg.c:361-379
       └─ stage 17: j=0 only. ppgBgTex[0].be=1, handles populated for 0x7E7E0000 bitmask.
       └─ ppgBgTex[1] UNTOUCHED — be stays 0.

Bg_Kakikae_Set (bg.c:83) runs after. Default case sets rw_num=0 for stage 17.

Game2_2                              src/sf33rd/Source/Game/game.c:586
├─ BG_Draw_System                    sys_sub.c:922
├─ ... setup/reset work ...
└─ for i<3: if stage_bgw_number[stage][i]>0 Bg_On_R(1<<i)   game.c:634-638   ← re-asserts Screen_Switch=0x3

Per-frame Game2_5 → BG_Draw_System → (Screen_Switch_Buffer bit i set) → scr_trans(i):
  scr_trans(0) → ppgSetupCurrentDataList(&ppgBgList[0]) → bgDrawOneScreen(0, 100, ...) → bgDrawOneChip → ppgCheckTextureNumber(0, gbix)
                   → ppgBgTex[0].be==1 → ix check → 12 OK / 20 handle=0 (intentional)
  scr_trans(1) → ppgSetupCurrentDataList(&ppgBgList[1]) → bgDrawOneScreen(1, 164, ...) → bgDrawOneChip → ppgCheckTextureNumber(0, gbix)
                   → ppgBgTex[1].be==0 → FAIL:be=0 every tile → no draw
```

### Appendix 2 — `BG_Draw_System` call sites

From `src/sf33rd/Source/Game/game.c`: 17 call sites (one per gameplay phase). All follow the pattern "draw BG, then sprites, then HUD". Relevant to stage 17 in gameplay:
- `Game2_1` at `game.c:562` (round 1).
- `Game2_2` at `game.c:589` (setup/stage-load).
- `Game2_3` → `Game2_1`.
- `Game2_5` at `game.c:662` (rounds 2+).

All consume `Screen_Switch_Buffer` identically.

### Appendix 3 — Files touched by the proposed fix

- `src/sf33rd/Source/Game/stage/bg.c` (edit 1, §G.1).
- `src/sf33rd/Source/Game/game.c` (edit 2, §G.2).

No header changes. No data-table changes. No GameState schema changes. No netplay changes. Roll-forward only.

### Appendix 4 — Why attempt #2 (`scrno = use_real_scr[bg_w.stage]`) hangs

Attempt #2 modified `bg_sub.c:1114` from `bg_w.scrno = use_real_scr[bg_w.bg_index]` to `bg_w.scrno = use_real_scr[bg_w.stage]`. For stage 17 this forces `bg_w.scrno = 2`, making the main texture-load loop run `j=0` and `j=1`.

`j=1` iteration:
```c
tgbix = bgtex_stage_gbix[bg_w.stage][j] = bgtex_stage_gbix[17][1] = 0x077FFFFF;
ppgSetupCurrentDataList(&ppgBgList[stg]);                                 /* ppgBgList[1] */
ppgSetupTexChunk_1st(NULL, loadAdrs, loadSize, (stg*64)+0x84, 32, 0, 0);   /* allocates handle[32] for ppgBgTex[1] */
ppgSetupTexChunk_1st_Accnum(0, accnum);
```

`ppgSetupTexChunk_1st` at `src/sf33rd/Source/Common/PPGFile.c:1239-1336`:
- Scans the source blob at `loadAdrs` counting `pTEX` magic-tagged chunks (`:1277-1289`).
- Records the count in `tch->textures`.
- For stage 17, `loadAdrs` is the ramcnt-0x12 blob. That blob is the **same blob as for stage 4** (they share `bg_index == 4`). That blob has enough `pTEX` chunks for `use_real_scr[4] == 1` screen worth of textures. For screen 0 of stage 4, `bgtex_stage_gbix[4][0] == 0xFFFFFFFF` has **all 32 bits set** → the blob contains exactly 32 `pTEX` chunks (the count needed to supply 32 handle slots).
- So `tch->textures == 32` after `_1st` completes.

Then the inner loop runs 32 iterations, one per bit in `0x077FFFFF`. `0x077FFFFF` has bits 0-20 and 24-26 set → actually `0x07_7F_FF_FF` = `00000111 01111111 11111111 11111111` → bits 0-20 and 24-26 = 24 bits set.

The `j=0` iteration also runs 32 setups (`0x7E7E0000` has 12 bits set; so 12 calls to `_2nd`). After `j=0`, `accnum == 12`.

Then `j=1` iteration:
- `_1st` resets `accnum` via `tch->accnum = 0` at `:1254`, but then `ppgSetupTexChunk_1st_Accnum(0, accnum)` at `bg.c:366` sets `tch->accnum = 12` (the accumulated value from j=0).
- Inner loop: for each set bit, call `_2nd` which does `if (tch->textures <= tch->accnum) while(1) {}` at `PPGFile.c:1355-1358`.
- After first `_2nd`: `tch->accnum = 13`. Continues to 14, 15... up to `tch->accnum == tch->textures == 32`.
- 33rd `_2nd` call → `tch->textures (32) <= tch->accnum (32)` → **while(1) hang**.

Exact trigger count: 12 from j=0 + N from j=1. At N=20 (the 21st bit of 0x077FFFFF iterated), `accnum == 32`, next iter hangs. So yes — attempt #2 hangs because **the source blob keyed by `bg_index == 4` only contains 32 `pTEX` chunks, which is enough for exactly one screen of `0xFFFFFFFF` (the bitmask for stage 4)**, not for stage 17's implied two screens.

This is the definitive proof that **`bg_w.scrno` MUST stay `use_real_scr[bg_w.bg_index]`** — changing it destroys the PPG-file accounting.

### Appendix 5 — Double verification: exact bit counts of `bgtex_stage_gbix` vs `textures`

For reference (all values from `bg.c:373-378`'s `for (i=0; i<32; i++, mask>>=1)` loop semantics):

| stage | bg_index | `use_real_scr[bg_index]` = scrno | tgbix[0] bits set | tgbix[1] bits set | tgbix[2] bits set | total handles requested |
|------:|---------:|--------:|-------:|-------:|-------:|---:|
|  0 |  0 | 2 | 16 | 23 | —  | 39 |
|  4 |  4 | 1 | 32 | — | — | 32 |
| 15 | 15 | 2 | 32 | 19 | — | 51 |
| 17 |  4 | 1 | 12 | — | — | 12 (loader) / would be 36 if j=1 ran |

For stage 17 the loader-side count is 12 (only j=0 runs). But it uses the bg_index=4 blob which has 32 pTEX chunks. The `accnum` ends at 12 after stage 17 loads, nowhere near the `textures == 32` limit — **so stage 17 loads cleanly** under the CURRENT code. Attempt #2 breaks this; §G's proposed fix does not touch the loader count.

### Appendix 6 — `Setup_Battle_Country` and the stage-17 reachability question

Full code at `src/sf33rd/Source/Game/screen/sel_pl.c:2013-2035`:

```c
u8 Setup_Battle_Country() {
    s16 Rnd32;

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

    if (My_char[New_Challenger] == 17) {
        return My_char[Champion];
    }

    return My_char[New_Challenger];
}
```

- `Mode_Type == MODE_NETWORK` set by `netplay.c:205` — NOT MODE_VERSUS → falls through to arcade-like branch.
- `Champion = 0, New_Challenger = 1` set by `netplay.c:305` — both fixed for a netplay session.
- `My_char[0..1]` in PS2/3SX enum (non-CPS3) where `CHAR_Q = 17`.
- If P2 (= New_Challenger) picks Q: `My_char[1] == 17` → return `My_char[0]` (P1's char) → NEVER 17.
- If P1 (= Champion) picks Q: `My_char[0] == 17, My_char[1] != 17` → fall through to `return My_char[1]` → NEVER 17.
- Both Q: `return Random_Stage_Data[0][random_32() & 31]`. Table is `{14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 2, 3, 4, 5, 6, 7, 8, 11, 12, 15, 16, 19, 0}` — no 17.

So in a **clean** netplay session without debug overrides, `Setup_Battle_Country` cannot return 17.

Yet the prompt log shows `bg_w.stage == 17` is reached. Search for other writes:

```
src/sf33rd/Source/Game/screen/sel_pl.c:1582:   bg_w.stage = Battle_Country;         (VS-mode exit 2nd)
src/sf33rd/Source/Game/screen/sel_pl.c:1602:   Battle_Country = bg_w.stage = Debug_w[31] - 1;
src/sf33rd/Source/Game/screen/sel_pl.c:1710:   bg_w.stage = Battle_Country;         (Exit_6th alt path)
src/sf33rd/Source/Game/screen/next_cpu.c:1120: bg_w.stage = Q_Country;              (EM_id==17 branch)
src/sf33rd/Source/Game/screen/next_cpu.c:1128: bg_w.stage = Battle_Country;         (regular next-fighter)
src/sf33rd/Source/Game/screen/next_cpu.c:1132: Battle_Country = bg_w.stage = Debug_w[31] - 1;
src/sf33rd/Source/Game/screen/next_cpu.c:1140: Super_Arts[COM_id] = bg_w.stage = Debug_w[32] - 1;
src/sf33rd/Source/Game/screen/next_cpu.c:1491: bg_w.stage = Bonus_Type;             (bonus stage; 20/21)
src/sf33rd/Source/Game/demo/demo02.c:286:      bg_w.stage = Demo_Stage_Play_Data[Demo_Stage_Index][rnd];
src/test/test_runner.c:1044:                  bg_w.stage = stage;
```

For netplay, the **arcade CPU-progression path** (`next_cpu.c:1113-1145`) is not exercised — that fires after a CPU-opponent win. Netplay has no CPU opponent; it uses the sel_pl.c path. `sel_pl.c:1582` uses the direct `Battle_Country` as just computed above — gives non-17. `sel_pl.c:1710` (`Exit_6th`) also uses Battle_Country directly. So **by static code inspection alone, netplay cannot reach stage 17 without a Debug override.**

**Open question**: the prompt log clearly shows `bg_w.stage == 17` in netplay. Possible explanations (none verified):

1. `Debug_w[31]` is non-zero from a prior single-player debug session and is **not cleared by `setup_vs_mode`**. Check:
   - `src/netplay/netplay.c` does NOT reset `Debug_w[]`. Debug_w is a persistent array across mode changes.
   - If a dev/tester set `Debug_w[31] = 18` (to force stage 17 via the `Debug_w[31] - 1` formula), that override survives into netplay and pins `bg_w.stage = 17`.
   - Grep for Debug_w[31] writes in runtime code: none. It's set via a debug UI only.
2. A netplay transition edge case where `bg_w` is loaded from an earlier save that had `stage=17` (e.g. replay of a prior session). `GS_LOAD(bg_w)` at `game_state.c:1273` would then clobber whatever `Setup_Battle_Country` computed. Would require stage=17 to appear in the **first** GameState hash exchange.
3. A byte-swap / wrap-around in `Battle_Country`'s u8 vs `bg_w.stage`'s type. Grep: `struct BG.stage` is `u8` (see `src/sf33rd/Source/Game/stage/bg.h`). `Battle_Country` is `u8` in game_state.h:383 — same.
4. `next_cpu.c:1120` Q_Country branch via some netplay-specific path. Q_Country is set at `manage.c:1877`. Would need trace to see if it fires.

This document cannot resolve how stage 17 actually leaks in without runtime instrumentation. **But it does not have to.** The fix in §G works regardless of how stage 17 is reached — the internal inconsistency is the bug, and the fix resolves it.

---

### Appendix 7 — Every write to `ppgBgTex[1].be`

Full grep of write sites (read-only sites excluded):

1. `src/sf33rd/Source/Common/PPGWork.c:51` — `ppgBgTex[i].be = 0;` in `ppgWorkInitializeApprication`. One-shot application startup.
2. `src/sf33rd/Source/Common/PPGFile.c:1252` — `tch->be = 0;` inside `ppgSetupTexChunk_1st`, about to build fresh handle array. Called from `Bg_Texture_Load_EX` with `tch == ppg_w.cur->tex == &ppgBgTex[stg]`. **For stage 17 this call is NEVER made for ppgBgTex[1]** because the loader stops at j=0.
3. `src/sf33rd/Source/Common/PPGFile.c:1321` — `tch->be = 1;` at the end of a successful `ppgSetupTexChunk_1st`. Same "never called for ppgBgTex[1] on stage 17" logic.
4. `src/sf33rd/Source/Common/PPGFile.c:1761` — `tch->be = 0;` inside `ppgCheckTextureDataBe` when all handles in a chunk have been released. Called via `ppgReleaseTextureHandle` at the bottom of `bg.c:232-234` (Bg_Close) and inside the Fix-B block at `bg.c:311`. **These DO fire for ppgBgTex[1] during stage transitions** — but only to set be=0, never to set it to 1.

So for stage 17's lifecycle:
1. ppgWorkInitializeApprication → ppgBgTex[1].be = 0.
2. Loading into stage 17 → Bg_Close (via System_all_clear_Level_B) → ppgReleaseTextureHandle → ppgBgTex[1].be = 0 (already was).
3. Bg_Texture_Load_EX → Fix-B loop → ppgReleaseTextureHandle → still 0.
4. Bg_Texture_Load_EX main loop → j=0 only → touches ppgBgTex[0], NEVER ppgBgTex[1].
5. Per-frame rendering queries ppgBgTex[1].be == 0 → fail.

**ppgBgTex[1].be stays 0 for the entire lifetime of a stage-17 session.** This is by design of the loader (given scrno=1). The fact that the renderer still asks about ppgBgTex[1] is the bug.

### Appendix 8 — Every write to `Screen_Switch` bit 1 during a stage-17 match

Sites that can set bit 1 of Screen_Switch:
- `Bg_On_R(2)` — sets bit 1 explicitly.
- `Bg_On_R(0x3)`, `Bg_On_R(0x6)`, etc. — composite sets.

Every call to `Bg_On_R(1 << 1)` across the codebase, filtered for stages relevant to stage 17:
- `src/sf33rd/Source/Game/stage/bg.c:337` — fires when `stage_bgw_number[17][1] = 2 > 0`. YES, sets bit 1.
- `src/sf33rd/Source/Game/game.c:636` — same condition. YES, sets bit 1.
- `src/sf33rd/Source/Game/stage/bg.c:452` (inside `Bg_Texture_Load2` for non-stage BG) — not reached in gameplay.
- `src/sf33rd/Source/Game/effect/eff77.c:118`, `effd3.c:114, 196` — conditional on effect state, specific bg_numbers. eff77's bg_num is 0 for standard Necro-stage (stage 5). effd3 is Akebono effect for stage 7. Neither triggers for stage 17.

Sites that clear bit 1:
- `Bg_Off_R(2)` — clears bit 1.
- `Bg_Off_R(0x6)`, etc.

Searching across the code: `Bg_Off_R(2)` and `Bg_Off_R(0xFFFD)` (mask that clears bit 1 while preserving others) — none are reachable in stage 17 gameplay. Specifically:
- `bg_sub.c:1104` — `Bg_Off_R(7)` (bitmask: clear bits not in 7, i.e. clears everything above bit 2). Runs BEFORE the Bg_On_R loop, so bit 1 reappears immediately.
- `bg_sub.c:1190` — `Bg_Off_R(8)` on akebono_initialize. Bit 1 unaffected.
- `end_main.c:80` — `Bg_Off_R(7)` on ending entry. Not gameplay.
- `effect/eff77.c:71, 92`, `effd3.c:56, 97, 156` — effect-triggered clears, none fire for stage 17.

**Bit 1 of `Screen_Switch_Buffer` is invariant at 1 during every frame of a stage-17 match.** Confirmed: `BG_Draw_System` calls `scr_trans(1)` every frame.

### Appendix 9 — Rollback replay sanity check

Scenario: latency causes peer to rollback N frames, replay with corrected inputs.

On replay:
- `load_state_from_event` at `game_state.c` restores `bg_w, Screen_Switch, Screen_Switch_Buffer, bg_pos, bg_prm, fm_pos, chainex_check, ...` — all correct.
- NOT restored: `ppgBgTex[], ppgBgList[].tex`, `ppg_w.mm heap, bg_priority[], scr_bcm[]`.
- If replay does NOT cross a `Bg_Texture_Load_EX` boundary: ppgBgTex state is whatever it was at the end of the original run. For stage 17 that's `ppgBgTex[0].be=1, ppgBgTex[1].be=0` — the correct state. Renderer queries match state. No new divergence.
- If replay DOES cross `Bg_Texture_Load_EX`: Fix-B tear-down at bg.c:307-317 zeroes be for ppgBgTex[0..2]. Main loop repopulates ppgBgTex[0] for stage 17. ppgBgTex[1] stays at be=0 (NEVER written). **Same final state as the pre-rollback run.**

Therefore rollback **cannot** change the ppgBgTex[1].be state after replay finishes. The black-BG mechanism produces identical output with or without rollback. Rollback only changes how quickly / how often we re-enter `Bg_Texture_Load_EX`, which is a performance concern, not a visibility one.

### Appendix 10 — Interaction with prior `Bg_Close` from `System_all_clear_Level_B`

Called from many game.c locations (game.c:360, 452, 1082, 1128, 1310, 1324, 1429, 1730, 1772, 1802, etc). `System_all_clear_Level_B` calls `Bg_Close` which:
1. Releases all ppgBgTex, ppgRwBgTex, ppgAkeTex, ppgAkanePal, ppgAkaneTex, ppgAkanePal handles (all be → 0).
2. Screen_Switch = 0.
3. Screen_Switch_Buffer = 0.
4. bg_disp_off = 0.

Before `Bg_Texture_Load_EX` runs (via `bg_initialize`), `System_all_clear_Level_B` has already run at some earlier game.c site (e.g. `game.c:452` inside `Game1_2`). So ppgBgTex are all be=0 entering `Bg_Texture_Load_EX`. The Fix-B block at bg.c:307-317 is **belt-and-suspenders**: it's a no-op in the normal flow but covers the rollback case where state is snapshot-restored mid-load. This is correct and should stay.

### Appendix 11 — Upstream (3sxtra) comparison matrix

| artefact                                                 | this port                                                 | 3sxtra                                                  | diff                                                        |
|----------------------------------------------------------|-----------------------------------------------------------|---------------------------------------------------------|-------------------------------------------------------------|
| `use_real_scr[22]`                                       | identical values                                          | identical values                                        | none                                                        |
| `use_scr[22]`                                            | identical                                                 | identical                                               | none                                                        |
| `stage_bgw_number[22][3]`                                | identical                                                 | identical                                               | none                                                        |
| `bg_index_tbl[22][3]`                                    | identical (row 17 = {4,4,4})                              | identical                                               | none                                                        |
| `bgtex_stage_gbix[22][3]`                                | identical                                                 | identical                                               | none                                                        |
| `bg_map_tbl[22][3]`                                      | identical                                                 | identical                                               | none                                                        |
| `Bg_Texture_Load_EX` scr_bcm loop bound                  | `use_real_scr[bg_w.stage]` (bg.c:331)                     | `use_real_scr[bg_w.stage]` (bg_load.c:149)              | none                                                        |
| `Bg_Texture_Load_EX` Bg_On_R loop                        | `stage_bgw_number[stage][i] > 0` (bg.c:335-339)           | `stage_bgw_number[stage][i] > 0` (bg_load.c:153-157)    | none                                                        |
| `Bg_Texture_Load_EX` main texture loop bound             | `bg_w.scrno` (bg.c:361)                                   | `bg_w.scrno` (bg_load.c:171)                            | none                                                        |
| `bg_initialize` scrno assignment                         | `use_real_scr[bg_w.bg_index]` (bg_sub.c:1114)             | `use_real_scr[bg_w.bg_index]` (bg_sub.c:1139)           | none                                                        |
| `Bg_Close` scope                                         | ppgBgTex, ppgRwBgTex, ppgAke*, ppgAkane* (bg.c:226-244)   | same + ModdedStage_Unload (bg_load.c:91-113)            | port lacks HD modded-stage plumbing — irrelevant to this bug |
| `Bg_TexInit` scope                                       | plain pal/tex wiring (bg.c:67-81)                         | + RENDERER_HAS_PLUGIN() ClearBGTileCache (bg_load.c:66) | irrelevant                                                   |
| Fix-B tear-down inside Bg_Texture_Load_EX                | YES (bg.c:307-317)                                        | NO                                                      | this port adds it to handle rollback; no regression          |

**Every behaviourally-relevant artefact on the black-BG path is identical between this port and 3sxtra.** The stage-17 inconsistency is therefore an upstream-inherited issue, not a port regression.

---

## Confidence summary

| Claim | Confidence | Basis |
|-------|-----------|-------|
| `bg_index_tbl[17] = {4,4,4}` is the only non-identity row | **Very high** | Direct read of bg_data.c:537-541. Enumerated every row. |
| `bg_w.scrno = use_real_scr[bg_w.bg_index]` → scrno=1 for stage 17 | **Very high** | bg_sub.c:1114 + use_real_scr[4]=1 |
| Screen_Switch for stage 17 = 0x3 | **Very high** | Two code sites set bits 0,1 from stage_bgw_number[17]={1,2,0}; no code site clears bit 1 for stage 17. Log confirms. |
| Layer 1 `scr_trans(1)` binds `ppgBgList[1]`, queries `ppgBgTex[1]` | **Very high** | bg.c:729; ppgCheckTextureNumber resolution at PPGFile.c:1860 |
| `ppgBgTex[1].be == 0` because j=1 never runs | **Very high** | bg.c:361 loop bound is bg_w.scrno=1; no other write site for ppgBgTex[1].be |
| clear color is black → uncovered pixels appear black | **High** | sdl_game_renderer.c:9487-9494, default FrameClearColor |
| Fix in §G resolves the black-BG INTERNAL STATE inconsistency | **Very high** | Eliminates the Screen_Switch bit 1 set, so scr_trans(1) never runs. |
| Fix makes stage 17 "look correct" | **Low** | Layer 0 still only covers 12 tiles. §G.4 and §G.5. |
| Upstream behaviour matches on stage 17 | **Medium-High** | Identical code in `/tmp/3sxtra/src/sf33rd/Source/Game/stage/bg_load.c:116-215`; table values identical. |
| Upstream users never encountered this | **Low (speculation)** | Probable because stage 17 is unreachable via standard UI paths in both ports; not verified. |
| Rollback is not causal | **Medium-High** | Mechanism is pure-function of saved state; rollback faithfully restores it. Could theoretically interact via unsaved `bg_priority[]` or `rw_num=0` stage-17 subtleties but none of those produce the observed symptom. |
| The "shift-left" regression is not from `scr_bcm` | **Very high** | `scr_bcm` is write-only per grep. |
| The "shift-left" cause is the `stg` / `bg_w.scno` index change | **Medium** | Inferred from code structure; reverted patch not in git log. |

---
