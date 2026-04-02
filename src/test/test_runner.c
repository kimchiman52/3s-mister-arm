#if DEBUG

#include "test/test_runner.h"
#include "arcade/arcade_constants.h"
#include "constants.h"
#include "main.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/debug/debug_config.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/spgauge.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"
#include "test/replay_game.h"
#include "test/test_runner_compare.h"
#include "test/test_runner_utils.h"

#include "stb/stb_ds.h"
#include <SDL3/SDL.h>

#include <signal.h>
#include <stdio.h>

#define MY_CHAR_OFFSET 0x11387
#define SUPER_ARTS_OFFSET 0x1138B
#define GAME_ROUTINE_OFFSET 0x15438
#define P1SW_OFFSET 0x6AA8C
#define P2SW_OFFSET 0x6AA90

#define REPLAY_FRAMES_MAX 3 * 100 * 60

#define SWAP16(val) ((val << 8) | (val >> 8))

typedef enum Character {
    CHAR_GILL = 0,
    CHAR_ALEX = 1,
    CHAR_RYU = 2,
    CHAR_YUN = 3,
    CHAR_DUDLEY = 4,
    CHAR_NECRO = 5,
    CHAR_HUGO = 6,
    CHAR_IBUKI = 7,
    CHAR_ELENA = 8,
    CHAR_ORO = 9,
    CHAR_YANG = 10,
    CHAR_KEN = 11,
    CHAR_SEAN = 12,
    CHAR_URIEN = 13,
    CHAR_AKUMA = 14,
    CHAR_CHUNLI = 15,
    CHAR_MAKOTO = 16,
    CHAR_Q = 17,
    CHAR_TWELVE = 18,
    CHAR_REMY = 19,
} Character;

typedef enum Phase {
    PHASE_INIT,
    PHASE_TITLE,
    PHASE_MENU,
    PHASE_CHARACTER_SELECT_TRANSITION,
    PHASE_CHARACTER_SELECT,
    PHASE_GAME_TRANSITION,
    PHASE_GAME,
} Phase;

typedef enum TestScenePreset {
    TEST_SCENE_PRESET_NONE,
    TEST_SCENE_PRESET_STAGE_HEAVY,
    TEST_SCENE_PRESET_EFFECT_HEAVY,
    TEST_SCENE_PRESET_SUPER_HEAVY,
    TEST_SCENE_PRESET_YUN_SA3_REPEAT,
    TEST_SCENE_PRESET_YUN_SA3_REPEAT_PRESSURE,
    TEST_SCENE_PRESET_Q_SA1_REPEAT,
    TEST_SCENE_PRESET_Q_SA1_REPEAT_PRESSURE,
    TEST_SCENE_PRESET_KEN_SA3_REPEAT,
    TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE,
    TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT,
    TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE,
    TEST_SCENE_PRESET_BASIC_EXCHANGE,
    TEST_SCENE_PRESET_PRESSURE_EXCHANGE,
    TEST_SCENE_PRESET_LEFT_CORNER_RYU_STAGE,
    TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE,
} TestScenePreset;

static const Uint8 character_to_cursor[20][2] = { { 7, 1 }, { 1, 0 }, { 5, 2 }, { 6, 1 }, { 3, 2 }, { 4, 0 }, { 1, 2 },
                                                  { 3, 0 }, { 2, 2 }, { 4, 2 }, { 0, 1 }, { 0, 2 }, { 2, 0 }, { 5, 0 },
                                                  { 6, 0 }, { 3, 1 }, { 2, 1 }, { 4, 1 }, { 1, 1 }, { 5, 1 } };
static const Sint8 scene_preset_basic_exchange_stage = 11;
static const Sint8 scene_preset_stage_heavy_stage = 19;
static const Sint8 scene_preset_training_yun_ryu_ryu_stage = 2;
static const int scene_preset_repeat_first_super_frame = 150;
static const int scene_preset_repeat_second_super_frame = 620;
static const int scene_preset_repeat_super_input_frames = 40;
static const int scene_preset_repeat_super_prep_frames = 32;
static const int scene_preset_repeat_second_super_refill_frame = 560;

static Uint64 frame = 0;
static Phase phase = PHASE_INIT;
static int char_select_phase = 0;
static int wait_timer = 0;
static int game_frame = 0;
static Sint8 characters[2] = { -1, -1 };
static Sint8 selected_super_arts[2] = { -1, -1 };
static Sint8 stage = -1;
static TestScenePreset scene_preset = TEST_SCENE_PRESET_NONE;
static int player_super_art_activation_starts[2] = { 0, 0 };
static bool player_super_art_was_active[2] = { false, false };
static bool scene_preset_repeat_second_super_ready = false;
static u16 inputs[REPLAY_FRAMES_MAX][2] = { 0 };
static int inputs_index = 0;
static int inputs_total = 0;

static const char* phase_name(Phase current_phase) {
    switch (current_phase) {
    case PHASE_INIT:
        return "init";
    case PHASE_TITLE:
        return "title";
    case PHASE_MENU:
        return "menu";
    case PHASE_CHARACTER_SELECT_TRANSITION:
        return "character-select-transition";
    case PHASE_CHARACTER_SELECT:
        return "character-select";
    case PHASE_GAME_TRANSITION:
        return "game-transition";
    case PHASE_GAME:
        return "game";
    }

    return "unknown";
}

