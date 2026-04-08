# Plan: MTS Tile Cache Hash Index + Free-List Stack

## Problem

The MTS tile cache (`get_mltbuf16`, `get_mltbuf32`, and their `_ext`/`_ext_2` variants)
performs O(N) linear scans over `PatternState` arrays on every tile lookup. With cache
sizes up to 1024 (16x16) and 2176 (32x32) entries, and hundreds of tiles per frame,
this costs 2-6ms per frame on the ARM Cortex-A9 @ 800MHz.

## Solution

Add a separate hash index (open-addressed, linear probing) and a free-list stack alongside
the existing `mltcsh16`/`mltcsh32` `PatternState` flat arrays. The flat arrays are unchanged;
the hash table is a parallel lookup accelerator. The free-list replaces scan-for-free-slot.

### Key design decisions

- **Hash table**: Dynamically-sized array of `u16` indices per cache (16x16 and 32x32),
  separate from the `PatternState` arrays. Sentinel value `0xFFFF` means empty bucket.
- **Hash function**: Knuth multiplicative hash on the composite key `(code, palt)`.
  `palt` is stored in `PatternState.state` (s16); `code` is `PatternState.cs.code` (u32).
  Compose a 32-bit key: `(code ^ (palt << 16))`, multiply by `0x9E3779B9`, shift right to
  get bucket index.
- **Table size**: Power-of-two bucket counts, chosen at init time per cache:
  - `mltnum <= 1024`: use 2048 buckets (mask `0x7FF`), load factor <= 0.5.
  - `1024 < mltnum <= 2048`: use 4096 buckets (mask `0xFFF`), load factor <= 0.5.
  - `mltnum > 2048` (only DM x32 = 2176): use 4096 buckets (mask `0xFFF`), load factor ~0.53.
  Open-addressed linear probing requires bucket count > entry count. The previous plan used
  max 2048 buckets, which is impossible for DM's x32 cache (2176 entries > 2048 buckets).
- **Probe limit**: 16 probes. If exceeded, fall back to the original linear scan (safety net).
- **Free-list stack**: Array of `u16` indices + top-of-stack counter. Initialized with all
  slots. On allocation, pop from stack. On eviction (TTL expiry), push back to stack.
- **Memory**: Allocated as part of `make_texcash_work`'s single `Pull_ramcnt_key` allocation.

### Scope of change

| File | What changes |
|------|-------------|
| `include/structs.h` | New structs: `MtsCacheIndex`, `MtsFreeList`. New fields in `MultiTexture`. |
| `src/.../rendering/mts_hash.h` | **New file.** Static inline hash/free-list helpers, included by both mtrans.c and texcash.c. |
| `src/.../mtrans.c` | Includes `mts_hash.h`. Modified `get_mltbuf*`. Modified `mlt_obj_trans_init`, `mlt_obj_trans_update`. |
| `src/.../texcash.c` | Includes `mts_hash.h`. Modified `make_texcash_work`, `clear_texcash_work`, `init_texcash_2nd`, `update_with_tpu_free`. |

---

## Step 1: Add data structures

### Title
Define `MtsCacheIndex` and `MtsFreeList` structs; add pointer fields to `MultiTexture`.

### Why
All subsequent steps depend on these types existing. This step has zero behavioral change --
the new fields are added but never initialized or used.

### Files to read
- `/Users/sb/Developer/3sx-mister/include/structs.h` (lines 1445-1513, the TexturePoolFree
  through MultiTexture definitions)

### Files to modify
- `/Users/sb/Developer/3sx-mister/include/structs.h`

### Changes
Add these structs immediately before the `MultiTexture` typedef:

```c
#define MTS_HASH_BUCKETS_2K  2048
#define MTS_HASH_BUCKETS_4K  4096
#define MTS_HASH_EMPTY       0xFFFF
#define MTS_HASH_PROBE_LIMIT 16

typedef struct {
    u16* buckets;     /* indexed by hash; value = slot index or MTS_HASH_EMPTY */
    u16 bucket_mask;  /* (bucket_count - 1); either 0x7FF (2K) or 0xFFF (4K) */
    u16 bucket_count; /* 2048 or 4096 */
} MtsCacheIndex;

typedef struct {
    u16* slots;   /* stack array, capacity = mltnum16 or mltnum32 */
    s32 top;      /* index of next free slot to pop; -1 = empty */
} MtsFreeList;
```

Add these fields to `MultiTexture`, after the `tpu` field and before the `id` field:

```c
    MtsCacheIndex* hash16;
    MtsCacheIndex* hash32;
    MtsFreeList free16;
    MtsFreeList free32;
```

Note: `MtsFreeList` is embedded by value (small: just a pointer + int). `MtsCacheIndex` is
allocated by pointer because it lives in the ramcnt allocation block. The `buckets` array is
also allocated within the ramcnt block (dynamically sized: 4KB for 2K buckets, 8KB for 4K
buckets), not embedded in the struct. This avoids wasting memory on small caches while
supporting the 4096-bucket tables needed for DM's x32 cache (2176 entries).

### Success criteria
- `tools/mister/build-game.sh --flavor telemetry --jobs 4` compiles cleanly with zero errors.
- No behavioral change. Game runs identically.

### Dependencies
None.

### Scope limits
- Do NOT initialize the new fields.
- Do NOT allocate memory for them.
- Do NOT modify any function body.

### Fallback
If the build fails due to include order or type conflicts, check that the new structs are
placed after `PatternState` and before `MultiTexture`. Ensure `u16`, `s32` are available
(they are, since `structs.h` includes `types.h` at the top).

---

## Step 2: Add hash helper functions (static, unused)

### Title
Implement static hash helper functions in `mtrans.c`: `mts_hash_key`, `mts_hash_insert`,
`mts_hash_remove`, `mts_hash_lookup`, `mts_hash_clear`.

### Why
These are the building blocks for all subsequent steps. Keeping them static and unused means
zero behavioral risk. The compiler will warn about unused statics, but the build should succeed.

### Files to read
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c` (lines 1709-1886,
  the existing `get_mltbuf*` functions, to understand the key structure)
- `/Users/sb/Developer/3sx-mister/include/structs.h` (the new structs from Step 1)

### Files to create
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mts_hash.h`

### Files to modify
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c` (add `#include "mts_hash.h"`)
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/texcash.c` (add `#include "mts_hash.h"`)

### Changes
Create `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mts_hash.h` with all
hash/free-list helper functions as `static inline` or `static`. Include this header from both
`mtrans.c` and `texcash.c`. This avoids the cross-file visibility problem that would otherwise
require moving functions later in Step 3 (texcash.c needs `mts_hash_clear`, `mts_hash_insert`,
etc.). Placing all helpers in a shared header from the start is cleaner.

