#include "llm_infer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int llm_infer_managed_object_key(char *out, size_t size, uint64_t run,
    uint64_t request, uint32_t producer, uint64_t step)
{
    char key[65];
    if (!out || size < sizeof(key) || !run || !request) return -EINVAL;
    int length = snprintf(key, sizeof(key), "w5v1/%016" PRIx64 "/%016" PRIx64
        "/%08" PRIx32 "/%016" PRIx64, run, request, producer, step);
    if (length != 64) return -EOVERFLOW;
    memcpy(out, key, sizeof(key));
    return 0;
}

static uint64_t result_word(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static uint64_t result_checksum(const unsigned char *bytes, size_t length,
                                bool positional)
{
    uint64_t value = positional ? UINT64_C(0xcbf29ce484222325) :
                                  UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; ++i) {
        value ^= bytes[i] | (positional ? (uint64_t)i << 8 : 0);
        value *= UINT64_C(1099511628211);
    }
    return value;
}

int llm_infer_decode_range_result(const void *result, size_t length,
    uint64_t allocation_offset, const struct llm_infer_range_identity *expected,
    struct llm_infer_range_result *out)
{
    /* Rust's result-block metadata layout is fixed independently of the
     * request's pipeline width. This is a wire offset, not an eight-node gate. */
    const size_t metadata = 39424, header_bytes = 64, entry_bytes = 144;
    const unsigned char *bytes = result;
    struct llm_infer_range_result decoded = {0};
    uint64_t entry[18];
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!bytes || !expected || length < metadata + header_bytes ||
        length > 64u * 1024u * 1024u || allocation_offset > UINT64_MAX - length ||
        !expected->pipeline_nodes || expected->node >= expected->pipeline_nodes ||
        expected->next_node >= expected->pipeline_nodes ||
        expected->layer_start >= expected->layer_end ||
        expected->layer_end > expected->total_layers || !expected->hidden_bytes)
        return -EINVAL;
    uint64_t header = result_word(bytes + metadata + 40);
    if (header < metadata + header_bytes || header % 8 ||
        header > length - header_bytes) return -EPROTO;
    const unsigned char *table = bytes + (size_t)header;
    uint64_t table_bytes = result_word(table + 24);
    if (result_word(table) != UINT64_C(0x7133773472667430) ||
        result_word(table + 8) != 1 || result_word(table + 16) != 18 ||
        table_bytes < entry_bytes || table_bytes > length - header - header_bytes)
        return -EPROTO;
    const unsigned char *record = table + header_bytes;
    for (unsigned i = 0; i < 18; ++i) entry[i] = result_word(record + i * 8);
    if (result_checksum(record, entry_bytes, true) != result_word(table + 32) ||
        entry[0] != expected->node || entry[1] != expected->layer_start ||
        entry[2] != expected->layer_end ||
        entry[3] != expected->layer_end - expected->layer_start ||
        entry[4] != expected->next_node || entry[5] != expected->pipeline_nodes ||
        entry[6] != expected->total_layers || entry[7] != expected->hidden_bytes ||
        entry[11] != entry[3] || entry[14] != expected->hidden_bytes ||
        entry[15] != expected->hidden_bytes || !entry[8] || !entry[9] || !entry[10] ||
        !entry[12] || !entry[13] ||
        entry[8] != result_word(table + 48) || entry[9] != result_word(table + 56) ||
        entry[10] != result_word(table + 40) ||
        entry[15] > table_bytes - entry_bytes ||
        entry[16] != table_bytes - entry_bytes - entry[15]) return -EPROTO;
    size_t hidden_offset = (size_t)header + header_bytes + entry_bytes;
    size_t kv_offset = hidden_offset + (size_t)entry[15];
    uint64_t hidden_checksum = result_checksum(bytes + hidden_offset, entry[15], true);
    uint64_t kv_checksum = entry[16] ? result_checksum(bytes + kv_offset, entry[16], true) : 0;
    /* Numerical layer/tensor checksums depend on the producer's execution
     * path; do not equate them with each other or the serialized payload.
     * Whole-result integrity is the caller's provider/completion obligation;
     * KV has an explicit serialized checksum. */
    if (kv_checksum != entry[17]) return -EBADMSG;
    decoded.hidden = (struct llm_infer_result_view) {
        allocation_offset + hidden_offset, entry[15], hidden_checksum,
        result_checksum(bytes + hidden_offset, entry[15], false),
    };
    if (entry[16]) decoded.kv = (struct llm_infer_result_view) {
        allocation_offset + kv_offset, entry[16], kv_checksum,
        result_checksum(bytes + kv_offset, entry[16], false),
    };
    decoded.input_checksum = entry[8];
    decoded.output_tensor_checksum = entry[9];
    decoded.range_checksum = entry[10];
    decoded.real_layers = entry[11];
    *out = decoded;
    return 0;
}

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
    uint64_t tensor_shards = llm_infer_env_u64_or("SIM_QWEN3_DENSE_TP_NODES",
        LLM_INFER_QWEN3_DEFAULT_PIPELINE_NODES);
    return llm_infer_env_u64_or("SIM_W5_CLUSTER_NODE_COUNT", tensor_shards);
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
