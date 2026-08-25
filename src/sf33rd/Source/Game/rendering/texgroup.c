/**
 * @file texgroup.c
 * Texture Group Manager and Loader
 */

#include "sf33rd/Source/Game/rendering/texgroup.h"
#include "arcade/arcade_balance.h"
#include "arcade/arcade_char_data.h"
#include "common.h"
#include "main.h"
#include "port/config/config.h"
#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "sf33rd/Source/Game/engine/charid.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/io/gd3rd.h"
#include "sf33rd/Source/Game/rendering/chren3rd.h"
#include "sf33rd/Source/Game/rendering/texcash.h"
#include "sf33rd/Source/Game/system/ramcnt.h"
#include "structs.h"
#include "test/texgroup_window_probe.h"

#include <SDL3/SDL.h>

#include <stdlib.h>

typedef struct {
    s16 x;
    s16 y;
    u16 attr;
    u16 code;
} TexGroup_UNK_0;

u8 omSelObjNowOnMemoryType = 0xFF;
TEX_GRP_LD texgrplds[100];

const TexGroupData texgrpdat[100] = { { 0, -1, 0, 0, 0, 0, 0 },
                                      { 0, 1460 /* pl00.bin */, 0, 1, 3040072, 210820, 0x2CF158 },     // Gill (0)
                                      { 1568, 1465 /* pl01.bin */, 0, 1, 2552860, 116432, 0x24EDE0 },  // Alex (1)
                                      { 2592, 1468 /* pl02.bin */, 0, 1, 1630652, 72828, 0x178F38 },   // Ryu (2)
                                      { 3552, 1472 /* pl03.bin */, 0, 1, 2231332, 114816, 0x1FAA0C },  // Yun (3)
                                      { 4992, 1476 /* pl04.bin */, 0, 1, 2093456, 110728, 0x1E1D54 },  // Dudley (4)
                                      { 6144, 1479 /* pl05.bin */, 0, 1, 2987268, 116636, 0x2BCE28 },  // Necro (5)
                                      { 7392, 1483 /* pl06.bin */, 0, 1, 3342276, 158400, 0x31392C },  // Hugo (6)
                                      { 8384, 1487 /* pl07.bin */, 0, 1, 2700876, 151744, 0x25D908 },  // Ibuki (7)
                                      { 10208, 1492 /* pl08.bin */, 0, 1, 1849896, 142292, 0x1A0CCC }, // Elena (8)
                                      { 11776, 1495 /* pl09.bin */, 0, 1, 2241340, 137680, 0x205694 }, // Oro (9)
                                      { 13280, 1499 /* pl10.bin */, 0, 1, 2337716, 116892, 0x216E64 }, // Yang (10)
                                      { 14656, 1502 /* pl11.bin */, 0, 1, 1649368, 71900, 0x17C0F8 },  // Ken (11)
                                      { 15712, 1506 /* pl12.bin */, 0, 1, 1715944, 80596, 0x18B378 },  // Sean (12)
                                      { 16800, 1510 /* pl13.bin */, 0, 1, 2286852, 135428, 0x215C68 }, // Urien (13)
                                      { 18272, 1514 /* pl14.bin */, 0, 1, 2649420, 116116, 0x26DFC8 }, // Akuma (14)
                                      { 19456, 1518 /* pl16.bin */, 0, 1, 2672552, 144584, 0x26C73C }, // Chun-Li (15)
                                      { 21120, 1522 /* pl17.bin */, 0, 1, 2770372, 177724, 0x2837C4 }, // Makoto (16)
                                      { 23008, 1525 /* pl18.bin */, 0, 1, 3792908, 222124, 0x37F0B4 }, // Q (17)
                                      { 24704, 1528 /* pl19.bin */, 0, 1, 2345792, 131348, 0x21B47C }, // Twelve (18)
                                      { 25856, 1531 /* pl20.bin */, 0, 1, 1830216, 125420, 0x1A4F50 }, // Remy (19)
                                      { 27040, 1452, 0, 0, 1483, 436, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 27104, 1454, 0, 0, 1128236, 134308, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 29152, 1455, 0, 0, 176650, 14180, 0 },
                                      { 29344, 1456, 0, 0, 595797, 92368, 0 },
                                      { 30640, 1461, 0, 2, 88153, 5788, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 30896, 1457, 0, 0, 35820, 3448, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 31152, 1446, 0, 0, 2148437, 120700, 0 },
                                      { 32432, 1444, 0, 0, 36241, 2580, 0 },
                                      { 36896, 1462, 0, 2, 147800, 4212, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 32560, 1458, 0, 0, 170685, 25088, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 34352, 1401, 0, 0, 47835, 1704, 0 },
                                      { 34384, 1410, 0, 0, 57961, 2180, 0 },
                                      { 34448, 1389, 0, 0, 81332, 19772, 0 },
                                      { 34576, 1395, 0, 0, 47809, 3256, 0 },
                                      { 34672, 1428, 0, 0, 15596, 1672, 0 },
                                      { 34704, 1405, 0, 0, 41072, 1832, 0 },
                                      { 34736, 1413, 0, 0, 16744, 2220, 0 },
                                      { 34832, 1425, 0, 0, 5845, 296, 0 },
                                      { 34864, 1398, 0, 0, 157729, 12208, 0 },
                                      { 34960, 1434, 0, 0, 96435, 4016, 0 },
                                      { 35024, 1386, 0, 0, 62413, 4568, 0 },
                                      { 35120, 1407, 0, 0, 32337, 1100, 0 },
                                      { 35152, 1443, 0, 0, 2302, 64, 0 },
                                      { 35184, 1440, 0, 0, 111032, 7372, 0 },
                                      { 35328, 1431, 0, 0, 214221, 17548, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 35648, 1392, 0, 0, 40123, 2100, 0 },
                                      { 35744, 1448, 0, 0, 102144, 8496, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 },
                                      { 35904, 74, 0, 0, 1024977, 54492, 0 },
                                      { 36096, 34, 0, 0, 335656, 6552, 0 },
                                      { 36160, 35, 0, 0, 67041, 1540, 0 },
                                      { 36192, 36, 0, 0, 171245, 11480, 0 },
                                      { 36288, 37, 0, 0, 84254, 2300, 0 },
                                      { 36320, 38, 0, 0, 57154, 1712, 0 },
                                      { 36352, 39, 0, 0, 120474, 6460, 0 },
                                      { 36384, 40, 0, 0, 62031, 2332, 0 },
                                      { 36416, 41, 0, 0, 20849, 3412, 0 },
                                      { 36448, 42, 0, 0, 10724, 444, 0 },
                                      { 36480, 43, 0, 0, 32145, 1072, 0 },
                                      { 36512, 44, 0, 0, 83717, 2832, 0 },
                                      { 36544, 45, 0, 0, 152553, 4676, 0 },
                                      { 36576, 46, 0, 0, 69000, 5992, 0 },
                                      { 36608, 47, 0, 0, 233695, 8904, 0 },
                                      { 36640, 48, 0, 0, 320671, 14508, 0 },
                                      { 36704, 49, 0, 0, 65229, 1536, 0 },
                                      { 36736, 50, 0, 0, 122647, 2968, 0 },
                                      { 36768, 51, 0, 0, 166764, 6628, 0 },
                                      { 36800, 52, 0, 0, 61918, 2868, 0 },
                                      { 36864, 53, 0, 0, 182919, 4776, 0 },
                                      { 37024, 1459, 0, 0, 621536, 42292, 0 },
                                      { 37408, 1384, 0, 0, 35825, 2488, 0 },
                                      { 37536, 1385, 0, 0, 135261, 12984, 0 },
                                      { 34576, 1416, 0, 0, 47809, 3256, 0 },
                                      { 34448, 1419, 0, 0, 81332, 19772, 0 },
                                      { 34736, 1422, 0, 0, 16744, 2220, 0 },
                                      { 34352, 1437, 0, 0, 47835, 1704, 0 },
                                      { 30640, 1469, 0, 2, 88153, 5788, 0 },
                                      { 30640, 1488, 0, 2, 88153, 5788, 0 },
                                      { 30640, 1496, 0, 2, 88153, 5788, 0 },
                                      { 30640, 1503, 0, 2, 88153, 5788, 0 },
                                      { 30640, 1507, 0, 2, 88153, 5788, 0 },
                                      { 30640, 1511, 0, 2, 88153, 5788, 0 },
                                      { 30640, 1515, 0, 2, 88153, 5788, 0 },
                                      { 30640, 1519, 0, 2, 88153, 5788, 0 },
                                      { 30640, 1532, 0, 2, 88153, 5788, 0 },
                                      { 27104, 1453, 0, 0, 1126129, 134044, 0 },
                                      { 0, -1, 0, 0, 0, 0, 0 } };

