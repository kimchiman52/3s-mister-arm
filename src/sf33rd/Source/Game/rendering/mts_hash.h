/**
 * @file mts_hash.h
 * Static inline hash/free-list helpers for the MTS tile cache.
 */

#ifndef MTS_HASH_H
#define MTS_HASH_H

#include "structs.h"

/* --- MTS hash index invariants ---
 *
 * Three separate claims in this cluster are stated as comments and consumed
 * as bare literals, and all three are only true because the bucket counts are
 * powers of two:
 *
 *   - MtsCacheIndex::bucket_mask is documented as "(bucket_count - 1); either
 *     0x7FF (2K) or 0xFFF (4K)" and is used to fold a hash into a bucket. A
 *     mask is only a valid modulo for a power-of-two count; at any other size
 *     the fold silently stops reaching part of the table and lookups start
 *     missing entries that are present.
 *   - mts_hash_bits() answers 11 or 12 from bare literals rather than
 *     computing log2 of the count.
 *   - the mask values quoted in the struct comment are themselves derived.
 *
 * Pinned here rather than restated, so a retune of either bucket count has to
 * come past all three. */
_Static_assert((MTS_HASH_BUCKETS_2K & (MTS_HASH_BUCKETS_2K - 1)) == 0,
               "MTS_HASH_BUCKETS_2K must be a power of two — bucket_mask folds "
               "the hash with (count - 1), which is only a modulo for powers of two");
_Static_assert((MTS_HASH_BUCKETS_4K & (MTS_HASH_BUCKETS_4K - 1)) == 0,
               "MTS_HASH_BUCKETS_4K must be a power of two — bucket_mask folds "
               "the hash with (count - 1), which is only a modulo for powers of two");
_Static_assert(MTS_HASH_BUCKETS_2K == (1u << 11) && MTS_HASH_BUCKETS_4K == (1u << 12),
               "mts_hash_bits() returns the literals 11 and 12 for these two "
               "bucket counts; they must stay the counts' actual log2");
_Static_assert(MTS_HASH_BUCKETS_2K - 1 == 0x7FF && MTS_HASH_BUCKETS_4K - 1 == 0xFFF,
               "MtsCacheIndex::bucket_mask is documented as 0x7FF / 0xFFF");
/* bucket_count and bucket_mask are both u16; the larger table must still fit. */
_Static_assert(MTS_HASH_BUCKETS_4K <= 0xFFFF,
               "bucket_count is u16 — MTS_HASH_BUCKETS_4K must fit in it");
/* MTS_HASH_EMPTY is a sentinel stored in the same u16 as a slot index, so it
 * must not be a value any real slot index can take. */
_Static_assert(MTS_HASH_EMPTY == 0xFFFF,
               "MTS_HASH_EMPTY must stay the out-of-range u16 sentinel it is "
               "compared against throughout this header");

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
static inline s32 mts_hash_lookup(const MtsCacheIndex* idx, u32 code, u32 palt,
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
static inline s32 mts_hash_insert(MtsCacheIndex* idx, u32 code, u32 palt, u16 slot) {
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
static inline void mts_hash_remove(MtsCacheIndex* idx, u32 code, u32 palt,
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

static inline void mts_hash_clear(MtsCacheIndex* idx) {
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

#endif /* MTS_HASH_H */
