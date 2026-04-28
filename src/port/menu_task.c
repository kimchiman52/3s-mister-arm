/**
 * @file menu_task.c
 * @brief Accessor implementations for cross-slot task[TASK_MENU] access.
 *
 * This keeps the struct layout unchanged (netplay rollback safe)
 * while routing cross-slot mutations through a single chokepoint.
 *
 * @note Some call sites still pass &task[TASK_MENU] as a function
 *       parameter (e.g. Menu_Init, MenuScreen_ExitToLegacy). Those
 *       are parameter-based pointer passes, not cross-slot field
 *       access, and are intentionally outside this accessor layer.
 */

#include "port/menu_task.h"

#include "main.h"                               /* TaskID, TASK_MENU               */
#include "sf33rd/Source/Game/system/work_sys.h" /* extern struct _TASK task[11]     */
#include "structs.h"                            /* struct _TASK                     */

#include <assert.h>

/* ── Phase getters ─────────────────────────────────────────────── */

MenuTaskPhase MenuTask_GetPhase(void) {
    return (MenuTaskPhase)task[TASK_MENU].r_no[0];
}

MenuTaskSubPhase MenuTask_GetSubPhase(void) {
    return (MenuTaskSubPhase)task[TASK_MENU].r_no[1];
}

int MenuTask_GetRNo(int idx) {
    assert(idx >= 0 && idx < 4);
    return task[TASK_MENU].r_no[idx];
}

/* ── Phase setters ─────────────────────────────────────────────── */

void MenuTask_SetPhase(MenuTaskPhase phase) {
    assert((unsigned)phase < MTP_PHASE_MAX_);
    task[TASK_MENU].r_no[0] = (u8)phase;
}

void MenuTask_SetSubPhase(MenuTaskSubPhase sub) {
    task[TASK_MENU].r_no[1] = (u8)sub;
}

/* ── Combined transitions ──────────────────────────────────────── */

void MenuTask_GotoPhase(MenuTaskPhase phase) {
    MenuTask_SetPhase(phase);
    MenuTask_SetSubPhase(MTSP_INIT);
}

/* ── Condition ─────────────────────────────────────────────────── */

bool MenuTask_IsActive(void) {
    return task[TASK_MENU].condition == 1;
}

void MenuTask_Activate(void) {
    task[TASK_MENU].condition = 1;
}

/* ── Legacy Interop ────────────────────────────────────────────── */

struct _TASK* MenuTask_GetTaskPtr(void) {
    return &task[TASK_MENU];
}