static bool gameplay_input_active(void) {
    return phase == PHASE_GAME && plw[0].wu.routine_no[0] == 4 && plw[1].wu.routine_no[0] == 4;
}

static bool player_super_art_active(int player) {
    return phase == PHASE_GAME && player >= 0 && player < 2 && plw[player].sa != NULL && plw[player].sa->ok == -1;
}

static bool wipe_transition_type1_active(void) {
    return Exec_Wipe != 0 && Active_Wipe_Type == 1 && WipeLimit < 8;
}

static TestScenePreset resolve_scene_preset(const char* preset_name) {
    if (preset_name == NULL || preset_name[0] == '\0') {
        return TEST_SCENE_PRESET_NONE;
    }

    if (SDL_strcmp(preset_name, "stage-heavy") == 0) {
        return TEST_SCENE_PRESET_STAGE_HEAVY;
    }
    if (SDL_strcmp(preset_name, "effect-heavy") == 0) {
        return TEST_SCENE_PRESET_EFFECT_HEAVY;
    }
    if (SDL_strcmp(preset_name, "super-heavy") == 0) {
        return TEST_SCENE_PRESET_SUPER_HEAVY;
    }
    if (SDL_strcmp(preset_name, "yun-sa3-repeat") == 0) {
        return TEST_SCENE_PRESET_YUN_SA3_REPEAT;
    }
    if (SDL_strcmp(preset_name, "yun-sa3-repeat-pressure") == 0) {
        return TEST_SCENE_PRESET_YUN_SA3_REPEAT_PRESSURE;
    }
    if (SDL_strcmp(preset_name, "q-sa1-repeat") == 0) {
        return TEST_SCENE_PRESET_Q_SA1_REPEAT;
    }
    if (SDL_strcmp(preset_name, "q-sa1-repeat-pressure") == 0) {
        return TEST_SCENE_PRESET_Q_SA1_REPEAT_PRESSURE;
    }
    if (SDL_strcmp(preset_name, "ken-sa3-repeat") == 0) {
        return TEST_SCENE_PRESET_KEN_SA3_REPEAT;
    }
    if (SDL_strcmp(preset_name, "ken-sa3-repeat-pressure") == 0) {
        return TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE;
    }
    if (SDL_strcmp(preset_name, "chunli-sa2-repeat") == 0) {
        return TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT;
    }
    if (SDL_strcmp(preset_name, "chunli-sa2-repeat-pressure") == 0) {
        return TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE;
    }
    if (SDL_strcmp(preset_name, "basic-exchange") == 0) {
        return TEST_SCENE_PRESET_BASIC_EXCHANGE;
    }
    if (SDL_strcmp(preset_name, "pressure-exchange") == 0) {
        return TEST_SCENE_PRESET_PRESSURE_EXCHANGE;
    }
    if (SDL_strcmp(preset_name, "left-corner-ryu-stage") == 0) {
        return TEST_SCENE_PRESET_LEFT_CORNER_RYU_STAGE;
    }
    if (SDL_strcmp(preset_name, "training-yun-ryu-ryu-stage") == 0) {
        return TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE;
    }

    return TEST_SCENE_PRESET_NONE;
}

bool TestRunner_IsSupportedPhaseName(const char* phase_name_value) {
    if (phase_name_value == NULL || phase_name_value[0] == '\0') {
        return false;
    }

    return SDL_strcmp(phase_name_value, "init") == 0 || SDL_strcmp(phase_name_value, "title") == 0 ||
           SDL_strcmp(phase_name_value, "menu") == 0 ||
           SDL_strcmp(phase_name_value, "character-select-transition") == 0 ||
           SDL_strcmp(phase_name_value, "character-select") == 0 ||
           SDL_strcmp(phase_name_value, "game-transition") == 0 || SDL_strcmp(phase_name_value, "game") == 0 ||
           SDL_strcmp(phase_name_value, "game-input-active") == 0 ||
           SDL_strcmp(phase_name_value, "p1-super-art-active") == 0 ||
           SDL_strcmp(phase_name_value, "p1-super-art-active-2") == 0 ||
           SDL_strcmp(phase_name_value, "wipe-transition-type1") == 0;
}

const char* TestRunner_GetPhaseName(void) {
    return phase_name(phase);
}

bool TestRunner_IsPhaseActive(const char* phase_name_value) {
    if (!TestRunner_IsSupportedPhaseName(phase_name_value)) {
        return false;
    }

    if (SDL_strcmp(phase_name_value, "game") == 0) {
        return phase == PHASE_GAME;
    }

    if (SDL_strcmp(phase_name_value, "game-input-active") == 0) {
        return gameplay_input_active();
    }

    if (SDL_strcmp(phase_name_value, "p1-super-art-active") == 0) {
        return player_super_art_active(0);
    }

    if (SDL_strcmp(phase_name_value, "p1-super-art-active-2") == 0) {
        return player_super_art_active(0) && player_super_art_activation_starts[0] >= 2;
    }

    if (SDL_strcmp(phase_name_value, "wipe-transition-type1") == 0) {
        return wipe_transition_type1_active();
    }

    return SDL_strcmp(TestRunner_GetPhaseName(), phase_name_value) == 0;
}

