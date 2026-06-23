#ifndef MEM_SERVICE_QWEN3_RUNTIME_H
#define MEM_SERVICE_QWEN3_RUNTIME_H

#include "mem_service.h"
#include "mem_service_guest_runtime.h"
#include "mem_service_qwen3_placement.h"

#include <stdbool.h>
#include <stdint.h>

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
