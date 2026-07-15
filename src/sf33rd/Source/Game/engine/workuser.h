#ifndef WORKUSER_H
#define WORKUSER_H

#include "sf33rd/Source/Game/engine/cmd_data.h"
#include "types.h"

#include <stdbool.h>

typedef enum ModeType {
    MODE_ARCADE,
    MODE_VERSUS,
    MODE_NETWORK,
    MODE_NORMAL_TRAINING,
    MODE_PARRY_TRAINING,
    MODE_REPLAY,
} ModeType;

typedef enum PlayMode {
    PLAY_MODE_NORMAL    = 0,
    PLAY_MODE_RECORDING = 1,
    PLAY_MODE_REPLAY    = 3,
} PlayMode;

typedef enum ReplayStatus {
    REPLAY_STATUS_IDLE        = 0,
    REPLAY_STATUS_RECORDING   = 1,
    REPLAY_STATUS_DONE        = 2,  /* buffer exhausted / playback finished */
    REPLAY_STATUS_REPLAYING   = 3,
    REPLAY_STATUS_BUFFER_FULL = 99,
} ReplayStatus;

typedef enum GamePauseState {
    GAME_PAUSE_RUNNING  = 0,
    GAME_PAUSE_NORMAL   = 1,    /* standard pause (demo mode, etc.) */
    GAME_PAUSE_TRAINING = 0x81, /* training-paused: suppresses hardware input reads and record/replay */
} GamePauseState;

/* Values for Training_Menu_From_Pause */
typedef enum TrainingMenuSource {
    TRAINING_MENU_DIRECT    = 0, /* normal entry (initial load or character change) */
    TRAINING_MENU_PAUSED    = 1, /* player pressed START during gameplay */
    TRAINING_MENU_RESETTING = 2, /* Record/Replay selected; Reset_Training will auto-advance to gameplay */
} TrainingMenuSource;

/* Values for control_pl_rno — what input to force on the dummy each frame via Control_Player_Tr() */
typedef enum DummyAction {
    DUMMY_ACTION_STAND    = 0,  /* force neutral input (dummy stands still) */
    DUMMY_ACTION_CROUCH   = 1,  /* force down direction */
    DUMMY_ACTION_JUMP     = 2,  /* force up direction */
    DUMMY_ACTION_UNFORCED = 99, /* don't force; behavior set by operator field (CPU or HUMAN) */
} DummyAction;

/* Indices into Menu_Jmp_Tbl — the outer game task state machine (task_ptr->r_no[0]) */
typedef enum MenuState {
    MENU_STATE_AFTER_TITLE      = 0,
    MENU_STATE_IN_GAME          = 1,
    MENU_STATE_WAIT_LOAD_SAVE   = 2,
    MENU_STATE_WAIT_REPLAY_CHK  = 3,
    MENU_STATE_DISP_AUTO_SAVE   = 4,
    MENU_STATE_SUSPEND          = 5,
    MENU_STATE_WAIT_REPLAY_LOAD = 6,
    MENU_STATE_TRAINING_MENU    = 7,  /* Training_Menu: training menu state machine */
    MENU_STATE_AFTER_REPLAY     = 8,
    MENU_STATE_DISP_AUTO_SAVE2  = 9,
    MENU_STATE_GAMEPLAY         = 10, /* Wait_Pause_in_Tr: gameplay with pause detection */
    MENU_STATE_RESET_TRAINING   = 11, /* Reset_Training: full screen wipe + character reinit */
    MENU_STATE_RESET_REPLAY     = 12,
    MENU_STATE_END_REPLAY       = 13,
} MenuState;

static inline bool Is_Training_Mode(ModeType mode) {
    return mode == MODE_NORMAL_TRAINING || mode == MODE_PARRY_TRAINING;
}

// MARK: - Non-serializable

extern const_s16_arr Tech_Address[2];
extern void* Shell_Address[2];
extern void* Synchro_Address[2][2];