static SWKey player_forward_button(int player) {
    return player ? SWK_LEFT : SWK_RIGHT;
}

static u16 projectile_script_input(int player, int local_frame) {
    const SWKey forward = player_forward_button(player);

    switch (local_frame) {
    case 0:
    case 1:
    case 2:
        return SWK_DOWN;
    case 3:
    case 4:
    case 5:
        return (u16)(SWK_DOWN | forward);
    case 6:
    case 7:
        return forward;
    case 8:
        return (u16)(forward | SWK_WEST);
    default:
        return 0;
    }
}

static u16 super_script_input_with_button(int player, int local_frame, SWKey attack_button) {
    const SWKey forward = player_forward_button(player);

    switch (local_frame) {
    case 0:
    case 1:
    case 2:
    case 3:
        return SWK_DOWN;
    case 4:
    case 5:
    case 6:
    case 7:
        return (u16)(SWK_DOWN | forward);
    case 8:
    case 9:
    case 10:
    case 11:
        return forward;
    case 16:
    case 17:
    case 18:
    case 19:
        return SWK_DOWN;
    case 20:
    case 21:
    case 22:
    case 23:
        return (u16)(SWK_DOWN | forward);
    case 24:
    case 25:
    case 26:
    case 27:
        return forward;
    case 30:
        return (u16)(forward | attack_button);
    default:
        return 0;
    }
}

static u16 super_script_input(int player, int local_frame) {
    return super_script_input_with_button(player, local_frame, SWK_WEST);
}

static u16 basic_exchange_script_input(int player, int local_frame) {
    const SWKey forward = player_forward_button(player);
    const SWKey backward = player ? SWK_RIGHT : SWK_LEFT;
    const int cycle_frame = local_frame % 96;

    switch (cycle_frame) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
        return forward;
    case 16:
    case 17:
    case 18:
    case 19:
        return (u16)(SWK_DOWN | backward);
    case 20:
    case 21:
        return SWK_SOUTH;
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
        return (u16)(SWK_UP | forward);
    case 36:
    case 37:
        return (u16)(SWK_UP | forward | SWK_SOUTH);
    case 42:
    case 43:
    case 44:
    case 45:
        return (u16)(SWK_DOWN | SWK_WEST);
    case 56:
    case 57:
    case 58:
    case 59:
    case 60:
    case 61:
    case 62:
    case 63:
        return backward;
    case 64:
    case 65:
    case 66:
    case 67:
    case 68:
    case 69:
    case 70:
    case 71:
        return forward;
    case 72:
    case 73:
        return SWK_WEST;
    default:
        return 0;
    }
}

static u16 pressure_exchange_attack_input(int player, int local_frame) {
    const SWKey forward = player_forward_button(player);

    switch (local_frame) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
        return forward;
    case 12:
    case 13:
        return SWK_SOUTH;
    case 20:
    case 21:
    case 22:
    case 23:
        return (u16)(SWK_DOWN | SWK_WEST);
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
        return (u16)(SWK_UP | forward);
    case 38:
    case 39:
        return (u16)(SWK_UP | forward | SWK_SOUTH);
    case 48:
    case 49:
        return SWK_WEST;
    case 58:
    case 59:
    case 60:
    case 61:
    case 62:
    case 63:
        return forward;
    case 64:
    case 65:
    case 66:
    case 67:
        return (u16)(SWK_DOWN | SWK_WEST);
    default:
        return 0;
    }
}

static u16 pressure_exchange_defend_input(int player, int local_frame) {
    const SWKey forward = player_forward_button(player);
    const SWKey backward = player ? SWK_RIGHT : SWK_LEFT;

    switch (local_frame) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        return forward;
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
        return (u16)(SWK_DOWN | backward);
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
        return backward;
    case 38:
    case 39:
    case 40:
    case 41:
    case 42:
    case 43:
    case 44:
    case 45:
        return (u16)(SWK_UP | backward);
    case 52:
    case 53:
    case 54:
    case 55:
        return backward;
    case 60:
    case 61:
        return SWK_SOUTH;
    case 66:
    case 67:
    case 68:
    case 69:
        return (u16)(SWK_DOWN | backward);
    default:
        return 0;
    }
}

static u16 pressure_exchange_script_input(int player, int local_frame) {
    const int cycle_frame = local_frame % 144;
    const bool p1_pressure_turn = cycle_frame < 72;
    const int turn_frame = p1_pressure_turn ? cycle_frame : (cycle_frame - 72);
    const bool is_attacker = (player == 0) ? p1_pressure_turn : !p1_pressure_turn;

    if (is_attacker) {
        return pressure_exchange_attack_input(player, turn_frame);
    }

    return pressure_exchange_defend_input(player, turn_frame);
}

