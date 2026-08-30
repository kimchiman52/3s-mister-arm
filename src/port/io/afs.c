#include "port/io/afs.h"
#include "common.h"
#include <SDL3/SDL.h>
#include <stdio.h>

// Inspired by https://github.com/MaikelChan/AFSLib

#define AFS_MAGIC 0x41465300
#define AFS_ATTRIBUTE_HEADER_SIZE 8
#define AFS_ATTRIBUTE_ENTRY_SIZE 48
#define AFS_MAX_NAME_LENGTH 32

#define AFS_MAX_READ_REQUESTS 100

// Uncomment this to enable debug prints
// #define AFS_DEBUG

typedef struct AFSEntry {
    unsigned int offset;
    unsigned int size;
    char name[AFS_MAX_NAME_LENGTH];
} AFSEntry;

typedef struct AFS {
    char* file_path;
    unsigned int entry_count;
    AFSEntry* entries;
} AFS;

typedef struct ReadRequest {
    bool initialized;
    bool close_queued;
    bool close_pending;
    int index;
    int file_num;
    int sector;
    AFSReadState state;
    SDL_AsyncIO* asyncio;
    /* TEST-ONLY (--afs-inject-latency-ms). Earliest SDL_GetTicksNS() at
     * which AFS_GetState() is allowed to stop reporting READING for this
     * slot. Zero whenever injection is off, which makes the gate in
     * AFS_GetState() a compare against 0 that is never taken. */
    Uint64 release_ticks_ns;
} ReadRequest;

static AFS afs = { 0 };
static SDL_AsyncIOQueue* asyncio_queue = NULL;
static ReadRequest requests[AFS_MAX_READ_REQUESTS] = { { 0 } };
static const int asyncio_completion_budget_per_tick = 32;
/* TEST-ONLY: see AFS_SetInjectedLatencyMs() in afs.h. */
static int afs_injected_latency_ms = 0;
/* Monotonic bytes-requested counter; see AFS_GetTotalBytesRequested(). */
static unsigned long long afs_total_bytes_requested = 0;

static bool is_valid_attribute_data(Uint32 attributes_offset, Uint32 attributes_size, Sint64 file_size,
                                    Uint32 entries_end_offset, Uint32 entry_count) {
    if ((attributes_offset == 0) || (attributes_size == 0)) {
        return false;
    }

    if (attributes_size > (file_size - entries_end_offset)) {
        return false;
    }

    if (attributes_size < (entry_count * AFS_ATTRIBUTE_ENTRY_SIZE)) {
        return false;
    }

    if (attributes_offset < entries_end_offset) {
        return false;
    }

    if (attributes_offset > (file_size - attributes_size)) {
        return false;
    }

    return true;
}

static void read_string(SDL_IOStream* src, char* dst) {
    char c;

    do {
        SDL_ReadIO(src, &c, 1);
        *dst++ = c;
    } while (c != '\0');
}

static bool init_afs(const char* file_path) {
    afs.file_path = SDL_strdup(file_path);
    SDL_IOStream* io = SDL_IOFromFile(file_path, "rb");

    if (io == NULL) {
        return false;
    }

    // Check magic

    Uint32 magic = 0;
    SDL_ReadU32BE(io, &magic);

    if (magic != AFS_MAGIC) {
        SDL_CloseIO(io);
        return false;
    }

    // Read entries

    SDL_ReadU32LE(io, &afs.entry_count);
    afs.entries = SDL_malloc(sizeof(AFSEntry) * afs.entry_count);

    Uint32 entries_start_offset = 0;
    Uint32 entries_end_offset = 0;

    for (int i = 0; i < afs.entry_count; i++) {
        AFSEntry* entry = &afs.entries[i];
        SDL_ReadU32LE(io, &entry->offset);
        SDL_ReadU32LE(io, &entry->size);

        if (entry->offset != 0) {
            if (entries_start_offset == 0) {
                entries_start_offset = entry->offset;
            }

            entries_end_offset = entry->offset + entry->size;
        }
    }

    // Locate attributes

    Uint32 attributes_offset;
    Uint32 attributes_size;
    bool has_attributes = false;

    SDL_ReadU32LE(io, &attributes_offset);
    SDL_ReadU32LE(io, &attributes_size);

    if (is_valid_attribute_data(
            attributes_offset, attributes_size, SDL_GetIOSize(io), entries_end_offset, afs.entry_count)) {
        has_attributes = true;
    } else {
        SDL_SeekIO(io, entries_start_offset - AFS_ATTRIBUTE_HEADER_SIZE, SDL_IO_SEEK_SET);

        SDL_ReadU32LE(io, &attributes_offset);
        SDL_ReadU32LE(io, &attributes_size);

        if (is_valid_attribute_data(
                attributes_offset, attributes_size, SDL_GetIOSize(io), entries_end_offset, afs.entry_count)) {
            has_attributes = true;
        }
    }

    for (int i = 0; i < afs.entry_count; i++) {
        AFSEntry* entry = &afs.entries[i];

        if ((entry->offset != 0) && has_attributes) {
            SDL_SeekIO(io, attributes_offset + i * AFS_ATTRIBUTE_ENTRY_SIZE, SDL_IO_SEEK_SET);
            read_string(io, &entry->name);
        } else {
            SDL_memset(&entry->name, 0, sizeof(entry->name));
        }
    }

    SDL_CloseIO(io);
    return true;
}

