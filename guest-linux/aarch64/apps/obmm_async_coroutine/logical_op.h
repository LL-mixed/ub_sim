/* SPDX-License-Identifier: MIT */
#ifndef OBMM_ASYNC_LOGICAL_OP_H
#define OBMM_ASYNC_LOGICAL_OP_H

#include <stdint.h>

static inline uint64_t obmm_logical_ordinal(uint32_t coroutine_id,
                                            uint64_t local_ordinal)
{
    return (uint64_t)coroutine_id << 32 |
        (local_ordinal & UINT32_MAX);
}

static inline uint64_t obmm_logical_remote_ordinal(
    uint32_t coroutine_id, uint64_t local_ordinal,
    uint32_t coroutine_count, int mixed_pattern)
{
    uint64_t remote_local_ordinal = mixed_pattern ?
        local_ordinal / 2 : local_ordinal;

    return remote_local_ordinal * coroutine_count + coroutine_id;
}

static inline uint64_t obmm_logical_splitmix64(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

static inline uint64_t obmm_logical_gcd(uint64_t left, uint64_t right)
{
    while (right) {
        uint64_t remainder = left % right;

        left = right;
        right = remainder;
    }
    return left;
}

static inline uint64_t obmm_logical_page_index(uint64_t seed,
                                               uint64_t ordinal,
                                               uint64_t pages,
                                               int random_pattern)
{
    uint64_t cycle;
    uint64_t position;
    uint64_t multiplier;
    uint64_t addend;

    if (!pages) {
        return 0;
    }
    cycle = ordinal / pages;
    position = ordinal % pages;
    if (!random_pattern || pages == 1) {
        return position;
    }
    multiplier = obmm_logical_splitmix64(seed ^ cycle) % pages;
    if (!multiplier) {
        multiplier = 1;
    }
    while (obmm_logical_gcd(multiplier, pages) != 1) {
        multiplier++;
        if (multiplier == pages) {
            multiplier = 1;
        }
    }
    addend = obmm_logical_splitmix64(
        seed ^ cycle ^ 0xd1b54a32d192ed03ULL) % pages;
    return (position * multiplier + addend) % pages;
}

static inline uint64_t obmm_logical_worker_page(
    uint64_t seed, uint32_t worker_id, uint64_t local_ordinal,
    uint32_t worker_count, uint64_t pages, int random_pattern)
{
    uint64_t pages_per_worker;
    uint64_t local_page;

    if (!worker_count || worker_id >= worker_count ||
        pages < worker_count || pages % worker_count) {
        return pages;
    }
    pages_per_worker = pages / worker_count;
    local_page = obmm_logical_page_index(
        seed ^ ((uint64_t)worker_id << 32), local_ordinal,
        pages_per_worker, random_pattern);
    return local_page * worker_count + worker_id;
}

#endif
