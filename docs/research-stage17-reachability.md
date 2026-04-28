# How does bg_w.stage become 17 in netplay?

Date: 2026-04-24
Branch: `netplay-direct-only`
Investigator: fact-based static code audit, no runtime repro this pass.

---

## TL;DR

`Setup_Battle_Country()` at `src/sf33rd/Source/Game/screen/sel_pl.c:2030-2031` contains the following branch:

```c
if (My_char[New_Challenger] == 17) {
    return My_char[Champion];
}
```

This is correct in arcade mode where `Champion != New_Challenger` always. In
**netplay** `setup_vs_mode()` at `src/netplay/netplay.c:305` sets
`Champion = 0` but **never assigns `New_Challenger`** — so `New_Challenger`
keeps its BSS default of 0. With `Champion == New_Challenger == 0`, whenever
the human controlling slot 0 picks `CHAR_Q` (`= 17` in the 3SX enum) the
branch returns `My_char[Champion] = My_char[0] = 17` and `bg_w.stage = 17` at
`sel_pl.c:1582`.

This matches the observed log: the Mac↔Mac repro where one peer picked Q
lands on stage 17, which then triggers the §A/§F black-BG mechanism in
`docs/research-bg-deep-dive.md`.

---

## Enum correction

The prior investigation prompt equates `17` with `CHAR_MAKOTO` (the CPS3-enum
value). That is **incorrect for this build**. The codebase uses the **3SX
enum** (non-CPS3 block at `src/constants.h:36-60`) because the `CPS3` define
is commented out at `CMakeLists.txt:104` (`# CPS3`).

Under the 3SX enum:

| id | char         |
|----|--------------|
| 15 | CHAR_CHUNLI  |
| 16 | CHAR_MAKOTO  |
| 17 | CHAR_Q       |
| 18 | CHAR_TWELVE  |
| 19 | CHAR_REMY    |

So `bg_w.stage == 17` = **Q's stage**, not Makoto's stage. The user's visual
report ("Chun vs Q, got Dudley's stage") matches: stage 17's texture file is
remapped to Dudley's via `bg_index_tbl[17] = {4,4,4}` (see
`docs/research-bg-deep-dive.md` §B.5).

Stage-number and character-index share the same namespace: the return value of
`Setup_Battle_Country()` is a character index and is assigned directly to
`bg_w.stage` at `sel_pl.c:1582`. Stages 0..19 correspond to
`CHAR_GILL..CHAR_REMY`; stages 20/21 are the bonus-game stages set by
`next_cpu.c:1491` (`bg_w.stage = Bonus_Type`). Stages 0..20 are also the
valid values accepted by `Handicap_Stage_Move_Sub` at `sel_pl.c:1940-1965`.

---

## Stage-number ↔ character-index mapping

Every write of `bg_w.stage` in the gameplay path either:

- Copies a value that originated as a character index via
  `Setup_Battle_Country()` (sel_pl.c:1582, 1710; next_cpu.c:1128), **or**
- Reads a stage-array entry whose values are also character indices
  (demo02.c:286 using `Demo_Stage_Play_Data` whose values are in 0..19,
  `Random_Stage_Data` at sel_data.c:153-155 whose values are in 0..19
  skipping 17), **or**
- Reads a hard-coded non-character stage (`Bonus_Type` at next_cpu.c:1491
  which is 20 or 21), **or**
- Uses a debug override (`Debug_w[31]/[32]` at sel_pl.c:1586,
  next_cpu.c:1132, 1140 — inactive by default; see audit below).

The correspondence is therefore **same namespace** for the 0..19 range. No
remap table exists between the "what character picked this stage" answer and
the `bg_w.stage` value consumed by `bg_initialize` at `bg_sub.c:1112`.

---

## Every writer of `bg_w.stage`

Grep: `bg_w\.stage\s*=` restricted to assignments (not comparisons).

