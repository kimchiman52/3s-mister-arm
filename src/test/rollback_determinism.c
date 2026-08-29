#if DEBUG && defined(ENABLE_NETPLAY)

/* Rollback-determinism harness — in-game capture + rollback injection.
 * See rollback_determinism.h for the overview and
 * docs/rollback-determinism-harness.md for the full design, usage and
 * known limits. Driven by
 * tools/rollback-determinism/check_rollback_determinism.py.
 *
 * Naming rule: every object with static storage duration in this TU is
 * prefixed rbd_. The capture skips map symbols with that prefix so the
 * harness never diffs its own bookkeeping (the driver applies the same
 * filter when it builds the map).
 */

#include "test/rollback_determinism.h"

#include "configuration.h"
#include "main.h"
#include "test/test_runner.h"
#include "netplay/game_state.h"
#include "port/sdl/sdl_app.h"
#include "sf33rd/Source/Game/effect/effect.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/rendering/dc_ghost.h"
#include "sf33rd/Source/Game/rendering/mtrans.h"
#include "sf33rd/Source/Game/system/work_sys.h"

#include "gekkonet.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Process address-space introspection, for the pointer-canonicalizing hash
 * below (task #65).
 *
 * Mach-O and ELF are the only backends, which costs nothing in practice: the
 * driver can only build a symbol map for those two formats
 * (build_symbol_map_macho / build_symbol_map_elf) and only knows how to
 * launch a child with ASLR disabled via posix_spawn or setarch, so a capture
 * cannot run anywhere else regardless. Elsewhere the capture aborts at
 * startup with the usual plumbing-failure exit rather than degrading to a
 * classification that would reintroduce the #65 flake. Deliberately a
 * RUNTIME failure and not an #error: this TU still compiles into any Debug
 * build, and breaking a Windows Debug build over a harness that platform
 * cannot drive would be a worse trade than a loud abort nobody will reach. */
#if defined(__APPLE__)
#define RBD_HAVE_VM_BACKEND 1
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif defined(__linux__)
#define RBD_HAVE_VM_BACKEND 1
#include <elf.h>
#include <link.h> /* ElfW() — width-correct for both x86-64 and 32-bit armhf */
#include <unistd.h>
#define RBD_ELFCLASS_NATIVE (__ELF_NATIVE_CLASS == 64 ? ELFCLASS64 : ELFCLASS32)
#else
#define RBD_HAVE_VM_BACKEND 0
#endif

/* Exit code for capture-setup/IO failures. Distinct from input_script.c's
 * 3 (failed-to-start) so the driver can tell the two apart; both mean
 * "harness plumbing", never "divergence found" (divergence is judged by
 * the driver, not in-game). */
#define RBD_EXIT_CODE_FAILED 4

/* "RBD2" — bumped from "RBD1" when the footer gained the
 * pointer-canonicalization count (task #65). The driver pins the exact
 * value, so a stream written by a stale binary fails with a clear magic
 * mismatch instead of a confusing short-footer error. */
#define RBD_STREAM_MAGIC 0x32444252u /* "RBD2" little-endian */
#define RBD_FOOTER_MAGIC 0x46444252u /* "RBDF" little-endian */

typedef struct RbdSym {
    uintptr_t addr; /* runtime address (map address + slide) */
    uint32_t size;
} RbdSym;

static bool rbd_init_attempted = false;
static bool rbd_active = false;

static RbdSym* rbd_syms = NULL;
static uint32_t rbd_sym_count = 0;
static uint32_t* rbd_row = NULL;
static FILE* rbd_file = NULL;
static char* rbd_file_buf = NULL;

static uint32_t rbd_frame_index = 0;
static uint32_t rbd_cycle_count = 0;
static uint32_t rbd_ingame_first_frame = 0xFFFFFFFFu;

/* Gekko-shaped save buffer for the production save_state() encoder. Sized
 * for the larger of the sparse ceiling and the full-state fallback, the
 * same worst case a real GekkoNet ring slot must hold. */
static unsigned char* rbd_gekko_buf = NULL;

/* Scratch for the canonicalized effect-slot hashing (see
 * rbd_hash_frw_canonical). Heap so it never lands in the hashed image. */
static unsigned char* rbd_slot_scratch = NULL;

/* Index into rbd_syms of the entry covering frw[], or UINT32_MAX. frw is
 * hashed through a canonicalizing view rather than raw bytes — see
 * rbd_hash_frw_canonical for why. */
static uint32_t rbd_frw_sym_index = 0xFFFFFFFFu;

/* Ditto for plw[2]: hashed through the production checksum's sanitized
 * view (GameState_SanitizePlwCopyForHash). Raw plw bytes contain heap
 * pointers into AFS-loaded character data whose addresses vary per
 * process even with ASLR disabled, which would push plw into the
 * baseline-noise set and blind the harness to the single most important
 * feedback signal (player state drifting after rollbacks). */
static uint32_t rbd_plw_sym_index = 0xFFFFFFFFu;
static unsigned char* rbd_plw_scratch = NULL;

/* === Pointer canonicalization for whole-pointer statics (task #65) ========
 *
 * THE DEFECT THIS FIXES. The driver classifies a symbol as NOISE when its
 * two baselines (A1 vs A2) disagree. That is a two-sample estimate of
 * "is this symbol nondeterministic across processes", and for symbols whose
 * whole content is a heap address it is a COIN FLIP, not a determination:
 * when the two baselines happen to allocate at the same address the symbol
 * is not excluded, and the rollback run's different address is then reported
 * as a genuine divergence. That is the intermittent false FAIL filed as #65
 * (pref_path, afs_path, debug_renderer, message_canvas).
 *
 * WHY THE ADDRESSES MOVE AT ALL. The driver spawns every run with
 * _POSIX_SPAWN_DISABLE_ASLR, which zeroes the *image* slide only. Measured
 * on this host (40 spawns of a probe binary through the driver's own
 * spawn_no_aslr_darwin): the main executable's `&main` was identical in
 * 40/40 runs, while a 16-byte malloc took 30 distinct values (1.5% pairwise
 * collision) and a 1 MB malloc took 40 distinct values (0% collision). The
 * kernel randomizes where the allocator's regions are mapped, and
 * DISABLE_ASLR does not touch that. ~1.5% is exactly the "rare but real"
 * flake rate #65 describes.
 *
 * THE FIX, AND WHY IT IS NOT AN EXCLUSION. Rather than trying to *classify*
 * these symbols out of the verdict (which is what NOISE was failing to do
 * reliably), the capture removes the process-varying quantity from the
 * hashed image entirely, in every run, exactly as it already does for frw[]
 * and plw[]. A symbol that is exactly one aligned pointer wide and whose
 * value is a live address OUTSIDE the main executable's own segments hashes
 * a fixed token instead of the address. Both baselines and the rollback run
 * then agree by construction, so the classification can no longer depend on
 * whether A1 and A2 coincided — there is nothing left for them to coincide
 * about.
 *
 * WHAT IS DELIBERATELY STILL COMPARED. NULL is not canonicalized, so
 * "pointer became NULL" / "NULL became a pointer" still diverges. Only
 * pointer IDENTITY is dropped, and pointer identity was never comparable
 * across processes in the first place.
 *
 * SCOPE, AND WHY IT IS THIS NARROW. Two independent bounds.
 *
 *   1. Only symbols of exactly sizeof(void*) at an aligned address are
 *      eligible. A wider rule (canonicalize every aligned word of every
 *      symbol) would let an ordinary pair of adjacent int32s that happens to
 *      land inside a mapped region silence real gameplay state — the exact
 *      "silencing to get green" this harness exists to refuse. The narrow
 *      rule cannot do that to a multi-field object: it only ever replaces a
 *      whole symbol that is a single pointer slot.
 *
 *   2. The decision is FROZEN ONCE, from the state of the world at the first
 *      captured frame, and never revisited. This is what makes the rule
 *      "single-assignment-at-startup pointers" rather than "anything that
 *      currently looks like a pointer", and it is not a refinement — a
 *      value-based test evaluated every frame is actively WRONG here, for a
 *      reason worth recording:
 *
 *        sdl_app.c's Uint64 wall-clock statics (frame_deadline,
 *        last_frame_end_time, perf_frame_start_ns, perf_update_start_ns)
 *        hold nanosecond counts. Measured on this tree, those counts grow
 *        past the end of the image around frame 96 and from then on land
 *        INSIDE the malloc region that sits immediately above it — a
 *        nanosecond timestamp and a heap address are numerically
 *        indistinguishable in this process. A per-frame test canonicalized
 *        all four from frame ~96 onward, which would have invented a NEW
 *        flake: whenever both baselines tokenized a timer and the rollback
 *        run did not, an always-noise wall-clock symbol would have been
 *        promoted to DIVERGENT.
 *
 *      Freezing at the first frame excludes them for free — all four are
 *      still 0 there — while every symbol the rule is actually for (SDL
 *      objects, pref/afs paths, the render targets) is already assigned.
 *      Anything that only becomes a pointer later stays fully byte-compared,
 *      which is the safe direction: state that changes during simulation is
 *      never silenced.
 *
 * AUDITABILITY. Nothing is silenced invisibly: every symbol the rule fires
 * on is named on stderr the first time it fires, and the total is written
 * to the capture footer so the driver can compare the three runs and warn
 * when they disagree. */
#define RBD_PTR_TOKEN 0xA55E5510A55E5510ULL

typedef struct RbdRegion {
    uintptr_t lo;
    uintptr_t hi;
} RbdRegion;

static RbdRegion* rbd_regions = NULL;
static uint32_t rbd_region_count = 0;
static uint32_t rbd_region_cap = 0;
static uintptr_t rbd_image_lo = 0;
static uintptr_t rbd_image_hi = 0;
static bool rbd_regions_refreshed_this_frame = false;

/* Per-symbol "the pointer rule fired here at least once", for the audit log
 * and the footer count. Parallel to rbd_syms. */
static uint8_t* rbd_ptr_canon = NULL;
static uint32_t rbd_ptr_canon_count = 0;
static bool rbd_ptr_slots_frozen = false;
/* Names, retained only for pointer-width symbols (the only ones the rule can
 * fire on) so the audit log can name them. NULL for every other entry. */
static char** rbd_sym_names = NULL;

static void rbd_fail(const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "[rbd] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(RBD_EXIT_CODE_FAILED);
}

/* 64-bit multiply-xor chain over 8-byte little-endian chunks (tail
 * zero-padded). Not cryptographic — only needs to make "any byte
 * changed" overwhelmingly likely to change the output. Chunked so the
 * whole ~17 MB writable image hashes in a few ms per frame (this TU is
 * compiled -O2 even in Debug builds; see CMakeLists.txt). */
#define RBD_HASH_SEED 0x9E3779B97F4A7C15ULL

static uint64_t rbd_hash64(uint64_t h, const void* mem, size_t len) {
    const unsigned char* p = (const unsigned char*)mem;
    const uint64_t prime = 0x2545F4914F6CDD1DULL;

    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h = (h ^ v) * prime;
        p += 8;
        len -= 8;
    }
    if (len > 0) {
        uint64_t v = 0;
        memcpy(&v, p, len);
        h = (h ^ v) * prime;
    }
    return h;
}

