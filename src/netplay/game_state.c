#include "netplay/game_state.h"
#include <unistd.h>
#include "sf33rd/Source/Game/animation/appear.h"
#include "sf33rd/Source/Game/animation/win_pl.h"
#include "sf33rd/Source/Game/effect/eff56.h"
#include "sf33rd/Source/Game/effect/eff79.h"
#include "sf33rd/Source/Game/effect/effb2.h"
#include "sf33rd/Source/Game/effect/effb8.h"
#include "sf33rd/Source/Game/effect/effect.h"
/* effl8.h is the source of truth for the ColorRAM rows effect L8 mutates;
 * EFFL8_COLORRAM_ROWS below is derived from its macros, not restated. */
#include "sf33rd/Source/Game/effect/effl8.h"
#include "sf33rd/Source/Game/ending/end_data.h"
#include "sf33rd/Source/Game/engine/charset.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/slowf.h"
#include "sf33rd/Source/Game/engine/spgauge.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/stage/bg_data.h"
#include "sf33rd/Source/Game/stage/ta_sub.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "sf33rd/Source/Game/ui/count.h"
#include "sf33rd/Source/Game/ui/sc_sub.h"
#include "sf33rd/utils/djb2_hash.h"

#include "gekkonet.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>

// ============================================================================
// Compile-time guard: sizeof(GameState) tripwire.
// If this assert fires, a field was added or removed from GameState.
// Steps: 1) Add the corresponding GS_SAVE/GS_LOAD line in this file.
//        2) Update EXPECTED_GAME_STATE_SIZE to the new sizeof(GameState).
// These values differ from 3sxtra's (17800 / 19376) because our fork keeps
// combo_type, remake_power, and Disp_Input_History as top-level fields that
// 3sxtra moved into PLW (research doc §5.4). The actual numbers are pinned
// after the Phase 1 backfill by adjusting until the build compiles.
// ============================================================================
// Our sizes differ from 3sxtra's (17800 / 19376). Contributing structural
// deltas (research doc §5.4 and §5.6) include:
//   - PLW shrinkage on our side: 3sxtra embeds combo_type and remake_power
//     inside PLW, so 3sxtra's PLW is 2 × 2 × sizeof(ComboType) larger.
//   - Top-level additions on our side: combo_type[2], remake_power[2],
//     Disp_Input_History as fork-exclusive GameState fields.
//   - _TASK shrinkage on our side: ours lacks 3sxtra's callback_adrs
//     (4 bytes × 11 task slots on 32-bit).
//   - Padding effects from struct alignment.
// The exact net bytes don't reconcile cleanly via casual arithmetic; the
// 32-bit value below was pinned empirically from compiler output. Treat it
// as ground truth and update via re-pinning whenever GameState changes.
#if UINTPTR_MAX == 0xffffffff
/* 17580 + 72 (chainex_check[2][36]) = 17652 (added 2026-04-24 when
 * chainex_check was pulled into GameState to close a rollback-unsafe
 * file-static escape, see GameState_Save comment); + 4 for the
 * Disp_Frame_Data u8 (one byte rounded to the struct's 4-byte
 * alignment) = 17656.
 *
 * The #298 port (e57b16bd) grew this on ARM32 (32-bit pointers) by a
 * net +8, to 17664:
 *   + 2  OK_Appear79[2]  (u8[2])
 *   + 2  Extra_Counter[2] (u8[2])
 *   + 8  Demo_Ptr[2]     (u16*[2], two 4-byte pointers on ARM32)
 *   - 2  Random_ix16_bg  (s16, removed by that port; RESTORED later — see
 *                         the re-pin note at the bottom of this comment)
 *   - 2  padding re-absorbed by the field-layout shuffle above
 *   -----
 *   + 8  net
 * 17656 + 8 = 17664.
 *
 * Port of d5f301cc (ca_check_flag + Color7 only — eff79 OK_Appear79/
 * Extra_Counter are already covered above by the #298 port, so they are
 * NOT duplicated) grew this by a further +8, to 17672:
 *   + 4  Color7[2]       (u16[2], appended right after chainex_check)
 *   + 1  ca_check_flag   (s8, appended right after Color7)
 *   + 3  trailing padding to restore the struct's 4-byte alignment
 *   -----
 *   + 8  net
 * 17664 + 8 = 17672. Empirically confirmed via ARM32 cross-compile
 * (clang --target=arm-linux-gnueabihf): actual sizeof(GameState) == 17672,
 * matching this arithmetic exactly.
 *
 * Upstream #296 (de2eccc9, "Unlock extra colors and Gill by default")
 * converts the Present_Mode global from u8 to the new PresentMode enum;
 * game_state.h's Present_Mode field follows suit (u8 -> PresentMode) so
 * GS_ASSERT_SAME_SIZE keeps passing. On the AAPCS ARM32 ABI a plain enum
 * is int-sized (4 bytes), so the field grows from 1 byte to 4 bytes where
 * it sits between VS_Stage (s8) and Play_Mode (u8) - both single bytes -
 * which also forces alignment padding before it. The net effect doesn't
 * reduce to simple arithmetic (some of the padding shift is absorbed
 * downstream), so this was re-pinned empirically the same way as the
 * entries above: cross-compiled with the OLD 17672 constant, read the
 * static_assert failure's "note: expression evaluates to 'N == 17672'",
 * and took N. Actual sizeof(GameState) == 17676 (net +4 over 17672).
 *
 * H-6 fix (2026-08-23) appended spmv_ng_save[2] (u32[2], Makoto SA-buff
 * backup of the checksummed PLW.spmv_ng_flag — see the struct comment for
 * the corrected Candidate-0b history) right after ca_check_flag, growing
 * this by +8: the s8's former 3 trailing pad bytes stay as pre-u32
 * alignment padding, then 8 bytes of payload land on the struct's 4-byte
 * boundary. Re-pinned with the clang --target=arm-linux-gnueabihf layout
 * probe (deliberate `extern char probe[sizeof(GameState)]` vs
 * `extern char probe[1]` redeclaration conflict; compiler reported
 * 'char[17684]', and the same probe reproduces 'char[17676]' on the
 * pre-change header, validating the environment). 17676 + 8 = 17684.
 *
 * Restoring Random_ix16_bg (s16, put back between Random_ix32_ex_com and
 * Opening_Now — the #298 port had dropped it on a premise that is false on
 * this fork; see the GameState_Save comment) grew this by +4, not +2: the
 * field lands in a run of s16s that ends at `struct _TASK task[11]`, so the
 * 2 payload bytes turn an even halfword count into an odd one and 2 bytes of
 * realignment padding follow. Re-measured, not computed — same clang
 * --target=arm-linux-gnueabihf redeclaration probe as above (`extern char
 * probe[sizeof(GameState)]` vs `extern char probe[1]`), which reported
 * 'char[17688]' after the change and reproduced 'char[17684]' on the
 * pre-change header. 17684 + 4 = 17688.
 *
 * effl8_colorram[4][12] (u16) appended at the end of the struct grew this by
 * a clean +96: 4 rows x 12 u16 = 96 bytes of payload landing on the struct's
 * 4-byte alignment with no padding on either side. Re-measured with the same
 * probe: 'char[17784]'. 17688 + 96 = 17784.
 *
 * Task #109 removed select_timer_state (SelectTimerState, the last remaining
 * user of src/sf33rd/Source/Game/select_timer.{c,h}). Upstream 33dfd75b
 * (#216) had already moved the opponent-select countdown into effect A5 and
 * deleted both the module and this GameState member; our a752e2ca omnibus
 * squash re-added the module without any call site, so the member was saved
 * and loaded as permanent zeros on every rollback frame. Removing it
 * re-converges us with upstream. On ARM32 the member is 12 bytes
 * (bool + 2 x s32, 4-byte aligned) sitting at offset 12, preceded by one
 * pad byte after hoji_counter@10; dropping it lets u8 Order[148] start at
 * offset 11, so 13 bytes go away and 1 byte of trailing realignment comes
 * back -- net -12, which is why neither "-12 payload" nor "-13 including
 * the pad byte" is the right answer on its own.
 * Re-measured, not computed, with the same clang
 * --target=arm-linux-gnueabihf redeclaration probe (`extern char
 * probe[sizeof(GameState)]` vs `extern char probe[1]`), which reported
 * 'char[17772]' after the change and reproduced 'char[17784]' on the
 * pre-change header, validating the environment. 17784 - 12 = 17772. */
#define EXPECTED_GAME_STATE_SIZE 17772
#define EXPECTED_TASK_SIZE 16

_Static_assert(sizeof(GameState) == EXPECTED_GAME_STATE_SIZE,
               "sizeof(GameState) changed! Did you add/remove a field in game_state.h? "
               "Update GS_SAVE/GS_LOAD in this file, then set EXPECTED_GAME_STATE_SIZE "
               "to the new sizeof(GameState).");

// Guard the task struct layout specifically — task[11] is saved/loaded wholesale
// via GS_SAVE(task)/GS_LOAD(task), so any size change causes silent corruption.
_Static_assert(sizeof(struct _TASK) == EXPECTED_TASK_SIZE,
               "sizeof(struct _TASK) changed! This struct is saved/loaded wholesale "
               "during netplay rollback. DO NOT change its layout without updating "
               "GameState and verifying rollback compatibility.");
#else
// 64-bit build: tripwires are disabled because cross-arch determinism is
// unsupported (see docs/research-3sxtra-netplay-port.md §9.7 and the
// cross-arch research agent report). A MiSTer (32-bit) peer and a desktop
// (64-bit) peer will desync on GekkoNet's SessionHealthMsg checksum within
// seconds regardless of struct layout, so pinning the 64-bit expected size
// is not a meaningful correctness check. Desktop builds exist for local
// testing of the orchestrator/transport only.
#endif

