#include "frame_data_overlay.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "structs.h"
#include "rendering/game_renderer.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/pls01.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/ui/frame_trace.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"

/* Used across the engine (sc_sub.c, demo00.c, etc.) without a public
 * header — forward-declare what we need. */
extern s32  SSGetDrawSizePro(const s8* str);

/*
 * Parry actionable-recovery (frames defender unavailable after parry).
 *   r2=31, 32 — verified at 16f via 17f autoparry intervals.
 *   r2=33    — arcade value 19f (engine produced 25f standing-defender).
 *   r2=34    — arcade value 26f (no observed events).
 *
 * TODO: verify r2=33 with crouching defender; verify r2=34 with airborne
 * defender. If engine differs, branch on defender pat_status / xyz[1].pos.
 */
#define PARRY_RECOV_HIGH_FRONT 16
#define PARRY_RECOV_HIGH_BACK  16
#define PARRY_RECOV_LOW        19
#define PARRY_RECOV_AIR        26
#define PARRY_RECOV_OTHER      16

/*
 * Layout on the 384x224 canvas. Bottom strip y=[186,210] is free between
 * the input-history HUD (left edge, y<=186) and the SA gauges (y>=210).
 */
#define FD_METER_LEN          72
/* BUFFER-1 (2026-07-13): capture depth, separate from the 72-cell DISPLAY
 * width above. M1 register (docs/frame-data-synthesis.md §12.2.4 :947-961)
 * proved 24 legs / 11 moves measure R short or 0 because raw[] stops
 * appending at FD_METER_LEN=72 while the move's recovery window is still
 * open — the capture-independent `busyr` diagnostic already equals the
 * oracle on every member. FD_CAPTURE_LEN bounds raw[]/meter_len/atk_cells/
 * def_cells (the MEASUREMENT side); FD_METER_LEN keeps its name, value,
 * and every DISPLAY use (meter geometry, FdLatched cell arrays, the
 * draw-row clamp) untouched — the visual meter stays 72 cells wide.
 * Bound derivation (plan §3.2): worst known M1 member (Ken SA2 busyr=61)
 * needs <=133; worst known saturated non-member (Yun SA1 BLOCK busyr=109)
 * needs <=181. 256 covers both with >40% margin. FD_CAPTURE_LEN=72 is
 * this fix's own mutation contract (§3.3): at 72 the binary must
 * reproduce today's suite byte-identically. */
#define FD_CAPTURE_LEN        256
#define FD_CELL_W             4
#define FD_CELL_H             6
#define FD_METER_TOTAL_W      (FD_METER_LEN * FD_CELL_W)
#define FD_METER_X_LEFT       ((384 - FD_METER_TOTAL_W) / 2)
#define FD_METER_Y_ATK        190
#define FD_METER_Y_DEF        198
#define FD_NUMERIC_Y          178
#define FD_TEXT_ATR           9
#define FD_TEXT_PRIO          1
#define FD_RECT_PRIO          2

#define FD_COL_WHITE          0xFFFFFFFFu
#define FD_COL_GREEN          0xFF40D040u
#define FD_COL_RED            0xFFFF2020u  /* matches engine's existing red palette (sc_data.c:124) */

#define FD_COL_BG             0xFF383838u  /* idle/empty — visible against busy backgrounds */
#define FD_COL_STARTUP        0xFF3060C0u  /* blue */
#define FD_COL_ACTIVE         0xFFE03030u  /* red */
#define FD_COL_CONTACT        0xFF800000u  /* dark red tick — distinct from FD_COL_ACTIVE within active phase */
#define FD_COL_RECOVERY       0xFFE0A030u  /* orange */
#define FD_COL_HIT            0xFFFF8040u  /* defender on hit edge */
#define FD_COL_BLOCK          0xFF80C0E0u  /* defender on block edge */
#define FD_COL_PARRY          0xFFE0E040u  /* defender on parry edge */
#define FD_COL_HITSTUN        0xFF902020u  /* defender during hitstun */
#define FD_COL_BLOCKSTUN      0xFF305080u  /* defender during blockstun */
#define FD_COL_THROW          0xFFC040C0u  /* throw state */

typedef enum {
    FD_OUTCOME_NONE = 0,
    FD_OUTCOME_WHIFF,
    FD_OUTCOME_HIT,
    FD_OUTCOME_BLOCK,
    FD_OUTCOME_PARRY
} FdOutcome;

typedef enum {
    FD_CELL_NONE = 0,
    FD_CELL_IDLE,
    FD_CELL_STARTUP,
    FD_CELL_ACTIVE,
    FD_CELL_CONTACT,    /* single-frame tick on attacker row marking moment of contact */
    FD_CELL_RECOVERY,
    FD_CELL_HIT,        /* defender contact-frame markers */
    FD_CELL_BLOCK,
    FD_CELL_PARRY,
    FD_CELL_HITSTUN,
    FD_CELL_BLOCKSTUN,
    FD_CELL_THROW
} FdCellKind;

typedef struct {
    s16 r1;
    s16 r2;
    s16 cg_att_ix;
    u16 cg_hit_ix;
    s16 hit_stop;
    s16 dm_stop;
    s16 dm_count_up;
    u8  h_att_set;        /* h_att != attack_adrs */
    u8  cg_cancel;        /* cancel-window flag — non-zero when cancellable */
    /* §13.17 lever N/O raw signals (RE-ANCHOR-1): `box_active` is the
     * any-of-four-nonzero predicate over the RAW `att_box[j][k]` s16s
     * (all 4 slots, all 4 components) — deliberately NOT `h_att_set`,
     * which ORs a cell-transition flag with a width-only dim check and is
     * proven to diverge from raw dims (ENGINE-10 shape). `busy` packs
     * `old_gdflag`/`guard_flag` as one word (`(old_gdflag<<8)|guard_flag`)
     * so its 771->768 transition is a single, cheap edge test. */
    bool box_active;
    u16  busy;
} FdSnap;

typedef struct {
    bool      active;          /* a move is being tracked right now */
    bool      is_throw;        /* throws are not metered */
    int       atk_idx;         /* 0 or 1 */
    s32       move_start;
    s32       first_active;    /* -1 until first active hitbox observed */
    s32       last_active;
    s32       attacker_idle;   /* -1 until attacker.r1 returns to 0 */
    s32       defender_idle;   /* hit/block: defender.r1 returns to 0 */
    /* §13.7.7 engine_a snapshot. Captured on the FIRST tick where
     * attacker_idle is set (regardless of which code path set it:
     * §13.5.1 cghi=1 cut, §13.6.1 partner-release, or natural r1=4→0
     * edge). Used by fd_finalize in place of the live counter so a
     * multi-move-merge case (defender stays in r1=1 past attacker
     * animation end, fresh attacker move starts before defender_idle)
     * does not let the next move's active cells inflate this move's A.
     * -1 until set. */
    s32       engine_a_at_atk_idle;
    FdOutcome event;
    s32       event_frame;
    s16       parry_r2;
    bool      partner_in_throw;

    /* Counts and meter cells are computed at finalize time from raw[] —
     * we need the whole-move history to pick the right active signal
     * (h_att for some moves, cg_hit_ix for moves where h_att never fires).
     * Per-frame raw state of the attacker (hitstop frames are skipped
     * before being added here, so raw[] is hitstop-free). */
    s32       startup_count;
    s32       active_count;
    s32       recovery_count;

    /* Multi-segment recovery cut (§13.5.1). When the engine concatenates
     * a "return-to-neutral" segment under the same r1=4 block (UOH and
     * Q's HCB family), `r1: 4 → 0` fires too late. We cut earlier on the
     * predicate "cgix-reset has fired AND cghi=1 has persisted ≥3 frames
     * since the reset." Duration gate distinguishes UOH (cghi=1 holds 6+
     * frames as a real cancel-window) from Dashing Leg / Dashing Head
     * (1-2-frame end-anim transient that arcade counts as recovery). */
    s16       prev_cgix;
    s32       cgix_reset_frame;       /* -1 until first cgix-reset under r1=4 */
    s32       cghi1_first_frame;      /* -1 until first cghi=1 after reset */
    int       cghi1_first_raw_slot;   /* raw_len at the moment cghi1_first hit */
    s32       cghi1_count;            /* total cghi=1 frames since reset */

    /* §13.5.1b guard-rearm commit gate (lever H): set true only in the tick
     * where the dwell commit actually fires (cghi1_count reaching 3 AND the
     * guard-rearm gate passing). fd_finalize() reads this instead of
     * re-deriving "committed" from cghi1_count >= 3, because on a REFUSED
     * commit (gate false) cghi1_count still reaches and stays >= 3 — without
     * this explicit flag a later dwellbrk with a positive anchor snapshot
     * would wrongly read as committed at finalize time. */
    bool      cut_committed;

    /* §13.5.1a cut-gate (2026-07-08): the dwell may only anchor on a
     * grounded fresh cghi->1 edge (see the tick-side block). prev_cghi/
     * prev_cghi_frame/prev_y sample the last tick seen by the r1=4/2
     * block, giving the edge test its "previous tick" operand. */
    s32       prev_cghi;              /* -1 until first sample */
    s32       prev_cghi_frame;        /* g_local_frame of that sample */
    s16       prev_y;                 /* wu.xyz[1].disp.pos at that sample */

    /* §13.7.1/§13.9.4 same-r1 retrigger fix: a second UOH-style press
     * landing while the attacker's r1 is still 4 opens no new MOVE_START
     * edge, so the engine accumulator (fd_engine_active_count) keeps
     * adding the retrigger's active cells onto the first tap's total.
     * `engine_a_at_cut_anchor` snapshots the accumulator at the FIRST
     * cghi=1 frame after the cgix-reset (the same anchor §13.5.1 already
     * uses to cut recovery) — before any retrigger's active phase can
     * begin — and `cghi1_dwell_broken` records whether that cghi=1 dwell
     * was interrupted (cghi left 1 while r1 stayed 4/2, before the cut
     * committed) rather than running the 3 consecutive frames a clean
     * UOH takes. Both fields are inert until fd_finalize() reads them;
     * see the gate at the effective-A computation there. */
    bool      cghi1_dwell_broken;
    s32       engine_a_at_cut_anchor; /* -1 until the anchor snapshot is taken */

    /* §13.6.1/Phase 4 item 4: set true only inside the partner-release
     * branch below (dp->r1==3 && dn->r1==1 mid cgix-reset). Explicit flag
     * rather than inferring from kd/def_r1 at finalize time — normal Throw
     * also lands on def_r1==3 at attacker_idle, which would be ambiguous.
     * fd_finalize() uses this to re-derive recovery from the engine credit
     * on this path only (see the R re-attribution comment there); it must
     * never be inferred, only set here. */
    bool      ended_by_partner_release;

    /* Phase 6A "arcade-split" projectile design
     * (/tmp/phase6-nonq-plan.md §2/Step 2). `proj_spawn_slot` latches
     * g_cur.raw_len (i.e. S, the player's startup frame budget) at the
     * tick the engine's fd_engine_proj_spawned flag is consumed — -1
     * until a spawn has been observed this move. `proj_seen` is a
     * separate latch (not just `proj_spawn_slot >= 0`) so the finalize
     * gate below reads unambiguously even though 0 is itself a valid
     * (if unusual) spawn slot. Both reset in fd_reset_move() so a stale
     * spawn from the previous move can never leak forward. */
    s32       proj_spawn_slot;
    bool      proj_seen;

    /* §13.12 (ENGINE-4, added 2026-07-09): "hit-checkable projectile
     * split" S/R anchors (lever J). `proj_athok_slot` latches
     * g_cur.raw_len PRE-raw[]-append (the athok tick's own offset) the
     * first tick the engine's fd_engine_proj_hitok flag is observed set
     * — -1 until latched. `proj_firstact_slot` latches the same way on
     * the first tick fd_engine_proj_active_count goes nonzero (the
     * tama's first atix!=0 tick) — -1 until latched. `proj_natural_end`
     * is set true when fd_engine_proj_natend fires for this move's tama
     * (chart reached its own scripted end, never cut to an erase chart —
     * see eff13.c's fd_tama_chart_cut() and charset.c's tama branch).
     * Together with proj_spawn_slot/proj_seen these select
     * S' = max(spawn, athok) and, on a chart-natural end,
     * R' = meter_len - (firstact + proj_a) instead of the fire-and-forget
     * R' = meter_len - S'. All three reset in fd_reset_move() so a stale
     * latch from the previous move can never leak forward — same
     * contract as proj_spawn_slot/proj_seen above. */
    s32       proj_athok_slot;
    s32       proj_firstact_slot;
    bool      proj_natural_end;

    /* §13.6.2b (ENGINE-3, added 2026-07-09): tick-side strike-KD latch.
     * The last-raw[]-cell test alone (`fd_is_knockdown_at_atk_idle`) can
     * miss a genuine down-family traversal that happens mid-window and
     * is over by the time the sample point arrives (measured instance:
     * urien-vkd-lk-hit, whose last cell is back at the ordinary-looking
     * def_r2==1 wakeup tail even though the defender passed through
     * def_r2==14 earlier in the same deferred-finalize window — see the
     * tick-side latch site below). `strike_kd_seen` is sticky for the
     * rest of the move once any tick observes `def_r1==1 &&
     * fd_r2_is_down_family(def_r2)` on a HIT outcome; `strike_kd_r2`
     * additionally records the FIRST such def_r2 value purely as a FINAL
     * diagnostic (`kdr2=%d`, -1 until latched) — additive, checker-
     * ignored, same precedent as `proj_a`/`proj_spawn_raw`. Both reset in
     * fd_reset_move(). */
    bool      strike_kd_seen;
    s16       strike_kd_r2;

    /* §13.17 lever N/O tick-side tracking (RE-ANCHOR-1, added 2026-07-11):
     * passive, outcome-independent latches derived purely from the RAW
     * `box_active`/`busy` signals (§13.17, `fd_snap_player`), never from
     * `h_att_set` or any overlay classification. `box_first`/`box_last`
     * are frame ids (-1 until observed); `box_count` tallies box-active
     * frames (hitstop ticks excluded, matching `twin_derive.py:116`);
     * `box_runs` counts contiguous box-active runs (multi-run whiff
     * windows are a construction-validity gate failure for both levers,
     * §2.3 — the §14 scattered-active precedent applied to the numeric
     * path). `busy_edge_frame` (-1 until latched) is the first busy
     * 771->768 transition observed at/after `box_last`; a later box run
     * starting after this latch invalidates it (§2.3 — the finalize-time
     * `busy_edge_frame > box_last` check, using the FINAL box_last, is
     * the second, independent check for the same failure mode).
     * `busyr_fallback` is a FINAL-line diagnostic only (`busyr_fb=%d`):
     * set when lever N's tick-side deferral hit its 20-tick bound or a
     * new-move edge before the busy edge latched, so fd_finalize()'s own
     * gate fell back to legacy `recovery_pf` (not a crash, not a special
     * value — the same reachable-only-via-fallback outcome any other
     * gate failure produces). All reset in fd_reset_move(). */
    s32       box_first;
    s32       box_last;
    s32       box_count;
    s32       box_runs;
    s32       busy_edge_frame;
    bool      busyr_fallback;

    /* CONTACT-2 Step 1 (2026-07-13, diagnostics only — design.md §1.3):
     * lever R's gate machinery, additive to lever N/O's box tracking above.
     * `hstop_after_box` (G5) is a passive tick-side counter: incremented on
     * freeze-excluded ticks observed while box_last >= 0 and
     * busy_edge_frame < 0 (i.e. attacker freeze between the last box frame
     * and a not-yet-latched busy edge); reset to 0 whenever box_last
     * advances. `move_is_uoh` (G4) is latched at MOVE_START from the
     * engine's own UOH dispatch tag (`fd_engine_move_is_uoh`, set by
     * check_leap_attack() right before hissatsu_setup_union(), pls03.c) —
     * the preferred, assumption-free form. `koc` (G6) is latched at
     * MOVE_START from `plw->wu.cmoa.koc` (the specials-class value set at
     * move-init, charset.c:100-102), the same way move_is_uoh is latched.
     * All reset in fd_reset_move(). Additive FINAL-line diagnostics only
     * (`hsab=%d uoh=%d koc=%d leverR_pred=%d`); never gate `recovery` this
     * step — see fd_contact_busy_edge_r above. */
    s32       hstop_after_box;
    bool      move_is_uoh;
    s32       koc;

    /* UOH-CLOSURE (2026-07-13, design.md <sp>/zero/uoh-fit/uoh-design.md
     * §3.3): `hstop_in_box`, a passive per-move tick-side counter distinct
     * from `hstop_after_box` above — it counts raw box-active ticks during
     * which the attacker's own hit_stop is nonzero (any sign), with NO
     * event-tick exemption (deliberately different from `in_hitstop`'s
     * exemption, which exists so box_last/A can count the event tick —
     * see the tick-side increment site). On the current verified universe
     * this is the freeze/box overlap the hardware discriminator measured
     * directly off tape (8 on all 33 UOH contact windows, 16 on the
     * two-contact chain-retrigger row; §2.1/§2.2 of the design). Read by
     * `fd_lever_r_applies`'s amended G4 disjunct (~line 1107-1109 below):
     * when lever T is on and this counter is >0 on a UOH move, the
     * disjunct is satisfied and the window is routed into the busy-edge
     * `recovery` arithmetic (the `else if (fd_lever_r_applies)` branch,
     * ~line 1170) the same way non-UOH contact windows already are — it
     * DOES gate the displayed `recovery` for UOH contact windows. Also
     * read independently by `fd_lever_t_applies`/`leverT_pred` in
     * fd_finalize as a cross-check/diagnostic that must always agree with
     * the G4 disjunct's own read (§8.1 tripwire); that second read is
     * diagnostic-only and does not itself gate anything. Reset in
     * fd_reset_move() alongside box_count/box_runs. */
    s32       hstop_in_box;

    int       raw_len;
    struct {
        u8  h_att_set;
        u16 cg_hit_ix;
        s16 def_r1;
        s16 def_r2;         /* §13.6.2 strike-KD: defender routine_no[2],
                              * needed alongside def_r1 to tell a down-family
                              * reaction (Damage_16000..23000 buttobi/blown-
                              * away dispatch) apart from ordinary hitstun
                              * (Damage_12000 standing hit-reaction) — both
                              * present as def_r1==1 at the sampling point. */
        s16 def_dm_stop;
        s16 def_hit_stop;
        s16 def_dm_count_up;
        bool event_this_frame;
    } raw[FD_CAPTURE_LEN];

    int       meter_len;
    u8        atk_cells[FD_CAPTURE_LEN];
    u8        def_cells[FD_CAPTURE_LEN];
} FdMove;

