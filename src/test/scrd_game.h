#if defined(STATCHECK)

#ifndef SCRD_GAME_H
#define SCRD_GAME_H

#include "constants.h"
#include "test/ram_archive.h"

#include <SDL3/SDL.h>

/* Inclusive upper bounds on the five raw match-setup bytes ScrdGame_Init reads
 * out of the archive. Every one of them becomes an array subscript or a
 * two-element player id once `statcheck_runner.c` injects it; the block comment
 * on `scrd_read_bounded` names the target array for each. They live in the
 * header so the consumer can assert its own tables against them -- see the
 * `_Static_assert`s in `statcheck_runner.c`, which are what stops a resized
 * table from leaving a bound behind. */

/* Arcade index space, pre-CHAR_ARCADE_TO_3SX: the arcade carries one character
 * more than the port (Shin Akuma at 15), so the highest legal raw is NUM_CHARS
 * and the transform brings it to NUM_CHARS - 1. */
#define SCRD_MAX_RAW_CHARACTER NUM_CHARS

/* Super_Arts[] selects an SA_DATA slot; only 0..2 are reachable in a shipped
 * build (docs/research-arcade-cg-data-accuracy.md §16.3). */
#define SCRD_MAX_SUPER_ART 2

/* Indexes `color_to_keys[13]` (`statcheck_runner.c`). */
#define SCRD_MAX_PLAYER_COLOR 12

/* A player id: `Champion = New_Challenger ^ 1`, then `plw[New_Challenger]`. */
#define SCRD_MAX_NEW_CHALLENGER 1

typedef struct ScrdGame {
    int start_index;
    Uint8 characters[2];
    Uint8 supers[2];
    Uint8 colors[2];
    Uint8 new_challenger;
    /* Home stage, already mapped into the port's 3SX index space (H2). */
    Uint8 stage;
    /* Per-player `wu.wu_operator` as the archive holds it at `start_index`
     * (H4b): 1 = human, 0 = CPU AI. Latched for the rejection message only --
     * the harness cannot reproduce a CPU-driven side, see ScrdGame_Init. */
    Uint8 wu_operator[2];
    RamArchive archive;
} ScrdGame;

/* Outcome of ScrdGame_Init. Two of the four outcomes are HARNESS LIMITS, not
 * engine divergences, and callers must keep them distinct from
 * ARCHIVE_ERROR/divergence so a sweep never turns one into a worklist item.
 *
 * NO_MATCH_START: the runner's segmenter cuts a new game_N only when G_No[1]
 * stops being 2 (docs/research-arcade-balance-desyncs.md H4), which can hand
 * us a segment that is entirely the post-KO tail of the previous match and
 * contains no Game2_0() at all -- nothing to compare (H1).
 *
 * CPU_PLAYER: the recording had `wu.wu_operator == 0` on at least one side,
 * i.e. the cabinet ran that side from `cpu_algorithm()` under
 * `Play_Type == 0`. The harness cannot reproduce that (H4b); see the block
 * comment above the check in ScrdGame_Init for why.
 *
 * BAD_SETUP: one of the five raw match-setup bytes was outside the range its
 * consumer can subscript (the SCRD_MAX_* bounds above). A THIRD harness-limit
 * outcome, kept out of the divergence code for the same reason as the other
 * two: a hand-edited or truncated archive that would have indexed off the end
 * of `character_to_cursor[20]` / `color_to_keys[13]` / `Super_Arts[]` is not an
 * engine divergence, and the run is refused rather than clamped because the
 * archive IS the setup under test. */
typedef enum ScrdGameInitResult {
    SCRD_GAME_INIT_OK,
    SCRD_GAME_INIT_ARCHIVE_ERROR,
    SCRD_GAME_INIT_NO_MATCH_START,
    SCRD_GAME_INIT_CPU_PLAYER,
    SCRD_GAME_INIT_BAD_SETUP,
} ScrdGameInitResult;

ScrdGameInitResult ScrdGame_Init(ScrdGame* game, const char* ram_archive_path);
void ScrdGame_Destroy(ScrdGame* game);

#endif

#endif
