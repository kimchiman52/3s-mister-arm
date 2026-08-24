#include "arcade/rom_load.h"
#include "arcade/cps3_decrypt.h"
#include "utils/sha256.h"

#include <SDL3/SDL.h>
#include <minizip-ng/mz.h>
#include <minizip-ng/mz_strm.h>
#include <minizip-ng/mz_strm_os.h>
#include <minizip-ng/mz_zip.h>

#include <stdbool.h>

#define READ_CHUNK_SIZE (1024 * 10)

/* The four decryption inputs are the sfiii3nr1 SIMM1 slices. Each is
 * exactly 2 MiB in every known packaging of the set. */
#define ROM_SIMM_COUNT 4
#define ROM_SIMM_SIZE (2u * 1024u * 1024u)

/* Entries are matched by CONTENT, not by name or path, so the loader is
 * immune to merged-vs-split packaging, variant subdirectories
 * (sfiii3n/ vs sfiii3nar1/ ...) and future set reorganizations. The
 * merged MAME set even carries same-named SIMMs with DIFFERENT bytes
 * (sfiii3n/sfiii3-simm1.0 != sfiii3nar1/sfiii3-simm1.0); a name match
 * would silently pick the wrong revision depending on zip entry order.
 *
 * Match pipeline per required slice:
 *   1. cheap pre-filter on the zip central directory's stored CRC32 +
 *      uncompressed size (no decompression, so scanning a ~95 MB merged
 *      set touches only its directory);
 *   2. decompress the candidate (2 MiB) and verify its SHA-256 against
 *      the pinned digest below — the authoritative check.
 * Digests pinned 2026-08-23 from a known-good sfiii3nr1 set; the same
 * bytes appear in the update_all merged set under sfiii3nar1/. */
typedef struct RomSimmSpec {
    const char* name; /* canonical flat name — logging only, never matched */
    Uint32 crc32;
    const char* sha256_hex;
} RomSimmSpec;

static const RomSimmSpec simm_specs[ROM_SIMM_COUNT] = {
    { "sfiii3-simm1.0", 0x66E66235, "0ddcfaa946a4c22c141980a137aa495d3accfe93e9e2893448a602a139b3715e" },
    { "sfiii3-simm1.1", 0x186E8C5F, "7c0395585e77411d1dca6b60184df75631a34cf13d8bff25ba99d1354d5eb053" },
    { "sfiii3-simm1.2", 0xBCE18CAB, "30b5e727c071f3fff2a93fb61c518f447257cb9dea8e5ec1cf96f612c6ecedb0" },
    { "sfiii3-simm1.3", 0x129DC2C9, "d9597fdc7baea1571cd3332d2b29e73da000e2995cbd753a113b62399bfdc900" },
};

/* Decompress the currently-open zip entry (exactly ROM_SIMM_SIZE bytes)
 * into `dst`, hashing as it streams. Returns true when the entry
 * yielded exactly ROM_SIMM_SIZE bytes and its SHA-256 hex equals
 * `expected_sha256_hex`. */
static bool read_and_verify_entry(void* zip, Uint8* dst, void* read_buf, const char* expected_sha256_hex) {
    if (mz_zip_entry_read_open(zip, false, NULL) != MZ_OK) {
        return false;
    }

    sha256 sha;

    if (!sha256_init(&sha)) {
        mz_zip_entry_close(zip);
        return false;
    }

    size_t total = 0;
    int32_t read = 0;

    while ((read = mz_zip_entry_read(zip, read_buf, READ_CHUNK_SIZE)) > 0) {
        if (total + (size_t)read > ROM_SIMM_SIZE) {
            /* Larger than declared — cannot be our slice. */
            mz_zip_entry_close(zip);
            return false;
        }

        SDL_memcpy(dst + total, read_buf, (size_t)read);
        sha256_append(&sha, read_buf, (size_t)read);
        total += (size_t)read;
    }

    mz_zip_entry_close(zip);

    if (total != ROM_SIMM_SIZE) {
        return false;
    }

    char hex[SHA256_HEX_SIZE];

    if (!sha256_finalize_hex(&sha, hex)) {
        return false;
    }

    return SDL_strcmp(hex, expected_sha256_hex) == 0;
}

static void* decrypt(Uint8* const simms[ROM_SIMM_COUNT], size_t* size) {
    const size_t buf_size = (size_t)ROM_SIMM_SIZE * ROM_SIMM_COUNT;
    Uint32* buf = SDL_malloc(buf_size);

    /* 8 MiB on a 1 GiB MiSTer that has already loaded the game — the one
     * allocation here most likely to actually fail. Unchecked, the loop
     * below wrote through a NULL pointer and took the process down at
     * boot; returning NULL instead folds into Rom_Load's existing
     * "no usable ROM" contract, which the balance auto-select reads as
     * ROM-absent and answers with PS2 balance. */
    if (buf == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Rom_Load: out of memory allocating the %zu-byte decrypt buffer",
                     buf_size);
        *size = 0;
        return NULL;
    }

    for (Uint32 i = 0; i < ROM_SIMM_SIZE; i++) {
        buf[i] = cps3_decrypt(simms[0][i], simms[1][i], simms[2][i], simms[3][i], i);
    }

    *size = buf_size;
    return buf;
}