**Contents of `mts_hash.h`**:

```c
/* --- MTS hash index helpers --- */

static inline u32 mts_hash_key(u32 code, u32 palt) {
    u32 k = code ^ ((u32)palt << 16);
    return k * 0x9E3779B9u;
}

/*
 * Compute the number of index bits from bucket_count.
 * 2048 -> 11 bits, 4096 -> 12 bits.
 */
static inline u32 mts_hash_bits(const MtsCacheIndex* idx) {
    return (idx->bucket_count == MTS_HASH_BUCKETS_4K) ? 12 : 11;
}

/*
 * Lookup (code, palt) in hash table.
 * Returns the slot index stored in the bucket, or -1 if not found.
 */
static s32 mts_hash_lookup(const MtsCacheIndex* idx, u32 code, u32 palt,
                           const PatternState* mc) {
    u32 h = mts_hash_key(code, palt) >> (32 - mts_hash_bits(idx));
    s32 probe;

    for (probe = 0; probe < MTS_HASH_PROBE_LIMIT; probe++) {
        u16 slot = idx->buckets[(h + probe) & idx->bucket_mask];
        if (slot == MTS_HASH_EMPTY)
            return -1;
        if (mc[slot].cs.code == code && mc[slot].state == (s16)palt)
            return (s32)slot;
    }
    return -1; /* probe limit exceeded */
}

/*
 * Insert slot into hash table. Caller must ensure slot is valid and
 * mc[slot] already has code/state set.
 * Returns 1 on success, 0 if table is full (probe limit exceeded).
 */
static s32 mts_hash_insert(MtsCacheIndex* idx, u32 code, u32 palt, u16 slot) {
    u32 h = mts_hash_key(code, palt) >> (32 - mts_hash_bits(idx));
    s32 probe;

    for (probe = 0; probe < MTS_HASH_PROBE_LIMIT; probe++) {
        u16 bucket = (h + probe) & idx->bucket_mask;
        if (idx->buckets[bucket] == MTS_HASH_EMPTY) {
            idx->buckets[bucket] = slot;
            return 1;
        }
    }
    return 0; /* probe limit exceeded */
}

/*
 * Remove (code, palt) from hash table. Uses backward-shift deletion
 * to maintain linear-probing invariant.
 */
static void mts_hash_remove(MtsCacheIndex* idx, u32 code, u32 palt,
                            const PatternState* mc) {
    u32 bits = mts_hash_bits(idx);
    u32 h = mts_hash_key(code, palt) >> (32 - bits);
    u16 mask = idx->bucket_mask;
    s32 probe;
    u16 pos = 0xFFFF;

    /* Find the bucket holding this entry */
    for (probe = 0; probe < MTS_HASH_PROBE_LIMIT; probe++) {
        u16 b = (h + probe) & mask;
        if (idx->buckets[b] == MTS_HASH_EMPTY)
            return; /* not found, nothing to remove */
        if (mc[idx->buckets[b]].cs.code == code &&
            mc[idx->buckets[b]].state == (s16)palt) {
            pos = b;
            break;
        }
    }
    if (pos == 0xFFFF)
        return; /* probe limit exceeded, not found */

    /* Backward-shift deletion */
    while (1) {
        u16 next = (pos + 1) & mask;
        u16 ns;

        if (idx->buckets[next] == MTS_HASH_EMPTY)
            break;

        ns = idx->buckets[next];
        {
            u32 ideal = mts_hash_key(mc[ns].cs.code, (u32)(u16)mc[ns].state)
                        >> (32 - bits);
            ideal &= mask;
            /*
             * Check if 'next' entry's ideal position is at or before 'pos'
             * (in circular sense). If so, shift it back.
             */
            if (((next - ideal) & mask) > ((next - pos) & mask)) {
                /* This entry would benefit from being at 'pos' */
            } else {
                break;
            }
        }
        idx->buckets[pos] = idx->buckets[next];
        pos = next;
    }
    idx->buckets[pos] = MTS_HASH_EMPTY;
}

static void mts_hash_clear(MtsCacheIndex* idx) {
    s32 i;
    for (i = 0; i < (s32)idx->bucket_count; i++) {
        idx->buckets[i] = MTS_HASH_EMPTY;
    }
}

static inline u16 mts_hash_bucket_count(u32 mltnum) {
    /* bucket count must be > mltnum for open addressing. Use ~2x headroom. */
    if (mltnum <= 1024) return MTS_HASH_BUCKETS_2K;
    return MTS_HASH_BUCKETS_4K;
}

/* --- MTS free-list helpers --- */

static inline s32 mts_freelist_pop(MtsFreeList* fl) {
    if (fl->top < 0)
        return -1;
    return (s32)fl->slots[fl->top--];
}

static inline void mts_freelist_push(MtsFreeList* fl, u16 slot) {
    fl->slots[++fl->top] = slot;
}
```

### Success criteria
- Build compiles cleanly (warnings about unused statics are acceptable).
- No behavioral change.

### Dependencies
Step 1 must be complete.

### Scope limits
- Do NOT call these functions from any existing code path.
- Do NOT modify any existing function.

### Fallback
If the compiler flags errors on the inline functions (e.g., C89 mode), remove `inline` and
use plain `static`. The ARM GCC cross-compiler is C99+ so this should not happen.

---

## Step 3: Allocate memory for hash tables and free lists

### Title
Extend `make_texcash_work` to allocate `MtsCacheIndex` and free-list arrays as part of the
ramcnt memory block, and initialize the `MultiTexture` pointer fields.

### Why
This wires up the memory so the hash tables and free lists exist at runtime. Still no
behavioral change -- the hash tables are allocated and cleared but not consulted during
lookups.

### Files to read
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/texcash.c` (lines 320-405,
  `make_texcash_work`)
- `/Users/sb/Developer/3sx-mister/include/structs.h` (new structs from Step 1)

### Files to modify
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/texcash.c`

### Changes

In `make_texcash_work`, increase `memreq` for `key0` allocation to include space for
the `MtsCacheIndex` structs, their bucket arrays, and the free-list arrays. Assign pointers.

Compute bucket counts early (add local variables at top of function):
```c
u16 bc16 = mts_hash_bucket_count(mts[ix].mltnum16);
u16 bc32 = mts_hash_bucket_count(mts[ix].mltnum32);
```

