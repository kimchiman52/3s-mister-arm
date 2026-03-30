#include "port/sdl/sdl_pad.h"
#include "port/config/keymap.h"

#include <SDL3/SDL.h>

#if defined(PORT_MISTER)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "mister_joy_shm.h"
#endif

#define INPUT_SOURCES_MAX 2

typedef enum SDLPad_InputType {
    SDLPAD_INPUT_NONE = 0,
    SDLPAD_INPUT_GAMEPAD,
    SDLPAD_INPUT_KEYBOARD,
#if defined(PORT_MISTER)
    SDLPAD_INPUT_MISTER_SHM,
#endif
} SDLPad_InputType;

typedef struct SDLPad_GamepadInputSource {
    Uint32 type;
    SDL_Gamepad* gamepad;
} SDLPad_GamepadInputSource;

typedef struct SDLPad_KeyboardInputSource {
    Uint32 type;
} SDLPad_KeyboardInputSource;

typedef union SDLPad_InputSource {
    Uint32 type;
    SDLPad_GamepadInputSource gamepad;
    SDLPad_KeyboardInputSource keyboard;
} SDLPad_InputSource;

static SDLPad_InputSource input_sources[INPUT_SOURCES_MAX] = { 0 };
static int connected_input_sources = 0;
static int keyboard_index = -1;
static SDLPad_ButtonState button_state[INPUT_SOURCES_MAX] = { 0 };

#if defined(PORT_MISTER)
static const volatile MisterJoyShm *mister_shm = NULL;
#endif

static int input_source_index_from_joystick_id(SDL_JoystickID id) {
    for (int i = 0; i < INPUT_SOURCES_MAX; i++) {
        const SDLPad_InputSource* input_source = &input_sources[i];

        if (input_source->type != SDLPAD_INPUT_GAMEPAD) {
            continue;
        }

        const SDL_JoystickID this_id = SDL_GetGamepadID(input_source->gamepad.gamepad);

        if (this_id == id) {
            return i;
        }
    }

    return -1;
}

static void setup_keyboard() {
    if (keyboard_index >= 0) {
        return;
    }

    for (int i = 0; i < SDL_arraysize(input_sources); i++) {
        SDLPad_InputSource* input_source = &input_sources[i];

        if (input_source->type == SDLPAD_INPUT_NONE) {
            input_source->type = SDLPAD_INPUT_KEYBOARD;
            keyboard_index = i;
            connected_input_sources += 1;
            break;
        }
    }
}

static void remove_keyboard() {
    if (keyboard_index < 0) {
        return;
    }

    for (int i = 0; i < SDL_arraysize(input_sources); i++) {
        SDLPad_InputSource* input_source = &input_sources[i];

        if (input_source->type == SDLPAD_INPUT_KEYBOARD) {
            input_source->type = SDLPAD_INPUT_NONE;
            keyboard_index = -1;
            connected_input_sources -= 1;
            break;
        }
    }
}

static bool add_gamepad_by_id(SDL_JoystickID id) {
    if (input_source_index_from_joystick_id(id) >= 0) {
        return false;
    }

    // Remove keyboard to potentially make space for the new gamepad
    remove_keyboard();

    if (connected_input_sources >= INPUT_SOURCES_MAX) {
        setup_keyboard();
        return false;
    }

    SDL_Gamepad* gamepad = SDL_OpenGamepad(id);

    if (gamepad == NULL) {
        setup_keyboard();
        return false;
    }

    int assigned_index = -1;

    for (int i = 0; i < INPUT_SOURCES_MAX; i++) {
        SDLPad_InputSource* input_source = &input_sources[i];

        if (input_source->type != SDLPAD_INPUT_NONE) {
            continue;
        }

        input_source->type = SDLPAD_INPUT_GAMEPAD;
        input_source->gamepad.gamepad = gamepad;
        assigned_index = i;
        break;
    }

    if (assigned_index < 0) {
        SDL_CloseGamepad(gamepad);
        setup_keyboard();
        return false;
    }

    connected_input_sources += 1;

    SDL_Log("SDLPad: gamepad added id=%d index=%d name=%s", id, assigned_index, SDL_GetGamepadName(gamepad));

    // Setup keyboard again, if there's a free slot
    setup_keyboard();
    return true;
}

static void handle_gamepad_added_event(SDL_GamepadDeviceEvent* event) {
    add_gamepad_by_id(event->which);
}

