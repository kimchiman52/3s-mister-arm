#ifndef MTRANS_H
#define MTRANS_H

#include "structs.h"
#include "types.h"

extern f32 PrioBase[128];

void appSetupBasePriority();
void appSetupTempPriority();
void seqsInitialize(void* adrs);
void seqsBeforeProcess();
void seqsAfterProcess();
u32 seqsGetUseMemorySize();
void makeup_tpu_free(s32 x16, s32 x32, PatternMap* map);
/* [task #61] Bounds-checked replacement for get_free_patcash_index() plus
 * the unchecked `adr[kazu++] = cp` that followed it.  Returns NULL when the
 * collection is exhausted; callers must skip the sprite.  Non-static so the
 * --test-texcash-bounds harness can drive it directly. */
PatternInstance* patcash_acquire(PatternCollection* padr);
void mlt_obj_trans_update(MultiTexture* mt);
void mlt_obj_melt2(MultiTexture* mt, u16 cg_number);
void mlt_obj_trans_init(MultiTexture* mt, s32 mode, u8* adrs);
void mlt_obj_matrix(WORK* wk, s32 base_y);
void mlt_obj_disp_rgb(MultiTexture* mt, WORK* wk, s32 base_y);
void mlt_obj_disp(MultiTexture* mt, WORK* wk, s32 base_y);
void mlt_obj_trans_cp3(MultiTexture* mt, WORK* wk, s32 base_y);
void mlt_obj_trans_rgb(MultiTexture* mt, WORK* wk, s32 base_y);
void mlt_obj_trans(MultiTexture* mt, WORK* wk, s32 base_y);
void draw_box(f64 arg0, f64 arg1, f64 arg2, f64 arg3, u32 col, u32 attr, s16 prio);
u16 seqsGetSprMax();
s16 getObjectHeight(u16 cgnum);

#include <stdint.h>
uint64_t Mtrans_GetPerfTexRenewNs(void);
uint64_t Mtrans_GetPerfSprSubmitNs(void);
void Mtrans_ResetPerfTimers(void);

#endif
