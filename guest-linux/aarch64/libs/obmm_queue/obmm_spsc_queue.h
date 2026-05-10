/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM SPSC lockless queue -- header-only implementation.
 *
 * Single-producer, single-consumer fixed-size ring buffer with
 * power-of-two depth.  Head and tail live on separate cache lines.
 *
 * Memory ordering follows the design spec:
 *   producer: relaxed tail load, acquire head load, release tail store
 *   consumer: relaxed head load, acquire tail load, release head store
 */

#ifndef OBMM_SPSC_QUEUE_H
#define OBMM_SPSC_QUEUE_H

#include "obmm_queue_types.h"

#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * obmm_align_up_u64 is provided by obmm_common.h when included first.
 * Define it here only as a fallback for standalone compilation.
 */
#ifndef OBMM_COMMON_H
static inline uint64_t obmm_align_up_u64(uint64_t v, uint64_t align)
{
    return (v + align - 1) & ~(align - 1);
}
#endif

static inline uint64_t obmm_queue_region_size(uint32_t depth)
{
    return obmm_align_up_u64(sizeof(struct obmm_spsc_queue), 64) +
           (uint64_t)depth * sizeof(struct obmm_desc);
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

/*
 * obmm_spsc_queue_init -- zero and configure a queue in the given memory.
 *
 * @base:  pointer to a memory region of at least obmm_queue_region_size(depth)
 * @depth: queue depth (must be power-of-two, in [MIN_DEPTH, MAX_DEPTH])
 *
 * Returns 0 on success, -EINVAL on invalid depth.
 */
static inline int obmm_spsc_queue_init(void *base, uint32_t depth)
{
    struct obmm_spsc_queue *q = (struct obmm_spsc_queue *)base;

    if (depth < OBMM_QUEUE_MIN_DEPTH || depth > OBMM_QUEUE_MAX_DEPTH)
        return -EINVAL;
    if (depth == 0 || (depth & (depth - 1)) != 0)
        return -EINVAL;

    memset(q, 0, obmm_queue_region_size(depth));
    q->size = depth;
    q->mask = depth - 1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Producer path                                                       */
/* ------------------------------------------------------------------ */

/*
 * obmm_spsc_push -- append a descriptor to the queue.
 *
 * Must be called only from the single producer thread/node.
 * Returns 0 on success, -EAGAIN if the queue is full.
 */
static inline int obmm_spsc_push(struct obmm_spsc_queue *q,
                                 const struct obmm_desc *desc)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (tail - head == q->size)
        return -EAGAIN;

    q->desc[tail & q->mask] = *desc;

    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Consumer path                                                       */
/* ------------------------------------------------------------------ */

/*
 * obmm_spsc_pop -- consume a descriptor from the queue.
 *
 * Must be called only from the single consumer thread/node.
 * Returns 0 on success, -EAGAIN if the queue is empty.
 */
static inline int obmm_spsc_pop(struct obmm_spsc_queue *q,
                                struct obmm_desc *desc)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (head == tail)
        return -EAGAIN;

    *desc = q->desc[head & q->mask];

    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Debug / introspection                                               */
/* ------------------------------------------------------------------ */

static inline uint32_t obmm_spsc_capacity(const struct obmm_spsc_queue *q)
{
    return q->size;
}

static inline uint32_t obmm_spsc_available(const struct obmm_spsc_queue *q)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    return tail - head;
}

/* ------------------------------------------------------------------ */
/* Bitmask iteration                                                   */
/* ------------------------------------------------------------------ */

#define OBMM_FOR_EACH_NODE_ID(nid, mask)                             \
    for (uint64_t _m = (mask);                                       \
         _m != 0 && ((nid) = (uint32_t)(__builtin_ffsll(_m) - 1), 1); \
         _m &= _m - 1)

/* ------------------------------------------------------------------ */
/* OBMM visibility helpers                                             */
/* ------------------------------------------------------------------ */

/*
 * These stubs wrap the visibility boundary between local cacheable
 * writes and remote NC reads.  For the first user-space demo they are
 * release fences.  If validation fails, these become the place to add
 * cache maintenance or a driver operation.
 */

static inline void obmm_publish_payload_for_remote_read(const void *addr,
                                                         uint64_t len)
{
    (void)addr; (void)len;
    atomic_thread_fence(memory_order_release);
}

static inline void obmm_publish_desc_for_remote_read(const void *addr,
                                                      uint64_t len)
{
    (void)addr; (void)len;
    atomic_thread_fence(memory_order_release);
}

static inline void obmm_publish_cursor_for_provider_read(const void *addr,
                                                          uint64_t len)
{
    (void)addr; (void)len;
    atomic_thread_fence(memory_order_release);
}

#endif /* OBMM_SPSC_QUEUE_H */