// forward decls
s32 load_any_texture_grpnum(u8 grp, u8 kokey);

void q_ldreq_texture_group(REQ* curr) {
    const TexGroupData* bsd;
    uintptr_t ldadr;
    uintptr_t ldchd;
    s32 err;
    s16 i;
    u16* trsbas;
    TexGroup_UNK_0* trsptr;
    s16 count;
    s16 loop;

    bsd = &texgrpdat[curr->ix];

    switch (curr->rno) {
    case 0:
        if (fsCheckCommandExecuting() != 0) {
            break;
        }

        curr->rno = 1;
        curr->fnum = bsd->apfn;

        if (bsd->apfn == -1) {
            *curr->result |= lpr_wrdata[curr->id];
            curr->be = 0;
        }

        if (bsd->num_of_1st == 0) {
            curr->group = obj_group_table[bsd->num_of_1st + 1];
        } else {
            curr->group = obj_group_table[bsd->num_of_1st];
        }

        curr->lds = &texgrplds[curr->group];

        if (curr->lds->ok) {
            if (bsd->ix1st == 1 || bsd->ix1st == 2) {
                switch (rckey_work[curr->lds->key].type) {
                case 3:
#if ENABLE_PERF_TELEMETRY
                    flLogOut("[texgroup-trace] case=3 key=%d id=%d apfn=%d\n", curr->lds->key, curr->id, bsd->apfn);
#endif
                    if (curr->id) {
                        rckey_work[curr->lds->key].type = 5;
                    }

                    break;

                case 4:
#if ENABLE_PERF_TELEMETRY
                    flLogOut("[texgroup-trace] case=4 key=%d id=%d apfn=%d\n", curr->lds->key, curr->id, bsd->apfn);
#endif
                    if (curr->id == 0) {
                        rckey_work[curr->lds->key].type = 5;
                    }

                    break;

                case 5:
#if ENABLE_PERF_TELEMETRY
                    flLogOut("[texgroup-trace] case=5 key=%d id=%d apfn=%d\n", curr->lds->key, curr->id, bsd->apfn);
#endif
                    break;
                }

                if (rckey_work[curr->lds->key].type == 5) {
                    *curr->result |= lpr_wrdata[curr->id];
                    curr->be = 0;
                    return;
                }

                // A duplicate transfer occurred. File number: %d\n
                flLogOut("二重転送が発生しました。ファイル番号：%d\n", bsd->apfn);
                // originally while(1){} from arcade source; skip + log + be=2 (case-4 error convention) instead of hanging
                flLogOut("[texgroup-skip] %s dup-transfer key=%d type=%d id=%d ix=%d apfn=%d kokey=%d\n",
                         __func__, curr->lds->key, rckey_work[curr->lds->key].type,
                         curr->id, curr->ix, bsd->apfn, curr->kokey);
                curr->be = 2;
                return;
            }

            rckey_work[curr->lds->key].type = curr->kokey;
            *curr->result |= lpr_wrdata[curr->id];
            curr->be = 0;
            break;
        }

        /* fallthrough */

    case 1:
        err = fsOpen(curr);

        if (err == 0) {
            curr->rno = 0;
            return;
        }

        curr->rno = 2;
        /* fallthrough */

    case 2:
        curr->size = fsGetFileSize(curr->fnum);
        curr->sect = fsCalSectorSize(curr->size);

        /* texgrplds[grp].key is a SINGLE-SLOT key holder: it is the only
         * surviving reference to the block this group owns, and
         * purge_texture_group() (below) frees exactly that one key. If we
         * land here while the group still holds a live block, the
         * assignment below strands it forever.
         *
         * That state is reachable: the dup-transfer guard at case 0 sets
         * be = 2 and returns *without* resetting rno, which case 0 already
         * advanced to 1. The next pump therefore re-enters at case 1,
         * bypassing the lds->ok check, and falls through to here with
         * lds->ok still 1 and lds->key still live. Measured on the
         * char06-pressure-super rollback repro: "OVERWRITE grp=7 oldkey=5
         * oldok=1 olduse=1 newkey=12" -- 3,342,336 B lost.
         *
         * DO NOT "fix" that asymmetry by resetting rno in the case-0
         * guard. be == 2 keeps the entry at slot 0 (Check_LDREQ_Queue only
         * shifts the queue down when be reaches 0), so rno = 0 would
         * re-enter case 0, hit the same dup-transfer guard, and spin
         * forever -- a permanent head-of-line stall on the load queue.
         * Case 4's reset is safe only because it calls Push_ramcnt_key on
         * the key first, so the retry has something to allocate into.
         *
         * Release through purge_texture_group() rather than a bare
         * Push_ramcnt_key(): purge also clears lds->ok. There are eleven
         * readers of lds->texture_table / lds->trans_table; ten are gated
         * on ok != 0 with an early return (mtrans.c:179, 278, 367, 416,
         * 662, 785, 1043, 1179, 1415, 2393). The eleventh is the Akuma
         * special-case at texgroup.c:371 below, which has NO ok check --
         * it is safe only because it hardcodes texgrplds[15] and runs in
         * the case-4 branch for curr->ix == 15, where that is the group
         * just repointed at the new block, so it never observes the freed
         * one. That is an index coincidence, not the ok invariant; do not
         * generalise the invariant to it.
         *
         * Clearing ok is what makes the now-stale table pointers
         * unreachable instead of dangling; case 4 re-asserts ok once the
         * new block is populated. Freeing before the Pull also lets the
         * new allocation reuse the same block instead of transiently
         * needing two.
         *
         * The nested purge is bounded: purge_texture_group clears ok
         * before calling Push_ramcnt_key (texgroup.c:477-479), so the
         * purge_texture_group(group_num) re-entry inside
         * Push_ramcnt_key_original_2 (ramcnt.c:102) sees ok == 0 and does
         * nothing.
         *
         * Task #64 measured the two behaviours this reclaim introduces
         * that the pre-patch leak did not have. Both are closed; the
         * instrument is src/test/texgroup_window_probe.c, compiled only
         * under -DENABLE_TEXGROUP_WINDOW_PROBE=ON.
         *
         * (1) THE char_init_data 25-POINTER WINDOW. The stated worry was
         * that case 4 republishes those pointers only for ix1st == 1, so
         * a reclaim of an already-published group would leave them
         * dangling permanently. That cannot happen. Reaching here with
         * ok == 1 requires the case-0 dup-transfer guard above, which is
         * itself inside `if (bsd->ix1st == 1 || bsd->ix1st == 2)`
         * (texgroup.c:176) -- ix1st == 0 drains at :221-224 and never
         * sets be = 2. Resolving texgrpdat[]'s num_of_1st through
         * obj_group_table gives: ix1st == 1 covers groups 1..20 and
         * nothing else, ix1st == 2 covers groups 27 and 35 and nothing
         * else. The two sets are disjoint, and char_init_data /
         * parabora_own_table are written only in the ix1st == 1 branch
         * below (:443-445, :471). So every reclaim that can strand those
         * pointers is a reclaim of a group in 1..20, i.e. one whose own
         * case 4 republishes them. Groups 2..20 have exactly one
         * texgrpdat entry each and group 1's other entries all carry
         * apfn == -1 (drained at :162-165 without loading), so the block
         * freed at :359 and the file re-read at :362/:369 are always the
         * same file at the same length: the refill rewrites the block
         * with the bytes that were already in it.
         *
         * What is real is the transient window, and it is wider than one
         * pump: measured on tools/rollback-determinism/run.sh select,
         * both scenarios, with the ldreq barrier OFF (the offline
         * single-step path), the reclaim stranded 25/25 pointers of one
         * char_init_data slot plus one parabora_own_table slot and the
         * window stayed open across 7 Main_Jmp_Tbl dispatches. No read
         * landed in it: set_char_base_data (charid.c) and
         * setup_butt_own_data (pls02.c) are the tree's only readers of
         * those two tables, and both run under Game02/Game09, which are
         * entered only through Game2_0 (game.c:475-477) or Game2_2
         * (game.c:618-620) -- each of which fatal_error()s unless
         * Check_LDREQ_Clear(), and no Push_LDREQ_Queue_* site executes
         * under Game02. A live reclaim and a live reader are therefore
         * mutually exclusive by that assertion. Residual not observed in
         * 2/2 samples and not forced: mmAllocSub is best-fit
         * (MemMan.c:65-93), so if the freed cell coalesces with adjacent
         * free space the Pull at :362 may land at a different address;
         * both measured refills returned the identical address.
         *
         * (2) getObjectHeight RETURNING 0 INTO CHECKSUMMED plw STATE
         * while ok == 0 (mtrans.c:366-368 -> plpcu.c:248). Unreachable.
         * Its only caller is check_tsukamare_keizoku_check, which runs
         * under G_No == {2,2,1} or G_No[1] == 9, and for a PLW
         * obj_group_table[wk->wu.cg_number] lands in 1..20. Groups 1..20
         * lose ok only via purge_texture_group, whose three call sites
         * are this one, texgroup.c:557 (reset_dma_group, attract mode,
         * group 61 only) and ramcnt.c:102. This one cannot fire during
         * Game02 by the Check_LDREQ_Clear() argument above. ramcnt.c:102
         * needs a key with a nonzero group_num, and the only such free
         * that can execute while G_No == {2,2,1} is the soft reset --
         * Purge_mmtm_area(6) at game.c:1819, which is followed four
         * lines later at game.c:1822-1828 by G_No[ix] = 0 inside the
         * same Next_Title_Sub() call, with no Main_Jmp_Tbl dispatch in
         * between (and game.c:164 suppresses that dispatch under
         * nowSoftReset() anyway). Empirically: 2611 executions of
         * check_tsukamare_keizoku_check across 77 frame-data battle
         * corpora produced zero ok == 0 returns. */
        if (curr->lds->ok && curr->lds->key > 0 && rckey_work[curr->lds->key].use) {
#if ENABLE_PERF_TELEMETRY
            flLogOut("[texgroup-reclaim] %s freeing stranded block grp=%d key=%d ix=%d id=%d\n",
                     __func__, (int)curr->group, (int)curr->lds->key, (int)curr->ix, (int)curr->id);
            TGWP_ReclaimOpen(curr->group, curr->lds->key, curr->id, curr->ix, bsd->ix1st);
#endif
            purge_texture_group(curr->group);
        }

        curr->key = Pull_ramcnt_key(curr->sect << 0xB, curr->kokey, curr->group, curr->frre);
        curr->lds->key = curr->key;
        Set_size_data_ramcnt_key(curr->key, curr->size);
        curr->rno = 3;
        /* fallthrough */

    case 3:
        err = fsRequestFileRead(curr, curr->sect, (void*)Get_ramcnt_address(curr->key));

        if (err == 0) {
            Push_ramcnt_key(curr->key);
            fsClose(curr);
            curr->rno = 0;
            return;
        }

        curr->rno = 4;
        curr->be = 1;
        break;

    case 4:
        switch (fsCheckFileReaded(curr)) {
        case 1:
            fsClose(curr);
            ldadr = Get_ramcnt_address(curr->key);
            curr->lds->texture_table = ldadr + bsd->to_tex;
            curr->lds->trans_table = ldadr;
            curr->lds->ok = 1;

            switch (bsd->ix1st) {
            case 1:
                ldchd = ldadr + bsd->to_chd;

                // Explanation:
                //
                // The code above loads a bunch of data from the AFS partition.
                // This data includes character init data which starts at `ldchd`.
                // Data at `ldchd` starts with 25 4-byte ints which are offsets
                // from `ldchd` to the actual data.
                //
                // On PS2 it is okay to just add `ldchd` to each of these offsets
                // to turn them into pointers, because a 4-byte int can hold a pointer.
                // However on modern 64-bit platforms pointers are bigger, meaning we
                // can't add `ldchd` to the offsets inplace. That's why we have to
                // allocate a separate memory region for `cit` and compute the pointers
                // that comprise it there.
                //
                // Because 25 is the number of members in CharInitData struct, `i` goes
                // to 25 too.

                const s16 character_id = plt_req[curr->id];
                CharInitData* dst = &char_init_data[plid_data[character_id]];

                bool arcade_data_applied = false;

                if (ArcadeBalance_IsEnabled()) {
                    const size_t ps2_char_data_size = curr->size - bsd->to_chd;
                    const bool adapted = ArcadeCharData_Apply3SXRenderingConventions(
                        character_id, (const void*)ldchd, ps2_char_data_size
                    );
                    const CharInitData* arcade_data = ArcadeCharData_Get(character_id);

                    SDL_assert(adapted && arcade_data != NULL);

                    if (!adapted || arcade_data == NULL) {
                        // Fall through to the PS2 population below. An early
                        // return here (upstream's shape) leaves the load
                        // request undrained: be stays nonzero, the queue never
                        // clears, and Check_LDREQ_Clear() aborts on timeout.
                        SDL_LogCritical(
                            SDL_LOG_CATEGORY_APPLICATION,
                            "Could not adapt arcade character data for character %d; using PS2 data",
                            character_id
                        );
                    } else {
                        SDL_copyp(dst, arcade_data);
                        arcade_data_applied = true;
                    }
                }

                if (!arcade_data_applied) {
                    for (i = 0; i < 25; i++) {
                        ((uintptr_t*)dst)[i] = ldchd + ((u32*)ldchd)[i];
                    }

                    // Q specific code
                    if (curr->ix == 18) {
                        dst->cbca[37] = dst->cbca[3];
                    }

                    // Akuma specific code
                    if (curr->ix == 15) {
                        trsbas = (u16*)(((u32*)texgrplds[15].trans_table)[166] + texgrplds[15].trans_table);
                        count = *trsbas;
                        count -= 1;
                        trsbas[0] = count;
                        trsbas += 1;
                        trsptr = (TexGroup_UNK_0*)trsbas;
                        trsptr[0].x += trsptr[1].x;
                        trsptr[0].y += trsptr[1].y;
                        trsptr[0].attr = trsptr[1].attr;
                        trsptr[0].code = trsptr[1].code;

                        for (loop = 1; loop < count; loop++) {
                            trsptr[loop] = trsptr[loop + 1];
                        }
                    }
                }

                parabora_own_table[character_id] = dst->prot;
            }

            TGWP_LoadComplete(curr->group, bsd->ix1st, curr->key);
            *curr->result |= lpr_wrdata[curr->id];
            curr->be = 0;
            break;

        case 0:
            break;

        default:
            Push_ramcnt_key(curr->key);
            fsClose(curr);
            curr->be = 2;
            curr->rno = 0;
            break;
        }

        break;
    }
}

