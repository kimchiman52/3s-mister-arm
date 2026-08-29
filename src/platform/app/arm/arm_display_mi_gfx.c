#if defined(CRS_APP_DRIVER_ARM) && defined(CRS_ARM_HAVE_MI_GFX)

// SigmaStar MI_GFX hardware-blitter presenter for the Miyoo Mini Plus
// (PORT_MIYOO_MINI_PLUS=ON). Drives the SSD202D MI_GFX block to scale
// + rotate the 384x224 RGB565 giblet canvas onto the panel's ARGB8888
// framebuffer.
//
// Authored against the vendored SigmaStar BSP headers at
// vendor/sigmastar/inc/. Structurally informed by steward-fu/sdl2's
// mmiyoo backend (LGPL-2.1, structural reference only — no code copied).
// Optimizations vs vanilla mmiyoo:
//   - Zero-copy canvas via MI_SYS_MMA_Alloc (mi_gfx_alloc_canvas) so
//     the software renderer writes directly into a DMA-mapped buffer
//     readable by the GFX block.
//   - Single scratch buffer (vs vanilla two: tmp + overlay).
//   - Pre-computed letterbox rect at init (vs vanilla per-frame).
//
// The deployed package does NOT bundle libmi_*.so; runtime resolves
// libmi_sys.so.0 / libmi_gfx.so.0 / libmi_common.so.0 from the device's
// /customer/lib/, populated by the OnionOS firmware BSP.

#include "platform/app/arm/arm_display.h"
#include "platform/app/arm/arm_display_mi_gfx.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "mi_common.h"
#include "mi_common_datatype.h"
#include "mi_gfx.h"
#include "mi_gfx_datatype.h"
#include "mi_sys.h"
#include "mi_sys_datatype.h"

#define FBDEV_PATH "/dev/fb0"

// Giblet canvas worst-case footprint (384x224 RGB565 = 168000 bytes).
// Used to size the fallback scratch DMA buffer; the zero-copy path
// uses an MMA buffer of exactly canvas_bytes allocated by the
// software renderer via mi_gfx_alloc_canvas().
#define CANVAS_W_MAX 384
#define CANVAS_H_MAX 224
#define CANVAS_BPP 2

// ---------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------

static bool mi_initialized = false;
static bool gfx_opened = false;

static int fb_dev_fd = -1;
static struct fb_var_screeninfo fb_var = { 0 };
static struct fb_fix_screeninfo fb_fix = { 0 };

static MI_PHY fb_phy_addr = 0;
static void* fb_vir_addr = NULL;
static size_t fb_size = 0;

// Fallback scratch DMA buffer (used only when caller passes a non-MMA
// pointer; the zero-copy path bypasses this entirely).
static MI_PHY scratch_phy = 0;
static void* scratch_vir = NULL;
static size_t scratch_size = 0;

static int panel_w = 0;
static int panel_h = 0;
static MI_GFX_Rect_t lb_rect = { 0, 0, 0, 0 };

// Tearing mode: when THIRDSARM_NO_VSYNC=1 is set in the launch env, init
// configures the framebuffer for single-buffer scan-out (yres_virtual ==
// yres) and present() skips FBIOPAN_DISPLAY entirely. The blit writes
// directly into the visible buffer while the panel is scanning it; this
// produces tear lines but bypasses the ~11ms vsync wait that
// FBIOPAN_DISPLAY incurs on the Miyoo BSP. Cut 1 escape hatch for hitting
// 60fps before the NEON-16bpp render-side optimizations land.
static bool no_vsync = false;

// One registered (vir, phy) pair for the zero-copy canvas.
// Cut 1 has exactly one MMA-allocated buffer (the canvas); a generic
// capacity-N table is YAGNI. If a future cut adds more MMA buffers
// (spritesheet, OSD layer), generalize to a small array then.
static void* registered_canvas_vir = NULL;
static MI_PHY registered_canvas_phy = 0;
static size_t registered_canvas_size = 0;

// ---------------------------------------------------------------------
// Public mini-API consumed by software_renderer.c (substep 4) is now
// declared in arm_display_mi_gfx.h (included above) — single source of
// truth for the symbol prototype.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// arm_display backend entry points
// ---------------------------------------------------------------------

