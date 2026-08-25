#ifndef GD3RD_H
#define GD3RD_H

#include "structs.h"
#include "types.h"

#include <stdbool.h>

extern s16 plt_req[2];
extern const u8 lpr_wrdata[3];
extern const u8 lpt_seldat[4];

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

/* === Netplay LDREQ frame barrier (task #66) ===
 *
 * True while the simulation must not be allowed to observe a load that
 * is still in flight. See the block comment above Check_LDREQ_Queue()
 * in gd3rd.c for the full argument. Two sources, OR'd:
 *
 *   - a GekkoNet session is in NETPLAY_SESSION_RUNNING, i.e. frames are
 *     being driven by the rollback engine and a peer is watching the
 *     same checksummed state; or
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
 * The barrier's invariant is scoped to frames DURING a session. It says
 * nothing about the instant a session begins, and Ldreq_BarrierActive()
 * turns on only AFTER session_state becomes NETPLAY_SESSION_RUNNING
 * (gd3rd.c:510), so anything still in flight when that flip happens was
 * pumped under the stock single-step path. Whether two peers can be at
 * different points of a load at that instant is a measurement, not an
 * argument, and this is the instrument: it dumps the whole loader surface
 * — every q_ldreq[].be, afs_handle, the AFS in-flight/open counts, and a
 * hash + set-bit count of ldreq_result[] — to the SDL log under a caller
 * -supplied tag, so two peers' logs can be diffed line for line.
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
 * System_all_clear_Level_B() (sys_sub.c:983-986, called by setup_vs_mode)
 * is only Bg_Close() + effect_work_init(). The TRANSITIONING flip is
 * gated on task[TASK_INIT].condition == 0 alone (netplay.c:1533) and on
 * nothing at all for matchmaking (netplay.c:1584-1592), and the
 * G_No[1] 12 -> 1 path it then waits on (game.c:303-341) gates on
 * Switch_Screen(1), a frame-counted wipe, not on Check_PL_Load() or
 * Check_LDREQ_Clear(). A load left in flight at that flip therefore has
 * no structural guarantee of being drained before configure_gekko().
 * Producing one needs a session start from a screen with an outstanding
 * push (e.g. demo00.c:130/154/166), which no automation in this tree can
 * drive. Treat "empty at session start" as measured-on-one-path, not
 * proven. */
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
