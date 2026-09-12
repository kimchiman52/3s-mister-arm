# Versus win tally

A running count of matches won by each player, drawn under each health bar,
in local versus and in netplay. Code: `src/hud/versus_score.c` (the tally)
and `src/hud/hud_strip.c` (the label row it shares with the replay viewer).

Citations here are **symbol-first**: grep the symbol. Line numbers are not
maintained (see `AGENTS.md`). What the code *does* is asserted by
`test_netplay_units.c` -> `unit_versus_score_lifetime`; this page records
only the decisions, and what no test can hold.

## Decisions

### The tally belongs to the pairing, not to the match or the characters

It starts at 0 when a session with an opponent begins, accumulates across
every match, survives a rematch and a trip through character select (the
same two people picking different characters are the same set), and resets
only when the opponent changes. The alternatives -- per match (that is
`PL_Wins`, the round count, already on screen as the win marks) and per
character pair -- were rejected because neither answers the question a set
asks, "who is up".

### Where the boundaries are

- **Netplay:** `netplay.c` -> `Netplay_BeginDirectP2P` (the only writer of
  `NETPLAY_SESSION_TRANSITIONING`, so every session starts there) and the
  `NETPLAY_SESSION_EXITING` arm of `Netplay_Run` (every exit, disconnect and
  desync lands there before `IDLE`). The post-match char-select and rematch
  branches (`menu.c` -> `VS_Result_Rematch_Select`) never pass either point,
  which is what makes "survives char select" true by construction rather
  than by a special case.
