# Training-mode SELECT reset

Pressing **SELECT** during training snaps both characters back to the
round-start position — no black wipe, no BGM restart, no round transition.

Implemented in `src/sf33rd/Source/Game/menu/menu.c`
(`Tr_Reset_Check` / `Tr_Reset_Apply` / `Tr_Reset_Read_Input` /
`Tr_Reset_Release_L8` / `Tr_Reset_Finish_Teardown`), hooked at the top of
`Wait_Pause_in_Tr` ahead of `Control_Player_Tr`.

Citations here are **symbol-first**: grep the symbol. Any line number is a
timestamp, not an address, and is not maintained (see `AGENTS.md`).

## What ships today (increment 1)

| Input | Result |
|---|---|
| SELECT, no direction | Reset to centre, original sides |
| SELECT + ↓ (including ↓↙ / ↓↘) | Same — reset to centre |
| SELECT + ↑ / ← / → | **Nothing.** Increment 2. |
| START + SELECT | Unchanged: soft reset (`Check_Reset_IO`) |

Both characters land at `centre − 88` (P1) and `centre + 88` (P2), where
`centre` is `get_center_position()` — 512 on most stages, 464 on stages 0
and 19.

Down is tested as a **bit**, not an exact word, so down-back and down-forward
work. Those are the ordinary resting stick positions in training; an exact
match would swallow the reset for most of what a player actually holds.

Up, left and right deliberately do nothing rather than resolving to centre —
answering them with the centre arrangement would teach the wrong mapping
before increment 2's presets exist.

### Not yet implemented (increment 2)

The side-swap and the two corner presets. They need a post-`Player_move`
position override, a facing recompute from the new relative positions, and a
direct `bg_w.bgw[1]` camera write for the corners. The preset latch
(`Tr_Reset_Preset`) already exists but only ever holds `TR_RESET_CENTRE`, so
the apply path does not branch on it yet.

## When it will not fire

Gated in `Tr_Reset_Check`. All of these must hold:

- `Is_Training_Mode(Mode_Type)` — normal or parry training. Excludes
  `MODE_NETWORK`, so the feature is netplay-unreachable by construction.
- `task_ptr->r_no[1] < 2` — not while the pause menu or the
  controller-disconnected screen is up.
- `Allow_a_battle_f != 0` — the round is live. This closes at K.O. with zero
  frames of slack: `Game2_1` runs `Player_control` (which sets
  `Conclusion_Flag`) before `Game_Management` (where `Game_Manage_3rd` clears
  the flag) in the same frame, and the menu task reads it the frame after.
- `Extra_Break == 0` — not during a super-art screen flash, which
  `effect_77_move` raises for 64 frames.
- `(Game_pause & 0x7F) == 0` — not during a round message. **Masked, not
  compared to zero:** `cpLoopTask` (`src/main.c`) ORs in bit 7 under
  `#if defined(DEBUG)` whenever `sysSLOW` is active, and DEBUG is the flavour
  used for on-device testing. A whole-byte comparison would make the feature
  silently dead in exactly the build you would test with.
- `Play_Mode == PLAY_MODE_NORMAL` — blocked in both RECORD and REPLAY. A
  reset is not captured in the 12-bit recording, so a take spanning one would
  replay against different positions than it was recorded at.
- START held on **either** pad disqualifies the frame.

SELECT on the dummy's pad is inert unless the dummy is set to HUMAN —
`Game_Manage_1st` clears `wu_operator` on the dummy in training. That matches
the existing pause path.

## How it works

The reset drives the engine's own start-position path rather than inventing
one. Clearing `routine_no` makes `Player_move` dispatch to `player_mv_0000`,
which re-runs the round-start clears (vitality, hit stop, throw flags, stun,
ukemi, super-art state, and the per-round S.A.GAUGE training option) and hands
off to `player_mv_1000` → `plmv_1010` → `plmv_1020`. Training's appear type is
`APPEAR_TYPE_NON_ANIMATED`, so the step is 88 and `plmv_1020` already writes
exactly the centre preset with the correct `rl_flag` per side. **No position
override is needed for increment 1.**

