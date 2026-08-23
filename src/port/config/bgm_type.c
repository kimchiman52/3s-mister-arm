#include "port/config/bgm_type.h"

#include "port/config/config.h"
#include "port/config/config_helpers.h"
#include "port/paths.h"
#include "sf33rd/Source/Game/system/work_sys.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

static const char* bgm_type_config_value(BgmType type) {
    return (type == BGM_ORIGINAL) ? "original" : "arranged";
}

void BgmType_ApplyBootOverride(void) {
    if (!Config_HasExplicitKey(CFG_KEY_BGM_TYPE)) {
        // No `bgm-type` line on disk yet (e.g. the OSD has never written
        // one) -- leave the value savesub.c's deserialize_settings() just
        // set from the settings save file alone.
        return;
    }

    const char* value = Config_GetString(CFG_KEY_BGM_TYPE);
    if (value != NULL && SDL_strcasecmp(value, "original") != 0 && SDL_strcasecmp(value, "arranged") != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "BgmType: unrecognized '%s' value '%s'; falling back to 'arranged'",
                    CFG_KEY_BGM_TYPE,
                    value);
    }
    sys_w.bgm_type = (value != NULL && SDL_strcasecmp(value, "original") == 0) ? BGM_ORIGINAL : BGM_ARRANGED;
}

// Config_SetString() only updates the in-memory entries table (config.c);
// Config_Save() is a documented no-op stub. The wrapper owns this exact
// find-line/replace-or-append/rename-into-place technique for every OSD
// key it persists (thirdsarm_wrapper.cpp's write_runtime_*_default()
// helpers) -- this is the same technique run from the game process so an
// in-game change round-trips into the same on-disk `config` file.
void BgmType_PersistToConfig(BgmType type) {
    const char* pref_path = Paths_GetPrefPath();
    if (pref_path == NULL) {
        return;
    }

    char path[512];
    char temp_path[512];
    SDL_snprintf(path, sizeof(path), "%sconfig", pref_path);
    SDL_snprintf(temp_path, sizeof(temp_path), "%sconfig.tmp", pref_path);

    FILE* in = fopen(path, "r");
    FILE* out = fopen(temp_path, "w");
    if (out == NULL) {
        if (in != NULL) {
            fclose(in);
        }
        return;
    }

    bool wrote_value = false;
    char line[256];

    if (in != NULL) {
        while (fgets(line, sizeof(line), in)) {
            char inspect[256];
            SDL_snprintf(inspect, sizeof(inspect), "%s", line);

            char* cursor = inspect;
            while (*cursor && SDL_isspace((unsigned char)*cursor)) {
                cursor++;
            }
            if (*cursor == '#') {
                fputs(line, out);
                continue;
            }

            char* equals = strchr(cursor, '=');
            if (equals != NULL) {
                *equals = 0;
                trim(cursor);
                if (SDL_strcasecmp(cursor, CFG_KEY_BGM_TYPE) == 0) {
                    fprintf(out, "%s = %s\n", CFG_KEY_BGM_TYPE, bgm_type_config_value(type));
                    wrote_value = true;
                    continue;
                }
            }

            fputs(line, out);
        }

        fclose(in);
    }

    if (!wrote_value) {
        fprintf(out, "\n%s = %s\n", CFG_KEY_BGM_TYPE, bgm_type_config_value(type));
    }

    if (fclose(out) != 0) {
        return;
    }
    if (rename(temp_path, path) != 0) {
        remove(temp_path);
        return;
    }

    // Keep the in-memory Config store in sync too, so a same-session
    // Config_GetString(CFG_KEY_BGM_TYPE) call observes the new value
    // without needing to re-read the file we just wrote.
    Config_SetString(CFG_KEY_BGM_TYPE, bgm_type_config_value(type));
}
