/**
 * @file dc_ghost.c
 * Compatibility Layer for Sega Dreamcast's Ninja SDK
 */

#include "sf33rd/Source/Game/rendering/dc_ghost.h"
#include "common.h"
#include "port/build_config.h"
#include "rendering/game_renderer.h"
#include "sf33rd/AcrSDK/ps2/flps2render.h"
#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "sf33rd/Source/Common/PPGFile.h"
#include "sf33rd/Source/Game/rendering/aboutspr.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "structs.h"

#include <string.h>

#define NTH_BYTE(value, n) ((((value >> n * 8) & 0xFF) << n * 8))
#define NJDP2D_PRIM_MAX 512

typedef struct {
    Vertex v;
    u32 col;
} _Polygon;

// `col` needs to be `uintptr_t` because it sometimes stores a pointer to `WORK`
typedef struct {
    Vec3 v[4];
    uintptr_t col;
    u32 type;
} NJDP2D_PRIM;

typedef struct {
    s16 total;
    s32 overflow_drops; // prims dropped this frame after hitting NJDP2D_PRIM_MAX
    u8 overflow_logged; // rate-limit: at most one overflow log per njdp2d_draw() drain (up to twice/frame)
    NJDP2D_PRIM prim[NJDP2D_PRIM_MAX];
} NJDP2D_W;

NJDP2D_W njdp2d_w;
MTX cmtx;

void njUnitMatrix(MTX* mtx) {
    if (mtx == NULL) {
        mtx = &cmtx;
    }

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            mtx->a[i][j] = (i == j);
        }
    }
}

void njGetMatrix(MTX* m) {
    *m = cmtx;
}

void njSetMatrix(MTX* md, MTX* ms) {
    if (md == NULL) {
        md = &cmtx;
    }

    *md = *ms;
}

void njScale(MTX* mtx, f32 x, f32 y, f32 z) {
    if (mtx == NULL) {
        mtx = &cmtx;
    }

    for (int i = 0; i < 4; i++) {
        mtx->a[0][i] *= x;
        mtx->a[1][i] *= y;
        mtx->a[2][i] *= z;
    }
}

void njTranslate(MTX* mtx, f32 x, f32 y, f32 z) {
    if (mtx == NULL) {
        mtx = &cmtx;
    }

    for (int j = 0; j < 4; j++) {
        mtx->a[3][j] += x * mtx->a[0][j] + y * mtx->a[1][j] + z * mtx->a[2][j];
    }
}

void njTranslateZ(MTX* mtx, f32 z) {
    if (mtx == NULL) {
        mtx = &cmtx;
    }

    mtx->a[3][0] += z * mtx->a[2][0];
    mtx->a[3][1] += z * mtx->a[2][1];
    mtx->a[3][2] += z * mtx->a[2][2];
    mtx->a[3][3] += z * mtx->a[2][3];
}

void njSetBackColor(u32 c0, u32 c1, u32 c2) {
    c0 = c0 | c1 | c2;
    flSetRenderState(FLRENDER_BACKCOLOR, NTH_BYTE(c0, 3) | NTH_BYTE(c0, 2) | NTH_BYTE(c0, 1) | NTH_BYTE(c0, 0));
}

void njColorBlendingMode(s32 target, s32 mode) {
    flSetRenderState(FLRENDER_ALPHABLENDMODE, 0x32);
}

void njCalcPoint(MTX* mtx, Vec3* ps, Vec3* pd) {
    if (mtx == NULL) {
        mtx = &cmtx;
    }

    const f32 x = ps->x;
    const f32 y = ps->y;
    const f32 z = ps->z;
    const f32 w = 1.0f;

    pd->x = x * mtx->a[0][0] + y * mtx->a[1][0] + z * mtx->a[2][0] + w * mtx->a[3][0];
    pd->y = x * mtx->a[0][1] + y * mtx->a[1][1] + z * mtx->a[2][1] + w * mtx->a[3][1];
    pd->z = x * mtx->a[0][2] + y * mtx->a[1][2] + z * mtx->a[2][2] + w * mtx->a[3][2];
}

void njCalcPoints(MTX* mtx, Vec3* ps, Vec3* pd, s32 num) {
    s32 i;

    if (mtx == NULL) {
        mtx = &cmtx;
    }

    for (i = 0; i < num; i++) {
        njCalcPoint(mtx, ps++, pd++);
    }
}

