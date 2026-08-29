/*
 * plw_canon.c — the canonical, architecture-independent hash image of PLW.
 *
 * Kept in its own translation unit with no engine or SDL dependency for two
 * reasons: the generated member table (plw_canon_fields.h) belongs next to
 * the one function that consumes it, and a dependency-free TU can be
 * compiled straight for armv7 by an offline check, so the SAME emitter that
 * production hashes can be run on both architectures and the two images
 * compared byte for byte (tools/netplay/plw_canon_crossarch.c).
 *
 * Why this exists at all: see the block comment on GameState_EmitPlwCanonical
 * in game_state.h, and the deleted-sweep note in game_state.c.
 */

#include "netplay/game_state.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* === Canonical PLW hash image (task #111) ===============================
 * See the block comment on GameState_EmitPlwCanonical in game_state.h. */

typedef struct {
    uint16_t off;
    uint16_t len;
} PlwCanonSpan;

#define PLW_CANON_SPAN(f) { (uint16_t)offsetof(PLW, f), (uint16_t)sizeof(((PLW*)0)->f) },

/* Every non-pointer member of PLW, in declaration order. Generated — see
 * tools/netplay/gen_plw_canon_fields.py. */
static const PlwCanonSpan plw_canon_spans[] = { PLW_CANON_FIELD_LIST(PLW_CANON_SPAN) };

enum { PLW_CANON_SPAN_COUNT = (int)(sizeof(plw_canon_spans) / sizeof(plw_canon_spans[0])) };

_Static_assert(PLW_CANON_SPAN_COUNT == 399,
               "PLW gained or lost a non-pointer member; regenerate "
               "src/netplay/plw_canon_fields.h with "
               "tools/netplay/gen_plw_canon_fields.py --write and update this count");

/* THE cross-architecture assertion: the canonical image is the same size
 * on every target this compiles for. If a member's width ever became
 * pointer-dependent, this literal would stop matching on one of the two
 * builds. */
_Static_assert(PLW_CANON_SIZE == 885, "canonical PLW image size changed");

/* Whole-struct accounting, per architecture: emitted member bytes +
 * skipped pointer bytes + padding == sizeof(PLW), with no byte counted
 * twice (the no-overlap half is proven at runtime by
 * --test-gs-coverage). Padding is the only term not derived from the
 * member list, so it is spelled out per target rather than computed as a
 * remainder — a wrong padding constant fails the build instead of being
 * absorbed silently. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(PLW) == 1092, "armv7 sizeof(PLW) changed");
_Static_assert(PLW_CANON_POINTER_BYTES == 196, "armv7 PLW pointer bytes (49 slots x 4)");
_Static_assert(PLW_CANON_SIZE + PLW_CANON_POINTER_BYTES + 11 == sizeof(PLW),
               "armv7 PLW byte accounting: 885 emitted + 196 pointer + 11 padding");
#elif defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(PLW) == 1304, "64-bit sizeof(PLW) changed");
_Static_assert(PLW_CANON_POINTER_BYTES == 392, "64-bit PLW pointer bytes (49 slots x 8)");
_Static_assert(PLW_CANON_SIZE + PLW_CANON_POINTER_BYTES + 27 == sizeof(PLW),
               "64-bit PLW byte accounting: 885 emitted + 392 pointer + 27 padding");
#else
#error "unexpected pointer width — add the PLW byte-accounting assertions for this target"
#endif

/* The canonical image is only byte-comparable between peers if both read
 * the members the same way round. Every emitted member is a fixed-width
 * integer, array, or struct of those, so little-endian is the only
 * remaining assumption. */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "canonical PLW image assumes little-endian members");

/* Adjacent members coalesced into contiguous byte runs: 399 members become
 * 15 runs, because the only things that break a run are a pointer slot and a
 * padding hole. Built once from plw_canon_spans — the member table stays the
 * single source of truth and the run table is derived from it, never
 * hand-written, so the two cannot disagree.
 *
 * Deliberately derived at runtime rather than generated per architecture:
 * the offsets differ between armv7 and 64-bit, and a second generated table
 * would be a second thing to keep in sync for no gain. The content is a pure
 * function of compile-time constants, so it is identical in every process and
 * every frame — including between a rollback-determinism baseline run and its
 * rollback run, which is why it cannot show up as a capture divergence. */
static PlwCanonSpan plw_canon_runs[PLW_CANON_SPAN_COUNT];
static int plw_canon_run_count = 0;

static void plw_canon_build_runs(void) {
    int runs = 0;

    for (int i = 0; i < PLW_CANON_SPAN_COUNT;) {
        unsigned off = plw_canon_spans[i].off;
        unsigned len = plw_canon_spans[i].len;
        i++;
        while (i < PLW_CANON_SPAN_COUNT && plw_canon_spans[i].off == off + len) {
            len += plw_canon_spans[i].len;
            i++;
        }
        plw_canon_runs[runs].off = (uint16_t)off;
        plw_canon_runs[runs].len = (uint16_t)len;
        runs++;
    }

    plw_canon_run_count = runs;
}

unsigned GameState_EmitPlwCanonical(const PLW* copy, uint8_t* out) {
    const uint8_t* base = (const uint8_t*)copy;
    unsigned written = 0;

    if (plw_canon_run_count == 0) {
        plw_canon_build_runs();
    }

    for (int i = 0; i < plw_canon_run_count; i++) {
        memcpy(out + written, base + plw_canon_runs[i].off, plw_canon_runs[i].len);
        written += plw_canon_runs[i].len;
    }

    return written;
}

/* Number of coalesced runs, for the coverage harness to report and pin. */
int GameState_PlwCanonicalRunCount(void) {
    if (plw_canon_run_count == 0) {
        plw_canon_build_runs();
    }
    return plw_canon_run_count;
}
