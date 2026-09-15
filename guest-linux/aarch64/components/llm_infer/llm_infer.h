#ifndef LLM_INFER_H
#define LLM_INFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct llm_infer_range_identity {
    uint64_t node, layer_start, layer_end, next_node;
    uint64_t pipeline_nodes, total_layers, hidden_bytes;
};

struct llm_infer_result_view {
    uint64_t offset, bytes, model_checksum, service_checksum;
};

struct llm_infer_range_result {
    struct llm_infer_result_view hidden, kv;
    uint64_t input_checksum, output_tensor_checksum, range_checksum, real_layers;
};

/* Decode an already completed and CPU-visible result under the caller's SDK
 * lease. Offsets are relative to the allocation, not this CPU view. Never
 * copies payloads, maps memory, publishes references or grants access. An
 * error clears out. The serialized metadata layout is shared by both models.
 * The caller verifies whole-result integrity. Numerical layer/tensor checksum
 * metadata may differ from the serialized hidden payload's model_checksum. */
int llm_infer_decode_range_result(const void *result, size_t result_bytes,
    uint64_t allocation_offset, const struct llm_infer_range_identity *expected,
    struct llm_infer_range_result *out);

uint64_t llm_infer_qwen3_pipeline_nodes(void);
uint64_t llm_infer_qwen3_total_layers(void);
uint64_t llm_infer_qwen3_vocab_size(void);
const char *llm_infer_qwen3_model_id(void);
const char *llm_infer_qwen3_model_key(void);
uint64_t llm_infer_qwen3_hidden_range_bytes(void);
uint64_t llm_infer_qwen3_decode_hidden_bytes(void);
uint64_t llm_infer_qwen3_handoff_hidden_bytes(uint64_t decode_step);
uint64_t llm_infer_qwen3_kv_heads(void);
uint64_t llm_infer_qwen3_head_dim(void);
uint64_t llm_infer_qwen3_kv_streams(void);
uint64_t llm_infer_qwen3_kv_elem_bytes(void);
uint64_t llm_infer_qwen3_range_kv_state_bytes(uint32_t layer_start,
                                              uint32_t layer_end);
int llm_infer_qwen3_layer_range_for_node(uint32_t local_node,
                                         uint32_t cluster_node_count,
                                         uint32_t *layer_start_out,
                                         uint32_t *layer_end_out,
                                         uint32_t *next_node_out);
bool llm_infer_is_qwen3_profile_name(const char *profile);
bool llm_infer_qwen3_real_tokenizer_required(void);

#endif
