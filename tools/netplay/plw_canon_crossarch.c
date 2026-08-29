/*
 * plw_canon_crossarch.c — cross-architecture proof + cost measurement for the
 * canonical PLW hash image (task #111).
 *
 * Builds against the PRODUCTION emitter (src/netplay/plw_canon.c), not a
 * re-implementation, and is deliberately free of SDL/engine dependencies so
 * the same two source files can be compiled for the host and for armv7 and
 * the results compared:
 *
 *   host:  cc -Iinclude -Isrc -O2 -o /tmp/plwx tools/netplay/plw_canon_crossarch.c \
 *              src/netplay/plw_canon.c && /tmp/plwx
 *   armv7: arm-linux-gnueabihf-gcc -Iinclude -Isrc -O2 -static \
 *              -o /tmp/plwx-arm tools/netplay/plw_canon_crossarch.c \
 *              src/netplay/plw_canon.c        # then run it on the MiSTer
 *
 * It builds the same LOGICAL PLW on both architectures — filling members by
 * declaration index rather than by byte offset, since the offsets differ —
 * scribbles architecture-specific junk over every pointer member, and prints
 * the canonical image size, its djb2 checksum, and the per-emit cost. Equal
 * checksums from the two runs is the cross-architecture result; unequal means
 * the image is not portable and the netplay checksum cannot cross a peer
 * boundary between a MiSTer and a desktop.
 */

#include "netplay/game_state.h"
#include "netplay/plw_canon_fields.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint16_t off;
    uint16_t len;
} Span;

#define SPAN(f) { (uint16_t)offsetof(PLW, f), (uint16_t)sizeof(((PLW*)0)->f) },
static const Span fields[] = { PLW_CANON_FIELD_LIST(SPAN) };
static const Span pointers[] = { PLW_CANON_POINTER_LIST(SPAN) };
#define FIELD_COUNT ((int)(sizeof(fields) / sizeof(fields[0])))
#define POINTER_COUNT ((int)(sizeof(pointers) / sizeof(pointers[0])))

/* djb2, byte for byte the same function the netplay checksum uses
 * (src/sf33rd/utils/djb2_hash.h) — restated here only to keep this TU free of
 * engine headers. */
static uint32_t djb2(const uint8_t* p, size_t n) {
    uint32_t h = 5381u;
    for (size_t i = 0; i < n; i++) {
        h = ((h << 5) + h) ^ (uint32_t)p[i];
    }
    return h;
}

static uint32_t rng;
static uint8_t next_byte(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (uint8_t)(rng >> 24);
}

/* Fill the PLW so that the LOGICAL content is identical on both
 * architectures: walk members in declaration order and give member i the same
 * bytes everywhere, regardless of where that member sits in this
 * architecture's layout. */
static void build_reference_plw(PLW* p) {
    memset(p, 0, sizeof(*p));
    rng = 0x13572468u;
    for (int i = 0; i < FIELD_COUNT; i++) {
        uint8_t* dst = (uint8_t*)p + fields[i].off;
        for (unsigned b = 0; b < fields[i].len; b++) {
            dst[b] = next_byte();
        }
    }
    /* Pointer members get architecture-flavoured junk: they must not reach
     * the image. On a 32-bit target these are 4-byte slots, on a 64-bit
     * target 8-byte, which is exactly why raw struct bytes cannot be
     * hashed. */
    for (int i = 0; i < POINTER_COUNT; i++) {
        memset((uint8_t*)p + pointers[i].off, (int)(0xC0 + sizeof(void*)), pointers[i].len);
    }
}

/* Same tiling proof --test-gs-coverage runs, repeated here so it is also
 * proven on the architecture that cannot run the test harness: no PLW byte is
 * claimed by two members, and the bytes claimed by none are exactly the
 * padding the static assertions in plw_canon.c account for. */
