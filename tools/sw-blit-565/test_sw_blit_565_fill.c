// Exhaustive bit-exactness harness for the RGB565 partial-alpha solid-fill row
// kernel (src/platform/video/software/sw_blit_565_fill.h): compares the scalar
// reference against the NEON implementation over the value space the engine can
// present, plus row-geometry edge cases. Exits non-zero on the first mismatch.
//
// Build/run: tools/sw-blit-565/run.sh (needs an arm64 / __ARM_NEON host so the
// NEON path actually compiles). On a non-NEON host the kernel cannot be proven
// and the harness reports SKIP.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/video/software/sw_blit_565_fill.h"

#if !defined(__ARM_NEON)

int main(void) {
    printf("SKIP: __ARM_NEON not defined on this host; NEON kernel not compiled, cannot verify.\n");
    return 0;
}

#else

#define MAX_ROW 4096
#define MAX_COUNT 65536 // largest single compare_row call (full 16-bit dst sweep)

static uint16_t buf_scalar[MAX_COUNT + 32];
static uint16_t buf_neon[MAX_COUNT + 32];

static uint64_t g_pixels_checked = 0;

// Run both kernels over identical dst content and compare element-wise.
// `off` shifts the working pointer to exercise unaligned starts / the NEON tail.
static int compare_row(const uint16_t* dst_src, int count, uint32_t src_565, uint32_t a, int off, const char* what) {
    memcpy(buf_scalar + off, dst_src, (size_t)count * sizeof(uint16_t));
    memcpy(buf_neon + off, dst_src, (size_t)count * sizeof(uint16_t));

    sw_fill_565_blend_row_scalar(buf_scalar + off, src_565, a, count);
    sw_fill_565_blend_row_neon(buf_neon + off, src_565, a, count);

    for (int i = 0; i < count; i++) {
        if (buf_scalar[off + i] != buf_neon[off + i]) {
            printf("MISMATCH [%s]: src_565=0x%04X a=0x%02X count=%d off=%d idx=%d dst=0x%04X scalar=0x%04X neon=0x%04X\n",
                   what, src_565, a, count, off, i, dst_src[i], buf_scalar[off + i], buf_neon[off + i]);
            return 1;
        }
    }
    g_pixels_checked += (uint64_t)count;
    return 0;
}

// rb-plane decomposition: zero the g bits of src and dst so only the rb result
// is exercised; sweep all 1024 distinct (val & 0xF81F) source and dst values.
static int test_rb_plane(uint32_t a) {
    static uint16_t dst[1024];
    // Enumerate the 1024 distinct values of (x & 0xF81F): 5 high bits + 5 low.
    int n = 0;
    for (uint32_t hi = 0; hi < 32; hi++) {
        for (uint32_t lo = 0; lo < 32; lo++) {
            dst[n++] = (uint16_t)((hi << 11) | lo);
        }
    }
    for (int s = 0; s < 1024; s++) {
        if (compare_row(dst, n, dst[s], a, 0, "rb-plane")) {
            return 1;
        }
    }
    return 0;
}

// g-plane decomposition: only the 6-bit green field is non-zero in src and dst.
static int test_g_plane(uint32_t a) {
    static uint16_t dst[64];
    for (uint32_t v = 0; v < 64; v++) {
        dst[v] = (uint16_t)(v << 5);
    }
    for (int s = 0; s < 64; s++) {
        if (compare_row(dst, 64, dst[s], a, 0, "g-plane")) {
            return 1;
        }
    }
    return 0;
}

// Full 16-bit dst sweep for a fixed src: one call feeds all 65536 dst values.
// Confirms rb/g composition (disjoint-OR) empirically, not just per-plane.
static int test_full_dst_sweep(uint32_t src_565, uint32_t a) {
    static uint16_t dst[65536];
    for (int v = 0; v < 65536; v++) {
        dst[v] = (uint16_t)v;
    }
    return compare_row(dst, 65536, src_565, a, 0, "full-dst-sweep");
}

static uint32_t rng_state = 0x1234567u;
static uint32_t xrand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// Random combined fuzz across the full 16-bit space and full alpha domain.
static int test_combined_fuzz(int iters) {
    static uint16_t dst[MAX_ROW];
    for (int it = 0; it < iters; it++) {
        const int count = 1 + (int)(xrand() % MAX_ROW);
        const uint32_t src = xrand() & 0xFFFFu;
        const uint32_t a = 1u + (xrand() % 254u); // 1..254 (the 0<a<255 branch)
        for (int i = 0; i < count; i++) {
            dst[i] = (uint16_t)xrand();
        }
        if (compare_row(dst, count, src, a, 0, "combined-fuzz")) {
            return 1;
        }
    }
    return 0;
}

// Row widths 1..128 at every unaligned start offset 0..15, random content and
// alpha — hits the NEON 8-px tail and every misaligned vld1q_u16 start.
static int test_geometry(void) {
    static uint16_t dst[128];
    for (int count = 1; count <= 128; count++) {
        for (int off = 0; off <= 15; off++) {
            for (int rep = 0; rep < 4; rep++) {
                const uint32_t src = xrand() & 0xFFFFu;
                const uint32_t a = 1u + (xrand() % 254u);
                for (int i = 0; i < count; i++) {
                    dst[i] = (uint16_t)xrand();
                }
                if (compare_row(dst, count, src, a, off, "geometry")) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

int main(void) {
    // Alpha domain: sw_fill_solid_row only reaches this kernel for 0 < a < 255,
    // and the maths depends on a solely through (a >> 3), so the 32 buckets
    // a = {0,8,16,...,248} exhaust every distinct behaviour. Test all of them,
    // plus BOX_FILL_ALPHA=0x33 (aboutspr.c:39) and its neighbours explicitly.
    uint32_t alphas[32 + 8];
    int na = 0;
    for (uint32_t b = 0; b < 32; b++) {
        alphas[na++] = b << 3; // bucket representative (a>>3 == b)
    }
    const uint32_t extra[] = {0x01, 0x33, 0x34, 0x7F, 0x80, 0xFD, 0xFE, 0x40};
    for (size_t k = 0; k < sizeof(extra) / sizeof(extra[0]); k++) {
        alphas[na++] = extra[k];
    }

    for (int k = 0; k < na; k++) {
        const uint32_t a = alphas[k];
        if (test_rb_plane(a)) return 1;
        if (test_g_plane(a)) return 1;
    }

    // Full 16-bit dst cross for representative sources at BOX_FILL_ALPHA and a
    // few other buckets (composition guard beyond the per-plane decomposition).
    const uint32_t full_alphas[] = {0x33, 0x08, 0x40, 0x80, 0xC0, 0xF8, 0x01, 0xFE};
    const uint32_t full_srcs[] = {0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x1234, 0xABCD, 0x8410, 0x33CC};
    for (size_t ai = 0; ai < sizeof(full_alphas) / sizeof(full_alphas[0]); ai++) {
        for (size_t si = 0; si < sizeof(full_srcs) / sizeof(full_srcs[0]); si++) {
            if (test_full_dst_sweep(full_srcs[si], full_alphas[ai])) return 1;
        }
    }

    if (test_geometry()) return 1;
    if (test_combined_fuzz(20000)) return 1;

    printf("PASS: scalar == NEON bit-exact over %llu pixel comparisons.\n",
           (unsigned long long)g_pixels_checked);
    return 0;
}

#endif // __ARM_NEON
