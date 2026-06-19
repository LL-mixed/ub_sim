#ifndef MEM_SERVICE_QWEN3_H
#define MEM_SERVICE_QWEN3_H

#include <stdint.h>

uint32_t mem_service_qwen3_layer_count(void);
uint32_t mem_service_qwen3_range_nodes(void);
uint64_t mem_service_qwen3_hidden_range_bytes(void);
uint64_t mem_service_qwen3_handoff_hidden_bytes(uint64_t decode_step);
const char *mem_service_qwen3_model_key(void);
uint64_t mem_service_qwen3_range_kv_state_bytes(uint32_t layer_start,
                                                uint32_t layer_end,
                                                uint64_t token_count);
int mem_service_qwen3_layer_range_for_node(uint32_t local_node,
                                           uint32_t cluster_node_count,
                                           uint32_t *layer_start_out,
                                           uint32_t *layer_end_out,
                                           uint32_t *next_node_out);
void mem_service_qwen3_node_range(uint32_t node,
                                  uint32_t node_count,
                                  uint32_t *start_out,
                                  uint32_t *end_out);

#endif
