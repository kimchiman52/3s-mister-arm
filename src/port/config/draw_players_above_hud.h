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

// Session-only override, mirrors ArcadeBalance_ForceDisable (arcade/arcade_balance.c):
// this is a per-machine config that peers never negotiate, so a mismatch
// between two netplay peers' local settings would diverge which effect-table
// entries get created / which of scr_trans-vs-scr_calc runs, guaranteeing a
// rollback desync. Does not rewrite the on-disk config.
void DrawPlayersAboveHud_ForceDisable(void);

#endif