typedef struct {
    bool      valid;
    bool      is_throw;
    bool      kd;            /* §13.6/§13.6.2/§13.6.2b: defender still in
                              * r1=2/3 (thrown/grappled) at attacker_idle,
                              * OR strike-knocked-down — r1==1 with r2 in
                              * the measured down-family set
                              * (`fd_r2_is_down_family()`) on EITHER the
                              * last raw[] cell OR any tick of the
                              * finalize-deferred window (the tick-side
                              * latch, §13.6.2b) — display "KD" instead of
                              * a numeric advantage. The numeric
                              * `defender_idle - attacker_idle` is
                              * technically correct (time until partner
                              * gets back up) but useless to a player;
                              * arcade convention is "KD". */
    s32       startup;
    s32       active;
    s32       recovery;
    s32       total;
    s32       advantage;
    FdOutcome outcome;
    int       meter_len;
    u8        atk_cells[FD_METER_LEN];
    u8        def_cells[FD_METER_LEN];
} FdLatched;

static FdSnap     g_prev[2];
static bool       g_have_prev = false;
static s32        g_local_frame = 0;
static FdMove     g_cur;        /* in-progress */
static FdLatched  g_latched;    /* shown until next move starts */

/* §13.17 levers N/O (RE-ANCHOR-1, 2026-07-11): whiff-leg raw-signal R/A.
 * File-scope (unlike G/H/M/J's function-local consts) because lever N's
 * mutation contract must gate BOTH fd_finalize()'s R computation AND
 * frame_data_overlay_tick()'s deferral control-flow from the SAME single
 * toggle — a function-local copy in each site would let the gauntlet
 * disable one half without the other, breaking the "lever at 0 restores
 * today's behavior byte-identically" contract (§2.4). Lever O is
 * finalize-only (single site) but declared alongside N for readability. */
static const int fd_whiff_busy_edge_r = 1; /* lever N */
static const int fd_whiff_raw_box_a   = 1; /* lever O */
/* §13.18 lever S (ENGINE-D2, 2026-07-13): MOVE_START same-tick declared-credit
 * hold. See the reset-site comment at the fd_prev_active_cgix_tick[i] ==
 * Game_timer check below for the full mechanism; declared here alongside N/O
 * per house convention for file-scope mutation levers. */
static const int fd_movestart_same_tick_credit_hold = 1; /* lever S */
/* §13.19 lever R (CONTACT-2, 2026-07-13): contact-leg busy-edge R extension
 * (renumbered past lever S's §13.18, which landed just before this design).
 * (design: <sp>/zero/contact2/design.md). Step 1 added the §1.3 gate and
 * the predicted R value (FINAL-line `leverR_pred=%d` diagnostic) without
 * wiring the result into `recovery`. Step 2 (this commit) wires it: the
 * gate (now including G7, `recovery_pf > 0` — AMENDMENT 1) is applied to
 * `recovery` in the else-if chain below, between lever N's whiff branch
 * and the legacy `recovery_pf` fallback. Lever at 0 restores today's
 * display byte-identically (the gate's first conjunct short-circuits
 * false). */
static const int fd_contact_busy_edge_r = 1; /* lever R (CONTACT-2 Step 2) */
/* §13.20 lever T (UOH-CLOSURE, 2026-07-13): UOH contact-leg busy-edge R
 * extension (design: <sp>/zero/uoh-fit/uoh-design.md). Adds the passive
 * `hstop_in_box` counter above and wires it into the display path by
 * amending lever R's G4 term (`!move_is_uoh`) to
 * `!move_is_uoh || (fd_uoh_contact_busy_edge_t && hstop_in_box > 0)`
 * inside `fd_lever_r_applies` (design §3.2). When this lever is 1 and a
 * UOH contact window's raw box overlapped the attacker's own hit_stop
 * (`hstop_in_box > 0`), the window is routed into the same busy-edge
 * `recovery` arithmetic as any other lever-R contact window instead of
 * falling back to the flat UOH exclusion. `leverT_pred` (fd_finalize) is
 * kept as an independent cross-check/diagnostic that must agree with the
 * G4 disjunct's own read (§8.1 tripwire) — it does not itself gate
 * `recovery`; the gating happens through `fd_lever_r_applies`. Lever at 0
 * restores the flat `!move_is_uoh` exclusion token-for-token (no UOH
 * window can ever satisfy the disjunct), byte-identical to
 * pre-UOH-CLOSURE display. */
static const int fd_uoh_contact_busy_edge_t = 1; /* lever T (UOH-CLOSURE, WIRED — G4 disjunct in fd_lever_r_applies below) */
/* §13.21 lever U (ENGINE-JINCHU, 2026-07-14): jinchu-class bounce-recovery
 * contact-leg busy-edge R. Relaxes lever R's G7 (recovery_pf > 0) to admit the
 * airborne-handoff HIT legs whose legacy recovery tally is 0 because the move
 * ends by Player_normal dispatch while still airborne (never enters an in-engine
 * recovery state) — oro-jinchu-lk-hit + oro-exjinchu-hit, the ONLY recovery_pf==0
 * members among all lever-R gate firers (census-verified: the only two "0 -> N"
 * would-movers, <sp>/zero/contact2/step1/census-analysis.txt). The BLOCK legs are
 * NOT reachable — their busy 771->768 edge never latches (busyr=-1) so
 * fd_busy_edge_valid is false and they stay at legacy R=0. Arcade (Session 6):
 * base-LK HIT R=33 (busy-edge EXACT -> PASS); EX HIT R=34 (busy-edge 33, 1f short
 * -> stays XFAIL, engine recovers 1f faster than hardware). Lever at 0 restores
 * G7 to `recovery_pf > 0` byte-identically. */
static const int fd_jinchu_bounce_recovery_r = 1; /* lever U (ENGINE-JINCHU) */

static void fd_snap_player(int i, FdSnap* out) {
    const PLW* p = &plw[i];
    out->r1          = p->wu.routine_no[1];
    out->r2          = p->wu.routine_no[2];
    out->cg_att_ix   = p->wu.cg_att_ix;
    out->cg_hit_ix   = p->wu.cg_hit_ix;
    out->hit_stop    = p->wu.hit_stop;
    out->dm_stop     = p->wu.dm_stop;
    out->dm_count_up = p->wu.dm_count_up;
    out->cg_cancel   = p->wu.cg_cancel;
    /* Combine post-tick att_box dimension check (catches HP/MP/HK whose
     * boxes survive the tick) with the engine's per-frame flag (catches
     * cell-transition frames for moves like LK/MK whose box state gets
     * cleared before the overlay samples). */
    out->h_att_set = fd_engine_hitbox_active[i] ? 1 : 0;
    if (out->h_att_set == 0 && p->wu.h_att != NULL) {
        for (int j = 0; j < 4; j++) {
            if (p->wu.h_att->att_box[j][1] != 0) {
                out->h_att_set = 1;
                break;
            }
        }
    }
    /* Catch hitbox counts as an active signal (§13.6 — Q's Capture and
     * Deadly Blow uses caix, not atix). Treat caix > 0 as h_att_set so
     * downstream classification sees throws as active phases without
     * needing a parallel signal path. */
    if (out->h_att_set == 0 && p->wu.cg_ja.caix != 0) {
        out->h_att_set = 1;
    }
    /* §13.17 lever N/O raw signals (RE-ANCHOR-1): any-of-four-nonzero over
     * the RAW att_box s16s (all 4 slots, all 4 components each) — the
     * twin's own `attack_box_count()` construction, verbatim. Read only;
     * never feeds h_att_set or any existing classification. */
    out->box_active = false;
    if (p->wu.h_att != NULL) {
        for (int j = 0; j < 4 && !out->box_active; j++) {
            for (int k = 0; k < 4; k++) {
                if (p->wu.h_att->att_box[j][k] != 0) {
                    out->box_active = true;
                    break;
                }
            }
        }
    }
    out->busy = (u16)(((u16)p->old_gdflag << 8) | (u16)p->guard_flag);
}

static s32 fd_parry_recovery_for(s16 r2) {
    switch (r2) {
        case 31: return PARRY_RECOV_HIGH_FRONT;
        case 32: return PARRY_RECOV_HIGH_BACK;
        case 33: return PARRY_RECOV_LOW;
        case 34: return PARRY_RECOV_AIR;
        default: return PARRY_RECOV_OTHER;
    }
}

static void fd_reset_move(void) {
    g_cur.active             = false;
    g_cur.is_throw           = false;
    g_cur.atk_idx            = -1;
    g_cur.move_start         = 0;
    g_cur.first_active       = -1;
    g_cur.last_active        = -1;
    g_cur.attacker_idle      = -1;
    g_cur.defender_idle      = -1;
    g_cur.engine_a_at_atk_idle = -1;
    g_cur.event              = FD_OUTCOME_NONE;
    g_cur.event_frame        = -1;
    g_cur.parry_r2           = 0;
    g_cur.partner_in_throw   = false;
    g_cur.startup_count      = 0;
    g_cur.active_count       = 0;
    g_cur.recovery_count     = 0;
    g_cur.prev_cgix             = -1;
    g_cur.cgix_reset_frame      = -1;
    g_cur.cghi1_first_frame     = -1;
    g_cur.cghi1_first_raw_slot  = 0;
    g_cur.cghi1_count           = 0;
    g_cur.cut_committed         = false;
    g_cur.cghi1_dwell_broken    = false;
    g_cur.prev_cghi             = -1;
    g_cur.prev_cghi_frame       = -1;
    g_cur.prev_y                = 0;
    g_cur.engine_a_at_cut_anchor = -1;
    g_cur.ended_by_partner_release = false;
    g_cur.proj_spawn_slot    = -1;
    g_cur.proj_seen          = false;
    g_cur.proj_athok_slot    = -1;
    g_cur.proj_firstact_slot = -1;
    g_cur.proj_natural_end   = false;
    g_cur.strike_kd_seen     = false;
    g_cur.strike_kd_r2       = -1;
    g_cur.box_first          = -1;
    g_cur.box_last           = -1;
    g_cur.box_count          = 0;
    g_cur.box_runs           = 0;
    g_cur.busy_edge_frame    = -1;
    g_cur.busyr_fallback     = false;
    g_cur.hstop_after_box    = 0;
    g_cur.move_is_uoh        = false;
    g_cur.koc                = -1;
    g_cur.hstop_in_box       = 0;
    g_cur.raw_len            = 0;
    g_cur.meter_len          = 0;
}

/* Classify a frame given the move-level decision of which active signal to use.
 * `use_hatt` is decided at finalize from whether h_att ever fired in the move. */
static u8 fd_classify_attacker_finalize(const u8 h_att_set, u16 cg_hit_ix,
                                        bool event_this_frame, bool use_hatt,
                                        bool* seen_active) {
    if (event_this_frame) {
        *seen_active = true;
        return FD_CELL_CONTACT;
    }
    const bool active = use_hatt ? (h_att_set != 0)
                                 : (cg_hit_ix != 1);
    if (active) {
        *seen_active = true;
        return FD_CELL_ACTIVE;
    }
    return (*seen_active) ? FD_CELL_RECOVERY : FD_CELL_STARTUP;
}

/* Phase 6A "arcade-split" projectile design (/tmp/phase6-nonq-plan.md §2).
 * The player WORK never fires its own active signal on these moves (the
 * attack lives on a separate effect WORK), so there's no h_att/cg_hit_ix
 * "active" phase to detect on the player's own cells at all — the
 * player's raw[] cells are STARTUP before the projectile spawns and
 * RECOVERY from the spawn frame on, with the CONTACT tick preserved on
 * the actual event frame (display/meter classification only — the
 * numeric S/R for this branch come from proj_spawn_slot directly in
 * fd_finalize(), not from counting these cells). */
static u8 fd_classify_attacker_proj_split(int i, s32 proj_spawn_slot, bool event_this_frame) {
    if (event_this_frame) {
        return FD_CELL_CONTACT;
    }
    return (i < proj_spawn_slot) ? FD_CELL_STARTUP : FD_CELL_RECOVERY;
}

static u8 fd_classify_defender(const FdSnap* d, const FdMove* m, bool event_this_frame) {
    if (d->r1 == 2 || d->r1 == 3) return FD_CELL_THROW;
    if (event_this_frame) {
        switch (m->event) {
            case FD_OUTCOME_HIT:   return FD_CELL_HIT;
            case FD_OUTCOME_BLOCK: return FD_CELL_BLOCK;
            case FD_OUTCOME_PARRY: return FD_CELL_PARRY;
            default: break;
        }
    }
    if (d->r1 == 1) {
        return (m->event == FD_OUTCOME_HIT) ? FD_CELL_HITSTUN : FD_CELL_BLOCKSTUN;
    }
    return FD_CELL_IDLE;
}

