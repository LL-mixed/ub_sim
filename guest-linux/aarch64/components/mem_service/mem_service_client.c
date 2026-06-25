#include "mem_service_client.h"

#include <string.h>

#include "mem_service_wire_client.h"
#include "mem_service_wire_payload.h"

static void mem_service_client_set_status(enum mem_service_wire_status *status_out,
                                          enum mem_service_wire_status status)
{
    if (status_out != NULL) {
        *status_out = status;
    }
}

static const char *mem_service_client_connect_spec(
    const struct mem_service_client *client)
{
    if (client == NULL || client->connect_spec == NULL ||
        client->connect_spec[0] == '\0') {
        return NULL;
    }
    return client->connect_spec;
}

static bool mem_service_client_has_value(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static int mem_service_client_invalid(enum mem_service_wire_status *status_out)
{
    mem_service_client_set_status(status_out, MEM_SERVICE_WIRE_STATUS_INVALID_SESSION);
    return 2;
}

static int mem_service_client_append_required_string(char *payload,
                                                     size_t payload_len,
                                                     const char *name,
                                                     const char *value)
{
    if (!mem_service_client_has_value(value)) {
        return -1;
    }
    return mem_service_wire_payload_append_field(payload, payload_len, name, value);
}

static int mem_service_client_append_optional_string(char *payload,
                                                     size_t payload_len,
                                                     const char *name,
                                                     const char *value)
{
    if (!mem_service_client_has_value(value)) {
        return 0;
    }
    return mem_service_wire_payload_append_field(payload, payload_len, name, value);
}

static int mem_service_client_append_optional_u32(char *payload,
                                                  size_t payload_len,
                                                  const char *name,
                                                  bool present,
                                                  uint32_t value)
{
    if (!present) {
        return 0;
    }
    return mem_service_wire_payload_append_u64(payload,
                                               payload_len,
                                               name,
                                               (uint64_t)value);
}

static int mem_service_client_append_optional_u64(char *payload,
                                                  size_t payload_len,
                                                  const char *name,
                                                  bool present,
                                                  uint64_t value)
{
    if (!present) {
        return 0;
    }
    return mem_service_wire_payload_append_u64(payload, payload_len, name, value);
}

static void mem_service_client_payload_copy(
    const struct mem_service_wire_payload_view *view,
    const char *name,
    char *out,
    size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (!mem_service_wire_payload_get_string(view, name, out, out_len)) {
        out[0] = '\0';
    }
}

static int mem_service_client_parse_record(const char *payload,
                                           struct mem_service_client_record *record_out)
{
    struct mem_service_wire_payload_view view;

    if (record_out == NULL) {
        return 0;
    }
    if (payload == NULL || payload[0] == '\0') {
        return -1;
    }
    memset(record_out, 0, sizeof(*record_out));
    view = mem_service_wire_payload_view_from_cstr(payload);
    mem_service_client_payload_copy(&view,
                                    "key",
                                    record_out->key,
                                    sizeof(record_out->key));
    mem_service_client_payload_copy(&view,
                                    "request_id",
                                    record_out->request_id,
                                    sizeof(record_out->request_id));
    mem_service_client_payload_copy(&view,
                                    "prefix_group",
                                    record_out->prefix_group,
                                    sizeof(record_out->prefix_group));
    mem_service_client_payload_copy(&view,
                                    "group_id",
                                    record_out->group_id,
                                    sizeof(record_out->group_id));
    mem_service_client_payload_copy(&view,
                                    "session_id",
                                    record_out->session_id,
                                    sizeof(record_out->session_id));
    mem_service_client_payload_copy(&view,
                                    "model_key",
                                    record_out->model_key,
                                    sizeof(record_out->model_key));
    mem_service_client_payload_copy(&view,
                                    "artifact_kind",
                                    record_out->artifact_kind,
                                    sizeof(record_out->artifact_kind));
    mem_service_client_payload_copy(&view,
                                    "artifact_id",
                                    record_out->artifact_id,
                                    sizeof(record_out->artifact_id));
    mem_service_client_payload_copy(&view,
                                    "block_hash",
                                    record_out->block_hash,
                                    sizeof(record_out->block_hash));
    mem_service_client_payload_copy(&view,
                                    "state",
                                    record_out->state,
                                    sizeof(record_out->state));
    record_out->kind = mem_service_wire_payload_get_u32(&view, "kind", 0);
    record_out->placement_node =
        mem_service_wire_payload_get_u32(&view, "placement_node", 0);
    record_out->placement_level =
        mem_service_wire_payload_get_u32(&view, "placement_level", 0);
    record_out->hot_segment_id =
        mem_service_wire_payload_get_u64(&view, "hot_segment_id", 0);
    record_out->version = mem_service_wire_payload_get_u64(&view, "version", 0);
    record_out->last_result_segment =
        mem_service_wire_payload_get_u64(&view, "last_result_segment", 0);
    record_out->object_owner_node =
        mem_service_wire_payload_get_u32(&view, "object_owner_node", 0);
    record_out->object_payload_kind =
        mem_service_wire_payload_get_u32(&view, "object_payload_kind", 0);
    record_out->object_backing_offset =
        mem_service_wire_payload_get_u64(&view, "object_backing_offset", 0);
    record_out->object_backing_len =
        mem_service_wire_payload_get_u64(&view, "object_backing_len", 0);
    record_out->object_payload_checksum =
        mem_service_wire_payload_get_u64(&view, "object_payload_checksum", 0);
    if (record_out->object_payload_checksum == 0) {
        record_out->object_payload_checksum =
            mem_service_wire_payload_get_u64(&view, "checksum", 0);
    }
    return record_out->key[0] == '\0' ? -1 : 0;
}

static int mem_service_client_send(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const char *payload,
    char *response,
    size_t response_len,
    enum mem_service_wire_status *status_out)
{
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    const struct mem_service_wire_client_options *options =
        client != NULL ? &client->wire_options : NULL;
    int rc = mem_service_send_unix_request_with_options(
        mem_service_client_connect_spec(client),
        options,
        operation,
        payload,
        response,
        response_len,
        &status);

    mem_service_client_set_status(status_out, status);
    return rc;
}

static int mem_service_client_send_record(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const char *payload,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;

    memset(response, 0, sizeof(response));
    rc = mem_service_client_send(client,
                                 operation,
                                 payload,
                                 response,
                                 sizeof(response),
                                 &status);
    if (status_out != NULL) {
        *status_out = status;
    }
    if (rc != 0) {
        return rc;
    }
    if (mem_service_client_parse_record(response, record_out) != 0) {
        mem_service_client_set_status(status_out, MEM_SERVICE_WIRE_STATUS_INTERNAL);
        return 1;
    }
    return 0;
}

static int mem_service_client_append_object_payload(
    char *payload,
    size_t payload_len,
    const struct mem_service_client_object *object)
{
    if (object == NULL ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "key",
                                                  object->key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "idempotency_key",
                                                  object->idempotency_key) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "owner",
                                               object->has_owner,
                                               object->owner) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "payload_kind",
                                               object->has_payload_kind,
                                               object->payload_kind) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backing_offset",
                                               object->has_backing_offset,
                                               object->backing_offset) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backing_len",
                                               object->has_backing_len,
                                               object->backing_len) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "checksum",
                                               object->has_checksum,
                                               object->checksum) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "version",
                                               object->has_version,
                                               object->version) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_client_append_block_payload(
    char *payload,
    size_t payload_len,
    const struct mem_service_client_block_entry *entry,
    bool require_result)
{
    if (entry == NULL ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "request_id",
                                                  entry->request_id) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "prefix_group",
                                                  entry->prefix_group) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "group_id",
                                                  entry->group_id) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "block_hash",
                                                  entry->block_hash) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "idempotency_key",
                                                  entry->idempotency_key) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "placement_node",
                                               entry->has_placement_node,
                                               entry->placement_node) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "placement_level",
                                               entry->has_placement_level,
                                               entry->placement_level) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "hot_segment_id",
                                               entry->has_hot_segment_id,
                                               entry->hot_segment_id) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "state",
                                                  entry->state) != 0) {
        return -1;
    }
    if (require_result &&
        (!entry->has_result_segment_id || entry->result_segment_id == 0)) {
        return -1;
    }
    return mem_service_client_append_optional_u64(payload,
                                                  payload_len,
                                                  "result_segment_id",
                                                  entry->has_result_segment_id,
                                                  entry->result_segment_id);
}

