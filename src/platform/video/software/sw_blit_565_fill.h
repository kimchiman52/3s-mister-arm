#if CRS_VIDEO_DRIVER_SOFTWARE

#ifndef SW_BLIT_565_FILL_H
#define SW_BLIT_565_FILL_H

// Partial-alpha (0 < a < 255) RGB565 solid-fill row kernel, factored out of
// sw_fill_solid_row so the scalar reference and the NEON implementation share a
// single source of truth for the blend arithmetic. Only compiled for the 16bpp
// canvas; the 32bpp path in sw_blit.c is unaffected.
//
// Blend maths (per pixel, disjoint rb/g planes of RGB565):
//   sa_5 = a >> 3            (0..31)
//   ia_5 = 32 - sa_5         (1..32)
//   rb   = ((src_rb*sa_5 + dst_rb*ia_5) >> 5) & 0xF81F
//   g    = ((src_g *sa_5 + dst_g *ia_5) >> 5) & 0x07E0
// All intermediates are pure integer add/mul/shift/and; the largest value is
// 0xF81F*31 + 0xF81F*32 = 4,001,697 < 2^32, so the NEON u32-lane path is
// bit-identical to the scalar path by construction.

#if defined(CRS_SW_CANVAS_16BPP)

#include <stdint.h>

// Scalar reference. Byte-for-byte the arithmetic sw_fill_solid_row used inline.
// Stay at 2-pixel unroll here; 4-pixel regressed on real hardware.
static inline void sw_fill_565_blend_row_scalar(uint16_t* dst, uint32_t src_565, uint32_t a, int count) {
    const uint32_t sa_5 = a >> 3;           // 0..31
    const uint32_t ia_5 = 32u - sa_5;       // 1..32
    const uint32_t s_rb_a = (src_565 & 0xF81Fu) * sa_5;
    const uint32_t s_g_a  = (src_565 & 0x07E0u) * sa_5;
    int i = 0;
    for (; i + 2 <= count; i += 2) {
        const uint32_t d0 = (uint32_t)dst[i + 0];
        const uint32_t d1 = (uint32_t)dst[i + 1];
        const uint32_t rb0 = ((s_rb_a + (d0 & 0xF81Fu) * ia_5) >> 5) & 0xF81Fu;
        const uint32_t g0  = ((s_g_a  + (d0 & 0x07E0u) * ia_5) >> 5) & 0x07E0u;
        const uint32_t rb1 = ((s_rb_a + (d1 & 0xF81Fu) * ia_5) >> 5) & 0xF81Fu;
        const uint32_t g1  = ((s_g_a  + (d1 & 0x07E0u) * ia_5) >> 5) & 0x07E0u;
        dst[i + 0] = (uint16_t)(rb0 | g0);
        dst[i + 1] = (uint16_t)(rb1 | g1);
    }
    for (; i < count; i++) {
        const uint32_t d = (uint32_t)dst[i];
        const uint32_t rb = ((s_rb_a + (d & 0xF81Fu) * ia_5) >> 5) & 0xF81Fu;
        const uint32_t g  = ((s_g_a  + (d & 0x07E0u) * ia_5) >> 5) & 0x07E0u;
        dst[i] = (uint16_t)(rb | g);
    }
}

#if defined(__ARM_NEON)

#include <arm_neon.h>

// NEON kernel: 8 px/iteration in u32 lanes, scalar tail. The u32 lane ops
// (vmulq/vaddq/vshrq/vandq/vorrq) reproduce the scalar integer arithmetic
// exactly (no overflow, see header note), and the disjoint rb/g bit ranges make
// vmovn_u32 truncation lossless. The tail defers to the scalar kernel so the
// last <8 px are bit-identical to the scalar path by construction.
static inline void sw_fill_565_blend_row_neon(uint16_t* dst, uint32_t src_565, uint32_t a, int count) {
    const uint32_t sa_5 = a >> 3;
    const uint32_t ia_5 = 32u - sa_5;
    const uint32_t s_rb_a = (src_565 & 0xF81Fu) * sa_5;
    const uint32_t s_g_a  = (src_565 & 0x07E0u) * sa_5;

    const uint32x4_t v_srb = vdupq_n_u32(s_rb_a);
    const uint32x4_t v_sg  = vdupq_n_u32(s_g_a);
    const uint32x4_t v_ia  = vdupq_n_u32(ia_5);
    const uint32x4_t m_rb  = vdupq_n_u32(0xF81Fu);
    const uint32x4_t m_g   = vdupq_n_u32(0x07E0u);

    int i = 0;
    for (; i + 8 <= count; i += 8) {
        const uint16x8_t d   = vld1q_u16(dst + i);
        const uint32x4_t dlo = vmovl_u16(vget_low_u16(d));
        const uint32x4_t dhi = vmovl_u16(vget_high_u16(d));

        const uint32x4_t rb_lo = vandq_u32(vshrq_n_u32(vaddq_u32(v_srb, vmulq_u32(vandq_u32(dlo, m_rb), v_ia)), 5), m_rb);
        const uint32x4_t g_lo  = vandq_u32(vshrq_n_u32(vaddq_u32(v_sg,  vmulq_u32(vandq_u32(dlo, m_g),  v_ia)), 5), m_g);
        const uint32x4_t rb_hi = vandq_u32(vshrq_n_u32(vaddq_u32(v_srb, vmulq_u32(vandq_u32(dhi, m_rb), v_ia)), 5), m_rb);
        const uint32x4_t g_hi  = vandq_u32(vshrq_n_u32(vaddq_u32(v_sg,  vmulq_u32(vandq_u32(dhi, m_g),  v_ia)), 5), m_g);

        const uint16x8_t out = vcombine_u16(vmovn_u32(vorrq_u32(rb_lo, g_lo)), vmovn_u32(vorrq_u32(rb_hi, g_hi)));
        vst1q_u16(dst + i, out);
    }
    if (i < count) {
        sw_fill_565_blend_row_scalar(dst + i, src_565, a, count - i);
    }
}

#endif // __ARM_NEON

#endif // CRS_SW_CANVAS_16BPP

#endif // SW_BLIT_565_FILL_H

#endif // CRS_VIDEO_DRIVER_SOFTWARE
