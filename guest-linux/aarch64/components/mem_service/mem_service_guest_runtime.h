#ifndef MEM_SERVICE_GUEST_RUNTIME_H
#define MEM_SERVICE_GUEST_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/obmm_common.h"
#include "kernel_ub/include/uapi/ub/obmm.h"
#include "libs/obmm_queue/obmm_queue_types.h"
#include "libs/obmm_queue/obmm_spsc_queue.h"
#include "mem_service_cluster_payload_contract.h"

#define MEM_SERVICE_CLUSTER_MAX_NODES 8
#define MEM_SERVICE_DEFAULT_REGION_SIZE_MB 512
#define MEM_SERVICE_CMDLINE_REGION_SIZE "mem_service_region_size_mb"
#define MEM_SERVICE_CLUSTER_QUEUE_DEPTH 512
#define MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH 16
#define MEM_SERVICE_CLUSTER_IMPORT_ALIGN (2ULL * 1024ULL * 1024ULL)
#define MEM_SERVICE_CLUSTER_MAX_WINDOWS 16

struct mem_service_cluster_meta {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t token_id;
    uint32_t export_cna;
};

struct mem_service_mapped_region {
    int fd;
    void *addr;
    size_t len;
    uint64_t mem_id;
};

struct mem_service_cluster_slot {
    int owner_idx;
    int reader_idx;
    bool is_local;
    bool map_osync;
    uint32_t export_cna;
    uint64_t mem_id;
    uint64_t local_pa;
    struct mem_service_mapped_region region;
};

struct mem_service_cluster_runtime {
    bool active;
    bool lazy_remote_activation;
    int node_count;
    int local_idx;
    int obmm_fd;
    uint32_t local_cna;
    uint32_t publish_seq;
    uint16_t observe_epoch;
    uint64_t region_size;
    uint64_t payload_offset;
    uint64_t payload_arena_base;
    uint64_t payload_arena_next;
    uint64_t payload_arena_high_water;
    bool pool_layout_reported;
    struct mem_service_cluster_meta metas[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct mem_service_cluster_slot slots[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct obmm_spsc_queue *ingress_queues[MEM_SERVICE_CLUSTER_MAX_NODES];
    void *ingress_queue_base;
    struct obmm_spsc_queue *egress_queues[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct obmm_helpers_region egress_import[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct obmm_desc pending_descs[MEM_SERVICE_CLUSTER_MAX_NODES][MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH];
    uint8_t pending_desc_count[MEM_SERVICE_CLUSTER_MAX_NODES];
};

#endif
