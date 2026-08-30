#ifndef TEST_TEXGROUP_WINDOW_PROBE_H
#define TEST_TEXGROUP_WINDOW_PROBE_H

/* Task #64 measurement instrument — NOT a fix, NOT a guard.
 *
 * Two open items were left undemonstrated by the case-2 reclaim added in
 * 9f9beda1 (see the block comment at texgroup.c:261-369):
 *
 *   F3  purge_texture_group() frees the block that char_init_data[]'s 25
 *       raw pointers (texgroup.c:421-427) and parabora_own_table[]
 *       (texgroup.c:488) point into, and clears only texgrplds[].ok.
 *       Nothing re-publishes those pointers until case 4 runs, and only
 *       for bsd->ix1st == 1. Question: can a consumer read them inside
 *       the free -> refill window?
 *
 *   F4  getObjectHeight() returns 0 when texgrplds[i].ok == 0
 *       (mtrans.c:369-375) into wk->reserv_add_y, which is checksummed
 *       rollback state: `GS_SAVE(plw)` game_state.c:690, member
 *       `X(reserv_add_y)` plw_canon_fields.h:419. Question: can the
 *       ok == 0 window overlap check_tsukamare_keizoku_check (plpcu.c:236)?
 *
 * This header declares the observation points. Everything compiles to
 * nothing unless ENABLE_PERF_TELEMETRY is on, and even when on the
 * per-read hooks do no I/O: they bump counters and only emit a log line
 * on the rare event being hunted, because flLogOut() appends to a file
 * on every call (foundaps2.c:89-113) and the read hooks sit on hot
 * per-object paths.
 */

#include "port/build_config.h" /* ENABLE_TEXGROUP_WINDOW_PROBE */
#include "types.h"

#include <stdint.h>

#if ENABLE_TEXGROUP_WINDOW_PROBE

/* --- F3: char_init_data / parabora_own_table dangling window --- */

/* Called at the case-2 reclaim site immediately BEFORE
 * purge_texture_group(). Scans all 23 char_init_data[] slots (25
 * pointers each) and all 20 parabora_own_table[] entries for pointers
 * inside the block about to be freed, and opens a window on every slot
 * that has at least one. */
void TGWP_ReclaimOpen(u8 grp, s16 oldkey, s16 req_id, s16 ix, s16 ix1st);

/* Called from game.c immediately after the Main_Jmp_Tbl[G_No[0]] dispatch,
 * i.e. once per executed game-logic frame. This is the unit the F3
 * question is actually posed in ("does the free -> refill window span a
 * frame of game logic?"); system_timer is not usable for it because a
 * rollback resim rewinds it. */
void TGWP_LogicTick(void);

/* Called when the case-4 branch finishes a load for `grp` (be = 0),
 * after char_init_data / parabora_own_table have been re-published (or
 * not, when ix1st != 1). Closes any window opened for `grp` and reports
 * whether the refill landed at the same address and whether the slot's
 * pointers actually moved out of the freed range. */
void TGWP_LoadComplete(u8 grp, s16 ix1st, s16 newkey);

/* Read hooks. `slot` is the char_init_data[] index (wk->charset_id);
 * `cid` is the parabora_own_table[] index (wk->dm_plnum). */
void TGWP_ReadCid(s16 slot, const char* who);
void TGWP_ReadPara(s16 cid, const char* who);

/* --- F4: getObjectHeight ok == 0 census --- */

/* Bracket the check_tsukamare_keizoku_check body (plpcu.c:236) so the
 * census can attribute an ok == 0
 * return to the checksummed-state caller specifically. */
void TGWP_TsukamareEnter(void);
void TGWP_TsukamareExit(void);

/* Called from getObjectHeight()'s texgrplds[i].ok == 0 early return. */
void TGWP_ObjectHeightZero(u16 cgnum, s32 grp);

/* Dump accumulated counters. Safe to call repeatedly. */
void TGWP_Report(const char* tag);

#else

#define TGWP_ReclaimOpen(grp, oldkey, req_id, ix, ix1st) ((void)0)
#define TGWP_LogicTick() ((void)0)
#define TGWP_LoadComplete(grp, ix1st, newkey) ((void)0)
#define TGWP_ReadCid(slot, who) ((void)0)
#define TGWP_ReadPara(cid, who) ((void)0)
#define TGWP_TsukamareEnter() ((void)0)
#define TGWP_TsukamareExit() ((void)0)
#define TGWP_ObjectHeightZero(cgnum, grp) ((void)0)
#define TGWP_Report(tag) ((void)0)

#endif /* ENABLE_TEXGROUP_WINDOW_PROBE */

#endif /* TEST_TEXGROUP_WINDOW_PROBE_H */