bool arm_display_mi_gfx_init(void);
void arm_display_mi_gfx_shutdown(void);
void arm_display_mi_gfx_get_resolution(int* out_width, int* out_height);
void arm_display_mi_gfx_present(const uint32_t* pixels, int sw_in, int sh_in);

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static void mi_gfx_log_failure(const char* tag, MI_S32 rc) {
    fprintf(stderr, "[arm_display_mi_gfx] %s failed (rc=0x%08x)\n", tag, (unsigned)rc);
}

static void mi_gfx_release_partial(void) {
    if (scratch_vir != NULL) {
        MI_SYS_Munmap(scratch_vir, (MI_U32)scratch_size);
        scratch_vir = NULL;
    }

    if (scratch_phy != 0) {
        MI_SYS_MMA_Free(scratch_phy);
        scratch_phy = 0;
    }

    if (fb_vir_addr != NULL) {
        MI_SYS_Munmap(fb_vir_addr, (MI_U32)fb_size);
        fb_vir_addr = NULL;
    }

    if (fb_dev_fd >= 0) {
        // Best-effort restore yoffset to 0 before closing.
        fb_var.yoffset = 0;
        ioctl(fb_dev_fd, FBIOPUT_VSCREENINFO, &fb_var);
        close(fb_dev_fd);
        fb_dev_fd = -1;
    }

    if (gfx_opened) {
        MI_GFX_Close();
        gfx_opened = false;
    }

    if (mi_initialized) {
        MI_SYS_Exit();
        mi_initialized = false;
    }
}

// ---------------------------------------------------------------------
// MMA canvas accessor (consumed by software_renderer.c when
// PORT_MIYOO_MINI_PLUS && CRS_ARM_HAVE_MI_GFX). The software renderer
// calls this in place of malloc() for the giblet canvas so MI_GFX can
// blit directly without a memcpy.
// ---------------------------------------------------------------------

bool mi_gfx_alloc_canvas(size_t bytes, void** out_vir, unsigned long long* out_phy) {
    if (out_vir == NULL || out_phy == NULL) {
        return false;
    }

    if (!mi_initialized) {
        // Should not happen — arm_display_mi_gfx_init must have run
        // before SoftwareRenderer_Init. Log and refuse.
        fprintf(stderr, "[arm_display_mi_gfx] mi_gfx_alloc_canvas called before init\n");
        return false;
    }

    if (registered_canvas_vir != NULL) {
        // Idempotent: if SoftwareRenderer_Init is called twice without
        // an intervening _Quit and the request is for the same byte
        // count, hand back the existing pair instead of aborting. A
        // size mismatch is still fatal — that's a programming error.
        if (bytes == registered_canvas_size) {
            *out_vir = registered_canvas_vir;
            *out_phy = (unsigned long long)registered_canvas_phy;
            return true;
        }
        fprintf(stderr,
                "[arm_display_mi_gfx] mi_gfx_alloc_canvas: slot already in use "
                "(have %zu bytes, requested %zu)\n",
                registered_canvas_size, bytes);
        return false;
    }

    MI_PHY phy = 0;
    MI_S32 rc = MI_SYS_MMA_Alloc(NULL, (MI_U32)bytes, &phy);

    if (rc != MI_SUCCESS) {
        mi_gfx_log_failure("MI_SYS_MMA_Alloc(canvas)", rc);
        return false;
    }

    void* vir = NULL;
    rc = MI_SYS_Mmap(phy, (MI_U32)bytes, &vir, TRUE);

    if (rc != MI_SUCCESS || vir == NULL) {
        mi_gfx_log_failure("MI_SYS_Mmap(canvas)", rc);
        MI_SYS_MMA_Free(phy);
        return false;
    }

    registered_canvas_vir = vir;
    registered_canvas_phy = phy;
    registered_canvas_size = bytes;

    *out_vir = vir;
    *out_phy = (unsigned long long)phy;
    return true;
}

void mi_gfx_free_canvas(void* vir) {
    if (vir == NULL) {
        return;
    }

    if (vir != registered_canvas_vir) {
        fprintf(stderr, "[arm_display_mi_gfx] mi_gfx_free_canvas: unknown pointer\n");
        return;
    }

    MI_SYS_Munmap(registered_canvas_vir, (MI_U32)registered_canvas_size);
    MI_SYS_MMA_Free(registered_canvas_phy);

    registered_canvas_vir = NULL;
    registered_canvas_phy = 0;
    registered_canvas_size = 0;
}

// ---------------------------------------------------------------------
// arm_display_mi_gfx_init
// ---------------------------------------------------------------------