**For the ext path** (line ~360):
```c
// BEFORE (existing):
memreq = (mts[ix].mltnum16 * 8) + (mts[ix].mltnum32 * 8) + sizeof(PatternCollection) +
         sizeof(TexturePoolFree) + sizeof(TexturePoolUsed);

// AFTER:
memreq = (mts[ix].mltnum16 * 8) + (mts[ix].mltnum32 * 8) + sizeof(PatternCollection) +
         sizeof(TexturePoolFree) + sizeof(TexturePoolUsed) +
         sizeof(MtsCacheIndex) * 2 +
         (bc16 * sizeof(u16)) + (bc32 * sizeof(u16)) +
         (mts[ix].mltnum16 * sizeof(u16)) + (mts[ix].mltnum32 * sizeof(u16));
```

After assigning `mts[ix].tpu`, continue assigning the new pointers:
```c
adrs += sizeof(TexturePoolUsed);
mts[ix].hash16 = (MtsCacheIndex*)adrs;
adrs += sizeof(MtsCacheIndex);
mts[ix].hash32 = (MtsCacheIndex*)adrs;
adrs += sizeof(MtsCacheIndex);
mts[ix].hash16->buckets = (u16*)adrs;
mts[ix].hash16->bucket_count = bc16;
mts[ix].hash16->bucket_mask = bc16 - 1;
adrs += bc16 * sizeof(u16);
mts[ix].hash32->buckets = (u16*)adrs;
mts[ix].hash32->bucket_count = bc32;
mts[ix].hash32->bucket_mask = bc32 - 1;
adrs += bc32 * sizeof(u16);
mts[ix].free16.slots = (u16*)adrs;
adrs += mts[ix].mltnum16 * sizeof(u16);
mts[ix].free32.slots = (u16*)adrs;
mts_hash_clear(mts[ix].hash16);
mts_hash_clear(mts[ix].hash32);
mts[ix].free16.top = -1;
mts[ix].free32.top = -1;
```

**For the non-ext path** (line ~378):
```c
// BEFORE:
memreq = mts[ix].mltnum16 * 8 + mts[ix].mltnum32 * 8;

// AFTER:
memreq = mts[ix].mltnum16 * 8 + mts[ix].mltnum32 * 8 +
         sizeof(MtsCacheIndex) * 2 +
         (bc16 * sizeof(u16)) + (bc32 * sizeof(u16)) +
         (mts[ix].mltnum16 * sizeof(u16)) + (mts[ix].mltnum32 * sizeof(u16));
```

After assigning `mts[ix].mltcsh32`:
```c
adrs += mts[ix].mltnum32 * 8;
mts[ix].hash16 = (MtsCacheIndex*)adrs;
adrs += sizeof(MtsCacheIndex);
mts[ix].hash32 = (MtsCacheIndex*)adrs;
adrs += sizeof(MtsCacheIndex);
mts[ix].hash16->buckets = (u16*)adrs;
mts[ix].hash16->bucket_count = bc16;
mts[ix].hash16->bucket_mask = bc16 - 1;
adrs += bc16 * sizeof(u16);
mts[ix].hash32->buckets = (u16*)adrs;
mts[ix].hash32->bucket_count = bc32;
mts[ix].hash32->bucket_mask = bc32 - 1;
adrs += bc32 * sizeof(u16);
mts[ix].free16.slots = (u16*)adrs;
adrs += mts[ix].mltnum16 * sizeof(u16);
mts[ix].free32.slots = (u16*)adrs;
mts_hash_clear(mts[ix].hash16);
mts_hash_clear(mts[ix].hash32);
mts[ix].free16.top = -1;
mts[ix].free32.top = -1;
```

**Note**: The `mts_hash.h` header was already created in Step 2, so `texcash.c` just needs
`#include "mts_hash.h"` to access `mts_hash_clear` and the other helpers.

### Success criteria
- Build compiles cleanly.
- No behavioral change. Hash tables are allocated and zeroed but not consulted.
- Verify with telemetry that frame times are unchanged (the allocation adds ~12KB per cache).

### Dependencies
Steps 1 and 2 must be complete.

### Scope limits
- Do NOT modify any `get_mltbuf*` function.
- Do NOT modify `mlt_obj_trans_init` or `mlt_obj_trans_update`.
- Do NOT populate the free list yet (that happens in Step 4).

### Fallback
If ramcnt allocation fails at runtime (the game halts with "TEXCASH KEY ERROR"), the extra
memory is too large. Reduce by using `MTS_HASH_BUCKETS_2K` unconditionally and skipping hash
allocation for caches where `mltnum > 2048` (only DM x32 = 2176). Set `hash32 = NULL` for
those caches; the lookup functions will fall back to linear scan when the pointer is NULL.
DM x32 is the demo-mode background cache, which is less performance-critical than character
sprites.

---

## Step 4: Populate hash tables and free lists during init/clear

### Title
Wire up `mlt_obj_trans_init`, `clear_texcash_work`, and `init_texcash_2nd` to populate the
hash index and free-list stack on initialization and clear.

### Why
After this step, the hash tables and free lists are in sync with the `PatternState` arrays
at all initialization and clear points. Still no behavioral change -- the data structures
are maintained but not consulted during lookups.

### Files to read
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c` (lines 2077-2149,
  `mlt_obj_trans_init` and `mlt_obj_trans_update`)
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/texcash.c` (lines 190-236,
  `init_texcash_2nd`; lines 415-440, `clear_texcash_work`)

### Files to modify
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c`
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/texcash.c`

### Changes

**`mlt_obj_trans_init`** (mtrans.c line 2108-2124): The existing `if (!(mode & 0x20))` block
initializes PatternState arrays to code=-1/time=0. That block is unchanged. The hash/free-list
init is placed **after** (outside) the `if (!(mode & 0x20))` block so it runs for ALL caches,
including persist caches like DM (index 9, mode 0x1021, has mode & 0x20 set).

**Rationale**: DM's PatternState arrays are intentionally not cleared on re-init (persist
mode), but its hash table and free list must still be populated to match the current array
contents. On first init, the ramcnt memory is fresh and all slots are empty (code will not
match any valid lookup). On re-init, the slots may contain live entries from a previous round.
Either way, the hash/free-list must be rebuilt from the actual array contents.