/* §13.6.2b (ENGINE-3, 2026-07-09): measured down-family set, and why it
 * must be sampled every tick of the finalize-deferred window rather than
 * only at the last raw[] cell. Supersedes §13.6.2's original {16, 19}
 * last-cell-only predicate (kept below as lever E's anchor clause).
 *
 * Mechanism (plpdm.c). A landed hit sets the defender's routine_no[1]=1
 * and routine_no[2]=<raw "kind"> in the same tick the attacker's contact
 * lands (`dm_reaction_init_set`, hitcheck.c:574-589 —
 * `routine_no[2] = as->wu.att.reaction`, the attacker's own move-data
 * reaction kind). On the defender's own next `Player_damage` dispatch
 * (routine_no[3]==0), `get_damage_reaction_data` (plpdm.c:1581-1619)
 * looks that raw kind up in `dm_reaction_table` (plpdm.c:103-121):
 * `dm_reaction_table[kind].r_no` (plpdm.c:1618-1619) is what's actually
 * stored back into routine_no[2] and what `plpdm_lv_00[32]`
 * (plpdm.c:157-162) dispatches on. That single lookup is the ONLY table
 * the ordinary path passes through — every strike-KD row in the corpus
 * takes it. A second table, `dd_convert[kind][dm_attlv]` (plpdm.c:123-
 * 153, an intermediate value — some of its own entries, e.g. 95/96/39
 * at plpdm.c:132-133, coincidentally look like they could be inside the
 * FINAL r_no range even though they are not it), pre-converts kind
 * BEFORE that same `dm_reaction_table` lookup runs, but ONLY when
 * `wk->dead_flag` is set (plpdm.c:1602-1607) — i.e. only on the
 * defender's KO'ing blow, never on the ordinary hits this predicate
 * measures. `plpdm_lv_00[14]`/
 * `[15]` are `Damage_14000` (slam-down); `[16]`..`[23]` are
 * `Damage_16000`..`Damage_23000` (buttobi/blown-away launch family, all
 * running `setup_butt_own_data` + buttobi landing checks) — INCLUDED.
 * `[24]`/`[25]`/`[26]` are EXCLUDED: 24 is a "zuru" shake reaction
 * (`Damage_24000`, plpdm.c:837-882, resets `zuru_timer` — a juggle-scale
 * wobble, not a knockdown); 25 is `kizetsu` stun/faint (`Damage_25000`,
 * plpdm.c:884-926, `grade_add_em_stun`) — both excluded on SEMANTIC
 * grounds (known not to be knockdowns). 26 (`Damage_26000`,
 * plpdm.c:928-975) DOES run the same `setup_butt_own_data` buttobi shape
 * as 16-23/27/28, but is excluded on CENSUS grounds only: no corpus
 * member (KD or non-KD) has ever traversed it, so unlike 24/25 its
 * exclusion isn't a confirmed semantic fact, only an unproven gap —
 * a future capture that lands on 26 needs its own investigation before
 * either including or ruling it out. `[27]`/`[28]` (`Damage_27000`/
 * `_28000`, down-state handlers measured only inside already-KD
 * contexts: airstampede, powerbomb, moonsault) are INCLUDED. `[12]`/
 * `[13]` (`Damage_12000` standing hit-reaction, plus `oki_select_
 * table2`'s quick-stand reuse of 12/13, plpdm.c:84,1281) and `[1]`..
 * `[11]` (stage-1/settling transients) are EXCLUDED.
 *
 * The measured set is `{14..23} ∪ {27, 28}` (`fd_r2_is_down_family`
 * below) — the FULL block is required, not a narrower "strike-measured-
 * only" subset: dropping even one member (14) would empty lever I's own
 * signature, since urien-vkd-lk-hit's entire proof that a tick-side
 * latch is needed rests on 14 specifically being in the set (see below).
 *
 * Why last-cell sampling alone isn't enough — two distinct timing
 * failures, both fixed by the same per-tick latch:
 *
 * 1. Two-stage damage charts. `get_damage_reaction_data` runs once, but
 *    some charts later RE-DISPATCH `routine_no[2] = wk->as->data_ix`
 *    (plpdm.c:1033, :1061) into a different down-family value than the
 *    one first assigned. A short-recovery attacker can go idle before
 *    that re-dispatch happens, so the last raw[] cell still reads the
 *    original (non-down-family, ordinary-looking) value.
 *
 * 2. The reverse timing failure — measured on urien-vkd-lk-hit. Violence
 *    Knee Drop's defender reaction is a DIRECT kind→r_no=14 dispatch (no
 *    two-stage detour): the down phase (`Damage_14000`) runs and
 *    COMPLETES — the defender wakes up and routine_no[2] returns to the
 *    ordinary-looking `1` (a wakeup-tail value, not a fresh hit) —
 *    BEFORE Urien's own long-recovery attacker animation finally goes
 *    idle. The last raw[] cell therefore reads `def_r1==1 / def_r2==1`,
 *    even though the defender spent real mid-window time at
 *    `def_r2==14`. This is the mirror image of failure 1 (there the
 *    window ends too early to ever reach the down family; here the
 *    window runs long enough to leave it again) but breaks the same
 *    last-cell test.
 *
 * Fix: a sticky per-tick latch (`FdMove.strike_kd_seen`) samples EVERY
 * tick of the already-existing finalize-deferred window (the overlay's
 * tick loop already runs past attacker_idle waiting on defender_idle on
 * HIT/BLOCK — see the raw-append gate and the finalize trigger below),
 * not just the last counted cell, at zero new plumbing cost. The last-
 * cell test is KEPT as lever E's anchor (a genuine, load-bearing
 * fallback — every currently-passing strike-KD row's own last cell is
 * already inside the set) but is no longer the ONLY test.
 *
 * Residual hazard (documented, not observed in any of the 19 corpora):
 * the raw pre-conversion "kind" value written by `dm_reaction_init_set`
 * (hitcheck.c:575) is overlay-visible in routine_no[2] for at least one
 * hitstop tick while routine_no[1] is already 1, before `get_damage_
 * reaction_data`'s conversion overwrites it — if a move's raw kind value
 * ever fell in 14..23 during that window, this latch would falsely
 * fire. Census-checked 2026-07-09: no such traversal occurs suite-wide
 * (every observed pre-conversion sample is outside the down-family
 * range) — a documented hazard for a future capture, not a present bug.
 */
static bool fd_r2_is_down_family(s16 r2) {
    return (r2 >= 14 && r2 <= 23) || r2 == 27 || r2 == 28;
}

/* §13.6/§13.6.2/§13.6.2b KD detection: at fd_finalize() time, raw[] has
 * stopped growing at attacker_idle (`attacker_already_idle` gate below).
 * The last appended raw[] cell's `def_r1`/`def_r2` reflect the
 * defender's routine_no[1]/[2] on the last counted frame before
 * attacker became idle; `g_cur.strike_kd_seen` (set by the tick-side
 * latch in the deferred-finalize window, §13.6.2b above) additionally
 * records whether ANY tick of that window saw a down-family traversal.
 *
 * Two independent families both count as "knocked down":
 *
 * 1. Throw/grapple (§13.6, unchanged). For Capture and Deadly Blow,
 *    attacker r1 transitions 4→2 and stays at 2 through the throw
 *    animation; meanwhile partner r1 sits at 3 (thrown). When the §13.5.1
 *    cghi=1 cut fires and sets attacker_idle, the partner is still in
 *    r1=3, so the last raw[] cell's def_r1 ∈ {2, 3} — exactly the KD
 *    signal we want. Also covers normal Throw (LP+LK): its defender is in
 *    the same r1=3 grapple family at attacker-idle.
 *
 * 2. Strike knockdown (§13.6.2, added 2026-07-07; latched over the full
 *    deferred window per §13.6.2b, added 2026-07-09). A swept/blown-away
 *    defender stays in `Player_damage` (r1=1) for the whole knockdown, so
 *    the throw test alone misses it — this was the Phase 4 item 3 bug
 *    (cr.HK and similar rendered a green "+N" instead of "KD"). A
 *    down-family reaction (`def_r1 == 1` AND `def_r2` in the measured set
 *    `{14..23} ∪ {27, 28}`, `fd_r2_is_down_family` above) is
 *    distinguishable from ordinary hitstun (`def_r2 == 12`) at either the
 *    last raw[] cell (lever E's anchor test, kept for its own load-
 *    bearing value) or, per §13.6.2b, at any tick of the deferred window
 *    via the sticky `strike_kd_seen` latch.
 *
 * Returns true iff the most recent recorded defender state is "thrown or
 * grappled" (r1 ∈ {2, 3}), or "strike-knocked-down" — either the latch
 * fired mid-window, or the last raw[] cell itself has r1 == 1 and r2 in
 * the measured down-family set. */
static bool fd_is_knockdown_at_atk_idle(void) {
    if (g_cur.raw_len <= 0) return false;
    const s16 last_def_r1 = g_cur.raw[g_cur.raw_len - 1].def_r1;
    const s16 last_def_r2 = g_cur.raw[g_cur.raw_len - 1].def_r2;
    if (last_def_r1 == 2 || last_def_r1 == 3) return true;
    if (g_cur.strike_kd_seen) return true;                        /* latch, §13.6.2b */
    if (last_def_r1 == 1 && fd_r2_is_down_family(last_def_r2))     /* lever E anchor */
        return true;
    return false;
}