void Init_texgrplds_work() {
    s16 i;

    // Zero out the 0-th element of texgrplds
    for (i = 0; i < sizeof(TEX_GRP_LD) / sizeof(u32); i++) {
        ((u32*)texgrplds)[i] = 0;
    }

    for (i = 1; i < 100; i++) {
        texgrplds[i] = texgrplds[0];
    }
}

void reservMemKeySelObj() {
    TEX_GRP_LD* lds;
    s32 size;

    size = fsCalSectorSize(0x11372CU) << 0xB;
    lds = &texgrplds[obj_group_table[0x69E0]];
    lds->key = Pull_ramcnt_key(size, 0xD, 0, 1);

    if (lds->key < 0) {
        // originally while(1){} from arcade source; skip + log instead of hanging
        flLogOut("[texgroup-skip] %s key-alloc-failed key=%d size=%d\n", __func__, lds->key, size);
        return;
    }
}

void checkSelObjFileLoaded() {
    const TexGroupData* bsd;
    TEX_GRP_LD* lds;
    uintptr_t ldadr;
    s32 rnum;

    if (omSelObjNowOnMemoryType == mpp_w.language) {
        return;
    }

    if (mpp_w.language == LANG_JAPANESE) {
        bsd = &texgrpdat[0x62];
    } else {
        bsd = &texgrpdat[0x17];
    }

    lds = &texgrplds[obj_group_table[0x69E0]];

    while (1) {
        rnum = load_it_use_this_key(bsd->apfn, lds->key);

        if (rnum != 0) {
            break;
        }
    }

    ldadr = Get_ramcnt_address(lds->key);
    lds->texture_table = ldadr + bsd->to_tex;
    lds->trans_table = ldadr;
    lds->ok = 1;
    omSelObjNowOnMemoryType = mpp_w.language;
    Clear_texcash_work();
}

