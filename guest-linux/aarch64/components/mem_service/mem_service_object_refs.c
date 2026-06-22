#include "mem_service_object_refs.h"

#include <stdint.h>
#include <string.h>

uint64_t mem_service_checksum_bytes(const uint8_t *bytes, uint64_t len)
{
    uint64_t hash = 1469598103934665603ULL;
    uint64_t i;

    for (i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

int mem_service_record_to_lingqu_obmm_ref(const struct mem_service_record *record,
                                    struct lingqu_obmm_object_ref_wire *ref_out)
{
    if (!record || !record->in_use || !ref_out ||
        record->object_backing_len == 0) {
        return -1;
    }
    memset(ref_out, 0, sizeof(*ref_out));
    ref_out->magic = LINGQU_OBMM_OBJECT_REF_MAGIC;
    ref_out->layout_version = LINGQU_OBMM_OBJECT_REF_LAYOUT_VERSION;
    ref_out->object_kind = (uint16_t)record->object_payload_kind;
    ref_out->state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    ref_out->owner_entity = record->object_owner_node;
    ref_out->producer_entity = record->object_owner_node;
    ref_out->object_version = record->version;
    ref_out->key_hash =
        mem_service_checksum_bytes((const uint8_t *)record->key,
                             (uint64_t)strnlen(record->key,
                                               sizeof(record->key)));
    ref_out->payload_offset = record->object_backing_offset;
    ref_out->payload_bytes = record->object_backing_len;
    ref_out->payload_checksum = record->object_payload_checksum;
    return 0;
}
