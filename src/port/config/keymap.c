#include "port/config/keymap.h"
#include "port/config/config_helpers.h"
#include "port/paths.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#if defined(PORT_MIYOO_MINI_PLUS)
/* Miyoo Mini Plus / OnionOS — keymon emits keyboard scancodes for the
 * physical buttons. Mapping below is Capcom-PS2-style for SF3 (punches
 * top row Y/X/L1, kicks bottom row B/A/R1). The keymon scancodes are
 * documented in OnionUI/Onion's keymon.c. */
static const SDL_Scancode default_keymap[KEYMAP_BUTTON_COUNT][KEYMAP_CODES_PER_BUTTON] = {
    { SDL_SCANCODE_UP },        // up      — D-pad up
    { SDL_SCANCODE_DOWN },      // down    — D-pad down
    { SDL_SCANCODE_LEFT },      // left    — D-pad left
    { SDL_SCANCODE_RIGHT },     // right   — D-pad right
    { SDL_SCANCODE_LSHIFT },    // north   — X button   → MP
    { SDL_SCANCODE_LALT },      // west    — Y button   → LP
    { SDL_SCANCODE_LCTRL },     // south   — B button   → LK
    { SDL_SCANCODE_SPACE },     // east    — A button   → MK
    { SDL_SCANCODE_E },         // L-shldr — L1 (KEY_E)         → HP
    { SDL_SCANCODE_BACKSPACE }, // R-shldr — R1 (KEY_BACKSPACE) → HK (was T; swapped per user report)
    { SDL_SCANCODE_TAB },       // L-trig  — L2 (KEY_TAB)       → 3P macro
    { SDL_SCANCODE_T },         // R-trig  — R2 (KEY_T)         → 3K macro (was Backspace; swapped)
    { SDL_SCANCODE_UNKNOWN },   // L-stick
    { SDL_SCANCODE_UNKNOWN },   // R-stick
    { SDL_SCANCODE_RCTRL },     // back    — Select
    { SDL_SCANCODE_RETURN },    // start   — Start
};
#else
static const SDL_Scancode default_keymap[KEYMAP_BUTTON_COUNT][KEYMAP_CODES_PER_BUTTON] = {
    { SDL_SCANCODE_UP, SDL_SCANCODE_W, SDL_SCANCODE_SPACE }, // up
    { SDL_SCANCODE_DOWN, SDL_SCANCODE_S },                   // down
    { SDL_SCANCODE_LEFT, SDL_SCANCODE_A },                   // left
    { SDL_SCANCODE_RIGHT, SDL_SCANCODE_D },                  // right
    { SDL_SCANCODE_I },                                      // north
    { SDL_SCANCODE_U },                                      // west
    { SDL_SCANCODE_J },                                      // south
    { SDL_SCANCODE_K },                                      // east
    { SDL_SCANCODE_P },                                      // left shoulder
    { SDL_SCANCODE_O },                                      // right shoulder
    { SDL_SCANCODE_SEMICOLON },                              // left trigger
    { SDL_SCANCODE_L },                                      // right trigger
    { SDL_SCANCODE_9 },                                      // left stick
    { SDL_SCANCODE_0 },                                      // right stick
    { SDL_SCANCODE_BACKSPACE },                              // back
    { SDL_SCANCODE_RETURN },                                 // start
};
#endif

static SDL_Scancode keymap[KEYMAP_BUTTON_COUNT][KEYMAP_CODES_PER_BUTTON] = {};
static bool initialized_buttons[KEYMAP_BUTTON_COUNT] = { false };

const char* Keymap_GetButtonName(KeymapButton button) {
    switch (button) {
    case KEYMAP_BUTTON_UP:
        return "up";
    case KEYMAP_BUTTON_DOWN:
        return "down";
    case KEYMAP_BUTTON_LEFT:
        return "left";
    case KEYMAP_BUTTON_RIGHT:
        return "right";
    case KEYMAP_BUTTON_NORTH:
        return "north";
    case KEYMAP_BUTTON_WEST:
        return "west";
    case KEYMAP_BUTTON_SOUTH:
        return "south";
    case KEYMAP_BUTTON_EAST:
        return "east";
    case KEYMAP_BUTTON_LEFT_SHOULDER:
        return "left-shoulder";
    case KEYMAP_BUTTON_RIGHT_SHOULDER:
        return "right-shoulder";
    case KEYMAP_BUTTON_LEFT_TRIGGER:
        return "left-trigger";
    case KEYMAP_BUTTON_RIGHT_TRIGGER:
        return "right-trigger";
    case KEYMAP_BUTTON_LEFT_STICK:
        return "left-stick";
    case KEYMAP_BUTTON_RIGHT_STICK:
        return "right-stick";
    case KEYMAP_BUTTON_BACK:
        return "back";
    case KEYMAP_BUTTON_START:
        return "start";
    default:
        return "";
    }

    return "unknown";
}

