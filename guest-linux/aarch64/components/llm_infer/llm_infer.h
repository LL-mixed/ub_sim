#ifndef LLM_INFER_H
#define LLM_INFER_H

#include <stdbool.h>
#include <stdint.h>

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
