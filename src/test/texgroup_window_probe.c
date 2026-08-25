#include "test/texgroup_window_probe.h"

#if ENABLE_TEXGROUP_WINDOW_PROBE

#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "sf33rd/Source/Game/engine/charid.h"
#include "sf33rd/Source/Game/rendering/texgroup.h"
#include "sf33rd/Source/Game/system/ramcnt.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/system/work_sys.h"

#include <stdlib.h>
#include <string.h>

#define TGWP_CID_SLOTS 23 /* char_init_data[23]      — charid.c:10 */
#define TGWP_PAR_SLOTS 20  /* parabora_own_table[20]  — charid.c:11 */
#define TGWP_CID_PTRS 25   /* CharInitData members    — structs.h:1212-1238 */

typedef struct {
    int open;
    uintptr_t lo;
    uintptr_t hi;
    u8 grp;
    s16 key;
    u32 open_timer;
    unsigned long open_tick;
    int ptrs_in_range;
    int reads_while_open;
} TgwpWindow;

static TgwpWindow tgwp_cid[TGWP_CID_SLOTS];
static TgwpWindow tgwp_par[TGWP_PAR_SLOTS];

static unsigned long tgwp_reclaims;          /* case-2 reclaims observed */
static unsigned long tgwp_reclaims_with_cid; /* ... that stranded >=1 cid slot */
static unsigned long tgwp_reclaims_with_par;
static unsigned long tgwp_windows_opened;
static unsigned long tgwp_windows_closed;
static unsigned long tgwp_windows_spanned_frame; /* closed on a later system_timer */
static unsigned long tgwp_cid_reads;
static unsigned long tgwp_par_reads;
static unsigned long tgwp_cid_reads_in_window; /* THE F3 HIT */
static unsigned long tgwp_par_reads_in_window; /* THE F3 HIT */
static unsigned long tgwp_refill_same_addr;
static unsigned long tgwp_refill_diff_addr;
static unsigned long tgwp_no_republish; /* closed with pointers still in freed range */

static unsigned long tgwp_goh_zero;           /* getObjectHeight ok==0 returns */
static unsigned long tgwp_goh_zero_tsukamare; /* ... under check_tsukamare_keizoku_check */
static int tgwp_in_tsukamare;
static unsigned long tgwp_tsukamare_calls;
static unsigned long tgwp_logic_ticks;      /* Main_Jmp_Tbl dispatches executed */
static unsigned long tgwp_windows_spanned_logic; /* closed after >=1 further dispatch */
static unsigned long tgwp_open_barriered;   /* reclaims taken with the ldreq barrier ON */
static unsigned long tgwp_open_unbarriered;

static void tgwp_atexit(void) {
    TGWP_Report("atexit");
}

__attribute__((constructor)) static void tgwp_install(void) {
    atexit(tgwp_atexit);
}

static int tgwp_ptrs_in_range(const uintptr_t* words, int n, uintptr_t lo, uintptr_t hi) {
    int hits = 0;
    int i;

    for (i = 0; i < n; i++) {
        if (words[i] >= lo && words[i] < hi) {
            hits += 1;
        }
    }

    return hits;
}

void TGWP_LogicTick(void) {
    tgwp_logic_ticks += 1;
}