static int mem_service_client_append_artifact_payload(
    char *payload,
    size_t payload_len,
    const struct mem_service_client_artifact *artifact)
{
    if (artifact == NULL ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "key",
                                                  artifact->key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "idempotency_key",
                                                  artifact->idempotency_key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "session_id",
                                                  artifact->session_id) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "request_id",
                                                  artifact->request_id) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "model_key",
                                                  artifact->model_key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "artifact_kind",
                                                  artifact->artifact_kind) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "artifact_id",
                                                  artifact->artifact_id) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "owner",
                                               artifact->has_owner,
                                               artifact->owner) != 0 ||
        mem_service_client_append_optional_u32(payload,
                                               payload_len,
                                               "payload_kind",
                                               artifact->has_payload_kind,
                                               artifact->payload_kind) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backing_offset",
                                               artifact->has_backing_offset,
                                               artifact->backing_offset) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "backing_len",
                                               artifact->has_backing_len,
                                               artifact->backing_len) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "checksum",
                                               artifact->has_checksum,
                                               artifact->checksum) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "version",
                                               artifact->has_version,
                                               artifact->version) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_client_append_artifact_query_payload(
    char *payload,
    size_t payload_len,
    const struct mem_service_client_artifact_query *query)
{
    if (query == NULL ||
        mem_service_client_append_required_string(payload,
                                                  payload_len,
                                                  "key",
                                                  query->key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "expected_session_id",
                                                  query->expected_session_id) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "expected_model_key",
                                                  query->expected_model_key) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "expected_artifact_kind",
                                                  query->expected_artifact_kind) != 0 ||
        mem_service_client_append_optional_string(payload,
                                                  payload_len,
                                                  "expected_artifact_id",
                                                  query->expected_artifact_id) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "expected_version",
                                               query->has_expected_version,
                                               query->expected_version) != 0 ||
        mem_service_client_append_optional_u64(payload,
                                               payload_len,
                                               "expected_checksum",
                                               query->has_expected_checksum,
                                               query->expected_checksum) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_client_publish_artifact(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[768] = "";

    if (mem_service_client_append_artifact_payload(payload,
                                                   sizeof(payload),
                                                   artifact) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          operation,
                                          payload,
                                          record_out,
                                          status_out);
}