// Copies are sized by the global, so a mismatched field would spill into its
// neighbours (this bit netplay before: Present_Mode's global was 4 bytes,
// the struct field was u8, and the memcpy silently overran into the next
// two fields). Ported from upstream #298.
#define GS_ASSERT_SAME_SIZE(member)                                                                                    \
    _Static_assert(sizeof(((GameState*)0)->member) == sizeof(member), #member " does not match its global")

#define GS_SAVE(member)                                                                                                 \
    GS_ASSERT_SAME_SIZE(member);                                                                                        \
    SDL_memcpy(&dst->member, &member, sizeof(member))

/* ColorRAM rows touched by Makoto's SA buff effect (effect/effl8.c). These are
 * NOT restated here: every row index below expands from the same
 * EFFL8_STEP_ROW/EFFL8_MOVE_ROW macros that effect_L8_move() itself uses to
 * build step_xy_table/move_xy_table (see effl8.h), so editing the effect's
 * addressing moves this save window with it. effl8.c additionally asserts at
 * runtime that the pointers it computed really are those rows, which catches
 * an edit that bypasses the macros. Concretely the expansion is:
 *   EFFL8_STEP_ROW(0)=0, EFFL8_MOVE_ROW(0)=8,
 *   EFFL8_STEP_ROW(1)=16, EFFL8_MOVE_ROW(1)=24
 * effl8 reads EFFL8_COLOR_ENTRIES entries from step_xy_table
 * (save_old_color_data) and writes the same count to BOTH tables
 * (get_new_color_data_L8, load_old_color_data), so the full mutated window is
 * exactly these four rows' first EFFL8_COLOR_ENTRIES u16 — the whole point of
 * pinning it here is that ColorRAM is otherwise outside the save set. */
#define EFFL8_COLORRAM_ROW_LIST                                                                                        \
    EFFL8_STEP_ROW(0), EFFL8_MOVE_ROW(0), EFFL8_STEP_ROW(1), EFFL8_MOVE_ROW(1)
#define EFFL8_COLORRAM_ROW_COUNT (EFFL8_MASTER_IDS * 2)
static const int EFFL8_COLORRAM_ROWS[EFFL8_COLORRAM_ROW_COUNT] = { EFFL8_COLORRAM_ROW_LIST };
_Static_assert(sizeof(((GameState*)0)->effl8_colorram) / sizeof(((GameState*)0)->effl8_colorram[0]) ==
                   EFFL8_COLORRAM_ROW_COUNT,
               "effl8_colorram row count must match EFFL8_COLORRAM_ROWS");
_Static_assert(sizeof(((GameState*)0)->effl8_colorram[0]) <= sizeof(ColorRAM[0]),
               "effl8_colorram row slice must fit inside a ColorRAM row");
/* The slice width is effl8's own loop bound, not an independent constant. */
_Static_assert(sizeof(((GameState*)0)->effl8_colorram[0]) / sizeof(((GameState*)0)->effl8_colorram[0][0]) ==
                   EFFL8_COLOR_ENTRIES,
               "effl8_colorram row slice must be exactly EFFL8_COLOR_ENTRIES wide");
/* Bound every row index against ColorRAM's real first dimension. Without this
 * a typo'd row is an out-of-bounds SDL_memcpy on a 64 KB global rather than a
 * build failure. Checked on the macro expansions, which is what initialises
 * EFFL8_COLORRAM_ROWS, so the array cannot hold an unchecked value. */
#define EFFL8_ROW_IN_RANGE(row) ((row) >= 0 && (row) < EFFL8_COLORRAM_TOTAL_ROWS)
_Static_assert(EFFL8_ROW_IN_RANGE(EFFL8_STEP_ROW(0)) && EFFL8_ROW_IN_RANGE(EFFL8_MOVE_ROW(0)) &&
                   EFFL8_ROW_IN_RANGE(EFFL8_STEP_ROW(1)) && EFFL8_ROW_IN_RANGE(EFFL8_MOVE_ROW(1)),
               "EFFL8 ColorRAM row index is outside ColorRAM");

void GameState_Save(GameState* dst) {
    if (!dst)
        return;
    GS_SAVE(Scene_Cut);
    GS_SAVE(Time_Over);
    GS_SAVE(round_timer);
    GS_SAVE(flash_timer);
    GS_SAVE(flash_r_num);
    GS_SAVE(flash_col);
    GS_SAVE(math_counter_hi);
    GS_SAVE(math_counter_low);
    GS_SAVE(counter_color);
    GS_SAVE(mugen_flag);
    GS_SAVE(hoji_counter);
    GS_SAVE(Order);
    GS_SAVE(Order_Timer);
    GS_SAVE(Order_Dir);
    GS_SAVE(Score);
    GS_SAVE(Complete_Bonus);
    GS_SAVE(Stock_Score);
    GS_SAVE(Vital_Bonus);
    GS_SAVE(Time_Bonus);
    GS_SAVE(Stage_Stock_Score);
    GS_SAVE(Bonus_Score);
    GS_SAVE(Final_Bonus_Score);
    GS_SAVE(WGJ_Score);
    GS_SAVE(Bonus_Score_Plus);
    GS_SAVE(Perfect_Bonus);
    GS_SAVE(Keep_Score);
    GS_SAVE(Disp_Score_Buff);
    GS_SAVE(Winner_id);
    GS_SAVE(Loser_id);
    GS_SAVE(Break_Into);
    GS_SAVE(My_char);
    GS_SAVE(Allow_a_battle_f);
    GS_SAVE(Round_num);
    GS_SAVE(Complete_Judgement);
    GS_SAVE(Fade_Flag);
    GS_SAVE(Super_Arts);
    GS_SAVE(Forbid_Break);
    GS_SAVE(Request_Break);
    GS_SAVE(Continue_Count);
    GS_SAVE(Counter_hi);
    GS_SAVE(Counter_low);
    GS_SAVE(Unit_Of_Timer);
    GS_SAVE(Select_Timer);
    GS_SAVE(Cursor_X);
    GS_SAVE(Cursor_Y);
    GS_SAVE(Cursor_Y_Pos);
    GS_SAVE(Cursor_Timer);
    GS_SAVE(Time_Stop);
    GS_SAVE(Suicide);
    GS_SAVE(Complete_Face);
    GS_SAVE(Play_Type);
    GS_SAVE(Sel_PL_Complete);
    GS_SAVE(New_Challenger);
    GS_SAVE(S_No);
    GS_SAVE(Select_Start);
    GS_SAVE(request_message);
    GS_SAVE(judge_flag);
    GS_SAVE(WINNER);
    GS_SAVE(LOSER);
    GS_SAVE(Champion);
    GS_SAVE(Fade_Half_Flag);
    GS_SAVE(Reserve_Cut);
    GS_SAVE(Perfect_Flag);
    GS_SAVE(Next_Step);
    GS_SAVE(Switch_Type);
    GS_SAVE(Cover_Timer);
    GS_SAVE(Personal_Timer);
    GS_SAVE(Request_E_No);
    GS_SAVE(Request_G_No);
    GS_SAVE(Present_Rank);
    GS_SAVE(Best_Grade);
    GS_SAVE(Demo_Type);
    GS_SAVE(Rank_Type);
    GS_SAVE(Flash_Sign);
    GS_SAVE(Flash_Rank_Time);
    GS_SAVE(Flash_Rank_Interval);
    GS_SAVE(Ranking_X);
    GS_SAVE(Rank);
    GS_SAVE(Rank_X);
    GS_SAVE(E_07_Flag);
    GS_SAVE(Complete_Victory);
    GS_SAVE(Demo_Flag);
    GS_SAVE(Next_Demo);
    GS_SAVE(Demo_PL_Index);
    GS_SAVE(Demo_Stage_Index);
    GS_SAVE(Face_MV_Request);
    GS_SAVE(Face_Move);
    GS_SAVE(Player_id);
    GS_SAVE(Last_Player_id);
    GS_SAVE(Player_Number);
    GS_SAVE(DENJIN_Term);
    GS_SAVE(Rapid_No);
    GS_SAVE(COM_id);
    GS_SAVE(EM_id);
    GS_SAVE(Select_Status);
    GS_SAVE(Select_Demo_Index);
    GS_SAVE(Country);
    GS_SAVE(Demo_Time_Stop);
    GS_SAVE(Combo_Speed);
    GS_SAVE(Exec_Wipe);
    GS_SAVE(Passive_Mode);
    GS_SAVE(Passive_Flag);
    GS_SAVE(Flip_Flag);
    GS_SAVE(Lie_Flag);
    GS_SAVE(Counter_Attack);
    GS_SAVE(Attack_Flag);
    GS_SAVE(Limited_Flag);
    GS_SAVE(Shell_Ignore_Timer);
    GS_SAVE(Event_Judge_Gals);
    GS_SAVE(EJG_index);
    GS_SAVE(Guard_Flag);
    GS_SAVE(Pierce_Menu);
    GS_SAVE(Face_MV_Time);
    GS_SAVE(Before_Jump);
    GS_SAVE(Stop_Combo);
    GS_SAVE(Stock_Hit_Flag);
    GS_SAVE(Rolling_Flag);
    GS_SAVE(Continue_Coin);
    GS_SAVE(Ignore_Entry);
    GS_SAVE(Slide_Type);
    GS_SAVE(Moving_Plate);
    GS_SAVE(Naming_Cut);
    GS_SAVE(Moving_Plate_Counter);
    GS_SAVE(Player_Color);
    GS_SAVE(PP_Priority);
    GS_SAVE(OK_Priority);
    GS_SAVE(Stock_My_char);
    GS_SAVE(Stock_Player_Color);
    GS_SAVE(Music_Fade);
    GS_SAVE(Stop_SG);
    GS_SAVE(Operator_Status);
    GS_SAVE(Round_Operator);
    GS_SAVE(another_bg);
    GS_SAVE(Last_Super_Arts);
    GS_SAVE(Last_My_char);
    GS_SAVE(Continue_Menu);
    GS_SAVE(Timer_Freeze);
    GS_SAVE(Type_of_Attack);
    GS_SAVE(Standing_Timer);
    GS_SAVE(Before_Look);
    GS_SAVE(Attack_Count_No0);
    GS_SAVE(Standing_Master_Timer);
    GS_SAVE(PB_Music_Off);
    GS_SAVE(No_Death);
    GS_SAVE(Flash_MT);
    GS_SAVE(Squat_Timer);
    GS_SAVE(Squat_Master_Timer);
    GS_SAVE(Turn_Over);
    GS_SAVE(Turn_Over_Timer);
    GS_SAVE(Jump_Pass_Timer);
    GS_SAVE(sa_gauge_flash);
    GS_SAVE(Receive_Flag);
    GS_SAVE(Disposal_Again);
    GS_SAVE(BGM_Vol);
    GS_SAVE(Used_char);
    GS_SAVE(Break_Com);
    GS_SAVE(aiuchi_flag);
    GS_SAVE(paring_counter);
    GS_SAVE(paring_bonus_r);
    GS_SAVE(paring_ctr_vs);
    GS_SAVE(paring_ctr_ori);
    GS_SAVE(Attack_Count_Buff);
    GS_SAVE(Attack_Count_Index);
    GS_SAVE(CC_Value);
    GS_SAVE(Continue_Coin2);
    GS_SAVE(Weak_PL);
    GS_SAVE(Bullet_No);
    GS_SAVE(Bullet_Counter);
    GS_SAVE(Final_Result_id);
    GS_SAVE(Disp_Win_Name);
    GS_SAVE(Perfect_Counter);
    GS_SAVE(Straight_Counter);
    GS_SAVE(Appear_Q);
    GS_SAVE(Cut_Scroll);
    GS_SAVE(Break_Into_CPU);
    GS_SAVE(ID_of_Face);
    GS_SAVE(Cursor_Move);
    GS_SAVE(Auto_Cursor);
    GS_SAVE(Auto_No);
    GS_SAVE(Auto_Index);
    GS_SAVE(Auto_Timer);
    GS_SAVE(Explosion);
    GS_SAVE(Introduce_Break_Into);
    GS_SAVE(gouki_wins);
    GS_SAVE(EM_Rank);
    GS_SAVE(Disp_PERFECT);
    GS_SAVE(Escape_SS);
    GS_SAVE(Deley_Shot_No);
    GS_SAVE(Deley_Shot_Timer);
    GS_SAVE(Lost_Round);
    GS_SAVE(Super_Arts_Finish);
    GS_SAVE(Stage_SA_Finish);
    GS_SAVE(Perfect_Finish);
    GS_SAVE(Cheap_Finish);
    GS_SAVE(Last_My_char2);
    GS_SAVE(gouki_app);
    GS_SAVE(Bonus_Game_Complete);
    GS_SAVE(Get_Demo_Index);
    GS_SAVE(Combo_Demo_Flag);
    GS_SAVE(Stage_Continue);
    GS_SAVE(Pause_Hit_Marks);
    GS_SAVE(Extra_Break);
    GS_SAVE(Shin_Gouki_BGM);
    GS_SAVE(Stage_Lost_Round);
    GS_SAVE(Stage_Perfect_Finish);
    GS_SAVE(Stage_Cheap_Finish);
    GS_SAVE(EXE_obroll);
    GS_SAVE(End_PL);
    GS_SAVE(Stock_Com_Arts);
    GS_SAVE(PB_Status);
    GS_SAVE(Flip_Counter);
    GS_SAVE(Stage_Time_Finish);
    GS_SAVE(Bonus_Type);
    GS_SAVE(Completion_Bonus);
    GS_SAVE(ichikannkei);
    GS_SAVE(Plate_Disposal_No);
    GS_SAVE(SO_No);
    GS_SAVE(Disp_Command_Name);
    GS_SAVE(OK_Appear79);
    GS_SAVE(Extra_Counter);
    GS_SAVE(SC_No);
    GS_SAVE(BGM_No);
    GS_SAVE(BGM_Timer);
    GS_SAVE(EM_List);
    GS_SAVE(Sel_EM_Complete);
    GS_SAVE(Temporary_EM);
    GS_SAVE(OK_Moving_SA_Plate);
    GS_SAVE(Battle_Q);
    GS_SAVE(EM_History);
    GS_SAVE(GO_No);
    GS_SAVE(Aborigine);
    GS_SAVE(Continue_Count_Down);
    GS_SAVE(WGJ_Target);
    GS_SAVE(EM_Candidate);
    GS_SAVE(Last_Selected_EM);
    GS_SAVE(Q_Country);
    GS_SAVE(Continue_Cut);
    GS_SAVE(Introduce_Boss);
    GS_SAVE(Final_Play_Type);
    GS_SAVE(Rank_In);
    GS_SAVE(Request_Disp_Rank);
    GS_SAVE(Reset_Timer);
    GS_SAVE(bbbs_type);
    GS_SAVE(Straight_Flag);
    GS_SAVE(kakushi_ix);
    GS_SAVE(kakushi_op);
    GS_SAVE(RO_backup);
    GS_SAVE(PT_backup);
    GS_SAVE(E_Number);
    GS_SAVE(E_No);
    GS_SAVE(C_No);
    GS_SAVE(G_No);
    GS_SAVE(D_No);
    GS_SAVE(M_No);
    GS_SAVE(Exit_No);
    GS_SAVE(SP_No);
    GS_SAVE(Face_No);
    GS_SAVE(Stop_Cursor);
    GS_SAVE(Training_Index);
    GS_SAVE(Connect_Status);
    GS_SAVE(Menu_Suicide);
    GS_SAVE(Game_pause);
    GS_SAVE(Game_difficulty);
    GS_SAVE(Pause);
    GS_SAVE(Pause_ID);
    GS_SAVE(Exit_Menu);
    GS_SAVE(Conclusion_Flag);
    GS_SAVE(CP_No);
    GS_SAVE(CP_Index);
    GS_SAVE(Gap_Timer);
    GS_SAVE(Message_Suicide);
    GS_SAVE(Disp_Cockpit);
    GS_SAVE(Select_Arts);
    GS_SAVE(Lamp_No);
    GS_SAVE(Lamp_Index);
    GS_SAVE(Lamp_Color);
    GS_SAVE(Stop_Update_Score);
    GS_SAVE(test_flag);
    GS_SAVE(ixbfw_cut);
    GS_SAVE(Cont_No);
    GS_SAVE(PL_Wins);
    GS_SAVE(Fade_R_No0);
    GS_SAVE(Fade_R_No1);
    GS_SAVE(Conclusion_Type);
    GS_SAVE(win_type);
    GS_SAVE(message_index);
    GS_SAVE(F_No0);
    GS_SAVE(F_No1);
    GS_SAVE(F_No2);
    GS_SAVE(F_No3);
    GS_SAVE(keep_condition);
    GS_SAVE(Check_Buff);
    GS_SAVE(Convert_Buff);
    GS_SAVE(Unsubstantial_BG);
    GS_SAVE(Menu_Cursor_X);
    GS_SAVE(Menu_Cursor_Y);
    GS_SAVE(Replay_Status);
    GS_SAVE(Disappear_LOGO);
    GS_SAVE(count_end);
    GS_SAVE(Play_Game);
    GS_SAVE(Menu_Cursor_Move);
    GS_SAVE(flash_win_type);
    GS_SAVE(sync_win_type);
    GS_SAVE(Mode_Type);
    GS_SAVE(Menu_Page);
    GS_SAVE(Menu_Max);
    GS_SAVE(reset_NG_flag);
    GS_SAVE(VS_Stage);
    GS_SAVE(Present_Mode);
    GS_SAVE(Play_Mode);
    GS_SAVE(Page_Max);
    GS_SAVE(Direction_Working);
    GS_SAVE(Vital_Handicap);
    GS_SAVE(Cursor_Limit);
    GS_SAVE(Synchro_No);
    GS_SAVE(SA_shadow_on);
    GS_SAVE(Pause_Down);
    GS_SAVE(Training_ID);
    GS_SAVE(Disp_Attack_Data);
    GS_SAVE(Disp_Input_History);
    GS_SAVE(Disp_Frame_Data);
    GS_SAVE(Record_Data_Tr);
    GS_SAVE(End_Training);
    GS_SAVE(Menu_Page_Buff);
    GS_SAVE(Reset_Bootrom);
    GS_SAVE(Decide_ID);
    GS_SAVE(Training_Cursor);
    GS_SAVE(Lag_Timer);
    GS_SAVE(CPU_Time_Lag);
    GS_SAVE(Forbid_Reset);
    GS_SAVE(CPU_Rec);
    GS_SAVE(Pause_Type);
    GS_SAVE(Game_timer);
    GS_SAVE(Control_Time);
    GS_SAVE(Time_in_Time);
    GS_SAVE(Round_Level);
    GS_SAVE(Round_Result);
    GS_SAVE(Fade_Number);
    GS_SAVE(G_Timer);
    GS_SAVE(D_Timer);
    GS_SAVE(Rank_Pos_X);
    GS_SAVE(Rank_Pos_Y);
    GS_SAVE(E_Timer);
    GS_SAVE(F_Timer);
    GS_SAVE(ENTRY_X);
    GS_SAVE(C_Timer);
    GS_SAVE(S_Timer);
    GS_SAVE(Flash_Complete);
    GS_SAVE(Sel_Arts_Complete);
    GS_SAVE(Arts_Y);
    GS_SAVE(Move_Super_Arts);
    GS_SAVE(Battle_Country);
    GS_SAVE(Face_Status);
    GS_SAVE(ID);
    GS_SAVE(ID2);
    GS_SAVE(mes_already);
    GS_SAVE(Timer_00);
    GS_SAVE(Timer_01);
    GS_SAVE(PL_Distance);
    GS_SAVE(Area_Number);
    GS_SAVE(Lever_Buff);
    GS_SAVE(Lever_Pool);
    GS_SAVE(Tech_Index);
    GS_SAVE(Random_ix16);
    GS_SAVE(Random_ix32);
    GS_SAVE(M_Timer);
    GS_SAVE(VS_Tech);
    GS_SAVE(Guard_Type);
    GS_SAVE(Separate_Area);
    GS_SAVE(Free_Lever);
    GS_SAVE(Term_No);
    GS_SAVE(Com_Width_Data);
    GS_SAVE(Lever_Squat);
    GS_SAVE(M_Lv);
    GS_SAVE(Insert_Y);
    GS_SAVE(scr_req_x);
    GS_SAVE(scr_req_y);
    GS_SAVE(zoom_req_flag_old);
    GS_SAVE(zoom_request_flag);
    GS_SAVE(zoom_request_level);
    GS_SAVE(Last_Selected_ID);
    GS_SAVE(Last_Called_SE);
    GS_SAVE(VS_Index);
    GS_SAVE(Rapid_Index);
    GS_SAVE(Shell_Separate_Area);
    GS_SAVE(Attack_Counter);
    GS_SAVE(Last_Attack_Counter);
    GS_SAVE(Pattern_Index);
    GS_SAVE(Com_Color_Shot);
    GS_SAVE(Resume_Lever);
    GS_SAVE(players_timer);
    GS_SAVE(Lever_Store);
    GS_SAVE(Return_CP_No);
    GS_SAVE(Return_CP_Index);
    GS_SAVE(Return_Pattern_Index);
    GS_SAVE(Lever_LR);
    GS_SAVE(Last_Eftype);
    GS_SAVE(DENJIN_No);
    GS_SAVE(SC_Personal_Time);
    GS_SAVE(Guard_Counter);
    GS_SAVE(Limit_Time);
    GS_SAVE(Last_Pattern_Index);
    GS_SAVE(Random_ix16_ex);
    GS_SAVE(Random_ix32_ex);
    GS_SAVE(DE_X);
    GS_SAVE(Exit_Timer);
    GS_SAVE(Max_vitality);
    GS_SAVE(Bonus_Game_Flag);
    GS_SAVE(Bonus_Game_Work);
    GS_SAVE(Bonus_Game_result);
    GS_SAVE(Stock_Bonus_Game_Result);
    GS_SAVE(bs_scrrrl);
    GS_SAVE(Bonus_Stage_RNO);
    GS_SAVE(Bonus_Stage_Level);
    GS_SAVE(Bonus_Stage_Tix);
    GS_SAVE(Bonus_Game_ex_result);
    GS_SAVE(Stock_Com_Color);
    GS_SAVE(bs2_floor);
    GS_SAVE(bs2_hosei);
    GS_SAVE(bs2_current_damage);
    GS_SAVE(Win_Record);
    GS_SAVE(Stock_Win_Record);
    GS_SAVE(WGJ_Win);
    GS_SAVE(Target_BG_X);
    GS_SAVE(Offset_BG_X);
    GS_SAVE(Result_Timer);
    GS_SAVE(scrl);
    GS_SAVE(scrr);
    GS_SAVE(vital_stop_flag);
    GS_SAVE(gauge_stop_flag);
    GS_SAVE(Lamp_Timer);
    GS_SAVE(Cont_Timer);
    GS_SAVE(Plate_X);
    GS_SAVE(Plate_Y);
    GS_SAVE(Demo_Timer);
    GS_SAVE(Condense_Buff);
    GS_SAVE(Demo_Ptr);
    GS_SAVE(Keep_Grade);
    GS_SAVE(IO_Result);
    GS_SAVE(VS_Win_Record);
    GS_SAVE(PLsw);
    GS_SAVE(plsw_00);
    GS_SAVE(plsw_01);
    GS_SAVE(Flash_Synchro);
    GS_SAVE(Synchro_Level);
    GS_SAVE(Random_ix16_com);
    GS_SAVE(Random_ix32_com);
    GS_SAVE(Random_ix16_ex_com);
    GS_SAVE(Random_ix32_ex_com);
    /* Random_ix16_bg IS saved. The upstream #298 port (e57b16bd) dropped it
     * with the rationale "it only drives stage flashing, and the state
     * deciding when to draw from it isn't saved" — the second clause is
     * factually wrong on this fork. random_16_bg() (pls02.c:734-743) is
     * called only from scr_trans() (stage/bg.c:811, 948, 949), and every
     * field that call site writes from the result — stage_flash,
     * stage_ftimer, rw_dat[0] (rw_cnt/rwd_ptr/brw_ptr) and rw3col_ptr — is
     * GS_SAVEd right below in the `bg` block. Since step_game() runs
     * njUserMain() unconditionally on rolled-back frames, an unsaved index
     * advances while its saved consumers are rewound, so the saved BG-flash
     * state drifts from the no-rollback timeline. Confirmed empirically by
     * the rollback-determinism harness (makoto-sa3-super, stage 3): the
     * index plus rw_dat/stage_flash/stage_ftimer all went DIVERGENT(+
     * FEEDBACK) from frames 347-349 and stayed divergent to the end of the
     * run. Severity is cosmetic cross-peer (none of these four are in the
     * SessionHealthMsg checksum), but it is a real rollback divergence —
     * save the index. */
    GS_SAVE(Random_ix16_bg);
    GS_SAVE(Opening_Now);
    GS_SAVE(task);

    // plcnt

    GS_SAVE(plw);
    GS_SAVE(combo_type);
    GS_SAVE(remake_power);
    GS_SAVE(zanzou_table);
    GS_SAVE(super_arts);
    GS_SAVE(piyori_type);
    GS_SAVE(appear_type);
    GS_SAVE(pcon_rno);
    GS_SAVE(round_slow_flag);
    GS_SAVE(pcon_dp_flag);
    GS_SAVE(win_sp_flag);
    GS_SAVE(dead_voice_flag);
    GS_SAVE(rambod);
    GS_SAVE(ramhan);
    GS_SAVE(vital_inc_timer);
    GS_SAVE(vital_dec_timer);
    GS_SAVE(sag_inc_timer);

    // cmd_data

    GS_SAVE(wcp);
    GS_SAVE(t_pl_lvr);
    GS_SAVE(waza_work);

    // cmb_win

    GS_SAVE(cmst_buff);
    GS_SAVE(old_cmb_flag);
    GS_SAVE(cmb_stock);
    GS_SAVE(first_attack);
    GS_SAVE(rever_attack);
    GS_SAVE(paring_attack);
    GS_SAVE(bonus_pts);
    GS_SAVE(hit_num);
    GS_SAVE(sa_kind);
    GS_SAVE(end_flag);
    GS_SAVE(calc_hit);
    GS_SAVE(score_calc);
    GS_SAVE(cmb_all_stock);
    GS_SAVE(sarts_finish_flag);
    GS_SAVE(last_hit_time);
    GS_SAVE(cmb_calc_now);
    GS_SAVE(cst_read);
    GS_SAVE(cst_write);

    // bg

    GS_SAVE(bg_w);
    GS_SAVE(Screen_Switch);
    GS_SAVE(Screen_Switch_Buffer);
    GS_SAVE(rw_num);
    GS_SAVE(rw_bg_flag);
    GS_SAVE(tokusyu_stage);
    GS_SAVE(rw_gbix);
    GS_SAVE(stage_flash);
    GS_SAVE(stage_ftimer);
    GS_SAVE(yang_ix_plus);
    GS_SAVE(yang_ix);
    GS_SAVE(yang_timer);
    GS_SAVE(ending_flag);
    GS_SAVE(end_prm);
    GS_SAVE(gouki_end_gbix);
    GS_SAVE(rw3col_ptr);
    GS_SAVE(bg_disp_off);
    GS_SAVE(bgPalCodeOffset);
    GS_SAVE(rw_dat);

    // charset

    GS_SAVE(att_req);

    // slowf

    GS_SAVE(SLOW_timer);
    GS_SAVE(SLOW_flag);
    GS_SAVE(EXE_flag);

    // grade

    GS_SAVE(judge_gals);
    GS_SAVE(judge_com);
    GS_SAVE(last_judge_dada);
    GS_SAVE(judge_final);
    GS_SAVE(judge_item);
    GS_SAVE(ji_sat);

    // spgauge

    GS_SAVE(Old_Stop_SG);
    GS_SAVE(Exec_Wipe_F);
    GS_SAVE(time_clear);
    GS_SAVE(spg_number);
    GS_SAVE(spg_work);
    GS_SAVE(spg_offset);
    GS_SAVE(time_num);
    GS_SAVE(time_timer);
    GS_SAVE(time_flag);
    GS_SAVE(col);
    GS_SAVE(time_operate);
    GS_SAVE(sast_now);
    GS_SAVE(max2);
    GS_SAVE(max_rno2);
    GS_SAVE(spg_dat);

    // stun

    GS_SAVE(sdat);

    // vital

    GS_SAVE(vit);

    // win_pl

    GS_SAVE(win_free);
    GS_SAVE(win_rno);
    GS_SAVE(poison_flag);

    // ta_sub

    GS_SAVE(eff_hit_flag);

    // sc_sub

    GS_SAVE(FadeLimit);
    GS_SAVE(WipeLimit);

    // appear

    GS_SAVE(Appear_car_stop);
    GS_SAVE(Appear_hv);
    GS_SAVE(Appear_free);
    GS_SAVE(Appear_flag);
    GS_SAVE(app_counter);
    GS_SAVE(appear_work);
    GS_SAVE(Appear_end);

    // bg_data

    GS_SAVE(y_sitei_pos);
    GS_SAVE(y_sitei_flag);
    GS_SAVE(c_number);
    GS_SAVE(c_kakikae);
    GS_SAVE(g_number);
    GS_SAVE(g_kakikae);
    GS_SAVE(nosekae);
    GS_SAVE(scrn_adgjust_y);
    GS_SAVE(scrn_adgjust_x);
    GS_SAVE(zoom_add);
    GS_SAVE(ls_cnt1);
    GS_SAVE(bg_app);
    GS_SAVE(sa_pa_flag);
    GS_SAVE(aku_flag);
    GS_SAVE(seraph_flag);
    GS_SAVE(akebono_flag);
    GS_SAVE(bg_mvxy);
    GS_SAVE(chase_time_y);
    GS_SAVE(chase_time_x);
    GS_SAVE(chase_y);
    GS_SAVE(chase_x);
    GS_SAVE(demo_car_flag);
    GS_SAVE(ideal_w);
    GS_SAVE(bg_app_stop);
    GS_SAVE(bg_stop);
    GS_SAVE(base_y_pos);
    GS_SAVE(etcBgPalCnvTable);
    GS_SAVE(etcBgGixCnvTable);

    // eff56

    GS_SAVE(ci_pointer);
    GS_SAVE(ci_col);
    GS_SAVE(ci_timer);

    // effb2

    GS_SAVE(rf_b2_flag);
    GS_SAVE(b2_curr_no);

    // effb8

    GS_SAVE(test_pl_no);
    GS_SAVE(test_mes_no);
    GS_SAVE(test_in);
    GS_SAVE(old_mes_no2);
    GS_SAVE(old_mes_no3);
    GS_SAVE(old_mes_no_pl);
    GS_SAVE(mes_timer);

    // work_sys — rollback-critical system globals

    GS_SAVE(bg_pos);
    GS_SAVE(fm_pos);
    GS_SAVE(bg_prm);
    GS_SAVE(system_timer);
    GS_SAVE(Gill_Appear_Flag);

    // plcnt — DIP switch combat config

    GS_SAVE(cmd_sel);
    GS_SAVE(no_sa);

    // sc_sub

    GS_SAVE(Hnc_Num);

    // ending

    GS_SAVE(end_w);

    // work_sys (extension)

    GS_SAVE(scr_sc);
    GS_SAVE(X_Adjust);
    GS_SAVE(Y_Adjust);

    // Additional globals

    GS_SAVE(BgMATRIX);
    GS_SAVE(vm_w);
    GS_SAVE(ck_ex_option);
    GS_SAVE(X_Adjust_Buff);
    GS_SAVE(Y_Adjust_Buff);

    /* chainex_check is an extern defined in sysdir.c. The GS_SAVE macro
     * uses the bare name, so we declare it visible here. */
    {
        extern u8 chainex_check[2][36];
        SDL_memcpy(&dst->chainex_check, chainex_check, sizeof(chainex_check));
    }

    /* Color7 (char-select color chord) — extern defined in screen/sel_pl.c.
     * ca_check_flag (battle throw/CA gate) — extern in engine/hitcheck.c.
     * See the GameState struct comments for the rollback rationale.
     * Ported from d5f301cc. */
    {
        extern u16 Color7[2];
        SDL_memcpy(&dst->Color7, Color7, sizeof(Color7));
    }
    {
        extern s8 ca_check_flag;
        SDL_memcpy(&dst->ca_check_flag, &ca_check_flag, sizeof(ca_check_flag));
    }

    /* spmv_ng_save (Makoto SA-buff backup of PLW.spmv_ng_flag) — extern
     * defined in effect/effl8.c. See the GameState struct comment for the
     * rollback rationale and the corrected Candidate-0b history. */
    {
        extern u32 spmv_ng_save[2];
        SDL_memcpy(&dst->spmv_ng_save, spmv_ng_save, sizeof(spmv_ng_save));
    }

    /* effl8_colorram — the ColorRAM rows Makoto's SA buff effect latches from
     * and overwrites. See the GameState struct comment for the full rationale
     * and EFFL8_COLORRAM_ROWS for how the rows are derived from effl8.c. */
    for (int i = 0; i < EFFL8_COLORRAM_ROW_COUNT; i++) {
        SDL_memcpy(dst->effl8_colorram[i], ColorRAM[EFFL8_COLORRAM_ROWS[i]], sizeof(dst->effl8_colorram[i]));
    }
}

#define GS_LOAD(member)                                                                                                 \
    GS_ASSERT_SAME_SIZE(member);                                                                                        \
    SDL_memcpy(&member, &src->member, sizeof(member))

void GameState_Load(const GameState* src) {
    if (!src)
        return;
    GS_LOAD(Scene_Cut);
    GS_LOAD(Time_Over);
    GS_LOAD(round_timer);
    GS_LOAD(flash_timer);
    GS_LOAD(flash_r_num);
    GS_LOAD(flash_col);
    GS_LOAD(math_counter_hi);
    GS_LOAD(math_counter_low);
    GS_LOAD(counter_color);
    GS_LOAD(mugen_flag);
    GS_LOAD(hoji_counter);
    GS_LOAD(Order);
    GS_LOAD(Order_Timer);
    GS_LOAD(Order_Dir);
    GS_LOAD(Score);
    GS_LOAD(Complete_Bonus);
    GS_LOAD(Stock_Score);
    GS_LOAD(Vital_Bonus);
    GS_LOAD(Time_Bonus);
    GS_LOAD(Stage_Stock_Score);
    GS_LOAD(Bonus_Score);
    GS_LOAD(Final_Bonus_Score);
    GS_LOAD(WGJ_Score);
    GS_LOAD(Bonus_Score_Plus);
    GS_LOAD(Perfect_Bonus);
    GS_LOAD(Keep_Score);
    GS_LOAD(Disp_Score_Buff);
    GS_LOAD(Winner_id);
    GS_LOAD(Loser_id);
    GS_LOAD(Break_Into);
    GS_LOAD(My_char);
    GS_LOAD(Allow_a_battle_f);
    GS_LOAD(Round_num);
    GS_LOAD(Complete_Judgement);
    GS_LOAD(Fade_Flag);
    GS_LOAD(Super_Arts);
    GS_LOAD(Forbid_Break);
    GS_LOAD(Request_Break);
    GS_LOAD(Continue_Count);
    GS_LOAD(Counter_hi);
    GS_LOAD(Counter_low);
    GS_LOAD(Unit_Of_Timer);
    GS_LOAD(Select_Timer);
    GS_LOAD(Cursor_X);
    GS_LOAD(Cursor_Y);
    GS_LOAD(Cursor_Y_Pos);
    GS_LOAD(Cursor_Timer);
    GS_LOAD(Time_Stop);
    GS_LOAD(Suicide);
    GS_LOAD(Complete_Face);
    GS_LOAD(Play_Type);
    GS_LOAD(Sel_PL_Complete);
    GS_LOAD(New_Challenger);
    GS_LOAD(S_No);
    GS_LOAD(Select_Start);
    GS_LOAD(request_message);
    GS_LOAD(judge_flag);
    GS_LOAD(WINNER);
    GS_LOAD(LOSER);
    GS_LOAD(Champion);
    GS_LOAD(Fade_Half_Flag);
    GS_LOAD(Reserve_Cut);
    GS_LOAD(Perfect_Flag);
    GS_LOAD(Next_Step);
    GS_LOAD(Switch_Type);
    GS_LOAD(Cover_Timer);
    GS_LOAD(Personal_Timer);
    GS_LOAD(Request_E_No);
    GS_LOAD(Request_G_No);
    GS_LOAD(Present_Rank);
    GS_LOAD(Best_Grade);
    GS_LOAD(Demo_Type);
    GS_LOAD(Rank_Type);
    GS_LOAD(Flash_Sign);
    GS_LOAD(Flash_Rank_Time);
    GS_LOAD(Flash_Rank_Interval);
    GS_LOAD(Ranking_X);
    GS_LOAD(Rank);
    GS_LOAD(Rank_X);
    GS_LOAD(E_07_Flag);
    GS_LOAD(Complete_Victory);
    GS_LOAD(Demo_Flag);
    GS_LOAD(Next_Demo);
    GS_LOAD(Demo_PL_Index);
    GS_LOAD(Demo_Stage_Index);
    GS_LOAD(Face_MV_Request);
    GS_LOAD(Face_Move);
    GS_LOAD(Player_id);
    GS_LOAD(Last_Player_id);
    GS_LOAD(Player_Number);
    GS_LOAD(DENJIN_Term);
    GS_LOAD(Rapid_No);
    GS_LOAD(COM_id);
    GS_LOAD(EM_id);
    GS_LOAD(Select_Status);
    GS_LOAD(Select_Demo_Index);
    GS_LOAD(Country);
    GS_LOAD(Demo_Time_Stop);
    GS_LOAD(Combo_Speed);
    GS_LOAD(Exec_Wipe);
    GS_LOAD(Passive_Mode);
    GS_LOAD(Passive_Flag);
    GS_LOAD(Flip_Flag);
    GS_LOAD(Lie_Flag);
    GS_LOAD(Counter_Attack);
    GS_LOAD(Attack_Flag);
    GS_LOAD(Limited_Flag);
    GS_LOAD(Shell_Ignore_Timer);
    GS_LOAD(Event_Judge_Gals);
    GS_LOAD(EJG_index);
    GS_LOAD(Guard_Flag);
    GS_LOAD(Pierce_Menu);
    GS_LOAD(Face_MV_Time);
    GS_LOAD(Before_Jump);
    GS_LOAD(Stop_Combo);
    GS_LOAD(Stock_Hit_Flag);
    GS_LOAD(Rolling_Flag);
    GS_LOAD(Continue_Coin);
    GS_LOAD(Ignore_Entry);
    GS_LOAD(Slide_Type);
    GS_LOAD(Moving_Plate);
    GS_LOAD(Naming_Cut);
    GS_LOAD(Moving_Plate_Counter);
    GS_LOAD(Player_Color);
    GS_LOAD(PP_Priority);
    GS_LOAD(OK_Priority);
    GS_LOAD(Stock_My_char);
    GS_LOAD(Stock_Player_Color);
    GS_LOAD(Music_Fade);
    GS_LOAD(Stop_SG);
    GS_LOAD(Operator_Status);
    GS_LOAD(Round_Operator);
    GS_LOAD(another_bg);
    GS_LOAD(Last_Super_Arts);
    GS_LOAD(Last_My_char);
    GS_LOAD(Continue_Menu);
    GS_LOAD(Timer_Freeze);
    GS_LOAD(Type_of_Attack);
    GS_LOAD(Standing_Timer);
    GS_LOAD(Before_Look);
    GS_LOAD(Attack_Count_No0);
    GS_LOAD(Standing_Master_Timer);
    GS_LOAD(PB_Music_Off);
    GS_LOAD(No_Death);
    GS_LOAD(Flash_MT);
    GS_LOAD(Squat_Timer);
    GS_LOAD(Squat_Master_Timer);
    GS_LOAD(Turn_Over);
    GS_LOAD(Turn_Over_Timer);
    GS_LOAD(Jump_Pass_Timer);
    GS_LOAD(sa_gauge_flash);
    GS_LOAD(Receive_Flag);
    GS_LOAD(Disposal_Again);
    GS_LOAD(BGM_Vol);
    GS_LOAD(Used_char);
    GS_LOAD(Break_Com);
    GS_LOAD(aiuchi_flag);
    GS_LOAD(paring_counter);
    GS_LOAD(paring_bonus_r);
    GS_LOAD(paring_ctr_vs);
    GS_LOAD(paring_ctr_ori);
    GS_LOAD(Attack_Count_Buff);
    GS_LOAD(Attack_Count_Index);
    GS_LOAD(CC_Value);
    GS_LOAD(Continue_Coin2);
    GS_LOAD(Weak_PL);
    GS_LOAD(Bullet_No);
    GS_LOAD(Bullet_Counter);
    GS_LOAD(Final_Result_id);
    GS_LOAD(Disp_Win_Name);
    GS_LOAD(Perfect_Counter);
    GS_LOAD(Straight_Counter);
    GS_LOAD(Appear_Q);
    GS_LOAD(Cut_Scroll);
    GS_LOAD(Break_Into_CPU);
    GS_LOAD(ID_of_Face);
    GS_LOAD(Cursor_Move);
    GS_LOAD(Auto_Cursor);
    GS_LOAD(Auto_No);
    GS_LOAD(Auto_Index);
    GS_LOAD(Auto_Timer);
    GS_LOAD(Explosion);
    GS_LOAD(Introduce_Break_Into);
    GS_LOAD(gouki_wins);
    GS_LOAD(EM_Rank);
    GS_LOAD(Disp_PERFECT);
    GS_LOAD(Escape_SS);
    GS_LOAD(Deley_Shot_No);
    GS_LOAD(Deley_Shot_Timer);
    GS_LOAD(Lost_Round);
    GS_LOAD(Super_Arts_Finish);
    GS_LOAD(Stage_SA_Finish);
    GS_LOAD(Perfect_Finish);
    GS_LOAD(Cheap_Finish);
    GS_LOAD(Last_My_char2);
    GS_LOAD(gouki_app);
    GS_LOAD(Bonus_Game_Complete);
    GS_LOAD(Get_Demo_Index);
    GS_LOAD(Combo_Demo_Flag);
    GS_LOAD(Stage_Continue);
    GS_LOAD(Pause_Hit_Marks);
    GS_LOAD(Extra_Break);
    GS_LOAD(Shin_Gouki_BGM);
    GS_LOAD(Stage_Lost_Round);
    GS_LOAD(Stage_Perfect_Finish);
    GS_LOAD(Stage_Cheap_Finish);
    GS_LOAD(EXE_obroll);
    GS_LOAD(End_PL);
    GS_LOAD(Stock_Com_Arts);
    GS_LOAD(PB_Status);
    GS_LOAD(Flip_Counter);
    GS_LOAD(Stage_Time_Finish);
    GS_LOAD(Bonus_Type);
    GS_LOAD(Completion_Bonus);
    GS_LOAD(ichikannkei);
    GS_LOAD(Plate_Disposal_No);
    GS_LOAD(SO_No);
    GS_LOAD(Disp_Command_Name);
    GS_LOAD(OK_Appear79);
    GS_LOAD(Extra_Counter);
    GS_LOAD(SC_No);
    GS_LOAD(BGM_No);
    GS_LOAD(BGM_Timer);
    GS_LOAD(EM_List);
    GS_LOAD(Sel_EM_Complete);
    GS_LOAD(Temporary_EM);
    GS_LOAD(OK_Moving_SA_Plate);
    GS_LOAD(Battle_Q);
    GS_LOAD(EM_History);
    GS_LOAD(GO_No);
    GS_LOAD(Aborigine);
    GS_LOAD(Continue_Count_Down);
    GS_LOAD(WGJ_Target);
    GS_LOAD(EM_Candidate);
    GS_LOAD(Last_Selected_EM);
    GS_LOAD(Q_Country);
    GS_LOAD(Continue_Cut);
    GS_LOAD(Introduce_Boss);
    GS_LOAD(Final_Play_Type);
    GS_LOAD(Rank_In);
    GS_LOAD(Request_Disp_Rank);
    GS_LOAD(Reset_Timer);
    GS_LOAD(bbbs_type);
    GS_LOAD(Straight_Flag);
    GS_LOAD(kakushi_ix);
    GS_LOAD(kakushi_op);
    GS_LOAD(RO_backup);
    GS_LOAD(PT_backup);
    GS_LOAD(E_Number);
    GS_LOAD(E_No);
    GS_LOAD(C_No);
    GS_LOAD(G_No);
    GS_LOAD(D_No);
    GS_LOAD(M_No);
    GS_LOAD(Exit_No);
    GS_LOAD(SP_No);
    GS_LOAD(Face_No);
    GS_LOAD(Stop_Cursor);
    GS_LOAD(Training_Index);
    GS_LOAD(Connect_Status);
    GS_LOAD(Menu_Suicide);
    GS_LOAD(Game_pause);
    GS_LOAD(Game_difficulty);
    GS_LOAD(Pause);
    GS_LOAD(Pause_ID);
    GS_LOAD(Exit_Menu);
    GS_LOAD(Conclusion_Flag);
    GS_LOAD(CP_No);
    GS_LOAD(CP_Index);
    GS_LOAD(Gap_Timer);
    GS_LOAD(Message_Suicide);
    GS_LOAD(Disp_Cockpit);
    GS_LOAD(Select_Arts);
    GS_LOAD(Lamp_No);
    GS_LOAD(Lamp_Index);
    GS_LOAD(Lamp_Color);
    GS_LOAD(Stop_Update_Score);
    GS_LOAD(test_flag);
    GS_LOAD(ixbfw_cut);
    GS_LOAD(Cont_No);
    GS_LOAD(PL_Wins);
    GS_LOAD(Fade_R_No0);
    GS_LOAD(Fade_R_No1);
    GS_LOAD(Conclusion_Type);
    GS_LOAD(win_type);
    GS_LOAD(message_index);
    GS_LOAD(F_No0);
    GS_LOAD(F_No1);
    GS_LOAD(F_No2);
    GS_LOAD(F_No3);
    GS_LOAD(keep_condition);
    GS_LOAD(Check_Buff);
    GS_LOAD(Convert_Buff);
    GS_LOAD(Unsubstantial_BG);
    GS_LOAD(Menu_Cursor_X);
    GS_LOAD(Menu_Cursor_Y);
    GS_LOAD(Replay_Status);
    GS_LOAD(Disappear_LOGO);
    GS_LOAD(count_end);
    GS_LOAD(Play_Game);
    GS_LOAD(Menu_Cursor_Move);
    GS_LOAD(flash_win_type);
    GS_LOAD(sync_win_type);
    GS_LOAD(Mode_Type);
    GS_LOAD(Menu_Page);
    GS_LOAD(Menu_Max);
    GS_LOAD(reset_NG_flag);
    GS_LOAD(VS_Stage);
    GS_LOAD(Present_Mode);
    GS_LOAD(Play_Mode);
    GS_LOAD(Page_Max);
    GS_LOAD(Direction_Working);
    GS_LOAD(Vital_Handicap);
    GS_LOAD(Cursor_Limit);
    GS_LOAD(Synchro_No);
    GS_LOAD(SA_shadow_on);
    GS_LOAD(Pause_Down);
    GS_LOAD(Training_ID);
    GS_LOAD(Disp_Attack_Data);
    GS_LOAD(Disp_Input_History);
    GS_LOAD(Disp_Frame_Data);
    GS_LOAD(Record_Data_Tr);
    GS_LOAD(End_Training);
    GS_LOAD(Menu_Page_Buff);
    GS_LOAD(Reset_Bootrom);
    GS_LOAD(Decide_ID);
    GS_LOAD(Training_Cursor);
    GS_LOAD(Lag_Timer);
    GS_LOAD(CPU_Time_Lag);
    GS_LOAD(Forbid_Reset);
    GS_LOAD(CPU_Rec);
    GS_LOAD(Pause_Type);
    GS_LOAD(Game_timer);
    GS_LOAD(Control_Time);
    GS_LOAD(Time_in_Time);
    GS_LOAD(Round_Level);
    GS_LOAD(Round_Result);
    GS_LOAD(Fade_Number);
    GS_LOAD(G_Timer);
    GS_LOAD(D_Timer);
    GS_LOAD(Rank_Pos_X);
    GS_LOAD(Rank_Pos_Y);
    GS_LOAD(E_Timer);
    GS_LOAD(F_Timer);
    GS_LOAD(ENTRY_X);
    GS_LOAD(C_Timer);
    GS_LOAD(S_Timer);
    GS_LOAD(Flash_Complete);
    GS_LOAD(Sel_Arts_Complete);
    GS_LOAD(Arts_Y);
    GS_LOAD(Move_Super_Arts);
    GS_LOAD(Battle_Country);
    GS_LOAD(Face_Status);
    GS_LOAD(ID);
    GS_LOAD(ID2);
    GS_LOAD(mes_already);
    GS_LOAD(Timer_00);
    GS_LOAD(Timer_01);
    GS_LOAD(PL_Distance);
    GS_LOAD(Area_Number);
    GS_LOAD(Lever_Buff);
    GS_LOAD(Lever_Pool);
    GS_LOAD(Tech_Index);
    GS_LOAD(Random_ix16);
    GS_LOAD(Random_ix32);
    GS_LOAD(M_Timer);
    GS_LOAD(VS_Tech);
    GS_LOAD(Guard_Type);
    GS_LOAD(Separate_Area);
    GS_LOAD(Free_Lever);
    GS_LOAD(Term_No);
    GS_LOAD(Com_Width_Data);
    GS_LOAD(Lever_Squat);
    GS_LOAD(M_Lv);
    GS_LOAD(Insert_Y);
    GS_LOAD(scr_req_x);
    GS_LOAD(scr_req_y);
    GS_LOAD(zoom_req_flag_old);
    GS_LOAD(zoom_request_flag);
    GS_LOAD(zoom_request_level);
    GS_LOAD(Last_Selected_ID);
    GS_LOAD(Last_Called_SE);
    GS_LOAD(VS_Index);
    GS_LOAD(Rapid_Index);
    GS_LOAD(Shell_Separate_Area);
    GS_LOAD(Attack_Counter);
    GS_LOAD(Last_Attack_Counter);
    GS_LOAD(Pattern_Index);
    GS_LOAD(Com_Color_Shot);
    GS_LOAD(Resume_Lever);
    GS_LOAD(players_timer);
    GS_LOAD(Lever_Store);
    GS_LOAD(Return_CP_No);
    GS_LOAD(Return_CP_Index);
    GS_LOAD(Return_Pattern_Index);
    GS_LOAD(Lever_LR);
    GS_LOAD(Last_Eftype);
    GS_LOAD(DENJIN_No);
    GS_LOAD(SC_Personal_Time);
    GS_LOAD(Guard_Counter);
    GS_LOAD(Limit_Time);
    GS_LOAD(Last_Pattern_Index);
    GS_LOAD(Random_ix16_ex);
    GS_LOAD(Random_ix32_ex);
    GS_LOAD(DE_X);
    GS_LOAD(Exit_Timer);
    GS_LOAD(Max_vitality);
    GS_LOAD(Bonus_Game_Flag);
    GS_LOAD(Bonus_Game_Work);
    GS_LOAD(Bonus_Game_result);
    GS_LOAD(Stock_Bonus_Game_Result);
    GS_LOAD(bs_scrrrl);
    GS_LOAD(Bonus_Stage_RNO);
    GS_LOAD(Bonus_Stage_Level);
    GS_LOAD(Bonus_Stage_Tix);
    GS_LOAD(Bonus_Game_ex_result);
    GS_LOAD(Stock_Com_Color);
    GS_LOAD(bs2_floor);
    GS_LOAD(bs2_hosei);
    GS_LOAD(bs2_current_damage);
    GS_LOAD(Win_Record);
    GS_LOAD(Stock_Win_Record);
    GS_LOAD(WGJ_Win);
    GS_LOAD(Target_BG_X);
    GS_LOAD(Offset_BG_X);
    GS_LOAD(Result_Timer);
    GS_LOAD(scrl);
    GS_LOAD(scrr);
    GS_LOAD(vital_stop_flag);
    GS_LOAD(gauge_stop_flag);
    GS_LOAD(Lamp_Timer);
    GS_LOAD(Cont_Timer);
    GS_LOAD(Plate_X);
    GS_LOAD(Plate_Y);
    GS_LOAD(Demo_Timer);
    GS_LOAD(Condense_Buff);
    GS_LOAD(Demo_Ptr);
    GS_LOAD(Keep_Grade);
    GS_LOAD(IO_Result);
    GS_LOAD(VS_Win_Record);
    GS_LOAD(PLsw);
    GS_LOAD(plsw_00);
    GS_LOAD(plsw_01);
    GS_LOAD(Flash_Synchro);
    GS_LOAD(Synchro_Level);
    GS_LOAD(Random_ix16_com);
    GS_LOAD(Random_ix32_com);
    GS_LOAD(Random_ix16_ex_com);
    GS_LOAD(Random_ix32_ex_com);
    GS_LOAD(Random_ix16_bg); // see the GameState_Save comment for why this is saved
    GS_LOAD(Opening_Now);
    GS_LOAD(task);

    // plcnt

    GS_LOAD(plw);
    GS_LOAD(combo_type);
    GS_LOAD(remake_power);
    GS_LOAD(zanzou_table);
    GS_LOAD(super_arts);
    GS_LOAD(piyori_type);
    GS_LOAD(appear_type);
    GS_LOAD(pcon_rno);
    GS_LOAD(round_slow_flag);
    GS_LOAD(pcon_dp_flag);
    GS_LOAD(win_sp_flag);
    GS_LOAD(dead_voice_flag);
    GS_LOAD(rambod);
    GS_LOAD(ramhan);
    GS_LOAD(vital_inc_timer);
    GS_LOAD(vital_dec_timer);
    GS_LOAD(sag_inc_timer);

    // cmd_data

    GS_LOAD(wcp);
    GS_LOAD(t_pl_lvr);
    GS_LOAD(waza_work);

    // cmb_win

    GS_LOAD(cmst_buff);
    GS_LOAD(old_cmb_flag);
    GS_LOAD(cmb_stock);
    GS_LOAD(first_attack);
    GS_LOAD(rever_attack);
    GS_LOAD(paring_attack);
    GS_LOAD(bonus_pts);
    GS_LOAD(hit_num);
    GS_LOAD(sa_kind);
    GS_LOAD(end_flag);
    GS_LOAD(calc_hit);
    GS_LOAD(score_calc);
    GS_LOAD(cmb_all_stock);
    GS_LOAD(sarts_finish_flag);
    GS_LOAD(last_hit_time);
    GS_LOAD(cmb_calc_now);
    GS_LOAD(cst_read);
    GS_LOAD(cst_write);

    // bg

    GS_LOAD(bg_w);
    GS_LOAD(Screen_Switch);
    GS_LOAD(Screen_Switch_Buffer);
    GS_LOAD(rw_num);
    GS_LOAD(rw_bg_flag);
    GS_LOAD(tokusyu_stage);
    GS_LOAD(rw_gbix);
    GS_LOAD(stage_flash);
    GS_LOAD(stage_ftimer);
    GS_LOAD(yang_ix_plus);
    GS_LOAD(yang_ix);
    GS_LOAD(yang_timer);
    GS_LOAD(ending_flag);
    GS_LOAD(end_prm);
    GS_LOAD(gouki_end_gbix);
    GS_LOAD(rw3col_ptr);
    GS_LOAD(bg_disp_off);
    GS_LOAD(bgPalCodeOffset);
    GS_LOAD(rw_dat);

    // charset

    GS_LOAD(att_req);

    // slowf

    GS_LOAD(SLOW_timer);
    GS_LOAD(SLOW_flag);
    GS_LOAD(EXE_flag);

    // grade

    GS_LOAD(judge_gals);
    GS_LOAD(judge_com);
    GS_LOAD(last_judge_dada);
    GS_LOAD(judge_final);
    GS_LOAD(judge_item);
    GS_LOAD(ji_sat);

    // spgauge

    GS_LOAD(Old_Stop_SG);
    GS_LOAD(Exec_Wipe_F);
    GS_LOAD(time_clear);
    GS_LOAD(spg_number);
    GS_LOAD(spg_work);
    GS_LOAD(spg_offset);
    GS_LOAD(time_num);
    GS_LOAD(time_timer);
    GS_LOAD(time_flag);
    GS_LOAD(col);
    GS_LOAD(time_operate);
    GS_LOAD(sast_now);
    GS_LOAD(max2);
    GS_LOAD(max_rno2);
    GS_LOAD(spg_dat);

    // stun

    GS_LOAD(sdat);

    // vital

    GS_LOAD(vit);

    // win_pl

    GS_LOAD(win_free);
    GS_LOAD(win_rno);
    GS_LOAD(poison_flag);

    // ta_sub

    GS_LOAD(eff_hit_flag);

    // sc_sub

    GS_LOAD(FadeLimit);
    GS_LOAD(WipeLimit);

    // appear

    GS_LOAD(Appear_car_stop);
    GS_LOAD(Appear_hv);
    GS_LOAD(Appear_free);
    GS_LOAD(Appear_flag);
    GS_LOAD(app_counter);
    GS_LOAD(appear_work);
    GS_LOAD(Appear_end);

    // bg_data

    GS_LOAD(y_sitei_pos);
    GS_LOAD(y_sitei_flag);
    GS_LOAD(c_number);
    GS_LOAD(c_kakikae);
    GS_LOAD(g_number);
    GS_LOAD(g_kakikae);
    GS_LOAD(nosekae);
    GS_LOAD(scrn_adgjust_y);
    GS_LOAD(scrn_adgjust_x);
    GS_LOAD(zoom_add);
    GS_LOAD(ls_cnt1);
    GS_LOAD(bg_app);
    GS_LOAD(sa_pa_flag);
    GS_LOAD(aku_flag);
    GS_LOAD(seraph_flag);
    GS_LOAD(akebono_flag);
    GS_LOAD(bg_mvxy);
    GS_LOAD(chase_time_y);
    GS_LOAD(chase_time_x);
    GS_LOAD(chase_y);
    GS_LOAD(chase_x);
    GS_LOAD(demo_car_flag);
    GS_LOAD(ideal_w);
    GS_LOAD(bg_app_stop);
    GS_LOAD(bg_stop);
    GS_LOAD(base_y_pos);
    GS_LOAD(etcBgPalCnvTable);
    GS_LOAD(etcBgGixCnvTable);

    // eff56

    GS_LOAD(ci_pointer);
    GS_LOAD(ci_col);
    GS_LOAD(ci_timer);

    // effb2

    GS_LOAD(rf_b2_flag);
    GS_LOAD(b2_curr_no);

    // effb8

    GS_LOAD(test_pl_no);
    GS_LOAD(test_mes_no);
    GS_LOAD(test_in);
    GS_LOAD(old_mes_no2);
    GS_LOAD(old_mes_no3);
    GS_LOAD(old_mes_no_pl);
    GS_LOAD(mes_timer);

    // work_sys — rollback-critical system globals

    GS_LOAD(bg_pos);
    GS_LOAD(fm_pos);
    GS_LOAD(bg_prm);
    GS_LOAD(system_timer);
    GS_LOAD(Gill_Appear_Flag);

    // plcnt — DIP switch combat config

    GS_LOAD(cmd_sel);
    GS_LOAD(no_sa);

    // sc_sub

    GS_LOAD(Hnc_Num);

    // ending

    GS_LOAD(end_w);

    // work_sys (extension)

    GS_LOAD(scr_sc);
    GS_LOAD(X_Adjust);
    GS_LOAD(Y_Adjust);

    // Additional globals

    GS_LOAD(BgMATRIX);
    GS_LOAD(vm_w);
    GS_LOAD(ck_ex_option);
    GS_LOAD(X_Adjust_Buff);
    GS_LOAD(Y_Adjust_Buff);

    /* chainex_check restore — see GameState_Save comment for rationale. */
    {
        extern u8 chainex_check[2][36];
        SDL_memcpy(chainex_check, &src->chainex_check, sizeof(chainex_check));
    }

    /* Color7 / ca_check_flag restore — see GameState_Save. Ported from
     * d5f301cc. */
    {
        extern u16 Color7[2];
        SDL_memcpy(Color7, &src->Color7, sizeof(Color7));
    }
    {
        extern s8 ca_check_flag;
        SDL_memcpy(&ca_check_flag, &src->ca_check_flag, sizeof(ca_check_flag));
    }

    /* spmv_ng_save restore — see GameState_Save comment. */
    {
        extern u32 spmv_ng_save[2];
        SDL_memcpy(spmv_ng_save, &src->spmv_ng_save, sizeof(spmv_ng_save));
    }

    /* effl8_colorram restore — rewinds the palette window effl8 latches from,
     * so a rollback that straddles the SA activation cannot make routine 0
     * re-latch already-buffed colours. See GameState_Save.
     *
     * DELIBERATELY does NOT call palUpdateGhostCP3() for these rows, even
     * though rendering/meta_col.c:19-24 and :82-84 do after writing the same
     * rows. Three reasons, in order of weight:
     *
     *  1. It would make the rollback path do something the simulation never
     *     does. effl8.c writes these rows on every activation and every
     *     buff-end and NEVER refreshes the CP3 ghost — palUpdateGhostCP3 has
     *     no caller in effl8.c (the complete caller list is color3rd.c,
     *     meta_col.c and effe6.c). Restoring ColorRAM without a ghost refresh
     *     is therefore exactly symmetric with what the effect itself does.
     *  2. It would not fix the case it appears to. A torn row arises when a
     *     speculative frame runs a WIDER writer over one of these rows and the
     *     load then restores only the first 12 entries. Two such writers exist,
     *     both on rows 0/8/16/24:
     *       - metamor_color_trans/_restore (meta_col.c:19-24, :73-84) write all
     *         64 entries and push the ghost;
     *       - effect J7's get_new_color_data (effj7.c:491) writes 48 entries to
     *         both tables, which effj7.c:400-401 point at exactly these rows.
     *     In both cases the ghost still holds a self-consistent row; refreshing
     *     it would PUBLISH the tear to VRAM instead of leaving a stale-but-
     *     coherent row. The remedy for that case would be widening the slice or
     *     saving those writers' rows, not a ghost push.
     *
     *     Note the tear cannot feed back into simulation state: the only path
     *     from ColorRAM into saved state is effl8's save_old_color_data
     *     (effl8.c:47), which reads exactly the EFFL8_COLOR_ENTRIES prefix this
     *     loop restores. effj7 has no save_old_color_data at all. So the save
     *     window is the right width for correctness even though it is narrower
     *     than the widest writer.
     *  3. Cost and blast radius. palUpdateGhostCP3 is VRAM work
     *     (flLockPalette/palConvRowTim2CI8Clut/flUnlockPalette, color3rd.c:531)
     *     — four lock/copy-64/unlock round trips on every rollback tick on a
     *     target with limited frame budget — and it would introduce
     *     rollback-driven mutation of render-side state, the exact class
     *     docs/rollback-determinism-harness.md's Known limit 1 records as
     *     crash-prone (the ppgSetupPalChunk arcade trap).
     *
     * The residual is cosmetic, self-healing on the next full write, and
     * strictly better than the pre-fix behaviour (which left all 64 wrong).
     * Revisit only with a reproduction, not on principle. */
    for (int i = 0; i < EFFL8_COLORRAM_ROW_COUNT; i++) {
        SDL_memcpy(ColorRAM[EFFL8_COLORRAM_ROWS[i]], src->effl8_colorram[i], sizeof(src->effl8_colorram[i]));
    }
}

// ============================================================================
// Phase 3: focused checksum, sanitizers, desync dump.
// Ported from /tmp/3sxtra/src/netplay/game_state.c:1447-1810.
//
// Key deltas from 3sxtra:
//  - sanitize_plw_pointers also zeros p->cb and p->rp (our PLW-only fields;
//    research doc §19 risk 2 — verbatim copy would leak heap pointers into
//    the checksum and produce false-positive desyncs).
//  - The focused whitelist explicitly hashes combo_type and remake_power
//    (research doc §19 risk 1 — 3sxtra moved these into PLW; we kept them
//    as top-level globals, so the PLW hash alone would miss damage-scaling
//    drift).
//  - Main checksum path is NOT behind #if DEBUG. Research §19 risk 5
//    and §8.2: our Release MiSTer binary MUST have desync detection.
//  - dump_desync_state + ring buffers were promoted out of #if DEBUG on
//    2026-04-26 so telemetry builds capture pre-desync history when a
//    session terminates abnormally — the recording is cheap and the dump
//    only fires once per session (right before soft-reset to title).
// ============================================================================

#define SDL_copya(dst, src) SDL_memcpy(dst, src, sizeof(src))

static int battle_start_frame = -1;

/* Diagnostic ring buffers for desync triage. Promoted out of #if DEBUG
 * 2026-04-26 — telemetry builds need to dump pre-desync history when a
 * session terminates abnormally. Per-frame recording cost is small
 * (~7 djb2s + ~36 scalar hashes + a few memcpys, all over memory we
 * already touched for the combined checksum). Memory footprint is
 * dominated by state_buffer at sizeof(State) * STATE_BUFFER_MAX
 * (~2.6 MB on 32-bit), which is acceptable on the MiSTer's 1 GB RAM. */
#define STATE_BUFFER_MAX 20

// Per-subsystem checksums for faster desync triage.
typedef struct {
    uint32_t plw0;
    uint32_t plw1;
    uint32_t bg;
    uint32_t tasks;
    uint32_t effects;
    uint32_t globals;
    uint32_t combined;
} SectionedChecksum;

/* 2026-04-24 desync investigation — per-field hash of every scalar/array
 * included in the combined checksum. Populated each frame; dumped at desync
 * so diffing two peer dumps pinpoints the first-diverging field. */
enum {
    FH_Random_ix16, FH_Random_ix32,
    FH_Random_ix16_ex, FH_Random_ix32_ex,
    FH_Random_ix16_com, FH_Random_ix32_com,
    FH_Random_ix16_ex_com, FH_Random_ix32_ex_com,
    FH_Round_num, FH_Round_Level, FH_Round_Result,
    FH_PL_Wins, FH_Conclusion_Type, FH_win_type,
    FH_My_char, FH_Super_Arts,
    FH_combo_type, FH_remake_power,
    FH_Attack_Flag, FH_Counter_Attack, FH_Guard_Flag,
    FH_Flip_Flag, FH_Lie_Flag, FH_Attack_Counter,
    FH_Bullet_No, FH_Bullet_Counter, FH_paring_counter,
    FH_Present_Mode, FH_VS_Stage,
    FH_SLOW_timer, FH_SLOW_flag, FH_EXE_flag,
    FH_super_arts, FH_piyori_type, FH_Max_vitality,
    FH_chainex_check,
    FH_Color7, FH_ca_check_flag,
    FH_spmv_ng_save,
    FH_COUNT
};
static const char* const FH_NAMES[FH_COUNT] = {
    "Random_ix16", "Random_ix32",
    "Random_ix16_ex", "Random_ix32_ex",
    "Random_ix16_com", "Random_ix32_com",
    "Random_ix16_ex_com", "Random_ix32_ex_com",
    "Round_num", "Round_Level", "Round_Result",
    "PL_Wins", "Conclusion_Type", "win_type",
    "My_char", "Super_Arts",
    "combo_type", "remake_power",
    "Attack_Flag", "Counter_Attack", "Guard_Flag",
    "Flip_Flag", "Lie_Flag", "Attack_Counter",
    "Bullet_No", "Bullet_Counter", "paring_counter",
    "Present_Mode", "VS_Stage",
    "SLOW_timer", "SLOW_flag", "EXE_flag",
    "super_arts", "piyori_type", "Max_vitality",
    "chainex_check",
    "Color7", "ca_check_flag",
    "spmv_ng_save",
};

static State state_buffer[STATE_BUFFER_MAX];
static SectionedChecksum saved_section_checksums[STATE_BUFFER_MAX];
static PLW saved_plw_scratch[STATE_BUFFER_MAX][2];
static uint32_t saved_field_hashes[STATE_BUFFER_MAX][FH_COUNT];

/**
 * @brief Snapshot the complete game state.
 *
 * @netplay_sync Called by save_state() on every GekkoSaveEvent. Copies both
 * the GameState (via GameState_Save) and the EffectState (effect pool +
 * free list) into dst.
 */
static void gather_state(State* dst) {
    // GameState
    GameState* gs = &dst->gs;
    GameState_Save(gs);

    // EffectState
    EffectState* es = &dst->es;
    SDL_copya(es->frw, frw);
    SDL_copya(es->exec_tm, exec_tm);
    SDL_copya(es->frwque, frwque);
    SDL_copya(es->head_ix, head_ix);
    SDL_copya(es->tail_ix, tail_ix);
    es->frwctr = frwctr;
    es->frwctr_min = frwctr_min;
}

/* === Sparse effect-pool save path (Option A) =====================
 *
 * Wire format (matches game_state.h's SPARSE_HEADER_BYTES layout):
 *
 *   offset  size  field
 *   ------  ----  ----------------------------------------------------
 *        0     N  GameState (sizeof(GameState))
 *        N     2  s16 frwctr
 *      N+2     2  s16 frwctr_min
 *      N+4    16  s16 head_ix[8]
 *     N+20    16  s16 tail_ix[8]
 *     N+36    16  s16 exec_tm[8]
 *     N+52   256  s16 frwque[128]
 *    N+308    16  u8  active_mask[16]   (128 bits, slot index → bit)
 *    N+324     2  u16 active_count      (popcount(active_mask))
 *    N+326     2  u16 reserved          (zero, alignment pad)
 *    N+328     M  u8  active_slots_data[active_count][1792]
 *
 * SPARSE_HEADER_BYTES = 328 (verified by _Static_assert below).
 * The full size on the wire is sizeof(GameState) + 328 + active_count*1792.
 * 2026-04-26: A `frw[]` byte-blob produced by this path is NEVER mixed with
 * the legacy full-State byte-blob inside the same save buffer — load_state
 * dispatches by total state_len.
 */
#define SPARSE_OFF_FRWCTR        0
#define SPARSE_OFF_FRWCTR_MIN    2
#define SPARSE_OFF_HEAD_IX       4
#define SPARSE_OFF_TAIL_IX      20
#define SPARSE_OFF_EXEC_TM      36
#define SPARSE_OFF_FRWQUE       52
#define SPARSE_OFF_ACTIVE_MASK 308
#define SPARSE_OFF_ACTIVE_COUNT 324
#define SPARSE_OFF_RESERVED    326
#define SPARSE_OFF_PAYLOAD     328

_Static_assert(SPARSE_OFF_PAYLOAD == SPARSE_HEADER_BYTES,
               "Sparse-save header layout mismatch — update SPARSE_HEADER_BYTES "
               "in game_state.h or the SPARSE_OFF_* offsets in game_state.c.");

/* The offsets above are a hand-written table, but every field they address is
 * memcpy'd with `sizeof(<the live global>)` in pack_sparse_state /
 * unpack_sparse_state — so the table is a DERIVATION of those globals' sizes,
 * not an independent fact. Left as a comment it rots the way the netplay_nav
 * timeout derivation rotted: growing EFFECT_MAX silently widens `frwque` past
 * SPARSE_OFF_ACTIVE_MASK and the frwque memcpy scribbles over active_mask
 * inside the very buffer both peers rewind from. Each offset is therefore
 * pinned to the previous field's real size rather than restated. */
_Static_assert(SPARSE_OFF_FRWCTR_MIN == SPARSE_OFF_FRWCTR + sizeof(frwctr),
               "sparse header: frwctr_min offset must follow sizeof(frwctr)");
_Static_assert(SPARSE_OFF_HEAD_IX == SPARSE_OFF_FRWCTR_MIN + sizeof(frwctr_min),
               "sparse header: head_ix offset must follow sizeof(frwctr_min)");
_Static_assert(SPARSE_OFF_TAIL_IX == SPARSE_OFF_HEAD_IX + sizeof(head_ix),
               "sparse header: tail_ix offset must follow sizeof(head_ix)");
_Static_assert(SPARSE_OFF_EXEC_TM == SPARSE_OFF_TAIL_IX + sizeof(tail_ix),
               "sparse header: exec_tm offset must follow sizeof(tail_ix)");
_Static_assert(SPARSE_OFF_FRWQUE == SPARSE_OFF_EXEC_TM + sizeof(exec_tm),
               "sparse header: frwque offset must follow sizeof(exec_tm)");
_Static_assert(SPARSE_OFF_ACTIVE_MASK == SPARSE_OFF_FRWQUE + sizeof(frwque),
               "sparse header: active_mask offset must follow sizeof(frwque) — "
               "raising EFFECT_MAX widens frwque and overruns the mask");
_Static_assert(SPARSE_OFF_RESERVED == SPARSE_OFF_ACTIVE_COUNT + sizeof(uint16_t),
               "sparse header: reserved pad must follow the u16 active_count");
_Static_assert(SPARSE_OFF_PAYLOAD == SPARSE_OFF_RESERVED + 2,
               "sparse header: payload must follow the 2-byte reserved pad");

/* active_mask is a bitmap with one bit per frw[] slot. Its width is implied by
 * the offset table (ACTIVE_COUNT - ACTIVE_MASK) and hard-coded as the literal
 * 16 in pack_sparse_state's SDL_memset and in popcount16_bytes. Tie all three
 * to EFFECT_MAX so a pool that outgrows 128 slots cannot silently ship a mask
 * that only describes the first 128. */
_Static_assert(EFFECT_MAX % 8 == 0,
               "sparse header: EFFECT_MAX must be a whole number of mask bytes");
_Static_assert(SPARSE_OFF_ACTIVE_COUNT - SPARSE_OFF_ACTIVE_MASK == EFFECT_MAX / 8,
               "sparse header: active_mask must be exactly EFFECT_MAX bits wide");

/* load_state_from_event() distinguishes the two wire formats by total length
 * alone: `len == sizeof(State)` means legacy full-state, anything else is
 * parsed as sparse. That dispatch is only sound because no legal sparse length
 * can equal sizeof(State) — the "No-collision proof" written out above
 * load_state_from_event(). Collision requires an integer N with
 *   SPARSE_HEADER_BYTES + N * SPARSE_FRW_SLOT_BYTES == sizeof(EffectState),
 * so proving the remainder non-zero rules it out for EVERY N, not just the
 * [0, EFFECT_MAX] range the proof enumerates. The prose proof substitutes
 * EFFECT_MAX=128 and a 32-bit layout by hand; this checks the real sizes on
 * whatever bitness is being built, which is the part the prose cannot do. */
_Static_assert(sizeof(EffectState) > SPARSE_HEADER_BYTES,
               "sparse/full dispatch: EffectState must exceed the sparse header");
_Static_assert((sizeof(EffectState) - SPARSE_HEADER_BYTES) % SPARSE_FRW_SLOT_BYTES != 0,
               "sparse/full dispatch collision: a sparse buffer can be exactly "
               "sizeof(State) bytes long, so load_state_from_event() would parse "
               "a full-state save as sparse (or vice versa)");

static bool s_sparse_effect_save_enabled = true;

void Netplay_SetSparseEffectSaveEnabled(bool enabled) {
    if (s_sparse_effect_save_enabled != enabled) {
        SDL_Log("[netplay] sparse effect-pool save: %s",
                enabled ? "ENABLED" : "DISABLED (full-state save)");
    }
    s_sparse_effect_save_enabled = enabled;
}

bool Netplay_GetSparseEffectSaveEnabled(void) {
    return s_sparse_effect_save_enabled;
}

/* Pack the sparse wire format into out_buf, returning the number of bytes
 * written. Reads from the live GameState (already gathered into gs_src)
 * and the live effect-pool globals (frw, head_ix, etc.). */
static unsigned int pack_sparse_state(unsigned char* out_buf,
                                      const GameState* gs_src) {
    /* GameState first — verbatim copy of the gathered scratch. */
    SDL_memcpy(out_buf, gs_src, sizeof(GameState));
    unsigned char* hdr = out_buf + sizeof(GameState);

    /* Header scalars + arrays (all from live globals). */
    SDL_memcpy(hdr + SPARSE_OFF_FRWCTR,     &frwctr,     sizeof(frwctr));
    SDL_memcpy(hdr + SPARSE_OFF_FRWCTR_MIN, &frwctr_min, sizeof(frwctr_min));
    SDL_memcpy(hdr + SPARSE_OFF_HEAD_IX, head_ix, sizeof(head_ix));
    SDL_memcpy(hdr + SPARSE_OFF_TAIL_IX, tail_ix, sizeof(tail_ix));
    SDL_memcpy(hdr + SPARSE_OFF_EXEC_TM, exec_tm, sizeof(exec_tm));
    SDL_memcpy(hdr + SPARSE_OFF_FRWQUE,  frwque,  sizeof(frwque));

    /* Build active_mask + payload by walking frw[] linearly. The canonical
     * "active" predicate is be_flag != 0 — the same predicate the legacy
     * save path's inactive-zero step at save_current_state:1676-1693 uses.
     * Walking the head_ix linked lists would also work but linear is
     * simpler and gives byte-stable slot ordering for the parity tests. */
    unsigned char* mask = hdr + SPARSE_OFF_ACTIVE_MASK;
    SDL_memset(mask, 0, 16);
    unsigned char* payload = hdr + SPARSE_OFF_PAYLOAD;
    uint16_t active_count = 0;
    for (int i = 0; i < EFFECT_MAX; i++) {
        const WORK* w = (const WORK*)frw[i];
        if (w->be_flag != 0) {
            mask[i >> 3] |= (unsigned char)(1u << (i & 7));
            SDL_memcpy(payload, frw[i], SPARSE_FRW_SLOT_BYTES);
            payload += SPARSE_FRW_SLOT_BYTES;
            active_count++;
        }
    }
    SDL_memcpy(hdr + SPARSE_OFF_ACTIVE_COUNT, &active_count, sizeof(active_count));
    /* Zero the reserved/pad word so two peers see a deterministic image. */
    SDL_memset(hdr + SPARSE_OFF_RESERVED, 0, 2);

    return (unsigned int)(sizeof(GameState) + SPARSE_HEADER_BYTES +
                          (size_t)active_count * SPARSE_FRW_SLOT_BYTES);
}

/* Reconstruct frw[] + the EffectState scalars from a sparse-format buffer.
 * Returns true on success. On any structural failure (bad active_count vs
 * popcount, buffer size mismatch) returns false and leaves the live
 * globals untouched so the caller can fall back to a fatal-grade log. */
static int popcount16_bytes(const unsigned char mask[16]) {
    int n = 0;
    for (int i = 0; i < 16; i++) {
        unsigned char b = mask[i];
        b = (unsigned char)((b & 0x55) + ((b >> 1) & 0x55));
        b = (unsigned char)((b & 0x33) + ((b >> 2) & 0x33));
        b = (unsigned char)((b & 0x0F) + ((b >> 4) & 0x0F));
        n += b;
    }
    return n;
}

static bool unpack_sparse_state(const unsigned char* in_buf, unsigned int in_len) {
    if (in_len < sizeof(GameState) + SPARSE_HEADER_BYTES) {
        return false;
    }
    const unsigned char* hdr = in_buf + sizeof(GameState);
    uint16_t active_count;
    SDL_memcpy(&active_count, hdr + SPARSE_OFF_ACTIVE_COUNT, sizeof(active_count));
    if (active_count > EFFECT_MAX) {
        return false;
    }
    const size_t expected = sizeof(GameState) + SPARSE_HEADER_BYTES +
                            (size_t)active_count * SPARSE_FRW_SLOT_BYTES;
    if ((size_t)in_len != expected) {
        return false;
    }
    const unsigned char* mask = hdr + SPARSE_OFF_ACTIVE_MASK;
    if (popcount16_bytes(mask) != (int)active_count) {
        return false;
    }

    /* GameState — caller already restored via GameState_Load(); we only
     * touch the effect-pool globals here. */

    /* Header scalars. */
    SDL_memcpy(&frwctr,     hdr + SPARSE_OFF_FRWCTR,     sizeof(frwctr));
    SDL_memcpy(&frwctr_min, hdr + SPARSE_OFF_FRWCTR_MIN, sizeof(frwctr_min));
    SDL_memcpy(head_ix, hdr + SPARSE_OFF_HEAD_IX, sizeof(head_ix));
    SDL_memcpy(tail_ix, hdr + SPARSE_OFF_TAIL_IX, sizeof(tail_ix));
    SDL_memcpy(exec_tm, hdr + SPARSE_OFF_EXEC_TM, sizeof(exec_tm));
    SDL_memcpy(frwque,  hdr + SPARSE_OFF_FRWQUE,  sizeof(frwque));

    /* Reset frw[] to canonical empty state. Mirrors effect_work_init's
     * post-SDL_zeroa pass (effect.c:88-99): each slot's myself = i,
     * before = -1, behind = -1, everything else zero. This matches the
     * legacy save path's per-slot inactive-zero (game_state.c:1676-1693)
     * exactly. */
    SDL_zeroa(frw);
    for (int i = 0; i < EFFECT_MAX; i++) {
        WORK* w = (WORK*)frw[i];
        w->myself = (s16)i;
        w->before = -1;
        w->behind = -1;
    }

    /* Splat each active slot back from the payload, in slot-index order. */
    const unsigned char* payload = hdr + SPARSE_OFF_PAYLOAD;
    for (int i = 0; i < EFFECT_MAX; i++) {
        if (mask[i >> 3] & (unsigned char)(1u << (i & 7))) {
            SDL_memcpy(frw[i], payload, SPARSE_FRW_SLOT_BYTES);
            payload += SPARSE_FRW_SLOT_BYTES;
        }
    }
    return true;
}

/* Pack the legacy full-state wire format. Sized to sizeof(State); contents
 * match what gather_state writes (header is the GameState then the full
 * EffectState). Used both as the kill-switch path and as the safety
 * fallback when the active-slot count exceeds SPARSE_CEILING_SLOTS. */
static unsigned int pack_full_state(unsigned char* out_buf,
                                    const State* scratch) {
    SDL_memcpy(out_buf, scratch, sizeof(State));
    return (unsigned int)sizeof(State);
}

// ============================================================================
// Sanitizers — zero pointer fields and rendering-only bits so they don't
// pollute the rollback checksum (ASLR makes pointers differ between peers).
// Only ever called on scratch copies, never on live restore targets.
// ============================================================================

static void sanitize_work_pointers(WORK* w) {
    w->target_adrs = NULL;
    w->hit_adrs = NULL;
    w->dmg_adrs = NULL;
    w->suzi_offset = NULL;
    SDL_zeroa(w->char_table);
    w->se_random_table = NULL;
    w->step_xy_table = NULL;
    w->move_xy_table = NULL;
    w->overlap_char_tbl = NULL;
    w->olc_ix_table = NULL;
    w->rival_catch_tbl = NULL;
    w->curr_rca = NULL;
    w->set_char_ad = NULL;
    w->hit_ix_table = NULL;
    w->body_adrs = NULL;
    w->h_bod = NULL;
    w->hand_adrs = NULL;
    w->h_han = NULL;
    w->dumm_adrs = NULL;
    w->h_dumm = NULL;
    w->catch_adrs = NULL;
    w->h_cat = NULL;
    w->caught_adrs = NULL;
    w->h_cau = NULL;
    w->attack_adrs = NULL;
    w->h_att = NULL;
    w->h_eat = NULL;
    w->hosei_adrs = NULL;
    w->h_hos = NULL;
    w->att_ix_table = NULL;
    w->my_effadrs = NULL;
}

/// Mask rendering-only bits/fields from WORK color fields (3sxtra surgical;
/// matches /tmp/3sxtra/src/netplay/game_state.c:1513-1519).
static void sanitize_work_rendering(WORK* w) {
    w->current_colcd &= ~0x2000;
    w->my_col_code &= ~0x2000;
    w->colcd = 0; // Rendering-derived, not gameplay state
    w->extra_col &= ~0x2000;
    w->extra_col_2 &= ~0x2000;
}

/// Zero all pointer fields and mask rendering bits in a PLW struct.
/// KEEP our cb/rp zeroing — those fields only exist in our PLW; verbatim
/// 3sxtra copy would leak heap pointers into the checksum (research §19
/// risk 2, tier-2 plan Phase 3 sub-task 2).
static void sanitize_plw_pointers(PLW* p) {
    sanitize_work_pointers(&p->wu);
    sanitize_work_rendering(&p->wu);
    p->cp = NULL;
    p->dm_step_tbl = NULL;
    p->as = NULL;
    p->sa = NULL;
    p->py = NULL;
    // Our fork's PLW has these two; 3sxtra's does not.
    p->cb = NULL;
    p->rp = NULL;
}

/* Public wrappers around the sanitizers, shared by the focused-checksum
 * path in save_current_state() below and the rollback-determinism harness
 * (src/test/rollback_determinism.c), which must hash plw / effect-pool
 * WORK slots through EXACTLY the view the production checksum compares —
 * one source of truth for what counts as gameplay bytes vs pointer/render
 * noise. Both operate on scratch COPIES only, never live state. */

void GameState_SanitizeWorkCopyForHash(WORK* w) {
    sanitize_work_pointers(w);
    sanitize_work_rendering(w);
}

void GameState_SanitizePlwCopyForHash(PLW* copy) {
    sanitize_plw_pointers(copy);
    sanitize_work_rendering(&copy->wu);

    // Linked-list indices and timing differ per allocation order.
    copy->wu.before = 0;
    copy->wu.behind = 0;
    copy->wu.myself = 0;
    copy->wu.listix = 0;
    copy->wu.timing = 0;

    /* NO POINTER-LIKE u64 SWEEP HERE — deleted 2026-08-29 (task #111).
     *
     * What used to be here scanned PLW as uint64_t words and zeroed any
     * word whose value was > 4 GiB with the top 17 bits clear, under a
     * comment claiming the fixed stride made "both 32-bit and 64-bit
     * platforms scan the same bytes and produce identical checksums".
     * Both halves of that claim were false, and the sweep was actively
     * destroying checksum coverage:
     *
     *  - Coverage was never identical: sizeof(PLW) is 1092 on armv7 and
     *    1304 on 64-bit, so the loop ran 136 vs 163 iterations over
     *    different bytes at different offsets.
     *  - The filter itself is architecture-sensitive: on armv7 no single
     *    pointer can exceed 0x100000000, so it could only ever fire on
     *    coincidental adjacent-slot pairs, while on 64-bit it fires on
     *    heap addresses.
     *  - Every pointer in PLW is already NULLed above by
     *    sanitize_plw_pointers / sanitize_work_pointers (all 38 pointer
     *    members / 49 slots), so by the time the sweep ran there was no
     *    pointer left to catch. Measured on 22,716 real-gameplay PLW
     *    sanitizations (rollback-determinism fast profile): it zeroed
     *    241,132 words — 10.6 words / 84.9 bytes per PLW, 6.5% of the
     *    hashed image — and every single zeroed word was ordinary
     *    gameplay data: routine_no[], vitality, position_x/y, guard_flag,
     *    attack_num, current_attack, hit_stop, the cm* command buffers.
     *    Two peers whose values BOTH landed in the filter range were
     *    zeroed to the same 0, which is a desync the checksum could not
     *    see.
     *
     * Architecture independence is now provided properly, by hashing the
     * canonical member image (GameState_EmitPlwCanonical) instead of raw
     * struct bytes. */
}

/// Save state in state buffer (ring buffer for desync dump). Always-on
/// 2026-04-26 — telemetry needs the same dump infrastructure as DEBUG.
static State* note_state(const State* state, int frame) {
    if (frame < 0) {
        frame += STATE_BUFFER_MAX;
    }
    State* dst = &state_buffer[frame % STATE_BUFFER_MAX];
    SDL_memcpy(dst, state, sizeof(State));
    return dst;
}

/**
 * @brief Save game state for rollback — GekkoNet callback backend.
 *
 * @netplay_sync Called by save_state() on every frame. Computes a focused
 * gameplay checksum for desync detection in BOTH Debug and Release.
 * In DEBUG builds, additionally saves per-subsystem checksums and PLW
 * copies for binary comparison when a desync is detected.
 *
 * The checksum covers only a whitelist of gameplay-critical fields
 * (PLW as its canonical member image — see GameState_EmitPlwCanonical —
 * after pointer/rendering sanitization, RNG indices, round state,
 * combat flags, slow-motion, super gauge, stun, PLUS combo_type and
 * remake_power which are our fork-exclusive top-level globals).
 * UI-only fields are saved but excluded from the hash.
 *
 * 2026-04-26 (Option A sparse save): this routine still writes a full
 * sizeof(State) blob into `buffer`. After the sparse refactor, save_state
 * uses an internal scratch buffer here for checksum + ring-buffer + dump
 * bookkeeping, then re-encodes into Gekko's actual save buffer in either
 * sparse or full format (see save_state below). The per-slot inactive-
 * zero pass below remains useful for the *dump* artefact (so post-mortem
 * State binaries don't contain stale-pointer noise in inactive slots),
 * but is bypassed by the sparse wire format which simply omits inactive
 * slots and reconstructs canonical empty on load.
 */
uint32_t save_current_state(void* buffer, int frame) {
    State* dst = (State*)buffer;
    gather_state(dst);

    // Activate checksumming from the very first synced frame.
    if (battle_start_frame < 0) {
        battle_start_frame = frame;
        SDL_Log("[netplay] checksumming active from frame %d (G_No[1]=%d)", frame, G_No[1]);
    }

    const bool checksumming_active = battle_start_frame >= 0;

    note_state(dst, frame);

    // Sanitize non-functional data in dst (safe for rollback restore):
    // inactive effect slots, padding arrays, WORK_Other unused tails.
    {
        EffectState* es = &dst->es;
        for (int i = 0; i < EFFECT_MAX; i++) {
            WORK* w = (WORK*)es->frw[i];
            if (w->be_flag == 0) {
                s16 before = w->before;
                s16 behind = w->behind;
                s16 myself = w->myself;
                SDL_memset(es->frw[i], 0, sizeof(es->frw[i]));
                w->before = before;
                w->behind = behind;
                w->myself = myself;
            } else {
                SDL_zeroa(w->wrd_free);
                WORK_Other* wo = (WORK_Other*)w;
                SDL_zeroa(wo->et_free);
            }
        }
        note_state(dst, frame);
    }

    if (checksumming_active) {
        // === Focused gameplay checksum ===
        // Hash ONLY gameplay-critical data, not the full 247KB State.

        // --- Sanitized PLW copies + their canonical hash images ---
        // plw_scratch keeps the sanitized STRUCT (the desync-dump artefact
        // and the per-field hashes below index into it); plw_canon is the
        // architecture-independent member image the checksum actually
        // hashes (task #111).
        static PLW plw_scratch[2];
        static uint8_t plw_canon[2][PLW_CANON_SIZE];
        for (int p = 0; p < 2; p++) {
            SDL_memcpy(&plw_scratch[p], &dst->gs.plw[p], sizeof(PLW));
            GameState_SanitizePlwCopyForHash(&plw_scratch[p]);
            GameState_EmitPlwCanonical(&plw_scratch[p], plw_canon[p]);
        }

        // --- Build combined hash from PLW + whitelisted globals ---
        const GameState* gs = &dst->gs;
        uint32_t h = djb2_init();

        // PLW (sanitized, canonical member image)
        h = djb2_update_mem(h, plw_canon[0], PLW_CANON_SIZE);
        h = djb2_update_mem(h, plw_canon[1], PLW_CANON_SIZE);

        // RNG indices
        h = djb2_update_mem(h, (const uint8_t*)&gs->Random_ix16, sizeof(gs->Random_ix16));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Random_ix32, sizeof(gs->Random_ix32));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Random_ix16_ex, sizeof(gs->Random_ix16_ex));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Random_ix32_ex, sizeof(gs->Random_ix32_ex));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Random_ix16_com, sizeof(gs->Random_ix16_com));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Random_ix32_com, sizeof(gs->Random_ix32_com));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Random_ix16_ex_com, sizeof(gs->Random_ix16_ex_com));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Random_ix32_ex_com, sizeof(gs->Random_ix32_ex_com));

        // Round/match
        h = djb2_update_mem(h, (const uint8_t*)&gs->Round_num, sizeof(gs->Round_num));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Round_Level, sizeof(gs->Round_Level));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Round_Result, sizeof(gs->Round_Result));
        h = djb2_update_mem(h, (const uint8_t*)&gs->PL_Wins, sizeof(gs->PL_Wins));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Conclusion_Type, sizeof(gs->Conclusion_Type));
        h = djb2_update_mem(h, (const uint8_t*)&gs->win_type, sizeof(gs->win_type));

        // Player identity
        h = djb2_update_mem(h, (const uint8_t*)&gs->My_char, sizeof(gs->My_char));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Super_Arts, sizeof(gs->Super_Arts));

        // Our fork-exclusive top-level globals (NOT in 3sxtra's PLW hash).
        // Research doc §19 risk 1: without this, damage scaling drift goes
        // undetected.
        h = djb2_update_mem(h, (const uint8_t*)&gs->combo_type, sizeof(gs->combo_type));
        h = djb2_update_mem(h, (const uint8_t*)&gs->remake_power, sizeof(gs->remake_power));

        // Combat flags
        h = djb2_update_mem(h, (const uint8_t*)&gs->Attack_Flag, sizeof(gs->Attack_Flag));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Counter_Attack, sizeof(gs->Counter_Attack));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Guard_Flag, sizeof(gs->Guard_Flag));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Flip_Flag, sizeof(gs->Flip_Flag));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Lie_Flag, sizeof(gs->Lie_Flag));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Attack_Counter, sizeof(gs->Attack_Counter));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Bullet_No, sizeof(gs->Bullet_No));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Bullet_Counter, sizeof(gs->Bullet_Counter));
        h = djb2_update_mem(h, (const uint8_t*)&gs->paring_counter, sizeof(gs->paring_counter));

        // Game flow
        h = djb2_update_mem(h, (const uint8_t*)&gs->Present_Mode, sizeof(gs->Present_Mode));
        h = djb2_update_mem(h, (const uint8_t*)&gs->VS_Stage, sizeof(gs->VS_Stage));

        // Slow motion
        h = djb2_update_mem(h, (const uint8_t*)&gs->SLOW_timer, sizeof(gs->SLOW_timer));
        h = djb2_update_mem(h, (const uint8_t*)&gs->SLOW_flag, sizeof(gs->SLOW_flag));
        h = djb2_update_mem(h, (const uint8_t*)&gs->EXE_flag, sizeof(gs->EXE_flag));

        // Super gauge / stun / vitality
        h = djb2_update_mem(h, (const uint8_t*)&gs->super_arts, sizeof(gs->super_arts));
        h = djb2_update_mem(h, (const uint8_t*)&gs->piyori_type, sizeof(gs->piyori_type));
        h = djb2_update_mem(h, (const uint8_t*)&gs->Max_vitality, sizeof(gs->Max_vitality));

        // chainex_check (EX-SA chain gating) — previously rollback-unsafe;
        // now saved via GameState (see struct field). Hash the saved copy so
        // cross-peer comparison catches any divergence just like plw_scratch.
        h = djb2_update_mem(h, (const uint8_t*)&gs->chainex_check, sizeof(gs->chainex_check));

        // Color7 (char-select color chord) + ca_check_flag (battle throw/CA
        // gate) — previously rollback-unsafe cross-frame globals; now saved
        // via GameState. Hash the saved copies so any residual divergence is
        // caught cross-peer just like chainex_check. Ported from d5f301cc.
        h = djb2_update_mem(h, (const uint8_t*)&gs->Color7, sizeof(gs->Color7));
        h = djb2_update_mem(h, (const uint8_t*)&gs->ca_check_flag, sizeof(gs->ca_check_flag));

        // spmv_ng_save (Makoto SA-buff backup of PLW.spmv_ng_flag) —
        // previously rollback-unsafe (2026-04-24 Candidate 0b wrongly
        // exonerated it as dead code via CPS3 character numbering; with
        // CPS3 off, id 16 IS Makoto and the code is live). Now saved via
        // GameState; hash the saved copy so any residual divergence is
        // caught cross-peer just like chainex_check.
        h = djb2_update_mem(h, (const uint8_t*)&gs->spmv_ng_save, sizeof(gs->spmv_ng_save));

        // Per-section checksums + per-field hashes for desync triage. Always-on
        // 2026-04-26: telemetry consumes the same diagnostic surface as DEBUG.
        SectionedChecksum sc;
        uint32_t sh;
        sh = djb2_init();
        sh = djb2_update_mem(sh, plw_canon[0], PLW_CANON_SIZE);
        sc.plw0 = sh;
        sh = djb2_init();
        sh = djb2_update_mem(sh, plw_canon[1], PLW_CANON_SIZE);
        sc.plw1 = sh;
        sc.bg = 0;
        sc.tasks = 0;
        sc.effects = 0;
        sc.combined = h;
        sc.globals = h ^ sc.plw0 ^ sc.plw1;
        saved_section_checksums[frame % STATE_BUFFER_MAX] = sc;
        SDL_memcpy(&saved_plw_scratch[frame % STATE_BUFFER_MAX][0], &plw_scratch[0], sizeof(PLW));
        SDL_memcpy(&saved_plw_scratch[frame % STATE_BUFFER_MAX][1], &plw_scratch[1], sizeof(PLW));

        /* Per-field hashes — 2026-04-24 desync diag. Each FH_* is djb2 of
         * that field's raw bytes at this frame. Diff two peer dumps to find
         * the first-diverging field. */
        {
            uint32_t* fh = saved_field_hashes[frame % STATE_BUFFER_MAX];
#define HASHONE(ix, fld) do { \
    uint32_t _s = djb2_init(); \
    _s = djb2_update_mem(_s, (const uint8_t*)&(fld), sizeof(fld)); \
    fh[ix] = _s; \
} while (0)
            HASHONE(FH_Random_ix16, gs->Random_ix16);
            HASHONE(FH_Random_ix32, gs->Random_ix32);
            HASHONE(FH_Random_ix16_ex, gs->Random_ix16_ex);
            HASHONE(FH_Random_ix32_ex, gs->Random_ix32_ex);
            HASHONE(FH_Random_ix16_com, gs->Random_ix16_com);
            HASHONE(FH_Random_ix32_com, gs->Random_ix32_com);
            HASHONE(FH_Random_ix16_ex_com, gs->Random_ix16_ex_com);
            HASHONE(FH_Random_ix32_ex_com, gs->Random_ix32_ex_com);
            HASHONE(FH_Round_num, gs->Round_num);
            HASHONE(FH_Round_Level, gs->Round_Level);
            HASHONE(FH_Round_Result, gs->Round_Result);
            HASHONE(FH_PL_Wins, gs->PL_Wins);
            HASHONE(FH_Conclusion_Type, gs->Conclusion_Type);
            HASHONE(FH_win_type, gs->win_type);
            HASHONE(FH_My_char, gs->My_char);
            HASHONE(FH_Super_Arts, gs->Super_Arts);
            HASHONE(FH_combo_type, gs->combo_type);
            HASHONE(FH_remake_power, gs->remake_power);
            HASHONE(FH_Attack_Flag, gs->Attack_Flag);
            HASHONE(FH_Counter_Attack, gs->Counter_Attack);
            HASHONE(FH_Guard_Flag, gs->Guard_Flag);
            HASHONE(FH_Flip_Flag, gs->Flip_Flag);
            HASHONE(FH_Lie_Flag, gs->Lie_Flag);
            HASHONE(FH_Attack_Counter, gs->Attack_Counter);
            HASHONE(FH_Bullet_No, gs->Bullet_No);
            HASHONE(FH_Bullet_Counter, gs->Bullet_Counter);
            HASHONE(FH_paring_counter, gs->paring_counter);
            HASHONE(FH_Present_Mode, gs->Present_Mode);
            HASHONE(FH_VS_Stage, gs->VS_Stage);
            HASHONE(FH_SLOW_timer, gs->SLOW_timer);
            HASHONE(FH_SLOW_flag, gs->SLOW_flag);
            HASHONE(FH_EXE_flag, gs->EXE_flag);
            HASHONE(FH_super_arts, gs->super_arts);
            HASHONE(FH_piyori_type, gs->piyori_type);
            HASHONE(FH_Max_vitality, gs->Max_vitality);
            {
                extern u8 chainex_check[2][36];
                uint32_t _s = djb2_init();
                _s = djb2_update_mem(_s, (const uint8_t*)chainex_check, sizeof(chainex_check));
                fh[FH_chainex_check] = _s;
            }
            {
                extern u16 Color7[2];
                extern s8 ca_check_flag;
                uint32_t _s = djb2_init();
                _s = djb2_update_mem(_s, (const uint8_t*)Color7, sizeof(Color7));
                fh[FH_Color7] = _s;
                _s = djb2_init();
                _s = djb2_update_mem(_s, (const uint8_t*)&ca_check_flag, sizeof(ca_check_flag));
                fh[FH_ca_check_flag] = _s;
            }
            {
                extern u32 spmv_ng_save[2];
                uint32_t _s = djb2_init();
                _s = djb2_update_mem(_s, (const uint8_t*)spmv_ng_save, sizeof(spmv_ng_save));
                fh[FH_spmv_ng_save] = _s;
            }