static uint32_t rbd_hash_fold(uint64_t h) {
    return (uint32_t)(h ^ (h >> 32));
}

/* Canonicalized hash of the effect work pool. Raw bytes of frw[] are NOT
 * comparable between a baseline run and a rollback run by design: the
 * production save path (save_current_state's sanitize pass, mirrored by
 * the sparse encoder's canonical-empty reconstruction) zeroes inactive
 * slots (preserving before/behind/myself) and zeroes the wrd_free /
 * et_free padding tails of active slots. A rollback run's live pool
 * therefore holds canonicalized bytes where a straight-line run holds
 * stale garbage from dead effects — a guaranteed, by-design difference
 * that says nothing about the save set. Hash the same canonical view the
 * save/restore contract guarantees instead, in both runs:
 *   - inactive slot (be_flag == 0): zeros + preserved before/behind/myself
 *   - active slot: verbatim bytes with wrd_free and et_free zeroed
 * Any divergence that survives THIS view is real effect-pool state the
 * rollback failed to reproduce. */
static uint32_t rbd_hash_frw_canonical(void) {
    uint64_t h = RBD_HASH_SEED;
    const size_t slot_bytes = sizeof(frw[0]);

    for (int i = 0; i < EFFECT_MAX; i++) {
        WORK* live = (WORK*)frw[i];
        WORK* canon = (WORK*)rbd_slot_scratch;

        if (live->be_flag == 0) {
            memset(rbd_slot_scratch, 0, slot_bytes);
            canon->before = live->before;
            canon->behind = live->behind;
            canon->myself = live->myself;
        } else {
            memcpy(rbd_slot_scratch, frw[i], slot_bytes);
            SDL_zeroa(canon->wrd_free);
            WORK_Other* canon_other = (WORK_Other*)rbd_slot_scratch;
            SDL_zeroa(canon_other->et_free);
            /* Null the WORK pointer fields and mask rendering-only bits
             * through the production sanitizer: active slots cache heap
             * pointers (char tables, hit tables) whose addresses vary per
             * process, which would otherwise make frw permanent baseline
             * noise. Same view the cross-peer checksum uses. */
            GameState_SanitizeWorkCopyForHash(canon);
        }

        /* Chain across slots: keep the running 64-bit state as seed. */
        h = rbd_hash64(h, rbd_slot_scratch, slot_bytes);
    }

    return rbd_hash_fold(h);
}