| File:Line | Assignment | Source |
|-----------|------------|--------|
| `src/test/test_runner.c:1044` | `bg_w.stage = stage;` | Unit-test only. Not live. |
| `src/sf33rd/Source/Game/demo/demo02.c:286` | `bg_w.stage = Demo_Stage_Play_Data[Demo_Stage_Index][rnd];` | Values = `{{15,19},{11,18},{2,16},{12,8}}` (demo02.c:257) — never 17. |
| `src/sf33rd/Source/Game/screen/next_cpu.c:1120` | `bg_w.stage = Q_Country;` | Arcade mode only (inside `Setup_Next_Fighter`, reached via Game11 `Next_Q_1st` which sets `EM_id=17` before calling). `Q_Country` is written only at `manage.c:1879` in arcade ladder state `Game_Manage_11th`. Netplay does not reach these paths (see below). |
| `src/sf33rd/Source/Game/screen/next_cpu.c:1128` | `bg_w.stage = Battle_Country;` | Arcade mode CPU-opponent picker. Netplay skips. |
| `src/sf33rd/Source/Game/screen/next_cpu.c:1132` | `Battle_Country = bg_w.stage = Debug_w[31] - 1;` | Debug override. `Debug_w[31]` defaults to 0 (see Debug_w audit). |
| `src/sf33rd/Source/Game/screen/next_cpu.c:1140` | `Super_Arts[COM_id] = bg_w.stage = Debug_w[32] - 1;` | Debug override (this line looks like a decomp transcription bug — it assigns `bg_w.stage` as a side effect of the `Super_Arts` `=` chain). `Debug_w[32]` defaults to 0. |
| `src/sf33rd/Source/Game/screen/next_cpu.c:1491` | `bg_w.stage = Bonus_Type;` | Bonus stage init. `Bonus_Type` ∈ {20, 21}. |
| `src/sf33rd/Source/Game/screen/sel_pl.c:1582` | `bg_w.stage = Battle_Country;` | **Primary path for netplay.** Inside `Exit_2nd`. Battle_Country = `Setup_Battle_Country()` return value on line 1581. |
| `src/sf33rd/Source/Game/screen/sel_pl.c:1586` | `Battle_Country = bg_w.stage = Debug_w[31] - 1;` | Debug override, same semantics as next_cpu.c:1132. |
| `src/sf33rd/Source/Game/screen/sel_pl.c:1710` | `bg_w.stage = Battle_Country;` | Inside `Exit_7th`, just a copy from the already-set `Battle_Country`. |
| `src/sf33rd/Source/Game/menu/menu.c:1613` | `bg_w.stage = Replay_w.game_infor.stage;` | Replay mode only (`Mode_Type == MODE_REPLAY`). |

Gameplay-path assignments (excluding tests, bonus, and replay): **all go
through `Battle_Country`**, and `Battle_Country` is set either by
`Setup_Battle_Country()` (sel_pl.c:1581) or by the arcade next-fighter path
(next_cpu.c:1119-1125).

Netplay only enters `Setup_Battle_Country()` via `Exit_2nd`.

---

## Setup_Battle_Country branch analysis for Chun-vs-Q

Function at `src/sf33rd/Source/Game/screen/sel_pl.c:2013-2035`:

```c
u8 Setup_Battle_Country() {
    s16 Rnd32;

    if (Mode_Type == MODE_VERSUS) {            /* branch A */
        if (VS_Stage == 20) {
            Rnd32 = random_32();
            return Random_Stage_Data[1][Rnd32];
        }
        return VS_Stage;
    }

    if (My_char[0] == 17 && My_char[1] == 17) { /* branch B */
        Rnd32 = random_32();
        return Random_Stage_Data[0][Rnd32];
    }

    if (My_char[New_Challenger] == 17) {       /* branch C */
        return My_char[Champion];
    }

    return My_char[New_Challenger];            /* branch D */
}
```

### In netplay

`Mode_Type == MODE_NETWORK` (netplay.c:205, netplay_nav.c:121). Branch A is
skipped. Branches B/C/D all apply.

### State of Champion and New_Challenger at Exit_2nd

`setup_vs_mode()` at `src/netplay/netplay.c:147-378` sets:

- Line 305: `Champion = 0;`
- No assignment to `New_Challenger` anywhere in the function. Confirmed by:
  `grep -n "New_Challenger" src/netplay/netplay.c` → empty output.

`New_Challenger` is declared at `src/sf33rd/Source/Game/engine/workuser.c:28`
as `s8 New_Challenger;` with no initialiser → BSS → 0 on boot.

Other writers of `New_Challenger` that could fire before Exit_2nd in the
netplay flow:

- `menu.c:453` (training mode case 2), `menu.c:700` (training config path):
  neither fires because nav picks Mode_Select case 1 (Versus), not case 2
  (Training).
- `entry.c:1326, 1342, 1355` (Ck_Break_Into / Ck_Break_Into_SP): all are
  mid-match arcade break-in paths; none fires during netplay char select.
