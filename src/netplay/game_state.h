#ifndef NETPLAY_GAME_STATE_H
#define NETPLAY_GAME_STATE_H

#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/cmb_win.h"
#include "sf33rd/Source/Game/engine/grade.h"
#include "sf33rd/Source/Game/engine/plcnt.h"
#include "sf33rd/Source/Game/engine/spgauge.h"
#include "sf33rd/Source/Game/engine/stun.h"
#include "sf33rd/Source/Game/engine/vital.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/select_timer.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "structs.h"
#include "types.h"

#include <stdint.h>

typedef struct EffectState {
    s16 frwctr;
    s16 frwctr_min;
    s16 head_ix[8];
    s16 tail_ix[8];
    s16 exec_tm[8];
    uintptr_t frw[EFFECT_MAX][448];
    s16 frwque[EFFECT_MAX];
} EffectState;

typedef struct GameState {
    bool Scene_Cut;
    bool Time_Over;

    s8 round_timer;
    s8 flash_timer;
    s8 flash_r_num;
    s8 flash_col;
    s8 math_counter_hi;
    s8 math_counter_low;
    u8 counter_color;
    bool mugen_flag;
    s8 hoji_counter;

    SelectTimerState select_timer_state;

    u8 Order[148];
    u8 Order_Timer[148];
    u8 Order_Dir[148];
    u32 Score[2][3];
    u32 Complete_Bonus;
    u32 Stock_Score[2];
    u32 Vital_Bonus[2];
    u32 Time_Bonus[2];
    u32 Stage_Stock_Score[2];
    u32 Bonus_Score;
    u32 Final_Bonus_Score;
    u32 WGJ_Score;
    u32 Bonus_Score_Plus;
    u32 Perfect_Bonus[2];
    u32 Keep_Score[2];
    u32 Disp_Score_Buff[2];
    s8 Winner_id;
    s8 Loser_id;
    s8 Break_Into;
    u8 My_char[2];
    u8 Allow_a_battle_f;
    u8 Round_num;
    s8 Complete_Judgement;
    s8 Fade_Flag;
    s8 Super_Arts[2];
    s8 Forbid_Break;
    s8 Request_Break[2];
    s8 Continue_Count[2];
    s8 Counter_hi;
    s8 Counter_low;
    s16 Unit_Of_Timer;
    s8 Select_Timer;
    s8 Cursor_X[2];
    s8 Cursor_Y[2];
    s8 Cursor_Y_Pos[2][4];
    s8 Cursor_Timer[2];
    s8 Time_Stop;
    s8 Suicide[8];
    s8 Complete_Face;
    u8 Play_Type;
    s16 Sel_PL_Complete[2];
    s8 New_Challenger;
    u8 S_No[4];
    s8 Select_Start[2];

    s8 request_message;
    s8 judge_flag;
    s8 WINNER;
    s8 LOSER;
    s8 Champion;
    s8 Fade_Half_Flag;
    s8 Reserve_Cut;
    s8 Perfect_Flag;
    s8 Next_Step;
    s8 Switch_Type;
    s8 Cover_Timer;
    s8 Personal_Timer[2];
    s8 Request_E_No;
    s8 Request_G_No;
    u8 Present_Rank[2];
    s8 Best_Grade[2];
    s8 Demo_Type;
    s8 Rank_Type;
    s8 Flash_Sign[2];
    s8 Flash_Rank_Time;
    s8 Flash_Rank_Interval;
    s32 Ranking_X;
    s8 Rank;
    s8 Rank_X;
    s8 E_07_Flag[2];
    s8 Complete_Victory;
    s8 Demo_Flag;
    s32 Next_Demo;
    s8 Demo_PL_Index;
    s8 Demo_Stage_Index;
    s8 Face_MV_Request;
    s8 Face_Move;
    s8 Player_id;
    s8 Last_Player_id;
    s8 Player_Number;
    u8 DENJIN_Term[2];
    s8 Rapid_No[2][4];
    s8 COM_id;
    s8 EM_id;
    s8 Select_Status[2];
    s8 Select_Demo_Index;
    u8 Country;
    s8 Demo_Time_Stop;
    s8 Combo_Speed[2];
    s8 Exec_Wipe;
    s8 Passive_Mode;
    s8 Passive_Flag[2];
    s8 Flip_Flag[2];
    s8 Lie_Flag[2];
    s8 Counter_Attack[2];
    s8 Attack_Flag[2];
    s8 Limited_Flag[2];
    s8 Shell_Ignore_Timer[2];
    s8 Event_Judge_Gals;
    u8 EJG_index[4];
    s8 Guard_Flag[2];
    s8 Pierce_Menu[2];
    s8 Face_MV_Time;
    s8 Before_Jump[2];
    s8 Stop_Combo;
    u8 Stock_Hit_Flag[2];
    s8 Rolling_Flag[2];
    u8 Continue_Coin[2];
    s8 Ignore_Entry[2];
    s8 Slide_Type;
    s8 Moving_Plate[2];
    s8 Naming_Cut[2];
    s8 Moving_Plate_Counter[2];
    s8 Player_Color[2];
    s8 PP_Priority[2][3];
    s8 OK_Priority[2];
    u8 Stock_My_char[2];
    s8 Stock_Player_Color[2];
    s8 Music_Fade;
    s8 Stop_SG;
    s8 Operator_Status[2];
    s8 Round_Operator[2];
    s8 another_bg[2];
    s8 Last_Super_Arts[2];
    s8 Last_My_char[2];
    s8 Continue_Menu[2];
    s8 Timer_Freeze;
    u8 Type_of_Attack[2];
    s8 Standing_Timer[2];
    s8 Before_Look[2];
    s8 Attack_Count_No0[2];
    s8 Standing_Master_Timer[2];
    s8 PB_Music_Off;
    s8 No_Death;
    s8 Flash_MT[2];
    s8 Squat_Timer[2];
    s8 Squat_Master_Timer[2];
    s8 Turn_Over[2];
    s8 Turn_Over_Timer[2];
    s8 Jump_Pass_Timer[2][4];
    s8 sa_gauge_flash[2];
    s8 Receive_Flag[2];
    s8 Disposal_Again[2];
    s8 BGM_Vol;
    u8 Used_char[2];
    s8 Break_Com[2][20];
    s8 aiuchi_flag;
    u8 paring_counter[2];
    u8 paring_bonus_r[2];
    u8 paring_ctr_vs[2][2];
    u8 paring_ctr_ori[2];
    u8 Attack_Count_Buff[2][4];
    u8 Attack_Count_Index[2];
    u8 CC_Value[2];
    u8 Continue_Coin2[2];
    u8 Weak_PL;
    u8 Bullet_No[2];
    u8 Bullet_Counter[2];
    u8 Final_Result_id;
    s8 Disp_Win_Name;
    u8 Perfect_Counter[2];
    u8 Straight_Counter[2];
    u8 Appear_Q;
    s8 Cut_Scroll;
    s8 Break_Into_CPU;
    s8 ID_of_Face[3][8];
    s8 Cursor_Move[2];
    s8 Auto_Cursor[2];
    s8 Auto_No[2];
    s8 Auto_Index[2];
    s8 Auto_Timer[2];
    s8 Explosion;
    s8 Introduce_Break_Into[2];
    s8 gouki_wins;
    s8 EM_Rank;
    s8 Disp_PERFECT;
    s8 Escape_SS;
    s8 Deley_Shot_No[2];
    s8 Deley_Shot_Timer[2];
    s8 Lost_Round[2];
    s8 Super_Arts_Finish[2];
    s8 Stage_SA_Finish[2];
    s8 Perfect_Finish[2];
    s8 Cheap_Finish[2];
    s8 Last_My_char2[2];
    s8 gouki_app;
    s8 Bonus_Game_Complete;
    u8 Get_Demo_Index;
    u8 Combo_Demo_Flag;
    u8 Stage_Continue[2];
    u8 Pause_Hit_Marks;
    u8 Extra_Break;
    u8 Shin_Gouki_BGM;
    s8 Stage_Lost_Round[2];
    s8 Stage_Perfect_Finish[2];
    s8 Stage_Cheap_Finish[2];
    s8 EXE_obroll;
    u8 End_PL;
    s8 Stock_Com_Arts[2];
    u8 PB_Status;
    u8 Flip_Counter[2];
    u8 Stage_Time_Finish[2];
    u8 Bonus_Type;
    s8 Completion_Bonus[2][2];
    s8 ichikannkei;
    u8 Plate_Disposal_No[2][3];
    u8 SO_No[2];
    u8 Disp_Command_Name[2][3];
    u8 SC_No[4];
    u8 BGM_No[2];
    u8 BGM_Timer[2];
    u8 EM_List[2][2];
    s8 Sel_EM_Complete[2];
    s8 Temporary_EM[2];
    s8 OK_Moving_SA_Plate[2];
    u8 Battle_Q[2];
    u8 EM_History[2][10];
    u8 GO_No[4];
    u8 Aborigine;
    u8 Continue_Count_Down[2];
    u8 WGJ_Target;
    u8 EM_Candidate[2][2][10];
    s8 Last_Selected_EM[2];
    u8 Q_Country;
    u8 Continue_Cut[2];
    u8 Introduce_Boss[2][2];
    u8 Final_Play_Type[2];
    s8 Rank_In[2][4];
    s8 Request_Disp_Rank[2][4];
    u8 Reset_Timer[2];
    u8 bbbs_type;
    u8 Straight_Flag[2];
    u8 kakushi_ix;
    u8 kakushi_op;
    u8 RO_backup[2];
    u8 PT_backup;
    u8 E_Number[2][4];
    u8 E_No[4];
    u8 C_No[4];
    u8 G_No[4];
    u8 D_No[4];
    u8 M_No[4];
    u8 Exit_No;
    u8 SP_No[2][4];
    u8 Face_No[2];
    s8 Stop_Cursor[2];
    u8 Training_Index;
    u8 Connect_Status;
    u8 Menu_Suicide[4];
    u8 Game_pause;
    u8 Game_difficulty;
    u8 Pause;
    u8 Pause_ID;
    u8 Exit_Menu;
    u8 Conclusion_Flag;
    u8 CP_No[2][4];
    u8 CP_Index[2][8];
    u8 Gap_Timer;
    u8 Message_Suicide[4];
    u8 Disp_Cockpit;
    s8 Select_Arts[2];
    u8 Lamp_No;
    u8 Lamp_Index;
    u8 Lamp_Color;
    u8 Stop_Update_Score;
    u8 test_flag;
    u8 ixbfw_cut;
    u8 Cont_No[4];
    u8 PL_Wins[2];
    u8 Fade_R_No0;
    u8 Fade_R_No1;
    u8 Conclusion_Type;
    u8 win_type[2][4];
    u8 message_index;
    u8 F_No0[2];
    u8 F_No1[2];
    u8 F_No2[2];
    u8 F_No3[2];
    u8 keep_condition[11];
    s8 Check_Buff[4][2][12];
    s8 Convert_Buff[4][2][12];
    u8 Unsubstantial_BG[4];
    s8 Menu_Cursor_X[2];
    s8 Menu_Cursor_Y[2];
    u8 Replay_Status[2];
    u8 Disappear_LOGO;
    u8 count_end;
    u8 Play_Game;
    s8 Menu_Cursor_Move;
    u8 flash_win_type[2][4];
    u8 sync_win_type[2][4];
    ModeType Mode_Type;
    s8 Menu_Page;
    s8 Menu_Max;
    u8 reset_NG_flag;
    s8 VS_Stage;
    u8 Present_Mode;
    u8 Play_Mode;
    u8 Page_Max;
    u8 Direction_Working[6];
    s8 Vital_Handicap[6][2];
    s8 Cursor_Limit[2];
    u8 Synchro_No;
    s8 SA_shadow_on;
    u8 Pause_Down;
    u8 Training_ID;
    u8 Disp_Attack_Data;
    u8 Disp_Input_History;
    u8 Disp_Frame_Data;
    u8 Record_Data_Tr;
    u8 End_Training;
    s8 Menu_Page_Buff;
    u8 Reset_Bootrom;
    u8 Decide_ID;
    s8 Training_Cursor;
    s8 Lag_Timer;
    u8 CPU_Time_Lag[2];
    u8 Forbid_Reset;
    u8 CPU_Rec[2];
    u8 Pause_Type;
    u16 Game_timer;
    s16 Control_Time;
    s16 Time_in_Time;
    s16 Round_Level;
    u16 Round_Result;
    u16 Fade_Number;
    s16 G_Timer;
    s16 D_Timer;
    s16 Rank_Pos_X;
    s16 Rank_Pos_Y;
    s16 E_Timer;
    s16 F_Timer[2];
    s16 ENTRY_X;
    s16 C_Timer;
    s16 S_Timer;
    s16 Flash_Complete[2];
    s16 Sel_Arts_Complete[2];
    s16 Arts_Y[2];
    s16 Move_Super_Arts[2];
    s16 Battle_Country;
    s16 Face_Status;
    s16 ID;
    s8 ID2;
    s16 mes_already;
    s16 Timer_00[2];
    s16 Timer_01[2];
    s16 PL_Distance[2];
    s16 Area_Number[2];
    u16 Lever_Buff[2];
    u16 Lever_Pool[2];
    s16 Tech_Index[2];
    s16 Random_ix16;
    s16 Random_ix32;
    s16 M_Timer;
    s16 VS_Tech[2];
    u16 Guard_Type[2];
    s16 Separate_Area[2][3];
    u16 Free_Lever[2];
    s16 Term_No[2];
    s16 Com_Width_Data[2];
    u16 Lever_Squat[2];
    u16 M_Lv[2];
    s16 Insert_Y;
    s16 scr_req_x;
    s16 scr_req_y;
    s16 zoom_req_flag_old;
    s16 zoom_request_flag;
    s16 zoom_request_level;
    s16 Last_Selected_ID;
    s16 Last_Called_SE;
    s16 VS_Index[2];
    s16 Rapid_Index[2];
    s16 Shell_Separate_Area[2][3];
    s16 Attack_Counter[2];
    s16 Last_Attack_Counter[2];
    u16 Pattern_Index[2];
    s16 Com_Color_Shot;
    u16 Resume_Lever[2][20];
    u16 players_timer;
    u16 Lever_Store[2][3];
    s16 Return_CP_No[2];
    s16 Return_CP_Index[2];
    s16 Return_Pattern_Index[2];
    u16 Lever_LR[2];
    s16 Last_Eftype[2];
    u16 DENJIN_No[2];
    u16 SC_Personal_Time[2];
    s16 Guard_Counter[2];
    s16 Limit_Time;
    s16 Last_Pattern_Index[2];
    s16 Random_ix16_ex;
    s16 Random_ix32_ex;
    s16 DE_X[2];
    s16 Exit_Timer;
    s16 Max_vitality;
    s16 Bonus_Game_Flag;
    s16 Bonus_Game_Work;
    s16 Bonus_Game_result;
    s16 Stock_Bonus_Game_Result;
    s16 bs_scrrrl[2][2];
    s16 Bonus_Stage_RNO[4];
    s16 Bonus_Stage_Level;
    s16 Bonus_Stage_Tix;
    s16 Bonus_Game_ex_result;
    s16 Stock_Com_Color[2];
    s16 bs2_floor[3];
    s16 bs2_hosei[3];
    s16 bs2_current_damage;
    u16 Win_Record[2];
    u16 Stock_Win_Record[2];
    u16 WGJ_Win;
    s16 Target_BG_X[6];
    s16 Offset_BG_X[6];
    u16 Result_Timer[2];
    s16 scrl;
    s16 scrr;
    u16 vital_stop_flag[2];
    u16 gauge_stop_flag[2];
    s16 Lamp_Timer;
    s16 Cont_Timer;
    s16 Plate_X[2][3];
    s16 Plate_Y[2][3];
    u16 Demo_Timer[2];
    u16 Condense_Buff[2];
    u16 Keep_Grade[2];
    u16 IO_Result;
    u16 VS_Win_Record[2];
    u16 PLsw[2][2];
    u16 plsw_00[2];
    u16 plsw_01[2];
    s16 Flash_Synchro;
    s16 Synchro_Level;
    s16 Random_ix16_com;
    s16 Random_ix32_com;
    s16 Random_ix16_ex_com;
    s16 Random_ix32_ex_com;
    s16 Random_ix16_bg;
    s16 Opening_Now;
    struct _TASK task[11];

    // plcnt

    PLW plw[2];
    ComboType combo_type[2];
    ComboType remake_power[2];
    ZanzouTableEntry zanzou_table[2][48];
    SA_WORK super_arts[2];
    PiyoriType piyori_type[2];
    AppearanceType appear_type;
    s16 pcon_rno[4];
    bool round_slow_flag;
    bool pcon_dp_flag;
    u8 win_sp_flag;
    bool dead_voice_flag;
    UNK_1 rambod[2];
    UNK_2 ramhan[2];
    u16 vital_inc_timer;
    u16 vital_dec_timer;
    s16 sag_inc_timer[2];

    // cmd_data

    WORK_CP wcp[2];
    T_PL_LVR t_pl_lvr[2];
    WAZA_WORK waza_work[2][56];

    // cmb_win

    CMST_BUFF cmst_buff[2][5];
    s16 old_cmb_flag[2];
    s8 cmb_stock[2];
    s8 first_attack;
    s8 rever_attack[2];
    s8 paring_attack[2];
    s8 bonus_pts[2];
    s16 hit_num;
    u8 sa_kind;
    u8 end_flag[2];
    s16 calc_hit[2][10];
    s16 score_calc[2][12];
    s8 cmb_all_stock[1];
    s8 sarts_finish_flag[2];
    s8 last_hit_time;
    s8 cmb_calc_now[2];
    u8 cst_read[2];
    u8 cst_write[2];

    // bg

    BG bg_w;
    u16 Screen_Switch;
    u16 Screen_Switch_Buffer;
    u8 rw_num;
    u8 rw_bg_flag[4];
    u8 tokusyu_stage;
    s32 rw_gbix[13];
    s8 stage_flash;
    s8 stage_ftimer;
    s32 yang_ix_plus;
    s8 yang_ix;
    s8 yang_timer;
    u8 ending_flag;
    BackgroundParameters end_prm[8];
    u8 gouki_end_gbix[16];
    const u32* rw3col_ptr;
    u8 bg_disp_off;
    s32 bgPalCodeOffset[8];
    RW_DATA rw_dat[20];

    // charset

    u16 att_req;

    // slowf

    s16 SLOW_timer;
    s16 SLOW_flag;
    s16 EXE_flag;

    // grade

    JudgeGals judge_gals[2];
    JudgeCom judge_com[2];
    s16 last_judge_dada[2][5];
    GradeFinalData judge_final[2][2];
    GradeData judge_item[2][2];
    u8 ji_sat[2][384];

    // spgauge

    s8 Old_Stop_SG;
    s8 Exec_Wipe_F;
    s8 time_clear[2];
    s16 spg_number;
    s16 spg_work;
    s16 spg_offset;
    s8 time_num;
    s8 time_timer;
    s8 time_flag[2];
    s16 col;
    s8 time_operate[2];
    s8 sast_now[2];
    s8 max2[2];
    s8 max_rno2[2];
    SPG_DAT spg_dat[2];

    // stun

    SDAT sdat[2];

    // vital

    VIT vit[2];

    // win_pl

    s16 win_free[2];
    s16 win_rno[2];
    s16 poison_flag[2];

    // ta_sub

    s16 eff_hit_flag[11];

    // sc_sub

    u8 FadeLimit;
    u8 WipeLimit;

    // appear

    s8 Appear_car_stop[2];
    s8 Appear_hv[2];
    s8 Appear_free[2];
    s8 Appear_flag[2];
    s16 app_counter[2];
    s16 appear_work[2];
    s16 Appear_end;

    // bg_data

    s16 y_sitei_pos;
    u8 y_sitei_flag;
    u8 c_number;
    u8 c_kakikae;
    u8 g_number[2];
    u8 g_kakikae[2];
    u8 nosekae;
    s16 scrn_adgjust_y;
    s16 scrn_adgjust_x;
    u16 zoom_add;
    s16 ls_cnt1;
    s8 bg_app;
    s8 sa_pa_flag;
    s8 aku_flag;
    s8 seraph_flag;
    s8 akebono_flag;
    MVXY bg_mvxy;
    s16 chase_time_y;
    s16 chase_time_x;
    s16 chase_y;
    s16 chase_x;
    s8 demo_car_flag[2];
    Ideal_W ideal_w;
    s8 bg_app_stop;
    s16 bg_stop;
    s16 base_y_pos;
    s32 etcBgPalCnvTable[7];
    u8 etcBgGixCnvTable[7][16];

    // eff56

    const u8* ci_pointer;
    u8 ci_col;
    u8 ci_timer;

    // effb2

    s16 rf_b2_flag;
    s16 b2_curr_no;

    // effb8

    s16 test_pl_no;
    s16 test_mes_no;
    s16 test_in;
    s16 old_mes_no2;
    s16 old_mes_no3;
    s16 old_mes_no_pl;
    s16 mes_timer;

    // work_sys — rollback-critical system globals

    BG_POS bg_pos[8];
    FM_POS fm_pos[8];
    BackgroundParameters bg_prm[8];
    u32 system_timer;
    s8 Gill_Appear_Flag;

    // plcnt — DIP switch combat config

    char cmd_sel[2];
    char no_sa[2];

    // sc_sub

    s16 Hnc_Num;

    // ending

    END_W end_w;

    // work_sys (extension)

    f32 scr_sc;
    s32 X_Adjust;
    s32 Y_Adjust;

    // Additional globals

    MTX BgMATRIX[9];
    struct _VM_W vm_w;
    _EXTRA_OPTION ck_ex_option;
    s32 X_Adjust_Buff[3];
    s32 Y_Adjust_Buff[3];

    /* EX-SA chain-ex gating flag. Per-player-per-gauge-index flag set when
     * an EX-SA chain fires (pls03.c:169,211,276) and cleared on many SA
     * state transitions (plcnt.c:1418, called from ~20 sites in pls00.c).
     * Previously a file-static in sysdir.c that escaped rollback, causing
     * desync on Mac↔Mac loopback + latency after ~15s of play. Added to
     * GameState on 2026-04-24 so save/restore covers it. */
    u8 chainex_check[2][36];
} GameState;

