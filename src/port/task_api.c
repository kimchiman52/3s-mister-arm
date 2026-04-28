#include "port/task_api.h"
#include "sf33rd/Source/Game/system/work_sys.h" /* extern struct _TASK task[11] */

#include <assert.h>

/*
 * 3sx-mister include-path fixup: upstream's main.h declares TASK_SAVER2 = 7
 * in the TaskID enum. Our fork does not yet define TASK_SAVER2 (the saver2
 * task slot is unused on MiSTer). The accessor below is still required by
 * the upstream port contract; we alias the slot index locally to keep
 * source parity without modifying main.h in this step.
 */
#ifndef TASK_SAVER2
#define TASK_SAVER2 7
#endif

_Static_assert(TASK_SAVER2 == 7, "TASK_SAVER2 must stay 7 to match upstream");

/* ── Generic Task Accessors ────────────────────────────────────────────── */

struct _TASK* Task_GetPtr(TaskID id) {
    assert(id < 11);
    return &task[id];
}

bool Task_IsActive(TaskID id) {
    assert(id < 11);
    return task[id].condition == 1;
}

void Task_Activate(TaskID id) {
    assert(id < 11);
    task[id].condition = 1;
}

void Task_Deactivate(TaskID id) {
    assert(id < 11);
    task[id].condition = 0;
}

/* ── Specific Task State Setters ───────────────────────────────────────── */

void PauseTask_SetPhase(int phase) {
    task[TASK_PAUSE].r_no[2] = (u8)phase;
}

void PauseTask_SetTimer(int timer) {
    task[TASK_PAUSE].free[0] = (u8)timer;
}

void Saver2_Task_SetPhase(int phase) {
    task[TASK_SAVER2].r_no[0] = (u8)phase;
}
