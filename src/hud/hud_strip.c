/* hud_strip.c -- see hud_strip.h. */

#include "hud/hud_strip.h"

#include "sf33rd/Source/Common/PPGWork.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"

bool HudStrip_Visible(void) {
    /* Gate on the HUD being up, NOT on the round being live: Disp_Cockpit is
     * set with the health bars (manage.c -> Game_Manage_2_4), while
     * Allow_a_battle_f only goes true once the round-start banner finishes.
     * Gating on the latter held the replay names back through the whole
     * "FIGHT!" intro, which is precisely when a viewer looks for who is
     * playing. Disp_Cockpit alone already excludes menus, char-select and
     * the result screen, because it is 0 there. */
    return Disp_Cockpit != 0 && Game_pause != GAME_PAUSE_TRAINING && ppgScrList.tex != NULL;
}

s32 HudStrip_DrawLabel(int player, char* label, u32 vtxcol) {
    const s32 w = SSFitStrPro(label, HUD_STRIP_LABEL_MAX_W);
    s32 x;

    if (player == 0) {
        x = HUD_STRIP_X_LEFT;
    } else {
        /* Right-anchor: subtract the real glyph width from the right edge.
         * The clamp cannot fire while the width is capped below the edge,
         * but it costs nothing and keeps the u16 x safe. */
        x = HUD_STRIP_X_RIGHT - w;
        if (x < 0) {
            x = 0;
        }
    }

    SSPutStrProP(0, (u16)x, HUD_STRIP_Y, HUD_STRIP_ATR, vtxcol, label, HUD_STRIP_PRIO);
    return w;
}
