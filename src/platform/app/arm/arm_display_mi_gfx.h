#ifndef CRS_PLATFORM_APP_ARM_ARM_DISPLAY_MI_GFX_H
#define CRS_PLATFORM_APP_ARM_ARM_DISPLAY_MI_GFX_H

// Public mini-API exported by arm_display_mi_gfx.c for the Miyoo
// Mini Plus profile (PORT_MIYOO_MINI_PLUS=ON, CRS_ARM_HAVE_MI_GFX=1).
//
// SoftwareRenderer_Init / _Quit calls these in place of malloc/free
// for the giblet canvas so MI_GFX can blit directly out of an
// MMA-allocated DMA buffer with no intermediate memcpy.
//
// Header-gated under the same conditions as the .c file: the symbols
// only exist when the Miyoo profile is active, so include sites must
// guard with the same #if to avoid an unresolved-symbol link error
// on every other build profile.

#if CRS_APP_DRIVER_ARM && CRS_ARM_HAVE_MI_GFX

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool mi_gfx_alloc_canvas(size_t bytes, void** out_vir, unsigned long long* out_phy);
void mi_gfx_free_canvas(void* vir);

#ifdef __cplusplus
}
#endif

#endif  // CRS_APP_DRIVER_ARM && CRS_ARM_HAVE_MI_GFX

#endif  // CRS_PLATFORM_APP_ARM_ARM_DISPLAY_MI_GFX_H
