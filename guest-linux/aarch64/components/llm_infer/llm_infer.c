#include "llm_infer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define LLM_INFER_QWEN3_DEFAULT_PIPELINE_NODES 8ULL
#define LLM_INFER_QWEN3_DEFAULT_TOTAL_LAYERS 28ULL
#define LLM_INFER_QWEN3_DEFAULT_VOCAB_SIZE 151936ULL
#define LLM_INFER_QWEN3_DEFAULT_MODEL_ID "Qwen/Qwen3-0.6B"
#define LLM_INFER_QWEN3_DEFAULT_HIDDEN_RANGE_BYTES 262144ULL
#define LLM_INFER_QWEN3_DEFAULT_HIDDEN_SIZE 1024ULL
#define LLM_INFER_QWEN3_DEFAULT_DECODE_TOKENS 1ULL

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