static void fd_finalize(void) {
    g_latched.valid    = true;
    g_latched.is_throw = g_cur.is_throw;
    g_latched.kd       = false;

    if (g_cur.is_throw) {
        g_latched.startup   = 0;
        g_latched.active    = 0;
        g_latched.recovery  = 0;
        g_latched.total     = (g_cur.attacker_idle >= 0) ? (g_cur.attacker_idle - g_cur.move_start) : 0;
        g_latched.advantage = 0;
        g_latched.outcome   = g_cur.partner_in_throw ? FD_OUTCOME_HIT : FD_OUTCOME_WHIFF;
        g_latched.meter_len = 0;
        return;
    }

    /* Whole-move active-signal pick: h_att if it ever fired (covers most
     * moves), else cg_hit_ix != 1 (covers Q's close LK/MK and similar). */
    bool use_hatt = false;
    for (int i = 0; i < g_cur.raw_len; i++) {
        if (g_cur.raw[i].h_att_set) { use_hatt = true; break; }
    }

    /* Phase 6A "arcade-split" projectile design
     * (/tmp/phase6-nonq-plan.md §2/Step 2). A move where the player WORK
     * spawned a player-owned projectile (proj_seen, latched by the tick
     * side above) AND never fired its own h_att (use_hatt false — the
     * attack lives on the effect WORK, not the player's) takes precedence
     * over both the generic h_att/cg_hit_ix classification AND the
     * §13.3 no-active-signal guard below: without this branch, finalize
     * falls through to the `cg_hit_ix != 1` fallback and classifies
     * nearly the whole player animation ACTIVE (today's hadouken
     * S=0 A=42 R=4 garbage — Step 1 discovery, §1.3 probe P3). Takes
     * priority over no_active_signal too: a projectile that never
     * connects (event stays NONE) is still a real spawn, not a
     * whiff-command-grab-attempt false negative.
     *
     * Step-5 mutation lever: this gate is the finalize-time on/off switch
     * for the whole arcade-split design. Force it false (e.g. hardcode
     * `false` here) to reproduce the legacy pre-Phase-6A hadouken signature
     * (S=0 A=42 R=4) as a mutation-testing sanity check that the harness's
     * corpus actually exercises this branch. */
    const bool use_proj_split = g_cur.proj_seen && !use_hatt;

    /* §13.3 non-attack-move guard. When NO real active signal fires
     * (h_att never set, accumulator never incremented, no defender event),
     * the cghi-fallback would mark every cghi != 1 frame as ACTIVE — wrong
     * for taunt and for whiff command-grab attempts that never engage a
     * hitbox. Treat the whole move as STARTUP (no active phase) so the
     * numeric line shows S=raw_len A=0 R=0 instead of S=0 garbage. */
    const s32 engine_a_pre = (g_cur.atk_idx >= 0)
        ? (s32)fd_engine_active_count[g_cur.atk_idx] : 0;
    const bool no_active_signal = !use_hatt
        && engine_a_pre == 0
        && g_cur.event == FD_OUTCOME_NONE;

    bool seen_active = false;

    g_cur.meter_len = (g_cur.raw_len < FD_CAPTURE_LEN) ? g_cur.raw_len : FD_CAPTURE_LEN;
    for (int i = 0; i < g_cur.meter_len; i++) {
        u8 atk_cell;
        if (use_proj_split) {
            atk_cell = fd_classify_attacker_proj_split(
                i, g_cur.proj_spawn_slot, g_cur.raw[i].event_this_frame);
        } else if (no_active_signal) {
            atk_cell = FD_CELL_STARTUP;
        } else {
            atk_cell = fd_classify_attacker_finalize(
                g_cur.raw[i].h_att_set, g_cur.raw[i].cg_hit_ix,
                g_cur.raw[i].event_this_frame, use_hatt, &seen_active);
        }

        FdSnap dn = {0};
        dn.r1          = g_cur.raw[i].def_r1;
        dn.dm_stop     = g_cur.raw[i].def_dm_stop;
        dn.hit_stop    = g_cur.raw[i].def_hit_stop;
        dn.dm_count_up = g_cur.raw[i].def_dm_count_up;
        const u8 def_cell = fd_classify_defender(&dn, &g_cur, g_cur.raw[i].event_this_frame);

        g_cur.atk_cells[i] = atk_cell;
        g_cur.def_cells[i] = def_cell;
    }

    /* Tally per-frame classification BEFORE the meter override below.
     *   startup_pf  = frames before the first per-frame ACTIVE/CONTACT
     *   active_pf   = frames the engine flagged as a live hitbox in raw[]
     *   recovery_pf = frames after the active phase ended in raw[]
     * S/R for the numeric line read directly from these counts so they
     * match what the engine animation actually did, frame-for-frame.
     * A is overridden below to engine_a (the cgctr-accumulator sum) —
     * that is the canonical arcade-table A and the only source that
     * survives moves whose active cells get sub-frame'd by hit-detection
     * (close LK / close MK / close LP) or stretched by hitstop on a
     * 1-frame active cell (close HP). */
    s32 startup_pf  = 0;
    s32 active_pf   = 0;
    s32 recovery_pf = 0;
    int last_active_pf_idx = -1;
    for (int i = 0; i < g_cur.meter_len; i++) {
        switch (g_cur.atk_cells[i]) {
            case FD_CELL_STARTUP:  startup_pf++;  break;
            case FD_CELL_ACTIVE:
            case FD_CELL_CONTACT:  active_pf++; last_active_pf_idx = i; break;
            case FD_CELL_RECOVERY: recovery_pf++; break;
            default: break;
        }
    }

    /* §13.5.2 multi-hit recovery fix: for moves with disjoint active
     * windows (HSB Jab/Strong/Fierce), inter-hit gaps were appearing in
     * raw[] as RECOVERY because seen_active stays true and h_att=0
     * during the gap. Override recovery_pf to count only frames AFTER
     * the LAST active cell — matching arcade's "actionable frame"
     * convention. Verified non-regressing on single-active moves
     * (last_active_pf_idx == last ACTIVE cell, count after = today's
     * recovery_pf). */
    if (last_active_pf_idx >= 0) {
        s32 post_last_active_recovery = 0;
        for (int i = last_active_pf_idx + 1; i < g_cur.meter_len; i++) {
            if (g_cur.atk_cells[i] == FD_CELL_RECOVERY) {
                post_last_active_recovery++;
            }
        }
        recovery_pf = post_last_active_recovery;
    }

    /* Meter visualization: extend the active band forward from the first
     * per-frame ACTIVE/CONTACT cell to engine_active_count cells. Anchors
     * the contact tick at the actual event frame (which can differ from
     * first_active_idx for moves that connect mid-active, e.g. Q far MK
     * at distance). The numeric S/A/R below do NOT read from atk_cells —
     * they read from the per-frame tally above and engine_a — so this
     * override is purely cosmetic. */
    /* §13.7.7 prefer the engine_a snapshot taken at attacker_idle set,
     * not the live counter. In the multi-move-merge case (defender stays
     * in r1=1 past attacker animation end, finalize defers waiting on
     * defender_idle, fresh attacker move starts), the live counter has
     * been advanced by the next move's active cells; the snapshot has
     * not. Fall back to the live counter only if the snapshot was never
     * taken (defensive — shouldn't happen on the finalize path since
     * attacker_idle is required to reach finalize). */
    const s32 engine_a = (g_cur.engine_a_at_atk_idle >= 0)
        ? g_cur.engine_a_at_atk_idle
        : ((g_cur.atk_idx >= 0) ? (s32)fd_engine_active_count[g_cur.atk_idx] : 0);

    /* §13.7.1/§13.9.4 same-r1 retrigger fix. Override A with the
     * anchor-time engine_a snapshot ONLY when the cut actually committed
     * (the tick-side §13.5.1b commit block fired — G3) AND the cghi=1 dwell
     * that set the anchor was interrupted (the retrigger signature — see
     * the tick-side else-if chain) AND the snapshot was actually taken and
     * is positive (G4 fallback). Read `g_cur.cut_committed` (not
     * `cghi1_count >= 3`) here: since §13.5.1b (lever H), the dwell counter
     * still reaches and stays >= 3 on a REFUSED commit (guard-rearm gate
     * false), so the raw count alone would over-report "committed" for a
     * move that ultimately finalizes on the natural r1 edge instead. This
     * never fires on the §13.6.1 partner-release branch (that branch
     * doesn't touch cghi1_count/cghi1_first_frame/cut_committed at all), so
     * CnDB's A keeps reading engine_a untouched. */
    const bool cut_committed = g_cur.cut_committed;
    const bool use_anchor_a = cut_committed
        && g_cur.cghi1_dwell_broken
        && g_cur.engine_a_at_cut_anchor > 0;
    const s32 effective_a = use_anchor_a ? g_cur.engine_a_at_cut_anchor
                           : ((engine_a > 0) ? engine_a : active_pf);

    /* §13.17 levers N/O common gate (RE-ANCHOR-1): a WHIFF-outcome window
     * (g_cur.event == FD_OUTCOME_NONE — outcome defaults to WHIFF below
     * iff event stays NONE) whose raw box-active tally is a single
     * contiguous run. `box_runs == 1` is the SAME contiguity guard for
     * both levers (not two separate gates) — lever N's busy-edge
     * construction is validated only on single-run windows
     * (`twin_derive.py:118-133`); multi-run/proj-split/no-signal windows
     * fall through to today's paths, byte-identical. Contact legs never
     * take either branch (event != FD_OUTCOME_NONE). */
    const bool fd_whiff_common_gate =
        (g_cur.event == FD_OUTCOME_NONE)
        && !use_proj_split
        && !no_active_signal
        && g_cur.box_count > 0
        && g_cur.box_runs == 1;

    /* §13.17 lever O — whiff raw-box A (`fd_whiff_raw_box_a`). Displayed A
     * is the raw box-frame count (box_count) instead of the declared-
     * credit accumulator — proven arcade-exact on every measured whiff
     * row (LAYER-1 §13.16; census `<sp>/reanchor/census-report.md`,
     * lever-O predicted-drift table). Else falls through to today's
     * effective_a path, byte-identical. */
    const s32 displayed_a = (fd_whiff_raw_box_a && fd_whiff_common_gate)
        ? (s32)g_cur.box_count
        : effective_a;

    if (!g_cur.is_throw && (use_anchor_a || engine_a > 0)) {
        /* BUFFER-1 (2026-07-13): this block is cosmetic-paint-only (see
         * the header comment above) and must keep scanning/painting
         * exactly the first 72 cells regardless of how far capture itself
         * grows post-raise (up to FD_CAPTURE_LEN) — otherwise the meter's
         * first 72 display cells could repaint differently as a still-open
         * move's capture window keeps extending past slot 71. Bounding to
         * paint_len makes this block's behavior on cells [0,71] identical
         * BY CONSTRUCTION at every capture depth (trivially identical to
         * today's code whenever meter_len<=72 too — the FD_CAPTURE_LEN=72
         * mutation-contract identity case, plan §3.1). */
        const int paint_len = (g_cur.meter_len < FD_METER_LEN) ? g_cur.meter_len : FD_METER_LEN;
        int first_active_idx = -1;
        int last_active_idx_post = -1;
        int active_count_post = 0;
        for (int i = 0; i < paint_len; i++) {
            if (g_cur.atk_cells[i] == FD_CELL_ACTIVE || g_cur.atk_cells[i] == FD_CELL_CONTACT) {
                if (first_active_idx < 0) first_active_idx = i;
                last_active_idx_post = i;
                active_count_post++;
            }
        }
        int event_idx = -1;
        for (int i = 0; i < paint_len; i++) {
            if (g_cur.raw[i].event_this_frame) {
                event_idx = i;
                break;
            }
        }
        /* Skip the contiguous override for multi-hit moves whose ACTIVE
         * cells are scattered (HSB and similar). The per-frame painting
         * already shows individual hits; the contiguous band would
         * squash them. */
        const bool active_contiguous_post =
            (first_active_idx < 0)
            || ((last_active_idx_post - first_active_idx + 1) == active_count_post);
        if (active_contiguous_post && first_active_idx >= 0) {
            for (int i = first_active_idx; i < paint_len; i++) {
                const int off = i - first_active_idx;
                if (i == event_idx) {
                    g_cur.atk_cells[i] = FD_CELL_CONTACT;
                } else if (off < effective_a) {
                    g_cur.atk_cells[i] = FD_CELL_ACTIVE;
                } else {
                    g_cur.atk_cells[i] = FD_CELL_RECOVERY;
                }
            }
        }
    }

    /* Numeric counts: S and R from the engine's actual raw[] frame budget,
     * A from the canonical accumulator (or the anchor snapshot on a
     * committed, dwell-broken retrigger — see effective_a above). T may
     * differ from raw_len by ±N when the engine sub-frames active cells on
     * contact or stretches a 1-frame active cell with hitstop — that delta
     * is the gap between what the engine animated and what arcade tables
     * canonicalize.
     *
     * Phase 6A "arcade-split" branch (use_proj_split): S/A come from the
     * projectile machinery instead of the generic per-frame tally —
     * S = proj_spawn_slot (the player's startup budget up to the spawn
     * frame), A = the projectile's own owner-indexed accumulator
     * (fd_engine_proj_active_count), display-only per the plan (the
     * arcade oracle gives no numeric active-frame figure for a
     * projectile move, so this is never asserted against a corpus
     * expect.A). engine_a/effective_a stay 0 on this branch (the
     * player's own accumulator never fires — that's the whole reason
     * this branch exists), so falling through to them here would be
     * wrong, not just imprecise.
     *
     * §13.12 (ENGINE-4, lever J) "hit-checkable projectile split": on the
     * use_proj_split branch, S anchors on whichever slot is LATER —
     * proj_spawn_slot (the legacy anchor) or proj_athok_slot (the tama's
     * own hit-check-gated arming tick, latched pre-append on the tick
     * side above). Fireballs arm athok at birth (athok offset = spawn
     * slot - 1), so max() is a byte-identical no-op for all 45 non-N.D.L.
     * projectile rows; N.D.L. arms athok two chart cells after its own
     * atix, so max() picks up the later anchor only for it (S 12/12/13 ->
     * 15/15/16, arcade-exact). Lever J at 0 restores the legacy
     * spawn-only anchor. */
    const int fd_proj_hitcheck_split = 1; /* lever J */
    s32 proj_s = g_cur.proj_spawn_slot;
    if (fd_proj_hitcheck_split && g_cur.proj_athok_slot >= 0
        && g_cur.proj_athok_slot > proj_s) {
        proj_s = g_cur.proj_athok_slot;
    }
    const s32 startup = use_proj_split ? proj_s : startup_pf;
    const s32 active  = use_proj_split
        ? ((g_cur.atk_idx >= 0) ? (s32)fd_engine_proj_active_count[g_cur.atk_idx] : 0)
        : displayed_a;  /* lever O substitutes effective_a on the whiff gate, §13.17 */

    /* §13.6.1 R re-attribution (Phase 4 item 4 — CnDB "R+1"). On the
     * partner-release path the move's end is clocked by an *external*
     * event (defender exits the grapple, dp->r1==3 && dn->r1==1, tick side
     * above), so the window is wall-clock-fixed and raw_len == arcade
     * S+A+R exactly (verified 56/59/62 for LK/MK/HK, HIT and WHIFF both).
     * The catch cell (declared cg_ctr=2) collapses into the throw segment
     * after only 1 real tick; the lost declared tick's duration flows into
     * the recovery tail while displayed A (effective_a) already credits it
     * as active — double-attributed across the A/R boundary. Re-derive R
     * from the engine credit on this path only, so the boundary matches
     * what A already claimed. Gated on the explicit ended_by_partner_release
     * flag set at the tick side — NOT inferred from kd/def_r1 here, since
     * ordinary Throw also has def_r1==3 at attacker_idle and would be
     * ambiguous. This formula is FALSIFIED as a universal form: it
     * regresses ~20+ currently-PASS entries whose engine_a != active_pf
     * (sub-framed contact cells / hitstop-stretched sentinels — see
     * docs/frame-data-synthesis.md §13.6.1) and must stay gated to this
     * branch.
     *
     * Phase 6A "arcade-split" branch (use_proj_split) takes priority over
     * both the generic recovery_pf tally AND the partner-release
     * re-derivation above (mutually exclusive in practice — a projectile
     * move is never ended_by_partner_release — but use_proj_split is
     * checked first for clarity). Per the plan's P-1.1 fix: the generic
     * §13.5.2 override recounts recovery from the last ACTIVE/CONTACT
     * cell, which on this branch is the CONTACT tick — that yields
     * R = meter_len − event_raw − 1, tracking the BLOCK/HIT contact frame
     * (different per entry) instead of the fixed arcade R. Bypass it and
     * compute directly from the spawn boundary: R = meter_len −
     * proj_spawn_slot. Verified against Step 1's probe: spawn slot 10,
     * meter_len 46 → R = 36, exactly arcade Hadouken R — S + R = 46 = T
     * with no remainder, so the contact frame is understood to land
     * *inside* R by arcade convention (a display CONTACT cell, not a slot
     * carved out of the S/R budget).
     *
     * §13.12 (ENGINE-4, lever J): if the tama's chart reached a
     * CHART-NATURAL end (proj_natural_end — atix->0 with the chart never
     * cut to an erase chart, see eff13.c's fd_tama_chart_cut() and
     * charset.c's natend latch) before finalize, R is instead the
     * fixed-arithmetic declared-active-window form: meter_len minus the
     * tama's first-active slot plus its own accumulator total (both
     * freeze-immune, outcome-independent — N.D.L.'s atix sequence is
     * byte-identical WHIFF vs CONNECT). Every fireball ending is a CUT
     * (kotp_00000's four erase transitions), so proj_natural_end is never
     * true for them and this branch is unreachable on those 45 rows —
     * they keep the legacy meter_len-minus-S form byte-identically.
     * N.D.L. (kotp_13000) never cuts on consumption, so its atix->0 edge
     * is always chart-natural: 38-(13+11)=14, 42-(13+13)=16,
     * 47-(14+13)=20 — arcade-exact on all 9 legs. Lever J at 0 restores
     * the legacy fire-and-forget form unconditionally.
     *
     * §13.17 lever N — whiff busy-edge R (`fd_whiff_busy_edge_r`,
     * RE-ANCHOR-1). On the same gate as lever O (fd_whiff_common_gate),
     * displayed R is the frame count strictly between the last raw-box-
     * active frame and the attacker's busy 771->768 edge
     * (busy_edge_frame - box_last - 1) instead of the legacy recovery_pf
     * tally — arcade-exact on every measured whiff row (LAYER-1 §13.16;
     * census + Session-4/5 hardware capture confirm OUTCOME A: the busy
     * edge IS the arcade's own first-actionable frame, including on the
     * two rows whose overlay-natural-end/oracle agreement had masked this
     * within the suite's own PASS population until the census found them —
     * `<sp>/reanchor/leverN-rediagnosis.md` §3/§4). Requires
     * busy_edge_frame to have actually latched AND the finalize-time
     * sanity check busy_edge_frame > box_last (the FINAL box_last,
     * catching a stale latch the tick-side invalidation missed) — either
     * failing falls back to legacy recovery_pf, byte-identical, the same
     * reachable-only-via-fallback outcome as any other gate failure
     * (§2.3). Checked before the plain recovery_pf else-branch; mutually
     * exclusive with use_proj_split/ended_by_partner_release by
     * construction (both require an event or a projectile spawn; lever N
     * requires g_cur.event == FD_OUTCOME_NONE). */
    const bool fd_busy_edge_valid = (g_cur.busy_edge_frame >= 0)
        && (g_cur.busy_edge_frame > g_cur.box_last);
    const bool fd_lever_n_applies = fd_whiff_busy_edge_r
        && fd_whiff_common_gate
        && fd_busy_edge_valid;

    /* §13.19 lever R gate (CONTACT-2 design.md §1.3, Step 2 — wired into
     * `recovery` below): on a HIT/BLOCK window passing this gate, displayed
     * R is the busy-edge arithmetic instead of the legacy recovery_pf
     * tally. G1 (contact-only: HIT/BLOCK, mutually exclusive with lever
     * N's WHIFF-only fd_whiff_common_gate), the shared box/proj/signal
     * conjuncts lever N/O also require, G4 (UOH term — AMENDED by
     * UOH-CLOSURE Step 2, design.md §3.2: was a flat UOH exclusion
     * `!move_is_uoh`; now `!move_is_uoh || (fd_uoh_contact_busy_edge_t &&
     * g_cur.hstop_in_box > 0)` — the non-UOH disjunct is unchanged and
     * still short-circuits true exactly as before on every non-UOH window
     * (`hstop_in_box` is never read there); the new disjunct routes UOH
     * contact windows into this same busy-edge arithmetic when lever T is
     * on and the attacker's freeze overlapped the raw box (engine contact
     * witness, §2.1/§2.2). Lever T at 0 makes the new disjunct
     * unconditionally false, restoring the flat exclusion token-for-token),
     * G5 (freeze-free window, hstop_after_box == 0), G6 (specials-class
     * latch, koc == 5), G7 (AMENDMENT 1 — engine-entered-recovery
     * requirement, recovery_pf > 0: excludes the ENGINE-8/TRACK-E jinchu
     * family, whose legacy recovery tally is zero because the move window
     * ends by Player_normal dispatch handoff while still airborne, never
     * entering an in-engine recovery state at all — census-verified to be
     * exactly oro-jinchu-lk-hit/oro-exjinchu-hit among all gate firers).
     * G7 AMENDED (§13.21 lever U, ENGINE-JINCHU): now
     * `recovery_pf > 0 || (fd_jinchu_bounce_recovery_r && recovery_pf ==
     * 0)` — re-admits exactly those two recovery_pf==0 gate firers when
     * lever U is on; lever U at 0 restores the flat `recovery_pf > 0`
     * exclusion token-for-token. Each conjunct has a named census
     * counterexample in design.md §1.3. */
    const bool fd_lever_r_applies = fd_contact_busy_edge_r
        && (g_cur.event == FD_OUTCOME_HIT || g_cur.event == FD_OUTCOME_BLOCK)
        && !g_cur.is_throw
        && !g_cur.ended_by_partner_release
        && !use_proj_split && !no_active_signal
        && g_cur.box_count > 0 && g_cur.box_runs == 1
        && fd_busy_edge_valid
        && g_cur.koc == 5
        && (!g_cur.move_is_uoh
            || (fd_uoh_contact_busy_edge_t                /* lever T */
                && g_cur.hstop_in_box > 0))                /* freeze-overlap:
                                                               engine contact
                                                               witness */
        && g_cur.hstop_after_box == 0
        && (recovery_pf > 0
            || (fd_jinchu_bounce_recovery_r          /* lever U (ENGINE-JINCHU) */
                && recovery_pf == 0));               /* admit the airborne-handoff
                                                        jinchu-class contact legs */
    const s32 lever_r_pred = fd_lever_r_applies
        ? (g_cur.busy_edge_frame - g_cur.box_last - 1)
        : -1;

    /* §13.20 lever T gate (UOH-CLOSURE Step 2, design.md §3/§8.2 — WIRED
     * IN: this is the same predicate the amended G4 disjunct above now
     * folds into `fd_lever_r_applies`, kept here as an independent
     * cross-check/diagnostic (Gate U1/U2's tripwire, §8.1) rather than
     * removed — the two must always agree by construction. Predicts what
     * the amended G4 term (§3.2) computes on the UOH-contact branch it
     * adds — i.e. `fd_lever_r_applies` above with its G4 conjunct
     * evaluated purely on the new disjunct (`move_is_uoh && lever T
     * enabled && hstop_in_box > 0`), all of lever R's OTHER conjuncts (G1
     * contact-only, G5/G6/G7, box_runs==1, valid busy edge, !is_throw,
     * !endrel, !use_proj_split, !no_active_signal) unchanged. Every
     * non-UOH window has move_is_uoh == false and so this is -1 there
     * unconditionally (byte-identical to today, regardless of lever_r's
     * own firing — Gate U1's "leverT_pred == -1 on every non-UOH window"
     * tripwire). Every UOH WHIFF window has event == NONE (fails G1) and
     * so is also -1 (16 windows). Only the 33 UOH HIT/BLOCK contact
     * windows can read non -1 here, and on those `leverT_pred ==
     * lever_r_pred` whenever fd_contact_busy_edge_r is also on (both
     * ship at 1). */
    const bool fd_lever_t_applies = g_cur.move_is_uoh
        && fd_uoh_contact_busy_edge_t
        && g_cur.hstop_in_box > 0
        && (g_cur.event == FD_OUTCOME_HIT || g_cur.event == FD_OUTCOME_BLOCK)
        && !g_cur.is_throw
        && !g_cur.ended_by_partner_release
        && !use_proj_split && !no_active_signal
        && g_cur.box_count > 0 && g_cur.box_runs == 1
        && fd_busy_edge_valid
        && g_cur.koc == 5
        && g_cur.hstop_after_box == 0
        && recovery_pf > 0;
    const s32 leverT_pred = fd_lever_t_applies
        ? (g_cur.busy_edge_frame - g_cur.box_last - 1)
        : -1;

    s32 recovery;
    if (use_proj_split) {
        if (fd_proj_hitcheck_split && g_cur.proj_natural_end
            && g_cur.proj_firstact_slot >= 0) {
            recovery = (s32)g_cur.meter_len
                     - (g_cur.proj_firstact_slot
                        + (s32)fd_engine_proj_active_count[g_cur.atk_idx]);
        } else {
            recovery = (s32)g_cur.meter_len - proj_s;
        }
        if (recovery < 0) recovery = 0;
    } else if (g_cur.ended_by_partner_release) {
        recovery = (s32)g_cur.meter_len - startup_pf - effective_a;
        if (recovery < 0) recovery = 0;
    } else if (fd_lever_n_applies) {
        recovery = g_cur.busy_edge_frame - g_cur.box_last - 1;
        if (recovery < 0) recovery = 0;
    } else if (fd_lever_r_applies) {
        /* §13.19 lever R (CONTACT-2 Step 2): same busy-edge arithmetic as
         * lever N, applied on the CONTACT (HIT/BLOCK) side instead of
         * WHIFF. Mutually exclusive with fd_lever_n_applies by construction
         * (g_cur.event == NONE there vs HIT/BLOCK here). Lever at 0
         * (fd_contact_busy_edge_r) restores today's recovery_pf display
         * byte-identically, since fd_lever_r_applies short-circuits false
         * on its first conjunct. */
        recovery = g_cur.busy_edge_frame - g_cur.box_last - 1;
        if (recovery < 0) recovery = 0;
    } else {
        recovery = recovery_pf;
    }

    /* T = measured hitstop-free move duration (meter_len, the trimmed
     * raw_len). Displayed honestly even when S+A+R != T: multi-hit moves
     * leave inter-hit gap frames in neither S, A, nor R (§13.5.2 override
     * counts recovery only after the LAST active cell), and sub-framed or
     * hitstop-stretched contact cells can shift T by a frame or two from
     * the S+A+R sum — that delta is real engine behavior, not a display
     * bug. Display/FINAL-only: the checker's NUMERIC_FIELDS never include
     * T and no corpus entry asserts expect.T. Saturates at FD_METER_LEN
     * for moves whose raw[] capture window is the limiting factor. Throw
     * outcome keeps its own total (early-return block above, unchanged). */
    const s32 total = (s32)g_cur.meter_len;

    s32 advantage = 0;
    FdOutcome outcome = FD_OUTCOME_WHIFF;
    switch (g_cur.event) {
        case FD_OUTCOME_PARRY: {
            const s32 def_idle = g_cur.event_frame + fd_parry_recovery_for(g_cur.parry_r2);
            advantage = def_idle - g_cur.attacker_idle;
            outcome = FD_OUTCOME_PARRY;
            break;
        }
        case FD_OUTCOME_HIT:
        case FD_OUTCOME_BLOCK:
            if (g_cur.defender_idle >= 0) {
                advantage = g_cur.defender_idle - g_cur.attacker_idle;
            }
            outcome = g_cur.event;
            /* §13.6 KD: throw outcome (CnDB and similar) leaves the
             * partner thrown for ~120 frames before they get back up,
             * which makes adv = +121..+125 — technically right but
             * useless to a player. Flag for the draw path to render
             * "KD" instead of the numeric value. Only meaningful on
             * HIT outcome — block can't put a partner into r1=2/3. */
            if (g_cur.event == FD_OUTCOME_HIT && fd_is_knockdown_at_atk_idle()) {
                g_latched.kd = true;
            }
            break;
        default:
            break;
    }

    g_latched.startup   = startup;
    g_latched.active    = active;
    g_latched.recovery  = recovery;
    g_latched.total     = total;
    g_latched.advantage = advantage;
    g_latched.outcome   = outcome;
    g_latched.meter_len = g_cur.meter_len;
    if (g_latched.meter_len > FD_METER_LEN) g_latched.meter_len = FD_METER_LEN;
    memcpy(g_latched.atk_cells, g_cur.atk_cells, (size_t)g_latched.meter_len);
    memcpy(g_latched.def_cells, g_cur.def_cells, (size_t)g_latched.meter_len);

    /* Self-describing trace: emit the finalized numbers exactly as the
     * overlay will display them, plus the indices needed to relate them
     * back to the per-frame trace rows. */
    int first_active_in_raw = -1;
    int event_in_raw        = -1;
    for (int i = 0; i < g_cur.meter_len; i++) {
        if (first_active_in_raw < 0
            && (g_cur.atk_cells[i] == FD_CELL_ACTIVE || g_cur.atk_cells[i] == FD_CELL_CONTACT)) {
            first_active_in_raw = i;
        }
        if (event_in_raw < 0 && g_cur.raw[i].event_this_frame) {
            event_in_raw = i;
        }
    }
    const char* outcome_str =
        (outcome == FD_OUTCOME_HIT)   ? "HIT"   :
        (outcome == FD_OUTCOME_BLOCK) ? "BLOCK" :
        (outcome == FD_OUTCOME_PARRY) ? "PARRY" :
        (outcome == FD_OUTCOME_WHIFF) ? "WHIFF" : "NONE";
    /* Phase 6A "arcade-split" diagnostics (/tmp/phase6-nonq-plan.md §2/
     * Step 2): proj=1 iff this FINAL took the arcade-split branch;
     * proj_a is the projectile's own owner-indexed accumulator (display
     * count, unconditionally reported for diagnostic visibility even on
     * non-split FINALs, where it reads 0 for any move that never spawned
     * a player-owned projectile); proj_spawn_raw is the latched spawn
     * slot, or -1 if no spawn was ever observed this move. §13.6.2b
     * (ENGINE-3, 2026-07-09) adds `kdr2`: the first down-family def_r2
     * value the strike-KD latch observed this move, or -1 if the latch
     * never fired. The checker's KV parser ignores unknown keys
     * (check_frame_data.py's KV_RE scans the whole line), so these are
     * additive and never assessed against any corpus expect.*. */
    const s32 proj_a_diag = (g_cur.atk_idx >= 0)
        ? (s32)fd_engine_proj_active_count[g_cur.atk_idx] : 0;
    /* §13.17 lever N/O diagnostics (RE-ANCHOR-1): box_a is the raw
     * box-frame tally regardless of whether lever O actually applied;
     * busyr is the candidate busy-edge R (-1 if busy_edge_frame never
     * latched this move, whether or not lever N's gate would have used
     * it); box_runs is the contiguous-run count both levers gate on;
     * busyr_fb is lever N's fallback flag (§ above). Additive, KV_RE
     * ignores unknown keys, never assessed against any corpus expect.*. */
    const s32 box_a_diag  = g_cur.box_count;
    const s32 busyr_diag  = (g_cur.busy_edge_frame >= 0)
        ? (g_cur.busy_edge_frame - g_cur.box_last - 1) : -1;
    frame_trace_annotate(
        "FINAL atk=%d outcome=%s S=%d A=%d R=%d T=%d adv=%+d kd=%d "
        "engine_a=%d use_hatt=%d raw_len=%d first_active_raw=%d event_raw=%d "
        "move_start_F=%d atk_idle_F=%d def_idle_F=%d event_F=%d parry_r2=%d "
        "active_pf=%d cut=%d dwellbrk=%d anchor_a=%d endrel=%d "
        "proj=%d proj_a=%d proj_spawn_raw=%d kdr2=%d "
        "athok=%d firstact=%d natend=%d "
        "box_a=%d busyr=%d box_runs=%d busyr_fb=%d raw_sat=%d "
        "hsab=%d uoh=%d koc=%d leverR_pred=%d "
        "hsib=%d leverT_pred=%d",
        g_cur.atk_idx, outcome_str,
        (int)startup, (int)active, (int)recovery, (int)total, (int)advantage,
        (int)g_latched.kd,
        (int)engine_a, (int)use_hatt, g_cur.raw_len,
        first_active_in_raw, event_in_raw,
        (int)g_cur.move_start, (int)g_cur.attacker_idle, (int)g_cur.defender_idle,
        (int)g_cur.event_frame, (int)g_cur.parry_r2,
        (int)active_pf, (int)cut_committed, (int)g_cur.cghi1_dwell_broken,
        (int)g_cur.engine_a_at_cut_anchor, (int)g_cur.ended_by_partner_release,
        (int)use_proj_split, (int)proj_a_diag, (int)g_cur.proj_spawn_slot,
        (int)g_cur.strike_kd_r2,
        (int)g_cur.proj_athok_slot, (int)g_cur.proj_firstact_slot, (int)g_cur.proj_natural_end,
        (int)box_a_diag, (int)busyr_diag, (int)g_cur.box_runs, (int)g_cur.busyr_fallback,
        (int)(g_cur.raw_len == FD_CAPTURE_LEN),
        (int)g_cur.hstop_after_box, (int)g_cur.move_is_uoh, (int)g_cur.koc, (int)lever_r_pred,
        (int)g_cur.hstop_in_box, (int)leverT_pred);
}