// BOOT_LOG: progress marker macro for first-light bring-up. Writes to
// stderr with explicit fflush so nothing is lost if the next call
// hangs. Also writes an unbuffered copy to /mnt/SDCARD/Roms/PORTS/Games/3s-arm/boot.log
// (O_SYNC) so SD-card forensics work when stderr can't be observed.
//
// Cut 1 keeps these on permanently to support remote-debug flows; the
// volume is tiny (one or two lines per init step) and the cost is a
// handful of fsync() per boot.
static int boot_log_fd = -1;
static void boot_log_init(void) {
    if (boot_log_fd >= 0) return;
    setvbuf(stderr, NULL, _IONBF, 0);
    boot_log_fd = open("/mnt/SDCARD/Roms/PORTS/Games/3s-arm/boot.log",
                       O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
}
static void boot_log_emit(const char* tag, const char* fmt, ...) {
    boot_log_init();
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "[BOOT %s] ", tag);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof(buf) - 1 && buf[n - 1] != '\n') {
        buf[n++] = '\n';
        buf[n] = '\0';
    }
    fputs(buf, stderr);
    fflush(stderr);
    if (boot_log_fd >= 0) {
        ssize_t _ = write(boot_log_fd, buf, (size_t)n);
        (void)_;
        fsync(boot_log_fd);
    }
}
#define BL(fmt, ...) boot_log_emit("mi_gfx", fmt, ##__VA_ARGS__)