static bool init_asyncio() {
    asyncio_queue = SDL_CreateAsyncIOQueue();
    return asyncio_queue != NULL;
}

bool AFS_Init(const char* file_path) {
    if (!init_afs(file_path)) {
        return false;
    }

    return init_asyncio();
}

static void process_asyncio_outcome(const SDL_AsyncIOOutcome* outcome);

void AFS_Finish() {
    for (int i = 0; i < SDL_arraysize(requests); i++) {
        if (requests[i].initialized) {
            AFS_Close(i);
        }
    }

    // Drain any pending close completions before destroying the queue.
    for (;;) {
        bool any_pending = false;
        for (int i = 0; i < SDL_arraysize(requests); i++) {
            if (requests[i].close_queued) {
                any_pending = true;
                break;
            }
        }
        if (!any_pending) break;

        SDL_AsyncIOOutcome outcome;
        if (!SDL_WaitAsyncIOResult(asyncio_queue, &outcome, -1)) {
            break;
        }
        process_asyncio_outcome(&outcome);
    }

    SDL_free(afs.file_path);
    SDL_free(afs.entries);
    SDL_zero(afs);
    SDL_zeroa(requests);
    SDL_DestroyAsyncIOQueue(asyncio_queue);
    asyncio_queue = NULL;
}

unsigned int AFS_GetFileCount() {
    return afs.entry_count;
}

unsigned int AFS_GetSize(int file_num) {
    if ((file_num < 0) || (file_num >= afs.entry_count)) {
        return 0;
    }

    return afs.entries[file_num].size;
}

bool AFS_ReadRange(int file_num, unsigned int offset, unsigned int size, void* buf) {
    if ((file_num < 0) || (file_num >= afs.entry_count) || (buf == NULL)) {
        return false;
    }

    const AFSEntry* entry = &afs.entries[file_num];

    if ((offset > entry->size) || (size > entry->size - offset)) {
        return false;
    }

    SDL_IOStream* io = SDL_IOFromFile(afs.file_path, "rb");

    if (io == NULL) {
        return false;
    }

    bool ok = false;

    if (SDL_SeekIO(io, (Sint64)entry->offset + offset, SDL_IO_SEEK_SET) >= 0) {
        ok = SDL_ReadIO(io, buf, size) == size;
    }

    SDL_CloseIO(io);
    return ok;
}

// AFS reading

static void process_asyncio_outcome(const SDL_AsyncIOOutcome* outcome) {
    ReadRequest* request = (ReadRequest*)outcome->userdata;
    if ((request == NULL) || !request->initialized) {
        return;
    }

#if defined(AFS_DEBUG)
    printf("📂 %d: request complete (type = %d, result = %d, offset = 0x%llX, requested = 0x%llX, transferred = "
           "0x%llX)\n",
           request->index,
           outcome->type,
           outcome->result,
           outcome->offset,
           outcome->bytes_requested,
           outcome->bytes_transferred);
#endif

    switch (outcome->type) {
    case SDL_ASYNCIO_TASK_READ:
        switch (outcome->result) {
        case SDL_ASYNCIO_COMPLETE:
            request->state = request->close_queued ? AFS_READ_STATE_READING : AFS_READ_STATE_FINISHED;
            break;

        case SDL_ASYNCIO_CANCELED:
            request->state = request->close_queued ? AFS_READ_STATE_READING : AFS_READ_STATE_IDLE;
            break;

        case SDL_ASYNCIO_FAILURE:
            request->state = AFS_READ_STATE_ERROR;
            break;
        }

        break;

    case SDL_ASYNCIO_TASK_CLOSE:
        request->close_queued = false;
        request->asyncio = NULL;

        if (request->close_pending) {
            SDL_zerop(request);
        } else {
            request->state = AFS_READ_STATE_IDLE;
        }
        break;

    case SDL_ASYNCIO_TASK_WRITE:
        // Do nothing
        break;
    }

#if defined(AFS_DEBUG)
    printf("📂 %d: new state = %d\n", request->index, request->state);
#endif
}

