#ifndef DRAW_PLAYERS_ABOVE_HUD_H
#define DRAW_PLAYERS_ABOVE_HUD_H

#include <stdbool.h>

// Gameplay-affecting readers of CFG_DRAW_PLAYERS_ABOVE_HUD (effect-table
// population and scr_trans/scr_calc selection — see eff06.c, bg150.c,
// sys_sub.c bg_layer_disabled()) must go through this accessor instead of
// reading the config directly. Draw-order-only readers (pure presentation,
// e.g. sc_sub.c HUD_Shift_Init()'s sprite priority values) are unaffected by
// netplay determinism and may keep reading Config_GetBool(CFG_DRAW_PLAYERS_ABOVE_HUD)
// directly.
bool DrawPlayersAboveHud_Enabled(void);

// Netplay-session suppression. This is a per-machine config that peers never
// negotiate, so a mismatch between two netplay peers' local settings would
// diverge which effect-table entries get created / which of
// scr_trans-vs-scr_calc runs, guaranteeing a rollback desync.
// Suppressed(true) at netplay arm (NetplayNav_Arm) and at the common
// session-setup chokepoint (setup_vs_mode); Suppressed(false) when the
// netplay session finishes tearing down (Netplay_Run EXITING -> IDLE), so —
// unlike the old never-cleared ForceDisable latch — LOCAL play after a
// netplay session gets the user's configured value back. Does not rewrite
// the on-disk config. Residual: a process whose netplay attempt armed but
// never produced a session keeps the suppression until relaunch (the MiSTer
// OSD flow re-execs per attempt).
void DrawPlayersAboveHud_SetNetplaySuppressed(bool suppressed);

#endif
