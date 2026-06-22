#ifndef MEM_SERVICE_CLUSTER_PAYLOAD_CONTRACT_H
#define MEM_SERVICE_CLUSTER_PAYLOAD_CONTRACT_H

#include <stdint.h>

#include "mem_service.h"

#define MEM_SERVICE_CLUSTER_MAX_RECORDS 1024

#define MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC 0x57344450U
#define MEM_SERVICE_CLUSTER_PAYLOAD_VERSION 1U

#define MEM_SERVICE_COMPACT_PREFIX_STATE_READY 0x0001U
#define MEM_SERVICE_COMPACT_PREFIX_VIEW_READY 0x0002U

struct mem_service_cluster_payload {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t publish_seq;
    uint32_t publish_done_seq;
    uint8_t record_pad[48];
    struct mem_service_record records[MEM_SERVICE_CLUSTER_MAX_RECORDS];
};

struct mem_service_cluster_payload_header {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t publish_seq;
    uint32_t publish_done_seq;
};

struct mem_service_cluster_payload_compact_summary {
    uint16_t record_count;
    uint16_t prefix_count;
    uint16_t block_count;
    uint16_t group_count;
    uint16_t weight_tile_count;
    uint16_t kvcache_object_count;
    uint16_t flags;
    uint16_t hidden_range_count;
    uint64_t block_version_floor;
    uint64_t block_result_floor;
    uint64_t prefix_version_floor;
    uint64_t prefix_result_floor;
};

#endif
