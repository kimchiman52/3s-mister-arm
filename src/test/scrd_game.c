#if defined(STATCHECK)

#include "test/scrd_game.h"
#include "arcade/arcade_constants.h"
#include "constants.h"
#include "test/ram_archive.h"

#include <SDL3/SDL.h>

// Self-contained big-endian u16 reader. Upstream's replay_game.c pulls this
// from test/test_runner_utils.c, but the fork's copy of that file is gated
// `#if DEBUG` (never compiled in a STATCHECK build), so we inline it here to
// keep the STATCHECK translation unit independent of the DEBUG harness.
static Uint16 scrd_read_u16(SDL_IOStream* io, Sint64 offset) {
    Uint16 result = 0;
    SDL_SeekIO(io, offset, SDL_IO_SEEK_SET);
    SDL_ReadU16BE(io, &result);
    return result;
}

static void scrd_adjust_character_numbers(ScrdGame* game) {
    for (int i = 0; i < 2; i++) {
        game->characters[i] = CHAR_ARCADE_TO_3SX(game->characters[i]);
    }
}

/* The raw character bound below is stated in the ARCADE index space, because
 * that is what the archive byte holds; every consumer sees it only after
 * CHAR_ARCADE_TO_3SX. This assertion is what makes the raw bound imply the
 * subscript bound, so the two cannot drift apart silently. */
_Static_assert(CHAR_ARCADE_TO_3SX(SCRD_MAX_RAW_CHARACTER) == NUM_CHARS - 1,
               "the raw arcade character bound must map to the last 3SX character index");

/* Read `count` bytes at `offset` and REFUSE the run if any is outside 0..max.
 *
 * Every field this reads is an array subscript or a two-element player id once
 * the runner injects it, and the archive byte is raw, so 0..255. Verified
 * targets, all in `statcheck_runner.c` -> `StatcheckRunner_Prologue` unless
 * noted:
 *
 *   characters[]   -> `set_cursor` -> `character_to_cursor[20][2]`, and
 *                     `Last_My_char2[]` -> `My_char[]`, the 20-row FIRST
 *                     dimension of `super_arts_data` (`plcnt.c`)
 *   supers[]       -> `Last_Super_Arts[]` -> `Super_Arts[]`, whose fourth
 *                     `SA_DATA` slot no shipped build can select
 *                     (docs/research-arcade-cg-data-accuracy.md §16.3 -- that
 *                     unreachability is what holds the three SA clamps inert)
 *   colors[]       -> `color_to_keys[13]`
 *   new_challenger -> `New_Challenger`, then `Champion = New_Challenger ^ 1`
 *                     and `plw[New_Challenger]` / `Operator_Status[...]` /
 *                     `Continue_Coin[...]` (`entry.c` -> Break_Into_Check),
 *                     all `[2]`
 *   stage          -> `Debug_w[DEBUG_STAGE_SELECT] = stage + 1`, a DIRECT write
 *                     that bypasses `DebugConfig_Set`'s clamp against
 *                     `debug_string_data[31].max` of 21, then
 *                     `bg_w.stage = Debug_w[31] - 1` (`sel_pl.c` -> Exit_2nd)
 *                     and `app_type_tbl[20][20][22]` (`appear.c`)
 *
 * REJECT rather than clamp: the archive IS the setup under test, so a clamped
 * value runs the harness against a match the archive does not describe and
 * then reports it as a PASS -- the one outcome this harness exists to make
 * trustworthy. Same choice `args.c` makes for `--test-p1-super-art` and
 * `test_runner.c` -> `read_bounded_archive_pair` makes for `--test-states`.
 *
 * Reported through ScrdGameInitResult rather than `exit(2)`: this file has no
 * other exit, and routing it through the enum is what lets `main.c` keep a
 * malformed archive out of exit code 1, which means "the engine diverged from
 * CPS3" and is what a sweep turns into a worklist item. */
