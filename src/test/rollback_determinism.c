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

/* Exit code for capture-setup/IO failures. Distinct from input_script.c's
 * 3 (failed-to-start) so the driver can tell the two apart; both mean
 * "harness plumbing", never "divergence found" (divergence is judged by
 * the driver, not in-game). */
#define RBD_EXIT_CODE_FAILED 4

#define RBD_STREAM_MAGIC 0x31444252u /* "RBD1" little-endian */
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
        }

        /* Chain across slots: keep the running 64-bit state as seed. */
        h = rbd_hash64(h, rbd_slot_scratch, slot_bytes);
    }

    return rbd_hash_fold(h);
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

        rbd_syms[count].addr = addr; /* slide applied below */
        rbd_syms[count].size = size;
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

    rbd_load_symmap(test->rbd_symmap_path);

    rbd_row = (uint32_t*)SDL_malloc((size_t)rbd_sym_count * sizeof(uint32_t));
    rbd_slot_scratch = (unsigned char*)SDL_malloc(sizeof(frw[0]));

    const size_t gekko_buf_size = sizeof(State) > SPARSE_CEILING_BYTES ? sizeof(State) : SPARSE_CEILING_BYTES;
    rbd_gekko_buf = (unsigned char*)SDL_malloc(gekko_buf_size);

    if (rbd_row == NULL || rbd_slot_scratch == NULL || rbd_gekko_buf == NULL) {
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
    fprintf(stderr, "[rbd] capture active: %u symbols, %d frames, rollback period=%d depth=%d out=%s\n",
            rbd_sym_count, test->rbd_frames, test->rbd_rollback_period, test->rbd_rollback_depth,
            test->rbd_capture_path);
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
     * Character select is additionally covered only at a GENTLE cadence
     * (default period 8, depth clamped to 2): every-frame depth>=2
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
        const int depth = configuration.test.rbd_rollback_depth;
        rbd_rollback_cycle(depth > 2 ? 2 : depth);
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
        fwrite(&completed, sizeof(completed), 1, rbd_file) != 1) {
        rbd_fail("write failure on capture footer");
    }
    if (fflush(rbd_file) != 0 || fclose(rbd_file) != 0) {
        rbd_fail("flush/close failure on capture output");
    }
    rbd_file = NULL;

    printf("[rbd] capture complete: frames=%u symbols=%u rollback_cycles=%u ingame_first_frame=%d out=%s\n",
           rbd_frame_index, rbd_sym_count, rbd_cycle_count,
           rbd_ingame_first_frame == 0xFFFFFFFFu ? -1 : (int)rbd_ingame_first_frame, test->rbd_capture_path);
    fflush(stdout);

    rbd_active = false;
    /* Same clean shutdown path input_script.c's Q directive and the perf
     * capture completion use: push SDL_EVENT_QUIT so the main loop ends
     * next tick with exit code 0. Failures in this harness NEVER ride
     * this path — they exit(RBD_EXIT_CODE_FAILED) immediately. */
    SDLApp_Exit();
}

void RollbackDeterminism_FrameEnd(void) {
    if (!rbd_active) {
        return;
    }

    if (rbd_ingame_first_frame == 0xFFFFFFFFu && G_No[1] == 2) {
        rbd_ingame_first_frame = rbd_frame_index;
    }

    for (uint32_t i = 0; i < rbd_sym_count; i++) {
        if (i == rbd_frw_sym_index) {
            rbd_row[i] = rbd_hash_frw_canonical();
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
