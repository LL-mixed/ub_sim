#include "mem_service_ub_ssd_gsva_backend.h"

#include <limits.h>
#include <string.h>

#define MEM_SERVICE_UB_SSD_GSVA_KEY_VERSION 1U
#define MEM_SERVICE_UB_SSD_GSVA_KEY_EPOCH 1U
#define MEM_SERVICE_UB_SSD_GSVA_DIRECTORY_MESI 4U

static int mem_service_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (!out || a > UINT64_MAX - b) {
        return -1;
    }
    *out = a + b;
    return 0;
}

int mem_service_make_ub_ssd_gsva_buffer_desc_from_source(
    const struct mem_service_ub_ssd_gsva_desc_source *source,
    const struct mem_service_record *record,
    struct mem_service_ub_ssd_gsva_buffer_desc *out)
{
    const struct mem_service_ub_ssd_gsva_meta *meta;
    uint64_t payload_gsva_base;
    uint64_t gsva_base;
    uint64_t payload_bytes;
    uint32_t owner;

    if (!source || !record || !out || !source->active || !record->in_use ||
        record->object_backing_len == 0 || source->node_count <= 0 ||
        source->node_count > (int)MEM_SERVICE_UB_SSD_GSVA_MAX_NODES ||
        source->local_idx < 0 || source->local_idx >= source->node_count) {
        return -1;
    }
    owner = record->object_owner_node;
    if (owner >= (uint32_t)source->node_count) {
        return -1;
    }
    meta = &source->metas[owner];
    if (meta->export_mem_id == 0 || meta->remote_uba == 0 ||
        meta->size == 0 || meta->token_id == 0 || meta->export_cna == 0 ||
        source->payload_offset >= meta->size) {
        return -1;
    }
    payload_bytes = meta->size - source->payload_offset;
    if (record->object_backing_offset > payload_bytes ||
        record->object_backing_len > payload_bytes - record->object_backing_offset) {
        return -1;
    }
    if (mem_service_add_u64(meta->remote_uba,
                            source->payload_offset,
                            &payload_gsva_base) != 0 ||
        mem_service_add_u64(payload_gsva_base,
                            record->object_backing_offset,
                            &gsva_base) != 0) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->gsva_base = gsva_base;
    out->bytes = record->object_backing_len;
    out->key_version = MEM_SERVICE_UB_SSD_GSVA_KEY_VERSION;
    out->key_segment_id = meta->export_mem_id;
    out->key_home_va = meta->remote_uba;
    out->key_size = meta->size;
    out->key_p_tag = meta->export_cna & 0x00ffffffu;
    out->key_cache_policy = MEM_SERVICE_UB_SSD_GSVA_DIRECTORY_MESI;
    out->key_epoch = MEM_SERVICE_UB_SSD_GSVA_KEY_EPOCH;
    out->token_id = meta->token_id;
    out->token_value = meta->token_id;
    out->owner_node = owner;
    out->source_cna = source->local_cna;
    return 0;
}

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
