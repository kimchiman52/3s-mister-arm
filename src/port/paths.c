#include "port/paths.h"

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdlib.h>

#define ORG "CrowdedStreet"
#define APP "3S-ARM"

static char* pref_path = NULL;

static bool has_trailing_slash(const char* path) {
    const size_t len = SDL_strlen(path);
    return len > 0 && path[len - 1] == '/';
}

/*
 * THIRDSARM_HOME is honoured on EVERY port, not just the embedded ones
 * (task #125).
 *
 * It used to be MiSTer/Miyoo-only, and on the desktop/host build there was
 * no way at all to move this directory: SDL_GetPrefPath resolves through
 * the platform's own API, and on macOS that is
 * NSApplicationSupportDirectory, which ignores $HOME (measured: running
 * a harness with HOME=/tmp/fakehome1 still wrote to
 * ~/Library/Application Support/CrowdedStreet/3S-ARM/). So two host
 * processes could not be given separate state even in principle.
 *
 * That is what made #125 possible. Concurrent gate runs shared ONE logs
 * directory, and the netplay session log is named netplay-<utc_ms>.log,
 * so processes that started in the same millisecond opened the SAME FILE
 * and interleaved writes into it. Measured, 4 concurrent
 * --test-connect-observability runs: three of them opened
 * netplay-1788111677761.log, and all three then validated against
 * netplay-1788111677763.log (a fourth process's file) because the harness
 * picked "the newest log in the directory". Symptoms: "800 of 800 MT
 * lines missing or torn" and "session file grew AFTER the TRUNCATED
 * marker".
 *
 * With the override universal, tools/gates/run-gates.sh hands every
 * harness invocation a private home and the sharing is gone by
 * construction — logs, config, keymap, the tester report, and the #44
 * prune all become per-process.
 */
const char* Paths_GetPrefPath() {
    if (pref_path == NULL) {
        const char* override = getenv("THIRDSARM_HOME");

        if (override != NULL && override[0] != '\0') {
            if (has_trailing_slash(override)) {
                pref_path = SDL_strdup(override);
            } else {
                SDL_asprintf(&pref_path, "%s/", override);
            }
            SDL_CreateDirectory(pref_path);
        } else {
#if defined(PORT_MISTER)
            pref_path = SDL_strdup("/media/fat/games/3s-arm/");
            SDL_CreateDirectory(pref_path);
#elif defined(PORT_MIYOO_MINI_PLUS)
            pref_path = SDL_strdup("/mnt/SDCARD/Roms/PORTS/Games/3s-arm/");
            SDL_CreateDirectory(pref_path);
#else
            pref_path = SDL_GetPrefPath(ORG, APP);
#endif
        }
    }

    return pref_path;
}

const char* Paths_GetBasePath() {
    return SDL_GetBasePath();
}

SDL_Storage* Paths_OpenUserStorage(SDL_PropertiesID props) {
    // Deliberately NOT SDL_OpenUserStorage(ORG, APP, props): SDL's generic
    // user-storage backend resolves its own SDL_GetPrefPath(org, app)
    // internally (see SDL src/storage/generic/SDL_genericstorage.c,
    // GENERIC_User_Create), bypassing the PORT_MISTER/PORT_MIYOO_MINI_PLUS
    // override + THIRDSARM_HOME logic above entirely. That would silently
    // relocate saves away from Paths_GetPrefPath() (where config/keymap/
    // training already live) to whatever XDG default SDL_GetPrefPath()
    // picks on the embedded Linux target. SDL_OpenFileStorage(path) uses
    // the same generic file-storage interface but against an arbitrary
    // path, so pointing it at our own Paths_GetPrefPath() keeps saves/
    // colocated with the rest of this port's persisted state.
    (void)props;
    return SDL_OpenFileStorage(Paths_GetPrefPath());
}