// MARK: - Unhandled

extern const u8* Free_Ptr[2];
extern u8* Lag_Ptr;
extern u16* Demo_Ptr[2];

/* Set by the engine in set_jugde_area() when cg_ja.atix != 0 for a
 * player WORK. Read by the frame-data overlay; reset by main.c at the
 * start of each game frame. Captures the engine's true "active hitbox
 * this frame" signal even when cg_ja.atix gets reset before our overlay
 * tick runs (which is what happens for some kicks like Q close MK). */
extern u8 fd_engine_hitbox_active[2];

/* CONTACT-2 Step 1 (2026-07-13, diagnostics only — design.md §1.3 G4):
 * set to 1 by check_leap_attack() (pls03.c) the instant it dispatches the
 * Universal-Overhead move, right before hissatsu_setup_union() — the same
 * engine tick that produces the r1 0->4 edge the overlay's MOVE_START
 * detector sees (hissatsu_setup_union writes routine_no[1]=4 directly).
 * Reset every frame by main.c's game_step_0(), alongside
 * fd_engine_hitbox_active, BEFORE njUserMain() runs each frame — same
 * precedent/contract as that array. Read (and latched into
 * g_cur.move_is_uoh) by frame_data_overlay_tick()'s MOVE_START block; never
 * read anywhere else. Preferred (assumption-free) form of the G4 UOH
 * dispatch tag; the cmoa.koc==5 && cmoa.ix==waza_r[14][0] fallback compare
 * is also computed and printed (MOVE_START `uoh_fb=%d`) for cross-check. */
extern u8 fd_engine_move_is_uoh[2];

/* Accumulated by char_move() across all cells with non-zero cg_ja.atix
 * during a move. Each active cell contributes its freshly-loaded cgctr
 * value (its arcade-table "active frames" share). Sum matches the
 * canonical arcade A. Cleared by the overlay on r1: 0→4 transition. */
extern u8 fd_engine_active_count[2];

/* Tracks the cgix of the most recently counted active cell, so we don't
 * re-add the same cell's cgctr across multiple char_move calls within
 * its display window. */
extern s16 fd_prev_active_cgix[2];

/* Same-tick cgix-transit revoke (§13.7.4). When the cell-data dispatch
 * advances cg_ix multiple times within a single Game_timer tick (e.g.
 * Q's UOH cgix=16→20 on one game frame), the old cell has no visible
 * duration — its cgctr was consumed by the same-tick advance. Track the
 * Game_timer at which the previous transit's `add` was applied, plus the
 * size of that add, so the next transit on the same tick can revoke it
 * before counting the new cell. The jatix gate (added 2026-05-04) limits
 * the revoke to phantom transits where both calls share the same hitbox
 * (jatix), so legitimate "different hitbox per call" same-tick advances
 * (cr.MK, st.Far Forward) keep both contributions. */
extern u16 fd_prev_active_cgix_tick[2];
extern u8  fd_prev_active_cgix_add[2];
extern u8  fd_prev_active_cgix_jatix[2];

/* Phase 6A "arcade-split" projectile design (/tmp/phase6-nonq-plan.md §2).
 * Indexed by the OWNING PLAYER's id (master_id, 0/1) — never by the
 * projectile effect WORK's own id (always 13 for eff13.c "tama"
 * instances). Set by eff13.c's effect_13_init() (spawn) and charset.c's
 * char_move() (active-tick accumulation); consumed and reset by the
 * overlay (frame_data_overlay.c). Same sim-write-only / no-desync-risk
 * property as the arrays above — see the workuser.c definition comment. */

/* Set to 1 by effect_13_init() when a player-owned ("wk->work_id == 1")
 * projectile spawns, indexed by the owning player's id. Consumed (and
 * cleared for the next move) by the overlay tick the same frame it's
 * observed, to latch the move's proj_spawn_slot/proj_seen fields. */
