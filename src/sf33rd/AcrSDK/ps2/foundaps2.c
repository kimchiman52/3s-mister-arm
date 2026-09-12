#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "common.h"
#include "netplay/netplay.h"
#include "port/utils.h"
#include "sf33rd/AcrSDK/MiddleWare/PS2/CapSndEng/cse.h"
#include "sf33rd/AcrSDK/common/fbms.h"
#include "sf33rd/AcrSDK/common/memfound.h"
#include "sf33rd/AcrSDK/common/mlPAD.h"
#include "sf33rd/AcrSDK/common/prilay.h"
#include "sf33rd/AcrSDK/ps2/flps2debug.h"
#include "sf33rd/AcrSDK/ps2/flps2etc.h"
#include "sf33rd/AcrSDK/ps2/flps2render.h"
#include "sf33rd/AcrSDK/ps2/flps2vram.h"
#include "sf33rd/AcrSDK/ps2/ps2PAD.h"
#include "structs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FLPS2State flPs2State;
FLTexture flTexture[256];
FLTexture flPalette[1088];
s32 flWidth;
s32 flHeight;
u32 flSystemRenderOperation;
FL_FMS flFMS;
s32 flVramStaticNum;
u32 flDebugStrHan;
u32 flDebugStrCol;
u32 flDebugStrCtr;

// forward decls
static s32 system_work_init();
static void flPS2InitRenderBuff();

s32 flInitialize() {
    if (system_work_init() == 0) {
        return 0;
    }

    flPS2SystemTmpBuffInit();
    flPS2InitRenderBuff();
    flPADInitialize();
    flPS2DebugInit();

    return 1;
}

static s32 system_work_init() {
    void* temp;

    flMemset(&flPs2State, 0, sizeof(FLPS2State));
    temp = malloc(0x01800000);

    if (temp == NULL) {
        return 0;
    }

    fmsInitialize(&flFMS, temp, 0x01800000, 0x40);
    const int system_memory_size = 0xA00000;
    temp = flAllocMemoryS(system_memory_size);
    mflInit(temp, system_memory_size, 0x40);

    return 1;
}

s32 flFlip(u32 flag) {
    flPS2SystemTmpBuffFlush();
    cseExecServer(); // FIXME: This shouldn't be called from multiple places
    return 1;
}

static void flPS2InitRenderBuff() {
    s32 width;
    s32 height;
    s32 disp_height;

    width = 512;
    height = 448;
    disp_height = 448;
    flWidth = width;
    flHeight = height;
    flPs2State.DispWidth = width;
    flPs2State.DispHeight = disp_height;
    flPs2State.ZBuffMax = (f32)65535;
}

/* Engine diagnostics. Every caller is on the game thread INSIDE the
 * simulation (ramcnt.c, texgroup.c, PPGFile.c, mtrans.c, bg.c, gd3rd.c ...),
 * which under netplay is replayed on every rollback. So while a session sink
 * is live the line is handed to the non-blocking deferred sink
 * (Netplay_LogGameplayDiagnostic: queued for the logger thread, or refused
 * and counted -- never silently lost, never a write on this thread). With no
 * session sink the behaviour is the original one: the formatted text plus
 * "\r\n" written to stderr, byte for byte.
 *
 * Removed: the PS2-era flFileWrite/flFileAppend("../acrout.txt") pair. On
 * this port they built "cdrom0:\THIRD\..\ACROUT.TXT;1" and issued an open(2)
 * that failed with ENOENT on every single call -- one wasted syscall per
 * line, on the game thread, for a file that could never exist. */
s32 flLogOut(s8* format, ...) {
    s8 str[2048];
    size_t len;

    va_list args;
    va_start(args, format);
    /* Leave room for "\r\n" below; the original wrote them past the buffer
     * when the message filled it. */
    vsnprintf(str, sizeof(str) - 3, format, args);
    va_end(args);

    len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r')) {
        str[--len] = '\0';
    }

    if (Netplay_LogGameplayDiagnostic(str)) {
        return 1;
    }

    str[len++] = '\r';
    str[len++] = '\n';
    str[len] = '\0';
    fprintf(stderr, "%s", str);
    return 1;
}
