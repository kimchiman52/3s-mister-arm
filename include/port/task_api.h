#ifndef TASK_API_H
#define TASK_API_H

#include "main.h"
#include <stdbool.h>

struct _TASK;

/* ── Generic Task Accessors ────────────────────────────────────────────── */

/** @brief Get a pointer to the specified task struct. */
struct _TASK* Task_GetPtr(TaskID id);

/** @brief Check if the specified task is active (condition == 1). */
bool Task_IsActive(TaskID id);

/** @brief Activate the specified task (condition = 1). */
void Task_Activate(TaskID id);

/** @brief Deactivate the specified task (condition = 0). */
void Task_Deactivate(TaskID id);

/* ── Specific Task State Setters ───────────────────────────────────────── */

/** @brief Set the phase for the pause task. */
void PauseTask_SetPhase(int phase);

/** @brief Set the timer (free[0]) for the pause task. */
void PauseTask_SetTimer(int timer);

/** @brief Set the phase for the saver2 task (TASK_SAVER2). */
void Saver2_Task_SetPhase(int phase);

#endif /* TASK_API_H */
