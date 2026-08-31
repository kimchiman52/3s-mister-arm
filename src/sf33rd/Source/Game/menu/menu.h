#ifndef MENU_H
#define MENU_H

#include "structs.h"
#include "types.h"

void Menu_Task(struct _TASK* task_ptr);
void Menu_Init(struct _TASK* task_ptr);
void Setup_Pad_or_Stick();
u16 Check_Menu_Lever(u8 PL_id, s16 type);
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
