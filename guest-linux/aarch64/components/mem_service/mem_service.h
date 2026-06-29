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
    MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT = 9,
    MEM_SERVICE_RECORD_MODEL_ENGRAM_HISTORY = 10,
    MEM_SERVICE_RECORD_MODEL_ENGRAM_CANDIDATES = 11,
    MEM_SERVICE_RECORD_MODEL_ENGRAM_SELECTED = 12,
    MEM_SERVICE_RECORD_MODEL_ENGRAM_STATE = 13,
    MEM_SERVICE_RECORD_RUNTIME_HANDOFF = 14,
    MEM_SERVICE_RECORD_EXECUTION_ARTIFACT = 15,
    MEM_SERVICE_RECORD_TRAINING_ARTIFACT = 16,
};

enum mem_service_kvcache_state {
    MEM_SERVICE_KVCACHE_STATE_MISSING = 0,
    MEM_SERVICE_KVCACHE_STATE_FILLED = 1,
    MEM_SERVICE_KVCACHE_STATE_HOT = 2,
    MEM_SERVICE_KVCACHE_STATE_RELOADED = 3,
};

#define MEM_SERVICE_MAX_RECORDS 1024U
#define MEM_SERVICE_MAX_GROUP_MEMBERS 4U
#define MEM_SERVICE_METRIC_LATENCY_BUCKET_COUNT 6U
#define MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS 64U
#define MEM_SERVICE_IDEMPOTENCY_KEY_LEN 96U
#define MEM_SERVICE_IDEMPOTENCY_RESPONSE_LEN 4096U
#define MEM_SERVICE_MAX_AUDIT_EVENTS 256U
#define MEM_SERVICE_PAYLOAD_KIND_SEALED_LOCAL_BLOCK 64U
#define MEM_SERVICE_PAYLOAD_KIND_SEALED_CHUNKED_BLOCK 65U
#define MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_LOOPBACK_BLOCK 66U
#define MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK 67U

struct mem_service_record {
    bool in_use;
    enum mem_service_record_kind kind;
    char key[96];
    char request_id[64];
    char prefix_group[64];
    char group_id[64];
    char block_hash[96];
    char session_id[64];
    char model_key[64];
    char artifact_kind[64];
    char artifact_id[96];
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

struct mem_service_metrics {
    uint64_t request_count;
    uint64_t ok_count;
    uint64_t error_count;
    uint64_t not_found_count;
    uint64_t stale_ref_count;
    uint64_t checksum_mismatch_count;
    uint64_t version_conflict_count;
    uint64_t invalid_model_binding_count;
    uint64_t invalid_session_count;
    uint64_t timeout_count;
    uint64_t capacity_exceeded_count;
    uint64_t unsupported_count;
    uint64_t internal_count;
    uint64_t fail_closed_count;
    uint64_t health_count;
    uint64_t ready_count;
    uint64_t status_count;
    uint64_t list_records_count;
    uint64_t metrics_count;
    uint64_t audit_log_count;
    uint64_t export_snapshot_count;
    uint64_t export_snapshot_page_count;
    uint64_t restore_snapshot_count;
    uint64_t restore_snapshot_page_count;
    uint64_t put_object_count;
    uint64_t get_object_count;
    uint64_t inspect_object_count;
    uint64_t get_object_hit_count;
    uint64_t get_object_miss_count;
    uint64_t register_prefix_count;
    uint64_t lookup_prefix_count;
    uint64_t prefix_lookup_hit_count;
    uint64_t prefix_lookup_miss_count;
    uint64_t publish_kv_count;
    uint64_t resolve_kv_count;
    uint64_t kv_resolve_hit_count;
    uint64_t kv_resolve_miss_count;
    uint64_t publish_runtime_handoff_count;
    uint64_t resolve_runtime_handoff_count;
    uint64_t register_execution_artifact_count;
    uint64_t query_execution_artifact_count;
    uint64_t register_training_artifact_count;
    uint64_t query_training_artifact_count;
    uint64_t artifact_query_hit_count;
    uint64_t artifact_query_miss_count;
    uint64_t idempotency_replay_count;
    uint64_t idempotency_conflict_count;
    uint64_t request_latency_total_ms;
    uint64_t request_latency_max_ms;
    uint64_t request_latency_bucket_counts[MEM_SERVICE_METRIC_LATENCY_BUCKET_COUNT];
};

struct mem_service_idempotency_record {
    bool in_use;
    char key[MEM_SERVICE_IDEMPOTENCY_KEY_LEN];
    uint32_t operation;
    uint32_t request_checksum;
    uint32_t status;
    uint32_t response_len;
    char response[MEM_SERVICE_IDEMPOTENCY_RESPONSE_LEN];
};

struct mem_service_audit_event {
    bool in_use;
    uint64_t sequence;
    uint64_t monotonic_ms;
    uint32_t operation;
    uint32_t status;
    uint32_t request_checksum;
    uint32_t response_checksum;
    uint32_t idempotency_replay;
    char key[96];
    char session_id[64];
    char model_key[64];
    char artifact_kind[64];
    char artifact_id[96];
    char idempotency_key[MEM_SERVICE_IDEMPOTENCY_KEY_LEN];
    uint64_t version;
    uint64_t checksum;
};

struct mem_service {
    bool shmem_ready;
    bool urma_ready;
    bool block_ready;
    bool enforce_expected_context;
    size_t record_count;
    uint64_t audit_next_sequence;
    uint64_t audit_event_count;
    struct mem_service_metrics metrics;
    struct mem_service_record records[MEM_SERVICE_MAX_RECORDS];
    struct mem_service_idempotency_record
        idempotency_records[MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS];
    struct mem_service_audit_event audit_events[MEM_SERVICE_MAX_AUDIT_EVENTS];
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
int mem_service_get_record(struct mem_service *svc, const char *key, struct mem_service_record *out);
int mem_service_record_to_lingqu_obmm_ref(const struct mem_service_record *record,
                                    struct lingqu_obmm_object_ref_wire *ref_out);

#endif
