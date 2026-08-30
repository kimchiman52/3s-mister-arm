/**
 * @file effa5.c
 * Select timer runner
 */

#include "sf33rd/Source/Game/effect/effa5.h"
#include "common.h"
#include "constants.h"
#include "sf33rd/Source/Game/debug/Debug.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/ui/frame_trace.h"

s32 Check_Sleep_A5(WORK_Other* ewk);

static s16 bcdext;

u8 sbcd(u8 a, u8 b) {
    s16 c, d;

    if ((d = (b & 0xF) - (a & 0xF) - (bcdext & 1)) < 0) {
        d += 10;
        d |= 16;
    }

    c = (b & 0xF0) - (a & 0xF0) - (d & 0xF0);
    d &= 0xF;

    if ((d |= c) < 0) {
        d += 160;
        bcdext = 1;
    } else {
        bcdext = 0;
    }

    return d;
}

void effect_A5_move(WORK_Other* ewk) {
    /* Task #108. Observation only, env-gated on FD_SELECT_PROBE and inert
     * without it (frame_trace.c). Placed BEFORE the early return on purpose:
     * the fact the runner is entered and returns immediately in training mode
     * is the measurement -- 280 entries, zero ticks -- and it is invisible if
     * the probe sits after the return. Nothing below is changed. */
    frame_select_timer_probe(
        (int)Present_Mode, (int)ewk->wu.routine_no[0], (int)Unit_Of_Timer, (int)Select_Timer,
        (Present_Mode == 4 || Present_Mode == 5) ? 1 : 0);

    if (Present_Mode == 4 || Present_Mode == 5) {
        return;
    }

#if defined(DEBUG)
    if (Debug_w[24]) {
        return;
    }
#endif

    if (Break_Into) {
        return;
    }

    switch (ewk->wu.routine_no[0]) {
    case 0:
        if (Time_Stop == 0) {
            ewk->wu.routine_no[0]++;
        }

        break;

    case 1:
        if (!Check_Sleep_A5(ewk)) {
            break;
        }

        if (--Unit_Of_Timer) {
            break;
        }

        Unit_Of_Timer = UNIT_OF_TIMER_MAX;
        bcdext = 0;

        if ((Select_Timer = sbcd(1, Select_Timer)) == 0) {
            ewk->wu.routine_no[0]++;
            ewk->wu.dir_timer = 30;
        }

        break;

    case 2:
        if (!Check_Sleep_A5(ewk)) {
            break;
        }

        if (Select_Timer) {
            ewk->wu.routine_no[0] = 1;
            Unit_Of_Timer = UNIT_OF_TIMER_MAX;
        } else if (--ewk->wu.dir_timer == 0) {
            Time_Over = 1;
            ewk->wu.routine_no[0]++;
        }

        break;

    case 3:
        if (!Check_Sleep_A5(ewk)) {
            break;
        }

        Time_Over = 1;

        if (Select_Timer) {
            ewk->wu.routine_no[0] = 1;
            Unit_Of_Timer = UNIT_OF_TIMER_MAX;
        }

        break;

    default:
        push_effect_work(&ewk->wu);
        break;
    }
}

s32 Check_Sleep_A5(WORK_Other* ewk) {
    if (Time_Stop == 2) {
        ewk->wu.routine_no[0] = 0;
    }

    return 1;
}

s32 effect_A5_init() {
    WORK_Other* ewk;
    s16 ix;

    if ((ix = pull_effect_work(4)) == -1) {
        return -1;
    }

    ewk = (WORK_Other*)frw[ix];
    ewk->wu.be_flag = 1;
    ewk->wu.id = 105;
    return 0;
}
