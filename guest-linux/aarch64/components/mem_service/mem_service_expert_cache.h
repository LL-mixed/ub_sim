#ifndef MEM_SERVICE_EXPERT_CACHE_H
#define MEM_SERVICE_EXPERT_CACHE_H

/*
 * Node-side expert cache simulator (stage 2).
 *
 * Models an LRU cache of expert weight tiles with a fixed slot budget and
 * optional hotlist preload, mirroring ds4_ssd.c + ds4_streaming_hotlist.inc.
 * As the decode stream touches experts per the routing trace, the cache
 * records hits, misses, evictions, and the pread byte budget — inputs to
 * the latency model max(compute_time, miss_load_time) (plan section 5,
 * stage 2.3).
 *
 * The cache is a node-side optimization layer over objects resolved from
 * mem_service, NOT a new handoff flow (plan section 3.3). It does not hold
 * payload bytes; it only tracks residency and statistics.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Resident-cache slot capacity is bounded; stage 2 uses a modest fixed cap
 * matching the OBMM dynamic arena sizing. Real tuning via
 * SIM_MODEL_EXPERT_CACHE_BYTES / SIM_MODEL_EXPERT_PRELOAD is stage-2 polish.
 */
#define MEM_SERVICE_EXPERT_CACHE_DEFAULT_SLOTS 64U

struct mem_service_expert_cache_stats {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t pread_bytes;
};

struct mem_service_expert_cache {
    uint32_t capacity_slots;
    uint64_t expert_bytes;
    /* LRU ring: resident[slot] = packed key (layer*1000 + expert), 0 = empty */
    uint64_t resident[MEM_SERVICE_EXPERT_CACHE_DEFAULT_SLOTS];
    uint32_t resident_count;
    struct mem_service_expert_cache_stats stats;
};

/*
 * Initialize a cache with the given slot capacity (clamped to the compile-time
 * cap) and per-expert byte size.
 */
void mem_service_expert_cache_init(struct mem_service_expert_cache *cache,
                                   uint32_t capacity_slots,
                                   uint64_t expert_bytes);

/* Preload a hotlist of (layer, expert) pairs at startup (no misses counted). */
void mem_service_expert_cache_preload(struct mem_service_expert_cache *cache,
                                      const uint32_t (*pairs)[2],
                                      uint32_t pair_count);

/*
 * Touch one expert for one layer. Records a hit or a miss (+pread +
 * possible eviction) and updates LRU order. Returns true on hit.
 */
bool mem_service_expert_cache_touch(struct mem_service_expert_cache *cache,
                                    uint32_t layer_id,
                                    uint32_t expert_id);

/* Snapshot the current hit/miss/eviction/pread statistics. */
void mem_service_expert_cache_stats(const struct mem_service_expert_cache *cache,
                                    struct mem_service_expert_cache_stats *out);

#endif
