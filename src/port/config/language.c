#include "port/config/language.h"

#include "main.h"
#include "port/config/config.h"
#include "port/config/config_helpers.h"
#include "port/paths.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

static const char* language_config_value(Language language) {
    return (language == LANG_JAPANESE) ? "japanese" : "english";
}

void Language_ApplyBootOverride(void) {
    // Config_GetString() falls back to the default_entries value
    // (CFG_KEY_LANGUAGE = DEFAULT_LANGUAGE = "auto") when the key is absent
    // from the on-disk config, so the `auto` branch below covers both the
    // "never written" and the "explicitly auto" cases -- no
    // Config_HasExplicitKey() probe needed.
    const char* value = Config_GetString(CFG_KEY_LANGUAGE);

    if (value != NULL && SDL_strcasecmp(value, "english") == 0) {
        mpp_w.language = LANG_ENGLISH;
    } else if (value != NULL && SDL_strcasecmp(value, "japanese") == 0) {
        mpp_w.language = LANG_JAPANESE;
    } else if (value == NULL || SDL_strcasecmp(value, "auto") != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Language: unrecognized '%s' value '%s'; treating it as 'auto'",
                    CFG_KEY_LANGUAGE,
                    value != NULL ? value : "(null)");
    }

    // Materialize the resolved language back into the key. On `auto` (a
    // fresh install, or a save file written before the Language selector
    // shipped) this is what makes the OSD row truthful: the wrapper reads
    // this key to seed status bit [47] and cannot otherwise know which way
    // Get_Default_Language()/the settings save resolved. Once concrete this
    // is a no-op (Language_PersistToConfig() skips matching values).
    Language_PersistToConfig(mpp_w.language);
}

// Config_SetString() only updates the in-memory entries table (config.c);
// Config_Save() is a documented no-op stub. This is the same
// find-line/replace-or-append/rename-into-place technique that
// BgmType_PersistToConfig() (bgm_type.c) and the wrapper's
// write_runtime_*_default() helpers use, so an in-game change round-trips
// into the same on-disk `config` file the OSD reads.
void Language_PersistToConfig(Language language) {
    const char* target = language_config_value(language);

    // Skip the rewrite when the key already holds this value. Keeps the
    // boot-time materialization above free after the first run and makes
    // the Screen Adjust exit hook safe to call unconditionally.
    const char* current = Config_GetString(CFG_KEY_LANGUAGE);
    if (current != NULL && SDL_strcasecmp(current, target) == 0) {
        return;
    }

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
                if (SDL_strcasecmp(cursor, CFG_KEY_LANGUAGE) == 0) {
                    fprintf(out, "%s = %s\n", CFG_KEY_LANGUAGE, target);
                    wrote_value = true;
                    continue;
                }
            }

            fputs(line, out);
        }

        fclose(in);
    }

    if (!wrote_value) {
        fprintf(out, "\n%s = %s\n", CFG_KEY_LANGUAGE, target);
    }

    if (fclose(out) != 0) {
        return;
    }
    if (rename(temp_path, path) != 0) {
        remove(temp_path);
        return;
    }

    // Keep the in-memory Config store in sync too, so a same-session
    // Config_GetString(CFG_KEY_LANGUAGE) call observes the new value
    // without needing to re-read the file we just wrote.
    Config_SetString(CFG_KEY_LANGUAGE, target);
}
