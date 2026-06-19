#include "llm_infer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define LLM_INFER_QWEN3_DEFAULT_PIPELINE_NODES 8ULL
#define LLM_INFER_QWEN3_DEFAULT_TOTAL_LAYERS 28ULL
#define LLM_INFER_QWEN3_DEFAULT_VOCAB_SIZE 151936ULL
#define LLM_INFER_QWEN3_DEFAULT_MODEL_ID "Qwen/Qwen3-0.6B"
#define LLM_INFER_QWEN3_DEFAULT_MODEL_KEY "qwen3-0.6b"
#define LLM_INFER_QWEN3_DEFAULT_HIDDEN_RANGE_BYTES 262144ULL
#define LLM_INFER_QWEN3_DEFAULT_HIDDEN_SIZE 1024ULL
#define LLM_INFER_QWEN3_DEFAULT_DECODE_TOKENS 1ULL
#define LLM_INFER_QWEN3_DEFAULT_KV_HEADS 8ULL
#define LLM_INFER_QWEN3_DEFAULT_HEAD_DIM 128ULL
#define LLM_INFER_QWEN3_DEFAULT_KV_STREAMS 2ULL
#define LLM_INFER_QWEN3_DEFAULT_KV_ELEM_BYTES 4ULL

static uint64_t llm_infer_env_u64_or(const char *name, uint64_t fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (!value || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return fallback;
    }
    return (uint64_t)parsed;
}

uint64_t llm_infer_qwen3_pipeline_nodes(void)
{
    return llm_infer_env_u64_or("SIM_QWEN3_DENSE_TP_NODES",
                                LLM_INFER_QWEN3_DEFAULT_PIPELINE_NODES);
}

uint64_t llm_infer_qwen3_total_layers(void)
{
    return llm_infer_env_u64_or("SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS",
                                LLM_INFER_QWEN3_DEFAULT_TOTAL_LAYERS);
}

uint64_t llm_infer_qwen3_vocab_size(void)
{
    return llm_infer_env_u64_or("SIM_QWEN3_DENSE_VOCAB_SIZE",
                                LLM_INFER_QWEN3_DEFAULT_VOCAB_SIZE);
}

const char *llm_infer_qwen3_model_id(void)
{
    const char *model_id = getenv("SIM_QWEN3_DENSE_MODEL_ID");

    return model_id && model_id[0] != '\0' ?
        model_id : LLM_INFER_QWEN3_DEFAULT_MODEL_ID;
}

const char *llm_infer_qwen3_model_key(void)
{
    const char *model_key = getenv("SIM_QWEN3_DENSE_MODEL_KEY");

    return model_key && model_key[0] != '\0' ?
        model_key : LLM_INFER_QWEN3_DEFAULT_MODEL_KEY;
}

uint64_t llm_infer_qwen3_hidden_range_bytes(void)
{
    return llm_infer_env_u64_or("SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES",
                                LLM_INFER_QWEN3_DEFAULT_HIDDEN_RANGE_BYTES);
}

uint64_t llm_infer_qwen3_decode_hidden_bytes(void)
{
    uint64_t hidden_size =
        llm_infer_env_u64_or("SIM_QWEN3_DENSE_HIDDEN_SIZE",
                             LLM_INFER_QWEN3_DEFAULT_HIDDEN_SIZE);
    uint64_t decode_tokens =
        llm_infer_env_u64_or("SIM_QWEN3_DENSE_DECODE_TOKENS",
                             LLM_INFER_QWEN3_DEFAULT_DECODE_TOKENS);

    return llm_infer_env_u64_or("SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES",
                                hidden_size * decode_tokens * 2ULL);
}

uint64_t llm_infer_qwen3_handoff_hidden_bytes(uint64_t decode_step)
{
    return decode_step > 0 ?
        llm_infer_qwen3_decode_hidden_bytes() :
        llm_infer_qwen3_hidden_range_bytes();
}

uint64_t llm_infer_qwen3_kv_heads(void)
{
    return llm_infer_env_u64_or(
        "SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS",
        llm_infer_env_u64_or("SIM_QWEN3_DENSE_KV_HEADS",
                             LLM_INFER_QWEN3_DEFAULT_KV_HEADS));
}

uint64_t llm_infer_qwen3_head_dim(void)
{
    return llm_infer_env_u64_or("SIM_QWEN3_DENSE_HEAD_DIM",
                                LLM_INFER_QWEN3_DEFAULT_HEAD_DIM);
}

uint64_t llm_infer_qwen3_kv_streams(void)
{
    return llm_infer_env_u64_or("SIM_QWEN3_DENSE_KV_STREAMS",
                                LLM_INFER_QWEN3_DEFAULT_KV_STREAMS);
}

uint64_t llm_infer_qwen3_kv_elem_bytes(void)
{
    return llm_infer_env_u64_or("SIM_QWEN3_DENSE_KV_ELEM_BYTES",
                                LLM_INFER_QWEN3_DEFAULT_KV_ELEM_BYTES);
}

uint64_t llm_infer_qwen3_range_kv_state_bytes(uint32_t layer_start,
                                              uint32_t layer_end)
{
    uint64_t bytes_per_token_per_layer;

    if (layer_end <= layer_start ||
        layer_end > llm_infer_qwen3_total_layers()) {
        return 0;
    }

    bytes_per_token_per_layer = llm_infer_qwen3_kv_heads() *
                                llm_infer_qwen3_head_dim() *
                                llm_infer_qwen3_kv_streams() *
                                llm_infer_qwen3_kv_elem_bytes();
    return ((uint64_t)(layer_end - layer_start)) * bytes_per_token_per_layer;
}

int llm_infer_qwen3_layer_range_for_node(uint32_t local_node,
                                         uint32_t cluster_node_count,
                                         uint32_t *layer_start_out,
                                         uint32_t *layer_end_out,
                                         uint32_t *next_node_out)
{
    uint32_t layer_count = (uint32_t)llm_infer_qwen3_total_layers();
    uint32_t base;
    uint32_t rem;
    uint32_t idx;
    uint32_t start;
    uint32_t count;

    if (cluster_node_count == 0 || local_node >= cluster_node_count ||
        layer_start_out == NULL ||
        layer_end_out == NULL || next_node_out == NULL) {
        return -1;
    }

    base = layer_count / cluster_node_count;
    rem = layer_count % cluster_node_count;
    idx = local_node;
    start = idx * base + (idx < rem ? idx : rem);
    count = base + (idx < rem ? 1U : 0U);

    *layer_start_out = start;
    *layer_end_out = start + count;
    *next_node_out = (local_node + 1U) % cluster_node_count;
    return 0;
}

bool llm_infer_is_qwen3_profile_name(const char *profile)
{
    return profile &&
        (strcmp(profile, "qwen3_dense_reference") == 0 ||
         strcmp(profile, "qwen3_dense") == 0);
}

bool llm_infer_qwen3_real_tokenizer_required(void)
{
    const char *profile = getenv("SIM_UAPI_W4_CHIPBACKEND_PROFILE");
    const char *weights_path = getenv("SIM_QWEN3_DENSE_WEIGHTS_PATH");

    return llm_infer_is_qwen3_profile_name(profile) &&
        weights_path && weights_path[0] != '\0';
}
