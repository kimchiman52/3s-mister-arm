/* versus_score.c -- see versus_score.h for the lifetime and rollback design. */

#include "hud/versus_score.h"

#include "hud/hud_strip.h"
#include "netplay/netplay.h"
#include "replay/replay_player.h"
#include "sf33rd/Source/Game/engine/workuser.h"

#include <SDL3/SDL.h>

/* --- State (NOT in GameState; never saved, loaded or rolled back) -------- */

static int s_score[2];
static char s_name[2][VERSUS_SCORE_NAME_MAX];

/* Edge detector over the simulated timeline. s_prev_concluded is the value
 * observed on the previous simulated frame (re-seeded on every load);
 * s_pending_frame is the frame whose simulation produced the edge, -1 when
 * none; s_pending_winner is who won it. */
static bool s_prev_concluded;
static int s_pending_frame = -1;
static int s_pending_winner;

/* Offline frame counter for TickLocal; only has to be monotonic. */
static int s_local_frame;

/* --- Session boundary ---------------------------------------------------- */

void VersusScore_Reset(void) {
    s_score[0] = 0;
    s_score[1] = 0;
    s_name[0][0] = '\0';
    s_name[1][0] = '\0';
    s_prev_concluded = false;
    s_pending_frame = -1;
    s_pending_winner = 0;
}

int VersusScore_Get(int player) {
    if (player < 0 || player > 1) {
        return 0;
    }
    return s_score[player];
}

void VersusScore_SetName(int player, const char* name) {
    if (player < 0 || player > 1) {
        return;
    }
    if (name == NULL) {
        s_name[player][0] = '\0';
        return;
    }
    SDL_strlcpy(s_name[player], name, sizeof(s_name[player]));
}

const char* VersusScore_GetName(int player) {
    if (player < 0 || player > 1) {
        return "";
    }
    return s_name[player];
}

/* --- Pure core ----------------------------------------------------------- */

void VersusScore_Observe(bool concluded, int winner, int sim_frame) {
    if (concluded && !s_prev_concluded && s_pending_frame < 0) {
        /* Keep-first if an edge is already pending: a second match cannot
         * end inside one prediction window (<= 33 frames), so a second edge
         * there is not a real result and must not displace the first. */
        s_pending_frame = sim_frame;
        s_pending_winner = (winner == 1) ? 1 : 0;
    }
    s_prev_concluded = concluded;
}

void VersusScore_OnLoad(int load_frame, bool concluded_at_load) {
    /* A load to L restores state AT L and re-advances from L + 1, so the
     * edge seen while simulating R is erased iff L < R (L == R keeps R's own
     * post-state; R is not re-run). Same derivation as netplay.c ->
     * menu_exit_request_erased_by_load. */
    if (s_pending_frame >= 0 && load_frame < s_pending_frame) {
        s_pending_frame = -1;
    }
    s_prev_concluded = concluded_at_load;
}

bool VersusScore_Confirm(int head_frame, int pred_window) {
    if (s_pending_frame < 0 || head_frame - s_pending_frame < pred_window) {
        return false;
    }
    s_score[s_pending_winner] += 1;
    s_pending_frame = -1;
    return true;
}

int VersusScore_PendingFrame(void) {
    return s_pending_frame;
}

void VersusScore_ComposeLabel(const char* name, int score, int player, char* out, size_t out_sz) {
    if (name == NULL || name[0] == '\0') {
        SDL_snprintf(out, out_sz, "%d", score);
    } else if (player == 0) {
        SDL_snprintf(out, out_sz, "%s %d", name, score);
    } else {
        SDL_snprintf(out, out_sz, "%d %s", score, name);
    }
}

/* --- Engine-bound -------------------------------------------------------- */

bool VersusScore_EngineMatchConcluded(void) {
    /* Main_Jmp_Tbl[2] = Game, Game_Jmp_Tbl[3] = Game03: the winner scene
     * that manage.c -> Game_Manage_10th routes a decided match into. Demo_Flag
     * is 0 on the attract loop (game.c -> Next_Demo_Loop), which never
     * decides a match but does leave Mode_Type wherever the last mode left
     * it. MODE_REPLAY reaches Game03 too and is excluded by the mode test. */
    return G_No[0] == 2 && G_No[1] == 3 && Demo_Flag != 0 &&
           (Mode_Type == MODE_VERSUS || Mode_Type == MODE_NETWORK);
}

void VersusScore_ObserveEngine(int sim_frame) {
    VersusScore_Observe(VersusScore_EngineMatchConcluded(), Winner_id, sim_frame);
}

void VersusScore_OnLoadEngine(int load_frame) {
    VersusScore_OnLoad(load_frame, VersusScore_EngineMatchConcluded());
}

void VersusScore_TickLocal(void) {
    /* The replay viewer plays Fightcade sets through the console VERSUS
     * flow (replay_player.c); its results are not this pairing's. */
    if (ReplayPlayer_IsActive()) {
        return;
    }
    s_local_frame += 1;
    VersusScore_ObserveEngine(s_local_frame);
    (void)VersusScore_Confirm(s_local_frame, 0);
}

void VersusScore_Draw(void) {
    /* The replay viewer draws its own name labels on this strip
     * (replay_overlay.c -> draw_name_labels); never overprint them. */
    if (ReplayPlayer_IsActive()) {
        return;
    }
    if (!HudStrip_Visible() || Demo_Flag == 0) {
        return;
    }
    if (Mode_Type == MODE_VERSUS) {
        /* local versus */
    } else if (Mode_Type == MODE_NETWORK && Netplay_GetSessionState() == NETPLAY_SESSION_RUNNING) {
        /* netplay, in session */
    } else {
        return;
    }

    char label[VERSUS_SCORE_NAME_MAX + 16];
    s32 w[2];

    for (int p = 0; p < 2; p++) {
        VersusScore_ComposeLabel(s_name[p], s_score[p], p, label, sizeof(label));
        w[p] = HudStrip_DrawLabel(p, label, HUD_STRIP_COL_YELLOW);
    }

    /* Headless/SSH-verifiable evidence, once per process: the display is
     * occlusion-throttled, so log the first frame the strip actually draws. */
    static bool logged = false;
    if (!logged) {
        logged = true;
        SDL_Log("versus-score: HUD strip drawn (y=%d) p1=%d p2=%d widths=%d,%d", HUD_STRIP_Y, s_score[0],
                s_score[1], (int)w[0], (int)w[1]);
    }
}