void purge_texture_group_of_this(u16 patnum) {
    purge_texture_group(obj_group_table[patnum]);
}

void purge_texture_group(u8 grp) {
    if (texgrplds[grp].ok != 0) {
        texgrplds[grp].ok = 0;
        Push_ramcnt_key(texgrplds[grp].key);
    }
}

void purge_player_texture(s16 id) {
    s16 emid;
    s16 pkey;

    emid = (id + 1) & 1;

    if ((pkey = Search_ramcnt_type(lpt_seldat[2])) != 0) {
        while (1) {
            rckey_work[pkey].type = lpt_seldat[emid];

            if (!(pkey = Search_ramcnt_type(lpt_seldat[2]))) {
                break;
            }
        }
    }

    while (1) {
        pkey = Search_ramcnt_type(lpt_seldat[id]);

        if (pkey == 0) {
            break;
        }

        Push_ramcnt_key(pkey);
    }
}

s32 load_any_texture_patnum(u16 patnum, u8 kokey, u8 _unused) {
    return load_any_texture_grpnum(obj_group_table[patnum], kokey);
}

s32 load_any_texture_grpnum(u8 grp, u8 kokey) {
    const TexGroupData* bsd;
    TEX_GRP_LD* lds;
    uintptr_t ldadr;

    if (grp == 0) {
        return 0;
    }

    lds = &texgrplds[grp];
    bsd = &texgrpdat[grp];

    if (lds->ok) {
        return 0;
    }

    lds->key = load_it_use_any_key(bsd->apfn, kokey, grp);
    ldadr = Get_ramcnt_address(lds->key);
    lds->texture_table = ldadr + bsd->to_tex;
    lds->trans_table = ldadr;
    lds->ok = 1;
    return 1;
}
