#ifndef MEM_SERVICE_QWEN3_H
#define MEM_SERVICE_QWEN3_H

#include <stdint.h>

#include "mem_service.h"

#define MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT 6U
#define MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES 64ULL

uint32_t mem_service_qwen3_layer_count(void);
uint32_t mem_service_qwen3_range_nodes(void);
uint64_t mem_service_qwen3_hidden_range_bytes(void);
uint64_t mem_service_qwen3_handoff_hidden_bytes(uint64_t decode_step);
const char *mem_service_qwen3_model_key(void);
uint64_t mem_service_qwen3_range_kv_state_bytes(uint32_t layer_start,
                                                uint32_t layer_end,
                                                uint64_t token_count);
int mem_service_qwen3_layer_range_for_node(uint32_t local_node,
                                           uint32_t cluster_node_count,
                                           uint32_t *layer_start_out,
                                           uint32_t *layer_end_out,
                                           uint32_t *next_node_out);
void mem_service_qwen3_node_range(uint32_t node,
                                  uint32_t node_count,
                                  uint32_t *start_out,
                                  uint32_t *end_out);

int mem_service_obmm_service_v0_wait_runtime_range_input(uint32_t local_node,
                                                         uint32_t cluster_node_count,
                                                         uint64_t decode_step,
                                                         uint8_t *payload_out,
                                                         uint64_t payload_len,
                                                         uint64_t *checksum_out);
int mem_service_obmm_service_v0_wait_runtime_range_input_view(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_object_payload_view *view_out);
int mem_service_obmm_service_v0_wait_scheduler_work_item(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_scheduler_work_item *item_out);
int mem_service_obmm_service_v0_publish_runtime_range_output(struct mem_service *svc,
                                                             uint32_t local_node,
                                                             uint32_t cluster_node_count,
                                                             uint64_t decode_step,
                                                             const uint8_t *payload,
                                                             uint64_t payload_len,
                                                             uint64_t expected_checksum,
                                                             const uint8_t *kv_payload,
                                                             uint64_t kv_payload_len,
                                                             uint64_t expected_kv_checksum);
int mem_service_obmm_service_v0_publish_runtime_range_kv_state(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    const uint8_t *kv_payload,
    uint64_t kv_payload_len,
    uint64_t expected_kv_checksum);
int mem_service_obmm_service_v0_resolve_previous_range_kv_state_view(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_object_payload_view *view_out);
int mem_service_obmm_service_v0_try_resolve_range_kv_state_view(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t kv_step,
    struct mem_service_object_payload_view *view_out);
int mem_service_obmm_service_v0_resolve_previous_range_kv_state(struct mem_service *svc,
                                                                uint32_t local_node,
                                                                uint32_t cluster_node_count,
                                                                uint64_t decode_step,
                                                                uint8_t *payload_out,
                                                                uint64_t payload_capacity,
                                                                uint64_t *payload_len_out,
                                                                uint64_t *checksum_out);
int mem_service_obmm_service_v0_publish_terminal_token_result(struct mem_service *svc,
                                                              uint32_t local_node,
                                                              uint32_t cluster_node_count,
                                                              uint64_t decode_step,
                                                              uint64_t sampled_token,
                                                              uint64_t runner_up_token,
                                                              uint64_t margin_milli,
                                                              uint64_t logits_checksum,
                                                              uint64_t text_checksum,
                                                              uint64_t piece_word0,
                                                              uint64_t piece_word1);
int mem_service_obmm_service_v0_publish_shortpath_terminal_token_result(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    uint64_t sampled_token,
    uint64_t runner_up_token,
    uint64_t margin_milli,
    uint64_t logits_checksum,
    uint64_t text_checksum,
    uint64_t piece_word0,
    uint64_t piece_word1);
int mem_service_obmm_service_v0_publish_engram_step(struct mem_service *svc,
                                                    uint32_t local_node,
                                                    uint32_t cluster_node_count,
                                                    uint64_t decode_step,
                                                    const uint64_t *history_tokens,
                                                    uint64_t history_token_count,
                                                    uint64_t raw_sampled_token,
                                                    uint64_t runner_up_token,
                                                    uint64_t selected_token,
                                                    uint64_t blocked_count,
                                                    uint64_t fallback_used,
                                                    int64_t top_score_milli,
                                                    int64_t runner_up_score_milli,
                                                    uint64_t no_repeat_ngram_size,
                                                    uint64_t repetition_penalty_milli,
                                                    uint64_t history_window,
                                                    uint64_t logits_checksum,
                                                    uint64_t text_checksum);
int mem_service_obmm_service_v0_publish_engram_candidates(struct mem_service *svc,
                                                          uint32_t local_node,
                                                          uint32_t cluster_node_count,
                                                          uint64_t decode_step,
                                                          const uint64_t *candidate_tokens,
                                                          const uint64_t *candidate_logit_bits,
                                                          const uint64_t *candidate_text_checksums,
                                                          const uint64_t *candidate_piece_bytes,
                                                          const uint64_t *candidate_piece_word0,
                                                          const uint64_t *candidate_piece_word1,
                                                          uint64_t candidate_count);
int mem_service_obmm_service_v0_wait_engram_candidates(struct mem_service *svc,
                                                       uint64_t decode_step,
                                                       uint64_t timeout_ms,
                                                       uint64_t *candidate_tokens_out,
                                                       uint64_t *candidate_logit_bits_out,
                                                       uint64_t *candidate_text_checksums_out,
                                                       uint64_t *candidate_piece_bytes_out,
                                                       uint64_t *candidate_piece_word0_out,
                                                       uint64_t *candidate_piece_word1_out,
                                                       uint64_t candidate_capacity,
                                                       uint64_t *candidate_count_out,
                                                       uint64_t *candidate_checksum_out);
int mem_service_obmm_service_v0_wait_terminal_token_result(struct mem_service *svc,
                                                           uint64_t decode_step,
                                                           uint64_t timeout_ms,
                                                           uint64_t *sampled_token_out);
int mem_service_obmm_service_v0_wait_engram_selected_token(struct mem_service *svc,
                                                           uint64_t decode_step,
                                                           uint64_t timeout_ms,
                                                           uint64_t *selected_token_out);
int mem_service_obmm_service_v0_wait_engram_history(struct mem_service *svc,
                                                    uint64_t decode_step,
                                                    uint64_t timeout_ms,
                                                    uint64_t *history_tokens_out,
                                                    uint64_t history_token_capacity,
                                                    uint64_t *history_token_count_out,
                                                    uint64_t *history_checksum_out);
int mem_service_obmm_service_v0_wait_engram_state(struct mem_service *svc,
                                                  uint64_t decode_step,
                                                  uint64_t timeout_ms,
                                                  uint64_t expected_history_token_count,
                                                  uint64_t expected_selected_token,
                                                  uint64_t expected_history_checksum,
                                                  uint64_t no_repeat_ngram_size,
                                                  uint64_t repetition_penalty_milli,
                                                  uint64_t *state_checksum_out);
int mem_service_obmm_service_v0_publish_decode_round_done(struct mem_service *svc,
                                                          uint32_t local_node,
                                                          uint32_t cluster_node_count,
                                                          uint64_t decode_step,
                                                          uint64_t round_scope_hash);
int mem_service_obmm_service_v0_wait_all_decode_round_done(struct mem_service *svc,
                                                           uint32_t cluster_node_count,
                                                           uint64_t decode_step,
                                                           uint64_t round_scope_hash,
                                                           uint64_t timeout_ms);

#endif
