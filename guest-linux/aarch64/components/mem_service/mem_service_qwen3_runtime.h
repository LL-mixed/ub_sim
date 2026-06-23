#ifndef MEM_SERVICE_QWEN3_RUNTIME_H
#define MEM_SERVICE_QWEN3_RUNTIME_H

#include "mem_service.h"
#include "mem_service_guest_runtime.h"
#include "mem_service_qwen3_placement.h"

#include <stdbool.h>
#include <stdint.h>

uint64_t mem_service_qwen3_hidden_payload_checksum(const uint8_t *bytes,
                                                   uint64_t len);
int mem_service_qwen3_kv_state_alloc(struct mem_service_cluster_runtime *rt,
                                     uint64_t payload_len,
                                     uint64_t *offset_out,
                                     uint64_t *block_bytes_out,
                                     uint64_t *block_count_out,
                                     uint64_t *reserved_bytes_out);
int mem_service_qwen3_decode_entry_node(uint32_t cluster_node_count,
                                        uint32_t *node_out);
int mem_service_publish_qwen3_layer_range_placements(
    struct mem_service *svc,
    uint32_t node_count);
bool mem_service_read_qwen3_layer_range_placement(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_qwen3_layer_range_placement *placement_out);
bool mem_service_find_qwen3_layer_range_predecessor(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_qwen3_layer_range_placement *placement_out);
void mem_service_report_obmm_pool_layout_once(struct mem_service_cluster_runtime *rt);
void mem_service_report_obmm_pool_usage(struct mem_service_cluster_runtime *rt,
                                        uint32_t local_node,
                                        uint64_t decode_step);

#endif
