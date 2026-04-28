#include "port/config/config.h"
#include "port/config/config_helpers.h"
#include "port/paths.h"

#include <stdbool.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#define CONFIG_ENTRIES_MAX 128

typedef enum ConfigType {
    CFG_BOOL,
    CFG_INT,
    CFG_STRING,
} ConfigType;

typedef union ConfigValue {
    bool b;
    int i;
    char* s;
} ConfigValue;

typedef struct ConfigEntry {
    const char* key;
    ConfigType type;
    ConfigValue value;
} ConfigEntry;

#if defined(PORT_MISTER)
#define DEFAULT_VIDEO_DRIVER_ORDER "dummy"
#define DEFAULT_RENDER_DRIVER_ORDER "software"
#define DEFAULT_WINDOW_WIDTH 320
#define DEFAULT_WINDOW_HEIGHT 240
#define DEFAULT_SCALE_MODE "native"
#define DEFAULT_SOFTWARE_FRAME_MODE "on"
#else
#define DEFAULT_VIDEO_DRIVER_ORDER ""
#define DEFAULT_RENDER_DRIVER_ORDER ""
#define DEFAULT_WINDOW_WIDTH 640
#define DEFAULT_WINDOW_HEIGHT 480
#define DEFAULT_SCALE_MODE "soft-linear"
#define DEFAULT_SOFTWARE_FRAME_MODE "off"
#endif

#define DEFAULT_SUPER_EFFECT_QUALITY "full"
#define DEFAULT_GHOST_RESOLUTION "full"
#define DEFAULT_GHOST_COUNT "4"
#define DEFAULT_ARM_CLOCK "800"
#define DEFAULT_GAME_MODE "console"
#define DEFAULT_HOLD_TO_PAUSE "off"

static const ConfigEntry default_entries[] = {
    { .key = CFG_KEY_FULLSCREEN, .type = CFG_BOOL, .value.b = true },
    { .key = CFG_KEY_WINDOW_WIDTH, .type = CFG_INT, .value.i = DEFAULT_WINDOW_WIDTH },
    { .key = CFG_KEY_WINDOW_HEIGHT, .type = CFG_INT, .value.i = DEFAULT_WINDOW_HEIGHT },
    { .key = CFG_KEY_SCALEMODE, .type = CFG_STRING, .value.s = DEFAULT_SCALE_MODE },
    { .key = CFG_KEY_SCANLINES, .type = CFG_INT, .value.i = 0 },
    { .key = CFG_DRAW_PLAYERS_ABOVE_HUD, .type = CFG_BOOL, .value.b = false },
    { .key = CFG_ARCADE_BALANCE, .type = CFG_BOOL, .value.b = false },
    { .key = CFG_KEY_SOFTWARE_FRAME_MODE, .type = CFG_STRING, .value.s = DEFAULT_SOFTWARE_FRAME_MODE },
    { .key = CFG_KEY_SUPER_EFFECT_QUALITY, .type = CFG_STRING, .value.s = DEFAULT_SUPER_EFFECT_QUALITY },
    { .key = CFG_KEY_GHOST_RESOLUTION, .type = CFG_STRING, .value.s = DEFAULT_GHOST_RESOLUTION },
    { .key = CFG_KEY_GHOST_COUNT, .type = CFG_STRING, .value.s = DEFAULT_GHOST_COUNT },
    { .key = CFG_KEY_ARM_CLOCK, .type = CFG_STRING, .value.s = DEFAULT_ARM_CLOCK },
    { .key = CFG_KEY_GAME_MODE, .type = CFG_STRING, .value.s = DEFAULT_GAME_MODE },
    { .key = CFG_KEY_HOLD_TO_PAUSE, .type = CFG_STRING, .value.s = DEFAULT_HOLD_TO_PAUSE },
    { .key = CFG_KEY_SHOW_FPS, .type = CFG_STRING, .value.s = "off" },
    { .key = CFG_KEY_VIDEO_DRIVER_ORDER, .type = CFG_STRING, .value.s = DEFAULT_VIDEO_DRIVER_ORDER },
    { .key = CFG_KEY_RENDER_DRIVER_ORDER, .type = CFG_STRING, .value.s = DEFAULT_RENDER_DRIVER_ORDER },
    /* Direct-P2P defaults (docs/plan-stun-direct-p2p.md Step 5). The 3
     * runtime-populated keys (LAST_PEER_CODE + HOST_PORT at 0 + DISABLE_UPNP)
     * are not here — they live in the entries[] at runtime only. HANDOFF_PATH
     * default is the wrapper's canonical tmpfs path; STUN_TIMEOUT_MS budget
     * bumped from upstream's 2000ms to 4000ms for congested public STUN. */
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_HANDOFF_PATH, .type = CFG_STRING, .value.s = "/tmp/3s-arm-netplay.handoff" },
    { .key = CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS, .type = CFG_INT, .value.i = 4000 },
    { .key = CFG_KEY_NETPLAY_INPUT_PREDICTION_WINDOW, .type = CFG_INT, .value.i = 8 },
    { .key = CFG_KEY_NETPLAY_DIAG_ENABLE, .type = CFG_BOOL, .value.b = true },
    { .key = CFG_KEY_NETPLAY_SPARSE_EFFECT_SAVE_ENABLED, .type = CFG_BOOL, .value.b = true },
};

static ConfigEntry entries[CONFIG_ENTRIES_MAX] = { 0 };
static int entry_count = 0;

static bool is_int(const char* string) {
    for (int i = 0; i < SDL_strlen(string); i++) {
        if (SDL_isdigit(string[i]) || ((i == 0) && (string[i] == '-'))) {
            continue;
        } else {
            return false;
        }
    }

    return true;
}

