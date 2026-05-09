#ifndef W4_KVCACHE_DB_SERVICE_H
#define W4_KVCACHE_DB_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum w4_db_record_kind {
    W4_DB_RECORD_PREFIX_GROUP = 1,
    W4_DB_RECORD_REQUEST_PREFIX = 2,
    W4_DB_RECORD_BLOCK_META = 3,
    W4_DB_RECORD_WEIGHT_TILE = 4,
    W4_DB_RECORD_KVCACHE_OBJECT = 5,
    W4_DB_RECORD_HIDDEN_RANGE_INPUT = 6,
    W4_DB_RECORD_HIDDEN_RANGE_OUTPUT = 7,
    W4_DB_RECORD_LAYER_RANGE_PLACEMENT = 8,
    W4_DB_RECORD_QWEN3_TOKEN_RESULT = 9,
};

enum w4_kvcache_state {
    W4_KVCACHE_STATE_MISSING = 0,
    W4_KVCACHE_STATE_FILLED = 1,
    W4_KVCACHE_STATE_HOT = 2,
    W4_KVCACHE_STATE_RELOADED = 3,
};

#define W4_DB_MAX_RECORDS 128U
#define W4_DB_MAX_GROUP_MEMBERS 4U

struct w4_db_record {
    bool in_use;
    enum w4_db_record_kind kind;
    char key[96];
    char request_id[64];
    char prefix_group[64];
    char group_id[64];
    char block_hash[96];
    uint32_t placement_node;
    uint32_t placement_level;
    uint64_t hot_segment_id;
    enum w4_kvcache_state state;
    uint64_t version;
    uint64_t last_result_segment;
    uint32_t object_owner_node;
    uint32_t object_payload_kind;
    uint64_t object_backing_offset;
    uint64_t object_backing_len;
    uint64_t object_payload_checksum;
    uint32_t member_count;
    char member_block_hashes[W4_DB_MAX_GROUP_MEMBERS][96];
};

struct w4_db_service {
    bool shmem_ready;
    bool urma_ready;
    bool block_ready;
    size_t record_count;
    struct w4_db_record records[W4_DB_MAX_RECORDS];
};

struct w4_db_cluster_summary {
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

struct w4_db_block_ctx {
    char request_id[64];
    char prefix_group[64];
    char group_id[64];
    char block_hash[96];
    uint32_t placement_node;
    uint32_t placement_level;
    uint64_t hot_segment_id;
    uint64_t result_segment_id;
};

const char *w4_kvcache_state_name(enum w4_kvcache_state state);
void w4_db_build_prefix_key_from_parts(const char *request_id,
                                       const char *prefix_group,
                                       char *out,
                                       size_t out_len);
void w4_db_build_group_key_from_parts(const char *request_id,
                                      const char *group_id,
                                      char *out,
                                      size_t out_len);
void w4_db_build_block_key_from_hash(const char *block_hash, char *out, size_t out_len);
bool w4_db_record_has_member_block(const struct w4_db_record *rec, const char *block_hash);
bool w4_db_prefix_matches_block_meta(const struct w4_db_record *prefix_meta,
                                     const struct w4_db_record *block_meta);
bool w4_db_group_covers_blocks(const struct w4_db_record *group_meta,
                               const struct w4_db_record *primary_block_meta,
                               const struct w4_db_record *aux_block_meta);
int w4_db_service_init(struct w4_db_service *svc,
                       bool shmem_ready,
                       bool urma_ready,
                       bool block_ready);
int w4_db_bootstrap_kvcache(struct w4_db_service *svc,
                            const struct w4_db_block_ctx *ctx,
                            struct w4_db_record *resolved_out);
int w4_db_update_prefix_metadata(struct w4_db_service *svc,
                                 const struct w4_db_block_ctx *ctx,
                                 const struct w4_db_record *block_record,
                                 struct w4_db_record *resolved_out);
int w4_db_get_prefix_group_metadata(struct w4_db_service *svc,
                                    const struct w4_db_block_ctx *ctx,
                                    struct w4_db_record *resolved_out);
int w4_db_apply_block_result(struct w4_db_service *svc,
                             const struct w4_db_block_ctx *ctx,
                             uint64_t result_segment_id,
                             enum w4_kvcache_state next_state,
                             struct w4_db_record *resolved_out);
int w4_db_rebind_block_view(struct w4_db_service *svc,
                            const struct w4_db_block_ctx *ctx,
                            uint64_t hot_segment_id,
                            uint32_t placement_level,
                            struct w4_db_record *resolved_out);
int w4_db_handoff_block_owner(struct w4_db_service *svc,
                              const struct w4_db_block_ctx *ctx,
                              uint32_t placement_node,
                              uint32_t placement_level,
                              uint64_t hot_segment_id,
                              struct w4_db_record *resolved_out);
int w4_db_cluster_fetch_record(struct w4_db_service *svc,
                               const char *key,
                               struct w4_db_record *resolved_out);
int w4_db_publish_observe_cluster(struct w4_db_service *svc,
                                  const struct w4_db_record *local_record,
                                  struct w4_db_cluster_summary *summary);
int w4_db_obmm_service_v0_publish_resolve(struct w4_db_service *svc,
                                          uint32_t local_node,
                                          uint32_t cluster_node_count);
int w4_db_obmm_service_v0_wait_runtime_range_input(uint32_t local_node,
                                                   uint32_t cluster_node_count,
                                                   uint64_t decode_step,
                                                   uint8_t *payload_out,
                                                   uint64_t payload_len,
                                                   uint64_t *checksum_out);
int w4_db_obmm_service_v0_publish_runtime_range_output(struct w4_db_service *svc,
                                                       uint32_t local_node,
                                                       uint32_t cluster_node_count,
                                                       uint64_t decode_step,
                                                       const uint8_t *payload,
                                                       uint64_t payload_len,
                                                       uint64_t expected_checksum,
                                                       const uint8_t *kv_payload,
                                                       uint64_t kv_payload_len,
                                                       uint64_t expected_kv_checksum);
int w4_db_obmm_service_v0_resolve_previous_range_kv_state(struct w4_db_service *svc,
                                                          uint32_t local_node,
                                                          uint32_t cluster_node_count,
                                                          uint64_t decode_step);
int w4_db_obmm_service_v0_publish_terminal_token_result(struct w4_db_service *svc,
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
int w4_db_obmm_service_v0_wait_terminal_token_result(struct w4_db_service *svc,
                                                     uint64_t decode_step,
                                                     uint64_t timeout_ms,
                                                     uint64_t *sampled_token_out);
int w4_db_obmm_service_v0_publish_decode_round_done(struct w4_db_service *svc,
                                                    uint32_t local_node,
                                                    uint32_t cluster_node_count,
                                                    uint64_t decode_step);
int w4_db_obmm_service_v0_wait_all_decode_round_done(struct w4_db_service *svc,
                                                     uint32_t cluster_node_count,
                                                     uint64_t decode_step,
                                                     uint64_t timeout_ms);
int w4_db_qwen3_layer_range_for_node(uint32_t local_node,
                                     uint32_t cluster_node_count,
                                     uint32_t *layer_start_out,
                                     uint32_t *layer_end_out,
                                     uint32_t *next_node_out);
int w4_db_get_record(struct w4_db_service *svc, const char *key, struct w4_db_record *out);

#endif
