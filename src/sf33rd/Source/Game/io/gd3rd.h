#ifndef GD3RD_H
#define GD3RD_H

#include "structs.h"
#include "types.h"

#include <stdbool.h>

extern s16 plt_req[2];
extern const u8 lpr_wrdata[3];
extern const u8 lpt_seldat[4];

/* Declared here rather than left to each consumer's own `extern`. Both of
 * these were being re-declared by hand, with their dimensions written out a
 * second time, in src/test/ldreq_timing_trace.c -- which then hashes
 * `sizeof(ldreq_result)` and sweeps `q_ldreq` slot by slot to decide whether
 * the loader barrier holds. A private copy of the dimension means the probe
 * keeps compiling after the real array changes size and silently measures a
 * PREFIX of it: the barrier evidence would still read green while covering
 * less than the queue. With one declaration visible to every translation
 * unit, a size change is a conflicting-declaration error at each site
 * instead. */
extern REQ q_ldreq[16];
extern u8 ldreq_result[294];

s32 fsOpen(REQ* req);
void fsClose(REQ* /* unused */);
u32 fsGetFileSize(u16 fnum);
u32 fsCalSectorSize(u32 size);
s32 fsCheckCommandExecuting();
s32 fsRequestFileRead(REQ* /* unused */, u32 sec, void* buff);
s32 fsCheckFileReaded(REQ* /* unused */);
s32 fsFileReadSync(REQ* req, u32 sec, void* buff);
void waitVsyncDummy();
s16 load_it_use_any_key(u16 fnum, u8 kokey, u8 group);
s32 load_it_use_any_key2(u16 fnum, void** adrs, s16* key, u8 kokey, u8 group);
s32 load_it_use_this_key(u16 fnum, s16 key);
void Init_Load_Request_Queue_1st();
void Request_LDREQ_Break();
u8 Check_LDREQ_Break();
void Push_LDREQ_Queue_Player(s16 id, s16 ix);
void Check_LDREQ_Queue();
s32 Check_LDREQ_Clear();
s32 Check_LDREQ_Queue_Player(s16 id);
void Push_LDREQ_Queue_Direct(s16 ix, s16 id);
void Push_LDREQ_Queue_Player(s16 id, s16 ix);
void Push_LDREQ_Queue_BG(s16 ix);
s32 Check_LDREQ_Queue_BG(s16 ix);
s32 Check_LDREQ_Queue_Direct(s16 ix);

/* === Netplay LDREQ frame barrier (task #66, widened by task #72) ===
 *
 * True while the simulation must not be allowed to observe a load that
 * is still in flight. See the block comment above Check_LDREQ_Queue()
 * in gd3rd.c for the full argument. Two sources, OR'd:
 *
 *   - a netplay session exists and is running the simulation, i.e. the
 *     session state is NETPLAY_SESSION_TRANSITIONING, _CONNECTING or
 *     _RUNNING. Task #72 established that all three step the game and
 *     therefore pump this queue; keying on _RUNNING alone (the #66 form)
 *     left the pre-session run and session frame 0 on the stock
 *     wall-clock-coupled path. The per-state enumeration and the reason
 *     _IDLE and _EXITING are excluded live on Ldreq_BarrierActive() in
 *     gd3rd.c; or
 *   - Ldreq_SetBarrierForced(true), the harness override
 *     (--ldreq-barrier-force), which exists so the timing-invariance
 *     instrument can exercise the barrier from an offline test-runner
 *     scene. Never set outside a Debug harness run.
 *
 * Offline single-player therefore returns false here and takes the
 * unmodified code path.
 */
bool Ldreq_BarrierActive(void);
void Ldreq_SetBarrierForced(bool forced);

