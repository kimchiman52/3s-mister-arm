/**
 * @file menu_task.h
 * @brief Accessor functions for cross-slot task[TASK_MENU] access.
 *
 * Phase 2 of the TASK system modernization.  All external code that
 * reads or writes task[TASK_MENU].r_no[] or .condition should use
 * these helpers instead of touching the struct directly.
 *
 * This keeps the struct layout unchanged (netplay rollback safe)
 * while routing cross-slot mutations through a single chokepoint.
 */
#ifndef MENU_TASK_H
#define MENU_TASK_H

/*
 * 3sx-mister include-path fixup: upstream splits the MenuTaskPhase /
 * MenuTaskSubPhase enums into `sf33rd/Source/Game/menu/menu_task_phases.h`.
 * That header is not yet ported into our tree (Phase 6 Step 4 ripple),
 * and Step 3 is constrained to 10 files total (plan §Verification #3).
 * Inline the contents here verbatim so the accessor layer compiles
 * against the current tree. When Step 4 lands the external header,
 * swap this block for `#include "sf33rd/Source/Game/menu/menu_task_phases.h"`.
 */

/**
 * @brief Top-level phases of the Menu task (task[TASK_MENU].r_no[0]).
 *
 * The menu task uses r_no[0] as the primary state selector.
 * Each value corresponds to a different dispatcher function
 * inside Menu_Task().
 */
typedef enum MenuTaskPhase {
    MTP_AFTER_TITLE = 0,     /**< menu.c: After_Title() — initial post-title dispatcher */
    MTP_IDLE = 1,            /**< pause.c: re-activate menu after pause exit */
    MTP_NETPLAY_IDLE = 5,    /**< netplay.c: park menu in idle routine during matchmaking */
    MTP_IN_GAME = 7,         /**< manage.c: in-game menu (pause UI, training, etc.) */
    MTP_SCREEN_DISPATCH = 8, /**< game.c: post-match/pre-match screen dispatch */
    MTP_GOTO_GAME = 9,       /**< game.c: transition into gameplay */
    MTP_TRAINING = 10,       /**< manage.c/ioconv.c: training mode dispatcher */
    MTP_RESET = 13,          /**< sys_sub.c: soft reset flow */
    MTP_PHASE_MAX_           /**< sentinel — do not use as a value */
} MenuTaskPhase;

/**
 * @brief Cross-slot sub-phase constants for task[TASK_MENU].r_no[1].
 *
 * Only values written or checked by files OTHER than menu.c are named here.
 * Internal menu.c jump-table indices are left as raw values.
 */
typedef enum MenuTaskSubPhase {
    MTSP_INIT = 0,           /**< Initial sub-state after phase change */
    MTSP_MODE_SELECT = 1,    /**< Mode select screen (test_runner, menu.c) */
    MTSP_IN_GAME_ACTIVE = 7, /**< sys_sub.c: in-game menu is fully active */
    MTSP_SA_CUT = 16,        /**< game.c: super-arts select cut-in */
    MTSP_NETWORK_LOBBY = 21, /**< menu.c: network lobby entry (for reference) */
} MenuTaskSubPhase;

#include <stdbool.h>

/* ── Phase getters ─────────────────────────────────────────────── */

/** @brief Read task[TASK_MENU].r_no[0] as a MenuTaskPhase. */
MenuTaskPhase MenuTask_GetPhase(void);

/** @brief Read task[TASK_MENU].r_no[1] as a MenuTaskSubPhase. */
MenuTaskSubPhase MenuTask_GetSubPhase(void);

/** @brief Read a raw r_no slot (0–3). For internal sub-states not yet named. */
int MenuTask_GetRNo(int idx);

/* ── Phase setters ─────────────────────────────────────────────── */

/** @brief Write task[TASK_MENU].r_no[0]. */
void MenuTask_SetPhase(MenuTaskPhase phase);

/** @brief Write task[TASK_MENU].r_no[1]. */
void MenuTask_SetSubPhase(MenuTaskSubPhase sub);

/* ── Combined transitions ──────────────────────────────────────── */

/** @brief Set r_no[0] = phase, r_no[1] = MTSP_INIT. */
void MenuTask_GotoPhase(MenuTaskPhase phase);

/* ── Condition ─────────────────────────────────────────────────── */

/** @brief Returns true when task[TASK_MENU].condition == 1. */
bool MenuTask_IsActive(void);

/** @brief Activate the Menu task (condition = 1). */
void MenuTask_Activate(void);

/* ── Legacy Interop ────────────────────────────────────────────── */

/**
 * @brief Get the direct pointer to the Menu task struct.
 * @note Use only for passing to legacy functions (e.g., Menu_Init)
 *       that expect a struct _TASK* parameter.
 */
struct _TASK* MenuTask_GetTaskPtr(void);

#endif /* MENU_TASK_H */
