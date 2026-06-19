#include "mem_service_qwen3.h"

#include <limits.h>
#include <stddef.h>

#include "components/llm_infer/llm_infer.h"

uint32_t mem_service_qwen3_layer_count(void)
{
    uint64_t value = llm_infer_qwen3_total_layers();

    return value > UINT32_MAX ? 28U : (uint32_t)value;
}

uint32_t mem_service_qwen3_range_nodes(void)
{
    uint64_t value = llm_infer_qwen3_pipeline_nodes();

    return value == 0 || value > UINT32_MAX ? 8U : (uint32_t)value;
}

uint64_t mem_service_qwen3_hidden_range_bytes(void)
{
    return llm_infer_qwen3_hidden_range_bytes();
}

uint64_t mem_service_qwen3_handoff_hidden_bytes(uint64_t decode_step)
{
    return llm_infer_qwen3_handoff_hidden_bytes(decode_step);
}

const char *mem_service_qwen3_model_key(void)
{
    return llm_infer_qwen3_model_key();
}

uint64_t mem_service_qwen3_range_kv_state_bytes(uint32_t layer_start,
                                                uint32_t layer_end,
                                                uint64_t token_count)
{
    uint64_t bytes_per_token =
        llm_infer_qwen3_range_kv_state_bytes(layer_start, layer_end);

    if (bytes_per_token == 0) {
        return 0;
    }
    return bytes_per_token * token_count;
}

int mem_service_qwen3_layer_range_for_node(uint32_t local_node,
                                           uint32_t cluster_node_count,
                                           uint32_t *layer_start_out,
                                           uint32_t *layer_end_out,
                                           uint32_t *next_node_out)
{
    if (cluster_node_count != mem_service_qwen3_range_nodes()) {
        return -1;
    }
    return llm_infer_qwen3_layer_range_for_node(local_node,
                                                cluster_node_count,
                                                layer_start_out,
                                                layer_end_out,
                                                next_node_out);
}

void mem_service_qwen3_node_range(uint32_t node,
                                  uint32_t node_count,
                                  uint32_t *start_out,
                                  uint32_t *end_out)
{
    uint32_t next_node = 0;

    if (!start_out || !end_out ||
        mem_service_qwen3_layer_range_for_node(node,
                                               node_count,
                                               start_out,
                                               end_out,
                                               &next_node) != 0) {
        if (start_out) {
            *start_out = 0;
        }
        if (end_out) {
            *end_out = 0;
        }
    }
}
