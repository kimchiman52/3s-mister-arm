#include "arcade/arcade_balance.h"
#include "arcade/arcade_char_data.h"
#include "port/config/config.h"

#include <SDL3/SDL.h>

static bool is_enabled = false;
static bool force_disabled = false;

void ArcadeBalance_Init() {
    /* force_disabled latch: NetplayNav_Arm runs inside set_netplay_params(),
     * BEFORE this init (main.c initialize_game ordering), so the netplay
     * force must survive the config re-read here. */
    is_enabled = !force_disabled && Config_GetBool(CFG_ARCADE_BALANCE);

    if (is_enabled) {
        ArcadeCharData_Init();

        if (!ArcadeCharData_IsInitialized()) {
            /* Half-enabled arcade balance (flag on, char data missing) would
             * abort every character texture-group load. Fall back wholesale. */
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Arcade balance enabled but sfiii3nr1.zip could not be loaded from "
                         "resources; falling back to PS2 balance");
            is_enabled = false;
        }
    }
}

bool ArcadeBalance_IsEnabled() {
    return is_enabled;
}

void ArcadeBalance_ForceDisable() {
    /* Session-only override -- does not rewrite the on-disk config. The
     * next launch re-reads whatever the user has saved. Balance is not
     * negotiated between netplay peers, so a local-only arcade setting
     * would guarantee a rollback desync. */
    if (is_enabled) {
        SDL_Log("Arcade balance disabled for netplay session (not negotiated between peers)");
    }
    force_disabled = true;
    is_enabled = false;
}