static bool scene_preset_uses_repeat_second_super(void) {
    switch (scene_preset) {
    case TEST_SCENE_PRESET_YUN_SA3_REPEAT:
    case TEST_SCENE_PRESET_YUN_SA3_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_Q_SA1_REPEAT:
    case TEST_SCENE_PRESET_Q_SA1_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT:
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE:
        return true;
    default:
        return false;
    }
}

static bool scene_preset_uses_repeat_pressure(void) {
    switch (scene_preset) {
    case TEST_SCENE_PRESET_YUN_SA3_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_Q_SA1_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE:
        return true;
    default:
        return false;
    }
}

static bool scene_preset_repeat_super_input_active(int start_frame) {
    return game_frame >= start_frame && game_frame < (start_frame + scene_preset_repeat_super_input_frames);
}

static bool scene_preset_repeat_super_prep_active(int start_frame) {
    const int prep_start = start_frame - scene_preset_repeat_super_prep_frames;

    return game_frame >= prep_start && game_frame < start_frame;
}

static SWKey scene_preset_repeat_super_attack_button(void) {
    switch (scene_preset) {
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT:
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE:
        return SWK_SOUTH;
    default:
        return SWK_WEST;
    }
}

static u16 left_corner_ryu_stage_reanchor_input(int player) {
    return player == 0 ? SWK_LEFT : SWK_LEFT;
}

static bool left_corner_ryu_stage_needs_reanchor(void) {
    const int p1_x = plw[0].wu.position_x;
    const int p2_x = plw[1].wu.position_x;

    return p1_x > 144 || p2_x > 224 || (p2_x - p1_x) > 96;
}

static u16 left_corner_ryu_stage_script_input(int player, int local_frame) {
    const int cycle_frame = local_frame % 192;

    // Keep the fight biased into the left corner, then reuse the existing exchange script
    // so both players still trade attacks once they get there.
    if (cycle_frame < 48 || left_corner_ryu_stage_needs_reanchor()) {
        return left_corner_ryu_stage_reanchor_input(player);
    }

    if (cycle_frame < 64) {
        return (u16)(left_corner_ryu_stage_reanchor_input(player) | (player == 0 ? SWK_DOWN : 0));
    }

    return pressure_exchange_script_input(player, cycle_frame - 64);
}

static u16 training_yun_ryu_ryu_stage_script_input(int player, int local_frame) {
    if (player != 0) {
        return 0;
    }

    switch (local_frame % 192) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
        return SWK_RIGHT;
    case 36:
    case 37:
        return SWK_SOUTH;
    case 44:
    case 45:
        return SWK_WEST;
    case 56:
    case 57:
    case 58:
    case 59:
    case 60:
    case 61:
    case 62:
    case 63:
        return SWK_RIGHT;
    case 64:
    case 65:
        return (u16)(SWK_RIGHT | SWK_SOUTH);
    case 72:
    case 73:
    case 74:
    case 75:
    case 76:
    case 77:
        return (u16)(SWK_UP | SWK_RIGHT);
    case 78:
    case 79:
        return (u16)(SWK_UP | SWK_RIGHT | SWK_SOUTH);
    case 92:
    case 93:
        return SWK_WEST;
    case 108:
    case 109:
    case 110:
    case 111:
    case 112:
    case 113:
    case 114:
    case 115:
    case 116:
    case 117:
    case 118:
    case 119:
        return SWK_RIGHT;
    case 120:
    case 121:
        return (u16)(SWK_DOWN | SWK_WEST);
    case 130:
    case 131:
        return SWK_SOUTH;
    case 142:
    case 143:
    case 144:
    case 145:
    case 146:
    case 147:
    case 148:
    case 149:
        return SWK_LEFT;
    case 156:
    case 157:
    case 158:
    case 159:
    case 160:
    case 161:
    case 162:
    case 163:
        return SWK_RIGHT;
    case 164:
    case 165:
        return SWK_WEST;
    default:
        return 0;
    }
}

static bool scene_preset_uses_training_mode(void) {
    return scene_preset == TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE;
}

static bool training_mode_gameplay_started(void) {
    return scene_preset_uses_training_mode() && Mode_Type == MODE_NORMAL_TRAINING && Allow_a_battle_f != 0 && Game_pause == 0 &&
           Pause_Down == 0;
}

static void maybe_force_training_scene_character_and_super_state(void) {
    if (scene_preset != TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE) {
        return;
    }

    for (int player = 0; player < 2; player++) {
        if (Sel_Arts_Complete[player] != 0) {
            continue;
        }

        if (My_char[player] != characters[player]) {
            My_char[player] = characters[player];
            Last_My_char2[player] = characters[player];
        }

        if (selected_super_arts[player] >= 0 && Arts_Y[player] != selected_super_arts[player]) {
            Arts_Y[player] = selected_super_arts[player];
            Super_Arts[player] = selected_super_arts[player];
            Last_Super_Arts[player] = selected_super_arts[player];
        }
    }
}

static void maybe_force_training_scene_super_confirm(void) {
    if (scene_preset != TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE) {
        return;
    }

    for (int player = 0; player < 2; player++) {
        if (Sel_Arts_Complete[player] != 0 || My_char[player] != characters[player]) {
            continue;
        }

        Slide_Type = player;
        Sel_Arts_Complete[player] = 1;
        Last_Super_Arts[player] = Arts_Y[player];
        Super_Arts[player] = Arts_Y[player];
        if (Used_char[player] != My_char[player]) {
            Last_Player_id = player;
        }
        Used_char[player] = My_char[player];
        Setup_ID();
    }
}