It takes three frames: N sets up and runs `player_mv_0000`; N+1 runs
`player_mv_1000` and the position is written; N+2 `pli_1000` fires and
`player_mv_4000` accepts input.

### Why `pcon_rno` is written

`check_lever_data` (`engine/pls00.c`) is the sole entry point for pad input
into a player, and is itself gated on `routine_no[0] == 4`. The reset chain
runs 0 → 1 → 3 and stops there; `player_mv_3000` is `Player_normal` and
nothing else, and never calls it. The only writers of `routine_no[0] = 4` are
`pli_1000` (`engine/plcnt.c`) and `plcnt_b_init` case 1 (`engine/plcnt2.c`),
both reachable only while `pcon_rno[0] == 0` — and a live round sits at
`pcon_rno[0] == 1`.

So a reset that stops at `routine_no` leaves **both pads and the dummy CPU
permanently inert** until the player exits to the training menu. The
`pcon_rno` write is what hands them back.

`pcon_rno[1] = 2` rather than 0 is deliberate. `appear_initalize[]` is
`{ init_app_10000, init_app_10000, init_app_20000, init_app_30000 }` and
`APPEAR_TYPE_NON_ANIMATED` is 0, so training dispatches to `init_app_10000`.
Its case 0 is `pli_0000` → `SDL_zeroa(plw)` + `setup_base_and_other_data`,
the full round-boot re-init, which would clobber `wu.target_adrs` (dereferenced
every frame by `set_rl_waza`) and re-run the training-only E3/E4 works.
Entering at case 2 skips it and still ends at `pli_1000`.

This is not invented: `Game_Manage_2_3`'s training branch already does exactly
this, down to the `pcon_rno[1] = 2` constant.

## Four defects handled, with their evidence

### 1. `Suicide[0]` must be cleared by this code

The effect teardown needs a one-frame `Suicide[0]` pulse so list-5 effects take
their own exit paths — notably the super-art shadow, which clears the global
`SA_shadow_on` only from `effect_J4_move`'s exit branch. `erase_extra_plef_work`
never walks list 5, so without the pulse the stage stays dark for good after a
reset during a super.

But **nothing on an in-round path clears the flag.** `All_Clear_Suicide` is
called only from `Game_Manage_1st`, `Menu_Init`, `Sel_PL_Cont_1st`, `Win_1st`
and `After_Bonus_1st` — all screen/phase entry. `Game2_5` gets away with a bare
set because the round-restart flow later reaches `Game_Manage_2_0` /
`Game_Manage_2_2`, which write `Suicide[0] = 0` explicitly. Leaving it set makes
every effect that reads it suicide on creation.

### 2. The pulse destroys `effect_84`, and it is not transient

`effect_84` is the round-message controller singleton (list 4, id 84).
`erase_extra_plef_work`'s list-4 filters are `0x81` / `0x25` / `0xAC`, so it
never frees it — the pulse is the only thing that does, and it is created only
by `Game_Manage_2_2` and `Game_Manage_12_0`.

It is the only writer of `request_message = 0` outside those screen-boundary
phases, and three phases spin on that clear (`Game_Manage_5_2`,
`Game_Manage_7_5`, `Game_Manage_12_3`). `effect_56_init`, which draws the
message, is called only from `effect_84_move`. Lose the singleton and no
K.O. / TIME UP message renders for the rest of the session, and the
`Game_pause = 1` it raises over the K.O. window stops happening at all.

`Tr_Reset_Finish_Teardown` rebuilds it the frame after the pulse, mirroring
`Game_Manage_2_2`'s clear-then-init ordering and its retry-on-pool-exhaustion
convention (`effect_84_init` returns -1 when `pull_effect_work(4)` fails).

**This is reachable in training, not theoretical.** `Demo_Flag` is nonzero
during training — despite the name, it means "a real credited session is
running" — so `settle_check` does issue `request_center_message(0)` on a
training K.O.

