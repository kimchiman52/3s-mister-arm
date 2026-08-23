#include "port/paths.h"

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdlib.h>

#define ORG "CrowdedStreet"
#define APP "3S-ARM"

static char* pref_path = NULL;

#if defined(PORT_MISTER) || defined(PORT_MIYOO_MINI_PLUS)
static bool has_trailing_slash(const char* path) {
    const size_t len = SDL_strlen(path);
    return len > 0 && path[len - 1] == '/';
}
#endif

const char* Paths_GetPrefPath() {
    if (pref_path == NULL) {
#if defined(PORT_MISTER) || defined(PORT_MIYOO_MINI_PLUS)
        const char* override = getenv("THIRDSARM_HOME");

        if (override != NULL && override[0] != '\0') {
            if (has_trailing_slash(override)) {
                pref_path = SDL_strdup(override);
            } else {
                SDL_asprintf(&pref_path, "%s/", override);
            }
        } else {
#if defined(PORT_MISTER)
            pref_path = SDL_strdup("/media/fat/games/3s-arm/");
#else
            pref_path = SDL_strdup("/mnt/SDCARD/Roms/PORTS/Games/3s-arm/");
#endif
        }

        SDL_CreateDirectory(pref_path);
#else
        pref_path = SDL_GetPrefPath(ORG, APP);
#endif
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
