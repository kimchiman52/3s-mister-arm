#include "port/config/training_config.h"
#include "port/paths.h"
#include "structs.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/engine/workuser.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#define TRAINING_CONFIG_MAGIC   0x54524E31  // "TRN1"
#define TRAINING_CONFIG_VERSION 1

typedef struct {
    u32 magic;
    u32 version;
    s8 contents[2][2][6];
    s8 cursor_x[2];
    s8 cursor_y[2];
    s8 super_arts[2];
    u8 my_char[2];
} TrainingConfigFile;

_Static_assert(sizeof(TrainingConfigFile) == 40, "TrainingConfigFile must be 40 bytes");

// Must match first 6 columns of Menu_Max_Data_Tr[2][2][8] in menu.c
static const s8 max_values[2][2][6] = {
    { { 4, 6, 2, 2, 0, 0 }, { 3, 1, 3, 7, 1, 1 } },
    { { 2, 3, 1, 3, 0, 0 }, { 0, 0, 0, 0, 0, 0 } }
};

bool TrainingConfig_Load(void) {
    const char* pref_path = Paths_GetPrefPath();
    if (pref_path == NULL) {
        return false;
    }

    char path[512];
    SDL_snprintf(path, sizeof(path), "%straining", pref_path);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    TrainingConfigFile file;
    if (fread(&file, sizeof(file), 1, f) != 1) {
        fclose(f);
        return false;
    }
    fclose(f);

    if (file.magic != TRAINING_CONFIG_MAGIC || file.version != TRAINING_CONFIG_VERSION) {
        return false;
    }

    // Bounds-check loaded values; clamp anything out of range to 0
    for (int id = 0; id < 2; id++) {
        for (int type = 0; type < 2; type++) {
            for (int slot = 0; slot < 6; slot++) {
                if (file.contents[id][type][slot] < 0 ||
                    file.contents[id][type][slot] > max_values[id][type][slot]) {
                    file.contents[id][type][slot] = 0;
                }
            }
        }
    }

    memcpy(Training[0].contents, file.contents, sizeof(file.contents));
    memcpy(Training[2].contents, file.contents, sizeof(file.contents));

    return true;
}

void TrainingConfig_Save(void) {
    const char* pref_path = Paths_GetPrefPath();
    if (pref_path == NULL) {
        return;
    }

    char path[512];
    SDL_snprintf(path, sizeof(path), "%straining", pref_path);

    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }

    TrainingConfigFile file;
    file.magic = TRAINING_CONFIG_MAGIC;
    file.version = TRAINING_CONFIG_VERSION;
    memcpy(file.contents, Training[2].contents, sizeof(file.contents));
    memcpy(file.cursor_x, Cursor_X, sizeof(file.cursor_x));
    memcpy(file.cursor_y, Cursor_Y, sizeof(file.cursor_y));
    memcpy(file.super_arts, Super_Arts, sizeof(file.super_arts));
    memcpy(file.my_char, My_char, sizeof(file.my_char));

    if (fwrite(&file, sizeof(file), 1, f) != 1) {
        fclose(f);
        remove(path);
        return;
    }
    fclose(f);
}

void TrainingConfig_RestoreCharSelect(void) {
    const char* pref_path = Paths_GetPrefPath();
    if (pref_path == NULL) {
        return;
    }

    char path[512];
    SDL_snprintf(path, sizeof(path), "%straining", pref_path);

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }

    TrainingConfigFile file;
    if (fread(&file, sizeof(file), 1, f) != 1) {
        fclose(f);
        return;
    }
    fclose(f);

    if (file.magic != TRAINING_CONFIG_MAGIC || file.version != TRAINING_CONFIG_VERSION) {
        return;
    }

    // Bounds-check cursor positions (grid is 8 columns x 3 rows)
    for (int i = 0; i < 2; i++) {
        if (file.cursor_x[i] >= 0 && file.cursor_x[i] < 8) {
            Cursor_X[i] = file.cursor_x[i];
        }
        if (file.cursor_y[i] >= 0 && file.cursor_y[i] < 3) {
            Cursor_Y[i] = file.cursor_y[i];
        }
        if (file.super_arts[i] >= 0 && file.super_arts[i] < 3) {
            Arts_Y[i] = file.super_arts[i];
            Last_Super_Arts[i] = file.super_arts[i];
        }
        // Set Last_My_char2 so sel_pl.c sees "same character" and doesn't reset Arts_Y
        Last_My_char2[i] = file.my_char[i];
    }
}