```c
    if (!(mode & 0x20)) {
        mc = mt->mltcsh16;
        for (i = 0; i < mt->mltnum16; i++) {
            mc->time = 0;
            mc->cs.code = -1;
            mc++;
        }
        mc = mt->mltcsh32;
        for (i = 0; i < mt->mltnum32; i++) {
            mc->time = 0;
            mc->cs.code = -1;
            mc++;
        }
    }

    /* --- NEW: clear hash tables and populate free lists --- */
    /* This runs for ALL caches, including persist (mode & 0x20) caches like DM.
     * For non-persist caches, all slots are code=-1 so all go to free list.
     * For persist caches, we scan the array to find live vs free slots. */
    if (mt->hash16) {
        mts_hash_clear(mt->hash16);
        mt->free16.top = -1;
        mc = mt->mltcsh16;
        for (i = 0; i < mt->mltnum16; i++) {
            if (mc[i].cs.code == (u32)-1) {
                mts_freelist_push(&mt->free16, (u16)i);
            } else {
                mts_hash_insert(mt->hash16, mc[i].cs.code,
                                (u32)(u16)mc[i].state, (u16)i);
            }
        }
    }
    if (mt->hash32) {
        mts_hash_clear(mt->hash32);
        mt->free32.top = -1;
        mc = mt->mltcsh32;
        for (i = 0; i < mt->mltnum32; i++) {
            if (mc[i].cs.code == (u32)-1) {
                mts_freelist_push(&mt->free32, (u16)i);
            } else {
                mts_hash_insert(mt->hash32, mc[i].cs.code,
                                (u32)(u16)mc[i].state, (u16)i);
            }
        }
    }
```

Note: For non-persist caches that were just zeroed above, every slot has code=-1 so all
slots go to the free list and the hash stays empty. The scan-based approach handles both
persist and non-persist caches uniformly.

**Performance note**: The scan adds O(N) work at init time, but init runs once per scene
load, not per frame. Even for DM's 2176-entry x32 cache, this is negligible.

The free list is ordered so that `pop` returns low indices first (slot 0, then 1, ...).
This matches the original linear scan's preference to fill the first free slot.

**`clear_texcash_work`** (texcash.c line 415-440): This function is already guarded by
`(mts_base[ix].mode & 0x20) == 0`, so persist caches (DM) never enter it. After setting
all slots to `code = -1`, also clear hash and rebuild free list. This is correct because
persist caches get their hash/freelist rebuilt in `mlt_obj_trans_init` instead (see above):

```c
    // After the existing loops that clear mltcsh16 and mltcsh32:
    if (mts[ix].hash16) {
        mts_hash_clear(mts[ix].hash16);
        mts[ix].free16.top = mts[ix].mltnum16 - 1;
        for (i = 0; i < mts[ix].mltnum16; i++)
            mts[ix].free16.slots[i] = (u16)(mts[ix].mltnum16 - 1 - i);
    }
    if (mts[ix].hash32) {
        mts_hash_clear(mts[ix].hash32);
        mts[ix].free32.top = mts[ix].mltnum32 - 1;
        for (i = 0; i < mts[ix].mltnum32; i++)
            mts[ix].free32.slots[i] = (u16)(mts[ix].mltnum32 - 1 - i);
    }
```

**`init_texcash_2nd`** (texcash.c line 190-236): This rebuilds the tpf/tpu lists for ext
caches by scanning for free vs. occupied slots. After the existing rebuild, also rebuild
the hash table from occupied slots and the free list from free slots:

```c
    // After the existing tpf/tpu rebuild loops, before the cpat rebuild:
    if (mts[ix].hash16) {
        mts_hash_clear(mts[ix].hash16);
        mts[ix].free16.top = -1;
        mc = mts[ix].mltcsh16;
        for (i = 0; i < mts[ix].mltnum16; i++) {
            if (mc[i].cs.code == (u32)-1) {
                mts_freelist_push(&mts[ix].free16, (u16)i);
            } else {
                mts_hash_insert(mts[ix].hash16, mc[i].cs.code,
                                (u32)(u16)mc[i].state, (u16)i);
            }
        }
    }
    if (mts[ix].hash32) {
        mts_hash_clear(mts[ix].hash32);
        mts[ix].free32.top = -1;
        mc = mts[ix].mltcsh32;
        for (i = 0; i < mts[ix].mltnum32; i++) {
            if (mc[i].cs.code == (u32)-1) {
                mts_freelist_push(&mts[ix].free32, (u16)i);
            } else {
                mts_hash_insert(mts[ix].hash32, mc[i].cs.code,
                                (u32)(u16)mc[i].state, (u16)i);
            }
        }
    }
```

### Success criteria
- `tools/mister/build-game.sh --flavor telemetry --jobs 4` exits with code 0.
- No behavioral change. Game launches, reaches character select, and plays one round without
  crash.

### Dependencies
Steps 1, 2, 3 must be complete.

### Scope limits
- Do NOT modify any `get_mltbuf*` function.
- Do NOT modify `mlt_obj_trans_update`.

### Fallback
If the game crashes during init, add `SDL_Log` calls to verify the hash/free-list population
counts match the expected slot counts. The most likely issue is a NULL pointer (hash16/hash32
not allocated for a particular cache) -- guard all accesses with NULL checks.

---

## Step 5: Replace `get_mltbuf16` and `get_mltbuf32` with hash lookup

### Title
Modify `get_mltbuf16` and `get_mltbuf32` to use `mts_hash_lookup` for cache hits, and
`mts_freelist_pop` + `mts_hash_insert` for cache misses. Fall back to original linear
scan if hash is NULL or probe limit exceeded.

### Why
This is the core performance win. These two functions handle all non-ext tile lookups
(character sprites, effects, UI). Eliminating their O(N) scan directly reduces per-frame
overhead.

### Files to read
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c` (lines 1709-1783,
  `get_mltbuf16` and `get_mltbuf32`)
- The hash helpers from Step 2 (in `mts_hash.h` or top of `mtrans.c`)

### Files to modify
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c`

### Changes

Replace `get_mltbuf16` with:

