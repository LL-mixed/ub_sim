#ifndef MEM_SERVICE_PROFILE_H
#define MEM_SERVICE_PROFILE_H

/*
 * Range-flow request contract.
 *
 * mem_service is infrastructure: it stores, validates, routes, and observes
 * objects. Model/runtime code computes model geometry and passes the concrete
 * object-flow request here. There is no global active model in mem_service.
 */

#include <stdbool.h>
#include <stdint.h>

struct mem_service;
struct mem_service_record;

typedef struct mem_service_record *(*mem_service_record_recycler_fn)(
    struct mem_service *svc,
    const char *incoming_key);

typedef int (*mem_service_layer_range_for_node_fn)(uint32_t local_node,
                                                   uint32_t cluster_node_count,
                                                   uint32_t *layer_start_out,
                                                   uint32_t *layer_end_out,
                                                   uint32_t *next_node_out);

struct mem_service_layer_range_placement {
    uint32_t owner_node;
    uint32_t layer_start;
    uint32_t layer_end;
    uint32_t next_owner_node;
    uint32_t layer_count;
    bool terminal;
};

struct mem_service_obmm_range_flow_request {
    const char *model_key;
    uint32_t total_layers;
    uint32_t range_nodes;
    uint64_t hidden_range_bytes;
    uint64_t kv_state_bytes;
    struct mem_service_layer_range_placement local_placement;
    struct mem_service_layer_range_placement next_placement;
    bool has_predecessor;
    struct mem_service_layer_range_placement predecessor_placement;
    mem_service_record_recycler_fn recycle_runtime_record;
};

int mem_service_init_obmm_range_flow_request(
    struct mem_service_obmm_range_flow_request *req,
    const char *model_key,
    uint32_t total_layers,
    uint32_t range_nodes,
    uint64_t hidden_range_bytes,
    uint64_t kv_state_bytes,
    uint32_t local_node,
    mem_service_layer_range_for_node_fn layer_range_for_node,
    mem_service_record_recycler_fn recycle_runtime_record);

#endif
