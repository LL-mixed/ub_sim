#ifndef MEM_SERVICE_CLIENT_H
#define MEM_SERVICE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem_service_wire.h"
#include "mem_service_wire_client.h"

#define MEM_SERVICE_CLIENT_KEY_LEN 96U
#define MEM_SERVICE_CLIENT_ID_LEN 64U
#define MEM_SERVICE_CLIENT_STATE_LEN 32U
#define MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND "training-step-commit"
#define MEM_SERVICE_CLIENT_PAYLOAD_KIND_SEALED_LOCAL_BLOCK 64U

struct mem_service_client {
    const char *connect_spec;
    struct mem_service_wire_client_options wire_options;
};

struct mem_service_client_record {
    char key[MEM_SERVICE_CLIENT_KEY_LEN];
    uint32_t kind;
    char request_id[MEM_SERVICE_CLIENT_ID_LEN];
    char prefix_group[MEM_SERVICE_CLIENT_ID_LEN];
    char group_id[MEM_SERVICE_CLIENT_ID_LEN];
    char session_id[MEM_SERVICE_CLIENT_ID_LEN];
    char model_key[MEM_SERVICE_CLIENT_ID_LEN];
    char artifact_kind[MEM_SERVICE_CLIENT_ID_LEN];
    char artifact_id[MEM_SERVICE_CLIENT_ID_LEN];
    char block_hash[MEM_SERVICE_CLIENT_KEY_LEN];
    uint32_t placement_node;
    uint32_t placement_level;
    uint64_t hot_segment_id;
    char state[MEM_SERVICE_CLIENT_STATE_LEN];
    uint64_t version;
    uint64_t last_result_segment;
    uint32_t object_owner_node;
    uint32_t object_payload_kind;
    uint64_t object_backing_offset;
    uint64_t object_backing_len;
    uint64_t object_payload_checksum;
};

struct mem_service_client_object {
    const char *key;
    const char *idempotency_key;
    bool has_owner;
    uint32_t owner;
    bool has_payload_kind;
    uint32_t payload_kind;
    bool has_backing_offset;
    uint64_t backing_offset;
    bool has_backing_len;
    uint64_t backing_len;
    bool has_checksum;
    uint64_t checksum;
    bool has_version;
    uint64_t version;
    const char *payload_inline;
};

struct mem_service_client_block_entry {
    const char *request_id;
    const char *prefix_group;
    const char *group_id;
    const char *block_hash;
    const char *idempotency_key;
    bool has_placement_node;
    uint32_t placement_node;
    bool has_placement_level;
    uint32_t placement_level;
    bool has_hot_segment_id;
    uint64_t hot_segment_id;
    const char *state;
    bool has_result_segment_id;
    uint64_t result_segment_id;
};

struct mem_service_client_kv_selector {
    const char *key;
    const char *block_hash;
};

struct mem_service_client_artifact {
    const char *key;
    const char *idempotency_key;
    const char *session_id;
    const char *request_id;
    const char *model_key;
    const char *artifact_kind;
    const char *artifact_id;
    bool has_owner;
    uint32_t owner;
    bool has_payload_kind;
    uint32_t payload_kind;
    bool has_backing_offset;
    uint64_t backing_offset;
    bool has_backing_len;
    uint64_t backing_len;
    bool has_checksum;
    uint64_t checksum;
    bool has_version;
    uint64_t version;
    const char *payload_inline;
};

struct mem_service_client_artifact_query {
    const char *key;
    const char *expected_session_id;
    const char *expected_model_key;
    const char *expected_artifact_kind;
    const char *expected_artifact_id;
    bool has_expected_version;
    uint64_t expected_version;
    bool has_expected_checksum;
    uint64_t expected_checksum;
};

struct mem_service_client_training_ref {
    const char *key;
    const char *idempotency_key;
    const char *session_id;
    const char *request_id;
    const char *model_key;
    const char *artifact_id;
    bool has_owner;
    uint32_t owner;
    bool has_payload_kind;
    uint32_t payload_kind;
    bool has_backing_offset;
    uint64_t backing_offset;
    bool has_backing_len;
    uint64_t backing_len;
    bool has_checksum;
    uint64_t checksum;
    bool has_version;
    uint64_t version;
    const char *payload_inline;
};

struct mem_service_client_training_ref_query {
    const char *key;
    const char *expected_session_id;
    const char *expected_model_key;
    const char *expected_artifact_id;
    bool has_expected_version;
    uint64_t expected_version;
    bool has_expected_checksum;
    uint64_t expected_checksum;
};

void mem_service_client_init(struct mem_service_client *client,
                             const char *connect_spec);
void mem_service_client_init_with_options(
    struct mem_service_client *client,
    const char *connect_spec,
    const struct mem_service_wire_client_options *options);

int mem_service_client_health(const struct mem_service_client *client,
                              enum mem_service_wire_status *status_out);
int mem_service_client_ready(const struct mem_service_client *client,
                             enum mem_service_wire_status *status_out);
int mem_service_client_status(const struct mem_service_client *client,
                              char *payload_out,
                              size_t payload_out_len,
                              enum mem_service_wire_status *status_out);
int mem_service_client_list_records(const struct mem_service_client *client,
                                    char *payload_out,
                                    size_t payload_out_len,
                                    enum mem_service_wire_status *status_out);
int mem_service_client_export_snapshot(const struct mem_service_client *client,
                                       char *payload_out,
                                       size_t payload_out_len,
                                       enum mem_service_wire_status *status_out);
int mem_service_client_export_snapshot_page(const struct mem_service_client *client,
                                            uint64_t start_index,
                                            uint64_t max_records,
                                            char *payload_out,
                                            size_t payload_out_len,
                                            enum mem_service_wire_status *status_out);
int mem_service_client_restore_snapshot(const struct mem_service_client *client,
                                        const char *snapshot_payload,
                                        char *payload_out,
                                        size_t payload_out_len,
                                        enum mem_service_wire_status *status_out);
int mem_service_client_restore_snapshot_page(const struct mem_service_client *client,
                                             const char *page_payload,
                                             char *payload_out,
                                             size_t payload_out_len,
                                             enum mem_service_wire_status *status_out);

int mem_service_client_put_object(const struct mem_service_client *client,
                                  const struct mem_service_client_object *object,
                                  struct mem_service_client_record *record_out,
                                  enum mem_service_wire_status *status_out);
int mem_service_client_get_object(const struct mem_service_client *client,
                                  const char *key,
                                  struct mem_service_client_record *record_out,
                                  enum mem_service_wire_status *status_out);
int mem_service_client_inspect_object(const struct mem_service_client *client,
                                      const char *key,
                                      struct mem_service_client_record *record_out,
                                      enum mem_service_wire_status *status_out);

int mem_service_client_register_prefix_entry(
    const struct mem_service_client *client,
    const struct mem_service_client_block_entry *entry,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_lookup_prefix_entry(
    const struct mem_service_client *client,
    const char *request_id,
    const char *prefix_group,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_kv_segment(
    const struct mem_service_client *client,
    const struct mem_service_client_block_entry *entry,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_kv_segment(
    const struct mem_service_client *client,
    const struct mem_service_client_kv_selector *selector,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_runtime_handoff(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_runtime_handoff(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_register_execution_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_query_execution_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_register_training_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_query_training_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_dataset_shard(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_dataset_shard(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_sample_batch(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_sample_batch(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_checkpoint(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_checkpoint(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_gradient_bucket(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_gradient_bucket(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_optimizer_state(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_optimizer_state(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_commit_training_step(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_training_step(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

#endif