```c
static s32 get_mltbuf16(MultiTexture* mt, u32 code, u32 palt, s32* ret) {
    MtsCacheIndex* idx = mt->hash16;
    MtsFreeList* fl = &mt->free16;

    /* Fast path: hash lookup */
    if (idx) {
        s32 slot = mts_hash_lookup(idx, code, palt, mt->mltcsh16);
        if (slot >= 0) {
            mt->mltcsh16[slot].time = mt->mltcshtime16;
            *ret = slot;
            return 0;
        }

        /* Cache miss: allocate from free list */
        slot = mts_freelist_pop(fl);
        if (slot >= 0) {
            mt->mltcsh16[slot].time = mt->mltcshtime16;
            mt->mltcsh16[slot].state = palt;
            mt->mltcsh16[slot].cs.code = code;
            mts_hash_insert(idx, code, palt, (u16)slot);
            *ret = slot;
            return 1;
        }

        /* Free list empty = cache full, fall through to fatal */
    }

    /* Fallback: original linear scan (safety net, or hash not allocated) */
    {
        s32 i;
        s32 b = -1;
        PatternState* mc = mt->mltcsh16;

        i = mt->mltnum16;
        while (1) {
            if ((mc->cs.code == code) && (mc->state == palt)) {
                mc->time = mt->mltcshtime16;
                *ret = mt->mltnum16 - i;
                /* Sync hash: try to insert this hit so future lookups use fast path */
                if (idx)
                    mts_hash_insert(idx, code, palt, (u16)*ret);
                return 0;
            }
            if ((mc->cs.code == -1) && (b < 0)) {
                b = i;
            }
            mc++;
            i -= 1;
            if (i <= 0) {
                if (b >= 0) {
                    b = mt->mltnum16 - b;
                    mt->mltcsh16[b].time = mt->mltcshtime16;
                    mt->mltcsh16[b].state = palt;
                    mt->mltcsh16[b].cs.code = code;
                    *ret = b;
                    /* Sync hash: insert newly allocated entry */
                    if (idx)
                        mts_hash_insert(idx, code, palt, (u16)b);
                    return 1;
                }
                flLogOut("\xef\xbc\xa3\xef\xbc\xa7\xE3\x82\xAD\xE3\x83\xA3\xE3\x83\x83\xE3\x82\xB7\xE3\x83\xA5\xE3\x81\x8C\xE4\xB8\x80\xE6\x9D\xAF\xE3\x81\xAB\xE3\x81\xAA\xE3\x82\x8A\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F\xE3\x80\x82\xEF\xBC\x91\xEF\xBC\x96\xC3\x97\xEF\xBC\x91\xEF\xBC\x96 : %d\n", mt->id);
                while (1) {}
            }
        }
    }
}
```

Apply the same pattern to `get_mltbuf32`, substituting:
- `mt->hash32` / `mt->free32` / `mt->mltcsh32`
- `mt->mltcshtime32` / `mt->mltnum32`
- `code >> 6` / `code & 0x3F` (these are in the caller, not this function)
- The fallback path MUST include the hash sync calls (`mts_hash_insert` on both hit and
  miss), same as the x16 version above.

**Critical invariant**: The return value semantics MUST be identical:
- Return `0` = cache hit (tile already decoded), `*ret` = slot index.
- Return `1` = cache miss (tile needs decoding), `*ret` = slot index.
- The slot index is used by callers as `code >> 8` for texture group and `code & 0xFF` for
  sub-index within group. This is unchanged because we return the same flat-array index.

### Success criteria
- `tools/mister/build-game.sh --flavor telemetry --jobs 4` exits with code 0.
- Game launches on MiSTer, plays Remy vs Remy on Cafe Dejavu for 2+ rounds without crash,
  hang, or visual corruption. Also test DM (demo mode) by letting the attract loop run.
- **Performance test**: Frame time in Remy stage (Cafe Dejavu) should drop measurably (expect
  1-3ms improvement). Measure via telemetry overlay over 300+ frames.

### Dependencies
Steps 1-4 must be complete.

### Scope limits
- Do NOT modify `get_mltbuf16_ext`, `get_mltbuf16_ext_2`, `get_mltbuf32_ext`, or
  `get_mltbuf32_ext_2` yet.
- Do NOT modify `mlt_obj_trans_update` yet.

### Fallback
If rendering is visually corrupted:
1. Check that the fallback linear scan path still works by temporarily setting `idx = NULL`
   at the top of the function.
2. Check for off-by-one errors in the free list (top initialized to `mltnum - 1` means the
   first pop returns slot `0`, which is correct).
3. Add an assertion that `mts_hash_lookup` after `mts_hash_insert` returns the same slot.

If the hash probe limit is hit frequently (game slows down or uses the fallback path), increase
`MTS_HASH_PROBE_LIMIT` from 16 to 32, or switch to a different hash function.

---

## Step 6: Wire up eviction in `mlt_obj_trans_update`

### Title
Modify `mlt_obj_trans_update` to remove expired entries from the hash table and push their
slots to the free list.

### Why
Without this, evicted slots (TTL expired, `code` set to `-1`) remain in the hash table as
stale entries, and their slot indices are never returned to the free list. This would cause
the free list to drain to zero and fall back to linear scan.

### Files to read
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c` (lines 2127-2149,
  `mlt_obj_trans_update`)

### Files to modify
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c`

### Changes

Replace `mlt_obj_trans_update`:

```c
void mlt_obj_trans_update(MultiTexture* mt) {
    s32 i;
    PatternState* mc;

    PatternState* assign1;
    PatternState* assign2;

    for (mc = mt->mltcsh16, i = 0; i < mt->mltnum16; i++, mc += 1, assign1 = mc) {
        if (mc->time) {
            if (--mc->time == 0) {
                /* Remove from hash before invalidating */
                if (mt->hash16) {
                    mts_hash_remove(mt->hash16, mc->cs.code,
                                    (u32)(u16)mc->state, mt->mltcsh16);
                    mts_freelist_push(&mt->free16, (u16)i);
                }
                mc->cs.code = -1;
            }
        }
    }

    for (mc = mt->mltcsh32, i = 0; i < mt->mltnum32; i++, mc += 1, assign2 = mc) {
        if (mc->time) {
            if (--mc->time == 0) {
                /* Remove from hash before invalidating */
                if (mt->hash32) {
                    mts_hash_remove(mt->hash32, mc->cs.code,
                                    (u32)(u16)mc->state, mt->mltcsh32);
                    mts_freelist_push(&mt->free32, (u16)i);
                }
                mc->cs.code = -1U;
            }
        }
    }
}
```

Note the existing code uses `-1` for x16 and `-1U` for x32. Preserve this exactly.

### Success criteria
- Build compiles cleanly.
- Game runs for extended sessions (5+ minutes) without cache-full errors.
- The free list count should stabilize (entries are returned as TTL expires and reused on
  new tile lookups).
- Performance improvement from Step 5 is sustained over time (not just for the first few
  frames before the free list drains).

### Dependencies
Steps 1-5 must be complete.

### Scope limits
- Do NOT modify the ext variants yet.
- Do NOT modify `update_with_tpu_free` yet.

### Fallback
If the game hangs with "cache full" after running for a while, the free list is not being
replenished. Debug by logging `mt->free16.top` in `mlt_obj_trans_update` to verify push
operations are happening. Check that `mts_hash_remove` is correctly finding and deleting
entries (the backward-shift deletion is the trickiest part -- if suspected, temporarily
replace with a simpler tombstone approach).

