/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM SPMC broadcast stream -- header-only implementation.
 *
 * Provider-owned ring with per-consumer cursors (64-bit counters).
 * Single provider writes tail and descriptor slots; each consumer
 * writes only its own cursor.
 *
 * See docs/drafts/obmm_spmc_mpsc_queue_design.md for full specification.
 */

#ifndef OBMM_SPMC_QUEUE_H
#define OBMM_SPMC_QUEUE_H

#include "obmm_queue.h"

#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Runtime view (not shared-memory wire format)                        */
/* ------------------------------------------------------------------ */

struct obmm_spmc_stream_view {
    uint8_t *pool_base;
    uint64_t pool_size;
    const struct obmm_region_dirent *dir;
    uint32_t dir_count;
    uint32_t provider_node;
    struct obmm_spmc_stream *stream;
};

/* ------------------------------------------------------------------ */
/* Layout helpers                                                      */
/* ------------------------------------------------------------------ */

static inline uint64_t obmm_spmc_region_size(uint32_t depth,
                                              uint32_t max_consumers)
{
    uint64_t cursor_off = obmm_align_up_u64(sizeof(struct obmm_spmc_stream), 64);
    uint64_t desc_off = obmm_align_up_u64(
        cursor_off + (uint64_t)max_consumers * sizeof(struct obmm_spmc_consumer_cursor),
        64);
    return desc_off + (uint64_t)depth * sizeof(struct obmm_desc);
}

static inline struct obmm_spmc_consumer_cursor *
obmm_spmc_cursor(struct obmm_spmc_stream *s, uint32_t node_id)
{
    return (struct obmm_spmc_consumer_cursor *)
        ((uint8_t *)s + s->cursor_offset) + node_id;
}

static inline struct obmm_desc *
obmm_spmc_desc_ring(struct obmm_spmc_stream *s)
{
    return (struct obmm_desc *)((uint8_t *)s + s->desc_offset);
}

/* ------------------------------------------------------------------ */
/* Stream initialization                                               */
/* ------------------------------------------------------------------ */

