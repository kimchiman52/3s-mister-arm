/**
 * @file select_timer.h
 * @brief Character select screen countdown timer.
 *
 * Manages the timer that counts down during character/super-art selection.
 * Uses BCD (Binary-Coded Decimal) arithmetic inherited from the CPS3 arcade hardware.
 *
 * Part of the game flow module.
 * Originally from the PS2 game module.
 *
 * NOTE (track-a, 2026-04-20): Ported from 3sxtra as part of the netplay port
 * (Track A Phase 1). The call sites (game.c, sel_pl.c, next_cpu.c, sys_sub.c)
 * are INTENTIONALLY NOT wired yet — this fork's select-timer logic lives in
 * effect/effa5.c. This module exists so GameState can carry
 * `SelectTimerState select_timer_state` through rollback. Future Track D will
 * wire the call sites and remove the effa5.c inline countdown.
 */

#ifndef SELECT_TIMER_H
#define SELECT_TIMER_H

#include "types.h"

#include <stdbool.h>

typedef struct SelectTimerState {
    bool is_running; /**< Whether the select timer is actively counting down. */
    s32 step;        /**< Current state machine step (0=wait, 1=counting, 2=zero, 3=timeout). */
    s32 timer;       /**< Sub-timer for delay after reaching zero before triggering timeout. */
} SelectTimerState;

extern SelectTimerState select_timer_state;

void SelectTimer_Init();
void SelectTimer_Finish();
void SelectTimer_Run();

#endif
