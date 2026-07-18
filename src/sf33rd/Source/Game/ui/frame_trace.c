#include "frame_trace.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "port/build_config.h"
#include "structs.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/pls01.h"
#include "sf33rd/Source/Game/engine/workuser.h"

#define FRAME_TRACE_DEFAULT_PATH "/tmp/3sx-frame-trace.log"
#define FRAME_TRACE_MAX_ROWS 200000

/* Phase 5 hygiene item 2 fflush decision (docs/plan-frame-data-harness.md):
 * emit_row()/frame_trace_annotate() fflush() every write. Now that both
 * are gated on Disp_Frame_Data (see frame_trace_tick()), this only runs
 * for a training-mode player who has explicitly turned the "FRAME DATA"
 * option on, or the harness. On MiSTer the default path is under /tmp,
 * which is tmpfs (RAM-backed, no SD-card wear) — see
 * docs/plan-stun-direct-p2p.md's netplay handoff notes, which document
 * the same fact for a different feature ("/tmp is tmpfs, safe"). An
 * opt-in diagnostic overlay doing a RAM-only fflush per row is an
 * acceptable, deliberately-chosen cost; it never runs for ordinary
 * play. */

/* Step H4a (docs/plan-frame-data-harness.md section 1.6): allow the
 * harness to redirect the trace into a per-run temp dir via
 * FRAME_TRACE_PATH so concurrent/consecutive runs can't cross-
 * contaminate each other's output. Falls back to the historical
 * hardcoded path when unset. */
static const char* frame_trace_path(void) {
    const char* override = getenv("FRAME_TRACE_PATH");
    if (override != NULL && override[0] != '\0') {
        return override;
    }
    return FRAME_TRACE_DEFAULT_PATH;
}

/* Opt-in gate: even with training mode + Disp_Frame_Data on, the trace is
 * a heavy per-frame diagnostic (~69-field fprintf + fflush() every active
 * frame) and was the prime frame-drop suspect during normal play (see
 * perf investigation). Require an explicit opt-in — the FRAME_TRACE_PATH
 * env var — before any formatting, I/O, or file open. The frame-data
 * harness (tools/frame-data/run.sh) already sets it, so the suite keeps
 * working unchanged; an ordinary training-mode player who enables the
 * FRAME DATA overlay pays zero trace I/O. */
static bool frame_trace_opt_in(void) {
    /* Resolved once and cached: this runs per active frame, and FRAME_TRACE_PATH
     * cannot change mid-run for our processes (the harness sets it before launch).
     * -1 = not yet resolved, 0/1 = cached result. */
    static int cached = -1;
    if (cached < 0) {
        const char* override = getenv("FRAME_TRACE_PATH");
        cached = (override != NULL && override[0] != '\0') ? 1 : 0;
    }
    return cached != 0;
}

static FILE* g_fp = NULL;
static bool g_open_attempted = false;
static unsigned long g_local_frame = 0;
static unsigned long g_rows_written = 0;

typedef struct {
    s16 rno1;
    s16 rno2;
    s16 cg_ix;
    s16 cg_att_ix;
    u16 cg_hit_ix;
    u16 cg_ja_atix;   /* engine's active-hitbox gate (hitcheck.c:1618) */
    u16 cg_ja_caix;   /* engine's catch-hitbox gate (throw active window) */
    u8  cg_cancel;    /* cancel-window flag — non-zero when cancellable */
    u8  cg_status;
    u8  att_hit_ok;
    u8  h_att_set;   /* 1 if h_att != attack_adrs (i.e. points to non-empty entry) */
    u8  h_han_set;
    u8  h_cat_set;
    u8  h_dumm_set;  /* h_dumm != dumm_adrs */
    u8  h_eat_set;   /* h_eat != attack_adrs */
    u8  h_cau_set;   /* h_cau != caught_adrs */
    s16 hit_stop;
    s16 dm_stop;
    s16 dm_count_up;
    s16 dm_guard_success;
    bool tsukami_f;
    bool tsukamare_f;
    u8  guard_flag;
    u8  guard_chuu;
    u8  paring_counter;
    u8  pat_status;
    u8  kow;
    u8  cg_ctr;       /* per-cell decrementing counter — at advance, reloads
                       * to the cell's full duration. cell duration = first
                       * cg_ctr value seen on a new cg_ix. */
    u16 cp_sw_new;    /* WORK_CP::sw_new — per-player rising-edge input bits
                       * at this tick. Lets us identify which button/direction
                       * triggered a move without needing the captor to tell us. */
    /* §13.2 diagnosis columns (added 2026-05-04). On the defender side
     * during ground-block (r1=1), these expose the sub-state machine
     * driving blockstun:
     *   rno3      — routine_no[3], the Damage_04000 sub-state
     *               (0=setup, 1=set pause, 2=count down, 3=release anim).
     *               See plpdm.c:309-352.
     *   cmwk14    — cmwk[14], the blockstun pause counter loaded from
     *               _guard_pause_table[ground/air][dm_attlv].
     *               See bin2obj/etc.c:3, plpdm.c case 1.
     *   cg_wca_ix — guard-release animation entry point. char_move_wca()
     *               (charset.c:237-244) sets cg_ix = (cg_wca_ix - 1)
     *               * cgd_type - cgd_type when the pause expires.
     * For investigating whether the cr.* low-block adv +2 (§13.2) is
     * block-specific or also fires on hit, we need these visible in
     * every row so HIT vs BLOCK traces can be diff'd cell-for-cell. */
    s16 rno3;
    s16 cmwk14;
    s16 cg_wca_ix;
    s16 x;
    s16 y;
} TraceSnap;