void frame_data_overlay_tick(void) {
    if (!Is_Training_Mode(Mode_Type)) {
        g_have_prev = false;
        fd_reset_move();
        g_latched.valid = false;
        return;
    }
    /* Phase 5 hygiene item 3 (docs/plan-frame-data-harness.md): the draw
     * side (sc_sub.c) already checks Disp_Frame_Data before rendering the
     * meter; add the matching early-out here so the tick's per-frame
     * bookkeeping (snapshot diffing, MOVE_START/FINAL trace_annotate
     * calls) doesn't run either when the overlay is off. Reset state the
     * same way the Is_Training_Mode branch above does, so re-enabling the
     * overlay mid-session starts clean instead of replaying stale prev-
     * frame state. The char_move()/check_cm_extended_code() engine
     * accumulator hooks in charset.c (fd_engine_hitbox_active,
     * fd_engine_active_count, fd_prev_active_cgix*) are NOT gated here —
     * they already run unconditionally in every mode (arcade/vs/netplay),
     * not just training, and are a couple of branch checks + u8/u16/s16
     * array writes bounded to at most the 2 player WORKs per tick; no
     * loops, no I/O, no measurable cost added to the hot path whether or
     * not the overlay is in use. */
    if (!Disp_Frame_Data) {
        g_have_prev = false;
        fd_reset_move();
        g_latched.valid = false;
        return;
    }
    if (sa_stop_check() != 0) {
        return;  /* super-freeze: don't advance accounting */
    }

    FdSnap now[2];
    fd_snap_player(0, &now[0]);
    fd_snap_player(1, &now[1]);

    if (!g_have_prev) {
        g_prev[0] = now[0];
        g_prev[1] = now[1];
        g_have_prev = true;
        g_local_frame = 0;
        fd_reset_move();
        return;
    }
    g_local_frame++;

    /* If no move is active, look for a starting edge on either player.
     * Filter to attacker-initiated transitions (r1 ∈ {2,3,4}): r1=4 is
     * attacking, r1=2/3 are throw motions. r1=1 is exclusively defender
     * reaction (blockstun/hitstun) and would otherwise spam spurious
     * MOVE_STARTs every time the dummy gets hit. */
    if (!g_cur.active) {
        for (int i = 0; i < 2; i++) {
            if (g_prev[i].r1 == 0 &&
                (now[i].r1 == 4 || now[i].r1 == 2 || now[i].r1 == 3)) {
                fd_reset_move();
                g_cur.active       = true;
                g_cur.atk_idx      = i;
                g_cur.is_throw     = (now[i].r1 == 2 || now[i].r1 == 3);
                g_cur.move_start   = g_local_frame;
                /* CONTACT-2 Step 1 (design.md §1.3 G4/G6, diagnostics only):
                 * latch this move's specials-class value and UOH dispatch
                 * tag right at move-init, same timing as the engine_a/proj
                 * captures below (the engine tick that set cmoa.koc / set
                 * fd_engine_move_is_uoh already ran this same real frame,
                 * main.c:604 vs :616 — see lever S comment above). */
                g_cur.koc          = (s32)plw[i].wu.cmoa.koc;
                g_cur.move_is_uoh  = (fd_engine_move_is_uoh[i] != 0);
                /* Clear the engine's active-count capture slot so char_move
                 * can accumulate this move's active cells' cgctrs fresh. */
                /* lever S (ENGINE-D2): the engine tick of THIS same real frame runs before
                 * this overlay tick (main.c:604 vs :616). If char_move() already entered the
                 * new move's first active/catch cell this tick (freeze-deferred MOVE_START:
                 * sa_stop parks this scan while the engine runs, so on charts whose first
                 * post-flash cell is active, the add lands before this reset), preserve
                 * exactly that cell's contribution and its prev-cgix bookkeeping instead of
                 * zeroing it — otherwise the first cell is silently re-entered next tick at
                 * ctr-1 (or dropped), reading A one short of declared credit (SA-WHIFF-A/D2,
                 * docs/frame-data-synthesis.md §12.2.4 :978/:980/:982).
                 *
                 * Known caveat (documented, not gated): Game_timer is a u16
                 * (workuser.c:395, workuser.h:562) that wraps at 65536 and is reset to 0 at
                 * six sites (game.c:506,607,1332; sys_sub.c:1440; netplay.c:419,486). A stale
                 * fd_prev_active_cgix_tick[i] stamp surviving across one of those
                 * resets/wraps can numerically collide with a later, unrelated MOVE_START's
                 * Game_timer and wrongly take the preserve branch below. Suite-invisible
                 * today (every corpus run is short and monotonic — no reset/wrap falls
                 * inside a tracked [MOVE_START, FINAL] window); would only manifest as a
                 * display-only artifact during long free-play/training sessions, never
                 * inside the harness. The cheap tightening (bound the preserve to
                 * Game_timer - fd_prev_active_cgix_tick[i] < N, or clear the stamp at each
                 * reset site) is deferred, not part of this fix's shape. */
                if (fd_movestart_same_tick_credit_hold
                    && fd_prev_active_cgix_tick[i] == Game_timer) {
                    fd_engine_active_count[i] = fd_prev_active_cgix_add[i];
                    /* keep fd_prev_active_cgix / _tick / _add / _jatix intact so the same
                     * cell is NOT re-credited on the next engine tick */
                } else {
                    fd_engine_active_count[i] = 0;
                    fd_prev_active_cgix[i] = -1;
                    fd_prev_active_cgix_tick[i] = 0;
                    fd_prev_active_cgix_add[i] = 0;
                    fd_prev_active_cgix_jatix[i] = 0;
                }
                /* Phase 6A: same reset for the projectile accumulator/spawn
                 * flag (workuser.c/.h), so a fireball from a PRIOR move
                 * (or one already in flight when this move starts) can
                 * never be misattributed to this fresh move.
                 *
                 * BUT: effect_13_init() (which sets fd_engine_proj_spawned)
                 * runs inside njUserMain(), strictly before this overlay
                 * tick (main.c:603 vs 615) — so a projectile that spawns on
                 * THIS move's very first engine tick already has the flag
                 * set by the time we get here, on the same tick we're about
                 * to zero it for the fresh move. Latch it as a legitimate
                 * slot-0 spawn BEFORE zeroing, or it's lost: the per-tick
                 * consume site below (same tick, later in this function)
                 * would otherwise read an already-cleared flag and silently
                 * miss the spawn. is_throw/atk_idx are already set above,
                 * so the guard mirrors the consume site's. */
                if (!g_cur.is_throw && fd_engine_proj_spawned[i]) {
                    g_cur.proj_spawn_slot = 0;
                    g_cur.proj_seen       = true;
                }
                fd_engine_proj_spawned[i] = 0;
                fd_engine_proj_active_count[i] = 0;

                /* §13.12 (ENGINE-4): same slot-0 re-latch caveat applies
                 * to fd_engine_proj_hitok as to fd_engine_proj_spawned
                 * above — a fireball arms athok the same engine tick it
                 * spawns, which is this move's very first tick, before
                 * this overlay tick runs. Latch that as proj_athok_slot=0
                 * BEFORE zeroing the flag, or it's lost the same way an
                 * un-latched slot-0 spawn would be. Safe to treat as a
                 * genuine same-tick signal (not a leftover from a PRIOR
                 * move's still-flying tama) because fd_engine_proj_hitok
                 * is edge-triggered (charset.c's fd_engine_proj_hitok_armed
                 * gate) — it cannot still read 1 here from an EARLIER
                 * tick of a different tama; the only way it's 1 at this
                 * exact point is a fresh rising edge on THIS tick. Caveat:
                 * a re-arming multi-hit projectile (set_new_attnum()
                 * re-setting att_hit_ok=1, charset.c:2928) could in
                 * principle leave a SECOND, not-yet-consumed rising edge
                 * straddling this same move/tick boundary; measured
                 * corpus-clean (zero athok=0 FINALs across the proj=1
                 * population) — same family as the documented
                 * multi-projectile residual (§13.12, out of scope).
                 * fd_engine_proj_cut/_natend need no such caveat (per
                 * §13.12/B.3): a cut or a chart-natural end cannot occur
                 * on a tama's own spawn tick, so they are simply zeroed
                 * here — this only matters for preventing a PRIOR move's
                 * still-flying tama from leaking its cut/natend state
                 * into this fresh move. fd_engine_proj_hitok_armed is
                 * zeroed defensively too (charset.c already resets it on
                 * every atix==0 tick, but a fresh move should never
                 * inherit any stale armed-state either way). */
                if (!g_cur.is_throw && fd_engine_proj_hitok[i]) {
                    g_cur.proj_athok_slot = 0;
                }
                fd_engine_proj_hitok[i]       = 0;
                fd_engine_proj_hitok_armed[i] = 0;
                fd_engine_proj_cut[i]         = 0;
                fd_engine_proj_natend[i]      = 0;

                /* Self-describing trace: capture the inputs/state that
                 * pin down which move this is, so post-hoc analysis
                 * doesn't depend on a separate "what did I press" log. */
                const int def = 1 - i;
                const u16 sw_new = (plw[i].cp != NULL) ? plw[i].cp->sw_new : (u16)0;
                const s16 atk_x  = plw[i].wu.xyz[0].disp.pos;
                const s16 def_x  = plw[def].wu.xyz[0].disp.pos;
                const int dist   = (atk_x > def_x) ? (atk_x - def_x) : (def_x - atk_x);
                /* CONTACT-2 Step 1 risk R1 (design.md §1.3 G4 / §7): the
                 * fallback (assumption-free-alternative) UOH test, computed
                 * and printed alongside the preferred dispatch-tag latch
                 * (g_cur.move_is_uoh) purely for cross-check — never read by
                 * any gate. Same cmoa/waza_r snapshot the preferred form's
                 * design doc cites as the fallback definition. */
                const bool uoh_cmoa_fallback = (plw[i].wu.cmoa.koc == 5)
                    && (plw[i].cp != NULL)
                    && (plw[i].wu.cmoa.ix == (s16)plw[i].cp->waza_r[14][0]);
                frame_trace_annotate(
                    "MOVE_START GT=%u atk=%d char=%u r1=%d r2=%d "
                    "cgix=%d cgctr=%u cghi=%u pat=%u kow=%u sw_new=0x%04x "
                    "atk_x=%d def_x=%d dist=%d "
                    "koc=%d uoh_pref=%d uoh_fb=%d",
                    (unsigned)Game_timer, i, (unsigned)My_char[i],
                    (int)now[i].r1, (int)now[i].r2,
                    (int)plw[i].wu.cg_ix, (unsigned)plw[i].wu.cg_ctr,
                    (unsigned)plw[i].wu.cg_hit_ix, (unsigned)plw[i].wu.pat_status,
                    (unsigned)plw[i].wu.kow, (unsigned)sw_new,
                    (int)atk_x, (int)def_x, dist,
                    (int)g_cur.koc, (int)g_cur.move_is_uoh, (int)uoh_cmoa_fallback);
                break;
            }
        }
    }

    if (g_cur.active) {
        const int atk = g_cur.atk_idx;
        const int def = 1 - atk;
        const FdSnap* an = &now[atk];
        const FdSnap* dn = &now[def];
        const FdSnap* dp = &g_prev[def];
        const FdSnap* ap = &g_prev[atk];  /* §13.17 lever N/O: attacker's own previous-tick snap */

        bool event_this_frame = false;

        if (g_cur.is_throw) {
            if (dn->r1 == 2 || dn->r1 == 3) {
                g_cur.partner_in_throw = true;
            }
        } else {
            /* Active-frame counting (cg_att_ix > 0 while attacker in r1=4). */
            if (an->r1 == 4 && an->cg_att_ix > 0) {
                if (g_cur.first_active < 0) g_cur.first_active = g_local_frame;
                g_cur.last_active = g_local_frame;
            }

            /* Throw-grapple event edge (§13.6): defender enters r1=2 or r1=3
             * (thrown / grappled) from r1=0. Throws bypass dm_stop because
             * set_caught_status writes routine_no[1] = 3 directly without
             * touching dm_stop. Detect the partner-state transition instead.
             * Treat as HIT outcome so advantage calc / event_this_frame
             * coloring flow through the existing path. */
            if (g_cur.event == FD_OUTCOME_NONE
                && dp->r1 == 0
                && (dn->r1 == 2 || dn->r1 == 3)) {
                g_cur.event_frame = g_local_frame;
                g_cur.event       = FD_OUTCOME_HIT;
                event_this_frame  = true;
                if (g_cur.first_active < 0) {
                    g_cur.first_active = g_local_frame;
                    g_cur.last_active  = g_local_frame;
                }
            }

            /* Defender event edge: dm_stop went 0 → nonzero. The engine writes
             * dm_stop = att.hs_you at contact (hitcheck.c dm_status_copy, hit
             * and guard paths alike); hs_you is a SIGNED per-move data byte
             * whose sign selects the hitstop style (positive = frozen,
             * negative = animation advances during the stop — plmain.c
             * check_hit_stop), NOT the event kind. Q/Ryu cr.LK carry
             * hs_you=+7, so the old `< 0` test missed their contact entirely
             * (issue #14, synthesis §13.2 + item 14). No non-contact engine
             * code writes dm_stop (writer inventory in synthesis item 14), so
             * a 0 → nonzero edge is contact-only in both signs. Parries write
             * -15 (set_paring_status) and are unaffected.
             *   r1==0 → PARRY, r1==1 + dcnt unchanged → BLOCK, r1==1 + dcnt+1 → HIT */
            if (g_cur.event == FD_OUTCOME_NONE && dp->dm_stop == 0 && dn->dm_stop != 0) {
                g_cur.event_frame = g_local_frame;
                if (dn->r1 == 0) {
                    g_cur.event    = FD_OUTCOME_PARRY;
                    g_cur.parry_r2 = dn->r2;
                } else if (dn->r1 == 1) {
                    g_cur.event = (dn->dm_count_up != dp->dm_count_up)
                                ? FD_OUTCOME_HIT : FD_OUTCOME_BLOCK;
                }
                event_this_frame = (g_cur.event != FD_OUTCOME_NONE);

                /* Some attacks (Q's command normals, certain specials) connect
                 * without ever setting cg_att_ix > 0 — the engine bypasses the
                 * cell's attack-data flag. Use the contact frame as the active-
                 * window proxy if we never saw a real active frame. */
                if (g_cur.first_active < 0 && event_this_frame) {
                    g_cur.first_active = g_local_frame;
                    g_cur.last_active  = g_local_frame;
                }
            }

            /* §13.14 (ENGINE-9, lever M): multi-contact adv anchor. A SUBSEQUENT
             * contact of this same move (event already latched HIT/BLOCK, fresh
             * dm_stop 0->nonzero contact edge on the defender, defender back in
             * stun r1==1) re-opens defender-idle accounting so the FINAL stun exit
             * anchors adv, not the first. Guarded on the attacker's chart never
             * having reset (§13.5.1's own cgix_reset_frame signal): a contact
             * arriving after a cgix reset under r1=4 belongs to a return-to-neutral
             * / retriggered chart (q-uoh-chain-retrigger), not a later cell of the
             * original attack chart (remy cr.RH advances 12->16->...->48 with no
             * reset). No-ops by construction on: single-contact moves (no second
             * edge), continuous-stun multi-hits (defender_idle still -1 — HSB),
             * parried later contacts (r1==0), throws (set_caught_status clears
             * dm_stop to 0, hitcheck.c:335 — never sets it nonzero; reached via
             * set_judge_result -> set_caught_status, hitcheck.c:86/:162), WHIFF (no event). */
            const int fd_adv_last_stun_exit = 1; /* lever M */
            if (fd_adv_last_stun_exit
                && (g_cur.event == FD_OUTCOME_HIT || g_cur.event == FD_OUTCOME_BLOCK)
                && g_cur.defender_idle >= 0
                && g_cur.cgix_reset_frame < 0          /* same-chart gate (§13.14 conjunct 3) */
                && dp->dm_stop == 0 && dn->dm_stop != 0
                && dn->r1 == 1) {
                g_cur.defender_idle = -1;              /* re-arm: final exit wins */
            }

            /* Defender idle return (hit/block; parry stays r1=0). */
            if ((g_cur.event == FD_OUTCOME_HIT || g_cur.event == FD_OUTCOME_BLOCK)
                && g_cur.defender_idle < 0
                && dp->r1 != 0 && dn->r1 == 0) {
                g_cur.defender_idle = g_local_frame;
            }

            /* §13.6.2b strike-KD latch (lever I): two-stage damage charts
             * (plpdm.c:1033, :1061) can enter the down family after
             * attacker_idle, or leave it before — sample every tick of
             * this block (which already runs through the finalize-
             * deferred window, not just up to attacker_idle) instead of
             * only the last raw[] cell. Trace-proven member:
             * urien-vkd-lk-hit (last cell def_r2=1, down value 14
             * traversed mid-window). */
            const int fd_strike_kd_latch = 1; /* lever I */
            if (fd_strike_kd_latch
                && g_cur.event == FD_OUTCOME_HIT
                && dn->r1 == 1 && fd_r2_is_down_family(dn->r2)) {
                if (!g_cur.strike_kd_seen) g_cur.strike_kd_r2 = dn->r2;
                g_cur.strike_kd_seen = true;
            }

            /* §13.12 (ENGINE-4): hit-checkable projectile split anchors
             * (lever J). Consumed PRE-raw[]-append (unlike the existing
             * proj_spawn_slot consume site below, which is deliberately
             * post-append) so g_cur.raw_len here is this tick's own slot
             * index — a post-append read would shift both anchors +1 and
             * break the §13.12/B.2 arithmetic.
             *
             * proj_athok_slot: latch once per move, the first tick the
             * engine's fd_engine_proj_hitok flag is observed set (the
             * tama's own hit-check-gated arming tick — the same atix!=0
             * && att_hit_ok!=0 pair hitcheck.c's attack_hit_check() gates
             * on). A fireball's slot-0 case is pre-latched at MOVE_START
             * above (mirrors proj_spawn_slot's own slot-0 handling); this
             * site owns every later tick.
             *
             * proj_firstact_slot: latch once per move, the first tick
             * fd_engine_proj_active_count (this move's owner-attributed
             * tama accumulator, charset.c) goes nonzero — the tama's
             * first atix!=0 tick.
             *
             * proj_natural_end: consume fd_engine_proj_natend once it
             * fires. Requires proj_firstact_slot to already be latched —
             * a natend arriving before this move ever saw an active tama
             * tick belongs to a PRIOR move's still-flying tama and must
             * not be attributed here (the engine flag is still cleared
             * either way, so it can't leak into a later move either).
             * ALSO re-checks fd_engine_proj_cut here, independent of
             * charset.c's own write-time gate on the same flag: on a
             * kind-0 (fireball) case-0-internal cut (chart-end/ground/
             * timeout), fd_tama_chart_cut() is called strictly AFTER the
             * char_move() call that first shows atix==0, within the SAME
             * kotp_00000 invocation — charset.c's write-time read can
             * catch this cut still at 0, but njUserMain() (and thus this
             * SAME engine tick's cut call) has always finished by the
             * time this overlay tick runs, so re-checking here sees the
             * fully-settled value. */
            if (g_cur.proj_athok_slot < 0 && fd_engine_proj_hitok[atk]) {
                fd_engine_proj_hitok[atk] = 0;
                g_cur.proj_athok_slot = g_cur.raw_len;
            }
            if (g_cur.proj_firstact_slot < 0 && fd_engine_proj_active_count[atk] != 0) {
                g_cur.proj_firstact_slot = g_cur.raw_len;
            }
            if (fd_engine_proj_natend[atk]) {
                fd_engine_proj_natend[atk] = 0;
                if (g_cur.proj_firstact_slot >= 0 && fd_engine_proj_cut[atk] == 0) {
                    g_cur.proj_natural_end = true;
                }
            }
        }

        /* Attacker idle return: r1: 4→0 (animation fully ends). This is
         * the engine's authoritative move-end signal. We previously also
         * tried `cg_cancel` returning to 0 as a "non-cancellable tail
         * begins" proxy, but that fires several frames before the arcade
         * move-end for Q's far LP and gives short recovery counts; r1
         * alone is closer to arcade across the move set. */
        if (g_cur.attacker_idle < 0) {
            const bool r1_ended = (g_prev[atk].r1 != 0 && an->r1 == 0);
            if (r1_ended) {
                g_cur.attacker_idle = g_local_frame;
            }
        }

        /* §13.5.1 multi-segment recovery cut (see frame-data-synthesis.md).
         * Fires while move is in attacker-active state (r1=4) OR command-
         * throw-attacker state (r1=2). r1=2 covers Q's Capture and Deadly
         * Blow — when the catch lands, attacker.r1 transitions 4→2 and
         * stays at 2 through the long throw animation. After reset, count
         * cghi=1 frames; once duration ≥ 3 we commit attacker_idle to the
         * FIRST cghi=1 frame and trim raw_len so finalize ignores the
         * post-cut cleanup-anim cells. */
        if ((an->r1 == 4 || an->r1 == 2) && g_cur.attacker_idle < 0 && !g_cur.is_throw) {
            const s16 now_cgix = plw[atk].wu.cg_ix;
            const u16 now_cghi = an->cg_hit_ix;
            if (g_cur.prev_cgix >= 0
                && now_cgix < g_cur.prev_cgix
                && g_cur.first_active >= 0
                && g_cur.cgix_reset_frame < 0) {
                g_cur.cgix_reset_frame = g_local_frame;
            }
            /* §13.6.1 partner-release predicate: command grabs (CnDB) end
             * the throw-cleanup tail when the defender exits the grappled
             * state (r1: 3 → 1). Across all three CnDB strengths this edge
             * fires exactly at arcade-actionable + 1 game-tick. Cuts ~5
             * frames earlier than the cghi=1 fallback. Gated on
             * cgix_reset_frame so it only applies to multi-segment moves;
             * never fires on UOH (no r1=3 partner) or HSB (partner stays
             * in r1=1 blockstun, not r1=3). */
            /* §13.5.1a cut-gate (2026-07-08, lever G): the dwell may only
             * ANCHOR on a grounded fresh edge into cghi==1 — a transition
             * whose previous sampled tick (a) had cghi != 1, (b) is itself
             * at-or-after the cgix reset, and (c) was grounded, with the
             * current tick grounded too. Kills the two trace-proven
             * §13.5.1 false-positive shapes (chart already at cghi=1 when
             * a mid-anim cgix dip fires — Oro Niou Riki; landing-tick
             * label flip on airborne moves — Oro Jinchu / Ken SRK / Yun
             * Nishou) while keeping every protected cut: UOH R=5 (fresh
             * grounded edge 234->1), Throw HIT R=34 (0->1), CnDB
             * (partner-release branch, untouched). Once anchored,
             * everything downstream is unchanged. */
            const int fd_cut_requires_grounded_fresh_edge = 1; /* lever G */
            const s16 now_y = plw[atk].wu.xyz[1].disp.pos;
            const bool cghi1_fresh_edge =
                (now_cghi == 1)
                && g_cur.cgix_reset_frame >= 0
                && (!fd_cut_requires_grounded_fresh_edge
                    || (g_cur.prev_cghi >= 0
                        && g_cur.prev_cghi != 1
                        && g_cur.prev_cghi_frame >= g_cur.cgix_reset_frame
                        && g_cur.prev_y == 0
                        && now_y == 0));
            /* §13.5.1b guard-rearm commit gate (lever H): a committed cut is
             * only trusted if the engine's own guard-type data re-armed
             * guarding during the dwell — jumping_guard_type_check()
             * (pls00.c:1160) clears guard_flag to 0 iff the current chart
             * cell's cg_type is guard-capable ({0xFF,64,2,3,7});
             * Player_attack() (plpat.c:57,89) re-asserts 3 every r1=4 tick
             * otherwise. True cleanup tails (UOH family, Oniyama,
             * Senkyuutai) re-arm at anchor+0/+1; ENGINE-2/18(b) false cuts
             * keep guard_flag=3 until the natural r1 edge. r1==2 grapple
             * tails are exempt (Player_catch never re-arms guarding;
             * q-throw R=34 wall). */
            const int fd_cut_requires_guard_rearm = 1; /* lever H */
            const bool guard_rearmed =
                (an->r1 != 4) || (plw[atk].guard_flag == 0);
            if (g_cur.cgix_reset_frame >= 0 && dp->r1 == 3 && dn->r1 == 1) {
                g_cur.attacker_idle = g_local_frame - 1;
                g_cur.ended_by_partner_release = true;
                if (g_cur.raw_len > 0) {
                    g_cur.raw_len--;
                    g_cur.meter_len = g_cur.raw_len;
                }
            } else if (g_cur.cgix_reset_frame >= 0 && now_cghi == 1
                       && (g_cur.cghi1_first_frame >= 0 || cghi1_fresh_edge)) {
                if (g_cur.cghi1_first_frame < 0) {
                    g_cur.cghi1_first_frame    = g_local_frame;
                    g_cur.cghi1_first_raw_slot = g_cur.raw_len;
                    /* §13.9.4 anchor-time snapshot. Valid because
                     * frame_data_overlay_tick() (main.c:615) runs after
                     * the engine tick / njUserMain() (main.c:603), so
                     * every char_move() add for this frame (charset.c's
                     * fd_engine_active_count accumulator) has already
                     * landed by the time we read it here — the snapshot
                     * is exact through end-of-anchor-frame. Trace-verified
                     * (Step 1 v2, run1+run2/cm.log) that no add fires
                     * between tap-1's last active cell and a retrigger's
                     * first active cell, so on a same-r1 retrigger this
                     * holds exactly tap-1's total. A future main-loop
                     * reorder that puts the overlay tick before the
                     * engine tick would silently break this invariant —
                     * mutation-B-style regression would show up as the
                     * retrigger corpus entry going red. */
                    g_cur.engine_a_at_cut_anchor = (s32)fd_engine_active_count[g_cur.atk_idx];
                }
                g_cur.cghi1_count++;
                if (g_cur.cghi1_count >= 3
                    && (!fd_cut_requires_guard_rearm || guard_rearmed)) {
                    g_cur.cut_committed = true;          /* new explicit flag */
                    g_cur.attacker_idle = g_cur.cghi1_first_frame;
                    if (g_cur.raw_len > g_cur.cghi1_first_raw_slot) {
                        g_cur.raw_len   = g_cur.cghi1_first_raw_slot;
                        g_cur.meter_len = g_cur.raw_len;
                    }
                }
            } else if (g_cur.cghi1_first_frame >= 0) {
                /* Dwell interrupted: a cghi=1 anchor was set (a cut is in
                 * progress) but cghi left 1 again — while r1 stays 4/2 and
                 * attacker_idle is still unset, i.e. before g_cur.cut_committed
                 * is set (§13.5.1b: cghi1_count reaching 3 AND the
                 * guard-rearm gate passing, not cghi1_count alone) — the
                 * signature of a same-r1 retrigger landing mid-dwell. Pure
                 * observation: read only at finalize, never mutates live
                 * accounting. */
                g_cur.cghi1_dwell_broken = true;
            }
            g_cur.prev_cgix = now_cgix;
            g_cur.prev_cghi = (s32)now_cghi;
            g_cur.prev_cghi_frame = g_local_frame;
            g_cur.prev_y = now_y;
        }

        /* Skip frames while the ATTACKER is in hitstop (SF6-style: meter
         * freezes for the side that's frozen). Defender's hit_stop is
         * separate — for some moves (e.g. Q's MP) the defender is frozen
         * longer than the attacker, and we want those post-attacker-freeze
         * frames to count toward the attacker's active phase. */
        const bool in_hitstop = (an->hit_stop != 0) && !event_this_frame;

        /* §13.17 lever N/O tick-side tracking (RE-ANCHOR-1 overlay
         * re-anchor): passive latch of the raw box-active predicate
         * (`an->box_active`, §13.17, `fd_snap_player`) and the attacker's
         * own busy-flag 771->768 edge (`guard_flag` 3->0 with
         * `old_gdflag` still 3). Written every tick the move is active
         * regardless of outcome (cheap, passive — steers nothing; only
         * fd_finalize()'s WHIFF-gated lever N/O branches read these
         * fields), hitstop ticks excluded to match `twin_derive.py:116`
         * (inert on whiff, where hstop==0 throughout every measured
         * window, §13.16). Runs past attacker_idle too (unlike raw[]
         * append below) — the 18(b)-family busy edge lands AFTER the cut
         * anchor, so this must keep observing during lever N's finalize
         * deferral (§ below). Does not run for throws (att_box/guard_flag
         * semantics untested there; lever N/O never apply to throws). */
        if (!g_cur.is_throw && !in_hitstop) {
            if (an->box_active) {
                if (g_cur.box_first < 0) g_cur.box_first = g_local_frame;
                g_cur.box_last = g_local_frame;
                /* CONTACT-2 Step 1 (design.md §1.3 G5, diagnostics only):
                 * a fresh box_last observation means we're no longer in a
                 * freeze gap between the last box frame and the (not yet
                 * latched) busy edge — reset the counter below. */
                g_cur.hstop_after_box = 0;
                g_cur.box_count++;
                if (!ap->box_active) {
                    g_cur.box_runs++;
                    /* §2.3 busy-edge latch invalidation: a fresh run
                     * starting after an edge was already latched means
                     * that edge described the gap before THIS run, not
                     * the move's true recovery boundary. The finalize-
                     * time assertion (fd_busy_edge_valid, using the FINAL
                     * box_last) is the second, independent check for the
                     * same failure mode. */
                    if (g_cur.busy_edge_frame >= 0) {
                        g_cur.busy_edge_frame = -1;
                    }
                }
            }
            if (g_cur.busy_edge_frame < 0 && g_cur.box_last >= 0
                && ap->busy == 771 && an->busy == 768) {
                g_cur.busy_edge_frame = g_local_frame;
            }
        } else if (!g_cur.is_throw && in_hitstop) {
            /* CONTACT-2 Step 1 (design.md §1.3 G5, diagnostics only):
             * this tick is freeze-excluded (attacker hit_stop != 0, not the
             * event tick). If we've already seen a box-active frame and the
             * busy edge hasn't latched yet, this freeze tick sits strictly
             * between box_last and the eventual busy edge — count it so
             * fd_finalize()'s G5 gate can require zero of these (a
             * freeze-free window) before predicting lever R fires.
             * Counterexamples requiring this: urien-chariot-mk (12
             * in-window freeze ticks), necro-flyingviper (9 ticks). */
            if (g_cur.box_last >= 0 && g_cur.busy_edge_frame < 0) {
                g_cur.hstop_after_box++;
            }
        }

        /* UOH-CLOSURE Step 1 (design.md §3.3, diagnostics only):
         * `hstop_in_box` — raw box-active (`an->box_active`, the same
         * any-of-four att_box signal box_first/box_last/box_count already
         * consume above) AND the attacker's raw hit_stop != 0 (any sign).
         * Deliberately independent of the `in_hitstop`/event_this_frame
         * branching above: NO event-tick exemption, so this reads the
         * true box/freeze overlap (8 on every measured UOH contact
         * window, matching the hardware tapes) rather than the
         * event-tick-adjusted count `hstop_after_box`'s sibling logic
         * would give. Not gated on `in_hitstop` at all — checked once,
         * unconditionally, per tick this move is active. Excluded for
         * throws (box_active/hit_stop semantics untested there, same
         * precedent as the box tracking above). */
        if (!g_cur.is_throw && an->box_active && an->hit_stop != 0) {
            g_cur.hstop_in_box++;
        }

        /* Don't grow the attacker meter past attacker_idle. We continue
         * the loop (defender_idle is still needed for advantage), but
         * raw[] must stop appending. Otherwise short-recovery moves whose
         * defender stays in hitstun past the attacker's animation end
         * (Q's far LP, MP, HP, HK) over-count R by (def_idle - atk_idle).
         * attacker_idle is set earlier in this tick by the r1=4→0 block
         * above, so on the move-end transition frame this gate already
         * excludes the (now-idle) attacker frame from the meter. */
        const bool attacker_already_idle = (g_cur.attacker_idle >= 0);

        if (!g_cur.is_throw && !in_hitstop && !attacker_already_idle
            && g_cur.raw_len < FD_CAPTURE_LEN) {
            const int slot = g_cur.raw_len;
            g_cur.raw[slot].h_att_set        = an->h_att_set;
            g_cur.raw[slot].cg_hit_ix        = an->cg_hit_ix;
            g_cur.raw[slot].def_r1           = dn->r1;
            g_cur.raw[slot].def_r2           = dn->r2;
            g_cur.raw[slot].def_dm_stop      = dn->dm_stop;
            g_cur.raw[slot].def_hit_stop     = dn->hit_stop;
            g_cur.raw[slot].def_dm_count_up  = dn->dm_count_up;
            g_cur.raw[slot].event_this_frame = event_this_frame;
            g_cur.raw_len++;

            /* Live classification for in-progress display. We don't yet know
             * which active signal (h_att vs cg_hit_ix) the move will use, so
             * use a best-guess: CONTACT on event-this-frame, ACTIVE if h_att
             * fired this frame OR cg_att_ix>0, STARTUP until first active is
             * seen, RECOVERY otherwise. fd_finalize() re-classifies properly
             * at move-end with full move history, including the engine
             * active-count override. */
            const bool live_active = (an->h_att_set != 0) || (an->cg_att_ix > 0);
            u8 live_atk;
            if (event_this_frame) {
                live_atk = FD_CELL_CONTACT;
            } else if (live_active) {
                live_atk = FD_CELL_ACTIVE;
            } else if (g_cur.first_active >= 0) {
                live_atk = FD_CELL_RECOVERY;
            } else {
                live_atk = FD_CELL_STARTUP;
            }

            FdSnap dn_snap = {0};
            dn_snap.r1          = dn->r1;
            dn_snap.dm_stop     = dn->dm_stop;
            dn_snap.hit_stop    = dn->hit_stop;
            dn_snap.dm_count_up = dn->dm_count_up;
            const u8 live_def = fd_classify_defender(&dn_snap, &g_cur, event_this_frame);

            g_cur.atk_cells[slot] = live_atk;
            g_cur.def_cells[slot] = live_def;
            g_cur.meter_len = g_cur.raw_len;

            /* Live mirror of the §8.3 finalize engine-active-count override
             * (see §14 visual nit): paint engine_a contiguous cells as
             * ACTIVE / CONTACT starting at the first per-frame ACTIVE
             * cell, so the meter doesn't visibly flip colors at finalize.
             *
             * Skip when ACTIVE cells are scattered (multi-hit moves like
             * HSB): the per-frame painting already shows individual hits
             * with gaps between them, and the contiguous override would
             * squash them into a single band, hiding the later hits. */
            const s32 cur_engine_a = (g_cur.atk_idx >= 0)
                ? (s32)fd_engine_active_count[g_cur.atk_idx] : 0;
            /* BUFFER-1 (2026-07-13): the append gate above now keeps
             * appending/classifying past slot 71 for measurement (raw_len
             * < FD_CAPTURE_LEN), but this mirror-repaint sub-block must not
             * — it only ever needs to paint the first 72 DISPLAY cells, and
             * letting its own scan (which reads g_cur.meter_len, now
             * possibly > FD_METER_LEN) keep widening past slot 71 would
             * repaint cells 0-71 differently every tick as a still-open
             * move's capture window grows. Gate it off entirely once
             * slot >= FD_METER_LEN; appends and first_active tracking above
             * are unaffected past that point (plan §3.1 remedy (i), live
             * half). */
            if (cur_engine_a > 0 && slot < FD_METER_LEN) {
                int first_active_idx_live = -1;
                int last_active_idx_live  = -1;
                int active_count_live     = 0;
                int event_idx_live        = -1;
                for (int j = 0; j < g_cur.meter_len; j++) {
                    if (g_cur.atk_cells[j] == FD_CELL_ACTIVE
                        || g_cur.atk_cells[j] == FD_CELL_CONTACT) {
                        if (first_active_idx_live < 0) first_active_idx_live = j;
                        last_active_idx_live = j;
                        active_count_live++;
                    }
                    if (event_idx_live < 0 && g_cur.raw[j].event_this_frame) {
                        event_idx_live = j;
                    }
                }
                const bool active_contiguous =
                    (first_active_idx_live < 0)
                    || ((last_active_idx_live - first_active_idx_live + 1)
                        == active_count_live);
                if (active_contiguous && first_active_idx_live >= 0) {
                    for (int j = first_active_idx_live; j < g_cur.meter_len; j++) {
                        const int off = j - first_active_idx_live;
                        if (j == event_idx_live) {
                            g_cur.atk_cells[j] = FD_CELL_CONTACT;
                        } else if (off < cur_engine_a) {
                            g_cur.atk_cells[j] = FD_CELL_ACTIVE;
                        } else {
                            g_cur.atk_cells[j] = FD_CELL_RECOVERY;
                        }
                    }
                }
            }
        }

        /* Phase 6A "arcade-split" projectile design
         * (/tmp/phase6-nonq-plan.md §2/Step 2). Consume the engine's
         * fd_engine_proj_spawned flag the same tick it's observed and
         * latch the move's S/R release boundary at whatever raw_len is
         * RIGHT NOW — deliberately placed AFTER the raw[] append block
         * above, so a spawn landing on this same tick is captured
         * post-append. Step 1 discovery traced a live hadouken
         * same-binary and found raw_len == 10 exactly at the tick the
         * engine's spawn flag is set (three-for-three across LP/MP/HP),
         * matching arcade Hadouken S=10 with no fudge — see Step 2's
         * finalize branch below for how S/R are derived from this slot.
         *
         * Ordering fix: fd_reset_move() at MOVE_START (above, same tick if
         * the move just started) unconditionally re-zeroes
         * fd_engine_proj_spawned[atk] for the fresh move — that happens
         * BEFORE this consume site runs, since MOVE_START detection and
         * this per-tick block both execute within the same call, in that
         * order. effect_13_init() (which sets the flag) runs inside
         * njUserMain(), strictly before this overlay tick (main.c:603 vs
         * 615), so a projectile spawning on a move's very first engine
         * tick would otherwise have its flag cleared out from under it
         * right here, never reaching this `if`. The MOVE_START block now
         * latches that slot-0 case itself (proj_spawn_slot=0,
         * proj_seen=true) before zeroing the flag, so this site's
         * `!g_cur.proj_seen` guard below simply no-ops on that tick — the
         * spawn was already captured upstream. This site still owns every
         * other case: a spawn on any tick AFTER move start.
         *
         * Latches once per move (`!g_cur.proj_seen` guard: multi-projectile
         * supers are explicitly out of scope, see the plan's Step 2 "Do
         * NOT" list — saturate and move on) and does NOT gate on
         * in_hitstop/attacker_already_idle: a spawn on a hitstop tick or
         * after attacker_idle is still well-defined (proj_spawn_slot is
         * simply whatever raw_len is at that moment) — no special-casing
         * needed per the plan.
         *
         * Residual (documented, not fixed): raw_len/proj_seen are read live
         * off g_cur here, not from a snapshot. If a future corpus hits the
         * merged-move case — attacker goes idle (r1=4→0) but finalize is
         * deferred waiting on defender_idle (line ~1176 below), and a fresh
         * attacker move starts (retrigger) before that deferred finalize
         * runs — a spawn belonging to the NEW move could latch onto the
         * OLD move's still-live g_cur.proj_seen/proj_spawn_slot before
         * fd_finalize() reads them. Not constructible in the current
         * corpora (no retriggerable projectile move deferred on
         * defender_idle exists in corpus-q.yaml as of this fix). If it ever
         * shows up, the fix is a §13.7.7-style anchor snapshot: capture
         * proj_seen/proj_spawn_slot at the same "first tick attacker_idle
         * is set" point engine_a_at_atk_idle already uses, instead of
         * reading the live fields at finalize. */
        if (!g_cur.is_throw && !g_cur.proj_seen && fd_engine_proj_spawned[atk]) {
            fd_engine_proj_spawned[atk] = 0;
            g_cur.proj_spawn_slot = g_cur.raw_len;
            g_cur.proj_seen       = true;
        }

        /* §13.7.7 engine_a snapshot. Capture on the FIRST tick where
         * attacker_idle is set, BEFORE any finalize-deferred ticks let
         * a fresh attacker move's active cells inflate the live counter.
         * Fires regardless of which code path set attacker_idle:
         * §13.5.1 cghi=1 cut, §13.6.1 partner-release, or the natural
         * r1=4→0 edge. */
        if (g_cur.atk_idx >= 0
            && g_cur.attacker_idle >= 0
            && g_cur.engine_a_at_atk_idle < 0) {
            g_cur.engine_a_at_atk_idle = (s32)fd_engine_active_count[g_cur.atk_idx];
        }

        /* Finalize when attacker is idle (and defender too for hit/block). */
        if (g_cur.attacker_idle >= 0) {
            const bool needs_def_idle =
                (g_cur.event == FD_OUTCOME_HIT || g_cur.event == FD_OUTCOME_BLOCK);
            if (!needs_def_idle || g_cur.defender_idle >= 0) {
                /* §13.17 lever N finalize deferral (RE-ANCHOR-1) — the
                 * one control-flow change. A WHIFF-candidate window
                 * (g_cur.event == FD_OUTCOME_NONE — the corrected
                 * deferral predicate, NOT !needs_def_idle, which is also
                 * true for PARRY and must never defer here, §2.3) whose
                 * busy edge hasn't yet latched gets up to 20 ticks past
                 * attacker_idle to find it (18(b)-family cut anchors land
                 * 2-4 ticks before the edge; max observed gap in evidence
                 * is 4). Gated on the same box_count/box_runs signals
                 * fd_finalize()'s own lever-N branch reads, so a window
                 * that could never take the busy-edge branch anyway is
                 * never deferred — it finalizes on this very first tick,
                 * byte-identical to legacy. A fresh r1 MOVE_START-shape
                 * edge on the SAME attacker slot (a same-r1 retrigger, or
                 * a genuinely new move) ends the wait immediately: the
                 * ordinary MOVE_START scan can't see this case (gated on
                 * !g_cur.active, above), so this is new, narrowly-scoped
                 * peek logic whose only effect is ending the deferral —
                 * it never touches g_cur's move-identity fields; the next
                 * move's own tracking still begins on the FOLLOWING tick
                 * via the normal scan, once fd_reset_move() below has
                 * run. On timeout or a new-move edge, finalize
                 * immediately: fd_finalize()'s own lever-N gate falls
                 * back to legacy recovery_pf automatically (busy_edge_frame
                 * is still unlatched), so no special-casing is needed
                 * there — busyr_fallback is set purely as a FINAL-line
                 * diagnostic. The census (Step 1) showed zero rows
                 * suite-wide need this path; any occurrence in the
                 * shipped suite is unpredicted drift. */
                bool defer_now = false;
                if (fd_whiff_busy_edge_r
                    && !g_cur.is_throw
                    && g_cur.event == FD_OUTCOME_NONE
                    && g_cur.box_count > 0
                    && g_cur.box_runs == 1
                    && g_cur.busy_edge_frame < 0) {
                    const int elapsed = g_local_frame - g_cur.attacker_idle;
                    const bool new_move_edge =
                        (g_prev[atk].r1 == 0)
                        && (now[atk].r1 == 4 || now[atk].r1 == 2 || now[atk].r1 == 3);
                    if (elapsed < 20 && !new_move_edge) {
                        defer_now = true;
                    } else {
                        g_cur.busyr_fallback = true;
                    }
                }
                if (!defer_now) {
                    fd_finalize();
                    fd_reset_move();
                }
            }
        }
    }

    g_prev[0] = now[0];
    g_prev[1] = now[1];
}