static KeymapButton get_button(const char* name) {
    for (int i = 0; i < KEYMAP_BUTTON_COUNT; i++) {
        const char* this_name = Keymap_GetButtonName(i);

        if (SDL_strcmp(name, this_name) == 0) {
            return i;
        }
    }

    return -1;
}

/* `table` is the flat base of a [KEYMAP_BUTTON_COUNT][KEYMAP_CODES_PER_BUTTON]
 * array; flat because C will not implicitly convert SDL_Scancode(*)[4] to
 * const SDL_Scancode(*)[4], which a 2-D parameter would need at one of the
 * two call sites. */
static bool write_table(const char* dst_path, const SDL_Scancode* table) {
    SDL_IOStream* io = SDL_IOFromFile(dst_path, "w");

    if (io == NULL) {
        return false;
    }

    for (int i = 0; i < KEYMAP_BUTTON_COUNT; i++) {
        io_printf(io, "%s = ", Keymap_GetButtonName(i));

        bool is_first = true;

        for (int j = 0; j < KEYMAP_CODES_PER_BUTTON; j++) {
            const SDL_Scancode code = table[i * KEYMAP_CODES_PER_BUTTON + j];

            if (code == SDL_SCANCODE_UNKNOWN) {
                break;
            }

            if (!is_first) {
                io_printf(io, ", ");
            }

            is_first = false;
            io_printf(io, "%s", SDL_GetScancodeName(code));
        }

        io_printf(io, "\n");
    }

    return SDL_CloseIO(io);
}

static void write_defaults(const char* dst_path) {
    write_table(dst_path, &default_keymap[0][0]);
}

static bool dict_iterator(const char* key, const char* value) {
    const KeymapButton button = get_button(key);

    if (button == -1) {
        return true;
    }

    int code_index = 0;
    char val[128];
    SDL_strlcpy(val, value, sizeof(val));

    char name[32];
    char* saveptr;
    char* token = SDL_strtok_r(val, ",", &saveptr);

    while (token != NULL && code_index < KEYMAP_CODES_PER_BUTTON) {
        SDL_strlcpy(name, token, sizeof(name));
        trim(name);
        const SDL_Scancode code = SDL_GetScancodeFromName(name);

        if (code != SDL_SCANCODE_UNKNOWN) {
            keymap[button][code_index] = code;
            code_index += 1;
        }

        token = SDL_strtok_r(NULL, ",", &saveptr);
    }

    if (code_index > 0) {
        initialized_buttons[button] = true;
    }

    return true;
}

static void initialize_empty_buttons() {
    for (int i = 0; i < KEYMAP_BUTTON_COUNT; i++) {
        if (!initialized_buttons[i]) {
            SDL_memcpy(keymap[i], default_keymap[i], sizeof(default_keymap[0]));
            initialized_buttons[i] = true;
        }
    }
}

void Keymap_Init() {
    const char* pref_path = Paths_GetPrefPath();
    char* keymap_path;
    SDL_asprintf(&keymap_path, "%skeymap", pref_path);

    FILE* f = fopen(keymap_path, "r");

    if (f == NULL) {
        // Key map doesn't exist. Write defaults
        write_defaults(keymap_path);
        SDL_free(keymap_path);
        initialize_empty_buttons();
        return;
    }

    SDL_free(keymap_path);
    dict_read(f, dict_iterator);
    fclose(f);
    initialize_empty_buttons();

    /* Diagnostic: dump parsed keymap so we can verify file→runtime
     * binding. Log to stderr so it lands in launch.log. */
    for (int i = 0; i < KEYMAP_BUTTON_COUNT; i++) {
        SDL_Log("[keymap] %s =", Keymap_GetButtonName(i));
        for (int j = 0; j < KEYMAP_CODES_PER_BUTTON; j++) {
            SDL_Scancode c = keymap[i][j];
            if (c != SDL_SCANCODE_UNKNOWN) {
                SDL_Log("  [%d] %d (%s)", j, (int)c, SDL_GetScancodeName(c));
            }
        }
    }
}

const SDL_Scancode* Keymap_GetScancodes(KeymapButton button) {
    return keymap[button];
}

const SDL_Scancode* Keymap_GetDefaultScancodes(KeymapButton button) {
    return default_keymap[button];
}

void Keymap_SetScancodes(KeymapButton button, const SDL_Scancode* codes, int count) {
    for (int i = 0; i < KEYMAP_CODES_PER_BUTTON; i++) {
        keymap[button][i] = i < count ? codes[i] : SDL_SCANCODE_UNKNOWN;
    }

    /* An editor that binds a button before Keymap_Init() has filled the
     * rest would otherwise have its edit overwritten by the defaults on
     * the next initialize_empty_buttons(). */
    initialized_buttons[button] = true;
}

bool Keymap_Save(void) {
    char* keymap_path = NULL;
    SDL_asprintf(&keymap_path, "%skeymap", Paths_GetPrefPath());

    if (keymap_path == NULL) {
        return false;
    }

    const bool ok = write_table(keymap_path, &keymap[0][0]);
    SDL_free(keymap_path);
    return ok;
}