static bool scrd_read_bounded(
    SDL_IOStream* io, Sint64 offset, const char* what, unsigned max, int count, Uint8* out) {
    SDL_SeekIO(io, offset, SDL_IO_SEEK_SET);

    if (SDL_ReadIO(io, out, (size_t)count) != (size_t)count) {
        SDL_Log("ScrdGame_Init: archive frame is too short to hold %s at offset 0x%llX",
                what,
                (unsigned long long)offset);
        return false;
    }

    for (int i = 0; i < count; i++) {
        if (out[i] > max) {
            SDL_Log("ScrdGame_Init: archive frame holds %s[%d] = %u, outside 0..%u", what, i, out[i], max);
            return false;
        }
    }

    return true;
}

static bool scrd_read_match_setup(ScrdGame* game, SDL_IOStream* io) {
    /* Arcade index space -- scrd_adjust_character_numbers() maps it below.
     * Arcade 15 (CHAR_SHIN_AKUMA) is NOT rejected here: CHAR_ARCADE_TO_3SX
     * folds it onto CHAR_AKUMA, which is in range, so it is a fidelity
     * question about a character the port does not have rather than an
     * out-of-bounds one. Recorded, not decided here. */
    if (!scrd_read_bounded(io, MY_CHAR_OFFSET, "My_char", SCRD_MAX_RAW_CHARACTER, 2, game->characters)) {
        return false;
    }

    if (!scrd_read_bounded(io, SUPER_ARTS_OFFSET, "Super_Arts", SCRD_MAX_SUPER_ART, 2, game->supers)) {
        return false;
    }

    if (!scrd_read_bounded(
            io, NEW_CHALLENGER_OFFSET, "New_Challenger", SCRD_MAX_NEW_CHALLENGER, 1, &game->new_challenger)) {
        return false;
    }

    if (!scrd_read_bounded(io, PLAYER_COLOR_OFFSET, "Player_Color", SCRD_MAX_PLAYER_COLOR, 2, game->colors)) {
        return false;
    }

    /* Stage (H2, docs/research-arcade-balance-desyncs.md). It is NOT
     * reconstructible from the character select: on the arcade the stage
     * carries across matches, and `appear_data_init_set()` (`appear.c`) indexes
     * `app_type_tbl[own][opp][bg_w.stage]` to pick both `wu.routine_no[4]` and
     * `wu.xyz[0].disp.pos`, so getting it wrong moves a player at battle start.
     *
     * CHAR_ARCADE_TO_3SX is the right transform, not a coincidence: both sides
     * derive the home stage from a character id in their own index space.
     * `Setup_Battle_Country()` (`sel_pl.c`) returns `My_char[...]` verbatim, and
     * on the arcade side the byte at BG_W_STAGE_OFFSET equals one of the two
     * players' arcade character ids in all 11 measured segments. The port drops
     * arcade index 15 (CHAR_SHIN_AKUMA) from both spaces -- which is exactly why
     * `app_type_tbl` is [20][20][22] here against the arcade's [21][21][23]. */
    Uint8 stage = 0;

    /* Same arcade-space bound as the characters above, and for the reason the
     * comment gives: the byte at BG_W_STAGE_OFFSET is one of the two players'
     * arcade character ids. */
    if (!scrd_read_bounded(io, BG_W_STAGE_OFFSET, "bg_w.stage", SCRD_MAX_RAW_CHARACTER, 1, &stage)) {
        return false;
    }

    game->stage = (Uint8)CHAR_ARCADE_TO_3SX(stage);

    scrd_adjust_character_numbers(game);
    return true;
}