extern u8 fd_engine_proj_spawned[2];

/* Accumulated by char_move()'s effect-WORK branch across all ticks where
 * a player-owned projectile's cg_ja.atix is non-zero. Simple +1-per-tick
 * (not the player accumulator's cgctr-weighted cell-duration arithmetic —
 * Phase 6A Step 1 discovery found the simpler form sufficient since this
 * count is display-only, never asserted against the arcade oracle).
 * Saturating u8. Cleared by the overlay on the owning player's MOVE_START,
 * same as fd_engine_active_count. */
extern u8 fd_engine_proj_active_count[2];

/* §13.12 (ENGINE-4): "hit-checkable projectile split" arcade-anchor
 * arrays. Same shape/indexing/reset/rollback contract as the two arrays
 * above (owning-player-id-indexed, write-only overlay feed, cleared by
 * the overlay on MOVE_START). Set by charset.c's char_move() effect-WORK
 * branch (hitok/natend) and eff13.c's tama chart routines (cut). Read
 * (and cleared) by frame_data_overlay.c's tick side, pre-raw[]-append.
 *
 * fd_engine_proj_hitok: set to 1 on the RISING EDGE of "cg_ja.atix != 0
 * AND att_hit_ok != 0" — the exact pair hitcheck.c's attack_hit_check()
 * gates the hit-check on (hitcheck.c:1618-1623). Edge-triggered (see
 * fd_engine_proj_hitok_armed below), not level-triggered: a WHIFF tama's
 * att_hit_ok stays 1 for the rest of its flight (hitcheck.c only clears
 * it on a confirmed hit), so re-asserting this every tick the pair holds
 * would keep re-arming it long after the overlay already consumed it
 * once — including into a LATER move, if this tama outlives the move
 * boundary (measured: N.D.L.'s WHIFF leg does exactly this). The overlay
 * latches the FIRST such edge's slot as proj_athok_slot, used alongside
 * proj_spawn_slot to anchor S = max(spawn, athok) — N.D.L.'s tama arms
 * athok two chart cells after atix, fireballs arm it at birth (so
 * max() is a no-op for them).
 *
 * fd_engine_proj_hitok_armed: internal edge-detector for
 * fd_engine_proj_hitok, not consumed by the overlay. Tracks whether the
 * owning player's tama was hit-checkable on the PREVIOUS tick, so
 * fd_engine_proj_hitok is written only on the 0->1 transition. Cleared
 * whenever cg_ja.atix reads 0 (not hit-checkable this tick) so the NEXT
 * genuine arm — this tama re-arming, or a brand new tama's own first arm
 * after the next MOVE_START zeroes it — is seen as a fresh edge.
 *
 * fd_engine_proj_cut: the engine's own KILL-REASON latch. Set to 1 (and
 * never cleared until the next MOVE_START) the moment a player-owned
 * tama's chart is retired to an erase chart, for ANY reason — chart end,
 * ground touch, life_time/off-screen timeout, or on-hit/on-deflect
 * consumption once vital depletes (kotp_00000's four erase transitions,
 * eff13.c ~:315-363). Deliberately NOT set by kotp_13000's (N.D.L.'s)
 * consumption handler (eff13.c ~:1592-1608) — that path clears
 * att_hit_ok and keeps calling char_move() on the SAME chart, so its
 * eventual atix->0 edge is chart-natural, not a cut. Read twice: once by
 * charset.c (write-time gate on fd_engine_proj_natend) and once more by
 * the overlay at consume time (frame_data_overlay.c) — the second read
 * is not redundant, for two same-invocation (not cross-tick) reasons: on
 * a confirmed hit, set_char_move_init(erase)'s own internal char_move()
 * call (charset.c:126, called from eff13.c:365/367/370) runs — and can
 * false-latch fd_engine_proj_natend — before fd_tama_chart_cut()
 * (eff13.c:377) records the cut, a few statements later in the SAME
 * kotp_00000 invocation (measured on ryu-had's contact tick: atix=0,
 * hit_flag!=0, cut still 0 at that point); and separately, kotp_00000's
 * own case-0 cut sites call fd_tama_chart_cut() strictly AFTER the
 * char_move() call that first shows atix==0, also within the SAME
 * invocation. Either way, the overlay's read (strictly after that whole
 * engine tick's njUserMain() pass has finished, including any
 * later-in-the-same-call cut) sees the fully-settled value where
 * charset.c's write-time read might not. In review, dropping either
 * read alone left the golden suite green; dropping both reproduced
 * exactly a 12-row regression (ryu-had and chunli-kik, R off by 2) — the
 * double read is defense-in-depth, not redundant.
 *
 * fd_engine_proj_natend: set to 1 by charset.c's tama branch on the tick
 * a player-owned tama's cg_ja.atix goes 0 after having been active AND
 * hf.hit_flag is 0 AND fd_engine_proj_cut is still 0 for that tama's
 * owner — i.e. the chart reached its own scripted end without ever being
 * cut or being mid-consumption. The hf.hit_flag==0 clause excludes the
 * same-invocation window between a confirmed hit's consumption dispatch
 * (case 1's own char_move(), charset.c:126) and its cut actually being
 * recorded a few statements later in that SAME kotp_00000 call (see
 * fd_engine_proj_cut above); it never affects N.D.L., whose own hit_flag
 * is long cleared by the time its true mid-chart atix->0 edge arrives.
 * The overlay latches this into proj_natural_end (gated on having seen
 * an active tick this
 * move — a natend from a PRIOR move's still-flying tama must not leak
 * forward — AND on fd_engine_proj_cut still reading 0 at consume time),
 * which selects the declared-active-window R formula over the
 * fire-and-forget meter_len-minus-S form. */