- **Local versus:** the main menu's VERSUS entry (`menu.c` -> the
  `Mode_Type = MODE_VERSUS` store, its sole writer). Chosen over the exits
  because there is exactly one entry and several exits (VS_Result's exit,
  the soft reset, the title return), and `Mode_Type` is deliberately *not*
  cleared on the way out (`menu.c` VS_Result case 6 comments "leave
  Mode_Type be"), so no observer of the mode can see a versus -> menu ->
  versus round trip. The consequence to know about: the number is *held*,
  not zeroed, between leaving versus and re-entering it. It is invisible in
  that window (the draw gate below), so nothing shows it.

### The tally is not engine state

It lives in file statics, outside `GameState`, so it is never `GS_SAVE`'d,
never restored by a rollback, and `EXPECTED_GAME_STATE_SIZE` (and with it
the MIST handshake's `state_size`, hence `MIST_PROTO_VER`) is untouched.
Its lifetime is exactly the session's, which the session layer already
tracks and tears down in the right places; putting it inside the rolled-back
simulation would have bought nothing and cost a protocol bump.

### The increment is confirmed, not simulated

Rollback can re-simulate the frame a match ends on, so `score[winner]++`
anywhere inside the simulation double-counts. Instead an edge detector runs
over rollback-saved engine state on every simulated frame, replay legs
included (`netplay.c` -> `advance_game`), latching the frame on which
`G_No[0]==2 && G_No[1]==3` (game.c -> `Game_Jmp_Tbl[3]`, `Game03`, the
winner scene `manage.c` -> `Game_Manage_10th` routes a decided match into)
first becomes true. A `GekkoLoadEvent` to frame L drops the latch iff
L < that frame (L's own simulation is not re-run) and re-seeds the detector
from the restored state; `run_netplay` applies the latch once
`head - frame >= input_prediction_window`. That bound is task #145's,
derived from GekkoNet @ 7be848c (`netplay.c` ->
`menu_exit_request_confirmed`): predicted input is capped at
`input_prediction_window` consecutive frames, so a misprediction at or
before the edge frame cannot coexist with a head that far past it.

Both peers reach the same number because every input to the detector --
`G_No`, `Winner_id`, `Mode_Type`, `Demo_Flag` -- is `GS_SAVE`'d, so the
confirmed timeline is identical on both sides and the edge frame is the same.
A tally that differed between peers would be a desync-class bug; the
harness pins the single-count under re-simulation of the end frame, a later
corrected end, and a load that does not reach the edge.

**The single-count is the bound's property, not the code's.** `Confirm`
clears the pending frame but leaves the edge detector's seed alone, so a
load below R that arrived *after* the confirm would re-seed the detector,
the corrected timeline would re-enter the winner scene, and the same match
would count twice. That load cannot arrive while the GekkoNet bound holds
(a misprediction older than the window is impossible by construction), and
no defensive latch was added for it on purpose: a guard that swallowed the
second count would also swallow the only evidence of a regression in the
bound. `test_netplay_units.c` -> `unit_versus_score_post_confirm_load` pins
today's double count for exactly that sequence (at R-3 and at R-1, where
only `OnLoad`'s re-seed can expose the edge), so a change there is a red
line to argue over rather than a silent absorption. Measured by mutation:
`Confirm`'s reset and `OnLoad`'s drop rule were already held by
`unit_versus_score_lifetime`; an `OnLoad` that skips the re-seed when
nothing is pending, and a confirmed-frame guard that refuses to re-latch,
leave every other test green and fail only this one. Re-derive the bound
before changing those numbers.

Offline the window is 0: `main.c` -> `game_step_0` calls
`VersusScore_TickLocal` after the engine tick and the edge applies on the
frame it is seen.

### It shares the strip with the replay viewer and never draws over it

The replay viewer already owned the y=48 row for its "name [rank]" labels.
Rather than a second divergent copy, the row's geometry, width cap and gate
moved to `src/hud/hud_strip.c`, and both features draw through
`HudStrip_DrawLabel`. The tally's draw is gated off whenever
`ReplayPlayer_IsActive()`, so while a `.3sr` plays (which runs the console
VERSUS flow, `replay_player.c`) only the viewer's labels appear; its
observer is gated the same way, so replay results never enter the tally.

### The label is a name slot plus the score

A lobby with player names is planned and the name will live in this strip.
The element is composed as `"NAME 2"` (P1) / `"2 NAME"` (P2) -- numbers
inboard, like a scoreboard -- through `VersusScore_ComposeLabel`, fitted to
`HUD_STRIP_LABEL_MAX_W` by the same measured-width path the replay names
use. Today every name is empty and the bare number draws; wiring the lobby
is `VersusScore_SetName`, not a re-layout. Names clear with the pairing.

### Draw gate

HUD up (`HudStrip_Visible`: the engine's own
`Disp_Cockpit && Game_pause != GAME_PAUSE_TRAINING`, plus the SCR font
texture group existing), `Demo_Flag != 0`, and `Mode_Type == MODE_VERSUS` or
a `RUNNING` netplay session. `Demo_Flag` is load-bearing: the attract loop
(`game.c` -> `Next_Demo_Loop`) fights with the HUD up and leaves `Mode_Type`
wherever the last mode left it, so without it a stale versus number would
draw over the attract demo.

In a netplay session the draw rides `sdl_app.c`'s overlay pass, after
`NetplayScreen_Render`, so a held frame (exhausted prediction window)
still carries it; offline it draws from `game_step_0` beside
`ReplayOverlay_Draw`. The two never fire in the same frame: the session-live
branches in `sdl_app.c` and the offline branch in `game_step_0` are
mutually exclusive on `Netplay_GetSessionState()`.

## Not covered, on purpose

- No persistence to disk or across launches.
- No names wired (the slot exists; nothing fills it).
- The harness cannot drive `Netplay_Run`'s EXITING arm,
  `Netplay_BeginDirectP2P`, or the main-menu VERSUS case without a live
  session or the menu task, so the *placement* of the three
  `VersusScore_Reset` calls is unpinned; the core, the engine predicate and
  the lifetime rules over the engine-bound path are pinned.
- Not yet run on the MiSTer. The y=48 placement is the replay viewer's,
  already seen on a TV; the tally's own first-draw log line
  (`versus-score: HUD strip drawn`) is the SSH-verifiable evidence.
