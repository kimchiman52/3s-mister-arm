#if defined(DEBUG)

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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Distinct from rollback_determinism.c's 4 so a plumbing failure here is
 * never confused with one there. */
#define LDT_EXIT_CODE_FAILED 5

extern u8 ldreq_result[294];
extern REQ q_ldreq[16];
extern u8 ldreq_break;

static bool ldt_init_attempted = false;
static bool ldt_active = false;
static FILE* ldt_file = NULL;
static FILE* ldt_slot_file = NULL;
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

/* === Per-slot q_ldreq residue probe (task #69.2) ===
 *
 * The main trace above records the HEAD slot only, which is enough to
 * gate timing invariance but cannot answer the question task #69.2 asks:
 * when the barrier is on and the two latency runs still disagree about
 * q_ldreq, are the differing bytes confined to slots that are DRAINED
 * (be == 0) in BOTH runs, or is there live residue?
 *
 * Two things make a naive byte-for-byte dump useless here, and both are
 * handled explicitly rather than hidden:
 *
 *   1. REQ carries two pointers (result, lds). Their raw values are heap
 *      / data-segment addresses and differ between ANY two processes on
 *      a macOS host (ASLR is not disabled by this harness — see
 *      check_ldreq_timing.py, which sets only SDL_VIDEODRIVER and
 *      SDL_AUDIODRIVER). Dumping them raw would make every populated
 *      slot "differ" for a reason that has nothing to do with timing.
 *      So the byte image zeroes both pointer fields and the decoded
 *      columns carry `result` as an INDEX into ldreq_result[] (which is
 *      exactly its meaning — gd3rd.c:422, :486) and `lds` as nullness.
 *
 *   2. Push_LDREQ_Queue_Player (gd3rd.c:405-432) fills a stack-local
 *      `REQ ldreq` field by field and leaves rno/retry/kokey/size/sect/
 *      fnum/free[]/lds/info and every padding hole uninitialised;
 *      Push_LDREQ_Queue then does `q_ldreq[i] = ldreq[0]` (gd3rd.c:559),
 *      a whole-struct copy. Stack garbage and padding therefore land in
 *      the queue. The analyser names every differing byte offset via the
 *      fieldmap in the header, so a padding difference is reported AS
 *      padding and never confused with a semantic field.
 */
static const struct {
    const char* name;
    size_t off;
    size_t len;
} ldt_req_fields[] = {
    { "be", offsetof(REQ, be), sizeof(((REQ*)0)->be) },
    { "type", offsetof(REQ, type), sizeof(((REQ*)0)->type) },
    { "id", offsetof(REQ, id), sizeof(((REQ*)0)->id) },
    { "rno", offsetof(REQ, rno), sizeof(((REQ*)0)->rno) },
    { "retry", offsetof(REQ, retry), sizeof(((REQ*)0)->retry) },
    { "ix", offsetof(REQ, ix), sizeof(((REQ*)0)->ix) },
    { "frre", offsetof(REQ, frre), sizeof(((REQ*)0)->frre) },
    { "key", offsetof(REQ, key), sizeof(((REQ*)0)->key) },
    { "kokey", offsetof(REQ, kokey), sizeof(((REQ*)0)->kokey) },
    { "group", offsetof(REQ, group), sizeof(((REQ*)0)->group) },
    { "result", offsetof(REQ, result), sizeof(((REQ*)0)->result) },
    { "size", offsetof(REQ, size), sizeof(((REQ*)0)->size) },
    { "sect", offsetof(REQ, sect), sizeof(((REQ*)0)->sect) },
    { "fnum", offsetof(REQ, fnum), sizeof(((REQ*)0)->fnum) },
    { "free", offsetof(REQ, free), sizeof(((REQ*)0)->free) },
    { "lds", offsetof(REQ, lds), sizeof(((REQ*)0)->lds) },
    { "info.number", offsetof(REQ, info.number), sizeof(((REQ*)0)->info.number) },
    { "info.size", offsetof(REQ, info.size), sizeof(((REQ*)0)->info.size) },
};