/* Finding the match start (H1, docs/research-arcade-balance-desyncs.md).
 *
 * `G_No[1..3] == (2, 0, 0)` says only "the Game task is parked on the
 * Game2_0 slot" (`Game_Jmp_Tbl[G_No[1]]` -> `Game02` ->
 * `Game02_Jmp_Tbl[G_No[2]]`, game.c). It does NOT say the match started.
 * A segment cut right after a final KO can carry that triple frozen for its
 * whole length while the Game task is not being ticked at all -- measured on
 * two archives whose (G_No, C_No) pair never changes across 2,270 and 2,286
 * frames. Starting there put the archive mid-match against a fresh engine and
 * reported `Game_timer (0) != 6501` at archive frame 1: a harness artifact,
 * not a divergence.
 *
 * The signature of a match that actually started is the visible effect of
 * `Game2_0()` (game.c) having run: it writes, in one frame,
 *     `Game_timer = 0; C_No[0..3] = 0; G_No[2] = 3;`
 * So require the frame AFTER the triple to show `Game_timer == 0` and
 * `G_No[2] == 3`. Measured over the 16-segment corpus in
 * /Volumes/KimchDrive/3sarm-convert-tmp/rerun2: identical `start_index` (1)
 * on all 14 segments that contain a match, and no start found on the two that
 * do not -- which is the correct answer for those, not an error. */
