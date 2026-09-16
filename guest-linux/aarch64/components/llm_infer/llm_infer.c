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

#define LLM_INFER_LOGITS_MARKER UINT64_C(0x713377346c6f6730)
#define LLM_INFER_TOKEN_TEXT_MARKER UINT64_C(0x7133773474787430)
#define LLM_INFER_LOGITS_HEADER_BYTES 64u
#define LLM_INFER_LOGITS_ENTRY_WORDS 45u
#define LLM_INFER_LOGITS_ENTRY_BYTES (LLM_INFER_LOGITS_ENTRY_WORDS * 8u)
#define LLM_INFER_TOKEN_TEXT_HEADER_BYTES 64u
#define LLM_INFER_TOKEN_TEXT_ENTRY_WORDS 8u
#define LLM_INFER_TOKEN_TEXT_ENTRY_BYTES (LLM_INFER_TOKEN_TEXT_ENTRY_WORDS * 8u)
#define LLM_INFER_MAX_LOGITS_ENTRIES 16u

static int llm_infer_decode_terminal_result(const unsigned char *bytes,
    size_t metadata_end, const struct llm_infer_range_identity *expected,
    struct llm_infer_terminal_result *out)
{
    struct llm_infer_terminal_result decoded = {0};
    unsigned matches = 0;
    if (!bytes || !expected || !out || !expected->vocab_size) return -EINVAL;
    for (size_t header = 0; header + LLM_INFER_LOGITS_HEADER_BYTES <= metadata_end;
         header += 8) {
        if (result_word(bytes + header) != LLM_INFER_LOGITS_MARKER) continue;
        uint64_t count = result_word(bytes + header + 8);
        uint64_t entry_words = result_word(bytes + header + 16);
        uint64_t table_bytes = result_word(bytes + header + 24);
        if (!count || count > LLM_INFER_MAX_LOGITS_ENTRIES ||
            entry_words != LLM_INFER_LOGITS_ENTRY_WORDS ||
            table_bytes != count * LLM_INFER_LOGITS_ENTRY_BYTES ||
            table_bytes > SIZE_MAX) continue;
        size_t token_header = header + LLM_INFER_LOGITS_HEADER_BYTES;
        if (token_header > metadata_end || table_bytes > metadata_end - token_header)
            continue;
        token_header += (size_t)table_bytes;
        if (token_header > metadata_end - LLM_INFER_TOKEN_TEXT_HEADER_BYTES ||
            result_word(bytes + token_header) != LLM_INFER_TOKEN_TEXT_MARKER ||
            result_word(bytes + token_header + 8) != count ||
            result_word(bytes + token_header + 16) !=
                LLM_INFER_TOKEN_TEXT_ENTRY_WORDS ||
            result_word(bytes + token_header + 24) !=
                count * LLM_INFER_TOKEN_TEXT_ENTRY_BYTES)
            continue;
        size_t token_base = token_header + LLM_INFER_TOKEN_TEXT_HEADER_BYTES;
        size_t token_bytes = (size_t)count * LLM_INFER_TOKEN_TEXT_ENTRY_BYTES;
        if (token_bytes > metadata_end - token_base) continue;
        uint64_t total_piece_bytes = 0;
        bool valid = true;
        for (uint64_t index = 0; index < count && valid; ++index) {
            const unsigned char *entry = bytes + header +
                LLM_INFER_LOGITS_HEADER_BYTES + (size_t)index * LLM_INFER_LOGITS_ENTRY_BYTES;
            const unsigned char *text = bytes + token_base +
                (size_t)index * LLM_INFER_TOKEN_TEXT_ENTRY_BYTES;
            uint64_t sampled = result_word(entry + 32);
            uint64_t runner_up = result_word(entry + 40);
            uint64_t text_checksum = result_word(entry + 64);
            uint64_t candidate_count = result_word(entry + 160);
            uint64_t piece_bytes = result_word(text + 24);
            bool sampled_candidate = false;
            if (result_word(entry + 24) != expected->vocab_size ||
                sampled >= expected->vocab_size || runner_up >= expected->vocab_size ||
                !result_word(entry + 56) || !text_checksum ||
                result_word(entry + 72) != expected->step ||
                result_word(entry + 104) != expected->vocab_size ||
                !result_word(entry + 112) || !candidate_count ||
                candidate_count > LLM_INFER_TERMINAL_CANDIDATES ||
                result_word(text) != expected->step || result_word(text + 8) != sampled ||
                !piece_bytes || piece_bytes > 16 ||
                result_word(text + 48) != text_checksum ||
                total_piece_bytes > UINT64_MAX - piece_bytes) {
                valid = false;
                break;
            }
            total_piece_bytes += piece_bytes;
            for (uint64_t candidate = 0; candidate < candidate_count; ++candidate) {
                const unsigned char *item = entry + 168 +
                    (size_t)candidate * 48;
                uint64_t token = result_word(item);
                uint64_t item_piece_bytes = result_word(item + 24);
                if (token >= expected->vocab_size || !result_word(item + 16) ||
                    !item_piece_bytes || item_piece_bytes > 16) {
                    valid = false;
                    break;
                }
                if (token == sampled) {
                    if (sampled_candidate || result_word(item + 16) != text_checksum ||
                        item_piece_bytes != piece_bytes ||
                        result_word(item + 32) != result_word(text + 32) ||
                        result_word(item + 40) != result_word(text + 40)) {
                        valid = false;
                        break;
                    }
                    sampled_candidate = true;
                }
            }
            if (!sampled_candidate) valid = false;
        }
        if (!valid || result_word(bytes + token_header + 32) != total_piece_bytes)
            continue;
        if (++matches != 1) return -EPROTO;
        const unsigned char *entry = bytes + header + LLM_INFER_LOGITS_HEADER_BYTES;
        const unsigned char *text = bytes + token_base;
        decoded.step = result_word(entry + 72);
        decoded.sampled_token = result_word(entry + 32);
        decoded.runner_up_token = result_word(entry + 40);
        decoded.margin_milli = result_word(entry + 48);
        decoded.logits_checksum = result_word(entry + 56);
        decoded.text_checksum = result_word(entry + 64);
        decoded.top_logit_bits = result_word(entry + 120);
        decoded.runner_up_logit_bits = result_word(entry + 128);
        decoded.full_vocab_checked_token_count = result_word(entry + 104);
        decoded.full_vocab_logits_checksum = result_word(entry + 112);
        decoded.piece_bytes = result_word(text + 24);
        decoded.piece_word0 = result_word(text + 32);
        decoded.piece_word1 = result_word(text + 40);
        decoded.candidate_count = result_word(entry + 160);
        for (uint64_t candidate = 0; candidate < decoded.candidate_count; ++candidate) {
            const unsigned char *item = entry + 168 + (size_t)candidate * 48;
            decoded.candidate_tokens[candidate] = result_word(item);
            decoded.candidate_logit_bits[candidate] = result_word(item + 8);
            decoded.candidate_text_checksums[candidate] = result_word(item + 16);
            decoded.candidate_piece_bytes[candidate] = result_word(item + 24);
            decoded.candidate_piece_word0[candidate] = result_word(item + 32);
            decoded.candidate_piece_word1[candidate] = result_word(item + 40);
        }
    }
    if (matches != 1) return -EPROTO;
    *out = decoded;
    return 0;
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
        expected->layer_end > expected->total_layers || !expected->hidden_bytes ||
        !expected->vocab_size)
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
    decoded.serialized_bytes = header + header_bytes + table_bytes;
    if (expected->layer_end == expected->total_layers) {
        int rc = llm_infer_decode_terminal_result(bytes, (size_t)header,
            expected, &decoded.terminal);
        if (rc) return rc;
        decoded.terminal_present = 1;
    }
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

bool llm_infer_qwen3_range_pipeline_enabled(uint32_t cluster_node_count)
{
    return (cluster_node_count == 2 || cluster_node_count == 3 ||
            cluster_node_count == 4 || cluster_node_count == 8) &&
           cluster_node_count == llm_infer_qwen3_pipeline_nodes() &&
           cluster_node_count <= llm_infer_qwen3_total_layers();
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