/* Persistent ARGB strips for the two meter rows (288x6 each). Regenerated
 * only when a row's visible cell kinds change (memcmp below), then submitted
 * as a single UI-bitmap quad — replacing 72 per-cell solid rects per row.
 * Static lifetime matches the glyph cache in the software renderer: the
 * borrowed pointer stays valid from submit through RenderFrame. */
static uint32_t fd_meter_px[2][FD_CELL_H * FD_METER_TOTAL_W];
static u8       fd_cache_kinds[2][FD_METER_LEN];
static bool     fd_cache_valid[2] = { false, false };

/* Per-channel scale to ~75% brightness (factor 192/256). Alpha preserved.
 * Applied to odd-indexed cells in fd_submit_meter_row so individual frames
 * are visible without spending pixels on borders. */
static u32 fd_darken_color(u32 color) {
    const u32 a = color & 0xFF000000u;
    const u32 r = (((color >> 16) & 0xFFu) * 192u) >> 8;
    const u32 g = (((color >>  8) & 0xFFu) * 192u) >> 8;
    const u32 b = (( color        & 0xFFu) * 192u) >> 8;
    return a | (r << 16) | (g << 8) | b;
}

static u32 fd_cell_color(u8 kind) {
    switch (kind) {
        case FD_CELL_STARTUP:   return FD_COL_STARTUP;
        case FD_CELL_ACTIVE:    return FD_COL_ACTIVE;
        case FD_CELL_CONTACT:   return FD_COL_CONTACT;
        case FD_CELL_RECOVERY:  return FD_COL_RECOVERY;
        case FD_CELL_HIT:       return FD_COL_HIT;
        case FD_CELL_BLOCK:     return FD_COL_BLOCK;
        case FD_CELL_PARRY:     return FD_COL_PARRY;
        case FD_CELL_HITSTUN:   return FD_COL_HITSTUN;
        case FD_CELL_BLOCKSTUN: return FD_COL_BLOCKSTUN;
        case FD_CELL_THROW:     return FD_COL_THROW;
        case FD_CELL_IDLE:
        case FD_CELL_NONE:
        default:                return FD_COL_BG;
    }
}