static void handle_gamepad_removed_event(SDL_GamepadDeviceEvent* event) {
    const int index = input_source_index_from_joystick_id(event->which);

    if (index < 0) {
        return;
    }

    SDLPad_InputSource* input_source = &input_sources[index];
    SDL_CloseGamepad(input_source->gamepad.gamepad);
    input_source->type = SDLPAD_INPUT_NONE;
    memset(&button_state[index], 0, sizeof(SDLPad_ButtonState));
    connected_input_sources -= 1;

    // Setup keyboard in the newly freed slot
    setup_keyboard();
}

static bool any_pressed(const bool* keys, KeymapButton button) {
    bool result = false;
    const SDL_Scancode* codes = Keymap_GetScancodes(button);

    for (int i = 0; i < KEYMAP_CODES_PER_BUTTON; i++) {
        const SDL_Scancode code = codes[i];

        if (code == SDL_SCANCODE_UNKNOWN) {
            break;
        }

        result = result || keys[code];    
    }

    return result;
}

static void get_keyboard_state(SDLPad_ButtonState* state) {
    SDL_zerop(state);
    const bool* keys = SDL_GetKeyboardState(NULL);

    state->dpad_up = any_pressed(keys, KEYMAP_BUTTON_UP);
    state->dpad_left = any_pressed(keys, KEYMAP_BUTTON_LEFT);
    state->dpad_down = any_pressed(keys, KEYMAP_BUTTON_DOWN);
    state->dpad_right = any_pressed(keys, KEYMAP_BUTTON_RIGHT);
    state->north = any_pressed(keys, KEYMAP_BUTTON_NORTH);
    state->west = any_pressed(keys, KEYMAP_BUTTON_WEST);
    state->south = any_pressed(keys, KEYMAP_BUTTON_SOUTH);
    state->east = any_pressed(keys, KEYMAP_BUTTON_EAST);
    state->left_shoulder = any_pressed(keys, KEYMAP_BUTTON_LEFT_SHOULDER);
    state->right_shoulder = any_pressed(keys, KEYMAP_BUTTON_RIGHT_SHOULDER);
    state->left_trigger = any_pressed(keys, KEYMAP_BUTTON_LEFT_TRIGGER) ? SDL_MAX_SINT16 : 0;
    state->right_trigger = any_pressed(keys, KEYMAP_BUTTON_RIGHT_TRIGGER) ? SDL_MAX_SINT16 : 0;
    state->left_stick = any_pressed(keys, KEYMAP_BUTTON_LEFT_STICK);
    state->right_stick = any_pressed(keys, KEYMAP_BUTTON_RIGHT_STICK);
    state->back = any_pressed(keys, KEYMAP_BUTTON_BACK);
    state->start = any_pressed(keys, KEYMAP_BUTTON_START);

#if DEBUG
    state->right_stick |= keys[SDL_SCANCODE_TAB];
#endif
}

static void get_gamepad_state(int id, SDLPad_ButtonState* state) {
    const SDL_Gamepad* pad = input_sources[id].gamepad.gamepad;

    state->dpad_up = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    state->dpad_left = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    state->dpad_down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    state->dpad_right = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    state->north = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
    state->west = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
    state->south = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
    state->east = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
    state->left_shoulder = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    state->right_shoulder = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    state->left_stick = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    state->right_stick = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    state->back = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
    state->start = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);

    state->left_trigger = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    state->right_trigger = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    state->left_stick_x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    state->left_stick_y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    state->right_stick_x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    state->right_stick_y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
}