extern u8 fd_engine_proj_hitok[2];
extern u8 fd_engine_proj_hitok_armed[2];
extern u8 fd_engine_proj_cut[2];
extern u8 fd_engine_proj_natend[2];

// MARK: - Serialized

extern u8 Order[148];
extern u8 Order_Timer[148];
extern u8 Order_Dir[148];
extern u32 Score[2][3];
extern u32 Complete_Bonus;
extern u32 Stock_Score[2];
extern u32 Vital_Bonus[2];
extern u32 Time_Bonus[2];
extern u32 Stage_Stock_Score[2];
extern u32 Bonus_Score;
extern u32 Final_Bonus_Score;
extern u32 WGJ_Score;
extern u32 Bonus_Score_Plus;
extern u32 Perfect_Bonus[2];
extern u32 Keep_Score[2];
extern u32 Disp_Score_Buff[2];
extern s8 Winner_id;
extern s8 Loser_id;
extern s8 Break_Into;
extern u8 My_char[2];
extern u8 Allow_a_battle_f;
extern u8 Round_num;
extern s8 Complete_Judgement;
extern s8 Fade_Flag;
extern s8 Super_Arts[2];
extern s8 Forbid_Break;
extern s8 Request_Break[2];
extern s8 Continue_Count[2];

/// Go faster during a non-gameplay animation
extern bool Scene_Cut;

extern bool Time_Over;

extern s8 Counter_hi;
extern s8 Counter_low;
extern s16 Unit_Of_Timer;
extern s8 Select_Timer;
extern s8 Cursor_X[2];
extern s8 Cursor_Y[2];
extern s8 Cursor_Y_Pos[2][4];
extern s8 Cursor_Timer[2];
extern s8 Time_Stop;
extern s8 Suicide[8];
extern s8 Complete_Face;
extern u8 Play_Type;
extern s16 Sel_PL_Complete[2];
extern s8 New_Challenger;

// Character select routine indices
extern u8 S_No[4];