void njDrawTexture(Polygon* polygon, s32 /* unused */, s32 tex, s32 /* unused */) {
    Vertex vtx[4];
    s32 i;

    for (i = 0; i < 4; i++) {
        vtx[i] = ((_Polygon*)polygon)[i].v;
    }

    ppgWriteQuadWithST_B(vtx, polygon[0].col, NULL, tex, -1);
}

// Same quad as njDrawTexture, but skips the redundant texture-register bind.
// Only valid after a njDrawTexture that bound the identical texCode.
void njDrawTextureNoBind(Polygon* polygon, s32 /* unused */, s32 tex, s32 /* unused */) {
    Vertex vtx[4];
    s32 i;

    for (i = 0; i < 4; i++) {
        vtx[i] = ((_Polygon*)polygon)[i].v;
    }

    ppgWriteQuadWithST_B_NoBind(vtx, polygon[0].col, NULL, tex, -1);
}

void njDrawSprite(Polygon* polygon, s32 /* unused */, s32 tex, s32 /* unused */) {
    Vertex vtx[4];

    if ((polygon[0].x >= 384.0f) || (polygon[3].x < 0.0f) || (polygon[0].y >= 224.0f) || (polygon[3].y < 0.0f)) {
        return;
    }

    vtx[0] = ((_Polygon*)polygon)[0].v;
    vtx[3] = ((_Polygon*)polygon)[3].v;

    ppgWriteQuadWithST_B2(vtx, polygon[0].col, 0, tex, -1);
}

#if ENABLE_PERF_TELEMETRY
/* Per-frame prim-buffer diagnostics (perf-overlay report §4).  njdp2d_draw()
   runs up to twice per game frame and njdp2d_init() clears njdp2d_w.total each
   time, so accumulate the peak/drops here and reset once per frame from
   game_step_0() via Njdp2d_ResetPerf(). */
static s32 njdp2d_perf_peak_total = 0;
static s32 njdp2d_perf_drops = 0;
s32 Njdp2d_GetPerfPeakTotal(void) { return njdp2d_perf_peak_total; }
s32 Njdp2d_GetPerfDrops(void) { return njdp2d_perf_drops; }
void Njdp2d_ResetPerf(void) {
    njdp2d_perf_peak_total = 0;
    njdp2d_perf_drops = 0;
}
#endif

void njdp2d_init() {
    njdp2d_w.total = 0;
    njdp2d_w.overflow_drops = 0;
    njdp2d_w.overflow_logged = 0;
}

