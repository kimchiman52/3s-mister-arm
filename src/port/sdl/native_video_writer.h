#ifndef PORT_SDL_NATIVE_VIDEO_WRITER_H
#define PORT_SDL_NATIVE_VIDEO_WRITER_H

#include <stdbool.h>
#include <stdint.h>

/// Initialize the DDR3 direct writer for FPGA native video output.
/// Maps /dev/mem at the native video buffer region (0x3A000000) and clears
/// both frame buffers.  Returns true when the writer is ready.
bool NativeVideoWriter_Init(void);

/// Release DDR3 mapping and close /dev/mem.
void NativeVideoWriter_Shutdown(void);

/// Write one RGB565 frame (384x224) into the inactive DDR3 double-buffer,
/// then flip the control word so the FPGA reads from the freshly-written buffer.
/// @param pixels_rgb565  Source pixel data in RGB565 format.
/// @param width          Must be 384.
/// @param height         Must be 224.
/// @param pitch          Row stride in bytes (384*2 if contiguous).
void NativeVideoWriter_WriteFrame(const void* pixels_rgb565, int width, int height, int pitch);

/// True if the DDR3 writer has been initialised and is ready for frames.
bool NativeVideoWriter_IsActive(void);

#endif
