/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM MPMC bus -- header-only implementation.
 *
 * Multi-producer, multi-consumer queue built from MPSC lane sets.
 * Each consumer owns an MPSC lane set; publishers push to a specific
 * consumer's lane.  Consumers drain their own lane set with round-robin
 * fairness.
 *
 * Targeted delivery: the producer chooses which consumer receives the
 * message.  This is the natural MPMC for OBMM shared-memory where each
 * node hosts its own ingress queues.
 *
 * No new shared-memory wire format -- reuses OBMM_REGION_QUEUE entries.
 *
 * See docs/drafts/obmm_spmc_mpsc_queue_design.md for full specification.
 */

#ifndef OBMM_MPMC_QUEUE_H
#define OBMM_MPMC_QUEUE_H

#include "obmm_mpsc_queue.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct obmm_mpmc_bus {
    uint32_t node_count;
    uint32_t local_node;
    struct obmm_mpsc_consumer_set rx;
    struct obmm_mpsc_publisher_lane tx[OBMM_MPSC_MAX_LANES];
    bool tx_valid[OBMM_MPSC_MAX_LANES];
};

/* ------------------------------------------------------------------ */
/* Consumer-side initialization                                        */
/* ------------------------------------------------------------------ */

static inline int
obmm_mpmc_consumer_init(struct obmm_mpmc_bus *bus,
                         const struct obmm_region_dirent *dir,
                         uint32_t dir_count,
                         uint32_t local_node)
{
    int rc;

    memset(bus, 0, sizeof(*bus));
    bus->local_node = local_node;

    rc = obmm_mpsc_consumer_set_init_from_directory(&bus->rx,
            dir, dir_count, local_node);
    if (rc != 0)
        return rc;

    bus->node_count = bus->rx.lane_count + 1; /* consumers + self */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Publisher-side initialization                                       */
/* ------------------------------------------------------------------ */

static inline int
obmm_mpmc_publisher_init(struct obmm_mpmc_bus *bus,
                          uint32_t target,
                          const struct obmm_region_dirent *dir,
                          uint32_t dir_count,
                          uint32_t local_node)
{
    int rc;

    if (target >= OBMM_MPSC_MAX_LANES)
        return -EINVAL;

    if (bus->tx_valid[target])
        return -EEXIST;

    rc = obmm_mpsc_publisher_lane_init_from_directory(&bus->tx[target],
            dir, dir_count, local_node, target);
    if (rc != 0)
        return rc;

    bus->tx_valid[target] = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Producer path                                                       */
/* ------------------------------------------------------------------ */

static inline int
obmm_mpmc_send(struct obmm_mpmc_bus *bus, uint32_t target,
                const struct obmm_desc *desc)
{
    if (target >= OBMM_MPSC_MAX_LANES || !bus->tx_valid[target])
        return -ENOENT;
    return obmm_mpsc_push(&bus->tx[target], desc);
}

/* ------------------------------------------------------------------ */
/* Consumer path                                                       */
/* ------------------------------------------------------------------ */

static inline int
obmm_mpmc_recv(struct obmm_mpmc_bus *bus, struct obmm_desc *out,
                uint32_t *src_out)
{
    uint64_t rx_seq;
    return obmm_mpsc_poll(&bus->rx, out, src_out, &rx_seq);
}

#endif /* OBMM_MPMC_QUEUE_H */
