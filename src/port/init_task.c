/**
 * @file init_task.c
 * @brief Accessor implementations for cross-slot task[TASK_INIT] access.
 *
 * This keeps the struct layout unchanged (netplay rollback safe)
 * while routing cross-slot mutations through a single chokepoint.
 *
 * Mirrors the MenuTask_* pattern from menu_task.c.
 */

#include "port/init_task.h"

#include "main.h"                               /* TaskID, TASK_INIT               */
#include "sf33rd/Source/Game/system/work_sys.h" /* extern struct _TASK task[11]     */
#include "structs.h"                            /* struct _TASK                     */

#include <assert.h>

/* ── Phase getters ─────────────────────────────────────────────── */

InitTaskPhase InitTask_GetPhase(void) {
    return (InitTaskPhase)task[TASK_INIT].r_no[0];
}

/* ── Phase setters ─────────────────────────────────────────────── */

void InitTask_SetPhase(InitTaskPhase phase) {
    task[TASK_INIT].r_no[0] = (u8)phase;
}

void InitTask_ResetSubPhases(void) {
    task[TASK_INIT].r_no[1] = 0;
    task[TASK_INIT].r_no[2] = 0;
    task[TASK_INIT].r_no[3] = 0;
}

void InitTask_ClearAllRNo(void) {
    task[TASK_INIT].r_no[0] = 0;
    task[TASK_INIT].r_no[1] = 0;
    task[TASK_INIT].r_no[2] = 0;
    task[TASK_INIT].r_no[3] = 0;
}

/* ── Condition ─────────────────────────────────────────────────── */

void InitTask_Deactivate(void) {
    task[TASK_INIT].condition = 0;
}

bool InitTask_IsActive(void) {
    return task[TASK_INIT].condition == 1;
}