/* Hash plw[2] through the production checksum's sanitized view (see
 * rbd_plw_sym_index above). Trailing map-entry padding beyond sizeof(plw)
 * carries no information and is skipped. */
static uint32_t rbd_hash_plw_sanitized(void) {
    uint64_t h = RBD_HASH_SEED;

    for (int p = 0; p < 2; p++) {
        memcpy(rbd_plw_scratch, &plw[p], sizeof(PLW));
        GameState_SanitizePlwCopyForHash((PLW*)rbd_plw_scratch);
        h = rbd_hash64(h, rbd_plw_scratch, sizeof(PLW));
    }

    return rbd_hash_fold(h);
}

/* Bounds of the MAIN EXECUTABLE's mapped segments. Addresses in this range
 * are NOT process-varying (the driver disables image ASLR, and the capture
 * already proves the slide is constant by cross-checking three anchor
 * symbols), so pointers into the image — function pointers, &static — stay
 * fully comparable and are never canonicalized. */
static void rbd_compute_image_bounds(void) {
#if !RBD_HAVE_VM_BACKEND
    rbd_fail("no VM-map backend on this platform, so whole-pointer statics cannot be identified; "
             "refusing to capture rather than reintroducing the task-#65 flake (the driver cannot "
             "build a symbol map for this binary format either)");
#elif defined(__APPLE__)
    const struct mach_header_64* mh = (const struct mach_header_64*)_dyld_get_image_header(0);
    if (mh == NULL) {
        rbd_fail("could not locate the main executable's Mach-O header (_dyld_get_image_header(0))");
    }
    const intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    uintptr_t lo = UINTPTR_MAX;
    uintptr_t hi = 0;
    const struct load_command* lc = (const struct load_command*)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* sc = (const struct segment_command_64*)lc;
            /* Skip __PAGEZERO (initprot 0, and 4 GB wide on arm64). Folding
             * it into the image range would exempt every value below
             * 0x100000000 from the pointer rule. Nothing is mapped there so
             * it changes no verdict today, but it makes the range mean
             * something other than "the image", which is the kind of quiet
             * imprecision that later gets reasoned from. */
            if (sc->vmsize != 0 && sc->initprot != 0) {
                const uintptr_t s = (uintptr_t)((intptr_t)sc->vmaddr + slide);
                if (s < lo) {
                    lo = s;
                }
                if (s + (uintptr_t)sc->vmsize > hi) {
                    hi = s + (uintptr_t)sc->vmsize;
                }
            }
        }
        lc = (const struct load_command*)((const char*)lc + lc->cmdsize);
    }
    if (lo >= hi) {
        rbd_fail("main-executable segment walk produced an empty range");
    }
    rbd_image_lo = lo;
    rbd_image_hi = hi;
#elif defined(__linux__)
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        rbd_fail("readlink(\"/proc/self/exe\") failed");
    }
    exe[n] = '\0';

    FILE* f = fopen("/proc/self/maps", "r");
    if (f == NULL) {
        rbd_fail("could not open /proc/self/maps");
    }
    uintptr_t lo = UINTPTR_MAX;
    uintptr_t hi = 0;
    char line[8192];
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long a = 0, b = 0;
        int consumed = -1;
        if (sscanf(line, "%llx-%llx %*s %*s %*s %*s %n", &a, &b, &consumed) != 2 || consumed < 0) {
            continue;
        }
        const char* path = line + consumed;
        size_t len = strcspn(path, "\r\n");
        if (len == 0 || strncmp(path, exe, len) != 0 || exe[len] != '\0') {
            continue;
        }
        if ((uintptr_t)a < lo) {
            lo = (uintptr_t)a;
        }
        if ((uintptr_t)b > hi) {
            hi = (uintptr_t)b;
        }
    }
    fclose(f);
    if (lo >= hi) {
        rbd_fail("no /proc/self/maps entry matched the main executable \"%s\"", exe);
    }

    /* The maps walk alone is NOT the image, and stopping here was measurably
     * wrong. /proc/self/maps carries the executable's pathname only on the
     * FILE-BACKED part of each PT_LOAD; the .bss tail past the last file page
     * is an ANONYMOUS mapping with an empty pathname, so it is (a) excluded
     * from this range and (b) admitted by rbd_collect_regions below, which
     * treats every anonymous mapping as allocator-owned because ELF exposes
     * no allocator tag. Net effect: the address of any executable-owned
     * object in that tail passes rbd_addr_is_process_varying, and a
     * pointer-width static holding one is silently canonicalized — real game
     * state replaced by a constant token, in a harness whose entire purpose
     * is to refuse that.
     *
     * Measured with a probe binary compiled from this very source text on
     * Debian 11 x86-64 under the driver's own `setarch -R` launch: a 24 MB
     * `static unsigned char[]` reported image=[0x555555554000,0x555555559000)
     * — 20 KB — with &array[mid] and &array[end-8] both landing in a
     * 25165824-byte "allocator" region and classifying CANON, while the same
     * probe on macOS classified all of them raw/in-image. This game's
     * writable image is ~17 MB and overwhelmingly .bss, so the exposure is
     * the whole state the harness watches, not an edge.
     *
     * The macOS branch above has never had this hole because it sums
     * `sc->vmsize`, which counts bss. Do the ELF-equivalent: take p_memsz
     * from the program headers. They are reachable without dl_iterate_phdr
     * (which needs _GNU_SOURCE — unavailable here, the tree builds -std=gnu11
     * and this include block sits below <stdio.h>): the first PT_LOAD maps
     * file offset 0, so the ELF header is mapped at the image's lowest
     * address, and e_phoff indexes the headers from there. ElfW() picks the
     * right width, which matters — the MiSTer target is 32-bit armhf. */
    const ElfW(Ehdr)* ehdr = (const ElfW(Ehdr)*)lo;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_ident[EI_CLASS] != RBD_ELFCLASS_NATIVE ||
        ehdr->e_phentsize != (uint16_t)sizeof(ElfW(Phdr)) || ehdr->e_phnum == 0) {
        rbd_fail("mapping at %#llx for \"%s\" is not a native-class ELF header — cannot bound the image",
                 (unsigned long long)lo, exe);
    }
    /* Keep the header walk inside what the maps walk already proved is
     * mapped. A malformed e_phoff must abort loudly like every other
     * plumbing failure here, not segfault — a crash reads as a crash-class
     * finding this harness does not make. */
    if ((uintptr_t)ehdr->e_phoff > hi - lo ||
        (uintptr_t)ehdr->e_phnum * (uintptr_t)ehdr->e_phentsize > (hi - lo) - (uintptr_t)ehdr->e_phoff) {
        rbd_fail("program headers of \"%s\" (e_phoff %llu, %u x %u) fall outside its mapped file range "
                 "[%#llx,%#llx)",
                 exe, (unsigned long long)ehdr->e_phoff, (unsigned)ehdr->e_phnum, (unsigned)ehdr->e_phentsize,
                 (unsigned long long)lo, (unsigned long long)hi);
    }
    const ElfW(Phdr)* phdr = (const ElfW(Phdr)*)(lo + (uintptr_t)ehdr->e_phoff);

    uintptr_t min_vaddr = UINTPTR_MAX;
    uintptr_t max_end = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) {
            continue;
        }
        const uintptr_t v = (uintptr_t)phdr[i].p_vaddr;
        if (v < min_vaddr) {
            min_vaddr = v;
        }
        if (v + (uintptr_t)phdr[i].p_memsz > max_end) {
            max_end = v + (uintptr_t)phdr[i].p_memsz;
        }
    }
    if (min_vaddr == UINTPTR_MAX || max_end <= min_vaddr) {
        rbd_fail("program-header walk for \"%s\" found no loadable segment", exe);
    }

    /* Bias handles both PIE (min p_vaddr 0, bias = load address) and a
     * fixed-address binary (bias 0). */
    const uintptr_t bias = lo - min_vaddr;

    /* Round the end up to a page: the kernel maps whole pages, so the bytes
     * between p_memsz's end and the page boundary belong to the image's own
     * anonymous tail mapping and would otherwise be claimed by the region
     * set. Rounding can only make the rule fire LESS, which is the safe
     * direction. */
    long pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize <= 0) {
        pagesize = 4096;
    }
    const uintptr_t page_mask = (uintptr_t)pagesize - 1u;

    /* The program headers are the whole answer; `hi` from the maps walk is
     * deliberately NOT folded back in. Any mapping past the last PT_LOAD that
     * still carries the executable's pathname is a SECOND mapping of the
     * file, not part of the loaded image, and widening to reach it is how
     * this range stops meaning anything. Measured, when a probe mmap'd its
     * own argv[0]: the maps-derived end jumped to that mapping and produced
     * image=[0x64f5960b8000,0x765850394000) — 19 TB, inside which every value
     * tests as "in the image" and NOTHING is ever canonicalized, i.e. the #65
     * flake back in full. */
    rbd_image_lo = bias + min_vaddr;
    rbd_image_hi = ((bias + max_end) + page_mask) & ~page_mask;
