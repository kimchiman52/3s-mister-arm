/**
 * @file renderer.c
 * @brief Renderer_ dispatch — routes to the active platform backend.
 */

#include "rendering/game_renderer.h"

#if defined(TARGET_PSP)
#include "port/psp/psp_renderer.h"
#elif defined(TARGET_3DS)
#include "port/ctr/ctr_game_renderer.h"
#else
#include "port/sdl/sdl_game_renderer.h"
#endif

void Renderer_CreateTexture(unsigned int th) {
#if defined(TARGET_PSP)
    PSPRenderer_CreateTexture(th);
#elif defined(TARGET_3DS)
    CTRRenderer_CreateTexture(th);
#else
    SDLGameRenderer_CreateTexture(th);
#endif
}

void Renderer_DestroyTexture(unsigned int texture_handle) {
#if defined(TARGET_PSP)
    PSPRenderer_DestroyTexture(texture_handle);
#elif defined(TARGET_3DS)
    CTRRenderer_DestroyTexture(texture_handle);
#else
    SDLGameRenderer_DestroyTexture(texture_handle);
#endif
}

void Renderer_UnlockTexture(unsigned int th) {
#if defined(TARGET_PSP)
    PSPRenderer_UnlockTexture(th);
#elif defined(TARGET_3DS)
    CTRRenderer_UnlockTexture(th);
#else
    SDLGameRenderer_UnlockTexture(th);
#endif
}

void Renderer_CreatePalette(unsigned int ph) {
#if defined(TARGET_PSP)
    PSPRenderer_CreatePalette(ph);
#elif defined(TARGET_3DS)
    CTRRenderer_CreatePalette(ph);
#else
    SDLGameRenderer_CreatePalette(ph);
#endif
}

void Renderer_DestroyPalette(unsigned int palette_handle) {
#if defined(TARGET_PSP)
    PSPRenderer_DestroyPalette(palette_handle);
#elif defined(TARGET_3DS)
    CTRRenderer_DestroyPalette(palette_handle);
#else
    SDLGameRenderer_DestroyPalette(palette_handle);
#endif
}

void Renderer_UnlockPalette(unsigned int th) {
#if defined(TARGET_PSP)
    PSPRenderer_UnlockPalette(th);
#elif defined(TARGET_3DS)
    CTRRenderer_UnlockPalette(th);
#else
    SDLGameRenderer_UnlockPalette(th);
#endif
}

void Renderer_SetTexture(unsigned int th) {
#if defined(TARGET_PSP)
    PSPRenderer_SetTexture(th);
#elif defined(TARGET_3DS)
    CTRRenderer_SetTexture(th);
#else
    SDLGameRenderer_SetTexture(th);
#endif
}

void Renderer_DrawTexturedQuad(const Sprite* sprite, unsigned int color) {
#if defined(TARGET_PSP)
    PSPRenderer_DrawTexturedQuad(sprite, color);
#elif defined(TARGET_3DS)
    CTRRenderer_DrawTexturedQuad(sprite, color);
#else
    SDLGameRenderer_DrawTexturedQuad(sprite, color);
#endif
}

void Renderer_DrawSprite(const Sprite* sprite, unsigned int color) {
#if defined(TARGET_PSP)
    PSPRenderer_DrawSprite(sprite, color);
#elif defined(TARGET_3DS)
    CTRRenderer_DrawSprite(sprite, color);
#else
    SDLGameRenderer_DrawSprite(sprite, color);
#endif
}

void Renderer_DrawSprite2(const Sprite2* sprite2) {
#if defined(TARGET_PSP)
    PSPRenderer_DrawSprite2(sprite2);
#elif defined(TARGET_3DS)
    CTRRenderer_DrawSprite2(sprite2);
#else
    SDLGameRenderer_DrawSprite2(sprite2);
#endif
}

void Renderer_DrawSprites2Batch(const Sprite2* sprites,
                                int sprite_count,
                                const signed char* up_flags,
                                int up_flag_count) {
#if defined(TARGET_PSP) || defined(TARGET_3DS)
    /* Non-SDL targets: fall back to individual per-sprite calls.
       These targets are not active for MiSTer but kept for completeness. */
    unsigned int keep = 0;
    for (int i = 0; i < sprite_count; i++) {
        if ((int)sprites[i].id < up_flag_count && up_flags[sprites[i].id]) {
            if (keep != sprites[i].tex_code) {
                keep = sprites[i].tex_code;
                Renderer_SetTexture(keep);
            }
#if defined(TARGET_PSP)
            PSPRenderer_DrawSprite2(&sprites[i]);
#else
            CTRRenderer_DrawSprite2(&sprites[i]);
#endif
        }
    }
#else
    SDLGameRenderer_DrawSprites2Batch(sprites, sprite_count, up_flags, up_flag_count);
#endif
}

void Renderer_DrawSolidQuad(const Quad* quad, unsigned int color) {
#if defined(TARGET_PSP)
    PSPRenderer_DrawSolidQuad(quad, color);
#elif defined(TARGET_3DS)
    CTRRenderer_DrawSolidQuad(quad, color);
#else
    SDLGameRenderer_DrawSolidQuad(quad, color);
#endif
}