---

## Step 7: Replace ext_2 variants with hash lookup

### Title
Modify `get_mltbuf16_ext_2` and `get_mltbuf32_ext_2` to use hash lookup for the hit path
while preserving the `tpu->x16_used[]` bookkeeping and `x16_mapping_set` calls.

### Why
The ext_2 path handles per-pattern tile lookups in the ext (extended) cache mode. It currently
scans `tpu->x16_used[]` linearly. Replacing the scan with a hash lookup completes the
optimization for character sprites in ext mode.

### Files to read
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c` (lines 1785-1858,
  `get_mltbuf16_ext_2` and `get_mltbuf32_ext_2`)

### Files to modify
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c`

### Changes

Replace `get_mltbuf16_ext_2`:

```c
static s32 get_mltbuf16_ext_2(MultiTexture* mt, u32 code, u32 palt, s32* ret, PatternInstance* cp) {
    PatternState* mc = mt->mltcsh16;

    /* Fast path: hash lookup */
    if (mt->hash16) {
        s32 slot = mts_hash_lookup(mt->hash16, code, palt, mc);
        if (slot >= 0) {
            *ret = slot;
            if (x16_mapping_set(&cp->map, *ret)) {
                cp->x16 += 1;
                mc[slot].time += 1;
            }
            return 0;
        }

        /* Cache miss: allocate from tpf free pool (ext uses tpf, not our free list) */
        if (mt->tpf->x16 != 0) {
            s32 i = mt->tpu->x16;
            mt->tpf->x16 -= 1;
            mt->tpu->x16_used[i] = mt->tpf->x16_free[mt->tpf->x16];
            mt->tpu->x16 += 1;
            mc[mt->tpu->x16_used[i]].cs.code = code;
            mc[mt->tpu->x16_used[i]].state = palt;
            *ret = mt->tpu->x16_used[i];
            mc[mt->tpu->x16_used[i]].time = 1;
            mts_hash_insert(mt->hash16, code, palt, (u16)*ret);
            if (x16_mapping_set(&cp->map, *ret)) {
                cp->x16 += 1;
            }
            return 1;
        }

        /* Fall through to fatal */
    }

    /* Fallback: original linear scan (sync hash on hit/miss to avoid future fallbacks) */
    {
        s32 i;
        for (i = 0; i < mt->tpu->x16; i++) {
            if ((code == mc[mt->tpu->x16_used[i]].cs.code) && (palt == mc[mt->tpu->x16_used[i]].state)) {
                *ret = mt->tpu->x16_used[i];
                if (x16_mapping_set(&cp->map, *ret)) {
                    cp->x16 += 1;
                    mc[mt->tpu->x16_used[i]].time += 1;
                }
                /* Sync hash: insert so future lookups use fast path */
                if (mt->hash16)
                    mts_hash_insert(mt->hash16, code, palt, (u16)*ret);
                return 0;
            }
        }
        if ((i != mt->mltnum16) && (mt->tpf->x16 != 0)) {
            mt->tpf->x16 -= 1;
            mt->tpu->x16_used[i] = mt->tpf->x16_free[mt->tpf->x16];
            mt->tpu->x16 += 1;
            mc[mt->tpu->x16_used[i]].cs.code = code;
            mc[mt->tpu->x16_used[i]].state = palt;
            *ret = mt->tpu->x16_used[i];
            mc[mt->tpu->x16_used[i]].time = 1;
            /* Sync hash: insert newly allocated entry */
            if (mt->hash16)
                mts_hash_insert(mt->hash16, code, palt, (u16)*ret);
            if (x16_mapping_set(&cp->map, *ret)) {
                cp->x16 += 1;
            }
            return 1;
        }
        flLogOut("\xef\xbc\xa3\xef\xbc\xa7\xE3\x82\xAD\xE3\x83\xA3\xE3\x83\x83\xE3\x82\xB7\xE3\x83\xA5\xE3\x81\x8C\xE4\xB8\x80\xE6\x9D\xAF\xE3\x81\xAB\xE3\x81\xAA\xE3\x82\x8A\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F\xE3\x80\x82\xC3\x97\xEF\xBC\x91\xEF\xBC\x96\xE3\x80\x80\xEF\xBC\xA5\xEF\xBC\xB8\xEF\xBC\xB4\xEF\xBC\x92\n");
        while (1) {}
    }
}
```

Apply the same pattern to `get_mltbuf32_ext_2`, substituting x32 for x16 throughout, **with
one critical difference**: The time assignment on cache miss (allocation path) is different
between x16 and x32.

- **x16 ext_2 miss path**: `mc[slot].time = 1;` (set to 1)
- **x32 ext_2 miss path**: `mc[slot].time += 1;` (increment, NOT set)

This is how the original code works (mtrans.c line 1809 vs 1847). The x32 variant uses `+= 1`
on both the hit and miss paths, while x16 uses `+= 1` on hit but `= 1` on miss. The hash
fast path for x32 must preserve this: use `time += 1` in the miss allocation block, not
`time = 1`. In the x16 fast path (shown above), `time = 1` is correct.

**Critical note**: The ext path uses `tpf` (TexturePoolFree) and `tpu` (TexturePoolUsed) for
slot allocation, NOT our `MtsFreeList`. The hash table is only used as a lookup accelerator.
The ext path's free/used tracking MUST remain unchanged.

**Fallback hash sync**: The fallback linear scan in both x16 and x32 ext_2 variants must
include `mts_hash_insert` calls (guarded by `if (mt->hash16)` / `if (mt->hash32)`) on both
hit and miss paths, same as the non-ext fallbacks in Step 5. This ensures the hash stays
in sync when the fallback fires due to probe limit being exceeded.

### Success criteria
- Build compiles cleanly.
- Game renders correctly in all ext-mode scenarios (player characters with complex animations).
- No "cache full" errors.

### Dependencies
Steps 1-6 must be complete.

### Scope limits
- Do NOT modify `get_mltbuf16_ext` or `get_mltbuf32_ext` yet (those are the simpler
  read-only lookup variants used during update_with_tpu_free).
- Do NOT modify `update_with_tpu_free`.

### Fallback
If ext rendering breaks, the `tpu` bookkeeping may be out of sync with the hash table.
Temporarily disable the hash fast path by setting `mt->hash16 = NULL` in the ext allocation
path of `make_texcash_work` to confirm the fallback works. Then add logging to compare hash
lookup results with linear scan results.

---

## Step 8: Replace ext (read-only) variants with hash lookup