#endif
}

/* Second of the two filters that keep an ordinary integer from being
 * mistaken for a pointer (the first is the 8-byte value alignment in
 * rbd_addr_is_process_varying). Only mappings the ALLOCATOR owns count as
 * "where a pointer can point"; mapped files, the dyld shared cache, SDL's
 * own mmaps and guard pages are all excluded.
 *
 * This is not theoretical tidiness. Measured on this tree with the region
 * set unfiltered, `msgTalkCtrPL01` — `static s8 msgTalkCtrPL01[5] =
 * {1,2,2,2,2}` (message/en/pl01tlk_en.c:31), whose nm entry is padded to 8
 * bytes — hashed as the value 0x202020201, landed inside a non-allocator
 * mapping, and was canonicalized. That is real game data being silenced by
 * accident, i.e. exactly the failure this harness exists to refuse. Either
 * filter alone rejects it (the value is odd, and it is not in an allocator
 * region); both are applied because they fail in different directions. */
#if defined(__APPLE__)
#ifndef VM_MEMORY_MALLOC_NANO
#define VM_MEMORY_MALLOC_NANO 11
#endif
static bool rbd_tag_is_allocator(unsigned int tag) {
    /* <mach/vm_statistics.h> assigns the malloc family a contiguous block of
     * user tags: VM_MEMORY_MALLOC (1) through VM_MEMORY_MALLOC_NANO (11),
     * covering the TINY/SMALL/LARGE/HUGE zones and realloc. */
    return tag >= VM_MEMORY_MALLOC && tag <= VM_MEMORY_MALLOC_NANO;
}
#endif

/* Fill `out` (capacity `cap`) with this process's allocator-owned readable
 * mappings, in ascending address order, and return how many exist. Passing
 * out=NULL counts without storing.
 *
 * "Allocator-owned" is EXACT on Mach and a documented SUPERSET on ELF, and
 * the difference is stated here rather than left for someone to rediscover.
 * /proc/self/maps carries no allocator tag, so the ELF branch's only
 * available proxy is "anonymous" — which also catches thread stacks, raw
 * mmap()s, and the .bss tails of shared objects. Narrowing it to [heap] is
 * NOT the fix: measured on Debian 11 x86-64 under `setarch -R`, a 1 MB
 * malloc landed at 0x7ffff7cce010 inside the anonymous mapping
 * 7ffff7cce000-7ffff7dd2000 (`rw-p 00000000 00:00 0`, no pathname), so
 * dropping anonymous mappings would stop canonicalizing the large-malloc
 * class and hand back the #65 flake this rule exists to close.
 *
 * The superset only ever canonicalizes MORE addresses, never fewer, and
 * every address it adds is one the kernel picks per process — the same
 * quantity the rule is built to drop. What keeps an ordinary integer out is
 * the pair of bounds in rbd_addr_is_process_varying: 8-byte value alignment,
 * and the frozen-at-frame-0 decision. The second one is load-bearing on
 * Linux specifically, not just in principle: with ASLR disabled the mmap
 * area sits at ~0x7ffff7xxxxxx, i.e. ~1.4e14, and CLOCK_MONOTONIC in
 * nanoseconds reaches that at roughly 1.6 days of uptime — a ns timer and a
 * mapped address are numerically indistinguishable there, exactly as
 * measured for sdl_app.c's timers on macOS. Frame 0 is before those counters
 * leave zero, which is what makes the rule safe here.
 *
 * What IS excluded on both, and verified on ELF from the same run: PROT_NONE
 * guard pages (7ffff74cd000-7ffff74ce000 `---p` was absent from the region
 * set), every named mapping (the executable, libc, ld.so, [stack], [vvar],
 * [vdso], [vsyscall]) — and, since rbd_compute_image_bounds now bounds the
 * image by p_memsz, the executable's own .bss tail is checked against the
 * image range before this set is ever consulted. */
