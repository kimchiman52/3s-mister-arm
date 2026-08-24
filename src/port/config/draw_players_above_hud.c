#include "port/config/draw_players_above_hud.h"
#include "port/config/config.h"

static bool netplay_suppressed = false;

bool DrawPlayersAboveHud_Enabled(void) {
    return !netplay_suppressed && Config_GetBool(CFG_DRAW_PLAYERS_ABOVE_HUD);
}

void DrawPlayersAboveHud_SetNetplaySuppressed(bool suppressed) {
    netplay_suppressed = suppressed;
}