static int mem_service_client_query_artifact(
    const struct mem_service_client *client,
    enum mem_service_wire_operation operation,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[512] = "";

    if (mem_service_client_append_artifact_query_payload(payload,
                                                         sizeof(payload),
                                                         query) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          operation,
                                          payload,
                                          record_out,
                                          status_out);
}

static int mem_service_client_publish_training_ref(
    const struct mem_service_client *client,
    const char *artifact_kind,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    struct mem_service_client_artifact artifact;

    if (ref == NULL || !mem_service_client_has_value(artifact_kind)) {
        return mem_service_client_invalid(status_out);
    }
    memset(&artifact, 0, sizeof(artifact));
    artifact.key = ref->key;
    artifact.idempotency_key = ref->idempotency_key;
    artifact.session_id = ref->session_id;
    artifact.request_id = ref->request_id;
    artifact.model_key = ref->model_key;
    artifact.artifact_kind = artifact_kind;
    artifact.artifact_id = ref->artifact_id;
    artifact.has_owner = ref->has_owner;
    artifact.owner = ref->owner;
    artifact.has_payload_kind = ref->has_payload_kind;
    artifact.payload_kind = ref->payload_kind;
    artifact.has_backing_offset = ref->has_backing_offset;
    artifact.backing_offset = ref->backing_offset;
    artifact.has_backing_len = ref->has_backing_len;
    artifact.backing_len = ref->backing_len;
    artifact.has_checksum = ref->has_checksum;
    artifact.checksum = ref->checksum;
    artifact.has_version = ref->has_version;
    artifact.version = ref->version;
    return mem_service_client_publish_artifact(
        client,
        MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
        &artifact,
        record_out,
        status_out);
}

