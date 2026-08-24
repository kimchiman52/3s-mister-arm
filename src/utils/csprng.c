/*
 * csprng.c — see csprng.h for the contract and the SDL_rand rationale.
 */
#ifdef _WIN32
/* rand_s requires _CRT_RAND_S before <stdlib.h>. */
#define _CRT_RAND_S
#endif

#include "utils/csprng.h"

#include <stdlib.h>

#ifdef _WIN32

bool Csprng_Bytes(void* out, size_t len) {
    if (out == NULL) {
        return false;
    }
    unsigned char* p = (unsigned char*)out;
    size_t off = 0;
    while (off < len) {
        unsigned int r = 0;
        if (rand_s(&r) != 0) { /* CRT wrapper over RtlGenRandom */
            return false;
        }
        size_t take = len - off;
        if (take > sizeof(r)) {
            take = sizeof(r);
        }
        for (size_t i = 0; i < take; i++) {
            p[off + i] = (unsigned char)((r >> (8 * i)) & 0xFFu);
        }
        off += take;
    }
    return true;
}

#else /* POSIX: macOS host + MiSTer ARM Linux */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

bool Csprng_Bytes(void* out, size_t len) {
    if (out == NULL) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    /* Open per call: a few dozen bytes per connection attempt, so the
     * open/close cost is irrelevant, and there is no shared fd state to
     * make thread-safe (the stun worker, host worker, and main thread
     * all call in here). O_CLOEXEC so a future exec never inherits it. */
    int fd;
    do {
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return false;
    }
    unsigned char* p = (unsigned char*)out;
    size_t off = 0;
    bool ok = true;
    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        ok = false; /* EOF or hard error — /dev/urandom should do neither */
        break;
    }
    close(fd);
    return ok;
}

#endif /* _WIN32 */