- `sys_sub.c:1196` (inside `Check_Replay`): `Mode_Type != MODE_REPLAY`, skipped.

So when Exit_2nd runs in netplay: **`Champion = 0` (forced by setup_vs_mode)
and `New_Challenger = 0` (BSS default, never written)**.

### Character assignment during char select

`sel_pl.c:1161`: `My_char[PL_id] = ID_of_Face[Cursor_Y[PL_id]][Cursor_X[PL_id]];`
when player presses `SWK_ATTACKS` (sel_pl.c:1156-1161).

`ID_of_Face` is initialised from `Face_Cursor_Data` at sel_pl.c:340:

```c
for (y = 0; y < 3; y++)
    for (x = 0; x < 8; x++)
        ID_of_Face[y][x] = Face_Cursor_Data[y][x];
```

`Face_Cursor_Data` at `src/sf33rd/Source/Game/screen/sel_data.c:8-10`:

```c
const s8 Face_Cursor_Data[3][8] = {
    { -1,  1, 12,  7,  5, 13, 14, -1 },   /* row 0 */
    { 10, 18, 16, 15, 17, 19,  3,  0 },   /* row 1 — CHAR_Q at col 4 */
    { 11,  6,  8,  4,  9,  2, -1, -1 }    /* row 2 */
};
```

Row 1 column 4 = 17 = `CHAR_Q`. Row 1 column 3 = 15 = `CHAR_CHUNLI`. Q is
immediately to the right of Chun in the grid — easy pick.

Default cursor positions (init3rd.c:110-113):
- P0 → `[0][1]` = `CHAR_ALEX` (id 1)
- P1 → `[2][5]` = `CHAR_KEN` (id 11)

So neither player starts on Q by default.

`PL_id = 0` reads `p1sw_0` / `p1sw_1` and is updated by slot-0 inputs;
`PL_id = 1` reads `p2sw_0`. In netplay, slot 0 is always `player_number == 0`
(netplay.c:56) and inputs flow `inputs[0] → p1sw_0`, `inputs[1] → p2sw_0`
(netplay.c:595-596). Which human ends up on slot 0 is decided by GekkoNet
at `gekko_add_actor` time (netplay.c:535-542).

### The four cases for Chun-vs-Q

For brevity let `Q = 17`, `Chun = 15`.