void TGWP_ReclaimOpen(u8 grp, s16 oldkey, s16 req_id, s16 ix, s16 ix1st) {
    uintptr_t lo;
    uintptr_t hi;
    size_t size;
    int i;
    int cid_hits = 0;
    int par_hits = 0;

    tgwp_reclaims += 1;

    if (Ldreq_BarrierActive()) {
        tgwp_open_barriered += 1;
    } else {
        tgwp_open_unbarriered += 1;
    }

    lo = Get_ramcnt_address(oldkey);
    size = Get_size_data_ramcnt_key(oldkey);
    hi = lo + size;

    for (i = 0; i < TGWP_CID_SLOTS; i++) {
        const uintptr_t* words = (const uintptr_t*)&char_init_data[i];
        int n = tgwp_ptrs_in_range(words, TGWP_CID_PTRS, lo, hi);

        if (n == 0) {
            continue;
        }

        cid_hits += 1;

        if (tgwp_cid[i].open) {
            flLogOut("[tgwp] WARN cid slot=%d re-opened while already open (grp=%d over grp=%d)\n", i, (int)grp,
                     (int)tgwp_cid[i].grp);
        }

        tgwp_cid[i].open = 1;
        tgwp_cid[i].lo = lo;
        tgwp_cid[i].hi = hi;
        tgwp_cid[i].grp = grp;
        tgwp_cid[i].key = oldkey;
        tgwp_cid[i].open_timer = system_timer;
        tgwp_cid[i].open_tick = tgwp_logic_ticks;
        tgwp_cid[i].ptrs_in_range = n;
        tgwp_cid[i].reads_while_open = 0;
        tgwp_windows_opened += 1;

        flLogOut("[tgwp] OPEN cid slot=%d ptrs=%d/%d grp=%d key=%d range=[%p,%p) timer=%u ix=%d ix1st=%d id=%d\n", i, n,
                 TGWP_CID_PTRS, (int)grp, (int)oldkey, (void*)lo, (void*)hi, (unsigned)system_timer, (int)ix, (int)ix1st,
                 (int)req_id);
    }

    for (i = 0; i < TGWP_PAR_SLOTS; i++) {
        uintptr_t p = (uintptr_t)parabora_own_table[i];

        if (p < lo || p >= hi) {
            continue;
        }

        par_hits += 1;
        tgwp_par[i].open = 1;
        tgwp_par[i].lo = lo;
        tgwp_par[i].hi = hi;
        tgwp_par[i].grp = grp;
        tgwp_par[i].key = oldkey;
        tgwp_par[i].open_timer = system_timer;
        tgwp_par[i].open_tick = tgwp_logic_ticks;
        tgwp_par[i].ptrs_in_range = 1;
        tgwp_par[i].reads_while_open = 0;
        tgwp_windows_opened += 1;

        flLogOut("[tgwp] OPEN par slot=%d grp=%d key=%d ptr=%p range=[%p,%p) timer=%u\n", i, (int)grp, (int)oldkey,
                 (void*)p, (void*)lo, (void*)hi, (unsigned)system_timer);
    }

    if (cid_hits) {
        tgwp_reclaims_with_cid += 1;
    }

    if (par_hits) {
        tgwp_reclaims_with_par += 1;
    }

    flLogOut("[tgwp] RECLAIM grp=%d key=%d size=%u ix=%d ix1st=%d id=%d cid_slots=%d par_slots=%d timer=%u tick=%lu "
             "barrier=%d\n",
             (int)grp, (int)oldkey, (unsigned)size, (int)ix, (int)ix1st, (int)req_id, cid_hits, par_hits,
             (unsigned)system_timer, tgwp_logic_ticks, Ldreq_BarrierActive() ? 1 : 0);
}

void TGWP_LoadComplete(u8 grp, s16 ix1st, s16 newkey) {
    uintptr_t nlo = Get_ramcnt_address(newkey);
    int i;

    for (i = 0; i < TGWP_CID_SLOTS; i++) {
        TgwpWindow* w = &tgwp_cid[i];
        const uintptr_t* words;
        int still;

        if (!w->open || w->grp != grp) {
            continue;
        }

        words = (const uintptr_t*)&char_init_data[i];
        still = tgwp_ptrs_in_range(words, TGWP_CID_PTRS, w->lo, w->hi);

        tgwp_windows_closed += 1;

        if (system_timer != w->open_timer) {
            tgwp_windows_spanned_frame += 1;
        }

        if (tgwp_logic_ticks != w->open_tick) {
            tgwp_windows_spanned_logic += 1;
        }

        if (nlo == w->lo) {
            tgwp_refill_same_addr += 1;
        } else {
            tgwp_refill_diff_addr += 1;
        }

        if (still != 0 && nlo != w->lo) {
            tgwp_no_republish += 1;
        }

        flLogOut("[tgwp] CLOSE cid slot=%d grp=%d ix1st=%d oldlo=%p newlo=%p same=%d still_in_range=%d/%d "
                 "reads_in_window=%d open_timer=%u close_timer=%u dframes=%d dlogic=%lu\n",
                 i, (int)grp, (int)ix1st, (void*)w->lo, (void*)nlo, nlo == w->lo ? 1 : 0, still, w->ptrs_in_range,
                 w->reads_while_open, (unsigned)w->open_timer, (unsigned)system_timer,
                 (int)(system_timer - w->open_timer), tgwp_logic_ticks - w->open_tick);

        w->open = 0;
    }

    for (i = 0; i < TGWP_PAR_SLOTS; i++) {
        TgwpWindow* w = &tgwp_par[i];
        uintptr_t p;

        if (!w->open || w->grp != grp) {
            continue;
        }

        p = (uintptr_t)parabora_own_table[i];
        tgwp_windows_closed += 1;

        if (system_timer != w->open_timer) {
            tgwp_windows_spanned_frame += 1;
        }

        if (tgwp_logic_ticks != w->open_tick) {
            tgwp_windows_spanned_logic += 1;
        }

        flLogOut("[tgwp] CLOSE par slot=%d grp=%d ix1st=%d oldlo=%p newlo=%p same=%d still_in_range=%d "
                 "reads_in_window=%d dframes=%d dlogic=%lu\n",
                 i, (int)grp, (int)ix1st, (void*)w->lo, (void*)nlo, nlo == w->lo ? 1 : 0,
                 (p >= w->lo && p < w->hi) ? 1 : 0, w->reads_while_open, (int)(system_timer - w->open_timer),
                 tgwp_logic_ticks - w->open_tick);

        w->open = 0;
    }
}