static int tiling_check(void) {
    static uint8_t owner[sizeof(PLW)];
    int overlaps = 0;
    unsigned holes = 0;

    memset(owner, 0, sizeof(owner));
    for (int i = 0; i < FIELD_COUNT; i++) {
        for (unsigned b = fields[i].off; b < (unsigned)(fields[i].off + fields[i].len); b++) {
            overlaps += owner[b] ? 1 : 0;
            owner[b] = 1;
        }
    }
    for (int i = 0; i < POINTER_COUNT; i++) {
        for (unsigned b = pointers[i].off; b < (unsigned)(pointers[i].off + pointers[i].len); b++) {
            overlaps += owner[b] ? 1 : 0;
            owner[b] = 2;
        }
    }
    for (unsigned b = 0; b < sizeof(PLW); b++) {
        holes += owner[b] ? 0u : 1u;
    }
    printf("TILING: overlaps=%d padding_holes=%u  %d emitted + %d pointer + %u padding = %u vs sizeof(PLW)=%zu\n",
           overlaps, holes, (int)PLW_CANON_SIZE, (int)PLW_CANON_POINTER_BYTES, holes,
           (unsigned)(PLW_CANON_SIZE + PLW_CANON_POINTER_BYTES) + holes, sizeof(PLW));
    return (overlaps == 0 && (size_t)PLW_CANON_SIZE + (size_t)PLW_CANON_POINTER_BYTES + holes == sizeof(PLW)) ? 0 : 1;
}

int main(int argc, char** argv) {
    static PLW p;
    static uint8_t image[PLW_CANON_SIZE];

    if (tiling_check() != 0) {
        printf("TILING=FAIL\n");
        return 1;
    }

    build_reference_plw(&p);
    unsigned written = GameState_EmitPlwCanonical(&p, image);
    uint32_t sum = djb2(image, PLW_CANON_SIZE);

    printf("pointer_width=%zu sizeof(PLW)=%zu canon_size=%d written=%u fields=%d pointers=%d\n", sizeof(void*),
           sizeof(PLW), (int)PLW_CANON_SIZE, written, FIELD_COUNT, POINTER_COUNT);
    printf("CANON_CHECKSUM=0x%08x\n", sum);

    /* Determinism across repeated emits on this machine. */
    for (int rep = 0; rep < 100; rep++) {
        static uint8_t again[PLW_CANON_SIZE];
        memset(again, 0xEE, sizeof(again));
        GameState_EmitPlwCanonical(&p, again);
        if (memcmp(again, image, PLW_CANON_SIZE) != 0) {
            printf("DETERMINISM=FAIL at rep %d\n", rep);
            return 1;
        }
    }
    printf("DETERMINISM=OK (100 repeats)\n");

    /* Per-emit cost. Two PLWs are emitted per saved frame in
     * save_current_state(), so the per-frame figure is 2x the per-emit one. */
    /* Iteration count is overridable so a run on a shared device can be
     * kept short: plw_canon_crossarch [iters]. */
    int iters = 200000;
    if (argc > 1) {
        iters = atoi(argv[1]);
        if (iters < 1000) {
            iters = 1000;
        }
    }
    struct timespec t0, t1;
    volatile uint32_t sink = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++) {
        GameState_EmitPlwCanonical(&p, image);
        sink += image[i % PLW_CANON_SIZE];
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / iters;
    printf("EMIT_NS_PER_CALL=%.1f  PER_FRAME_NS=%.1f (2 PLW/frame)  sink=%u\n", ns, ns * 2.0, sink);

    /* Reference point measured the same way: the memcpy traffic the save path
     * already does per frame is dominated by sizeof(State); this prints the
     * cost of a single PLW-sized memcpy for scale. */
    static uint8_t copy_dst[sizeof(PLW)];
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++) {
        memcpy(copy_dst, &p, sizeof(PLW));
        sink += copy_dst[i % sizeof(PLW)];
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns2 = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / iters;
    printf("MEMCPY_PLW_NS_PER_CALL=%.1f  sink=%u\n", ns2, sink);

    return 0;
}
