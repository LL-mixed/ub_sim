#ifndef MEM_SERVICE_H
#define MEM_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lingqu_object_service.h"

enum mem_service_record_kind {
    MEM_SERVICE_RECORD_PREFIX_GROUP = 1,
    MEM_SERVICE_RECORD_REQUEST_PREFIX = 2,
    MEM_SERVICE_RECORD_BLOCK_META = 3,
    MEM_SERVICE_RECORD_WEIGHT_TILE = 4,
    MEM_SERVICE_RECORD_KVCACHE_OBJECT = 5,
    MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT = 6,
    MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT = 7,
    MEM_SERVICE_RECORD_LAYER_RANGE_PLACEMENT = 8,
    MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT = 9,
    MEM_SERVICE_RECORD_QWEN3_ENGRAM_HISTORY = 10,
    MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES = 11,
    MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED = 12,
    MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE = 13,
};

enum mem_service_kvcache_state {
    MEM_SERVICE_KVCACHE_STATE_MISSING = 0,
    MEM_SERVICE_KVCACHE_STATE_FILLED = 1,
    MEM_SERVICE_KVCACHE_STATE_HOT = 2,
    MEM_SERVICE_KVCACHE_STATE_RELOADED = 3,
};

#define MEM_SERVICE_MAX_RECORDS 1024U
#define MEM_SERVICE_MAX_GROUP_MEMBERS 4U
#define MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT 6U
#define MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES 64ULL

struct mem_service_record {
    bool in_use;
    enum mem_service_record_kind kind;
    char key[96];
    char request_id[64];
    char prefix_group[64];
    char group_id[64];
    char block_hash[96];
    uint32_t placement_node;
    uint32_t placement_level;
    uint64_t hot_segment_id;
    enum mem_service_kvcache_state state;
    uint64_t version;
    uint64_t last_result_segment;
    uint32_t object_owner_node;
    uint32_t object_payload_kind;
    uint64_t object_backing_offset;
    uint64_t object_backing_len;
    uint64_t object_payload_checksum;
    uint64_t object_publish_monotonic_ms;
    uint64_t object_publish_supernode_ms;
    int64_t object_publish_supernode_offset_ms;
    uint32_t member_count;
    char member_block_hashes[MEM_SERVICE_MAX_GROUP_MEMBERS][96];
};

struct mem_service {
    bool shmem_ready;
    bool urma_ready;
    bool block_ready;
    size_t record_count;
    struct mem_service_record records[MEM_SERVICE_MAX_RECORDS];
};

struct mem_service_object_payload_view {
    const uint8_t *data;
    uint64_t len;
    uint64_t checksum;
    uint32_t owner_node;
    uint32_t payload_kind;
    uint64_t backing_offset;
    struct lingqu_obmm_object_ref_wire object_ref;
    uint64_t wait_enter_monotonic_ms;
    uint64_t found_monotonic_ms;
    uint64_t ready_monotonic_ms;
    uint64_t producer_publish_supernode_ms;
    uint64_t producer_publish_monotonic_ms;
    int64_t producer_clock_offset_ms;
    int64_t producer_to_found_supernode_ms;
    int64_t producer_to_found_monotonic_ms;
    uint32_t source_node;
    uint32_t wait_attempts;
    uint64_t activate_ms;
    uint64_t metadata_ms;
    uint64_t token_result_words[8];
};

enum mem_service_scheduler_work_item_kind {
    MEM_SERVICE_SCHEDULER_WORK_ITEM_NONE = 0,
    MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD = 1,
    MEM_SERVICE_SCHEDULER_WORK_ITEM_NO_DISPATCH = 2,
};

struct mem_service_scheduler_work_item {
    enum mem_service_scheduler_work_item_kind kind;
    struct mem_service_object_payload_view range_input;
    uint64_t terminal_step;
    uint64_t terminal_token;
    uint32_t terminal_owner_node;
    uint64_t checksum;
    uint64_t wait_enter_monotonic_ms;
    uint64_t found_monotonic_ms;
    uint64_t ready_monotonic_ms;
    uint64_t producer_publish_supernode_ms;
    uint64_t producer_publish_monotonic_ms;
    int64_t producer_clock_offset_ms;
    int64_t producer_to_found_supernode_ms;
    int64_t producer_to_found_monotonic_ms;
    uint32_t wait_attempts;
    uint64_t activate_ms;
    uint64_t metadata_ms;
};