typedef struct State {
    GameState gs;
    EffectState es;
} State;

/* === Sparse effect-pool save (Option A) ===========================
 *
 * The effect work pool (EffectState.frw[128][448]) is 229,376 bytes —
 * 93% of sizeof(State) on 32-bit. Empirical telemetry on stock MiSTer
 * (Pk<N> overlay) recorded a peak of 57 simultaneously-active slots
 * across full-roster super-art-heavy sessions. With 71 of 128 slots
 * idle on average and the canonical "inactive" predicate (`be_flag==0`,
 * with linked-list head_ix[8] as ground truth), we can serialize only
 * the active slots and reconstruct the rest on load.
 *
 * SPARSE_CEILING_SLOTS  — worst-case # of active slots Gekko's ring
 * sizes for. 82 ≈ 1.44 × empirical peak. At save time, an active count
 * > SPARSE_CEILING_SLOTS triggers the full-state fallback path (see
 * save_current_state in game_state.c).
 *
 * SPARSE_FRW_SLOT_BYTES — per-slot payload size (matches the inner
 * uintptr_t[448] dimension on either bitness). Wire-stable on a single
 * peer; cross-arch netplay isn't supported so 32 vs 64 doesn't matter
 * to interop.
 *
 * SPARSE_HEADER_BYTES   — fixed-size prefix: scalar EffectState
 * fields + an active_mask + active_count.
 *
 * SPARSE_CEILING_BYTES  — buffer ceiling (sizeof(GameState) +
 * SPARSE_HEADER_BYTES + 82 × SPARSE_FRW_SLOT_BYTES). Used as
 * gekko_start config.state_size so the rollback ring buffer shrinks
 * proportionally. Each Gekko save still reports its own variable
 * state_len via GekkoSave.state_len, so the ceiling is just a max.
 */