// Sort prim indices instead of moving full prims around.  Draw order is
// descending by priority (prim.v[0].z), with equal priorities kept in insertion
// (FIFO) order.  The key below canonicalizes signed zero so -0.0/+0.0 tie, then
// tie-breaks on the plain insertion index to hold the equal-priority FIFO.
static inline u32 float_to_sortable_u32(float f) {
    u32 b;
    memcpy(&b, &f, sizeof(b));
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

static void njdp2d_insertion_sort_idx(u16* idx, int lo, int hi, const u64* keys) {
    for (int i = lo + 1; i <= hi; i++) {
        const u16 x = idx[i];
        const u64 xk = keys[x];
        int j = i - 1;
        while (j >= lo && keys[idx[j]] > xk) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = x;
    }
}

static void njdp2d_quick_sort_idx(u16* idx, int lo, int hi, const u64* keys) {
    while (hi - lo > 15) {
        const int mid = lo + ((hi - lo) >> 1);
        if (keys[idx[lo]] > keys[idx[mid]]) {
            u16 t = idx[lo];
            idx[lo] = idx[mid];
            idx[mid] = t;
        }
        if (keys[idx[lo]] > keys[idx[hi]]) {
            u16 t = idx[lo];
            idx[lo] = idx[hi];
            idx[hi] = t;
        }
        if (keys[idx[mid]] > keys[idx[hi]]) {
            u16 t = idx[mid];
            idx[mid] = idx[hi];
            idx[hi] = t;
        }
        const u16 pivot_i = idx[mid];
        const u64 pivot_k = keys[pivot_i];
        idx[mid] = idx[hi - 1];
        idx[hi - 1] = pivot_i;
        int i = lo, j = hi - 1;
        for (;;) {
            while (keys[idx[++i]] < pivot_k) { /* */
            }
            while (keys[idx[--j]] > pivot_k) { /* */
            }
            if (i >= j)
                break;
            u16 t = idx[i];
            idx[i] = idx[j];
            idx[j] = t;
        }
        {
            u16 t = idx[i];
            idx[i] = idx[hi - 1];
            idx[hi - 1] = t;
        }
        if (i - lo < hi - i) {
            njdp2d_quick_sort_idx(idx, lo, i - 1, keys);
            lo = i + 1;
        } else {
            njdp2d_quick_sort_idx(idx, i + 1, hi, keys);
            hi = i - 1;
        }
    }
    njdp2d_insertion_sort_idx(idx, lo, hi, keys);
}

void njdp2d_draw() {
    static u16 order[NJDP2D_PRIM_MAX];
    static u64 keys[NJDP2D_PRIM_MAX];
    Quad prm;
    s32 i;
    s32 j;
    s32 k;
    s32 n = njdp2d_w.total;

    for (i = 0; i < n; i++) {
        f32 z = njdp2d_w.prim[i].v[0].z;
        if (z == 0.0f) {
            z = 0.0f; // canonicalize -0.0 to +0.0 so signed zeros tie like the old strict `>`
        }
        keys[i] = ((u64)(~float_to_sortable_u32(z)) << 32) | (u32)i;
        order[i] = (u16)i;
    }

    if (n > 1) {
        njdp2d_quick_sort_idx(order, 0, n - 1, keys);
    }

    for (i = 0; i < n; i++) {
        k = order[i];
        switch (njdp2d_w.prim[k].type) {
        case 0:
            for (j = 0; j < 4; j++) {
                prm.v[j] = njdp2d_w.prim[k].v[j];
            }

            Renderer_DrawSolidQuad(&prm, njdp2d_w.prim[k].col);
            break;

        case 1:
            shadow_drawing((WORK*)njdp2d_w.prim[k].col, njdp2d_w.prim[k].v[0].y);
            break;
        }
    }

#if ENABLE_PERF_TELEMETRY
    if (njdp2d_w.total > njdp2d_perf_peak_total) {
        njdp2d_perf_peak_total = njdp2d_w.total;
    }
    njdp2d_perf_drops += njdp2d_w.overflow_drops;
#endif

    njdp2d_init();
}

// `col` needs to be `uintptr_t` because it sometimes stores a pointer to `WORK`
void njdp2d_sort(f32* pos, f32 pri, uintptr_t col, s32 flag) {
    s32 ix = njdp2d_w.total;

    if (ix >= NJDP2D_PRIM_MAX) {
        njdp2d_w.overflow_drops += 1;
        if (!njdp2d_w.overflow_logged) {
            njdp2d_w.overflow_logged = 1;
            // The 2D polygon display request has exceeded the buffer\n
            flLogOut("２Ｄポリゴンの表示要求がバッファをオーバーしました\n");
        }
        return;
    }

    if (flag == 0) {
        njdp2d_w.prim[ix].v[0].z = njdp2d_w.prim[ix].v[1].z = njdp2d_w.prim[ix].v[2].z = njdp2d_w.prim[ix].v[3].z = pri;
        njdp2d_w.prim[ix].v[0].x = pos[0];
        njdp2d_w.prim[ix].v[0].y = pos[1];
        njdp2d_w.prim[ix].v[1].x = pos[2];
        njdp2d_w.prim[ix].v[1].y = pos[3];
        njdp2d_w.prim[ix].v[2].x = pos[4];
        njdp2d_w.prim[ix].v[2].y = pos[5];
        njdp2d_w.prim[ix].v[3].x = pos[6];
        njdp2d_w.prim[ix].v[3].y = pos[7];
        njdp2d_w.prim[ix].type = 0;
        njdp2d_w.prim[ix].col = col;
    }

    if (flag == 1) {
        njdp2d_w.prim[ix].v[0].z = pri;
        njdp2d_w.prim[ix].v[0].y = pos[0];
        njdp2d_w.prim[ix].type = 1;
        njdp2d_w.prim[ix].col = col;
    }

    njdp2d_w.total += 1;
}

void njDrawPolygon2D(PAL_CURSOR* p, s32 /* unused */, f32 pri, u32 attr) {
    if (attr & 0x20) {
        njdp2d_sort((f32*)p->p, pri, p->col->color, 0);
    }
}

void njSetPaletteBankNumG(u32 globalIndex, s32 bank) {
    ppgSetupCurrentPaletteNumber(0, bank);
}

void njSetPaletteData(s32 offset, s32 count, void* data) {
    palCopyGhostDC(offset, count, data);
    palUpdateGhostDC();
}

s32 njReLoadTexturePartNumG(u32 gix, s8* srcAdrs, u32 ofs, u32 size) {
    ppgRenewDotDataSeqs(0, gix, (u32*)srcAdrs, ofs, size);
    return 1;
}