#undef HASHONE
        }
        return h;
    }
    return 0;
}

/* save_state — Gekko-callback entry. Splits responsibilities:
 *   1. save_current_state writes a full State into a scratch buffer for the
 *      checksum/dump infrastructure (ring buffer, per-section checksums,
 *      per-field hashes). This bookkeeping is unaffected by sparse-save.
 *   2. We then re-encode that scratch into Gekko's actual save buffer in
 *      either sparse or full format, depending on the runtime kill switch
 *      AND the active-slot count.
 *
 * Two paths the sparse encoding is bypassed:
 *   - kill switch off → full state.
 *   - active count > SPARSE_CEILING_SLOTS → backend_logf warning + full
 *     state (a "soft" overflow that's safe but blows past the ceiling
 *     Gekko's ring is sized for; user MUST raise SPARSE_CEILING_SLOTS or
 *     accept the rollback-buffer pressure for that frame). */
static unsigned char s_save_scratch[sizeof(State)];

void save_state(const GekkoGameEvent* event) {
    /* (1) Always run the legacy path on a scratch State so the checksum +
     * dump ring stays consistent regardless of sparse mode. */
    State* scratch = (State*)s_save_scratch;
    uint32_t h = save_current_state(scratch, event->data.save.frame);
    *event->data.save.checksum = h;

    /* (2) Encode into Gekko's buffer. */
    unsigned char* dst = event->data.save.state;
    unsigned int dst_len;

    if (!s_sparse_effect_save_enabled) {
        dst_len = pack_full_state(dst, scratch);
    } else {
        /* Active count = EFFECT_MAX - frwctr (live global). frwctr is
         * already gathered into scratch->es by save_current_state, but
         * reading the live value avoids a second indirection. */
        int active = EFFECT_MAX - (int)frwctr;
        if (active < 0 || active > SPARSE_CEILING_SLOTS) {
            /* Safety net: fall back to full save for this frame. We log
             * once per overrun threshold so a sustained busy stage doesn't
             * spam serial. backend_logf is the same channel as the rest
             * of the netplay diagnostics. */
            static int last_warned_active = -1;
            if (active != last_warned_active) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[netplay] sparse-save: active=%d exceeds ceiling %d "
                            "for frame %d; falling back to full state. "
                            "Raise SPARSE_CEILING_SLOTS in game_state.h.",
                            active, SPARSE_CEILING_SLOTS, event->data.save.frame);
                last_warned_active = active;
            }
            dst_len = pack_full_state(dst, scratch);
        } else {
            dst_len = pack_sparse_state(dst, &scratch->gs);
        }
    }
    *event->data.save.state_len = dst_len;
}