/* Phase 5 hygiene item 5 (docs/plan-frame-data-harness.md): this was
 * bumped 82 -> 100 by cce9095a ("chore(netplay): bump
 * SPARSE_CEILING_SLOTS 82 -> 100"), whose own commit message says the
 * motivation was "headroom for the in-flight lobby-mvp state
 * additions". That lobby feature was abandoned — netplay.c:272-273
 * ("Dormant since the RmlUi lobby UI was removed — no code path sets
 * s_lobby_session = true anymore") confirms no lobby-derived GameState
 * fields ever landed that would grow the effect-pool active-slot peak.
 * The frame-data overlay work this bump got squashed alongside
 * (a386e057) only added a 1-byte Disp_Frame_Data scalar to GameState,
 * not additional concurrent effect-pool slots — it doesn't motivate 100
 * either. Reverted to the original empirically-derived 82 (see comment
 * above: peak observed 57, 82 ≈ 1.44 × that peak). */
#define SPARSE_CEILING_SLOTS 82
#define SPARSE_FRW_SLOT_BYTES (sizeof(uintptr_t) * 448)
/* Fixed-size header on the wire. Lays out frwctr/frwctr_min,
 * head_ix/tail_ix/exec_tm/frwque, active_mask[16] (128-bit), and
 * active_count (u16). Layout keeps strict 16-byte mask alignment
 * after the s16 arrays so reading on either side of a save/load is
 * trivially memcpy. The exact byte breakdown:
 *   2 (frwctr) + 2 (frwctr_min)                         =   4
 *   2*8 (head_ix) + 2*8 (tail_ix) + 2*8 (exec_tm)       =  48
 *   2*128 (frwque)                                       = 256
 *   16 (active_mask) + 2 (active_count) + 2 (pad)       =  20
 *   total                                                = 328
 */