void AFS_RunServer() {
    SDL_AsyncIOOutcome outcome;
    int processed = 0;

    // Keep update latency bounded by draining async completions in chunks.
    while ((processed < asyncio_completion_budget_per_tick) && SDL_GetAsyncIOResult(asyncio_queue, &outcome)) {
        process_asyncio_outcome(&outcome);
        processed += 1;
    }
}

bool AFS_PumpBlocking(int timeout_ms) {
    SDL_AsyncIOOutcome outcome;

    if (asyncio_queue == NULL) {
        return false;
    }

    if (!SDL_WaitAsyncIOResult(asyncio_queue, &outcome, timeout_ms)) {
        return false;
    }

    process_asyncio_outcome(&outcome);
    AFS_RunServer();
    return true;
}

unsigned long long AFS_GetTotalBytesRequested(void) {
    return afs_total_bytes_requested;
}

void AFS_SetInjectedLatencyMs(int ms) {
    afs_injected_latency_ms = (ms > 0) ? ms : 0;
}

int AFS_GetInjectedLatencyMs(void) {
    return afs_injected_latency_ms;
}

#if ENABLE_PERF_TELEMETRY
int AFS_GetInFlightCount(int* open_out) {
    int open_slots = 0;
    int reading = 0;

    for (int i = 0; i < SDL_arraysize(requests); i++) {
        if (!requests[i].initialized) {
            continue;
        }

        open_slots += 1;

        if (requests[i].state == AFS_READ_STATE_READING) {
            reading += 1;
        }
    }

    if (open_out != NULL) {
        *open_out = open_slots;
    }

    return reading;
}
#endif

AFSHandle AFS_Open(int file_num) {
    AFSHandle retval = AFS_NONE;

    for (int i = 0; i < SDL_arraysize(requests); i++) {
        ReadRequest* request = &requests[i];

        if (request->initialized) {
            continue;
        }

        request->file_num = file_num;
        request->sector = 0;
        request->index = i;
        request->state = AFS_READ_STATE_IDLE;
        request->initialized = true;
        retval = i;
        break;
    }

#if defined(AFS_DEBUG)
    printf("📂 %d: open (file_num = %d, filename = %s)\n", retval, file_num, afs.entries[file_num].name);
#endif

    return retval;
}

void AFS_Read(AFSHandle handle, int sectors, void* buf) {
#if defined(AFS_DEBUG)
    printf("📂 %d: read (sectors = %d, bytes = 0x%X)\n", handle, sectors, sectors * 2048);
#endif

    ReadRequest* request = &requests[handle];
    const Uint64 offset = afs.entries[request->file_num].offset + request->sector * 2048;

    if (request->close_queued) {
        request->state = AFS_READ_STATE_ERROR;
        return;
    }

    if (request->asyncio == NULL) {
        request->asyncio = SDL_AsyncIOFromFile(afs.file_path, "r");
    }

    if (request->asyncio == NULL) {
        printf("SDL_AsyncIOFromFile error: %s\n", SDL_GetError());
        request->state = AFS_READ_STATE_ERROR;
        return;
    }

    request->state = AFS_READ_STATE_READING;
    request->release_ticks_ns =
        (afs_injected_latency_ms > 0)
            ? SDL_GetTicksNS() + (Uint64)afs_injected_latency_ms * SDL_NS_PER_MS
            : 0;

    afs_total_bytes_requested += (unsigned long long)sectors * 2048ull;

    const bool success = SDL_ReadAsyncIO(request->asyncio, buf, offset, sectors * 2048, asyncio_queue, request);

    if (!success) {
        printf("SDL_ReadAsyncIO error: %s\n", SDL_GetError());
        request->state = AFS_READ_STATE_ERROR;
        return;
    }

    request->sector += sectors;
}

static bool queue_request_close(ReadRequest* request) {
    if (request->close_queued || (request->asyncio == NULL)) {
        return true;
    }

    SDL_AsyncIO* asyncio = request->asyncio;
    const bool close_queued = SDL_CloseAsyncIO(asyncio, false, asyncio_queue, request);
    if (!close_queued) {
        printf("SDL_CloseAsyncIO error: %s\n", SDL_GetError());
        request->state = AFS_READ_STATE_ERROR;
        return false;
    }

    request->asyncio = NULL;
    request->close_queued = true;
    request->state = AFS_READ_STATE_READING;
    return true;
}

