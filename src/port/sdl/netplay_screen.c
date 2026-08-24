#include "port/sdl/netplay_screen.h"
#include "main.h"
#include "netplay/direct_p2p.h"
#include "netplay/matchmaking.h"
#include "netplay/netplay.h"
#include "netplay/netplay_nav.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"
#include <SDL3/SDL.h>

// Frames to hold the matchmaking message before switching to the next.
#define MM_TEXT_HOLD_FRAMES 30
// Frames to hold "Match found!" into the connecting phase before showing the game.
#define MATCH_FOUND_HOLD_FRAMES 90

static MatchmakingState display_state = MATCHMAKING_IDLE;
static int transition_hold = 0;
static int match_found_hold = 0;
// S3-review M-1: true while the connect phase (TRANSITIONING/CONNECTING)
// is being drawn. The "Connected!" hold is armed ONLY on the connect ->
// RUNNING transition; pre-fix the hold was re-armed every connect-phase
// frame and then drained as "Connected!" whatever state followed — a
// FAILED or aborted connection showed ~90 frames of "Connected!" on the
// attract screen.
static bool connect_phase_live = false;

static const char* mm_message(MatchmakingState s) {
    switch (s) {
    case MATCHMAKING_RESOLVING_DNS:
    case MATCHMAKING_CONNECTING_TCP:
    case MATCHMAKING_AWAITING_ID:
        return "Connecting to server...";

    case MATCHMAKING_SENDING_UDP:
    case MATCHMAKING_AWAITING_MATCH:
        return "Finding match...";

    case MATCHMAKING_ERROR:
        return "Matchmaking error";

    default:
        return "";
    }
}