extern s8 Select_Start[2];

extern s8 request_message;
extern s8 judge_flag;
extern s8 WINNER;
extern s8 LOSER;
extern s8 Champion;
extern s8 Fade_Half_Flag;
extern s8 Reserve_Cut;
extern s8 Perfect_Flag;
extern s8 Next_Step;
extern s8 Switch_Type;
extern s8 Cover_Timer;
extern s8 Personal_Timer[2];
extern s8 Request_E_No;
extern s8 Request_G_No;
extern u8 Present_Rank[2];
extern s8 Best_Grade[2];
extern s8 Demo_Type;
extern s8 Rank_Type;
extern s8 Flash_Sign[2];
extern s8 Flash_Rank_Time;
extern s8 Flash_Rank_Interval;
extern s32 Ranking_X;
extern s8 Rank;
extern s8 Rank_X;
extern s8 E_07_Flag[2];
extern s8 Complete_Victory;
extern s8 Demo_Flag;
extern s32 Next_Demo;
extern s8 Demo_PL_Index;
extern s8 Demo_Stage_Index;
extern s8 Face_MV_Request;
extern s8 Face_Move;
extern s8 Player_id;
extern s8 Last_Player_id;
extern s8 Player_Number;
extern u8 DENJIN_Term[2];
extern s8 Rapid_No[2][4];
extern s8 COM_id;
extern s8 EM_id;
extern s8 Select_Status[2];
extern s8 Select_Demo_Index;
extern u8 Country;
extern s8 Demo_Time_Stop;
extern s8 Combo_Speed[2];
extern s8 Exec_Wipe;
extern s8 Passive_Mode;
extern s8 Passive_Flag[2];
extern s8 Flip_Flag[2];
extern s8 Lie_Flag[2];
extern s8 Counter_Attack[2];
extern s8 Attack_Flag[2];
extern s8 Limited_Flag[2];
extern s8 Shell_Ignore_Timer[2];
extern s8 Event_Judge_Gals;
extern u8 EJG_index[4];
extern s8 Guard_Flag[2];
extern s8 Pierce_Menu[2];
extern s8 Face_MV_Time;
extern s8 Before_Jump[2];
extern s8 Stop_Combo;
extern u8 Stock_Hit_Flag[2];
extern s8 Rolling_Flag[2];
extern u8 Continue_Coin[2];
extern s8 Ignore_Entry[2];
extern s8 Slide_Type;
extern s8 Moving_Plate[2];
extern s8 Naming_Cut[2];
extern s8 Moving_Plate_Counter[2];
extern s8 Player_Color[2];
extern s8 PP_Priority[2][3];
extern s8 OK_Priority[2];
extern u8 Stock_My_char[2];
extern s8 Stock_Player_Color[2];
extern s8 Music_Fade;
extern s8 Stop_SG;
extern s8 Operator_Status[2];
extern s8 Round_Operator[2];
extern s8 another_bg[2];
extern s8 Last_Super_Arts[2];
extern s8 Last_My_char[2];
extern s8 Continue_Menu[2];
extern s8 Timer_Freeze;
extern u8 Type_of_Attack[2];
extern s8 Standing_Timer[2];
extern s8 Before_Look[2];
extern s8 Attack_Count_No0[2];
extern s8 Standing_Master_Timer[2];
extern s8 PB_Music_Off;
extern s8 No_Death;
extern s8 Flash_MT[2];
extern s8 Squat_Timer[2];
extern s8 Squat_Master_Timer[2];
extern s8 Turn_Over[2];
extern s8 Turn_Over_Timer[2];
extern s8 Jump_Pass_Timer[2][4];
extern s8 sa_gauge_flash[2];
extern s8 Receive_Flag[2];
extern s8 Disposal_Again[2];
extern s8 BGM_Vol;
extern u8 Used_char[2];
extern s8 Break_Com[2][20];
extern s8 aiuchi_flag;
extern u8 paring_counter[2];
extern u8 paring_bonus_r[2];
extern u8 paring_ctr_vs[2][2];
extern u8 paring_ctr_ori[2];
extern u8 Attack_Count_Buff[2][4];
extern u8 Attack_Count_Index[2];
extern u8 CC_Value[2];
extern u8 Continue_Coin2[2];
extern u8 Weak_PL;
extern u8 Bullet_No[2];
extern u8 Bullet_Counter[2];
extern u8 Final_Result_id;
extern s8 Disp_Win_Name;
extern u8 Perfect_Counter[2];
extern u8 Straight_Counter[2];
extern u8 Appear_Q;
extern s8 Cut_Scroll;
extern s8 Break_Into_CPU;
extern s8 ID_of_Face[3][8];
extern s8 Cursor_Move[2];
extern s8 Auto_Cursor[2];
extern s8 Auto_No[2];
extern s8 Auto_Index[2];
extern s8 Auto_Timer[2];
extern s8 Explosion;
extern s8 Introduce_Break_Into[2];
extern s8 gouki_wins;
extern s8 EM_Rank;
extern s8 Disp_PERFECT;
extern s8 Escape_SS;
extern s8 Deley_Shot_No[2];
extern s8 Deley_Shot_Timer[2];
extern s8 Lost_Round[2];
extern s8 Super_Arts_Finish[2];
extern s8 Stage_SA_Finish[2];
extern s8 Perfect_Finish[2];
extern s8 Cheap_Finish[2];
extern s8 Last_My_char2[2];
extern s8 gouki_app;
extern s8 Bonus_Game_Complete;
extern u8 Get_Demo_Index;
extern u8 Combo_Demo_Flag;
extern u8 Stage_Continue[2];
extern u8 Pause_Hit_Marks;
extern u8 Extra_Break;
extern u8 Shin_Gouki_BGM;
extern s8 Stage_Lost_Round[2];
extern s8 Stage_Perfect_Finish[2];
extern s8 Stage_Cheap_Finish[2];
extern s8 EXE_obroll;
extern u8 End_PL;
extern s8 Stock_Com_Arts[2];
extern u8 PB_Status;
extern u8 Flip_Counter[2];
extern u8 Stage_Time_Finish[2];
extern u8 Bonus_Type;
extern s8 Completion_Bonus[2][2];
extern s8 ichikannkei;
extern u8 Plate_Disposal_No[2][3];
extern u8 SO_No[2];
extern u8 Disp_Command_Name[2][3];
extern u8 SC_No[4];
extern u8 BGM_No[2];
extern u8 BGM_Timer[2];
extern u8 EM_List[2][2];
extern s8 Sel_EM_Complete[2];
extern s8 Temporary_EM[2];
extern s8 OK_Moving_SA_Plate[2];
extern u8 Battle_Q[2];
extern u8 EM_History[2][10];
extern u8 GO_No[4];
extern u8 Aborigine;
extern u8 Continue_Count_Down[2];
extern u8 WGJ_Target;
extern u8 EM_Candidate[2][2][10];
extern s8 Last_Selected_EM[2];
extern u8 Q_Country;
extern u8 Continue_Cut[2];
extern u8 Introduce_Boss[2][2];
extern u8 Final_Play_Type[2];
extern s8 Rank_In[2][4];
extern s8 Request_Disp_Rank[2][4];
extern u8 Reset_Timer[2];
extern u8 bbbs_type;
extern u8 Straight_Flag[2];
extern u8 kakushi_ix;
extern u8 kakushi_op;
extern u8 RO_backup[2];
extern u8 PT_backup;
extern u8 E_Number[2][4];
extern u8 E_No[4];
extern u8 C_No[4];