void* Rom_Load(const char* path, size_t* size) {
    void* stream = mz_stream_os_create();

    if (mz_stream_open(stream, path, MZ_OPEN_MODE_READ) != MZ_OK) {
        mz_stream_os_delete(&stream);
        return NULL;
    }

    void* zip = mz_zip_create();
    int32_t err = mz_zip_open(zip, stream, MZ_OPEN_MODE_READ);

    if (err != MZ_OK) {
        mz_zip_close(zip);
        mz_zip_delete(&zip);
        mz_stream_os_delete(&stream);
        return NULL;
    }

    err = mz_zip_goto_first_entry(zip);

    void* read_buf = SDL_malloc(READ_CHUNK_SIZE);
    Uint8* simms[ROM_SIMM_COUNT] = { 0 };
    int simm_count = 0;

    /* Allocation-failure latch. It cannot ride in `err`: the loop tail
     * reassigns err from mz_zip_goto_next_entry on every iteration, so
     * an error stored there would be silently erased before the loop
     * condition next reads it. */
    bool alloc_failed = false;

    if (read_buf == NULL) {
        /* read_and_verify_entry decompresses through this buffer, so a
         * NULL here corrupts every entry read. Skip the scan entirely —
         * simm_count stays 0, the shared cleanup below still runs, and
         * Rom_Load returns NULL (ROM-absent). */
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Rom_Load: out of memory allocating the %d-byte read buffer",
                     (int)READ_CHUNK_SIZE);
        alloc_failed = true;
    }

    while (err == MZ_OK && !alloc_failed && simm_count < ROM_SIMM_COUNT) {
        mz_zip_file* info = NULL;

        if (mz_zip_entry_get_info(zip, &info) == MZ_OK && info != NULL) {
            for (int slot = 0; slot < ROM_SIMM_COUNT; slot++) {
                const RomSimmSpec* spec = &simm_specs[slot];

                if (simms[slot] != NULL || info->crc != spec->crc32 ||
                    info->uncompressed_size != (int64_t)ROM_SIMM_SIZE) {
                    continue;
                }

                Uint8* data = SDL_malloc(ROM_SIMM_SIZE);

                if (data == NULL) {
                    /* 2 MiB per SIMM slot. read_and_verify_entry would
                     * write the decompressed entry straight through this
                     * pointer. Abandon the scan: leaving simms[slot] NULL
                     * makes simm_count < ROM_SIMM_COUNT, so the shared
                     * "no entry matched" path reports it and Rom_Load
                     * returns NULL (ROM-absent -> PS2 balance). */
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Rom_Load: out of memory allocating the %u-byte %s buffer",
                                 (unsigned)ROM_SIMM_SIZE, spec->name);
                    alloc_failed = true;
                    break;
                }

                if (read_and_verify_entry(zip, data, read_buf, spec->sha256_hex)) {
                    SDL_Log("Rom_Load: %s satisfied by entry '%s' (crc32 %08x, sha256 verified)",
                            spec->name,
                            info->filename,
                            (unsigned)info->crc);
                    simms[slot] = data;
                    simm_count += 1;
                } else {
                    /* CRC pre-filter hit but the content digest did not
                     * confirm (corrupt entry or a CRC collision). Keep
                     * scanning — another entry may carry the real bytes. */
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Rom_Load: entry '%s' matched the CRC32 pre-filter for %s "
                                "but failed SHA-256 verification; skipping it",
                                info->filename,
                                spec->name);
                    SDL_free(data);
                }

                /* An entry can satisfy at most one slot (digests are all
                 * distinct), and reading consumed the entry cursor. */
                break;
            }
        }

        err = mz_zip_goto_next_entry(zip);
    }

    void* result = NULL;

    if (simm_count == ROM_SIMM_COUNT) {
        result = decrypt(simms, size);
    } else if (alloc_failed) {
        /* The scan was cut short by the already-logged allocation
         * failure, so the unfilled slots prove nothing about this zip.
         * Do NOT claim "no entry matched" — that would misdirect a
         * field report toward a bad ROM file. */
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Rom_Load: %s: aborted after an allocation failure with %d/%d "
                     "SIMMs read; ROM treated as unavailable",
                     path, simm_count, ROM_SIMM_COUNT);
    } else {
        for (int slot = 0; slot < ROM_SIMM_COUNT; slot++) {
            if (simms[slot] == NULL) {
                SDL_Log("Rom_Load: %s: no entry matched %s (crc32 %08x)",
                        path,
                        simm_specs[slot].name,
                        (unsigned)simm_specs[slot].crc32);
            }
        }
    }

    // Cleanup

    for (int i = 0; i < ROM_SIMM_COUNT; i++) {
        SDL_free(simms[i]);
    }

    SDL_free(read_buf);
    mz_zip_close(zip);
    mz_zip_delete(&zip);
    mz_stream_os_delete(&stream);

    return result;
}
