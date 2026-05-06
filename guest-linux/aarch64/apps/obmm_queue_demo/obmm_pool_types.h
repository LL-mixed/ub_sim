/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM shared memory pool lockless queue -- protocol types.
 *
 * All structures are shared-memory wire format.  Keep fields naturally
 * aligned and do not rely on compiler-specific padding.
 */

#ifndef OBMM_POOL_TYPES_H
#define OBMM_POOL_TYPES_H

#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Pool header constants                                               */
/* ------------------------------------------------------------------ */

#define OBMM_POOL_MAGIC         0x4f424d51504f4f4cULL /* "OBMQPOOL" */
#define OBMM_POOL_LAYOUT_VERSION 1U

/* ------------------------------------------------------------------ */
/* Queue depth limits                                                  */
/* ------------------------------------------------------------------ */

#define OBMM_QUEUE_DEFAULT_DEPTH 1024U
#define OBMM_QUEUE_MIN_DEPTH     64U
#define OBMM_QUEUE_MAX_DEPTH     65536U

/* ------------------------------------------------------------------ */
/* Pool state                                                          */
/* ------------------------------------------------------------------ */

enum obmm_pool_state {
    OBMM_POOL_STATE_INIT  = 0,
    OBMM_POOL_STATE_READY = 1,
    OBMM_POOL_STATE_ERROR = 2,
};

/* ------------------------------------------------------------------ */
/* Region directory kinds                                              */
/* ------------------------------------------------------------------ */

enum obmm_region_kind {
    OBMM_REGION_QUEUE       = 1,
    OBMM_REGION_RX_ARENA    = 2,
    OBMM_REGION_TX_ARENA    = 3,
    OBMM_REGION_DATA_SLAB   = 4,
    OBMM_REGION_W4_PAYLOAD  = 5,
};

/* ------------------------------------------------------------------ */
/* Descriptor types                                                    */
/* ------------------------------------------------------------------ */

enum obmm_desc_type {
    OBMM_DESC_DATA         = 1, /* data payload descriptor */
    OBMM_DESC_ACK          = 2, /* consumer acknowledgment */
    OBMM_DESC_COMMIT       = 3, /* owner round commit */
    OBMM_DESC_STRESS       = 4, /* queue fill/drain visibility probe */
    OBMM_DESC_STRESS_ACK   = 5, /* stress batch completion */
    OBMM_DESC_W4_READY     = 6, /* w4_db: payload published */
    OBMM_DESC_W4_OBSERVED  = 7, /* w4_db: payload consumed */
};

/* ------------------------------------------------------------------ */
/* Pool header -- one 64-byte cache line at offset 0                   */
/* ------------------------------------------------------------------ */

struct obmm_pool_header {
    uint64_t        magic;
    uint32_t        layout_version;
    uint16_t        node_id;
    uint16_t        node_count;
    _Atomic uint32_t state;
    _Atomic uint32_t generation;
    uint64_t        region_size;
    uint64_t        directory_offset;
    uint32_t        directory_count;
    uint32_t        default_queue_depth;
    uint32_t        flags;
    uint32_t        reserved[3];
};

static_assert(sizeof(struct obmm_pool_header) == 64,
              "obmm_pool_header must be 64 bytes");
static_assert(alignof(struct obmm_pool_header) >= 8,
              "obmm_pool_header must be 8-byte aligned");

/* ------------------------------------------------------------------ */
/* Region directory entry -- 32 bytes each                             */
/* ------------------------------------------------------------------ */

struct obmm_region_dirent {
    uint32_t region_id;
    uint16_t kind;
    uint16_t peer_node_id;
    uint64_t offset;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
};

static_assert(sizeof(struct obmm_region_dirent) == 32,
              "obmm_region_dirent must be 32 bytes");
static_assert(alignof(struct obmm_region_dirent) >= 8,
              "obmm_region_dirent must be 8-byte aligned");

/* ------------------------------------------------------------------ */
/* Descriptor -- 32 bytes, two per cache line                          */
/* ------------------------------------------------------------------ */

struct obmm_desc {
    uint64_t seq;
    uint32_t region_id;
    uint32_t payload_len;
    uint64_t payload_offset;
    uint16_t type;
    uint16_t flags;
    uint32_t cookie;
};

static_assert(sizeof(struct obmm_desc) == 32,
              "obmm_desc must be 32 bytes");
static_assert(alignof(struct obmm_desc) >= 8,
              "obmm_desc must be 8-byte aligned");

/* ------------------------------------------------------------------ */
/* SPSC queue -- variable length, cache-line aligned                   */
/* ------------------------------------------------------------------ */

struct obmm_spsc_queue {
    alignas(64) _Atomic uint32_t head; /* consumer-owned */
    uint8_t head_pad[60];

    alignas(64) _Atomic uint32_t tail; /* producer-owned */
    uint8_t tail_pad[60];

    uint32_t size;
    uint32_t mask;
    uint8_t  reserved[56];

    alignas(64) struct obmm_desc desc[];
};

#endif /* OBMM_POOL_TYPES_H */
