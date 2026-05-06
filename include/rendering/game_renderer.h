/**
 * @file game_renderer.h
 * @brief Cross-platform renderer interface for the game core.
 *
 * Game logic calls Renderer_* functions for all rendering operations.
 * Each platform provides implementations in its GPU backend.
 */
#ifndef RENDERING_GAME_RENDERER_H
#define RENDERING_GAME_RENDERER_H

#include "rendering/primitives.h"

#include <stdbool.h>

void Renderer_CreateTexture(unsigned int th);
void Renderer_DestroyTexture(unsigned int texture_handle);
void Renderer_UnlockTexture(unsigned int th);
void Renderer_CreatePalette(unsigned int ph);
void Renderer_DestroyPalette(unsigned int palette_handle);
void Renderer_UnlockPalette(unsigned int th);
void Renderer_SetTexture(unsigned int th);
void Renderer_DrawTexturedQuad(const Sprite* sprite, unsigned int color);
void Renderer_DrawSprite(const Sprite* sprite, unsigned int color);
void Renderer_DrawSprite2(const Sprite2* sprite2);
void Renderer_DrawSprites2Batch(const Sprite2* sprites,
                                int sprite_count,
                                const signed char* up_flags,
                                int up_flag_count);
void Renderer_DrawSolidQuad(const Quad* quad, unsigned int color);
bool Renderer_DrawInputHistoryGlyph(float x, float y, float z, int glyph_index, unsigned int color);

typedef enum Renderer_InputHistoryGlyph {
    RENDERER_INPUT_GLYPH_UP = 0,
    RENDERER_INPUT_GLYPH_UP_RIGHT = 1,
    RENDERER_INPUT_GLYPH_RIGHT = 2,
    RENDERER_INPUT_GLYPH_DOWN_RIGHT = 3,
    RENDERER_INPUT_GLYPH_DOWN = 4,
    RENDERER_INPUT_GLYPH_DOWN_LEFT = 5,
    RENDERER_INPUT_GLYPH_LEFT = 6,
    RENDERER_INPUT_GLYPH_UP_LEFT = 7,
    RENDERER_INPUT_GLYPH_PUNCH = 8,
    RENDERER_INPUT_GLYPH_KICK = 9,
    RENDERER_INPUT_GLYPH_COUNT = 10,
} Renderer_InputHistoryGlyph;

#endif
