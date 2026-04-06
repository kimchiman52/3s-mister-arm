#ifndef GAME_H
#define GAME_H

#include "structs.h"
#include <stdint.h>

void Game();
void Game01();
void Game02();
void Before_Select_Sub();
void Game_Task(struct _TASK* task_ptr);
void Game01_Sub();
void Next_Title_Sub();

uint64_t Game_GetPerfGameLogicNs(void);
uint64_t Game_GetPerfSpriteSubmitNs(void);
uint64_t Game_GetPerfDispatchNs(void);

#endif
