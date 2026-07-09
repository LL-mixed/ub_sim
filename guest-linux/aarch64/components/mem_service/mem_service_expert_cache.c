#include "mem_service_expert_cache.h"

#include <string.h>

/*
 * Node-side LRU expert cache (stage 2). Mirrors the Rust
 * ExpertCacheSimulator for guest-side use. The resident set is a small
 * ring of packed keys (layer*1000 + expert); front is least-recently-used.
 */

static uint64_t expert_cache_key(uint32_t layer_id, uint32_t expert_id)
{
    return (uint64_t)layer_id * 1000ULL + (uint64_t)expert_id;
}

void mem_service_expert_cache_init(struct mem_service_expert_cache *cache,
                                   uint32_t capacity_slots,
                                   uint64_t expert_bytes)
{
    if (!cache) {
        return;
    }
    memset(cache, 0, sizeof(*cache));
    cache->capacity_slots = capacity_slots > MEM_SERVICE_EXPERT_CACHE_DEFAULT_SLOTS
        ? MEM_SERVICE_EXPERT_CACHE_DEFAULT_SLOTS
        : (capacity_slots == 0 ? MEM_SERVICE_EXPERT_CACHE_DEFAULT_SLOTS : capacity_slots);
    cache->expert_bytes = expert_bytes;
}

void mem_service_expert_cache_preload(struct mem_service_expert_cache *cache,
                                      const uint32_t (*pairs)[2],
                                      uint32_t pair_count)
{
    uint32_t i;
    uint32_t j;

    if (!cache || !pairs) {
        return;
    }
    for (i = 0; i < pair_count && cache->resident_count < cache->capacity_slots; ++i) {
        uint64_t key = expert_cache_key(pairs[i][0], pairs[i][1]);
        bool present = false;
        for (j = 0; j < cache->resident_count; ++j) {
            if (cache->resident[j] == key) {
                present = true;
                break;
            }
        }
        if (!present) {
            cache->resident[cache->resident_count++] = key;
        }
    }
}

static int32_t expert_cache_find(const struct mem_service_expert_cache *cache, uint64_t key)
{
    uint32_t i;

    for (i = 0; i < cache->resident_count; ++i) {
        if (cache->resident[i] == key) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void expert_cache_remove_at(struct mem_service_expert_cache *cache, uint32_t index)
{
    uint32_t i;

    for (i = index; i + 1 < cache->resident_count; ++i) {
        cache->resident[i] = cache->resident[i + 1];
    }
    if (cache->resident_count > 0) {
        cache->resident_count--;
        cache->resident[cache->resident_count] = 0;
    }
}

bool mem_service_expert_cache_touch(struct mem_service_expert_cache *cache,
                                    uint32_t layer_id,
                                    uint32_t expert_id)
{
    uint64_t key;
    int32_t index;

    if (!cache) {
        return false;
    }
    key = expert_cache_key(layer_id, expert_id);
    index = expert_cache_find(cache, key);
    if (index >= 0) {
        /* Hit: promote to MRU (back). */
        expert_cache_remove_at(cache, (uint32_t)index);
        cache->resident[cache->resident_count++] = key;
        cache->stats.hits++;
        return true;
    }
    /* Miss: account pread and possibly evict LRU (front). */
    cache->stats.misses++;
    cache->stats.pread_bytes += cache->expert_bytes;
    if (cache->resident_count >= cache->capacity_slots && cache->resident_count > 0) {
        expert_cache_remove_at(cache, 0);
        cache->stats.evictions++;
    }
    if (cache->resident_count < cache->capacity_slots) {
        cache->resident[cache->resident_count++] = key;
    }
    return false;
}

void mem_service_expert_cache_stats(const struct mem_service_expert_cache *cache,
                                    struct mem_service_expert_cache_stats *out)
{
    if (!out) {
        return;
    }
    if (!cache) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = cache->stats;
}