static uint32_t rbd_collect_regions(RbdRegion* out, uint32_t cap) {
    uint32_t n = 0;
#if defined(__APPLE__)
    mach_vm_address_t address = 0;
    natural_t depth = 0;
    /* Hard bound on the walk. The submap branch below deliberately does not
     * advance `address`, so a malformed/pathological map could otherwise spin
     * here; this harness must never hang (a hang costs a whole scenario via
     * the driver's timeout and reads as a crash-class finding it is not). The
     * real map on this tree is ~130 regions before the allocator filter. */
    for (uint32_t guard = 0; guard < 1u << 20; guard++) {
        mach_vm_size_t size = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        const kern_return_t kr =
            mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, (vm_region_recurse_info_t)&info, &count);
        if (kr != KERN_SUCCESS) {
            break;
        }
        if (info.is_submap) {
            /* Descend rather than swallowing the whole submap as one range:
             * a shared-cache submap spans hundreds of MB of mostly-unmapped
             * address space, and treating that as "mapped" would widen the
             * canonicalization rule far past what is actually addressable. */
            depth += 1;
            continue;
        }
        if (size == 0) {
            break;
        }
        if ((info.protection & VM_PROT_READ) != 0 && rbd_tag_is_allocator(info.user_tag)) {
            if (out != NULL && n < cap) {
                out[n].lo = (uintptr_t)address;
                out[n].hi = (uintptr_t)(address + size);
            }
            n += 1;
        }
        address += size;
    }
#elif defined(__linux__)
    FILE* f = fopen("/proc/self/maps", "r");
    if (f == NULL) {
        rbd_fail("could not open /proc/self/maps");
    }
    char line[8192];
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long a = 0, b = 0;
        char perms[8] = { 0 };
        int consumed = -1;
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", &a, &b, perms, &consumed) != 3) {
            continue;
        }
        if (perms[0] != 'r') {
            continue;
        }
        /* Allocator-owned only, mirroring the Mach user_tag filter above:
         * anonymous mappings (empty pathname) and the brk heap. Named
         * mappings — the executable, dylibs, mapped assets — are excluded. */
        if (consumed >= 0) {
            const char* path = line + consumed;
            const size_t plen = strcspn(path, "\r\n");
            if (plen != 0 && strncmp(path, "[heap]", plen) != 0) {
                continue;
            }
        }
        if (out != NULL && n < cap) {
            out[n].lo = (uintptr_t)a;
            out[n].hi = (uintptr_t)b;
        }
        n += 1;
    }
    fclose(f);
#endif
    return n;
}

static void rbd_refresh_regions(void) {
    const uint32_t needed = rbd_collect_regions(NULL, 0);
    if (needed == 0) {
        rbd_fail("VM-map enumeration returned no readable regions — the pointer-canonicalization "
                 "rule cannot be evaluated, and running without it would reintroduce the #65 flake");
    }
    /* Slack: the map can grow between the counting pass and the filling
     * pass. Anything past the cap is simply not recorded, which can only
     * make the rule fire less often (fail-closed toward "compare the raw
     * bytes"), never more. */
    const uint32_t want = needed + 256u;
    if (want > rbd_region_cap) {
        RbdRegion* grown = (RbdRegion*)SDL_realloc(rbd_regions, (size_t)want * sizeof(RbdRegion));
        if (grown == NULL) {
            rbd_fail("out of memory allocating %u VM-region entries", want);
        }
        rbd_regions = grown;
        rbd_region_cap = want;
    }
    const uint32_t filled = rbd_collect_regions(rbd_regions, rbd_region_cap);
    rbd_region_count = filled < rbd_region_cap ? filled : rbd_region_cap;
}

static bool rbd_region_contains(uintptr_t v) {
    uint32_t lo = 0;
    uint32_t hi = rbd_region_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (v < rbd_regions[mid].lo) {
            hi = mid;
        } else if (v >= rbd_regions[mid].hi) {
            lo = mid + 1u;
        } else {
            return true;
        }
    }
    return false;
}

/* True when `v` is a live address whose numeric value is chosen by the
 * kernel per process — i.e. mapped, and outside the main executable. NULL is
 * excluded on purpose so NULL-vs-non-NULL stays a detectable divergence. */
static bool rbd_addr_is_process_varying(uintptr_t v) {
    if (v == 0) {
        return false;
    }
    /* First of the two anti-false-positive filters (see rbd_tag_is_allocator
     * for the second and for the measured case that motivated both). Every
     * address an allocator hands back is at least 16-byte aligned on the
     * platforms this runs on; requiring 8 is the conservative form. It
     * rejects 7 of every 8 arbitrary bit patterns, and it rejected both of
     * the false positives actually observed on this tree
     * (msgTalkCtrPL01 = 0x202020201 and msgTalkCtrPL02 = 0x202010101, both
     * odd) while keeping all 21 genuine pointers, whose logged values were
     * every one of them 8-byte aligned.
     *
     * It fails CLOSED: an unaligned char* cursor into the middle of a buffer
     * is simply not canonicalized, which leaves that symbol behaving exactly
     * as it did before this rule existed. */
    if ((v % sizeof(void*)) != 0) {
        return false;
    }
    if (v >= rbd_image_lo && v < rbd_image_hi) {
        return false;
    }
    if (rbd_region_contains(v)) {
        return true;
    }
    /* A miss can mean "not an address" or "the map moved since the last
     * refresh" (the game allocates continuously). Re-enumerate at most once
     * per frame before concluding it is not an address, so a pointer into a
     * freshly-mapped region cannot slip back into the raw-bytes path and
     * resurrect the flake. */
    if (!rbd_regions_refreshed_this_frame) {
        rbd_regions_refreshed_this_frame = true;
        rbd_refresh_regions();
        return rbd_region_contains(v);
    }
    return false;
}

static bool rbd_symmap_line_parse(const char* line, uintptr_t* addr, uint32_t* size, char* name, size_t name_cap) {
    unsigned long long a = 0, s = 0;
    int consumed = -1;

    if (sscanf(line, "%llx %llx %n", &a, &s, &consumed) != 2 || consumed < 0) {
        return false;
    }
    const char* n = line + consumed;
    size_t len = strcspn(n, " \t\r\n");
    if (len == 0 || len >= name_cap) {
        return false;
    }
    memcpy(name, n, len);
    name[len] = '\0';
    *addr = (uintptr_t)a;
    *size = (uint32_t)s;
    return true;
}