static int mem_service_client_resolve_training_ref(
    const struct mem_service_client *client,
    const char *artifact_kind,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    struct mem_service_client_artifact_query artifact_query;

    if (query == NULL || !mem_service_client_has_value(artifact_kind)) {
        return mem_service_client_invalid(status_out);
    }
    memset(&artifact_query, 0, sizeof(artifact_query));
    artifact_query.key = query->key;
    artifact_query.expected_session_id = query->expected_session_id;
    artifact_query.expected_model_key = query->expected_model_key;
    artifact_query.expected_artifact_kind = artifact_kind;
    artifact_query.expected_artifact_id = query->expected_artifact_id;
    artifact_query.has_expected_version = query->has_expected_version;
    artifact_query.expected_version = query->expected_version;
    artifact_query.has_expected_checksum = query->has_expected_checksum;
    artifact_query.expected_checksum = query->expected_checksum;
    return mem_service_client_query_artifact(
        client,
        MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
        &artifact_query,
        record_out,
        status_out);
}

void mem_service_client_init(struct mem_service_client *client,
                             const char *connect_spec)
{
    mem_service_client_init_with_options(client, connect_spec, NULL);
}

void mem_service_client_init_with_options(
    struct mem_service_client *client,
    const char *connect_spec,
    const struct mem_service_wire_client_options *options)
{
    if (client != NULL) {
        memset(client, 0, sizeof(*client));
        client->connect_spec = connect_spec;
        mem_service_wire_client_options_init(&client->wire_options);
        if (options != NULL) {
            client->wire_options = *options;
        }
    }
}

int mem_service_client_health(const struct mem_service_client *client,
                              enum mem_service_wire_status *status_out)
{
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_HEALTH,
                                   NULL,
                                   NULL,
                                   0,
                                   status_out);
}

int mem_service_client_ready(const struct mem_service_client *client,
                             enum mem_service_wire_status *status_out)
{
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_READY,
                                   NULL,
                                   NULL,
                                   0,
                                   status_out);
}

int mem_service_client_status(const struct mem_service_client *client,
                              char *payload_out,
                              size_t payload_out_len,
                              enum mem_service_wire_status *status_out)
{
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_STATUS,
                                   NULL,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_list_records(const struct mem_service_client *client,
                                    char *payload_out,
                                    size_t payload_out_len,
                                    enum mem_service_wire_status *status_out)
{
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_LIST_RECORDS,
                                   NULL,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_export_snapshot(const struct mem_service_client *client,
                                       char *payload_out,
                                       size_t payload_out_len,
                                       enum mem_service_wire_status *status_out)
{
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT,
                                   NULL,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_export_snapshot_page(const struct mem_service_client *client,
                                            uint64_t start_index,
                                            uint64_t max_records,
                                            char *payload_out,
                                            size_t payload_out_len,
                                            enum mem_service_wire_status *status_out)
{
    char payload[160] = "";

    if (mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "start_index",
                                            start_index) != 0 ||
        (max_records != 0 &&
         mem_service_wire_payload_append_u64(payload,
                                             sizeof(payload),
                                             "max_records",
                                             max_records) != 0)) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
                                   payload,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_restore_snapshot(const struct mem_service_client *client,
                                        const char *snapshot_payload,
                                        char *payload_out,
                                        size_t payload_out_len,
                                        enum mem_service_wire_status *status_out)
{
    if (!mem_service_client_has_value(snapshot_payload)) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                   snapshot_payload,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_restore_snapshot_page(const struct mem_service_client *client,
                                             const char *page_payload,
                                             char *payload_out,
                                             size_t payload_out_len,
                                             enum mem_service_wire_status *status_out)
{
    if (!mem_service_client_has_value(page_payload)) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send(client,
                                   MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                   page_payload,
                                   payload_out,
                                   payload_out_len,
                                   status_out);
}

int mem_service_client_put_object(const struct mem_service_client *client,
                                  const struct mem_service_client_object *object,
                                  struct mem_service_client_record *record_out,
                                  enum mem_service_wire_status *status_out)
{
    char payload[512] = "";

