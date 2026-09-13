#ifndef MENU_H
#define MENU_H

#include "structs.h"
#include "types.h"
#include <stdbool.h>

void Menu_Task(struct _TASK* task_ptr);
void Menu_Init(struct _TASK* task_ptr);
void Setup_Pad_or_Stick();
u16 Check_Menu_Lever(u8 PL_id, s16 type);

/* Stages of the shared no-selection match start (menu.c -> Match_Start_Sub),
 * kept in task_ptr->r_no[3]. The values are Load_Replay_Sub's original
 * case numbers so the replay path is unchanged. */
enum {
    MATCH_START_LOAD = 3,
    MATCH_START_FADE_IN = 4,
    MATCH_START_WAIT = 5,
    MATCH_START_ENTER = 6
};

/* Fade-out frames both match-start entries (Load_Replay_Sub case 2,
 * VS_Result_Rematch) arm before MATCH_START_LOAD's `--timer <= 0` fires the
 * purge and the LDREQ pushes. Named so menu.c can ASSERT its relationship to
 * the netplay prediction window instead of describing it; see the
 * rollback-exposure note above Match_Start_Sub. */
#define MATCH_START_FADE_OUT_FRAMES 0xA

/* Setup_VS_Mode without parking the menu task (r_no[0] = 5). */
void Setup_VS_Players(void);
/* Rematch: the rollback-final confirmation wait (true while still waiting)
 * and the fresh-match state reset it performs before the loads. Public so
 * test_netplay_units.c can pin both. */
bool VS_Result_Rematch_ConfirmationPending(struct _TASK* task_ptr);
void VS_Result_Rematch_Reset_Match_State(void);
void Menu_Common_Init();
s32 Load_Replay_MC_Sub(struct _TASK* task_ptr, s16 PL_id);
void Setup_Save_Replay_2nd(struct _TASK* task_ptr, s16 /* unused */);
s32 Setup_Final_Cursor_Pos(s8 cursor_x, s16 dir);
void Default_Training_Data(s32 flag);
void Decide_PL(s16 PL_id);

/* Training-mode SELECT reset: applies the swap / left-corner / right-corner
 * position presets. Called from plcnt_init (TASK_GAME) immediately after
 * move_player_work, which is the frame and the moment plmv_1020 writes the
 * start positions. No-op unless a reset is in flight. */
void Tr_Reset_Position_Override();

#endif
