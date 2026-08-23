/*
 * netplay_nav: auto-drive the console-mode menu chain at cold launch.
 *
 * Injects SWK_START at Title, at Game0_1 title-screen, and at Mode_Select
 * case 3 so the game runs through Game0_2 cases 3-5 (texture teardown +
 * BGM switch) and Mode_Select's case-3 path (Setup_VS_Mode + char-select
 * init) on both peers, then fires Netplay_BeginDirectP2P() once
 * remote_ip has been wired.
 *
 * Natural console-mode flow (press counts needed):
 *   1. Loop_Demo attract (G_No[0]==1). Ck_Coin() consumes a Start edge,
 *      runs Next_Title_Sub() which flips G_No[0]=2, registers TASK_ENTRY
 *      (E_No[0]=1 -> Entry_01). Demo_Flag=1 but Title screen is now up.
 *   2. Game0_0 -> Game0_1 on the title screen. Entry_01 case 1 consumes
 *      a Start edge, calls Entry_01_Sub() which sets Request_G_No=1.
 *      Game0_1 sees Request_G_No and advances G_No[2]++. Game0_2 cycles
 *      its r_no[3] 0..5 over ~6 frames; case 5 does
 *      cpReadyTask(TASK_MENU, Menu_Task) and sets G_No[1]=12.
 *   3. Menu_Task runs After_Title -> Menu_Init -> Mode_Select. Mode_Select
 *      r_no[2] cycles 0..3 for the fade-in; case 3 accepts Start with
 *      cursor row 1 -> Setup_VS_Mode(), G_No[1]=12, cpExitTask(TASK_MENU),
 *      Mode_Type=MODE_VERSUS. G_No[1] was already 12; Game12 now owns the
 *      frame and runs Select_Player (char-select) with all textures and
 *      BGM primed by the path above.
 *
 * The nav state machine never stalls indefinitely: each state has a
 * safety timeout after which it falls through to the next state, and
 * the MODE_NETWORK override + DIP-switch sync are always applied before
 * NAV_WAIT_ORCHESTRATOR. Cold-launch still reaches NAV_START_NETPLAY
 * even if something upstream blocks the expected transition. A timeout
 * path means char select will render wrong (the bug this module exists
 * to fix), but the session still starts — better than deadlocking.
 */

#include "netplay/netplay_nav.h"

#include "arcade/arcade_balance.h"
#include "main.h"
#include "netplay/netplay.h"
#include "port/config/draw_players_above_hud.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/system/work_sys.h"

#include <stdio.h>

typedef enum {
    NAV_IDLE = 0,
    NAV_WAIT_INIT,
    NAV_PRESS_COIN,
    NAV_PRESS_TITLE,
    NAV_WAIT_MENU,
    NAV_DRIVE_VS,
    NAV_WAIT_ORCHESTRATOR,
    NAV_START_NETPLAY,
    NAV_DONE,
} NavState;

static const char* state_name(NavState s) {
    switch (s) {
    case NAV_IDLE: return "NAV_IDLE";
    case NAV_WAIT_INIT: return "NAV_WAIT_INIT";
    case NAV_PRESS_COIN: return "NAV_PRESS_COIN";
    case NAV_PRESS_TITLE: return "NAV_PRESS_TITLE";
    case NAV_WAIT_MENU: return "NAV_WAIT_MENU";
    case NAV_DRIVE_VS: return "NAV_DRIVE_VS";
    case NAV_WAIT_ORCHESTRATOR: return "NAV_WAIT_ORCHESTRATOR";
    case NAV_START_NETPLAY: return "NAV_START_NETPLAY";
    case NAV_DONE: return "NAV_DONE";
    }
    return "NAV_?";
}

static NavState s_state = NAV_IDLE;
/* Frame counter per state — used for debounce and safety timeouts. */
static int s_frames_in_state = 0;
/* Tracks whether the current-state Start-press has been injected this
 * cycle so we inject for exactly one frame (rising edge in Ck_Coin /
 * Entry_01 / Mode_Select is ~p*sw_1 & p*sw_0 & SWK_START). */
static bool s_press_injected = false;

static void transition_to(NavState next) {
    fprintf(stderr, "[netplay_nav] state %s -> %s\n", state_name(s_state), state_name(next));
    fflush(stderr);
    s_state = next;
    s_frames_in_state = 0;
    s_press_injected = false;
}

static void inject_start_press(void) {
    p1sw_buff |= SWK_START;
    p2sw_buff |= SWK_START;
    s_press_injected = true;
}

/* Periodic re-injection helper: fire SWK_START for one frame, skip a
 * debounce window, then re-arm. This lets us re-try if the receiving
 * state wasn't ready on the first attempt (e.g. Entry_Task hadn't
 * transitioned into Entry_01 yet when we pressed during NAV_PRESS_COIN).
 * The debounce is important — pressing every frame would trigger the
 * rising-edge check every frame and cause unwanted repeated activation.
 * 8 frames (~0.13s) is a conservative lower bound: the game polls input
 * every frame and Menu_Task's r_no machine advances at most once per
 * frame, so a missed press is never more than a handful of frames from
 * the next retry window. */
