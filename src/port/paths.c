#include "port/paths.h"

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdlib.h>

static char* pref_path = NULL;

static bool has_trailing_slash(const char* path) {
    const size_t len = SDL_strlen(path);
    return len > 0 && path[len - 1] == '/';
}

const char* Paths_GetPrefPath() {
    if (pref_path == NULL) {
#if defined(PORT_MISTER)
        const char* override = getenv("THIRDSARM_HOME");

        if (override != NULL && override[0] != '\0') {
            if (has_trailing_slash(override)) {
                pref_path = SDL_strdup(override);
            } else {
                SDL_asprintf(&pref_path, "%s/", override);
            }
        } else {
            pref_path = SDL_strdup("/media/fat/games/3s-arm/");
        }

        SDL_CreateDirectory(pref_path);
#else
        pref_path = SDL_GetPrefPath("CrowdedStreet", "3S-ARM");
#endif
    }

    return pref_path;
}

const char* Paths_GetBasePath() {
    return SDL_GetBasePath();
}