// Game routine indices
extern u8 G_No[4];

extern u8 D_No[4];
extern u8 M_No[4];
extern u8 Exit_No;
extern u8 SP_No[2][4];
extern u8 Face_No[2];
extern s8 Stop_Cursor[2];
extern u8 Training_Index;
extern u8 Connect_Status;
extern u8 Menu_Suicide[4];
extern u8 Game_pause;
extern u8 Game_difficulty;
extern u8 Pause;
extern u8 Pause_ID;
extern u8 Exit_Menu;
extern u8 Conclusion_Flag;
extern u8 CP_No[2][4];
extern u8 CP_Index[2][8];
extern u8 Gap_Timer;
extern u8 Message_Suicide[4];

// Whether or not battle UI is displayed
extern u8 Disp_Cockpit;

extern s8 Select_Arts[2];
extern u8 Lamp_No;
extern u8 Lamp_Index;
extern u8 Lamp_Color;
extern u8 Stop_Update_Score;
extern u8 test_flag;
extern u8 ixbfw_cut;
extern u8 Cont_No[4];
extern u8 PL_Wins[2];
extern u8 Fade_R_No0;
extern u8 Fade_R_No1;
extern u8 Conclusion_Type;
extern u8 win_type[2][4];
extern u8 message_index;
extern u8 F_No0[2];
extern u8 F_No1[2];
extern u8 F_No2[2];
extern u8 F_No3[2];
extern u8 keep_condition[11];
extern s8 Check_Buff[4][2][12];
extern s8 Convert_Buff[4][2][12];
extern u8 Unsubstantial_BG[4];
extern s8 Menu_Cursor_X[2];
extern s8 Menu_Cursor_Y[2];
extern u8 Replay_Status[2];
extern u8 Disappear_LOGO;
extern u8 count_end;
extern u8 Play_Game;
extern s8 Menu_Cursor_Move;
extern u8 flash_win_type[2][4];
extern u8 sync_win_type[2][4];
extern ModeType Mode_Type;
extern s8 Menu_Page;
extern s8 Menu_Max;
extern u8 reset_NG_flag;
extern s8 VS_Stage;
extern u8 Present_Mode;
extern u8 Play_Mode;
extern u8 Page_Max;
extern u8 Direction_Working[6];
extern s8 Vital_Handicap[6][2];
extern s8 Cursor_Limit[2];
extern u8 Synchro_No;
extern s8 SA_shadow_on;
extern u8 Pause_Down;
extern u8 Training_ID;
extern u8 Disp_Attack_Data;
extern u8 Disp_Input_History;
extern u8 Disp_Frame_Data;
extern u8 Record_Data_Tr;
extern u8 End_Training;
extern s8 Menu_Page_Buff;
extern u8 Reset_Bootrom;
extern u8 Decide_ID;
extern s8 Training_Cursor;
extern u8 Training_Menu_From_Pause;
/* What input to force on the training dummy each frame; re-read every
 * frame by Control_Player_Tr() (menu.c). See DummyAction above. */