static void apply_repeat_super_preset_defaults(Character player1_character, Sint8 player1_super_art) {
    characters[0] = player1_character;
    characters[1] = CHAR_RYU;
    selected_super_arts[0] = player1_super_art;
    selected_super_arts[1] = 0;
    stage = scene_preset_stage_heavy_stage;
}

static void apply_scene_preset_defaults() {
    scene_preset = resolve_scene_preset(configuration.test.scene_preset);

    switch (scene_preset) {
    case TEST_SCENE_PRESET_STAGE_HEAVY:
        stage = scene_preset_stage_heavy_stage;
        break;

    case TEST_SCENE_PRESET_EFFECT_HEAVY:
        characters[0] = CHAR_RYU;
        characters[1] = CHAR_KEN;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_stage_heavy_stage;
        break;

    case TEST_SCENE_PRESET_SUPER_HEAVY:
        characters[0] = CHAR_RYU;
        characters[1] = CHAR_RYU;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_stage_heavy_stage;
        break;

    case TEST_SCENE_PRESET_YUN_SA3_REPEAT:
    case TEST_SCENE_PRESET_YUN_SA3_REPEAT_PRESSURE:
        apply_repeat_super_preset_defaults(CHAR_YUN, 2);
        break;

    case TEST_SCENE_PRESET_Q_SA1_REPEAT:
    case TEST_SCENE_PRESET_Q_SA1_REPEAT_PRESSURE:
        apply_repeat_super_preset_defaults(CHAR_Q, 0);
        break;

    case TEST_SCENE_PRESET_KEN_SA3_REPEAT:
    case TEST_SCENE_PRESET_KEN_SA3_REPEAT_PRESSURE:
        apply_repeat_super_preset_defaults(CHAR_KEN, 2);
        break;

    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT:
    case TEST_SCENE_PRESET_CHUNLI_SA2_REPEAT_PRESSURE:
        apply_repeat_super_preset_defaults(CHAR_CHUNLI, 1);
        break;

    case TEST_SCENE_PRESET_BASIC_EXCHANGE:
        characters[0] = CHAR_RYU;
        characters[1] = CHAR_KEN;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_basic_exchange_stage;
        break;

    case TEST_SCENE_PRESET_PRESSURE_EXCHANGE:
        characters[0] = CHAR_RYU;
        characters[1] = CHAR_KEN;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_stage_heavy_stage;
        break;

    case TEST_SCENE_PRESET_LEFT_CORNER_RYU_STAGE:
        characters[0] = CHAR_RYU;
        characters[1] = CHAR_KEN;
        selected_super_arts[0] = 0;
        selected_super_arts[1] = 0;
        stage = scene_preset_basic_exchange_stage;
        break;

    case TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE:
        characters[0] = CHAR_YUN;
        characters[1] = CHAR_RYU;
        selected_super_arts[0] = 2;
        selected_super_arts[1] = 0;
        stage = scene_preset_training_yun_ryu_ryu_stage;
        break;

    case TEST_SCENE_PRESET_NONE:
        break;
    }
}

