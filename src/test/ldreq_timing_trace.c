#if DEBUG

/* Loader-timing invariance instrument — in-game capture side.
 * See ldreq_timing_trace.h for what it measures and why the
 * rollback-determinism harness cannot serve here. Driven by
 * tools/ldreq-timing/check_ldreq_timing.py.
 *
 * Naming rule (borrowed from rollback_determinism.c): every object with
 * static storage duration in this TU is prefixed ldt_. */

#include "test/ldreq_timing_trace.h"

#include "configuration.h"
#include "main.h"
#include "port/io/afs.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/system/sys_sub.h"
#include "structs.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Distinct from rollback_determinism.c's 4 so a plumbing failure here is
 * never confused with one there. */
#define LDT_EXIT_CODE_FAILED 5

extern u8 ldreq_result[294];
extern REQ q_ldreq[16];
extern u8 ldreq_break;

static bool ldt_init_attempted = false;
static bool ldt_active = false;
static FILE* ldt_file = NULL;
static uint32_t ldt_frame_index = 0;

static void ldt_fail(const char* what) {
    fprintf(stderr, "[ldreq-trace] FAILED: %s\n", what);
    fflush(stderr);
    exit(LDT_EXIT_CODE_FAILED);
}

static uint32_t ldt_djb2(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = 5381u;

    for (size_t i = 0; i < len; i++) {
        h = ((h << 5) + h) ^ (uint32_t)p[i];
    }

    return h;
}

static void ldt_init(void) {
    const TestRunnerConfiguration* test = &configuration.test;

    ldt_init_attempted = true;

    if (test->ldreq_trace_path == NULL) {
        return;
    }

    if (test->ldreq_trace_frames <= 0) {
        ldt_fail("--ldreq-trace requires --ldreq-trace-frames > 0");
    }

    ldt_file = fopen(test->ldreq_trace_path, "w");

    if (ldt_file == NULL) {
        ldt_fail("could not open --ldreq-trace output for writing");
    }

    /* Header carries the two knobs whose difference the driver is
     * testing, so a pair of traces is self-describing and the driver can
     * refuse to compare two runs that were configured identically (which
     * would trivially "pass"). */
    if (fprintf(ldt_file,
                "# ldreq-timing-trace v1 barrier_force=%d afs_inject_latency_ms=%d frames=%d\n"
                "frame,G_No0,G_No1,G_No2,G_No3,G_Timer,Exit_No,Exit_Timer,"
                "SP_No00,SP_No10,plt_req0,plt_req1,ldreq_result_h,head_be,head_type,head_rno,"
                "ldreq_break,pl_load,ldreq_clear\n",
                (int)Ldreq_BarrierActive(), AFS_GetInjectedLatencyMs(), test->ldreq_trace_frames) < 0) {
        ldt_fail("write failure on trace header");
    }

    ldt_active = true;
}

void LdreqTimingTrace_FrameEnd(void) {
    if (!ldt_init_attempted) {
        ldt_init();
    }

    if (!ldt_active) {
        return;
    }

    /* Check_PL_Load() and Check_LDREQ_Clear() are pure reads of
     * ldreq_result[] / q_ldreq[] (gd3rd.c:526-549, sys_sub.c:899-905) —
     * capturing them cannot perturb the run. They are recorded because
     * they are the exact expressions Exit_6th (sel_pl.c:1702-1707) gates
     * the SAVED Exit_No / Exit_Timer on. */
    if (fprintf(ldt_file,
                "%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%08x,%d,%d,%d,%d,%d,%d\n",
                ldt_frame_index,
                (int)G_No[0], (int)G_No[1], (int)G_No[2], (int)G_No[3],
                (int)G_Timer,
                (int)Exit_No, (int)Exit_Timer,
                (int)SP_No[0][0], (int)SP_No[1][0],
                (int)plt_req[0], (int)plt_req[1],
                ldt_djb2(ldreq_result, sizeof(ldreq_result)),
                (int)q_ldreq[0].be, (int)q_ldreq[0].type, (int)q_ldreq[0].rno,
                (int)ldreq_break,
                (int)Check_PL_Load(),
                (int)Check_LDREQ_Clear()) < 0) {
        ldt_fail("write failure on trace row");
    }

    ldt_frame_index += 1;

    if ((int)ldt_frame_index >= configuration.test.ldreq_trace_frames) {
        if (fflush(ldt_file) != 0 || fclose(ldt_file) != 0) {
            ldt_fail("flush/close failure on trace output");
        }

        ldt_file = NULL;
        ldt_active = false;

        printf("[ldreq-trace] capture complete: frames=%u barrier=%d latency_ms=%d out=%s\n",
               ldt_frame_index, (int)Ldreq_BarrierActive(), AFS_GetInjectedLatencyMs(),
               configuration.test.ldreq_trace_path);
        fflush(stdout);

        /* Same clean shutdown the rbd capture and the perf capture use:
         * push SDL_EVENT_QUIT so the main loop ends next tick with exit
         * code 0. Failures never ride this path. */
        SDLApp_Exit();
    }
}

#else /* !DEBUG — inert stub (args.c rejects --ldreq-trace here) */

#include "test/ldreq_timing_trace.h"

void LdreqTimingTrace_FrameEnd(void) {}

#endif