static TraceSnap g_prev[2];

static void snap_player(int i, TraceSnap* out) {
    const PLW* p = &plw[i];
    out->rno1             = p->wu.routine_no[1];
    out->rno2             = p->wu.routine_no[2];
    out->cg_ix            = p->wu.cg_ix;
    out->cg_att_ix        = p->wu.cg_att_ix;
    out->cg_hit_ix        = p->wu.cg_hit_ix;
    out->cg_ja_atix       = p->wu.cg_ja.atix;
    out->cg_ja_caix       = p->wu.cg_ja.caix;
    out->cg_cancel        = p->wu.cg_cancel;
    out->cg_status        = p->wu.cg_status;
    out->att_hit_ok       = p->wu.att_hit_ok;
    /* OR the engine's per-frame "hitbox active" flag with the post-tick
     * att_box dimension check. The flag fires for cell-transition frames
     * (catches moves like Q's MK/LK); the att_box check covers moves
     * whose dimensions survive the engine tick (HP, MP, HK). */
    out->h_att_set = fd_engine_hitbox_active[i] ? 1 : 0;
    if (out->h_att_set == 0 && p->wu.h_att != NULL) {
        for (int j = 0; j < 4; j++) {
            if (p->wu.h_att->att_box[j][1] != 0) {
                out->h_att_set = 1;
                break;
            }
        }
    }
    out->h_han_set        = (p->wu.h_han  != NULL && p->wu.h_han  != p->wu.hand_adrs)   ? 1 : 0;
    out->h_cat_set        = (p->wu.h_cat  != NULL && p->wu.h_cat  != p->wu.catch_adrs)  ? 1 : 0;
    out->h_dumm_set       = (p->wu.h_dumm != NULL && p->wu.h_dumm != p->wu.dumm_adrs)   ? 1 : 0;
    out->h_eat_set        = (p->wu.h_eat  != NULL && p->wu.h_eat  != p->wu.attack_adrs) ? 1 : 0;
    out->h_cau_set        = (p->wu.h_cau  != NULL && p->wu.h_cau  != p->wu.caught_adrs) ? 1 : 0;
    out->hit_stop         = p->wu.hit_stop;
    out->dm_stop          = p->wu.dm_stop;
    out->dm_count_up      = p->wu.dm_count_up;
    out->dm_guard_success = p->wu.dm_guard_success;
    out->tsukami_f        = p->tsukami_f;
    out->tsukamare_f      = p->tsukamare_f;
    out->guard_flag       = p->guard_flag;
    out->guard_chuu       = p->guard_chuu;
    out->paring_counter   = paring_counter[i];
    out->pat_status       = p->wu.pat_status;
    out->kow              = p->wu.kow;
    out->cg_ctr           = p->wu.cg_ctr;
    out->cp_sw_new        = (p->cp != NULL) ? p->cp->sw_new : (u16)0;
    out->rno3             = p->wu.routine_no[3];
    out->cmwk14           = p->wu.cmwk[14];
    out->cg_wca_ix        = p->wu.cg_wca_ix;
    out->x                = p->wu.xyz[0].disp.pos;
    out->y                = p->wu.xyz[1].disp.pos;
}

static bool snap_is_active(const TraceSnap* s) {
    return s->rno1 != 0
        || s->att_hit_ok != 0
        || s->hit_stop != 0
        || s->dm_stop != 0
        || s->tsukami_f
        || s->tsukamare_f;
}