static ConfigEntry* find_entry_in_array(const char* key, const ConfigEntry* array, size_t size) {
    for (int i = 0; i < size; i++) {
        const ConfigEntry* entry = &array[i];

        if (SDL_strcmp(key, entry->key) == 0) {
            return entry;
        }
    }

    return NULL;
}

static ConfigEntry* find_entry(const char* key) {
    ConfigEntry* default_entry = find_entry_in_array(key, default_entries, SDL_arraysize(default_entries));
    ConfigEntry* read_entry = find_entry_in_array(key, entries, entry_count);

    if (read_entry != NULL) {
        if (default_entry != NULL && read_entry->type != default_entry->type) {
            // If we expect a certain type and the one we read from config is unexpected, let's use the default entry
            // instead
            return default_entry;
        } else {
            return read_entry;
        }
    } else if (default_entry != NULL) {
        return default_entry;
    } else {
        SDL_assert(false);
        return NULL;
    }
}

bool Config_HasExplicitKey(const char* key) {
    return find_entry_in_array(key, entries, entry_count) != NULL;
}

static void print_config_entry_to_io(SDL_IOStream* io, const ConfigEntry* entry) {
    io_printf(io, "%s = ", entry->key);

    switch (entry->type) {
    case CFG_BOOL:
        io_printf(io, entry->value.b ? "true" : "false");
        break;

    case CFG_INT:
        io_printf(io, "%d", entry->value.i);
        break;

    case CFG_STRING:
        io_printf(io, entry->value.s);
        break;
    }
}

static void write_defaults(const char* dst_path) {
    SDL_IOStream* io = SDL_IOFromFile(dst_path, "w");
    io_printf(io,
              "# For the full list of settings see https://github.com/kimchiman52/3s-mister-arm/blob/main/docs/config.md\n\n");

    for (int i = 0; i < SDL_arraysize(default_entries); i++) {
        print_config_entry_to_io(io, &default_entries[i]);
        io_printf(io, "\n");
    }

    SDL_CloseIO(io);
}

static bool dict_iterator(const char* key, const char* value) {
    if (entry_count == CONFIG_ENTRIES_MAX) {
        printf("⚠️ Reached max config entry count (%d), skipping the rest\n", CONFIG_ENTRIES_MAX);
        return false;
    }

    ConfigEntry* entry = &entries[entry_count];
    entry->key = SDL_strdup(key);

    const bool is_true = SDL_strcmp(value, "true") == 0;
    const bool is_false = SDL_strcmp(value, "false") == 0;

    if (is_true || is_false) {
        entry->type = CFG_BOOL;
        entry->value.b = is_true;
    } else if (is_int(value)) {
        entry->type = CFG_INT;
        entry->value.i = SDL_atoi(value);
    } else {
        entry->type = CFG_STRING;
        entry->value.s = SDL_strdup(value);
    }

    entry_count += 1;
    return true;
}

void Config_Init() {
    const char* pref_path = Paths_GetPrefPath();
    char* config_path;
    SDL_asprintf(&config_path, "%sconfig", pref_path);

    FILE* f = fopen(config_path, "r");

    if (f == NULL) {
        // Config doesn't exist. Write defaults
        write_defaults(config_path);
        SDL_free(config_path);
        return;
    }

    SDL_free(config_path);
    dict_read(f, dict_iterator);
    fclose(f);
}

void Config_Destroy() {
    for (int i = 0; i < entry_count; i++) {
        ConfigEntry* entry = &entries[i];
        SDL_free(entry->key);

        if (entry->type == CFG_STRING) {
            SDL_free(entry->value.s);
        }
    }

    SDL_zeroa(entries);
    entry_count = 0;
}

bool Config_GetBool(const char* key) {
    const ConfigEntry* entry = find_entry(key);

    if (entry == NULL || entry->type != CFG_BOOL) {
        return false;
    }

    return entry->value.b;
}

int Config_GetInt(const char* key) {
    const ConfigEntry* entry = find_entry(key);

    if (entry == NULL || entry->type != CFG_INT) {
        return 0;
    }

    return entry->value.i;
}

const char* Config_GetString(const char* key) {
    const ConfigEntry* entry = find_entry(key);

    if (entry == NULL || entry->type != CFG_STRING) {
        return NULL;
    }

    return entry->value.s;
}

/* Config_SetString updates the in-memory entries table so subsequent
 * Config_GetString calls within the same session see the new value.
 * Config_Save is currently a no-op with a warn-once log so callers have a
 * stable symbol; real on-disk persistence is deferred until a caller
 * needs it. Config_Init's write_defaults() still bootstraps an on-disk
 * file so users can edit it manually as before. */

void Config_SetString(const char* key, const char* value) {
    if (key == NULL || value == NULL) {
        return;
    }

    ConfigEntry* existing = find_entry_in_array(key, entries, entry_count);
    if (existing != NULL) {
        if (existing->type == CFG_STRING) {
            char* dup = SDL_strdup(value);
            if (dup == NULL) {
                return;
            }
            SDL_free(existing->value.s);
            existing->value.s = dup;
        } else {
            existing->type = CFG_STRING;
            existing->value.s = SDL_strdup(value);
        }
        return;
    }

    if (entry_count >= CONFIG_ENTRIES_MAX) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Config_SetString: entries table full (%d); dropping key '%s'",
                    CONFIG_ENTRIES_MAX,
                    key);
        return;
    }

    ConfigEntry* entry = &entries[entry_count++];
    entry->key = SDL_strdup(key);
    entry->type = CFG_STRING;
    entry->value.s = SDL_strdup(value);
}

void Config_Save(void) {
    static bool warned = false;
    if (!warned) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Config_Save: in-memory only (stub); on-disk persistence not yet wired.");
        warned = true;
    }
}