static inline int obmm_spmc_stream_init(void *base, uint32_t depth,
                                         uint32_t max_consumers,
                                         uint32_t provider_node,
                                         uint64_t consumer_mask)
{
    struct obmm_spmc_stream *s = (struct obmm_spmc_stream *)base;
    uint64_t cursor_off, desc_off;

    if (depth < OBMM_QUEUE_MIN_DEPTH || depth > OBMM_QUEUE_MAX_DEPTH)
        return -EINVAL;
    if (depth == 0 || (depth & (depth - 1)) != 0)
        return -EINVAL;
    if (max_consumers == 0 || max_consumers > OBMM_SPMC_MAX_CONSUMERS)
        return -EINVAL;
    if (consumer_mask != 0 &&
        (uint32_t)(63 - __builtin_clzll(consumer_mask)) >= max_consumers)
        return -EINVAL;

    cursor_off = obmm_align_up_u64(sizeof(struct obmm_spmc_stream), 64);
    desc_off = obmm_align_up_u64(
        cursor_off + (uint64_t)max_consumers * sizeof(struct obmm_spmc_consumer_cursor),
        64);

    memset(base, 0, desc_off + (uint64_t)depth * sizeof(struct obmm_desc));

    s->magic = OBMM_SPMC_MAGIC;
    s->version = OBMM_SPMC_VERSION;
    s->flags = OBMM_SPMC_F_STRICT;
    s->generation = 1;
    s->header_bytes = sizeof(struct obmm_spmc_stream);
    s->cursor_offset = (uint32_t)cursor_off;
    s->desc_offset = (uint32_t)desc_off;
    s->depth = depth;
    s->mask = depth - 1;
    s->max_consumers = max_consumers;
    s->provider_node = provider_node;

    atomic_store_explicit(&s->active_consumer_mask, consumer_mask,
                          memory_order_relaxed);
    atomic_store_explicit(&s->tail, 0, memory_order_relaxed);

    uint32_t nid;
    OBMM_FOR_EACH_NODE_ID(nid, consumer_mask) {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, nid);
        c->node_id = nid;
        atomic_store_explicit(&c->generation_seen, s->generation,
                              memory_order_relaxed);
        atomic_store_explicit(&c->state, OBMM_SPMC_CONSUMER_ACTIVE,
                              memory_order_release);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* View initialization                                                 */
/* ------------------------------------------------------------------ */

static inline int
obmm_spmc_view_init_from_directory(struct obmm_spmc_stream_view *v,
                                   void *pool_base,
                                   uint64_t pool_size,
                                   const struct obmm_region_dirent *dir,
                                   uint32_t dir_count,
                                   uint32_t provider_node)
{
    const struct obmm_region_dirent *spmc_ent = NULL;

    for (uint32_t i = 0; i < dir_count; ++i) {
        if (dir[i].kind == OBMM_REGION_SPMC_STREAM) {
            if (spmc_ent != NULL)
                return -EEXIST;
            spmc_ent = &dir[i];
        }
    }
    if (spmc_ent == NULL)
        return -ENOENT;

    if (spmc_ent->offset > pool_size ||
        spmc_ent->size > pool_size - spmc_ent->offset)
        return -EINVAL;
    if (spmc_ent->size < sizeof(struct obmm_spmc_stream))
        return -EINVAL;

    struct obmm_spmc_stream *s =
        (struct obmm_spmc_stream *)((uint8_t *)pool_base + spmc_ent->offset);

    if (s->magic != OBMM_SPMC_MAGIC)
        return -EINVAL;
    if (s->version != OBMM_SPMC_VERSION)
        return -EINVAL;
    if (s->provider_node != provider_node)
        return -EINVAL;
    if (s->max_consumers == 0 || s->max_consumers > OBMM_SPMC_MAX_CONSUMERS)
        return -EINVAL;
    if (s->depth < OBMM_QUEUE_MIN_DEPTH || s->depth > OBMM_QUEUE_MAX_DEPTH)
        return -EINVAL;
    if ((s->depth & (s->depth - 1)) != 0)
        return -EINVAL;
    if (s->cursor_offset < sizeof(*s) || (s->cursor_offset & 63) != 0)
        return -EINVAL;
    if (s->desc_offset < s->cursor_offset || (s->desc_offset & 63) != 0)
        return -EINVAL;
    if (s->desc_offset - s->cursor_offset <
        (uint64_t)s->max_consumers * sizeof(struct obmm_spmc_consumer_cursor))
        return -EINVAL;
    if (s->desc_offset > spmc_ent->size ||
        (uint64_t)s->depth * sizeof(struct obmm_desc) >
            spmc_ent->size - s->desc_offset)
        return -EINVAL;

    v->pool_base = (uint8_t *)pool_base;
    v->pool_size = pool_size;
    v->dir = dir;
    v->dir_count = dir_count;
    v->provider_node = provider_node;
    v->stream = s;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Provider payload address lookup                                     */
/* ------------------------------------------------------------------ */

static inline int
obmm_spmc_provider_payload_addr(const struct obmm_spmc_stream_view *v,
                                const struct obmm_desc *desc,
                                const void **payload_addr_out)
{
    for (uint32_t i = 0; i < v->dir_count; ++i) {
        if (v->dir[i].region_id != desc->region_id)
            continue;

        uint64_t off = v->dir[i].offset;
        uint64_t sz = v->dir[i].size;

        if (off > v->pool_size || sz > v->pool_size - off)
            return -EINVAL;

        if (desc->payload_len == 0)
            return 0;

        if (desc->payload_offset > sz ||
            desc->payload_len > sz - desc->payload_offset)
            return -EINVAL;

        if (v->dir[i].kind == OBMM_REGION_TX_ARENA &&
            v->dir[i].peer_node_id == v->provider_node) {
            *payload_addr_out = v->pool_base + off + desc->payload_offset;
            return 1;
        }
        return 0;
    }
    return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* Publish path (provider)                                             */
/* ------------------------------------------------------------------ */

static inline int obmm_spmc_publish(struct obmm_spmc_stream_view *v,
                                     const struct obmm_desc *desc)
{
    struct obmm_spmc_stream *s = v->stream;
    struct obmm_desc *ring = obmm_spmc_desc_ring(s);
    const void *payload_addr = NULL;
    int payload_rc;
    uint64_t active = atomic_load_explicit(&s->active_consumer_mask,
                                           memory_order_acquire);
    uint64_t wait_mask = active;
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
    uint64_t min_head = tail;
    uint32_t i;

    if (wait_mask == 0)
        return -ENODEV;

    OBMM_FOR_EACH_NODE_ID(i, wait_mask) {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, i);
        uint32_t state = atomic_load_explicit(&c->state,
                                              memory_order_acquire);
        if (state != OBMM_SPMC_CONSUMER_ACTIVE)
            return -EPIPE;

        uint64_t head = atomic_load_explicit(&c->head, memory_order_acquire);
        if (head < min_head)
            min_head = head;
    }

    if (tail - min_head >= s->depth)
        return -EAGAIN;

    payload_rc = obmm_spmc_provider_payload_addr(v, desc, &payload_addr);
    if (payload_rc < 0)
        return payload_rc;
    if (payload_rc > 0)
        obmm_publish_payload_for_remote_read(payload_addr, desc->payload_len);

    ring[tail & s->mask] = *desc;
    obmm_publish_desc_for_remote_read(&ring[tail & s->mask],
                                      sizeof(ring[tail & s->mask]));
    atomic_store_explicit(&s->tail, tail + 1, memory_order_release);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Consume path (peer)                                                 */
/* ------------------------------------------------------------------ */

static inline int obmm_spmc_consume(struct obmm_spmc_stream_view *v,
                                     uint32_t consumer_idx,
                                     struct obmm_desc *desc)
{
    struct obmm_spmc_stream *s = v->stream;
    struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, consumer_idx);
    struct obmm_desc *ring = obmm_spmc_desc_ring(s);
    uint32_t state = atomic_load_explicit(&c->state, memory_order_acquire);
    uint64_t head = atomic_load_explicit(&c->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);

    if (state != OBMM_SPMC_CONSUMER_ACTIVE)
        return -ENODEV;

    if (head == tail)
        return -EAGAIN;

    if (tail - head > s->depth) {
        atomic_fetch_add_explicit(&c->drop_count, 1, memory_order_relaxed);
        atomic_store_explicit(&c->state, OBMM_SPMC_CONSUMER_PAUSED,
                              memory_order_release);
        obmm_publish_cursor_for_provider_read(c, sizeof(*c));
        return -EOVERFLOW;
    }

    *desc = ring[head & s->mask];
    atomic_store_explicit(&c->observed_seq, desc->seq, memory_order_relaxed);
    atomic_store_explicit(&c->head, head + 1, memory_order_release);
    obmm_publish_cursor_for_provider_read(c, sizeof(*c));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reclamation                                                         */
/* ------------------------------------------------------------------ */

static inline uint64_t
obmm_spmc_reclaimable_head(struct obmm_spmc_stream_view *v)
{
    struct obmm_spmc_stream *s = v->stream;
    uint64_t wait_mask = atomic_load_explicit(&s->active_consumer_mask,
                                              memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    uint64_t min_head = tail;
    uint32_t i;

    if (wait_mask == 0)
        return tail;

    OBMM_FOR_EACH_NODE_ID(i, wait_mask) {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, i);
        uint32_t state = atomic_load_explicit(&c->state,
                                              memory_order_acquire);
        if (state != OBMM_SPMC_CONSUMER_ACTIVE)
            continue;

        uint64_t head = atomic_load_explicit(&c->head, memory_order_acquire);
        if (head < min_head)
            min_head = head;
    }

    return min_head;
}

struct obmm_spmc_tx_reclaim_state {
    uint64_t desc_reclaimed_to;
    uint64_t tx_reclaim_offset;
};

static inline int
obmm_spmc_reclaim_payloads(struct obmm_spmc_stream_view *v,
                           struct obmm_spmc_tx_reclaim_state *st)
{
    struct obmm_spmc_stream *s = v->stream;
    struct obmm_desc *ring = obmm_spmc_desc_ring(s);
    uint64_t reclaim_head = obmm_spmc_reclaimable_head(v);

    while (st->desc_reclaimed_to < reclaim_head) {
        struct obmm_desc *d = &ring[st->desc_reclaimed_to & s->mask];
        const void *payload_addr;
        int rc = obmm_spmc_provider_payload_addr(v, d, &payload_addr);

        if (rc < 0)
            return rc;
        if (rc > 0) {
            uint64_t end = d->payload_offset + d->payload_len;
            if (end > st->tx_reclaim_offset)
                st->tx_reclaim_offset = end;
        }
        st->desc_reclaimed_to++;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Stream reset                                                        */
/* ------------------------------------------------------------------ */

static inline int obmm_spmc_stream_reset(struct obmm_spmc_stream *s,
                                          uint64_t consumer_mask)
{
    uint64_t new_generation = s->generation + 1;
    uint32_t nid;

    if (consumer_mask != 0 &&
        (uint32_t)(63 - __builtin_clzll(consumer_mask)) >= s->max_consumers)
        return -EINVAL;

    atomic_store_explicit(&s->active_consumer_mask, 0, memory_order_release);
    memset((uint8_t *)s + s->cursor_offset, 0,
           s->desc_offset - s->cursor_offset +
           (uint64_t)s->depth * sizeof(struct obmm_desc));

    s->generation = new_generation;
    atomic_store_explicit(&s->tail, 0, memory_order_relaxed);

    OBMM_FOR_EACH_NODE_ID(nid, consumer_mask) {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, nid);
        c->node_id = nid;
        atomic_store_explicit(&c->head, 0, memory_order_relaxed);
        atomic_store_explicit(&c->observed_seq, 0, memory_order_relaxed);
        atomic_store_explicit(&c->generation_seen, new_generation,
                              memory_order_relaxed);
        atomic_store_explicit(&c->state, OBMM_SPMC_CONSUMER_ACTIVE,
                              memory_order_release);
    }

    atomic_store_explicit(&s->active_consumer_mask, consumer_mask,
                          memory_order_release);
    return 0;
}

#endif /* OBMM_SPMC_QUEUE_H */
