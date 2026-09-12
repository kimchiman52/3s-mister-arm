/*
 * versus_score.h -- the running win tally of the current PAIRING.
 *
 * "Pairing" is the lifetime: two players, from the moment a session with an
 * opponent begins until the opponent changes. It accumulates across every
 * match, survives a rematch and a trip through character select (the same
 * two people picking different characters are still the same set), and
 * resets only at the session boundary:
 *   - netplay: Netplay_BeginDirectP2P() (the sole entry into
 *     NETPLAY_SESSION_TRANSITIONING) and the EXITING -> IDLE teardown in
 *     Netplay_Run(), which every exit / disconnect / desync path reaches;
 *   - local versus: choosing VERSUS on the main menu (menu.c, the only
 *     writer of `Mode_Type = MODE_VERSUS`). Nothing else in the local flow
 *     is a pairing boundary: VS_Result's char-select branch and the rematch
 *     branch both stay inside the set.
 *
 * NOT engine state. The tally is deliberately outside GameState (no
 * GS_SAVE/GS_LOAD), so it is never rolled back and never restored, and
 * EXPECTED_GAME_STATE_SIZE / the MIST handshake's state_size are untouched.
 * That makes the increment the sharp edge: rollback can re-simulate the frame
 * on which a match ends, so a naive `score[winner]++` inside the simulation
 * double-counts. The increment is therefore driven by an edge detector over
 * rollback-saved engine state and applied only once that edge's frame can no
 * longer be rolled back -- the same confirmation bound task #145 derived for
 * the deferred menu exit (netplay.c -> menu_exit_request_confirmed):
 *
 *   observe(frame R):  match-end edge seen while simulating R -> latch R
 *   load(frame L):     L < R re-simulates R -> drop the latch; the corrected
 *                      timeline re-observes the edge (or does not)
 *   confirm(head):     head - R >= prediction window -> apply once, clear
 *
 * The edge is `G_No[0]==2 && G_No[1]==3` (game.c -> Game_Jmp_Tbl[3] =
 * Game03, the winner scene the match routes into from manage.c ->
 * Game_Manage_10th), read with Winner_id. G_No, Winner_id, Mode_Type and
 * Demo_Flag are all GS_SAVE'd, so both peers observe the same edge on the
 * same confirmed frame and reach the same number. Offline the window is 0:
 * the edge is applied on the frame it is seen.
 *
 * The label is a NAME SLOT plus the score. The name is empty today (no
 * lobby yet); adding one is VersusScore_SetName(), not a re-layout -- the
 * composition and the width measurement already run over the full string.
 */
#ifndef VERSUS_SCORE_H
#define VERSUS_SCORE_H

#include <stdbool.h>
#include <stddef.h>

#define VERSUS_SCORE_NAME_MAX 64

/* --- Session boundary ---------------------------------------------------- */

/* New pairing: both tallies to 0, names cleared, any pending edge dropped. */
void VersusScore_Reset(void);

int VersusScore_Get(int player);

/* Future lobby hook: the display name for `player` (0/1). NULL or "" clears
 * it. Nothing calls this yet. */
void VersusScore_SetName(int player, const char* name);
const char* VersusScore_GetName(int player);

/* --- Pure core (unit-tested; no engine reads) ---------------------------- */

/* Called once per SIMULATED frame, replay legs included, with the post-frame
 * state. `concluded` is "the engine is on the winner scene of a versus /
 * netplay match" and `winner` is who won it. A false->true edge latches a
 * pending increment stamped `sim_frame`; an already-pending edge is kept. */
void VersusScore_Observe(bool concluded, int winner, int sim_frame);

/* A rollback restored state AT `load_frame` and will re-advance from
 * load_frame + 1. Drops the pending edge iff load_frame < its frame (that
 * frame's simulation re-runs), and re-seeds the edge detector from the
 * restored state so the corrected timeline can re-observe the edge. */
void VersusScore_OnLoad(int load_frame, bool concluded_at_load);

/* Apply the pending edge iff head_frame - edge_frame >= pred_window (the
 * #145 bound: GekkoNet caps predicted input at pred_window consecutive
 * frames, so a misprediction at or before the edge frame cannot coexist with
 * a head that far past it). Returns true when an increment was applied. */
bool VersusScore_Confirm(int head_frame, int pred_window);

/* -1 when nothing is pending. */
int VersusScore_PendingFrame(void);

/* Compose the strip label for `player`: name and score when a name is set
 * ("NAME 2" for P1, "2 NAME" for P2, so the numbers sit inboard like a
 * scoreboard), the bare score otherwise. */
void VersusScore_ComposeLabel(const char* name, int score, int player, char* out, size_t out_sz);

/* --- Engine-bound -------------------------------------------------------- */

/* The match-end predicate over engine globals; exposed so the harness can
 * pin it against the real G_No / Mode_Type / Demo_Flag values. */
bool VersusScore_EngineMatchConcluded(void);

/* Observe/OnLoad over engine globals. Netplay calls these from
 * advance_game() and the GekkoLoadEvent handler. */
void VersusScore_ObserveEngine(int sim_frame);
void VersusScore_OnLoadEngine(int load_frame);

/* Offline tick (no rollback): observe this frame and confirm with window 0.
 * Called from game_step_0 after the engine tick when no netplay session is
 * live. Admits MODE_VERSUS only: Mode_Type is still MODE_NETWORK after a
 * clean session exit (nothing in netplay.c writes it back), and this tick
 * is by definition the no-session path, so the predicate's netplay arm is
 * masked here and only here. */
void VersusScore_TickLocal(void);

/* Draw both labels on the HUD strip. Self-gates: HUD up (HudStrip_Visible),
 * a real game (Demo_Flag), Mode_Type VERSUS or a RUNNING netplay session,
 * and never while the replay viewer owns the strip (ReplayPlayer_IsActive). */
void VersusScore_Draw(void);

#endif /* VERSUS_SCORE_H */
