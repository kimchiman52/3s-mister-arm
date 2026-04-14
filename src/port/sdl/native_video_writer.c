#include "port/sdl/native_video_writer.h"

#if defined(PORT_MISTER)

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define NV_DDR_PHYS_BASE    0x3A000000u
#define NV_DDR_REGION_SIZE  0x00060000u  /* 384KB, covers both buffers + control */
#define NV_CTRL_OFFSET      0x00000000u
#define NV_BUF0_OFFSET      0x00000100u
#define NV_BUF1_OFFSET      0x0002A200u
#define NV_FEEDBACK_OFFSET  0x00000040u
#define NV_FRAME_WIDTH       384
#define NV_FRAME_HEIGHT      224
#define NV_FRAME_BYTES      (NV_FRAME_WIDTH * NV_FRAME_HEIGHT * 2)  /* 172,032 */

static int mem_fd = -1;
static volatile uint8_t* ddr_base = NULL;
static uint32_t frame_counter = 0;
static int active_buf = 0;

bool NativeVideoWriter_Init(void) {
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("NativeVideoWriter: open /dev/mem");
        return false;
    }

    ddr_base = (volatile uint8_t*)mmap(NULL, NV_DDR_REGION_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, NV_DDR_PHYS_BASE);
    if (ddr_base == MAP_FAILED) {
        perror("NativeVideoWriter: mmap");
        ddr_base = NULL;
        close(mem_fd);
        mem_fd = -1;
        return false;
    }

    /* Clear both buffers and control word */
    memset((void*)(ddr_base + NV_BUF0_OFFSET), 0, NV_FRAME_BYTES);
    memset((void*)(ddr_base + NV_BUF1_OFFSET), 0, NV_FRAME_BYTES);
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = 0;
    frame_counter = 0;
    active_buf = 0;

    /* Clear feedback words so ARM doesn't read stale data from a previous run */
    volatile uint32_t* feedback = (volatile uint32_t*)(ddr_base + NV_FEEDBACK_OFFSET);
    *feedback = 0;
    volatile uint32_t* feedback_seq = (volatile uint32_t*)(ddr_base + NV_FEEDBACK_OFFSET + 4);
    *feedback_seq = 0;

    return true;
}

void NativeVideoWriter_Shutdown(void) {
    if (ddr_base) {
        /* Clear control word so FPGA detects stale frame and blanks output */
        volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
        *ctrl = 0;

        munmap((void*)ddr_base, NV_DDR_REGION_SIZE);
        ddr_base = NULL;
    }

    if (mem_fd >= 0) {
        close(mem_fd);
        mem_fd = -1;
    }
}

void NativeVideoWriter_WriteFrame(const void* pixels_rgb565, int width, int height, int pitch) {
    if (!ddr_base || width != NV_FRAME_WIDTH || height != NV_FRAME_HEIGHT) {
        return;
    }

    uint32_t buf_offset = (active_buf == 0) ? NV_BUF0_OFFSET : NV_BUF1_OFFSET;
    volatile uint8_t* dst = ddr_base + buf_offset;

    if (pitch == NV_FRAME_WIDTH * 2) {
        /* Contiguous: single memcpy */
        memcpy((void*)dst, pixels_rgb565, NV_FRAME_BYTES);
    } else {
        /* Row-by-row copy */
        const uint8_t* src = (const uint8_t*)pixels_rgb565;
        for (int y = 0; y < NV_FRAME_HEIGHT; y++) {
            memcpy((void*)(dst + y * NV_FRAME_WIDTH * 2), src + y * pitch, NV_FRAME_WIDTH * 2);
        }
    }

    /*
     * Write ordering: on strongly-ordered/device memory (O_SYNC + MAP_SHARED),
     * ARM guarantees all prior writes (pixel data) complete before this write.
     * If this is ever changed to use cached memory + explicit flushes, a DSB
     * barrier must be inserted here before the control word write.
     */
    frame_counter++;
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = (frame_counter << 2) | (active_buf & 1);

    /* Swap buffer for next frame */
    active_buf ^= 1;
}

bool NativeVideoWriter_IsActive(void) {
    return ddr_base != NULL;
}

uint32_t NativeVideoWriter_ReadFeedback(void) {
    if (!ddr_base) return 0;
    volatile uint32_t* fb = (volatile uint32_t*)(ddr_base + NV_FEEDBACK_OFFSET);
    return *fb;
}

uint32_t NativeVideoWriter_ReadFeedbackSeq(void) {
    if (!ddr_base) return 0;
    volatile uint32_t* seq = (volatile uint32_t*)(ddr_base + NV_FEEDBACK_OFFSET + 4);
    return *seq;
}

#else

bool NativeVideoWriter_Init(void) {
    return false;
}

void NativeVideoWriter_Shutdown(void) {
}

void NativeVideoWriter_WriteFrame(const void* pixels_rgb565, int width, int height, int pitch) {
    (void)pixels_rgb565;
    (void)width;
    (void)height;
    (void)pitch;
}

bool NativeVideoWriter_IsActive(void) {
    return false;
}

uint32_t NativeVideoWriter_ReadFeedback(void) {
    return 0;
}

uint32_t NativeVideoWriter_ReadFeedbackSeq(void) {
    return 0;
}

#endif