struct mem_service_cluster_summary {
    bool active;
    bool ready;
    bool placement_coherent;
    bool state_coherent;
    bool prefix_state_ready;
    bool prefix_view_ready;
    uint32_t node_count;
    uint32_t peers_observed;
    uint32_t peer_record_count_floor;
    uint32_t peer_prefix_count_floor;
    uint32_t peer_block_count_floor;
    uint32_t peer_group_count_floor;
    uint64_t local_version;
    uint64_t peer_version_floor;
    uint64_t peer_result_floor;
    uint64_t peer_prefix_version_floor;
    uint64_t peer_prefix_result_floor;
};

struct mem_service_block_ctx {
    char request_id[64];
    char prefix_group[64];
    char group_id[64];
    char block_hash[96];
    uint32_t placement_node;
    uint32_t placement_level;
    uint64_t hot_segment_id;
    uint64_t result_segment_id;
};

const char *mem_service_kvcache_state_name(enum mem_service_kvcache_state state);
void mem_service_build_prefix_key_from_parts(const char *request_id,
                                       const char *prefix_group,
                                       char *out,
                                       size_t out_len);
void mem_service_build_group_key_from_parts(const char *request_id,
                                      const char *group_id,
                                      char *out,
                                      size_t out_len);
void mem_service_build_block_key_from_hash(const char *block_hash, char *out, size_t out_len);
bool mem_service_record_has_member_block(const struct mem_service_record *rec, const char *block_hash);
bool mem_service_prefix_matches_block_meta(const struct mem_service_record *prefix_meta,
                                     const struct mem_service_record *block_meta);
bool mem_service_group_covers_blocks(const struct mem_service_record *group_meta,
                               const struct mem_service_record *primary_block_meta,
                               const struct mem_service_record *aux_block_meta);
int mem_service_init(struct mem_service *svc,
                       bool shmem_ready,
                       bool urma_ready,
                       bool block_ready);
int mem_service_bootstrap_kvcache(struct mem_service *svc,
                            const struct mem_service_block_ctx *ctx,
                            struct mem_service_record *resolved_out);
int mem_service_update_prefix_metadata(struct mem_service *svc,
                                 const struct mem_service_block_ctx *ctx,
                                 const struct mem_service_record *block_record,
                                 struct mem_service_record *resolved_out);
int mem_service_get_prefix_group_metadata(struct mem_service *svc,
                                    const struct mem_service_block_ctx *ctx,
                                    struct mem_service_record *resolved_out);
int mem_service_apply_block_result(struct mem_service *svc,
                             const struct mem_service_block_ctx *ctx,
                             uint64_t result_segment_id,
                             enum mem_service_kvcache_state next_state,
                             struct mem_service_record *resolved_out);
int mem_service_rebind_block_view(struct mem_service *svc,
                            const struct mem_service_block_ctx *ctx,
                            uint64_t hot_segment_id,
                            uint32_t placement_level,
                            struct mem_service_record *resolved_out);
int mem_service_handoff_block_owner(struct mem_service *svc,
                              const struct mem_service_block_ctx *ctx,
                              uint32_t placement_node,
                              uint32_t placement_level,
                              uint64_t hot_segment_id,
                              struct mem_service_record *resolved_out);
int mem_service_cluster_fetch_record(struct mem_service *svc,
                               const char *key,
                               struct mem_service_record *resolved_out);
int mem_service_publish_observe_cluster(struct mem_service *svc,
                                  const struct mem_service_record *local_record,
                                  struct mem_service_cluster_summary *summary);
int mem_service_obmm_service_v0_publish_resolve(struct mem_service *svc,
                                          uint32_t local_node,
                                          uint32_t cluster_node_count);
int mem_service_obmm_service_v0_ensure_cluster_runtime(uint32_t local_node,
                                                 uint32_t cluster_node_count);
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
int mem_service_qwen3_layer_range_for_node(uint32_t local_node,
                                     uint32_t cluster_node_count,
                                     uint32_t *layer_start_out,
                                     uint32_t *layer_end_out,
                                     uint32_t *next_node_out);
int mem_service_get_record(struct mem_service *svc, const char *key, struct mem_service_record *out);
int mem_service_record_to_lingqu_obmm_ref(const struct mem_service_record *record,
                                    struct lingqu_obmm_object_ref_wire *ref_out);

#endif
