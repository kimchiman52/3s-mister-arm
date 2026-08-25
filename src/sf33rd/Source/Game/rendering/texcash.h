#ifndef TEXCASH_H
#define TEXCASH_H

#include "structs.h"
#include "types.h"

extern TexturePoolUsed* tpu_free;
extern u8* texcash_melt_buffer;

void disp_texcash_free_area();
void init_texcash_1st();
void init_texcash_2nd(s16 ix);
void init_texcash_before_process();
void search_texcash_free_area(s16 ix);
void update_with_tpu_free(MultiTexture* mt);
/* [task #61] Per-instance half of texture_cash_update()'s ext branch:
 * makeup_tpu_free + the MAPPING MISS guard + update_with_tpu_free.  Split
 * out so --test-texcash-bounds can drive the guard directly. */
void texcash_release_instance(MultiTexture* mt, PatternInstance* cp, s16 num, s16 i);
void texture_cash_update();

#ifdef ENABLE_NETPLAY_TESTS
extern unsigned texcash_guard_hits;
#endif
void make_texcash_work(s16 ix);
void purge_texcash_work(s16 ix);
void Clear_texcash_work();
s16 get_my_trans_mode(s16 curr);

#endif
