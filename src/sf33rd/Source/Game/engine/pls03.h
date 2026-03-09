#ifndef PLS03_H
#define PLS03_H

#include "structs.h"
#include "types.h"

typedef struct PLS03_SuperArtCommandTelemetry {
    u64 entry_calls_total;
    u64 entry_cmd_sel_calls_total;
    u64 entry_cmd_sel_not_ready_total;
    u64 entry_direct_calls_total;
    u64 check_calls_total;
    u64 ready_checks_total;
    u64 blocked_pcon_dp_total;
    u64 ground_candidate_checks_total;
    u64 ground_precondition_blocked_total;
    u64 ground_no_match_total;
    u64 air_candidate_checks_total;
    u64 air_precondition_blocked_total;
    u64 air_no_match_total;
    u64 command_matches_total;
} PLS03_SuperArtCommandTelemetry;

void hissatsu_setup_union(PLW* wk, s16 rno);
s16 cmdixconv(s16 ix);
s32 check_full_gauge_attack(PLW* wk, s8 always);
s32 check_full_gauge_attack2(PLW* wk, s8 always);
s16 check_super_arts_attack(PLW* wk);
s32 check_super_arts_attack_dc(PLW* wk);
void PLS03_ResetSuperArtCommandTelemetry(void);
s32 PLS03_GetSuperArtCommandTelemetry(s16 player, PLS03_SuperArtCommandTelemetry* out_telemetry);
s32 execute_super_arts(PLW* wk);
s32 check_special_attack(PLW* wk);
void chainex_spat_cancel_kidou(WORK* wk);
s32 check_leap_attack(PLW* wk);
s32 check_nm_attack(PLW* wk);
s16 hikusugi_check(WORK* wk);
s32 check_chouhatsu(PLW* wk);
s32 check_nagenuke_cmd(PLW* wk);
s32 check_catch_attack(PLW* wk);
void set_attack_routine_number(PLW* wk);
u16 get_nearing_range(s16 pnum, s16 kos);
s32 waza_select(PLW* wk, s16 kos, s16 sf);
u16 decode_wst_data(PLW* wk, u16 cmd, s16 cmd_ex);
s16 get_em_body_range(WORK* wk);
s32 cmd_ex_check(s16 px, s16 cx);
s16 shot_data_convert(u16 sw);
s16 shot_data_refresh(s16 sw);
s16 renbanshot_conpaneshot(const s16* dadr, s16 pow);
s16 datacmd_conpanecmd(s16 dat);
s32 check_renda_cancel(PLW* wk);
s32 check_meoshi_cancel(PLW* wk);
s16 get_meoshi_lever(s16 data);
s16 get_meoshi_shot(s16 data);

#endif
