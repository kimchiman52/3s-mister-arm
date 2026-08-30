#ifndef PORT_IO_AFS_H
#define PORT_IO_AFS_H

#include "port/build_config.h"

#include <stdbool.h>

typedef enum AFSReadState {
    AFS_READ_STATE_IDLE,
    AFS_READ_STATE_READING,
    AFS_READ_STATE_FINISHED,
    AFS_READ_STATE_ERROR
} AFSReadState;

typedef int AFSHandle;

#define AFS_NONE -1

bool AFS_Init(const char* file_path);
void AFS_Finish();
unsigned int AFS_GetFileCount();
unsigned int AFS_GetSize(int file_num);

/// Synchronous byte-range read of one archive member: reads `size` bytes
/// starting `offset` bytes into file `file_num`, independent of the async
/// request-slot machinery above. Used by the boot-time arcade-balance
/// adaptation to read only each character's PS2 char-data tail instead of
/// whole multi-megabyte texture-group files. Returns false on any
/// out-of-range or I/O failure.
bool AFS_ReadRange(int file_num, unsigned int offset, unsigned int size, void* buf);

void AFS_RunServer();

/// Blocking sibling of AFS_RunServer(): waits up to `timeout_ms` for at
/// least one async completion to arrive, processes it, then drains
/// whatever else is already ready. Returns true if anything was
/// processed. Used by the netplay LDREQ frame barrier
/// (Check_LDREQ_Queue(), gd3rd.c) to finish a load inside the simulated
/// frame that issued it, instead of letting the simulation observe a
/// wall-clock-dependent completion frame. A timeout of 0 makes this a
/// non-blocking poll, i.e. exactly AFS_RunServer().
bool AFS_PumpBlocking(int timeout_ms);

/// Monotonic count of bytes handed to SDL_ReadAsyncIO/AFS_Read since
/// process start. Used by the netplay LDREQ frame barrier to report how
/// much I/O a barrier stall actually covered, so the cost of the stall
/// is a measured number in the field log rather than an estimate.
unsigned long long AFS_GetTotalBytesRequested(void);

/// TEST-ONLY (harness): hold back the *observed* completion of every
/// subsequent AFS_Read() by `ms` milliseconds. The physical read is
/// unchanged; only AFS_GetState() keeps reporting AFS_READ_STATE_READING
/// until the injected delay has elapsed. This models the one variable
/// two netplay peers can never agree on — how long their disk takes —
/// so a single-process run can reproduce the cross-peer condition. Set
/// by --afs-inject-latency-ms; 0 (the default) is a no-op and leaves
/// every code path below byte-identical to an uninstrumented build.
void AFS_SetInjectedLatencyMs(int ms);
int AFS_GetInjectedLatencyMs(void);

#if ENABLE_PERF_TELEMETRY
/// Task #141 boot-stall attribution: per-frame ledger of the blocking
/// archive reads (AFS_ReadSync, AFS_ReadRange) issued inside one game
/// frame. src/main.c resets it at the top of game_step_0() and reports it
/// on the [step0] line it prints when a frame exceeds 50 ms, which is the
/// same threshold sdl_app.c's FRAME OUTLIER line uses. Diagnostic only.
void AFS_ResetSyncReadLedger(void);
void AFS_GetSyncReadLedger(unsigned long long* ns_out, unsigned long long* bytes_out, unsigned int* count_out);

/// Task #69.3 probe. `*open_out` receives the number of request slots
/// that are currently allocated (AFS_Open'd and not yet fully closed);
/// the return value is how many of those are still AFS_READ_STATE_READING,
/// i.e. reads genuinely in flight. Read-only over requests[]; exists so
/// the session-start skew probe in gd3rd.c can report "is there a read in
/// flight at this instant" as a measurement rather than an inference.
int AFS_GetInFlightCount(int* open_out);
#endif

AFSHandle AFS_Open(int file_num);
void AFS_Read(AFSHandle handle, int sectors, void* buf);
void AFS_ReadSync(AFSHandle handle, int sectors, void* buf);
void AFS_Stop(AFSHandle handle);
void AFS_Close(AFSHandle handle);
AFSReadState AFS_GetState(AFSHandle handle);
unsigned int AFS_GetSectorCount(AFSHandle handle);

#endif