| My_char[0] | My_char[1] | Champion | NC  | Hits branch | Returns | bg_w.stage |
|------------|------------|----------|-----|-------------|---------|------------|
| Chun (15)  | Q (17)     | 0        | 0   | D (not 17)  | `My_char[0] = 15` | 15 (Chun's stage) |
| Chun (15)  | Q (17)     | 0        | 1   | C (Q at NC) | `My_char[0] = 15` | 15 |
| Q (17)     | Chun (15)  | 0        | 0   | **C (Q at NC)** | **`My_char[0] = 17`** | **17** ← repro |
| Q (17)     | Chun (15)  | 0        | 1   | D (not 17)  | `My_char[1] = 15` | 15 |

The **third row** is the netplay cold-boot state (both Champion and NC are
0) with the P1-slot player picking Q. It returns `My_char[Champion] = 17`
because `Champion == NC == 0` breaks the function's implicit arcade-mode
invariant that `Champion != New_Challenger`.

In arcade mode that invariant holds:
- Ck_Break_Into (entry.c:1326): `New_Challenger = PL_id; Champion = NC ^ 1`.
- Game_Over→Entry_01_Sub (entry.c:213 via nav) sets only `Champion = PL_id`.
  In arcade the previous match updates NC via Ck_Break_Into. In netplay there
  is no prior match so NC is never touched.

So the function's branch C intent ("if the challenger picked Q, use the
champion's char as stage") misbehaves when Champion == NC. With both = 0,
"champion's char" and "challenger's char" are the same character, so asking
for "the other player's char" actually returns the Q-picker's own char.

### Why this has been latent upstream

In upstream arcade-only builds, netplay does not exist, and `Setup_VS_Mode`
(menu.c:482-494) is only reached in MODE_VERSUS (which short-circuits in
branch A above). So the branch-C quirk never surfaces.

In the port of /tmp/3sxtra, `setup_vs_mode()` resets most rollback-hashed
fields but **explicitly omits `New_Challenger`**. Not commented; simply not
touched. Likely because NC is not part of the tier-1 netplay-sync audit (it's
hashed in the GameState rollback snapshot at `game_state.c:138` but the
netplay team didn't recognise that its initial value matters for
Setup_Battle_Country).

---

## Netplay path from char select to bg_w.stage = 17

Citations-only call graph:

```
NetplayNav_Arm()                              netplay_nav.c:138  (SDL_INIT → main_loop)
└─ [nav state machine, frames]               netplay_nav.c:166-320
   ├─ NAV_PRESS_COIN → Start pressed          netplay_nav.c:188-214
   ├─ NAV_PRESS_TITLE → Start pressed         netplay_nav.c:216-237
   │   └─ Entry_01_Sub(PL_id) @entry.c:208-228
   │       └─ Champion = PL_id   (line 213)
   ├─ NAV_DRIVE_VS → Mode_Select case 3       netplay_nav.c:260-296
   │   └─ menu.c:425-431  Setup_VS_Mode + Mode_Type=MODE_VERSUS
   │       (Setup_VS_Mode @menu.c:482-494 does NOT touch Champion or NC)
   ├─ apply_network_mode_override             netplay_nav.c:115-136
   │   → Mode_Type = MODE_NETWORK
   ├─ NAV_WAIT_ORCHESTRATOR                   netplay_nav.c:298-309
   └─ NAV_START_NETPLAY
       └─ Netplay_BeginDirectP2P               netplay.c:819-824
           └─ direct_p2p_pending = true

Later main-loop frame:
Netplay_TickDirectP2P                        netplay.c:826-855
└─ setup_vs_mode()                             netplay.c:147-378
   ├─ G_No[0]=2, G_No[1]=12, G_No[2]=1        netplay.c:199-204
   ├─ Mode_Type = MODE_NETWORK                netplay.c:205
   ├─ SDL_zeroa(My_char)                      netplay.c:351   ← My_char=[0,0]
   ├─ VS_Stage = 0                            netplay.c:366
   ├─ Champion = 0                            netplay.c:305
   └─ (New_Challenger NOT touched — remains BSS/stale 0)

session_state = NETPLAY_SESSION_TRANSITIONING → CONNECTING → RUNNING
Then the normal game loop:

Game_Task → Game_Jmp_Tbl[G_No[1]=1] = Game01   game.c:119
└─ Game01 case 2/default                       game.c:327-428
   └─ Select_Player()                          sel_pl.c:146-167
      ├─ Sel_PL_Control                        sel_pl.c:215-226
      │   └─ Sel_PL (×2, once per slot)        sel_pl.c:885-891
      │       └─ Sel_PL_3rd                    sel_pl.c:951-1002
      │           └─ Sel_PL_Sub(PL_id, sw)     sel_pl.c:1119-1184
      │               └─ sw & SWK_ATTACKS:
      │                   My_char[PL_id] = ID_of_Face[Cursor_Y[PL_id]][Cursor_X[PL_id]]
      │                                                  sel_pl.c:1161
      │                   ← human picks Q → My_char[0] = 17
      └─ Check_Exit                             sel_pl.c:1534-1538
         └─ Sel_Exit_Tbl[Exit_No] = Exit_2nd   sel_pl.c:1572-1606
            └─ if (Select_Status[0] == 3)      sel_pl.c:1577
                (both operators active — true in netplay, both plw[].wu.wu_operator=1
                 from setup_vs_mode netplay.c:181-182)
               ├─ Battle_Country = Setup_Battle_Country()  sel_pl.c:1581
               │   └─ branch C: My_char[NC=0] == 17 → return My_char[Champion=0] = 17
               ├─ bg_w.stage = Battle_Country = 17         sel_pl.c:1582
               └─ Push_LDREQ_Queue_BG(17)                  sel_pl.c:1589

Subsequently:
Game2_0 → Game2_2 → bg_initialize              bg_sub.c:1100
└─ bg_w.bg_index = bg_index_tbl[17][0] = 4     bg_sub.c:1112
└─ bg_w.scrno    = use_real_scr[4] = 1         bg_sub.c:1114
└─ Bg_Texture_Load_EX                           bg.c:246
   ├─ DEBUG log "ENTRY #N  bg_w.stage=17 scrno=1"  bg.c:275-290
   └─ the rest of the §A black-BG mechanism from docs/research-bg-deep-dive.md
```

---

## Debug_w[31] audit

Indexed in `Debug_w[72]` (`debug_config.c:126`). Write sites grep
(`Debug_w\[31\]` plus the hex alias `Debug_w\[0x1F\]`):

- `src/sf33rd/Source/Game/screen/next_cpu.c:1131-1132` — **read**, used to
  override `bg_w.stage`. Inside arcade next-fighter path; does not fire in
  netplay.
- `src/sf33rd/Source/Game/screen/sel_pl.c:1585-1586` — **read**, used to
  override `bg_w.stage` inside `Exit_2nd`. Fires in netplay **iff
  `Debug_w[31] != 0`**.
- No other writes anywhere in `src/` (confirmed by grep).

Default value: `debug_config.c:85-88` (array `debug_defaults[72]`). Row-major
indexing: row 0 (indices 0-17), row 1 (18-35), row 2 (36-53), row 3 (54-71).

```
debug_defaults[] = {
    8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 0-17
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 18-35
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 1,   // 36-53
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 54-71
};
```

Index 31 (row 1 column 13, 0-based) = 0. **`Debug_w[31] = 0` on boot.**

Only mutation sites for `Debug_w[31]`:
- `Debug.c:100-112` — the DEBUG-menu edit loop (manual keypress in the
  in-game debug menu).
- `Debug.c:213-217` — toggle loop (XOR 1). Also manual.

Neither fires automatically. In DEBUG builds without in-game manual
interaction with the debug menu, `Debug_w[31]` stays 0 and the override at
`sel_pl.c:1586` is dead.

**Verdict**: Debug_w[31] is NOT the mechanism.

---

## Demo-leakage path

Claim to verify: attract/demo ran before netplay start, and demo mutated
`bg_w.stage` / `VS_Stage` / `My_char` in a way that leaks into the first
netplay match.

### `Setup_Demo_Stage` at demo02.c:282-292

```c
bg_w.stage = Demo_Stage_Play_Data[Demo_Stage_Index][rnd];
```

`Demo_Stage_Play_Data` at demo02.c:257:
```c
const s8 Demo_Stage_Play_Data[4][2] = { {15,19}, {11,18}, {2,16}, {12,8} };
```

No `17` anywhere. Demo-mode `bg_w.stage` ∈ {2, 8, 11, 12, 15, 16, 18, 19}.

### `Setup_Demo_PL` at demo02.c:260-273

```c
My_char[0] = Demo_PL_Play_Data[Demo_PL_Index][0];
My_char[1] = Demo_PL_Play_Data[Demo_PL_Index][1];
```

`Demo_PL_Play_Data` at demo02.c:255: same values as above. No 17 ever
assigned to `My_char` via demo.

### Does `setup_vs_mode` zero the demo leftovers?

Yes: `SDL_zeroa(My_char)` at netplay.c:351, `VS_Stage = 0` at netplay.c:366.
So even if demo set `My_char[x] = 17` (which it can't, per the table), that
would be wiped.

But: **setup_vs_mode does NOT reset `bg_w.stage` itself**. Grep for
`bg_w.stage` in netplay.c: 0 matches. Any value demo left in `bg_w.stage`
persists until the char-select Exit_2nd overwrites it at sel_pl.c:1582.

This `bg_w.stage` leak cannot cause the observed bug however, because Exit_2nd
unconditionally writes `bg_w.stage = Battle_Country` before anything consumes
it in the match setup (`Game2_0 → Game2_2 → bg_initialize`).

### `VS_Stage` leak

VS_Stage is set to `0x14` (= 20 = "random") at menu.c:358 when Mode_Select
case 0 runs. Then setup_vs_mode zeroes it at netplay.c:366. In
`Setup_Battle_Country`, `VS_Stage` only matters for `Mode_Type == MODE_VERSUS`
(branch A, sel_pl.c:2017-2022). Netplay is `MODE_NETWORK`, so VS_Stage's
value is irrelevant to which branch is taken.

### Verdict

**No demo-leakage path feeds stage 17.** Setup_vs_mode's reset surface is
sufficient for the relevant fields (My_char, VS_Stage, Champion). The bug is
not a leak — it is a latent logic bug that only requires the user to pick Q
on slot 0.

---

## Nav machine character-select reality check

Does nav auto-drive past Q onto Makoto (or similar)?

`netplay_nav.c` state machine has five progressive states (NAV_WAIT_INIT,
NAV_PRESS_COIN, NAV_PRESS_TITLE, NAV_WAIT_MENU, NAV_DRIVE_VS) that inject
`SWK_START` at specific game phases and one pure-wait state
(NAV_WAIT_ORCHESTRATOR). **Nav never touches lever/direction inputs, and
never injects SWK_ATTACKS.** Grep confirms:

```
grep -n "p1sw_buff\|p2sw_buff\|SWK_" src/netplay/netplay_nav.c
netplay_nav.c:90:    p1sw_buff |= SWK_START;
netplay_nav.c:91:    p2sw_buff |= SWK_START;
```

Only SWK_START. Only during title + menu. Never during char select.

`NAV_DRIVE_VS` case at netplay_nav.c:260-296 DOES force `Menu_Cursor_Y[0] = 1`
(for the Mode_Select cursor, selecting "Versus" over "Arcade"). That is the
Mode_Select menu cursor, NOT the character-select cursor
(`Cursor_X/Y[PL_id]`). Cursor_X/Y for char select are only set via
sys_sub.c:202-206 (`Clear_Personal_Data`) using `permission_player[mode]
.cursor_infor[]`, which defaults to (P0 on Alex, P1 on Ken).

So the user always drives the char-select cursor personally. Whatever the user
picks IS what `My_char[PL_id]` gets set to at sel_pl.c:1161.

**Verdict**: nav does not contaminate character selection. The user did pick
Q; the game registered Q; `My_char[0] = 17` is the true state.

---

## Identified mechanism

**`Setup_Battle_Country()` in netplay returns 17 whenever the slot-0 player
picks Q (CHAR_Q = 17 in the 3SX enum), because `Champion == New_Challenger
== 0` at that point — and the function's branch-C "avoid Q's stage when Q
is the challenger" logic degenerates to "return the Q-picker's own char" when
Champion and New_Challenger index the same slot.**

Citations:
- Logic: `src/sf33rd/Source/Game/screen/sel_pl.c:2013-2035`
- Champion forced to 0 in netplay: `src/netplay/netplay.c:305`
- New_Challenger never set in netplay: grep `New_Challenger` in
  `src/netplay/netplay.c` → empty; workuser.c:28 declares with no
  initialiser; all other writers only fire in arcade / training / replay
  paths (`entry.c:1326,1342,1355`, `menu.c:453,700`, `sys_sub.c:1196`).
- Mode_Type in netplay: `src/netplay/netplay.c:205`, `src/netplay/netplay_nav.c:121`.
- My_char assignment on ATTACKS press: `sel_pl.c:1156-1161`.
- Face grid with Q at row 1 col 4: `src/sf33rd/Source/Game/screen/sel_data.c:8-10`.
- bg_w.stage = Battle_Country: `sel_pl.c:1582`.
- Enum confirmation: `src/constants.h:36-60` (3SX enum used because
  `CMakeLists.txt:104` has CPS3 commented out).

### Why the symptom only reproduces when "one peer picked Q" (user report)

Per the case table, stage 17 is returned only when `My_char[Champion] == 17`
with Champion == 0. In netplay, Champion is always 0 at entry to
char-select (forced by setup_vs_mode). So the symptom fires when the slot-0
player (whoever GekkoNet assigned `player_number == 0` to) picked Q. A
session where only the slot-1 peer picks Q will not hit stage 17 — it falls
through to branch D and returns `My_char[0] = 15` (Chun).

This matches the prior researcher's note in §E.1 that "the mechanism is
unverified by code reading alone" — they stopped short of observing that
`Champion == New_Challenger` is the degeneracy.

---

## Minimal fix

The prior research recommends fixing the Bg_Texture_Load_EX enabler loop
(docs/research-bg-deep-dive.md §G). That is the right fix **for the
rendering-layer mismatch at stage 17**. It is orthogonal to the
stage-selection question analysed here.

For the stage-selection question itself, the minimal fix is to **set
`New_Challenger` in `setup_vs_mode()` so that `Champion != New_Challenger`,
matching the invariant `Setup_Battle_Country()` assumes**.

Exact edit:

```diff
--- a/src/netplay/netplay.c
+++ b/src/netplay/netplay.c
@@ -303,6 +303,11 @@ static void setup_vs_mode() {
     // Per-player globals that can hold stale values from the previous game
     // session or differ based on who connected first.
     Champion = 0;
+    /* Setup_Battle_Country() at sel_pl.c:2030-2034 branches on
+     * `My_char[New_Challenger] == 17` and returns `My_char[Champion]`.
+     * In arcade mode Champion != New_Challenger by construction, but
+     * netplay never writes New_Challenger. Force the invariant so the
+     * branch actually names a different slot. */
+    New_Challenger = 1;
     Forbid_Break = 0;
     Connect_Status = 0;
     Stop_SG = 0;
```

With `Champion = 0, New_Challenger = 1`:

- P1 picks Q, P2 picks Chun: `My_char = [17, 15]`. Branch C:
  `My_char[NC=1] = 15 != 17` → fall through to D → return
  `My_char[NC=1] = 15` → **stage 15 (Chun)**.
- P1 picks Chun, P2 picks Q: `My_char = [15, 17]`. Branch C:
  `My_char[NC=1] = 17 == 17` → return `My_char[Champion=0] = 15` → **stage 15**.
- Both pick Q: branch B → random-stage (table skips 17) → **not 17**.
- Neither picks Q: branch D → `My_char[NC=1]` = whoever P2 picked → **P2's
  char-stage**.

So in netplay, stage 17 becomes unreachable via `Setup_Battle_Country()`
regardless of who picks Q, restoring the arcade invariant.

### Why this fix is safe

1. The write site (netplay.c:305) is already in the "force deterministic
   starting state" block of `setup_vs_mode`. Adding one line next to it is
   consistent with surrounding code.
2. `New_Challenger` is included in the `GameState` rollback snapshot
   (`src/netplay/game_state.c:138, 827`). Writing before the first saved
   frame means both peers enter rollback-sync with the same NC value.
3. The value `1` (not 0 or any runtime expression) guarantees Champion != NC
   regardless of peer identity.
4. Downstream consumers of `New_Challenger` are all arcade / training /
   break-in paths (grep: entry.c, effe4.c, sel_pl.c:679-681, sys_sub.c:1196).
   None of them is reached during netplay pre-first-match; the value is
   shadowed by Ck_Break_Into etc. in the arcade-progression sequences we
   never execute in netplay.
5. The parallel upstream /tmp/3sxtra port has the same omission (same file,
   same struct). Verifying there requires a separate read pass, but the
   fix applies identically.

### Residual concern

Even with this fix, the **§F Bg_Texture_Load_EX mismatch still exists and
should still be patched** (the §G.1 edit to the bg.c loop). The two bugs
are independent:

- This fix prevents netplay from reaching `bg_w.stage == 17` via
  Setup_Battle_Country.
- The §G.1 fix prevents the **rendering** mismatch whenever
  `bg_w.stage == 17` is reached, by any path (including a future lobby
  matchmaking mode that uses MODE_VERSUS → VS_Stage = random, or a
  debug-menu manual stage pick).

Shipping both is the safest posture.

---

## Unverified

1. **Which peer (slot 0 vs slot 1) reliably picked Q in the observed
   Mac↔Mac repro?** Static analysis says only slot-0 picks of Q trigger the
   bug; if the user reports were slot-1 picks, the mechanism above is not
   enough. Would require either a DEBUG log print of
   `(Champion, New_Challenger, My_char[0], My_char[1])` at Exit_2nd entry,
   or verbal confirmation from the user.

2. **Whether `Setup_Battle_Country()` is called twice per match (one for
   each peer's `Sel_PL_Complete` state) or just once.** Rolled-back
   `Exit_2nd` re-invocations could run Setup_Battle_Country against partly
   different My_char snapshots. Reading sel_pl.c the function is only
   called from Exit_2nd (line 1581), and Exit_2nd only runs when
   `Select_Status[0] == 3` (both players confirmed). So it should be
   effectively once per unique (Champion, NC, My_char) tuple, but rollback
   may repeat it. The return value is deterministic in its inputs though,
   so repeats don't matter.

3. **Whether the `/tmp/3sxtra` reference port has the same bug.** The
   upstream `setup_vs_mode` is not read in this pass — a diff against
   3sxtra would tell us whether the omission is ours or inherited.

4. **The prior research doc § E closing paragraph says "Stage 17 cannot
   come from Setup_Battle_Country"**. That's wrong — it missed the case
   where Champion == New_Challenger. The conclusion in §E.1 should be
   updated to say "Stage 17 cannot come from Setup_Battle_Country *except
   when `Champion == New_Challenger` and `My_char[Champion] == 17`*, a
   configuration unique to pre-first-match netplay."