bool arm_display_mi_gfx_init(void) {
    BL("init entered");

    {
        const char* nv = getenv("THIRDSARM_NO_VSYNC");
        no_vsync = (nv != NULL && nv[0] == '1');
        BL("THIRDSARM_NO_VSYNC=%s -> no_vsync=%d",
           nv ? nv : "(unset)", (int)no_vsync);
    }

    BL("calling MI_SYS_Init");
    MI_S32 rc = MI_SYS_Init();
    BL("MI_SYS_Init returned rc=0x%08x", (unsigned)rc);

    if (rc != MI_SUCCESS) {
        mi_gfx_log_failure("MI_SYS_Init", rc);
        return false;
    }

    mi_initialized = true;

    BL("calling MI_GFX_Open");
    rc = MI_GFX_Open();
    BL("MI_GFX_Open returned rc=0x%08x", (unsigned)rc);

    if (rc != MI_SUCCESS) {
        mi_gfx_log_failure("MI_GFX_Open", rc);
        mi_gfx_release_partial();
        return false;
    }

    gfx_opened = true;

    BL("opening %s", FBDEV_PATH);
    fb_dev_fd = open(FBDEV_PATH, O_RDWR | O_CLOEXEC);
    BL("fb open returned fd=%d", fb_dev_fd);

    if (fb_dev_fd < 0) {
        fprintf(stderr, "[arm_display_mi_gfx] open(%s) failed: %s\n", FBDEV_PATH, strerror(errno));
        mi_gfx_release_partial();
        return false;
    }

    BL("FBIOGET_VSCREENINFO + FBIOGET_FSCREENINFO");
    if (ioctl(fb_dev_fd, FBIOGET_VSCREENINFO, &fb_var) != 0
        || ioctl(fb_dev_fd, FBIOGET_FSCREENINFO, &fb_fix) != 0) {
        fprintf(stderr, "[arm_display_mi_gfx] FBIOGET_*SCREENINFO failed: %s\n", strerror(errno));
        mi_gfx_release_partial();
        return false;
    }

    panel_w = (int)fb_var.xres;
    panel_h = (int)fb_var.yres;
    BL("panel=%dx%d, bpp=%u, line_length=%u, smem_start=0x%lx, smem_len=%u",
        panel_w, panel_h, fb_var.bits_per_pixel, fb_fix.line_length,
        (unsigned long)fb_fix.smem_start, fb_fix.smem_len);

    if (panel_w <= 0 || panel_h <= 0) {
        fprintf(stderr, "[arm_display_mi_gfx] invalid panel resolution %dx%d\n", panel_w, panel_h);
        mi_gfx_release_partial();
        return false;
    }

    fb_var.yoffset = 0;
    // In tearing mode keep yres_virtual == yres (single buffer); we'll
    // write directly into the visible region and skip FBIOPAN_DISPLAY.
    fb_var.yres_virtual = no_vsync ? fb_var.yres : (fb_var.yres * 2);
    BL("FBIOPUT_VSCREENINFO yres_virtual=%u (no_vsync=%d)",
       fb_var.yres_virtual, (int)no_vsync);

    if (ioctl(fb_dev_fd, FBIOPUT_VSCREENINFO, &fb_var) != 0) {
        fprintf(stderr, "[arm_display_mi_gfx] FBIOPUT_VSCREENINFO(double-buffer) failed: %s\n",
                strerror(errno));
        mi_gfx_release_partial();
        return false;
    }

    fb_phy_addr = (MI_PHY)fb_fix.smem_start;
    fb_size = (size_t)fb_fix.line_length * (size_t)fb_var.yres_virtual;
    BL("fb_phy_addr=0x%lx fb_size=%zu", (unsigned long)fb_phy_addr, fb_size);

    BL("MI_SYS_Mmap(fb)");
    rc = MI_SYS_Mmap(fb_phy_addr, (MI_U32)fb_size, &fb_vir_addr, FALSE);
    BL("MI_SYS_Mmap(fb) returned rc=0x%08x vir=%p", (unsigned)rc, fb_vir_addr);

    if (rc != MI_SUCCESS || fb_vir_addr == NULL) {
        mi_gfx_log_failure("MI_SYS_Mmap(framebuffer)", rc);
        mi_gfx_release_partial();
        return false;
    }

    BL("MI_SYS_MemsetPa(fb clear)");
    rc = MI_SYS_MemsetPa(fb_phy_addr, 0, (MI_U32)fb_size);
    BL("MI_SYS_MemsetPa returned rc=0x%08x", (unsigned)rc);

    if (rc != MI_SUCCESS) {
        mi_gfx_log_failure("MI_SYS_MemsetPa(fb clear)", rc);
    }

    int lb_x = 0;
    int lb_y = 0;
    int lb_w = 0;
    int lb_h = 0;
    ArmDisplay_ComputePresentRect(panel_w, panel_h, &lb_x, &lb_y, &lb_w, &lb_h);
    lb_rect.s32Xpos = lb_x;
    lb_rect.s32Ypos = lb_y;
    lb_rect.u32Width = (MI_U32)lb_w;
    lb_rect.u32Height = (MI_U32)lb_h;
    BL("letterbox=(%d,%d %dx%d)", lb_x, lb_y, lb_w, lb_h);

    scratch_size = (size_t)CANVAS_W_MAX * CANVAS_H_MAX * CANVAS_BPP;
    BL("MI_SYS_MMA_Alloc(scratch=%zu)", scratch_size);
    rc = MI_SYS_MMA_Alloc(NULL, (MI_U32)scratch_size, &scratch_phy);
    BL("MI_SYS_MMA_Alloc returned rc=0x%08x phy=0x%lx", (unsigned)rc, (unsigned long)scratch_phy);

    if (rc != MI_SUCCESS) {
        mi_gfx_log_failure("MI_SYS_MMA_Alloc(scratch)", rc);
        mi_gfx_release_partial();
        return false;
    }

    BL("MI_SYS_Mmap(scratch)");
    rc = MI_SYS_Mmap(scratch_phy, (MI_U32)scratch_size, &scratch_vir, TRUE);
    BL("MI_SYS_Mmap(scratch) returned rc=0x%08x vir=%p", (unsigned)rc, scratch_vir);

    if (rc != MI_SUCCESS || scratch_vir == NULL) {
        mi_gfx_log_failure("MI_SYS_Mmap(scratch)", rc);
        mi_gfx_release_partial();
        return false;
    }

    BL("init OK: panel=%dx%d, lb=(%d,%d %ux%u)",
       panel_w, panel_h, lb_rect.s32Xpos, lb_rect.s32Ypos,
       lb_rect.u32Width, lb_rect.u32Height);
    fprintf(stderr,
            "[arm_display_mi_gfx] init OK: panel=%dx%d, lb=(%d,%d %ux%u)\n",
            panel_w, panel_h, lb_rect.s32Xpos, lb_rect.s32Ypos,
            lb_rect.u32Width, lb_rect.u32Height);
    return true;
}

// ---------------------------------------------------------------------
// arm_display_mi_gfx_shutdown
// ---------------------------------------------------------------------

void arm_display_mi_gfx_shutdown(void) {
    // Note: the registered canvas is freed by SoftwareRenderer_Quit
    // through mi_gfx_free_canvas BEFORE this runs (see sdl_app.c
    // ordering). If for any reason it's still live, leak it
    // intentionally rather than free with MI_SYS already torn down.

    mi_gfx_release_partial();
}