extern u8 control_pl_rno;
extern u8 Training_Auto_Start;
extern s8 Lag_Timer;
extern u8 CPU_Time_Lag[2];
extern u8 Forbid_Reset;
extern u8 CPU_Rec[2];
extern u8 Pause_Type;
extern u16 Game_timer;
extern s16 Control_Time;
extern s16 Time_in_Time;
extern s16 Round_Level;
extern u16 Round_Result;
extern u16 Fade_Number;
extern s16 G_Timer;
extern s16 D_Timer;
extern s16 Rank_Pos_X;
extern s16 Rank_Pos_Y;
extern s16 E_Timer;
extern s16 F_Timer[2];
extern s16 ENTRY_X;
extern s16 C_Timer;
extern s16 S_Timer;
extern s16 Flash_Complete[2];
extern s16 Sel_Arts_Complete[2];
extern s16 Arts_Y[2];
extern s16 Move_Super_Arts[2];
extern s16 Battle_Country;
extern s16 Face_Status;

// ID of the player currently operated on during player selection routines
extern s16 ID;

// ID of the player currently operated on during player selection routines (similar to `ID`)
extern s8 ID2;

extern s16 mes_already;
extern s16 Timer_00[2];
extern s16 Timer_01[2];
extern s16 PL_Distance[2];
extern s16 Area_Number[2];
extern u16 Lever_Buff[2];
extern u16 Lever_Pool[2];
extern s16 Tech_Index[2];
extern s16 Random_ix16;
extern s16 Random_ix32;
extern s16 M_Timer;
extern s16 VS_Tech[2];
extern u16 Guard_Type[2];
extern s16 Separate_Area[2][3];
extern u16 Free_Lever[2];
extern s16 Term_No[2];
extern s16 Com_Width_Data[2];
extern u16 Lever_Squat[2];
extern u16 M_Lv[2];
extern s16 Insert_Y;
extern s16 scr_req_x;
extern s16 scr_req_y;
extern s16 zoom_req_flag_old;
extern s16 zoom_request_flag;
extern s16 zoom_request_level;
extern s16 Last_Selected_ID;
extern s16 Last_Called_SE;
extern s16 VS_Index[2];
extern s16 Rapid_Index[2];
extern s16 Shell_Separate_Area[2][3];
extern s16 Attack_Counter[2];
extern s16 Last_Attack_Counter[2];
extern u16 Pattern_Index[2];
extern s16 Com_Color_Shot;
extern u16 Resume_Lever[2][20];
extern u16 players_timer;
extern u16 Lever_Store[2][3];
extern s16 Return_CP_No[2];
extern s16 Return_CP_Index[2];
extern s16 Return_Pattern_Index[2];
extern u16 Lever_LR[2];
extern s16 Last_Eftype[2];
extern u16 DENJIN_No[2];
extern u16 SC_Personal_Time[2];
extern s16 Guard_Counter[2];
extern s16 Limit_Time;
extern s16 Last_Pattern_Index[2];
extern s16 Random_ix16_ex;
extern s16 Random_ix32_ex;
extern s16 DE_X[2];
extern s16 Exit_Timer;
extern s16 Max_vitality;
extern s16 Bonus_Game_Flag;
extern s16 Bonus_Game_Work;
extern s16 Bonus_Game_result;
extern s16 Stock_Bonus_Game_Result;
extern s16 bs_scrrrl[2][2];
extern s16 Bonus_Stage_RNO[4];
extern s16 Bonus_Stage_Level;
extern s16 Bonus_Stage_Tix;
extern s16 Bonus_Game_ex_result;
extern s16 Stock_Com_Color[2];
extern s16 bs2_floor[3];
extern s16 bs2_hosei[3];
extern s16 bs2_current_damage;
extern u16 Win_Record[2];
extern u16 Stock_Win_Record[2];
extern u16 WGJ_Win;
extern s16 Target_BG_X[6];
extern s16 Offset_BG_X[6];
extern u16 Result_Timer[2];
extern s16 scrl;
extern s16 scrr;
extern u16 vital_stop_flag[2];
extern u16 gauge_stop_flag[2];
extern s16 Lamp_Timer;
extern s16 Cont_Timer;
extern s16 Plate_X[2][3];
extern s16 Plate_Y[2][3];
extern u16 Demo_Timer[2];
extern u16 Condense_Buff[2];
extern u16 Keep_Grade[2];
extern u16 IO_Result;
extern u16 VS_Win_Record[2];
extern u16 plsw_00[2];
extern u16 plsw_01[2];
extern s16 Flash_Synchro;
extern s16 Synchro_Level;
extern s16 Random_ix16_com;
extern s16 Random_ix32_com;
extern s16 Random_ix16_ex_com;
extern s16 Random_ix32_ex_com;
extern s16 Random_ix16_bg;
extern s16 Opening_Now;

#endif
