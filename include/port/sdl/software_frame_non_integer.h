#ifndef SOFTWARE_FRAME_NON_INTEGER_H
#define SOFTWARE_FRAME_NON_INTEGER_H

#include <SDL3/SDL.h>
#include <stdbool.h>

bool SDLSoftwareFrame_RasterNonIntegerLookupARGB8888(const SDL_FRect* dst_rect,
                                                     const SDL_FRect* src_uv_rect,
                                                     SDL_FlipMode flip,
                                                     Uint32 color,
                                                     SDL_Surface* dst_surface,
                                                     const SDL_Surface* src_surface);

#endif