static void apply_scene_preset_inputs() {
    if (scene_preset == TEST_SCENE_PRESET_NONE || scene_preset == TEST_SCENE_PRESET_STAGE_HEAVY) {
        p1sw_buff = 0;
        p2sw_buff = 0;
        return;
    }

    if (scene_preset == TEST_SCENE_PRESET_EFFECT_HEAVY) {
        const int cycle_length = 48;
        p1sw_buff = projectile_script_input(0, game_frame % cycle_length);
        p2sw_buff = projectile_script_input(1, (game_frame + 24) % cycle_length);
        return;
    }

    if (scene_preset_uses_repeat_second_super() && !scene_preset_uses_repeat_pressure()) {
        p1sw_buff = 0;
        p2sw_buff = 0;

        if (scene_preset_repeat_super_input_active(scene_preset_repeat_first_super_frame)) {
            p1sw_buff = super_script_input_with_button(
                0, game_frame - scene_preset_repeat_first_super_frame, scene_preset_repeat_super_attack_button());
        }

        if (scene_preset_repeat_super_input_active(scene_preset_repeat_second_super_frame)) {
            p1sw_buff = super_script_input_with_button(
                0, game_frame - scene_preset_repeat_second_super_frame, scene_preset_repeat_super_attack_button());
        }
        return;
    }

    if (scene_preset_uses_repeat_pressure()) {
        p1sw_buff = pressure_exchange_script_input(0, game_frame);
        p2sw_buff = pressure_exchange_script_input(1, game_frame);

        if (scene_preset_repeat_super_prep_active(scene_preset_repeat_first_super_frame)) {
            p1sw_buff = 0;
            p2sw_buff = pressure_exchange_defend_input(
                1, game_frame - (scene_preset_repeat_first_super_frame - scene_preset_repeat_super_prep_frames));
            return;
        }

        if (scene_preset_repeat_super_input_active(scene_preset_repeat_first_super_frame)) {
            p1sw_buff = super_script_input_with_button(
                0, game_frame - scene_preset_repeat_first_super_frame, scene_preset_repeat_super_attack_button());
            p2sw_buff = pressure_exchange_defend_input(
                1, game_frame - (scene_preset_repeat_first_super_frame - scene_preset_repeat_super_prep_frames));
            return;
        }

        if (scene_preset_repeat_super_prep_active(scene_preset_repeat_second_super_frame)) {
            p1sw_buff = 0;
            p2sw_buff = pressure_exchange_defend_input(
                1, game_frame - (scene_preset_repeat_second_super_frame - scene_preset_repeat_super_prep_frames));
            return;
        }

        if (scene_preset_repeat_super_input_active(scene_preset_repeat_second_super_frame)) {
            p1sw_buff = super_script_input_with_button(
                0, game_frame - scene_preset_repeat_second_super_frame, scene_preset_repeat_super_attack_button());
            p2sw_buff = pressure_exchange_defend_input(
                1, game_frame - (scene_preset_repeat_second_super_frame - scene_preset_repeat_super_prep_frames));
            return;
        }

        return;
    }

    if (scene_preset == TEST_SCENE_PRESET_BASIC_EXCHANGE) {
        p1sw_buff = basic_exchange_script_input(0, game_frame);
        p2sw_buff = basic_exchange_script_input(1, game_frame);
        return;
    }

    if (scene_preset == TEST_SCENE_PRESET_PRESSURE_EXCHANGE) {
        p1sw_buff = pressure_exchange_script_input(0, game_frame);
        p2sw_buff = pressure_exchange_script_input(1, game_frame);
        return;
    }

    if (scene_preset == TEST_SCENE_PRESET_LEFT_CORNER_RYU_STAGE) {
        p1sw_buff = left_corner_ryu_stage_script_input(0, game_frame);
        p2sw_buff = left_corner_ryu_stage_script_input(1, game_frame);
        return;
    }

    if (scene_preset == TEST_SCENE_PRESET_TRAINING_YUN_RYU_RYU_STAGE) {
        p1sw_buff = training_yun_ryu_ryu_stage_script_input(0, game_frame);
        p2sw_buff = training_yun_ryu_ryu_stage_script_input(1, game_frame);
        return;
    }

    p1sw_buff = projectile_script_input(0, (game_frame + 12) % 56);
    p2sw_buff = projectile_script_input(1, (game_frame + 36) % 56);

    if (game_frame >= 150 && game_frame < 190) {
        p1sw_buff = super_script_input(0, game_frame - 150);
    }

    if (game_frame >= 240 && game_frame < 280) {
        p2sw_buff = super_script_input(1, game_frame - 240);
    }
}

static void apply_initial_super_full_overrides() {
    if (game_frame > 2) {
        return;
    }

    if (!configuration.test.initial_super_full) {
        return;
    }

    if (plw[0].sa != NULL && plw[0].sa->store > 0) {
        return;
    }

    if (My_char[0] >= 0 && Super_Arts[0] >= 0) {
        tr_spgauge_cont_init2(0);
    }
}

static void apply_scene_preset_super_refill_overrides() {
    if (!scene_preset_uses_repeat_second_super() || scene_preset_repeat_second_super_ready) {
        return;
    }

    if (game_frame < scene_preset_repeat_second_super_refill_frame || plw[0].sa == NULL || player_super_art_active(0)) {
        return;
    }

    if (plw[0].sa->store == 0 && My_char[0] >= 0 && Super_Arts[0] >= 0) {
        tr_spgauge_cont_init2(0);
    }
    scene_preset_repeat_second_super_ready = true;
}

static void initialize_default_data() {
    characters[0] = CHAR_RYU;
    characters[1] = CHAR_KEN;
    selected_super_arts[0] = 0;
    selected_super_arts[1] = 0;
    stage = -1;
    scene_preset = TEST_SCENE_PRESET_NONE;
    Debug_w[DEBUG_STAGE_SELECT] = 0;
    game_frame = 0;
    player_super_art_activation_starts[0] = 0;
    player_super_art_activation_starts[1] = 0;
    player_super_art_was_active[0] = false;
    player_super_art_was_active[1] = false;
    scene_preset_repeat_second_super_ready = false;

    apply_scene_preset_defaults();

    for (int player = 0; player < 2; player++) {
        if (configuration.test.characters[player] >= 0) {
            characters[player] = (Sint8)configuration.test.characters[player];
        }

        if (configuration.test.super_arts[player] >= 0) {
            selected_super_arts[player] = (Sint8)configuration.test.super_arts[player];
        }
    }

    if (configuration.test.stage >= 0) {
        stage = (Sint8)configuration.test.stage;
    }

    inputs_index = 0;
    inputs_total = 0;
}

static void apply_stage_override() {
    if (stage >= 0) {
        Debug_w[DEBUG_STAGE_SELECT] = stage + 1;
        VS_Stage = stage;
    }
}

