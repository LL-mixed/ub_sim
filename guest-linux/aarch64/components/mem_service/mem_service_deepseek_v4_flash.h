#ifndef MEM_SERVICE_DEEPSEEK_V4_FLASH_H
#define MEM_SERVICE_DEEPSEEK_V4_FLASH_H

/*
 * DeepSeek V4 Flash geometry helper for clients that want to build a
 * mem_service OBMM range-flow request. This is not a mem_service global model
 * selector; callers compute the request and pass it to the infrastructure.
 */

#include <stdint.h>

#include "mem_service_profile.h"

uint32_t mem_service_deepseek_v4_flash_layer_count(void);
uint64_t mem_service_deepseek_v4_flash_hidden_range_bytes(void);
uint64_t mem_service_deepseek_v4_flash_decode_hidden_bytes(void);
uint64_t mem_service_deepseek_v4_flash_vocab_size(void);
uint64_t mem_service_deepseek_v4_flash_handoff_hidden_bytes(uint64_t decode_step);
const char *mem_service_deepseek_v4_flash_model_key(void);
uint64_t mem_service_deepseek_v4_flash_range_kv_state_bytes(uint32_t layer_start,
                                                            uint32_t layer_end);
int mem_service_deepseek_v4_flash_layer_range_for_node(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint32_t *layer_start_out,
    uint32_t *layer_end_out,
    uint32_t *next_node_out);
int mem_service_deepseek_v4_flash_init_obmm_range_flow_request(
    struct mem_service_obmm_range_flow_request *req,
    uint32_t local_node,
    uint32_t cluster_node_count);

#endif