    if (mem_service_client_append_object_payload(payload, sizeof(payload), object) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_get_object(const struct mem_service_client *client,
                                  const char *key,
                                  struct mem_service_client_record *record_out,
                                  enum mem_service_wire_status *status_out)
{
    char payload[160] = "";

    if (mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "key",
                                                  key) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_GET_OBJECT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_inspect_object(const struct mem_service_client *client,
                                      const char *key,
                                      struct mem_service_client_record *record_out,
                                      enum mem_service_wire_status *status_out)
{
    char payload[160] = "";

    if (mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "key",
                                                  key) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_INSPECT_OBJECT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_register_prefix_entry(
    const struct mem_service_client *client,
    const struct mem_service_client_block_entry *entry,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[768] = "";

    if (mem_service_client_append_block_payload(payload,
                                                sizeof(payload),
                                                entry,
                                                true) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_lookup_prefix_entry(
    const struct mem_service_client *client,
    const char *request_id,
    const char *prefix_group,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[256] = "";

    if (mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "request_id",
                                                  request_id) != 0 ||
        mem_service_client_append_required_string(payload,
                                                  sizeof(payload),
                                                  "prefix_group",
                                                  prefix_group) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_publish_kv_segment(
    const struct mem_service_client *client,
    const struct mem_service_client_block_entry *entry,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[768] = "";

    if (mem_service_client_append_block_payload(payload,
                                                sizeof(payload),
                                                entry,
                                                false) != 0) {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_resolve_kv_segment(
    const struct mem_service_client *client,
    const struct mem_service_client_kv_selector *selector,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    char payload[192] = "";

    if (selector == NULL ||
        (mem_service_client_append_optional_string(payload,
                                                   sizeof(payload),
                                                   "key",
                                                   selector->key) != 0 ||
         mem_service_client_append_optional_string(payload,
                                                   sizeof(payload),
                                                   "block_hash",
                                                   selector->block_hash) != 0) ||
        payload[0] == '\0') {
        return mem_service_client_invalid(status_out);
    }
    return mem_service_client_send_record(client,
                                          MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT,
                                          payload,
                                          record_out,
                                          status_out);
}

int mem_service_client_publish_runtime_handoff(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_artifact(
        client,
        MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
        artifact,
        record_out,
        status_out);
}

int mem_service_client_resolve_runtime_handoff(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_query_artifact(
        client,
        MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
        query,
        record_out,
        status_out);
}

int mem_service_client_register_execution_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_artifact(
        client,
        MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT,
        artifact,
        record_out,
        status_out);
}

int mem_service_client_query_execution_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_query_artifact(
        client,
        MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT,
        query,
        record_out,
        status_out);
}

int mem_service_client_register_training_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_artifact(
        client,
        MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
        artifact,
        record_out,
        status_out);
}

int mem_service_client_query_training_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_query_artifact(
        client,
        MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
        query,
        record_out,
        status_out);
}

int mem_service_client_publish_dataset_shard(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(client,
                                                   "dataset-shard",
                                                   ref,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_resolve_dataset_shard(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(client,
                                                   "dataset-shard",
                                                   query,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_publish_sample_batch(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(client,
                                                   "sample-batch",
                                                   ref,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_resolve_sample_batch(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(client,
                                                   "sample-batch",
                                                   query,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_publish_checkpoint(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(client,
                                                   "checkpoint",
                                                   ref,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_resolve_checkpoint(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(client,
                                                   "checkpoint",
                                                   query,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_publish_gradient_bucket(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(client,
                                                   "gradient-bucket",
                                                   ref,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_resolve_gradient_bucket(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(client,
                                                   "gradient-bucket",
                                                   query,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_publish_optimizer_state(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_publish_training_ref(client,
                                                   "optimizer-state",
                                                   ref,
                                                   record_out,
                                                   status_out);
}

int mem_service_client_resolve_optimizer_state(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out)
{
    return mem_service_client_resolve_training_ref(client,
                                                   "optimizer-state",
                                                   query,
                                                   record_out,
                                                   status_out);
}