#define SPARSE_HEADER_BYTES 328
#define SPARSE_CEILING_BYTES (sizeof(GameState) + SPARSE_HEADER_BYTES + \
                              SPARSE_CEILING_SLOTS * SPARSE_FRW_SLOT_BYTES)

void GameState_Save(GameState* dst);
void GameState_Load(const GameState* src);

// Rollback save/load public API (Track A Phase 3). These mirror 3sxtra's
// signatures in /tmp/3sxtra/src/include/game_state.h:787-800.
struct GekkoGameEvent;
uint32_t save_current_state(void* buffer, int frame);
void save_state(const struct GekkoGameEvent* event);
void load_state(const struct State* src);
void load_state_from_event(const struct GekkoGameEvent* event);

/* Runtime kill switch for the sparse effect-pool save path (Option A).
 * Defaults to true. When false, gather_state writes a full sizeof(State)
 * blob exactly as the legacy path did, and load_state walks the matching
 * format back in. The wire format is self-describing via state_len so
 * mid-session toggles are safe (although the user-facing config knob is
 * read once at startup, not per-frame). */
void Netplay_SetSparseEffectSaveEnabled(bool enabled);
bool Netplay_GetSparseEffectSaveEnabled(void);

/* Promoted out of #if DEBUG 2026-04-26 so telemetry builds dump on desync.
 * The ring buffers it reads (state_buffer, saved_section_checksums,
 * saved_plw_scratch, saved_field_hashes) are populated unconditionally
 * inside save_current_state(); the dump itself is a once-per-session,
 * post-disconnect operation so it has no per-frame perf cost. */
void dump_desync_state(int frame, uint32_t local_checksum, uint32_t remote_checksum);

#endif