void NetplayScreen_Render() {
    const NetplaySessionState ns = Netplay_GetSessionState();
    const MatchmakingState mm = Matchmaking_GetState();

    // Direct-P2P orchestrator overlay (Step 8 of docs/plan-stun-direct-p2p.md).
    // Only inspected while the netplay session is IDLE — once a session is
    // live the direct_p2p module has already completed handoff and the
    // in-game/match-found rendering below takes over. Direct-P2P is
    // lobby-unaware (matchmaking stays IDLE throughout) so checking session
    // state alone is sufficient. DirectP2P_DrawOverlay is a no-op when the
    // orchestrator is idle, so the early-return is gated on the session
    // check plus a cheap DirectP2P_GetState inspection to avoid swallowing
    // the matchmaking render path when Direct-P2P isn't in play.
    //
    // This runs BEFORE the nav overlay below because on the handoff path
    // the orchestrator draws its own richer overlay (HOSTING + room code,
    // JOIN CONNECTING, HANDOFF, etc). Replacing that with a generic
    // "CONNECTING..." would hide the room code the player needs to read.
    if (ns == NETPLAY_SESSION_IDLE && DirectP2P_GetState() != DIRECT_P2P_IDLE) {
        // S3 defensive guard (inherited from the R-1 review): the nav
        // overlay below carries a task[TASK_INIT].condition == 0 gate
        // because SSPutStrPro null-derefs before sf3_init primes the
        // sprite bank / ppg list. The direct-P2P overlay had NO such
        // guard, and FAILED_HANDSHAKE is the first FAILED_* state ever
        // drawn immediately after a Soft_Reset_Sub (which re-registers
        // TASK_INIT and purges/rebuilds texcash) — text drawing during
        // those re-init frames is unverified on hardware. Skip the draw
        // while TASK_INIT is live; the overlay resumes the moment init
        // finishes (the FAILED_* states are sticky, so nothing is lost).
        //
        // The balance lane adds a SECOND, earlier way into this branch,
        // which makes the guard load-bearing rather than merely
        // defensive: the arm-time refusal path (Netplay_RefuseArm) parks
        // the orchestrator in FAILED_HANDSHAKE from inside
        // set_netplay_params(), which runs BEFORE sf3_init() — so on a
        // refused boot this branch fires on frames where the sprite bank
        // has never been initialized at all. Every pre-refusal
        // orchestrator state could only appear after the first game-loop
        // tick, which is why this branch historically had no guard.
        // The early-return stays UNCONDITIONAL: while the orchestrator
        // owns the screen the matchmaking/nav paths below must not draw
        // over it, init-complete or not.
        if (task[TASK_INIT].condition == 0) {
            DirectP2P_DrawOverlay();
        }
        return;
    }

    // netplay_nav overlay. Nav runs from cold launch through the console-mode
    // menu chain before the netplay session starts, which means the user sees
    // attract → title → mode-select → char-select transition with no visible
    // connection indicator unless we draw one. Show "CONNECTING..." for the
    // whole duration nav is driving (NAV_WAIT_INIT through NAV_DRIVE_VS); the
    // existing TRANSITIONING "Match found!" block below takes over once nav
    // fires Netplay_BeginDirectP2P at NAV_START_NETPLAY.
    //
    // Gate on Init_Task having finished (task[TASK_INIT].condition==0). Nav
    // arms inside set_netplay_params() which runs BEFORE sf3_init(), so
    // NetplayScreen_Render fires on very early frames when the sprite bank
    // and ppg list SSPutStrPro needs haven't been initialized yet and a
    // draw call would null-deref.
    if (NetplayNav_IsActive() && task[TASK_INIT].condition == 0) {
        SSPutStrPro(1, 384, 2, 9, 0xFFFFFFFF, "CONNECTING...");
        return;
    }

    // While matchmaking is in progress show status text at the top of the
    // screen. This is safe at any time and doesn't require the full render pipeline.
    if (mm != MATCHMAKING_IDLE && mm != MATCHMAKING_MATCHED) {
        // Errors show immediately. Other state changes hold the current
        // message for MM_TEXT_HOLD_FRAMES before switching.
        if (mm == MATCHMAKING_ERROR) {
            display_state = mm;
            transition_hold = 0;
        } else if (mm == display_state) {
            // Do nothing
        } else if (transition_hold > 0) {
            transition_hold--;
        } else {
            display_state = mm;
            transition_hold = MM_TEXT_HOLD_FRAMES;
        }

        SSPutStrPro(1, 384, 2, 9, 0xFFFFFFFF, mm_message(display_state));
        return;
    }

    display_state = MATCHMAKING_IDLE;
    transition_hold = 0;

    // S3: live connect-phase progress. Pre-S3 this block drew a static
    // "Match found!" for the entire TRANSITIONING phase (including the
    // up-to-~20 s MIST handshake retry window) and the first frames of
    // CONNECTING — a lie while retries/syncing were actually happening.
    // netplay.c now maintains honest text ("Verifying opponent (3s)...",
    // "Syncing with opponent (7s)... START quits") which also advertises
    // the S3 hold-START abort. After the session starts RUNNING we hold a
    // brief "Connected!" before revealing the game.
    if (ns == NETPLAY_SESSION_TRANSITIONING || ns == NETPLAY_SESSION_CONNECTING) {
        const char* msg = Netplay_GetConnectStatusText();
        if (msg == NULL || msg[0] == '\0') {
            msg = "Match found!";
        }
        SSPutStrPro(1, 384, 110, 9, 0xFFFFFFFF, msg);
        connect_phase_live = true;
        return;
    }
    if (ns == NETPLAY_SESSION_RUNNING) {
        // Arm the brief "Connected!" hold only when the session actually
        // REACHED RUNNING out of a connect phase; drain it over the first
        // frames of the live game.
        if (connect_phase_live) {
            connect_phase_live = false;
            match_found_hold = MATCH_FOUND_HOLD_FRAMES;
        }
        if (match_found_hold > 0) {
            match_found_hold--;
            SSPutStrPro(1, 384, 110, 9, 0xFFFFFFFF, "Connected!");
        }
        return;
    }
    // Any other state (EXITING back to attract, IDLE after a failure or
    // user abort): the connection did NOT succeed — never show
    // "Connected!".
    connect_phase_live = false;
    match_found_hold = 0;
}
