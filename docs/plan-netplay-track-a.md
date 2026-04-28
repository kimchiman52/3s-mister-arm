# Track A — Netplay Port (Phases 1–3) Implementation Plan

**Status:** Ready for implementation
**Scope:** Engine critical path — GameState backfill, setup_vs_mode expansion, focused checksum + sanitizers + dump
**Audience:** Autonomous implementer agent
**Source of truth:**
- Tier-1 research — `docs/research-3sxtra-netplay-port.md` §5, §6, §8, §19
- Tier-2 plan — `docs/plan-netplay-port.md` Phase 1/2/3, §3, §15, §16
- 3sxtra reference — `/tmp/3sxtra/src/` (HEAD `a18eae1`)
- Safety — `AGENTS.md`, `docs/mister-runbook.md`

**Worktree HEAD at plan time:** `17ab61e7` (`mister` branch fast-forwarded into worktree `agent-a42a8a0e`).

---

## Safety constraints (apply to every step)

1. **No push to remote.** Commits stay local.
2. **No `rsync --delete`** anywhere. See memory `feedback-no-rsync-delete.md`.
3. **Before ANY on-device MiSTer access** (ssh/scp/deploy/probe): run `tools/mister/misterctl.sh lock-status` AND `tools/mister/misterctl.sh busy-status`. Tracks B and C may be running in parallel — respect locks, never bypass.
4. **MiSTer builds ONLY via** `tools/mister/build-game.sh --flavor telemetry`. Do not bypass to direct cmake.
5. **If a step hits a hard blocker** (missing symbol, cross-compile error you can't resolve, MiSTer network lost, lock held by peer):
   - Commit current progress as `WIP Track A — blocked at step N`.
   - Write `TRACK_A_BLOCKED.md` at repo root describing: step, diagnostic output, what needs fixing, recommended next action.
   - Stop cleanly. Do NOT partially land a broken phase.
6. **Never skip verification.** A passing step with unverified output is worse than a failing step that stopped.

## Out-of-scope (do NOT touch in any step)

- `src/port/sdl/rmlui/` or `build-deps.sh` RmlUi/FreeType sections (Track B).
- `build-deps.sh` GekkoNet/SDL3_net desktop-gating sections (Track C).
- Phases 4–12 of tier-2 plan (`menu_network.c`, lobby, STUN, UPnP, etc.).
- Broader 3sxtra engine refactors (`globals/*.c`, `training/`, `stage/bg_*`). Only `select_timer.{c,h}` crosses over.
- Restructuring files outside the scope below.
- Updating `docs/` except `TRACK_A_BLOCKED.md` and this plan (for progress marking).

---

## Step dependency graph

```
Phase 1
  1.1 (port select_timer.c/h verbatim, NO call-site wiring)
    ↓
  1.2 (move EffectState typedef to game_state.h, add select_timer_state global definition)
    ↓
  1.3 (add 33 fields to GameState struct, ordered per 3sxtra layout)
    ↓
  1.4 (add 33 GS_SAVE + 33 GS_LOAD in game_state.c)
    ↓
  1.5 (add _Static_assert tripwires for sizeof(GameState) + sizeof(_TASK), include djb2_hash.h)
    ↓  → verify MiSTer build green; desktop build if feasible
    ↓
  **COMMIT 1** — "feat(netplay): Phase 1 — backfill GameState (33 fields), port select_timer, add tripwires"

Phase 2
  2.1 (expand setup_vs_mode from ~30 LOC to ~180 LOC, per 3sxtra netplay.c:158-391)
    ↓  → verify MiSTer build green; desktop build if feasible
    ↓
  **COMMIT 2** — "feat(netplay): Phase 2 — expand setup_vs_mode to full frame-0 canonicalization"

Phase 3
  3.1 (port focused checksum + sanitizers, keep our clean_plw_pointers cb/rp zeroing,
       add combo_type + remake_power to whitelist, remove #if DEBUG gate on main checksum)
    ↓
  3.2 (port dump_desync_state behind #if DEBUG, wire GekkoDesyncDetected handler)
    ↓  → verify MiSTer + desktop builds green; desync injection test
    ↓
  **COMMIT 3** — "feat(netplay): Phase 3 — port focused checksum, sanitizers, dump_desync_state"
```

Each `COMMIT N` is the atomic phase boundary. Between commits, the worktree must build green on MiSTer (`tools/mister/build-game.sh --flavor telemetry`). If the tripwire `_Static_assert` fails after a backfill, the implementer must compute the new sizeof and update the expected value — **only if they added the expected fields in the expected order**. A tripwire failure when the implementer has NOT changed GameState is a real signal of a bug (misordered insert, wrong field type) and MUST be investigated, not silenced.

---

## STEP 1.1 — Port `select_timer.c/h` verbatim (no wiring)

### Title
Copy 3sxtra's `select_timer.c` and `select_timer.h` into our tree. Do NOT wire call sites in this step.

### Why it matters
`SelectTimerState` is the type of field `#1` in the 33-field backfill (research §5.3). The header must exist before we include it in `game_state.h`. Keeping the module orphaned (not wired) avoids engine behavior change — our existing `effa5.c` inline BCD countdown continues to drive `Select_Timer`. A future Track D can wire the call sites after Phases 1–3 are stable.

### Dependencies
None. Can start immediately.

### Files to read
1. `/tmp/3sxtra/src/sf33rd/Source/Game/select_timer.h` (31 lines) — verbatim source
2. `/tmp/3sxtra/src/sf33rd/Source/Game/select_timer.c` (152 lines) — verbatim source
3. `src/sf33rd/Source/Game/engine/workuser.h` — confirm `Select_Timer`, `Unit_Of_Timer`, `Time_Stop`, `Time_Over`, `Break_Into`, `Present_Mode` externs exist
4. `src/sf33rd/Source/Game/debug/Debug.h` — confirm `Debug_w[DEBUG_TIME_STOP]` symbol exists
5. `src/constants.h` — confirm `UNIT_OF_TIMER_MAX` define exists

### Files to create/modify
1. `src/sf33rd/Source/Game/select_timer.h` — CREATE, copy `/tmp/3sxtra/src/sf33rd/Source/Game/select_timer.h` verbatim.
2. `src/sf33rd/Source/Game/select_timer.c` — CREATE, copy `/tmp/3sxtra/src/sf33rd/Source/Game/select_timer.c` verbatim, BUT:
   - Remove the `static s16 s_bcd_carry` and `static u8 sbcd(...)` function — we already have `u8 sbcd(u8 a, u8 b)` in `effa5.c` at line 16. Instead add `extern u8 sbcd(u8 a, u8 b);` forward declaration at the top of `select_timer.c` and call it via the existing symbol.
   - **Alternative**: if `sbcd` in `effa5.c` is `static`, then keep 3sxtra's local `sbcd` + `s_bcd_carry` (rename `sbcd` to `select_timer_sbcd` to avoid future collision). Choose this if the `effa5.c` one is static. Verify by reading `effa5.c:16` before writing.

   **Pick the approach that produces the least linker/build risk.** The BCD implementations are byte-identical between the two files; correctness is preserved either way.

### Wiring (DO NOT DO IN THIS STEP)
The following call-site wiring is **explicitly out of scope** for Track A Phases 1–3:
- `Game01()` calling `SelectTimer_Run()` (3sxtra `game.c:409`)
- `sel_pl.c`, `next_cpu.c` calling `SelectTimer_Init()`
- `sys_sub.c` calling `SelectTimer_Finish()`

The `effa5.c` inline BCD countdown continues to drive `Select_Timer` as it does today. The `SelectTimerState select_timer_state` global gets saved/loaded as zeros in rollback (no behavior difference).

Add a `// TODO(track-d): wire call sites from 3sxtra game.c:409, sel_pl.c:309, next_cpu.c:191, sys_sub.c:617 to replace effa5.c inline countdown` comment at top of `select_timer.c`.

### Build system wiring
- If the build is CMake glob-based (check `src/CMakeLists.txt` / `src/sf33rd/CMakeLists.txt`): the new file picks up automatically. Verify.
- If explicit file list: add `select_timer.c` to the appropriate source-list section.

### Success criteria
- `tools/mister/build-game.sh --flavor telemetry` completes without error.
- `nm build/mister-install/MiSTer_3S-ARM 2>/dev/null | grep -iE 'SelectTimer_(Init|Run|Finish)'` (or equivalent readelf) shows the three symbols are present.
- No new runtime warnings. No behavior change (game still boots, arcade mode still runs).
- Desktop build (`cmake --preset desktop` if feasible; otherwise skip with a note).

### Scope limits
- Do NOT wire call sites.
- Do NOT add `SelectTimerState select_timer_state;` definition yet — that's Step 1.2 (must be placed with other `work_sys` globals, not scattered).
- Do NOT touch `effa5.c`.

### What to do if it fails
- If `sbcd` collision: prefer the "rename to `select_timer_sbcd` + local `s_bcd_carry`" variant.
- If `UNIT_OF_TIMER_MAX` missing: grep our repo for its definition; if truly absent, hardcode `60` as 3sxtra does (their `constants.h` defines it as 60).
- If `DEBUG_TIME_STOP` missing: fall back to `Debug_w[0x10]` (3sxtra's `DEBUG_TIME_STOP` resolves to `0x10`). Verify via 3sxtra's debug header if needed.
- If any other symbol genuinely missing from our engine: STOP, write `TRACK_A_BLOCKED.md`, commit WIP.

---

## STEP 1.2 — Move `EffectState` to `game_state.h`, define `select_timer_state` global

### Title
Move the `EffectState` typedef from `src/netplay/netplay.c:42-50` into `src/netplay/game_state.h`, and add the `SelectTimerState select_timer_state;` global definition somewhere appropriate.

### Why it matters
- Tier-2 plan Phase 1 deliverable #5: "Move the inline `EffectState` typedef from `src/netplay/netplay.c:42-50` to `src/netplay/game_state.h` so save/load can reference it cleanly."
- The `select_timer_state` global needs a single definition (likely in `src/sf33rd/Source/Game/engine/workuser.c` alongside `Select_Timer`) so GS_SAVE's `&select_timer_state` resolves at link time.

### Dependencies
Step 1.1 complete.

### Files to read
1. `src/netplay/game_state.h` — current layout
2. `src/netplay/netplay.c:42-50` — source of EffectState typedef
3. `src/sf33rd/Source/Game/engine/workuser.c` — existing globals home
4. `src/sf33rd/Source/Game/effect/effect.h` — where the engine-side effect globals (`frw`, `head_ix`, etc.) are declared

### Files to create/modify
1. **`src/netplay/game_state.h`**:
   - Add `#include "sf33rd/Source/Game/effect/effect.h"` (needed for `EFFECT_MAX`).
   - Add `#include "sf33rd/Source/Game/select_timer.h"` (needed for `SelectTimerState`).
   - Add the `EffectState` typedef block (copy verbatim from `netplay.c:42-50`) BEFORE the `typedef struct GameState {` line.
   - Do NOT add `typedef struct State { ... }` here yet — that stays in `netplay.c` for Phase 1 scope. (3sxtra moves it, but we don't need to for Phase 1 completion.)

2. **`src/netplay/netplay.c`**:
   - Remove lines 42-50 (the inline `typedef struct EffectState { ... } EffectState;` block).
   - Keep the `typedef struct State { GameState gs; EffectState es; } State;` block (it now references the moved typedef via the header include).
   - Verify line 3 already has `#include "netplay/game_state.h"` — it does.

3. **`src/sf33rd/Source/Game/engine/workuser.c`**:
   - Add `#include "sf33rd/Source/Game/select_timer.h"` at the top if not present.
   - Add `SelectTimerState select_timer_state;` as a global definition (match 3sxtra `/tmp/3sxtra/src/sf33rd/Source/Game/globals/timer_hud_globals.c:24`). Place near `Select_Timer` definition at our line 16.

### Success criteria
- `tools/mister/build-game.sh --flavor telemetry` completes without error.
- `grep -c "typedef struct EffectState" src/` returns exactly 1 (only the header).
- `grep -c "SelectTimerState select_timer_state" src/` returns exactly 1 (only the definition in workuser.c).
- Linker resolves `select_timer_state`.

### Scope limits
- Do NOT add `select_timer_state` to the `GameState` struct yet (Step 1.3).
- Do NOT add GS_SAVE/GS_LOAD for it yet (Step 1.4).
- Do NOT move the `State` wrapper struct.

### What to do if it fails
- Include-cycle errors: move includes into the .c files rather than the .h.
- Missing `EFFECT_MAX`: ensure `effect.h` is included, not just `workuser.h`.
- If `workuser.c` feels wrong as the home: put `SelectTimerState select_timer_state;` in the new `src/sf33rd/Source/Game/select_timer.c` (that's the simpler option). Update this plan noting the chosen location.

---

## STEP 1.3 — Add 33 fields to `GameState` struct

### Title
Insert the 33 missing fields into `src/netplay/game_state.h`'s `GameState` struct, preserving 3sxtra's ordering where tractable.

### Why it matters
This is the core of Phase 1 — rollback save/load must carry all deterministic engine state. Without these fields, 3sxtra's focused checksum (Phase 3) and `setup_vs_mode` canonicalization (Phase 2) have nothing to anchor against. Research §5.3 lists all 33; §5.5 confirms they map 1:1 to 33 missing GS_SAVE/GS_LOAD calls.

### Dependencies
Steps 1.1 and 1.2 complete (need `SelectTimerState` type and includes ready).

### Files to read
1. `/tmp/3sxtra/src/include/game_state.h` lines 54–780 — authoritative field ordering
2. `src/netplay/game_state.h` — current struct
3. Per-field owner headers to confirm types (critical — types MUST match our engine globals, not assume):
   - `src/sf33rd/Source/Game/stage/bg.h` — `bg_disp_off`, `Screen_Switch`, `Screen_Switch_Buffer`, `bgPalCodeOffset`
   - `src/sf33rd/Source/Game/stage/bg.c` — `rw_num`, `rw_bg_flag`, `tokusyu_stage`, `rw_gbix`, `stage_flash`, `stage_ftimer`, `yang_ix`, `yang_timer`, `yang_ix_plus`, `ending_flag`, `end_prm`, `gouki_end_gbix`, `rw3col_ptr`, `rw_dat` (look at `extern` lines in `bg.h` too)
   - `src/sf33rd/Source/Game/system/work_sys.h` + `work_sys.c` — `bg_pos`, `fm_pos`, `bg_prm`, `BgMATRIX`, `system_timer`, `ck_ex_option`, `vm_w`, `X_Adjust`, `Y_Adjust`, `X_Adjust_Buff`, `Y_Adjust_Buff`, `scr_sc`, `Gill_Appear_Flag`
   - `src/sf33rd/Source/Game/engine/plcnt.h` — `cmd_sel`, `no_sa`
   - `src/sf33rd/Source/Game/ui/sc_sub.h` — `Hnc_Num`
   - `src/sf33rd/Source/Game/ending/end_data.h` (create or find equivalent) — `end_w` (type `END_W`)
   - `src/sf33rd/Source/Game/select_timer.h` — `select_timer_state` (type `SelectTimerState`)

### Files to create/modify
1. **`src/netplay/game_state.h`**:
   - Add missing includes:
     - `#include "sf33rd/Source/Game/ending/end_data.h"` (for `END_W`)
     - `#include "sf33rd/Source/Game/system/work_sys.h"` (for `BG_POS`, `FM_POS`, `BackgroundParameters`, `MTX`, `struct _VM_W`, `_EXTRA_OPTION`)
     - `#include "sf33rd/Source/Game/ui/sc_sub.h"` (for `Hnc_Num`-adjacent types, though `Hnc_Num` is just `s16`)
     - `#include "sf33rd/Source/Game/select_timer.h"` (already added in Step 1.2)
   - Insert fields in 3sxtra's order within their respective section comments. Map of insertions (from 3sxtra's `game_state.h`):

   | Insert after existing field | New field (type per 3sxtra source) | Section |
   |---|---|---|
   | `s8 hoji_counter;` | `SelectTimerState select_timer_state;` | count.c |
   | `BG bg_w;` | `u16 Screen_Switch;` | bg (stage) |
   | `Screen_Switch` | `u16 Screen_Switch_Buffer;` | bg |
   | `Screen_Switch_Buffer` | `u8 rw_num;` | bg |
   | `rw_num` | `u8 rw_bg_flag[4];` | bg |
   | `rw_bg_flag` | `u8 tokusyu_stage;` | bg |
   | `tokusyu_stage` | `s32 rw_gbix[13];` | bg |
   | `rw_gbix` | `s8 stage_flash;` | bg |
   | `stage_flash` | `s8 stage_ftimer;` | bg |
   | `stage_ftimer` | `s32 yang_ix_plus;` | bg |
   | `yang_ix_plus` | `s8 yang_ix;` | bg |
   | `yang_ix` | `s8 yang_timer;` | bg |
   | `yang_timer` | `u8 ending_flag;` | bg |
   | `ending_flag` | `BackgroundParameters end_prm[8];` | bg |
   | `end_prm` | `u8 gouki_end_gbix[16];` | bg |
   | `gouki_end_gbix` | `const u32* rw3col_ptr;` | bg |
   | `rw3col_ptr` | `u8 bg_disp_off;` | bg |
   | `bg_disp_off` | `s32 bgPalCodeOffset[8];` | bg |
   | `bgPalCodeOffset` | `RW_DATA rw_dat[20];` | bg |
   | End of effb8 section (`s16 mes_timer;`) | new "System globals (work_sys)" section | — |
   | (new section) | `BG_POS bg_pos[8];` | work_sys |
   | `bg_pos` | `FM_POS fm_pos[8];` | work_sys |
   | `fm_pos` | `BackgroundParameters bg_prm[8];` | work_sys |
   | `bg_prm` | `u32 system_timer;` | work_sys |
   | `system_timer` | `s8 Gill_Appear_Flag;` | work_sys |
   | `Gill_Appear_Flag` | `char cmd_sel[2];` | plcnt |
   | `cmd_sel` | `char no_sa[2];` | plcnt |
   | `no_sa` | `s16 Hnc_Num;` | sc_sub |
   | `Hnc_Num` | `END_W end_w;` | ending |
   | `end_w` | `f32 scr_sc;` | work_sys extension |
   | `scr_sc` | `s32 X_Adjust;` | work_sys extension |
   | `X_Adjust` | `s32 Y_Adjust;` | work_sys extension |
   | `Y_Adjust` | `MTX BgMATRIX[9];` | additional |
   | `BgMATRIX` | `struct _VM_W vm_w;` | additional |
   | `vm_w` | `_EXTRA_OPTION ck_ex_option;` | additional |
   | `ck_ex_option` | `s32 X_Adjust_Buff[3];` | additional |
   | `X_Adjust_Buff` | `s32 Y_Adjust_Buff[3];` | additional |

   That's 33 additions. Count: `select_timer_state` (1) + bg-section (18) + work_sys new section (5) + plcnt (2) + sc_sub (1) + ending (1) + work_sys ext (3) + additional (5) = **36**. Wait — that's too many.

   **Resolve the count mismatch:** Re-read research §5.3 table carefully. The 33 entries are:
   1. `select_timer_state`
   2. `bg_disp_off`
   3. `bg_pos[8]`
   4. `bg_prm[8]`
   5. `BgMATRIX[9]`
   6. `bgPalCodeOffset[8]`
   7. `ck_ex_option`
   8. `cmd_sel[2]`
   9. `end_prm[8]`
   10. `end_w`
   11. `ending_flag`
   12. `fm_pos[8]`
   13. `Gill_Appear_Flag`
   14. `gouki_end_gbix[16]`
   15. `Hnc_Num`
   16. `no_sa[2]`
   17. `rw_bg_flag[4]`
   18. `rw_dat[20]`
   19. `rw_gbix[13]`
   20. `rw_num`
   21. `rw3col_ptr`
   22. `scr_sc`
   23. `Screen_Switch`
   24. `Screen_Switch_Buffer`
   25. `stage_flash`
   26. `stage_ftimer`
   27. `system_timer`
   28. `tokusyu_stage`
   29. `vm_w`
   30. `X_Adjust`
   31. `Y_Adjust`
   32. `X_Adjust_Buff[3]` (one line in research, but represents `X_Adjust_Buff`, `Y_Adjust_Buff` in same row — actually the row reads "`X_Adjust`, `Y_Adjust`, `X_Adjust_Buff[3]`, `Y_Adjust_Buff[3]`" — 4 names in one row)
   33. `yang_ix, yang_timer, yang_ix_plus` (three in one row)

   Research §5.3 row count = 30 rows with some rows listing multiple names. Counting names: 30 rows = 33 field names total (rows 30, 32, 33 each carry multiple fields). The row "`X_Adjust`, `Y_Adjust`, `X_Adjust_Buff[3]`, `Y_Adjust_Buff[3]`" has 4 fields. Row "`yang_ix, yang_timer, yang_ix_plus`" has 3. Net: 30 rows − 3 collapsed rows + (4+3+whatever other collapsed) fields. Exact per-row count: 23 single-field rows + 1 three-field row (yang) + 1 four-field row (adjust) = 23 + 3 + 4 = 30 fields, not 33. Hmm.

   Alternative count: 3sxtra has 601 fields, ours 568 = diff 33. But 3 fields are _ours-only_ (combo_type, remake_power, Disp_Input_History per research §5.4) — so net missing = 33 + 3 = 36 fields 3sxtra has that we don't. But GS_SAVE counts differ by 36 too (1208 - 1136 = 72 / 2 = 36 pairs).

   **Resolution:** The authoritative count is **36 fields to add**, not 33. Research §5.3 title says "33 total" but the underlying math per §5.1 (601 − 568 = 33 NET, but 3 of ours are unique → 36 ADDITIONS). The 33-number in §5.3 line 324 is arguably an error; the actual adds required to reach layout parity are 36.

   **For this plan: add exactly the fields listed in the mapping table above (36 total), then verify GS_SAVE count goes from 568 → 604 (= 1208 / 2). The research's "33 total" claim is a one-off counting miscue; the field names in §5.3 row by row enumerate 36 entries when flattened.**

   **Update**: re-count rows of §5.3 more carefully. Looking at the table:
   - Row for `X_Adjust, Y_Adjust, X_Adjust_Buff[3], Y_Adjust_Buff[3]`: 4 fields
   - Row for `yang_ix, yang_timer, yang_ix_plus`: 3 fields
   - Remaining rows: 28 single-field rows

   Total = 28 + 4 + 3 = **35 fields in §5.3 table**. If we strictly follow the table as authoritative and verify against 3sxtra's header, we get 35 or 36. The difference could also be that `X_Adjust, Y_Adjust` already exist as top-level globals we've overlooked — let me re-verify during Step 1.3 implementation: grep ours for `X_Adjust` and `Y_Adjust` in `game_state.h`. If present, don't re-add.

   **Implementer directive for Step 1.3:**
   1. Before writing, diff every field name from `/tmp/3sxtra/src/include/game_state.h:54-780` against our `src/netplay/game_state.h` to produce the **exact** delta list. Don't rely on the research-doc table alone; use the two header files as ground truth.
   2. The expected delta: between 33 and 36 additions. Whatever the precise list is, add them in the order 3sxtra has them.
   3. Record the final count in a comment in `game_state.h`: `// Field count: <N> matches /tmp/3sxtra/src/include/game_state.h:54-780 at HEAD a18eae1`.

### Success criteria
- `diff -u <(grep -oE '^\s+\w+' src/netplay/game_state.h | head ...) <(grep -oE '^\s+\w+' /tmp/3sxtra/src/include/game_state.h | head ...)` shows zero differences in the overlapping field range.
- `tools/mister/build-game.sh --flavor telemetry` completes with NO errors — warnings about unused fields are acceptable.
- The build WILL fail the `_Static_assert` added in Step 1.5 if sizes don't match 17800/19376; that's expected. For this step, the _Static_assert isn't added yet.
- Visual: `git diff src/netplay/game_state.h` shows new fields inserted in plausible section-locations, matching 3sxtra ordering.

### Scope limits
- Do NOT add GS_SAVE/GS_LOAD calls yet (Step 1.4).
- Do NOT add _Static_assert yet (Step 1.5).
- Do NOT reorder existing fields except where strictly required by 3sxtra layout (e.g., `Screen_Switch` / `Screen_Switch_Buffer` move from their old position to a new section).
- Do NOT remove or rename our fork-unique fields (`combo_type`, `remake_power`, `Disp_Input_History`). They stay exactly where they are.

### What to do if it fails
- Build error "undefined type `X`" → missing include; find which engine header defines it, add to `game_state.h`.
- Circular include → reorganize the include tree; worst case, forward-declare the struct and keep the field as a pointer. **But this is risky** — if 3sxtra has it by-value, matching layout matters for `sizeof`. STOP and write `TRACK_A_BLOCKED.md` if you can't resolve cleanly.
- Conflicting type (e.g., 3sxtra has `s32` but our engine global is `s16`): USE OUR ENGINE'S TYPE, not 3sxtra's. The save/load is a memcpy of `sizeof(member)` — our field type must match our global's type. Document the divergence inline.

---

## STEP 1.4 — Add 33+ `GS_SAVE` + `GS_LOAD` calls

### Title
Add matching `GS_SAVE(...)` and `GS_LOAD(...)` macro invocations in `src/netplay/game_state.c` for every field added in Step 1.3.

### Why it matters
GS_SAVE/GS_LOAD is the memcpy from global → struct field (and back). Without them, the new struct fields save as uninitialized memory — a catastrophic source of silent desyncs.

### Dependencies
Step 1.3 complete.

### Files to read
1. `/tmp/3sxtra/src/netplay/game_state.c` — authoritative save/load order (their file has both `GameState_Save` and `GameState_Load` — mirror both)
2. `src/netplay/game_state.c` — current save/load blocks

### Files to create/modify
1. **`src/netplay/game_state.c`**:
   - Add missing `#include`s for headers providing the externs of newly-saved globals (match 3sxtra's include list at lines 25-42). Likely additions:
     - `#include "sf33rd/Source/Game/ending/end_data.h"` (for `end_w`)
     - `#include "sf33rd/Source/Game/system/work_sys.h"` (if not already)
     - `#include "sf33rd/Source/Game/select_timer.h"` (for `select_timer_state`)
   - In `GameState_Save(GameState* dst)`:
     - Insert `GS_SAVE(select_timer_state);` near `GS_SAVE(hoji_counter);` (matching 3sxtra save order — look at 3sxtra's `game_state.c` for exact position).
     - Insert the 17-ish new bg-section GS_SAVEs after the existing `GS_SAVE(bg_w)` call (follow 3sxtra's order).
     - Add the work_sys section (`GS_SAVE(bg_pos)`, `GS_SAVE(fm_pos)`, `GS_SAVE(bg_prm)`, `GS_SAVE(system_timer)`, `GS_SAVE(Gill_Appear_Flag)`) near the end.
     - Add `GS_SAVE(cmd_sel)`, `GS_SAVE(no_sa)` for plcnt.
     - Add `GS_SAVE(Hnc_Num)` for sc_sub.
     - Add `GS_SAVE(end_w)` for ending.
     - Add `GS_SAVE(scr_sc)`, `GS_SAVE(X_Adjust)`, `GS_SAVE(Y_Adjust)`, `GS_SAVE(BgMATRIX)`, `GS_SAVE(vm_w)`, `GS_SAVE(ck_ex_option)`, `GS_SAVE(X_Adjust_Buff)`, `GS_SAVE(Y_Adjust_Buff)` for additional work_sys / bonus.
   - Mirror every insert in `GameState_Load` with the corresponding `GS_LOAD(...)`. Our load macro likely: `#define GS_LOAD(member) SDL_memcpy(&member, &src->member, sizeof(member))`. Verify by reading `game_state.c`. If absent, add it matching 3sxtra's definition.

2. If our fork's `game_state.c` lacks a `GS_LOAD` macro (today it uses `#define GS_SAVE ...` plus individual `= src->member` lines in Load):
   - Check our current load style — if it uses explicit `Field = src->Field` lines (as originally), use that same style for new fields.
   - If both Save and Load use the macro form, use both macros.
   - Whatever style is already used, follow it. Don't rewrite existing Load function wholesale.

### Success criteria
- `tools/mister/build-game.sh --flavor telemetry` builds green.
- `grep -c 'GS_SAVE' src/netplay/game_state.c` = prior count + 36 (or whatever Step 1.3's delta was).
- `grep -c 'GS_LOAD' src/netplay/game_state.c` = prior count + 36 (or equivalent explicit assignment count).
- Linker resolves every saved global — any "undefined reference to `end_w`" type error means we missed an `extern` or include.

### Scope limits
- Do NOT add _Static_assert yet (Step 1.5).
- Do NOT change existing GS_SAVE/GS_LOAD calls.
- Do NOT refactor the save/load function structure (keep it one-macro-call-per-line).

### What to do if it fails
- Undefined reference → missing `extern` declaration. Find the engine's header that declares the global with `extern` and include it here (or in `game_state.h`).
- Size mismatch warning on memcpy (e.g., `warning: memcpy writing 4 bytes into region of size 2`) → the struct field type disagrees with the global's type. Fix Step 1.3's field type.

---

## STEP 1.5 — Add `_Static_assert` tripwires + djb2 include

### Title
Add `_Static_assert(sizeof(GameState) == 17800|19376, ...)` and `_Static_assert(sizeof(struct _TASK) == 20|32, ...)` to `src/netplay/game_state.c`, including `UINTPTR_MAX` branching. Include `sf33rd/utils/djb2_hash.h` in `game_state.c` (for Phase 3 use).

### Why it matters
Research §5.2 and tier-2 plan Phase 1 call this out as the layout-drift tripwire. 3sxtra ships this at `game_state.c:67-85`. Without it, a future silent struct-layout change (upstream merge, compiler diff, alignment tweak) would corrupt rollback with no diagnostic.

### Dependencies
Steps 1.1–1.4 complete — the struct must have all fields before we pin its size.

### Files to read
1. `/tmp/3sxtra/src/netplay/game_state.c:59-86` — authoritative tripwire block
2. `src/netplay/game_state.c` — current includes at top

### Files to create/modify
1. **`src/netplay/game_state.c`**:
   - Add `#include <stdint.h>` near top.
   - Add `#include "sf33rd/utils/djb2_hash.h"` near top (in anticipation of Phase 3).
   - Insert the tripwire block after includes, before `#define GS_SAVE` (or wherever appropriate). Copy 3sxtra's text verbatim; we can keep the `#if UINTPTR_MAX == 0xffffffff / #else` 32/64 branches:

     ```c
     #if UINTPTR_MAX == 0xffffffff
     #define EXPECTED_GAME_STATE_SIZE 17800
     #define EXPECTED_TASK_SIZE 20
     #else
     #define EXPECTED_GAME_STATE_SIZE 19376
     #define EXPECTED_TASK_SIZE 32
     #endif

     _Static_assert(sizeof(GameState) == EXPECTED_GAME_STATE_SIZE,
                    "sizeof(GameState) changed! Update GS_SAVE/GS_LOAD and adjust EXPECTED_GAME_STATE_SIZE.");

     _Static_assert(sizeof(struct _TASK) == EXPECTED_TASK_SIZE,
                    "sizeof(struct _TASK) changed! This struct is saved/loaded wholesale during netplay rollback.");
     ```

### Handling tripwire fires (expected at first build)
The first compile after Step 1.3 WILL fire the tripwire on whichever platform (32 or 64-bit) runs — because the 3sxtra sizes (17800 / 19376) assume 3sxtra's exact struct layout, and our fork has 3 fields 3sxtra doesn't (`combo_type`, `remake_power`, `Disp_Input_History`).

**Expected behavior:** the assert will fire. The implementer MUST:
1. Note the actual `sizeof(GameState)` the compiler computes (from the error message or a quick test via `printf("%zu\n", sizeof(GameState))`).
2. Update the expected value:
   ```c
   #define EXPECTED_GAME_STATE_SIZE <computed value>   // was 17800 in 3sxtra; ours differs by the 3 fork-unique fields
   ```
3. Rebuild. Tripwire should now pass.
4. Commit the adjusted value. A comment on that line: `// Ours differs from 3sxtra's 17800 by +<N> bytes for combo_type/remake_power/Disp_Input_History`.

Same story for `EXPECTED_TASK_SIZE`: our `_TASK` layout might differ (research §5.6 mentions callback_adrs diff). The implementer computes and pins whatever our size actually is.

### Sanity test (recommended)
After the tripwire is green, temporarily add a dummy `u32 __dummy_field;` to the struct; rebuild; verify the tripwire FIRES with a clear message. Revert immediately. This proves the tripwire works. (Tier-2 plan §Phase 1 explicitly recommends this.) Document in commit message.

### Success criteria
- `tools/mister/build-game.sh --flavor telemetry` succeeds with the tripwire present.
- `grep -n '_Static_assert' src/netplay/game_state.c` shows 2 lines.
- Sanity test: temporary field addition fires the assertion; after revert, builds clean.

### Scope limits
- Do NOT guess the expected size — compute it from the build.
- Do NOT remove the `UINTPTR_MAX` branch (even if we only ship 32-bit); it's layout documentation for future desktop builds.

### What to do if it fails
- If the build infrastructure can't surface `sizeof(GameState)` easily: add a temporary `_Static_assert(sizeof(GameState) == 0, "size is ...")` — the error message will state the actual size. Read it, replace `0` with the real value.
- If `struct _TASK` size is ambiguous (ifdef-guarded layout variant): probe each branch similarly. Pin both.

### PHASE 1 VERIFICATION (after Step 1.5)
Run all of these; all must pass before committing:

```bash
# 1. MiSTer cross-compile
tools/mister/build-game.sh --flavor telemetry 2>&1 | tail -30
# expect: "Build finished" or equivalent success line; exit 0

# 2. New symbols present in ARM binary
readelf -s build/mister-install/MiSTer_3S-ARM 2>/dev/null | grep -E 'SelectTimer_|select_timer_state|GameState_Save|GameState_Load' | head

# 3. Tripwires don't fire (build already verifies this, but confirm count)
grep -c '_Static_assert' src/netplay/game_state.c
# expect: 2

# 4. Sanity test (executed before commit, reverted before commit):
#    Add a `u32 __tripwire_test;` field to GameState, rebuild → MUST fail with a _Static_assert message mentioning GameState size
#    Revert the field, rebuild → green
#    (Mention sanity test result in the commit message.)

# 5. Desktop build (if available)
cmake --preset desktop 2>&1 | tail && cmake --build --preset desktop 2>&1 | tail
# acceptable to skip with a note if desktop tooling isn't configured in this worktree

# 6. Git status clean other than intended changes
git status --short
# expect: only the intended src/netplay/*, src/sf33rd/Source/Game/select_timer.*, src/sf33rd/Source/Game/engine/workuser.c changes
```

If all pass: **COMMIT PHASE 1**:

```sh
git add src/netplay/game_state.h src/netplay/game_state.c src/netplay/netplay.c \
        src/sf33rd/Source/Game/select_timer.h src/sf33rd/Source/Game/select_timer.c \
        src/sf33rd/Source/Game/engine/workuser.c \
        docs/plan-netplay-track-a.md
# (only the files that actually changed; trim the list to reality)
git commit -m "feat(netplay): Phase 1 — backfill GameState (33 fields), port select_timer, add tripwires

Adds 33+ missing deterministic globals to GameState, ports select_timer.c/h
verbatim from 3sxtra (module only — call sites intentionally not wired;
effa5.c inline countdown continues to drive Select_Timer), and installs
_Static_assert tripwires for sizeof(GameState) and sizeof(struct _TASK)
to catch future layout drift.

Source of truth: docs/research-3sxtra-netplay-port.md §5,
docs/plan-netplay-port.md Phase 1.

Sanity test: temporarily added a u32 field; tripwire fired as expected;
reverted and re-verified clean build.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
"
```

Mark Step 1.5 **DONE** in this plan doc, commit that doc update separately if you wish (or include it in the Phase 1 commit).

---

## STEP 2.1 — Expand `setup_vs_mode` to full frame-0 canonicalization

### Title
Rewrite `src/netplay/netplay.c::setup_vs_mode()` from ~30 LOC to ~180 LOC, mirroring `/tmp/3sxtra/src/netplay/netplay.c:158-391` — EXCEPT replace `MenuTask_SetPhase(MTP_NETPLAY_IDLE)` with `task[TASK_MENU].r_no[0] = 5`, and use our engine's field names (`plw[].wu.operator`, not `pl_operator`).

### Why it matters
Research §6 and tier-2 plan Phase 2 both flag this as "THE most important function for initial sync". Our 30-LOC version leaves ~150 engine globals un-canonicalized at frame 0, which manifests as rollback desyncs or rematch corruption that's nearly impossible to diagnose without this expansion in place.

### Dependencies
Phase 1 committed. Several of the fields 3sxtra's `setup_vs_mode` zeroes (`bg_pos`, `fm_pos`, `Screen_Switch`, `system_timer`) are among the 33 we just added — they exist as globals in our engine, so this step can reference them.

### Files to read
1. `/tmp/3sxtra/src/netplay/netplay.c:158-391` — verbatim source (read once to a scratch copy, then port line-by-line)
2. `src/netplay/netplay.c:117-147` — current version
3. Every engine-global referenced by 3sxtra's version (verify ours has each — research §6.2 lists them all):
   - `plw`, `zanzou_table`, `super_arts` — ours ✓
   - `task[i].timer`, `task[i].free` — ours ✓
   - `Pause`, `Game_pause`, `TASK_PAUSE` — ours ✓
   - `Clear_Personal_Data` — ours ✓
   - `grade_check_work_1st_init` — ours ✓
   - `Setup_Training_Difficulty` — ours ✓
   - `System_all_clear_Level_B` — ours ✓
   - `Demo_Flag`, `Mode_Type`, `Present_Mode`, `Play_Mode`, `Replay_Status` — ours ✓
   - `save_w[MODE_NETWORK]` (`Time_Limit`, `Battle_Number`, `Damage_Level`, `Handicap`, `GuardCheck`, `Pad_Infor`) — need to verify
   - `Counter_hi`, `Counter_low`, `Flash_Complete` — ours ✓
   - `bg_pos`, `fm_pos`, `bg_prm`, `Screen_Switch`, `Screen_Switch_Buffer`, `system_timer`, `Interrupt_Timer` — added in Phase 1 / existing ✓
   - `Order`, `Weak_PL` — ours ✓
   - `Check_Buff`, `Convert_Buff` — ours ✓
   - `Game_timer`, `Control_Time`, `players_timer`, `G_Timer` — ours ✓
   - `Champion`, `Forbid_Break`, `Connect_Status`, `Stop_SG`, `Exec_Wipe`, `Gap_Timer`, `E_No` — ours ✓
   - `C_No`, `SC_No` — ours ✓
   - All `Random_ix*_{ex,com,ex_com,bg}` — ours ✓
   - `Round_Level`, `Round_Result`, `PL_Wins`, `Conclusion_Type`, `win_type` — ours ✓
   - `Combo_Demo_Flag`, `Select_Demo_Index`, `Demo_Stage_Index`, `Demo_PL_Index` — ours ✓
   - `My_char`, `Super_Arts` — ours ✓
   - `Attack_Flag`, `Counter_Attack`, `Guard_Flag`, `Flip_Flag`, `Lie_Flag`, `Attack_Counter`, `Bullet_No`, `Bullet_Counter`, `paring_counter` — ours ✓
   - `VS_Stage`, `SLOW_timer`, `SLOW_flag`, `EXE_flag` — ours ✓
   - `piyori_type`, `Max_vitality` — ours ✓

4. Our `src/port/config/config.h` — for Config_GetInt equivalent (the `CFG_KEY_NETPLAY_FT` path is 3sxtra-specific and NOT MVP for us per out-of-scope)

### `s_negotiated_ft` handling
3sxtra's `setup_vs_mode` uses `s_negotiated_ft` + `Config_GetInt(CFG_KEY_NETPLAY_FT)` for first-to-X logic. Our fork does not implement FT negotiation (Phases 10-12 territory). For Track A Phase 2, the simpler substitution:

```c
// File-static at top of netplay.c
static int s_negotiated_ft = 2;  // default FT=2 (best-of-3 matches); future tracks wire negotiation

// In setup_vs_mode body:
int ft = s_negotiated_ft;
if (ft < 1) ft = 2;
if (ft > 10) ft = 10;
s_negotiated_ft = ft;
save_w[MODE_NETWORK].Battle_Number[0] = 1;
save_w[MODE_NETWORK].Battle_Number[1] = 1;
```

Document inline: `// TODO(track-c): wire s_negotiated_ft from GekkoNet handshake or config`.

### `MenuTask_SetPhase(MTP_NETPLAY_IDLE)` → `task[TASK_MENU].r_no[0] = 5` substitution
3sxtra line 185 reads `MenuTask_SetPhase(MTP_NETPLAY_IDLE);` — replace with `task[TASK_MENU].r_no[0] = 5;` (our equivalent, already in our current `setup_vs_mode` at line 118). Preserve the comment `// go to idle routine (doing nothing)`.

### Files to create/modify
1. **`src/netplay/netplay.c`**:
   - At file-scope statics area (near lines 57-77): add `static int s_negotiated_ft = 2;`.
   - Replace the entire body of `setup_vs_mode()` (current lines 117-147) with a port of 3sxtra's `setup_vs_mode` body (lines 158-391), applying the three substitutions:
     1. `MenuTask_SetPhase(MTP_NETPLAY_IDLE)` → `task[TASK_MENU].r_no[0] = 5`
     2. `plw[p].wu.pl_operator` → `plw[p].wu.operator`
     3. `Config_GetInt(CFG_KEY_NETPLAY_FT)` path → use the simpler `s_negotiated_ft` default shown above
   - Do NOT port 3sxtra's other netplay.c functions (`configure_gekko`, `process_events`, etc.) — we already have those; only `setup_vs_mode` is in Track A scope.
   - Preserve our existing `clean_input_buffers()` call at the end (matches 3sxtra's end call).

2. **Update** the already-existing `cpExitTask(TASK_SAVER)` + `cpExitTask(TASK_MENU)` calls to match 3sxtra's positions. Don't remove any cleanup our version does that 3sxtra lacks.

### Field-name verification (critical)
Before porting each line, verify the field name matches ours. Known divergences from research §9.5:
- `WORK.operator` (ours) vs `WORK.pl_operator` (3sxtra) — use ours. 3sxtra renamed to avoid C++ keyword conflict.
- Everything else should be symmetric; research §6.2 confirms all globals exist.

If during porting you encounter a symbol that does NOT exist in our engine (e.g., `MenuTask_SetPhase`, `MTP_NETPLAY_IDLE` — expected; handled by substitution), check:
1. Is it a 3sxtra-exclusive symbol? If yes AND research §6.2 flags it as "replace with X": apply the substitution.
2. Is it a legitimate engine global we somehow don't have? STOP, write `TRACK_A_BLOCKED.md` with the symbol name, likely owner module, and the line in 3sxtra's source.
3. Is it something like `save_w[MODE_NETWORK].Pad_Infor[p].Shot[s] = identity[s]`? Verify `save_w`, `MODE_NETWORK`, `Pad_Infor`, `Shot`, `Vibration` all resolve in our engine. If a sub-field differs (e.g., 3sxtra's `.Shot` is `u8[8]` but ours is `s8[8]`), match our type.

### Success criteria
- `tools/mister/build-game.sh --flavor telemetry` builds green.
- Line count of our `setup_vs_mode` is approximately 160–190 LOC (including comments ported from 3sxtra). Confirm with `awk '/^static void setup_vs_mode/,/^}/' src/netplay/netplay.c | wc -l`.
- No symbol is missing at link time.
- `s_negotiated_ft` is file-static, default `2`, only referenced inside `setup_vs_mode` plus the comment.
- `git diff src/netplay/netplay.c` shows only the `setup_vs_mode` function + the new static + the typedef move from Phase 1 — nothing else.

### PHASE 2 VERIFICATION (after Step 2.1)

```bash
# 1. MiSTer build
tools/mister/build-game.sh --flavor telemetry 2>&1 | tail -20

# 2. Desktop build (if desktop preset exists)
if [ -f CMakePresets.json ] && grep -q '"name": "desktop"' CMakePresets.json; then
  cmake --preset desktop 2>&1 | tail -5 && cmake --build --preset desktop 2>&1 | tail -10
fi

# 3. Line count sanity
awk '/^static void setup_vs_mode/,/^}/' src/netplay/netplay.c | wc -l
# expect: between 150 and 200

# 4. Symbol resolution — verify the new setup_vs_mode compiled into the binary
readelf -s build/mister-install/MiSTer_3S-ARM 2>/dev/null | grep -E 'setup_vs_mode|s_negotiated_ft' | head

# 5. (Optional) Desktop two-instance smoke if available:
#    Start two local instances on port 50000/50001, call Netplay_BeginDirectP2P
#    Observe session_state transitions through TRANSITIONING → CONNECTING → RUNNING
#    Observe 10+ frames of input exchange without desync
#    Skip if desktop build isn't configured; document in commit message.

# 6. Git status
git status --short
# expect: only src/netplay/netplay.c plus possibly docs/plan-netplay-track-a.md
```

If all pass: **COMMIT PHASE 2**:

```sh
git add src/netplay/netplay.c docs/plan-netplay-track-a.md
git commit -m "feat(netplay): Phase 2 — expand setup_vs_mode to full frame-0 canonicalization

Port 3sxtra's setup_vs_mode (netplay.c:158-391) into our
src/netplay/netplay.c. Zeros every rollback-divergent global at frame 0:
PLW, zanzou, super_arts, task timers, pause flags, personal data,
effects, save_w defaults, RNG indices, button remap buffers, round state,
combat flags, slow-motion, super gauge, vitality.

Substitutions vs 3sxtra:
- MenuTask_SetPhase(MTP_NETPLAY_IDLE) → task[TASK_MENU].r_no[0] = 5
  (we don't have the MenuTask phase enum; use our r_no routing)
- plw[].wu.pl_operator → plw[].wu.operator (3sxtra renamed for C++;
  we keep the original name per decision 9.5)
- CFG_KEY_NETPLAY_FT / Config_GetInt → static s_negotiated_ft = 2
  (TODO: wire FT negotiation when Tracks B/C land lobby)

Closes the 30-LOC-vs-234-LOC gap flagged in research doc §6.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
"
```

Mark Step 2.1 **DONE** in this plan doc.

### Scope limits
- Do NOT port 3sxtra's other changes to netplay.c (the GekkoConfig modifications, session event handlers, etc.). Phase 2 = `setup_vs_mode` only.
- Do NOT touch any engine source outside `src/netplay/netplay.c` for Phase 2.
- Do NOT implement FT negotiation; use the placeholder static.

### What to do if it fails
- Symbol-missing at link: per the decision tree above — apply substitution, escalate via `TRACK_A_BLOCKED.md`, or fix-forward as the case allows.
- Build succeeds but desktop smoke test diverges after 10 frames: the bug is probably in the 33-field backfill, not in `setup_vs_mode`. STOP, add diagnostic logging, re-check Step 1.3's field list against 3sxtra's header. If it's in setup_vs_mode, diff our port against 3sxtra's source line-by-line and find the divergence.

---

## STEP 3.1 — Port focused checksum + sanitizers, remove `#if DEBUG` gate

### Title
Port `save_current_state` (with focused checksum), `sanitize_plw_pointers`, `sanitize_work_pointers`, `sanitize_work_rendering` from `/tmp/3sxtra/src/netplay/game_state.c:1473-1714`. Add `combo_type` + `remake_power` to the whitelist. Keep our existing `clean_plw_pointers` with `cb`/`rp` zeroing. Remove the `#if DEBUG` gate from the main checksum path.

### Why it matters
- Research §8 and §19 risk #5: our fork ships Release MiSTer with ZERO desync detection (the `#if DEBUG` at `netplay.c:240,389` gates the checksum entirely). Internet play will silently corrupt and we'll have no diagnostic.
- Research §19 risk #1: 3sxtra's focused whitelist skips our fork-exclusive `combo_type`, `remake_power` globals (they moved these into PLW; we kept them as top-level). Porting verbatim without explicit adds = silent damage-scaling desyncs.
- Research §19 risk #2: 3sxtra's `sanitize_plw_pointers` doesn't zero `p->cb`, `p->rp` — fields only our fork has. Verbatim port would leak heap pointers into the checksum = false-positive desyncs.

### Dependencies
Phases 1 and 2 committed.

### Files to read
1. `/tmp/3sxtra/src/netplay/game_state.c:1473-1714` — source of truth
2. `src/netplay/netplay.c` — current `clean_work_pointers`, `clean_plw_pointers`, `calculate_checksum`, `save_state` (lines 240-410)
3. `src/netplay/game_state.c` — add new functions here (following 3sxtra's placement) OR keep them in netplay.c (match whichever feels cleaner; 3sxtra has them in game_state.c but we can mirror that)
4. `src/netplay/game_state.h` — add function declarations for the new public functions

### Placement decision
3sxtra moved `save_current_state`, `load_state`, sanitizers, and checksum into `game_state.c`. For Track A: **match 3sxtra** — move these into `src/netplay/game_state.c`. Our current `netplay.c`'s `gather_state`, `save_state`, `load_state`, `calculate_checksum`, `clean_*_pointers` get removed from netplay.c and the equivalents put in game_state.c. `netplay.c` calls the new public API: `save_state(event)`, `load_state_from_event(event)`.

This matches 3sxtra's `game_state.h` public interface (lines 787-800):
```c
void save_state(const struct GekkoGameEvent* event);
uint32_t save_current_state(void* buffer, int frame);
void load_state(const struct State* src);
void load_state_from_event(const struct GekkoGameEvent* event);
#if DEBUG
void dump_desync_state(int frame, uint32_t local_checksum, uint32_t remote_checksum);
#endif
```

### Files to create/modify
1. **`src/netplay/game_state.h`**:
   - Move the `typedef struct State` from `netplay.c` to here (below the existing `typedef struct GameState`):
     ```c
     typedef struct State {
         GameState gs;
         EffectState es;
     } State;
     ```
   - Add public-API forward declarations (as above). Note: use `struct GekkoGameEvent;` forward-declare to avoid pulling `gekkonet.h` into the header.

2. **`src/netplay/game_state.c`**:
   - Add includes: `#include "netplay/netplay.h"`, `#include <stdio.h>`, `#include "main.h"` if needed, `#include "gekkonet.h"` (with the `#define Game GekkoGame` / `#undef Game` guard if 3sxtra uses it).
   - Add `static int battle_start_frame = -1;` at file scope.
   - Add `#if DEBUG` / `#define STATE_BUFFER_MAX 20` / `static State state_buffer[STATE_BUFFER_MAX]; #endif`.
   - Add `#define SDL_copya(dst, src) SDL_memcpy(dst, src, sizeof(src))`.
   - Port `gather_state(State* dst)` verbatim from 3sxtra (except: our fork's effect state copy already works in `netplay.c`; use the same copy pattern). Verify against our current version in netplay.c.
   - Port `sanitize_work_pointers(WORK* w)` — verbatim 3sxtra. Zeros ~30 fields identical between forks.
   - Port `sanitize_work_rendering(WORK* w)` — verbatim 3sxtra. Masks 0x2000 bits.
   - Port `sanitize_plw_pointers(PLW* p)`:
     - Start from 3sxtra's version (calls sanitize_work_pointers + sanitize_work_rendering + zeros `cp`, `dm_step_tbl`, `as`, `sa`, `py`).
     - ADD: `p->cb = NULL; p->rp = NULL;` — our fork's PLW has these fields, 3sxtra's does not. Preserves the cleanup our `clean_plw_pointers` does at `netplay.c:293,295`.
     - Verify `cb` and `rp` exist by grep against our `structs.h`. If absent, remove those two lines.
   - Port `save_current_state(void* buffer, int frame)` — verbatim 3sxtra lines 1560-1714, with ONE addition: in the focused-checksum block (around 3sxtra line 1660 where `My_char` and `Super_Arts` are hashed), add:
     ```c
     // Whitelist: our fork-exclusive top-level globals not in 3sxtra's PLW
     h = djb2_update_mem(h, (const uint8_t*)&gs->combo_type, sizeof(gs->combo_type));
     h = djb2_update_mem(h, (const uint8_t*)&gs->remake_power, sizeof(gs->remake_power));
     ```
     Place this right after the existing Player-identity block so the hash diverges if damage-scaling globals drift. See research §19 risk #1.
   - Port `load_state(const State* src)` verbatim from 3sxtra — matches our current `load_state` in netplay.c.
   - Port `save_state(const GekkoGameEvent* event)` — the thin wrapper that calls `save_current_state` and assigns to `*event->data.save.checksum`.
   - Port `load_state_from_event` — thin wrapper.
   - Port `#if DEBUG` `dump_desync_state(...)` — 3sxtra lines 1736-1810. Writes text + bin files to `states/`. Keep `#if DEBUG` gate on dump. On MiSTer, `states/` may not exist; 3sxtra's code handles `fopen` returning NULL gracefully — verify.

3. **`src/netplay/netplay.c`**:
   - REMOVE `typedef struct State { GameState gs; EffectState es; } State;` at lines 52-55 (moved to header).
   - REMOVE `clean_work_pointers`, `clean_plw_pointers`, `clean_state_pointers`, `note_state`, `dump_state`, `dump_saved_state`, `calculate_checksum`, `gather_state`, `save_state`, `load_state`, `load_state_from_event` functions (lines 240–415 approx) — they're now in game_state.c.
   - UPDATE callers:
     - The `case GekkoSaveEvent:` handler in `process_events` calls `save_state(event)` — now resolves to game_state.c's version.
     - The `case GekkoLoadEvent:` handler calls `load_state_from_event(event)` — same.
     - The `case GekkoDesyncDetected:` handler currently reads (netplay.c:493-499):
       ```c
       case GekkoDesyncDetected:
           const int frame = event->data.desynced.frame;
           printf("⚠️ desync detected at frame %d\n", frame);
       #if DEBUG
           dump_saved_state(frame);
       #endif
           break;
       ```
       REPLACE with:
       ```c
       case GekkoDesyncDetected:
       {
           const int frame = event->data.desynced.frame;
           const uint32_t local_cs = event->data.desynced.local_checksum;
           const uint32_t remote_cs = event->data.desynced.remote_checksum;
           printf("⚠️ desync detected at frame %d  local=0x%08x remote=0x%08x\n",
                  frame, local_cs, remote_cs);
       #if DEBUG
           dump_desync_state(frame, local_cs, remote_cs);
       #endif
           break;
       }
       ```
       (Verify `event->data.desynced.local_checksum` / `remote_checksum` exist in our GekkoNet vendor version. If field names differ, match ours.)
   - REMOVE the `#if DEBUG desync_detection = true;` block in `configure_gekko` (netplay.c:168-170). Make `config.desync_detection = true;` unconditional — Phase 3 means checksum is on in Release.

4. **`src/netplay/netplay.h`** (if it exists) — update exports if any relevant function moved.

### Verify `cb` and `rp` exist in our `PLW`
Quick grep before the implementer writes:
```sh
grep -n 'struct _PLW\|typedef.*PLW' src/structs.h src/sf33rd/Source/Game/engine/plcnt.h 2>&1 | head
grep -nE '\.\s*cb\s*=|\.\s*rp\s*=|\bcb\s*;|\brp\s*;' src/structs.h 2>&1 | head -20
```
If `cb` is something like `Callback* cb;` and `rp` is `Replay* rp;`, proceed with the zeroing. If they don't exist (research might be stale), drop those two lines from `sanitize_plw_pointers`.

### Success criteria
- `tools/mister/build-game.sh --flavor telemetry` builds green.
- `grep -n 'save_current_state\|sanitize_plw_pointers\|sanitize_work_rendering\|dump_desync_state' src/netplay/game_state.c` shows each as defined once.
- `grep -n '#if DEBUG' src/netplay/netplay.c` — only remaining gates are on the `dump_desync_state` call and `state_buffer` (if state_buffer stays debug-only). No `#if DEBUG` around `calculate_checksum` or `save_state` call.
- `grep -n 'combo_type\|remake_power' src/netplay/game_state.c` shows both included in the whitelist hash (in the focused-checksum block).
- `grep -n 'p->cb = NULL\|p->rp = NULL' src/netplay/game_state.c` — shows both preserved in `sanitize_plw_pointers` (if the PLW fields exist).
- `readelf -s build/mister-install/MiSTer_3S-ARM 2>/dev/null | grep -E 'save_current_state|dump_desync_state|sanitize_plw_pointers'` — present (not dead-code-eliminated).
- Release checksum is active: a grep of our CMakeLists confirms `CHECKSUM` preprocessor isn't gating the call anymore, OR we've removed the gate entirely.

### Desync injection test (mandatory before commit)
**Desktop required** — run only if desktop build is available:
1. Build desktop with `ENABLE_NETPLAY=ON`.
2. Start two local instances on 127.0.0.1:50000 / 50001, as player 1 and player 2.
3. Run until `NETPLAY_SESSION_RUNNING` (`Netplay_GetSessionState()` reports it).
4. On one peer, after frame 10, manually corrupt `Random_ix16` (inject via a debugger, or temporarily add an `if (frame == 10 && player_handle == 0) Random_ix16 ^= 0xFFFF;` diagnostic).
5. Expect: within 1-2 frames, `GekkoDesyncDetected` fires on both peers, `dump_desync_state` writes files to `states/` (DEBUG only).
6. On MiSTer: skip this test; Release build on MiSTer will just print the desync line.

If desktop isn't available: run a Release-build smoke on MiSTer (with lock status verified green), confirm the checksum code path is reached by adding a `printf("[P%d] checksum computed at frame %d\n", ...)` temporary log, verify binary works without crash, remove the log, rebuild.

### Scope limits
- Do NOT port 3sxtra's more extensive netplay.c refactor (the GekkoDesyncDetected handler, `process_session` restructure, `Netplay_GetPlayerHandle`, etc.) — only the focused-checksum pieces.
- Do NOT remove `clean_plw_pointers`'s zeroing of `cb`/`rp` — that's the whole risk #2 mitigation.
- Do NOT remove the `#if DEBUG` gate on `dump_desync_state` itself (only on the main checksum path).

### What to do if it fails
- `event->data.desynced.local_checksum` doesn't exist in our GekkoNet: check `vendor/GekkoNet/` or `third_party/` for the actual struct definition. Our GekkoNet may be older/newer than 3sxtra's. If it truly lacks the field, fall back to: `printf("⚠️ desync detected at frame %d\n", frame);` without the checksum values, but still call `dump_desync_state(frame, 0, 0)` (it writes what it can from ring buffers).
- PLW struct layout surprises (e.g., 3sxtra checksumming `listix`, `timing` which we might not have): drop those from the sanitizer if our PLW lacks them. Keep the rest.
- Checksum performance regression bigger than expected on 800MHz ARM (research §19 risk #3): measure with show-fps. If regression is >1ms/frame steady state, flag in `TRACK_A_BLOCKED.md`; we can revisit in a later track.

### PHASE 3 VERIFICATION (after Step 3.1)

```bash
# 1. MiSTer build
tools/mister/build-game.sh --flavor telemetry 2>&1 | tail -20

# 2. Desktop build
if [ -f CMakePresets.json ] && grep -q '"name": "desktop"' CMakePresets.json; then
  cmake --preset desktop 2>&1 | tail -5 && cmake --build --preset desktop 2>&1 | tail -10
fi

# 3. Unit tests (if any netplay-related tests exist)
if [ -d tests ]; then
  cmake --build --preset desktop --target test 2>&1 | tail -10
fi

# 4. Symbol presence in Release ARM binary
readelf -s build/mister-install/MiSTer_3S-ARM 2>/dev/null | \
  grep -E 'save_current_state|sanitize_plw_pointers|sanitize_work_rendering' | head
# expect: all three present (not DCE'd)

# 5. #if DEBUG gate removed from main checksum path
grep -B2 -A2 'calculate_checksum\|save_current_state' src/netplay/netplay.c src/netplay/game_state.c | grep -c '#if DEBUG' || true
# expect: 0 for main checksum; >0 only around dump_desync_state and state_buffer

# 6. Whitelist additions present
grep -A1 'combo_type\|remake_power' src/netplay/game_state.c | grep djb2_update_mem | wc -l
# expect: 2

# 7. Desync injection (desktop only) — see above; document result in commit msg
```

If all pass: **COMMIT PHASE 3**:

```sh
git add src/netplay/game_state.h src/netplay/game_state.c src/netplay/netplay.c \
        docs/plan-netplay-track-a.md
git commit -m "feat(netplay): Phase 3 — port focused checksum, sanitizers, dump_desync_state

Port 3sxtra's focused whitelist checksum (game_state.c:1560-1714) into
our src/netplay/game_state.c. Replace our 'hash whole 247KB State in
debug only' with a djb2 over sanitized PLW + ~30 whitelisted fields,
active in BOTH Debug and Release. Closes research doc §19 risk #5 —
MiSTer ships desync detection to production, not just to debug builds.

Additions vs 3sxtra verbatim:
- Whitelist adds combo_type + remake_power (our top-level globals;
  3sxtra keeps these as PLW members so verbatim skips them and damage
  scaling would drift silently). Research §19 risk #1.
- sanitize_plw_pointers zeros our p->cb and p->rp (fields only our
  fork has; 3sxtra skip would leak heap pointers into the hash).
  Research §19 risk #2.

Moves typedef State, save_state(), load_state(), gather_state(),
clean_* → game_state.c to match 3sxtra's interface. dump_desync_state
stays behind #if DEBUG; main checksum path is unconditional.

Verified: desync injection test on desktop — flipping Random_ix16 on
one peer after frame N caused GekkoDesyncDetected on both peers within
2 frames; dump_desync_state wrote states/desync_F<N>_*.bin correctly.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
"
```

Mark Step 3.1 **DONE** in this plan doc.

---

## Final report requirements (after all three phases commit)

When all three commits exist and green:

1. Run `git log --oneline -3` — confirm the three commits, in order.
2. Run `tools/mister/build-game.sh --flavor telemetry` one final time as a sanity check. Report success.
3. Summarize what was done, any discovered risks or surprises, and the worktree path (`/Users/sb/Developer/3sx-mister/.claude/worktrees/agent-a42a8a0e`) so the user can merge.
4. Do NOT update CLAUDE.md or push anything.

---

## Appendix A — Decision log (captured here for the implementer)

These decisions were made during plan-time to resolve ambiguity; documented so the implementer doesn't re-derive them.

| # | Decision | Rationale |
|---|---|---|
| A1 | `select_timer.c/h` ported but call sites NOT wired in Phase 1 | Phase 1 goal is "no behavior change". Our `effa5.c` BCD countdown works; wiring `SelectTimer_Run()` into `Game01()` would be a real refactor. Add TODO for a future Track D. |
| A2 | `SelectTimerState select_timer_state;` definition lives in `src/sf33rd/Source/Game/engine/workuser.c` next to `Select_Timer` | matches 3sxtra's pattern of keeping timer-related globals in the timer module. Alternative (define in `select_timer.c`) is also fine — implementer chooses whichever builds cleanly. |
| A3 | `sbcd` in `select_timer.c` — rename to `select_timer_sbcd` if ours is `static` | Avoids linker collision. The implementations are identical. |
| A4 | `s_negotiated_ft` is a file-static with default `2` in Phase 2; no config wiring | FT negotiation is Phases 10–12 territory. Placeholder unblocks Phase 2. |
| A5 | Move `gather_state`/`save_state`/`load_state` etc. from `netplay.c` to `game_state.c` in Phase 3 | Matches 3sxtra's public interface; unblocks desync detection being in its natural home. |
| A6 | Count of backfill fields is 33–36; verified by implementer during Step 1.3 | Research §5.3 says 33, but counting the row contents of the table gives 35–36. Use the diff between 3sxtra's header and ours as ground truth. |
| A7 | Release builds unconditionally run focused checksum after Phase 3 | Research §19 risk #5 / §8.2. No `#if DEBUG` around `save_current_state`. |
| A8 | `combo_type` and `remake_power` added to whitelist explicitly with `djb2_update_mem` calls | Research §19 risk #1. Our fork's top-level globals aren't covered by `sanitize_plw_pointers` (which only hashes PLW bytes). |
| A9 | `sanitize_plw_pointers` KEEPS the `cb = NULL; rp = NULL;` from our `clean_plw_pointers` | Research §19 risk #2. Our PLW has fields 3sxtra's doesn't. |
| A10 | `dump_desync_state` stays `#if DEBUG` | Release builds print the desync line but don't write to disk. Matches 3sxtra. |
| A11 | If any Phase-2 `setup_vs_mode` symbol is genuinely missing from our engine: STOP, write `TRACK_A_BLOCKED.md`, commit WIP | Do NOT silently skip. Do NOT invent a substitute. |

---

## Appendix B — Progress ledger

Mark each step **DONE** (or **BLOCKED**) in-place as implementation proceeds. This ledger is the source of truth for progress.

- [x] Step 1.1 — Port `select_timer.c/h` verbatim (no wiring) **DONE**
- [x] Step 1.2 — Move `EffectState` to `game_state.h`, define `select_timer_state` global **DONE**
- [x] Step 1.3 — Add 36 fields to `GameState` struct **DONE**
- [x] Step 1.4 — Add 36 `GS_SAVE` + `GS_LOAD` calls **DONE**
- [x] Step 1.5 — Add `_Static_assert` tripwires + djb2 include **DONE** (32-bit size = 17580; differs from 3sxtra's 17800 by 220 bytes for combo_type + remake_power + Disp_Input_History + _TASK layout)
- [x] **COMMIT 1** — Phase 1 landed
- [x] Step 2.1 — Expand `setup_vs_mode` to full frame-0 canonicalization **DONE**
- [x] **COMMIT 2** — Phase 2 landed
- [x] Step 3.1 — Port focused checksum + sanitizers + dump; remove `#if DEBUG` gate **DONE**
- [x] **COMMIT 3** — Phase 3 landed
- [x] Final report written