static void fd_submit_meter_row(int row, int y, const u8* cells, int len) {
    /* Every cell-slot is painted. Idle cells get the bg color so the bar
     * has a continuous visible width even when the move was short. */
    u8 kinds[FD_METER_LEN];
    for (int i = 0; i < FD_METER_LEN; i++) {
        kinds[i] = (i < len && cells != NULL) ? cells[i] : (u8)FD_CELL_IDLE;
    }

    if (!fd_cache_valid[row]
        || memcmp(kinds, fd_cache_kinds[row], FD_METER_LEN) != 0) {
        for (int i = 0; i < FD_METER_LEN; i++) {
            u32 c = fd_cell_color(kinds[i]);
            if (i & 1) c = fd_darken_color(c);
            const int x0 = i * FD_CELL_W;
            for (int py = 0; py < FD_CELL_H; py++) {
                uint32_t* dst = &fd_meter_px[row][py * FD_METER_TOTAL_W + x0];
                for (int cx = 0; cx < FD_CELL_W; cx++) {
                    dst[cx] = c;
                }
            }
        }
        memcpy(fd_cache_kinds[row], kinds, FD_METER_LEN);
        fd_cache_valid[row] = true;
    }

    Renderer_DrawUIBitmap((float)FD_METER_X_LEFT, (float)y, PrioBase[FD_RECT_PRIO],
                          fd_meter_px[row], FD_METER_TOTAL_W, FD_CELL_H, FD_COL_WHITE);
}

