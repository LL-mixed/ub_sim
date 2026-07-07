#include "mem_service_ub_ssd_gsva_backend.h"

#include <string.h>

int mem_service_record_attach_ub_ssd_gsva_backend_ref(
    struct mem_service_record *record,
    uint32_t backend_node,
    uint32_t backend_device_cna,
    uint32_t backend_flags,
    const struct mem_service_ub_ssd_gsva_block_ref *block_ref,
    bool make_primary_payload)
{
    if (!record || !block_ref || block_ref->bytes == 0) {
        return -1;
    }
    record->object_backend_kind = MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA;
    record->object_backend_node = backend_node;
    record->object_backend_device_cna = backend_device_cna;
    record->object_backend_flags = backend_flags;
    record->object_backend_block_hi = block_ref->block_hi;
    record->object_backend_block_lo = block_ref->block_lo;
    record->object_backend_block_version = block_ref->version;
    record->object_backend_block_offset = block_ref->offset;
    record->object_backend_block_bytes = block_ref->bytes;
    record->object_backend_block_checksum = block_ref->checksum64;
    if (make_primary_payload) {
        record->object_payload_kind = MEM_SERVICE_PAYLOAD_KIND_UB_SSD_GSVA_BLOCK;
        record->object_backing_offset = block_ref->offset;
        record->object_backing_len = block_ref->bytes;
        record->object_payload_checksum = block_ref->checksum64;
    }
    return 0;
}