### Title
Modify `get_mltbuf16_ext` and `get_mltbuf32_ext` to use hash lookup.

### Why
These are simpler read-only lookup functions used in the `update_with_tpu_free` eviction
path. They scan `tpu_free->x16_used[]` (note: `tpu_free` is the global, not `mt->tpu`).
Replacing them completes the hash optimization for all tile lookup paths.

### Files to read
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c` (lines 1860-1886,
  `get_mltbuf16_ext` and `get_mltbuf32_ext`)
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/texcash.c` (lines 276-310,
  `update_with_tpu_free` -- the caller)

### Files to modify
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c`

### Changes

Replace `get_mltbuf16_ext`:

```c
static s32 get_mltbuf16_ext(MultiTexture* mt, u32 code, u32 palt) {
    PatternState* mc = mt->mltcsh16;

    /* Fast path: hash lookup */
    if (mt->hash16) {
        s32 slot = mts_hash_lookup(mt->hash16, code, palt, mc);
        if (slot >= 0)
            return slot;
        /* Hash miss but entry must exist -- fall through to linear scan */
    }

    /* Fallback: original linear scan */
    {
        s32 i;
        for (i = 0; i < tpu_free->x16; i++) {
            if ((code == mc[tpu_free->x16_used[i]].cs.code) && (palt == mc[tpu_free->x16_used[i]].state)) {
                return tpu_free->x16_used[i];
            }
        }
        flLogOut("\xef\xbc\xa3\xef\xbc\xa7\xE5\xB1\x95\xE9\x96\x8B\xE3\x82\xA8\xE3\x83\xA9\xE3\x83\xBC\xE3\x80\x80\xEF\xBC\x91\xEF\xBC\x96\xC3\x97\xEF\xBC\x91\xEF\xBC\x96\n");
        while (1) {}
    }
}
```

Same pattern for `get_mltbuf32_ext`.

### Success criteria
- Build compiles cleanly.
- Game runs correctly during pattern collection eviction (this is triggered by TTL expiry
  in ext mode via `texture_cash_update` -> `makeup_tpu_free` -> `update_with_tpu_free`).

### Dependencies
Steps 1-7 must be complete.

### Scope limits
- Do NOT modify `update_with_tpu_free` itself.
- Do NOT modify `makeup_tpu_free`.

### Fallback
These functions are called much less frequently than the others (only during ext eviction),
so the performance impact is small. If they cause issues, simply remove the hash fast path
and keep the linear scan. The overall performance win from Steps 5-7 is preserved.

---

## Step 9: Wire up `update_with_tpu_free` eviction to hash removal

### Title
Modify `update_with_tpu_free` in texcash.c to remove evicted entries from the hash table
and push their slots to the free list.

### Why
`update_with_tpu_free` is the ext-mode eviction path. It decrements time on specific slots
and sets `code = -1` when time reaches zero. Without hash removal, evicted ext slots become
stale hash entries. Without free-list push, those slots are never reused via the hash path.

### Files to read
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/texcash.c` (lines 276-310,
  `update_with_tpu_free`)
- The caller in `texture_cash_update` (lines 238-273) to understand which `mts[num]` is
  active when this is called.

### Files to modify
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/texcash.c`

### Changes

The challenge is that `update_with_tpu_free` receives raw `PatternState*` pointers, not the
`MultiTexture*` that contains the hash tables. We need to change the signature, or find the
parent `MultiTexture` from context.

**Option A (preferred)**: Change signature to pass `MultiTexture*`:
```c
void update_with_tpu_free(MultiTexture* mt);
```
And update the body to use `mt->mltcsh16`, `mt->mltcsh32`, `mt->hash16`, `mt->hash32`.

The single call site in `texture_cash_update` (line 259):
```c
// BEFORE:
update_with_tpu_free(mts[num].mltcsh16, mts[num].mltcsh32);
// AFTER:
update_with_tpu_free(&mts[num]);
```

Update the declaration in `texcash.h` accordingly.

**Modified `update_with_tpu_free`**:
```c
void update_with_tpu_free(MultiTexture* mt) {
    PatternState* mc16 = mt->mltcsh16;
    PatternState* mc32 = mt->mltcsh32;
    s16 i;

    for (i = 0; i < tpu_free->x16; i++) {
        u16 slot = tpu_free->x16_used[i];
        mc16[slot].time -= 1;
        if (mc16[slot].time < 0) {
            Debug_w[11] = 1;
            do {
                disp_texcash_free_area();
                flPrintL(2, 3, "CACHE MISS x16 : %3d", slot);
                njWaitVSync_with_N();
            } while (1);
        }
        if (mc16[slot].time <= 0) {
            if (mt->hash16) {
                mts_hash_remove(mt->hash16, mc16[slot].cs.code,
                                (u32)(u16)mc16[slot].state, mc16);
            }
            mc16[slot].cs.code = -1;
        }
    }

    for (i = 0; i < tpu_free->x32; i++) {
        u16 slot = tpu_free->x32_used[i];
        mc32[slot].time -= 1;
        if (mc32[slot].time < 0) {
            Debug_w[11] = 1;
            do {
                disp_texcash_free_area();
                flPrintL(2, 3, "CACHE MISS x32 : %3d", slot);
                njWaitVSync_with_N();
            } while (1);
        }
        if (mc32[slot].time <= 0) {
            if (mt->hash32) {
                mts_hash_remove(mt->hash32, mc32[slot].cs.code,
                                (u32)(u16)mc32[slot].state, mc32);
            }
            mc32[slot].cs.code = -1;
        }
    }
}
```

Note: We do NOT push to `free16`/`free32` here because the ext path uses `tpf`/`tpu` for
slot tracking, not our `MtsFreeList`. The `init_texcash_2nd` call that follows eviction will
rebuild the tpf/tpu lists (and our hash table, per Step 4). The hash removal here prevents
stale entries from causing false hits in the interval between eviction and the next
`init_texcash_2nd` call.

**Ordering safety note**: Between `update_with_tpu_free` eviction and the next frame's
`init_texcash_before_process` call, freed slots have `code = -1` and are removed from
the hash table but not yet in any free pool. During this interval, no ext_2 lookups occur
because `texture_cash_update` runs at end-of-frame after all rendering is complete, and
`init_texcash_before_process` runs at start-of-next-frame before any rendering. The freed
slots are correctly picked up by `init_texcash_2nd`'s scan at the start of the next frame.
This ordering is safe.

### Success criteria
- Build compiles cleanly.
- Game runs correctly during extended play sessions with ext-mode characters.
- No "CACHE MISS" or "MAPPING MISS" errors appear.

### Dependencies
Steps 1-8 must be complete.

### Scope limits
- Do NOT modify `makeup_tpu_free` or `texture_cash_update` beyond the call-site change.
- Do NOT change the `tpf`/`tpu` bookkeeping logic.

### Fallback
If changing the `update_with_tpu_free` signature causes issues elsewhere, use Option B: keep
the original signature and pass the `MultiTexture*` via a file-scope variable set before the
call in `texture_cash_update`. Less clean but zero-risk to other callers.

---

## Step 10: Cleanup and validation

### Title
Remove fallback linear scan code paths (or gate behind `#ifdef MTS_HASH_FALLBACK`),
add telemetry counters, final performance validation.