void AFS_ReadSync(AFSHandle handle, int sectors, void* buf) {
#if defined(AFS_DEBUG)
    printf("📂 %d: read sync\n", handle);
#endif

    AFS_Read(handle, sectors, buf);

    /* AFS_Read has three paths that set ERROR and return WITHOUT submitting
     * anything (close already queued, SDL_AsyncIOFromFile failed,
     * SDL_ReadAsyncIO failed). There is then no completion to wait for, and
     * SDL_WaitAsyncIOResult below blocks with an infinite timeout -- so the
     * wait must not be entered at all. The caller polls AFS_GetState() and
     * sees the ERROR, which is what it is there for. */
    if (requests[handle].state == AFS_READ_STATE_ERROR) {
        return;
    }

    /* TEST-ONLY: the injected-latency instrument targets the ASYNC LDREQ
     * pipeline, which is the only place a wall-clock completion frame can
     * leak into the simulation. Blocking reads are already frame-exact.
     * Leaving the delay armed here would also break callers that poll
     * AFS_GetState() right after this returns and re-issue the whole read
     * when it still says READING (load_it_use_this_key's while(1),
     * gd3rd.c:355-379). Disarm it for this slot. */
    requests[handle].release_ticks_ns = 0;

    SDL_AsyncIOOutcome outcome;

    while (SDL_WaitAsyncIOResult(asyncio_queue, &outcome, -1)) {
        /* Identity is read BEFORE process_asyncio_outcome(), and the wait
         * ends only on this slot's own READ. Both halves are load-bearing.
         *
         * process_asyncio_outcome() SDL_zerop()s a slot whose deferred close
         * has just landed (the close_pending branch), which sets
         * request->index back to 0. Reading ->index after that call made
         * every such completion indistinguishable from slot 0's, so
         * AFS_ReadSync(0, ...) returned while its OWN read was still in
         * flight. fsCheckFileReaded() then saw AFS_READ_STATE_READING and
         * reported failure, and load_it_use_this_key() (gd3rd.c:355-380)
         * logged "file load failed" and retried -- which succeeded, because
         * nothing was actually wrong with the file. That is the whole of the
         * five reproducible boot-time load failures: a lost wakeup, not a
         * missing asset.
         *
         * Matching on READ as well as on the slot closes the second half:
         * AFS_Open() reuses the first free slot, so a CLOSE completion left
         * over from that slot's PREVIOUS user carries the same index and
         * would end this wait just as wrongly. */
        const ReadRequest* completed_request = (const ReadRequest*)outcome.userdata;
        const int completed_index = (completed_request != NULL) ? completed_request->index : AFS_NONE;
        const bool completed_read = (outcome.type == SDL_ASYNCIO_TASK_READ);

        process_asyncio_outcome(&outcome);

        if (completed_read && completed_index == handle) {
            break;
        }
    }
}

void AFS_Stop(AFSHandle handle) {
#if defined(AFS_DEBUG)
    printf("📂 %d: stop\n", handle);
#endif

    ReadRequest* request = &requests[handle];

    if (!request->initialized) {
        return;
    }

    queue_request_close(request);
}

void AFS_Close(AFSHandle handle) {
#if defined(AFS_DEBUG)
    printf("📂 %d: close\n", handle);
#endif

    ReadRequest* request = &requests[handle];
    if (!request->initialized) {
        return;
    }

    if (!queue_request_close(request)) {
        // Close failed to queue -- still free the slot to avoid leaking it.
        SDL_zerop(request);
        return;
    }

    if (request->close_queued) {
        // Async close is in flight. Defer slot cleanup to AFS_RunServer()
        // via process_asyncio_outcome() when the close completion arrives.
        request->close_pending = true;
    } else {
        // No async IO was open (asyncio was NULL), close completed
        // synchronously. Free the slot now.
        SDL_zerop(request);
    }
}

AFSReadState AFS_GetState(AFSHandle handle) {
    ReadRequest* request = &requests[handle];

#if defined(AFS_DEBUG)
    printf("📂 %d: get state (%d)\n", handle, request->state);
#endif

    /* TEST-ONLY (--afs-inject-latency-ms): keep reporting READING until
     * the injected delay elapses, so a harness run can reproduce a peer
     * whose disk is slower without touching the real I/O path. Inert
     * (release_ticks_ns == 0) unless injection was armed. */
    if (request->release_ticks_ns != 0) {
        if (SDL_GetTicksNS() < request->release_ticks_ns) {
            return AFS_READ_STATE_READING;
        }
        request->release_ticks_ns = 0;
    }

    return request->state;
}

unsigned int AFS_GetSectorCount(AFSHandle handle) {
    ReadRequest* request = &requests[handle];
    const unsigned int size = afs.entries[request->file_num].size;
    return (size + 2048 - 1) / 2048;
}
