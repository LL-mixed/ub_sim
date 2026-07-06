#ifndef MEM_SERVICE_WIRE_SCHEMA_H
#define MEM_SERVICE_WIRE_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem_service_wire.h"
#include "mem_service_wire_payload.h"

#define MEM_SERVICE_WIRE_SCHEMA_VERSION 1U
#define MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV 1U

struct mem_service_wire_payload_oneof {
    const char *const *field_names;
    size_t field_count;
};

struct mem_service_wire_operation_schema {
    enum mem_service_wire_operation operation;
    const char *name;
    uint32_t schema_version;
    uint32_t payload_format;
    const struct mem_service_wire_payload_field *fields;
    size_t field_count;
    const struct mem_service_wire_payload_oneof *oneofs;
    size_t oneof_count;
};

static const struct mem_service_wire_payload_field
    mem_service_wire_object_put_fields[] = {
        {"key", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
        {"owner", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
        {"payload_kind", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
        {"backing_offset", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"backing_len", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"checksum", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"version", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"backend", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"backend_kind", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
        {"backend_node", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
        {"backend_device_cna", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
        {"backend_block_hi", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"backend_block_lo", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"backend_block_version", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"backend_block_offset", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"backend_block_bytes", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"backend_block_checksum", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"idempotency_key", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"payload_inline", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"payload_path", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
};

static const struct mem_service_wire_payload_field
    mem_service_wire_object_get_fields[] = {
        {"key", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
};

static const struct mem_service_wire_payload_field
    mem_service_wire_snapshot_page_fields[] = {
        {"start_index", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"max_records", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
};

static const struct mem_service_wire_payload_field
    mem_service_wire_audit_log_fields[] = {
        {"start_sequence", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"max_events", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
};

static const struct mem_service_wire_payload_field
    mem_service_wire_restore_snapshot_page_fields[] = {
        {"action", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
        {"page_index", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"expected_records", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"complete", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
};

static const struct mem_service_wire_payload_field
    mem_service_wire_block_context_fields[] = {
        {"request_id", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
        {"prefix_group", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
        {"group_id", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
        {"block_hash", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
        {"placement_node", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
        {"placement_level", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
        {"hot_segment_id", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"state", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"result_segment_id", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"idempotency_key", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
};

static const struct mem_service_wire_payload_field
    mem_service_wire_prefix_lookup_fields[] = {
        {"request_id", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
        {"prefix_group", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
};

static const struct mem_service_wire_payload_field
    mem_service_wire_kv_resolve_fields[] = {
        {"key", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"block_hash", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
};

static const char *const mem_service_wire_kv_resolve_selector_fields[] = {
    "key",
    "block_hash",
};

static const struct mem_service_wire_payload_oneof
    mem_service_wire_kv_resolve_oneofs[] = {
        {mem_service_wire_kv_resolve_selector_fields,
         sizeof(mem_service_wire_kv_resolve_selector_fields) /
             sizeof(mem_service_wire_kv_resolve_selector_fields[0])},
};

static const struct mem_service_wire_payload_field
    mem_service_wire_artifact_publish_fields[] = {
        {"key", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
        {"session_id", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"request_id", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"model_key", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"artifact_kind", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"artifact_id", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"owner", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
        {"payload_kind", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
        {"backing_offset", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"backing_len", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"checksum", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"version", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"idempotency_key", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"payload_inline", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"payload_path", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
};

static const struct mem_service_wire_payload_field
    mem_service_wire_artifact_query_fields[] = {
        {"key", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, true},
        {"expected_session_id", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"expected_model_key", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"expected_artifact_kind", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"expected_artifact_id", MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING, false},
        {"expected_owner", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32, false},
        {"expected_version", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
        {"expected_checksum", MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64, false},
};

static const struct mem_service_wire_operation_schema
    mem_service_wire_operation_schemas[] = {
        {MEM_SERVICE_WIRE_OP_HEALTH,
         "health",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         NULL,
         0,
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_READY,
         "ready",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         NULL,
         0,
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_STATUS,
         "status",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         NULL,
         0,
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_LIST_RECORDS,
         "list_records",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         NULL,
         0,
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_METRICS,
         "metrics",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         NULL,
         0,
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT,
         "export_snapshot",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         NULL,
         0,
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
         "export_snapshot_page",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_snapshot_page_fields,
         sizeof(mem_service_wire_snapshot_page_fields) /
             sizeof(mem_service_wire_snapshot_page_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
         "restore_snapshot",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         NULL,
         0,
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
         "restore_snapshot_page",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_restore_snapshot_page_fields,
         sizeof(mem_service_wire_restore_snapshot_page_fields) /
             sizeof(mem_service_wire_restore_snapshot_page_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_AUDIT_LOG,
         "audit_log",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_audit_log_fields,
         sizeof(mem_service_wire_audit_log_fields) /
             sizeof(mem_service_wire_audit_log_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_PUT_OBJECT,
         "put_object",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_object_put_fields,
         sizeof(mem_service_wire_object_put_fields) /
             sizeof(mem_service_wire_object_put_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_GET_OBJECT,
         "get_object",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_object_get_fields,
         sizeof(mem_service_wire_object_get_fields) /
             sizeof(mem_service_wire_object_get_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_INSPECT_OBJECT,
         "inspect_object",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_object_get_fields,
         sizeof(mem_service_wire_object_get_fields) /
             sizeof(mem_service_wire_object_get_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY,
         "register_prefix_entry",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_block_context_fields,
         sizeof(mem_service_wire_block_context_fields) /
             sizeof(mem_service_wire_block_context_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY,
         "lookup_prefix_entry",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_prefix_lookup_fields,
         sizeof(mem_service_wire_prefix_lookup_fields) /
             sizeof(mem_service_wire_prefix_lookup_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT,
         "publish_kv_segment",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_block_context_fields,
         sizeof(mem_service_wire_block_context_fields) /
             sizeof(mem_service_wire_block_context_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT,
         "resolve_kv_segment",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_kv_resolve_fields,
         sizeof(mem_service_wire_kv_resolve_fields) /
             sizeof(mem_service_wire_kv_resolve_fields[0]),
         mem_service_wire_kv_resolve_oneofs,
         sizeof(mem_service_wire_kv_resolve_oneofs) /
             sizeof(mem_service_wire_kv_resolve_oneofs[0])},
        {MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
         "publish_runtime_handoff",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_artifact_publish_fields,
         sizeof(mem_service_wire_artifact_publish_fields) /
             sizeof(mem_service_wire_artifact_publish_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
         "resolve_runtime_handoff",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_artifact_query_fields,
         sizeof(mem_service_wire_artifact_query_fields) /
             sizeof(mem_service_wire_artifact_query_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT,
         "register_execution_artifact",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_artifact_publish_fields,
         sizeof(mem_service_wire_artifact_publish_fields) /
             sizeof(mem_service_wire_artifact_publish_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT,
         "query_execution_artifact",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_artifact_query_fields,
         sizeof(mem_service_wire_artifact_query_fields) /
             sizeof(mem_service_wire_artifact_query_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
         "register_training_artifact",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_artifact_publish_fields,
         sizeof(mem_service_wire_artifact_publish_fields) /
             sizeof(mem_service_wire_artifact_publish_fields[0]),
         NULL,
         0},
        {MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
         "query_training_artifact",
         MEM_SERVICE_WIRE_SCHEMA_VERSION,
         MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV,
         mem_service_wire_artifact_query_fields,
         sizeof(mem_service_wire_artifact_query_fields) /
             sizeof(mem_service_wire_artifact_query_fields[0]),
         NULL,
         0},
};

static inline const struct mem_service_wire_operation_schema *
mem_service_wire_schema_for_operation(enum mem_service_wire_operation operation)
{
    size_t i;

    for (i = 0; i < sizeof(mem_service_wire_operation_schemas) /
                    sizeof(mem_service_wire_operation_schemas[0]);
         ++i) {
        if (mem_service_wire_operation_schemas[i].operation == operation) {
            return &mem_service_wire_operation_schemas[i];
        }
    }
    return NULL;
}

static inline bool mem_service_wire_schema_validate_payload(
    const struct mem_service_wire_operation_schema *schema,
    const struct mem_service_wire_payload_view *view,
    const char **failed_field_out)
{
    size_t failed_index = 0;
    size_t i;

    if (failed_field_out != NULL) {
        *failed_field_out = NULL;
    }
    if (schema == NULL) {
        return true;
    }
    if (!mem_service_wire_payload_validate_schema(view,
                                                  schema->fields,
                                                  schema->field_count,
                                                  &failed_index)) {
        if (failed_field_out != NULL && schema->fields != NULL) {
            *failed_field_out = schema->fields[failed_index].name;
        }
        return false;
    }
    for (i = 0; i < schema->oneof_count; ++i) {
        const struct mem_service_wire_payload_oneof *oneof = &schema->oneofs[i];
        size_t field_index;
        bool present = false;

        for (field_index = 0; field_index < oneof->field_count; ++field_index) {
            char value[128];

            if (mem_service_wire_payload_get_string(view,
                                                    oneof->field_names[field_index],
                                                    value,
                                                    sizeof(value))) {
                present = true;
                break;
            }
        }
        if (!present) {
            if (failed_field_out != NULL && oneof->field_count > 0) {
                *failed_field_out = oneof->field_names[0];
            }
            return false;
        }
    }
    return true;
}

#endif
