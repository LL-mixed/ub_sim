#ifndef LLM_INFER_H
#define LLM_INFER_H

#include <stdbool.h>
#include <stdint.h>

uint64_t llm_infer_qwen3_pipeline_nodes(void);
uint64_t llm_infer_qwen3_total_layers(void);
uint64_t llm_infer_qwen3_vocab_size(void);
const char *llm_infer_qwen3_model_id(void);
uint64_t llm_infer_qwen3_hidden_range_bytes(void);
uint64_t llm_infer_qwen3_decode_hidden_bytes(void);
uint64_t llm_infer_qwen3_handoff_hidden_bytes(uint64_t decode_step);
bool llm_infer_is_qwen3_profile_name(const char *profile);
bool llm_infer_qwen3_real_tokenizer_required(void);

#endif