#define NAV_PRESS_DEBOUNCE_FRAMES 8

static void drive_start_press(void) {
    if (!s_press_injected) {
        inject_start_press();
    } else if ((s_frames_in_state % NAV_PRESS_DEBOUNCE_FRAMES) == 0) {
        s_press_injected = false;
    }
}

static void apply_network_mode_override(void) {
    /* Switch to MODE_NETWORK and force both peers to use identical
     * DIP-switch settings. save_w[MODE_NETWORK] is read by Game01's
     * init path via mode-indexed accessors; applying here (before
     * Game12 advances into character select) ensures both peers
     * hash the same state on frame 0. */
    Mode_Type = MODE_NETWORK;
    Present_Mode = MODE_NETWORK;
    save_w[MODE_NETWORK].Time_Limit = 99;
    save_w[MODE_NETWORK].Battle_Number[0] = 1;
    save_w[MODE_NETWORK].Battle_Number[1] = 1;
    save_w[MODE_NETWORK].Damage_Level = 0;
    save_w[MODE_NETWORK].Handicap = 0;
    save_w[MODE_NETWORK].GuardCheck = 0;
    const u8 identity[8] = { 0, 1, 2, 11, 3, 4, 5, 11 };
    for (int p = 0; p < 2; p++) {
        for (int s = 0; s < 8; s++) {
            save_w[MODE_NETWORK].Pad_Infor[p].Shot[s] = identity[s];
        }
        save_w[MODE_NETWORK].Pad_Infor[p].Vibration = 0;
    }
}

void NetplayNav_Arm(void) {
    if (s_state != NAV_IDLE) {
        /* Idempotent — already armed or driving. */
        return;
    }
    /* Force console game mode for the session. The arcade path
     * (Loop_Demo branch in game.c) inline-jumps to Game12 / MODE_ARCADE
     * on the first Coin press, skipping Mode_Select registration
     * entirely. Nav would then wait in NAV_WAIT_MENU until its safety
     * timeout without accomplishing anything, and peers would also
     * diverge on the arcade-specific DIP-switch block that we never
     * synchronize. */
    SDLApp_ForceConsoleGameMode();
    /* Same class of divergence: arcade balance is a local config the peers
     * never negotiate; force PS2 balance for the session. */
    ArcadeBalance_ForceDisable();
    /* Same class again: CFG_DRAW_PLAYERS_ABOVE_HUD's gameplay-affecting
     * reads (effect-table population, scr_trans/scr_calc selection) are a
     * local config the peers never negotiate. */
    DrawPlayersAboveHud_ForceDisable();
    fprintf(stderr, "[netplay_nav] armed\n");
    fflush(stderr);
    transition_to(NAV_WAIT_INIT);
}

bool NetplayNav_IsActive(void) {
    return s_state != NAV_IDLE && s_state != NAV_DONE;
}

void NetplayNav_Reset(void) {
    s_state = NAV_IDLE;
    s_frames_in_state = 0;
    s_press_injected = false;
}

