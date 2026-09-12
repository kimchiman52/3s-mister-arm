/*
 * hud_strip.h -- the per-player label row directly under the health bars.
 *
 * One row on the 384x224 canvas, y=48, shared by every feature that wants
 * to name or score a player next to their bar: the replay viewer's
 * "name [rank]" labels (src/replay/replay_overlay.c) and the versus win
 * tally (src/hud/versus_score.c). P1 is left-anchored at x=8, P2 is
 * right-anchored so its label ends at x=376; both are fitted to half the
 * 368 px row minus an 8 px gap so they cannot collide.
 *
 * Why y=48: the HUD cluster (vital_put y16..24, stun_put y24..32, the face
 * portraits and char-name plate y24..48, all sc_sub.c) ends at y=48, so
 * 8 px glyphs at y48..56 sit directly under the bar without overwriting any
 * HUD element. A jumping sprite or super-flash can transiently overlap the
 * row; that is a TV-only judgment.
 *
 * Every caller must gate on HudStrip_Visible(): it is the engine's own
 * HUD gate (`Disp_Cockpit && Game_pause != GAME_PAUSE_TRAINING`, game.c ->
 * Game2_1) plus the SCR font texture group existing -- SSPutStrProP renders
 * through ppgScrList, whose texture is only bound by Scrscreen_Init(), and
 * drawing before that is a null deref (reproduced on MiSTer and on the
 * dummy video driver; see ReplayOverlay_Draw's boot-order guard).
 */
#ifndef HUD_STRIP_H
#define HUD_STRIP_H

#include "types.h"

#include <stdbool.h>

#define HUD_STRIP_Y 48
#define HUD_STRIP_X_LEFT 8
#define HUD_STRIP_X_RIGHT 376
/* Per-label width cap: half of the 368 px row (LEFT..RIGHT) minus an 8 px
 * gap between the two labels. Anything wider is cut with "..." by
 * SSFitStrPro -- truncation, not wrapping, because the row sits directly
 * above the play-field and there is no second line to wrap into. */
#define HUD_STRIP_LABEL_MAX_W 176
/* atr 9 is the only palette bank proven to hold the ASCII-pro font CLUT
 * (every text caller uses it); colour comes from the vertex colour, which
 * modulates that CLUT. Priority 1 matches the Direct-P2P overlay: above the
 * HUD default (PrioBase[2]), below full-screen wipes (PrioBase[0]). */
#define HUD_STRIP_ATR 9
#define HUD_STRIP_PRIO 1
/* Bright arcade yellow (ARGB): distinct from the white HUD name plates and
 * from the plain-white status lines. */
#define HUD_STRIP_COL_YELLOW 0xFFF0E040u

/* True while the engine draws the top-HUD cluster AND the SCR font texture
 * group exists. Read-only over engine state. */
bool HudStrip_Visible(void);

/* Fit `label` to HUD_STRIP_LABEL_MAX_W in place (SSFitStrPro) and draw it on
 * the strip: player 0 left-anchored, player 1 right-anchored by its measured
 * width. Returns the drawn width in px. Caller gates on HudStrip_Visible(). */
s32 HudStrip_DrawLabel(int player, char* label, u32 vtxcol);

#endif /* HUD_STRIP_H */