static void force_stage_transition_override() {
    if (stage < 0) {
        return;
    }

    apply_stage_override();
    if (Battle_Country != stage || bg_w.stage != stage || bg_w.area != 0) {
        Battle_Country = stage;
        bg_w.stage = stage;
        bg_w.area = 0;
        Push_LDREQ_Queue_BG(stage);
    } else {
        Battle_Country = stage;
    }
}

static void set_cursor(Character character, int player) {
    Cursor_X[player] = character_to_cursor[character][0];
    Cursor_Y[player] = character_to_cursor[character][1];
}

/// Repeatedly press and release a button
static void mash_button(SWKey button, int player) {
    u16* dst = player ? &p2sw_buff : &p1sw_buff;
    *dst |= (frame & 1) ? button : 0;
}

static void tap_button(SWKey button, int player) {
    u16* dst = player ? &p2sw_buff : &p1sw_buff;
    *dst |= button;
}

static u16 read_u16(SDL_IOStream* io, Sint64 offset) {
    u16 result;
    SDL_SeekIO(io, offset, SDL_IO_SEEK_SET);
    SDL_ReadIO(io, &result, 2);
    return SWAP16(result);
}

static u16 read_input_buff(SDL_IOStream* io, Sint64 offset) {
    const u16 raw_buff = read_u16(io, offset);
    u16 buff = 0;

    buff |= raw_buff & 0xF;              // directions
    buff |= raw_buff & (1 << 4);         // LP
    buff |= raw_buff & (1 << 5);         // MP
    buff |= raw_buff & (1 << 6);         // HP
    buff |= (raw_buff & (1 << 7)) << 1;  // LK
    buff |= (raw_buff & (1 << 8)) << 1;  // MK
    buff |= (raw_buff & (1 << 9)) << 1;  // HK
    buff |= (raw_buff & (1 << 12)) << 2; // start

    return buff;
}

static void initialize_data() {
    initialize_default_data();

    const char* base_path = configuration.test.states_path;

    if (base_path == NULL || base_path[0] == '\0') {
        return;
    }

    const size_t base_len = SDL_strlen(base_path);
    const size_t path_max_len = SDL_strlen(base_path) + 64;
    char* path = SDL_malloc(path_max_len);
    SDL_strlcpy(path, base_path, path_max_len);
    char filename[64];
    bool in_game_prev = false;
    bool did_set_char_data = false;

    for (int frame_num = 0;; frame_num++) {
        bool stop = false;
        path[base_len] = '\0';
        SDL_snprintf(filename, sizeof(filename), "/frame_%08d.ram", frame_num);
        SDL_strlcat(path, filename, path_max_len);
        SDL_IOStream* io = SDL_IOFromFile(path, "rb");

        if (io == NULL) {
            break;
        }

        const u16 routine = read_u16(io, GAME_ROUTINE_OFFSET);
        const bool in_game = (routine == 2);

        // Read character and SA indices until we get to game.
        // This ensures we read the latest data

        if (in_game && !did_set_char_data) {
            SDL_SeekIO(io, MY_CHAR_OFFSET, SDL_IO_SEEK_SET);
            SDL_ReadIO(io, characters, 2);

            SDL_SeekIO(io, SUPER_ARTS_OFFSET, SDL_IO_SEEK_SET);
            SDL_ReadIO(io, selected_super_arts, 2);

            did_set_char_data = true;
        }

        // Parse inputs

        if (in_game) {
            inputs[inputs_index][0] = read_input_buff(io, P1SW_OFFSET);
            inputs[inputs_index][1] = read_input_buff(io, P2SW_OFFSET);
            inputs_index += 1;
        } else if (in_game_prev) {
            stop = true;
        }

        in_game_prev = in_game;
        SDL_CloseIO(io);

        if (stop) {
            break;
        }
    }

    SDL_free(path);

    // There's no Shin Akuma in PS2 version, which is why we have to decrement character numbers after Akuma
    for (int i = 0; i < 2; i++) {
        if (characters[i] > CHAR_AKUMA) {
            characters[i] -= 1;
        }
    }

    inputs_total = inputs_index;
    inputs_index = 0;
}

static void update_player_super_art_activation_state(void) {
    for (int player = 0; player < 2; player++) {
        const bool is_active = player_super_art_active(player);
        if (is_active && !player_super_art_was_active[player]) {
            player_super_art_activation_starts[player] += 1;
        }
        player_super_art_was_active[player] = is_active;
    }
}