#if defined(PORT_MISTER)
static void get_mister_state(int id, SDLPad_ButtonState *state) {
    SDL_zerop(state);
    if (!mister_shm || id < 0 || id >= MISTER_JOY_MAX_PLAYERS) return;

    const uint32_t m = mister_shm->joy_mask[id];

    state->dpad_right      = (m >> 0)  & 1;
    state->dpad_left       = (m >> 1)  & 1;
    state->dpad_down       = (m >> 2)  & 1;
    state->dpad_up         = (m >> 3)  & 1;
    /* MiSTer uses SNES button convention:
       A = right face, B = bottom face, X = top face, Y = left face.
       Map to PS2/SDL pad for 3rd Strike (6-button fighter):
       Y → Square (LP), X → Triangle (MP), L → R1 (HP),
       B → Cross (LK),  A → Circle (MK),  R → R2 (HK). */
    state->east            = (m >> 4)  & 1;   /* A → Circle (MK)    */
    state->south           = (m >> 5)  & 1;   /* B → Cross (LK)     */
    state->north           = (m >> 6)  & 1;   /* X → Triangle (MP)  */
    state->west            = (m >> 7)  & 1;   /* Y → Square (LP)    */
    state->right_shoulder  = (m >> 8)  & 1;   /* L → R1 (HP)        */
    state->right_trigger   = (m >> 9) & 1 ? SDL_MAX_SINT16 : 0; /* R → R2 (HK) */
    state->back            = (m >> 10) & 1;
    state->start           = (m >> 11) & 1;

    state->left_stick_x    = (Sint16)mister_shm->left_stick_x[id]  * 256;
    state->left_stick_y    = (Sint16)mister_shm->left_stick_y[id]  * 256;
    state->right_stick_x   = (Sint16)mister_shm->right_stick_x[id] * 256;
    state->right_stick_y   = (Sint16)mister_shm->right_stick_y[id] * 256;
}
#endif

void SDLPad_Init() {
#if defined(PORT_MISTER)
    const char *shm_path = SDL_getenv("THREESX_JOY_SHM");
    if (shm_path) {
        int fd = open(shm_path, O_RDONLY);
        if (fd >= 0) {
            const MisterJoyShm *shm = (const MisterJoyShm *)mmap(
                NULL, sizeof(MisterJoyShm), PROT_READ, MAP_SHARED, fd, 0);
            close(fd);
            if (shm != MAP_FAILED
                && shm->magic == MISTER_JOY_SHM_MAGIC
                && shm->version == MISTER_JOY_SHM_VERSION) {
                mister_shm = shm;
                for (int i = 0; i < INPUT_SOURCES_MAX; i++) {
                    input_sources[i].type = SDLPAD_INPUT_MISTER_SHM;
                    connected_input_sources++;
                }
                SDL_Log("SDLPad: using MiSTer shared-memory joystick input");
                return;
            }
            if (shm != MAP_FAILED) munmap((void *)shm, sizeof(MisterJoyShm));
        }
    }
#endif

    setup_keyboard();

    int gamepad_count = 0;
    SDL_JoystickID* gamepad_ids = SDL_GetGamepads(&gamepad_count);

    if (gamepad_ids == NULL) {
        return;
    }

    for (int i = 0; i < gamepad_count; i++) {
        add_gamepad_by_id(gamepad_ids[i]);
    }

    SDL_free(gamepad_ids);
}

void SDLPad_HandleGamepadDeviceEvent(SDL_GamepadDeviceEvent* event) {
    switch (event->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        handle_gamepad_added_event(event);
        break;

    case SDL_EVENT_GAMEPAD_REMOVED:
        handle_gamepad_removed_event(event);
        break;

    default:
        // Do nothing
        break;
    }
}

bool SDLPad_IsGamepadConnected(int id) {
#if defined(PORT_MISTER)
    if (mister_shm && id >= 0 && id < MISTER_JOY_MAX_PLAYERS) return true;
#endif
    return input_sources[id].type != SDLPAD_INPUT_NONE;
}

void SDLPad_GetButtonState(int id, SDLPad_ButtonState* state) {
#if defined(PORT_MISTER)
    if (mister_shm) { get_mister_state(id, state); return; }
#endif
    if (id == keyboard_index) {
        get_keyboard_state(state);
    } else {
        get_gamepad_state(id, state);
    }
}

void SDLPad_RumblePad(int id, bool low_freq_enabled, Uint8 high_freq_rumble) {
    const SDLPad_InputSource* input_source = &input_sources[id];

    if (input_source->type != SDLPAD_INPUT_GAMEPAD) {
        return;
    }

    const Uint16 low_freq_rumble = low_freq_enabled ? UINT16_MAX : 0;
    const Uint16 high_freq_rumble_adjusted = ((float)high_freq_rumble / UINT8_MAX) * UINT16_MAX;
    const Uint32 duration = high_freq_rumble_adjusted > 0 ? 500 : 200;

    SDL_RumbleGamepad(input_source->gamepad.gamepad, low_freq_rumble, high_freq_rumble_adjusted, duration);
}