static void ldt_emit_slot_rows(void) {
    unsigned char image[sizeof(REQ)];

    for (int slot = 0; slot < 16; slot++) {
        const REQ* r = &q_ldreq[slot];

        memcpy(image, r, sizeof(REQ));
        memset(image + offsetof(REQ, result), 0, sizeof(r->result));
        memset(image + offsetof(REQ, lds), 0, sizeof(r->lds));

        if (fprintf(ldt_slot_file, "%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%ld,%d,%d,%d,%d,%d,%d,%u,%u,",
                    ldt_frame_index, slot,
                    (int)r->be, (int)r->type, (int)r->id, (int)r->rno, (int)r->retry,
                    (int)r->ix, (int)r->frre, (int)r->key, (int)r->kokey, (int)r->group,
                    (r->result == NULL) ? -1L : (long)(r->result - ldreq_result),
                    (int)r->size, (int)r->sect, (int)r->fnum,
                    (int)r->free[0], (int)r->free[1], (r->lds == NULL) ? 0 : 1,
                    (unsigned)r->info.number, (unsigned)r->info.size) < 0) {
            ldt_fail("write failure on slot-trace row");
        }

        for (size_t b = 0; b < sizeof(REQ); b++) {
            if (fprintf(ldt_slot_file, "%02x", image[b]) < 0) {
                ldt_fail("write failure on slot-trace raw image");
            }
        }

        if (fputc('\n', ldt_slot_file) == EOF) {
            ldt_fail("write failure on slot-trace row terminator");
        }
    }
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

    if (test->ldreq_slot_trace_path != NULL) {
        ldt_slot_file = fopen(test->ldreq_slot_trace_path, "w");

        if (ldt_slot_file == NULL) {
            ldt_fail("could not open --ldreq-slot-trace output for writing");
        }

        if (fprintf(ldt_slot_file,
                    "# ldreq-slot-trace v1 barrier_force=%d afs_inject_latency_ms=%d frames=%d sizeof_REQ=%d\n",
                    (int)Ldreq_BarrierActive(), AFS_GetInjectedLatencyMs(), test->ldreq_trace_frames,
                    (int)sizeof(REQ)) < 0) {
            ldt_fail("write failure on slot-trace header");
        }

        /* Byte-offset map so the analyser can name every differing byte,
         * including the padding holes, without hard-coding this ABI. */
        if (fprintf(ldt_slot_file, "# fieldmap") < 0) {
            ldt_fail("write failure on slot-trace fieldmap");
        }

        for (size_t i = 0; i < sizeof(ldt_req_fields) / sizeof(ldt_req_fields[0]); i++) {
            if (fprintf(ldt_slot_file, " %s:%d:%d", ldt_req_fields[i].name, (int)ldt_req_fields[i].off,
                        (int)ldt_req_fields[i].len) < 0) {
                ldt_fail("write failure on slot-trace fieldmap");
            }
        }

        if (fprintf(ldt_slot_file,
                    "\n"
                    "frame,slot,be,type,id,rno,retry,ix,frre,key,kokey,group,result_ix,"
                    "size,sect,fnum,free0,free1,lds_nonnull,info_number,info_size,raw\n") < 0) {
            ldt_fail("write failure on slot-trace column header");
        }
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
     * ldreq_result[] / q_ldreq[] (gd3rd.c:943-945, sys_sub.c:898-904) —
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

    if (ldt_slot_file != NULL) {
        ldt_emit_slot_rows();
    }

    ldt_frame_index += 1;

    if ((int)ldt_frame_index >= configuration.test.ldreq_trace_frames) {
        if (fflush(ldt_file) != 0 || fclose(ldt_file) != 0) {
            ldt_fail("flush/close failure on trace output");
        }

        if (ldt_slot_file != NULL) {
            if (fflush(ldt_slot_file) != 0 || fclose(ldt_slot_file) != 0) {
                ldt_fail("flush/close failure on slot-trace output");
            }

            ldt_slot_file = NULL;
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
