/*
 * direct_p2p_overlay.c — Step 8 of docs/plan-stun-direct-p2p.md.
 *
 * Game-side status overlay for the Direct-P2P orchestrator. Renders
 * through the existing SSPutStrPro native-text path into the 384x224
 * game canvas, so the overlay shows while the main menu is visible —
 * no RmlUi dependency, no separate SDL_Renderer target.
 *
 * Called once per frame from NetplayScreen_Render()
 * (src/port/sdl/netplay_screen.c). State is read non-blockingly via
 * DirectP2P_GetState / DirectP2P_GetHostCode / DirectP2P_GetStatusText.
 *
 * No-op when DirectP2P_GetState() == DIRECT_P2P_IDLE so the overlay
 * vanishes cleanly after the worker hands off to netplay.c.
 */

#include "netplay/direct_p2p.h"

#ifdef ENABLE_NETPLAY

#include "sf33rd/Source/Game/ui/sc_sub.h"

// Overlay layout on the 384x224 canvas. SSPutStrProP(flag=1, width, ...)
// centers the string in [0, width] — passing the canvas width centers
// on the canvas. Uses the explicit-priority variant so the overlay
// stays above the title-sequence logo, attract-mode game frames, and
// "PRESS ANY BUTTON" which otherwise overdraw at lower priorities.
#define DP2P_OVL_CANVAS_W 384
#define DP2P_OVL_LINE1_Y 70
#define DP2P_OVL_LINE2_Y 100
#define DP2P_OVL_LINE3_Y 120
#define DP2P_OVL_ATR 9
#define DP2P_OVL_COL 0xFFFFFFFFu
/* Engine convention: PrioBase index is a Z depth — LOWER index = closer
 * to camera = drawn in front. PrioBase[0] is used by full-screen wipes
 * (sc_sub.c:1289), PrioBase[2] by HUD / title text / "PRESS ANY BUTTON"
 * (entry.c:162). Priority 1 sits above the title sequence and attract
 * frames while staying below transition wipes. */
#define DP2P_OVL_PRIO 1

// Mode label for line 1. Groups each orchestrator state into one of
// four user-facing categories so the player immediately knows whether
// we are hosting, joining, connected, or failing. The detailed message
// is on line 3 via DirectP2P_GetStatusText().
static const char* dp2p_overlay_mode_label(DirectP2PState s) {
    switch (s) {
    case DIRECT_P2P_UPNP_PROBE:
    case DIRECT_P2P_STUN_DISCOVER:
    case DIRECT_P2P_HOST_WAITING:
        return "HOSTING";

    case DIRECT_P2P_JOIN_PUNCHING:
        return "CONNECTING";

    case DIRECT_P2P_HANDOFF:
        return "CONNECTED";

    case DIRECT_P2P_FAILED_SYMMETRIC:
    case DIRECT_P2P_FAILED_STUN:
    case DIRECT_P2P_FAILED_PUNCH:
        return "ERROR";

    case DIRECT_P2P_IDLE:
    default:
        return "";
    }
}

void DirectP2P_DrawOverlay(void) {
    const DirectP2PState state = DirectP2P_GetState();
    if (state == DIRECT_P2P_IDLE) {
        return;
    }

    const char* label = dp2p_overlay_mode_label(state);
    if (label && label[0] != '\0') {
        SSPutStrProP(1, DP2P_OVL_CANVAS_W, DP2P_OVL_LINE1_Y, DP2P_OVL_ATR,
                     DP2P_OVL_COL, label, DP2P_OVL_PRIO);
    }

    // Line 2: room code, standalone and prominent. DirectP2P_GetHostCode()
    // returns the code only during HOST_WAITING and "" in every other state,
    // so this draws exactly when a shareable code exists.
    const char* code = DirectP2P_GetHostCode();
    if (code && code[0] != '\0') {
        SSPutStrProP(1, DP2P_OVL_CANVAS_W, DP2P_OVL_LINE2_Y, DP2P_OVL_ATR,
                     DP2P_OVL_COL, code, DP2P_OVL_PRIO);
    }

    // Line 3: per-state status detail (no room code — that's line 2).
    // Always non-NULL, possibly empty — skip the draw when empty so
    // SSPutStrProP doesn't try to center a zero-width string.
    const char* status = DirectP2P_GetStatusText();
    if (status && status[0] != '\0') {
        SSPutStrProP(1, DP2P_OVL_CANVAS_W, DP2P_OVL_LINE3_Y, DP2P_OVL_ATR,
                     DP2P_OVL_COL, status, DP2P_OVL_PRIO);
    }
}

#endif /* ENABLE_NETPLAY */
