#ifndef LINGQU_OBJECT_SERVICE_H
#define LINGQU_OBJECT_SERVICE_H

#include <stdint.h>

#define LINGQU_OBMM_OBJECT_REF_MAGIC 0x514f424d4d524546ULL
#define LINGQU_OBMM_OBJECT_REF_LAYOUT_VERSION 1U
#define LINGQU_OBJECT_STATE_PENDING_WIRE 1U
#define LINGQU_OBJECT_STATE_COMMITTED_WIRE 2U
#define LINGQU_OBJECT_STATE_TOMBSTONED_WIRE 3U
#define LINGQU_OBJECT_STATE_QUARANTINED_WIRE 4U

struct lingqu_obmm_object_ref_wire {
    uint64_t magic;
    uint16_t layout_version;
    uint16_t object_kind;
    uint16_t state;
    uint16_t flags;
    uint32_t owner_entity;
    uint32_t producer_entity;
    uint64_t object_version;
    uint64_t key_hash;
    uint64_t payload_offset;
    uint64_t payload_bytes;
    uint64_t payload_checksum;
};

#endif
