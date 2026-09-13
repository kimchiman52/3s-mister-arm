#ifndef ARCADE_CHAR_DATA_H
#define ARCADE_CHAR_DATA_H

#include "constants.h"
#include "structs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum CharDataSection {
    CHAR_DATA_NMCA,
    CHAR_DATA_DMCA,
    CHAR_DATA_BTCA,
    CHAR_DATA_CACA,
    CHAR_DATA_CUCA,
    CHAR_DATA_ATCA,
    CHAR_DATA_SACA,
    CHAR_DATA_EXCA,
    CHAR_DATA_CBCA,
    CHAR_DATA_YUCA,
    CHAR_DATA_STXY,
    CHAR_DATA_MVXY,
    CHAR_DATA_SERND,
    CHAR_DATA_OVCT,
    CHAR_DATA_OVIX,
    CHAR_DATA_RICT,
    CHAR_DATA_HIIT,
    CHAR_DATA_BODA,
    CHAR_DATA_HANA,
    CHAR_DATA_CATA,
    CHAR_DATA_CAUA,
    CHAR_DATA_ATTA,
    CHAR_DATA_HOSA,
    CHAR_DATA_ATIT,
    CHAR_DATA_PROT,
    CHAR_DATA_SECTION_COUNT
} CharDataSection;

typedef struct CharDataSpan {
    void* data;
    size_t size;
    size_t element_size;
} CharDataSpan;

typedef struct CharDataImage {
    CharInitData tables;
    CharDataSpan spans[CHAR_DATA_SECTION_COUNT];
} CharDataImage;

void ArcadeCharData_Init();
bool ArcadeCharData_IsInitialized();
const CharInitData* ArcadeCharData_Get(Character character);
bool ArcadeCharData_Apply3SXRenderingConventions(Character character, const void* ps2_data, size_t ps2_size);

/// SHA-256-derived 64-bit digest over every parsed section span of every
/// character. Call AFTER the full adaptation pass so the digest covers the
/// exact bytes the simulation reads. Returns 0 when char data is not
/// initialized (0 is reserved for "no digest"). Wired into the MIST netplay
/// handshake so peers whose adapted arcade data differs (e.g. different ROM
/// revisions) are rejected instead of desyncing.
uint64_t ArcadeCharData_ComputeDigest();

#if defined(ENABLE_NETPLAY_TESTS)
/// Test seam for src/test/test_cg_se_remap.c: exposes the parse-time
/// per-character cg_se sound-code remap (doc §8.Q / §21) so the harness can
/// sweep the whole (character, code) domain. Test builds only.
uint16_t ArcadeCharData_TestRemapCgSe(uint16_t value, Character character);
#endif

#if defined(DEBUG) || defined(ENABLE_NETPLAY_TESTS)
/// One row of a character's CG-number remap table: raw CPS3 cg_numbers in
/// `[first, last]` are shifted by `delta` into the PS2 numbering
/// (doc §4.4). Visible here only so src/test/test_cg_ranges.c can hand
/// `ArcadeCharData_CgRangeDefects` synthetic tables as negative controls;
/// the twenty real tables stay private to arcade_char_data.c.
typedef struct CgRemapRange {
    uint16_t first;
    uint16_t last;
    int32_t delta;
} CgRemapRange;

/// Count (and SDL_LogError) the rows of ONE table that break the table
/// invariant: no row inverted (`first > last`), no two rows overlapping.
/// `remap_cg_number` takes the FIRST matching row and stops, so an overlap
/// does not fail — it silently applies whichever delta happens to be earlier
/// in the array, which makes the shipped mapping (and the netplay balance
/// digest that covers it) a function of source ORDER. `label` names the table
/// in the diagnostics. Returns 0 when the table is well formed.
int ArcadeCharData_CgRangeDefects(const CgRemapRange* ranges, size_t count, const char* label);

/// The same check over all twenty live `cg_maps[]` tables plus the five
/// `cg_se_maps[]` pair lists (duplicate `from`, which `remap_cg_se` would
/// also resolve by position). Returns the total defect count; 0 is the only
/// acceptable answer. Called from `ArcadeCharData_Init` under DEBUG and by
/// `--test-cg-ranges` in a test build.
///
/// A character whose `cg_maps[]` entry has no ranges at all counts as a defect:
/// `CgRangeDefects(NULL, 0, ...)` is vacuously 0, so without that a dropped
/// initializer read green while every raw for that character fell through to
/// `default_delta`.
int ArcadeCharData_CgTableDefects(void);

/// Total `CgRemapRange` rows across all twenty `cg_maps[]` tables. Exposed so
/// `--test-cg-ranges` can pin the population rather than print `NUM_CHARS`,
/// which is a compile-time constant and proves nothing about the tables.
size_t ArcadeCharData_CgRangeRows(void);
#endif

#endif
