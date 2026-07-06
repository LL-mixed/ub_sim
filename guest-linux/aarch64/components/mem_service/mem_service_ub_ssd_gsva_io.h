#ifndef MEM_SERVICE_UB_SSD_GSVA_IO_H
#define MEM_SERVICE_UB_SSD_GSVA_IO_H

#include "mem_service_ub_ssd_gsva_backend.h"

#include <stdint.h>

#define MEM_SERVICE_UB_SSD_GSVA_DEFAULT_DEVICE "/dev/ub_ssd0"
#define MEM_SERVICE_UB_SSD_GSVA_OP_BLOCK_WRITE 1U
#define MEM_SERVICE_UB_SSD_GSVA_OP_BLOCK_READ 2U

enum mem_service_ub_ssd_gsva_io_status {
    MEM_SERVICE_UB_SSD_GSVA_IO_OK = 0,
    MEM_SERVICE_UB_SSD_GSVA_IO_UNSUPPORTED = 1,
    MEM_SERVICE_UB_SSD_GSVA_IO_INVALID = 2,
    MEM_SERVICE_UB_SSD_GSVA_IO_STALE_REF = 3,
    MEM_SERVICE_UB_SSD_GSVA_IO_TIMEOUT = 4,
    MEM_SERVICE_UB_SSD_GSVA_IO_CHECKSUM_MISMATCH = 5,
    MEM_SERVICE_UB_SSD_GSVA_IO_VERSION_CONFLICT = 6,
    MEM_SERVICE_UB_SSD_GSVA_IO_INTERNAL = 7,
};

struct mem_service_ub_ssd_gsva_block_ref {
    uint64_t block_hi;
    uint64_t block_lo;
    uint64_t version;
    uint64_t offset;
    uint64_t bytes;
    uint64_t checksum64;
};

struct mem_service_ub_ssd_gsva_io_request {
    const char *device_path;
    uint32_t opcode;
    uint64_t request_id;
    uint32_t source_cna;
    uint32_t target_ssd_cna;
    uint32_t flags;
    struct mem_service_ub_ssd_gsva_block_ref block_ref;
    struct mem_service_ub_ssd_gsva_buffer_desc buffer;
};

struct mem_service_ub_ssd_gsva_io_completion {
    int32_t device_status;
    uint64_t request_id;
    struct mem_service_ub_ssd_gsva_block_ref committed_ref;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t checksum64;
    uint64_t error_detail;
};

enum mem_service_ub_ssd_gsva_io_status mem_service_ub_ssd_gsva_submit(
    const struct mem_service_ub_ssd_gsva_io_request *request,
    struct mem_service_ub_ssd_gsva_io_completion *completion);

#endif