static void rbd_load_symmap(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        rbd_fail("could not open symmap \"%s\"", path);
    }

    /* Pass 1: count lines to size the arrays. */
    uint32_t line_count = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        line_count += 1;
    }
    if (line_count == 0) {
        rbd_fail("symmap \"%s\" is empty", path);
    }
    rewind(f);

    rbd_syms = (RbdSym*)SDL_malloc((size_t)line_count * sizeof(RbdSym));
    if (rbd_syms == NULL) {
        rbd_fail("out of memory allocating %u symbol entries", line_count);
    }
    rbd_sym_names = (char**)SDL_calloc((size_t)line_count, sizeof(char*));
    rbd_ptr_canon = (uint8_t*)SDL_calloc((size_t)line_count, sizeof(uint8_t));
    if (rbd_sym_names == NULL || rbd_ptr_canon == NULL) {
        rbd_fail("out of memory allocating %u pointer-rule bookkeeping entries", line_count);
    }

    /* Slide derivation + verification anchors. The map stores link-time
     * addresses; ASLR slides the whole image by one constant, recovered
     * from any known symbol. Three independent anchors (data, common and
     * bss candidates) must agree or the map doesn't belong to this
     * binary. */
    struct {
        const char* name;
        uintptr_t runtime;
        uintptr_t map_addr;
        bool found;
    } anchors[] = {
        { "Random_ix16", (uintptr_t)&Random_ix16, 0, false },
        { "My_char", (uintptr_t)&My_char[0], 0, false },
        { "frw", (uintptr_t)&frw[0][0], 0, false },
    };
    const int anchor_count = (int)(sizeof(anchors) / sizeof(anchors[0]));

    uint32_t count = 0;
    int line_no = 0;
    char name[256];
    uintptr_t map_frw_addr = 0;
    bool have_map_frw = false;

    while (fgets(line, sizeof(line), f) != NULL) {
        line_no += 1;
        uintptr_t addr;
        uint32_t size;
        if (!rbd_symmap_line_parse(line, &addr, &size, name, sizeof(name))) {
            rbd_fail("symmap %s:%d: malformed line", path, line_no);
        }

        for (int a = 0; a < anchor_count; a++) {
            if (!anchors[a].found && strcmp(name, anchors[a].name) == 0) {
                anchors[a].map_addr = addr;
                anchors[a].found = true;
            }
        }

        /* Never hash this harness's own bookkeeping. */
        if (strncmp(name, "rbd_", 4) == 0) {
            continue;
        }
        if (size == 0) {
            continue;
        }

        if (strcmp(name, "frw") == 0) {
            map_frw_addr = addr;
            have_map_frw = true;
            rbd_frw_sym_index = count;
        }
        if (strcmp(name, "plw") == 0) {
            if (size < (uint32_t)sizeof(plw)) {
                rbd_fail("symmap plw entry size %u smaller than sizeof(plw) %u", size, (uint32_t)sizeof(plw));
            }
            rbd_plw_sym_index = count;
        }

        rbd_syms[count].addr = addr; /* slide applied below */
        rbd_syms[count].size = size;
        /* Retain the name only for pointer-width entries — the only ones the
         * canonicalization rule can fire on — so the audit log can name
         * them without holding ~3.8k strings. */
        if (size == (uint32_t)sizeof(void*)) {
            rbd_sym_names[count] = SDL_strdup(name);
            if (rbd_sym_names[count] == NULL) {
                rbd_fail("out of memory retaining symbol name \"%s\"", name);
            }
        }
        count += 1;
    }
    fclose(f);

    for (int a = 0; a < anchor_count; a++) {
        if (!anchors[a].found) {
            rbd_fail("symmap \"%s\" lacks anchor symbol %s — map not generated from this binary?", path,
                     anchors[a].name);
        }
    }

    const intptr_t slide = (intptr_t)(anchors[0].runtime - anchors[0].map_addr);
    for (int a = 1; a < anchor_count; a++) {
        const intptr_t s = (intptr_t)(anchors[a].runtime - anchors[a].map_addr);
        if (s != slide) {
            rbd_fail("symmap anchor %s disagrees on ASLR slide (%td vs %td) — map from a different binary",
                     anchors[a].name, s, slide);
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        rbd_syms[i].addr = (uintptr_t)((intptr_t)rbd_syms[i].addr + slide);
    }

    if (have_map_frw) {
        /* The canonical-view hasher covers exactly frw[]; the map entry
         * must agree on extent or the special-casing would silently skip
         * bytes. */
        if ((uintptr_t)((intptr_t)map_frw_addr + slide) != (uintptr_t)&frw[0][0] ||
            rbd_syms[rbd_frw_sym_index].size != (uint32_t)sizeof(frw)) {
            rbd_fail("symmap frw entry size %u disagrees with sizeof(frw) %u", rbd_syms[rbd_frw_sym_index].size,
                     (uint32_t)sizeof(frw));
        }
    }

    rbd_sym_count = count;
}

static void rbd_init(void) {
    rbd_init_attempted = true;

    const TestRunnerConfiguration* test = &configuration.test;

    /* Before the symmap, so a failure here reports as a plumbing failure
     * rather than half-initialising the capture. */
    rbd_compute_image_bounds();
    rbd_refresh_regions();

    rbd_load_symmap(test->rbd_symmap_path);

    rbd_row = (uint32_t*)SDL_malloc((size_t)rbd_sym_count * sizeof(uint32_t));
    rbd_slot_scratch = (unsigned char*)SDL_malloc(sizeof(frw[0]));
    rbd_plw_scratch = (unsigned char*)SDL_malloc(sizeof(PLW));

    const size_t gekko_buf_size = sizeof(State) > SPARSE_CEILING_BYTES ? sizeof(State) : SPARSE_CEILING_BYTES;
    rbd_gekko_buf = (unsigned char*)SDL_malloc(gekko_buf_size);

    if (rbd_row == NULL || rbd_slot_scratch == NULL || rbd_plw_scratch == NULL || rbd_gekko_buf == NULL) {
        rbd_fail("out of memory allocating capture buffers");
    }

    rbd_file = fopen(test->rbd_capture_path, "wb");
    if (rbd_file == NULL) {
        rbd_fail("could not open capture output \"%s\"", test->rbd_capture_path);
    }
    rbd_file_buf = (char*)SDL_malloc(1 << 20);
    if (rbd_file_buf != NULL) {
        setvbuf(rbd_file, rbd_file_buf, _IOFBF, 1 << 20);
    }

    const uint32_t magic = RBD_STREAM_MAGIC;
    if (fwrite(&magic, sizeof(magic), 1, rbd_file) != 1 || fwrite(&rbd_sym_count, sizeof(rbd_sym_count), 1, rbd_file) != 1) {
        rbd_fail("write failure on capture header");
    }

    rbd_active = true;
    fprintf(stderr,
            "[rbd] capture active: %u symbols, %d frames, rollback period=%d depth=%d, "
            "select period=%d depth=%d, %u readable VM regions, image=[%#llx,%#llx) out=%s\n",
            rbd_sym_count, test->rbd_frames, test->rbd_rollback_period, test->rbd_rollback_depth,
            test->rbd_select_rollback_period, test->rbd_select_rollback_depth, rbd_region_count,
            (unsigned long long)rbd_image_lo, (unsigned long long)rbd_image_hi, test->rbd_capture_path);
}