static void emit_header(void) {
    fputs(
        "# 3sx frame trace\n"
        "# F=local-frame  GT=Game_timer\n"
        "# Per side: r1=routine_no[1] r2=routine_no[2] cgix=cg_ix cgatt=cg_att_ix\n"
        "#           cghi=cg_hit_ix jatix=cg_ja.atix jcaix=cg_ja.caix cgcan=cg_cancel\n"
        "#           cgst=cg_status athok=att_hit_ok\n"
        "#           hatt=h_att_nonempty hhan=h_han_nonempty hcat=h_cat_nonempty\n"
        "#           hdum=h_dumm_nonempty heat=h_eat_nonempty hcau=h_cau_nonempty\n"
        "#           hstop=hit_stop dstop=dm_stop dcnt=dm_count_up\n"
        "#           dgrd=dm_guard_success tsuk=tsukami_f tsmd=tsukamare_f\n"
        "#           gflg=guard_flag gchu=guard_chuu prc=paring_counter[i]\n"
        "#           pat=pat_status kow=kow cgctr=cg_ctr swnew=cp->sw_new (rising-edge inputs, hex)\n"
        "#           rno3=routine_no[3] cmwk14=cmwk[14] wcaix=cg_wca_ix\n"
        "#             (rno3/cmwk14/wcaix added 2026-05-04 for §13.2 cr.* low-block diagnosis;\n"
        "#              defender-side under r1=1, plpdm.c:309-352 + bin2obj/etc.c:3)\n"
        "#           x=xyz[0].disp.pos y=xyz[1].disp.pos\n"
        "# Trailing: sastop=sa_stop_check()\n"
        "# Annotations: lines beginning with `# ` interleaved with data rows. MOVE_START\n"
        "#   marks r1: 0 -> !=0 transition, FINAL marks fd_finalize() with computed numbers.\n"
        "F\tGT\t"
        "P1.r1\tP1.r2\tP1.cgix\tP1.cgatt\tP1.cghi\tP1.jatix\tP1.jcaix\tP1.cgcan\tP1.cgst\tP1.athok\tP1.hatt\tP1.hhan\tP1.hcat\tP1.hdum\tP1.heat\tP1.hcau\t"
        "P1.hstop\tP1.dstop\tP1.dcnt\tP1.dgrd\tP1.tsuk\tP1.tsmd\tP1.gflg\tP1.gchu\tP1.prc\tP1.pat\tP1.kow\tP1.cgctr\tP1.swnew\tP1.rno3\tP1.cmwk14\tP1.wcaix\tP1.x\tP1.y\t"
        "P2.r1\tP2.r2\tP2.cgix\tP2.cgatt\tP2.cghi\tP2.jatix\tP2.jcaix\tP2.cgcan\tP2.cgst\tP2.athok\tP2.hatt\tP2.hhan\tP2.hcat\tP2.hdum\tP2.heat\tP2.hcau\t"
        "P2.hstop\tP2.dstop\tP2.dcnt\tP2.dgrd\tP2.tsuk\tP2.tsmd\tP2.gflg\tP2.gchu\tP2.prc\tP2.pat\tP2.kow\tP2.cgctr\tP2.swnew\tP2.rno3\tP2.cmwk14\tP2.wcaix\tP2.x\tP2.y\t"
        "sastop\n",
        g_fp);
}

static void emit_row(const TraceSnap* p1, const TraceSnap* p2) {
    fprintf(g_fp,
        "%lu\t%u\t"
        "%d\t%d\t%d\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%d\t%d\t%d\t%d\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t0x%04x\t%d\t%d\t%d\t%d\t%d\t"
        "%d\t%d\t%d\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%d\t%d\t%d\t%d\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t0x%04x\t%d\t%d\t%d\t%d\t%d\t"
        "%d\n",
        g_local_frame, (unsigned)Game_timer,
        p1->rno1, p1->rno2, p1->cg_ix, p1->cg_att_ix,
        (unsigned)p1->cg_hit_ix, (unsigned)p1->cg_ja_atix, (unsigned)p1->cg_ja_caix, (unsigned)p1->cg_cancel,
        (unsigned)p1->cg_status, (unsigned)p1->att_hit_ok,
        (unsigned)p1->h_att_set, (unsigned)p1->h_han_set, (unsigned)p1->h_cat_set,
        (unsigned)p1->h_dumm_set, (unsigned)p1->h_eat_set, (unsigned)p1->h_cau_set,
        p1->hit_stop, p1->dm_stop, p1->dm_count_up, p1->dm_guard_success,
        (int)p1->tsukami_f, (int)p1->tsukamare_f, (unsigned)p1->guard_flag,
        (unsigned)p1->guard_chuu, (unsigned)p1->paring_counter,
        (unsigned)p1->pat_status, (unsigned)p1->kow,
        (unsigned)p1->cg_ctr, (unsigned)p1->cp_sw_new,
        (int)p1->rno3, (int)p1->cmwk14, (int)p1->cg_wca_ix,
        p1->x, p1->y,
        p2->rno1, p2->rno2, p2->cg_ix, p2->cg_att_ix,
        (unsigned)p2->cg_hit_ix, (unsigned)p2->cg_ja_atix, (unsigned)p2->cg_ja_caix, (unsigned)p2->cg_cancel,
        (unsigned)p2->cg_status, (unsigned)p2->att_hit_ok,
        (unsigned)p2->h_att_set, (unsigned)p2->h_han_set, (unsigned)p2->h_cat_set,
        (unsigned)p2->h_dumm_set, (unsigned)p2->h_eat_set, (unsigned)p2->h_cau_set,
        p2->hit_stop, p2->dm_stop, p2->dm_count_up, p2->dm_guard_success,
        (int)p2->tsukami_f, (int)p2->tsukamare_f, (unsigned)p2->guard_flag,
        (unsigned)p2->guard_chuu, (unsigned)p2->paring_counter,
        (unsigned)p2->pat_status, (unsigned)p2->kow,
        (unsigned)p2->cg_ctr, (unsigned)p2->cp_sw_new,
        (int)p2->rno3, (int)p2->cmwk14, (int)p2->cg_wca_ix,
        p2->x, p2->y,
        (int)sa_stop_check());
    fflush(g_fp);
}

