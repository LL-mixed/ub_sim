#ifndef MEM_SERVICE_UB_SSD_GSVA_BACKEND_H
#define MEM_SERVICE_UB_SSD_GSVA_BACKEND_H

#include "mem_service.h"

#include <stdint.h>

#define MEM_SERVICE_UB_SSD_GSVA_MAX_NODES 8U

struct mem_service_ub_ssd_gsva_meta {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t token_id;
    uint32_t export_cna;
};

struct mem_service_ub_ssd_gsva_desc_source {
    bool active;
    int node_count;
    int local_idx;
    uint32_t local_cna;
    uint64_t payload_offset;
    struct mem_service_ub_ssd_gsva_meta metas[MEM_SERVICE_UB_SSD_GSVA_MAX_NODES];
};

struct mem_service_ub_ssd_gsva_buffer_desc {
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

struct mem_service_ub_ssd_gsva_block_ref {
    uint64_t block_hi;
    uint64_t block_lo;
    uint64_t version;
    uint64_t offset;
    uint64_t bytes;
    uint64_t checksum64;
};

int mem_service_make_ub_ssd_gsva_buffer_desc_from_source(
    const struct mem_service_ub_ssd_gsva_desc_source *source,
    const struct mem_service_record *record,
    struct mem_service_ub_ssd_gsva_buffer_desc *out);

int mem_service_record_attach_ub_ssd_gsva_backend_ref(
    struct mem_service_record *record,
    uint32_t backend_node,
    uint32_t backend_device_cna,
    uint32_t backend_flags,
    const struct mem_service_ub_ssd_gsva_block_ref *block_ref,
    bool make_primary_payload);

#endif