// ---------------------------------------------------------------------
// arm_display_mi_gfx_get_resolution
// ---------------------------------------------------------------------

void arm_display_mi_gfx_get_resolution(int* out_width, int* out_height) {
    if (out_width != NULL) {
        *out_width = panel_w;
    }

    if (out_height != NULL) {
        *out_height = panel_h;
    }
}

// ---------------------------------------------------------------------
// arm_display_mi_gfx_present
// ---------------------------------------------------------------------
//
// Hot path. The caller's `pixels` parameter is declared as
// `const uint32_t*` to match the existing ArmDisplay_Present signature,
// but on the Miyoo profile the giblet canvas is RGB565 packed
// (CRS_SW_CANVAS_16BPP=1). The reinterpretation is safe because the
// arm_display selector is locked to the MI_GFX backend under
// CRS_ARM_HAVE_MI_GFX (substep 5); DRM and fbdev would interpret the
// bytes as ARGB and produce garbage. See arm_display.c for the
// hard-fail rationale.
//
// Strategy: if `pixels` matches the registered MMA canvas, blit
// directly from its physical address (zero-copy fast path). Otherwise,
// memcpy into the scratch DMA buffer, flush its cache lines, and blit
// from there.

// Per-stage frame-time profiler. Accumulates timings across N frames
// then dumps once. Cheap (4 monotonic_ns reads per frame, 1 division at
// the dump). Cut 1 keeps it on; can gate behind a build flag in Cut 2.
#include <time.h>
static inline uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#define PROFILE_FRAMES 120
static uint64_t prof_flush_ns = 0;
static uint64_t prof_blit_ns = 0;
static uint64_t prof_wait_ns = 0;
static uint64_t prof_pan_ns = 0;
static uint64_t prof_interval_ns = 0;
static uint64_t prof_last_call_ns = 0;
static int prof_frames = 0;
static int prof_zero_copy_frames = 0;