### 3. Makoto's Tanden Renki palette (pre-existing engine bug)

`erase_extra_plef_work` frees the whole of list 6 with
`effect_work_list_init(6, -1)`, which releases each work through
`push_effect_work` **without running its move function**. `effect_L8` (Makoto's
Tanden Renki) latches two ColorRAM rows into its own `frw` slot and restores
them only from `effect_L8_move`'s case 1 — which therefore never runs — and
`push_effect_work` then `SDL_zeroa`s the slot holding the only saved copy.

Makoto stays tinted for the rest of the session, and it does not self-heal: the
next Tanden Renki latches the already-tinted rows as its "old" colours. The
`Suicide[0]` pulse does not help — `effect_L8_move` reads no `Suicide` entry at
all.

`Tr_Reset_Release_L8` walks list 6 and drives L8's own exit path before the
free. Fixed on this side rather than in `erase_extra_plef_work`, which is
shared with `Game2_5`.

**Not introduced here.** `Game2_5` tears down identically, so a stock round
restart during Tanden Renki leaks the same palette. A training reset only makes
it reachable at any moment instead of only at a round boundary.

### 4. Twelve's invisibility strands three brightness fields

`effect_L0_move` case 1 guards its restore with
`dead_f == 0 && Suicide[0] == 0` — inverted relative to the pulse — so the
restore is skipped and the work freed. `disp_flag` is covered by the explicit
write, and `my_col_mode` / `my_col_code` by `set_char_base_data`, but
`my_bright_type`, `my_bright_level` and `my_clear_level` are covered by nothing.

Zeroed in the per-player loop rather than by fixing `effl0.c`'s guard: those
fields live in `plw`, which is `GS_SAVE`'d, so changing the guard would be a
simulation change on the shared arcade/netplay path for a training-only feature.

## Smaller things worth knowing

- **Velocity, acceleration and sub-pixel position are zeroed.** `player_mv_0000`
  never touches `wu.mvxy`, so a reset mid-dash would otherwise carry the
  momentum straight back out of the start position — the same defect
  `input_script_apply_teleport` has. `plmv_1020` writes only `.disp.pos`, so
  `.disp.low` is cleared separately or the "same" reset lands a fraction of a
  pixel differently every time.
- **`Appear_end` is cleared.** `player_mv_1000` increments it on the
  NON_ANIMATED path and nothing in a live round takes it back down, so it would
  climb 2 at a time toward signed overflow (UB).
- **Accepted loss:** `effect_work_list_init(0, 0)` frees every id-0 hitbox
  overlay, and `setup_any_data` re-creates them only for the two players, so an
  already-airborne projectile loses its box for the rest of its life. Visible
  only with hitbox display on.
- **Accepted ordering:** SELECT on frame N then START on frame N+1 fires a reset
  before the soft-reset chord arms. Damage is bounded — `TASK_RESET` runs before
  `TASK_MENU`, `Reset_Move`'s `effect_work_init` wipes the pool, and
  `Menu_Task`'s `nowSoftReset()` early return stops this code running at all
  until recovery.

## Netplay safety

Netplay-invisible by construction: gated on `Is_Training_Mode`, which excludes
`MODE_NETWORK`, and `Wait_Pause_in_Tr` is unreachable online regardless.

The preset latch is a `menu.c` file-static, **not** a `GameState` field, on
purpose. `MIST_STATE_VER` is `sizeof(GameState)` (`src/netplay/mist_handshake.c`),
so a field there would force an `EXPECTED_GAME_STATE_SIZE` re-pin *and* a
`MIST_PROTO_VER` bump for a mode netplay cannot reach.

`sizeof(GameState)` is unchanged at 17772 and `MIST_PROTO_VER` remains 4; the
`_Static_assert` in `game_state.c` proves the first at build time. Everything
the feature writes (`plw`, `bg_w`, `Suicide`, `pcon_rno`) is already
rollback-tracked.

## Arcade balance