void frame_data_overlay_draw(void) {
    if (!Is_Training_Mode(Mode_Type)) return;

    /* Numeric line — only when we have data, and not for throws. */
    const bool show_numeric = g_latched.valid && !g_latched.is_throw;
    if (show_numeric) {
        char prefix[32];
        char advtag[16];

        snprintf(prefix, sizeof prefix, "S%d A%d R%d T%d ",
                 (int)g_latched.startup, (int)g_latched.active,
                 (int)g_latched.recovery, (int)g_latched.total);

        const bool show_adv = (g_latched.outcome == FD_OUTCOME_HIT
                            || g_latched.outcome == FD_OUTCOME_BLOCK
                            || g_latched.outcome == FD_OUTCOME_PARRY);

        /* §13.6 KD: arcade convention is to display "KD" for moves
         * that leave the opponent knocked-down (CnDB and similar).
         * Numeric adv (+121..+125 = time-until-partner-recovers) is
         * useless to a player. Render "KD" in the throw-color slot. */
        if (show_adv && g_latched.kd) {
            snprintf(advtag, sizeof advtag, "KD");
        } else if (show_adv) {
            snprintf(advtag, sizeof advtag, "%+d", (int)g_latched.advantage);
        }

        s32 prefix_w = SSGetDrawSizePro((const s8*)prefix);
        s32 advtag_w = 0;
        if (show_adv) {
            advtag_w = SSGetDrawSizePro((const s8*)advtag);
        }
        const s32 total_w = prefix_w + advtag_w;
        const s32 x_left  = (384 - total_w) / 2;

        SSPutStrProP(0, (u16)x_left, FD_NUMERIC_Y, FD_TEXT_ATR, FD_COL_WHITE,
                     prefix, FD_TEXT_PRIO);

        if (show_adv) {
            const u32 adv_color = g_latched.kd
                                ? FD_COL_THROW
                                : (g_latched.advantage > 0) ? FD_COL_GREEN
                                : (g_latched.advantage < 0) ? FD_COL_RED
                                : FD_COL_WHITE;
            SSPutStrProP(0, (u16)(x_left + prefix_w), FD_NUMERIC_Y, FD_TEXT_ATR,
                         adv_color, advtag, FD_TEXT_PRIO);
        }
    }

    /* Frame meter rows always render so the bars are visible from the
     * moment training mode opens — empty until the first move finalizes.
     * While a move is in progress, render from g_cur so cells appear in
     * real-time as raw[] is appended; otherwise show the latched meter. */
    const u8* atk_src;
    const u8* def_src;
    int       draw_len;
    if (g_cur.active && !g_cur.is_throw) {
        atk_src  = g_cur.atk_cells;
        def_src  = g_cur.def_cells;
        draw_len = g_cur.meter_len;
    } else if (g_latched.valid && !g_latched.is_throw) {
        atk_src  = g_latched.atk_cells;
        def_src  = g_latched.def_cells;
        draw_len = g_latched.meter_len;
    } else {
        atk_src  = NULL;
        def_src  = NULL;
        draw_len = 0;
    }
    fd_submit_meter_row(0, FD_METER_Y_ATK, atk_src, draw_len);
    fd_submit_meter_row(1, FD_METER_Y_DEF, def_src, draw_len);
}