void arm_display_mi_gfx_present(const uint32_t* pixels, int sw_in, int sh_in) {
    if (pixels == NULL || sw_in <= 0 || sh_in <= 0) {
        return;
    }

    if (fb_dev_fd < 0 || !gfx_opened) {
        return;
    }

    if (sw_in > CANVAS_W_MAX || sh_in > CANVAS_H_MAX) {
        // Source larger than the scratch buffer; refuse the fallback
        // path. The zero-copy fast path is still safe because it does
        // not use the scratch buffer.
        if (pixels != registered_canvas_vir) {
            fprintf(stderr,
                    "[arm_display_mi_gfx] source %dx%d exceeds scratch %dx%d (fallback path)\n",
                    sw_in, sh_in, CANVAS_W_MAX, CANVAS_H_MAX);
            return;
        }
    }

    const size_t src_bytes = (size_t)sw_in * (size_t)sh_in * CANVAS_BPP;
    MI_PHY src_phy = 0;

    const uint64_t t0 = mono_ns();
    if (prof_last_call_ns != 0) {
        prof_interval_ns += (t0 - prof_last_call_ns);
    }
    prof_last_call_ns = t0;
    bool zero_copy = false;

    if (registered_canvas_vir != NULL && (const void*)pixels == registered_canvas_vir) {
        zero_copy = true;
        MI_S32 rc = MI_SYS_FlushInvCache(registered_canvas_vir, (MI_U32)src_bytes);
        if (rc != MI_SUCCESS) {
            mi_gfx_log_failure("MI_SYS_FlushInvCache(canvas)", rc);
            return;
        }
        src_phy = registered_canvas_phy;
    } else {
        memcpy(scratch_vir, pixels, src_bytes);
        MI_S32 rc = MI_SYS_FlushInvCache(scratch_vir, (MI_U32)src_bytes);
        if (rc != MI_SUCCESS) {
            mi_gfx_log_failure("MI_SYS_FlushInvCache(scratch)", rc);
            return;
        }
        src_phy = scratch_phy;
    }

    const uint64_t t1 = mono_ns();

    MI_GFX_Surface_t src_surf = { 0 };
    src_surf.eColorFmt = E_MI_GFX_FMT_RGB565;
    src_surf.u32Width = (MI_U32)sw_in;
    src_surf.u32Height = (MI_U32)sh_in;
    src_surf.u32Stride = (MI_U32)sw_in * CANVAS_BPP;  // tightly packed
    src_surf.phyAddr = src_phy;

    MI_GFX_Rect_t src_rect = { 0, 0, (MI_U32)sw_in, (MI_U32)sh_in };

    MI_GFX_Surface_t dst_surf = { 0 };
    dst_surf.eColorFmt = E_MI_GFX_FMT_ARGB8888;
    dst_surf.u32Width = (MI_U32)panel_w;
    dst_surf.u32Height = (MI_U32)panel_h;
    dst_surf.u32Stride = (MI_U32)fb_fix.line_length;
    dst_surf.phyAddr = fb_phy_addr + (MI_PHY)((MI_U32)fb_var.yoffset * fb_fix.line_length);

    MI_GFX_Opt_t opt = { 0 };
    opt.u32GlobalSrcConstColor = 0;
    opt.eRotate = E_MI_GFX_ROTATE_180;     // panel mounted upside-down
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
    opt.eDstDfbBldOp = E_MI_GFX_DFB_BLD_ZERO;
    opt.eDFBBlendFlag = E_MI_GFX_DFB_BLEND_NOFX;

    MI_U16 fence = 0;
    MI_S32 rc = MI_GFX_BitBlit(&src_surf, &src_rect, &dst_surf, &lb_rect, &opt, &fence);

    if (rc != MI_SUCCESS) {
        mi_gfx_log_failure("MI_GFX_BitBlit", rc);
        return;
    }

    const uint64_t t2 = mono_ns();

    // Vanilla mmiyoo (steward-fu/sdl2 SDL_video_mini.c:153) passes TRUE.
    // Earlier Cut 1 used FALSE — possible cause of FBIOPAN_DISPLAY blocking
    // on incomplete blits. Keep aligned with vanilla.
    rc = MI_GFX_WaitAllDone(TRUE, fence);

    if (rc != MI_SUCCESS) {
        mi_gfx_log_failure("MI_GFX_WaitAllDone", rc);
        return;
    }

    const uint64_t t3 = mono_ns();

    // Page flip + alternate yoffset for next frame. Skipped in tearing
    // mode (no_vsync): single-buffer scan-out, write straight to visible
    // region, accept tear lines for the 11ms-per-frame win.
    if (!no_vsync) {
        if (ioctl(fb_dev_fd, FBIOPAN_DISPLAY, &fb_var) != 0) {
            static bool warned = false;
            if (!warned) {
                fprintf(stderr, "[arm_display_mi_gfx] FBIOPAN_DISPLAY failed: %s (further warnings suppressed)\n",
                        strerror(errno));
                warned = true;
            }
        }
    }

    const uint64_t t4 = mono_ns();

    if (!no_vsync) {
        fb_var.yoffset = (fb_var.yoffset == 0) ? (unsigned)panel_h : 0u;
    }

    // Profiler: accumulate per-stage timings, dump every PROFILE_FRAMES.
    prof_flush_ns += (t1 - t0);
    prof_blit_ns += (t2 - t1);
    prof_wait_ns += (t3 - t2);
    prof_pan_ns += (t4 - t3);
    prof_frames++;
    if (zero_copy) prof_zero_copy_frames++;
    if (prof_frames >= PROFILE_FRAMES) {
        const double f = (double)prof_frames;
        const double avg_interval_ms = (prof_interval_ns / 1e6) / f;
        const double fps = avg_interval_ms > 0.0 ? (1000.0 / avg_interval_ms) : 0.0;
        fprintf(stderr,
                "[mi_gfx_perf] frames=%d zc=%d  flush=%.2f blit=%.2f wait=%.2f pan=%.2f present_total=%.2f  frame_interval=%.2fms fps=%.1f\n",
                prof_frames, prof_zero_copy_frames,
                prof_flush_ns / 1e6 / f,
                prof_blit_ns  / 1e6 / f,
                prof_wait_ns  / 1e6 / f,
                prof_pan_ns   / 1e6 / f,
                (prof_flush_ns + prof_blit_ns + prof_wait_ns + prof_pan_ns) / 1e6 / f,
                avg_interval_ms, fps);
        fflush(stderr);
        prof_flush_ns = prof_blit_ns = prof_wait_ns = prof_pan_ns = 0;
        prof_interval_ns = 0;
        prof_frames = 0;
        prof_zero_copy_frames = 0;
    }
}

#endif // CRS_APP_DRIVER_ARM && CRS_ARM_HAVE_MI_GFX
