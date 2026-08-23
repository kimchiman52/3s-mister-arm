#include "port/config/draw_players_above_hud.h"
#include "port/config/config.h"

static bool force_disabled = false;

bool DrawPlayersAboveHud_Enabled(void) {
    return !force_disabled && Config_GetBool(CFG_DRAW_PLAYERS_ABOVE_HUD);
}

void DrawPlayersAboveHud_ForceDisable(void) {
    force_disabled = true;
}
