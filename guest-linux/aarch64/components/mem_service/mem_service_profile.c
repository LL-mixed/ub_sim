#include "mem_service_profile.h"

#include <string.h>

static int mem_service_copy_layer_range(struct mem_service_layer_range_placement *out,
                                        uint32_t owner_node,
                                        uint32_t layer_start,
                                        uint32_t layer_end,
                                        uint32_t next_owner_node,
                                        uint32_t range_nodes)
{
    if (!out || layer_end <= layer_start || owner_node >= range_nodes ||
        next_owner_node >= range_nodes) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->owner_node = owner_node;
    out->layer_start = layer_start;
    out->layer_end = layer_end;
    out->next_owner_node = next_owner_node;
    out->layer_count = layer_end - layer_start;
    out->terminal = next_owner_node == owner_node || next_owner_node == 0;
    return 0;
}
int mem_service_init_obmm_range_flow_request(
    struct mem_service_obmm_range_flow_request *req,
    const char *model_key,
    uint32_t total_layers,
    uint32_t range_nodes,
    uint64_t hidden_range_bytes,
    uint64_t kv_state_bytes,
    uint32_t local_node,
    mem_service_layer_range_for_node_fn layer_range_for_node,
    mem_service_record_recycler_fn recycle_runtime_record)
{
    uint32_t local_start = 0;
    uint32_t local_end = 0;
    uint32_t next_node = 0;
    uint32_t remote_start = 0;
    uint32_t remote_end = 0;
    uint32_t after_remote = 0;
    bool has_predecessor = false;
    struct mem_service_layer_range_placement predecessor;

    if (!req || !model_key || model_key[0] == '\0' || total_layers == 0 ||
        range_nodes == 0 || local_node >= range_nodes || hidden_range_bytes == 0 ||
        !layer_range_for_node) {
        return -1;
    }
    if (layer_range_for_node(local_node,
                             range_nodes,
                             &local_start,
                             &local_end,
                             &next_node) != 0 ||
        layer_range_for_node(next_node,
                             range_nodes,
                             &remote_start,
                             &remote_end,
                             &after_remote) != 0) {
        return -1;
    }
    if (local_end > total_layers || remote_end > total_layers ||
        mem_service_copy_layer_range(&req->local_placement,
                                     local_node,
                                     local_start,
                                     local_end,
                                     next_node,
                                     range_nodes) != 0 ||
        mem_service_copy_layer_range(&req->next_placement,
                                     next_node,
                                     remote_start,
                                     remote_end,
                                     after_remote,
                                     range_nodes) != 0) {
        return -1;
    }

    memset(&predecessor, 0, sizeof(predecessor));
    for (uint32_t node = 0; node < range_nodes; ++node) {
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t next = 0;

        if (node == local_node ||
            layer_range_for_node(node, range_nodes, &start, &end, &next) != 0) {
            continue;
        }
        if (next == local_node && end == local_start &&
            mem_service_copy_layer_range(&predecessor,
                                         node,
                                         start,
                                         end,
                                         next,
                                         range_nodes) == 0) {
            has_predecessor = true;
            break;
        }
    }
    if (local_start != 0 && !has_predecessor) {
        return -1;
    }

    req->model_key = model_key;
    req->total_layers = total_layers;
    req->range_nodes = range_nodes;
    req->hidden_range_bytes = hidden_range_bytes;
    req->kv_state_bytes = kv_state_bytes;
    req->has_predecessor = has_predecessor;
    req->predecessor_placement = predecessor;
    req->recycle_runtime_record = recycle_runtime_record;
    return 0;
}
