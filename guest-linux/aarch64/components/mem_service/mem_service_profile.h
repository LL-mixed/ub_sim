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
struct mem_service_object_payload_view;
struct mem_service_record;
struct mem_service_scheduler_work_item;

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

int mem_service_range_flow_wait_runtime_input_view(
    const struct mem_service_obmm_range_flow_request *request,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_object_payload_view *view_out);
int mem_service_range_flow_wait_scheduler_work_item(
    const struct mem_service_obmm_range_flow_request *request,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_scheduler_work_item *item_out);
int mem_service_range_flow_publish_runtime_output(
    struct mem_service *svc,
    const struct mem_service_obmm_range_flow_request *request,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    const uint8_t *payload,
    uint64_t payload_len,
    uint64_t expected_checksum,
    const uint8_t *kv_payload,
    uint64_t kv_payload_len,
    uint64_t expected_kv_checksum);
int mem_service_range_flow_publish_terminal_token(
    struct mem_service *svc,
    const struct mem_service_obmm_range_flow_request *request,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    uint64_t sampled_token,
    uint64_t runner_up_token,
    uint64_t margin_milli,
    uint64_t logits_checksum,
    uint64_t text_checksum,
    uint64_t piece_word0,
    uint64_t piece_word1);

#endif
