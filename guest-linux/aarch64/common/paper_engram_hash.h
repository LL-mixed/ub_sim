/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Paper Engram hash helpers shared by guest-side tools.
 *
 * Keep this contract in sync with sim_models::engram_hash and the vendor
 * engram_simt/engram_common.h helper. Row hashing is intentionally lossy;
 * exact keys are length-prefixed and value-sensitive so they can be used as
 * tags alongside hashed rows.
 */

#ifndef PAPER_ENGRAM_HASH_H
#define PAPER_ENGRAM_HASH_H

#include <stdint.h>

#define PAPER_ENGRAM_FNV1A_OFFSET_BASIS UINT64_C(0xcbf29ce484222325)
#define PAPER_ENGRAM_FNV1A_PRIME        UINT64_C(0x100000001b3)
#define PAPER_ENGRAM_HASH_ALGORITHM_V1  "fnv1a-x64+length-prefix"

static inline uint64_t paper_engram_hash_word64(uint64_t value, uint64_t seed)
{
    uint64_t acc = PAPER_ENGRAM_FNV1A_OFFSET_BASIS ^ seed;
    int b;
    for (b = 0; b < 8; b++) {
        acc ^= (uint8_t)((value >> (8 * b)) & 0xffu);
        acc *= PAPER_ENGRAM_FNV1A_PRIME;
    }
    return acc;
}

static inline uint64_t paper_engram_exact_key_v1(const uint64_t *tokens,
                                                 int ngram_size)
{
    uint64_t acc = PAPER_ENGRAM_FNV1A_OFFSET_BASIS;
    uint64_t ngram_len = (uint64_t)ngram_size;
    int i;
    int b;

    for (b = 0; b < 8; b++) {
        acc ^= (uint8_t)((ngram_len >> (8 * b)) & 0xffu);
        acc *= PAPER_ENGRAM_FNV1A_PRIME;
    }
    for (i = 0; i < ngram_size; i++) {
        uint64_t token = tokens[i];
        for (b = 0; b < 8; b++) {
            acc ^= (uint8_t)((token >> (8 * b)) & 0xffu);
            acc *= PAPER_ENGRAM_FNV1A_PRIME;
        }
    }
    return acc;
}

static inline uint64_t paper_engram_row_hash_v1(uint32_t order,
                                                uint16_t head,
                                                const uint64_t *tokens,
                                                int ngram_size,
                                                uint64_t table_rows,
                                                uint64_t seed)
{
    uint64_t head_salt;
    uint64_t acc;
    int i;

    if (order == 0 || ngram_size == 0 || ngram_size != (int)order)
        return 0;
    if (table_rows == 0)
        return 0;

    head_salt = paper_engram_hash_word64((uint64_t)head, seed);
    acc = head_salt ^ (uint64_t)order;
    for (i = 0; i < ngram_size; i++) {
        acc ^= paper_engram_hash_word64(tokens[i], head_salt);
        head_salt = ((head_salt << 7) | (head_salt >> 57)) + tokens[i];
        acc *= PAPER_ENGRAM_FNV1A_PRIME;
    }
    return acc % table_rows;
}

#endif /* PAPER_ENGRAM_HASH_H */