### Why
Once the hash path is validated, the fallback linear scan is dead code that adds maintenance
burden. Gating it behind a compile flag keeps it available for debugging while removing it
from the hot path.

### Files to read
- All modified files from Steps 1-9.
- Telemetry output from test runs to verify performance numbers.

### Files to modify
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/mtrans.c`
- `/Users/sb/Developer/3sx-mister/src/sf33rd/Source/Game/rendering/texcash.c`

### Changes

1. **Add telemetry** (under `#if ENABLE_PERF_TELEMETRY`): count hash hits, hash misses,
   free-list pops, free-list pushes, fallback-to-linear-scan events per frame. Add these
   to the existing telemetry overlay.

2. **Gate fallback paths**: Wrap the linear scan fallbacks in `#ifdef MTS_HASH_FALLBACK`.
   In the non-fallback path, replace them with a fatal error (the existing "cache full"
   infinite loop).

3. **Remove unused variables**: The `assign1`/`assign2` variables in `mlt_obj_trans_update`
   are already unused (they exist to match the original decompiled output). Consider whether
   to clean them up or leave them.

4. **Verify no regressions**: Test all character select screens, all stages (especially
   Remy's Cafe Dejavu and stages with complex backgrounds), training mode, and VS mode.

### Success criteria
- **Build**: `tools/mister/build-game.sh --flavor telemetry --jobs 4` exits with code 0,
  zero new warnings.
- **Smoke test**: Game launches on MiSTer, reaches character select, and completes one full
  round (2 rounds of play) without crash or hang. Use training mode with Remy on Cafe Dejavu.
- **Stability test**: Run 5 minutes of continuous play (or demo mode loop) without any
  "cache full", "CACHE MISS", or "MAPPING MISS" fatal errors.
- **Performance**: Remy stage (Cafe Dejavu) mean frame time drops by 1-3ms compared to
  pre-optimization baseline, measured via telemetry overlay over 300+ frames.
- **Telemetry**: Zero fallback-to-linear-scan events during normal gameplay. If any occur,
  log which cache index and key pattern triggered them before proceeding.

### Dependencies
Steps 1-9 must be complete.

### Scope limits
- Do NOT remove the fallback paths entirely -- keep them behind `#ifdef` for debugging.
- Do NOT change any rendering logic beyond the cache lookup optimization.

### Fallback
If telemetry shows non-zero fallback events, investigate which cache and key pattern causes
probe-limit overflow. Solutions: increase `MTS_HASH_PROBE_LIMIT`, increase table size, or
use a better hash function (e.g., add a finalizer mixing step).

---

## Appendix: Cache size reference

From `mts_base[24]` and the `mltnum = p << 8` / `p << 6` formulas:

| Index | Name | p16 | mltnum16 | p32 | mltnum32 | ext | TTL16 | TTL32 |
|-------|------|-----|----------|-----|----------|-----|-------|-------|
| 1     | QA   | 1   | 256      | 1   | 64       | no  | 0     | 0     |
| 2     | HT   | 2   | 512      | 4   | 256      | no  | 8     | 8     |
| 3     | 1P   | 3   | 768      | 6   | 384      | yes | 20    | 20    |
| 4     | 2P   | 3   | 768      | 6   | 384      | yes | 20    | 20    |
| 5     | CA   | 1   | 256      | 4   | 256      | yes | 2     | 2     |
| 6     | SW   | 1   | 256      | 5   | 320      | no  | 0     | 0     |
| 7     | OB   | var | var      | var | var      | yes | 12    | 12    |
| 8     | ED   | 2   | 512      | 8   | 512      | no  | 16    | 16    |
| 9     | DM   | 4   | 1024     | 34  | 2176     | no  | 20    | 20    |
| 10    | OT   | 4   | 1024     | 8   | 512      | no  | 4     | 4     |

**Hash table sizing**: Bucket counts are chosen per cache at init time:
- mltnum <= 1024: 2048 buckets (load factor <= 0.50)
- 1024 < mltnum <= 2048: 4096 buckets (load factor <= 0.50)
- mltnum = 2176 (DM x32, index 9): 4096 buckets (load factor ~0.53)

Note: DM (index 9) has mode=0x1021 (mode & 0x20 = persist). Its PatternState arrays are
not cleared between rounds. The hash/free-list is rebuilt by scanning the array contents
in `mlt_obj_trans_init`.

## Appendix: Key composition and collision analysis

The key is `(code: u32, palt: u32)`. In practice:
- `code` is a `PatternCode` union: `{offset: u16, group: u16}`. The group is the texture
  group index; offset is the tile index within the group.
- `palt` is the palette index (0 for indexed-color modes, 0-511 for direct-color modes).
  Most calls pass `palt = 0`.

The hash `code ^ (palt << 16)` ensures that when `palt = 0`, the hash is purely based on
`code`. Since `code` already has good entropy across both halves (group varies 0-80+,
offset varies 0-2000+), Knuth multiplicative hashing should distribute well.

## Appendix: Memory overhead

Per cache instance:
- `MtsCacheIndex` struct for x16: ~8 bytes (pointer + u16 + u16)
- `MtsCacheIndex` struct for x32: ~8 bytes
- Bucket array for x16: 4096 or 8192 bytes (2048 or 4096 u16 buckets)
- Bucket array for x32: 4096 or 8192 bytes
- Free-list for x16: `mltnum16 * 2` bytes (e.g., 2048 for mltnum16=1024)
- Free-list for x32: `mltnum32 * 2` bytes (e.g., 4352 for mltnum32=2176)

Worst case per cache (DM, index 9): 8192 + 8192 + 2048 + 4352 + 16 = ~14.8KB.
Typical cache: 4096 + 4096 + ~1024 + ~1024 + 16 = ~10.2KB.
Total overhead across all active caches (~10): ~100-150KB. The MiSTer DE10-Nano has 1GB
DDR3, so this is negligible.
