#ifndef LLM_INFER_H
#define LLM_INFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Canonical producer-assigned allocation identity for managed serving.
 * Requires 65 bytes including NUL; failures leave the output untouched.
 * run/request must be nonzero. This names an object, without granting access. */
int llm_infer_managed_object_key(char *out, size_t size, uint64_t run,
    uint64_t request, uint32_t producer, uint64_t step);

struct llm_infer_range_identity {
    uint64_t node, layer_start, layer_end, next_node;
    uint64_t pipeline_nodes, total_layers, hidden_bytes;
    uint64_t step, vocab_size;
};

struct llm_infer_result_view {
    uint64_t offset, bytes, model_checksum, service_checksum;
};

#define LLM_INFER_TERMINAL_CANDIDATES 4u

struct llm_infer_terminal_result {
    uint64_t step, sampled_token, runner_up_token, margin_milli;
    uint64_t logits_checksum, text_checksum;
    uint64_t top_logit_bits, runner_up_logit_bits;
    uint64_t full_vocab_checked_token_count, full_vocab_logits_checksum;
    uint64_t piece_bytes, piece_word0, piece_word1;
    uint64_t candidate_count;
    uint64_t candidate_tokens[LLM_INFER_TERMINAL_CANDIDATES];
    uint64_t candidate_logit_bits[LLM_INFER_TERMINAL_CANDIDATES];
    uint64_t candidate_text_checksums[LLM_INFER_TERMINAL_CANDIDATES];
    uint64_t candidate_piece_bytes[LLM_INFER_TERMINAL_CANDIDATES];
    uint64_t candidate_piece_word0[LLM_INFER_TERMINAL_CANDIDATES];
    uint64_t candidate_piece_word1[LLM_INFER_TERMINAL_CANDIDATES];
};

struct llm_infer_range_result {
    struct llm_infer_result_view hidden, kv, terminal_view, token_view;
    uint64_t input_checksum, output_tensor_checksum, range_checksum, real_layers;
    /* Exact bytes written by the serialized result, excluding spare capacity. */
    uint64_t serialized_bytes;
    /* Memory Service checksum over exactly serialized_bytes. */
    uint64_t serialized_checksum;
    uint64_t terminal_present;
    struct llm_infer_terminal_result terminal;
};

/* Decode an already completed and CPU-visible result under the caller's SDK
 * lease. Offsets are relative to the allocation, not this CPU view. Never
 * copies payloads, maps memory, publishes references or grants access. An
 * error clears out. The serialized metadata layout is shared by both models.
 * The caller verifies whole-result integrity. Numerical layer/tensor checksum
 * metadata may differ from the serialized hidden payload's model_checksum.
 * A terminal range must contain exactly one structurally valid logits/token
 * table for expected.step and expected.vocab_size. terminal_view covers the
 * exact contiguous logits and token-text tables for V2 publication. token_view
 * covers the first validated 64-byte sampled-token record used by the next
 * decode step; non-terminal ranges leave both views and terminal_present clear. */
int llm_infer_decode_range_result(const void *result, size_t result_bytes,
    uint64_t allocation_offset, const struct llm_infer_range_identity *expected,
    struct llm_infer_range_result *out);

uint64_t llm_infer_qwen3_pipeline_nodes(void);
bool llm_infer_qwen3_range_pipeline_enabled(uint32_t cluster_node_count);
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