void load_state(const State* src) {
    const GameState* gs = &src->gs;
    GameState_Load(gs);

    const EffectState* es = &src->es;
    SDL_copya(frw, es->frw);
    SDL_copya(exec_tm, es->exec_tm);
    SDL_copya(frwque, es->frwque);
    SDL_copya(head_ix, es->head_ix);
    SDL_copya(tail_ix, es->tail_ix);
    frwctr = es->frwctr;
    frwctr_min = es->frwctr_min;
}

/* load_state_from_event — dispatch by buffer size:
 *   == sizeof(State)               → legacy full-state buffer.
 *   else (matches sparse layout)   → sparse buffer.
 *
 * No-collision proof. The sparse format size is
 *   sizeof(GameState) + SPARSE_HEADER_BYTES + active_count * SPARSE_FRW_SLOT_BYTES.
 * Equality with sizeof(State) = sizeof(GameState) + sizeof(EffectState)
 * would require SPARSE_HEADER_BYTES + active_count * SPARSE_FRW_SLOT_BYTES
 * == sizeof(EffectState). Substituting EFFECT_MAX=128 and the layout:
 *   sizeof(EffectState) = (2+2+16+16+16+256) + 128 * SPARSE_FRW_SLOT_BYTES
 *                       = 308 + 128 * SPARSE_FRW_SLOT_BYTES.
 * So we need 328 + N*1792 = 308 + 128*1792 → N*1792 = 229356 → N ≈ 127.99,
 * not an integer for any allowed active_count in [0, 128]. The same math
 * holds on 64-bit (1792 → 3584) by symmetry. Format detection is unambiguous
 * within a single peer's bitness. */
