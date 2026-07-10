#include "mem_service_deepseek_v4_flash.h"

#include <limits.h>
#include <stddef.h>

/*
 * DeepSeek V4 Flash geometry constants. Model/runtime code owns these values
 * and passes the resulting request into mem_service; mem_service itself does
 * not select this model.
 */
#define DEEPSEEK_V4_FLASH_TOTAL_LAYERS 43U
#define DEEPSEEK_V4_FLASH_HIDDEN_SIZE 4096ULL
#define DEEPSEEK_V4_FLASH_VOCAB_SIZE 129280ULL
#define DEEPSEEK_V4_FLASH_PREFILL_TOKENS 128ULL
#define DEEPSEEK_V4_FLASH_DECODE_TOKENS 1ULL
#define DEEPSEEK_V4_FLASH_KV_HEADS 1ULL
#define DEEPSEEK_V4_FLASH_HEAD_DIM 512ULL
#define DEEPSEEK_V4_FLASH_KV_STREAMS 2ULL
#define DEEPSEEK_V4_FLASH_KV_ELEM_BYTES 4ULL
#define DEEPSEEK_V4_FLASH_MODEL_KEY "deepseek-v4-flash"

uint32_t mem_service_deepseek_v4_flash_layer_count(void)
{
    return DEEPSEEK_V4_FLASH_TOTAL_LAYERS;
}
uint64_t mem_service_deepseek_v4_flash_hidden_range_bytes(void)
{
    return DEEPSEEK_V4_FLASH_HIDDEN_SIZE *
           DEEPSEEK_V4_FLASH_PREFILL_TOKENS *
           2ULL;
}

uint64_t mem_service_deepseek_v4_flash_decode_hidden_bytes(void)
{
    return DEEPSEEK_V4_FLASH_HIDDEN_SIZE *
           DEEPSEEK_V4_FLASH_DECODE_TOKENS *
           2ULL;
}

uint64_t mem_service_deepseek_v4_flash_vocab_size(void)
{
    return DEEPSEEK_V4_FLASH_VOCAB_SIZE;
}

uint64_t mem_service_deepseek_v4_flash_handoff_hidden_bytes(uint64_t decode_step)
{
    return decode_step > 0
        ? mem_service_deepseek_v4_flash_decode_hidden_bytes()
        : mem_service_deepseek_v4_flash_hidden_range_bytes();
}

const char *mem_service_deepseek_v4_flash_model_key(void)
{
    return DEEPSEEK_V4_FLASH_MODEL_KEY;
}

uint64_t mem_service_deepseek_v4_flash_range_kv_state_bytes(uint32_t layer_start,
                                                            uint32_t layer_end)
{
    uint64_t bytes_per_layer;
    uint64_t layer_count;

    if (layer_end <= layer_start || layer_end > DEEPSEEK_V4_FLASH_TOTAL_LAYERS) {
        return 0;
    }
    bytes_per_layer = DEEPSEEK_V4_FLASH_KV_HEADS *
                      DEEPSEEK_V4_FLASH_HEAD_DIM *
                      DEEPSEEK_V4_FLASH_KV_STREAMS *
                      DEEPSEEK_V4_FLASH_KV_ELEM_BYTES;
    layer_count = (uint64_t)(layer_end - layer_start);
    return layer_count * bytes_per_layer;
}

int mem_service_deepseek_v4_flash_layer_range_for_node(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint32_t *layer_start_out,
    uint32_t *layer_end_out,
    uint32_t *next_node_out)
{
    uint32_t layer_count = DEEPSEEK_V4_FLASH_TOTAL_LAYERS;
    uint32_t base;
    uint32_t rem;
    uint32_t start;
    uint32_t count;

    if (cluster_node_count == 0 ||
        cluster_node_count > DEEPSEEK_V4_FLASH_TOTAL_LAYERS ||
        local_node >= cluster_node_count ||
        !layer_start_out || !layer_end_out || !next_node_out) {
        return -1;
    }
    base = layer_count / cluster_node_count;
    rem = layer_count % cluster_node_count;
    start = local_node * base + (local_node < rem ? local_node : rem);
    count = base + (local_node < rem ? 1U : 0U);
    *layer_start_out = start;
    *layer_end_out = start + count;
    *next_node_out = (local_node + 1U) % cluster_node_count;
    return 0;
}

int mem_service_deepseek_v4_flash_init_obmm_range_flow_request(
    struct mem_service_obmm_range_flow_request *req,
    uint32_t local_node,
    uint32_t cluster_node_count)
{
    uint32_t layer_start = 0;
    uint32_t layer_end = 0;
    uint32_t next_node = 0;

    if (mem_service_deepseek_v4_flash_layer_range_for_node(local_node,
                                                           cluster_node_count,
                                                           &layer_start,
                                                           &layer_end,
                                                           &next_node) != 0) {
        return -1;
    }
    return mem_service_init_obmm_range_flow_request(
        req,
        mem_service_deepseek_v4_flash_model_key(),
        mem_service_deepseek_v4_flash_layer_count(),
        cluster_node_count,
        mem_service_deepseek_v4_flash_hidden_range_bytes(),
        mem_service_deepseek_v4_flash_range_kv_state_bytes(layer_start, layer_end),
        local_node,
        mem_service_deepseek_v4_flash_layer_range_for_node,
        NULL);
}