/* One speculative sim step, mirroring src/netplay/netplay.c's
 * advance_game()+step_game() rolling-back leg: inputs applied straight to
 * p1sw_0/_1 + PLsw (bypassing the offline latch), No_Trans set (render
 * suppressed), then the same four calls step_game makes. Input prediction
 * mirrors GekkoNet's repeat-last-confirmed-input: the current p1sw_0 /
 * p2sw_0 are held, so the "previous frame" edge state becomes the held
 * word too. */
static void rbd_speculative_advance(void) {
    p1sw_1 = p1sw_0;
    p2sw_1 = p2sw_0;
    PLsw[0][0] = p1sw_0;
    PLsw[1][0] = p2sw_0;
    PLsw[0][1] = p1sw_1;
    PLsw[1][1] = p2sw_1;

    No_Trans = 1;
    njUserMain();
    seqsBeforeProcess();
    njdp2d_draw();
    seqsAfterProcess();
}

/* Save -> speculatively resimulate depth frames -> load, through the
 * PRODUCTION rollback path: save_state() (gather + sanitize + sparse/full
 * encode into a Gekko-shaped buffer) and load_state_from_event() (format
 * dispatch + GameState_Load + effect-pool reconstruction) — the same code
 * GekkoNet's GekkoSaveEvent/GekkoLoadEvent handlers run in netplay.c. */
static void rbd_rollback_cycle(int depth) {
    const u8 saved_no_trans = No_Trans;
    unsigned int checksum = 0;
    unsigned int state_len = 0;

    GekkoGameEvent save_ev;
    SDL_zero(save_ev);
    save_ev.type = GekkoSaveEvent;
    save_ev.data.save.frame = (int)rbd_frame_index;
    save_ev.data.save.checksum = &checksum;
    save_ev.data.save.state_len = &state_len;
    save_ev.data.save.state = rbd_gekko_buf;
    save_state(&save_ev);

    if (state_len == 0) {
        rbd_fail("save_state produced state_len == 0 at frame %u", rbd_frame_index);
    }

    for (int d = 0; d < depth; d++) {
        rbd_speculative_advance();
    }

    GekkoGameEvent load_ev;
    SDL_zero(load_ev);
    load_ev.type = GekkoLoadEvent;
    load_ev.data.load.frame = (int)rbd_frame_index;
    load_ev.data.load.state = rbd_gekko_buf;
    load_ev.data.load.state_len = state_len;
    load_state_from_event(&load_ev);

    /* No_Trans is deliberately NOT in GameState (render-side; in real
     * netplay every advance re-derives it from the render flag). Our
     * offline continuation frames don't manage it, so restore it here to
     * avoid a harness-inflicted artifact real netplay cannot produce. */
    No_Trans = saved_no_trans;

    rbd_cycle_count += 1;
}

void RollbackDeterminism_PreFrame(void) {
    if (configuration.test.rbd_capture_path == NULL) {
        return;
    }
    if (!rbd_init_attempted) {
        rbd_init();
    }
    if (!rbd_active) {
        return;
    }

    const int period = configuration.test.rbd_rollback_period;
    if (period <= 0) {
        return;
    }

    /* Coverage window: the steady character-select and in-game phases of
     * the test runner's boot script — the same screens real netplay
     * rollbacks cover (the GekkoNet session enters at character select,
     * netplay.c game_ready_to_run_character_select).
     *
     * Deliberately NOT covered: boot/title/menu (real netplay never rolls
     * back there — a first attempt gated on G_No[1] >= 1 injected a cycle
     * during the CAPCOM-logo demo, and re-running the logo's one-shot
     * palette-load init after the restore tripped the arcade trap
     * `while (1) {}` in ppgSetupPalChunk, PPGFile.c:753) and the
     * transition phases where LDREQ asset loads are in flight
     * (character-select-transition / game-transition).
     *
     * Character select is additionally covered at a GENTLE CADENCE but at
     * PRODUCTION DEPTH (default period 8, depth 8): every-frame depth>=2
     * cycles across select straddle one-shot ppg asset setups and hit
     * the crash-class arcade traps against un-rewound render-side
     * texture/palette bookkeeping (observed empirically: period 1 depth
     * 3 hangs in ppgSetupPalChunk's `while (1) {}` trap via the
     * be-flag re-entry check, period 1 depth 2 segfaults in
     * ppgSetupTexChunkSeqs with a NULL destination; period 8 depth 2
     * completes select and reaches in-game on the exact same frame as
     * the baseline run). That crash-class exposure is real and
     * catalogued (docs/rollback-determinism-harness.md "Known limits")
     * but it manifests as a hang/crash, not a byte divergence, so this
     * byte-diff harness scopes it out; the driver's per-run timeout and
     * exit-code checks are what surface it if the gate regresses. */
    if (TestRunner_IsPhaseActive("game")) {
        if ((rbd_frame_index % (uint32_t)period) == 0) {
            rbd_rollback_cycle(configuration.test.rbd_rollback_depth);
        }
        return;
    }

    const int select_period = configuration.test.rbd_select_rollback_period;
    if (select_period > 0 && TestRunner_IsPhaseActive("character-select") &&
        (rbd_frame_index % (uint32_t)select_period) == 0) {
        /* Select-phase depth is governed SOLELY by
         * --rbd-select-rollback-depth (default 8 = production's
         * input_prediction_window, netplay.c:914-916).
         *
         * It used to be `min(--rbd-rollback-depth, --rbd-select-rollback-depth)`,
         * and before that a hard clamp to 2. Both forms had the same defect:
         * the in-game knob silently capped the select knob, so the shipped
         * gate (--rbd-rollback-depth 3) could never exercise select deeper
         * than 3 no matter what the select flag said, and reaching select
         * depth 8 meant raising the IN-GAME depth to 8 as well — which walks
         * straight into the crash class in known limit 1 and changes what
         * the in-game half of the run is even measuring.
         *
         * The two knobs bound different risks and are now independent: the
         * in-game cadence stays at the depth the gate has always used, while
         * select runs at the depth production actually predicts at. That
         * matters concretely — the task-50 duplicate-load leak changes which
         * guard is load-bearing between depth 2 and depth >= 3, because by
         * then the head request has drained and the enqueue-side dedupe no
         * longer sees it, so a regression test written against a depth-2
         * matrix can pass with the texgroup.c reclaim reverted. */
        int select_depth = configuration.test.rbd_select_rollback_depth;
        if (select_depth < 1) {
            select_depth = 1;
        }
        rbd_rollback_cycle(select_depth);
    }
}