void NetplayNav_Tick(void) {
    if (s_state == NAV_IDLE || s_state == NAV_DONE) {
        return;
    }

    s_frames_in_state += 1;

    switch (s_state) {
    case NAV_WAIT_INIT:
        /* Wait for Init_Task to finish (condition=0 => exited) and the
         * boot sequence to land in Loop_Demo (G_No[0]==1). If someone
         * somehow advanced past Loop_Demo before we got here, pick up
         * at a later state. */
        if (G_No[0] == 2) {
            transition_to(NAV_PRESS_TITLE);
            break;
        }
        if (task[TASK_INIT].condition == 0 && G_No[0] == 1) {
            transition_to(NAV_PRESS_COIN);
        }
        break;

    case NAV_PRESS_COIN:
        /* Attract -> Title: inject Start (debounced) while G_No[0]==1.
         * Ck_Coin() rising-edge check wires this into Next_Title_Sub(),
         * which flips G_No[0]=2 and registers TASK_ENTRY. Safety
         * timeout ~10s. */
        if (G_No[0] == 2) {
            transition_to(NAV_PRESS_TITLE);
            break;
        }
        if (G_No[0] == 1) {
            drive_start_press();
        }
        if (s_frames_in_state > 600) {
            /* Timeout — pick the most-sensible fallthrough based on
             * where the engine actually is. If still in attract
             * (G_No[0]==1) the coin press never landed; give
             * NAV_PRESS_TITLE another 10s window in case the rising
             * edge fires there. If we somehow advanced (G_No[0]>1)
             * without this state seeing it, skip ahead to
             * NAV_WAIT_MENU. */
            if (G_No[0] == 1) {
                transition_to(NAV_PRESS_TITLE);
            } else {
                transition_to(NAV_WAIT_MENU);
            }
        }
        break;

    case NAV_PRESS_TITLE:
        /* Title -> Menu: inject Start (debounced) while G_No[0]==2 and
         * Menu_Task hasn't been registered yet. Entry_01 case 1 sees
         * the rising edge via ~p*sw_1 & p*sw_0 & SWK_START and calls
         * Entry_01_Sub(), setting Request_G_No=1. Game0_1 sees
         * Request_G_No and advances to Game0_2; Game0_2 case 5 does
         * cpReadyTask(TASK_MENU, Menu_Task) and sets G_No[1]=12.
         *
         * Advance to NAV_WAIT_MENU once TASK_MENU is registered
         * (condition 1 or 2 — cpReadyTask sets 2, the next main
         * loop iteration flips it to 1). Safety timeout ~10s. */
        if (task[TASK_MENU].condition == 1 || task[TASK_MENU].condition == 2) {
            transition_to(NAV_WAIT_MENU);
            break;
        }
        if (G_No[0] == 2) {
            drive_start_press();
        }
        if (s_frames_in_state > 600) {
            transition_to(NAV_WAIT_MENU);
        }
        break;

    case NAV_WAIT_MENU:
        /* Advance to NAV_DRIVE_VS as soon as Menu_Task is live. Don't
         * wait for Mode_Select's fade-in to hit r_no[2]==3 — that just
         * adds perceived latency on-screen. NAV_DRIVE_VS will keep
         * re-pressing Start on the NAV_PRESS_DEBOUNCE_FRAMES cadence;
         * Mode_Select ignores presses during case 0/1/2 (fade-in) and
         * accepts the first press after case 3 fires. Safety timeout
         * ~10s in case Menu_Task never registers. */
        if (task[TASK_MENU].condition == 1) {
            transition_to(NAV_DRIVE_VS);
            break;
        }
        if (s_frames_in_state > 600) {
            /* Safety fallthrough: apply the MODE_NETWORK override so
             * the orchestrator/session path still runs on sane state
             * even if Mode_Select never reached case 3. */
            apply_network_mode_override();
            transition_to(NAV_WAIT_ORCHESTRATOR);
        }
        break;

    case NAV_DRIVE_VS:
        /* Force cursor to row 1 (Versus) and inject Start. Mode_Select
         * case 3 sees the rising edge, runs Setup_VS_Mode(),
         * cpExitTask(TASK_MENU), sets G_No[1]=12.
         *
         * Connect_Status fixup: when only P1 has an Interface_Type
         * bound (the common case on desktop/MiSTer single-pad setups),
         * Menu_Task's header at menu.c:219 sets Connect_Status=0 and
         * Mode_Select case 3 then forces Menu_Cursor_Y[0] from 1 to 2
         * (Training) before the Start press latches. Override both
         * Interface_Type slots before Menu_Task reads them this frame
         * (nav tick runs after keyConvert() but before the task
         * dispatch that invokes Menu_Task) so Connect_Status=1 and the
         * cursor-coercion in case 3 is skipped. The override is
         * transient — ioconv restores pad->kind-derived values on the
         * next frame after we leave NAV_DRIVE_VS. */
        Interface_Type[0] = 2;
        Interface_Type[1] = 2;
        Menu_Cursor_Y[0] = 1;
        if (task[TASK_MENU].condition == 0) {
            /* Menu exited this frame — Mode_Select set Mode_Type=
             * MODE_VERSUS on the way out. Apply the MODE_NETWORK
             * override in the same tick so Game12's very first tick
             * already sees MODE_NETWORK and both peers hash identical
             * state from frame 0. */
            apply_network_mode_override();
            transition_to(NAV_WAIT_ORCHESTRATOR);
            break;
        }
        drive_start_press();
        if (s_frames_in_state > 600) {
            /* Safety fallthrough: apply the override anyway so the
             * orchestrator/session path still runs on sane state. */
            apply_network_mode_override();
            transition_to(NAV_WAIT_ORCHESTRATOR);
        }
        break;

    case NAV_WAIT_ORCHESTRATOR:
        /* For --p2p-remote-ip (LAN/localhost) Netplay_SetParams wired
         * remote_ip at set_netplay_params() time, so this flips true
         * immediately. For the direct-P2P orchestrator path the
         * UPnP+STUN hole-punch takes several seconds and remote_ip is
         * populated by do_handoff() after that. Wait here so
         * NAV_START_NETPLAY doesn't hit the early-return in
         * Netplay_BeginDirectP2P() when remote_ip is NULL. */
        if (Netplay_IsRemoteIpSet()) {
            transition_to(NAV_START_NETPLAY);
        }
        break;

    case NAV_START_NETPLAY:
        Netplay_BeginDirectP2P();
        transition_to(NAV_DONE);
        break;

    case NAV_IDLE:
    case NAV_DONE:
        /* Unreachable given the early-return above. */
        break;
    }
}
