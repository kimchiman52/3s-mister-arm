#include "arcade/arcade_balance.h"
#include "arcade/arcade_char_data.h"
#include "main.h"
#include "port/config/config.h"
#include "port/io/afs.h"
#include "port/paths.h"
#include "sf33rd/Source/Game/rendering/texgroup.h"

#include <SDL3/SDL.h>

#include <stdarg.h>

static bool is_enabled = false;
static uint64_t digest = 0;
static char ps2_reason[192] = "not initialized";

static void set_ps2_reason(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(ps2_reason, sizeof(ps2_reason), fmt, ap);
    va_end(ap);
}

/* Boot-time ALL-OR-NOTHING adaptation. Runs the same
 * Apply3SXRenderingConventions call the lazy texture-group loader
 * (texgroup.c q_ldreq case 4) would make, for every character up front,
 * using the identical PS2 bytes: each character's row in texgrpdat[]
 * (rows 1..NUM_CHARS are Gill(0)..Remy(19) in Character-enum order — the
 * same index Apply and location_data[] use) names the AFS member
 * (bsd->apfn) and the char-data offset (bsd->to_chd); the lazy path
 * computes ldchd = file + to_chd and size = fsGetFileSize(apfn) - to_chd,
 * and AFS_ReadRange reads exactly that tail (~100-220 KiB per character,
 * ~3 MiB total — not the multi-megabyte whole files).
 *
 * Any failure fails the whole pass: previously a single character's
 * adaptation failure silently fell back to PS2 data for just that
 * character, producing a mixed balance nobody chose. Now the session is
 * cleanly arcade (20/20 adapted) or cleanly PS2.
 *
 * Correct-pairing cross-check: Apply structurally compares the arcade
 * OVCT table against the PS2 one (overlap_behavior_matches) and fails on
 * divergence, so a mispaired (character, PS2 blob) cannot silently pass. */
static bool adapt_all_characters(void) {
    for (int character = 0; character < NUM_CHARS; character++) {
        const TexGroupData* bsd = &texgrpdat[character + 1];
        const unsigned int file_size = AFS_GetSize(bsd->apfn);

        if (bsd->ix1st != 1 || bsd->to_chd == 0 || file_size <= bsd->to_chd) {
            set_ps2_reason("arcade adaptation failed: bad char-data location for character %d "
                           "(apfn=%d ix1st=%d to_chd=0x%X afs_size=0x%X)",
                           character,
                           bsd->apfn,
                           bsd->ix1st,
                           (unsigned)bsd->to_chd,
                           file_size);
            return false;
        }

        const unsigned int ps2_size = file_size - bsd->to_chd;
        void* ps2_data = SDL_malloc(ps2_size);

        if (ps2_data == NULL || !AFS_ReadRange(bsd->apfn, bsd->to_chd, ps2_size, ps2_data)) {
            SDL_free(ps2_data);
            set_ps2_reason("arcade adaptation failed: could not read PS2 char data for character %d "
                           "(AFS file %d range 0x%X+0x%X)",
                           character,
                           bsd->apfn,
                           (unsigned)bsd->to_chd,
                           ps2_size);
            return false;
        }

        const bool adapted = ArcadeCharData_Apply3SXRenderingConventions(character, ps2_data, ps2_size);
        SDL_free(ps2_data);

        if (!adapted) {
            set_ps2_reason("arcade adaptation failed for character %d", character);
            return false;
        }
    }

    return true;
}

/* Machine-readable resolution for the wrapper/OSD status line:
 *   line 1: status text ("Arcade (CPS3)" | "PS2")
 *   line 2: reason (empty when arcade)
 * Written on every boot resolution, next to the config file. */
static void write_status_file(void) {
    char* path = NULL;
    SDL_asprintf(&path, "%sbalance.status", Paths_GetPrefPath());

    if (path == NULL) {
        return;
    }

    SDL_IOStream* io = SDL_IOFromFile(path, "w");

    if (io != NULL) {
        SDL_IOprintf(io, "%s\n%s\n", ArcadeBalance_GetStatusText(), ps2_reason);
        SDL_CloseIO(io);
    }

    SDL_free(path);
}

void ArcadeBalance_Init() {
    is_enabled = false;
    digest = 0;

    /* Balance AUTO-SELECTS at boot: CPS3 ROM present AND the full
     * 20-character adaptation succeeds -> arcade balance; anything else
     * -> PS2 balance with a logged reason. There is no OSD toggle. */
    do {
        if (configuration.test.enabled) {
            /* The frame-data suite's corpora encode PS2-balance
             * expectations; the harness must resolve identically on
             * every machine regardless of ROM presence. */
            set_ps2_reason("test runner pins PS2 balance");
            break;
        }

        const char* override = Config_GetString(CFG_KEY_BALANCE);

        if (override != NULL && SDL_strcasecmp(override, "ps2") == 0) {
            set_ps2_reason("config override balance=ps2");
            break;
        }

        if (override != NULL && SDL_strcasecmp(override, "auto") != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unknown balance override '%s' (expected 'auto' or 'ps2'); treating as auto",
                        override);
        }

        ArcadeCharData_Init();

        if (!ArcadeCharData_IsInitialized()) {
            set_ps2_reason("CPS3 ROM not found or failed content verification");
            break;
        }

        if (!adapt_all_characters()) {
            /* set_ps2_reason already holds the specific failure. Any
             * partially-adapted arcade tables stay allocated but are
             * never consulted: every runtime reader gates on
             * ArcadeBalance_IsEnabled(), which stays false. */
            break;
        }

        is_enabled = true;
        digest = ArcadeCharData_ComputeDigest();
        ps2_reason[0] = '\0';
    } while (0);

    if (is_enabled) {
        SDL_Log("Arcade balance auto-selected: CPS3 ROM verified, 20/20 characters adapted "
                "(digest %016llx)",
                (unsigned long long)digest);
    } else {
        SDL_Log("PS2 balance selected: %s", ps2_reason);
    }

    write_status_file();
}

bool ArcadeBalance_IsEnabled() {
    return is_enabled;
}

const char* ArcadeBalance_GetStatusText() {
    return is_enabled ? "Arcade (CPS3)" : "PS2";
}

const char* ArcadeBalance_GetReason() {
    return ps2_reason;
}

uint64_t ArcadeBalance_GetDigest() {
    return digest;
}
