#ifndef MEM_SERVICE_UB_SSD_GSVA_BACKEND_H
#define MEM_SERVICE_UB_SSD_GSVA_BACKEND_H

#include "mem_service_gsva_access.h"
#include "mem_service.h"

#include <stdint.h>

struct mem_service_ub_ssd_gsva_block_ref {
    uint64_t block_hi;
    uint64_t block_lo;
    uint64_t version;
    uint64_t offset;
    uint64_t bytes;
    uint64_t checksum64;
};

int mem_service_record_attach_ub_ssd_gsva_backend_ref(
    struct mem_service_record *record,
    uint32_t backend_node,
    uint32_t backend_device_cna,
    uint32_t backend_flags,
    const struct mem_service_ub_ssd_gsva_block_ref *block_ref,
    bool make_primary_payload);

#endif