void load_state_from_event(const GekkoGameEvent* event) {
    const unsigned char* src = event->data.load.state;
    unsigned int len = event->data.load.state_len;

    if (len == sizeof(State)) {
        load_state((const State*)src);
        return;
    }

    /* GameState restoration first (always at offset 0). */
    GameState_Load((const GameState*)src);

    if (!unpack_sparse_state(src, len)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[netplay] load_state_from_event: sparse buffer rejected "
                     "(state_len=%u, expected sizeof(State)=%zu or sparse layout). "
                     "Effect pool left untouched; rollback may diverge.",
                     len, sizeof(State));
    }
}

/**
 * @brief Dump desync diagnostic data to the states/ directory.
 *
 * Writes:
 *  1. states/desync_F<frame>.txt — per-section checksums + ring buffer.
 *  2. states/desync_F<frame>_plw0.bin / _plw1.bin — sanitized PLW snapshots.
 *  3. states/desync_F<frame>_state.bin — full State snapshot.
 *
 * Promoted out of #if DEBUG 2026-04-26 — telemetry builds also dump on
 * desync. The ring buffers feeding this dump are populated unconditionally
 * inside save_current_state(); the dump itself only fires once per session
 * (right before the soft-reset back to title), so it has no per-frame cost.
 */
void dump_desync_state(int frame, uint32_t local_checksum, uint32_t remote_checksum) {
    const int slot = frame % STATE_BUFFER_MAX;

    /* Ensure states/ exists. Runtime CWD is the install bindir on
     * MiSTer, so relative path lands at /media/fat/games/3s-arm/states/. */
    SDL_CreateDirectory("states");

    /* Per-process suffix so two peers on the same filesystem (Mac↔Mac
     * loopback test) don't race on the same filenames. getpid() is
     * unique between the two instances. */
    const int proc_id = (int)getpid();

    char path[256];
    SDL_snprintf(path, sizeof(path), "states/desync_F%d_pid%d.txt", frame, proc_id);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "=== DESYNC DETECTED ===\n");
        fprintf(f, "Frame:           %d\n", frame);
        fprintf(f, "Local checksum:  0x%08x\n", local_checksum);
        fprintf(f, "Remote checksum: 0x%08x\n", remote_checksum);
        fprintf(f, "STATE_BUFFER_MAX: %d\n", STATE_BUFFER_MAX);
        fprintf(f, "sizeof(PLW): %zu  sizeof(State): %zu\n\n", sizeof(PLW), sizeof(State));

        /* chainex_check dump — desync investigation 2026-04-24. If two peers'
         * chainex_check differs at frame of desync, candidate 0a is confirmed. */
        {
            extern u8 chainex_check[2][36];
            fprintf(f, "--- chainex_check[2][36] at desync frame ---\n");
            for (int p = 0; p < 2; p++) {
                fprintf(f, "  chainex_check[%d] = ", p);
                for (int j = 0; j < 36; j++) {
                    fprintf(f, "%02x ", (unsigned)chainex_check[p][j]);
                }
                fprintf(f, "\n");
            }
            fprintf(f, "\n");
        }

        /* RNG call counters. Peer with higher count = extra consumer. */
        {
            extern u32 g_random_32_calls;
            extern u32 g_random_16_calls;
            extern u32 g_random_32_ex_calls;
            extern u32 g_random_16_ex_calls;
            fprintf(f, "--- RNG call counters ---\n");
            fprintf(f, "  random_32    calls = %u\n", g_random_32_calls);
            fprintf(f, "  random_16    calls = %u\n", g_random_16_calls);
            fprintf(f, "  random_32_ex calls = %u\n", g_random_32_ex_calls);
            fprintf(f, "  random_16_ex calls = %u\n", g_random_16_ex_calls);
            fprintf(f, "\n");
        }
        /* Per-field hash ring buffer — for diffing two peer dumps. The
         * first field where the two peers' hashes differ (at the oldest
         * frame in the window) is the divergence source. */
        fprintf(f, "--- Per-field hashes (ring buffer, 20 frames) ---\n");
        fprintf(f, "%8s", "frame");
        for (int k = 0; k < FH_COUNT; k++) {
            fprintf(f, " %18s", FH_NAMES[k]);
        }
        fprintf(f, "\n");
        for (int i = 0; i < STATE_BUFFER_MAX; i++) {
            int f_idx = (frame - STATE_BUFFER_MAX + 1 + i);
            if (f_idx < 0) continue;
            int s2 = f_idx % STATE_BUFFER_MAX;
            fprintf(f, "%8d", f_idx);
            for (int k = 0; k < FH_COUNT; k++) {
                fprintf(f, " 0x%08x%8s", saved_field_hashes[s2][k], "");
            }
            fprintf(f, "%s\n", (f_idx == frame) ? " <== DESYNC" : "");
        }
        fprintf(f, "\n");

        fprintf(f, "--- Per-section checksums (ring buffer) ---\n");
        fprintf(f, "%8s  %10s  %10s  %10s  %10s  %10s  %10s  %10s\n",
                "frame", "combined", "plw0", "plw1", "globals", "bg", "tasks", "effects");
        const int window = STATE_BUFFER_MAX;
        for (int i = 0; i < window; i++) {
            int f_idx = (frame - window + 1 + i);
            if (f_idx < 0)
                continue;
            int s = f_idx % STATE_BUFFER_MAX;
            const SectionedChecksum* sc = &saved_section_checksums[s];
            const char* marker = (f_idx == frame) ? " <== DESYNC" : "";
            fprintf(f,
                    "%8d  0x%08x  0x%08x  0x%08x  0x%08x  0x%08x  0x%08x  0x%08x%s\n",
                    f_idx, sc->combined, sc->plw0, sc->plw1, sc->globals,
                    sc->bg, sc->tasks, sc->effects, marker);
        }
        fclose(f);
        SDL_Log("[desync] Wrote section checksums to %s", path);
    } else {
        SDL_Log("[desync] ERROR: Could not open %s for writing", path);
    }

    for (int p = 0; p < 2; p++) {
        SDL_snprintf(path, sizeof(path), "states/desync_F%d_pid%d_plw%d.bin", frame, proc_id, p);
        f = fopen(path, "wb");
        if (f) {
            fwrite(&saved_plw_scratch[slot][p], sizeof(PLW), 1, f);
            fclose(f);
            SDL_Log("[desync] Wrote PLW[%d] snapshot (%zu bytes) to %s", p, sizeof(PLW), path);
        }
    }

    SDL_snprintf(path, sizeof(path), "states/desync_F%d_pid%d_state.bin", frame, proc_id);
    f = fopen(path, "wb");
    if (f) {
        fwrite(&state_buffer[slot], sizeof(State), 1, f);
        fclose(f);
        SDL_Log("[desync] Wrote full State (%zu bytes) to %s", sizeof(State), path);
    }
}

#ifdef ENABLE_NETPLAY_TESTS
/* === Test-only trampolines for sparse-save unit tests ===========
 *
 * test_sparse_effect_save.c links against the game binary, but the
 * sparse pack/unpack helpers are file-static. These trampolines expose
 * them for the round-trip parity tests without leaking the wire format
 * into the public header. Gated by the same ENABLE_NETPLAY_TESTS macro
 * as the other Phase 6 test harnesses (test_event_queue, test_room_code,
 * test_stun_mock). */
unsigned int Netplay_Test_PackSparseState(unsigned char* out_buf,
                                          const GameState* gs_src);
bool Netplay_Test_UnpackSparseState(const unsigned char* in_buf,
                                    unsigned int in_len);

unsigned int Netplay_Test_PackSparseState(unsigned char* out_buf,
                                          const GameState* gs_src) {
    return pack_sparse_state(out_buf, gs_src);
}

bool Netplay_Test_UnpackSparseState(const unsigned char* in_buf,
                                    unsigned int in_len) {
    return unpack_sparse_state(in_buf, in_len);
}
#endif
