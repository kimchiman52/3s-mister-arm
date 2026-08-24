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