void TGWP_ReadCid(s16 slot, const char* who) {
    TgwpWindow* w;

    tgwp_cid_reads += 1;

    if (slot < 0 || slot >= TGWP_CID_SLOTS) {
        return;
    }

    w = &tgwp_cid[slot];

    if (!w->open) {
        return;
    }

    tgwp_cid_reads_in_window += 1;
    w->reads_while_open += 1;

    if (w->reads_while_open <= 8) {
        const uintptr_t* words = (const uintptr_t*)&char_init_data[slot];
        int still = tgwp_ptrs_in_range(words, TGWP_CID_PTRS, w->lo, w->hi);

        flLogOut("[tgwp] *** F3 HIT cid *** who=%s slot=%d grp=%d still_in_range=%d/%d range=[%p,%p) "
                 "open_timer=%u now=%u dframes=%d\n",
                 who, (int)slot, (int)w->grp, still, w->ptrs_in_range, (void*)w->lo, (void*)w->hi,
                 (unsigned)w->open_timer, (unsigned)system_timer, (int)(system_timer - w->open_timer));
    }
}

void TGWP_ReadPara(s16 cid, const char* who) {
    TgwpWindow* w;

    tgwp_par_reads += 1;

    if (cid < 0 || cid >= TGWP_PAR_SLOTS) {
        return;
    }

    w = &tgwp_par[cid];

    if (!w->open) {
        return;
    }

    tgwp_par_reads_in_window += 1;
    w->reads_while_open += 1;

    if (w->reads_while_open <= 8) {
        uintptr_t p = (uintptr_t)parabora_own_table[cid];

        flLogOut("[tgwp] *** F3 HIT par *** who=%s slot=%d grp=%d ptr=%p still_in_range=%d range=[%p,%p) "
                 "open_timer=%u now=%u dframes=%d\n",
                 who, (int)cid, (int)w->grp, (void*)p, (p >= w->lo && p < w->hi) ? 1 : 0, (void*)w->lo, (void*)w->hi,
                 (unsigned)w->open_timer, (unsigned)system_timer, (int)(system_timer - w->open_timer));
    }
}

void TGWP_TsukamareEnter(void) {
    tgwp_tsukamare_calls += 1;
    tgwp_in_tsukamare = 1;
}

void TGWP_TsukamareExit(void) {
    tgwp_in_tsukamare = 0;
}

void TGWP_ObjectHeightZero(u16 cgnum, s32 grp) {
    tgwp_goh_zero += 1;

    if (tgwp_in_tsukamare) {
        tgwp_goh_zero_tsukamare += 1;

        flLogOut("[tgwp] *** F4 HIT *** getObjectHeight ok==0 under check_tsukamare_keizoku_check cgnum=%u grp=%d "
                 "timer=%u\n",
                 (unsigned)cgnum, (int)grp, (unsigned)system_timer);
    }
}

void TGWP_Report(const char* tag) {
    int i;
    int open_now = 0;

    for (i = 0; i < TGWP_CID_SLOTS; i++) {
        open_now += tgwp_cid[i].open;
    }

    for (i = 0; i < TGWP_PAR_SLOTS; i++) {
        open_now += tgwp_par[i].open;
    }

    flLogOut("[tgwp-report] %s reclaims=%lu with_cid=%lu with_par=%lu | windows opened=%lu closed=%lu still_open=%d "
             "spanned_frame=%lu | refill same=%lu diff=%lu no_republish=%lu | reads cid=%lu par=%lu | IN-WINDOW "
             "cid=%lu par=%lu | F4 tsukamare_calls=%lu goh_zero=%lu goh_zero_in_tsukamare=%lu\n",
             tag ? tag : "", tgwp_reclaims, tgwp_reclaims_with_cid, tgwp_reclaims_with_par, tgwp_windows_opened,
             tgwp_windows_closed, open_now, tgwp_windows_spanned_frame, tgwp_refill_same_addr, tgwp_refill_diff_addr,
             tgwp_no_republish, tgwp_cid_reads, tgwp_par_reads, tgwp_cid_reads_in_window, tgwp_par_reads_in_window,
             tgwp_tsukamare_calls, tgwp_goh_zero, tgwp_goh_zero_tsukamare);
}

#endif /* ENABLE_TEXGROUP_WINDOW_PROBE */