void TestRunner_Prologue() {
    p1sw_buff = 0;
    p2sw_buff = 0;

    switch (phase) {
    case PHASE_INIT:
        initialize_data();
        phase = PHASE_TITLE;
        // fallthrough

    case PHASE_TITLE: {
        const struct _TASK* menu_task = &task[TASK_MENU];

        if (menu_task->r_no[0] == 0 && menu_task->r_no[1] == 1 && menu_task->r_no[2] == 3) {
            phase = PHASE_MENU;
            break;
        }

        mash_button(SWK_START, 0);
        break;
    }

    case PHASE_MENU:
        apply_stage_override();

        if (G_No[1] == 1 && G_No[2] == 2 && (!scene_preset_uses_training_mode() || Mode_Type == MODE_NORMAL_TRAINING)) {
            Last_My_char2[0] = characters[0];
            Last_My_char2[1] = characters[1];
            Last_Super_Arts[0] = selected_super_arts[0];
            Last_Super_Arts[1] = selected_super_arts[1];
            phase = PHASE_CHARACTER_SELECT_TRANSITION;
            wait_timer = 60;
            break;
        }

        if (scene_preset_uses_training_mode()) {
            const struct _TASK* menu_task = &task[TASK_MENU];

            if (menu_task->r_no[1] == 1 && menu_task->r_no[2] == 3) {
                Menu_Cursor_Y[0] = 2;
                mash_button(SWK_SOUTH, 0);
                break;
            }

            if (menu_task->r_no[1] == 4 && menu_task->r_no[2] == 3) {
                Menu_Cursor_Y[0] = 0;
                mash_button(SWK_SOUTH, 0);
                break;
            }
        }

        mash_button(SWK_SOUTH, 0);
        break;

    case PHASE_CHARACTER_SELECT_TRANSITION:
        apply_stage_override();
        wait_timer -= 1;

        if (wait_timer <= 0) {
            phase = PHASE_CHARACTER_SELECT;
        }

        break;

    case PHASE_CHARACTER_SELECT:
        apply_stage_override();
        maybe_force_training_scene_character_and_super_state();
        switch (char_select_phase) {
        case 0:
            set_cursor(characters[0], 0);
            set_cursor(characters[1], 1);
            tap_button(SWK_START, 1);
            char_select_phase = 1;
            break;

        case 1:
            set_cursor(characters[0], 0);
            set_cursor(characters[1], 1);
            if (Select_Start[1] >= 2) {
                char_select_phase = 2;
            }

            break;

        case 2:
            set_cursor(characters[0], 0);
            set_cursor(characters[1], 1);

            if (My_char[0] != characters[0] || Sel_Arts_Complete[0] < 0) {
                mash_button(SWK_SOUTH, 0);
            }

            if (My_char[1] != characters[1] || Sel_Arts_Complete[1] < 0) {
                mash_button(SWK_SOUTH, 1);
            }

            if (My_char[0] == characters[0] && My_char[1] == characters[1] && Sel_Arts_Complete[0] >= 0 &&
                Sel_Arts_Complete[1] >= 0) {
                char_select_phase = 3;
            }
            break;

        case 3:
            maybe_force_training_scene_super_confirm();
            if (Sel_Arts_Complete[0] == 0) {
                mash_button(SWK_SOUTH, 0);
            }

            if (Sel_Arts_Complete[1] == 0) {
                mash_button(SWK_SOUTH, 1);
            }

            if (Sel_Arts_Complete[0] > 0 && Sel_Arts_Complete[1] > 0) {
                phase = PHASE_GAME_TRANSITION;
            }

            break;
        }

        break;

    case PHASE_GAME_TRANSITION:
        force_stage_transition_override();
        if (scene_preset_uses_training_mode()) {
            const struct _TASK* menu_task = &task[TASK_MENU];

            if (menu_task->r_no[0] == 7 && menu_task->r_no[1] == 1 && menu_task->r_no[2] == 1) {
                Menu_Cursor_Y[0] = 0;
                mash_button(SWK_SOUTH, 0);
                break;
            }

            if (training_mode_gameplay_started()) {
                phase = PHASE_GAME;
                game_frame = 0;
                break;
            }

            mash_button(SWK_ATTACKS, 0);
            break;
        } else if (G_No[1] == 2) {
            phase = PHASE_GAME;
            game_frame = 0;
        } else {
            if (!configuration.test.preserve_game_transition) {
                // This skips the VS animation on the default automation path.
                mash_button(SWK_ATTACKS, 0);
            }
            break;
        }

        // fallthrough

    case PHASE_GAME:
        if (configuration.test.delay_gameplay_inputs_until_active && !gameplay_input_active()) {
            p1sw_buff = 0;
            p2sw_buff = 0;
            break;
        }

        apply_initial_super_full_overrides();
        apply_scene_preset_super_refill_overrides();
        if (inputs_index < inputs_total) {
            p1sw_buff = inputs[inputs_index][0];
            p2sw_buff = inputs[inputs_index][1];
            inputs_index += 1;
        } else if (scene_preset != TEST_SCENE_PRESET_NONE) {
            apply_scene_preset_inputs();
        } else {
            p1sw_buff = 0;
            p2sw_buff = 0;
        }
        break;
    }
}

void TestRunner_Epilogue() {
    frame += 1;
    update_player_super_art_activation_state();

    if (phase == PHASE_GAME) {
        if (configuration.test.delay_gameplay_inputs_until_active && !gameplay_input_active()) {
            return;
        }
        game_frame += 1;
    }
}

#else /* !DEBUG — provide stubs for perf-telemetry references */

#include "test/test_runner.h"
#include <stdbool.h>

bool TestRunner_IsSupportedPhaseName(const char* phase_name) {
    (void)phase_name;
    return false;
}

const char* TestRunner_GetPhaseName(void) {
    return "none";
}

bool TestRunner_IsPhaseActive(const char* phase_name) {
    (void)phase_name;
    return false;
}

#endif
