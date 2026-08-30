#ifndef TEST_LDREQ_TIMING_TRACE_H
#define TEST_LDREQ_TIMING_TRACE_H

/* Loader-timing invariance instrument (task #66).
 *
 * WHY THIS EXISTS AND NOT THE ROLLBACK-DETERMINISM HARNESS.
 * The LDREQ/AFS cluster is nondeterministic *without any rollback at
 * all* — docs/rollback-determinism-harness.md known limit 9 records that
 * q_ldreq, rckey_work, rckey_mmobj, texgrplds, char_init_data, requests,
 * afs and asyncio_queue all land in that harness's own A1-vs-A2 BASELINE
 * NOISE list. Two identical no-rollback runs of one binary disagree
 * about them. That harness therefore cannot see a divergence in this
 * cluster (noise classification eats it first), and its run-B model
 * compresses `depth` predicted frames into one real frame, so the
 * loader's wall-clock cadence in run B matches neither run A1 nor
 * production. The same document says in as many words: do not use it to
 * validate a claimed fix here.
 *
 * WHAT THIS MEASURES INSTEAD.
 * The bug is wall-clock coupling, so the experiment is to vary the wall
 * clock and check the simulation is invariant. Two runs of one binary,
 * identical scripted inputs, identical pinned RNG, differing only in
 * --afs-inject-latency-ms (which delays the *observed* completion of
 * every async read; see AFS_SetInjectedLatencyMs in port/io/afs.h).
 * Each run writes one CSV row per outer frame carrying
 *
 *   (a) the saved simulation state the loader actually feeds —
 *       Exit_No/Exit_Timer (game_state.c:456/1196, :607/1347) and
 *       G_No/G_Timer, i.e. the values a desync checksum compares; and
 *   (b) the loader's own observable surface — the ldreq_result[] hash,
 *       plt_req[], the head queue slot, and the return values of
 *       Check_PL_Load()/Check_LDREQ_Clear().
 *
 * If the two rows ever differ at the same frame index, a peer whose disk
 * ran that much slower would have diverged. That is the desync, isolated
 * from every other moving part.
 *
 * The instrument is self-neutralizing: the same binary run without
 * --ldreq-barrier-force takes the unbarriered path and must go RED. A
 * green result that cannot be turned red on demand is not evidence.
 *
 * WHY --ldreq-barrier-force HAS TO EXIST (rather than running a real
 * two-peer session). A scripted netplay match is impossible on this
 * tree, and the reason is structural, not effort:
 *
 *   - NetplayNav's state machine ends at NAV_START_NETPLAY -> NAV_DONE
 *     (netplay/netplay_nav.c:468-471) and injects no input past that
 *     point, so two auto-navigated peers connect and then sit in
 *     character select forever (docs/netplay-auto-nav.md:33-34, :88).
 *   - The only character-select automation in the tree is the test
 *     runner (test/test_runner.c:1358-1413), and --test-enable pins PS2
 *     balance (arcade/arcade_balance.c:115-121), which makes
 *     ArcadeBalance_IsEnabled() and therefore Netplay_ArmAllowed()
 *     (netplay/netplay.c:2030-2032) false, so netplay refuses to arm
 *     (netplay_nav.c:166-169, main.c:339-344).
 *
 * The two automations are mutually exclusive by construction. The force
 * flag is what lets the barrier be exercised from the automation that
 * does reach character select. The live gate it stands in for is one
 * line — Netplay_GetSessionState() == NETPLAY_SESSION_RUNNING — and is
 * the only part of this fix not covered by an executable test.
 *
 * Inert unless --ldreq-trace is passed; real body only in #if DEBUG.
 *
 * PER-SLOT SIDE CHANNEL (--ldreq-slot-trace, task #69.2).
 * The row above records q_ldreq[0] only. That is enough to gate timing
 * invariance but cannot answer whether the residual q_ldreq disagreement
 * between two latency runs sits in slots that are DRAINED (be == 0) or in
 * LIVE ones — and only the second would falsify the barrier's "the queue
 * is empty at every simulated-frame boundary" clause. --ldreq-slot-trace
 * writes a SEPARATE CSV with one row per (frame, slot) for all 16 slots,
 * every REQ field decoded plus a pointer-normalised raw byte image, and
 * tools/ldreq-timing/check_slot_residue.py classifies every difference.
 * It is a separate file on purpose: check_ldreq_timing.py compares whole
 * rows of the main trace, so folding 16 slots into it would silently
 * change what the PASS/FAIL gate means.
 *
 * Measured on this tree at 400 ms (2026-08-25, 600 frames):
 *   barrier ON  — 0 LIVE-slot differences, 0 frame boundaries with a
 *                 non-empty queue; 191 DRAINED-slot differences, all in
 *                 slot 0, all in the single field `size`.
 *   barrier OFF — 1396 LIVE-slot differences; 24 (lat 0) and 256 (lat
 *                 400) frame boundaries with a non-empty queue.
 * The OFF column is the neutralization: the probe goes red on demand.
 *
 * That `size` residue is dead, and provably so rather than by argument.
 * Push_LDREQ_Queue_Player/_Union/_Direct (gd3rd.c:405-488) never assign
 * REQ.size, and Push_LDREQ_Queue copies the whole stack-local struct
 * (gd3rd.c:559), so a drained slot carries a stack-garbage `size` — in
 * the two runs above, 0x0056BC30 vs 0x02287C30, equal in the low 12 bits,
 * the page-offset signature of an address-valued word. Nothing reads it:
 * the only readers of REQ.size are texgroup.c:259/381/435 and
 * color3rd.c:107/109, all inside ldreq_process[] whose sole call site is
 * gd3rd.c:743, reachable only with be != 0 (guards at gd3rd.c:872 and
 * :887); and every fresh request enters at rno == 0 (gd3rd.c:561) and is
 * assigned `size` at rno == 2 (texgroup.c:258, color3rd.c:106) before its
 * first read.
 */

/* Called once per outer frame, after game_step_1(), alongside
 * RollbackDeterminism_FrameEnd(). Appends one row; on reaching
 * --ldreq-trace-frames flushes, closes and requests a clean exit. */
void LdreqTimingTrace_FrameEnd(void);

#endif // TEST_LDREQ_TIMING_TRACE_H