#if ENABLE_PERF_TELEMETRY
/* === Session-start skew probe (task #69.3) ===
 *
 * WHEN THIS WAS WRITTEN, the barrier's invariant was scoped to frames
 * during NETPLAY_SESSION_RUNNING only. It said nothing about the instant
 * a session begins, so anything still in flight when the RUNNING flip
 * happened had been pumped under the stock single-step path. Whether two
 * peers can be at different points of a load at that instant is a
 * measurement, not an argument, and this is the instrument: it dumps the
 * whole loader surface — every q_ldreq[].be, afs_handle, the AFS
 * in-flight/open counts, and a hash + set-bit count of ldreq_result[] —
 * to the SDL log under a caller-supplied tag, so two peers' logs can be
 * diffed line for line.
 *
 * Pure reads. Call sites: netplay.c (the RUNNING flip and the top of
 * configure_gekko(), plus a bounded per-frame timeline over
 * TRANSITIONING / CONNECTING / the first 240 RUNNING frames).
 *
 * WHAT IT MEASURED (2026-08-25). Two real local peers, the documented
 * two-instance harness in docs/netplay-auto-nav.md, differing only in
 * --afs-inject-latency-ms (0 vs 400) — i.e. an actual GekkoNet session
 * start, not a forced flag. Both peers, at BOTH instants:
 *
 *   q_be=0000000000000000  afs_handle=-1  fs_busy=0
 *   ldreq_result_h=4de778c5  ldreq_result_bits=0  plt_req=0/0
 *
 * identical across the two peers, and identical across all 314 / 260
 * probe rows, even though the peers spent 66 vs 12 frames in
 * TRANSITIONING. So no skew on the path that can be exercised here.
 *
 * WHAT IT DID NOT SETTLE. Nothing on the start path CLEARS the queue:
 * Init_Load_Request_Queue_1st has zero call sites under src/netplay/, and
 * System_all_clear_Level_B() (sys_sub.c:982-985, called by setup_vs_mode)
 * is only Bg_Close() + effect_work_init(). The TRANSITIONING flip is
 * gated on task[TASK_INIT].condition == 0 alone (netplay.c:1551) and on
 * nothing at all for matchmaking (netplay.c:1584-1592), and the
 * G_No[1] 12 -> 1 path it then waits on (game.c:303-341) gates on
 * Switch_Screen(1), a frame-counted wipe, not on Check_PL_Load() or
 * Check_LDREQ_Clear(). A load left in flight at that flip therefore has
 * no structural guarantee of being drained before configure_gekko().
 * Producing one needs a session start from a screen with an outstanding
 * push (e.g. demo00.c:130/154/166), which no automation in this tree can
 * drive. Treat "empty at session start" as measured-on-one-path, not
 * proven.
 *
 * TASK #72 SUPPLIED THAT GUARANTEE (2026-08-25). Ldreq_BarrierActive()
 * now also covers TRANSITIONING and CONNECTING, so every pre-session
 * frame drains the queue before it ends and the "nothing clears the
 * queue" gap above is closed by construction rather than by coverage.
 * The probe is kept as the standing check on that claim: a
 * configure-gekko or session-running row with q_nonempty != 0 or
 * afs_handle != -1 still means the drain did not happen and this item
 * reopens.
 *
 * afs_reading ALONE IS NOT THAT TRIGGER. Task #72 saw afs_reading=1 /
 * afs_open=1 on the last CONNECTING row and at the RUNNING flip, on both
 * peers of both runs, with afs_handle == -1 and q_be all zeros. That is
 * not an LDREQ read: the only other AFS_Read call site in the tree is
 * adx.c:457 (track_start_async_load), which is outside this barrier's
 * domain. Read the afs_handle and q_nonempty columns, not afs_reading. */
void Ldreq_LogSessionProbe(const char* tag, int frame);
#endif

/* Two independent bounds on the barrier drain, because the two failure
 * modes they guard have opposite shapes.
 *
 * BUDGET_MS bounds a slow or stuck *disk*: it must stay comfortably
 * under GekkoNet's NetStats::DISCONNECT_TIMEOUT of 5000 ms
 * (third_party/GekkoNet/build/include/net.h:125), which the remote peer
 * measures from the last packet of any type it received from us — and we
 * send nothing while blocked here.
 *
 * MAX_STEPS bounds a *livelock* that never touches the disk: a head
 * request stuck cycling be == 2 (q_ldreq_texture_group's dup-transfer
 * guard, texgroup.c) would spin thousands of iterations per millisecond
 * and blow through no wall-clock budget at all. Note REQ.retry is set to
 * 0x40 at gd3rd.c but never decremented anywhere in the tree, so there
 * is no retry counter to rely on. */
#define LDREQ_BARRIER_BUDGET_MS 3000
#define LDREQ_BARRIER_MAX_STEPS 8192

#endif