/* Lazy-open the trace file. Shared by tick() and annotate() so an
 * annotation arriving before any data row still gets written. */
static void ensure_open(void) {
    if (g_open_attempted) return;
    g_open_attempted = true;
    const char* path = frame_trace_path();
    g_fp = fopen(path, "w");
    if (g_fp != NULL) {
        emit_header();
        fprintf(stderr, "[frame_trace] writing to %s\n", path);
    } else {
        fprintf(stderr, "[frame_trace] could not open %s\n", path);
    }
}

#if ENABLE_PERF_TELEMETRY
/* Wall-clock cost of frame_trace_tick(), sampled at the main.c call site
   (perf-overlay report §4).  Assigned once per game frame; read by the FPS
   overlay's debug telemetry. */
static uint64_t perf_tick_ns = 0;
void FrameTrace_SetPerfTickNs(uint64_t ns) { perf_tick_ns = ns; }
uint64_t FrameTrace_GetPerfTickNs(void) { return perf_tick_ns; }
#endif

void frame_trace_tick(void) {
    if (!Is_Training_Mode(Mode_Type)) {
        return;
    }
    /* Phase 5 hygiene item 2 (docs/plan-frame-data-harness.md): gate on
     * the training-menu "FRAME DATA" toggle (Disp_Frame_Data, sc_sub.c
     * draw-side already checks it) in addition to training mode, so an
     * ordinary training-mode player who never enables the overlay pays
     * zero trace I/O — no file open, no per-row fflush. The frame-data
     * harness (tools/frame-data/run.sh) pins Training[0/2].contents[0][1][6]
     * every frame for its scene preset (src/test/test_runner.c,
     * TestRunner_Epilogue), which rides the same production code path
     * Wait_Pause_in_Tr (menu.c) uses to latch Disp_Frame_Data at
     * gameplay-entry, so the harness keeps working without touching
     * Disp_Frame_Data directly. */
    if (!Disp_Frame_Data) {
        return;
    }
    if (!frame_trace_opt_in()) {
        return;
    }
    if (g_rows_written >= FRAME_TRACE_MAX_ROWS) {
        return;
    }

    TraceSnap now[2];
    snap_player(0, &now[0]);
    snap_player(1, &now[1]);

    bool now_active  = snap_is_active(&now[0]) || snap_is_active(&now[1]);
    bool prev_active = snap_is_active(&g_prev[0]) || snap_is_active(&g_prev[1]);

    if (now_active || prev_active) {
        ensure_open();
        if (g_fp != NULL) {
            emit_row(&now[0], &now[1]);
            g_rows_written++;
            if (g_rows_written == FRAME_TRACE_MAX_ROWS) {
                fprintf(stderr, "[frame_trace] reached %lu-row cap, stopping\n",
                        (unsigned long)FRAME_TRACE_MAX_ROWS);
            }
        }
        g_local_frame++;
    }

    g_prev[0] = now[0];
    g_prev[1] = now[1];
}

void frame_trace_annotate(const char* fmt, ...) {
    if (!Is_Training_Mode(Mode_Type)) return;
    if (!Disp_Frame_Data) return;  /* see frame_trace_tick() */
    if (!frame_trace_opt_in()) return;
    ensure_open();
    if (g_fp == NULL) return;

    /* Prefix with `# ` and current local-frame so annotations sort
     * inline with data rows: `# F=<frame> <message>`. */
    fprintf(g_fp, "# F=%lu ", g_local_frame);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_fp, fmt, ap);
    va_end(ap);
    fputc('\n', g_fp);
    fflush(g_fp);
}