ScrdGameInitResult ScrdGame_Init(ScrdGame* game, const char* ram_archive_path) {
    SDL_zerop(game);
    game->start_index = -1;

    if (!RamArchive_Init(&game->archive, ram_archive_path)) {
        SDL_Log("ScrdGame_Init: Failed to initialize RAM archive");
        return SCRD_GAME_INIT_ARCHIVE_ERROR;
    }

    /* True when the PREVIOUS frame carried the (2, 0, 0) triple and its
     * match setup has been latched into `game`. */
    bool armed = false;

    for (int frame_num = 0;; frame_num++) {
        SDL_IOStream* io = RamArchive_GetFrame(&game->archive, frame_num);

        if (io == NULL) {
            break;
        }

        const Uint16 g_no_1 = scrd_read_u16(io, G_NO_OFFSET + 2);
        const Uint16 g_no_2 = scrd_read_u16(io, G_NO_OFFSET + 4);
        const Uint16 g_no_3 = scrd_read_u16(io, G_NO_OFFSET + 6);
        const Uint16 game_timer = scrd_read_u16(io, GAME_TIMER_OFFSET);

        // Game2_0() ran between the armed frame and this one.
        if (armed && (game_timer == 0) && (g_no_2 == 3)) {
            game->start_index = frame_num;
            SDL_SeekIO(io, PLW_OFFSET + WORK_WU_OPERATOR_OFFSET, SDL_IO_SEEK_SET);
            SDL_ReadU8(io, &game->wu_operator[0]);
            SDL_SeekIO(io, PLW_OFFSET + PLW_SIZE + WORK_WU_OPERATOR_OFFSET, SDL_IO_SEEK_SET);
            SDL_ReadU8(io, &game->wu_operator[1]);
            SDL_CloseIO(io);
            break;
        }

        armed = (g_no_1 == 2) && (g_no_2 == 0) && (g_no_3 == 0);

        if (armed && !scrd_read_match_setup(game, io)) {
            /* scrd_read_bounded() has already named the field and the value.
             * Refuse the whole archive rather than the frame: a malformed setup
             * is not something a later armed frame can repair, and the runner
             * injects whichever armed frame's setup was latched last. */
            SDL_CloseIO(io);
            SDL_Log("ScrdGame_Init: '%s' holds an out-of-range match setup at archive frame %d; refusing to "
                    "compare rather than clamping, because the archive IS the setup under test",
                    ram_archive_path,
                    frame_num);
            RamArchive_Destroy(&game->archive);
            return SCRD_GAME_INIT_BAD_SETUP;
        }

        SDL_CloseIO(io);
    }

    if (game->start_index == -1) {
        SDL_Log("ScrdGame_Init: no match start in '%s' -- the (2,0,0) G_No triple is never "
                "followed by Game2_0()'s Game_timer=0 / G_No[2]=3 (segment holds no match)",
                ram_archive_path);
        RamArchive_Destroy(&game->archive);
        return SCRD_GAME_INIT_NO_MATCH_START;
    }

    /* Reject a segment the cabinet ran against the CPU (H4b,
     * docs/research-arcade-balance-desyncs.md).
     *
     * The harness synthesises a two-operator VERSUS match: it taps SWK_START
     * for player 2 at PHASE_CHARACTER_SELECT (`statcheck_runner.c`), which is
     * what sets `Operator_Status[1]` (`entry.c` -> Entry_Mark_Set,
     * `game.c` -> Break_Into_Check), `set_base_data()` (`plcnt.c`) copies that
     * into `plw[ix].wu.wu_operator`, and `Setup_Play_Type()` (`sys_sub.c`)
     * turns the pair into `Play_Type == 1`. A recording made against the CPU
     * ran at `Play_Type == 0` with `cpu_algorithm()` driving one side.
     *
     * That is not a gap that importing `wu_operator` would close, for three
     * independent reasons -- all read off the port's own sources:
     *
     * 1. It would DISABLE the oracle's input pinning on exactly those
     *    segments. `Player_move()` (`plmain.c`) is
     *        `if (wk->wu.wu_operator) { wk->cp->sw_lvbt = lv_data; }
     *         else { wk->cp->sw_lvbt = processed_lvbt(cpu_algorithm(wk)); }`
     *    -- with the operator flag clear it discards `lv_data`, i.e. the
     *    archive's own button word that `read_input_buff()` feeds it. The test
     *    would stop being "same inputs, same state" and become "re-derive the
     *    AI", where one wrong frame is unattributable.
     *
     * 2. The AI reads state the archive import set does not carry.
     *    `Setup_Lv18(save_w[Present_Mode].Difficulty)` (`com_pl.c`,
     *    `com_sub.c`) and `asagh_zuru[save_w[Present_Mode].Difficulty]` in
     *    `add_sp_arts_gauge_paring()` / `_tokushu()` / `_ukemi()` /
     *    `_nagenuke()` (`pls02.c`, each guarded by `wu_operator == 0`) all
     *    index the cabinet's service difficulty. `Statcheck_SyncValues()`
     *    (`statcheck_compare.c`) imports Random_ix16, Random_ix32,
     *    players_timer, t_pl_lvr and Round_Level -- and nothing else. There is
     *    no offset for `Difficulty` in `arcade_constants.h` and none was
     *    derivable, so the AI's own difficulty scaling cannot be reproduced.
     *
     * 3. `wu_operator == 0` inside the harness's VERSUS match is a state the
     *    cabinet never had. The recordings are arcade-mode matches; the
     *    harness drives console MODE_VERSUS (`StatcheckRunner_PinConfig` forces
     *    game-mode=console because the arcade path never reaches the Menu_Task
     *    sequence its phase machine watches). Sites branch on the two together
     *    -- e.g. `cmb_win.c`'s
     *    `(!ArcadeBalance_IsEnabled() && Mode_Type == MODE_VERSUS) ||
     *     plw[PLS].wu.wu_operator` and `sys_sub.c`'s
     *    `(Mode_Type != MODE_VERSUS && Mode_Type != MODE_REPLAY) &&
     *     plw[PL_id].wu.wu_operator == 0` -- so clearing the flag alone would
     *    manufacture NEW divergences rather than remove one.
     *
     * So the honest outcome is a typed rejection, exactly as H1 does for a
     * matchless segment: `main.c` turns this into exit code 3, kept distinct
     * from 1 (= engine divergence) and from 2 (= no match in segment). The
     * load-bearing property is that a segment the harness cannot reproduce is
     * never reported as an engine divergence. */
    if ((game->wu_operator[0] == 0) || (game->wu_operator[1] == 0)) {
        SDL_Log("ScrdGame_Init: '%s' is not human-vs-human -- wu_operator = (%u, %u) at the match "
                "start frame, so the cabinet ran Play_Type == 0 with cpu_algorithm() on at least "
                "one side; the harness forces two operators and cannot reproduce that (H4b)",
                ram_archive_path,
                game->wu_operator[0],
                game->wu_operator[1]);
        RamArchive_Destroy(&game->archive);
        return SCRD_GAME_INIT_CPU_PLAYER;
    }

    return SCRD_GAME_INIT_OK;
}

void ScrdGame_Destroy(ScrdGame* game) {
    RamArchive_Destroy(&game->archive);
    SDL_zerop(game);
}

#endif
