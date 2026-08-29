#ifndef NETPLAY_NETPLAY_NAV_H
#define NETPLAY_NETPLAY_NAV_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * netplay_nav: small state machine that drives the cold-launched game
 * through the natural console-mode menu path (Title -> Mode Select ->
 * Versus) by injecting SWK_START presses on the appropriate frames,
 * then calls Netplay_BeginDirectP2P() once character-select has been
 * reached via the real code path.
 *
 * Why: the previous cold-launch handoff jumped G_No[0]=2 / G_No[1]=12
 * via setup_vs_mode() without running Game0_2 cases 3-5 or the menu
 * transitions, so texture slot 2 / BGM / effect pool priming that
 * happen along the real menu path never fired and char-select drew
 * only its background layer.
 *
 * Entry points:
 *   NetplayNav_Arm      — called once from set_netplay_params() when
 *                         a netplay session is pending. Idempotent.
 *   NetplayNav_Tick     — called once per frame from game_step_0(),
 *                         BEFORE p1sw_buff is latched into p1sw_0,
 *                         so injected Start presses land on the same
 *                         frame.
 *   NetplayNav_IsActive — true while the state machine is driving;
 *                         false before Arm and after Netplay_Begin
 *                         has fired.
 *   NetplayNav_Reset    — rare: drop back to idle (e.g. session
 *                         teardown before it finished driving).
 */
void NetplayNav_Arm(void);
void NetplayNav_Tick(void);
bool NetplayNav_IsActive(void);
void NetplayNav_Reset(void);

/*
 * Task #76: the NAV_WAIT_ORCHESTRATOR backstop, in frames, for a given
 * orchestrator worst case in milliseconds. Adds nav's fixed scheduling
 * margin and converts at the nav tick rate.
 *
 * This used to be a flat `150 * 60` constant, which meant a wedged
 * orchestrator left the player on a static "Connecting..." overlay for
 * 150 s — read as a hang, not a timeout. It is now derived from
 * DirectP2P_OrchWorstCaseMs(); see netplay_nav.c for the full rationale
 * and why a smaller flat constant is not a safe substitute.
 *
 * Exported (rather than static) so the enforced deadline can be checked
 * directly. Pure function of its argument — no global state.
 */
int NetplayNav_OrchTimeoutFrames(int orch_worst_case_ms);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_NETPLAY_NAV_H */
