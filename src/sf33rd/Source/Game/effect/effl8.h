#ifndef EFFL8_H
#define EFFL8_H

#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "structs.h"
#include "types.h"

/* ---- effect L8's ColorRAM addressing: SINGLE SOURCE OF TRUTH ----
 *
 * Effect L8 (Makoto's SA buff) latches and rewrites a 12-entry prefix of two
 * ColorRAM rows per player. Those rows are simulation state: effl8 copies the
 * live palette into the SAVED attack-parameter window of its own frw slot, so
 * if a rollback restores frw but not ColorRAM, the effect re-latches
 * already-buffed colours as the "old" colours it will restore at buff end.
 * That was the `frw` DIVERGENT+FEEDBACK finding in
 * docs/rollback-determinism-harness.md; the fix pulled these rows into
 * GameState.effl8_colorram.
 *
 * The rollback save window in src/netplay/game_state.c is DERIVED from the
 * macros below — it does not restate the row numbers. Editing a row here
 * moves the save set with it. Editing the addressing in effect_L8_move()
 * WITHOUT these macros trips the runtime assertions there (Debug builds;
 * Release defines NDEBUG so they cost nothing on the target).
 *
 * Entries per row are 64 u16, taken from ColorRAM's own type rather than
 * written out, so the row-index arithmetic and the +512 s16 table stride stay
 * mechanically tied to the array they index.
 */
#define EFFL8_COLORRAM_TOTAL_ROWS ((int)(sizeof(ColorRAM) / sizeof(ColorRAM[0])))
#define EFFL8_COLORRAM_ROW_ENTRIES ((int)(sizeof(ColorRAM[0]) / sizeof(ColorRAM[0][0])))

/* master_id is 0 or 1 (the effect's owning player). */
#define EFFL8_MASTER_IDS 2

/* effl8.c: step_xy_table = (s16*)ColorRAM[(master_id == 1) * 16] */
#define EFFL8_STEP_ROW(master_id) (((master_id) == 1) * 16)
/* effl8.c: move_xy_table = step_xy_table + 512 s16 == 8 rows further on */
#define EFFL8_MOVE_ROW(master_id) (EFFL8_STEP_ROW(master_id) + 8)
/* The s16 stride effect_L8_move() actually adds, derived from the two rows so
 * it cannot drift from them (8 rows * 64 u16 == 512). */
#define EFFL8_MOVE_TABLE_S16_STRIDE ((EFFL8_MOVE_ROW(0) - EFFL8_STEP_ROW(0)) * EFFL8_COLORRAM_ROW_ENTRIES)

/* Loop bound shared by save_old_color_data / load_old_color_data /
 * get_new_color_data_L8 — the width of the mutated window in each row. */
#define EFFL8_COLOR_ENTRIES 12

void effect_L8_move(WORK_Other* ewk);
s32 effect_L8_init(PLW* wk);
void check_new_color_data_L8(WORK* wk);
void get_new_color_data_L8(WORK* /* unused */, s16* trom, s16* tram);
void save_old_color_data(s16* wram, s16* tram);
void load_old_color_data(s16* wram, s16* tram);

#endif
