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

/// Read the FPGA vsync feedback word from DDR3 (offset 0x40).
/// Feedback format: bits[31:8] = ARM timestamp (us, bottom 24 bits),
///                  bits[7:0]  = FPGA 8-bit frame counter.
/// Returns 0 if the writer is not initialized.
uint32_t NativeVideoWriter_ReadFeedback(void);

/// Read the feedback sequence number from DDR3 (offset 0x44).
uint32_t NativeVideoWriter_ReadFeedbackSeq(void);

/// Extract the FPGA frame counter from a feedback word.
static inline uint8_t NV_FeedbackFrameCounter(uint32_t fb) { return (uint8_t)(fb & 0xFF); }

/// Extract the ARM timestamp (microseconds, bottom 24 bits) from a feedback word.
static inline uint32_t NV_FeedbackTimestampUs(uint32_t fb) { return fb >> 8; }

#endif