static void rbd_finish(void) {
    const TestRunnerConfiguration* test = &configuration.test;
    const uint32_t magic = RBD_FOOTER_MAGIC;
    const uint32_t completed = 1;

    if (fwrite(&magic, sizeof(magic), 1, rbd_file) != 1 ||
        fwrite(&rbd_frame_index, sizeof(rbd_frame_index), 1, rbd_file) != 1 ||
        fwrite(&rbd_cycle_count, sizeof(rbd_cycle_count), 1, rbd_file) != 1 ||
        fwrite(&rbd_ingame_first_frame, sizeof(rbd_ingame_first_frame), 1, rbd_file) != 1 ||
        fwrite(&rbd_ptr_canon_count, sizeof(rbd_ptr_canon_count), 1, rbd_file) != 1 ||
        fwrite(&completed, sizeof(completed), 1, rbd_file) != 1) {
        rbd_fail("write failure on capture footer");
    }
    if (fflush(rbd_file) != 0 || fclose(rbd_file) != 0) {
        rbd_fail("flush/close failure on capture output");
    }
    rbd_file = NULL;

    printf("[rbd] capture complete: frames=%u symbols=%u rollback_cycles=%u ingame_first_frame=%d "
           "ptr_canonicalized=%u out=%s\n",
           rbd_frame_index, rbd_sym_count, rbd_cycle_count,
           rbd_ingame_first_frame == 0xFFFFFFFFu ? -1 : (int)rbd_ingame_first_frame, rbd_ptr_canon_count,
           test->rbd_capture_path);
    fflush(stdout);

    rbd_active = false;
    /* Same clean shutdown path input_script.c's Q directive and the perf
     * capture completion use: push SDL_EVENT_QUIT so the main loop ends
     * next tick with exit code 0. Failures in this harness NEVER ride
     * this path — they exit(RBD_EXIT_CODE_FAILED) immediately. */
    SDLApp_Exit();
}

/* Decide, once, which symbols the whole-pointer rule applies to, from the
 * state of the world at the first captured frame. Never revisited — see the
 * RBD_PTR_TOKEN block, bound 2, for why a per-frame test is wrong. */
static void rbd_freeze_pointer_slots(void) {
    rbd_ptr_slots_frozen = true;
    rbd_regions_refreshed_this_frame = false;
    rbd_refresh_regions();

    for (uint32_t i = 0; i < rbd_sym_count; i++) {
        if (i == rbd_frw_sym_index || i == rbd_plw_sym_index) {
            continue; /* already hashed through their own canonical views */
        }
        if (rbd_syms[i].size != (uint32_t)sizeof(void*) || (rbd_syms[i].addr % sizeof(void*)) != 0) {
            continue;
        }
        uintptr_t v;
        memcpy(&v, (const void*)rbd_syms[i].addr, sizeof(v));
        if (!rbd_addr_is_process_varying(v)) {
            continue;
        }
        rbd_ptr_canon[i] = 1;
        rbd_ptr_canon_count += 1;
        fprintf(stderr, "[rbd] pointer-canonicalized %s (map addr %#llx, value %#llx)\n",
                rbd_sym_names[i] != NULL ? rbd_sym_names[i] : "(unnamed)", (unsigned long long)rbd_syms[i].addr,
                (unsigned long long)v);
    }

    fprintf(stderr, "[rbd] froze %u whole-pointer statics at frame %u (of %u symbols, %u readable VM regions)\n",
            rbd_ptr_canon_count, rbd_frame_index, rbd_sym_count, rbd_region_count);
}

void RollbackDeterminism_FrameEnd(void) {
    if (!rbd_active) {
        return;
    }

    if (rbd_ingame_first_frame == 0xFFFFFFFFu && G_No[1] == 2) {
        rbd_ingame_first_frame = rbd_frame_index;
    }

    if (!rbd_ptr_slots_frozen) {
        rbd_freeze_pointer_slots();
    }

    const uint64_t ptr_token = RBD_PTR_TOKEN;
    for (uint32_t i = 0; i < rbd_sym_count; i++) {
        if (i == rbd_frw_sym_index) {
            rbd_row[i] = rbd_hash_frw_canonical();
        } else if (i == rbd_plw_sym_index) {
            rbd_row[i] = rbd_hash_plw_sanitized();
        } else if (rbd_ptr_canon[i] != 0) {
            /* Whole-pointer static: hash a fixed token rather than the
             * address. See the RBD_PTR_TOKEN block — this is a hash change,
             * not a classification, so it lands identically in A1, A2 and B
             * and there is nothing left for the baseline control to be
             * lucky about. */
            rbd_row[i] = rbd_hash_fold(rbd_hash64(RBD_HASH_SEED, &ptr_token, sizeof(ptr_token)));
        } else {
            rbd_row[i] = rbd_hash_fold(rbd_hash64(RBD_HASH_SEED, (const void*)rbd_syms[i].addr, rbd_syms[i].size));
        }
    }

    if (fwrite(&rbd_frame_index, sizeof(rbd_frame_index), 1, rbd_file) != 1 ||
        fwrite(rbd_row, sizeof(uint32_t), rbd_sym_count, rbd_file) != rbd_sym_count) {
        rbd_fail("write failure on capture row (frame %u)", rbd_frame_index);
    }

    rbd_frame_index += 1;

    if ((int)rbd_frame_index >= configuration.test.rbd_frames) {
        rbd_finish();
    }
}

#else /* !DEBUG || !ENABLE_NETPLAY — inert stubs (args.c rejects --rbd-capture here) */

#include "test/rollback_determinism.h"

void RollbackDeterminism_PreFrame(void) {}

void RollbackDeterminism_FrameEnd(void) {}

#endif
