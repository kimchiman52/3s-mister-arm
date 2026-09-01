#ifndef KEYMAP_H
#define KEYMAP_H

#include <SDL3/SDL.h>

#include <stdbool.h>

#define KEYMAP_CODES_PER_BUTTON 4
#define KEYMAP_BUTTON_COUNT 16

typedef enum KeymapButton {
    KEYMAP_BUTTON_UP,
    KEYMAP_BUTTON_DOWN,
    KEYMAP_BUTTON_LEFT,
    KEYMAP_BUTTON_RIGHT,
    KEYMAP_BUTTON_NORTH,
    KEYMAP_BUTTON_WEST,
    KEYMAP_BUTTON_SOUTH,
    KEYMAP_BUTTON_EAST,
    KEYMAP_BUTTON_LEFT_SHOULDER,
    KEYMAP_BUTTON_RIGHT_SHOULDER,
    KEYMAP_BUTTON_LEFT_TRIGGER,
    KEYMAP_BUTTON_RIGHT_TRIGGER,
    KEYMAP_BUTTON_LEFT_STICK,
    KEYMAP_BUTTON_RIGHT_STICK,
    KEYMAP_BUTTON_BACK,
    KEYMAP_BUTTON_START,
} KeymapButton;

void Keymap_Init();
const SDL_Scancode* Keymap_GetScancodes(KeymapButton button);

/*
 * Editing API. The game never calls these -- they exist so the desktop
 * launcher's key-config screen edits the one keymap the game will read,
 * instead of shipping a second copy of the defaults table that would
 * drift the moment either side changed.
 *
 * Keymap_Save() rewrites <pref>/keymap from the WHOLE in-memory table,
 * so the buttons an editor chooses not to show still round-trip: after
 * Keymap_Init() the table holds every button, file-supplied or default.
 */
const char* Keymap_GetButtonName(KeymapButton button);
const SDL_Scancode* Keymap_GetDefaultScancodes(KeymapButton button);
void Keymap_SetScancodes(KeymapButton button, const SDL_Scancode* codes, int count);
bool Keymap_Save(void);

#endif