Works under `ArcadeBalance_IsEnabled()`, which is the device default. Nothing
new sits inside a `!ArcadeBalance_IsEnabled()` branch, and no new consumer of
that predicate was added. `plmv_1020`'s position and `rl_flag` writes sit
outside its arcade-gated tail, and `erase_extra_plef_work`, `setup_any_data`
and `set_base_data_tiny` are ungated.

## Corrections to the design doc

The design doc is `~/Desktop/3sx-training-reset-position-2026-08-30.md`
(not in-tree). Six of its claims are wrong or incomplete; anyone building
increment 2 from it should read these first.

1. **§11's recipe omits `pcon_rno` entirely.** This is the most important gap —
   following §11 as written produces a reset after which nobody can move.
2. **§7 cites the wrong template.** It presents `init_app_20000` and says "uses
   no `pli_0000()` and no `SDL_zeroa(plw)`; don't add them". Training dispatches
   to `init_app_10000`, which *does* reach `pli_0000`. The instruction described
   a path training never takes.
3. **§4.2's ordering claim is wrong**, and the ordering does not matter.
   `Game2_5` sets `Suicide[0]` and calls `erase_extra_plef_work` in the same
   frame with no move pass between. Ordering is irrelevant because
   `erase_extra_plef_work` never walks list 5.
4. **§4.2 never says to clear `Suicide[0]`**, and nothing else will. See defect 1.
5. **§12 item 4 (effect L8 ColorRAM latch) is real** — it was listed as "same
   hazard class, not separately verified". See defect 3.
6. **§12 item 2 (`wu.target_adrs` across the teardown) resolves safe.**
   `setup_any_data` goes through `set_base_data_tiny`, which does not write
   `target_adrs`; only the full `set_base_data` does, and the pointer targets the
   file-scope `plw[2]` array so it cannot dangle.

## Verification status

Built for ARM (`tools/mister/build-game.sh --flavor telemetry`), clean.
`sizeof(GameState)` and `MIST_PROTO_VER` confirmed unchanged.

**Deliberately skipped**, per the design doc's §13: the frame-data suite (no
simulation-logic change — the reset writes only training-gated state) and the
desync/rollback gates (training-only, netplay-unreachable, no `GameState`
change).

### Still outstanding — needs a human with a pad, under arcade balance

- [ ] **Acceptance criterion:** after a reset, both pads control their
      characters again and the dummy resumes its configured behaviour. This is
      simulation-traced only, and it is the defect that made the first draft
      unusable.
- [ ] Does the three-frame gap read as a clean snap or a visible hitch? Is the
      one frame rendered at the old position (the position lands on N+1)
      noticeable?
- [ ] Reset during a super art — no lingering screen darkening (`SA_shadow_on`).
- [ ] Reset during Makoto's Tanden Renki, **then a second Tanden Renki** — the
      palette bug does not self-heal, so one reset is not a sufficient test.
- [ ] Reset during Twelve's invisibility fade — brightness restored.
- [ ] Reset mid-dash — no residual momentum.
- [ ] K.O. after a reset — the round message still renders (defect 2).
- [ ] Both characters visible, combo counter cleared, camera correct.
- [ ] Reset mid-throw and mid-super-freeze.
- [ ] Down-back / down-forward reset does not feel accidental during blockstring
      practice.
- [ ] START + SELECT soft reset still works from inside training.

### Unverified in simulation

- Whether skipping `plcnt_move` for the three reset frames — so `time_over_check`,
  `settle_check` and the training `dm_nodeathattack` write do not run — has any
  observable effect. Argued inert (no player can act, attacks were torn down,
  training forces `Counter_hi = -1`) but not proven exhaustively.
- Whether a training round can reach the `Game_Manage_5_2` / `7_5` / `12_3`
  stalls at all. The normal K.O. path cannot — `Game_Manage_6th` case 1 and
  `Game_Manage_7_3` both take the `Is_Training_Mode` branch to
  `C_No[0] = 12; End_Training = 1`. `Conclusion_Type` 1 (double K.O.) and 2
  (time over) were not traced. The structural defect is verified regardless.
