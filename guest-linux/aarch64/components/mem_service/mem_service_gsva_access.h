#ifndef MEM_SERVICE_GSVA_ACCESS_H
#define MEM_SERVICE_GSVA_ACCESS_H

#include "mem_service.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define MEM_SERVICE_GSVA_MAX_NODES 8U
#define MEM_SERVICE_GSVA_KEY_VERSION 1U
#define MEM_SERVICE_GSVA_KEY_EPOCH 1U
#define MEM_SERVICE_GSVA_DIRECTORY_MESI 4U

struct mem_service_gsva_region_meta {
    uint64_t segment_id;
    uint64_t home_va;
    uint64_t region_bytes;
    uint32_t token_id;
    uint32_t home_cna;
};

struct mem_service_gsva_desc_source {
    bool active;
    int node_count;
    int local_idx;
    uint32_t local_cna;
    uint64_t payload_offset;
    struct mem_service_gsva_region_meta metas[MEM_SERVICE_GSVA_MAX_NODES];
};

struct mem_service_gsva_buffer_desc {
    uint64_t gsva_base;
    uint64_t bytes;
    uint32_t key_version;
    uint32_t key_flags;
    uint64_t key_segment_id;
    uint64_t key_home_va;
    uint64_t key_size;
    uint64_t key_vmid;
    uint64_t key_asid;
    uint64_t key_pte_offset;
    uint32_t key_p_tag;
    uint32_t key_cache_policy;
    uint64_t key_epoch;
    uint32_t token_id;
    uint32_t token_value;
    uint32_t owner_node;
    uint32_t source_cna;
};

static inline int mem_service_gsva_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (!out || a > UINT64_MAX - b) {
        return -1;
    }
    *out = a + b;
    return 0;
}

static inline int mem_service_make_gsva_buffer_desc_from_source(
    const struct mem_service_gsva_desc_source *source,
    const struct mem_service_record *record,
    struct mem_service_gsva_buffer_desc *out)
{
    const struct mem_service_gsva_region_meta *meta;
    uint64_t payload_gsva_base;
    uint64_t gsva_base;
    uint64_t payload_bytes;
    uint32_t owner;

    if (!source || !record || !out || !source->active || !record->in_use ||
        record->object_backing_len == 0 || source->node_count <= 0 ||
        source->node_count > (int)MEM_SERVICE_GSVA_MAX_NODES ||
        source->local_idx < 0 || source->local_idx >= source->node_count) {
        return -1;
    }
    owner = record->object_owner_node;
    if (owner >= (uint32_t)source->node_count) {
        return -1;
    }
    meta = &source->metas[owner];
    if (meta->segment_id == 0 || meta->home_va == 0 ||
        meta->region_bytes == 0 || meta->token_id == 0 || meta->home_cna == 0 ||
        source->payload_offset >= meta->region_bytes) {
        return -1;
    }
    payload_bytes = meta->region_bytes - source->payload_offset;
    if (record->object_backing_offset > payload_bytes ||
        record->object_backing_len > payload_bytes - record->object_backing_offset) {
        return -1;
    }
    if (mem_service_gsva_add_u64(meta->home_va,
                                 source->payload_offset,
                                 &payload_gsva_base) != 0 ||
        mem_service_gsva_add_u64(payload_gsva_base,
                                 record->object_backing_offset,
                                 &gsva_base) != 0) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->gsva_base = gsva_base;
    out->bytes = record->object_backing_len;
    out->key_version = MEM_SERVICE_GSVA_KEY_VERSION;
    out->key_segment_id = meta->segment_id;
    out->key_home_va = meta->home_va;
    out->key_size = meta->region_bytes;
    out->key_p_tag = meta->home_cna & 0x00ffffffu;
    out->key_cache_policy = MEM_SERVICE_GSVA_DIRECTORY_MESI;
    out->key_epoch = MEM_SERVICE_GSVA_KEY_EPOCH;
    out->token_id = meta->token_id;
    out->token_value = meta->token_id;
    out->owner_node = owner;
    out->source_cna = source->local_cna;
    return 0;
}

#endif
