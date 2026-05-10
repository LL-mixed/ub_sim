/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM MPSC lane set -- header-only implementation.
 *
 * Multi-producer, single-consumer queue built from SPSC lanes.
 * Each publisher writes its own lane; the consumer drains all lanes
 * with round-robin fairness.
 *
 * No new shared-memory wire format -- reuses OBMM_REGION_QUEUE entries.
 *
 * See docs/drafts/obmm_spmc_mpsc_queue_design.md for full specification.
 */

#ifndef OBMM_MPSC_QUEUE_H
#define OBMM_MPSC_QUEUE_H

#include "obmm_queue.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OBMM_MPSC_MAX_LANES    OBMM_SPMC_MAX_CONSUMERS
#define OBMM_MPSC_DEFAULT_BUDGET 1

struct obmm_mpsc_lane {
    uint32_t publisher_node;
    uint32_t weight;
    uint32_t credit;
    struct obmm_spsc_queue *queue;
};

struct obmm_mpsc_consumer_set {
    uint32_t consumer_node;
    uint32_t lane_count;
    uint32_t next_lane;
    uint32_t budget;
    uint64_t rx_seq;
    struct obmm_mpsc_lane lane[OBMM_MPSC_MAX_LANES];
};

struct obmm_mpsc_publisher_lane {
    uint32_t publisher_node;
    uint32_t consumer_node;
    struct obmm_spsc_queue *queue;
};

/* ------------------------------------------------------------------ */
/* Consumer-side initialization                                        */
/* ------------------------------------------------------------------ */

/*
 * Compare helper for qsort: sort lanes by publisher_node ascending.
 */
static inline int mpsc_lane_cmp(const void *a, const void *b)
{
    const struct obmm_mpsc_lane *la = (const struct obmm_mpsc_lane *)a;
    const struct obmm_mpsc_lane *lb = (const struct obmm_mpsc_lane *)b;
    if (la->publisher_node < lb->publisher_node) return -1;
    if (la->publisher_node > lb->publisher_node) return 1;
    return 0;
}

static inline int
obmm_mpsc_consumer_set_init_from_directory(struct obmm_mpsc_consumer_set *set,
                                           const struct obmm_region_dirent *dir,
                                           uint32_t dir_count,
                                           uint32_t local_consumer_node)
{
    uint32_t count = 0;

    memset(set, 0, sizeof(*set));
    set->consumer_node = local_consumer_node;
    set->budget = OBMM_MPSC_DEFAULT_BUDGET;

    for (uint32_t i = 0; i < dir_count && i < OBMM_MPSC_MAX_LANES; ++i) {
        if (dir[i].kind != OBMM_REGION_QUEUE)
            continue;

        /* Check for duplicate publisher */
        for (uint32_t j = 0; j < count; ++j) {
            if (set->lane[j].publisher_node == dir[i].peer_node_id)
                return -EEXIST;
        }

        set->lane[count].publisher_node = dir[i].peer_node_id;
        set->lane[count].weight = 1;
        set->lane[count].credit = 0;
        set->lane[count].queue = NULL; /* filled by caller after mmap */
        count++;
    }

    /* Check if there are more lanes beyond what we scanned */
    for (uint32_t i = OBMM_MPSC_MAX_LANES; i < dir_count; ++i) {
        if (dir[i].kind == OBMM_REGION_QUEUE) {
            if (count >= OBMM_MPSC_MAX_LANES)
                return -E2BIG;
        }
    }

    if (count == 0)
        return -ENOENT;

    set->lane_count = count;

    /* Sort by publisher_node for stable logs and deterministic tests */
    qsort(set->lane, count, sizeof(set->lane[0]), mpsc_lane_cmp);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Publisher-side initialization                                       */
/* ------------------------------------------------------------------ */

static inline int
obmm_mpsc_publisher_lane_init_from_directory(struct obmm_mpsc_publisher_lane *lane,
                                             const struct obmm_region_dirent *dir,
                                             uint32_t dir_count,
                                             uint32_t local_publisher_node,
                                             uint32_t target_consumer_node)
{
    uint32_t found = 0;
    uint32_t found_idx = 0;

    (void)target_consumer_node;

    memset(lane, 0, sizeof(*lane));

    for (uint32_t i = 0; i < dir_count; ++i) {
        if (dir[i].kind != OBMM_REGION_QUEUE)
            continue;
        if (dir[i].peer_node_id != local_publisher_node)
            continue;

        if (found > 0)
            return -EEXIST;

        found_idx = i;
        found++;
    }

    if (found == 0)
        return -ENOENT;

    lane->publisher_node = local_publisher_node;
    lane->consumer_node = target_consumer_node;
    lane->queue = NULL; /* filled by caller after mmap */
    (void)found_idx;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Producer path                                                       */
/* ------------------------------------------------------------------ */

static inline int obmm_mpsc_push(struct obmm_mpsc_publisher_lane *lane,
                                  const struct obmm_desc *desc)
{
    return obmm_spsc_push(lane->queue, desc);
}

/* ------------------------------------------------------------------ */
/* Consumer path                                                       */
/* ------------------------------------------------------------------ */

static inline int obmm_mpsc_poll(struct obmm_mpsc_consumer_set *set,
                                  struct obmm_desc *out,
                                  uint32_t *publisher_out,
                                  uint64_t *rx_seq_out)
{
    for (uint32_t n = 0; n < set->lane_count; ++n) {
        uint32_t i = (set->next_lane + n) % set->lane_count;
        struct obmm_mpsc_lane *lane = &set->lane[i];

        if (obmm_spsc_pop(lane->queue, out) == 0) {
            if (rx_seq_out != NULL)
                *rx_seq_out = set->rx_seq++;
            set->next_lane = (i + 1) % set->lane_count;
            if (publisher_out != NULL)
                *publisher_out = lane->publisher_node;
            return 0;
        }
    }
    return -EAGAIN;
}

#endif /* OBMM_MPSC_QUEUE_H */
